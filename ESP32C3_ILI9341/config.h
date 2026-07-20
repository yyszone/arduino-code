#ifndef CONFIG_H
#define CONFIG_H

// ===============================================================
// ==================== 用户自定义配置区域 =========================
// ===============================================================
const char* ssid = "yang1234";
const char* password = "y123456789";
const unsigned long standbyDelay = 60000; // 60秒无操作后进入待机模式

// Home Assistant 配置
const char* ha_host = "192.168.31.22";
const int ha_port = 8123;
const char* ha_token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiIwYjU4YTMwOWMzNmE0ZDE2ODBjOGI2MzI4YzAwMTlkZCIsImlhdCI6MTc1ODk3NDgwMCwiZXhwIjoyMDc0MzM0ODAwfQ.e1e_iE6iIpdB2EG0d0VXZcb5bjePSoI8m8qTDEFTJ-w";
const char* ha_entity_id = "switch.sonoff_1000a68f48";

// HTTP 控制配置
const char* led_on_url = "http://192.168.31.162/LED-Control?ledPwm=3";
const char* led_off_url = "http://192.168.31.162/LED-Control?ledPwm=4";

// 配置文件存储路径
const char* configFile = "/config.json"; 


// ===============================================================
// ==================== ESP32-C3 引脚重新定义 ====================
// ===============================================================
#define TFT_MISO 5
#define TFT_MOSI 6
#define TFT_SCK  4

#define TFT_CS   7
#define TFT_DC   3
#define TFT_RST  2
#define TFT_BL   1   // 屏幕背光控制引脚
#define T_CS     0

const uint16_t kIrLedPin = 10; // 红外发射引脚 (GPIO 10)

// ==================== 物理引脚与触发电平优化 ====================
#define DHTPIN       20   // 温湿度信号线接 GPIO 20 
#define RELAY_PIN    21   // 继电器控制线接 GPIO 21 

// 高电平触发继电器，开机默认低电平（安全关闭）
#define RELAY_ACTIVE_LOW  false  

// ============== 赛博风格配色定义 ==============
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

// 默认红外编码
const unsigned long DEFAULT_CODE_ON          = 0x1FE48B7;
const unsigned long DEFAULT_CODE_OFF         = 0x1FE7887;
const unsigned long DEFAULT_CODE_BRIGHT_UP   = 0x1FE609F;
const unsigned long DEFAULT_CODE_BRIGHT_DOWN = 0x1FEA05F;
const unsigned long DEFAULT_CODE_AUTO        = 0x1FE807F;
const unsigned long DEFAULT_CODE_TIMER_3H    = 0x1FE58A7;
const unsigned long DEFAULT_CODE_TIMER_5H    = 0x1FE40BF;
const unsigned long DEFAULT_CODE_TIMER_8H    = 0x1FEC03F;

// ============== 永久保存设置结构体 ==============
struct Settings {
  uint8_t sleepHour = 22, sleepMinute = 0;
  uint8_t wakeHour = 6, wakeMinute = 0;
  unsigned long ir_on, ir_off, ir_bright_up, ir_bright_down, ir_auto, ir_timer_3h, ir_timer_5h, ir_timer_8h;
  char weatherCity[32];
  char weatherApiKey[64];
  
  // 温控保存项
  bool tempCtrlEnabled = false;   // 是否启用温控
  float tempThreshold = 28.0;     // 新增：温度控制“开启”阈值
  float tempThresholdOff = 27.0;  // 新增：温度控制“关闭”阈值
  
  int magic_key = 80101; 
};

#endif