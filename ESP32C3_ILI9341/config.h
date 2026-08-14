#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

struct LogEntry { 
  String timestamp; 
  String message; 
  unsigned long epochTime; 
};

enum ScreenMode { 
  SCREEN_CONTROL, 
  SCREEN_WEATHER, 
  SCREEN_CLOCK 
};

enum class TripReason : uint8_t {
  NONE = 0,
  UNDERVOLTAGE = 1,
  OVERCURRENT  = 2, 
  MANUAL       = 3
};

const char* ssid = "yang1234";
const char* password = "y123456789";
const unsigned long standbyDelay = 60000; 

// HTTP 控制配置
const char* led_on_url = "http://192.168.31.162/LED-Control?ledPwm=3";
const char* led_off_url = "http://192.168.31.162/LED-Control?ledPwm=4";

const char* configFile = "/config.json"; 
const char* stateFile  = "/state.json";

#define TFT_MISO 5
#define TFT_MOSI 6
#define TFT_SCK  4
#define TFT_CS   7
#define TFT_DC   3
#define TFT_RST  2
#define TFT_BL   1   
#define T_CS     0   

#define I2C_SDA_PIN  8
#define I2C_SCL_PIN  9

const uint16_t kIrLedPin = 10; 
#define DHTPIN       20   
#define RELAY_PIN    21   

#define RELAY_ACTIVE_LOW  false 

#define C_BG        0x0000
#define C_GREEN     0x07E0
#define C_CYAN      0x07FF
#define C_RED       0xF800
#define C_ORANGE    0xFD20
#define C_GRID      0x10A2
#define C_WHITE     0xFFFF
#define C_PURPLE    0x780F
#define C_DARK_GREY 0x31A6
#define C_YELLOW    0xFFE0

const unsigned long DEFAULT_CODE_ON          = 0x1FE48B7;
const unsigned long DEFAULT_CODE_OFF         = 0x1FE7887;
const unsigned long DEFAULT_CODE_BRIGHT_UP   = 0x1FE609F;
const unsigned long DEFAULT_CODE_BRIGHT_DOWN = 0x1FEA05F;

struct Settings {
  uint8_t sleepHour = 22, sleepMinute = 0;
  uint8_t wakeHour = 6, wakeMinute = 0;
  
  unsigned long ir_on = DEFAULT_CODE_ON, ir_off = DEFAULT_CODE_OFF, ir_bright_up = DEFAULT_CODE_BRIGHT_UP, ir_bright_down = DEFAULT_CODE_BRIGHT_DOWN;
  
  char weatherCity[32] = "zhumadian";
  char weatherApiKey[64] = "";
  
  // ⭐️ 新增：Home Assistant 动态磁盘保存配置
  char haHost[32] = "192.168.31.22";
  int haPort = 8123;
  char haEntity[64] = "switch.sonoff_1000a68f48";
  char haToken[256] = ""; // 存放动态 Token

  bool tempCtrlEnabled = false;  
  float tempThreshold = 28.0;    
  float tempThresholdOff = 27.0; 
  
  bool relayTimerEnabled = false;
  uint8_t relayOnHour = 8, relayOnMinute = 0;  
  uint8_t relayOffHour = 22, relayOffMinute = 0;

  float turnOnVoltage = 13.5f;   
  float underVoltage  = 11.5f;   
  float underPower    = 2.0f;    
  uint32_t cooldownSec = 3600UL; 
  
  int magic_key = 80101; 
};

struct SystemState {
  float busVoltage   = 0.0f; 
  float shuntVoltage = 0.0f; 
  float current_mA   = 0.0f; 
  float power_mW     = 0.0f; 

  bool relayOn        = false; 
  bool faultLatched   = false; 
  TripReason tripReason = TripReason::NONE; 
  uint32_t tripEpoch  = 0;     
  uint8_t retryCount  = 0;
  uint8_t confirmCounter = 0;  

  uint32_t todayOnSec     = 0; 
  uint32_t yesterdayOnSec = 0;
  double cumulativeWh     = 0.0;
  char lastLoggedDate[16] = "";

  unsigned long lastSampleMs = 0;
  unsigned long lastSaveMs   = 0;
};

#endif // CONFIG_H