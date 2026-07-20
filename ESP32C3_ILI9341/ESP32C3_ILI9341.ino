#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <FS.h>
#include <LittleFS.h>

#include "config.h"
#include "web_pages.h"
#include "dht11.h" // 本地底层高效驱动

// ============== 全局对象实例化 ==============
Settings settings;
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS);
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);
IRsend irsend(kIrLedPin);
DHT11_ESP32 dht(DHTPIN); // 实例化本地温湿度传感器对象

// ============== 状态变量 ==============
bool haDeviceState = false;
bool httpDeviceState = false;
bool irLightState = false;
bool relayState = false;          // 继电器当前逻辑状态
bool isInStandby = false;
bool lastInSleepWindow = false;   
bool firstTimeSyncDone = false;   // 标记开机后首次网络时间同步与状态初始化是否完成

// 补回漏掉的时间戳状态变量
unsigned long lastActivityTime = 0, lastStatusUpdate = 0, lastWakeupCheck = 0; 

// ============== 传感器缓存变量 ==============
float dhtTemp = NAN;
float dhtHum = NAN;

// ============== 日志系统配置 ==============
const int MAX_LOG_ENTRIES = 50;
struct LogEntry { String timestamp; String message; unsigned long epochTime; };
LogEntry logBuffer[MAX_LOG_ENTRIES];
int currentLogIndex = 0;
bool logBufferFull = false;

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
void handleRelayCommand(); 
void updateStatusLine();
void enterStandby();
void exitStandby(bool wifiAlreadyConnected = false); 
void setupWifiAndServices(bool wifiAlreadyConnected = false); 
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
bool isSleepTime();
void updateRelayLogic();
void setRelay(bool state); 

// ==================== 统一的继电器硬件控制器（防反偏） ====================
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, state ? (RELAY_ACTIVE_LOW ? LOW : HIGH) : (RELAY_ACTIVE_LOW ? HIGH : LOW));
}

// ==================== 主程序入口 ====================
void setup() {
  Serial.begin(115200);
  irsend.begin();
  dht.begin(); // 初始化温湿度传感器
  
  // 继电器引脚初始化并立即执行安全关闭
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false); // 默认开机完全关闭
  
  // 配置并使能屏幕背光控制引脚
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  
  // ESP32 文件系统开启格式化后挂载
  if (!LittleFS.begin(true)) {
    Serial.println("文件系统挂载失败");
    return;
  }
  
  loadSettings();
  
  // 在初始化屏幕与触控前，指定硬件 SPI 复用引脚
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);
  
  tft.begin();
  ts.begin();
  tft.setRotation(0); 

  exitStandby(); 
  addLog("系统启动: v8.4 Final ESP32-C3");
}

void loop() {
  handleTouch(); 
  
  // 5 秒周期无抢占采集 DHT (调用本地 dht11.h)
  static unsigned long lastDhtRead = 0;
  if (millis() - lastDhtRead > 5000) {
    float t = NAN;
    float h = NAN;
    if (dht.read(t, h)) {
      dhtTemp = t;
      dhtHum = h;
    }
    lastDhtRead = millis();
    updateRelayLogic(); // 计算定时与温控状态
  }

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

    // 自动待机判断
    if (isSleepTime() && (millis() - lastActivityTime > standbyDelay)) {
        enterStandby();
    }
  } else {
    // 自动唤醒后台检查
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
        if (!isSleepTime()) {
          addLog("到达唤醒时间，执行唤醒流程...");
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

// ==================== 继电器智能逻辑计算 ====================
bool isSleepTime() {
  if (WiFi.status() != WL_CONNECTED || timeClient.getEpochTime() <= 0) {
    return false; 
  }
  int sleepM = settings.sleepHour * 60 + settings.sleepMinute;
  int wakeM = settings.wakeHour * 60 + settings.wakeMinute;
  int currM = timeClient.getHours() * 60 + timeClient.getMinutes();
  if (wakeM > sleepM) {
    return (currM >= sleepM && currM < wakeM);
  } else {
    return (currM >= sleepM || currM < wakeM);
  }
}

void updateRelayLogic() {
  // 未联网获取到有效时间前，不进行逻辑判定
  if (WiFi.status() != WL_CONNECTED || timeClient.getEpochTime() <= 0) {
    return;
  }

  bool inSleep = isSleepTime();

  // 1. 判断是否是【开机首次时间同步】或者【正常的休眠/唤醒边界转换】
  if (!firstTimeSyncDone || (inSleep != lastInSleepWindow)) {
    if (inSleep) {
      setRelay(false); // 进入夜间时段 -> 强行关闭继电器
      addLog("定时通知：当前处于休眠时段，强制切断继电器");
    } else {
      if (settings.tempCtrlEnabled) {
        addLog("定时通知：当前处于唤醒时段，已激活温度自动控制模式");
      } else {
        setRelay(true); // 唤醒时段，且未开温控 -> 默认直接开启
        addLog("定时通知：当前处于唤醒时段，温控未开启，默认接通继电器");
      }
    }
    
    lastInSleepWindow = inSleep;
    firstTimeSyncDone = true; // 标记首次时间与状态对齐已完成
    
    if (currentScreen == SCREEN_CONTROL && !isInStandby) {
      drawControlScreen();
    }
  }

  // 2. 状态维持与自动温度控制 (仅在白天且开启温控时起效)
  if (inSleep) {
    if (relayState) {
      setRelay(false);
      if (currentScreen == SCREEN_CONTROL && !isInStandby) drawControlScreen();
    }
  } else {
    if (settings.tempCtrlEnabled) {
      if (!isnan(dhtTemp)) {
        // 【优化点】：当温度高于开启阈值时打开继电器，低于关闭阈值时关闭继电器
        if (dhtTemp > settings.tempThreshold) {
          if (!relayState) {
            setRelay(true);
            addLog("温控触发：温度达到 " + String(dhtTemp, 1) + "C 超过开启阈值，开启继电器");
            if (currentScreen == SCREEN_CONTROL && !isInStandby) drawControlScreen();
          }
        } else if (dhtTemp < settings.tempThresholdOff) { 
          if (relayState) {
            setRelay(false);
            addLog("温控触发：温度降至 " + String(dhtTemp, 1) + "C 低于关闭阈值，关闭继电器");
            if (currentScreen == SCREEN_CONTROL && !isInStandby) drawControlScreen();
          }
        }
      }
    }
  }
}

// ==================== 赛博风格绘图辅助函数 ====================
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
    tft.fillRect(10, y, 220, 55, C_DARK_GREY);
    tft.drawRect(10, y, 220, 55, color);
    drawCyberFrame(10, y, 220, 55, color, "");
    
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(25, y + 20);
    tft.print(label);
    
    uint16_t stateColor = state ? C_GREEN : C_RED;
    const char* stateText = state ? "ON" : "OFF";
    tft.fillRect(160, y + 10, 60, 35, stateColor);
    tft.setTextColor(C_BG);
    tft.setTextSize(2);
    tft.setCursor(170, y + 20);
    tft.print(stateText);
  };

  drawButton(35, C_GREEN, "HASSIST", haDeviceState);
  drawButton(100, C_CYAN, "HTTP", httpDeviceState);
  drawButton(165, C_YELLOW, "RELAY", relayState);
  drawButton(230, C_PURPLE, "IR", irLightState);
}

void drawWeatherIcon(String weather, int x, int y) {
  weather.toLowerCase();
  if (weather.indexOf("rain") >= 0 || weather.indexOf("drizzle") >= 0) {
    tft.fillCircle(x, y - 5, 20, C_DARK_GREY);
    tft.fillCircle(x - 10, y + 5, 15, C_DARK_GREY);
    tft.fillCircle(x + 10, y + 5, 15, C_DARK_GREY);
    for (int i=0; i<3; i++) {
      tft.drawLine(x - 12 + i*12, y + 15, x - 17 + i*12, y + 30, C_CYAN);
    }
  } else if (weather.indexOf("snow") >= 0) {
    tft.fillCircle(x, y - 5, 20, C_DARK_GREY);
    for (int i=0; i<3; i++) {
      tft.drawCircle(x - 10 + i*10, y + 20, 2, C_WHITE);
    }
  } else if (weather.indexOf("cloud") >= 0) {
    tft.fillCircle(x, y - 5, 22, C_DARK_GREY);
    tft.fillCircle(x - 15, y + 5, 18, C_DARK_GREY);
    tft.fillCircle(x + 18, y + 5, 18, C_WHITE);
  } else if (weather.indexOf("clear") >= 0) {
    tft.fillCircle(x, y, 22, C_YELLOW);
    for (float i=0; i<360; i+= 45) {
      float r = i * 3.14159 / 180;
      tft.drawLine(x + 26*cos(r), y + 26*sin(r), x + 34*cos(r), y + 34*sin(r), C_ORANGE);
    }
  } else { 
    for (int i=0; i<3; i++) {
      tft.drawFastHLine(x-20, y-10+i*8, 40, C_DARK_GREY);
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
  tft.setTextSize(5); 
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(weather_temp, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(120 - w/2, 35);
  tft.print(weather_temp);
  tft.setTextSize(2);
  tft.drawCircle(tft.getCursorX() + 8, 40, 4, C_WHITE);

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.getTextBounds(weather_desc, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(120 - w/2, 85);
  tft.print(weather_desc);

  drawWeatherIcon(weather_main, 120, 140);

  drawCyberFrame(10, 185, 220, 110, C_GREEN, "INDOOR CLIMATE");
  
  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  
  tft.setCursor(25, 210);
  tft.print("TEMP: ");
  if (isnan(dhtTemp)) {
    tft.print("-- C");
  } else {
    tft.print(dhtTemp, 1);
    tft.print(" C");
  }

  tft.setCursor(25, 240);
  tft.print("HUMI: ");
  if (isnan(dhtHum)) {
    tft.print("-- %");
  } else {
    tft.print(dhtHum, 1);
    tft.print(" %");
  }

  tft.setTextSize(1);
  tft.setTextColor(C_CYAN);
  tft.setCursor(25, 275);
  tft.print("RELAY: ");
  tft.print(relayState ? "ACTIVE" : "INACTIVE");
  if (settings.tempCtrlEnabled) {
    tft.print(" (AUTO ");
    tft.print(settings.tempThreshold, 0);
    tft.print("~");
    tft.print(settings.tempThresholdOff, 0);
    tft.print("C)");
  } else {
    tft.print(" (MANUAL)");
  }
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
      if (sy > 35 && sy < 90) {
        haDeviceState = !haDeviceState;
        controlHA(haDeviceState);
      } else if (sy > 100 && sy < 155) {
        httpDeviceState = !httpDeviceState;
        controlHttp(httpDeviceState);
      } else if (sy > 165 && sy < 220) {
        setRelay(!relayState);
        addLog("手动控制继电器: " + String(relayState ? "ON" : "OFF"));
      } else if (sy > 230 && sy < 285) {
        irLightState = !irLightState;
        irsend.sendNEC(irLightState ? settings.ir_on : settings.ir_off);
      }
      drawControlScreen();
    }
  }
}

// ==================== 统一的待机/睡眠与继电器控制优化逻辑 ====================
void enterStandby() {
  if (isInStandby) return;
  
  // 睡眠前先强行关闭继电器，避免失控
  setRelay(false); 
  addLog("睡眠开始：已优先安全切断继电器");
  
  isInStandby = true;
  addLog("进入待机模式。");
  
  digitalWrite(TFT_BL, LOW);
  tft.fillScreen(ILI9341_BLACK);
  tft.writeCommand(ILI9341_SLPIN);
  
  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  setCpuFrequencyMhz(80);
}

void exitStandby(bool wifiAlreadyConnected) {
  setCpuFrequencyMhz(160);
  
  digitalWrite(TFT_BL, HIGH);
  tft.begin(); 
  tft.setRotation(0);
  tft.writeCommand(ILI9341_SLPOUT);
  
  isInStandby = false;
  lastActivityTime = millis();
  addLog("退出待机模式，重新初始化服务...");
  
  // 过了睡眠区间（即执行正常唤醒时），根据温控设定恢复继电器状态
  if (settings.tempCtrlEnabled) {
    setRelay(false);
    addLog("唤醒恢复：温控开启，保持预关闭，等待温度测量触发");
  } else {
    setRelay(true);
    addLog("唤醒恢复：无温控模式，自动开启继电器状态");
  }
  
  setupWifiAndServices(wifiAlreadyConnected);
}

void setupWifiAndServices(bool wifiAlreadyConnected) {
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

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiAlreadyConnected) { 
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
    server.on("/relay", HTTP_GET, handleRelayCommand); 
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
  
  page.replace("##TEMP_CTRL_CHECKED##", settings.tempCtrlEnabled ? "checked" : "");
  page.replace("##TEMP_THRESHOLD##", String(settings.tempThreshold, 1));
  page.replace("##TEMP_THRESHOLD_OFF##", String(settings.tempThresholdOff, 1)); // 替换关闭温度
  
  page.replace("##DHT_TEMP##", isnan(dhtTemp) ? "--" : String(dhtTemp, 1));
  page.replace("##DHT_HUM##", isnan(dhtHum) ? "--" : String(dhtHum, 1));
  page.replace("##RELAY_STATUS##", relayState ? "开启 (ON)" : "关闭 (OFF)");
  page.replace("##RELAY_COLOR##", relayState ? "#3fb950" : "#da3633");

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

  if (server.hasArg("temp_ctrl")) {
    settings.tempCtrlEnabled = true;
  } else {
    settings.tempCtrlEnabled = false;
  }

  if (server.hasArg("temp_threshold")) {
    settings.tempThreshold = server.arg("temp_threshold").toFloat();
    addLog("温度控制开启阈值已调整。");
  }

  if (server.hasArg("temp_threshold_off")) {
    settings.tempThresholdOff = server.arg("temp_threshold_off").toFloat();
    addLog("温度控制关闭阈值已调整。");
  }
  
  firstTimeSyncDone = false; 
  
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

// ==================== 网页控制继电器接口 ====================
void handleRelayCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String cmd = server.arg("cmd");
  if (cmd.equals("on")) {
    setRelay(true);
    addLog("网页控制：开启继电器");
  } else if (cmd.equals("off")) {
    setRelay(false);
    addLog("网页控制：关闭继电器");
  }
  
  if (currentScreen == SCREEN_CONTROL && !isInStandby) {
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
  String url = "http://" + String(ha_host) + ":" + String(ha_port) + "/api/services/switch/" + (state ? "turn_on" : "turn_off");
  if (http.begin(client, url)) {
    http.addHeader("Authorization", "Bearer " + String(ha_token));
    http.addHeader("Content-Type", "application/json");
    http.POST("{\"entity_id\":\"" + String(ha_entity_id) + "\"}");
    http.end();
  }
}

// ==================== 日志与设置存储 ====================
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
    
    settings.tempCtrlEnabled = false;
    settings.tempThreshold = 28.0;
    settings.tempThresholdOff = 27.0;
    
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
    settings.sleepHour = doc["sleepHour"] | 22; 
    settings.sleepMinute = doc["sleepMinute"] | 0;
    settings.wakeHour = doc["wakeHour"] | 6; 
    settings.wakeMinute = doc["wakeMinute"] | 0;
    settings.ir_on = doc["ir_on"] | DEFAULT_CODE_ON; 
    settings.ir_off = doc["ir_off"] | DEFAULT_CODE_OFF;
    settings.ir_bright_up = doc["ir_bright_up"] | DEFAULT_CODE_BRIGHT_UP; 
    settings.ir_bright_down = doc["ir_bright_down"] | DEFAULT_CODE_BRIGHT_DOWN;
    settings.ir_auto = doc["ir_auto"] | DEFAULT_CODE_AUTO; 
    settings.ir_timer_3h = doc["ir_timer_3h"] | DEFAULT_CODE_TIMER_3H;
    settings.ir_timer_5h = doc["ir_timer_5h"] | DEFAULT_CODE_TIMER_5H; 
    settings.ir_timer_8h = doc["ir_timer_8h"] | DEFAULT_CODE_TIMER_8H;
    strlcpy(settings.weatherCity, doc["weatherCity"] | "zhumadian", sizeof(settings.weatherCity));
    strlcpy(settings.weatherApiKey, doc["weatherApiKey"] | "", sizeof(settings.weatherApiKey));
    
    settings.tempCtrlEnabled = doc["tempCtrlEnabled"].as<bool>();
    settings.tempThreshold = doc["tempThreshold"] | 28.0;
    settings.tempThresholdOff = doc["tempThresholdOff"] | 27.0; // 读取保存的关闭阈值
    
    settings.magic_key = doc["magic_key"] | 80101;
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
  
  doc["tempCtrlEnabled"] = settings.tempCtrlEnabled;
  doc["tempThreshold"] = settings.tempThreshold;
  doc["tempThresholdOff"] = settings.tempThresholdOff; // 保存关闭阈值
  
  doc["magic_key"] = settings.magic_key;

  if (serializeJson(doc, file) == 0) {
    addLog("错误：写入配置文件失败");
  } else {
    addLog("设置已保存到文件。");
  }
  file.close();
}