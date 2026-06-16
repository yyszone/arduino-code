#ifndef LOGS_H
#define LOGS_H

#include <LittleFS.h>
#include <WebServer.h> // 【修改】
#include <time.h>
#include <stdarg.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#define LOG_FILE      "/syslog.txt"
#define LOG_MAX_LINES  80           // 最多保留80条
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

// === 强制只解析 IPv4 (A记录) 辅助函数 ===
String resolveIPv4(const char* host) {
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;       // 强制只获取 IPv4 地址 (A记录)
  hints.ai_socktype = SOCK_STREAM; // 流套接字 (TCP)

  int err = getaddrinfo(host, NULL, &hints, &res);
  if (err != 0 || res == NULL) {
    Serial.printf("[DNS] IPv4 解析失败: %s\n", host);
    return "";
  }

  // 获取解析出的 IPv4 结构体并转换为字符串
  struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
  char ipstr[16];
  inet_ntop(AF_INET, &(ipv4->sin_addr), ipstr, sizeof(ipstr));
  freeaddrinfo(res); // 必须释放内存

  Serial.printf("[DNS] 强制 IPv4 解析成功: %s -> %s\n", host, ipstr);
  return String(ipstr);
}

// ─── 【重大修改】ESP32-C3 专属引导原因诊断 ────────────────────
void logBootReason() {
  esp_reset_reason_t reason = esp_reset_reason(); // 【修改】使用 ESP32 标准 API
  String reasonStr = "";
  switch (reason) {
    case ESP_RST_POWERON:   reasonStr = "上电复位 / 复位键重启"; break;
    case ESP_RST_EXT:       reasonStr = "外部引脚复位"; break;
    case ESP_RST_SW:        reasonStr = "软件自主重启 (Restart)"; break;
    case ESP_RST_PANIC:     reasonStr = "系统崩溃/内核恐慌 (Panic)"; break;
    case ESP_RST_INT_WDT:   reasonStr = "中断看门狗复位"; break;
    case ESP_RST_TASK_WDT:  reasonStr = "任务看门狗超时复位"; break;
    case ESP_RST_WDT:       reasonStr = "其他看门狗复位"; break;
    case ESP_RST_DEEPSLEEP: reasonStr = "深度睡眠唤醒重启"; break;
    case ESP_RST_BROWNOUT:  reasonStr = "电压不稳低压复位 (Brownout)"; break;
    case ESP_RST_SDIO:      reasonStr = "SDIO 传输复位"; break;
    default:                reasonStr = "未知原因"; break;
  }

  sysLogf(LOG_BOOT, "======== 设备启动 ========");
  sysLogf(LOG_BOOT, "复位原因: %s", reasonStr.c_str());

  if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT) {
    sysLogf(LOG_ERR, "系统异常重启！建议检查供电及系统内存完整度。");
  }

  // ESP32 eFuse 唯一 MAC 作为芯片ID
  uint32_t chipId = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  sysLogf(LOG_BOOT, "空闲堆内存: %u B  芯片ID: %08X",
    ESP.getFreeHeap(), chipId);
}

// ─── 注册 Web 端点 ────────────────────
void setupLogEndpoint(WebServer& srv) { // 【修改】WebServer

  srv.on("/logs", HTTP_GET, [&srv]() {
    String body = "";
    File f = LittleFS.open(LOG_FILE, "r");
    if (f) { body = f.readString(); f.close(); }
    else   { body = "（暂无日志）"; }

    body.replace("&",  "&amp;");
    body.replace("<",  "&lt;");
    body.replace(">",  "&gt;");
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
              ".t{color:#374151}"
              ".b{color:#a78bfa}"
              ".w{color:#fbbf24}"
              ".e{color:#f87171}"
              ".i{color:#34d399}"
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

  srv.on("/logs/clear", HTTP_GET, [&srv]() {
    LittleFS.remove(LOG_FILE);
    sysLog(LOG_INFO, "日志已手动清空");
    srv.sendHeader("Location", "/logs");
    srv.send(303);
  });
}

#endif