// =================================================================================================
// ==   ESP8266 智能继电器 & INA219 v6.15 (Chart UI Hotfix)                       ==
// =================================================================================================
// 描述: 此版本为最终UI修正版。修正了图表Y轴标签因添加单位'V'而可能导致显示不全的问题。
//       通过移除刻度标签的formatter，并依赖Y轴名称来表示单位，确保了数值的完整显示。
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
  const char* NTP_SERVERS           = "ntp.aliyun.com, pool.ntp.org, time.nist.gov";
  const int   GMT_OFFSET            = 8;
  const int   DAYLIGHT_OFFSET       = 0;
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
const int ADDR_MAGIC_NUM   = 0;
const int ADDR_LOW_V       = 2;
const int ADDR_HIGH_V      = 6;
const int ADDR_WARN_V      = 10;
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

float lastBusVoltage = 0;
const float WARNING_HYSTERESIS_V = 0.5;

const unsigned long NOTIFICATION_COOLDOWN_MS = 3600000;
unsigned long lastWarningNoticeTime = 0;
unsigned long lastLockoutNoticeTime = 0;

// --- 图表数据配置 ---
const char* VLOG_FILE_PATH = "/vlog.dat";
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
// ============== 网页 (HTML+CSS+JS) v6.15 ==============
// =====================================================
const char MAIN_HTML_PART1[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP8266 智能继电器</title><script src="https://cdn.jsdelivr.net/npm/echarts@5.5.0/dist/echarts.min.js"></script><style>:root{--bg-color:#111827;--card-color:#1f2937;--text-color:#d1d5db;--accent-color:#38bdf8;--green-color:#22c55e;--red-color:#ef4444;--warning-color:#f59e0b;--muted-text:#9ca3af}body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";background-color:var(--bg-color);color:var(--text-color);margin:0;padding:15px;display:flex;justify-content:center}h1,h2,h4{margin-top:0;color:#fff;text-align:center}h2{border-top:1px solid #374151;padding-top:15px;margin-top:20px}.container{width:100%;max-width:500px}.card{background-color:var(--card-color);border-radius:12px;padding:20px;margin-bottom:15px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1),0 2px 4px -1px rgba(0,0,0,.06)}.chart-card{padding:20px 0 10px 0;}.chart-card h2{padding:0 20px 15px;margin:0;border:none;}.data-box{text-align:center;padding:10px}.data-box .val{font-size:2.5em;font-weight:700;color:var(--accent-color);line-height:1.2;transition:color .3s ease}.data-box .unit{color:var(--muted-text)}.btn{width:100%;padding:15px;font-size:1.2em;font-weight:bold;border:none;border-radius:8px;cursor:pointer;transition:background-color .2s ease}.btn.on{background-color:var(--green-color);color:#fff}.btn.off{background-color:var(--red-color);color:#fff}.status-light{width:12px;height:12px;border-radius:50%;display:inline-block;margin-right:8px;background-color:#6b7280}.status-light.on{background-color:var(--green-color)}.input-group{display:flex;align-items:center;gap:10px;margin-bottom:10px}.input-group label{flex-basis:120px;flex-shrink:0}input[type=number]{width:100%;padding:8px;background-color:#374151;border:1px solid #4b5563;border-radius:6px;color:var(--text-color);font-size:1em}.btn-save{padding:10px 15px;background-color:var(--accent-color);color:#fff;border:none;border-radius:6px;cursor:pointer}#sysinfo{font-size:.8em;color:var(--muted-text);word-break:break-all}#lockoutStatus{color:var(--red-color);text-align:center;margin-bottom:10px;font-weight:bold;}</style></head><body><div class="container"><h1>ESP8266 智能继电器</h1><p style="text-align:center;color:var(--muted-text);">当前时间: <span id="currentTime">--:--:--</span></p><div class="card"><div class="data-box"><div>电池电压</div><div class="val" id="v">--</div><div class="unit">V</div></div></div><div class="card"><h2>手动控制</h2><div id="lockoutStatus" style="display:none;"></div><p><span id="relayStatusLight" class="status-light"></span>继电器状态: <strong id="relayStatusText">读取中...</strong></p><button id="relayBtn" class="btn">读取中...</button></div><div class="card"><h2>参数设置</h2><div class="input-group"><label for="highV">高压开启 (V)</label><input type="number" id="highV" step="0.1"></div><div class="input-group"><label for="warnV">电压警告 (V)</label><input type="number" id="warnV" step="0.1"></div><div class="input-group"><label for="lowV">低压关闭 (V)</label><input type="number" id="lowV" step="0.1"></div><div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveSettings()">保存设置</button></div></div><div class="card"><h2>系统信息与更新</h2><div id="sysinfo">加载中...</div><h4>固件更新 (OTA)</h4><div id="otaUi"><form id="otaForm" method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz" required><button type="submit" class="btn-save" style="margin-top:10px;">上传并更新</button></form></div><div id="otaStatus"></div></div><div class="card chart-card"><h2>24小时电压曲线</h2><div id="voltageChart" style="width: 100%; height: 250px;"></div></div></div>
)HTML";

const char MAIN_HTML_PART2[] PROGMEM = R"HTML(
<script>
var echartInstance;
var latestDeviceTimeStr = "--:--:--";
const DATA_INTERVAL_MIN = 1;

function $(s){return document.getElementById(s)}
function fetchJson(url,options){return fetch(url,options).then(r=>{if(!r.ok)throw new Error('Network error');return r.json()})}
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
})}
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
  } catch (error) {
    echartInstance.hideLoading();
    console.error("Failed to load chart data:", error);
    echartInstance.setOption({title: {text: '图表数据加载失败', left: 'center', top: 'center', textStyle: {color: 'var(--red-color)'}}});
  }
}
async function updateChart(){
  if(!echartInstance){return;}
  try {
    fetchInitialState();
    const chartData=await fetchJson('/getChartData');
    echartInstance.setOption({xAxis:{data:chartData.labels},series:[{data:chartData.data}]});
  } catch (error) {
    console.error("Failed to update chart data:", error);
  }
}
function saveSettings(){
  const lowV=$('lowV').value;
  const warnV=$('warnV').value;
  const highV=$('highV').value;
  fetch(`/setSettings?low=${lowV}&warn=${warnV}&high=${highV}`).then(r=>{if(r.ok){alert('设置已保存!')}else{alert('保存失败!')}}).catch(e=>alert('请求出错: '+e));
}
$('relayBtn').addEventListener('click',()=>{const newState=$('relayBtn').classList.contains('on');fetch('/setRelay?state='+(newState?'1':'0')).then(()=>setTimeout(fetchData,200))});
$('otaForm').addEventListener('submit', function(e){$('otaUi').style.display='none';$('otaStatus').innerHTML='<h4>正在上传并更新...</h4><p>请勿关闭此页面或断开设备电源。设备将在大约一分钟后自动重启。</p>';});

document.addEventListener('DOMContentLoaded', (event) => {
  fetchInitialState();
  fetchData();
  initChart();
});

setInterval(fetchData,2500);
setInterval(updateChart,60000);
</script></body></html>
)HTML";

const char OTA_SUCCESS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新成功</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#22c55e;}</style></head><body><div><h1>更新成功!</h1><p>设备正在重启，请在约1分钟后重新连接。</p></div></body></html>)HTML";
const char OTA_FAIL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新失败</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#ef4444;}</style></head><body><div><h1>更新失败!</h1><p>请检查上传的固件文件(.bin)是否正确，然后返回重试。</p></div></body></html>)HTML";


// ============== 通知发送函数 ==============
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


// ============== 核心功能函数 ==============
void recordVoltageHistory() {
  float voltage = 0.0;
  if (ina219_ok) {
    voltage = ina219.getBusVoltage_V();
    if (voltage < 0.1) voltage = 0.0; // 过滤掉无效的小读数
  }
  
  File vlogFile = LittleFS.open(VLOG_FILE_PATH, "r+");
  if (!vlogFile) {
    Serial.println("[错误] 无法打开电压日志文件进行写入!");
    return;
  }

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
  Serial.printf("继电器 (Pin %d) 已设置为: %s\n", RELAY_PIN, relayState ? "ON" : "OFF");
}

void checkVoltageProtection() {
  if (!ina219_ok) return;
  busVoltage = ina219.getBusVoltage_V();
  
  bool voltageIsDecreasing = (busVoltage < (lastBusVoltage - 0.02));

  if (relayState && busVoltage <= warningVoltageThreshold && voltageIsDecreasing) {
      isVoltageWarning = true;
      if (millis() - lastWarningNoticeTime > NOTIFICATION_COOLDOWN_MS || lastWarningNoticeTime == 0) {
          Serial.printf("!!! 电压警告 (%.2fV <= %.2fV 且呈下降趋势)，发送通知。\n", busVoltage, warningVoltageThreshold);
          #if defined(ENABLE_EMAIL_NOTIFICATION)
            String subject = "[电压警告] " + String(deviceName);
            String message = "设备当前电压为 " + String(busVoltage, 2) + "V，已低于警告阈值 " + String(warningVoltageThreshold, 2) + "V。";
            sendEmailNotification(subject, message);
          #elif defined(ENABLE_IFTTT_NOTIFICATION)
            sendIFTTTNotification(String(deviceName) + "%20Voltage%20Warning", String(busVoltage, 2));
          #endif
          lastWarningNoticeTime = millis();
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

  if (isLockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_DURATION_MS) {
      Serial.println("[OK] 1小时锁定时间已到，解除锁定。"); 
      isLockedOut = false;
    } else { 
      lastBusVoltage = busVoltage;
      return; 
    }
  }

  if (!relayState && !isLockedOut && busVoltage >= highVoltageThreshold) {
    Serial.printf("检测到高电压 (%.2fV >= %.2fV)，自动开启继电器。\n", busVoltage, highVoltageThreshold);
    setRelay(true);
  }
  
  if (relayState && busVoltage > 0.1 && busVoltage < lowVoltageThreshold) {
    Serial.printf("!!! 触发低压保护 (%.2fV < %.2fV)，自动关闭继电器并锁定1小时。\n", busVoltage, lowVoltageThreshold);
    if (millis() - lastLockoutNoticeTime > NOTIFICATION_COOLDOWN_MS || lastLockoutNoticeTime == 0) {
      #if defined(ENABLE_EMAIL_NOTIFICATION)
          String subject = "[严重] 低压保护已触发! " + String(deviceName);a
          String message = "设备当前电压为 " + String(busVoltage, 2) + "V，已触发低压保护 (" + String(lowVoltageThreshold, 2) + "V)。继电器已关闭并锁定1小时。";
          sendEmailNotification(subject, message);
      #elif defined(ENABLE_IFTTT_NOTIFICATION)
          sendIFTTTNotification(String(deviceName) + "%20Low%20Voltage%20Shutdown", String(busVoltage, 2));
      #endif
      lastLockoutNoticeTime = millis();
    }
    isLockedOut = true; 
    lockoutStartTime = millis(); 
    setRelay(false);
  }

  if (busVoltage > 0.1 && busVoltage < REBOOT_VOLTAGE_THRESHOLD) {
    if (lowVoltageRebootTimer == 0) {
      lowVoltageRebootTimer = millis();
      Serial.printf("[警告] 电压低于 %.2fV, 启动1小时重启倒计时。\n", REBOOT_VOLTAGE_THRESHOLD);
    }
    else if (millis() - lowVoltageRebootTimer > REBOOT_TIMER_DURATION_MS) {
      Serial.println("[严重] 电压持续低于10V超过1小时，设备将重启！");
      delay(100);
      ESP.restart();
    }
  } else {
    if (lowVoltageRebootTimer != 0) {
      Serial.printf("[OK] 电压已恢复至 %.2fV以上, 取消重启倒计时。\n", REBOOT_VOLTAGE_THRESHOLD);
      lowVoltageRebootTimer = 0;
    }
  }
  
  lastBusVoltage = busVoltage;
}


// ============== Web路由处理函数 ==============
void handleRoot() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=UTF-8", "");
  
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  
  server.sendContent_P(MAIN_HTML_PART1);
  server.sendContent_P(MAIN_HTML_PART2);
  
  server.sendContent("");
}

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
  
  FSInfo fs_info;
  LittleFS.info(fs_info);
  json += "\"free_heap\":" + String(ESP.getFreeHeap() / 1024) + ",";
  json += "\"total_heap\":" + String(81920 / 1024) + ",";
  json += "\"fs_free\":" + String((fs_info.totalBytes - fs_info.usedBytes) / 1024) + ",";
  json += "\"fs_total\":" + String(fs_info.totalBytes / 1024) + ",";
  json += "\"cpu_usage\":" + String(cpuUsage, 1);
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

void handleGetChartData() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");

  File vlogFile = LittleFS.open(VLOG_FILE_PATH, "r");
  if (!vlogFile) {
    server.sendContent("{\"error\":\"Failed to read log file\"}");
    server.sendContent("");
    return;
  }
  
  int savedIndex;
  vlogFile.read((byte*)&savedIndex, sizeof(int));
  
  server.sendContent("{\"labels\":[");
  for (int i = 0; i < DATA_POINTS; i++) {
    String label = "";
    if (i == DATA_POINTS - 1) {
      label = "\"Now\"";
    } else if ((DATA_POINTS - 1 - i) % 180 == 0) {
      label = "\"-" + String((DATA_POINTS - 1 - i) / 60) + "h\"";
    } else {
      label = "\"\"";
    }
    server.sendContent(label);
    if (i < DATA_POINTS - 1) {
      server.sendContent(",");
    }
  }
  server.sendContent("],\"data\":[");
  
  int startIndex = savedIndex;
  float voltage;
  
  for (int i = 0; i < DATA_POINTS; i++) {
    int currentIndex = (startIndex + i) % DATA_POINTS;
    vlogFile.seek(sizeof(int) + (currentIndex * sizeof(float)), SeekSet);
    vlogFile.read((byte*)&voltage, sizeof(float));

    if (isnan(voltage) || voltage < 0.1) { // 优化：小于0.1V也视为空
      server.sendContent("null");
    } else {
      server.sendContent(String(voltage, 2));
    }
    
    if (i < DATA_POINTS - 1) {
      server.sendContent(",");
    }
  }
  
  vlogFile.close();
  server.sendContent("]}");
  server.sendContent("");
}


// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP8266 智能继电器 & INA219 v6.15 (Chart UI Hotfix) =====");

  // 初始化文件系统
  if (!LittleFS.begin()) {
    Serial.println("[错误] 文件系统挂载失败!");
  } else {
    Serial.println("[OK] 文件系统已挂载。");
    File vlogFile = LittleFS.open(VLOG_FILE_PATH, "r+");
    if (!vlogFile) {
      Serial.println("[警告] 未找到电压日志文件，正在创建...");
      vlogFile = LittleFS.open(VLOG_FILE_PATH, "w+");
      if (vlogFile) {
        int initialIndex = 0;
        vlogFile.write((byte*)&initialIndex, sizeof(int));
        float nan_val = NAN;
        for (int i = 0; i < DATA_POINTS; i++) {
          vlogFile.write((byte*)&nan_val, sizeof(float));
        }
        vlogFile.close();
        Serial.println("[OK] 新的电压日志文件已创建并初始化。");
      } else {
        Serial.println("[错误] 创建电压日志文件失败!");
      }
    } else {
      vlogFile.read((byte*)&historyIndex, sizeof(int));
      vlogFile.close();
      Serial.printf("[OK] 已从文件加载历史数据指针: %d\n", historyIndex);
    }
  }

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  relayState = false;
  Serial.println("[OK] 继电器已设置为默认关闭状态。");

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
  server.on("/getChartData", HTTP_GET, handleGetChartData);
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
     if (millis() > 2000 && lastCpuMeasureTime == 0) {
        cpuIdleCounter = 0;
        lastCpuMeasureTime = millis();
     }
     if (millis() > 3000 && maxIdleCountsPerSecond == 0) {
        maxIdleCountsPerSecond = cpuIdleCounter;
        if (maxIdleCountsPerSecond < 1000) maxIdleCountsPerSecond = 2000000; // 安全值
        Serial.printf("[校准] CPU最大空闲计数值: %lu\n", maxIdleCountsPerSecond);
        cpuIdleCounter = 0;
        lastCpuMeasureTime = millis();
     }
  } else if (maxIdleCountsPerSecond > 0 && (millis() - lastCpuMeasureTime > CPU_MEASURE_INTERVAL_MS)) {
    float idleRatio = (float)cpuIdleCounter / maxIdleCountsPerSecond;
    if (idleRatio > 1.0) idleRatio = 1.0;
    cpuUsage = (1.0 - idleRatio) * 100.0;
    
    cpuIdleCounter = 0;
    lastCpuMeasureTime = millis();
  }
}