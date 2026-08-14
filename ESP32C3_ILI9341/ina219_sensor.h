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
        st.busVoltage   = ina.getVoltage();
        st.shuntVoltage = ina.getShuntVoltage() * 1000.f; // 转为 mV
        st.current_mA   = ina.getCurrent()      * 1000.f; // 转为 mA
        st.power_mW     = ina.getPower()        * 1000.f; // 转为 mW
    }
};

#endif // INA219_SENSOR_H