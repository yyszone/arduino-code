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

// ⭐️ 绝对精准倒计时：优先使用 millis() 毫秒级倒计时，解绑 NTP 网络依赖
long getCooldownRemaining() {
  if (st.tripReason == TripReason::NONE || st.tripReason == TripReason::MANUAL) return 0;

  // 1. 本次运行期间优先使用 millis() 高精度倒计时
  if (st.tripMillis > 0) {
    unsigned long elapsedSec = (millis() - st.tripMillis) / 1000UL;
    if (elapsedSec < settings.cooldownSec) {
      return (long)(settings.cooldownSec - elapsedSec);
    } else {
      return 0;
    }
  }

  // 2. 重启后尝试使用绝绝对时间戳
  if (st.tripEpoch > 0) {
    time_t now_t = time(nullptr);
    if (now_t > 1000000000L) {
      long elapsed = (long)(now_t - st.tripEpoch);
      long rem = (long)settings.cooldownSec - elapsed;
      return rem > 0 ? rem : 0;
    }
  }

  return 0;
}

void executeTrip(TripReason reason) {
    setRelay(false);
    st.faultLatched = true;
    st.tripReason   = reason;
    st.confirmCounter = 0;
    st.tripMillis   = millis(); // 记录跳闸时刻的毫秒数，防止倒计时归零
    
    time_t now_t = time(nullptr);
    st.tripEpoch    = (now_t > 1000000000L) ? now_t : 0;
    saveSystemState();
    
    String rStr = (reason == TripReason::UNDERVOLTAGE) ? "欠压保护" : "低功率保护";
    addLog("INA219 保护切断动作! 原因: " + rStr + " | 电压: " + String(st.busVoltage, 2) + "V");
}

void forceOnSystem() {
    st.faultLatched   = false;
    st.tripReason     = TripReason::NONE;
    st.tripEpoch      = 0;
    st.tripMillis     = 0;
    st.confirmCounter = 0;
    setRelay(true);
    saveSystemState();
    addLog("用户手动强制开启继电器。");
}

void resetSystem() {
    st.faultLatched   = false;
    st.tripReason     = TripReason::NONE;
    st.tripEpoch      = 0;
    st.tripMillis     = 0;
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

// ════════════ 电源与温控逻辑评估 ════════════
void updateRelayLogic() {
  unsigned long now = millis();
  
  if (now < 60000UL) {
    st.confirmCounter = 0;
    return;
  }

  // 1. 如果继电器当前处于断开状态
  if (!st.relayOn) {
    // 自动吸合必须满足：非手动断开 + 电压大于开启阈值 + 冷却倒计时完全归零(<=0)
    if (st.tripReason != TripReason::MANUAL && 
        st.busVoltage > settings.turnOnVoltage && 
        getCooldownRemaining() <= 0) {
      if (++st.confirmCounter >= 3) {
        setRelay(true);
        st.faultLatched = false;
        st.tripReason = TripReason::NONE;
        st.confirmCounter = 0;
        saveSystemState();
        addLog("电压高于阈值且冷却完毕，自动开启继电器。");
      }
    } else {
      st.confirmCounter = 0;
    }
  } 
  // 2. 如果继电器当前处于吸合状态
  else {
    TripReason pending = TripReason::NONE;

    // 欠压判定：电压低于阈值且 > 0.5V
    if (st.busVoltage < settings.underVoltage && st.busVoltage > 0.5f) {
      pending = TripReason::UNDERVOLTAGE;
    } 
    // 低功率保护：开启且吸合满 10 秒后，功率低于阈值
    else if ((settings.underPower > 0.05f) && 
             (now - relayOnTimeMs > 10000UL) && 
             (st.power_mW < settings.underPower * 1000.f)) {
      pending = TripReason::OVERCURRENT; 
    }

    if (pending != TripReason::NONE) {
      if (++st.confirmCounter >= 3) executeTrip(pending);
    } else {
      st.confirmCounter = 0;
    }
  }

  // 3. 温控判定
  if (settings.tempCtrlEnabled && !st.faultLatched && !isnan(dhtTemp)) {
    if (dhtTemp > settings.tempThreshold && !relayState) setRelay(true);
    else if (dhtTemp < settings.tempThresholdOff && relayState) setRelay(false);
  }
}

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