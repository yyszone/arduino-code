#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <NTPClient.h>
#include <time.h>
#include <string>
#include <ctype.h>

extern NTPClient timeClient;

// 8x16 基础点阵字模 (ASCII 32-90)
static const uint8_t FONT_8X16_MINI[][16] PROGMEM = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Space
    {0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, // !
    {0x00,0x36,0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    {0x00,0x00,0x36,0x36,0x7F,0x36,0x36,0x36,0x7F,0x36,0x36,0x00,0x00,0x00,0x00,0x00}, // #
    {0x00,0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // $
    {0x00,0x00,0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // %
    {0x00,0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // &
    {0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '
    {0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // (
    {0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // )
    {0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // *
    {0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // ,
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, // .
    {0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // /
    {0x00,0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0
    {0x00,0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 1
    {0x00,0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 2
    {0x00,0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 3
    {0x00,0x0C,0x1C,0x34,0x64,0x7E,0x04,0x0E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 4
    {0x00,0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 5
    {0x00,0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 6
    {0x00,0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 7
    {0x00,0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 8
    {0x00,0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 9
    {0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // :
    {0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ;
    {0x00,0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // <
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // =
    {0x00,0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // >
    {0x00,0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ?
    {0x00,0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // @
    {0x00,0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // A
    {0x00,0x7C,0x66,0x7C,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // B
    {0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // C
    {0x00,0x78,0x6C,0x66,0x66,0x6C,0x78,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // D
    {0x00,0x7E,0x60,0x7C,0x60,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // E
    {0x00,0x7E,0x60,0x7C,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // F
    {0x00,0x3C,0x66,0x60,0x6E,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // G
    {0x00,0x66,0x66,0x7E,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // H
    {0x00,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // I
    {0x00,0x0E,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // J
    {0x00,0x66,0x6C,0x78,0x78,0x6C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // K
    {0x00,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // L
    {0x00,0x63,0x77,0x7F,0x6B,0x63,0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // M
    {0x00,0x66,0x76,0x7E,0x7E,0x6E,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // N
    {0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // O
    {0x00,0x7C,0x66,0x7C,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // P
    {0x00,0x3C,0x66,0x66,0x66,0x6A,0x3C,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Q
    {0x00,0x7C,0x66,0x7C,0x6C,0x66,0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // R
    {0x00,0x3C,0x66,0x30,0x0C,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // S
    {0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // T
    {0x00,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // U
    {0x00,0x66,0x66,0x66,0x3C,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // V
    {0x00,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // W
    {0x00,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // X
    {0x00,0x66,0x66,0x3C,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Y
    {0x00,0x7E,0x0C,0x18,0x30,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  // Z
};

class PixelManager {
private:
    Preferences prefs;
    NimBLEClient* pClient = nullptr;
    NimBLERemoteCharacteristic* pWriteChar = nullptr;
    NimBLERemoteCharacteristic* pNotifyChar = nullptr;
    NimBLEAddress targetAddr;

    volatile bool deviceFound = false;
    volatile uint32_t nextScanAt = 0;
    int targetRSSI = 0;
    bool writeWithResponse = false;
    unsigned long lastClockCycleMs = 0;
    int lastCheckedMinute = -1;
    bool pendingSleepCheck = true;

    static inline uint8_t reverseByte(uint8_t b) {
        b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
        b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
        b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
        return b;
    }

    static uint32_t calcCrc32(const uint8_t *data, size_t length) {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; i++) {
            crc ^= data[i];
            for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
        return ~crc;
    }

    void renderGlyph(uint8_t fontIdx, uint8_t size, uint8_t* out) {
        if (size == 1) {
            for (int k = 0; k < 16; k++) out[k] = reverseByte(pgm_read_byte(&(FONT_8X16_MINI[fontIdx][k])));
        } else {
            for (int y = 0; y < 16; y++) {
                uint8_t r = pgm_read_byte(&(FONT_8X16_MINI[fontIdx][y]));
                uint8_t b0 = 0, b1 = 0;
                for (int bit = 0; bit < 4; bit++) {
                    if (r & (1 << (7 - bit))) b0 |= (0x03 << (bit * 2));
                    if (r & (1 << (3 - bit))) b1 |= (0x03 << (bit * 2));
                }
                size_t row = y * 4;
                out[row]     = b0; out[row + 1] = b1;
                out[row + 2] = b0; out[row + 3] = b1;
            }
        }
    }

    bool isInSleepWindow(int curH, int curM, const String& onStr, const String& offStr) {
        if (onStr.length() != 5 || offStr.length() != 5) return false;
        int onH = onStr.substring(0, 2).toInt();
        int onM = onStr.substring(3, 5).toInt();
        int offH = offStr.substring(0, 2).toInt();
        int offM = offStr.substring(3, 5).toInt();

        int curTotal = curH * 60 + curM;
        int onTotal = onH * 60 + onM;
        int offTotal = offH * 60 + offM;
        if (offTotal == onTotal) return false;

        if (offTotal > onTotal) {
            return (curTotal >= offTotal || curTotal < onTotal);
        } else {
            return (curTotal >= offTotal && curTotal < onTotal);
        }
    }

public:
    volatile bool isConnected = false;
    bool currentPower = true;
    uint8_t currentBrightness = 60;
    uint8_t currentClockStyle = 4;
    String targetMacStr = "23:44:90:fe:4a:79";
    int clockCycleSec = 0;
    String autoOnTime = "";
    String autoOffTime = "";

    // ⭐️ 核心改进：双重可靠时间获取（解绑对外部布尔变量的脆弱依赖）
    bool getCurrentTime(int &h, int &m, int &s) {
        // 途径 1: 使用 NTPClient (只要时间戳合理即有效)
        if (timeClient.getEpochTime() > 1000000) {
            h = timeClient.getHours();
            m = timeClient.getMinutes();
            s = timeClient.getSeconds();
            return true;
        }

        // 途径 2: 系统底层 RTC 时间回退
        time_t now = time(nullptr);
        if (now > 1000000000L) {
            struct tm ti;
            localtime_r(&now, &ti);
            h = ti.tm_hour;
            m = ti.tm_min;
            s = ti.tm_sec;
            return true;
        }

        return false;
    }

    void begin() {
        prefs.begin("pixel-cfg", false);
        targetMacStr = prefs.getString("mac", "23:44:90:fe:4a:79");
        clockCycleSec = prefs.getInt("cycle", 0);
        autoOnTime = prefs.getString("auto_on", "");
        autoOffTime = prefs.getString("auto_off", "");
        prefs.end();

        NimBLEDevice::init("ESP32_Fan_Pixel");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);

        NimBLEScan* pScan = NimBLEDevice::getScan();
        pScan->setScanCallbacks(new ScanCallbacks(this), false);
        startScan();
    }

    void saveConfig(const String& mac, int cycle, const String& onT, const String& offT) {
        targetMacStr = mac;
        clockCycleSec = cycle;
        autoOnTime = onT;
        autoOffTime = offT;

        prefs.begin("pixel-cfg", false);
        prefs.putString("mac", targetMacStr);
        prefs.putInt("cycle", clockCycleSec);
        prefs.putString("auto_on", autoOnTime);
        prefs.putString("auto_off", autoOffTime);
        prefs.end();
        reconnect();
    }

    class ClientCallbacks : public NimBLEClientCallbacks {
        PixelManager* parent;
    public:
        ClientCallbacks(PixelManager* p) : parent(p) {}
        void onConnect(NimBLEClient*) override {}
        void onDisconnect(NimBLEClient*, int reason) override {
            parent->isConnected = false;
            parent->pWriteChar = nullptr;
            parent->pNotifyChar = nullptr;
            parent->nextScanAt = millis() + 3000;
        }
    };

    class ScanCallbacks : public NimBLEScanCallbacks {
        PixelManager* parent;
    public:
        ScanCallbacks(PixelManager* p) : parent(p) {}
        void onDiscovered(const NimBLEAdvertisedDevice* dev) override {
            if (parent->deviceFound) return;
            std::string mac = dev->getAddress().toString();
            for (char& c : mac) c = tolower((unsigned char)c);
            String cur = parent->targetMacStr; cur.toLowerCase();

            if (mac == cur.c_str()) {
                if (!dev->isConnectable()) return;
                parent->targetAddr = dev->getAddress();
                parent->targetRSSI = dev->getRSSI();
                parent->deviceFound = true;
                NimBLEDevice::getScan()->stop();
            }
        }
    };

    void releaseClient() {
        isConnected = false;
        pWriteChar = nullptr;
        pNotifyChar = nullptr;
        if (pClient) { NimBLEDevice::deleteClient(pClient); pClient = nullptr; }
    }

    void startScan() {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan->isScanning()) return;
        deviceFound = false;
        pScan->setActiveScan(true);
        pScan->setInterval(200);
        pScan->setWindow(60);
        pScan->start(0, false);
    }

    bool connectToServer() {
        if (targetAddr.isNull()) return false;
        releaseClient();

        pClient = NimBLEDevice::createClient();
        if (!pClient) return false;

        pClient->setClientCallbacks(new ClientCallbacks(this), true);
        pClient->setConnectTimeout(8000);

        if (!pClient->connect(targetAddr, true, false, true)) {
            releaseClient();
            return false;
        }

        for (auto* s : pClient->getServices(true)) {
            if (!s) continue;
            auto* wc = s->getCharacteristic(NimBLEUUID("0000fa02-0000-1000-8000-00805f9b34fb"));
            auto* nc = s->getCharacteristic(NimBLEUUID("0000fa03-0000-1000-8000-00805f9b34fb"));
            if (wc) pWriteChar = wc;
            if (nc) pNotifyChar = nc;
        }

        if (!pWriteChar) { releaseClient(); return false; }
        writeWithResponse = !pWriteChar->canWriteNoResponse();

        isConnected = true;

        // 连上后立即检查是否处于夜间睡眠时间
        int h = 0, m = 0, s = 0;
        if (getCurrentTime(h, m, s)) {
            if (isInSleepWindow(h, m, autoOnTime, autoOffTime)) {
                setPower(false);
                pendingSleepCheck = false;
            }
        } else {
            pendingSleepCheck = true;
        }

        return true;
    }

    bool sendCommand(const uint8_t* data, size_t len) {
        if (!isConnected || !pClient || !pClient->isConnected() || !pWriteChar) return false;
        return pWriteChar->writeValue(data, len, writeWithResponse);
    }

    void setPower(bool on) {
        currentPower = on;
        uint8_t cmd[] = {0x05, 0x00, 0x07, 0x01, (uint8_t)(on ? 0x01 : 0x00)};
        sendCommand(cmd, sizeof(cmd));
    }

    void setBrightness(uint8_t level) {
        level = constrain(level, 1, 100);
        currentBrightness = level;
        uint8_t cmd[] = {0x05, 0x00, 0x04, 0x80, level};
        sendCommand(cmd, sizeof(cmd));
    }

    // ⭐️ 核心修复：时钟切换时，绝不无脑跌回 12:30！
    void setClockMode(uint8_t style, bool showDate = false, int h = -1, int m = -1, int s = -1) {
        if (style < 1) style = 1;
        if (style > 8) style = 8;
        currentClockStyle = style;

        // 若未提供时分秒，先尝试从网络或本地读取真实时间
        if (h < 0 || m < 0 || s < 0) {
            if (!getCurrentTime(h, m, s)) {
                // 如果目前确实还没有网络同步时间，直接不下发对时指令，避免打乱屏幕内部自身走的走时！
                uint8_t clockCmdOnly[] = {0x0B, 0x00, 0x06, 0x01, style, 0x01, (uint8_t)(showDate ? 0x01 : 0x00), 26, 5, 1, 1};
                sendCommand(clockCmdOnly, sizeof(clockCmdOnly));
                return;
            }
        }

        uint8_t timeCmd[] = {0x08, 0x00, 0x01, 0x80, (uint8_t)h, (uint8_t)m, (uint8_t)s, 0x00};
        sendCommand(timeCmd, sizeof(timeCmd));
        delay(60);

        uint8_t clockCmd[] = {0x0B, 0x00, 0x06, 0x01, style, 0x01, (uint8_t)(showDate ? 0x01 : 0x00), 26, 5, 1, 1};
        sendCommand(clockCmd, sizeof(clockCmd));
    }

    // ⭐️ 显式 NTP 校时接口
    bool syncNtp() {
        if (WiFi.status() == WL_CONNECTED) {
            timeClient.update();
        }
        int h = 0, m = 0, s = 0;
        if (getCurrentTime(h, m, s)) {
            setClockMode(currentClockStyle, false, h, m, s);
            return true;
        }
        return false;
    }

    bool sendText(const String& msg, uint8_t anim = 1, uint8_t speed = 90, String hexColor = "FF5500", uint8_t fontSize = 2) {
        if (!isConnected || msg.length() == 0) return false;

        anim = constrain(anim, 1, 8);
        speed = constrain(speed, 1, 100);
        fontSize = (fontSize == 1) ? 1 : 2;

        hexColor.replace("#", "");
        if (hexColor.length() < 6) hexColor = "FF5500";
        uint8_t r = strtoul(hexColor.substring(0, 2).c_str(), nullptr, 16);
        uint8_t g = strtoul(hexColor.substring(2, 4).c_str(), nullptr, 16);
        uint8_t b = strtoul(hexColor.substring(4, 6).c_str(), nullptr, 16);

        size_t charCount = msg.length();
        if (fontSize == 2 && charCount > 7) charCount = 7;
        if (fontSize == 1 && charCount > 25) charCount = 25;

        uint8_t flag = (fontSize == 1) ? 0x00 : 0x02;
        size_t bmpLen = (fontSize == 1) ? 16 : 64;
        size_t bytesPerChar = 4 + bmpLen;
        size_t payloadSize = 14 + (charCount * bytesPerChar);

        uint8_t* payload = (uint8_t*)malloc(payloadSize);
        if (!payload) return false;

        payload[0] = (uint8_t)(charCount & 0xFF);
        payload[1] = (uint8_t)((charCount >> 8) & 0xFF);
        payload[2] = 0x01; payload[3] = 0x00;
        payload[4] = anim; payload[5] = speed;
        payload[6] = 0x01;
        payload[7] = r; payload[8] = g; payload[9] = b;
        payload[10] = 0; payload[11] = 0; payload[12] = 0; payload[13] = 0;

        for (size_t i = 0; i < charCount; i++) {
            char c = msg[i];
            uint8_t fontIdx = 0;
            if (c >= 32 && c <= 90) fontIdx = c - 32;
            else if (c >= 'a' && c <= 'z') fontIdx = (c - 'a' + 'A') - 32;

            size_t offset = 14 + (i * bytesPerChar);
            payload[offset] = flag;
            payload[offset + 1] = r; payload[offset + 2] = g; payload[offset + 3] = b;
            renderGlyph(fontIdx, fontSize, &payload[offset + 4]);
        }

        uint32_t crc = calcCrc32(payload, payloadSize);
        size_t totalLen = 15 + payloadSize;
        uint8_t* packet = (uint8_t*)malloc(totalLen);
        if (!packet) { free(payload); return false; }

        packet[0] = (uint8_t)(totalLen & 0xFF);
        packet[1] = (uint8_t)((totalLen >> 8) & 0xFF);
        packet[2] = 0x00; packet[3] = 0x01; packet[4] = 0x00;
        packet[5] = (uint8_t)(payloadSize & 0xFF);
        packet[6] = (uint8_t)((payloadSize >> 8) & 0xFF);
        packet[7] = (uint8_t)((payloadSize >> 16) & 0xFF);
        packet[8] = (uint8_t)((payloadSize >> 24) & 0xFF);
        packet[9] = (uint8_t)(crc & 0xFF);
        packet[10] = (uint8_t)((crc >> 8) & 0xFF);
        packet[11] = (uint8_t)((crc >> 16) & 0xFF);
        packet[12] = (uint8_t)((crc >> 24) & 0xFF);
        packet[13] = 0x00; packet[14] = 0x00;

        memcpy(&packet[15], payload, payloadSize);
        free(payload);

        bool ok = sendCommand(packet, totalLen);
        free(packet);
        return ok;
    }

    void reconnect() {
        NimBLEScan* scan = NimBLEDevice::getScan();
        if (scan && scan->isScanning()) scan->stop();
        releaseClient();
        nextScanAt = millis();
    }

    void loop() {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (deviceFound) {
            pScan->stop();
            delay(50);
            deviceFound = false;
            if (!connectToServer()) nextScanAt = millis() + 3000;
        }

        if (!isConnected && !deviceFound && !pScan->isScanning() && (int32_t)(millis() - nextScanAt) >= 0) {
            startScan();
        }

        int h = 0, m = 0, s = 0;
        bool hasTime = getCurrentTime(h, m, s);
        bool isSleeping = false;

        if (hasTime) {
            isSleeping = isInSleepWindow(h, m, autoOnTime, autoOffTime);

            if (pendingSleepCheck && isConnected) {
                pendingSleepCheck = false;
                if (isSleeping) {
                    setPower(false);
                }
            }

            if (m != lastCheckedMinute) {
                lastCheckedMinute = m;
                char buf[8];
                snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
                String curTime = String(buf);

                if (autoOffTime.length() == 5 && curTime == autoOffTime) {
                    setPower(false);
                } else if (autoOnTime.length() == 5 && curTime == autoOnTime) {
                    setPower(true);
                }
            }
        }

        // 关机或处于睡眠区间时，坚决不发送蓝牙包
        if (!currentPower || isSleeping) {
            return;
        }

        if (isConnected && clockCycleSec > 0) {
            if (millis() - lastClockCycleMs >= ((unsigned long)clockCycleSec * 1000UL)) {
                lastClockCycleMs = millis();
                uint8_t next = (currentClockStyle % 8) + 1; // 1~8 循环
                setClockMode(next, false);
            }
        }
    }

    int getRssi() {
        return (pClient && isConnected) ? pClient->getRssi() : targetRSSI;
    }
};

extern PixelManager pixelManager;