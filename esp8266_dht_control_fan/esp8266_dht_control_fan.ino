/*
 * ESP8266 智能控制台
 * 功能：DHT11 温度/湿度 + PIR 人体感应 + 继电器自动控制
 * 新增：Web OTA 固件升级 / EEPROM 参数持久化 / 美化界面
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <DHT.h>
#include <EEPROM.h>

// ===== WiFi 配置 =====
const char* ssid     = "yang1234";
const char* password = "y123456789";

// ===== 引脚配置 =====
#define DHTPIN    D5
#define DHTTYPE   DHT11
#define PIRPIN    D6
#define RELAYPIN  D7

DHT dht(DHTPIN, DHTTYPE);
ESP8266WebServer       server(80);
ESP8266HTTPUpdateServer httpUpdater;

// ===== EEPROM 配置 =====
#define EEPROM_SIZE  64
#define EEPROM_MAGIC 0xAB  // 魔数，用于判断 EEPROM 是否已写入过有效数据

struct Settings {
  uint8_t  magic;
  float    temperatureThreshold;  // 温度阈值 °C
  uint32_t detectionWindow;       // 检测窗口 ms
  uint32_t checkInterval;         // 检查间隔 ms
};

// ===== 控制变量（从 EEPROM 加载或使用默认值）=====
float         temperatureThreshold = 25.0;
unsigned long detectionWindow      = 30 * 1000UL;
unsigned long checkInterval        = 2000UL;

// ===== 状态变量 =====
unsigned long lastMotionTime = 0;
unsigned long lastCheckTime  = 0;
float         temperature    = 0.0;
float         humidity       = 0.0;
bool          relayState     = false;
bool          manualOverride = false;   // 手动控制标志

// ===== 去抖参数 =====
static bool          lastPirState   = LOW;
static unsigned long lastPirTrigger = 0;
const  unsigned long pirMinInterval = 1000;

// ============================================================
//  EEPROM 读写
// ============================================================
void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == EEPROM_MAGIC &&
      s.temperatureThreshold > -40 && s.temperatureThreshold < 100 &&
      s.detectionWindow >= 5000 && s.detectionWindow <= 3600000UL &&
      s.checkInterval   >= 500  && s.checkInterval   <= 60000UL) {
    temperatureThreshold = s.temperatureThreshold;
    detectionWindow      = s.detectionWindow;
    checkInterval        = s.checkInterval;
    Serial.println("[EEPROM] 配置已加载");
  } else {
    Serial.println("[EEPROM] 使用默认配置");
  }
}

void saveSettings() {
  Settings s;
  s.magic                = EEPROM_MAGIC;
  s.temperatureThreshold = temperatureThreshold;
  s.detectionWindow      = detectionWindow;
  s.checkInterval        = checkInterval;
  EEPROM.put(0, s);
  EEPROM.commit();
  Serial.println("[EEPROM] 配置已保存");
}

// ============================================================
//  辅助：生成公共 HTML 头
// ============================================================
String htmlHead(const String& title) {
  return F("<!DOCTYPE html><html lang='zh'>"
           "<head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>") + title + F("</title>"
           "<style>"
           "*{box-sizing:border-box;margin:0;padding:0}"
           "body{font-family:'Segoe UI',sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh}"
           ".hdr{background:linear-gradient(135deg,#1e40af,#7c3aed);padding:18px 20px;text-align:center}"
           ".hdr h1{font-size:1.7em;color:#fff}"
           ".hdr p{color:#bfdbfe;font-size:.88em;margin-top:3px}"
           ".nav{display:flex;justify-content:center;gap:8px;padding:12px;background:#1e293b;border-bottom:1px solid #334155}"
           ".nav a{color:#93c5fd;text-decoration:none;padding:6px 16px;border-radius:20px;font-size:.88em;border:1px solid #334155;transition:.2s}"
           ".nav a:hover,.nav a.act{background:#3b82f6;color:#fff;border-color:#3b82f6}"
           ".wrap{max-width:960px;margin:20px auto;padding:0 14px}"
           ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:14px;margin-bottom:18px}"
           ".card{background:#1e293b;border-radius:12px;padding:18px;border:1px solid #334155}"
           ".card .ico{font-size:1.9em;margin-bottom:6px}"
           ".card .lbl{font-size:.78em;color:#94a3b8;text-transform:uppercase;letter-spacing:.05em}"
           ".card .val{font-size:1.75em;font-weight:700;margin-top:3px}"
           ".g{color:#4ade80}.r{color:#f87171}.b{color:#60a5fa}.y{color:#facc15}"
           ".sec{background:#1e293b;border-radius:12px;padding:20px;margin-bottom:18px;border:1px solid #334155}"
           ".sec h2{font-size:1.05em;margin-bottom:14px;color:#93c5fd;border-bottom:1px solid #334155;padding-bottom:9px}"
           ".frow{display:flex;flex-wrap:wrap;gap:14px}"
           ".fg{flex:1;min-width:160px}"
           ".fg label{display:block;font-size:.82em;color:#94a3b8;margin-bottom:5px}"
           ".fg input{width:100%;background:#0f172a;border:1px solid #475569;border-radius:8px;padding:9px 11px;color:#e2e8f0;font-size:.95em;outline:none}"
           ".fg input:focus{border-color:#3b82f6}"
           ".btn{padding:9px 22px;border:none;border-radius:8px;font-size:.92em;cursor:pointer;transition:.2s;font-weight:600}"
           ".bp{background:#3b82f6;color:#fff}.bp:hover{background:#2563eb}"
           ".bs{background:#22c55e;color:#fff}.bs:hover{background:#16a34a}"
           ".bd{background:#ef4444;color:#fff}.bd:hover{background:#dc2626}"
           ".by{background:#d97706;color:#fff}.by:hover{background:#b45309}"
           ".brow{display:flex;gap:10px;margin-top:14px;flex-wrap:wrap}"
           ".dot{width:11px;height:11px;border-radius:50%;display:inline-block;margin-right:6px}"
           ".don{background:#4ade80;box-shadow:0 0 7px #4ade80}.dof{background:#f87171}"
           ".prg{display:none;background:#0f172a;border-radius:8px;margin-top:12px;overflow:hidden;height:8px}"
           ".prg-bar{height:8px;background:linear-gradient(90deg,#3b82f6,#7c3aed);width:0%;transition:width .3s}"
           ".msg{margin-top:8px;font-size:.88em;color:#94a3b8}"
           ".info-row{display:flex;flex-wrap:wrap;gap:18px}"
           ".info-item .k{font-size:.78em;color:#64748b}.info-item .v{margin-top:3px;font-size:.95em}"
           ".footer{text-align:center;color:#475569;font-size:.78em;padding:18px}"
           "@media(max-width:540px){.hdr h1{font-size:1.3em}}"
           "</style></head><body>");
}

// ============================================================
//  首页
// ============================================================
void handleRoot() {
  unsigned long now       = millis();
  unsigned long secsSince = (now - lastMotionTime) / 1000;
  bool motionActive = (now - lastMotionTime <= detectionWindow);

  String html = htmlHead("ESP8266 智能控制台");

  // 顶部导航
  html += F("<div class='hdr'><h1>🏠 ESP8266 智能控制台</h1>"
            "<p>实时监控 · 自动控制 · 远程管理</p></div>"
            "<div class='nav'>"
            "<a href='/' class='act'>📊 状态</a>"
            "<a href='/config'>⚙️ 参数配置</a>"
            "<a href='/ota'>📦 OTA 升级</a>"
            "<a href='/sysinfo'>🖥️ 系统信息</a>"
            "</div>");

  html += F("<div class='wrap'>");

  // ---- 状态卡片 ----
  html += F("<div class='grid'>");

  // 温度卡片
  html += F("<div class='card'><div class='ico'>🌡️</div><div class='lbl'>当前温度</div>"
            "<div class='val ");
  html += (temperature > temperatureThreshold) ? F("r'>") : F("b'>");
  html += String(temperature, 1);
  html += F(" °C</div><div style='font-size:.8em;color:#64748b;margin-top:3px'>阈值 ");
  html += String(temperatureThreshold, 1);
  html += F(" °C</div></div>");

  // 湿度卡片
  html += F("<div class='card'><div class='ico'>💧</div><div class='lbl'>当前湿度</div>"
            "<div class='val b'>");
  html += String(humidity, 1);
  html += F(" %</div></div>");

  // 人体感应卡片
  html += F("<div class='card'><div class='ico'>👤</div><div class='lbl'>人体感应</div>"
            "<div class='val ");
  html += motionActive ? F("g'>活跃") : F("r'>静止");
  html += F("</div><div style='font-size:.8em;color:#64748b;margin-top:3px'>");
  if (secsSince < 3600) {
    html += String(secsSince) + F(" 秒前触发");
  } else {
    html += F("长时间未触发");
  }
  html += F("</div></div>");

  // 继电器卡片
  html += F("<div class='card'><div class='ico'>⚡</div><div class='lbl'>继电器</div>"
            "<div style='display:flex;align-items:center;margin-top:6px'>"
            "<span class='dot ");
  html += relayState ? F("don'></span><span class='val g'>开启") : F("dof'></span><span class='val r'>关闭");
  html += F("</span></div>");
  if (manualOverride) {
    html += F("<div style='font-size:.78em;color:#d97706;margin-top:4px'>手动模式</div>");
  } else {
    html += F("<div style='font-size:.78em;color:#64748b;margin-top:4px'>自动模式</div>");
  }
  html += F("</div>");

  html += F("</div>"); // end .grid

  // ---- 快速操作 ----
  html += F("<div class='sec'><h2>🎮 快速操作</h2><div class='brow'>"
            "<button class='btn bs' onclick=\"fetch('/relay/on').then(()=>location.reload())\">✅ 强制开启</button>"
            "<button class='btn bd' onclick=\"fetch('/relay/off').then(()=>location.reload())\">⛔ 强制关闭</button>"
            "<button class='btn by' onclick=\"fetch('/relay/auto').then(()=>location.reload())\">🔄 恢复自动</button>"
            "<button class='btn bp' onclick='location.reload()'>🔃 刷新状态</button>"
            "</div></div>");

  // ---- 当前参数预览 ----
  html += F("<div class='sec'><h2>📋 当前参数</h2><div class='info-row'>"
            "<div class='info-item'><div class='k'>温度阈值</div><div class='v'>");
  html += String(temperatureThreshold, 1) + F(" °C</div></div>"
            "<div class='info-item'><div class='k'>检测窗口</div><div class='v'>");
  html += String(detectionWindow / 1000) + F(" 秒</div></div>"
            "<div class='info-item'><div class='k'>检查间隔</div><div class='v'>");
  html += String(checkInterval / 1000) + F(" 秒</div></div>"
            "<div class='info-item'><div class='k'>逻辑说明</div>"
            "<div class='v' style='font-size:.85em;color:#94a3b8'>"
            "人体活跃 且 温度超阈值 → 开继电器</div></div>"
            "</div></div>");

  html += F("</div>"); // end .wrap

  // 自动刷新脚本
  html += F("<div class='footer'>ESP8266 智能控制台 — 5秒自动刷新</div>"
            "<script>setTimeout(()=>location.reload(),5000)</script>"
            "</body></html>");

  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================
//  参数配置页
// ============================================================
void handleConfig() {
  String html = htmlHead("参数配置");
  html += F("<div class='hdr'><h1>🏠 ESP8266 智能控制台</h1>"
            "<p>实时监控 · 自动控制 · 远程管理</p></div>"
            "<div class='nav'>"
            "<a href='/'>📊 状态</a>"
            "<a href='/config' class='act'>⚙️ 参数配置</a>"
            "<a href='/ota'>📦 OTA 升级</a>"
            "<a href='/sysinfo'>🖥️ 系统信息</a>"
            "</div><div class='wrap'>"
            "<div class='sec'><h2>⚙️ 参数配置（保存后永久有效）</h2>"
            "<div class='frow'>"
            "<div class='fg'><label>🌡️ 温度阈值 (°C)</label>"
            "<input type='number' id='thr' step='0.5' min='-10' max='80' value='");
  html += String(temperatureThreshold, 1);
  html += F("'></div>"
            "<div class='fg'><label>⏱️ 检测窗口 (秒) — 触发后保持有效的时长</label>"
            "<input type='number' id='win' min='5' max='3600' value='");
  html += String(detectionWindow / 1000);
  html += F("'></div>"
            "<div class='fg'><label>🔁 检查间隔 (秒) — 逻辑刷新频率</label>"
            "<input type='number' id='itv' min='1' max='60' value='");
  html += String(checkInterval / 1000);
  html += F("'></div></div>"
            "<div class='brow'>"
            "<button class='btn bp' onclick='doSave()'>💾 保存到 EEPROM</button>"
            "<button class='btn bd' onclick='doReset()'>↩️ 恢复默认值</button>"
            "</div>"
            "<div id='msg' class='msg'></div></div></div>"
            "<div class='footer'>参数会写入 EEPROM，掉电不丢失</div>"
            "<script>"
            "function doSave(){"
            "  var t=document.getElementById('thr').value,"
            "      w=document.getElementById('win').value,"
            "      i=document.getElementById('itv').value;"
            "  fetch('/settings?threshold='+t+'&window='+w+'&interval='+i)"
            "    .then(r=>r.text()).then(m=>{"
            "      var el=document.getElementById('msg');"
            "      el.style.color='#4ade80';el.textContent='✅ '+m;"
            "    }).catch(()=>{"
            "      document.getElementById('msg').style.color='#f87171';"
            "      document.getElementById('msg').textContent='❌ 保存失败';"
            "    });"
            "}"
            "function doReset(){"
            "  if(!confirm('恢复默认值？'))return;"
            "  fetch('/settings?threshold=25&window=30&interval=2')"
            "    .then(r=>r.text()).then(()=>location.reload());"
            "}"
            "</script></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================
//  OTA 升级页
// ============================================================
void handleOtaPage() {
  String html = htmlHead("OTA 固件升级");
  html += F("<div class='hdr'><h1>🏠 ESP8266 智能控制台</h1>"
            "<p>实时监控 · 自动控制 · 远程管理</p></div>"
            "<div class='nav'>"
            "<a href='/'>📊 状态</a>"
            "<a href='/config'>⚙️ 参数配置</a>"
            "<a href='/ota' class='act'>📦 OTA 升级</a>"
            "<a href='/sysinfo'>🖥️ 系统信息</a>"
            "</div><div class='wrap'>"
            "<div class='sec'><h2>📦 Web OTA 固件升级</h2>"
            "<p style='color:#94a3b8;font-size:.9em;margin-bottom:14px'>"
            "选择从 Arduino IDE 编译导出的 <strong>.bin</strong> 文件，"
            "点击上传后请勿断电或关闭页面，升级完成后设备自动重启。</p>"
            "<div style='background:#0f172a;border:1px solid #334155;border-radius:8px;padding:14px;margin-bottom:14px'>"
            "<div style='color:#94a3b8;font-size:.82em;margin-bottom:8px'>📁 选择固件文件 (.bin)</div>"
            "<input type='file' id='fw' accept='.bin' style='color:#cbd5e1'>"
            "</div>"
            "<div class='brow'>"
            "<button class='btn bp' onclick='startOTA()'>🚀 开始升级</button>"
            "</div>"
            "<div class='prg' id='prg'><div class='prg-bar' id='pbar'></div></div>"
            "<div id='otaMsg' class='msg'></div></div>"
            "<div class='sec'>"
            "<h2>💡 操作提示</h2>"
            "<div style='color:#94a3b8;font-size:.88em;line-height:1.8'>"
            "1. 在 Arduino IDE 中选择 <em>项目 → 导出已编译的二进制文件</em>，得到 .bin 文件<br>"
            "2. 上传期间保持同一局域网连接，不要刷新或关闭页面<br>"
            "3. 进度条到 100% 后，设备将自动重启（约 5 秒）<br>"
            "4. 重启后页面将自动跳转回首页"
            "</div></div>"
            "</div>"
            "<div class='footer'>OTA 升级 — 确保固件正确再上传</div>"
            "<script>"
            "function startOTA(){"
            "  var f=document.getElementById('fw').files[0];"
            "  if(!f){alert('请先选择 .bin 固件文件！');return;}"
            "  var fd=new FormData();fd.append('firmware',f);"
            "  var prg=document.getElementById('prg'),"
            "      bar=document.getElementById('pbar'),"
            "      msg=document.getElementById('otaMsg');"
            "  prg.style.display='block';"
            "  msg.style.color='#94a3b8';"
            "  msg.textContent='⏳ 正在上传固件...';"
            "  var xhr=new XMLHttpRequest();"
            "  xhr.upload.onprogress=function(e){"
            "    if(e.lengthComputable){"
            "      var p=Math.round(e.loaded/e.total*100);"
            "      bar.style.width=p+'%';"
            "      msg.textContent='⏳ 上传中... '+p+'%';"
            "    }"
            "  };"
            "  xhr.onload=function(){"
            "    if(xhr.status===200){"
            "      bar.style.width='100%';"
            "      msg.style.color='#4ade80';"
            "      msg.textContent='✅ 升级成功！设备正在重启，5 秒后跳转首页...';"
            "      setTimeout(()=>location.href='/',6000);"
            "    }else{"
            "      msg.style.color='#f87171';"
            "      msg.textContent='❌ 升级失败：'+xhr.responseText;"
            "    }"
            "  };"
            "  xhr.onerror=function(){"
            "    msg.style.color='#f87171';"
            "    msg.textContent='❌ 网络错误，请检查连接后重试';"
            "  };"
            "  xhr.open('POST','/update');xhr.send(fd);"
            "}"
            "</script></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================
//  系统信息页
// ============================================================
void handleSysInfo() {
  unsigned long upSec = millis() / 1000;
  String html = htmlHead("系统信息");
  html += F("<div class='hdr'><h1>🏠 ESP8266 智能控制台</h1>"
            "<p>实时监控 · 自动控制 · 远程管理</p></div>"
            "<div class='nav'>"
            "<a href='/'>📊 状态</a>"
            "<a href='/config'>⚙️ 参数配置</a>"
            "<a href='/ota'>📦 OTA 升级</a>"
            "<a href='/sysinfo' class='act'>🖥️ 系统信息</a>"
            "</div><div class='wrap'>"
            "<div class='sec'><h2>🖥️ 系统信息</h2>"
            "<div class='info-row'>");

  // 逐项输出信息
  auto infoItem = [&](const String& k, const String& v) {
    html += F("<div class='info-item' style='min-width:200px'>"
              "<div class='k'>") + k + F("</div><div class='v'>") + v + F("</div></div>");
  };

  infoItem("📡 IP 地址",      WiFi.localIP().toString());
  infoItem("📶 WiFi 信号",     String(WiFi.RSSI()) + " dBm");
  infoItem("🔗 MAC 地址",      WiFi.macAddress());
  infoItem("⏱️ 运行时长",
           String(upSec/3600) + "h " + String((upSec%3600)/60) + "m " + String(upSec%60) + "s");
  infoItem("💾 剩余堆内存",    String(ESP.getFreeHeap()) + " Bytes");
  infoItem("⚙️ CPU 频率",      String(ESP.getCpuFreqMHz()) + " MHz");
  infoItem("📦 Flash 大小",    String(ESP.getFlashChipSize()/1024) + " KB");
  infoItem("🔖 固件大小",      String(ESP.getSketchSize()/1024) + " KB");
  infoItem("🆓 可用 Flash",    String(ESP.getFreeSketchSpace()/1024) + " KB");
  infoItem("🔄 重置原因",      ESP.getResetReason());

  html += F("</div></div>"
            "<div class='sec'><h2>🔧 设备操作</h2><div class='brow'>"
            "<button class='btn bd' onclick=\"if(confirm('确认重启设备？'))location='/reboot'\">🔄 重启设备</button>"
            "</div></div>"
            "</div>"
            "<div class='footer'>系统信息页面</div>"
            "</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================
//  API 路由处理
// ============================================================
void handleSettings() {
  bool changed = false;
  if (server.hasArg("threshold")) {
    float v = server.arg("threshold").toFloat();
    if (v >= -10 && v <= 80) { temperatureThreshold = v; changed = true; }
  }
  if (server.hasArg("window")) {
    unsigned long v = (unsigned long)server.arg("window").toInt() * 1000UL;
    if (v >= 5000 && v <= 3600000UL) { detectionWindow = v; changed = true; }
  }
  if (server.hasArg("interval")) {
    unsigned long v = (unsigned long)server.arg("interval").toInt() * 1000UL;
    if (v >= 500 && v <= 60000UL) { checkInterval = v; changed = true; }
  }
  if (changed) {
    saveSettings();
    server.send(200, "text/plain; charset=utf-8", "参数已保存到 EEPROM ✅");
  } else {
    server.send(400, "text/plain; charset=utf-8", "参数不合法或未变更");
  }
}

void handleRelayOn() {
  manualOverride = true;
  relayState = true;
  digitalWrite(RELAYPIN, HIGH);
  server.send(200, "text/plain", "ON");
}

void handleRelayOff() {
  manualOverride = true;
  relayState = false;
  digitalWrite(RELAYPIN, LOW);
  server.send(200, "text/plain", "OFF");
}

void handleRelayAuto() {
  manualOverride = false;
  server.send(200, "text/plain", "AUTO");
}

void handleReboot() {
  server.send(200, "text/html; charset=utf-8",
              "<meta charset='utf-8'><p>重启中，3 秒后跳转...</p>"
              "<script>setTimeout(()=>location.href='/',4000)</script>");
  delay(500);
  ESP.restart();
}

// ============================================================
//  JSON API（可供外部应用调用）
// ============================================================
void handleApi() {
  unsigned long now = millis();
  String json = "{";
  json += "\"temperature\":" + String(temperature, 1) + ",";
  json += "\"humidity\":"    + String(humidity, 1) + ",";
  json += "\"relay\":"       + String(relayState ? "true" : "false") + ",";
  json += "\"manual\":"      + String(manualOverride ? "true" : "false") + ",";
  json += "\"motion\":"      + String((now - lastMotionTime <= detectionWindow) ? "true" : "false") + ",";
  json += "\"motionAge\":"   + String((now - lastMotionTime) / 1000) + ",";
  json += "\"threshold\":"   + String(temperatureThreshold, 1) + ",";
  json += "\"window\":"      + String(detectionWindow / 1000) + ",";
  json += "\"interval\":"    + String(checkInterval / 1000) + ",";
  json += "\"uptime\":"      + String(now / 1000);
  json += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ============================================================
//  setup
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== ESP8266 智能控制台 启动 =====");

  // 加载配置
  loadSettings();

  // 硬件初始化
  dht.begin();
  pinMode(PIRPIN,   INPUT);
  pinMode(RELAYPIN, OUTPUT);
  digitalWrite(RELAYPIN, LOW);

  // 防止上电误触发
  lastMotionTime = millis() - detectionWindow - 1;

  // WiFi 连接
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("连接 WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 15000) { Serial.println("\n[WiFi] 超时，重启!"); ESP.restart(); }
  }
  Serial.println("\n[WiFi] 已连接，IP：" + WiFi.localIP().toString());

  // ---- ArduinoOTA（网络上传，兼容 Arduino IDE）----
  ArduinoOTA.setHostname("esp8266-smart");
  ArduinoOTA.setPassword("ota12345");   // ← IDE 提示输入密码时填这个
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] 开始 ArduinoOTA 升级...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] 完成，重启中");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] 进度: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] 错误[%u]\n", error);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] ArduinoOTA 准备就绪（密码：ota12345）");

  // 注册 Web OTA（POST /update）
  httpUpdater.setup(&server, "/update");

  // 注册页面路由
  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/config",  HTTP_GET,  handleConfig);
  server.on("/ota",     HTTP_GET,  handleOtaPage);
  server.on("/sysinfo", HTTP_GET,  handleSysInfo);

  // 注册 API 路由
  server.on("/settings",   HTTP_GET, handleSettings);
  server.on("/relay/on",   HTTP_GET, handleRelayOn);
  server.on("/relay/off",  HTTP_GET, handleRelayOff);
  server.on("/relay/auto", HTTP_GET, handleRelayAuto);
  server.on("/reboot",     HTTP_GET, handleReboot);
  server.on("/api",        HTTP_GET, handleApi);

  server.begin();
  Serial.println("[Web] 服务器已启动");
  Serial.println("===================================");
}

// ============================================================
//  loop
// ============================================================
void loop() {
  unsigned long now = millis();
  ArduinoOTA.handle();
  server.handleClient();

  // 非阻塞定时检查
  if (now - lastCheckTime < checkInterval) return;
  lastCheckTime = now;

  // —— PIR 上升沿 + 最小间隔去抖 ——
  bool pir = digitalRead(PIRPIN);
  if (pir && !lastPirState && (now - lastPirTrigger > pirMinInterval)) {
    lastMotionTime = now;
    lastPirTrigger = now;
    Serial.println("[PIR] 👤 有效人体触发");
  }
  lastPirState = pir;

  // —— 读取 DHT11 ——
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  else           Serial.println("[DHT] ⚠️ 温度读取失败");
  if (!isnan(h)) humidity = h;

  // —— 自动控制逻辑（手动模式下跳过）——
  if (!manualOverride) {
    bool shouldOn = (now - lastMotionTime <= detectionWindow)
                    && (temperature > temperatureThreshold);
    if (shouldOn != relayState) {
      relayState = shouldOn;
      digitalWrite(RELAYPIN, relayState ? HIGH : LOW);
      Serial.printf("[Relay] %s 继电器 (温度=%.1f°C, 运动=%s)\n",
                    relayState ? "✅ 打开" : "⛔ 关闭",
                    temperature,
                    (now - lastMotionTime <= detectionWindow) ? "活跃" : "静止");
    }
  }
}
