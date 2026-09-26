"""Every address the hook DLL uses, carried from v1.0 race.exe to another build.

Reads the v1.0 addresses straight out of hook/viperport.cpp and hook/texture_table_fields.inc: function
entries, globals, and patch sites (a Field's instruction address + operand offset + the value it must
hold). Each is translated through out/map_<build>.json (tools/propagate.py): functions and globals
directly, patch sites by their offset inside the containing function -- then checked against the target
bytes: the instruction there must be the same instruction (same length and opcode bytes), and a patched
field must hold the translated address, or the same constant (a changed constant is reported).

    python tools/port_sites.py <race.exe v1.0> <target> out/map_X.json [out.inc]
"""
import bisect
import csv
import json
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

sys.path.insert(0, str(Path(__file__).resolve().parent))
import match_builds as M  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
md = Cs(CS_ARCH_X86, CS_MODE_32)

# Functions the matcher can't place because a build changed them, found by hand-written signatures:
# hex bytes, ?? for any byte, &"text" for the address of that string in the build being searched, and |
# where the function starts when the pattern begins before it. Each must hit exactly once in the code.
SIGNATURES = {
    # collide_phobs: the release builds lost the profiler calls, so only the loops are left
    0x427120: ['83 EC 10 53 56 57 33 F6 55 39 35 ?? ?? ?? ?? 7E ?? 33 FF A1 ?? ?? ?? ?? 8B 1C 38 85 DB 74 ?? '
               '8B 03 8B CB FF 50 2C'],
    # end_deferred_surfs: v1.1 swapped two registers
    0x457290: ['53 56 57 33 DB 55 88 1D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8D ?? 9D ?? ?? ?? ?? 83 ?? 00 74'],
    # KeyQueueChar and KeyQueueMetaChar: too small alone, so each is found with the other beside it
    0x413dd0: ['|8B 44 24 08 8B 4C 24 04 50 51 E8 ?? ?? ?? ?? 83 C4 08 C3 8D 9B 00 00 00 00 8D A4 24 00 00 00 00 '
               '56 8B 74 24 08 56 E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 ?? 66 A1'],
    0x413df0: ['8B 44 24 08 8B 4C 24 04 50 51 E8 ?? ?? ?? ?? 83 C4 08 C3 8D 9B 00 00 00 00 8D A4 24 00 00 00 00 '
               '|56 8B 74 24 08 56 E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 ?? 66 A1'],
    # CollisionVolume::GetExtents, the base-class stub: panics "GetExtents not defined"
    0x436080: ['68 &"GetExtents not defined" E8 ?? ?? ?? ?? 83 C4 04 C2 08 00'],
}


def find_signature(img, text, tva, sig):
    parts, start, n = [], 0, 0
    for tok in re.findall(r'&"[^"]*"|\?\?|\||[0-9A-Fa-f]{2}', sig):
        if tok == "|":
            start = n
            continue
        if tok.startswith("&"):
            hits = [mt.start() for mt in re.finditer(re.escape(b"\0" + tok[2:-1].encode() + b"\0"), img)]
            if len(hits) != 1:
                return None
            parts.append(re.escape(struct.pack("<I", file_to_va(img, hits[0] + 1))))
            n += 4
            continue
        parts.append(b"." if tok == "??" else re.escape(bytes([int(tok, 16)])))
        n += 1
    hits = [mt.start() for mt in re.finditer(b"".join(parts), text, re.S)]
    return tva + hits[0] + start if len(hits) == 1 else None


def file_to_va(img, off):
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    n, opt = struct.unpack_from("<H", img, pe + 6)[0], struct.unpack_from("<H", img, pe + 20)[0]
    base = struct.unpack_from("<I", img, pe + 52)[0]
    for k in range(n):
        vs, va, rs, rp = struct.unpack_from("<IIII", img, pe + 24 + opt + 40 * k + 8)
        if rp <= off < rp + rs:
            return base + va + off - rp


def ins_at(text, tva, va):
    return next(md.disasm(text[va - tva: va - tva + 16], va), None)


def main():
    _, sbase, stva, stext = M.load(sys.argv[1])
    dimg, dbase, dtva, dtext = M.load(sys.argv[2])
    m = json.loads(Path(sys.argv[3]).read_text())
    fmap = {int(k, 16): int(v, 16) for k, v in m["functions"].items()}
    dmap = {int(k, 16): int(v, 16) for k, v in m["data"].items()}
    inv = {int(r["va"], 16): r for r in csv.DictReader(open(ROOT / "out" / "inventory.csv", encoding="utf-8"))}
    starts = sorted(inv)
    how = {int(r["va"], 16): r["how"] for r in csv.DictReader(open(Path(sys.argv[3]).with_name(
        Path(sys.argv[3]).name.replace("map_", "match_").replace(".json", ".csv")), encoding="utf-8"))}

    src = "".join(f.read_text() for f in sorted((ROOT / "hook").glob("*.cpp"))) + \
        (ROOT / "hook" / "texture_table_fields.inc").read_text()
    fields = {}
    for a, off, old in re.findall(r"\{\s*(0x[0-9a-f]+),\s*(\d+),\s*(0x[0-9a-f]+|\d+),", src):
        fields[int(a, 16)] = (int(off), int(old, 0))
    lits = {int(x, 16) for x in re.findall(r"0x0{0,2}[45][0-9a-f]{5}\b", src)}
    lits = {v for v in lits if 0x401000 <= v < 0x600000} - set(fields)
    stext_end = stva + len(stext)

    out, bad = {}, 0
    for va, sigs in SIGNATURES.items():
        if va not in fmap:
            hit = next((h for h in (find_signature(dimg, dtext, dtva, sg) for sg in sigs) if h), None)
            if hit:
                fmap[va] = hit
                how[va] = "signature"
    disputed = {int(k, 16): v for k, v in m.get("disputed", {}).items()}

    def fn_of(va):
        i = bisect.bisect_right(starts, va) - 1
        return starts[i] if i >= 0 and va < starts[i] + int(inv[starts[i]]["size"]) else None

    def fn_range(f, table):
        """(start, end) of v1.0 function f in the target: its own match, or the gap its placed neighbours leave."""
        if f in table:                            # up to the next function placed in the target
            t = table[f]
            return t, min((v for v in table.values() if v > t), default=t + int(inv[f]["size"])), False
        i = starts.index(f)
        prev = next((s for s in reversed(starts[:i]) if s in table), None)
        nxt = next((s for s in starts[i + 1:] if s in table), None)
        if prev is None or nxt is None:
            return None, None, True
        return table[prev] + int(inv[prev]["size"]) - 16, table[nxt], True

    def insns(text, tva, lo, hi):
        return list(md.disasm(text[lo - tva: hi - tva], lo))

    def site(va, off=None, old=None):
        """(target address of the instruction, of the patched field, a note) -- or (None, None, why)."""
        f = fn_of(va)
        if f is None:
            return None, None, "not inside a known function"
        name = inv[f]["demangled"]
        if f in fmap:
            t = fmap[f] + (va - f)
            a, b = ins_at(stext, stva, va), ins_at(dtext, dtva, t)
            if a and b and a.size == b.size and a.mnemonic == b.mnemonic and a.bytes[:1] == b.bytes[:1]:
                return t, None if off is None else t + off, name + ("" if f in how else " (callee)")
        if old is None:
            return None, None, f"in {name}: instruction differs"
        # the compilers differ (register choice, short forms): find the instruction by what it does --
        # same mnemonic, holding the translated address (or the same constant) -- the k-th such in the function
        want = dmap.get(old) if sbase <= old < sbase + 0x300000 else old
        if want is None:
            return None, None, f"in {name}: {old:#x} not mapped"
        mine = [i for i in insns(stext, stva, f, f + int(inv[f]["size"]))
                if i.mnemonic == ins_at(stext, stva, va).mnemonic and struct.pack("<I", old) in i.bytes]
        k = [i.address for i in mine].index(va)
        lo, hi, gap = fn_range(f, fmap)
        if lo is None:
            return None, None, f"in {name}: function not placed, no neighbours"
        theirs = [i for i in insns(dtext, dtva, lo, hi)
                  if i.mnemonic == mine[k].mnemonic and struct.pack("<I", want) in i.bytes]
        if len(theirs) != len(mine):
            return None, None, f"in {name}: {len(mine)} '{mine[k].mnemonic} {old:#x}' in v1.0, {len(theirs)} here"
        b = theirs[k]
        return b.address, b.address + bytes(b.bytes).index(struct.pack("<I", want)),             f"{name} (found by operand{' in the gap' if gap else ''}: {b.mnemonic} {b.op_str})"

    for va in sorted(lits):
        if va in inv:
            t = fmap.get(va)
            note = inv[va]["demangled"] + (f" [{how.get(va, 'callee')}]" if t else "")
        elif stva <= va < stext_end:
            t, _, note = site(va)
            if t is not None:
                note = "site in " + note
        else:
            t, note = dmap.get(va), "data"
            if t is None:
                near = min(dmap, key=lambda d: abs(d - va))
                if abs(near - va) <= 64:
                    t, note = dmap[near] + (va - near), f"data (placed by its neighbour {near:08x})"
        if va in disputed:
            if len(disputed[va]) == 1:            # the code that calls it agrees on another copy: believe it
                t = int(next(iter(disputed[va])), 16)
                note += f"  (the signature hit a twin; its callers use {t:08x})"
            else:
                note += f"  DISPUTED by references: {disputed[va]}"
                bad += 1
        # a function's entry carries its first four bytes, so the DLL can tell a hex-edited copy
        first = struct.unpack_from("<I", dtext, t - dtva)[0] if t is not None and va in inv else None
        out[va] = (t, first)
        bad += t is None
        print(f"  {va:08x} -> {t and f'{t:08x}' or '--------'}  {note}")

    print("patch sites:")
    for va, (off, old) in sorted(fields.items()):
        t, at, note = site(va, off, old)
        if t is None:
            bad += 1
            print(f"  {va:08x} -> --------  {note}")
            continue
        at = at if at is not None else t + off
        now = struct.unpack_from("<I", dtext, at - dtva)[0]
        if sbase <= old < sbase + 0x300000:
            want = dmap.get(old)
            if want is None:                      # not referenced by matched code: a mapped neighbour, if one is close
                near = min(dmap, key=lambda d: abs(d - old))
                if abs(near - old) <= 64:
                    want = dmap[near] + (old - near)
                    note += f" (placed by its neighbour {near:08x})"
            ok = want == now
            verdict = "ok" if ok else f"MISMATCH: holds {now:08x}, map says {want and f'{want:08x}'}"
        else:
            ok = True
            verdict = "ok" if now == old else f"CONSTANT CHANGED {old:#x} -> {now:#x}"
        bad += not ok
        out[va] = (at, now)
        print(f"  {va:08x}+{off} -> {at:08x}  {old:08x} -> {now:08x}  {verdict:10s} {note}")
    print(f"{len(out)} addresses, {bad} problems")
    if len(sys.argv) > 4:
        with open(sys.argv[4], "w") as fh:
            fh.write(f"// generated by tools/port_sites.py from {Path(sys.argv[2]).name} -- {{v1.0 address, this build's (for a patch site: of the field itself), value there: a patched field's, or a function's first 4 bytes}}\n")
            for va, (t, now) in sorted(out.items()):
                if t is not None:
                    fh.write(f"    {{0x{va:06x}, 0x{t:06x}, 0x{(now or 0):x}}},\n")


if __name__ == "__main__":
    main()
