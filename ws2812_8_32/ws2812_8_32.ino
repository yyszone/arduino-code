#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <time.h>             // 使用内置库处理日期和时间
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

// ================= 核心配置 =================
const char* ssid = "yang1234";          
const char* password = "y123456789";    

#define PIN 4 // DIN 接 D2 (GPIO4)

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(32, 8, PIN,
  NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB            + NEO_KHZ800);

// ================= 服务与全局变量 =================
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

int currentBrightness = 20;     
int timeZone = 8;               
bool showSeconds = true;        
int currentTheme = 2;       // 0=单色, 1=三分色, 2=幻彩, 3=迈阿密, 4=火焰
int displayMode = 3;        // 0=仅时间, 1=仅日期, 2=同屏双显, 3=交替显示
uint8_t colorR = 0, colorG = 255, colorB = 255; 

// 交替模式使用的状态机变量
bool isShowingTime = true;
unsigned long lastSwitchTime = 0;

// ================= 极简锐利 3x6 字体 =================
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

// ================= 色彩渲染引擎 =================
uint16_t getPixelColor(int x, uint16_t baseColor) {
  if (currentTheme == 2) {
    // 🌈 流光幻彩 (随时间和X坐标变化)
    uint32_t rgb = Adafruit_NeoPixel::ColorHSV((millis() * 15) + (x * 2000), 255, 255);
    return matrix.Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
  } else if (currentTheme == 4) {
    // 🔥 火焰渐变 (红 -> 橙 -> 黄)
    uint8_t red = 255;
    uint8_t green = map(x, 0, 31, 20, 150); // 从红过渡到黄
    return matrix.Color(red, green, 0);
  }
  return baseColor;
}

// 绘制单个数字
void drawDigit(int x, int y, int digit, uint16_t baseC) {
  for (int row = 0; row < 6; row++) {
    for (int col = 0; col < 3; col++) {
      if (font3x6[digit][row] & (1 << (2 - col))) {
        matrix.drawPixel(x + col, y + row, getPixelColor(x + col, baseC));
      }
    }
  }
}

// 绘制冒号和点
void drawColon(int x, int y, uint16_t baseC) {
  matrix.drawPixel(x, y + 1, getPixelColor(x, baseC));
  matrix.drawPixel(x, y + 4, getPixelColor(x, baseC));
}
void drawDot(int x, int y, uint16_t baseC) {
  matrix.drawPixel(x, y + 4, getPixelColor(x, baseC)); // 只画下方一个点
}

// ================= 网页 UI =================
const char* htmlTemplate PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
  <meta charset='utf-8'>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>智能矩阵时钟控制台</title>
  <style>
    body { font-family: 'Segoe UI', sans-serif; background: #0f0f0f; color: #fff; padding: 20px; display: flex; justify-content: center; }
    .card { background: #1a1a1a; padding: 30px; border-radius: 15px; width: 100%; max-width: 400px; box-shadow: 0 8px 30px rgba(0,0,0,0.9);}
    h2 { text-align: center; color: #ff007f; letter-spacing: 2px;}
    label { display: block; margin-top: 15px; font-size: 13px; color: #aaa;}
    input, select, button { width: 100%; margin-top: 5px; padding: 12px; border-radius: 8px; border: none; box-sizing: border-box; font-size: 15px;}
    input[type=number], select { background: #2a2a2a; color: #fff; }
    input[type=color] { padding: 0; height: 45px; cursor: pointer; }
    button { background: linear-gradient(90deg, #ff007f, #7f00ff); color: #fff; font-weight: bold; margin-top: 25px; cursor: pointer; transition: 0.3s;}
    button:hover { opacity: 0.8; }
    .ota-btn { background: #333; color: #bbb; margin-top: 15px; }
  </style>
</head>
<body>
  <div class='card'>
    <h2>✨ 智能点阵时钟</h2>
    <form action='/save' method='POST'>
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

      <label>🎨 自定义纯色 (仅单色模式有效):</label>
      <input type='color' name='color' value='%COLOR_HEX%'>
      
      <label>☀️ 屏幕亮度 (1-100):</label>
      <input type='number' name='br' min='1' max='100' value='%BRIGHTNESS%'>
      
      <label>⏱️ 秒数显示 (仅在"仅显示时间"下有效):</label>
      <select name='sec'>
        <option value='1' %SEC_ON%>开启 (时:分:秒)</option>
        <option value='0' %SEC_OFF%>关闭 (时:分)</option>
      </select>
      
      <button type='submit'>💾 应用设置</button>
    </form>
    <a href='/update'><button class='ota-btn'>☁️ Web OTA 固件升级</button></a>
  </div>
</body>
</html>
)=====";

void handleRoot() {
  String html = FPSTR(htmlTemplate);
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
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("br")) currentBrightness = server.arg("br").toInt();
  if (server.hasArg("theme")) currentTheme = server.arg("theme").toInt();
  if (server.hasArg("mode")) displayMode = server.arg("mode").toInt();
  if (server.hasArg("sec")) showSeconds = server.arg("sec").toInt() == 1;
  
  if (server.hasArg("color")) {
    String hex = server.arg("color");
    long number = strtol(&hex[1], NULL, 16);
    colorR = number >> 16; colorG = number >> 8 & 0xFF; colorB = number & 0xFF;
  }

  matrix.setBrightness(currentBrightness);
  server.sendHeader("Location", "/");
  server.send(303);
}

// ================= 核心渲染引擎 =================
void updateClockDisplay() {
  matrix.fillScreen(0); 
  
  // 获取当前时间
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

  // ----- 主题色彩配置 -----
  uint16_t c_main = matrix.Color(colorR, colorG, colorB);
  uint16_t c_h = c_main, c_m = c_main, c_s = c_main, c_c = c_main;

  if (currentTheme == 1) { // 赛博三分色
    c_h = matrix.Color(0, 255, 255); c_m = matrix.Color(255, 0, 255); c_s = matrix.Color(255, 255, 0); c_c = matrix.Color(255, 255, 255);
  } else if (currentTheme == 3) { // 迈阿密风 (粉青交替)
    c_h = matrix.Color(255, 0, 127); c_m = matrix.Color(0, 255, 255); c_s = matrix.Color(255, 0, 127); c_c = matrix.Color(255, 255, 255);
  }

  // ----- 自动交替逻辑 -----
  int currentMode = displayMode;
  if (displayMode == 3) {
    if (isShowingTime && millis() - lastSwitchTime > 8000) { // 时间显8秒
      isShowingTime = false; lastSwitchTime = millis();
    } else if (!isShowingTime && millis() - lastSwitchTime > 3000) { // 日期显3秒
      isShowingTime = true; lastSwitchTime = millis();
    }
    currentMode = isShowingTime ? 0 : 1;
  }

  // ----- 渲染模式 0: 仅显示时间 -----
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
  // ----- 渲染模式 1: 仅显示日期 YYYY.MM.DD -----
  else if (currentMode == 1) {
    drawDigit(1, y, (Y/100)%10, c_h); drawDigit(5, y, Y%10, c_h); // 24
    drawDot(9, y, c_c); // .
    drawDigit(11, y, M/10, c_m); drawDigit(15, y, M%10, c_m); // 12
    drawDot(19, y, c_c); // .
    drawDigit(21, y, D/10, c_s); drawDigit(25, y, D%10, c_s); // 31
  }
  // ----- 渲染模式 2: 同屏双显 MM.DD HH:MM -----
  else if (currentMode == 2) {
    // 日期部分
    drawDigit(1, y, M/10, c_h); drawDigit(5, y, M%10, c_h);
    drawDot(8, y, c_c);
    drawDigit(9, y, D/10, c_m); drawDigit(13, y, D%10, c_m);
    // 时间部分 (缩进)
    drawDigit(17, y, h/10, c_s); drawDigit(21, y, h%10, c_s);
    if(blink) drawColon(24, y, c_c);
    drawDigit(25, y, m/10, c_main); drawDigit(29, y, m%10, c_main);
  }

  matrix.show();
}

// ================= 初始化 =================
void setup() {
  Serial.begin(115200);
  delay(100);
  
  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(currentBrightness);
  matrix.fillScreen(0);
  matrix.show();

  // 1. 连接 WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  Serial.println("\n\n==========================");
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  int loadX = 0;
  while (WiFi.status() != WL_CONNECTED) {
    matrix.fillScreen(0);
    matrix.drawPixel(loadX % 32, 4, matrix.Color(0, 150, 255)); 
    matrix.show();
    delay(100);
    loadX++;
    Serial.print(".");
  }
  
  // 💥 关键点：网络连接成功，高亮打印 IP 地址
  Serial.println("\n==========================");
  Serial.println("  WIFI CONNECTED SUCCESSFULLY!");
  Serial.print("  >>> YOUR IP ADDRESS: ");
  Serial.println(WiFi.localIP());
  Serial.println("==========================\n");

  // 2. 配置内置 NTP 时间同步 (包含日期)
  configTime(timeZone * 3600, 0, "ntp.aliyun.com", "time.pool.aliyun.com");
  
  matrix.fillScreen(0);
  matrix.drawPixel(15, 3, matrix.Color(0, 255, 0)); 
  matrix.drawPixel(16, 3, matrix.Color(0, 255, 0));
  matrix.show();
  delay(1000);

  // 3. 启动 Web 服务
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  httpUpdater.setup(&server);
  server.begin();
}

// ================= 主循环 =================
void loop() {
  server.handleClient();
  
  static unsigned long lastRefresh = 0;
  if (millis() - lastRefresh >= 30) { 
    lastRefresh = millis();
    updateClockDisplay();
  }
}