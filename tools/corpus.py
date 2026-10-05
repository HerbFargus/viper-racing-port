"""The regression corpus: replay a fixed set of recorded sessions on a build, unattended, and report each one.

    python tools/corpus.py                         every session in tools/corpus.json, on each of its routes
    python tools/corpus.py --only lan-ai-1 career  just these entries (by name)
    python tools/corpus.py --routes standalone     just this route (dll, original, standalone; comma-separated)
    python tools/corpus.py --list                  what the corpus holds, and whether each session is in the install
    python tools/corpus.py --dry-run               what would run, without starting the game
    python tools/corpus.py --parse <viperport.log> read a replay's log as the runner does (no game)
    python tools/corpus.py --selftest              the log parser, on synthetic lines

A route is how the session is replayed (hook/session.cpp, "[session] play="):
    dll         the entry's exe (race.exe beside the port's dinput.dll) with [port] default=new -- the rewrites
    original    the same exe with [port] default=original -- MGI's code (only for sessions without an Options visit:
                the Hacks list's scroll-bar fix differs by design)
    standalone  viperport.exe --race <exe> -- the port's code alone (forces default=new; traps any original code)

Each run: viperport.ini is set ([session] play/label, record off; [port] default; [replay] record=0; [test]
two_copies=0), the game is started in the install folder and waited for. A recording that ends with the game quitting
quits the replay too; one that doesn't hands the player control ("the player has control from here"): the runner then
closes the window it started (WM_CLOSE -- the game writes its report on the way out) and, if that doesn't end it, ends
the process. Only processes the runner started are ever closed. viperport.ini is restored afterwards, also on Ctrl+C
(a copy waits in viperport.ini.corpus-backup until then).

PASS = the session IDENTICAL over every frame compared, the recording fed to its end, 0 reads fell back, every race
IDENTICAL, every network send matching, the window at the recording's size, and (standalone) no original code reached.
Each run's viperport.log is kept as sessions\\<session>\\<run>.log; the results go to out/corpus/<time>.json.

The corpus lists session NAMES only; the sessions (game data) stay in the install, never in the repo.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MANIFEST = os.path.join(HERE, "corpus.json")
DEFAULT_INSTALL = os.path.normpath(os.path.join(REPO, "..", "game-files", "installs", "v1.0-RC"))
ROUTES = ("dll", "original", "standalone")
GAME_EXES = {"race.exe", "race_stock.exe", "viperport.exe", "race.bin"}


# ---- viperport.ini ---------------------------------------------------------------------------------------------------
def ini_set(text, section, key, value):
    """Set key=value in [section], keeping every other line (and comment) as it is; add what's missing."""
    lines = text.splitlines()
    sec_re = re.compile(r"^\s*\[([^\]]+)\]")
    start = None
    for i, ln in enumerate(lines):
        m = sec_re.match(ln)
        if m and m.group(1).strip().lower() == section.lower():
            start = i
            break
    if start is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines += ["[%s]" % section, "%s=%s" % (key, value)]
        return "\n".join(lines) + "\n"
    end = len(lines)
    for j in range(start + 1, len(lines)):
        if sec_re.match(lines[j]):
            end = j
            break
    key_re = re.compile(r"^\s*%s\s*=" % re.escape(key), re.I)
    for j in range(start + 1, end):
        if key_re.match(lines[j]):
            comment = ""
            m = re.search(r"\s+;.*$", lines[j])
            if m:
                comment = m.group(0)
            lines[j] = "%s=%s%s" % (key, value, comment)
            return "\n".join(lines) + "\n"
    ins = end
    while ins > start + 1 and not lines[ins - 1].strip():
        ins -= 1
    lines.insert(ins, "%s=%s" % (key, value))
    return "\n".join(lines) + "\n"


def ini_for_run(text, play, label, default, extra=None):
    for sec, keys in (extra or {}).items():
        for key, val in keys.items():
            text = ini_set(text, sec, key, str(val))
    if "two_copies" not in (extra or {}).get("test", {}):
        text = ini_set(text, "test", "two_copies", "0")
    for sec, key, val in (("session", "record", "0"), ("session", "play", play), ("session", "label", label),
                          ("port", "default", default), ("replay", "record", "0")):
        text = ini_set(text, sec, key, val)
    return text


# ---- the log ---------------------------------------------------------------------------------------------------------
R_REPLAYING = re.compile(r"session: replaying (.+?) \(\d+ input records, (\d+) frames\) as (\S+) ->")
R_NOT = re.compile(r"session: NOT (?:replaying|recording) -- (.*)")
R_IDENT = re.compile(r"exit: session: (\S+): IDENTICAL over all (\d+) frames compared(.*?) \((\d+) of them in races; "
                     r"(\d+) in races out of lockstep not compared\)(.*)")
R_PARTED = re.compile(r"exit: session: (\S+): parted at frame (\d+) \((.*)\); (\d+) of (\d+) frames compared differ")
R_FED = re.compile(r"exit: session: fed (\d+) Win32Idle calls \((\d+) input events\); (\d+) reads fell back on the last "
                   r"value or the live one, (\d+) recorded reads were never taken(.*)")
R_RACE_OK = re.compile(r"replay: (.*?): (\d+) ticks, IDENTICAL to the recording over all (\d+) ticks compared(.*)")
R_RACE_BAD = re.compile(r"replay: (.*?): (\d+) ticks; first diverged at tick (\d+), (\d+) ticks differ")
R_SENDS = re.compile(r"exit: session: network: (\d+) sends compared, (\d+) differ from the recording's")
R_WINDOW = re.compile(r"session: the window is (\d+)x(\d+), the recording's was (\d+)x(\d+)")
R_ORIGINAL = re.compile(r"standalone: ORIGINAL CODE REACHED at (.*)")
R_ENDED = re.compile(r"session: (.*) -- the player has control from here")
R_LOCKSTEP = re.compile(r"exit: session: (\d+) races, (\d+) of them in lockstep \((\d+) dropped it\)")


def parse_log(lines):
    """What a replay's viperport.log says. Returns a dict; 'verdict' is PASS / FAIL / ERROR, 'why' the reasons."""
    r = {"run": None, "frames_recorded": None, "identical": None, "frames_compared": None, "race_frames": None,
         "parted_at": None, "parted_what": None, "differ": None, "fed_all": None, "fallbacks": None,
         "never_taken": None, "redirected": None, "races": [], "sends": None, "sends_differ": None,
         "window": None, "original_code": [], "player_control": None, "lockstep_dropped": None, "not_replaying": None}
    for ln in lines:
        if (m := R_REPLAYING.search(ln)):
            r["frames_recorded"], r["run"] = int(m.group(2)), m.group(3)
        elif (m := R_NOT.search(ln)):
            r["not_replaying"] = m.group(1).strip()
        elif (m := R_IDENT.search(ln)):
            r["identical"] = True
            r["frames_compared"], r["race_frames"] = int(m.group(2)), int(m.group(4))
            r["fed_all"] = "closed before the recording ended" not in ln
        elif (m := R_PARTED.search(ln)):
            r["identical"] = False
            r["parted_at"], r["parted_what"] = int(m.group(2)), m.group(3)
            r["differ"], r["frames_compared"] = int(m.group(4)), int(m.group(5))
        elif (m := R_FED.search(ln)):
            r["fallbacks"], r["never_taken"] = int(m.group(3)), int(m.group(4))
            r["redirected"] = "NOT redirected" not in m.group(5)
        elif (m := R_RACE_OK.search(ln)):
            r["races"].append({"why": m.group(1), "ticks": int(m.group(2)), "identical": True,
                               "more": "the recording has more" in m.group(4)})
        elif (m := R_RACE_BAD.search(ln)):
            r["races"].append({"why": m.group(1), "ticks": int(m.group(2)), "identical": False,
                               "diverged_at": int(m.group(3)), "ticks_differ": int(m.group(4))})
        elif (m := R_SENDS.search(ln)):
            r["sends"], r["sends_differ"] = int(m.group(1)), int(m.group(2))
        elif (m := R_WINDOW.search(ln)):
            r["window"] = "%sx%s, recorded at %sx%s" % m.groups()
        elif (m := R_ORIGINAL.search(ln)):
            r["original_code"].append(m.group(1).strip())
        elif (m := R_ENDED.search(ln)):
            r["player_control"] = m.group(1)
        elif (m := R_LOCKSTEP.search(ln)):
            r["lockstep_dropped"] = int(m.group(3))
    why = []
    if r["not_replaying"]:
        why.append("not replayed: " + r["not_replaying"])
    elif r["identical"] is None:
        why.append("no session verdict in the log (the game didn't exit normally?)")
    elif not r["identical"]:
        why.append("parted at frame %d (%s); %d of %d frames differ" % (r["parted_at"], r["parted_what"], r["differ"],
                                                                         r["frames_compared"]))
    elif not r["fed_all"]:
        why.append("the game closed before the recording ended")
    if r["fallbacks"]:
        why.append("%d reads fell back" % r["fallbacks"])
    if r["redirected"] is False:
        why.append("the user directory was NOT redirected")
    for k, race in enumerate(r["races"], 1):
        if not race["identical"]:
            why.append("race %d diverged at tick %d (%d ticks differ)" % (k, race["diverged_at"], race["ticks_differ"]))
        elif race["more"]:
            why.append("race %d: the recording has more ticks" % k)
    if r["sends_differ"]:
        why.append("%d of %d network sends differ" % (r["sends_differ"], r["sends"]))
    if r["lockstep_dropped"]:
        why.append("%d races dropped lockstep (their frames weren't compared)" % r["lockstep_dropped"])
    if r["window"]:
        why.append("window " + r["window"])
    if r["original_code"]:
        why.append("ORIGINAL CODE REACHED (%d): %s" % (len(r["original_code"]), r["original_code"][0]))
    r["verdict"] = "ERROR" if r["not_replaying"] or r["identical"] is None else ("PASS" if not why else "FAIL")
    r["why"] = why
    return r


# ---- processes and windows -------------------------------------------------------------------------------------------
def running_game_exes():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    names = set()
    for ln in out.splitlines():
        if ln.startswith('"'):
            names.add(ln.split('","')[0].strip('"').lower())
    return sorted(names & GAME_EXES)


def tree_of(root):
    """root and its descendants: viperport.exe starts itself again when 0x400000 is taken, and the child is the game."""
    class PE(ctypes.Structure):
        _fields_ = [("dwSize", ctypes.c_ulong), ("cntUsage", ctypes.c_ulong), ("th32ProcessID", ctypes.c_ulong),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", ctypes.c_ulong),
                    ("cntThreads", ctypes.c_ulong), ("th32ParentProcessID", ctypes.c_ulong),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", ctypes.c_ulong), ("szExeFile", ctypes.c_char * 260)]
    k32 = ctypes.windll.kernel32
    k32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    snap = k32.CreateToolhelp32Snapshot(2, 0)                         # TH32CS_SNAPPROCESS
    kids = {}
    e = PE()
    e.dwSize = ctypes.sizeof(PE)
    ok = k32.Process32First(ctypes.c_void_p(snap), ctypes.byref(e))
    while ok:
        kids.setdefault(e.th32ParentProcessID, []).append(e.th32ProcessID)
        ok = k32.Process32Next(ctypes.c_void_p(snap), ctypes.byref(e))
    k32.CloseHandle(ctypes.c_void_p(snap))
    tree, todo = [], [root]
    while todo:
        p = todo.pop()
        if p not in tree:
            tree.append(p)
            todo += kids.get(p, [])
    return tree


def close_windows_of(pids):
    """Post WM_CLOSE to the top-level windows of processes the runner started. Returns how many it closed."""
    pids = set(pids)
    user32 = ctypes.windll.user32
    found = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(hwnd, _):
        p = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(p))
        if p.value in pids and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            found.append(hwnd)
        return True

    user32.EnumWindows(proto(each), 0)
    for h in found:
        user32.PostMessageW(ctypes.c_void_p(h), 0x0010, 0, 0)        # WM_CLOSE
    return len(found)


def sha12(path):
    try:
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()[:12]
    except OSError:
        return None


# ---- one replay ------------------------------------------------------------------------------------------------------
def run_one(install, entry, route, label, timeout, grace, log_tail):
    session = entry["session"].replace("/", "\\")
    exe = entry.get("exe", "race.exe")
    if route == "standalone":
        cmd = [os.path.join(install, "viperport.exe"), "--race", exe]
    else:
        cmd = [os.path.join(install, exe)]
    ini_path = os.path.join(install, "viperport.ini")
    with open(ini_path, encoding="latin-1") as f:
        base = f.read()
    with open(ini_path, "w", encoding="latin-1", newline="\r\n") as f:
        f.write(ini_for_run(base, session, label, "original" if route == "original" else "new", entry.get("ini")))
    log_path = os.path.join(install, "viperport.log")
    started = time.time()
    proc = subprocess.Popen(cmd, cwd=install)
    closed_at = None
    how = "exited"
    while True:
        try:
            proc.wait(timeout=2)
            break
        except subprocess.TimeoutExpired:
            pass
        now = time.time()
        if closed_at is None:
            text = ""
            try:                                                      # (this run's log: the game truncates it at start)
                if os.path.getmtime(log_path) >= started - 1:
                    with open(log_path, encoding="latin-1", errors="replace") as f:
                        text = f.read()
            except OSError:
                pass
            ctl = R_ENDED.search(text)
            if (ctl and _seen_since(ctl, grace, now)) or now - started > timeout:
                how = "closed by the runner (%s)" % ("timeout" if now - started > timeout else "the player had control")
                close_windows_of(tree_of(proc.pid))
                closed_at = now
        elif now - closed_at > 60:
            how = "ended by the runner (didn't close)"
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)   # (its tree only)
            proc.wait()
            break
    secs = time.time() - started
    try:
        with open(log_path, encoding="latin-1", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    res = parse_log(lines)
    res.update({"entry": entry["name"], "session": session, "route": route, "exe": exe, "exit_code": proc.returncode,
                "how": how, "seconds": round(secs, 1)})
    if res["run"] and os.path.exists(log_path):
        keep = os.path.join(session_dir(install, entry), res["run"] + ".log")
        try:
            shutil.copyfile(log_path, keep)
            res["log"] = keep
        except OSError:
            pass
    if res["verdict"] != "PASS":
        res["log_tail"] = [ln for ln in lines if " session: " in ln or "replay: " in ln or "standalone:" in ln][-log_tail:]
    return res


_ctl_first_seen = {}


def _seen_since(match, grace, now):
    """True once the 'player has control' line has been in the log for `grace` seconds."""
    key = match.group(0)
    t = _ctl_first_seen.setdefault(key, now)
    return now - t >= grace


# ---- the corpus ------------------------------------------------------------------------------------------------------
def load_manifest():
    with open(MANIFEST, encoding="utf-8") as f:
        return json.load(f)


def session_dir(install, entry):
    """sessions/<name> for a bare name (copy 1's, as play= takes it); a path (sessions-2/<name>) from the install."""
    s = entry["session"].replace("/", os.sep)
    return os.path.join(install, s if os.sep in s else os.path.join("sessions", s))


def cmd_list(install, man):
    print("install: %s" % install)
    for e in man["sessions"]:
        d = session_dir(install, e)
        ok = os.path.isfile(os.path.join(d, "session.vps"))
        print("  %-14s %-28s %-15s %-26s %s%s" % (e["name"], e["session"], e.get("exe", "race.exe"),
                                                 ",".join(e["routes"]), "" if ok else "MISSING  ",
                                                 e.get("what", "")))


def selftest():
    lines = """\
10:00:00.000  session: replaying C:\\x\\sessions\\20261005-030122 (1234 input records, 1490 frames) as corpus-dll -> C:\\x.trace -- real input is ignored while it plays
10:01:00.000  replay: the race ended: 861 ticks, IDENTICAL to the recording over all 861 ticks compared
10:02:00.000  exit: session: corpus-dll: IDENTICAL over all 1490 frames compared (1056 of them in races; 0 in races out of lockstep not compared)
10:02:00.000  exit: session: fed 9999 Win32Idle calls (321 input events); 0 reads fell back on the last value or the live one, 0 recorded reads were never taken
10:02:00.000  exit: session: 1 races, 1 of them in lockstep (0 dropped it): 900 physics updates let run at 3000 sync points""".splitlines()
    r = parse_log(lines)
    assert r["verdict"] == "PASS", r["why"]
    assert (r["run"], r["frames_compared"], r["race_frames"], len(r["races"])) == ("corpus-dll", 1490, 1056, 1), r
    bad = lines[:2] + [
        "x  exit: session: corpus-dll-2: parted at frame 812 (the 2D page, tiles 5 6); 40 of 1490 frames compared differ",
        "x  exit: session: fed 9 Win32Idle calls (3 input events); 2 reads fell back on the last value or the live one, "
        "0 recorded reads were never taken; the user directory was NOT redirected",
        "x  exit: session: network: 1138 sends compared, 3 differ from the recording's; 1138 hashed into frames",
        "x  session: the window is 1536x864, the recording's was 3840x2160 -- the 2D page ...",
        "x  standalone: ORIGINAL CODE REACHED at 0x4a1b2c Foo::Bar, called from 0x401000 (thread 12)",
        "x  replay: the race ended: 10 ticks; first diverged at tick 4, 6 ticks differ"]
    r = parse_log(bad)
    assert r["verdict"] == "FAIL" and r["parted_at"] == 812 and r["fallbacks"] == 2 and r["sends_differ"] == 3, r
    assert r["window"] and r["original_code"] and r["redirected"] is False and not r["races"][1]["identical"], r
    assert len(r["why"]) == 7, r["why"]
    short = lines[:2] + ["x  exit: session: corpus-dll: IDENTICAL over all 900 frames compared (0 of them in races; 0 "
                         "in races out of lockstep not compared); the game closed before the recording ended"]
    r = parse_log(short)
    assert r["verdict"] == "FAIL" and r["fed_all"] is False, r
    r = parse_log(["x  session: NOT replaying -- can't read C:\\s\\session.vps"])
    assert r["verdict"] == "ERROR", r
    assert parse_log([])["verdict"] == "ERROR"
    t = "; top\n[session]\nrecord=1   ; keep\n\n[port]\ndefault=new\n"
    t2 = ini_for_run(t, "sessions-2\\a", "corpus-dll", "original")
    assert "record=0   ; keep" in t2 and "play=sessions-2\\a" in t2 and "default=original" in t2, t2
    assert "[replay]\nrecord=0" in t2 and "[test]\ntwo_copies=0" in t2 and t2.startswith("; top\n"), t2
    assert ini_for_run(t2, "b", "corpus-x", "new").count("play=") == 1
    t3 = ini_for_run(t, "a", "corpus-x", "new", {"test": {"two_copies": 1}})
    assert "two_copies=1" in t3 and "two_copies=0" not in t3, t3
    print("selftest: ok")


def main():
    ap = argparse.ArgumentParser(description="Replay the regression corpus on a build.")
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--only", nargs="*", help="entry names")
    ap.add_argument("--routes", help="comma-separated: " + ", ".join(ROUTES))
    ap.add_argument("--label", default="corpus", help="replay label prefix (the run is <label>-<route>)")
    ap.add_argument("--timeout", type=int, default=1800, help="seconds before a replay is closed regardless")
    ap.add_argument("--grace", type=int, default=10, help="seconds the player may hold control before the window is closed")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--parse", metavar="LOG")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if a.parse:
        with open(a.parse, encoding="latin-1", errors="replace") as f:
            r = parse_log(f.read().splitlines())
        print(json.dumps(r, indent=2))
        return 0 if r["verdict"] == "PASS" else 1
    man = load_manifest()
    install = os.path.abspath(a.install)
    if a.list:
        return cmd_list(install, man)
    want_routes = set(a.routes.split(",")) if a.routes else set(ROUTES)
    bad = want_routes - set(ROUTES)
    if bad:
        sys.exit("unknown route(s): %s" % ", ".join(sorted(bad)))
    plan = []
    for e in man["sessions"]:
        if a.only and e["name"] not in a.only:
            continue
        if not os.path.isfile(os.path.join(session_dir(install, e), "session.vps")):
            print("skip %s: %s isn't in the install" % (e["name"], e["session"]))
            continue
        for r in e["routes"]:
            if r in want_routes:
                plan.append((e, r))
    if a.only:
        missing = set(a.only) - {e["name"] for e in man["sessions"]}
        if missing:
            sys.exit("not in the corpus: %s" % ", ".join(sorted(missing)))
    if not plan:
        sys.exit("nothing to run")
    for e, r in plan:
        print("%-14s %-11s %s" % (e["name"], r, e["session"]))
    if a.dry_run:
        return 0
    busy = running_game_exes()
    if busy:
        sys.exit("the game is running (%s): close it first -- the runner never closes what it didn't start" % ", ".join(busy))

    ini_path = os.path.join(install, "viperport.ini")
    backup = ini_path + ".corpus-backup"
    if os.path.exists(backup):
        sys.exit("%s exists: a previous run didn't finish -- check it and put it back as viperport.ini first" % backup)
    shutil.copyfile(ini_path, backup)
    build = {"dinput.dll": sha12(os.path.join(install, "dinput.dll")),
             "viperport.exe": sha12(os.path.join(install, "viperport.exe"))}
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    results = []
    try:
        for i, (e, r) in enumerate(plan, 1):
            print("[%d/%d] %s on %s ..." % (i, len(plan), e["name"], r), flush=True)
            busy = running_game_exes()
            if busy:
                print("stopped: the game is still running (%s) -- a replay left it running, or someone started it" %
                      ", ".join(busy))
                break
            res = run_one(install, e, r, "%s-%s" % (a.label, r), a.timeout, a.grace, 12)
            results.append(res)
            print("        %s  %s  (%s s, %s)" % (res["verdict"], "; ".join(res["why"]) or
                                                 "%s frames, %d races" % (res["frames_compared"], len(res["races"])),
                                                 res["seconds"], res["how"]), flush=True)
    finally:
        shutil.copyfile(backup, ini_path)
        os.remove(backup)

    os.makedirs(os.path.join(REPO, "out", "corpus"), exist_ok=True)
    out = os.path.join(REPO, "out", "corpus", stamp + ".json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"when": stamp, "install": install, "build": build, "results": results}, f, indent=2)
    print()
    print("%-14s %-11s %-6s %8s %6s %5s  %s" % ("entry", "route", "", "frames", "races", "sends", "notes"))
    for res in results:
        print("%-14s %-11s %-6s %8s %6s %5s  %s" % (
            res["entry"], res["route"], res["verdict"], res["frames_compared"] if res["frames_compared"] is not None else "-",
            len(res["races"]), res["sends"] if res["sends"] is not None else "-", "; ".join(res["why"])))
    passed = sum(r["verdict"] == "PASS" for r in results)
    print("\n%d of %d PASS  (dinput.dll %s, viperport.exe %s)  -> %s" % (passed, len(results), build["dinput.dll"],
                                                                         build["viperport.exe"], out))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main() or 0)
