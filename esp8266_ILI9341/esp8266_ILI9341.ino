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
#include <EEPROM.h>

extern "C" {
#include "user_interface.h"
}

// ===============================================================
// ================ ESP8266 智能控制器 v4.9 ======================
// ========= (精确分钟定时 & 界面精简 & 逻辑修复) ==========
// ===============================================================

// ===============================================================
// ==================== 用户配置区域 =============================
// ===============================================================
const char* ssid = "yang1234";
const char* password = "y123456789";
unsigned long standbyDelay = 60000; // 60秒无操作进入待机 (仅在睡眠时段生效)
const char* ha_host = "192.168.31.22";
const int ha_port = 8123;
const char* ha_token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiIwYjU4YTMwOWMzNmE0ZDE2ODBjOGI2MzI4YzAwMTlkZCIsImlhdCI6MTc1ODk3NDgwMCwiZXhwIjoyMDc0MzM0ODAwfQ.e1e_iE6iIpdB2EG0d0VXZcb5bjePSoI8m8qTDEFTJ-w";
const char* ha_entity_id = "switch.sonoff_1000a68f48";
const char* led_on_url = "http://192.168.31.162/LED-Control?ledPwm=3";
const char* led_off_url = "http://192.168.31.162/LED-Control?ledPwm=4";

#define TFT_CS   15
#define TFT_DC   5
#define TFT_RST  4
#define TFT_LED  16
#define T_CS     0

// ============== 日志系统配置 ==============
const int MAX_LOG_ENTRIES = 50;
struct LogEntry { String timestamp; String message; unsigned long epochTime; };
LogEntry logBuffer[MAX_LOG_ENTRIES];
int currentLogIndex = 0;
bool logBufferFull = false;

// ============== EEPROM 持久化设置 ==============
struct Settings {
  uint8_t sleepHour = 22, sleepMinute = 0;
  uint8_t wakeHour = 6, wakeMinute = 0;
  int magic_key = 54321; // 新的key确保EEPROM更新
};
Settings settings;
const int EEPROM_ADDR = 0;

// ===============================================================
// ==================== 网页界面 (v4.9 精简版) ====================
// ===============================================================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP8266 高级控制器</title><style>body, html { margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background-color: #0d1117; color: #c9d1d9; } .header { text-align: center; padding: 2rem 1rem; } .header h1 { font-size: 2rem; color: #58a6ff; display: flex; align-items: center; justify-content: center; gap: 10px; } .header .check-mark { color: #3fb950; font-size: 2rem; } .container { display: flex; flex-wrap: wrap; justify-content: center; gap: 1.5rem; padding: 0 1rem 2rem 1rem; } .card { background-color: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 1.5rem; width: 100%; max-width: 400px; box-sizing: border-box; } .card h2 { margin-top: 0; margin-bottom: 1.5rem; font-size: 1.25rem; color: #8b949e; display: flex; align-items: center; gap: 8px; } .btn-group { display: flex; gap: 10px; } .btn { text-decoration: none; display: inline-block; padding: 10px 20px; font-size: 1rem; font-weight: 500; border-radius: 6px; border: 1px solid #30363d; cursor: pointer; transition: all 0.2s ease-in-out; } .btn-primary { background-color: #238636; color: white; border-color: #3fb950; } .btn-primary:hover { background-color: #2ea043; } .btn-secondary { background-color: #21262d; color: #c9d1d9; } .btn-secondary:hover { border-color: #8b949e; } .form-group { margin-bottom: 1rem; } .form-group label { display: block; margin-bottom: 0.5rem; font-size: 0.9rem; color: #8b949e; } .input-field { width: 100%; background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px; padding: 10px; color: #c9d1d9; font-size: 1rem; box-sizing: border-box; } .schedule-display { font-size: 1.5rem; font-weight: bold; color: #58a6ff; text-align: center; margin: 1rem 0; } @media (min-width: 900px) { .container { max-width: 1200px; margin: 0 auto; } .card { width: calc(33.333% - 1.5rem * 2 / 3); } }</style></head><body><div class="header"><h1><span class="check-mark">✓</span> ESP8266 高级控制器</h1></div><div class="container"><div class="card"><h2>⚙️ 系统操作</h2><div class="btn-group"><a href="/update" class="btn btn-primary">固件更新 🚀</a><a href="/logs" class="btn btn-secondary">查看日志 📋</a></div></div><div class="card"><form action="/settings" method="post"><h2>🌙 夜间定时</h2><div class="form-group"><label>睡眠时间</label><input type="time" name="sleep" class="input-field" value="##SLEEP_TIME##" required></div><div class="form-group"><label>唤醒时间</label><input type="time" name="wake" class="input-field" value="##WAKE_TIME##" required></div><button type="submit" class="btn btn-primary">保存设置</button></form></div><div class="card"><h2>📝 当前计划</h2><div class="schedule-display">##CURRENT_SCHEDULE##</div></div></div></body></html>
)HTML";

// ===============================================================
// ==================== 全局变量 & 函数声明 =======================
// ===============================================================
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);

bool haDeviceState = false, httpDeviceState = false, isInStandby = false;
unsigned long lastActivityTime = 0, lastStatusUpdate = 0;

void loadSettings();
void saveSettings();
void handleRoot();
void handleSettings();
void updateStatusLine();

// ===============================================================
// ==================== 主程序逻辑 (Setup & Loop) =================
// ===============================================================
void setup() {
  Serial.begin(115200);
  EEPROM.begin(sizeof(Settings));
  loadSettings();
  pinMode(TFT_LED, OUTPUT);
  tft.begin();
  ts.begin();
  tft.setRotation(0);
  exitStandby();
  addLog("✅ 系统启动 (v4.9 - 精简优化版)。");
}

void loop() {
  handleTouch();
  if (!isInStandby) {
    server.handleClient();
    ArduinoOTA.handle();

    if (millis() - lastStatusUpdate > 1000) {
      updateStatusLine();
      lastStatusUpdate = millis();
    }

    // ⭐【核心睡眠逻辑重构】
    bool inSleepWindow = false;
    if (WiFi.status() == WL_CONNECTED) {
      timeClient.update();
      int sleepTimeInMinutes = settings.sleepHour * 60 + settings.sleepMinute;
      int wakeTimeInMinutes = settings.wakeHour * 60 + settings.wakeMinute;
      int currentTimeInMinutes = timeClient.getHours() * 60 + timeClient.getMinutes();

      if (wakeTimeInMinutes > sleepTimeInMinutes) { // 当天睡眠 (e.g., 01:00-05:00)
        if (currentTimeInMinutes >= sleepTimeInMinutes && currentTimeInMinutes < wakeTimeInMinutes) {
          inSleepWindow = true;
        }
      } else { // 跨天睡眠 (e.g., 22:30-06:45)
        if (currentTimeInMinutes >= sleepTimeInMinutes || currentTimeInMinutes < wakeTimeInMinutes) {
          inSleepWindow = true;
        }
      }
    }
    
    // 只有在睡眠时间段内，无操作超时才会触发待机
    if (inSleepWindow && (millis() - lastActivityTime > standbyDelay)) {
        addLog("🌙 在预设睡眠时段内无操作，进入待机。");
        enterStandby();
    }

  } else {
    delay(20);
  }
}

// ===============================================================
// ==================== 待机与唤醒 ===============================
// ===============================================================
void enterStandby() {
  if (isInStandby) return;
  isInStandby = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  tft.writeCommand(ILI9341_SLPIN);
  analogWrite(TFT_LED, 0);
  system_update_cpu_freq(80);
  addLog("  - WiFi, Backlight & Display OFF, CPU @ 80MHz.");
}

void exitStandby() {
  addLog("☀️ 触摸唤醒，退出待机模式。");
  system_update_cpu_freq(160);
  tft.writeCommand(ILI9341_SLPOUT);
  delay(120);
  analogWrite(TFT_LED, 1023);
  isInStandby = false;
  lastActivityTime = millis();
  setupWifiAndServices();
  drawButtons();
}

// ===============================================================
// ==================== 网络 & 网页服务 ==========================
// ===============================================================
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
    delay(500); Serial.print(F(".")); retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    addLog("📡 WiFi 已连接, IP: " + WiFi.localIP().toString());
    timeClient.begin();
    // ⭐【路由简化和修复】
    server.on("/", HTTP_GET, handleRoot);
    server.on("/logs", HTTP_GET, handleLogs); // 确保日志路由被正确注册
    server.on("/settings", HTTP_POST, handleSettings);
    httpUpdater.setup(&server);
    server.begin();
    ArduinoOTA.begin();
  } else {
    addLog("❌ WiFi 连接失败。");
  }
}

void handleRoot() {
  String page = FPSTR(MAIN_HTML);
  char time_buf[6];

  sprintf(time_buf, "%02d:%02d", settings.sleepHour, settings.sleepMinute);
  page.replace("##SLEEP_TIME##", time_buf);
  
  sprintf(time_buf, "%02d:%02d", settings.wakeHour, settings.wakeMinute);
  page.replace("##WAKE_TIME##", time_buf);

  String schedule_str = String(settings.sleepHour) + ":" + String(settings.sleepMinute < 10 ? "0" : "") + String(settings.sleepMinute) + 
                       " &rarr; " + 
                       String(settings.wakeHour) + ":" + String(settings.wakeMinute < 10 ? "0" : "") + String(settings.wakeMinute);
  page.replace("##CURRENT_SCHEDULE##", schedule_str);
  
  server.send(200, "text/html; charset=UTF-8", page);
}

void handleSettings() {
  String sleepTime = server.arg("sleep");
  String wakeTime = server.arg("wake");

  settings.sleepHour = sleepTime.substring(0, 2).toInt();
  settings.sleepMinute = sleepTime.substring(3, 5).toInt();
  settings.wakeHour = wakeTime.substring(0, 2).toInt();
  settings.wakeMinute = wakeTime.substring(3, 5).toInt();
  
  saveSettings();
  addLog("⚙️ 夜间定时已更新: " + sleepTime + " - " + wakeTime);
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// ===============================================================
// ==================== 屏幕绘制 & UI ============================
// ===============================================================
void drawButtons() {
  if (isInStandby) return;
  tft.fillScreen(ILI9341_BLACK);
  // (此函数内容无改动)
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(40, 30); tft.print(F("Home Assistant"));
  tft.drawRoundRect(40, 50, 160, 100, 10, ILI9341_WHITE);
  if (haDeviceState) {
    tft.fillRoundRect(50, 60, 140, 80, 10, ILI9341_GREEN);
    tft.setTextColor(ILI9341_BLACK); tft.setTextSize(3); tft.setCursor(95, 90); tft.print(F("ON"));
  } else {
    tft.fillRoundRect(50, 60, 140, 80, 10, ILI9341_RED);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3); tft.setCursor(85, 90); tft.print(F("OFF"));
  }
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(50, 170); tft.print(F("HTTP Control"));
  tft.drawRoundRect(40, 190, 160, 100, 10, ILI9341_WHITE);
  if (httpDeviceState) {
    tft.fillRoundRect(50, 200, 140, 80, 10, ILI9341_GREEN);
    tft.setTextColor(ILI9341_BLACK); tft.setTextSize(3); tft.setCursor(95, 230); tft.print(F("ON"));
  } else {
    tft.fillRoundRect(50, 200, 140, 80, 10, ILI9341_RED);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3); tft.setCursor(85, 230); tft.print(F("OFF"));
  }
}

void updateStatusLine() {
  if (isInStandby) return;
  tft.fillRect(0, 305, 240, 15, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_YELLOW);
  
  tft.setCursor(5, 310);
  if (WiFi.status() == WL_CONNECTED) {
      tft.print("IP: " + WiFi.localIP().toString());
  } else {
      tft.print(F("WiFi Disconnected"));
  }

  if (WiFi.status() == WL_CONNECTED) {
    tft.setCursor(180, 310);
    tft.print(timeClient.getFormattedTime().substring(0, 5));
  }
}

// ===============================================================
// ==================== 触摸 & 设备控制 ==========================
// ===============================================================
void handleTouch() {
  if (ts.touched()) {
    static unsigned long lastTouchDebounce = 0;
    if (millis() - lastTouchDebounce < 500) return;
    lastTouchDebounce = millis();
    lastActivityTime = millis();
    
    if (isInStandby) { exitStandby(); return; }

    TS_Point p = ts.getPoint();
    int screen_x = map(p.y, 295, 3750, 0, 240); 
    int screen_y = map(p.x, 358, 3810, 0, 320); 
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

void controlHttp(bool state) {
  if (isInStandby) return;
  addLog("  📤 发送 HTTP 请求. 状态: " + String(state ? "ON" : "OFF"));
  WiFiClient client; HTTPClient http;
  const char* url = state ? led_on_url : led_off_url;
  if (http.begin(client, url)) { http.GET(); http.end(); }
}

void controlHA(bool state) {
  if (isInStandby) return;
  addLog("  📤 发送 HA 请求. 状态: " + String(state ? "ON" : "OFF"));
  WiFiClient client; HTTPClient http;
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

// ===============================================================
// ==================== 日志 & EEPROM 管理 =======================
// ===============================================================
void addLog(String message) {
  // (此函数无改动)
  if (WiFi.status() != WL_CONNECTED) {
    logBuffer[currentLogIndex].timestamp = "N/A";
    logBuffer[currentLogIndex].epochTime = 0;
  } else {
    timeClient.update();
    unsigned long epochTime = timeClient.getEpochTime();
    logBuffer[currentLogIndex].epochTime = epochTime;
    time_t rawTime = epochTime;
    struct tm * timeinfo = localtime(&rawTime);
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    logBuffer[currentLogIndex].timestamp = String(buffer);
  }
  logBuffer[currentLogIndex].message = message;
  Serial.println("[LOG] " + logBuffer[currentLogIndex].timestamp + " - " + message);
  currentLogIndex = (currentLogIndex + 1) % MAX_LOG_ENTRIES;
  if (currentLogIndex == 0) logBufferFull = true;
}

void handleLogs() {
  // (此函数无改动)
  String html = "<!DOCTYPE html><html lang='zh-CN'><head><title>事件日志</title><meta http-equiv='refresh' content='10'><style>body{font-family: monospace; background-color: #0d1117; color: #c9d1d9; padding: 1rem;} h1{color:#58a6ff;} table{width: 100%; border-collapse: collapse; margin-top: 1rem;} th, td{border: 1px solid #30363d; padding: 10px; text-align: left;} th{background-color: #161b22;} a{color: #58a6ff; text-decoration:none;}</style></head><body><h1>📋 设备事件日志 (仅显示最近24小时)</h1><p><a href='/'>&larr; 返回主页</a></p><table><tr><th>时间戳</th><th>事件</th></tr>";
  int count = logBufferFull ? MAX_LOG_ENTRIES : currentLogIndex;
  unsigned long currentTime = timeClient.getEpochTime();
  unsigned long dayInSeconds = 24 * 3600;
  for (int i = 0; i < count; i++) {
    int index = (currentLogIndex - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    if (logBuffer[index].message.isEmpty() || (currentTime > 0 && logBuffer[index].epochTime > 0 && currentTime - logBuffer[index].epochTime > dayInSeconds)) continue;
    html += "<tr><td>" + logBuffer[index].timestamp + "</td><td>" + logBuffer[index].message + "</td></tr>";
  }
  html += "</table></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

void loadSettings() {
  EEPROM.get(EEPROM_ADDR, settings);
  if (settings.magic_key != 54321) {
    settings.sleepHour = 22;
    settings.sleepMinute = 0;
    settings.wakeHour = 6;
    settings.wakeMinute = 0;
    settings.magic_key = 54321;
    saveSettings();
    addLog("ℹ️ 未找到有效设置，已初始化为默认值 (22:00-06:00)。");
  } else {
    addLog("✅ 已从EEPROM加载定时设置。");
  }
}

void saveSettings() {
  EEPROM.put(EEPROM_ADDR, settings);
  EEPROM.commit();
}