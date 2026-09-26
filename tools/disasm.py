"""Disassemble v1.0 race.exe functions for porting: names, and the value and width of every constant.

    python tools/disasm.py 0x436120 MatrixNormalize ...

Each function (by address, or by a name matched against the map's demangled names) is listed whole, to
its size in the inventory. Calls and jumps are named; a memory operand in .rdata/.data shows its value as
the float or double it's read as ("dword" = float, "qword" = double) -- a rewrite must use constants of the
same width, or it isn't bit for bit.
"""
from __future__ import annotations

import csv
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
BASE = 0x400000


def main() -> int:
    exe = (OUT / "race_v10.exe").read_bytes()
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    n = struct.unpack_from("<H", exe, pe + 6)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    secs = [struct.unpack_from("<8sIIII", exe, pe + 24 + opt + i * 40) for i in range(n)]

    def off(va):
        for _nm, vs, vaddr, rs, ro in secs:
            if vaddr <= va - BASE < vaddr + max(vs, rs):
                return va - BASE - vaddr + ro if va - BASE - vaddr < rs else None
        return None

    inv = list(csv.DictReader(open(OUT / "inventory.csv", encoding="utf-8")))
    sym = {int(r["va"], 16): r["demangled"] for r in csv.DictReader(open(OUT / "symbols.csv", encoding="utf-8"))}
    size = {int(r["va"], 16): int(r["size"]) for r in inv}
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for arg in sys.argv[1:]:
        if arg.startswith("0x"):
            vas = [int(arg, 16)]
        else:
            vas = [int(r["va"], 16) for r in inv if arg in r["demangled"]]
        for va in vas:
            print(f"==== {va:08x} {sym.get(va, '?')}  ({size.get(va, 0)} bytes)")
            o = off(va)
            for ins in md.disasm(exe[o:o + size.get(va, 64)], va):
                note = ""
                if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
                    note = sym.get(int(ins.op_str, 16), "")
                m = re.search(r"(dword|qword) ptr \[(0x[0-9a-f]+)\]", ins.op_str)
                if m:
                    a = int(m.group(2), 16)
                    fo = off(a)
                    if fo is not None and m.group(1) == "qword":
                        note = f"= {struct.unpack_from('<d', exe, fo)[0]!r} (double)"
                    elif fo is not None:
                        u = struct.unpack_from("<I", exe, fo)[0]
                        f = struct.unpack_from("<f", exe, fo)[0]
                        note = f"= {f!r} (float, {u:08x})" if ins.mnemonic.startswith("f") else f"= {u:#x}"
                    if a in sym:
                        note = f"{sym[a][:50]} {note}"
                print(f"  {ins.address:08x}  {ins.mnemonic:7s} {ins.op_str:40s} {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
