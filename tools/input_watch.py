# -*- coding: utf-8 -*-
"""连续采样 PC 原生引擎的 intD，并只打印「相对上一次采样发生变化」的槽位。

用途：按键标定的"自走版"——不用每按一次回车，只要按固定节奏按键，
事后按时间戳对齐「哪一段握持 = 哪个键」。

用法（PC 侧，游戏停在**小游戏**里；探针需要 intD[70..76] 的签名）：

    python tools/input_watch.py --seconds 180
    python tools/input_watch.py --slots 95-115 --seconds 120

注意：本工具**只读**目标进程内存（经 tools\\pt00_probe.exe），不写任何东西。
每次采样会整片扫一遍目标（约 0.3~0.4s），因此 interval 取 >=0.5s 比较稳。
"""

import argparse
import io
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PROBE = os.path.join(HERE, "pt00_probe.exe")
OUTDIR = os.path.join(REPO, "build", "input-calib")

ROW_RE = re.compile(r"^\s*\[\s*(\d+)\]\s*((?:-?\d+\s*)+)$")


def parse_dump(text):
    values = {}
    for line in text.splitlines():
        m = ROW_RE.match(line.strip())
        if not m:
            m2 = re.match(r"^intD\[(\d+)\]=(-?\d+)", line.strip())
            if m2:
                values[int(m2.group(1))] = int(m2.group(2))
            continue
        base = int(m.group(1))
        for i, tok in enumerate(m.group(2).split()):
            values[base + i] = int(tok)
    return values


def parse_slots(spec):
    """'0-200,700-780' -> set()；空 = 全部。"""
    if not spec:
        return None
    out = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            out.update(range(int(a), int(b) + 1))
        else:
            out.add(int(part))
    return out


def sample(proc, probe, anchor=None):
    # 签名模式（默认）要求画面停在 intD[70..76]=20,19,15,10,0,-1,1；换屏后
    # 签名会失效，因此支持 --anchor <值>：探针把该值当成 intD[73]，
    # 再用 neighbors/sane 打分挑出真正的 intD 块（实测 sane 2000/2000 可锁定）。
    cmd = [probe, proc, "--full"]
    if anchor is not None:
        cmd = [probe, proc, str(anchor), "6", "--full"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           errors="replace", timeout=20)
    except subprocess.TimeoutExpired:
        return None, "timeout", None
    out = (p.stdout or "") + (p.stderr or "")
    v = parse_dump(out)
    if not v:
        tail = out.strip().splitlines()[-1] if out.strip() else "(no output)"
        return None, tail, None
    base = None
    m = re.search(r"intD base = (0x[0-9a-fA-F]+)", out)
    if m:
        base = m.group(1)
    return v, out, base


def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--proc", default="lbex_sc.exe")
    ap.add_argument("--probe", default=PROBE)
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--slots", default="0-200,700-790,1800-1930",
                    help="只报告这些槽位的变化（空 = 全部）")
    ap.add_argument("--all", action="store_true", help="忽略 --slots，报告全部变化")
    ap.add_argument("--anchor", type=int, default=None,
                    help="单值锚定（当成 intD[73]）；签名模式失效时用，例如 --anchor 10")
    ap.add_argument("--baseline", type=float, default=0.0,
                    help="先空采 N 秒，把这期间会动的槽位当'背景噪声'排除掉；"
                         "之后只报告**不在噪声集里**的变化（按键引起的那些）")
    ap.add_argument("--show-noise", action="store_true",
                    help="基线结束后仍打印噪声槽位的变化（默认只打 ★新变化★）")
    args = ap.parse_args()

    slots = None if args.all else parse_slots(args.slots)
    last_base = [None]
    t0 = time.time()
    prev = None
    n_ok = 0
    n_fail = 0
    # 沙箱只读时可能建不了目录/文件：这时退化成"只打印"，不影响取样本身。
    log = None
    log_path = "(未落盘)"
    try:
        os.makedirs(OUTDIR, exist_ok=True)
        log_path = os.path.join(OUTDIR, "watch-%s.txt" % time.strftime("%H%M%S"))
        log = io.open(log_path, "w", encoding="utf-8", newline="\n")
    except OSError as e:
        print("# 注意：写不了明细文件（%s），只在终端打印。" % e)

    def emit(line):
        print(line)
        if log is not None:
            log.write(line + "\n")
            log.flush()   # 立即落盘：上一轮进程被中断时，缓冲区没写出去，明细文件是空的

    # 心跳：每 ~10s 报一次「这一窗口里有多少槽位在动」。
    # 用途：区分「锁到了真正的活块」和「锁到了影子/已归零的副本」——
    # 后者会一直是 0，按键也不会有任何变化（本轮就踩过这个坑）。
    n_changed_in_window = 0
    next_heartbeat = 10.0
    # 背景噪声：baseline 阶段里动过的槽位（球/相机/计时器…），之后不再逐条刷。
    noise = set()
    baseline_done = (args.baseline <= 0.0)
    if not baseline_done:
        print("# 先空采 %gs 做噪声基线：这期间请**不要按键**" % args.baseline)
    print("# 目标=%s  探针=%s" % (args.proc, args.probe))
    print("# 采样 %g 秒 / 间隔 %gs / 只报告 %s 的变化（--all 可关）"
          % (args.seconds, args.interval,
             "全部" if slots is None else args.slots))
    print("# 明细同时写 %s" % log_path)

    while True:
        el = time.time() - t0
        if el >= args.seconds:
            break
        t_sample = time.time()
        values, raw, base = sample(args.proc, args.probe, args.anchor)
        if values is None:
            n_fail += 1
            if n_fail <= 3 or n_fail % 20 == 0:
                line = "[%6.1fs] (未锁定：%s)" % (el, raw[:110])
                emit(line)
            time.sleep(max(0.2, args.interval - (time.time() - t_sample)))
            continue
        n_ok += 1
        if base != last_base[0]:
            last_base[0] = base
            line = "[%6.1fs] intD 基址 = %s" % (el, base)
            emit(line)
        if prev is None:
            keep = {k: v for k, v in values.items()
                    if slots is None or k in slots}
            line = "[%6.1fs] 已锁定：抓到 %d 个槽位（关注域 %d 个）" % (
                el, len(values), len(keep))
            emit(line)
        else:
            changes = []
            for k in sorted(set(prev) & set(values)):
                if prev[k] != values[k] and (slots is None or k in slots):
                    changes.append((k, prev[k], values[k]))
            if changes:
                if not baseline_done:
                    noise.update(k for k, _a, _b in changes)
                if not baseline_done or args.show_noise:
                    line = "[%6.1fs] " % el + "  ".join(
                        "intD[%d]:%d->%d" % (k, a, b) for k, a, b in changes)
                    emit(line)
                n_changed_in_window += len(changes)
        prev = values
        time.sleep(max(0.0, args.interval - (time.time() - t_sample)))
        if not baseline_done and el >= args.baseline:
            baseline_done = True
            emit("[%6.1fs] 基线结束：%d 个槽位属于背景噪声，之后只报告**新出现**的变化"
                 "（现在可以开始按键了）" % (el, len(noise)))
        if baseline_done and changes:
            fresh = [(k, a, b) for k, a, b in changes if k not in noise]
            if fresh:
                emit("[%6.1fs] ★新变化★ " % el + "  ".join(
                    "intD[%d]:%d->%d" % (k, a, b) for k, a, b in fresh))
        if el >= next_heartbeat:
            emit("[%6.1fs] 心跳：最近 ~10s 有 %d 次槽位变化%s" % (
                el, n_changed_in_window,
                "" if n_changed_in_window else "（全静默 —— 可能没在活块上，或者游戏没在跑）"))
            n_changed_in_window = 0
            next_heartbeat = el + 10.0

    print("# 结束：成功采样 %d 次，未锁定 %d 次，明细 %s" % (n_ok, n_fail, log_path))
    if log is not None:
        log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
