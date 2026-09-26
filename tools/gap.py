"""Show an unplaced v1.0 function next to the gap its placed neighbours leave in the target build.
    python tools/gap.py <v1.0> <target> out/map_X.json <v1.0 va> [<max instructions>]"""
import bisect, csv, json, sys
from pathlib import Path
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
sys.path.insert(0, str(Path(__file__).resolve().parent))
import match_builds as M
ROOT = Path(__file__).resolve().parent.parent
_, sb, stva, st = M.load(sys.argv[1]); _, db, dtva, dt = M.load(sys.argv[2])
fmap = {int(k, 16): int(v, 16) for k, v in json.loads(Path(sys.argv[3]).read_text())["functions"].items()}
inv = {int(r["va"], 16): r for r in csv.DictReader(open(ROOT / "out/inventory.csv", encoding="utf-8"))}
starts = sorted(inv); va = int(sys.argv[4], 16); n = int(sys.argv[5]) if len(sys.argv) > 5 else 60
i = starts.index(va)
prev = next(s for s in reversed(starts[:i]) if s in fmap); nxt = next(s for s in starts[i + 1:] if s in fmap)
lo = fmap[prev] + int(inv[prev]["size"]) - 16; hi = fmap[nxt]
print(f"v1.0 {inv[va]['demangled']} {va:08x} size {inv[va]['size']}; prev {inv[prev]['name']} -> {fmap[prev]:08x}, next {inv[nxt]['name']} -> {fmap[nxt]:08x}")
md = Cs(CS_ARCH_X86, CS_MODE_32)
a = list(md.disasm(st[va - stva: va - stva + int(inv[va]["size"])], va))[:n]
b = list(md.disasm(dt[lo - dtva: hi - dtva], lo))[:n + 40]
for k in range(max(len(a), len(b))):
    x = f"{a[k].address:08x} {a[k].mnemonic} {a[k].op_str}" if k < len(a) else ""
    y = f"{b[k].address:08x} {b[k].mnemonic} {b[k].op_str}" if k < len(b) else ""
    print(f"{x:55s} | {y}")
