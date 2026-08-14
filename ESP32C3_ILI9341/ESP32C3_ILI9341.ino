#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
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
#include "ina219_sensor.h"
#include "dht11.h"
#include "web_pages.h"

// ==================== 全局实体变量定义 ====================
Settings settings;                
SystemState st;

bool haDeviceState = false;
bool httpDeviceState = false;
bool irLightState = false;
bool relayState = false;          
bool isInStandby = false;

unsigned long lastActivityTime = 0;
unsigned long lastStatusUpdate = 0;
unsigned long relayOnTimeMs = 0;

float dhtTemp = NAN;
float dhtHum = NAN;

const int MAX_LOG_ENTRIES = 50;
LogEntry logBuffer[MAX_LOG_ENTRIES];
int currentLogIndex = 0;
bool logBufferFull = false;

// 默认直接显示天气页
ScreenMode currentScreen = SCREEN_WEATHER;
unsigned long lastScreenSwitchTime = 0;
bool pauseRotation = false; 

String weather_main = "NODATA";
String weather_temp = "--";
String weather_desc = "SYSTEM INIT";
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_UPDATE_INTERVAL = 15 * 60 * 1000;

int lastMinute = -1;
int lastSecond = -1;
int lastDay = -1;

// ==================== 函数前置声明 ====================
void loadSettings();
void saveSettings();
void loadSystemState();
void saveSystemState();
void setRelay(bool state);
void executeTrip(TripReason reason);
void forceOnSystem();
void resetSystem();
long getCooldownRemaining();
void updateRelayLogic();

// 全局硬件实例化
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(T_CS);
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);
IRsend irsend(kIrLedPin);
DHT11_ESP32 dht(DHTPIN);
INA219Sensor inaSensor;

#include "gui.h"
#include "network_web.h"

// ==================== 定时与继电器硬件逻辑 ====================
bool isSleepTime() {
  if (timeClient.getEpochTime() <= 0) return false; 
  
  int sleepM = settings.sleepHour * 60 + settings.sleepMinute; 
  int wakeM  = settings.wakeHour * 60 + settings.wakeMinute;   
  int currM  = timeClient.getHours() * 60 + timeClient.getMinutes();

  if (wakeM > sleepM) {
    return (currM >= sleepM && currM < wakeM);
  } else {
    return (currM >= sleepM || currM < wakeM);
  }
}

void setRelay(bool state) {
  relayState = state;
  st.relayOn = state;
  digitalWrite(RELAY_PIN, state ? (RELAY_ACTIVE_LOW ? LOW : HIGH) : (RELAY_ACTIVE_LOW ? HIGH : LOW));
  if (state) relayOnTimeMs = millis();
}

long getCooldownRemaining() {
  if (st.tripEpoch == 0) return 0;
  time_t now_t = time(nullptr);
  if (now_t > 1000000000L) {
      long elapsed = (long)(now_t - st.tripEpoch);
      long rem = (long)settings.cooldownSec - elapsed;
      return rem > 0 ? rem : 0;
  }
  long elapsed = (long)(millis() / 1000UL);
  long rem = (long)settings.cooldownSec - elapsed;
  return rem > 0 ? rem : 0;
}

void executeTrip(TripReason reason) {
    setRelay(false);
    st.faultLatched = true;
    st.tripReason   = reason;
    st.confirmCounter = 0;
    time_t now_t = time(nullptr);
    st.tripEpoch    = (now_t > 1000000000L) ? now_t : 0;
    saveSystemState();
    addLog("INA219 保护跳闸触发！原因代码: " + String((int)reason));
}

void forceOnSystem() {
    st.faultLatched   = false;
    st.tripReason     = TripReason::NONE;
    st.tripEpoch      = 0;
    st.confirmCounter = 0;
    setRelay(true);
    saveSystemState();
    addLog("用户手动强制开启继电器。");
}

void resetSystem() {
    st.faultLatched   = false;
    st.tripReason     = TripReason::NONE;
    st.tripEpoch      = 0;
    st.confirmCounter = 0;
    if (st.busVoltage > settings.turnOnVoltage) {
        setRelay(true);
        addLog("故障已被重置，当前电压满足开启要求，继电器吸合。");
    } else {
        setRelay(false);
        addLog("故障已被重置，但当前电压低于开启阈值，保持关闭。");
    }
    saveSystemState();
}

void loadSystemState() {
  if (!LittleFS.exists(stateFile)) return;
  File file = LittleFS.open(stateFile, "r");
  if (!file) return;
  StaticJsonDocument<512> doc;
  if (!deserializeJson(doc, file)) {
      st.relayOn      = doc["relayOn"] | false; 
      st.faultLatched = doc["faultLatched"] | false;
      st.tripReason   = static_cast<TripReason>(doc["tripReason"] | 0);
      st.tripEpoch    = doc["tripEpoch"] | 0;
      st.cumulativeWh = doc["cumulativeWh"] | 0.0;
  }
  file.close();
}

void saveSystemState() {
  File file = LittleFS.open(stateFile, "w");
  if (!file) return;
  StaticJsonDocument<512> doc;
  doc["relayOn"]      = st.relayOn;
  doc["faultLatched"] = st.faultLatched;
  doc["tripReason"]   = static_cast<uint8_t>(st.tripReason);
  doc["tripEpoch"]    = st.tripEpoch;
  doc["cumulativeWh"] = st.cumulativeWh;
  serializeJson(doc, file); file.close();
}

void updateRelayLogic() {
  unsigned long now = millis();
  
  if (now < 60000UL) {
    st.confirmCounter = 0;
    return;
  }

  if (!st.relayOn) {
    if (st.tripReason != TripReason::MANUAL && st.busVoltage > settings.turnOnVoltage && getCooldownRemaining() <= 0) {
      if (++st.confirmCounter >= 3) {
        setRelay(true);
        st.faultLatched = false;
        st.tripReason = TripReason::NONE;
        st.confirmCounter = 0;
        saveSystemState();
        addLog("电压高于阈值且冷却结束，自动开启继电器。");
      }
    } else {
      st.confirmCounter = 0;
    }
  } else {
    TripReason pending = TripReason::NONE;
    if (st.busVoltage < settings.underVoltage && st.busVoltage > 0.5f) {
      pending = TripReason::UNDERVOLTAGE;
    } else if ((settings.underPower > 0.05f) && (now - relayOnTimeMs > 10000UL) && (st.power_mW < settings.underPower * 1000.f)) {
      pending = TripReason::OVERCURRENT; 
    }

    if (pending != TripReason::NONE) {
      if (++st.confirmCounter >= 3) executeTrip(pending);
    } else {
      st.confirmCounter = 0;
    }
  }

  if (settings.tempCtrlEnabled && !st.faultLatched && !isnan(dhtTemp)) {
    if (dhtTemp > settings.tempThreshold && !relayState) setRelay(true);
    else if (dhtTemp < settings.tempThresholdOff && relayState) setRelay(false);
  }
}

// ==================== Main Setup & Loop ====================
void setup() {
  Serial.begin(115200);
  irsend.begin();
  dht.begin();
  
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false); 
  
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS 挂载失败");
    return;
  }
  
  loadSettings(); 
  loadSystemState();

  st.relayOn = false;
  relayState = false;
  setRelay(false);

  if (!inaSensor.begin()) {
    Serial.println("[INA219] 初始化失败");
  } else {
    Serial.println("[INA219] 硬件上线正常");
  }

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);
  tft.begin();
  ts.begin();
  tft.setRotation(0); 

  exitStandby(false); 
  addLog("ESP32-C3 控制台 v9.2 上电启动。");
}

void loop() {
  handleTouch(); 

  unsigned long now = millis();

  static unsigned long lastSample = 0;
  static unsigned long lastValidDhtTime = 0;

  if (now - lastSample >= 2500) {
    lastSample = now;
    
    inaSensor.update(st); 

    if (st.relayOn) {
      st.todayOnSec += 2;
      st.cumulativeWh += (st.power_mW / 1000.f) * (2.5 / 3600.0);
    }

    float t = NAN, h = NAN;
    bool success = dht.read(t, h);

    if (!success) {
      delay(100);
      success = dht.read(t, h);
    }

    if (success) {
      dhtTemp = t;
      dhtHum = h;
      lastValidDhtTime = now;
    } else {
      if (now - lastValidDhtTime > 30000 && lastValidDhtTime > 0) {
        dhtTemp = NAN;
        dhtHum = NAN;
      }
    }

    updateRelayLogic(); 
  }

  if (!isInStandby) {
    server.handleClient();

    if (now - lastStatusUpdate > 1000) {
      if (WiFi.status() == WL_CONNECTED) timeClient.update();
      if (currentScreen == SCREEN_CLOCK) updateClockTime();
      else updateStatusLine();
      lastStatusUpdate = now;
    }

    // ⭐️ 核心点：限定仅在【天气页】与【时钟页】2 页之间自动轮播
    if (!pauseRotation) {
      if (currentScreen == SCREEN_CONTROL) {
        currentScreen = SCREEN_WEATHER;
        drawCurrentScreen(true);
      }
      if (now - lastScreenSwitchTime > 5000) {
        if (currentScreen == SCREEN_WEATHER) currentScreen = SCREEN_CLOCK;
        else currentScreen = SCREEN_WEATHER;
        drawCurrentScreen(true);
        lastScreenSwitchTime = now;
      }
    } else if (now - lastActivityTime > 10000) {
      // 在控制台界面无操作满 10 秒后，自动恢复天气与时钟两页轮播
      pauseRotation = false;
      currentScreen = SCREEN_WEATHER;
      drawCurrentScreen(true);
    }

    if (WiFi.status() == WL_CONNECTED && (now - lastWeatherUpdate > WEATHER_UPDATE_INTERVAL || lastWeatherUpdate == 0)) {
      updateWeather();
    }

    if (isSleepTime() && (now - lastActivityTime > standbyDelay)) {
      enterStandby();
    }

  } else {
    if (!isSleepTime() && timeClient.getEpochTime() > 0) {
      exitStandby(false);
    }
    delay(200);
  }
}