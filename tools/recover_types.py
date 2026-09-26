"""Recover C++ class layouts from v1.0 race.exe and its linker map, for Ghidra and for the hook DLL.

What the binary gives away, class by class:
  * methods: every __thiscall in the map, by its demangled class name
  * vtables: the map's `vftable' symbols; each slot is read and named after the function it points at
  * size: `push N; call operator new` followed by a call to that class's constructor
  * base class: the constructor's first call to another constructor on `this`
  * fields: every [this + d] a method touches (this = ecx on entry, followed through register copies
    and lea), with its width and whether x87 treats it as a float; `lea ecx,[this+d]; call Y::m`
    marks an embedded Y at d
  * names: one-instruction accessors (GetMass: `fld [ecx+d]; ret` -> mass), then tools/type_names.py,
    the hand-written overlay of everything already worked out elsewhere

    python tools/recover_types.py  ->  out/types.json
"""
import collections
import csv
import json
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM, CS_OP_REG, Cs
from capstone.x86 import X86_REG_ECX, X86_REG_ESP

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import type_names  # noqa: E402

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


class Image:
    def __init__(self, path):
        self.b = Path(path).read_bytes()
        pe = struct.unpack_from("<I", self.b, 0x3C)[0]
        n, opt = struct.unpack_from("<H", self.b, pe + 6)[0], struct.unpack_from("<H", self.b, pe + 20)[0]
        self.base = struct.unpack_from("<I", self.b, pe + 52)[0]
        self.secs = [struct.unpack_from("<IIII", self.b, pe + 24 + opt + 40 * k + 8) for k in range(n)]

    def read(self, va, n):
        for vs, v, rs, rp in self.secs:
            o = va - self.base - v
            if 0 <= o < rs:
                return self.b[rp + o: rp + o + n]
        return b""

    def dword(self, va):
        d = self.read(va, 4)
        return struct.unpack("<I", d)[0] if len(d) == 4 else None


METHOD = re.compile(r"__thiscall ((?:[\w<> ,*]+::)*?[\w<> ,*]+)::([~\w]+|`[^']+')\(")


def load_symbols():
    syms = list(csv.DictReader(open(ROOT / "out" / "symbols.csv", encoding="utf-8")))
    inv = {int(r["va"], 16): int(r["size"]) for r in csv.DictReader(open(ROOT / "out" / "inventory.csv", encoding="utf-8"))}
    names = collections.defaultdict(list)
    for r in syms:
        names[int(r["va"], 16)].append(r["demangled"])
    return syms, inv, names


def method_of(demangled):
    m = METHOD.search(demangled)
    return (m.group(1), m.group(2)) if m else None


def disasm(img, va, size):
    return list(md.disasm(img.read(va, size), va))


def reg_name(r):
    return md.reg_name(r) if r else None


def widest(reg):
    """al/ax/eax -> eax: the 32-bit register a partial register belongs to."""
    table = {"al": "eax", "ah": "eax", "ax": "eax", "bl": "ebx", "bh": "ebx", "bx": "ebx", "cl": "ecx", "ch": "ecx",
             "cx": "ecx", "dl": "edx", "dh": "edx", "dx": "edx", "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp"}
    return table.get(reg, reg)


def trace_this(insns, entry_regs):
    """Walk a function linearly, tracking which registers hold `this + k`.

    Yields (instruction, operand, offset from this) for each memory operand based on such a register,
    and ('call', instruction, ecx offset) for each call made with ecx pointing into the object.
    Linear, not flow-sensitive: good enough for MSVC 4's straight-line use of this."""
    regs = dict(entry_regs)                            # register name -> offset from this
    for ins in insns:
        if ins.mnemonic == "call":
            yield "call", ins, regs.get("ecx")
            for r in ("eax", "ecx", "edx"):
                regs.pop(r, None)
            continue
        for op in ins.operands:
            if op.type == CS_OP_MEM and op.mem.base and op.mem.segment == 0:
                b = reg_name(op.mem.base)
                if b in regs and 0 <= regs[b] + op.mem.disp < 0x10000:
                    yield "mem", ins, (op, regs[b] + op.mem.disp, bool(op.mem.index), op.mem.scale)
        # register updates
        _, written = ins.regs_access()
        written = {widest(reg_name(r)) for r in written} - {"esp", "eflags", None}
        new = None
        ops = ins.operands
        if ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == CS_OP_REG and ops[1].type == CS_OP_REG:
            new = (reg_name(ops[0].reg), regs.get(reg_name(ops[1].reg)))
        elif ins.mnemonic == "lea" and ops[1].mem.index == 0 and reg_name(ops[1].mem.base) in regs:
            new = (reg_name(ops[0].reg), regs[reg_name(ops[1].mem.base)] + ops[1].mem.disp)
        elif ins.mnemonic == "add" and ops[0].type == CS_OP_REG and ops[1].type == CS_OP_IMM and reg_name(ops[0].reg) in regs:
            new = (reg_name(ops[0].reg), regs[reg_name(ops[0].reg)] + ops[1].imm)
        for r in written:
            regs.pop(r, None)
        if new and new[1] is not None:
            regs[new[0]] = new[1]


def kind_of(ins, op):
    m = ins.mnemonic
    if m == "lea":
        return "addr"
    if m.startswith("f") and m not in ("fnstsw",):
        return {4: "float", 8: "double", 10: "ldouble"}.get(op.size, f"int{op.size * 8}")
    return f"int{op.size * 8}"


def accessor_field(insns):
    """(offset, kind, 'get'|'set') if the function is a one-field accessor on ecx, else None."""
    body = []
    for i in insns:
        if i.mnemonic == "ret":
            break
        body.append(i)
    else:
        return None
    if len(body) == 1:
        i = body[0]
        if i.mnemonic in ("mov", "fld", "lea", "movsx", "movzx") and i.operands[-1].type == CS_OP_MEM:
            m = i.operands[-1].mem
            if m.base == X86_REG_ECX and m.index == 0:
                return m.disp, kind_of(i, i.operands[-1]), "get"
    if len(body) == 2:
        a, b = body
        if a.operands and a.operands[-1].type == CS_OP_MEM and a.operands[-1].mem.base == X86_REG_ESP \
                and b.operands and b.operands[0].type == CS_OP_MEM and b.operands[0].mem.base == X86_REG_ECX \
                and b.operands[0].mem.index == 0:
            return b.operands[0].mem.disp, kind_of(b, b.operands[0]), "set"
    return None


def field_name_from_method(method):
    for p in ("Get", "Set", "get_", "set_", "get", "set"):
        if method.startswith(p) and len(method) > len(p):
            n = method[len(p):]
            return n[0].lower() + n[1:]
    if method.startswith(("Is", "Has", "Can")) and len(method) > 2:
        return method[0].lower() + method[1:]
    return None


def main():
    img = Image(ROOT / "out" / "race_v10.exe")
    syms, inv, names = load_symbols()

    methods = collections.defaultdict(list)            # class -> [(va, method, demangled)]
    for va, ns in names.items():
        for d in ns:
            mo = method_of(d)
            if mo:
                methods[mo[0]].append((va, mo[1], d))
    func_class = {}
    for c, ms in methods.items():
        for va, m, d in ms:
            func_class.setdefault(va, (c, m))
    ctors = {va: c for c, ms in methods.items() for va, m, d in ms if m == c.split("::")[-1]}

    # ---- vtables ----
    vtables = {}
    vt_syms = sorted((int(r["va"], 16), r["demangled"]) for r in syms if r["name"].startswith("??_7"))
    all_syms = sorted({int(r["va"], 16) for r in syms})
    for k, (va, d) in enumerate(vt_syms):
        c = d[len("const "):d.index("::`vftable'")]
        nxt = next((s for s in all_syms if s > va), va + 0x400)
        slots = []
        for a in range(va, nxt, 4):
            f = img.dword(a)
            if f is None or f not in names:
                break
            fc = func_class.get(f)
            nm = fc[1] if fc else names[f][0]
            slots.append({"slot": a - va, "va": f"{f:08x}", "name": nm,
                          "impl": (fc[0] + "::" + fc[1]) if fc else names[f][0]})
        vtables.setdefault(c, {"va": f"{va:08x}", "slots": slots})

    # ---- sizes (operator new + constructor) and bases (constructor's first base constructor) ----
    sizes = collections.defaultdict(collections.Counter)
    for fva, fsize in inv.items():
        insns = disasm(img, fva, fsize)
        for k, ins in enumerate(insns):
            # any allocator (operator new, MemAlloc...): push N; call alloc; ... mov ecx, eax; call ctor
            if ins.mnemonic == "call" and ins.operands[0].type == CS_OP_IMM and ins.operands[0].imm not in ctors:
                push = next((p for p in reversed(insns[max(0, k - 4):k]) if p.mnemonic == "push"), None)
                if not push or push.operands[0].type != CS_OP_IMM:
                    continue
                ecx_from_eax = False
                for j in insns[k + 1:k + 16]:
                    if j.mnemonic == "mov" and j.op_str == "ecx, eax":
                        ecx_from_eax = True
                    if j.mnemonic == "call" and j.operands[0].type == CS_OP_IMM:
                        if j.operands[0].imm in ctors and ecx_from_eax:
                            sizes[ctors[j.operands[0].imm]][push.operands[0].imm] += 1
                        break
    bases = {}
    for cva, c in ctors.items():
        if cva not in inv:
            continue
        for ev in trace_this(disasm(img, cva, inv[cva]), {"ecx": 0}):
            if ev[0] == "call" and ev[2] == 0:
                t = ev[1].operands[0].imm if ev[1].operands[0].type == CS_OP_IMM else None
                if t in ctors and ctors[t] != c:
                    bases.setdefault(c, ctors[t])
                    break
            if ev[0] == "mem":                             # the vtable store ends the base-construction part
                ins, (op, off, idx, sc) = ev[1], ev[2]
                if off == 0 and ins.mnemonic == "mov" and ins.operands[1].type == CS_OP_IMM and \
                        f"{ins.operands[1].imm:08x}" == vtables.get(c, {}).get("va"):
                    break

    # ---- fields ----
    acc = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))   # class -> off -> kind counts
    width = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    arrays = collections.defaultdict(dict)
    embedded = collections.defaultdict(dict)
    named = collections.defaultdict(dict)
    for c, ms in methods.items():
        for va, m, d in ms:
            if va not in inv:
                continue
            insns = disasm(img, va, inv[va])
            a = accessor_field(insns)
            if a and m != c.split("::")[-1]:
                nm = field_name_from_method(m)
                if nm and a[0] not in named[c]:
                    named[c][a[0]] = (nm, f"{c}::{m}")
            for ev in trace_this(insns, {"ecx": 0}):
                if ev[0] == "mem":
                    ins, (op, off, idx, scale) = ev[1], ev[2]
                    k = kind_of(ins, op)
                    acc[c][off][k] += 1
                    width[c][off][op.size] += 1
                    if idx and scale > 1:
                        arrays[c][off] = scale
                elif ev[0] == "call" and ev[2]:
                    t = ev[1].operands[0].imm if ev[1].operands[0].type == CS_OP_IMM else None
                    if t in func_class and func_class[t][0] != c:
                        embedded[c].setdefault(ev[2], func_class[t][0])

    # ---- assemble ----
    classes = {}
    for c in sorted(set(methods) | set(vtables)):
        size = sizes[c].most_common(1)[0][0] if sizes[c] else None
        fields = {}
        for off, kinds in acc[c].items():
            k = kinds.most_common(1)[0][0]
            if k == "addr":
                real = [x for x in kinds if x != "addr"]
                if real:
                    k = max(real, key=lambda x: kinds[x])
            w = {"float": 4, "double": 8, "ldouble": 10, "int8": 1, "int16": 2, "int32": 4, "addr": 0}.get(k, 4)
            fields[off] = {"offset": off, "kind": k, "size": w, "uses": sum(kinds.values())}
            if off in arrays[c]:
                fields[off]["array_stride"] = arrays[c][off]
            if off in embedded[c]:
                fields[off]["embedded"] = embedded[c][off]
            if off in named[c]:
                fields[off]["name"], fields[off]["from"] = named[c][off]
        for off, t in embedded[c].items():
            fields.setdefault(off, {"offset": off, "kind": "addr", "size": 0, "uses": 0, "embedded": t})
        classes[c] = {"size": size, "size_votes": dict(sizes[c]) if sizes[c] else {}, "base": bases.get(c),
                      "vtable": vtables.get(c), "methods": len(methods.get(c, [])),
                      "fields": [fields[o] for o in sorted(fields)]}

    # sizes the allocations don't give: embedded arrays (equal spacing between instances of one class
    # inside another), then abstract bases, from the furthest field their own methods touch
    for c, info in classes.items():
        by_type = collections.defaultdict(list)
        for off, t in embedded[c].items():
            by_type[t].append(off)
        for t, offs in by_type.items():
            offs.sort()
            gaps = {b - a for a, b in zip(offs, offs[1:])}
            if len(offs) >= 3 and len(gaps) == 1 and t in classes and not classes[t]["size"]:
                classes[t]["size"], classes[t]["size_from"] = gaps.pop(), f"spacing of the {t}s in {c}"
    order = []

    def visit(c, seen=()):
        if c in order or c in seen or c not in classes:
            return
        if classes[c]["base"]:
            visit(classes[c]["base"], seen + (c,))
        order.append(c)
    for c in classes:
        visit(c)
    for c in order:
        info = classes[c]
        if info["size"]:
            continue
        def extent(f):                              # an embedded member reaches as far as its class
            t = f.get("embedded")
            inner = classes.get(t, {}).get("size") or type_names.SIZES.get(t) or \
                type_names.PLAIN.get(t, (0,))[0] if t else 0
            return f["offset"] + max(f["size"], 4, inner or 0)
        end = max((extent(f) for f in info["fields"]), default=4 if info["vtable"] else 0)
        b = info["base"]
        bsize = classes.get(b, {}).get("size") or type_names.PLAIN.get(b, (0,))[0] if b else 0
        if bsize:
            end = max(end, bsize)
        info["size"], info["size_from"] = (end + 3) & ~3, "estimate: the furthest field its own methods touch"

    # fields a derived class touches inside its base's extent belong to the base (walk bases first)
    def base_size(c):
        return classes[c]["size"] if c in classes and classes[c]["size"] else None

    for c in reversed(order):
        info = classes[c]
        b = info["base"]
        bs = base_size(b) if b else None
        if b and bs and b in classes:
            mine, theirs = [], {f["offset"]: f for f in classes[b]["fields"]}
            for f in info["fields"]:
                if f["offset"] < bs:
                    theirs.setdefault(f["offset"], dict(f, uses=f["uses"], via=c))
                else:
                    mine.append(f)
            info["fields"] = mine
            classes[b]["fields"] = [theirs[o] for o in sorted(theirs)]

    type_names.apply(classes)
    out = ROOT / "out" / "types.json"
    out.write_text(json.dumps(classes, indent=1))
    write_tsv(classes, order, ROOT / "out" / "types.tsv")
    have = [c for c in classes if classes[c]["size"]]
    print(f"{len(classes)} classes: {len(have)} with a size, {sum(1 for c in classes if classes[c]['base'])} with a base, "
          f"{sum(1 for c in classes if classes[c]['vtable'])} with a vtable, "
          f"{sum(len(v['fields']) for v in classes.values())} fields "
          f"({sum(1 for v in classes.values() for f in v['fields'] if f.get('name'))} named) -> {out}")


def extended_slots(c, classes):
    """A root class's vtable slots, plus the ones its subclasses add after them. Only the root's
    structure holds the vtable pointer, so this is what calls through it are typed with; a slot the
    subclasses name differently gets both names."""
    own = classes[c]["vtable"]["slots"]
    if classes[c]["base"]:
        return own
    extra = collections.defaultdict(collections.Counter)
    impl = {}                                       # (slot, name) -> one implementation, for its signature
    for d, info in classes.items():
        r = d
        while classes.get(r, {}).get("base"):
            r = classes[r]["base"]
        if r != c or d == c or not info["vtable"]:
            continue
        for sl in info["vtable"]["slots"][len(own):]:
            extra[sl["slot"]][sl["name"]] += 1
            impl.setdefault((sl["slot"], sl["name"]), sl["va"])
    out = list(own)
    for slot in sorted(extra):
        if slot != out[-1]["slot"] + 4:
            break                                   # keep the table contiguous
        names = [n for n, _ in extra[slot].most_common(3)]
        out.append({"slot": slot, "va": impl[(slot, names[0])] if len(names) == 1 else "-",
                    "name": "_or_".join(names)})
    return out


KIND_TYPE = {"float": "float", "double": "double", "ldouble": "u10", "int8": "u1", "int16": "u2", "int32": "u4",
             "int64": "u8"}


def write_tsv(classes, order, path):
    """The layouts for tools/ApplyTypes.java, bases before the classes built on them:
        plain <name> <size>                      a struct with no methods (P3DBase, Matrix)
        class <name> <size> <base|-> <vtable va|->
        field <owner> <offset> <type> <name|-> <comment>
        vslot <class> <slot offset> <function va> <name>
    types: float double u1 u2 u4 u8 u10 int uint byte bool ptr ptr:<T> struct:<T> arr:<type>:<n>"""
    lines = []

    def row(*cells):
        lines.append("\t".join(str(c) for c in cells))

    for name, (size, fields) in type_names.PLAIN.items():
        if name in classes:                     # a class too (it has methods): keep its namespace
            row("class", name, size, "-", "-")
        else:
            row("plain", name, size)
        for off, typ, fname in fields:
            row("field", name, off, typ, fname, "")
    for c in order:
        if c in type_names.PLAIN:
            continue
        info = classes[c]
        vt = info["vtable"]
        row("class", c, info["size"], info["base"] or "-", vt["va"] if vt else "-")
        # typed (hand-named) fields first, so the recovered scalars inside them don't block them
        for f in sorted(info["fields"], key=lambda f: (not f.get("type"), f["offset"])):
            typ = f.get("type")
            if not typ and f.get("embedded") and classes.get(f["embedded"], {}).get("size"):
                typ = "struct:" + f["embedded"]
            if not typ:
                typ = KIND_TYPE.get(f["kind"])
            if not typ:
                continue
            note = f.get("from", "")
            if f.get("uses"):
                note = (note + "; " if note else "") + f"{f['uses']} uses" + (f" (from {f['via']})" if f.get("via") else "")
            row("field", c, f["offset"], typ, f.get("name") or "-", note)
        if vt:
            for sl in extended_slots(c, classes):
                row("vslot", c, sl["slot"], sl["va"], sl["name"])
    for va, (name, typ, note) in type_names.GLOBALS.items():
        row("global", f"{va:08x}", typ, name, note or "")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
