"""Generate the M3 harness's tables from the v1.0 race.exe, the map and the recovered types.

    python tools/gen_port_tables.py

Writes these files into hook/ (derived only from addresses, instruction lengths, hashes and names -- no game
code: a trampoline copies the live bytes of the player's own race.exe):

  prologues.inc     for every address hook/*.cpp hooks with PORT_FN / PORT_FN_BUILDS / detour: the length of
                    its first whole instructions (at least the 5 bytes a jmp overwrites), so a trampoline can
                    run the original, and an FNV-1a hash of them to check the live bytes against. A call/jmp
                    rel32 among them is marked for relocation; a short branch, or a jump from inside the
                    function back into those bytes, is refused.
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
    pat = re.compile(r"\b(?:PORT_FN|PORT_FN_GL|PORT_FN_AUDIO|PORT_FN_BUILDS|detour)\(\s*(0x[0-9a-fA-F]+)")
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
# A rewrite reproduces the STOCK v1.0 function. If the installed race.exe has that function patched,
# replacing it could silently undo the patch. So for each rewritten function this records a fingerprint of its
# code and of every read-only constant it reads (.rdata operands, less LIVE_CONSTS); the DLL recomputes it
# before patching anything and keeps a mismatching function original -- unless it matches one of vrmod's
# patches the rewrites take over (VRMOD_PATCHES, VRMOD_MASKS below: every patch vrmod makes to v1.0 race.exe).
# {address, code bytes, hash, count of constants, their addresses and widths}.
RDATA = (0x4DB000, 0x4E1000)


def fnv(data: bytes, h: int = 2166136261) -> int:
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def port_fn_addresses() -> list[int]:
    pat = re.compile(r"\bPORT_FN(?:_BUILDS|_GL|_AUDIO)?\(\s*(0x[0-9a-fA-F]+)")
    found = set()
    for f in sorted(HOOK.glob("*.cpp")):
        for m in pat.finditer(f.read_text(encoding="utf-8")):
            found.add(int(m.group(1), 16))
    return sorted(found)


# ---- vrmod's patches inside rewritten functions (viper-mod-manager vrmod/*.py) ----------------------------------
# A rewrite replaces an installed function that isn't stock only where it reproduces what vrmod's patch does (or fixes
# the same bug), so a vrmod race.exe runs every function as a rewrite with the same behaviour as today. Two ways:
#
# VRMOD_PATCHES: vrmod's exact bytes, as edits to the stock function ({va: [(what, [(offset, hex bytes), ...]), ...]});
# each gives one more accepted fingerprint (with the function's VRMOD_MASKS bytes as zeros, if it has any).
VRMOD_PATCHES = {
    # enginefix.py, always on: the rewrites carry both fixes (docs/FIXES.md, "Obstacles", "The AI crash")
    0x0043D3C0: [("enginefix: obstacle wake", [(0x1D, "909090")])],     # Obstacle::Reset: fall through into Perturb
    0x00421840: [("enginefix: the AI bead guard", [(0x09,               # IdealLine::advance_bead
        "c744242cffffffff31db837f040075098b472c894704895f08" + "90" * 7)])],
    # vrampatch.py, always on: the video-memory add NOP'd; the rewrite leaves out an add that would wrap (gx_dx.cpp)
    0x00455130: [("vrampatch", [(0x26, "90" * 8)])],
    # modassert.py: unsafe_check a bare ret; the rewrite's fix never panics (krn_core.cpp). It is the function's first
    # byte: port.cpp's live_prologue takes the trampoline's copy of it as it is (the ret runs, as vrmod's function does)
    0x00415020: [("modassert", [(0x00, "c3")])],
    # headon.py (opt-in "Turn off the AI's head-on panic"): AICar::headon_panic's first byte (sub esp, 4) a bare ret. A
    # behaviour, not a fix: the rewrite returns at once when the stock check found it (port_vrmod_has, phys_aicar2.cpp)
    # and does the stock panic otherwise; as with modassert, the trampoline's copy of the first byte is vrmod's ret
    0x004311D0: [("headon", [(0x00, "c3")])],
    # aspectfix.py, always on: Hor+ at widescreen DirectDraw modes; the rewrite does the same (gx_dd.cpp), reading R0 and
    # -0.5 through the code's own two addresses (VRMOD_MASKS: any addresses)
    0x0045EC20: [("aspectfix", [(0x3F,
        "da742414d9c0d90594024e00d8d1dfe09e7604ddd9eb02ddd8d9542420d8c0d8f1ddd9d9542424d80d54b04d00d95c241cc744243000"
        "00803fd9442420d8c0909090")])],
    # tablefix.py + needlefix.py (entries 0x800), always on: the triangle's edge tables doubled and both row tests bound
    # by them; the rewrites fix the same overrun with tables of 2048 rows (gx_2d.cpp). gxTriangle's fill test jumps to a
    # stub in .text slack (0x4da490), outside every function: data to the port, left as it is
    0x00450760: [("tablefix + needlefix", [(0x02, "40"), (0x21, "40"), (0x28, "40"), (0x2F, "40"), (0x36, "40"),
                                          (0x44, "40"), (0x52, "40"), (0x62, "20"), (0x71, "40"), (0x81, "20"),
                                          (0x8C, "40"), (0x98, "40"), (0xA8, "20"), (0xB3, "40"), (0xC5, "40"),
                                          (0xD5, "20"), (0xE0, "40"), (0xEC, "40"), (0xFC, "20"), (0x107, "40"),
                                          (0x144, "40"), (0x14B, "e9e09b08009090909090"), (0x15D, "20"),
                                          (0x195, "40")])],
    0x00450900: [("needlefix", [(0x8C, "81fb0008000090907d")])],
}
# VRMOD_MASKS: bytes vrmod sets to the player's values, which the rewrite reads from the installed race.exe's own
# instructions ({va: (vrmod's patch, [(offset, bytes), ...])}). Taken as zeros in the vrmod fingerprints, and the DLL zeroes the same
# bytes (stock.inc's VP_STOCK_MASKS: {address, offset, bytes}). A function with masks but no VRMOD_PATCHES entry is
# accepted as stock with any values there; one with both, only as each patch with any values there.
VRMOD_MASKS = {
    # carlist.py: the Hacks screen's Vehicle list's push h / push w (imm8) / push y / push x (menu_options.cpp)
    0x00486230: ("carlist", [(0x130, 4), (0x135, 1), (0x137, 4), (0x13C, 4)]),
    # resolution.py: the four menu modes' widths and heights (gx_tex.cpp, gx_dx.cpp: vrmod_operand)
    0x0044DF20: ("resolution", [(o, 4) for o in (0x4A, 0x54, 0x62, 0x6C, 0x78, 0x82, 0x8E, 0x98)]),     # gxSetMode: mov [gxScreenWid/Hit]
    0x0044E040: ("resolution", [(o, 4) for o in (0x4D, 0x57, 0x65, 0x6F, 0x7B, 0x85, 0x91, 0x9B)]),     # gxChangeMode: the same
    0x00454B50: ("resolution", [(o, 4) for o in (0x26, 0x2B, 0x39, 0x3E, 0x4C, 0x51, 0x5F, 0x64)]),     # set_mode: mov eax, w / mov ecx, h
    0x00455330: ("resolution", [(o, 4) for o in (0x29, 0x32, 0x49, 0x52, 0x69, 0x72, 0x89, 0x92)]),     # mode_callback: cmp ecx, w / [eax+8], h
    # hornball.py: create_ball's mass and collision radius (wld_world.cpp: vrmod_operand)
    0x004636A0: ("hornball", [(0x52, 4), (0x60, 4)]),
    # vertexbuffer.py (opt-in --max-verts): mr_model_begin's push of the lit-vertex buffer's size, up to 32,768 vertices;
    # the rewrite always makes it that big (gx_model.cpp, port.h VP_LIT_BUF_BYTES), so the value isn't read
    0x004556F0: ("vertexbuffer", [(0x7B, 4)]),
    # aspectfix.py: the addresses of R0 (fld dword [R0]) and of -0.5 (fmul dword [-0.5]) in its code (gx_dd.cpp)
    0x0045EC20: ("aspectfix", [(0x47, 4), (0x68, 4)]),
}
# .rdata the rewrites read at run time, whatever it holds: left out of every fingerprint. hornball.py's four Ball::Throw
# tunings (phys_dyno.cpp): the cooldown, the throw speed, the spawn's distance ahead and height; nothing else reads them.
LIVE_CONSTS = {0x004DC39C, 0x004DC3A4, 0x004DC3A8, 0x004DC3AC}


def masked(code: bytes, runs) -> bytes:
    b = bytearray(code)
    for at, n in runs:
        b[at:at + n] = bytes(n)
    return bytes(b)


def patched(code: bytes, edits) -> bytes:
    b = bytearray(code)
    for at, hx in edits:
        e = bytes.fromhex(hx)
        assert at + len(e) <= len(b), (at, hx)
        b[at:at + len(e)] = e
    return bytes(b)


# masked values a rewrite doesn't read through vrmod_operand (port.h), and why
VRMOD_MASKS_NOT_READ = {
    0x00486230: "menu_options.cpp's vehicle_list_geometry reads them, with its own check of the opcodes",
    0x004556F0: "the rewrite allocates for 32,768 vertices, past any value vertexbuffer.py writes",
}


def check_vrmod_operands():
    """Every masked operand is read by its rewrite with vrmod_operand(address), and every vrmod_operand names one."""
    src = "".join(f.read_text(encoding="utf-8") for f in sorted(HOOK.glob("*.cpp")))
    lits = {int(m, 16) for m in re.findall(r"\bvrmod_operand(?:_at)?\(\s*(0x[0-9a-fA-F]+)\s*\)", src)}
    want = {va + at for va, (_w, runs) in VRMOD_MASKS.items() if va not in VRMOD_MASKS_NOT_READ for at, n in runs if n == 4}
    errs = [f"the rewrite of the function holding {a:08x} doesn't read vrmod's operand there with vrmod_operand"
            for a in sorted(want - lits)]
    errs += [f"vrmod_operand({a:08x}) isn't an operand VRMOD_MASKS names" for a in sorted(lits - want)]
    if errs:
        raise SystemExit(chr(10).join(errs))


def write_stock(exe, secs, sizes):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    rows, all_consts, masks, variants = [], [], [], []
    addrs = port_fn_addresses()
    for va in list(VRMOD_PATCHES) + list(VRMOD_MASKS):
        if va not in addrs:
            raise SystemExit(f"gen_port_tables: VRMOD_PATCHES / VRMOD_MASKS name {va:08x}, which no PORT_FN rewrites")
    for va in addrs:
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
                if RDATA[0] <= a < RDATA[1] and (a, w) not in consts and a not in LIVE_CONSTS:
                    consts.append((a, w))
        consts.sort()

        def fp(c: bytes) -> int:
            h = fnv(c)
            for a, w in consts:
                fo = file_off(secs, a)
                h = fnv(exe[fo:fo + w], h)
            return h

        hc = fnv(code)
        h = fp(code)
        mwhat, runs = VRMOD_MASKS.get(va, ("", []))
        for at, n in runs:
            masks.append(f"    {{0x{va:08x}, 0x{at:x}, {n}}},")
        if runs and va not in VRMOD_PATCHES:
            variants.append(f'    {{0x{va:08x}, 0x{fp(masked(code, runs)):08x}, "{mwhat}"}},')
        for what, edits in VRMOD_PATCHES.get(va, []):
            variants.append(f'    {{0x{va:08x}, 0x{fp(masked(patched(code, edits), runs)):08x}, "{what}"}},')
        rows.append(f"    {{0x{va:08x}, {size}, 0x{hc:08x}, 0x{h:08x}, {len(all_consts)}, {len(consts)}}},")
        all_consts += consts                      # every constant: the DLL must hash exactly what this did
    (HOOK / "stock.inc").write_text(
        "// generated by tools/gen_port_tables.py: stock v1.0 fingerprints of every rewritten function\n"
        "#ifdef VP_STOCK\n"
        "// {address, code bytes, FNV-1a of the code then each constant, first constant, constants}\n"
        + "\n".join(rows) + "\n#endif\n#ifdef VP_STOCK_CONSTS\n// {address, width}\n"
        + "\n".join(f"    {{0x{a:08x}, {w}}}," for a, w in all_consts) + "\n#endif\n"
        + "#ifdef VP_STOCK_MASKS\n// {address, offset, bytes}: zeros in the vrmod fingerprints (VRMOD_MASKS)\n"
        + "\n".join(masks) + "\n#endif\n"
        + "#ifdef VP_STOCK_VARIANTS\n// {address, fingerprint, what}: vrmod's patched functions the rewrites replace too "
          "(VRMOD_PATCHES, VRMOD_MASKS)\n"
        + "\n".join(variants) + "\n#endif\n", encoding="utf-8")
    return len(rows)


def write_prologues(exe, secs, sizes):
    """{address, length, rel32 offset, hash, hash with M1's operand as zeros}. port.cpp's live_prologue hashes the live
    bytes the same way, with every byte M1 patched (port_note_m1) as a zero, and compares the first hash when M1 patched
    none of them, the second when it did. M1 writes a field's 4 bytes all or nothing, so a prologue holding at most one
    of M1's operands has just those two states: the same test as comparing every byte M1 didn't patch."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    ops = m1_operands()
    rows = []
    for va in hooked_addresses():
        code, rel = prologue(exe, secs, md, va, sizes.get(va, 64))
        if len(code) > 16:
            raise SystemExit(f"{va:08x}: a prologue of {len(code)} bytes (a trampoline holds 16)")
        mine = [op for op in ops if op < va + len(code) and va < op + 4]
        if len(mine) > 1:
            raise SystemExit(f"{va:08x}: more than one of M1's operands in its first {len(code)} bytes")
        runs = [(max(op - va, 0), min(op + 4, va + len(code)) - max(op, va)) for op in mine]
        rows.append(f"    {{0x{va:08x}, {len(code)}, {rel}, 0x{fnv(code):08x}, 0x{fnv(masked(code, runs)):08x}}},")
    (HOOK / "prologues.inc").write_text(
        "// generated by tools/gen_port_tables.py: v1.0's first instructions at every hooked address, as hashes\n"
        "// {address, bytes, offset of a rel32 to relocate (-1 none), FNV-1a of the bytes, the same with M1's operand\n"
        "// (if one lies in them) as zeros}\n" + "\n".join(rows) + "\n",
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
# offset, ...} fields, the texture table's (texture_table_fields.inc) and the options and language tables'
# (res_table_fields.inc: the open-file table's too). A rewrite of a function holding one must read that operand with m1_operand(its
# address), or it would undo the lift (docs/PORTING.md 12).
def m1_operands() -> list[int]:
    """The v1.0 address of every 4-byte operand M1's patch_fields may write (and note with port_note_m1)."""
    src = "".join((HOOK / f).read_text(encoding="utf-8") for f in ("viperport.cpp", "texture_table_fields.inc", "res_table_fields.inc"))
    return sorted({int(at, 16) + int(off) for at, off in re.findall(r"\{\s*(0x[0-9a-fA-F]{6,8}),\s*(\d+),", src)})


def check_m1_operands(sizes):
    operands = m1_operands()
    texts = {f: f.read_text(encoding="utf-8") for f in HOOK.glob("*.cpp") if f.name != "viperport.cpp"}
    pat = re.compile(r"\bPORT_FN(?:_BUILDS|_GL|_AUDIO)?\(\s*(0x[0-9a-fA-F]+)")
    missing = []
    for f, t in texts.items():
        for m in pat.finditer(t):
            va = int(m.group(1), 16)
            for op in operands:
                if not va <= op < va + sizes.get(va, 0):
                    continue
                if op < va + 5:                    # the hook's jmp overwrites it: port.h's m1_operand_hooked
                    if not re.search(rf"m1_operand_hooked\(0x0*{op:x},", t, re.I):
                        missing.append(f"{f.name}: M1's operand at {op:08x} is in the bytes the hook of {va:08x} overwrites: "
                                       f"read it with m1_operand_hooked")
                elif not re.search(rf"m1_operand\(0x0*{op:x}\)", t, re.I):
                    missing.append(f"{f.name}: the rewrite of {va:08x} doesn't read M1's operand at {op:08x}")
    if missing:
        raise SystemExit(chr(10).join(missing))


def main():
    exe = (OUT / "race_v10.exe").read_bytes()
    secs = sections(exe)
    sizes = {int(r["va"], 16): int(r["size"]) for r in csv.DictReader(open(OUT / "inventory.csv", encoding="utf-8"))}
    types = json.loads((OUT / "types.json").read_text(encoding="utf-8"))
    check_m1_operands(sizes)
    check_vrmod_operands()
    n = write_prologues(exe, secs, sizes)
    ns = write_stock(exe, secs, sizes)
    c, f = write_layout(types)
    g, gb = write_globals(exe, secs)
    print(f"stock.inc: {ns} fingerprints; prologues.inc: {n} hooked addresses; state_layout.inc: {c} classes, {f} named fields; "
          f"globals_phys.inc: {g} spans, {gb} bytes")


if __name__ == "__main__":
    sys.exit(main())
