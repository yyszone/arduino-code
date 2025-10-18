// ================== ESP8266 智能风扇控制器 v2.0-NAS-UI ==================
//
// 更新日志 (v2.0-NAS-UI):
// 1. [UI革新] Web 界面全面重构，采用专业的 NAS/服务器管理风格。
// 2. [视觉优化] 引入暗黑主题、SVG 图标和现代化的布局，提升用户体验。
// 3. [功能保持] 后端功能与 v1.9-NoLED 版本完全一致。
//
// 项目名称: AuraFan (NAS-UI Edition)
// =======================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>

// ==================================================================
// ==================== 用户配置 (在这里修改) =======================
// ==================================================================

// --- WiFi 设置 ---
const char* ssid = "yang1234";
const char* password = "y123456789";

// --- 设备名称 ---
const char* deviceName = "esp8266-fan";

// --- 风扇引脚 ---
const int PWM_PIN = 5;  // D1
const int TACH_PIN = 4; // D2

// ================== 配置结束, 以下代码无需修改 ==================


// --- PWM/RPM 派生配置 (自动) ---
const int PWM_FREQUENCY = 25000;
const int PWM_RESOLUTION = 1023;
const bool PWM_INVERTED = false;


// ====================== 全局变量 ======================
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

volatile int pulseCount = 0;
unsigned long lastRpmTime = 0;
int fanSliderValue = 5; // 初始速度设置为 10


// ====================== 中断及核心功能函数 ======================
void ICACHE_RAM_ATTR tachISR() {
  pulseCount++;
}

int computeRPM() {
  if (millis() == lastRpmTime) return 0;
  
  noInterrupts();
  int pulses = pulseCount;
  pulseCount = 0;
  interrupts();

  unsigned long elapsedTime = millis() - lastRpmTime;
  lastRpmTime = millis();
  
  int rpm = (int)((pulses / 2.0) * 60000.0 / elapsedTime);
  return rpm;
}

// ====================== 网页内容 (NAS 风格) ======================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP8266 Fan Controller</title><style>:root{--bg-color:#1a1d24;--panel-bg:#2c303a;--text-color:#e0e5f0;--text-muted:#8a93a2;--accent-color:#3498db;--accent-hover:#5dade2;--border-color:#3f4451}*,*:before,*:after{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;background-color:var(--bg-color);color:var(--text-color);display:flex;justify-content:center;align-items:center;min-height:100vh;padding:1rem}h1,h2{margin:0;font-weight:600}h1{font-size:1.5rem;text-align:center;margin-bottom:1.5rem;color:var(--text-color)}h2{font-size:1.1rem;display:flex;align-items:center;gap:.75rem;margin-bottom:1rem;padding-bottom:.75rem;border-bottom:1px solid var(--border-color)}.panel{width:100%;max-width:480px;background-color:var(--panel-bg);border-radius:12px;padding:1.5rem;box-shadow:0 10px 30px rgba(0,0,0,0.2);border:1px solid var(--border-color)}.panel-section{margin-bottom:2rem}.panel-section:last-child{margin-bottom:0}.stats-grid{display:grid;grid-template-columns:1fr 1fr;gap:1rem;margin-bottom:1.5rem}.stat-item{background-color:rgba(0,0,0,0.2);padding:1rem;border-radius:8px;text-align:center}.stat-item .label{display:block;font-size:.8rem;color:var(--text-muted);margin-bottom:.25rem}.stat-item .value{font-size:1.4rem;font-weight:600;font-feature-settings:"tnum" 1}#rpm .value{color:var(--accent-color)}.slider-container label{display:block;margin-bottom:.5rem;font-size:.9rem;color:var(--text-muted)}.slider{width:100%;-webkit-appearance:none;height:8px;background:var(--bg-color);border-radius:4px;outline:none}.slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:22px;height:22px;background:var(--accent-color);border-radius:50%;cursor:pointer;border:3px solid var(--panel-bg);transition:background-color .2s ease}.slider::-webkit-slider-thumb:hover{background-color:var(--accent-hover)}.system-info p{margin:0 0 .5rem}.system-info a{color:var(--accent-color);text-decoration:none;font-weight:500;transition:color .2s ease}.system-info a:hover{color:var(--accent-hover)}.update-link{display:flex;align-items:center;gap:.5rem}</style></head><body><div class="container"><div class="panel"><h1>ESP8266 风扇控制器</h1><div class="panel-section"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 12c2.76 0 5-2.24 5-5s-2.24-5-5-5-5 2.24-5 5 2.24 5 5 5z"/><path d="M19.78 14.78a2.5 2.5 0 0 0-3.53 0l-1.06 1.06a2.5 2.5 0 0 1-3.53 0l-1.06-1.06a2.5 2.5 0 0 0-3.53 0l-1.06 1.06a2.5 2.5 0 0 0 0 3.53l1.06 1.06a2.5 2.5 0 0 0 3.53 0l1.06-1.06a2.5 2.5 0 0 1 3.53 0l1.06 1.06a2.5 2.5 0 0 0 3.53 0l1.06-1.06a2.5 2.5 0 0 0 0-3.53l-1.06-1.06z"/></svg>风扇控制</h2><div class="stats-grid"><div class="stat-item" id="rpm"><span class="label">当前转速</span><span class="value">--</span></div><div class="stat-item" id="spd"><span class="label">设定速度</span><span class="value">--</span></div></div><div class="slider-container"><label for="fanSlider">调整风扇速度</label><input id="fanSlider" class="slider" type="range" min="0" max="255" value="0"></div></div><div class="panel-section"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="2" width="20" height="8" rx="2" ry="2"></rect><rect x="2" y="14" width="20" height="8" rx="2" ry="2"></rect><line x1="6" y1="6" x2="6.01" y2="6"></line><line x1="6" y1="18" x2="6.01" y2="18"></line></svg>系统信息</h2><div class="system-info"><p><strong>设备 IP:</strong> <span id="ipAddr">...</span></p><p><a href="/update" class="update-link"><svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>固件更新</a></p></div></div></div></div><script>const spdEl=document.querySelector('#spd .value'),rpmEl=document.querySelector('#rpm .value'),fanSlider=document.getElementById('fanSlider'),ipAddrEl=document.getElementById('ipAddr');function setFanLabel(v){const p=Math.round(v/255*100);spdEl.textContent=`${v} (${p}%)`}fanSlider.addEventListener('input',()=>{const v=fanSlider.value;setFanLabel(v);fetch(`/setSpeed?value=${v}`).catch(console.error)});window.addEventListener('load',()=>{fetch('/getState').then(r=>r.json()).then(s=>{fanSlider.value=s.fanSpeed;setFanLabel(s.fanSpeed);ipAddrEl.textContent=s.ip;}).catch(console.error)});setInterval(()=>{fetch('/getRPM').then(r=>r.text()).then(t=>{rpmEl.textContent=`${t} RPM`}).catch(console.error)},1500);</script></body></html>
)HTML";


// ====================== Web 路由处理 ======================
void handleRoot() { server.send(200, "text/html; charset=UTF-8", MAIN_HTML); }
void handleSetSpeed() {
  if (server.hasArg("value")) {
    fanSliderValue = server.arg("value").toInt();
    int dutyCycle = map(fanSliderValue, 0, 255, 0, PWM_RESOLUTION);
    if (PWM_INVERTED) dutyCycle = PWM_RESOLUTION - dutyCycle;
    analogWrite(PWM_PIN, dutyCycle);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}
void handleGetRPM() {
  server.send(200, "text/plain", String(computeRPM()));
}
void handleGetState() {
    String json = "{";
    json += "\"fanSpeed\":" + String(fanSliderValue) + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== AuraFan v2.0-NAS-UI =====");

  pinMode(PWM_PIN, OUTPUT);
  analogWriteFreq(PWM_FREQUENCY);

  int initialDutyCycle = map(fanSliderValue, 0, 255, 0, PWM_RESOLUTION);
  if (PWM_INVERTED) initialDutyCycle = PWM_RESOLUTION - initialDutyCycle;
  analogWrite(PWM_PIN, initialDutyCycle);
  
  pinMode(TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachISR, FALLING);
  lastRpmTime = millis();

  WiFi.hostname(deviceName);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400); Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());

  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS Responder started. Access at: http://%s.local\n", deviceName);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/setSpeed", HTTP_GET, handleSetSpeed);
  server.on("/getRPM", HTTP_GET, handleGetRPM);
  server.on("/getState", HTTP_GET, handleGetState);
  
  httpUpdater.setup(&server);
  server.begin();
  Serial.println("HTTP server started.");
}

// ====================== LOOP ======================
void loop() {
  server.handleClient();
  MDNS.update();
}