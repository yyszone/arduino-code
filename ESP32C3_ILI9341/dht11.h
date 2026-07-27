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

        // 1. 发送主机起始信号
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        delay(20); 
        
        digitalWrite(_pin, HIGH);
        delayMicroseconds(30);
        pinMode(_pin, INPUT_PULLUP); 
        delayMicroseconds(10);

        // 2. 检测温湿度响应
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

        // 3. 接收 40 位串行数据
        noInterrupts(); 
        
        for (int i = 0; i < 40; i++) {
            uint32_t low_start = micros();
            while (digitalRead(_pin) == LOW) {
                if (micros() - low_start > 100) {
                    interrupts();
                    return false;
                }
            }
            
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
            if (duration > 48) { 
                dht_dat[byteIndex] |= 1;
            }
        }
        
        interrupts(); 

        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, HIGH);

        // 4. 校验和测试
        uint8_t checksum = dht_dat[0] + dht_dat[1] + dht_dat[2] + dht_dat[3];
        
        Serial.printf("[DEBUG] DHT11 RAW: [%02X %02X %02X %02X %02X] | CRC: %s\n", 
                      dht_dat[0], dht_dat[1], dht_dat[2], dht_dat[3], dht_dat[4], 
                      (checksum == dht_dat[4]) ? "OK" : "FAILED");

        if (dht_dat[4] != checksum) {
            return false; 
        }

        humidity = (float)dht_dat[0];
        if (dht_dat[1] > 0 && dht_dat[1] < 10) {
            humidity += (float)dht_dat[1] * 0.1f;
        }
        
        temperature = (float)dht_dat[2];
        if (dht_dat[3] > 0 && dht_dat[3] < 10) {
            temperature += (float)dht_dat[3] * 0.1f;
        }

        if (temperature < -10.0f || temperature > 60.0f || humidity < 5.0f || humidity > 100.0f) {
            return false;
        }

        return true;
    }
};

#endif // DHT11_H