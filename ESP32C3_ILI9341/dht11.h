#ifndef DHT11_H
#define DHT11_H

#include <Arduino.h>

class DHT11_ESP32 {
private:
    uint8_t _pin;

    // 强行复位总线，防止 DHT11 芯片内部状态机挂起死锁
    void resetBus() {
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, HIGH);
        delay(100); // 拉高 100ms 让传感器回到 Standby 待命状态
    }

public:
    DHT11_ESP32(uint8_t pin) : _pin(pin) {}

    void begin() {
        resetBus();
    }

    bool read(float &temperature, float &humidity) {
        uint8_t dht_dat[5] = {0, 0, 0, 0, 0};

        // 1. 发送主机起始信号 (低电平至少 18ms)
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        delay(20); 

        digitalWrite(_pin, HIGH);
        delayMicroseconds(30);
        pinMode(_pin, INPUT_PULLUP);

        // 屏蔽全局中断，防止 ESP32 的 Wi-Fi 背景任务打断微秒时序
        noInterrupts();

        // 2. 检测温湿度响应信号 (80us 低 -> 80us 高)
        uint32_t timeout = micros();
        while (digitalRead(_pin) == LOW) {
            if (micros() - timeout > 100) { interrupts(); resetBus(); return false; }
        }

        timeout = micros();
        while (digitalRead(_pin) == HIGH) {
            if (micros() - timeout > 100) { interrupts(); resetBus(); return false; }
        }

        // 3. 接收 40 位串行数据
        for (int i = 0; i < 40; i++) {
            uint32_t low_start = micros();
            while (digitalRead(_pin) == LOW) {
                if (micros() - low_start > 100) { interrupts(); resetBus(); return false; }
            }

            uint32_t high_start = micros();
            while (digitalRead(_pin) == HIGH) {
                if (micros() - high_start > 150) { interrupts(); resetBus(); return false; }
            }

            // 高电平持续时间 > 40us 视作 logic '1'
            if ((micros() - high_start) > 40) {
                dht_dat[i / 8] |= (1 << (7 - (i % 8)));
            }
        }

        interrupts(); // 恢复中断
        resetBus();   // 读完恢复高电平

        // 4. 校验和测试
        uint8_t checksum = (dht_dat[0] + dht_dat[1] + dht_dat[2] + dht_dat[3]) & 0xFF;
        if (dht_dat[4] != checksum || dht_dat[4] == 0) {
            return false; 
        }

        humidity = (float)dht_dat[0] + (float)dht_dat[1] * 0.1f;
        temperature = (float)dht_dat[2] + (float)dht_dat[3] * 0.1f;

        if (temperature < -10.0f || temperature > 60.0f || humidity < 5.0f || humidity > 100.0f) {
            return false;
        }

        return true;
    }
};

#endif // DHT11_H