# -*- coding: utf-8 -*-
"""From an RLVM instruction trace, find the loop the script is stuck in.

Usage:
    python tools/trace_loop.py <logfile> [--tail 20000] [--top 15] [--show 40]

Every trace line looks like `(SEEN####)(Line NNNN):  <instruction>`; counting the
(scene, line) pairs in the *tail* of the log shows exactly which few instructions
the VM is bouncing between.
"""

import collections
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

RX = re.compile(r"\((SEEN\d+)\)\(Line (\d+)\):\s*(.*)$")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    tail = 20000
    top = 15
    show = 40
    for i, a in enumerate(sys.argv[2:], 2):
        if a == "--tail" and i + 1 < len(sys.argv):
            tail = int(sys.argv[i + 1])
        elif a == "--top" and i + 1 < len(sys.argv):
            top = int(sys.argv[i + 1])
        elif a == "--show" and i + 1 < len(sys.argv):
            show = int(sys.argv[i + 1])

    lines = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RX.search(line)
            if m:
                lines.append((m.group(1), int(m.group(2)), m.group(3).strip()))
    print("# 解析到 %d 条带行号的指令" % len(lines))
    if not lines:
        return 1

    tail_lines = lines[-tail:]
    print("\n## 尾部 %d 条里出现最多的 (场景, 行)：卡住的循环就是这几条" % len(tail_lines))
    cnt = collections.Counter((s, l) for s, l, _ in tail_lines)
    for (s, l), n in cnt.most_common(top):
        text = next((t for ss, ll, t in reversed(tail_lines) if ss == s and ll == l), "")
        print("  %6d 次  %s:%d  %s" % (n, s, l, text[:110]))

    print("\n## 日志最后 %d 条（当前执行位置）" % show)
    for s, l, t in lines[-show:]:
        print("  %s:%d  %s" % (s, l, t[:110]))
    return 0


sys.exit(main())
