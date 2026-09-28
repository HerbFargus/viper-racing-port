"""Generate hook/krn_leftover.cpp: the leftover functions of the finished libraries, rewritten mechanically.

    python tools/gen_leftovers.py [--list]              the finished libraries -> hook/krn_leftover.cpp
    python tools/gen_leftovers.py --lib ui [--list]     one stage's library -> its own file (STAGES)
    python tools/gen_leftovers.py --lib menu [--list]

M3 UI stage, step U0. The libraries whose stages are done -- physics, world, gx, ai, kernel, useful, state, sound --
still hold functions no hook/*.cpp registers: the $E static initialisers, empty virtual stubs, compiler-generated
deleting destructors, per-file ASSERT_MSG copies. This disassembles each one from out/race_v10.exe, recognises it by
its EXACT instruction bytes (never by its name), and writes a faithful rewrite plus a literal PORT_FN line for it.
A function whose bytes fit no known shape is refused: the script lists it and writes nothing. The few that fit no
shape are written by hand in hook/krn_leftover_hand.cpp, which, like every other hook/*.cpp, takes its functions out
of the list here.

Shapes (the whole body, to its `ret`, plus the linker's padding):
  $E static initialisers, a sequence of statements ending in `ret` (or a single tail jump):
    jmp T                                    a tail call to rcfunc_is_internal or to another $E of the same file
    mov ecx, C ; jmp T                       a tail call to a static's constructor (__thiscall, no arguments)
    mov dword [A], imm32 / mov byte [A], imm8 / xor eax, eax / mov eax, [A] / mov [A], eax
                                             stores (eax tracked: zero or a loaded value)
    [push imm]* ; mov ecx, C ; [push imm]* ; call F      a static's constructor (__thiscall, checked against the map)
    push esi ; push edi ; ... ; pop edi ; pop esi        around array-constructor loops:
      mov edi, BASE ; mov esi, N ; L: mov ecx, edi ; add edi, STRIDE ; call F ; dec esi ; jns L   (N + 1 calls)
  stubs: ret / ret N / mov al, imm ; ret / xor al, al ; ret / xor ax, ax ; ret / mov eax, ecx ; ret /
         lea eax, [ecx + d] ; ret   -- checked against the map's calling convention, parameters and return type
  deleting destructors (`scalar` / `vector deleting destructor'):
    push esi ; mov esi, ecx ; call DTOR ; test byte [esp+8], 1 ; je ; push esi ; call operator delete ; ...
    test byte [esp+4], 1 ; push esi ; [mov dword [ecx+d], imm32]+ ; mov esi, ecx ; je ; push esi ; call delete ; ...
      (d an 8-bit or a 32-bit displacement)

With --lib, the same shapes for one library whose stage is being written (STAGES: the UI toolkit, library `ui`,
into hook/ui_leftover.cpp; the menus, library `menu`, into hook/menu_leftover.cpp): its $E initialisers and whatever
else is mechanical there; the rest of it is written by hand in that stage's own hook/*.cpp files, which take their
functions out of the list the same way. Every generated file is left out of the registered set (each run sees only the
hand-written files), so a run for one target never changes another's output.

A stage may name the objects whose stubs and deleting destructors it generates (STAGE_FULL); in its other objects only
the $E initialisers are (U2, the menus: the $E of all eleven object files; stubs and deleting destructors of moptions /
mrace / mmixer only -- the other groups write their objects' by hand, and mmulti / msched wait for the multiplayer
stage). One more stub shape there: a `local static destructor helper' thunk (the atexit entry of a function-local
static Xlator, whose destructor is empty) that is a bare `ret`: a __cdecl void(void).

Left out on purpose (listed with --list): ds.obj / ds3d_x.obj except dsounderr2str (the dead hardware DirectSound
mixer), M2's SDL platform functions (platform.cpp detours them), WinMain (the main-loop stage).
"""
from __future__ import annotations

import csv
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_port_tables as gpt  # noqa: E402  (sections, file_off, prologue: read-only helpers)

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
HOOK = ROOT / "hook"
TARGET = HOOK / "krn_leftover.cpp"

LIBS = ("physics", "world", "gx", "ai", "kernel", "useful", "state", "sound")
# --lib: a stage's own library -> (its generated file, what the header calls it, its hand-written files, its harness)
STAGES = {
    "ui": ("ui_leftover.cpp", "M3 UI stage, step U1: the mechanical functions of the widget toolkit (library `ui`: ui.obj,\n"
                              "// _widget.obj, widget.obj, uistyle.obj)", "hook/ui_*.cpp", "test/world_ui.cpp"),
    "menu": ("menu_leftover.cpp", "M3 UI stage, step U2: the $E static initialisers of all eleven menu object files (library\n"
                                  "// `menu`), and the stubs and deleting destructors of moptions.obj, mrace.obj and mmixer.obj",
             "hook/menu_*.cpp", "test/world_menu_options.cpp"),
}
# --lib: the objects whose stubs and deleting destructors a stage generates (absent: all of them); elsewhere only $E
STAGE_FULL = {"menu": ("moptions.obj", "mrace.obj", "mmixer.obj")}
GENERATED = ["krn_leftover.cpp"] + [s[0] for s in STAGES.values()]
DEAD_OBJECTS = ("ds.obj", "ds3d_x.obj")                  # the hardware DirectSound mixer: dead in every build
DEAD_KEEP = {0x004756B0}                                  # dsounderr2str: live (wave.obj's error paths), by hand
PLATFORM = {                                              # M2: platform.cpp detours them to their SDL versions
    0x00412690: "create_window", 0x00412AC0: "hook_keys", 0x00412BF0: "Win32Idle", 0x00412F00: "ScanBegin",
    0x00413040: "ScanUpdate", 0x00414620: "MouseCenter", 0x00418BB0: "JoyBegin", 0x00418F30: "JoyEnd",
    0x00418FF0: "JoyGetPos", 0x00418FD0: "JoyGetName", 0x00419230: "JoyHasForceFeedback",
    0x00419250: "JoyEnableForceFeedback", 0x00419360: "JoySetForce",
}
MAIN_LOOP = {0x00412260: "WinMain@16"}
RCFUNC = 0x00410CC0                                        # rcfunc_is_internal: a bare `ret`
OP_DELETE = 0x00414390


class Refused(Exception):
    pass


# ---- the registered set --------------------------------------------------------------------------------------------
REG = re.compile(r"\b(?:PORT_FN|PORT_FN_GL|PORT_FN_AUDIO|PORT_FN_BUILDS|HARNESS_ONLY_FN|detour)\(\s*(0x[0-9a-fA-F]+)")
REG_NAME = re.compile(r"\bPORT_FN(?:_GL|_AUDIO|_BUILDS)?\(\s*0x[0-9a-fA-F]+,\s*\"([^\"]*)\"")


def registered() -> tuple[set[int], set[str]]:
    addrs, names = set(), set()
    for f in sorted(HOOK.glob("*.cpp")):
        if f.name in GENERATED:                # generated files: each run sees only the hand-written ones
            continue
        t = f.read_text(encoding="utf-8")
        addrs |= {int(m.group(1), 16) for m in REG.finditer(t)}
        names |= {m.group(1) for m in REG_NAME.finditer(t)}
    return addrs, names


# ---- the map's signatures -------------------------------------------------------------------------------------------
SIG = re.compile(r"^(?:(?:public|private|protected): )?(?:virtual |static )?(?P<ret>.*?)\s*__(?P<cc>cdecl|thiscall|stdcall|fastcall) "
                 r"(?P<name>[^(]+)\((?P<params>.*)\)\s*(?:const)?\s*$")


def split_params(p: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in p:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return [x for x in out if x not in ("void", "...")]


def c_type(t: str) -> tuple[str, int]:
    """A parameter or return type from the map, as the rewrite declares it, and its width in bytes (0: void)."""
    t = t.strip()
    if t in ("", "void"):
        return "void", 0
    if "*" in t or "&" in t:
        return "void*", 4
    base = t.replace("const", "").strip()
    if base == "float":
        return "float", 4
    if base in ("unsigned char", "char", "bool", "signed char"):
        return "uint8_t", 1
    if base in ("unsigned short", "short"):
        return "uint16_t", 2
    if base in ("int", "long", "unsigned int", "unsigned long") or base.startswith("enum "):
        return ("uint32_t" if base.startswith("unsigned") else "int32_t"), 4
    raise Refused(f"type {t!r}")


def parse_sig(dem: str) -> dict:
    m = SIG.match(dem)
    if not m:
        raise Refused(f"can't read the signature {dem!r}")
    name = m.group("name").strip()
    params = split_params(m.group("params"))
    return {"cc": m.group("cc"), "name": name, "ret": m.group("ret").strip(), "params": params,
            "variadic": "..." in m.group("params")}


def short_name(qual: str) -> str:
    """The map's qualified name in the house style: no struct/class keywords, no quotes on `scalar deleting destructor'."""
    q = re.sub(r"\b(?:struct|class|enum) ", "", qual)
    return q.replace("`", "").replace("'", "").strip()


def ident(s: str) -> str:
    s = s.replace("~", "dtor_").replace("$", "")
    s = re.sub(r"[^0-9A-Za-z]+", "_", s).strip("_")
    return s


# ---- the image ------------------------------------------------------------------------------------------------------
class Image:
    def __init__(self):
        self.exe = (OUT / "race_v10.exe").read_bytes()
        self.secs = gpt.sections(self.exe)
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)

    def bytes_at(self, va: int, n: int) -> bytes:
        o = gpt.file_off(self.secs, va)
        return self.exe[o:o + n]

    def u32(self, va: int) -> int:
        return struct.unpack_from("<I", self.bytes_at(va, 4))[0]


def padding_ok(img: Image, va: int, end: int, size: int) -> bool:
    """Everything after the body's last instruction, to the function's end, is the linker's padding."""
    rest = img.bytes_at(va + end, size - end)
    n = 0
    for ins in img.md.disasm(rest, va + end):
        lea_self = ins.mnemonic == "lea" and re.fullmatch(r"(\w+), \[\1(?: \+ 0)?\]", ins.op_str)
        mov_self = ins.mnemonic == "mov" and re.fullmatch(r"(\w+), \1", ins.op_str)
        if not (ins.mnemonic in ("int3", "nop") or lea_self or mov_self or (ins.mnemonic == "add" and ins.op_str == "eax, 0")):
            return False
        n += ins.size
    return n == len(rest)


def rel32(code: bytes, at: int, va: int) -> int:
    return (va + at + 5 + struct.unpack_from("<i", code, at + 1)[0]) & 0xFFFFFFFF


# ---- $E: statements -------------------------------------------------------------------------------------------------
def parse_e(img: Image, inv: dict, va: int, size: int, obj: str) -> tuple[list[str], set[str]]:
    """The C++ body of a static initialiser (lines), and the typedefs it uses."""
    code = img.bytes_at(va, size)
    types: set[str] = set()
    # a single tail jump
    if code[0] == 0xE9:
        t = rel32(code, 0, va)
        if not padding_ok(img, va, 5, size):
            raise Refused("jmp followed by code")
        tr = inv.get(t)
        if t != RCFUNC and not (tr and tr["object"] == obj and tr["demangled"].startswith("$E")):
            raise Refused(f"tail jump to {t:08x} ({tr and tr['demangled']}), neither rcfunc_is_internal nor this file's $E")
        what = "rcfunc_is_internal (a bare `ret`)" if t == RCFUNC else tr["demangled"]
        return [f"LO_FN(LoVoid_t, 0x{t:08x})();    // jmp: {what}"], {"LoVoid_t"}
    if code[0] == 0xB9 and code[5] == 0xE9:
        c, t = struct.unpack_from("<I", code, 1)[0], rel32(code, 5, va)
        if not padding_ok(img, va, 10, size):
            raise Refused("mov ecx; jmp followed by code")
        tr = inv.get(t)
        sig = parse_sig(tr["demangled"]) if tr else None
        if not sig or sig["cc"] != "thiscall" or sig["params"]:
            raise Refused(f"mov ecx; jmp {t:08x}: not a __thiscall without arguments")
        return [f"LO_FN(LoCtor0_t, 0x{t:08x})((void*)0x{c:08x}u, 0);    // jmp: {short_name(sig['name'])}"], {"LoCtor0_t"}

    lines: list[str] = []
    i = 0
    saved = False
    if code[0:2] == b"\x56\x57":                               # push esi ; push edi
        saved = True
        i = 2
    eax: str | None = None                                    # what eax holds: "0" or a local's name
    ecx: int | None = None
    pushes: list[int] = []
    nload = 0
    while True:
        if i >= len(code):
            raise Refused("ran off the end")
        b = code[i]
        if b == 0xC7 and code[i + 1] == 0x05:                  # mov dword [A], imm32
            a, v = struct.unpack_from("<II", code, i + 2)
            lines.append(f"LO_G32(0x{a:08x}) = 0x{v:08x}u;")
            i += 10
        elif b == 0xC6 and code[i + 1] == 0x05:                # mov byte [A], imm8
            a, v = struct.unpack_from("<IB", code, i + 2)
            lines.append(f"LO_G8(0x{a:08x}) = 0x{v:02x};")
            i += 7
        elif code[i:i + 2] == b"\x33\xC0":                     # xor eax, eax
            eax = "0"
            i += 2
        elif b == 0xA1:                                        # mov eax, [A]
            a = struct.unpack_from("<I", code, i + 1)[0]
            nload += 1
            eax = f"v{nload}"
            lines.append(f"const uint32_t {eax} = LO_G32(0x{a:08x});")
            i += 5
        elif b == 0xA3:                                        # mov [A], eax
            if eax is None:
                raise Refused("stores eax without knowing it")
            a = struct.unpack_from("<I", code, i + 1)[0]
            lines.append(f"LO_G32(0x{a:08x}) = {eax if eax != '0' else '0u'};")
            i += 5
        elif b == 0x68:                                        # push imm32
            pushes.append(struct.unpack_from("<I", code, i + 1)[0])
            i += 5
        elif b == 0x6A:                                        # push imm8 (sign-extended)
            pushes.append(struct.unpack_from("<b", code, i + 1)[0] & 0xFFFFFFFF)
            i += 2
        elif b == 0xB9:                                        # mov ecx, C
            ecx = struct.unpack_from("<I", code, i + 1)[0]
            i += 5
        elif b == 0xE8:                                        # call F: a static's constructor
            f = rel32(code, i, va)
            fr = inv.get(f)
            sig = parse_sig(fr["demangled"]) if fr else None
            if ecx is None or not sig or sig["cc"] != "thiscall" or len(sig["params"]) != len(pushes):
                raise Refused(f"call {f:08x} ({fr and fr['demangled']}): not a __thiscall taking the {len(pushes)} pushed values")
            args = [f"0x{p:08x}u" for p in reversed(pushes)]           # the last push is the first argument
            td = f"LoCtor{len(args)}_t"
            types.add(td)
            lines.append(f"LO_FN({td}, 0x{f:08x})({', '.join([f'(void*)0x{ecx:08x}u', '0'] + args)});    // {short_name(sig['name'])}")
            ecx, pushes, eax = None, [], None
            i += 5
        elif saved and b == 0xBF and code[i + 5] == 0xBE:      # an array-constructor loop
            base, n = struct.unpack_from("<I", code, i + 1)[0], struct.unpack_from("<I", code, i + 6)[0]
            j = i + 10
            if code[j:j + 2] != b"\x8B\xCF":
                raise Refused("array loop: no mov ecx, edi")
            top = j
            j += 2
            if code[j:j + 2] == b"\x83\xC7":
                stride = code[j + 2]
                j += 3
            elif code[j:j + 2] == b"\x81\xC7":
                stride = struct.unpack_from("<I", code, j + 2)[0]
                j += 6
            else:
                raise Refused("array loop: no add edi, STRIDE")
            if code[j] != 0xE8:
                raise Refused("array loop: no call")
            f = rel32(code, j, va)
            j += 5
            if code[j] != 0x4E or code[j + 1] != 0x79 or j + 3 + struct.unpack_from("<b", code, j + 2)[0] != top:
                raise Refused("array loop: no dec esi ; jns back to the top")
            j += 3
            fr = inv.get(f)
            sig = parse_sig(fr["demangled"]) if fr else None
            if not sig or sig["cc"] != "thiscall" or sig["params"] or n > 0xFFFF:
                raise Refused(f"array loop: {f:08x} isn't a __thiscall constructor without arguments, or the count is odd")
            types.add("LoCtor0_t")
            lines.append(f"for (uint32_t p = 0x{base:08x}u, n = 0; n <= {n}u; n++, p += 0x{stride:x}u)    // {n + 1} x {short_name(sig['name'])}")
            lines.append(f"    LO_FN(LoCtor0_t, 0x{f:08x})((void*)p, 0);")
            eax, ecx = None, None
            i = j
        elif saved and code[i:i + 3] == b"\x5F\x5E\xC3":         # pop edi ; pop esi ; ret
            i += 3
            break
        elif not saved and b == 0xC3:
            i += 1
            break
        else:
            raise Refused(f"unknown instruction at +{i:x}: {code[i:i + 6].hex()}")
    if pushes or ecx is not None:
        raise Refused("pushes or ecx left over")
    if not padding_ok(img, va, i, size):
        raise Refused("code after the ret")
    return lines, types


# ---- stubs and deleting destructors ---------------------------------------------------------------------------------
def rewrite_sig(sig: dict, ret: str) -> tuple[str, str, list[str]]:
    """(calling convention, the parameter list with names blank, parameter types)."""
    ptypes = [c_type(p)[0] for p in sig["params"]]
    if sig["cc"] == "thiscall":
        return "__fastcall", ", ".join(["void* self", "LoEdx"] + ptypes), ["void*", "LoEdx"] + ptypes
    if sig["cc"] == "cdecl":
        return "__cdecl", ", ".join(ptypes), ptypes
    raise Refused(f"calling convention {sig['cc']}")


def stack_bytes(sig: dict) -> int:
    return 4 * len(sig["params"])


def parse_other(img: Image, inv: dict, r: dict) -> tuple[str, list[str], str, dict]:
    """(shape, body lines, return type, sig) for a stub or deleting destructor."""
    va, size = int(r["va"], 16), int(r["size"])
    code = img.bytes_at(va, size)
    sig = parse_sig(r["demangled"])
    rt, rw = c_type(sig["ret"]) if sig["ret"] else ("", 0)
    qual = sig["name"]
    is_ctor = bool(re.search(r"(?:^|::)(\w+)::\1$", re.sub(r"<[^>]*>", "", qual)))
    is_dtor = "::~" in qual

    def done(n: int) -> None:
        if not padding_ok(img, va, n, size):
            raise Refused("code after the ret")

    def this_only():
        if sig["cc"] != "thiscall" or sig["params"]:
            raise Refused("expected a __thiscall without arguments")

    if "deleting destructor" in qual:
        if sig["cc"] != "thiscall" or len(sig["params"]) != 1:
            raise Refused("deleting destructor without its flags")
        # A: push esi; mov esi, ecx; call DTOR; test byte [esp+8], 1; je +9; push esi; call delete; add esp, 4;
        #    mov eax, esi; pop esi; ret 4
        tail = b"\x74\x09\x56\xE8"
        end = b"\x83\xC4\x04\x8B\xC6\x5E\xC2\x04\x00"
        if code[0:4] == b"\x56\x8B\xF1\xE8" and code[8:13] == b"\xF6\x44\x24\x08\x01" and code[13:17] == tail \
                and code[21:30] == end and rel32(code, 16, va) == OP_DELETE:
            d = rel32(code, 3, va)
            dr = inv.get(d)
            dsig = parse_sig(dr["demangled"]) if dr else None
            if not dsig or "::~" not in dsig["name"] or dsig["cc"] != "thiscall" or dsig["params"]:
                raise Refused(f"deleting destructor calls {d:08x}, not a destructor")
            done(30)
            return "dtor_call", [
                f"LO_FN(LoCtor0_t, 0x{d:08x})(self, 0);    // {short_name(dsig['name'])}",
                "if (flags & 1) LO_FN(LoPtr_t, 0x00414390)(self);    // operator delete",
                "return self;"], "void*", sig
        # B: test byte [esp+4], 1; push esi; [mov dword [ecx+d], imm32]+; mov esi, ecx; je +9; push esi; call delete;
        #    add esp, 4; mov eax, esi; pop esi; ret 4
        if code[0:6] == b"\xF6\x44\x24\x04\x01\x56":
            i, stores = 6, []
            while True:
                if code[i:i + 2] == b"\xC7\x01":
                    stores.append((0, struct.unpack_from("<I", code, i + 2)[0]))
                    i += 6
                elif code[i:i + 2] == b"\xC7\x41":
                    stores.append((code[i + 2], struct.unpack_from("<I", code, i + 3)[0]))
                    i += 7
                elif code[i:i + 2] == b"\xC7\x81":                  # mov dword [ecx + disp32], imm32
                    stores.append(struct.unpack_from("<II", code, i + 2))
                    i += 10
                else:
                    break
            if stores and code[i:i + 2] == b"\x8B\xF1" and code[i + 2:i + 6] == tail and rel32(code, i + 5, va) == OP_DELETE \
                    and code[i + 10:i + 19] == end:
                done(i + 19)
                body = [f"LO_G32((uint8_t*)self + 0x{d:x}) = 0x{v:08x}u;    // the inlined destructor" for d, v in stores]
                return "dtor_inline", body + ["if (flags & 1) LO_FN(LoPtr_t, 0x00414390)(self);    // operator delete",
                                              "return self;"], "void*", sig
        raise Refused("a deleting destructor of an unknown shape")

    if code[0] == 0xC3:                                        # ret
        if rw:
            raise Refused("bare ret, but the map says it returns a value")
        if sig["cc"] == "thiscall" and sig["params"]:
            raise Refused("bare ret, but a __thiscall with arguments")
        if is_ctor:
            raise Refused("a constructor that doesn't return this")
        done(1)
        return "ret", [], "void", sig
    if code[0] == 0xC2 and code[2] == 0:                       # ret N
        if sig["cc"] != "thiscall" or rw or stack_bytes(sig) != code[1]:
            raise Refused(f"ret {code[1]}: not a __thiscall taking {code[1]} bytes and returning nothing")
        done(3)
        return "ret_n", [], "void", sig
    if code[0] == 0xB0 and code[2] == 0xC3:                    # mov al, imm ; ret
        if rt != "uint8_t" or (sig["cc"] == "thiscall" and sig["params"]):
            raise Refused("mov al: not a byte returned")
        done(3)
        return "ret_al", [f"return 0x{code[1]:02x};"], "uint8_t", sig
    if code[0:3] in (b"\x32\xC0\xC3", b"\x30\xC0\xC3"):        # xor al, al ; ret
        if rt != "uint8_t" or (sig["cc"] == "thiscall" and sig["params"]):
            raise Refused("xor al: not a byte returned")
        done(3)
        return "ret_al", ["return 0;"], "uint8_t", sig
    if code[0:4] in (b"\x66\x33\xC0\xC3", b"\x66\x31\xC0\xC3"):  # xor ax, ax ; ret
        if rt != "uint16_t" or sig["cc"] != "cdecl":
            raise Refused("xor ax: not a word returned")
        done(4)
        return "ret_ax", ["return 0;"], "uint16_t", sig
    if code[0:3] in (b"\x8B\xC1\xC3", b"\x89\xC8\xC3"):        # mov eax, ecx ; ret
        this_only()
        if not is_ctor:
            raise Refused("mov eax, ecx: not a constructor")
        done(3)
        return "ret_this", ["return self;"], "void*", sig
    if code[0:2] == b"\x8D\x41" and code[3] == 0xC3:           # lea eax, [ecx + d] ; ret
        this_only()
        if rw != 4 or "*" not in sig["ret"]:
            raise Refused("lea eax, [ecx+d]: not a pointer returned")
        done(4)
        return "ret_member", [f"return (uint8_t*)self + 0x{code[2]:x};"], "void*", sig
    raise Refused(f"unknown shape {code[:8].hex()}")


# ---- local static destructor helpers (a stage with STAGE_FULL only) --------------------------------------------------
THUNK_HELPER = re.compile(r"^\[thunk\]:class (?P<cls>\w+) `(?P<fn>.*)'::`\d+'::(?P<var>\w+)`local static destructor helper'$")


def parse_thunk_helper(img: Image, r: dict) -> str:
    """The rewrite's name for a `local static destructor helper' thunk whose body is a bare `ret` (else Refused)."""
    va, size = int(r["va"], 16), int(r["size"])
    m = THUNK_HELPER.match(r["demangled"])
    if img.bytes_at(va, 1) != b"\xC3" or not padding_ok(img, va, 1, size):
        raise Refused("a local static destructor helper that isn't a bare ret")
    fn = parse_sig(m.group("fn"))
    return f"{short_name(fn['name'])}::{m.group('var')} destructor helper"


# ---- the file -------------------------------------------------------------------------------------------------------
HEADER = """\
// krn_leftover.cpp -- generated by tools/gen_leftovers.py -- don't edit
//
// M3 UI stage, step U0: every function left in the finished libraries (physics, world, gx, ai, kernel, useful,
// state, sound) whose shape is mechanical, rewritten from its exact v1.0 instruction bytes (tools/gen_leftovers.py
// says how each shape reads). The rest are in krn_leftover_hand.cpp; test/world_leftover.cpp checks every one against
// its original. Faithful (docs/PORTING.md): stores through volatile at the original's width, in its order; calls to
// the game by their v1.0 address (a __thiscall as __fastcall with an unused edx), so what runs there -- the original
// or its hooked rewrite -- is what the original would have called.
//
// Counts by shape: {counts}
//
// The $E static initialisers run once, from the CRT's _initterm: replay_only (as krn_core.cpp's). A tail jump
// (`jmp T`) stays a call to T by address. The stubs read and write nothing (pure); the deleting destructors free
// memory (replay_only).
#include <stdint.h>
#include "port.h"

typedef int LoEdx;                                  // the unused edx of a __thiscall received as __fastcall
#define LO_G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define LO_G32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define LO_FN(T, a) ((T)(uintptr_t)(a))
typedef void(__cdecl* LoVoid_t)();
typedef void(__cdecl* LoPtr_t)(void*);
typedef void*(__fastcall* LoCtor0_t)(void*, LoEdx);
typedef void*(__fastcall* LoCtor1_t)(void*, LoEdx, uint32_t);
typedef void*(__fastcall* LoCtor2_t)(void*, LoEdx, uint32_t, uint32_t);

static void fp_lo_static_init(Footprint& f) {{ f.replay_only = "a static initialiser (runs once, from the CRT's _initterm)"; }}
"""

STAGE_HEADER = """\
// {target} -- generated by tools/gen_leftovers.py --lib {lib} -- don't edit
//
// {what},
// rewritten from their exact v1.0 instruction bytes (tools/gen_leftovers.py says how each shape reads). The rest of the
// library is written by hand ({hand}); {harness} checks every one against its original. Faithful (docs/PORTING.md):
// stores through volatile at the original's width, in its order; calls to the game by their v1.0 address (a
// __thiscall as __fastcall with an unused edx), so what runs there -- the original or its hooked rewrite -- is what
// the original would have called.
//
// Counts by shape: {counts}
//
// The $E static initialisers run once, from the CRT's _initterm: replay_only. A tail jump (`jmp T`) stays a call to T
// by address. The stubs read and write nothing (pure); the deleting destructors free memory (replay_only).
#include <stdint.h>
#include "port.h"

// file-local, so a harness can include it beside krn_leftover.cpp
namespace {{
typedef int LoEdx;                                  // the unused edx of a __thiscall received as __fastcall
#define LO_G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define LO_G32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define LO_FN(T, a) ((T)(uintptr_t)(a))
typedef void(__cdecl* LoVoid_t)();
typedef void(__cdecl* LoPtr_t)(void*);
typedef void*(__fastcall* LoCtor0_t)(void*, LoEdx);
typedef void*(__fastcall* LoCtor1_t)(void*, LoEdx, uint32_t);
typedef void*(__fastcall* LoCtor2_t)(void*, LoEdx, uint32_t, uint32_t);

static void fp_lo_static_init(Footprint& f) {{ f.replay_only = "a static initialiser (runs once, from the CRT's _initterm)"; }}
"""


def main() -> int:
    global LIBS, TARGET
    args = sys.argv[1:]
    list_only = "--list" in args
    stage = None
    if "--lib" in args:
        i = args.index("--lib")
        stage = args[i + 1] if i + 1 < len(args) else ""
        if stage not in STAGES:
            print(f"--lib: one of {', '.join(STAGES)}")
            return 2
        LIBS, TARGET = (stage,), HOOK / STAGES[stage][0]
    img = Image()
    rows = list(csv.DictReader(open(OUT / "inventory.csv", encoding="utf-8")))
    inv = {int(r["va"], 16): r for r in rows}
    reg, names_taken = registered()
    qual_count: dict[str, int] = {}
    obj_count: dict[tuple[str, str], int] = {}
    for r in rows:
        m = SIG.match(r["demangled"])
        q = short_name(m.group("name")) if m else r["demangled"]
        qual_count[q] = qual_count.get(q, 0) + 1
        obj_count[(q, r["object"])] = obj_count.get((q, r["object"]), 0) + 1

    libs_of: dict[str, set[str]] = {}
    for r in rows:
        libs_of.setdefault(r["object"], set()).add(r["library"])

    def objname(r: dict) -> str:             # "track.obj", or "world:track.obj" where two libraries have one
        return f"{r['library']}:{r['object']}" if len(libs_of[r["object"]]) > 1 else r["object"]

    full = STAGE_FULL.get(stage) if stage else None
    targets, left_out = [], []
    for r in rows:
        va = int(r["va"], 16)
        if r["library"] not in LIBS or va in reg:
            continue
        if full is not None and r["object"] not in full and not r["demangled"].startswith("$E"):
            left_out.append((va, r, "not this step's: only its $E initialisers are generated here"))
        elif r["object"] in DEAD_OBJECTS and va not in DEAD_KEEP:
            left_out.append((va, r, "the dead hardware DirectSound mixer (ds.obj / ds3d_x.obj)"))
        elif va in PLATFORM:
            left_out.append((va, r, "M2: platform.cpp detours it to its SDL version"))
        elif va in MAIN_LOOP:
            left_out.append((va, r, "the main-loop stage"))
        else:
            targets.append(r)

    out, refused, counts = [], [], {}
    used_ids: set[str] = set()
    names: set[str] = set()
    for r in sorted(targets, key=lambda r: int(r["va"], 16)):
        va, size, obj = int(r["va"], 16), int(r["size"]), r["object"]
        oname = objname(r)
        stem = oname.replace(".obj", "")
        try:
            try:                                                 # the hook's jmp must fit (gen_port_tables' rule)
                gpt.prologue(img.exe, img.secs, img.md, va, size)
            except SystemExit as e:
                raise Refused(f"can't be hooked: {e}")
            dem = r["demangled"]
            if full is not None and THUNK_HELPER.match(dem):
                name = parse_thunk_helper(img, r)
                kind = "thunk ret"
                if name in names or name in names_taken:
                    raise Refused(f"can't name it uniquely ({name})")
                rid = ident(name) + "_lo"
                code = [f"// {dem.strip()}", f"static void __cdecl {rid}() {{}}",
                        f"static void fp_{rid}(Footprint& f) {{ f.pure = true; }}",
                        f"PORT_FN(0x{va:08x}, \"{name}\", {rid}, fp_{rid})"]
            elif dem.startswith("$E"):
                lines, _types = parse_e(img, inv, va, size, obj)
                shape = "$E"
                name = f"{dem}({oname})"
                if name in names or name in names_taken:
                    raise Refused(f"can't name it uniquely ({name})")
                rid = ident(f"{dem}_{stem}") + "_lo"
                if rid in used_ids:                              # _widget.obj and widget.obj: one identifier
                    rid = ident(f"{dem}_{stem}_{va:x}") + "_lo"
                if lines:
                    code = [f"static void __cdecl {rid}() {{"] + [f"    {x}" for x in lines] + ["}"]
                else:
                    code = [f"static void __cdecl {rid}() {{}}    // ret"]
                code += [f"PORT_FN(0x{va:08x}, \"{name}\", {rid}, fp_lo_static_init)"]
                kind = classify_e(lines)
            else:
                kind, body, rt, sig = parse_other(img, inv, r)
                cc, plist, ptypes = rewrite_sig(sig, rt)
                if "deleting destructor" in sig["name"]:
                    plist = "void* self, LoEdx, uint32_t flags"
                    ptypes = ["void*", "LoEdx", "uint32_t"]
                qual = short_name(sig["name"])
                name = qual
                if obj_count.get((qual, obj), 0) > 1:                # overloads in one file: by their parameters
                    name = f"{qual}({','.join(short_name(p).replace(' ', '') for p in sig['params'])})"
                elif qual_count.get(qual, 0) > 1 or name in names_taken:   # the same name in another file
                    name = f"{qual}({oname})"
                if name in names or name in names_taken:
                    raise Refused(f"can't name it uniquely ({name})")
                rid = ident(qual) + "_lo"
                if rid in used_ids:
                    rid = ident(f"{qual}_{stem}") + "_lo"
                if rid in used_ids:
                    rid = ident(f"{qual}_{va:x}") + "_lo"
                if "self" not in plist and body and any("self" in x for x in body):
                    raise Refused("uses self without one")
                fpid = "fp_" + rid
                fparams = ", ".join(["Footprint& f"] + ptypes)
                if kind.startswith("dtor"):
                    fp = f"static void {fpid}({fparams}) {{ f.replay_only = \"frees the object\"; }}"
                else:
                    fp = f"static void {fpid}({fparams}) {{ f.pure = true; }}"
                code = [f"// {dem.strip()}"]
                if body:
                    code += [f"static {rt} {cc} {rid}({plist}) {{"] + [f"    {x}" for x in body] + ["}"]
                else:
                    code += [f"static {rt} {cc} {rid}({plist}) {{}}"]
                code += [fp, f"PORT_FN(0x{va:08x}, \"{name}\", {rid}, {fpid})"]
            if rid in used_ids:
                raise Refused(f"identifier clash {rid}")
            used_ids.add(rid)
            names.add(name)
            counts[kind] = counts.get(kind, 0) + 1
            out.append((va, oname, code))
        except Refused as e:
            refused.append((va, r, str(e)))

    if list_only or refused:
        for va, r, why in left_out:
            print(f"left out  {va:08x} {r['object']:12s} {r['demangled'][:70]}  -- {why}")
    if refused:
        for va, r, why in refused:
            print(f"REFUSED   {va:08x} {r['object']:12s} {r['demangled'][:70]}  -- {why}")
        hand = STAGES[stage][2] if stage else "krn_leftover_hand.cpp"
        print(f"{len(refused)} functions fit no known shape: nothing written (hand-write them in {hand})")
        return 1

    order = ["$E ret", "$E jmp", "$E jmp (constructor)", "$E stores", "$E constructor", "$E array constructor", "$E mixed", "ret", "ret_n", "ret_al",
             "ret_ax", "ret_this", "ret_member", "thunk ret", "dtor_call", "dtor_inline"]
    ctext = ", ".join(f"{k} {counts[k]}" for k in order if k in counts) + f"; {sum(counts.values())} in all"
    if stage:
        target, what, hand, harness = STAGES[stage]
        text = [STAGE_HEADER.format(target=target, lib=stage, what=what, hand=hand, harness=harness, counts=ctext)]
    else:
        text = [HEADER.format(counts=ctext)]
    cur = None
    for va, obj, code in out:
        if obj != cur:
            text.append(f"\n// ---- {obj} " + "-" * max(4, 112 - len(obj)))
            cur = obj
        text.extend(code)
    if stage:
        text.append("\n}  // namespace")
    TARGET.write_text("\n".join(text) + "\n", encoding="utf-8", newline="\n")
    print(f"{TARGET.relative_to(ROOT)}: {sum(counts.values())} functions ({ctext}); {len(left_out)} left out")
    return 0


def classify_e(lines: list[str]) -> str:
    """The $E's shape, for the counts."""
    if not lines:
        return "$E ret"
    if len(lines) == 1 and "// jmp" in lines[0]:
        return "$E jmp (constructor)" if "LoCtor0_t" in lines[0] else "$E jmp"
    if any(x.startswith("for ") for x in lines):
        return "$E array constructor"
    calls = [x for x in lines if "LO_FN(" in x]
    stores = [x for x in lines if x.startswith("LO_G") or x.startswith("const uint32_t")]
    if calls and len(calls) + len(stores) == len(lines):
        return "$E constructor"
    if not calls:
        return "$E stores"
    return "$E mixed"


if __name__ == "__main__":
    sys.exit(main())
