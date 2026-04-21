#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Wire.h>
#include <RtcDS3231.h>

// ===== WiFi =====
const char* ssid     = "ESP32_WIFI";
const char* password = "12345678";
WebServer server(80);

// ===== DS3231 RTC ===== (SDA=GPIO21, SCL=GPIO22)
RtcDS3231<TwoWire> Rtc(Wire);

String getRtcTimestamp() {
  if (!Rtc.IsDateTimeValid()) return "N/A";
  RtcDateTime now = Rtc.GetDateTime();
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
    now.Year(), now.Month(), now.Day(), now.Hour(), now.Minute());
  return String(buf);
}

// Lưu lịch sử bơm — tối đa 3 lần gần nhất mỗi biển số
void saveHistory(const String& plate, float psi) {
  DynamicJsonDocument doc(65536);
  File f = SPIFFS.open("/history.json", FILE_READ);
  if (f && f.size() > 0) { deserializeJson(doc, f); f.close(); }
  else { if (f) f.close(); doc.set(JsonObject()); }

  if (!doc.containsKey(plate)) doc.createNestedArray(plate);
  JsonArray arr = doc[plate].as<JsonArray>();

  JsonObject entry = arr.createNestedObject();
  entry["time"] = getRtcTimestamp();
  entry["psi"]  = (int)psi;

  // Chỉ giữ 3 cái cuối
  while ((int)arr.size() > 3) arr.remove(0);

  File fw = SPIFFS.open("/history.json", FILE_WRITE);
  if (fw) { serializeJson(doc, fw); fw.close(); }
}

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
#define C_BG       0x0000
#define C_RED      0xF800
#define C_GREEN    0x07E0
#define C_YELLOW   0xFFE0
#define C_WHITE    0xFFFF
#define C_GRAY     0x7BEF
#define C_DARKGRAY 0x39E7
#define C_HEADER   0xA000

// ===== Trạng thái bơm =====
bool  pumpRunning    = false;
float targetPressure = 0.0;
float currentPressure = 0.0;
float lastDisplayedPressure = -1.0;

// ===== Menu =====
enum Screen { SCR_MAIN, SCR_BRAND, SCR_MODEL, SCR_PUMP, SCR_DONE };
Screen currentScreen = SCR_MAIN;

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

int    menuIndex  = 0;
int    menuScroll = 0;

unsigned long lastBtnTime = 0;
#define DEBOUNCE_MS 200

unsigned long lastPressureRead = 0;

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
void drawHeader(const char* title) {
  tft.fillRect(0, 0, 128, 18, C_HEADER);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 5);
  tft.print(title);
  tft.setCursor(80, 5);
  tft.printf("%.1fPSI", currentPressure);
}

void drawFooter() {
  tft.fillRect(0, 150, 128, 10, C_DARKGRAY);
  tft.setTextColor(C_GRAY);
  tft.setTextSize(1);
  tft.setCursor(2, 152);
  tft.print("UP=back DN SEL");
}

void drawMain() {
  tft.fillScreen(C_BG);
  drawHeader("PUMP SYSTEM");
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
  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 110);
  tft.print("SEL: Chon xe & bom");
  tft.setCursor(4, 122);
  tft.print("UP/DN: Xem ap suat");
  drawFooter();
}

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
  if (brandCount > visibleRows) {
    tft.setTextColor(C_GRAY);
    tft.setCursor(112, 22);
    tft.print(menuScroll > 0 ? "^" : " ");
    tft.setCursor(112, 140);
    tft.print((menuScroll + visibleRows) < brandCount ? "v" : " ");
  }
  drawFooter();
}

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

  if (menuIndex == 0) tft.fillRect(0, 62, 128, 18, C_RED);
  else tft.fillRect(0, 62, 128, 18, C_DARKGRAY);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(6, 67);
  tft.printf("Bom de xuat: %d PSI", suggestedPsi);

  if (menuIndex == 1) tft.fillRect(0, 84, 128, 18, C_RED);
  else tft.fillRect(0, 84, 128, 18, C_DARKGRAY);
  tft.setTextColor(C_WHITE);
  tft.setCursor(6, 89);
  tft.printf("Tuy chinh: %d PSI", customPsi);

  int maxPsi = (int)(suggestedPsi * 1.05);
  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 108);
  tft.printf("Min:20 Max:%dPSI", maxPsi);

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
  tft.fillRect(4, 75, 120, 10, C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  tft.setCursor(4, 75);
  tft.printf("Hien tai: %.1f PSI", currentPressure);
  int pct = 0;
  if (targetPressure > 0)
    pct = constrain((int)((currentPressure / targetPressure) * 100), 0, 100);
  tft.drawRect(4, 92, 120, 12, C_GRAY);
  tft.fillRect(4, 92, (int)(120 * pct / 100.0), 12, C_GREEN);
}

void startPumping() {
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
  tft.setTextColor(C_GRAY);
  tft.setCursor(4, 110);
  tft.print("SEL: Huy bom");
  drawPumping();
}

// ============================================================
void handleButtons() {
  if (millis() - lastBtnTime < DEBOUNCE_MS) return;

  bool up  = digitalRead(BTN_UP)   == LOW;
  bool dn  = digitalRead(BTN_DOWN) == LOW;
  bool sel = digitalRead(BTN_SEL)  == LOW;

  if (!up && !dn && !sel) return;
  lastBtnTime = millis();

  if (currentScreen == SCR_DONE) {
    if (sel || up) {
      currentScreen = SCR_MAIN;
      lastDisplayedPressure = -1.0;
      drawMain();
    }
    return;
  }

  if (currentScreen == SCR_MAIN) {
    if (sel) {
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
    } else if (up && menuIndex == 0) {
      currentScreen = SCR_MAIN;
      drawMain();
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
    } else if (up && menuIndex == 0) {
      menuIndex  = 0;
      menuScroll = 0;
      currentScreen = SCR_BRAND;
      drawBrandScreen();
    }

  } else if (currentScreen == SCR_PUMP) {
    int maxPsi = (int)(suggestedPsi * 1.05);

    if (menuIndex == 0) {
      if (up) {
        menuIndex  = 0;
        menuScroll = 0;
        currentScreen = SCR_MODEL;
        drawModelScreen();
      } else if (dn) {
        menuIndex = 1;
        drawPumpScreen();
      } else if (sel) {
        targetPressure = suggestedPsi;
        pumpRunning    = true;
        digitalWrite(PUMP_RELAY_PIN, HIGH);
        startPumping();
      }
    } else {
      if (up && customPsi < maxPsi) {
        customPsi++;
        drawPumpScreen();
      } else if (dn && customPsi > 20) {
        customPsi--;
        drawPumpScreen();
      } else if (dn && customPsi <= 20) {
        menuIndex = 0;
        drawPumpScreen();
      } else if (sel) {
        targetPressure = customPsi;
        pumpRunning    = true;
        digitalWrite(PUMP_RELAY_PIN, HIGH);
        startPumping();
      }
    }
  }
}

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

  // DS3231
  Wire.begin(21, 22);
  Rtc.Begin();
  if (!Rtc.IsDateTimeValid()) {
    Rtc.SetDateTime(RtcDateTime(__DATE__, __TIME__));
  }
  if (!Rtc.GetIsRunning()) Rtc.SetIsRunning(true);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);

  if (!SPIFFS.begin(true)) {
    tft.setTextColor(C_RED);
    tft.setCursor(4, 4);
    tft.print("SPIFFS LOI!");
    return;
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
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

  // ── Lưu lịch sử bơm ──
  // GET /history/save?plate=51A12345&psi=32
  server.on("/history/save", HTTP_GET, []() {
    String plate = server.arg("plate");
    String psiStr = server.arg("psi");
    plate.trim();
    if (plate.length() == 0 || psiStr.length() == 0) {
      server.send(400, "text/plain", "missing params");
      return;
    }
    saveHistory(plate, psiStr.toFloat());
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "OK");
  });

  // ── Tra cứu lịch sử theo biển số ──
  // GET /history/get?plate=51A12345
  server.on("/history/get", HTTP_GET, []() {
    String plate = server.arg("plate");
    plate.trim();
    if (plate.length() == 0) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "application/json", "[]");
      return;
    }

    DynamicJsonDocument doc(65536);
    File f = SPIFFS.open("/history.json", FILE_READ);
    if (f && f.size() > 0) { deserializeJson(doc, f); f.close(); }
    else { if (f) f.close(); }

   String result;
     if (doc.containsKey(plate)) serializeJson(doc[plate], result);
     else result = "[]";  

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", result);
  });
    server.on("/settime", HTTP_GET, []() {
    String y = server.arg("y"), mo = server.arg("mo"), d = server.arg("d");
    String h = server.arg("h"), mi = server.arg("mi"), s = server.arg("s");
    if (y.length() && mo.length() && d.length()) {
        RtcDateTime dt(y.toInt(), mo.toInt(), d.toInt(),
                       h.toInt(), mi.toInt(), s.toInt());
        Rtc.SetDateTime(dt);
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "missing params");
    }
});
  server.begin();

  currentPressure = readPressurePSI();
  drawMain();
}

// ============================================================
void loop() {
  server.handleClient();
  handleButtons();

  if (millis() - lastPressureRead >= 300) {
    lastPressureRead = millis();
    currentPressure  = readPressurePSI();

    if (pumpRunning && targetPressure > 0) {
      if (currentPressure >= targetPressure) {
        pumpRunning    = false;
        targetPressure = 0.0;
        digitalWrite(PUMP_RELAY_PIN, LOW);
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
        currentScreen = SCR_DONE;
      }
      drawPumping();
    }

    if (currentScreen == SCR_MAIN) {
      if (abs(currentPressure - lastDisplayedPressure) >= 0.5) {
        lastDisplayedPressure = currentPressure;
        drawMain();
      }
    }
  }

  if (currentScreen == SCR_DONE && digitalRead(BTN_SEL) == LOW &&
      millis() - lastBtnTime > DEBOUNCE_MS) {
    lastBtnTime   = millis();
    currentScreen = SCR_MAIN;
    lastDisplayedPressure = -1.0;
    drawMain();
  }
}