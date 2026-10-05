# -*- coding: utf-8 -*-
"""按调用前缀逐帧对照**状态**（intD+intF 整片），而不是 compare.py 的"这一帧改了哪些槽"。

为什么要这个：oracle 与 emu 可能把同一个值写在**不同的帧**里，逐帧差异行就会一直报噪声，
而脚本真正读到的是"每帧结束时的 intD"。所以判据应该是「第 N 帧结束时两边状态是否逐位一致」。

用法:
    python tools/pt00_oracle/state_walk.py <PT00.dll> <calls.txt> [--seed seed.bin] [--summary]

默认打印每一帧的差异明细（连续 7 帧有差异就停）；--summary 只打印汇总
（多少帧一致/有差异 + 涉及哪些槽位、各自首次出现在第几帧）。
退出码: 0 = 全部帧状态一致；1 = 有差异。
"""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ORACLE = os.path.join(HERE, "oracle.exe")
EMU = os.path.join(REPO, "tools", "pt00_emu", "emu.exe")
TMP = os.path.join(REPO, "build", "pt00-fixtures")


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    return p.returncode


def state(path):
    with open(path, "rb") as f:
        b = f.read()
    return struct.unpack("<%di" % (len(b) // 4), b)


def tag(i):
    return ("intD", i) if i < 2000 else ("intF", i - 2000)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dll, calls = sys.argv[1], sys.argv[2]
    seed = None
    summary = "--summary" in sys.argv
    for i, a in enumerate(sys.argv[3:], 3):
        if a == "--seed" and i + 1 < len(sys.argv):
            seed = sys.argv[i + 1]
            break
    with open(calls, encoding="utf-8") as f:
        lines = [l for l in f if l.strip() and not l.lstrip().startswith("#")]
    os.makedirs(TMP, exist_ok=True)
    seed_arg = ["--seed", seed] if seed else []
    first = None
    n_ok = n_bad = 0
    noisy = {}
    for n in range(1, len(lines) + 1):
        sub = os.path.join(TMP, "prefix_%d.txt" % n)
        with open(sub, "w", encoding="ascii") as f:
            f.write("".join(lines[:n]))
        ob = os.path.join(TMP, "st_oracle.bin")
        eb = os.path.join(TMP, "st_emu.bin")
        run([ORACLE, dll, sub, ob] + seed_arg)
        run([EMU, dll, sub, "--dump", eb] + seed_arg)
        sa, sb = state(ob), state(eb)
        diff = [(i, x, y) for i, (x, y) in enumerate(zip(sa, sb)) if x != y]
        if not diff:
            n_ok += 1
            if not summary:
                print("第 %d 帧（%s）：状态一致" % (n, lines[n - 1].strip()))
            continue
        n_bad += 1
        for i, _x, _y in diff:
            noisy.setdefault(i, n)
        if first is None:
            first = n
        if summary:
            continue
        print("第 %d 帧（%s）：状态差异 %d 处" % (n, lines[n - 1].strip(), len(diff)))
        for i, x, y in diff[:20]:
            t, idx = tag(i)
            print("   %s[%d] oracle=%d emu=%d" % (t, idx, x, y))
        if n >= first + 6:
            print("（连续 7 帧有差异，先停在这里；加 --summary 看整体）")
            break
    if summary:
        print("帧总数=%d  状态一致=%d  有差异=%d" % (len(lines), n_ok, n_bad))
        print("涉及槽位（首次出现的帧）：")
        for i, f in sorted(noisy.items(), key=lambda kv: (kv[1], kv[0]))[:40]:
            t, idx = tag(i)
            print("   %s[%d]  首次出现于第 %d 帧" % (t, idx, f))
    if first is None:
        print("OK：全部 %d 帧状态逐位一致" % len(lines))
        return 0
    print("DIFF：第一处状态差异在第 %d 帧" % first)
    return 1


sys.exit(main())
