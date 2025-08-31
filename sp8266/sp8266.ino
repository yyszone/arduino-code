#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h> // 用于网页固件更新
#include <Ticker.h>

#define MAX_SCHEDULES 20 // 最多支持20个时间计划

// --- 全局变量 ---
Ticker ticker;
String onTimes[MAX_SCHEDULES];
int onTimesindex = 0;
String offTimes[MAX_SCHEDULES];
int offTimesindex = 0;

int relay = 0; // 继电器引脚, ESP-01S 对应 GPIO0
ESP8266WebServer esp8266_server(80);
ESP8266HTTPUpdateServer httpUpdater; // 创建网页更新服务器对象
String currentTime = "00:00:00";

int hours = 0;    // 当前小时数
int minutes = 0;  // 当前分钟数
int seconds = 0;  // 当前秒数

// --- HTML 页面模板 (已添加更新页面的入口) ---
const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP-01S 智能插座</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background-color: #f4f7f6; margin: 0; padding: 20px; color: #333; }
    .container { max-width: 600px; margin: 0 auto; background: #fff; padding: 25px; border-radius: 10px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); }
    h1, h2 { text-align: center; color: #007bff; }
    h2 { border-top: 1px solid #eee; padding-top: 20px; margin-top: 20px; }
    .status-box, .control-box, .schedule-box { margin-bottom: 25px; }
    p { font-size: 1.2em; text-align: center; }
    #timeDisplay { font-weight: bold; color: #333; }
    .status-on { color: #28a745; font-weight: bold; }
    .status-off { color: #dc3545; font-weight: bold; }
    .btn-group { display: flex; justify-content: space-around; gap: 15px; }
    button, .button { display: inline-block; padding: 12px 20px; font-size: 1em; cursor: pointer; border: none; border-radius: 5px; color: #fff; text-align: center; text-decoration: none; }
    .btn-on { background-color: #28a745; }
    .btn-on:hover { background-color: #218838; }
    .btn-off { background-color: #dc3545; }
    .btn-off:hover { background-color: #c82333; }
    .btn-add { background-color: #007bff; }
    .btn-add:hover { background-color: #0069d9; }
    .btn-sync { background-color: #ffc107; color: #212529; }
    .btn-sync:hover { background-color: #e0a800; }
    .btn-update { background-color: #6c757d; }
    .btn-update:hover { background-color: #5a6268; }
    .input-group { display: flex; gap: 10px; margin-top: 10px; }
    input[type="time"] { flex-grow: 1; padding: 10px; border: 1px solid #ccc; border-radius: 5px; font-size: 1em; }
    ul { list-style: none; padding: 0; }
    li { background: #f9f9f9; border: 1px solid #eee; padding: 10px; margin-bottom: 8px; border-radius: 5px; display: flex; justify-content: space-between; align-items: center; }
    li button { background-color: #6c757d; font-size: 0.8em; padding: 5px 10px; }
    li button:hover { background-color: #5a6268; }
  </style>
</head>
<body>
  <div class="container">
    <h1>智能插座控制器</h1>
    <div class="status-box">
      <p>当前时间: <span id="timeDisplay">##CURRENT_TIME##</span></p>
      <p>继电器状态: ##RELAY_STATUS##</p>
    </div>

    <div class="control-box">
      <h2>手动控制</h2>
      <div class="btn-group">
        <button class="button btn-on" onclick="manualControl(3)">手动开启</button>
        <button class="button btn-off" onclick="manualControl(4)">手动关闭</button>
      </div>
    </div>

    <div class="schedule-box">
      <h2>添加定时任务</h2>
      <div class="input-group">
        <input type="time" id="scheduleTime" step="1" value="12:00:00">
        <button class="button btn-add" onclick="addSchedule(1)">添加开机</button>
        <button class="button btn-add" onclick="addSchedule(0)">添加关机</button>
      </div>
    </div>

    <div class="schedule-box">
        <h2>同步设备时间</h2>
        <div class="input-group">
          <input type="time" id="syncTime" step="1">
          <button class="button btn-sync" onclick="syncDeviceTime()">同步时间</button>
        </div>
      </div>

    <div class="schedule-box">
      <h2>任务列表</h2>
      <h3>开机任务</h3>
      <ul id="on-list">##ON_TASKS##</ul>
      <h3>关机任务</h3>
      <ul id="off-list">##OFF_TASKS##</ul>
    </div>

    <div class="schedule-box">
        <h2>固件更新</h2>
        <p><a href="/update" class="button btn-update">前往固件更新页面</a></p>
    </div>
  </div>

  <script>
    let hours = parseInt('##HOURS##');
    let minutes = parseInt('##MINUTES##');
    let seconds = parseInt('##SECONDS##');

    function updateTime() {
      seconds++;
      if (seconds >= 60) { seconds = 0; minutes++; }
      if (minutes >= 60) { minutes = 0; hours++; }
      if (hours >= 24) { hours = 0; }
      
      const pad = (num) => String(num).padStart(2, '0');
      document.getElementById('timeDisplay').textContent = `${pad(hours)}:${pad(minutes)}:${pad(seconds)}`;
    }
    setInterval(updateTime, 1000);

    function manualControl(pwm) {
      window.location.href = `/LED-Control?ledPwm=${pwm}`;
    }

    function addSchedule(pwm) {
      const time = document.getElementById('scheduleTime').value;
      if (time) {
        window.location.href = `/LED-Control?ledPwm=${pwm}&time=${time}`;
      } else {
        alert('请先选择时间！');
      }
    }

    function deleteSchedule(time) {
      if (confirm(`确定要删除时间点 ${time} 吗?`)) {
        window.location.href = `/LED-Control?ledPwm=5&time=${time}`;
      }
    }
    
    function syncDeviceTime() {
        const time = document.getElementById('syncTime').value;
        if (time) {
             let fullTime = time.split(':').length === 2 ? time + ':00' : time;
             window.location.href = `/LED-Control?ledPwm=2&time=${fullTime}`;
        } else {
            alert('请选择要同步的时间！');
        }
    }

    document.addEventListener('DOMContentLoaded', (event) => {
        const now = new Date();
        const pad = (num) => String(num).padStart(2, '0');
        const currentTimeString = `${pad(now.getHours())}:${pad(now.getMinutes())}`;
        document.getElementById('syncTime').value = currentTimeString;
    });
  </script>
</body>
</html>
)rawliteral";

// --- 函数声明 ---
String generatePage();

void setup(void) {
  pinMode(relay, OUTPUT);
  digitalWrite(relay, LOW); // 默认打开继电器 (低电平触发的继电器)
  
  Serial.begin(115200);
  Serial.println("\nBooting...");

  WiFi.begin("yang1234", "y123456789"); // 输入你的WiFi SSID和密码
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // --- Web服务器路由 ---
  esp8266_server.on("/LED-Control", []() {
    String ledPwm = esp8266_server.arg("ledPwm");
    int ledPwmVal = ledPwm.toInt();
    String timeParam = esp8266_server.arg("time");

    switch(ledPwmVal) {
      case 0: if (offTimesindex < MAX_SCHEDULES) offTimes[offTimesindex++] = timeParam; break;
      case 1: if (onTimesindex < MAX_SCHEDULES) onTimes[onTimesindex++] = timeParam; break;
      case 2: sscanf(timeParam.c_str(), "%d:%d:%d", &hours, &minutes, &seconds); break;
      case 3: digitalWrite(relay, LOW); break;
      case 4: digitalWrite(relay, HIGH); break;
      case 5: {
        int indexToDelete = -1;
        for (int i = 0; i < offTimesindex; i++) if (offTimes[i] == timeParam) { indexToDelete = i; break; }
        if (indexToDelete != -1) { for (int i = indexToDelete; i < offTimesindex - 1; i++) offTimes[i] = offTimes[i + 1]; offTimes[--offTimesindex] = ""; }
        indexToDelete = -1;
        for (int i = 0; i < onTimesindex; i++) if (onTimes[i] == timeParam) { indexToDelete = i; break; }
        if (indexToDelete != -1) { for (int i = indexToDelete; i < onTimesindex - 1; i++) onTimes[i] = onTimes[i + 1]; onTimes[--onTimesindex] = ""; }
        break;
      }
    }

    String referer = esp8266_server.header("Referer");
    if (referer.indexOf(WiFi.localIP().toString()) > -1) {
      esp8266_server.sendHeader("Location", "/", true);
      esp8266_server.send(302, "text/plain", "");
    } else {
      esp8266_server.send(200, "text/html", generatePage());
    }
  });

  esp8266_server.on("/", [](){ esp8266_server.send(200, "text/html", generatePage()); });

  // --- Web Updater 初始化 ---
  // 设置更新页面的URL为 /update
  // 也可以为更新页面设置用户名和密码，增加安全性，例如: httpUpdater.setup(&esp8266_server, "/update", "admin", "password");
  httpUpdater.setup(&esp8266_server, "/update");
  Serial.println("HTTPUpdateServer ready! Open http://[IP]/update");

  esp8266_server.onNotFound([](){ esp8266_server.send(404, "text/plain", "404 Not Found"); });
  esp8266_server.begin();
  Serial.println("HTTP server started.");

  // --- Ticker 定时器 ---
  ticker.attach(1, []() {
    seconds++;
    if (seconds >= 60) { seconds = 0; minutes++; if (minutes >= 60) { minutes = 0; hours++; if (hours >= 24) hours = 0; } }

    char timeBuffer[9];
    sprintf(timeBuffer, "%02d:%02d:%02d", hours, minutes, seconds);
    currentTime = String(timeBuffer);

    for (int i = 0; i < onTimesindex; i++) if (onTimes[i] == currentTime) { digitalWrite(relay, LOW); Serial.println("Scheduled ON"); break; }
    for (int i = 0; i < offTimesindex; i++) if (offTimes[i] == currentTime) { digitalWrite(relay, HIGH); Serial.println("Scheduled OFF"); break; }
  });
}

void loop(void) {
  // 这一行会同时处理普通网页请求和固件更新请求
  esp8266_server.handleClient();
}

// --- 动态生成HTML页面的函数 ---
String generatePage() {
  String page = FPSTR(HTML_TEMPLATE); // 从PROGMEM加载HTML模板

  String relayStatus = (digitalRead(relay) == LOW) ? "<span class='status-on'>开启</span>" : "<span class='status-off'>关闭</span>";
  
  String onTasksHtml = "";
  if (onTimesindex == 0) onTasksHtml = "<li>无</li>";
  else { for (int i = 0; i < onTimesindex; i++) onTasksHtml += "<li>" + onTimes[i] + "<button onclick='deleteSchedule(\"" + onTimes[i] + "\")'>删除</button></li>"; }

  String offTasksHtml = "";
  if (offTimesindex == 0) offTasksHtml = "<li>无</li>";
  else { for (int i = 0; i < offTimesindex; i++) offTasksHtml += "<li>" + offTimes[i] + "<button onclick='deleteSchedule(\"" + offTimes[i] + "\")'>删除</button></li>"; }

  page.replace("##CURRENT_TIME##", currentTime);
  page.replace("##RELAY_STATUS##", relayStatus);
  page.replace("##ON_TASKS##", onTasksHtml);
  page.replace("##OFF_TASKS##", offTasksHtml);
  page.replace("##HOURS##", String(hours));
  page.replace("##MINUTES##", String(minutes));
  page.replace("##SECONDS##", String(seconds));

  return page;
}