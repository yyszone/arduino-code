/**
 * ============================================================
 *  智能感应灯 v7.2 — ESP32-C3 + HC-SR501 + WS2812B (60珠)
 *
 *  v7.2 新增:
 *   [★] Light Sleep 省电模式
 *       灯灭 + WiFi断开后自动进入 Light Sleep
 *       PIR 高电平 GPIO 唤醒，唤醒延迟 < 2ms
 *       60s 定时器兜底唤醒（防止 GPIO 边沿丢失）
 *       网页可实时开关省电模式（NVS持久化）
 *       新增睡眠状态指示 + 唤醒次数统计
 *
 *  v7.1 功能全部保留:
 *   [✓] 日志写入 LittleFS 按日期分文件 / 7天轮转 / 64KB截断
 *   [✓] 所有设置 NVS 掉电永久保存
 *   [✓] 效果模式下拉 + RANDOM随机切换(30s)
 *   [✓] 三层省电 + 累计亮灯时长
 *   [✓] 日志时间戳北京时间 / NTP自动补同步
 *   [✓] IP末段暗号动画（网页按钮手动触发）
 *   [✓] 9种灯效 / OTA修复 / 双重灭灯防残留
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

// [★ v7.2] Light Sleep 所需头文件
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_pm.h"

// ==================== 用户配置区 ====================
const char* WIFI_SSID     = "yang1234";
const char* WIFI_PASSWORD = "y123456789";

#define PIR_PIN         4
#define NEOPIXEL_PIN    5
#define LED_COUNT       60

#define WIFI_OFF_DELAY  60000UL   // 灯灭后多久断WiFi (ms)
#define RAND_INTERVAL   30000UL   // 随机模式切换间隔 (ms)
#define LOG_KEEP_DAYS   7         // 保留天数
#define LOG_MAX_BYTES   65536     // 每个日志文件上限 64KB

// [★ v7.2] Light Sleep 配置
#define SLEEP_TIMER_US  (60ULL * 1000000ULL)  // 最长睡60s自动唤醒一次做维护
#define PIR_DEBOUNCE_MS 80                    // PIR唤醒后防抖等待

// ---- 云端笔记 API (开机写入一次) ----
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
uint32_t cfg_timeout    = 15000UL;
uint32_t stat_onSec     = 0;
bool     cfg_sleep_en   = true;   // [★ v7.2] Light Sleep 总开关

// ---- 运行时状态 ----
bool          pirTriggered   = false;
bool          isLightOn      = false;
bool          wifiOnline     = false;
bool          ntpSynced      = false;
bool          bootLogSent    = false;
unsigned long lastMotionTime = 0;
unsigned long lightOffTime   = 0;
unsigned long lightOnStart   = 0;

// [★ v7.2] Light Sleep 运行时状态
bool          inSleepReady   = false;   // 已满足入睡条件
uint32_t      stat_wakeCount = 0;       // 累计唤醒次数
unsigned long lastSleepEnter = 0;       // 最近一次进入睡眠的时间戳

// ---- 随机模式 ----
int           randCurrentMode = 1;
unsigned long lastRandSwitch  = 0;

// ---- 火焰热场 ----
static uint8_t fireHeat[LED_COUNT];

// ---- 日志文件状态 ----
static char logCurDate[9] = {0};


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
  cfg_timeout    = prefs.getULong("tout", 15000UL);
  stat_onSec     = prefs.getULong("ons",  0);
  cfg_sleep_en   = prefs.getBool ("slp",  true);    // [★ v7.2]
  stat_wakeCount = prefs.getULong("wkc",  0);       // [★ v7.2]
  prefs.end();
}

void savePrefs() {
  prefs.begin("led", false);
  prefs.putUChar("r",   cfg_r);
  prefs.putUChar("g",   cfg_g);
  prefs.putUChar("b",   cfg_b);
  prefs.putUChar("bri", cfg_brightness);
  prefs.putInt  ("mode",cfg_mode);
  prefs.putULong("tout",cfg_timeout);
  prefs.putBool ("slp", cfg_sleep_en);   // [★ v7.2]
  prefs.end();
}

void saveOnSec() {
  prefs.begin("led", false);
  prefs.putULong("ons", stat_onSec);
  prefs.end();
}

// [★ v7.2] 单独持久化唤醒次数（避免频繁写全量）
void saveWakeCount() {
  prefs.begin("led", false);
  prefs.putULong("wkc", stat_wakeCount);
  prefs.end();
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
  strip.clear(); strip.show(); delay(2);
  strip.clear(); strip.show();
}

uint32_t colorWheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return strip.Color(255-pos*3, 0, pos*3);
  if (pos < 170) { pos-=85; return strip.Color(0, pos*3, 255-pos*3); }
  pos -= 170;    return strip.Color(pos*3, 255-pos*3, 0);
}


// ============================================================
//  [★ v7.2] Light Sleep 核心函数
// ============================================================

/**
 * 配置 PIR GPIO 为唤醒源（HIGH电平触发）
 * 注意：gpio_wakeup_enable 只支持 GPIO 输入模式
 */
void setupSleepWakeup() {
  // 确保 PIR 引脚为输入
  gpio_set_direction((gpio_num_t)PIR_PIN, GPIO_MODE_INPUT);
  // HIGH 电平触发唤醒（HC-SR501 检测到人时输出 HIGH）
  gpio_wakeup_enable((gpio_num_t)PIR_PIN, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
}

/**
 * 进入 Light Sleep
 *  - 调用前必须已调用 setupSleepWakeup()
 *  - 同时启用定时器兜底（maxUs）防止 GPIO 电平丢失
 *  - 返回后 CPU/外设立即恢复，RAM 完整保留
 *  - 返回值: true=GPIO唤醒(PIR), false=定时器唤醒
 */
bool doLightSleep(uint64_t maxUs = SLEEP_TIMER_US) {
  // 确保灯带完全关闭（防止 NeoPixel 保持电流）
  stripOff();

  // 串口 flush，避免睡眠截断输出
  Serial.flush();

  // 启用定时器兜底
  esp_sleep_enable_timer_wakeup(maxUs);

  // ---- 进入 Light Sleep ----
  lastSleepEnter = millis();
  esp_light_sleep_start();
  // ---- 从此处恢复，CPU 继续运行 ----

  // 关闭定时器（避免影响下次睡眠）
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool gpioWake = (cause == ESP_SLEEP_WAKEUP_GPIO);

  stat_wakeCount++;

  if (gpioWake) {
    // 防抖：等待 PIR 信号稳定
    delay(PIR_DEBOUNCE_MS);
    bool confirmed = (digitalRead(PIR_PIN) == HIGH);
    LOG("Light Sleep 唤醒[GPIO] PIR=%s  累计唤醒=%lu次",
        confirmed ? "HIGH✓" : "LOW(抖动)", (unsigned long)stat_wakeCount);
    return confirmed;
  } else {
    // 定时器唤醒——静默维护，不打日志（节省闪存写入）
    return false;
  }
}

/**
 * loop() 中调用：判断是否应该入睡，并执行睡眠循环
 * 条件：灯灭 + WiFi已断 + lightOffTime倒计时已清零 + 省电开关开
 */
void sleepIfIdle() {
  if (!cfg_sleep_en)    return;
  if (isLightOn)        return;
  if (wifiOnline)       return;
  if (lightOffTime > 0) return;   // 仍在 WiFi 断开倒计时中

  // 每次进入睡眠前记录一次（仅第一次）
  if (!inSleepReady) {
    inSleepReady = true;
    LOG("进入 Light Sleep 待机，等待 PIR 唤醒...");
    saveWakeCount();
    setupSleepWakeup();
  }

  // 执行一次睡眠（阻塞至唤醒）
  bool pirWoke = doLightSleep();

  if (pirWoke) {
    // PIR 确认有人 → 退出睡眠状态，由主 loop 接管点灯逻辑
    inSleepReady = false;
    // 不在此处点灯，让 loop 正常流程处理
  }
  // 定时器唤醒 → inSleepReady 保持 true，下一帧继续睡
}


// ============================================================
//  IP末段暗号动画（保留功能，改为手动触发）
// ============================================================
void showIPLastOctet() {
  IPAddress ip = WiFi.localIP();
  uint8_t last = ip[3];
  LOG("IP暗号: %s  末段=%d", ip.toString().c_str(), last);

  strip.setBrightness(220); strip.clear();
  for (int f=0;f<3;f++){
    strip.fill(strip.Color(55,55,55)); strip.show(); delay(90);
    strip.clear(); strip.show(); delay(110);
  }
  delay(250);

  if (last <= LED_COUNT) {
    for (int i=0;i<(int)last;i++){
      int idx=LED_COUNT-1-i;
      if(i>0){float f=0.55f+0.45f*((float)i/last);strip.setPixelColor(idx+1,strip.Color(0,(uint8_t)(200*f),(uint8_t)(255*f)));}
      strip.setPixelColor(idx,strip.Color(255,255,255)); strip.show(); delay(35);
    }
    delay(100);
    if(last>0){strip.setPixelColor(LED_COUNT-last,strip.Color(0,200,255));strip.show();}
    LOG("  末尾%d颗亮=IP末段%d",last,last);
  } else {
    uint8_t tens=last/10,ones=last%10;
    for(int i=0;i<(int)tens;i++){
      if(i>0){float f=0.5f+0.5f*((float)i/tens);strip.setPixelColor(i-1,strip.Color((uint8_t)(255*f),(uint8_t)(100*f),0));}
      strip.setPixelColor(i,strip.Color(255,255,255)); strip.show(); delay(45);
    }
    for(int i=0;i<(int)tens;i++){float f=0.5f+0.5f*((float)i/tens);strip.setPixelColor(i,strip.Color((uint8_t)(255*f),(uint8_t)(100*f),0));}
    strip.show(); delay(400);
    for(int i=0;i<(int)ones;i++){
      int idx=LED_COUNT-1-i;
      if(i>0)strip.setPixelColor(idx+1,strip.Color(0,200,255));
      strip.setPixelColor(idx,strip.Color(255,255,255)); strip.show(); delay(80);
    }
    if(ones>0){strip.setPixelColor(LED_COUNT-ones,strip.Color(0,200,255));strip.show();}
    LOG("  头%d橙+尾%d青=%d",tens,ones,last);
  }
  delay(3000);
  for(int b=220;b>=0;b-=6){strip.setBrightness(b);strip.show();delay(16);}
  strip.clear(); strip.show();
  strip.setBrightness(cfg_brightness);
  LOG("IP暗号显示完毕");
}


// ============================================================
//  灯效 (全部非阻塞)
// ============================================================
void fxSolid(){strip.setBrightness(cfg_brightness);strip.fill(strip.Color(cfg_r,cfg_g,cfg_b));strip.show();}
void fxRainbow(){static unsigned long t=0;static uint16_t j=0;if(millis()-t<18)return;t=millis();for(int i=0;i<LED_COUNT;i++)strip.setPixelColor(i,colorWheel(((i*256/LED_COUNT)+j)&255));strip.show();j=(j+1)%256;}
void fxBreathe(){static unsigned long t=0;if(millis()-t<15)return;t=millis();float s=(exp(sin(millis()/2000.0*PI))-0.36787944f)*108.0f;strip.setBrightness(constrain((uint8_t)s,8,255));strip.fill(strip.Color(cfg_r,cfg_g,cfg_b));strip.show();}
void fxMeteor(){
  static unsigned long t=0;static int pos=0;if(millis()-t<22)return;t=millis();
  for(int i=0;i<LED_COUNT;i++){uint32_t c=strip.getPixelColor(i);strip.setPixelColor(i,strip.Color(((c>>16)&0xFF)*180/255,((c>>8)&0xFF)*180/255,(c&0xFF)*180/255));}
  for(int j=0;j<8;j++){int idx=pos-j;if(idx>=0&&idx<LED_COUNT){float f=1.0f-(float)j/8.0f;strip.setPixelColor(idx,strip.Color((uint8_t)(255*f),(uint8_t)(220*f),(uint8_t)(140*f)));}}
  strip.show();if(++pos>=LED_COUNT+10)pos=0;
}
void fxFire(){
  static unsigned long t=0;if(millis()-t<28)return;t=millis();
  for(int i=0;i<LED_COUNT;i++)fireHeat[i]=(uint8_t)max(0,(int)fireHeat[i]-(int)random(0,(55*10/LED_COUNT)+2));
  for(int k=LED_COUNT-1;k>=2;k--)fireHeat[k]=(fireHeat[k-1]+fireHeat[k-2]*2)/3;
  if(random(255)<120)fireHeat[random(5)]=(uint8_t)min(255,(int)fireHeat[random(5)]+(int)random(160,255));
  for(int i=0;i<LED_COUNT;i++){uint8_t h=fireHeat[i],r2,g2,b2;
    if(h<85){r2=h*3;g2=0;b2=0;}else if(h<170){r2=255;g2=(h-85)*3;b2=0;}else{r2=255;g2=255;b2=(h-170)*3;}
    strip.setPixelColor(i,strip.Color(r2,g2,b2));}strip.show();
}
void fxTwinkle(){
  static unsigned long t=0;if(millis()-t<35)return;t=millis();
  for(int i=0;i<LED_COUNT;i++){uint32_t c=strip.getPixelColor(i);strip.setPixelColor(i,strip.Color(((c>>16)&0xFF)*210/255,((c>>8)&0xFF)*210/255,(c&0xFF)*210/255));}
  for(int k=0;k<random(2,5);k++)strip.setPixelColor(random(LED_COUNT),colorWheel(random(256)));strip.show();
}
void fxPolice(){
  static unsigned long t=0;static int ph=0;if(millis()-t<75)return;t=millis();strip.clear();
  switch(ph%4){case 0:for(int i=0;i<LED_COUNT/2;i++)strip.setPixelColor(i,strip.Color(255,0,0));break;
    case 2:for(int i=LED_COUNT/2;i<LED_COUNT;i++)strip.setPixelColor(i,strip.Color(0,60,255));break;default:break;}
  strip.show();ph++;
}
void fxChase(){
  static unsigned long t=0;static int pos=0;if(millis()-t<38)return;t=millis();strip.clear();
  strip.setPixelColor(pos,strip.Color(cfg_r,cfg_g,cfg_b));
  for(int j=1;j<=8;j++){int idx=(pos-j+LED_COUNT)%LED_COUNT;float f=1.0f-(float)j/9.0f;strip.setPixelColor(idx,strip.Color((uint8_t)(cfg_r*f),(uint8_t)(cfg_g*f),(uint8_t)(cfg_b*f)));}
  strip.show();pos=(pos+1)%LED_COUNT;
}
void fxLightning(){
  static unsigned long nxt=0,fend=0;static bool act=false;unsigned long now=millis();
  if(act){if(now<fend){strip.fill(strip.Color(180+random(75),180+random(75),255));strip.show();delay(random(8,35));strip.clear();strip.show();delay(random(4,18));}
    else{act=false;strip.clear();strip.show();nxt=now+random(800,4000);}}
  else if(now>=nxt){act=true;fend=now+random(120,450);}
}

void runEffects(){
  int eff=cfg_mode;
  if(cfg_mode==9){
    unsigned long now=millis();
    if(now-lastRandSwitch>=RAND_INTERVAL){int nx;do{nx=random(1,9);}while(nx==randCurrentMode);randCurrentMode=nx;lastRandSwitch=now;LOG("随机切换→效果%d",randCurrentMode);}
    eff=randCurrentMode;
  }
  strip.setBrightness(cfg_brightness);
  switch(eff){case 1:fxRainbow();break;case 2:fxBreathe();break;case 3:fxMeteor();break;case 4:fxFire();break;
    case 5:fxTwinkle();break;case 6:fxPolice();break;case 7:fxChase();break;case 8:fxLightning();break;default:fxSolid();break;}
}


// ============================================================
//  HTML 控制面板  (v7.2 新增省电模式卡片)
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>感应灯控制台</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Noto+Sans+SC:wght@300;500&display=swap');
:root{--bg:#0b0d11;--card:#13161e;--border:#1f2430;--accent:#39e0ac;--warn:#ff5f5f;--text:#bec8da;--dim:#485068;--sleep:#7c3aed;}
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
.sval.slp{color:var(--sleep)}
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

/* ---- [★ v7.2] 省电模式开关 ---- */
.sleep-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}
.sleep-label{font-size:12px;color:var(--text);font-family:'Noto Sans SC',sans-serif}
.sleep-sub{font-size:10px;color:var(--dim);margin-top:2px}
.toggle{position:relative;width:48px;height:26px;flex-shrink:0}
.toggle input{opacity:0;width:0;height:0}
.slider{position:absolute;inset:0;background:#1a1e28;border-radius:13px;cursor:pointer;transition:background .3s;border:1px solid var(--border)}
.slider:before{content:'';position:absolute;width:20px;height:20px;left:2px;top:2px;background:#485068;border-radius:50%;transition:transform .3s,background .3s}
.toggle input:checked + .slider{background:#3b1f7a;border-color:var(--sleep)}
.toggle input:checked + .slider:before{transform:translateX(22px);background:var(--sleep);box-shadow:0 0 8px #7c3aed88}
.sleep-stats{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:12px}
.sleep-stat{background:#090b0e;border:1px solid var(--border);border-radius:8px;padding:10px;text-align:center}
.sleep-stat .lbl{font-size:9px;letter-spacing:2px;color:var(--dim);text-transform:uppercase}
.sleep-stat .val{font-family:'Share Tech Mono',monospace;font-size:14px;color:var(--sleep);margin-top:4px}
.sleep-badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:10px;font-family:'Share Tech Mono',monospace;letter-spacing:1px;margin-left:8px}
.sleep-badge.on{background:#1e1040;color:var(--sleep);border:1px solid #3b1f7a}
.sleep-badge.off{background:#1a0a0a;color:#666;border:1px solid #2a1010}
.sleep-note{font-size:10px;color:var(--dim);margin-top:10px;line-height:1.6;padding:8px;background:#090b0e;border-radius:6px;border-left:2px solid var(--sleep)}

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
<h1>// LED_CTRL · v7.2</h1>

<div class="row2">
  <div class="sbox"><div class="slbl">PIR</div><div id="pv" class="sval off">○ 无人</div></div>
  <div class="sbox"><div class="slbl">灯带</div><div id="lv" class="sval off">▼ 关闭</div></div>
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
    <option value="0">SOLID     · 常亮纯色</option>
    <option value="1">RAINBOW   · 动态彩虹</option>
    <option value="2">BREATHE   · 呼吸灯</option>
    <option value="3">METEOR    · 流星雨 ✦</option>
    <option value="4">FIRE      · 火焰 🔥</option>
    <option value="5">TWINKLE   · 星光 ✧</option>
    <option value="6">POLICE    · 警灯 🚨</option>
    <option value="7">CHASE     · 追光 →</option>
    <option value="8">LIGHTNING · 闪电 ⚡</option>
    <option value="9" style="color:#a78bfa;font-weight:bold">✦ RANDOM  · 随机切换(30s)</option>
  </select>
  <label>颜色 <span style="font-size:10px;color:var(--dim)">(纯色/追光/呼吸有效)</span></label>
  <input type="color" id="clr" onchange="setColor(this.value)">
  <label>亮度 <span class="vb" id="bv">150</span></label>
  <input type="range" id="bri" min="10" max="255"
    oninput="document.getElementById('bv').innerText=this.value"
    onchange="send('brightness',this.value)">
  <label>无人超时 <span class="vb"><span id="tv">15</span> s</span></label>
  <input type="range" id="tout" min="1" max="300"
    oninput="document.getElementById('tv').innerText=this.value"
    onchange="send('timeout',this.value)">
</div>

<!-- [★ v7.2] 省电模式 -->
<div class="card">
  <div class="sec">省电模式
    <span class="sleep-badge" id="slpBadge">-- --</span>
  </div>
  <div class="sleep-row">
    <div>
      <div class="sleep-label">Light Sleep 自动待机</div>
      <div class="sleep-sub">灯灭 + WiFi断开后自动进入，PIR触发立即唤醒</div>
    </div>
    <label class="toggle">
      <input type="checkbox" id="slpToggle" onchange="setSleep(this.checked)">
      <span class="slider"></span>
    </label>
  </div>
  <div class="sleep-stats">
    <div class="sleep-stat">
      <div class="lbl">累计唤醒</div>
      <div class="val" id="wkc">--</div>
    </div>
    <div class="sleep-stat">
      <div class="lbl">当前状态</div>
      <div class="val" id="slpState">--</div>
    </div>
  </div>
  <div class="sleep-note">
    💡 Light Sleep 期间 CPU 暂停，RAM 保留，功耗降至 &lt;1mA（不含 PIR 模块自身 ~65µA）<br>
    唤醒延迟 &lt;2ms，对日常使用无感知。关闭此开关将保持 WiFi 监听（~15mA）。
  </div>
</div>

<!-- IP 暗号 -->
<div class="card">
  <div class="sec">IP 末段暗号</div>
  <div class="ileg"><span id="ll">LED 0</span><span id="lm"></span><span id="lr">LED 59</span></div>
  <div class="lrow" id="lrow"></div>
  <div class="iinfo" id="ii">等待连接...</div>
  <div class="irule" id="ir"></div>
  <button class="btn-ip" id="ipBtn" onclick="triggerIPAnim()">SHOW IP ANIMATION</button>
  <div class="ip-note">▲ 点击后灯带将播放IP暗号动画（约5秒）</div>
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
function setSleep(en){
  send('sleep', en?'1':'0');
  document.getElementById('slpBadge').className='sleep-badge '+(en?'on':'off');
  document.getElementById('slpBadge').innerText=en?'● ENABLED':'○ DISABLED';
}
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
    const pv=document.getElementById('pv'),lv=document.getElementById('lv');
    pv.innerText=d.pir?'● 有人':'○ 无人';pv.className='sval'+(d.pir?'':' off');
    lv.innerText=d.light?'▲ 点亮':'▼ 关闭';lv.className='sval'+(d.light?'':' off');
    document.getElementById('uv').innerText=fmt(d.uptime);
    document.getElementById('ov').innerText=fmt(d.onSec);
    const rv=document.getElementById('rv');rv.innerText=d.rssi+'dBm';
    rv.style.color=d.rssi>-65?'#39e0ac':d.rssi>-80?'#fbbf24':'#ff5f5f';

    // [★ v7.2] 状态栏
    const sv=document.getElementById('sv');
    if(d.sleeping){sv.innerText='💤 SLEEP';sv.className='sval slp';}
    else if(d.light){sv.innerText='💡 ON';sv.className='sval';}
    else{sv.innerText='待机';sv.className='sval off';}

    // [★ v7.2] 省电模式卡片
    document.getElementById('wkc').innerText=d.wakeCount||'0';
    document.getElementById('slpState').innerText=d.sleeping?'💤 睡眠中':(d.sleepEn?'就绪':'已关闭');

    if(!loaded){
      document.getElementById('bri').value=d.brightness;document.getElementById('bv').innerText=d.brightness;
      document.getElementById('tout').value=Math.round(d.timeout/1000);document.getElementById('tv').innerText=Math.round(d.timeout/1000);
      document.getElementById('mode').value=d.mode;
      const h=n=>n.toString(16).padStart(2,'0');
      document.getElementById('clr').value='#'+h(d.r)+h(d.g)+h(d.b);

      // 省电开关初始化
      document.getElementById('slpToggle').checked=d.sleepEn;
      const badge=document.getElementById('slpBadge');
      badge.className='sleep-badge '+(d.sleepEn?'on':'off');
      badge.innerText=d.sleepEn?'● ENABLED':'○ DISABLED';

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
//  云端笔记开机日志
// ============================================================
void sendBootLog() {
  if (bootLogSent) return;
  if (!ntpSynced)  return;
  if (WiFi.status() != WL_CONNECTED) return;

  struct tm ti;
  if (!getLocalTime(&ti, 500) || ti.tm_year <= 120) return;

  char timeStr[20];
  snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
           ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
           ti.tm_hour, ti.tm_min, ti.tm_sec);

  const char* modeNames[] = {"SOLID","RAINBOW","BREATHE","METEOR",
                              "FIRE","TWINKLE","POLICE","CHASE","LIGHTNING","RANDOM"};
  const char* modeName = (cfg_mode>=0 && cfg_mode<=9) ? modeNames[cfg_mode] : "?";

  char logRow[256];
  // 在 sendBootLog 函数中，修改 snprintf 部分：

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
           utcTm.tm_year+1900, utcTm.tm_mon+1, utcTm.tm_mday,
           utcTm.tm_hour, utcTm.tm_min, utcTm.tm_sec);

  char payload[450];
  snprintf(payload, sizeof(payload),
    "{\"noteId\":\"%s\",\"appendText\":\"%s\",\"updatedAt\":\"%s\"}",
    NOTE_ID, logRow, updatedAt);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, NOTE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cookie", String("auth_token=") + NOTE_AUTH);
  http.setTimeout(8000);

  int code = http.POST(payload);
  if (code == 200) {
    bootLogSent = true;
    LOG("云端笔记写入成功 (HTTP %d)", code);
  } else {
    LOG("云端笔记写入失败 (HTTP %d)", code);
  }
  http.end();
}


// ============================================================
//  WiFi 工具
// ============================================================
bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("连接WiFi");
  for (int i=0; i<40 && WiFi.status()!=WL_CONNECTED; i++) { delay(250); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime("CST-8", "pool.ntp.org", "ntp.aliyun.com", "time.asia.apple.com");
    struct tm ti; ntpSynced = false;
    for (int i=0; i<20; i++) {
      if (getLocalTime(&ti,500) && ti.tm_year>120) { ntpSynced=true; break; }
      delay(500);
    }
    LOG("WiFi已连接 IP:%s RSSI:%ddBm NTP:%s",
        WiFi.localIP().toString().c_str(), WiFi.RSSI(),
        ntpSynced?"已同步北京时间":"同步失败");
    return true;
  }
  LOG("WiFi连接失败，离线运行");
  return false;
}

void disconnectWifi() {
  if (!wifiOnline) return;
  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiOnline = false;
  inSleepReady = false;   // [★ v7.2] 重置睡眠标志，下次进入时重新配置唤醒源
  LOG("WiFi已断开（省电）");
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
    if (isLightOn && lightOnStart>0) curOn += (millis()-lightOnStart)/1000;
    char buf[380];
    snprintf(buf, sizeof(buf),
      "{\"pir\":%s,\"light\":%s,\"r\":%d,\"g\":%d,\"b\":%d,"
      "\"brightness\":%d,\"timeout\":%lu,\"mode\":%d,"
      "\"uptime\":%lu,\"onSec\":%lu,\"rssi\":%d,"
      "\"sleepEn\":%s,\"sleeping\":%s,\"wakeCount\":%lu}",
      pirTriggered?"true":"false",
      isLightOn?"true":"false",
      cfg_r, cfg_g, cfg_b,
      cfg_brightness, cfg_timeout, cfg_mode,
      millis()/1000, (unsigned long)curOn,
      (int)WiFi.RSSI(),
      // [★ v7.2]
      cfg_sleep_en?"true":"false",
      inSleepReady?"true":"false",
      (unsigned long)stat_wakeCount);
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
    if(server.hasArg("brightness")){cfg_brightness=constrain(server.arg("brightness").toInt(),10,255);strip.setBrightness(cfg_brightness);}
    if(server.hasArg("mode")){
      int m=server.arg("mode").toInt();
      if(m==9&&cfg_mode!=9){lastRandSwitch=millis()-RAND_INTERVAL;randCurrentMode=random(1,9);LOG("切换至随机模式(当前效果%d)",randCurrentMode);}
      cfg_mode=m;
    }
    if(server.hasArg("timeout"))cfg_timeout=(uint32_t)server.arg("timeout").toInt()*1000UL;
    // [★ v7.2] 省电模式开关
    if(server.hasArg("sleep")){
      bool en = server.arg("sleep") == "1";
      if(en != cfg_sleep_en){
        cfg_sleep_en = en;
        if(!en) inSleepReady = false;   // 关闭时清除待机状态
        LOG("Light Sleep %s", en?"已启用":"已关闭");
      }
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
  LOG("HTTP服务器已启动");
}


// ============================================================
//  setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== 智能感应灯 v7.2 ===");

  loadPrefs();

  if (!LittleFS.begin(true)) {
    Serial.println("⚠ LittleFS 初始化失败！");
  } else {
    Serial.printf("✅ LittleFS 就绪  已用:%uKB / 总计:%uKB\n",
                  (unsigned)LittleFS.usedBytes()/1024,
                  (unsigned)LittleFS.totalBytes()/1024);
  }

  pinMode(PIR_PIN, INPUT);
  memset(fireHeat, 0, sizeof(fireHeat));

  strip.begin();
  strip.setBrightness(cfg_brightness);
  stripOff();

  LOG("系统启动  累计亮灯=%lu秒  累计唤醒=%lu次  Light Sleep=%s",
      (unsigned long)stat_onSec,
      (unsigned long)stat_wakeCount,
      cfg_sleep_en ? "开" : "关");

  wifiOnline = connectWifi();
  if (wifiOnline) {
    setupRoutes();
    sendBootLog();
  }
}


// ============================================================
//  loop
// ============================================================
void loop() {
  unsigned long now = millis();

  if (wifiOnline) server.handleClient();

  // NTP补同步
  if (wifiOnline && !ntpSynced) {
    static unsigned long lastRetry = 0;
    if (now - lastRetry > 30000UL) {
      lastRetry = now;
      struct tm ti;
      if (getLocalTime(&ti,1000) && ti.tm_year>120) {
        ntpSynced = true;
        LOG("NTP补同步成功 → 北京时间");
        sendBootLog();
      }
    }
  }

  // PIR 检测
  bool newPir = (digitalRead(PIR_PIN) == HIGH);
  if (newPir && !pirTriggered && !wifiOnline) {
    LOG("检测到人，重连WiFi...");
    wifiOnline = connectWifi();
    if (wifiOnline) setupRoutes();
  }
  pirTriggered = newPir;

  // 点灯逻辑
  if (pirTriggered) {
    lastMotionTime = now;
    if (!isLightOn) {
      isLightOn=true; lightOnStart=now;
      inSleepReady=false;   // [★ v7.2] 唤醒后退出睡眠就绪状态
      strip.setBrightness(cfg_brightness);
      LOG("灯带点亮 效果=%d", cfg_mode);
    }
  }

  // 超时灭灯
  if (isLightOn && (now-lastMotionTime > cfg_timeout)) {
    if (lightOnStart>0) stat_onSec += (now-lightOnStart)/1000;
    lightOnStart=0; isLightOn=false; lightOffTime=now;
    stripOff();
    LOG("超时熄灯  累计亮灯=%lu秒", (unsigned long)stat_onSec);
    saveOnSec();
  }

  // 运行灯效
  if (isLightOn) runEffects();

  // WiFi 断开计时
  if (!isLightOn && wifiOnline && lightOffTime>0 &&
      (now-lightOffTime > WIFI_OFF_DELAY)) {
    disconnectWifi();
    lightOffTime=0;
  }

  // [★ v7.2] Light Sleep 入口
  // 条件：灯灭 + WiFi已断 + 不在WiFi断开倒计时 + 开关开启
  sleepIfIdle();

  // 动态 delay（睡眠模式不走此处，已在 sleepIfIdle 阻塞）
  if (!inSleepReady) {
    delay((isLightOn || pirTriggered) ? 8 : 200);
  }
}
