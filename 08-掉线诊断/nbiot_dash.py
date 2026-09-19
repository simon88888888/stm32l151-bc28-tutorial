# -*- coding: utf-8 -*-
"""BC28 数据看板 —— /apps 门户里点进来的那一页（第 5 篇的"云端看板"）。

数据源是 nbiot-sink 落的 SQLite:  /opt/nbiot-sink/data/history.db
    rx    一行 = 板子上报的一条报文  #12,CSQ=28,RSRP=-69.1,SNR=13.6,ECL=0,UP=158,T=26.8
    conn  一行 = 模块建的一次连接(AUTH 成功 / REJECT 被拒)

页面上的「📜 接收日志」按钮读的是**另一份**东西: 接收端的纯文本 sink.log
    /opt/nbiot-sink/logs/sink.log
跟 SQLite 的区别是它多出 AUTH(谁在何时连上、token 对不对)和"连接结束" ——
排查"板子到底碰没碰到服务器"靠的正是这些过程行, DB 里只有报文本身。

**只读打开**(mode=ro): 看板没有任何写权限, 采集那条路不受它影响。
authproxy.py 只把 /nb/ 和 /nb/api 两条路由指过来, 其余什么都不用知道。
"""
import json
import os
import re
import sqlite3
import sys
import time
from datetime import datetime, timedelta, timezone

# 下行配置跟 sink 共用同一个模块 + 同一个 config.json(两个进程、一个文件)。
# ★ 载入失败**不能影响看板**: 看板照样出图, 只是不能改配置。
#   nbcfg 放在 nbiot-sink/ 下, 所以要把那个目录加进 sys.path。
_CFGDIR = os.environ.get("NBIOT_CFGDIR", "/opt/nbiot-sink")
try:
    sys.path.insert(0, _CFGDIR)
    import nbcfg
except Exception as _exc:
    nbcfg = None
    print("!! nbiot_dash 载入 nbcfg 失败(%s): %s"
          % (_CFGDIR, _exc), file=sys.stderr, flush=True)

DB = os.environ.get("NBIOT_DB", "/opt/nbiot-sink/data/history.db")
SITE_FILE = os.environ.get("NBIOT_SITE", "/opt/nbiot-dash/site.json")
LOG = os.environ.get("NBIOT_LOG", "/opt/nbiot-sink/logs/sink.log")
TZ = timezone(timedelta(hours=8))  # 固定 UTC+8: 跟板子上的 uptime、你的墙上钟对得上

# ---- 「接收日志」弹窗的两个上限 ----
# 这个日志**不轮转**(sink.py 只追加), 只会一直长, 所以从文件尾部回读、别整份读进来。
LOG_TAIL_BYTES = 512 * 1024    # 最多回读这么多字节(实测约 75 字节/行, 够 6000+ 行)
LOG_MAX_LINES = 5000           # 单次最多返回这么多行, 防止"全部"把响应撑大

# sink 自己写的行长这样:  [2026-09-15 22:16:15 +08] RX     39.144.78.150  #7,CSQ=12,...
# 9101 是公网裸端口, 扫描器会往里塞任意字节, 那些行既不是数据也不该显示 —— 用这个筛掉。
LOG_LINE_RE = re.compile(r"^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \+08\] [A-Z]+")

# ---- 离线判定 / 掉线缺口 —— ★ 必须跟着上报周期走, 不能写死 ----
# 板子 30 秒一报时, 180 / 300 秒足够宽松(单次建连失败重试 3~5 秒, 硬恢复
# 重启模块 ~15 秒)。但**省电档 300 秒才报一次**, 沿用 180 秒的后果是
# 每一轮之间都显示"离线"、满屏假告警; GAP_S=300 更糟 —— 每两条就记一次
# "掉线缺口", 因为 300 秒间隔本来就等于 300 秒。
# 所以两个阈值都从当前配置的 report_ms 推出来:
#   STALE = 3 个周期(容得下一次重试 + 一次模块重启), GAP = 2 个周期
# 下限仍是 180 / 300: 正常档下跟以前一模一样, 不改老行为。
# 每次 payload() 现读一次配置(小文件, 开销可忽略) —— 这样改配置**不用重启
# uvicorn**, 也就不会把所有人的登录会话清掉。
STALE_S_MIN = 180
GAP_S_MIN = 300


def thresholds():
    """按当前上报周期算 (stale_s, gap_s)。读不到配置就按正常档 30 秒。"""
    rms = nbcfg.load()["report_ms"] if nbcfg else 30000
    return (max(STALE_S_MIN, rms // 1000 * 3),
            max(GAP_S_MIN, rms // 1000 * 2))

RANGES = [("1", "近 1 小时"), ("6", "近 6 小时"), ("24", "近 24 小时"),
          ("168", "近 7 天"), ("all", "全部")]

PAGE = r"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BC28 数据看板</title>
<style>
  :root { --bg:#10141d; --panel:#171c28; --border:#2a3244; --text:#e8edf6;
          --dim:#8b95a7; --primary:#4f8cff; --ok:#3ddc97; --warn:#ffb454;
          --danger:#ff6b6b; --grad:linear-gradient(135deg,#4f8cff,#3b6fe0); }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { background:radial-gradient(1200px 600px at 70% -10%, #1b2440 0%, var(--bg) 55%);
         min-height:100vh; padding:28px 16px; color:var(--text);
         font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif; }
  .wrap { max-width:760px; margin:0 auto; }
  .top { display:flex; align-items:center; gap:10px; }
  h1 { font-size:19px; }
  a.back { margin-left:auto; font-size:12px; color:var(--dim); text-decoration:none;
           border:1px solid var(--border); padding:5px 11px; border-radius:8px;
           white-space:nowrap; }
  a.back:hover { color:var(--text); border-color:var(--primary); }
  .sub { font-size:12px; color:var(--dim); margin:4px 0 16px; line-height:1.7; }
  .sec { font-size:13px; font-weight:600; color:var(--dim); margin:22px 0 10px;
         display:flex; align-items:center; gap:8px; }
  .sec::after { content:""; flex:1; height:1px; background:var(--border); }
  .hero { display:grid; grid-template-columns:repeat(4,1fr); gap:8px; }
  .hero > div { background:var(--panel); border:1px solid var(--border);
                border-radius:12px; padding:12px 8px; text-align:center; }
  .hero .k { font-size:11px; color:var(--dim); margin-bottom:6px; }
  .hero .v { font-size:21px; font-weight:600; font-variant-numeric:tabular-nums; }
  .hero .u { font-size:10px; color:var(--dim); margin-top:3px; }
  table { width:100%; border-collapse:collapse; background:var(--panel);
          border:1px solid var(--border); border-radius:12px; overflow:hidden; }
  td, th { padding:9px 13px; font-size:13px; border-bottom:1px solid var(--border);
           text-align:left; font-variant-numeric:tabular-nums; }
  th { font-size:11px; color:var(--dim); font-weight:600; }
  tr:last-child td { border-bottom:none; }
  td.k { color:var(--dim); width:44%; font-size:12px; }
  .chartbox { background:var(--panel); border:1px solid var(--border);
              border-radius:12px; padding:12px; }
  canvas { width:100%; height:200px; display:block; }
  .ranges { display:flex; gap:6px; margin-bottom:10px; flex-wrap:wrap; }
  .ranges button { font-size:11px; padding:4px 11px; border-radius:20px; cursor:pointer;
                   background:var(--bg); color:var(--dim);
                   border:1px solid var(--border); font-family:inherit; }
  .ranges button.on { background:var(--grad); color:#fff; border-color:transparent; }
  .empty { color:var(--dim); font-size:12px; padding:24px; text-align:center; }
  .foot { margin-top:22px; font-size:11px; color:var(--dim); line-height:1.9;
          padding:12px 14px; background:var(--panel); border:1px solid var(--border);
          border-radius:12px; }
  .foot b { color:var(--text); }
  .foot code { font-family:Consolas,monospace; color:var(--primary); }
  /* ---- 右上角那颗按钮 + 「接收日志」弹窗 ---- */
  .acts { margin-left:auto; display:flex; align-items:center; gap:8px; }
  .tbtn { font-size:12px; font-family:inherit; cursor:pointer; color:var(--dim);
          background:var(--bg); border:1px solid var(--border);
          padding:5px 11px; border-radius:8px; white-space:nowrap; }
  .tbtn:hover { color:var(--text); border-color:var(--primary); }
  /* ---- 上报模式两档开关 ---- */
  .cfgbar { display:flex; align-items:center; gap:10px; flex-wrap:wrap;
            margin:0 0 7px; }
  .cfglab { font-size:12px; color:var(--dim); }
  .seg { display:inline-flex; background:var(--bg);
         border:1px solid var(--border); border-radius:9px; overflow:hidden; }
  .segbtn { font-family:inherit; font-size:12px; cursor:pointer; padding:6px 13px;
            background:transparent; color:var(--dim); border:0; white-space:nowrap; }
  .segbtn + .segbtn { border-left:1px solid var(--border); }
  .segbtn:hover { color:var(--text); }
  .segbtn.on { background:var(--grad); color:#fff; }
  .segbtn:disabled { opacity:.45; cursor:default; }
  .segbtn:disabled:hover { color:var(--dim); }
  .cfgmsg { font-size:11px; color:var(--dim); }
  .cfgnote { font-size:11px; color:var(--dim); line-height:1.75; margin:0 0 14px; }
  .cfgnote b { color:var(--text); }
  .cfgnote .warn { color:var(--warn); }
  .modal { position:fixed; inset:0; z-index:50; display:flex; align-items:center;
           justify-content:center; padding:18px; background:rgba(6,9,15,.74); }
  .mbox { display:flex; flex-direction:column; width:100%; max-width:900px;
          max-height:84vh; background:var(--panel); border:1px solid var(--border);
          border-radius:14px; overflow:hidden; }
  .mhead { display:flex; align-items:center; gap:7px; flex-wrap:wrap;
           padding:11px 13px; border-bottom:1px solid var(--border); }
  .mhead b { font-size:13px; }
  .mhead .sp { flex:1; }
  .chip { font-size:11px; font-family:inherit; padding:3px 10px; border-radius:20px;
          cursor:pointer; background:var(--bg); color:var(--dim);
          border:1px solid var(--border); }
  .chip.on { background:var(--grad); color:#fff; border-color:transparent; }
  .mini { font-size:11px; font-family:inherit; padding:4px 10px; border-radius:8px;
          cursor:pointer; background:var(--bg); color:var(--text);
          border:1px solid var(--border); white-space:nowrap; }
  .mini:hover { border-color:var(--primary); }
  #mlog { margin:0; padding:11px 13px; flex:1; overflow:auto; white-space:pre;
          font:12px/1.6 Consolas,"Courier New",monospace; color:#cfd8e6; }
  #mlog .ts { color:#5d6a80; }
  .k-RX { color:#3ddc97; font-weight:600; }
  .k-AUTH { color:#4f8cff; font-weight:600; }
  .k-INFO { color:#8b95a7; }
  .k-REJECT { color:#ff6b6b; font-weight:600; }
  .k-other { color:#ffb454; font-weight:600; }
  .mfoot { padding:8px 13px; border-top:1px solid var(--border);
           font-size:11px; color:var(--dim); line-height:1.75; }
  @media (max-width:560px) { .hero { grid-template-columns:repeat(2,1fr); }
                             .mhead b { width:100%; } }
</style>
</head>
<body>
<div class="wrap">
  <div class="top">
    <h1>📡 BC28 数据看板</h1>
    <div class="acts">
      <button class="tbtn" id="btnlog" type="button">📜 接收日志</button>
      <a class="back" href="/apps">← 服务总览</a>
    </div>
  </div>
  <div class="sub" id="sub">加载中…</div>

  <div class="cfgbar">
    <span class="cfglab">上报模式</span>
    <div class="seg" id="seg">
      <button class="segbtn" type="button" data-v="normal">正常 · 30 秒</button>
      <button class="segbtn" type="button" data-v="save">省电 · 300 秒 + STOP + PSM</button>
    </div>
    <span class="cfgmsg" id="cfgmsg"></span>
  </div>
  <div class="cfgnote" id="cfgnote"></div>

  <div class="hero">
    <div><div class="k">室内温度</div><div class="v" id="h_temp">—</div><div class="u" id="h_temp_u">&nbsp;</div></div>
    <div><div class="k">运行时长</div><div class="v" id="h_up">—</div><div class="u">MCU 本次上电起</div></div>
    <div><div class="k">信号 CSQ</div><div class="v" id="h_sig">—</div><div class="u" id="h_sig_u">&nbsp;</div></div>
    <div><div class="k">状态</div><div class="v" id="h_st">—</div><div class="u" id="h_st_u">&nbsp;</div></div>
  </div>

  <div class="sec">📋 最新一条</div>
  <table id="t_last"></table>

  <div class="sec">🌡 室内温度历史</div>
  <div class="chartbox">
    <div class="ranges" id="ranges"></div>
    <canvas id="chart"></canvas>
    <div class="empty" id="chartempty" style="display:none">这段时间没有数据</div>
  </div>

  <div class="sec">📊 统计</div>
  <table id="t_stats"></table>

  <div class="sec">🕘 最近 50 条</div>
  <div id="recentbox"><table id="t_recent"></table></div>
  <div class="empty" id="recentempty" style="display:none">还没有数据</div>

  <div class="foot" id="foot"></div>
</div>

<!-- 「接收日志」弹窗: 默认 display:none, 点右上角那颗按钮才出现 -->
<div class="modal" id="modal" style="display:none">
  <div class="mbox" role="dialog" aria-modal="true" aria-label="接收日志">
    <div class="mhead">
      <b>📜 接收日志 sink.log</b>
      <span class="sp"></span>
      <span id="mcount"></span>
    </div>
    <div class="mhead">
      <span style="font-size:11px;color:var(--dim)">行数</span>
      <span id="mn"></span>
      <span style="font-size:11px;color:var(--dim);margin-left:6px">类型</span>
      <span id="mkinds"></span>
      <span class="sp"></span>
      <button class="mini" id="mauto" type="button">⏸ 自动刷新</button>
      <button class="mini" id="mrefresh" type="button">🔄 刷新</button>
      <button class="mini" id="mclose" type="button">✕ 关闭</button>
    </div>
    <pre id="mlog"></pre>
    <div class="mfoot" id="mfoot"></div>
  </div>
</div>
<script>
var RANGES = __RANGES__;
var cur = "24";

function hhmm(t) {           // 一律按 UTC+8 显示，跟服务端日志同一个钟
  var d = new Date((t + 8 * 3600) * 1000);
  return d.toISOString().slice(5, 16).replace("T", " ");
}
function ago(s) {
  if (s == null) return "";
  if (s < 90) return Math.round(s) + " 秒前";
  if (s < 5400) return Math.round(s / 60) + " 分钟前";
  if (s < 172800) return (s / 3600).toFixed(1) + " 小时前";
  return Math.round(s / 86400) + " 天前";
}
function fmtUp(s) {
  if (s == null) return "—";
  var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600),
      m = Math.floor(s % 3600 / 60), ss = Math.floor(s % 60);
  var p = function (n) { return String(n).padStart(2, "0"); };
  return d > 0 ? d + "天" + p(h) + ":" + p(m) : p(h) + ":" + p(m) + ":" + p(ss);
}
function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"]/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
  });
}
function row(k, v) { return "<tr><td class='k'>" + k + "</td><td>" + v + "</td></tr>"; }

function draw(pts) {
  var cv = document.getElementById("chart"), ctx = cv.getContext("2d");
  var box = cv.parentElement.getBoundingClientRect();
  var W = Math.max(280, box.width - 24), H = 200, dpr = window.devicePixelRatio || 1;
  cv.width = W * dpr; cv.height = H * dpr;
  cv.style.width = W + "px"; cv.style.height = H + "px";
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  var PL = 40, PR = 10, PT = 12, PB = 22;
  var iw = W - PL - PR, ih = H - PT - PB;

  if (!pts || pts.length < 2) {
    ctx.fillStyle = "#8b95a7"; ctx.font = "12px sans-serif";
    ctx.textAlign = "center"; ctx.fillText("数据点不够画线（至少 2 条）", W / 2, H / 2);
    return;
  }
  var t0 = pts[0].t, t1 = pts[pts.length - 1].t;
  if (t1 <= t0) t1 = t0 + 1;
  var vals = pts.map(function (p) { return p.v; });
  var lo = Math.min.apply(null, vals), hi = Math.max.apply(null, vals);
  if (hi - lo < 1) { var mid = (hi + lo) / 2; lo = mid - 0.5; hi = mid + 0.5; }
  var pad = (hi - lo) * 0.12; lo -= pad; hi += pad;

  var X = function (t) { return PL + (t - t0) / (t1 - t0) * iw; };
  var Y = function (v) { return PT + (hi - v) / (hi - lo) * ih; };

  // 网格 + 纵轴刻度
  ctx.font = "10px Consolas,monospace"; ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (var i = 0; i <= 3; i++) {
    var v = lo + (hi - lo) * i / 3, y = Y(v);
    ctx.strokeStyle = "#222939"; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(PL, y + 0.5); ctx.lineTo(W - PR, y + 0.5); ctx.stroke();
    ctx.fillStyle = "#8b95a7";
    ctx.fillText(v.toFixed(1) + "°", PL - 6, y);
  }

  // 折线 + 渐变填充
  ctx.beginPath();
  ctx.moveTo(X(pts[0].t), Y(pts[0].v));
  for (var j = 1; j < pts.length; j++) ctx.lineTo(X(pts[j].t), Y(pts[j].v));
  var g = ctx.createLinearGradient(0, PT, 0, PT + ih);
  g.addColorStop(0, "rgba(79,140,255,.30)");
  g.addColorStop(1, "rgba(79,140,255,0)");
  ctx.save();
  ctx.lineTo(X(pts[pts.length - 1].t), PT + ih);
  ctx.lineTo(X(pts[0].t), PT + ih);
  ctx.closePath(); ctx.fillStyle = g; ctx.fill();
  ctx.restore();

  ctx.beginPath();
  ctx.moveTo(X(pts[0].t), Y(pts[0].v));
  for (var k = 1; k < pts.length; k++) ctx.lineTo(X(pts[k].t), Y(pts[k].v));
  ctx.strokeStyle = "#4f8cff"; ctx.lineWidth = 1.8;
  ctx.lineJoin = "round"; ctx.stroke();

  // 最后一个点
  var lp = pts[pts.length - 1];
  ctx.beginPath(); ctx.arc(X(lp.t), Y(lp.v), 3, 0, 6.2832);
  ctx.fillStyle = "#4f8cff"; ctx.fill();

  // 横轴时间
  ctx.fillStyle = "#8b95a7"; ctx.font = "10px Consolas,monospace";
  ctx.textBaseline = "top";
  ctx.textAlign = "left";  ctx.fillText(hhmm(t0), PL, PT + ih + 6);
  if (t1 > t0) { ctx.textAlign = "right"; ctx.fillText(hhmm(t1), W - PR, PT + ih + 6); }
}

function render(d) {
  var L = d.last, S = d.stats || {};
  var nowS = d.now_s;

  if (!L) {
    document.getElementById("sub").textContent = d.empty || "还没有收到过数据。";
    document.getElementById("t_last").innerHTML = row("状态", "等待板子上报…");
    document.getElementById("h_st").textContent = "离线";
    document.getElementById("h_st").style.color = "var(--danger)";
    document.getElementById("chartempty").style.display = "block";
    document.getElementById("recentbox").style.display = "none";
    document.getElementById("recentempty").style.display = "block";
  } else {
    var age = d.now - L.epoch;
    var online = age < d.stale_s;
    /* 「每 30 秒一条」原来是写死的 —— 省电档下这句话就是假的。
       改成跟着服务端配置走(那是板子**将要**用的值)。 */
    document.getElementById("sub").innerHTML =
      "最后一条 <b>" + esc(L.ts) + "</b>（" + ago(age) + "） · 数据每 " +
      fmtSec(d.cfg && d.cfg.report_ms) + " 一条 · 页面每 30 秒自刷";

    document.getElementById("h_temp").textContent =
      L.temp == null ? "—" : L.temp.toFixed(1) + "°C";
    document.getElementById("h_up").textContent = fmtUp(L.up);
    document.getElementById("h_sig").textContent = L.csq == null ? "—" : L.csq;
    document.getElementById("h_sig_u").textContent =
      L.rsrp == null ? " " : L.rsrp.toFixed(1) + " dBm";
    var st = document.getElementById("h_st");
    st.textContent = online ? "在线" : "离线";
    st.style.color = online ? "var(--ok)" : "var(--danger)";
    document.getElementById("h_st_u").textContent = ago(age);

    var sig = "";
    if (L.csq != null) {
      sig = (L.csq >= 20 ? "优" : L.csq >= 15 ? "良" : L.csq >= 10 ? "一般" : "差");
    }
    document.getElementById("h_temp_u").textContent = sig ? "信号 " + sig : " ";

    var c = d.cell || {};      /* 小区统计 —— 「当前小区」那两行和统计页都用它 */
    var rows = "";
    rows += row("🕘 时间", esc(L.ts) + " <span style='color:var(--dim)'>(" + ago(age) + ")</span>");
    rows += row("🌡 室内温度", L.temp == null ? "本条没带 T=" : "<b>" + L.temp.toFixed(1) + " °C</b>");
    rows += row("⏱ 运行时长 (uptime)", fmtUp(L.up) + (L.up != null ? " <span style='color:var(--dim)'>(" + L.up + " 秒)</span>" : ""));
    if (d.boot) rows += row("🔌 本次上电于", esc(d.boot.ts) + " <span style='color:var(--dim)'>（按 uptime 倒推）</span>");
    rows += row("📶 CSQ", L.csq == null ? "—" : L.csq + " <span style='color:var(--dim)'>(0~31, 越大越好)</span>");
    rows += row("📶 RSRP / SNR", (L.rsrp == null ? "—" : L.rsrp.toFixed(1) + " dBm") +
                " / " + (L.snr == null ? "—" : L.snr.toFixed(1) + " dB"));
    rows += row("📶 ECL 覆盖等级", L.ecl == null ? "—" : L.ecl + " <span style='color:var(--dim)'>(0=好 1/2=靠重传补)</span>");
    rows += row("🔢 上报序号 #N", L.seq == null ? "—" : "#" + L.seq + " <span style='color:var(--dim)'>(MCU 重启会归 1)</span>");
    rows += row("🌐 来源 IP", "<span style='font-family:Consolas,monospace'>" + esc(L.peer) + "</span> <span style='color:var(--dim)'>(运营商分配)</span>");
    /* ---- 小区（Cell ID）----
       板子报文末尾的 CID=/PCI= **只说明连的是哪个基站，本身不含坐标**。
       摆在这里是为了回答"夜里那几次掉线是不是都在跳小区" —— 之前只能从
       RSRP 从 -69 掉到 -100、出口 IP 整段换掉去猜。定位那条路走不通，
       理由写在页面底部那段说明里（AT+QLBS 不支持 / 众包库查不到）。 */
    if (L.cid == null) {
      rows += row("📡 当前小区", "<span style='color:var(--dim)'>本条没带 CID= " +
        "（固件还没升到带小区号那版，或这轮没读上来）</span>");
    } else {
      rows += row("📡 当前小区", "<b>Cell ID " + L.cid + "</b>" +
        (L.pci == null ? "" : " <span style='color:var(--dim)'>· PCI " + L.pci + "</span>"));
      rows += row("🔀 本区间换过小区", (c.n_seen > 1)
        ? "<span style='color:var(--warn)'>见过 " + c.n_seen + " 个 · 换了 " + c.n_switch + " 次</span>"
        : "只见过 1 个 <span style='color:var(--dim)'>（没换过）</span>");
    }
    rows += row("📍 经纬度", d.site
      ? "<b>" + d.site.lat + ", " + d.site.lon + "</b>" + (d.site.name ? " · " + esc(d.site.name) : "")
      : "<span style='color:var(--warn)'>未提供</span> <span style='color:var(--dim)'>— 板子只能给出小区号（见上一行），给不出坐标</span>");
    document.getElementById("t_last").innerHTML = rows;

    // 图表
    if (d.series && d.series.length >= 2) {
      document.getElementById("chartempty").style.display = "none";
      document.getElementById("chart").style.display = "block";
      draw(d.series);
    } else {
      document.getElementById("chart").style.display = "none";
      document.getElementById("chartempty").style.display = "block";
      document.getElementById("chartempty").textContent =
        d.series && d.series.length === 1 ? "这个区间只有 1 条数据" : "这段时间没有数据";
    }

    // 统计
    var sr = "";
    sr += row("累计收到上报", "<b>" + S.rx_total + "</b> 条");
    sr += row("累计模块建连", "<b>" + S.conn_total + "</b> 次" +
              (S.rej_total ? " <span style='color:var(--danger)'>（另有 " + S.rej_total + " 次鉴权被拒）</span>" : ""));
    sr += row("本区间 (" + esc(S.window) + ")", S.win_rx + " 条 · " + esc(d.chart_span.from || "—") + " → " + esc(d.chart_span.to || "—"));
    sr += row("掉线缺口 (>5 分钟)", S.gap_n ? "<span style='color:var(--warn)'>" + S.gap_n + " 次</span>" : "0 次 🎉");
    (S.gaps || []).slice(0, 6).forEach(function (gp) {
      sr += row("&nbsp;&nbsp;↳ 断了 " + gp.min + " 分钟", esc(gp.from) + " → " + esc(gp.to));
    });
    /* ---- 换过的小区 ----
       掉线缺口和换小区这两件事**要对着看**：如果每次断线前面都跟着一次换小区，
       那根因就是无线环境在飘，而不是 socket / 固件的问题。这正是加 CID 的目的。 */
    if (c.n_seen > 1) {
      sr += row("🔀 换过的小区", (c.ids || []).map(function (v) {
        return "<code>" + v + "</code>"; }).join(" → "));
      (c.switches || []).forEach(function (sw) {
        sr += row("&nbsp;&nbsp;↳ " + esc(sw.at), sw.from + " → " + sw.to);
      });
    }
    sr += row("最早一条记录", esc(S.first_ts || "—"));
    document.getElementById("t_stats").innerHTML = sr;

    // 最近 50 条
    var rr = "<tr><th>时间</th><th>温度</th><th>运行</th><th>信号 CSQ / RSRP / SNR</th></tr>";
    (d.recent || []).forEach(function (r) {
      rr += "<tr><td style='font-family:Consolas,monospace;font-size:12px'>" + esc(r.ts) + "</td>" +
            "<td>" + (r.temp == null ? "—" : r.temp.toFixed(1) + "°") + "</td>" +
            "<td style='font-size:12px'>" + fmtUp(r.up) + "</td>" +
            "<td style='font-size:12px'>" + (r.csq == null ? "—" : r.csq) + " / " +
            (r.rsrp == null ? "—" : r.rsrp.toFixed(1)) + " / " +
            (r.snr == null ? "—" : r.snr.toFixed(1)) + "</td></tr>";
    });
    document.getElementById("t_recent").innerHTML = rr;
  }

  document.getElementById("foot").innerHTML =
    "🔎 <b>这些数从哪来</b><br>" +
    "板子每 " + fmtSec(d.cfg && d.cfg.report_ms) +
    "建一条 TCP 连到 VPS 的 <code>9101</code>，发一行<br>" +
    "<code>#N,CSQ=..,RSRP=..,SNR=..,ECL=0,UP=..,T=..,CID=..,PCI=..</code><br>" +
    "接收端 (<code>nbiot-sink</code>) 落成两份：人看的纯文本 <code>logs/sink.log</code>，" +
    "和这张页面查的 SQLite <code>data/history.db</code>。<br><br>" +
    "⚠️ <b>位置是怎么来的（以及为什么只有小区号）</b>：板上没有 GPS 模块。" +
    "报文里能带的只有<b>小区号</b>（<code>CID</code> / <code>PCI</code>，就是上面那行「当前小区」）——" +
    "它只说明<b>连的是哪个基站</b>，<b>本身不含坐标</b>。<br>" +
    "想把小区号换成坐标，两条路都试过了：模块固件（<code>V100R001C00SPC051</code>）" +
    "<b>不支持</b> <code>AT+QLBS</code> 一类的定位命令；拿这个小区号去查公开的众包库，" +
    "免费那家直接回 404 —— 中国大陆的 NB-IoT 小区在库里基本是空白。<br>" +
    "所以要看真实坐标，只能手工写一个 <code>/opt/nbiot-dash/site.json</code>：<br>" +
    "<code>{\"lat\": 39.90, \"lon\": 116.40, \"name\": \"家里\"}</code><br>" +
    "（<b>手工填写，不是上报的</b>；本页在登录后才可见，小区号和坐标都不会被公网看到。）";

  renderCfg(d);          /* 开关 + 「服务端配置 vs 板子当前」 */
}

function load() {
  fetch("/nb/api?h=" + cur, { cache: "no-store" })
    .then(function (r) { return r.json(); })
    .then(render)
    .catch(function (e) {
      document.getElementById("sub").textContent = "取数失败: " + e;
    });
}

/* ---------------- 上报模式开关（正常 / 省电） ----------------
   这是**拉取式**配置: 点这里只是把值写进服务端的 config.json,
   板子下次上报时在 ACK 里顺带取回。所以改完必须明说"等下一次上报",
   否则用户会以为点了没反应。
   ★ 开关状态由**服务端配置**决定, 不是由"我们刚点了什么"决定 ——
     这样两个人同时开这个页面, 看到的是一致的; 而"板子当前用的"是另一回事,
     单独用上行报文里的 RM=/S=/P= 回显来对照。 */
var PRESETS = {
  normal: { report_ms: 30000,  stop: 0, psm: 0 },
  save:   { report_ms: 300000, stop: 1, psm: 1 }
};
var cfgBusy = false;
var lastBoardCfg = null;

function fmtSec(ms) {
  if (ms == null) return "? 秒";
  return (ms % 1000 === 0) ? (ms / 1000) + " 秒" : (ms / 1000).toFixed(1) + " 秒";
}

function cfgMode(c) {
  if (!c) return "";
  for (var k in PRESETS) {
    var p = PRESETS[k];
    if (c.report_ms === p.report_ms && c.stop === p.stop && c.psm === p.psm) return k;
  }
  return "";   /* 手改过 config.json 的既不是正常也不是省电 —— 两个都不高亮, 别撒谎 */
}

/* 板子上行报文末尾的回显: ,RM=30000,S=0,P=0 (要烧了第一批固件才有)
   ★ 三个正则都必须锚到**逗号边界**。不锚的话 `/P=(\d)/` 会先撞上报文里的
     `UP=10` 的 "P=1", 把 psm 读成 1(实际是 0) —— 实测抓到的坑。
     报文里还有 CSQ=/RSRP=/SNR=/T=, 同理都得当成"词"而不是"子串"来匹配。 */
function boardFromRaw(raw) {
  if (!raw) return null;
  var m = /(?:^|,)RM=(\d+)/.exec(raw),
      s = /(?:^|,)S=(\d)/.exec(raw),
      p = /(?:^|,)P=(\d)/.exec(raw);
  if (!m || !s || !p) return null;
  return { report_ms: +m[1], stop: +s[1], psm: +p[1] };
}

function sameCfg(a, b) {
  return !!a && !!b && a.report_ms === b.report_ms &&
         a.stop === b.stop && a.psm === b.psm;
}

function renderCfg(d) {
  var c = d.cfg, seg = document.getElementById("seg");
  var ok = c && c.ok;
  var mode = cfgMode(ok ? c : null);

  Array.prototype.forEach.call(seg.children, function (b) {
    b.className = "segbtn" + (ok && b.getAttribute("data-v") === mode ? " on" : "");
    b.disabled = !ok || cfgBusy;
  });

  document.getElementById("cfgmsg").textContent =
    !ok ? ("配置不可用: " + ((c && c.err) || "nbcfg 未载入"))
        : cfgBusy ? "保存中…"
        : (mode ? "" : "非预设值 — 有人手改过 config.json");

  /* 服务端配置 vs 板子当前。板子那侧靠上行回显, 没烧新固件就取不到。 */
  lastBoardCfg = d.last ? boardFromRaw(d.last.raw) : null;
  var note;
  if (!ok) {
    note = "服务端配置读不到，开关已禁用。";
  } else {
    var srv = "服务端：<b>" + fmtSec(c.report_ms) + " 一报"
            + (c.stop ? " · 进 STOP" : "") + (c.psm ? " · PSM 协商" : "") + "</b>";
    var brd;
    if (!lastBoardCfg) {
      brd = "板子当前：<span class='warn'>读不到回显</span>"
          + "（旧固件不发 RM=/S=/P=，烧了第一批固件才有）";
    } else if (sameCfg(lastBoardCfg, c)) {
      brd = "板子当前：<b>" + fmtSec(lastBoardCfg.report_ms) + " 一报</b> 已一致 ✓";
    } else {
      brd = "板子当前：<b class='warn'>" + fmtSec(lastBoardCfg.report_ms) + " 一报"
          + (lastBoardCfg.stop ? " · STOP" : "") + (lastBoardCfg.psm ? " · PSM" : "")
          + "</b> — 还在用旧配置，等下次上报";
    }
    note = srv + "<br>" + brd
         + "<br>开关改动<b>不立即生效</b>：板子下次上报时取回，"
         + "最多等一个上报周期（省电档最长 " + Math.round(c.report_ms / 60000)
         + " 分钟）。" + (c.updated ? "　上次修改：" + esc(c.updated) : "");
  }
  document.getElementById("cfgnote").innerHTML = note;

  /* 省电档下掉线阈值跟着放大, 说一句 —— 免得用户以为看板不报警了 */
  if (ok && d.stale_s) {
    document.getElementById("cfgnote").innerHTML +=
      "<br>离线判定已按周期放大到 " + Math.round(d.stale_s / 60) + " 分钟。";
  }
}

function setMode(v) {
  if (cfgBusy) return;
  cfgBusy = true;
  document.getElementById("cfgmsg").textContent = "保存中…";
  Array.prototype.forEach.call(document.getElementById("seg").children,
                               function (b) { b.disabled = true; });
  fetch("/nb/cfg", { method: "POST", cache: "no-store",
                     headers: { "Content-Type": "application/json" },
                     body: JSON.stringify({ preset: v }) })
    .then(function (r) {
      /* 没登录/会话过期时后端回的是登录页 HTML(还是 200) —— 别硬解 JSON */
      var ct = r.headers.get("content-type") || "";
      if (ct.indexOf("json") < 0) { var e = new Error("登录已过期"); e.expired = true; throw e; }
      return r.json().then(function (j) { return { ok: r.ok, j: j }; });
    })
    .then(function (res) {
      cfgBusy = false;
      if (!res.ok || !res.j.ok) {
        document.getElementById("cfgmsg").textContent =
          "保存失败：" + ((res.j && res.j.err) || "未知错误");
        load();                                   // 把开关拨回服务端的真实值
        return;
      }
      load();
    })
    .catch(function (e) {
      cfgBusy = false;
      document.getElementById("cfgmsg").textContent = e.expired
        ? "登录已过期 —— 刷新页面重新登录。"
        : ("请求失败：" + e);
      load();
    });
}

function initCfg() {
  Array.prototype.forEach.call(document.getElementById("seg").children,
    function (b) {
      b.onclick = function () { setMode(b.getAttribute("data-v")); };
    });
}

/* ---------------- 「接收日志」弹窗 ---------------- */
/* 读的是服务端 sink.log 的原始行(不是 DB): 排查"板子碰没碰到服务器"要看 AUTH 和
   "连接结束"这些过程行, DB 里只有报文本身。日志里可能混着公网扫描器塞的字节,
   所以每行都过 esc()。 */
var NCHOICES = [[100, "100"], [200, "200"], [500, "500"], [0, "全部"]];
var KINDS = ["RX", "AUTH", "INFO", "REJECT"];
var logN = 200;                 // 0 = 全部(服务端上限 5000 行)
var logKinds = {};
var logTimer = null;
var lastLog = null;

KINDS.forEach(function (k) { logKinds[k] = true; });

function mkchip(label, on, onclick) {
  var b = document.createElement("button");
  b.type = "button";
  b.className = "chip" + (on ? " on" : "");
  b.textContent = label;
  b.onclick = onclick;
  return b;
}

function kindOf(l) {
  var m = l.match(/^\[[^\]]+\]\s+([A-Za-z_]+)/);
  return m ? m[1].toUpperCase() : "";
}

function renderLog(d) {
  lastLog = d;
  var el = document.getElementById("mlog");
  if (!d || !d.ok) {
    el.textContent = "读日志失败: " + ((d && d.err) || "未知错误");
    document.getElementById("mcount").textContent = "";
    return;
  }
  var atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 24;

  var n = 0, out = [];
  d.lines.forEach(function (l) {
    var k = kindOf(l);
    if (k && logKinds[k] === false) return;              // 这个类型被关掉了
    n++;
    var cls = logKinds.hasOwnProperty(k) ? "k-" + k : "k-other";
    var m = l.match(/^(\[[^\]]+\])\s+([A-Za-z_]+)\s*(.*)$/);
    if (m) {
      out.push("<span class='ts'>" + esc(m[1]) + "</span> " +
               "<span class='" + cls + "'>" + esc(m[2]) + "</span> " + esc(m[3]));
    } else {
      out.push("<span class='" + cls + "'>" + esc(l) + "</span>");
    }
  });
  el.innerHTML = out.join("\n");

  var bits = ["显示 " + n + " / 回读窗口 " + d.read_lines + " 行"];
  if (d.dropped) bits.push("筛掉垃圾 " + d.dropped + " 行");
  if (d.truncated) bits.push("⚠ 日志更大，只回读了尾部 " + Math.round(d.size / 1024) + " KB");
  document.getElementById("mcount").textContent = bits.join(" · ");

  if (atBottom) el.scrollTop = el.scrollHeight;          // 本来就贴底才跟着滚
}

function loadLog() {
  fetch("/nb/api?log=" + (logN || 5000), { cache: "no-store" })
    .then(function (r) {
      // 未登录/会话过期时后端回的是登录页 HTML(还是 200), 不是 JSON ——
      // 别拿 r.json() 硬解, 否则每 10 秒把日志区刷成一句 SyntaxError。
      var ct = r.headers.get("content-type") || "";
      if (!r.ok || ct.indexOf("json") < 0) { var e = new Error("登录已过期"); e.expired = true; throw e; }
      return r.json();
    })
    .then(renderLog)
    .catch(function (e) {
      if (logTimer) { clearInterval(logTimer); logTimer = null; autoLabel(); }   // 别继续空转
      document.getElementById("mlog").textContent = e.expired
        ? "登录已过期（重启服务会清空会话）—— 刷新页面重新登录，再重开这个窗口。"
        : "取日志失败: " + e;
    });
}

function autoLabel() {
  document.getElementById("mauto").textContent = logTimer ? "⏸ 自动刷新" : "▶ 自动刷新";
}

function toggleAuto() {
  if (logTimer) { clearInterval(logTimer); logTimer = null; }
  else {
    logTimer = setInterval(function () { if (!document.hidden) loadLog(); }, 10000);
  }
  autoLabel();
}

function openLog() {
  document.getElementById("modal").style.display = "flex";
  loadLog();
  if (!logTimer) toggleAuto();                           // 打开就默认自动刷新
  document.getElementById("mclose").focus();
}

function closeLog() {
  document.getElementById("modal").style.display = "none";
  if (logTimer) { clearInterval(logTimer); logTimer = null; }
  autoLabel();
}

function initLog() {
  var nb = document.getElementById("mn"), kb = document.getElementById("mkinds");
  NCHOICES.forEach(function (c) {
    nb.appendChild(mkchip(c[1], c[0] === logN, function () {
      logN = c[0];
      Array.prototype.forEach.call(nb.children, function (x, i) {
        x.className = "chip" + (NCHOICES[i][0] === logN ? " on" : "");
      });
      loadLog();
    }));
  });
  KINDS.forEach(function (k) {
    kb.appendChild(mkchip(k, true, function (ev) {
      logKinds[k] = !logKinds[k];
      ev.target.className = "chip" + (logKinds[k] ? " on" : "");
      renderLog(lastLog);                                // 纯前端筛选, 不重新请求
    }));
  });

  document.getElementById("btnlog").onclick = openLog;
  document.getElementById("mclose").onclick = closeLog;
  document.getElementById("mrefresh").onclick = loadLog;
  document.getElementById("mauto").onclick = toggleAuto;
  document.getElementById("modal").onclick = function (ev) {
    if (ev.target === this) closeLog();                  // 只有点遮罩才关
  };
  document.addEventListener("keydown", function (ev) {
    if (ev.key === "Escape" &&
        document.getElementById("modal").style.display === "flex") closeLog();
  });
  autoLabel();
}

(function init() {
  var box = document.getElementById("ranges");
  RANGES.forEach(function (rg) {
    var b = document.createElement("button");
    b.textContent = rg[1];
    if (rg[0] === cur) b.className = "on";
    b.onclick = function () {
      cur = rg[0];
      Array.prototype.forEach.call(box.children, function (x) { x.className = ""; });
      b.className = "on";
      load();
    };
    box.appendChild(b);
  });
  initLog();
  initCfg();
  load();
  setInterval(load, 30000);
})();
</script>
</body>
</html>
"""


def _hhmm(ep):
    return datetime.fromtimestamp(ep, TZ).strftime("%m-%d %H:%M")


def _rosql():
    """只读连接。库里一条数据都没有(文件还没建)时返回 None，调用方当作"空"。

    先试 mode=ro(看板绝对改不到采集数据)；万一 WAL 的 -shm 打不开就退回普通连接。
    """
    if not os.path.exists(DB):
        return None
    try:
        c = sqlite3.connect("file:%s?mode=ro" % DB, uri=True, timeout=3)
        c.execute("SELECT 1 FROM rx LIMIT 1")
        return c
    except sqlite3.Error:
        try:
            return sqlite3.connect(DB, timeout=3)
        except sqlite3.Error:
            return None


def _has_col(cur, table, col):
    """表里有没有这一列。cid/pci 是 2026-09-19 才加的，而**看板有可能先于 sink 重启**
    （库里还是老结构）；直接 SELECT 那一列会让整个 payload 变成一句"读库失败"，
    为了一个附加字段把整页打掉不值当，所以先探一下、没有就用 NULL 顶上。"""
    try:
        return any(d[1] == col for d in
                   cur.execute("PRAGMA table_info(%s)" % table).fetchall())
    except sqlite3.Error:
        return False


def _thin(pts, cap):
    """抽稀到 cap 个点以内 —— 7 天的原始点会到 2 万个，客户端没必要扛。"""
    n = len(pts)
    if n <= cap:
        return pts
    step = (n + cap - 1) // cap
    out = pts[::step]
    if out[-1] is not pts[-1]:
        out.append(pts[-1])
    return out


def _site():
    try:
        with open(SITE_FILE, encoding="utf-8") as fh:
            d = json.load(fh)
        return {"lat": float(d["lat"]), "lon": float(d["lon"]),
                "name": str(d.get("name", "")).strip()}
    except Exception:
        return None


def _span_label(h):
    if h == 24:
        return "近 24 小时"
    if h == 168:
        return "近 7 天"
    if h == 1:
        return "近 1 小时"
    return "近 %g 小时" % h


# ---- 上报模式（正常 / 省电）------------------------------------------------
# 两档是**固定预设**，不给任意值 —— 看板上是个两档开关，不是个输入框。
# 服务端只负责把值写进 config.json；板子下次上报时在 ACK 里顺带取回，
# 所以**改完不会立刻生效**（这不是缺陷，是 NB-IoT 上唯一可行的做法）。
PRESET_NORMAL = {"report_ms": 30000, "stop": 0, "psm": 0}
PRESET_SAVE = {"report_ms": 300000, "stop": 1, "psm": 1}


def cfg_payload():
    """当前配置。nbcfg 没载入时 ok=False，看板据此把开关禁掉。"""
    if nbcfg is None:
        return {"ok": False, "err": "nbcfg 未载入", "presets": {}}
    cfg = nbcfg.load()
    return {"ok": True, "err": None,
            "report_ms": cfg["report_ms"], "stop": cfg["stop"], "psm": cfg["psm"],
            "updated": cfg["updated"], "file": nbcfg.CONFIG_FILE,
            "min_ms": nbcfg.REPORT_MS_MIN, "max_ms": nbcfg.REPORT_MS_MAX,
            "presets": {"normal": PRESET_NORMAL, "save": PRESET_SAVE}}


def cfg_set(body):
    """/nb/cfg 的 POST。body 给 {"preset":"normal"|"save"} 或三个显式字段。

    返回 (json, http_status)。**写失败如实回 500** —— 这是交互路径，
    必须让用户看见"没保存成功"，而不是默默以为生效了。
    """
    if nbcfg is None:
        return {"ok": False, "err": "nbcfg 未载入"}, 500
    if not isinstance(body, dict):
        return {"ok": False, "err": "请求体必须是 JSON 对象"}, 400

    p = body.get("preset")
    if p is not None:
        if p == "normal":
            new = dict(PRESET_NORMAL)
        elif p == "save":
            new = dict(PRESET_SAVE)
        else:
            return {"ok": False, "err": "preset 只能是 normal 或 save"}, 400
    else:
        new = dict((k, body[k]) for k in ("report_ms", "stop", "psm") if k in body)
        if not new:
            return {"ok": False, "err": "要么给 preset，要么给 report_ms/stop/psm"}, 400

    try:
        nbcfg.save(new)
    except Exception as exc:
        return {"ok": False, "err": "保存失败: %s: %s" % (type(exc).__name__, exc)}, 500

    out = cfg_payload()          # 回读一遍: 显示的必须是**夹完之后真正存下去**的值
    return out, 200


def payload(h):
    """给 /nb/api 的 JSON。h = 1 / 6 / 24 / 168 / all。"""
    now = time.time()
    stale_s, gap_s = thresholds()
    out = {"ok": True, "now": now, "now_s": datetime.fromtimestamp(now, TZ).strftime("%Y-%m-%d %H:%M:%S"),
           "stale_s": stale_s, "gap_s": gap_s,
           "cfg": cfg_payload(), "cfg_ok": nbcfg is not None,
           "site": _site(), "last": None, "boot": None,
           "series": [], "recent": [], "stats": {},
           "cell": {"n_switch": 0, "n_seen": 0, "ids": [], "switches": []},
           "chart_span": {"from": None, "to": None}, "empty": ""}

    c = _rosql()
    if c is None:
        out["empty"] = "还没收到过数据 —— 板子还没成功上报过，或者 %s 还没建起来。" % DB
        return out

    try:
        cur = c.cursor()
        last = cur.execute("SELECT * FROM rx ORDER BY id DESC LIMIT 1").fetchone()
        cols = [d[0] for d in cur.description] if cur.description else []
        if last:
            out["last"] = dict(zip(cols, last))
            if out["last"].get("up") is not None:
                b = out["last"]["epoch"] - out["last"]["up"]
                out["boot"] = {"epoch": b, "ts": _hhmm(b), "uptime_s": out["last"]["up"]}

        rx_total = cur.execute("SELECT COUNT(*) FROM rx").fetchone()[0]
        conn_total = cur.execute("SELECT COUNT(*) FROM conn WHERE kind='AUTH'").fetchone()[0]
        rej_total = cur.execute("SELECT COUNT(*) FROM conn WHERE kind='REJECT'").fetchone()[0]
        first = cur.execute("SELECT ts FROM rx ORDER BY id ASC LIMIT 1").fetchone()

        if h == "all":
            since, label = 0.0, "全部"
        else:
            try:
                hours = max(1.0, min(24.0 * 31, float(h)))
            except (TypeError, ValueError):
                hours = 24.0
            since, label = now - hours * 3600, _span_label(hours)

        rows = cur.execute(
            "SELECT epoch, temp, up, csq, rsrp, snr, ecl, seq, ts, %s FROM rx"
            " WHERE epoch >= ? ORDER BY id"
            % ("cid" if _has_col(cur, "rx", "cid") else "NULL"), (since,)).fetchall()
        rows = [dict(zip([d[0] for d in cur.description], r)) for r in rows]

        gaps, prev = [], None
        for r in rows:
            if prev is not None and r["epoch"] - prev > gap_s:
                gaps.append({"from": _hhmm(prev), "to": _hhmm(r["epoch"]),
                             "min": round((r["epoch"] - prev) / 60.0, 1)})
            prev = r["epoch"]

        # ---- 小区切换 ----
        # 只回答一件事：**这个区间里换没换过小区、什么时候换的**。
        # 拿去查坐标那条路走不通（见 PAGE 底部那段说明），也不该在看板里偷偷调外部接口。
        #
        # ★ 老固件/没读到的那几条 cid 是 NULL，**必须跳过** —— 不跳的话
        #   "先有后无"或"一直没有"会被数成一串换小区，头一晚全是假数据。
        seen, switches, last_cid = [], [], None
        for r in rows:
            v = r.get("cid")
            if v is None:
                continue
            if v not in seen:
                seen.append(v)
            if (last_cid is not None) and (v != last_cid):
                switches.append({"from": last_cid, "to": v, "at": _hhmm(r["epoch"])})
            last_cid = v
        out["cell"] = {"n_switch": len(switches), "n_seen": len(seen),
                       "ids": seen[:12], "switches": switches[-8:]}

        out["series"] = _thin([{"t": r["epoch"], "v": r["temp"]}
                               for r in rows if r["temp"] is not None], 700)
        out["recent"] = [dict(zip([d[0] for d in cur.description], r)) for r in
                         cur.execute("SELECT * FROM rx ORDER BY id DESC LIMIT 50").fetchall()]
        out["stats"] = {"rx_total": rx_total, "conn_total": conn_total,
                        "rej_total": rej_total, "window": label, "win_rx": len(rows),
                        "gaps": gaps[:20], "gap_n": len(gaps),
                        "first_ts": first[0] if first else None}
        out["chart_span"] = {"from": _hhmm(rows[0]["epoch"]) if rows else None,
                             "to": _hhmm(rows[-1]["epoch"]) if rows else None}
        return out
    except sqlite3.Error as exc:
        out["empty"] = "读库失败: %s" % exc
        return out
    finally:
        c.close()


def page():
    """整页 HTML。区间按钮从 RANGES 生成，跟 payload() 支持的取值同一份定义。"""
    rng = json.dumps([[k, v] for k, v in RANGES], ensure_ascii=False)
    return PAGE.replace("__RANGES__", rng)


def _read_tail(path, max_bytes):
    """从文件**尾部**回读最多 max_bytes 字节。返回 (行列表, 文件大小, 错误串)。

    刻意不整份读进来: 这个日志不轮转, 只会一直长(约 480 行/天)。
    回读到的那一行很可能是被截断的半行, 丢掉。
    """
    try:
        f = open(path, "rb")
    except OSError as exc:
        return None, 0, "打不开日志 %s: %s" % (path, exc)
    try:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        want = min(size, max_bytes)
        f.seek(size - want)
        blob = f.read(want)
    except OSError as exc:
        return None, 0, "读日志失败 %s: %s" % (path, exc)
    finally:
        f.close()

    lines = blob.decode("utf-8", "replace").splitlines()
    if want < size and lines:
        lines = lines[1:]              # 首行多半被截断, 丢了
    return lines, size, None


def log_payload(n):
    """给 /nb/api?log=N 的 JSON —— 页面右上角「📜 接收日志」弹窗用的。

    返回的是 sink.log 的**原始行**(AUTH / RX / INFO / REJECT), 不是 DB 里的报文:
    排查"板子碰没碰到服务器"要看的就是 AUTH 和"连接结束"这些过程行。
    """
    try:
        n = int(n)
    except (TypeError, ValueError):
        n = 200
    n = max(1, min(LOG_MAX_LINES, n))

    lines, size, err = _read_tail(LOG, LOG_TAIL_BYTES)
    if lines is None:
        return {"ok": False, "err": err, "file": LOG, "lines": [], "n": 0,
                "read_lines": 0, "dropped": 0, "truncated": False, "size": size}

    kept = [ln for ln in lines if LOG_LINE_RE.match(ln)]
    out = kept[-n:]
    return {"ok": True, "err": None, "file": LOG,
            "lines": out, "n": len(out),
            "read_lines": len(kept),           # 回读窗口里合格的行数
            "dropped": len(lines) - len(kept),  # 被筛掉的(扫描器垃圾), 如实报
            "truncated": size > LOG_TAIL_BYTES,  # 撞到回读上限: 更早的行没读进来
            "size": size}
