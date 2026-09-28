// session.cpp -- M3 UI step R: the session recorder. A whole run of the game -- start-up, the menus, races, quitting --
// recorded, and replayed on a fresh start of the game, with every frame compared against the recording's.
//
// The race recorder (replay.cpp) covers the physics thread; the UI stage rewrites code that runs on the main thread, in
// the menus. This is its check: record a session on the rewrites (default=new), replay it on the original code
// (default=original), and read "IDENTICAL over all N frames compared" or the first frame where they part, and what
// differed.
//
//   [session]
//   record=1                   ; record this run to sessions\<time>\ beside the DLL
//   play=20260927-193012       ; instead, replay that recording (it needs a fresh start: the ini is read at start-up)
//   label=original             ; this replay's name: sessions\<play>\<label>.trace, and its own copy of the user folder
//
// WHAT IS RECORDED (sessions\<name>\session.vps): everything the main thread reads that can differ between two runs, in
// the order it reads it --
//   * the input the platform layer (platform.cpp) hands the game: every Win32Idle call's events (keys, characters, the
//     mouse in the game's pixels, switching away and back, KeyClearBits / gxRestore on the way back, the window closing)
//     and the keys held afterwards (ScanUpdate's DirectInput state); JoyGetPos, JoyGetName, JoyHasForceFeedback;
//   * every clock: QueryPerformanceCounter / QueryPerformanceFrequency (PTimeNow, ProfBegin), timeGetTime
//     (Win32GetTime), GetLocalTime (TimeGetTimeOfDay) -- through the game's own import slots, which the rewrites use
//     too. (rdtsc is PTimeNow's clock only on a one-processor machine, where ProfBegin picks it: logged, not recorded.)
//     There is no other: the imports have no GetTickCount, GetSystemTime or C-runtime time();
//   * the random numbers the main thread draws (Random: the world's effects, the career, the credits), and the start-up
//     seed (Randomize, as replay.cpp chooses it);
//   * the frame boundaries (Flip), and where each race starts and ends (PhysicsStart / PhysicsStop).
// Each record is a kind byte, a u32 length and the value. A replay feeds them back in the same order: real input is
// ignored while it plays (closing the window still quits), and the game runs as fast as the fed clock allows (vsync
// off). Where the replay reads something other than what the recording has next, that is the first place it parts
// ("the game read the clock where the recording has a random number"); it goes on feeding by Win32Idle call from then
// on, each read taking the next of its kind (tolerant). When the recording runs out the player gets control, the clocks
// carrying on from the recording's.
//
// THE FRAME HASH (sessions\<name>\record.trace, a replay's <label>.trace): per frame, what the GAME handed the renderer
// (gl_core.cpp), not the upscaled picture: the 2D page at each Unlock (also as a 4 x 4 grid of tiles), the offscreen and
// texture surfaces it writes (made, Unlock, blits, texture loads, colour keys), the 3D state (render states, matrices,
// viewports, materials; a texture handle by its pixels, never its address) and the 3D work (draws with their vertices --
// minus the stack garbage Direct3D never reads, as the shadow checks leave it out -- clears, TransformVertices). Four
// part hashes, sixteen tile hashes and each event's hash in order, so a mismatch names the part, the tiles and the first
// differing event ("the 3rd draw of 40"). A replay compares every frame live; tools/session_diff.py compares any two
// traces. The 2D page as the game locks it holds the renderer's 3D read back at the window's size, so record and replay
// at the same window size (the recording's is logged; a replay warns if it differs).
//
// FILES: the user directory (Config\: options.cfg, records, setups, ghosts, paint jobs, the career, the game's replays)
// is copied into sessions\<name>\config\ when get_user_directory names it, before anything reads it; a replay copies
// that snapshot again, into sessions\<name>\<label>\config\, and points the game's user directory there, so it starts
// from the same files and the player's own Config\ is never touched. Game data (tracks, cars, sounds) is read-only and
// stays where it is. Not covered: what the game writes elsewhere (its logs; the AI learn mode's notes, a dev option).
//
// THREADS: in the menus only the main thread and the BG timer thread (the SoundManager) run. The main thread sends the
// sound manager commands but reads nothing back that changes a frame (the game has no "is it playing" query), and no
// other thread draws random numbers outside a race, so a menu frame depends only on what is recorded. In a race the
// physics runs on the BG thread (physics_thread, every 16 ms), and the two threads read each other's state all the
// time: the main thread the physics' packet, clock, deity and records; the physics what the main thread sets between
// its updates -- the pause, the camera and focus car, the abort (quitting), the game's replay controls. Where one
// thread's view of the other lands depends on thread timing, so a race is run in LOCKSTEP, recorded and replayed:
//   * a physics update (PhysTaskUpdate in the task's run states, 3 and 6) runs only when the main thread lets it, at
//     a sync point, and the main thread waits there until it's done. The sync points: PhysicsGetStatePacket (the world
//     reading the packet, once a frame: one update, always -- where the original would have waited for a fresh packet)
//     and every Win32Idle (one update if the physics has waited 16 ms or more, so it keeps its pace when the main thread
//     isn't drawing the world). The recording stores each sync point's count ('P' records, and each Win32Idle record's
//     first byte); a replay lets exactly those updates run at exactly those points. The task's other states (begin,
//     restart, post-race, end), which the main thread waits for in PhysicsStart / Restart / PostRace / Stop, aren't
//     gated: they run once each, at the point the main thread asks for them, in both runs.
//     The physics then sees every main-thread write at the same update, and the main thread every physics state at
//     the same frame: the race's frames are compared like the menus', and its reads are fed strictly.
//   * Each race is also handed to the race recorder, recorded to (replayed from) sessions\<name>\race-<k>.vpr -- ticks
//     (still from the real-time clock while recording), pauses, randoms, controls -- restarts are races of their own.
//   * If the lockstep can't be kept -- the physics waited 3 s for a sync point (a main-thread stall), or a replay's
//     physics doesn't come -- it's dropped for the rest of that race and logged (a 'K' record, so the replay drops it at
//     the same point): its frames aren't compared, its reads are fed by Win32Idle call (tolerant), and at its end the
//     stream skips to the recording's end of the race and comparing starts again.
// Not covered: the network (the multiplayer screens read sockets; a network race can't be gated either).
//
// Shadow checks can run alongside: what a check's rewrite pass reads or draws is never really read or drawn, so it goes
// past the session (live, unrecorded, unhashed); the original's pass is the game's own.
//
// v1.0 only, on the SDL platform layer ([platform] sdl=1); frames are hashed on the OpenGL renderer (renderer=gl).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "viperport.h"
#include "port.h"
#include "session.h"
#ifndef SESSION_TEST
#include "SDL.h"
SDL_Window* platform_window();
#endif

volatile int g_session_mode = SESSION_OFF;
bool g_session_frames;

namespace {

// ---- the stream's kinds ------------------------------------------------------------------------------------------------
enum : uint8_t {
    K_QPC = 'Q', K_QPF = 'q', K_TGT = 'W', K_LOCALTIME = 'L', K_RANDOM = 'R', K_SEED = 'Z',   // values (reads)
    K_GRANT = 'P',                                                    // PhysicsGetStatePacket's sync point: updates let run
    K_IDLE = 'I', K_ACTIVE = 'A', K_FRAME = 'F', K_RACE_BEGIN = 'B', K_RACE_END = 'E', K_END = 'X',
    K_LOST = 'K',                                                     // the race's lockstep was dropped here
    K_INFO = 'G',                                                     // the window's size: not a read
};

const char* kind_name(uint8_t k) {
    switch (k) {
    case K_QPC: return "the clock (QueryPerformanceCounter)";
    case K_QPF: return "the clock's rate (QueryPerformanceFrequency)";
    case K_TGT: return "the millisecond clock (timeGetTime)";
    case K_LOCALTIME: return "the date (GetLocalTime)";
    case K_RANDOM: return "a random number";
    case K_SEED: return "the random seed";
    case K_GRANT: return "a physics sync point (PhysicsGetStatePacket)";
    case K_LOST: return "the race's lockstep dropped";
    case SK_JOYPOS: return "the joystick (JoyGetPos)";
    case SK_JOYNAME: return "the joystick's name";
    case SK_JOYFF: return "the joystick's force feedback";
    case K_IDLE: return "the input events (Win32Idle)";
    case K_ACTIVE: return "a switch away or back";
    case K_FRAME: return "a frame's end (Flip)";
    case K_RACE_BEGIN: return "a race's start";
    case K_RACE_END: return "a race's end";
    case K_END: return "the end of the recording";
    }
    return "?";
}
bool is_value(uint8_t k) {
    return k == K_QPC || k == K_QPF || k == K_TGT || k == K_LOCALTIME || k == K_RANDOM || k == K_SEED || k == K_GRANT || k == SK_JOYPOS ||
           k == SK_JOYNAME || k == SK_JOYFF;
}
bool is_boundary(uint8_t k) { return k == K_IDLE || k == K_RACE_BEGIN || k == K_RACE_END || k == K_END; }

#pragma pack(push, 1)
struct StreamHeader { char magic[4]; uint32_t version, build, reserved; };
struct TraceHeader { char magic[4]; uint32_t version, reserved[2]; };
struct TraceFrame {
    uint32_t frame;              // the recording's numbering (a replay renumbers after each race to match)
    uint32_t flags;              // F_*
    uint32_t nevents, nstored;   // events in the frame; how many of their hashes follow the record
    uint32_t count[SP_N];        // events (and state changes) per part
    uint64_t part[SP_N];
    uint64_t tile[16];           // the 2D page, 4 x 4 (row-major, top-left first)
    uint32_t ms;                 // the game's clock at the frame's end, from the first clock read
    uint16_t page_w, page_h;     // the 2D page's size (0: no page this frame)
    uint32_t input_at;           // input records read so far
};
enum { IDLE_HEAD = 1 };                          // a Win32Idle record: the physics updates let run there, then its ops
struct RaceMark { uint32_t race, frame; };
#pragma pack(pop)
// flags: in a race out of lockstep (not compared) / after the recording ran out / after a part / events cut short /
// in a race in lockstep (compared)
enum { F_RACE = 1, F_PLAYER = 2, F_TOLERANT = 4, F_CUT = 8, F_LOCKSTEP = 16 };
enum { MAX_EVENTS = 4096 };                                        // event hashes kept per frame

const char* const PART_NAME[SP_N] = {"the 2D page", "the surfaces (textures, offscreen images)",
                                     "the 3D state (render states, matrices, viewports, materials)",
                                     "the 3D work (draws, clears, transforms)"};
const char* event_name(uint32_t k) {
    static const char* const n[] = {"?", "2D page (Unlock)", "surface made", "surface written (Unlock)", "blit",
                                    "texture load", "colour key", "display mode", "clear", "TransformVertices", "draw"};
    return k < sizeof n / sizeof n[0] ? n[k] : "?";
}
uint8_t part_of(uint8_t kind) {
    switch (kind) {
    case SG_PAGE: return SP_PAGE;
    case SG_SURFACE: case SG_UNLOCK: case SG_BLIT: case SG_TEXLOAD: case SG_KEY: return SP_SURFACES;
    case SG_MODE: return SP_STATE;
    }
    return SP_DRAWS;
}

// ---- state ---------------------------------------------------------------------------------------------------------------
DWORD g_main;                                   // the session's thread: the game's main thread (DllMain's)
char g_dir[MAX_PATH];                           // sessions\<name>
char g_name[128];                               // the recording
char g_run[96];                                 // play: this replay's name (the label, made unique)
char g_run_dir[MAX_PATH];                       // play: sessions\<name>\<run>
bool g_redirect_ok;                             // play: the user directory was pointed at the run's copy
FILE* g_stream;                                 // record
FILE* g_trace;                                  // record: record.trace; play: <run>.trace

// play: the recording, indexed
struct Rec { uint8_t kind; uint32_t off, len; };
std::vector<uint8_t> g_in;
std::vector<Rec> g_recs;
std::vector<uint8_t> g_used;
size_t g_ri;                                    // the next record
bool g_tolerant;                                // parted: feeding by Win32Idle call, each read the next of its kind
bool g_tolerant_before_race;
std::vector<uint8_t> g_last[256];               // the last value fed of each kind (a fallback once a group runs dry)
// the recording's frames (play)
std::vector<uint8_t> g_ref;
std::vector<size_t> g_ref_at;                   // frame -> offset in g_ref

// the race (main thread's view): between PhysicsStart and the end of PhysicsStop
bool g_in_race;
uint32_t g_races;                               // races started (PhysicsStart)
volatile LONG g_race_files;                     // races the race recorder began (restarts too): race-<k>
bool g_lockstep_race;                           // this race is in lockstep (main thread's view)
bool g_race_tolerant;                           // play: this race's reads are fed tolerantly (its lockstep dropped)

// idle groups (record)
bool g_idle_open;
std::vector<uint8_t> g_idle;

// counts
uint32_t g_frame;                               // frames ended (Flip)
unsigned long g_records, g_idles, g_events;     // record: written; play: fed
unsigned long g_left_over, g_fallbacks, g_race_events_unused, g_other_thread_draws;
unsigned long g_fb_kind[256], g_lo_kind[256];                     // by kind, and the frame of the first of each
uint32_t g_fb_first[256], g_lo_first[256];
void count_kind(unsigned long* n, uint32_t* first, uint8_t k, uint32_t frame) { if (!n[k]++) first[k] = frame; }
uint32_t g_compared, g_differ, g_race_frames, g_race_compared, g_first_part = UINT32_MAX;
unsigned long g_sync_points, g_grants_given, g_races_lockstep, g_races_lost;
char g_first_part_what[400];
bool g_first_present_done;
int g_win_w, g_win_h;

// clocks (for the log's times, and a replay's other threads / the player's control afterwards)
int64_t g_first_qpc, g_last_qpc, g_qpf;
volatile LONGLONG g_qpc_off;                    // play: added to the live QPC outside the fed reads
volatile LONG g_tgt_off;
bool g_have_qpc_off, g_have_tgt_off;
uint32_t g_last_tgt;

// the session's thread -- and not a shadow check's rewrite pass (port.h), whose reads are never really made: they go
// live, unrecorded, so a recording with shadow checks on holds only what the game really read
inline bool on_main() { return GetCurrentThreadId() == g_main && shadow_com_phase() != 2; }

// ---- hashing -------------------------------------------------------------------------------------------------------------
inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
    return h ^ (h >> 29);
}

// ---- files ---------------------------------------------------------------------------------------------------------------
bool is_dir(const char* p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void path_in(char* out, size_t n, const char* dir, const char* name) {
    _snprintf(out, n, "%s\\%s", dir, name);
    out[n - 1] = 0;
}

// every file and folder under from (no trailing backslash) copied into to, made; the source only read
struct TreeCount { unsigned files, failed, skipped; unsigned long long bytes; };
void copy_tree(const char* from, const char* to, TreeCount& c, int depth) {
    CreateDirectoryA(to, 0);
    char pat[MAX_PATH];
    if (strlen(from) + 3 > MAX_PATH) { c.skipped++; return; }
    path_in(pat, sizeof pat, from, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char a[MAX_PATH], b[MAX_PATH];
        if (strlen(from) + strlen(fd.cFileName) + 2 > MAX_PATH || strlen(to) + strlen(fd.cFileName) + 2 > MAX_PATH) {
            c.skipped++;
            continue;
        }
        path_in(a, sizeof a, from, fd.cFileName);
        path_in(b, sizeof b, to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth >= 16 || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { c.skipped++; continue; }
            copy_tree(a, b, c, depth + 1);
        } else if (CopyFileA(a, b, FALSE)) {
            c.files++;
            c.bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        } else {
            c.failed++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

bool load_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    return got == out.size();
}

// ---- the stream: writing (record) ------------------------------------------------------------------------------------------
void put(uint8_t kind, const void* p, size_t n) {
    if (!g_stream) return;
    uint32_t len = (uint32_t)n;
    fputc(kind, g_stream);
    fwrite(&len, 4, 1, g_stream);
    if (n) fwrite(p, 1, n, g_stream);
    g_records++;
}

// ---- the stream: reading (play) ----------------------------------------------------------------------------------------------
void end_of_recording(const char* why);
void lose(const char* why);
void apply_idle(const Rec& r);

bool index_stream() {
    if (g_in.size() < sizeof(StreamHeader) || memcmp(g_in.data(), "VPSS", 4)) return false;
    size_t at = sizeof(StreamHeader);
    while (at + 5 <= g_in.size()) {
        Rec r;
        r.kind = g_in[at];
        memcpy(&r.len, &g_in[at + 1], 4);
        r.off = (uint32_t)(at + 5);
        if (r.off + (size_t)r.len > g_in.size()) break;               // cut short (a crash): what's whole is kept
        g_recs.push_back(r);
        at = r.off + r.len;
    }
    g_used.assign(g_recs.size(), 0);
    return true;
}

const uint8_t* payload(const Rec& r) { return g_in.data() + r.off; }

void apply_active(const Rec& r) {
    if (r.len >= 1) platform_session_apply(SOP_ACTIVE, payload(r), 1);
}

// past the records already taken and what's no read (the window's size); a switch away or back is applied on the way.
// Tolerant, a frame's end is passed too (only strict reading checks them).
void settle() {
    while (g_ri < g_recs.size()) {
        const Rec& r = g_recs[g_ri];
        if (g_used[g_ri] || r.kind == K_INFO || (g_tolerant && r.kind == K_FRAME)) { g_ri++; continue; }
        if (r.kind == K_ACTIVE) { apply_active(r); g_used[g_ri++] = 1; continue; }
        break;
    }
}

bool at_end() { return g_ri >= g_recs.size() || g_recs[g_ri].kind == K_END; }

// move on to record `to`, passing everything before it: switches applied, reads never taken counted
void pass_to(size_t to, bool race_end = false) {
    for (; g_ri < to && g_ri < g_recs.size(); g_ri++) {
        if (g_used[g_ri]) continue;
        const Rec& r = g_recs[g_ri];
        if (r.kind == K_ACTIVE) apply_active(r);
        else if (is_value(r.kind)) g_left_over++, count_kind(g_lo_kind, g_lo_first, r.kind, g_frame);
        else if (r.kind == K_IDLE && race_end) {
            const uint8_t* p = payload(r);                            // events that never reached the replay
            for (uint32_t i = IDLE_HEAD; i + 2 <= r.len; i += 2 + p[i + 1])
                if (p[i] != SOP_SCAN) g_race_events_unused++;
        }
        g_used[g_ri] = 1;
    }
}

uint32_t game_ms() {
    if (!g_qpf || !g_first_qpc) return 0;
    return (uint32_t)((g_last_qpc - g_first_qpc) * 1000 / g_qpf);
}

// the first place the replay's reads part from the recording's: logged once, then tolerant reading
void part_input(uint8_t want, uint8_t have, const char* more = 0) {
    if (!g_tolerant) {
        char what[300];
        if (more) _snprintf(what, sizeof what, "the input reads part: %s", more);
        else _snprintf(what, sizeof what, "the input reads part: the game read %s where the recording has %s", kind_name(want),
                       kind_name(have));
        what[sizeof what - 1] = 0;
        if (g_first_part == UINT32_MAX) {
            g_first_part = g_frame;
            strcpy(g_first_part_what, what);
        }
        logf("session: PARTED from the recording at frame %u (%.3f s in): %s -- feeding the rest by Win32Idle call, each"
             " read the next of its kind", g_frame, game_ms() / 1000.0, what);
    }
    g_tolerant = true;
    if (g_lockstep_race && g_session_mode == SESSION_PLAY) lose("the replay's input parted from the recording's");
}

// the next record of `kind` in the current Win32Idle's group (tolerant)
bool take_tolerant(uint8_t kind, void* v, size_t n) {
    settle();
    if (at_end()) { end_of_recording("the recording ended"); return false; }
    for (size_t j = g_ri; j < g_recs.size() && !is_boundary(g_recs[j].kind); j++) {
        const Rec& r = g_recs[j];
        if (g_used[j] || r.kind != kind || r.len != n) continue;
        memcpy(v, payload(r), n);
        g_used[j] = 1;
        if (j == g_ri) g_ri++;
        return true;
    }
    return false;
}

bool take(uint8_t kind, void* v, size_t n) {
    if (g_session_mode != SESSION_PLAY) return false;
    bool ok;
    if (!g_tolerant) {
        settle();
        if (at_end()) { end_of_recording("the recording ended"); return false; }
        const Rec& r = g_recs[g_ri];
        if (r.kind == kind && r.len == n) {
            memcpy(v, payload(r), n);
            g_used[g_ri++] = 1;
            ok = true;
        } else {
            part_input(kind, r.kind);
            ok = take_tolerant(kind, v, n);
        }
    } else {
        ok = take_tolerant(kind, v, n);
    }
    if (g_session_mode != SESSION_PLAY) return false;                  // (it ran out on the way)
    if (ok) {
        g_last[kind].assign((const uint8_t*)v, (const uint8_t*)v + n);
        return true;
    }
    // tolerant, and the group has none: the last one fed, if the caller can use it (a clock stands still)
    g_fallbacks++;
    count_kind(g_fb_kind, g_fb_first, kind, g_frame);
    if (g_last[kind].size() == n && kind != K_RANDOM) {
        memcpy(v, g_last[kind].data(), n);
        return true;
    }
    return false;
}

void save_value(uint8_t kind, const void* v, size_t n) {
    if (g_session_mode == SESSION_RECORD) put(kind, v, n);
}

// ---- the clocks: the game's import slots --------------------------------------------------------------------------------------
typedef BOOL(WINAPI* Qpc_t)(LARGE_INTEGER*);
typedef DWORD(WINAPI* Tgt_t)(void);
typedef void(WINAPI* LocalTime_t)(LPSYSTEMTIME);
Qpc_t o_QPC, o_QPF;
Tgt_t o_timeGetTime;
LocalTime_t o_GetLocalTime;
enum : uint32_t { SLOT_QPC = 0x005d7488, SLOT_QPF = 0x005d74c4, SLOT_TGT = 0x005d76d8, SLOT_LOCALTIME = 0x005d75f0 };

#pragma pack(push, 1)
struct QpcRec { int64_t v; int32_t ok; };
#pragma pack(pop)

void note_qpc(int64_t v) {
    if (!g_first_qpc) g_first_qpc = v;
    g_last_qpc = v;
}

// a replay's other threads, and the player's clock once the recording ends: the live clock moved to carry on from the
// recording's (only ever forwards, so it never runs back)
void align_clocks() {
    if (g_last[K_QPC].size() == sizeof(QpcRec) && o_QPC) {
        QpcRec r;
        memcpy(&r, g_last[K_QPC].data(), sizeof r);
        LARGE_INTEGER now;
        o_QPC(&now);
        LONGLONG off = r.v - now.QuadPart;
        if (!g_have_qpc_off || off > g_qpc_off) g_qpc_off = off, g_have_qpc_off = true;
    }
    if (g_last[K_TGT].size() == 4 && o_timeGetTime) {
        uint32_t v;
        memcpy(&v, g_last[K_TGT].data(), 4);
        LONG off = (LONG)(v - o_timeGetTime());
        if (!g_have_tgt_off || off > g_tgt_off) g_tgt_off = off, g_have_tgt_off = true;
    }
}

BOOL WINAPI h_QPC(LARGE_INTEGER* p) {
    const int mode = g_session_mode;
    if (!on_main()) {
        BOOL ok = o_QPC(p);
        if (ok && mode != SESSION_RECORD && g_have_qpc_off) p->QuadPart += g_qpc_off;
        return ok;
    }
    QpcRec r;
    if (take(K_QPC, &r, sizeof r)) {
        p->QuadPart = r.v;
        note_qpc(r.v);
        return r.ok;
    }
    BOOL ok = o_QPC(p);                                               // (a replay's player, or its first read went short)
    if (ok && g_session_mode != SESSION_RECORD && g_have_qpc_off) p->QuadPart += g_qpc_off;
    r.v = p->QuadPart, r.ok = ok;
    save_value(K_QPC, &r, sizeof r);
    note_qpc(r.v);
    return ok;
}

BOOL WINAPI h_QPF(LARGE_INTEGER* p) {
    if (!on_main()) return o_QPF(p);
    QpcRec r;
    if (take(K_QPF, &r, sizeof r)) {
        p->QuadPart = r.v;
        g_qpf = r.v;
        return r.ok;
    }
    BOOL ok = o_QPF(p);
    r.v = p->QuadPart, r.ok = ok;
    save_value(K_QPF, &r, sizeof r);
    g_qpf = r.v;
    return ok;
}

DWORD WINAPI h_timeGetTime() {
    const int mode = g_session_mode;
    if (!on_main()) return o_timeGetTime() + (mode != SESSION_RECORD && g_have_tgt_off ? (DWORD)g_tgt_off : 0);
    uint32_t v;
    if (take(K_TGT, &v, 4)) return g_last_tgt = v;
    v = o_timeGetTime();
    if (g_session_mode != SESSION_RECORD && g_have_tgt_off) v += (DWORD)g_tgt_off;
    save_value(K_TGT, &v, 4);
    return g_last_tgt = v;
}

void WINAPI h_GetLocalTime(LPSYSTEMTIME t) {
    if (!on_main()) return o_GetLocalTime(t);
    if (take(K_LOCALTIME, t, sizeof *t)) return;
    o_GetLocalTime(t);
    save_value(K_LOCALTIME, t, sizeof *t);
}

void* patch_slot(uint32_t slot, void* to) {
    void** p = (void**)(uintptr_t)slot;
    DWORD old;
    if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) return 0;
    void* was = *p;
    *p = to;
    VirtualProtect(p, 4, old, &old);
    return was;
}

// ---- the end of a replay's recording ---------------------------------------------------------------------------------------------
void end_of_recording(const char* why) {
    if (g_session_mode != SESSION_PLAY) return;
    align_clocks();                                                   // the player's clocks carry on from the recording's
    g_session_mode = SESSION_ENDED;
    logf("session: %s at frame %u (%.3f s in) -- the player has control from here", why, g_frame, game_ms() / 1000.0);
}

// ---- Win32Idle: the input events -------------------------------------------------------------------------------------------------
void apply_idle(const Rec& r) {
    const uint8_t* p = payload(r);
    for (uint32_t i = IDLE_HEAD; i + 2 <= r.len;) {
        uint8_t op = p[i], n = p[i + 1];
        if (i + 2 + n > r.len) break;
        if (op != SOP_SCAN) g_events++;
        platform_session_apply(op, p + i + 2, n);
        i += 2 + n;
    }
    g_idles++;
}

// ---- lockstep: the race's physics updates at the main thread's sync points ---------------------------------------------
#ifdef SESSION_TEST
volatile int32_t* g_phys_state_ptr;                  // the harness's
#define PHYS_STATE (*g_phys_state_ptr)
#else
#define PHYS_STATE (*(volatile int32_t*)0x004ecf88)  // physics.obj's task state: 3 and 6 run an update each tick
#endif
volatile LONG g_gate_on;                        // the physics waits for its updates (a race in lockstep)
volatile LONG g_gate_lost;                      // it gave up waiting (its thread); the main thread notes it at its next sync point
volatile LONG g_at_gate, g_grants, g_done;
volatile DWORD g_arrived;                       // when it came to the gate (GetTickCount)
HANDLE g_ev_grant, g_ev_arrive, g_ev_done;

inline bool gated_state(int32_t s) { return s == 3 || s == 6; }

enum Grant { G_NONE, G_DONE, G_LOST };
// the main thread: one physics update let run, and waited for. G_NONE: the task isn't in a run state (nothing to let)
Grant grant_one(DWORD limit) {
    DWORD t0 = GetTickCount();
    while (!g_at_gate) {
        if (!g_gate_on) return G_LOST;
        if (!gated_state(PHYS_STATE)) return G_NONE;
        if (GetTickCount() - t0 > limit) return G_LOST;
        WaitForSingleObject(g_ev_arrive, 2);
    }
    const LONG d0 = g_done;
    InterlockedIncrement(&g_grants);
    SetEvent(g_ev_grant);
    t0 = GetTickCount();
    while (g_done == d0) {
        if (GetTickCount() - t0 > limit) return G_LOST;
        WaitForSingleObject(g_ev_done, 2);
    }
    g_grants_given++;
    return G_DONE;
}

void lockstep_on() {
    InterlockedExchange(&g_grants, 0);
    InterlockedExchange(&g_gate_lost, 0);
    g_lockstep_race = true;
    InterlockedExchange(&g_gate_on, 1);
}
void lockstep_off() {
    InterlockedExchange(&g_gate_on, 0);
    InterlockedExchange(&g_grants, 0);
    SetEvent(g_ev_grant);
    g_lockstep_race = false;
}

void frame_race_flag();

// the race's lockstep dropped (main thread): its frames not compared, its reads fed tolerantly
void lose(const char* why) {
    if (!g_lockstep_race) return;
    lockstep_off();
    g_races_lost++;
    frame_race_flag();
    if (g_session_mode == SESSION_RECORD) put(K_LOST, 0, 0);
    else if (g_session_mode == SESSION_PLAY) g_race_tolerant = g_tolerant = true;
    logf("session: race %u: lockstep dropped at frame %u (%s) -- this race's frames aren't compared from here", g_races,
         g_frame, why);
}

// a sync point while recording: how many updates were let run (idle: one if the physics has waited 16 ms or more)
uint8_t sync_record(bool idle) {
    if (!g_lockstep_race) return 0;
    g_sync_points++;
    if (g_gate_lost) { lose("the physics waited 3 s for the main thread"); return 0; }
    if (idle && !(g_at_gate && GetTickCount() - g_arrived >= 16)) return 0;
    const Grant r = grant_one(2000);
    if (r == G_LOST) { lose("the physics didn't come to its update in 2 s"); return 0; }
    return r == G_DONE ? 1 : 0;
}

// a sync point while replaying: the recording dropped its lockstep here ('K' next), or let `n` updates run
bool play_lost_here() {
    if (!g_lockstep_race) return false;
    settle();
    if (g_ri < g_recs.size() && g_recs[g_ri].kind == K_LOST) {
        g_used[g_ri++] = 1;
        lose("the recording dropped it here");
        return true;
    }
    return false;
}
void sync_play(uint8_t n) {
    if (!g_lockstep_race) return;
    g_sync_points++;
    for (uint8_t i = 0; i < n && g_lockstep_race; i++) {
        const Grant r = grant_one(15000);
        if (r != G_DONE)
            lose(r == G_NONE ? "the replayed physics isn't running where the recording's ran"
                             : "the replayed physics didn't come to its update in 15 s");
    }
    if (g_gate_lost) lose("the replayed physics waited 30 s for the main thread");
}

size_t next_idle(size_t from) {
    for (size_t j = from; j < g_recs.size(); j++) {
        uint8_t k = g_recs[j].kind;
        if (k == K_IDLE && !g_used[j]) return j;
        if (k == K_RACE_BEGIN || k == K_RACE_END || k == K_END) return SIZE_MAX;
    }
    return SIZE_MAX;
}

bool play_idle_tolerant() {
    settle();
    if (at_end()) { end_of_recording("the recording ended"); return false; }
    size_t j = next_idle(g_ri);
    if (j == SIZE_MAX) {
        if (g_in_race) { g_fallbacks++; return true; }                // none left before the race's end: no events
        // parted, and the recording's input runs out before its next race (or its end): nothing more fits
        size_t b = g_ri;
        while (b < g_recs.size() && g_recs[b].kind != K_RACE_BEGIN) b++;
        end_of_recording(b < g_recs.size() ? "the replay parted, and the recording has no more input before its next race"
                                           : "the replay parted, and the recording has no more input");
        return false;
    }
    pass_to(j);
    g_used[j] = 1;
    g_ri = j + 1;
    apply_idle(g_recs[j]);
    return true;
}

// ---- the race recorder's races, and the lockstep's hooks -----------------------------------------------------------------------
typedef void(__cdecl* Void_t)(void);
Void_t o_PhysicsStart, o_PhysicsStop;
typedef uint8_t*(__cdecl* GetPacket_t)(void*, void*);
GetPacket_t o_PhysicsGetStatePacket;

void __cdecl h_PhysicsStart() {
    if (on_main() && g_session_mode != SESSION_OFF) {
        g_races++;
        RaceMark m = {g_races, g_frame};
        g_race_tolerant = false;
#ifndef SESSION_TEST
        const bool network = ((uint8_t(__cdecl*)())0x004a23c0)() != 0;   // MultiEnabled: a network race can't wait
#else
        const bool network = false;
#endif
        if (g_session_mode == SESSION_RECORD) {
            put(K_RACE_BEGIN, &m, sizeof m);
            if (!network) lockstep_on();
        } else if (g_session_mode == SESSION_PLAY) {
            settle();
            if (!g_tolerant && !at_end() && g_recs[g_ri].kind != K_RACE_BEGIN) part_input(K_RACE_BEGIN, g_recs[g_ri].kind);
            size_t j = g_ri;
            while (j < g_recs.size() && g_recs[j].kind != K_RACE_BEGIN && g_recs[j].kind != K_END) j++;
            if (j < g_recs.size() && g_recs[j].kind == K_RACE_BEGIN) {
                pass_to(j);
                g_used[j] = 1;
                g_ri = j + 1;
            }
            g_tolerant_before_race = g_tolerant;
            if (!g_tolerant && !network) lockstep_on();               // (a replay that has parted can't keep step)
            else g_race_tolerant = g_tolerant = true;                   // (fed by Win32Idle call)
            align_clocks();                                           // the physics thread's clock near the fed one
        }
        g_in_race = true;
        frame_race_flag();
        if (g_lockstep_race) g_races_lockstep++;
        if (g_session_mode != SESSION_ENDED)
            logf("session: race %u starts at frame %u -- %s; the race recorder %s its inputs", g_races, g_frame,
                 g_lockstep_race ? "in lockstep (the physics updates at the main thread's sync points), frames compared"
                 : network ? "NOT in lockstep (a network race): frames aren't compared until it ends"
                           : "NOT in lockstep (the replay had parted): frames aren't compared until it ends",
                 g_session_mode == SESSION_RECORD ? "records" : "replays");
    }
    o_PhysicsStart();
}

void __cdecl h_PhysicsStop() {
    o_PhysicsStop();                                  // (sets the end state: a physics waiting at the gate goes on to it)
    if (!on_main() || g_session_mode == SESSION_OFF || !g_in_race) return;
    const bool stepped = g_lockstep_race;
    frame_race_flag();                                // (this frame: in lockstep if the race was)
    lockstep_off();
    g_in_race = false;
    RaceMark m = {g_races, g_frame};
    if (g_session_mode == SESSION_RECORD) {
        put(K_RACE_END, &m, sizeof m);
        logf("session: race %u ended at frame %u%s", g_races, g_frame, stepped ? " (in lockstep)" : "");
    } else if (g_session_mode == SESSION_PLAY) {
        settle();
        if (!g_tolerant && !at_end() && g_recs[g_ri].kind != K_RACE_END) part_input(K_RACE_END, g_recs[g_ri].kind);
        size_t j = g_ri;
        while (j < g_recs.size() && g_recs[j].kind != K_RACE_END && g_recs[j].kind != K_END) j++;
        unsigned long unused0 = g_race_events_unused;
        if (j < g_recs.size() && g_recs[j].kind == K_RACE_END) {
            pass_to(j, true);
            RaceMark r;
            memcpy(&r, payload(g_recs[j]), sizeof r);
            g_used[j] = 1;
            g_ri = j + 1;
            logf("session: race %u ended at frame %u (the recording's frame %u)%s", g_races, g_frame, r.frame,
                 stepped ? ", in lockstep" : "; comparing frames again from the next one, numbered as the recording's");
            if (g_race_events_unused > unused0)
                logf("session: race %u: %lu of the recording's input events in the race were never reached", g_races,
                     g_race_events_unused - unused0);
            g_frame = r.frame;
        } else {
            logf("session: race %u ended at frame %u, but the recording has no end for it", g_races, g_frame);
        }
        if (g_race_tolerant) g_tolerant = g_tolerant_before_race;    // (a part inside a lockstep race stands)
        g_race_tolerant = false;
    }
}

// the world reading the physics' packet (main thread, once a frame): a sync point, one update
uint8_t* __cdecl h_PhysicsGetStatePacket(void* pkt, void* ev) {
    if (on_main() && g_lockstep_race) {
        if (g_session_mode == SESSION_RECORD) {
            const uint8_t n = sync_record(false);
            if (g_lockstep_race) put(K_GRANT, &n, 1);
        } else if (g_session_mode == SESSION_PLAY && !play_lost_here()) {
            uint8_t n = 1;
            if (take(K_GRANT, &n, 1)) sync_play(n);
            else lose("the recording has no sync point here");
        }
    }
    return o_PhysicsGetStatePacket(pkt, ev);
}

// ---- the user directory ----------------------------------------------------------------------------------------------------------
typedef uint8_t(__cdecl* U8_t)(void);
U8_t o_get_user_directory;
#ifdef SESSION_TEST
char* g_user_dir_ptr;                            // the harness's
#define USER_DIR g_user_dir_ptr
#else
#define USER_DIR ((char*)0x00507f48)             // [0x104]; the game takes at most 200 characters
#endif

uint8_t __cdecl h_get_user_directory() {
    const uint8_t r = o_get_user_directory();
    if (g_session_mode == SESSION_OFF || !on_main()) return r;
#ifndef SESSION_TEST
    // (KernelBegin has run ProfBegin by now: on a one-processor machine PTimeNow reads rdtsc, which isn't recorded)
    if (*(volatile uint8_t*)0x005081a8)
        logf("session: this machine has one processor, so the game's clock (PTimeNow) is rdtsc, which a session doesn't"
             " record -- a replay will part wherever the menus look at the time");
#endif
    char ud[MAX_PATH];
    strncpy(ud, USER_DIR, sizeof ud - 1);
    ud[sizeof ud - 1] = 0;
    size_t n = strlen(ud);
    if (n && (ud[n - 1] == '\\' || ud[n - 1] == '/')) ud[n - 1] = 0;
    char snap[MAX_PATH];
    path_in(snap, sizeof snap, g_dir, "config");
    if (g_session_mode == SESSION_RECORD) {
        TreeCount c = {0, 0, 0, 0};
        copy_tree(ud, snap, c, 0);
        logf("session: the user directory %s snapshotted into %s: %u files, %llu KB%s", USER_DIR, snap, c.files,
             c.bytes / 1024, c.failed || c.skipped ? " (some couldn't be copied: see below)" : "");
        if (c.failed || c.skipped) logf("session: %u files couldn't be copied, %u left out (links, too deep)", c.failed, c.skipped);
    } else if (g_session_mode == SESSION_PLAY) {
        char copy[MAX_PATH];
        path_in(copy, sizeof copy, g_run_dir, "config");
        TreeCount c = {0, 0, 0, 0};
        CreateDirectoryA(g_run_dir, 0);
        copy_tree(snap, copy, c, 0);
        char with[MAX_PATH];
        _snprintf(with, sizeof with, "%s\\", copy);
        with[sizeof with - 1] = 0;
        if (strlen(with) < 0xc8 && (c.files || is_dir(copy))) {
            strcpy(USER_DIR, with);
            g_redirect_ok = true;
            logf("session: the user directory is %s, a copy of the recording's (%u files, %llu KB) -- %s isn't touched",
                 with, c.files, c.bytes / 1024, ud);
        } else {
            logf("session: can't point the user directory at %s -- the replay stops feeding (the game now reads %s)",
                 with, USER_DIR);
            end_of_recording("the user directory couldn't be copied");
        }
    }
    return r;
}

// ---- the frame hash ---------------------------------------------------------------------------------------------------------------
struct Acc {
    TraceFrame f;
    uint64_t pending;
    std::vector<uint32_t> ev;
};
Acc g_acc;

void acc_reset() {
    memset(&g_acc.f, 0, sizeof g_acc.f);
    g_acc.pending = 0;
    g_acc.ev.clear();
    for (int i = 0; i < SP_N; i++) g_acc.f.part[i] = 1469598103934665603ull;
    for (int i = 0; i < 16; i++) g_acc.f.tile[i] = 1469598103934665603ull;
    if (g_in_race) g_acc.f.flags |= g_lockstep_race ? F_LOCKSTEP : F_RACE;
}

void frame_race_flag() { g_acc.f.flags |= g_lockstep_race ? F_LOCKSTEP : F_RACE; }

bool gfx_here() { return g_session_frames && g_session_mode != SESSION_OFF && on_main(); }   // (not a rewrite pass: never drawn)

void add_event(uint8_t kind, uint64_t h) {
    h = mix(h, g_acc.pending);
    g_acc.pending = 0;
    const uint8_t part = part_of(kind);
    g_acc.f.part[part] = mix(g_acc.f.part[part], h);
    g_acc.f.count[part]++;
    if (g_acc.f.nevents < MAX_EVENTS) g_acc.ev.push_back((uint32_t)kind << 28 | (uint32_t)(h & 0x0fffffff));
    else g_acc.f.flags |= F_CUT;
    g_acc.f.nevents++;
}

// the reference frame (play) as a record, and its event hashes
const TraceFrame* ref_frame(uint32_t frame, const uint32_t** ev) {
    if (frame >= g_ref_at.size() || g_ref_at[frame] == SIZE_MAX) return 0;
    const TraceFrame* r = (const TraceFrame*)(g_ref.data() + g_ref_at[frame]);
    *ev = (const uint32_t*)(r + 1);
    return r;
}

void describe_tiles(const TraceFrame& a, const TraceFrame& b, char* out, size_t n) {
    size_t w = 0;
    out[0] = 0;
    int pw = a.page_w ? a.page_w : 640, ph = a.page_h ? a.page_h : 480;
    for (int i = 0; i < 16 && w + 48 < n; i++)
        if (a.tile[i] != b.tile[i])
            w += _snprintf(out + w, n - w, "%sx %d-%d y %d-%d", w ? ", " : "", (i % 4) * pw / 4, (i % 4 + 1) * pw / 4 - 1,
                           (i / 4) * ph / 4, (i / 4 + 1) * ph / 4 - 1);
}

// what differs between the recording's frame r and this one: parts, tiles, the first differing event
void describe(const TraceFrame& r, const uint32_t* rev, const TraceFrame& f, const uint32_t* fev, char* out, size_t n) {
    size_t w = 0;
    out[0] = 0;
    for (int p = 0; p < SP_N && w + 80 < n; p++)
        if (r.part[p] != f.part[p] || r.count[p] != f.count[p]) {
            w += _snprintf(out + w, n - w, "%s%s", w ? "; " : "", PART_NAME[p]);
            if (r.count[p] != f.count[p])
                w += _snprintf(out + w, n - w, " (%u here, %u in the recording)", f.count[p], r.count[p]);
            if (p == SP_PAGE && r.count[p] == f.count[p]) {
                char t[200];
                describe_tiles(r, f, t, sizeof t);
                if (t[0]) w += _snprintf(out + w, n - w, " at %s", t);
            }
        }
    const uint32_t m = r.nstored < f.nstored ? r.nstored : f.nstored;
    uint32_t e = 0;
    while (e < m && rev[e] == fev[e]) e++;
    if (w + 120 < n && (e < m || r.nevents != f.nevents)) {
        if (e < m) {
            uint32_t k = fev[e] >> 28, ord = 0, of = 0;
            for (uint32_t i = 0; i < f.nstored; i++)
                if ((fev[i] >> 28) == k) { of++; if (i <= e) ord++; }
            w += _snprintf(out + w, n - w, "; first at event %u of %u: %s %u of %u%s", e + 1, f.nevents, event_name(k), ord, of,
                           (rev[e] >> 28) != k ? " (the recording has another kind of event there)" : "");
        } else {
            w += _snprintf(out + w, n - w, "; the frame has %u events, the recording's %u", f.nevents, r.nevents);
        }
    }
    if (!out[0]) _snprintf(out, n, "?");
    out[n - 1] = 0;
}

void compare_frame() {
    const uint32_t* rev = 0;
    const TraceFrame* r = ref_frame(g_frame, &rev);
    if (!r) return;
    if ((r->flags | g_acc.f.flags) & F_RACE) { g_race_frames++; return; }
    g_compared++;
    if (g_acc.f.flags & F_LOCKSTEP) g_race_compared++;
    const TraceFrame& f = g_acc.f;
    bool same = r->nevents == f.nevents && !memcmp(r->part, f.part, sizeof f.part) && !memcmp(r->count, f.count, sizeof f.count) &&
                !memcmp(r->tile, f.tile, sizeof f.tile);
    if (same && r->nstored == f.nstored && f.nstored && memcmp(rev, g_acc.ev.data(), f.nstored * 4)) same = false;
    if (same) return;
    g_differ++;
    if (g_differ > 6) return;
    char what[600];
    describe(*r, rev, f, g_acc.ev.data(), what, sizeof what);
    if (g_first_part == UINT32_MAX) {
        g_first_part = g_frame;
        _snprintf(g_first_part_what, sizeof g_first_part_what, "%s", what);
        g_first_part_what[sizeof g_first_part_what - 1] = 0;
        logf("session: PARTED from the recording at frame %u (%.3f s in): %s", g_frame, game_ms() / 1000.0, what);
    } else if (g_differ <= 5) {
        logf("session: frame %u differs too: %s", g_frame, what);
    } else {
        logf("session: ... (the rest of the differing frames are counted, not logged)");
    }
}

void first_present() {
    g_first_present_done = true;
#ifndef SESSION_TEST
    SDL_Window* w = platform_window();
    int ww = 0, wh = 0;
    if (w) SDL_GL_GetDrawableSize(w, &ww, &wh);
    g_win_w = ww, g_win_h = wh;
    if (g_session_mode == SESSION_PLAY) {
        SDL_GL_SetSwapInterval(0);                                    // as fast as the fed clock allows
        int rec[2] = {0, 0};
        for (const Rec& r : g_recs)
            if (r.kind == K_INFO && r.len == sizeof rec) { memcpy(rec, payload(r), sizeof rec); break; }
        if (rec[0] && (rec[0] != ww || rec[1] != wh))
            logf("session: the window is %dx%d, the recording's was %dx%d -- the 2D page holds the 3D read back at the window's"
                 " size, so frames with 3D under 2D will differ: replay at the recording's size", ww, wh, rec[0], rec[1]);
    }
#endif
    if (g_session_mode == SESSION_RECORD) {
        int rec[2] = {g_win_w, g_win_h};
        put(K_INFO, rec, sizeof rec);
    }
}

}  // namespace

// ---- the frame hash, from the renderer (gl_core.cpp) ---------------------------------------------------------------------------
uint64_t session_hash(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = (const uint8_t*)p;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h ^ (h >> 31);
}

void session_gfx(uint8_t kind, const void* a, size_t na, const void* b, size_t nb, const void* c, size_t nc) {
    if (!gfx_here()) return;
    uint64_t h = mix(1469598103934665603ull, kind);
    if (na) h = session_hash(a, na, h);
    if (nb) h = session_hash(b, nb, h);
    if (nc) h = session_hash(c, nc, h);
    add_event(kind, h);
}

void session_gfx_state(uint32_t what, const void* p, size_t n) {
    if (!gfx_here()) return;
    uint64_t h = session_hash(p, n, mix(1469598103934665603ull, what));
    g_acc.f.part[SP_STATE] = mix(g_acc.f.part[SP_STATE], h);
    g_acc.f.count[SP_STATE]++;
    g_acc.pending = mix(g_acc.pending, h);
}

void session_gfx_page(const uint16_t* page, int w, int h) {
    if (!gfx_here() || w <= 0 || h <= 0) return;
    uint64_t t[16];
    for (int i = 0; i < 16; i++) t[i] = 1469598103934665603ull;
    for (int y = 0; y < h; y++) {
        const uint16_t* row = page + (size_t)y * w;
        const int ty = y * 4 / h;
        for (int c = 0; c < 4; c++) {
            const int x0 = c * w / 4, x1 = (c + 1) * w / 4;
            t[ty * 4 + c] = session_hash(row + x0, (size_t)(x1 - x0) * 2, t[ty * 4 + c]);
        }
    }
    uint64_t e = mix(1469598103934665603ull, SG_PAGE);
    e = mix(e, (uint64_t)w << 32 | (uint32_t)h);
    for (int i = 0; i < 16; i++) {
        g_acc.f.tile[i] = mix(g_acc.f.tile[i], t[i]);
        e = mix(e, t[i]);
    }
    g_acc.f.page_w = (uint16_t)w, g_acc.f.page_h = (uint16_t)h;
    add_event(SG_PAGE, e);
}

// Flip: a frame ends
void session_frame() {
    if (g_session_mode == SESSION_OFF || !on_main()) return;
    if (!g_first_present_done) first_present();
    const int mode = g_session_mode;
    if (mode == SESSION_RECORD) {
        put(K_FRAME, &g_frame, 4);
    } else if (mode == SESSION_PLAY) {
        settle();
        if (!g_tolerant) {
            if (at_end()) end_of_recording("the recording ended");
            else if (g_recs[g_ri].kind == K_FRAME) g_used[g_ri++] = 1;
            else part_input(K_FRAME, g_recs[g_ri].kind);
        } else {
            for (size_t j = g_ri; j < g_recs.size() && !is_boundary(g_recs[j].kind); j++)
                if (!g_used[j] && g_recs[j].kind == K_FRAME) { g_used[j] = 1; break; }
        }
    }
    if (g_session_frames) {
        TraceFrame& f = g_acc.f;
        f.frame = g_frame;
        f.nstored = (uint32_t)g_acc.ev.size();
        f.ms = game_ms();
        f.input_at = mode == SESSION_RECORD ? (uint32_t)g_records : (uint32_t)g_ri;
        if (g_session_mode == SESSION_ENDED) f.flags |= F_PLAYER;
        if (g_tolerant) f.flags |= F_TOLERANT;
        if (g_trace) {
            fwrite(&f, sizeof f, 1, g_trace);
            if (f.nstored) fwrite(g_acc.ev.data(), 4, f.nstored, g_trace);
        }
        if (mode == SESSION_PLAY) compare_frame();
    }
    if (mode == SESSION_RECORD && g_stream) fflush(g_stream);
    if (g_trace && (g_frame % 30) == 0) fflush(g_trace);
    g_frame++;
    acc_reset();
}

// ---- the lockstep's gate: the physics' side (replay.cpp's PhysTaskUpdate hook, the BG thread) -----------------------------------------
// In a race in lockstep an update of a run state (3, 6) waits here to be let run; if the main thread moves the task on
// meanwhile (restart, post-race, end) it's dropped -- as if the timer had come a moment later -- in both runs alike.
// (The post-race's own update, in state 5, isn't gated: the main thread waits for it in PhysicsPostRace.) Gating the
// update rather than physics_thread's entry closes the window where the main thread sets state 3 (PhysicsStart)
// between a check and the task's own switch.
bool session_update_gate(bool* granted) {
    *granted = false;
    if (!g_gate_on || !gated_state(PHYS_STATE)) return true;
    g_arrived = GetTickCount();
    InterlockedExchange(&g_at_gate, 1);
    SetEvent(g_ev_arrive);
    const DWORD limit = g_session_mode == SESSION_RECORD ? 3000 : 30000;
    bool run = true;
    for (;;) {
        const LONG g = g_grants;
        if (g > 0 && InterlockedCompareExchange(&g_grants, g - 1, g) == g) { *granted = true; break; }
        if (!g_gate_on) break;                                            // dropped: it runs free
        if (!gated_state(PHYS_STATE)) { run = false; break; }             // the main thread moved the task on
        if (GetTickCount() - g_arrived > limit) {
            InterlockedExchange(&g_gate_lost, 1);
            InterlockedExchange(&g_gate_on, 0);
            break;
        }
        WaitForSingleObject(g_ev_grant, 2);
    }
    InterlockedExchange(&g_at_gate, 0);
    return run;
}
void session_update_done() {
    InterlockedIncrement(&g_done);
    SetEvent(g_ev_done);
}

// ---- reads from the platform layer (platform.cpp) ---------------------------------------------------------------------------------
bool session_feed(uint8_t kind, void* v, size_t n) {
    if (g_session_mode != SESSION_PLAY || !on_main()) return false;
    return take(kind, v, n);
}

void session_saw(uint8_t kind, const void* v, size_t n) {
    if (g_session_mode == SESSION_RECORD && on_main()) put(kind, v, n);
}

void session_idle_begin() {
    if (g_session_mode != SESSION_RECORD || !on_main() || g_idle_open) return;   // (nested: the outer call's group)
    g_idle.clear();
    g_idle.push_back(sync_record(true));                              // (lockstep: the updates let run here)
    g_idle_open = true;
}

void session_idle_end() {
    if (!g_idle_open) return;
    g_idle_open = false;
    put(K_IDLE, g_idle.data(), g_idle.size());
    g_idles++;
}

void session_op(uint8_t op, const void* p, uint8_t n) {
    if (!g_idle_open || !on_main()) return;
    g_idle.push_back(op);
    g_idle.push_back(n);
    if (n) g_idle.insert(g_idle.end(), (const uint8_t*)p, (const uint8_t*)p + n);
    if (op != SOP_SCAN) g_events++;
    if (op == SOP_QUIT) {                                             // ExitProcess follows: the group goes now
        session_idle_end();
        if (g_stream) fflush(g_stream);
    }
}

void session_active(bool active) {
    if (g_session_mode != SESSION_RECORD || !on_main()) return;
    uint8_t a = active ? 1 : 0;
    if (g_idle_open) session_op(SOP_ACTIVE, &a, 1);
    else put(K_ACTIVE, &a, 1);
}

bool session_play_idle() {
    if (g_session_mode != SESSION_PLAY || !on_main()) return false;
    play_lost_here();
    if (!g_tolerant) {
        settle();
        if (at_end()) { end_of_recording("the recording ended"); return false; }
        const Rec& r = g_recs[g_ri];
        if (r.kind == K_IDLE) {
            g_used[g_ri++] = 1;
            if (r.len >= IDLE_HEAD) sync_play(payload(r)[0]);         // the physics updates let run here
            apply_idle(r);
            return true;
        }
        part_input(K_IDLE, r.kind);
        if (g_lockstep_race) lose("the replay's input parted from the recording's");
    }
    return play_idle_tolerant() && g_session_mode == SESSION_PLAY;
}

// ---- the race recorder (replay.cpp) --------------------------------------------------------------------------------------------------
uint32_t session_seed(uint32_t chosen) {
    if (g_session_mode == SESSION_OFF || !on_main()) return chosen;
    uint32_t s = chosen;
    if (take(K_SEED, &s, 4)) return s;
    save_value(K_SEED, &chosen, 4);
    return chosen;
}

bool session_random_feed(int range, int* v) {
    if (g_session_mode != SESSION_PLAY) return false;
    if (!on_main()) {
        if (!g_in_race) g_other_thread_draws++;                       // (none expected: the physics' are the race's)
        return false;
    }
    int rec[2];
    if (!g_tolerant) {
        settle();
        if (!at_end() && g_recs[g_ri].kind == K_RANDOM && g_recs[g_ri].len == sizeof rec) {
            memcpy(rec, payload(g_recs[g_ri]), sizeof rec);
            if (rec[0] != range) {
                char more[96];
                _snprintf(more, sizeof more, "the game drew a random number below %d where the recording drew one below %d",
                          range, rec[0]);
                part_input(K_RANDOM, K_RANDOM, more);
            }
        }
    }
    if (!take(K_RANDOM, rec, sizeof rec)) return false;
    *v = rec[1];
    return true;
}

void session_random_saw(int range, int v) {
    if (g_session_mode != SESSION_RECORD) return;
    if (!on_main()) {
        if (!g_in_race) g_other_thread_draws++;
        return;
    }
    int rec[2] = {range, v};
    put(K_RANDOM, rec, sizeof rec);
}

bool session_race_file(char* dir, size_t ndir, char* name, size_t nname, bool* play, char* label, size_t nlabel) {
    const int mode = g_session_mode;
    if (mode != SESSION_RECORD && mode != SESSION_PLAY) return false;
    const LONG k = InterlockedIncrement(&g_race_files);
    _snprintf(dir, ndir, "%s", g_dir);
    dir[ndir - 1] = 0;
    _snprintf(name, nname, "race-%ld", k);
    name[nname - 1] = 0;
    *play = mode == SESSION_PLAY;
    _snprintf(label, nlabel, "%s", g_run);
    label[nlabel - 1] = 0;
    return true;
}

// ---- install and report ----------------------------------------------------------------------------------------------------------
namespace {

void make_name(char* out, size_t n) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snprintf(out, n, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    out[n - 1] = 0;
}

bool load_reference(const char* path) {
    if (!load_file(path, g_ref) || g_ref.size() < sizeof(TraceHeader) || memcmp(g_ref.data(), "VPSF", 4)) return false;
    size_t at = sizeof(TraceHeader);
    while (at + sizeof(TraceFrame) <= g_ref.size()) {
        const TraceFrame* f = (const TraceFrame*)(g_ref.data() + at);
        size_t next = at + sizeof(TraceFrame) + (size_t)f->nstored * 4;
        if (next > g_ref.size()) break;
        if (f->frame >= g_ref_at.size()) g_ref_at.resize(f->frame + 1, SIZE_MAX);
        g_ref_at[f->frame] = at;
        at = next;
    }
    return true;
}

bool hook_all() {
    g_ev_grant = CreateEventA(0, FALSE, FALSE, 0);
    g_ev_arrive = CreateEventA(0, FALSE, FALSE, 0);
    g_ev_done = CreateEventA(0, FALSE, FALSE, 0);
#ifndef SESSION_TEST
    struct D { uint32_t v10; void* to; void** orig; const char* what; } d[] = {
        // detour(0x00412430) detour(0x0042ba30) detour(0x0042bae0) detour(0x0042bd70) -- rewrites, so prologues.inc
        // has them (the gate itself is in the race recorder's PhysTaskUpdate hook: session_update_gate)
        {0x00412430, (void*)h_get_user_directory, (void**)&o_get_user_directory, "get_user_directory"},
        {0x0042ba30, (void*)h_PhysicsStart, (void**)&o_PhysicsStart, "PhysicsStart"},
        {0x0042bae0, (void*)h_PhysicsStop, (void**)&o_PhysicsStop, "PhysicsStop"},
        {0x0042bd70, (void*)h_PhysicsGetStatePacket, (void**)&o_PhysicsGetStatePacket, "PhysicsGetStatePacket (a sync point)"},
    };
    for (D& h : d) {
        *h.orig = detour_front(h.v10, h.to, h.what);
        if (!*h.orig) return false;
    }
    o_QPC = (Qpc_t)patch_slot(SLOT_QPC, (void*)h_QPC);
    o_QPF = (Qpc_t)patch_slot(SLOT_QPF, (void*)h_QPF);
    o_timeGetTime = (Tgt_t)patch_slot(SLOT_TGT, (void*)h_timeGetTime);
    o_GetLocalTime = (LocalTime_t)patch_slot(SLOT_LOCALTIME, (void*)h_GetLocalTime);
    return o_QPC && o_QPF && o_timeGetTime && o_GetLocalTime;
#else
    return true;
#endif
}

}  // namespace

void session_install(const char* ini) {
    const bool record = GetPrivateProfileIntA("session", "record", 0, ini) != 0;
    char play[128], label[64];
    GetPrivateProfileStringA("session", "play", "", play, sizeof play, ini);
    GetPrivateProfileStringA("session", "label", "", label, sizeof label, ini);
    if (!record && !play[0]) return;
    g_main = GetCurrentThreadId();
#ifndef SESSION_TEST
    if (!build_is_v10()) { logf("session: NOT %s -- sessions need v1.0 race.exe", play[0] ? "replaying" : "recording"); return; }
    if (!GetPrivateProfileIntA("platform", "sdl", 0, ini)) {
        logf("session: NOT %s -- sessions need the SDL platform layer ([platform] sdl=1): it's where the input is recorded",
             play[0] ? "replaying" : "recording");
        return;
    }
    g_session_frames = platform_plans_gl(ini);
#else
    g_session_frames = true;
#endif
    char base[MAX_PATH];
    strcpy(base, ini);
    char* slash = strrchr(base, '\\');
    strcpy(slash ? slash + 1 : base, "sessions");
    CreateDirectoryA(base, 0);
    char path[MAX_PATH];
    if (play[0]) {
        if (record) logf("session: both record and play are set -- replaying %s, not recording", play);
        _snprintf(g_name, sizeof g_name, "%s", play);
        path_in(g_dir, sizeof g_dir, base, g_name);
        path_in(path, sizeof path, g_dir, "session.vps");
        if (!load_file(path, g_in) || !index_stream()) { logf("session: NOT replaying -- can't read %s", path); return; }
        if (((const StreamHeader*)g_in.data())->version != 2) {
            logf("session: NOT replaying -- %s was recorded by another version of the recorder: record it again", path);
            return;
        }
        char snap[MAX_PATH];
        path_in(snap, sizeof snap, g_dir, "config");
        if (!is_dir(snap)) {
            logf("session: NOT replaying -- %s has no snapshot of the user directory (the recording stopped before the game"
                 " named it), and a replay never runs on the player's own", g_dir);
            return;
        }
        if (!label[0]) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            _snprintf(label, sizeof label, "run-%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
        }
        // this run's name: the label, made unique (a replay never reuses another's user directory or trace)
        for (int i = 1; i < 1000; i++) {
            if (i == 1) _snprintf(g_run, sizeof g_run, "%s", label);
            else _snprintf(g_run, sizeof g_run, "%s-%d", label, i);
            path_in(g_run_dir, sizeof g_run_dir, g_dir, g_run);
            char tr[MAX_PATH];
            _snprintf(tr, sizeof tr, "%s.trace", g_run_dir);
            if (!is_dir(g_run_dir) && GetFileAttributesA(tr) == INVALID_FILE_ATTRIBUTES) break;
        }
        if (strlen(g_run_dir) + 9 >= 0xc8) {
            logf("session: NOT replaying -- the replay's user directory %s\\config\\ would be %u characters (the game takes"
                 " 199): move the game to a shorter path or use a shorter label", g_run_dir, (unsigned)strlen(g_run_dir) + 8);
            return;
        }
        path_in(path, sizeof path, g_dir, "record.trace");
        if (!g_session_frames) logf("session: frames aren't compared -- they're hashed on the OpenGL renderer ([platform] renderer=gl)");
        else if (!load_reference(path)) logf("session: %s can't be read -- frames aren't compared", path);
        if (!hook_all()) { logf("session: NOT replaying -- its hooks need v1.0 race.exe"); return; }
        _snprintf(path, sizeof path, "%s.trace", g_run_dir);
        path[sizeof path - 1] = 0;
        if (g_session_frames) g_trace = fopen(path, "wb");
        g_session_mode = SESSION_PLAY;
        logf("session: replaying %s (%u input records, %u frames) as %s -> %s -- real input is ignored while it plays", g_dir,
             (unsigned)g_recs.size(), (unsigned)g_ref_at.size(), g_run, g_trace ? path : "(no trace)");
    } else {
        make_name(g_name, sizeof g_name);
        path_in(g_dir, sizeof g_dir, base, g_name);
        for (int i = 2; is_dir(g_dir) && i < 100; i++) {             // (two starts in one second)
            char n2[140];
            _snprintf(n2, sizeof n2, "%s-%d", g_name, i);
            n2[sizeof n2 - 1] = 0;
            path_in(g_dir, sizeof g_dir, base, n2);
        }
        CreateDirectoryA(g_dir, 0);
        const char* nm = strrchr(g_dir, '\\');
        _snprintf(g_name, sizeof g_name, "%s", nm ? nm + 1 : g_dir);
        if (!hook_all()) { logf("session: NOT recording -- its hooks need v1.0 race.exe"); return; }
        path_in(path, sizeof path, g_dir, "session.vps");
        g_stream = fopen(path, "wb");
        if (!g_stream) { logf("session: NOT recording -- can't create %s", path); return; }
        setvbuf(g_stream, 0, _IOFBF, 1 << 16);
        StreamHeader h = {{'V', 'P', 'S', 'S'}, 2, 0, 0};
#ifndef SESSION_TEST
        uint8_t* exe = (uint8_t*)GetModuleHandleA(0);
        h.build = ((IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew))->FileHeader.TimeDateStamp;
#endif
        fwrite(&h, sizeof h, 1, g_stream);
        path_in(path, sizeof path, g_dir, "record.trace");
        if (g_session_frames) g_trace = fopen(path, "wb");
        g_session_mode = SESSION_RECORD;
        logf("session: recording this run to %s (the input it reads, %s; the user directory is copied when the game names it)",
             g_dir, g_session_frames ? "and a hash of every frame" : "but no frame hashes: they're taken on the OpenGL renderer"
             " ([platform] renderer=gl)");
    }
    if (g_trace) {
        setvbuf(g_trace, 0, _IOFBF, 1 << 16);
        TraceHeader th = {{'V', 'P', 'S', 'F'}, 1, {0, 0}};
        fwrite(&th, sizeof th, 1, g_trace);
    }
    acc_reset();
}

void session_report() {
    const int mode = g_session_mode;
    if (mode == SESSION_OFF) return;
    if (mode == SESSION_RECORD) {
        if (g_idle_open) session_idle_end();
        put(K_END, 0, 0);
        if (g_stream) fclose(g_stream), g_stream = 0;
        logf("exit: session: recorded %s: %u frames, %lu input records (%lu Win32Idle calls, %lu input events), %u races",
             g_name, g_frame, g_records, g_idles, g_events, g_races);
    } else {
        const bool fed_all = mode == SESSION_ENDED || at_end();
        if (g_first_part == UINT32_MAX)
            logf("exit: session: %s: IDENTICAL over all %u frames compared%s (%u of them in races; %u in races out of lockstep"
                 " not compared)%s", g_run, g_compared, g_session_frames ? "" : " -- none were: no frame hashes",
                 g_race_compared, g_race_frames, fed_all ? "" : "; the game closed before the recording ended");
        else
            logf("exit: session: %s: parted at frame %u (%s); %u of %u frames compared differ", g_run, g_first_part,
                 g_first_part_what, g_differ, g_compared);
        logf("exit: session: fed %lu Win32Idle calls (%lu input events); %lu reads fell back on the last value or the live one, "
             "%lu recorded reads were never taken%s", g_idles, g_events, g_fallbacks, g_left_over,
             g_redirect_ok ? "" : "; the user directory was NOT redirected");
        for (int k = 0; k < 256; k++)
            if (g_fb_kind[k] || g_lo_kind[k]) {
                char a[64] = "", b[64] = "";
                if (g_fb_kind[k]) _snprintf(a, sizeof a, "%lu fell back (the first at frame %u)", g_fb_kind[k], g_fb_first[k]);
                if (g_lo_kind[k]) _snprintf(b, sizeof b, "%lu never taken (the first at frame %u)", g_lo_kind[k], g_lo_first[k]);
                logf("exit: session:   %s: %s%s%s", kind_name((uint8_t)k), a, a[0] && b[0] ? ", " : "", b);
            }
    }
    if (g_races)
        logf("exit: session: %u races, %lu of them in lockstep (%lu dropped it): %lu physics updates let run at %lu sync points",
             g_races, g_races_lockstep, g_races_lost, g_grants_given, g_sync_points);
    if (g_other_thread_draws)
        logf("exit: session: %lu random numbers were drawn outside the main thread and a race (not recorded)", g_other_thread_draws);
    if (g_trace) fclose(g_trace), g_trace = 0;
    g_session_mode = SESSION_OFF;
}
