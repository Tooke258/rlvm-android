# -*- coding: utf-8 -*-
"""把指定地址区间的反汇编打成文本（定位「这个常量是怎么传进去的」这类问题）。

用法（IDA 批处理模式）：
    set IDA_DISASM_OUT=<out.txt>
    set IDA_DISASM_RANGES=0x100064E0-0x10006680
    idat.exe -A -S"<repo>\\tools\\ida_disasm.py" "PT00.dll"

和 ida_decompile.py 一样，参数走**环境变量**（idat 8.3 里 -S 之后的参数不保证进 argv）。
"""

import os

import ida_auto
import idc


def main():
    out_path = os.environ.get("IDA_DISASM_OUT")
    ranges = os.environ.get("IDA_DISASM_RANGES")
    if not out_path or not ranges:
        raise SystemExit("need IDA_DISASM_OUT and IDA_DISASM_RANGES")
    ida_auto.auto_wait()
    with open(out_path, "w", encoding="utf-8") as out:
        for spec in ranges.split(","):
            lo_s, _, hi_s = spec.strip().partition("-")
            lo, hi = int(lo_s, 0), int(hi_s, 0)
            out.write("\n===== disasm %08x..%08x =====\n" % (lo, hi))
            ea = lo
            while ea < hi:
                out.write("%08x  %s\n" % (ea, idc.generate_disasm_line(ea, 0)))
                nxt = idc.next_head(ea, hi)
                if nxt <= ea:
                    ea += 1
                else:
                    ea = nxt
    idc.qexit(0)


if __name__ == "__main__":
    main()
