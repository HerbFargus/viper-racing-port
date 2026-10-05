"""Generate hook/standalone.inc: what viperport.exe's standalone mode fills with int3, keeps, and names.

    python tools/gen_standalone.py            (needs out/race_v10.exe, out/inventory.csv, hook/prologues.inc)

Standalone (loader/viperport_main.cpp + hook/standalone.cpp), every v1.0 race.exe function in the inventory is either
hooked (its first 5 bytes a jmp into the port DLL) or never runs, so its code is overwritten with int3 and an int3
reached is caught and named. Only CODE is overwritten: the bytes of the instructions a recursive descent reaches from
each function's entry (and from every other address something jumps or calls into, SEH filters and handlers, switch
targets). Data MSVC put in .text -- switch tables, the import libraries' data after their thunks, alignment padding --
stays, and so does every byte of .text the rewrites read at run time (KEEP, below).

Writes (derived only from addresses and instruction lengths -- no game bytes):
  VP_SA_FUNCS  {va, size, first span, spans, name}   every inventory function, sorted
  VP_SA_SPANS  {start, end}                           its code bytes, as runs
  VP_SA_KEEP   {start, bytes, why}                    .text bytes the rewrites read at run time: never filled
  VP_SA_THUNKS {va, IAT slot}                         import thunks (jmp [slot]): left as they are, checked at run time
and prints the audit of every .text address the DLL's sources name (call/jump/read), by kind.
"""
from __future__ import annotations

import csv
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
HOOK = ROOT / "hook"
BASE = 0x400000
TEXT = (0x401000, 0x401000 + 0xD948D)
RDATA = (0x4DB000, 0x4E1000)
IAT = (0x5D7410, 0x5D7410 + 0x320)
EXCEPT_HANDLER3 = 0x4D4490


def load():
    exe = (OUT / "race_v10.exe").read_bytes()
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    n = struct.unpack_from("<H", exe, pe + 6)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    secs = [struct.unpack_from("<8sIIII", exe, pe + 24 + opt + i * 40) for i in range(n)]
    return exe, secs


def reader(exe, secs):
    def at(va: int, n: int) -> bytes:
        for _nm, vs, vaddr, rs, ro in secs:
            if vaddr <= va - BASE < vaddr + max(vs, rs):
                o = va - BASE - vaddr + ro
                return exe[o:o + n]
        return b""
    return at


def short_name(mangled: str, demangled: str) -> str:
    """?Update@Obstacle@@UAEXXZ -> Obstacle::Update; _strchr -> strchr; ??0Car@@ -> Car::Car."""
    m = mangled
    if m.startswith("?"):
        special = {"?0": None, "?1": "~", "?_G": "`scalar deleting destructor'", "?_E": "`vector deleting destructor'"}
        body = m[1:]
        prefix = None
        for k, v in special.items():
            if body.startswith(k):
                body, prefix = body[len(k):], (k, v)
                break
        parts = body.split("@@")[0].split("@")
        parts = [p for p in parts if p]
        if prefix:
            k, v = prefix
            cls = parts[0] if parts else "?"
            name = cls if v is None else (v + cls if v == "~" else v)
            return "::".join(list(reversed(parts)) + [name])
        return "::".join(reversed(parts)) if parts else m
    if m.startswith("_") and "@" in m:                      # _name@12 (stdcall)
        return m[1:].split("@")[0]
    if m.startswith("_"):
        return m[1:]
    return m.split("@")[0] or demangled[:60]


def main():
    exe, secs = load()
    rd = reader(exe, secs)
    inv = [r for r in csv.DictReader(open(OUT / "inventory.csv", encoding="utf-8"))]
    funcs = sorted((int(r["va"], 16), int(r["size"]), short_name(r["name"], r["demangled"]), r) for r in inv)
    starts = [f[0] for f in funcs]
    hooked = sorted({int(m, 16) for m in re.findall(r"\{0x([0-9a-f]{8}),", (HOOK / "prologues.inc").read_text())})
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    import bisect

    def owner(va: int):
        k = bisect.bisect_right(starts, va) - 1
        if k >= 0 and funcs[k][0] <= va < funcs[k][0] + funcs[k][1]:
            return k
        return None

    # ---- every address something jumps or calls into, across .text (direct rel32/rel8), plus the hooked addresses
    seeds: dict[int, set[int]] = {}
    text = rd(TEXT[0], TEXT[1] - TEXT[0])

    def add_seed(va):
        k = owner(va)
        if k is not None:
            seeds.setdefault(k, set()).add(va)

    for va in hooked:
        add_seed(va)

    # ---- recursive descent per function; two passes so a fall-through never runs into a table found later
    tables: dict[int, list[tuple[int, int]]] = {}      # function -> [(start, end)] data
    reached: dict[int, set[tuple[int, int]]] = {}       # function -> {(addr, len)}
    unknown_flow = 0

    def descend(k: int, extra: set[int], stop: list[tuple[int, int]]):
        va0, size, _nm, _r = funcs[k]
        end = va0 + size
        body = rd(va0, size)
        todo = [va0] + sorted(extra)
        seen: set[int] = set()
        ins_out: set[tuple[int, int]] = set()
        tabs: list[tuple[int, int]] = []
        while todo:
            pc = todo.pop()
            while va0 <= pc < end and pc not in seen:
                if any(a <= pc < b for a, b in stop) or any(a <= pc < b for a, b in tabs):
                    break
                seen.add(pc)
                try:
                    ins = next(md.disasm(body[pc - va0:pc - va0 + 16], pc), None)
                except Exception:
                    ins = None
                if ins is None:
                    break
                ins_out.add((pc, ins.size))
                mn = ins.mnemonic
                # switch tables: a memory operand inside this function's own range
                for op in ins.operands:
                    if op.type == X86_OP_MEM and op.mem.segment == 0 and va0 <= (op.mem.disp & 0xFFFFFFFF) < end:
                        t = op.mem.disp & 0xFFFFFFFF
                        if op.size == 4:                       # a jump table: dwords aimed into this function
                            e = t
                            while e + 4 <= end:
                                v = struct.unpack_from("<I", body, e - va0)[0]
                                if not (va0 <= v < end):
                                    break
                                todo.append(v)
                                e += 4
                            if e > t:
                                tabs.append((t, e))
                        elif op.size == 1:                     # an index table: bytes up to the next known thing
                            e = t
                            while e < end and not any(a <= e < b for a, b in tabs if a != t) and (e, 1) not in ins_out:
                                e += 1
                            if e > t:
                                tabs.append((t, e))
                # SEH: push scopetable; push __except_handler3 -> filters and handlers are code
                if mn == "push" and ins.operands and ins.operands[0].type == X86_OP_IMM:
                    imm = ins.operands[0].imm & 0xFFFFFFFF
                    if RDATA[0] <= imm < RDATA[1]:
                        nxt = next(md.disasm(body[pc + ins.size - va0:pc + ins.size - va0 + 16], pc + ins.size), None)
                        if nxt and nxt.mnemonic == "push" and nxt.operands and (nxt.operands[0].imm & 0xFFFFFFFF) == EXCEPT_HANDLER3:
                            i = 0
                            while i < 32:
                                ent = rd(imm + 12 * i, 12)
                                if len(ent) < 12:
                                    break
                                _lvl, flt, hnd = struct.unpack("<iII", ent)
                                if not (va0 <= hnd < end) or (flt and not va0 <= flt < end):
                                    break
                                todo.append(hnd)
                                if flt:
                                    todo.append(flt)
                                i += 1
                if mn in ("ret", "retf", "int3", "hlt", "ud2"):
                    break
                if mn == "jmp" or mn.startswith("j") or mn in ("loop", "loope", "loopne", "jecxz", "jcxz"):
                    if ins.operands and ins.operands[0].type == X86_OP_IMM:
                        tgt = ins.operands[0].imm & 0xFFFFFFFF
                        if va0 <= tgt < end:
                            todo.append(tgt)
                        if mn == "jmp":
                            break
                    elif mn == "jmp":
                        break                                   # jmp reg / [mem]: tables were noted above
                pc += ins.size
        return ins_out, tabs

    def run(k):
        ins, tabs = descend(k, seeds.get(k, set()), [])
        if tabs:
            ins, tabs2 = descend(k, seeds.get(k, set()), tabs)
            tabs = sorted(set(tabs) | set(tabs2))
        reached[k] = ins
        tables[k] = tabs

    def cross_targets(k):
        # direct calls and jumps from k's code into another function's interior: entries there too
        va0, size, _nm, _r = funcs[k]
        body = rd(va0, size)
        new = set()
        for a, n in reached[k]:
            ins = next(md.disasm(body[a - va0:a - va0 + n], a), None)
            if ins and (ins.mnemonic == "call" or ins.mnemonic.startswith("j")) and ins.operands                     and ins.operands[0].type == X86_OP_IMM:
                t = ins.operands[0].imm & 0xFFFFFFFF
                j = owner(t)
                if j is not None and j != k and t != funcs[j][0] and t not in seeds.get(j, set()):
                    new.add((j, t))
        return new

    for k in range(len(funcs)):
        run(k)
    pending = set(range(len(funcs)))
    while pending:
        grow = set()
        for k in pending:
            for j, t in cross_targets(k):
                seeds.setdefault(j, set()).add(t)
                grow.add(j)
        for j in grow:
            run(j)
        pending = grow

    # ---- import thunks: jmp [IAT slot]
    thunks = []
    for k, (va, size, nm, r) in enumerate(funcs):
        b = rd(va, 6)
        if b[:2] == b"\xff\x25":
            slot = struct.unpack_from("<I", b, 2)[0]
            if IAT[0] <= slot < IAT[1]:
                thunks.append((va, slot, nm))

    # ---- MSVC's alignment padding: int3, nop, mov edi,edi, lea r,[r+0] (and the linker's zeros)
    plain = Cs(CS_ARCH_X86, CS_MODE_32)

    def is_padding(b: bytes, va: int) -> bool:
        if all(x in (0xCC, 0x90, 0x00) for x in b):
            return True
        n = 0
        for ins in plain.disasm(b, va):
            r, _, m = ins.op_str.partition(", ")
            ok = ins.mnemonic in ("nop", "int3") or (ins.mnemonic == "mov" and r == m) or (ins.mnemonic == "lea" and m in ("[%s]" % r, "[%s + 0]" % r) or (ins.mnemonic == "add" and ins.op_str == "eax, 0"))
            if not ok:
                return False
            n += ins.size
        return n == len(b)

    # ---- spans
    # Code no direct branch reaches is reached through a table in .data (adj_fdiv_r's handlers at 0x50252e, 87disp's
    # result routines, _fFMOD, the trig entries): every run left that isn't padding, a switch table or known data is
    # decoded linearly and filled as code too -- an int3 there is how an unported one shows up.
    DATA_IN_TEXT = {0x4D4488: 8}      # "VC20XC00": exsup3's signature just before __except_handler3
    span_rows, func_rows = [], []
    stats = dict(code=0, unreached=0, padding=0, tables=0, linear=0)
    unreached_list, linear_runs = [], []
    thunk_set = {t[0] for t in thunks}

    def coverage(k, va, size):
        covered = bytearray(size)
        for a, n in reached[k]:
            covered[a - va:a - va + n] = b"\1" * n
        for a, b in tables[k]:
            covered[a - va:b - va] = b"\2" * (b - a)
        for a, n in DATA_IN_TEXT.items():
            if va <= a < va + size:
                covered[a - va:a - va + n] = b"\3" * n
        return covered

    def gaps(covered, body, va):
        i, size = 0, len(covered)
        while i < size:
            if covered[i]:
                i += 1
                continue
            j = i
            while j < size and not covered[j]:
                j += 1
            yield i, j, is_padding(body[i:j], va + i)
            i = j

    for k, (va, size, nm, r) in enumerate(funcs):
        first = len(span_rows)
        if va not in thunk_set:
            body = rd(va, size)
            for i, j, pad in list(gaps(coverage(k, va, size), body, va)):
                if pad:
                    continue
                s = i
                while s < j and body[s] in (0xCC, 0x90):     # leading alignment
                    s += 1
                got = 0
                for ins in plain.disasm(body[s:j], va + s):
                    reached[k].add((ins.address, ins.size))
                    got += ins.size
                if got:
                    linear_runs.append((va + s, va + s + got, nm))
                    stats["linear"] += got
            covered = coverage(k, va, size)
            cover = sorted(reached[k])
            runs = []
            for a, n in cover:
                if runs and a <= runs[-1][1]:
                    runs[-1][1] = max(runs[-1][1], a + n)
                else:
                    runs.append([a, a + n])
            for a, b in runs:
                span_rows.append((a, b))
                stats["code"] += b - a
            for a, b in tables[k]:
                stats["tables"] += b - a
            for i, j, pad in gaps(covered, body, va):
                if pad:
                    stats["padding"] += j - i
                else:
                    stats["unreached"] += j - i
                    unreached_list.extend(range(va + i, va + j))
        func_rows.append((va, size, first, len(span_rows) - first, nm))

    # ---- what the rewrites read in .text at run time (never filled)
    src = "".join((HOOK / f).read_text(encoding="utf-8") for f in ("viperport.cpp", "texture_table_fields.inc", "res_table_fields.inc"))
    m1 = sorted({int(at, 16) + int(off) for at, off in re.findall(r"\{\s*(0x[0-9a-fA-F]{6,8}),\s*(\d+),", src)})
    keep = [(a, 4, "an operand M1 patches (read with m1_operand)") for a in m1 if TEXT[0] <= a < TEXT[1]]
    keep += [
        (0x0048635F, 20, "vehicle_list_geometry (menu_options.cpp) reads the Vehicle list's push immediates"),
        (0x0045EC5F, 8, "aspectfix_r0 (gx_dd.cpp) tells vrmod's aspectfix code by its first instructions"),
        (0x0045EC86, 2, "aspectfix_r0 (gx_dd.cpp) checks aspectfix's fmul [-0.5]"),
    ]
    # vrmod's values in the code (port.h, vrmod_operand / vrmod_operand_at): the rewrites read them at run time
    vrm = set()
    for f in sorted(HOOK.glob("*.cpp")):
        for m in re.finditer(r"\bvrmod_operand(?:_at)?\(\s*0x([0-9a-fA-F]+)\s*\)", f.read_text(encoding="utf-8")):
            vrm.add(int(m.group(1), 16))
    keep += [(a, 4, "vrmod's value, read with vrmod_operand") for a in sorted(vrm) if TEXT[0] <= a < TEXT[1]]
    # every m1_operand / m1_operand_hooked literal must be one of M1's operands
    lits = set()
    for f in sorted(HOOK.glob("*.cpp")):
        t = f.read_text(encoding="utf-8")
        for m in re.finditer(r"m1_operand\(\s*0x([0-9a-fA-F]+)\s*\)", t):
            lits.add(int(m.group(1), 16))
        for m in re.finditer(r"m1_operand_hooked\(\s*0x([0-9a-fA-F]+)\s*,\s*0x[0-9a-fA-F]+\s*,\s*0x[0-9a-fA-F]+\s*,\s*0x([0-9a-fA-F]+)", t):
            lits.add(int(m.group(1), 16))
            lits.add(int(m.group(2), 16))
    stray = sorted(a for a in lits if TEXT[0] <= a < TEXT[1] and a not in m1)
    for a in stray:
        keep.append((a, 4, "read with m1_operand (not an M1 field)"))

    # ---- audit: every .text address named in the DLL's code (comments and strings left out)
    hooked_set = set(hooked)
    m1_set = set(m1)
    keep_ranges = [(a, a + n) for a, n, _ in keep]
    kinds: dict[str, list[str]] = {}
    named: dict[int, str] = {}                            # unhooked entries the DLL's code names: checked at run time
    start_set = set(starts)
    M2_HOOKS = {0x412690, 0x412AC0, 0x412BF0, 0x412F00, 0x413040, 0x414620, 0x418BB0, 0x418F30, 0x418FF0, 0x418FD0,
                0x419230, 0x419250, 0x419360}
    pat = re.compile(r"\b0x0*(4[0-9a-dA-D][0-9a-fA-F]{4})\b")
    for f in sorted(list(HOOK.glob("*.cpp")) + list(HOOK.glob("*.h"))):
        if f.name in ("viperport.cpp", "standalone.cpp"):
            continue                                  # M1's own patch tables (install time) / this file's
        for ln, line in enumerate(f.read_text(encoding="utf-8").split("\n"), 1):
            code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', re.sub(r"//.*", "", line))
            for m in pat.finditer(code):
                a = int(m.group(1), 16)
                if not TEXT[0] <= a < TEXT[1]:
                    continue
                if a in hooked_set:
                    continue                              # a hooked entry: runs the rewrite
                if a in thunk_set:
                    kinds.setdefault("import thunk (left in place)", []).append(f"{f.name}:{ln} {a:08x}")
                elif any(x <= a < y for x, y in keep_ranges) or a - 1 in m1_set or a in m1_set:
                    continue                              # kept: read at run time
                elif "PORT_FN" in code or "HARNESS_ONLY_FN" in code or "detour" in code:
                    kinds.setdefault("registered but not in prologues.inc (rerun gen_port_tables.py)", []).append(f"{f.name}:{ln} {a:08x}")
                elif a in start_set:
                    named.setdefault(a, f"{f.name}:{ln}")
                    if a in M2_HOOKS:
                        continue                          # jmp-hooked by platform.cpp at install (M2)
                    kinds.setdefault("an UNHOOKED function's entry (int3 in standalone unless hooked at install)", []).append(f"{f.name}:{ln} {a:08x}")
                else:
                    kinds.setdefault("inside a function (install-time patch site, or check)", []).append(f"{f.name}:{ln} {a:08x}")

    # ---- write
    def q(s):
        return s.replace("\\", "\\\\").replace('"', "'")

    lines = ["// generated by tools/gen_standalone.py: the standalone int3 fill (hook/standalone.cpp) -- addresses and lengths only",
             "#ifdef VP_SA_FUNCS", "// {va, size, first span, spans, name}"]
    lines += [f'    {{0x{va:08x}, {size}, {first}, {n}, "{q(nm)}"}},' for va, size, first, n, nm in func_rows]
    lines += ["#endif", "#ifdef VP_SA_SPANS", "// {start, end}: instruction bytes a recursive descent reaches"]
    lines += [f"    {{0x{a:08x}, 0x{b:08x}}}," for a, b in span_rows]
    lines += ["#endif", "#ifdef VP_SA_KEEP", "// {start, bytes, why}: read by the rewrites at run time, never filled"]
    lines += [f'    {{0x{a:08x}, {n}, "{q(w)}"}},' for a, n, w in sorted(keep)]
    lines += ["#endif", "#ifdef VP_SA_THUNKS", "// {va, IAT slot}: jmp [slot], left in place"]
    lines += [f"    {{0x{va:08x}, 0x{slot:08x}}},   // {nm}" for va, slot, nm in thunks]
    lines += ["#endif", "#ifdef VP_SA_NAMED", "// {va, where}: unhooked entries the DLL's code calls or names: must be hooked live"]
    lines += [f'    {{0x{a:08x}, "{w}"}},' for a, w in sorted(named.items())]
    lines += ["#endif"]
    (HOOK / "standalone.inc").write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"standalone.inc: {len(func_rows)} functions, {len(span_rows)} code spans ({stats['code']} bytes), "
          f"{len(keep)} kept reads, {len(thunks)} import thunks")
    print(f"  left unfilled inside functions: {stats['tables']} bytes of switch tables, {stats['padding']} of padding, "
          f"{stats['unreached']} other bytes no branch reaches (import-library data after thunks, ...)")
    runs = []
    for a in unreached_list:
        if runs and a == runs[-1][1]:
            runs[-1][1] = a + 1
        else:
            runs.append([a, a + 1])
    big = [(a, b) for a, b in runs if b - a >= 8]
    print(f"  filled by linear decoding (code only a .data table reaches): {stats['linear']} bytes in {len(linear_runs)} runs")
    for a, b, nm in linear_runs:
        print(f"    {a:08x}-{b:08x} ({b - a}) in {nm}")
    print(f"  unreached runs >= 8 bytes: {len(big)}")
    for a, b in big[:40]:
        k = owner(a)
        print(f"    {a:08x}-{b:08x} ({b - a}) in {funcs[k][2]}")
    for kind, items in sorted(kinds.items()):
        print(f"  audit: {kind}: {len(items)}")
        for it in items[:60]:
            print(f"    {it}")


if __name__ == "__main__":
    sys.exit(main())
