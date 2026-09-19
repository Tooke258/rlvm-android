# Headless IDA: find the patch's own memory-management call sites.
#
# Rationale (docs/LOCALIZATION.md): data-level probes ruled out plain GBK,
# UTF-8, UTF-16, short-period XOR, single-byte add/sub/NOT and deflate, and the
# executable has no high-entropy payload, yet the patch produces megabytes of
# Chinese scene text at runtime in a growing region (observed at VA 0x0E560000
# in two separate dumps). So the translation must be decoded by code, and the
# decoder must allocate that buffer. This script lists every call site of the
# allocator imports, then decompiles them with Hex-Rays so the routine can be
# read without a GUI.
#
# Run headless:
#   idat.exe -A -Stools/ida_alloc_xrefs.py -L<log> <copy-of-exe>

import idaapi
import idautils
import ida_auto
import ida_funcs
import ida_nalt
import ida_hexrays
import idc

ida_auto.auto_wait()


def out(msg):
    print(msg)


WANTED = ("virtualalloc", "virtualprotect", "virtualfree", "heapalloc",
          "globalalloc", "realloc", "malloc")

out("=== IMPORTS OF INTEREST ===")
targets = []
qty = ida_nalt.get_import_module_qty()
out("  import modules: %d" % qty)
all_names = []
for i in range(qty):
    mod = ida_nalt.get_import_module_name(i)
    out("  module[%d] = %s" % (i, mod))
    def cb(ea, name, ordinal):
        all_names.append((ea, name, ordinal))
        if name:
            # IDA 8.3 hands out str here; older builds hand out bytes.
            text = name.decode("ascii", "replace") if isinstance(name, bytes) else name
            lname = text.lower()
            for w in WANTED:
                if lname == w or lname.endswith("!" + w) or lname.endswith("_" + w):
                    targets.append((ea, text))
        return True
    ida_nalt.enum_import_names(i, cb)

out("  total imports: %d" % len(all_names))
for ea, name, ordinal in all_names[:40]:
    out("    %08X ord=%s name=%s" % (ea, ordinal, name))

for ea, name in targets:
    out("  %08X  %s" % (ea, name))

out("=== CALL SITES ===")
seen_funcs = []
for ea, name in targets:
    for xref in idautils.XrefsTo(ea):
        func = ida_funcs.get_func(xref.frm)
        fname = ida_funcs.get_func_name(func.start_ea) if func else "<none>"
        fsize = (func.end_ea - func.start_ea) if func else 0
        out("  %-14s called from %08X in %s (size %d)" % (name, xref.frm, fname, fsize))
        if func and func.start_ea not in [f[0] for f in seen_funcs]:
            seen_funcs.append((func.start_ea, fsize, fname))

# Decompile the callers, smallest first: the patch's own helper is likely small.
seen_funcs.sort(key=lambda f: f[1])
out("=== DECOMPILED CALLERS (up to 6) ===")
for ea, size, fname in seen_funcs[:6]:
    out("---- %08X %s (size %d) ----" % (ea, fname, size))
    try:
        cfunc = ida_hexrays.decompile(ea)
        if cfunc is None:
            out("  <decompilation failed>")
            continue
        text = str(cfunc)
        for line in text.splitlines()[:120]:
            out("  " + line)
    except Exception as exc:
        out("  <exception: %s>" % exc)

idc.save_database(idc.get_idb_path(), 0)
out("=== DONE ===")
idc.qexit(0)
