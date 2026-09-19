# Headless IDA Pro recon for the Kud Wafter Chinese patch executable.
#
# Purpose (see docs/LOCALIZATION.md): the patch keeps its translation in an
# encoded form and decodes scene data at runtime into a huge .data buffer
# (vsize 27.9MB vs rawsize 0.23MB). Plain GBK, UTF-16, short-period XOR and
# deflate were all ruled out by data-level probes, so the decode routine has to
# be found in code. This script gathers the anchors needed for that:
#   1. section map (so file offsets can be mapped to VAs)
#   2. addresses + cross-references of the patch's own plain GBK strings
#      (the Config-tool messages, which we know exist in the binary)
#   3. the functions that reference the big .data buffer, ranked
#   4. a listing of the most promising function, ready for decompilation
#
# Run headless:
#   idat.exe -A -Stools/ida_recon.py -L<log> <copy-of-exe>
#
# ASCII only on purpose: PowerShell 5.1 mangles non-ASCII .ps1/.py comments in
# this repo's workflow, and IDAPython is happier without them too.

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


out("=== SECTIONS ===")
sections = []
for seg_ea in idautils.Segments():
    seg = ida_segment.getseg(seg_ea)
    name = ida_segment.get_segm_name(seg)
    sections.append((name, seg.start_ea, seg.end_ea))
    out("%-10s %08X-%08X vsize=%X" % (name, seg.start_ea, seg.end_ea,
                                      seg.end_ea - seg.start_ea))

# Anchors: the patch's own GBK strings (hex), so we can find the patch code.
ANCHORS = [
    ("config_struct_msg",
     "bde1b9b9b2bbcdead5fba3acc7ebd6d8d0c2b0b2d7b0bababbafb2b9b6a1a1a3"),
    ("config_aspect_msg",
     "bbadc3e6d7ddbae1b1c8d3ebc6c1c4bbb2bbb7fbcab1a3accdbccff1cfd4cabebdabbbe1b1e4d0cda1a3"),
    ("config_res_msg",
     "d4dabdf8d0d0b7d6b1e6c2cad1a1d4f1cab1a3acc7ebd1a1d4f1"),
]

out("=== ANCHOR STRINGS ===")
anchor_vas = []
for label, hexbytes in ANCHORS:
    pattern = bytes.fromhex(hexbytes)
    # IDA 8.3 has no ida_bytes.find_bytes; idc.find_binary takes a spaced hex
    # pattern and searches from an address downwards.
    spaced = " ".join("%02X" % b for b in pattern)
    ea = idc.find_binary(0, idc.SEARCH_DOWN, spaced)
    if ea == idaapi.BADADDR:
        out("%s: not found" % label)
        continue
    anchor_vas.append(ea)
    out("%s: VA=%08X" % (label, ea))

out("=== XREFS TO ANCHORS ===")
anchor_funcs = set()
for ea in anchor_vas:
    for xref in idautils.DataRefsTo(ea):
        func = ida_funcs.get_func(xref)
        fname = ida_funcs.get_func_name(func.start_ea) if func else "<none>"
        out("  from %08X in %s" % (xref, fname))
        if func:
            anchor_funcs.add(func.start_ea)

# The big .data buffer: pick the largest section whose vsize >> rawsize.
big = None
for name, start, end in sections:
    size = end - start
    if size > 0x400000 and (big is None or size > big[3]):
        big = (name, start, end, size)

if big is None:
    out("no large data section found")
else:
    out("=== BIG DATA BUFFER: %s %08X-%08X (%d bytes) ===" % (big[0], big[1], big[2], big[3]))
    counts = {}
    total = 0
    for ea in range(big[1], big[2], 4):
        for xref in idautils.DataRefsTo(ea):
            func = ida_funcs.get_func(xref)
            if func is None:
                continue
            counts[func.start_ea] = counts.get(func.start_ea, 0) + 1
            total += 1
        if total > 20000:
            break
    out("references into the buffer: %d over %d functions" % (total, len(counts)))
    ranked = sorted(counts.items(), key=lambda kv: kv[1], reverse=True)[:15]
    for ea, n in ranked:
        func = ida_funcs.get_func(ea)
        prototype = idc.get_type(ea) or ""
        out("  %08X  refs=%-5d size=%d  %s %s" %
            (ea, n, func.end_ea - func.start_ea if func else 0,
             ida_funcs.get_func_name(ea), prototype))
    out("=== ANCHOR FUNCTIONS (patch's own code) ===")
    for ea in sorted(anchor_funcs):
        func = ida_funcs.get_func(ea)
        out("  %08X size=%d %s" % (ea, func.end_ea - func.start_ea if func else 0,
                                   ida_funcs.get_func_name(ea)))

idc.save_database(idc.get_idb_path(), 0)
out("=== DONE ===")
idc.qexit(0)
