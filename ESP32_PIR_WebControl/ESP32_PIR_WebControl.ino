/**
 * ============================================================
 *  LED 灯带控制器 v7.5 — ESP32-C3 + WS2812B (60珠)
 *
 *  v7.5 调整:
 *   [✓] 移除 HC-SR501 红外人体传感器，通电后自动亮灯
 *   [✓] 移除 Light Sleep，WiFi 保持在线，方便随时网页控制
 *   [✓] NOTE_URL 每个北京时间自然日最多尝试写入一次
 *       写入日期先保存至 NVS，断电/重启后仍然有效
 *   [✓] 移除闪电/警灯/星光等强闪烁灯效，统一改为柔和渐变
 *   [✓] 新增全局帧缓冲与约 1.2 秒灯效交叉渐变，切换更平滑
 *   [✓] 新增慢速渐亮渐暗模式，亮度从低到高平滑变化
 *   [✓] 保留累计亮灯时长，累计时长每5分钟保存，降低断电丢失
 *   [✓] 网页不再提供点亮/关闭按钮，上电自动运行当前灯效
 *   [✓] Wi-Fi连接期间灯效仍持续运行，不必等待联网才亮灯
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <vector>

// ==================== 用户配置区 ====================
const char* WIFI_SSID     = "yang1234";
const char* WIFI_PASSWORD = "y123456789";

#define NEOPIXEL_PIN    5
#define LED_COUNT       60

#define RAND_INTERVAL   30000UL   // 随机模式切换间隔 (ms)
#define LOG_KEEP_DAYS   7         // 保留天数
#define LOG_MAX_BYTES   65536     // 每个日志文件上限 64KB
#define ONSEC_SAVE_INTERVAL 300000UL // 累计亮灯时长每5分钟写入NVS

// ---- 云端笔记 API (每个北京时间自然日最多尝试一次) ----
#define NOTE_URL        "https://note.yysresume.work/api/note-op"
#define NOTE_ID         "97577ea2-5ce8-43e5-83eb-8b848abdb242"
#define NOTE_AUTH       "a_secret_fixed_token"
// ====================================================

Adafruit_NeoPixel strip(LED_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
WebServer   server(80);
Preferences prefs;

// ---- 配置参数 (NVS持久化) ----
uint8_t  cfg_r          = 255;
uint8_t  cfg_g          = 150;
uint8_t  cfg_b          = 0;
uint8_t  cfg_brightness = 150;
int      cfg_mode       = 0;
uint32_t stat_onSec     = 0;

// ---- 运行时状态 ----
bool          isLightOn      = true;  // 通电自动亮灯，不再需要网页按钮
bool          wifiOnline     = false;
bool          ntpSynced      = false;
bool          routesConfigured = false;
unsigned long lightOnStart   = 0;
unsigned long lastOnSecSave  = 0;


// ---- 随机模式 ----
int           randCurrentMode = 1;
unsigned long lastRandSwitch  = 0;

// ---- 灯效帧缓冲：所有效果先绘制到缓冲区，再统一柔和输出 ----
static uint32_t effectFrame[LED_COUNT];          // 当前灯效的原始帧
static uint32_t lastShownFrame[LED_COUNT];       // 上一帧实际输出颜色
static uint32_t transitionFromFrame[LED_COUNT];  // 灯效切换渐变的起始帧
float effectLevel = 1.0f;                        // 单个灯效自己的亮度系数
int activeEffectMode = -1;
bool modeTransitionActive = false;
unsigned long modeTransitionStarted = 0;
#define MODE_CROSSFADE_MS 1200UL                 // 模式切换交叉渐变时长

// ---- 日志文件状态 ----
static char logCurDate[9] = {0};

// NVS 中记录最近一次云端笔记请求日期，格式 YYYYMMDD
String lastNoteAttemptDate = "";
unsigned long lastWifiRetry = 0;
#define WIFI_RETRY_INTERVAL 30000UL


// ============================================================
//  NVS 持久化
// ============================================================
void loadPrefs() {
  prefs.begin("led", true);
  cfg_r          = prefs.getUChar("r",    255);
  cfg_g          = prefs.getUChar("g",    150);
  cfg_b          = prefs.getUChar("b",    0);
  cfg_brightness = prefs.getUChar("bri",  150);
  cfg_mode       = prefs.getInt  ("mode", 0);
  stat_onSec     = prefs.getULong("ons",  0);
  lastNoteAttemptDate = prefs.getString("noteDate", "");
  prefs.end();
}

void savePrefs() {
  prefs.begin("led", false);
  prefs.putUChar("r",   cfg_r);
  prefs.putUChar("g",   cfg_g);
  prefs.putUChar("b",   cfg_b);
  prefs.putUChar("bri", cfg_brightness);
  prefs.putInt  ("mode",cfg_mode);
  prefs.end();
}

void saveOnSec() {
  prefs.begin("led", false);
  prefs.putULong("ons", stat_onSec);
  prefs.end();
}

// 灯带始终随设备通电运行。每5分钟把已累计秒数保存到NVS，
// 采用增量基准，状态页显示值不会因保存操作而倒退。
void persistOnSecondsIfDue() {
  if (!isLightOn || lightOnStart == 0) return;

  const unsigned long now = millis();
  if (lastOnSecSave == 0) lastOnSecSave = now;
  if (now - lastOnSecSave < ONSEC_SAVE_INTERVAL) return;

  const uint32_t elapsed = (now - lightOnStart) / 1000UL;
  if (elapsed > 0) {
    stat_onSec += elapsed;
    lightOnStart += (unsigned long)elapsed * 1000UL;
    saveOnSec();
  }
  lastOnSecSave = now;
}



// ============================================================
//  LittleFS 日志系统
// ============================================================
bool getDateStr(char buf[9]) {
  struct tm ti;
  if (ntpSynced && getLocalTime(&ti, 5) && ti.tm_year > 120) {
    snprintf(buf, 9, "%04d%02d%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
    return true;
  }
  buf[0] = '\0';
  return false;
}

String logPath(const char* date) {
  return String("/log_") + date + ".txt";
}

void logRotate() {
  struct tm ti;
  if (!getLocalTime(&ti, 5) || ti.tm_year <= 120) return;
  time_t now = mktime(&ti);

  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) return;

  std::vector<String> toDelete;
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    f.close();
    if (name.startsWith("/")) name = name.substring(1);
    if (name.startsWith("log_") && name.endsWith(".txt") && name.length() == 16) {
      String dp = name.substring(4, 12);
      struct tm ft = {0};
      ft.tm_year = dp.substring(0,4).toInt() - 1900;
      ft.tm_mon  = dp.substring(4,6).toInt() - 1;
      ft.tm_mday = dp.substring(6,8).toInt();
      ft.tm_hour = 12;
      time_t ft_t = mktime(&ft);
      if (ft_t > 0 && difftime(now, ft_t) > (double)LOG_KEEP_DAYS * 86400.0) {
        toDelete.push_back("/" + name);
      }
    }
    f = root.openNextFile();
  }
  root.close();

  for (auto& p : toDelete) {
    LittleFS.remove(p);
    Serial.printf("🗑 删除旧日志: %s\n", p.c_str());
  }
}

void logAppend(const char* line) {
  char date[9];
  String path;

  if (getDateStr(date)) {
    if (strcmp(date, logCurDate) != 0) {
      strncpy(logCurDate, date, 9);
      logRotate();
    }
    path = logPath(date);
  } else {
    path = "/log_boot.txt";
  }

  if (LittleFS.exists(path)) {
    File chk = LittleFS.open(path, "r");
    size_t sz = chk ? chk.size() : 0;
    if (chk) chk.close();
    if (sz > LOG_MAX_BYTES) {
      File fw = LittleFS.open(path, "w");
      if (fw) { fw.println("[--- 日志已截断，超过64KB ---]"); fw.close(); }
    }
  }

  File f = LittleFS.open(path, "a");
  if (f) { f.print(line); f.close(); }
}

String getLogByDate(const char* date) {
  String path;
  if (strcmp(date, "boot") == 0 || strlen(date) == 0) {
    path = "/log_boot.txt";
  } else {
    path = logPath(date);
  }
  if (!LittleFS.exists(path)) return "(暂无日志)";
  File f = LittleFS.open(path, "r");
  if (!f) return "(读取失败)";
  size_t sz = f.size();
  if (sz > 8192) f.seek(sz - 8192);
  String s = f.readString();
  f.close();
  return s;
}

String getLogList() {
  String json = "[";
  bool first = true;

  if (LittleFS.exists("/log_boot.txt")) {
    json += "\"boot\"";
    first = false;
  }

  File root = LittleFS.open("/");
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f) {
      String name = String(f.name());
      f.close();
      if (name.startsWith("/")) name = name.substring(1);
      if (name.startsWith("log_") && name.endsWith(".txt") && name.length() == 16) {
        String label = name.substring(4, 12);
        if (!first) json += ",";
        json += "\"" + label + "\"";
        first = false;
      }
      f = root.openNextFile();
    }
    root.close();
  }
  json += "]";
  return json;
}

String getFsInfo() {
  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  char buf[80];
  snprintf(buf, sizeof(buf), "{\"total\":%u,\"used\":%u}", (unsigned)total, (unsigned)used);
  return String(buf);
}


// ============================================================
//  LOG
// ============================================================
void LOG(const char* fmt, ...) {
  char ts[22] = {0};
  struct tm ti;
  if (ntpSynced && getLocalTime(&ti, 5) && ti.tm_year > 120) {
    snprintf(ts, sizeof(ts), "[%04d-%02d-%02d %02d:%02d:%02d]",
             ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    unsigned long s = millis() / 1000;
    snprintf(ts, sizeof(ts), "[boot %02lu:%02lu:%02lu]",
             s/3600, (s%3600)/60, s%60);
  }
  char msg[128] = {0};
  va_list ap; va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  char line[180] = {0};
  snprintf(line, sizeof(line), "%s %s\n", ts, msg);
  Serial.print(line);
  logAppend(line);
}


// ============================================================
//  工具
// ============================================================
void stripOff() {
  // 使用 255 作为灯带驱动的固定亮度，实际亮度由帧缓冲统一计算，避免反复 setBrightness 造成色彩损失。
  strip.setBrightness(255);
  strip.clear(); strip.show(); delay(2);
  strip.clear(); strip.show();
  memset(effectFrame, 0, sizeof(effectFrame));
  memset(lastShownFrame, 0, sizeof(lastShownFrame));
  memset(transitionFromFrame, 0, sizeof(transitionFromFrame));
  effectLevel = 1.0f;
  activeEffectMode = -1;
  modeTransitionActive = false;
}

uint32_t colorWheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return strip.Color(255-pos*3, 0, pos*3);
  if (pos < 170) { pos-=85; return strip.Color(0, pos*3, 255-pos*3); }
  pos -= 170;    return strip.Color(pos*3, 255-pos*3, 0);
}


// ============================================================
//  IP末段暗号动画（保留功能，改为手动触发）
// ============================================================
void showIPLastOctet() {
  IPAddress ip = WiFi.localIP();
  uint8_t last = ip[3];
  LOG("IP暗号: %s  末段=%d", ip.toString().c_str(), last);

  // 不再三次快速闪烁，改为低亮度柔和淡入。
  strip.setBrightness(255);
  for (int b=0; b<=90; b+=3) {
    const uint8_t v = (uint8_t)b;
    strip.fill(strip.Color(v, v, v));
    strip.show();
    delay(18);
  }
  strip.clear(); strip.show();
  delay(150);

  if (last <= LED_COUNT) {
    for (int i=0;i<(int)last;i++){
      int idx=LED_COUNT-1-i;
      if(i>0){float f=0.55f+0.45f*((float)i/last);strip.setPixelColor(idx+1,strip.Color(0,(uint8_t)(105*f),(uint8_t)(145*f)));}
      strip.setPixelColor(idx,strip.Color(105,105,105)); strip.show(); delay(35);
    }
    delay(100);
    if(last>0){strip.setPixelColor(LED_COUNT-last,strip.Color(0,95,130));strip.show();}
    LOG("  末尾%d颗亮=IP末段%d",last,last);
  } else {
    uint8_t tens=last/10,ones=last%10;
    for(int i=0;i<(int)tens;i++){
      if(i>0){float f=0.5f+0.5f*((float)i/tens);strip.setPixelColor(i-1,strip.Color((uint8_t)(130*f),(uint8_t)(50*f),0));}
      strip.setPixelColor(i,strip.Color(105,105,105)); strip.show(); delay(45);
    }
    for(int i=0;i<(int)tens;i++){float f=0.5f+0.5f*((float)i/tens);strip.setPixelColor(i,strip.Color((uint8_t)(130*f),(uint8_t)(50*f),0));}
    strip.show(); delay(400);
    for(int i=0;i<(int)ones;i++){
      int idx=LED_COUNT-1-i;
      if(i>0)strip.setPixelColor(idx+1,strip.Color(0,95,130));
      strip.setPixelColor(idx,strip.Color(105,105,105)); strip.show(); delay(80);
    }
    if(ones>0){strip.setPixelColor(LED_COUNT-ones,strip.Color(0,95,130));strip.show();}
    LOG("  头%d橙+尾%d青=%d",tens,ones,last);
  }
  delay(3000);
  // 保存当前图案，并按原始像素值逐帧淡出，避免 setBrightness 反复缩放造成色彩损失。
  uint32_t ipSnapshot[LED_COUNT];
  for (int i = 0; i < LED_COUNT; ++i) ipSnapshot[i] = strip.getPixelColor(i);
  for (int level = 220; level >= 0; level -= 5) {
    const float f = (float)level / 220.0f;
    for (int i = 0; i < LED_COUNT; ++i) {
      const uint32_t c = ipSnapshot[i];
      strip.setPixelColor(i,
        (uint8_t)(((c >> 16) & 0xFF) * f),
        (uint8_t)(((c >> 8)  & 0xFF) * f),
        (uint8_t)(( c        & 0xFF) * f));
    }
    strip.show();
    delay(16);
  }
  strip.clear(); strip.show();
  strip.setBrightness(255);
  memset(effectFrame, 0, sizeof(effectFrame));
  memset(lastShownFrame, 0, sizeof(lastShownFrame));
  memset(transitionFromFrame, 0, sizeof(transitionFromFrame));
  activeEffectMode = -1;
  modeTransitionActive = false;
  LOG("IP暗号显示完毕");
}


// ============================================================
//  灯效：全部采用帧缓冲，取消闪电/警灯/星光等强闪烁效果
// ============================================================
uint32_t scaleColor(uint32_t c, float factor) {
  factor = constrain(factor, 0.0f, 1.0f);
  uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * factor);
  uint8_t g = (uint8_t)(((c >> 8)  & 0xFF) * factor);
  uint8_t b = (uint8_t)(( c        & 0xFF) * factor);
  return strip.Color(r, g, b);
}

void frameFill(uint32_t c) {
  for (int i = 0; i < LED_COUNT; ++i) effectFrame[i] = c;
}

void frameAdd(int idx, uint8_t r, uint8_t g, uint8_t b) {
  if (idx < 0 || idx >= LED_COUNT) return;
  uint32_t old = effectFrame[idx];
  uint16_t nr = ((old >> 16) & 0xFF) + r;
  uint16_t ng = ((old >> 8)  & 0xFF) + g;
  uint16_t nb = ( old        & 0xFF) + b;
  effectFrame[idx] = strip.Color((uint8_t)min((uint16_t)255, nr),
                                 (uint8_t)min((uint16_t)255, ng),
                                 (uint8_t)min((uint16_t)255, nb));
}

void renderEffectFrame() {
  const unsigned long now = millis();
  float blend = 1.0f;
  bool useBlend = modeTransitionActive;

  if (useBlend) {
    float p = (float)(now - modeTransitionStarted) / (float)MODE_CROSSFADE_MS;
    if (p >= 1.0f) {
      modeTransitionActive = false;
      useBlend = false;
      blend = 1.0f;
    } else {
      // Smoothstep：起步和结束都更柔和，不会突然跳变。
      p = constrain(p, 0.0f, 1.0f);
      blend = p * p * (3.0f - 2.0f * p);
    }
  }

  const float scale = ((float)cfg_brightness / 255.0f) * constrain(effectLevel, 0.0f, 1.0f);
  for (int i = 0; i < LED_COUNT; ++i) {
    const uint32_t c = effectFrame[i];
    int r = (int)(((c >> 16) & 0xFF) * scale);
    int g = (int)(((c >> 8)  & 0xFF) * scale);
    int b = (int)(( c        & 0xFF) * scale);

    if (useBlend) {
      const uint32_t from = transitionFromFrame[i];
      r = (int)(((from >> 16) & 0xFF) + (r - (int)((from >> 16) & 0xFF)) * blend);
      g = (int)(((from >> 8)  & 0xFF) + (g - (int)((from >> 8)  & 0xFF)) * blend);
      b = (int)(( from        & 0xFF) + (b - (int)( from        & 0xFF)) * blend);
    }

    r = constrain(r, 0, 255); g = constrain(g, 0, 255); b = constrain(b, 0, 255);
    strip.setPixelColor(i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    lastShownFrame[i] = strip.Color((uint8_t)r, (uint8_t)g, (uint8_t)b);
  }
  strip.show();
}

void fxSolid() {
  effectLevel = 1.0f;
  frameFill(strip.Color(cfg_r, cfg_g, cfg_b));
}

// 1：缓慢流动的彩虹，不再快速跳色。
void fxRainbow() {
  static unsigned long last = 0;
  static uint16_t phase = 0;
  const unsigned long now = millis();
  if (now - last >= 45) {
    last = now;
    phase = (phase + 1) & 0xFF;
  }
  effectLevel = 0.88f;
  for (int i = 0; i < LED_COUNT; ++i) {
    effectFrame[i] = colorWheel(((i * 256 / LED_COUNT) + phase) & 0xFF);
  }
}

// 2：柔和呼吸，低亮度约为设定亮度的 22%，再平滑升至设定亮度。
void fxBreathe() {
  const float phase = (float)(millis() % 6000UL) / 6000.0f * 2.0f * PI;
  effectLevel = 0.22f + 0.78f * (0.5f - 0.5f * cosf(phase));
  frameFill(strip.Color(cfg_r, cfg_g, cfg_b));
}

// 3：柔和流星。长尾逐步衰减，亮度受控，不做全灯闪白。
void fxMeteor() {
  static unsigned long last = 0;
  static int pos = 0;
  const unsigned long now = millis();
  if (now - last >= 55) {
    last = now;
    for (int i = 0; i < LED_COUNT; ++i) effectFrame[i] = scaleColor(effectFrame[i], 0.78f);
    const int tail = 18;
    for (int j = 0; j < tail; ++j) {
      const int idx = pos - j;
      if (idx >= 0 && idx < LED_COUNT) {
        float f = 1.0f - (float)j / (float)tail;
        f = f * f;
        frameAdd(idx, (uint8_t)(155 * f), (uint8_t)(105 * f), (uint8_t)(48 * f));
      }
    }
    if (++pos >= LED_COUNT + tail) pos = 0;
  }
  effectLevel = 0.82f;
}

// 4：平缓的暖色波动，取代随机跳变的火焰闪烁。
void fxFire() {
  const float t = (float)millis() / 2300.0f;
  for (int i = 0; i < LED_COUNT; ++i) {
    const float wave = 0.5f + 0.5f * sinf(t + i * 0.19f + 0.20f * sinf(t * 0.43f + i * 0.07f));
    const float v = 0.22f + 0.48f * wave;  // 限制变化范围，避免突然窜亮
    effectFrame[i] = strip.Color((uint8_t)(220 * v), (uint8_t)(78 * v), (uint8_t)(14 * v));
  }
  effectLevel = 0.95f;
}

// 5：整条灯带缓慢变色，不再随机闪烁星点。
void fxColorFade() {
  const uint8_t hue = (uint8_t)((millis() / 95UL) & 0xFF);
  effectLevel = 0.85f;
  frameFill(colorWheel(hue));
}

// 6：柔和暖光呼吸，固定暖白色并缓慢明暗变化，取代警灯闪烁。
void fxWarmGlow() {
  const float phase = (float)(millis() % 9000UL) / 9000.0f * 2.0f * PI;
  effectLevel = 0.30f + 0.48f * (0.5f - 0.5f * cosf(phase));
  frameFill(strip.Color(255, 145, 65));
}

// 7：长尾追光。灯珠逐渐衰减，不清屏、不硬切。
void fxChase() {
  static unsigned long last = 0;
  static int pos = 0;
  const unsigned long now = millis();
  if (now - last >= 65) {
    last = now;
    for (int i = 0; i < LED_COUNT; ++i) effectFrame[i] = scaleColor(effectFrame[i], 0.82f);
    const int tail = 18;
    for (int j = 0; j < tail; ++j) {
      int idx = pos - j;
      while (idx < 0) idx += LED_COUNT;
      float f = 1.0f - (float)j / (float)tail;
      f = f * f;
      frameAdd(idx, (uint8_t)(cfg_r * f), (uint8_t)(cfg_g * f), (uint8_t)(cfg_b * f));
    }
    pos = (pos + 1) % LED_COUNT;
  }
  effectLevel = 0.90f;
}

// 8：慢速渐亮渐暗（推荐）。一个完整周期 12 秒，曲线平滑，无闪烁。
void fxSlowFade() {
  const float phase = (float)(millis() % 12000UL) / 12000.0f * 2.0f * PI;
  effectLevel = 0.12f + 0.88f * (0.5f - 0.5f * cosf(phase));
  frameFill(strip.Color(cfg_r, cfg_g, cfg_b));
}

void runEffects() {
  const unsigned long now = millis();
  int eff = cfg_mode;

  if (cfg_mode == 9) {
    if (lastRandSwitch == 0) lastRandSwitch = now;
    if (now - lastRandSwitch >= RAND_INTERVAL) {
      int nx;
      do { nx = random(1, 9); } while (nx == randCurrentMode);
      randCurrentMode = nx;
      lastRandSwitch = now;
      LOG("随机平滑切换 → 效果%d", randCurrentMode);
    }
    eff = randCurrentMode;
  }

  if (eff < 0 || eff > 8) eff = 0;

  // 任意手动切换或随机切换，先缓存上一帧，再用 1.2 秒交叉渐变进入新模式。
  if (activeEffectMode != eff) {
    memcpy(transitionFromFrame, lastShownFrame, sizeof(transitionFromFrame));
    activeEffectMode = eff;
    modeTransitionStarted = now;
    modeTransitionActive = true;
  }

  effectLevel = 1.0f;
  switch (eff) {
    case 1: fxRainbow();   break;
    case 2: fxBreathe();   break;
    case 3: fxMeteor();    break;
    case 4: fxFire();      break;
    case 5: fxColorFade(); break;
    case 6: fxWarmGlow();  break;
    case 7: fxChase();     break;
    case 8: fxSlowFade();  break;
    default: fxSolid();    break;
  }
  renderEffectFrame();
}


// ============================================================
//  HTML 控制面板（网页手动开关灯带）
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LED 灯带控制台</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Noto+Sans+SC:wght@300;500&display=swap');
:root{--bg:#0b0d11;--card:#13161e;--border:#1f2430;--accent:#39e0ac;--warn:#ff5f5f;--text:#bec8da;--dim:#485068;}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:'Noto Sans SC',sans-serif;min-height:100vh;padding:22px 14px}
h1{font-family:'Share Tech Mono',monospace;font-size:15px;letter-spacing:5px;text-align:center;color:var(--accent);margin-bottom:22px;text-transform:uppercase}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px}
.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:10px}
.row4{display:grid;grid-template-columns:1fr 1fr 1fr 1fr;gap:8px;margin-bottom:14px}
.sbox{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px;text-align:center}
.row3 .sbox,.row4 .sbox{padding:10px}
.slbl{font-size:10px;letter-spacing:3px;text-transform:uppercase;color:var(--dim);margin-bottom:5px}
.sval{font-family:'Share Tech Mono',monospace;font-size:18px;font-weight:bold;color:var(--accent);transition:color .3s}
.row3 .sval,.row4 .sval{font-size:12px}
.sval.off{color:var(--warn)}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:18px;margin-bottom:13px}
.sec{font-size:10px;letter-spacing:4px;text-transform:uppercase;color:var(--dim);margin-bottom:14px}
label{display:block;font-size:11px;color:var(--dim);letter-spacing:1px;margin:13px 0 5px;text-transform:uppercase}
.vb{float:right;color:var(--accent);font-family:'Share Tech Mono',monospace;font-size:13px}
select,input[type=range]{width:100%;background:#0b0d11;border:1px solid var(--border);color:var(--text);padding:9px 12px;border-radius:7px;outline:none;font-family:'Share Tech Mono',monospace;font-size:13px}
select option{background:#13161e}
input[type=color]{width:100%;height:46px;border:1px solid var(--border);border-radius:7px;background:none;cursor:pointer;padding:3px}
input[type=file]{width:100%;padding:9px;background:#0b0d11;border:1px solid var(--border);color:var(--text);border-radius:7px;font-size:12px;margin-bottom:4px}
.pw{height:4px;background:var(--border);border-radius:2px;margin:10px 0 0;overflow:hidden;display:none}
.pf{height:100%;width:0%;background:var(--accent);transition:width .25s}
.btn{width:100%;padding:12px;background:linear-gradient(135deg,#39e0ac,#00b896);color:#0b0d11;border:none;border-radius:8px;font-weight:700;font-size:13px;letter-spacing:2px;cursor:pointer;margin-top:10px;font-family:'Share Tech Mono',monospace;text-transform:uppercase}
.btn:active{transform:scale(.98)}
.btn-ip{width:100%;padding:11px;background:linear-gradient(135deg,#4f46e5,#7c3aed);color:#fff;border:none;border-radius:8px;font-weight:700;font-size:12px;letter-spacing:2px;cursor:pointer;margin-top:12px;font-family:'Share Tech Mono',monospace;text-transform:uppercase;transition:opacity .2s}
.btn-ip:active{transform:scale(.98);opacity:.85}
#om{text-align:center;margin-top:9px;font-size:12px;color:var(--dim);font-family:'Share Tech Mono',monospace;min-height:18px}

/* IP */
.ileg{display:flex;justify-content:space-between;font-size:10px;color:var(--dim);letter-spacing:1px;margin-bottom:6px;font-family:'Share Tech Mono',monospace}
.lrow{display:flex;gap:2px}
.dot{flex:1;height:10px;border-radius:2px;background:#1a1e28;min-width:0;transition:background .4s,box-shadow .4s}
.dot.cy{background:#00c8ff;box-shadow:0 0 5px #00c8ff88}
.dot.or{background:#ff6418;box-shadow:0 0 5px #ff641888}
.dot.wh{background:#fff;box-shadow:0 0 8px #fff}
.iinfo{font-family:'Share Tech Mono',monospace;font-size:12px;color:var(--dim);text-align:center;margin-top:8px}
.irule{font-size:10px;color:#2e3545;text-align:center;margin-top:4px}
.ip-note{font-size:10px;color:#2e3545;text-align:center;margin-top:6px;font-style:italic}

/* 日志 */
.log-head{display:flex;align-items:center;gap:8px;margin-bottom:10px}
.log-head .sec{margin-bottom:0;flex:1}
.log-day{flex-shrink:0;background:#0b0d11;border:1px solid var(--border);color:var(--accent);padding:5px 10px;border-radius:6px;font-family:'Share Tech Mono',monospace;font-size:11px;outline:none}
.log-day option{background:#13161e}
.log-fs{font-size:10px;color:var(--dim);text-align:right;margin-bottom:6px;font-family:'Share Tech Mono',monospace}
.logbox{background:#090b0e;border:1px solid var(--border);border-radius:8px;padding:10px;height:180px;overflow-y:auto;font-family:'Share Tech Mono',monospace;font-size:11px;line-height:1.7;white-space:pre-wrap;word-break:break-all}
.lts{color:#2a4a3a}
.lmsg{color:#39e0ac}
</style>
</head>
<body>
<h1>// LED_CTRL · v7.5 AUTO ON</h1>

<div class="row2">
  <div class="sbox"><div class="slbl">灯带状态</div><div id="lv" class="sval">⚡ 自动运行</div></div>
  <div class="sbox"><div class="slbl">启动方式</div><div class="sval">通电自动亮</div></div>
</div>
<div class="row4">
  <div class="sbox"><div class="slbl">运行时长</div><div id="uv" class="sval">--:--:--</div></div>
  <div class="sbox"><div class="slbl">累计亮灯</div><div id="ov" class="sval">--:--:--</div></div>
  <div class="sbox"><div class="slbl">WiFi信号</div><div id="rv" class="sval">-- dBm</div></div>
  <div class="sbox"><div class="slbl">状态</div><div id="sv" class="sval">--</div></div>
</div>

<!-- 灯光控制 -->
<div class="card">
  <div class="sec">灯光控制</div>
  <label>效果模式</label>
  <select id="mode" onchange="send('mode',this.value)">
    <option value="0">SOLID      · 常亮纯色</option>
    <option value="1">RAINBOW    · 慢速彩虹流动</option>
    <option value="2">BREATHE    · 柔和呼吸灯</option>
    <option value="3">SOFT METEOR · 长尾柔和流星</option>
    <option value="4">WARM WAVE  · 暖色缓慢波动</option>
    <option value="5">COLOR FADE · 全灯慢速变色</option>
    <option value="6">WARM GLOW  · 暖光渐亮渐暗</option>
    <option value="7">SOFT CHASE · 柔和长尾追光</option>
    <option value="8">SLOW FADE  · 慢速渐亮渐暗（推荐）</option>
    <option value="9" style="color:#a78bfa;font-weight:bold">✦ RANDOM · 柔和随机切换(30s)</option>
  </select>
  <label>颜色 <span style="font-size:10px;color:var(--dim)">(常亮/追光/渐亮渐暗有效)</span></label>
  <input type="color" id="clr" onchange="setColor(this.value)">
  <label>亮度 <span class="vb" id="bv">150</span></label>
  <input type="range" id="bri" min="10" max="255"
    oninput="document.getElementById('bv').innerText=this.value"
    onchange="send('brightness',this.value)">
</div>

<!-- IP 暗号 -->
<div class="card">
  <div class="sec">IP 末段暗号</div>
  <div class="ileg"><span id="ll">LED 0</span><span id="lm"></span><span id="lr">LED 59</span></div>
  <div class="lrow" id="lrow"></div>
  <div class="iinfo" id="ii">等待连接...</div>
  <div class="irule" id="ir"></div>
  <button class="btn-ip" id="ipBtn" onclick="triggerIPAnim()">SHOW IP ANIMATION</button>
  <div class="ip-note">▲ 点击后灯带将以柔和亮度播放IP暗号动画（约5秒）</div>
</div>

<!-- 日志 (7天历史) -->
<div class="card">
  <div class="log-head">
    <div class="sec">设备日志</div>
    <select class="log-day" id="dayPicker" onchange="loadLog(this.value)">
      <option value="">加载中...</option>
    </select>
  </div>
  <div class="log-fs" id="fsInfo">存储: --</div>
  <div class="logbox" id="lb">(等待日志...)</div>
</div>

<!-- OTA -->
<div class="card">
  <div class="sec">OTA 固件升级</div>
  <input type="file" id="bin" accept=".bin">
  <div class="pw" id="pw"><div class="pf" id="pf"></div></div>
  <button class="btn" onclick="doOTA()">FLASH FIRMWARE</button>
  <div id="om"></div>
</div>

<script>
function send(k,v){fetch('/set?'+k+'='+v);}
function setColor(c){send('color',c.substring(1));}
function fmt(s){return[Math.floor(s/3600),Math.floor((s%3600)/60),s%60].map(n=>String(n).padStart(2,'0')).join(':');}
function fmtDate(d){
  if(d==='boot')return '启动日志';
  if(d.length===8)return d.substring(0,4)+'-'+d.substring(4,6)+'-'+d.substring(6,8);
  return d;
}

// IP 可视化
function buildIP(last){
  const N=60,row=document.getElementById('lrow');
  row.innerHTML='';
  const dots=[];
  for(let i=0;i<N;i++){const d=document.createElement('div');d.className='dot';row.appendChild(d);dots.push(d);}
  if(last<=N){
    for(let i=0;i<last;i++)dots[N-1-i].className='dot '+(i===last-1?'wh':'cy');
    document.getElementById('ii').innerText='📡 末尾'+last+'颗亮 = IP末段'+last;
    document.getElementById('ir').innerText='从LED59向左数亮灯颗数 = 你的IP末段';
    document.getElementById('ll').innerText='← 灭';document.getElementById('lm').innerText='';document.getElementById('lr').innerText='亮'+last+'颗 →';
  }else{
    const t=Math.floor(last/10),o=last%10;
    for(let i=0;i<t;i++)dots[i].className='dot or';
    for(let i=0;i<o;i++)dots[N-1-i].className='dot cy';
    document.getElementById('ii').innerText='📡 头'+t+'橙(十位) + 尾'+o+'青(个位) = '+last;
    document.getElementById('ir').innerText=t+'×10+'+o+'='+last;
    document.getElementById('ll').innerText='橙×'+t+'→';document.getElementById('lm').innerText='间隔';document.getElementById('lr').innerText='←青×'+o;
  }
}

// IP 动画手动触发
function triggerIPAnim(){
  const btn=document.getElementById('ipBtn');
  btn.disabled=true;
  btn.innerText='▶ 播放中...';
  fetch('/showip').catch(()=>{});
  setTimeout(()=>{btn.disabled=false;btn.innerText='SHOW IP ANIMATION';},7000);
}

// 日志加载
let currentDay='', autoScroll=true;
function renderLog(text){
  const box=document.getElementById('lb');
  box.innerHTML='';
  text.trim().split('\n').forEach(line=>{
    if(!line)return;
    const d=document.createElement('div');
    const m=line.match(/^(\[[^\]]+\])\s(.*)$/);
    d.innerHTML=m?'<span class="lts">'+m[1]+'</span> <span class="lmsg">'+m[2]+'</span>':'<span class="lmsg">'+line+'</span>';
    box.appendChild(d);
  });
  if(autoScroll)box.scrollTop=box.scrollHeight;
}
function loadLog(day){
  currentDay=day;
  autoScroll=(day===''||day===todayKey);
  fetch('/log?date='+day).then(r=>r.text()).then(t=>renderLog(t)).catch(()=>{});
}
let todayKey='';
function initLogList(){
  fetch('/loglist').then(r=>r.json()).then(list=>{
    const sel=document.getElementById('dayPicker');
    sel.innerHTML='';
    list.reverse().forEach(d=>{
      const opt=document.createElement('option');
      opt.value=d; opt.innerText=fmtDate(d);
      sel.appendChild(opt);
    });
    const latest=list.find(d=>d!=='boot')||list[0]||'';
    todayKey=latest;
    sel.value=latest;
    loadLog(latest);
  }).catch(()=>{});
  fetch('/fsinfo').then(r=>r.json()).then(d=>{
    const pct=((d.used/d.total)*100).toFixed(1);
    document.getElementById('fsInfo').innerText=
      '存储: '+(d.used/1024).toFixed(0)+'KB / '+(d.total/1024).toFixed(0)+'KB  ('+pct+'%)';
  }).catch(()=>{});
}

// 状态轮询
let loaded=false;
function ps(){
  fetch('/status').then(r=>r.json()).then(d=>{
    const lv=document.getElementById('lv');
    lv.innerText=d.light?'⚡ 自动运行':'等待启动';
    lv.className='sval'+(d.light?'':' off');
    document.getElementById('uv').innerText=fmt(d.uptime);
    document.getElementById('ov').innerText=fmt(d.onSec);
    const rv=document.getElementById('rv');
    rv.innerText=d.rssi+'dBm';
    rv.style.color=d.rssi>-65?'#39e0ac':d.rssi>-80?'#fbbf24':'#ff5f5f';

    const sv=document.getElementById('sv');
    if(d.light){sv.innerText='⚡ AUTO ON';sv.className='sval';}
    else if(d.wifi){sv.innerText='在线待机';sv.className='sval';}
    else{sv.innerText='WiFi断开';sv.className='sval off';}

    if(!loaded){
      document.getElementById('bri').value=d.brightness;
      document.getElementById('bv').innerText=d.brightness;
      document.getElementById('mode').value=d.mode;
      const h=n=>n.toString(16).padStart(2,'0');
      document.getElementById('clr').value='#'+h(d.r)+h(d.g)+h(d.b);
      fetch('/ip').then(r=>r.text()).then(ip=>{const p=ip.split('.');if(p.length===4)buildIP(parseInt(p[3]));}).catch(()=>{});
      initLogList();
      loaded=true;
    }
  }).catch(()=>{});
}

// 当前日志实时刷新
function pl(){
  if(!autoScroll)return;
  loadLog(currentDay);
}

setInterval(ps,1500);
setInterval(pl,3000);

// OTA
function doOTA(){
  const f=document.getElementById('bin').files[0];
  if(!f){alert('请先选择.bin固件文件');return;}
  const msg=document.getElementById('om');
  document.getElementById('pw').style.display='block';
  msg.style.color='';msg.innerText='上传中，请勿断电...';
  const fd=new FormData();fd.append('update',f,f.name);
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);document.getElementById('pf').style.width=p+'%';msg.innerText='写入'+p+'%';}};
  xhr.onload=()=>{
    if(xhr.status===200&&xhr.responseText==='OK'){document.getElementById('pf').style.width='100%';msg.style.color='#39e0ac';msg.innerText='✔ 升级成功！5s后重启...';setTimeout(()=>location.reload(),5000);}
    else{msg.style.color='#ff5f5f';msg.innerText='✘ 失败('+xhr.status+')';}
  };
  xhr.onerror=()=>{msg.style.color='#ff5f5f';msg.innerText='✘ 网络错误';};
  xhr.open('POST','/update');xhr.send(fd);
}
</script>
</body>
</html>
)rawliteral";


// ============================================================
//  云端笔记日志：每个北京时间自然日最多请求一次
// ============================================================
bool saveNoteAttemptDate(const char* date) {
  if (!prefs.begin("led", false)) return false;
  prefs.putString("noteDate", date);
  String saved = prefs.getString("noteDate", "");
  prefs.end();
  if (saved != date) return false;
  lastNoteAttemptDate = saved;
  return true;
}

void sendDailyNote() {
  if (!ntpSynced || WiFi.status() != WL_CONNECTED) return;

  struct tm ti;
  if (!getLocalTime(&ti, 500) || ti.tm_year <= 120) return;

  char today[9];
  snprintf(today, sizeof(today), "%04d%02d%02d",
           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);

  // 已经为今天预留过请求额度，断电重启后也不会重复请求。
  if (lastNoteAttemptDate == today) return;

  // 必须先把今天的日期写入 NVS，再发 HTTP 请求；严格保证最多一次尝试。
  // 这样即使服务器已经收到请求后设备突然断电，也不会在重启后重复发送。
  if (!saveNoteAttemptDate(today)) {
    LOG("云端笔记日期锁保存失败，取消今日请求，避免重复写入");
    return;
  }

  char timeStr[20];
  snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
           ti.tm_hour, ti.tm_min, ti.tm_sec);

  const char* modeNames[] = {"SOLID","RAINBOW","BREATHE","SOFT_METEOR",
                              "WARM_WAVE","COLOR_FADE","WARM_GLOW","SOFT_CHASE","SLOW_FADE","RANDOM"};
  const char* modeName = (cfg_mode >= 0 && cfg_mode <= 9) ? modeNames[cfg_mode] : "?";

  char logRow[256];
  snprintf(logRow, sizeof(logRow),
    "| %s | 开机 | IP: %s | RSSI: %ddBm | 效果: %s | 累计亮灯: %lus |",
    timeStr,
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI(),
    modeName,
    (unsigned long)stat_onSec);

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

  LOG("开始今日云端笔记请求，日期=%s", today);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, NOTE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cookie", String("auth_token=") + NOTE_AUTH);
  http.setTimeout(8000);

  int code = http.POST(payload);
  if (code == 200) {
    LOG("云端笔记写入成功 (HTTP %d)，今日不再请求", code);
  } else {
    LOG("云端笔记请求失败 (HTTP %d)，为避免重复，今日不再重试", code);
  }
  http.end();
}


// ============================================================
//  WiFi 工具
// ============================================================
bool connectWifi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // 不进入 WiFi modem sleep，保持网页控制响应
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("连接WiFi");

    // 联网等待时继续更新灯效，保证上电后不必等 Wi-Fi 连好才亮灯。
    const unsigned long connectStarted = millis();
    unsigned long lastDot = connectStarted;
    while (WiFi.status() != WL_CONNECTED && millis() - connectStarted < 10000UL) {
      runEffects();
      const unsigned long now = millis();
      if (now - lastDot >= 250UL) {
        Serial.print(".");
        lastDot = now;
      }
      delay(8);
    }
    Serial.println();
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTzTime("CST-8", "pool.ntp.org", "ntp.aliyun.com", "time.asia.apple.com");
    struct tm ti;
    ntpSynced = false;

    // 最多短暂等待约4秒同步时间，期间灯效仍然持续运行。
    const unsigned long ntpStarted = millis();
    while (!ntpSynced && millis() - ntpStarted < 4000UL) {
      if (getLocalTime(&ti, 50) && ti.tm_year > 120) {
        ntpSynced = true;
        break;
      }
      runEffects();
      delay(8);
    }
    LOG("WiFi已连接 IP:%s RSSI:%ddBm NTP:%s",
        WiFi.localIP().toString().c_str(), WiFi.RSSI(),
        ntpSynced ? "已同步北京时间" : "同步失败，稍后补同步");
    return true;
  }

  ntpSynced = false;
  LOG("WiFi连接失败，灯带继续运行，稍后自动重试");
  return false;
}


// ============================================================
//  Web 路由
// ============================================================
void setupRoutes() {
  server.on("/", HTTP_GET, [](){
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/ip", HTTP_GET, [](){
    server.send(200, "text/plain", WiFi.localIP().toString());
  });

  server.on("/showip", HTTP_GET, [](){
    server.send(200, "text/plain", "OK");
    LOG("手动触发IP暗号动画");
    showIPLastOctet();
  });

  server.on("/log", HTTP_GET, [](){
    String date = server.hasArg("date") ? server.arg("date") : "";
    server.send(200, "text/plain; charset=utf-8", getLogByDate(date.c_str()));
  });

  server.on("/loglist", HTTP_GET, [](){
    server.send(200, "application/json", getLogList());
  });

  server.on("/fsinfo", HTTP_GET, [](){
    server.send(200, "application/json", getFsInfo());
  });

  server.on("/status", HTTP_GET, [](){
    uint32_t curOn = stat_onSec;
    if (isLightOn && lightOnStart > 0) curOn += (millis() - lightOnStart) / 1000;
    char buf[300];
    snprintf(buf, sizeof(buf),
      "{\"light\":%s,\"r\":%d,\"g\":%d,\"b\":%d,"
      "\"brightness\":%d,\"mode\":%d,"
      "\"uptime\":%lu,\"onSec\":%lu,\"rssi\":%d,\"wifi\":%s}",
      isLightOn ? "true" : "false",
      cfg_r, cfg_g, cfg_b,
      cfg_brightness, cfg_mode,
      millis() / 1000, (unsigned long)curOn,
      (int)WiFi.RSSI(),
      WiFi.status() == WL_CONNECTED ? "true" : "false");
    server.send(200, "application/json", buf);
  });

  server.on("/set", HTTP_GET, [](){
    if (server.hasArg("color")) {
      String h=server.arg("color");
      if(h.length()>=6){
        cfg_r=strtol(h.substring(0,2).c_str(),NULL,16);
        cfg_g=strtol(h.substring(2,4).c_str(),NULL,16);
        cfg_b=strtol(h.substring(4,6).c_str(),NULL,16);
        if(cfg_mode!=9)cfg_mode=0;
      }
    }
    if(server.hasArg("brightness")){cfg_brightness=constrain(server.arg("brightness").toInt(),10,255);}
    if(server.hasArg("mode")){
      int m=server.arg("mode").toInt();
      m = constrain(m, 0, 9);
      if(m==9&&cfg_mode!=9){lastRandSwitch=millis();randCurrentMode=random(1,9);LOG("切换至柔和随机模式(当前效果%d)",randCurrentMode);}
      cfg_mode=m;
    }
    savePrefs();
    server.send(200,"text/plain","OK");
  });

  server.on("/update", HTTP_POST,
    [](){
      bool ok=!Update.hasError();
      server.sendHeader("Connection","close");
      server.send(200,"text/plain",ok?"OK":"FAIL");
      delay(200);
      if(ok){LOG("OTA完成，重启中...");ESP.restart();}
    },
    [](){
      HTTPUpload& up=server.upload();
      if(up.status==UPLOAD_FILE_START){LOG("OTA开始: %s",up.filename.c_str());if(!Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH))Update.printError(Serial);}
      else if(up.status==UPLOAD_FILE_WRITE){if(Update.write(up.buf,up.currentSize)!=up.currentSize)Update.printError(Serial);}
      else if(up.status==UPLOAD_FILE_END){if(Update.end(true))LOG("OTA写入%u字节完毕",up.totalSize);else Update.printError(Serial);}
      else if(up.status==UPLOAD_FILE_ABORTED){Update.abort();LOG("OTA中止");}
    }
  );

  server.begin();
  routesConfigured = true;
  LOG("HTTP服务器已启动");
}


// ============================================================
//  setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== LED 灯带控制器 v7.5 自动亮灯版 ===");

  loadPrefs();

  if (!LittleFS.begin(true)) {
    Serial.println("⚠ LittleFS 初始化失败！");
  } else {
    Serial.printf("✅ LittleFS 就绪  已用:%uKB / 总计:%uKB\n",
                  (unsigned)LittleFS.usedBytes() / 1024,
                  (unsigned)LittleFS.totalBytes() / 1024);
  }

  memset(effectFrame, 0, sizeof(effectFrame));
  memset(lastShownFrame, 0, sizeof(lastShownFrame));
  memset(transitionFromFrame, 0, sizeof(transitionFromFrame));
  strip.begin();
  strip.setBrightness(255);
  stripOff();

  // 上电即开始运行当前保存的灯效；网页不再提供点亮/关闭按钮。
  isLightOn = true;
  lightOnStart = millis();
  lastOnSecSave = lightOnStart;
  LOG("系统启动：灯带自动点亮  累计亮灯=%lu秒  最近云端笔记日期=%s",
      (unsigned long)stat_onSec,
      lastNoteAttemptDate.length() ? lastNoteAttemptDate.c_str() : "无");

  wifiOnline = connectWifi();
  lastWifiRetry = millis();
  if (wifiOnline) {
    setupRoutes();
    sendDailyNote();
  }
}


// ============================================================
//  loop
// ============================================================
void loop() {
  unsigned long now = millis();

  // WiFi 断线后自动重连；不再依赖红外传感器唤醒网络。
  if (wifiOnline && WiFi.status() != WL_CONNECTED) {
    wifiOnline = false;
    ntpSynced = false;
    server.stop();
    LOG("WiFi连接中断，将自动重连");
  }

  if (!wifiOnline && (lastWifiRetry == 0 || now - lastWifiRetry >= WIFI_RETRY_INTERVAL)) {
    lastWifiRetry = now;
    wifiOnline = connectWifi();
    if (wifiOnline) {
      if (!routesConfigured) {
        setupRoutes();
      } else {
        server.begin();
        LOG("HTTP服务器已重新启动");
      }
      sendDailyNote();
    }
  }

  if (wifiOnline) server.handleClient();

  // NTP 补同步：时间恢复后检查当天的云端笔记额度。
  if (wifiOnline && !ntpSynced) {
    static unsigned long lastRetry = 0;
    if (now - lastRetry > 30000UL) {
      lastRetry = now;
      struct tm ti;
      if (getLocalTime(&ti, 1000) && ti.tm_year > 120) {
        ntpSynced = true;
        LOG("NTP补同步成功 → 北京时间");
        sendDailyNote();
      }
    }
  }

  // 灯带通电自动运行，无人体检测、手动点亮按钮或自动休眠。
  if (isLightOn) runEffects();
  persistOnSecondsIfDue();
  delay(isLightOn ? 8 : 50);
}
