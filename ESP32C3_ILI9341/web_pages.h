#ifndef WEB_PAGES_H
#define WEB_PAGES_H

#include <Arduino.h>

const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>ESP32-C3 电源监控控制台</title><style>body, html { margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background-color: #0d1117; color: #c9d1d9; } .header { text-align: center; padding: 2rem 1rem 1rem 1rem; } .header h1 { font-size: 1.8rem; color: #58a6ff; display: flex; align-items: center; justify-content: center; gap: 10px; margin: 0; } .header .check-mark { color: #3fb950; } .container { display: flex; flex-wrap: wrap; justify-content: center; gap: 1.5rem; padding: 0 1rem 2rem 1rem; } .card { background-color: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 1.5rem; width: 100%; max-width: 450px; box-sizing: border-box; } .card h2 { margin-top: 0; margin-bottom: 1.2rem; font-size: 1.2rem; color: #8b949e; display: flex; align-items: center; gap: 8px; border-bottom: 1px dashed #30363d; padding-bottom: 8px; } .btn-group { display: grid; grid-template-columns: repeat(auto-fit, minmax(110px, 1fr)); gap: 10px; } .btn { text-decoration: none; display: inline-block; padding: 10px 15px; font-size: 0.95rem; font-weight: 500; border-radius: 6px; border: 1px solid #30363d; cursor: pointer; transition: all 0.2s ease-in-out; text-align: center; box-sizing: border-box; width: 100%; } .btn-primary { background-color: #238636; color: white; border-color: #3fb950; } .btn-primary:hover { background-color: #2ea043; } .btn-secondary { background-color: #21262d; color: #c9d1d9; } .btn-secondary:hover { border-color: #8b949e; } .btn-danger { background-color: #da3633; color: white; border-color: #d0302d; } .btn-danger:hover { background-color: #e04442; } .btn-warning { background-color: #d29922; color: #161b22; border-color: #f0883e; font-weight: bold; } .form-group { margin-bottom: 1rem; } .form-group label { display: block; margin-bottom: 0.4rem; font-size: 0.85rem; color: #8b949e; } .input-field { width: 100%; background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px; padding: 8px 10px; color: #c9d1d9; font-size: 0.95rem; box-sizing: border-box; } .val-grid { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 10px; text-align: center; margin-bottom: 15px; } .val-card { background: #0d1117; border: 1px solid #30363d; border-radius: 6px; padding: 10px 5px; } .val-num { font-size: 1.3rem; font-weight: bold; color: #58a6ff; } .val-lbl { font-size: 0.75rem; color: #8b949e; margin-top: 4px; } .ir-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; } .footer { text-align: center; padding: 1.5rem 1rem; font-size: 0.85rem; color: #8b949e; border-top: 1px solid #30363d; margin-top: 2rem; width: 100%; } </style></head><body>

<div class="header">
  <h1><span class="check-mark">⚡</span> ESP32-C3 电源监控核心 v9.2</h1>
</div>

<div class="container">

<div class="card">
  <h2>⚡ INA219 实时电力监控</h2>
  <div class="val-grid">
    <div class="val-card"><div class="val-num" id="val-v">##VOLTAGE## V</div><div class="val-lbl">总线电压</div></div>
    <div class="val-card"><div class="val-num" id="val-a">##CURRENT## A</div><div class="val-lbl">回路电流</div></div>
    <div class="val-card"><div class="val-num" id="val-w">##POWER## W</div><div class="val-lbl">实时功率</div></div>
  </div>
  <p style="font-size:0.85rem; color:#8b949e; margin-top:0;">分流压降: <span id="val-mv" style="color:#58a6ff;">##SHUNT_MV##</span> mV</p>
  <div style="font-size:0.9rem; margin-bottom:15px;" id="val-status">状态提示: ##PROTECT_STATUS##</div>
  <div class="btn-group">
    <a href="/on" class="btn btn-primary">手动开启</a>
    <a href="/off" class="btn btn-danger">安全断开</a>
    <a href="/reset" class="btn btn-warning">故障重置</a>
  </div>
</div>

<div class="card">
  <h2>📊 温湿度与开关状态</h2>
  <div class="form-group">
    <p>室内温度: <span style="color:#58a6ff; font-weight:bold;">##DHT_TEMP## °C</span></p>
    <p>室内湿度: <span style="color:#58a6ff; font-weight:bold;">##DHT_HUM## %</span></p>
    <p>继电器状态: <span style="color:##RELAY_COLOR##; font-weight:bold;">##RELAY_STATUS##</span></p>
  </div>
</div>

<!-- 新增：Home Assistant 动态配置卡片 -->
<div class="card">
  <form action="/settings" method="post">
    <h2>🏠 Home Assistant 联动配置</h2>
    <div class="form-group"><label>HA 主机 IP</label><input type="text" name="ha_host" class="input-field" value="##HA_HOST##" required></div>
    <div class="form-group"><label>HA 端口 (默认 8123)</label><input type="number" name="ha_port" class="input-field" value="##HA_PORT##" required></div>
    <div class="form-group"><label>实体 ID (如 switch.xxx / light.xxx)</label><input type="text" name="ha_entity" class="input-field" value="##HA_ENTITY##" required></div>
    <div class="form-group"><label>长期控制 Token (全选粘贴完整 Token)</label><input type="text" name="ha_token" class="input-field" value="##HA_TOKEN##" placeholder="粘贴长 Token"></div>
    <button type="submit" class="btn btn-primary">保存 HA 配置</button>
  </form>
</div>

<div class="card">
  <form action="/save_ina" method="post">
    <h2>🛡️ INA219 电力保护配置</h2>
    <div class="form-group"><label>高于此电压自动复位吸合 (V)</label><input type="number" step="0.1" name="tonv" class="input-field" value="##TURN_ON_V##" required></div>
    <div class="form-group"><label>欠压切断保护阈值 (V)</label><input type="number" step="0.1" name="uv" class="input-field" value="##UNDER_V##" required></div>
    <div class="form-group"><label>低功率保护切断阈值 (W, 设0禁用)</label><input type="number" step="0.1" name="upw" class="input-field" value="##UNDER_P##" required></div>
    <div class="form-group"><label>保护锁定待机冷却时间 (秒)</label><input type="number" step="1" name="cds" class="input-field" value="##COOLDOWN_S##" required></div>
    <button type="submit" class="btn btn-primary">保存电源保护设置</button>
  </form>
</div>

<div class="card">
  <form action="/settings" method="post">
    <h2>🌙 屏幕/网络待机定时</h2>
    <div class="form-group"><label>休眠时间</label><input type="time" name="sleep" class="input-field" value="##SLEEP_TIME##" required></div>
    <div class="form-group"><label>唤醒时间</label><input type="time" name="wake" class="input-field" value="##WAKE_TIME##" required></div>

    <h2>⏱️ 继电器定时开关</h2>
    <div class="form-group">
      <label style="display: flex; align-items: center; gap: 8px;">
        <input type="checkbox" name="relay_timer_en" value="1" ##RELAY_TIMER_CHECKED## style="width:18px; height:18px; margin:0;"> 启用继电器定时开关
      </label>
    </div>
    <div class="form-group"><label>定时开启时间</label><input type="time" name="relay_on" class="input-field" value="##RELAY_ON_TIME##"></div>
    <div class="form-group"><label>定时关闭时间</label><input type="time" name="relay_off" class="input-field" value="##RELAY_OFF_TIME##"></div>

    <h2>🌡️ 智能温控设置</h2>
    <div class="form-group">
      <label style="display: flex; align-items: center; gap: 8px;">
        <input type="checkbox" name="temp_ctrl" value="1" ##TEMP_CTRL_CHECKED## style="width:18px; height:18px; margin:0;"> 启用温度自动控制继电器
      </label>
    </div>
    <div class="form-group"><label>温控开启阈值 (°C)</label><input type="number" step="0.1" name="temp_threshold" class="input-field" value="##TEMP_THRESHOLD##"></div>
    <div class="form-group"><label>温控关闭阈值 (°C)</label><input type="number" step="0.1" name="temp_threshold_off" class="input-field" value="##TEMP_THRESHOLD_OFF##"></div>

    <h2>🌦️ 基础配置</h2>
    <div class="form-group"><label>OpenWeather API Key</label><input type="text" name="apikey" class="input-field" value="##APIKEY##"></div>
    <div class="form-group"><label>城市拼音 (如 zhumadian)</label><input type="text" name="city" class="input-field" value="##CITY##"></div>

    <button type="submit" class="btn btn-primary">保存基础与温控设置</button>
  </form>
</div>

<div class="card">
  <h2>📡 红外配置 (HEX)</h2>
  <form action="/save_ir" method="post">
    <div class="ir-grid">
      <div class="form-group"><label>ON</label><input type="text" name="ir_on" class="input-field" value="##IR_ON##"></div>
      <div class="form-group"><label>OFF</label><input type="text" name="ir_off" class="input-field" value="##IR_OFF##"></div>
      <div class="form-group"><label>亮度+</label><input type="text" name="ir_up" class="input-field" value="##IR_UP##"></div>
      <div class="form-group"><label>亮度-</label><input type="text" name="ir_down" class="input-field" value="##IR_DOWN##"></div>
    </div>
    <button type="submit" class="btn btn-primary" style="margin-top:10px;">更新红外码</button>
  </form>
</div>

<div class="card">
  <h2>⚙️ 系统操作</h2>
  <div class="btn-group">
    <a href="/update" class="btn btn-primary">固件更新 🚀</a>
    <a href="/logs" class="btn btn-secondary">查看日志 📋</a>
  </div>
</div>

<div class="footer">
  <p><a href="https://github.com/yyszone/arduino-code/tree/main/ESP32C3_ILI9341" target="_blank" rel="noopener noreferrer">ESP32C3_ILI9341.ino</a> & INA219 Controller v9.2</p>
</div>

</div>

<script>
setInterval(function(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('val-v').textContent = d.v.toFixed(3) + " V";
    document.getElementById('val-a').textContent = (d.a/1000).toFixed(3) + " A";
    document.getElementById('val-w').textContent = (d.w/1000).toFixed(2) + " W";
    document.getElementById('val-mv').textContent = d.mv.toFixed(2);
    document.getElementById('val-status').innerHTML = "状态提示: " + d.status;
  }).catch(()=>{});
}, 3000);
</script>

</body></html>
)HTML";

#endif // WEB_PAGES_H