# -*- coding: utf-8 -*-
"""生成对照夹具：intD/intF 种子快照 + 代表性调用序列。

用法（在仓库根执行）：
    python tools/pt00_oracle/make_fixtures.py

产出（路径可用 --out-dir 改，默认仓库根的 build/pt00-fixtures）：
    seed.bin           intD[2000] + intF[2000]（小端 32 位），给 oracle/emu 的 --seed
    calls_long.txt     280 次调用：8 帧真实形态（60/12/71 ×22/31/921/931/911/901）

为什么要自己造序列：设备日志里的真实调用序列会随 logcat 缓冲滚掉，而脚本每帧的
调用形态是**静态可知**的（见 docs/PT00-CALLSITES.md），配上贴近真机的 intD 种子
就能稳定复现同一条对照输入。
"""

import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    out_dir = os.path.join(REPO, "build", "pt00-fixtures")
    for i, a in enumerate(sys.argv[1:]):
        if a == "--out-dir" and i + 2 < len(sys.argv):
            out_dir = sys.argv[i + 2]
    os.makedirs(out_dir, exist_ok=True)

    d = [0] * 2000
    f = [0] * 2000
    # 接近真机「第二场（模式 10/11）」的相位：练习模式号、倒计时、场上角色与球
    d[70] = 20
    d[71] = 19
    d[72] = 15
    d[73] = 10
    d[75] = -1
    d[76] = 1
    d[1800] = 1
    d[1801] = -6000
    d[1803] = 3100
    d[1804] = 1
    d[1831] = 1
    d[1835] = 1000
    d[1837] = 2000
    d[1850] = 1
    d[1851] = 1
    d[1852] = -1000
    d[1853] = -1000
    d[1854] = 2000
    d[226] = 500
    d[228] = 2500
    d[245] = 1
    for i in range(22):
        d[1000 + i * 36] = 1  # 实体记录「有效」位

    seed_path = os.path.join(out_dir, "seed.bin")
    with open(seed_path, "wb") as fp:
        fp.write(struct.pack("<2000i", *d))
        fp.write(struct.pack("<2000i", *f))

    calls = []
    for _frame in range(8):
        for (a, b) in [(1801, 1803), (1806, 1808), (1811, 1813), (1816, 1818)]:
            calls.append("60 %d %d 0 0" % (d[a], d[b]))
            calls.append("12 %d %d %d 0" % (d[a], d[a + 1], d[b]))
        for i in range(22):
            calls.append("71 %d 0 0 0" % i)
        calls += ["31 0 0 0 0", "921 0 0 0 0", "931 0 0 0 0", "911 0 0 0 0",
                  "901 0 0 0 0"]
    calls_path = os.path.join(out_dir, "calls_long.txt")
    with open(calls_path, "w", encoding="ascii") as fp:
        fp.write("\n".join(calls) + "\n")

    print("seed   -> %s" % seed_path)
    print("calls  -> %s（%d 次）" % (calls_path, len(calls)))


main()
