// ================== ESP32C3 Fan Controller with Web UI + OTA (Optimized v2.3 - Final) ==================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>

// ============== 用户配置 ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp32c3-fan-yys";  // Hostname + mDNS

// ============== 硬件引脚 ==============
const int PWM_PIN  = 5;
const int TACH_PIN = 4;

// ============== PWM/LEDC 配置 ==============
const int LEDC_FREQUENCY = 25000;
const int LEDC_RES_BITS  = 8;
const int PWM_MAX        = (1 << LEDC_RES_BITS) - 1; // 255
// *** PWM 逻辑反向开关: 如果你的风扇是“占空比越高，转速越慢”，请将这里改成 true ***
const bool PWM_INVERTED = false; 

// ============== RPM 采样配置 ==============
const int PULSES_PER_REV = 2;
const uint32_t MIN_PULSE_INTERVAL_US = 800;
const int MAX_REASONABLE_RPM = 15000;

WebServer server(80);

volatile uint32_t pulseCount = 0;
volatile uint32_t lastPulseMicros = 0;
uint32_t lastRpmCalcMs = 0;
int fanSliderValue = 0; // 存储滑块的当前位置 (0-255)
int lastRpm = 0;

// ============== 中断：测速脉冲计数（去毛刺） ==============
void IRAM_ATTR tachISR() {
  uint32_t now = micros();
  if (now - lastPulseMicros >= MIN_PULSE_INTERVAL_US) {
    pulseCount++;
    lastPulseMicros = now;
  }
}

// ============== RPM 计算 (增加读数稳定性) ==============
int computeRPM() {
  uint32_t now = millis();
  uint32_t elapsed = now - lastRpmCalcMs;
  if (elapsed < 1000) return -1;
  noInterrupts();
  uint32_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();
  lastRpmCalcMs = now;
  if (elapsed == 0 || PULSES_PER_REV == 0) return 0;
  uint32_t rpm = (uint32_t)((uint64_t)pulses * 60000ULL / (elapsed * PULSES_PER_REV));
  if (rpm > MAX_REASONABLE_RPM) { return lastRpm; }
  lastRpm = (int)rpm;
  return lastRpm;
}

// ============== 系统信息 JSON ==============
String getSystemInfoJSON() {
  String j = "{";
  j += "\"chip_model\":\"" + String(ESP.getChipModel()) + "\",";
  j += "\"chip_cores\":" + String(ESP.getChipCores()) + ",";
  j += "\"chip_rev\":" + String(ESP.getChipRevision()) + ",";
  j += "\"cpu_freq_mhz\":" + String(ESP.getCpuFreqMHz()) + ",";
  j += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"flash_size\":" + String(ESP.getFlashChipSize()) + ",";
  j += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
  j += "\"free_sketch_space\":" + String(ESP.getFreeSketchSpace()) + ",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += "}";
  return j;
}

// ============== 网页（自适应卡片式） ==============
const char MAIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32C3 风扇控制</title><style>:root{--bg:#0f172a;--card:#111827;--text:#e5e7eb;--accent:#22c55e;--muted:#94a3b8;}*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,'Helvetica Neue',Arial}.wrapper{min-height:100vh;background:linear-gradient(135deg,#0f172a,#1f2937);color:var(--text);display:flex;align-items:center;justify-content:center;padding:24px}.container{width:100%;max-width:560px}.card{background:linear-gradient(180deg,#0b1220,#0b1220) padding-box,linear-gradient(135deg,#22c55e33,#06b6d433) border-box;border:1px solid transparent;border-radius:16px;padding:20px;margin:14px 0;box-shadow:0 10px 30px rgba(0,0,0,.35)}h1,h2{margin:0 0 12px}h1{text-align:center;font-weight:700;font-size:22px}h2{font-size:18px;color:#d1d5db}.label{margin:8px 0 6px;font-weight:600}.value{font-feature-settings:'tnum' 1;letter-spacing:.3px}.slider{width:100%}small{color:var(--muted)}.btn{background:var(--accent);border:none;color:#052e13;padding:12px 18px;border-radius:10px;font-weight:700;cursor:pointer;box-shadow:0 6px 16px rgba(34,197,94,.35)}.btn:hover{filter:brightness(1.05)}input[type=file]{color:var(--text)}pre{white-space:pre-wrap;word-break:break-word}</style></head><body><div class="wrapper"><div class="container"><div class="card"><h1>ESP32C3 风扇控制面板</h1><div class="label">当前速度：<span id="spd" class="value">--</span></div><input id="speed" class="slider" type="range" min="0" max="255" value="0"/><div class="label" style="margin-top:12px">当前转速：<span id="rpm" class="value">--</span> RPM</div><small>提示：数值越大占空比越高，风扇转得更快。</small></div><div class="card"><h2>系统信息</h2><div id="sys">加载中...</div></div><div class="card"><h2>固件在线更新 OTA</h2><form method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz"/><br/><br/><button class="btn" type="submit">上传并更新</button></form><small>更新完成后设备会自动重启。</small></div></div></div><script>const spd=document.getElementById('spd'),rpm=document.getElementById('rpm'),slider=document.getElementById('speed'),sys=document.getElementById('sys');function setLabel(v){const p=Math.round(v/255*100);spd.textContent=v+' ('+p+'%)'}slider.addEventListener('input',()=>{const v=slider.value;setLabel(v);fetch('/setSpeed?value='+v).catch(()=>{})});window.addEventListener('load',()=>{fetch('/getSpeed').then(r=>r.text()).then(v=>{slider.value=v;setLabel(v)}).catch(()=>{})});setInterval(()=>{fetch('/getRPM').then(r=>r.text()).then(t=>{rpm.textContent=t.replace(/[^0-9-]/g,'')}).catch(()=>{});fetch('/sysinfo').then(r=>r.json()).then(info=>{sys.innerHTML=`芯片: ${info.chip_model} (rev ${info.chip_rev}) · 核心: ${info.chip_cores}<br>`+`CPU: ${info.cpu_freq_mhz} MHz · 空闲内存: ${info.free_heap} B<br>`+`Flash: ${info.flash_size} B · 固件: ${info.sketch_size} B<br>`+`可用固件空间: ${info.free_sketch_space} B<br>`+`IP: ${info.ip}`}).catch(()=>{})},1500);</script></body></html>
)HTML";

// ============== 路由处理 ==============
void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", MAIN_HTML);
}

void handleSetSpeed() {
  if (server.hasArg("value")) {
    fanSliderValue = server.arg("value").toInt();
    int fanDuty = fanSliderValue;
    if (PWM_INVERTED) {
      fanDuty = PWM_MAX - fanSliderValue;
    }
    ledcWrite(PWM_PIN, fanDuty);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleGetSpeed() {
  server.send(200, "text/plain", String(fanSliderValue));
}

void handleGetRPM() {
  int rpm = computeRPM();
  if (rpm < 0) server.send(200, "text/plain", "...");
  else         server.send(200, "text/plain", String(rpm));
}

void handleSysInfo() {
  server.send(200, "application/json", getSystemInfoJSON());
}

// ============== OTA 文件上传过程处理 ==============
void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  switch(upload.status) {
    case UPLOAD_FILE_START:
      Serial.printf("OTA Update Start: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
      break;
    case UPLOAD_FILE_WRITE:
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
      break;
    case UPLOAD_FILE_END:
      if (Update.end(true)) {
        Serial.printf("OTA 成功: %u 字节\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      break;
  }
  yield(); // 让 ESP32 处理其他任务，防止上传大文件时超时
}


// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP32C3 Fan Controller Booting Up (v2.3) =====");
  
  // 检查并打印 OTA 分区信息
  if (ESP.getFreeSketchSpace() == 0) {
    Serial.println("!!! 警告: 没有 OTA 更新分区，请在 Tools > Partition Scheme 中选择一个带 OTA 的方案 !!!");
  }

  // PWM 初始化
  ledcAttach(PWM_PIN, LEDC_FREQUENCY, LEDC_RES_BITS);
  int initialDuty = PWM_INVERTED ? PWM_MAX : 0;
  ledcWrite(PWM_PIN, initialDuty);

  // 转速输入
  pinMode(TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachISR, FALLING);

  // Wi-Fi 连接
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName);
  WiFi.begin(ssid, password);
  Serial.print("连接 Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400); Serial.print(".");
  }
  Serial.println("\n已连接!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // mDNS
  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local\n", deviceName);
  }

  // ---------------- Web 路由 ----------------
  server.on("/", HTTP_GET, handleRoot);
  server.on("/setSpeed", HTTP_GET, handleSetSpeed);
  server.on("/getSpeed", HTTP_GET, handleGetSpeed);
  server.on("/getRPM", HTTP_GET, handleGetRPM);
  server.on("/sysinfo", HTTP_GET, handleSysInfo);

  // OTA 更新路由
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html", MAIN_HTML); // 如果直接访问 /update，也显示主页
  });
  
  server.on("/update", HTTP_POST, 
    // 这是上传成功后的响应函数
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "更新失败" : "更新成功!");
      delay(1000);
      ESP.restart();
    },
    // 这是文件上传过程中的处理函数
    handleUpdateUpload
  );

  server.begin();
  Serial.println("HTTP 服务器已启动");
}

// ============== LOOP ==============
void loop() {
  server.handleClient();
}