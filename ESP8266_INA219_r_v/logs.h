#ifndef LOGS_H
#define LOGS_H

// =============================================================
//  SysLog — LittleFS 不易失日志  v1.0
//  用法：
//    sysLog(LOG_BOOT, "设备启动");
//    sysLogf(LOG_WARN, "电压低: %.2fV", v);
//  接线无需改动，重启后日志保留，浏览器访问 /logs 查看
// =============================================================

#include <LittleFS.h>
#include <ESP8266WebServer.h>
#include <time.h>
#include <stdarg.h>

#define LOG_FILE      "/syslog.txt"
#define LOG_MAX_LINES  80           // 最多保留80条，约8KB
#define LOG_LINE_MAX   128

#define LOG_BOOT  "BOOT"
#define LOG_INFO  "INFO"
#define LOG_WARN  "WARN"
#define LOG_ERR   "ERR "

// ─── 内部：统计行数 ───────────────────────────────────────────
static int _logCountLines() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return 0;
  int n = 0;
  while (f.available()) if (f.read() == '\n') n++;
  f.close();
  return n;
}

// ─── 内部：超限时裁掉最早的行 ────────────────────────────────
static void _logTrim() {
  int lines = _logCountLines();
  if (lines <= LOG_MAX_LINES) return;
  File src = LittleFS.open(LOG_FILE, "r");
  if (!src) return;
  int skip = lines - LOG_MAX_LINES;
  for (int i = 0; i < skip; i++) src.readStringUntil('\n');
  String rest = src.readString();
  src.close();
  File dst = LittleFS.open(LOG_FILE, "w");
  if (dst) { dst.print(rest); dst.close(); }
}

// ─── 核心写入 ─────────────────────────────────────────────────
void sysLog(const char* level, const char* msg) {
  char line[LOG_LINE_MAX];
  time_t now = time(nullptr);

  if (now > 1000000UL) {
    struct tm* t = localtime(&now);
    snprintf(line, sizeof(line), "[%02d-%02d %02d:%02d:%02d] %s: %s\n",
      t->tm_mon + 1, t->tm_mday,
      t->tm_hour, t->tm_min, t->tm_sec,
      level, msg);
  } else {
    // NTP 未同步时用运行时长代替
    unsigned long s = millis() / 1000;
    snprintf(line, sizeof(line), "[+%02lu:%02lu:%02lu] %s: %s\n",
      s / 3600, (s % 3600) / 60, s % 60, level, msg);
  }

  Serial.print(line);

  File f = LittleFS.open(LOG_FILE, "a");
  if (f) { f.print(line); f.close(); }
  _logTrim();
}

// ─── 格式化版本 ────────────────────────────────────────────────
void sysLogf(const char* level, const char* fmt, ...) {
  char msg[LOG_LINE_MAX - 32];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  sysLog(level, msg);
}

// ─── 开机自动记录复位原因（在 setup() 最早调用） ──────────────
void logBootReason() {
  String reason = ESP.getResetReason();   // 关键：判断是崩溃还是正常开机
  String info   = ESP.getResetInfo();

  sysLogf(LOG_BOOT, "======== 设备启动 ========");
  sysLogf(LOG_BOOT, "复位原因: %s", reason.c_str());

  // 只有异常/WDT时才写详细info（避免刷屏）
  if (reason.indexOf("Exception") >= 0 || reason.indexOf("Watchdog") >= 0) {
    sysLogf(LOG_ERR,  "崩溃详情: %s", info.c_str());
  }

  sysLogf(LOG_BOOT, "堆内存: %u B  芯片ID: %06X",
    ESP.getFreeHeap(), ESP.getChipId());
}

// ─── 注册 Web 端点（在 setup() 调用一次） ────────────────────
void setupLogEndpoint(ESP8266WebServer& srv) {

  // GET /logs — 日志查看页
  srv.on("/logs", HTTP_GET, [&srv]() {
    String body = "";
    File f = LittleFS.open(LOG_FILE, "r");
    if (f) { body = f.readString(); f.close(); }
    else   { body = "（暂无日志）"; }

    // HTML 转义
    body.replace("&",  "&amp;");
    body.replace("<",  "&lt;");
    body.replace(">",  "&gt;");
    // 着色
    body.replace("] BOOT:", "] <b class='b'>BOOT:</b>");
    body.replace("] WARN:", "] <b class='w'>WARN:</b>");
    body.replace("] ERR :", "] <b class='e'>ERR :</b>");
    body.replace("] INFO:", "] <b class='i'>INFO:</b>");

    String html;
    html.reserve(512 + body.length());
    html  = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>系统日志</title><style>"
              "*{box-sizing:border-box}"
              "body{background:#0d1117;color:#b0c4b1;font-family:'Courier New',monospace;"
              "font-size:12.5px;margin:0;padding:14px}"
              "h2{color:#38bdf8;font-size:14px;letter-spacing:3px;margin:0 0 12px}"
              "pre{white-space:pre-wrap;word-break:break-all;line-height:1.8;"
              "background:#111827;border-radius:10px;padding:14px;margin:0}"
              ".t{color:#374151}"           /* 时间戳 */
              ".b{color:#a78bfa}"           /* BOOT 紫 */
              ".w{color:#fbbf24}"           /* WARN 黄 */
              ".e{color:#f87171}"           /* ERR  红 */
              ".i{color:#34d399}"           /* INFO 绿 */
              ".bar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px}"
              "a{color:#38bdf8;background:#1f2937;padding:6px 14px;"
              "border-radius:6px;text-decoration:none;font-size:12px}"
              "a.clr{color:#f87171;background:#1f1010}"
              "</style></head><body>"
              "<h2>📋 系统日志</h2>"
              "<div class='bar'>"
              "<a href='/'>← 主页</a>"
              "<a href='/logs'>🔄 刷新</a>"
              "<a class='clr' href='/logs/clear' "
              "onclick=\"return confirm('确认清空全部日志？')\">🗑 清空</a>"
              "</div><pre>");
    html += body;
    html += F("</pre></body></html>");

    srv.send(200, "text/html; charset=utf-8", html);
  });

  // GET /logs/clear — 清空
  srv.on("/logs/clear", HTTP_GET, [&srv]() {
    LittleFS.remove(LOG_FILE);
    sysLog(LOG_INFO, "日志已手动清空");
    srv.sendHeader("Location", "/logs");
    srv.send(303);
  });
}

#endif // LOGS_H