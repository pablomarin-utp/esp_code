#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>

const char* ssid = "MARIN";
const char* password = "10028370";

const char* hostname = "esp32-bridge";
const char* backend_url = "https://sternum-untaxed-vigorous.ngrok-free.dev/api/v1/esp";

#define LED_PIN 2
#define TRIG_PIN 5
#define ECHO_PIN 18
#define DETECTION_DISTANCE_CM 45

WebServer server(80);
String cam_ip = "";
bool ledState = false;
bool motionDetected = false;
float lastDistanceCm = -1;

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleCamIp() {
  sendCorsHeaders();
  
  if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    int ipIndex = body.indexOf("\"ip\":");
    if (ipIndex >= 0) {
      int start = body.indexOf("\"", ipIndex + 5) + 1;
      int end = body.indexOf("\"", start);
      if (start > 0 && end > start) {
        cam_ip = body.substring(start, end);
        Serial.println("IP de CAM recibida: " + cam_ip);
      }
    }
    server.send(200, "application/json", "{\"status\": \"ok\"}");
  } else {
    String json = "{\"ip\": \"" + cam_ip + "\"}";
    server.send(200, "application/json", json);
  }
}

void handleOptions() {
  sendCorsHeaders();
  server.send(200, "text/plain", "");
}

void registerToBackend() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  http.begin(String(backend_url) + "/register");
  http.addHeader("Content-Type", "application/json");
  
  String payload = "{\"device_type\": \"bridge\", \"ip\": \"" + WiFi.localIP().toString() + "\"}";
  int httpCode = http.POST(payload);
  
  if (httpCode == 200) {
    String response = http.getString();
    Serial.println("Registro al backend: " + response);
  } else {
    Serial.print("Error registro bridge: ");
    Serial.println(httpCode);
  }
  http.end();
}

void handleLedToggle() {
  sendCorsHeaders();
  
  if (cam_ip != "") {
    HTTPClient http;
    String cam_url = "http://" + cam_ip + "/api/led";
    http.begin(cam_url);
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST("");
    if (httpCode == 200) {
      String response = http.getString();
      server.send(200, "application/json", response);
    } else {
      server.send(500, "application/json", "{\"error\": \"Failed to toggle LED on CAM\"}");
    }
    http.end();
  } else {
    server.send(500, "application/json", "{\"error\": \"CAM IP not known\"}");
  }
}

void handleLedStatus() {
  sendCorsHeaders();
  
  if (cam_ip != "") {
    HTTPClient http;
    String cam_url = "http://" + cam_ip + "/api/led";
    http.begin(cam_url);
    
    int httpCode = http.GET();
    if (httpCode == 200) {
      String response = http.getString();
      server.send(200, "application/json", response);
    } else {
      server.send(500, "application/json", "{\"error\": \"Failed to get LED status from CAM\"}");
    }
    http.end();
  } else {
    server.send(500, "application/json", "{\"error\": \"CAM IP not known\"}");
  }
}

void handleMotion() {
  sendCorsHeaders();
  String json = "{\"motion\": " + String(motionDetected ? "true" : "false") +
                ", \"distance\": " + String(lastDistanceCm, 1) + "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = "<html><head><title>ESP32 Bridge</title></head><body>";
  html += "<h1>ESP32 Bridge</h1>";
  html += "<p>ESP32-CAM IP: " + cam_ip + "</p>";
  html += "<p><a href='/cam_ip'>Ver IP en JSON</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi conectado!");
  Serial.print("IP ESP32: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(hostname)) {
    Serial.println("mDNS iniciado: http://esp32-bridge.local");
    MDNS.addService("http", "tcp", 80);
  }

  server.on("/", handleRoot);
  server.on("/api/cam_ip", handleCamIp);
  server.on("/api/cam_ip", HTTP_OPTIONS, handleOptions);
  server.on("/api/led", HTTP_POST, handleLedToggle);
  server.on("/api/led", HTTP_OPTIONS, handleOptions);
  server.on("/api/led", HTTP_GET, handleLedStatus);
  server.on("/api/motion", HTTP_GET, handleMotion);
  server.on("/api/motion", HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("Servidor ESP32 iniciado!");
  
  delay(1000);
  registerToBackend();
}

void loop() {
  server.handleClient();

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 23500); // timeout = ~400cm máximo
  lastDistanceCm = (duration > 0) ? (duration * 0.034) / 2.0 : -1;

  motionDetected = (lastDistanceCm > 0 && lastDistanceCm <= DETECTION_DISTANCE_CM);

  static unsigned long lastReport = 0;
  unsigned long now = millis();
  if (now - lastReport >= 500) {  // Reportar cada 500ms al navegador
    Serial.print("duration raw: ");
    Serial.print(duration);
    Serial.print(" us | Distancia: ");
    if (lastDistanceCm >= 0) {
      Serial.print(lastDistanceCm, 1);
      Serial.println(" cm");
    } else {
      Serial.println("sin lectura");
    }
    lastReport = now;
  }

  delay(60); // el HC-SR04 necesita mínimo 60ms entre mediciones

  static unsigned long lastBackendReport = 0;
  if (millis() - lastBackendReport > 60000) {
    lastBackendReport = millis();
    registerToBackend();
  }
}