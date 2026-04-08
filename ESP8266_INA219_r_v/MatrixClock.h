#ifndef MATRIX_CLOCK_H
#define MATRIX_CLOCK_H

#include <ESP8266WebServer.h>
#include <time.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

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
.card { background: #1a1a1a; padding: 30px; border-radius: 15px; width: 100%; max-width: 420px; box-shadow: 0 8px 30px rgba(0,0,0,0.9); }
h2 { text-align: center; color: #ff007f; letter-spacing: 2px; margin-top: 0; }
label { display: block; margin-top: 15px; font-size: 13px; color: #aaa; }
input, select, button { width: 100%; margin-top: 5px; padding: 12px; border-radius: 8px; border: none; box-sizing: border-box; font-size: 15px; }
input[type=number], select { background: #2a2a2a; color: #fff; }
input[type=color] { padding: 0; height: 45px; cursor: pointer; }
button { background: linear-gradient(90deg, #ff007f, #7f00ff); color: #fff; font-weight: bold; margin-top: 25px; cursor: pointer; transition: 0.3s; }
button:hover { opacity: 0.8; }
.back-btn { background: #333; margin-top: 15px; text-align: center; display: block; text-decoration: none; color: #bbb; padding: 12px; border-radius: 8px; }
.stat-row { display: flex; justify-content: space-between; align-items: center; background: #111; border-radius: 8px; padding: 10px 14px; margin-top: 10px; font-size: 13px; }
.stat-label { color: #888; }
.stat-value { color: #fff; font-weight: bold; font-size: 15px; }
.batt-bar-wrap { background: #222; border-radius: 6px; height: 14px; margin-top: 6px; overflow: hidden; }
.batt-bar-fill { height: 100%; border-radius: 6px; transition: width 0.5s ease, background 0.5s ease; }
.divider { border-top: 1px solid #2a2a2a; margin: 18px 0; }
.sub-hint { font-size: 11px; color: #555; margin-top: 4px; }
.br-manual-block { transition: opacity 0.2s; }
.palette-preview { display: flex; gap: 4px; margin-top: 8px; border-radius: 6px; overflow: hidden; }
.palette-swatch { flex: 1; height: 16px; }
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

  <div class="divider"></div>

  <form action='/clock/save' method='POST'>
    <label>🖥️ 显示模式:</label>
    <select name='mode'>
      <option value='0' %MD_0%>🕒 仅显示时间</option>
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

    <!-- 数字彩虹主题色板预览 -->
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
    <label>☀️ 亮度模式:</label>
    <select name='bmode' id='bmodeSelect' onchange="toggleBrMode(this.value)">
      <option value='0' %BM_0%>🔧 手动亮度</option>
      <option value='1' %BM_1%>🔋 自动亮度 (跟随电量百分比)</option>
    </select>

    <div class="br-manual-block" id="brManualBlock" style="display:%BR_MANUAL_DISPLAY%">
      <label>☀️ 手动亮度 (0=关闭, 1-100):</label>
      <input type='number' name='br' id='brInput' min='0' max='100' value='%BRIGHTNESS%'>
      <div class="sub-hint">0 = 完全关闭屏幕；1-100 = 亮度等级</div>
    </div>
    <div id="brAutoHint" style="display:%BR_AUTO_DISPLAY%; font-size:12px; color:#888; margin-top:8px; padding:8px; background:#111; border-radius:6px;">
      🔋 自动模式：电量 0% → 最低亮度 5，电量 100% → 亮度 100<br>
      电量低于 5% 时屏幕自动关闭。
    </div>

    <div class="divider"></div>

    <label>⏱️ 秒数显示:</label>
    <select name='sec'>
      <option value='1' %SEC_ON%>开启 (时:分:秒)</option>
      <option value='0' %SEC_OFF%>关闭 (时:分)</option>
    </select>

    <div class="divider"></div>

    <label>🔋 电池类型 (采用智能曲线算法计算电量):</label>
    <select name='batt'>
      <option value='0' %BT_0%>12V 磷酸铁锂 (放电曲线精确匹配)</option>
      <option value='1' %BT_1%>24V 磷酸铁锂 (放电曲线精确匹配)</option>
    </select>
    <div class="sub-hint">※ 最下行像素条实时显示电量，颜色：绿&gt;60% / 黄&gt;30% / 红≤30%</div>

    <button type='submit'>💾 永久保存设置</button>
  </form>
  <a href='/' class='back-btn'>🔙 返回继电器主页</a>
</div>

<script>
function toggleBrMode(val) {
  var manual = document.getElementById('brManualBlock');
  var hint   = document.getElementById('brAutoHint');
  if (val === '0') { manual.style.display = 'block'; hint.style.display = 'none'; }
  else             { manual.style.display = 'none';  hint.style.display = 'block'; }
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
  ESP8266WebServer*   webServer;

  int   clockPin;
  int   currentBrightness = 20;   // 0-100 (0=off)
  int   brightnessMode    = 0;    // 0=手动, 1=自动(跟随电量)
  bool  showSeconds       = true;
  int   currentTheme      = 2;
  int   displayMode       = 3;
  uint8_t colorR = 0, colorG = 255, colorB = 255;

  // 电池
  int   batteryType    = 0;       // 0=12V LiFePO4, 1=24V LiFePO4
  float batteryVoltage = 0.0f;

  bool  isShowingTime  = true;
  unsigned long lastSwitchTime = 0;
  unsigned long lastRefresh    = 0;

  // ── 每位数字独立颜色 (主题 5: 0~9 各一色) ───────────────
  uint16_t getDigitColor(int digit) {
    switch (digit) {
      case 0: return matrix->Color(255,  50,  50); // 红
      case 1: return matrix->Color(255, 140,   0); // 橙
      case 2: return matrix->Color(255, 215,   0); // 黄
      case 3: return matrix->Color(  0, 224,  80); // 绿
      case 4: return matrix->Color(  0, 220, 200); // 青
      case 5: return matrix->Color(  0, 150, 255); // 蓝
      case 6: return matrix->Color( 80,   0, 255); // 靛
      case 7: return matrix->Color(192,   0, 255); // 紫
      case 8: return matrix->Color(255,   0, 180); // 粉
      case 9: return matrix->Color(220, 220, 220); // 白
      default: return matrix->Color(255, 255, 255);
    }
  }

  // ── 【重大优化】精准电量百分比计算（磷酸铁锂真实放电曲线） ──────────
  int getBatteryPercent() {
    if (batteryVoltage < 0.5f) return 0; // 未连接电池

    // 巧妙算法：如果是 24V(8串)，直接将读数除以 2，套用 12V(4串) 的对照表
    float v = batteryVoltage;
    if (batteryType == 1) { 
      v = v / 2.0f; 
    }

    // 保护限制：极高或极低电压直接返回
    if (v >= 13.6f) return 100; // 满电静置电压约为 13.6V (充电中为14.4V)
    if (v <= 11.0f) return 0;   // 磷酸铁锂低于 11.0V 基本完全空载

    // 磷酸铁锂电压 -> 电量 对应表 (分段插值法)
    // 根据 LiFePO4 特性，平台期在 13.3V ~ 12.8V 之间
    const float curve[11][2] = {
      {13.6, 100},
      {13.4, 90},
      {13.3, 80},
      {13.2, 70},
      {13.1, 60},
      {13.0, 50},
      {12.9, 40},
      {12.8, 30},
      {12.5, 20},
      {12.0, 10},
      {11.0, 0}
    };

    // 遍历曲线，找出当前电压落在哪个区间，进行线性插值计算
    for (int i = 0; i < 10; i++) {
      if (v <= curve[i][0] && v > curve[i+1][0]) {
        float v_high = curve[i][0];
        float p_high = curve[i][1];
        float v_low  = curve[i+1][0];
        float p_low  = curve[i+1][1];
        
        // 数学公式：比例插值
        float exactPct = p_low + (v - v_low) * (p_high - p_low) / (v_high - v_low);
        return (int)exactPct;
      }
    }
    return 0; // 兜底
  }

  // ── 有效亮度计算（考虑自动/手动模式）────────────────────
  int getEffectiveBrightness() {
    int br100; 
    if (brightnessMode == 1) {
      int pct = getBatteryPercent();
      if (pct <= 5) return 0; // 极低电量关闭屏幕
      br100 = map(pct, 5, 100, 5, 100);
    } else {
      br100 = currentBrightness; 
    }
    if (br100 <= 0) return 0;
    return (int)map(br100, 1, 100, 4, 255);
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

    readLine(currentBrightness); 
    readLine(currentTheme);      
    readLine(displayMode);       
    readLineBool(showSeconds);   
    readLineU8(colorR);          
    readLineU8(colorG);          
    readLineU8(colorB);          
    readLine(batteryType);       
    readLine(brightnessMode);    
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

  // ── 运行时长显示（模式 4）────────────────────────────────
  void drawUptime(int y, uint16_t c_h, uint16_t c_m, uint16_t c_s, uint16_t c_c, bool blink) {
    unsigned long upSec = millis() / 1000UL;
    int totalH = (int)(upSec / 3600);
    int upM    = (int)((upSec % 3600) / 60);
    int upS    = (int)(upSec % 60);

    if (totalH < 100) {
      drawDigit(2,  y, totalH / 10, c_h); drawDigit(6,  y, totalH % 10, c_h);
      if (blink) drawColon(10, y, c_c);
      drawDigit(12, y, upM / 10,   c_m); drawDigit(16, y, upM % 10,   c_m);
      if (blink) drawColon(20, y, c_c);
      drawDigit(22, y, upS / 10,   c_s); drawDigit(26, y, upS % 10,   c_s);
    } else {
      int upD  = totalH / 24;
      int upHH = totalH % 24;
      if (upD > 99) upD = 99;
      drawDigit(1,  y, upD  / 10, c_h); drawDigit(5,  y, upD  % 10, c_h);
      drawDot(9, y, c_c);
      drawDigit(11, y, upHH / 10, c_m); drawDigit(15, y, upHH % 10, c_m);
      if (blink) drawColon(19, y, c_c);
      drawDigit(21, y, upM  / 10, c_s); drawDigit(25, y, upM  % 10, c_s);
    }
  }

  // ── 主刷新 ────────────────────────────────────────────────
  void updateDisplay() {
    int effBr = getEffectiveBrightness();
    matrix->setBrightness(effBr);
    matrix->fillScreen(0);

    if (effBr == 0) { 
      matrix->show(); 
      yield(); // 防止关闭屏幕时死机
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

    bool blink = (millis() / 500) % 2;
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

    if (currentMode == 0) {
      if (showSeconds) {
        drawDigit(2,  y, h/10, c_h);    drawDigit(6,  y, h%10, c_h);
        if (blink) drawColon(10, y, c_c);
        drawDigit(12, y, m/10, c_m);    drawDigit(16, y, m%10, c_m);
        if (blink) drawColon(20, y, c_c);
        drawDigit(22, y, s/10, c_s_col); drawDigit(26, y, s%10, c_s_col);
      } else {
        drawDigit(7,  y, h/10, c_h);    drawDigit(11, y, h%10, c_h);
        if (blink) drawColon(15, y, c_c);
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
      if (blink) drawColon(24, y, c_c);
      drawDigit(25, y, m/10, c_main); drawDigit(29, y, m%10, c_main);
    }
    else if (currentMode == 4) {
      drawUptime(y, c_h, c_m, c_s_col, c_c, blink);
    }

    drawBatteryBar();

    matrix->show();

    // 【极其重要】：刷完灯后必须交出控制权，防止看门狗(WDT)判定死机而重启设备！
    yield(); 
  }

  // ── 运行时长格式化（供网页）─────────────────────────────
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
  SmartMatrixClock(int pin, ESP8266WebServer& srv) {
    clockPin  = pin;
    webServer = &srv;
    matrix    = new Adafruit_NeoMatrix(32, 8, clockPin,
      NEO_MATRIX_TOP     + NEO_MATRIX_LEFT  +
      NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
      NEO_GRB            + NEO_KHZ800);
  }

  void setBatteryVoltage(float v) {
    batteryVoltage = v;
  }

  void begin() {
    loadSettings();

    matrix->begin();
    matrix->setTextWrap(false);
    matrix->setBrightness(getEffectiveBrightness());
    matrix->fillScreen(0);
    matrix->show();
    yield(); // 喂狗

    configTime(8 * 3600, 0, "ntp.aliyun.com");

    // ── GET /clock ─────────────────────────────────────────
    webServer->on("/clock", HTTP_GET, [this]() {
      String html = FPSTR(CLOCK_HTML_TEMPLATE);

      html.replace("%UPTIME%", formatUptime());

      // 电池信息 (展示新的精确比例)
      int pct = getBatteryPercent();
      char voltBuf[8]; dtostrf(batteryVoltage, 4, 2, voltBuf);
      html.replace("%BATT_PCT%", String(pct));
      html.replace("%BATT_V%",   String(voltBuf));
      String battColor = (pct > 60) ? "#00c800" : (pct > 30) ? "#c89600" : "#c80000";
      html.replace("%BATT_COLOR%", battColor);

      // 自定义色
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

      html.replace("%BT_0%", batteryType == 0 ? "selected" : "");
      html.replace("%BT_1%", batteryType == 1 ? "selected" : "");

      // 亮度模式
      html.replace("%BM_0%", brightnessMode == 0 ? "selected" : "");
      html.replace("%BM_1%", brightnessMode == 1 ? "selected" : "");
      html.replace("%BR_MANUAL_DISPLAY%", brightnessMode == 0 ? "block" : "none");
      html.replace("%BR_AUTO_DISPLAY%",   brightnessMode == 1 ? "block" : "none");

      // 数字彩虹色板预览
      html.replace("%PALETTE_DISPLAY%", currentTheme == 5 ? "flex" : "none");

      webServer->send(200, "text/html", html);
    });

    // ── POST /clock/save ────────────────────────────────────
    webServer->on("/clock/save", HTTP_POST, [this]() {
      if (webServer->hasArg("bmode")) brightnessMode  = webServer->arg("bmode").toInt();
      // 这里接收页面传来的0-100亮度（如果你加了小风扇，这里可以放开限制；没加风扇建议在输入时悠着点）
      if (webServer->hasArg("br"))    currentBrightness = constrain(webServer->arg("br").toInt(), 0, 100);
      if (webServer->hasArg("theme")) currentTheme    = webServer->arg("theme").toInt();
      if (webServer->hasArg("mode"))  displayMode     = webServer->arg("mode").toInt();
      if (webServer->hasArg("sec"))   showSeconds     = webServer->arg("sec").toInt() == 1;
      if (webServer->hasArg("batt"))  batteryType     = webServer->arg("batt").toInt();
      if (webServer->hasArg("color")) {
        String hex  = webServer->arg("color");
        long number = strtol(&hex[1], NULL, 16);
        colorR = number >> 16; colorG = (number >> 8) & 0xFF; colorB = number & 0xFF;
      }
      matrix->setBrightness(getEffectiveBrightness());
      saveSettings();

      webServer->sendHeader("Location", "/clock");
      webServer->send(303);
    });
  }

  void loop() {
    if (millis() - lastRefresh >= 100) {
      lastRefresh = millis();
      updateDisplay();
    }
  }
};

#endif