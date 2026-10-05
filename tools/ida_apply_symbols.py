# -*- coding: utf-8 -*-
"""IDA 批处理脚本（layer 1）：按 TSV 清单给 PT00.dll 里的函数/数据改名，然后
顺手调用同目录的 dump_dll.py 导出一份**带名字**的反编译 dump。

用法（32 位 PE 用 idat.exe；x64 用 idat64.exe）：

    idat.exe -A -Lida.log -S"<repo>\\tools\\ida_apply_symbols.py <repo>\\tools\\pt00_symbols.tsv <out.txt>" PT00.dll

设计取舍：**不依赖 .idb 的持久化**——每次要用重新跑一遍即可（改名是幂等的），
这样仓库里只有可审查的文本清单，不带任何二进制数据库。
"""

import os
import sys

import idaapi
import idautils
import idc


def _auto_wait():
    try:
        import ida_auto
        ida_auto.auto_wait()
    except Exception:  # noqa: BLE001
        try:
            idaapi.auto_wait()
        except Exception:  # noqa: BLE001
            pass


def _parse_args():
    argv = getattr(idc, "ARGV", None) or sys.argv
    symbols_path = None
    dump_path = None
    for a in argv[1:]:
        low = a.lower()
        if low.endswith(".tsv"):
            symbols_path = a
        elif low.endswith(".txt"):
            dump_path = a
    return symbols_path, dump_path


def _apply_one(ea, name, note):
    flags = 0
    try:
        flags = idc.SN_NOWARN | idc.SN_FORCE
    except Exception:  # noqa: BLE001
        flags = 0x800 | 0x2
    ok = False
    try:
        ok = bool(idc.set_name(ea, name, flags))
    except Exception:  # noqa: BLE001
        ok = False
    if not ok:
        try:
            ok = bool(idc.set_name(ea, name, 0))
        except Exception:  # noqa: BLE001
            ok = False
    if ok and note:
        try:
            idc.set_cmt(ea, note, 1)  # 1 = repeatable comment
        except Exception:  # noqa: BLE001
            pass
    return ok


def main():
    symbols_path, dump_path = _parse_args()
    if not symbols_path or not os.path.isfile(symbols_path):
        sys.stderr.write("ida_apply_symbols: missing symbols tsv (arg=%r)\n"
                         % symbols_path)
        idc.qexit(1)
        return

    _auto_wait()

    applied = 0
    missing = []
    with open(symbols_path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = raw.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            addr, kind, name = parts[0].strip(), parts[1].strip(), parts[2].strip()
            note = parts[3].strip() if len(parts) > 3 else ""
            if not addr.lower().startswith("0x") or not name:
                continue
            ea = int(addr, 16)
            try:
                mapped = idaapi.is_loaded(ea)
            except Exception:  # noqa: BLE001
                mapped = True
            if not mapped:
                missing.append(addr)
                continue
            if _apply_one(ea, name, ("[%s] %s" % (kind, note)).strip()):
                applied += 1
            else:
                missing.append(addr)

    sys.stderr.write("ida_apply_symbols: applied=%d missing=%d%s\n"
                     % (applied, len(missing),
                        (" (" + ", ".join(missing) + ")") if missing else ""))

    # 顺手导出带名字的 dump（复用 dump_dll.py，避免两份导出代码）。
    here = os.path.dirname(os.path.abspath(symbols_path))
    dump_script = os.path.join(here, "dump_dll.py")
    if os.path.isfile(dump_script):
        if dump_path:
            try:
                idc.ARGV = [dump_script, dump_path]
            except Exception:  # noqa: BLE001
                pass
        with open(dump_script, "r", encoding="utf-8") as f:
            code = f.read()
        exec(compile(code, dump_script, "exec"), {"__name__": "__main__"})
    else:
        sys.stderr.write("ida_apply_symbols: dump_dll.py not found next to the "
                         "symbols file; database left with new names only\n")
        idc.qexit(0)


main()
