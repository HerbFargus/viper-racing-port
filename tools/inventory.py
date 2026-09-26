"""Function inventory for race.exe: size, callers and callees, and which Windows/DirectX APIs each
function reaches -- directly, or through anything it calls.

Direct calls (e8 rel32) and import calls (ff 15 [__imp_X], or calls to a jmp [__imp_X] thunk) are
followed. Calls through a register or a vtable are counted but can't be resolved statically: that is
how COM (DirectDraw, Direct3D, DirectSound, DirectInput) is reached, so a function's library is the
better guide there -- see the "platform" column.

    python tools/inventory.py <race.exe> [out/symbols.csv]  ->  out/inventory.csv, printed summary
"""
import collections
import csv
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

OUT = Path(__file__).resolve().parent.parent / "out"
PLATFORM_DLLS = {"DDRAW": "graphics", "DSOUND": "sound", "DINPUT": "input", "USER32": "window", "GDI32": "window",
                 "WSOCK32": "network", "TAPI32": "network", "WINMM": "timer/audio", "ADVAPI32": "registry",
                 "KERNEL32": "system"}
PLATFORM_LIBS = {"gx": "graphics", "sound": "sound", "kernel": "system", "multi": "network"}


def sections(b):
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    n = struct.unpack_from("<H", b, pe + 6)[0]
    opt = struct.unpack_from("<H", b, pe + 20)[0]
    base = struct.unpack_from("<I", b, pe + 24 + 28)[0]
    out = []
    for i in range(n):
        o = pe + 24 + opt + 40 * i
        vs, va, rs, rp = struct.unpack_from("<IIII", b, o + 8)
        out.append((b[o:o + 8].rstrip(b"\0").decode(), base + va, vs, rp, rs))
    return out


def main():
    exe = Path(sys.argv[1]).read_bytes()
    syms = list(csv.DictReader(open(sys.argv[2] if len(sys.argv) > 2 else OUT / "symbols.csv", encoding="utf-8")))
    text = next(s for s in sections(exe) if s[0] == ".text")
    _, tva, tvs, trp, _ = text
    first = {}
    for r in syms:                      # identical functions folded together share an address: keep one name
        va = int(r["va"], 16)
        if r["kind"] == "function" and tva <= va < tva + tvs:
            first.setdefault(va, r)
    funcs = sorted(first.items())
    starts = [va for va, _ in funcs]
    by_va = {va: r for va, r in funcs}
    imports = {int(r["va"], 16): r for r in syms if r["section"] == ".idata"}

    def import_dll(r):
        # object column carries "KERNEL32.dll" etc. for import thunks and __imp_ slots
        return r["object"].split(".")[0].upper()

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    info = {}
    thunk_to_dll = {}
    for i, (va, r) in enumerate(funcs):
        end = starts[i + 1] if i + 1 < len(starts) else tva + tvs
        code = exe[trp + va - tva: trp + end - tva]
        calls, dlls, indirect = set(), set(), 0
        for ins in md.disasm(code, va):
            op = ins.bytes
            if ins.mnemonic == "call" and op[0] == 0xE8:
                calls.add(struct.unpack_from("<i", op, 1)[0] + ins.address + 5)
            elif ins.mnemonic in ("call", "jmp") and op[:2] == b"\xff\x15" or (ins.mnemonic == "jmp" and op[:2] == b"\xff\x25"):
                tgt = struct.unpack_from("<I", op, 2)[0]
                if tgt in imports:
                    d = import_dll(imports[tgt])
                    dlls.add(d)
                    if ins.mnemonic == "jmp" and op[:2] == b"\xff\x25" and ins.address == va:
                        thunk_to_dll[va] = d
            elif ins.mnemonic == "call":
                indirect += 1
        info[va] = dict(size=end - va, calls=calls, dlls=dlls, indirect=indirect)
    # a call to an import thunk counts as a call to that DLL
    for va, d in info.items():
        for c in list(d["calls"]):
            if c in thunk_to_dll:
                d["dlls"].add(thunk_to_dll[c])
    callers = collections.Counter(c for d in info.values() for c in d["calls"] if c in info)
    # transitive reach, by fixed point over the direct call graph
    reach = {va: {PLATFORM_DLLS.get(x, x.lower()) for x in d["dlls"]} for va, d in info.items()}
    changed = True
    while changed:
        changed = False
        for va, d in info.items():
            before = len(reach[va])
            for c in d["calls"]:
                if c in reach:
                    reach[va] |= reach[c]
            changed |= len(reach[va]) != before
    rows = []
    for va, r in funcs:
        d = info[va]
        lib = r["library"]
        direct = sorted({PLATFORM_DLLS.get(x, x.lower()) for x in d["dlls"]})
        plat = sorted((reach[va] - {"system"}) | ({PLATFORM_LIBS[lib]} if lib in PLATFORM_LIBS else set()))
        rows.append(dict(va=f"{va:08x}", size=d["size"], library=lib, object=r["object"], name=r["name"], demangled=r["demangled"],
                         callees=len([c for c in d["calls"] if c in info]), callers=callers[va], indirect_calls=d["indirect"],
                         direct_apis=" ".join(direct), reaches=" ".join(sorted(reach[va])), platform=" ".join(plat),
                         leaf=int(not d["calls"] and not d["indirect"])))
    out = OUT / "inventory.csv"
    with out.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    # summary
    game = [r for r in rows if r["library"] not in ("crt",) and not r["object"].upper().endswith(".DLL")]
    print(f"{len(rows):,} functions in .text; {len(game):,} game functions ({sum(r['size'] for r in game):,} bytes) -> {out}")
    by = collections.defaultdict(lambda: [0, 0, 0, 0])
    for r in game:
        b = by[r["library"]]
        b[0] += 1; b[1] += r["size"]
        if not r["platform"]:
            b[2] += 1; b[3] += r["size"]
    print(f"\n{'library':10s} {'funcs':>6s} {'bytes':>9s}   {'no platform reach':>18s}")
    for lib, (n, sz, pn, psz) in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print(f"{lib:10s} {n:6d} {sz:9,}   {pn:6d} fns {100 * psz / max(sz, 1):5.0f}% of bytes")
    tiny = sum(1 for r in game if r["size"] <= 16)
    print(f"\ntiny functions (<= 16 bytes: thunks, $E initialisers, one-liners): {tiny:,}")
    print("direct API users by DLL:", dict(collections.Counter(x for r in game for x in r["direct_apis"].split())))


if __name__ == "__main__":
    main()
