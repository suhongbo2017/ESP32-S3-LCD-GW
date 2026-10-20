/* 自动生成/维护：Web 控制台首页（纯内嵌，无外部依赖，简洁监控仪表盘） */
#pragma once

static const char WEB_CONSOLE_INDEX_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 网关 · 设备监控</title>
<style>
  :root{
    --bg:#f2f5f4;--panel:#fff;--deep:#f7f9f8;--line:#d9e2df;--line2:#c6d3cf;
    --fg:#1c2b29;--mut:#687773;--ok:#16835d;--warn:#b66a14;--bad:#c43d46;
    --teal:#087e75;--blue:#426f9c;--amber:#a86713;--violet:#7654a6;--sky:#3279a8;--coral:#b94e55;
    --tint-teal:#f1f8f6;--tint-blue:#f2f6fa;--tint-amber:#fbf6ed;--tint-sky:#f0f7fb;--tint-violet:#f6f3fa;--tint-coral:#fbf3f3;
    --page-pad:clamp(12px,2vw,28px);--gap:clamp(12px,1.6vw,20px);--text:clamp(14px,calc(13px + .32vw),16px)
  }
  [data-theme="ocean"]{--bg:#edf4f8;--deep:#f4f8fb;--line:#d4e1e9;--line2:#b9ccd9;--fg:#1c303b;--mut:#637783;--teal:#176b87;--blue:#376ca8;--amber:#a46b24;--violet:#6757a5;--sky:#2585a0;--coral:#b65363;--tint-teal:#edf6f8;--tint-blue:#eff3fa;--tint-amber:#f9f4eb;--tint-sky:#edf7f8;--tint-violet:#f3f1f9;--tint-coral:#faf0f2}
  [data-theme="sunset"]{--bg:#f8f2ed;--deep:#fcf7f2;--line:#e9dcd1;--line2:#d9c3b3;--fg:#382c28;--mut:#786a61;--teal:#2c8277;--blue:#4e76a8;--amber:#b26b24;--violet:#84568c;--sky:#39829a;--coral:#b84e58;--tint-teal:#f0f7f3;--tint-blue:#f1f4f9;--tint-amber:#fbf3e8;--tint-sky:#eff6f7;--tint-violet:#f7f1f6;--tint-coral:#fbf0ef}
  [data-theme="violet"]{--bg:#f3f0f8;--deep:#f8f6fb;--line:#e0d9eb;--line2:#c9bddb;--fg:#30283d;--mut:#71677e;--teal:#287c78;--blue:#4b70a8;--amber:#a8732d;--violet:#7650a2;--sky:#357e9b;--coral:#b34e67;--tint-teal:#eff6f4;--tint-blue:#f0f3fa;--tint-amber:#faf4e9;--tint-sky:#edf6f8;--tint-violet:#f4f0fa;--tint-coral:#faeff3}
  .kv:nth-child(1){background:var(--tint-teal)}.kv:nth-child(2){background:var(--tint-blue)}
  .kv:nth-child(3){background:var(--tint-amber)}.kv:nth-child(4){background:var(--tint-sky)}
  .kv:nth-child(5){background:var(--tint-violet)}.kv:nth-child(6){background:var(--tint-coral)}.kv:nth-child(7){background:var(--tint-teal)}
  .theme-picker{display:flex;align-items:center;gap:6px;margin-left:8px;color:var(--mut);font-size:12px;white-space:nowrap}
  .theme-picker select{padding:5px 8px;font-size:13px}
  *{box-sizing:border-box;margin:0;padding:0}
  body{font:var(--text)/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",sans-serif;
    background:var(--bg);color:var(--fg);padding:var(--page-pad);overflow-x:hidden}
  .wrap{width:100%;max-width:1440px;margin:0 auto;display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:var(--gap)}
  .pane{background:var(--panel);border:1px solid var(--line);border-radius:4px;min-width:0}
  .top{grid-column:1/-1;display:flex;align-items:center;gap:14px;padding:14px 18px;border-top:3px solid var(--teal)}
  .top h1{font-size:clamp(18px,2vw,24px);font-weight:650;letter-spacing:0;overflow-wrap:anywhere}
  .logo{display:grid;place-items:center;width:38px;height:38px;background:var(--tint-teal);color:var(--teal);font-size:22px;line-height:1;flex:0 0 auto}
  .top .grow{flex:1;min-width:8px}
  .clock{font-variant-numeric:tabular-nums;font-size:15px;color:var(--teal);white-space:nowrap}
  .cursor{display:inline-block;width:7px;height:7px;background:var(--ok);margin-left:4px;vertical-align:1px}
  .card{grid-column:span 6;min-width:0;overflow:hidden}
  .card:nth-of-type(1),.card:nth-of-type(2),.card:nth-of-type(5){grid-column:1/-1}
  .card h2{font-size:15px;font-weight:650;color:var(--fg);background:var(--deep);border-bottom:1px solid var(--line);padding:10px 14px;display:flex;gap:9px;align-items:center;min-width:0}
  .card h2::before{content:"";width:8px;height:8px;border-radius:2px;background:var(--teal);flex:0 0 auto}
  .card:nth-of-type(2) h2::before{background:var(--sky)}
  .card:nth-of-type(3) h2::before{background:var(--amber)}
  .card:nth-of-type(4) h2::before{background:var(--blue)}
  .card:nth-of-type(5) h2::before{background:var(--violet)}
  .card:nth-of-type(6) h2::before{background:var(--coral)}
  .card h2 .msg{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .card .bd{padding:clamp(11px,1.4vw,16px);min-width:0}
  .msg{font-size:clamp(12px,1.1vw,14px);color:var(--mut);min-height:16px;font-weight:normal;letter-spacing:0;overflow-wrap:anywhere}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,190px),1fr));gap:10px;padding:12px}
  .kv{background:var(--deep);padding:12px 14px;border:1px solid var(--line);border-left:3px solid var(--line2);border-radius:3px;min-width:0}
  .kv:nth-child(1){border-left-color:var(--teal)}
  .kv:nth-child(2){border-left-color:var(--blue)}
  .kv:nth-child(3){border-left-color:var(--amber)}
  .kv:nth-child(4){border-left-color:var(--sky)}
  .kv:nth-child(5){border-left-color:var(--violet)}
  .kv:nth-child(6){border-left-color:var(--coral)}
  .kv:nth-child(7){border-left-color:var(--teal)}
  .kv .k{font-size:12px;color:var(--mut);overflow-wrap:anywhere}
  .kv .v{font-variant-numeric:tabular-nums;font-size:clamp(20px,2vw,28px);font-weight:650;color:var(--teal);margin-top:5px;word-break:break-word;line-height:1.25}
  .kv:nth-child(2) .v{color:var(--blue)}.kv:nth-child(3) .v{color:var(--amber)}.kv:nth-child(4) .v{color:var(--sky)}
  .kv:nth-child(5) .v{color:var(--violet)}.kv:nth-child(7) .v{color:var(--teal)}
  .kv .v small{font-size:12px;color:var(--mut);font-weight:400}
  .ok{color:var(--ok)!important}.bad{color:var(--bad)!important}.warn{color:var(--warn)!important}
  .led{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:7px;background:var(--mut);vertical-align:1px}
  .led.ok{background:var(--ok)!important}.led.warn{background:var(--warn)!important}.led.bad{background:var(--bad)!important}
  button{font:inherit;font-size:14px;color:var(--fg);cursor:pointer;background:var(--panel);border:1px solid var(--line2);border-radius:3px;padding:8px 13px;max-width:100%;transition:background-color .12s,border-color .12s}
  button:hover{background:var(--deep);border-color:var(--line2)}button:active{background:var(--line)}
  button.pri{background:var(--teal);border-color:var(--teal);color:#fff}button.pri:hover{filter:brightness(.92)}
  button.sec{background:var(--deep);color:var(--fg)}
  select,input{font:inherit;font-size:14px;background:var(--panel);color:var(--fg);border:1px solid var(--line2);border-radius:3px;padding:8px 10px;min-width:0;max-width:100%}
  input:focus,select:focus,button:focus-visible{outline:2px solid var(--teal);outline-offset:2px}
  .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px;min-width:0}
  .row > .sub[style*="width:112px"]{width:clamp(90px,15vw,112px)!important;flex:0 0 clamp(90px,15vw,112px)}
  .chartbox{background:var(--deep);border:1px solid var(--line);border-radius:3px;padding:8px;margin-top:8px;min-width:0;overflow:hidden}
  .chartbox svg{display:block;width:100%;height:auto;max-width:100%}
  table{width:100%;border-collapse:collapse;font-size:14px;table-layout:fixed}
  td,th{text-align:left;padding:8px 9px;border:1px solid var(--line);overflow-wrap:anywhere}
  th{background:var(--deep);color:var(--fg);font-weight:600}
  tr:nth-child(even) td{background:var(--deep)}a{color:var(--teal)}
  code{background:var(--deep);border:1px solid var(--line);border-radius:2px;padding:2px 5px;font-size:12px;overflow-wrap:anywhere}
  .wrap > .sub{grid-column:1/-1;margin-top:0}
  @media (max-width:700px){
    .card{grid-column:1/-1}.top{align-items:flex-start;flex-wrap:wrap}.top .grow{display:none}
    .clock{margin-left:auto}.row > .sub[style*="width:112px"]{flex-basis:100%;width:100%!important}
  }
  @media (max-width:480px){
    body{padding:8px}.top{padding:11px;gap:9px}.top h1{font-size:18px}.clock{font-size:13px}
    .grid{grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;padding:9px}.kv{padding:10px}
    .kv .v{font-size:clamp(18px,6vw,24px)}.card .bd{padding:10px}
  }
  .grid > .kv:only-child{grid-column:1/-1}
  @media (max-width:340px){.grid{grid-template-columns:1fr}}
 </style>
</head>
<body>
<div class="wrap">
  <header class="top pane">
    <div class="logo">▚</div>
    <div>
      <h1>ESP32-S3 双网网关</h1>
      <div class="sub" id="host">—</div>
    </div>
    <div class="clock"><span id="ts">--:--:--</span><span class="cursor"></span></div>
    <label class="theme-picker" for="theme-selector"><span>主题</span><select id="theme-selector" aria-label="颜色主题">
      <option value="teal">青绿</option><option value="ocean">海洋</option><option value="sunset">暖阳</option><option value="violet">紫藤</option>
    </select></label>
  </header>

  <section class="card pane">
    <h2>运行状态 <span class="msg" id="ts2"></span></h2>
    <div class="grid" id="status"><div class="kv"><div class="k">LOADING…</div><div class="v">—</div></div></div>
  </section>

  <section class="card pane">
    <h2>GPIO 控制 <span class="msg" id="gmsg"></span></h2>
    <div class="bd">
      <div class="row">
        <span class="sub">引脚</span>
        <select id="pin">
          <option>1</option><option>2</option><option>3</option><option>4</option><option>5</option>
          <option>6</option><option>7</option><option>8</option><option>9</option>
          <option>39</option><option>40</option><option>41</option><option>42</option><option>47</option><option>48</option>
        </select>
        <button onclick="gpioRead()">读取</button>
        <button class="pri" onclick="gpioSet(1)">置高</button>
        <button class="sec" onclick="gpioSet(0)">置低</button>
      </div>
      <div class="msg">可用引脚：GPIO1–9、39–42、47–48（其余为板载占用，已屏蔽）</div>
    </div>
  </section>

  <section class="card pane">
    <h2>TF 卡日志</h2>
    <div class="bd">
      <table id="logs"><tbody><tr><td>加载中…</td></tr></tbody></table>
    </div>
  </section>

  <section class="card pane">
    <h2>温湿度曲线 <span class="msg" id="hmsg"></span></h2>
    <div class="bd">
      <div class="row">
        <button onclick="drawChart()">刷新曲线</button>
        <span class="msg">已采样 <span id="hcnt">0</span> 条（约 15s/条，最多 240 条 ≈ 1 小时）</span>
      </div>
      <div class="chartbox" id="chart"><div class="msg">加载中…</div></div>
    </div>
  </section>

  <section class="card pane">
    <h2>参数配置 <span class="msg" id="cmsg"></span></h2>
    <div class="bd">
      <div class="row"><span class="sub" style="width:112px">WiFi SSID</span><input id="c_ssid" style="flex:1"></div>
      <div class="row"><span class="sub" style="width:112px">WiFi 密码</span><input id="c_pass" type="password" placeholder="(不改请留空)" style="flex:1"></div>
      <div class="row"><span class="sub" style="width:112px">MQTT 主机</span><input id="c_host" style="flex:1"></div>
      <div class="row"><span class="sub" style="width:112px">MQTT 端口</span><input id="c_port" style="width:90px"></div>
      <div class="row"><span class="sub" style="width:112px">通知主题</span><input id="c_tn" style="flex:1"></div>
      <div class="row"><span class="sub" style="width:112px">传感器主题</span><input id="c_ts" style="flex:1"></div>
      <div class="row">
        <button class="pri" onclick="cfgSave()">保存配置</button>
        <button onclick="saveAndReboot()">保存并重启</button>
        <button class="sec" onclick="cfgReset()">恢复默认</button>
        <button class="sec" onclick="reboot(false)">仅重启</button>
      </div>
      <div class="msg">保存写入 NVS，重启后生效</div>
    </div>
  </section>

  <section class="card pane">
    <h2>OTA 固件升级 <span class="msg" id="omsg"></span></h2>
    <div class="bd">
      <div class="row">
        <input type="file" id="fw" accept=".bin" style="flex:1;padding:5px">
        <button onclick="ota()">上传升级</button>
      </div>
      <div class="msg">选 <code>build/esp32s3_gateway.bin</code>；升级成功后自动重启</div>
    </div>
  </section>

  <div class="sub" style="margin-top:6px">
    只读接口：<code>/api/status</code> <code>/api/logs</code> <code>/api/history</code> ｜ 控制接口：<code>POST /api/gpio</code>
  </div>
</div>

<script>
const $ = id => document.getElementById(id);
async function api(path, options = {}){
  return fetch(path, options);
}
function setTheme(name){
  const themes = ['teal','ocean','sunset','violet'];
  const theme = themes.includes(name) ? name : 'teal';
  document.documentElement.dataset.theme = theme;
  $('theme-selector').value = theme;
  try{ localStorage.setItem('gateway-theme', theme); }catch(e){}
  drawChart();
}
try{ setTheme(localStorage.getItem('gateway-theme') || 'teal'); }catch(e){ setTheme('teal'); }
$('theme-selector').addEventListener('change', e => setTheme(e.target.value));

async function refresh(){
  try{
    const r = await api('/api/status');
    const s = await r.json();
    $('host').textContent = location.host + ' · ' + (s.mdns ? ('http://' + s.mdns + '.local') : '');
    const up = (s.uptime|0);
    const hh = String(Math.floor(up/3600)).padStart(2,'0');
    const mm = String(Math.floor(up%3600/60)).padStart(2,'0');
    const ss = String(up%60).padStart(2,'0');
    const live = s.sensor_valid && !s.sensor_stale;
    const cled = c => `<span class="led ${c||''}"></span>`;
    const tile = (k,v,c,sub) =>
      `<div class="kv"><div class="k">${k}</div>` +
      `<div class="v ${c==='ok'?'ok':c==='bad'?'bad':c==='warn'?'warn':''}">${cled(c)}${v}</div>` +
      (sub ? `<div class="k">${sub}</div>` : '') + `</div>`;
    const items = [
      tile('上行链路 UPLINK', s.uplink || '--', s.uplink && s.uplink !== 'None' ? 'ok' : 'warn'),
      tile('IP 地址', s.ip || '-', ''),
      tile('温度 TEMP', live ? s.sensor_temp.toFixed(1) + '℃' : '--', live ? 'ok' : 'warn'),
      tile('湿度 HUMID', live ? s.sensor_hum.toFixed(1) + '%' : '--', live ? 'ok' : 'warn'),
      tile('MQTT', s.mqtt ? '已连接' : '未连接', s.mqtt ? 'ok' : 'bad'),
      tile('TF 卡', s.tf_ready ? '已挂载' : '未挂载', s.tf_ready ? 'ok' : 'warn', s.tf_ready ? (s.tf_written + ' 条已写') : ''),
      tile('运行时长 UPTIME', hh + ':' + mm + ':' + ss, ''),
    ];
    $('status').innerHTML = items.join('');
    $('ts').textContent = new Date().toLocaleTimeString([], {hour12:false});
    $('ts2').textContent = '自动刷新 2s';
  }catch(e){ $('ts2').textContent = '刷新失败: ' + e; }
}

async function loadLogs(){
  try{
    const r = await api('/api/logs'); const j = await r.json();
    if(!j.files || !j.files.length){ $('logs').innerHTML = '<tbody><tr><td>（无文件）</td></tr></tbody>'; return; }
    $('logs').innerHTML = '<tbody><tr><th>文件</th><th>大小</th><th></th></tr>' +
      j.files.map(f => `<tr><td>${f.name}</td><td>${(f.size/1024).toFixed(1)} KB</td>
        <td><button class="sec" onclick="downloadLog('${encodeURIComponent(f.name)}')">[下载]</button></td></tr>`).join('') +
      '</tbody>';
  }catch(e){ $('logs').innerHTML = '<tbody><tr><td>获取失败</td></tr></tbody>'; }
}
async function downloadLog(name){
  let url;
  try{
    const r = await api('/api/logs/' + name);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    url = URL.createObjectURL(await r.blob());
    const a = document.createElement('a');
    a.href = url; a.download = decodeURIComponent(name); a.click();
  }catch(e){ $('ts2').textContent = '下载失败: ' + e; }
  finally{ if (url) URL.revokeObjectURL(url); }
}

async function gpioRead(){
  try{
    const r = await api('/api/gpio?pin=' + $('pin').value);
    const j = await r.json();
    $('gmsg').textContent = j.ok ? ('GPIO' + j.pin + ' 电平=' + j.level) : ('失败: ' + j.error);
  }catch(e){ $('gmsg').textContent = '失败: ' + e; }
}
async function gpioSet(v){
  try{
    const r = await api('/api/gpio?pin=' + $('pin').value + '&value=' + v, {method:'POST'});
    const j = await r.json();
    $('gmsg').textContent = j.ok ? ('GPIO' + j.pin + ' 已置' + (v ? '高' : '低')) : ('失败: ' + (j.error||r.status));
  }catch(e){ $('gmsg').textContent = '失败: ' + e; }
}


/* ---- 温湿度曲线（纯 SVG 像素阶梯，无外部库） ---- */
async function drawChart(){
  try{
    const j = await (await api('/api/history')).json();
    const it = j.items || [];
    $('hcnt').textContent = j.count || 0;
    if (it.length < 2){
      $('chart').innerHTML = '<div class="msg">数据不足（等待传感器上报，约 15s 一条）</div>';
      $('hmsg').textContent = '';
      return;
    }
    const W = 700, H = 240, P = 42;
    const style = getComputedStyle(document.documentElement);
    const C_T = style.getPropertyValue('--amber').trim(), C_H = style.getPropertyValue('--teal').trim();
    const C_G = style.getPropertyValue('--line').trim(), C_M = style.getPropertyValue('--mut').trim();
    const ts = it.map(x => x.t), temps = it.map(x => x.temp), hums = it.map(x => x.hum);
    const pad = (a, b) => a === b ? [a - 1, b + 1] : [a - (b-a)*0.1, b + (b-a)*0.1];
    const [t0, t1] = pad(Math.min(...temps), Math.max(...temps));
    const [h0, h1] = pad(Math.min(...hums),  Math.max(...hums));
    const N = it.length;
    const X  = i => Math.round(P + (W - 2*P) * i / (N - 1));
    const Yt = v => Math.round(P + (H - 2*P) * (1 - (v - t0) / (t1 - t0)));
    const Yh = v => Math.round(P + (H - 2*P) * (1 - (v - h0) / (h1 - h0)));
    /* 阶梯（先横后竖）路径，像素感 */
    const path = (arr, Y) => 'M' + X(0) + ' ' + Y(arr[0]) +
      arr.slice(1).map((v, i) => 'H' + X(i+1) + ' V' + Y(v)).join(' ');
    const fmt = t => j.wall_clock ? new Date(t * 1000).toLocaleTimeString([], {hour12:false}) : ('+' + Math.round(t/60) + 'min');
    let grid = '';
    for (let k = 0; k <= 4; k++){
      const y = Math.round(P + (H - 2*P) * k / 4);
      grid += `<line x1="${P}" y1="${y}" x2="${W-P}" y2="${y}" stroke="${C_G}" stroke-width="2" stroke-dasharray="4 4"/>`;
      grid += `<text x="${P-6}" y="${y+4}" fill="${C_T}" font-size="11" text-anchor="end">${(t1-(t1-t0)*k/4).toFixed(1)}</text>`;
      grid += `<text x="${W-P+6}" y="${y+4}" fill="${C_H}" font-size="11">${(h1-(h1-h0)*k/4).toFixed(1)}</text>`;
    }
    for (let k = 0; k <= 6; k++){
      const x = Math.round(P + (W - 2*P) * k / 6);
      grid += `<line x1="${x}" y1="${P}" x2="${x}" y2="${H-P}" stroke="${C_G}" stroke-width="2" stroke-dasharray="2 6"/>`;
    }
    const cur = (arr, Y, c) => `<circle cx="${X(N-1)}" cy="${Y(arr[N-1])}" r="4" fill="${c}" stroke="#ffffff" stroke-width="2"/>`;
    $('chart').innerHTML =
      `<svg viewBox="0 0 ${W} ${H}" font-family="Arial,sans-serif">${grid}
        <path d="${path(temps, Yt)}" fill="none" stroke="${C_T}" stroke-width="3"/>
        <path d="${path(hums,  Yh)}" fill="none" stroke="${C_H}" stroke-width="3"/>
        ${cur(temps, Yt, C_T)}${cur(hums, Yh, C_H)}
        <text x="${P}" y="${H-10}" fill="${C_M}" font-size="11">${fmt(ts[0])}</text>
        <text x="${W-P}" y="${H-10}" fill="${C_M}" font-size="11" text-anchor="end">${fmt(ts[N-1])}</text>
        <rect x="${P}" y="${P-22}" width="9" height="9" fill="${C_T}"/>
        <text x="${P+14}" y="${P-13}" fill="${C_T}" font-size="12">温度 ℃</text>
        <rect x="${W-P-72}" y="${P-22}" width="9" height="9" fill="${C_H}"/>
        <text x="${W-P-58}" y="${P-13}" fill="${C_H}" font-size="12">湿度 %</text>
      </svg>`;
    $('hmsg').textContent = '温度 ' + temps[N-1].toFixed(1) + '℃ · 湿度 ' + hums[N-1].toFixed(1) + '%';
  }catch(e){ $('chart').innerHTML = '<div class="msg">获取失败: ' + e + '</div>'; }
}

/* ---- 参数配置 ---- */
async function cfgLoad(){
  try{
    const c = await (await api('/api/config')).json();
    $('c_ssid').value = c.wifi_ssid || '';
    $('c_host').value = c.mqtt_host || '';
    $('c_port').value = c.mqtt_port || '';
    $('c_tn').value   = c.topic_notify || '';
    $('c_ts').value   = c.topic_sensor || '';
    $('cmsg').textContent = c.from_nvs ? '（含 NVS 自定义值）' : '（当前为默认值）';
  }catch(e){}
}
async function cfgSave(){
  const p = new URLSearchParams();
  const put = (k,id) => { const v = $(id).value.trim(); if (v) p.set(k, v); };
  put('wifi_ssid','c_ssid'); put('wifi_pass','c_pass'); put('mqtt_host','c_host');
  put('mqtt_port','c_port'); put('topic_notify','c_tn'); put('topic_sensor','c_ts');
  try{
    const j = await (await api('/api/config?' + p.toString(), {method:'POST'})).json();
    $('cmsg').textContent = j.ok ? ('已保存 ' + j.applied + ' 项，重启生效') : ('失败: ' + (j.error || j.message));
  }catch(e){ $('cmsg').textContent = '失败: ' + e; }
}
async function cfgReset(){
  try{
    const j = await (await api('/api/config?action=reset', {method:'POST'})).json();
    $('cmsg').textContent = j.ok ? '已恢复默认，重启生效' : '失败';
  }catch(e){ $('cmsg').textContent = '失败: ' + e; }
}

/* ---- 重启 ---- */
async function reboot(skipConfirm){
  if (!skipConfirm && !confirm('确定重启设备？')) return;
  $('cmsg').textContent = '设备重启中…（约 20 秒后恢复）';
  try{ await api('/api/reboot', {method:'POST'}); }catch(e){ /* 重启会断开连接，属正常 */ }
}
async function saveAndReboot(){
  await cfgSave();
  setTimeout(() => reboot(true), 800);
}

/* ---- OTA ---- */
async function ota(){
  const f = $('fw').files[0];
  if (!f) { $('omsg').textContent = '请先选择固件文件'; return; }
  $('omsg').textContent = '上传中 ' + (f.size/1048576).toFixed(2) + ' MB …（约 1 分钟，请勿断电）';
  try{
    const j = await (await api('/api/ota', {method:'POST', body:f})).json();
    $('omsg').textContent = j.ok ? '升级成功，设备重启中…' : ('失败: ' + (j.error || j.message));
  }catch(e){ $('omsg').textContent = '连接中断（设备可能已升级并重启）'; }
}
refresh(); loadLogs(); cfgLoad(); drawChart();
setInterval(refresh, 2000);
setInterval(drawChart, 30000);
</script>
</body>
</html>)HTML";