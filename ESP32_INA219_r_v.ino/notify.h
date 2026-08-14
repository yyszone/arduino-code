#ifndef NOTIFY_H
#define NOTIFY_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>

namespace Notify {

static const char* CONFIG_FILE = "/notify_cfg.txt";

// 默认 API
static String apiUrl = "https://notify.yysresume.work/api/send";

// --------------------------------------------------
// URL 编码
// --------------------------------------------------
String urlEncode(const String& str) {
    String encoded;
    encoded.reserve(str.length() + 16);

    const char* hex = "0123456789ABCDEF";

    for (size_t i = 0; i < str.length(); i++) {
        uint8_t c = (uint8_t)str[i];

        if (
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~'
        ) {
            encoded += (char)c;
        } else {
            encoded += '%';
            encoded += hex[(c >> 4) & 0x0F];
            encoded += hex[c & 0x0F];
        }
    }

    return encoded;
}

// --------------------------------------------------
// 加载配置
// --------------------------------------------------
void loadConfig() {
    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("[Notify] 未找到配置，使用默认 API");
        return;
    }

    File file = LittleFS.open(CONFIG_FILE, "r");
    if (!file) {
        Serial.println("[Notify] 打开配置失败");
        return;
    }

    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() > 0) {
        apiUrl = line;
    }

    file.close();

    Serial.printf("[Notify] API: %s\n", apiUrl.c_str());
}

// --------------------------------------------------
// 保存配置
// --------------------------------------------------
bool saveConfig(const String& url) {
    if (url.length() < 8) {
        return false;
    }

    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) {
        Serial.println("[Notify] 创建配置失败");
        return false;
    }

    file.println(url);
    file.close();

    apiUrl = url;

    Serial.printf("[Notify] API 已保存: %s\n", apiUrl.c_str());
    return true;
}

// --------------------------------------------------
// 获取 API 地址
// --------------------------------------------------
String getApiUrl() {
    return apiUrl;
}

// --------------------------------------------------
// 判断是否已经配置
// --------------------------------------------------
bool configured() {
    return apiUrl.startsWith("http://") ||
           apiUrl.startsWith("https://");
}

// --------------------------------------------------
// 发送通知
//
// 对应原来的 curl:
//
// curl -X POST https://notify.yysresume.work/api/send \
// -F "title=🚨 逆变器低电压警告" \
// -F "message=逆变器通知" \
// -F "level=critical" \
// -F "project=逆变器" \
// -F "ip=esp32c3-smart-relay ip" \
// -F "ttl=3600" \
// -F 'actions=[...]'
// --------------------------------------------------
bool send(
    const String& title,
    const String& message,
    const String& deviceName,
    const String& level = "critical",
    const String& project = "逆变器",
    int ttl = 3600
) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Notify] WiFi 未连接");
        return false;
    }

    if (!configured()) {
        Serial.println("[Notify] API 未配置");
        return false;
    }

    // --------------------------------------------
    // 当前 ESP32 IP
    // --------------------------------------------
    String localIP = WiFi.localIP().toString();

    // 点击通知后关闭继电器
    //
    // 你的 WebServer 是 HTTP_GET
    // 所以必须 state=0 + GET
    //
    String actionUrl =
        "http://" +
        localIP +
        "/setRelay?state=0";

    String actions =
        "[{\"action\":\"down\","
        "\"title\":\"点击关闭逆变器\","
        "\"url\":\"" + actionUrl + "\","
        "\"method\":\"GET\"}]";

    // --------------------------------------------
    // multipart/form-data
    // --------------------------------------------
    String boundary = "----ESP32NotifyBoundary7MA4YWxkTrZu0gW";

    String body;

    auto addField = [&](const String& name, const String& value) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + name + "\"\r\n";
        body += "\r\n";
        body += value;
        body += "\r\n";
    };

    addField("title", title);
    addField("message", message);
    addField("level", level);
    addField("project", project);
    addField("ip", deviceName + " " + localIP);
    addField("ttl", String(ttl));
    addField("actions", actions);

    body += "--" + boundary + "--\r\n";

    Serial.println("[Notify] 开始发送通知...");
    Serial.printf("[Notify] URL: %s\n", apiUrl.c_str());
    Serial.printf("[Notify] Action: %s\n", actionUrl.c_str());

    HTTPClient http;
    int httpCode = -1;

    // --------------------------------------------
    // HTTPS
    // --------------------------------------------
    if (apiUrl.startsWith("https://")) {

        WiFiClientSecure client;

        // ESP32 设备这里直接关闭证书验证
        // 避免 CA / 时间 / 证书链导致发送失败
        client.setInsecure();

        if (!http.begin(client, apiUrl)) {
            Serial.println("[Notify] HTTPS begin 失败");
            return false;
        }

        String contentType =
            "multipart/form-data; boundary=" + boundary;

        http.addHeader("Content-Type", contentType);
        http.setTimeout(10000);

        httpCode = http.POST(body);

    // --------------------------------------------
    // HTTP
    // --------------------------------------------
    } else {

        WiFiClient client;

        if (!http.begin(client, apiUrl)) {
            Serial.println("[Notify] HTTP begin 失败");
            return false;
        }

        String contentType =
            "multipart/form-data; boundary=" + boundary;

        http.addHeader("Content-Type", contentType);
        http.setTimeout(10000);

        httpCode = http.POST(body);
    }

    String response = http.getString();

    Serial.printf("[Notify] HTTP Code: %d\n", httpCode);

    if (response.length() > 0) {
        Serial.printf(
            "[Notify] Response: %s\n",
            response.substring(0, 300).c_str()
        );
    }

    http.end();

    return httpCode >= 200 && httpCode < 300;
}

} // namespace Notify

#endif