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


def _procname():
    """IDA 8.x 里 processor 名在 ida_ida.inf_get_procname()；旧 API 作兜底。"""
    try:
        import ida_ida
        return ida_ida.inf_get_procname()
    except Exception:  # noqa: BLE001
        pass
    try:
        return idaapi.get_inf_structure().procName
    except Exception:  # noqa: BLE001
        return "?"


def main():
    argv = getattr(idc, "ARGV", None) or sys.argv
    out_path = None
    for a in argv[1:]:
        if a.lower().endswith(".txt"):
            out_path = a
    if out_path is None:
        out_path = idc.get_input_file_path() + ".dump.txt"

    # 批处理模式（-A）下脚本会在自动分析结束前被调用：不等到分析完成，
    # IDA 只会认出入口那个函数、Hex-Rays 也没什么可反编译的。
    try:
        import ida_auto
        ida_auto.auto_wait()
    except Exception:  # noqa: BLE001
        try:
            idaapi.auto_wait()
        except Exception:  # noqa: BLE001
            pass

    with open(out_path, "w", encoding="utf-8", errors="replace") as f:
        def w(s=""):
            f.write(str(s) + "\n")

        w("== file ==")
        w("path      : %s" % idc.get_input_file_path())
        w("processor : %s" % _procname())
        try:
            w("imagebase : 0x%08X" % idaapi.get_imagebase())
        except Exception as exc:  # noqa: BLE001
            w("imagebase : ? (%s)" % exc)
        try:
            w("entry     : 0x%08X" % idc.get_inf_attr(idc.INF_START_EA))
        except Exception as exc:  # noqa: BLE001
            w("entry     : ? (%s)" % exc)
        w("")

        w("== imports ==")
        try:
            for i in range(idaapi.get_import_module_qty()):
                w("[%s]" % idaapi.get_import_module_name(i))

                def cb(ea, name, ordinal):
                    w("  0x%08X  %s" % (ea, name or ("#%d" % ordinal)))
                    return True

                idaapi.enum_import_names(i, cb)
        except Exception as exc:  # noqa: BLE001
            w("// imports failed: %s" % exc)
        w("")

        w("== exports ==")
        try:
            for _index, ordinal, ea, name in idautils.Entries():
                w("  ord=%-5d ea=0x%08X  %s" % (ordinal, ea, name))
        except Exception as exc:  # noqa: BLE001
            w("// exports failed: %s" % exc)
        w("")

        try:
            funcs = list(idautils.Functions())
        except Exception as exc:  # noqa: BLE001
            funcs = []
            w("// function enumeration failed: %s" % exc)
        w("== functions (%d) ==" % len(funcs))
        for ea in funcs:
            try:
                end = idc.get_func_attr(ea, idc.FUNCATTR_END)
                w("  0x%08X  size=%-6d %s" % (ea, end - ea, idc.get_func_name(ea)))
            except Exception:  # noqa: BLE001
                w("  0x%08X  ?" % ea)
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
