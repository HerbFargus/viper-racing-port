"""Compare a session recording's frames with a replay's, or read a session's input stream (hook/session.cpp).

    python tools/session_diff.py <sessions\\name>                 the recording against every replay in the folder
    python tools/session_diff.py <record.trace> <replay.trace>    two traces
    python tools/session_diff.py --stream <session.vps>           what a recording holds: reads by kind, frames, races
    python tools/session_diff.py --selftest                       the comparison, on synthetic traces

A trace holds, per frame, what the game handed the renderer: four part hashes (the 2D page, the surfaces, the 3D state,
the 3D work), the 2D page as a 4 x 4 grid of tiles, and each event's hash in order (the event's kind in the top 4 bits).
Frames are matched by number (a replay numbers its frames after a race as the recording did); a race's frames are
compared where it ran in lockstep, not where its lockstep was dropped; nor are a replay's frames after its recording
ran out.
"""
import os
import struct
import sys

HEAD = struct.Struct("<4sIII")
FRAME = struct.Struct("<IIII4I4Q16QIHHI")
F_RACE, F_PLAYER, F_TOLERANT, F_CUT, F_LOCKSTEP = 1, 2, 4, 8, 16   # F_RACE: out of lockstep, not compared
PARTS = ["the 2D page", "the surfaces (textures, offscreen images)",
         "the 3D state (render states, matrices, viewports, materials)", "the 3D work (draws, clears, transforms)"]
EVENTS = ["?", "2D page (Unlock)", "surface made", "surface written (Unlock)", "blit", "texture load", "colour key",
          "display mode", "clear", "TransformVertices", "draw"]
KINDS = {"Q": "clock (QueryPerformanceCounter)", "q": "clock rate (QueryPerformanceFrequency)",
         "W": "millisecond clock (timeGetTime)", "L": "date (GetLocalTime)", "R": "random number", "Z": "random seed",
         "J": "joystick (JoyGetPos)", "N": "joystick name", "H": "joystick force feedback", "I": "Win32Idle call",
         "A": "switch away / back", "F": "frame end (Flip)", "B": "race start", "E": "race end", "X": "end",
         "G": "window size", "P": "physics sync point (PhysicsGetStatePacket)", "K": "race lockstep dropped"}
OPS = {1: "KeyDown", 2: "KeyUp", 3: "KeyQueueChar", 4: "KeyQueueMetaChar", 5: "MouseQueueEvent", 6: "switch",
       7: "KeyClearBits", 8: "gxRestore", 9: "keys held", 10: "quit"}


class Frame:
    __slots__ = ("frame", "flags", "nevents", "count", "part", "tile", "ms", "page_w", "page_h", "input_at", "events")


def read_trace(path):
    data = open(path, "rb").read()
    if len(data) < HEAD.size or data[:4] != b"VPSF":
        raise SystemExit("%s isn't a session trace" % path)
    frames, at = [], HEAD.size
    while at + FRAME.size <= len(data):
        v = FRAME.unpack_from(data, at)
        f = Frame()
        f.frame, f.flags, f.nevents, nstored = v[0:4]
        f.count, f.part, f.tile = list(v[4:8]), list(v[8:12]), list(v[12:28])
        f.ms, f.page_w, f.page_h, f.input_at = v[28:32]
        at += FRAME.size
        if at + 4 * nstored > len(data):
            break                                                 # cut short (a crash)
        f.events = list(struct.unpack_from("<%dI" % nstored, data, at))
        at += 4 * nstored
        frames.append(f)
    return frames


def by_number(frames):
    """frame number -> frame; where a replay has two (a race ran longer than the recording's), the one outside a race"""
    out = {}
    for f in frames:
        old = out.get(f.frame)
        if old is None or (old.flags & F_RACE) or not (f.flags & F_RACE):
            out[f.frame] = f
    return out


def tiles(a, b):
    w, h = a.page_w or 640, a.page_h or 480
    return ", ".join("x %d-%d y %d-%d" % (i % 4 * w // 4, (i % 4 + 1) * w // 4 - 1, i // 4 * h // 4, (i // 4 + 1) * h // 4 - 1)
                     for i in range(16) if a.tile[i] != b.tile[i])


def describe(r, f):
    """what differs between the recording's frame r and the replay's f"""
    out = []
    for p in range(4):
        if r.part[p] != f.part[p] or r.count[p] != f.count[p]:
            s = PARTS[p]
            if r.count[p] != f.count[p]:
                s += " (%d in the replay, %d in the recording)" % (f.count[p], r.count[p])
            elif p == 0:
                t = tiles(r, f)
                if t:
                    s += " at " + t
            out.append(s)
    m = min(len(r.events), len(f.events))
    e = next((i for i in range(m) if r.events[i] != f.events[i]), m)
    if e < m:
        k = f.events[e] >> 28
        same = [i for i in range(len(f.events)) if f.events[i] >> 28 == k]
        s = "first at event %d of %d: %s %d of %d" % (e + 1, f.nevents, EVENTS[k] if k < len(EVENTS) else "?",
                                                      same.index(e) + 1, len(same))
        if r.events[e] >> 28 != k:
            rk = r.events[e] >> 28
            s += " (the recording has a %s there)" % (EVENTS[rk] if rk < len(EVENTS) else "?")
        out.append(s)
    elif r.nevents != f.nevents:
        out.append("the replay's frame has %d events, the recording's %d" % (f.nevents, r.nevents))
    return "; ".join(out) or "?"


def same(r, f):
    return (r.part == f.part and r.count == f.count and r.tile == f.tile and r.nevents == f.nevents
            and r.events == f.events)


def compare(rec_path, rep_path, show=5):
    rec, rep = by_number(read_trace(rec_path)), by_number(read_trace(rep_path))
    compared = differ = races = player = in_race = 0
    first = None
    shown = 0
    for n in sorted(rec):
        r = rec[n]
        f = rep.get(n)
        if f is None:
            continue
        if f.flags & F_PLAYER:
            player += 1
            continue
        if (r.flags | f.flags) & F_RACE:
            races += 1
            continue
        compared += 1
        in_race += bool((r.flags | f.flags) & F_LOCKSTEP)
        if same(r, f):
            continue
        differ += 1
        if first is None:
            first = n
            print("PARTED at frame %d (%.3f s in): %s" % (n, f.ms / 1000.0, describe(r, f)))
            if f.input_at != r.input_at:
                print("  (the input had already parted: the replay had read %d records, the recording %d)"
                      % (f.input_at, r.input_at))
        elif shown < show:
            print("  frame %d differs too: %s" % (n, describe(r, f)))
            shown += 1
    missing = sum(1 for n in rec if n not in rep)
    print("%s vs %s: %d frames compared (%d in races, in lockstep), %d differ; %d in races out of lockstep not compared; "
          "%d after the recording ran out; %d of the recording's frames the replay never reached"
          % (os.path.basename(rec_path), os.path.basename(rep_path), compared, in_race, differ, races, player, missing))
    if first is None and compared:
        print("IDENTICAL over all %d frames compared" % compared)
    return first is None


def read_stream(path):
    data = open(path, "rb").read()
    if data[:4] != b"VPSS":
        raise SystemExit("%s isn't a session stream" % path)
    at, recs = 16, []
    while at + 5 <= len(data):
        k = chr(data[at])
        n = struct.unpack_from("<I", data, at + 1)[0]
        if at + 5 + n > len(data):
            break
        recs.append((k, data[at + 5:at + 5 + n]))
        at += 5 + n
    return recs


def stream_summary(path):
    recs = read_stream(path)
    counts, ops = {}, {}
    grants = [0]
    frames = races = 0
    for k, p in recs:
        counts[k] = counts.get(k, 0) + 1
        if k == "F":
            frames += 1
        if k == "B":
            races += 1
            r, f = struct.unpack_from("<II", p)
            print("race %d starts at frame %d" % (r, f))
        if k == "E":
            r, f = struct.unpack_from("<II", p)
            print("race %d ends at frame %d" % (r, f))
        if k == "P":
            grants[0] += p[0]
        if k == "K":
            print("  the race's lockstep was dropped here")
        if k == "G":
            print("window %dx%d" % struct.unpack_from("<ii", p))
        if k == "I":
            grants[0] += p[0] if p else 0
            i = 1
            while i + 2 <= len(p):
                op, n = p[i], p[i + 1]
                ops[op] = ops.get(op, 0) + 1
                i += 2 + n
    print("%s: %d records, %d frames, %d races%s" % (path, len(recs), frames, races,
                                                   "" if recs and recs[-1][0] == "X" else " (no end: cut short)"))
    for k in sorted(counts, key=lambda c: -counts[c]):
        print("  %-7d %s" % (counts[k], KINDS.get(k, "'%s'?" % k)))
    if grants[0]:
        print("  %-7d physics updates let run at sync points (races in lockstep)" % grants[0])
    for op in sorted(ops):
        print("  %-7d op %s" % (ops[op], OPS.get(op, op)))


def selftest():
    import tempfile
    d = tempfile.mkdtemp()

    def write(path, frames):
        with open(path, "wb") as fh:
            fh.write(HEAD.pack(b"VPSF", 1, 0, 0))
            for (n, flags, parts, tl, ev) in frames:
                fh.write(FRAME.pack(n, flags, len(ev), len(ev), 1, 0, 0, max(len(ev) - 1, 0), *parts, *tl, n * 16, 640, 480, n * 3))
                fh.write(struct.pack("<%dI" % len(ev), *ev))

    base = [(n, 1 if 10 <= n < 20 else 0, [n, 2, 3, 4], [n + i for i in range(16)], [0x1000000 | n, 0xa0000001, 0xa0000002])
            for n in range(40)]
    a, b, c = os.path.join(d, "record.trace"), os.path.join(d, "same.trace"), os.path.join(d, "differ.trace")
    write(a, base)
    rep = [x for x in base if not 10 <= x[0] < 20] + [(n, 1, [0, 0, 0, 0], [0] * 16, []) for n in range(10, 25)]
    write(b, rep)                                                 # a longer race, renumbered after it
    bad = list(base)
    n, fl, parts, tl, ev = bad[30]
    tl = list(tl)
    tl[5] ^= 1
    bad[30] = (n, fl, [parts[0] ^ 1, parts[1], parts[2], parts[3] ^ 1], tl, [ev[0], ev[1], ev[2] ^ 1])
    write(c, bad)
    print("-- identical (the replay's race ran 15 frames, the recording's 10):")
    ok1 = compare(a, b)
    print("-- a frame differing:")
    ok2 = compare(a, c)
    assert ok1 and not ok2
    print("selftest: ok")


def main():
    args = sys.argv[1:]
    if args == ["--selftest"]:
        return selftest()
    if len(args) == 2 and args[0] == "--stream":
        return stream_summary(args[1])
    if len(args) == 2:
        return compare(args[0], args[1])
    if len(args) == 1 and os.path.isdir(args[0]):
        rec = os.path.join(args[0], "record.trace")
        runs = sorted(f for f in os.listdir(args[0]) if f.endswith(".trace") and f != "record.trace" and
                      not f.startswith("race-"))
        if not runs:
            print("no replays in %s" % args[0])
        for r in runs:
            print("== %s" % r)
            compare(rec, os.path.join(args[0], r))
        return
    print(__doc__)


if __name__ == "__main__":
    main()
