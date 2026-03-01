// 【最核心修复】必须放在 FastLED.h 之前！禁止 WiFi 中断打乱灯管时序，解决颜色发紫发乱的问题！
#define FASTLED_ALLOW_INTERRUPTS 0
#include <FastLED.h>

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ==================== 用户配置区 ====================
const char* ssid = "yang1234";         // 换成你的 WiFi 名称
const char* password = "y123456789";   // 换成你的 WiFi 密码

#define LED_PIN     2       // 数据引脚接 D4 (GPIO2)
#define NUM_LEDS    256     // 4块 8x8 = 32x8 = 256 颗灯珠
#define MATRIX_W    32      // 屏幕总宽度
#define MATRIX_H    8       // 屏幕总高度
#define BRIGHTNESS  15      // 亮度 (0-255，强烈建议保持 15 测试)

// ==================== 全局对象 ====================
CRGB leds[NUM_LEDS];
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600); // 阿里云NTP，东八区
ESP8266WebServer server(80); // 加入了网页服务器，这样你访问 IP 就不会报错了

// ==================== 像素字体 (4x5) ====================
const uint8_t font4x5[10][5] = {
  { 0b0110, 0b1001, 0b1001, 0b1001, 0b0110 }, // 0
  { 0b0010, 0b0110, 0b0010, 0b0010, 0b0111 }, // 1
  { 0b1110, 0b0001, 0b0110, 0b1000, 0b1111 }, // 2
  { 0b1110, 0b0001, 0b0110, 0b0001, 0b1110 }, // 3
  { 0b1001, 0b1001, 0b1111, 0b0001, 0b0001 }, // 4
  { 0b1111, 0b1000, 0b1110, 0b0001, 0b1110 }, // 5
  { 0b0110, 0b1000, 0b1110, 0b1001, 0b0110 }, // 6
  { 0b1111, 0b0001, 0b0010, 0b0100, 0b0100 }, // 7
  { 0b0110, 0b1001, 0b0110, 0b1001, 0b0110 }, // 8
  { 0b0110, 0b1001, 0b0111, 0b0001, 0b0110 }  // 9
};

// ==================== XY坐标映射函数 ====================
// 将物理走线映射为标准的 X/Y 坐标
uint16_t XY(uint8_t x, uint8_t y) {
  if(x >= MATRIX_W || y >= MATRIX_H) return 0; // 防止越界
  
  uint8_t panel = x / 8;        // 算出当前像素在第几块板子 (0, 1, 2, 3)
  uint8_t local_x = x % 8;      // 在该板子内部的 X 坐标 (0-7)
  uint8_t local_y = y;          // 在该板子内部的 Y 坐标 (0-7)
  uint8_t index_in_panel = 0;
  
  // 大部分 8x8 WS2812 是蛇形走线
  if (local_y % 2 == 0) {
    index_in_panel = local_y * 8 + local_x;       // 偶数行：从左向右
  } else {
    index_in_panel = local_y * 8 + (7 - local_x); // 奇数行：从右向左
  }

  return (panel * 64) + index_in_panel;
}

// ==================== 绘制单个数字 ====================
void drawDigit(uint8_t x, uint8_t y, uint8_t digit, CRGB color) {
  if(digit > 9) return;
  for(int r = 0; r < 5; r++) {
    uint8_t rowBits = font4x5[digit][r];
    for(int c = 0; c < 4; c++) {
      if(rowBits & (1 << (3 - c))) { // 逐位读取
        leds[XY(x + c, y + r)] = color;
      }
    }
  }
}

// ==================== 渲染整个时钟画面 ====================
void drawClockScreen(uint8_t hour, uint8_t minute, uint8_t second, CRGB clockColor, CRGB barColor) {
  FastLED.clear(); // 清空上一帧

  uint8_t yOffset = 1; // 字体往下移动1像素，留出顶部边距

  // 1. 绘制小时 (占用 X: 4~7 和 9~12)
  drawDigit(4, yOffset, hour / 10, clockColor);
  drawDigit(9, yOffset, hour % 10, clockColor);

  // 2. 绘制冒号 ":" (固定在 X: 15)
  // 让冒号每秒闪烁：偶数秒亮，奇数秒灭
  if (second % 2 == 0) {
    leds[XY(15, yOffset + 1)] = clockColor;
    leds[XY(15, yOffset + 3)] = clockColor;
  }

  // 3. 绘制分钟 (占用 X: 18~21 和 23~26)
  drawDigit(18, yOffset, minute / 10, clockColor);
  drawDigit(23, yOffset, minute % 10, clockColor);

  // 4. 绘制底部秒数进度条 (最底下一行: Y=7)
  // 把 0-59 秒等比例映射到 0-32 个像素格子上
  int barLength = map(second, 0, 59, 1, 32); 
  for(int i = 0; i < barLength; i++) {
    leds[XY(i, 7)] = barColor; 
  }

  FastLED.show(); // 推送到灯带显示
}

// ==================== 网页测试界面 ====================
void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", "<h1>像素时钟测试成功！</h1><p>你的 ESP8266 已经连上网络，网页服务正常运行中。</p>");
}

// ==================== 初始化 ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n[系统启动]");

  // 初始化 WS2812
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);
  
  // 开机点亮第一个红灯，表示正在连 WiFi
  leds[0] = CRGB::Red; 
  FastLED.show();

  // 连接 WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi已连接! IP: " + WiFi.localIP().toString());
  
  // WiFi连上后变绿灯
  leds[0] = CRGB::Green;
  FastLED.show();

  // 启动网络校时
  timeClient.begin();
  timeClient.update();

  // 启动网页服务器 (解决你上个版本 IP 打不开的问题)
  server.on("/", handleRoot);
  server.begin();

  // 启动 OTA 远程升级配置
  ArduinoOTA.setHostname("ws2812-matrix"); 
  ArduinoOTA.begin();
  
  Serial.println("系统初始化完毕，开始走字！");
}

void loop() {
  // 处理网页访问请求
  server.handleClient();
  
  // 处理远程 OTA 升级请求
  ArduinoOTA.handle();

  // 更新时间
  timeClient.update();

  uint8_t h = timeClient.getHours();
  uint8_t m = timeClient.getMinutes();
  uint8_t s = timeClient.getSeconds();

  // 只有秒数改变时才刷新屏幕（节省CPU，防止画面闪烁）
  static int lastSecond = -1;
  if (s != lastSecond) {
    lastSecond = s;
    // 渲染时钟：纯白色的字(White)，青色的进度条(Cyan)
    drawClockScreen(h, m, s, CRGB::White, CRGB::Cyan);
    Serial.printf("当前时间: %02d:%02d:%02d\n", h, m, s);
  }
}