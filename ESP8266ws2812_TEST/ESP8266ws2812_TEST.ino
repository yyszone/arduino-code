/*
 * ╔══════════════════════════════════════════════════════════╗
 *  32x8 WS2812B 矩阵时钟 —— ESP8266 终极版
 *  修复：上下翻转（XY映射 ly = 7 - y）
 *  功能：NTP 时间 · 冒号闪烁 · 底部秒条 · OTA · Web
 * ╚══════════════════════════════════════════════════════════╝
 */

#define FASTLED_ALLOW_INTERRUPTS 0
#include <FastLED.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ==================== 配置区 ====================
const char* ssid     = "yang1234";
const char* password = "y123456789";

#define LED_PIN     2
#define NUM_LEDS    256
#define MATRIX_W    32
#define MATRIX_H    8
#define BRIGHTNESS  25       // 亮度 0-255
// ================================================

CRGB leds[NUM_LEDS];
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600, 30000);
ESP8266WebServer server(80);

// ──────────────────────────────────────────────
//  5×6 字体（粗体饱满）
// ──────────────────────────────────────────────
const uint8_t FONT[10][6] = {
  { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 }, // 0
  { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110 }, // 1
  { 0b01110, 0b10001, 0b00010, 0b00100, 0b01000, 0b11111 }, // 2
  { 0b01110, 0b10001, 0b00110, 0b00001, 0b10001, 0b01110 }, // 3
  { 0b10001, 0b10001, 0b11111, 0b00001, 0b00001, 0b00001 }, // 4
  { 0b11111, 0b10000, 0b11110, 0b00001, 0b10001, 0b01110 }, // 5
  { 0b01110, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 }, // 6
  { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000 }, // 7
  { 0b01110, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 }, // 8
  { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110 }, // 9
};

// ──────────────────────────────────────────────
//  XY 坐标 → LED 索引
//  ★ 修复翻转：ly = 7 - y（上下镜像修正）
// ──────────────────────────────────────────────
uint16_t XY(int x, int y) {
  if (x < 0 || x >= MATRIX_W || y < 0 || y >= MATRIX_H)
    return NUM_LEDS; // 越界保护

  uint8_t panel = 3 - (x / 8);       // 面板顺序（右→左串联）
  uint8_t lx    = x % 8;
  uint8_t ly    = 7 - y;             // ★ 上下翻转修正

  return (uint16_t)panel * 64 + ly * 8 + (7 - lx);
}

void setPixel(int x, int y, CRGB c) {
  uint16_t i = XY(x, y);
  if (i < NUM_LEDS) leds[i] = c;
}

// ──────────────────────────────────────────────
//  绘制数字（5×6）
// ──────────────────────────────────────────────
void drawDigit(int sx, int sy, uint8_t d, CRGB c) {
  if (d > 9) return;
  for (int r = 0; r < 6; r++) {
    uint8_t bits = FONT[d][r];
    for (int col = 0; col < 5; col++)
      if (bits & (1 << (4 - col)))
        setPixel(sx + col, sy + r, c);
  }
}

// ──────────────────────────────────────────────
//  绘制冒号（2×2 大点，垂直居中）
// ──────────────────────────────────────────────
void drawColon(int sx, CRGB c) {
  // 上点 y=1~2，下点 y=4~5
  setPixel(sx,   1, c); setPixel(sx+1, 1, c);
  setPixel(sx,   2, c); setPixel(sx+1, 2, c);
  setPixel(sx,   4, c); setPixel(sx+1, 4, c);
  setPixel(sx,   5, c); setPixel(sx+1, 5, c);
}

// ──────────────────────────────────────────────
//  绘制秒进度条（最底行 y=7）
// ──────────────────────────────────────────────
void drawSecondsBar(uint8_t sec, CRGB c) {
  int lit = map(sec, 0, 59, 1, 32);
  for (int x = 0; x < MATRIX_W; x++)
    setPixel(x, 7, x < lit ? c : CRGB::Black);
}

// ──────────────────────────────────────────────
//  整屏时钟渲染
//
//  布局（32列）：
//   x= 3  十位小时（5宽）
//   x= 9  个位小时（5宽）
//   x=15  冒号（2宽）
//   x=18  十位分钟（5宽）
//   x=24  个位分钟（5宽）
//   y= 0~5 字符区
//   y= 7   秒进度条
// ──────────────────────────────────────────────
void drawClock(uint8_t h, uint8_t m, uint8_t s) {
  FastLED.clear();

  CRGB cTime  = CRGB(255, 200, 0);   // 金黄
  CRGB cColon = CRGB(255, 100, 0);   // 橙
  CRGB cBar   = CRGB(0,   160, 255); // 蓝

  drawDigit( 3, 0, h / 10, cTime);
  drawDigit( 9, 0, h % 10, cTime);

  if (s % 2 == 0)                     // 每秒闪烁
    drawColon(15, cColon);

  drawDigit(18, 0, m / 10, cTime);
  drawDigit(24, 0, m % 10, cTime);

  drawSecondsBar(s, cBar);

  FastLED.show();
}

// ──────────────────────────────────────────────
//  WiFi 连接动画
// ──────────────────────────────────────────────
void wifiAnim() {
  static int pos = 0;
  FastLED.clear();
  for (int i = 0; i < 4; i++)
    setPixel((pos + i * 4) % MATRIX_W, 3, CRGB(0, 200, 50));
  FastLED.show();
  pos = (pos + 1) % MATRIX_W;
  delay(50);
}

// ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== 矩阵时钟启动 ===");

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS)
         .setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);

  // 上电指示：LED0 亮红
  leds[0] = CRGB::Red;
  FastLED.show();

  // 连接 WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("连接 WiFi: %s ", ssid);
  while (WiFi.status() != WL_CONNECTED) {
    wifiAnim();
    Serial.print(".");
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  // 连接成功：LED0 亮绿
  leds[0] = CRGB::Green;
  FastLED.show();
  delay(500);

  // NTP 同步
  timeClient.begin();
  timeClient.update();
  Serial.printf("时间: %02d:%02d:%02d\n",
    timeClient.getHours(), timeClient.getMinutes(), timeClient.getSeconds());

  // Web 服务器
  server.on("/", []() {
    String h = String(timeClient.getHours());
    String m = String(timeClient.getMinutes());
    server.send(200, "text/html; charset=UTF-8",
      "<meta charset='UTF-8'>"
      "<h2>矩阵时钟运行中</h2>"
      "<p>当前时间：" + h + ":" + m + "</p>"
      "<p>IP：" + WiFi.localIP().toString() + "</p>");
  });
  server.begin();

  // OTA
  ArduinoOTA.setHostname("esp-matrix");
  ArduinoOTA.begin();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  timeClient.update();

  uint8_t h = timeClient.getHours();
  uint8_t m = timeClient.getMinutes();
  uint8_t s = timeClient.getSeconds();

  static uint8_t lastS = 255;
  if (s != lastS) {
    lastS = s;
    drawClock(h, m, s);
  }
}
