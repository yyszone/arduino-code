#ifndef INA219_SENSOR_H
#define INA219_SENSOR_H

#include <Arduino.h>
#include <Wire.h>
#include <INA219.h>
#include "config.h"

class INA219Sensor {
private:
    INA219 ina;
    bool isReady = false;

public:
    INA219Sensor() : ina(0.1f, 5.0f, 0x40) {} // 0.1欧电阻, 5A最大范围, 0x40地址

    bool begin() {
        Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
        Wire.setTimeOut(1000); // 防卡死看门狗锁
        if (ina.begin()) {
            isReady = true;
            return true;
        }
        isReady = false;
        return false;
    }

    bool ready() const { return isReady; }

    void update(SystemState &st) {
        if (!isReady) return;
        float v = ina.getVoltage();
        // 简单合理性过滤，防止偶发总线抖动读出负几千伏或 NaN
        if (!isnan(v) && v >= 0.0f && v < 36.0f) {
            st.busVoltage   = v;
            st.shuntVoltage = ina.getShuntVoltage() * 1000.f; 
            st.current_mA   = ina.getCurrent()      * 1000.f; 
            st.power_mW     = ina.getPower()        * 1000.f; 
        }
    }

};

#endif // INA219_SENSOR_H