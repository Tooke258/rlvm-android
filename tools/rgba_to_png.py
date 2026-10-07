# -*- coding: utf-8 -*-
"""把 `g00_decode_probe --raw` 导出的 RGBA8888 原始图转成 PNG（可选整数放大）。

用法：
    python tools/rgba_to_png.py <in.rgba> <out.png> <width> <height> [scale]

背景：判断「脚本给的图案号该映射到哪一格」时，最快的办法是把素材本身看一眼——
比继续读反汇编快得多（见 docs/MINIGAME-UI-PRESENTATION.md）。
"""

import struct
import sys
import zlib


def write_png(path, w, h, rgba):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main(argv):
    src, dst, w, h = argv[0], argv[1], int(argv[2]), int(argv[3])
    scale = int(argv[4]) if len(argv) > 4 else 1
    data = open(src, "rb").read()
    if len(data) != w * h * 4:
        print("size mismatch: %d != %d" % (len(data), w * h * 4))
        return 1
    if scale != 1:
        out = bytearray()
        for y in range(h):
            row = bytearray()
            for x in range(w):
                px = data[(y * w + x) * 4:(y * w + x) * 4 + 4]
                row += px * scale
            for _ in range(scale):
                out += row
        data, w, h = bytes(out), w * scale, h * scale
    write_png(dst, w, h, data)
    print("wrote %s (%dx%d)" % (dst, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
