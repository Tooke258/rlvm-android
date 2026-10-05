# -*- coding: utf-8 -*-
"""从 PE 文件里把指定虚拟地址处的 vtable 读出来（PT00.dll 取证用）。

用法：
    python tools/dump_vtables.py <dll 路径> 0x1001D2A8 0x1001D284 ...

只需标准库。会打印 image_base、每个 vtable 的前 12 个指针。
配合 IDA dump 的函数清单（0x100xxxxx 对函数名）就能知道每个虚槽是谁。
"""

import struct
import sys


def parse_sections(data):
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
    raise KeyError("VA not mapped: %s" % hex(va))


def main():
    data = open(sys.argv[1], "rb").read()
    image_base, sections = parse_sections(data)
    print("image_base = %s" % hex(image_base))
    for name, vaddr, vsize, rawptr, rawsize in sections:
        print("  section %-8s va=%s vsize=%d raw=%s size=%d" %
              (name, hex(vaddr), vsize, hex(rawptr), rawsize))
    for arg in sys.argv[2:]:
        va = int(arg, 16)
        raw = read_va(data, image_base, sections, va, 12 * 4)
        vals = struct.unpack("<12I", raw)
        print("vtable @ %s :" % hex(va))
        for i, v in enumerate(vals):
            tag = "  <- code" if image_base <= v < image_base + 0x20000 else ""
            print("   [+%02X] idx%d = %s%s" % (i * 4, i, hex(v), tag))


main()
