# -*- coding: utf-8 -*-
"""Sys 模块（modtype 1 / module 4）覆盖度审计。

回答一个问题：**脚本实际用到的 Sys opcode 里，哪些是 RLVM 已经实现、哪些是我们平台层
补的桩、哪些两边都没有**（后者就是真机日志里的 `Undefined: opcode<1:4:NNN, K>`，
会被引擎跳过 —— 可能影响剧情/表现）。

用法：
    python tools/sys_opcode_coverage.py
    python tools/sys_opcode_coverage.py --min-count 20    # 只看用得多于 20 次的

输入（都是仓库内的现成文件，不需要真机 / PC）：
  * build/rlvm-scenes.txt                    全幕反汇编（351 幕 / 1000+ 段）
  * rlvm-release-0.14/src/modules/module_sys*.cc   上游实现的 Sys opcode
  * app/src/main/cpp/native-bridge.cpp       我们平台层给 module(1,4) 补的 opcode
"""

import argparse
import io
import os
import re
import sys
from collections import Counter

# Windows 控制台默认 CP936，直接 print 中文会乱码/抛错；和 scene_search 一样强制 UTF-8。
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SCENES = os.path.join(REPO, "build", "rlvm-scenes.txt")
MODDIR = os.path.join(REPO, "rlvm-release-0.14", "src", "modules")
BRIDGE = os.path.join(REPO, "app", "src", "main", "cpp", "native-bridge.cpp")

USE_RE = re.compile(r"op<1:004:(\d{5}), (\d)>")
UPSTREAM_RE = re.compile(r"Add(?:Unsupported)?Opcode\(\s*(\d+)\s*,\s*(\d+)\s*,")
# 平台层：module->AddOpcode(<opcode>, <overload>, ...)（都在 module_number()==4 分支里）
PLATFORM_RE = re.compile(r"module->AddOpcode\(\s*(\d+)\s*,\s*(\d+)\s*,")


def read(path):
    with io.open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def script_usage():
    counts = Counter()
    for m in USE_RE.finditer(read(SCENES)):
        counts[(int(m.group(1)), int(m.group(2)))] += 1
    return counts


def upstream_ops():
    ops = set()
    for name in sorted(os.listdir(MODDIR)):
        if not (name.startswith("module_sys") and name.endswith(".cc")):
            continue
        for m in UPSTREAM_RE.finditer(read(os.path.join(MODDIR, name))):
            ops.add((int(m.group(1)), int(m.group(2))))
    return ops


def platform_ops():
    ops = set()
    for m in PLATFORM_RE.finditer(read(BRIDGE)):
        ops.add((int(m.group(1)), int(m.group(2))))
    return ops


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--min-count", type=int, default=1)
    args = ap.parse_args()

    used = script_usage()
    up = upstream_ops()
    plat = platform_ops()
    print("# 脚本用到的 Sys (opcode, overload) 组合：%d 种，累计 %d 次"
          % (len(used), sum(used.values())))
    print("# 上游 module_sys*.cc 注册：%d 组；平台层补的：%d 组" % (len(up), len(plat)))

    missing = [(k, c) for k, c in used.items() if k not in up and k not in plat]
    missing.sort(key=lambda kv: -kv[1])
    plat_used = [(k, c) for k, c in used.items() if k in plat and k not in up]
    plat_used.sort(key=lambda kv: -kv[1])

    print("\n## 平台层补的桩（脚本在用）—— 其中标 TODO 的是「空实现」：")
    for (op, ov), c in plat_used:
        if c < args.min_count:
            continue
        print("  Sys %-5d ov=%d  用了 %6d 次" % (op, ov, c))

    print("\n## 两边都没有（引擎会打 Undefined 并跳过）：")
    if not missing:
        print("  （无）")
    for (op, ov), c in missing:
        if c < args.min_count:
            continue
        print("  Sys %-5d ov=%d  用了 %6d 次" % (op, ov, c))

    top = [(k, c) for k, c in missing if c >= 100]
    print("\n# 其中使用 >=100 次的：%d 种" % len(top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
