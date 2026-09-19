# Headless IDA: find large non-code blobs inside the code segment.
#
# Rationale: every data-level probe for the Chinese translation failed (plain
# GBK/UTF-8/UTF-16, single-byte XOR/add/sub/NOT, repeating XOR, chained XOR,
# deflate, full-disk scan). A payload compressed with a text-aware scheme can
# sit around 6.8-7.0 bits/byte, which is indistinguishable from x86 code by
# entropy alone - but IDA will not have recognised it as instructions. So look
# for the largest runs of bytes that no function covers and that are not code.
#
# Run headless:
#   idat.exe -A -Stools/ida_data_blobs.py -L<log> <copy-of-exe>

import idaapi
import idautils
import ida_auto
import ida_bytes
import ida_funcs
import ida_segment
import idc

ida_auto.auto_wait()


def out(msg):
    print(msg)


out("=== NON-CODE RUNS PER SEGMENT ===")
for seg_ea in idautils.Segments():
    seg = ida_segment.getseg(seg_ea)
    name = ida_segment.get_segm_name(seg)
    runs = []
    start = None
    ea = seg.start_ea
    while ea < seg.end_ea:
        is_code = ida_bytes.is_code(ida_bytes.get_flags(ea))
        func = ida_funcs.get_func(ea)
        if (not is_code) and func is None:
            if start is None:
                start = ea
        else:
            if start is not None and ea - start >= 4096:
                runs.append((start, ea - start))
            start = None
        ea += 1
    if start is not None and seg.end_ea - start >= 4096:
        runs.append((start, seg.end_ea - start))
    runs.sort(key=lambda r: r[1], reverse=True)
    out("%s: %d runs >=4KB" % (name, len(runs)))
    for addr, size in runs[:12]:
        out("   %08X  size=%d (0x%X)" % (addr, size, size))

out("=== FUNCTIONS OVERLAPPING THE BIGGEST RUN ===")
best = None
for seg_ea in idautils.Segments():
    seg = ida_segment.getseg(seg_ea)
    ea = seg.start_ea
    while ea < seg.end_ea:
        if ida_bytes.is_code(ida_bytes.get_flags(ea)) or ida_funcs.get_func(ea):
            ea += 1
            continue
        run_start = ea
        while ea < seg.end_ea and (not ida_bytes.is_code(ida_bytes.get_flags(ea))) \
                and ida_funcs.get_func(ea) is None:
            ea += 1
        size = ea - run_start
        if best is None or size > best[1]:
            best = (run_start, size)
if best is not None:
    out("largest run: %08X size=%d" % best)
    for f in idautils.Functions(best[0] - 0x100, best[0] + best[1] + 0x100):
        func = ida_funcs.get_func(f)
        out("   function %08X size=%d %s" % (f, func.end_ea - func.start_ea,
                                             ida_funcs.get_func_name(f)))

idc.save_database(idc.get_idb_path(), 0)
out("=== DONE ===")
idc.qexit(0)
