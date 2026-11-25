// =========================================================================================
// =          ESP8266 智能风扇 & LED & 继电器 & ILI9341 屏幕一体化控制器 v3.4              =
// =========================================================================================
// 项目来源: https://github.com/yyszone/arduino-code/tree/main/esp8266_AuraFan/
//
// 本次仅增加屏幕恢复和 POST 响应优化：
// 1. 增加 /tft-fix，白屏时可通过浏览器重新初始化 ILI9341 并重绘。
// 2. /set、/image 先返回 HTTP 响应，再在 loop 中刷新屏幕，避免脚本等待整屏重绘。
// 3. 每次完整重绘前关闭 ILI9341 显示反转。
// 4. 不修改原有风扇、灯带、继电器控制逻辑及引脚定义。
// =========================================================================================

#define FASTLED_INTERNAL

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <FastLED.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <map>
#include <vector>

#include "RelayManager.h"
#include "IndexPage.h"

// --- 用户配置 ---
const char* ssid = "yang1234";
const char* password = "y123456789";
const char* deviceName = "esp8266-controller";

// --- 引脚定义 ---
const int PWM_PIN = 5;  // D1
const int TACH_PIN = 4; // D2

#define NUM_STRIPS 1
#define LED_PIN    D8
#define TOTAL_LEDS 120

#define TFT_CS   D0
#define TFT_DC   D3
#define TFT_RST  D4

const int PWM_FREQUENCY = 25000;
const int PWM_RESOLUTION = 1023;
const bool PWM_INVERTED = false;
CRGB leds[TOTAL_LEDS > 0 ? TOTAL_LEDS : 1];

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
RelayManager relayManager;

String bootResetReason = "";
bool lastDrawnRelayState = false;

// 屏幕刷新任务：HTTP 先响应，loop 再绘制
bool displayRefreshPending = false;

// 屏幕硬件恢复任务：由 /tft-fix 触发，在 loop 中执行
bool tftRecoveryPending = false;

// 🌟【高亮色相流转状态机】
bool hasOfflineAlert = false;
String offlineIpsText = "";
int16_t offlineIpsX = 0;
int16_t offlineIpsY = 0;
int offlineIpsFontSize = 2;
unsigned long lastBreathUpdate = 0;

// --- AuraFan 全局变量 ---
volatile int pulseCount = 0;
volatile unsigned long lastTachMicros = 0;
unsigned long lastRpmTime = 0;
int fanSliderValue = 0;
enum LedMode { OFF, STATIC, RAINBOW, FADE };
LedMode currentLedMode = RAINBOW;
CRGB staticColor = CRGB(139, 0, 255);
uint8_t gHue = 0;
long lastLedUpdate = 0;
int numLedsToLight = TOTAL_LEDS;

struct LineData { String text; uint16_t color; uint8_t size; String colorHtml; };
std::map<int, LineData> lines;
struct ImageData { std::vector<uint16_t> pixels; int16_t width; int16_t height; int16_t y_offset; };
ImageData image;
bool useGBR = true;

void ICACHE_RAM_ATTR tachISR() {
    unsigned long curMicros = micros();
    if (curMicros - lastTachMicros > 1200) {
        pulseCount++;
        lastTachMicros = curMicros;
    }
}

int computeRPM() {
    if (millis() == lastRpmTime) return 0;
    noInterrupts();
    int pulses = pulseCount;
    pulseCount = 0;
    interrupts();
    unsigned long elapsedTime = millis() - lastRpmTime;
    lastRpmTime = millis();
    return (int)((pulses / 2.0) * 60000.0 / elapsedTime);
}

String getUptime() {
    unsigned long sec = millis() / 1000;
    unsigned long days = sec / 86400;
    sec %= 86400;
    unsigned int hours = sec / 3600;
    sec %= 3600;
    unsigned int mins = sec / 60;
    sec %= 60;

    char buf[48];
    if (days > 0) {
        snprintf(buf, sizeof(buf), "%lud %02u:%02u:%02u", days, hours, mins, (unsigned int)sec);
    } else {
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hours, mins, (unsigned int)sec);
    }
    return String(buf);
}

// 原有灯带和风扇更新逻辑：保持不变
void updateLeds() {
    if (TOTAL_LEDS == 0 || millis() - lastLedUpdate < 20) return;
    lastLedUpdate = millis();

    fill_solid(leds, TOTAL_LEDS, CRGB::Black);

    if (currentLedMode != OFF) {
        if (currentLedMode != FADE) {
            FastLED.setBrightness(255);
        }
        gHue++;

        switch (currentLedMode) {
            case STATIC:
                fill_solid(leds, numLedsToLight, staticColor);
                break;
            case RAINBOW:
                fill_rainbow(leds, numLedsToLight, gHue, 7);
                break;
            case FADE: {
                uint8_t brightness = (exp(sin(millis() / 2000.0 * PI)) - 0.36787944) * 108.0;
                fill_solid(leds, numLedsToLight, staticColor);
                FastLED.setBrightness(brightness);
                break;
            }
            default: break;
        }
    }

    int dutyCycle = map(fanSliderValue, 0, 255, 0, PWM_RESOLUTION);
    if (PWM_INVERTED) dutyCycle = PWM_RESOLUTION - dutyCycle;

    analogWrite(PWM_PIN, PWM_INVERTED ? PWM_RESOLUTION : 0);
    FastLED.show();
    analogWrite(PWM_PIN, dutyCycle);
}

// 屏幕右下角状态角标
void drawRelayBadge(bool state) {
    lastDrawnRelayState = state;
    int16_t bw = 66;
    int16_t bh = 13;
    int16_t bx = tft.width() - bw - 4;
    int16_t by = tft.height() - bh - 3;

    uint16_t bgColor     = state ? 0x0340 : 0x2104;
    uint16_t borderColor = state ? 0x07E0 : 0x7BEF;
    uint16_t textColor   = state ? 0x07E0 : 0xBDF7;

    tft.fillRect(bx, by, bw, bh, bgColor);
    tft.drawRect(bx, by, bw, bh, borderColor);
    tft.setTextSize(1);
    tft.setTextColor(textColor, bgColor);
    tft.setCursor(bx + 6, by + 3);
    tft.print(state ? "RLY: ON" : "RLY: OFF");
}

// 屏幕底栏 (左侧 IP，右侧继电器)
void drawScreenFooter() {
    int16_t footerY = tft.height() - 19;
    tft.drawFastHLine(0, footerY, tft.width(), 0x2124);
    tft.fillRect(0, footerY + 1, tft.width(), 18, 0x0842);

    tft.setTextSize(1);
    tft.setTextColor(0x6BC9, 0x0842);
    tft.setCursor(6, footerY + 5);
    tft.print("IP:");
    tft.setTextColor(0x5EBF, 0x0842);
    tft.print(WiFi.localIP().toString());

    drawRelayBadge(relayManager.getState());
}

void drawScreenHeader() {
    tft.fillRect(0, 0, tft.width(), 19, 0x10A2);
    tft.drawFastHLine(0, 19, tft.width(), 0x05BF);
    tft.setTextSize(1);
    tft.setTextColor(0x07FF, 0x10A2);
    tft.setCursor(6, 6);
    tft.print("AURAFAN MONITOR");
    tft.setTextColor(0x07E0, 0x10A2);
    tft.setCursor(tft.width() - 54, 6);
    tft.print("[ONLINE]");
}

int parsePercentage(const String& str) {
    int idx = str.indexOf('%');
    if (idx <= 0) return -1;
    int start = idx - 1;
    while (start >= 0 && isDigit(str.charAt(start))) {
        start--;
    }
    String numStr = str.substring(start + 1, idx);
    if (numStr.length() > 0) {
        int v = numStr.toInt();
        if (v >= 0 && v <= 100) return v;
    }
    return -1;
}

// 提取离线 IP 尾段字符串，仅保留 "(120) (125)"
String extractOfflineTails(const String& raw) {
    int colonIdx = raw.indexOf(':');
    String ipListStr = (colonIdx != -1) ? raw.substring(colonIdx + 1) : raw;
    ipListStr.trim();
    if (ipListStr.length() == 0) return "";

    String result = "";
    int start = 0;
    int count = 0;

    while (start < (int)ipListStr.length()) {
        int pipeIdx = ipListStr.indexOf('|', start);
        String item = (pipeIdx == -1) ? ipListStr.substring(start) : ipListStr.substring(start, pipeIdx);
        item.trim();

        if (item.length() > 0) {
            int dotIdx = item.lastIndexOf('.');
            String lastOctet = (dotIdx != -1) ? item.substring(dotIdx + 1) : item;
            lastOctet.trim();
            if (lastOctet.length() > 0) {
                if (count > 0) result += " ";
                result += "(" + lastOctet + ")";
                count++;
            }
        }

        if (pipeIdx == -1) break;
        start = pipeIdx + 1;
    }
    return result;
}

// =========================================================================
// 高亮色相流转引擎：R=255，在赤红与琥珀金之间平滑流转
// =========================================================================
uint16_t getBreathColor(float progress) {
    uint8_t r = 255;
    uint8_t g = 20 + (uint8_t)(185 * progress);
    uint8_t b = (uint8_t)(15 * (1.0 - progress));

    if (useGBR) {
        return ((g & 0xF8) << 8) | ((b & 0xFC) << 3) | (r >> 3);
    } else {
        return tft.color565(r, g, b);
    }
}

// 高透色彩转换
uint16_t parseColor(String colorString) {
    uint8_t r = 255, g = 255, b = 255;

    if (colorString.equalsIgnoreCase("#0000FF") || colorString.equalsIgnoreCase("blue")) {
        r = 60; g = 175; b = 255;
    } else if (colorString.equalsIgnoreCase("#FF0000") || colorString.equalsIgnoreCase("red")) {
        r = 255; g = 80; b = 80;
    } else if (colorString.equalsIgnoreCase("#00FF00") || colorString.equalsIgnoreCase("green")) {
        r = 52; g = 211; b = 153;
    } else if (colorString.equalsIgnoreCase("#FFFF00") || colorString.equalsIgnoreCase("yellow")) {
        r = 251; g = 191; b = 36;
    } else if (colorString.equalsIgnoreCase("#FF00FF") || colorString.equalsIgnoreCase("magenta")) {
        r = 244; g = 114; b = 182;
    } else if (colorString.equalsIgnoreCase("#432f40")) {
        r = 251; g = 146; b = 60;
    } else if (colorString.startsWith("#") && colorString.length() == 7) {
        long number = strtol(colorString.substring(1).c_str(), NULL, 16);
        r = (number >> 16) & 0xFF;
        g = (number >> 8) & 0xFF;
        b = number & 0xFF;
    }

    if (useGBR) {
        return ((g & 0xF8) << 8) | ((b & 0xFC) << 3) | (r >> 3);
    } else {
        return tft.color565(r, g, b);
    }
}

// 现代化双卡片排版
void updateDisplay() {
    // 完整重绘前确保关闭 ILI9341 显示反转
    tft.invertDisplay(false);
    tft.fillScreen(ILI9341_BLACK);
    drawScreenHeader();

    bool isStandardDashboard = (lines.count(4) || lines.count(5) || lines.count(6));

    if (isStandardDashboard) {
        // 卡片 1: 节点与遥测区
        tft.drawRoundRect(4, 23, tft.width() - 8, 102, 4, 0x18E3);
        int16_t yPos = 28;
        for (int i = 0; i <= 3; i++) {
            if (lines.count(i)) {
                tft.setCursor(10, yPos);
                tft.setTextColor(lines[i].color, ILI9341_BLACK);
                tft.setTextSize(2);
                tft.println(lines[i].text);
            }
            yPos += 23;
        }

        // 卡片 2: 系统硬件资源区
        tft.drawRoundRect(4, 131, tft.width() - 8, 164, 4, 0x18E3);
        yPos = 139;

        for (int i = 4; i <= 7; i++) {
            if (!lines.count(i)) continue;

            // OFF: 前缀常亮，右侧括号内容进行金-红流光渐变
            if (lines[i].text.startsWith("OFF:")) {
                int totalLen = lines[i].text.length();
                int fontSize = (totalLen > 16) ? 1 : 2;
                offlineIpsFontSize = fontSize;

                int16_t tagY = (fontSize == 1) ? (yPos + 6) : yPos;
                tft.setTextSize(fontSize);

                tft.setCursor(12, tagY);
                tft.setTextColor(0xAEDC, ILI9341_BLACK);
                tft.print("OFF: ");

                offlineIpsX = tft.getCursorX();
                offlineIpsY = tft.getCursorY();

                tft.setTextColor(getBreathColor(1.0), ILI9341_BLACK);
                tft.print(offlineIpsText);

                yPos += 22;
                continue;
            }

            tft.setCursor(12, yPos);
            tft.setTextColor(lines[i].color, ILI9341_BLACK);
            tft.setTextSize(2);
            tft.print(lines[i].text);

            int pct = parsePercentage(lines[i].text);
            if (pct >= 0) {
                int16_t barX = 12;
                int16_t barY = yPos + 19;
                int16_t barW = tft.width() - 32;
                int16_t barH = 5;
                tft.fillRoundRect(barX, barY, barW, barH, 2, 0x18E3);

                int16_t fillW = (barW * pct) / 100;
                if (fillW > 0) {
                    tft.fillRoundRect(barX, barY, fillW, barH, 2, lines[i].color);
                }
                yPos += 38;
            } else {
                yPos += 24;
            }
        }
    } else {
        int16_t yPos = 26;
        int16_t maxY = tft.height() - 24;

        for (auto& [lineNum, line] : lines) {
            if (yPos >= maxY) break;
            tft.setCursor(10, yPos);
            tft.setTextColor(line.color, ILI9341_BLACK);
            tft.setTextSize(line.size);
            tft.println(line.text);
            yPos += line.size * 8 + 8;
        }
    }

    drawScreenFooter();
}

// 重新初始化 ILI9341：白屏时由 /tft-fix 调用
void initTFT() {
    tft.begin();
    delay(50);
    SPI.setFrequency(1000000);
    tft.setRotation(0);

    uint8_t caset_data[] = {0x00, 0x00, 0x00, 0xEF};
    tft.sendCommand(ILI9341_CASET, caset_data, 4);

    uint8_t raset_data[] = {0x00, 0x00, 0x01, 0x3F};
    tft.sendCommand(ILI9341_PASET, raset_data, 4);

    tft.writeCommand(ILI9341_RAMWR);

    uint8_t madctl_data = 0xE8;
    tft.sendCommand(ILI9341_MADCTL, &madctl_data, 1);

    tft.invertDisplay(false);
    tft.fillScreen(ILI9341_BLACK);
}

void handleRoot() {
    server.send_P(200, "text/html; charset=UTF-8", MAIN_PAGE_HTML);
}

void handleSetSpeed() {
    if (server.hasArg("value")) {
        fanSliderValue = constrain(server.arg("value").toInt(), 0, 255);
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

void handleGetRPM() {
    server.send(200, "text/plain", String(computeRPM()));
}

void handleGetUptime() {
    server.send(200, "text/plain", getUptime());
}

void handleSetRGB() {
    if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
        staticColor = CRGB(server.arg("r").toInt(), server.arg("g").toInt(), server.arg("b").toInt());
        currentLedMode = STATIC;
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

void handleSetActiveLeds() {
    if (server.hasArg("value")) {
        numLedsToLight = server.arg("value").toInt();
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

void handleSetLedMode() {
    if (server.hasArg("mode")) {
        String mode = server.arg("mode");
        if (mode == "off") currentLedMode = OFF;
        else if (mode == "static") currentLedMode = STATIC;
        else if (mode == "rainbow") currentLedMode = RAINBOW;
        else if (mode == "fade") currentLedMode = FADE;
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

void handleGetState() {
    String modeStr = "off";
    if (currentLedMode == STATIC) modeStr = "static";
    else if (currentLedMode == RAINBOW) modeStr = "rainbow";
    else if (currentLedMode == FADE) modeStr = "fade";

    String json = "{";
    json += "\"fanSpeed\":" + String(fanSliderValue) + ",";
    json += "\"rpm\":" + String(computeRPM()) + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"uptime\":\"" + getUptime() + "\",";
    json += "\"resetReason\":\"" + bootResetReason + "\",";
    json += "\"relayState\":" + String(relayManager.getState() ? "true" : "false") + ",";
    json += "\"ledMode\":\"" + modeStr + "\",";
    json += "\"ledR\":" + String(staticColor.r) + ",";
    json += "\"ledG\":" + String(staticColor.g) + ",";
    json += "\"ledB\":" + String(staticColor.b) + ",";
    json += "\"activeLeds\":" + String(numLedsToLight) + ",";
    json += "\"maxLeds\":" + String(TOTAL_LEDS) + ",";
    json += "\"lines\":[";
    bool first = true;
    for (auto const& [key, val] : lines) {
        if (!first) json += ",";
        first = false;
        json += "{\"text\":\"" + val.text + "\",\"color\":\"" + val.colorHtml + "\",\"size\":" + String(val.size) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleSetText() {
    lines.clear();
    hasOfflineAlert = false;
    offlineIpsText = "";

    for (int i = 0; i < 100; i++) {
        String lineKey = "line" + String(i);
        if (server.hasArg(lineKey)) {
            LineData line;
            line.text = server.arg(lineKey);
            String colorKey = "color" + String(i);
            line.colorHtml = server.hasArg(colorKey) ? server.arg(colorKey) : "#FFFFFF";

            if (line.text.startsWith("offline:") || line.text.startsWith("offline")) {
                offlineIpsText = extractOfflineTails(line.text);
                if (offlineIpsText.length() > 0) {
                    hasOfflineAlert = true;
                    line.text = "OFF: " + offlineIpsText;
                } else {
                    line.text = "";
                }
                line.colorHtml = "#fb923c";
            }

            line.color = parseColor(line.colorHtml);
            int size = server.hasArg("size" + String(i)) ? server.arg("size" + String(i)).toInt() : 2;
            line.size = (size >= 1 && size <= 7) ? size : 2;
            lines[i] = line;
        }
    }

    // 先结束 HTTP 响应；屏幕刷新放到 loop，避免 POST 等待整屏重绘
    displayRefreshPending = true;
    server.send(200, "text/plain", "OK");
    Serial.printf(
        "[POST /set] OK | args=%d | heap=%u | maxBlock=%u\n",
        server.args(),
        ESP.getFreeHeap(),
        ESP.getMaxFreeBlockSize()
    );
}

void handleImage() {
    if (server.hasArg("width") && server.hasArg("height") && server.hasArg("data")) {
        int width = server.arg("width").toInt();
        int height = server.arg("height").toInt();
        String data = server.arg("data");
        if (width > 0 && height > 0 && data.length() > 0) {
            image.width = width;
            image.height = height;
            image.pixels.clear();
            image.pixels.reserve(width * height);
            for (unsigned int i = 0; i + 4 <= data.length(); i += 4) {
                String hexColor = data.substring(i, i + 4);
                image.pixels.push_back(strtoul(hexColor.c_str(), NULL, 16));
            }
            image.y_offset = server.hasArg("y_offset") ? server.arg("y_offset").toInt() : 0;

            // 保留原有行为：请求成功后刷新现有仪表盘
            displayRefreshPending = true;
            server.send(200, "text/plain", "Image OK");
        } else {
            server.send(400, "text/plain", "Invalid image parameters");
        }
    } else {
        server.send(400, "text/plain", "Image parameters missing");
    }
}

// 白屏恢复接口：重新初始化 TFT，并重新绘制当前数据
void handleTftFix() {
    tftRecoveryPending = true;
    server.send(200, "text/plain", "TFT recovery scheduled");
}

void handleNotFound() {
    server.send(404, "text/plain", "404: Not Found");
}

void setup() {
    // 1. 上电最优先：立刻拉低灯带引脚，锁定继电器关闭电平
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    digitalWrite(RELAY_PIN, !RELAY_ON_LEVEL);
    pinMode(RELAY_PIN, OUTPUT);

    // 2. 冷启动等待 500ms，让电源稳定
    delay(500);

    Serial.begin(115200);
    delay(100);
    bootResetReason = ESP.getResetReason();

    Serial.println();
    Serial.println("========== ESP8266 RESET INFO ==========");
    Serial.print("Reset Reason: ");
    Serial.println(bootResetReason);
    Serial.println(ESP.getResetInfo());
    Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Max Free Block: %u bytes\n", ESP.getMaxFreeBlockSize());
    Serial.printf("Heap Fragmentation: %u%%\n", ESP.getHeapFragmentation());
    Serial.println("========================================");

    // 3. 初始化风扇 PWM 与测速引脚（原样保留）
    pinMode(PWM_PIN, OUTPUT);
    analogWriteFreq(PWM_FREQUENCY);
    analogWrite(PWM_PIN, PWM_INVERTED ? PWM_RESOLUTION : 0);
    pinMode(TACH_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachISR, FALLING);
    lastRpmTime = millis();

    // 4. 尽早清空灯带（原样保留）
    if (TOTAL_LEDS > 0) {
        FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, TOTAL_LEDS);
        FastLED.setBrightness(50);
        FastLED.clear(true);
    }

    // 5. 初始化 ILI9341
    initTFT();

    drawScreenHeader();
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 35);
    tft.println("Booting...");

    // 6. Wi-Fi 连接
    tft.setCursor(10, 65);
    tft.setTextSize(1);
    tft.print("Connecting WiFi...");

    WiFi.persistent(false);
    WiFi.hostname(deviceName);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int wifiRetry = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(400);
        Serial.print(".");
        wifiRetry++;
        if (wifiRetry > 40) {
            ESP.restart();
        }
        yield();
    }

    Serial.println();
    Serial.print("WiFi IP: ");
    Serial.println(WiFi.localIP());

    relayManager.begin(&server);
    updateDisplay();

    if (MDNS.begin(deviceName)) {
        MDNS.addService("http", "tcp", 80);
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/setSpeed", HTTP_GET, handleSetSpeed);
    server.on("/getRPM", HTTP_GET, handleGetRPM);
    server.on("/getUptime", HTTP_GET, handleGetUptime);
    server.on("/setRGB", HTTP_GET, handleSetRGB);
    server.on("/setLedMode", HTTP_GET, handleSetLedMode);
    server.on("/setActiveLeds", HTTP_GET, handleSetActiveLeds);
    server.on("/getState", HTTP_GET, handleGetState);
    server.on("/set", HTTP_POST, handleSetText);
    server.on("/image", HTTP_POST, handleImage);
    server.on("/tft-fix", HTTP_GET, handleTftFix);
    server.onNotFound(handleNotFound);

    httpUpdater.setup(&server);
    server.begin();

    Serial.println("[HTTP] Server started");
    Serial.println("[TFT] Recovery endpoint: /tft-fix");
}

void loop() {
    server.handleClient();

    // 收到恢复请求：重新初始化屏幕控制器，再绘制原有页面
    if (tftRecoveryPending) {
        tftRecoveryPending = false;
        displayRefreshPending = false;
        Serial.println("[TFT] Recovery started");
        initTFT();
        updateDisplay();
        Serial.println("[TFT] Recovery completed");
    }
    // 普通 POST：HTTP 已先返回，再执行屏幕刷新
    else if (displayRefreshPending) {
        displayRefreshPending = false;
        updateDisplay();
    }

    MDNS.update();
    updateLeds();
    relayManager.update();

    bool currentRelayState = relayManager.getState();
    if (currentRelayState != lastDrawnRelayState) {
        drawRelayBadge(currentRelayState);
    }

    // 高亮色相流转：周期 2.2 秒，每 50ms 原位更新
    if (hasOfflineAlert && offlineIpsText.length() > 0 && millis() - lastBreathUpdate >= 50) {
        lastBreathUpdate = millis();
        float progress = (sin(millis() / 2200.0 * 2.0 * PI) + 1.0) / 2.0;
        uint16_t breathCol = getBreathColor(progress);

        tft.setTextSize(offlineIpsFontSize);
        tft.setCursor(offlineIpsX, offlineIpsY);
        tft.setTextColor(breathCol, ILI9341_BLACK);
        tft.print(offlineIpsText);
    }
}
