#pragma once
#include <Arduino.h>

const char PIXEL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>iPixel 像素屏控制台</title>
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background-color: #0d1117; color: #c9d1d9; margin: 0; padding: 20px; }
  .container { max-width: 620px; margin: 0 auto; }
  h1 { text-align: center; color: #58a6ff; font-size: 1.8rem; }
  .card { background-color: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 1.5rem; margin-bottom: 1.5rem; box-sizing: border-box; }
  .card h2 { margin-top: 0; font-size: 1.2rem; color: #8b949e; border-bottom: 1px dashed #30363d; padding-bottom: 8px; display: flex; justify-content: space-between; align-items: center; }
  .badge { display: inline-block; padding: 3px 8px; border-radius: 6px; font-weight: bold; font-size: 12px; }
  .badge-ok { background: #238636; color: #fff; }
  .badge-fail { background: #da3633; color: #fff; }
  .btn-group { display: flex; gap: 10px; margin-bottom: 12px; }
  .btn { display: inline-block; padding: 10px 15px; font-size: 0.95rem; font-weight: 500; border-radius: 6px; border: 1px solid #30363d; cursor: pointer; text-align: center; width: 100%; color: white; text-decoration: none; }
  .btn:active { transform: scale(0.98); }
  .btn-primary { background-color: #238636; border-color: #3fb950; }
  .btn-danger { background-color: #da3633; border-color: #d0302d; }
  .btn-purple { background-color: #8957e5; border-color: #a371f7; }
  .btn-secondary { background-color: #21262d; color: #c9d1d9; }
  .grid-btn { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; margin-top: 10px; }
  .grid-btn button { background: #21262d; color: #c9d1d9; border: 1px solid #30363d; padding: 8px; border-radius: 6px; cursor: pointer; font-weight: bold; }
  .grid-btn button:hover { border-color: #8b949e; }
  .grid-btn button:active { background: #30363d; }
  .input-field { width: 100%; background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px; padding: 8px 10px; color: #c9d1d9; font-size: 0.95rem; box-sizing: border-box; margin-top: 4px; margin-bottom: 12px; }
  .row { display: flex; gap: 10px; } .row > div { flex: 1; }
  a.nav-link { display: block; margin-top: 20px; color: #58a6ff; text-decoration: none; font-weight: bold; text-align: center; }
  .slider { width: 100%; margin: 10px 0; }
</style>
</head>
<body>
<div class="container">
  <h1>✨ iPixel 32×32 像素屏控制</h1>

  <!-- 卡片 1: 基础控制 -->
  <div class="card">
    <h2><span>📱 屏幕硬件状态</span><span class="badge" id="bleBadge">连接中...</span></h2>
    <div class="btn-group">
      <button class="btn btn-primary" onclick="callApi('/api/pixel/power?state=1')">💡 强力开机</button>
      <button class="btn btn-danger" onclick="callApi('/api/pixel/power?state=0')">🌑 关机熄屏</button>
      <button class="btn btn-purple" onclick="callApi('/api/pixel/sync_ntp')">🕒 NTP校时</button>
    </div>
    
    <div style="font-size:0.9rem; color:#8b949e;">亮度调节: <span id="bVal" style="color:#58a6ff;font-weight:bold">60%</span></div>
    <input type="range" id="bSlider" class="slider" min="1" max="100" value="60" onchange="callApi('/api/pixel/brightness?val='+this.value)">

    <div style="font-size:0.9rem; color:#8b949e; margin-top:10px;">32×32 专属时钟样式 (1-8):</div>
    <div class="grid-btn">
      <button onclick="callApi('/api/pixel/clock?style=1')">样式 1</button>
      <button onclick="callApi('/api/pixel/clock?style=2')">样式 2</button>
      <button onclick="callApi('/api/pixel/clock?style=3')">样式 3</button>
      <button onclick="callApi('/api/pixel/clock?style=4')">样式 4</button>
      <button onclick="callApi('/api/pixel/clock?style=5')">样式 5</button>
      <button onclick="callApi('/api/pixel/clock?style=6')">样式 6</button>
      <button onclick="callApi('/api/pixel/clock?style=7')">样式 7</button>
      <button onclick="callApi('/api/pixel/clock?style=8')">样式 8</button>
    </div>
  </div>

  <!-- 卡片 2: 文字发送 -->
  <div class="card">
    <h2>💬 文字发生器</h2>
    <label>文本内容 (支持英数标点):</label>
    <input type="text" id="txtMsg" class="input-field" value="IP:105">

    <div class="row">
      <div>
        <label>字号规格:</label>
        <select id="txtSize" class="input-field">
          <option value="2" selected>2: 纵向满屏大字 (16×32)</option>
          <option value="1">1: 标准小字 (8×16)</option>
        </select>
      </div>
      <div>
        <label>动画模式:</label>
        <select id="txtAnim" class="input-field">
          <option value="1">1: 向左跑马灯</option>
          <option value="2">2: 静态居中</option>
        </select>
      </div>
    </div>

    <div class="row">
      <div>
        <label>速度 (1-100):</label>
        <input type="number" id="txtSpeed" class="input-field" value="90" min="1" max="100">
      </div>
      <div>
        <label>颜色 (HEX):</label>
        <input type="text" id="txtColor" class="input-field" value="FF5500">
      </div>
    </div>
    <button class="btn btn-primary" onclick="sendText()">🚀 发送文字上屏</button>
  </div>

  <!-- 卡片 3: 配置与防扰民睡眠规则 -->
  <div class="card">
    <h2>⚙️ 蓝牙与睡眠防扰民调度</h2>
    <div style="font-size:0.85rem; color:#8b949e; margin-bottom:10px;">当前蓝牙 MAC: <b id="bleMac" style="color:#58a6ff">--</b> (RSSI: <span id="bleRssi">--</span> dBm)</div>
    <form action="/api/pixel/save_cfg" method="POST">
      <label>目标蓝牙 MAC 地址:</label>
      <input type="text" name="mac" id="inputMac" class="input-field">

      <label>白天时钟自动轮换周期 (秒, 填 0 禁用):</label>
      <input type="number" name="cycle" id="inputCycle" class="input-field" min="0" max="3600">

      <div class="row">
        <div><label>定时开机 (如 08:00):</label><input type="text" name="auto_on" id="inputAutoOn" class="input-field" placeholder="08:00"></div>
        <div><label>定时关机 (如 23:30):</label><input type="text" name="auto_off" id="inputAutoOff" class="input-field" placeholder="23:30"></div>
      </div>
      <small style="color:#8b949e;display:block;margin-bottom:10px;">💡 防扰民说明：在此时间段内或关机状态下，时钟轮播强制冻结，即使突发重启也会自动补刀灭屏。</small>

      <button type="submit" class="btn btn-secondary">💾 保存像素屏配置</button>
    </form>
  </div>

  <a href="/" class="nav-link">&lt;&lt; 返回风扇与电源监控主页</a>
</div>

<script>
// ⭐️ 修复：同时兼容 JSON 和普通文本响应，避免报错静默中断
function callApi(url){ 
  fetch(url)
    .then(r => r.text())
    .then(t => {
      refresh();
    })
    .catch(err => {
      console.error(err);
      alert('请求失败，请检查设备连接');
    }); 
}

function sendText(){
  const msg = document.getElementById('txtMsg').value.trim();
  const size = document.getElementById('txtSize').value;
  const anim = document.getElementById('txtAnim').value;
  const speed = document.getElementById('txtSpeed').value;
  const color = document.getElementById('txtColor').value.trim();
  if(!msg) return alert('请输入内容');
  
  const url = `/api/pixel/text?msg=${encodeURIComponent(msg)}&size=${size}&anim=${anim}&speed=${speed}&color=${color}`;
  fetch(url).then(r=>r.text()).then(t=>{
    alert(t.includes('ok') ? '已成功推送到屏幕' : '发送失败，请检查蓝牙');
    refresh();
  }).catch(()=>alert('网络异常'));
}

function refresh(){
  fetch('/api/pixel/status').then(r=>r.json()).then(d=>{
    const b = document.getElementById('bleBadge');
    b.className = 'badge ' + (d.connected ? 'badge-ok' : 'badge-fail');
    b.innerText = d.connected ? (d.power ? '已连通 (开机)' : '已连通 (关机待机)') : '未连接/搜索中';
    document.getElementById('bVal').innerText = d.brightness + '%';
    document.getElementById('bSlider').value = d.brightness;
    document.getElementById('bleMac').innerText = d.mac;
    document.getElementById('bleRssi').innerText = d.rssi;
    if(!document.getElementById('inputMac').value) {
      document.getElementById('inputMac').value = d.mac;
      document.getElementById('inputCycle').value = d.cycle_sec;
      document.getElementById('inputAutoOn').value = d.auto_on;
      document.getElementById('inputAutoOff').value = d.auto_off;
    }
  }).catch(()=>{});
}
window.onload = refresh;
setInterval(refresh, 3000);
</script>
</body>
</html>
)rawliteral";