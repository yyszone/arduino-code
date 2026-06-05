#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// ================== 参数配置 ==================
const char* ssid     = "yang1234";
const char* password = "y123456789";

// ---- 云端笔记 API ----
#define NOTE_URL  "https://note.yysresume.work/api/note-op"
#define NOTE_ID   "6dcc8bc1-072c-4ca9-bba0-8914df44cb46"
#define NOTE_AUTH "a_secret_fixed_token"

#define SENSOR_PIN A0
#define RELAY_PIN  5

#define PUMP_ON  HIGH
#define PUMP_OFF LOW

const int dryValue = 780;
const int wetValue = 380;
// ==============================================

// ================== EEPROM 布局 v2 ==================
// Addr 0   : MAGIC1  = 0xAB
// Addr 1   : MAGIC2  = 0xCD
// Addr 2   : VERSION = 0x02
// Addr 3   : moistureThreshold
// Addr 4   : wateringDuration
// Addr 5   : isAutoMode
// Addr 6~9 : totalWateringSeconds (4 字节 big-endian)
// =====================================================
#define EEPROM_SIZE 512
const byte EEPROM_MAGIC1  = 0xAB;
const byte EEPROM_MAGIC2  = 0xCD;
const byte EEPROM_VERSION = 0x02;
// 合理上限：1 年秒数；超出则视为脏数据归零
const unsigned long MAX_SANE_SECONDS = 365UL * 24UL * 3600UL;

// ---- 运行变量 ----
int  rawMoisture      = 0;
int  moisturePercent  = 0;
int  moistureThreshold = 40;
int  wateringDuration  = 10;
bool isAutoMode  = true;
bool pumpState   = false;

// ---- 统计变量 ----
unsigned long totalWateringSeconds = 0;

// ---- 自动浇水逻辑 ----
const unsigned long AUTO_CHECK_INTERVAL = 3600000UL;
unsigned long lastAutoCheckTime  = 0;
unsigned long autoPumpStartTime  = 0;
unsigned long lastCheckTime      = 0;
bool isPumpRunningAuto  = false;
bool firstAutoCheckDone = false;

// ---- 手动浇水计时 ----
unsigned long manualPumpStartTime = 0;

// ---- OTA 重启标志 ----
bool shouldReboot = false;
unsigned long rebootTime = 0;

ESP8266WebServer server(80);

// ==================== EEPROM ====================

void saveSettings() {
  EEPROM.write(0, EEPROM_MAGIC1);
  EEPROM.write(1, EEPROM_MAGIC2);
  EEPROM.write(2, EEPROM_VERSION);
  EEPROM.write(3, (byte)moistureThreshold);
  EEPROM.write(4, (byte)wateringDuration);
  EEPROM.write(5, (byte)(isAutoMode ? 1 : 0));
  EEPROM.write(6, (byte)((totalWateringSeconds >> 24) & 0xFF));
  EEPROM.write(7, (byte)((totalWateringSeconds >> 16) & 0xFF));
  EEPROM.write(8, (byte)((totalWateringSeconds >> 8)  & 0xFF));
  EEPROM.write(9, (byte)(totalWateringSeconds & 0xFF));
  EEPROM.commit();
  Serial.println("[EEPROM] Settings saved.");
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  bool valid = (EEPROM.read(0) == EEPROM_MAGIC1) &&
               (EEPROM.read(1) == EEPROM_MAGIC2) &&
               (EEPROM.read(2) == EEPROM_VERSION);
  if (valid) {
    moistureThreshold = EEPROM.read(3);
    wateringDuration  = EEPROM.read(4);
    isAutoMode        = (EEPROM.read(5) == 1);
    totalWateringSeconds =
        ((unsigned long)EEPROM.read(6) << 24) |
        ((unsigned long)EEPROM.read(7) << 16) |
        ((unsigned long)EEPROM.read(8) << 8)  |
        ((unsigned long)EEPROM.read(9));

    // ★ 越界保护：脏数据直接归零
    if (totalWateringSeconds > MAX_SANE_SECONDS) {
      Serial.printf("[EEPROM] totalWateringSeconds=%lu INVALID, reset to 0\n", totalWateringSeconds);
      totalWateringSeconds = 0;
      saveSettings();
    }

    // 参数范围保护
    if (moistureThreshold < 5  || moistureThreshold > 95) moistureThreshold = 40;
    if (wateringDuration  < 3  || wateringDuration  > 120) wateringDuration  = 10;

    Serial.printf("[EEPROM] Loaded: threshold=%d%% dur=%ds auto=%s total=%lus\n",
      moistureThreshold, wateringDuration, isAutoMode ? "ON" : "OFF", totalWateringSeconds);
  } else {
    Serial.println("[EEPROM] No valid config (magic/version mismatch). Writing defaults.");
    moistureThreshold    = 40;
    wateringDuration     = 10;
    isAutoMode           = true;
    totalWateringSeconds = 0;
    saveSettings();
  }
}

// ==================== 日志工具 ====================

void buildLogRowText(String eventName, char* outBuf, size_t bufSize) {
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  char timeStr[30];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", ti);

  String effectStr;
  if (eventName == "开机") {
    effectStr = isAutoMode ? "自动(阀值:" + String(moistureThreshold) + "%)" : "手动模式";
  } else if (eventName == "自动浇水") {
    effectStr = "自动(检测:" + String(moisturePercent) + "%<" + String(moistureThreshold) + "%)";
  } else if (eventName == "手动浇水") {
    effectStr = "手动控制";
  } else {
    effectStr = isAutoMode ? "自动模式" : "手动模式";
  }

  snprintf(outBuf, bufSize, "| %s | %s | %s | %s | %lus |",
           timeStr, eventName.c_str(),
           WiFi.localIP().toString().c_str(),
           effectStr.c_str(), totalWateringSeconds);
}
String pendingLogEvent = "";
String writeCloudLog(String eventName) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 1024);

  HTTPClient http;
  String statusResult = "未发送";

  if (http.begin(client, NOTE_URL)) {
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(8000);

    char logRow[160];
    buildLogRowText(eventName, logRow, sizeof(logRow));

    time_t utcNow = time(nullptr);
    struct tm utcTm;
    gmtime_r(&utcNow, &utcTm);
    char updatedAt[25];
    snprintf(updatedAt, sizeof(updatedAt), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             utcTm.tm_year + 1900, utcTm.tm_mon + 1, utcTm.tm_mday,
             utcTm.tm_hour, utcTm.tm_min, utcTm.tm_sec);

    char payload[450];
    snprintf(payload, sizeof(payload),
      "{\"noteId\":\"%s\",\"appendText\":\"%s\",\"updatedAt\":\"%s\"}",
      NOTE_ID, logRow, updatedAt);

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Cookie", String("auth_token=") + NOTE_AUTH);

    int httpCode = http.POST(payload);
    if (httpCode > 0) {
      statusResult = (httpCode == 200 || httpCode == 201) ? "成功" : "失败(HTTP " + String(httpCode) + ")";
    } else {
      statusResult = "失败(" + http.errorToString(httpCode) + ")";
    }
    http.end();
  } else {
    statusResult = "连接失败";
  }
  return statusResult;
}

void appendLocalLog(String eventName, String cloudStatus) {
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  char timeStr[30];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", ti);

  String effectStr;
  if (eventName == "开机") {
    effectStr = isAutoMode ? "自动(阀值:" + String(moistureThreshold) + "%)" : "手动模式";
  } else if (eventName == "自动浇水") {
    effectStr = "自动(检测:" + String(moisturePercent) + "%<" + String(moistureThreshold) + "%)";
  } else if (eventName == "手动浇水") {
    effectStr = "手动控制";
  } else {
    effectStr = isAutoMode ? "自动模式" : "手动模式";
  }

  File f = LittleFS.open("/log.txt", "a");
  if (f) {
    f.printf("%s\t%s\t%s\t%s\t%lus\t%s\n",
             timeStr, eventName.c_str(),
             WiFi.localIP().toString().c_str(),
             effectStr.c_str(), totalWateringSeconds,
             cloudStatus.c_str());
    f.close();
  } else {
    Serial.println("[LocalLog] Failed to open /log.txt");
  }
}

void logEvent(String eventName) {
  String cloudStatus = writeCloudLog(eventName);
  appendLocalLog(eventName, cloudStatus);
}

void initLocalLog() {
  if (!LittleFS.begin()) {
    Serial.println("[LittleFS] Mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
  if (!LittleFS.exists("/log.txt")) {
    File f = LittleFS.open("/log.txt", "w");
    if (f) {
      f.println("时间\t事件\tIP\t效果\t累计浇水\t云端状态");
      f.close();
    }
  }
}

// ==================== 网页 HTML ====================

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>智能植物灌溉</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=DM+Sans:wght@300;400;500;600&family=DM+Mono:wght@400;500&display=swap" rel="stylesheet">
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

    :root {
      --bg:        #0d1a12;
      --surface:   #13221a;
      --surface2:  #192b20;
      --border:    #243b2c;
      --green:     #4ade80;
      --green-dim: #22543d;
      --green-glow:#4ade8033;
      --blue:      #60a5fa;
      --amber:     #fbbf24;
      --red:       #f87171;
      --text:      #e2f0e8;
      --text-muted:#6b8f76;
      --font:      'DM Sans', sans-serif;
      --mono:      'DM Mono', monospace;
    }

    body {
      font-family: var(--font);
      background: var(--bg);
      color: var(--text);
      min-height: 100vh;
      display: flex;
      justify-content: center;
      align-items: flex-start;
      padding: 24px 16px 48px;
    }

    /* 背景纹理 */
    body::before {
      content: '';
      position: fixed;
      inset: 0;
      background-image:
        radial-gradient(circle at 20% 20%, #1a3a2488 0%, transparent 50%),
        radial-gradient(circle at 80% 80%, #0d2a1888 0%, transparent 50%);
      pointer-events: none;
      z-index: 0;
    }

    .app {
      width: 100%;
      max-width: 440px;
      position: relative;
      z-index: 1;
    }

    /* ---- 顶栏 ---- */
    .header {
      display: flex;
      align-items: center;
      gap: 12px;
      margin-bottom: 28px;
      padding: 0 4px;
    }
    .header-icon {
      width: 44px; height: 44px;
      background: var(--green-dim);
      border-radius: 12px;
      display: flex; align-items: center; justify-content: center;
      font-size: 22px;
      border: 1px solid #2d5a3d;
    }
    .header-title { font-size: 20px; font-weight: 600; letter-spacing: -0.3px; }
    .header-sub   { font-size: 12px; color: var(--text-muted); font-family: var(--mono); margin-top: 2px; }
    .online-dot {
      margin-left: auto;
      width: 8px; height: 8px;
      background: var(--green);
      border-radius: 50%;
      box-shadow: 0 0 8px var(--green);
      animation: pulse 2s infinite;
    }
    @keyframes pulse {
      0%,100% { opacity: 1; }
      50%      { opacity: 0.4; }
    }

    /* ---- 卡片基础 ---- */
    .card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 18px;
      padding: 20px;
      margin-bottom: 14px;
    }
    .card-title {
      font-size: 11px;
      font-weight: 500;
      letter-spacing: 1.2px;
      text-transform: uppercase;
      color: var(--text-muted);
      margin-bottom: 16px;
    }

    /* ---- 湿度大圆环 ---- */
    .moisture-card {
      display: flex;
      align-items: center;
      gap: 24px;
    }
    .ring-wrap {
      position: relative;
      flex-shrink: 0;
    }
    .ring-svg { display: block; }
    .ring-bg   { fill: none; stroke: var(--surface2); stroke-width: 10; }
    .ring-fill {
      fill: none;
      stroke: var(--green);
      stroke-width: 10;
      stroke-linecap: round;
      stroke-dasharray: 283;
      stroke-dashoffset: 283;
      transform: rotate(-90deg);
      transform-origin: center;
      filter: drop-shadow(0 0 6px var(--green));
      transition: stroke-dashoffset 0.8s cubic-bezier(.4,0,.2,1), stroke 0.4s;
    }
    .ring-center {
      position: absolute;
      inset: 0;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
    }
    .ring-val  { font-size: 30px; font-weight: 600; line-height: 1; }
    .ring-unit { font-size: 11px; color: var(--text-muted); margin-top: 3px; }

    .moisture-info { flex: 1; }
    .info-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 8px 0;
      border-bottom: 1px solid var(--border);
    }
    .info-row:last-child { border-bottom: none; }
    .info-label { font-size: 12px; color: var(--text-muted); }
    .info-value { font-size: 14px; font-weight: 500; font-family: var(--mono); }

    /* ---- 状态徽章 ---- */
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 5px;
      padding: 3px 10px;
      border-radius: 99px;
      font-size: 12px;
      font-weight: 500;
    }
    .badge::before {
      content: '';
      width: 6px; height: 6px;
      border-radius: 50%;
    }
    .badge-green  { background: #0f3320; color: var(--green);  border: 1px solid #1a4a2c; }
    .badge-green::before  { background: var(--green); box-shadow: 0 0 4px var(--green); }
    .badge-muted  { background: var(--surface2); color: var(--text-muted); border: 1px solid var(--border); }
    .badge-muted::before  { background: var(--text-muted); }
    .badge-blue   { background: #0c1f3a; color: var(--blue);  border: 1px solid #1a3a5c; }
    .badge-blue::before   { background: var(--blue); }
    .badge-amber  { background: #2a1f08; color: var(--amber); border: 1px solid #4a3a18; }
    .badge-amber::before  { background: var(--amber); }

    /* ---- 模式/控制行 ---- */
    .ctrl-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      min-height: 44px;
    }
    .ctrl-label { font-size: 14px; font-weight: 500; }
    .ctrl-sub   { font-size: 11px; color: var(--text-muted); margin-top: 2px; }

    /* ---- 按钮 ---- */
    .btn {
      border: none;
      outline: none;
      cursor: pointer;
      font-family: var(--font);
      font-weight: 500;
      border-radius: 10px;
      transition: all .18s ease;
      white-space: nowrap;
    }
    .btn:active { transform: scale(0.96); }

    .btn-sm  { padding: 7px 14px; font-size: 13px; }
    .btn-md  { padding: 10px 20px; font-size: 14px; }
    .btn-full{ width: 100%; padding: 12px; font-size: 14px; }

    .btn-ghost {
      background: var(--surface2);
      color: var(--text);
      border: 1px solid var(--border);
    }
    .btn-ghost:hover { background: var(--border); }

    .btn-primary {
      background: var(--green);
      color: #0a1a0f;
      box-shadow: 0 0 16px var(--green-glow);
    }
    .btn-primary:hover { background: #6ee799; }

    .btn-danger {
      background: transparent;
      color: var(--red);
      border: 1px solid #3a1f1f;
    }
    .btn-danger:hover { background: #2a1515; }

    .btn-blue {
      background: #1a3a6a;
      color: var(--blue);
      border: 1px solid #1e4a80;
    }
    .btn-blue:hover { background: #1e4a88; }

    /* ---- 泵开关大按钮 ---- */
    .pump-btn-wrap {
      margin-top: 14px;
    }
    #pump_btn {
      background: linear-gradient(135deg, #1a4a2c, #12321e);
      color: var(--green);
      border: 1px solid #2d6040;
      font-size: 15px;
      font-weight: 600;
      letter-spacing: 0.3px;
      padding: 13px;
    }
    #pump_btn.active {
      background: linear-gradient(135deg, #3a0f0f, #2a0a0a);
      color: var(--red);
      border-color: #6a2020;
      box-shadow: 0 0 16px #f8717133;
    }

    /* ---- 滑块 ---- */
    .slider-group { margin-top: 12px; }
    .slider-head {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      margin-bottom: 10px;
    }
    .slider-head-label { font-size: 14px; font-weight: 500; }
    .slider-head-val   { font-size: 18px; font-weight: 600; color: var(--green); font-family: var(--mono); }

    input[type=range] {
      -webkit-appearance: none;
      appearance: none;
      width: 100%;
      height: 4px;
      background: var(--border);
      border-radius: 2px;
      outline: none;
    }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 20px; height: 20px;
      border-radius: 50%;
      background: var(--green);
      cursor: pointer;
      box-shadow: 0 0 8px var(--green-glow);
      border: 2px solid #0d1a12;
      transition: transform .15s;
    }
    input[type=range]::-webkit-slider-thumb:active { transform: scale(1.2); }
    .slider-hint { font-size: 11px; color: var(--text-muted); margin-top: 8px; text-align: right; font-family: var(--mono); }

    /* ---- 统计条 ---- */
    .stat-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }
    .stat-cell {
      background: var(--surface2);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 14px;
    }
    .stat-cell-label { font-size: 11px; color: var(--text-muted); margin-bottom: 6px; }
    .stat-cell-val   { font-size: 20px; font-weight: 600; font-family: var(--mono); color: var(--green); }
    .stat-cell-unit  { font-size: 11px; color: var(--text-muted); margin-top: 1px; }

    /* ---- 日志表格 ---- */
    .log-scroll {
      max-height: 240px;
      overflow: auto;
      margin-top: 12px;
      border-radius: 10px;
      border: 1px solid var(--border);
    }
    .log-scroll::-webkit-scrollbar { width: 4px; height: 4px; }
    .log-scroll::-webkit-scrollbar-track { background: transparent; }
    .log-scroll::-webkit-scrollbar-thumb { background: var(--border); border-radius: 2px; }

    table { width: 100%; border-collapse: collapse; font-size: 11px; white-space: nowrap; }
    thead th {
      position: sticky; top: 0;
      background: #0f1e16;
      color: var(--text-muted);
      font-weight: 500;
      padding: 8px 10px;
      text-align: left;
      border-bottom: 1px solid var(--border);
      letter-spacing: 0.5px;
    }
    tbody tr { border-bottom: 1px solid #1a2b20; transition: background .15s; }
    tbody tr:hover { background: var(--surface2); }
    tbody td { padding: 8px 10px; color: var(--text); font-family: var(--mono); font-size: 11px; }
    .log-empty { text-align: center; color: var(--text-muted); padding: 24px; font-size: 13px; }
    .log-ok   { color: var(--green); }
    .log-fail { color: var(--red); }

    /* ---- OTA ---- */
    .ota-area { margin-top: 14px; display: flex; flex-direction: column; gap: 10px; }
    .ota-filename { font-size: 12px; color: var(--text-muted); text-align: center; font-family: var(--mono); }

    .progress-wrap { display: none; }
    .progress-meta {
      display: flex;
      justify-content: space-between;
      font-size: 12px;
      color: var(--text-muted);
      margin-bottom: 6px;
      font-family: var(--mono);
    }
    .progress-track {
      width: 100%; height: 6px;
      background: var(--surface2);
      border-radius: 3px;
      overflow: hidden;
    }
    .progress-fill {
      height: 100%;
      width: 0%;
      background: var(--green);
      border-radius: 3px;
      box-shadow: 0 0 8px var(--green-glow);
      transition: width .1s ease;
    }

    /* ---- 分隔标题 ---- */
    .section-divider {
      display: flex;
      align-items: center;
      gap: 10px;
      margin: 6px 0 14px;
      color: var(--text-muted);
      font-size: 11px;
      letter-spacing: 1px;
      text-transform: uppercase;
    }
    .section-divider::before,
    .section-divider::after {
      content: '';
      flex: 1;
      height: 1px;
      background: var(--border);
    }

    /* 动画 */
    @keyframes fadeUp {
      from { opacity: 0; transform: translateY(12px); }
      to   { opacity: 1; transform: translateY(0); }
    }
    .card { animation: fadeUp .35s ease both; }
    .card:nth-child(1) { animation-delay: .05s; }
    .card:nth-child(2) { animation-delay: .10s; }
    .card:nth-child(3) { animation-delay: .15s; }
    .card:nth-child(4) { animation-delay: .20s; }
    .card:nth-child(5) { animation-delay: .25s; }
    .card:nth-child(6) { animation-delay: .30s; }
  </style>
</head>
<body>
<div class="app">

  <!-- 顶栏 -->
  <div class="header">
    <div class="header-icon">🌿</div>
    <div>
      <div class="header-title">智能植物灌溉</div>
      <div class="header-sub" id="ip_addr">连接中...</div>
    </div>
    <div class="online-dot" id="online_dot"></div>
  </div>

  <!-- 湿度 + 状态 -->
  <div class="card moisture-card">
    <div class="ring-wrap">
      <svg class="ring-svg" width="110" height="110" viewBox="0 0 100 100">
        <circle class="ring-bg"   cx="50" cy="50" r="45"/>
        <circle class="ring-fill" cx="50" cy="50" r="45" id="ring_fill"/>
      </svg>
      <div class="ring-center">
        <span class="ring-val" id="moisture_val">--</span>
        <span class="ring-unit">湿度%</span>
      </div>
    </div>
    <div class="moisture-info">
      <div class="info-row">
        <span class="info-label">水泵</span>
        <span id="pump_badge"><span class="badge badge-muted">空闲</span></span>
      </div>
      <div class="info-row">
        <span class="info-label">模式</span>
        <span id="mode_badge"><span class="badge badge-blue">自动</span></span>
      </div>
      <div class="info-row">
        <span class="info-label">原始值</span>
        <span class="info-value" id="raw_val">---</span>
      </div>
    </div>
  </div>

  <!-- 统计 -->
  <div class="card">
    <div class="card-title">运行统计</div>
    <div class="stat-grid">
      <div class="stat-cell">
        <div class="stat-cell-label">累计浇水</div>
        <div class="stat-cell-val" id="total_val">--</div>
        <div class="stat-cell-unit" id="total_unit">秒</div>
      </div>
      <div class="stat-cell">
        <div class="stat-cell-label">阈值设定</div>
        <div class="stat-cell-val" id="stat_threshold">--</div>
        <div class="stat-cell-unit">% 触发</div>
      </div>
    </div>
  </div>

  <!-- 模式 & 手动控制 -->
  <div class="card">
    <div class="card-title">控制面板</div>
    <div class="ctrl-row">
      <div>
        <div class="ctrl-label">工作模式</div>
        <div class="ctrl-sub" id="mode_desc">每小时自动检测</div>
      </div>
      <button class="btn btn-ghost btn-sm" id="mode_btn" onclick="toggleMode()">切换</button>
    </div>
    <div id="manual_row" style="display:none;" class="pump-btn-wrap">
      <button class="btn btn-full" id="pump_btn" onclick="togglePump()">开启水泵</button>
    </div>
  </div>

  <!-- 参数设置 -->
  <div class="card">
    <div class="card-title">参数设置</div>

    <div class="slider-group">
      <div class="slider-head">
        <span class="slider-head-label">自动浇水阈值</span>
        <span class="slider-head-val" id="threshold_val">--%</span>
      </div>
      <input type="range" min="5" max="95" value="40" id="threshold_slider"
             oninput="document.getElementById('threshold_val').innerText=this.value+'%'"
             onchange="updateThreshold(this.value)">
    </div>

    <div style="height:18px;"></div>

    <div class="slider-group">
      <div class="slider-head">
        <span class="slider-head-label">单次浇水时长</span>
        <span class="slider-head-val" id="duration_val">--秒</span>
      </div>
      <input type="range" min="3" max="120" value="10" id="duration_slider"
             oninput="document.getElementById('duration_val').innerText=this.value+'秒'"
             onchange="updateDuration(this.value)">
      <div class="slider-hint">传感器原始 ADC: <span id="raw_hint">---</span></div>
    </div>
  </div>

  <!-- 运行日志 -->
  <div class="card">
    <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:4px;">
      <div class="card-title" style="margin-bottom:0;">运行日志</div>
      <button class="btn btn-danger btn-sm" onclick="clearLogs()">清空</button>
    </div>
    <div class="log-scroll" id="log_content">
      <div class="log-empty">日志加载中...</div>
    </div>
  </div>

  <!-- OTA -->
  <div class="card">
    <div class="card-title">固件升级 OTA</div>
    <div class="ota-area">
      <input type="file" name="update" id="ota_file" accept=".bin"
             style="display:none;" onchange="updateFileName(this)">
      <button class="btn btn-ghost btn-full" onclick="document.getElementById('ota_file').click()">
        选择固件文件 (.bin)
      </button>
      <div class="ota-filename" id="file_name">未选择文件</div>
      <button class="btn btn-blue btn-full" id="upload_btn"
              style="display:none;" onclick="handleUpload()">
        开始上传升级
      </button>
      <div class="progress-wrap" id="progress_container">
        <div class="progress-meta">
          <span id="progress_status">正在上传...</span>
          <span id="progress_text">0%</span>
        </div>
        <div class="progress-track">
          <div class="progress-fill" id="progress_bar"></div>
        </div>
      </div>
    </div>
  </div>

</div>

<script>
  // 累计秒转易读字符串
  function fmtSeconds(s) {
    if (s < 60) return { val: s, unit: '秒' };
    if (s < 3600) return { val: (s/60).toFixed(1), unit: '分钟' };
    return { val: (s/3600).toFixed(2), unit: '小时' };
  }

  function fetchData() {
    fetch('/data')
      .then(r => r.json())
      .then(d => {
        // IP
        document.getElementById('ip_addr').innerText = d.ip || '';

        // 圆环
        const pct = Math.max(0, Math.min(100, d.moisture));
        document.getElementById('moisture_val').innerText = pct + '%';
        const circ = 2 * Math.PI * 45;
        const offset = circ * (1 - pct / 100);
        const ring = document.getElementById('ring_fill');
        ring.style.strokeDashoffset = offset.toFixed(1);
        ring.style.stroke = pct > 50 ? 'var(--green)' : pct > 25 ? 'var(--blue)' : 'var(--red)';

        document.getElementById('raw_val').innerText  = d.raw;
        document.getElementById('raw_hint').innerText = d.raw;

        // 水泵 badge
        document.getElementById('pump_badge').innerHTML = d.pump
          ? '<span class="badge badge-green">浇水中</span>'
          : '<span class="badge badge-muted">空闲</span>';

        // 模式 badge
        const isAuto = (d.mode === 'AUTO');
        document.getElementById('mode_badge').innerHTML = isAuto
          ? '<span class="badge badge-blue">自动</span>'
          : '<span class="badge badge-amber">手动</span>';
        document.getElementById('mode_desc').innerText = isAuto ? '每小时自动检测' : '手动控制水泵';
        document.getElementById('mode_btn').innerText  = isAuto ? '切为手动' : '切为自动';

        // 手动行
        const manualRow = document.getElementById('manual_row');
        if (!isAuto) {
          manualRow.style.display = 'block';
          const pb = document.getElementById('pump_btn');
          if (d.pump) {
            pb.innerText = '关闭水泵';
            pb.classList.add('active');
          } else {
            pb.innerText = '开启水泵';
            pb.classList.remove('active');
          }
        } else {
          manualRow.style.display = 'none';
        }

        // 滑块同步（不在拖动时才更新）
        if (document.activeElement !== document.getElementById('threshold_slider')) {
          document.getElementById('threshold_val').innerText = d.threshold + '%';
          document.getElementById('threshold_slider').value  = d.threshold;
          document.getElementById('stat_threshold').innerText = d.threshold;
        }
        if (document.activeElement !== document.getElementById('duration_slider')) {
          document.getElementById('duration_val').innerText = d.duration + '秒';
          document.getElementById('duration_slider').value  = d.duration;
        }

        // 统计
        const fmt = fmtSeconds(d.total || 0);
        document.getElementById('total_val').innerText  = fmt.val;
        document.getElementById('total_unit').innerText = fmt.unit;
      })
      .catch(() => {
        document.getElementById('online_dot').style.background = 'var(--red)';
      });
  }

  function fetchLogs() {
    fetch('/get_logs')
      .then(r => r.text())
      .then(text => {
        const lines = text.trim().split('\n').filter(l => l.trim());
        if (lines.length <= 1) {
          document.getElementById('log_content').innerHTML = '<div class="log-empty">暂无日志</div>';
          return;
        }
        const headers = lines[0].split('\t').map(h => h.trim());
        let html = '<table><thead><tr>';
        headers.forEach(h => html += `<th>${h}</th>`);
        html += '</tr></thead><tbody>';
        for (let i = lines.length - 1; i >= 1; i--) {
          const cols = lines[i].split('\t').map(c => c.trim());
          html += '<tr>';
          cols.forEach((col, ci) => {
            let content = col;
            if (ci === headers.length - 1) { // 云端状态列
              if (col.includes('成功'))       content = `<span class="log-ok">${col}</span>`;
              else if (col.includes('失败'))   content = `<span class="log-fail">${col}</span>`;
            }
            html += `<td>${content}</td>`;
          });
          html += '</tr>';
        }
        html += '</tbody></table>';
        document.getElementById('log_content').innerHTML = html;
      });
  }

  function clearLogs() {
    if (confirm('确定清空所有本地日志？')) {
      fetch('/clear_logs').then(() => fetchLogs());
    }
  }

  function toggleMode()         { fetch('/toggle_mode').then(() => fetchData()); }
  function togglePump()         { fetch('/toggle_pump').then(() => fetchData()); }
  function updateThreshold(v)   { fetch('/set_threshold?val=' + v).then(() => fetchData()); }
  function updateDuration(v)    { fetch('/set_duration?val=' + v).then(() => fetchData()); }

  function updateFileName(input) {
    const name = input.files[0] ? input.files[0].name : '未选择文件';
    document.getElementById('file_name').innerText = name;
    document.getElementById('upload_btn').style.display = input.files[0] ? 'block' : 'none';
  }

  function handleUpload() {
    const fileInput = document.getElementById('ota_file');
    if (!fileInput.files[0]) return;
    const formData = new FormData();
    formData.append('update', fileInput.files[0]);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/update', true);
    document.getElementById('progress_container').style.display = 'block';
    document.getElementById('upload_btn').disabled = true;
    xhr.upload.addEventListener('progress', evt => {
      if (evt.lengthComputable) {
        const pct = Math.round(evt.loaded / evt.total * 100);
        document.getElementById('progress_bar').style.width  = pct + '%';
        document.getElementById('progress_text').innerText   = pct + '%';
      }
    });
    xhr.onload = () => {
      if (xhr.status === 200 && xhr.responseText.trim() === 'OK') {
        document.getElementById('progress_status').innerText = '升级成功，8秒后重启...';
        document.getElementById('progress_bar').style.background = 'var(--green)';
        setTimeout(() => location.reload(), 8000);
      } else {
        document.getElementById('progress_status').innerText = '升级失败，请重试';
        document.getElementById('progress_bar').style.background = 'var(--red)';
        document.getElementById('upload_btn').disabled = false;
      }
    };
    xhr.onerror = () => {
      document.getElementById('progress_status').innerText = '上传错误，检查连接';
      document.getElementById('progress_bar').style.background = 'var(--red)';
      document.getElementById('upload_btn').disabled = false;
    };
    xhr.send(formData);
  }

  setInterval(fetchData, 1500);
  setInterval(fetchLogs, 5000);
  window.onload = () => { fetchData(); fetchLogs(); };
</script>
</body>
</html>
)rawliteral";

// ==================== HTTP 路由 ====================

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleData() {
  String json = "{";
  json += "\"moisture\":"  + String(moisturePercent) + ",";
  json += "\"raw\":"       + String(rawMoisture)      + ",";
  json += "\"pump\":"      + String(pumpState ? "true" : "false") + ",";
  json += "\"mode\":\""    + String(isAutoMode ? "AUTO" : "MANUAL") + "\",";
  json += "\"threshold\":" + String(moistureThreshold) + ",";
  json += "\"duration\":"  + String(wateringDuration)  + ",";
  json += "\"total\":"     + String(totalWateringSeconds) + ",";
  json += "\"ip\":\""      + WiFi.localIP().toString() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleToggleMode() {
  isAutoMode = !isAutoMode;
  if (!isAutoMode) {
    // 切到手动：若泵还在跑（自动模式下）立即停掉并累计
    if (isPumpRunningAuto) {
      unsigned long elapsed = (millis() - autoPumpStartTime) / 1000;
      if (elapsed > 0 && elapsed < 3600) totalWateringSeconds += elapsed;
      isPumpRunningAuto = false;
    }
    pumpState = false;
    saveSettings();
  } else {
    firstAutoCheckDone = false; // 切回自动立即触发一次检测
    saveSettings();
  }
  server.send(200, "text/plain", "OK");
}

void handleTogglePump() {
  if (!isAutoMode) {
    pumpState = !pumpState;
    if (pumpState) {
      manualPumpStartTime = millis();
    } else {
      unsigned long elapsed = (millis() - manualPumpStartTime) / 1000;
      if (elapsed > 0 && elapsed < 3600) {
        totalWateringSeconds += elapsed;
        saveSettings();
        pendingLogEvent = "手动浇水"; // ★ 只标记，不阻塞
      }
    }
  }
  server.send(200, "text/plain", "OK"); // ★ 立即响应
}

void handleSetThreshold() {
  if (server.hasArg("val")) {
    int v = server.arg("val").toInt();
    if (v >= 5 && v <= 95) {
      moistureThreshold = v;
      saveSettings();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSetDuration() {
  if (server.hasArg("val")) {
    int v = server.arg("val").toInt();
    if (v >= 3 && v <= 120) {
      wateringDuration = v;
      saveSettings();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleGetLogs() {
  if (LittleFS.exists("/log.txt")) {
    File f = LittleFS.open("/log.txt", "r");
    server.streamFile(f, "text/plain; charset=utf-8");
    f.close();
  } else {
    server.send(200, "text/plain", "暂无日志");
  }
}

void handleClearLogs() {
  LittleFS.remove("/log.txt");
  initLocalLog();
  server.send(200, "text/plain", "OK");
}

// ==================== setup / loop ====================

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, PUMP_OFF);

  loadSettings();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected: " + WiFi.localIP().toString());

  // NTP 时间同步
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  Serial.print("NTP sync");
  time_t now = time(nullptr);
  for (int i = 0; i < 15 && now < 8 * 3600 * 2; i++) {
    delay(500); Serial.print("."); now = time(nullptr);
  }
  Serial.println();

  initLocalLog();
  logEvent("开机");

  server.on("/",              handleRoot);
  server.on("/data",          handleData);
  server.on("/toggle_mode",   handleToggleMode);
  server.on("/toggle_pump",   handleTogglePump);
  server.on("/set_threshold", handleSetThreshold);
  server.on("/set_duration",  handleSetDuration);
  server.on("/get_logs",      handleGetLogs);
  server.on("/clear_logs",    handleClearLogs);

  // OTA
  server.on("/update", HTTP_POST,
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
      if (!Update.hasError()) { shouldReboot = true; rebootTime = millis() + 1500; }
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.setDebugOutput(true);
        WiFiUDP::stopAll();
        Serial.printf("OTA start: %s\n", upload.filename.c_str());
        uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSpace)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
          Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("OTA success: %u bytes\n", upload.totalSize);
        else Update.printError(Serial);
        Serial.setDebugOutput(false);
      }
    }
  );

  server.begin();
  Serial.println("HTTP Server started");
}

void loop() {
  if (pendingLogEvent.length() > 0) {
    logEvent(pendingLogEvent);
    pendingLogEvent = "";
  }
  server.handleClient();
  unsigned long now = millis();

  // OTA 重启
  if (shouldReboot && now >= rebootTime) {
    Serial.println("Rebooting...");
    delay(100);
    ESP.restart();
  }

  // 每 1.5 秒读传感器
  if (now - lastCheckTime >= 1500) {
    lastCheckTime = now;
    rawMoisture   = analogRead(SENSOR_PIN);
    moisturePercent = map(rawMoisture, dryValue, wetValue, 0, 100);
    moisturePercent = constrain(moisturePercent, 0, 100);
  }

  // 自动模式逻辑
  if (isAutoMode) {
    if (!firstAutoCheckDone || (now - lastAutoCheckTime >= AUTO_CHECK_INTERVAL)) {
      firstAutoCheckDone = true;
      lastAutoCheckTime  = now;
      if (moisturePercent < moistureThreshold && !isPumpRunningAuto) {
        isPumpRunningAuto = true;
        autoPumpStartTime = now;
        pumpState = true;
        Serial.println("[AUTO] Soil dry, pump ON.");
      } else {
        Serial.println("[AUTO] Moisture OK.");
      }
    }

    // 自动浇水超时停止 —— ★ 用实际 elapsed 而非配置值累计
    if (isPumpRunningAuto) {
      unsigned long elapsed = (now - autoPumpStartTime) / 1000;
      if (elapsed >= (unsigned long)wateringDuration) {
        isPumpRunningAuto = false;
        pumpState = false;
        totalWateringSeconds += elapsed; // ★ 实际运行秒数
        saveSettings();
        logEvent("自动浇水");
        Serial.printf("[AUTO] Pump OFF. elapsed=%lus total=%lus\n", elapsed, totalWateringSeconds);
      }
    }
  } else {
    isPumpRunningAuto  = false;
    firstAutoCheckDone = false;
  }

  digitalWrite(RELAY_PIN, pumpState ? PUMP_ON : PUMP_OFF);
}
