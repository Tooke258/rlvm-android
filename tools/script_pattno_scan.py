# -*- coding: utf-8 -*-
"""扫脚本转储，统计 `objPattNo`（op 081:01039）的实参，并按"最近的 objOfFile 文件名"归类。

用途：判断 `k*5` 这种"图案号是 5 的倍数"是某个文件的专有约定，还是全局写法。
    python tools/script_pattno_scan.py build/rlvm-scenes.txt
"""

import collections
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

P_PATT = re.compile(r"081:01039, \d+>\((.*)\)\s*$")
P_FILE = re.compile(r'01000, \d+>\(.*?"([^"]+)"')


def main(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    per_file = collections.defaultdict(collections.Counter)
    plain = collections.Counter()
    for i, line in enumerate(lines):
        m = P_PATT.search(line)
        if not m:
            continue
        arg = m.group(1).split(",")[-1].strip()
        owner = None
        # 回看窗口要够大：SEEN7800 里 switch 的分支在 L132..149 写图案号，
        # 而 objOfFile 在 L160 才出现（相距 ~28 行）。
        for j in range(i, max(-1, i - 40), -1):
            fm = P_FILE.search(lines[j])
            if fm:
                owner = fm.group(1)
                break
        per_file[owner][arg] += 1
        if "*" not in arg:
            plain[arg] += 1

    print("== 完全不成 5 倍的普通实参（前 15）==")
    for k, v in plain.most_common(15):
        print("  x%-5d %s" % (v, k))
    print()
    print("== 用了 `* 5` 的文件（按出现次数）==")
    rows = []
    for owner, c in per_file.items():
        n5 = sum(v for k, v in c.items() if "* 5" in k)
        if n5:
            rows.append((n5, owner, sorted(c.items())))
    for n5, owner, args in sorted(rows, reverse=True):
        print("  x%-4d %-22s args=%s" % (n5, owner, args[:8]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/rlvm-scenes.txt"))
