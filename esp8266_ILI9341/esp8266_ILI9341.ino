#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
extern "C" {
#include "user_interface.h"
}

// ===============================================================
// ================ ESP8266 智能控制器 v4.5 ======================
// ============= (日志修复 & 深度功耗优化) =================
// ===============================================================

// ===============================================================
// ==================== 用户配置区域 =============================
// ===============================================================
const char* ssid = "yang1234";
const char* password = "y123456789";
unsigned long standbyDelay = 60000;
const char* ha_host = "192.168.31.22";
const int ha_port = 8123;
const char* ha_token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiIwYjU4YTMwOWMzNmE0ZDE2ODBjOGI2MzI4YzAwMTlkZCIsImlhdCI6MTc1ODk3NDgwMCwiZXhwIjoyMDc0MzM0ODAwfQ.e1e_iE6iIpdB2EG0d0VXZcb5bjePSoI8m8qTDEFTJ-w";
const char* ha_entity_id = "switch.sonoff_1000a68f48";
const char* led_on_url = "http://192.168.31.162/LED-Control?ledPwm=3";
const char* led_off_url = "http://192.168.31.162/LED-Control?ledPwm=4";

#define TFT_CS   15 // D8
#define TFT_DC   5  // D1
#define TFT_RST  4  // D2
#define TFT_LED  16 // D0
#define T_CS     0  // D3

// ============== 日志系统配置 ==============
const int MAX_LOG_ENTRIES = 30;
struct LogEntry { String timestamp; String message; };
LogEntry logBuffer[MAX_LOG_ENTRIES];
int currentLogIndex = 0;
bool logBufferFull = false;

// ===============================================================
// ==================== 网页界面 (美化中文版) =======================
// ===============================================================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP8266 控制面板</title><style>:root { --bg-color: #121826; --card-color: #1F2937; --text-color: #E5E7EB; --accent-color: #22C55E; --muted-color: #9CA3AF; } body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; background-color: var(--bg-color); color: var(--text-color); display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; } .container { width: 90%; max-width: 500px; background-color: var(--card-color); padding: 2rem; border-radius: 16px; box-shadow: 0 10px 30px rgba(0, 0, 0, 0.3); text-align: center; } h1 { color: var(--accent-color); margin-top: 0; margin-bottom: 1rem; } p { color: var(--muted-color); margin-bottom: 2rem; } .button-group { display: flex; flex-direction: column; gap: 1rem; } .btn { display: inline-block; text-decoration: none; padding: 14px 28px; font-size: 16px; font-weight: bold; border-radius: 12px; border: none; background-color: var(--accent-color); color: #14532D; cursor: pointer; transition: transform 0.2s ease, background-color 0.2s ease; } .btn:hover { transform: scale(1.05); background-color: #2fed78; }</style></head><body><div class="container"><h1>✅ ESP8266 触屏控制器</h1><p>设备运行正常，等待指令。</p><div class="button-group"><a href="/update" class="btn">固件更新 🚀</a><a href="/logs" class="btn">查看事件日志 📋</a></div></div></body></html>
)HTML";

// ===============================================================
// ==================== 代码实现区域 =============================
// ===============================================================
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);

bool haDeviceState = false;
bool httpDeviceState = false;
bool isInStandby = false;
unsigned long lastActivityTime = 0;

void setup() {
  Serial.begin(115200);
  pinMode(TFT_LED, OUTPUT);
  tft.begin();
  ts.begin();
  tft.setRotation(0);
  exitStandby();
  addLog("✅ 系统启动 (高可靠性待机模式 v4.5)。");
}

void loop() {
  handleTouch();
  if (!isInStandby) {
    server.handleClient();
    ArduinoOTA.handle();
    if (millis() - lastActivityTime > standbyDelay) {
      enterStandby();
    }
  } else {
    delay(20);
  }
}

void enterStandby() {
  addLog("💤 无操作超时，进入软件待机模式。");
  isInStandby = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  tft.writeCommand(ILI9341_SLPIN); // 【核心修正】让屏幕控制器进入睡眠
  analogWrite(TFT_LED, 0);       // 关闭背光

  system_update_cpu_freq(80);
  Serial.println("CPU frequency set to 80MHz.");
  addLog("  - WiFi, Backlight & Display OFF, CPU @ 80MHz.");
}

void exitStandby() {
  addLog("☀️ 触摸唤醒，退出待机模式。");
  
  system_update_cpu_freq(160);
  Serial.println("CPU frequency set to 160MHz.");
  
  tft.writeCommand(ILI9341_SLPOUT); // 【核心修正】唤醒屏幕控制器
  delay(120);                      // 【核心修正】等待屏幕控制器稳定
  analogWrite(TFT_LED, 1023);      // 点亮背光
  
  isInStandby = false;
  lastActivityTime = millis(); 
  setupWifiAndServices();
  drawButtons();
}

void setupWifiAndServices() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(20, 150);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.print(F("Connecting WiFi..."));
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(F("."));
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    addLog("📡 WiFi 已连接, IP: " + WiFi.localIP().toString());
    timeClient.begin();
    timeClient.forceUpdate();
    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=UTF-8", MAIN_HTML); });
    server.on("/logs", HTTP_GET, handleLogs);
    httpUpdater.setup(&server);
    server.begin();
    ArduinoOTA.begin();
  } else {
    addLog("❌ WiFi 连接失败。");
  }
}

void handleTouch() {
  if (ts.touched()) {
    static unsigned long lastTouchDebounce = 0;
    if (millis() - lastTouchDebounce < 500) return;
    lastTouchDebounce = millis();
    
    if (isInStandby) {
      exitStandby();
      return;
    }

    lastActivityTime = millis();
    TS_Point p = ts.getPoint();
    int screen_x = map(p.y, 295, 3750, 0, 240); 
    int screen_y = map(p.x, 358, 3810, 0, 320); 
    addLog("👆 触摸. 原始:(" + String(p.x) + "," + String(p.y) + ") 映射:(" + String(screen_x) + "," + String(screen_y) + ")");
    if (screen_x > 40 && screen_x < 200 && screen_y > 50 && screen_y < 150) {
      haDeviceState = !haDeviceState;
      addLog("▶️ HA 按钮按下. 新状态: " + String(haDeviceState ? "ON" : "OFF"));
      controlHA(haDeviceState);
      drawButtons();
    }
    else if (screen_x > 40 && screen_x < 200 && screen_y > 190 && screen_y < 290) {
      httpDeviceState = !httpDeviceState;
      addLog("▶️ HTTP 按钮按下. 新状态: " + String(httpDeviceState ? "ON" : "OFF"));
      controlHttp(httpDeviceState);
      drawButtons();
    }
  }
}

void handleLogs() {
  String html = "<!DOCTYPE html><html lang='zh-CN'><head><title>事件日志</title><meta http-equiv='refresh' content='10'><style>body{font-family: monospace; background-color: #121826; color: #E5E7EB; padding: 1rem;} h1{color:#22C55E;} table{width: 100%; border-collapse: collapse; margin-top: 1rem;} th, td{border: 1px solid #374151; padding: 10px; text-align: left;} th{background-color: #1F2937;} a{color: #22C55E; text-decoration:none;}</style></head><body><h1>📋 设备事件日志</h1><p><a href='/'>&larr; 返回主页</a></p><table><tr><th>时间戳</th><th>事件</th></tr>";
  int count = logBufferFull ? MAX_LOG_ENTRIES : currentLogIndex;
  for (int i = 0; i < count; i++) {
    int index = (currentLogIndex - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    if (logBuffer[index].message.isEmpty()) continue;
    html += "<tr><td>" + logBuffer[index].timestamp + "</td><td>" + logBuffer[index].message + "</td></tr>";
  }
  html += "</table></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

void addLog(String message) {
  timeClient.update();
  unsigned long epochTime = timeClient.getEpochTime();
  time_t rawTime = epochTime;
  struct tm * timeinfo = localtime(&rawTime);
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
  String timestamp(buffer);
  logBuffer[currentLogIndex].timestamp = timestamp;
  logBuffer[currentLogIndex].message = message;
  Serial.println("[LOG] " + timestamp + " - " + message);
  currentLogIndex = (currentLogIndex + 1) % MAX_LOG_ENTRIES;
  if (currentLogIndex == 0) logBufferFull = true;
}

void drawButtons() {
  if (isInStandby) return;
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(40, 30);
  tft.print(F("Home Assistant"));
  tft.drawRoundRect(40, 50, 160, 100, 10, ILI9341_WHITE);
  if (haDeviceState) {
    tft.fillRoundRect(50, 60, 140, 80, 10, ILI9341_GREEN);
    tft.setTextColor(ILI9341_BLACK); tft.setTextSize(3);
    tft.setCursor(95, 90); tft.print(F("ON"));
  } else {
    tft.fillRoundRect(50, 60, 140, 80, 10, ILI9341_RED);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3);
    tft.setCursor(85, 90); tft.print(F("OFF"));
  }
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(50, 170);
  tft.print(F("HTTP Control"));
  tft.drawRoundRect(40, 190, 160, 100, 10, ILI9341_WHITE);
  if (httpDeviceState) {
    tft.fillRoundRect(50, 200, 140, 80, 10, ILI9341_GREEN);
    tft.setTextColor(ILI9341_BLACK); tft.setTextSize(3);
    tft.setCursor(95, 230); tft.print(F("ON"));
  } else {
    tft.fillRoundRect(50, 200, 140, 80, 10, ILI9341_RED);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3);
    tft.setCursor(85, 230); tft.print(F("OFF"));
  }
  drawStatus();
}

void drawStatus() {
  tft.fillRect(0, 300, 240, 20, ILI9341_BLACK);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(10, 300);
  if (WiFi.status() == WL_CONNECTED) {
      tft.print(F("IP: "));
      tft.print(WiFi.localIP());
      tft.drawLine(210, 305, 215, 310, ILI9341_GREEN);
      tft.drawLine(215, 310, 225, 300, ILI9341_GREEN);
  } else {
      tft.print(F("WiFi Disconnected"));
  }
}

void controlHttp(bool state) {
  if (isInStandby) return;
  addLog("  📤 发送 HTTP 请求. 状态: " + String(state ? "ON" : "OFF"));
  WiFiClient client;
  HTTPClient http;
  const char* url = state ? led_on_url : led_off_url;
  if (http.begin(client, url)) { http.GET(); http.end(); }
}

void controlHA(bool state) {
  if (isInStandby) return;
  addLog("  📤 发送 HA 请求. 状态: " + String(state ? "ON" : "OFF"));
  WiFiClient client;
  HTTPClient http;
  String action = state ? "turn_on" : "turn_off";
  String url_path = "/api/services/switch/" + action;
  if(http.begin(client, ha_host, ha_port, url_path)) {
    http.addHeader(F("Authorization"), "Bearer " + String(ha_token));
    http.addHeader(F("Content-Type"), F("application/json"));
    StaticJsonDocument<200> doc;
    doc["entity_id"] = ha_entity_id;
    String requestBody;
    serializeJson(doc, requestBody);
    http.POST(requestBody);
    http.end();
  }
}