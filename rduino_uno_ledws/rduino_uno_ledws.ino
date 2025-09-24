#include <Adafruit_NeoPixel.h>

// ===== 引脚定义 =====
#define PIR_PIN         2       // HC-SR501 信号输出引脚
#define LDR_PIN         A0      // 5528 光敏电阻模拟输入引脚
#define RELAY_PIN       7       // 继电器信号控制引脚
#define NEOPIXEL_PIN    6       // WS2812 LED 数据引脚

// ===== 配置参数 =====
#define LED_COUNT       60      // WS2812 灯珠数量
const int LIGHT_THRESHOLD   = 400;  // 天黑阈值 (光线值 > 阈值, 判断为天黑)
const unsigned long MOTION_TIMEOUT = 30000UL; // 30秒内无新动作则关闭

// ===== 状态与触发控制变量 (参考 ESP8266 代码) =====
Adafruit_NeoPixel strip(LED_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
bool      relayState      = false;    // 继电器当前状态
unsigned long lastMotionTime  = 0;      // 上次有效触发的时刻

static bool lastPirState      = LOW;    // 用于检测PIR信号的上升沿
static unsigned long lastPirTrigger  = 0;    // 用于触发间隔去抖
const unsigned long pirMinInterval = 1000;  // 1秒内不重复触发，防止抖动

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n===== 系统启动 (采用上升沿触发逻辑) =====");
  Serial.print("天黑判断阈值 (光线值高于此值算天黑): ");
  Serial.println(LIGHT_THRESHOLD);
  Serial.print("无动作超时关闭时间: ");
  Serial.print(MOTION_TIMEOUT / 1000);
  Serial.println(" 秒");
  Serial.println("------------------------------------");

  pinMode(PIR_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // 默认关闭继电器

  strip.begin();
  strip.show(); // 初始化时关闭所有LED
  strip.setBrightness(150); // 设置亮度 (0-255)

  // 为了防止设备刚上电就意外触发，将上次活动时间初始化为一个很久以前的值
  lastMotionTime = millis() - MOTION_TIMEOUT - 1000;
}

void loop() {
  unsigned long now = millis();

  // 1. 读取传感器原始状态
  bool pir = digitalRead(PIR_PIN);
  int lightLevel = analogRead(LDR_PIN);

  // 【调试打印】
  Serial.print("光线值: ");
  Serial.print(lightLevel);
  Serial.print(lightLevel > LIGHT_THRESHOLD ? " (天黑) " : " (白天) ");
  Serial.print(" | PIR 原始信号: ");
  Serial.println(pir ? "1" : "0");

  // 2.【核心】PIR 上升沿检测 + 去抖逻辑 (来自您提供的ESP8266代码)
  // 条件: 当前为高电平 && 上一刻为低电平 && 距离上次有效触发已超过1秒
  if (pir && !lastPirState && (now - lastPirTrigger > pirMinInterval)) {
    lastMotionTime = now;     // 关键：更新最后活动时间！
    lastPirTrigger = now;     // 更新触发时间，用于去抖
    Serial.println("        └─────> 👤 有效人体触发 (检测到上升沿)，已刷新计时！");
  }
  lastPirState = pir; // 在每次循环后都更新PIR状态，用于下次比较

  // 3. 自动控制总逻辑
  // 条件: (当前时间 - 上次有效活动时间 <= 超时时长) 并且 (天黑)
  bool inTimeWindow = (now - lastMotionTime <= MOTION_TIMEOUT);
  bool isDark = (lightLevel > LIGHT_THRESHOLD);
  
  bool shouldBeOn = inTimeWindow && isDark;

  // 4. 根据总逻辑结果，改变设备状态 (仅在需要改变时执行)
  if (shouldBeOn != relayState) {
    setDevicesState(shouldBeOn); // 调用函数统一控制设备
  }

  // 5. 如果设备是开启状态，则持续更新彩虹灯效
  if (relayState) {
    rainbowCycle(20);
  }
  
  delay(50); // 短暂延时，稳定检测
}

/**
 * @brief 控制继电器和WS2812灯带的状态
 * @param state true为打开, false为关闭
 */
void setDevicesState(bool state) {
  if (state) {
    Serial.println(">>>>> ✅ 打开设备");
  } else {
    Serial.print(">>>>> ⛔ 关闭设备 (原因: ");
    if (!(millis() - lastMotionTime <= MOTION_TIMEOUT)) {
      Serial.print("超时");
    } else {
      Serial.print("天亮了");
    }
    Serial.println(")");
  }
  
  relayState = state;
  digitalWrite(RELAY_PIN, state ? HIGH : LOW);

  if (!state) {
    strip.clear();
    strip.show();
  }
}

// ... (rainbowCycle 和 wheel 函数与之前相同，无需修改) ...
void rainbowCycle(uint8_t wait) {
  uint16_t i, j;
  for(j=0; j<256; j++) {
    for(i=0; i<strip.numPixels(); i++) {
      strip.setPixelColor(i, wheel(((i * 256 / strip.numPixels()) + j) & 255));
    }
    if (!relayState) break; 
    strip.show();
    delay(wait);
  }
}
uint32_t wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) { return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3); }
  if(WheelPos < 170) { WheelPos -= 85; return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3); }
  WheelPos -= 170; return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}