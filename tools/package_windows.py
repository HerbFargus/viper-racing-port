"""package_windows.py -- the Windows engine on its own, for players who don't use vrmod (the counterpart of
tools/package_linux.sh):

    python tools/package_windows.py

packs the current MSVC build (hook/build.bat -> hook/build/dinput.dll + SDL2.dll, loader/build_loader.bat ->
loader/build/viperport.exe) into
    out/release/viperport-windows-<version>.zip  (+ .sha256)
      viperport.exe, dinput.dll, SDL2.dll, viperport.ini, README-windows.txt, LICENSES/SDL2-LICENSE.txt
<version> = the commit's date and short hash (e.g. 2026.10.07-50cd1db), "-dirty" when hook/, loader/ or tools/ have
uncommitted changes. Build first; the binaries must be newer than the newest source under hook/ and loader/.
"""
import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDL_LICENSE = ROOT.parent / "sdl2" / "SDL2-2.32.10" / "LICENSE.txt"
MARK = b"viperport"


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout.strip()


def crlf(text: str) -> bytes:
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def main():
    files = {"viperport.exe": ROOT / "loader" / "build" / "viperport.exe",
             "dinput.dll": ROOT / "hook" / "build" / "dinput.dll",
             "SDL2.dll": ROOT / "hook" / "build" / "SDL2.dll"}
    for name, p in files.items():
        if not p.is_file():
            sys.exit(f"package_windows: no {p} -- build first (hook\\build.bat, loader\\build_loader.bat)")
    for name in ("viperport.exe", "dinput.dll"):
        if MARK not in files[name].read_bytes():
            sys.exit(f"package_windows: {files[name]} doesn't carry the viperport mark")
    # the binaries must be built from the sources as they are
    sources = {"dinput.dll": [p for p in (ROOT / "hook").glob("*.*") if p.suffix in (".cpp", ".h", ".inc", ".def", ".bat")],
               "viperport.exe": [ROOT / "loader" / "viperport_main.cpp", ROOT / "loader" / "build_loader.bat"]}
    for name, srcs in sources.items():
        newest = max(srcs, key=lambda p: p.stat().st_mtime)
        if files[name].stat().st_mtime < newest.stat().st_mtime:
            sys.exit(f"package_windows: {name} is older than {newest.name} -- rebuild first")
    if not SDL_LICENSE.is_file():
        sys.exit(f"package_windows: no {SDL_LICENSE}")

    version = git("log", "-1", "--date=format:%Y.%m.%d", "--format=%cd-%h")
    if git("status", "--porcelain", "--", "hook", "loader", "tools"):
        version += "-dirty"
    name = f"viperport-windows-{version}"
    out = ROOT / "out" / "release"
    out.mkdir(parents=True, exist_ok=True)
    zpath = out / f"{name}.zip"
    readme = (ROOT / "tools" / "win_pkg" / "README-windows.txt").read_text(encoding="utf-8").replace("@VERSION@", version)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, p in files.items():
            z.write(p, arc)
        z.writestr("viperport.ini", crlf((ROOT / "tools" / "win_pkg" / "viperport.ini").read_text(encoding="utf-8")))
        z.writestr("README-windows.txt", crlf(readme))
        z.write(SDL_LICENSE, "LICENSES/SDL2-LICENSE.txt")
    sha = hashlib.sha256(zpath.read_bytes()).hexdigest()
    (out / f"{name}.zip.sha256").write_text(f"{sha}  {name}.zip\n", encoding="utf-8")
    print(f"package_windows: {zpath.relative_to(ROOT)} ({zpath.stat().st_size // 1024} KB)")
    for arc, p in files.items():
        print(f"  {arc:14} sha256 {hashlib.sha256(p.read_bytes()).hexdigest()[:12]}")


if __name__ == "__main__":
    main()
