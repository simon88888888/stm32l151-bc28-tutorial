# -*- coding: utf-8 -*-
"""把已有的纯文本 sink.log 回填进 SQLite —— 看板一上线就有历史曲线，不用干等。

幂等：只插**早于库里最早那条**的记录，重复跑不会插重。
"""
import os
import re
import sqlite3
import sys
from datetime import datetime, timedelta, timezone

TZ = timezone(timedelta(hours=8))
LOG = "/opt/nbiot-sink/logs/sink.log"
DB = "/opt/nbiot-sink/data/history.db"

RE_LINE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) \+08\] (\S+)\s+(\S+)\s\s(.*)$")
RE_SEQ = re.compile(r"#(\d+)")
RE_KV = re.compile(r"([A-Z]+)=(-?\d+(?:\.\d+)?)")

if not os.path.exists(DB):
    sys.exit("库还没建起来: %s（sink 重启过吗？）" % DB)

c = sqlite3.connect(DB, timeout=10)
with c:
    c.execute("PRAGMA journal_mode=WAL")

cut = c.execute("SELECT MIN(epoch) FROM rx").fetchone()[0]
print("库里最早一条 epoch = %s" % cut)

rx = conns = skipped = 0
with open(LOG, encoding="utf-8", errors="replace") as fh:
    for line in fh:
        m = RE_LINE.match(line.rstrip("\n"))
        if not m:
            continue
        ts, kind, peer, text = m.groups()
        ep = datetime.strptime(ts, "%Y-%m-%d %H:%M:%S").replace(tzinfo=TZ).timestamp()
        if cut is not None and ep >= cut:
            skipped += 1
            continue
        if kind == "RX":
            kv = dict(RE_KV.findall(text))
            sq = RE_SEQ.search(text)

            def num(k, cast):
                try:
                    return cast(kv[k])
                except (KeyError, TypeError, ValueError):
                    return None

            with c:
                c.execute("INSERT INTO rx(ts, epoch, peer, seq, csq, rsrp, snr, ecl,"
                          " up, temp, raw) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                          (ts, ep, peer, int(sq.group(1)) if sq else None,
                           num("CSQ", int), num("RSRP", float), num("SNR", float),
                           num("ECL", int), num("UP", int), num("T", float), text[:200]))
            rx += 1
        elif kind in ("AUTH", "REJECT"):
            with c:
                c.execute("INSERT INTO conn(ts, epoch, peer, kind) VALUES(?,?,?,?)",
                          (ts, ep, peer, kind))
            conns += 1

print("回填 rx %d 条 / conn %d 条（跳过已存在的 %d 行）" % (rx, conns, skipped))
print("库内合计 rx=%d conn=%d" % (c.execute("SELECT COUNT(*) FROM rx").fetchone()[0],
                                  c.execute("SELECT COUNT(*) FROM conn").fetchone()[0]))
row = c.execute("SELECT ts, temp, up, csq, rsrp FROM rx ORDER BY id LIMIT 1").fetchone()
print("最早: %s" % (row,))
row = c.execute("SELECT ts, temp, up, csq, rsrp FROM rx ORDER BY id DESC LIMIT 1").fetchone()
print("最新: %s" % (row,))
c.close()
