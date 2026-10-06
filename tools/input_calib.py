# -*- coding: utf-8 -*-
"""按键 / 鼠标输入标定：抓 PC 原生引擎的 intD 快照并 diff。

目的：**用真值标定编码**，而不是猜。RLVM 侧的 `Sys151/152` 是「轮询一串键码，
把被按下的那个写进给定引用」（例：`Sys151(intD[101], 5,4,49,0,1,2,3,100)`），
我们目前只实现了鼠标版，键码表全被忽略。要把它做对，必须先知道
「按 X → 哪个槽位变成几」—— 就是在原生引擎上测出来的。
参见 `docs/INPUT-KEY-RECON.md`（摸排结论）与 `docs/ENGINE-INTEGRATION-LESSONS.md` §6.1（计划）。

用法（PC 侧，游戏已进到想标定的状态，例如小游戏的循环里）：

    # 0) 先确认探针能找到进程（PC 上真实进程名是 lbex_sc.exe）
    tools\\pt00_probe.exe --list lbex

    # 1) 交互标定：每次回车抓一帧，自动 diff 上一帧
    python tools/input_calib.py --session
      提示里可以顺带打个标签，例如：  回车（空=stepN） | A按下 | A松开 | 左键按下

    # 2) 只想盯某几个槽位（即使没变化也打印）
    python tools/input_calib.py --session --watch-slots 101,109

    # 3) 存一帧 / 事后比对两帧
    python tools/input_calib.py --snap before
    python tools/input_calib.py --diff build/input-calib/before.txt build/input-calib/after.txt

本工具**只读**目标进程（内存读取全部由 pt00_probe.exe 完成），不写任何东西；
快照与日志写到 `build/input-calib/`（该目录不进仓库）。
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
    """把 `    [ 101] 0 1 -2 ...` 这种行解析成 {index: value}。"""
    values = {}
    for line in text.splitlines():
        m = ROW_RE.match(line.strip())
        if not m:
            # 「单值模式」的 intD[nn]=v 行也顺手收一下，方便 --watch-slots
            m2 = re.match(r"^intD\[(\d+)\]=(-?\d+)", line.strip())
            if m2:
                values[int(m2.group(1))] = int(m2.group(2))
            continue
        base = int(m.group(1))
        for i, tok in enumerate(m.group(2).split()):
            values[base + i] = int(tok)
    return values


def run_probe(probe, proc):
    if not os.path.exists(probe):
        raise SystemExit("找不到探针：%s（先跑 tools\\build_pt00_probe.bat）" % probe)
    cmd = [probe, proc, "--full"]
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    out = (p.stdout or "") + (p.stderr or "")
    values = parse_dump(out)
    if not values:
        sys.stderr.write(
            "!! 没解析到 intD。命令：%s\n"
            "   常见原因：进程名不对（PC 上通常是 lbex_sc.exe，不是 REALLIVE.EXE）、\n"
            "   或游戏还没进到会写 intD 的状态。可以先跑：\n"
            "     tools\\pt00_probe.exe --list lbex\n"
            "     tools\\pt00_probe.exe --scan-all\n"
            % " ".join(cmd)
        )
        sys.stderr.write("---- 探针输出（尾部 1200 字节）----\n%s\n" % out[-1200:])
    return values, out


def load_dump(path):
    with io.open(path, encoding="utf-8", errors="replace") as f:
        v = parse_dump(f.read())
    if not v:
        raise SystemExit("解析失败（文件里没有 [idx] 值行）：%s" % path)
    return v


def diff_values(a, b):
    """返回 [(idx, old, new)]，只含变化的槽位；两侧都出现过的下标才比。"""
    return [(k, a[k], b[k]) for k in sorted(set(a) & set(b)) if a[k] != b[k]]


def fmt_diff(changes, watch=()):
    out = []
    if changes:
        for k, old, new in changes:
            out.append("    intD[%d]: %d -> %d" % (k, old, new))
    else:
        out.append("    （intD 无变化）")
    if watch:
        out.append("    关注槽位：" + "  ".join(
            "intD[%d]=%s" % (k, "?" if k not in watch else watch[k]) for k in sorted(watch)))
    return "\n".join(out)


def save_snapshot(label, values, raw):
    os.makedirs(OUTDIR, exist_ok=True)
    safe = re.sub(r"[^0-9A-Za-z_.-]+", "_", label) or "snap"
    path = os.path.join(OUTDIR, safe + ".txt")
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# label=%s time=%s\n" % (label, time.strftime("%Y-%m-%d %H:%M:%S")))
        f.write("# parsed %d values\n" % len(values))
        for k in sorted(values):
            f.write("intD[%d]=%d\n" % (k, values[k]))
    return path


def cmd_snap(args):
    values, raw = run_probe(args.probe, args.proc)
    if not values:
        return 1
    label = args.snap or ("snap-" + time.strftime("%H%M%S"))
    path = save_snapshot(label, values, raw)
    print("已保存 %s（%d 个槽位）" % (path, len(values)))
    return 0


def cmd_diff(args):
    a, b = load_dump(args.diff[0]), load_dump(args.diff[1])
    changes = diff_values(a, b)
    print("# %s -> %s" % (args.diff[0], args.diff[1]))
    print("# 两边共有 %d 个槽位，变化 %d 个" % (len(set(a) & set(b)), len(changes)))
    print(fmt_diff(changes))
    return 0


def cmd_session(args):
    watch_ids = [int(x) for x in args.watch_slots.split(",") if x.strip()] if args.watch_slots else []
    steps = []   # [(label, values)]
    print(__doc__.strip().splitlines()[0])
    print("提示：每一步先在游戏里**按住/按下**要标定的键，再回车抓取；空标签 = stepN。")
    print("      输入 q 结束并出汇总表。目标进程：%s" % args.proc)
    prev = None
    while True:
        n = len(steps)
        try:
            label = input("步骤 %d（回车抓取 / 输入标签 / q 退出）: " % n).strip()
        except EOFError:
            break
        if label.lower() in ("q", "quit", "exit"):
            break
        if not label:
            label = "step%d" % n
        values, raw = run_probe(args.probe, args.proc)
        if not values:
            print("  （本步抓取失败，重试或换进程名）")
            continue
        steps.append((label, values))
        path = save_snapshot("%02d-%s" % (n, label), values, raw)
        shown = {k: values.get(k) for k in watch_ids}
        if prev is None:
            print("  基线已记录（%d 个槽位）→ %s" % (len(values), path))
            if watch_ids:
                print("    " + "  ".join("intD[%d]=%s" % (k, shown.get(k)) for k in watch_ids))
        else:
            changes = diff_values(prev, values)
            print("  与上一帧相比：")
            print(fmt_diff(changes, shown))
            print("  → %s" % path)
        prev = values

    if len(steps) >= 2:
        print("\n=== 汇总：所有变化过的槽位 ===")
        touched = sorted({k for _, v in steps for k in diff_values(steps[0][1], v)})
        if not touched:
            print("（整段会话里 intD 没有任何槽位变化 —— 说明这个状态下引擎不写输入槽，"
                  "换到小游戏循环里再试）")
        for k in touched:
            row = "  intD[%4d]:" % k
            for label, v in steps:
                row += "  %s=%s" % (label, v.get(k, "?"))
            print(row)
        print("\n判读：出现 1 的步 = 「这个输入被引擎看见了」；把 (键, 槽位, 值) 三个一起记下来，"
              "就是 Sys151/152 键码表要用的真值。")
    return 0


def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--proc", default="lbex_sc.exe", help="PC 引擎进程名或 PID（默认 lbex_sc.exe）")
    ap.add_argument("--probe", default=PROBE, help="pt00_probe.exe 路径")
    ap.add_argument("--snap", nargs="?", const="", help="抓一帧存盘（可带标签）")
    ap.add_argument("--diff", nargs=2, metavar=("A", "B"), help="比对两份已保存的快照")
    ap.add_argument("--session", action="store_true", help="交互式标定（推荐）")
    ap.add_argument("--watch-slots", default="", help="额外打印这些槽位，如 101,109")
    args = ap.parse_args()

    if args.diff:
        return cmd_diff(args)
    if args.session:
        return cmd_session(args)
    if args.snap is not None:
        return cmd_snap(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
