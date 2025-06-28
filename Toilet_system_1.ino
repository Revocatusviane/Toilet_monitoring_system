#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>

const char* ssid = "RevoAiTechs";
const char* password = "12345678";
const char* serverIP = "http://172.20.10.5:5000";

#define TOILET_ID 1 // Change to 2 or 3 for other toilets

IPAddress TOILET_IP(172, 20, 10, TOILET_ID == 1 ? 4 : TOILET_ID == 2 ? 6 : 134);
IPAddress gateway(172, 20, 10, 1);
IPAddress subnet(255, 255, 255, 0);

WebServer server(5000);

// Pin definitions
const int status_led_green = 2;    // Green LED
const int status_led_red = 4;      // Red LED
const int door_servo = 5;          // Servo motor
const int flush_pump_relay = 18;   // Flush relay (changed to pin 14)
const int gas_fume_sensor = 34;    // Gas sensor (analog)

// Servo object
Servo doorServo;

// State machine
enum ActionState { IDLE, FLUSHING, DESTROYED, RELAY_ON, RED_BLINK };
ActionState currentState = IDLE;
unsigned long actionStartTime = 0;

// Red LED blink variables
unsigned long lastBlinkToggle = 0;
const unsigned long blinkInterval = 250; // 250ms ON/OFF for blinking
bool blinkState = false;

// Log buffer
#define LOG_SIZE 20
String logMessages[LOG_SIZE];
int logIndex = 0;

// Gas sensor variables
unsigned long lastGasCheck = 0;
const unsigned long gasCheckInterval = 100; // Check every 100ms

void addLog(String message) {
  logMessages[logIndex] = String(millis()) + ": " + message;
  logIndex = (logIndex + 1) % LOG_SIZE;
  Serial.println("[ESP32] " + message);
}

void setup() {
  Serial.begin(115200);
  pinMode(status_led_green, OUTPUT);
  pinMode(status_led_red, OUTPUT);
  pinMode(flush_pump_relay, OUTPUT);
  pinMode(gas_fume_sensor, INPUT);
  digitalWrite(status_led_green, LOW);
  digitalWrite(status_led_red, LOW);
  
  digitalWrite(flush_pump_relay, HIGH); // Relay OFF (LOW = OFF)
  delay(1000);
  digitalWrite(flush_pump_relay, LOW); // Relay OFF (LOW = OFF)
  

  // Initialize servo
  doorServo.attach(door_servo);
  doorServo.write(90); // Door open
  addLog("Servo initialized to open position");

  connectWiFi();

  // Server routes
  server.on("/control", HTTP_GET, handleControl);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/servo/unlock", HTTP_GET, []() {
    doorServo.write(90);
    server.send(200, "text/plain", "Servo unlocked");
    addLog("Servo unlocked");
  });
  server.on("/servo/lock", HTTP_GET, []() {
    doorServo.write(0);
    server.send(200, "text/plain", "Servo locked");
    addLog("Servo locked");
  });
  server.on("/pump/on", HTTP_GET, []() {
    digitalWrite(flush_pump_relay, HIGH); // Pump ON (HIGH = ON)
    server.send(200, "text/plain", "Pump ON");
    addLog("Pump ON");
  });
  server.on("/pump/off", HTTP_GET, []() {
    digitalWrite(flush_pump_relay, LOW); // Pump OFF (LOW = OFF)
    server.send(200, "text/plain", "Pump OFF");
    addLog("Pump OFF");
  });
  server.on("/led/red/on", HTTP_GET, []() {
    digitalWrite(status_led_red, HIGH);
    server.send(200, "text/plain", "Red LED ON");
    addLog("Red LED ON");
  });
  server.on("/led/red/off", HTTP_GET, []() {
    digitalWrite(status_led_red, LOW);
    server.send(200, "text/plain", "Red LED OFF");
    addLog("Red LED OFF");
  });
  server.on("/led/green/on", HTTP_GET, []() {
    digitalWrite(status_led_green, HIGH);
    server.send(200, "text/plain", "Green LED ON");
    addLog("Green LED ON");
  });
  server.on("/led/green/off", HTTP_GET, []() {
    digitalWrite(status_led_green, LOW);
    server.send(200, "text/plain", "Green LED OFF");
    addLog("Green LED OFF");
  });
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not Found");
    addLog("404: Invalid endpoint");
  });
  server.begin();
  addLog("Server started at http://" + WiFi.localIP().toString() + ":5000");
}

void connectWiFi() {
  WiFi.config(TOILET_IP, gateway, subnet);
  WiFi.begin(ssid, password);
  addLog("Connecting to WiFi: " + String(ssid));
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    addLog("WiFi connected: " + WiFi.localIP().toString());
  } else {
    addLog("WiFi connection failed");
    ESP.restart();
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("WiFi lost, reconnecting...");
    connectWiFi();
  }

  server.handleClient();

  // Continuous gas sensor check
  if (millis() - lastGasCheck >= gasCheckInterval) {
    int gasValue = analogRead(gas_fume_sensor);
    if (gasValue > 5 && currentState != RED_BLINK) {
      currentState = RED_BLINK;
      actionStartTime = millis();
      lastBlinkToggle = millis();
      blinkState = true;
      digitalWrite(status_led_red, HIGH);
      addLog("Gas > 5 detected: " + String(gasValue) + ", starting red LED blink");
    }
    lastGasCheck = millis();
  }

  // State machine
  switch (currentState) {
    case FLUSHING:
      if (millis() - actionStartTime >= 1000) {
        digitalWrite(flush_pump_relay, LOW); // Relay OFF (LOW = OFF)
        currentState = IDLE;
        addLog("Flushing completed");
      }
      break;
    case DESTROYED:
      if (millis() - actionStartTime >= 2000) {
        doorServo.write(0); // Servo to 0
        currentState = IDLE;
        addLog("Destroyed state completed, servo closed");
      }
      break;
    case RELAY_ON:
      if (millis() - actionStartTime >= 2000) {
        digitalWrite(flush_pump_relay, HIGH); // Relay OFF (LOW = OFF)
        currentState = IDLE;
        addLog("Relay turned OFF after 2 seconds");
      }
      break;
    case RED_BLINK:
      if (millis() - actionStartTime >= 2000) {
        digitalWrite(status_led_red, LOW); // Ensure LED OFF
        currentState = IDLE;
        addLog("Red LED blinking completed");
      } else if (millis() - lastBlinkToggle >= blinkInterval) {
        blinkState = !blinkState;
        digitalWrite(status_led_red, blinkState ? HIGH : LOW);
        lastBlinkToggle = millis();
        addLog("Red LED " + String(blinkState ? "ON" : "OFF") + " (blink)");
      }
      break;
    case IDLE:
      break;
  }
}

String checkFumes() {
  int fumes = analogRead(gas_fume_sensor);
  String status = fumes > 1000 ? "BAD_FUMES" : "OK";
  addLog("Fumes checked: " + status + " (value: " + String(fumes) + ")");
  return status;
}

bool notifyServer(String endpoint) {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("Cannot send HTTP POST: WiFi disconnected");
    return false;
  }

  HTTPClient http;
  String url = String(serverIP) + endpoint;
  http.begin(url);
  addLog("Sending HTTP POST to " + url);
  int attempts = 0;
  int httpCode = 0;
  while (attempts < 3) {
    httpCode = http.POST("");
    if (httpCode > 0) {
      addLog("HTTP POST response: " + String(httpCode) + ", " + http.getString());
      http.end();
      return true;
    }
    addLog("HTTP POST attempt " + String(attempts + 1) + " failed");
    attempts++;
    delay(500);
  }
  addLog("HTTP POST failed after 3 attempts");
  http.end();
  return false;
}

void handleStatus() {
  String html = "<html><head><title>ESP32 Toilet Status</title>";
  html += "<meta http-equiv='refresh' content='1'>";
  html += "<style>body{font-family:Arial;margin:20px;padding:20px;background:#f4f4f4;}"
          "h1{color:#333;} .log{background:white;padding:15px;border-radius:5px;"
          "box-shadow:0 0 10px rgba(0,0,0,0.1);}</style></head>";
  html += "<body><h1>ESP32 Toilet " + String(TOILET_ID) + " Status</h1>";
  html += "<div class='log'><h2>Recent Logs</h2><ul>";
  for (int i = 0; i < LOG_SIZE; i++) {
    int idx = (logIndex - i - 1 + LOG_SIZE) % LOG_SIZE;
    if (logMessages[idx] != "") {
      html += "<li>" + logMessages[idx] + "</li>";
    }
  }
  html += "</ul></div></body></html>";
  server.send(200, "text/plain", html);
  addLog("Serving status page");
}

void handleControl() {
  if (!server.hasArg("action")) {
    server.send(400, "text/plain", "ERROR,Missing action");
    addLog("Invalid request: Missing action");
    return;
  }

  String action = server.arg("action");
  String fumes = checkFumes();
  String response = "OK";

  addLog("Received action: " + action);

  if (action == "ping") {
    server.send(200, "text/plain", response + (fumes == "BAD_FUMES" ? ",BAD_FUMES" : ""));
    addLog("Ping responded");
  }
  else if (action == "start") {
    server.send(200, "text/plain", response);
    addLog("System started");
  }
  else if (action == "scan_user") {
    digitalWrite(status_led_green, HIGH);
    delay(100);
    digitalWrite(status_led_green, LOW);
    server.send(200, "text/plain", "User scanned");
    addLog("User image requested");
    notifyServer("/capture/toilet" + String(TOILET_ID) + "/user");
  }
  else if (action == "check_before") {
    digitalWrite(status_led_green, HIGH);
    delay(100);
    digitalWrite(status_led_green, LOW);
    server.send(200, "text/plain", "Pre-use check started");
    addLog("Pre-use image requested");
    notifyServer("/capture/toilet" + String(TOILET_ID) + "/pre_use");
  }
  else if (action == "select_toilet") {
    doorServo.write(90); // Door open
    server.send(200, "text/plain", "Toilet selected");
    addLog("Toilet selected, servo opened");
  }
  else if (action.startsWith("post_use=")) {
    if (currentState != IDLE) {
      server.send(429, "text/plain", "ERROR,Action in progress");
      addLog("Post_use rejected: Action in progress");
      
      return;
    }

    String status = action.substring(9); // Extract condition
    doorServo.write(0); // Close servo
    digitalWrite(flush_pump_relay, HIGH); // Relay ON (HIGH = ON)
    actionStartTime = millis();
    currentState = RELAY_ON; // Use RELAY_ON for 2s
    addLog("Post_use started: " + status + ", relay ON");

    if (status == "DESTROYED TOILET") {
      digitalWrite(status_led_red, HIGH);
      digitalWrite(status_led_green, LOW);
      response = "DESTROYED";
      currentState = DESTROYED;
      addLog("Status: DESTROYED");
    } else {
      digitalWrite(status_led_green, HIGH);
      digitalWrite(status_led_red, LOW);
      response = "UNDESTROYED";
      addLog("Status: UNDESTROYED");
    }
    server.send(200, "text/plain", response + (fumes == "BAD_FUMES" ? ",BAD_FUMES" : ""));
    notifyServer("/capture/toilet" + String(TOILET_ID) + "/post_use");
  }
  else if (action == "done") {
    server.send(200, "text/plain", "Done received");
    addLog("Done action received");
    notifyServer("/done/toilet" + String(TOILET_ID));
    
  digitalWrite(flush_pump_relay, HIGH); // Relay OFF (LOW = OFF)
  delay(2000);
  digitalWrite(flush_pump_relay, LOW); // Relay OFF (LOW = OFF)
  
  }
  else if (action.startsWith("red_led_")) {
    bool state = action.endsWith("on");
    digitalWrite(status_led_red, state ? HIGH : LOW);
    server.send(200, "text/plain", "Red LED " + String(state ? "ON" : "OFF"));
    addLog("Red LED " + String(state ? "ON" : "OFF"));
  }
  else if (action.startsWith("green_led_")) {
    bool state = action.endsWith("on");
    digitalWrite(status_led_green, state ? HIGH : LOW);
    server.send(200, "text/plain", "Green LED " + String(state ? "ON" : "OFF"));
    addLog("Green LED " + String(state ? "ON" : "OFF"));
  }
  else {
    server.send(400, "text/plain", "ERROR,Invalid action: " + action);
    addLog("Invalid action: " + action);
  }
}
