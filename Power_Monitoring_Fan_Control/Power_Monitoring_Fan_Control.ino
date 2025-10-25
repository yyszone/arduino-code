// =================================================================================================
// ==     ESP32C3 智能风扇 & 双路定时插座控制器 v4.7 (通电即开版)     ==
// =================================================================================================
// 描述: 此版本在 v4.6 的基础上，根据用户反馈重构了启动逻辑。
// 新功能:
// 1. 移除了 "首次启动" 逻辑。
// 2. 现在，设备每次断电后重新通电，两个插座继电器都会默认开启。
// 3. 在每次启动时，会自动创建一个30分钟后关闭两个插座的临时定时任务（此任务不保存到闪存）。
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

// ============== 用户配置 ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp32-smart-fan";
const int WEB_SERVER_PORT = 15715;

// ============== 硬件引脚配置 ==============
// --- 风扇部分 ---
const int RELAY_PIN   = 1;  // 风扇主电源继电器
const int PWM_PIN     = 5;
const int TACH_PIN    = 4;
const int I2C_SDA_PIN = 10;
const int I2C_SCL_PIN = 8;
// --- 双路插座部分 ---
const int RELAY2_PIN  = 6;  // 插座1继电器
const int RELAY3_PIN  = 7;  // 插座2继电器

// ============== PWM/LEDC 配置 (风扇) ==============
const int LEDC_FREQUENCY = 25000;
const int LEDC_RES_BITS  = 8;
const int PWM_MAX        = (1 << LEDC_RES_BITS) - 1;
const bool PWM_INVERTED  = false;

// ============== RPM 采样配置 (风扇) ==============
const int PULSES_PER_REV = 2;
const uint32_t MIN_PULSE_INTERVAL_US = 800;
const int MAX_REASONABLE_RPM = 15000;

// ============== 继电器与智能电源管理配置 (风扇) ==============
const float VOLTAGE_THRESHOLD = 3.5;
const float VOLTAGE_HIGH_THRESHOLD = 4.2;
const long LOCKOUT_DURATION_MS = 3600000;

// ============== 时间与定时任务配置 ==============
const char* NTP_SERVER = "ntp.aliyun.com";
const long  GMT_OFFSET_SEC = 8 * 3600;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, GMT_OFFSET_SEC);
// --- 风扇定时任务 ---
struct TimerTask { int hour; int minute; bool action; bool enabled; };
TimerTask tasks[2] = { {0, 0, false, false}, {0, 0, false, false} };
// --- 插座定时任务 ---
const int MAX_SOCKET_SCHEDULES = 10; // 每个插座最多10个定时任务
struct SocketSchedule { int hour; int minute; int second; bool action; bool enabled; };
SocketSchedule relay2_schedules[MAX_SOCKET_SCHEDULES];
SocketSchedule relay3_schedules[MAX_SOCKET_SCHEDULES];
int relay2_schedule_count = 0;
int relay3_schedule_count = 0;


// ============== 电量统计配置 (风扇) ==============
const int HISTORY_DAYS = 7; 

// ============== 全局对象与变量 ==============
WebServer server(WEB_SERVER_PORT);
Adafruit_INA219 ina219;
Preferences preferences;
// --- 风扇相关变量 ---
volatile uint32_t pulseCount = 0;
volatile uint32_t lastPulseMicros = 0;
uint32_t lastRpmCalcMs = 0;
int fanSliderValue = 0;
int lastRpm = 0;
float loadVoltage = 0, current_mA = 0, power_mW = 0;
float lockoutTriggerVoltage = 0.0;
long lastRunDurationMinutes = 0;
unsigned long lastRunStartTime = 0;
char lockoutStopTime[6] = "--:--";
bool ina219_ok = false;
bool relayState = false; // 风扇继电器状态
bool isLockedOut = false;
unsigned long lockoutStartTime = 0;
// --- 风扇电量统计相关变量 ---
float dailyEnergyWh[HISTORY_DAYS] = {0};
float todayEnergyWh = 0;
unsigned long lastEnergyCalcMs = 0;
int lastDayChecked = -1;
// --- 插座相关变量 ---
bool relay2State = false; // 插座1继电器状态
bool relay3State = false; // 插座2继电器状态


// ===============================================
// ============== 网页 (HTML+CSS+JS) ==============
// ===============================================

// ---------- 主页面：风扇控制器 ----------
const char MAIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 智能风扇控制</title><style>:root{--bg:#0f172a;--card:#111827;--text:#e5e7eb;--accent:#22c55e;--muted:#94a3b8;--red:#ef4444;--blue:#3b82f6;}*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,'Helvetica Neue',Arial}.wrapper{min-height:100vh;background:linear-gradient(135deg,#0f172a,#1f2937);color:var(--text);display:flex;align-items:center;justify-content:center;padding:18px}.container{width:100%;max-width:600px}.card{background:linear-gradient(180deg,#0b1220,#0b1220) padding-box,linear-gradient(135deg,#22c55e33,#06b6d433) border-box;border:1px solid transparent;border-radius:16px;padding:20px;margin:14px 0;box-shadow:0 10px 30px rgba(0,0,0,.35)}h1,h2{margin:0 0 12px}h1{text-align:center;font-weight:700;font-size:22px}h2{font-size:18px;color:#d1d5db}.label{margin:8px 0 6px;font-weight:600}.value{font-feature-settings:'tnum' 1;letter-spacing:.3px}.slider{width:100%}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px;text-align:center}.data-box{padding:10px;border-radius:8px;background-color:rgba(0,0,0,.2)}.data-box .val{font-size:1.8em;color:var(--accent)}.data-box .unit{color:var(--muted);font-size:0.9em}small{color:var(--muted)}.btn{background:var(--accent);border:none;color:#052e13;padding:12px 18px;border-radius:10px;font-weight:700;cursor:pointer;box-shadow:0 6px 16px rgba(34,197,94,.35)}.btn.off{background:var(--red);color:#fff}.btn:hover{filter:brightness(1.05)}input[type=file]{color:var(--text)}pre{white-space:pre-wrap;word-break:break-word}.timer-row{display:flex;align-items:center;gap:10px;margin:10px 0}input[type=time],input[type=checkbox]{margin-right:5px}.chart-container{padding-top:10px}.chart{display:flex;justify-content:space-around;align-items:flex-end;height:120px;border-bottom:1px solid var(--muted)}.chart-bar{width:11%;background:linear-gradient(to top,var(--accent),#6ee7b7);border-radius:4px 4px 0 0;position:relative;transition:height .3s ease-in-out}.chart-bar .value{position:absolute;top:-20px;left:50%;transform:translateX(-50%);font-size:.8em;color:var(--text)}.chart-labels{display:flex;justify-content:space-around;font-size:.8em;color:var(--muted);margin-top:5px}.chart-labels div{width:11%;text-align:center}a.nav-link{display:inline-block;margin-top:15px;color:var(--accent);text-decoration:none;font-weight:bold;}</style></head><body><div class="wrapper"><div class="container"><div class="card"><h1>ESP32 智能风扇控制面板</h1><div class="label">当前时间: <span id="currentTime">--:--:--</span></div><div id="lockoutStatus" style="color:var(--red);margin-bottom:10px;display:none"></div><div class="grid"><div class="data-box"><div>转速 (RPM)</div><div class="val" id="rpm">--</div><div class="unit">&nbsp;</div></div><div class="data-box"><div>电压 (V)</div><div class="val" id="v">--</div><div class="unit">&nbsp;</div></div><div class="data-box"><div>电流 (mA)</div><div class="val" id="c">--</div><div class="unit">&nbsp;</div></div><div class="data-box"><div>功率 (mW)</div><div class="val" id="p">--</div><div class="unit">&nbsp;</div></div></div></div><div class="card"><h2>手动控制</h2><div class="label">风扇调速: <span id="spd" class="value">--</span></div><input id="speed" class="slider" type="range" min="0" max="255" value="0"/>
<small id="auto_relay_info" style="display:block; text-align:center; margin-top:10px;">读取设定值...</small>
<div style="margin-top:20px;display:flex;justify-content:center;gap:15px"><button id="relayBtn" class="btn">读取状态...</button></div></div><div class="card"><h2>定时任务 (风扇)</h2><div class="timer-row"><input type="checkbox" id="t1_en"><input type="time" id="t1_time"><select id="t1_act"><option value="1">开启</option><option value="0">关闭</option></select></div><div class="timer-row"><input type="checkbox" id="t2_en"><input type="time" id="t2_time"><select id="t2_act"><option value="1">开启</option><option value="0">关闭</option></select></div><button id="saveTimerBtn" class="btn" style="background:var(--blue);margin-top:10px">保存定时设置</button></div><div class="card"><h2>系统信息与导航</h2><div id="sys">加载中...</div><a href="/socket" class="nav-link">>> 前往双路插座控制页面</a></div><div class="card"><h2>固件在线更新 OTA</h2><form method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz"/><br/><br/><button class="btn" type="submit">上传并更新</button></form></div>
<div class="card"><h2>七天电量统计 (Wh)</h2><div class="chart-container"><div class="chart" id="powerChart"></div><div class="chart-labels" id="powerChartLabels"></div></div></div></div></div>
<script>
const spd=document.getElementById('spd'),rpm=document.getElementById('rpm'),slider=document.getElementById('speed'),sys=document.getElementById('sys'),v_el=document.getElementById('v'),c_el=document.getElementById('c'),p_el=document.getElementById('p'),relayBtn=document.getElementById('relayBtn'),lockoutEl=document.getElementById('lockoutStatus'),timeEl=document.getElementById('currentTime'),autoInfoEl=document.getElementById('auto_relay_info'),powerChartEl=document.getElementById('powerChart'),powerChartLabelsEl=document.getElementById('powerChartLabels');
function setLabel(v){const p=Math.round(v/255*100);spd.textContent=v+' ('+p+'%)'}
function fetchJson(url,options){return fetch(url,options).then(r=>{if(!r.ok)throw new Error('Network error');return r.json()})}
function updatePowerChart(){fetchJson('/getPowerStats').then(data=>{const values=[data.today,...data.history.slice(0,6)];const maxVal=Math.max(...values,0.1);let chartHTML='';for(const val of values){const height=(val/maxVal)*100;chartHTML+=`<div class="chart-bar" style="height:${height}%"><div class="value">${val.toFixed(2)}</div></div>`}
powerChartEl.innerHTML=chartHTML;const labels=["今天","昨天","前天","3天前","4天前","5天前","6天前"];let labelsHTML='';for(const label of labels){labelsHTML+=`<div>${label}</div>`}
powerChartLabelsEl.innerHTML=labelsHTML;}).catch(e=>console.error('Power chart update failed:',e))}
slider.addEventListener('input',()=>{const v=slider.value;setLabel(v);fetch('/setSpeed?value='+v).catch(e=>console.error(e))});
relayBtn.addEventListener('click',()=>{const newState=!relayBtn.classList.contains('off');fetch('/setRelay?state='+(newState?'1':'0')).then(()=>{updateRelayBtn(newState)})});
function updateRelayBtn(state){if(state){relayBtn.textContent='关闭继电器';relayBtn.classList.add('off')}else{relayBtn.textContent='开启继电器';relayBtn.classList.remove('off')}}
function saveTimers(){const data=new FormData();data.append('t1_en',document.getElementById('t1_en').checked?'1':'0');data.append('t1_time',document.getElementById('t1_time').value);data.append('t1_act',document.getElementById('t1_act').value);data.append('t2_en',document.getElementById('t2_en').checked?'1':'0');data.append('t2_time',document.getElementById('t2_time').value);data.append('t2_act',document.getElementById('t2_act').value);fetch('/setTimers',{method:'POST',body:data}).then(r=>alert(r.ok?'定时任务已保存':'保存失败')).catch(e=>alert('保存出错'));}
document.getElementById('saveTimerBtn').addEventListener('click',saveTimers);
window.addEventListener('load',()=>{fetchJson('/getStatus').then(data=>{slider.value=data.speed;setLabel(data.speed);updateRelayBtn(data.relay);document.getElementById('t1_en').checked=data.tasks[0].enabled;document.getElementById('t1_time').value=String(data.tasks[0].hour).padStart(2,'0')+':'+String(data.tasks[0].minute).padStart(2,'0');document.getElementById('t1_act').value=data.tasks[0].action?'1':'0';document.getElementById('t2_en').checked=data.tasks[1].enabled;document.getElementById('t2_time').value=String(data.tasks[1].hour).padStart(2,'0')+':'+String(data.tasks[1].minute).padStart(2,'0');document.getElementById('t2_act').value=data.tasks[1].action?'1':'0';if(data.high_voltage_threshold>0){autoInfoEl.textContent=`自动模式: 低于 ${data.low_voltage_threshold.toFixed(1)}V 关闭，高于 ${data.high_voltage_threshold.toFixed(1)}V 开启 (保护后锁定1小时)。`}
else{autoInfoEl.textContent=`自动启动已禁用。仅启用低于 ${data.low_voltage_threshold.toFixed(1)}V 的低压保护。`}}).catch(e=>console.error(e));updatePowerChart();});
setInterval(()=>{fetchJson('/getData').then(data=>{rpm.textContent=data.rpm;v_el.textContent=data.voltage.toFixed(2);c_el.textContent=data.current.toFixed(1);p_el.textContent=data.power.toFixed(0);timeEl.textContent=data.time;if(data.lockout){lockoutEl.style.display='block';lockoutEl.textContent='低压保护 ('+data.lockout_trigger_v.toFixed(2)+'V)! 上次运行 '+data.last_run_duration+' 分钟 (停止于 '+data.lockout_stop_time+')。继电器已锁定, 剩余: '+data.lockout_rem+' 分钟';}else{lockoutEl.style.display='none';}
updateRelayBtn(data.relay);}).catch(e=>console.error(e));fetchJson('/sysinfo').then(info=>{sys.innerHTML=`芯片: ${info.chip_model} (rev ${info.chip_rev})<br>CPU: ${info.cpu_freq_mhz} MHz<br>空闲内存: ${info.free_heap} B<br>IPv4: ${info.ip}<br>IPv6: ${info.ipv6}`}).catch(e=>console.error(e));},1500);
setInterval(updatePowerChart,60000);
</script></body></html>
)HTML";

// ---------- 插座控制器页面 (含用法说明) ----------
const char SOCKET_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 智能双路插座</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background-color: #f4f7f6; margin: 0; padding: 20px; color: #333; }
    .container { max-width: 600px; margin: 0 auto; background: #fff; padding: 25px; border-radius: 10px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); }
    h1, h2 { text-align: center; color: #007bff; }
    h2 { border-top: 1px solid #eee; padding-top: 20px; margin-top: 20px; }
    h3 { display: flex; justify-content: space-between; align-items: center; }
    h4 { color: #0056b3; }
    .status-box, .control-box, .schedule-box { margin-bottom: 25px; }
    p { font-size: 1.1em; text-align: center; }
    #timeDisplay { font-weight: bold; color: #333; }
    .status-on { color: #28a745; font-weight: bold; }
    .status-off { color: #dc3545; font-weight: bold; }
    .btn-group { display: flex; justify-content: space-around; gap: 15px; }
    button, .button { display: inline-block; padding: 12px 20px; font-size: 1em; cursor: pointer; border: none; border-radius: 5px; color: #fff; text-align: center; text-decoration: none; }
    .btn-on { background-color: #28a745; }
    .btn-on:hover { background-color: #218838; }
    .btn-off { background-color: #dc3545; }
    .btn-off:hover { background-color: #c82333; }
    .btn-add { background-color: #007bff; }
    .btn-add:hover { background-color: #0069d9; }
    .btn-delete-all { background-color: #dc3545; font-size: 0.7em; padding: 3px 8px; vertical-align: middle; }
    .btn-delete-all:hover { background-color: #c82333; }
    .input-group { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 10px; align-items: center; }
    input[type="time"] { flex-grow: 1; padding: 10px; border: 1px solid #ccc; border-radius: 5px; font-size: 1em; }
    select { padding: 10px; border: 1px solid #ccc; border-radius: 5px; font-size: 1em;}
    ul { list-style: none; padding: 0; }
    li { background: #f9f9f9; border: 1px solid #eee; padding: 10px; margin-bottom: 8px; border-radius: 5px; display: flex; justify-content: space-between; align-items: center; }
    li button { background-color: #6c757d; font-size: 0.8em; padding: 5px 10px; }
    li button:hover { background-color: #5a6268; }
    code { background-color: #e9ecef; padding: 2px 6px; border-radius: 4px; font-family: SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace; }
    .api-docs p { text-align: left; font-size: 0.95em; }
    a.nav-link{display:block;margin-top:20px;color:#007bff;text-decoration:none;font-weight:bold;text-align:center;}
  </style>
</head>
<body>
  <div class="container">
    <h1>智能双路插座控制器</h1>
    <div class="status-box">
      <p>NTP网络时间: <span id="timeDisplay">--:--:--</span></p>
    </div>

    <div class="control-box">
      <h2>插座 1 (GPIO ##R2_PIN##)</h2>
      <p>状态: <span id="r2_status">--</span></p>
      <div class="btn-group">
        <button class="button btn-on" onclick="manualControl(1, 1)">手动开启</button>
        <button class="button btn-off" onclick="manualControl(1, 0)">手动关闭</button>
      </div>
    </div>
    <div class="control-box">
      <h2>插座 2 (GPIO ##R3_PIN##)</h2>
      <p>状态: <span id="r3_status">--</span></p>
      <div class="btn-group">
        <button class="button btn-on" onclick="manualControl(2, 1)">手动开启</button>
        <button class="button btn-off" onclick="manualControl(2, 0)">手动关闭</button>
      </div>
    </div>

    <div class="schedule-box">
      <h2>添加定时任务</h2>
      <div class="input-group">
        <select id="relaySelect">
          <option value="1">插座 1</option>
          <option value="2">插座 2</option>
        </select>
        <input type="time" id="scheduleTime" step="1" value="12:00:00">
        <button class="button btn-add" onclick="addSchedule(1)">添加开机</button>
        <button class="button btn-add" onclick="addSchedule(0)">添加关机</button>
      </div>
    </div>

    <div class="schedule-box">
      <h2>任务列表</h2>
      <h3>插座 1 任务 <button class="btn-delete-all" onclick="deleteAll(1)">删除全部</button></h3>
      <ul id="r2-list"><li>加载中...</li></ul>
      <h3>插座 2 任务 <button class="btn-delete-all" onclick="deleteAll(2)">删除全部</button></h3>
      <ul id="r3-list"><li>加载中...</li></ul>
    </div>

    <div class="schedule-box api-docs">
        <h2>API & 用法说明</h2>
        <h4>通过 URL 添加定时任务 (API)</h4>
        <p>您可以通过访问特定URL来远程添加定时任务，无需打开网页。这对于自动化脚本非常有用。</p>
        <p><b>URL 端点:</b> <code>/LED-Control</code></p>
        <p><b>必需参数:</b></p>
        <ul>
            <li><code><b>relay</b>=[1|2]</code>: 目标插座 (1 或 2)。</li>
            <li><code><b>ledPwm</b>=[0|1]</code>: 执行的动作 (0=关闭, 1=开启)。</li>
            <li><code><b>time</b>=HH:MM:SS</code>: 任务执行时间 (秒是可选的)。</li>
        </ul>
        <p><b>示例:</b> 添加一个任务，在晚上10点半开启插座1。</p>
        <p><code>http://[设备IP]/LED-Control?relay=1&amp;ledPwm=1&amp;time=22:30:00</code></p>

        <h4 style="margin-top: 20px;">通过 URL 手动控制 (API)</h4>
        <p>立即打开或关闭一个插座。</p>
        <p><b>URL 端点:</b> <code>/setSocketTask</code></p>
        <p><b>必需参数:</b></p>
        <ul>
            <li><code><b>cmd</b>=manual</code>: 指定为手动模式。</li>
            <li><code><b>relay</b>=[1|2]</code>: 目标插座 (1 或 2)。</li>
            <li><code><b>state</b>=[0|1]</code>: 继电器状态 (0=关闭, 1=开启)。</li>
        </ul>
        <p><b>示例:</b> 立即关闭插座2。</p>
        <p><code>http://[设备IP]/setSocketTask?cmd=manual&amp;relay=2&amp;state=0</code></p>

        <h4 style="margin-top: 20px;">通过 URL 删除所有任务 (API)</h4>
        <p>一键清除指定插座的所有定时任务。</p>
        <p><b>URL 端点:</b> <code>/deleteAllTasks</code></p>
        <p><b>必需参数:</b></p>
        <ul>
            <li><code><b>relay</b>=[1|2]</code>: 目标插座 (1 或 2)。</li>
        </ul>
        <p><b>示例:</b> 删除插座1的所有任务。</p>
        <p><code>http://[设备IP]/deleteAllTasks?relay=1</code></p>
    </div>

    <a href="/" class="nav-link">>> 返回风扇控制页面</a>
  </div>

  <script>
    function fetchJson(url,options){return fetch(url,options).then(r=>{if(!r.ok)throw new Error('Network error');return r.json()})}
    
    function updateStatus(data) {
        document.getElementById('timeDisplay').textContent = data.time;
        document.getElementById('r2_status').innerHTML = data.relay2 ? "<span class='status-on'>开启</span>" : "<span class='status-off'>关闭</span>";
        document.getElementById('r3_status').innerHTML = data.relay3 ? "<span class='status-on'>开启</span>" : "<span class='status-off'>关闭</span>";

        const r2list = document.getElementById('r2-list');
        r2list.innerHTML = '';
        if (data.r2_tasks.length === 0) r2list.innerHTML = '<li>无</li>';
        data.r2_tasks.forEach(task => {
            const timeStr = `${String(task.h).padStart(2,'0')}:${String(task.m).padStart(2,'0')}:${String(task.s).padStart(2,'0')}`;
            r2list.innerHTML += `<li>${timeStr} - ${task.a ? '开启' : '关闭'} <button onclick="deleteSchedule(1, '${timeStr}')">删除</button></li>`;
        });

        const r3list = document.getElementById('r3-list');
        r3list.innerHTML = '';
        if (data.r3_tasks.length === 0) r3list.innerHTML = '<li>无</li>';
        data.r3_tasks.forEach(task => {
            const timeStr = `${String(task.h).padStart(2,'0')}:${String(task.m).padStart(2,'0')}:${String(task.s).padStart(2,'0')}`;
            r3list.innerHTML += `<li>${timeStr} - ${task.a ? '开启' : '关闭'} <button onclick="deleteSchedule(2, '${timeStr}')">删除</button></li>`;
        });
    }

    function manualControl(relay, state) {
      fetch(`/setSocketTask?cmd=manual&relay=${relay}&state=${state}`).then(()=>setTimeout(refreshAllData, 200));
    }

    function addSchedule(action) {
      const relay = document.getElementById('relaySelect').value;
      const time = document.getElementById('scheduleTime').value;
      if (time) {
        fetch(`/setSocketTask?cmd=add&relay=${relay}&action=${action}&time=${time}`).then(()=>setTimeout(refreshAllData, 200));
      } else {
        alert('请先选择时间！');
      }
    }

    function deleteSchedule(relay, time) {
      if (confirm(`确定要为 插座 ${relay} 删除时间点 ${time} 吗?`)) {
        fetch(`/setSocketTask?cmd=delete&relay=${relay}&time=${time}`).then(()=>setTimeout(refreshAllData, 200));
      }
    }

    function deleteAll(relay) {
        if (confirm(`您确定要删除 插座 ${relay} 的所有定时任务吗？此操作无法撤销。`)) {
            fetch(`/deleteAllTasks?relay=${relay}`).then(()=>setTimeout(refreshAllData, 200));
        }
    }
    
    function refreshAllData(){
        fetchJson('/getSocketData').then(updateStatus).catch(e=>console.error('Failed to get socket data', e));
    }

    setInterval(refreshAllData, 2000);
    window.addEventListener('load', refreshAllData);
  </script>
</body>
</html>
)rawliteral";


// ============== 中断：测速脉冲计数 (风扇) ==============
void IRAM_ATTR tachISR() {
  uint32_t now = micros();
  if (now - lastPulseMicros >= MIN_PULSE_INTERVAL_US) {
    pulseCount++;
    lastPulseMicros = now;
  }
}

// ============== RPM 计算 (风扇) ==============
int computeRPM() {
  uint32_t now = millis();
  uint32_t elapsed = now - lastRpmCalcMs;
  if (elapsed < 1000) return -1;
  noInterrupts();
  uint32_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();
  lastRpmCalcMs = now;
  if (elapsed == 0) return 0;
  uint32_t rpm = (uint32_t)((uint64_t)pulses * 60000ULL / (elapsed * PULSES_PER_REV));
  if (rpm > MAX_REASONABLE_RPM) return lastRpm;
  lastRpm = (int)rpm;
  return lastRpm;
}

// ============== INA219 数据采样 (风扇) ==============
void sampleINA219() {
  if (!ina219_ok) return;
  loadVoltage = ina219.getBusVoltage_V() + (ina219.getShuntVoltage_mV() / 1000.0);
  current_mA = ina219.getCurrent_mA();
  power_mW = ina219.getPower_mW();
}

// ============== 继电器控制 ==============
void setRelay(bool state, bool manualOverride = false) {
  if (isLockedOut && state == true && manualOverride) {
    Serial.println("!!! 管理员手动覆盖低压锁定 !!!");
    isLockedOut = false;
  }
  
  if (isLockedOut && state == true) {
      Serial.println("继电器处于锁定状态，自动开启请求被拒绝。");
      return;
  }
  
  if (state == true && relayState == false) {
    lastRunStartTime = millis();
  }

  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  Serial.printf("风扇继电器 (Pin %d) 已设置为: %s\n", RELAY_PIN, relayState ? "ON" : "OFF");
}

void setSocketRelay(int internalRelayNum, bool state) {
  if (internalRelayNum == 2) {
    relay2State = state;
    digitalWrite(RELAY2_PIN, relay2State ? HIGH : LOW);
    Serial.printf("插座1继电器 (Pin %d) 已设置为: %s\n", RELAY2_PIN, relay2State ? "ON" : "OFF");
  } else if (internalRelayNum == 3) {
    relay3State = state;
    digitalWrite(RELAY3_PIN, relay3State ? HIGH : LOW);
    Serial.printf("插座2继电器 (Pin %d) 已设置为: %s\n", RELAY3_PIN, relay3State ? "ON" : "OFF");
  }
}

// ============== IPv6 地址获取函数 ==============
String formatIPv6(const esp_ip6_addr_t *addr) {
  if (addr == nullptr) return String("::");
  char buf[40];
  uint32_t word0 = ntohl(addr->addr[0]);
  uint32_t word1 = ntohl(addr->addr[1]);
  uint32_t word2 = ntohl(addr->addr[2]);
  uint32_t word3 = ntohl(addr->addr[3]);
  snprintf(buf, sizeof(buf), "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
           (uint16_t)(word0 >> 16), (uint16_t)(word0 & 0xFFFF),
           (uint16_t)(word1 >> 16), (uint16_t)(word1 & 0xFFFF),
           (uint16_t)(word2 >> 16), (uint16_t)(word2 & 0xFFFF),
           (uint16_t)(word3 >> 16), (uint16_t)(word3 & 0xFFFF));
  return String(buf);
}
String getIPv6() {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return String("Not Available");
  esp_ip6_addr_t addr;
  if (esp_netif_get_ip6_global(netif, &addr) == ESP_OK) {
      if (addr.addr[0] || addr.addr[1] || addr.addr[2] || addr.addr[3]) return formatIPv6(&addr);
  }
  if (esp_netif_get_ip6_linklocal(netif, &addr) == ESP_OK) {
    if (addr.addr[0] || addr.addr[1] || addr.addr[2] || addr.addr[3]) return formatIPv6(&addr);
  }
  return String("Not Available");
}

// =======================================================
// ============== 路由处理函数 (Web Handlers) ==============
// =======================================================

// ---------- 风扇页面 Handlers ----------
void handleRoot() { server.send(200, "text/html; charset=UTF-8", MAIN_HTML); }
void handleGetData() {
  int currentRpm = computeRPM();
  sampleINA219();
  timeClient.update();
  String json = "{";
  json += "\"rpm\":" + String(currentRpm < 0 ? lastRpm : currentRpm) + ",";
  json += "\"voltage\":" + String(loadVoltage) + ",";
  json += "\"current\":" + String(current_mA) + ",";
  json += "\"power\":" + String(power_mW) + ",";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"lockout\":" + String(isLockedOut ? "true" : "false") + ",";
  json += "\"lockout_trigger_v\":" + String(lockoutTriggerVoltage) + ",";
  json += "\"last_run_duration\":" + String(lastRunDurationMinutes) + ",";
  json += "\"lockout_stop_time\":\"" + String(lockoutStopTime) + "\",";
  if(isLockedOut) {
    long remaining_ms = LOCKOUT_DURATION_MS - (millis() - lockoutStartTime);
    json += "\"lockout_rem\":" + String(remaining_ms / 60000) + ",";
  } else {
    json += "\"lockout_rem\":0,";
  }
  json += "\"time\":\"" + timeClient.getFormattedTime() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}
void handleGetStatus() {
  String json = "{";
  json += "\"speed\":" + String(fanSliderValue) + ",";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"tasks\":[";
  for(int i=0; i<2; i++){
    json += "{\"enabled\":" + String(tasks[i].enabled?"true":"false");
    json += ",\"hour\":" + String(tasks[i].hour);
    json += ",\"minute\":" + String(tasks[i].minute);
    json += ",\"action\":" + String(tasks[i].action?"true":"false") + "}";
    if(i==0) json += ",";
  }
  json += "],";
  json += "\"low_voltage_threshold\":" + String(VOLTAGE_THRESHOLD) + ",";
  json += "\"high_voltage_threshold\":" + String(VOLTAGE_HIGH_THRESHOLD);
  json += "}";
  server.send(200, "application/json", json);
}
void handleGetPowerStats() {
  String json = "{\"today\": " + String(todayEnergyWh, 4) + ", \"history\": [";
  for(int i=0; i < HISTORY_DAYS; i++){
    json += String(dailyEnergyWh[i], 4);
    if(i < HISTORY_DAYS - 1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}
void handleSetSpeed() {
  if (server.hasArg("value")) {
    fanSliderValue = server.arg("value").toInt();
    int fanDuty = PWM_INVERTED ? (PWM_MAX - fanSliderValue) : fanSliderValue;
    ledcWrite(PWM_PIN, fanDuty);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}
void handleSetRelay() {
  if (server.hasArg("state")) {
    bool newState = server.arg("state").toInt() == 1;
    setRelay(newState, true);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}
void handleSetTimers() {
  for(int i=0; i<2; i++){
    String en_str = "t" + String(i+1) + "_en";
    String time_str = "t" + String(i+1) + "_time";
    String act_str = "t" + String(i+1) + "_act";
    if(server.hasArg(en_str) && server.hasArg(time_str) && server.hasArg(act_str)){
      tasks[i].enabled = server.arg(en_str) == "1";
      String timeVal = server.arg(time_str);
      tasks[i].hour = timeVal.substring(0,2).toInt();
      tasks[i].minute = timeVal.substring(3,5).toInt();
      tasks[i].action = server.arg(act_str) == "1";
      Serial.printf("风扇定时任务 %d: %s, %02d:%02d, Action: %s\n", i+1, tasks[i].enabled?"启用":"禁用", tasks[i].hour, tasks[i].minute, tasks[i].action?"ON":"OFF");
    }
  }
  server.send(200, "text/plain", "Timers Saved");
}

// ---------- 插座页面 Handlers ----------
void saveSocketSchedules(); // 前向声明
void handleSocketPage() { 
    String page = FPSTR(SOCKET_HTML);
    page.replace("##R2_PIN##", String(RELAY2_PIN));
    page.replace("##R3_PIN##", String(RELAY3_PIN));
    server.send(200, "text/html; charset=UTF-8", page); 
}
void handleGetSocketData() {
    timeClient.update();
    String json = "{";
    json += "\"time\":\"" + timeClient.getFormattedTime() + "\",";
    json += "\"relay2\":" + String(relay2State ? "true" : "false") + ",";
    json += "\"relay3\":" + String(relay3State ? "true" : "false") + ",";
    
    json += "\"r2_tasks\":[";
    for(int i=0; i < relay2_schedule_count; i++) {
        json += "{\"h\":" + String(relay2_schedules[i].hour) + ", \"m\":" + String(relay2_schedules[i].minute) + ", \"s\":" + String(relay2_schedules[i].second) + ", \"a\":" + String(relay2_schedules[i].action ? 1:0) + "}";
        if (i < relay2_schedule_count - 1) json += ",";
    }
    json += "],";

    json += "\"r3_tasks\":[";
    for(int i=0; i < relay3_schedule_count; i++) {
        json += "{\"h\":" + String(relay3_schedules[i].hour) + ", \"m\":" + String(relay3_schedules[i].minute) + ", \"s\":" + String(relay3_schedules[i].second) + ", \"a\":" + String(relay3_schedules[i].action ? 1:0) + "}";
        if (i < relay3_schedule_count - 1) json += ",";
    }
    json += "]}";

    server.send(200, "application/json", json);
}
void handleSetSocketTask() {
    if (!server.hasArg("cmd") || !server.hasArg("relay")) { 
        server.send(400, "text/plain", "Bad Request: Missing cmd or relay parameter"); 
        return; 
    }

    int relayIndex = server.arg("relay").toInt();
    int internalRelayNum = 0;
    if (relayIndex == 1) internalRelayNum = 2;
    else if (relayIndex == 2) internalRelayNum = 3;
    else {
        server.send(400, "text/plain", "Bad Request: Invalid relay index. Must be 1 or 2.");
        return;
    }

    String cmd = server.arg("cmd");

    if (cmd == "manual" && server.hasArg("state")) {
        bool state = server.arg("state").toInt() == 1;
        setSocketRelay(internalRelayNum, state);
    } 
    else if (cmd == "add" && server.hasArg("action") && server.hasArg("time")) {
        if ((internalRelayNum == 2 && relay2_schedule_count >= MAX_SOCKET_SCHEDULES) || (internalRelayNum == 3 && relay3_schedule_count >= MAX_SOCKET_SCHEDULES)) {
            server.send(400, "text/plain", "Schedule list is full");
            return;
        }

        SocketSchedule newSchedule;
        newSchedule.action = server.arg("action").toInt() == 1;
        newSchedule.enabled = true;
        String timeVal = server.arg("time");
        newSchedule.hour = timeVal.substring(0,2).toInt();
        newSchedule.minute = timeVal.substring(3,5).toInt();
        newSchedule.second = timeVal.length() > 5 ? timeVal.substring(6,8).toInt() : 0;
        
        if (internalRelayNum == 2) relay2_schedules[relay2_schedule_count++] = newSchedule;
        else if (internalRelayNum == 3) relay3_schedules[relay3_schedule_count++] = newSchedule;
        saveSocketSchedules();
    } 
    else if (cmd == "delete" && server.hasArg("time")) {
        String timeVal = server.arg("time");
        int h = timeVal.substring(0,2).toInt();
        int m = timeVal.substring(3,5).toInt();
        int s = timeVal.length() > 5 ? timeVal.substring(6,8).toInt() : 0;

        int indexToDelete = -1;
        if (internalRelayNum == 2) {
            for (int i=0; i<relay2_schedule_count; i++) {
                if(relay2_schedules[i].hour == h && relay2_schedules[i].minute == m && relay2_schedules[i].second == s) {
                    indexToDelete = i; break;
                }
            }
            if (indexToDelete != -1) {
                for (int i = indexToDelete; i < relay2_schedule_count - 1; i++) relay2_schedules[i] = relay2_schedules[i+1];
                relay2_schedule_count--;
            }
        } else if (internalRelayNum == 3) {
            for (int i=0; i<relay3_schedule_count; i++) {
                if(relay3_schedules[i].hour == h && relay3_schedules[i].minute == m && relay3_schedules[i].second == s) {
                    indexToDelete = i; break;
                }
            }
            if (indexToDelete != -1) {
                for (int i = indexToDelete; i < relay3_schedule_count - 1; i++) relay3_schedules[i] = relay3_schedules[i+1];
                relay3_schedule_count--;
            }
        }
        saveSocketSchedules();
    }
    
    server.send(200, "text/plain", "OK");
}
void handleDeleteAllTasks() {
    if (!server.hasArg("relay")) {
        server.send(400, "text/plain", "Bad Request: Missing relay parameter");
        return;
    }
    
    int relayIndex = server.arg("relay").toInt();
    int internalRelayNum = 0;
    if (relayIndex == 1) internalRelayNum = 2;
    else if (relayIndex == 2) internalRelayNum = 3;
    else {
        server.send(400, "text/plain", "Bad Request: Invalid relay index. Must be 1 or 2.");
        return;
    }

    if (internalRelayNum == 2) {
        relay2_schedule_count = 0;
        Serial.println("已清除插座1的所有定时任务。");
    } else if (internalRelayNum == 3) {
        relay3_schedule_count = 0;
        Serial.println("已清除插座2的所有定时任务。");
    }
    
    saveSocketSchedules();
    server.send(200, "text/plain", "All tasks deleted successfully.");
}
void handleApiAddTask() {
    if (!server.hasArg("ledPwm") || !server.hasArg("time") || !server.hasArg("relay")) {
        server.send(400, "text/plain", "Bad Request: Missing one or more parameters (ledPwm, time, relay).");
        return;
    }

    int action = server.arg("ledPwm").toInt();
    if (action != 0 && action != 1) {
        server.send(400, "text/plain", "Bad Request: 'ledPwm' parameter must be 0 or 1.");
        return;
    }

    int relayIndex = server.arg("relay").toInt();
    int internalRelayNum = 0;
    if (relayIndex == 1) internalRelayNum = 2;
    else if (relayIndex == 2) internalRelayNum = 3;
    else {
        server.send(400, "text/plain", "Bad Request: 'relay' parameter must be 1 or 2.");
        return;
    }

    String timeVal = server.arg("time");
    if (timeVal.length() < 5) {
        server.send(400, "text/plain", "Bad Request: Invalid 'time' format. Use HH:MM:SS.");
        return;
    }
    
    if ((internalRelayNum == 2 && relay2_schedule_count >= MAX_SOCKET_SCHEDULES) || (internalRelayNum == 3 && relay3_schedule_count >= MAX_SOCKET_SCHEDULES)) {
        server.send(409, "text/plain", "Conflict: Schedule list for the selected relay is full.");
        return;
    }

    SocketSchedule newSchedule;
    newSchedule.action = (action == 1);
    newSchedule.enabled = true;
    newSchedule.hour = timeVal.substring(0,2).toInt();
    newSchedule.minute = timeVal.substring(3,5).toInt();
    newSchedule.second = timeVal.length() > 5 ? timeVal.substring(6,8).toInt() : 0;

    if (internalRelayNum == 2) {
        relay2_schedules[relay2_schedule_count++] = newSchedule;
    } else { // internalRelayNum is 3
        relay3_schedules[relay3_schedule_count++] = newSchedule;
    }

    saveSocketSchedules();
    String successMessage = "Task added successfully for Relay " + String(relayIndex) + " at " + timeVal;
    Serial.println(successMessage);
    server.send(200, "text/plain", successMessage);
}

// ---------- 通用 Handlers ----------
String getSystemInfoJSON() {
  String j = "{";
  j += "\"chip_model\":\"" + String(ESP.getChipModel()) + "\",";
  j += "\"chip_rev\":" + String(ESP.getChipRevision()) + ",";
  j += "\"cpu_freq_mhz\":" + String(ESP.getCpuFreqMHz()) + ",";
  j += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"ipv6\":\"" + getIPv6() + "\"";
  j += "}";
  return j;
}
void handleSysInfo() { server.send(200, "application/json", getSystemInfoJSON()); }
void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  switch(upload.status) {
    case UPLOAD_FILE_START: if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial); break;
    case UPLOAD_FILE_WRITE: if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial); break;
    case UPLOAD_FILE_END: if (!Update.end(true)) Update.printError(Serial); break;
  }
  yield();
}

// ============== 定时任务检查 ==============
void checkFanTimers() {
  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  
  for(int i=0; i<2; i++){
    if(tasks[i].enabled && tasks[i].hour == currentHour && tasks[i].minute == currentMinute) {
      if(relayState != tasks[i].action) {
        Serial.printf("执行风扇定时任务 %d: %s\n", i+1, tasks[i].action?"开启":"关闭");
        setRelay(tasks[i].action);
      }
    }
  }
}
void checkSocketSchedules() {
    timeClient.update();
    int h = timeClient.getHours();
    int m = timeClient.getMinutes();
    int s = timeClient.getSeconds();

    for (int i = 0; i < relay2_schedule_count; i++) {
        if (relay2_schedules[i].enabled && relay2_schedules[i].hour == h && relay2_schedules[i].minute == m && relay2_schedules[i].second == s) {
            if (relay2State != relay2_schedules[i].action) {
                Serial.printf("执行插座1定时任务: %s\n", relay2_schedules[i].action ? "开启" : "关闭");
                setSocketRelay(2, relay2_schedules[i].action);
            }
        }
    }
    for (int i = 0; i < relay3_schedule_count; i++) {
        if (relay3_schedules[i].enabled && relay3_schedules[i].hour == h && relay3_schedules[i].minute == m && relay3_schedules[i].second == s) {
            if (relay3State != relay3_schedules[i].action) {
                Serial.printf("执行插座2定时任务: %s\n", relay3_schedules[i].action ? "开启" : "关闭");
                setSocketRelay(3, relay3_schedules[i].action);
            }
        }
    }
}


// ============== 智能电源管理逻辑 (风扇) ==============
void checkVoltageProtection() {
  if (!ina219_ok) return;

  if (isLockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_DURATION_MS) {
      Serial.println("锁定时间已到，正在检查电压以尝试自动恢复...");
      sampleINA219();
      if (loadVoltage >= VOLTAGE_HIGH_THRESHOLD) {
        Serial.printf("电压已恢复至安全水平 (%.2fV > %.2fV)。自动重新开启继电器。\n", loadVoltage, VOLTAGE_HIGH_THRESHOLD);
        isLockedOut = false;
        setRelay(true);
      } else {
        Serial.printf("电压仍然不足 (%.2fV)。解除锁定，但继电器保持关闭，等待手动操作或充电。\n", loadVoltage);
        isLockedOut = false;
      }
    }
    return;
  }

  if (!relayState && VOLTAGE_HIGH_THRESHOLD > 0) {
    sampleINA219();
    if (loadVoltage >= VOLTAGE_HIGH_THRESHOLD) {
      Serial.printf("检测到高电压 (%.2fV)，自动开启继电器。\n", loadVoltage);
      setRelay(true);
    }
  }

  if (relayState) {
    sampleINA219();
    if (loadVoltage > 0.1 && loadVoltage < VOLTAGE_THRESHOLD) {
      Serial.printf("!!! 触发低压保护: V=%.2fV (阈值: %.2fV)\n", loadVoltage, VOLTAGE_THRESHOLD);
      timeClient.update();
      String formattedTime = timeClient.getFormattedTime();
      snprintf(lockoutStopTime, 6, "%s", formattedTime.substring(0, 5).c_str());
      unsigned long lastRunDurationMs = millis() - lastRunStartTime;
      lastRunDurationMinutes = lastRunDurationMs / 60000;
      Serial.printf("!!! 上次运行了 %ld 分钟。停止于 %s\n", lastRunDurationMinutes, lockoutStopTime);
      Serial.println("!!! 继电器将关闭并锁定1小时。");
      isLockedOut = true;
      lockoutStartTime = millis();
      lockoutTriggerVoltage = loadVoltage;
      setRelay(false);
    }
  }
}

// ============== 电量计算与存储逻辑 (风扇) ==============
void accumulateEnergy() {
  if ( !ina219_ok) { lastEnergyCalcMs = millis(); return; }
  unsigned long now = millis();
  unsigned long elapsedMs = now - lastEnergyCalcMs;
  if (elapsedMs > 0) {
    double elapsedHours = (double)elapsedMs / 3600000.0;
    double powerWatts = (double)power_mW / 1000.0;
    todayEnergyWh += powerWatts * elapsedHours;
  }
  lastEnergyCalcMs = now;
}
void checkDailyRollover() {
  timeClient.update();
  int currentDay = timeClient.getDay();
  if (lastDayChecked == -1) {
      lastDayChecked = currentDay;
      preferences.begin("fan-stats", false);
      preferences.putInt("lastDay", lastDayChecked);
      preferences.end();
      return;
  }
  if (lastDayChecked != currentDay) {
    Serial.printf("检测到日期变更 (从 %d 到 %d)。正在处理电量数据...\n", lastDayChecked, currentDay);
    preferences.begin("fan-stats", false);
    for (int i = HISTORY_DAYS - 1; i > 0; i--) dailyEnergyWh[i] = dailyEnergyWh[i - 1];
    dailyEnergyWh[0] = todayEnergyWh;
    char key[10];
    for (int i = 0; i < HISTORY_DAYS; i++) {
      sprintf(key, "dayWh_%d", i);
      preferences.putFloat(key, dailyEnergyWh[i]);
    }
    todayEnergyWh = 0.0;
    lastDayChecked = currentDay;
    preferences.putFloat("todayWh", todayEnergyWh);
    preferences.putInt("lastDay", lastDayChecked);
    preferences.end();
    Serial.println("电量数据处理完毕。");
  }
}

// ============== 插座定时任务持久化 ==============
void loadSocketSchedules() {
    preferences.begin("socket-sched", true);
    relay2_schedule_count = preferences.getInt("r2_count", 0);
    relay3_schedule_count = preferences.getInt("r3_count", 0);

    for (int i=0; i < relay2_schedule_count; i++) {
        char key_base[10];
        sprintf(key_base, "r2_%d_", i);
        relay2_schedules[i].hour = preferences.getUChar((String(key_base) + "h").c_str(), 0);
        relay2_schedules[i].minute = preferences.getUChar((String(key_base) + "m").c_str(), 0);
        relay2_schedules[i].second = preferences.getUChar((String(key_base) + "s").c_str(), 0);
        relay2_schedules[i].action = preferences.getBool((String(key_base) + "a").c_str(), false);
        relay2_schedules[i].enabled = true;
    }
    for (int i=0; i < relay3_schedule_count; i++) {
        char key_base[10];
        sprintf(key_base, "r3_%d_", i);
        relay3_schedules[i].hour = preferences.getUChar((String(key_base) + "h").c_str(), 0);
        relay3_schedules[i].minute = preferences.getUChar((String(key_base) + "m").c_str(), 0);
        relay3_schedules[i].second = preferences.getUChar((String(key_base) + "s").c_str(), 0);
        relay3_schedules[i].action = preferences.getBool((String(key_base) + "a").c_str(), false);
        relay3_schedules[i].enabled = true;
    }
    preferences.end();
    Serial.printf("[OK] 已加载插座定时任务: 插座1 (%d个), 插座2 (%d个)\n", relay2_schedule_count, relay3_schedule_count);
}
void saveSocketSchedules() {
    preferences.begin("socket-sched", false);
    preferences.clear();
    preferences.putInt("r2_count", relay2_schedule_count);
    preferences.putInt("r3_count", relay3_schedule_count);

    for (int i=0; i < relay2_schedule_count; i++) {
        char key_base[10];
        sprintf(key_base, "r2_%d_", i);
        preferences.putUChar((String(key_base) + "h").c_str(), relay2_schedules[i].hour);
        preferences.putUChar((String(key_base) + "m").c_str(), relay2_schedules[i].minute);
        preferences.putUChar((String(key_base) + "s").c_str(), relay2_schedules[i].second);
        preferences.putBool((String(key_base) + "a").c_str(), relay2_schedules[i].action);
    }
    for (int i=0; i < relay3_schedule_count; i++) {
        char key_base[10];
        sprintf(key_base, "r3_%d_", i);
        preferences.putUChar((String(key_base) + "h").c_str(), relay3_schedules[i].hour);
        preferences.putUChar((String(key_base) + "m").c_str(), relay3_schedules[i].minute);
        preferences.putUChar((String(key_base) + "s").c_str(), relay3_schedules[i].second);
        preferences.putBool((String(key_base) + "a").c_str(), relay3_schedules[i].action);
    }
    preferences.end();
    Serial.println("[OK] 插座定时任务已保存到闪存。");
}

// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP32C3 智能风扇 & 双路定时插座控制器 v4.7 =====");
  
  Serial.println("--- 硬件初始化开始 ---");
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // 默认关闭
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  digitalWrite(RELAY2_PIN, LOW); // 默认先关闭
  digitalWrite(RELAY3_PIN, LOW); // 默认先关闭
  Serial.printf("[OK] 所有继电器引脚初始化完成。\n");
  
  ledcAttach(PWM_PIN, LEDC_FREQUENCY, LEDC_RES_BITS);
  ledcWrite(PWM_PIN, PWM_INVERTED ? PWM_MAX : 0);
  Serial.printf("[OK] PWM 引脚 %d 初始化完成。\n", PWM_PIN);

  pinMode(TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachISR, FALLING);
  Serial.printf("[OK] 测速引脚 %d (中断) 初始化完成。\n", TACH_PIN);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!ina219.begin()) {
    Serial.println("[!!] 硬件错误: 未能找到INA219芯片。请检查接线！");
    ina219_ok = false;
  } else {
    Serial.println("[OK] INA219 通信成功。");
    ina219.setCalibration_32V_2A();
    ina219_ok = true;
  }
  Serial.println("--- 硬件初始化结束 ---\n");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName);
  WiFi.enableIPv6(); 
  WiFi.begin(ssid, password);
  Serial.print("连接 Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println("\n[OK] WiFi 已连接!");
  Serial.print("IPv4 地址: "); Serial.println(WiFi.localIP());
  Serial.print("IPv6 地址: "); Serial.println(getIPv6());

  timeClient.begin();
  Serial.print("正在同步NTP时间...");
  while (!timeClient.update()) {
    timeClient.forceUpdate();
    Serial.print(".");
    delay(500);
  }
  Serial.println("\n[OK] NTP 时间服务已同步。");

  // 加载永久保存的定时任务
  loadSocketSchedules();

  // --- 新增: 通电默认开启半小时逻辑 ---
  Serial.println("[OK] 执行通电启动逻辑: 默认开启插座并创建30分钟后关闭的临时任务。");
  time_t offTimeEpoch = timeClient.getEpochTime() + (30 * 60);
  struct tm *offTimeInfo;
  offTimeInfo = localtime(&offTimeEpoch);

  // 创建临时关闭任务
  SocketSchedule tempOffTask;
  tempOffTask.hour = offTimeInfo->tm_hour;
  tempOffTask.minute = offTimeInfo->tm_min;
  tempOffTask.second = offTimeInfo->tm_sec;
  tempOffTask.action = false; // 关闭动作
  tempOffTask.enabled = true;

  // 为插座1添加临时任务 (如果列表未满)
  if (relay2_schedule_count < MAX_SOCKET_SCHEDULES) {
    relay2_schedules[relay2_schedule_count++] = tempOffTask;
    Serial.printf("  -> 已为插座1创建临时关闭任务于 %02d:%02d:%02d\n", tempOffTask.hour, tempOffTask.minute, tempOffTask.second);
  }
  // 为插座2添加临时任务 (如果列表未满)
  if (relay3_schedule_count < MAX_SOCKET_SCHEDULES) {
    relay3_schedules[relay3_schedule_count++] = tempOffTask;
     Serial.printf("  -> 已为插座2创建临时关闭任务于 %02d:%02d:%02d\n", tempOffTask.hour, tempOffTask.minute, tempOffTask.second);
  }
  
  // 立即开启两个插座
  setSocketRelay(2, true);
  setSocketRelay(3, true);
  
  preferences.begin("fan-stats", true);
  lastDayChecked = preferences.getInt("lastDay", -1);
  int currentDay = timeClient.getDay();
  if (lastDayChecked == currentDay) {
    todayEnergyWh = preferences.getFloat("todayWh", 0.0);
  } else {
    todayEnergyWh = 0.0;
  }
  char key[10];
  for (int i = 0; i < HISTORY_DAYS; i++) {
    sprintf(key, "dayWh_%d", i);
    dailyEnergyWh[i] = preferences.getFloat(key, 0.0);
  }
  preferences.end();
  Serial.println("[OK] 风扇电量统计数据已加载。");

  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", WEB_SERVER_PORT);
    Serial.printf("[OK] mDNS 已启动，访问地址: http://%s.local:%d\n", deviceName, WEB_SERVER_PORT);
  }

  // 风扇页面路由
  server.on("/", HTTP_GET, handleRoot);
  server.on("/getData", HTTP_GET, handleGetData);
  server.on("/getStatus", HTTP_GET, handleGetStatus);
  server.on("/getPowerStats", HTTP_GET, handleGetPowerStats);
  server.on("/setSpeed", HTTP_GET, handleSetSpeed);
  server.on("/setRelay", HTTP_GET, handleSetRelay);
  server.on("/setTimers", HTTP_POST, handleSetTimers);
  // 插座页面路由
  server.on("/socket", HTTP_GET, handleSocketPage);
  server.on("/getSocketData", HTTP_GET, handleGetSocketData);
  server.on("/setSocketTask", HTTP_GET, handleSetSocketTask);
  server.on("/deleteAllTasks", HTTP_GET, handleDeleteAllTasks);
  server.on("/LED-Control", HTTP_GET, handleApiAddTask);
  // 通用路由
  server.on("/sysinfo", HTTP_GET, handleSysInfo);
  server.on("/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "更新失败" : "更新成功!");
      delay(1000);
      ESP.restart();
    }, handleUpdateUpload);
  
  server.begin();
  Serial.printf("[OK] HTTP 服务器已在端口 %d 启动。\n========================================\n", WEB_SERVER_PORT);
  lastEnergyCalcMs = millis();
}

// ============== LOOP ==============
void loop() {
  server.handleClient();
  
  static unsigned long lastTimerCheck = 0;
  static unsigned long lastVoltageCheck = 0;
  static unsigned long lastDataSave = 0;
  static unsigned long lastSocketScheduleCheck = 0;

  if (millis() - lastSocketScheduleCheck >= 1000) {
      lastSocketScheduleCheck = millis();
      checkSocketSchedules();
  }

  accumulateEnergy();

  if (millis() - lastVoltageCheck > 5000) {
    lastVoltageCheck = millis();
    checkVoltageProtection();
  }
  
  if (millis() - lastTimerCheck > 30000) {
    lastTimerCheck = millis();
    checkDailyRollover();
    checkFanTimers();
  }
  
  if (millis() - lastDataSave > 60000) {
    lastDataSave = millis();
    preferences.begin("fan-stats", false);
    preferences.putFloat("todayWh", todayEnergyWh);
    preferences.end();
  }
}