#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <driver/can.h>   // ESP32 CAN (TWAI) built-in driver

const char* ssid     = "ESP32";
const char* password = "12345679";

WebServer server(80);

/* ===== CAN CONFIG =====
 * TX: GPIO 5  -> CTX của TJA1050
 * RX: GPIO 4  -> CRX của TJA1050
 *
 * QUAN TRỌNG - ESP32 không 5V tolerant:
 * TJA1050 output (CRX) là 5V, ESP32 GPIO chịu max 3.3V
 * Cần level shifting trên đường TJA1050 CRX -> ESP32 GPIO4:
 *
 *   TJA1050 CRX ---[3.3kΩ]---+--- ESP32 GPIO4
 *                              |
 *                           [6.8kΩ]
 *                              |
 *                             GND
 *   Ratio = 6.8/(3.3+6.8) = 0.673 => 5V * 0.673 = 3.36V ≈ 3.3V ✓
 *
 * FLYBACK DIODE:
 * - Relay cuộn dây: 1N4007 song song, cathode về VCC
 * - Van điện từ cuộn dây: 1N4007 song song, cathode về 12V
 */
#define CAN_TX_PIN  GPIO_NUM_5
#define CAN_RX_PIN  GPIO_NUM_4

/* CAN Message IDs - phải khớp với STM32 */
#define CAN_ID_PRESSURE   0x100   // STM32 -> ESP32: áp suất hiện tại
#define CAN_ID_SETPOINT   0x101   // ESP32 -> STM32: setpoint PSI
#define CAN_ID_CMD        0x102   // ESP32 -> STM32: lệnh thủ công

/* Commands */
#define CMD_PUMP_OFF   0x00
#define CMD_PUMP_ON    0x01
#define CMD_AUTO_MODE  0x02

/* Dữ liệu nhận từ STM32 */
float currentPressure = 0.0;
bool  pumpRunning     = false;
bool  autoMode        = true;

/* ===== CAN HELPERS ===== */

/* Pack float -> 4 byte big-endian */
void packFloat(uint8_t* buf, float val) {
  uint32_t raw;
  memcpy(&raw, &val, 4);
  buf[0] = (raw >> 24) & 0xFF;
  buf[1] = (raw >> 16) & 0xFF;
  buf[2] = (raw >> 8)  & 0xFF;
  buf[3] =  raw        & 0xFF;
}

/* Unpack 4 byte big-endian -> float */
float unpackFloat(uint8_t* buf) {
  uint32_t raw = ((uint32_t)buf[0] << 24) |
                 ((uint32_t)buf[1] << 16) |
                 ((uint32_t)buf[2] << 8)  |
                  (uint32_t)buf[3];
  float val;
  memcpy(&val, &raw, 4);
  return val;
}

void canInit() {
  can_general_config_t g_config = CAN_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, CAN_MODE_NORMAL);
  can_timing_config_t  t_config = CAN_TIMING_CONFIG_500KBITS();
  can_filter_config_t  f_config = CAN_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = can_driver_install(&g_config, &t_config, &f_config);
  if (err != ESP_OK) {
    Serial.printf("CAN driver install failed: %s\n", esp_err_to_name(err));
    return;
  }
  err = can_start();
  if (err != ESP_OK) {
    Serial.printf("CAN start failed: %s\n", esp_err_to_name(err));
    return;
  }
  Serial.println("CAN OK");
}

void canSendSetpoint(float psi) {
  can_message_t msg;
  msg.identifier = CAN_ID_SETPOINT;
  msg.flags      = CAN_MSG_FLAG_NONE;
  msg.data_length_code = 4;
  packFloat(msg.data, psi);
  can_transmit(&msg, pdMS_TO_TICKS(10));
}

void canSendCmd(uint8_t cmd) {
  can_message_t msg;
  msg.identifier = CAN_ID_CMD;
  msg.flags      = CAN_MSG_FLAG_NONE;
  msg.data_length_code = 1;
  msg.data[0] = cmd;
  can_transmit(&msg, pdMS_TO_TICKS(10));
}

void canReceive() {
  can_message_t msg;
  while (can_receive(&msg, 0) == ESP_OK) {
    if (msg.identifier == CAN_ID_PRESSURE && msg.data_length_code >= 6) {
      currentPressure = unpackFloat(msg.data);
      pumpRunning     = msg.data[4];
      autoMode        = msg.data[5];
    }
  }
}

/* ===== HELPER: thêm CORS header ===== */
void addCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET");
}

void setup() 
{
  Serial.begin(115200);
  delay(1000);  // Đợi Serial ổn định

  // ── SPIFFS ──
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS lỗi! Tiếp tục mà không có file system.");
    // Không return, để AP và server vẫn chạy (nhưng route file sẽ fail)
  } else {
    Serial.println("SPIFFS OK");
  }

  // ── CAN ──
  canInit();

  // ── WiFi Access Point ──
  WiFi.disconnect(true);  // Ngắt kết nối trước nếu có
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(ssid, password);
  Serial.print("softAP: ");
  Serial.println(apOk ? "OK" : "FAIL");
  Serial.print("IP ESP32: ");
  Serial.println(WiFi.softAPIP());
  delay(2000);  // Đợi AP khởi động hoàn toàn

  // ── Route: trang chính ──
  server.on("/", HTTP_GET, []() {
    if (!SPIFFS.exists("/index.html")) {
      server.send(404, "text/plain", "index.html not found");
      return;
    }
    File file = SPIFFS.open("/index.html", FILE_READ);
    if (!file) { 
      server.send(404, "text/plain", "index.html not found"); 
      return; 
    }
    server.streamFile(file, "text/html");
    file.close();
  });

  // ── Route: cars.json ──
  server.on("/cars.json", HTTP_GET, []() {
    if (!SPIFFS.exists("/cars.json")) {
      server.send(404, "text/plain", "cars.json not found");
      return;
    }
    File file = SPIFFS.open("/cars.json", FILE_READ);
    if (!file) { 
      server.send(404, "text/plain", "cars.json not found"); 
      return; 
    }
    addCORSHeaders();
    server.streamFile(file, "application/json");
    file.close();
  });

  // ── Route: áp suất hiện tại ──
  server.on("/current", HTTP_GET, []() {
    addCORSHeaders();
    String json = "{\"pressure\":" + String(currentPressure, 1) +
                  ",\"pump\":"     + String(pumpRunning ? "true" : "false") +
                  ",\"auto\":"     + String(autoMode    ? "true" : "false") + "}";
    server.send(200, "application/json", json);
  });

  // ── Route: lệnh bơm từ web ──
  server.on("/pump", HTTP_GET, []() {
    String target = server.arg("target");
    String cmd    = server.arg("cmd");

    if (cmd == "on") {
      canSendCmd(CMD_PUMP_ON);
    } else if (cmd == "off") {
      canSendCmd(CMD_PUMP_OFF);
    } else if (cmd == "auto" && target.length() > 0) {
      float psi = target.toFloat();
      canSendSetpoint(psi);
      canSendCmd(CMD_AUTO_MODE);
    }

    Serial.println("Lenh bom: cmd=" + cmd + " target=" + target);
    addCORSHeaders();
    server.send(200, "text/plain", "OK");
  });

  // ── Route: áp suất đề xuất ──
  server.on("/pressure", HTTP_GET, []() {
    String brand = server.arg("brand");
    String model = server.arg("model");

    if (!SPIFFS.exists("/cars.json")) {
      server.send(500, "text/plain", "-1");
      return;
    }
    File file = SPIFFS.open("/cars.json", FILE_READ);
    if (!file) {
      server.send(500, "text/plain", "-1");
      return;
    }

    DynamicJsonDocument doc(16384);
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
      Serial.println("JSON parse lỗi: " + String(err.c_str()));
      server.send(500, "text/plain", "-1");
      return;
    }

    for (JsonObject car : doc.as<JsonArray>()) {
      String b = car["brand"].as<String>();
      String m = car["model"].as<String>();
      b.trim();
      m.trim();
      if (b == brand && m == model) {
        addCORSHeaders();
        server.send(200, "text/plain", String((int)car["pressure"]));
        return;
      }
    }
    server.send(200, "text/plain", "-1");
  });

  server.begin();
  Serial.println("Web server started");
}

void loop() {
  canReceive();
  server.handleClient();
  delay(10); 
}