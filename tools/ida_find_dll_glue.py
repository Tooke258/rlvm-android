# -*- coding: utf-8 -*-
"""在原生引擎（REALLIVE.EXE）里定位「小游戏 DLL 胶水」。

思路：引擎里必然有对这四个导出名做 GetProcAddress 的代码（真值字符串已在文件里
确认存在），以及读 Gameexe `#DLL.NNN` 的代码。把引用它们的函数、以及这些函数的
调用者一起列出来，并导出 Hex-Rays 伪码，就能看清原引擎是怎么驱动小游戏 DLL 的
（每帧调哪个 func、是否写 intG[1900]/[1901] 之类的相位标志）。

用法（批处理，别让 IDA 往游戏目录写 .id0/.idb，先复制到临时目录）：
    idat.exe -A -L<log> -S"<repo>\tools\ida_find_dll_glue.py <out.txt>" "<tmp>\REALLIVE.EXE"
"""

import sys

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import idc
import idautils

TARGETS = [
    "reallive_dll_func_load",
    "reallive_dll_func_init",
    "reallive_dll_func_free",
    "reallive_dll_func_call",
    "#DLL.",
    "^#DLL.",
]


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "dll_glue.txt"
    ida_auto.auto_wait()
    out = open(out_path, "w", encoding="utf-8")

    found = {}
    for s in idautils.Strings():
        text = str(s)
        for t in TARGETS:
            if t in text:
                found.setdefault(t, []).append((s.ea, text))

    out.write("== 命中的字符串 ==\n")
    for t, items in found.items():
        for ea, text in items[:4]:
            out.write("  %-28s @%08x  %r\n" % (t, ea, text))

    funcs = set()
    out.write("\n== 引用这些字符串的函数 ==\n")
    for t, items in found.items():
        for ea, text in items:
            for xref in idautils.DataRefsTo(ea):
                f = ida_funcs.get_func(xref)
                if not f:
                    continue
                funcs.add(f.start_ea)
                out.write("  %-28s <- %08x (%s)\n" % (t, f.start_ea,
                                                       idc.get_func_name(f.start_ea)))

    out.write("\n== 这些函数的调用者 ==\n")
    callers = set()
    for fea in sorted(funcs):
        for xref in idautils.CodeRefsTo(fea, 0):
            cf = ida_funcs.get_func(xref)
            if cf:
                callers.add(cf.start_ea)
                out.write("  %08x (%s) -> %08x\n" % (cf.start_ea,
                                                     idc.get_func_name(cf.start_ea), fea))

    out.write("\n== 伪码 ==\n")
    for fea in sorted(funcs | callers):
        name = idc.get_func_name(fea)
        out.write("\n/* ---------- %s @%08x ---------- */\n" % (name, fea))
        try:
            cfunc = ida_hexrays.decompile(fea)
            if cfunc:
                text = str(cfunc).split("\n")
                for line in text[:160]:
                    out.write(line + "\n")
                if len(text) > 160:
                    out.write("    /* …共 %d 行，已截断 */\n" % len(text))
            else:
                out.write("/* 反编译失败 */\n")
        except Exception as e:  # noqa: BLE001
            out.write("/* 反编译异常: %s */\n" % e)

    out.close()
    idc.qexit(0)


main()
