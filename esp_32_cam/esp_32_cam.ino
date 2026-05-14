#include <WiFi.h>
#include <esp_camera.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>

const char* ssid     = "Pablo";
const char* password = "tinteamo";

// Hostname del PC con backend (Windows publica <COMPUTERNAME>.local nativo)
const char* backendHost = "pablo";
const uint16_t backendPort = 8000;

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define LED_PIN           4

// URL por defecto: backend en LAN. Se sobreescribe al arrancar con lo que responda el Bridge,
// o por resolución mDNS si el Bridge no contesta.
String backend_url = "http://pablo.local:8000/api/v1/esp";

WebServer server(80);
bool ledState = false;

// ── Cámara ────────────────────────────────────────────────────────────────────

camera_config_t getCameraConfig() {
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sscb_sda  = SIOD_GPIO_NUM;
  config.pin_sscb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.frame_size    = FRAMESIZE_SVGA;
  config.pixel_format  = PIXFORMAT_JPEG;
  config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location   = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality  = 10;
  config.fb_count      = 2;
  return config;
}

// ── Handlers HTTP ─────────────────────────────────────────────────────────────

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleCapture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  server.client().write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void handleStream() {
  WiFiClient client = server.client();
  client.print("HTTP/1.1 200 OK\r\n"
               "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n");
  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) break;
    String header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                    + String(fb->len) + "\r\n\r\n";
    client.print(header);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);
    delay(50);
  }
}

void handleStatus() {
  server.send(200, "application/json",
    "{\"status\": \"connected\", \"ip\": \"" + WiFi.localIP().toString() + "\"}");
}

void handleLedToggle() {
  sendCorsHeaders();
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  server.send(200, "application/json",
    "{\"led\": " + String(ledState ? "true" : "false") + "}");
}

void handleLedStatus() {
  sendCorsHeaders();
  server.send(200, "application/json",
    "{\"led\": " + String(ledState ? "true" : "false") + "}");
}

// ── Resolución de URL del backend ─────────────────────────────────────────────

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

// Intenta obtener la URL del backend desde el Bridge via mDNS.
// Si falla, resuelve directamente pablo.local.
void fetchConfigFromBridge() {
  Serial.println("Pidiendo config al Bridge (esp32-bridge.local)...");
  for (int attempt = 1; attempt <= 3; attempt++) {
    HTTPClient http;
    http.begin("http://esp32-bridge.local/api/config");
    http.setTimeout(4000);
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      int idx = body.indexOf("\"backend_url\":\"");
      if (idx >= 0) {
        int start = idx + 15;
        int end   = body.indexOf("\"", start);
        String base = body.substring(start, end);
        if (!base.endsWith("/")) base += "/";
        backend_url = base + "api/v1/esp";
        Serial.println("✅ Config del Bridge: " + backend_url);
        http.end();
        return;
      }
    }
    http.end();
    Serial.printf("   intento %d falló (código %d)\n", attempt, code);
    delay(1000);
  }

  // Fallback: resolver pablo.local directamente
  Serial.printf("Bridge no respondió. Resolviendo %s.local...\n", backendHost);
  String ip = resolveMdns(backendHost);
  if (ip.length() > 0) {
    backend_url = "http://" + ip + ":" + String(backendPort) + "/api/v1/esp";
    Serial.println("✅ Backend (mDNS): " + backend_url);
  } else {
    Serial.println("⚠️  Usando URL por defecto: " + backend_url);
  }
}

// ── Helper HTTP ───────────────────────────────────────────────────────────────

int httpPost(const String& url, const uint8_t* buf, size_t len, const String& contentType) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", contentType);
  int code = http.POST((uint8_t*)buf, len);
  http.end();
  return code;
}

int httpPostJson(const String& url, const String& body) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code;
}

// ── Registro al backend ───────────────────────────────────────────────────────

void sendIpToBackend() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi no conectado, saltando registro");
    return;
  }
  String payload = "{\"device_type\": \"cam\", \"ip\": \""
                   + WiFi.localIP().toString() + "\"}";
  int code = httpPostJson(backend_url + "/register", payload);
  Serial.println(code == 200
    ? "✅ CAM registrada: " + WiFi.localIP().toString()
    : "❌ Error registro: " + String(code));
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32-CAM ===");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  camera_config_t config = getCameraConfig();
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("❌ Camera init failed: 0x%x\n", err);
    return;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);

  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK — IP: " + WiFi.localIP().toString());

  // mDNS local necesario para queryHost
  if (!MDNS.begin("esp32-cam")) {
    Serial.println("⚠️  MDNS.begin falló");
  }

  // Dar tiempo al Bridge para que esté listo (mDNS)
  delay(2000);

  fetchConfigFromBridge();
  sendIpToBackend();

  server.on("/",        HTTP_GET, handleStream);
  server.on("/stream",  HTTP_GET, handleStream);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/status",  HTTP_GET, handleStatus);
  server.on("/api/led", HTTP_POST, handleLedToggle);
  server.on("/api/led", HTTP_GET,  handleLedStatus);

  server.begin();
  Serial.println("Servidor listo");
}

// ── Push de frame al backend ──────────────────────────────────────────────────

void pushFrameToBackend() {
  if (WiFi.status() != WL_CONNECTED) return;
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("⚠️  No se pudo capturar frame"); return; }

  int code = httpPost(backend_url + "/cam/frame", fb->buf, fb->len, "image/jpeg");
  if (code != 200) Serial.printf("⚠️  Push frame: %d\n", code);
  esp_camera_fb_return(fb);
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  server.handleClient();

  // Push de frame cada 1 s — el backend lo usa para reconocimiento
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame >= 1000) {
    lastFrame = millis();
    pushFrameToBackend();
  }

  // Re-registro cada 30 s
  static unsigned long lastReg = 0;
  if (millis() - lastReg > 30000) {
    lastReg = millis();
    sendIpToBackend();
  }
}
