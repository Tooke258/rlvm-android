# -*- coding: utf-8 -*-
"""RLVM 全局存档（global.sav.gz）读写工具 —— 用来编"收集全解锁"的档。

文件格式（实测）：
  zlib 压缩的 **boost 文本归档**；内容顺序 = CURRENT_GLOBAL_VERSION,
  GlobalMemory（第一个字段就是 intG[0..1999]）, SystemGlobals, 各子系统 globals, config。
  ⇒ 只要"只改 intG 那 2000 个十进制数字、其余字节原样保留"，就不需要真去
    反序列化那些类。

用法：
  python tools/rlvm-global.py dump   <global.sav.gz> [--all]
  python tools/rlvm-global.py set    <global.sav.gz> <out.sav.gz> 1000=1 1009=128 ...
  python tools/rlvm-global.py preset <global.sav.gz> <out.sav.gz> [--bool-lo 1000 --bool-hi 1049]

  dump   ：打印 intG 的非 0 项（--all 则打印全部 2000 项）
  set    ：按 idx=value 逐项赋值（可多个）
  preset ：把 [--bool-lo, --bool-hi] 区间里"当前为 0 的"统统置 1（收集位全解锁），
           不动已有的计数/列表值 —— 这是最保守的"全解锁"做法。

注意：脚本只写 intG[1000..1042] 里的布尔解锁位；[1009]/[1021]/[1036] 这类是
      引擎累加的计数（它们只被读、不被写），`preset` 不会去猜它们的上限。
"""

import re
import sys
import zlib

INTG_COUNT = 2000


def load(path):
    raw = open(path, "rb").read()
    text = zlib.decompress(raw).decode("utf-8")
    return text


def find_intg(tokens, which=0):
    """返回第 which 个 2000 长整型数组在 token 列表中的起点（count 令牌之后）。

    实测这个 global 里有三张 2000 长的表（token 位置 6 / 2007 / 4008）：
      第 0 张 = intG（216 个非 0）
      第 1 张 = 既读/进度表（793 个非 0，含 [391]=542711623 这类位域）
      第 2 张 = 全 0（未使用 / 另一套标志）
    """
    hits = []
    for i, t in enumerate(tokens):
        if t == str(INTG_COUNT) and i + INTG_COUNT < len(tokens):
            ok = True
            for k in range(i + 1, i + 1 + INTG_COUNT):
                if not re.fullmatch(r"-?\d+", tokens[k]):
                    ok = False
                    break
            if ok:
                hits.append(i + 1)
    if which >= len(hits):
        raise SystemExit("找不到第 %d 张 2000 长数组（共发现 %d 张）" % (which, len(hits)))
    return hits[which]


def tokenize(text):
    """保留分隔符地切开：返回 (tokens, seps)；tokens[i] 后面跟 seps[i]。"""
    parts = re.split(r"(\s+)", text)
    tokens, seps = [], []
    for p in parts:
        if p == "":
            continue
        if p.isspace():
            if seps:
                seps[-1] += p
            else:
                seps.append(p)  # 文件开头的空白
        else:
            tokens.append(p)
            seps.append("")
    while len(seps) <= len(tokens):
        seps.append("")
    return tokens, seps


def rebuild(tokens, seps):
    out = []
    for i, t in enumerate(tokens):
        out.append(t)
        out.append(seps[i])
    return "".join(out)


def get_arr(tokens, off):
    return [int(tokens[off + i]) for i in range(INTG_COUNT)]


def put_arr(tokens, off, arr):
    for i, v in enumerate(arr):
        tokens[off + i] = str(int(v))


def cmd_dump(path, show_all, which=0):
    tokens, _ = tokenize(load(path))
    off = find_intg(tokens, which)
    arr = get_arr(tokens, off)
    nz = [(i, v) for i, v in enumerate(arr) if v]
    print("第 %d 张表非 0 项 %d 个：" % (which, len(nz)))
    if show_all:
        for i, v in enumerate(arr):
            print("  [%4d] = %d" % (i, v))
    else:
        for i, v in nz:
            print("  [%4d] = %d" % (i, v))


def cmd_set(path, out, pairs, which=0):
    tokens, seps = tokenize(load(path))
    off = find_intg(tokens, which)
    arr = get_arr(tokens, off)
    for p in pairs:
        idx_s, val_s = p.split("=", 1)
        idx = int(idx_s)
        if not (0 <= idx < INTG_COUNT):
            raise SystemExit("下标越界：%d" % idx)
        old = arr[idx]
        arr[idx] = int(val_s)
        print("  [%4d] %d -> %d" % (idx, old, arr[idx]))
    put_arr(tokens, off, arr)
    data = rebuild(tokens, seps).encode("utf-8")
    open(out, "wb").write(zlib.compress(data, 9))
    print("写出 %s（%d 字节，压缩前 %d）" % (out, len(open(out, "rb").read()), len(data)))


def cmd_preset(path, out, lo, hi, which=0):
    tokens, seps = tokenize(load(path))
    off = find_intg(tokens, which)
    arr = get_arr(tokens, off)
    changed = 0
    for i in range(lo, min(hi, INTG_COUNT - 1) + 1):
        if arr[i] == 0:
            arr[i] = 1
            changed += 1
    print("第 %d 张表 区间 [%d,%d]：把 %d 个 0 置成 1（已有值一律不动）" % (which, lo, hi, changed))
    put_arr(tokens, off, arr)
    data = rebuild(tokens, seps).encode("utf-8")
    open(out, "wb").write(zlib.compress(data, 9))
    print("写出 %s" % out)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mode, path = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    which = 0
    if "--array" in rest:
        which = int(rest[rest.index("--array") + 1])
    if mode == "dump":
        cmd_dump(path, "--all" in rest, which)
    elif mode == "set":
        out = rest[0]
        pairs = [x for x in rest[1:] if "=" in x]
        if not pairs:
            raise SystemExit("set 需要至少一组 idx=value")
        cmd_set(path, out, pairs, which)
    elif mode == "preset":
        out = rest[0]
        lo, hi = 1000, 1049
        if "--bool-lo" in rest:
            lo = int(rest[rest.index("--bool-lo") + 1])
        if "--bool-hi" in rest:
            hi = int(rest[rest.index("--bool-hi") + 1])
        cmd_preset(path, out, lo, hi, which)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
