#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ===== WiFi =====
const char* ssid     = "ESP32_WIFI";
const char* password = "12345678";
WebServer server(80);

// ===== TFT ST7735 1.77" 128x160 =====
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// ===== Nút nhấn =====
#define BTN_UP     12
#define BTN_DOWN   13
#define BTN_SEL    14

// ===== Hardware =====
#define PUMP_RELAY_PIN  5
#define PRESSURE_PIN    34
#define ADC_SAMPLES     10

// ===== Màu =====
#define C_BG       0x0000  // đen
#define C_RED      0xF800
#define C_GREEN    0x07E0
#define C_YELLOW   0xFFE0
#define C_WHITE    0xFFFF
#define C_GRAY     0x7BEF
#define C_DARKGRAY 0x39E7
#define C_HEADER   0xA000  // đỏ tối

// ===== Trạng thái bơm =====
bool  pumpRunning    = false;
float targetPressure = 0.0;
float currentPressure = 0.0;

// ===== Menu =====
enum Screen { SCR_MAIN, SCR_BRAND, SCR_MODEL, SCR_PUMP };
Screen currentScreen = SCR_MAIN;

// Danh sách hãng xe (lấy từ cars.json)
#define MAX_BRANDS  20
#define MAX_MODELS  30
String brands[MAX_BRANDS];
int    brandCount = 0;

String models[MAX_MODELS];
int    modelPressure[MAX_MODELS];
int    modelCount = 0;

String selectedBrand = "";
String selectedModel = "";
int    suggestedPsi  = 0;
int    customPsi     = 0;

int    menuIndex  = 0;   // vị trí con trỏ trong danh sách
int    menuScroll = 0;   // offset cuộn

// ===== Nút nhấn debounce =====
unsigned long lastBtnTime = 0;
#define DEBOUNCE_MS 200

// ===== Cập nhật áp suất =====
unsigned long lastPressureRead = 0;

// ============================================================
// ĐỌC CẢM BIẾN
// ============================================================
float readPressurePSI() {
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(PRESSURE_PIN);
    delay(2);
  }
  float adcAvg = sum / (float)ADC_SAMPLES;
  float psi = ((adcAvg - 820.0) / (4095.0 - 820.0)) * 50.0;
  if (psi < 0) psi = 0;
  return psi;
}

// ============================================================
// LOAD BRANDS TỪ SPIFFS
// ============================================================
void loadBrands() {
  brandCount = 0;
  File f = SPIFFS.open("/cars.json", FILE_READ);
  if (!f) return;

  DynamicJsonDocument doc(32768);
  deserializeJson(doc, f);
  f.close();

  for (JsonObject car : doc.as<JsonArray>()) {
    String b = car["brand"].as<String>();
    b.trim();
    bool found = false;
    for (int i = 0; i < brandCount; i++) {
      if (brands[i] == b) { found = true; break; }
    }
    if (!found && brandCount < MAX_BRANDS) brands[brandCount++] = b;
  }
}

void loadModels(String brand) {
  modelCount = 0;
  File f = SPIFFS.open("/cars.json", FILE_READ);
  if (!f) return;

  DynamicJsonDocument doc(32768);
deserializeJson(doc, f);
  f.close();

  for (JsonObject car : doc.as<JsonArray>()) {
    String b = car["brand"].as<String>();
    b.trim();
    if (b != brand) continue;

    String m = car["model"].as<String>();
    m.trim();
    bool found = false;
    for (int i = 0; i < modelCount; i++) {
      if (models[i] == m) { found = true; break; }
    }
    if (!found && modelCount < MAX_MODELS) {
      models[modelCount]        = m;
      modelPressure[modelCount] = (int)car["pressure"];
      modelCount++;
    }
  }
}

// ============================================================
// VẼ MÀN HÌNH
// ============================================================
void drawHeader(const char* title) {
  tft.fillRect(0, 0, 128, 18, C_HEADER);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 5);
  tft.print(title);
  // Áp suất hiện tại ở góc phải header
  tft.setCursor(80, 5);
  tft.printf("%.1fPSI", currentPressure);
}

void drawFooter() {
  tft.fillRect(0, 150, 128, 10, C_DARKGRAY);
  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(2, 152);
  tft.print("UP DN SEL");
}

// --- Màn hình chính ---
void drawMain() {
  tft.fillScreen(C_BG);
  drawHeader("PUMP SYSTEM");

  // Áp suất lớn ở giữa
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 24);
  tft.print("Ap suat hien tai:");

  tft.setTextColor(currentPressure < 28 ? C_RED : C_GREEN);
  tft.setTextSize(3);
  tft.setCursor(10, 36);
  tft.printf("%.1f", currentPressure);
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(90, 50);
  tft.print("PSI");

  // Trạng thái bơm
  tft.setTextSize(1);
  tft.setCursor(4, 80);
  if (pumpRunning) {
    tft.setTextColor(C_YELLOW);
    tft.print("DANG BOM...");
    tft.setCursor(4, 92);
    tft.setTextColor(C_WHITE);
    tft.printf("Muc tieu: %d PSI", (int)targetPressure);
  } else {
    tft.setTextColor(C_GREEN);
    tft.print("San sang");
  }

  // Hướng dẫn
  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 110);
  tft.print("SEL: Chon xe & bom");
  tft.setCursor(4, 122);
  tft.print("UP/DN: Xem ap suat");

  drawFooter();
}

// --- Màn hình chọn hãng ---
void drawBrandScreen() {
  tft.fillScreen(C_BG);
  drawHeader("CHON HANG XE");

  int visibleRows = 6;
  for (int i = 0; i < visibleRows && (menuScroll + i) < brandCount; i++) {
    int idx = menuScroll + i;
    int y   = 22 + i * 20;

    if (idx == menuIndex) {
      tft.fillRect(0, y - 1, 128, 18, C_RED);
      tft.setTextColor(C_WHITE);
    } else {
      tft.setTextColor(idx % 2 == 0 ? C_WHITE : C_GRAY);
    }

    tft.setTextSize(1);
    tft.setCursor(6, y + 4);
    tft.print(brands[idx]);
  }

  // scroll indicator
  if (brandCount > visibleRows) {
    tft.setTextColor(C_GRAY);
    tft.setCursor(112, 22);
    tft.print(menuScroll > 0 ? "^" : " ");
    tft.setCursor(112, 140);
    tft.print((menuScroll + visibleRows) < brandCount ? "v" : " ");
  }

  drawFooter();
}
// --- Màn hình chọn model ---
void drawModelScreen() {
  tft.fillScreen(C_BG);
  drawHeader("CHON MODEL");

  int visibleRows = 6;
  for (int i = 0; i < visibleRows && (menuScroll + i) < modelCount; i++) {
    int idx = menuScroll + i;
    int y   = 22 + i * 20;

    if (idx == menuIndex) {
      tft.fillRect(0, y - 1, 128, 18, C_RED);
      tft.setTextColor(C_WHITE);
    } else {
      tft.setTextColor(idx % 2 == 0 ? C_WHITE : C_GRAY);
    }

    tft.setTextSize(1);
    tft.setCursor(6, y + 4);
    tft.printf("%-12s %dP", models[idx].c_str(), modelPressure[idx]);
  }

  drawFooter();
}

// --- Màn hình bơm ---
void drawPumpScreen() {
  tft.fillScreen(C_BG);
  drawHeader("BOM LOP");

  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(4, 22);
  tft.printf("%s %s", selectedBrand.c_str(), selectedModel.c_str());

  tft.setCursor(4, 34);
  tft.setTextColor(C_WHITE);
  tft.printf("De xuat: %d PSI", suggestedPsi);

  tft.setCursor(4, 46);
  tft.printf("Hien tai: %.1f PSI", currentPressure);

  // 2 lựa chọn
  // Option 0: bơm theo đề xuất
  // Option 1: bơm tùy chỉnh (+-1 PSI)
  // menuIndex 0 hoặc 1

  // Option: Bơm đề xuất
  if (menuIndex == 0) tft.fillRect(0, 62, 128, 18, C_RED);
  else tft.fillRect(0, 62, 128, 18, C_DARKGRAY);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(6, 67);
  tft.printf("Bom de xuat: %d PSI", suggestedPsi);

  // Option: Bơm tùy chỉnh
  if (menuIndex == 1) tft.fillRect(0, 84, 128, 18, C_RED);
  else tft.fillRect(0, 84, 128, 18, C_DARKGRAY);
  tft.setTextColor(C_WHITE);
  tft.setCursor(6, 89);
  tft.printf("Tuy chinh: %d PSI", customPsi);

  // Giới hạn
  int maxPsi = (int)(suggestedPsi * 1.05);
  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 108);
  tft.printf("Max: %d PSI (105%%)", maxPsi);

  // UP/DN để chỉnh customPsi khi ở option 1
  if (menuIndex == 1) {
    tft.setTextColor(C_YELLOW);
    tft.setCursor(4, 120);
    tft.print("UP/DN: chinh PSI");
    tft.setCursor(4, 132);
    tft.print("SEL: Xac nhan bom");
  } else {
    tft.setTextColor(C_YELLOW);
    tft.setCursor(4, 120);
    tft.print("SEL: Bat dau bom");
  }

  drawFooter();
}

void drawPumping() {
  tft.fillScreen(C_BG);
  drawHeader("DANG BOM...");

  tft.setTextColor(C_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(10, 30);
  tft.print("PUMPING");

  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  tft.setCursor(4, 60);
  tft.printf("Muc tieu: %d PSI", (int)targetPressure);

  tft.setCursor(4, 75);
  tft.printf("Hien tai: %.1f PSI", currentPressure);

  // Progress bar
  int pct = 0;
  if (targetPressure > 0 && currentPressure > 0)
    pct = (int)((currentPressure / targetPressure) * 100);
  if (pct > 100) pct = 100;

  tft.drawRect(4, 92, 120, 12, C_GRAY);
  tft.fillRect(4, 92, (int)(120 * pct / 100.0), 12, C_GREEN);

  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 110);
  tft.print("SEL: Huy bom");
}

// ============================================================
// XỬ LÝ NÚT NHẤN
// ============================================================
void handleButtons() {
  if (millis() - lastBtnTime < DEBOUNCE_MS) return;

  bool up  = digitalRead(BTN_UP)   == LOW;
  bool dn  = digitalRead(BTN_DOWN) == LOW;
  bool sel = digitalRead(BTN_SEL)  == LOW;

  if (!up && !dn && !sel) return;
  lastBtnTime = millis();

  if (currentScreen == SCR_MAIN) {
    if (sel) {
      // Vào chọn hãng
      loadBrands();
      menuIndex  = 0;
      menuScroll = 0;
      currentScreen = SCR_BRAND;
      drawBrandScreen();
    }

  } else if (currentScreen == SCR_BRAND) {
    if (up && menuIndex > 0) {
      menuIndex--;
      if (menuIndex < menuScroll) menuScroll--;
      drawBrandScreen();
    } else if (dn && menuIndex < brandCount - 1) {
      menuIndex++;
      if (menuIndex >= menuScroll + 6) menuScroll++;
      drawBrandScreen();
    } else if (sel) {
      selectedBrand = brands[menuIndex];
      loadModels(selectedBrand);
      menuIndex  = 0;
      menuScroll = 0;
      currentScreen = SCR_MODEL;
      drawModelScreen();
    }

  } else if (currentScreen == SCR_MODEL) {
    if (up && menuIndex > 0) {
      menuIndex--;
      if (menuIndex < menuScroll) menuScroll--;
      drawModelScreen();
    } else if (dn && menuIndex < modelCount - 1) {
      menuIndex++;
      if (menuIndex >= menuScroll + 6) menuScroll++;
      drawModelScreen();
    } else if (sel) {
      selectedModel = models[menuIndex];
      suggestedPsi  = modelPressure[menuIndex];
      customPsi     = suggestedPsi;
      menuIndex     = 0;
      currentScreen = SCR_PUMP;
      drawPumpScreen();
    }

  } else if (currentScreen == SCR_PUMP) {
    int maxPsi = (int)(suggestedPsi * 1.05);

    if (menuIndex == 0) {
      // đang ở option "bơm đề xuất"
      if (dn) { menuIndex = 1; drawPumpScreen(); }
      else if (sel) {
        // Bơm theo đề xuất
        targetPressure = suggestedPsi;
        pumpRunning    = true;
        digitalWrite(PUMP_RELAY_PIN, HIGH);
        drawPumping();
      }
    } else {
      // đang ở option "tùy chỉnh"
      if (up) {
        menuIndex = 0;
        drawPumpScreen();
      } else if (sel) {
        if (customPsi > maxPsi) customPsi = maxPsi;
        targetPressure = customPsi;
        pumpRunning    = true;
        digitalWrite(PUMP_RELAY_PIN, HIGH);
        drawPumping();
      } else if (up && menuIndex == 1) {
        // tăng PSI (chỉ khi đang ở option 1 và nhấn UP)
        // Note: logic UP ở đây bị override bởi "quay lại option 0"
        // Dùng long press hoặc tách riêng - đơn giản hóa: UP/DN khi ở option 1 chỉnh PSI
      }
    }
  }
}

// Xử lý UP/DN chỉnh customPsi khi đang ở SCR_PUMP option 1
// Tách riêng để tránh conflict với nav
void handlePumpCustomBtn() {
  if (currentScreen != SCR_PUMP || menuIndex != 1) return;
  if (millis() - lastBtnTime < DEBOUNCE_MS) return;

  bool up = digitalRead(BTN_UP)   == LOW;
  bool dn = digitalRead(BTN_DOWN) == LOW;
if (!up && !dn) return;

  lastBtnTime = millis();
  int maxPsi  = (int)(suggestedPsi * 1.05);

  if (up && customPsi < maxPsi) { customPsi++; drawPumpScreen(); }
  if (dn && customPsi > suggestedPsi) { customPsi--; drawPumpScreen(); }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);

  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SEL,  INPUT_PULLUP);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // TFT
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // landscape nếu cần, hoặc 0 cho portrait
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    tft.setTextColor(C_RED);
    tft.setCursor(4, 4);
    tft.print("SPIFFS LOI!");
    return;
  }

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  // Routes web (giữ nguyên chức năng web)
  server.on("/", HTTP_GET, []() {
    File f = SPIFFS.open("/index.html", FILE_READ);
    if (!f) { server.send(404, "text/plain", "Not found"); return; }
    server.streamFile(f, "text/html");
    f.close();
  });

  server.on("/cars.json", HTTP_GET, []() {
    File f = SPIFFS.open("/cars.json", FILE_READ);
    if (!f) { server.send(404, "text/plain", "Not found"); return; }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.streamFile(f, "application/json");
    f.close();
  });

  server.on("/current", HTTP_GET, []() {
    String json = "{\"pressure\":" + String(currentPressure, 1) +
                  ",\"pump\":"     + String(pumpRunning ? "true" : "false") +
                  ",\"auto\":"     + String(targetPressure > 0 ? "true" : "false") + "}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
  });

  server.on("/pump", HTTP_GET, []() {
    String cmd    = server.arg("cmd");
    String target = server.arg("target");

    if (cmd == "on") {
      pumpRunning = true;
      digitalWrite(PUMP_RELAY_PIN, HIGH);
    } else if (cmd == "off") {
      pumpRunning    = false;
      targetPressure = 0.0;
      digitalWrite(PUMP_RELAY_PIN, LOW);
      if (currentScreen == SCR_PUMP) { currentScreen = SCR_MAIN; drawMain(); }
    } else if (cmd == "auto" && target.length() > 0) {
      targetPressure = target.toFloat();
      pumpRunning    = true;
      digitalWrite(PUMP_RELAY_PIN, HIGH);
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "OK");
  });

  server.on("/pressure", HTTP_GET, []() {
    String brand = server.arg("brand");
    String model = server.arg("model");
    File f = SPIFFS.open("/cars.json", FILE_READ);
    if (!f) { server.send(500, "text/plain", "-1"); return; }
    DynamicJsonDocument doc(32768);
    deserializeJson(doc, f);
f.close();
    for (JsonObject car : doc.as<JsonArray>()) {
      String b = car["brand"].as<String>(); b.trim();
      String m = car["model"].as<String>(); m.trim();
      if (b == brand && m == model) {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "text/plain", String((int)car["pressure"]));
        return;
      }
    }
    server.send(200, "text/plain", "-1");
  });

  server.begin();

  // Vẽ màn hình chính
  currentPressure = readPressurePSI();
  drawMain();
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  server.handleClient();
  handleButtons();

  // Đọc áp suất mỗi 300ms
  if (millis() - lastPressureRead >= 300) {
    lastPressureRead = millis();
    currentPressure  = readPressurePSI();

    // Auto mode: tắt bơm khi đạt setpoint
    if (pumpRunning && targetPressure > 0) {
      if (currentPressure >= targetPressure) {
        pumpRunning    = false;
        targetPressure = 0.0;
        digitalWrite(PUMP_RELAY_PIN, LOW);
        Serial.println("Dat muc tieu, tat bom");

        // Hiển thị done trên TFT
        tft.fillScreen(C_BG);
        drawHeader("HOAN THANH");
        tft.setTextColor(C_GREEN);
        tft.setTextSize(2);
        tft.setCursor(10, 50);
        tft.print("DONE!");
        tft.setTextSize(1);
        tft.setTextColor(C_WHITE);
        tft.setCursor(4, 80);
        tft.printf("%.1f PSI", currentPressure);
        tft.setCursor(4, 100);
        tft.setTextColor(C_GRAY);
        tft.print("SEL: Ve man chinh");
        return;
      }
      // Cập nhật màn hình bơm
      if (currentScreen == SCR_PUMP) drawPumping();
    }

    // Cập nhật header áp suất ở màn hình chính
    if (currentScreen == SCR_MAIN) drawMain();
  }

  // SEL từ màn hình DONE về main
  if (!pumpRunning && digitalRead(BTN_SEL) == LOW &&
      millis() - lastBtnTime > DEBOUNCE_MS) {
    lastBtnTime   = millis();
    currentScreen = SCR_MAIN;
    drawMain();
  }
}

// NOTE: Thêm vào platformio.ini:
// lib_deps =
//     bblanchon/ArduinoJson @ ^6.21.0
//     adafruit/Adafruit ST7735 and ST7789 Library
//     adafruit/Adafruit GFX Library