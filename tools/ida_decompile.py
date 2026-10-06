# -*- coding: utf-8 -*-
"""把指定地址的函数（Hex-Rays 反编译）写成文本，供移植用。

用法（IDA 批处理模式；-S 后面的参数会原样进 sys.argv）：
    idat.exe -A -S"<repo>\\tools\\ida_decompile.py <out.txt> 0x100144F0 0x10014550" "PT00.dll"

注意：第一次跑会在样本旁边生成 .id0/.id1/.nam/.til 数据库；换样本换目录即可。
"""

import sys
import os

import ida_auto
import ida_hexrays
import idc

# 反编译结果里常见的“辅助函数也可以一起打”，用这个上限防爆：
EXTRA_CALLEES = False


def dump(out, ea):
    out.write("\n/* ===== %s @ %08x ===== */\n" % (idc.get_func_name(ea), ea))
    try:
        cfunc = ida_hexrays.decompile(ea)
    except Exception as exc:  # noqa: BLE001
        out.write("/* decompile exception: %s */\n" % exc)
        return
    if not cfunc:
        out.write("/* decompile returned nothing */\n")
        return
    out.write(str(cfunc))
    out.write("\n")


def main():
    # 参数优先从环境变量拿：`-S` 之后的参数在不同 IDA 版本里进不进 sys.argv 并不稳定
    # （实测 idat 8.3 走 argv 会拿不到），环境变量则一定继承得到。
    #   IDA_DECOMP_OUT   = 输出 txt 路径
    #   IDA_DECOMP_SEEDS = "0x100144F0,0x10014550"
    env_out = os.environ.get("IDA_DECOMP_OUT")
    env_seeds = os.environ.get("IDA_DECOMP_SEEDS")
    if env_out and env_seeds:
        out_path = env_out
        seeds = [int(a, 0) for a in env_seeds.split(",") if a.strip()]
    else:
        out_path = sys.argv[1] if len(sys.argv) > 1 else "decomp.txt"
        seeds = [int(a, 0) for a in sys.argv[2:]]
    ida_auto.auto_wait()
    with open(out_path, "w", encoding="utf-8") as out:
        for ea in seeds:
            dump(out, ea)
    idc.qexit(0)


if __name__ == "__main__":
    main()
