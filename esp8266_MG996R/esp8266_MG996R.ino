#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <Servo.h>  // 引入舵机库

// 1. WiFi 配置
const char* ssid = "yang1234";
const char* password = "y123456789";

// 2. 引脚定义
const int servoPin = 5; // D1 (GPIO 5)

Servo myServo;
ESP8266WebServer server(80);

// 3. 全局变量
int forwardTime = 5;
int backwardTime = 5;

// 保存到 EEPROM
void saveSettings() {
  EEPROM.begin(512);
  EEPROM.put(0, forwardTime);
  EEPROM.put(sizeof(int), backwardTime);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("设置已永久保存");
}

// 从 EEPROM 读取
void loadSettings() {
  EEPROM.begin(512);
  EEPROM.get(0, forwardTime);
  EEPROM.get(sizeof(int), backwardTime);
  if (forwardTime < 0 || forwardTime > 3600) forwardTime = 5;
  if (backwardTime < 0 || backwardTime > 3600) backwardTime = 5;
  EEPROM.end();
}

// 停止舵机
void stopServo() {
  // 360度舵机通常在1500us停止
  myServo.writeMicroseconds(1500); 
  // 可选：detach() 可以彻底防止舵机“吱吱”响或缓慢爬行
  // myServo.detach(); 
}

// 生成网页
String getHTML() {
  String html = "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f7f6; padding:20px;} ";
  html += ".card{background:white; max-width:400px; margin:15px auto; padding:20px; border-radius:12px; box-shadow:0 4px 10px rgba(0,0,0,0.1);} ";
  html += "input{width:60%; padding:10px; margin:10px; border:1px solid #ddd; border-radius:8px;} ";
  html += "button{width:90%; padding:12px; margin:8px; border:none; border-radius:8px; color:white; font-size:16px; cursor:pointer;} ";
  html += ".btn-f{background:#2ecc71;} .btn-b{background:#e74c3c;} .btn-ota{background:#3498db;} </style>";
  html += "<title>舵机门帘控制台</title></head><body>";
  
  html += "<h2>智能门帘舵机控制</h2>";
  
  html += "<div class='card'><h3>门帘操作</h3>";
  html += "<form action='/forward' method='GET'>拉开(秒): <input type='number' name='t' value='" + String(forwardTime) + "'><button class='btn-f'>保存并执行拉开</button></form>";
  html += "<hr>";
  html += "<form action='/backward' method='GET'>合上(秒): <input type='number' name='t' value='" + String(backwardTime) + "'><button class='btn-b'>保存并执行合上</button></form></div>";

  html += "<div class='card' style='background:#ebf5fb;'><h3>系统升级 (OTA)</h3>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update' style='font-size:12px;'><button type='submit' class='btn-ota'>上传新固件 (.bin)</button></form></div>";
  
  html += "<p>IP地址: " + WiFi.localIP().toString() + "</p>";
  html += "</body></html>";
  return html;
}

void setup() {
  Serial.begin(115200);
  loadSettings();

  // 初始不连接舵机，防止通电瞬间乱转
  stopServo();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  MDNS.begin("curtain-control");

  server.on("/", []() {
    server.send(200, "text/html; charset=utf-8", getHTML());
  });

  server.on("/forward", []() {
    if (server.hasArg("t")) { forwardTime = server.arg("t").toInt(); saveSettings(); }
    server.send(200, "text/html; charset=utf-8", "<h1>正在拉开门帘... " + String(forwardTime) + "秒</h1><script>setTimeout(function(){window.location.href='/';}," + String(forwardTime * 1000) + ");</script>");
    
    myServo.attach(servoPin);
    myServo.writeMicroseconds(2000); // 全速旋转（正向）
    delay(forwardTime * 1000);
    stopServo();
  });

  server.on("/backward", []() {
    if (server.hasArg("t")) { backwardTime = server.arg("t").toInt(); saveSettings(); }
    server.send(200, "text/html; charset=utf-8", "<h1>正在合上门帘... " + String(backwardTime) + "秒</h1><script>setTimeout(function(){window.location.href='/';}," + String(backwardTime * 1000) + ");</script>");
    
    myServo.attach(servoPin);
    myServo.writeMicroseconds(1000); // 全速旋转（反向）
    delay(backwardTime * 1000);
    stopServo();
  });

  // OTA 升级路由
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain; charset=utf-8", (Update.hasError()) ? "升级失败" : "升级成功，正在重启...");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("开始升级: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) { 
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("升级完成: %u 字节\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.begin();
}

void loop() {
  server.handleClient();
  MDNS.update();
}