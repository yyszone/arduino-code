/**
 * 智能垃圾桶翻盖控制器 v2.1
 * ESP32 | 360°舵机 | HC-SR04 | WiFi | OTA | 日志
 * 依赖库：ESP32Servo
 */
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESP32Servo.h>
#include <EEPROM.h>
#include <esp_sleep.h>

// ─── 引脚 ────────────────────────────────────────────────
#define TRIG_PIN   4
#define ECHO_PIN   5
#define SERVO_PIN  6

const char* SSID = "yang1234";
const char* PASS = "y123456789";
const uint32_t WIFI_ALIVE = 60000;

// ─── 日志 ────────────────────────────────────────────────
#define LOG_CAP 50
struct LogEntry { uint32_t ts; char msg[72]; };
LogEntry logBuf[LOG_CAP];
uint8_t logHead = 0, logCount = 0;

void logf(const char *fmt, ...) {
  char buf[72]; va_list ap;
  va_start(ap, fmt); vsnprintf(buf, 72, fmt, ap); va_end(ap);
  logBuf[logHead] = { (uint32_t)millis(), {} };
  strncpy(logBuf[logHead].msg, buf, 71);
  logHead = (logHead + 1) % LOG_CAP;
  if (logCount < LOG_CAP) logCount++;
  Serial.println(buf);
}

// ─── 配置 ────────────────────────────────────────────────
#define MAGIC 0xC6
struct Config {
  uint8_t magic;
  int16_t trigDist, openSpeed, closeSpeed, stopVal;
  int16_t openTime, closeTime, holdTime;
} cfg;
const Config DEF = { MAGIC, 25, 10, 170, 90, 800, 800, 8000 };

void saveConfig() { EEPROM.put(0, cfg); EEPROM.commit(); logf("[CFG] 已保存"); }
void loadConfig() {
  EEPROM.get(0, cfg);
  if (cfg.magic != MAGIC) { cfg = DEF; saveConfig(); }
  logf("[CFG] trigDist=%d openSpd=%d", cfg.trigDist, cfg.openSpeed);
}

// ─── 状态机（enum 必须在 enterState 前声明）────────────
enum LidState : uint8_t { IDLE, OPENING, OPEN, CLOSING };
const char* STATE_STR[] = { "IDLE","OPENING","OPEN","CLOSING" };
LidState  lidState  = IDLE;
uint32_t  stateEnter = 0, trigCount = 0, lastPoll = 0;
Servo     myServo;

void enterState(uint8_t s) {
  lidState = (LidState)s; stateEnter = millis();
  logf("[LID] -> %s", STATE_STR[s]);
}

// ─── 传感器 ──────────────────────────────────────────────
float getDist() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 25000);
  return d ? d * 0.017f : 999.f;
}

// ─── Web 服务器 ──────────────────────────────────────────
WebServer server(80);
bool     wifiOn    = false;
uint32_t wifiStart = 0;
float    lastDist  = 999;

static const char HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>智能垃圾桶</title>
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
</style></head>
<body>
<h1>🗑️ 智能垃圾桶控制器 <small id="wb">WiFi ●</small></h1>

<div class="card">
  <div class="sec">📊 实时状态</div>
  <div class="row"><span class="rl">盖子状态</span><span id="ss" class="badge g">—</span></div>
  <div class="row"><span class="rl">传感器距离 <span style="font-size:.6rem;color:var(--mu)">(实测值)</span></span><span class="rv" id="sd">—</span></div>
  <div class="row"><span class="rl">触发次数</span><span class="rv" id="st">—</span></div>
  <div class="row"><span class="rl">运行时长</span><span class="rv" id="su">—</span></div>
  <div class="row"><span class="rl">WiFi剩余</span><span class="rv" id="sw">—</span></div>
</div>

<div class="card">
  <div class="sec">⚙️ 参数设置</div>
  <div class="field"><label>触发距离 (cm)</label><input type="number" id="f-trigDist" min="5" max="100"><div class="hint">手距离小于此值时开盖</div></div>
  <div class="g2">
    <div class="field"><label>开盖速度值</label><input type="number" id="f-openSpeed" min="0" max="89"><div class="hint">0-89，越小越快</div></div>
    <div class="field"><label>关盖速度值</label><input type="number" id="f-closeSpeed" min="91" max="180"><div class="hint">91-180，越大越快</div></div>
  </div>
  <div class="g2">
    <div class="field"><label>开盖时长 (ms)</label><input type="number" id="f-openTime" min="100" max="5000" step="50"></div>
    <div class="field"><label>关盖时长 (ms)</label><input type="number" id="f-closeTime" min="100" max="5000" step="50"></div>
  </div>
  <div class="field"><label>保持开启时长 (ms)</label><input type="number" id="f-holdTime" min="500" max="30000" step="500"></div>
  <button class="btn bp" onclick="save()">💾 保存设置</button>
  <button class="btn bg_" onclick="rst()">🔄 恢复默认</button>
</div>

<div class="card">
  <div class="sec">🚀 固件升级 OTA</div>
  <div class="drop" onclick="document.getElementById('ff').click()">
    📦 点击选择 .bin 固件文件<br><span id="fn" style="color:var(--acc);font-size:.72rem"></span>
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
var SM={IDLE:['g','● 空闲'],OPENING:['b','▶ 开盖中'],OPEN:['o','◉ 已开'],CLOSING:['b','◀ 关盖']};
var K=['trigDist','openSpeed','closeSpeed','openTime','closeTime','holdTime'];
function fmt(ms){var s=ms/1000|0,m=s/60|0,h=m/60|0;return h?h+'h'+(m%60)+'m'+(s%60)+'s':(m?m+'m'+(s%60)+'s':s+'s')}
function toast(t,c){var e=document.createElement('div');e.className='toast '+(c||'tok');e.textContent=t;document.body.appendChild(e);setTimeout(function(){e.remove()},2500)}
function $(id){return document.getElementById(id)}

/* ── 配置只在这里写入输入框，不在 loadStatus 里碰 ── */
function fillInputs(cfg){
  K.forEach(function(k){var e=$('f-'+k);if(e&&cfg[k]!=null)e.value=cfg[k]});
}

/* ── 状态刷新：只更新状态栏，绝不碰输入框 ── */
function loadStatus(){
  fetch('/api/status').then(function(r){return r.json()}).then(function(d){
    var sm=SM[d.state]||['g',d.state];
    $('ss').className='badge '+sm[0]; $('ss').textContent=sm[1];
    $('sd').textContent=d.dist.toFixed(1)+' cm';
    $('st').textContent=d.triggers+' 次';
    $('su').textContent=fmt(d.uptime);
    var wl=Math.max(0,60-Math.floor(d.uptime/1000));
    $('sw').textContent=d.wifiOn?wl+'s 后关闭':'已关闭';
    $('wb').style.opacity=d.wifiOn?1:.35;
  }).catch(function(){});
}

/* ── 单独拉取配置，写入输入框 ── */
function loadCfg(){
  fetch('/api/status').then(function(r){return r.json()}).then(function(d){
    fillInputs(d.cfg);
  }).catch(function(){});
}

/* ── 保存：POST 后重新拉配置确认 ── */
function save(){
  var p=new URLSearchParams();
  K.forEach(function(k){p.append(k,$('f-'+k).value)});
  fetch('/api/save',{method:'POST',body:p}).then(function(r){
    if(r.ok){toast('✅ 已保存');loadCfg();}
    else toast('❌ 保存失败','ter');
  });
}

function rst(){
  if(!confirm('恢复默认设置？'))return;
  fetch('/api/reset',{method:'POST'}).then(function(){
    toast('✅ 已恢复默认');loadCfg();
  });
}
function reboot(){if(!confirm('重启设备？'))return;fetch('/api/reboot',{method:'POST'});toast('🔄 重启中...')}
function pick(i){fw=i.files[0];$('fn').textContent=fw?fw.name+' ('+(fw.size/1024).toFixed(1)+'KB)':''}
function ota(){
  if(!fw){toast('⚠️ 请选择文件','ter');return}
  if(!confirm('刷入 '+fw.name+'？'))return;
  $('pb').style.display='block';
  var x=new XMLHttpRequest(); x.open('POST','/update',true);
  x.upload.onprogress=function(e){if(e.lengthComputable){var p=e.loaded/e.total*100|0;$('pf').style.width=p+'%';$('pl').textContent='上传 '+p+'%'}};
  x.onload=function(){$('pl').textContent=x.status===200?'✅ 成功，即将重启...':'❌ 失败'};
  var fd=new FormData();fd.append('update',fw);x.send(fd);
}
function loadLog(){
  fetch('/api/log').then(function(r){return r.json()}).then(function(d){
    $('log').innerHTML=[].concat(d.logs).reverse().map(function(l){
      var c=l.msg.indexOf('[ERR]')>=0?'e':(l.msg.indexOf('WARN')>=0?'w':'i');
      return '<div class="le"><span class="lt">'+fmt(l.ts)+'</span><span class="lm '+c+'">'+l.msg+'</span></div>';
    }).join('');
  }).catch(function(){});
}

/* 启动：先拉一次配置填入输入框，之后状态刷新永不碰输入框 */
loadCfg(); loadStatus(); loadLog();
setInterval(loadStatus,2500);
setInterval(loadLog,6000);
</script>
</body></html>)HTML";

String buildStatus() {
  String s = "{";
  s += "\"state\":\"" + String(STATE_STR[lidState]) + "\",";
  s += "\"dist\":"    + String(lastDist, 1) + ",";
  s += "\"triggers\":" + String(trigCount) + ",";
  s += "\"uptime\":"  + String(millis()) + ",";
  s += "\"wifiOn\":"  + String(wifiOn ? "true" : "false") + ",";
  s += "\"cfg\":{";
  s += "\"trigDist\":"   + String(cfg.trigDist)   + ",";
  s += "\"openSpeed\":"  + String(cfg.openSpeed)  + ",";
  s += "\"closeSpeed\":" + String(cfg.closeSpeed) + ",";
  s += "\"openTime\":"   + String(cfg.openTime)   + ",";
  s += "\"closeTime\":"  + String(cfg.closeTime)  + ",";
  s += "\"holdTime\":"   + String(cfg.holdTime);
  s += "}}";
  return s;
}

String buildLog() {
  String s = "{\"logs\":[";
  int start = (logCount < LOG_CAP) ? 0 : logHead;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_CAP;
    if (i) s += ",";
    s += "{\"ts\":" + String(logBuf[idx].ts) + ",\"msg\":\"";
    for (int j = 0; logBuf[idx].msg[j]; j++) {
      char c = logBuf[idx].msg[j];
      if (c == '"' || c == '\\') s += '\\';
      s += c;
    }
    s += "\"}";
  }
  return s + "]}";
}

void setupServer() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", HTML); });
  server.on("/api/status", HTTP_GET, []() {
    lastDist = getDist();
    server.send(200, "application/json", buildStatus());
  });
  server.on("/api/save", HTTP_POST, []() {
    auto gi = [&](const char *k, int16_t &f, int mn, int mx) {
      if (server.hasArg(k)) f = (int16_t)constrain(server.arg(k).toInt(), mn, mx);
    };
    gi("trigDist",   cfg.trigDist,   5,  100);
    gi("openSpeed",  cfg.openSpeed,  0,   89);
    gi("closeSpeed", cfg.closeSpeed, 91, 180);
    gi("openTime",   cfg.openTime,  100, 5000);
    gi("closeTime",  cfg.closeTime, 100, 5000);
    gi("holdTime",   cfg.holdTime,  500, 30000);
    saveConfig();
    server.send(200, "text/plain", "OK");
  });
  server.on("/api/reset",  HTTP_POST, []() { cfg = DEF; saveConfig(); server.send(200, "text/plain", "OK"); });
  server.on("/api/log",    HTTP_GET,  []() { server.send(200, "application/json", buildLog()); });
  server.on("/api/reboot", HTTP_POST, []() { server.send(200, "text/plain", "OK"); delay(300); ESP.restart(); });
  server.on("/update", HTTP_POST,
    []() {
      bool ok = !Update.hasError();
      server.send(ok ? 200 : 400, "text/plain", ok ? "OK" : Update.errorString());
      if (ok) { logf("[OTA] 成功，重启"); delay(500); ESP.restart(); }
    },
    []() {
      HTTPUpload &u = server.upload();
      if      (u.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); logf("[OTA] 开始: %s", u.filename.c_str()); }
      else if (u.status == UPLOAD_FILE_WRITE) { Update.write(u.buf, u.currentSize); }
      else if (u.status == UPLOAD_FILE_END)   { Update.end(true); logf("[OTA] 完成 %uB", u.totalSize); }
    }
  );
  server.begin();
  logf("[NET] HTTP 启动 %s", WiFi.localIP().toString().c_str());
}

// ─── setup ───────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  EEPROM.begin(64);
  loadConfig();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  ESP32PWM::allocateTimer(0);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(cfg.stopVal);
  logf("[SRV] 舵机就绪 stop=%d", cfg.stopVal);

  WiFi.begin(SSID, PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(300);
  if (WiFi.status() == WL_CONNECTED) {
    wifiOn = true; wifiStart = millis();
    setupServer();
  } else {
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    logf("[NET] WiFi 失败，跳过");
  }
}

// ─── loop ────────────────────────────────────────────────
void loop() {
  uint32_t now = millis();

  if (wifiOn) {
    server.handleClient();
    if (now - wifiStart >= WIFI_ALIVE) {
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
      wifiOn = false; logf("[NET] WiFi 已关闭");
    }
  }

  if (now - lastPoll < 150) {
    if (!wifiOn && lidState == IDLE) {
      esp_sleep_enable_timer_wakeup((150-(now-lastPoll)) * 1000ULL);
      esp_light_sleep_start();
    }
    return;
  }
  lastPoll = now;
  lastDist = getDist();

  switch (lidState) {
    case IDLE:
      if (lastDist < cfg.trigDist) {
        trigCount++;
        logf("[LID] 触发#%lu %.1fcm", trigCount, lastDist);
        myServo.write(cfg.openSpeed); enterState(OPENING);
      }
      break;
    case OPENING:
      if (now - stateEnter >= (uint32_t)cfg.openTime) {
        myServo.write(cfg.stopVal); enterState(OPEN);
      }
      break;
    case OPEN:
      if (now - stateEnter >= (uint32_t)cfg.holdTime) {
        myServo.write(cfg.closeSpeed); enterState(CLOSING);
      }
      break;
    case CLOSING:
      if (now - stateEnter >= (uint32_t)cfg.closeTime) {
        myServo.write(cfg.stopVal); enterState(IDLE);
      }
      break;
  }
}
