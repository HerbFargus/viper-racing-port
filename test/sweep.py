"""sweep.py -- build every test/world_*.cpp harness with MSVC and with GCC, run both, and say whether they agree
(relink stage R1: the GCC build beside the MSVC one).

    python test\\sweep.py [--only name ...] [--skip name ...] [--msvc-only | --gcc-only] [--no-build] [--no-run]
                          [--timeout s] [--jobs n] [--out dir] [--gcc-flags "extra g++ flags"]

Each harness is built the way its header comment says (the `cl /nologo ...` line and its continuation lines: the /D
defines, /I folders, the extra hook\\*.cpp sources, the /link options), by MSVC as written (only /Fo and /Fe moved to
the output folder) and by GCC (mingw-w64 i686, C:\\msys64\\mingw32) with the R1 flags. The harness source is passed by
its absolute Windows path (the harnesses find the repository through __FILE__). Each exe then runs with its defaults
from the repository root, with a time limit; a run that outlives it is killed (only the processes this script
started, by PID, with their children). The result of a run is its exit code and its output; "same?" compares the
two builds' runs:
    yes    the same output line for line (timings aside);
    agree  the same exit code and the same failures (none, or the same nonzero "N differ / mismatches / failed / bad /
           footprint violations ..." and the same MISMATCH / FAIL / CRASH lines), other counts having moved -- some
           harnesses put random pointers in their worlds, and which of them fault depends on each build's memory layout;
    NO     anything else (look at the two .out files).
--no-build --no-run tabulates the last runs again.

Output: <out>\\msvc\\<name>.{exe,build.log,out,code}, <out>\\gcc\\<name>.{exe,build.log,build.cmd,out,code}, and the
table in <out>\\sweep.txt (default <out>: %TEMP%\\vp_sweep). A harness whose build fails names the files the errors
are in (a hook file still being converted shows up there).
"""
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST = os.path.join(REPO, "test")
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
MINGW = r"C:\msys64\mingw32\bin"
GPP = os.path.join(MINGW, "g++.exe")
GCC_FLAGS = ["-m32", "-O2", "-march=i686", "-mfpmath=387", "-fexcess-precision=standard", "-fno-strict-aliasing", "-masm=intel", "-std=c++17",
             "-fms-extensions"]
EXTRA_GCC_FLAGS = []                                             # --gcc-flags "..." (an experiment's extra flags)
GCC_LIBS = ["-luser32", "-lgdi32", "-lwinmm", "-lws2_32", "-ladvapi32", "-lshell32", "-lole32", "-ldxguid"]
SUMMARY_RE = re.compile(r"mismatch|differ|HUNG|checks|identical|FAIL|fault|escape|passed|PASS|OK\b|crash", re.I)

_print_lock = threading.Lock()


def say(*a):
    with _print_lock:
        print(*a, flush=True)


def header_command(path):
    """The tokens of the harness's build command: its comment's `cl /nologo` line and the continuation lines after it."""
    toks, on = [], False
    for ln in open(path, encoding="utf-8", errors="replace"):
        if not ln.startswith("//"):
            break
        body = ln[2:].strip()
        if not on:
            m = re.search(r"\bcl (/nologo.*)", body)
            if m:
                on = True
                toks += m.group(1).split()
            continue
        if ln.startswith("//        ") and not re.match(r"(run|build)\b", body) and not body.startswith("("):
            toks += body.split()
        else:
            break
    return toks


def parse(toks):
    """Split the cl tokens: compiler flags, defines, include folders, sources, /link options, libraries."""
    r = {"cflags": [], "defs": [], "incs": [], "srcs": [], "link": [], "libs": []}
    i, link = 0, False
    while i < len(toks):
        t = toks[i]
        if t == "/link":
            link = True
        elif t.startswith("/Fo") or t.startswith("/Fe"):
            if t in ("/Fe:", "/Fo:"):
                i += 1
        elif link:
            if t.lower().endswith(".lib"):
                r["libs"].append(t)
            else:
                r["link"].append(t)
        elif t.startswith("/D"):
            r["defs"].append(t[2:])
        elif t.startswith("/I"):
            r["incs"].append(t[2:])
        elif t.lower().endswith(".cpp"):
            r["srcs"].append(t)
        elif t.lower().endswith(".lib"):
            r["libs"].append(t)
        else:
            r["cflags"].append(t)
        i += 1
    return r


def absrepo(p):
    return os.path.normpath(os.path.join(REPO, p.replace("/", "\\")))


def build_msvc(name, cmd, out):
    exe = os.path.join(out, name + ".exe")
    objdir = os.path.join(out, name + ".obj")
    os.makedirs(objdir, exist_ok=True)
    srcs = " ".join('"%s"' % absrepo(s) for s in cmd["srcs"])
    line = "cl %s %s %s %s /Fo\"%s\\\\\" /Fe\"%s\"" % (
        " ".join(cmd["cflags"]), " ".join("/D" + d for d in cmd["defs"]),
        " ".join('/I"%s"' % absrepo(d) for d in cmd["incs"]), srcs, objdir, exe)
    if cmd["link"] or cmd["libs"]:
        line += " /link %s %s" % (" ".join(cmd["link"]), " ".join('"%s"' % absrepo(l) if "\\" in l else l for l in cmd["libs"]))
    bat = os.path.join(out, name + ".build.bat")
    with open(bat, "w") as f:
        f.write('@echo off\r\ncall "%s" x86 >nul\r\ncd /d "%s"\r\n%s\r\n' % (VCVARS, REPO, line))
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run(["cmd.exe", "/c", bat], capture_output=True, text=True, errors="replace")
    return exe, b.returncode == 0 and os.path.exists(exe), b.stdout + b.stderr


def build_gcc(name, cmd, out):
    exe = os.path.join(out, name + ".exe")
    args = [GPP] + GCC_FLAGS + EXTRA_GCC_FLAGS + ["-D" + d for d in cmd["defs"]] + ["-I" + absrepo(d) for d in cmd["incs"]]
    args += [absrepo(s) for s in cmd["srcs"]]                 # test\world_x.cpp by its absolute Windows path
    link = [l.upper() for l in cmd["link"]]
    if any(l.startswith("/BASE:") for l in link):
        base = [l for l in link if l.startswith("/BASE:")][0][6:]
        args += ["-Wl,--image-base," + base, "-Wl,--disable-dynamicbase", "-Wl,--disable-nxcompat"]
        stack = [l for l in link if l.startswith("/STACK:")]
        # MSVC's default stack (1 MB) unless the header sets one: GNU ld's bigger reservation lands on 0x400000, where
        # the harness loads race_v10.exe
        args += ["-Wl,--stack," + (stack[0][7:].split(",")[0] if stack else "0x100000")]
    args += ["-static", "-o", exe]
    for lib in cmd["libs"]:
        low = lib.lower()
        if low == "delayimp.lib":
            continue                                             # GNU ld has no delay-load: linked normally
        if "\\" in lib or "/" in lib:
            args.append(absrepo(lib))                            # (ld reads an MSVC import library as it is)
            dll = absrepo(lib)[:-4] + ".dll"
            if os.path.exists(dll):
                shutil.copy2(dll, out)                           # beside the exe: a normal import needs it to start
        else:
            args.append("-l" + lib[:-4])
    args += GCC_LIBS
    env = dict(os.environ)
    env["PATH"] = MINGW + ";" + env.get("PATH", "")
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run(args, capture_output=True, text=True, errors="replace", env=env, cwd=REPO)
    with open(os.path.join(out, name + ".build.cmd"), "w") as f:
        f.write(subprocess.list2cmdline(args) + "\n")
    return exe, b.returncode == 0 and os.path.exists(exe), b.stdout + b.stderr


def error_files(log):
    """Where a failed build's errors are: {file: count}, the harness's own file and the hook files it includes."""
    files = {}
    for l in log.splitlines():
        m = re.match(r"(.*?)(?::\d+:\d+: |\(\d+\)\s*: )(?:fatal )?error", l)
        if m:
            k = os.path.basename(m.group(1))
            files[k] = files.get(k, 0) + 1
        elif "undefined reference" in l or "unresolved external" in l:
            files["(link)"] = files.get("(link)", 0) + 1
    return files


def run(exe, timeout):
    """Run exe from the repository root: (exit code or None, output, seconds). Kills its own process tree at the limit."""
    t0 = time.time()
    p = subprocess.Popen([exe], cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    try:
        data, _ = p.communicate(timeout=timeout)
        code = p.returncode
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True)
        data, _ = p.communicate()
        code = None
    return code, data.decode("utf-8", errors="replace"), time.time() - t0


VERDICT_RE = re.compile(r"\b(differ\w*|mismatch\w*|identical|failed|bad|violations?|misses|escape\w*)\b", re.I)
BAD_NUM_RE = re.compile(r"(\d+)\s+(?:of \d+ [\w ]*?)?(differ\w*|mismatch\w*|failed|bad|footprint violations?|footprint misses|"
                        r"protocol failures|unexpected)\b", re.I)
BAD_LINE_RE = re.compile(r"^\s*(MISMATCH|FAIL|CRASH|DIFF|HUNG|ESCAPE|LEAK|BUG)\b")


def summary(text):
    """The run's summary: (the lines shown, the full output normalised, the failure signature). The lines shown are
    its last verdict lines (differ / mismatch / identical / failed ...); the signature is what the run counted as
    failures (every 'N differ / mismatches / failed / bad / footprint violations ...' that isn't 0, and the lines
    starting MISMATCH / FAIL / CRASH / DIFF / HUNG ...), which two builds must share even where a harness's other
    counts move with the memory layout (random pointers that fault in one build's address space and not the other's)."""
    lines = [l.rstrip() for l in text.splitlines() if l.strip()]
    keep = [l.strip() for l in lines if VERDICT_RE.search(l) and len(l) < 600 and not re.match(r"\s", l)][-2:]
    if not keep:
        keep = [l.strip() for l in lines if SUMMARY_RE.search(l) and len(l) < 600][-2:] or [l.strip() for l in lines[-2:]]
    norm = [re.sub(r"\b\d+(\.\d+)?\s*(s|ms|sec|seconds)\b", "<t>", l) for l in lines]
    sig = []
    for l in lines:
        for m in BAD_NUM_RE.finditer(l):
            if int(m.group(1)):
                sig.append("%s %s" % (m.group(1), m.group(2).lower()))
        m = BAD_LINE_RE.match(l)
        if m:
            sig.append(m.group(1))
    return keep, norm, sorted(sig)


def code_str(code):
    if code is None:
        return "TIMEOUT"
    return "exit %d" % code if code < 0x10000 else "exit %08x" % (code & 0xffffffff)


def do_one(name, comp, args, out_root):
    out = os.path.join(out_root, comp)
    os.makedirs(out, exist_ok=True)
    src = os.path.join(TEST, name + ".cpp")
    cmd = parse(header_command(src))
    if not any("world_" in s for s in cmd["srcs"]):
        cmd["srcs"].insert(0, "test\\%s.cpp" % name)
    exe = os.path.join(out, name + ".exe")
    if not args["no_build"]:
        t0 = time.time()
        exe, ok, log = (build_msvc if comp == "msvc" else build_gcc)(name, cmd, out)
        with open(os.path.join(out, name + ".build.log"), "w", encoding="utf-8") as f:
            f.write(log)
        failf = os.path.join(out, name + ".build.fail")
        if not ok:
            ef = error_files(log)
            detail = " ".join("%s:%d" % kv for kv in sorted(ef.items())) or log.strip()[-200:]
            with open(failf, "w", encoding="utf-8") as f:
                f.write(detail)
            for stale in (".out", ".code"):
                if os.path.exists(os.path.join(out, name + stale)):
                    os.remove(os.path.join(out, name + stale))
            return {"result": "BUILD FAILED", "detail": detail, "norm": None, "built": False}
        if os.path.exists(failf):
            os.remove(failf)
        say("  built %-24s %-5s %5.0fs" % (name, comp, time.time() - t0))
    elif not os.path.exists(exe):
        failf = os.path.join(out, name + ".build.fail")
        if os.path.exists(failf):
            return {"result": "BUILD FAILED", "detail": open(failf, encoding="utf-8").read(), "norm": None, "built": False}
        return {"result": "NOT BUILT", "detail": "", "norm": None, "built": False}
    outf, codef = os.path.join(out, name + ".out"), os.path.join(out, name + ".code")
    if args["no_run"]:
        if not args["no_build"] or not (os.path.exists(outf) and os.path.exists(codef)):
            return {"result": "built", "detail": "", "norm": None, "built": True}
        text = open(outf, encoding="utf-8").read()                # --no-build --no-run: the last run's result again
        c = open(codef).read().strip()
        code, secs = (None if c == "None" else int(c)), 0
    else:
        code, text, secs = run(exe, args["timeout"])
        with open(outf, "w", encoding="utf-8") as f:
            f.write(text)
        with open(codef, "w") as f:
            f.write("%s\n" % code)
    keep, norm, sig = summary(text)
    return {"result": code_str(code), "detail": " || ".join(keep), "norm": (code, norm), "sig": (code, sig), "built": True,
            "secs": secs}


def main():
    a = sys.argv[1:]
    known = {"--only", "--skip", "--msvc-only", "--gcc-only", "--no-build", "--no-run", "--timeout", "--jobs", "--out",
             "--gcc-flags"}
    bad = [x for x in a if x.startswith("-") and x not in known and not x.startswith("-f") and not x.startswith("-W")]
    if bad or "-h" in a or "--help" in a:
        print(__doc__)
        sys.exit(0 if not bad or bad == ["--help"] or bad == ["-h"] else 2)

    def opt(n, d):
        return a[a.index(n) + 1] if n in a else d

    def multi(n):
        if n not in a:
            return []
        i = a.index(n) + 1
        r = []
        while i < len(a) and not a[i].startswith("--"):
            r.append(a[i])
            i += 1
        return r

    args = {"timeout": int(opt("--timeout", "900")), "no_build": "--no-build" in a, "no_run": "--no-run" in a}
    out_root = opt("--out", os.path.join(os.environ.get("TEMP", REPO), "vp_sweep"))
    jobs = int(opt("--jobs", "4"))
    EXTRA_GCC_FLAGS.extend(opt("--gcc-flags", "").split())
    comps = ["msvc"] if "--msvc-only" in a else ["gcc"] if "--gcc-only" in a else ["msvc", "gcc"]
    names = sorted(f[:-4] for f in os.listdir(TEST) if f.startswith("world_") and f.endswith(".cpp"))
    only, skip = multi("--only"), multi("--skip")
    if only:
        names = [n for n in names if n in only or n.replace("world_", "") in only]
    names = [n for n in names if n not in skip and n.replace("world_", "") not in skip]
    os.makedirs(out_root, exist_ok=True)
    say("sweep: %d harnesses, %s, timeout %ds, %d jobs, output in %s" % (len(names), "+".join(comps), args["timeout"], jobs,
                                                                         out_root))
    res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(do_one, n, c, args, out_root): (n, c) for n in names for c in comps}
        for f in concurrent.futures.as_completed(futs):
            n, c = futs[f]
            try:
                res[(n, c)] = f.result()
            except Exception as e:                              # a sweep keeps going
                res[(n, c)] = {"result": "ERROR", "detail": repr(e), "norm": None, "built": False}
            r = res[(n, c)]
            say("  %-24s %-5s %-12s %s" % (n, c, r["result"], r["detail"][:200]))

    rows = []
    w = max(len(n) for n in names) if names else 10
    head = "%-*s | %-14s | %-14s | %s" % (w, "harness", "MSVC", "GCC", "same?")
    rows.append(head)
    rows.append("-" * len(head))
    counts = {}
    for n in names:
        rm, rg = res.get((n, "msvc")), res.get((n, "gcc"))
        if rm and rg:
            if rm["norm"] is not None and rg["norm"] is not None:
                if rm["norm"] == rg["norm"]:
                    same = "yes"                                 # the same output, line for line
                elif rm["sig"] == rg["sig"] and rm["sig"][0] is not None:
                    same = "agree"                               # the same verdict and failures; other counts moved
                else:
                    same = "NO"
            elif not rg["built"]:
                same = "gcc build failed"
            elif not rm["built"]:
                same = "msvc build failed"
            else:
                same = "-"
        else:
            same = "-"
        counts[same] = counts.get(same, 0) + 1
        rows.append("%-*s | %-14s | %-14s | %s" % (w, n, rm["result"] if rm else "-", rg["result"] if rg else "-", same))
    rows.append("")
    rows.append("totals: " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    rows.append("(yes: the same output line for line, timings aside; agree: the same exit code and the same failures "
                "-- none, or the same nonzero 'differ / mismatch / failed / bad ...' counts and MISMATCH / FAIL / CRASH "
                "lines -- while other counts moved with the memory layout)")
    rows.append("")
    rows.append("details (summary lines; build failures: the files the errors are in):")
    for n in names:
        for c in comps:
            r = res.get((n, c))
            if r:
                rows.append("  %-*s %-5s %s" % (w, n, c, r["detail"][:600]))
    table = "\n".join(rows)
    say("\n" + table)
    with open(os.path.join(out_root, "sweep.txt"), "w", encoding="utf-8") as f:
        f.write(table + "\n")


if __name__ == "__main__":
    main()
