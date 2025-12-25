#include <Adafruit_NeoPixel.h>

// ================= 硬件接线 =================
const int PIR_PIN   = 2;       // SR505 信号线 -> Arduino Pin 2
const int LED_PIN   = 6;       // 灯带数据线 -> Arduino Pin 6
const int LED_COUNT = 120;     // 灯珠数量

// ================= 用户参数 =================
const int BRIGHTNESS_MAX = 255;           // 最大亮度
const unsigned long KEEP_ON_TIME = 30000; // 亮灯保持 30秒
const unsigned long PIR_FILTER_MS = 800;  // 滤波 0.8秒

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// 逻辑变量
unsigned long lastMotionTime = 0;       
unsigned long pirHighStartTime = 0;     
bool pirActive = false;                 
bool lastPirActive = false;             
bool isTriggered = false;               
bool seedInitialized = false;           

// 动画变量
int currentEffect = -1;                 
int previousEffect = -1;                
unsigned long lastAnimTime = 0;         
uint32_t animState = 0;                 
uint16_t animHue = 0;                   

void setup() {
  Serial.begin(9600);
  pinMode(PIR_PIN, INPUT);
  
  randomSeed(analogRead(0)); 

  strip.begin();
  strip.setBrightness(BRIGHTNESS_MAX);
  strip.clear();
  strip.show();

  Serial.println(">>> 15种护眼特效系统启动 <<<");
}

void loop() {
  unsigned long now = millis();

  // 1. 传感器滤波
  int rawPIR = digitalRead(PIR_PIN);
  if (rawPIR == HIGH) {
    if (pirHighStartTime == 0) pirHighStartTime = now;
    else if (now - pirHighStartTime > PIR_FILTER_MS) pirActive = true;
  } else {
    pirHighStartTime = 0;
    pirActive = false;
  }

  // 2. 状态机逻辑：检测新动作 -> 切换特效
  if (pirActive && !lastPirActive) {
    Serial.print(">>> 捕捉到新动作！");
    lastMotionTime = now;
    isTriggered = true;

    // 随机数增强
    if (!seedInitialized) { randomSeed(micros()); seedInitialized = true; } 
    else { randomSeed(micros() + random(1000)); }

    // 随机去重
    int newEffect;
    do { newEffect = random(0, 15); } while (newEffect == previousEffect);
    
    currentEffect = newEffect;
    previousEffect = currentEffect;

    // 重置动画参数
    animState = 0;
    animHue = random(65535); // 随机起始颜色
    strip.setBrightness(BRIGHTNESS_MAX);

    Serial.print(" 切换特效 ID: ");
    Serial.println(currentEffect);
  }

  if (pirActive) lastMotionTime = now; 
  lastPirActive = pirActive;

  // 3. 执行输出
  if (isTriggered) {
    if (now - lastMotionTime > KEEP_ON_TIME) {
      Serial.println("超时 -> 柔和关灯");
      fadeOut(); 
      isTriggered = false;
      currentEffect = -1;
      strip.clear();
      strip.show();
    } else {
      runAnimation(currentEffect);
    }
  } else {
    strip.clear();
    strip.show();
  }
}

// ==========================================
//              15种护眼特效库
// ==========================================
void runAnimation(int effect) {
  unsigned long now = millis();
  
  switch(effect) {
    case 0: effectRainbowFlow(now);   break; // 丝滑彩虹
    case 1: effectWarmBreath(now);    break; // 暖白呼吸 (最护眼)
    case 2: effectOceanWaves(now);    break; // 海洋流光 (蓝青)
    case 3: effectLavaFlow(now);      break; // 熔岩慢流 (红橙)
    case 4: effectForest(now);        break; // 森林氧气 (绿金)
    case 5: effectSoftConfetti(now);  break; // 柔和碎纸机
    case 6: effectGoldenSparkle(now); break; // 金色星光
    case 7: effectColorWipe(now);     break; // 慢速填色
    case 8: effectScannerSoft(now);   break; // 柔和扫描
    case 9: effectMeteorRain(now);    break; // 流星雨
    case 10: effectAurora(now);       break; // 极光 (绿紫)
    case 11: effectPlasma(now);       break; // 等离子波动
    case 12: effectCloud(now);        break; // 云端 (白蓝淡色)
    case 13: effectTheaterSoft(now);  break; // 慢速跑马灯
    case 14: effectFairyLights(now);  break; // 萤火虫
  }
}

// --- 0. 丝滑彩虹 (速度放慢) ---
void effectRainbowFlow(unsigned long now) {
  if (now - lastAnimTime < 20) return; // 20ms刷新一次，比较慢
  lastAnimTime = now;
  animState++;
  for(int i=0; i< strip.numPixels(); i++) {
    int pixelHue = animState * 100 + (i * 65536L / strip.numPixels());
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
  }
  strip.show();
}

// --- 1. 暖白呼吸 (居家推荐) ---
void effectWarmBreath(unsigned long now) {
  if (now - lastAnimTime < 20) return;
  lastAnimTime = now;
  float val = (exp(sin(animState / 100.0 * PI)) - 0.36787944) * 108.0;
  if (val > 255) val = 255;
  // 暖色温: Hue 5000, Sat 150
  for(int i=0; i<strip.numPixels(); i++) {
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(5000, 150, (int)val)));
  }
  strip.show();
  animState++;
}

// --- 2. 海洋流光 (蓝/青色系) ---
void effectOceanWaves(unsigned long now) {
  if (now - lastAnimTime < 30) return;
  lastAnimTime = now;
  animState++;
  for(int i=0; i< strip.numPixels(); i++) {
    // 限制 Hue 在蓝色范围内 (30000 - 45000)
    int hue = 30000 + sin((animState + i * 10) / 50.0) * 8000;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 255, 200)));
  }
  strip.show();
}

// --- 3. 熔岩慢流 (红/橙色系) ---
void effectLavaFlow(unsigned long now) {
  if (now - lastAnimTime < 30) return;
  lastAnimTime = now;
  animState++;
  for(int i=0; i< strip.numPixels(); i++) {
    // 限制 Hue 在红色/橙色范围内 (0 - 8000)
    int hue = 4000 + sin((animState + i * 8) / 40.0) * 4000;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 255, 200)));
  }
  strip.show();
}

// --- 4. 森林氧气 (绿/金色系) ---
void effectForest(unsigned long now) {
  if (now - lastAnimTime < 30) return;
  lastAnimTime = now;
  animState++;
  for(int i=0; i< strip.numPixels(); i++) {
    int hue = 22000 + sin((animState + i * 5) / 60.0) * 5000;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 240, 180)));
  }
  strip.show();
}

// --- 5. 柔和碎纸机 (不刺眼) ---
void effectSoftConfetti(unsigned long now) {
  if (now - lastAnimTime < 20) return;
  lastAnimTime = now;
  fadeToBlack(5); // 拖尾很长，不急着灭
  if (random(20) == 0) { // 降低生成频率
    int pos = random(strip.numPixels());
    // 随机柔和颜色
    strip.setPixelColor(pos, strip.ColorHSV(random(65535), 200, 200));
  }
  strip.show();
}

// --- 6. 金色星光 (背景微亮) ---
void effectGoldenSparkle(unsigned long now) {
  if (now - lastAnimTime < 30) return;
  lastAnimTime = now;
  // 背景保持微弱的琥珀色
  for(int i=0; i<strip.numPixels(); i++) {
    strip.setPixelColor(i, strip.Color(20, 10, 0)); 
  }
  // 随机闪烁
  if (random(10) == 0) {
    int pos = random(strip.numPixels());
    strip.setPixelColor(pos, strip.Color(255, 200, 100)); // 金色高亮
  }
  strip.show();
}

// --- 7. 慢速填色 ---
void effectColorWipe(unsigned long now) {
  if (now - lastAnimTime < 30) return;
  lastAnimTime = now;
  if (animState < strip.numPixels()) {
    strip.setPixelColor(animState, strip.ColorHSV(animHue, 255, 200));
    strip.show();
    animState++;
  } else {
    animState = 0;
    animHue += 10000; // 换个颜色
  }
}

// --- 8. 柔和扫描 (拖尾) ---
void effectScannerSoft(unsigned long now) {
  if (now - lastAnimTime < 25) return;
  lastAnimTime = now;
  fadeToBlack(40);
  int pos = (animState / 2) % (strip.numPixels() * 2);
  int actualPos = (pos >= strip.numPixels()) ? (strip.numPixels() * 2 - pos - 1) : pos;
  // 青色扫描，比较科技感但不刺眼
  strip.setPixelColor(actualPos, strip.Color(0, 150, 200));
  animState++;
  strip.show();
}

// --- 9. 流星雨 (经典) ---
void effectMeteorRain(unsigned long now) {
  if (now - lastAnimTime < 20) return;
  lastAnimTime = now;
  fadeToBlack(60);
  int pos = animState % (strip.numPixels() + 10);
  if (pos < strip.numPixels()) strip.setPixelColor(pos, strip.Color(200, 200, 255));
  animState++;
  strip.show();
}

// --- 10. 极光 (绿/紫渐变) ---
void effectAurora(unsigned long now) {
  if (now - lastAnimTime < 30) return;
  lastAnimTime = now;
  animState++;
  for(int i=0; i< strip.numPixels(); i++) {
    // 在紫色(50000)和绿色(20000)之间波动
    int hue = 35000 + sin((animState + i * 5) / 50.0) * 15000;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 220, 180)));
  }
  strip.show();
}

// --- 11. 等离子波动 ---
void effectPlasma(unsigned long now) {
  if (now - lastAnimTime < 30) return;
  lastAnimTime = now;
  animState++;
  for (int i = 0; i < strip.numPixels(); i++) {
    int color = sin(animState / 50.0 + i / 10.0) * 32768 + 32768;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(color, 255, 200)));
  }
  strip.show();
}

// --- 12. 云端 (淡彩) ---
void effectCloud(unsigned long now) {
  if (now - lastAnimTime < 40) return;
  lastAnimTime = now;
  animState++;
  for(int i=0; i< strip.numPixels(); i++) {
    // 极低饱和度，接近白光但带点色彩
    int hue = animState * 50;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 60, 200)));
  }
  strip.show();
}

// --- 13. 慢速跑马灯 (彩虹版) ---
void effectTheaterSoft(unsigned long now) {
  if (now - lastAnimTime < 80) return; // 慢一点
  lastAnimTime = now;
  for (int i=0; i < strip.numPixels(); i++) {
    if ((i + animState) % 3 == 0) {
      strip.setPixelColor(i, strip.ColorHSV((animState * 1000) % 65535, 180, 180));
    } else {
      strip.setPixelColor(i, 0);
    }
  }
  strip.show();
  animState++;
}

// --- 14. 萤火虫 (随机游走) ---
void effectFairyLights(unsigned long now) {
  if (now - lastAnimTime < 50) return;
  lastAnimTime = now;
  fadeToBlack(10); // 慢慢消失
  if (random(15) == 0) {
    int pos = random(strip.numPixels());
    // 柠檬黄色
    strip.setPixelColor(pos, strip.Color(200, 255, 50));
  }
  strip.show();
}

// ================= 辅助函数 =================
void fadeToBlack(int fadeValue) {
  for(int i = 0; i < strip.numPixels(); i++) {
    uint32_t old = strip.getPixelColor(i);
    uint8_t r = (old >> 16) & 0xff;
    uint8_t g = (old >> 8) & 0xff;
    uint8_t b = old & 0xff;
    r = (r <= fadeValue) ? 0 : (r - fadeValue);
    g = (g <= fadeValue) ? 0 : (g - fadeValue);
    b = (b <= fadeValue) ? 0 : (b - fadeValue);
    strip.setPixelColor(i, r, g, b);
  }
}

// 柔和关灯 (Fade Out)
void fadeOut() {
  for(int i=255; i>=0; i-=3) { // 减慢关灯速度
    strip.setBrightness(i);
    strip.show();
    delay(8);
  }
  strip.clear();
  strip.show();
}