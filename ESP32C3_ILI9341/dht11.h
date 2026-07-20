#ifndef DHT11_H
#define DHT11_H

#include <Arduino.h>

class DHT11_ESP32 {
private:
    uint8_t _pin;

public:
    DHT11_ESP32(uint8_t pin) : _pin(pin) {}

    void begin() {
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, HIGH);
    }

    bool read(float &temperature, float &humidity) {
        uint8_t dht_dat[5] = {0, 0, 0, 0, 0};

        // 1. 主机发送起始信号 (MCU 拉低总线 20ms)
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        delay(20); 
        
        // 主机拉高总线，并切换为上拉输入，等待传感器响应
        digitalWrite(_pin, HIGH);
        delayMicroseconds(30);
        pinMode(_pin, INPUT_PULLUP); 
        delayMicroseconds(10);

        // 2. 检测温湿度计的响应信号 (80us 低电平 + 80us 高电平)
        uint32_t timeout = micros();
        while (digitalRead(_pin) == HIGH) {
            if (micros() - timeout > 150) return false;
        }
        
        timeout = micros();
        while (digitalRead(_pin) == LOW) {
            if (micros() - timeout > 150) return false;
        }
        
        timeout = micros();
        while (digitalRead(_pin) == HIGH) {
            if (micros() - timeout > 150) return false;
        }

        // 3. 开始接收 40 位串行数据 (5个字节)
        noInterrupts(); // 进入临界区，临时屏蔽中断防止 FreeRTOS 任务调度打断微秒时序
        
        for (int i = 0; i < 40; i++) {
            // 等待低电平结束 (每位数据开头的 50us 低电平)
            uint32_t low_start = micros();
            while (digitalRead(_pin) == LOW) {
                if (micros() - low_start > 100) {
                    interrupts();
                    return false;
                }
            }
            
            // 测量高电平持续时间 (26-28us 代表 0，70us 代表 1)
            uint32_t high_start = micros();
            while (digitalRead(_pin) == HIGH) {
                if (micros() - high_start > 150) {
                    interrupts();
                    return false;
                }
            }
            uint32_t duration = micros() - high_start;
            
            int byteIndex = i / 8;
            dht_dat[byteIndex] <<= 1;
            // 针对 ESP32-C3 优化判据：大于 48us 判定为二进制 1，否则为 0
            if (duration > 48) { 
                dht_dat[byteIndex] |= 1;
            }
        }
        
        interrupts(); // 恢复中断

        // 恢复总线空闲
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, HIGH);

        // 4. 计算并校验校验和
        uint8_t checksum = dht_dat[0] + dht_dat[1] + dht_dat[2] + dht_dat[3];
        
        // 原始数据 16 进制诊断输出
        Serial.printf("[DEBUG] DHT11 RAW: [%02X %02X %02X %02X %02X] | CRC: %s\n", 
                      dht_dat[0], dht_dat[1], dht_dat[2], dht_dat[3], dht_dat[4], 
                      (checksum == dht_dat[4]) ? "OK" : "FAILED");

        if (dht_dat[4] != checksum) {
            return false; // 校验失败
        }

        // 5. 换算温湿度数据
        humidity = (float)dht_dat[0];
        if (dht_dat[1] > 0 && dht_dat[1] < 10) {
            humidity += (float)dht_dat[1] * 0.1f;
        }
        
        temperature = (float)dht_dat[2];
        if (dht_dat[3] > 0 && dht_dat[3] < 10) {
            temperature += (float)dht_dat[3] * 0.1f;
        }

        // 过滤环境异常值
        if (temperature < -10.0f || temperature > 60.0f || humidity < 5.0f || humidity > 100.0f) {
            return false;
        }

        return true;
    }
};

#endif // DHT11_H