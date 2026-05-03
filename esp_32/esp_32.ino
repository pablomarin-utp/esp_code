#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>

const char* ssid     = "MARIN";
const char* password = "10028370";

const char* hostname    = "esp32-bridge";
const char* backend_url = "https://sternum-untaxed-vigorous.ngrok-free.dev/api/v1/esp";

#define LED_BOARD  2   // LED integrado en la placa
#define LED_EXT    26  // LED externo (activo en HIGH)
#define FAN_PIN    27  // IN1 del YW Robot → ventilador
#define TRIG_PIN   5
#define ECHO_PIN   18
#define DETECTION_DISTANCE_CM 45

WebServer server(80);
bool bulbOn         = false;
bool fanOn          = false;
bool motionDetected = false;
float lastDistanceCm = -1;

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ── Relay ─────────────────────────────────────────────────────────────────────

void setBulb(bool on) {
  bulbOn = on;
  digitalWrite(LED_EXT,   on ? HIGH : LOW);
  digitalWrite(LED_BOARD, on ? HIGH : LOW);
  Serial.println(on ? "LED ENCENDIDO" : "LED APAGADO");
}

// ── Ventilador ────────────────────────────────────────────────────────────────

void setFan(bool on) {
  fanOn = on;
  digitalWrite(FAN_PIN, on ? LOW : HIGH);  // módulo relé activo en LOW
  Serial.println(on ? "VENTILADOR ON" : "VENTILADOR OFF");
}

// POST /api/fan  → con body {"state": true/false} establece; sin body, togglea
// GET  /api/fan  → devuelve estado actual
void handleFan() {
  sendCorsHeaders();
  if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    if (body.indexOf("\"state\"") >= 0)
      setFan(body.indexOf("true") >= 0);
    else
      setFan(!fanOn);
  }
  server.send(200, "application/json",
              "{\"fan\": " + String(fanOn ? "true" : "false") + "}");
}

// POST /api/led  → con body {"state": true/false} establece; sin body, togglea
// GET  /api/led  → devuelve estado actual
void handleBulb() {
  sendCorsHeaders();

  if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    if (body.indexOf("\"state\"") >= 0) {
      // El backend envía el estado deseado directamente → sin riesgo de desync
      bool desired = (body.indexOf("true") >= 0);
      setBulb(desired);
    } else {
      // Botón manual desde el frontend → toggle
      setBulb(!bulbOn);
    }
  }

  String json = "{\"led\": " + String(bulbOn ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// ── Sensor de distancia ───────────────────────────────────────────────────────

void handleMotion() {
  sendCorsHeaders();
  String json = "{\"motion\": " + String(motionDetected ? "true" : "false") +
                ", \"distance\": " + String(lastDistanceCm, 1) + "}";
  server.send(200, "application/json", json);
}

// ── Registro al backend ───────────────────────────────────────────────────────

void registerToBackend() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(backend_url) + "/register");
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"device_type\": \"bridge\", \"ip\": \"" + WiFi.localIP().toString() + "\"}";
  int code = http.POST(payload);
  Serial.println(code == 200 ? "✅ Bridge registrado" : "❌ Error registro bridge: " + String(code));
  http.end();
}

// ── OPTIONS (CORS preflight) ──────────────────────────────────────────────────

void handleOptions() {
  sendCorsHeaders();
  server.send(200, "text/plain", "");
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  pinMode(LED_BOARD, OUTPUT); digitalWrite(LED_BOARD, LOW);
  pinMode(LED_EXT,   OUTPUT); digitalWrite(LED_EXT,   LOW);
  pinMode(FAN_PIN,   OUTPUT); digitalWrite(FAN_PIN,   HIGH); // relay abierto al arrancar
  pinMode(TRIG_PIN,  OUTPUT); digitalWrite(TRIG_PIN,  LOW);
  pinMode(ECHO_PIN,  INPUT);

  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK — IP: " + WiFi.localIP().toString());

  if (MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://esp32-bridge.local");
  }

  server.on("/api/led",    HTTP_GET,     handleBulb);
  server.on("/api/led",    HTTP_POST,    handleBulb);
  server.on("/api/led",    HTTP_OPTIONS, handleOptions);
  server.on("/api/fan",    HTTP_GET,     handleFan);
  server.on("/api/fan",    HTTP_POST,    handleFan);
  server.on("/api/fan",    HTTP_OPTIONS, handleOptions);
  server.on("/api/motion", HTTP_GET,     handleMotion);
  server.on("/api/motion", HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("Servidor ESP32 listo");

  delay(1000);
  registerToBackend();
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  server.handleClient();

  // HC-SR04
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 23500);
  lastDistanceCm = (duration > 0) ? (duration * 0.034f) / 2.0f : -1;
  motionDetected = (lastDistanceCm > 0 && lastDistanceCm <= DETECTION_DISTANCE_CM);

  static unsigned long lastLog = 0;
  if (millis() - lastLog >= 500) {
    lastLog = millis();
    if (lastDistanceCm >= 0)
      Serial.printf("Dist: %.1f cm | motion: %s | bulb: %s\n",
                    lastDistanceCm,
                    motionDetected ? "SI" : "no",
                    bulbOn        ? "ON" : "off");
  }

  delay(60);  // mínimo HC-SR04

  static unsigned long lastReg = 0;
  if (millis() - lastReg > 60000) { lastReg = millis(); registerToBackend(); }
}
