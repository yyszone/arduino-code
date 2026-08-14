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
extern long getCooldownRemaining();

void handleRoot();
void handleApiStatus();
void handleSettings();
void handleSaveINA();
void handleSaveIR();
void handleIrCommand();
void handleLogs();
void controlHA(bool state);
void controlHttp(bool state);

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

// ════════════ 状态栏更新：去时间，加按键提示 ════════════
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
  
  // 右下角去除时间，改为进入控制台的醒目提示
  tft.setTextColor(C_ORANGE);
  tft.setCursor(150, 308);
  tft.print("[TOUCH: MENU]");
}

void enterStandby() {
  if (isInStandby) return;
  isInStandby = true;
  addLog("屏幕待机：进入省电模式。");
  digitalWrite(TFT_BL, LOW);
  tft.fillScreen(ILI9341_BLACK);
  tft.writeCommand(ILI9341_SLPIN);
  
  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void setupWifiAndServices(bool wifiAlreadyConnected);

void exitStandby(bool wifiAlreadyConnected) {
  digitalWrite(TFT_BL, HIGH);
  tft.begin(); 
  tft.setRotation(0);
  tft.writeCommand(ILI9341_SLPOUT);
  
  isInStandby = false;
  lastActivityTime = millis();
  addLog("屏幕唤醒，恢复服务。");
  setupWifiAndServices(wifiAlreadyConnected);
}

void setupWifiAndServices(bool wifiAlreadyConnected) {
  if (!wifiAlreadyConnected) {
    tft.fillScreen(C_BG);
    tft.setTextColor(C_GREEN);
    tft.setTextSize(2); 
    tft.setCursor(10, 100);
    tft.print("CONNECTING NETWORK...");
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 15) {
      delay(500);
      retry++;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    addLog("WiFi 已连接, IP: " + WiFi.localIP().toString());
    timeClient.begin();
    timeClient.update();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/save_ina", HTTP_POST, handleSaveINA);
    server.on("/save_ir", HTTP_POST, handleSaveIR);
    server.on("/ir", HTTP_GET, handleIrCommand);
    server.on("/on", HTTP_GET, [](){ forceOnSystem(); server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });
    server.on("/off", HTTP_GET, [](){ executeTrip(TripReason::MANUAL); server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });
    server.on("/reset", HTTP_GET, [](){ resetSystem(); server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });
    server.on("/logs", HTTP_GET, handleLogs);

    httpUpdater.setup(&server);
    server.begin();
    updateWeather(); 
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

void handleApiStatus() {
  unsigned long nowMs = millis();
  String statusHtml = "";
  
  if (nowMs < 60000UL) {
    long remWarm = (60000UL - nowMs) / 1000UL;
    statusHtml = "<span style='color:#f0883e;font-weight:bold;'>⏳ 系统开机预热中（剩余 " + String(remWarm) + " 秒）</span>";
  } else if (st.relayOn) {
    statusHtml = "<span style='color:#3fb950;font-weight:bold;'>✔ 继电器吸合（正常运行）</span>";
  } else {
    long rem = getCooldownRemaining();
    if (rem > 0) {
      statusHtml = "<span style='color:#d29922;font-weight:bold;'>⌛ 保护待机锁定中（剩余冷却 " + String(rem) + " 秒）</span>";
    } else {
      statusHtml = "<span style='color:#da3633;font-weight:bold;'>⛔ 待机/断开状态</span>";
    }
  }

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

  unsigned long nowMs = millis();
  String statusHtml = "";
  if (nowMs < 60000UL) {
    long remWarm = (60000UL - nowMs) / 1000UL;
    statusHtml = "<span style='color:#f0883e;font-weight:bold;'>⏳ 系统开机预热中（剩余 " + String(remWarm) + " 秒）</span>";
  } else if (st.relayOn) {
    statusHtml = "<span style='color:#3fb950;font-weight:bold;'>✔ 继电器吸合（正常运行）</span>";
  } else {
    long rem = getCooldownRemaining();
    if (rem > 0) statusHtml = "<span style='color:#d29922;font-weight:bold;'>⌛ 保护待机锁定中（剩余冷却 " + String(rem) + " 秒）</span>";
    else statusHtml = "<span style='color:#da3633;font-weight:bold;'>⛔ 待机/断开状态</span>";
  }
  page.replace("##PROTECT_STATUS##", statusHtml);

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

// ════════════ 触摸逻辑：任意位置点击进入控制台 ════════════
void handleTouch() {
  if (ts.touched()) {
    static unsigned long lastTouchDebounce = 0;
    if (millis() - lastTouchDebounce < 300) return; 
    lastTouchDebounce = millis(); 
    lastActivityTime = millis();

    if (isInStandby) {
      exitStandby(false); 
      return;
    }

    // ⭐️ 核心点：若在天气或时钟页，点击屏幕任意地方直接进入电器控制页
    if (currentScreen != SCREEN_CONTROL) {
      pauseRotation = true;
      currentScreen = SCREEN_CONTROL; 
      drawCurrentScreen(true); 
      return;
    }

    // 在控制界面下，响应 4 按键
    TS_Point p = ts.getPoint();
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
        if (relayState) executeTrip(TripReason::MANUAL);
        else forceOnSystem();
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