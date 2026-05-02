#include <WiFi.h>
#include <esp_camera.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

const char* ssid = "MARIN";
const char* password = "10028370";

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

const char* backend_url = "http://192.168.1.100:8000/api/v1/recognize";

WebServer server(80);
String esp32_ip = "";
bool ledState = false;

camera_config_t getCameraConfig() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_SVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 10;
  config.fb_count = 2;

  return config;
}

void sendFrameToBackend(camera_fb_t* fb) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(backend_url);
    http.addHeader("Content-Type", "image/jpeg");

    int httpCode = http.POST(fb->buf, fb->len);
    Serial.printf("Backend response: %d\n", httpCode);
    http.end();
  }
}

void handleStream() {
  WiFiClient* client = server.client();
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n";
  response += "Access-Control-Allow-Origin: *\r\n";
  response += "\r\n";
  client->print(response);

  while (client->connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      break;
    }

    String header = "--frame\r\n";
    header += "Content-Type: image/jpeg\r\n";
    header += "Content-Length: " + String(fb->len) + "\r\n";
    header += "\r\n";
    client->print(header);
    client->write(fb->buf, fb->len);
    client->print("\r\n");

    esp_camera_fb_return(fb);
    delay(50);  // ~20fps max
  }
}

void handleStatus() {
  String status = "{\"status\": \"connected\", \"ip\": \"" + WiFi.localIP().toString() + "\"}";
  server.send(200, "application/json", status);
}

void handleCapture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }

  server.sendHeader("Content-Type", "image/jpeg");
  server.sendHeader("Content-Length", String(fb->len));
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.client().write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleLedToggle() {
  sendCorsHeaders();
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  String json = "{\"led\": " + String(ledState ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void handleLedStatus() {
  sendCorsHeaders();
  String json = "{\"led\": " + String(ledState ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

void sendIpToEsp32() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://esp32-bridge.local/api/cam_ip");
    http.addHeader("Content-Type", "application/json");
    
    String payload = "{\"ip\": \"" + WiFi.localIP().toString() + "\"}";
    int httpCode = http.POST(payload);
    Serial.printf("IP enviada a puente. HTTP: %d\n", httpCode);
    if (httpCode < 0) {
      Serial.println("Error de conexion al puente");
    } else if (httpCode == 200) {
      Serial.println("IP enviada correctamente");
    }
    http.end();
  } else {
    Serial.println("WiFi no conectado");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  camera_config_t config = getCameraConfig();
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);

  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  sendIpToEsp32();

  server.on("/", HTTP_GET, handleStream);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/api/led", HTTP_POST, handleLedToggle);
  server.on("/api/led", HTTP_GET, handleLedStatus);

  server.begin();
  Serial.println("Servidor iniciado!");
}

void loop() {
  server.handleClient();

  static unsigned long lastIpSent = 0;
  if (millis() - lastIpSent > 5000) {
    sendIpToEsp32();
    lastIpSent = millis();
  }
}