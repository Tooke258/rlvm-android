# -*- coding: utf-8 -*-
"""Dump the first 64 bytes (and size) of G00 files for a quick A/B comparison.

Usage:
    python tools/g00_head.py "<a.g00>" "<b.g00>" ...

Why: when the object table looks fine (names/rects sane) but the pixels are
empty, the next question is "what does this image decode to". Comparing the
headers of an image that renders against one that does not is the cheapest
first look (type byte / compression flag / dimensions).
"""

import os
import sys


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for path in sys.argv[1:]:
        if not os.path.exists(path):
            print("=== %s MISSING ===" % path)
            continue
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            head = f.read(64)
        hexs = " ".join("%02x" % c for c in head[:32])
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in head[:32])
        print("=== %s (%d bytes) ===" % (os.path.basename(path), size))
        print("  hex: %s" % hexs)
        print("  asc: %s" % asc)
        le = " ".join("%d" % int.from_bytes(head[i:i + 4], "little")
                      for i in range(0, 16, 4))
        print("  u32le[0..3]: %s" % le)
    return 0


sys.exit(main())
