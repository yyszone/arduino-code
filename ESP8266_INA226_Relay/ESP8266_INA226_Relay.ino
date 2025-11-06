// =================================================================================================
// ==      ESP8266 智能继电器 & 电池电量监控器 v2.3 (月度统计版)     ==
// ==      已适配 INA226 模块 (GyverINA库 - 最终编译修正版)         ==
// =================================================================================================
// 描述: 本脚本专为ESP8266设计，用于12V锂电池供电系统。它通过INA226监测电压电流，
//       控制一个继电器，并提供网页界面进行手动控制、参数设置和OTA固件更新。
// ... (其他描述保持不变) ...
// =================================================================================================


#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <GyverINA.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <time.h>

// ============== 用户配置 ==============
const char* ssid       = "yang1234";
const char* password   = "y123456789";
const char* deviceName = "esp8266-smart-relay";
const int WEB_SERVER_PORT = 80;

// ============== 硬件引脚配置 ==============
const int RELAY_PIN   = 14; // D5
const int I2C_SDA_PIN = 4;  // D2
const int I2C_SCL_PIN = 5;  // D1

// ============== 时间与定时任务配置 ==============
const char* NTP_SERVER = "ntp.aliyun.com";
const long  GMT_OFFSET_SEC = 8 * 3600;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, GMT_OFFSET_SEC);

// ============== EEPROM 存储地址定义 ==============
const int EEPROM_SIZE = 256;
const int ADDR_MAGIC_NUM   = 0;
const int ADDR_LOW_V       = 2;
const int ADDR_HIGH_V      = 6;
const int ADDR_TODAY_WH    = 10;
const int ADDR_LAST_DAY    = 14;
const int ADDR_HISTORY_WH  = 15;
const int ADDR_PRICE_PER_KWH = 40;
const int ADDR_MONTH_WH      = 44;
const int ADDR_LAST_MONTH    = 48;
const uint16_t EEPROM_MAGIC_NUMBER = 0x1A2D;

// ============== 全局对象与变量 ==============
ESP8266WebServer server(WEB_SERVER_PORT);

// 确认模块电阻为 R100 (0.1欧姆), GyverINA库默认就是0.1, 所以直接声明即可
INA226 ina;

// ======================= 电流校准系数 =========================
// 如果修正接线后，电流读数仍有固定偏差，请修改此值。
// 校准系数 = 真实电流 / 测量电流
// 例如：万用表测得1200mA, 网页显示1100mA, 系数 = 1200 / 1100 = 1.09
// 初始值为 1.0，即不进行校准。
const float CURRENT_CALIBRATION_FACTOR = 1.0f;
// ==========================================================

bool relayState = false, ina_ok = false;
float loadVoltage = 0, current_mA = 0, power_mW = 0;
float lowVoltageThreshold = 10.5, highVoltageThreshold = 12.8;
bool isLockedOut = false;
unsigned long lockoutStartTime = 0;
const unsigned long LOCKOUT_DURATION_MS = 3600000;

// --- 电量统计变量 ---
float todayEnergyWh = 0;
float dailyEnergyHistory[6] = {0};
unsigned long lastEnergyCalcMs = 0;
int lastDayChecked = -1;

// --- 新增月度电费统计变量 ---
float pricePerKwh = 0.68;
float currentMonthEnergyWh = 0;
int lastMonthChecked = -1;


// =====================================================
// ============== 网页 (HTML+CSS+JS) v2.3 ==============
// =====================================================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP8266 智能继电器</title><style>:root{--bg-color:#111827;--card-color:#1f2937;--text-color:#d1d5db;--accent-color:#38bdf8;--green-color:#22c55e;--red-color:#ef4444;--muted-text:#9ca3af}body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";background-color:var(--bg-color);color:var(--text-color);margin:0;padding:15px;display:flex;justify-content:center}h1,h2,h4{margin-top:0;color:#fff;text-align:center}h2{border-top:1px solid #374151;padding-top:15px;margin-top:20px}h4{margin-bottom:10px;}.container{width:100%;max-width:500px}.card{background-color:var(--card-color);border-radius:12px;padding:20px;margin-bottom:15px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1),0 2px 4px -1px rgba(0,0,0,.06)}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:15px}.data-box{text-align:center}.data-box .val{font-size:2em;font-weight:700;color:var(--accent-color);line-height:1.2}.data-box .unit{color:var(--muted-text)}.btn{width:100%;padding:15px;font-size:1.2em;font-weight:bold;border:none;border-radius:8px;cursor:pointer;transition:background-color .2s ease}.btn.on{background-color:var(--green-color);color:#fff}.btn.off{background-color:var(--red-color);color:#fff}.status-light{width:12px;height:12px;border-radius:50%;display:inline-block;margin-right:8px;background-color:#6b7280}.status-light.on{background-color:var(--green-color)}.input-group{display:flex;align-items:center;gap:10px;margin-bottom:10px}.input-group label{flex-basis:120px;flex-shrink:0}input[type=number]{width:100%;padding:8px;background-color:#374151;border:1px solid #4b5563;border-radius:6px;color:var(--text-color);font-size:1em}.btn-save{padding:10px 15px;background-color:var(--accent-color);color:#fff;border:none;border-radius:6px;cursor:pointer}#sysinfo{font-size:.8em;color:var(--muted-text);word-break:break-all}#lockoutStatus{color:var(--red-color);text-align:center;margin-bottom:10px;font-weight:bold;}.realtime-power{text-align:center;margin-bottom:15px}.realtime-power-val{font-size:1.5em;color:var(--accent-color);font-weight:bold;}.chart-container{padding-top:10px}.chart{display:flex;justify-content:space-around;align-items:flex-end;height:120px;border-bottom:1px solid var(--muted-text)}.chart-bar{width:11%;background:linear-gradient(to top,var(--accent-color),#7dd3fc);border-radius:4px 4px 0 0;position:relative;transition:height .3s ease-in-out}.chart-bar .value{position:absolute;top:-20px;left:50%;transform:translateX(-50%);font-size:.8em;color:var(--text-color)}.chart-labels{display:flex;justify-content:space-around;font-size:.8em;color:var(--muted-text);margin-top:5px}.chart-labels div{width:11%;text-align:center}#monthlyStats{display:grid;grid-template-columns:1fr 1fr;gap:15px;text-align:center;margin-top:20px;padding-top:15px;border-top:1px solid #374151}</style></head><body><div class="container"><h1>ESP8266 智能继电器</h1><p style="text-align:center;color:var(--muted-text);">当前时间: <span id="currentTime">--:--:--</span></p><div class="card"><div class="grid"><div class="data-box"><div>电压</div><div class="val" id="v">--</div><div class="unit">V</div></div><div class="data-box"><div>电流</div><div class="val" id="c">--</div><div class="unit">mA</div></div><div class="data-box"><div>功率</div><div class="val" id="p">--</div><div class="unit">mW</div></div></div></div><div class="card"><h2>手动控制</h2><div id="lockoutStatus" style="display:none;"></div><p><span id="relayStatusLight" class="status-light"></span>继电器状态: <strong id="relayStatusText">读取中...</strong></p><button id="relayBtn" class="btn">读取中...</button></div><div class="card"><h2>参数设置</h2><div class="input-group"><label for="highV">高压开启 (V)</label><input type="number" id="highV" step="0.1"></div><div class="input-group"><label for="lowV">低压关闭 (V)</label><input type="number" id="lowV" step="0.1"></div><div class="input-group"><label for="price">电价 (元/kWh)</label><input type="number" id="price" step="0.01"></div><div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveSettings()">保存全部设置</button></div></div><div class="card"><h2>电量统计</h2><div class="realtime-power">当前功耗: <span id="currentPowerW" class="realtime-power-val">--</span> W</div><h4>七天用电量 (Wh)</h4><div class="chart-container" id="powerChartContainer"><div class="chart" id="powerChart"><p style="color:var(--muted-text);text-align:center;width:100%;">加载中...</p></div><div class="chart-labels" id="powerChartLabels"></div></div><div id="monthlyStats"><div><h4>本月总用电量</h4><div class="val" id="monthlyWh">--</div><div class="unit">kWh</div></div><div><h4>预估电费</h4><div class="val" id="monthlyCost">--</div><div class="unit">元</div></div></div></div><div class="card"><h2>系统信息与更新</h2><div id="sysinfo">加载中...</div><h4>固件更新 (OTA)</h4><div id="otaUi"><form id="otaForm" method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz" required><button type="submit" class="btn-save" style="margin-top:10px;">上传并更新</button></form></div><div id="otaStatus"></div></div></div>
<script>
function $(s){return document.getElementById(s)}
function fetchJson(url,options){return fetch(url,options).then(r=>{if(!r.ok)throw new Error('Network error');return r.json()})}
function updateStatus(data){
  const relayOn=data.relay;
  $('relayStatusText').textContent=relayOn?'已开启':'已关闭';
  $('relayStatusText').style.color=relayOn?'var(--green-color)':'var(--red-color)';
  $('relayStatusLight').className=relayOn?'status-light on':'status-light';
  $('relayBtn').textContent=relayOn?'关闭继电器':'开启继电器';
  $('relayBtn').className=relayOn?'btn off':'btn on';
  if(data.lockout){$('lockoutStatus').style.display='block';$('lockoutStatus').textContent='低压保护锁定中！剩余 '+data.lockout_rem+' 分钟可自动恢复。'}else{$('lockoutStatus').style.display='none';}
}
function fetchData(){fetchJson('/getData').then(data=>{
  $('v').textContent=data.voltage.toFixed(2);
  $('c').textContent=data.current.toFixed(1);
  $('p').textContent=data.power.toFixed(0);
  $('currentTime').textContent=data.time;
  $('currentPowerW').textContent=(data.power/1000).toFixed(2);
  updateStatus(data);
})}
function fetchInitialState(){fetchJson('/getStatus').then(data=>{
  updateStatus(data);
  $('lowV').value=data.low_v;
  $('highV').value=data.high_v;
  $('price').value=data.price;
  $('sysinfo').innerHTML=`IPv4: ${data.ip}<br>芯片ID: ${data.chip_id}<br>空闲内存: ${data.free_heap} B`;
})}
function saveSettings(){
  const lowV=$('lowV').value;
  const highV=$('highV').value;
  const price=$('price').value;
  fetch(`/setSettings?low=${lowV}&high=${highV}&price=${price}`).then(r=>{if(r.ok){alert('设置已保存!')}else{alert('保存失败!')}}).catch(e=>alert('请求出错: '+e));
}
function updatePowerChart(){fetchJson('/getPowerStats').then(data=>{
  const values=[data.today,...data.history];
  const totalEnergy=values.reduce((a,b)=>a+b,0);
  if(totalEnergy<=0){
    $('powerChartContainer').innerHTML='<p style="color:var(--muted-text);text-align:center;">暂无用电数据</p>';
    return;
  }
  const maxVal=Math.max(...values,0.01);
  let chartHTML='';
  for(const val of values){
    const height=(val/maxVal)*100;
    chartHTML+=`<div class="chart-bar" style="height:${height}%"><div class="value">${val.toFixed(2)}</div></div>`;
  }
  $('powerChart').innerHTML=chartHTML;
  const labels=["今天","昨天","前天","3天前","4天前","5天前","6天前"];
  let labelsHTML='';
  for(const label of labels){labelsHTML+=`<div>${label}</div>`;}
  $('powerChartLabels').innerHTML=labelsHTML;
})}
function updateMonthlyStats(){fetchJson('/getMonthlyStats').then(data=>{
    $('monthlyWh').textContent = (data.monthly_wh / 1000.0).toFixed(2);
    $('monthlyCost').textContent = data.monthly_cost.toFixed(2);
})}
$('relayBtn').addEventListener('click',()=>{const newState=$('relayBtn').classList.contains('on');fetch('/setRelay?state='+(newState?'1':'0')).then(()=>setTimeout(fetchData,200))});
$('otaForm').addEventListener('submit', function(e) {
  $('otaUi').style.display = 'none';
  $('otaStatus').innerHTML = '<h4>正在上传并更新...</h4><p>请勿关闭此页面或断开设备电源。设备将在大约一分钟后自动重启。</p>';
});
window.addEventListener('load',()=>{fetchInitialState();fetchData();updatePowerChart();updateMonthlyStats();});
setInterval(fetchData,2500);
setInterval(updatePowerChart,30000);
setInterval(updateMonthlyStats,31000);
</script></body></html>
)HTML";

const char OTA_SUCCESS_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新成功</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#22c55e;}</style></head><body><div><h1>更新成功!</h1><p>设备正在重启，请在约1分钟后重新连接。</p></div></body></html>
)HTML";

const char OTA_FAIL_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>更新失败</title><style>body{background-color:#111827;color:#d1d5db;font-family:system-ui;text-align:center;padding-top:50px;}div{background-color:#1f2937;padding:30px;border-radius:12px;display:inline-block;}h1{color:#ef4444;}</style></head><body><div><h1>更新失败!</h1><p>请检查上传的固件文件(.bin)是否正确，然后返回重试。</p></div></body></html>
)HTML";


// ============== EEPROM 数据读写函数 ==============
void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint16_t magic; EEPROM.get(ADDR_MAGIC_NUM, magic);
  if (magic == EEPROM_MAGIC_NUMBER) {
    Serial.println("[OK] 从EEPROM加载数据...");
    EEPROM.get(ADDR_LOW_V, lowVoltageThreshold); EEPROM.get(ADDR_HIGH_V, highVoltageThreshold);
    EEPROM.get(ADDR_TODAY_WH, todayEnergyWh); lastDayChecked = EEPROM.read(ADDR_LAST_DAY);
    for (int i = 0; i < 6; i++) EEPROM.get(ADDR_HISTORY_WH + i * sizeof(float), dailyEnergyHistory[i]);
    EEPROM.get(ADDR_PRICE_PER_KWH, pricePerKwh);
    EEPROM.get(ADDR_MONTH_WH, currentMonthEnergyWh);
    lastMonthChecked = EEPROM.read(ADDR_LAST_MONTH);
  } else {
    Serial.println("[!!] EEPROM未初始化或数据无效, 使用默认值并写入。");
    EEPROM.put(ADDR_MAGIC_NUM, EEPROM_MAGIC_NUMBER);
    EEPROM.put(ADDR_LOW_V, lowVoltageThreshold);
    EEPROM.put(ADDR_HIGH_V, highVoltageThreshold);
    EEPROM.put(ADDR_PRICE_PER_KWH, pricePerKwh);
    EEPROM.commit();
  }
  EEPROM.end();
  Serial.printf("  -> 低压:%.2fV, 高压:%.2fV, 电价:%.2f元/kWh\n", lowVoltageThreshold, highVoltageThreshold, pricePerKwh);
}

void saveSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(ADDR_LOW_V, lowVoltageThreshold);
  EEPROM.put(ADDR_HIGH_V, highVoltageThreshold);
  EEPROM.put(ADDR_PRICE_PER_KWH, pricePerKwh);
  EEPROM.commit(); EEPROM.end();
  Serial.println("[OK] 电压阈值和电价已保存到EEPROM。");
}

void saveEnergyData() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(ADDR_TODAY_WH, todayEnergyWh);
  EEPROM.write(ADDR_LAST_DAY, lastDayChecked);
  for (int i = 0; i < 6; i++) EEPROM.put(ADDR_HISTORY_WH + i * sizeof(float), dailyEnergyHistory[i]);
  EEPROM.put(ADDR_MONTH_WH, currentMonthEnergyWh);
  EEPROM.write(ADDR_LAST_MONTH, lastMonthChecked);
  EEPROM.commit(); EEPROM.end();
  Serial.println("[OK] 电量统计数据已保存。");
}

// ============== 核心功能函数 ==============
void sampleINA226() {
  if (!ina_ok) return;
  loadVoltage = ina.getVoltage();
  // 读取电流(A), 转换为(mA), 并应用校准系数
  current_mA = (ina.getCurrent() * 1000.0) * CURRENT_CALIBRATION_FACTOR;
  // 功率也需要校准
  power_mW = (ina.getPower() * 1000.0) * CURRENT_CALIBRATION_FACTOR;
}

void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  Serial.printf("继电器 (Pin %d) 已设置为: %s\n", RELAY_PIN, relayState ? "ON" : "OFF");
}

void checkVoltageProtection() {
  if (!ina_ok) return;
  if (isLockedOut) {
    if (millis() - lockoutStartTime >= LOCKOUT_DURATION_MS) {
      Serial.println("[OK] 1小时锁定时间已到，解除锁定。");
      isLockedOut = false;
    } else { return; }
  }
  sampleINA226();
  if (!relayState && !isLockedOut && loadVoltage >= highVoltageThreshold) {
    Serial.printf("检测到高电压 (%.2fV >= %.2fV)，自动开启继电器。\n", loadVoltage, highVoltageThreshold);
    setRelay(true);
  }
  if (relayState && loadVoltage > 0.1 && loadVoltage < lowVoltageThreshold) {
    Serial.printf("!!! 触发低压保护 (%.2fV < %.2fV)，自动关闭继电器并锁定1小时。\n", loadVoltage, lowVoltageThreshold);
    isLockedOut = true;
    lockoutStartTime = millis();
    setRelay(false);
  }
}

void accumulateEnergy() {
  if (!ina_ok || !relayState) { lastEnergyCalcMs = millis(); return; }
  unsigned long now = millis();
  unsigned long elapsedMs = now - lastEnergyCalcMs;
  if (elapsedMs > 0) {
    sampleINA226();
    double elapsedHours = (double)elapsedMs / 3600000.0;
    double powerWatts = (double)power_mW / 1000.0;
    float energyIncrementWh = powerWatts * elapsedHours;
    todayEnergyWh += energyIncrementWh;
    currentMonthEnergyWh += energyIncrementWh;
  }
  lastEnergyCalcMs = now;
}

void checkDataRollover() {
  timeClient.update();
  time_t epochTime = timeClient.getEpochTime();
  struct tm* ptm = localtime(&epochTime);
  int currentDay = ptm->tm_mday;
  int currentMonth = ptm->tm_mon; // 0-11

  if (lastDayChecked == -1) {
    lastDayChecked = currentDay;
    lastMonthChecked = currentMonth;
    return;
  }

  if (lastMonthChecked != currentMonth) {
    Serial.printf("检测到月份变更 (从 %d月 到 %d月)。正在重置月度用电量...\n", lastMonthChecked + 1, currentMonth + 1);
    currentMonthEnergyWh = 0.0;
    lastMonthChecked = currentMonth;
    saveEnergyData();
  }

  if (lastDayChecked != currentDay) {
    Serial.printf("检测到日期变更 (从 %d日 到 %d日)。正在滚动记录用电历史...\n", lastDayChecked, currentDay);
    // <-- 关键修正: 将错误的 dailyHistory 修正为 dailyEnergyHistory -->
    for (int i = 5; i > 0; i--) { dailyEnergyHistory[i] = dailyEnergyHistory[i - 1]; }
    dailyEnergyHistory[0] = todayEnergyWh;
    todayEnergyWh = 0.0;
    lastDayChecked = currentDay;
    saveEnergyData();
  }
}

// ============== 路由处理函数 (Web Handlers) ==============
void handleRoot() { server.send(200, "text/html; charset=UTF-8", MAIN_HTML); }

void handleGetData() {
  sampleINA226();
  timeClient.update();
  long remaining_min = 0;
  if (isLockedOut) {
    unsigned long elapsed_ms = millis() - lockoutStartTime;
    if (elapsed_ms < LOCKOUT_DURATION_MS) remaining_min = (LOCKOUT_DURATION_MS - elapsed_ms) / 60000;
  }
  String json = "{";
  json += "\"voltage\":" + String(loadVoltage, 2) + ",";
  json += "\"current\":" + String(current_mA, 1) + ",";
  json += "\"power\":" + String(power_mW, 0) + ",";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"time\":\"" + timeClient.getFormattedTime() + "\",";
  json += "\"lockout\":" + String(isLockedOut ? "true" : "false") + ",";
  json += "\"lockout_rem\":" + String(remaining_min);
  json += "}";
  server.send(200, "application/json", json);
}

void handleGetStatus() {
  String json = "{";
  json += "\"relay\":" + String(relayState ? "true" : "false") + ",";
  json += "\"low_v\":" + String(lowVoltageThreshold, 2) + ",";
  json += "\"high_v\":" + String(highVoltageThreshold, 2) + ",";
  json += "\"price\":" + String(pricePerKwh, 2) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"chip_id\":\"" + String(ESP.getChipId(), HEX) + "\",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap());
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetRelay() {
  if (server.hasArg("state")) {
    bool newState = server.arg("state").toInt() == 1;
    if (newState && isLockedOut) {
      Serial.println("[OK] 手动操作覆盖了低压锁定。");
      isLockedOut = false;
    }
    setRelay(newState);
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}

void handleSetSettings() {
  if (server.hasArg("low") && server.hasArg("high") && server.hasArg("price")) {
    lowVoltageThreshold = server.arg("low").toFloat();
    highVoltageThreshold = server.arg("high").toFloat();
    pricePerKwh = server.arg("price").toFloat();
    saveSettings();
    server.send(200, "text/plain", "OK");
  } else { server.send(400, "text/plain", "Bad Request"); }
}

void handleGetPowerStats() {
  String json = "{";
  json += "\"today\":" + String(todayEnergyWh, 4) + ",";
  json += "\"history\":[";
  for(int i=0; i < 6; i++) {
    json += String(dailyEnergyHistory[i], 4);
    if(i < 5) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleGetMonthlyStats() {
  float monthlyKwh = currentMonthEnergyWh / 1000.0;
  float monthlyCost = monthlyKwh * pricePerKwh;

  String json = "{";
  json += "\"monthly_wh\":" + String(currentMonthEnergyWh, 4) + ",";
  json += "\"monthly_cost\":" + String(monthlyCost, 2);
  json += "}";
  server.send(200, "application/json", json);
}


// ============== SETUP ==============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n===== ESP8266 智能继电器 v2.3 (INA226 GyverINA最终校准版) =====");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  relayState = true;
  Serial.println("[OK] 继电器已设置为默认开启状态。");

  // 根据GyverINA库的示例，begin函数会处理Wire.begin()
  if (!ina.begin(I2C_SDA_PIN, I2C_SCL_PIN)) {
    Serial.println("[!!] 硬件错误: 未能找到INA226芯片! 请检查接线,特别是VBUS到电池正极的连接。");
    ina_ok = false;
  } else {
    Serial.println("[OK] INA226 通信成功。");
    ina_ok = true;
  }

  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.hostname(deviceName);
  WiFi.begin(ssid, password);
  Serial.print("连接 Wi-Fi");
  int wifi_retries = 20;
  while (WiFi.status() != WL_CONNECTED && wifi_retries > 0) { delay(500); Serial.print("."); wifi_retries--; }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[!!] WiFi 连接失败! 请检查SSID和密码。设备将重启。");
    delay(1000); ESP.restart();
  }
  
  Serial.println("\n[OK] WiFi 已连接!");
  Serial.print("IPv4 地址: "); Serial.println(WiFi.localIP());

  timeClient.begin();
  timeClient.forceUpdate();
  Serial.println("[OK] NTP 时间服务已同步。");

  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", WEB_SERVER_PORT);
    Serial.printf("[OK] mDNS 已启动, 访问: http://%s.local\n", deviceName);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/getData", HTTP_GET, handleGetData);
  server.on("/getStatus", HTTP_GET, handleGetStatus);
  server.on("/getPowerStats", HTTP_GET, handleGetPowerStats);
  server.on("/getMonthlyStats", HTTP_GET, handleGetMonthlyStats);
  server.on("/setRelay", HTTP_GET, handleSetRelay);
  server.on("/setSettings", HTTP_GET, handleSetSettings);
  
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", Update.hasError() ? OTA_FAIL_HTML : OTA_SUCCESS_HTML);
    if (!Update.hasError()) { delay(1000); ESP.restart(); }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA] Update Start: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) { Update.printError(Serial); }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { Update.printError(Serial); }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { Serial.printf("[OTA] Update Success: %u bytes\n", upload.totalSize); }
      else { Update.printError(Serial); }
    }
  });

  server.begin();
  Serial.printf("[OK] HTTP 服务器已启动。\n========================================\n");
  lastEnergyCalcMs = millis();
}

// ============== LOOP ==============
void loop() {
  server.handleClient();
  MDNS.update();

  static unsigned long lastVoltageCheck = 0;
  static unsigned long lastDataRolloverCheck = 0;
  static unsigned long lastDataSave = 0;

  accumulateEnergy();

  if (millis() - lastVoltageCheck > 5000) {
    lastVoltageCheck = millis();
    checkVoltageProtection();
  }

  if (millis() - lastDataRolloverCheck > 30000) {
    lastDataRolloverCheck = millis();
    checkDataRollover();
  }
  
  if (millis() - lastDataSave > 5 * 60000) {
    lastDataSave = millis();
    saveEnergyData();
  }
}