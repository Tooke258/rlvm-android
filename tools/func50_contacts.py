# -*- coding: utf-8 -*-
"""把「手机 peek 日志」和「PC 采样器输出」统一抽成 func 50 的触球表。

背景（docs/MINIGAME-HIT-DIRECTION.md §9）：
  func 50（0x10003BB0）算「击球方向」时只读 7 个 intD 槽 —— 见
  tools/rlvm-diag.peek-func50.txt 的注释；输出是 intD[264]（= 1000·sin(总角)）。
  方向 = (cos 总角, sin 总角)，坐标系里 +z 朝投手/外野、−z 朝击球手身后
  ⇒ **[264] > 0 = 飞外野；[264] < 0 = 飞身后**。

用法：
    python tools/func50_contacts.py build/phone_func50.txt [build/pc_swing.txt ...]
    # 只想要所有行（不筛触球）：加 --all

输入格式自动识别：
  手机：`[pt00] peek 104009c0=835(0x...) 104009b0=...`（地址 → 槽 = (addr-0x10400000)/4）
  PC  ：`tools/pt00_fast.exe` 的 46 列（t + 226..231 | 250..262 | 263..277 | 610..613 | 620..626）

触球判据：
  手机：相邻两行的 [264] 变了（peek 在 CallDLL 入口打，所以新 [264] 出现在下一行）
  PC  ：同上，但额外丢掉 [624] == -215 的变化（那是球落地/野手回传，不是挥棒触球）
"""

import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

SLOT_226, SLOT_264, SLOT_620, SLOT_624 = 226, 264, 620, 624


def norm(dx, dz):
    n = (dx * dx + dz * dz) ** 0.5
    return (0.0, 0.0) if n == 0 else (dx / n, dz / n)


def row_from_slots(s):
    """把「槽 → 值」转成我们有话可说的元组；缺的槽按 None。"""
    ball = (s.get(226), s.get(228))
    prev = (s.get(229), s.get(231))
    box = (s.get(620), s.get(622))
    out = s.get(264)
    bat = s.get(624)
    d_in = None
    if None not in ball and None not in prev:
        d_in = (ball[0] - prev[0], ball[1] - prev[1])
    d_box = None
    if None not in ball and None not in box:
        d_box = (ball[0] - box[0], ball[1] - box[1])
    return {
        "ball": ball,
        "prev": prev,
        "box": box,
        "bat": bat,
        "out": out,
        "din": d_in,
        "dbox": d_box,
        "raw": s,
    }


P_PEEK = re.compile(r"(\d\d-\d\d \d\d:\d\d:\d\d\.\d+)")
P_ADDR = re.compile(r"\b(104[0-9a-fA-F]{5})=(-?\d+)")


def parse_phone(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if "peek" not in line:
            continue
        pairs = P_ADDR.findall(line)
        if not pairs:
            continue
        s = {}
        for addr, val in pairs:
            a = int(addr, 16)
            if a < 0x10400000 or (a - 0x10400000) % 4:
                continue
            s[(a - 0x10400000) // 4] = int(val)
        ts = P_PEEK.search(line)
        rows.append((ts.group(1) if ts else "", row_from_slots(s)))
    return rows


def parse_pc(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        p = [x for x in line.split() if x != "|"]
        if len(p) != 46:
            continue
        try:
            v = [int(x) for x in p[1:]]
        except ValueError:
            continue
        s = {}
        for i, slot in enumerate(range(226, 232)):
            s[slot] = v[i]
        for i, slot in enumerate(range(250, 263)):
            s[slot] = v[6 + i]
        for i, slot in enumerate(range(263, 278)):
            s[slot] = v[19 + i]
        for i, slot in enumerate(range(610, 614)):
            s[slot] = v[34 + i]
        for i, slot in enumerate(range(620, 627)):
            s[slot] = v[38 + i]
        rows.append((p[0], row_from_slots(s)))
    return rows


def parse(path):
    # 注意：手机侧是整机 logcat 落盘（前面几千行是 SurfaceFlinger 等噪音），
    # 所以必须扫全文，不能只看开头 4KB。
    text = open(path, encoding="utf-8", errors="replace").read()
    if "peek" in text:
        return parse_phone(path), "phone"
    return parse_pc(path), "pc"


def fmt(r):
    def pair(p):
        return "(%7s,%7s)" % tuple("None" if x is None else "%d" % x for x in p)

    din = "-"
    if r["din"]:
        u = norm(*r["din"])
        din = "(%+6d,%+6d)=%+.3f,%+.3f" % (r["din"][0], r["din"][1], u[0], u[1])
    db = "-"
    if r["dbox"]:
        db = "(%+6d,%+6d)" % r["dbox"]
    return "bat=%6s ball=%s prev=%s box=%s dbox=%s  入球速度=%s  → [264]=%6s" % (
        r["bat"], pair(r["ball"]), pair(r["prev"]), pair(r["box"]), db, din, r["out"])


def main(argv):
    show_all = "--all" in argv
    files = [a for a in argv if not a.startswith("--")]
    if not files:
        print(__doc__)
        return 2
    for path in files:
        rows, kind = parse(path)
        print("===== %s (%s, %d 行) =====" % (path, kind, len(rows)))
        prev = None
        n = 0
        for tag, r in rows:
            hit = False
            if prev is not None and r["out"] != prev:
                hit = kind != "pc" or r["bat"] != -215
            if show_all or hit:
                if hit:
                    n += 1
                print("  %s %s %s" % (tag, "★触球" if hit else "  ·  ", fmt(r)))
            prev = r["out"]
        print("  → 触球事件 %d 次" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
