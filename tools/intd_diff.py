# -*- coding: utf-8 -*-
"""Diff two full intD dumps (Android "[pt00] full intD..." log lines vs PC probe --full).

Usage:
    python tools/intd_diff.py <android.log> <pc_dump.txt> [--limit 60]

Why: the minigame's camera / phase / flags all live in intD; when one side renders
nothing, the first question is "which slots differ".
"""

import re
import sys


def parse_android(path):
    """Read "[  16] v v v ..." rows out of a logcat/stderr log file."""
    values = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"\[\s*(\d+)\]\s*((?:-?\d+\s*)+)$", line.strip())
            if not m:
                continue
            base = int(m.group(1))
            for i, tok in enumerate(m.group(2).split()):
                values[base + i] = int(tok)
    return values


def parse_pc(path):
    """Same shape, but the PC probe prints '    [   0] 19 23 ...' lines too."""
    return parse_android(path)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    limit = 60
    for i, a in enumerate(sys.argv[3:], 3):
        if a == "--limit" and i + 1 < len(sys.argv):
            limit = int(sys.argv[i + 1])
    a = parse_android(sys.argv[1])
    p = parse_pc(sys.argv[2])
    print("# android parsed %d values, pc parsed %d values" % (len(a), len(p)))
    if not a or not p:
        print("# parse failed - check the dumps")
        return 1
    keys = sorted(set(a) & set(p))
    diffs = [(k, a[k], p[k]) for k in keys if a[k] != p[k]]
    print("# common indices %d, differing %d" % (len(keys), len(diffs)))
    for k, av, pv in diffs[:limit]:
        print("  intD[%d]: android=%d  pc=%d" % (k, av, pv))
    if len(diffs) > limit:
        print("  ... (%d more)" % (len(diffs) - limit))
    return 0


sys.exit(main())
