"""Static check of hook/crt_start.cpp's naked rewrites against the v1.0 race.exe, instruction by instruction.

    python test/world_crt_start_static.py %TEMP%\\crts\\world_crt_start.exe

The companion of test/world_crt_start.cpp (built as its header says). That harness runs the start-up, exit, message,
fpinit and x87 code against the originals; the exception machinery -- exsup (_global_unwind2, _unwind_handler,
_local_unwind2, _abnormal_termination, _NLG_Notify, _NLG_Notify1), exsup3 (_except_handler3, _seh_longjmp_unwind@4)
and _WinMainCRTStartup's own filter and handler -- is checked here instead, without raising anything: each rewrite's
machine code (read from the harness exe, whose addresses `world_crt_start.exe addrs` prints) is disassembled with
capstone next to the original's bytes in out/race_v10.exe, and the two instruction streams compared. Every other naked
rewrite of the file (_fpmath, __crtGetEnvironmentStringsA, _WinMainCRTStartup, _fFMOD, 87disp's result routines,
__adj_fpatan, __rdtsc) is compared the same way.

Both sides are put in one canonical form: the code is traced from the entry (fall-through first, conditional targets
queued), an unconditional jump inside the routine is followed rather than listed (so a shared tail the original jumps
into and the rewrite's own copy of it read the same), branch targets become labels (the index of the instruction they
reach), an immediate that is an address in the trace becomes that label too, a call through one of the rewrite's
address constants (`call dword ptr [a_heap_init]`) becomes a call to the v1.0 address it holds, a call to an import
thunk (`call 0x4da340` = `jmp [RtlUnwind]`) becomes a call through the import slot, and the rewrite's own copies of
v1.0 data (the scope table, the filter / handler, the continuation label) are mapped to their v1.0 addresses. Operands
are compared by value (an imm8 and an imm32 of the same value are the same; so are a short and a near jump).
"""
from __future__ import annotations

import difflib
import struct
import subprocess
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

ROOT = Path(__file__).resolve().parent.parent


class Image:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        n = struct.unpack_from("<H", self.data, pe + 6)[0]
        opt = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.base = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]
        self.secs = []
        for i in range(n):
            name, vs, va, rs, ro = struct.unpack_from("<8sIIII", self.data, pe + 24 + opt + i * 40)
            ch = struct.unpack_from("<I", self.data, pe + 24 + opt + i * 40 + 36)[0]
            self.secs.append((name.rstrip(b"\0").decode(), self.base + va, max(vs, rs), rs, ro, ch))
        self.size = max(s[1] + s[2] for s in self.secs) - self.base

    def contains(self, va: int) -> bool:
        return self.base <= va < self.base + self.size

    def is_code(self, va: int) -> bool:
        return any(lo <= va < lo + n and ch & 0x20000000 for _nm, lo, n, _rs, _ro, ch in self.secs)

    def read(self, va: int, n: int) -> bytes:
        for _nm, lo, size, rs, ro, _ch in self.secs:
            if lo <= va < lo + size:
                o = va - lo
                b = self.data[ro + o: ro + min(rs, o + n)] if o < rs else b""
                return b + b"\0" * (n - len(b))
        return b"\0" * n

    def u32(self, va: int) -> int:
        return struct.unpack("<I", self.read(va, 4))[0]


MD = Cs(CS_ARCH_X86, CS_MODE_32)
MD.detail = True
JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg", "jecxz",
       "loop", "loope", "loopne"}


def decode(img: Image, va: int):
    for ins in MD.disasm(img.read(va, 16), va):
        return ins
    raise RuntimeError(f"can't decode {va:08x}")


def direct_target(ins):
    if len(ins.operands) == 1 and ins.operands[0].type == X86_OP_IMM:
        return ins.operands[0].imm & 0xFFFFFFFF
    return None


def abs_mem(op):
    """An absolute memory operand's address (no base / index register), else None."""
    if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
        return op.mem.disp & 0xFFFFFFFF
    return None


class Side:
    """One routine's code: the original in the image, or the rewrite in the harness exe."""

    def __init__(self, img: Image, entry: int, rewrite: bool, ctx):
        self.img, self.entry, self.rewrite, self.ctx = img, entry, rewrite, ctx
        self.order = []          # canonical instruction list: capstone insns, or ("join", target)
        self.index = {}          # address -> canonical index
        self.trace()

    def followable(self, t: int) -> bool:
        if not self.img.is_code(t):
            return False
        if self.rewrite:
            return t not in self.ctx.rewrite_entries or t == self.entry
        return True

    def resolve(self, t: int) -> int:
        seen = set()
        while t not in seen and self.followable(t):
            seen.add(t)
            ins = decode(self.img, t)
            if ins.mnemonic == "jmp" and direct_target(ins) is not None and self.followable(direct_target(ins)):
                t = direct_target(ins)
            else:
                break
        return t

    def trace(self):
        work = [self.entry]
        while work:
            a = self.resolve(work.pop(0))
            started = False
            while True:
                if a in self.index:
                    if started:
                        self.order.append(("join", a))
                    break
                ins = decode(self.img, a)
                t = direct_target(ins)
                if ins.mnemonic == "jmp" and t is not None and self.followable(t):
                    a = self.resolve(t)
                    continue
                self.index[a] = len(self.order)
                self.order.append(ins)
                started = True
                if ins.mnemonic in ("ret", "retf", "jmp", "int3", "hlt"):
                    break
                if ins.mnemonic in JCC and t is not None:
                    work.append(t)
                a += ins.size

    def label(self, t: int):
        r = self.resolve(t)
        if r in self.index:
            self.labels.append(self.index[r])
            return "L"
        return None

    def addr_name(self, v: int) -> str:
        """An address outside the trace, as v1.0 names it."""
        if self.rewrite:
            if v in self.ctx.alias:
                return f"{self.ctx.alias[v]:#x}"
            if v in self.ctx.helper:
                return f"<{self.ctx.helper[v]}>"
            if v in self.ctx.rewrite_by_addr:
                return f"{self.ctx.rewrite_by_addr[v]:#x}"
        return f"{v:#x}"

    def norm_op(self, ins, op, branch: bool) -> str:
        if op.type == X86_OP_REG:
            return ins.reg_name(op.reg)
        if op.type == X86_OP_IMM:
            v = op.imm & 0xFFFFFFFF
            if branch or self.img.is_code(v) or (self.rewrite and self.img.contains(v)):
                lab = self.label(v) if self.img.contains(v) else None
                if lab:
                    return lab
                if branch and not self.rewrite and self.img.is_code(v):
                    th = decode(self.img, v)                 # an import thunk: jmp dword ptr [slot]
                    if th.mnemonic == "jmp" and th.operands and abs_mem(th.operands[0]) is not None:
                        return f"{th.operands[0].size}:[{abs_mem(th.operands[0]):#x}]"
                return self.addr_name(v)
            return f"{v:#x}"
        m = op.mem
        parts = []
        if m.base:
            parts.append(ins.reg_name(m.base))
        if m.index:
            parts.append(f"{ins.reg_name(m.index)}*{m.scale}")
        disp = m.disp & 0xFFFFFFFF
        if not m.base and not m.index:
            if branch and self.rewrite and self.img.contains(disp):
                return self.addr_name(self.img.u32(disp))        # call [an address constant]: the address it holds
            parts.append(self.addr_name(disp) if self.rewrite and self.img.contains(disp) else f"{disp:#x}")
        elif m.disp:
            parts.append(f"{m.disp:#x}" if m.disp > 0 else f"-{-m.disp:#x}")
        seg = f"{ins.reg_name(m.segment)}:" if m.segment and ins.reg_name(m.segment) != "ds" else ""   # (a redundant ds: prefix)
        return f"{op.size}:{seg}[{'+'.join(parts)}]"

    def canonical(self):
        """[(text, [label targets as canonical indices])]: labels print as "L" and are compared through the alignment."""
        out = []
        for e in self.order:
            self.labels = []
            if isinstance(e, tuple):
                out.append((f"jmp {self.label(e[1])}", self.labels))
                continue
            mn = e.mnemonic.replace("notrack ", "")         # (capstone's name for a ds: prefix on an indirect branch)
            branch = mn in JCC or mn in ("call", "jmp")
            ops = [self.norm_op(e, op, branch) for op in e.operands]
            if mn in ("ret", "retf") and ops == ["0x0"]:
                ops = []                                      # ret 0 = ret
            out.append((f"{mn} {', '.join(ops)}".strip(), self.labels))
        return out

    def where(self, i: int) -> str:
        e = self.order[i]
        return f"{e[1]:08x}(join)" if isinstance(e, tuple) else f"{e.address:08x}"


class Ctx:
    def __init__(self, exe: Path):
        txt = subprocess.run([str(exe), "addrs"], capture_output=True, text=True, check=True).stdout
        self.fn = {}             # v10 -> (rewrite address, name)
        self.island = {}         # v10 -> (rewrite address, name)
        self.alias = {}          # rewrite-side address -> v10 address
        self.helper = {}         # rewrite-side address -> name
        for line in txt.splitlines():
            w = line.split(None, 3)
            if w[0] == "fn":
                self.fn[int(w[1], 16)] = (int(w[2], 16), w[3])
            elif w[0] == "island":
                self.island[int(w[1], 16)] = (int(w[2], 16), w[3])
            elif w[0] == "alias":
                self.alias[int(w[2], 16)] = int(w[1], 16)
            elif w[0] == "helper":
                self.helper[int(w[1], 16)] = w[2]
        self.rewrite_entries = {a for a, _ in self.fn.values()} | {a for a, _ in self.island.values()}
        self.rewrite_by_addr = {a: v for v, (a, _) in self.fn.items()}


# what is compared: (v1.0 address, name, why); a rewrite found by its PORT_FN / island / alias
CHECKS = [
    (0x004D37DC, "_global_unwind2", "exsup"),
    (0x004D37FC, "_unwind_handler", "exsup"),
    (0x004D381E, "_local_unwind2", "exsup"),
    (0x004D3886, "_abnormal_termination", "exsup"),
    (0x004D38A9, "_NLG_Notify1", "exsup"),
    (0x004D38B2, "_NLG_Notify", "exsup"),
    (0x004D4490, "_except_handler3", "exsup3"),
    (0x004D454D, "_seh_longjmp_unwind@4", "exsup3"),
    (0x004CF6E5, "_WinMainCRTStartup's filter", "wincrt0 (scope table)"),
    (0x004CF700, "_WinMainCRTStartup's handler", "wincrt0 (scope table)"),
    (0x004CF5A0, "_WinMainCRTStartup", "wincrt0"),
    (0x004CE120, "_fpmath", "fpinit"),
    (0x004D3E20, "__crtGetEnvironmentStringsA", "aw_env"),
    (0x004CF374, "_fFMOD", "87fmod"),
    (0x004D2E9C, "__rttosnpop", "87disp (island)"),
    (0x004D2EA4, "__rtzeropop", "87disp (island)"),
    (0x004D2EA6, "__rtzeronpop", "87disp (island)"),
    (0x004D2EB2, "__tosnan1", "87disp (island)"),
    (0x004D2EDD, "__nosnan2", "87disp (island)"),
    (0x004D2EDF, "__tosnan2", "87disp (island)"),
    (0x004D2F07, "__nan2", "87disp (island)"),
    (0x004D2F46, "__rtindfpop", "87disp (island)"),
    (0x004D2F48, "__rtindfnpop", "87disp (island)"),
    (0x004D2F63, "__rtchsifneg", "87disp (island)"),
    (0x004CEF65, "__adj_fpatan", "adj_fdiv (island)"),
    (0x004188AD, "__rdtsc", "_prof (island)"),
]
# differences the rewrite makes on purpose (crt_start.cpp's comments): (v10, normalised rewrite line)
EXPECTED = {
    (0x004CF5A0, "call <wmcs_islands>"): "the islands' installer, first thing (a no-op in the harness build)",
}


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = Path(sys.argv[1])
    ctx = Ctx(exe)
    v10 = Image(ROOT / "out" / "race_v10.exe")
    harness = Image(exe)
    alias_rev = {v: a for a, v in ctx.alias.items()}
    rows, details, bad = [], [], 0
    for va, name, obj in CHECKS:
        rw = ctx.fn.get(va, ctx.island.get(va, (alias_rev.get(va), None)))[0]
        if rw is None:
            rows.append((name, va, 0, 0, 0, "NO REWRITE FOUND", obj))
            bad += 1
            continue
        o = Side(v10, va, False, ctx)
        r = Side(harness, rw, True, ctx)
        ca, cb = o.canonical(), r.canonical()
        a, b = [t for t, _ in ca], [t for t, _ in cb]
        diffs = []
        ops = difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes()
        o2r = {}
        for tag, i1, i2, j1, j2 in ops:
            if tag == "equal":
                o2r.update({i1 + k: j1 + k for k in range(i2 - i1)})
        for tag, i1, i2, j1, j2 in ops:
            if tag == "equal":
                for k in range(i2 - i1):                       # the same instruction: do its labels land on the same one?
                    la, lb = ca[i1 + k][1], cb[j1 + k][1]
                    if [o2r.get(x) for x in la] != lb:
                        diffs.append((o.where(i1 + k), f"{a[i1 + k]} -> {la}", r.where(j1 + k),
                                      f"{b[j1 + k]} -> {lb} (orig's target aligns with {[o2r.get(x) for x in la]})", None))
                continue
            for k in range(max(i2 - i1, j2 - j1)):
                oa = a[i1 + k] if i1 + k < i2 else None
                ob = b[j1 + k] if j1 + k < j2 else None
                why = EXPECTED.get((va, ob)) if oa is None else None
                diffs.append((o.where(i1 + k) if oa else "-", oa or "", r.where(j1 + k) if ob else "-", ob or "", why))
        unexpected = [d for d in diffs if not d[4]]
        status = "identical" if not diffs else "expected only" if not unexpected else f"{len(unexpected)} DIFFER"
        bad += bool(unexpected)
        rows.append((name, va, rw, len(a), len(b), status, obj))
        if diffs:
            details.append((name, diffs))
        if "-v" in sys.argv:
            print(f"---- {name}")
            for k in range(max(len(a), len(b))):
                print(f"  {k:4d} {a[k] + str(ca[k][1]) if k < len(a) else '':52s} | {b[k] + str(cb[k][1]) if k < len(b) else ''}")
    print(f"{'routine':32s} {'v1.0':>10s} {'rewrite':>10s} {'orig':>5s} {'new':>5s}  result          object")
    for name, va, rw, na, nb, st, obj in rows:
        print(f"{name:32s} {va:#010x} {rw:#010x} {na:5d} {nb:5d}  {st:15s} {obj}")
    for name, diffs in details:
        print(f"\n{name}:")
        for wo, oa, wr, ob, why in diffs:
            print(f"  v1.0 {wo:>16s}  {oa:40s} | rewrite {wr:>16s}  {ob}" + (f"   [expected: {why}]" if why else ""))
    print(f"\n{len(rows)} routines; {bad} with unexpected differences")
    print("(the exception machinery written in C -- _XcptFilter, xcptlookup, _raise_exc, _handle_exc, _87except --"
          " is compared with the disassembly by reading, and run directly by world_crt_start.exe's xcpt / fpexc"
          " sections)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
