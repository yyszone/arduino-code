// =================================================================================================
// ==   ESP8266 智能继电器 & INA219高精度电压表 v6.2 (优化通知逻辑版)             ==
// =================================================================================================
// 描述: 此版本根据用户反馈进行优化：
//       1. 增加1小时通知冷却机制，防止因电压波动在短时间内重复发送警告或锁定通知。
//       2. 更新了Web UI在锁定状态下显示的文本。
//       代码逻辑确保了发送警告通知绝不会导致继电器关闭。
// =================================================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <time.h>
#include <ESP_Mail_Client.h>
#include <WiFiClientSecure.h>

// ============== 用户配置 ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp8266-smart-relay";
const int WEB_SERVER_PORT = 80;

// ============== 邮件与IFTTT通知配置 (请二选一或全部注释) ==============
#define ENABLE_EMAIL_NOTIFICATION 
// #define ENABLE_IFTTT_NOTIFICATION 

#if defined(ENABLE_EMAIL_NOTIFICATION)
  const char* SMTP_HOST             = "smtp.qq.com";
  const int   SMTP_PORT             = 465;
  const char* AUTHOR_EMAIL          = "534640040@qq.com";
  const char* AUTHOR_PASSWORD       = "zkddrplhdfabbjah";
  const char* RECIPIENT_EMAIL       = "534640040@qq.com";
  // 新版库的时间设置
  const char* NTP_SERVERS           = "ntp.aliyun.com, pool.ntp.org, time.nist.gov";
  const int   GMT_OFFSET            = 8; // 东八区为8
  const int   DAYLIGHT_OFFSET       = 0; // 夏令时偏移
#elif defined(ENABLE_IFTTT_NOTIFICATION)
  const char* IFTTT_API_KEY         = "YOUR_IFTTT_KEY_HERE";
  const char* IFTTT_EVENT_NAME      = "esp_voltage_alert";
#endif


// ============== 硬件引脚配置 ==============
const int RELAY_PIN   = 14; // D5
const int I2C_SDA_PIN = 4;  // D2
const int I2C_SCL_PIN = 5;  // D1

// ============== 时间与定时任务配置 ==============
const char* WEB_UI_NTP_SERVER = "ntp.aliyun.com";
const long  WEB_UI_GMT_OFFSET_SEC = 8 * 3600;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, WEB_UI_NTP_SERVER, WEB_UI_GMT_OFFSET_SEC);

// ============== EEPROM 存储地址定义 ==============
const int EEPROM_SIZE = 64; 
const int ADDR_MAGIC_NUM   = 0;  // 2字节
const int ADDR_LOW_V       = 2;  // 4字节
const int ADDR_HIGH_V      = 6;  // 4字节
const int ADDR_WARN_V      = 10; // 4字节
const uint16_t EEPROM_MAGIC_NUMBER = 0x5A1D;

// ============== 全局对象与变量 ==============
ESP8266WebServer server(WEB_SERVER_PORT);
Adafruit_INA219 ina219;
SMTPSession smtp;

bool relayState = false, ina219_ok = false;
float busVoltage = 0;
float lowVoltageThreshold = 10.5, highVoltageThreshold = 12.8, warningVoltageThreshold = 11.5;
bool isLockedOut = false;
bool isVoltageWarning = false;
unsigned long lockoutStartTime = 0;
const unsigned long LOCKOUT_DURATION_MS = 3600000;

// --- 智能防误报变量 ---
float lastBusVoltage = 0;
const float WARNING_HYSTERESIS_V = 0.5;

// --- v6.2 新增: 通知冷却机制 ---
const unsigned long NOTIFICATION_COOLDOWN_MS = 3600000; // 1小时冷却时间
unsigned long lastWarningNoticeTime = 0; // 上次发送警告通知的时间戳
unsigned long lastLockoutNoticeTime = 0; // 上次发送锁定通知的时间戳


// =====================================================
// ============== 网页 (HTML+CSS+JS) v6.2 ==============
// =====================================================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP8266 智能继电器</title><style>:root{--bg-color:#111827;--card-color:#1f2937;--text-color:#d1d5db;--accent-color:#38bdf8;--green-color:#22c55e;--red-color:#ef4444;--warning-color:#f59e0b;--muted-text:#9ca3af}body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";background-color:var(--bg-color);color:var(--text-color);margin:0;padding:15px;display:flex;justify-content:center}h1,h2,h4{margin-top:0;color:#fff;text-align:center}h2{border-top:1px solid #374151;padding-top:15px;margin-top:20px}.container{width:100%;max-width:500px}.card{background-color:var(--card-color);border-radius:12px;padding:20px;margin-bottom:15px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1),0 2px 4px -1px rgba(0,0,0,.06)}.data-box{text-align:center;padding:10px}.data-box .val{font-size:2.5em;font-weight:700;color:var(--accent-color);line-height:1.2;transition:color .3s ease}.data-box .unit{color:var(--muted-text)}.btn{width:100%;padding:15px;font-size:1.2em;font-weight:bold;border:none;border-radius:8px;cursor:pointer;transition:background-color .2s ease}.btn.on{background-color:var(--green-color);color:#fff}.btn.off{background-color:var(--red-color);color:#fff}.status-light{width:12px;height:12px;border-radius:50%;display:inline-block;margin-right:8px;background-color:#6b7280}.status-light.on{background-color:var(--green-color)}.input-group{display:flex;align-items:center;gap:10px;margin-bottom:10px}.input-group label{flex-basis:120px;flex-shrink:0}input[type=number]{width:100%;padding:8px;background-color:#374151;border:1px solid #4b5563;border-radius:6px;color:var(--text-color);font-size:1em}.btn-save{padding:10px 15px;background-color:var(--accent-color);color:#fff;border:none;border-radius:6px;cursor:pointer}#sysinfo{font-size:.8em;color:var(--muted-text);word-break:break-all}#lockoutStatus{color:var(--red-color);text-align:center;margin-bottom:10px;font-weight:bold;}</style></head><body><div class="container"><h1>ESP8266 智能继电器</h1><p style="text-align:center;color:var(--muted-text);">当前时间: <span id="currentTime">--:--:--</span></p><div class="card"><div class="data-box"><div>电池电压</div><div class="val" id="v">--</div><div class="unit">V</div></div></div><div class="card"><h2>手动控制</h2><div id="lockoutStatus" style="display:none;"></div><p><span id="relayStatusLight" class="status-light"></span>继电器状态: <strong id="relayStatusText">读取中...</strong></p><button id="relayBtn" class="btn">读取中...</button></div><div class="card"><h2>参数设置</h2><div class="input-group"><label for="highV">高压开启 (V)</label><input type="number" id="highV" step="0.1"></div><div class="input-group"><label for="warnV">电压警告 (V)</label><input type="number" id="warnV" step="0.1"></div><div class="input-group"><label for="lowV">低压关闭 (V)</label><input type="number" id="lowV" step="0.1"></div><div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveSettings()">保存设置</button></div></div><div class="card"><h2>系统信息与更新</h2><div id="sysinfo">加载中...</div><h4>固件更新 (OTA)</h4><div id="otaUi"><form id="otaForm" method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz" required><button type="submit" class="btn-save" style="margin-top:10px;">上传并更新</button></form></div><div id="otaStatus"></div></div></div>
<script>
function $(s){return document.getElementById(s)}
function fetchJson(url,options){return fetch(url,options).then(r=>{if(!r.ok)throw new Error('Network error');return r.json()})}
function updateStatus(data){
  const relayOn=data.relay;
  $('relayStatusText').textContent=relayOn?'已开启':'已关闭';
  $('relayStatusText').style.color=relayOn?'var(--green-color)':'var(--red-color)';
  $('relayStatusLight').className=relayOn?'status-light on':'status-light';
  $('relayBtn').textContent=relayOn?'关闭继电器':'开启继电器';
  $('relayBtn').className=relayOn?'btn off':'btn on';
  /* v6.2: 更新锁定状态的显示文本 */
  if(data.lockout){$('lockoutStatus').style.display='block';$('lockoutStatus').textContent='电压警告中！剩余 '+data.lockout_rem+' 分钟可自动恢复。'}else{$('lockoutStatus').style.display='none';}
}
function fetchData(){fetchJson('/getData').then(data=>{
  $('v').textContent=data.voltage.toFixed(2);
  $('v').style.color=data.voltage_warning?'var(--warning-color)':'var(--accent-color)';
  $('currentTime').textContent=data.time;
  updateStatus(data);
})}
function fetchInitialState(){fetchJson('/getStatus').then(data=>{
  updateStatus(data);
  $('lowV').value=data.low_v;
  $('warnV').value=data.warn_v;
  $('highV').value=data.high_v;
  $('sysinfo').innerHTML=`IPv4: ${data.ip}<br>芯片ID: ${data.chip_id}<br>空闲内存: ${data.free_heap} B`;
})}
function saveSettings(){
  const lowV=$('lowV').value;
  const warnV=$('warnV').value;
  const highV=$('highV').value;
  fetch(`/setSettings?low=${lowV}&warn=${warnV}&high=${highV}`).then(r=>{if(r.ok){alert('设置已保存!')}else{alert('保存失败!')}}).catch(e=>alert('请求出错: '+e));
}
$('relayBtn').addEventListener('click',()=>{const newState=$('relayBtn').classList.contains('on');fetch('/setRelay?state='+(newState?'1':'0')).then(()=>setTimeout(fetchData,200))});
$('otaForm').addEventListener('submit', function(e) {
  $('otaUi').style.display = 'none';
  $('otaStatus').innerHTML = '<h4>正在上传并更新...</h4><p>请勿关闭此页面或断开设备电源。设备将在大约一分钟后自动重启。</p>';
});
window.addEventListener('load',()=>{fetchInitialState();fetchData();});
setInterval(fetchData,2500);
</script></body></html>
)HTML";

const char OTA_SUCCESS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新成功</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#22c55e;}</style></head><body><div><h1>更新成功!</h1><p>设备正在重启，请在约1分钟后重新连接。</p></div></body></html>)HTML";
const char OTA_FAIL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新失败</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#ef4444;}</style></head><body><div><h1>更新失败!</h1><p>请检查上传的固件文件(.bin)是否正确，然后返回重试。</p></div></body></html>)HTML";


// ============== 通知发送函数 (适配最新版库 v3/v4 API) ==============
#if defined(ENABLE_EMAIL_NOTIFICATION)
void smtpCallback(SMTP_Status status){
  Serial.println(status.info());
  if (status.success()){
    Serial.println("----------------");
    MailClient.printf("Message sent success: %d\n", status.completedCount());
    MailClient.printf("Message sent failed: %d\n", status.failedCount());
    Serial.println("----------------\n");
  }
}

void sendEmailNotification(String subject, String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[!!] 无法发送邮件，WiFi未连接。");
    return;
  }
  
  Session_Config config;
  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_PASSWORD;
  config.login.user_domain = F("127.0.0.1");

  config.time.ntp_server = NTP_SERVERS;
  config.time.gmt_offset = GMT_OFFSET;
  config.time.day_light_offset = DAYLIGHT_OFFSET;

  SMTP_Message email;
  email.sender.name = F("ESP8266 继电器");
  email.sender.email = AUTHOR_EMAIL;
  email.subject = subject;
  email.addRecipient(F("User"), RECIPIENT_EMAIL);
  
  String htmlMsg = "<h2>" + subject + "</h2><p>" + message + "</p><p>设备名称: " + String(deviceName) + "</p><p>当前时间: " + timeClient.getFormattedTime() + "</p>";
  email.html.content = htmlMsg;
  email.html.transfer_encoding = Content_Transfer_Encoding::enc_base64;

  Serial.println("准备发送邮件...");
  if (!smtp.connect(&config)) {
    MailClient.printf("Connection error, Status Code: %d, Error Code: %d, Reason: %s\n", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());
    return;
  }

  if (!MailClient.sendMail(&smtp, &email)) {
    MailClient.printf("Error, Status Code: %d, Error Code: %d, Reason: %s\n", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());
  }
}
#elif defined(ENABLE_IFTTT_NOTIFICATION)
void sendIFTTTNotification(String value1, String value2) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[!!] 无法发送IFTTT通知，WiFi未连接。");
    return;
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  
  if (!client.connect("maker.ifttt.com", 443)) {
    Serial.println("[!!] IFTTT 连接失败!");
    return;
  }

  String url = String("/trigger/") + IFTTT_EVENT_NAME + "/with/key/" + IFTTT_API_KEY +
               "?value1=" + value1 + "&value2=" + value2;
  
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: maker.ifttt.com\r\n" +
               "Connection: close\r\n\r\n");
  
  client.stop();
  Serial.println("[OK] IFTTT请求已发送。");
}
#endif

// ============== EEPROM 函数 ==============
void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint16_t magic; EEPROM.get(ADDR_MAGIC_NUM, magic);
  if (magic == EEPROM_MAGIC_NUMBER) {
    Serial.println("[OK] 从EEPROM加载数据...");
    EEPROM.get(ADDR_LOW_V, lowVoltageThreshold); 
    EEPROM.get(ADDR_HIGH_V, highVoltageThreshold);
    EEPROM.get(ADDR_WARN_V, warningVoltageThreshold);
  } else {
    Serial.println("[!!] EEPROM未初始化, 使用默认值并写入。");
    EEPROM.put(ADDR_MAGIC_NUM, EEPROM_MAGIC_NUMBER); 
    EEPROM.put(ADDR_LOW_V, lowVoltageThreshold);
    EEPROM.put(ADDR_HIGH_V, highVoltageThreshold);
    EEPROM.put(ADDR_WARN_V, warningVoltageThreshold);
    EEPROM.commit();
  }
  EEPROM.end();
  Serial.printf("  -> 低压:%.2fV, 警告:%.2fV, 高压:%.2fV\n", lowVoltageThreshold, warningVoltageThreshold, highVoltageThreshold);
}
void saveSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(ADDR_LOW_V, lowVoltageThreshold); 
  EEPROM.put(ADDR_HIGH_V, highVoltageThreshold);
  EEPROM.put(ADDR_WARN_V, warningVoltageThreshold);
  EEPROM.commit(); 
  EEPROM.end();
  Serial.println("[OK] 电压阈值已保存到EEPROM。");
}


// ============== 核心功能函数 (v6.2 优化通知逻辑) ==============
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  Serial.printf("继电器 (Pin %d) 已设置为: %s\n", RELAY_PIN, relayState ? "ON" : "OFF");
}

void checkVoltageProtection() {
  if (!ina219_ok) return;
  busVoltage = ina219.getBusVoltage_V();
  
  bool voltageIsDecreasing = (busVoltage < (lastBusVoltage - 0.02));

  // 1. 智能电压警告逻辑
  if (relayState && busVoltage <= warningVoltageThreshold && voltageIsDecreasing) {
      isVoltageWarning = true;
      // v6.2: 增加1小时冷却判断
      if (millis() - lastWarningNoticeTime > NOTIFICATION_COOLDOWN_MS || lastWarningNoticeTime == 0) {
          Serial.printf("!!! 电压警告 (%.2fV <= %.2fV 且呈下降趋势)，发送通知。\n", busVoltage, warningVoltageThreshold);
          #if defined(ENABLE_EMAIL_NOTIFICATION)
            String subject = "[电压警告] " + String(deviceName);
            String message = "设备当前电压为 " + String(busVoltage, 2) + "V，已低于警告阈值 " + String(warningVoltageThreshold, 2) + "V。";
            sendEmailNotification(subject, message);
          #elif defined(ENABLE_IFTTT_NOTIFICATION)
            sendIFTTTNotification(String(deviceName) + "%20Voltage%20Warning", String(busVoltage, 2));
          #endif
          lastWarningNoticeTime = millis(); // 更新发送时间戳
      }
  } 
  else if (busVoltage > (warningVoltageThreshold + WARNING_HYSTERESIS_V)) {
      if (isVoltageWarning) {
          Serial.printf("[OK] 电压已恢复到 %.2fV，高于警告重置阈值(%.2fV)。解除警告。\n", busVoltage, warningVoltageThreshold + WARNING_HYSTERESIS_V);
      }
      isVoltageWarning = false;
  }
  else if (busVoltage > warningVoltageThreshold) {
      isVoltageWarning = false;
  }

  // 2. 锁定解除逻辑
  if (isLockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_DURATION_MS) {
      Serial.println("[OK] 1小时锁定时间已到，解除锁定。"); 
      isLockedOut = false;
    } else { 
      lastBusVoltage = busVoltage;
      return; 
    }
  }

  // 3. 高压开启逻辑
  if (!relayState && !isLockedOut && busVoltage >= highVoltageThreshold) {
    Serial.printf("检测到高电压 (%.2fV >= %.2fV)，自动开启继电器。\n", busVoltage, highVoltageThreshold);
    setRelay(true);
  }
  
  // 4. 低压关断逻辑
  if (relayState && busVoltage > 0.1 && busVoltage < lowVoltageThreshold) {
    Serial.printf("!!! 触发低压保护 (%.2fV < %.2fV)，自动关闭继电器并锁定1小时。\n", busVoltage, lowVoltageThreshold);
    // v6.2: 增加1小时冷却判断
    if (millis() - lastLockoutNoticeTime > NOTIFICATION_COOLDOWN_MS || lastLockoutNoticeTime == 0) {
      #if defined(ENABLE_EMAIL_NOTIFICATION)
          String subject = "[严重] 低压保护已触发! " + String(deviceName);
          String message = "设备当前电压为 " + String(busVoltage, 2) + "V，已触发低压保护 (" + String(lowVoltageThreshold, 2) + "V)。继电器已关闭并锁定1小时。";
          sendEmailNotification(subject, message);
      #elif defined(ENABLE_IFTTT_NOTIFICATION)
          sendIFTTTNotification(String(deviceName) + "%20Low%20Voltage%20Shutdown", String(busVoltage, 2));
      #endif
      lastLockoutNoticeTime = millis(); // 更新发送时间戳
    }
    isLockedOut = true; 
    lockoutStartTime = millis(); 
    setRelay(false);
  }
  lastBusVoltage = busVoltage;
}


// ============== Web路由处理函数 ==============
void handleRoot() { server.send(200, "text/html; charset=UTF-8", MAIN_HTML); }
void handleGetData() {
  if (ina219_ok) busVoltage = ina219.getBusVoltage_V();
  timeClient.update();
  long remaining_min = 0;
  if (isLockedOut) {
    unsigned long elapsed_ms = millis() - lockoutStartTime;
    if (elapsed_ms < LOCKOUT_DURATION_MS) remaining_min = (LOCKOUT_DURATION_MS - elapsed_ms) / 60000;
  }
  String json = "{";
  json += "\"voltage\":" + String(busVoltage, 2) + ",";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"time\":\"" + timeClient.getFormattedTime() + "\",";
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
  json += "\"chip_id\":\"" + String(ESP.getChipId(), HEX) + "\",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap());
  json += "}";
  server.send(200, "application/json", json);
}
void handleSetRelay() {
  if (server.hasArg("state")) {
    bool newState = server.arg("state").toInt() == 1;
    if (newState && isLockedOut) {
      Serial.println("[OK] 手动操作覆盖了低压锁定。");
      isLockedOut = false;
    }
    setRelay(newState);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}
void handleSetSettings() {
  if (server.hasArg("low") && server.hasArg("high") && server.hasArg("warn")) {
    lowVoltageThreshold = server.arg("low").toFloat();
    highVoltageThreshold = server.arg("high").toFloat();
    warningVoltageThreshold = server.arg("warn").toFloat();
    saveSettings();
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}


// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP8266 智能继电器 & INA219电压表 v6.2 (优化通知逻辑版) =====");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  relayState = true;
  Serial.println("[OK] 继电器已设置为默认开启状态。");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!ina219.begin()) {
    Serial.println("[!!] 硬件错误: 未能找到INA219芯片! 请检查I2C接线。");
    ina219_ok = false;
  } else {
    Serial.println("[OK] INA219 通信成功。");
    ina219_ok = true;
  }

  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.hostname(deviceName);
  WiFi.begin(ssid, password);
  Serial.print("连接 Wi-Fi");
  int wifi_retries = 20;
  while (WiFi.status() != WL_CONNECTED && wifi_retries > 0) { delay(500); Serial.print("."); wifi_retries--; }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[!!] WiFi 连接失败! 设备将重启。");
    delay(1000); ESP.restart();
  }
  Serial.println("\n[OK] WiFi 已连接!");
  Serial.print("IPv4 地址: "); Serial.println(WiFi.localIP());
  
  timeClient.begin();
  timeClient.forceUpdate();
  Serial.println("[OK] Web UI的NTP 时间服务已同步。");

  #if defined(ENABLE_EMAIL_NOTIFICATION)
    smtp.debug(1);
    smtp.callback(smtpCallback);
  #endif
  
  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", WEB_SERVER_PORT);
    Serial.printf("[OK] mDNS 已启动, 访问: http://%s.local\n", deviceName);
  }
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/getData", HTTP_GET, handleGetData);
  server.on("/getStatus", HTTP_GET, handleGetStatus);
  server.on("/setRelay", HTTP_GET, handleSetRelay);
  server.on("/setSettings", HTTP_GET, handleSetSettings);
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", Update.hasError() ? OTA_FAIL_HTML : OTA_SUCCESS_HTML);
    if (!Update.hasError()) { delay(1000); ESP.restart(); }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA] Update Start: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) { Update.printError(Serial); }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { Update.printError(Serial); }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { Serial.printf("[OTA] Update Success: %u bytes\n", upload.totalSize); } 
      else { Update.printError(Serial); }
    }
  });
  
  server.begin();
  Serial.printf("[OK] HTTP 服务器已启动。\n========================================\n");
}

// ============== LOOP ==============
void loop() {
  server.handleClient();
  MDNS.update();

  static unsigned long lastVoltageCheck = 0;
  if (millis() - lastVoltageCheck > 5000) {
    lastVoltageCheck = millis();
    checkVoltageProtection();
  }
}