#pragma once
/*
 * fan_stats.h — 风扇运行时长统计
 *
 * 存储策略：
 *   主存储 → EEPROM（OTA升级/重启均不丢失）
 *   备份   → LittleFS /fan_daily.txt（方便调试查看）
 *
 * EEPROM 布局（起始地址 STATS_EEPROM_BASE，共 8+7*8=64 字节）：
 *   [0..3]  魔数 0x46414E53 ("FANS")
 *   [4..7]  条目数量 (uint32_t)
 *   每条目 8 字节：
 *     [0..3] 日期压缩 = year*10000 + month*100 + day  (uint32_t)
 *     [4..7] 累计秒数 (uint32_t)
 *
 * 主文件需已 #include <EEPROM.h> 并在 setup() 调用 EEPROM.begin(512)
 */

#include <LittleFS.h>
#include <EEPROM.h>
#include <time.h>

// ──── 配置 ─────────────────────────────────────────────────
#define MAX_DAYS             7
#define STATS_EEPROM_BASE    100          // 避开主文件已用的 0~63
#define STATS_EEPROM_MAGIC   0x46414E53UL // "FANS"
#define STATS_FILE           "/fan_daily.txt"
#define SAVE_INTERVAL_MS     60000UL

// ──── EEPROM 数据结构 ──────────────────────────────────────
struct FanDayEntry {
  uint32_t dateKey;   // year*10000 + month*100 + day
  uint32_t secs;
};

struct FanStatsEEPROM {
  uint32_t    magic;
  uint32_t    count;                  // 有效条目数，最多 MAX_DAYS
  FanDayEntry entries[MAX_DAYS];
};

// ──── 内部状态 ─────────────────────────────────────────────
static bool          _fanOn        = false;
static unsigned long _fanStartMs   = 0;
static unsigned long _todayFanSec  = 0;
static uint32_t      _todayKey     = 0;   // 当天 dateKey
static unsigned long _lastSaveMs   = 0;
static FanStatsEEPROM _eepData;           // 内存镜像

// ──── 工具函数 ─────────────────────────────────────────────
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
  // buf 至少 11 字节
  uint32_t y = key / 10000;
  uint32_t m = (key % 10000) / 100;
  uint32_t d = key % 100;
  snprintf(buf, 11, "%04u-%02u-%02u", (unsigned)y, (unsigned)m, (unsigned)d);
}

// ──── EEPROM 读写 ──────────────────────────────────────────
static void _eepLoad() {
  EEPROM.get(STATS_EEPROM_BASE, _eepData);
  if (_eepData.magic != STATS_EEPROM_MAGIC ||
      _eepData.count > MAX_DAYS) {
    Serial.println("[FanStats] EEPROM blank/corrupt, init fresh");
    memset(&_eepData, 0, sizeof(_eepData));
    _eepData.magic = STATS_EEPROM_MAGIC;
    _eepData.count = 0;
  }
}

static void _eepSave() {
  EEPROM.put(STATS_EEPROM_BASE, _eepData);
  EEPROM.commit();
}

// ──── 找/插入某天的条目索引，不存在时追加 ────────────────
static int _findOrAdd(uint32_t key) {
  for (int i = 0; i < (int)_eepData.count; i++) {
    if (_eepData.entries[i].dateKey == key) return i;
  }
  // 若已满，把最旧的移除（向前平移）
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

// ──── 同步当天内存数据到 EEPROM 镜像并保存 ───────────────
static void _flushToday(uint32_t key, unsigned long secs) {
  int idx = _findOrAdd(key);
  _eepData.entries[idx].secs = (uint32_t)secs;
  _eepSave();
  Serial.printf("[FanStats] EEPROM saved key=%lu secs=%lu\n", (unsigned long)key, secs);
}

// ──── 同步 EEPROM → LittleFS（调试备份）─────────────────
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

// ──── 初始化（setup 里调用）──────────────────────────────
void fanStats_begin() {
  // EEPROM.begin 由主文件负责，这里只读取
  _eepLoad();

  _todayKey = _todayDateKey();

  // 恢复当天已有的累计值
  for (int i = 0; i < (int)_eepData.count; i++) {
    if (_eepData.entries[i].dateKey == _todayKey) {
      _todayFanSec = _eepData.entries[i].secs;
      Serial.printf("[FanStats] Restored today secs=%lu\n", _todayFanSec);
      break;
    }
  }

  // 同步一份到 LittleFS 方便调试
  LittleFS.begin();
  _syncToFile();
}

// ──── 跨天处理 ─────────────────────────────────────────────
static void _checkDayRollover() {
  uint32_t key = _todayDateKey();
  if (key == _todayKey) return;

  // 跨天：保存昨天，重置今天
  unsigned long now = millis();
  if (_fanOn && _fanStartMs > 0) {
    _todayFanSec += (now - _fanStartMs) / 1000;
    _fanStartMs   = now;
  }
  _flushToday(_todayKey, _todayFanSec);
  _syncToFile();

  _todayKey    = key;
  _todayFanSec = 0;
  // 确保新的一天在 EEPROM 里有位置（带 0 初始化）
  _findOrAdd(key);
  _eepSave();
}

// ──── 继电器状态变更时调用 ────────────────────────────────
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
    _flushToday(_todayKey, _todayFanSec);
    _syncToFile();
    _lastSaveMs = now;
  }
}

// ──── loop 里调用（跨天检测 + 定时刷盘）─────────────────
void fanStats_loop() {
  _checkDayRollover();
  unsigned long now = millis();
  if (_fanOn && (now - _lastSaveMs >= SAVE_INTERVAL_MS)) {
    unsigned long live = _todayFanSec + (now - _fanStartMs) / 1000;
    _flushToday(_todayKey, live);
    _lastSaveMs = now;
  }
}

// ──── 构建 ECharts 7 天条形图 HTML ───────────────────────
String fanStats_buildPage() {
  // 从 EEPROM 镜像读取，实时值叠加今天
  unsigned long liveSec = _todayFanSec;
  if (_fanOn && _fanStartMs > 0)
    liveSec += (millis() - _fanStartMs) / 1000;

  // 临时拷贝，避免修改镜像
  FanStatsEEPROM d = _eepData;
  // 更新今天实时值
  for (int i = 0; i < (int)d.count; i++) {
    if (d.entries[i].dateKey == _todayKey) {
      d.entries[i].secs = (uint32_t)liveSec;
      break;
    }
  }

  // 构建 JSON 数组（按日期顺序，oldest first）
  String labels  = "[";
  String dataArr = "[";
  String tips    = "[";
  char buf[12];
  for (int i = 0; i < (int)d.count; i++) {
    if (i) { labels += ","; dataArr += ","; tips += ","; }
    _dateKeyToStr(d.entries[i].dateKey, buf);
    // 标签只显 MM-DD
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