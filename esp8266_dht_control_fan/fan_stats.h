#pragma once

#include <LittleFS.h>
#include <EEPROM.h>
#include <time.h>

// ──── 配置 ─────────────────────────────────────────────────
#define MAX_DAYS             7
#define STATS_EEPROM_BASE    100          
#define STATS_EEPROM_MAGIC   0x46414E53UL 
#define STATS_FILE           "/fan_daily.txt"
#define SAVE_INTERVAL_MS     60000UL

// ──── EEPROM 数据结构 ──────────────────────────────────────
struct FanDayEntry {
  uint32_t dateKey;   // year*10000 + month*100 + day
  uint32_t secs;
};

struct FanStatsEEPROM {
  uint32_t    magic;
  uint32_t    count;                  
  FanDayEntry entries[MAX_DAYS];
};

// ──── 内部状态 ─────────────────────────────────────────────
static bool          _fanOn        = false;
static unsigned long _fanStartMs   = 0;
static unsigned long _todayFanSec  = 0;
static uint32_t      _todayKey     = 0;   
static unsigned long _lastSaveMs   = 0;
static FanStatsEEPROM _eepData;           

// ──── 工具函数 ─────────────────────────────────────────────
static bool _isTimeSynced() {
  time_t now = time(nullptr);
  return now > 8 * 3600 * 2; // 判断 NTP 是否已成功同步
}

static uint32_t _makeDateKey(struct tm* t) {
  return (uint32_t)(t->tm_year + 1900) * 10000
       + (uint32_t)(t->tm_mon  + 1)   * 100
       + (uint32_t)(t->tm_mday);
}

static uint32_t _todayDateKey() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  return _makeDateKey(t);
}

static void _dateKeyToStr(uint32_t key, char* buf) {
  uint32_t y = key / 10000;
  uint32_t m = (key % 10000) / 100;
  uint32_t d = key % 100;
  snprintf(buf, 11, "%04u-%02u-%02u", (unsigned)y, (unsigned)m, (unsigned)d);
}

// ──── EEPROM 读写 ──────────────────────────────────────────
static void _eepSave() {
  EEPROM.put(STATS_EEPROM_BASE, _eepData);
  EEPROM.commit();
}

static void _eepLoad() {
  EEPROM.get(STATS_EEPROM_BASE, _eepData);
  if (_eepData.magic != STATS_EEPROM_MAGIC || _eepData.count > MAX_DAYS) {
    Serial.println("[FanStats] EEPROM blank/corrupt, init fresh");
    memset(&_eepData, 0, sizeof(_eepData));
    _eepData.magic = STATS_EEPROM_MAGIC;
    _eepData.count = 0;
    return;
  }

  // 【修复】自动过滤掉非法的历史日期（如未同步时间时写入的 1970 年 key）
  int validCount = 0;
  FanDayEntry tempEntries[MAX_DAYS];
  for (uint32_t i = 0; i < _eepData.count; i++) {
    if (_eepData.entries[i].dateKey >= 20200000UL) { // 过滤掉小于2020年的数据
      tempEntries[validCount++] = _eepData.entries[i];
    }
  }
  if (validCount != (int)_eepData.count) {
    memset(_eepData.entries, 0, sizeof(_eepData.entries));
    memcpy(_eepData.entries, tempEntries, sizeof(FanDayEntry) * validCount);
    _eepData.count = validCount;
    _eepSave();
    Serial.println("[FanStats] Filtered out invalid history dates");
  }
}

static int _findOrAdd(uint32_t key) {
  for (int i = 0; i < (int)_eepData.count; i++) {
    if (_eepData.entries[i].dateKey == key) return i;
  }
  if (_eepData.count >= MAX_DAYS) {
    memmove(&_eepData.entries[0], &_eepData.entries[1],
            sizeof(FanDayEntry) * (MAX_DAYS - 1));
    _eepData.count = MAX_DAYS - 1;
  }
  int idx = _eepData.count;
  _eepData.entries[idx].dateKey = key;
  _eepData.entries[idx].secs    = 0;
  _eepData.count++;
  return idx;
}

static void _flushToday(uint32_t key, unsigned long secs) {
  int idx = _findOrAdd(key);
  _eepData.entries[idx].secs = (uint32_t)secs;
  _eepSave();
  Serial.printf("[FanStats] EEPROM saved key=%lu secs=%lu\n", (unsigned long)key, secs);
}

static void _syncToFile() {
  if (!LittleFS.begin()) return;
  File f = LittleFS.open(STATS_FILE, "w");
  if (!f) return;
  char buf[12];
  for (int i = 0; i < (int)_eepData.count; i++) {
    _dateKeyToStr(_eepData.entries[i].dateKey, buf);
    f.printf("%s\t%lu\n", buf, (unsigned long)_eepData.entries[i].secs);
  }
  f.close();
}

// ──── 初始化 ──────────────────────────────────────────────
void fanStats_begin() {
  _eepLoad();

  // 【修复】仅在 NTP 已同步时初始化今日 key，否则延后处理
  if (_isTimeSynced()) {
    _todayKey = _todayDateKey();
    for (int i = 0; i < (int)_eepData.count; i++) {
      if (_eepData.entries[i].dateKey == _todayKey) {
        _todayFanSec = _eepData.entries[i].secs;
        Serial.printf("[FanStats] Restored today secs=%lu\n", _todayFanSec);
        break;
      }
    }
  } else {
    _todayKey = 0;
    _todayFanSec = 0;
    Serial.println("[FanStats] NTP not synced yet. Postponing date key initialization.");
  }

  LittleFS.begin();
  _syncToFile();
}

// ──── 跨天处理 ─────────────────────────────────────────────
static void _checkDayRollover() {
  if (!_isTimeSynced()) return; // 【修复】未同步时不进行日期逻辑，防止假跨天

  uint32_t key = _todayDateKey();

  // 【修复】处理首次从未同步突变到同步成功的时刻
  if (_todayKey == 0) {
    _todayKey = key;
    for (int i = 0; i < (int)_eepData.count; i++) {
      if (_eepData.entries[i].dateKey == _todayKey) {
        _todayFanSec = _eepData.entries[i].secs;
        Serial.printf("[FanStats] NTP Synced. Restored today secs=%lu\n", _todayFanSec);
        break;
      }
    }
    return;
  }

  if (key == _todayKey) return;

  // 真正的跨天：保存昨天，重置今天
  unsigned long now = millis();
  if (_fanOn && _fanStartMs > 0) {
    _todayFanSec += (now - _fanStartMs) / 1000;
    _fanStartMs   = now;
  }
  _flushToday(_todayKey, _todayFanSec);
  _syncToFile();

  _todayKey    = key;
  _todayFanSec = 0;
  _findOrAdd(key);
  _eepSave();
  Serial.printf("[FanStats] Day Rollover! New day key=%lu\n", (unsigned long)_todayKey);
}

void fanStats_onRelayChange(bool newState) {
  _checkDayRollover();
  unsigned long now = millis();

  if (newState && !_fanOn) {
    _fanOn      = true;
    _fanStartMs = now;
  } else if (!newState && _fanOn) {
    _fanOn = false;
    if (_fanStartMs > 0) {
      _todayFanSec += (now - _fanStartMs) / 1000;
      Serial.printf("[FanStats] Fan off, today total=%lus\n", _todayFanSec);
    }
    if (_todayKey > 0) {
      _flushToday(_todayKey, _todayFanSec);
      _syncToFile();
    }
    _lastSaveMs = now;
  }
}

void fanStats_loop() {
  _checkDayRollover();
  unsigned long now = millis();
  if (_fanOn && (now - _lastSaveMs >= SAVE_INTERVAL_MS)) {
    unsigned long live = _todayFanSec + (now - _fanStartMs) / 1000;
    if (_todayKey > 0) {
      _flushToday(_todayKey, live);
    }
    _lastSaveMs = now;
  }
}

// ──── 构建 HTML ───
String fanStats_buildPage() {
  unsigned long liveSec = _todayFanSec;
  if (_fanOn && _fanStartMs > 0)
    liveSec += (millis() - _fanStartMs) / 1000;

  FanStatsEEPROM d = _eepData;
  for (int i = 0; i < (int)d.count; i++) {
    if (d.entries[i].dateKey == _todayKey) {
      d.entries[i].secs = (uint32_t)liveSec;
      break;
    }
  }

  String labels  = "[";
  String dataArr = "[";
  String tips    = "[";
  char buf[12];
  for (int i = 0; i < (int)d.count; i++) {
    if (i) { labels += ","; dataArr += ","; tips += ","; }
    _dateKeyToStr(d.entries[i].dateKey, buf);
    char shortDate[6] = "";
    strncpy(shortDate, buf + 5, 5); shortDate[5] = '\0';
    labels += "\""; labels += shortDate; labels += "\"";

    uint32_t s = d.entries[i].secs;
    float minVal = s / 60.0f;
    dataArr += String(minVal, 2);

    char tipBuf[24];
    if (s >= 60) snprintf(tipBuf, sizeof(tipBuf), "%.1f 分钟", s / 60.0f);
    else         snprintf(tipBuf, sizeof(tipBuf), "%u 秒", (unsigned)s);
    tips += "\""; tips += tipBuf; tips += "\"";
  }
  labels  += "]";
  dataArr += "]";
  tips    += "]";

  String html = F("<!DOCTYPE html><html lang='zh'><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>风扇运行统计</title>"
    "<script src='https://cdn.jsdelivr.net/npm/echarts@5/dist/echarts.min.js'></script>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#0f172a;color:#e2e8f0;font-family:'Segoe UI',sans-serif;"
    "min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:20px 14px}"
    ".hdr{text-align:center;margin-bottom:18px}"
    ".hdr h1{font-size:1.4em;color:#60a5fa}"
    ".hdr p{color:#64748b;font-size:.85em;margin-top:4px}"
    "#chart{width:100%;max-width:680px;height:340px;background:#1e293b;"
    "border-radius:14px;border:1px solid #334155}"
    ".back{margin-top:16px;color:#60a5fa;text-decoration:none;font-size:.9em}"
    ".note{margin-top:10px;color:#475569;font-size:.78em}"
    "</style></head><body>"
    "<div class='hdr'><h1>📊 风扇运行统计</h1>"
    "<p>近 7 天每日开启时长（分钟）· 数据存于 EEPROM，OTA 不丢失</p></div>"
    "<div id='chart'></div>"
    "<a class='back' href='/'>← 返回主控制台</a>"
    "<div class='note'>数据持久化：EEPROM（掉电/OTA 均保留）</div>"
    "<script>"
    "var labels=");
  html += labels;
  html += F(";var rawData=");
  html += dataArr;
  html += F(";var tips=");
  html += tips;
  html += F(";"
    "var chart=echarts.init(document.getElementById('chart'),null,{renderer:'canvas'});"
    "chart.setOption({"
    "  backgroundColor:'transparent',"
    "  tooltip:{trigger:'axis',axisPointer:{type:'shadow'},"
    "    formatter:function(p){return p[0].name+'<br/>开风扇：'+tips[p[0].dataIndex];}"
    "  },"
    "  grid:{left:'14%',right:'5%',top:'12%',bottom:'18%'},"
    "  xAxis:{type:'category',data:labels,"
    "    axisLine:{lineStyle:{color:'#475569'}},"
    "    axisLabel:{color:'#94a3b8',fontSize:12}"
    "  },"
    "  yAxis:{type:'value',name:'分钟',"
    "    nameTextStyle:{color:'#64748b'},"
    "    axisLine:{lineStyle:{color:'#475569'}},"
    "    axisLabel:{color:'#94a3b8'},"
    "    splitLine:{lineStyle:{color:'#334155',type:'dashed'}}"
    "  },"
    "  series:[{type:'bar',data:rawData,barMaxWidth:40,"
    "    itemStyle:{color:new echarts.graphic.LinearGradient(0,0,0,1,"
    "      [{offset:0,color:'#38bdf8'},{offset:1,color:'#0ea5e9'}]"
    "    ),borderRadius:[6,6,0,0]},"
    "    emphasis:{itemStyle:{color:'#7dd3fc'}}"
    "  }]"
    "});"
    "window.addEventListener('resize',()=>chart.resize());"
    "</script></body></html>");

  return html;
}