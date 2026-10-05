# -*- coding: utf-8 -*-
"""在 `dump_scenes=all` 产出的全幕反汇编里按正则搜，输出「场景 + 文件行号 + 原文」。

用法：
    python tools/scene_search.py <scenes.txt> "<正则>" [--limit N] [--count]
例：
    python tools/scene_search.py build/rlvm-scenes.txt "op<2:071:01000" --limit 30
    python tools/scene_search.py build/rlvm-scenes.txt "intG\\[19" --count
"""

import re
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, pattern = sys.argv[1], sys.argv[2]
    limit = 40
    count_only = False
    for i, a in enumerate(sys.argv[3:], 3):
        if a == "--limit" and i + 1 < len(sys.argv):
            limit = int(sys.argv[i + 1])
        elif a == "--count":
            count_only = True
    rx = re.compile(pattern)
    scene = "?"
    hits = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            t = line.strip()
            if t.startswith("===== SEEN") and t.endswith("====="):
                scene = t.replace("=", "").strip()
                continue
            if rx.search(t):
                hits += 1
                if not count_only and hits <= limit:
                    print("%-10s %-9d %s" % (scene, lineno, t))
    print("共 %d 处" % hits)
    return 0


sys.exit(main())
