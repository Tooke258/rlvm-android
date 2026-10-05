# -*- coding: utf-8 -*-
"""在 `dump_scenes=all` 产出的全幕反汇编里按正则搜，输出「场景 + 文件行号 + 原文」。

用法：
    python tools/scene_search.py <scenes.txt> "<正则>" [--limit N] [--count] [--scene SEEN7111]
                                     [--range A B]   # 直接打印文件第 A..B 行（看上下文）
例：
    python tools/scene_search.py build/rlvm-scenes.txt "op<2:071:01000" --limit 30
    python tools/scene_search.py build/rlvm-scenes.txt "intG\\[19" --count
    python tools/scene_search.py build/rlvm-scenes.txt "." --scene SEEN7111 --limit 400   # 整幕
    python tools/scene_search.py build/rlvm-scenes.txt "." --range 1248600 1248700
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
    only_scene = None
    range_from = range_to = None
    for i, a in enumerate(sys.argv[3:], 3):
        if a == "--limit" and i + 1 < len(sys.argv):
            limit = int(sys.argv[i + 1])
        elif a == "--count":
            count_only = True
        elif a == "--scene" and i + 1 < len(sys.argv):
            only_scene = sys.argv[i + 1]
        elif a == "--range" and i + 2 < len(sys.argv):
            range_from, range_to = int(sys.argv[i + 1]), int(sys.argv[i + 2])
    rx = re.compile(pattern)
    scene = "?"
    hits = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            t = line.strip()
            if t.startswith("===== SEEN") and t.endswith("====="):
                scene = t.replace("=", "").strip()
                continue
            if range_from is not None:
                if range_from <= lineno <= range_to:
                    print("%-10s %-9d %s" % (scene, lineno, t))
                continue
            if only_scene and scene != only_scene:
                continue
            if rx.search(t):
                hits += 1
                if not count_only and hits <= limit:
                    print("%-10s %-9d %s" % (scene, lineno, t))
    print("共 %d 处" % hits)
    return 0


sys.exit(main())
