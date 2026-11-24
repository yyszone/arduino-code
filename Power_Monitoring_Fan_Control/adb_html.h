#pragma once

const char ADB_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<meta name="mobile-web-app-capable" content="yes"><meta name="theme-color" content="#0f172a">
<title>ADB 手机电量 & 智能充电</title>
<style>
  :root{--bg:#0f172a;--card:#111827;--text:#e5e7eb;--primary:#38bdf8;--accent:#22c55e;--warning:#f59e0b;--danger:#ef4444;--muted:#94a3b8}
  *{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:linear-gradient(135deg,#0f172a,#1f2937);color:var(--text);padding:16px}
  .container{max-width:760px;margin:0 auto}
  .card{background:#0b1220;border:1px solid rgba(56,189,248,0.2);border-radius:14px;padding:18px;margin-bottom:16px;box-shadow:0 10px 25px rgba(0,0,0,0.3)}
  h1{font-size:20px;text-align:center;margin:0 0 16px;color:#38bdf8}h2{font-size:16px;margin:0 0 10px;color:#f3f4f6}
  input[type=text],input[type=number],textarea,select{width:100%;background:rgba(0,0,0,0.3);border:1px solid #374151;color:#fff;padding:9px;border-radius:8px;margin:6px 0 12px;font-size:14px}
  textarea{font-family:monospace;font-size:11px;height:85px}
  button{background:var(--primary);color:#082f49;border:none;padding:8px 14px;border-radius:8px;font-weight:700;cursor:pointer;font-size:14px}
  button.del{background:var(--danger);color:#fff;padding:5px 8px;font-size:12px}
  button.test{background:var(--accent);color:#052e16;padding:5px 10px;font-size:12px;margin-right:4px}
  table{width:100%;border-collapse:collapse;margin-top:6px}th,td{text-align:left;padding:9px 4px;border-bottom:1px solid #1f2937;font-size:13px}
  .badge{display:inline-block;padding:3px 8px;border-radius:12px;font-size:12px;font-weight:bold;color:#fff}
  .badge-gray{background:#4b5563}.badge-green{background:var(--accent);color:#052e16}.badge-orange{background:var(--warning);color:#451a03}.badge-red{background:var(--danger)}
  .tip{font-size:12px;color:var(--muted);background:rgba(255,255,255,0.04);padding:8px 12px;border-radius:6px;margin-bottom:12px;line-height:1.5}
  a.nav{display:inline-block;margin-right:15px;color:var(--primary);text-decoration:none;font-weight:bold;font-size:14px}
  #toast{visibility:hidden;min-width:180px;background:#22c55e;color:#052e16;font-weight:bold;text-align:center;border-radius:20px;padding:9px 16px;position:fixed;z-index:99;left:50%;bottom:30px;transform:translateX(-50%)}
  #toast.show{visibility:visible}
</style>
</head><body><div class="container">
<h1>🔋  ESP32 ADB 手机电量监控 & 自动充电联动</h1>
<div id="toast"></div>

<!-- 卡片 1: 设备列表 -->
<div class="card">
  <h2>📱 安卓设备监控</h2>
  <div class="tip">支持绑定<b>本地插座</b>或<b>自定义外部插座IP</b>。低于阈值自动发送开机指令并开启40分钟倒计时。</div>
  
  <div style="display:flex;gap:8px;flex-wrap:wrap">
    <input type="text" id="dev_ip" placeholder="输入手机 IP (如 192.168.31.182)" style="flex:2;min-width:150px;margin:0">
    <select id="dev_relay" onchange="onRelayChange(this.value)" style="flex:1.5;min-width:140px;margin:0">
      <option value="0">不绑定插座</option>
      <option value="2">本地插座1 (GPIO6)</option>
      <option value="3">本地插座2 (GPIO7)</option>
      <option value="ext">外部插座 IP...</option>
    </select>
    <input type="text" id="ext_ip_input" placeholder="输入外部插座IP/URL" style="display:none;flex:1.5;min-width:140px;margin:0">
    <button onclick="addDevice()">添加设备</button>
  </div>

  <table>
    <thead><tr><th>设备 IP</th><th>绑定插座</th><th>连接状态</th><th>当前电量</th><th>更新时间</th><th>操作</th></tr></thead>
    <tbody id="dev_tbody"></tbody>
  </table>
</div>

<!-- 卡片 2: 自动化参数 -->
<div class="card">
  <h2>⚙️ 自动充电与轮询设置</h2>
  <div style="display:flex;gap:12px">
    <div style="flex:1"><label>低电量阈值 (%):</label><input type="number" id="cfg_th" min="5" max="95"></div>
    <div style="flex:1"><label>充电时长 (分钟):</label><input type="number" id="cfg_dur" min="5" max="240" value="40"></div>
    <div style="flex:1"><label>排队轮询周期 (分钟):</label><input type="number" id="cfg_int" min="1" max="1440"></div>
    <div style="flex:1"><label>重试次数:</label><input type="number" id="cfg_retries" min="1" max="5"></div>
  </div>
  <button style="width:100%" onclick="saveSettings()">保存充电与轮询配置</button>
</div>

<!-- 卡片 3: 私钥 -->
<div class="card">
  <h2>🔐 电脑 ADB 私钥 (adbkey)</h2>
  <div class="tip">Windows: <code>%USERPROFILE%\.android\adbkey</code> | Linux: <code>~/.android/adbkey</code> (勿选 .pub)</div>
  <textarea id="adb_key"></textarea>
  <button style="width:100%;background:var(--accent);color:#052e16" onclick="saveKey()">保存私钥至 Flash 闪存</button>
</div>

<!-- 导航 -->
<div style="text-align:center;margin-top:15px">
  <a href="/" class="nav">&lt;&lt; 返回智能风扇主页</a>
  <a href="/socket" class="nav">&gt;&gt; 前往双路插座 &amp; NAS 控制</a>
</div>

<script>
function toast(m){const t=document.getElementById('toast');t.innerText=m;t.className='show';setTimeout(()=>t.className='',2000)}

function onRelayChange(val) {
  const extInput = document.getElementById('ext_ip_input');
  extInput.style.display = (val === 'ext') ? 'block' : 'none';
  if(val === 'ext') extInput.focus();
}

function renderTable(data){
  const tb = document.getElementById('dev_tbody');
  if(!data.devices || data.devices.length === 0){
    tb.innerHTML = '<tr><td colspan="6" style="color:var(--muted);text-align:center">暂无设备</td></tr>'; return;
  }
  tb.innerHTML = '';
  data.devices.forEach(d=>{
    let bClass = 'badge badge-gray';
    let bText = d.battery >= 0 ? d.battery + '% 🔋' : '--';
    if(d.battery >= 0){
      if(d.battery <= data.threshold) bClass = 'badge badge-red';
      else if(d.battery < 50) bClass = 'badge badge-orange';
      else bClass = 'badge badge-green';
      if(d.charging) bText = '⚡ ' + d.battery + '%';
    }

    // 插座名称显示
    let rName = '无';
    let isExt = false;
    if(d.relay === '2') rName = '本地插座 1';
    else if(d.relay === '3') rName = '本地插座 2';
    else if(d.relay !== '0' && d.relay.length > 0) {
      rName = d.relay; // 显示外部 IP
      isExt = true;
    }

    // 组装悬浮框诊断信息
    let tip = [];
    if(d.relay !== '0' && d.relay.length > 0) {
      if(d.relay_on) {
        if(d.adb_triggered && d.charge_rem > 0) {
          tip.push(`⚡ 供电来源：ADB 低电量触发 (${isExt ? '外部设备' : '本地继电器'}, 剩余 ${Math.ceil(d.charge_rem/60)} 分钟)`);
        } else {
          tip.push(`🔌 供电来源：外部开启 (手动按键 / 开机40分保护 / 定时)`);
        }
      } else {
        tip.push(`⚪ 插座状态：当前断电中`);
      }
    } else {
      tip.push(`⚪ 未绑定插座`);
    }

    if(d.battery >= 0) {
      tip.push(d.charging ? '🔋 手机状态：正在充电 (检测到电流)' : '🔋 手机状态：未充电 (请检查接线)');
    }
    let tooltipText = tip.join('\n');

    let relayHtml = '无';
    if(d.relay !== '0' && d.relay.length > 0) {
      let dotColor = d.relay_on ? '#22c55e' : '#6b7280';
      relayHtml = `<span title="${tooltipText}" style="cursor:help;text-decoration:underline dotted;color:${dotColor}">
        ${rName} ${d.relay_on ? '●' : '○'}
      </span>`;
    }

    tb.innerHTML += `<tr>
      <td><b>${d.ip}</b></td>
      <td>${relayHtml}</td>
      <td><span class="${d.connected ? 'badge badge-green':'badge badge-red'}">${d.msg}</span></td>
      <td><span class="${bClass}" title="${tooltipText}" style="cursor:help">${bText}</span></td>
      <td style="color:var(--muted);font-size:11px;white-space:nowrap">${d.lastTime}</td>
      <td>
        <button class="test" onclick="testDev('${d.ip}')">检测</button>
        <button class="del" onclick="delDev('${d.ip}')">删除</button>
      </td>
    </tr>`;
  });
}

function refreshData(){
  fetch('/getAdbData').then(r=>r.json()).then(data=>{
    document.getElementById('cfg_th').value = data.threshold;
    document.getElementById('cfg_dur').value = data.duration;
    document.getElementById('cfg_int').value = data.interval;
    document.getElementById('cfg_retries').value = data.retries;
    if(data.key) document.getElementById('adb_key').value = data.key;
    renderTable(data);
  }).catch(e=>console.error(e));
}

function saveKey(){
  const k = document.getElementById('adb_key').value.trim();
  if(!k) return alert('私钥不能为空！');
  const fd = new FormData(); fd.append('key', k);
  fetch('/setAdbKey', {method:'POST', body:fd}).then(()=>{toast('✅ 私钥已保存'); refreshData();});
}

function saveSettings(){
  const fd = new FormData();
  fd.append('th', document.getElementById('cfg_th').value);
  fd.append('dur', document.getElementById('cfg_dur').value);
  fd.append('int', document.getElementById('cfg_int').value);
  fd.append('ret', document.getElementById('cfg_retries').value);
  fetch('/setAdbSettings', {method:'POST', body:fd}).then(()=>{toast('✅ 配置已更新'); refreshData();});
}

function addDevice(){
  const ip = document.getElementById('dev_ip').value.trim();
  if(!ip) return alert('请输入手机 IP');
  
  let relay = document.getElementById('dev_relay').value;
  if(relay === 'ext') {
    relay = document.getElementById('ext_ip_input').value.trim();
    if(!relay) return alert('请输入外部插座 IP 地址！');
  }

  const fd = new FormData(); 
  fd.append('ip', ip); 
  fd.append('relay', relay);

  fetch('/addAdbDevice', {method:'POST', body:fd}).then(()=>{
    document.getElementById('dev_ip').value='';
    document.getElementById('ext_ip_input').value='';
    document.getElementById('dev_relay').value='0';
    document.getElementById('ext_ip_input').style.display='none';
    refreshData();
  });
}

function delDev(ip){
  if(!confirm('确定删除 '+ip+'?')) return;
  const fd = new FormData(); fd.append('ip', ip);
  fetch('/delAdbDevice', {method:'POST', body:fd}).then(()=>refreshData());
}

function testDev(ip){
  toast('⏳ 正在排队连接 '+ip+'...');
  fetch('/testAdbDevice?ip='+ip).then(r=>r.json()).then(()=>refreshData());
}

window.onload = refreshData;
setInterval(refreshData, 3000);
</script></body></html>
)rawliteral";