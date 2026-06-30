// =================================================================
// PowerMonitor_INA219.ino — 继电器 + INA219 独立电源监控保护模块
// 适用平台: ESP8266 (NodeMCU / Wemos D1 Mini)
// 特性：断电记忆、NTP时间校验、1小时跨断电保护、Web控制、Web OTA更新
// 库依赖: GyverINA, ArduinoJson, NTPClient
/*
INA219 引脚接线说明:
  VCC  → 3.3V       模块供电（勿接 5V，防止损坏 I2C 逻辑）
  GND  → GND        共地
  SDA  → D2 (GPIO4) I2C 数据线
  SCL  → D1 (GPIO5) I2C 时钟线
  IN+  → 电源正极端（电源侧）
  IN-  → 负载正极端（INA219 IN- 接继电器 COM，继电器 NO 接负载正极）
  VBS  → 与 IN+ 短接（总线电压参考，INA219 同名引脚功能相同）
  A0/A1 → 悬空（I2C 地址默认 0x40）

继电器模块接线:
  VCC  → 5V / VIN（外部稳压 5V 供电，太阳能场景推荐独立稳压）
  GND  → GND（与 ESP8266 共地）
  IN   → D5 (GPIO14) 控制信号线
  COM  → INA219 IN- 输出端
  NO   → 负载正极（必须接 NO，不能接 NC！）

负极共地:
  电源负极 = ESP8266 GND = INA219 GND = 继电器 GND = 负载负极（全部共地）
*/
// =================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <INA219.h>      // GyverINA 库（INA219 头文件）
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

#include "note.h"        // 引入打卡模块（含 SystemState 与 TripReason 声明）

// =================================================================
// 1. 用户配置
// =================================================================
const char* ssid     = "yang1234";
const char* password = "y123456789";

const char* dns_hostname = "powermonitor";

// 引脚定义
constexpr uint8_t  RELAY_PIN       = 14;    // D5
constexpr uint8_t  RELAY_ON_LEVEL  = HIGH;  // HIGH 吸合（根据你的继电器模块调整）
constexpr uint8_t  RELAY_OFF_LEVEL = LOW;   // LOW  释放

// I2C: SDA → D2 (GPIO4), SCL → D1 (GPIO5)
constexpr uint8_t  I2C_SDA_PIN    = 4;
constexpr uint8_t  I2C_SCL_PIN    = 5;

// INA219 配置
constexpr uint8_t  INA219_ADDR         = 0x40;
constexpr float    SHUNT_RESISTOR_OHM  = 0.1f;  // 采样电阻 (Ω)，模块自带通常是 0.1Ω
constexpr float    MAX_CURRENT_A       = 5.0f;  // 最大设计电流 (A)

// 保护逻辑参数
constexpr unsigned long COOLDOWN_SEC       = 3600UL; // 跳闸后冷却时间（秒）
constexpr uint8_t       MAX_RETRIES        = 3;      // 最大自动重试次数
constexpr unsigned long SAMPLE_INTERVAL_MS = 1000UL; // 采样间隔（毫秒）
constexpr uint8_t       CONFIRM_COUNT      = 3;      // 连续确认次数才触发保护

// 默认保护阈值（可通过网页修改并保存）
struct ProtectSettings {
    float overVoltage  = 14.5f; // 过压保护 (V)
    float underVoltage = 9.0f;  // 欠压保护 (V)
    float hysteresis   = 0.5f;  // 电压恢复回差 (V)，防继电器频繁抖动
    float overCurrent  = 5.0f;  // 过流保护 (A)
} cfg;

// 全局状态实例（note.h 通过 extern 引用）
SystemState st;

// =================================================================
// 2. 全局对象
// =================================================================
INA219 ina(SHUNT_RESISTOR_OHM, MAX_CURRENT_A, INA219_ADDR);

ESP8266WebServer        server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiUDP                 ntpUDP;
NTPClient               timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);

const char* configFile = "/config.json";
const char* stateFile  = "/state.json";

// =================================================================
// 3. 配置与状态持久化
// =================================================================
void saveConfig() {
    File file = LittleFS.open(configFile, "w");
    if (!file) return;
    StaticJsonDocument<256> doc;
    doc["overVoltage"]  = cfg.overVoltage;
    doc["underVoltage"] = cfg.underVoltage;
    doc["hysteresis"]   = cfg.hysteresis;
    doc["overCurrent"]  = cfg.overCurrent;
    serializeJson(doc, file);
    file.close();
}

void loadConfig() {
    if (!LittleFS.exists(configFile)) { saveConfig(); return; }
    File file = LittleFS.open(configFile, "r");
    if (!file) return;
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, file)) {
        cfg.overVoltage  = doc["overVoltage"]  | 14.5f;
        cfg.underVoltage = doc["underVoltage"] | 9.0f;
        cfg.hysteresis   = doc["hysteresis"]   | 0.5f;
        cfg.overCurrent  = doc["overCurrent"]  | 5.0f;
    }
    file.close();
}

void saveSystemState() {
    File file = LittleFS.open(stateFile, "w");
    if (!file) return;
    StaticJsonDocument<512> doc;
    doc["relayOn"]        = st.relayOn;
    doc["faultLatched"]   = st.faultLatched;
    doc["tripReason"]     = static_cast<uint8_t>(st.tripReason);
    doc["tripEpoch"]      = st.tripEpoch;
    doc["retryCount"]     = st.retryCount;
    doc["todayOnSec"]     = st.todayOnSec;
    doc["yesterdayOnSec"] = st.yesterdayOnSec;
    doc["cumulativeWh"]   = st.cumulativeWh;
    doc["lastLoggedDate"] = st.lastLoggedDate;
    serializeJson(doc, file);
    file.close();
}

void loadSystemState() {
    if (!LittleFS.exists(stateFile)) { saveSystemState(); return; }
    File file = LittleFS.open(stateFile, "r");
    if (!file) return;
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, file)) {
        st.relayOn        = doc["relayOn"]        | true;
        st.faultLatched   = doc["faultLatched"]   | false;
        st.tripReason     = static_cast<TripReason>(doc["tripReason"] | 0);
        st.tripEpoch      = doc["tripEpoch"]      | 0;
        st.retryCount     = doc["retryCount"]     | 0;
        st.todayOnSec     = doc["todayOnSec"]     | 0UL;
        st.yesterdayOnSec = doc["yesterdayOnSec"] | 0UL;
        st.cumulativeWh   = doc["cumulativeWh"]   | 0.0;
        String d          = doc["lastLoggedDate"] | "";
        strncpy(st.lastLoggedDate, d.c_str(), sizeof(st.lastLoggedDate) - 1);
    }
    file.close();
}

void setRelayPhysical(bool on) {
    digitalWrite(RELAY_PIN, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

// =================================================================
// 4. 电压回差 + 1小时冷却判断
// =================================================================
bool checkCooldownPassed() {
    if (st.relayOn)                          return true;
    if (st.tripReason == TripReason::NONE)   return true;

    // 电压回差检测：防止继电器在阈值附近反复振荡
    float v = st.busVoltage;
    if (st.tripReason == TripReason::OVERVOLTAGE) {
        if (v >= (cfg.overVoltage - cfg.hysteresis)) return false;
    } else if (st.tripReason == TripReason::UNDERVOLTAGE) {
        if (v <= (cfg.underVoltage + cfg.hysteresis)) return false;
    }

    // 1小时时间冷却（有网络 NTP 优先，无网络用 millis() 兜底）
    if (timeClient.isTimeSet() && st.tripEpoch > 0) {
        unsigned long elapsed = timeClient.getEpochTime() - st.tripEpoch;
        return elapsed >= COOLDOWN_SEC;
    }

    return millis() >= 3600000UL;
}

// =================================================================
// 5. 跳闸 / 恢复 / 重置
// =================================================================
void executeTrip(TripReason reason) {
    st.relayOn        = false;
    st.tripReason     = reason;
    st.confirmCounter = 0;
    st.tripEpoch      = timeClient.isTimeSet() ? timeClient.getEpochTime() : 0;

    setRelayPhysical(false);
    saveSystemState();

    const char* rStr =
        reason == TripReason::OVERVOLTAGE  ? "过压"   :
        reason == TripReason::UNDERVOLTAGE ? "欠压"   :
        reason == TripReason::OVERCURRENT  ? "过流"   :
        reason == TripReason::MANUAL       ? "手动断开" : "未知";

    Serial.printf("[ALARM] 跳闸保护! 原因: %s  V=%.2fV  I=%.3fA\n",
                  rStr, st.busVoltage, st.current_mA / 1000.f);
}

void executeRecover() {
    st.retryCount++;
    st.relayOn    = true;
    st.tripReason = TripReason::NONE;
    st.tripEpoch  = 0;
    setRelayPhysical(true);
    saveSystemState();
    Serial.printf("[Recovery] 自动恢复，第 %d 次重试\n", st.retryCount);
}

void resetSystem() {
    st.relayOn        = true;
    st.faultLatched   = false;
    st.tripReason     = TripReason::NONE;
    st.tripEpoch      = 0;
    st.retryCount     = 0;
    st.confirmCounter = 0;
    setRelayPhysical(true);
    saveSystemState();
}

// =================================================================
// 6. Web 前端
// =================================================================
String getTripReasonText(TripReason r) {
    switch (r) {
        case TripReason::OVERVOLTAGE:  return "过压跳闸";
        case TripReason::UNDERVOLTAGE: return "欠压跳闸";
        case TripReason::OVERCURRENT:  return "过流跳闸";
        case TripReason::MANUAL:       return "手动切断";
        default:                       return "运行正常";
    }
}

void handleRoot() {
    // ── 冷却剩余时间计算 ──
    String cooldownStr = "状态良好 / 回路无异常";
    if (!st.relayOn && !st.faultLatched) {
        time_t now = time(nullptr);
        if (now > 86400 * 2 && st.tripEpoch > 0) {
            long rem = (long)COOLDOWN_SEC - (long)(now - (time_t)st.tripEpoch);
            cooldownStr = rem > 0
                ? "⌛ 1小时冷却剩余: <strong>" + String(rem) + " 秒</strong>"
                : "✅ 冷却完成，正在检查电压恢复...";
        } else {
            long rem = (long)COOLDOWN_SEC - (long)(millis() / 1000UL);
            cooldownStr = rem > 0
                ? "⌛ 离线冷却剩余: <strong>" + String(rem) + " 秒</strong>"
                : "✅ 保护解除";
        }
    }

    // ── 状态文本 ──
    String statusStr;
    if (st.faultLatched) {
        statusStr = "<span style='color:#da3633'>严重故障锁定（已达重试上限）</span>";
    } else if (!st.relayOn) {
        statusStr = "<span style='color:#f0883e'>保护断开中（" + getTripReasonText(st.tripReason) + "）</span>";
    } else {
        statusStr = "<span style='color:#3fb950'>继电器已吸合 ✓</span>";
    }

    String html;
    html.reserve(4096);
    html = F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<meta http-equiv='refresh' content='3'>"
             "<title>电源监控 INA219</title>"
             "<style>"
             "body{background:#0d1117;color:#c9d1d9;font-family:sans-serif;margin:0;padding:10px;text-align:center}"
             ".card{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:20px;"
                   "max-width:500px;margin:20px auto;box-sizing:border-box}"
             "h1{color:#58a6ff;margin-bottom:15px}"
             ".val{font-size:1.9em;font-weight:bold;color:#58a6ff;margin:6px 0}"
             ".sub{color:#8b949e;font-size:.8em;margin-bottom:12px}"
             ".btn{display:inline-block;padding:10px 20px;margin:5px;border-radius:6px;"
                   "font-weight:bold;text-decoration:none;cursor:pointer}"
             ".btn-on{background:#238636;color:#fff}"
             ".btn-off{background:#da3633;color:#fff}"
             ".btn-rst{background:#21262d;color:#c9d1d9;border:1px solid #30363d}"
             "hr{border-color:#30363d}"
             ".form-group{margin:12px 0;text-align:left}"
             "label{display:block;margin-bottom:4px;color:#8b949e;font-size:.88em}"
             ".inp{width:100%;background:#0d1117;border:1px solid #30363d;border-radius:6px;"
                   "padding:9px;color:#c9d1d9;font-size:1em;box-sizing:border-box}"
             ".sub-btn{background:#21262d;color:#58a6ff;border:1px solid #30363d;width:100%;"
                       "padding:10px;font-weight:bold;cursor:pointer;border-radius:6px}"
             ".sub-btn:hover{background:#30363d}"
             ".meta{color:#8b949e;font-size:.85em;line-height:1.7;margin-top:10px;text-align:left}"
             "</style></head><body>"
             "<div class='card'>"
             "<h1>⚡ 电源监控保护站 <small style='font-size:.5em;color:#8b949e'>INA219</small></h1>");

    html += "<div class='val'>" + String(st.busVoltage, 3)         + " V</div>";
    html += "<div class='val'>" + String(st.current_mA / 1000.f, 4) + " A</div>";
    html += "<div class='val'>" + String(st.power_mW   / 1000.f, 2) + " W</div>";
    html += "<div class='sub'>分流电压: " + String(st.shuntVoltage, 2) + " mV</div>";

    html += "<hr><h3>状态: " + statusStr + "</h3>";
    html += "<div>"
            "<a href='/on'    class='btn btn-on' >手动开启</a>"
            "<a href='/off'   class='btn btn-off'>安全断开</a>"
            "<a href='/reset' class='btn btn-rst'>故障重置</a>"
            "</div>";

    // 保护参数表单
    html += "<hr><h3>⚙️ 保护参数设置</h3>"
            "<form action='/save_settings' method='POST'>";
    html += "<div class='form-group'><label>过压保护阈值 (V)</label>"
            "<input type='number' name='ov'  class='inp' step='0.1' value='"
            + String(cfg.overVoltage,  1) + "' required></div>";
    html += "<div class='form-group'><label>欠压保护阈值 (V)</label>"
            "<input type='number' name='uv'  class='inp' step='0.1' value='"
            + String(cfg.underVoltage, 1) + "' required></div>";
    html += "<div class='form-group'><label>恢复回差 (V) — 防振荡</label>"
            "<input type='number' name='hys' class='inp' step='0.1' value='"
            + String(cfg.hysteresis,   1) + "' required></div>";
    html += "<div class='form-group'><label>过流保护阈值 (A)</label>"
            "<input type='number' name='oc'  class='inp' step='0.1' value='"
            + String(cfg.overCurrent,  1) + "' required></div>";
    html += "<input type='submit' class='sub-btn' value='保存并应用'></form>";

    // 统计数据
    html += "<hr><h3>📊 运行统计</h3>"
            "<p class='meta'>"
            "• 昨天开启时长：<strong>" + _fmtSec(st.yesterdayOnSec) + "</strong><br>"
            "• 今天开启时长：<strong>" + _fmtSec(st.todayOnSec)     + "</strong><br>"
            "• 历史累计电能：<strong>" + String(st.cumulativeWh, 3)  + " Wh</strong><br>"
            "• 上次打卡日期：<strong>" + String(st.lastLoggedDate)   + "</strong><br>"
            "• 自动重试次数：<strong>" + String(st.retryCount)       + " / " + String(MAX_RETRIES) + "</strong>"
            "</p>"
            "<p class='meta'>" + cooldownStr + "</p>";

    html += "<p class='meta'><a href='/update' style='color:#58a6ff'>🚀 固件 OTA 升级</a></p>"
            "</div></body></html>";

    server.send(200, "text/html", html);
}

void handleSaveSettings() {
    if (server.hasArg("ov"))  cfg.overVoltage  = server.arg("ov").toFloat();
    if (server.hasArg("uv"))  cfg.underVoltage = server.arg("uv").toFloat();
    if (server.hasArg("hys")) cfg.hysteresis   = server.arg("hys").toFloat();
    if (server.hasArg("oc"))  cfg.overCurrent  = server.arg("oc").toFloat();
    saveConfig();
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
}

// =================================================================
// 7. Setup
// =================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(RELAY_PIN, OUTPUT);

    note_begin();       // 初始化 LittleFS + 读取上次打卡日期
    loadConfig();
    loadSystemState();
    setRelayPhysical(st.relayOn);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    // INA219 初始化
    // GyverINA 的 INA219 默认 ±32V / ±2A 量程（0.1Ω 时），
    // begin() 会根据构造参数自动计算校准寄存器
    if (!ina.begin()) {
        Serial.println("[INA219] 初始化失败！检查接线和 I2C 地址");
    } else {
        // 平均 8 次采样，转换时间 2.116ms，精度与噪声平衡
        ina.setAveraging(INA219_AVG_X8);
        ina.setSampleTime(INA219_VBUS,   INA219_CONV_2116US);
        ina.setSampleTime(INA219_VSHUNT, INA219_CONV_2116US);
        Serial.println("[INA219] 初始化成功");
    }

    // WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("[Network] 连接中");
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
        delay(500); Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[Network] 已连接 IP: %s\n", WiFi.localIP().toString().c_str());

        configTime(8 * 3600, 0, "ntp.aliyun.com");

        if (MDNS.begin(dns_hostname)) {
            Serial.printf("[MDNS] http://%s.local\n", dns_hostname);
            MDNS.addService("http", "tcp", 80);
        }

        timeClient.begin();
        timeClient.update();

        server.on("/",             HTTP_GET,  handleRoot);
        server.on("/save_settings",HTTP_POST, handleSaveSettings);
        server.on("/on",  HTTP_GET, [](){ resetSystem();               server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });
        server.on("/off", HTTP_GET, [](){ executeTrip(TripReason::MANUAL); server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });
        server.on("/reset",HTTP_GET,[](){ resetSystem();               server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });

        httpUpdater.setup(&server);
        server.begin();
        ArduinoOTA.begin();
    } else {
        Serial.println("[Network] 无网络，独立运行");
    }

    st.lastSaveMs = millis();
}

// =================================================================
// 8. Loop
// =================================================================
void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        server.handleClient();
        ArduinoOTA.handle();
        timeClient.update();
        MDNS.update();
        note_loop();
    }

    unsigned long now = millis();

    // ── 每秒采样一次 ──
    if (now - st.lastSampleMs >= SAMPLE_INTERVAL_MS) {
        st.lastSampleMs = now;

        st.busVoltage   = ina.getVoltage();
        st.shuntVoltage = ina.getShuntVoltage() * 1000.f; // V → mV
        st.current_mA   = ina.getCurrent()      * 1000.f; // A → mA
        st.power_mW     = ina.getPower()        * 1000.f; // W → mW

        // 累计统计
        if (st.relayOn) {
            st.todayOnSec++;
            st.cumulativeWh += (st.power_mW / 1000.f) / 3600.0;
        }

        // ── 继电器断开：冷却 / 重试 / 锁定 ──
        if (!st.relayOn) {
            if (st.faultLatched) return;
            if (checkCooldownPassed()) {
                if (st.retryCount < MAX_RETRIES) {
                    executeRecover();
                } else {
                    st.faultLatched = true;
                    saveSystemState();
                    Serial.println("[Protection] 超过重试上限，永久锁定");
                }
            }
            return;
        }

        // ── 继电器闭合：检测异常 ──
        TripReason pending = TripReason::NONE;

        if      (st.busVoltage > cfg.overVoltage)
            pending = TripReason::OVERVOLTAGE;
        else if (st.busVoltage < cfg.underVoltage && st.busVoltage > 0.5f)
            pending = TripReason::UNDERVOLTAGE;
        else if (st.current_mA > cfg.overCurrent * 1000.f)
            pending = TripReason::OVERCURRENT;

        if (pending != TripReason::NONE) {
            if (++st.confirmCounter >= CONFIRM_COUNT)
                executeTrip(pending);
        } else {
            st.confirmCounter = 0;
        }
    }

    // ── 每 5 分钟持久化一次（防太阳能断电丢数据）──
    if (now - st.lastSaveMs >= 300000UL) {
        st.lastSaveMs = now;
        saveSystemState();
        Serial.println("[Storage] 周期归档完成");
    }
}
