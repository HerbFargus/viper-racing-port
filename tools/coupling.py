"""How coupled each library is to the platform layers, from out/inventory.csv plus the call graph.

The game's own libraries 'gx' (graphics), 'sound', 'multi' (networking) and 'kernel' (logging, memory,
files, timers, tasks) are the platform layers a port replaces. For every other function this asks which
of those layers it calls into, directly or through other non-layer game code -- stopping at a layer's
boundary rather than tracing through it, and ignoring the C runtime.

    python tools/coupling.py <race.exe>
"""
import collections
import csv
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

OUT = Path(__file__).resolve().parent.parent / "out"
LAYERS = ("gx", "sound", "multi", "kernel")


def main():
    exe = Path(sys.argv[1]).read_bytes()
    inv = list(csv.DictReader(open(OUT / "inventory.csv", encoding="utf-8")))
    lib_of = {int(r["va"], 16): r["library"] for r in inv}
    size_of = {int(r["va"], 16): int(r["size"]) for r in inv}
    name_of = {int(r["va"], 16): r["demangled"] for r in inv}
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    vs, va0, rs, rp = struct.unpack_from("<IIII", exe, pe + 24 + opt + 8)
    tva = base + va0
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    calls = {}
    for va, sz in size_of.items():
        code = exe[rp + va - tva: rp + va - tva + sz]
        calls[va] = {struct.unpack_from("<i", i.bytes, 1)[0] + i.address + 5 for i in md.disasm(code, va)
                     if i.mnemonic == "call" and i.bytes[0] == 0xE8}
    reach = {}

    def layers_of(va, seen):
        if va in reach:
            return reach[va]
        if va in seen:
            return set()
        seen.add(va)
        out = set()
        for c in calls.get(va, ()):
            lib = lib_of.get(c)
            if lib is None or lib in ("LIBC", "crt"):
                continue
            if lib in LAYERS:
                out.add(lib)
            else:
                out |= layers_of(c, seen)
        reach[va] = out
        return out

    sys.setrecursionlimit(100000)
    real = {va for va, sz in size_of.items() if sz > 16 and not name_of[va].startswith("$E")}
    by = collections.defaultdict(lambda: collections.Counter())
    bybytes = collections.defaultdict(lambda: collections.Counter())
    for va in real:
        lib = lib_of[va]
        if lib in LAYERS or lib in ("LIBC", "crt"):
            continue
        ls = layers_of(va, set()) - {"kernel"}
        key = "pure (at most kernel services)" if not ls else "uses " + "+".join(sorted(ls))
        by[lib][key] += 1
        bybytes[lib][key] += size_of[va]
    print(f"real functions (>16 bytes, not $E initialisers): {len(real):,}\n")
    for lib in sorted(bybytes, key=lambda l: -sum(bybytes[l].values())):
        tot = sum(bybytes[lib].values())
        parts = ", ".join(f"{k}: {by[lib][k]} fns / {100 * v / tot:.0f}%" for k, v in bybytes[lib].most_common())
        print(f"{lib:9s} {tot:8,} B  {parts}")
    for lay in LAYERS:
        n = sum(1 for va in real if lib_of[va] == lay)
        b = sum(size_of[va] for va in real if lib_of[va] == lay)
        print(f"layer {lay:7s}: {n:4d} real functions, {b:7,} bytes")


if __name__ == "__main__":
    main()
