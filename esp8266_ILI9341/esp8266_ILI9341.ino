// ===============================================================
// ================ ESP8266_ILI9341 v8.3 Final CN =================
// == (修复自动唤醒联网 + 网页UI + 永久配置 + 屏幕UI) ===========
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
#include <IRremoteESP8266.h>
#include <IRsend.h>

// [关键修复] 引入 LittleFS 用于永久保存设置，替代 EEPROM
#include <FS.h>
#include <LittleFS.h>

extern "C" {
#include "user_interface.h"
}

// ===============================================================
// ==================== 用户配置区域 =============================
// ===============================================================
const char* ssid = "yang1234";
const char* password = "y123456789";
unsigned long standbyDelay = 60000; // 60秒无操作后进入待机模式

// Home Assistant 配置
const char* ha_host = "192.168.31.22";
const int ha_port = 8123;
const char* ha_token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiIwYjU4YTMwOWMzNmE0ZDE2ODBjOGI2MzI4YzAwMTlkZCIsImlhdCI6MTc1ODk3NDgwMCwiZXhwIjoyMDc0MzM0ODAwfQ.e1e_iE6iIpdB2EG0d0VXZcb5bjePSoI8m8qTDEFTJ-w";
const char* ha_entity_id = "switch.sonoff_1000a68f48";

// HTTP 控制配置
const char* led_on_url = "http://192.168.31.162/LED-Control?ledPwm=3";
const char* led_off_url = "http://192.168.31.162/LED-Control?ledPwm=4";

// 引脚定义
#define TFT_CS   15
#define TFT_DC   5
#define TFT_RST  4
#define T_CS     0
const uint16_t kIrLedPin = 16; // 红外发射引脚 (D0)

// 默认红外编码 (用于首次启动或重置)
const unsigned long DEFAULT_CODE_ON          = 0x1FE48B7;
const unsigned long DEFAULT_CODE_OFF         = 0x1FE7887;
const unsigned long DEFAULT_CODE_BRIGHT_UP   = 0x1FE609F;
const unsigned long DEFAULT_CODE_BRIGHT_DOWN = 0x1FEA05F;
const unsigned long DEFAULT_CODE_AUTO        = 0x1FE807F;
const unsigned long DEFAULT_CODE_TIMER_3H    = 0x1FE58A7;
const unsigned long DEFAULT_CODE_TIMER_5H    = 0x1FE40BF;
const unsigned long DEFAULT_CODE_TIMER_8H    = 0x1FEC03F;

// ============== 赛博风格配色定义 ==============
#define C_BG        0x0000
#define C_GREEN     0x07E0
#define C_CYAN      0x07FF
#define C_RED       0xF800
#define C_ORANGE    0xFD20
#define C_GRID      0x10A2
#define C_WHITE     0xFFFF
#define C_PURPLE    0x780F
#define C_DARK_GREY 0x31A6
#define C_YELLOW    0xFFE0

// ============== 日志系统配置 ==============
const int MAX_LOG_ENTRIES = 50;
struct LogEntry { String timestamp; String message; unsigned long epochTime; };
LogEntry logBuffer[MAX_LOG_ENTRIES];
int currentLogIndex = 0;
bool logBufferFull = false;

// [关键修复] 用于从文件保存/加载的设置结构体
struct Settings {
  uint8_t sleepHour = 22, sleepMinute = 0;
  uint8_t wakeHour = 6, wakeMinute = 0;
  unsigned long ir_on, ir_off, ir_bright_up, ir_bright_down, ir_auto, ir_timer_3h, ir_timer_5h, ir_timer_8h;
  char weatherCity[32];
  char weatherApiKey[64];
  int magic_key = 80101; // 更新校验码
};
Settings settings;
const char* configFile = "/config.json"; // 配置文件名

// ============== 全局对象 ==============
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);
IRsend irsend(kIrLedPin);

// ============== 状态变量 ==============
bool haDeviceState = false, httpDeviceState = false, isInStandby = false;
bool irLightState = false;
unsigned long lastActivityTime = 0, lastStatusUpdate = 0, lastWakeupCheck = 0;

// ============== 屏幕轮播控制 ==============
enum ScreenMode { SCREEN_CONTROL, SCREEN_WEATHER, SCREEN_CLOCK };
ScreenMode currentScreen = SCREEN_CONTROL;
unsigned long lastScreenSwitchTime = 0;
bool pauseRotation = false; 

// ============== 天气数据缓存 ==============
String weather_main = "NODATA";
String weather_temp = "--";
String weather_desc = "SYSTEM INIT";
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_UPDATE_INTERVAL = 15 * 60 * 1000;

// ============== 时钟局部刷新状态变量 ==============
int lastMinute = -1, lastSecond = -1, lastDay = -1;

// ============== 函数声明 ==============
void loadSettings();
void saveSettings();
void handleRoot();
void handleSettings();
void handleSaveIR();
void handleIrCommand();
void updateStatusLine();
void enterStandby();
void exitStandby(bool wifiAlreadyConnected = false); // [修复] 增加参数
void setupWifiAndServices(bool wifiAlreadyConnected = false); // [修复] 增加参数
void handleTouch();
void drawCurrentScreen(bool forceRedraw = false);
void drawControlScreen();
void drawWeatherScreen();
void drawClockScreen(bool isInitialDraw);
void updateClockTime();
void updateWeather();
void controlHttp(bool state);
void controlHA(bool state);
void addLog(String message);
void handleLogs();
void drawCyberFrame(int x, int y, int w, int h, uint16_t color, String label);
void drawGridBackground();
void drawWeatherIcon(String weather, int x, int y);


// ==================== 网页界面 HTML (v8.1 版本) =========================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>esp8266_ILI9341 控制台</title><style>body, html { margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background-color: #0d1117; color: #c9d1d9; } .header { text-align: center; padding: 2rem 1rem; } .header h1 { font-size: 2rem; color: #58a6ff; display: flex; align-items: center; justify-content: center; gap: 10px; } .header .check-mark { color: #3fb950; } .container { display: flex; flex-wrap: wrap; justify-content: center; gap: 1.5rem; padding: 0 1rem 2rem 1rem; } .card { background-color: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 1.5rem; width: 100%; max-width: 400px; box-sizing: border-box; } .card h2 { margin-top: 0; margin-bottom: 1.5rem; font-size: 1.25rem; color: #8b949e; display: flex; align-items: center; gap: 8px; } .btn-group { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; } .btn { text-decoration: none; display: inline-block; padding: 10px 20px; font-size: 1rem; font-weight: 500; border-radius: 6px; border: 1px solid #30363d; cursor: pointer; transition: all 0.2s ease-in-out; text-align: center; box-sizing: border-box; width: 100%; } .btn-primary { background-color: #238636; color: white; border-color: #3fb950; } .btn-primary:hover { background-color: #2ea043; } .btn-secondary { background-color: #21262d; color: #c9d1d9; } .btn-secondary:hover { border-color: #8b949e; } .btn-danger { background-color: #da3633; color: white; border-color: #d0302d; } .btn-danger:hover { background-color: #e04442; } .form-group { margin-bottom: 1rem; } .form-group label { display: block; margin-bottom: 0.5rem; font-size: 0.9rem; color: #8b949e; } .input-field { width: 100%; background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px; padding: 10px; color: #c9d1d9; font-size: 1rem; box-sizing: border-box; } .schedule-display { font-size: 1.5rem; font-weight: bold; color: #58a6ff; text-align: center; margin: 1rem 0; } .ir-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; } </style></head><body><div class="header"><h1><span class="check-mark">✓</span> esp8266_ILI9341 v8.2</h1></div><div class="container"><div class="card"><h2>⚙️ 系统操作</h2><div class="btn-group"><a href="/update" class="btn btn-primary">固件更新 🚀</a><a href="/logs" class="btn btn-secondary">查看日志 📋</a></div></div><div class="card"><form action="/settings" method="post"><h2>🌙 夜间定时</h2><div class="form-group"><label>睡眠时间</label><input type="time" name="sleep" class="input-field" value="##SLEEP_TIME##" required></div><div class="form-group"><label>唤醒时间</label><input type="time" name="wake" class="input-field" value="##WAKE_TIME##" required></div><h2>🌦️ 基础配置</h2><div class="form-group"><label>OpenWeather API Key</label><input type="text" name="apikey" class="input-field" value="##APIKEY##"></div><div class="form-group"><label>城市拼音 (如 beijing)</label><input type="text" name="city" class="input-field" value="##CITY##"></div><button type="submit" class="btn btn-primary">保存设置</button></form></div><div class="card"><h2>📡 红外配置 (HEX)</h2><form action="/save_ir" method="post"><div class="ir-grid"><div class="form-group"><label>ON</label><input type="text" name="ir_on" class="input-field" value="##IR_ON##"></div><div class="form-group"><label>OFF</label><input type="text" name="ir_off" class="input-field" value="##IR_OFF##"></div><div class="form-group"><label>亮度+</label><input type="text" name="ir_up" class="input-field" value="##IR_UP##"></div><div class="form-group"><label>亮度-</label><input type="text" name="ir_down" class="input-field" value="##IR_DOWN##"></div><div class="form-group"><label>AUTO</label><input type="text" name="ir_auto" class="input-field" value="##IR_AUTO##"></div><div class="form-group"><label>3H</label><input type="text" name="ir_3h" class="input-field" value="##IR_3H##"></div><div class="form-group"><label>5H</label><input type="text" name="ir_5h" class="input-field" value="##IR_5H##"></div><div class="form-group"><label>8H</label><input type="text" name="ir_8h" class="input-field" value="##IR_8H##"></div></div><button type="submit" class="btn btn-primary">更新红外码</button></form></div><div class="card"><h2>📝 当前计划</h2><div class="schedule-display">##CURRENT_SCHEDULE##</div></div><div class="card"><h2>💡 灯光红外遥控</h2><div class="btn-group"><a href="/ir?cmd=on" class="btn btn-primary">ON</a><a href="/ir?cmd=off" class="btn btn-danger">OFF</a><a href="/ir?cmd=bright_up" class="btn btn-secondary">亮度 +</a><a href="/ir?cmd=bright_down" class="btn btn-secondary">亮度 -</a><a href="/ir?cmd=auto" class="btn btn-secondary">AUTO</a><a href="/ir?cmd=timer_3h" class="btn btn-secondary">3H</a><a href="/ir?cmd=timer_5h" class="btn btn-secondary">5H</a><a href="/ir?cmd=timer_8h" class="btn btn-secondary">8H</a></div></div></div>
</body></html>
)HTML";


void setup() {
  Serial.begin(115200);
  irsend.begin();
  
  if (!LittleFS.begin()) {
    Serial.println("文件系统挂载失败");
    return;
  }
  
  loadSettings();
  
  tft.begin();
  ts.begin();
  tft.setRotation(0); 

  exitStandby(); 
  addLog("系统启动: v8.3 Final CN");
}

void loop() {
  handleTouch(); 
  
  if (!isInStandby) {
    server.handleClient();
    ArduinoOTA.handle();

    if (millis() - lastStatusUpdate > 1000) {
      if(WiFi.status() == WL_CONNECTED) {
        timeClient.update();
      }

      if (currentScreen == SCREEN_CLOCK) {
        updateClockTime();
      } else {
        updateStatusLine();
      }
      lastStatusUpdate = millis();
    }

    if (!pauseRotation) {
        unsigned long interval = 5000;
        if (currentScreen == SCREEN_WEATHER || currentScreen == SCREEN_CLOCK) {
          interval = 4000;
        }

        if (millis() - lastScreenSwitchTime > interval) {
            ScreenMode nextScreen = currentScreen;
            if (currentScreen == SCREEN_CONTROL) nextScreen = SCREEN_WEATHER;
            else if (currentScreen == SCREEN_WEATHER) nextScreen = SCREEN_CLOCK;
            else nextScreen = SCREEN_CONTROL;
            
            if (nextScreen != currentScreen) {
                currentScreen = nextScreen;
                drawCurrentScreen();
            }
            lastScreenSwitchTime = millis();
        }
    } else {
        if (millis() - lastActivityTime > 10000) { 
            pauseRotation = false;
        }
    }

    if (WiFi.status() == WL_CONNECTED && (millis() - lastWeatherUpdate > WEATHER_UPDATE_INTERVAL || lastWeatherUpdate == 0)) {
        updateWeather();
    }

    bool inSleepWindow = false;
    if (WiFi.status() == WL_CONNECTED && timeClient.getEpochTime() > 0) {
      int sleepM = settings.sleepHour * 60 + settings.sleepMinute;
      int wakeM = settings.wakeHour * 60 + settings.wakeMinute;
      int currM = timeClient.getHours() * 60 + timeClient.getMinutes();
      if (wakeM > sleepM) {
        if (currM >= sleepM && currM < wakeM) inSleepWindow = true;
      } else {
        if (currM >= sleepM || currM < wakeM) inSleepWindow = true;
      }
    }
    if (inSleepWindow && (millis() - lastActivityTime > standbyDelay)) {
        enterStandby();
    }
  } else {
    // [关键修复] 重新实现自动唤醒逻辑
    if (millis() - lastWakeupCheck > 30000) {
      addLog("待机中，执行定时唤醒检查...");
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      
      long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
        delay(100);
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        timeClient.forceUpdate();
        int sleepM = settings.sleepHour * 60 + settings.sleepMinute;
        int wakeM = settings.wakeHour * 60 + settings.wakeMinute;
        int currM = timeClient.getHours() * 60 + timeClient.getMinutes();
        
        bool shouldBeAsleep = false;
        if (wakeM > sleepM) {
          shouldBeAsleep = (currM >= sleepM && currM < wakeM);
        } else {
          shouldBeAsleep = (currM >= sleepM || currM < wakeM);
        }
        
        if (!shouldBeAsleep) {
          addLog("到达唤醒时间，执行唤醒流程...");
          // [修复] 关键一步：带着已建立的WiFi连接去唤醒
          exitStandby(true); 
        } else {
          addLog("仍在睡眠时段，断开WiFi继续待机。");
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
        }
      } else {
        addLog("唤醒检查：WiFi连接失败，继续待机。");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      }
      lastWakeupCheck = millis();
    }
    delay(200);
  }
}

// ==================== 赛博风格绘图辅助函数 ====================
// ... (此部分代码与 v8.1 完全相同，为了简洁此处省略)
void drawGridBackground() {
  for (int i = 0; i < 320; i += 40) {
    tft.drawFastHLine(0, i, 240, C_GRID);
  }
  for (int i = 0; i < 240; i += 40) {
    tft.drawFastVLine(i, 0, 320, C_GRID);
  }
}

void drawCyberFrame(int x, int y, int w, int h, uint16_t color, String label) {
  tft.drawRect(x, y, w, h, color);
  int len = 8;
  tft.fillRect(x, y, len, 2, color);
  tft.fillRect(x, y, 2, len, color); 
  tft.fillRect(x + w - len, y, len, 2, color);
  tft.fillRect(x + w - 2, y, 2, len, color); 
  tft.fillRect(x, y + h - 2, len, 2, color);
  tft.fillRect(x, y + h - len, 2, len, color); 
  tft.fillRect(x + w - len, y + h - 2, len, 2, color);
  tft.fillRect(x + w - 2, y + h - len, 2, len, color); 
  
  if (label.length() > 0) {
    tft.setTextSize(1);
    int16_t x1, y1;
    uint16_t text_w, text_h;
    tft.getTextBounds(label, 0, 0, &x1, &y1, &text_w, &text_h);
    tft.fillRect(x + 10, y - (text_h / 2) - 2, text_w + 10, text_h + 4, C_BG);
    tft.setTextColor(color);
    tft.setCursor(x + 15, y - (text_h / 2));
    tft.print(label);
  }
}

// ==================== 屏幕绘制逻辑 ====================
// ... (此部分代码与 v8.1 完全相同，为了简洁此处省略)
void drawCurrentScreen(bool forceRedraw) {
    if (isInStandby) return;
    
    switch(currentScreen) {
        case SCREEN_CONTROL:
            drawControlScreen();
            break;
        case SCREEN_WEATHER:
            drawWeatherScreen();
            break;
        case SCREEN_CLOCK:
            drawClockScreen(true);
            break;
    }
    
    if (currentScreen != SCREEN_CLOCK) {
      updateStatusLine();
    }
}

void drawControlScreen() {
  tft.fillScreen(C_BG);
  drawGridBackground();
  
  tft.fillRect(0, 0, 240, 24, C_PURPLE);
  tft.setTextColor(C_BG);
  tft.setTextSize(2);
  tft.setCursor(35, 5);
  tft.print("[SYSTEM CONTROL]");

  auto drawButton = [&](int y, uint16_t color, const char* label, bool state) {
    tft.fillRect(10, y, 220, 75, C_DARK_GREY);
    tft.drawRect(10, y, 220, 75, color);
    drawCyberFrame(10, y, 220, 75, color, "");
    
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(25, y + 30);
    tft.print(label);
    
    uint16_t stateColor = state ? C_GREEN : C_RED;
    const char* stateText = state ? "ON" : "OFF";
    tft.fillRect(160, y + 20, 60, 35, stateColor);
    tft.setTextColor(C_BG);
    tft.setTextSize(2);
    tft.setCursor(170, y + 30);
    tft.print(stateText);
  };

  drawButton(35, C_GREEN, "HASSIST", haDeviceState);
  drawButton(120, C_CYAN, "HTTP", httpDeviceState);
  drawButton(205, C_PURPLE, "IR", irLightState);
}

void drawWeatherIcon(String weather, int x, int y) {
  weather.toLowerCase();
  if (weather.indexOf("rain") >= 0 || weather.indexOf("drizzle") >= 0) {
    tft.fillCircle(x, y - 5, 25, C_DARK_GREY);
    tft.fillCircle(x - 15, y + 5, 20, C_DARK_GREY);
    tft.fillCircle(x + 15, y + 5, 20, C_DARK_GREY);
    for (int i=0; i<4; i++) {
      tft.drawLine(x - 20 + i*12, y + 25, x - 25 + i*12, y + 45, C_CYAN);
    }
  } else if (weather.indexOf("snow") >= 0) {
    tft.fillCircle(x, y - 5, 25, C_DARK_GREY);
    for (int i=0; i<5; i++) {
      tft.drawCircle(x - 20 + i*10, y + 35, 2, C_WHITE);
    }
  } else if (weather.indexOf("cloud") >= 0) {
    tft.fillCircle(x, y - 5, 30, C_DARK_GREY);
    tft.fillCircle(x - 20, y + 10, 25, C_DARK_GREY);
    tft.fillCircle(x + 25, y + 10, 25, C_WHITE);
  } else if (weather.indexOf("clear") >= 0) {
    tft.fillCircle(x, y, 30, C_YELLOW);
    for (float i=0; i<360; i+= 45) {
      float r = i * 3.14159 / 180;
      tft.drawLine(x + 35*cos(r), y + 35*sin(r), x + 45*cos(r), y + 45*sin(r), C_ORANGE);
    }
  } else { // 默认为雾天效果
    for (int i=0; i<4; i++) {
      tft.drawFastHLine(x-30, y-15+i*10, 60, C_DARK_GREY);
    }
  }
}

void drawWeatherScreen() {
  tft.fillScreen(C_BG);
  drawGridBackground();
  
  tft.fillRect(0, 0, 240, 24, C_GREEN);
  tft.setTextColor(C_BG);
  tft.setTextSize(2);
  tft.setCursor(5, 5);
  tft.print("[ENV SCAN]");

  tft.setTextColor(C_DARK_GREY);
  tft.setTextSize(1);
  tft.setCursor(130, 9);
  tft.print(settings.weatherCity);
  
  tft.setTextColor(C_WHITE);
  tft.setTextSize(8);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(weather_temp, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(120 - w/2, 45);
  tft.print(weather_temp);
  tft.setTextSize(3);
  tft.drawCircle(tft.getCursorX() + 10, 55, 5, C_WHITE);

  drawWeatherIcon(weather_main, 120, 160);

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.getTextBounds(weather_desc, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(120 - w/2, 250);
  tft.print(weather_desc);
}

void drawClockScreen(bool isInitialDraw) {
    if (!isInitialDraw) return;
    
    tft.fillScreen(C_BG);
    drawGridBackground();
  
    tft.fillRect(0, 0, 240, 24, C_CYAN);
    tft.setTextColor(C_BG);
    tft.setTextSize(2);
    tft.setCursor(30, 5);
    tft.print("[SYSTEM CHRONOMETER]");

    drawCyberFrame(20, 250, 200, 40, C_GREEN, "");
    drawCyberFrame(20, 210, 200, 15, C_ORANGE, "");
    
    lastMinute = -1;
    lastSecond = -1;
    lastDay = -1;
    updateClockTime();
    updateStatusLine();
}

void updateClockTime() {
    time_t rawTime = timeClient.getEpochTime();
    struct tm * ti = localtime(&rawTime);
    
    if (ti->tm_mday != lastDay) {
        lastDay = ti->tm_mday;
        char dateBuf[20];
        sprintf(dateBuf, "%04d.%02d.%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
        tft.fillRect(10, 40, 220, 25, C_BG);
        tft.setTextColor(C_GREEN);
        tft.setTextSize(2);
        tft.setCursor(55, 45);
        tft.print(dateBuf);
        
        const char* weeks[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
        tft.fillRect(22, 252, 196, 36, C_BG);
        tft.setTextColor(C_CYAN);
        tft.setTextSize(3);
        int16_t x1, y1;
        uint16_t w, h;
        tft.getTextBounds(weeks[ti->tm_wday], 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(120 - w / 2, 260);
        tft.print(weeks[ti->tm_wday]);
    }
    
    if (ti->tm_min != lastMinute) {
        lastMinute = ti->tm_min;
        char timeBuf[6];
        sprintf(timeBuf, "%02d:%02d", ti->tm_hour, ti->tm_min);
        tft.fillRect(5, 80, 230, 80, C_BG);
        tft.setTextColor(C_WHITE);
        tft.setTextSize(7);
        tft.setCursor(20, 100);
        tft.print(timeBuf);
    }

    if (ti->tm_sec != lastSecond) {
        lastSecond = ti->tm_sec;
        int barWidth = map(ti->tm_sec, 0, 59, 0, 198);
        tft.fillRect(21, 211, 198, 13, C_DARK_GREY);
        tft.fillRect(21, 211, barWidth, 13, C_ORANGE);
        
        char secBuf[3];
        sprintf(secBuf, "%02d", ti->tm_sec);
        tft.fillRect(105, 185, 30, 16, C_BG);
        tft.setTextSize(2);
        tft.setTextColor(C_ORANGE);
        tft.setCursor(105, 185);
        tft.print(secBuf);
    }
}

// ==================== 网络与控制 ====================
// ... (此部分代码与 v8.1 完全相同，为了简洁此处省略)
void updateWeather() {
  if (strlen(settings.weatherApiKey) < 10 || strlen(settings.weatherCity) == 0) {
    addLog("天气 API Key 或城市未设置。");
    return;
  }
  
  WiFiClient client;
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + String(settings.weatherCity) + "&appid=" + String(settings.weatherApiKey) + "&units=metric&lang=en";
  
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
        StaticJsonDocument<1024> doc;
        deserializeJson(doc, http.getString());
        
        weather_temp = String((int)doc["main"]["temp"]);
        weather_main = (const char*)doc["weather"][0]["main"];
        weather_desc = (const char*)doc["weather"][0]["description"];
        
        weather_main.toUpperCase(); 
        weather_desc.toUpperCase();
        
        addLog("天气已更新: " + weather_temp + "C");
        lastWeatherUpdate = millis();
        if (currentScreen == SCREEN_WEATHER) {
          drawWeatherScreen();
        }
    } else {
        addLog("天气更新失败, 错误码: " + String(httpCode));
    }
    http.end();
  } else {
      addLog("天气 HTTP 连接失败。");
  }
}

void updateStatusLine() {
  tft.fillRect(0, 305, 240, 15, C_BG);
  tft.drawFastHLine(0, 304, 240, C_CYAN);
  tft.setTextSize(1);
  tft.setTextColor(C_GREEN);
  tft.setCursor(2, 308);
  if (WiFi.status() == WL_CONNECTED) {
    tft.print("IP:");
    tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(C_RED);
    tft.print("NET_ERR");
  }
  String t = timeClient.getFormattedTime().substring(0, 5);
  tft.setTextColor(C_CYAN);
  tft.setCursor(205, 308);
  tft.print(t);
}

void handleTouch() {
  if (ts.touched()) {
    static unsigned long lastTouchDebounce = 0;
    if (millis() - lastTouchDebounce < 300) return;
    
    lastTouchDebounce = millis();
    lastActivityTime = millis();
    
    if (isInStandby) {
      addLog("触摸唤醒，退出待机模式。");
      exitStandby();
      return;
    }
    
    pauseRotation = true;
    if (currentScreen != SCREEN_CONTROL) {
      currentScreen = SCREEN_CONTROL;
      drawCurrentScreen();
      return;
    }
    
    TS_Point p = ts.getPoint();
    int sx = map(p.y, 295, 3750, 0, 240);
    int sy = map(p.x, 358, 3810, 0, 320);
    
    if (sx > 10 && sx < 230) {
      if (sy > 35 && sy < 110) {
        haDeviceState = !haDeviceState;
        controlHA(haDeviceState);
      } else if (sy > 120 && sy < 195) {
        httpDeviceState = !httpDeviceState;
        controlHttp(httpDeviceState);
      } else if (sy > 205 && sy < 280) {
        irLightState = !irLightState;
        irsend.sendNEC(irLightState ? settings.ir_on : settings.ir_off);
      }
      drawControlScreen();
    }
  }
}

void enterStandby() {
  if (isInStandby) return;
  isInStandby = true;
  addLog("进入待机模式。");
  tft.fillScreen(ILI9341_BLACK);
  tft.writeCommand(ILI9341_SLPIN);
  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  system_update_cpu_freq(80);
}

// [修复] exitStandby 现在接受一个参数来处理不同的唤醒情况
void exitStandby(bool wifiAlreadyConnected) {
  system_update_cpu_freq(160);
  
  // 【新增代码】不要仅仅唤醒，直接重新初始化一次屏幕，防止白屏死机
  tft.begin(); 
  tft.setRotation(0);
  
  tft.writeCommand(ILI9341_SLPOUT);
  isInStandby = false;
  lastActivityTime = millis();
  addLog("退出待机模式，重新初始化服务...");
  setupWifiAndServices(wifiAlreadyConnected);
}

// [修复] setupWifiAndServices 现在可以跳过WiFi连接步骤
void setupWifiAndServices(bool wifiAlreadyConnected) {
  // 如果WiFi尚未连接（例如通过触摸唤醒），则执行完整的连接流程
  if (!wifiAlreadyConnected) {
    tft.fillScreen(C_BG);
    drawGridBackground();
    tft.setTextColor(C_GREEN);
    tft.setTextSize(2); 
    tft.setCursor(10, 100);
    tft.print("CONNECTING NETWORK...");
    tft.drawRect(10, 130, 220, 20, C_GREEN);
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
      delay(500);
      tft.fillRect(12, 132, retry * 10.9, 16, C_GREEN);
      retry++;
    }
  }

  // 公共的服务启动流程
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiAlreadyConnected) { // 如果是首次连接，显示成功信息
        tft.setCursor(10, 160);
        tft.print("WIFI CONNECTED!");
    }
    addLog("WiFi 已连接, IP: " + WiFi.localIP().toString());
    timeClient.begin();
    timeClient.update();
    server.on("/", HTTP_GET, handleRoot);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/save_ir", HTTP_POST, handleSaveIR);
    server.on("/ir", HTTP_GET, handleIrCommand);
    server.on("/logs", HTTP_GET, handleLogs);
    httpUpdater.setup(&server);
    server.begin();
    ArduinoOTA.begin();
    updateWeather(); 
  } else {
    addLog("WiFi 连接失败。");
  }
  
  delay(1000);
  drawCurrentScreen();
}

// ==================== Web 处理函数 ====================
// ... (此部分代码与 v8.1 完全相同，为了简洁此处省略)
void handleRoot() {
  String page = FPSTR(MAIN_HTML);
  char time_buf[6];
  sprintf(time_buf, "%02d:%02d", settings.sleepHour, settings.sleepMinute);
  page.replace("##SLEEP_TIME##", time_buf);
  sprintf(time_buf, "%02d:%02d", settings.wakeHour, settings.wakeMinute);
  page.replace("##WAKE_TIME##", time_buf);
  
  page.replace("##APIKEY##", String(settings.weatherApiKey));
  page.replace("##CITY##", String(settings.weatherCity));
  
  String schedule_str = String(settings.sleepHour) + ":" + (settings.sleepMinute < 10 ? "0" : "") + String(settings.sleepMinute) + 
                       " &rarr; " + 
                       String(settings.wakeHour) + ":" + (settings.wakeMinute < 10 ? "0" : "") + String(settings.wakeMinute);
  page.replace("##CURRENT_SCHEDULE##", schedule_str);

  auto toHex = [&](unsigned long val) { String s = String(val, HEX); s.toUpperCase(); return s; };
  page.replace("##IR_ON##", toHex(settings.ir_on));
  page.replace("##IR_OFF##", toHex(settings.ir_off));
  page.replace("##IR_UP##", toHex(settings.ir_bright_up));
  page.replace("##IR_DOWN##", toHex(settings.ir_bright_down));
  page.replace("##IR_AUTO##", toHex(settings.ir_auto));
  page.replace("##IR_3H##", toHex(settings.ir_timer_3h));
  page.replace("##IR_5H##", toHex(settings.ir_timer_5h));
  page.replace("##IR_8H##", toHex(settings.ir_timer_8h));
  
  server.send(200, "text/html; charset=UTF-8", page);
}

void handleSettings() {
  if (server.hasArg("sleep")) {
    String sleepTime = server.arg("sleep");
    settings.sleepHour = sleepTime.substring(0, 2).toInt();
    settings.sleepMinute = sleepTime.substring(3, 5).toInt();
    addLog("夜间定时已更新。");
  }
  
  if (server.hasArg("wake")) {
    String wakeTime = server.arg("wake");
    settings.wakeHour = wakeTime.substring(0, 2).toInt();
    settings.wakeMinute = wakeTime.substring(3, 5).toInt();
  }

  if(server.hasArg("city")) {
    strncpy(settings.weatherCity, server.arg("city").c_str(), sizeof(settings.weatherCity) - 1);
    addLog("城市已更新。");
  }

  if(server.hasArg("apikey")) {
    strncpy(settings.weatherApiKey, server.arg("apikey").c_str(), sizeof(settings.weatherApiKey) - 1);
    addLog("API Key已更新。");
  }
  
  saveSettings();
  updateWeather(); 
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSaveIR() {
  auto fromHex = [&](const char* n) { return strtoul(server.arg(n).c_str(), NULL, 16); };
  if (server.hasArg("ir_on")) settings.ir_on = fromHex("ir_on");
  if (server.hasArg("ir_off")) settings.ir_off = fromHex("ir_off");
  if (server.hasArg("ir_up")) settings.ir_bright_up = fromHex("ir_up");
  if (server.hasArg("ir_down")) settings.ir_bright_down = fromHex("ir_down");
  if (server.hasArg("ir_auto")) settings.ir_auto = fromHex("ir_auto");
  if (server.hasArg("ir_3h")) settings.ir_timer_3h = fromHex("ir_3h");
  if (server.hasArg("ir_5h")) settings.ir_timer_5h = fromHex("ir_5h");
  if (server.hasArg("ir_8h")) settings.ir_timer_8h = fromHex("ir_8h");
  
  saveSettings();
  addLog("红外编码已更新");
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleIrCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String cmd = server.arg("cmd");
  unsigned long code_to_send = 0;
  
  if(cmd.equals("on")) { code_to_send = settings.ir_on; irLightState = true; }
  else if(cmd.equals("off")) { code_to_send = settings.ir_off; irLightState = false; }
  else if(cmd.equals("bright_up")) code_to_send = settings.ir_bright_up;
  else if(cmd.equals("bright_down")) code_to_send = settings.ir_bright_down;
  else if(cmd.equals("auto")) code_to_send = settings.ir_auto;
  else if(cmd.equals("timer_3h")) code_to_send = settings.ir_timer_3h;
  else if(cmd.equals("timer_5h")) code_to_send = settings.ir_timer_5h;
  else if(cmd.equals("timer_8h")) code_to_send = settings.ir_timer_8h;
  else {
    server.send(404, "text/plain", "Command not found.");
    return;
  }
  
  addLog("发送 IR 请求. 指令: " + cmd);
  irsend.sendNEC(code_to_send);
  
  if (currentScreen == SCREEN_CONTROL) {
    drawControlScreen();
  }
   
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", ""); 
}

void controlHttp(bool state) {
  if (isInStandby) return;
  addLog("HTTP 控制: " + String(state ? "ON" : "OFF"));
  WiFiClient client;
  HTTPClient http;
  if (http.begin(client, state ? led_on_url : led_off_url)) {
    http.GET();
    http.end();
  }
}

void controlHA(bool state) {
  if (isInStandby) return;
  addLog("HA 控制: " + String(state ? "ON" : "OFF"));
  WiFiClient client;
  HTTPClient http;
  if (http.begin(client, ha_host, ha_port, String("/api/services/switch/") + (state ? "turn_on" : "turn_off"))) {
    http.addHeader("Authorization", "Bearer " + String(ha_token));
    http.addHeader("Content-Type", "application/json");
    http.POST("{\"entity_id\":\"" + String(ha_entity_id) + "\"}");
    http.end();
  }
}

// ==================== 日志与设置存储 ====================
// ... (此部分代码与 v8.1 完全相同，为了简洁此处省略)
void addLog(String m) {
  if (WiFi.status() == WL_CONNECTED && timeClient.getEpochTime() > 0) {
    logBuffer[currentLogIndex] = {timeClient.getFormattedTime(), m, timeClient.getEpochTime()}; 
  } else {
    logBuffer[currentLogIndex] = {"[No Time]", m, 0};
  }
  
  currentLogIndex = (currentLogIndex + 1) % MAX_LOG_ENTRIES;
  if (currentLogIndex == 0) {
    logBufferFull = true;
  }
  
  Serial.println("LOG: " + m);
}

void handleLogs() {
  String h = "<html><head><meta charset='UTF-8'><style>body{font-family:monospace;background:#000;color:#0f0;}</style></head><body><h2>系统日志</h2><ul>"; 
  int c = logBufferFull ? MAX_LOG_ENTRIES : currentLogIndex; 
  for (int i=0; i<c; i++) {
    int idx = (currentLogIndex - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    h += "<li>" + logBuffer[idx].timestamp + ": " + logBuffer[idx].message + "</li>";
  }
  h += "</ul><a href='/'>返回</a></body></html>";
  server.send(200, "text/html; charset=UTF-8", h);
}

void loadSettings() {
  File file = LittleFS.open(configFile, "r");
  if (!file) {
    addLog("配置文件未找到，加载默认设置。");
    settings.sleepHour = 22; settings.sleepMinute = 0; settings.wakeHour = 6; settings.wakeMinute = 0;
    settings.ir_on = DEFAULT_CODE_ON; settings.ir_off = DEFAULT_CODE_OFF; settings.ir_bright_up = DEFAULT_CODE_BRIGHT_UP;
    settings.ir_bright_down = DEFAULT_CODE_BRIGHT_DOWN; settings.ir_auto = DEFAULT_CODE_AUTO; settings.ir_timer_3h = DEFAULT_CODE_TIMER_3H;
    settings.ir_timer_5h = DEFAULT_CODE_TIMER_5H; settings.ir_timer_8h = DEFAULT_CODE_TIMER_8H;
    strcpy(settings.weatherCity, "zhumadian");
    strcpy(settings.weatherApiKey, ""); 
    settings.magic_key = 80101;
    saveSettings();
    return;
  }

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error || doc["magic_key"] != 80101) {
    addLog("配置文件无效，加载默认设置。");
    LittleFS.remove(configFile);
    loadSettings();
  } else {
    settings.sleepHour = doc["sleepHour"]; settings.sleepMinute = doc["sleepMinute"];
    settings.wakeHour = doc["wakeHour"]; settings.wakeMinute = doc["wakeMinute"];
    settings.ir_on = doc["ir_on"]; settings.ir_off = doc["ir_off"];
    settings.ir_bright_up = doc["ir_bright_up"]; settings.ir_bright_down = doc["ir_bright_down"];
    settings.ir_auto = doc["ir_auto"]; settings.ir_timer_3h = doc["ir_timer_3h"];
    settings.ir_timer_5h = doc["ir_timer_5h"]; settings.ir_timer_8h = doc["ir_timer_8h"];
    strlcpy(settings.weatherCity, doc["weatherCity"], sizeof(settings.weatherCity));
    strlcpy(settings.weatherApiKey, doc["weatherApiKey"], sizeof(settings.weatherApiKey));
    settings.magic_key = doc["magic_key"];
    addLog("已从文件加载设置。");
  }
}

void saveSettings() {
  File file = LittleFS.open(configFile, "w");
  if (!file) {
    addLog("错误：无法打开配置文件进行写入");
    return;
  }
  
  StaticJsonDocument<1024> doc;
  doc["sleepHour"] = settings.sleepHour; doc["sleepMinute"] = settings.sleepMinute;
  doc["wakeHour"] = settings.wakeHour; doc["wakeMinute"] = settings.wakeMinute;
  doc["ir_on"] = settings.ir_on; doc["ir_off"] = settings.ir_off;
  doc["ir_bright_up"] = settings.ir_bright_up; doc["ir_bright_down"] = settings.ir_bright_down;
  doc["ir_auto"] = settings.ir_auto; doc["ir_timer_3h"] = settings.ir_timer_3h;
  doc["ir_timer_5h"] = settings.ir_timer_5h; doc["ir_timer_8h"] = settings.ir_timer_8h;
  doc["weatherCity"] = settings.weatherCity; doc["weatherApiKey"] = settings.weatherApiKey;
  doc["magic_key"] = settings.magic_key;

  if (serializeJson(doc, file) == 0) {
    addLog("错误：写入配置文件失败");
  } else {
    addLog("设置已保存到文件。");
  }
  file.close();
}