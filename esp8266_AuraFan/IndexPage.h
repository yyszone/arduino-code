#ifndef INDEX_PAGE_H
#define INDEX_PAGE_H

#include <Arduino.h>

const char MAIN_PAGE_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>AuraFan 控制中心</title>
  <style>
    :root {
      --bg-gradient: radial-gradient(circle at 10% 20%, #f1f5f9 0%, #cbd5e1 90%);
      --card-bg: rgba(255, 255, 255, 0.75);
      --card-border: rgba(255, 255, 255, 0.8);
      --text-main: #1e293b; --text-muted: #64748b;
      --accent: #8b5cf6; --accent-hover: #7c3aed;
      --green: #10b981; --red: #ef4444; --cyan: #06b6d4;
      --shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.05), 0 8px 10px -6px rgba(0, 0, 0, 0.01);
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
      background: var(--bg-gradient); color: var(--text-main);
      display: flex; justify-content: center; min-height: 100vh; padding: 1.25rem;
    }
    .container { width: 100%; max-width: 580px; }
    header {
      display: flex; justify-content: space-between; align-items: center;
      margin-bottom: 1.25rem; padding: 0.2rem 0.5rem;
    }
    .brand { font-size: 1.4rem; font-weight: 800; display: flex; align-items: center; gap: 0.5rem; color: #0f172a; }
    .status-dot { width: 10px; height: 10px; background: var(--green); border-radius: 50%; box-shadow: 0 0 10px var(--green); display: inline-block; }
    .top-chips { display: flex; gap: 0.5rem; flex-wrap: wrap; }
    .chip {
      background: rgba(255, 255, 255, 0.85); border: 1px solid rgba(0,0,0,0.06);
      padding: 0.35rem 0.75rem; border-radius: 2rem; font-size: 0.82rem; font-weight: 600;
      color: var(--text-muted); display: inline-flex; align-items: center; gap: 0.3rem; backdrop-filter: blur(8px);
    }
    .card {
      background: var(--card-bg); backdrop-filter: blur(16px); -webkit-backdrop-filter: blur(16px);
      border-radius: 1.25rem; padding: 1.35rem; margin-bottom: 1.15rem;
      border: 1px solid var(--card-border); box-shadow: var(--shadow);
    }
    h2 { font-size: 1.1rem; font-weight: 700; color: #34495e; margin-bottom: 0.9rem; display: flex; align-items: center; gap: 0.45rem; }
    
    /* 硬件显示屏仿真卡片 */
    .screen-bezel {
      background: #090d16; border-radius: 0.9rem; padding: 0.8rem 1rem;
      box-shadow: inset 0 2px 8px rgba(0,0,0,0.8), 0 4px 12px rgba(0,0,0,0.1);
      border: 2px solid #334155; position: relative; min-height: 160px; display: flex; flex-direction: column; justify-content: space-between;
    }
    .screen-header-line {
      display: flex; justify-content: space-between; font-size: 0.72rem; color: #64748b;
      border-bottom: 1px solid #1e293b; padding-bottom: 0.35rem; font-family: monospace;
    }
    .screen-canvas { padding: 0.6rem 0; flex-grow: 1; }
    .screen-canvas p { font-family: "Courier New", Courier, monospace; margin: 3px 0; font-weight: 700; }
    
    /* 🌟 高饱和色相流转动画 (赤红 <-> 耀眼金，绝不发暗) */
    @keyframes alertHueShift {
      0%, 100% { color: #ff2233; text-shadow: 0 0 8px rgba(255, 34, 51, 0.75); }
      50% { color: #f59e0b; text-shadow: 0 0 10px rgba(245, 158, 11, 0.9); }
    }
    .off-label { color: #94a3b8; font-weight: bold; }
    .off-ips-glow {
      animation: alertHueShift 2.2s infinite ease-in-out;
      font-weight: 800;
      letter-spacing: 0.5px;
    }

    .screen-footer-bar {
      display: flex; justify-content: space-between; align-items: center;
      border-top: 1px solid #1e293b; padding-top: 0.35rem; margin-top: 0.4rem;
    }
    .screen-ip-tag { font-size: 0.72rem; font-family: monospace; color: #38bdf8; font-weight: 600; }
    .rly-tag {
      font-size: 0.7rem; font-family: monospace; font-weight: bold; padding: 2px 8px;
      border-radius: 3px; border: 1px solid #10b981; color: #10b981; background: rgba(16, 185, 129, 0.15);
    }
    .rly-tag.off { border-color: #64748b; color: #94a3b8; background: rgba(100, 116, 139, 0.15); }

    /* 风扇监控 */
    .metric-row { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 0.6rem; }
    .metric-val { font-size: 1.25rem; font-weight: 800; color: var(--accent); font-family: monospace; }
    .slider {
      width: 100%; height: 8px; -webkit-appearance: none; background: #e2e8f0;
      border-radius: 4px; outline: none; transition: 0.2s;
    }
    .slider::-webkit-slider-thumb {
      -webkit-appearance: none; width: 22px; height: 22px; border-radius: 50%;
      background: var(--accent); border: 3px solid #fff; box-shadow: 0 2px 6px rgba(0,0,0,0.2); cursor: pointer;
    }

    /* 灯效按钮组 */
    .btn-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 0.5rem; margin-top: 0.8rem; }
    .btn {
      padding: 0.65rem 0.5rem; border: none; border-radius: 0.75rem; font-weight: 700;
      cursor: pointer; background: #fff; color: var(--text-main); font-size: 0.85rem;
      border: 1px solid rgba(0,0,0,0.06); transition: all 0.2s; text-align: center;
    }
    .btn:hover { transform: translateY(-2px); box-shadow: 0 4px 10px rgba(0,0,0,0.08); }
    .btn.active { background: var(--accent); color: #fff; border-color: var(--accent); }
    .color-btn { display: flex; align-items: center; justify-content: center; gap: 0.4rem; }
    input[type=color] { width: 28px; height: 28px; border: none; border-radius: 50%; cursor: pointer; background: none; }

    /* 继电器卡片 */
    .relay-box { display: flex; justify-content: space-between; align-items: center; }
    .relay-state-label { font-size: 1.15rem; font-weight: 800; }
    .status-on { color: var(--green); }
    .status-off { color: var(--red); }
    .btn-toggle {
      background: var(--accent); color: #fff; padding: 0.65rem 1.2rem;
      border-radius: 0.75rem; border: none; font-weight: 700; text-decoration: none; cursor: pointer;
    }

    footer { text-align: center; margin: 1.5rem 0 1rem; font-size: 0.85rem; color: var(--text-muted); }
    footer a { color: var(--accent); font-weight: 700; text-decoration: none; }
    footer a:hover { text-decoration: underline; }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="brand"><span class="status-dot"></span> AuraFan 控制中心</div>
      <div class="top-chips">
        <span class="chip" id="chipUptime">⏱️ --:--:--</span>
        <span class="chip" id="chipRpm" style="color:var(--cyan);">0 RPM</span>
      </div>
    </header>

    <div class="card">
      <h2>🖥️ ILI9341 屏幕实况监视</h2>
      <div class="screen-bezel">
        <div class="screen-header-line">
          <span>AURAFAN MONITOR</span>
          <span id="screenOnline">[ONLINE]</span>
        </div>
        <div class="screen-canvas" id="screenLines">
          <p style="color:#64748b;">屏幕暂无文本内容</p>
        </div>
        <div class="screen-footer-bar">
          <span class="screen-ip-tag" id="screenIpTag">IP: 192.168.xx.xx</span>
          <span class="rly-tag" id="screenRlyBadge">RLY: ON</span>
        </div>
      </div>
    </div>

    <div class="card">
      <h2>⚡ 5V 继电器管理</h2>
      <div class="relay-box">
        <div>当前状态: <strong id="relayStatusText" class="relay-state-label"><span class="status-on">开启</span></strong></div>
        <div style="display:flex; gap:0.5rem;">
          <button class="btn" onclick="toggleRelay()">一键切换</button>
          <a href="/LED-Control" class="btn-toggle">定时排期 ➡</a>
        </div>
      </div>
    </div>

    <div class="card">
      <h2>🌪️ 智能风扇调速</h2>
      <div class="metric-row">
        <span>当前设定转速</span>
        <span class="metric-val" id="fanLabel">0 (0%)</span>
      </div>
      <input id="fanSlider" class="slider" type="range" min="0" max="255" value="0">
    </div>

    <div class="card">
      <h2>✨ WS2812B 幻彩灯控</h2>
      <div class="btn-grid">
        <button id="btn_rainbow" class="btn" onclick="setMode('rainbow')">🌈 彩虹</button>
        <button id="btn_fade" class="btn" onclick="setMode('fade')">🫁 呼吸</button>
        <button id="btn_static" class="btn color-btn" onclick="clickColor()">
          🎨 单色 <input id="colorPicker" type="color" value="#8b00ff">
        </button>
        <button id="btn_off" class="btn" onclick="setMode('off')">🌑 关灯</button>
      </div>
      <div class="metric-row" style="margin-top:1rem;">
        <span style="font-size:0.9rem; font-weight:600; color:var(--text-muted);">点亮灯珠数量</span>
        <span class="metric-val" id="ledCountVal" style="font-size:1rem;">120</span>
      </div>
      <input id="ledCountSlider" class="slider" type="range" min="0" max="120" value="120">
    </div>

    <div class="card" style="font-size:0.9rem; line-height:1.7;">
      <h2>⚙️ 系统诊断</h2>
      <div>设备局域网 IP: <strong id="ipAddr">...</strong></div>
      <div>上次引导原因: <code id="resetReasonCode" style="background:#f1f5f9; padding:2px 6px; border-radius:4px;">...</code></div>
      <div style="margin-top:0.4rem;"><a href="/update" style="color:var(--accent); font-weight:700; text-decoration:none;">前往固件 OTA 升级 ➡</a></div>
    </div>

    <footer>
      来自脚本 <a href="https://github.com/yyszone/arduino-code/tree/main/esp8266_AuraFan" target="_blank">esp8266_AuraFan</a>
    </footer>
  </div>

  <script>
    const fanSlider = document.getElementById('fanSlider'),
          fanLabel = document.getElementById('fanLabel'),
          colorPicker = document.getElementById('colorPicker'),
          ledCountSlider = document.getElementById('ledCountSlider'),
          ledCountVal = document.getElementById('ledCountVal'),
          screenLines = document.getElementById('screenLines'),
          screenIpTag = document.getElementById('screenIpTag'),
          screenRlyBadge = document.getElementById('screenRlyBadge'),
          relayStatusText = document.getElementById('relayStatusText'),
          chipRpm = document.getElementById('chipRpm'),
          chipUptime = document.getElementById('chipUptime');

    function updateBtnActive(activeMode) {
      ['rainbow', 'fade', 'static', 'off'].forEach(m => {
        const el = document.getElementById('btn_' + m);
        if (el) el.classList.toggle('active', m === activeMode);
      });
    }

    fanSlider.oninput = () => {
      const v = fanSlider.value;
      fanLabel.innerText = `${v} (${Math.round(v/255*100)}%)`;
      fetch(`/setSpeed?value=${v}`).catch(console.error);
    };

    ledCountSlider.oninput = () => {
      const v = ledCountSlider.value;
      ledCountVal.innerText = v;
      fetch(`/setActiveLeds?value=${v}`).catch(console.error);
    };

    colorPicker.oninput = () => {
      const hex = colorPicker.value;
      const r = parseInt(hex.slice(1,3),16), g = parseInt(hex.slice(3,5),16), b = parseInt(hex.slice(5,7),16);
      fetch(`/setRGB?r=${r}&g=${g}&b=${b}`).then(() => updateBtnActive('static')).catch(console.error);
    };

    function clickColor() { colorPicker.click(); }
    function setMode(m) { fetch(`/setLedMode?mode=${m}`).then(() => updateBtnActive(m)).catch(console.error); }
    function toggleRelay() { fetch('/relay/api/toggle').then(() => fetchAllState()).catch(console.error); }

    function fetchAllState() {
      fetch('/getState').then(r => r.json()).then(s => {
        fanSlider.value = s.fanSpeed;
        fanLabel.innerText = `${s.fanSpeed} (${Math.round(s.fanSpeed/255*100)}%)`;
        chipRpm.innerText = `${s.rpm} RPM`;
        chipUptime.innerText = `⏱️ ${s.uptime}`;
        document.getElementById('ipAddr').innerText = s.ip;
        document.getElementById('resetReasonCode').innerText = s.resetReason;
        screenIpTag.innerText = 'IP: ' + s.ip;
        
        if (s.relayState) {
          relayStatusText.innerHTML = "<span class='status-on'>开启</span>";
          screenRlyBadge.className = "rly-tag";
          screenRlyBadge.innerText = "RLY: ON";
        } else {
          relayStatusText.innerHTML = "<span class='status-off'>关闭</span>";
          screenRlyBadge.className = "rly-tag off";
          screenRlyBadge.innerText = "RLY: OFF";
        }

        updateBtnActive(s.ledMode);
        ledCountSlider.max = s.maxLeds;
        ledCountSlider.value = s.activeLeds;
        ledCountVal.innerText = s.activeLeds;
        colorPicker.value = `#${s.ledR.toString(16).padStart(2,'0')}${s.ledG.toString(16).padStart(2,'0')}${s.ledB.toString(16).padStart(2,'0')}`;

        // 🌟 网页仿真器：前缀 OFF: 冷灰固定，仅括号文字呈现高饱和金-红律动
        if (s.lines && s.lines.length > 0) {
          screenLines.innerHTML = s.lines.map(l => {
            if (l.text.startsWith('OFF:')) {
              const parts = l.text.split('OFF:');
              return `<p><span class="off-label">OFF:</span><span class="off-ips-glow"> ${parts[1]}</span></p>`;
            }
            return `<p style="color:${l.color}; font-size:${l.size * 7 + 5}px;">${l.text}</p>`;
          }).join('');
        } else {
          screenLines.innerHTML = '<p style="color:#64748b;">屏幕暂无文本内容</p>';
        }
      }).catch(console.error);
    }

    window.onload = fetchAllState;
    setInterval(fetchAllState, 1500);
  </script>
</body>
</html>
)rawliteral";

#endif