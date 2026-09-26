"""Pull the linker map out of a v1.0 race.exe and write it as a CSV symbol table.

The v1.0 retail race.exe (2,404,451 bytes) carries its MSVC linker map as printable text appended
after the image. Each symbol line reads

    0001:0003c2e0       ?Update@Obstacle@@UAEXXZ   0043d2e0 f physics:obstacle.obj

i.e. section:offset, mangled name, virtual address, an optional 'f' (function) or 'i' (inline)
flag, and the object file it came from ("library:object" for the game's own libraries).

    python tools/extract_map.py <race.exe> [out/symbols.csv]
"""
import csv
import ctypes
import re
import sys
from pathlib import Path

LINE = re.compile(rb"^\s*(000[1-9]):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})\s+(f\s+)?(i\s+)?(\S+)?\s*$", re.M)
SECTIONS = {"0001": ".text", "0002": ".rdata", "0003": ".data", "0004": ".idata"}

_undname = ctypes.windll.dbghelp.UnDecorateSymbolName
_undname.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]


def demangle(name: str) -> str:
    if not name.startswith("?"):
        return name.lstrip("_")
    buf = ctypes.create_string_buffer(1024)
    n = _undname(name.encode("latin-1"), buf, len(buf), 0)
    return buf.value.decode("latin-1") if n else name


def extract(exe: Path) -> list[dict]:
    blob = exe.read_bytes()
    rows = {}
    for m in LINE.finditer(blob):
        sec, off, name, va, f, i, obj = (g.decode("latin-1") if g else "" for g in m.groups())
        va = int(va, 16)
        key = (va, name)
        if key in rows:
            continue
        lib, _, objname = obj.rpartition(":") if ":" in obj else ("", "", obj)
        rows[key] = dict(va=f"{va:08x}", section=SECTIONS.get(sec, sec), kind="function" if f.strip() == "f" else "data",
                         name=name, demangled=demangle(name), library=lib or ("crt" if objname.lower().startswith(("libc", "msvcrt", "oldnames")) else "root"),
                         object=objname)
    return sorted(rows.values(), key=lambda r: int(r["va"], 16))


def main():
    exe = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "out" / "symbols.csv"
    rows = extract(exe)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    funcs = sum(r["kind"] == "function" for r in rows)
    print(f"{len(rows):,} symbols ({funcs:,} functions) -> {out}")


if __name__ == "__main__":
    main()
