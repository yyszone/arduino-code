// =================================================================================================
// ==   ESP32C3 智能风扇 & 双路定时插座 & NAS控制器 v4.8.0                         ==
// =================================================================================================
// 更新日志 v4.8.0 (基于 v4.7.1):
//   [新增] RELAY4_PIN — NAS主机专用继电器，默认开启。
//          操作需在网页输入密码 "123456" 方可执行，防止误触。
//          同步支持 API: /setNasRelay?state=[0|1]&pwd=123456
//   [新增] 双路插座通电后立即开启 40 分钟，基于 millis() 实现，
//          无论是否有 WiFi / NTP 均可正常工作。
//   [新增] WiFi 连接加入 30 秒超时机制，超时后以离线模式运行，
//          40分钟定时仍然有效；获取到 WiFi 后路由器/NTP 正常工作。
//   [重构] HTML 页面拆分为独立头文件:
//          main_html.h   — 风扇控制主页
//          socket_html.h — 双路插座 & NAS 控制页
//   [保留] 其余所有 v4.7.1 逻辑不变。
// =================================================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <esp_netif.h>
#include <arpa/inet.h>
#include <time.h>

// 拆分出的 HTML 头文件
#include "main_html.h"
#include "socket_html.h"

// ============== 用户配置 ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp32-smart-fan";
const int   WEB_SERVER_PORT = 15715;

// NAS 继电器操作密码
const char* NAS_PASSWORD = "123456";

// ============== 硬件引脚配置 ==============
// --- 风扇部分 ---
const int RELAY_PIN   = 1;   // 风扇主电源继电器
const int PWM_PIN     = 5;
const int TACH_PIN    = 4;
const int I2C_SDA_PIN = 10;
const int I2C_SCL_PIN = 8;
// --- 双路插座部分 ---
const int RELAY2_PIN  = 6;   // 插座1继电器
const int RELAY3_PIN  = 7;   // 插座2继电器
// --- NAS 主机电源 ---
// ⚠️ 请确认 GPIO2 在您的板子上可用，如有冲突请修改为其他空闲引脚
const int RELAY4_PIN  = 2;   // NAS 主机电源继电器（默认开启）

// ============== PWM/LEDC 配置 (风扇) ==============
const int  LEDC_FREQUENCY    = 25000;
const int  LEDC_RES_BITS     = 8;
const int  PWM_MAX           = (1 << LEDC_RES_BITS) - 1;
const bool PWM_INVERTED      = false;

// ============== RPM 采样配置 (风扇) ==============
const int    PULSES_PER_REV        = 2;
const uint32_t MIN_PULSE_INTERVAL_US = 800;
const int    MAX_REASONABLE_RPM    = 15000;

// ============== 继电器与智能电源管理配置 (风扇) ==============
float VOLTAGE_THRESHOLD      = 3.5;
float VOLTAGE_HIGH_THRESHOLD = 4.2;
const long LOCKOUT_DURATION_MS = 3600000;  // 1 小时

// ============== 时间与定时任务配置 ==============
const char* NTP_SERVER   = "ntp.aliyun.com";
const long  GMT_OFFSET_SEC = 8 * 3600;
WiFiUDP    ntpUDP;
NTPClient  timeClient(ntpUDP, NTP_SERVER, GMT_OFFSET_SEC);

// --- 风扇定时任务 ---
struct TimerTask { int hour; int minute; bool action; bool enabled; };
TimerTask tasks[2] = { {0,0,false,false}, {0,0,false,false} };

// --- 插座定时任务 ---
const int MAX_SOCKET_SCHEDULES = 10;
struct SocketSchedule { int hour; int minute; int second; bool action; bool enabled; };
SocketSchedule relay2_schedules[MAX_SOCKET_SCHEDULES];
SocketSchedule relay3_schedules[MAX_SOCKET_SCHEDULES];
int relay2_schedule_count = 0;
int relay3_schedule_count = 0;

// ============== 电量统计配置 (风扇) ==============
const int HISTORY_DAYS = 7;

// ============== 全局对象与变量 ==============
WebServer     server(WEB_SERVER_PORT);
Adafruit_INA219 ina219;
Preferences   preferences;

// --- WiFi 状态 ---
bool wifiAvailable = false;

// --- 启动定时 (40分钟, 基于 millis, 无需 WiFi) ---
const unsigned long BOOT_ON_DURATION_MS = 40UL * 60UL * 1000UL;  // 40 分钟
unsigned long bootTime      = 0;
bool          bootTimerActive = false;

// --- 风扇相关变量 ---
volatile uint32_t pulseCount      = 0;
volatile uint32_t lastPulseMicros = 0;
uint32_t  lastRpmCalcMs           = 0;
int       fanSliderValue          = 0;
int       lastRpm                 = 0;
float     loadVoltage = 0, current_mA = 0, power_mW = 0;
float     lockoutTriggerVoltage   = 0.0;
long      lastRunDurationMinutes  = 0;
unsigned long lastRunStartTime    = 0;
char      lockoutStopTime[6]      = "--:--";
bool      ina219_ok               = false;
bool      relayState              = false;   // 风扇继电器
bool      isLockedOut             = false;
unsigned long lockoutStartTime    = 0;

// --- 风扇电量统计 ---
float    dailyEnergyWh[HISTORY_DAYS] = {0};
float    todayEnergyWh               = 0;
unsigned long lastEnergyCalcMs       = 0;
int      lastDayChecked              = -1;

// --- 插座继电器状态 ---
bool relay2State = false;  // 插座1
bool relay3State = false;  // 插座2
bool relay4State = false;  // NAS 主机电源


// =======================================================
// ============== 中断 & 计算函数 ==============
// =======================================================

void IRAM_ATTR tachISR() {
  uint32_t now = micros();
  if (now - lastPulseMicros >= MIN_PULSE_INTERVAL_US) {
    pulseCount++;
    lastPulseMicros = now;
  }
}

int computeRPM() {
  uint32_t now     = millis();
  uint32_t elapsed = now - lastRpmCalcMs;
  if (elapsed < 1000) return -1;
  noInterrupts();
  uint32_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();
  lastRpmCalcMs = now;
  if (elapsed == 0) return 0;
  uint32_t rpm = (uint32_t)((uint64_t)pulses * 60000ULL / (elapsed * PULSES_PER_REV));
  if (rpm > (uint32_t)MAX_REASONABLE_RPM) return lastRpm;
  lastRpm = (int)rpm;
  return lastRpm;
}

void sampleINA219() {
  if (!ina219_ok) return;
  loadVoltage = ina219.getBusVoltage_V() + (ina219.getShuntVoltage_mV() / 1000.0f);
  current_mA  = ina219.getCurrent_mA();
  power_mW    = ina219.getPower_mW();
}


// =======================================================
// ============== 继电器控制函数 ==============
// =======================================================

void setRelay(bool state, bool manualOverride = false) {
  if (isLockedOut && state && manualOverride) {
    Serial.println("!!! 管理员手动覆盖低压锁定 !!!");
    isLockedOut = false;
  }
  if (isLockedOut && state) {
    Serial.println("继电器处于锁定状态，自动开启请求被拒绝。");
    return;
  }
  if (state && !relayState) lastRunStartTime = millis();
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  Serial.printf("风扇继电器 (Pin %d): %s\n", RELAY_PIN, relayState ? "ON" : "OFF");
}

void setSocketRelay(int internalRelayNum, bool state) {
  if (internalRelayNum == 2) {
    relay2State = state;
    digitalWrite(RELAY2_PIN, state ? HIGH : LOW);
    Serial.printf("插座1继电器 (Pin %d): %s\n", RELAY2_PIN, state ? "ON" : "OFF");
  } else if (internalRelayNum == 3) {
    relay3State = state;
    digitalWrite(RELAY3_PIN, state ? HIGH : LOW);
    Serial.printf("插座2继电器 (Pin %d): %s\n", RELAY3_PIN, state ? "ON" : "OFF");
  }
}

// NAS 继电器无调度逻辑，只能手动操作（需密码）
void setNasRelay(bool state) {
  relay4State = state;
  // NC接法：state=true(NAS供电) → 继电器不通电 → LOW
  //          state=false(NAS断电) → 继电器通电   → HIGH
  digitalWrite(RELAY4_PIN, state ? LOW : HIGH);
  Serial.printf("NAS继电器 (Pin %d) [NC接法]: %s\n", RELAY4_PIN, state ? "ON(NAS供电)" : "OFF(NAS断电)");
}


// =======================================================
// ============== IPv6 工具函数 ==============
// =======================================================

String formatIPv6(const esp_ip6_addr_t *addr) {
  if (!addr) return String("::");
  char buf[40];
  uint32_t w0 = ntohl(addr->addr[0]), w1 = ntohl(addr->addr[1]);
  uint32_t w2 = ntohl(addr->addr[2]), w3 = ntohl(addr->addr[3]);
  snprintf(buf, sizeof(buf), "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
    (uint16_t)(w0>>16),(uint16_t)(w0&0xFFFF),(uint16_t)(w1>>16),(uint16_t)(w1&0xFFFF),
    (uint16_t)(w2>>16),(uint16_t)(w2&0xFFFF),(uint16_t)(w3>>16),(uint16_t)(w3&0xFFFF));
  return String(buf);
}

String getIPv6() {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return String("Not Available");
  esp_ip6_addr_t addr;
  if (esp_netif_get_ip6_global(netif, &addr) == ESP_OK)
    if (addr.addr[0]||addr.addr[1]||addr.addr[2]||addr.addr[3]) return formatIPv6(&addr);
  if (esp_netif_get_ip6_linklocal(netif, &addr) == ESP_OK)
    if (addr.addr[0]||addr.addr[1]||addr.addr[2]||addr.addr[3]) return formatIPv6(&addr);
  return String("Not Available");
}


// =======================================================
// ============== Web Handler 函数 ==============
// =======================================================

// ---------- 风扇页面 ----------
void handleRoot()     { server.send(200, "text/html; charset=UTF-8", MAIN_HTML); }

void handleGetData() {
  int currentRpm = computeRPM();
  sampleINA219();
  if (wifiAvailable) timeClient.update();

  String t = wifiAvailable ? timeClient.getFormattedTime() : "--:--:--";
  String json = "{";
  json += "\"rpm\":"              + String(currentRpm < 0 ? lastRpm : currentRpm) + ",";
  json += "\"voltage\":"          + String(loadVoltage)   + ",";
  json += "\"current\":"          + String(current_mA)    + ",";
  json += "\"power\":"            + String(power_mW)      + ",";
  json += "\"relay\":"            + String(relayState  ? "true":"false") + ",";
  json += "\"lockout\":"          + String(isLockedOut ? "true":"false") + ",";
  json += "\"lockout_trigger_v\":" + String(lockoutTriggerVoltage) + ",";
  json += "\"last_run_duration\":" + String(lastRunDurationMinutes) + ",";
  json += "\"lockout_stop_time\":\"" + String(lockoutStopTime) + "\",";
  if (isLockedOut) {
    long rem = LOCKOUT_DURATION_MS - (long)(millis() - lockoutStartTime);
    json += "\"lockout_rem\":" + String(rem / 60000) + ",";
  } else {
    json += "\"lockout_rem\":0,";
  }
  json += "\"time\":\"" + t + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleGetStatus() {
  String json = "{";
  json += "\"speed\":"  + String(fanSliderValue) + ",";
  json += "\"relay\":"  + String(relayState ? "true":"false") + ",";
  json += "\"tasks\":[";
  for (int i = 0; i < 2; i++) {
    json += "{\"enabled\":" + String(tasks[i].enabled?"true":"false");
    json += ",\"hour\":"    + String(tasks[i].hour);
    json += ",\"minute\":"  + String(tasks[i].minute);
    json += ",\"action\":"  + String(tasks[i].action?"true":"false") + "}";
    if (i == 0) json += ",";
  }
  json += "],";
  json += "\"low_voltage_threshold\":"  + String(VOLTAGE_THRESHOLD)      + ",";
  json += "\"high_voltage_threshold\":" + String(VOLTAGE_HIGH_THRESHOLD);
  json += "}";
  server.send(200, "application/json", json);
}

void handleGetPowerStats() {
  String json = "{\"today\":" + String(todayEnergyWh, 4) + ",\"history\":[";
  for (int i = 0; i < HISTORY_DAYS; i++) {
    json += String(dailyEnergyWh[i], 4);
    if (i < HISTORY_DAYS - 1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleSetSpeed() {
  if (server.hasArg("value")) {
    fanSliderValue = server.arg("value").toInt();
    int duty = PWM_INVERTED ? (PWM_MAX - fanSliderValue) : fanSliderValue;
    ledcWrite(PWM_PIN, duty);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}

void handleSetRelay() {
  if (server.hasArg("state")) {
    setRelay(server.arg("state").toInt() == 1, true);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}

void handleSetTimers() {
  for (int i = 0; i < 2; i++) {
    String en_s   = "t" + String(i+1) + "_en";
    String time_s = "t" + String(i+1) + "_time";
    String act_s  = "t" + String(i+1) + "_act";
    if (server.hasArg(en_s) && server.hasArg(time_s) && server.hasArg(act_s)) {
      tasks[i].enabled = (server.arg(en_s) == "1");
      String tv = server.arg(time_s);
      tasks[i].hour    = tv.substring(0,2).toInt();
      tasks[i].minute  = tv.substring(3,5).toInt();
      tasks[i].action  = (server.arg(act_s) == "1");
    }
  }
  server.send(200, "text/plain", "Timers Saved");
}

void saveVoltageConfig() {
  preferences.begin("fan-config", false);
  preferences.putFloat("lowV_thresh",  VOLTAGE_THRESHOLD);
  preferences.putFloat("highV_thresh", VOLTAGE_HIGH_THRESHOLD);
  preferences.end();
  Serial.println("[OK] 电压阈值已保存到闪存。");
}

void handleSetVoltageThresholds() {
  if (server.hasArg("low") && server.hasArg("high")) {
    VOLTAGE_THRESHOLD      = server.arg("low").toFloat();
    VOLTAGE_HIGH_THRESHOLD = server.arg("high").toFloat();
    saveVoltageConfig();
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}

// ---------- 插座页面 ----------
void saveSocketSchedules();  // 前向声明

void handleSocketPage() {
  String page = FPSTR(SOCKET_HTML);
  page.replace("##R2_PIN##", String(RELAY2_PIN));
  page.replace("##R3_PIN##", String(RELAY3_PIN));
  page.replace("##R4_PIN##", String(RELAY4_PIN));
  server.send(200, "text/html; charset=UTF-8", page);
}

void handleGetSocketData() {
  if (wifiAvailable) timeClient.update();
  String t = wifiAvailable ? timeClient.getFormattedTime() : "--:--:--";

  // 计算启动定时剩余秒数
  long bootRemainS = 0;
  if (bootTimerActive) {
    long elapsed = (long)(millis() - bootTime);
    long remain  = (long)BOOT_ON_DURATION_MS - elapsed;
    bootRemainS  = remain > 0 ? remain / 1000L : 0;
  }

  String json = "{";
  json += "\"time\":\"" + t + "\",";
  json += "\"relay2\":" + String(relay2State ? "true":"false") + ",";
  json += "\"relay3\":" + String(relay3State ? "true":"false") + ",";
  json += "\"relay4\":" + String(relay4State ? "true":"false") + ",";
  json += "\"boot_timer_active\":" + String(bootTimerActive ? "true":"false") + ",";
  json += "\"boot_timer_remain_s\":" + String(bootRemainS) + ",";

  json += "\"r2_tasks\":[";
  for (int i = 0; i < relay2_schedule_count; i++) {
    json += "{\"h\":" + String(relay2_schedules[i].hour)
          + ",\"m\":" + String(relay2_schedules[i].minute)
          + ",\"s\":" + String(relay2_schedules[i].second)
          + ",\"a\":" + String(relay2_schedules[i].action ? 1:0) + "}";
    if (i < relay2_schedule_count - 1) json += ",";
  }
  json += "],\"r3_tasks\":[";
  for (int i = 0; i < relay3_schedule_count; i++) {
    json += "{\"h\":" + String(relay3_schedules[i].hour)
          + ",\"m\":" + String(relay3_schedules[i].minute)
          + ",\"s\":" + String(relay3_schedules[i].second)
          + ",\"a\":" + String(relay3_schedules[i].action ? 1:0) + "}";
    if (i < relay3_schedule_count - 1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleSetSocketTask() {
  if (!server.hasArg("cmd") || !server.hasArg("relay")) {
    server.send(400, "text/plain", "Bad Request: Missing cmd or relay"); return;
  }
  int relayIndex = server.arg("relay").toInt();
  int internalNum = 0;
  if      (relayIndex == 1) internalNum = 2;
  else if (relayIndex == 2) internalNum = 3;
  else { server.send(400, "text/plain", "Bad Request: relay must be 1 or 2"); return; }

  String cmd = server.arg("cmd");

  if (cmd == "manual" && server.hasArg("state")) {
    setSocketRelay(internalNum, server.arg("state").toInt() == 1);
  }
  else if (cmd == "add" && server.hasArg("action") && server.hasArg("time")) {
    int cnt = (internalNum == 2) ? relay2_schedule_count : relay3_schedule_count;
    if (cnt >= MAX_SOCKET_SCHEDULES) { server.send(400,"text/plain","Schedule full"); return; }
    SocketSchedule ns;
    ns.action  = (server.arg("action").toInt() == 1);
    ns.enabled = true;
    String tv  = server.arg("time");
    ns.hour    = tv.substring(0,2).toInt();
    ns.minute  = tv.substring(3,5).toInt();
    ns.second  = (tv.length() > 5) ? tv.substring(6,8).toInt() : 0;
    if (internalNum == 2) relay2_schedules[relay2_schedule_count++] = ns;
    else                  relay3_schedules[relay3_schedule_count++] = ns;
    saveSocketSchedules();
  }
  else if (cmd == "delete" && server.hasArg("time")) {
    String tv = server.arg("time");
    int h = tv.substring(0,2).toInt(), m = tv.substring(3,5).toInt();
    int s = (tv.length() > 5) ? tv.substring(6,8).toInt() : 0;
    int idx = -1;
    if (internalNum == 2) {
      for (int i = 0; i < relay2_schedule_count; i++)
        if (relay2_schedules[i].hour==h && relay2_schedules[i].minute==m && relay2_schedules[i].second==s)
          { idx=i; break; }
      if (idx != -1) { for (int i=idx; i<relay2_schedule_count-1; i++) relay2_schedules[i]=relay2_schedules[i+1]; relay2_schedule_count--; }
    } else {
      for (int i = 0; i < relay3_schedule_count; i++)
        if (relay3_schedules[i].hour==h && relay3_schedules[i].minute==m && relay3_schedules[i].second==s)
          { idx=i; break; }
      if (idx != -1) { for (int i=idx; i<relay3_schedule_count-1; i++) relay3_schedules[i]=relay3_schedules[i+1]; relay3_schedule_count--; }
    }
    saveSocketSchedules();
  }
  server.send(200, "text/plain", "OK");
}

void handleDeleteAllTasks() {
  if (!server.hasArg("relay")) { server.send(400,"text/plain","Missing relay"); return; }
  int relayIndex = server.arg("relay").toInt();
  if      (relayIndex == 1) { relay2_schedule_count = 0; Serial.println("已清除插座1全部任务。"); }
  else if (relayIndex == 2) { relay3_schedule_count = 0; Serial.println("已清除插座2全部任务。"); }
  else { server.send(400,"text/plain","Invalid relay"); return; }
  saveSocketSchedules();
  server.send(200, "text/plain", "All tasks deleted.");
}

void handleApiAddTask() {
  if (!server.hasArg("ledPwm") || !server.hasArg("time") || !server.hasArg("relay")) {
    server.send(400, "text/plain", "Bad Request: Missing ledPwm / time / relay"); return;
  }
  int action = server.arg("ledPwm").toInt();
  if (action != 0 && action != 1) { server.send(400,"text/plain","ledPwm must be 0 or 1"); return; }
  int relayIndex = server.arg("relay").toInt();
  int internalNum = 0;
  if      (relayIndex == 1) internalNum = 2;
  else if (relayIndex == 2) internalNum = 3;
  else { server.send(400,"text/plain","relay must be 1 or 2"); return; }
  String tv = server.arg("time");
  if (tv.length() < 5) { server.send(400,"text/plain","Invalid time format"); return; }
  int cnt = (internalNum == 2) ? relay2_schedule_count : relay3_schedule_count;
  if (cnt >= MAX_SOCKET_SCHEDULES) { server.send(409,"text/plain","Schedule full"); return; }
  SocketSchedule ns;
  ns.action  = (action == 1);
  ns.enabled = true;
  ns.hour    = tv.substring(0,2).toInt();
  ns.minute  = tv.substring(3,5).toInt();
  ns.second  = (tv.length() > 5) ? tv.substring(6,8).toInt() : 0;
  if (internalNum == 2) relay2_schedules[relay2_schedule_count++] = ns;
  else                  relay3_schedules[relay3_schedule_count++] = ns;
  saveSocketSchedules();
  String msg = "Task added for relay " + String(relayIndex) + " at " + tv;
  Serial.println(msg);
  server.send(200, "text/plain", msg);
}

// ---------- NAS 继电器控制 Handler ----------
void handleSetNasRelay() {
  // 必须携带 state 和 pwd 参数
  if (!server.hasArg("state") || !server.hasArg("pwd")) {
    server.send(400, "text/plain", "Bad Request: Missing state or pwd");
    return;
  }
  // 密码校验
  if (server.arg("pwd") != String(NAS_PASSWORD)) {
    Serial.printf("[WARN] NAS继电器操作失败: 密码错误 (来自 %s)\n",
                  server.client().remoteIP().toString().c_str());
    server.send(403, "text/plain", "Forbidden: Wrong Password");
    return;
  }
  bool state = (server.arg("state").toInt() == 1);
  setNasRelay(state);
  server.send(200, "text/plain", String("NAS relay ") + (state ? "ON" : "OFF"));
}

// ---------- 系统信息 ----------
String getSystemInfoJSON() {
  String j = "{";
  j += "\"chip_model\":\"" + String(ESP.getChipModel()) + "\",";
  j += "\"chip_rev\":"     + String(ESP.getChipRevision()) + ",";
  j += "\"cpu_freq_mhz\":" + String(ESP.getCpuFreqMHz())  + ",";
  j += "\"free_heap\":"    + String(ESP.getFreeHeap())     + ",";
  j += "\"ip\":\""         + WiFi.localIP().toString()     + "\",";
  j += "\"ipv6\":\""       + getIPv6() + "\"";
  j += "}";
  return j;
}
void handleSysInfo() { server.send(200, "application/json", getSystemInfoJSON()); }

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial); break;
    case UPLOAD_FILE_WRITE: if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial); break;
    case UPLOAD_FILE_END:   if (!Update.end(true)) Update.printError(Serial); break;
  }
  yield();
}


// =======================================================
// ============== 定时任务检查 ==============
// =======================================================

void checkFanTimers() {
  if (!wifiAvailable) return;
  timeClient.update();
  int h = timeClient.getHours(), m = timeClient.getMinutes();
  for (int i = 0; i < 2; i++) {
    if (tasks[i].enabled && tasks[i].hour == h && tasks[i].minute == m) {
      if (relayState != tasks[i].action) {
        Serial.printf("执行风扇定时任务 %d: %s\n", i+1, tasks[i].action?"ON":"OFF");
        setRelay(tasks[i].action);
      }
    }
  }
}

void checkSocketSchedules() {
  if (!wifiAvailable) return;
  timeClient.update();
  int h = timeClient.getHours(), m = timeClient.getMinutes(), s = timeClient.getSeconds();
  for (int i = 0; i < relay2_schedule_count; i++) {
    if (relay2_schedules[i].enabled &&
        relay2_schedules[i].hour   == h &&
        relay2_schedules[i].minute == m &&
        relay2_schedules[i].second == s) {
      if (relay2State != relay2_schedules[i].action) {
        Serial.printf("执行插座1定时: %s\n", relay2_schedules[i].action?"ON":"OFF");
        setSocketRelay(2, relay2_schedules[i].action);
      }
    }
  }
  for (int i = 0; i < relay3_schedule_count; i++) {
    if (relay3_schedules[i].enabled &&
        relay3_schedules[i].hour   == h &&
        relay3_schedules[i].minute == m &&
        relay3_schedules[i].second == s) {
      if (relay3State != relay3_schedules[i].action) {
        Serial.printf("执行插座2定时: %s\n", relay3_schedules[i].action?"ON":"OFF");
        setSocketRelay(3, relay3_schedules[i].action);
      }
    }
  }
}


// =======================================================
// ============== 智能电源管理 (风扇) ==============
// =======================================================

void checkVoltageProtection() {
  if (!ina219_ok) return;
  if (isLockedOut) {
    if (millis() - lockoutStartTime >= (unsigned long)LOCKOUT_DURATION_MS) {
      Serial.println("锁定已到期，检查电压...");
      sampleINA219();
      if (loadVoltage >= VOLTAGE_HIGH_THRESHOLD) {
        Serial.printf("电压恢复 (%.2fV)，自动重新开启。\n", loadVoltage);
        isLockedOut = false; setRelay(true);
      } else {
        Serial.printf("电压仍不足 (%.2fV)，解除锁定但保持关闭。\n", loadVoltage);
        isLockedOut = false;
      }
    }
    return;
  }
  if (!relayState && VOLTAGE_HIGH_THRESHOLD > 0) {
    sampleINA219();
    if (loadVoltage >= VOLTAGE_HIGH_THRESHOLD) {
      Serial.printf("高压触发 (%.2fV)，自动开启继电器。\n", loadVoltage);
      setRelay(true);
    }
  }
  if (relayState) {
    sampleINA219();
    if (loadVoltage > 0.1f && loadVoltage < VOLTAGE_THRESHOLD) {
      Serial.printf("!!! 低压保护触发: %.2fV < %.2fV\n", loadVoltage, VOLTAGE_THRESHOLD);
      if (wifiAvailable) {
        timeClient.update();
        String ft = timeClient.getFormattedTime();
        snprintf(lockoutStopTime, 6, "%s", ft.substring(0,5).c_str());
      }
      lastRunDurationMinutes = (millis() - lastRunStartTime) / 60000;
      isLockedOut        = true;
      lockoutStartTime   = millis();
      lockoutTriggerVoltage = loadVoltage;
      setRelay(false);
    }
  }
}


// =======================================================
// ============== 电量统计 (风扇) ==============
// =======================================================

void accumulateEnergy() {
  if (!ina219_ok) { lastEnergyCalcMs = millis(); return; }
  unsigned long now = millis();
  unsigned long elapsed = now - lastEnergyCalcMs;
  if (elapsed > 0) {
    double hrs   = (double)elapsed / 3600000.0;
    double watts = (double)power_mW / 1000.0;
    todayEnergyWh += (float)(watts * hrs);
  }
  lastEnergyCalcMs = now;
}

void checkDailyRollover() {
  if (!wifiAvailable) return;
  timeClient.update();
  int today = timeClient.getDay();
  if (lastDayChecked == -1) {
    lastDayChecked = today;
    preferences.begin("fan-stats", false);
    preferences.putInt("lastDay", lastDayChecked);
    preferences.end();
    return;
  }
  if (lastDayChecked != today) {
    Serial.printf("日期变更 %d→%d，处理电量数据...\n", lastDayChecked, today);
    preferences.begin("fan-stats", false);
    for (int i = HISTORY_DAYS-1; i > 0; i--) dailyEnergyWh[i] = dailyEnergyWh[i-1];
    dailyEnergyWh[0] = todayEnergyWh;
    char key[12];
    for (int i = 0; i < HISTORY_DAYS; i++) {
      sprintf(key, "dayWh_%d", i);
      preferences.putFloat(key, dailyEnergyWh[i]);
    }
    todayEnergyWh  = 0.0f;
    lastDayChecked = today;
    preferences.putFloat("todayWh", todayEnergyWh);
    preferences.putInt("lastDay",  lastDayChecked);
    preferences.end();
    Serial.println("电量数据处理完毕。");
  }
}


// =======================================================
// ============== 插座定时任务持久化 ==============
// =======================================================

void loadSocketSchedules() {
  preferences.begin("socket-sched", true);
  relay2_schedule_count = preferences.getInt("r2_count", 0);
  relay3_schedule_count = preferences.getInt("r3_count", 0);
  char key[12];
  for (int i = 0; i < relay2_schedule_count; i++) {
    sprintf(key, "r2_%d_", i);
    relay2_schedules[i].hour   = preferences.getUChar((String(key)+"h").c_str(), 0);
    relay2_schedules[i].minute = preferences.getUChar((String(key)+"m").c_str(), 0);
    relay2_schedules[i].second = preferences.getUChar((String(key)+"s").c_str(), 0);
    relay2_schedules[i].action  = preferences.getBool ((String(key)+"a").c_str(), false);
    relay2_schedules[i].enabled = true;
  }
  for (int i = 0; i < relay3_schedule_count; i++) {
    sprintf(key, "r3_%d_", i);
    relay3_schedules[i].hour   = preferences.getUChar((String(key)+"h").c_str(), 0);
    relay3_schedules[i].minute = preferences.getUChar((String(key)+"m").c_str(), 0);
    relay3_schedules[i].second = preferences.getUChar((String(key)+"s").c_str(), 0);
    relay3_schedules[i].action  = preferences.getBool ((String(key)+"a").c_str(), false);
    relay3_schedules[i].enabled = true;
  }
  preferences.end();
  Serial.printf("[OK] 加载插座定时: 插座1(%d条), 插座2(%d条)\n",
                relay2_schedule_count, relay3_schedule_count);
}

void saveSocketSchedules() {
  preferences.begin("socket-sched", false);
  preferences.clear();
  preferences.putInt("r2_count", relay2_schedule_count);
  preferences.putInt("r3_count", relay3_schedule_count);
  char key[12];
  for (int i = 0; i < relay2_schedule_count; i++) {
    sprintf(key, "r2_%d_", i);
    preferences.putUChar((String(key)+"h").c_str(), relay2_schedules[i].hour);
    preferences.putUChar((String(key)+"m").c_str(), relay2_schedules[i].minute);
    preferences.putUChar((String(key)+"s").c_str(), relay2_schedules[i].second);
    preferences.putBool ((String(key)+"a").c_str(), relay2_schedules[i].action);
  }
  for (int i = 0; i < relay3_schedule_count; i++) {
    sprintf(key, "r3_%d_", i);
    preferences.putUChar((String(key)+"h").c_str(), relay3_schedules[i].hour);
    preferences.putUChar((String(key)+"m").c_str(), relay3_schedules[i].minute);
    preferences.putUChar((String(key)+"s").c_str(), relay3_schedules[i].second);
    preferences.putBool ((String(key)+"a").c_str(), relay3_schedules[i].action);
  }
  preferences.end();
  Serial.println("[OK] 插座定时任务已写入闪存。");
}


// =======================================================
// ============== Web 路由注册 ==============
// =======================================================

void setupRoutes() {
  // 风扇页
  server.on("/",                    HTTP_GET,  handleRoot);
  server.on("/getData",             HTTP_GET,  handleGetData);
  server.on("/getStatus",           HTTP_GET,  handleGetStatus);
  server.on("/getPowerStats",       HTTP_GET,  handleGetPowerStats);
  server.on("/setSpeed",            HTTP_GET,  handleSetSpeed);
  server.on("/setRelay",            HTTP_GET,  handleSetRelay);
  server.on("/setTimers",           HTTP_POST, handleSetTimers);
  server.on("/setVoltageThresholds",HTTP_GET,  handleSetVoltageThresholds);
  // 插座页
  server.on("/socket",              HTTP_GET,  handleSocketPage);
  server.on("/getSocketData",       HTTP_GET,  handleGetSocketData);
  server.on("/setSocketTask",       HTTP_GET,  handleSetSocketTask);
  server.on("/deleteAllTasks",      HTTP_GET,  handleDeleteAllTasks);
  server.on("/LED-Control",         HTTP_GET,  handleApiAddTask);
  // NAS 继电器
  server.on("/setNasRelay",         HTTP_GET,  handleSetNasRelay);
  // 通用
  server.on("/sysinfo",             HTTP_GET,  handleSysInfo);
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "更新失败" : "更新成功!");
    delay(1000);
    ESP.restart();
  }, handleUpdateUpload);
}


// =======================================================
// ============== SETUP ==============
// =======================================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP32C3 智能风扇 & 双路插座 & NAS控制器 v4.8.0 =====");

  // -------- 1. 硬件初始化 --------
  Serial.println("--- 硬件初始化 ---");

  // 风扇继电器（默认关）
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // 插座继电器（先关，稍后由启动定时开启）
  pinMode(RELAY2_PIN, OUTPUT); digitalWrite(RELAY2_PIN, LOW);
  pinMode(RELAY3_PIN, OUTPUT); digitalWrite(RELAY3_PIN, LOW);

  // NAS 继电器（默认开）
  pinMode(RELAY4_PIN, OUTPUT);
  setNasRelay(true);  // 上电即开启
  Serial.printf("[OK] NAS继电器 (Pin %d) 已默认开启。\n", RELAY4_PIN);

  // PWM (风扇)
  ledcAttach(PWM_PIN, LEDC_FREQUENCY, LEDC_RES_BITS);
  ledcWrite(PWM_PIN, PWM_INVERTED ? PWM_MAX : 0);

  // 测速引脚
  pinMode(TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachISR, FALLING);

  // INA219
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!ina219.begin()) {
    Serial.println("[!!] INA219 未找到，跳过电压检测。");
    ina219_ok = false;
  } else {
    ina219.setCalibration_32V_2A();
    ina219_ok = true;
    Serial.println("[OK] INA219 初始化完成。");
  }

  // -------- 2. 从闪存加载配置（不依赖网络）--------
  preferences.begin("fan-config", true);
  VOLTAGE_THRESHOLD      = preferences.getFloat("lowV_thresh",  3.5f);
  VOLTAGE_HIGH_THRESHOLD = preferences.getFloat("highV_thresh", 4.2f);
  preferences.end();
  Serial.printf("[OK] 电压阈值: 低压 %.2fV / 高压 %.2fV\n",
                VOLTAGE_THRESHOLD, VOLTAGE_HIGH_THRESHOLD);

  loadSocketSchedules();

  // -------- 3. 启动定时: 立即开启双路插座 40 分钟（无需 WiFi）--------
  bootTime      = millis();
  bootTimerActive = true;
  setSocketRelay(2, true);
  setSocketRelay(3, true);
  Serial.println("[OK] 双路插座已开启，将在 40 分钟后自动关闭（millis计时，无需WiFi）。");

  // -------- 4. WiFi 连接（带超时，最多等 30 秒）--------
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName);
  WiFi.enableIPv6();
  WiFi.begin(ssid, password);
  Serial.print("连接 Wi-Fi (最多等待30秒)");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000UL) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  wifiAvailable = (WiFi.status() == WL_CONNECTED);
  if (wifiAvailable) {
    Serial.println("[OK] WiFi 已连接!");
    Serial.print("IPv4: "); Serial.println(WiFi.localIP());
    Serial.print("IPv6: "); Serial.println(getIPv6());

    // NTP 同步
    timeClient.begin();
    Serial.print("同步NTP时间...");
    unsigned long ntpStart = millis();
    while (!timeClient.update() && millis() - ntpStart < 10000UL) {
      timeClient.forceUpdate(); delay(500); Serial.print(".");
    }
    Serial.println(timeClient.isTimeSet() ? "\n[OK] NTP已同步。" : "\n[!!] NTP同步超时，时间功能受限。");

    // 加载电量统计
    preferences.begin("fan-stats", true);
    lastDayChecked = preferences.getInt("lastDay", -1);
    int curDay = timeClient.getDay();
    todayEnergyWh = (lastDayChecked == curDay) ? preferences.getFloat("todayWh", 0.0f) : 0.0f;
    char key[12];
    for (int i = 0; i < HISTORY_DAYS; i++) {
      sprintf(key, "dayWh_%d", i);
      dailyEnergyWh[i] = preferences.getFloat(key, 0.0f);
    }
    preferences.end();
    Serial.println("[OK] 电量统计数据已加载。");

    // mDNS
    if (MDNS.begin(deviceName)) {
      MDNS.addService("http", "tcp", WEB_SERVER_PORT);
      Serial.printf("[OK] mDNS: http://%s.local:%d\n", deviceName, WEB_SERVER_PORT);
    }
  } else {
    Serial.println("[!!] WiFi 连接超时，以离线模式运行。");
    Serial.println("     ↳ 40分钟启动定时仍然有效。");
    Serial.println("     ↳ NTP定时任务、电量统计暂不可用。");
    Serial.println("     ↳ Web界面在获取到IP后自动恢复。");
  }

  // -------- 5. 注册路由并启动服务器 --------
  setupRoutes();
  server.begin();
  Serial.printf("[OK] HTTP 服务器已启动，端口: %d\n", WEB_SERVER_PORT);
  Serial.println("=========================================\n");

  lastEnergyCalcMs = millis();
}


// =======================================================
// ============== LOOP ==============
// =======================================================

void loop() {
  server.handleClient();

  // ---- 1. 启动定时检查：40分钟后关闭双路插座（millis实现，不依赖WiFi）----
  if (bootTimerActive && (millis() - bootTime >= BOOT_ON_DURATION_MS)) {
    bootTimerActive = false;
    setSocketRelay(2, false);
    setSocketRelay(3, false);
    Serial.println("[Timer] 40分钟启动定时结束，双路插座已自动关闭。");
  }

  // ---- 2. 电量累积 ----
  accumulateEnergy();

  // ---- 3. 电压保护检查（每5秒）----
  static unsigned long lastVoltageCheck = 0;
  if (millis() - lastVoltageCheck > 5000UL) {
    lastVoltageCheck = millis();
    checkVoltageProtection();
  }

  // ---- 4. 插座定时任务检查（每1秒，需WiFi）----
  static unsigned long lastSocketCheck = 0;
  if (millis() - lastSocketCheck >= 1000UL) {
    lastSocketCheck = millis();
    checkSocketSchedules();
  }

  // ---- 5. 日期翻转 + 风扇定时（每30秒，需WiFi）----
  static unsigned long lastTimerCheck = 0;
  if (millis() - lastTimerCheck > 30000UL) {
    lastTimerCheck = millis();
    checkDailyRollover();
    checkFanTimers();
  }

  // ---- 6. 每60秒写入今日电量（需WiFi）----
  static unsigned long lastDataSave = 0;
  if (millis() - lastDataSave > 60000UL) {
    lastDataSave = millis();
    if (wifiAvailable) {
      preferences.begin("fan-stats", false);
      preferences.putFloat("todayWh", todayEnergyWh);
      preferences.end();
    }
  }

  // ---- 7. WiFi 断线重连（每30秒检查一次）----
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000UL) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiAvailable) {
        Serial.println("[WiFi] 连接断开，尝试重连...");
        wifiAvailable = false;
      }
      WiFi.reconnect();
    } else if (!wifiAvailable) {
      wifiAvailable = true;
      Serial.println("[WiFi] 重新连接成功！");
      if (!timeClient.isTimeSet()) timeClient.forceUpdate();
    }
  }
}
