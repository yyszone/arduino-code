// =================================================================
// esp32_PowerMonitor_INA219.ino — 继电器 + INA219 独立电源监控保护模块
// 适用平台: ESP32-C3 (C3 Mini / C3 SuperMini)
// 特性：高于电压开启、欠压关闭（带倒计时显示）、低功率保护、首次通电WiFi保护、断电记忆、全中文智能状态提示系统、Web OTA更新、TM1637数码管轮播显示
// 库依赖: GyverINA, ArduinoJson, NTPClient, TM1637Display
// =================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <Wire.h>
#include <INA219.h>      // GyverINA 库
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <time.h>
#include <esp_system.h>

#include "note.h"           // 引入打卡模块（含 SystemState 与 TripReason 声明）
#include "display_tm1637.h" // 引入数码管显示模块

// =================================================================
// 1. 用户配置与结构体
// =================================================================
const char* ssid     = "yang1234";
const char* password = "y123456789";

// ESP32-C3 Mini 引脚定义
constexpr uint8_t  RELAY_PIN       = 10;    // GPIO10
constexpr uint8_t  RELAY_ON_LEVEL  = HIGH;  // HIGH 吸合
constexpr uint8_t  RELAY_OFF_LEVEL = LOW;   // LOW  释放

// I2C 接口引脚 (ESP32-C3 默认硬I2C: SDA 为 GPIO8, SCL 为 GPIO9)
constexpr uint8_t  I2C_SDA_PIN    = 8;
constexpr uint8_t  I2C_SCL_PIN    = 9;

// INA219 配置
constexpr uint8_t  INA219_ADDR         = 0x40;
constexpr float    SHUNT_RESISTOR_OHM  = 0.1f;  // 采样电阻
constexpr float    MAX_CURRENT_A       = 5.0f;  // 最大设计电流

// 保护逻辑参数
constexpr unsigned long SAMPLE_INTERVAL_MS = 1000UL; // 采样间隔
constexpr uint8_t       CONFIRM_COUNT      = 3;      // 连续确认次数才触发动作

// 保护阈值与定时配置
struct ProtectSettings {
    float turnOnVoltage = 13.5f;   // 高于多少 V 自动开启 (V)
    float underVoltage  = 11.5f;   // 欠压保护阈值 (V)
    float underPower    = 2.0f;    // 低于多少 W 自动关闭 (W)，设为 0 可禁用
    uint32_t cooldownSec = 3600UL; // 自动保护锁定待机时间（秒）
    
    // Wi-Fi 定时控制参数
    bool timerEnabled  = false;    
    uint8_t sleepHour  = 22;       
    uint8_t sleepMinute = 0;       
    uint8_t wakeHour   = 6;        
    uint8_t wakeMinute = 0;        

    // 数码管物理屏幕控制与低电压休眠
    bool displayEnabledUser = true; 
    float sleepVoltage      = 11.0f; 
} cfg;

// 全局状态实例
SystemState st;

// =================================================================
// 2. 全局对象与状态
// =================================================================
INA219 ina(SHUNT_RESISTOR_OHM, MAX_CURRENT_A, INA219_ADDR);

WebServer               server(80);
HTTPUpdateServer        httpUpdater;
WiFiUDP                 ntpUDP;
NTPClient               timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);

const char* configFile = "/config.json";
const char* stateFile  = "/state.json";

// 定时及休眠控制变量
bool currentWifiState  = true; 
unsigned long lastWifiCheckMs = 0;

// 低电压待机休眠追踪
bool lowVoltageSleeping = false;
unsigned long sleepStartMs = 0;
unsigned long sleepGuardUntilMs = 120000UL; 

// 继电器启动时间记录
unsigned long relayOnTimeMs = 0;

// =================================================================
// 3. 配置与状态持久化与复位原因诊断
// =================================================================
String getResetReasonString() {
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON (正常上电)";
        case ESP_RST_EXT:       return "EXTERNAL (外部按键复位)";
        case ESP_RST_SW:        return "SOFTWARE (软件重启)";
        case ESP_RST_PANIC:     return "PANIC (异常崩溃重启)";
        case ESP_RST_INT_WDT:   return "INT_WDT (看门狗复位)";
        case ESP_RST_TASK_WDT:  return "TASK_WDT (任务看门狗复位)";
        case ESP_RST_WDT:       return "OTHER_WDT (看门狗复位)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP (休眠唤醒)";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (供电电压不足/下拉重启)";
        case ESP_RST_SDIO:      return "SDIO (SDIO复位)";
        default:                return "UNKNOWN (未知原因)";
    }
}

void saveConfig() {
    File file = LittleFS.open(configFile, "w");
    if (!file) return;
    StaticJsonDocument<768> doc;
    doc["turnOnVoltage"]      = cfg.turnOnVoltage;
    doc["underVoltage"]       = cfg.underVoltage;
    doc["underPower"]         = cfg.underPower;
    doc["cooldownSec"]        = cfg.cooldownSec;  
    doc["timerEnabled"]       = cfg.timerEnabled;
    doc["sleepHour"]          = cfg.sleepHour;
    doc["sleepMinute"]        = cfg.sleepMinute;
    doc["wakeHour"]           = cfg.wakeHour;
    doc["wakeMinute"]         = cfg.wakeMinute;
    doc["displayEnabledUser"] = cfg.displayEnabledUser;
    doc["sleepVoltage"]       = cfg.sleepVoltage;
    
    serializeJson(doc, file);
    file.close();
}

void loadConfig() {
    if (!LittleFS.exists(configFile)) { saveConfig(); return; }
    File file = LittleFS.open(configFile, "r");
    if (!file) return;
    StaticJsonDocument<768> doc;
    if (!deserializeJson(doc, file)) {
        cfg.turnOnVoltage      = doc["turnOnVoltage"] | 13.5f;
        cfg.underVoltage       = doc["underVoltage"]  | 11.5f;
        cfg.underPower         = doc["underPower"]    | 2.0f;
        cfg.cooldownSec        = doc["cooldownSec"]   | 3600UL; 
        cfg.timerEnabled       = doc["timerEnabled"]  | false;
        cfg.sleepHour          = doc["sleepHour"]     | (uint8_t)22;
        cfg.sleepMinute        = doc["sleepMinute"]   | (uint8_t)0;
        cfg.wakeHour           = doc["wakeHour"]      | (uint8_t)6;
        cfg.wakeMinute         = doc["wakeMinute"]    | (uint8_t)0;
        cfg.displayEnabledUser = doc["displayEnabledUser"] | true;
        cfg.sleepVoltage       = doc["sleepVoltage"]  | 11.0f;
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
        st.relayOn        = doc["relayOn"]        | false; 
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
// 4. 倒计时剩余秒数查询
// =================================================================
long getCooldownRemaining() {
    if (st.tripEpoch == 0) return 0;

    time_t now_t = time(nullptr);
    if (now_t > 1000000000L) {
        long elapsed = (long)(now_t - st.tripEpoch);
        long rem = (long)cfg.cooldownSec - elapsed;
        return rem > 0 ? rem : 0;
    }
    long elapsed = (long)(millis() / 1000UL);
    long rem = (long)cfg.cooldownSec - elapsed;
    return rem > 0 ? rem : 0;
}

String formatSeconds(long secs) {
    if (secs <= 0) return "0秒";
    if (secs < 60) return String(secs) + "秒";
    long mins = secs / 60;
    long rsecs = secs % 60;
    if (mins < 60) {
        return String(mins) + "分" + String(rsecs) + "秒";
    }
    long hrs = mins / 60;
    long rmins = mins % 60;
    return String(hrs) + "小时" + String(rmins) + "分" + String(rsecs) + "秒";
}

// =================================================================
// 5. 跳闸 / 恢复 / 重置
// =================================================================
void executeTrip(TripReason reason) {
    st.relayOn        = false;
    st.faultLatched   = true;   
    st.tripReason     = reason;
    st.confirmCounter = 0;
    
    time_t now_t = time(nullptr);
    st.tripEpoch      = (now_t > 1000000000L) ? now_t : 0;

    setRelayPhysical(false);
    saveSystemState();  

    const char* rStr =
        reason == TripReason::UNDERVOLTAGE ? "欠压"   :
        reason == TripReason::OVERCURRENT  ? "低功率" : 
        reason == TripReason::MANUAL       ? "手动断开" : "未知";

    Serial.printf("[ALARM] 关闭保护动作! 原因: %s  V=%.2fV  W=%.3fW\n",
                  rStr, st.busVoltage, st.power_mW / 1000.f);
}

void forceOnSystem() {
    st.relayOn        = true;
    st.faultLatched   = false;
    st.tripReason     = TripReason::NONE;
    st.tripEpoch      = 0;
    st.retryCount     = 0;
    st.confirmCounter = 0;
    relayOnTimeMs     = millis(); 
    setRelayPhysical(true);
    saveSystemState();
    Serial.println("[Manual] 手动强制开启继电器");
}

void resetSystem() {
    st.faultLatched   = false;
    st.tripReason     = TripReason::NONE;
    st.tripEpoch      = 0;
    st.retryCount     = 0;
    st.confirmCounter = 0;
    
    if (millis() < 60000UL) {
        st.relayOn    = false;
        setRelayPhysical(false);
        Serial.println("[Reset] 预热期内清除故障，保持默认关闭");
    } else {
        if (st.busVoltage > cfg.turnOnVoltage) {
            st.relayOn    = true;
            relayOnTimeMs = millis(); 
            setRelayPhysical(true);
            Serial.printf("[Reset] 故障清除并重新开启继电器 (%.2fV > 阈值 %.2fV)\n", st.busVoltage, cfg.turnOnVoltage);
        } else {
            st.relayOn    = false;
            setRelayPhysical(false);
            Serial.printf("[Reset] 故障清除，但电压低于阈值，保持关闭等待自动恢复 (%.2fV <= 阈值 %.2fV)\n", st.busVoltage, cfg.turnOnVoltage);
        }
    }
    saveSystemState();
}

// =================================================================
// 6. Web 前端
// =================================================================
String getTripReasonText(TripReason r) {
    switch (r) {
        case TripReason::UNDERVOLTAGE: return "欠压保护";
        case TripReason::OVERCURRENT:  return "低功率保护"; 
        case TripReason::MANUAL:       return "手动切断";
        default:                       return "运行正常";
    }
}

void getStatusDisplay(String &statusHtml, String &cooldownHtml) {
    unsigned long now = millis();
    
    if (now < 60000UL) {
        long remainSec = (60000UL - now) / 1000UL;
        statusHtml = "<span style='color:#ffea00; text-shadow: 0 0 10px rgba(255,234,0,0.5);'>⏳ 系统开机预热中（剩余 " + String(remainSec) + " 秒）</span>";
        cooldownHtml = "🔌 <b>开机安全隔离期</b>：开机默认关闭继电器。1分钟内系统不执行任何自动开启或关闭动作。此时您可以点击“手动开启”强制吸合，或等待预热结束进入自动托管。";
        return;
    }

    if (lowVoltageSleeping) {
        statusHtml = "<span style='color:#ff00ea; text-shadow: 0 0 10px rgba(255,0,234,0.5);'>🌙 系统处于低压省电休眠中</span>";
        cooldownHtml = "💤 <b>低压待机休眠激活</b>：当电压低于设定电压时，系统已自动关闭数码管并切断网络射频。此时正处于冷却周期中。";
        return;
    }

    if (st.relayOn) {
        statusHtml = "<span style='color:#39ff14; text-shadow: 0 0 10px rgba(57,255,20,0.5);'>✔ 继电器已吸合（自动托管运行中）</span>";
        cooldownHtml = "🔌 当前系统运行状态良好，回路负载与电压均在安全范围内。";
    } else {
        if (st.tripReason == TripReason::MANUAL) {
            statusHtml = "<span style='color:#ff0055; text-shadow: 0 0 10px rgba(255,0,85,0.5);'>⛔ 手动安全断开状态</span>";
            cooldownHtml = "🔒 <b>安全锁定激活</b>：系统已由用户手动断开，系统<b>禁止自动开机</b>。如需恢复自动电压控制，请点击上方“故障重置”键释放锁定。";
        } else {
            long rem = getCooldownRemaining();
            if (rem > 0 && st.tripReason != TripReason::NONE) {
                statusHtml = "<span style='color:#ffaa00; text-shadow: 0 0 10px rgba(255,170,0,0.5);'>⌛ 保护待机锁定中（" + getTripReasonText(st.tripReason) + "，剩余冷却：" + formatSeconds(rem) + "）</span>";
                cooldownHtml = "🛡️ <b>设备冷却保护中</b>：为了防止负载设备频繁通断电损坏，系统正在强制待机。建议等待倒计时结束后自动启动，或直接点击“手动开启”强制跳过保护。";
            } else {
                if (st.busVoltage > cfg.turnOnVoltage) {
                    statusHtml = "<span style='color:#00f3ff; text-shadow: 0 0 10px rgba(0,243,255,0.5);'>⏳ 正在启动准备中...</span>";
                    cooldownHtml = "⚡ 当前电压（" + String(st.busVoltage, 2) + "V）已满足开启条件（高于 " + String(cfg.turnOnVoltage, 1) + "V），系统正在进行多样本滤波确认，请稍候...";
                } else {
                    statusHtml = "<span style='color:#00f3ff; text-shadow: 0 0 10px rgba(0,243,255,0.5);'>🔍 自动控制模式：待机监测中</span>";
                    cooldownHtml = "✨ <b>故障与时间锁已释放！</b>当前系统状态良好，正在持续监测输入电压。等待电压升至高于 <strong>" + String(cfg.turnOnVoltage, 1) + " V</strong>（当前电压为 " + String(st.busVoltage, 3) + " V）时，系统将<b>自动吸合开启</b>继电器。";
                }
            }
        }
    }
}

void handleRoot() {
    String statusStr, cooldownStr;
    getStatusDisplay(statusStr, cooldownStr); 

    String ipStr = WiFi.localIP().toString();

    String html;
    html.reserve(4096);
    html = F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>电源监控 INA219</title>"
             "<style>"
             ":root {"
               "--bg: #03030c;"
               "--panel-bg: rgba(10, 10, 25, 0.85);"
               "--border-color: #3f007f;"
               "--neon-cyan: #00f3ff;"
               "--neon-magenta: #ff0055;"
               "--neon-green: #39ff14;"
               "--neon-purple: #b026ff;"
               "--neon-yellow: #ffea00;"
             "}"
             "body{background:var(--bg); background-image:radial-gradient(circle at 50% 50%, #12052c 0%, #03030c 100%); color:#e2d9f3; font-family:'Courier New', Courier, monospace, sans-serif; margin:0; padding:10px; text-align:center}"
             ".card{background:var(--panel-bg); border:1px solid var(--border-color); border-radius:4px; box-shadow: 0 0 15px rgba(176, 38, 255, 0.15); padding:20px; max-width:550px; margin:20px auto; box-sizing:border-box; position:relative;"
                   "background-image: linear-gradient(0deg, transparent 24%, rgba(0, 243, 255, .01) 25%, rgba(0, 243, 255, .01) 26%, transparent 27%, transparent 74%, rgba(0, 243, 255, .01) 75%, rgba(0, 243, 255, .01) 76%, transparent 77%, transparent), linear-gradient(90deg, transparent 24%, rgba(0, 243, 255, .01) 25%, rgba(0, 243, 255, .01) 26%, transparent 27%, transparent 74%, rgba(0, 243, 255, .01) 75%, rgba(0, 243, 255, .01) 76%, transparent 77%, transparent);"
                   "background-size: 30px 30px;}"
             ".card::before {content:''; position:absolute; top:-2px; left:-2px; width:12px; height:12px; border-top:3px solid var(--neon-cyan); border-left:3px solid var(--neon-cyan);}"
             ".card::after {content:''; position:absolute; bottom:-2px; right:-2px; width:12px; height:12px; border-bottom:3px solid var(--neon-cyan); border-right:3px solid var(--neon-cyan);}"
             "h1{color:var(--neon-cyan); text-shadow:0 0 10px rgba(0, 243, 255, 0.5); font-size:1.5em; border-bottom:2px dashed var(--border-color); padding-bottom:15px; margin-bottom:20px; letter-spacing:1px;}"
             "h3{font-size:1em; color:var(--neon-purple); text-shadow: 0 0 6px rgba(176,38,255,0.4); text-align:left; margin-top:25px; margin-bottom:12px; border-left:3px solid var(--neon-magenta); padding-left:8px; text-transform:uppercase;}"
             ".val-container {display:grid; grid-template-columns: 1fr 1fr 1fr; gap:10px; margin-bottom:15px;}"
             ".val-box {border:1px solid rgba(0, 243, 255, 0.2); padding:12px 2px; background:rgba(0,0,0,0.5); border-radius:3px; position:relative;}"
             ".val-box .num {font-size:1.5em; font-weight:bold; color:var(--neon-cyan); text-shadow: 0 0 8px rgba(0,243,255,0.5);}"
             ".val-box .lbl {font-size:0.7em; color:#8b949e; text-transform:uppercase; letter-spacing:1px; margin-top:5px;}"
             ".sub{color:#8b949e; font-size:.78em; margin-bottom:12px; font-family:monospace;}"
             ".btn{display:inline-block; padding:10px 15px; margin:5px; border-radius:4px; font-weight:bold; text-decoration:none; cursor:pointer; text-transform:uppercase; letter-spacing:1px; transition:all 0.2s ease; font-size:0.85em; border:1px solid transparent;}"
             ".btn-on{background:transparent; color:var(--neon-green); border-color:var(--neon-green); box-shadow:0 0 8px rgba(57,255,20,0.15); text-shadow: 0 0 4px var(--neon-green);}"
             ".btn-on:hover{background:var(--neon-green); color:#000; box-shadow:0 0 15px rgba(57,255,20,0.5);}"
             ".btn-off{background:transparent; color:var(--neon-magenta); border-color:var(--neon-magenta); box-shadow:0 0 8px rgba(255,0,85,0.15); text-shadow: 0 0 4px var(--neon-magenta);}"
             ".btn-off:hover{background:var(--neon-magenta); color:#000; box-shadow:0 0 15px rgba(255,0,85,0.5);}"
             ".btn-rst{background:transparent; color:var(--neon-purple); border-color:var(--neon-purple); box-shadow:0 0 8px rgba(176,38,255,0.15); text-shadow: 0 0 4px var(--neon-purple);}"
             ".btn-rst:hover{background:var(--neon-purple); color:#000; box-shadow:0 0 15px rgba(176,38,255,0.5);}"
             "hr{border:0; border-top:1px dashed var(--border-color); margin:20px 0;}"
             ".form-grid{display:grid; grid-template-columns:1fr 1fr; gap:12px;}"
             "@media(max-width:480px){.form-grid{grid-template-columns:1fr;}}"
             ".form-group{margin:6px 0; text-align:left}"
             "label{display:block; margin-bottom:4px; color:#8b949e; font-size:.78em; letter-spacing:0.5px;}"
             ".inp{width:100%; background:rgba(0,0,0,0.6); border:1px solid var(--border-color); border-radius:4px; padding:9px; color:#fff; font-size:0.92em; box-sizing:border-box; transition:all 0.3s ease;}"
             ".inp:focus{outline:none; border-color:var(--neon-cyan); box-shadow: 0 0 8px rgba(0,243,255,0.35);}"
             "select.inp{appearance:none; background-image:url(\"data:image/svg+xml;utf8,<svg fill='cyan' height='24' viewBox='0 0 24 24' width='24' xmlns='http://www.w3.org/2000/svg'><path d='M7 10l5 5 5-5z'/><path d='M0 0h24v24H0z' fill='none'/></svg>\"); background-repeat:no-repeat; background-position:right 8px center;}"
             ".sub-btn{background:transparent; color:var(--neon-cyan); border:1px solid var(--neon-cyan); width:100%; padding:10px; font-weight:bold; cursor:pointer; border-radius:4px; text-transform:uppercase; letter-spacing:1px; margin-top:15px; transition:all 0.3s ease;}"
             ".sub-btn:hover{background:var(--neon-cyan); color:#000; box-shadow: 0 0 15px rgba(0,243,255,0.5);}"
             ".meta{color:#8b949e; font-size:.82em; line-height:1.7; margin-top:10px; text-align:left; font-family:monospace;}"
             "</style></head><body>"
             "<div class='card'>"
             "<h1>⚡ SYSTEM POWER CORE <small style='font-size:.6em; color:#8b949e; float:right;'>INA219</small></h1>");

    html += "<div class='sub' style='text-align:left; border-left:3px solid var(--neon-cyan); padding-left:8px; margin-bottom:15px; font-size:0.8em; line-height:1.5;'>"
            "📂 脚本程序: <strong style='color:#fff;'>esp32_PowerMonitor_INA219.ino</strong><br>"
            "🔌 IP直连访问: <a href='http://" + ipStr + "' style='color:var(--neon-cyan); text-decoration:none;'>http://" + ipStr + "</a>"
            "</div>";

    html += "<div class='val-container'>";
    html += "  <div class='val-box'><div class='num' id='val-v'>" + String(st.busVoltage, 3) + " V</div><div class='lbl'>电压</div></div>";
    html += "  <div class='val-box'><div class='num' id='val-a'>" + String(st.current_mA / 1000.f, 4) + " A</div><div class='lbl'>电流</div></div>";
    html += "  <div class='val-box'><div class='num' id='val-w'>" + String(st.power_mW / 1000.f, 2) + " W</div><div class='lbl'>功率</div></div>";
    html += "</div>";
    html += "<div class='sub' id='val-mv'>分流阻抗压降: " + String(st.shuntVoltage, 2) + " mV</div>";

    html += "<hr><h3>🛰️ 运行状态指示</h3>";
    html += "<div style='margin-bottom:15px;' id='val-status'>" + statusStr + "</div>";
    html += "<div>"
            "<a href='/on'    class='btn btn-on' >手动开启</a>"
            "<a href='/off'   class='btn btn-off'>安全断开</a>"
            "<a href='/reset' class='btn btn-rst'>故障重置</a>"
            "</div>";

    // 保护参数与屏幕开关
    html += "<hr><h3>⚙️ 阈值与参数配置</h3>"
            "<form action='/save_settings' method='POST'>"
            "<div class='form-grid'>";
    html += "<div class='form-group'><label>高于多少 V 自动复位开启</label>"
            "<input type='number' name='tonv' class='inp' step='0.1' value='"
            + String(cfg.turnOnVoltage,  1) + "' required></div>";
    html += "<div class='form-group'><label>欠压关闭切断阈值 (V)</label>"
            "<input type='number' name='uv'   class='inp' step='0.1' value='"
            + String(cfg.underVoltage,  1) + "' required></div>";
    html += "<div class='form-group'><label>低于多少 W 触发自动保护 (设0禁用)</label>"
            "<input type='number' name='upw'  class='inp' step='0.1' value='"
            + String(cfg.underPower,    1) + "' required></div>";
    html += "<div class='form-group'><label>自动保护待机冷却时间 (秒)</label>"
            "<input type='number' name='cds'  class='inp' step='1' value='"
            + String(cfg.cooldownSec) + "' required></div>";
    html += "<div class='form-group'><label>数码管物理屏幕开关</label>"
            "<select name='deu' class='inp'>"
            "<option value='1' " + String(cfg.displayEnabledUser ? "selected" : "") + ">开启</option>"
            "<option value='0' " + String(!cfg.displayEnabledUser ? "selected" : "") + ">关闭</option>"
            "</select></div>";
    html += "<div class='form-group'><label>低于多少 V 自动待机休眠 (V)</label>"
            "<input type='number' name='slpv' class='inp' step='0.1' value='"
            + String(cfg.sleepVoltage, 1) + "' required></div>";
    html += "</div>"; 

    // 定时参数
    html += "<hr><h3>📶 无线电定时控制</h3>"
            "<div class='form-grid'>";
    html += "<div class='form-group'><label>定时休眠模式</label>"
            "<select name='te' class='inp'>"
            "<option value='0' " + String(!cfg.timerEnabled ? "selected" : "") + ">禁用</option>"
            "<option value='1' " + String(cfg.timerEnabled ? "selected" : "") + ">启用</option>"
            "</select></div>";

    auto formatTimeInput = [](uint8_t h, uint8_t m) {
        char buf[6];
        sprintf(buf, "%02d:%02d", h, m);
        return String(buf);
    };

    html += "<div class='form-group'><label>休眠断开 Wi-Fi 时间</label>"
            "<input type='time' name='st' class='inp' value='" + formatTimeInput(cfg.sleepHour, cfg.sleepMinute) + "' required></div>";
    html += "<div class='form-group'><label>唤醒重连 Wi-Fi 时间</label>"
            "<input type='time' name='wt' class='inp' value='" + formatTimeInput(cfg.wakeHour, cfg.wakeMinute) + "' required></div>";
    html += "</div>"; 

    html += "<input type='submit' class='sub-btn' value='保存配置并写入EEPROM'></form>";

    // 运行历史数据与复位诊断
    String tripTimeStr = "无";
    if (st.tripEpoch > 0) {
        time_t t = (time_t)st.tripEpoch + 8 * 3600; 
        struct tm* lt = localtime(&t);
        char buf[20];
        strftime(buf, sizeof(buf), "%m-%d %H:%M", lt);
        tripTimeStr = String(buf);
    }

    String bootGuardStr = "";
    if (millis() < 120000UL) {
        bootGuardStr = "• [BOOT_LOCK] 首次启动网络强制期 (剩余 " + String((120000UL - millis()) / 1000UL) + " 秒)<br>";
    }

    html += "<hr><h3>📊 运行历史数据</h3>"
            "<p class='meta'>"
            + bootGuardStr +
            "• 上次芯片复位原因: <strong style='color:#ffea00;'>" + getResetReasonString() + "</strong><br>"
            "• 昨日在线累计时长: <strong>" + _fmtSec(st.yesterdayOnSec) + "</strong><br>"
            "• 今日在线累计时长: <strong>" + _fmtSec(st.todayOnSec)     + "</strong><br>"
            "• 累计集成消耗电能: <strong>" + String(st.cumulativeWh, 3)  + " Wh</strong><br>"
            "• 最后同步日期戳  : <strong>" + String(st.lastLoggedDate)   + "</strong><br>"
            "• 上次核心闭开动作: <strong>" + tripTimeStr                 + "</strong>"
            "</p>"
            "<p class='meta' id='val-cooldown' style='border-top:1px dashed rgba(176,38,255,0.2); padding-top:10px; color:#ffea00; text-shadow:0 0 5px rgba(255,234,0,0.3);'>" + cooldownStr + "</p>";

    html += "<p class='meta' style='text-align:right;'><a href='/update' style='color:var(--neon-cyan); text-decoration:none; text-shadow:0 0 5px rgba(0,243,255,0.4);'>🚀 SYSTEM FIRMWARE UPDATE (OTA)</a></p>"
            "<script>"
            "setInterval(function(){"
              "fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
                "document.getElementById('val-v').innerHTML=d.v.toFixed(3)+' <span style=\"font-size:0.5em; color:#8b949e;\">V</span>';"
                "document.getElementById('val-a').innerHTML=(d.a/1000).toFixed(4)+' <span style=\"font-size:0.5em; color:#8b949e;\">A</span>';"
                "document.getElementById('val-w').innerHTML=(d.w/1000).toFixed(2)+' <span style=\"font-size:0.5em; color:#8b949e;\">W</span>';"
                "document.getElementById('val-mv').textContent='分流阻抗压降: '+d.mv.toFixed(2)+' mV';"
                "document.getElementById('val-status').innerHTML=d.status;"
                "document.getElementById('val-cooldown').innerHTML=d.cooldown;"
              "}).catch(function(){});"
            "},3000);"
            "</script>"
            "</div></body></html>";

    server.send(200, "text/html", html);
}

void handleApiStatus() {
    String statusHtml, cooldownHtml;
    getStatusDisplay(statusHtml, cooldownHtml); 

    String json = "{";
    json += "\"v\":"  + String(st.busVoltage,            6) + ",";
    json += "\"a\":"  + String(st.current_mA,            4) + ",";
    json += "\"w\":"  + String(st.power_mW,              4) + ",";
    json += "\"mv\":" + String(st.shuntVoltage,          4) + ",";
    statusHtml.replace("\"", "\\\"");
    cooldownHtml.replace("\"", "\\\"");
    json += "\"status\":\"" + statusHtml   + "\",";
    json += "\"cooldown\":\"" + cooldownHtml + "\"";
    json += "}";

    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "application/json", json);
}

void handleSaveSettings() {
    if (server.hasArg("tonv")) cfg.turnOnVoltage = server.arg("tonv").toFloat();
    if (server.hasArg("uv"))   cfg.underVoltage  = server.arg("uv").toFloat();
    if (server.hasArg("upw"))  cfg.underPower    = server.arg("upw").toFloat();
    if (server.hasArg("cds"))  cfg.cooldownSec   = server.arg("cds").toInt(); 
    if (server.hasArg("deu"))  cfg.displayEnabledUser = (server.arg("deu").toInt() == 1);
    if (server.hasArg("slpv")) cfg.sleepVoltage       = server.arg("slpv").toFloat();
    
    // 定时参数
    if (server.hasArg("te"))  cfg.timerEnabled = (server.arg("te").toInt() == 1);
    if (server.hasArg("st")) {
        String stVal = server.arg("st"); 
        cfg.sleepHour   = stVal.substring(0, 2).toInt();
        cfg.sleepMinute = stVal.substring(3, 5).toInt();
    }
    if (server.hasArg("wt")) {
        String wtVal = server.arg("wt"); 
        cfg.wakeHour   = wtVal.substring(0, 2).toInt();
        cfg.wakeMinute = wtVal.substring(3, 5).toInt();
    }

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

    note_begin();       
    loadConfig();
    loadSystemState();
    display_begin(); 

    // 默认开机继电器物理关闭
    st.relayOn = false;
    setRelayPhysical(false);
    
    // 初始化 ESP32-C3 默认硬件 I2C 接口
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    if (!ina.begin()) {
        Serial.println("[INA219] 初始化失败！检查接线和 I2C 地址");
    } else {
        Serial.println("[INA219] 初始化成功");
    }

    // WiFi 初始化
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

        timeClient.begin();
        timeClient.update();

        server.on("/",             HTTP_GET,  handleRoot);
        server.on("/api/status",   HTTP_GET,  handleApiStatus);
        server.on("/save_settings",HTTP_POST, handleSaveSettings);
        server.on("/on",  HTTP_GET, [](){ forceOnSystem();             server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });
        server.on("/off", HTTP_GET, [](){ executeTrip(TripReason::MANUAL); server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });
        server.on("/reset",HTTP_GET,[](){ resetSystem();               server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });

        httpUpdater.setup(&server);
        server.begin();
    } else {
        Serial.println("[Network] 无网络，独立运行");
    }

    st.lastSaveMs = millis();
}

// =================================================================
// 8. Loop
// =================================================================
void loop() {
    unsigned long now = millis();

    // ── 低压自动待机休眠检测（当未开机、电压低，且过了首次开机/唤醒后的 2 分钟安全期时触发） ──
    if (st.relayOn == false && st.busVoltage < cfg.sleepVoltage && st.busVoltage > 2.0f) {
        if (!lowVoltageSleeping && (now > sleepGuardUntilMs)) {
            lowVoltageSleeping = true;
            sleepStartMs = now;
            Serial.printf("[Sleep] 电压 %.2fV 低于阈值 %.2fV 且继电器断开，进入节能省电待机，关闭数码管及射频，冷却: %d秒...\n", 
                          st.busVoltage, cfg.sleepVoltage, cfg.cooldownSec);
            
            // 关闭 ESP32-C3 Wi-Fi 射频信号
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            currentWifiState = false;
        }
    }

    // ── 低压休眠唤醒计时 ──
    if (lowVoltageSleeping) {
        if (now - sleepStartMs >= (cfg.cooldownSec * 1000UL)) {
            lowVoltageSleeping = false;
            sleepGuardUntilMs = now + 120000UL; // 唤醒后保持 120 秒安全期不进入休眠，允许网页后台操作
            Serial.println("[Sleep] 待机冷却已过，系统自动唤醒 WiFi 建立连接（开启 2 分钟安全配置期）...");
            
            WiFi.mode(WIFI_STA);
            WiFi.begin(ssid, password);
            currentWifiState = true;
        }
    }

    // ── 定时休眠唤醒逻辑，每 5 秒检测一次（仅在非低电压休眠状态下运行） ──
    if (!lowVoltageSleeping && (now - lastWifiCheckMs >= 5000UL)) {
        lastWifiCheckMs = now;
        
        bool shouldWifiBeOn = true;
        
        if (now < 120000UL) {
            shouldWifiBeOn = true;
        } 
        else if (cfg.timerEnabled) {
            time_t now_t = time(nullptr);
            if (now_t > 1000000000L) {
                struct tm* timeinfo = localtime(&now_t);
                int currentMin = timeinfo->tm_hour * 60 + timeinfo->tm_min;
                int sleepMin = cfg.sleepHour * 60 + cfg.sleepMinute;
                int wakeMin = cfg.wakeHour * 60 + cfg.wakeMinute;
                
                bool inSleepWindow = false;
                if (wakeMin > sleepMin) {
                    if (currentMin >= sleepMin && currentMin < wakeMin) {
                        inSleepWindow = true;
                    }
                } else { 
                    if (currentMin >= sleepMin || currentMin < wakeMin) {
                        inSleepWindow = true;
                    }
                }
                
                if (inSleepWindow) {
                    shouldWifiBeOn = false;
                }
            }
        }
        
        // 应用 Wi-Fi 定时状态
        if (shouldWifiBeOn && !currentWifiState) {
            Serial.println("[Schedule] 到达唤醒时间，开启 Wi-Fi 联网...");
            WiFi.mode(WIFI_STA);
            WiFi.begin(ssid, password);
            currentWifiState = true;
        } 
        else if (!shouldWifiBeOn && currentWifiState) {
            Serial.println("[Schedule] 到达休眠时间，进入低功耗，关闭 Wi-Fi 射频信号...");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            currentWifiState = false;
        }
    }

    // ── 仅在网络开启且连接成功时执行 Web、NTP 逻辑 ──
    if (currentWifiState && WiFi.status() == WL_CONNECTED) {
        server.handleClient();
        timeClient.update();
        note_loop();
    }

    // ── 每秒采样一次（即便 Wi-Fi 定时关闭，核心监控依然正常独立运行）──
    if (now - st.lastSampleMs >= SAMPLE_INTERVAL_MS) {
        st.lastSampleMs = now;

        st.busVoltage   = ina.getVoltage();
        st.shuntVoltage = ina.getShuntVoltage() * 1000.f; 
        st.current_mA   = ina.getCurrent()      * 1000.f; 
        st.power_mW     = ina.getPower()        * 1000.f; 

        // 累计统计
        if (st.relayOn) {
            st.todayOnSec++;
            st.cumulativeWh += (st.power_mW / 1000.f) / 3600.0;
        }

        // ── 1分钟开机保护判定延迟 ──
        if (now < 60000UL) {
            st.confirmCounter = 0;
        } else {
            // ── 继电器断开时的自动恢复逻辑 ──
            if (!st.relayOn) {
                if (!st.faultLatched) {
                    st.faultLatched = true;
                    saveSystemState();
                }
                
                // 如果不是手动关闭(MANUAL)，且当前电压已经高出设定的自动开启电压值，且保护冷却倒计时已经结束，才恢复吸合
                if (st.tripReason != TripReason::MANUAL && st.busVoltage > cfg.turnOnVoltage && getCooldownRemaining() <= 0) {
                    if (++st.confirmCounter >= CONFIRM_COUNT) {
                        st.relayOn = true;
                        st.faultLatched = false;
                        st.tripReason = TripReason::NONE;
                        st.confirmCounter = 0;
                        relayOnTimeMs = millis(); 
                        setRelayPhysical(true);
                        saveSystemState();
                        Serial.printf("[Auto-ON] 保护时间已过且电压升至 %.2fV (大于阈值 %.2fV)，自动开启继电器\n", st.busVoltage, cfg.turnOnVoltage);
                    }
                } else {
                    st.confirmCounter = 0; 
                }
            } else {
                // ── 继电器吸合时的保护检测逻辑 ──
                TripReason pending = TripReason::NONE;

                if (st.busVoltage < cfg.underVoltage && st.busVoltage > 0.5f) {
                    pending = TripReason::UNDERVOLTAGE;
                } 
                // 仅在设置的低功率阈值大于 0.05W 时触发低功率切断，避免误判断
                else if ((cfg.underPower > 0.05f) && (now - relayOnTimeMs > 10000UL) && (st.power_mW < cfg.underPower * 1000.f)) {
                    pending = TripReason::OVERCURRENT; 
                }

                if (pending != TripReason::NONE) {
                    if (++st.confirmCounter >= CONFIRM_COUNT)
                        executeTrip(pending);
                } else {
                    st.confirmCounter = 0;
                }
            }
        }
    }

    // ── 每 5 分钟持久化状态数据 ──
    if (now - st.lastSaveMs >= 300000UL) {
        st.lastSaveMs = now;
        saveSystemState();
        Serial.println("[Storage] 周期归档完成");
    }

    // 更新数码管显示（融合网页显示开关和低电压睡眠检测）
    display_update(st.busVoltage, st.current_mA, cfg.displayEnabledUser && !lowVoltageSleeping);
}