# -*- coding: utf-8 -*-
"""扫描整个 DLL：找出所有操作数里带指定字节偏移（如 9C0h）的指令。

用途：反查「某个 intD 槽是谁写的」。例：intD[624] 的字节偏移是 624*4 = 2496 = 0x9C0，
DLL 里以 `[reg+9C0h]` 的形式访问；本脚本把命中处连同所在函数一起打出来。

用法（IDA 批处理；参数走环境变量，理由同 ida_decompile.py）：
    set IDA_OFFSET_OUT=<out.txt>
    set IDA_OFFSET_PAT=9C0h
    idat.exe -A -S"<repo>\\tools\\ida_find_offset.py" "PT00.dll"
"""

import os

import ida_auto
import idautils
import idc


def main():
    out_path = os.environ.get("IDA_OFFSET_OUT")
    pats = [p.strip() for p in os.environ.get("IDA_OFFSET_PAT", "9C0h").split(",") if p.strip()]
    ida_auto.auto_wait()
    with open(out_path, "w", encoding="utf-8") as out:
        out.write("# patterns = %s\n" % ", ".join(pats))
        seen = 0
        for ea in idautils.Heads():
            line = idc.generate_disasm_line(ea, 0)
            if not line or not any(p in line for p in pats):
                continue
            seen += 1
            fn = idc.get_func_name(ea)
            out.write("%08x  %-28s  %s\n" % (ea, fn, line))
        out.write("# total hits = %d\n" % seen)
    idc.qexit(0)


if __name__ == "__main__":
    main()
