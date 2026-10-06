# -*- coding: utf-8 -*-
"""离线列出 Seen.txt 里某个场景的「真实 entrypoint id」对照表。

为什么需要它：`dump_scenario`（`build/rlvm-scenes.txt`）打出的 `#entrypoint N`
是 **元数据里的原始值**，也就是 kidoku 表的下标；而 `farcall(场景, X)` 用的 X 是
**真实 id**，两者不是一回事：

    bytecode.cc:273   entrypoint_index_ = kidoku_table[value_] - 1000000

本脚本用同一套规则离线解析容器，省掉「改 dump → 重编 → 真机跑一轮」的代价。

用法：
    python tools/seen_entrypoints.py "<游戏目录>/Seen.txt" 7110 7111 7030
"""

import struct
import sys


def read_i32(buf, off):
    return struct.unpack_from("<i", buf, off)[0]


def describe(data, scene_num):
    off = read_i32(data, scene_num * 8)
    length = read_i32(data, scene_num * 8 + 4)
    if off == 0 or length <= 0:
        print("SEEN%-5d (空场景：TOC 里没有条目)" % scene_num)
        return
    blob = data[off:off + length]
    if len(blob) < 0x1D0:
        print("SEEN%-5d off=0x%x len=%d —— 不是字节码文件（太短）"
              % (scene_num, off, length))
        return
    kidoku_off = read_i32(blob, 0x08)
    kidoku_len = read_i32(blob, 0x0C)
    compiler = read_i32(blob, 0x04)
    print("SEEN%-5d off=0x%-8x len=%-8d compiler=%-8d kidoku: off=0x%x 条目=%d"
          % (scene_num, off, length, compiler, kidoku_off, kidoku_len))
    if kidoku_off <= 0 or kidoku_len <= 0 or kidoku_off + kidoku_len * 4 > len(blob):
        print("        kidoku 表越界，跳过")
        return
    for i in range(kidoku_len):
        value = read_i32(blob, kidoku_off + i * 4)
        if value >= 1000000:
            print("        索引 %-3d -> entrypoint id %d" % (i, value - 1000000))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    with open(argv[1], "rb") as handle:
        data = handle.read()
    for arg in argv[2:]:
        describe(data, int(arg, 0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
