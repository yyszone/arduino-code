// =================================================================
// log.h — 独立系统事件日志归档模块
// =================================================================
#ifndef LOG_H
#define LOG_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <time.h>

constexpr const char* LOG_FILE_PATH = "/system_event.log";
constexpr size_t MAX_LOG_FILE_SIZE = 8192; // 日志最大保留 8KB

// 写入日志
inline void sysLog(const String& msg) {
    time_t now = time(nullptr);
    char timeBuf[32];
    if (now > 1000000000L) {
        struct tm* lt = localtime(&now);
        strftime(timeBuf, sizeof(timeBuf), "%m-%d %H:%M:%S", lt);
    } else {
        snprintf(timeBuf, sizeof(timeBuf), "Up:%lu s", millis() / 1000UL);
    }

    String logLine = "[" + String(timeBuf) + "] " + msg;
    Serial.println("[LOG] " + logLine);

    // 日志文件尺寸检查，超限自动清空重置
    if (LittleFS.exists(LOG_FILE_PATH)) {
        File checkF = LittleFS.open(LOG_FILE_PATH, "r");
        if (checkF) {
            if (checkF.size() > MAX_LOG_FILE_SIZE) {
                checkF.close();
                File resetF = LittleFS.open(LOG_FILE_PATH, "w");
                if (resetF) {
                    resetF.println("[" + String(timeBuf) + "] 日志已自动轮转清理");
                    resetF.close();
                }
            } else {
                checkF.close();
            }
        }
    }

    File f = LittleFS.open(LOG_FILE_PATH, "a");
    if (f) {
        f.println(logLine);
        f.close();
    }
}

// 获取日志文本
inline String getLogsText() {
    if (!LittleFS.exists(LOG_FILE_PATH)) {
        return "暂无日志记录";
    }
    File f = LittleFS.open(LOG_FILE_PATH, "r");
    if (!f) return "读取日志文件失败";
    
    String content = "";
    content.reserve(MAX_LOG_FILE_SIZE);
    while (f.available()) {
        content += (char)f.read();
    }
    f.close();
    return content;
}

// 清空日志
inline void clearLogs() {
    LittleFS.remove(LOG_FILE_PATH);
    sysLog("日志已手动清空");
}

#endif