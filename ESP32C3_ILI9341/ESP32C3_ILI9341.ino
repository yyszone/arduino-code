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
#include "dht11.h"

// ==================== 1. 定义全局实体变量 ====================
Settings settings;                
bool haDeviceState = false;
bool httpDeviceState = false;
bool irLightState = false;
bool relayState = false;          // 继电器当前逻辑状态
bool isInStandby = false;
bool lastInSleepWindow = false;   
bool firstTimeSyncDone = false;   // 标记开机后首次网络时间同步与状态初始化是否完成

// 时间戳状态变量
unsigned long lastActivityTime = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastWakeupCheck = 0; 

// 【继电器防噪优化】：记录继电器物理动作的时间戳，初始化为0
unsigned long lastRelaySwitchTime = 0; 

// 传感器缓存变量
float dhtTemp = NAN;
float dhtHum = NAN;

// 日志系统配置
const int MAX_LOG_ENTRIES = 50;
LogEntry logBuffer[MAX_LOG_ENTRIES];
int currentLogIndex = 0;
bool logBufferFull = false;

// 屏幕轮播控制
ScreenMode currentScreen = SCREEN_CONTROL;
unsigned long lastScreenSwitchTime = 0;
bool pauseRotation = false; 

// 天气数据缓存
String weather_main = "NODATA";
String weather_temp = "--";
String weather_desc = "SYSTEM INIT";
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_UPDATE_INTERVAL = 15 * 60 * 1000;

// 时钟局部刷新状态变量
int lastMinute = -1;
int lastSecond = -1;
int lastDay = -1;

// ==================== 2. 声明默认函数（默认参数值只在这里定义一次） ====================
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
bool isRelayTimerActive(); 
void updateRelayLogic();
void setRelay(bool state); 

// ==================== 3. 加载具体的实现头文件 ====================
#include "gui.h"
#include "network_web.h"

// ============== 全局硬件对象实例化 ==============
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS);
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);
IRsend irsend(kIrLedPin);
DHT11_ESP32 dht(DHTPIN); // 实例化本地温湿度传感器对象

// ==================== 统一的继电器硬件控制器（防反偏） ====================
void setRelay(bool state) {
  if (relayState != state) {
    relayState = state;
    digitalWrite(RELAY_PIN, state ? (RELAY_ACTIVE_LOW ? LOW : HIGH) : (RELAY_ACTIVE_LOW ? HIGH : LOW));
    lastRelaySwitchTime = millis(); // 记录硬件动作发生时间，用于防抖判定
  }
}

// ==================== 主程序入口 ====================
void setup() {
  Serial.begin(115200);
  irsend.begin();
  dht.begin(); // 初始化温湿度传感器
  
  // 继电器引脚初始化，开机默认闭合、安全关闭
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false); 
  
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
  
  // 5 秒周期无抢占采集 DHT 并判定温控与定时 (即使待机中同样可以低频稳定后台计算)
  static unsigned long lastDhtRead = 0;
  if (millis() - lastDhtRead > 5000) {
    float t = NAN;
    float h = NAN;
    if (dht.read(t, h)) {
      dhtTemp = t;
      dhtHum = h;
    }
    lastDhtRead = millis();
    updateRelayLogic(); 
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
    // ==================== 【重置崩溃/唤醒循环核心修复】 ====================
    // 待机时静默利用底层 timeClient 的离线 millis() 本地计时。
    // 每 5 秒低功耗检测一次本地时钟，一旦判定过了睡眠区间，执行自动唤醒。
    if (millis() - lastWakeupCheck > 5000) {
      if (!isSleepTime() && timeClient.getEpochTime() > 0) {
        addLog("本地时钟判定：已到达睡眠区间终点，执行自动唤醒。");
        exitStandby(false); // 退出待机，重新拉高屏幕并恢复 Wi-Fi 联网
      }
      lastWakeupCheck = millis();
    }
    delay(200);
  }
}

// ==================== 定时与温控算法优化 ====================

// 判断当前时间是否在屏幕待机时间范围内（【核心修复点】：移除对 WiFi 连接状态的强依赖，离线仍能精准判断睡眠时段）
bool isSleepTime() {
  if (timeClient.getEpochTime() <= 0) {
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

// 判断当前时间是否在继电器定时时间范围内 (独立计算)
bool isRelayTimerActive() {
  if (!settings.relayTimerEnabled) return false;
  // 必须获取过有效网络时间
  if (timeClient.getEpochTime() <= 0) {
    return false; 
  }
  int onM = settings.relayOnHour * 60 + settings.relayOnMinute;
  int offM = settings.relayOffHour * 60 + settings.relayOffMinute;
  int currM = timeClient.getHours() * 60 + timeClient.getMinutes();
  
  if (onM < offM) {
    return (currM >= onM && currM < offM);
  } else {
    return (currM >= onM || currM < offM);
  }
}

// 核心计算：温控优先级大于定时优先级
void updateRelayLogic() {
  // 未联网获取到有效时间前，不做自动动作判定
  if (timeClient.getEpochTime() <= 0) {
    return;
  }

  // 【核心修复点】：仅在已经发生过动作（lastRelaySwitchTime != 0）且动作间隔小于 20 秒时阻断，防止开机首分钟发生控制死锁
  if (lastRelaySwitchTime != 0 && (millis() - lastRelaySwitchTime < 20000)) {
    return;
  }

  bool targetState = relayState; // 默认维持当前物理状态

  // 1. 如果开启了智能温控（温控具有最高优先级）
  if (settings.tempCtrlEnabled) {
    if (!isnan(dhtTemp)) {
      if (dhtTemp > settings.tempThreshold) {
        targetState = true;  // 高于开启阈值 -> 开启继电器
      } else if (dhtTemp < settings.tempThresholdOff) {
        targetState = false; // 低于关闭阈值 -> 关闭继电器
      }
    }
  } 
  // 2. 如果温控未启动，但开启了继电器的专用定时开关
  else if (settings.relayTimerEnabled) {
    targetState = isRelayTimerActive(); // 在设定时间范围内开启，范围外关闭
  }

  // 状态改变判定：若计算出的期望状态与物理电平不符，则执行硬件切换
  if (targetState != relayState) {
    setRelay(targetState);
    addLog("继电器电平自动更新: " + String(relayState ? "ON" : "OFF") + 
           (settings.tempCtrlEnabled ? " (温控高优先级触发)" : " (独立定时触发)"));
    if (currentScreen == SCREEN_CONTROL && !isInStandby) {
      drawControlScreen();
    }
  }
}