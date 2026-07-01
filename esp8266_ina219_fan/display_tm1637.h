// =================================================================
// display_tm1637.h — TM1637 4位数字数码管模拟驱动（ESP8266 深度兼容版）
// =================================================================
#ifndef DISPLAY_TM1637_H
#define DISPLAY_TM1637_H

#include <Arduino.h>
#include <time.h>

// 声明外部 Wi-Fi 定时控制变量
extern bool currentWifiState;

// 自动兼容不同的 ESP8266 开发板引脚定义
#ifndef D6
#define D6 12 // GPIO12
#endif
#ifndef D7
#define D7 13 // GPIO13
#endif

// 引脚定义：D6 (CLK), D7 (DIO)
constexpr uint8_t TM1637_CLK_PIN = D6; 
constexpr uint8_t TM1637_DIO_PIN = D7; 

// 轮播显示间隔 (毫秒)
constexpr unsigned long DISPLAY_ROTATION_INTERVAL = 3000UL;

enum DisplayMode {
    DISP_TIME,
    DISP_VOLTAGE,
    DISP_CURRENT
};

DisplayMode currentDispMode = DISP_TIME;
unsigned long lastDispSwitchMs = 0;
bool displayEnabled = true;

// =================================================================
// 纯软件模拟 TM1637 协议类（融合官方库逻辑并针对高速 ESP 增加电容穿透延时）
// =================================================================
class TM1637SoftwareDriver {
private:
    uint8_t clk;
    uint8_t dio;
    uint8_t brightnessLevel; // 0~7 亮度级别

    // 100微秒安全延时：平抑 ESP8266 极速翻转，穿透数码管板载滤波电容
    void wait() {
        delayMicroseconds(100);
    }

    void startSignal() {
        pinMode(dio, OUTPUT);
        digitalWrite(clk, HIGH);
        digitalWrite(dio, HIGH);
        wait();
        digitalWrite(dio, LOW);
        wait();
        digitalWrite(clk, LOW);
        wait();
    }

    void stopSignal() {
        pinMode(dio, OUTPUT);
        digitalWrite(clk, LOW);
        digitalWrite(dio, LOW);
        wait();
        digitalWrite(clk, HIGH);
        wait();
        digitalWrite(dio, HIGH);
        wait();
    }

    void writeByte(uint8_t wr_data) {
        pinMode(dio, OUTPUT);
        for (uint8_t i = 0; i < 8; i++) {
            digitalWrite(clk, LOW);
            wait();
            if (wr_data & 0x01) {
                digitalWrite(dio, HIGH);
            } else {
                digitalWrite(dio, LOW);
            }
            wait();
            digitalWrite(clk, HIGH);
            wait();
            wr_data >>= 1;
        }

        // 产生并等待 ACK 响应信号（参考官方库检测逻辑）
        digitalWrite(clk, LOW);
        digitalWrite(dio, HIGH); // 释放数据线准备读取
        pinMode(dio, INPUT);
        wait();
        digitalWrite(clk, HIGH);
        wait();
        
        // 循环等待 ACK 释放，超时强制拉低退出防止死锁
        uint8_t ack_timeout = 0;
        while (digitalRead(dio)) {
            ack_timeout++;
            if (ack_timeout >= 200) {
                pinMode(dio, OUTPUT);
                digitalWrite(dio, LOW);
                break;
            }
            delayMicroseconds(5);
        }
        
        digitalWrite(clk, LOW);
        wait();
        pinMode(dio, OUTPUT);
    }

public:
    TM1637SoftwareDriver(uint8_t clk_pin, uint8_t dio_pin) : clk(clk_pin), dio(dio_pin), brightnessLevel(4) {}

    void init() {
        pinMode(clk, OUTPUT);
        pinMode(dio, OUTPUT);
        digitalWrite(clk, HIGH);
        digitalWrite(dio, HIGH);
        clearDisplay();
    }

    void setBrightness(uint8_t level, bool on = true) {
        brightnessLevel = level & 7; // 约束在 0~7
        startSignal();
        // 0x80 用于显示控制，第4位（值为8）表示打开显示
        writeByte(0x80 | (on ? 8 : 0) | brightnessLevel);
        stopSignal();
    }

    void displaySegments(const uint8_t segments[]) {
        // 命令1：写数据命令，设置自动地址加 1 (0x40)
        startSignal();
        writeByte(0x40);
        stopSignal();

        // 命令2：设置首地址 0xC0
        startSignal();
        writeByte(0xC0);
        for (uint8_t i = 0; i < 4; i++) {
            writeByte(segments[i]);
        }
        stopSignal();

        // 命令3：设置显示开与亮度级别 (0x88 + brightnessLevel)
        startSignal();
        writeByte(0x88 | brightnessLevel);
        stopSignal();
    }

    void clearDisplay() {
        uint8_t blank[4] = {0, 0, 0, 0};
        displaySegments(blank);
    }
};

// 实例化驱动
TM1637SoftwareDriver displayDriver(TM1637_CLK_PIN, TM1637_DIO_PIN);

// 共阳极 7 段数码管段位对照表 (0-9)
const uint8_t DIGIT_SEGMENTS[] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

// 提取段码
uint8_t getDigitSegment(uint8_t val) {
    if (val < 10) return DIGIT_SEGMENTS[val];
    return 0;
}

// 辅助自定义符号
const uint8_t SEG_U = 0x1c;    // "U"
const uint8_t SEG_A = 0x77;    // "A"
const uint8_t SEG_LINE = 0x40; // "-"

// 外部调用初始化
void display_begin() {
    displayDriver.init();
    displayDriver.setBrightness(4, true); // 设置初始亮度为 4
}

// 休眠显示
void display_off() {
    if (displayEnabled) {
        displayDriver.setBrightness(0, false);
        displayDriver.clearDisplay();
        displayEnabled = false;
        Serial.println("[TM1637] 休眠断开：已关闭数码管显示");
    }
}

// 唤醒显示
void display_on() {
    if (!displayEnabled) {
        displayDriver.setBrightness(4, true);
        displayEnabled = true;
        Serial.println("[TM1637] 休眠恢复：已开启数码管显示");
    }
}

// 数码管定时更新刷新函数
void display_update(float voltage, float current_mA, bool forceUpdate = false) {
    // 1. 根据 WiFi 射频定时开关决定数码管是否休眠
    if (!currentWifiState) {
        display_off();
        return;
    } else {
        display_on();
    }

    unsigned long now = millis();
    // 2. 达到轮播时间间隔
    if (now - lastDispSwitchMs >= DISPLAY_ROTATION_INTERVAL || forceUpdate) {
        lastDispSwitchMs = now;

        if (!forceUpdate) {
            if (currentDispMode == DISP_TIME) {
                currentDispMode = DISP_VOLTAGE;
            } else if (currentDispMode == DISP_VOLTAGE) {
                currentDispMode = DISP_CURRENT;
            } else {
                currentDispMode = DISP_TIME;
            }
        }

        uint8_t showData[4] = {0, 0, 0, 0};

        // 3. 多模式数据渲染
        if (currentDispMode == DISP_TIME) {
            time_t now_t = time(nullptr);
            if (now_t > 1000000000L) {
                struct tm* timeinfo = localtime(&now_t);
                int hh = timeinfo->tm_hour;
                int mm = timeinfo->tm_min;
                bool showColon = (timeinfo->tm_sec % 2 == 0); // 冒号每秒交替闪烁

                showData[0] = getDigitSegment(hh / 10);
                showData[1] = getDigitSegment(hh % 10) | (showColon ? 0x80 : 0); // 点亮第2位小数点充当时钟冒号
                showData[2] = getDigitSegment(mm / 10);
                showData[3] = getDigitSegment(mm % 10);
            } else {
                // 尚未获取到 NTP 网络时间时，显示 "----"
                showData[0] = SEG_LINE;
                showData[1] = SEG_LINE;
                showData[2] = SEG_LINE;
                showData[3] = SEG_LINE;
            }
        } 
        else if (currentDispMode == DISP_VOLTAGE) {
            // 显示电压，例如 "12.5U"
            int v_10 = (int)(voltage * 10.0f);
            if (v_10 > 999) v_10 = 999;

            showData[3] = SEG_U;
            if (v_10 >= 100) {
                showData[0] = getDigitSegment(v_10 / 100);
                showData[1] = getDigitSegment((v_10 / 10) % 10) | 0x80; // 带小数点
                showData[2] = getDigitSegment(v_10 % 10);
            } else {
                showData[0] = 0;
                showData[1] = getDigitSegment(v_10 / 10) | 0x80;
                showData[2] = getDigitSegment(v_10 % 10);
            }
        } 
        else if (currentDispMode == DISP_CURRENT) {
            // 显示电流，例如 "0.35A"
            int amp_100 = (int)(current_mA / 10.0f); 
            if (amp_100 > 999) amp_100 = 999;

            showData[3] = SEG_A;
            if (amp_100 >= 100) {
                showData[0] = getDigitSegment(amp_100 / 100);
                showData[1] = getDigitSegment((amp_100 / 10) % 10) | 0x80; 
                showData[2] = getDigitSegment(amp_100 % 10);
            } else {
                showData[0] = getDigitSegment(0);
                showData[1] = getDigitSegment((amp_100 / 10) % 10) | 0x80; 
                showData[2] = getDigitSegment(amp_100 % 10);
            }
        }

        displayDriver.displaySegments(showData);
    }
}

#endif