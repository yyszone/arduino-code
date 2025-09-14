// =======================================================================================
// ==     ESP32C3 智能风扇控制器 v3.4.1 (最终修复版: 修复重定义错误/可手动解锁)     ==
// =======================================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ============== 用户配置 ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp32-smart-fan";

// ============== 硬件引脚配置 ==============
const int PWM_PIN    = 5;
const int TACH_PIN   = 4;
const int RELAY_PIN  = 1;
const int I2C_SDA_PIN = 10;
const int I2C_SCL_PIN = 8;

// ============== PWM/LEDC 配置 ==============
const int LEDC_FREQUENCY = 25000;
const int LEDC_RES_BITS  = 8;
const int PWM_MAX        = (1 << LEDC_RES_BITS) - 1;
const bool PWM_INVERTED  = false;

// ============== RPM 采样配置 ==============
const int PULSES_PER_REV = 2;
const uint32_t MIN_PULSE_INTERVAL_US = 800;
const int MAX_REASONABLE_RPM = 15000;

// ============== 继电器与智能电源管理配置 ==============
const float VOLTAGE_THRESHOLD = 2.0;
const float VOLTAGE_HIGH_THRESHOLD = 4.0;
const long LOCKOUT_DURATION_MS = 3600000;

// ============== 时间与定时任务配置 ==============
const char* NTP_SERVER = "ntp.aliyun.com";
const long  GMT_OFFSET_SEC = 8 * 3600;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, GMT_OFFSET_SEC);
struct TimerTask { int hour; int minute; bool action; bool enabled; };
TimerTask tasks[2] = { {0, 0, false, false}, {0, 0, false, false} };

// ============== 全局对象与变量 ==============
WebServer server(80);
Adafruit_INA219 ina219;
volatile uint32_t pulseCount = 0;
volatile uint32_t lastPulseMicros = 0;
uint32_t lastRpmCalcMs = 0;
int fanSliderValue = 0;
int lastRpm = 0;
float loadVoltage = 0, current_mA = 0, power_mW = 0;
float lockoutTriggerVoltage = 0.0;
bool ina219_ok = false;
bool relayState = false;
bool isLockedOut = false;
unsigned long lockoutStartTime = 0;

// ============== 中断：测速脉冲计数 ==============
void IRAM_ATTR tachISR() {
  uint32_t now = micros();
  if (now - lastPulseMicros >= MIN_PULSE_INTERVAL_US) {
    pulseCount++;
    lastPulseMicros = now;
  }
}

// ============== RPM 计算 ==============
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

// ============== INA219 数据采样 ==============
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
  
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  Serial.printf("继电器 (Pin %d) 已设置为: %s\n", RELAY_PIN, relayState ? "ON (HIGH)" : "OFF (LOW)");
}

// ============== 网页 (HTML+CSS+JS) ==============
const char MAIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 智能风扇控制</title><style>:root{--bg:#0f172a;--card:#111827;--text:#e5e7eb;--accent:#22c55e;--muted:#94a3b8;--red:#ef4444;--blue:#3b82f6;}*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,'Helvetica Neue',Arial}.wrapper{min-height:100vh;background:linear-gradient(135deg,#0f172a,#1f2937);color:var(--text);display:flex;align-items:center;justify-content:center;padding:18px}.container{width:100%;max-width:600px}.card{background:linear-gradient(180deg,#0b1220,#0b1220) padding-box,linear-gradient(135deg,#22c55e33,#06b6d433) border-box;border:1px solid transparent;border-radius:16px;padding:20px;margin:14px 0;box-shadow:0 10px 30px rgba(0,0,0,.35)}h1,h2{margin:0 0 12px}h1{text-align:center;font-weight:700;font-size:22px}h2{font-size:18px;color:#d1d5db}.label{margin:8px 0 6px;font-weight:600}.value{font-feature-settings:'tnum' 1;letter-spacing:.3px}.slider{width:100%}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px;text-align:center}.data-box{padding:10px;border-radius:8px;background-color:rgba(0,0,0,.2)}.data-box .val{font-size:1.8em;color:var(--accent)}.data-box .unit{color:var(--muted);font-size:0.9em}small{color:var(--muted)}.btn{background:var(--accent);border:none;color:#052e13;padding:12px 18px;border-radius:10px;font-weight:700;cursor:pointer;box-shadow:0 6px 16px rgba(34,197,94,.35)}.btn.off{background:var(--red);color:#fff}.btn:hover{filter:brightness(1.05)}input[type=file]{color:var(--text)}pre{white-space:pre-wrap;word-break:break-word}.timer-row{display:flex;align-items:center;gap:10px;margin:10px 0}input[type=time],input[type=checkbox]{margin-right:5px}</style></head><body><div class="wrapper"><div class="container"><div class="card"><h1>ESP32 智能风扇控制面板</h1><div class="label">当前时间: <span id="currentTime">--:--:--</span></div><div id="lockoutStatus" style="color:var(--red);margin-bottom:10px;display:none"></div><div class="grid"><div class="data-box"><div>转速 (RPM)</div><div class="val" id="rpm">--</div><div class="unit">&nbsp;</div></div><div class="data-box"><div>电压 (V)</div><div class="val" id="v">--</div><div class="unit">&nbsp;</div></div><div class="data-box"><div>电流 (mA)</div><div class="val" id="c">--</div><div class="unit">&nbsp;</div></div><div class="data-box"><div>功率 (mW)</div><div class="val" id="p">--</div><div class="unit">&nbsp;</div></div></div></div><div class="card"><h2>手动控制</h2><div class="label">风扇调速: <span id="spd" class="value">--</span></div><input id="speed" class="slider" type="range" min="0" max="255" value="0"/>
<small id="auto_relay_info" style="display:block; text-align:center; margin-top:10px;">读取设定值...</small>
<div style="margin-top:20px;display:flex;justify-content:center;gap:15px"><button id="relayBtn" class="btn">读取状态...</button></div></div><div class="card"><h2>定时任务</h2><div class="timer-row"><input type="checkbox" id="t1_en"><input type="time" id="t1_time"><select id="t1_act"><option value="1">开启</option><option value="0">关闭</option></select></div><div class="timer-row"><input type="checkbox" id="t2_en"><input type="time" id="t2_time"><select id="t2_act"><option value="1">开启</option><option value="0">关闭</option></select></div><button id="saveTimerBtn" class="btn" style="background:var(--blue);margin-top:10px">保存定时设置</button></div><div class="card"><h2>系统信息</h2><div id="sys">加载中...</div></div><div class="card"><h2>固件在线更新 OTA</h2><form method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz"/><br/><br/><button class="btn" type="submit">上传并更新</button></form></div></div></div>
<script>
const spd=document.getElementById('spd'),rpm=document.getElementById('rpm'),slider=document.getElementById('speed'),sys=document.getElementById('sys'),v_el=document.getElementById('v'),c_el=document.getElementById('c'),p_el=document.getElementById('p'),relayBtn=document.getElementById('relayBtn'),lockoutEl=document.getElementById('lockoutStatus'),timeEl=document.getElementById('currentTime'),autoInfoEl=document.getElementById('auto_relay_info');
function setLabel(v){const p=Math.round(v/255*100);spd.textContent=v+' ('+p+'%)'}
function fetchJson(url,options){return fetch(url,options).then(r=>{if(!r.ok)throw new Error('Network error');return r.json()})}
slider.addEventListener('input',()=>{const v=slider.value;setLabel(v);fetch('/setSpeed?value='+v).catch(e=>console.error(e))});
relayBtn.addEventListener('click',()=>{const newState=!relayBtn.classList.contains('off');fetch('/setRelay?state='+(newState?'1':'0')).then(()=>{updateRelayBtn(newState)})});
function updateRelayBtn(state){if(state){relayBtn.textContent='关闭继电器';relayBtn.classList.add('off')}else{relayBtn.textContent='开启继电器';relayBtn.classList.remove('off')}}
function saveTimers(){
  const data=new FormData();
  data.append('t1_en',document.getElementById('t1_en').checked?'1':'0');data.append('t1_time',document.getElementById('t1_time').value);data.append('t1_act',document.getElementById('t1_act').value);
  data.append('t2_en',document.getElementById('t2_en').checked?'1':'0');data.append('t2_time',document.getElementById('t2_time').value);data.append('t2_act',document.getElementById('t2_act').value);
  fetch('/setTimers',{method:'POST',body:data}).then(r=>alert(r.ok?'定时任务已保存':'保存失败')).catch(e=>alert('保存出错'));
}
document.getElementById('saveTimerBtn').addEventListener('click',saveTimers);
window.addEventListener('load',()=>{
  fetchJson('/getStatus').then(data=>{
    slider.value=data.speed;setLabel(data.speed);updateRelayBtn(data.relay);
    document.getElementById('t1_en').checked=data.tasks[0].enabled;
    document.getElementById('t1_time').value=String(data.tasks[0].hour).padStart(2,'0')+':'+String(data.tasks[0].minute).padStart(2,'0');
    document.getElementById('t1_act').value=data.tasks[0].action?'1':'0';
    document.getElementById('t2_en').checked=data.tasks[1].enabled;
    document.getElementById('t2_time').value=String(data.tasks[1].hour).padStart(2,'0')+':'+String(data.tasks[1].minute).padStart(2,'0');
    document.getElementById('t2_act').value=data.tasks[1].action?'1':'0';
    if(data.high_voltage_threshold>0){autoInfoEl.textContent=`自动模式: 低于 ${data.low_voltage_threshold.toFixed(1)}V 关闭，高于 ${data.high_voltage_threshold.toFixed(1)}V 开启 (保护后锁定1小时)。`}
    else{autoInfoEl.textContent=`自动启动已禁用。仅启用低于 ${data.low_voltage_threshold.toFixed(1)}V 的低压保护。`}
  }).catch(e=>console.error(e));
});
setInterval(()=>{
  fetchJson('/getData').then(data=>{
    rpm.textContent=data.rpm;v_el.textContent=data.voltage.toFixed(2);c_el.textContent=data.current.toFixed(1);p_el.textContent=data.power.toFixed(0);timeEl.textContent=data.time;
    if(data.lockout){lockoutEl.style.display='block';lockoutEl.textContent='低压保护已触发 ('+data.lockout_trigger_v.toFixed(2)+'V)! 继电器已锁定关闭。 剩余时间: '+data.lockout_rem+' 分钟'}
    else{lockoutEl.style.display='none'}
    updateRelayBtn(data.relay);
  }).catch(e=>console.error(e));
  fetchJson('/sysinfo').then(info=>{sys.innerHTML=`芯片: ${info.chip_model} (rev ${info.chip_rev})<br>CPU: ${info.cpu_freq_mhz} MHz<br>空闲内存: ${info.free_heap} B<br>IP: ${info.ip}`}).catch(e=>console.error(e));
},1500);
</script></body></html>
)HTML";

// ============== 路由处理函数 ==============
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

void handleRoot() { server.send(200, "text/html; charset=UTF-8", MAIN_HTML); }

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
      Serial.printf("定时任务 %d: %s, %02d:%02d, Action: %s\n", i+1, tasks[i].enabled?"启用":"禁用", tasks[i].hour, tasks[i].minute, tasks[i].action?"ON":"OFF");
    }
  }
  server.send(200, "text/plain", "Timers Saved");
}

String getSystemInfoJSON() {
  String j = "{";
  j += "\"chip_model\":\"" + String(ESP.getChipModel()) + "\",";
  j += "\"chip_rev\":" + String(ESP.getChipRevision()) + ",";
  j += "\"cpu_freq_mhz\":" + String(ESP.getCpuFreqMHz()) + ",";
  j += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += "}";
  return j;
}

void handleSysInfo() { server.send(200, "application/json", getSystemInfoJSON()); }

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  switch(upload.status) {
    case UPLOAD_FILE_START:
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      break;
    case UPLOAD_FILE_WRITE:
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
      break;
    case UPLOAD_FILE_END:
      if (!Update.end(true)) Update.printError(Serial);
      break;
  }
  yield();
}

// ============== 定时任务检查 ==============
void checkTimers() {
  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  
  for(int i=0; i<2; i++){
    if(tasks[i].enabled && tasks[i].hour == currentHour && tasks[i].minute == currentMinute) {
      if(relayState != tasks[i].action) {
        Serial.printf("执行定时任务 %d: %s\n", i+1, tasks[i].action?"开启":"关闭");
        setRelay(tasks[i].action);
      }
    }
  }
}

// ============== 智能电源管理逻辑 ==============
void checkVoltageProtection() {
  if (!ina219_ok) return;

  if (isLockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_DURATION_MS) {
      Serial.println("锁定时间已到，正在检查电压以尝试自动恢复...");
      sampleINA219();
      if (loadVoltage >= VOLTAGE_THRESHOLD) {
        Serial.printf("电压已恢复 (%.2fV)。自动重新开启继电器。\n", loadVoltage);
        isLockedOut = false;
        setRelay(true);
      } else {
        Serial.printf("电压仍然过低 (%.2fV)。解除锁定，但继电器保持关闭。\n", loadVoltage);
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
      Serial.println("!!! 继电器将关闭并锁定1小时。");
      isLockedOut = true;
      lockoutStartTime = millis();
      lockoutTriggerVoltage = loadVoltage;
      setRelay(false);
    }
  }
}

// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP32C3 智能风扇控制器 v3.4.1 (最终修复版) =====");
  
  Serial.println("--- 硬件初始化开始 ---");
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);
  Serial.printf("[OK] 继电器引脚 %d 初始化完成，默认关闭。\n", RELAY_PIN);

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
  WiFi.begin(ssid, password);
  Serial.print("连接 Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println("\n[OK] WiFi 已连接!");
  Serial.print("IP 地址: "); Serial.println(WiFi.localIP());

  timeClient.begin();
  Serial.println("[OK] NTP 时间服务已启动。");

  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[OK] mDNS 已启动，访问地址: http://%s.local\n", deviceName);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/getData", HTTP_GET, handleGetData);
  server.on("/getStatus", HTTP_GET, handleGetStatus);
  server.on("/setSpeed", HTTP_GET, handleSetSpeed);
  server.on("/setRelay", HTTP_GET, handleSetRelay);
  server.on("/setTimers", HTTP_POST, handleSetTimers);
  server.on("/sysinfo", HTTP_GET, handleSysInfo);
  server.on("/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "更新失败" : "更新成功!");
      delay(1000);
      ESP.restart();
    }, handleUpdateUpload);
  
  server.begin();
  Serial.println("[OK] HTTP 服务器已启动。\n========================================\n");
}

// ============== LOOP ==============
void loop() {
  server.handleClient();
  
  unsigned long static lastTimerCheck = 0;
  unsigned long static lastVoltageCheck = 0;

  if (millis() - lastVoltageCheck > 5000) {
    lastVoltageCheck = millis();
    checkVoltageProtection();
  }
  
  if (millis() - lastTimerCheck > 30000) {
    lastTimerCheck = millis();
    checkTimers();
  }
}