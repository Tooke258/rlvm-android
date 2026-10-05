# -*- coding: utf-8 -*-
"""逐位对照：同一个调用序列 + 同一个种子，分别喂给
    oracle.exe（原生 LoadLibrary 跑原版 PT00.dll）
    emu.exe    （我们自己的 x86 执行器跑同一份 DLL）
再把两边每次调用的「返回值 + intD 改动」逐项比对。

用法：
    python tools/pt00_oracle/compare.py <PT00.dll> <calls.txt> [--seed seed.bin] [--limit N]

退出码：0 = 全部一致；1 = 有差异（并打印前若干处）。
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ORACLE = os.path.join(HERE, "oracle.exe")
EMU = os.path.join(REPO, "tools", "pt00_emu", "emu.exe")


def per_call_lines(text):
    """把一次调用的多行输出折成一条：`N func=.. ret=.. intD[..]=..`。"""
    out = []
    for raw in text.splitlines():
        line = raw.strip()
        m = re.match(r"^(\d+) func=(\S+) ret=(-?\d+)(.*)$", line)
        if m:
            tail = " ".join(m.group(4).split())
            out.append("%s f=%s ret=%s%s" % (m.group(1), m.group(2), m.group(3),
                                             (" " + tail) if tail else ""))
    return out


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    return p.stdout + p.stderr


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dll, calls = sys.argv[1], sys.argv[2]
    seed = None
    limit = 0
    for i, a in enumerate(sys.argv[3:], 3):
        if a == "--seed" and i + 1 < len(sys.argv):
            seed = sys.argv[i + 1]
        elif a == "--limit" and i + 1 < len(sys.argv):
            limit = int(sys.argv[i + 1])

    seed_arg = ["--seed", seed] if seed else []
    ref = per_call_lines(run([ORACLE, dll, calls] + seed_arg))
    got = per_call_lines(run([EMU, dll, calls] + seed_arg))
    if limit:
        ref, got = ref[:limit], got[:limit]

    print("# oracle 调用数=%d  emu 调用数=%d" % (len(ref), len(got)))
    bad = 0
    for i in range(max(len(ref), len(got))):
        a = ref[i] if i < len(ref) else "(缺)"
        b = got[i] if i < len(got) else "(缺)"
        if a != b:
            bad += 1
            if bad <= 20:
                print("第 %d 处不一致：" % bad)
                print("   oracle: %s" % a)
                print("   emu   : %s" % b)
    if bad == 0:
        print("OK：两边逐位一致（%d 次调用）" % len(ref))
        return 0
    print("DIFF：共 %d 处不一致" % bad)
    return 1


sys.exit(main())
