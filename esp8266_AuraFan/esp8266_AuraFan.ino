// =======================================================================
// = ESP8266 智能风扇 & LED & ILI9341 显示屏一体化控制器 v2.6 (单引脚简化版) =
// =======================================================================
//
// 更新日志 (v2.6):
// 1. [硬件简化] 根据用户要求，将两条60灯珠的灯带合并为一条120灯珠的
//    长灯带，统一由 D8 引脚控制。移除了所有关于 D6 引脚的逻辑。
// 2. [代码优化] 大幅简化了 setupLeds() 和 updateLeds() 函数，使其
//    逻辑更清晰、效率更高。
// 3. [功能保留] 完整保留了 v2.5 的所有功能，包括动态首页、屏幕同步等。
//
// 项目名称: AuraFan Display Integrator
// =======================================================================


// --- 核心库 ---
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <FastLED.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// --- C++ 标准库 ---
#include <map>
#include <vector>

// ==================================================================
// ==================== 用户配置 (在这里修改) =======================
// ==================================================================

// --- WiFi 设置 ---
const char* ssid = "yang1234";
const char* password = "y123456789";

// --- 设备名称 ---
const char* deviceName = "esp8266-controller";

// --- 风扇引脚 (AuraFan) ---
const int PWM_PIN = 5;  // D1
const int TACH_PIN = 4; // D2

// --- ✅【重要修改】LED 灯带配置 (简化为单条) ---
#define NUM_STRIPS 1
#define LED_PIN    D8  // 所有灯带现在都由 D8 控制
#define TOTAL_LEDS 120 // 两条 60 灯珠的灯带串联后的总数

// --- ILI9341 显示屏引脚 (已解决冲突) ---
#define TFT_CS   D0
#define TFT_DC   D3
#define TFT_RST  D4
// 硬件 SPI 引脚 (SCK=D5, MOSI=D7) 由硬件决定

// ================== 配置结束, 以下代码无需修改 ==================

// --- 派生配置 ---
const int PWM_FREQUENCY = 25000;
const int PWM_RESOLUTION = 1023;
const bool PWM_INVERTED = false;
CRGB leds[TOTAL_LEDS > 0 ? TOTAL_LEDS : 1];

// ====================== 全局对象和变量 ======================
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// --- AuraFan 全局变量 ---
volatile int pulseCount = 0;
unsigned long lastRpmTime = 0;
int fanSliderValue = 0;
enum LedMode { OFF, STATIC, RAINBOW, FADE };
LedMode currentLedMode = RAINBOW;
CRGB staticColor = CRGB(139, 0, 255);
uint8_t gHue = 0;
long lastLedUpdate = 0;
int numLedsToLight = TOTAL_LEDS;

// --- ILI9341 显示屏全局变量 ---
struct LineData { String text; uint16_t color; uint8_t size; String colorHtml; };
std::map<int, LineData> lines;
struct ImageData { std::vector<uint16_t> pixels; int16_t width; int16_t height; int16_t y_offset; };
ImageData image;
bool useGBR = true;

// ====================== 函数声明 ======================
void updateLeds();
void updateDisplay();
uint16_t parseColor(String colorString);
void handleRoot();

// ====================== 核心功能函数 ======================
void ICACHE_RAM_ATTR tachISR() { pulseCount++; }
int computeRPM() { if (millis() == lastRpmTime) return 0; noInterrupts(); int pulses = pulseCount; pulseCount = 0; interrupts(); unsigned long elapsedTime = millis() - lastRpmTime; lastRpmTime = millis(); return (int)((pulses / 2.0) * 60000.0 / elapsedTime); }

// ✅ [修改] 简化了 updateLeds 函数，直接操作单条灯带
void updateLeds() {
    if (TOTAL_LEDS == 0 || millis() - lastLedUpdate < 20) return;
    lastLedUpdate = millis();

    // 先将所有灯珠清空
    fill_solid(leds, TOTAL_LEDS, CRGB::Black);

    if (currentLedMode != OFF) {
        if (currentLedMode != FADE) {
            FastLED.setBrightness(255);
        }
        gHue++;

        // 根据模式对需要点亮的灯珠应用效果
        switch (currentLedMode) {
            case STATIC:
                fill_solid(leds, numLedsToLight, staticColor);
                break;
            case RAINBOW:
                fill_rainbow(leds, numLedsToLight, gHue, 7);
                break;
            case FADE: {
                uint8_t brightness = (exp(sin(millis()/2000.0*PI)) - 0.36787944)*108.0;
                fill_solid(leds, numLedsToLight, staticColor);
                FastLED.setBrightness(brightness);
                break;
            }
            default: break;
        }
    }

    // PWM暂停法，确保灯光和风扇无冲突
    int dutyCycle = map(fanSliderValue, 0, 255, 0, PWM_RESOLUTION);
    if (PWM_INVERTED) dutyCycle = PWM_RESOLUTION - dutyCycle;
    
    analogWrite(PWM_PIN, PWM_INVERTED ? PWM_RESOLUTION : 0);
    FastLED.show();
    analogWrite(PWM_PIN, dutyCycle);
}

// ====================== Web 路由处理 ======================
void handleRoot() {
    String html = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>一体化控制器</title>
<style>:root{--bg-start:#e0eafc;--bg-end:#cfdef3;--card-bg:rgba(255,255,255,0.65);--text:#3a3a3a;--accent:#8e44ad;--accent-dark:#592a6e;--muted:#5f6c7b;--shadow:rgba(0,0,0,0.1)}*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,'Helvetica Neue',Arial;background:linear-gradient(135deg,var(--bg-start),var(--bg-end));color:var(--text);display:flex;justify-content:center;padding:1.5rem;min-height:100vh}.container{width:100%;max-width:560px}.card{background:var(--card-bg);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);border-radius:1.25rem;padding:1.5rem;margin-bottom:1.25rem;box-shadow:0 8px 32px 0 var(--shadow);border:1px solid rgba(255,255,255,0.2)}h1,h2{margin:0 0 1rem;color:#2c3e50}h1{text-align:center;font-size:1.6rem;font-weight:600}h2{font-size:1.2rem;display:flex;align-items:center;gap:0.5rem}.label{margin:1rem 0 .5rem;font-weight:600}.value{font-feature-settings:'tnum' 1;font-weight:normal;color:var(--muted)}.slider{width:100%;-webkit-appearance:none;height:10px;background:#dcdfe4;border-radius:5px;outline:none;transition:opacity .2s}.slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:24px;height:24px;background:var(--accent);border-radius:50%;cursor:pointer;border:3px solid #fff;box-shadow:0 2px 5px var(--shadow)}.btn-group{display:flex;flex-wrap:wrap;gap:.75rem;margin-top:.8rem}.btn{background:var(--accent);border:none;color:#fff;padding:.8rem 1.2rem;border-radius:.75rem;font-weight:600;cursor:pointer;box-shadow:0 4px 12px rgba(142,68,173,.3);transition:all .2s ease-in-out}.btn:hover{background:var(--accent-dark);transform:translateY(-2px);box-shadow:0 6px 16px rgba(142,68,173,.35)}.btn.active{background:var(--accent-dark);box-shadow:inset 0 2px 4px rgba(0,0,0,.2)}input[type=color]{vertical-align:middle;margin-left:.5rem;width:44px;height:36px;border:1px solid #ddd;padding:2px;background-color:#fff;border-radius:.5rem;cursor:pointer}a{color:var(--accent);text-decoration:none;font-weight:600}a:hover{text-decoration:underline}
#screen-content p{margin:8px 0; font-family:monospace; white-space:pre-wrap; font-weight:bold;}
</style></head><body><div class="container"><h1>🦋 一体化控制器 v2.6</h1>
)HTML";
    html += R"HTML(<div class="card"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"></rect><line x1="3" y1="9" x2="21" y2="9"></line><line x1="9" y1="21" x2="9" y2="9"></line></svg> 当前屏幕内容</h2><div id="screen-content">)HTML";
    if (lines.empty()) {
        html += "<p style='color:#888;'>屏幕当前无内容</p>";
    } else {
        for (auto const& [key, val] : lines) {
            html += "<p style='color:" + val.colorHtml + "; font-size:" + String(val.size * 8) + "px;'>" + val.text + "</p>";
        }
    }
    html += R"HTML(</div></div>)HTML";
    html += R"HTML(<div class="card"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 12c2.76 0 5-2.24 5-5s-2.24-5-5-5-5 2.24-5 5 2.24 5 5 5z"/><path d="M19.78 14.78a2.5 2.5 0 0 0-3.53 0l-1.06 1.06a2.5 2.5 0 0 1-3.53 0l-1.06-1.06a2.5 2.5 0 0 0-3.53 0l-1.06 1.06a2.5 2.5 0 0 0 0 3.53l1.06 1.06a2.5 2.5 0 0 0 3.53 0l1.06-1.06a2.5 2.5 0 0 1 3.53 0l1.06 1.06a2.5 2.5 0 0 0 3.53 0l1.06-1.06a2.5 2.5 0 0 0 0-3.53l-1.06-1.06z"/></svg>风扇控制</h2><div class="label">当前速度: <span id="spd" class="value">--</span></div><input id="fanSlider" class="slider" type="range" min="0" max="255" value="0"><div class="label">当前转速: <span id="rpm" class="value">-- RPM</span></div></div><div class="card"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2.69l.34.34a8 8 0 0 1 0 11.32l-1.06-1.06a6 6 0 0 0 0-8.5l-1.06-1.06-1.06 1.06a6 6 0 0 0 0 8.5L10 14.83a8 8 0 0 1-11.32-11.32l.34-.34L12 15l1-1 1-1-1-1-1-1 1-1z"/></svg>灯光控制</h2><div class="btn-group"><button id="btn_static" class="btn">静态单色</button><input id="colorPicker" type="color" value="#8b00ff"><button id="btn_rainbow" class="btn">彩虹</button><button id="btn_fade" class="btn">呼吸</button><button id="btn_off" class="btn">关灯</button></div><div class="label">点亮灯珠数量: <span id="ledCountVal" class="value">--</span></div><input id="ledCountSlider" class="slider" type="range" min="0" max="120" value="120"></div><div class="card"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>系统管理</h2><div style="line-height:1.6"><p style="margin:4px 0">设备 IP: <strong id="ipAddr">...</strong></p><p style="margin:4px 0">点击 <a href="/update">这里</a> 进入固件更新页面。</p></div></div></div>)HTML";
    html += R"HTML(<script>const spdEl=document.getElementById('spd'),rpmEl=document.getElementById('rpm'),fanSlider=document.getElementById('fanSlider'),ipAddrEl=document.getElementById('ipAddr'),colorPicker=document.getElementById('colorPicker'),btnStatic=document.getElementById('btn_static'),btnRainbow=document.getElementById('btn_rainbow'),btnFade=document.getElementById('btn_fade'),btnOff=document.getElementById('btn_off'),allBtns=[btnStatic,btnRainbow,btnFade,btnOff],ledCountSlider=document.getElementById('ledCountSlider'),ledCountVal=document.getElementById('ledCountVal');function setFanLabel(v){const p=Math.round(v/255*100);spdEl.textContent=`${v} (${p}%)`}function updateButtonState(activeMode){allBtns.forEach(btn=>{btn.id===`btn_${activeMode}`?btn.classList.add('active'):btn.classList.remove('active')})}fanSlider.addEventListener('input',()=>{const v=fanSlider.value;setFanLabel(v);fetch(`/setSpeed?value=${v}`).catch(console.error)});ledCountSlider.addEventListener('input',()=>{const v=ledCountSlider.value;ledCountVal.textContent=v;fetch(`/setActiveLeds?value=${v}`).catch(console.error)});colorPicker.addEventListener('input',()=>{const hex=colorPicker.value;const r=parseInt(hex.slice(1,3),16),g=parseInt(hex.slice(3,5),16),b=parseInt(hex.slice(5,7),16);fetch(`/setRGB?r=${r}&g=${g}&b=${b}`).then(()=>updateButtonState('static')).catch(console.error)});btnStatic.onclick=()=>{colorPicker.dispatchEvent(new Event('input'))};btnRainbow.onclick=()=>{fetch('/setLedMode?mode=rainbow').then(()=>updateButtonState('rainbow')).catch(console.error)};btnFade.onclick=()=>{fetch('/setLedMode?mode=fade').then(()=>updateButtonState('fade')).catch(console.error)};btnOff.onclick=()=>{fetch('/setLedMode?mode=off').then(()=>updateButtonState('off')).catch(console.error)};function fetchAllState(){fetch('/getState').then(r=>r.json()).then(s=>{fanSlider.value=s.fanSpeed;setFanLabel(s.fanSpeed);ipAddrEl.textContent=s.ip;const c=`#${s.ledR.toString(16).padStart(2,'0')}${s.ledG.toString(16).padStart(2,'0')}${s.ledB.toString(16).padStart(2,'0')}`;colorPicker.value=c;updateButtonState(s.ledMode);ledCountSlider.max=s.maxLeds;ledCountSlider.value=s.activeLeds;ledCountVal.textContent=s.activeLeds}).catch(console.error)}window.addEventListener('load',fetchAllState);setInterval(()=>{fetch('/getRPM').then(r=>r.text()).then(t=>{rpmEl.textContent=`${t} RPM`}).catch(console.error)},1500);</script></body></html>)HTML";
    server.send(200, "text/html; charset=UTF-8", html);
}
void handleSetSpeed() { if (server.hasArg("value")) { fanSliderValue = server.arg("value").toInt(); server.send(200, "text/plain", "OK"); } else { server.send(400, "text/plain", "Bad Request"); }}
void handleGetRPM() { server.send(200, "text/plain", String(computeRPM())); }
void handleSetRGB() { if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) { staticColor = CRGB(server.arg("r").toInt(), server.arg("g").toInt(), server.arg("b").toInt()); currentLedMode = STATIC; server.send(200, "text/plain", "OK"); } else { server.send(400, "text/plain", "Bad Request"); }}
void handleSetActiveLeds() { if (server.hasArg("value")) { numLedsToLight = server.arg("value").toInt(); server.send(200, "text/plain", "OK"); } else { server.send(400, "text/plain", "Bad Request"); }}
void handleSetLedMode() { if (server.hasArg("mode")) { String mode = server.arg("mode"); if (mode == "off") currentLedMode = OFF; else if (mode == "static") currentLedMode = STATIC; else if (mode == "rainbow") currentLedMode = RAINBOW; else if (mode == "fade") currentLedMode = FADE; server.send(200, "text/plain", "OK"); } else { server.send(400, "text/plain", "Bad Request"); }}
void handleGetState() { String modeStr = "off"; if (currentLedMode == STATIC) modeStr = "static"; else if (currentLedMode == RAINBOW) modeStr = "rainbow"; else if (currentLedMode == FADE) modeStr = "fade"; String json = "{\"fanSpeed\":" + String(fanSliderValue) + ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"ledMode\":\"" + modeStr + "\",\"ledR\":" + String(staticColor.r) + ",\"ledG\":" + String(staticColor.g) + ",\"ledB\":" + String(staticColor.b) + ",\"activeLeds\":" + String(numLedsToLight) + ",\"maxLeds\":" + String(TOTAL_LEDS) + "}"; server.send(200, "application/json", json); }
void handleSetText() { lines.clear(); for (int i = 0; i < 100; i++) { String lineKey = "line" + String(i); if (server.hasArg(lineKey)) { LineData line; line.text = server.arg(lineKey); String colorKey = "color" + String(i); if(server.hasArg(colorKey)){ line.colorHtml = server.arg(colorKey); } else { line.colorHtml = "#FFFFFF"; } line.color = parseColor(line.colorHtml); int size = server.hasArg("size" + String(i)) ? server.arg("size" + String(i)).toInt() : 2; line.size = (size >= 1 && size <= 7) ? size : 2; lines[i] = line; } } updateDisplay(); server.send(200, "text/plain", "OK"); }
void handleImage() { if (server.hasArg("width") && server.hasArg("height") && server.hasArg("data")) { int width = server.arg("width").toInt(); int height = server.arg("height").toInt(); String data = server.arg("data"); if (width > 0 && height > 0 && data.length() > 0) { image.width = width; image.height = height; image.pixels.clear(); image.pixels.reserve(width * height); for (unsigned int i = 0; i + 4 <= data.length(); i += 4) { String hexColor = data.substring(i, i + 4); image.pixels.push_back(strtoul(hexColor.c_str(), NULL, 16)); } image.y_offset = server.hasArg("y_offset") ? server.arg("y_offset").toInt() : 0; updateDisplay(); server.send(200, "text/plain", "Image OK"); } else { server.send(400, "text/plain", "Invalid image parameters"); } } else { server.send(400, "text/plain", "Image parameters missing"); } }
void handleNotFound() { server.send(404, "text/plain", "404: Not Found"); }

// ====================== 显示屏辅助函数 ======================
void updateDisplay() {
    tft.fillScreen(ILI9341_BLACK);
    int16_t yPos = 20;
    for (auto& [lineNum, line] : lines) {
        int16_t xPos = 20;
        tft.setCursor(xPos, yPos);
        tft.setTextColor(line.color);
        tft.setTextSize(line.size);
        tft.println(line.text);
        yPos += line.size * 6 * (tft.getRotation() % 2 == 0 ? 1 : 2) + 15;
    }
    if (!image.pixels.empty()) {
        int16_t startY = yPos + image.y_offset;
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                if (x < tft.width() && (startY + y) < tft.height()) {
                    tft.drawPixel(x, startY + y, image.pixels[y * image.width + x]);
                }
            }
        }
    }
}
uint16_t parseColor(String colorString) {
    if (colorString == "red") return ILI9341_RED;
    if (colorString == "green") return ILI9341_GREEN;
    if (colorString == "blue") return ILI9341_BLUE;
    if (colorString == "yellow") return ILI9341_YELLOW;
    if (colorString == "cyan") return ILI9341_CYAN;
    if (colorString == "magenta") return ILI9341_MAGENTA;
    if (colorString == "white") return ILI9341_WHITE;
    if (colorString == "black") return ILI9341_BLACK;
    if (colorString == "orange") return ILI9341_ORANGE;
    if (colorString.startsWith("#") && colorString.length() == 7) {
        long number = strtol(colorString.substring(1).c_str(), NULL, 16);
        uint8_t r = (number >> 16) & 0xFF;
        uint8_t g = (number >> 8) & 0xFF;
        uint8_t b = number & 0xFF;
        if (useGBR) { return ((g & 0xF8) << 8) | ((b & 0xFC) << 3) | (r >> 3); } 
        else { return tft.color565(r, g, b); }
    }
    return ILI9341_WHITE;
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== AuraFan & Display Controller v2.6 (Single Pin) =====");

  pinMode(PWM_PIN, OUTPUT);
  analogWriteFreq(PWM_FREQUENCY);
  analogWrite(PWM_PIN, PWM_INVERTED ? PWM_RESOLUTION : 0);
  pinMode(TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachISR, FALLING);
  lastRpmTime = millis();
  Serial.println("Fan system initialized.");

  // ✅ [修改] 简化了LED初始化
  if (TOTAL_LEDS > 0) {
      FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, TOTAL_LEDS);
      FastLED.clear(true);
      Serial.println("Single FastLED strip initialized.");
  }

  tft.begin();
  SPI.setFrequency(1000000);
  tft.setRotation(0);
  uint8_t caset_data[] = {0x00, 0x00, 0x00, 0xEF};
  tft.sendCommand(ILI9341_CASET, caset_data, 4);
  uint8_t raset_data[] = {0x00, 0x00, 0x01, 0x3F};
  tft.sendCommand(ILI9341_PASET, raset_data, 4);
  tft.writeCommand(ILI9341_RAMWR);
  uint8_t madctl_data = 0xE8;
  tft.sendCommand(ILI9341_MADCTL, &madctl_data, 1);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 20);
  tft.println("Controller Booting...");
  Serial.println("ILI9341 display initialized CORRECTLY.");

  WiFi.hostname(deviceName);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  tft.setCursor(10, 50);
  tft.print("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println("\nConnected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());

  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(10, 20);
  tft.setTextSize(2);
  tft.println("System Ready!");
  tft.setTextSize(1);
  tft.setCursor(10, 50);
  tft.print("Web UI & API at:");
  tft.setCursor(10, 70);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_CYAN);
  tft.println(WiFi.localIP());

  if (MDNS.begin(deviceName)) { MDNS.addService("http", "tcp", 80); Serial.printf("mDNS Responder started. Access at: http://%s.local\n", deviceName); }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/setSpeed", HTTP_GET, handleSetSpeed);
  server.on("/getRPM", HTTP_GET, handleGetRPM);
  server.on("/setRGB", HTTP_GET, handleSetRGB);
  server.on("/setLedMode", HTTP_GET, handleSetLedMode);
  server.on("/setActiveLeds", HTTP_GET, handleSetActiveLeds);
  server.on("/getState", HTTP_GET, handleGetState);
  server.on("/set", HTTP_POST, handleSetText);
  server.on("/image", HTTP_POST, handleImage);
  server.onNotFound(handleNotFound);
  
  httpUpdater.setup(&server);
  server.begin();
  Serial.println("HTTP server started with all routes.");
}

// ====================== LOOP ======================
void loop() {
  server.handleClient();
  MDNS.update();
  updateLeds();
}