# -*- coding: utf-8 -*-
"""IDA 8.x 批处理脚本：导出 PE 基本信息 + 导入/导出表 + 函数清单 + Hex-Rays 反编译。

用法（32 位 PE 用 idat.exe，64 位用 idat64.exe）：

    idat.exe -A -Lida.log -S"<repo>\\tools\\dump_dll.py <out.txt>" PT00.dll

`-A` 无人值守、`-L` 日志、`-S` 启动后跑脚本（引号内可带参数）。反编译需要
Hex-Rays（IDA Pro 自带）；不可用时自动退化为只导函数清单。

为什么先看导入表：它决定 DLL 是「自己读输入/自己画屏幕」还是「纯数据」——
PT00/EF00 都只导入 KERNEL32，属于后者，移植时不需要接输入与绘制层。
"""

import sys

import idaapi
import idautils
import idc

try:
    import ida_hexrays
    _HAVE_HEXRAYS = True
except Exception:  # noqa: BLE001
    _HAVE_HEXRAYS = False


def main():
    argv = getattr(idc, "ARGV", None) or sys.argv
    out_path = None
    for a in argv[1:]:
        if a.lower().endswith(".txt"):
            out_path = a
    if out_path is None:
        out_path = idc.get_input_file_path() + ".dump.txt"

    with open(out_path, "w", encoding="utf-8", errors="replace") as f:
        def w(s=""):
            f.write(str(s) + "\n")

        w("== file ==")
        w("path      : %s" % idc.get_input_file_path())
        w("processor : %s" % idaapi.get_processor_name())
        w("imagebase : 0x%08X" % idaapi.get_imagebase())
        w("entry     : 0x%08X" % idc.get_inf_attr(idc.INF_START_EA))
        w("")

        w("== imports ==")
        for i in range(idaapi.get_import_module_qty()):
            w("[%s]" % idaapi.get_import_module_name(i))

            def cb(ea, name, ordinal):
                w("  0x%08X  %s" % (ea, name or ("#%d" % ordinal)))
                return True

            idaapi.enum_import_names(i, cb)
        w("")

        w("== exports ==")
        for _index, ordinal, ea, name in idautils.Entries():
            w("  ord=%-5d ea=0x%08X  %s" % (ordinal, ea, name))
        w("")

        funcs = list(idautils.Functions())
        w("== functions (%d) ==" % len(funcs))
        for ea in funcs:
            end = idc.get_func_attr(ea, idc.FUNCATTR_END)
            w("  0x%08X  size=%-6d %s" % (ea, end - ea, idc.get_func_name(ea)))
        w("")

        ok = False
        if _HAVE_HEXRAYS:
            try:
                ok = ida_hexrays.init_hexrays_plugin()
            except Exception:  # noqa: BLE001
                ok = False
        w("== decompiled (%s) ==" % ("hex-rays" if ok else "unavailable"))
        if ok:
            for ea in funcs:
                w("")
                w("/* ---- %s @ 0x%08X ---- */" % (idc.get_func_name(ea), ea))
                try:
                    cf = ida_hexrays.decompile(ea)
                    w(str(cf) if cf is not None else "// decompile returned None")
                except Exception as exc:  # noqa: BLE001
                    w("// decompile failed: %s" % exc)

    sys.stderr.write("dump_dll: wrote %s\n" % out_path)
    idc.qexit(0)


main()
