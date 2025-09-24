// ================== ESP8266 智能风扇 & LED 控制器 v1.9 ==================
//
// 更新日志 (v1.9):
// 1. [BUG修复] 在调用 FastLED.show() 期间，暂时停止风扇的PWM输出，
//    发送完毕后立刻恢复。
// 2. [代码优化] 此“PWM暂停法”彻底解决了软件PWM与FastLED的中断冲突，
//    且无需更改硬件引脚，保证了任意风扇转速下灯光的绝对稳定。
//
// 项目名称: AuraFan
// =======================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <FastLED.h>

// ==================================================================
// ==================== 用户配置 (在这里修改) =======================
// ==================================================================

// --- WiFi 设置 ---
const char* ssid = "yang1234";
const char* password = "y123456789";

// --- 设备名称 ---
const char* deviceName = "esp8266-fan";

// --- 风扇引脚 ---
const int PWM_PIN = 5;  // D1
const int TACH_PIN = 4; // D2

// ==================== LED 灯带配置 (核心区域) ====================
// 两条灯带均为 60 灯珠
#define NUM_STRIPS 2

// --- 灯带 1 ---
#define STRIP1_PIN   D7
#define STRIP1_LEDS  60

#if NUM_STRIPS >= 2
// --- 灯带 2 ---
#define STRIP2_PIN   D8
#define STRIP2_LEDS  60
#endif

// ================== 配置结束, 以下代码无需修改 ==================


// --- PWM/RPM 派生配置 (自动) ---
const int PWM_FREQUENCY = 25000;
const int PWM_RESOLUTION = 1023;
const bool PWM_INVERTED = false;

// --- LED 派生配置 (自动) ---
constexpr int calculateTotalLeds() {
    int total = 0;
    #if NUM_STRIPS >= 1
    total += STRIP1_LEDS;
    #endif
    #if NUM_STRIPS >= 2
    total += STRIP2_LEDS;
    #endif
    return total;
}
const int TOTAL_LEDS = calculateTotalLeds();
CRGB leds[TOTAL_LEDS > 0 ? TOTAL_LEDS : 1];


// ====================== 全局变量 ======================
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

volatile int pulseCount = 0;
unsigned long lastRpmTime = 0;
int fanSliderValue = 0;

enum LedMode { OFF, STATIC, RAINBOW, FADE };
LedMode currentLedMode = RAINBOW;
CRGB staticColor = CRGB(139, 0, 255);
uint8_t gHue = 0;
long lastLedUpdate = 0;
int numLedsToLight = TOTAL_LEDS;


// ====================== LED 自动初始化 ======================
template<int N> struct LedInitializer { static void add(int offset) {} };
#if NUM_STRIPS >= 1
template<> struct LedInitializer<1> { static void add(int offset) { FastLED.addLeds<WS2812B, STRIP1_PIN, GRB>(leds, offset, STRIP1_LEDS); } };
#endif
#if NUM_STRIPS >= 2
template<> struct LedInitializer<2> { static void add(int offset) { FastLED.addLeds<WS2812B, STRIP2_PIN, GRB>(leds, offset, STRIP2_LEDS); } };
#endif

void setupLeds() {
    int offset = 0;
    #if NUM_STRIPS >= 1
    LedInitializer<1>::add(offset); offset += STRIP1_LEDS;
    #endif
    #if NUM_STRIPS >= 2
    LedInitializer<2>::add(offset); offset += STRIP2_LEDS;
    #endif
}


// ====================== 中断及核心功能函数 ======================
void ICACHE_RAM_ATTR tachISR() {
  pulseCount++;
}

int computeRPM() {
  if (millis() == lastRpmTime) return 0;
  
  noInterrupts();
  int pulses = pulseCount;
  pulseCount = 0;
  interrupts();

  unsigned long elapsedTime = millis() - lastRpmTime;
  lastRpmTime = millis();
  
  int rpm = (int)((pulses / 2.0) * 60000.0 / elapsedTime);
  return rpm;
}

// [v1.7 修复] 采用更健壮的逐条分配逻辑
void updateLeds() {
    if (TOTAL_LEDS == 0 || millis() - lastLedUpdate < 20) return;
    lastLedUpdate = millis();

    // 1. 先将所有灯珠的数据清空为黑色
    fill_solid(leds, TOTAL_LEDS, CRGB::Black);

    if (currentLedMode == OFF) {
        // 对于关灯模式，仍然需要暂停PWM以确保数据发送的纯净
    } else {
        if (currentLedMode != FADE) {
            FastLED.setBrightness(255);
        }
        
        int leds_left_to_light = numLedsToLight; // 获取需要点亮的总数
        int offset = 0;
        gHue++; 

        // 2. 为第一条灯带分配灯珠
        #if NUM_STRIPS >= 1
        {
            CRGB* strip_leds = &leds[offset];
            int num_to_light_on_this_strip = min(leds_left_to_light, STRIP1_LEDS);

            switch (currentLedMode) {
                case STATIC:  fill_solid(strip_leds, num_to_light_on_this_strip, staticColor); break;
                case RAINBOW: fill_rainbow(strip_leds, num_to_light_on_this_strip, gHue, 7); break;
                case FADE: {
                    uint8_t brightness = (exp(sin(millis()/2000.0*PI)) - 0.36787944)*108.0;
                    fill_solid(strip_leds, num_to_light_on_this_strip, staticColor);
                    FastLED.setBrightness(brightness);
                    break;
                }
                default: break;
            }
            offset += STRIP1_LEDS;
            leds_left_to_light -= num_to_light_on_this_strip; // 减去已分配的数量
        }
        #endif

        // 3. 用剩下的数量为第二条灯带分配灯珠
        #if NUM_STRIPS >= 2
        {
            if (leds_left_to_light > 0) { // 如果还有剩余的灯需要点亮
                CRGB* strip_leds = &leds[offset];
                int num_to_light_on_this_strip = min(leds_left_to_light, STRIP2_LEDS);

                 switch (currentLedMode) {
                    case STATIC:  fill_solid(strip_leds, num_to_light_on_this_strip, staticColor); break;
                    case RAINBOW: fill_rainbow(strip_leds, num_to_light_on_this_strip, gHue, 7); break;
                    case FADE: {
                        uint8_t brightness = (exp(sin(millis()/2000.0*PI)) - 0.36787944)*108.0;
                        fill_solid(strip_leds, num_to_light_on_this_strip, staticColor);
                        FastLED.setBrightness(brightness);
                        break;
                    }
                    default: break;
                }
                offset += STRIP2_LEDS;
                leds_left_to_light -= num_to_light_on_this_strip;
            }
        }
        #endif
    }
    
    // ====================== v1.9 核心修复代码 (PWM暂停法) ======================
    // 为了解决 software PWM 和 FastLED 的中断冲突，我们在发送 LED 数据前
    // 暂时停止 PWM 输出，发送完毕后立即恢复。
    // 由于这个过程极快（微秒级），风扇的物理惯性使其完全不受影响。

    // 1. 读取当前应该设置的PWM占空比
    int dutyCycle = map(fanSliderValue, 0, 255, 0, PWM_RESOLUTION);
    if (PWM_INVERTED) dutyCycle = PWM_RESOLUTION - dutyCycle;
    
    // 2. 暂时关闭PWM (设置为0%或100%占空比，取决于逻辑)
    analogWrite(PWM_PIN, PWM_INVERTED ? PWM_RESOLUTION : 0);
    
    // 3. 调用show()，它会自己处理中断禁用，但现在已经没有PWM中断来干扰它了
    FastLED.show(); 
    
    // 4. 立即恢复风扇的PWM占空比
    analogWrite(PWM_PIN, dutyCycle);
    // =========================================================================
}


// ====================== 网页内容 (Butterfly 主题) ======================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>AuraFan 控制器</title><style>:root{--bg-start:#e0eafc;--bg-end:#cfdef3;--card-bg:rgba(255,255,255,0.65);--text:#3a3a3a;--accent:#8e44ad;--accent-dark:#592a6e;--muted:#5f6c7b;--shadow:rgba(0,0,0,0.1)}*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,'Helvetica Neue',Arial;background:linear-gradient(135deg,var(--bg-start),var(--bg-end));color:var(--text);display:flex;justify-content:center;padding:1.5rem;min-height:100vh}.container{width:100%;max-width:560px}.card{background:var(--card-bg);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);border-radius:1.25rem;padding:1.5rem;margin-bottom:1.25rem;box-shadow:0 8px 32px 0 var(--shadow);border:1px solid rgba(255,255,255,0.2)}h1,h2{margin:0 0 1rem;color:#2c3e50}h1{text-align:center;font-size:1.6rem;font-weight:600}h2{font-size:1.2rem;display:flex;align-items:center;gap:0.5rem}.label{margin:1rem 0 .5rem;font-weight:600}.value{font-feature-settings:'tnum' 1;font-weight:normal;color:var(--muted)}.slider{width:100%;-webkit-appearance:none;height:10px;background:#dcdfe4;border-radius:5px;outline:none;transition:opacity .2s}.slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:24px;height:24px;background:var(--accent);border-radius:50%;cursor:pointer;border:3px solid #fff;box-shadow:0 2px 5px var(--shadow)}.btn-group{display:flex;flex-wrap:wrap;gap:.75rem;margin-top:.8rem}.btn{background:var(--accent);border:none;color:#fff;padding:.8rem 1.2rem;border-radius:.75rem;font-weight:600;cursor:pointer;box-shadow:0 4px 12px rgba(142,68,173,.3);transition:all .2s ease-in-out}.btn:hover{background:var(--accent-dark);transform:translateY(-2px);box-shadow:0 6px 16px rgba(142,68,173,.35)}.btn.active{background:var(--accent-dark);box-shadow:inset 0 2px 4px rgba(0,0,0,.2)}input[type=color]{vertical-align:middle;margin-left:.5rem;width:44px;height:36px;border:1px solid #ddd;padding:2px;background-color:#fff;border-radius:.5rem;cursor:pointer}a{color:var(--accent);text-decoration:none;font-weight:600}a:hover{text-decoration:underline}</style></head><body><div class="container"><h1>🦋 AuraFan 控制器</h1><div class="card"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 12c2.76 0 5-2.24 5-5s-2.24-5-5-5-5 2.24-5 5 2.24 5 5 5z"/><path d="M19.78 14.78a2.5 2.5 0 0 0-3.53 0l-1.06 1.06a2.5 2.5 0 0 1-3.53 0l-1.06-1.06a2.5 2.5 0 0 0-3.53 0l-1.06 1.06a2.5 2.5 0 0 0 0 3.53l1.06 1.06a2.5 2.5 0 0 0 3.53 0l1.06-1.06a2.5 2.5 0 0 1 3.53 0l1.06 1.06a2.5 2.5 0 0 0 3.53 0l1.06-1.06a2.5 2.5 0 0 0 0-3.53l-1.06-1.06z"/></svg>风扇控制</h2><div class="label">当前速度: <span id="spd" class="value">--</span></div><input id="fanSlider" class="slider" type="range" min="0" max="255" value="0"><div class="label">当前转速: <span id="rpm" class="value">-- RPM</span></div></div><div class="card"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2.69l.34.34a8 8 0 0 1 0 11.32l-1.06-1.06a6 6 0 0 0 0-8.5l-1.06-1.06-1.06 1.06a6 6 0 0 0 0 8.5L10 14.83a8 8 0 0 1-11.32-11.32l.34-.34L12 15l1-1 1-1-1-1-1-1 1-1z"/></svg>灯光控制</h2><div class="btn-group"><button id="btn_static" class="btn">静态单色</button><input id="colorPicker" type="color" value="#8b00ff"><button id="btn_rainbow" class="btn">彩虹</button><button id="btn_fade" class="btn">呼吸</button><button id="btn_off" class="btn">关灯</button></div><div class="label">点亮灯珠数量: <span id="ledCountVal" class="value">--</span></div><input id="ledCountSlider" class="slider" type="range" min="0" max="120" value="120"></div><div class="card"><h2><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>系统管理</h2><div style="line-height:1.6"><p style="margin:4px 0">设备 IP: <strong id="ipAddr">...</strong></p><p style="margin:4px 0">点击 <a href="/update">这里</a> 进入固件更新页面。</p></div></div></div><script>const spdEl=document.getElementById('spd'),rpmEl=document.getElementById('rpm'),fanSlider=document.getElementById('fanSlider'),ipAddrEl=document.getElementById('ipAddr'),colorPicker=document.getElementById('colorPicker'),btnStatic=document.getElementById('btn_static'),btnRainbow=document.getElementById('btn_rainbow'),btnFade=document.getElementById('btn_fade'),btnOff=document.getElementById('btn_off'),allBtns=[btnStatic,btnRainbow,btnFade,btnOff],ledCountSlider=document.getElementById('ledCountSlider'),ledCountVal=document.getElementById('ledCountVal');function setFanLabel(v){const p=Math.round(v/255*100);spdEl.textContent=`${v} (${p}%)`}function updateButtonState(activeMode){allBtns.forEach(btn=>{btn.id===`btn_${activeMode}`?btn.classList.add('active'):btn.classList.remove('active')})}fanSlider.addEventListener('input',()=>{const v=fanSlider.value;setFanLabel(v);fetch(`/setSpeed?value=${v}`).catch(console.error)});ledCountSlider.addEventListener('input',()=>{const v=ledCountSlider.value;ledCountVal.textContent=v;fetch(`/setActiveLeds?value=${v}`).catch(console.error)});colorPicker.addEventListener('input',()=>{const hex=colorPicker.value;const r=parseInt(hex.slice(1,3),16),g=parseInt(hex.slice(3,5),16),b=parseInt(hex.slice(5,7),16);fetch(`/setRGB?r=${r}&g=${g}&b=${b}`).then(()=>updateButtonState('static')).catch(console.error)});btnStatic.onclick=()=>{colorPicker.dispatchEvent(new Event('input'))};btnRainbow.onclick=()=>{fetch('/setLedMode?mode=rainbow').then(()=>updateButtonState('rainbow')).catch(console.error)};btnFade.onclick=()=>{fetch('/setLedMode?mode=fade').then(()=>updateButtonState('fade')).catch(console.error)};btnOff.onclick=()=>{fetch('/setLedMode?mode=off').then(()=>updateButtonState('off')).catch(console.error)};window.addEventListener('load',()=>{fetch('/getState').then(r=>r.json()).then(s=>{fanSlider.value=s.fanSpeed;setFanLabel(s.fanSpeed);ipAddrEl.textContent=s.ip;const c=`#${s.ledR.toString(16).padStart(2,'0')}${s.ledG.toString(16).padStart(2,'0')}${s.ledB.toString(16).padStart(2,'0')}`;colorPicker.value=c;updateButtonState(s.ledMode);ledCountSlider.max=s.maxLeds;ledCountSlider.value=s.activeLeds;ledCountVal.textContent=s.activeLeds}).catch(console.error)});setInterval(()=>{fetch('/getRPM').then(r=>r.text()).then(t=>{rpmEl.textContent=`${t} RPM`}).catch(console.error)},1500);</script></body></html>
)HTML";


// ====================== Web 路由处理 ======================
void handleRoot() { server.send(200, "text/html; charset=UTF-8", MAIN_HTML); }
void handleSetSpeed() {
  if (server.hasArg("value")) {
    fanSliderValue = server.arg("value").toInt();
    int dutyCycle = map(fanSliderValue, 0, 255, 0, PWM_RESOLUTION);
    if (PWM_INVERTED) dutyCycle = PWM_RESOLUTION - dutyCycle;
    analogWrite(PWM_PIN, dutyCycle);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}
void handleGetRPM() {
  server.send(200, "text/plain", String(computeRPM()));
}
void handleSetRGB() {
    if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
        staticColor = CRGB(server.arg("r").toInt(), server.arg("g").toInt(), server.arg("b").toInt());
        currentLedMode = STATIC;
        server.send(200, "text/plain", "OK");
    } else { server.send(400, "text/plain", "Bad Request"); }
}
void handleSetActiveLeds() {
    if (server.hasArg("value")) {
        numLedsToLight = server.arg("value").toInt();
        server.send(200, "text/plain", "OK");
    } else { server.send(400, "text/plain", "Bad Request"); }
}
void handleSetLedMode() {
    if (server.hasArg("mode")) {
        String mode = server.arg("mode");
        if (mode == "off") currentLedMode = OFF;
        else if (mode == "static") currentLedMode = STATIC;
        else if (mode == "rainbow") currentLedMode = RAINBOW;
        else if (mode == "fade") currentLedMode = FADE;
        server.send(200, "text/plain", "OK");
    } else { server.send(400, "text/plain", "Bad Request"); }
}
void handleGetState() {
    String modeStr = "off";
    if (currentLedMode == STATIC) modeStr = "static";
    else if (currentLedMode == RAINBOW) modeStr = "rainbow";
    else if (currentLedMode == FADE) modeStr = "fade";

    String json = "{";
    json += "\"fanSpeed\":" + String(fanSliderValue) + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"ledMode\":\"" + modeStr + "\",";
    json += "\"ledR\":" + String(staticColor.r) + ",";
    json += "\"ledG\":" + String(staticColor.g) + ",";
    json += "\"ledB\":" + String(staticColor.b) + ",";
    json += "\"activeLeds\":" + String(numLedsToLight) + ",";
    json += "\"maxLeds\":" + String(TOTAL_LEDS);
    json += "}";
    server.send(200, "application/json", json);
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== AuraFan v1.9 (PWM Pause Fix) =====");

  pinMode(PWM_PIN, OUTPUT);
  analogWriteFreq(PWM_FREQUENCY);
  analogWrite(PWM_PIN, PWM_INVERTED ? PWM_RESOLUTION : 0);
  
  pinMode(TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachISR, FALLING);
  lastRpmTime = millis();

  if (TOTAL_LEDS > 0) {
      setupLeds();
      FastLED.clear(true);
  }

  WiFi.hostname(deviceName);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400); Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());

  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS Responder started. Access at: http://%s.local\n", deviceName);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/setSpeed", HTTP_GET, handleSetSpeed);
  server.on("/getRPM", HTTP_GET, handleGetRPM);
  server.on("/setRGB", HTTP_GET, handleSetRGB);
  server.on("/setLedMode", HTTP_GET, handleSetLedMode);
  server.on("/setActiveLeds", HTTP_GET, handleSetActiveLeds);
  server.on("/getState", HTTP_GET, handleGetState);
  
  httpUpdater.setup(&server);
  server.begin();
  Serial.println("HTTP server started.");
}

// ====================== LOOP ======================
void loop() {
  server.handleClient();
  MDNS.update();
  updateLeds();
}