# -*- coding: utf-8 -*-
"""打印一批地址的引用点（含跳表/虚表这类间接引用），并把引用者所在函数一起打出来。

用法（IDA 批处理；参数走环境变量，理由同 ida_decompile.py）：
    set IDA_XREFS_OUT=<out.txt>
    set IDA_XREFS_ADDRS=0x10004AD0,0x10004AE0
    idat.exe -A -S"<repo>\\tools\\ida_xrefs.py" "PT00.dll"
"""

import os

import ida_auto
import idautils
import idc


def main():
    out_path = os.environ.get("IDA_XREFS_OUT")
    addrs = os.environ.get("IDA_XREFS_ADDRS", "")
    if not out_path or not addrs:
        raise SystemExit("need IDA_XREFS_OUT and IDA_XREFS_ADDRS")
    ida_auto.auto_wait()
    with open(out_path, "w", encoding="utf-8") as out:
        for spec in addrs.split(","):
            ea = int(spec.strip(), 0)
            out.write("\n===== xrefs to %08x (%s) =====\n" % (ea, idc.get_name(ea)))
            n = 0
            for xref in idautils.XrefsTo(ea, 0):
                fn = idc.get_func_name(xref.frm)
                out.write("  from %08x  type=%d  func=%s  %s\n"
                          % (xref.frm, xref.type, fn,
                             idc.generate_disasm_line(xref.frm, 0)))
                n += 1
            out.write("  total=%d\n" % n)
    idc.qexit(0)


if __name__ == "__main__":
    main()
