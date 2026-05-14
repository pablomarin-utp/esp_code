#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <Preferences.h>

const char* ssid     = "Pablo";
const char* password = "tinteamo";
const char* hostname = "esp32-bridge";

// Hostname del PC con backend (Windows publica <COMPUTERNAME>.local nativo)
const char* backendHost = "pablo";
const uint16_t backendPort = 8000;

#define LED_BOARD  2
#define LED_EXT    26
#define FAN_PIN    27
#define TRIG_PIN   5
#define ECHO_PIN   18
#define DETECTION_DISTANCE_CM 45

WebServer   server(80);
Preferences prefs;

// URL del backend en LAN. Se resuelve por mDNS al inicio y queda como http://<ip>:8000/
String backendUrl = "http://pablo.local:8000/";

bool  bulbOn         = false;
bool  fanOn          = false;
bool  motionDetected = false;
float lastDistanceCm = -1;

// ── FreeRTOS: cola de mensajes para el task HTTP ──────────────────────────────
struct HttpMsg {
  char url[128];
  char body[128];
};
QueueHandle_t httpQueue;

// Task HTTP — corre en core 0, nunca bloquea el loop del sensor
void httpTask(void* param) {
  HttpMsg msg;
  while (true) {
    if (xQueueReceive(httpQueue, &msg, portMAX_DELAY) == pdTRUE) {
      WiFiClient client;
      HTTPClient http;
      http.setTimeout(8000);
      if (http.begin(client, String(msg.url))) {
        http.addHeader("Content-Type", "application/json");
        int code = http.POST((uint8_t*)msg.body, strlen(msg.body));
        Serial.printf("[HTTP] %d → %s\n", code, msg.url);
        http.end();
      } else {
        Serial.println("[HTTP] begin falló: " + String(msg.url));
      }
    }
  }
}

// Encola un POST sin bloquear el loop
void asyncPost(const String& url, const String& body) {
  HttpMsg msg;
  url.toCharArray(msg.url, sizeof(msg.url));
  body.toCharArray(msg.body, sizeof(msg.body));
  xQueueSend(httpQueue, &msg, 0);  // no espera si la cola está llena
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void setBulb(bool on) {
  bulbOn = on;
  digitalWrite(LED_EXT,   on ? HIGH : LOW);
  digitalWrite(LED_BOARD, on ? HIGH : LOW);
  Serial.println(on ? "💡 LED ENCENDIDO" : "💡 LED APAGADO");
}

void setFan(bool on) {
  fanOn = on;
  digitalWrite(FAN_PIN, on ? LOW : HIGH);
  Serial.println(on ? "🌀 VENTILADOR ON" : "🌀 VENTILADOR OFF");
}

// Resuelve <host>.local en la LAN. Devuelve "" si falla.
String resolveMdns(const char* host) {
  for (int i = 0; i < 8; i++) {
    IPAddress ip = MDNS.queryHost(host);
    if (ip != IPAddress(0, 0, 0, 0)) {
      return ip.toString();
    }
    Serial.printf("   mDNS intento %d: sin respuesta\n", i + 1);
    delay(500);
  }
  return "";
}

// Registro bloqueante (solo en setup y cada 60s — no importa si tarda)
void registerToBackend() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  String url = backendUrl + "api/v1/esp/register";
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"device_type\": \"bridge\", \"ip\": \"" + WiFi.localIP().toString() + "\"}";
    int code = http.POST((uint8_t*)payload.c_str(), payload.length());
    Serial.println(code == 200 ? "✅ Bridge registrado" : "❌ Error registro: " + String(code));
    http.end();
  } else {
    Serial.println("❌ No se pudo abrir conexión al backend");
  }
}

// ── Endpoints ─────────────────────────────────────────────────────────────────

void handleOptions() { sendCorsHeaders(); server.send(200, "text/plain", ""); }

void handleBulb() {
  sendCorsHeaders();
  if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    if (body.indexOf("\"state\"") >= 0)
      setBulb(body.indexOf("true") >= 0);
    else
      setBulb(!bulbOn);
  }
  server.send(200, "application/json",
              "{\"led\": " + String(bulbOn ? "true" : "false") + "}");
}

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

void handleMotion() {
  sendCorsHeaders();
  server.send(200, "application/json",
    "{\"motion\": " + String(motionDetected ? "true" : "false") +
    ", \"distance\": " + String(lastDistanceCm, 1) + "}");
}

void handleConfig() {
  sendCorsHeaders();
  server.send(200, "application/json",
              "{\"backend_url\": \"" + backendUrl + "\"}");
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 Bridge ===");

  pinMode(LED_BOARD, OUTPUT); digitalWrite(LED_BOARD, LOW);
  pinMode(LED_EXT,   OUTPUT); digitalWrite(LED_EXT,   LOW);
  pinMode(FAN_PIN,   OUTPUT); digitalWrite(FAN_PIN,   HIGH);
  pinMode(TRIG_PIN,  OUTPUT); digitalWrite(TRIG_PIN,  LOW);
  pinMode(ECHO_PIN,  INPUT);

  // Override manual guardado por Serial (si existe). Si no, se usa el default.
  prefs.begin("config", true);
  String stored = prefs.getString("backend_url", "");
  prefs.end();
  if (stored.length() > 0) {
    backendUrl = stored;
    Serial.println("Backend URL guardado: " + backendUrl);
  }

  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK — IP: " + WiFi.localIP().toString());

  if (MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS listo: http://esp32-bridge.local");
  }

  // Resuelve pablo.local → IP cada arranque (DHCP puede cambiarla)
  Serial.printf("Resolviendo %s.local por mDNS...\n", backendHost);
  String ip = resolveMdns(backendHost);
  if (ip.length() > 0) {
    backendUrl = "http://" + ip + ":" + String(backendPort) + "/";
    Serial.println("✅ Backend (mDNS): " + backendUrl);
  } else {
    Serial.println("⚠️  mDNS falló — usando: " + backendUrl);
  }

  server.on("/api/config", HTTP_GET,     handleConfig);
  server.on("/api/config", HTTP_OPTIONS, handleOptions);
  server.on("/api/led",    HTTP_GET,     handleBulb);
  server.on("/api/led",    HTTP_POST,    handleBulb);
  server.on("/api/led",    HTTP_OPTIONS, handleOptions);
  server.on("/api/fan",    HTTP_GET,     handleFan);
  server.on("/api/fan",    HTTP_POST,    handleFan);
  server.on("/api/fan",    HTTP_OPTIONS, handleOptions);
  server.on("/api/motion", HTTP_GET,     handleMotion);
  server.on("/api/motion", HTTP_OPTIONS, handleOptions);

  server.begin();
  Serial.println("Servidor listo");

  // Cola de 4 mensajes, task en core 0
  httpQueue = xQueueCreate(4, sizeof(HttpMsg));
  xTaskCreatePinnedToCore(httpTask, "httpTask", 8192, NULL, 1, NULL, 0);
  Serial.println("HTTP task iniciado en core 0");

  delay(500);
  registerToBackend();
}

// ── Loop (core 1) ─────────────────────────────────────────────────────────────

void loop() {
  server.handleClient();

  // Cambiar URL desde Serial Monitor (override permanente)
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.startsWith("http")) {
      if (!input.endsWith("/")) input += "/";
      backendUrl = input;
      prefs.begin("config", false);
      prefs.putString("backend_url", backendUrl);
      prefs.end();
      Serial.println("✅ URL guardada: " + backendUrl);
      registerToBackend();
    } else if (input == "reset") {
      prefs.begin("config", false);
      prefs.remove("backend_url");
      prefs.end();
      Serial.println("✅ Preferences borradas (reinicia para re-resolver mDNS)");
    }
  }

  // HC-SR04 — lee sin bloquearse
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 23500);
  lastDistanceCm = (duration > 0) ? (duration * 0.034f) / 2.0f : -1;
  motionDetected = (lastDistanceCm > 0 && lastDistanceCm <= DETECTION_DISTANCE_CM);

  static unsigned long lastLog = 0;
  if (millis() - lastLog >= 500) {
    lastLog = millis();
    Serial.printf("Dist: %.1f cm | motion: %s | bulb: %s | fan: %s\n",
                  lastDistanceCm,
                  motionDetected ? "SI" : "no",
                  bulbOn ? "ON" : "off",
                  fanOn  ? "ON" : "off");
  }

  // Push al backend cada 500ms — no bloqueante
  static unsigned long lastPush = 0;
  if (millis() - lastPush >= 500) {
    lastPush = millis();
    if (WiFi.status() == WL_CONNECTED) {
      String body = "{\"motion\": " + String(motionDetected ? "true" : "false") +
                    ", \"distance\": " + String(lastDistanceCm, 1) + "}";
      asyncPost(backendUrl + "api/v1/esp/motion", body);
    }
  }

  // Re-registro cada 60s
  static unsigned long lastReg = 0;
  if (millis() - lastReg > 60000) { lastReg = millis(); registerToBackend(); }

  delay(60);
}
