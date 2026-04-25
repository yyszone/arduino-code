/**
 * 智能垃圾桶 v4.1
 * ESP32-C3 Mini | SG90 | VL53L0X(TOF200C) | 深度睡眠 | OTA
 *
 * v4.1 修复：
 *  ① Wire.begin 显式传引脚（ESP32-C3 必须）
 *  ② quickMeasure 不再 Wire.end()，避免 I2C 二次冲突
 *  ③ tofInitialized 标志避免重复 tof.init()
 *  ④ 首次上电强制保持唤醒（STAY_AWAKE_MS），方便 WiFi 配置
 *  ⑤ 舵机 attach 后先等 200 ms 再写角度，防止上电抖动
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESP32Servo.h>
#include <EEPROM.h>
#include <Wire.h>
#include <VL53L0X.h>
#include <time.h>
#include "esp_sleep.h"

// ══════════════════════════════════════════════════════════
//  引脚
// ══════════════════════════════════════════════════════════
#define I2C_SDA_PIN  4
#define I2C_SCL_PIN  5
#define SERVO_PIN    6
#define TOF_INT_PIN  7      // 预留
#define TOF_SHUT_PIN 10

inline bool distValid(uint16_t d) {
  return d >= 30 && d < 2000;  // 30 → 2000mm
}

// ══════════════════════════════════════════════════════════
//  WiFi / 时间
// ══════════════════════════════════════════════════════════
const char*    SSID         = "yang1234";
const char*    PASS         = "y123456789";
const uint32_t WIFI_ALIVE   = 60000;   // WiFi 存活 ms
const uint32_t STAY_AWAKE_MS = 90000;  // 首次上电强制保持唤醒时长（含WiFi 60s）
#define CST_OFFSET 28800
bool ntpSynced = false;

// ══════════════════════════════════════════════════════════
//  RTC 内存（深睡后保留）
// ══════════════════════════════════════════════════════════
RTC_DATA_ATTR static uint32_t g_trigCount   = 0;
RTC_DATA_ATTR static bool     g_firstBoot   = true;  // 首次上电标志

// ══════════════════════════════════════════════════════════
//  日志
// ══════════════════════════════════════════════════════════
#define LOG_CAP 50
struct LogEntry { char ts[10]; char msg[62]; };
LogEntry logBuf[LOG_CAP];
uint8_t  logHead = 0, logCount = 0;

String nowStr() {
  if (ntpSynced) {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      char b[10];
      snprintf(b, sizeof(b), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
      return String(b);
    }
  }
  return "+" + String(millis() / 1000) + "s";
}

void logf(const char* fmt, ...) {
  char buf[62]; va_list ap;
  va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  String t = nowStr();
  strncpy(logBuf[logHead].ts,  t.c_str(), 9);  logBuf[logHead].ts[9]  = '\0';
  strncpy(logBuf[logHead].msg, buf,       61);  logBuf[logHead].msg[61] = '\0';
  logHead = (logHead + 1) % LOG_CAP;
  if (logCount < LOG_CAP) logCount++;
  Serial.printf("[%s] %s\n", t.c_str(), buf);
}

void syncNTP() {
  configTime(CST_OFFSET, 0, "ntp.aliyun.com", "pool.ntp.org");
  struct tm ti;
  if (getLocalTime(&ti, 5000)) {
    ntpSynced = true;
    logf("[NTP] %02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    logf("[NTP] 同步失败");
  }
}

// ══════════════════════════════════════════════════════════
//  配置（EEPROM）
// ══════════════════════════════════════════════════════════
#define MAGIC 0xD3   // v4.1
struct Config {
  uint8_t  magic;
  int16_t  trigDist;     // 触发距离下限 mm
  int16_t  trigDistMax;  // 触发距离上限 mm ← 新增
  int16_t  closeAngle;
  int16_t  openAngle;
  int16_t  openTime;
  int16_t  closeTime;
  int16_t  holdTime;
  int16_t  sleepMs;
} cfg;

inline bool inTrigRange(float d) {
  return d > (float)cfg.trigDist && d < (float)cfg.trigDistMax;
}

const Config DEF = { MAGIC, 100, 200, 0, 90, 600, 600, 8000, 500 };

void saveConfig() { EEPROM.put(0, cfg); EEPROM.commit(); logf("[CFG] 已保存"); }
void loadConfig() {
  EEPROM.get(0, cfg);
  if (cfg.magic != MAGIC) {
    cfg = DEF; saveConfig();
    logf("[CFG] 使用默认值");
  } else {
    logf("[CFG] dist=%dmm sleep=%dms", cfg.trigDist, cfg.sleepMs);
  }
}

// ══════════════════════════════════════════════════════════
//  全局对象 & 状态
// ══════════════════════════════════════════════════════════
Servo     myServo;
VL53L0X   tof;
WebServer server(80);

enum LidState : uint8_t { IDLE, OPENING, OPEN, CLOSING };
const char* STATE_STR[] = { "IDLE","OPENING","OPEN","CLOSING" };
LidState  lidState        = IDLE;
uint32_t  stateEnter      = 0;
uint32_t  lastPoll        = 0;
bool      wifiOn          = false;
uint32_t  wifiStart       = 0;
uint32_t  bootTime        = 0;     // 首次上电时间戳，用于强制保持唤醒
bool      forceAwake      = false; // 首次上电强制保持唤醒
float     lastDistMM      = 9999;
bool      tofInitialized  = false; // tof.init() 是否已执行
bool      tofContinuous   = false; // startContinuous 是否已执行

void enterState(uint8_t s) {
  lidState = (LidState)s; stateEnter = millis();
  logf("[LID] → %s", STATE_STR[s]);
}

// ══════════════════════════════════════════════════════════
//  TOF：扫描 I2C 地址（调试用）
// ══════════════════════════════════════════════════════════
void i2cScan() {
  logf("[I2C] 扫描...");
  bool found = false;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      logf("[I2C] 发现设备: 0x%02X", addr);
      found = true;
    }
  }
  if (!found) logf("[I2C] 未发现任何设备，检查接线");
}

// ══════════════════════════════════════════════════════════
//  TOF：上电唤醒快速单次测量
//  ★ 修复：不再调用 Wire.end()，保留总线供后续使用
// ══════════════════════════════════════════════════════════
uint16_t quickMeasure() {
  digitalWrite(TOF_SHUT_PIN, HIGH);
  delay(10);                             // 唤醒预热压缩到10ms
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);                 // ★ 提速到 400kHz (I2C Fast Mode)
  tof.setTimeout(500);
  if (!tof.init()) {
    logf("[TOF] quickMeasure init 失败");
    i2cScan();
    return 9999;
  }
  tofInitialized = true;
  tof.setMeasurementTimingBudget(20000); // ★ 测距时间压缩到 20ms，加快反应
  uint16_t d = tof.readRangeSingleMillimeters();
  if (tof.timeoutOccurred() || !distValid(d)) {
    return 9999;
  }
  return d;
}

// ══════════════════════════════════════════════════════════
//  TOF：切换到连续模式（完整启动后调用）
//  ★ 修复：若 quickMeasure 已 init，跳过 tof.init()
// ══════════════════════════════════════════════════════════
bool initTOFContinuous() {
  if (!tofInitialized) {
    // Wire 可能未 begin（首次上电路径）
    digitalWrite(TOF_SHUT_PIN, HIGH);
    delay(15);
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);
    tof.setTimeout(500);
    if (!tof.init()) {
      logf("[TOF] continuous init 失败");
      i2cScan();
      return false;
    }
    tofInitialized = true;
  }
  // 无论哪条路径，设置 budget 并启动连续
  tof.setMeasurementTimingBudget(20000);
  tof.startContinuous(0);
  tofContinuous = true;
  logf("[TOF] 连续模式就绪");
  return true;
}

// ══════════════════════════════════════════════════════════
//  深度睡眠
// ══════════════════════════════════════════════════════════
void goToSleep() {
  logf("[PWR] 深睡 %d ms", cfg.sleepMs);
  Serial.flush();
  delay(30);

  if (myServo.attached()) myServo.detach();

  if (tofContinuous) tof.stopContinuous();
  if (tofInitialized) { Wire.end(); }
  tofInitialized = false;
  tofContinuous  = false;
  digitalWrite(TOF_SHUT_PIN, LOW);   // 关断 TOF → ~5 µA

  if (wifiOn) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiOn = false;
  }

  esp_sleep_enable_timer_wakeup((uint64_t)cfg.sleepMs * 1000ULL);
  esp_deep_sleep_start();
}

// ══════════════════════════════════════════════════════════
//  WiFi 启动
// ══════════════════════════════════════════════════════════
void startWiFi() {
  logf("[NET] 连接 WiFi: %s", SSID);
  WiFi.begin(SSID, PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifiOn    = true;
    wifiStart = millis();
    logf("[NET] 已连接 IP: %s", WiFi.localIP().toString().c_str());
    syncNTP();
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    logf("[NET] WiFi 连接失败（SSID/密码正确？）");
  }
}

// ══════════════════════════════════════════════════════════
//  舵机初始化（带防抖）
// ══════════════════════════════════════════════════════════
void initServo(bool isTriggered) {
  ESP32PWM::allocateTimer(0);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  
  if (!isTriggered) {
    // 只有非触发唤醒（比如首次上电），才需要归位并等待
    delay(200);                        
    myServo.write(cfg.closeAngle);
    logf("[SRV] SG90 归位 → %d°", cfg.closeAngle);
    delay(400);                        
  } else {
    // 触发唤醒时，只给极短的通电稳定时间，立刻准备弹开
    delay(30); 
  }
}

// ══════════════════════════════════════════════════════════
//  JSON 构建
// ══════════════════════════════════════════════════════════
String buildStatus() {
  int32_t wLeft = 0;
  if (wifiOn) wLeft = max(0L, (long)(WIFI_ALIVE - (millis() - wifiStart)));
  String s = "{";
  s += "\"state\":\""  + String(STATE_STR[lidState]) + "\",";
  s += "\"dist\":"     + String((int)lastDistMM)     + ",";
  s += "\"triggers\":" + String(g_trigCount)          + ",";
  s += "\"uptime\":"   + String(millis())             + ",";
  s += "\"wifiOn\":"   + String(wifiOn ? "true" : "false") + ",";
  s += "\"wifiLeft\":" + String(wLeft)                + ",";
  s += "\"cfg\":{";
  s += "\"trigDist\":"   + String(cfg.trigDist)   + ",";   // 👈 把这行补回来
  s += "\"trigDistMax\":"+ String(cfg.trigDistMax)+ ",";
  s += "\"closeAngle\":" + String(cfg.closeAngle) + ",";
  s += "\"openAngle\":"  + String(cfg.openAngle)  + ",";
  s += "\"openTime\":"   + String(cfg.openTime)   + ",";
  s += "\"closeTime\":"  + String(cfg.closeTime)  + ",";
  s += "\"holdTime\":"   + String(cfg.holdTime)   + ",";
  s += "\"sleepMs\":"    + String(cfg.sleepMs);
  s += "}}";
  return s;
}

String buildLog() {
  String s = "{\"logs\":[";
  int start = (logCount < LOG_CAP) ? 0 : logHead;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_CAP;
    if (i) s += ",";
    s += "{\"ts\":\""; s += logBuf[idx].ts; s += "\",\"msg\":\"";
    for (int j = 0; logBuf[idx].msg[j]; j++) {
      char c = logBuf[idx].msg[j];
      if (c == '"' || c == '\\') s += '\\';
      s += c;
    }
    s += "\"}";
  }
  return s + "]}";
}

// ══════════════════════════════════════════════════════════
//  Web 界面（与 v4.0 相同，略作字段适配）
// ══════════════════════════════════════════════════════════
static const char HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>智能垃圾桶 v4.1</title>
<style>
:root{--bg:#0f1117;--card:#181c27;--bd:#252a3a;--acc:#00e5a0;--a2:#0095ff;--wn:#ff6b35;--tx:#dde1f0;--mu:#5a6070}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:-apple-system,'Segoe UI',sans-serif;padding:14px 12px 48px;max-width:480px;margin:auto}
h1{font-size:1.1rem;font-weight:700;padding:18px 0 12px;border-bottom:1px solid var(--bd);margin-bottom:14px;display:flex;align-items:center;gap:8px}
h1 small{font-size:.68rem;color:var(--mu);font-weight:400;margin-left:auto}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:16px;margin-bottom:12px;position:relative;overflow:hidden}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;background:linear-gradient(90deg,var(--acc),var(--a2));opacity:0;transition:.3s}
.card:hover::before{opacity:1}
.sec{font-size:.62rem;font-weight:700;letter-spacing:1.5px;text-transform:uppercase;color:var(--mu);margin-bottom:12px}
.row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid var(--bd)}
.row:last-child{border:none;padding-bottom:0}
.rl{font-size:.8rem;color:var(--mu)}.rv{font-size:.83rem;font-weight:600;font-family:'Courier New',monospace}
.badge{padding:3px 9px;border-radius:20px;font-size:.68rem;font-weight:600}
.g{background:rgba(0,229,160,.1);color:var(--acc);border:1px solid rgba(0,229,160,.25)}
.b{background:rgba(0,149,255,.1);color:var(--a2);border:1px solid rgba(0,149,255,.25)}
.o{background:rgba(255,107,53,.1);color:var(--wn);border:1px solid rgba(255,107,53,.25)}
.field{margin-bottom:11px}
label{display:block;font-size:.65rem;font-weight:700;letter-spacing:.7px;text-transform:uppercase;color:var(--mu);margin-bottom:5px}
input[type=number]{width:100%;background:#1a1f2e;border:1px solid var(--bd);color:var(--tx);border-radius:8px;padding:9px 12px;font-size:.92rem;outline:none;transition:.2s}
input[type=number]:focus{border-color:var(--acc);box-shadow:0 0 0 3px rgba(0,229,160,.1)}
.hint{font-size:.65rem;color:var(--mu);margin-top:4px}
.pwr{font-size:.65rem;color:var(--acc);margin-top:5px;padding:5px 9px;background:rgba(0,229,160,.06);border-radius:6px;border:1px solid rgba(0,229,160,.15)}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.btn{width:100%;padding:10px;border-radius:8px;font-weight:700;font-size:.86rem;cursor:pointer;border:none;margin-bottom:7px;transition:.15s}
.bp{background:linear-gradient(135deg,var(--acc),var(--a2));color:#0f1117}
.bg_{background:transparent;color:var(--mu);border:1px solid var(--bd)}
.bg_:hover{color:var(--tx);border-color:var(--mu)}
.bd_{background:rgba(255,107,53,.08);color:var(--wn);border:1px solid rgba(255,107,53,.2)}
.drop{border:2px dashed var(--bd);border-radius:8px;padding:18px;text-align:center;cursor:pointer;margin-bottom:11px;transition:.2s;font-size:.8rem;color:var(--mu)}
.drop:hover{border-color:var(--acc);background:rgba(0,229,160,.03)}
.pbar{height:5px;background:var(--bd);border-radius:3px;overflow:hidden;margin:8px 0 4px;display:none}
.pfill{height:100%;width:0;background:linear-gradient(90deg,var(--acc),var(--a2));transition:.25s}
.plbl{font-size:.7rem;color:var(--mu)}
.log{background:#090b10;border:1px solid var(--bd);border-radius:8px;padding:10px;height:200px;overflow-y:auto;font-family:'Courier New',monospace;font-size:.68rem;margin-bottom:10px}
.le{display:flex;gap:8px;padding:2px 0;border-bottom:1px solid #10131c}
.lt{color:var(--mu);flex-shrink:0}.lm{color:#b0bcd8}
.lm.i{color:var(--acc)}.lm.e{color:var(--wn)}.lm.w{color:#ffd166}
.toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);background:var(--card);border:1px solid var(--bd);border-radius:9px;padding:9px 16px;font-size:.8rem;box-shadow:0 8px 24px rgba(0,0,0,.5);animation:ti .25s;z-index:9}
@keyframes ti{from{opacity:0;transform:translate(-50%,7px)}to{opacity:1;transform:translateX(-50%)}}
.tok{border-color:rgba(0,229,160,.35);color:var(--acc)}.ter{border-color:rgba(255,107,53,.35);color:var(--wn)}
input[type=file]{display:none}
.angle-wrap{display:flex;align-items:center;gap:8px}
.angle-wrap input[type=range]{flex:1;accent-color:var(--acc)}
.angle-val{font-size:.8rem;font-family:'Courier New',monospace;color:var(--acc);min-width:36px;text-align:right}
</style></head>
<body>
<h1>🗑️ 智能垃圾桶 v4.1 <small id="wb">WiFi ●</small></h1>
<div class="card">
  <div class="sec">📊 实时状态</div>
  <div class="row"><span class="rl">盖子状态</span><span id="ss" class="badge g">—</span></div>
  <div class="row"><span class="rl">TOF 距离</span><span class="rv" id="sd">—</span></div>
  <div class="row"><span class="rl">触发次数</span><span class="rv" id="st">—</span></div>
  <div class="row"><span class="rl">运行时长</span><span class="rv" id="su">—</span></div>
  <div class="row"><span class="rl">WiFi 剩余</span><span class="rv" id="sw">—</span></div>
</div>
<div class="card">
  <div class="sec">⚙️ 参数设置</div>
  <div class="g2">
    <div class="field">
      <label>触发下限 (mm)</label>
      <input type="number" id="f-trigDist" min="30" max="1000">
    </div>
    <div class="field">
      <label>触发上限 (mm)</label>
      <input type="number" id="f-trigDistMax" min="50" max="2000">
    </div>
  </div>
  <div class="hint" style="margin-bottom:11px; margin-top:-6px;">(在此范围内触发开盖，例如: 下限100, 上限200)</div>
  <div class="field">
    <label>深睡间隔 (ms)</label>
    <input type="number" id="f-sleepMs" min="100" max="5000" step="100">
    <div class="pwr">⚡ 500ms≈12mA &nbsp;|&nbsp; 1000ms≈6mA &nbsp;|&nbsp; 2000ms≈3mA</div>
  </div>
  <div class="g2">
    <div class="field">
      <label>关盖角度 (°)</label>
      <div class="angle-wrap">
        <input type="range" id="r-closeAngle" min="0" max="180" oninput="sync('closeAngle',this.value)">
        <span class="angle-val" id="v-closeAngle">0°</span>
      </div>
      <input type="number" id="f-closeAngle" min="0" max="180" oninput="syncR('closeAngle',this.value)" style="margin-top:6px">
    </div>
    <div class="field">
      <label>开盖角度 (°)</label>
      <div class="angle-wrap">
        <input type="range" id="r-openAngle" min="0" max="180" oninput="sync('openAngle',this.value)">
        <span class="angle-val" id="v-openAngle">90°</span>
      </div>
      <input type="number" id="f-openAngle" min="0" max="180" oninput="syncR('openAngle',this.value)" style="margin-top:6px">
    </div>
  </div>
  <div class="g2">
    <div class="field">
      <label>开盖等待 (ms)</label>
      <input type="number" id="f-openTime" min="100" max="3000" step="50">
    </div>
    <div class="field">
      <label>关盖等待 (ms)</label>
      <input type="number" id="f-closeTime" min="100" max="3000" step="50">
    </div>
  </div>
  <div class="field">
    <label>保持开启时长 (ms)</label>
    <input type="number" id="f-holdTime" min="500" max="30000" step="500">
  </div>
  <button class="btn bp" onclick="save()">💾 保存设置</button>
  <button class="btn bg_" onclick="rst()">🔄 恢复默认</button>
</div>
<div class="card">
  <div class="sec">🚀 固件升级 OTA</div>
  <div class="drop" onclick="document.getElementById('ff').click()">
    📦 点击选择 .bin 固件<br><span id="fn" style="color:var(--acc);font-size:.72rem"></span>
  </div>
  <input type="file" id="ff" accept=".bin" onchange="pick(this)">
  <div class="pbar" id="pb"><div class="pfill" id="pf"></div></div>
  <div class="plbl" id="pl"></div>
  <button class="btn bp" onclick="ota()">⬆️ 开始升级</button>
  <button class="btn bd_" onclick="reboot()">⚡ 重启设备</button>
</div>
<div class="card">
  <div class="sec">📋 系统日志</div>
  <div class="log" id="log"></div>
  <button class="btn bg_" onclick="loadLog()">🔄 刷新日志</button>
</div>
<script>
var fw=null;
var SM={IDLE:['g','● 空闲'],OPENING:['b','▶ 开盖中'],OPEN:['o','◉ 已开'],CLOSING:['b','◀ 关盖中']};
var K=['trigDist','trigDistMax','sleepMs','closeAngle','openAngle','openTime','closeTime','holdTime'];
function fmt(ms){var s=ms/1000|0,m=s/60|0,h=m/60|0;return h?h+'h'+(m%60)+'m'+(s%60)+'s':(m?m+'m'+(s%60)+'s':s+'s')}
function toast(t,c){var e=document.createElement('div');e.className='toast '+(c||'tok');e.textContent=t;document.body.appendChild(e);setTimeout(function(){e.remove()},2500)}
function $(id){return document.getElementById(id)}
function sync(k,v){$('f-'+k).value=v;var vd=$('v-'+k);if(vd)vd.textContent=v+'°';}
function syncR(k,v){var r=$('r-'+k);if(r){r.value=v;var vd=$('v-'+k);if(vd)vd.textContent=v+'°';}}
function fillInputs(cfg){K.forEach(function(k){var e=$('f-'+k);if(e&&cfg[k]!=null){e.value=cfg[k];syncR(k,cfg[k]);}});}
function loadStatus(){
  fetch('/api/status').then(function(r){return r.json()}).then(function(d){
    var sm=SM[d.state]||['g',d.state];
    $('ss').className='badge '+sm[0];$('ss').textContent=sm[1];
    $('sd').textContent=d.dist+' mm';
    $('st').textContent=d.triggers+' 次';
    $('su').textContent=fmt(d.uptime);
    $('sw').textContent=d.wifiOn?Math.ceil(d.wifiLeft/1000)+'s 后关闭':'已关闭';
    $('wb').style.opacity=d.wifiOn?1:.35;
  }).catch(function(){});
}
function loadCfg(){fetch('/api/status').then(function(r){return r.json()}).then(function(d){fillInputs(d.cfg);}).catch(function(){});}
function save(){
  var p=new URLSearchParams();
  K.forEach(function(k){
    var e = $('f-'+k);
    if(e) p.append(k, e.value); // 增加判空保护，只有元素存在才读取 value
  });
  fetch('/api/save',{method:'POST',body:p}).then(function(r){

    if(r.ok){toast('✅ 已保存');loadCfg();}else toast('❌ 保存失败','ter');
  });
}
function rst(){if(!confirm('恢复默认？'))return;fetch('/api/reset',{method:'POST'}).then(function(){toast('✅ 已恢复');loadCfg();});}
function reboot(){if(!confirm('重启？'))return;fetch('/api/reboot',{method:'POST'});toast('🔄 重启中...')}
function pick(i){fw=i.files[0];$('fn').textContent=fw?fw.name+' ('+(fw.size/1024).toFixed(1)+'KB)':'';}
function ota(){
  if(!fw){toast('⚠️ 请先选择文件','ter');return;}
  if(!confirm('刷入 '+fw.name+'？'))return;
  $('pb').style.display='block';
  var x=new XMLHttpRequest();x.open('POST','/update',true);
  x.upload.onprogress=function(e){if(e.lengthComputable){var p=e.loaded/e.total*100|0;$('pf').style.width=p+'%';$('pl').textContent='上传 '+p+'%';}};
  x.onload=function(){$('pl').textContent=x.status===200?'✅ 成功，即将重启...':'❌ 失败';};
  var fd=new FormData();fd.append('update',fw);x.send(fd);
}
function loadLog(){
  fetch('/api/log').then(function(r){return r.json()}).then(function(d){
    $('log').innerHTML=[].concat(d.logs).reverse().map(function(l){
      var c=l.msg.indexOf('[ERR]')>=0?'e':(l.msg.indexOf('WARN')>=0?'w':'i');
      return '<div class="le"><span class="lt">'+l.ts+'</span><span class="lm '+c+'">'+l.msg+'</span></div>';
    }).join('');
  }).catch(function(){});
}
loadCfg();loadStatus();loadLog();
setInterval(loadStatus,2500);
setInterval(loadLog,6000);
</script>
</body></html>
)HTML";

// ══════════════════════════════════════════════════════════
//  Web 路由
// ══════════════════════════════════════════════════════════
void setupServer() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", HTML); });
  server.on("/api/status", HTTP_GET, []() {
    // 状态查询时顺便读一次 TOF
    if (tofContinuous) {
      uint16_t d = tof.readRangeContinuousMillimeters();
      if (!tof.timeoutOccurred()) lastDistMM = d;
    }
    server.send(200, "application/json", buildStatus());
  });
  server.on("/api/save", HTTP_POST, []() {
    auto gi = [&](const char* k, int16_t& f, int mn, int mx) {
      if (server.hasArg(k)) f = (int16_t)constrain(server.arg(k).toInt(), mn, mx);
    };
    gi("trigDistMax", cfg.trigDistMax, 50, 2000);
    gi("trigDist",   cfg.trigDist,   50,  1000);
    gi("closeAngle", cfg.closeAngle, 0,   180);
    gi("openAngle",  cfg.openAngle,  0,   180);
    gi("openTime",   cfg.openTime,   100, 3000);
    gi("closeTime",  cfg.closeTime,  100, 3000);
    gi("holdTime",   cfg.holdTime,   500, 30000);
    gi("sleepMs",    cfg.sleepMs,    100, 5000);
    saveConfig();
    server.send(200, "text/plain", "OK");
  });
  server.on("/api/reset", HTTP_POST, []() {
    cfg = DEF; saveConfig();
    server.send(200, "text/plain", "OK");
  });
  server.on("/api/log", HTTP_GET, []() {
    server.send(200, "application/json", buildLog());
  });
  server.on("/api/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "OK");
    delay(300); ESP.restart();
  });
  server.on("/update", HTTP_POST,
    []() {
      bool ok = !Update.hasError();
      server.send(ok ? 200 : 400, "text/plain", ok ? "OK" : Update.errorString());
      if (ok) { logf("[OTA] 成功，重启"); delay(500); ESP.restart(); }
    },
    []() {
      HTTPUpload& u = server.upload();
      if      (u.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      else if (u.status == UPLOAD_FILE_WRITE) { Update.write(u.buf, u.currentSize); }
      else if (u.status == UPLOAD_FILE_END)   { Update.end(true); logf("[OTA] 完成 %uB", u.totalSize); }
    }
  );
  server.begin();
  logf("[NET] HTTP 启动 http://%s", WiFi.localIP().toString().c_str());
}

// ══════════════════════════════════════════════════════════
//  setup
// ══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);  // 等串口稳定
  Serial.println("\n\n=== 智能垃圾桶 v4.1 启动 ===");

  EEPROM.begin(sizeof(Config) + 4);
  loadConfig();

  // XSHUT 默认低电平关断 TOF
  pinMode(TOF_SHUT_PIN, OUTPUT);
  digitalWrite(TOF_SHUT_PIN, LOW);
  pinMode(TOF_INT_PIN, INPUT_PULLUP);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  logf("[SYS] 唤醒原因: %d", (int)cause);

  bool isTriggered = false; // ★ 增加一个标志位

  // ── 定时器唤醒：快速测距 ──────────────────────────────
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    uint16_t d = quickMeasure();
    lastDistMM = d;

    if (!inTrigRange(d)) {
      // 无目标，关断 TOF，立即重入睡
      digitalWrite(TOF_SHUT_PIN, LOW);
      esp_sleep_enable_timer_wakeup((uint64_t)cfg.sleepMs * 1000ULL);
      esp_deep_sleep_start();
    }
    // 有目标，标记为已触发
    isTriggered = true;
    g_trigCount++;
    logf("[LID] 触发#%lu %d mm", g_trigCount, d);

  } else {
    // ── 首次上电 / 手动重启 ───────────────────────────
    logf("[SYS] 首次上电，保持唤醒 %lu s 供配置", STAY_AWAKE_MS / 1000);
    forceAwake = true;
    bootTime   = millis();
    g_firstBoot = false;
  }

  // ── 完整初始化 ────────────────────────────────────────
  initServo(isTriggered);  // ★ 把标志位传进去
  initTOFContinuous();
  
  // 如果是手触发的，就不要再去连 WiFi 了，直接跳过以节省时间
  if (!isTriggered) {
    startWiFi();
    if (wifiOn) setupServer();
  }

  // 检测到目标 → 立即开盖
  if (isTriggered) {
    myServo.write(cfg.openAngle);
    enterState(OPENING);
  } else {
    enterState(IDLE);
  }
}

// ══════════════════════════════════════════════════════════
//  loop
// ══════════════════════════════════════════════════════════
void loop() {
  uint32_t now = millis();

  // ── Web 服务器 & WiFi 超时 ────────────────────────────
  if (wifiOn) {
    server.handleClient();
    if (now - wifiStart >= WIFI_ALIVE) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      wifiOn = false;
      logf("[NET] WiFi 60s 到期");
    }
  }

  // ── 首次上电强制保持唤醒判断 ─────────────────────────
  if (forceAwake && (now - bootTime >= STAY_AWAKE_MS)) {
    forceAwake = false;
    logf("[SYS] 强制唤醒期结束，准备深睡");
  }

  // ── 限制轮询频率 ──────────────────────────────────────
  if (now - lastPoll < 150) return;
  lastPoll = now;

  // ── 读取 TOF ─────────────────────────────────────────
  if (tofContinuous) {
    uint16_t d = tof.readRangeContinuousMillimeters();
    if (!tof.timeoutOccurred() && distValid(d)) {
      lastDistMM = d;
    }
  }

  // ── 状态机 ───────────────────────────────────────────
  switch (lidState) {

    case IDLE:
      // 强制唤醒期 或 WiFi 开启期：保持运行，检测新触发
      if (!wifiOn && !forceAwake) {
        goToSleep();  // 永不返回
      }
      if (inTrigRange(lastDistMM))  {
        g_trigCount++;
        logf("[LID] 触发#%lu %d mm", g_trigCount, (int)lastDistMM);
        myServo.write(cfg.openAngle);
        enterState(OPENING);
      }
      break;

    case OPENING:
      if (now - stateEnter >= (uint32_t)cfg.openTime) enterState(OPEN);
      break;

    case OPEN:
    // 手仍在（距离 < trigDist）且读数有效 → 延长保持
    if (inTrigRange(lastDistMM)) {
      stateEnter = now;
    }
    if (now - stateEnter >= (uint32_t)cfg.holdTime) {
      myServo.write(cfg.closeAngle);
      enterState(CLOSING);
    }
    break;

    case CLOSING:
      if (now - stateEnter >= (uint32_t)cfg.closeTime) {
        enterState(IDLE);
        if (!wifiOn && !forceAwake) goToSleep();
      }
      break;
  }
}