# -*- coding: utf-8 -*-
"""BC28 数据看板 —— /apps 门户里点进来的那一页（第 5 篇的"云端看板"）。

数据源是 nbiot-sink 落的 SQLite:  /opt/nbiot-sink/data/history.db
    rx    一行 = 板子上报的一条报文  #12,CSQ=28,RSRP=-69.1,SNR=13.6,ECL=0,UP=158,T=26.8
    conn  一行 = 模块建的一次连接(AUTH 成功 / REJECT 被拒)

**只读打开**(mode=ro): 看板没有任何写权限, 采集那条路不受它影响。
authproxy.py 只把 /nb/ 和 /nb/api 两条路由指过来, 其余什么都不用知道。
"""
import json
import os
import sqlite3
import time
from datetime import datetime, timedelta, timezone

DB = os.environ.get("NBIOT_DB", "/opt/nbiot-sink/data/history.db")
SITE_FILE = os.environ.get("NBIOT_SITE", "/opt/nbiot-dash/site.json")
TZ = timezone(timedelta(hours=8))  # 固定 UTC+8: 跟板子上的 uptime、你的墙上钟对得上

# 离线判定: 板子约 30 秒发一条, 超过这个秒数没动静就当掉线。
# (单次建连失败重试是 3~5 秒, 硬恢复要重启模块 ~15 秒, 所以 3 分钟足够宽松。)
STALE_S = 180

# 相邻两条间隔超过这个秒数, 算一次"掉线缺口"(正常 30 秒一条)
GAP_S = 300

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
  @media (max-width:560px) { .hero { grid-template-columns:repeat(2,1fr); } }
</style>
</head>
<body>
<div class="wrap">
  <div class="top">
    <h1>📡 BC28 数据看板</h1>
    <a class="back" href="/apps">← 服务总览</a>
  </div>
  <div class="sub" id="sub">加载中…</div>

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
    document.getElementById("sub").innerHTML =
      "最后一条 <b>" + esc(L.ts) + "</b>（" + ago(age) + "） · 数据每 30 秒一条 · 页面每 30 秒自刷";

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
    rows += row("📍 经纬度", d.site
      ? "<b>" + d.site.lat + ", " + d.site.lon + "</b>" + (d.site.name ? " · " + esc(d.site.name) : "")
      : "<span style='color:var(--warn)'>未提供</span> <span style='color:var(--dim)'>— 板上没有 GPS，报文里只有 #N,CSQ,RSRP,SNR,ECL,UP,T</span>");
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
    "板子每 30 秒建一条 TCP 连到 VPS 的 <code>9101</code>，发一行<br>" +
    "<code>#N,CSQ=..,RSRP=..,SNR=..,ECL=0,UP=..,T=..</code><br>" +
    "接收端 (<code>nbiot-sink</code>) 落成两份：人看的纯文本 <code>logs/sink.log</code>，" +
    "和这张页面查的 SQLite <code>data/history.db</code>。<br><br>" +
    "⚠️ <b>经纬度为什么没有</b>：板上没有 GPS 模块，报文里也不含位置。" +
    "要显示真实坐标，在 VPS 上写一个 <code>/opt/nbiot-dash/site.json</code>：<br>" +
    "<code>{\"lat\": 39.90, \"lon\": 116.40, \"name\": \"家里\"}</code><br>" +
    "（<b>手工填写，不是上报的</b>；本页在登录后才可见，坐标不会被公网看到。）";
}

function load() {
  fetch("/nb/api?h=" + cur, { cache: "no-store" })
    .then(function (r) { return r.json(); })
    .then(render)
    .catch(function (e) {
      document.getElementById("sub").textContent = "取数失败: " + e;
    });
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


def payload(h):
    """给 /nb/api 的 JSON。h = 1 / 6 / 24 / 168 / all。"""
    now = time.time()
    out = {"ok": True, "now": now, "now_s": datetime.fromtimestamp(now, TZ).strftime("%Y-%m-%d %H:%M:%S"),
           "stale_s": STALE_S, "site": _site(), "last": None, "boot": None,
           "series": [], "recent": [], "stats": {},
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
            "SELECT epoch, temp, up, csq, rsrp, snr, ecl, seq, ts FROM rx"
            " WHERE epoch >= ? ORDER BY id", (since,)).fetchall()
        rows = [dict(zip([d[0] for d in cur.description], r)) for r in rows]

        gaps, prev = [], None
        for r in rows:
            if prev is not None and r["epoch"] - prev > GAP_S:
                gaps.append({"from": _hhmm(prev), "to": _hhmm(r["epoch"]),
                             "min": round((r["epoch"] - prev) / 60.0, 1)})
            prev = r["epoch"]

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
