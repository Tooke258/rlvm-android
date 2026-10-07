# -*- coding: utf-8 -*-
"""生成对照夹具：intD/intF 种子快照 + 代表性调用序列。

用法（在仓库根执行）：
    python tools/pt00_oracle/make_fixtures.py

产出（路径可用 --out-dir 改，默认仓库根的 build/pt00-fixtures）：
    seed.bin           intD[2000] + intF[2000]（小端 32 位），给 oracle/emu 的 --seed
    calls_long.txt     280 次调用：8 帧真实形态（60/12/71 ×22/31/921/931/911/901）
    calls_func50.txt   func 50（击球方向）对照：种子里直接放**触球帧**的输入向量
    seed_f50_*.bin     上面对应的种子（pc_hit / pc_hit2 / ph_fwd1 / ph_fwd2 / ph_late）

func 50 夹具是让"方向"这条线可**离线复现**（原本只能靠真机）：
    python tools/pt00_oracle/compare.py build/PT00.dll build/pt00-fixtures/calls_func50.txt \
        --seed build/pt00-fixtures/seed_f50_pc_hit.bin
再配 `--emu <旧版exe>` 做反向对照 —— 旧版必须报 DIFF，才说明夹具真的走到了被修的那条指令。

为什么要自己造序列：设备日志里的真实调用序列会随 logcat 缓冲滚掉，而脚本每帧的
调用形态是**静态可知**的（见 docs/PT00-CALLSITES.md），配上贴近真机的 intD 种子
就能稳定复现同一条对照输入。
"""

import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# 【触球帧】的 7 个方向输入槽（语义见 docs/MINIGAME-HIT-DIRECTION.md §10.1）：
#   [226..228] = 球当前 (x,y,z)；[229..231] = 球上一帧 (x,y,z)
#   [620..622] = box (x,y,z)；[623] = 挥棒中标志；[624] = 棒角（0.1°，15°/帧梯级）
# 取值来源：build/pc_swing.txt（PC 活动副本，"挥棒中的触球"）
#           build/phone_func50_fix.txt（修复后真机，触球帧）
F50_CASES = {
    "pc_hit":  dict(ball=(63, 269, 2113), prev=(61, 288, 2153), box=(-59, 160, 2119), bat=985),
    "pc_hit2": dict(ball=(-18, 159, 2129), prev=(-37, 178, 2158), box=(-80, 160, 2119), bat=1285),
    "ph_fwd1": dict(ball=(53, 220, 2107), prev=(51, 239, 2145), box=(-59, 160, 2097), bat=985),
    "ph_fwd2": dict(ball=(109, 220, 2064), prev=(106, 239, 2102), box=(-14, 160, 2097), bat=985),
    "ph_late": dict(ball=(-115, 220, 2149), prev=(-112, 239, 2188), box=(-158, 160, 2094), bat=1435),
}


def write_func50_fixtures(out_dir, base_d, base_f):
    """calls_func50.txt + 每个用例一个种子（其余字段沿用 calls_long 的基础种子）。"""
    for name, c in F50_CASES.items():
        seed = list(base_d)
        seed[226], seed[227], seed[228] = c["ball"]
        seed[229], seed[230], seed[231] = c["prev"]
        seed[620], seed[621], seed[622] = c["box"]
        seed[623] = 1          # 挥棒中
        seed[624] = c["bat"]   # 棒角
        with open(os.path.join(out_dir, "seed_f50_%s.bin" % name), "wb") as fp:
            fp.write(struct.pack("<2000i", *seed))
            fp.write(struct.pack("<2000i", *base_f))
    # func 50 是脚本里的 `50()` 无参调用（SEEN7420:319）；连调几次把状态往下推。
    # 两边起点相同 ⇒ 任何不一致都是执行器的锅。
    calls_path = os.path.join(out_dir, "calls_func50.txt")
    with open(calls_path, "w", encoding="ascii") as fp:
        fp.write("\n".join(["50"] * 4) + "\n")
    return calls_path


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
    f50_calls = write_func50_fixtures(out_dir, d, f)
    print("calls  -> %s（func 50 ×4）" % f50_calls)
    for name in F50_CASES:
        print("seed   -> %s" % os.path.join(out_dir, "seed_f50_%s.bin" % name))


main()
