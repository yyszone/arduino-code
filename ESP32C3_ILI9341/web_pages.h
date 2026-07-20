#ifndef WEB_PAGES_H
#define WEB_PAGES_H

// ==================== 网页界面 HTML =========================
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP32-C3 控制台</title><style>body, html { margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background-color: #0d1117; color: #c9d1d9; } .header { text-align: center; padding: 2rem 1rem; } .header h1 { font-size: 2rem; color: #58a6ff; display: flex; align-items: center; justify-content: center; gap: 10px; } .header .check-mark { color: #3fb950; } .container { display: flex; flex-wrap: wrap; justify-content: center; gap: 1.5rem; padding: 0 1rem 2rem 1rem; } .card { background-color: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 1.5rem; width: 100%; max-width: 400px; box-sizing: border-box; } .card h2 { margin-top: 0; margin-bottom: 1.5rem; font-size: 1.25rem; color: #8b949e; display: flex; align-items: center; gap: 8px; } .btn-group { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; } .btn { text-decoration: none; display: inline-block; padding: 10px 20px; font-size: 1rem; font-weight: 500; border-radius: 6px; border: 1px solid #30363d; cursor: pointer; transition: all 0.2s ease-in-out; text-align: center; box-sizing: border-box; width: 100%; } .btn-primary { background-color: #238636; color: white; border-color: #3fb950; } .btn-primary:hover { background-color: #2ea043; } .btn-secondary { background-color: #21262d; color: #c9d1d9; } .btn-secondary:hover { border-color: #8b949e; } .btn-danger { background-color: #da3633; color: white; border-color: #d0302d; } .btn-danger:hover { background-color: #e04442; } .form-group { margin-bottom: 1rem; } .form-group label { display: block; margin-bottom: 0.5rem; font-size: 0.9rem; color: #8b949e; } .input-field { width: 100%; background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px; padding: 10px; color: #c9d1d9; font-size: 1rem; box-sizing: border-box; } .schedule-display { font-size: 1.5rem; font-weight: bold; color: #58a6ff; text-align: center; margin: 1rem 0; } .ir-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; } .footer { text-align: center; padding: 2rem 1rem; font-size: 0.85rem; color: #8b949e; border-top: 1px solid #30363d; margin-top: 2rem; width: 100%; } </style></head><body><div class="header"><h1><span class="check-mark">✓</span> ESP32-C3 SuperMini v8.4</h1></div><div class="container">

<!-- 📊 实时传感器展示卡片 -->
<div class="card">
  <h2>📊 传感器与设备状态</h2>
  <div class="form-group">
    <p>室内温度: <span style="color:#58a6ff; font-weight:bold;">##DHT_TEMP## °C</span></p>
    <p>室内湿度: <span style="color:#58a6ff; font-weight:bold;">##DHT_HUM## %</span></p>
    <p>继电器状态: <span style="color:##RELAY_COLOR##; font-weight:bold;">##RELAY_STATUS##</span></p>
  </div>
</div>

<!-- 🔌 继电器手动网页遥控 -->
<div class="card">
  <h2>🔌 继电器手动控制</h2>
  <div class="btn-group">
    <a href="/relay?cmd=on" class="btn btn-primary">手动开启 (ON)</a>
    <a href="/relay?cmd=off" class="btn btn-danger">手动关闭 (OFF)</a>
  </div>
</div>

<div class="card"><h2>⚙️ 系统操作</h2><div class="btn-group"><a href="/update" class="btn btn-primary">固件更新 🚀</a><a href="/logs" class="btn btn-secondary">查看日志 📋</a></div></div><div class="card"><form action="/settings" method="post"><h2>🌙 夜间定时</h2><div class="form-group"><label>睡眠时间</label><input type="time" name="sleep" class="input-field" value="##SLEEP_TIME##" required></div><div class="form-group"><label>唤醒时间</label><input type="time" name="wake" class="input-field" value="##WAKE_TIME##" required></div><h2>🌦️ 基础配置</h2><div class="form-group"><label>OpenWeather API Key</label><input type="text" name="apikey" class="input-field" value="##APIKEY##"></div><div class="form-group"><label>城市拼音 (如 beijing)</label><input type="text" name="city" class="input-field" value="##CITY##"></div>

<!-- 自动温控配置项 -->
<h2>🌡️ 智能温控设置</h2>
<div class="form-group">
  <label style="display: flex; align-items: center; gap: 8px;">
    <input type="checkbox" name="temp_ctrl" value="1" ##TEMP_CTRL_CHECKED## style="width:18px; height:18px; margin:0;"> 启用温度自动控制继电器
  </label>
</div>
<div class="form-group">
  <label>温控开启阈值 (高于此温度开启, °C)</label>
  <input type="number" step="0.1" name="temp_threshold" class="input-field" value="##TEMP_THRESHOLD##">
</div>
<div class="form-group">
  <label>温控关闭阈值 (低于此温度关闭, °C)</label>
  <input type="number" step="0.1" name="temp_threshold_off" class="input-field" value="##TEMP_THRESHOLD_OFF##">
</div>

<button type="submit" class="btn btn-primary">保存设置</button></form></div><div class="card"><h2>📡 红外配置 (HEX)</h2><form action="/save_ir" method="post"><div class="ir-grid"><div class="form-group"><label>ON</label><input type="text" name="ir_on" class="input-field" value="##IR_ON##"></div><div class="form-group"><label>OFF</label><input type="text" name="ir_off" class="input-field" value="##IR_OFF##"></div><div class="form-group"><label>亮度+</label><input type="text" name="ir_up" class="input-field" value="##IR_UP##"></div><div class="form-group"><label>亮度-</label><input type="text" name="ir_down" class="input-field" value="##IR_DOWN##"></div><div class="form-group"><label>AUTO</label><input type="text" name="ir_auto" class="input-field" value="##IR_AUTO##"></div><div class="form-group"><label>3H</label><input type="text" name="ir_3h" class="input-field" value="##IR_3H##"></div><div class="form-group"><label>5H</label><input type="text" name="ir_5h" class="input-field" value="##IR_5H##"></div><div class="form-group"><label>8H</label><input type="text" name="ir_8h" class="input-field" value="##IR_8H##"></div></div><button type="submit" class="btn btn-primary">更新红外码</button></form></div><div class="card"><h2>📝 当前计划</h2><div class="schedule-display">##CURRENT_SCHEDULE##</div></div><div class="card"><h2>💡 灯光红外遥控</h2><div class="btn-group"><a href="/ir?cmd=on" class="btn btn-primary">ON</a><a href="/ir?cmd=off" class="btn btn-danger">OFF</a><a href="/ir?cmd=bright_up" class="btn btn-secondary">亮度 +</a><a href="/ir?cmd=bright_down" class="btn btn-secondary">亮度 -</a><a href="/ir?cmd=auto" class="btn btn-secondary">AUTO</a><a href="/ir?cmd=timer_3h" class="btn btn-secondary">3H</a><a href="/ir?cmd=timer_5h" class="btn btn-secondary">5H</a><a href="/ir?cmd=timer_8h" class="btn btn-secondary">8H</a></div></div>

<div class="footer">
  <p>Script Name: ESP32C3_ILI9341.ino</p>
</div>

</div></body></html>
)HTML";

#endif