// ===============================================================
// ================ ESP8266 高级控制器 v6.1 ======================
// ================ (终极稳定版) ===============================
// ===============================================================

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
#include <IRremoteESP8266.h>
#include <IRsend.h>

extern "C" {
#include "user_interface.h"
}

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

#define TFT_CS   15
#define TFT_DC   5
#define TFT_RST  4
// #define TFT_LED  16 // 【已移除】不再通过代码控制背光
#define T_CS     0

// 红外配置
const uint16_t kIrLedPin = 16; // D0
#define CODE_ON         0xFFC23D
#define CODE_OFF        0xFFB04F
#define CODE_BRIGHT_UP  0xFF10EF
#define CODE_BRIGHT_DOWN 0xFF5AA5
#define CODE_AUTO       0xFF22DD
#define CODE_TIMER_3H   0xFFA857
#define CODE_TIMER_5H   0xFF6897
#define CODE_TIMER_8H   0xFF629D

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
  int magic_key = 54321;
};
Settings settings;
const int EEPROM_ADDR = 0;

// ===============================================================
// ==================== 网页界面 (无改动) =========================
// ===============================================================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP8266 高级控制器</title><style>body, html { margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background-color: #0d1117; color: #c9d1d9; } .header { text-align: center; padding: 2rem 1rem; } .header h1 { font-size: 2rem; color: #58a6ff; display: flex; align-items: center; justify-content: center; gap: 10px; } .header .check-mark { color: #3fb950; font-size: 2rem; } .container { display: flex; flex-wrap: wrap; justify-content: center; gap: 1.5rem; padding: 0 1rem 2rem 1rem; } .card { background-color: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 1.5rem; width: 100%; max-width: 400px; box-sizing: border-box; } .card h2 { margin-top: 0; margin-bottom: 1.5rem; font-size: 1.25rem; color: #8b949e; display: flex; align-items: center; gap: 8px; } .btn-group { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; } .btn { text-decoration: none; display: inline-block; padding: 10px 20px; font-size: 1rem; font-weight: 500; border-radius: 6px; border: 1px solid #30363d; cursor: pointer; transition: all 0.2s ease-in-out; text-align: center; } .btn-primary { background-color: #238636; color: white; border-color: #3fb950; } .btn-primary:hover { background-color: #2ea043; } .btn-secondary { background-color: #21262d; color: #c9d1d9; } .btn-secondary:hover { border-color: #8b949e; } .btn-danger { background-color: #da3633; color: white; border-color: #d0302d; } .form-group { margin-bottom: 1rem; } .form-group label { display: block; margin-bottom: 0.5rem; font-size: 0.9rem; color: #8b949e; } .input-field { width: 100%; background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px; padding: 10px; color: #c9d1d9; font-size: 1rem; box-sizing: border-box; } .schedule-display { font-size: 1.5rem; font-weight: bold; color: #58a6ff; text-align: center; margin: 1rem 0; } @media (min-width: 1300px) { .container { max-width: 1600px; } .card { width: calc(25% - 1.5rem * 3 / 4); } } @media (min-width: 900px) and (max-width: 1299px) { .container { max-width: 1200px; } .card { width: calc(33.333% - 1.5rem * 2 / 3); } }</style></head><body><div class="header"><h1><span class="check-mark">✓</span> ESP8266 高级控制器</h1></div><div class="container"><div class="card"><h2>⚙️ 系统操作</h2><div class="btn-group"><a href="/update" class="btn btn-primary">固件更新 🚀</a><a href="/logs" class="btn btn-secondary">查看日志 📋</a></div></div><div class="card"><form action="/settings" method="post"><h2>🌙 夜间定时</h2><div class="form-group"><label>睡眠时间</label><input type="time" name="sleep" class="input-field" value="##SLEEP_TIME##" required></div><div class="form-group"><label>唤醒时间</label><input type="time" name="wake" class="input-field" value="##WAKE_TIME##" required></div><button type="submit" class="btn btn-primary">保存设置</button></form></div><div class="card"><h2>📝 当前计划</h2><div class="schedule-display">##CURRENT_SCHEDULE##</div></div><div class="card"><h2>💡 灯光红外遥控</h2><div class="btn-group"><a href="/ir?cmd=on" class="btn btn-primary">ON</a><a href="/ir?cmd=off" class="btn btn-danger">OFF</a><a href="/ir?cmd=bright_up" class="btn btn-secondary">亮度 +</a><a href="/ir?cmd=bright_down" class="btn btn-secondary">亮度 -</a><a href="/ir?cmd=auto" class="btn btn-secondary">AUTO</a><a href="/ir?cmd=timer_3h" class="btn btn-secondary">3H</a><a href="/ir?cmd=timer_5h" class="btn btn-secondary">5H</a><a href="/ir?cmd=timer_8h" class="btn btn-secondary">8H</a></div></div></div>
</body></html>
)HTML";

// (此处省略了全局变量、函数声明、主程序逻辑、网络服务、触摸屏、核心功能等所有未改动的代码部分)
// ...
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);
IRsend irsend(kIrLedPin);

bool haDeviceState = false, httpDeviceState = false, isInStandby = false;
bool irLightState = false;
unsigned long lastActivityTime = 0, lastStatusUpdate = 0, lastWakeupCheck = 0;

void loadSettings();
void saveSettings();
void handleRoot();
void handleSettings();
void handleIrCommand();
void updateStatusLine();
void enterStandby();
void exitStandby();
void setupWifiAndServices();
void handleTouch();
void drawButtons();
void controlHttp(bool state);
void controlHA(bool state);
void addLog(String message);
void handleLogs();

void setup() {
  Serial.begin(115200);
  
  irsend.begin();
  Serial.println("[OK] IR sender initialized.");

  EEPROM.begin(sizeof(Settings));
  loadSettings();
  
  // 【已移除】不再需要 pinMode(TFT_LED, OUTPUT);
  
  tft.begin();
  ts.begin();
  tft.setRotation(0);

  exitStandby(); 
  addLog("✅ 系统启动 (v6.1 - 终极稳定版)。");
}
void loop() {
  handleTouch();
  if (!isInStandby) {
    server.handleClient();
    ArduinoOTA.handle();
    if (millis() - lastStatusUpdate > 1000) {
      if(WiFi.status() == WL_CONNECTED) timeClient.update();
      updateStatusLine();
      lastStatusUpdate = millis();
    }
    bool inSleepWindow = false;
    if (WiFi.status() == WL_CONNECTED) {
      int sleepTimeInMinutes = settings.sleepHour * 60 + settings.sleepMinute;
      int wakeTimeInMinutes = settings.wakeHour * 60 + settings.wakeMinute;
      int currentTimeInMinutes = timeClient.getHours() * 60 + timeClient.getMinutes();
      if (wakeTimeInMinutes > sleepTimeInMinutes) {
        if (currentTimeInMinutes >= sleepTimeInMinutes && currentTimeInMinutes < wakeTimeInMinutes) inSleepWindow = true;
      } else {
        if (currentTimeInMinutes >= sleepTimeInMinutes || currentTimeInMinutes < wakeTimeInMinutes) inSleepWindow = true;
      }
    }
    if (inSleepWindow && (millis() - lastActivityTime > standbyDelay)) {
        addLog("🌙 在预设睡眠时段内无操作，进入待机。");
        enterStandby();
    }
  } else {
    if (millis() - lastWakeupCheck > 30000) {
      bool shouldWakeUp = false;
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 10) {
        delay(500); retries++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        timeClient.forceUpdate();
        int sleepTimeInMinutes = settings.sleepHour * 60 + settings.sleepMinute;
        int wakeTimeInMinutes = settings.wakeHour * 60 + settings.wakeMinute;
        int currentTimeInMinutes = timeClient.getHours() * 60 + timeClient.getMinutes();
        bool shouldBeAsleep = false;
        if (wakeTimeInMinutes > sleepTimeInMinutes) {
          if(currentTimeInMinutes >= sleepTimeInMinutes && currentTimeInMinutes < wakeTimeInMinutes) shouldBeAsleep = true;
        } else {
          if(currentTimeInMinutes >= sleepTimeInMinutes || currentTimeInMinutes < wakeTimeInMinutes) shouldBeAsleep = true;
        }
        if (!shouldBeAsleep) {
          shouldWakeUp = true;
        }
      }
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      if(shouldWakeUp){
        addLog("⏰ 到达唤醒时间，执行唤醒流程...");
        exitStandby();
      }
      lastWakeupCheck = millis();
    }
    delay(200);
  }
}


// ===============================================================
// ==================== 待机与唤醒 (已修改) ========================
// ===============================================================
void enterStandby() {
  if (isInStandby) return;
  isInStandby = true;
  
  tft.fillScreen(ILI9341_BLACK);
  delay(50);
  // 【已移除】不再需要 analogWrite(TFT_LED, 0);
  tft.writeCommand(ILI9341_SLPIN); // 屏幕进入睡眠模式

  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  system_update_cpu_freq(80);
  addLog("  - 进入低功耗待机模式。");
}

void exitStandby() {
  system_update_cpu_freq(160);
  tft.writeCommand(ILI9341_SLPOUT); // 屏幕从睡眠中唤醒
  delay(120);
  // 【已移除】不再需要 analogWrite(TFT_LED, 1023);
  
  isInStandby = false;
  lastActivityTime = millis();
  
  setupWifiAndServices();
}

// (此处省略了 网络&网页服务, 屏幕&触摸, 核心功能函数 等，它们与v5.9.2完全相同)
// ...
void setupWifiAndServices() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(20, 150);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.print(F("Connecting WiFi..."));
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500); Serial.print(F(".")); retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    addLog("📡 WiFi 已连接, IP: " + WiFi.localIP().toString());
    tft.fillScreen(ILI9341_BLACK);
    tft.setCursor(20, 150);
    tft.print(F("Syncing Time..."));
    timeClient.begin();
    if (timeClient.forceUpdate()) {
       addLog("🕒 时间同步完成: " + timeClient.getFormattedTime());
    } else {
       addLog("⚠️ 时间同步失败!");
    }
    server.on("/", HTTP_GET, handleRoot);
    server.on("/logs", HTTP_GET, handleLogs);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/ir", HTTP_GET, handleIrCommand);
    httpUpdater.setup(&server);
    server.begin();
    ArduinoOTA.begin();
  } else {
    addLog("❌ WiFi 连接失败。");
  }
  drawButtons();
  updateStatusLine();
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
void drawButtons() {
  if (isInStandby) return;
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(40, 10); tft.print(F("Home Assistant"));
  tft.drawRoundRect(40, 30, 160, 80, 10, ILI9341_WHITE);
  if (haDeviceState) {
    tft.fillRoundRect(50, 40, 140, 60, 10, ILI9341_GREEN);
    tft.setTextColor(ILI9341_BLACK); tft.setTextSize(3); tft.setCursor(95, 60); tft.print(F("ON"));
  } else {
    tft.fillRoundRect(50, 40, 140, 60, 10, ILI9341_RED);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3); tft.setCursor(85, 60); tft.print(F("OFF"));
  }
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(50, 125); tft.print(F("HTTP Control"));
  tft.drawRoundRect(40, 145, 160, 80, 10, ILI9341_WHITE);
  if (httpDeviceState) {
    tft.fillRoundRect(50, 155, 140, 60, 10, ILI9341_GREEN);
    tft.setTextColor(ILI9341_BLACK); tft.setTextSize(3); tft.setCursor(95, 175); tft.print(F("ON"));
  } else {
    tft.fillRoundRect(50, 155, 140, 60, 10, ILI9341_RED);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3); tft.setCursor(85, 175); tft.print(F("OFF"));
  }
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(55, 240); tft.print(F("IR Remote"));
  tft.drawRoundRect(40, 260, 160, 45, 10, ILI9341_WHITE);
  if (irLightState) {
    tft.fillRoundRect(50, 265, 140, 35, 10, ILI9341_GREEN);
    tft.setTextColor(ILI9341_BLACK); tft.setTextSize(3); tft.setCursor(95, 275); tft.print(F("ON"));
  } else {
    tft.fillRoundRect(50, 265, 140, 35, 10, ILI9341_RED);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3); tft.setCursor(85, 275); tft.print(F("OFF"));
  }
}
void updateStatusLine() {
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
void handleTouch() {
  if (ts.touched()) {
    static unsigned long lastTouchDebounce = 0;
    if (millis() - lastTouchDebounce < 500) return;
    lastTouchDebounce = millis();
    if (isInStandby) { 
      addLog("☀️ 触摸唤醒，退出待机模式。");
      exitStandby(); 
      return; 
    }
    lastActivityTime = millis();
    TS_Point p = ts.getPoint();
    int screen_x = map(p.y, 295, 3750, 0, 240); 
    int screen_y = map(p.x, 358, 3810, 0, 320); 
    if (screen_x > 40 && screen_x < 200 && screen_y > 30 && screen_y < 110) { // HA区域
      haDeviceState = !haDeviceState;
      addLog("▶️ HA 按钮按下. 新状态: " + String(haDeviceState ? "ON" : "OFF"));
      controlHA(haDeviceState);
      drawButtons();
    }
    else if (screen_x > 40 && screen_x < 200 && screen_y > 145 && screen_y < 225) { // HTTP区域
      httpDeviceState = !httpDeviceState;
      addLog("▶️ HTTP 按钮按下. 新状态: " + String(httpDeviceState ? "ON" : "OFF"));
      controlHttp(httpDeviceState);
      drawButtons();
    }
    else if (screen_x > 40 && screen_x < 200 && screen_y > 260 && screen_y < 305) { // IR区域
      irLightState = !irLightState;
      addLog("▶️ IR 按钮按下. 新状态: " + String(irLightState ? "ON" : "OFF"));
      irsend.sendNEC(irLightState ? CODE_ON : CODE_OFF);
      drawButtons();
    }
  }
}
void handleIrCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String cmd = server.arg("cmd");
  unsigned long code_to_send = 0;
  if(cmd.equals("on")) { code_to_send = CODE_ON; irLightState = true; }
  else if(cmd.equals("off")) { code_to_send = CODE_OFF; irLightState = false; }
  else if(cmd.equals("bright_up")) code_to_send = CODE_BRIGHT_UP;
  else if(cmd.equals("bright_down")) code_to_send = CODE_BRIGHT_DOWN;
  else if(cmd.equals("auto")) code_to_send = CODE_AUTO;
  else if(cmd.equals("timer_3h")) code_to_send = CODE_TIMER_3H;
  else if(cmd.equals("timer_5h")) code_to_send = CODE_TIMER_5H;
  else if(cmd.equals("timer_8h")) code_to_send = CODE_TIMER_8H;
  else {
    server.send(404, "text/plain", "Command not found.");
    return;
  }
  addLog("  📤 发送 IR 请求. 指令: " + cmd);
  irsend.sendNEC(code_to_send);
  drawButtons(); 
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", ""); 
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
void addLog(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    logBuffer[currentLogIndex].timestamp = "N/A";
    logBuffer[currentLogIndex].epochTime = 0;
  } else {
    time_t rawTime = timeClient.getEpochTime();
    struct tm * timeinfo = localtime(&rawTime);
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    logBuffer[currentLogIndex].timestamp = String(buffer);
    logBuffer[currentLogIndex].epochTime = rawTime;
  }
  logBuffer[currentLogIndex].message = message;
  Serial.println("[LOG] " + logBuffer[currentLogIndex].timestamp + " - " + message);
  currentLogIndex = (currentLogIndex + 1) % MAX_LOG_ENTRIES;
  if (currentLogIndex == 0) logBufferFull = true;
}
void handleLogs() {
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