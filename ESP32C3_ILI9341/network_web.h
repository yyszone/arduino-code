#ifndef NETWORK_WEB_H
#define NETWORK_WEB_H

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <NTPClient.h>
#include <IRsend.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "config.h"

// 引用外部全局对象
extern Settings settings;
extern WebServer server;
extern NTPClient timeClient;
extern IRsend irsend;
extern HTTPUpdateServer httpUpdater;

extern bool haDeviceState;
extern bool httpDeviceState;
extern bool irLightState;
extern bool relayState;
extern bool isInStandby;
extern bool lastInSleepWindow;
extern bool firstTimeSyncDone;

extern float dhtTemp;
extern float dhtHum;

extern unsigned long lastActivityTime;
extern unsigned long lastStatusUpdate;
extern unsigned long lastWakeupCheck;

extern ScreenMode currentScreen;
extern unsigned long lastScreenSwitchTime;
extern bool pauseRotation; 

extern String weather_main;
extern String weather_temp;
extern String weather_desc;
extern unsigned long lastWeatherUpdate;
extern const unsigned long WEATHER_UPDATE_INTERVAL;

extern LogEntry logBuffer[];
extern int currentLogIndex;
extern bool logBufferFull;
extern const int MAX_LOG_ENTRIES;

// 外部硬件方法的前置声明
extern void setRelay(bool state);
extern void updateRelayLogic();
extern void drawWeatherScreen();
extern void drawControlScreen();
extern void drawCurrentScreen(bool forceRedraw);

// ==================== 所有网络及后台具体实现代码体 ====================

void updateWeather() {
  if (strlen(settings.weatherApiKey) < 10 || strlen(settings.weatherCity) == 0) {
    addLog("天气 API Key 或城市未设置。");
    return;
  }
  
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
        if (currentScreen == SCREEN_WEATHER) {
          drawWeatherScreen();
        }
    } else {
        addLog("天气更新失败, 错误码: " + String(httpCode));
    }
    http.end();
  } else {
      addLog("天气 HTTP 连接失败。");
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
  String t = timeClient.getFormattedTime().substring(0, 5);
  tft.setTextColor(C_CYAN);
  tft.setCursor(205, 308);
  tft.print(t);
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

// 【修复点】：在此处补回之前遗漏的 exitStandby 函数实体
void exitStandby(bool wifiAlreadyConnected) {
  digitalWrite(TFT_BL, HIGH);
  tft.begin(); 
  tft.setRotation(0);
  tft.writeCommand(ILI9341_SLPOUT);
  
  isInStandby = false;
  lastActivityTime = millis();
  addLog("屏幕唤醒，重新加载服务。");
  
  setupWifiAndServices(wifiAlreadyConnected);
}

void setupWifiAndServices(bool wifiAlreadyConnected) {
  if (!wifiAlreadyConnected) {
    tft.fillScreen(C_BG);
    drawGridBackground();
    tft.setTextColor(C_GREEN);
    tft.setTextSize(2); 
    tft.setCursor(10, 100);
    tft.print("CONNECTING NETWORK...");
    tft.drawRect(10, 130, 220, 20, C_GREEN);
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
      delay(500);
      tft.fillRect(12, 132, retry * 10.9, 16, C_GREEN);
      retry++;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiAlreadyConnected) { 
        tft.setCursor(10, 160);
        tft.print("WIFI CONNECTED!");
    }
    addLog("WiFi 已连接, IP: " + WiFi.localIP().toString());
    timeClient.begin();
    timeClient.update();
    server.on("/", HTTP_GET, handleRoot);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/save_ir", HTTP_POST, handleSaveIR);
    server.on("/ir", HTTP_GET, handleIrCommand);
    server.on("/relay", HTTP_GET, handleRelayCommand); 
    server.on("/logs", HTTP_GET, handleLogs);
    httpUpdater.setup(&server);
    server.begin();
    ArduinoOTA.begin();
    updateWeather(); 
  } else {
    addLog("WiFi 连接失败。");
  }
  
  delay(1000);
  drawCurrentScreen(true);
}

void handleRoot() {
  String page = FPSTR(MAIN_HTML);
  char time_buf[6];
  sprintf(time_buf, "%02d:%02d", settings.sleepHour, settings.sleepMinute);
  page.replace("##SLEEP_TIME##", time_buf);
  sprintf(time_buf, "%02d:%02d", settings.wakeHour, settings.wakeMinute);
  page.replace("##WAKE_TIME##", time_buf);
  
  page.replace("##APIKEY##", String(settings.weatherApiKey));
  page.replace("##CITY##", String(settings.weatherCity));
  
  String schedule_str = String(settings.sleepHour) + ":" + (settings.sleepMinute < 10 ? "0" : "") + String(settings.sleepMinute) + 
                       " &rarr; " + 
                       String(settings.wakeHour) + ":" + (settings.wakeMinute < 10 ? "0" : "") + String(settings.wakeMinute);
  page.replace("##CURRENT_SCHEDULE##", schedule_str);

  // 渲染新：继电器专用定时参数
  page.replace("##RELAY_TIMER_CHECKED##", settings.relayTimerEnabled ? "checked" : "");
  char r_time_buf[6];
  sprintf(r_time_buf, "%02d:%02d", settings.relayOnHour, settings.relayOnMinute);
  page.replace("##RELAY_ON_TIME##", r_time_buf);
  sprintf(r_time_buf, "%02d:%02d", settings.relayOffHour, settings.relayOffMinute);
  page.replace("##RELAY_OFF_TIME##", r_time_buf);

  String r_schedule_str = "";
  if (settings.relayTimerEnabled) {
    char r_sched_buf[32];
    sprintf(r_sched_buf, "%02d:%02d &rarr; %02d:%02d", 
            settings.relayOnHour, settings.relayOnMinute,
            settings.relayOffHour, settings.relayOffMinute);
    r_schedule_str = String(r_sched_buf);
  } else {
    r_schedule_str = "未启用定时";
  }
  page.replace("##RELAY_SCHEDULE##", r_schedule_str);

  auto toHex = [&](unsigned long val) { String s = String(val, HEX); s.toUpperCase(); return s; };
  page.replace("##IR_ON##", toHex(settings.ir_on));
  page.replace("##IR_OFF##", toHex(settings.ir_off));
  page.replace("##IR_UP##", toHex(settings.ir_bright_up));
  page.replace("##IR_DOWN##", toHex(settings.ir_bright_down));
  page.replace("##IR_AUTO##", toHex(settings.ir_auto));
  page.replace("##IR_3H##", toHex(settings.ir_timer_3h));
  page.replace("##IR_5H##", toHex(settings.ir_timer_5h));
  page.replace("##IR_8H##", toHex(settings.ir_timer_8h));
  
  page.replace("##TEMP_CTRL_CHECKED##", settings.tempCtrlEnabled ? "checked" : "");
  page.replace("##TEMP_THRESHOLD##", String(settings.tempThreshold, 1));
  page.replace("##TEMP_THRESHOLD_OFF##", String(settings.tempThresholdOff, 1));
  
  page.replace("##DHT_TEMP##", isnan(dhtTemp) ? "--" : String(dhtTemp, 1));
  page.replace("##DHT_HUM##", isnan(dhtHum) ? "--" : String(dhtHum, 1));
  page.replace("##RELAY_STATUS##", relayState ? "开启 (ON)" : "关闭 (OFF)");
  page.replace("##RELAY_COLOR##", relayState ? "#3fb950" : "#da3633");

  server.send(200, "text/html; charset=UTF-8", page);
}

void handleSettings() {
  if (server.hasArg("sleep")) {
    String sleepTime = server.arg("sleep");
    settings.sleepHour = sleepTime.substring(0, 2).toInt();
    settings.sleepMinute = sleepTime.substring(3, 5).toInt();
    addLog("屏幕夜间定时已更新。");
  }
  
  if (server.hasArg("wake")) {
    String wakeTime = server.arg("wake");
    settings.wakeHour = wakeTime.substring(0, 2).toInt();
    settings.wakeMinute = wakeTime.substring(3, 5).toInt();
  }

  // 解析新：继电器定时开关及时间范围
  if (server.hasArg("relay_timer_en")) {
    settings.relayTimerEnabled = true;
  } else {
    settings.relayTimerEnabled = false;
  }

  if (server.hasArg("relay_on")) {
    String rOn = server.arg("relay_on");
    settings.relayOnHour = rOn.substring(0, 2).toInt();
    settings.relayOnMinute = rOn.substring(3, 5).toInt();
    addLog("继电器定时开启时间已调整。");
  }

  if (server.hasArg("relay_off")) {
    String rOff = server.arg("relay_off");
    settings.relayOffHour = rOff.substring(0, 2).toInt();
    settings.relayOffMinute = rOff.substring(3, 5).toInt();
    addLog("继电器定时关闭时间已调整。");
  }

  if(server.hasArg("city")) {
    strncpy(settings.weatherCity, server.arg("city").c_str(), sizeof(settings.weatherCity) - 1);
    addLog("城市已更新。");
  }

  if(server.hasArg("apikey")) {
    strncpy(settings.weatherApiKey, server.arg("apikey").c_str(), sizeof(settings.weatherApiKey) - 1);
    addLog("API Key已更新。");
  }

  if (server.hasArg("temp_ctrl")) {
    settings.tempCtrlEnabled = true;
  } else {
    settings.tempCtrlEnabled = false;
  }

  if (server.hasArg("temp_threshold")) {
    settings.tempThreshold = server.arg("temp_threshold").toFloat();
    addLog("温度控制开启阈值已调整。");
  }

  if (server.hasArg("temp_threshold_off")) {
    settings.tempThresholdOff = server.arg("temp_threshold_off").toFloat();
    addLog("温度控制关闭阈值已调整。");
  }
  
  firstTimeSyncDone = false; 
  
  saveSettings();
  updateWeather(); 
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSaveIR() {
  auto fromHex = [&](const char* n) { return strtoul(server.arg(n).c_str(), NULL, 16); };
  if (server.hasArg("ir_on")) settings.ir_on = fromHex("ir_on");
  if (server.hasArg("ir_off")) settings.ir_off = fromHex("ir_off");
  if (server.hasArg("ir_up")) settings.ir_bright_up = fromHex("ir_up");
  if (server.hasArg("ir_down")) settings.ir_bright_down = fromHex("ir_down");
  if (server.hasArg("ir_auto")) settings.ir_auto = fromHex("ir_auto");
  if (server.hasArg("ir_3h")) settings.ir_timer_3h = fromHex("ir_3h");
  if (server.hasArg("ir_5h")) settings.ir_timer_5h = fromHex("ir_5h");
  if (server.hasArg("ir_8h")) settings.ir_timer_8h = fromHex("ir_8h");
  
  saveSettings();
  addLog("红外编码已更新");
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleIrCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String cmd = server.arg("cmd");
  unsigned long code_to_send = 0;
  
  if(cmd.equals("on")) { code_to_send = settings.ir_on; irLightState = true; }
  else if(cmd.equals("off")) { code_to_send = settings.ir_off; irLightState = false; }
  else if(cmd.equals("bright_up")) code_to_send = settings.ir_bright_up;
  else if(cmd.equals("bright_down")) code_to_send = settings.ir_bright_down;
  else if(cmd.equals("auto")) code_to_send = settings.ir_auto;
  else if(cmd.equals("timer_3h")) code_to_send = settings.ir_timer_3h;
  else if(cmd.equals("timer_5h")) code_to_send = settings.ir_timer_5h;
  else if(cmd.equals("timer_8h")) code_to_send = settings.ir_timer_8h;
  else {
    server.send(404, "text/plain", "Command not found.");
    return;
  }
  
  addLog("发送 IR 请求. 指令: " + cmd);
  irsend.sendNEC(code_to_send);
  
  if (currentScreen == SCREEN_CONTROL) {
    drawControlScreen();
  }
   
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", ""); 
}

void handleRelayCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String cmd = server.arg("cmd");
  if (cmd.equals("on")) {
    setRelay(true);
    addLog("网页控制：开启继电器");
  } else if (cmd.equals("off")) {
    setRelay(false);
    addLog("网页控制：关闭继电器");
  }
  
  if (currentScreen == SCREEN_CONTROL && !isInStandby) {
    drawControlScreen();
  }
  
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void controlHttp(bool state) {
  if (isInStandby) return;
  addLog("HTTP 控制: " + String(state ? "ON" : "OFF"));
  WiFiClient client; HTTPClient http;
  if (http.begin(client, state ? led_on_url : led_off_url)) { http.GET(); http.end(); }
}

void controlHA(bool state) {
  if (isInStandby) return;
  addLog("HA 控制: " + String(state ? "ON" : "OFF"));
  WiFiClient client; HTTPClient http;
  String url = "http://" + String(ha_host) + ":" + String(ha_port) + "/api/services/switch/" + (state ? "turn_on" : "turn_off");
  if (http.begin(client, url)) {
    http.addHeader("Authorization", "Bearer " + String(ha_token));
    http.addHeader("Content-Type", "application/json");
    http.POST("{\"entity_id\":\"" + String(ha_entity_id) + "\"}");
    http.end();
  }
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

void handleLogs() {
  String h = "<html><head><meta charset='UTF-8'><style>body{font-family:monospace;background:#000;color:#0f0;}</style></head><body><h2>系统日志</h2><ul>"; 
  int c = logBufferFull ? MAX_LOG_ENTRIES : currentLogIndex; 
  for (int i=0; i<c; i++) {
    int idx = (currentLogIndex - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    h += "<li>" + logBuffer[idx].timestamp + ": " + logBuffer[idx].message + "</li>";
  }
  h += "</ul><a href='/'>返回</a></body></html>";
  server.send(200, "text/html; charset=UTF-8", h);
}

void loadSettings() {
  File file = LittleFS.open(configFile, "r");
  if (!file) {
    addLog("配置文件未找到，加载默认设置。");
    settings.sleepHour = 22; settings.sleepMinute = 0; settings.wakeHour = 6; settings.wakeMinute = 0;
    settings.ir_on = DEFAULT_CODE_ON; settings.ir_off = DEFAULT_CODE_OFF; settings.ir_bright_up = DEFAULT_CODE_BRIGHT_UP;
    settings.ir_bright_down = DEFAULT_CODE_BRIGHT_DOWN; settings.ir_auto = DEFAULT_CODE_AUTO; settings.ir_timer_3h = DEFAULT_CODE_TIMER_3H;
    settings.ir_timer_5h = DEFAULT_CODE_TIMER_5H; settings.ir_timer_8h = DEFAULT_CODE_TIMER_8H;
    strcpy(settings.weatherCity, "zhumadian"); strcpy(settings.weatherApiKey, ""); 
    settings.tempCtrlEnabled = false; settings.tempThreshold = 28.0; settings.tempThresholdOff = 27.0;
    settings.relayTimerEnabled = false; settings.relayOnHour = 8; settings.relayOnMinute = 0; settings.relayOffHour = 22; settings.relayOffMinute = 0;
    settings.magic_key = 80101; saveSettings(); return;
  }
  StaticJsonDocument<1024> doc; deserializeJson(doc, file); file.close();
  if (doc["magic_key"] != 80101) { loadSettings(); } else {
    settings.sleepHour = doc["sleepHour"] | 22; settings.sleepMinute = doc["sleepMinute"] | 0;
    settings.wakeHour = doc["wakeHour"] | 6; settings.wakeMinute = doc["wakeMinute"] | 0;
    settings.ir_on = doc["ir_on"] | DEFAULT_CODE_ON; settings.ir_off = doc["ir_off"] | DEFAULT_CODE_OFF;
    settings.ir_bright_up = doc["ir_bright_up"] | DEFAULT_CODE_BRIGHT_UP; settings.ir_bright_down = doc["ir_bright_down"] | DEFAULT_CODE_BRIGHT_DOWN;
    settings.ir_auto = doc["ir_auto"] | DEFAULT_CODE_AUTO; settings.ir_timer_3h = doc["ir_timer_3h"] | DEFAULT_CODE_TIMER_3H;
    settings.ir_timer_5h = doc["ir_timer_5h"] | DEFAULT_CODE_TIMER_5H; settings.ir_timer_8h = doc["ir_timer_8h"] | DEFAULT_CODE_TIMER_8H;
    strlcpy(settings.weatherCity, doc["weatherCity"] | "zhumadian", sizeof(settings.weatherCity));
    strlcpy(settings.weatherApiKey, doc["weatherApiKey"] | "", sizeof(settings.weatherApiKey));
    settings.tempCtrlEnabled = doc["tempCtrlEnabled"].as<bool>();
    settings.tempThreshold = doc["tempThreshold"] | 28.0; settings.tempThresholdOff = doc["tempThresholdOff"] | 27.0; 
    settings.relayTimerEnabled = doc["relayTimerEnabled"] | false;
    settings.relayOnHour = doc["relayOnHour"] | 8; settings.relayOnMinute = doc["relayOnMinute"] | 0;
    settings.relayOffHour = doc["relayOffHour"] | 22; settings.relayOffMinute = doc["relayOffMinute"] | 0;
  }
}

void saveSettings() {
  File file = LittleFS.open(configFile, "w");
  if (!file) return;
  StaticJsonDocument<1024> doc;
  doc["sleepHour"] = settings.sleepHour; doc["sleepMinute"] = settings.sleepMinute;
  doc["wakeHour"] = settings.wakeHour; doc["wakeMinute"] = settings.wakeMinute;
  doc["ir_on"] = settings.ir_on; doc["ir_off"] = settings.ir_off;
  doc["ir_bright_up"] = settings.ir_bright_up; doc["ir_bright_down"] = settings.ir_bright_down;
  doc["ir_auto"] = settings.ir_auto; doc["ir_timer_3h"] = settings.ir_timer_3h;
  doc["ir_timer_5h"] = settings.ir_timer_5h; doc["ir_timer_8h"] = settings.ir_timer_8h;
  doc["weatherCity"] = settings.weatherCity; doc["weatherApiKey"] = settings.weatherApiKey;
  doc["tempCtrlEnabled"] = settings.tempCtrlEnabled;
  doc["tempThreshold"] = settings.tempThreshold; doc["tempThresholdOff"] = settings.tempThresholdOff;
  doc["relayTimerEnabled"] = settings.relayTimerEnabled;
  doc["relayOnHour"] = settings.relayOnHour; doc["relayOnMinute"] = settings.relayOnMinute;
  doc["relayOffHour"] = settings.relayOffHour; doc["relayOffMinute"] = settings.relayOffMinute;
  doc["magic_key"] = settings.magic_key;
  serializeJson(doc, file); file.close();
}

void handleTouch() {
  if (ts.touched()) {
    static unsigned long lastTouchDebounce = 0;
    if (millis() - lastTouchDebounce < 300) return;
    lastTouchDebounce = millis(); lastActivityTime = millis();
    if (isInStandby) {
      addLog("触摸唤醒，退出待机模式。");
      exitStandby(false); return; // 补回调用
    }
    pauseRotation = true;
    if (currentScreen != SCREEN_CONTROL) {
      currentScreen = SCREEN_CONTROL; drawCurrentScreen(); return;
    }
    TS_Point p = ts.getPoint();
    int sx = map(p.y, 295, 3750, 0, 240); int sy = map(p.x, 358, 3810, 0, 320);
    if (sx > 10 && sx < 230) {
      if (sy > 35 && sy < 90) { haDeviceState = !haDeviceState; controlHA(haDeviceState); }
      else if (sy > 100 && sy < 155) { httpDeviceState = !httpDeviceState; controlHttp(httpDeviceState); }
      else if (sy > 165 && sy < 220) { setRelay(!relayState); addLog("手动控制继电器: " + String(relayState ? "ON" : "OFF")); }
      else if (sy > 230 && sy < 285) { irLightState = !irLightState; irsend.sendNEC(irLightState ? settings.ir_on : settings.ir_off); }
      drawControlScreen();
    }
  }
}

#endif // NETWORK_WEB_H