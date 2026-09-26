"""Compare two runs of the same race, tick by tick: their traces, and (if written) their object dumps.

    python tools/trace_diff.py <replays>/<a>.trace <replays>/<b>.trace

A trace (hook/replay.cpp) holds, for every physics tick, a hash of every physics object's state; a dump
(viperport.ini [replay] dump_ticks=...) holds whole objects at chosen ticks. This prints the first tick
where the two runs part and which objects differ, then -- from the .dump files beside the traces, if both
have that tick -- every field that differs, named from the recovered types (out/types.json).
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_port_tables import flat_fields  # noqa: E402

N_OBJ = 32
TICK = struct.Struct("<II Q" + "Q" * N_OBJ)
ROOT = Path(__file__).resolve().parent.parent


def read_trace(p: Path) -> list[tuple]:
    b = p.read_bytes()
    if b[:4] != b"VPTR":
        raise SystemExit(f"{p} isn't a trace")
    body = b[16:]
    return [TICK.unpack_from(body, i * TICK.size) for i in range(len(body) // TICK.size)]


def read_dump(p: Path) -> dict[int, list[tuple[str, bytes]]]:
    out: dict[int, list[tuple[str, bytes]]] = {}
    if not p.is_file():
        return out
    b, at = p.read_bytes(), 0
    while at + 8 <= len(b):
        tick, n = struct.unpack_from("<Ii", b, at)
        at += 8
        objs = []
        for _ in range(n):
            name = b[at:at + 32].split(b"\0")[0].decode("latin-1")
            size = struct.unpack_from("<I", b, at + 32)[0]
            at += 36
            objs.append((name, b[at:at + size]))
            at += size
        out[tick] = objs
    return out


def pointer_like(v: int) -> bool:
    return v & 3 == 0 and 0x10000 <= v < 0x30000000


def field_diffs(types, cls: str, a: bytes, b: bytes, limit: int = 40) -> list[str]:
    fields = sorted(set(flat_fields(types, cls))) if cls in types else []
    lines = []
    for off in range(0, min(len(a), len(b)) - 3, 4):
        va, vb = struct.unpack_from("<I", a, off)[0], struct.unpack_from("<I", b, off)[0]
        if va == vb or (pointer_like(va) and pointer_like(vb)):
            continue
        name = f"+0x{off:x}"
        for fo, fs, fn in fields:
            if fo <= off < fo + max(fs, 4):
                name = fn if fo == off else f"{fn}+{off - fo}"
        fa, fb = struct.unpack("<f", a[off:off + 4])[0], struct.unpack("<f", b[off:off + 4])[0]
        lines.append(f"      {name:40s} {va:08x} ({fa:.9g})  vs  {vb:08x} ({fb:.9g})")
        if len(lines) >= limit:
            lines.append("      ...")
            break
    return lines


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    pa, pb = Path(sys.argv[1]), Path(sys.argv[2])
    ta, tb = read_trace(pa), read_trace(pb)
    n = min(len(ta), len(tb))
    print(f"{pa.name}: {len(ta)} ticks; {pb.name}: {len(tb)} ticks; comparing {n}")
    first = next((i for i in range(n) if ta[i][2] != tb[i][2] or ta[i][1] != tb[i][1]), None)
    if first is None:
        print(f"IDENTICAL over all {n} ticks ({n * 0.016:.1f} s of racing)")
        return 0
    differing = sum(1 for i in range(n) if ta[i][2] != tb[i][2])
    a, b = ta[first], tb[first]
    print(f"first differs at tick {first} ({first * 0.016:.3f} s); {differing} of {n} ticks differ")
    if a[1] != b[1]:
        print(f"  object count: {a[1]} vs {b[1]}")
    objs = [i for i in range(min(N_OBJ, a[1], b[1])) if a[3 + i] != b[3 + i]]
    print(f"  objects that differ (of the first {N_OBJ}): {objs or 'none -- one past the first 32'}")
    da, db = read_dump(pa.with_suffix(".dump")), read_dump(pb.with_suffix(".dump"))
    common = sorted(set(da) & set(db))
    if not common:
        print(f"  no dumps to compare: rerun both with viperport.ini [replay] dump_ticks={max(first - 1, 0)},{first}")
        return 1
    types = json.loads((ROOT / "out" / "types.json").read_text(encoding="utf-8"))
    for t in common:
        print(f"  tick {t}:")
        for i, ((ca, ba), (cb, bb)) in enumerate(zip(da[t], db[t])):
            d = field_diffs(types, ca, ba, bb)
            if d:
                print(f"    object {i} ({ca}):")
                print("\n".join(d))
    return 1


if __name__ == "__main__":
    sys.exit(main())
