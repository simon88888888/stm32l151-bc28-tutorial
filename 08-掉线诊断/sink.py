#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""NB-IoT 上报接收端 —— **只收、只落盘**，不做任何"按输入去执行"的动作。

    SINK_TOKEN=<你自己定的串> python3 sink.py

环境变量:
    SINK_TOKEN   必填。模块每条连接的第一行必须发 `#TOKEN <它>`，不匹配就丢弃。
                 **没设就直接拒绝启动** —— 免得无意中把一个无鉴权的公网端口跑起来。
    SINK_PORT    默认 9101（避开本机已占的 80/443/7000/8080/8081/8188/8189/8191）
    SINK_LOGDIR  默认 /opt/nbiot-sink/logs
    SINK_CFG     默认 /opt/nbiot-sink/config.json（下行配置，与看板共用）

协议（一行一条，`\\r\\n` 结尾）:
    → #TOKEN <串>        握手，服务端回 `AUTH OK` + `#CFG <ms> <stop> <psm>`
    → <数据行>           服务端落盘，回 `OK <累计条数>` + `#CFG <ms> <stop> <psm>`
    首行不是合法 `#TOKEN` 的连接：记一条 REJECT 后立刻关闭。

下行配置（`#CFG`）:
    服务端**每条回执都带**一遍（幂等），板子下次上报时顺便取回 —— 这是 NB-IoT 上
    唯一合理的做法：板子进了 STOP 就收不到字节，没法靠服务端"推"。所以开关
    **不立即生效**，最多等一个上报周期。
    配置存在同目录 config.json，两个进程共用，改它**不用重启任何服务**。
    ★ 拼在**同一次 write** 里，不能拆成两次 —— 一次 write = 一次 sendall。

日志格式（**故意排成好 grep 的样子**）:
    [2026-09-13 21:04:05 +08] RX     203.0.113.5  #12,CSQ=28,RSRP=-69.1,...
    [2026-09-13 21:04:05 +08] REJECT 1.2.3.4      #TOKEN bad

    收了多少条:      grep -c ' RX ' sink.log
    被谁扫过:        grep ' REJECT ' sink.log

统计模式（读日志算间隔，用来核"30 秒一条"）:
    python3 sink.py --stats
"""

import os
import re
import sqlite3
import sys
import threading
from datetime import datetime, timedelta, timezone
from socketserver import StreamRequestHandler, ThreadingTCPServer

TZ = timezone(timedelta(hours=8))  # 固定 UTC+8：跟板子上的 uptime、你的墙上钟对得上

PORT = int(os.environ.get("SINK_PORT", "9101"))
LOGDIR = os.environ.get("SINK_LOGDIR", "/opt/nbiot-sink/logs")
TOKEN = os.environ.get("SINK_TOKEN", "").strip()

LOG_FILE = os.path.join(LOGDIR, "sink.log")

_lock = threading.Lock()
_rx = 0


# ---- 下行配置（板子的上报间隔 / STOP / PSM）--------------------------------
# 配置本体在 nbcfg.py + config.json，**和看板(authproxy)共用同一份**。
# 这里只负责"每条回执顺手带一遍"。
#
# ★ 载入失败**绝不能影响收数** —— 收数远比配置重要。所以整个包在 try 里，
#   失败就把 nbcfg 置 None，cfg_now() 返回空串，退化成原来的"只回 OK"。
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nbcfg
except Exception as _exc:
    nbcfg = None
    print("!! 载入 nbcfg 失败(%s) —— 下行配置关闭, 收数不受影响"
          % _exc, file=sys.stderr, flush=True)

_cfg_lock = threading.Lock()   # 只管 _cfg_seen；**与 _lock 是两个锁, 别混**
_cfg_seen = None               # 上一次发出去的配置，用来"只在变化时记一条 INFO"


def cfg_now():
    """当前该下发的 `#CFG` 行(bytes)；nbcfg 没载入就返回空串。

    ★ 只在配置**变化**时记一条 INFO。每条 ACK 都记的话，哪怕 300 秒一轮也会把
      sink.log 刷满，把 `grep -c ' RX '` 这个最常用的数稀释掉。
    ★ log() 内部会拿 _lock，所以必须**在 _cfg_lock 之外**调它 ——
      这里先 compare-and-set 再出锁调用，两个锁的获取顺序只有 _cfg_lock -> _lock
      这一个方向，不会死锁。
    ★ 整体再包一层 try：配置出任何问题都只是"这一轮不下发"，**绝不能让
      一条上报因为配置处理失败而丢掉**。
    """
    global _cfg_seen
    if nbcfg is None:
        return b""
    try:
        cfg = nbcfg.load()
        key = (cfg["report_ms"], cfg["stop"], cfg["psm"])
        with _cfg_lock:
            changed = key != _cfg_seen
            _cfg_seen = key
        if changed:
            log("INFO", "-", "下行配置 -> #CFG %d %d %d" % key)
        return nbcfg.cfg_line(cfg)
    except Exception as exc:
        print("!! 取下行配置失败: %s" % exc, file=sys.stderr, flush=True)
        return b""


# ---- 结构化落盘（第 5 篇的云端看板要查历史）--------------------------------
# sink 原本只写纯文本 sink.log：人看得舒服、grep 得动，但**没法查历史**。
# 这里在文本日志之外再落一份 SQLite，一行 RX 一条记录，看板直接查它。
#
# ★ 日志仍然是唯一真相：DB 写失败只在 stderr 留一行，绝不影响收数、更不会断连接。
DB_FILE = os.environ.get("SINK_DB", "/opt/nbiot-sink/data/history.db")

# 板子报文形如:  #12,CSQ=28,RSRP=-69.1,SNR=13.6,ECL=0,UP=158,T=26.8
# 逐字段取值, **不按位置切** —— 老固件少发 T= 的那几条也能照样存下来(存成 NULL)。
# 2026-09-19 起固件末尾多了 ,CID=<小区号>,PCI=<物理小区号>: 老固件没有这两栏,
# 上面那条"少一栏也能存"的规矩照样兜住(存成 NULL), 不需要板子先升。
RE_SEQ = re.compile(r"#(\d+)")
RE_KV = re.compile(r"([A-Z]+)=(-?\d+(?:\.\d+)?)")


def db_init():
    os.makedirs(os.path.dirname(DB_FILE), exist_ok=True)
    c = sqlite3.connect(DB_FILE, timeout=5)
    try:
        with c:
            c.execute("PRAGMA journal_mode=WAL")   # 看板在读的同时还能继续写
            c.execute("""CREATE TABLE IF NOT EXISTS rx(
                           id INTEGER PRIMARY KEY AUTOINCREMENT,
                           ts TEXT, epoch REAL, peer TEXT, seq INTEGER,
                           csq INTEGER, rsrp REAL, snr REAL, ecl INTEGER,
                           up INTEGER, temp REAL, raw TEXT,
                           cid INTEGER, pci INTEGER)""")
            c.execute("""CREATE TABLE IF NOT EXISTS conn(
                           id INTEGER PRIMARY KEY AUTOINCREMENT,
                           ts TEXT, epoch REAL, peer TEXT, kind TEXT)""")
            c.execute("CREATE INDEX IF NOT EXISTS ix_rx_epoch ON rx(epoch)")
            _db_migrate(c)
    finally:
        c.close()


def _db_migrate(c):
    """给**已经存在**的老库补列 —— 上面那句 CREATE TABLE IF NOT EXISTS 对已有的表
    一个字都不会动，所以它只管新库，老库得走这里。

    2026-09-19 补 cid / pci：板子上报末尾的 `,CID=<小区号>,PCI=<物理小区号>`。
    这两个数**不是用来定位的**（这条路上试过了，见看板上那段说明），是用来回答
    "晚上老掉线，到底是不是在跳小区" —— 之前只能从 RSRP 从 -69 掉到 -100、
    出口 IP 整段换掉去**猜**，猜了整整一晚上。

    ★ SQLite 没有 `ADD COLUMN IF NOT EXISTS`，只能先 PRAGMA 查一遍再补。
    ★ 外面套 try：**补列失败绝不能让 sink 起不来** —— 收数比这个功能重要得多。
    """
    try:
        have = set(r[1] for r in c.execute("PRAGMA table_info(rx)").fetchall())
    except sqlite3.Error as exc:
        print("!! 读表结构失败, 跳过补列: %s" % exc, file=sys.stderr, flush=True)
        return
    for col in ("cid", "pci"):
        if col in have:
            continue
        try:
            c.execute("ALTER TABLE rx ADD COLUMN %s INTEGER" % col)
            print(">> rx 补列 %s 完成" % col, file=sys.stderr, flush=True)
        except sqlite3.Error as exc:
            print("!! 补列 %s 失败: %s" % (col, exc), file=sys.stderr, flush=True)


def _db_write(sql, args):
    """每次开一条新连接写完就关。一分钟才两三条，开销可以忽略，
    换来的是**没有跨线程共享的连接对象** —— ThreadingTCPServer 是多线程的，
    共享 sqlite3 连接默认不能跨线程用。"""
    try:
        c = sqlite3.connect(DB_FILE, timeout=5)
        try:
            with c:
                c.execute(sql, args)
        finally:
            c.close()
    except Exception as exc:
        print("!! 写 DB 失败: %s" % exc, file=sys.stderr, flush=True)


def db_conn(peer, kind):
    """记一次模块建连（AUTH 成功 / REJECT 被拒）—— 看板的"累计建连次数"。"""
    now = datetime.now(TZ)
    _db_write("INSERT INTO conn(ts, epoch, peer, kind) VALUES(?,?,?,?)",
              (now.strftime("%Y-%m-%d %H:%M:%S"), now.timestamp(), peer, kind))


def db_rx(peer, text):
    now = datetime.now(TZ)
    kv = dict(RE_KV.findall(text))
    m = RE_SEQ.search(text)

    def num(key, cast):
        try:
            return cast(kv[key])
        except (KeyError, TypeError, ValueError):
            return None

    _db_write("INSERT INTO rx(ts, epoch, peer, seq, csq, rsrp, snr, ecl, up, temp, raw,"
              " cid, pci) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
              (now.strftime("%Y-%m-%d %H:%M:%S"), now.timestamp(), peer,
               int(m.group(1)) if m else None,
               num("CSQ", int), num("RSRP", float), num("SNR", float), num("ECL", int),
               num("UP", int), num("T", float), text[:200],
               num("CID", int), num("PCI", int)))


def log(kind, peer, text):
    """kind: AUTH / RX / REJECT / INFO —— 定宽 6 字符，日志好对齐也好 grep。"""
    stamp = datetime.now(TZ).strftime("%Y-%m-%d %H:%M:%S +08")
    line = "[%s] %-6s %s  %s" % (stamp, kind, peer, text)
    try:
        with _lock, open(LOG_FILE, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError as exc:  # 落盘失败也要让 journal 里留下痕迹
        print("!! 写日志失败: %s" % exc, file=sys.stderr, flush=True)
    print(line, flush=True)


def bump():
    """累计收条数，回给模块用作 ACK 序号（也方便你一眼看出有没有丢）。"""
    global _rx
    with _lock:
        _rx += 1
        return _rx


class Handler(StreamRequestHandler):
    timeout = 600  # 一条连接最多挂 10 分钟；长连接模式下模块会一直连着，够用

    def handle(self):
        peer = self.client_address[0]
        authed = False
        try:
            while True:
                raw = self.rfile.readline(1024)  # 一行最多 1 KB，防被灌爆内存
                if not raw:
                    break
                text = raw.decode("utf-8", "replace").rstrip("\r\n")
                if not text:
                    continue

                if not authed:
                    m = re.match(r"^#TOKEN\s+(\S+)$", text)
                    if not m or m.group(1) != TOKEN:
                        log("REJECT", peer, text[:80])
                        db_conn(peer, "REJECT")
                        break
                    authed = True
                    log("AUTH", peer, "token ok")
                    db_conn(peer, "AUTH")
                    # ★ 必须和 #CFG 拼在**同一次 write**（一次 write = 一次
                    #   sendall）：拆成两次的话小包可能被拆到两个 TCP 段里，
                    #   板子读完第一条就走了，配置要等到下一轮才拿到。
                    self.wfile.write(b"AUTH OK\n" + cfg_now())
                    continue

                log("RX", peer, text)
                db_rx(peer, text)
                # 求值顺序: bump() 先跑完(内部拿/放 _lock), cfg_now() 才开始 ——
                # 不会出现"持着 _lock 再去 log()"那种死锁。
                self.wfile.write(("OK %d\n" % bump()).encode() + cfg_now())
        except (TimeoutError, ConnectionResetError, BrokenPipeError, OSError):
            pass  # 模块睡下去/被运营商掐掉，都是常态，不值得刷错误
        finally:
            if authed:
                log("INFO", peer, "连接结束")


class Server(ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True  # 主进程退出时别被卡住的连接拖住


def stats():
    """读日志，报「收了多少条 + 相邻两条的间隔」，用来核 30 秒一次的节奏。"""
    if not os.path.exists(LOG_FILE):
        print("没有日志: %s" % LOG_FILE)
        return
    stamps, kinds = [], {}
    pat = re.compile(r"^\[(\S+ \S+) \+08\] (\S+)\s")
    with open(LOG_FILE, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = pat.match(line)
            if not m:
                continue
            kinds[m.group(2)] = kinds.get(m.group(2), 0) + 1
            if m.group(2) == "RX":
                stamps.append(datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S"))

    print("日志: %s" % LOG_FILE)
    for k in sorted(kinds):
        print("  %-6s %d" % (k, kinds[k]))

    if len(stamps) < 2:
        return
    gaps = [(b - a).total_seconds() for a, b in zip(stamps, stamps[1:])]
    span = (stamps[-1] - stamps[0]).total_seconds()
    print("\n收到 %d 条，跨度 %.0f 秒（平均 %.1f 秒一条）" % (len(stamps), span, span / len(gaps)))
    print("间隔: 最小 %.0f / 中位 %.0f / 最大 %.0f 秒" % (
        min(gaps), sorted(gaps)[len(gaps) // 2], max(gaps)))


def main():
    if "--stats" in sys.argv:
        stats()
        return
    if not TOKEN:
        sys.exit("拒绝启动: 没设 SINK_TOKEN。公网端口必须有鉴权，"
                 "例如  SINK_TOKEN=$(openssl rand -hex 16) python3 sink.py")
    os.makedirs(LOGDIR, exist_ok=True)
    db_init()
    log("INFO", "-", "监听 0.0.0.0:%d，日志 %s" % (PORT, LOG_FILE))
    Server(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
