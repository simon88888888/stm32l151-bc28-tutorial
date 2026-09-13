#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""NB-IoT 上报接收端 —— **只收、只落盘**，不做任何"按输入去执行"的动作。

    SINK_TOKEN=<你自己定的串> python3 sink.py

环境变量:
    SINK_TOKEN   必填。模块每条连接的第一行必须发 `#TOKEN <它>`，不匹配就丢弃。
                 **没设就直接拒绝启动** —— 免得无意中把一个无鉴权的公网端口跑起来。
    SINK_PORT    默认 9101（避开本机已占的 80/443/7000/8080/8081/8188/8189/8191）
    SINK_LOGDIR  默认是 sink.py 同级的 logs/ 目录

协议（一行一条，`\\r\\n` 结尾）:
    → #TOKEN <串>        握手，服务端回 `AUTH OK`
    → <数据行>           服务端落盘，回 `OK <累计条数>`
    首行不是合法 `#TOKEN` 的连接：记一条 REJECT 后立刻关闭。

日志格式（**故意排成好 grep 的样子**）:
    [2026-09-13 21:04:05 +08] RX     172.104.1.1  #12,CSQ=28,RSRP=-69.1,...
    [2026-09-13 21:04:05 +08] REJECT 1.2.3.4      #TOKEN bad

    收了多少条:      grep -c ' RX ' sink.log
    被谁扫过:        grep ' REJECT ' sink.log

统计模式（读日志算间隔，用来核"30 秒一条"）:
    python3 sink.py --stats
"""

import os
import re
import sys
import threading
from datetime import datetime, timedelta, timezone
from socketserver import StreamRequestHandler, ThreadingTCPServer

TZ = timezone(timedelta(hours=8))  # 固定 UTC+8：跟板子上的 uptime、你的墙上钟对得上

PORT = int(os.environ.get("SINK_PORT", "9101"))
LOGDIR = os.environ.get("SINK_LOGDIR",
                 os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs"))
TOKEN = os.environ.get("SINK_TOKEN", "").strip()

LOG_FILE = os.path.join(LOGDIR, "sink.log")

_lock = threading.Lock()
_rx = 0


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
                        break
                    authed = True
                    log("AUTH", peer, "token ok")
                    self.wfile.write(b"AUTH OK\n")
                    continue

                log("RX", peer, text)
                self.wfile.write(("OK %d\n" % bump()).encode())
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
    log("INFO", "-", "监听 0.0.0.0:%d，日志 %s" % (PORT, LOG_FILE))
    Server(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
