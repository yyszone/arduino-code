/*
 * =================================================================================
 * ==                                                                             ==
 * ==         ESP8266 & INA219 最终版实时监控网页服务器 (内置硬件自检)            ==
 * ==                                                                             ==
 * =================================================================================
 * 
 * 本代码是包含了您需要的所有功能的最终版本：
 * 1. 【硬件自检】: 在程序启动时，会首先检查与INA219的通信。如果失败，会报错并停止，
 *    强制我们解决最根本的硬件连接问题。
 * 2. 【WiFi连接】: 硬件自检通过后，会自动连接到您指定的WiFi网络。
 * 3. 【网页服务器】: 提供一个可以通过IP地址访问的、每半秒自动刷新的监控网页。
 * 
 * =================================================================================
 * ==                             成功的唯一前提：正确接线                           ==
 * =================================================================================
 * 
 * I2C通信线路 (这是成败的关键，请最后一次仔细检查):
 * ----------------------------------------------------
 * | ESP8266 引脚 | 连接到 | INA219 引脚 | 功能         |
 * |--------------|----------|-------------|--------------|
 * |      3V3     |  ----->  |     VCC     | 传感器供电   |
 * |      GND     |  ----->  |     GND     | 公共地线     |
 * |      D1      |  ----->  |     SCL     | I2C 时钟线   |
 * |      D2      |  ----->  |     SDA     | I2C 数据线   |
 * ----------------------------------------------------
 * 
 * 主电源测量线路:
 * - [电源 +] ---> [INA219 Vin+]
 * - [INA219 Vin-] ---> [负载(风扇) +]
 * - [负载(风扇) -] ---> [电源 -]
 * 
 */

// 引用所有必需的库
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// --- WiFi凭证 ---
const char* ssid = "yang1234";
const char* password = "y123456789";

// --- 创建所需的对象 ---
ESP8266WebServer server(80);
Adafruit_INA219 ina219;

// --- 全局变量 ---
float loadVoltage = 0, current_mA = 0, power_mW = 0;

/**
 * @brief 进行n次测量并取平均值，使结果更平滑
 */
void sampleAveraged(uint8_t n = 5) {
  float sum_v = 0, sum_c = 0, sum_p = 0;
  for (uint8_t i = 0; i < n; i++) {
    float busV = ina219.getBusVoltage_V();
    float shunt_mV = ina219.getShuntVoltage_mV();
    sum_v += busV + (shunt_mV / 1000.0);
    sum_c += ina219.getCurrent_mA();
    sum_p += ina219.getPower_mW();
    delay(5);
  }
  loadVoltage = sum_v / n;
  current_mA = sum_c / n;
  power_mW = sum_p / n;
}

// =================================================
// ==             程序初始化设置部分              ==
// =================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== 最终版监控程序启动 =====");

  // =================================================================
  // ==                      内置硬件自检 (最关键的一步)                      ==
  // =================================================================
  Serial.println("步骤 1: 正在进行硬件自检，尝试与INA219通信...");
  if (!ina219.begin()) {
    // 如果ina219.begin()返回false，说明硬件通信失败
    Serial.println("\n!!! 自检失败: 致命硬件错误 !!!");
    Serial.println("!!! 未能找到INA219芯片。网页将无法工作。");
    Serial.println("!!! 这100%是物理连接问题，与代码无关。");
    Serial.println("\n唯一的解决方案:");
    Serial.println("  1. 仔细检查 VCC, GND, SCL, SDA 的引脚是否插对、插紧。");
    Serial.println("  2. 【请立刻更换全部四根杜邦线再试！】这是最常见但最易被忽略的故障点。");
    Serial.println("\n程序将在此停止运行，直到硬件问题被解决。");
    while (1) { // 程序在此处无限循环，停止一切后续操作
      delay(1000);
    }
  }
  
  // 如果程序能运行到这里，说明硬件自检通过！
  Serial.println(">>> 自检成功！硬件通信已建立。");
  ina219.setCalibration_32V_2A();
  Serial.println("INA219 初始化成功。");

  // --- 步骤 2: WiFi 连接 ---
  Serial.println("\n步骤 2: 正在连接到WiFi网络...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("网络名称: %s\n", ssid);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\n>>> WiFi连接成功！IP地址: %s\n", WiFi.localIP().toString().c_str());

  // --- 步骤 3: Web 服务器配置 ---
  server.on("/", [](){
    String html = R"rawliteral(
<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 INA219</title>
<style>body{font-family:Arial,sans-serif;text-align:center;margin:0;background:#f0f2f5} h2{color:#333} .card{display:inline-block;padding:20px;margin:10px;background:#fff;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1)} .val{font-size:2.5em;font-weight:bold}</style>
</head><body>
<h2>ESP8266 & INA219 实时监控</h2>
<div class="card"><h3>负载电压</h3><span class="val" id="v">--</span> V</div>
<div class="card"><h3>电流</h3><span class="val" id="c">--</span> mA</div>
<div class="card"><h3>功率</h3><span class="val" id="p">--</span> mW</div>
<script>
setInterval(() => {
  fetch('/data').then(response => response.json()).then(data => {
    document.getElementById('v').textContent = data.voltage.toFixed(2);
    document.getElementById('c').textContent = data.current.toFixed(1);
    document.getElementById('p').textContent = data.power.toFixed(0);
  });
}, 500);
</script></body></html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/data", [](){
    sampleAveraged(5);
    String json = "{";
    json += "\"voltage\":" + String(loadVoltage) + ",";
    json += "\"current\":" + String(current_mA) + ",";
    json += "\"power\":" + String(power_mW);
    json += "}";
    server.send(200, "application/json", json);
  });

  server.begin();
  Serial.println(">>> Web服务器已启动。您现在可以通过IP地址访问监控页面了。");
  Serial.println("========================================\n");
}

// =================================================
// ==              主循环 (持续运行)              ==
// =================================================
void loop() {
  server.handleClient(); // 持续处理网页客户端的请求
}