// =================================================================================================
// ==   ESP32-C3 Mini 智能继电器 & INA219 v7.0 (完全适配 ESP32-C3 架构，运行更稳定)              ==
/*系统供电	VCC / GND	5V / GND	12V 电池经 LM2596  降压至 5V 接入 INA219 传感器供电 (3.3V)
I2C 传感器	INA219 SDA	GPIO 4	避开 GPIO 8 以防上电异常
I2C 传感器	INA219 SCL	GPIO 5	避开 GPIO 9 以防上电异常
大电流继电器	IRL8721 MOS Gate	GPIO 10	输出高电平开启，Gate极并接 10k 下拉电阻至 GND
32x8 像素屏	WS2812 DIN	GPIO 3	
*/
// =================================================================================================

#include <WiFi.h>              // 【修改】
#include <WebServer.h>         // 【修改】
#include <ESPmDNS.h>           // 【修改】
#include "ipv6.h"              // IPv6 工具模块
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Preferences.h>
#include <sys/time.h>
#include <EEPROM.h>
#include <time.h>
#include <ESP_Mail_Client.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <Update.h>            // 【修改】引入 ESP32 固件更新支持
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_attr.h>      // RTC_NOINIT_ATTR 看门狗重启标记
#include "MatrixClock.h" 
#include "notify.h"
#include "logs.h" 
#include "main_html.h"

// ============== 用户 WiFi 配置 ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp32c3-smart-relay";
const int WEB_SERVER_PORT = 80;

// ============== 邮件配置 ==============
bool pendingTestEmail = false;
String testEmailResult = "idle"; 
bool pendingWarningEmail  = false;
bool pendingLockoutEmail  = false;
bool pendingNotifyWarning = false;

String pendingEmailSubject = "";
String pendingEmailBody    = "";

String smtp_host     = "smtp.qq.com";
int    smtp_port     = 465;
String author_email  = "";
String author_pass   = "";
String recipient_email = "";

const char* NTP_SERVERS           = "ntp.aliyun.com, pool.ntp.org, time.nist.gov";
const int   GMT_OFFSET            = 8;
const int   DAYLIGHT_OFFSET       = 0;

// ============== 【修改】硬件引脚配置（避开 C3 Strap 启动引脚） ==============
const int RELAY_PIN   = 10; // 继电器控制 (GPIO 10)
const int I2C_SDA_PIN = 4;  // I2C SDA (GPIO 4)
const int I2C_SCL_PIN = 5;  // I2C SCL (GPIO 5)
const int MATRIX_PIN  = 3;  // 点阵 WS2812 DIN (GPIO 3)
const int MATRIX_POWER_PIN = 1; // GPIO1 控制外部高侧电子开关 EN/CTRL（HIGH=通电，LOW=断开）；不接12/24/48V

// ============== 时间与定时任务配置 ==============
// 主时钟统一使用 ESP32 系统时钟：正常运行时靠 millis/RTC 走时，联网时由 SNTP 后台校正。
const long  WEB_UI_GMT_OFFSET_SEC = 8 * 3600;
const char* CLOCK_SNAPSHOT_PATH = "/heartbeat.txt";
const char* RELAY_STATE_PATH = "/relay.txt";

// 继电器状态同时写入 NVS 和 LittleFS。NVS 用于重启早期恢复，文件用于兼容旧固件。
Preferences relayPreferences;
bool relayPreferencesReady = false;
bool firstBootDetected = false;
static const uint32_t RELAY_RTC_MAGIC = 0x52E1A7E1;
RTC_NOINIT_ATTR uint32_t relayRtcStateMagic;
RTC_NOINIT_ATTR uint32_t relayRtcStateValue;

// ============== EEPROM 存储地址定义 ==============
const int EEPROM_SIZE = 64; 
const int ADDR_MAGIC_NUM   = 0;
const int ADDR_LOW_V       = 2;
const int ADDR_HIGH_V      = 6;
const int ADDR_WARN_V      = 10;
const uint16_t EEPROM_MAGIC_NUMBER = 0x5A1D;

// ============== 配置文件路径 ==============
const char* VLOG_FILE_PATH = "/vlog.dat";
const char* EMAIL_CFG_PATH = "/email_cfg.txt";

// ============== 全局对象与变量 ==============
WebServer server(WEB_SERVER_PORT);      // 【修改】
SmartMatrixClock myClock(MATRIX_PIN, MATRIX_POWER_PIN, server); // 灯板 DIN=GPIO3，外部高侧电子开关逻辑控制=GPIO1

// ===== 灯板刷新任务状态 =====
// 灯板降压模块的定时硬断电恢复由 MatrixClock.h 中独立 PowerCycleTask 执行。
// 不再提供 /clock/reboot 软件重启按钮；手动/自动恢复只切断灯板 DC-DC 输入电源。
bool matrixClockTaskRunning = false;

Adafruit_INA219 ina219;
SMTPSession smtp;

bool relayState = false, ina219_ok = false;
bool wifiConnected = false;
float busVoltage = 0;

// 首次开机时先启动本地服务；仅在确认 15 秒内没有 Wi-Fi 后，才按规则默认开启继电器。
bool initialOfflineDefaultPending = false;
uint32_t initialOfflineDefaultDeadline = 0;
unsigned long lastWifiAttemptMs = 0;
float lowVoltageThreshold = 10.5, highVoltageThreshold = 12.8, warningVoltageThreshold = 11.5;
bool isLockedOut = false;
bool isVoltageWarning = false;
unsigned long lockoutStartTime = 0;
const unsigned long LOCKOUT_DURATION_MS = 3600000;

float lastBusVoltage = 0;
const float WARNING_HYSTERESIS_V = 0.5;

const unsigned long NOTIFICATION_COOLDOWN_MS = 3600000;
unsigned long lastWarningNoticeTime = 0;
unsigned long lastLockoutNoticeTime = 0;

// --- 图表数据配置 ---
const int DATA_POINTS = 1440;
const int CHART_SAMPLE_STEP = 5;                 // 图表按 5 分钟取 1 点，降低首页加载和 CPU 开销
const int CHART_POINTS = DATA_POINTS / CHART_SAMPLE_STEP; // 288 点，仍覆盖完整 24 小时
const unsigned long DATA_INTERVAL_MS = 60000;
int historyIndex = 0;

// 曲线请求先复制历史样本，再在 RAM 中降采样生成 JSON。
static float chartSnapshot[DATA_POINTS];

// WS2812 刷新独立运行；即使首页、曲线、SMTP 或通知请求耗时，也能按 250ms 周期刷新。
void matrixClockTask(void* parameter) {
  (void)parameter;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(20);
  for (;;) {
    myClock.setBatteryVoltage(busVoltage);
    myClock.loop();
    vTaskDelayUntil(&lastWake, period);
  }
}

// --- CPU使用率估算 (优化版：杜绝空转，精准计算工作时间比例) ---
unsigned long lastCpuMeasureTime = 0;
float cpuUsage = 0.0;
unsigned long totalWorkTime = 0; // 累计实际工作耗时 (微秒)
const unsigned long CPU_MEASURE_INTERVAL_MS = 1000;
// ============== 辅助函数 ==============
uint32_t getChipId() {
  return (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
}

// 设置中国时区，不需要网络。POSIX 的 CST-8 表示 UTC+8。
void setLocalTimezone() {
  setenv("TZ", "CST-8", 1);
  tzset();
}

// 只在本机系统时间有效时返回时间；系统时间在同步后会独立走时，不依赖网页或网络轮询。
String getDeviceFormattedTime() {
  time_t nowTs = time(nullptr);
  struct tm tmInfo;
  if (nowTs >= 1700000000UL) {
    localtime_r(&nowTs, &tmInfo);
  } else {
    // 首次冷启动且完全离线、又没有历史快照时无法知道真实墙上时间；至少显示会走动的临时钟。
    uint32_t daySeconds = (8UL * 3600UL + millis() / 1000UL) % 86400UL;
    tmInfo.tm_hour = daySeconds / 3600UL;
    tmInfo.tm_min = (daySeconds % 3600UL) / 60UL;
    tmInfo.tm_sec = daySeconds % 60UL;
  }
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
  return String(buf);
}

// 保存最近的有效时间。断网时系统仍继续走时；如果软件重启后系统时钟无效，可恢复这个快照。
void saveClockSnapshot() {
  time_t nowTs = time(nullptr);
  if (nowTs < 1700000000UL) return;
  File f = LittleFS.open(CLOCK_SNAPSHOT_PATH, "w");
  if (f) { f.print((uint32_t)nowTs); f.close(); }
}

void restoreClockSnapshotIfNeeded() {
  if (time(nullptr) >= 1700000000UL) return;
  File f = LittleFS.open(CLOCK_SNAPSHOT_PATH, "r");
  if (!f) return;
  uint32_t savedEpoch = (uint32_t)f.parseInt();
  f.close();
  if (savedEpoch < 1700000000UL) return;
  struct timeval tv;
  tv.tv_sec = (time_t)savedEpoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.printf("[Clock] 从本地快照恢复时间戳：%lu\n", (unsigned long)savedEpoch);
}

// SNTP 在系统后台工作，不在 loop() 里同步等待。联网时调用；断网时系统时钟继续走。
void configureNetworkTimeSync() {
  setLocalTimezone();
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov");
}

// 读取 NVS 持久状态。返回 true 表示有可靠的历史继电器状态。
bool readRelayStateFromNvs(bool &stateOut) {
  if (!relayPreferencesReady) return false;
  if (!relayPreferences.getBool("has_state", false)) return false;
  stateOut = relayPreferences.getBool("state", false);
  return true;
}

// =====================================================
// ============== 网页 (HTML+CSS+JS) ==============
// =====================================================
// 首页 HTML/JavaScript 模板已移至 main_html.h，避免 Arduino .ino 预处理误解析。

const char OTA_SUCCESS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新成功</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#22c55e;}</style></head><body><div><h1>更新成功!</h1><p>设备正在重启，请在约1分钟后重新连接。</p></div></body></html>)HTML";
const char OTA_FAIL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新失败</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#ef4444;}</style></head><body><div><h1>更新失败!</h1><p>请检查上传的固件文件(.bin)是否正确，然后返回重试。</p></div></body></html>)HTML";


// ============== 邮件功能函数 ==============
void smtpCallback(SMTP_Status status){
  Serial.println(status.info());
  if (status.success()){
    Serial.println("----------------");
    MailClient.printf("Message sent success: %d\n", status.completedCount());
    MailClient.printf("Message sent failed: %d\n", status.failedCount());
    Serial.println("----------------\n");
  }
}

// ============== 完美兼容 IPv6 环境的邮件发送函数 ==============
void sendEmailNotification(String subject, String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[!!] 无法发送邮件，WiFi未连接。");
    return;
  }
  if (author_email.length() < 5 || recipient_email.length() < 5) {
     Serial.println("[!!] 邮件功能未配置 (请在网页端设置)，跳过发送。");
     return;
  }
  
  Session_Config config;

  // === 【最核心修复：利用 getaddrinfo 强制走 IPv4】 ===
  String resolved_host = resolveIPv4(smtp_host.c_str());
  if (resolved_host.length() == 0) {
    // 如果强解失败，作为兜底方案才使用原域名
    resolved_host = smtp_host;
  }
  
  config.server.host_name = resolved_host.c_str(); // 此时 host_name 将是纯 IPv4 的 IP 字符串 (例如 "14.18.245.164")
  config.server.port = smtp_port;
  config.login.email = author_email.c_str();
  config.login.password = author_pass.c_str();
  config.login.user_domain = F("127.0.0.1");
  config.time.ntp_server = NTP_SERVERS;
  config.time.gmt_offset = GMT_OFFSET;
  config.time.day_light_offset = DAYLIGHT_OFFSET;

  // 465 与 587 端口均统一使用 esp_mail_secure_mode_ssl_tls
  if (smtp_port == 465 || smtp_port == 587) {
    config.secure.mode = esp_mail_secure_mode_ssl_tls;
  } else {
    config.secure.mode = esp_mail_secure_mode_nonsecure;
  }

  // 强行关闭 SSL 证书链验证（因为我们是用 IPv4 IP 直接建立 SSL 握手，不关闭验证会导致“证书域名不匹配”报错）
  config.certificate.verify = false;
  
  smtp.setTCPTimeout(10);

  SMTP_Message email;
  email.sender.name = F("ESP32-C3 继电器");
  email.sender.email = author_email.c_str();
  email.subject = subject;
  email.addRecipient(F("User"), recipient_email.c_str());
  
  String htmlMsg = "<h2>" + subject + "</h2><p>" + message + "</p><p>设备名称: " + String(deviceName) + "</p><p>当前时间: " + getDeviceFormattedTime() + "</p>";
  email.html.content = htmlMsg;
  email.html.transfer_encoding = Content_Transfer_Encoding::enc_base64;

  Serial.printf("准备通过 IPv4 发送邮件至 %s ...\n", recipient_email.c_str());
  if (!smtp.connect(&config)) {
    MailClient.printf("SMTP Error: %d, %s\n", smtp.statusCode(), smtp.errorReason().c_str());
    return;
  }
  if (!MailClient.sendMail(&smtp, &email)) {
    MailClient.printf("Send Error: %d, %s\n", smtp.statusCode(), smtp.errorReason().c_str());
  }
  smtp.closeSession(); 
}



// ============== 数据存储函数 ==============
void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint16_t magic; EEPROM.get(ADDR_MAGIC_NUM, magic);
  if (magic == EEPROM_MAGIC_NUMBER) {
    EEPROM.get(ADDR_LOW_V, lowVoltageThreshold); 
    EEPROM.get(ADDR_HIGH_V, highVoltageThreshold);
    EEPROM.get(ADDR_WARN_V, warningVoltageThreshold);
  } else {
    EEPROM.put(ADDR_MAGIC_NUM, EEPROM_MAGIC_NUMBER); 
    EEPROM.put(ADDR_LOW_V, lowVoltageThreshold);
    EEPROM.put(ADDR_HIGH_V, highVoltageThreshold);
    EEPROM.put(ADDR_WARN_V, warningVoltageThreshold);
    EEPROM.commit();
  }
  EEPROM.end();

  if (LittleFS.exists(EMAIL_CFG_PATH)) {
    File cfgFile = LittleFS.open(EMAIL_CFG_PATH, "r");
    if (cfgFile) {
      String line;
      if(cfgFile.available()) { line = cfgFile.readStringUntil('\n'); line.trim(); if(line.length()>0) smtp_host = line; }
      if(cfgFile.available()) { line = cfgFile.readStringUntil('\n'); line.trim(); if(line.length()>0) smtp_port = line.toInt(); }
      if(cfgFile.available()) { line = cfgFile.readStringUntil('\n'); line.trim(); if(line.length()>0) author_email = line; }
      if(cfgFile.available()) { line = cfgFile.readStringUntil('\n'); line.trim(); if(line.length()>0) author_pass = line; }
      if(cfgFile.available()) { line = cfgFile.readStringUntil('\n'); line.trim(); if(line.length()>0) recipient_email = line; }
      cfgFile.close();
      Serial.println("[OK] 邮件配置已加载。");
    }
  } else {
    Serial.println("[INFO] 未找到邮件配置。");
  }
}

void saveVoltageSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(ADDR_LOW_V, lowVoltageThreshold); 
  EEPROM.put(ADDR_HIGH_V, highVoltageThreshold);
  EEPROM.put(ADDR_WARN_V, warningVoltageThreshold);
  EEPROM.commit(); 
  EEPROM.end();
}

void saveEmailConfigToFile() {
  File cfgFile = LittleFS.open(EMAIL_CFG_PATH, "w");
  if (cfgFile) {
    cfgFile.println(smtp_host);
    cfgFile.println(smtp_port);
    cfgFile.println(author_email);
    cfgFile.println(author_pass);
    cfgFile.println(recipient_email);
    cfgFile.close();
    Serial.println("[OK] 邮件配置已保存。");
  }
}

// ============== 核心功能函数 ==============
void recordVoltageHistory() {
  float voltage = 0.0;
  if (ina219_ok) {
    voltage = ina219.getBusVoltage_V();
    if (voltage < 0.1) voltage = 0.0;
  }

  File vlogFile = LittleFS.open(VLOG_FILE_PATH, "r+");
  if (!vlogFile) return;
  vlogFile.seek(sizeof(int) + (historyIndex * sizeof(float)), SeekSet);
  vlogFile.write((byte*)&voltage, sizeof(float));
  historyIndex = (historyIndex + 1) % DATA_POINTS;
  vlogFile.seek(0, SeekSet);
  vlogFile.write((byte*)&historyIndex, sizeof(int));
  vlogFile.close();
}

void setRelay(bool state) {
  // 先更新 RTC 快速备份和 GPIO，再写 NVS/LittleFS；软重启时可在 setup 最早阶段恢复。
  relayState = state;
  relayRtcStateValue = state ? 1U : 0U;
  relayRtcStateMagic = RELAY_RTC_MAGIC;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

  if (relayPreferencesReady) {
    relayPreferences.putBool("state", state);
    relayPreferences.putBool("has_state", true);
    relayPreferences.putBool("booted", true);
  }

  File f = LittleFS.open(RELAY_STATE_PATH, "w");
  if (f) { f.print(state ? "1" : "0"); f.close(); }
  sysLogf(LOG_INFO, "继电器 -> %s  电压=%.2fV  堆=%uB",
    state ? "ON" : "OFF", busVoltage, ESP.getFreeHeap());
}

void checkVoltageProtection() {
  if (!ina219_ok) return;
  busVoltage = ina219.getBusVoltage_V();
  bool voltageIsDecreasing = (busVoltage < (lastBusVoltage - 0.02));

  if (relayState && busVoltage <= warningVoltageThreshold && voltageIsDecreasing) {
      isVoltageWarning = true;
      if (millis() - lastWarningNoticeTime > NOTIFICATION_COOLDOWN_MS || lastWarningNoticeTime == 0) {
          lastWarningNoticeTime = millis();
          sysLogf(LOG_WARN, "电压警告 %.2fV (阈值%.2fV)", busVoltage, warningVoltageThreshold);

          pendingEmailSubject = "[电压警告] " + String(deviceName);
          pendingEmailBody    = "电压为 " + String(busVoltage, 2) + "V，低于警告值 " + String(warningVoltageThreshold, 2) + "V。";
          pendingWarningEmail = true;
          pendingNotifyWarning = true;

      }
  } else if (busVoltage > (warningVoltageThreshold + WARNING_HYSTERESIS_V)) {
      isVoltageWarning = false;
  }

  if (isLockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_DURATION_MS) {
      isLockedOut = false;
    } else { 
      lastBusVoltage = busVoltage;
      return; 
    }
  }

  if (!relayState && !isLockedOut && busVoltage >= highVoltageThreshold) {
    setRelay(true);
  }
  
  if (relayState && busVoltage > 0.1 && busVoltage < lowVoltageThreshold) {
    Serial.printf("!!! 低压切断 (%.2fV)\n", busVoltage);
    if (millis() - lastLockoutNoticeTime > NOTIFICATION_COOLDOWN_MS || lastLockoutNoticeTime == 0) {
      lastLockoutNoticeTime = millis();
      sysLogf(LOG_ERR, "低压切断 %.2fV < %.2fV", busVoltage, lowVoltageThreshold);
      pendingEmailSubject = "[低压切断] " + String(deviceName);
      pendingEmailBody    = "电压 " + String(busVoltage, 2) + "V 低于阈值 " + String(lowVoltageThreshold, 2) + "V，已切断。";
      pendingLockoutEmail = true;
    }
    isLockedOut = true; 
    lockoutStartTime = millis(); 
    setRelay(false);
  }

  lastBusVoltage = busVoltage;
}

// ============== Web路由处理 ==============
void handleSetNotifyApi() {
  if (!server.hasArg("url")) {
    server.send(400, "text/plain", "缺少 url");
    return;
  }

  String url = server.arg("url");
  url.trim();

  if (!url.startsWith("http://") &&
      !url.startsWith("https://")) {
    server.send(400, "text/plain", "API URL 无效");
    return;
  }

  if (!Notify::saveConfig(url)) {
    server.send(500, "text/plain", "保存失败");
    return;
  }

  server.send(200, "text/plain", "OK");
}
void handleTestEmail() {
  if (author_email.length() < 5 || recipient_email.length() < 5) {
    server.send(400, "text/plain", "未配置邮件"); return;
  }
  pendingTestEmail = true;
  testEmailResult = "pending";
  server.send(200, "text/plain", "queued"); 
}

void handleTestEmailResult() {
  server.send(200, "text/plain", testEmailResult);
}
void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);

  server.send(200, "text/html", "");

  server.sendContent_P(MAIN_HTML_PART1);
  server.sendContent_P(MAIN_HTML_PART2);

  server.sendContent("");
}

void handleGetData() {
  if (ina219_ok) busVoltage = ina219.getBusVoltage_V();
  long remaining_min = isLockedOut ? (LOCKOUT_DURATION_MS - (millis() - lockoutStartTime)) / 60000 : 0;
  
  String json = "{";
  json += "\"voltage\":" + String(busVoltage, 2) + ",";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"time\":\"" + getDeviceFormattedTime() + "\",";
  json += "\"lockout\":" + String(isLockedOut ? "true" : "false") + ",";
  json += "\"lockout_rem\":" + String(remaining_min) + ",";
  json += "\"voltage_warning\":" + String(isVoltageWarning ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleGetStatus() {
  String json = "{";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"low_v\":" + String(lowVoltageThreshold, 2) + ",";
  json += "\"warn_v\":" + String(warningVoltageThreshold, 2) + ",";
  json += "\"high_v\":" + String(highVoltageThreshold, 2) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  ipv6Mgr.appendJSON(json);    // 追加 "ipv6":"xxxx",
  json += "\"chip_id\":\"" + String(getChipId(), HEX) + "\","; // 【修改】
  
  // 【修改】移除了 ESP8266 特有的 FSInfo 限制，直接使用 ESP32 LittleFS API
  json += "\"free_heap\":" + String(ESP.getFreeHeap() / 1024) + ",";
  json += "\"total_heap\":" + String(320000 / 1024) + ","; // ESP32 C3 典型剩余 Heap 分配
  json += "\"fs_free\":" + String((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024) + ",";
  json += "\"fs_total\":" + String(LittleFS.totalBytes() / 1024) + ",";
  json += "\"cpu_usage\":" + String(cpuUsage, 1) + ",";
  json += "\"mail_host\":\"" + smtp_host + "\",";
  json += "\"mail_port\":" + String(smtp_port) + ",";
  json += "\"mail_user\":\"" + author_email + "\",";
  json += "\"mail_pass\":\"" + author_pass + "\",";
  json += "\"mail_to\":\"" + recipient_email + "\"";
  json += ",\"notify_api\":\"" + Notify::getApiUrl() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetRelay() {
  if (server.hasArg("state")) {
    bool newState = server.arg("state").toInt() == 1;
    if (newState && isLockedOut) isLockedOut = false;
    setRelay(newState);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}

void handleSetSettings() {
  if (server.hasArg("low") && server.hasArg("high") && server.hasArg("warn")) {
    lowVoltageThreshold = server.arg("low").toFloat();
    highVoltageThreshold = server.arg("high").toFloat();
    warningVoltageThreshold = server.arg("warn").toFloat();
    saveVoltageSettings();
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}

void handleSetEmail() {
  if (server.hasArg("host") && server.hasArg("port") && server.hasArg("user") && server.hasArg("pass") && server.hasArg("to")) {
    smtp_host = server.arg("host");
    smtp_port = server.arg("port").toInt();
    author_email = server.arg("user");
    author_pass = server.arg("pass");
    recipient_email = server.arg("to");
    saveEmailConfigToFile();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing args");
  }
}

void handleGetChartData() {
  // 先顺序读取到 RAM，再生成较小的 JSON；避免在 HTTP 请求中逐点 seek 文件。
  File vlogFile = LittleFS.open(VLOG_FILE_PATH, "r");
  if (!vlogFile || vlogFile.size() < (size_t)(sizeof(int) + DATA_POINTS * sizeof(float))) {
    if (vlogFile) vlogFile.close();
    server.send(503, "application/json", "{\"labels\":[],\"data\":[]}");
    return;
  }

  int savedIndex = 0;
  if (vlogFile.read((uint8_t*)&savedIndex, sizeof(savedIndex)) != sizeof(savedIndex)) {
    vlogFile.close();
    server.send(503, "application/json", "{\"labels\":[],\"data\":[]}");
    return;
  }
  if (savedIndex < 0 || savedIndex >= DATA_POINTS) savedIndex = 0;

  int outIndex = 0;
  const int firstSegment = DATA_POINTS - savedIndex;
  vlogFile.seek(sizeof(int) + (size_t)savedIndex * sizeof(float), SeekSet);
  for (int i = 0; i < firstSegment; ++i) {
    if (vlogFile.read((uint8_t*)&chartSnapshot[outIndex], sizeof(float)) != sizeof(float)) {
      chartSnapshot[outIndex] = NAN;
    }
    ++outIndex;
  }
  if (savedIndex > 0) {
    vlogFile.seek(sizeof(int), SeekSet);
    for (int i = 0; i < savedIndex; ++i) {
      if (vlogFile.read((uint8_t*)&chartSnapshot[outIndex], sizeof(float)) != sizeof(float)) {
        chartSnapshot[outIndex] = NAN;
      }
      ++outIndex;
    }
  }
  vlogFile.close();

  // 1440 个点降采样到 288 个点（每 5 分钟一个），足够显示 24 小时趋势。
  // 减小响应体，减少 String 拼接和 Wi-Fi 发送期间的 CPU 占用。
  String json;
  json.reserve(5200);
  json += "{\"labels\":[";
  for (int i = 0; i < CHART_POINTS; ++i) {
    if (i) json += ',';
    const int remaining = CHART_POINTS - 1 - i;
    if (i == CHART_POINTS - 1) {
      json += "\"Now\"";
    } else if (i == 0) {
      json += "\"-24h\"";
    } else if (remaining % 36 == 0) { // 每 3 小时一个标签：36 点 × 5 分钟
      char label[16];
      snprintf(label, sizeof(label), "\"-%dh\"", remaining / 12);
      json += label;
    } else {
      json += "\"\"";
    }
  }

  json += "],\"data\":[";
  for (int i = 0; i < CHART_POINTS; ++i) {
    if (i) json += ',';
    const int sampleIndex = (i + 1) * CHART_SAMPLE_STEP - 1;
    const float voltage = chartSnapshot[sampleIndex];
    if (isnan(voltage) || voltage < 0.1f) {
      json += "null";
    } else {
      char value[16];
      snprintf(value, sizeof(value), "%.2f", voltage);
      json += value;
    }
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// ============== SETUP ==============
void setup() {
  bool savedRelay = false;
  bool savedRelayAvailable = false;

  // 最优先恢复 RTC 缓存的输出状态，减少软件重启后继电器重新拉高的延迟。
  const bool rtcRelayStateValid =
      (relayRtcStateMagic == RELAY_RTC_MAGIC && relayRtcStateValue <= 1U);
  pinMode(RELAY_PIN, OUTPUT);
  relayState = rtcRelayStateValid ? (relayRtcStateValue == 1U) : false;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  if (rtcRelayStateValid) {
    savedRelay = relayState;
    savedRelayAvailable = true;
  }

  Serial.begin(115200);

  relayPreferencesReady = relayPreferences.begin("relaycfg", false);
  bool wasBootedBefore = false;
  if (relayPreferencesReady) {
    wasBootedBefore = relayPreferences.getBool("booted", false);
    firstBootDetected = !wasBootedBefore;
    if (!wasBootedBefore) relayPreferences.putBool("booted", true);

    bool nvsRelay = false;
    if (readRelayStateFromNvs(nvsRelay)) {
      savedRelay = nvsRelay;
      savedRelayAvailable = true;
      // NVS 为跨断电的持久记录；若与 RTC 不同，以 NVS 为准。
      if (!rtcRelayStateValid || relayState != nvsRelay) {
        relayState = nvsRelay;
        digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
      }
    }
  }

  if (savedRelayAvailable) {
    firstBootDetected = false;
    relayRtcStateValue = savedRelay ? 1U : 0U;
    relayRtcStateMagic = RELAY_RTC_MAGIC;
    Serial.printf("[Boot] 启动早期恢复继电器状态：%s\n", savedRelay ? "ON" : "OFF");
  }

  // 1. 文件系统：历史图表、配置与旧版本继电器状态文件。
  bool littleFsReady = LittleFS.begin(true);
  if (littleFsReady) {
    logBootReason();
    File vlogFile = LittleFS.open(VLOG_FILE_PATH, "r+");
    if (!vlogFile) {
      vlogFile = LittleFS.open(VLOG_FILE_PATH, "w+");
      if (vlogFile) {
        int initialIndex = 0; vlogFile.write((byte*)&initialIndex, sizeof(int));
        float nan_val = NAN;
        for (int i = 0; i < DATA_POINTS; i++) vlogFile.write((byte*)&nan_val, sizeof(float));
        vlogFile.close();
      }
    } else {
      vlogFile.read((byte*)&historyIndex, sizeof(int));
      vlogFile.close();
    }

    // 兼容升级前只把继电器状态存于 /relay.txt 的设备。
    if (!savedRelayAvailable && LittleFS.exists(RELAY_STATE_PATH)) {
      File relayFile = LittleFS.open(RELAY_STATE_PATH, "r");
      if (relayFile) {
        int c = relayFile.read(); relayFile.close();
        if (c == '0' || c == '1') {
          savedRelay = (c == '1');
          savedRelayAvailable = true;
          firstBootDetected = false;
          relayState = savedRelay;
          digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
          relayRtcStateValue = relayState ? 1U : 0U;
          relayRtcStateMagic = RELAY_RTC_MAGIC;
          if (relayPreferencesReady) {
            relayPreferences.putBool("state", savedRelay);
            relayPreferences.putBool("has_state", true);
            relayPreferences.putBool("booted", true);
          }
        }
      }
    }

    loadSettings();
    Notify::loadConfig();
    setLocalTimezone();
    restoreClockSnapshotIfNeeded();
  } else {
    Serial.println("[Boot] LittleFS 挂载失败，无法读取历史时间/旧版状态文件");
    setLocalTimezone();
  }

  // 2. I2C 初始化（使用 ESP32-C3 安全引脚）
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (ina219.begin()) { 
    ina219_ok = true; 
    Serial.println("[OK] INA219 Ready."); 
  }

  // 4. 读当前电压
  if (ina219_ok) busVoltage = ina219.getBusVoltage_V();

  // 5. 非阻塞启动 Wi-Fi：不再连续等待 3×10 秒。网页服务和灯板会先启动，网络在 loop 中重连。
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  lastWifiAttemptMs = millis();
  WiFi.begin(ssid, password);  // 只发起连接，不在 setup 里等待结果

  // 继电器状态先于联网恢复。真正首次启动且没有历史状态时，暂时保持 OFF，给 Wi-Fi 15 秒判断窗口。
  if (savedRelayAvailable) {
    Serial.printf("[Boot] 启动时恢复上次继电器状态：%s\n", savedRelay ? "ON" : "OFF");
    setRelay(savedRelay);
  } else if (firstBootDetected) {
    Serial.println("[Boot] 首次启动：先保持继电器 OFF，15 秒内若仍无 Wi-Fi 才默认 ON");
    setRelay(false);
    initialOfflineDefaultPending = true;
    initialOfflineDefaultDeadline = millis() + 15000UL;
  } else {
    Serial.println("[Boot] 未发现有效历史状态且不是首次启动，默认 OFF");
    setRelay(false);
  }

  // 在线/离线都注册 API 路由，不再因为启动时没网而跳过路由配置。
  server.on("/setNotifyApi", HTTP_GET, handleSetNotifyApi);

  smtp.debug(1);
  smtp.callback(smtpCallback);

  // 其他路由与 myClock 路由统一在下方注册，最后启动 server。

  // mDNS 在 Wi-Fi 真正连接后由 loop 中的连接状态处理分支启动。

  server.on("/", HTTP_GET, handleRoot);
  server.on("/getData", HTTP_GET, handleGetData);
  server.on("/getStatus", HTTP_GET, handleGetStatus);
  server.on("/setRelay", HTTP_GET, handleSetRelay);
  server.on("/setSettings", HTTP_GET, handleSetSettings);
  server.on("/setEmail", HTTP_GET, handleSetEmail);
  server.on("/getChartData", HTTP_GET, handleGetChartData);
  server.on("/testEmail", HTTP_GET, handleTestEmail);
  server.on("/testEmailResult", HTTP_GET, handleTestEmailResult);
  ipv6Mgr.begin(server);  // 注册 /getIPv6 路由（纯文本，供 Go DDNS 拉取）

  // 【修改】ESP32 Web 固件在线更新端点 (OTA)
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", Update.hasError() ? OTA_FAIL_HTML : OTA_SUCCESS_HTML);
    if (!Update.hasError()) { delay(1000); ESP.restart(); }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) { 
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { 
        Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) { 
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_END) { 
      if (!Update.end(true)) {
        Update.printError(Serial);
      }
    }
  });

  setupLogEndpoint(server);
  myClock.begin();               // 先注册时钟路由并初始化灯板
  server.begin();

  // ESP32-C3 是单核；独立的优先级 2 灯板任务可与优先级 1 的 HTTP/控制 loop 交替运行。
  BaseType_t matrixTaskResult = xTaskCreate(
    matrixClockTask, "matrixClock", 4096, nullptr, 2, nullptr
  );
  matrixClockTaskRunning = (matrixTaskResult == pdPASS);
  Serial.println(matrixClockTaskRunning ? "[MatrixTask] 灯板独立任务已启动" : "[MatrixTask] 创建失败，将由主 loop 刷新灯板");

  Serial.println("[Recovery] 自动/手动屏幕恢复只操作外部高侧电子开关，不软件重启 ESP32。");
}

void loop() {
  unsigned long loopStart = micros(); // 记录本轮循环开始时间 (微秒)

  // WebServer 仍在主 loop 顺序处理；高优先级灯板任务独立运行，不会被 HTTP 卡住。
  server.handleClient();

  // 首次启动且没有历史继电器状态时，等待 Wi-Fi 判断窗口结束；仅确认离线才默认开启。
  if (initialOfflineDefaultPending) {
    if (WiFi.status() == WL_CONNECTED) {
      initialOfflineDefaultPending = false;
      Serial.println("[Boot] Wi-Fi 在判断窗口内连接成功，首次启动继电器保持 OFF");
      setRelay(false);
    } else if ((int32_t)(millis() - initialOfflineDefaultDeadline) >= 0) {
      initialOfflineDefaultPending = false;
      if (firstBootDetected) {
        Serial.println("[Boot] 首次启动 15 秒后仍无 Wi-Fi，按要求默认开启继电器");
        setRelay(true);
      }
    }
  }

  // 系统时钟靠硬件计时持续走时，不需要每几秒访问 NTP。每小时只在联网时重新配置后台 SNTP。
  static unsigned long lastNtpConfig = 0;
  if (lastNtpConfig == 0) lastNtpConfig = millis();
  if (WiFi.status() == WL_CONNECTED && millis() - lastNtpConfig >= 3600000UL) {
    lastNtpConfig = millis();
    configureNetworkTimeSync(); // 后台同步，不在 loop 中等待网络响应
  }
  if (pendingTestEmail) {
    pendingTestEmail = false;
    // 灯板在独立高优先级任务中刷新，不依赖邮件发送完成。
    sendEmailNotification("[测试] " + String(deviceName), "这是一封测试邮件，邮件通知功能正常。");
    testEmailResult = (smtp.statusCode() > 0 && smtp.statusCode() < 400) 
                      ? "ok" 
                      : ("fail:" + smtp.errorReason());
  }
  if (pendingWarningEmail || pendingLockoutEmail) {
      pendingWarningEmail = false;
      pendingLockoutEmail = false;
      
      sendEmailNotification(pendingEmailSubject, pendingEmailBody);
  }
  if (pendingNotifyWarning) {
      pendingNotifyWarning = false;

      String title = "🚨 逆变器低电压警告";

      String message =
          "逆变器电压异常，当前电压 " +
          String(busVoltage, 2) +
          "V，警告阈值 " +
          String(warningVoltageThreshold, 2) +
          "V";

      bool ok = Notify::send(
          title,
          message,
          String(deviceName),
          "critical",
          "逆变器",
          3600
      );

      if (ok) {
          Serial.println("[Notify] 电压警告 API 发送成功");
      } else {
          Serial.println("[Notify] 电压警告 API 发送失败");
      }
  }

  // 灯板更新由 matrixClockTask 独立执行，不放在可能被网络/文件操作阻塞的 loop 中。
  if (!matrixClockTaskRunning) {
    myClock.setBatteryVoltage(busVoltage);
    myClock.loop();
  }

  // WiFi 自动重连
  static unsigned long lastWifiCheck = 0;
  static bool ntpSynced = wifiConnected;
  static bool prevWifiOk = wifiConnected;
  if (millis() - lastWifiCheck > 15000) { 
    lastWifiCheck = millis();
    bool curWifiOk = (WiFi.status() == WL_CONNECTED);
    if (!curWifiOk) {
      if (prevWifiOk) {
        ntpSynced = false;
        sysLog(LOG_WARN, "WiFi断线，等待重连...");
      }
      if (millis() - lastWifiAttemptMs >= 15000UL) {
        Serial.println("[WiFi] 未连接，非阻塞发起重连...");
        lastWifiAttemptMs = millis();
        WiFi.begin(ssid, password);
      }
    } else if (!ntpSynced) {
      ntpSynced = true;
      WiFi.setSleep(false); 
      WiFi.enableIPv6();        // ← 移到这里，连上之后再启用
      ipv6Mgr.onWiFiConnected();
      sysLogf(LOG_INFO, "WiFi已连接(重连) IP=%s", WiFi.localIP().toString().c_str());
      configureNetworkTimeSync();
      Serial.println("[WiFi] 重连成功，后台 NTP 校时已启动");
      if (MDNS.begin(deviceName)) MDNS.addService("http", "tcp", WEB_SERVER_PORT);
    }
    prevWifiOk = curWifiOk;
  }

  static unsigned long lastVoltageCheck = 0;
  if (millis() - lastVoltageCheck > 5000) {
    lastVoltageCheck = millis();
    checkVoltageProtection();
  }

  static unsigned long lastHistoryRecord = 0;
  if (millis() - lastHistoryRecord > DATA_INTERVAL_MS) {
    lastHistoryRecord = millis();
    recordVoltageHistory();
  }

  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 60000) {
    lastHeartbeat = millis();
    saveClockSnapshot();
  }

  // 累计本轮实际工作做功时长 (微秒)
  unsigned long loopEnd = micros();
  if (loopEnd >= loopStart) {
    totalWorkTime += (loopEnd - loopStart);
  }

  // 每 1 秒精准计算一次 CPU 真实的做功耗时比例
  if (millis() - lastCpuMeasureTime >= CPU_MEASURE_INTERVAL_MS) {
    unsigned long totalElapsedUs = (millis() - lastCpuMeasureTime) * 1000; // 转换为微秒
    if (totalElapsedUs > 0) {
      cpuUsage = ((float)totalWorkTime / totalElapsedUs) * 100.0;
      if (cpuUsage > 100.0) cpuUsage = 100.0;
    }
    totalWorkTime = 0;
    lastCpuMeasureTime = millis();
  }

  // 主动让出 CPU，给 Wi-Fi/TCP 栈调度时间。
  delay(5);
}