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
// 用户手动安全断开状态
bool manualOff = false;

// 手动安全断开的计时
unsigned long manualOffMillis = 0;
time_t manualOffEpoch = 0;

unsigned long lastActivityTime = 0;
unsigned long lastStatusUpdate = 0;
unsigned long relayOnTimeMs = 0;

unsigned long lastWifiReconnectAttempt = 0;
bool wifiReconnectRunning = false;

const unsigned long WIFI_RECONNECT_INTERVAL = 30000UL;

// ==================== 电源保护确认计数 ====================
uint8_t underVoltageCounter = 0;
uint8_t underPowerCounter   = 0;

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
bool isRelayTimerActive(); 

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
  if (st.tripReason == TripReason::NONE) return 0;

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

long getManualOffRemaining() {
  if (!manualOff) return 0;

  // ==============================
  // 本次运行期间
  // ==============================
  if (manualOffMillis > 0) {

    unsigned long elapsedSec =
        (millis() - manualOffMillis) / 1000UL;

    if (elapsedSec < settings.cooldownSec) {
      return (long)(settings.cooldownSec - elapsedSec);
    }

    // 冷却结束
    manualOff = false;
    manualOffMillis = 0;
    manualOffEpoch = 0;

    saveSystemState();

    addLog("手动安全断开冷却结束，恢复自动控制。");
    return 0;
  }

  // ==============================
  // 重启后根据真实时间计算
  // ==============================
  if (manualOffEpoch > 0) {

    time_t now_t = time(nullptr);

    if (now_t > 1000000000L) {

      long elapsed =
          (long)(now_t - manualOffEpoch);

      if (elapsed < (long)settings.cooldownSec) {
        return (long)(settings.cooldownSec - elapsed);
      }

      // 冷却结束
      manualOff = false;
      manualOffMillis = 0;
      manualOffEpoch = 0;

      saveSystemState();

      addLog("手动安全断开冷却结束，恢复自动控制。");
      return 0;
    }
  }

  // 没有可靠时间时，继续保持安全关闭
  return settings.cooldownSec;
}

// ============================================================
// INA219 电源保护
// 欠压 / 低功率独立处理，不参与普通自动控制
// ============================================================
bool checkPowerProtection() {

  // ==========================================================
  // 1. 欠压保护
  // ==========================================================
  if (settings.underVoltage > 0.05f &&
      st.busVoltage > 0.5f &&
      st.busVoltage < settings.underVoltage) {

    underVoltageCounter++;
    underPowerCounter = 0;

    Serial.printf(
      "[保护] 欠压 %d/3 | %.3fV < %.3fV\n",
      underVoltageCounter,
      st.busVoltage,
      settings.underVoltage
    );

    if (underVoltageCounter >= 3) {

      Serial.printf(
        "!!! 欠压保护触发：%.3fV < %.3fV\n",
        st.busVoltage,
        settings.underVoltage
      );

      executeTrip(TripReason::UNDERVOLTAGE);

      underVoltageCounter = 0;
      underPowerCounter = 0;

      return true;
    }

  } else {
    underVoltageCounter = 0;
  }

  // ==========================================================
  // 2. 低功率保护
  // ==========================================================
  if (settings.underPower > 0.05f &&
      st.relayOn &&
      relayOnTimeMs > 0 &&
      (millis() - relayOnTimeMs >= 10000UL) &&
      st.power_mW < settings.underPower * 1000.0f) {

    underPowerCounter++;

    Serial.printf(
      "[保护] 低功率 %d/3 | %.3fW < %.3fW\n",
      underPowerCounter,
      st.power_mW / 1000.0f,
      settings.underPower
    );

    if (underPowerCounter >= 3) {

      Serial.printf(
        "!!! 低功率保护触发：%.3fW < %.3fW\n",
        st.power_mW / 1000.0f,
        settings.underPower
      );

      executeTrip(TripReason::OVERCURRENT);

      underPowerCounter = 0;
      underVoltageCounter = 0;

      return true;
    }

  } else {
    underPowerCounter = 0;
  }

  return false;
}


void executeTrip(TripReason reason) {

    // 先断继电器
    setRelay(false);

    // 锁定故障
    st.faultLatched = true;
    st.tripReason = reason;
    st.confirmCounter = 0;

    // 记录时间
    st.tripMillis = millis();

    time_t now_t = time(nullptr);
    st.tripEpoch = (now_t > 1000000000L) ? now_t : 0;

    saveSystemState();

    if (reason == TripReason::UNDERVOLTAGE) {

        Serial.printf(
            "========== 欠压保护 ==========\n"
            "电压：%.3f V\n"
            "阈值：%.3f V\n"
            "继电器：OFF\n"
            "===============================\n",
            st.busVoltage,
            settings.underVoltage
        );

        addLog(
            "欠压保护："
            + String(st.busVoltage, 2)
            + "V < "
            + String(settings.underVoltage, 2)
            + "V，继电器已关闭。"
        );

    } else if (reason == TripReason::OVERCURRENT) {

        Serial.printf(
            "========== 低功率保护 ==========\n"
            "功率：%.3f W\n"
            "阈值：%.3f W\n"
            "继电器：OFF\n"
            "================================\n",
            st.power_mW / 1000.0f,
            settings.underPower
        );

        addLog(
            "低功率保护："
            + String(st.power_mW / 1000.0f, 2)
            + "W < "
            + String(settings.underPower, 2)
            + "W，继电器已关闭。"
        );
    }
}


void forceOnSystem() {
    // 手动开启，同时解除安全断开冷却
    manualOff = false;
    manualOffMillis = 0;
    manualOffEpoch = 0;

    st.faultLatched = false;
    st.tripReason = TripReason::NONE;
    st.tripEpoch = 0;
    st.tripMillis = 0;
    st.confirmCounter = 0;

    setRelay(true);

    saveSystemState();

    addLog("用户手动强制开启继电器。");
}

void manualSafeOff() {

    manualOff = true;
    manualOffMillis = millis();

    time_t now_t = time(nullptr);
    manualOffEpoch =
        (now_t > 1000000000L) ? now_t : 0;

    // 手动关闭本身不是故障
    st.confirmCounter = 0;

    // 只有当前没有真正故障时，才清除 NONE
    if (!st.faultLatched) {
        st.tripReason = TripReason::NONE;
        st.tripEpoch = 0;
        st.tripMillis = 0;
    }

    setRelay(false);

    saveSystemState();

    addLog("用户手动安全断开，进入 " +
           String(settings.cooldownSec) +
           " 秒冷却锁定。");
}

void resetSystem() {
    manualOff = false;
    manualOffMillis = 0;
    manualOffEpoch = 0;

    st.faultLatched = false;
    st.tripReason = TripReason::NONE;
    st.tripEpoch = 0;
    st.tripMillis = 0;
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

      st.relayOn =
          doc["relayOn"] | false;

      st.faultLatched =
          doc["faultLatched"] | false;

      st.tripReason =
          static_cast<TripReason>(
              doc["tripReason"] | 0);

      st.tripEpoch =
          doc["tripEpoch"] | 0;

      st.cumulativeWh =
          doc["cumulativeWh"] | 0.0;

      // 恢复手动安全断开
      manualOff =
          doc["manualOff"] | false;

      manualOffEpoch =
          doc["manualOffEpoch"] | 0;

      manualOffMillis = 0;
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

    // 手动安全断开状态
    doc["manualOff"] = manualOff;
    doc["manualOffEpoch"] = manualOffEpoch;

    serializeJson(doc, file);
    file.close();
}

// ════════════ 电源与温控逻辑评估 ════════════
// ============================================================
// 继电器自动控制
// 注意：INA219 欠压 / 低功率保护已经独立处理
// ============================================================
// ============================================================
// 判定当前时间是否处于定时开启时段
// ============================================================
bool isRelayTimerActive() {
  if (!settings.relayTimerEnabled || timeClient.getEpochTime() <= 0) {
    return false;
  }
  
  int onMinutes  = settings.relayOnHour * 60 + settings.relayOnMinute;
  int offMinutes = settings.relayOffHour * 60 + settings.relayOffMinute;
  int curMinutes = timeClient.getHours() * 60 + timeClient.getMinutes();

  if (onMinutes < offMinutes) {
    // 正常时段 (例如 08:00 到 22:00)
    return (curMinutes >= onMinutes && curMinutes < offMinutes);
  } else if (onMinutes > offMinutes) {
    // 跨天时段 (例如 22:00 到次日 08:00)
    return (curMinutes >= onMinutes || curMinutes < offMinutes);
  }
  return false; // 开关时间相同视为无效
}

// ============================================================
// 继电器核心自动控制评估
// ============================================================
void updateRelayLogic() {
  unsigned long now = millis();
  
  // 1. 开机 60 秒预热保护
  if (now < 60000UL) {
    st.confirmCounter = 0;
    return;
  }

  // 2. 硬件保护切断检测 (最高优先级)
  if (st.relayOn && checkPowerProtection()) {
    return;
  }

  // 3. 故障冷却或手动安全断开锁定时，严禁任何自动开启
  if (st.faultLatched && getCooldownRemaining() > 0) {
    st.confirmCounter = 0;
    return;
  }
  if (manualOff || getManualOffRemaining() > 0) {
    st.confirmCounter = 0;
    return;
  }
  if (st.tripReason == TripReason::MANUAL) {
    st.confirmCounter = 0;
    return;
  }

  // ==========================================================
  // 4. 计算业务期望状态 (期望开启 true，期望关闭 false)
  // ==========================================================
  bool desiredState = st.relayOn; // 默认维持当前状态

  if (settings.tempCtrlEnabled) {
    // --- 模式 A：启用了智能温控 (回差控制，防止临界抖动) ---
    if (!isnan(dhtTemp)) {
      if (dhtTemp >= settings.tempThreshold) {
        desiredState = true;
      } else if (dhtTemp <= settings.tempThresholdOff) {
        desiredState = false;
      }
    }
    // 如果同时开启了定时，则定时作为总使能条件 (定时外强制关闭)
    if (settings.relayTimerEnabled && !isRelayTimerActive()) {
      desiredState = false;
    }

  } else if (settings.relayTimerEnabled) {
    // --- 模式 B：未启用温控，仅启用了定时开关 ---
    desiredState = isRelayTimerActive();

  } else {
    // --- 模式 C：未启用温控与定时，走默认 INA219 电压吸合策略 ---
    // 达到开启阈值时吸合；若低于开启阈值则保持现态（低于欠压由保护层切断）
    if (st.busVoltage > settings.turnOnVoltage) {
      desiredState = true;
    }
  }

  // ==========================================================
  // 5. 执行控制与多重防抖确认
  // ==========================================================
  if (desiredState) {
    // 期望开启：必须满足电压安全前置条件 (未发生欠压且大于启动电压)
    if (st.busVoltage > settings.turnOnVoltage) {
      if (!st.relayOn) {
        if (++st.confirmCounter >= 3) { // 连续 3 次采样确认开启
          setRelay(true);
          st.faultLatched = false;
          st.tripReason = TripReason::NONE;
          st.tripEpoch = 0;
          st.tripMillis = 0;
          st.confirmCounter = 0;
          saveSystemState();
          addLog("触发自动控制条件，继电器已吸合。");
        }
      } else {
        st.confirmCounter = 0;
      }
    } else {
      st.confirmCounter = 0;
    }
  } else {
    // 期望关闭：立即响应
    st.confirmCounter = 0;
    if (st.relayOn) {
      setRelay(false);
      saveSystemState();
      addLog("触发控制退出条件(温控/定时)，继电器已断开。");
    }
  }
}

void maintainWiFi() {

  if (isInStandby) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (wifiReconnectRunning) {
    return;
  }

  if (millis() - lastWifiReconnectAttempt <
      WIFI_RECONNECT_INTERVAL) {
    return;
  }

  lastWifiReconnectAttempt = millis();
  wifiReconnectRunning = true;

  Serial.println();
  Serial.println("[WiFi] 检测到断线，开始自动重连...");

  Serial.printf(
    "[WiFi] 当前状态码: %d\n",
    WiFi.status()
  );

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  WiFi.begin(ssid, password);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 15000UL) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("[WiFi] 自动重连成功！");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("[WiFi] RSSI: ");
    Serial.println(WiFi.RSSI());

    timeClient.begin();
    timeClient.update();

    // 确保 WebServer 继续监听
    server.begin();

    addLog(
      "WiFi 自动重连成功，IP: " +
      WiFi.localIP().toString()
    );

  } else {

    Serial.printf(
      "[WiFi] 自动重连失败，status=%d\n",
      WiFi.status()
    );
  }

  wifiReconnectRunning = false;
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
  tft.begin(20000000);
  ts.begin();
  tft.setRotation(0);

  exitStandby(false); 
  addLog("ESP32-C3 控制台 v9.3 上电启动。");
}

void loop() {
  handleTouch(); 

  unsigned long now = millis();
  maintainWiFi();
  static unsigned long lastSample = 0;
  static unsigned long lastValidDhtTime = 0;

  if (now - lastSample >= 2500) {
      lastSample = now;

      inaSensor.update(st);

      if (st.relayOn) {
          st.todayOnSec += 2;
          st.cumulativeWh +=
              (st.power_mW / 1000.f) * (2.5 / 3600.0);
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
          if (now - lastValidDhtTime > 30000 &&
              lastValidDhtTime > 0) {

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