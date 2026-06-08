#pragma once

#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <time.h>
#include "fan_stats.h"   // 【修复】直接读 EEPROM，不再依赖文件时序

#define FAN_NOTE_URL  "https://note.yysresume.work/api/note-op"
#define FAN_NOTE_ID   "0cad9dc8-d703-4d20-9d75-cc0ec457b788"
#define FAN_NOTE_AUTH "a_secret_fixed_token"
#define NOTE_DONE_FILE "/fan_note_done.txt"

// 【修复】用两个标志分离"本次运行已写"和"文件记录已写"，防止重入
static bool _noteWrittenToday  = false;   // 本次运行内存标志
static bool _notePosting       = false;   // 防重入锁
static char _noteDoneDate[12]  = "";

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

static String _fmtSec(unsigned long s) {
  if (s == 0) return "0 秒";
  if (s < 60) return String(s) + " 秒";
  char buf[24];
  snprintf(buf, sizeof(buf), "%.1f 分钟", s / 60.0);
  return String(buf);
}

static unsigned long _readTotalSec() {
  unsigned long total = 0;
  for (int i = 0; i < (int)_eepData.count; i++) {
    total += _eepData.entries[i].secs;
  }
  return total;
}

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

  unsigned long totalSec = _readTotalSec();
  String fanYest  = _fmtSec(daySec);
  String fanTotal = _fmtSec(totalSec);
  String ip = WiFi.localIP().toString();

  char tableRow[220];
  snprintf(tableRow, sizeof(tableRow),
    "| %s | %s | %s(%s) | %s |",
    nowStr, ip.c_str(),
    fanYest.c_str(), dateYesterday,
    fanTotal.c_str()
  );

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
  if (WiFi.status() != WL_CONNECTED) return;

  time_t now = time(nullptr);
  if (now < 8 * 3600 * 2) return;   // NTP 未同步

  // 【修复】防重入：HTTPS 发送期间不允许再次进入
  if (_notePosting) return;

  struct tm* lt = localtime(&now);
  char today[12];
  strftime(today, sizeof(today), "%Y-%m-%d", lt);

  // 内存标志优先（本次运行已写过，直接跳过）
  if (_noteWrittenToday && strcmp(_noteDoneDate, today) == 0) return;

  // 文件标志：本次启动后首次进来，同步内存标志
  if (strcmp(_noteDoneDate, today) == 0) {
    _noteWrittenToday = true;
    return;
  }

  // ──── 到这里才真正需要发送 ────────────────────────────────

  // 【修复】先计算好昨天数据，再写乐观锁文件
  // 这样即使崩溃重启，也能保证数据已经在发送时是正确的
  uint32_t yesterdayKey = fanStats_yesterdayKey();
  unsigned long daySec  = fanStats_getDaySec(yesterdayKey);  // 直接读 EEPROM，无时序问题

  time_t yesterday_t = now - 86400;
  struct tm* yt = localtime(&yesterday_t);
  char yesterday[12];
  strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", yt);

  Serial.printf("[FanNote] Posting note for %s, fan=%lus\n", yesterday, daySec);

  // 乐观锁：先写文件，防止崩溃后重发
  File f = LittleFS.open(NOTE_DONE_FILE, "w");
  if (f) { f.println(today); f.close(); }
  strncpy(_noteDoneDate, today, sizeof(_noteDoneDate) - 1);
  _noteWrittenToday = true;
  _notePosting      = true;   // 上锁

  bool ok = _postNote(yesterday, daySec);
  _notePosting = false;       // 解锁

  if (ok) {
    Serial.printf("[FanNote] Note successfully written for date %s\n", today);
  } else {
    // 发送失败（非崩溃），撤销标记，下次 loop 重试
    Serial.println("[FanNote] Post failed, removing local done flag to retry later.");
    _noteWrittenToday = false;
    memset(_noteDoneDate, 0, sizeof(_noteDoneDate));
    LittleFS.remove(NOTE_DONE_FILE);
  }
}