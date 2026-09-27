"""Generate the M3 harness's tables from the v1.0 race.exe, the map and the recovered types.

    python tools/gen_port_tables.py

Writes three files into hook/ (derived only from addresses, instruction bytes and names -- no game code
beyond each hooked function's first few instructions, which the trampolines must reproduce):

  prologues.inc     for every address hook/*.cpp hooks with PORT_FN / PORT_FN_BUILDS / detour: its first
                    whole instructions (at least the 5 bytes a jmp overwrites), so a trampoline can run
                    the original. A call/jmp rel32 among them is marked for relocation; a short branch, or
                    a jump from inside the function back into those bytes, is refused.
  state_layout.inc  every physics-object class (PhobRoot and what derives from it): vtable, size, name;
                    and the named fields of each, flattened through bases and embedded parts ("wheels[2].
                    load"), so logs can name the byte where two runs part.
  globals_phys.inc  the .data owned by the physics and AI modules (and the random-number generator), by
                    symbol: what a shadow check saves, restores and compares besides the footprint.

Needs out/race_v10.exe, out/symbols.csv, out/inventory.csv, out/types.json.
"""
from __future__ import annotations

import csv
import json
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
HOOK = ROOT / "hook"
BASE = 0x400000
DATA_END = 0x4E1000 + 0xF5F2C              # .data's virtual end (bss included)


def sections(exe: bytes):
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    n = struct.unpack_from("<H", exe, pe + 6)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    return [struct.unpack_from("<8sIIII", exe, pe + 24 + opt + i * 40) for i in range(n)]


def file_off(secs, va: int) -> int:
    for _name, vs, vaddr, _rs, ro in secs:
        if vaddr <= va - BASE < vaddr + vs:
            return va - BASE - vaddr + ro
    raise ValueError(f"{va:08x} is in no section")


# ---- prologues ------------------------------------------------------------------------------------------
def hooked_addresses() -> list[int]:
    pat = re.compile(r"\b(?:PORT_FN|PORT_FN_BUILDS|detour)\(\s*(0x[0-9a-fA-F]+)")
    found = set()
    for f in sorted(HOOK.glob("*.cpp")):
        for m in pat.finditer(f.read_text(encoding="utf-8")):
            found.add(int(m.group(1), 16))
    return sorted(found)


def prologue(exe, secs, md, va: int, size: int) -> tuple[bytes, int]:
    """The first whole instructions covering 5 bytes, and the offset of a rel32 to relocate (-1: none)."""
    off = file_off(secs, va)
    code, rel = b"", -1
    for ins in md.disasm(exe[off:off + 32], va):
        b = bytes(ins.bytes)
        if b[0] in (0xE8, 0xE9) and len(b) == 5:
            rel = len(code) + 1
        elif b[0] in (0xEB, 0xE3) or 0x70 <= b[0] <= 0x7F or (b[0] == 0x0F and 0x80 <= b[1] <= 0x8F):
            # can't be moved into a trampoline: the rewrite can replace it (new), but not be shadowed
            print(f"  note: {va:08x} branches within its first 5 bytes ({ins.mnemonic} {ins.op_str}): new only, no shadow")
            return bytes(exe[off:off + 5]), -2
        elif ins.mnemonic in ("ret", "retn") and len(code) + len(b) < 5:
            # a function shorter than the jump: fine if what follows is the linker's alignment padding
            # (int3 / nop), which nothing executes -- the trampoline copies the ret and the padding
            end = len(code) + len(b)
            rest = exe[off + end:off + size]           # to the next function: MSVC's padding is
            ok = len(rest) >= 5 - end                   # int3, nop, lea r,[r], add eax,0
            for pi in md.disasm(rest, va + end):
                lea_self = pi.mnemonic == "lea" and re.fullmatch(r"(\w+), \[\1(?: \+ 0)?\]", pi.op_str)
                if not (pi.mnemonic in ("int3", "nop") or lea_self or (pi.mnemonic == "add" and pi.op_str == "eax, 0")):
                    ok = False
            if ok:
                code += b + bytes(exe[off + end:off + 5])
                break
            raise SystemExit(f"{va:08x}: returns within its first 5 bytes -- too short to hook")
        code += b
        if len(code) >= 5:
            break
    # nothing in the function may jump back into the bytes being moved
    body = exe[off:off + size]
    for ins in md.disasm(body, va):
        if ins.mnemonic.startswith("j") or ins.mnemonic in ("loop", "call"):
            try:
                t = int(ins.op_str, 16)
            except ValueError:
                continue
            if va < t < va + len(code):
                # a loop back into the moved bytes: the trampoline can't run it, the rewrite can replace it
                print(f"  note: {va:08x}: {ins.address:08x} jumps into the first {len(code)} bytes: new only, no shadow")
                return bytes(exe[off:off + 5]), -2
    return code, rel


# ---- stock fingerprints ---------------------------------------------------------------------------------
# A rewrite reproduces the STOCK v1.0 function. If the installed race.exe has that function patched --
# vrmod's engine fixes, hornball, the AI crash fix -- replacing it would silently undo the patch. So for
# each rewritten function this records a fingerprint of its code and of every read-only constant it reads
# (.rdata operands); the DLL recomputes it before patching anything and keeps a mismatching function
# original. {address, code bytes, hash, count of constants, their addresses and widths}.
RDATA = (0x4DB000, 0x4E1000)


def fnv(data: bytes, h: int = 2166136261) -> int:
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def port_fn_addresses() -> list[int]:
    pat = re.compile(r"\bPORT_FN(?:_BUILDS)?\(\s*(0x[0-9a-fA-F]+)")
    found = set()
    for f in sorted(HOOK.glob("*.cpp")):
        for m in pat.finditer(f.read_text(encoding="utf-8")):
            found.add(int(m.group(1), 16))
    return sorted(found)


# vrmod's two always-on engine fixes (viper-mod-manager vrmod/enginefix.py), as edits to the stock
# function's bytes. The rewrites carry both fixes, so a function with exactly this patch is replaced too.
VRMOD_FIXES = {
    0x0043D3C0: (0x1D, bytes.fromhex("909090")),              # Obstacle::Reset: fall through into Perturb
    0x00421840: (0x09, bytes.fromhex(                          # IdealLine::advance_bead: the bead guard
        "c744242cffffffff31db837f040075098b472c894704895f08" + "90" * 7)),
}


def write_stock(exe, secs, sizes):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    rows, all_consts = [], []
    for va in port_fn_addresses():
        size = sizes.get(va, 0)
        if size <= 0:
            continue
        off = file_off(secs, va)
        code = exe[off:off + size]
        consts = []
        for ins in md.disasm(code, va):
            for m in re.finditer(r"(byte|word|dword|qword|tbyte|xword) ptr \[(0x[0-9a-f]+)\]", ins.op_str):
                a = int(m.group(2), 16)
                w = {"byte": 1, "word": 2, "dword": 4, "qword": 8, "tbyte": 10, "xword": 10}[m.group(1)]
                if RDATA[0] <= a < RDATA[1] and (a, w) not in consts:
                    consts.append((a, w))
        consts.sort()
        hc = h = fnv(code)
        for a, w in consts:
            fo = file_off(secs, a)
            h = fnv(exe[fo:fo + w], h)
        hv = 0
        if va in VRMOD_FIXES:                     # the same fingerprint over the vrmod-fixed bytes
            at, patch = VRMOD_FIXES[va]
            hv = fnv(code[:at] + patch + code[at + len(patch):])
            for a, w in consts:
                fo = file_off(secs, a)
                hv = fnv(exe[fo:fo + w], hv)
        rows.append(f"    {{0x{va:08x}, {size}, 0x{hc:08x}, 0x{h:08x}, {len(all_consts)}, {len(consts)}, 0x{hv:08x}}},")
        all_consts += consts                      # every constant: the DLL must hash exactly what this did
    (HOOK / "stock.inc").write_text(
        "// generated by tools/gen_port_tables.py: stock v1.0 fingerprints of every rewritten function\n"
        "#ifdef VP_STOCK\n"
        "// {address, code bytes, FNV-1a of the code then each constant, first constant, constants,\n"
        "//  the same fingerprint with vrmod's engine fix applied (0: none)}\n"
        + "\n".join(rows) + "\n#endif\n#ifdef VP_STOCK_CONSTS\n// {address, width}\n"
        + "\n".join(f"    {{0x{a:08x}, {w}}}," for a, w in all_consts) + "\n#endif\n", encoding="utf-8")
    return len(rows)


def write_prologues(exe, secs, sizes):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    rows = []
    for va in hooked_addresses():
        code, rel = prologue(exe, secs, md, va, sizes.get(va, 64))
        rows.append(f"    {{0x{va:08x}, {len(code)}, {rel}, {{{', '.join(f'0x{b:02x}' for b in code)}}}}},")
    (HOOK / "prologues.inc").write_text(
        "// generated by tools/gen_port_tables.py: v1.0's first instructions at every hooked address\n"
        "// {address, bytes, offset of a rel32 to relocate (-1 none), the bytes}\n" + "\n".join(rows) + "\n",
        encoding="utf-8")
    return len(rows)


# ---- class layouts and field names ---------------------------------------------------------------------
KNOWN_SIZE = {"P3DBase": 12, "P3D": 12, "Point": 12, "Frame": 48, "Matrix": 36, "Quat": 16}


def type_size(types, tname: str | None) -> int:
    if not tname:
        return 4
    if tname.startswith("ptr") or tname in ("float", "int", "uint"):
        return 4
    if tname in ("bool", "byte", "u1", "char"):
        return 1
    if tname == "u2":
        return 2
    m = re.fullmatch(r"arr:(.+):(\d+)", tname)
    if m:
        return type_size(types, m.group(1)) * int(m.group(2))
    if tname.startswith("struct:"):
        n = tname[7:]
        return KNOWN_SIZE.get(n) or types.get(n, {}).get("size") or 4
    return 4


def flat_fields(types, cls: str, prefix: str = "", base_off: int = 0, depth: int = 0) -> list[tuple[int, int, str]]:
    """(offset, size, name) for every named field of cls: bases first, embedded parts expanded."""
    if depth > 6 or cls not in types:
        return []
    c = types[cls]
    out = []
    if c.get("base"):
        out += flat_fields(types, c["base"], prefix, base_off, depth + 1)
    for f in c.get("fields", []):
        name, t = f.get("name"), f.get("type")
        if not name:
            continue
        off = base_off + f["offset"]
        emb = f.get("embedded")
        m = re.fullmatch(r"arr:struct:([\w:]+):(\d+)", t or "")
        if emb and m and m.group(1) in types and types[m.group(1)].get("size"):
            esz = types[m.group(1)]["size"]
            for i in range(int(m.group(2))):
                out += flat_fields(types, m.group(1), f"{prefix}{name}[{i}].", off + i * esz, depth + 1)
            continue
        if t and t.startswith("struct:") and t[7:] in types and types[t[7:]].get("fields") and t[7:] not in KNOWN_SIZE:
            sub = flat_fields(types, t[7:], f"{prefix}{name}.", off, depth + 1)
            if sub:
                out += sub
                continue
        out.append((off, type_size(types, t), prefix + name))
    return out


def derives_from(types, cls: str, root: str) -> bool:
    seen = 0
    while cls and seen < 12:
        if cls == root:
            return True
        cls = types.get(cls, {}).get("base")
        seen += 1
    return False


def write_layout(types):
    classes = [n for n, c in types.items() if c.get("vtable") and c.get("size") and
               (derives_from(types, n, "PhobRoot") or derives_from(types, n, "CollisionVolume"))]
    classes.sort(key=lambda n: int(types[n]["vtable"]["va"], 16))
    lines = ["// generated by tools/gen_port_tables.py: physics-object and collision-volume classes and their named fields",
             "#ifdef VP_CLASSES", "// {vtable, size, name, first field, field count}"]
    fields, rows = [], []
    for n in classes:
        ff = sorted(set(flat_fields(types, n)))
        rows.append(f'    {{0x{int(types[n]["vtable"]["va"], 16):08x}, {types[n]["size"]}, "{n}", {len(fields)}, {len(ff)}}},')
        fields += ff
    lines += rows + ["#endif", "#ifdef VP_FIELDS", "// {offset, size, name}"]
    lines += [f'    {{{o}, {s}, "{nm}"}},' for o, s, nm in fields]
    lines += ["#endif"]
    (HOOK / "state_layout.inc").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(classes), len(fields)


# ---- physics and AI globals --------------------------------------------------------------------------------
# What a shadow check saves, restores and compares besides a function's footprint: the statics of the
# physics and AI object files. The map names only public data, so a symbol's "size up to the next name"
# can swallow other files' statics; instead every .data address the code references is attributed to the
# object file(s) that reference it. The linker lays each file's statics out together, so a run of
# addresses used by one file is that file's block, up to where the next file's begins.
SIM_LIBS = ("physics", "ai")
SIM_OBJECTS = ()           # (random.obj is left out: random numbers are a check's inputs, fed to the rewrite)
# Physics-library files that are the player's input, run on the MAIN thread: not simulation state. The
# physics reads the controls only through DriverGet* (inputs, which the recorder records) and writes them
# only through DriverSetGear / DriverSetForce (outputs); a check on the physics thread would see the main
# thread's writes land mid-check, and restoring would undo them.
INPUT_OBJECTS = (("physics", "control.obj"), ("physics", "driver.obj"))
# and single buffers the main thread writes inside simulation files
MAIN_THREAD = [
    (0x004ECF8C, 4, "PhysicsReadControls: the physics time it last read the controls"),
    (0x005215D8, 0x00521608 - 0x005215D8, "PhysicsTimeString's text buffer (the HUD clock)"),
    (0x00521608, 1, "physics_paused: set by the pause menu; an input, which the recorder records"),
]


def data_owners(exe, secs, inv) -> dict[int, set[tuple[str, str]]]:
    """.data address -> the (library, object) of every function that references it."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    owners: dict[int, set[tuple[str, str]]] = {}
    for r in inv:
        va, size = int(r["va"], 16), int(r["size"])
        if size < 2:
            continue
        off = file_off(secs, va)
        for ins in md.disasm(exe[off:off + size], va):
            for m in re.findall(r"0x([0-9a-f]{6,8})\b", ins.op_str):
                x = int(m, 16)
                if 0x4E1000 <= x < DATA_END:
                    owners.setdefault(x, set()).add((r["library"], r["object"]))
    return owners


def carve(va: int, end: int) -> list[tuple[int, int]]:
    """[va, end) with the main-thread ranges cut out."""
    spans = [(va, end)]
    for x, n, _why in MAIN_THREAD:
        out = []
        for a, b in spans:
            if x + n <= a or x >= b:
                out.append((a, b))
                continue
            if a < x:
                out.append((a, x))
            if x + n < b:
                out.append((x + n, b))
        spans = out
    return spans


def write_globals(exe, secs):
    inv = list(csv.DictReader(open(OUT / "inventory.csv", encoding="utf-8")))
    owners = data_owners(exe, secs, inv)
    syms = [r for r in csv.DictReader(open(OUT / "symbols.csv", encoding="utf-8")) if r["section"] == ".data"]
    named = {int(r["va"], 16): r["demangled"].split("(")[0].split()[-1].replace('"', "'")[:48] for r in syms}
    # a public symbol belongs to the file that defines it (the map says which), whoever else uses it
    for r in syms:
        va = int(r["va"], 16)
        lib = next((l for l, o in owners.get(va, ()) if o == r["object"]), r["library"])
        owners[va] = {(lib, r["object"])}
    addrs = sorted(owners)

    def sim(o: set) -> bool:
        return bool(o) and all((lib in SIM_LIBS or (lib, obj) in SIM_OBJECTS) and (lib, obj) not in INPUT_OBJECTS
                               for lib, obj in o)

    # runs of addresses owned by one simulation file; a run ends where another file's addresses begin
    blocks = []
    i = 0
    while i < len(addrs):
        o = owners[addrs[i]]
        j = i
        while j + 1 < len(addrs) and owners[addrs[j + 1]] == o:
            j += 1
        end = addrs[j + 1] if j + 1 < len(addrs) else DATA_END
        if sim(o) and len(o) == 1:
            lib, obj = next(iter(o))
            blocks.append((addrs[i], end, f"{obj} statics"))
        i = j + 1
    out, total = [], 0
    for va, end, what in blocks:
        cuts = sorted({va, end} | {n for n in named if va < n < end})
        for a0, a1 in zip(cuts, cuts[1:]):
            name = named.get(a0) or f"{what}"
            for a, b in carve(a0, a1):
                out.append(f'    {{0x{a:08x}, {b - a}, "{name}"}},')
                total += b - a
    (HOOK / "globals_phys.inc").write_text(
        "// generated by tools/gen_port_tables.py: the statics of the physics and AI object files\n"
        "// (the input files and main-thread buffers left out); {address, bytes, name}\n" + "\n".join(out) + "\n",
        encoding="utf-8")
    return len(out), total


# ---- M1's patches inside rewritten functions --------------------------------------------------------------
# M1 (hook/viperport.cpp) lifts limits by patching operands of the original code: {instruction, operand
# offset, ...} fields, and the texture table's (texture_table_fields.inc). A rewrite of a function holding
# one must read that operand with m1_operand(its address), or it would undo the lift (docs/PORTING.md 12).
def check_m1_operands(sizes):
    src = (HOOK / "viperport.cpp").read_text(encoding="utf-8") + (HOOK / "texture_table_fields.inc").read_text(encoding="utf-8")
    operands = sorted({int(at, 16) + int(off) for at, off in re.findall(r"\{\s*(0x[0-9a-fA-F]{6,8}),\s*(\d+),", src)})
    texts = {f: f.read_text(encoding="utf-8") for f in HOOK.glob("*.cpp") if f.name != "viperport.cpp"}
    pat = re.compile(r"PORT_FN(?:_BUILDS)?\(\s*(0x[0-9a-fA-F]+)")
    missing = []
    for f, t in texts.items():
        for m in pat.finditer(t):
            va = int(m.group(1), 16)
            for op in operands:
                if va <= op < va + sizes.get(va, 0) and not re.search(rf"m1_operand\(0x0*{op:x}\)", t, re.I):
                    missing.append(f"{f.name}: the rewrite of {va:08x} doesn't read M1's operand at {op:08x}")
    if missing:
        raise SystemExit(chr(10).join(missing))


def main():
    exe = (OUT / "race_v10.exe").read_bytes()
    secs = sections(exe)
    sizes = {int(r["va"], 16): int(r["size"]) for r in csv.DictReader(open(OUT / "inventory.csv", encoding="utf-8"))}
    types = json.loads((OUT / "types.json").read_text(encoding="utf-8"))
    check_m1_operands(sizes)
    n = write_prologues(exe, secs, sizes)
    ns = write_stock(exe, secs, sizes)
    c, f = write_layout(types)
    g, gb = write_globals(exe, secs)
    print(f"stock.inc: {ns} fingerprints; prologues.inc: {n} hooked addresses; state_layout.inc: {c} classes, {f} named fields; "
          f"globals_phys.inc: {g} spans, {gb} bytes")


if __name__ == "__main__":
    sys.exit(main())
