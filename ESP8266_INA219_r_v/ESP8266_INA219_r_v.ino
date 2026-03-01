// =================================================================================================
// ==   ESP8266 智能继电器 & INA219 v6.30 (Tooltip Time Restored， 87a换87，承受更大电流)                 ==
// =================================================================================================
// 描述: 
// 1. 继电器逻辑 (反转): 
//    - setRelay(true)  -> digitalWrite(LOW)  -> 开启
//    - setRelay(false) -> digitalWrite(HIGH) -> 关闭
// 2. 图表功能增强: 
//    - 恢复了 ECharts 悬停提示框的时间计算功能，现在可以显示具体的“日期 时间”和“电压”。
// 3. 内存显示: 
//    - 格式为 "剩余 / 总共 KB"。
// 4. 邮件配置: 
//    - 安全存储在 LittleFS，代码中无明文密码。
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
#include <LittleFS.h>

// ============== 用户 WiFi 配置 (请修改此处) ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp8266-smart-relay";
const int WEB_SERVER_PORT = 80;

// ============== 邮件配置 (默认留空，请去网页设置) ==============
String smtp_host     = "smtp.qq.com";
int    smtp_port     = 465;
String author_email  = "";
String author_pass   = "";
String recipient_email = "";

const char* NTP_SERVERS           = "ntp.aliyun.com, pool.ntp.org, time.nist.gov";
const int   GMT_OFFSET            = 8;
const int   DAYLIGHT_OFFSET       = 0;

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
const int ADDR_MAGIC_NUM   = 0;
const int ADDR_LOW_V       = 2;
const int ADDR_HIGH_V      = 6;
const int ADDR_WARN_V      = 10;
const uint16_t EEPROM_MAGIC_NUMBER = 0x5A1D;

// ============== 配置文件路径 ==============
const char* VLOG_FILE_PATH = "/vlog.dat";
const char* EMAIL_CFG_PATH = "/email_cfg.txt";

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

float lastBusVoltage = 0;
const float WARNING_HYSTERESIS_V = 0.5;

const unsigned long NOTIFICATION_COOLDOWN_MS = 3600000;
unsigned long lastWarningNoticeTime = 0;
unsigned long lastLockoutNoticeTime = 0;

// --- 图表数据配置 ---
const int DATA_POINTS = 1440;
const unsigned long DATA_INTERVAL_MS = 60000;
int historyIndex = 0;

// --- 低电压重启 ---
unsigned long lowVoltageRebootTimer = 0;
const float REBOOT_VOLTAGE_THRESHOLD = 10.0;
const unsigned long REBOOT_TIMER_DURATION_MS = 3600000;

// --- CPU使用率估算 ---
volatile unsigned long cpuIdleCounter = 0;
unsigned long lastCpuMeasureTime = 0;
float cpuUsage = 0.0;
const unsigned long CPU_MEASURE_INTERVAL_MS = 1000;
unsigned long maxIdleCountsPerSecond = 0;

void ICACHE_RAM_ATTR countCpuIdle() {
  cpuIdleCounter++;
}

// =====================================================
// ============== 网页 (HTML+CSS+JS) v6.20 ==============
// =====================================================
const char MAIN_HTML_PART1[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP8266 智能继电器</title><script src="https://cdn.jsdelivr.net/npm/echarts@5.5.0/dist/echarts.min.js"></script><style>:root{--bg-color:#111827;--card-color:#1f2937;--text-color:#d1d5db;--accent-color:#38bdf8;--green-color:#22c55e;--red-color:#ef4444;--warning-color:#f59e0b;--muted-text:#9ca3af}body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";background-color:var(--bg-color);color:var(--text-color);margin:0;padding:15px;display:flex;justify-content:center}h1,h2,h4{margin-top:0;color:#fff;text-align:center}h2{border-top:1px solid #374151;padding-top:15px;margin-top:20px}.container{width:100%;max-width:500px}.card{background-color:var(--card-color);border-radius:12px;padding:20px;margin-bottom:15px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1),0 2px 4px -1px rgba(0,0,0,.06)}.chart-card{padding:20px 0 10px 0;}.chart-card h2{padding:0 20px 15px;margin:0;border:none;}.data-box{text-align:center;padding:10px}.data-box .val{font-size:2.5em;font-weight:700;color:var(--accent-color);line-height:1.2;transition:color .3s ease}.data-box .unit{color:var(--muted-text)}.btn{width:100%;padding:15px;font-size:1.2em;font-weight:bold;border:none;border-radius:8px;cursor:pointer;transition:background-color .2s ease}.btn.on{background-color:var(--green-color);color:#fff}.btn.off{background-color:var(--red-color);color:#fff}.status-light{width:12px;height:12px;border-radius:50%;display:inline-block;margin-right:8px;background-color:#6b7280}.status-light.on{background-color:var(--green-color)}.input-group{display:flex;align-items:center;gap:10px;margin-bottom:10px}.input-group label{flex-basis:120px;flex-shrink:0;font-size:0.9em}input[type=number],input[type=text],input[type=password]{width:100%;padding:8px;background-color:#374151;border:1px solid #4b5563;border-radius:6px;color:var(--text-color);font-size:1em}.btn-save{padding:10px 15px;background-color:var(--accent-color);color:#fff;border:none;border-radius:6px;cursor:pointer}#sysinfo{font-size:.8em;color:var(--muted-text);word-break:break-all}#lockoutStatus{color:var(--red-color);text-align:center;margin-bottom:10px;font-weight:bold;}.toggle-section{cursor:pointer;color:var(--accent-color);text-align:center;font-size:0.9em;margin-top:10px;}</style></head><body><div class="container"><h1>ESP8266 智能继电器</h1><p style="text-align:center;color:var(--muted-text);">当前时间: <span id="currentTime">--:--:--</span></p><div class="card"><div class="data-box"><div>电池电压</div><div class="val" id="v">--</div><div class="unit">V</div></div></div><div class="card"><h2>手动控制</h2><div id="lockoutStatus" style="display:none;"></div><p><span id="relayStatusLight" class="status-light"></span>继电器状态: <strong id="relayStatusText">读取中...</strong></p><button id="relayBtn" class="btn">读取中...</button></div><div class="card"><h2>参数设置</h2><div class="input-group"><label for="highV">高压开启 (V)</label><input type="number" id="highV" step="0.1"></div><div class="input-group"><label for="warnV">电压警告 (V)</label><input type="number" id="warnV" step="0.1"></div><div class="input-group"><label for="lowV">低压关闭 (V)</label><input type="number" id="lowV" step="0.1"></div><p style="font-size:0.85em;color:var(--muted-text);text-align:right;">邮件通知将发送至: <span id="dispRecvEmail" style="color:var(--accent-color)">未设置</span></p><div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveSettings()">保存电压设置</button></div></div>
)HTML";

const char MAIN_HTML_PART2[] PROGMEM = R"HTML(
<div class="card"><h2>邮件通知设置</h2><div class="input-group"><label>SMTP服务器</label><input type="text" id="smtpHost" placeholder="如: smtp.qq.com"></div><div class="input-group"><label>SMTP端口</label><input type="number" id="smtpPort" placeholder="465"></div><div class="input-group"><label>发件邮箱</label><input type="text" id="authEmail" placeholder="xxxx@qq.com"></div><div class="input-group"><label>授权码/密码</label><input type="password" id="authPass"></div><div class="input-group"><label>收件邮箱</label><input type="text" id="recvEmail" placeholder="接收通知的邮箱"></div><div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveEmailConfig()">保存邮件配置</button></div></div><div class="card"><h2>系统信息与更新</h2><div id="sysinfo">加载中...</div><h4>固件更新 (OTA)</h4><div id="otaUi"><form id="otaForm" method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz" required><button type="submit" class="btn-save" style="margin-top:10px;">上传并更新</button></form></div><div id="otaStatus"></div></div><div class="card chart-card"><h2>24小时电压曲线</h2><div id="voltageChart" style="width: 100%; height: 250px;"></div></div></div>
<script>
var echartInstance;
var latestDeviceTimeStr = "--:--:--";
const DATA_INTERVAL_MIN = 1;
function $(s){return document.getElementById(s)}
function fetchJson(url){return fetch(url).then(r=>{if(!r.ok)throw new Error('Network error');return r.json()})}
function updateStatus(data){
  const relayOn=data.relay;
  $('relayStatusText').textContent=relayOn?'已开启':'已关闭';
  $('relayStatusText').style.color=relayOn?'var(--green-color)':'var(--red-color)';
  $('relayStatusLight').className=relayOn?'status-light on':'status-light';
  $('relayBtn').textContent=relayOn?'关闭继电器':'开启继电器';
  $('relayBtn').className=relayOn?'btn off':'btn on';
  if(data.lockout){$('lockoutStatus').style.display='block';$('lockoutStatus').textContent='电压警告中！剩余 '+data.lockout_rem+' 分钟可自动恢复。'}else{$('lockoutStatus').style.display='none';}
}
function fetchData(){fetchJson('/getData').then(data=>{
  $('v').textContent=data.voltage.toFixed(2);
  $('v').style.color=data.voltage_warning?'var(--warning-color)':'var(--accent-color)';
  $('currentTime').textContent=data.time;
  latestDeviceTimeStr = data.time;
  updateStatus(data);
})}
function fetchInitialState(){fetchJson('/getStatus').then(data=>{
  updateStatus(data);
  $('lowV').value=data.low_v;
  $('warnV').value=data.warn_v;
  $('highV').value=data.high_v;
  $('sysinfo').innerHTML=`IPv4: ${data.ip}<br>芯片ID: ${data.chip_id}<br>CPU繁忙度: ${data.cpu_usage}%<br>内存(RAM): ${data.free_heap} / ${data.total_heap} KB<br>存储(Flash): ${data.fs_free} / ${data.fs_total} KB`;
  $('smtpHost').value = data.mail_host;
  $('smtpPort').value = data.mail_port;
  $('authEmail').value = data.mail_user;
  $('authPass').value = data.mail_pass;
  $('recvEmail').value = data.mail_to;
  if(data.mail_to && data.mail_to.length > 3) $('dispRecvEmail').textContent = data.mail_to;
})}
function saveSettings(){
  const lowV=$('lowV').value; const warnV=$('warnV').value; const highV=$('highV').value;
  fetch(`/setSettings?low=${lowV}&warn=${warnV}&high=${highV}`).then(r=>{if(r.ok){alert('电压设置已保存!')}else{alert('保存失败!')}}).catch(e=>alert('请求出错: '+e));
}
function saveEmailConfig(){
  const host=$('smtpHost').value;
  const port=$('smtpPort').value;
  const user=$('authEmail').value;
  const pass=$('authPass').value;
  const to=$('recvEmail').value;
  const url = `/setEmail?host=${encodeURIComponent(host)}&port=${port}&user=${encodeURIComponent(user)}&pass=${encodeURIComponent(pass)}&to=${encodeURIComponent(to)}`;
  fetch(url).then(r=>{if(r.ok){alert('邮件配置已保存! 以后将发送通知至: '+to);$('dispRecvEmail').textContent = to;} else { alert('保存失败!'); }}).catch(e=>alert('请求出错: '+e));
}
$('relayBtn').addEventListener('click',()=>{const newState=$('relayBtn').classList.contains('on');fetch('/setRelay?state='+(newState?'1':'0')).then(()=>setTimeout(fetchData,200))});
$('otaForm').addEventListener('submit', function(e){$('otaUi').style.display='none';$('otaStatus').innerHTML='<h4>正在上传并更新...</h4><p>请勿关闭此页面或断开设备电源。设备将在大约一分钟后自动重启。</p>';});

// --- 重点: 恢复图表详细提示框功能 ---
async function initChart(){
  const chartDom = $('voltageChart');
  echartInstance = echarts.init(chartDom);
  echartInstance.showLoading({ text: '正在加载历史数据...' });
  try {
    const chartData = await fetchJson('/getChartData');
    const option={
      tooltip:{
        trigger:'axis',
        formatter: function(params){
          const point = params[0];
          if (point.value === null || point.value === '-') return null;
          const dataIndex = point.dataIndex;
          const voltage = parseFloat(point.value).toFixed(2);
          const minutesAgo = (chartData.data.length - 1 - dataIndex) * DATA_INTERVAL_MIN;
          
          let now = new Date();
          if (latestDeviceTimeStr !== "--:--:--") {
            const timeParts = latestDeviceTimeStr.split(':');
            now.setHours(timeParts[0], timeParts[1], timeParts[2]);
          }
          now.setMinutes(now.getMinutes() - minutesAgo);
          const historicalDate = `${now.getFullYear()}-${(now.getMonth()+1).toString().padStart(2,'0')}-${now.getDate().toString().padStart(2,'0')}`;
          const historicalTime = `${now.getHours().toString().padStart(2,'0')}:${now.getMinutes().toString().padStart(2,'0')}`;
          
          return `${historicalDate} ${historicalTime}<br/>电压: <strong>${voltage} V</strong>`;
        }
      },
      grid:{left:'8%',right:'4%',bottom:'10%',containLabel:false},
      xAxis:{type:'category',boundaryGap:false,data:chartData.labels,axisLine:{lineStyle:{color:'var(--muted-text)'}},axisTick:{show:false}},
      yAxis:{type:'value',name:'电压 (V)',nameTextStyle:{color:'var(--muted-text)',padding:[0,0,0,35]},min:'dataMin',max:'dataMax',axisLine:{show:true,lineStyle:{color:'var(--muted-text)'}},splitLine:{lineStyle:{color:'rgba(255,255,255,0.1)'}}},
      series:[{name:'电压',type:'line',smooth:true,connectNulls:false,data:chartData.data,symbol:'none',areaStyle:{color:new echarts.graphic.LinearGradient(0,0,0,1,[{offset:0,color:'rgba(56,189,248,0.5)'},{offset:1,color:'rgba(56,189,248,0.1)'}])},lineStyle:{color:'var(--accent-color)'}}]
    };
    echartInstance.hideLoading();
    echartInstance.setOption(option);
    window.addEventListener('resize',()=>echartInstance.resize());
  } catch (error) { echartInstance.hideLoading(); console.error(error); }
}
async function updateChart(){ if(!echartInstance)return; try { const chartData=await fetchJson('/getChartData'); echartInstance.setOption({xAxis:{data:chartData.labels},series:[{data:chartData.data}]}); } catch(e){} }

document.addEventListener('DOMContentLoaded', (event) => { fetchInitialState(); fetchData(); initChart(); });
setInterval(fetchData,2500); setInterval(updateChart,60000);
</script></body></html>
)HTML";

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
  config.server.host_name = smtp_host.c_str();
  config.server.port = smtp_port;
  config.login.email = author_email.c_str();
  config.login.password = author_pass.c_str();
  config.login.user_domain = F("127.0.0.1");
  config.time.ntp_server = NTP_SERVERS;
  config.time.gmt_offset = GMT_OFFSET;
  config.time.day_light_offset = DAYLIGHT_OFFSET;

  SMTP_Message email;
  email.sender.name = F("ESP8266 继电器");
  email.sender.email = author_email.c_str();
  email.subject = subject;
  email.addRecipient(F("User"), recipient_email.c_str());
  
  String htmlMsg = "<h2>" + subject + "</h2><p>" + message + "</p><p>设备名称: " + String(deviceName) + "</p><p>当前时间: " + timeClient.getFormattedTime() + "</p>";
  email.html.content = htmlMsg;
  email.html.transfer_encoding = Content_Transfer_Encoding::enc_base64;

  Serial.printf("准备发送邮件至 %s ...\n", recipient_email.c_str());
  if (!smtp.connect(&config)) {
    MailClient.printf("SMTP Error: %d, %s\n", smtp.statusCode(), smtp.errorReason().c_str());
    return;
  }
  if (!MailClient.sendMail(&smtp, &email)) {
    MailClient.printf("Send Error: %d, %s\n", smtp.statusCode(), smtp.errorReason().c_str());
  }
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
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ?  HIGH: LOW);
  Serial.printf("继电器 -> %s\n", relayState ? "ON" : "OFF");
}

void checkVoltageProtection() {
  if (!ina219_ok) return;
  busVoltage = ina219.getBusVoltage_V();
  bool voltageIsDecreasing = (busVoltage < (lastBusVoltage - 0.02));

  if (relayState && busVoltage <= warningVoltageThreshold && voltageIsDecreasing) {
      isVoltageWarning = true;
      if (millis() - lastWarningNoticeTime > NOTIFICATION_COOLDOWN_MS || lastWarningNoticeTime == 0) {
          Serial.printf("!!! 电压警告 (%.2fV)\n", busVoltage);
          sendEmailNotification("[电压警告] " + String(deviceName), 
            "电压为 " + String(busVoltage, 2) + "V，低于警告值 " + String(warningVoltageThreshold, 2) + "V。");
          lastWarningNoticeTime = millis();
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
      sendEmailNotification("[低压切断] " + String(deviceName), 
        "电压 " + String(busVoltage, 2) + "V 低于阈值 " + String(lowVoltageThreshold, 2) + "V，已切断。");
      lastLockoutNoticeTime = millis();
    }
    isLockedOut = true; 
    lockoutStartTime = millis(); 
    setRelay(false);
  }

  if (busVoltage > 0.1 && busVoltage < REBOOT_VOLTAGE_THRESHOLD) {
    if (lowVoltageRebootTimer == 0) lowVoltageRebootTimer = millis();
    else if (millis() - lowVoltageRebootTimer > REBOOT_TIMER_DURATION_MS) {
      delay(100); ESP.restart();
    }
  } else {
    lowVoltageRebootTimer = 0;
  }
  lastBusVoltage = busVoltage;
}

// ============== Web路由处理 ==============
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
  timeClient.update();
  long remaining_min = isLockedOut ? (LOCKOUT_DURATION_MS - (millis() - lockoutStartTime)) / 60000 : 0;
  
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
  
  FSInfo fs_info; LittleFS.info(fs_info);
  json += "\"free_heap\":" + String(ESP.getFreeHeap() / 1024) + ",";
  json += "\"total_heap\":" + String(81920 / 1024) + ",";
  json += "\"fs_free\":" + String((fs_info.totalBytes - fs_info.usedBytes) / 1024) + ",";
  json += "\"fs_total\":" + String(fs_info.totalBytes / 1024) + ",";
  json += "\"cpu_usage\":" + String(cpuUsage, 1) + ",";
  json += "\"mail_host\":\"" + smtp_host + "\",";
  json += "\"mail_port\":" + String(smtp_port) + ",";
  json += "\"mail_user\":\"" + author_email + "\",";
  json += "\"mail_pass\":\"" + author_pass + "\",";
  json += "\"mail_to\":\"" + recipient_email + "\"";
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
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  File vlogFile = LittleFS.open(VLOG_FILE_PATH, "r");
  if (!vlogFile) { server.sendContent("{}"); return; }
  
  int savedIndex; vlogFile.read((byte*)&savedIndex, sizeof(int));
  server.sendContent("{\"labels\":[");
  for (int i = 0; i < DATA_POINTS; i++) {
    server.sendContent(i == DATA_POINTS - 1 ? "\"Now\"" : ((DATA_POINTS-1-i)%180==0 ? ("\"-"+String((DATA_POINTS-1-i)/60)+"h\"") : "\"\""));
    if (i < DATA_POINTS - 1) server.sendContent(",");
  }
  server.sendContent("],\"data\":[");
  float voltage;
  for (int i = 0; i < DATA_POINTS; i++) {
    int curr = (savedIndex + i) % DATA_POINTS;
    vlogFile.seek(sizeof(int) + (curr * sizeof(float)), SeekSet);
    vlogFile.read((byte*)&voltage, sizeof(float));
    server.sendContent((isnan(voltage) || voltage < 0.1) ? "null" : String(voltage, 2));
    if (i < DATA_POINTS - 1) server.sendContent(",");
  }
  vlogFile.close();
  server.sendContent("]}");
  server.sendContent("");
}

// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP8266 Relay v6.20 (Tooltip Restored) =====");

  if (LittleFS.begin()) {
    File vlogFile = LittleFS.open(VLOG_FILE_PATH, "r+");
    if (!vlogFile) {
      vlogFile = LittleFS.open(VLOG_FILE_PATH, "w+");
      int initialIndex = 0; vlogFile.write((byte*)&initialIndex, sizeof(int));
      float nan_val = NAN;
      for (int i = 0; i < DATA_POINTS; i++) vlogFile.write((byte*)&nan_val, sizeof(float));
      vlogFile.close();
    } else {
      vlogFile.read((byte*)&historyIndex, sizeof(int));
      vlogFile.close();
    }
    loadSettings(); 
  }

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);   // 默认low关闭
  relayState = false;

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (ina219.begin()) { ina219_ok = true; Serial.println("[OK] INA219 Ready."); }

  WiFi.mode(WIFI_STA);
  WiFi.hostname(deviceName);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n[OK] WiFi Connected.");

  timeClient.begin();
  timeClient.forceUpdate();

  smtp.debug(1);
  smtp.callback(smtpCallback);

  if (MDNS.begin(deviceName)) MDNS.addService("http", "tcp", WEB_SERVER_PORT);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/getData", HTTP_GET, handleGetData);
  server.on("/getStatus", HTTP_GET, handleGetStatus);
  server.on("/setRelay", HTTP_GET, handleSetRelay);
  server.on("/setSettings", HTTP_GET, handleSetSettings);
  server.on("/setEmail", HTTP_GET, handleSetEmail);
  server.on("/getChartData", HTTP_GET, handleGetChartData);
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", Update.hasError() ? OTA_FAIL_HTML : OTA_SUCCESS_HTML);
    if (!Update.hasError()) { delay(1000); ESP.restart(); }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) { Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000); }
    else if (upload.status == UPLOAD_FILE_WRITE) { Update.write(upload.buf, upload.currentSize); }
    else if (upload.status == UPLOAD_FILE_END) { Update.end(true); }
  });
  
  server.begin();
}

void loop() {
  countCpuIdle();
  server.handleClient();
  MDNS.update();

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

  if (millis() < 5000) {
     if (millis() > 3000 && maxIdleCountsPerSecond == 0) {
        maxIdleCountsPerSecond = cpuIdleCounter;
        if (maxIdleCountsPerSecond < 1000) maxIdleCountsPerSecond = 2000000;
        cpuIdleCounter = 0; lastCpuMeasureTime = millis();
     }
  } else if (maxIdleCountsPerSecond > 0 && (millis() - lastCpuMeasureTime > CPU_MEASURE_INTERVAL_MS)) {
    float idleRatio = (float)cpuIdleCounter / maxIdleCountsPerSecond;
    cpuUsage = (1.0 - (idleRatio > 1.0 ? 1.0 : idleRatio)) * 100.0;
    cpuIdleCounter = 0; lastCpuMeasureTime = millis();
  }
}