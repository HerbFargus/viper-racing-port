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
//   play=20260926-153012       ; instead, replay this recording in the next race
//   label=original             ; this run's trace: replays\<play>.<label>.trace
//   dump_ticks=1200,1201       ; also write whole objects at these ticks to <trace>.dump (tick 0 always)
//
// v1.0 only (the hooks go through trampolines; see port.h).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "viperport.h"
#include "port.h"

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

static bool on_physics_thread() { return g_active && GetCurrentThreadId() == g_physics_thread; }


// ---- the stream -----------------------------------------------------------------------------------------
static void put(Kind k, const void* p, size_t n) {
    fputc(k, g_stream);
    if (n) fwrite(p, 1, n, g_stream);
}

static void stop_playing(const char* why) {
    if (!g_playing) return;
    g_playing = false;
    logf("replay: stopped feeding %s at update %u, tick %u: %s -- the player drives from here", g_play, g_updates, g_ticks, why);
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
                for (int i = 0; i < TRACE_OBJECTS && i < n && w < sizeof which - 40; i++)
                    if (r.object[i] != t.object[i]) {
                        const char* cls = (*PHOBS)[i] ? class_of((*PHOBS)[i], 0) : 0;
                        w += _snprintf(which + w, sizeof which - w, "%s#%d %s", w ? ", " : "", i, cls ? cls : "?");
                    }
                logf("replay: DIVERGED from the recording at tick %u (%.3f s): %s%s", g_ticks, g_ticks * 0.016,
                     r.nobjects != t.nobjects ? "the number of objects differs; " : "",
                     which[0] ? which : "an object past the first 32");
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
    _snprintf(path, sizeof path, "%s\\%s%s", g_dir, name, ext);
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
    if (g_play[0]) {
        g_ref.clear();
        std::vector<uint8_t> ref;
        if (!load_file(g_play, ".vpr", g_in) || g_in.size() < sizeof(StreamHeader) || memcmp(g_in.data(), "VPRP", 4)) {
            logf("replay: can't read %s\\%s.vpr -- nothing replayed", g_dir, g_play);
            return;
        }
        if (((const StreamHeader*)g_in.data())->version < 2) {
            logf("replay: %s was recorded before recordings kept the AI drivers' seed -- record it again", g_play);
            return;
        }
        if (load_file(g_play, ".trace", ref) && ref.size() >= sizeof(TraceHeader)) {
            size_t n = (ref.size() - sizeof(TraceHeader)) / sizeof(TraceTick);
            g_ref.resize(n);
            memcpy(g_ref.data(), ref.data() + sizeof(TraceHeader), n * sizeof(TraceTick));
        }
        g_at = sizeof(StreamHeader);
        char out[MAX_PATH];
        _snprintf(out, sizeof out, "%s.%s", g_play, g_label);
        open_trace(out);
        g_playing = g_active = true;
        logf("replay: playing %s (%u bytes; its trace has %u ticks) -> %s.trace", g_play, (unsigned)g_in.size(),
             (unsigned)g_ref.size(), out);
    } else if (g_record) {
        make_name();
        g_stream = open_in_dir(g_name, ".vpr", "wb");
        if (!g_stream) { logf("replay: can't create %s\\%s.vpr -- not recording", g_dir, g_name); return; }
        StreamHeader h = {{'V', 'P', 'R', 'P'}, 2, 0x362de68c, g_seed};
        fwrite(&h, sizeof h, 1, g_stream);
        open_trace(g_name);
        g_active = true;
        logf("replay: recording this race to %s\\%s.vpr", g_dir, g_name);
    }
}

static void finish(const char* why) {
    if (!g_active) return;
    if (g_stream) { put(K_END, 0, 0); fclose(g_stream); g_stream = 0; g_races_recorded++; }
    if (g_trace) { fclose(g_trace); g_trace = 0; }
    if (g_dump) { fclose(g_dump); g_dump = 0; }
    if (g_play[0]) {
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
    g_active = g_playing = false;
}

static void __cdecl h_PhysTaskBegin(void* stream) {
    begin_race();
    o_PhysTaskBegin(stream);
    if (g_active) {
        int n = *NPHOBS;
        logf("replay: the race has %d physics objects", n);
    }
}

static void __cdecl h_PhysTaskEnd(void) {
    finish("the race ended");
    o_PhysTaskEnd();
}

static void __cdecl h_PhysTaskRestart(void) {
    bool was = g_active, playing = g_play[0] != 0;
    finish("the race was restarted");
    o_PhysTaskRestart();
    if (was && !playing) begin_race();                  // recording: the restarted race is a new recording
}

static void __cdecl h_PhysTaskUpdate(void) {
    if (on_physics_thread()) {
        uint8_t paused = *PHYSICS_PAUSED;
        if (g_stream) put(K_UPDATE, &paused, 1);
        else if (take(K_UPDATE, &paused, 1)) *PHYSICS_PAUSED = paused;
        g_updates++;
    }
    o_PhysTaskUpdate();
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
    rmarin((int)(g_seed >> 16), (int)(g_seed & 0xffff));
    logf("replay: random numbers seeded with %u/%u%s", g_seed >> 16, g_seed & 0xffff,
         g_play[0] ? " -- the recording's, so the AI drivers are rolled as they were" : "");
}

// Random and the controls are also a shadow check's inputs (port.h): a check's rewrite pass is fed what
// the original's pass read, and never reaches the recorder.
enum { IN_RANDOM = 1, IN_DRIVER = 2 };

static int __cdecl h_Random(int range) {
    int v;
    if (shadow_feed(IN_RANDOM, &v, 4)) return v;
    if (!(on_physics_thread() && g_playing && take(K_RANDOM, &v, 4))) {
        v = o_Random(range);
        if (on_physics_thread() && g_stream) put(K_RANDOM, &v, 4);
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

// ---- install and report -----------------------------------------------------------------------------------
void replay_install(const char* ini) {
    g_record = GetPrivateProfileIntA("replay", "record", 0, ini) != 0;
    GetPrivateProfileStringA("replay", "play", "", g_play, sizeof g_play, ini);
    GetPrivateProfileStringA("replay", "label", "", g_label, sizeof g_label, ini);
    char dumps[256];
    GetPrivateProfileStringA("replay", "dump_ticks", "", dumps, sizeof dumps, ini);
    for (char* s = strtok(dumps, ", "); s; s = strtok(0, ", ")) g_dump_ticks.push_back(atoi(s));
    // the input hooks (Random, DriverGet*) are also a shadow check's inputs, so they go in for either
    const bool race = g_record || g_play[0];
    if (!race && !shadow_on()) return;
    if (race && !g_label[0]) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        _snprintf(g_label, sizeof g_label, "run-%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
    }
    strcpy(g_dir, ini);
    char* slash = strrchr(g_dir, '\\');
    strcpy(slash ? slash + 1 : g_dir, "replays");
    CreateDirectoryA(g_dir, 0);
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
        {0x00426b90, (void*)h_PhysTaskUpdate, (void**)&o_PhysTaskUpdate, "PhysTaskUpdate", true},
        {0x004274d0, (void*)h_update_phobs, (void**)&o_update_phobs, "update_phobs", true},
        {0x00428c30, (void*)h_GetTicks, (void**)&o_GetTicks, "TimerConditioner::GetTicks", true},
        {0x0041b6b0, (void*)h_Randomize, (void**)&o_Randomize, "Randomize", true},
        {0x0041b6e0, (void*)h_Random, (void**)&o_Random, "Random"},
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
        // detour(0x0041b6b0)
        *h.orig = detour(h.v10, h.to, h.what);
        ok += *h.orig != 0;
    }
    if (ok != wanted) {
        logf("replay: only %d of %d hooks installed -- recording, replay and shadow inputs need v1.0 race.exe", ok, wanted);
        g_record = false;
        g_play[0] = 0;
        return;
    }
    if (!race) logf("replay: input hooks in (random numbers and controls feed shadow checks)");
    else if (g_play[0]) logf("replay: will replay %s\\%s.vpr in the next race (this run's trace: %s)", g_dir, g_play, g_label);
    else logf("replay: recording every race to %s", g_dir);
}

void replay_report() {
    if (g_active) finish("the game closed");
    if (g_play[0] && g_ticks == 0 && !g_active) logf("exit: replay: no race was replayed");
}
