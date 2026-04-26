#pragma once

// =============================================================================
// socket_html.h — 双路插座控制器 + NAS主机电源页面 HTML
// 占位符说明:
//   ##R2_PIN##  → 插座1 GPIO 编号
//   ##R3_PIN##  → 插座2 GPIO 编号
//   ##R4_PIN##  → NAS继电器 GPIO 编号
// 由 handleSocketPage() 在运行时替换。
// =============================================================================

const char SOCKET_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 双路插座 & NAS 控制</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background-color: #f4f7f6; margin: 0; padding: 20px; color: #333; }
    .container { max-width: 620px; margin: 0 auto; background: #fff; padding: 25px; border-radius: 10px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); }
    h1, h2 { text-align: center; color: #007bff; }
    h2 { border-top: 1px solid #eee; padding-top: 20px; margin-top: 20px; }
    h3 { display: flex; justify-content: space-between; align-items: center; }
    h4 { color: #0056b3; }
    .status-box, .control-box, .schedule-box { margin-bottom: 25px; }
    p { font-size: 1.1em; text-align: center; }
    #timeDisplay { font-weight: bold; color: #333; }
    .status-on  { color: #28a745; font-weight: bold; }
    .status-off { color: #dc3545; font-weight: bold; }
    .btn-group { display: flex; justify-content: space-around; gap: 15px; }
    button, .button { display: inline-block; padding: 12px 20px; font-size: 1em; cursor: pointer; border: none; border-radius: 5px; color: #fff; text-align: center; text-decoration: none; }
    .btn-on  { background-color: #28a745; }
    .btn-on:hover  { background-color: #218838; }
    .btn-off { background-color: #dc3545; }
    .btn-off:hover { background-color: #c82333; }
    .btn-add { background-color: #007bff; }
    .btn-add:hover { background-color: #0069d9; }
    .btn-delete-all { background-color: #dc3545; font-size: 0.7em; padding: 3px 8px; vertical-align: middle; }
    .btn-delete-all:hover { background-color: #c82333; }
    .input-group { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 10px; align-items: center; }
    input[type="time"] { flex-grow: 1; padding: 10px; border: 1px solid #ccc; border-radius: 5px; font-size: 1em; }
    select { padding: 10px; border: 1px solid #ccc; border-radius: 5px; font-size: 1em;}
    ul { list-style: none; padding: 0; }
    li { background: #f9f9f9; border: 1px solid #eee; padding: 10px; margin-bottom: 8px; border-radius: 5px; display: flex; justify-content: space-between; align-items: center; }
    li button { background-color: #6c757d; font-size: 0.8em; padding: 5px 10px; }
    li button:hover { background-color: #5a6268; }
    code { background-color: #e9ecef; padding: 2px 6px; border-radius: 4px; font-family: SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace; }
    .api-docs p { text-align: left; font-size: 0.95em; }
    a.nav-link { display: block; margin-top: 20px; color: #007bff; text-decoration: none; font-weight: bold; text-align: center; }

    /* ===== NAS 继电器专属样式 ===== */
    .nas-box {
      border: 2px solid #ffc107;
      border-radius: 10px;
      padding: 18px;
      background: #fffbf0;
      margin-bottom: 25px;
    }
    .nas-box h2 {
      color: #856404;
      border-top: none;
      padding-top: 0;
      margin-top: 0;
    }
    .nas-warning {
      color: #856404;
      background: #fff3cd;
      border: 1px solid #ffc107;
      border-radius: 6px;
      padding: 10px 14px;
      font-size: 0.92em;
      text-align: center;
      margin-bottom: 14px;
    }
    .btn-nas-on  { background-color: #fd7e14; }
    .btn-nas-on:hover  { background-color: #e06c00; }
    .btn-nas-off { background-color: #6f42c1; }
    .btn-nas-off:hover { background-color: #5a31a0; }

    /* ===== 密码弹窗遮罩 ===== */
    .modal-overlay {
      display: none;
      position: fixed; top: 0; left: 0; width: 100%; height: 100%;
      background: rgba(0,0,0,0.55);
      z-index: 1000;
      align-items: center;
      justify-content: center;
    }
    .modal-overlay.active { display: flex; }
    .modal-box {
      background: #fff;
      border-radius: 12px;
      padding: 28px 32px;
      max-width: 340px;
      width: 90%;
      box-shadow: 0 8px 32px rgba(0,0,0,0.25);
      text-align: center;
    }
    .modal-box h3 { margin: 0 0 8px; color: #856404; font-size: 1.1em; }
    .modal-box p  { font-size: 0.92em; color: #555; margin: 0 0 16px; }
    .modal-box input[type="password"] {
      width: 100%; padding: 10px; font-size: 1.1em;
      border: 2px solid #ffc107; border-radius: 6px;
      margin-bottom: 16px; text-align: center;
      letter-spacing: 4px;
      box-sizing: border-box;
    }
    .modal-box input[type="password"]:focus { outline: none; border-color: #fd7e14; }
    .modal-actions { display: flex; gap: 12px; justify-content: center; }
    .modal-actions .btn-confirm { background: #28a745; }
    .modal-actions .btn-cancel  { background: #6c757d; }
    .modal-error { color: #dc3545; font-size: 0.88em; min-height: 20px; margin-bottom: 8px; }

    /* 启动倒计时提示 */
    .boot-timer-bar {
      background: #e9f7ef;
      border: 1px solid #28a745;
      border-radius: 6px;
      padding: 8px 14px;
      font-size: 0.9em;
      color: #155724;
      text-align: center;
      margin-bottom: 16px;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>双路插座 &amp; NAS 控制器</h1>

    <div class="status-box">
      <p>NTP网络时间: <span id="timeDisplay">--:--:--</span></p>
      <div class="boot-timer-bar" id="bootTimerBar" style="display:none">
        🔌 启动40分钟定时运行中，剩余: <b id="bootTimerRemain">--</b>
      </div>
    </div>

    <!-- ===== 双路插座 ===== -->
    <div class="control-box">
      <h2>插座 1 (GPIO ##R2_PIN##)</h2>
      <p>状态: <span id="r2_status">--</span></p>
      <div class="btn-group">
        <button class="button btn-on"  onclick="manualControl(1, 1)">手动开启</button>
        <button class="button btn-off" onclick="manualControl(1, 0)">手动关闭</button>
      </div>
    </div>
    <div class="control-box">
      <h2>插座 2 (GPIO ##R3_PIN##)</h2>
      <p>状态: <span id="r3_status">--</span></p>
      <div class="btn-group">
        <button class="button btn-on"  onclick="manualControl(2, 1)">手动开启</button>
        <button class="button btn-off" onclick="manualControl(2, 0)">手动关闭</button>
      </div>
    </div>

    <!-- ===== NAS 主机电源 ===== -->
    <div class="nas-box">
      <h2>⚠️ NAS 主机电源 (GPIO ##R4_PIN##)</h2>
      <div class="nas-warning">
        ⚠️ 此继电器直接控制 NAS 主机的 220V 供电。<br>
        操作前请确认 NAS 已正常关机，误操作可能造成数据丢失！<br>
        每次操作均需输入确认密码。
      </div>
      <p>当前状态: <span id="r4_status">--</span></p>
      <div class="btn-group">
        <button class="button btn-nas-on"  onclick="openNasModal(1)">开启 NAS 电源</button>
        <button class="button btn-nas-off" onclick="openNasModal(0)">关闭 NAS 电源</button>
      </div>
    </div>

    <!-- ===== 密码确认弹窗 ===== -->
    <div class="modal-overlay" id="nasModal">
      <div class="modal-box">
        <h3 id="modalTitle">⚠️ NAS 电源操作确认</h3>
        <p id="modalDesc">此操作将影响 NAS 主机供电，请输入操作密码：</p>
        <input type="password" id="nasPassword" placeholder="••••••" maxlength="20" onkeydown="if(event.key==='Enter')confirmNas()">
        <div class="modal-error" id="modalError"></div>
        <div class="modal-actions">
          <button class="button btn-confirm" onclick="confirmNas()">✔ 确认</button>
          <button class="button btn-cancel"  onclick="closeNasModal()">✖ 取消</button>
        </div>
      </div>
    </div>

    <!-- ===== 定时任务 ===== -->
    <div class="schedule-box">
      <h2>添加定时任务</h2>
      <div class="input-group">
        <select id="relaySelect">
          <option value="1">插座 1</option>
          <option value="2">插座 2</option>
        </select>
        <input type="time" id="scheduleTime" step="1" value="12:00:00">
        <button class="button btn-add" onclick="addSchedule(1)">添加开机</button>
        <button class="button btn-add" onclick="addSchedule(0)">添加关机</button>
      </div>
    </div>

    <div class="schedule-box">
      <h2>任务列表</h2>
      <h3>插座 1 任务 <button class="btn-delete-all" onclick="deleteAll(1)">删除全部</button></h3>
      <ul id="r2-list"><li>加载中...</li></ul>
      <h3>插座 2 任务 <button class="btn-delete-all" onclick="deleteAll(2)">删除全部</button></h3>
      <ul id="r3-list"><li>加载中...</li></ul>
    </div>

    <!-- ===== API 说明 ===== -->
    <div class="schedule-box api-docs">
      <h2>API &amp; 用法说明</h2>
      <h4>通过 URL 添加定时任务 (API)</h4>
      <p>您可以通过访问特定URL来远程添加定时任务，无需打开网页。</p>
      <p><b>URL 端点:</b> <code>/LED-Control</code></p>
      <p><b>必需参数:</b></p>
      <ul>
        <li><code><b>relay</b>=[1|2]</code>: 目标插座 (1 或 2)。</li>
        <li><code><b>ledPwm</b>=[0|1]</code>: 执行的动作 (0=关闭, 1=开启)。</li>
        <li><code><b>time</b>=HH:MM:SS</code>: 任务执行时间。</li>
      </ul>
      <p><b>示例:</b></p>
      <p><code>http://[设备IP]/LED-Control?relay=1&amp;ledPwm=1&amp;time=22:30:00</code></p>

      <h4 style="margin-top:20px">通过 URL 手动控制 (API)</h4>
      <p><b>URL 端点:</b> <code>/setSocketTask</code></p>
      <ul>
        <li><code><b>cmd</b>=manual</code></li>
        <li><code><b>relay</b>=[1|2]</code></li>
        <li><code><b>state</b>=[0|1]</code></li>
      </ul>
      <p><b>示例:</b> <code>http://[设备IP]/setSocketTask?cmd=manual&amp;relay=2&amp;state=0</code></p>

      <h4 style="margin-top:20px">NAS 继电器 API (需密码)</h4>
      <p><b>URL 端点:</b> <code>/setNasRelay</code></p>
      <ul>
        <li><code><b>state</b>=[0|1]</code>: 0=关闭, 1=开启。</li>
        <li><code><b>pwd</b>=密码</code>: 操作密码 (必须)。</li>
      </ul>
      <p><b>示例:</b> <code>http://[设备IP]/setNasRelay?state=0&amp;pwd=123456</code></p>

      <h4 style="margin-top:20px">通过 URL 删除所有任务 (API)</h4>
      <p><b>URL 端点:</b> <code>/deleteAllTasks?relay=[1|2]</code></p>
    </div>

    <a href="/" class="nav-link">&lt;&lt; 返回风扇控制页面</a>
  </div>

  <script>
    /* ---- 工具函数 ---- */
    function fetchJson(url, options) {
      return fetch(url, options).then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); });
    }

    /* ---- 状态更新 ---- */
    function updateStatus(data) {
      if (data.time) document.getElementById('timeDisplay').textContent = data.time;

      document.getElementById('r2_status').innerHTML = data.relay2
        ? "<span class='status-on'>开启</span>" : "<span class='status-off'>关闭</span>";
      document.getElementById('r3_status').innerHTML = data.relay3
        ? "<span class='status-on'>开启</span>" : "<span class='status-off'>关闭</span>";
      document.getElementById('r4_status').innerHTML = data.relay4
        ? "<span class='status-on'>供电中 ✔</span>" : "<span class='status-off'>已断电</span>";

      /* 启动倒计时 */
      const bar = document.getElementById('bootTimerBar');
      if (data.boot_timer_active && data.boot_timer_remain_s > 0) {
        bar.style.display = 'block';
        const m = Math.floor(data.boot_timer_remain_s / 60);
        const s = data.boot_timer_remain_s % 60;
        document.getElementById('bootTimerRemain').textContent =
          m + ' 分 ' + String(s).padStart(2, '0') + ' 秒';
      } else {
        bar.style.display = 'none';
      }

      /* 任务列表 */
      const r2list = document.getElementById('r2-list');
      r2list.innerHTML = '';
      if (!data.r2_tasks || data.r2_tasks.length === 0) { r2list.innerHTML = '<li>无</li>'; }
      else data.r2_tasks.forEach(task => {
        const ts = `${String(task.h).padStart(2,'0')}:${String(task.m).padStart(2,'0')}:${String(task.s).padStart(2,'0')}`;
        r2list.innerHTML += `<li>${ts} - ${task.a ? '开启' : '关闭'} <button onclick="deleteSchedule(1,'${ts}')">删除</button></li>`;
      });

      const r3list = document.getElementById('r3-list');
      r3list.innerHTML = '';
      if (!data.r3_tasks || data.r3_tasks.length === 0) { r3list.innerHTML = '<li>无</li>'; }
      else data.r3_tasks.forEach(task => {
        const ts = `${String(task.h).padStart(2,'0')}:${String(task.m).padStart(2,'0')}:${String(task.s).padStart(2,'0')}`;
        r3list.innerHTML += `<li>${ts} - ${task.a ? '开启' : '关闭'} <button onclick="deleteSchedule(2,'${ts}')">删除</button></li>`;
      });
    }

    /* ---- 插座控制 ---- */
    function manualControl(relay, state) {
      fetch(`/setSocketTask?cmd=manual&relay=${relay}&state=${state}`)
        .then(() => setTimeout(refreshAllData, 200));
    }
    function addSchedule(action) {
      const relay = document.getElementById('relaySelect').value;
      const time  = document.getElementById('scheduleTime').value;
      if (!time) { alert('请先选择时间！'); return; }
      fetch(`/setSocketTask?cmd=add&relay=${relay}&action=${action}&time=${time}`)
        .then(() => setTimeout(refreshAllData, 200));
    }
    function deleteSchedule(relay, time) {
      if (confirm(`确定要为插座 ${relay} 删除时间点 ${time} 吗?`))
        fetch(`/setSocketTask?cmd=delete&relay=${relay}&time=${time}`)
          .then(() => setTimeout(refreshAllData, 200));
    }
    function deleteAll(relay) {
      if (confirm(`您确定要删除插座 ${relay} 的所有定时任务吗？此操作无法撤销。`))
        fetch(`/deleteAllTasks?relay=${relay}`)
          .then(() => setTimeout(refreshAllData, 200));
    }

    /* ---- NAS 密码弹窗 ---- */
    let _nasTargetState = 0;
    function openNasModal(state) {
      _nasTargetState = state;
      document.getElementById('modalTitle').textContent =
        state ? '⚠️ 确认开启 NAS 电源' : '⚠️ 确认关闭 NAS 电源';
      document.getElementById('modalDesc').textContent =
        state
          ? '开启后 NAS 将通电启动，请确认操作密码：'
          : '关闭后 NAS 将立即断电！请确认 NAS 已关机，并输入操作密码：';
      document.getElementById('nasPassword').value = '';
      document.getElementById('modalError').textContent = '';
      document.getElementById('nasModal').classList.add('active');
      setTimeout(() => document.getElementById('nasPassword').focus(), 80);
    }
    function closeNasModal() {
      document.getElementById('nasModal').classList.remove('active');
    }
    function confirmNas() {
      const pwd = document.getElementById('nasPassword').value;
      if (!pwd) { document.getElementById('modalError').textContent = '请输入密码！'; return; }
      fetch(`/setNasRelay?state=${_nasTargetState}&pwd=${encodeURIComponent(pwd)}`)
        .then(r => {
          if (r.status === 403) {
            document.getElementById('modalError').textContent = '❌ 密码错误，操作已拒绝！';
            document.getElementById('nasPassword').value = '';
            document.getElementById('nasPassword').focus();
          } else if (!r.ok) {
            document.getElementById('modalError').textContent = '操作失败，请检查连接。';
          } else {
            closeNasModal();
            setTimeout(refreshAllData, 300);
          }
        })
        .catch(e => { document.getElementById('modalError').textContent = '网络错误: ' + e; });
    }
    /* 点击遮罩层关闭弹窗 */
    document.getElementById('nasModal').addEventListener('click', function(e) {
      if (e.target === this) closeNasModal();
    });

    /* ---- 数据刷新 ---- */
    function refreshAllData() {
      fetchJson('/getSocketData').then(updateStatus).catch(e => console.error('socket data err:', e));
    }
    setInterval(refreshAllData, 2000);
    window.addEventListener('load', refreshAllData);
  </script>
</body>
</html>
)rawliteral";