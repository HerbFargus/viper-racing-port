"""Generate hook/res_table_fields.inc: every instruction of the v1.0 race.exe that addresses the options table, the
language table or the open-file table, for M1 to repoint at bigger tables (hook/viperport.cpp, relocate_res_tables).

    python tools/gen_res_tables.py

    table 0  opt.obj     the options items, 256 x 0x90 at 0x55a078 (find_item appends with no bound)
    table 1  locale.obj  the languages (LangInfo), 8 x 0x40 at 0x509438 (enumerate_language_resources: no bound)
    table 2  file.obj    the open files {char name[0x100]; HANDLE handle}, 32 x 0x104 at 0x505ea8 (a resource set keeps
                         its archive open, so this is also the limit on resource sets loaded at once)
    table 3  file.obj    the same table's loop ends: FileBegin's `cmp eax, 0x508028` (the handle after the last) and
                         FileVerifyNoOpenFiles' `cmp esi, 0x507f28` (the name after the last) -- they move with its END

Every function's code is scanned for a 32-bit operand (an address or displacement) inside a table, its first byte to
its end: {instruction address, operand offset, v1.0 value, table}. The $E static initialisers are skipped (the
colour constants they set lie just outside the options table). The data sections are scanned for pointers into the
tables as well (there are none). The counts the tables are used with are statics outside them (0x55a05c, 0x509638,
0x505ea0), read as `dword ptr [..]`, and stay where they are. The script stops if anything else refers to a table's
end, or if an instruction compares with a table's capacity other than the one M1 patches by hand: a `cmp` with 0x100
or 8 in opt.obj / locale.obj, or with 0x20 in file.obj other than alloc_file's `cmp edx, 0x20` at 0x411650 (an imm8:
viperport.cpp's Field for it). The generated file isn't game code -- only addresses and the values found there.
"""
import csv
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

ROOT = Path(__file__).resolve().parent.parent
TABLES = [  # (name, base, end, object file, capacity immediates, loop-end values -> table index 3)
    ("options", 0x55a078, 0x55a078 + 0x100 * 0x90, "opt.obj", ("0x100", "8"), ()),
    ("languages", 0x509438, 0x509438 + 8 * 0x40, "locale.obj", ("0x100", "8"), ()),
    ("open-file", 0x505ea8, 0x505ea8 + 32 * 0x104, "file.obj", ("0x20",), (0x507f28, 0x508028)),
]
FILE_CAPACITY_CMP = 0x411650                 # alloc_file: cmp edx, 0x20 (patched by hand in viperport.cpp)


def main():
    exe = (ROOT / "out" / "race_v10.exe").read_bytes()
    inv = list(csv.DictReader(open(ROOT / "out" / "inventory.csv", encoding="utf-8")))
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec = struct.unpack_from("<H", exe, pe + 6)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        vs, va, rs, rp = struct.unpack_from("<IIII", exe, o + 8)
        secs.append((exe[o:o + 8].rstrip(b"\0").decode(), base + va, rs, rp))

    def file_off(va):
        for _, sva, rs, rp in secs:
            if sva <= va < sva + rs:
                return rp + va - sva
        return None

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lines, problems = [], []
    counts = [0] * 4
    for r in sorted(inv, key=lambda r: int(r["va"], 16)):
        va, size, name, obj = int(r["va"], 16), int(r["size"]), r["demangled"], r["object"]
        o = file_off(va)
        if o is None or not size:
            continue
        short = name.split("(")[0].split(" ")[-1]
        for ins in md.disasm(exe[o:o + size], va):
            b = ins.bytes
            fields = [(ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)]
            for k in sorted({off for off, n in fields if off and n == 4}):   # the instruction's own 32-bit operands
                v = struct.unpack_from("<I", b, k)[0]
                for t, (tname, lo, hi, tobj, caps, ends) in enumerate(TABLES):
                    if short.startswith("$E"):
                        continue
                    if v in ends and obj == tobj and f"[{v:#x}]" not in ins.op_str:
                        lines.append(f"    {{0x{ins.address:06x}, {k}, 0x{v:06x}, 3}},   // {short}: {ins.mnemonic} {ins.op_str}")
                        counts[3] += 1
                    elif v == hi and f"[{hi:#x}]" not in ins.op_str:   # (the static just after it is fine)
                        problems.append(f"{ins.address:#x} {ins.mnemonic} {ins.op_str} ({short}): the {tname} table's end")
                    elif lo <= v < hi:
                        lines.append(f"    {{0x{ins.address:06x}, {k}, 0x{v:06x}, {t}}},   // {short}: {ins.mnemonic} {ins.op_str}")
                        counts[t] += 1
            for tname, lo, hi, tobj, caps, ends in TABLES:
                if obj == tobj and ins.mnemonic == "cmp" and ins.op_str.split(", ")[-1] in caps and ins.address != FILE_CAPACITY_CMP:
                    problems.append(f"{ins.address:#x} {ins.mnemonic} {ins.op_str} ({short}): a capacity compare?")
    for sname, sva, rs, rp in secs:
        if sname == ".text":
            continue
        d = exe[rp:rp + rs]
        for k in range(0, len(d) - 3, 4):
            v = struct.unpack_from("<I", d, k)[0]
            for tname, lo, hi, *_ in TABLES:
                if lo <= v <= hi:
                    problems.append(f"{sname} {sva + k:#x}: a pointer into the {tname} table")
    if problems:
        sys.exit("\n".join(problems))
    out = ROOT / "hook" / "res_table_fields.inc"
    out.write_text("// generated by tools/gen_res_tables.py -- {address, operand offset, v1.0 value, table: 0 options / "
                   "1 languages / 2 open files / 3 the open files' loop ends}\n" + "\n".join(lines) + "\n", encoding="utf-8")
    print(f"{counts[0]} options-table fields, {counts[1]} language-table fields, {counts[2]} open-file-table fields and "
          f"{counts[3]} of its loop ends -> {out}")


if __name__ == "__main__":
    main()
