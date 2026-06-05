#pragma once
/*
 * fan_note.h — 每日云端笔记上报（每天只写一次）
 *
 * 逻辑：
 *   - 0 点之后，WiFi 连通状态下，找到昨天的统计数据
 *   - 向笔记 API append 一行 Markdown 表格行
 *   - 写入成功后标记"今天已写"，重启后从 LittleFS 恢复标记
 *
 * 表格格式：
 *   | 时间 | IP | 昨天开风扇时间 | 累计开风扇 |
 *
 * 在 loop() 里调用 fanNote_loop() 即可，不阻塞。
 */

#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <time.h>

// ──── 笔记 API 配置 ────────────────────────────────────────
#define FAN_NOTE_URL  "https://note.yysresume.work/api/note-op"
#define FAN_NOTE_ID   "0cad9dc8-d703-4d20-9d75-cc0ec457b788"
#define FAN_NOTE_AUTH "a_secret_fixed_token"

#define NOTE_DONE_FILE "/fan_note_done.txt"  // 存"今天已写"的日期

// ──── 内部状态 ─────────────────────────────────────────────
static bool _noteWrittenToday = false;
static char _noteDoneDate[12] = "";   // "YYYY-MM-DD"

// ──── 初始化：读取上次已写日期 ────────────────────────────
void fanNote_begin() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  if (LittleFS.exists(NOTE_DONE_FILE)) {
    File f = LittleFS.open(NOTE_DONE_FILE, "r");
    if (f) {
      String s = f.readStringUntil('\n');
      s.trim();
      strncpy(_noteDoneDate, s.c_str(), sizeof(_noteDoneDate) - 1);
      f.close();
    }
  }
  Serial.printf("[FanNote] Last note written date: %s\n", _noteDoneDate);
}

// ──── 辅助：把秒数格式化为可读字符串（>60s 用分钟）────────
static String _fmtSec(unsigned long s) {
  if (s == 0) return "0 秒";
  if (s < 60) return String(s) + " 秒";
  char buf[24];
  snprintf(buf, sizeof(buf), "%.1f 分钟", s / 60.0);
  return String(buf);
}

// ──── 读取指定日期的风扇秒数（从 fan_daily.txt）────────────
static unsigned long _readDaySec(const char* dateStr) {
  if (!LittleFS.exists("/fan_daily.txt")) return 0;
  File f = LittleFS.open("/fan_daily.txt", "r");
  unsigned long result = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 11) continue;
    int tab = line.indexOf('\t');
    if (tab < 0) continue;
    if (line.substring(0, tab) == String(dateStr)) {
      result = line.substring(tab + 1).toInt();
      break;
    }
  }
  f.close();
  return result;
}

// ──── 读取累计总开风扇秒数（所有日期之和）────────────────
static unsigned long _readTotalSec() {
  if (!LittleFS.exists("/fan_daily.txt")) return 0;
  File f = LittleFS.open("/fan_daily.txt", "r");
  unsigned long total = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 3) continue;
    int tab = line.indexOf('\t');
    if (tab >= 0) total += line.substring(tab + 1).toInt();
  }
  f.close();
  return total;
}

// ──── 实际发送请求 ─────────────────────────────────────────
static bool _postNote(const char* dateYesterday, unsigned long daySec) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 1024);

  HTTPClient http;
  if (!http.begin(client, FAN_NOTE_URL)) {
    Serial.println("[FanNote] http.begin failed");
    return false;
  }

  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(9000);

  // 获取 UTC 时间戳（updatedAt 字段）
  time_t utcNow = time(nullptr);
  struct tm utcTm;
  gmtime_r(&utcNow, &utcTm);
  char updatedAt[25];
  snprintf(updatedAt, sizeof(updatedAt), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           utcTm.tm_year + 1900, utcTm.tm_mon + 1, utcTm.tm_mday,
           utcTm.tm_hour, utcTm.tm_min, utcTm.tm_sec);

  // 获取今天（写入时的）本地时间
  time_t localNow = time(nullptr);
  struct tm* lt = localtime(&localNow);
  char nowStr[20];
  strftime(nowStr, sizeof(nowStr), "%Y-%m-%d %H:%M", lt);

  // 读取累计
  unsigned long totalSec = _readTotalSec();

  // 拼装表格行（转义反斜杠以放入 JSON 字符串）
  // | 时间 | IP | 昨天开风扇时间 | 累计开风扇 |
  String fanYest = _fmtSec(daySec);
  String fanTotal = _fmtSec(totalSec);
  String ip = WiFi.localIP().toString();

  // appendText 里的竖线需要用 \n 换行，JSON 里转义
  char tableRow[220];
  snprintf(tableRow, sizeof(tableRow),
    "| %s | %s | %s(%s) | %s |",
    nowStr, ip.c_str(),
    fanYest.c_str(), dateYesterday,
    fanTotal.c_str()
  );

  // 构建 JSON payload（appendText 换行符用 \\n）
  char payload[512];
  snprintf(payload, sizeof(payload),
    "{\"noteId\":\"%s\",\"appendText\":\"%s\",\"updatedAt\":\"%s\"}",
    FAN_NOTE_ID, tableRow, updatedAt
  );

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cookie", String("auth_token=") + FAN_NOTE_AUTH);

  int code = http.POST(payload);
  bool ok = (code == 200 || code == 201);
  Serial.printf("[FanNote] POST %d %s\n", code, ok ? "OK" : "FAIL");
  http.end();
  return ok;
}

// ──── loop 调用入口 ────────────────────────────────────────
void fanNote_loop() {
  // 必须 WiFi 已连接
  if (WiFi.status() != WL_CONNECTED) return;

  time_t now = time(nullptr);
  if (now < 8 * 3600 * 2) return;   // NTP 未同步（时间戳太小）

  struct tm* lt = localtime(&now);

  // 今天日期
  char today[12];
  strftime(today, sizeof(today), "%Y-%m-%d", lt);

  // 已经写过今天了
  if (_noteWrittenToday && strcmp(_noteDoneDate, today) == 0) return;

  // 检查重启后的持久标记
  if (strcmp(_noteDoneDate, today) == 0) {
    _noteWrittenToday = true;
    return;
  }

  // 0 点之后才执行（hour >= 0，即任何时刻都可，但必须是"今天"的统计还没写）
  // 昨天日期
  time_t yesterday_t = now - 86400;
  struct tm* yt = localtime(&yesterday_t);
  char yesterday[12];
  strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", yt);

  // 读昨天的风扇秒数
  unsigned long daySec = _readDaySec(yesterday);

  Serial.printf("[FanNote] Posting note for %s, fan=%lus\n", yesterday, daySec);

  bool ok = _postNote(yesterday, daySec);
  if (ok) {
    _noteWrittenToday = true;
    strncpy(_noteDoneDate, today, sizeof(_noteDoneDate) - 1);

    // 持久化今天已写的标记
    File f = LittleFS.open(NOTE_DONE_FILE, "w");
    if (f) { f.println(today); f.close(); }
    Serial.printf("[FanNote] Note written for date %s\n", today);
  } else {
    Serial.println("[FanNote] Post failed, will retry next loop.");
  }
}