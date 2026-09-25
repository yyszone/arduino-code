#ifndef NETWORK_WEB_H
#define NETWORK_WEB_H

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <NTPClient.h>
#include <IRsend.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <FS.h>
#include <LittleFS.h>
#include "config.h"

extern Settings settings;
extern SystemState st;
extern WebServer server;
extern NTPClient timeClient;
extern IRsend irsend;
extern HTTPUpdateServer httpUpdater;

extern bool haDeviceState;
extern bool httpDeviceState;
extern bool irLightState;
extern bool relayState;
extern bool manualOff;
extern unsigned long manualOffMillis;
extern time_t manualOffEpoch;
extern bool isInStandby;

extern float dhtTemp;
extern float dhtHum;

extern unsigned long lastActivityTime;
extern unsigned long lastStatusUpdate;

extern ScreenMode currentScreen;

extern String weather_main;
extern String weather_temp;
extern String weather_desc;
extern unsigned long lastWeatherUpdate;
extern const unsigned long WEATHER_UPDATE_INTERVAL;

extern LogEntry logBuffer[];
extern int currentLogIndex;
extern bool logBufferFull;
extern const int MAX_LOG_ENTRIES;

extern void setRelay(bool state);
extern void executeTrip(TripReason reason);
extern void forceOnSystem();
extern void resetSystem();
extern void drawWeatherScreen();
extern void drawControlScreen();
extern void drawCurrentScreen(bool forceRedraw);
extern void manualSafeOff();
extern long getCooldownRemaining();
extern long getManualOffRemaining();

void handleRoot();
void handleApiStatus();
void handleSettings();
void handleSaveINA();
void handleSaveIR();
void handleIrCommand();
void handleLogs();
void controlHA(bool state);
void controlHttp(bool state);

// 格式化秒数为人性化时间（如：15分30秒）
String formatSeconds(long secs) {
  if (secs <= 0) return "0秒";
  if (secs < 60) return String(secs) + "秒";
  long mins = secs / 60;
  long rsecs = secs % 60;
  if (mins < 60) return String(mins) + "分" + String(rsecs) + "秒";
  long hrs = mins / 60;
  long rmins = mins % 60;
  return String(hrs) + "小时" + String(rmins) + "分" + String(rsecs) + "秒";
}

void addLog(String m) {
  if (WiFi.status() == WL_CONNECTED && timeClient.getEpochTime() > 0) {
    logBuffer[currentLogIndex] = {timeClient.getFormattedTime(), m, timeClient.getEpochTime()}; 
  } else {
    logBuffer[currentLogIndex] = {"[No Time]", m, 0};
  }
  currentLogIndex = (currentLogIndex + 1) % MAX_LOG_ENTRIES;
  if (currentLogIndex == 0) logBufferFull = true;
  Serial.println("LOG: " + m);
}

void controlHttp(bool state) {
  if (WiFi.status() != WL_CONNECTED) return;
  addLog("HTTP 设备控制: " + String(state ? "ON" : "OFF"));
  WiFiClient client; 
  HTTPClient http;
  http.setTimeout(1200); 
  if (http.begin(client, state ? led_on_url : led_off_url)) { 
    http.GET(); 
    http.end(); 
  }
}

void controlHA(bool state) {
  if (isInStandby || WiFi.status() != WL_CONNECTED) return;

  if (strlen(settings.haToken) < 10) {
    addLog("HA 失败: 未在网页设置 Token！");
    return;
  }

  WiFiClient client; 
  HTTPClient http;
  http.setTimeout(1500); 

  String service = state ? "turn_on" : "turn_off";
  String url = "http://" + String(settings.haHost) + ":" + String(settings.haPort) + "/api/services/homeassistant/" + service;

  addLog("HA 请求: " + service + " -> " + String(settings.haEntity));

  if (http.begin(client, url)) {
    http.addHeader("Authorization", "Bearer " + String(settings.haToken));
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.POST("{\"entity_id\":\"" + String(settings.haEntity) + "\"}");

    if (httpCode == 200 || httpCode == 201) {
      addLog("HA 控制成功! [200 OK]");
    } else if (httpCode == 401) {
      addLog("HA 失败 [401]: Token 凭据无效");
    } else if (httpCode == 404) {
      addLog("HA 失败 [404]: 实体 ID 不存在");
    } else {
      addLog("HA 失败, 响应码: " + String(httpCode));
    }
    http.end();
  } else {
    addLog("HA 连接失败: 无法连接 " + String(settings.haHost));
  }
}

void updateWeather() {
  if (strlen(settings.weatherApiKey) < 10 || strlen(settings.weatherCity) == 0) return;
  
  WiFiClient client;
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + String(settings.weatherCity) + "&appid=" + String(settings.weatherApiKey) + "&units=metric&lang=en";
  
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
        StaticJsonDocument<1024> doc;
        deserializeJson(doc, http.getString());
        
        weather_temp = String((int)doc["main"]["temp"]);
        weather_main = (const char*)doc["weather"][0]["main"];
        weather_desc = (const char*)doc["weather"][0]["description"];
        
        weather_main.toUpperCase(); 
        weather_desc.toUpperCase();
        
        addLog("天气已更新: " + weather_temp + "C");
        lastWeatherUpdate = millis();
        if (currentScreen == SCREEN_WEATHER) drawWeatherScreen();
    }
    http.end();
  }
}

void updateStatusLine() {
  tft.fillRect(0, 305, 240, 15, C_BG);
  tft.drawFastHLine(0, 304, 240, C_CYAN);
  tft.setTextSize(1);
  tft.setTextColor(C_GREEN);
  tft.setCursor(2, 308);
  if (WiFi.status() == WL_CONNECTED) {
    tft.print("IP:");
    tft.print(WiFi.localIP());
  } else {
    tft.setTextColor(C_RED);
    tft.print("NET_ERR");
  }
  
  tft.setTextColor(C_ORANGE);
  tft.setCursor(150, 308);
  tft.print("[TOUCH: MENU]");
}

void enterStandby() {
  if (isInStandby) return;

  isInStandby = true;
  addLog("进入待机模式。");

  // 先关闭背光
  digitalWrite(TFT_BL, LOW);
  delay(20);

  // 让触摸和 TFT 都释放 SPI 总线
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);

  // ILI9341 进入睡眠
  tft.writeCommand(ILI9341_SLPIN);
  delay(120);

  // 停止网络
  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // 降低 CPU 频率
  setCpuFrequencyMhz(80);
}

void setupWifiAndServices(bool wifiAlreadyConnected);

void exitStandby(bool wifiAlreadyConnected) {

  // 先恢复 CPU
  setCpuFrequencyMhz(160);
  delay(20);

  // 背光先保持关闭
  digitalWrite(TFT_BL, LOW);

  // 释放 SPI
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);

  // ===== 完整重建 SPI =====
  SPI.end();
  delay(20);

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);
  delay(20);

  // ===== 硬件复位 ILI9341 =====
  pinMode(TFT_RST, OUTPUT);

  digitalWrite(TFT_RST, LOW);
  delay(50);

  digitalWrite(TFT_RST, HIGH);
  delay(150);

  // ===== 重新初始化 TFT =====
  tft.begin(20000000);
  delay(20);

  tft.setRotation(0);

  // 重新初始化触摸
  ts.begin();
  delay(20);

  // 明确退出睡眠
  tft.writeCommand(ILI9341_SLPOUT);
  delay(120);

  tft.writeCommand(ILI9341_DISPON);
  delay(20);

  // 先清屏
  tft.fillScreen(C_BG);

  // 到这里才打开背光
  digitalWrite(TFT_BL, HIGH);
  delay(30);

  isInStandby = false;
  lastActivityTime = millis();

  // 强制下一次重新绘制
  lastScreenSwitchTime = millis();

  addLog("退出待机模式，TFT 已完整重新初始化。");

  setupWifiAndServices(wifiAlreadyConnected);

  // 网络初始化完成后立即重绘
  drawCurrentScreen(true);
}

void setupWifiAndServices(bool wifiAlreadyConnected) {

  if (!wifiAlreadyConnected) {

    tft.fillScreen(C_BG);
    tft.setTextColor(C_GREEN);
    tft.setTextSize(2);
    tft.setCursor(10, 100);
    tft.print("CONNECTING NETWORK...");

    Serial.println();
    Serial.println("========== WiFi 连接开始 ==========");

    // 不要使用 disconnect(true)
    // true 会清除底层连接配置/状态，没必要每次都这么做
    WiFi.disconnect(false);
    delay(300);

    WiFi.mode(WIFI_STA);

    // 允许 ESP32 自动重连
    WiFi.setAutoReconnect(true);

    // 不让 WiFi 配置频繁写 Flash
    WiFi.persistent(false);

    Serial.print("[WiFi] SSID: ");
    Serial.println(ssid);

    WiFi.begin(ssid, password);

    unsigned long start = millis();
    unsigned long lastPrint = 0;

    // 最多等待 20 秒
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < 20000UL) {

      delay(250);

      if (millis() - lastPrint >= 1000) {
        lastPrint = millis();

        Serial.printf(
          "[WiFi] 等待中... status=%d RSSI=%d\n",
          WiFi.status(),
          WiFi.RSSI()
        );
      }
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {

      Serial.println("========== WiFi 连接成功 ==========");
      Serial.print("[WiFi] IP: ");
      Serial.println(WiFi.localIP());

      Serial.print("[WiFi] RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");

      Serial.print("[WiFi] Gateway: ");
      Serial.println(WiFi.gatewayIP());

      Serial.print("[WiFi] DNS: ");
      Serial.println(WiFi.dnsIP());

    } else {

      Serial.println("========== WiFi 连接失败 ==========");

      Serial.printf(
        "[WiFi] 最终状态码: %d\n",
        WiFi.status()
      );

      Serial.println("[WiFi] 之后由后台自动重连");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {

    addLog(
      "WiFi 已连接, IP: " +
      WiFi.localIP().toString()
    );

    timeClient.begin();
    timeClient.update();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/save_ina", HTTP_POST, handleSaveINA);
    server.on("/save_ir", HTTP_POST, handleSaveIR);
    server.on("/ir", HTTP_GET, handleIrCommand);

    server.on("/on", HTTP_GET, [](){
      forceOnSystem();
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "");
    });

    server.on("/off", HTTP_GET, [](){
      executeTrip(TripReason::MANUAL);
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "");
    });

    server.on("/reset", HTTP_GET, [](){
      resetSystem();
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "");
    });

    server.on("/logs", HTTP_GET, handleLogs);
    server.on("/tft-reset", HTTP_GET, []() {
      Serial.println("[TFT] HTTP 强制恢复");

      digitalWrite(TFT_BL, LOW);

      pinMode(TFT_CS, OUTPUT);
      digitalWrite(TFT_CS, HIGH);

      pinMode(T_CS, OUTPUT);
      digitalWrite(T_CS, HIGH);

      SPI.end();
      delay(20);

      SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI);
      delay(20);

      pinMode(TFT_RST, OUTPUT);

      digitalWrite(TFT_RST, LOW);
      delay(50);

      digitalWrite(TFT_RST, HIGH);
      delay(150);

      tft.begin(20000000);
      tft.setRotation(0);

      ts.begin();

      delay(30);

      tft.writeCommand(ILI9341_SLPOUT);
      delay(120);

      tft.writeCommand(ILI9341_DISPON);
      delay(20);

      tft.fillScreen(C_BG);

      digitalWrite(TFT_BL, HIGH);

      drawCurrentScreen(true);

      server.send(200, "text/plain", "TFT RESET OK");
    });

    httpUpdater.setup(&server);
    server.begin();

    updateWeather();

    addLog(
      "网络服务启动成功，IP: " +
      WiFi.localIP().toString()
    );

  } else {

    addLog(
      "WiFi 连接失败，等待后台自动重连。"
    );
  }

  drawCurrentScreen(true);
}

void handleIrCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String cmd = server.arg("cmd");
  unsigned long code_to_send = 0;
  
  if (cmd.equals("on")) { code_to_send = settings.ir_on; irLightState = true; }
  else if (cmd.equals("off")) { code_to_send = settings.ir_off; irLightState = false; }
  else if (cmd.equals("bright_up")) code_to_send = settings.ir_bright_up;
  else if (cmd.equals("bright_down")) code_to_send = settings.ir_bright_down;
  
  if (code_to_send != 0) {
    irsend.sendNEC(code_to_send);
    addLog("网页红外遥控发码: " + cmd);
  }
  
  if (currentScreen == SCREEN_CONTROL && !isInStandby) {
    drawControlScreen();
  }
  
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// ⭐️ 生成网页与 API 状态 HTML
String buildStatusHtml() {

  unsigned long nowMs = millis();

  if (nowMs < 60000UL) {
    long remWarm = (60000UL - nowMs) / 1000UL;

    return "<span style='color:#f0883e;font-weight:bold;'>"
           "⏳ 系统开机预热中（剩余 " +
           String(remWarm) +
           " 秒）</span>";
  }

  if (st.relayOn) {
    return "<span style='color:#3fb950;font-weight:bold;'>"
           "✔ 继电器吸合（正常运行）</span>";
  }

  // 手动安全断开冷却
  long manualRemain = getManualOffRemaining();

  if (manualOff || manualRemain > 0) {

    return "<div style='color:#d29922;font-weight:bold;line-height:1.6;'>"
           "🛑 <b>手动安全断开</b><br>"
           "⏳ <b>冷却倒计时："
           "<span style='color:#ffea00;font-size:1.1em;'>"
           + formatSeconds(manualRemain) +
           "</span></b><br>"
           "<small style='color:#8b949e;font-size:0.8em;'>"
           "冷却结束后自动恢复控制"
           "</small>"
           "</div>";
  }

  long rem = getCooldownRemaining();

  if (rem > 0) {

    String rText =
        (st.tripReason == TripReason::UNDERVOLTAGE)
        ? "欠压保护切断"
        : (st.tripReason == TripReason::OVERCURRENT)
        ? "低功率保护切断"
        : "保护触发";

    return "<div style='color:#d29922;font-weight:bold;line-height:1.6;'>"
           "🛡️ <b>" + rText + "</b><br>"
           "⏳ <b>冷却锁定倒计时："
           "<span style='color:#ffea00;font-size:1.1em;'>"
           + formatSeconds(rem) +
           "</span></b><br>"
           "<small style='color:#8b949e;font-size:0.8em;'>"
           "在此期间防频繁通断保护中，无法自动吸合"
           "</small>"
           "</div>";
  }

  if (st.busVoltage > settings.turnOnVoltage) {
    return "<span style='color:#58a6ff;font-weight:bold;'>"
           "⏳ 电压高于开启阈值，正在确认采样...</span>";
  }

  return "<span style='color:#8b949e;font-weight:bold;'>"
         "🔍 待机监测中（待电压高于 " +
         String(settings.turnOnVoltage, 1) +
         "V 自动吸合）</span>";
}

void handleApiStatus() {
  String statusHtml = buildStatusHtml();

  String json = "{";
  json += "\"v\":"  + String(st.busVoltage, 3) + ",";
  json += "\"a\":"  + String(st.current_mA, 1) + ",";
  json += "\"w\":"  + String(st.power_mW, 1) + ",";
  json += "\"mv\":" + String(st.shuntVoltage, 2) + ",";
  statusHtml.replace("\"", "\\\"");
  json += "\"status\":\"" + statusHtml + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", json);
}

void handleRoot() {
  String page = FPSTR(MAIN_HTML);
  
  page.replace("##VOLTAGE##", String(st.busVoltage, 3));
  page.replace("##CURRENT##", String(st.current_mA / 1000.f, 3));
  page.replace("##POWER##", String(st.power_mW / 1000.f, 2));
  page.replace("##SHUNT_MV##", String(st.shuntVoltage, 2));

  page.replace("##PROTECT_STATUS##", buildStatusHtml());

  page.replace("##TURN_ON_V##", String(settings.turnOnVoltage, 1));
  page.replace("##UNDER_V##", String(settings.underVoltage, 1));
  page.replace("##UNDER_P##", String(settings.underPower, 1));
  page.replace("##COOLDOWN_S##", String(settings.cooldownSec));

  page.replace("##HA_HOST##", String(settings.haHost));
  page.replace("##HA_PORT##", String(settings.haPort));
  page.replace("##HA_ENTITY##", String(settings.haEntity));
  page.replace("##HA_TOKEN##", String(settings.haToken));

  char time_buf[6];
  sprintf(time_buf, "%02d:%02d", settings.sleepHour, settings.sleepMinute);
  page.replace("##SLEEP_TIME##", time_buf);
  sprintf(time_buf, "%02d:%02d", settings.wakeHour, settings.wakeMinute);
  page.replace("##WAKE_TIME##", time_buf);
  
  page.replace("##APIKEY##", String(settings.weatherApiKey));
  page.replace("##CITY##", String(settings.weatherCity));
  
  page.replace("##RELAY_TIMER_CHECKED##", settings.relayTimerEnabled ? "checked" : "");
  sprintf(time_buf, "%02d:%02d", settings.relayOnHour, settings.relayOnMinute);
  page.replace("##RELAY_ON_TIME##", time_buf);
  sprintf(time_buf, "%02d:%02d", settings.relayOffHour, settings.relayOffMinute);
  page.replace("##RELAY_OFF_TIME##", time_buf);

  page.replace("##TEMP_CTRL_CHECKED##", settings.tempCtrlEnabled ? "checked" : "");
  page.replace("##TEMP_THRESHOLD##", String(settings.tempThreshold, 1));
  page.replace("##TEMP_THRESHOLD_OFF##", String(settings.tempThresholdOff, 1));

  page.replace("##DHT_TEMP##", isnan(dhtTemp) ? "--" : String(dhtTemp, 1));
  page.replace("##DHT_HUM##", isnan(dhtHum) ? "--" : String(dhtHum, 1));
  page.replace("##RELAY_STATUS##", relayState ? "开启 (ON)" : "关闭 (OFF)");
  page.replace("##RELAY_COLOR##", relayState ? "#3fb950" : "#da3633");

  auto toHex = [&](unsigned long val) { String s = String(val, HEX); s.toUpperCase(); return s; };
  page.replace("##IR_ON##", toHex(settings.ir_on));
  page.replace("##IR_OFF##", toHex(settings.ir_off));
  page.replace("##IR_UP##", toHex(settings.ir_bright_up));
  page.replace("##IR_DOWN##", toHex(settings.ir_bright_down));

  server.send(200, "text/html; charset=UTF-8", page);
}

void handleSaveINA() {
  if (server.hasArg("tonv")) settings.turnOnVoltage = server.arg("tonv").toFloat();
  if (server.hasArg("uv"))   settings.underVoltage  = server.arg("uv").toFloat();
  if (server.hasArg("upw"))  settings.underPower    = server.arg("upw").toFloat();
  if (server.hasArg("cds"))  settings.cooldownSec   = server.arg("cds").toInt();

  saveSettings();
  addLog("INA219 电源保护参数已保存。");
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSettings() {
  if (server.hasArg("sleep")) {
    String sleepTime = server.arg("sleep");
    settings.sleepHour = sleepTime.substring(0, 2).toInt();
    settings.sleepMinute = sleepTime.substring(3, 5).toInt();
  }
  if (server.hasArg("wake")) {
    String wakeTime = server.arg("wake");
    settings.wakeHour = wakeTime.substring(0, 2).toInt();
    settings.wakeMinute = wakeTime.substring(3, 5).toInt();
  }
  settings.relayTimerEnabled = server.hasArg("relay_timer_en");
  if (server.hasArg("relay_on")) {
    String rOn = server.arg("relay_on");
    settings.relayOnHour = rOn.substring(0, 2).toInt();
    settings.relayOnMinute = rOn.substring(3, 5).toInt();
  }
  if (server.hasArg("relay_off")) {
    String rOff = server.arg("relay_off");
    settings.relayOffHour = rOff.substring(0, 2).toInt();
    settings.relayOffMinute = rOff.substring(3, 5).toInt();
  }
  if (server.hasArg("city")) strncpy(settings.weatherCity, server.arg("city").c_str(), sizeof(settings.weatherCity) - 1);
  if (server.hasArg("apikey")) strncpy(settings.weatherApiKey, server.arg("apikey").c_str(), sizeof(settings.weatherApiKey) - 1);
  
  if (server.hasArg("ha_host")) strncpy(settings.haHost, server.arg("ha_host").c_str(), sizeof(settings.haHost) - 1);
  if (server.hasArg("ha_port")) settings.haPort = server.arg("ha_port").toInt();
  if (server.hasArg("ha_entity")) strncpy(settings.haEntity, server.arg("ha_entity").c_str(), sizeof(settings.haEntity) - 1);
  if (server.hasArg("ha_token")) strncpy(settings.haToken, server.arg("ha_token").c_str(), sizeof(settings.haToken) - 1);

  settings.tempCtrlEnabled = server.hasArg("temp_ctrl");
  if (server.hasArg("temp_threshold")) settings.tempThreshold = server.arg("temp_threshold").toFloat();
  if (server.hasArg("temp_threshold_off")) settings.tempThresholdOff = server.arg("temp_threshold_off").toFloat();

  saveSettings();
  updateWeather(); 
  addLog("基础与 HA 参数已保存更新！");
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSaveIR() {
  auto fromHex = [&](const char* n) { return strtoul(server.arg(n).c_str(), NULL, 16); };
  if (server.hasArg("ir_on")) settings.ir_on = fromHex("ir_on");
  if (server.hasArg("ir_off")) settings.ir_off = fromHex("ir_off");
  if (server.hasArg("ir_up")) settings.ir_bright_up = fromHex("ir_up");
  if (server.hasArg("ir_down")) settings.ir_bright_down = fromHex("ir_down");
  saveSettings();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleLogs() {
  String h = "<html><head><meta charset='UTF-8'><style>body{font-family:monospace;background:#000;color:#0f0;}</style></head><body><h2>系统日志</h2><ul>"; 
  int c = logBufferFull ? MAX_LOG_ENTRIES : currentLogIndex; 
  for (int i=0; i<c; i++) {
    int idx = (currentLogIndex - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    h += "<li>" + logBuffer[idx].timestamp + ": " + logBuffer[idx].message + "</li>";
  }
  h += "</ul><a href='/'>返回控制台</a></body></html>";
  server.send(200, "text/html; charset=UTF-8", h);
}

void handleTouch() {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();

    // 1. 严格过滤伪触摸（杂波时直接返回，不刷新活跃时间，不中断屏保/轮播）
    if (p.z < 400 || p.z > 3800 || p.x < 100 || p.x > 3900 || p.y < 100 || p.y > 3900) {
      return; 
    }

    // 2. 防抖
    static unsigned long lastTouchDebounce = 0;
    if (millis() - lastTouchDebounce < 400) return; 
    lastTouchDebounce = millis(); 

    // 3. 只有确认是人手真触摸，才更新活跃时间
    lastActivityTime = millis();

    if (isInStandby) {
      exitStandby(false); 
      return;
    }

    if (currentScreen != SCREEN_CONTROL) {
      pauseRotation = true;
      currentScreen = SCREEN_CONTROL; 
      drawCurrentScreen(true); 
      return;
    }

    int sx = map(p.y, 295, 3750, 0, 240); 
    int sy = map(p.x, 358, 3810, 0, 320);

    if (sx > 10 && sx < 230) {
      if (sy > 35 && sy < 90) {
        haDeviceState = !haDeviceState;
        controlHA(haDeviceState);
      } 
      else if (sy > 100 && sy < 155) {
        httpDeviceState = !httpDeviceState;
        controlHttp(httpDeviceState);
      } 
      else if (sy > 165 && sy < 220) {
          if (relayState) {
              manualSafeOff();
          } else {
              forceOnSystem();
          }
      }

      else if (sy > 230 && sy < 285) {
        irLightState = !irLightState;
        irsend.sendNEC(irLightState ? settings.ir_on : settings.ir_off);
        addLog("红外灯光控制发码: " + String(irLightState ? "ON" : "OFF"));
      }
      
      drawControlScreen(); 
    }
  }
}

void loadSettings() {
  if (!LittleFS.exists(configFile)) { saveSettings(); return; }
  File file = LittleFS.open(configFile, "r");
  if (!file) return;
  StaticJsonDocument<1024> doc;
  if (!deserializeJson(doc, file)) {
    settings.sleepHour = doc["sleepHour"] | 22; settings.sleepMinute = doc["sleepMinute"] | 0;
    settings.wakeHour = doc["wakeHour"] | 6; settings.wakeMinute = doc["wakeMinute"] | 0;
    settings.ir_on = doc["ir_on"] | DEFAULT_CODE_ON; settings.ir_off = doc["ir_off"] | DEFAULT_CODE_OFF;
    strlcpy(settings.weatherCity, doc["weatherCity"] | "zhumadian", sizeof(settings.weatherCity));
    strlcpy(settings.weatherApiKey, doc["weatherApiKey"] | "", sizeof(settings.weatherApiKey));
    
    strlcpy(settings.haHost, doc["haHost"] | "192.168.31.22", sizeof(settings.haHost));
    settings.haPort = doc["haPort"] | 8123;
    strlcpy(settings.haEntity, doc["haEntity"] | "switch.sonoff_1000a68f48", sizeof(settings.haEntity));
    strlcpy(settings.haToken, doc["haToken"] | "", sizeof(settings.haToken));

    settings.tempCtrlEnabled = doc["tempCtrlEnabled"] | false;
    settings.tempThreshold = doc["tempThreshold"] | 28.0; settings.tempThresholdOff = doc["tempThresholdOff"] | 27.0; 
    settings.relayTimerEnabled = doc["relayTimerEnabled"] | false;
    settings.turnOnVoltage = doc["turnOnVoltage"] | 13.5f;
    settings.underVoltage  = doc["underVoltage"]  | 11.5f;
    settings.underPower    = doc["underPower"]    | 2.0f;
    settings.cooldownSec   = doc["cooldownSec"]   | 3600UL;
  }
  file.close();
}

void saveSettings() {
  File file = LittleFS.open(configFile, "w");
  if (!file) return;
  StaticJsonDocument<1024> doc;
  doc["sleepHour"] = settings.sleepHour; doc["sleepMinute"] = settings.sleepMinute;
  doc["wakeHour"] = settings.wakeHour; doc["wakeMinute"] = settings.wakeMinute;
  doc["ir_on"] = settings.ir_on; doc["ir_off"] = settings.ir_off;
  doc["weatherCity"] = settings.weatherCity; doc["weatherApiKey"] = settings.weatherApiKey;
  
  doc["haHost"] = settings.haHost;
  doc["haPort"] = settings.haPort;
  doc["haEntity"] = settings.haEntity;
  doc["haToken"] = settings.haToken;

  doc["tempCtrlEnabled"] = settings.tempCtrlEnabled;
  doc["tempThreshold"] = settings.tempThreshold; doc["tempThresholdOff"] = settings.tempThresholdOff;
  doc["relayTimerEnabled"] = settings.relayTimerEnabled;
  doc["turnOnVoltage"] = settings.turnOnVoltage; doc["underVoltage"] = settings.underVoltage;
  doc["underPower"] = settings.underPower; doc["cooldownSec"] = settings.cooldownSec;
  serializeJson(doc, file); file.close();
}

#endif // NETWORK_WEB_H