"""Match the named v1.0 race.exe functions to another engine build (a race.bin), by masked byte signature.

Each v1.0 function's bytes, with everything that moves between builds masked -- rel32 call/jump
targets, and any 32-bit immediate or displacement that points inside the image -- is searched for in
the target's .text. A unique hit is a match; its masked words also map v1.0 globals to the target's.

    python tools/match_builds.py <race.exe v1.0> <target race.bin> <out/name.csv>
"""
import csv
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

ROOT = Path(__file__).resolve().parent.parent


def load(path):
    b = Path(path).read_bytes()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    opt = struct.unpack_from("<H", b, pe + 20)[0]
    base = struct.unpack_from("<I", b, pe + 24 + 28)[0]
    vs, va, rs, rp = struct.unpack_from("<IIII", b, pe + 24 + opt + 8)
    return b, base, base + va, b[rp:rp + vs]


def masked(code, va, lo, hi, all_imm=False):
    """(bytes, mask positions) for one function: rel32 targets and in-image absolute values masked."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    out = bytearray(code)
    holes = []
    for ins in md.disasm(code, va):
        o = ins.address - va
        if ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j"):
            if ins.bytes[0] in (0xE8, 0xE9) or (ins.bytes[0] == 0x0F and 0x80 <= ins.bytes[1] <= 0x8F):
                holes.append(o + len(ins.bytes) - 4)
                continue
        for off, size in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
            if off and size == 4:
                v = struct.unpack_from("<I", ins.bytes, off)[0]
                if all_imm or lo <= v < hi:
                    holes.append(o + off)
    return bytes(out), sorted(set(holes))


FILLERS = {("mov", "edi, edi"), ("nop", ""), ("int3", ""), ("add", "eax, 0")}


def trim_filler(code, va):
    """Drop the alignment filler MSVC 4 puts after a function (lea reg,[reg]; mov edi,edi; int3; ...):
    it depends on what follows the function, so it differs between builds."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    end = 0
    for ins in md.disasm(code, va):
        ops = ins.op_str.split(", ")
        filler = (ins.mnemonic, ins.op_str) in FILLERS or (
            ins.mnemonic == "lea" and len(ops) == 2 and ops[1].replace(" + 0", "") == f"[{ops[0]}]")
        if not filler:
            end = ins.address - va + ins.size
    return code[:end] if end else code


def pattern(code, holes):
    parts, i = [], 0
    for h in holes:
        if h < i:
            continue
        parts.append(re.escape(code[i:h]))
        parts.append(b"....")
        i = h + 4
    parts.append(re.escape(code[i:]))
    return re.compile(b"".join(parts), re.S)


def main():
    src, sbase, stva, stext = load(sys.argv[1])
    dst, dbase, dtva, dtext = load(sys.argv[2])
    out = Path(sys.argv[3])
    inv = list(csv.DictReader(open(ROOT / "out" / "inventory.csv", encoding="utf-8")))
    rows, matched_bytes, total_bytes = [], 0, 0
    todo = []
    for r in inv:
        va, size = int(r["va"], 16), int(r["size"])
        if size < 12 or r["library"] in ("LIBC",):
            continue
        todo.append((r, va, size, trim_filler(stext[va - stva: va - stva + size], va)))
        total_bytes += size
    claimed = set()
    # pass 1: addresses masked; pass 2 (the rest): EVERY 32-bit constant masked -- builds tweak limits
    # (1.2.x raised the world list 0x400 -> 0x410), and the DLL checks each value it patches anyway
    for all_imm in (False, True):
        left = []
        for r, va, size, code in todo:
            body, holes = masked(code, va, sbase, sbase + 0x300000, all_imm)
            if all_imm and len(code) - 4 * len(holes) < 16:
                left.append((r, va, size, code))            # too little left to be distinctive
                continue
            hits = [m.start() for _, m in zip(range(2), pattern(body, holes).finditer(dtext))]
            if len(hits) == 1 and hits[0] not in claimed:
                claimed.add(hits[0])
                matched_bytes += size
                rows.append(dict(va=r["va"], target=f"{dtva + hits[0]:08x}", size=size, name=r["demangled"],
                                 how="constants masked" if all_imm else "addresses masked"))
            else:
                left.append((r, va, size, code))
        todo = left
    with out.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=["va", "target", "size", "name", "how"])
        w.writeheader()
        w.writerows(rows)
    print(f"{Path(sys.argv[2]).name}: {len(rows)} functions matched uniquely, "
          f"{100 * matched_bytes / total_bytes:.0f}% of code by size -> {out}")


if __name__ == "__main__":
    main()
