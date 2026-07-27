#ifndef GUI_H
#define GUI_H

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <NTPClient.h>  
#include "config.h"

// ==================== 建立与主 ino 全局变量的一对一 extern 映射映射 ====================
extern Adafruit_ILI9341 tft;
extern XPT2046_Touchscreen ts;
extern Settings settings;
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

// 主程序中声明的、需要跨模块调用的辅助函数
extern void updateStatusLine();

// ==================== 所有绘图函数实现（彻底去除了 inline） ====================

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

void drawWeatherIcon(String weather, int x, int y) {
  weather.toLowerCase();
  if (weather.indexOf("rain") >= 0 || weather.indexOf("drizzle") >= 0) {
    tft.fillCircle(x, y - 5, 20, C_DARK_GREY);
    tft.fillCircle(x - 10, y + 5, 15, C_DARK_GREY);
    tft.fillCircle(x + 10, y + 5, 15, C_DARK_GREY);
    for (int i=0; i<3; i++) {
      tft.drawLine(x - 12 + i*12, y + 15, x - 17 + i*12, y + 30, C_CYAN);
    }
  } else if (weather.indexOf("snow") >= 0) {
    tft.fillCircle(x, y - 5, 20, C_DARK_GREY);
    for (int i=0; i<3; i++) {
      tft.drawCircle(x - 10 + i*10, y + 20, 2, C_WHITE);
    }
  } else if (weather.indexOf("cloud") >= 0) {
    tft.fillCircle(x, y - 5, 22, C_DARK_GREY);
    tft.fillCircle(x - 15, y + 5, 18, C_DARK_GREY);
    tft.fillCircle(x + 18, y + 5, 18, C_WHITE);
  } else if (weather.indexOf("clear") >= 0) {
    tft.fillCircle(x, y, 22, C_YELLOW);
    for (float i=0; i<360; i+= 45) {
      float r = i * 3.14159 / 180;
      tft.drawLine(x + 26*cos(r), y + 26*sin(r), x + 34*cos(r), y + 34*sin(r), C_ORANGE);
    }
  } else { 
    for (int i=0; i<3; i++) {
      tft.drawFastHLine(x-20, y-10+i*8, 40, C_DARK_GREY);
    }
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
  
  tft.setTextColor(C_WHITE);
  tft.setTextSize(5); 
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(weather_temp, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(120 - w/2, 35);
  tft.print(weather_temp);
  tft.setTextSize(2);
  tft.drawCircle(tft.getCursorX() + 8, 40, 4, C_WHITE);

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN);
  tft.getTextBounds(weather_desc, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(120 - w/2, 85);
  tft.print(weather_desc);

  drawWeatherIcon(weather_main, 120, 140);

  drawCyberFrame(10, 185, 220, 110, C_GREEN, "INDOOR CLIMATE");
  
  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  
  tft.setCursor(25, 210);
  tft.print("TEMP: ");
  if (isnan(dhtTemp)) {
    tft.print("-- C");
  } else {
    tft.print(dhtTemp, 1);
    tft.print(" C");
  }

  tft.setCursor(25, 240);
  tft.print("HUMI: ");
  if (isnan(dhtHum)) {
    tft.print("-- %");
  } else {
    tft.print(dhtHum, 1);
    tft.print(" %");
  }

  tft.setTextSize(1);
  tft.setTextColor(C_CYAN);
  tft.setCursor(25, 275);
  tft.print("RELAY: ");
  tft.print(relayState ? "ACTIVE" : "INACTIVE");
  if (settings.tempCtrlEnabled) {
    tft.print(" (AUTO ");
    tft.print(settings.tempThreshold, 0);
    tft.print("~");
    tft.print(settings.tempThresholdOff, 0);
    tft.print("C)");
  } else if (settings.relayTimerEnabled) {
    tft.print(" (TIMER)");
  } else {
    tft.print(" (MANUAL)");
  }
}

void drawCurrentScreen(bool forceRedraw) {
    if (isInStandby) return;
    
    switch(currentScreen) {
        case SCREEN_CONTROL:
            drawControlScreen();
            break;
        case SCREEN_WEATHER:
            drawWeatherScreen();
            break;
        case SCREEN_CLOCK:
            drawClockScreen(true);
            break;
    }
    
    if (currentScreen != SCREEN_CLOCK) {
      updateStatusLine();
    }
}

void drawClockScreen(bool isInitialDraw) {
    if (!isInitialDraw) return;
    
    tft.fillScreen(C_BG);
    drawGridBackground();
  
    tft.fillRect(0, 0, 240, 24, C_CYAN);
    tft.setTextColor(C_BG);
    tft.setTextSize(2);
    tft.setCursor(30, 5);
    tft.print("[SYSTEM CHRONOMETER]");

    drawCyberFrame(20, 250, 200, 40, C_GREEN, "");
    drawCyberFrame(20, 210, 200, 15, C_ORANGE, "");
    
    lastMinute = -1;
    lastSecond = -1;
    lastDay = -1;
    updateClockTime();
    updateStatusLine();
}

void updateClockTime() {
    time_t rawTime = timeClient.getEpochTime();
    struct tm * ti = localtime(&rawTime);
    
    if (ti->tm_mday != lastDay) {
        lastDay = ti->tm_mday;
        char dateBuf[20];
        sprintf(dateBuf, "%04d.%02d.%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
        tft.fillRect(10, 40, 220, 25, C_BG);
        tft.setTextColor(C_GREEN);
        tft.setTextSize(2);
        tft.setCursor(55, 45);
        tft.print(dateBuf);
        
        const char* weeks[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
        tft.fillRect(22, 252, 196, 36, C_BG);
        tft.setTextColor(C_CYAN);
        tft.setTextSize(3);
        int16_t x1, y1;
        uint16_t w, h;
        tft.getTextBounds(weeks[ti->tm_wday], 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(120 - w / 2, 260);
        tft.print(weeks[ti->tm_wday]);
    }
    
    if (ti->tm_min != lastMinute) {
        lastMinute = ti->tm_min;
        char timeBuf[6];
        sprintf(timeBuf, "%02d:%02d", ti->tm_hour, ti->tm_min);
        tft.fillRect(5, 80, 230, 80, C_BG);
        tft.setTextColor(C_WHITE);
        tft.setTextSize(7);
        tft.setCursor(20, 100);
        tft.print(timeBuf);
    }

    if (ti->tm_sec != lastSecond) {
        lastSecond = ti->tm_sec;
        int barWidth = map(ti->tm_sec, 0, 59, 0, 198);
        tft.fillRect(21, 211, 198, 13, C_DARK_GREY);
        tft.fillRect(21, 211, barWidth, 13, C_ORANGE);
        
        char secBuf[3];
        sprintf(secBuf, "%02d", ti->tm_sec);
        tft.fillRect(105, 185, 30, 16, C_BG);
        tft.setTextSize(2);
        tft.setTextColor(C_ORANGE);
        tft.setCursor(105, 185);
        tft.print(secBuf);
    }
}

#endif // GUI_H