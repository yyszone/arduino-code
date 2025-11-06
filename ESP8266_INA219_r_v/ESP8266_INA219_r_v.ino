// =================================================================================================
// ==      ESP8266 智能继电器 & INA219高精度电压表 v5.1 (最终标准-仅电压版)      ==
// =================================================================================================
// 描述: 本方案将INA219仅作为高精度电压表使用，通过并联方式测量电池电压，
//       完全避免了在主回路中串联分流电阻所带来的压降问题。
//       这是兼顾测量精度与大功率负载兼容性的最佳方案。
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

// ============== 用户配置 ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp8266-smart-relay";
const int WEB_SERVER_PORT = 80;

// ============== 硬件引脚配置 ==============
const int RELAY_PIN   = 14; // D5
const int I2C_SDA_PIN = 4;  // D2
const int I2C_SCL_PIN = 5;  // D1

// ============== 时间与定时任务配置 ==============
const char* NTP_SERVER = "ntp.aliyun.com";
const long  GMT_OFFSET_SEC = 8 * 3600;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, GMT_OFFSET_SEC);

// ============== EEPROM 存储地址定义 ==============
const int EEPROM_SIZE = 64; 
const int ADDR_MAGIC_NUM   = 0;  // 2字节
const int ADDR_LOW_V       = 2;  // 4字节
const int ADDR_HIGH_V      = 6;  // 4字节
const uint16_t EEPROM_MAGIC_NUMBER = 0x5A1C; // 全新魔法数字

// ============== 全局对象与变量 ==============
ESP8266WebServer server(WEB_SERVER_PORT);
Adafruit_INA219 ina219;

bool relayState = false, ina219_ok = false;
float busVoltage = 0;
float lowVoltageThreshold = 10.5, highVoltageThreshold = 12.8;
bool isLockedOut = false;
unsigned long lockoutStartTime = 0;
const unsigned long LOCKOUT_DURATION_MS = 3600000;


// =====================================================
// ============== 网页 (HTML+CSS+JS) v5.1 ==============
// =====================================================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP8266 智能继电器</title><style>:root{--bg-color:#111827;--card-color:#1f2937;--text-color:#d1d5db;--accent-color:#38bdf8;--green-color:#22c55e;--red-color:#ef4444;--muted-text:#9ca3af}body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";background-color:var(--bg-color);color:var(--text-color);margin:0;padding:15px;display:flex;justify-content:center}h1,h2,h4{margin-top:0;color:#fff;text-align:center}h2{border-top:1px solid #374151;padding-top:15px;margin-top:20px}.container{width:100%;max-width:500px}.card{background-color:var(--card-color);border-radius:12px;padding:20px;margin-bottom:15px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1),0 2px 4px -1px rgba(0,0,0,.06)}.data-box{text-align:center;padding:10px}.data-box .val{font-size:2.5em;font-weight:700;color:var(--accent-color);line-height:1.2}.data-box .unit{color:var(--muted-text)}.btn{width:100%;padding:15px;font-size:1.2em;font-weight:bold;border:none;border-radius:8px;cursor:pointer;transition:background-color .2s ease}.btn.on{background-color:var(--green-color);color:#fff}.btn.off{background-color:var(--red-color);color:#fff}.status-light{width:12px;height:12px;border-radius:50%;display:inline-block;margin-right:8px;background-color:#6b7280}.status-light.on{background-color:var(--green-color)}.input-group{display:flex;align-items:center;gap:10px;margin-bottom:10px}.input-group label{flex-basis:120px;flex-shrink:0}input[type=number]{width:100%;padding:8px;background-color:#374151;border:1px solid #4b5563;border-radius:6px;color:var(--text-color);font-size:1em}.btn-save{padding:10px 15px;background-color:var(--accent-color);color:#fff;border:none;border-radius:6px;cursor:pointer}#sysinfo{font-size:.8em;color:var(--muted-text);word-break:break-all}#lockoutStatus{color:var(--red-color);text-align:center;margin-bottom:10px;font-weight:bold;}</style></head><body><div class="container"><h1>ESP8266 智能继电器</h1><p style="text-align:center;color:var(--muted-text);">当前时间: <span id="currentTime">--:--:--</span></p><div class="card"><div class="data-box"><div>电池电压</div><div class="val" id="v">--</div><div class="unit">V</div></div></div><div class="card"><h2>手动控制</h2><div id="lockoutStatus" style="display:none;"></div><p><span id="relayStatusLight" class="status-light"></span>继电器状态: <strong id="relayStatusText">读取中...</strong></p><button id="relayBtn" class="btn">读取中...</button></div><div class="card"><h2>参数设置</h2><div class="input-group"><label for="highV">高压开启 (V)</label><input type="number" id="highV" step="0.1"></div><div class="input-group"><label for="lowV">低压关闭 (V)</label><input type="number" id="lowV" step="0.1"></div><div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveSettings()">保存设置</button></div></div><div class="card"><h2>系统信息与更新</h2><div id="sysinfo">加载中...</div><h4>固件更新 (OTA)</h4><div id="otaUi"><form id="otaForm" method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz" required><button type="submit" class="btn-save" style="margin-top:10px;">上传并更新</button></form></div><div id="otaStatus"></div></div></div>
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
  if(data.lockout){$('lockoutStatus').style.display='block';$('lockoutStatus').textContent='低压保护锁定中！剩余 '+data.lockout_rem+' 分钟可自动恢复。'}else{$('lockoutStatus').style.display='none';}
}
function fetchData(){fetchJson('/getData').then(data=>{
  $('v').textContent=data.voltage.toFixed(2);
  $('currentTime').textContent=data.time;
  updateStatus(data);
})}
function fetchInitialState(){fetchJson('/getStatus').then(data=>{
  updateStatus(data);
  $('lowV').value=data.low_v;
  $('highV').value=data.high_v;
  $('sysinfo').innerHTML=`IPv4: ${data.ip}<br>芯片ID: ${data.chip_id}<br>空闲内存: ${data.free_heap} B`;
})}
function saveSettings(){
  const lowV=$('lowV').value;
  const highV=$('highV').value;
  fetch(`/setSettings?low=${lowV}&high=${highV}`).then(r=>{if(r.ok){alert('设置已保存!')}else{alert('保存失败!')}}).catch(e=>alert('请求出错: '+e));
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

// (OTA HTML 代码与之前相同)
const char OTA_SUCCESS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新成功</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#22c55e;}</style></head><body><div><h1>更新成功!</h1><p>设备正在重启，请在约1分钟后重新连接。</p></div></body></html>)HTML";
const char OTA_FAIL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新失败</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#ef4444;}</style></head><body><div><h1>更新失败!</h1><p>请检查上传的固件文件(.bin)是否正确，然后返回重试。</p></div></body></html>)HTML";


// (EEPROM 函数与之前相同)
void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint16_t magic; EEPROM.get(ADDR_MAGIC_NUM, magic);
  if (magic == EEPROM_MAGIC_NUMBER) {
    Serial.println("[OK] 从EEPROM加载数据...");
    EEPROM.get(ADDR_LOW_V, lowVoltageThreshold); EEPROM.get(ADDR_HIGH_V, highVoltageThreshold);
  } else {
    Serial.println("[!!] EEPROM未初始化或数据无效, 使用默认值并写入。");
    EEPROM.put(ADDR_MAGIC_NUM, EEPROM_MAGIC_NUMBER); EEPROM.put(ADDR_LOW_V, lowVoltageThreshold);
    EEPROM.put(ADDR_HIGH_V, highVoltageThreshold); EEPROM.commit();
  }
  EEPROM.end();
  Serial.printf("  -> 低压:%.2fV, 高压:%.2fV\n", lowVoltageThreshold, highVoltageThreshold);
}
void saveSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(ADDR_LOW_V, lowVoltageThreshold); EEPROM.put(ADDR_HIGH_V, highVoltageThreshold);
  EEPROM.commit(); EEPROM.end();
  Serial.println("[OK] 电压阈值已保存到EEPROM。");
}


// ============== 核心功能函数 ==============
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  Serial.printf("继电器 (Pin %d) 已设置为: %s\n", RELAY_PIN, relayState ? "ON" : "OFF");
}

void checkVoltageProtection() {
  if (!ina219_ok) return;
  busVoltage = ina219.getBusVoltage_V(); // 每次检查都更新读数

  if (isLockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_DURATION_MS) {
      Serial.println("[OK] 1小时锁定时间已到，解除锁定。"); isLockedOut = false;
    } else { return; }
  }

  if (!relayState && !isLockedOut && busVoltage >= highVoltageThreshold) {
    Serial.printf("检测到高电压 (%.2fV >= %.2fV)，自动开启继电器。\n", busVoltage, highVoltageThreshold);
    setRelay(true);
  }
  if (relayState && busVoltage > 0.1 && busVoltage < lowVoltageThreshold) {
    Serial.printf("!!! 触发低压保护 (%.2fV < %.2fV)，自动关闭继电器并锁定1小时。\n", busVoltage, lowVoltageThreshold);
    isLockedOut = true; lockoutStartTime = millis(); setRelay(false);
  }
}

// (Web路由函数已精简)
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
  json += "\"lockout_rem\":" + String(remaining_min);
  json += "}";
  server.send(200, "application/json", json);
}
void handleGetStatus() {
  String json = "{";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"low_v\":" + String(lowVoltageThreshold, 2) + ",";
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
  if (server.hasArg("low") && server.hasArg("high")) {
    lowVoltageThreshold = server.arg("low").toFloat();
    highVoltageThreshold = server.arg("high").toFloat();
    saveSettings();
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}


// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP8266 智能继电器 & INA219电压表 v5.1 =====");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  relayState = true;
  Serial.println("[OK] 继电器已设置为默认开启状态。");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!ina219.begin()) {
    Serial.println("[!!] 硬件错误: 未能找到INA219芯片! 请检查I2C接线。");
    ina219_ok = false;
  } else {
    Serial.println("[OK] INA219 通信成功，已配置为仅电压表模式。");
    ina219_ok = true;
  }

  loadSettings();

  // (WiFi, NTP, mDNS, Web Server 的启动代码与之前完全相同)
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
  Serial.println("[OK] NTP 时间服务已同步。");
  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", WEB_SERVER_PORT);
    Serial.printf("[OK] mDNS 已启动, 访问: http://%s.local\n", deviceName);
  }
  server.on("/", HTTP_GET, handleRoot);
  server.on("/getData", HTTP_GET, handleGetData);
  server.on("/getStatus", HTTP_GET, handleGetStatus);
  server.on("/setRelay", HTTP_GET, handleSetRelay);
  server.on("/setSettings", HTTP_GET, handleSetSettings);
  server.on("/update", HTTP_POST, []() { /* ...OTA代码... */
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", Update.hasError() ? OTA_FAIL_HTML : OTA_SUCCESS_HTML);
    if (!Update.hasError()) { delay(1000); ESP.restart(); }
  }, []() { /* ...OTA代码... */
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

