#ifndef GUI_H
#define GUI_H

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <NTPClient.h>  
#include "config.h"

extern Adafruit_ILI9341 tft;
extern XPT2046_Touchscreen ts;
extern Settings settings;
extern SystemState st;
extern NTPClient timeClient; 

extern bool haDeviceState;
extern bool httpDeviceState;
extern bool irLightState;
extern bool relayState;
extern bool isInStandby;

extern float dhtTemp;
extern float dhtHum;

extern ScreenMode currentScreen;
extern unsigned long lastScreenSwitchTime;
extern bool pauseRotation; 

extern String weather_main;
extern String weather_temp;
extern String weather_desc;

extern int lastMinute;
extern int lastSecond;
extern int lastDay;

extern void updateStatusLine();
extern long getCooldownRemaining();
void updateClockTime();

void drawGridBackground() {
  for (int i = 0; i < 320; i += 40) {
    tft.drawFastHLine(0, i, 240, C_GRID);
  }
  for (int i = 0; i < 240; i += 40) {
    tft.drawFastVLine(i, 0, 320, C_GRID);
  }
}

void drawCyberFrame(int x, int y, int w, int h, uint16_t color, String label) {
  tft.drawRect(x, y, w, h, color);
  int len = 8;
  tft.fillRect(x, y, len, 2, color);
  tft.fillRect(x, y, 2, len, color); 
  tft.fillRect(x + w - len, y, len, 2, color);
  tft.fillRect(x + w - 2, y, 2, len, color); 
  tft.fillRect(x, y + h - 2, len, 2, color);
  tft.fillRect(x, y + h - len, 2, len, color); 
  tft.fillRect(x + w - len, y + h - 2, len, 2, color);
  tft.fillRect(x + w - 2, y + h - len, 2, len, color); 
  
  if (label.length() > 0) {
    tft.setTextSize(1);
    int16_t x1, y1;
    uint16_t text_w, text_h;
    tft.getTextBounds(label, 0, 0, &x1, &y1, &text_w, &text_h);
    tft.fillRect(x + 10, y - (text_h / 2) - 2, text_w + 10, text_h + 4, C_BG);
    tft.setTextColor(color);
    tft.setCursor(x + 15, y - (text_h / 2));
    tft.print(label);
  }
}

void drawControlScreen() {
  tft.fillScreen(C_BG);
  drawGridBackground();
  
  tft.fillRect(0, 0, 240, 24, C_PURPLE);
  tft.setTextColor(C_BG);
  tft.setTextSize(2);
  tft.setCursor(35, 5);
  tft.print("[SYSTEM CONTROL]");

  auto drawButton = [&](int y, uint16_t color, const char* label, bool state) {
    tft.fillRect(10, y, 220, 55, C_DARK_GREY);
    tft.drawRect(10, y, 220, 55, color);
    drawCyberFrame(10, y, 220, 55, color, "");
    
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(25, y + 20);
    tft.print(label);
    
    uint16_t stateColor = state ? C_GREEN : C_RED;
    const char* stateText = state ? "ON" : "OFF";
    tft.fillRect(160, y + 10, 60, 35, stateColor);
    tft.setTextColor(C_BG);
    tft.setTextSize(2);
    tft.setCursor(170, y + 20);
    tft.print(stateText);
  };

  drawButton(35, C_GREEN, "HASSIST", haDeviceState);
  drawButton(100, C_CYAN, "HTTP", httpDeviceState);
  drawButton(165, C_YELLOW, "RELAY", relayState);
  drawButton(230, C_PURPLE, "IR", irLightState);
}

void drawWeatherScreen() {
  tft.fillScreen(C_BG);
  drawGridBackground();
  
  tft.fillRect(0, 0, 240, 24, C_GREEN);
  tft.setTextColor(C_BG);
  tft.setTextSize(2);
  tft.setCursor(5, 5);
  tft.print("[ENV SCAN]");

  tft.setTextColor(C_DARK_GREY);
  tft.setTextSize(1);
  tft.setCursor(130, 9);
  tft.print(settings.weatherCity);
  
  // 室外温度 - 7 号巨型大字
  tft.setTextColor(C_WHITE);
  tft.setTextSize(7); 
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(weather_temp, 0, 0, &x1, &y1, &w, &h);
  
  int startX = 110 - w / 2;
  tft.setCursor(startX, 35);
  tft.print(weather_temp);

  tft.setTextSize(3);
  tft.drawCircle(startX + w + 12, 42, 5, C_WHITE);
  tft.setCursor(startX + w + 22, 45);
  tft.print("C");

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.getTextBounds(weather_desc, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(120 - w/2, 98);
  tft.print(weather_desc);

  drawCyberFrame(10, 185, 220, 110, C_GREEN, "INDOOR CLIMATE");
  
  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  
  tft.setCursor(25, 210);
  tft.print("TEMP: ");
  if (isnan(dhtTemp)) tft.print("-- C");
  else { tft.print(dhtTemp, 1); tft.print(" C"); }

  tft.setCursor(25, 240);
  tft.print("HUMI: ");
  if (isnan(dhtHum)) tft.print("-- %");
  else { tft.print(dhtHum, 1); tft.print(" %"); }

  tft.setTextSize(1);
  tft.setTextColor(C_CYAN);
  tft.setCursor(25, 275);
  tft.print("RELAY: ");
  tft.print(relayState ? "ACTIVE" : "INACTIVE");
}

void drawClockScreen(bool isInitialDraw) {
    if (!isInitialDraw) return;
    
    tft.fillScreen(C_BG);
    drawGridBackground();
  
    tft.fillRect(0, 0, 240, 24, C_CYAN);
    tft.setTextColor(C_BG);
    tft.setTextSize(2);
    tft.setCursor(20, 5);
    tft.print("[TIME & POWER Core]");

    drawCyberFrame(10, 160, 220, 100, C_YELLOW, "INA219 POWER MONITOR");
    drawCyberFrame(20, 270, 200, 25, C_ORANGE, "");
    
    lastMinute = -1;
    lastSecond = -1;
    lastDay = -1;
    updateClockTime();
}

void updateClockTime() {
    time_t rawTime = timeClient.getEpochTime();
    struct tm * ti = localtime(&rawTime);
    
    if (ti->tm_mday != lastDay) {
        lastDay = ti->tm_mday;
        char dateBuf[20];
        sprintf(dateBuf, "%04d.%02d.%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
        tft.fillRect(10, 30, 220, 20, C_BG);
        tft.setTextColor(C_GREEN);
        tft.setTextSize(2);
        tft.setCursor(60, 32);
        tft.print(dateBuf);
    }
    
    if (ti->tm_min != lastMinute) {
        lastMinute = ti->tm_min;
        char timeBuf[6];
        sprintf(timeBuf, "%02d:%02d", ti->tm_hour, ti->tm_min);
        tft.fillRect(10, 55, 220, 50, C_BG);
        tft.setTextColor(C_WHITE);
        tft.setTextSize(6);
        tft.setCursor(30, 60);
        tft.print(timeBuf);
    }

    if (ti->tm_sec != lastSecond) {
        lastSecond = ti->tm_sec;
        int barWidth = map(ti->tm_sec, 0, 59, 0, 196);
        tft.fillRect(22, 272, 196, 21, C_DARK_GREY);
        tft.fillRect(22, 272, barWidth, 21, C_ORANGE);
        
        char secBuf[3];
        sprintf(secBuf, "%02d", ti->tm_sec);
        tft.fillRect(105, 120, 30, 16, C_BG);
        tft.setTextSize(2);
        tft.setTextColor(C_ORANGE);
        tft.setCursor(105, 120);
        tft.print(secBuf);

        tft.fillRect(15, 172, 210, 83, C_BG); 

        tft.setTextSize(3);
        tft.setTextColor(C_CYAN);
        tft.setCursor(20, 175);
        tft.print("V:");
        tft.print(st.busVoltage, 2);
        tft.print("V");

        tft.setTextSize(2);
        tft.setTextColor(C_WHITE);
        tft.setCursor(20, 210);
        tft.print("I:");
        tft.print(st.current_mA / 1000.f, 1);
        tft.print("A");

        tft.setCursor(125, 210); 
        tft.print("P:");
        tft.print(st.power_mW / 1000.f, 1);
        tft.print("W");

        tft.setTextSize(1);
        tft.setCursor(20, 238);
        unsigned long nowMs = millis();
        
        if (nowMs < 60000UL) {
            long remWarm = (60000UL - nowMs) / 1000UL;
            tft.setTextColor(C_ORANGE);
            tft.print("STATUS: WARMUP (");
            tft.print(remWarm);
            tft.print("s LEFT)");
        } else if (st.relayOn) {
            tft.setTextColor(C_GREEN);
            tft.print("STATUS: RUNNING (ON)");
        } else {
            long rem = getCooldownRemaining();
            if (rem > 0) {
                tft.setTextColor(C_RED);
                tft.print("LOCKOUT: "); tft.print(rem); tft.print("s LEFT");
            } else {
                tft.setTextColor(C_YELLOW);
                tft.print("STATUS: STANDBY (OFF)");
            }
        }
    }
}

void drawCurrentScreen(bool forceRedraw) {
    if (isInStandby) return;
    
    switch(currentScreen) {
        case SCREEN_CONTROL: drawControlScreen(); break;
        case SCREEN_WEATHER: drawWeatherScreen(); break;
        case SCREEN_CLOCK:   drawClockScreen(true); break;
    }
    
    updateStatusLine(); // 统一重绘底部状态栏与按键提示
}

#endif // GUI_H