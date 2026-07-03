// =================================================================
// display_tm1637.h — TM1637 4位数字数码管模拟驱动（电流对齐 & 单小数优化版）
// =================================================================
#ifndef DISPLAY_TM1637_H
#define DISPLAY_TM1637_H

#include <Arduino.h>
#include <time.h>

// 声明外部 Wi-Fi 定时控制变量
extern bool currentWifiState;

// ── 屏幕硬件类型配置 ──
// 【请根据您的数码管实际物理类型进行选择】
// 1 : 小数点屏（物理上只有底部单点 "."，如 12.5U）
// 0 : 时钟屏（物理上只有中间双点冒号 ":"，如 12:5U）
#define TM1637_SCREEN_TYPE 0 

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
unsigned long lastModeSwitchMs = 0; // 控制轮播模式切换
unsigned long lastSegWriteMs = 0;   // 控制屏幕实际刷新率
bool displayEnabled = true;

// =================================================================
// 纯软件模拟 TM1637 协议类（恢复正序写入 & 100us 安全延时）
// =================================================================
class TM1637SoftwareDriver {
private:
    uint8_t clk;
    uint8_t dio;
    uint8_t brightnessLevel; 

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

        digitalWrite(clk, LOW);
        digitalWrite(dio, HIGH); 
        pinMode(dio, INPUT);
        wait();
        digitalWrite(clk, HIGH);
        wait();
        
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
        brightnessLevel = level & 7; 
        startSignal();
        writeByte(0x80 | (on ? 8 : 0) | brightnessLevel);
        stopSignal();
    }

    void displaySegments(const uint8_t segments[]) {
        startSignal();
        writeByte(0x40);
        stopSignal();

        startSignal();
        writeByte(0xC0);
        for (uint8_t i = 0; i < 4; i++) {
            writeByte(segments[i]);
        }
        stopSignal();

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
const uint8_t SEG_U = 0x3e;    // "U" (饱满的大写 U)
const uint8_t SEG_A = 0x77;    // "A" (大写 A)
const uint8_t SEG_LINE = 0x40; // "-"

// 外部调用初始化
void display_begin() {
    displayDriver.init();
    displayDriver.setBrightness(4, true); 
}

// 休眠显示
void display_off() {
    if (displayEnabled) {
        displayDriver.setBrightness(0, false);
        displayDriver.clearDisplay();
        displayEnabled = false;
        Serial.println("[TM1637] 节能休眠：已关闭数码管显示");
    }
}

// 唤醒显示
void display_on() {
    if (!displayEnabled) {
        displayDriver.setBrightness(4, true);
        displayEnabled = true;
        Serial.println("[TM1637] 节能恢复：已开启数码管显示");
    }
}

// 刷新数码管显示
void display_update(float voltage, float current_mA, bool userEnabled, bool forceUpdate = false) {
    if (!userEnabled || !currentWifiState) {
        display_off();
        return;
    } else {
        display_on();
    }

    unsigned long now = millis();
    bool modeChanged = false;

    // 1. 轮播定时器：每 3 秒切换一次显示模式
    if (now - lastModeSwitchMs >= DISPLAY_ROTATION_INTERVAL || forceUpdate) {
        lastModeSwitchMs = now;
        modeChanged = true;

        if (!forceUpdate) {
            if (currentDispMode == DISP_TIME) {
                currentDispMode = DISP_VOLTAGE;
            } else if (currentDispMode == DISP_VOLTAGE) {
                currentDispMode = DISP_CURRENT;
            } else {
                currentDispMode = DISP_TIME;
            }
        }
    }

    // 2. 刷新定时器：时间模式下每 500ms 刷新（闪烁冒号），电压电流模式每 1000ms 刷新
    unsigned long refreshInterval = (currentDispMode == DISP_TIME) ? 500UL : 1000UL;
    if (now - lastSegWriteMs >= refreshInterval || modeChanged || forceUpdate) {
        lastSegWriteMs = now;

        uint8_t showData[4] = {0, 0, 0, 0};

        if (currentDispMode == DISP_TIME) {
            time_t now_t = time(nullptr);
            if (now_t > 1000000000L) {
                struct tm* timeinfo = localtime(&now_t);
                int hh = timeinfo->tm_hour;
                int mm = timeinfo->tm_min;
                
                // 冒号闪烁控制
                bool toggleState = (now / 500) % 2 == 0; 

                showData[0] = getDigitSegment(hh / 10);
                showData[1] = getDigitSegment(hh % 10) | (toggleState ? 0x80 : 0); 
                showData[2] = getDigitSegment(mm / 10);
                showData[3] = getDigitSegment(mm % 10);
            } else {
                showData[0] = SEG_LINE;
                showData[1] = SEG_LINE;
                showData[2] = SEG_LINE;
                showData[3] = SEG_LINE;
            }
        } 
        else if (currentDispMode == DISP_VOLTAGE) {
            // 电压显示（双位整数+1位小数，不需要后移）：如 "12.5U" 或 "12:5U"
            int v_10 = (int)(voltage * 10.0f);
            if (v_10 > 999) v_10 = 999;

            showData[3] = SEG_U; 
            if (v_10 >= 100) {
                showData[0] = getDigitSegment(v_10 / 100);
                showData[1] = getDigitSegment((v_10 / 10) % 10) | 0x80; // 点亮小数点/冒号
                showData[2] = getDigitSegment(v_10 % 10);
            } else {
                showData[0] = 0;
                showData[1] = getDigitSegment(v_10 / 10) | 0x80; 
                showData[2] = getDigitSegment(v_10 % 10);
            }
        } 
        else if (currentDispMode == DISP_CURRENT) {
            // 【全新电流显示优化】：保留一位小数，整体向右平移 1 位，完美对齐物理冒号位置
            // 采用 +50mA 偏置来实现精准的四舍五入算法
            int amp_10 = (int)((current_mA + 50.0f) / 100.0f); 
            if (amp_10 > 99) amp_10 = 99; // 限制最高显示 9.9A

            showData[0] = 0;                                   // 【关键】第一位物理留空
            showData[1] = getDigitSegment(amp_10 / 10) | 0x80; // 第二位（个位）带点/冒号
            showData[2] = getDigitSegment(amp_10 % 10);        // 第三位（十分位）
            showData[3] = SEG_A;                               // 第四位固定显示单位 'A'
        }

        displayDriver.displaySegments(showData);
    }
}

#endif