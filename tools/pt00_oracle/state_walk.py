# -*- coding: utf-8 -*-
"""按调用前缀逐帧对照**状态**（intD+intF 整片），而不是 compare.py 的"这一帧改了哪些槽"。

为什么要这个：oracle 与 emu 可能把同一个值写在**不同的帧**里，逐帧差异行就会一直报噪声，
而脚本真正读到的是"每帧结束时的 intD"。所以判据应该是「第 N 帧结束时两边状态是否逐位一致」。

用法:
    python tools/pt00_oracle/state_walk.py <PT00.dll> <calls.txt> [--seed seed.bin]

输出: 每一帧的差异槽数；出现差异时打印明细，并在连续差异 6 帧后停下（避免刷屏）。
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


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dll, calls = sys.argv[1], sys.argv[2]
    seed = None
    for i, a in enumerate(sys.argv[3:], 3):
        if a == "--seed" and i + 1 < len(sys.argv):
            seed = sys.argv[i + 1]
            break
    with open(calls, encoding="utf-8") as f:
        lines = [l for l in f if l.strip() and not l.lstrip().startswith("#")]
    os.makedirs(TMP, exist_ok=True)
    seed_arg = ["--seed", seed] if seed else []
    first = None
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
            print("第 %d 帧（%s）：状态一致" % (n, lines[n - 1].strip()))
            continue
        if first is None:
            first = n
        print("第 %d 帧（%s）：状态差异 %d 处" % (n, lines[n - 1].strip(), len(diff)))
        for i, x, y in diff[:20]:
            tag, idx = ("intD", i) if i < 2000 else ("intF", i - 2000)
            print("   %s[%d] oracle=%d emu=%d" % (tag, idx, x, y))
        if n >= first + 6:
            print("（连续 7 帧有差异，先停在这里）")
            break
    if first is None:
        print("OK：全部 %d 帧状态逐位一致" % len(lines))
        return 0
    print("DIFF：第一处状态差异在第 %d 帧" % first)
    return 1


sys.exit(main())
