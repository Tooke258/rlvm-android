# -*- coding: utf-8 -*-
"""把指定函数的「调用者」及其伪码导出来（用于顺着 REALLIVE.EXE 的调用链找引擎胶水）。

用法（IDA 批处理，复用已建好的 .idb 会很快）：
    idat.exe -A -S"<repo>\tools\ida_xrefs_of.py <out.txt> 0x4F01B0 [0x...]" "REALLIVE.EXE"
"""

import sys

import ida_auto
import ida_funcs
import ida_hexrays
import idc
import idautils


def dump_func(out, fea, limit=200):
    out.write("\n/* ---------- %s @%08x ---------- */\n" % (idc.get_func_name(fea), fea))
    try:
        cfunc = ida_hexrays.decompile(fea)
        if not cfunc:
            out.write("/* 反编译失败 */\n")
            return
        text = str(cfunc).split("\n")
        for line in text[:limit]:
            out.write(line + "\n")
        if len(text) > limit:
            out.write("    /* …共 %d 行，已截断 */\n" % len(text))
    except Exception as e:  # noqa: BLE001
        out.write("/* 反编译异常: %s */\n" % e)


def main():
    # 注意：IDA 的 -S 并不会把后面的参数传进 sys.argv（实测），所以默认写当前目录。
    out_path = sys.argv[1] if len(sys.argv) > 1 else "callers.txt"
    # 默认种子：sub_4F01B0 = 引擎里唯一的 CallDLL 分发器；
    # sub_4A5BF0 = 「DLL 名 → 槽位」的解析器（小游戏加载路径）。
    seeds = [int(a, 0) for a in sys.argv[2:]] or [0x4F01B0, 0x4A5BF0]
    ida_auto.auto_wait()
    out = open(out_path, "w", encoding="utf-8")

    all_callers = set()
    for ea in seeds:
        out.write("== 目标 %08x (%s) 的调用者 ==\n" % (ea, idc.get_func_name(ea)))
        found = False
        for xref in idautils.CodeRefsTo(ea, 0):
            f = ida_funcs.get_func(xref)
            if f:
                found = True
                all_callers.add(f.start_ea)
                out.write("   %08x (%s)\n" % (f.start_ea, idc.get_func_name(f.start_ea)))
        if not found:
            out.write("   （没有直接调用者：可能是通过函数指针调用）\n")
        # 同时列出数据引用（放进函数指针表的情况）
        for xref in idautils.DataRefsTo(ea):
            out.write("   [数据引用] %08x\n" % xref)

    out.write("\n== 调用者伪码 ==\n")
    for fea in sorted(all_callers):
        dump_func(out, fea)

    out.close()
    idc.qexit(0)


main()
