"""Identify the toolchain that built race.exe: PE header linker version, Rich header (if any),
timestamp, and the C runtime's own version strings.

    python tools/compiler_id.py <race.exe>
"""
import datetime
import re
import struct
import sys
from pathlib import Path


def main():
    b = Path(sys.argv[1]).read_bytes()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    machine, nsec, stamp = struct.unpack_from("<HHI", b, pe + 4)
    opt = pe + 24
    magic, lmaj, lmin = struct.unpack_from("<HBB", b, opt)
    osmaj, osmin, imaj, imin, smaj, smin = struct.unpack_from("<HHHHHH", b, opt + 40)
    subsystem = struct.unpack_from("<H", b, opt + 68)[0]
    print(f"machine {machine:#x}, {nsec} sections, link time {datetime.datetime.utcfromtimestamp(stamp)} UTC")
    print(f"linker {lmaj}.{lmin:02d}; OS {osmaj}.{osmin}; subsystem version {smaj}.{smin}; subsystem {subsystem} ({'GUI' if subsystem == 2 else 'console'})")
    rich = b.find(b"Rich", 0x80, pe)
    print("Rich header:", "present" if rich > 0 else "absent (predates it: the Rich header arrived with Visual C++ 6.0's linker)")
    for pat in (rb"Microsoft Visual C\+\+ Runtime Library", rb"MSVCRT[0-9]*\.dll", rb"Copyright \(c\) [^\x00]{0,60}Microsoft[^\x00]{0,40}",
                rb"NON-DEBUG MSVC[^\x00]{0,20}", rb"[A-Z][a-z]{2} [ 0-9]{2} 199[0-9] [0-9:]{8}"):
        hits = sorted({m.group(0).decode("latin-1") for m in re.finditer(pat, b[:0x1c0000])})[:4]
        if hits:
            print(f"  {pat.decode()[:34]:36s} {hits}")


if __name__ == "__main__":
    main()
