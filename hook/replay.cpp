// replay.cpp -- M3 3.1: record a race's inputs, replay them, and trace the physics state every tick.
//
// The physics runs on the game's timer thread (physics_thread -> PhysTaskUpdate), at a fixed 0.016 s a
// tick, in x87 single precision. Given the same inputs it is a pure function, and the inputs are few:
//   * how many ticks each PhysTaskUpdate runs (TimerConditioner::GetTicks: real time, 0-4 a call), and
//     whether the physics is paused (physics_paused, set by the menu);
//   * every random number the physics thread draws (Random(int), the one caller of ranmar);
//   * the player's controls, which the physics reads only through the 13 DriverGet* functions (PlayCar::
//     Update, each tick; DriverUpdate fills them on the main thread from the keyboard and pads);
//   * and, from before the race, the AI drivers: their personalities (grip, braking, aggression...) are
//     rolled once at start-up (app_begin -> AIDriverBegin -> each driver's init, random_real), from the
//     seed Randomize takes from the clock. The recorder chooses that seed and stores it in every
//     recording; a replay session seeds with it, so the same drivers are rolled. (Replaying therefore
//     needs a fresh start of the game, which it has anyway: the ini is read at start-up.)
// The recorder writes those, in the order the physics thread consumes them, to replays\<time>.vpr. The
// replayer feeds them back in the same order, so the race runs again with nobody driving: the player's
// car does what it did, and the AI and physics are computed afresh.
//
// Both write a trace: after every tick, a hash of every physics object's state (cars, checkpoints,
// obstacles), so two runs can be compared tick by tick. A replay compares itself live against the
// recording's trace and logs the first tick where they part; tools/trace_diff.py compares any two.
// dump_ticks writes whole objects at chosen ticks, which trace_diff.py names field by field.
//
//   [replay]
//   record=1                   ; record every race (and its trace) to replays\ beside the DLL
//   play=20260926-153012       ; instead, replay this recording in the next race (just that one)
//   label=original             ; this run's trace: replays\<play>.<label>.trace
//   dump_ticks=1200,1201       ; also write whole objects at these ticks to <trace>.dump (tick 0 always)
//
// A session (session.cpp, [session] record / play) owns every race of its run: each is recorded to, or replayed from,
// the session's folder as race-<k> (restarts are races of their own), whatever [replay] says; [replay] record=1 still
// gets its copy of each in replays\. The session also keeps the start-up seed, and the main thread's random numbers.
//
// v1.0 only (the hooks go through trampolines; see port.h).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <intrin.h>
#include "viperport.h"
#include "port.h"
#include "session.h"
#ifdef VP_GCC
#define _ReturnAddress() __builtin_return_address(0)    // (mingw declares the MSVC intrinsic but has no body for it)
#endif

// ---- the game's globals ----------------------------------------------------------------------------------
static void*** const PHOBS = (void***)0x00520bb4;     // PhobRoot** phobs
static int* const NPHOBS = (int*)0x00521090;
static uint8_t* const PHYSICS_PAUSED = (uint8_t*)0x00521608;
static int* const PHYSICS_TICK = (int*)0x0052161c;

// ---- files ------------------------------------------------------------------------------------------------
static char g_dir[MAX_PATH];                          // replays\ beside the DLL
static bool g_record;
static char g_play[128], g_label[64];
static std::vector<int> g_dump_ticks;

enum Kind : uint8_t { K_UPDATE = 'U', K_TICKS = 'T', K_RANDOM = 'R', K_DRIVER = 'D', K_END = 'E' };
enum { TRACE_OBJECTS = 32 };                          // per-object hashes kept for the first 32 objects

#pragma pack(push, 1)
struct StreamHeader { char magic[4]; uint32_t version, build, seed; };   // seed: ij << 16 | kl
struct TraceHeader { char magic[4]; uint32_t version, reserved[2]; };
struct TraceTick { uint32_t tick, nobjects; uint64_t all; uint64_t object[TRACE_OBJECTS]; };
#pragma pack(pop)

// ---- state ----------------------------------------------------------------------------------------------
static DWORD g_physics_thread;
static bool g_active;                                 // a race is being recorded or replayed
static bool g_playing;                                // feeding a recording (false once it ends or desyncs)
static FILE* g_stream;                                // record: the .vpr being written
static std::vector<uint8_t> g_in;                     // play: the whole recording
static size_t g_at;
static FILE* g_trace;
static FILE* g_dump;
static std::vector<TraceTick> g_ref;                  // play: the recording's own trace
static uint32_t g_ticks, g_updates, g_first_diverged = UINT32_MAX, g_diverged_ticks;
static char g_name[MAX_PATH];                         // this race's base name (record) / the recording (play)
static uint32_t g_seed = UINT32_MAX;                  // the session's start-up seed (ij << 16 | kl)
static long g_races_recorded;
// this race's files: [replay]'s (g_dir, g_play, g_label), or a session's (session_race_file)
static char g_cur_dir[MAX_PATH], g_cur_play[128], g_cur_label[96];
static bool g_session_race;

static bool on_physics_thread() { return g_active && GetCurrentThreadId() == g_physics_thread; }

// The pause flag is an input: each update's is recorded and fed back. But the game sets it from the main thread
// (the Esc menu, the P key, the switch-away pause in platform.cpp) at any moment, even in the middle of an update,
// which then reads it two ways (whether to run, whether to count the tick) while the stream holds the one it saw
// on the way in. So in a recorded race a main-thread pause or unpause waits for the next update and is taken there,
// before it's recorded (PhysicsIsPaused tells the game at once); in a replay the recording's flag stands, and the
// game's requests are left out -- until the replay stops feeding, when the game's own last one is taken (else
// the race is left as the recording ended it, usually paused by its Esc menu, and the game shows its pause icon).
static volatile LONG g_pause_wanted = -1;             // a main-thread pause (1) or unpause (0) for the next update
static LONG g_game_wanted = -1;                       // replay: the game's own last request, left out while feeding
// replay: that request, as PhysicsIsPaused answers the main thread until the next update -- the recording's main thread
// saw its own request (g_pause_wanted) until the update took it, and the recording's flag from then on
static volatile LONG g_replay_wanted = -1;
static unsigned g_pauses_left_out;

static void take_pause_wanted() {
    LONG w = InterlockedExchange(&g_pause_wanted, -1);
    if (w >= 0) *PHYSICS_PAUSED = (uint8_t)w;
}


// ---- the stream -----------------------------------------------------------------------------------------
static void put(Kind k, const void* p, size_t n) {
    fputc(k, g_stream);
    if (n) fwrite(p, 1, n, g_stream);
}

static void stop_playing(const char* why) {
    if (!g_playing) return;
    g_playing = false;
    if (g_game_wanted >= 0) InterlockedExchange(&g_pause_wanted, g_game_wanted);   // taken at the next update
    logf("replay: stopped feeding %s at update %u, tick %u: %s -- the player drives from here", g_cur_play, g_updates, g_ticks, why);
}

// the next record, which must be of kind k (else the replay has desynced: stop feeding it)
static bool take(Kind k, void* p, size_t n) {
    if (!g_playing) return false;
    if (g_at >= g_in.size() || g_in[g_at] == K_END) { stop_playing("the recording ended"); return false; }
    if (g_in[g_at] != k) {
        char why[96];
        _snprintf(why, sizeof why, "desynced: it has '%c' where the game asked for '%c'", g_in[g_at], k);
        stop_playing(why);
        return false;
    }
    if (g_at + 1 + n > g_in.size()) { stop_playing("the recording is cut short"); return false; }
    memcpy(p, &g_in[g_at + 1], n);
    g_at += 1 + n;
    return true;
}

// ---- hashing the physics objects --------------------------------------------------------------------------
// Pointers differ between runs, so they're left out: a 4-aligned word in [0x10000, 0x30000000) is taken to
// be one (heap and module addresses; as a float it would be below 5e-10, never a physics value here).
static inline bool pointer_like(uint32_t v) { return (v & 3) == 0 && v >= 0x10000 && v < 0x30000000; }

static uint64_t hash_object(const void* obj, uint32_t* size_out) {
    uint32_t size = 0;
    class_of(obj, &size);
    if (!size) size = 108;                            // unknown class: PhobRoot's own state
    if (size_out) *size_out = size;
    uint64_t h = 1469598103934665603ull;
    const uint32_t* w = (const uint32_t*)obj;
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t v = w[i];
        if (pointer_like(v)) v = 0;
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

static void dump_objects(uint32_t tick) {
    if (!g_dump) return;
    int n = *NPHOBS;
    fwrite(&tick, 4, 1, g_dump);
    fwrite(&n, 4, 1, g_dump);
    for (int i = 0; i < n; i++) {
        void* p = (*PHOBS)[i];
        uint32_t size = 0;
        const char* cls = p ? class_of(p, &size) : 0;
        if (p && !size) size = 108;
        char name[32] = {0};
        strncpy(name, cls ? cls : (p ? "?" : ""), sizeof name - 1);
        fwrite(name, 1, sizeof name, g_dump);
        fwrite(&size, 4, 1, g_dump);
        if (size) fwrite(p, 1, size, g_dump);
    }
    fflush(g_dump);
}

static void trace_tick() {
    TraceTick t = {};
    t.tick = g_ticks;
    int n = *NPHOBS;
    t.nobjects = (uint32_t)n;
    uint64_t all = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
        void* p = (*PHOBS)[i];
        uint64_t h = p ? hash_object(p, 0) : 0;
        if (i < TRACE_OBJECTS) t.object[i] = h;
        all = (all ^ h) * 1099511628211ull;
    }
    t.all = all;
    if (g_trace) fwrite(&t, sizeof t, 1, g_trace);
    bool dump = g_ticks == 0;
    for (int d : g_dump_ticks) dump |= (uint32_t)d == g_ticks;
    if (dump) dump_objects(g_ticks);
    // a replay checks itself against the recording as it goes
    if (!g_ref.empty() && g_ticks < g_ref.size()) {
        const TraceTick& r = g_ref[g_ticks];
        if (r.all != t.all || r.nobjects != t.nobjects) {
            g_diverged_ticks++;
            if (g_first_diverged == UINT32_MAX) {
                g_first_diverged = g_ticks;
                char which[256] = "";
                size_t w = 0;
                bool checkpoint = false;                   // checkpoints never move: one that differs is another race
                for (int i = 0; i < TRACE_OBJECTS && i < n && w < sizeof which - 40; i++)
                    if (r.object[i] != t.object[i]) {
                        const char* cls = (*PHOBS)[i] ? class_of((*PHOBS)[i], 0) : 0;
                        checkpoint |= cls && !strcmp(cls, "CheckPoint");
                        w += _snprintf(which + w, sizeof which - w, "%s#%d %s", w ? ", " : "", i, cls ? cls : "?");
                    }
                logf("replay: DIVERGED from the recording at tick %u (%.3f s): %s%s%s", g_ticks, g_ticks * (double)0.016,
                     r.nobjects != t.nobjects ? "the number of objects differs; " : "",
                     which[0] ? which : "an object past the first 32",
                     g_ticks == 0 && (checkpoint || r.nobjects != t.nobjects)
                         ? " -- this isn't the race that was recorded: start the same track, direction and number of"
                           " opponents"
                         : "");
            }
        }
    }
    g_ticks++;
}

// ---- hooks ---------------------------------------------------------------------------------------------------
typedef void(__cdecl* PhysTaskBegin_t)(void*);
typedef void(__cdecl* Void_t)(void);
typedef int(__fastcall* GetTicks_t)(void*, void*);
typedef int(__cdecl* Random_t)(int);
typedef float(__cdecl* FloatGet_t)(void);
typedef float(__cdecl* Steering_t)(float);
typedef int(__cdecl* IntGet_t)(void);
typedef unsigned char(__cdecl* ByteGet_t)(void);

static PhysTaskBegin_t o_PhysTaskBegin;
static Void_t o_PhysTaskEnd, o_PhysTaskRestart, o_PhysTaskUpdate, o_update_phobs;
static GetTicks_t o_GetTicks;
static Random_t o_Random;
static Void_t o_Randomize;
typedef void(__cdecl* Rmarin_t)(int, int);
static const Rmarin_t rmarin = (Rmarin_t)0x0041b720;

static void finish(const char* why);

static void make_name() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snprintf(g_name, sizeof g_name, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (g_races_recorded) {                             // a restart within the same second
        size_t l = strlen(g_name);
        _snprintf(g_name + l, sizeof g_name - l, "-%ld", g_races_recorded + 1);
    }
}

static FILE* open_in_dir(const char* name, const char* ext, const char* mode) {
    char path[MAX_PATH];
    _snprintf(path, sizeof path, "%s\\%s%s", g_cur_dir, name, ext);
    path[sizeof path - 1] = 0;
    return fopen(path, mode);
}

static bool load_file(const char* name, const char* ext, std::vector<uint8_t>& out) {
    FILE* f = open_in_dir(name, ext, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}

static void open_trace(const char* name) {
    g_trace = open_in_dir(name, ".trace", "wb");
    TraceHeader th = {{'V', 'P', 'T', 'R'}, 1, {0, 0}};
    if (g_trace) fwrite(&th, sizeof th, 1, g_trace);
    g_dump = open_in_dir(name, ".dump", "wb");
}

static void begin_race() {
    g_physics_thread = GetCurrentThreadId();
    g_ticks = g_updates = g_diverged_ticks = 0;
    g_first_diverged = UINT32_MAX;
    // a session's race, or [replay]'s
    char sname[64];
    bool splay = false;
    g_session_race = session_race_file(g_cur_dir, sizeof g_cur_dir, sname, sizeof sname, &splay, g_cur_label, sizeof g_cur_label);
    bool record = g_record;
    if (g_session_race) {
        strcpy(g_cur_play, splay ? sname : "");
        record = !splay;
        if (record) strcpy(g_name, sname);
    } else {
        strcpy(g_cur_dir, g_dir);
        strcpy(g_cur_play, g_play);
        strcpy(g_cur_label, g_label);
    }
    if (g_cur_play[0]) {
        g_ref.clear();
        std::vector<uint8_t> ref;
        if (!load_file(g_cur_play, ".vpr", g_in) || g_in.size() < sizeof(StreamHeader) || memcmp(g_in.data(), "VPRP", 4)) {
            logf("replay: can't read %s\\%s.vpr -- nothing replayed", g_cur_dir, g_cur_play);
            return;
        }
        if (((const StreamHeader*)g_in.data())->version < 2) {
            logf("replay: %s was recorded before recordings kept the AI drivers' seed -- record it again", g_cur_play);
            return;
        }
        if (load_file(g_cur_play, ".trace", ref) && ref.size() >= sizeof(TraceHeader)) {
            size_t n = (ref.size() - sizeof(TraceHeader)) / sizeof(TraceTick);
            g_ref.resize(n);
            memcpy(g_ref.data(), ref.data() + sizeof(TraceHeader), n * sizeof(TraceTick));
        }
        g_at = sizeof(StreamHeader);
        char out[MAX_PATH];
        _snprintf(out, sizeof out, "%s.%s", g_cur_play, g_cur_label);
        open_trace(out);
        g_playing = g_active = true;
        g_game_wanted = -1;
        logf("replay: playing %s\\%s (%u bytes; its trace has %u ticks) -> %s.trace", g_cur_dir, g_cur_play,
             (unsigned)g_in.size(), (unsigned)g_ref.size(), out);
    } else if (record) {
        if (!g_session_race) make_name();
        g_stream = open_in_dir(g_name, ".vpr", "wb");
        if (!g_stream) { logf("replay: can't create %s\\%s.vpr -- not recording", g_cur_dir, g_name); return; }
        StreamHeader h = {{'V', 'P', 'R', 'P'}, 2, 0x362de68c, g_seed};
        fwrite(&h, sizeof h, 1, g_stream);
        open_trace(g_name);
        g_active = true;
        logf("replay: recording this race to %s\\%s.vpr", g_cur_dir, g_name);
    }
}

// [replay] record=1 alongside a session: the session's race recording copied into replays\ as ever
static void copy_to_replays() {
    char race[MAX_PATH];
    strcpy(race, g_name);                                // the session's name for it (make_name writes g_name)
    make_name();
    char stamp[MAX_PATH];
    strcpy(stamp, g_name);
    strcpy(g_name, race);
    for (const char* ext : {".vpr", ".trace"}) {
        char from[MAX_PATH], to[MAX_PATH];
        _snprintf(from, sizeof from, "%s\\%s%s", g_cur_dir, race, ext);
        _snprintf(to, sizeof to, "%s\\%s%s", g_dir, stamp, ext);
        from[sizeof from - 1] = to[sizeof to - 1] = 0;
        CopyFileA(from, to, TRUE);
    }
    logf("replay: the session's %s copied to %s\\%s.vpr ([replay] record=1)", race, g_dir, stamp);
}

static void finish(const char* why) {
    if (!g_active) return;
    bool recorded = false;
    if (g_stream) { put(K_END, 0, 0); fclose(g_stream); g_stream = 0; g_races_recorded++; recorded = true; }
    if (g_trace) { fclose(g_trace); g_trace = 0; }
    if (g_dump) { fclose(g_dump); g_dump = 0; }
    if (recorded && g_session_race && g_record) copy_to_replays();
    if (g_cur_play[0]) {
        bool fed_all = g_at < g_in.size() && g_in[g_at] == K_END;
        uint32_t compared = g_ticks < g_ref.size() ? g_ticks : (uint32_t)g_ref.size();
        if (g_first_diverged == UINT32_MAX)
            logf("replay: %s: %u ticks, IDENTICAL to the recording over all %u ticks compared%s", why, g_ticks,
                 compared, fed_all || !g_playing ? "" : " (the recording has more)");
        else
            logf("replay: %s: %u ticks; first diverged at tick %u, %u ticks differ", why, g_ticks, g_first_diverged,
                 g_diverged_ticks);
    } else {
        logf("replay: recorded %s: %u updates, %u ticks (%s)", g_name, g_updates, g_ticks, why);
    }
    if (g_playing && g_game_wanted >= 0) g_pause_wanted = g_game_wanted;   // a replay: the game's own, as it ends
    take_pause_wanted();                               // a pause asked for as the race ended
    g_active = g_playing = false;
    if (g_cur_play[0] && !g_session_race) {           // a replay drives one race; later races are the player's
        logf("replay: done with %s -- the next races are yours (restart the game to replay it again)", g_play);
        g_play[0] = 0;
    }
}

static void __cdecl h_PhysTaskBegin(void* stream) {
    begin_race();
    const bool clock = session_phys_clock(true);        // (a session's network race: the physics task's clock is fed)
    o_PhysTaskBegin(stream);
    session_phys_clock_restore(clock);
    if (g_active) {
        int n = *NPHOBS;
        logf("replay: the race has %d physics objects", n);
    }
}

static void __cdecl h_PhysTaskEnd(void) {
    finish("the race ended");
    const bool clock = session_phys_clock(true);
    o_PhysTaskEnd();
    session_phys_clock_restore(clock);
}

static void __cdecl h_PhysTaskRestart(void) {
    bool was = g_active, playing = g_cur_play[0] != 0, session = g_session_race;
    finish("the race was restarted");
    const bool clock = session_phys_clock(true);
    o_PhysTaskRestart();
    session_phys_clock_restore(clock);
    // recording: the restarted race is a new recording; a session's replay goes on to its next race file too
    if (session || (was && !playing)) begin_race();
}

static void __cdecl h_PhysTaskUpdate(void) {
    g_physics_thread_id = GetCurrentThreadId();          // for shadow checks (port.h), race or not
    bool granted;
    if (!session_update_gate(&granted)) return;          // a session's lockstep: the update waits for the main thread
    if (on_physics_thread()) {
        take_pause_wanted();
        InterlockedExchange(&g_replay_wanted, -1);          // (a replay: the game's request, now the recording's flag)
        uint8_t paused = *PHYSICS_PAUSED;
        if (g_stream) put(K_UPDATE, &paused, 1);
        else if (take(K_UPDATE, &paused, 1)) *PHYSICS_PAUSED = paused;
        g_updates++;
    }
    const bool clock = session_phys_clock(true);
    o_PhysTaskUpdate();
    session_phys_clock_restore(clock);
    if (granted) session_update_done();
}

static Void_t o_PhysicsPause, o_PhysicsUnpause;
static unsigned char(__cdecl* o_PhysicsIsPaused)(void);

// true when the request is handled here (waiting for the next update, or left out of a replay)
static bool pause_request(LONG want, void* from) {
    if (!g_active || GetCurrentThreadId() == g_physics_thread) return false;
    if (g_playing) {
        g_game_wanted = want;
        InterlockedExchange(&g_replay_wanted, want);
        if (g_pauses_left_out++ < 8)
            logf("replay: the game asked to %s the physics during the replay (from %p) -- the recording's pause stands",
                 want ? "pause" : "unpause", from);
        return true;
    }
    if (!g_stream) return false;                         // a replay that stopped feeding: the player's race
    InterlockedExchange(&g_pause_wanted, want);
    return true;
}
static void __cdecl h_PhysicsPause(void) { if (!pause_request(1, _ReturnAddress())) o_PhysicsPause(); }
static void __cdecl h_PhysicsUnpause(void) { if (!pause_request(0, _ReturnAddress())) o_PhysicsUnpause(); }
static unsigned char __cdecl h_PhysicsIsPaused(void) {
    LONG w = g_pause_wanted;
    if (w < 0 && g_playing) w = g_replay_wanted;
    if (w >= 0 && GetCurrentThreadId() != g_physics_thread) return (unsigned char)w;
    return o_PhysicsIsPaused();
}

static void __cdecl h_update_phobs(void) {
    o_update_phobs();
    if (on_physics_thread()) trace_tick();
}

static int __fastcall h_GetTicks(void* self, void* edx) {
    int n = o_GetTicks(self, edx);
    if (on_physics_thread()) {
        if (g_stream) put(K_TICKS, &n, 4);
        else take(K_TICKS, &n, 4);
    }
    return n;
}

// Randomize (start-up, from RandomBegin): the original seeds rmarin with the clock mod 30000; here the
// recorder picks the seed the same way and keeps it, and a replay reuses the recording's.
static void __cdecl h_Randomize(void) {
    if (g_seed == UINT32_MAX) {
        uint32_t t = GetTickCount();
        g_seed = ((t % 30000) << 16) | ((t / 7) % 30000);
    }
    const uint32_t chosen = g_seed;
    g_seed = session_seed(g_seed);                      // a session keeps it; a session's replay takes its recording's
    rmarin((int)(g_seed >> 16), (int)(g_seed & 0xffff));
    logf("replay: random numbers seeded with %u/%u%s", g_seed >> 16, g_seed & 0xffff,
         session_playing() ? " -- the session's recording's, so the AI drivers are rolled as they were"
         : g_play[0] ? " -- the recording's, so the AI drivers are rolled as they were" : "");
    if (session_playing() && g_play[0] && chosen != g_seed)
        logf("replay: [replay] play=%s is ignored: the session's replay owns the races", g_play);
}

// Random and the controls are also a shadow check's inputs (port.h): a check's rewrite pass is fed what
// the original's pass read, and never reaches the recorder.
enum { IN_RANDOM = 1, IN_DRIVER = 2 };

static int __cdecl h_Random(int range) {
    int v;
    if (shadow_feed(IN_RANDOM, &v, 4)) return v;
    const bool physics = on_physics_thread();
    // the physics thread's in a race: the race recorder's; the main thread's: the session's
    if (!(physics && g_playing && take(K_RANDOM, &v, 4)) && !(!physics && session_random_feed(range, &v))) {
        v = o_Random(range);
        if (physics && g_stream) put(K_RANDOM, &v, 4);
        else if (!physics) session_random_saw(range, v);
    }
    shadow_saw(IN_RANDOM, &v, 4);
    return v;
}

// the 13 control readers: the original runs; a recording stores its value, a replay replaces it
template <typename T> static T drive(uint8_t id, T v) {
    if (!on_physics_thread()) return v;
    uint8_t rec[5] = {id};
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof v);
    if (g_stream) { memcpy(rec + 1, &bits, 4); put(K_DRIVER, rec, 5); return v; }
    if (take(K_DRIVER, rec, 5)) {
        if (rec[0] != id) { stop_playing("desynced: a different control was read"); return v; }
        memcpy(&bits, rec + 1, 4);
        memcpy(&v, &bits, sizeof v);
    }
    return v;
}

#define DRIVER_GET(ID, ADDR, T, NAME)                                                                       \
    static T(__cdecl* o_##NAME)(void);                                                                     \
    static T __cdecl h_##NAME(void) {                                                                      \
        T v;                                                                                               \
        if (shadow_feed(IN_DRIVER + 16 * ID, &v, sizeof v)) return v;                                      \
        v = drive<T>(ID, o_##NAME());                                                                      \
        shadow_saw(IN_DRIVER + 16 * ID, &v, sizeof v);                                                     \
        return v;                                                                                          \
    }
DRIVER_GET(1, 0x00441e60, float, DriverGetThrottle)
DRIVER_GET(2, 0x00441e70, float, DriverGetBraking)
DRIVER_GET(3, 0x00441e80, float, DriverGetClutch)
DRIVER_GET(4, 0x00441ec0, float, DriverGetEBrake)
DRIVER_GET(5, 0x00441ed0, int, DriverGetGear)
DRIVER_GET(6, 0x00441ef0, unsigned char, DriverGetHorn)
DRIVER_GET(7, 0x00441f00, unsigned char, DriverGetAirlift)
DRIVER_GET(8, 0x00441f10, unsigned char, DriverGetReverse)
DRIVER_GET(9, 0x00441f20, unsigned char, DriverGetHelp)
DRIVER_GET(10, 0x00441f30, float, DriverGetLookSide)
DRIVER_GET(11, 0x00441f40, float, DriverGetPitch)
DRIVER_GET(12, 0x00441f50, unsigned char, DriverGetLookBack)
static Steering_t o_DriverGetSteering;
static float __cdecl h_DriverGetSteering(float speed) {
    float v;
    if (shadow_feed(IN_DRIVER, &v, 4)) return v;
    v = drive<float>(0, o_DriverGetSteering(speed));
    shadow_saw(IN_DRIVER, &v, 4);
    return v;
}

// The physics clock (PhysicsGetTime: the tick count x dt) is an input to a check made on the MAIN thread -- the
// world's effects time their puffs and bursts by it -- because the physics thread can tick between the
// original's pass and the rewrite's. On the physics thread it's one of the saved globals, so it isn't fed.
enum { IN_CLOCK = 3 };
typedef double(__cdecl* Clock_t)();
static Clock_t o_PhysicsGetTime;
static double __cdecl h_PhysicsGetTime() {
    if (GetCurrentThreadId() == g_physics_thread_id) return o_PhysicsGetTime();
    double v;                                          // exact: a tick count times a float
    if (shadow_feed(IN_CLOCK, &v, 8)) return v;
    v = o_PhysicsGetTime();
    shadow_saw(IN_CLOCK, &v, 8);
    return v;
}

// ---- install and report -----------------------------------------------------------------------------------
void replay_install(const char* ini) {
    g_record = GetPrivateProfileIntA("replay", "record", 0, ini) != 0;
    GetPrivateProfileStringA("replay", "play", "", g_play, sizeof g_play, ini);
    GetPrivateProfileStringA("replay", "label", "", g_label, sizeof g_label, ini);
    char dumps[256];
    GetPrivateProfileStringA("replay", "dump_ticks", "", dumps, sizeof dumps, ini);
    for (char* s = strtok(dumps, ", "); s; s = strtok(0, ", ")) g_dump_ticks.push_back(atoi(s));
    // the input hooks (Random, DriverGet*, the clock) are also a shadow check's inputs, so they go in for either; a
    // session (session_install, before this) needs the race hooks whatever [replay] says
    const bool session = g_session_mode != SESSION_OFF;
    const bool race = g_record || g_play[0] || session;
    if (!race && !shadow_on()) return;
    if (race && !g_label[0]) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        _snprintf(g_label, sizeof g_label, "run-%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
    }
    strcpy(g_dir, ini);
    char* slash = strrchr(g_dir, '\\');
    strcpy(slash ? slash + 1 : g_dir, "replays");
    if (g_record || g_play[0]) CreateDirectoryA(g_dir, 0);
    if (g_play[0]) {                                   // the seed must be known before start-up rolls the AI
        std::vector<uint8_t> v;
        if (load_file(g_play, ".vpr", v) && v.size() >= sizeof(StreamHeader) && !memcmp(v.data(), "VPRP", 4) &&
            ((const StreamHeader*)v.data())->version >= 2)
            g_seed = ((const StreamHeader*)v.data())->seed;
    }

    struct H { uint32_t v10; void* to; void** orig; const char* what; bool race_only; } hooks[] = {
        {0x00426850, (void*)h_PhysTaskBegin, (void**)&o_PhysTaskBegin, "PhysTaskBegin", true},
        {0x00426a80, (void*)h_PhysTaskEnd, (void**)&o_PhysTaskEnd, "PhysTaskEnd", true},
        {0x00426b20, (void*)h_PhysTaskRestart, (void**)&o_PhysTaskRestart, "PhysTaskRestart", true},
        {0x00426b90, (void*)h_PhysTaskUpdate, (void**)&o_PhysTaskUpdate, "PhysTaskUpdate", false},
        {0x004274d0, (void*)h_update_phobs, (void**)&o_update_phobs, "update_phobs", true},
        {0x00428c30, (void*)h_GetTicks, (void**)&o_GetTicks, "TimerConditioner::GetTicks", true},
        {0x0041b6b0, (void*)h_Randomize, (void**)&o_Randomize, "Randomize", true},
        {0x0042bcc0, (void*)h_PhysicsPause, (void**)&o_PhysicsPause, "PhysicsPause", true},
        {0x0042bcf0, (void*)h_PhysicsUnpause, (void**)&o_PhysicsUnpause, "PhysicsUnpause", true},
        {0x0042bd20, (void*)h_PhysicsIsPaused, (void**)&o_PhysicsIsPaused, "PhysicsIsPaused", true},
        {0x0041b6e0, (void*)h_Random, (void**)&o_Random, "Random"},
        {0x0042bc80, (void*)h_PhysicsGetTime, (void**)&o_PhysicsGetTime, "PhysicsGetTime"},
        {0x00441df0, (void*)h_DriverGetSteering, (void**)&o_DriverGetSteering, "DriverGetSteering"},
        {0x00441e60, (void*)h_DriverGetThrottle, (void**)&o_DriverGetThrottle, "DriverGetThrottle"},
        {0x00441e70, (void*)h_DriverGetBraking, (void**)&o_DriverGetBraking, "DriverGetBraking"},
        {0x00441e80, (void*)h_DriverGetClutch, (void**)&o_DriverGetClutch, "DriverGetClutch"},
        {0x00441ec0, (void*)h_DriverGetEBrake, (void**)&o_DriverGetEBrake, "DriverGetEBrake"},
        {0x00441ed0, (void*)h_DriverGetGear, (void**)&o_DriverGetGear, "DriverGetGear"},
        {0x00441ef0, (void*)h_DriverGetHorn, (void**)&o_DriverGetHorn, "DriverGetHorn"},
        {0x00441f00, (void*)h_DriverGetAirlift, (void**)&o_DriverGetAirlift, "DriverGetAirlift"},
        {0x00441f10, (void*)h_DriverGetReverse, (void**)&o_DriverGetReverse, "DriverGetReverse"},
        {0x00441f20, (void*)h_DriverGetHelp, (void**)&o_DriverGetHelp, "DriverGetHelp"},
        {0x00441f30, (void*)h_DriverGetLookSide, (void**)&o_DriverGetLookSide, "DriverGetLookSide"},
        {0x00441f40, (void*)h_DriverGetPitch, (void**)&o_DriverGetPitch, "DriverGetPitch"},
        {0x00441f50, (void*)h_DriverGetLookBack, (void**)&o_DriverGetLookBack, "DriverGetLookBack"},
    };
    int ok = 0, wanted = 0;
    for (H& h : hooks) {
        if (h.race_only && !race) continue;
        wanted++;
        // detour() is the only way these are hooked; listed here so gen_port_tables.py finds them:
        // detour(0x00426850) detour(0x00426a80) detour(0x00426b20) detour(0x00426b90) detour(0x004274d0)
        // detour(0x00428c30) detour(0x0041b6e0) detour(0x00441df0) detour(0x00441e60) detour(0x00441e70)
        // detour(0x00441e80) detour(0x00441ec0) detour(0x00441ed0) detour(0x00441ef0) detour(0x00441f00)
        // detour(0x00441f10) detour(0x00441f20) detour(0x00441f30) detour(0x00441f40) detour(0x00441f50)
        // detour(0x0041b6b0) detour(0x0042bc80) detour(0x0042bcc0) detour(0x0042bcf0) detour(0x0042bd20)
        *h.orig = detour_front(h.v10, h.to, h.what);     // in front of the rewrite, where there is one
        ok += *h.orig != 0;
    }
    if (ok != wanted) {
        logf("replay: only %d of %d hooks installed -- recording, replay and shadow inputs need v1.0 race.exe", ok, wanted);
        g_record = false;
        g_play[0] = 0;
        return;
    }
    if (!race) logf("replay: input hooks in (random numbers, controls and the clock feed shadow checks)");
    else if (session) logf("replay: the session's races are recorded or replayed in its folder%s", g_record || g_play[0]
                           ? " ([replay] record=1 still copies each into replays\\; [replay] play is ignored: the session owns the races)" : "");
    else if (g_play[0]) logf("replay: will replay %s\\%s.vpr in the next race (this run's trace: %s)", g_dir, g_play, g_label);
    else logf("replay: recording every race to %s", g_dir);
}

void replay_report() {
    if (g_active) finish("the game closed");
    if (g_play[0] && g_ticks == 0 && !g_active && g_session_mode == SESSION_OFF) logf("exit: replay: no race was replayed");
}
