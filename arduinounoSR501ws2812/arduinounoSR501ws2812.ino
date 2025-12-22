#include <Adafruit_NeoPixel.h>

// --- 配置参数 ---
const int PIR_PIN = 2;          // HC-SR501 信号引脚
const int LED_PIN = 6;          // WS2812 数据引脚
const int LED_COUNT = 120;       // 灯珠数量

// 时间设置：1分钟 = 60000毫秒
unsigned long keepOnDuration = 30000; 

// --- 变量定义 ---
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

unsigned long lastTriggerTime = 0; // 最后一次检测到人的时间
bool isLightActive = false;        // 灯光当前状态
uint16_t j = 0;                    // 彩虹动画循环变量

void setup() {
  pinMode(PIR_PIN, INPUT);
  strip.begin();
  strip.show(); // 初始化关闭所有灯
  strip.setBrightness(50); // 设置亮度 (0-255)
  
  Serial.begin(9600);
  Serial.println("系统启动，正在预热传感器...");
  // HC-SR501 启动后需要约 30-60秒预热
}

void loop() {
  // 读取传感器状态
  int sensorValue = digitalRead(PIR_PIN);

  if (sensorValue == HIGH) {
    // 如果检测到人，重置/更新计时器
    if (!isLightActive) {
      Serial.println("检测到人体，开灯！");
    }
    isLightActive = true;
    lastTriggerTime = millis(); // 只要有人，就刷新这个时间点
  }

  // 判断是否在“保持时间”内
  if (isLightActive) {
    if (millis() - lastTriggerTime < keepOnDuration) {
      // 在一分钟内：显示彩虹效果
      rainbowCycle(10); 
    } else {
      // 超过一分钟没人：关灯
      Serial.println("超时，关灯。");
      allOff();
      isLightActive = false;
    }
  }
}

// 非阻塞式彩虹循环函数
void rainbowCycle(uint8_t wait) {
  uint16_t i;
  for(i=0; i< strip.numPixels(); i++) {
    strip.setPixelColor(i, Wheel(((i * 256 / strip.numPixels()) + j) & 255));
  }
  strip.show();
  
  j++; // 移动彩虹颜色
  if (j >= 256*5) j = 0; // 5个完整的颜色循环
  delay(wait); // 控制彩虹旋转速度
}

// 关闭所有灯
void allOff() {
  for(int i=0; i<LED_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(0, 0, 0));
  }
  strip.show();
}

// 彩虹颜色计算辅助函数
uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if(WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}