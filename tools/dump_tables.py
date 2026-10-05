# -*- coding: utf-8 -*-
"""从 PE 里按虚拟地址导出数据（PT00.dll 的动画帧表取证用）。

PT00.dll 的动画表是「增量表 + 前缀和」：运行时用
`sub_100022D0(dst, src, count)` 把 src 的 count 个 dword 做前缀和写进 dst，
所以 `.data` 里存的是增量，运行期才变成帧阈值。

用法：
    python tools/dump_tables.py <dll> --range 0x1001F040 0x1001F210
    python tools/dump_tables.py <dll> 0x1001F128 0x1001F164 ...

只依赖标准库。每行给出 地址 / 十进制 / 十六进制，方便直接抄进 C++。
"""

import struct
import sys


def parse_pe(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    size_opt = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt = e_lfanew + 24
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    st = opt + size_opt
    sections = []
    for i in range(nsec):
        off = st + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((name, vaddr, vsize, rawptr, rawsize))
    return image_base, sections


def read_va(data, image_base, sections, va, nbytes):
    rva = va - image_base
    for _name, vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            start = rawptr + (rva - vaddr)
            return data[start:start + nbytes]
    raise KeyError("VA 不在任何段里（可能是 BSS，运行期才有值）: %s" % hex(va))


def main():
    args = sys.argv[2:]
    if not args:
        print(__doc__)
        return
    data = open(sys.argv[1], "rb").read()
    image_base, sections = parse_pe(data)

    ranges = []
    if args[0] == "--range":
        start, end = int(args[1], 16), int(args[2], 16)
        ranges.append((start, end))
    else:
        for a in args:
            va = int(a, 16)
            ranges.append((va, va + 4))

    for start, end in ranges:
        count = (end - start) // 4
        raw = read_va(data, image_base, sections, start, count * 4)
        print("== %s .. %s (%d dwords) ==" % (hex(start), hex(end), count))
        vals = struct.unpack("<%dI" % count, raw)
        for i, v in enumerate(vals):
            as_signed = struct.unpack("<i", struct.pack("<I", v))[0]
            print("  %s  %-12d  0x%08X" % (hex(start + i * 4), as_signed, v))


main()
