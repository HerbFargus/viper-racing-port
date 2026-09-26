"""From matched function pairs (tools/match_builds.py), walk each pair in lockstep and map what they
reference: every rel32 call/jump target (functions the matcher couldn't place) and every in-image
address operand (globals, strings, vtables). Repeats until nothing new is found.

    python tools/propagate.py <race.exe v1.0> <target race.bin> <out/match_X.csv>  ->  out/map_X.json
"""
import csv
import json
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

sys.path.insert(0, str(Path(__file__).resolve().parent))
import match_builds as M  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def refs(code, va, lo, hi):
    """[(offset of instruction, kind, value)] for rel32 targets and in-image 32-bit operands."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    out = []
    for ins in md.disasm(code, va):
        o = ins.address - va
        if ins.bytes[0] in (0xE8, 0xE9) or (ins.bytes[0] == 0x0F and len(ins.bytes) > 1 and 0x80 <= ins.bytes[1] <= 0x8F):
            if ins.bytes[0] == 0xE8 or ins.bytes[0] == 0xE9 and len(ins.bytes) == 5 or ins.bytes[0] == 0x0F:
                tgt = ins.address + ins.size + struct.unpack_from("<i", ins.bytes, ins.size - 4)[0]
                out.append((o, "call" if ins.bytes[0] == 0xE8 else "jump", tgt))
                continue
        for off, size in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
            if off and size == 4:
                v = struct.unpack_from("<I", ins.bytes, off)[0]
                if lo <= v < hi:
                    out.append((o, "data", v))
    return out


def main():
    src, sbase, stva, stext = M.load(sys.argv[1])
    dst, dbase, dtva, dtext = M.load(sys.argv[2])
    pairs = {int(r["va"], 16): int(r["target"], 16) for r in csv.DictReader(open(sys.argv[3], encoding="utf-8"))}
    inv = {int(r["va"], 16): r for r in csv.DictReader(open(ROOT / "out" / "inventory.csv", encoding="utf-8"))}
    funcs = dict(pairs)                       # v1.0 function entry -> target entry
    data, conflicts = {}, 0
    votes = {}                                # v1.0 address -> {target: how many references say so}
    for sva, dva in pairs.items():
        size = int(inv[sva]["size"])
        scode = M.trim_filler(stext[sva - stva: sva - stva + size], sva)
        dcode = dtext[dva - dtva: dva - dtva + len(scode)]
        sr = refs(scode, sva, sbase, sbase + 0x300000)
        dr = refs(dcode, dva, dbase, dbase + 0x300000)
        dmap = {(o, k): v for o, k, v in dr}
        for o, k, v in sr:
            w = dmap.get((o, k))
            if w is None:
                continue
            table = data if k == "data" else funcs
            if k != "data" and v not in inv:
                continue                      # a jump inside the function, not to another function
            vv = votes.setdefault(v, {})
            vv[w] = vv.get(w, 0) + 1
            if v in table and table[v] != w:
                conflicts += 1
                continue
            table[v] = w
    named = {inv[v]["demangled"]: f"{w:08x}" for v, w in funcs.items() if v in inv}
    out = ROOT / "out" / (Path(sys.argv[3]).stem.replace("match_", "map_") + ".json")
    out.write_text(json.dumps({"functions": {f"{v:08x}": f"{w:08x}" for v, w in funcs.items()},
                               "data": {f"{v:08x}": f"{w:08x}" for v, w in data.items()},
                               "names": named,
                               # references that disagree: a signature match can land on a byte-identical twin
                               "disputed": {f"{v:08x}": {f"{w:08x}": c for w, c in vv.items()}
                                            for v, vv in votes.items()
                                            if len(vv) > 1 or next(iter(vv)) != (funcs.get(v) if v in funcs else data.get(v))}},
                              indent=1))
    print(f"{len(pairs)} matched -> {len(funcs)} functions placed (callees added), {len(data)} data addresses mapped, "
          f"{conflicts} conflicts -> {out}")


if __name__ == "__main__":
    main()
