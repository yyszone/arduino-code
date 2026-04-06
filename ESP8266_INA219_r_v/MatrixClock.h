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

const char CLOCK_HTML_TEMPLATE[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset='utf-8'><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>独立时钟控制台</title><style>
body { font-family: 'Segoe UI', sans-serif; background: #0f0f0f; color: #fff; padding: 20px; display: flex; justify-content: center; flex-direction: column; align-items: center;}
.card { background: #1a1a1a; padding: 30px; border-radius: 15px; width: 100%; max-width: 400px; box-shadow: 0 8px 30px rgba(0,0,0,0.9);}
h2 { text-align: center; color: #ff007f; letter-spacing: 2px;}
label { display: block; margin-top: 15px; font-size: 13px; color: #aaa;}
input, select, button { width: 100%; margin-top: 5px; padding: 12px; border-radius: 8px; border: none; box-sizing: border-box; font-size: 15px;}
input[type=number], select { background: #2a2a2a; color: #fff; }
input[type=color] { padding: 0; height: 45px; cursor: pointer; }
button { background: linear-gradient(90deg, #ff007f, #7f00ff); color: #fff; font-weight: bold; margin-top: 25px; cursor: pointer; transition: 0.3s;}
button:hover { opacity: 0.8; }
.back-btn { background: #333; margin-top: 15px; text-align: center; display: block; text-decoration: none; color: #bbb; padding: 12px; border-radius: 8px;}
</style></head><body>
<div class='card'><h2>✨ 独立时钟控制台</h2>
<form action='/clock/save' method='POST'>
  <label>🖥️ 显示模式:</label>
  <select name='mode'>
    <option value='0' %MD_0%>🕒 仅显示时间</option>
    <option value='1' %MD_1%>📅 仅显示日期</option>
    <option value='2' %MD_2%>🕒+📅 同屏双显 (MM.DD HH:MM)</option>
    <option value='3' %MD_3%>🔄 自动交替 (时间+日期滚动)</option>
  </select>
  <label>🎨 色彩主题:</label>
  <select name='theme'>
    <option value='2' %TH_2%>🌈 流光幻彩 (RGB流水)</option>
    <option value='4' %TH_4%>🔥 赛博火焰 (红橙渐变)</option>
    <option value='3' %TH_3%>🌅 迈阿密风 (粉青双拼)</option>
    <option value='1' %TH_1%>🔴 赛博三分色 (区分模块)</option>
    <option value='0' %TH_0%>⚪ 极简单色 (使用下方自定义)</option>
  </select>
  <label>🎨 自定义纯色:</label><input type='color' name='color' value='%COLOR_HEX%'>
  <label>☀️ 屏幕亮度 (1-100):</label><input type='number' name='br' min='1' max='100' value='%BRIGHTNESS%'>
  <label>⏱️ 秒数显示:</label>
  <select name='sec'><option value='1' %SEC_ON%>开启 (时:分:秒)</option><option value='0' %SEC_OFF%>关闭 (时:分)</option></select>
  <button type='submit'>💾 永久保存设置</button>
</form>
<a href='/' class='back-btn'>🔙 返回继电器主页</a>
</div></body></html>
)=====";

class SmartMatrixClock {
private:
  Adafruit_NeoMatrix* matrix;
  ESP8266WebServer* webServer;
  
  int clockPin;
  int currentBrightness = 20;     
  bool showSeconds = true;        
  int currentTheme = 2;       
  int displayMode = 3;        
  uint8_t colorR = 0, colorG = 255, colorB = 255; 

  bool isShowingTime = true;
  unsigned long lastSwitchTime = 0;
  unsigned long lastRefresh = 0;

  // --- 永久记忆存储功能 ---
  void loadSettings() {
    if (LittleFS.exists("/clock_cfg.txt")) {
      File f = LittleFS.open("/clock_cfg.txt", "r");
      if (f) {
        String val;
        if(f.available()) { val = f.readStringUntil('\n'); val.trim(); if(val.length()) currentBrightness = val.toInt(); }
        if(f.available()) { val = f.readStringUntil('\n'); val.trim(); if(val.length()) currentTheme = val.toInt(); }
        if(f.available()) { val = f.readStringUntil('\n'); val.trim(); if(val.length()) displayMode = val.toInt(); }
        if(f.available()) { val = f.readStringUntil('\n'); val.trim(); if(val.length()) showSeconds = (val == "1"); }
        if(f.available()) { val = f.readStringUntil('\n'); val.trim(); if(val.length()) colorR = val.toInt(); }
        if(f.available()) { val = f.readStringUntil('\n'); val.trim(); if(val.length()) colorG = val.toInt(); }
        if(f.available()) { val = f.readStringUntil('\n'); val.trim(); if(val.length()) colorB = val.toInt(); }
        f.close();
        Serial.println("[Clock] 设置读取成功！");
      }
    }
  }

  void saveSettings() {
    File f = LittleFS.open("/clock_cfg.txt", "w");
    if (f) {
      f.println(currentBrightness);
      f.println(currentTheme);
      f.println(displayMode);
      f.println(showSeconds ? "1" : "0");
      f.println(colorR);
      f.println(colorG);
      f.println(colorB);
      f.close();
      Serial.println("[Clock] 设置保存成功！");
    }
  }

  uint16_t getPixelColor(int x, uint16_t baseColor) {
    if (currentTheme == 2) {
      uint32_t rgb = Adafruit_NeoPixel::ColorHSV((millis() * 15) + (x * 2000), 255, 255);
      return matrix->Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    } else if (currentTheme == 4) {
      uint8_t red = 255;
      uint8_t green = map(x, 0, 31, 20, 150); 
      return matrix->Color(red, green, 0);
    }
    return baseColor;
  }

  void drawDigit(int x, int y, int digit, uint16_t baseC) {
    for (int row = 0; row < 6; row++) {
      for (int col = 0; col < 3; col++) {
        if (font3x6[digit][row] & (1 << (2 - col))) {
          matrix->drawPixel(x + col, y + row, getPixelColor(x + col, baseC));
        }
      }
    }
  }

  void drawColon(int x, int y, uint16_t baseC) {
    matrix->drawPixel(x, y + 1, getPixelColor(x, baseC));
    matrix->drawPixel(x, y + 4, getPixelColor(x, baseC));
  }

  void drawDot(int x, int y, uint16_t baseC) {
    matrix->drawPixel(x, y + 4, getPixelColor(x, baseC)); 
  }

  void updateDisplay() {
    matrix->fillScreen(0); 
    
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
    uint16_t c_h = c_main, c_m = c_main, c_s = c_main, c_c = c_main;

    if (currentTheme == 1) { 
      c_h = matrix->Color(0, 255, 255); c_m = matrix->Color(255, 0, 255); c_s = matrix->Color(255, 255, 0); c_c = matrix->Color(255, 255, 255);
    } else if (currentTheme == 3) { 
      c_h = matrix->Color(255, 0, 127); c_m = matrix->Color(0, 255, 255); c_s = matrix->Color(255, 0, 127); c_c = matrix->Color(255, 255, 255);
    }

    int currentMode = displayMode;
    if (displayMode == 3) {
      if (isShowingTime && millis() - lastSwitchTime > 8000) { 
        isShowingTime = false; lastSwitchTime = millis();
      } else if (!isShowingTime && millis() - lastSwitchTime > 3000) { 
        isShowingTime = true; lastSwitchTime = millis();
      }
      currentMode = isShowingTime ? 0 : 1;
    }

    if (currentMode == 0) {
      if (showSeconds) {
        drawDigit(2, y, h/10, c_h); drawDigit(6, y, h%10, c_h);
        if(blink) drawColon(10, y, c_c);
        drawDigit(12, y, m/10, c_m); drawDigit(16, y, m%10, c_m);
        if(blink) drawColon(20, y, c_c);
        drawDigit(22, y, s/10, c_s); drawDigit(26, y, s%10, c_s);
      } else {
        drawDigit(7, y, h/10, c_h); drawDigit(11, y, h%10, c_h);
        if(blink) drawColon(15, y, c_c);
        drawDigit(17, y, m/10, c_m); drawDigit(21, y, m%10, c_m);
      }
    }
    else if (currentMode == 1) {
      // ==== 修复：获取年份的最后两位 (2026 -> 26) ====
      int shortYear = Y % 100;
      drawDigit(1, y, shortYear / 10, c_h); // 十位 (2)
      drawDigit(5, y, shortYear % 10, c_h); // 个位 (6)
      drawDot(9, y, c_c); 
      drawDigit(11, y, M/10, c_m); drawDigit(15, y, M%10, c_m); 
      drawDot(19, y, c_c); 
      drawDigit(21, y, D/10, c_s); drawDigit(25, y, D%10, c_s); 
    }
    else if (currentMode == 2) {
      drawDigit(1, y, M/10, c_h); drawDigit(5, y, M%10, c_h);
      drawDot(8, y, c_c);
      drawDigit(9, y, D/10, c_m); drawDigit(13, y, D%10, c_m);
      drawDigit(17, y, h/10, c_s); drawDigit(21, y, h%10, c_s);
      if(blink) drawColon(24, y, c_c);
      drawDigit(25, y, m/10, c_main); drawDigit(29, y, m%10, c_main);
    }
    matrix->show();
  }

public:
  SmartMatrixClock(int pin, ESP8266WebServer& srv) {
    clockPin = pin;
    webServer = &srv;
    matrix = new Adafruit_NeoMatrix(32, 8, clockPin,
      NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
      NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
      NEO_GRB            + NEO_KHZ800);
  }

  void begin() {
    // 初始化时从 LittleFS 读取记忆
    loadSettings();

    matrix->begin();
    matrix->setTextWrap(false);
    matrix->setBrightness(currentBrightness);
    matrix->fillScreen(0);
    matrix->show();

    // NTP 时间配置
    configTime(8 * 3600, 0, "ntp.aliyun.com");

    // 绑定时钟专用路由
    webServer->on("/clock", HTTP_GET, [this]() {
      String html = FPSTR(CLOCK_HTML_TEMPLATE);
      char hexColor[8]; sprintf(hexColor, "#%02x%02x%02x", colorR, colorG, colorB);
      html.replace("%COLOR_HEX%", String(hexColor));
      html.replace("%BRIGHTNESS%", String(currentBrightness));
      html.replace("%MD_0%", displayMode == 0 ? "selected" : "");
      html.replace("%MD_1%", displayMode == 1 ? "selected" : "");
      html.replace("%MD_2%", displayMode == 2 ? "selected" : "");
      html.replace("%MD_3%", displayMode == 3 ? "selected" : "");
      html.replace("%TH_2%", currentTheme == 2 ? "selected" : "");
      html.replace("%TH_4%", currentTheme == 4 ? "selected" : "");
      html.replace("%TH_3%", currentTheme == 3 ? "selected" : "");
      html.replace("%TH_1%", currentTheme == 1 ? "selected" : "");
      html.replace("%TH_0%", currentTheme == 0 ? "selected" : "");
      html.replace("%SEC_ON%", showSeconds ? "selected" : "");
      html.replace("%SEC_OFF%", !showSeconds ? "selected" : "");
      webServer->send(200, "text/html", html);
    });

    webServer->on("/clock/save", HTTP_POST, [this]() {
      if (webServer->hasArg("br")) currentBrightness = webServer->arg("br").toInt();
      if (webServer->hasArg("theme")) currentTheme = webServer->arg("theme").toInt();
      if (webServer->hasArg("mode")) displayMode = webServer->arg("mode").toInt();
      if (webServer->hasArg("sec")) showSeconds = webServer->arg("sec").toInt() == 1;
      if (webServer->hasArg("color")) {
        String hex = webServer->arg("color");
        long number = strtol(&hex[1], NULL, 16);
        colorR = number >> 16; colorG = number >> 8 & 0xFF; colorB = number & 0xFF;
      }
      
      matrix->setBrightness(currentBrightness);
      saveSettings(); // 保存更改到 Flash 中，永久记忆

      webServer->sendHeader("Location", "/clock"); 
      webServer->send(303);
    });
  }

  void loop() {
    if (millis() - lastRefresh >= 30) { 
      lastRefresh = millis();
      updateDisplay();
    }
  }
};

#endif