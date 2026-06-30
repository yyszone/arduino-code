// =================================================================
// note.h — 独立 Daily Note 自动打卡推送模块
// =================================================================
#pragma once

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <time.h>

#define NOTE_URL   "https://note.yysresume.work/api/note-op"
#define NOTE_ID    "7587e26e-87cc-4721-bb0a-23cf5ca7d574"
#define NOTE_AUTH  "a_secret_fixed_token"
#define NOTE_DONE_FILE "/note_done_date.txt"

// 前置枚举声明
enum class TripReason : uint8_t {
    NONE = 0,
    OVERVOLTAGE,
    UNDERVOLTAGE,
    OVERCURRENT,
    MANUAL
};

// 状态结构体定义（供主程序及本模块共同使用）
struct SystemState {
    float busVoltage   = 0.f;  // V
    float shuntVoltage = 0.f;  // mV
    float current_mA   = 0.f;  // mA
    float power_mW     = 0.f;  // mW

    bool       relayOn       = true;  
    bool       faultLatched  = false; 
    TripReason tripReason    = TripReason::NONE;
    unsigned long tripEpoch  = 0;     
    uint8_t       retryCount = 0;

    unsigned long todayOnSec     = 0;   // 今天开启总秒数 [2]
    unsigned long yesterdayOnSec = 0;   // 昨天开启总秒数 [2]
    double        cumulativeWh   = 0.0; // 累计电能消耗 (瓦时 Wh)
    char          lastLoggedDate[12] = ""; // 上一次打卡的日期

    unsigned long lastSampleMs   = 0;
    unsigned long lastSaveMs     = 0; 
    uint8_t       confirmCounter = 0;
};

// 声明外部全局变量，由主程序实例化并链接
extern SystemState st;
extern void saveSystemState(); 

static bool _notePosting = false; // 防 HTTPS 重入锁

// 格式化时长
static String _fmtSec(unsigned long s) {
  if (s == 0) return "0 秒";
  if (s < 60) return String(s) + " 秒";
  if (s < 3600) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f 分钟", s / 60.0);
    return String(buf);
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "%.2f 小时", s / 3600.0);
  return String(buf);
}

// HTTPS 提交打卡
static bool _postNote(const char* targetDate, unsigned long runSec, double cumulativeWh) {
  WiFiClientSecure client;
  client.setInsecure(); // 绕过 HTTPS 证书链校验 [2]
  client.setBufferSizes(1024, 1024);

  HTTPClient http;
  if (!http.begin(client, NOTE_URL)) {
    Serial.println("[DailyNote] http.begin 建立失败");
    return false;
  }

  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(9000);

  time_t utcNow = time(nullptr);
  struct tm utcTm;
  gmtime_r(&utcNow, &utcTm);
  char updatedAt[25];
  snprintf(updatedAt, sizeof(updatedAt), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           utcTm.tm_year + 1900, utcTm.tm_mon + 1, utcTm.tm_mday,
           utcTm.tm_hour, utcTm.tm_min, utcTm.tm_sec);

  time_t localNow = time(nullptr);
  struct tm* lt = localtime(&localNow);
  char nowStr[20];
  strftime(nowStr, sizeof(nowStr), "%Y-%m-%d %H:%M", lt);

  String ip = WiFi.localIP().toString();
  String fanTime = _fmtSec(runSec);
  String energyText = String(cumulativeWh, 2) + " Wh";

  // 符合您期望的 Markdown 表格输出：| 时间 | IP | 昨天开风扇时间 | 累计功率 | [2]
  char tableRow[256];
  snprintf(tableRow, sizeof(tableRow),
    "| %s | %s | %s (%s) | %s |",
    nowStr, ip.c_str(),
    fanTime.c_str(), targetDate,
    energyText.c_str()
  );

  char payload[512];
  snprintf(payload, sizeof(payload),
    "{\"noteId\":\"%s\",\"appendText\":\"%s\",\"updatedAt\":\"%s\"}",
    NOTE_ID, tableRow, updatedAt
  );

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cookie", String("auth_token=") + NOTE_AUTH);

  int code = http.POST(payload);
  bool ok = (code == 200 || code == 201);
  Serial.printf("[DailyNote] POST %d %s\n", code, ok ? "OK" : "FAIL");
  http.end();
  return ok;
}

// 初始化打卡标志
void note_begin() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  if (LittleFS.exists(NOTE_DONE_FILE)) {
    File f = LittleFS.open(NOTE_DONE_FILE, "r");
    if (f) {
      String s = f.readStringUntil('\n');
      s.trim();
      strncpy(st.lastLoggedDate, s.c_str(), sizeof(st.lastLoggedDate) - 1);
      f.close();
    }
  }
  Serial.printf("[DailyNote] 上次记录日期: %s\n", st.lastLoggedDate);
}

// 跨天检查与自动提交 (loop 轮询)
void note_loop() {
  if (WiFi.status() != WL_CONNECTED) return;

  time_t now = time(nullptr);
  if (now < 8 * 3600 * 2) return; // 确保时钟已通过网络成功同步

  if (_notePosting) return;

  struct tm* lt = localtime(&now);
  char today[12];
  strftime(today, sizeof(today), "%Y-%m-%d", lt);

  // 1. 本日已打卡则退出
  if (strcmp(st.lastLoggedDate, today) == 0) {
    return;
  }

  // 2. 跨天自动触发：计算昨日数据，开始推送
  time_t yesterday_t = now - 86400;
  struct tm* yt = localtime(&yesterday_t);
  char yesterday[12];
  strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", yt);

  Serial.printf("[DailyNote] 触发跨天打卡。发送日期: %s, 开启时长: %lus\n", yesterday, st.todayOnSec);

  // 乐观锁：先写入标志文件，防止断电崩溃后死循环重发
  File f = LittleFS.open(NOTE_DONE_FILE, "w");
  if (f) { f.println(today); f.close(); }

  // 滚动并留存昨日数据，重置今日时长
  st.yesterdayOnSec = st.todayOnSec;
  st.todayOnSec = 0; // 重置今天开机时长 [2]
  strncpy(st.lastLoggedDate, today, sizeof(st.lastLoggedDate) - 1);
  saveSystemState();

  _notePosting = true; // 上锁
  bool ok = _postNote(yesterday, st.yesterdayOnSec, st.cumulativeWh);
  _notePosting = false; // 解锁

  if (ok) {
    Serial.printf("[DailyNote] 日期为 %s 的记录成功打卡\n", today);
  } else {
    // 提交失败（例如网络闪断），回滚标志文件和内存状态，下次 loop 重试
    Serial.println("[DailyNote] 提交失败，撤销标记下次尝试重试");
    memset(st.lastLoggedDate, 0, sizeof(st.lastLoggedDate));
    LittleFS.remove(NOTE_DONE_FILE);
    saveSystemState();
  }
}