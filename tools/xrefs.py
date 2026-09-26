"""Every instruction in race.exe's .text that mentions one of the given addresses, grouped by function.

    python tools/xrefs.py <race.exe> 0x520bb4 0x521090 ...
"""
import bisect
import csv
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

OUT = Path(__file__).resolve().parent.parent / "out"


def main():
    exe = Path(sys.argv[1]).read_bytes()
    targets = {int(a, 16) for a in sys.argv[2:]}
    inv = list(csv.DictReader(open(OUT / "inventory.csv", encoding="utf-8")))
    funcs = sorted((int(r["va"], 16), int(r["size"]), r["demangled"]) for r in inv)
    starts = [f[0] for f in funcs]
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    vs, va0, rs, rp = struct.unpack_from("<IIII", exe, pe + 24 + opt + 8)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    hits = {}
    for va, size, name in funcs:
        for ins in md.disasm(exe[rp + va - base - va0: rp + va - base - va0 + size], va):
            for t in targets:
                if f"{t:#x}" in ins.op_str:
                    hits.setdefault((va, name), []).append(f"  {ins.address:08x}  {ins.mnemonic} {ins.op_str}")
    for (va, name), lines in sorted(hits.items()):
        print(f"{va:08x} {name}")
        print("\n".join(lines))


if __name__ == "__main__":
    main()
