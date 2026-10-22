#ifndef MAIN_HTML_H
#define MAIN_HTML_H

// 将长 HTML/JavaScript 放入 .h，避免 Arduino .ino 自动预处理器
// 把 raw string 里的 JavaScript 误识别成 C++ 代码。
#include <Arduino.h>

const char MAIN_HTML_PART1[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32-C3 智能继电器</title><style>:root{--bg-color:#111827;--card-color:#1f2937;--text-color:#d1d5db;--accent-color:#38bdf8;--green-color:#22c55e;--red-color:#ef4444;--warning-color:#f59e0b;--muted-text:#9ca3af}body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";background-color:var(--bg-color);color:var(--text-color);margin:0;padding:15px;display:flex;justify-content:center}h1,h2,h4{margin-top:0;color:#fff;text-align:center}h2{border-top:1px solid #374151;padding-top:15px;margin-top:20px}.container{width:100%;max-width:500px}.card{background-color:var(--card-color);border-radius:12px;padding:20px;margin-bottom:15px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1),0 2px 4px -1px rgba(0,0,0,.06)}.chart-card{padding:20px 0 10px 0;}.chart-card h2{padding:0 20px 15px;margin:0;border:none;}.data-box{text-align:center;padding:10px}.data-box .val{font-size:2.5em;font-weight:700;color:var(--accent-color);line-height:1.2;transition:color .3s ease}.data-box .unit{color:var(--muted-text)}.btn{width:100%;padding:15px;font-size:1.2em;font-weight:bold;border:none;border-radius:8px;cursor:pointer;transition:background-color .2s ease}.btn.on{background-color:var(--green-color);color:#fff}.btn.off{background-color:var(--red-color);color:#fff}.status-light{width:12px;height:12px;border-radius:50%;display:inline-block;margin-right:8px;background-color:#6b7280}.status-light.on{background-color:var(--green-color)}.input-group{display:flex;align-items:center;gap:10px;margin-bottom:10px}.input-group label{flex-basis:120px;flex-shrink:0;font-size:0.9em}input[type=number],input[type=text],input[type=password]{width:100%;padding:8px;background-color:#374151;border:1px solid #4b5563;border-radius:6px;color:var(--text-color);font-size:1em}.btn-save{padding:10px 15px;background-color:var(--accent-color);color:#fff;border:none;border-radius:6px;cursor:pointer}#sysinfo{font-size:.8em;color:var(--muted-text);word-break:break-all}#lockoutStatus{color:var(--red-color);text-align:center;margin-bottom:10px;font-weight:bold;}.toggle-section{cursor:pointer;color:var(--accent-color);text-align:center;font-size:0.9em;margin-top:10px;}</style></head><body>
<div class="container"><h1>ESP32-C3 智能继电器</h1><p style="text-align:center;color:var(--muted-text);font-size:0.8em;margin-bottom:2px;">来自脚本 ESP32_INA219_r_v.ino.ino</p><p style="text-align:center;color:var(--muted-text);margin-top:2px;">当前时间: <span id="currentTime">--:--:--</span></p><div class="card"><div class="data-box"><div>电池电压</div><div class="val" id="v">--</div><div class="unit">V</div></div></div><div class="card"><h2>手动控制</h2><div id="lockoutStatus" style="display:none;"></div><p><span id="relayStatusLight" class="status-light"></span>继电器状态: <strong id="relayStatusText">读取中...</strong></p><button id="relayBtn" class="btn">读取中...</button></div><div class="card"><h2>参数设置</h2><div class="input-group"><label for="highV">高压开启 (V)</label><input type="number" id="highV" step="0.1"></div><div class="input-group"><label for="warnV">电压警告 (V)</label><input type="number" id="warnV" step="0.1"></div><div class="input-group"><label for="lowV">低压关闭 (V)</label><input type="number" id="lowV" step="0.1"></div>
<p style="font-size:0.85em;color:var(--muted-text);text-align:right;">
  邮件通知将发送至: 
  <span id="dispRecvEmail" 
    style="color:var(--accent-color);cursor:pointer;text-decoration:underline dotted;" 
    title="点击发送测试邮件"
    onclick="sendTestEmail()">未设置</span>
</p>
<p id="testEmailStatus" style="font-size:0.8em;text-align:right;margin:0;min-height:1.2em;"></p>
<div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveSettings()">保存电压设置</button></div></div>
)HTML";

const char MAIN_HTML_PART2[] PROGMEM = R"HTML(
<div class="card"><h2>邮件通知设置</h2><div class="input-group"><label>SMTP服务器</label><input type="text" id="smtpHost" placeholder="如: smtp.qq.com"></div><div class="input-group"><label>SMTP端口</label><input type="number" id="smtpPort" placeholder="465"></div><div class="input-group"><label>发件邮箱</label><input type="text" id="authEmail" placeholder="xxxx@qq.com"></div><div class="input-group"><label>授权码/密码</label><input type="password" id="authPass"></div><div class="input-group"><label>收件邮箱</label><input type="text" id="recvEmail" placeholder="接收通知的邮箱"></div><div style="text-align:right;margin-top:10px;"><button class="btn-save" onclick="saveEmailConfig()">保存邮件配置</button></div></div>

<div class="card">
  <h2>API 通知设置</h2>

  <div class="input-group">
    <label>通知 API</label>
    <input
      type="text"
      id="notifyApi"
      placeholder="https://notify.yysresume.work/api/send">
  </div>

  <p style="font-size:0.8em;color:var(--muted-text);">
    用于电压警告 API 推送通知
  </p>

  <div style="text-align:right;margin-top:10px;">
    <button class="btn-save" onclick="saveNotifyConfig()">
      保存 API 配置
    </button>
  </div>

  <p id="notifyStatus"
     style="font-size:0.8em;text-align:right;margin:8px 0 0;">
  </p>
</div>

<div class="card"><h2>系统信息与更新</h2>
<a href="/clock" style="display:block; text-align:center; background:linear-gradient(90deg, #ff007f, #7f00ff); color:#fff; padding:12px; border-radius:8px; text-decoration:none; margin-bottom:15px; font-weight:bold; box-shadow: 0 4px 10px rgba(255,0,127,0.3);">✨ 进入矩阵时钟控制台</a><div id="sysinfo">加载中...</div><h4>固件更新 (OTA)</h4><div id="otaUi"><form id="otaForm" method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update" accept=".bin,.bin.gz" required><button type="submit" class="btn-save" style="margin-top:10px;">上传并更新</button></form></div><div id="otaStatus"></div></div><div class="card chart-card"><h2>24小时电压曲线</h2><div id="voltageChart" style="width: 100%; height: 250px;"></div></div>
<a href="/logs" style="display:block;text-align:center;background:#1f2937;
color:#34d399;padding:10px;border-radius:8px;text-decoration:none;
margin-bottom:10px;font-size:13px;">📋 查看系统日志</a>
</div>


<script>
var echartInstance = null;
var latestDeviceTimeStr = "--:--:--";
const DATA_INTERVAL_MIN = 1;
let deviceClockBaseSeconds = null;
let deviceClockSyncPerf = 0;
let dataFetchInFlight = false;
let chartLoadStarted = false;
function $(s){return document.getElementById(s)}
function fetchJson(url){return fetch(url, { cache: 'no-store' }).then(r=>{if(!r.ok)throw new Error('Network error');return r.json()})}

// 本地每秒走表；HTTP 只负责定期校准，因此图表加载或断网不会冻结这段显示。
function syncDeviceClock(timeText) {
  if (!timeText || timeText === '--:--:--') return;
  const parts = timeText.split(':').map(Number);
  if (parts.length !== 3 || parts.some(v => !Number.isFinite(v))) return;
  deviceClockBaseSeconds = parts[0] * 3600 + parts[1] * 60 + parts[2];
  deviceClockSyncPerf = performance.now();
  latestDeviceTimeStr = timeText;
  renderDeviceClock();
}
function renderDeviceClock() {
  if (deviceClockBaseSeconds === null) return;
  const elapsed = Math.floor((performance.now() - deviceClockSyncPerf) / 1000);
  const seconds = (deviceClockBaseSeconds + elapsed) % 86400;
  const hh = String(Math.floor(seconds / 3600)).padStart(2, '0');
  const mm = String(Math.floor((seconds % 3600) / 60)).padStart(2, '0');
  const ss = String(seconds % 60).padStart(2, '0');
  $('currentTime').textContent = `${hh}:${mm}:${ss}`;
}
function updateStatus(data){
  const relayOn=data.relay;
  $('relayStatusText').textContent=relayOn?'已开启':'已关闭';
  $('relayStatusText').style.color=relayOn?'var(--green-color)':'var(--red-color)';
  $('relayStatusLight').className=relayOn?'status-light on':'status-light';
  $('relayBtn').textContent=relayOn?'关闭继电器':'开启继电器';
  $('relayBtn').className=relayOn?'btn off':'btn on';
  if(data.lockout){$('lockoutStatus').style.display='block';$('lockoutStatus').textContent='电压警告中！剩余 '+data.lockout_rem+' 分钟可自动恢复。'}else{$('lockoutStatus').style.display='none';}
}

function fetchData(){
  if (dataFetchInFlight) return Promise.resolve();
  dataFetchInFlight = true;
  return fetchJson('/getData').then(data=>{
    $('v').textContent=data.voltage.toFixed(2);
    $('v').style.color=data.voltage_warning?'var(--warning-color)':'var(--accent-color)';
    syncDeviceClock(data.time);
    updateStatus(data);
  }).catch(()=>{}).finally(()=>{ dataFetchInFlight = false; });
}
function fetchInitialState(){fetchJson('/getStatus').then(data=>{
  updateStatus(data);
  $('lowV').value=data.low_v;
  $('warnV').value=data.warn_v;
  $('highV').value=data.high_v;
  $('sysinfo').innerHTML=`IPv4: ${data.ip}<br>IPv6: ${data.ipv6}<br>芯片ID: ${data.chip_id}<br>CPU繁忙度: ${data.cpu_usage}%<br>内存(RAM): ${data.free_heap} / ${data.total_heap} KB<br>存储(Flash): ${data.fs_free} / ${data.fs_total} KB`;
  $('smtpHost').value = data.mail_host;
  $('smtpPort').value = data.mail_port;
  $('authEmail').value = data.mail_user;
  $('authPass').value = data.mail_pass;
  $('recvEmail').value = data.mail_to;
  $('notifyApi').value = data.notify_api || '';
  if(data.mail_to && data.mail_to.length > 3) $('dispRecvEmail').textContent = data.mail_to;
})}
function saveSettings(){
  const lowV=$('lowV').value; const warnV=$('warnV').value; const highV=$('highV').value;
  fetch(`/setSettings?low=${lowV}&warn=${warnV}&high=${highV}`).then(r=>{if(r.ok){alert('电压设置已保存!')}else{alert('保存失败!')}}).catch(e=>alert('请求出错: '+e));
}

function sendTestEmail() {
  const addr = $('dispRecvEmail').textContent;
  if (!addr || addr === '未设置') {
    $('testEmailStatus').style.color = 'var(--warning-color)';
    $('testEmailStatus').textContent = '请先保存邮件配置';
    return;
  }
  $('testEmailStatus').style.color = 'var(--muted-text)';
  $('testEmailStatus').textContent = '发送中...';

  fetch('/testEmail').then(r => {
    if (!r.ok) return r.text().then(t => { throw new Error(t); });
    let attempts = 0;
    const poll = setInterval(() => {
      attempts++;
      fetch('/testEmailResult').then(r => r.text()).then(result => {
        if (result === 'pending') return; 
        clearInterval(poll);
        if (result === 'ok') {
          $('testEmailStatus').style.color = 'var(--green-color)';
          $('testEmailStatus').textContent = '✓ 发送成功';
        } else {
          $('testEmailStatus').style.color = 'var(--red-color)';
          $('testEmailStatus').textContent = '✗ ' + result.replace('fail:', '');
        }
      });
      if (attempts > 30) { 
        clearInterval(poll);
        $('testEmailStatus').style.color = 'var(--warning-color)';
        $('testEmailStatus').textContent = '⚠ 超时，请查收邮件确认';
      }
    }, 2000); 
  }).catch(e => {
    $('testEmailStatus').style.color = 'var(--red-color)';
    $('testEmailStatus').textContent = '✗ 请求出错: ' + e;
  });
}

function saveEmailConfig(){
  const host=$('smtpHost').value;
  const port=$('smtpPort').value;
  const user=$('authEmail').value;
  const pass=$('authPass').value;
  const to=$('recvEmail').value;
  const url = `/setEmail?host=${encodeURIComponent(host)}&port=${port}&user=${encodeURIComponent(user)}&pass=${encodeURIComponent(pass)}&to=${encodeURIComponent(to)}`;
  fetch(url).then(r=>{if(r.ok){alert('邮件配置已保存! 以后将发送通知至: '+to);$('dispRecvEmail').textContent = to;} else { alert('保存失败!'); }}).catch(e=>alert('请求出错: '+e));
}
$('relayBtn').addEventListener('click',()=>{const newState=$('relayBtn').classList.contains('on');fetch('/setRelay?state='+(newState?'1':'0')).then(()=>setTimeout(fetchData,200))});
$('otaForm').addEventListener('submit', function(e){$('otaUi').style.display='none';$('otaStatus').innerHTML='<h4>正在上传并更新...</h4><p>请勿关闭此页面或断开设备电源。设备将在大约一分钟后自动重启。</p>';});

async function initChart(){
  if (!window.echarts) return;
  const chartDom = $('voltageChart');
  echartInstance = echarts.init(chartDom);
  echartInstance.showLoading({ text: '正在加载历史数据...' });
  try {
    const chartData = await fetchJson('/getChartData');
    const option={
      tooltip:{
        trigger:'axis',
        formatter: function(params){
          const point = params[0];
          if (point.value === null || point.value === '-') return null;
          const dataIndex = point.dataIndex;
          const voltage = parseFloat(point.value).toFixed(2);
          const minutesAgo = (chartData.data.length - 1 - dataIndex) * DATA_INTERVAL_MIN;
          
          let now = new Date();
          if (latestDeviceTimeStr !== "--:--:--") {
            const timeParts = latestDeviceTimeStr.split(':');
            now.setHours(timeParts[0], timeParts[1], timeParts[2]);
          }
          now.setMinutes(now.getMinutes() - minutesAgo);
          const historicalDate = `${now.getFullYear()}-${(now.getMonth()+1).toString().padStart(2,'0')}-${now.getDate().toString().padStart(2,'0')}`;
          const historicalTime = `${now.getHours().toString().padStart(2,'0')}:${now.getMinutes().toString().padStart(2,'0')}`;
          
          return `${historicalDate} ${historicalTime}<br/>电压: <strong>${voltage} V</strong>`;
        }
      },
      grid:{left:'8%',right:'4%',bottom:'10%',containLabel:false},
      xAxis:{type:'category',boundaryGap:false,data:chartData.labels,axisLine:{lineStyle:{color:'var(--muted-text)'}},axisTick:{show:false}},
      yAxis:{type:'value',name:'电压 (V)',nameTextStyle:{color:'var(--muted-text)',padding:[0,0,0,35]},min:'dataMin',max:'dataMax',axisLine:{show:true,lineStyle:{color:'var(--muted-text)'}},splitLine:{lineStyle:{color:'rgba(255,255,255,0.1)'}}},
      series:[{name:'电压',type:'line',smooth:true,connectNulls:false,data:chartData.data,symbol:'none',areaStyle:{color:new echarts.graphic.LinearGradient(0,0,0,1,[{offset:0,color:'rgba(56,189,248,0.5)'},{offset:1,color:'rgba(56,189,248,0.1)'}])},lineStyle:{color:'var(--accent-color)'}}]
    };
    echartInstance.hideLoading();
    echartInstance.setOption(option);
    window.addEventListener('resize',()=>echartInstance.resize());
  } catch (error) { echartInstance.hideLoading(); console.error(error); }
}
async function updateChart(){ if(!echartInstance || !window.echarts)return; try { const chartData=await fetchJson('/getChartData'); echartInstance.setOption({xAxis:{data:chartData.labels},series:[{data:chartData.data}]}); } catch(e){} }

// ECharts 从 CDN 异步加载，不再阻塞首页时间、继电器状态等基础功能。
function loadEChartsAsync(){
  if (chartLoadStarted) return;
  chartLoadStarted = true;
  const chartDom = $('voltageChart');
  chartDom.textContent = '图表库加载中；不影响时钟和继电器控制…';
  const script = document.createElement('script');
  script.src = 'https://cdn.jsdelivr.net/npm/echarts@5.5.0/dist/echarts.min.js';
  script.async = true;
  script.onload = () => { initChart(); };
  script.onerror = () => { chartDom.textContent = 'ECharts CDN 加载失败；时钟和继电器控制仍可正常使用。'; };
  document.head.appendChild(script);
}

document.addEventListener('DOMContentLoaded', () => {
  fetchInitialState();
  // 先完成一次设备时间请求，再开始加载历史曲线，避免图表抢占首次初始化时机。
  fetchData().finally(loadEChartsAsync);
});
setInterval(fetchData,5000);
setInterval(renderDeviceClock,1000);
setInterval(updateChart,120000);

function saveNotifyConfig() {
  const url = $('notifyApi').value.trim();

  if (!url.startsWith('http://') && !url.startsWith('https://')) {
    $('notifyStatus').style.color = 'var(--red-color)';
    $('notifyStatus').textContent = 'API 地址格式错误';
    return;
  }

  fetch('/setNotifyApi?url=' + encodeURIComponent(url))
    .then(r => {
      if (!r.ok) {
        return r.text().then(t => {
          throw new Error(t);
        });
      }

      $('notifyStatus').style.color = 'var(--green-color)';
      $('notifyStatus').textContent = '✓ API 配置已保存';
    })
    .catch(e => {
      $('notifyStatus').style.color = 'var(--red-color)';
      $('notifyStatus').textContent = '✗ 保存失败: ' + e;
    });
}

</script>
</body></html>
)HTML";

#endif // MAIN_HTML_H
