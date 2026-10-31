#ifndef RELAY_MANAGER_H
#define RELAY_MANAGER_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <time.h>

#define MAX_SCHEDULES 20
#define RELAY_PIN D6 // 继电器控制引脚 D6 (GPIO12)

// ==========================================================
// 【电平控制定义】
// ON 时输出 HIGH，OFF 时输出 LOW (修正反向)
// ==========================================================
#define RELAY_ON_LEVEL   HIGH  // 开启吸合电平
#define RELAY_OFF_LEVEL  LOW   // 关闭断开电平

// HTML 模板存入 Flash (PROGMEM)，保留原有 ID 和 class 以兼容 Python 爬虫解析
const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>智能插座控制器 - AuraFan</title>
  <style>
    :root {
      --bg-start: #e0eafc; --bg-end: #cfdef3; --card-bg: rgba(255, 255, 255, 0.75);
      --text: #2c3e50; --accent: #8e44ad; --shadow: rgba(0,0,0,0.08);
      --green: #27ae60; --red: #e74c3c;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
      background: linear-gradient(135deg, var(--bg-start), var(--bg-end)); color: var(--text);
      display: flex; justify-content: center; padding: 1.5rem; min-height: 100vh;
    }
    .container { width: 100%; max-width: 580px; }
    .card {
      background: var(--card-bg); backdrop-filter: blur(12px); -webkit-backdrop-filter: blur(12px);
      border-radius: 1.25rem; padding: 1.5rem; margin-bottom: 1.25rem;
      box-shadow: 0 8px 30px var(--shadow); border: 1px solid rgba(255, 255, 255, 0.4);
    }
    h1 { text-align: center; font-size: 1.6rem; margin: 0 0 1.2rem; color: #2c3e50; font-weight: 700; }
    h2 { font-size: 1.15rem; margin: 0 0 0.8rem; color: #34495e; }
    p { font-size: 1.1rem; text-align: center; margin: 0.5rem 0; }
    .status-on { color: var(--green); font-weight: bold; }
    .status-off { color: var(--red); font-weight: bold; }
    .btn-group { display: flex; gap: 1rem; justify-content: space-around; margin-top: 0.8rem; }
    .button, button {
      padding: 0.75rem 1.2rem; font-size: 1rem; cursor: pointer; border: none;
      border-radius: 0.7rem; color: #fff; font-weight: 600; text-align: center;
      text-decoration: none; transition: all 0.2s; box-shadow: 0 4px 10px rgba(0,0,0,0.1);
    }
    .button:hover, button:hover { transform: translateY(-2px); opacity: 0.95; }
    .btn-on { background-color: var(--green); flex: 1; }
    .btn-off { background-color: var(--red); flex: 1; }
    .btn-add { background-color: var(--accent); }
    .input-group { display: flex; gap: 0.6rem; align-items: center; flex-wrap: wrap; margin-top: 0.5rem; }
    input[type="time"] {
      flex: 1; min-width: 140px; padding: 0.7rem 0.8rem; border: 1px solid #ccc;
      border-radius: 0.6rem; font-size: 1rem; background: #fff; outline: none;
    }
    ul { list-style: none; padding: 0; margin: 0.5rem 0 0; }
    li {
      background: rgba(255,255,255,0.7); border: 1px solid rgba(0,0,0,0.06); padding: 0.65rem 1rem;
      margin-bottom: 0.5rem; border-radius: 0.6rem; display: flex; justify-content: space-between; align-items: center;
    }
    li button { background-color: #7f8c8d; font-size: 0.85rem; padding: 0.35rem 0.7rem; border-radius: 0.4rem; }
    .nav-links { display: flex; justify-content: space-between; margin-top: 1rem; font-size: 0.95rem; }
    .nav-links a { color: var(--accent); text-decoration: none; font-weight: 600; }
    footer { text-align: center; margin-top: 1.8rem; font-size: 0.88rem; color: #7f8c8d; }
    footer a { color: var(--accent); text-decoration: none; font-weight: 600; }
    footer a:hover { text-decoration: underline; }
  </style>
</head>
<body>
  <div class="container">
    <h1>⚡ 智能插座控制器</h1>

    <div class="card">
      <p>当前时间: <span id="timeDisplay" style="font-family:monospace; font-weight:bold;">##CURRENT_TIME##</span></p>
      <p>继电器状态: ##RELAY_STATUS##</p>
      <div class="btn-group">
        <button class="button btn-on" onclick="manualControl(3)">手动开启</button>
        <button class="button btn-off" onclick="manualControl(4)">手动关闭</button>
      </div>
    </div>

    <div class="card">
      <h2>⏰ 添加定时任务</h2>
      <div class="input-group">
        <input type="time" id="scheduleTime" step="1" value="12:00:00">
        <button class="button btn-add" onclick="addSchedule(1)">添加开机</button>
        <button class="button btn-add" onclick="addSchedule(0)" style="background:#5c6bc0;">添加关机</button>
      </div>
    </div>

    <div class="card">
      <h2>📋 任务列表</h2>
      <h3 style="font-size:1rem; margin:0.8rem 0 0.3rem; color:var(--green);">🟢 开机任务</h3>
      <ul id="on-list">##ON_TASKS##</ul>
      <h3 style="font-size:1rem; margin:1.2rem 0 0.3rem; color:var(--red);">🔴 关机任务</h3>
      <ul id="off-list">##OFF_TASKS##</ul>
      
      <div class="nav-links">
        <a href="/">⬅ 返回一体化主控主页</a>
        <a href="/update">固件 OTA 更新 ➡</a>
      </div>
    </div>

    <footer>
      来自脚本 <a href="https://github.com/yyszone/arduino-code/tree/main/esp8266_AuraFan" target="_blank">esp8266_AuraFan</a>
    </footer>
  </div>

  <script>
    // 严格与原请求 URL (/LED-Control?ledPwm=...) 保持完全一致
    function manualControl(pwm) { window.location.href = `/LED-Control?ledPwm=${pwm}`; }
    function addSchedule(pwm) {
      const time = document.getElementById('scheduleTime').value;
      if (time) window.location.href = `/LED-Control?ledPwm=${pwm}&time=${time.split(':').length==2 ? time+':00' : time}`;
      else alert('请先选择时间！');
    }
    function deleteSchedule(time) {
      if (confirm(`确定要删除时间点 ${time} 吗?`)) window.location.href = `/LED-Control?ledPwm=5&time=${time}`;
    }
    setInterval(() => {
        const d = new Date();
        document.getElementById('timeDisplay').innerText = d.toLocaleTimeString('zh-CN', {hour12:false});
    }, 1000);
  </script>
</body>
</html>
)rawliteral";

class RelayManager {
private:
    ESP8266WebServer* server;
    bool currentState = true; // ✅ 默认通电开
    unsigned long lastCheckTime = 0;

    String onTimes[MAX_SCHEDULES];
    int onTimesCount = 0;
    String offTimes[MAX_SCHEDULES];
    int offTimesCount = 0;

    void setHardware(bool state) {
        currentState = state;
        digitalWrite(RELAY_PIN, state ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
    }

public:
    RelayManager() : server(nullptr) {}

    String getNowTime() {
        time_t now = time(nullptr);
        if (now < 100000) return "00:00:00";
        struct tm* t = localtime(&now);
        char buf[10];
        strftime(buf, sizeof(buf), "%H:%M:%S", t);
        return String(buf);
    }

    bool getState() {
        return currentState;
    }

    void saveSchedules() {
        File f = LittleFS.open("/schedules.dat", "w");
        if (!f) return;
        f.println(onTimesCount);
        for (int i = 0; i < onTimesCount; i++) f.println(onTimes[i]);
        f.println(offTimesCount);
        for (int i = 0; i < offTimesCount; i++) f.println(offTimes[i]);
        f.close();
    }

    void loadSchedules() {
        if (!LittleFS.exists("/schedules.dat")) return;
        File f = LittleFS.open("/schedules.dat", "r");
        if (!f) return;
        onTimesCount = f.readStringUntil('\n').toInt();
        for (int i = 0; i < onTimesCount && i < MAX_SCHEDULES; i++) {
            onTimes[i] = f.readStringUntil('\n');
            onTimes[i].trim();
        }
        offTimesCount = f.readStringUntil('\n').toInt();
        for (int i = 0; i < offTimesCount && i < MAX_SCHEDULES; i++) {
            offTimes[i] = f.readStringUntil('\n');
            offTimes[i].trim();
        }
        f.close();
    }

    String generatePage() {
        String page = FPSTR(HTML_TEMPLATE);
        String relayStatus = currentState ? "<span class='status-on'>开启</span>" : "<span class='status-off'>关闭</span>";
        
        String onTasksHtml = "";
        if (onTimesCount == 0) onTasksHtml = "<li>无</li>";
        else {
            for (int i = 0; i < onTimesCount; i++) {
                onTasksHtml += "<li>" + onTimes[i] + "<button onclick='deleteSchedule(\"" + onTimes[i] + "\")'>删除</button></li>";
            }
        }

        String offTasksHtml = "";
        if (offTimesCount == 0) offTasksHtml = "<li>无</li>";
        else {
            for (int i = 0; i < offTimesCount; i++) {
                offTasksHtml += "<li>" + offTimes[i] + "<button onclick='deleteSchedule(\"" + offTimes[i] + "\")'>删除</button></li>";
            }
        }

        page.replace("##CURRENT_TIME##", getNowTime());
        page.replace("##RELAY_STATUS##", relayStatus);
        page.replace("##ON_TASKS##", onTasksHtml);
        page.replace("##OFF_TASKS##", offTasksHtml);
        return page;
    }

    void begin(ESP8266WebServer* webServer) {
        server = webServer;

        // ✅ 默认开机开启
        digitalWrite(RELAY_PIN, RELAY_ON_LEVEL);
        pinMode(RELAY_PIN, OUTPUT);
        currentState = true;

        if (LittleFS.begin()) {
            loadSchedules();
        }

        // NTP 中国时间同步
        configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tuna.tsinghua.edu.cn", "pool.ntp.org");

        // 等待 NTP 校时
        time_t now = time(nullptr);
        int retry = 0;
        while (now < 1000000 && retry < 15) {
            delay(200);
            now = time(nullptr);
            retry++;
        }

        // ✅ 自动设置 40 分钟后关闭
        if (now > 1000000) {
            time_t future = now + (40 * 60);
            struct tm* fInfo = localtime(&future);
            char buf[10];
            strftime(buf, sizeof(buf), "%H:%M:%S", fInfo);
            String autoOffTime = String(buf);
            if (offTimesCount < MAX_SCHEDULES) {
                offTimes[offTimesCount++] = autoOffTime;
                saveSchedules();
            }
        }

        setupRoutes();
    }

    // 核心：严格对齐原 Python 脚本的 URL 调度逻辑
    void handleLedControl() {
        String ledPwm = server->arg("ledPwm");
        int ledPwmVal = ledPwm.toInt();
        String timeParam = server->arg("time");

        switch (ledPwmVal) {
            case 0: // 添加关机
                if (offTimesCount < MAX_SCHEDULES && timeParam.length() > 0) {
                    offTimes[offTimesCount++] = timeParam;
                    saveSchedules();
                }
                break;
            case 1: // 添加开机
                if (onTimesCount < MAX_SCHEDULES && timeParam.length() > 0) {
                    onTimes[onTimesCount++] = timeParam;
                    saveSchedules();
                }
                break;
            case 2: // 手动同步时间备用
                break;
            case 3: // 手动开启
                setHardware(true);
                break;
            case 4: // 手动关闭
                setHardware(false);
                break;
            case 5: { // 删除计划
                int idx = -1;
                for (int i = 0; i < offTimesCount; i++) {
                    if (offTimes[i] == timeParam) { idx = i; break; }
                }
                if (idx != -1) {
                    for (int i = idx; i < offTimesCount - 1; i++) offTimes[i] = offTimes[i + 1];
                    offTimes[--offTimesCount] = "";
                    saveSchedules();
                }

                idx = -1;
                for (int i = 0; i < onTimesCount; i++) {
                    if (onTimes[i] == timeParam) { idx = i; break; }
                }
                if (idx != -1) {
                    for (int i = idx; i < onTimesCount - 1; i++) onTimes[i] = onTimes[i + 1];
                    onTimes[--onTimesCount] = "";
                    saveSchedules();
                }
                break;
            }
        }
        server->send(200, "text/html; charset=UTF-8", generatePage());
    }

    void setupRoutes() {
        // =========================================================================
        // 🌟 1. 适配 NewSwitch2400Optimized 的三大专属接口
        // =========================================================================

        // ① 清空所有任务 (/deleteAllTasks?relay=1)
        server->on("/deleteAllTasks", [this]() {
            this->onTimesCount = 0;
            this->offTimesCount = 0;
            this->saveSchedules();
            this->server->send(200, "text/plain", "All tasks deleted successfully");
        });

        // ② 手动设置开关状态 (/setSocketTask?cmd=manual&relay=1&state=1/0)
        server->on("/setSocketTask", [this]() {
            String cmd = this->server->arg("cmd");
            if (cmd == "manual" && this->server->hasArg("state")) {
                int stateVal = this->server->arg("state").toInt();
                this->setHardware(stateVal == 1);
            }
            this->server->send(200, "text/plain", "OK");
        });

        // ③ 返回插座 JSON 状态，满足 Go 代码读取 relay2 和 relay3 校验要求
        server->on("/getSocketData", [this]() {
            String stateStr = this->currentState ? "true" : "false";
            String json = "{\"relay1\":" + stateStr + ",\"relay2\":" + stateStr + ",\"relay3\":" + stateStr + "}";
            this->server->send(200, "application/json", json);
        });

        // =========================================================================
        // 🌟 ④ 【就是漏了这里！】首页“一键切换”按钮专属接口
        // =========================================================================
        server->on("/relay/api/toggle", [this]() {
            if (this->server->hasArg("state")) {
                String s = this->server->arg("state");
                this->setHardware(s == "1" || s == "true" || s == "on");
            } else {
                // 点击一键切换时，没有传参，直接状态取反（开变关，关变开）
                this->setHardware(!this->currentState);
            }
            this->server->send(200, "text/plain", "OK");
        });

        // =========================================================================
        // 🌟 2. 适配经典版 Switch2400 接口 (支持 GET 和 POST，兼容 relay 参数)
        // =========================================================================
        server->on("/LED-Control", [this]() {
            String ledPwm = this->server->arg("ledPwm");
            int ledPwmVal = ledPwm.toInt();
            String timeParam = this->server->arg("time");

            switch (ledPwmVal) {
                case 0:
                    if (this->offTimesCount < MAX_SCHEDULES && timeParam.length() > 0) {
                        this->offTimes[this->offTimesCount++] = timeParam;
                        this->saveSchedules();
                    }
                    break;
                case 1:
                    if (this->onTimesCount < MAX_SCHEDULES && timeParam.length() > 0) {
                        this->onTimes[this->onTimesCount++] = timeParam;
                        this->saveSchedules();
                    }
                    break;
                case 2: break;
                case 3: this->setHardware(true); break;
                case 4: this->setHardware(false); break;
                case 5: {
                    int idx = -1;
                    for (int i = 0; i < this->offTimesCount; i++) {
                        if (this->offTimes[i] == timeParam) { idx = i; break; }
                    }
                    if (idx != -1) {
                        for (int i = idx; i < this->offTimesCount - 1; i++) this->offTimes[i] = this->offTimes[i + 1];
                        this->offTimes[--this->offTimesCount] = "";
                        this->saveSchedules();
                    }

                    idx = -1;
                    for (int i = 0; i < this->onTimesCount; i++) {
                        if (this->onTimes[i] == timeParam) { idx = i; break; }
                    }
                    if (idx != -1) {
                        for (int i = idx; i < this->onTimesCount - 1; i++) this->onTimes[i] = this->onTimes[i + 1];
                        this->onTimes[--this->onTimesCount] = "";
                        this->saveSchedules();
                    }
                    break;
                }
            }
            this->server->send(200, "text/html; charset=UTF-8", this->generatePage());
        });

        server->on("/relay", [this]() {
            this->server->send(200, "text/html; charset=UTF-8", this->generatePage());
        });
    }

    // 在主循环 loop 中稳定轮询
    void update() {
        if (millis() - lastCheckTime >= 1000) {
            lastCheckTime = millis();
            String cTime = getNowTime();
            if (cTime == "00:00:00") return;

            for (int i = 0; i < onTimesCount; i++) {
                if (onTimes[i] == cTime) setHardware(true);
            }
            for (int i = 0; i < offTimesCount; i++) {
                if (offTimes[i] == cTime) setHardware(false);
            }
        }
    }
};

#endif