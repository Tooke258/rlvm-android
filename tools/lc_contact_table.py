# -*- coding: utf-8 -*-
"""把 lc_*.txt 里的"每次触球"抽成一张表：棒角 → 几何 → 抖动 → 方向。

背景（docs/MINIGAME-HIT-DIRECTION.md §4）：
  func 50 入口 0x10003bf6 处 `fild` 出来的第一个值就是 **intD[624]（棒角，0.1° 单位）**；
  0x100040da 是 `rand()%400`（命中抖动）；
  0x1000421d 是尾部把方向分量 ×1000 写进 intD[264] 的最后一算（= 1000·sinθ）。
本脚本按 0x10003bf6 的出现把日志切成"每次调用"，把这三处串成一行。

用法：
    python tools/lc_contact_table.py build/lc_x87.txt [build/lc_tail.txt ...]
"""

import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

P_CALL = re.compile(r"\[x87\] 10003bf6 op=\S+ modrm=\S+ rc=\d+\s+st\[[^\]]*\] -> \[([-\d.eE+]+)")
P_JITT = re.compile(r"\[x87\] 100040da op=\S+ modrm=\S+ rc=\d+\s+st\[[^\]]*\] -> \[([-\d.eE+]+)")
P_DIR = re.compile(r"\[x87\] 1000421d op=\S+ modrm=\S+ rc=\d+\s+st\[[^\]]*\] -> \[([-\d.eE+]+)")
P_TS = re.compile(r"(\d\d-\d\d \d\d:\d\d:\d\d\.\d+)")


def rows(path):
    calls = []
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        ts = P_TS.search(line)
        m = P_CALL.search(line)
        if m:
            if cur:
                calls.append(cur)
            cur = {"ts": ts.group(1) if ts else "", "bat": float(m.group(1))}
            continue
        if cur is None:
            continue
        m = P_JITT.search(line)
        if m:
            cur["jitter"] = float(m.group(1))
        m = P_DIR.search(line)
        if m:
            cur["dir"] = float(m.group(1))
    if cur:
        calls.append(cur)
    return calls


for path in sys.argv[1:]:
    rs = rows(path)
    full = [r for r in rs if "dir" in r]
    print("===== %s : %d 次调用，其中 %d 次跑到尾部 =====" % (path, len(rs), len(full)))
    for r in full:
        bat = r["bat"]
        k = (bat - 85.0) / 150.0
        print("   %s  棒角[624]=%-8.0f (85+150*%.2f)  抖动=%-6.0f  方向[264]=%+.1f  %s"
              % (r["ts"], bat, k, r.get("jitter", float("nan")), r["dir"],
                 "外野" if r["dir"] > 0 else "身后"))
    if not full and rs:
        print("   （都没有跑到尾部，只有入口：棒角样本 %s）"
              % ", ".join("%.0f" % r["bat"] for r in rs[:10]))
