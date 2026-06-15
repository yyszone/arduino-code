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
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <time.h>
#include <ESP_Mail_Client.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <Update.h>            // 【修改】引入 ESP32 固件更新支持
#include "MatrixClock.h" 
#include "logs.h" 

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
const int MATRIX_PIN  = 3;  // 点阵时钟 DIN (GPIO 3)

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
WebServer server(WEB_SERVER_PORT);      // 【修改】
SmartMatrixClock myClock(MATRIX_PIN, server); // 【修改】初始化时钟引脚指向 GPIO 3

Adafruit_INA219 ina219;
SMTPSession smtp;

bool relayState = false, ina219_ok = false;
bool wifiConnected = false; 
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

// --- CPU使用率估算 (优化版：杜绝空转，精准计算工作时间比例) ---
unsigned long lastCpuMeasureTime = 0;
float cpuUsage = 0.0;
unsigned long totalWorkTime = 0; // 累计实际工作耗时 (微秒)
const unsigned long CPU_MEASURE_INTERVAL_MS = 1000;
// ============== 辅助函数 ==============
uint32_t getChipId() {
  return (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF); // 【修改】ESP32 获取唯一 ID
}

// =====================================================
// ============== 网页 (HTML+CSS+JS) ==============
// =====================================================
const char MAIN_HTML_PART1[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32-C3 智能继电器</title><script src="https://cdn.jsdelivr.net/npm/echarts@5.5.0/dist/echarts.min.js"></script><style>:root{--bg-color:#111827;--card-color:#1f2937;--text-color:#d1d5db;--accent-color:#38bdf8;--green-color:#22c55e;--red-color:#ef4444;--warning-color:#f59e0b;--muted-text:#9ca3af}body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";background-color:var(--bg-color);color:var(--text-color);margin:0;padding:15px;display:flex;justify-content:center}h1,h2,h4{margin-top:0;color:#fff;text-align:center}h2{border-top:1px solid #374151;padding-top:15px;margin-top:20px}.container{width:100%;max-width:500px}.card{background-color:var(--card-color);border-radius:12px;padding:20px;margin-bottom:15px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1),0 2px 4px -1px rgba(0,0,0,.06)}.chart-card{padding:20px 0 10px 0;}.chart-card h2{padding:0 20px 15px;margin:0;border:none;}.data-box{text-align:center;padding:10px}.data-box .val{font-size:2.5em;font-weight:700;color:var(--accent-color);line-height:1.2;transition:color .3s ease}.data-box .unit{color:var(--muted-text)}.btn{width:100%;padding:15px;font-size:1.2em;font-weight:bold;border:none;border-radius:8px;cursor:pointer;transition:background-color .2s ease}.btn.on{background-color:var(--green-color);color:#fff}.btn.off{background-color:var(--red-color);color:#fff}.status-light{width:12px;height:12px;border-radius:50%;display:inline-block;margin-right:8px;background-color:#6b7280}.status-light.on{background-color:var(--green-color)}.input-group{display:flex;align-items:center;gap:10px;margin-bottom:10px}.input-group label{flex-basis:120px;flex-shrink:0;font-size:0.9em}input[type=number],input[type=text],input[type=password]{width:100%;padding:8px;background-color:#374151;border:1px solid #4b5563;border-radius:6px;color:var(--text-color);font-size:1em}.btn-save{padding:10px 15px;background-color:var(--accent-color);color:#fff;border:none;border-radius:6px;cursor:pointer}#sysinfo{font-size:.8em;color:var(--muted-text);word-break:break-all}#lockoutStatus{color:var(--red-color);text-align:center;margin-bottom:10px;font-weight:bold;}.toggle-section{cursor:pointer;color:var(--accent-color);text-align:center;font-size:0.9em;margin-top:10px;}</style></head><body>
<div class="container"><h1>ESP32-C3 智能继电器</h1><p style="text-align:center;color:var(--muted-text);font-size:0.8em;margin-bottom:2px;">来自脚本 ESP32_INA219_r_v.ino.ino</p><p style="text-align:center;color:var(--muted-text);margin-top:2px;">当前时间: <span id="currentTime">--:--:--</span></p><div class="card"><div class="data-box"><div>电池电压</div><div class="val" id="v">--</div><div class="unit">V</div></div></div><div class="card"><h2>手动控制</h2><div id="lockoutStatus" style="display:none;"></div><p><span id="relayStatusLight" class="status-light"></span>继电器状态: <strong id="relayStatusText">读取中...</strong></p><button id="relayBtn" class="btn">读取中...</button></div><div class="card"><h2>参数设置</h2><div class="input-group"><label for="highV">高压开启 (V)</label><input type="number" id="highV" step="0.1"></div><div class="input-group"><label for="warnV">电压警告 (V)</label><input type="number" id="warnV" step="0.1"></div><div class="input-group"><label for="lowV">低压关闭 (V)</label><input type="number" id="lowV" step="0.1"></div>
<p style="font-size:0.85em;color:var(--muted-text);text-align:right;">
  邮件通知将发送至: 
  <span id="dispRecvEmail" 
    style="color:var(--accent-color);cursor:pointer;text-decoration:underline dotted;" 
    title="点击发送测试邮件"
    onclick="sendTestEmail()">未设置</span>
</p>
<p id="testEmailStatus" style="font-size:0.8em;text-align:right;margin:0;min-height:1.2em;"></p>
<div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveSettings()">保存电压设置</button></div></div>
)HTML";

const char MAIN_HTML_PART2[] PROGMEM = R"HTML(
<div class="card"><h2>邮件通知设置</h2><div class="input-group"><label>SMTP服务器</label><input type="text" id="smtpHost" placeholder="如: smtp.qq.com"></div><div class="input-group"><label>SMTP端口</label><input type="number" id="smtpPort" placeholder="465"></div><div class="input-group"><label>发件邮箱</label><input type="text" id="authEmail" placeholder="xxxx@qq.com"></div><div class="input-group"><label>授权码/密码</label><input type="password" id="authPass"></div><div class="input-group"><label>收件邮箱</label><input type="text" id="recvEmail" placeholder="接收通知的邮箱"></div><div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveEmailConfig()">保存邮件配置</button></div></div><div class="card"><h2>系统信息与更新</h2>
<a href="/clock" style="display:block; text-align:center; background:linear-gradient(90deg, #ff007f, #7f00ff); color:#fff; padding:12px; border-radius:8px; text-decoration:none; margin-bottom:15px; font-weight:bold; box-shadow: 0 4px 10px rgba(255,0,127,0.3);">✨ 进入矩阵时钟控制台</a><div id="sysinfo">加载中...</div><h4>固件更新 (OTA)</h4><div id="otaUi"><form id="otaForm" method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz" required><button type="submit" class="btn-save" style="margin-top:10px;">上传并更新</button></form></div><div id="otaStatus"></div></div><div class="card chart-card"><h2>24小时电压曲线</h2><div id="voltageChart" style="width: 100%; height: 250px;"></div></div>
<a href="/logs" style="display:block;text-align:center;background:#1f2937;
color:#34d399;padding:10px;border-radius:8px;text-decoration:none;
margin-bottom:10px;font-size:13px;">📋 查看系统日志</a>
</div>
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
  $('sysinfo').innerHTML=`IPv4: ${data.ip}<br>IPv6: ${data.ipv6}<br>芯片ID: ${data.chip_id}<br>CPU繁忙度: ${data.cpu_usage}%<br>内存(RAM): ${data.free_heap} / ${data.total_heap} KB<br>存储(Flash): ${data.fs_free} / ${data.fs_total} KB`;
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

function sendTestEmail() {
  const addr = $('dispRecvEmail').textContent;
  if (!addr || addr === '未设置') {
    $('testEmailStatus').style.color = 'var(--warning-color)';
    $('testEmailStatus').textContent = '请先保存邮件配置';
    return;
  }
  $('testEmailStatus').style.color = 'var(--muted-text)';
  $('testEmailStatus').textContent = '发送中...';

  fetch('/testEmail').then(r => {
    if (!r.ok) return r.text().then(t => { throw new Error(t); });
    let attempts = 0;
    const poll = setInterval(() => {
      attempts++;
      fetch('/testEmailResult').then(r => r.text()).then(result => {
        if (result === 'pending') return; 
        clearInterval(poll);
        if (result === 'ok') {
          $('testEmailStatus').style.color = 'var(--green-color)';
          $('testEmailStatus').textContent = '✓ 发送成功';
        } else {
          $('testEmailStatus').style.color = 'var(--red-color)';
          $('testEmailStatus').textContent = '✗ ' + result.replace('fail:', '');
        }
      });
      if (attempts > 30) { 
        clearInterval(poll);
        $('testEmailStatus').style.color = 'var(--warning-color)';
        $('testEmailStatus').textContent = '⚠ 超时，请查收邮件确认';
      }
    }, 2000); 
  }).catch(e => {
    $('testEmailStatus').style.color = 'var(--red-color)';
    $('testEmailStatus').textContent = '✗ 请求出错: ' + e;
  });
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
$('relayBtn').addEventListener('click', () => {
  const newState = $('relayBtn').classList.contains('on');
  // 立即更新 UI（不等网络）
  updateStatus({ relay: !newState, lockout: false, lockout_rem: 0 });
  fetch('/setRelay?state=' + (newState ? '1' : '0'))
    .then(r => { if (!r.ok) fetchData(); }) // 失败才回滚
    .catch(() => fetchData());
});
$('otaForm').addEventListener('submit', function(e){$('otaUi').style.display='none';$('otaStatus').innerHTML='<h4>正在上传并更新...</h4><p>请勿关闭此页面或断开设备电源。设备将在大约一分钟后自动重启。</p>';});

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
setInterval(fetchData,5000); setInterval(updateChart,120000);
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
  email.sender.name = F("ESP32-C3 继电器");
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
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  sysLogf(LOG_INFO, "继电器 -> %s  电压=%.2fV  堆=%uB",   
    state ? "ON" : "OFF", busVoltage, ESP.getFreeHeap());
  File f = LittleFS.open("/relay.txt", "w");
  if (f) { f.print(state ? "1" : "0"); f.close(); }
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

  // 1. 【修改】文件系统，若首次运行未格式化则自动格式化挂载
  if (LittleFS.begin(true)) { 
    logBootReason();
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
  
  // 2. 引脚配置
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // 默认安全关闭

  // 3. I2C 初始化（使用 ESP32-C3 安全引脚）
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (ina219.begin()) { 
    ina219_ok = true; 
    Serial.println("[OK] INA219 Ready."); 
  }

  // 4. 读当前电压
  if (ina219_ok) busVoltage = ina219.getBusVoltage_V();

  // 5. 读上次继电器状态
  bool savedRelay = false;
  if (LittleFS.exists("/relay.txt")) {
    File f = LittleFS.open("/relay.txt", "r");
    if (f) { savedRelay = (f.read() == '1'); f.close(); }
  }

  // 6. WiFi连接
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName); // 【修改】ESP32 设置主机名
  WiFi.enableIPv6();            // 启用 IPv6
  wifiConnected = false;
  const int    WIFI_MAX_RETRIES    = 3;
  const unsigned long WIFI_TIMEOUT_MS = 10000UL; 
  for (int attempt = 1; attempt <= WIFI_MAX_RETRIES; attempt++) {
    Serial.printf("\n[WiFi] 第 %d/%d 次尝试连接...", attempt, WIFI_MAX_RETRIES);
    WiFi.begin(ssid, password);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      break;
    }
    Serial.printf(" 超时，断开重试");
    WiFi.disconnect(true);
    delay(1000);
  }

  if (!wifiConnected) {
    sysLog(LOG_WARN, "WiFi连接失败(3次)，进入停电保护：强制开启继电器");
    Serial.println("\n[Boot] WiFi失败 → 停电保护，强制 setRelay(ON)");
    setRelay(true);
    timeClient.begin(); 
    goto SETUP_NETWORK_DONE;
  }

  WiFi.setSleep(false); // 【修改】ESP32 关闭 WiFi 自动睡眠防掉线
  ipv6Mgr.onWiFiConnected();   // 等待 RA 下发，缓存 IPv6 地址
  sysLogf(LOG_BOOT, "WiFi已连接 IP=%s IPv6=%s", WiFi.localIP().toString().c_str(), ipv6Mgr.cached().c_str());

  // 7. NTP同步
  timeClient.begin();
  timeClient.forceUpdate();
  configTime(8 * 3600, 0, "ntp.aliyun.com"); 

  Serial.print("[Boot] 等待NTP同步");
  {
    unsigned long ntpWait = millis();
    while (time(nullptr) < 1000000UL && millis() - ntpWait < 10000) {
      delay(200); Serial.print(".");
    }
    Serial.printf(" time=%lu\n", (unsigned long)time(nullptr));
  }

  // 8. 决策与状态恢复
  {
    unsigned long offlineSec = 0;
    bool offlineTimeKnown = false;
    if (LittleFS.exists("/heartbeat.txt")) {
      File f = LittleFS.open("/heartbeat.txt", "r");
      if (f) {
        unsigned long savedTs = f.parseInt();
        f.close();
        time_t nowTs = time(nullptr);
        if (nowTs > savedTs && savedTs > 1000000UL) {
          offlineSec = (unsigned long)(nowTs - savedTs);
          offlineTimeKnown = true;
        }
      }
    }

    bool bootRelayOn;
    if (!offlineTimeKnown || offlineSec <= 10) {
      bootRelayOn = savedRelay;
      Serial.printf("[Boot] 崩溃重启(离线%lus) → 恢复上次: %s\n", offlineSec, savedRelay?"ON":"OFF");
    } else if (busVoltage > warningVoltageThreshold) {
      bootRelayOn = true;
      Serial.printf("[Boot] 正常重启(离线%lus), %.2fV > %.2fV → 强制开机\n", offlineSec, busVoltage, warningVoltageThreshold);
    } else {
      bootRelayOn = false;
      Serial.printf("[Boot] 正常重启(离线%lus), %.2fV ≤ %.2fV → 强制关机\n", offlineSec, busVoltage, warningVoltageThreshold);
    }
    setRelay(bootRelayOn);
  }

SETUP_NETWORK_DONE:

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
  server.begin();
  myClock.begin(); 
}

void loop() {
  unsigned long loopStart = micros(); // 记录本轮循环开始时间 (微秒)

  server.handleClient();

  if (pendingTestEmail) {
    pendingTestEmail = false;
    myClock.loop();           // 先让屏幕刷新一次再去发邮件
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
  myClock.setBatteryVoltage(busVoltage);  
  myClock.loop(); 

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
      Serial.println("[WiFi] 未连接，尝试重连...");
      WiFi.disconnect(true);
      delay(200);
      WiFi.begin(ssid, password);
    } else if (!ntpSynced) {
      ntpSynced = true;
      WiFi.setSleep(false); 
      sysLogf(LOG_INFO, "WiFi已连接(重连) IP=%s", WiFi.localIP().toString().c_str());
      timeClient.begin();
      timeClient.forceUpdate();
      configTime(8 * 3600, 0, "ntp.aliyun.com");
      Serial.println("[WiFi] 重连成功，NTP补同步完成");
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
    time_t nowTs = time(nullptr);
    if (nowTs > 1000000UL) {
      File f = LittleFS.open("/heartbeat.txt", "w");
      if (f) { f.print((unsigned long)nowTs); f.close(); }
    }
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

  // 【最核心修复】主动让出 CPU 1 毫秒，给底层 WiFi 栈和 TCP 握手留出喘息时间。
  // 彻底解决单核高速空转导致的网络响应极度迟缓、芯片发热、以及 CPU 虚高的问题。
  delay(5); 
}