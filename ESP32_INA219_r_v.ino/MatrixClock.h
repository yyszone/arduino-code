#ifndef MATRIX_CLOCK_H
#define MATRIX_CLOCK_H

#include <WebServer.h>
#include <time.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>


// 极简锐利 3x6 字体
const byte font3x6[10][6] = {
  {B111, B101, B101, B101, B101, B111}, // 0
  {B010, B110, B010, B010, B010, B111}, // 1
  {B111, B001, B001, B111, B100, B111}, // 2
  {B111, B001, B001, B111, B001, B111}, // 3
  {B101, B101, B101, B111, B001, B001}, // 4
  {B111, B100, B100, B111, B001, B111}, // 5
  {B111, B100, B100, B111, B101, B111}, // 6
  {B111, B001, B001, B010, B010, B010}, // 7
  {B111, B101, B101, B111, B101, B111}, // 8
  {B111, B101, B101, B111, B001, B111}  // 9
};

// ============================================================
// HTML 模板
// ============================================================
const char CLOCK_HTML_TEMPLATE[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset='utf-8'><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>独立时钟控制台</title><style>
* { box-sizing: border-box; }
body { font-family: 'Segoe UI', sans-serif; background: #0f0f0f; color: #fff; padding: 20px; display: flex; justify-content: center; flex-direction: column; align-items: center; margin: 0; }
.card { background: #1a1a1a; padding: 30px; border-radius: 15px; width: 100%; max-width: 440px; box-shadow: 0 8px 30px rgba(0,0,0,0.9); }
h2 { text-align: center; color: #ff007f; letter-spacing: 2px; margin-top: 0; }
h3 { margin: 15px 0 5px 0; font-size: 14px; color: #38bdf8; }
label { display: block; margin-top: 15px; font-size: 13px; color: #aaa; }
input, select, button { width: 100%; margin-top: 5px; padding: 12px; border-radius: 8px; border: none; box-sizing: border-box; font-size: 15px; }
input[type=number], select { background: #2a2a2a; color: #fff; }
input[type=color] { padding: 0; height: 45px; cursor: pointer; }
button { background: linear-gradient(90deg, #ff007f, #7f00ff); color: #fff; font-weight: bold; margin-top: 25px; cursor: pointer; transition: 0.3s; }
button:hover { opacity: 0.8; }
.back-btn { background: #333; margin-top: 15px; text-align: center; display: block; text-decoration: none; color: #bbb; padding: 12px; border-radius: 8px; }
.refresh-btn { background: #164e63; margin-top: 12px; text-align: center; display: block; text-decoration: none; color: #cffafe; padding: 12px; border-radius: 8px; border: 1px solid #0891b2; }
.stat-row { display: flex; justify-content: space-between; align-items: center; background: #111; border-radius: 8px; padding: 10px 14px; margin-top: 10px; font-size: 13px; }
.stat-label { color: #888; }
.stat-value { color: #fff; font-weight: bold; font-size: 15px; }
.batt-bar-wrap { background: #222; border-radius: 6px; height: 14px; margin-top: 6px; overflow: hidden; }
.batt-bar-fill { height: 100%; border-radius: 6px; transition: width 0.5s ease, background 0.5s ease; }
.divider { border-top: 1px solid #2a2a2a; margin: 18px 0; }
.sub-hint { font-size: 11px; color: #777; margin-top: 4px; }
.br-manual-block { transition: opacity 0.2s; }
.palette-preview { display: flex; gap: 4px; margin-top: 8px; border-radius: 6px; overflow: hidden; }
.palette-swatch { flex: 1; height: 16px; }
.time-range-box { display: flex; gap: 10px; align-items: center; margin-top: 5px; }
.time-range-box input { width: 50%; }
.tag-night { display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 12px; }
</style></head><body>
<div class='card'>
  <h2>✨ 独立时钟控制台</h2>

  <!-- 实时状态卡片 -->
  <div class="stat-row"><span class="stat-label">⏱️ 运行时长</span><span class="stat-value">%UPTIME%</span></div>
  <div class="stat-row">
    <span class="stat-label">🔋 电池电量</span>
    <span class="stat-value">%BATT_PCT%%&nbsp;&nbsp;(%BATT_V% V)</span>
  </div>
  <div class="batt-bar-wrap"><div class="batt-bar-fill" style="width:%BATT_PCT%%; background:%BATT_COLOR%;"></div></div>
  <div class="stat-row"><span class="stat-label">🌙 夜间模式状态</span><span class="stat-value">%NIGHT_STATE%</span></div>

  <div class="divider"></div>

  <form action='/clock/save' method='POST'>
    <label>🖥️ 显示模式:</label>
    <select name='mode'>
      <option value='0' %MD_0%>🕒 仅显示时间 (xx:xx:xx)</option>
      <option value='1' %MD_1%>📅 仅显示日期</option>
      <option value='2' %MD_2%>🕒+📅 同屏双显 (MM.DD HH:MM)</option>
      <option value='3' %MD_3%>🔄 自动交替 (时间+日期滚动)</option>
      <option value='4' %MD_4%>⏱️ 显示运行时长</option>
    </select>

    <label>🎨 色彩主题:</label>
    <select name='theme' onchange="updatePalette(this.value)">
      <option value='2' %TH_2%>🌈 流光幻彩 (RGB流水)</option>
      <option value='4' %TH_4%>🔥 赛博火焰 (红橙渐变)</option>
      <option value='3' %TH_3%>🌅 迈阿密风 (粉青双拼)</option>
      <option value='1' %TH_1%>🔴 赛博三分色 (区分模块)</option>
      <option value='5' %TH_5%>🎲 数字彩虹 (每位独立色)</option>
      <option value='0' %TH_0%>⚪ 极简单色 (使用下方自定义)</option>
    </select>

    <div class="palette-preview" id="palettePrev" style="display:%PALETTE_DISPLAY%">
      <div class="palette-swatch" style="background:#ff3232" title="0"></div>
      <div class="palette-swatch" style="background:#ff8c00" title="1"></div>
      <div class="palette-swatch" style="background:#ffd700" title="2"></div>
      <div class="palette-swatch" style="background:#00e050" title="3"></div>
      <div class="palette-swatch" style="background:#00dcc8" title="4"></div>
      <div class="palette-swatch" style="background:#0096ff" title="5"></div>
      <div class="palette-swatch" style="background:#5000ff" title="6"></div>
      <div class="palette-swatch" style="background:#c000ff" title="7"></div>
      <div class="palette-swatch" style="background:#ff00b4" title="8"></div>
      <div class="palette-swatch" style="background:#dcdcdc" title="9"></div>
    </div>

    <label>🎨 自定义纯色 (仅极简单色模式):</label>
    <input type='color' name='color' value='%COLOR_HEX%'>

    <div class="divider"></div>

    <!-- 亮度模式 -->
    <label>☀️ 基础亮度模式:</label>
    <select name='bmode' id='bmodeSelect' onchange="toggleBrMode(this.value)">
      <option value='0' %BM_0%>🔧 手动亮度</option>
      <option value='1' %BM_1%>🔋 自动亮度 (跟随电量百分比)</option>
    </select>

    <div class="br-manual-block" id="brManualBlock" style="display:%BR_MANUAL_DISPLAY%">
      <label>☀️ 手动亮度 (0=关闭, 1-100):</label>
      <input type='number' name='br' id='brInput' min='0' max='100' value='%BRIGHTNESS%'>
      <div class="sub-hint">100 = 硬件安全最高亮度，防烧电源</div>
    </div>
    <div id="brAutoHint" style="display:%BR_AUTO_DISPLAY%; font-size:12px; color:#888; margin-top:8px; padding:8px; background:#111; border-radius:6px;">
      🔋 自动模式：电量 0% → 最低亮度，电量 100% → 最高亮度<br>电量低于 5% 时屏幕自动关闭。
    </div>

    <div class="divider"></div>

    <!-- 夜间模式配置 -->
    <h3>🌙 夜间节能暗光模式</h3>
    <label>夜间模式开关:</label>
    <select name='nm_en' id='nmSelect' onchange="toggleNightConfig(this.value)">
      <option value='1' %NM_ON%>✅ 启用夜间模式</option>
      <option value='0' %NM_OFF%>❌ 停用夜间模式</option>
    </select>

    <div id="nightConfigWrap" style="display:%NM_BOX_DISPLAY%;">
      <label>生效时间区间 (24小时制，支持跨天):</label>
      <div class="time-range-box">
        <input type="number" name="nm_sh" min="0" max="23" value="%NM_SH%" placeholder="开始(时)">
        <span style="color:#aaa;">至</span>
        <input type="number" name="nm_eh" min="0" max="23" value="%NM_EH%" placeholder="结束(时)">
      </div>
      <div class="sub-hint">例如：23 至 7，代表 23:00~次日07:00 自动调暗</div>

      <label>夜间亮度比例 (%，默认50%基础减半):</label>
      <input type='number' name='nm_scale' min='5' max='100' value='%NM_SCALE%'>
      <div class="sub-hint">在原亮度基础上乘以该比例 (例如原亮度 20，设 50% 则夜间为 10)</div>
    </div>

    <div class="divider"></div>

    <label>⏱️ 秒数与冒号样式:</label>
    <select name='sec'>
      <option value='1' %SEC_ON%>开启秒数 (xx:xx:xx)</option>
      <option value='0' %SEC_OFF%>仅时分 (xx:xx)</option>
    </select>

    <label>冒号显示效果:</label>
    <select name='colon_blink'>
      <option value='0' %COL_SOLID%>常亮 (稳定显示 xx:xx:xx)</option>
      <option value='1' %COL_BLINK%>闪烁 (每秒呼吸)</option>
    </select>

    <div class="divider"></div>

    <!-- 灯板降压模块输入电源定时循环 -->
    <h3>⚡ 灯板降压模块硬件断电恢复（MOSFET）</h3>
    <label>基础恢复周期（分钟，0=关闭自动恢复）:</label>
    <input type='number' name='pc_min' min='0' max='1440' step='1' value='%PC_MIN%'>
    <div class='sub-hint'>只切断灯板降压模块输入电源，不重启 ESP32，不影响 GPIO10 控制的逆变器。建议先设 10 分钟。</div>

    <label>每次断电时间（秒）:</label>
    <input type='number' name='pc_sec' min='1' max='10' step='1' value='%PC_SEC%'>
    <div class='sub-hint'>建议 2 秒。断电时 ESP32 保持运行，数据 GPIO 暂时设为高阻。</div>

    <label>高电压加速阈值（V，0=关闭加速）:</label>
    <input type='number' name='pc_hv' min='0' max='80' step='0.1' value='%PC_HV%'>
    <div class='sub-hint'>当 INA219 实测电压达到该值时，使用下方倍数缩短恢复周期。阈值必须按你实际测量的电池电压设置。</div>

    <label>达到高电压阈值后的周期缩短倍数（1～10）:</label>
    <input type='number' name='pc_mul' min='1' max='10' step='1' value='%PC_MUL%'>
    <div class='sub-hint'>例如基础周期 10 分钟、倍数 3：电压低于阈值时每 10 分钟恢复；达到阈值后每约 3 分钟恢复（最低 1 分钟）。倍数 1 表示不加速。</div>

    <div class="divider"></div>

    <label>🔋 电池类型 (用于智能曲线电量计算):</label>
    <select name='batt'>
      <option value='0' %BT_0%>12V 磷酸铁锂 (放电曲线精确匹配)</option>
      <option value='1' %BT_1%>24V 磷酸铁锂 (放电曲线精确匹配)</option>
    </select>
    <div class="sub-hint">※ 最下行像素条实时显示电量，颜色：绿&gt;60% / 黄&gt;30% / 红≤30%</div>

    <button type='submit'>💾 永久保存设置</button>
  </form>
  <a href='/clock/refresh' class='refresh-btn' onclick="return confirm('切断灯板降压模块输入电源 %PC_SEC% 秒再恢复？灯板会短暂熄灭。');">⚡ 立即断电 %PC_SEC% 秒并恢复</a>
  <div class='sub-hint' style='text-align:center'>基础周期：%PC_MIN% 分钟；断电：%PC_SEC% 秒；高压阈值：%PC_HV% V；缩短倍数：%PC_MUL%。0 分钟关闭自动恢复，0V 关闭高压加速。</div>
  <a href='/' class='back-btn'>🔙 返回继电器主页</a>
</div>

<script>
function toggleBrMode(val) {
  var manual = document.getElementById('brManualBlock');
  var hint   = document.getElementById('brAutoHint');
  if (val === '0') { manual.style.display = 'block'; hint.style.display = 'none'; }
  else             { manual.style.display = 'none';  hint.style.display = 'block'; }
}
function toggleNightConfig(val) {
  document.getElementById('nightConfigWrap').style.display = (val === '1') ? 'block' : 'none';
}
function updatePalette(val) {
  document.getElementById('palettePrev').style.display = (val === '5') ? 'flex' : 'none';
}
</script>
</body></html>
)=====";

// ============================================================
// SmartMatrixClock 类
// ============================================================
class SmartMatrixClock {
private:
  Adafruit_NeoMatrix* matrix;
  WebServer*          webServer;
  SemaphoreHandle_t matrixMutex = nullptr;

  void lockState() {
    if (matrixMutex) xSemaphoreTake(matrixMutex, portMAX_DELAY);
  }
  void unlockState() {
    if (matrixMutex) xSemaphoreGive(matrixMutex);
  }

  int   clockPin;
  int   matrixPowerPin; // 外部高侧电子开关模块 EN/CTRL：HIGH=接通降压模块 VIN+，LOW=断开
  int   currentBrightness = 20;   // 0-100 (0=off)
  int   brightnessMode    = 0;    // 0=手动, 1=自动(跟随电量)
  bool  showSeconds       = true;
  bool  colonBlink        = false; // 默认冒号常亮，彻底杜绝 xx xx xx
  int   currentTheme      = 2;
  int   displayMode       = 0;    // 0=仅时间, 1=仅日期, 2=双显, 3=交替, 4=运行
  uint8_t colorR = 0, colorG = 255, colorB = 255;

  // 夜间模式设置
  bool  nightModeEnabled      = true; // 默认启用夜间模式
  int   nightStartHour        = 23;   // 默认 23:00
  int   nightEndHour          = 7;    // 默认 07:00
  int   nightBrightnessScale  = 50;   // 默认 50% 减半

  // 电池
  int   batteryType    = 0;       // 0=12V LiFePO4, 1=24V LiFePO4
  float batteryVoltage = 0.0f;

  // 降压模块输入正极由外部高侧电子开关控制：GPIO HIGH=通电，LOW=断电。
  int autoPowerCycleMinutes = 10;  // 基础周期，0=禁用，1-1440 分钟
  int powerOffSeconds = 2;         // 每次断电 1-10 秒
  float highVoltageThreshold = 13.0f; // 达到阈值后缩短周期；0=关闭高压加速
  int highVoltageMultiplier = 3;       // 基础周期除以该倍数，最低 1 分钟

  // 以下快照供独立电源恢复任务读取；该任务不依赖 WebServer/main loop。
  volatile int scheduleCycleMinutes = 10;
  volatile int scheduleOffSeconds = 2;
  volatile float scheduleHighVoltageThreshold = 13.0f;
  volatile int scheduleHighVoltageMultiplier = 3;
  volatile float scheduleVoltage = 0.0f;
  volatile uint32_t lastPowerCycleMs = 0;
  volatile bool manualPowerCycleRequested = false;
  volatile bool driverRefreshRequested = false;
  volatile bool powerCycleInProgress = false;
  TaskHandle_t powerTaskHandle = nullptr;

  bool  isShowingTime  = true;
  unsigned long lastSwitchTime = 0;
  unsigned long lastRefresh    = 0;
  int   lastEffBr      = -1;

  // ── 判断当前是否属于夜间区间 ──────────────────────────────
  bool isNightTimeNow() {
    if (!nightModeEnabled) return false;
    time_t now = time(nullptr);
    if (now < 1000000UL) return false; // 未授时暂不触发
    struct tm* t = localtime(&now);
    int h = t->tm_hour;

    if (nightStartHour == nightEndHour) {
      return false; 
    }
    if (nightStartHour < nightEndHour) {
      return (h >= nightStartHour && h < nightEndHour);
    } else {
      // 跨夜 (例如 23点 至 7点)
      return (h >= nightStartHour || h < nightEndHour);
    }
  }

  // ── 每位数字独立颜色 ──────────────────────────────────────
  uint16_t getDigitColor(int digit) {
    switch (digit) {
      case 0: return matrix->Color(255,  50,  50);
      case 1: return matrix->Color(255, 140,   0);
      case 2: return matrix->Color(255, 215,   0);
      case 3: return matrix->Color(  0, 224,  80);
      case 4: return matrix->Color(  0, 220, 200);
      case 5: return matrix->Color(  0, 150, 255);
      case 6: return matrix->Color( 80,   0, 255);
      case 7: return matrix->Color(192,   0, 255);
      case 8: return matrix->Color(255,   0, 180);
      case 9: return matrix->Color(220, 220, 220);
      default: return matrix->Color(255, 255, 255);
    }
  }

  // ── 精准电量百分比计算（磷酸铁锂真实放电曲线） ──────────
  int getBatteryPercent() {
    if (batteryVoltage < 0.5f) return 0;

    float v = batteryVoltage;
    if (batteryType == 1) { 
      v = v / 2.0f; 
    }

    if (v >= 13.6f) return 100;
    if (v <= 11.0f) return 0;  

    const float curve[11][2] = {
      {13.6, 100}, {13.4, 90}, {13.3, 80}, {13.2, 70},
      {13.1, 60},  {13.0, 50}, {12.9, 40}, {12.8, 30},
      {12.5, 20},  {12.0, 10}, {11.0, 0}
    };

    for (int i = 0; i < 10; i++) {
      if (v <= curve[i][0] && v > curve[i+1][0]) {
        float v_high = curve[i][0];
        float p_high = curve[i][1];
        float v_low  = curve[i+1][0];
        float p_low  = curve[i+1][1];
        float exactPct = p_low + (v - v_low) * (p_high - p_low) / (v_high - v_low);
        return (int)exactPct;
      }
    }
    return 0;
  }

  // ── 实际发光亮度转换（加入夜间缩减比例） ──────────────────
  int getEffectiveBrightness() {
    int br100; 
    if (brightnessMode == 1) {
      int pct = getBatteryPercent();
      if (pct <= 5) return 0; 
      br100 = map(pct, 5, 100, 5, 100);
    } else {
      br100 = currentBrightness; 
    }
    
    if (br100 <= 0) return 0;

    // 夜间模式缩减亮度 (默认减半为 50%)
    if (isNightTimeNow()) {
      br100 = (br100 * nightBrightnessScale) / 100;
      if (br100 < 1) br100 = 1; // 保持微光不彻底全黑
    }

    return (int)map(br100, 1, 100, 4, 120);
  }

  // ── 永久记忆存储 ──────────────────────────────────────────
  void loadSettings() {
    if (!LittleFS.exists("/clock_cfg.txt")) return;
    File f = LittleFS.open("/clock_cfg.txt", "r");
    if (!f) return;

    auto readLine = [&](int& out) {
      if (f.available()) { String v = f.readStringUntil('\n'); v.trim(); if (v.length()) out = v.toInt(); }
    };
    auto readLineBool = [&](bool& out) {
      if (f.available()) { String v = f.readStringUntil('\n'); v.trim(); if (v.length()) out = (v == "1"); }
    };
    auto readLineU8 = [&](uint8_t& out) {
      if (f.available()) { String v = f.readStringUntil('\n'); v.trim(); if (v.length()) out = (uint8_t)v.toInt(); }
    };
    auto readLineFloat = [&](float& out) {
      if (f.available()) { String v = f.readStringUntil('\n'); v.trim(); if (v.length()) out = v.toFloat(); }
    };

    readLine(currentBrightness); 
    readLine(currentTheme);      
    readLine(displayMode);       
    readLineBool(showSeconds);   
    readLineU8(colorR);          
    readLineU8(colorG);          
    readLineU8(colorB);          
    readLine(batteryType);       
    readLine(brightnessMode);    

    // 新增扩展参数兼容读取
    readLineBool(nightModeEnabled);
    readLine(nightStartHour);
    readLine(nightEndHour);
    readLine(nightBrightnessScale);
    readLineBool(colonBlink);
    // 新增自动断电恢复配置；旧配置文件没有这两行时保留默认值。
    readLine(autoPowerCycleMinutes);
    readLine(powerOffSeconds);
    readLineFloat(highVoltageThreshold);  // 兼容旧配置：没有该行时保留默认 13.0V
    readLine(highVoltageMultiplier);      // 兼容旧配置：没有该行时保留默认 3 倍
    autoPowerCycleMinutes = constrain(autoPowerCycleMinutes, 0, 1440);
    powerOffSeconds = constrain(powerOffSeconds, 1, 10);
    highVoltageThreshold = constrain(highVoltageThreshold, 0.0f, 80.0f);
    highVoltageMultiplier = constrain(highVoltageMultiplier, 1, 10);

    f.close();
    Serial.println("[Clock] 设置读取成功！");
  }

  void saveSettings() {
    File f = LittleFS.open("/clock_cfg.txt", "w");
    if (!f) return;
    f.println(currentBrightness);
    f.println(currentTheme);
    f.println(displayMode);
    f.println(showSeconds ? "1" : "0");
    f.println(colorR);
    f.println(colorG);
    f.println(colorB);
    f.println(batteryType);
    f.println(brightnessMode);   
    f.println(nightModeEnabled ? "1" : "0");
    f.println(nightStartHour);
    f.println(nightEndHour);
    f.println(nightBrightnessScale);
    f.println(colonBlink ? "1" : "0");
    f.println(autoPowerCycleMinutes);
    f.println(powerOffSeconds);
    f.println(highVoltageThreshold, 1);
    f.println(highVoltageMultiplier);
    f.close();
    Serial.println("[Clock] 设置保存成功！");
  }

  // ── 颜色辅助 ──────────────────────────────────────────────
  uint16_t getPixelColor(int x, int digit, uint16_t baseColor) {
    if (currentTheme == 5) {
      return getDigitColor(digit);
    } else if (currentTheme == 2) {
      uint32_t rgb = Adafruit_NeoPixel::ColorHSV((millis() * 15) + (x * 2000), 255, 255);
      return matrix->Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    } else if (currentTheme == 4) {
      return matrix->Color(255, (uint8_t)map(x, 0, 31, 20, 150), 0);
    }
    return baseColor;
  }

  // ── 绘制函数 ──────────────────────────────────────────────
  void drawDigit(int x, int y, int digit, uint16_t baseC) {
    for (int row = 0; row < 6; row++)
      for (int col = 0; col < 3; col++)
        if (font3x6[digit][row] & (1 << (2 - col)))
          matrix->drawPixel(x + col, y + row, getPixelColor(x + col, digit, baseC));
  }

  void drawColon(int x, int y, uint16_t baseC) {
    matrix->drawPixel(x, y + 1, getPixelColor(x, 10, baseC));
    matrix->drawPixel(x, y + 4, getPixelColor(x, 10, baseC));
  }

  void drawDot(int x, int y, uint16_t baseC) {
    matrix->drawPixel(x, y + 4, getPixelColor(x, 10, baseC));
  }

  // ── 最下行电池电量像素条 ──────────────────────────────────
  void drawBatteryBar() {
    int pct = getBatteryPercent();
    int filled = (pct * 32 + 50) / 100;
    if (filled > 32) filled = 32;

    uint16_t barColor;
    if      (pct > 60) barColor = matrix->Color(  0, 200,   0);
    else if (pct > 30) barColor = matrix->Color(200, 150,   0);
    else               barColor = matrix->Color(200,   0,   0);

    for (int x = 0; x < filled; x++)
      matrix->drawPixel(x, 7, barColor);
  }

  // ── 运行时长显示 ──────────────────────────────────────────
  void drawUptime(int y, uint16_t c_h, uint16_t c_m, uint16_t c_s, uint16_t c_c, bool colonVisible) {
    unsigned long upSec = millis() / 1000UL;
    int totalH = (int)(upSec / 3600);
    int upM    = (int)((upSec % 3600) / 60);
    int upS    = (int)(upSec % 60);

    if (totalH < 100) {
      drawDigit(2,  y, totalH / 10, c_h); drawDigit(6,  y, totalH % 10, c_h);
      if (colonVisible) drawColon(10, y, c_c);
      drawDigit(12, y, upM / 10,   c_m); drawDigit(16, y, upM % 10,   c_m);
      if (colonVisible) drawColon(20, y, c_c);
      drawDigit(22, y, upS / 10,   c_s); drawDigit(26, y, upS % 10,   c_s);
    } else {
      int upD  = totalH / 24;
      int upHH = totalH % 24;
      if (upD > 99) upD = 99;
      drawDigit(1,  y, upD  / 10, c_h); drawDigit(5,  y, upD  % 10, c_h);
      drawDot(9, y, c_c);
      drawDigit(11, y, upHH / 10, c_m); drawDigit(15, y, upHH % 10, c_m);
      if (colonVisible) drawColon(19, y, c_c);
      drawDigit(21, y, upM  / 10, c_s); drawDigit(25, y, upM  % 10, c_s);
    }
  }

  // ── 主刷新 ────────────────────────────────────────────────
  void updateDisplay() {
    if (matrix == nullptr) return;
    yield();
    int effBr = getEffectiveBrightness();
    if (effBr != lastEffBr) {
        matrix->setBrightness(effBr);
        lastEffBr = effBr;
    }
    matrix->fillScreen(0);

    if (effBr == 0) { 
      matrix->show(); 
      yield(); 
      return; 
    }

    time_t now = time(nullptr);
    struct tm* tInfo = localtime(&now);
    int Y = tInfo->tm_year + 1900;
    int M = tInfo->tm_mon + 1;
    int D = tInfo->tm_mday;
    int h = tInfo->tm_hour;
    int m = tInfo->tm_min;
    int s = tInfo->tm_sec;

    // 冒号显示控制：如果不开启闪烁则始终显示常亮，呈现稳健的 xx:xx:xx
    bool colonVisible = true;
    if (colonBlink) {
      colonVisible = ((millis() / 500) % 2) == 0;
    }

    int y = 1;

    uint16_t c_main = matrix->Color(colorR, colorG, colorB);
    uint16_t c_h = c_main, c_m = c_main, c_s_col = c_main, c_c = c_main;

    if (currentTheme == 1) {
      c_h     = matrix->Color(  0, 255, 255);
      c_m     = matrix->Color(255,   0, 255);
      c_s_col = matrix->Color(255, 255,   0);
      c_c     = matrix->Color(255, 255, 255);
    } else if (currentTheme == 3) {
      c_h     = matrix->Color(255,   0, 127);
      c_m     = matrix->Color(  0, 255, 255);
      c_s_col = matrix->Color(255,   0, 127);
      c_c     = matrix->Color(255, 255, 255);
    } else if (currentTheme == 5) {
      c_c = matrix->Color(180, 180, 180);
    }

    int currentMode = displayMode;
    if (displayMode == 3) {
      if ( isShowingTime && millis() - lastSwitchTime > 8000) { isShowingTime = false; lastSwitchTime = millis(); }
      if (!isShowingTime && millis() - lastSwitchTime > 3000) { isShowingTime = true;  lastSwitchTime = millis(); }
      currentMode = isShowingTime ? 0 : 1;
    }

    // xx:xx:xx 精准对齐布局
    if (currentMode == 0) {
      if (showSeconds) {
        drawDigit(2,  y, h/10, c_h);    drawDigit(6,  y, h%10, c_h);
        if (colonVisible) drawColon(10, y, c_c);
        drawDigit(12, y, m/10, c_m);    drawDigit(16, y, m%10, c_m);
        if (colonVisible) drawColon(20, y, c_c);
        drawDigit(22, y, s/10, c_s_col); drawDigit(26, y, s%10, c_s_col);
      } else {
        drawDigit(7,  y, h/10, c_h);    drawDigit(11, y, h%10, c_h);
        if (colonVisible) drawColon(15, y, c_c);
        drawDigit(17, y, m/10, c_m);    drawDigit(21, y, m%10, c_m);
      }
    }
    else if (currentMode == 1) {
      int shortYear = Y % 100;
      drawDigit(1,  y, shortYear / 10, c_h); drawDigit(5,  y, shortYear % 10, c_h);
      drawDot(9, y, c_c);
      drawDigit(11, y, M/10, c_m); drawDigit(15, y, M%10, c_m);
      drawDot(19, y, c_c);
      drawDigit(21, y, D/10, c_s_col); drawDigit(25, y, D%10, c_s_col);
    }
    else if (currentMode == 2) {
      drawDigit(1,  y, M/10, c_h);  drawDigit(5,  y, M%10, c_h);
      drawDot(8, y, c_c);
      drawDigit(9,  y, D/10, c_m);  drawDigit(13, y, D%10, c_m);
      drawDigit(17, y, h/10, c_s_col); drawDigit(21, y, h%10, c_s_col);
      if (colonVisible) drawColon(24, y, c_c);
      drawDigit(25, y, m/10, c_main); drawDigit(29, y, m%10, c_main);
    }
    else if (currentMode == 4) {
      drawUptime(y, c_h, c_m, c_s_col, c_c, colonVisible);
    }

    drawBatteryBar();
    matrix->show();     
    yield();
  }

  // ── 运行时长格式化（供网页） ─────────────────────────────
  String formatUptime() {
    unsigned long upSec = millis() / 1000UL;
    int totalH = (int)(upSec / 3600);
    int upM    = (int)((upSec % 3600) / 60);
    int upS    = (int)(upSec % 60);
    char buf[32];
    if (totalH < 24) {
      sprintf(buf, "%02d:%02d:%02d", totalH, upM, upS);
    } else {
      int upD = totalH / 24;
      int upHH = totalH % 24;
      sprintf(buf, "%d天 %02d:%02d:%02d", upD, upHH, upM, upS);
    }
    return String(buf);
  }

public:
  SmartMatrixClock(int pin, int powerPin, WebServer& srv) {
    clockPin = pin;
    matrixPowerPin = powerPin;
    webServer = &srv;
    matrix = new Adafruit_NeoMatrix(32, 8, clockPin,
      NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
      NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
      NEO_GRB + NEO_KHZ800);
  }

  // 重建 NeoMatrix 对象。调用前必须持有 matrixMutex。
  // 失败时不再软件重启 ESP32；硬件电源恢复任务仍继续工作并可再次尝试。
  bool recreateMatrixDriver() {
    if (matrix != nullptr) {
      delete matrix;
      matrix = nullptr;
    }
    delay(5);
    matrix = new Adafruit_NeoMatrix(32, 8, clockPin,
      NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
      NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
      NEO_GRB + NEO_KHZ800);
    if (matrix == nullptr) {
      Serial.println("[Clock] 重建灯板驱动失败；不重启 ESP32，将在后续周期重试");
      return false;
    }
    matrix->begin();
    matrix->setTextWrap(false);
    lastEffBr = -1;
    return true;
  }

  // 定时/手动电源恢复任务。它单独运行，不在 WebServer 请求处理中 delay，
  // 因此即使网页接口处理慢，也能按周期真实切断 DC-DC 的输入正极。
  void powerCycleWorker() {
    Serial.println("[MatrixPowerTask] 独立硬件电源恢复任务已启动");
    for (;;) {
      const uint32_t now = millis();
      const int baseMinutes = scheduleCycleMinutes;
      const int offSec = constrain((int)scheduleOffSeconds, 1, 10);
      const float voltage = scheduleVoltage;
      const float threshold = scheduleHighVoltageThreshold;
      const int multiplier = constrain((int)scheduleHighVoltageMultiplier, 1, 10);
      const bool manualRequest = manualPowerCycleRequested;

      int effectiveMinutes = baseMinutes;
      const bool highVoltage = (threshold > 0.0f && voltage >= threshold);
      if (baseMinutes > 0 && highVoltage && multiplier > 1) {
        effectiveMinutes = baseMinutes / multiplier;
        if (effectiveMinutes < 1) effectiveMinutes = 1;
      }

      const bool autoDue = (effectiveMinutes > 0) &&
          ((uint32_t)(now - lastPowerCycleMs) >= (uint32_t)effectiveMinutes * 60000UL);

      if (!powerCycleInProgress && (manualRequest || autoDue)) {
        manualPowerCycleRequested = false;
        powerCycleInProgress = true;

        Serial.printf("[MatrixPowerTask] 开始硬件断电恢复：电压=%.2fV，高压状态=%s，基础周期=%d分钟，当前周期=%d分钟，断电=%d秒\n",
                      voltage, highVoltage ? "是" : "否", baseMinutes,
                      effectiveMinutes, offSec);

        // 先停止数据驱动，降低 DIN 在灯板掉电时的反向供电风险。
        pinMode(clockPin, INPUT);
        digitalWrite(clockPin, LOW);
        digitalWrite(matrixPowerPin, LOW);  // 外部高侧开关 CTRL：LOW=断电
        vTaskDelay(pdMS_TO_TICKS((uint32_t)offSec * 1000UL));

        digitalWrite(matrixPowerPin, HIGH); // 恢复降压模块输入电源
        vTaskDelay(pdMS_TO_TICKS(200));      // 等 DC-DC 输出稳定

        // 驱动对象只能由显示任务销毁/重建；此处仅发出请求。
        driverRefreshRequested = true;
        lastPowerCycleMs = millis();
        powerCycleInProgress = false;
        Serial.printf("[MatrixPowerTask] 电源已重新接通；等待显示任务重建驱动，heap=%u\n",
                      (unsigned)ESP.getFreeHeap());
      }

      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  static void powerCycleTaskEntry(void* arg) {
    SmartMatrixClock* self = static_cast<SmartMatrixClock*>(arg);
    if (self) self->powerCycleWorker();
    vTaskDelete(nullptr);
  }

  bool requestPowerCycle() {
    if (powerCycleInProgress || manualPowerCycleRequested) return false;
    manualPowerCycleRequested = true;
    return true;
  }

  void setBatteryVoltage(float v) {
    lockState();
    batteryVoltage = v;
    scheduleVoltage = v; // 独立电源任务按 INA219 实测电压选择恢复周期
    unlockState();
  }

  void begin() {
    if (!matrixMutex) matrixMutex = xSemaphoreCreateMutex();
    if (!matrixMutex) Serial.println("[Clock] 警告：矩阵互斥锁创建失败");
    loadSettings();
    scheduleCycleMinutes = autoPowerCycleMinutes;
    scheduleOffSeconds = powerOffSeconds;
    scheduleHighVoltageThreshold = highVoltageThreshold;
    scheduleHighVoltageMultiplier = highVoltageMultiplier;
    lastPowerCycleMs = millis();

    // GPIO1 只接外部高侧电子开关的逻辑 EN/CTRL，禁止连接电池正极。
    // 选购支持 3.3V 逻辑触发、HIGH=ON 的模块；控制端加下拉，复位时默认断电。
    pinMode(clockPin, INPUT);
    digitalWrite(clockPin, LOW);
    pinMode(matrixPowerPin, OUTPUT);
    digitalWrite(matrixPowerPin, HIGH);
    delay(120);

    matrix->begin();
    matrix->setTextWrap(false);
    matrix->setBrightness(getEffectiveBrightness());
    matrix->fillScreen(0);
    matrix->show();
    yield();
    lastRefresh = millis();
    Serial.printf("[MatrixPower] GPIO%d：基础周期=%d分钟，断电=%d秒，高压阈值=%.1fV，缩短倍数=%d\n",
                  matrixPowerPin, autoPowerCycleMinutes, powerOffSeconds,
                  highVoltageThreshold, highVoltageMultiplier);

    // 系统时间与联网校时由主 .ino 统一管理；此处不进行任何网络同步调用。

    // ── GET /clock ─────────────────────────────────────────
    webServer->on("/clock", HTTP_GET, [this]() {
      lockState();
      String html = FPSTR(CLOCK_HTML_TEMPLATE);

      html.replace("%UPTIME%", formatUptime());

      int pct = getBatteryPercent();
      char voltBuf[8]; dtostrf(batteryVoltage, 4, 2, voltBuf);
      html.replace("%BATT_PCT%", String(pct));
      html.replace("%BATT_V%",   String(voltBuf));
      String battColor = (pct > 60) ? "#00c800" : (pct > 30) ? "#c89600" : "#c80000";
      html.replace("%BATT_COLOR%", battColor);

      // 夜间模式状态显示
      if (isNightTimeNow()) {
        html.replace("%NIGHT_STATE%", "<span class='tag-night' style='background:#701a75; color:#f0abfc;'>🌙 调光生效中 (" + String(nightBrightnessScale) + "%)</span>");
      } else if (nightModeEnabled) {
        html.replace("%NIGHT_STATE%", "<span class='tag-night' style='background:#14532d; color:#86efac;'>☀️ 正常亮度 (待命)</span>");
      } else {
        html.replace("%NIGHT_STATE%", "<span class='tag-night' style='background:#374151; color:#9ca3af;'>已关闭</span>");
      }

      char hexColor[8]; sprintf(hexColor, "#%02x%02x%02x", colorR, colorG, colorB);
      html.replace("%COLOR_HEX%", String(hexColor));

      html.replace("%BRIGHTNESS%", String(currentBrightness));

      html.replace("%MD_0%", displayMode == 0 ? "selected" : "");
      html.replace("%MD_1%", displayMode == 1 ? "selected" : "");
      html.replace("%MD_2%", displayMode == 2 ? "selected" : "");
      html.replace("%MD_3%", displayMode == 3 ? "selected" : "");
      html.replace("%MD_4%", displayMode == 4 ? "selected" : "");

      html.replace("%TH_2%", currentTheme == 2 ? "selected" : "");
      html.replace("%TH_4%", currentTheme == 4 ? "selected" : "");
      html.replace("%TH_3%", currentTheme == 3 ? "selected" : "");
      html.replace("%TH_1%", currentTheme == 1 ? "selected" : "");
      html.replace("%TH_5%", currentTheme == 5 ? "selected" : "");
      html.replace("%TH_0%", currentTheme == 0 ? "selected" : "");

      html.replace("%SEC_ON%",  showSeconds ? "selected" : "");
      html.replace("%SEC_OFF%", !showSeconds ? "selected" : "");

      html.replace("%COL_SOLID%", !colonBlink ? "selected" : "");
      html.replace("%COL_BLINK%", colonBlink ? "selected" : "");

      html.replace("%BT_0%", batteryType == 0 ? "selected" : "");
      html.replace("%BT_1%", batteryType == 1 ? "selected" : "");

      html.replace("%BM_0%", brightnessMode == 0 ? "selected" : "");
      html.replace("%BM_1%", brightnessMode == 1 ? "selected" : "");
      html.replace("%BR_MANUAL_DISPLAY%", brightnessMode == 0 ? "block" : "none");
      html.replace("%BR_AUTO_DISPLAY%",   brightnessMode == 1 ? "block" : "none");

      // 夜间模式变量渲染
      html.replace("%NM_ON%",  nightModeEnabled ? "selected" : "");
      html.replace("%NM_OFF%", !nightModeEnabled ? "selected" : "");
      html.replace("%NM_BOX_DISPLAY%", nightModeEnabled ? "block" : "none");
      html.replace("%NM_SH%", String(nightStartHour));
      html.replace("%NM_EH%", String(nightEndHour));
      html.replace("%NM_SCALE%", String(nightBrightnessScale));

      html.replace("%PALETTE_DISPLAY%", currentTheme == 5 ? "flex" : "none");
      html.replace("%PC_MIN%", String(autoPowerCycleMinutes));
      html.replace("%PC_SEC%", String(powerOffSeconds));
      html.replace("%PC_HV%", String(highVoltageThreshold, 1));
      html.replace("%PC_MUL%", String(highVoltageMultiplier));

      unlockState();
      webServer->send(200, "text/html", html);
    });

    // ── 手动灯板硬件电源循环：只切断 DC-DC 输入，不重启 ESP32 ──
    webServer->on("/clock/refresh", HTTP_GET, [this]() {
      if (requestPowerCycle()) {
        webServer->sendHeader("Location", "/clock");
        webServer->send(303);
      } else {
        webServer->send(409, "text/plain; charset=utf-8", "灯板电源恢复正在执行，请稍后再试。");
      }
    });

    // ── POST /clock/save ────────────────────────────────────
    webServer->on("/clock/save", HTTP_POST, [this]() {
      lockState();
      if (webServer->hasArg("bmode")) brightnessMode  = webServer->arg("bmode").toInt();
      if (webServer->hasArg("br"))    currentBrightness = constrain(webServer->arg("br").toInt(), 0, 100);
      if (webServer->hasArg("theme")) currentTheme    = webServer->arg("theme").toInt();
      if (webServer->hasArg("mode"))  displayMode     = webServer->arg("mode").toInt();
      if (webServer->hasArg("sec"))   showSeconds     = webServer->arg("sec").toInt() == 1;
      if (webServer->hasArg("colon_blink")) colonBlink = webServer->arg("colon_blink").toInt() == 1;
      if (webServer->hasArg("batt"))  batteryType     = webServer->arg("batt").toInt();

      // 夜间模式解析
      if (webServer->hasArg("nm_en")) nightModeEnabled = webServer->arg("nm_en").toInt() == 1;
      if (webServer->hasArg("nm_sh")) nightStartHour   = constrain(webServer->arg("nm_sh").toInt(), 0, 23);
      if (webServer->hasArg("nm_eh")) nightEndHour     = constrain(webServer->arg("nm_eh").toInt(), 0, 23);
      if (webServer->hasArg("nm_scale")) nightBrightnessScale = constrain(webServer->arg("nm_scale").toInt(), 5, 100);

      if (webServer->hasArg("color")) {
        String hex  = webServer->arg("color");
        long number = strtol(&hex[1], NULL, 16);
        colorR = number >> 16; colorG = (number >> 8) & 0xFF; colorB = number & 0xFF;
      }
      
      // 设置自动硬件断电周期；改动任一恢复参数后重新开始计时。
      bool powerSettingsChanged = false;
      if (webServer->hasArg("pc_min")) {
        autoPowerCycleMinutes = constrain(webServer->arg("pc_min").toInt(), 0, 1440);
        powerSettingsChanged = true;
      }
      if (webServer->hasArg("pc_sec")) {
        powerOffSeconds = constrain(webServer->arg("pc_sec").toInt(), 1, 10);
        powerSettingsChanged = true;
      }
      if (webServer->hasArg("pc_hv")) {
        highVoltageThreshold = constrain(webServer->arg("pc_hv").toFloat(), 0.0f, 80.0f);
        powerSettingsChanged = true;
      }
      if (webServer->hasArg("pc_mul")) {
        highVoltageMultiplier = constrain(webServer->arg("pc_mul").toInt(), 1, 10);
        powerSettingsChanged = true;
      }
      scheduleCycleMinutes = autoPowerCycleMinutes;
      scheduleOffSeconds = powerOffSeconds;
      scheduleHighVoltageThreshold = highVoltageThreshold;
      scheduleHighVoltageMultiplier = highVoltageMultiplier;
      if (powerSettingsChanged) lastPowerCycleMs = millis();

      // 不在 HTTP 请求处理期间执行 NeoPixel show；让独立灯板任务在下一帧应用设置。
      lastEffBr = -1;
      lastRefresh = 0;
      saveSettings();
      unlockState();

      webServer->sendHeader("Location", "/clock");
      webServer->send(303);
    });

    // 独立于 main loop 和 MatrixTask 的硬件电源恢复定时器。
    BaseType_t powerTaskResult = xTaskCreate(
      &SmartMatrixClock::powerCycleTaskEntry,
      "matrixPowerCycle", 3072, this, 3, &powerTaskHandle
    );
    if (powerTaskResult == pdPASS) {
      Serial.println("[MatrixPowerTask] 独立定时任务创建成功");
    } else {
      Serial.println("[MatrixPowerTask] 创建失败；自动硬件断电恢复不可用，请检查剩余 RAM");
    }
  }

  void loop() {
    // 物理电源循环由独立 PowerCycleTask 执行。显示任务仅在电源恢复后重建驱动、重绘。
    if (driverRefreshRequested) {
      lockState();
      if (driverRefreshRequested) {
        driverRefreshRequested = false;
        if (recreateMatrixDriver()) {
          const int effBr = getEffectiveBrightness();
          matrix->setBrightness(effBr);
          lastEffBr = effBr;
          matrix->fillScreen(0);
          matrix->show();
          delay(5);
          updateDisplay();
          lastRefresh = millis();
          Serial.printf("[MatrixPower] 灯板驱动已重建并绘制当前时钟，heap=%u\n",
                        (unsigned)ESP.getFreeHeap());
        }
      }
      unlockState();
    }

    lockState();
    const unsigned long now = millis();
    // 保留原有 250ms 刷新周期，不增加平时的刷新频率。
    if (matrix != nullptr && now - lastRefresh >= 250UL) {
      lastRefresh = now;
      updateDisplay();
    }
    unlockState();
  }
};

#endif