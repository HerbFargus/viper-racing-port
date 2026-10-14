// root_tune.cpp -- the tune race: one car's lap times on one track, run unattended, for setting up a car's stats.
//
// Off unless viperport.ini has `[tune] car=<car>`. Then AppMain (root_main.cpp) runs one tune race between app_begin
// and app_end instead of the menus, and the game quits (no credits).
//
//   [tune]
//   car=puttputt        the car, as the race's car list names it (the .car file's name, no extension)
//   track=0             the track: its slot 0..7, or its name as GetTrackName gives it (default 0)
//   reverse=0           1: run it reversed
//   strength=2          the AI strength whose driver drives it: 0 Easy, 1 Normal, 2 Hard (the default); 3..7 are the
//                       career's, which need their career resources loaded, so they don't run here
//   driver=0            which of that strength's drivers, 0..11 (its personality; the car is always `car`)
//   out=tune-results.txt   the results file, beside race.exe (appended, one line a race)
//   test=handling       instead of a race: the scripted handling tests on the test pad, hell.trk (below)
//   trace=1             (the tests) their state every 0.5 s of physics time in viperport.log
//   trace_ticks=31      ... every this many physics ticks instead (1 = every tick, for acceleration curves)
//   realism= traction_control= abs= yaw_control=   (the tests) one aid set over what aids= gives, to see what
//                       each does (unset: aids= decides)
//   aids=off            (the tests) off: realism 2, no traction control / ABS / yaw control; player: the player's
//                       own -- [GAME] realism (0 and 1 add the yaw- and slide-stabilising forces) and the CONTROL
//                       traction_control, abs_braking and yaw_control as they are. Shifting is automatic either way.
//                       In a race (not the tests) aids=player sets only the AI car's realism to the player's: the AI
//                       always runs its own traction control, ABS and yaw control, whatever the options say.
//
// One race a start: a second race in the same start doesn't repeat the first (something carries over between races).
//
// The player's options are left alone: the learn screen's closing "press a key" ([AI] test_prompt) is skipped in
// tune mode (gx_2d.cpp). A race that hasn't ended after an hour of racing is stopped (status=timeout).
//
// The Options menu's hacks are off for the race (pave the world, no walls, the grip / throttle / gravity boosts, the
// horn ball): it always runs on the stock rules, and the player's hacks are back afterwards.
//
// The race is the AI's learn race (generate_notes_for in phys_ai.cpp, set up the same way): the driver alone on the
// track in `car`, learning its notes for that car from nothing (no notes file is read), the physics at speed 0 (as
// fast as it runs: AIDisplayLearnMode's 24 ticks every 16 ms timer tick, one car) with no 3D drawn and the car sounds
// muted. The race ends when the driver has gone four laps without changing a note (AICar::analyze's "done learning"),
// so the last laps are the car's settled pace on its own learned line. Its notes are not written (notes\ is
// untouched). Random numbers are seeded with a fixed seed (TUNE_SEED) at start-up, so the drivers' personalities roll
// the same, and again as the race loads; with the physics deterministic, the same car and track should give the same
// lap times every run.
//
// A results line: key=value fields separated by tabs --
//   car track reverse strength driver (its number) name laps best settled (the mean of the last 3 laps), the incidents
//   in those last 3 laps: spins backwards (seconds) offroad (seconds) walls wall_time (seconds) teleports; in the whole
//   race: all_spins all_offroad all_walls all_teleports; raced (seconds of racing), wall (real seconds), times
// -- the times in seconds, comma-separated, lap 1 first. status=ok, or status=no-laps (nothing was recorded).
// The incidents are counted every physics tick while the car races (tune_tick, reading the car, changing nothing):
// a spin is the car moving over 5 m/s more than 90 degrees from where it points (it ends back under 45); off road is
// two wheels or more off the road surface; a wall contact is a run of ticks where the AI car's wall heat
// rose (each wall contact force adds 1 to it); a teleport is the car put back on the track (stranded, airlifted).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include "vp_os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "viperport.h"

typedef int Edx;

// the game's (v1.0), as phys_ai.cpp's generate_notes_for calls them
static void** const g_lounge = (void**)0x004eb8b4;                       // IDriverLounge* Lounge (driver.obj)
static uint8_t* const g_learn_mode = (uint8_t*)0x004eb648;              // ai_learn_mode
static void** const g_notes_data = (void**)0x00520b10;                  // aicar_record_notes' buffer
typedef const char*(__cdecl* StrInt_t)(int);
static const StrInt_t GetTrackName = (StrInt_t)0x00406810;
static const StrInt_t GetAIStrengthString = (StrInt_t)0x004061a0;
typedef void(__fastcall* ThisVoid_t)(void*, Edx);
static const ThisVoid_t World_ctor = (ThisVoid_t)0x00462a90;
typedef void(__cdecl* WorldFn_t)(void*);
static const WorldFn_t LoadRace = (WorldFn_t)0x0040a840;
static const WorldFn_t UnloadRace = (WorldFn_t)0x0040a870;
typedef void(__cdecl* DoRace_t)(void*, void*, void*);
static const DoRace_t DoRace = (DoRace_t)0x00401e10;
typedef void(__cdecl* SetDriverMap_t)(int*, int);
static const SetDriverMap_t AISetDriverMap = (SetDriverMap_t)0x00420c50;
typedef void(__cdecl* Void_t)();
static const Void_t AIResetDriverMap = (Void_t)0x00420c30;
typedef const uint8_t*(__cdecl* LapResult_t)(int32_t, int32_t);
static const LapResult_t RecordGetLapResult = (LapResult_t)0x0042a940;    // +4 the lap's time (float, seconds)
typedef void(__cdecl* Rmarin_t)(int, int);
static const Rmarin_t rmarin = (Rmarin_t)0x0041b720;                   // seeds Random's generator
typedef void(__cdecl* Free_t)(void*);
static const Free_t MemFree = (Free_t)0x00414300;
// the lounge's virtuals: +0 Count(strength), +4 Get(strength, driver) -> the driver (+0x210 its name)
#define LOUNGE_VFN(obj, off, R, ...) ((R(__fastcall*)(void*, Edx, ##__VA_ARGS__))((*(void***)(obj))[(off) / 4]))

typedef void(__cdecl* OptGetB_t)(const char*, const char*, uint8_t*);
typedef void(__cdecl* OptSetB_t)(const char*, const char*, uint32_t);
static const OptGetB_t OptionsGetBool = (OptGetB_t)0x00471470;
static const OptSetB_t OptionsSetBool = (OptSetB_t)0x004714c0;
static const char* const* const k_ai_section = (const char* const*)0x004db500;   // "AI"
typedef int(__cdecl* GetInt_t)();
static const GetInt_t GetGameState = (GetInt_t)0x0040a3d0;
#define g_tick (*(volatile int32_t*)0x0052161c)   // physics_tick

static volatile bool g_racing;

// ---- the incidents (tune_tick, on the physics thread, once a tick; reads only, so the race is unchanged) ------------
#define g_cars ((uint8_t* const*)0x00520c18)            // Car*[16] (PhysTaskRegisterCar)
#define CAR_F(c, off) (*(const float*)((c) + (off)))
static const int K_RACE_STATE = 0x4fc;                  // Car: 2 racing
static const int K_ROT = 0x38, K_VEL = 0x234;          // Car: frame.rot (row 2 forward), velocity (world)
static const int K_WHEELS = 0x554, K_WHEEL_SIZE = 0x1a4, K_W_SURFACE = 0x19c;   // Wheel: surface 0 = road
static const int K_TELEPORT_TIME = 0xe44;               // Car: PhysicsGetTime at the last Teleport
static const int K_WALL_HEAT = 0xfd4;                   // AICar: +1 for every wall contact force, decaying
static const int K_LAP = 0xf34;                         // AICar: the lap it's on
struct LapIncidents {
    int ticks;                 // racing ticks seen
    int offroad_ticks;         // two wheels or more off the road surface
    int backwards_ticks;       // moving over 5 m/s more than 90 degrees from where the car points
    int spins;                 // times it went backwards (back under 45 degrees ends one)
    int wall_ticks, walls;     // ticks touching a wall, and separate contacts
    int teleports;             // put back on the track (the AI's stranded reset, an airlift)
};
static const int MAX_LAPS = 64;
static const int RACE_CAP_TICKS = 225000;     // an hour of racing (16 ms ticks)
struct Incidents {
    LapIncidents lap[MAX_LAPS];   // by the AI car's lap counter (later laps share the last)
    bool backwards, on_wall;
    bool stopped;              // the race was stopped (the physics paused there)
    bool timed_out;            // stopped by the cap (RACE_CAP_TICKS)
    int ticks;
    float wall_heat, teleport_time;
};
static Incidents g_inc;

static bool g_test_mode;                         // (g_test, as the physics thread reads it)
static volatile int32_t g_ai_realism = -1;       // [tune] aids=player in a race: the AI car's realism, else -1
static void test_tick();
void tune_tick() {
    if (g_test_mode) {
        test_tick();
        return;
    }
    const uint8_t* c = g_cars[0];
    // aids=player: the AI car races at the player's realism (an AICar is built at 2 whatever the options say; 0 and
    // 1 add the yaw- and slide-stabilising forces). Its traction control, ABS and yaw control are its own.
    if (c && g_ai_realism >= 0) *(int32_t*)(c + 0x4f4) = g_ai_realism;
    Incidents& G = g_inc;
    // The race over (game state 3: the learning done, or the laps run), the physics pauses on this tick. The learn
    // screen only looks every 500 ms of real time -- at this speed several laps, which the car went on lapping (slowly,
    // as a stopped race's cars do) and which counted: a race the learning ended early came out with extra, slow laps,
    // as many as the clock allowed. Pausing here ends every race on the same tick.
    if (!G.stopped && !G.timed_out && G.ticks >= RACE_CAP_TICKS) {   // an hour of racing: stop it (status=timeout)
        G.timed_out = true;
        void* deity = *(void**)0x005218ac;
        ((void(__fastcall*)(void*, int))((*(void***)deity)[0x08 / 4]))(deity, 0);   // Deity::StopRace
        logf("tune: an hour of racing without the race ending: stopped");
    }
    if (!G.stopped && *(const volatile int32_t*)0x004e4e3c == 3) {
        G.stopped = true;
        *(volatile uint8_t*)0x00521608 = 1;      // physics_paused (PhysicsPause's flag)
    }
    if (!c || *(const int32_t*)(c + K_RACE_STATE) != 2) return;
    int lap = *(const int32_t*)(c + K_LAP);
    lap = lap < 0 ? 0 : lap >= MAX_LAPS ? MAX_LAPS - 1 : lap;
    LapIncidents& I = G.lap[lap];
    G.ticks++;
    I.ticks++;
    int off = 0;
    for (int w = 0; w < 4; w++)
        if (*(const int32_t*)(c + K_WHEELS + w * K_WHEEL_SIZE + K_W_SURFACE) != 0) off++;
    if (off >= 2) I.offroad_ticks++;
    const float* m = (const float*)(c + K_ROT);
    const float vx = CAR_F(c, K_VEL), vz = CAR_F(c, K_VEL + 8);
    const float sp = sqrtf(vx * vx + vz * vz);
    if (sp > 5.0f) {
        const float fx = m[6], fz = m[8], fl = sqrtf(fx * fx + fz * fz);
        const float cosang = fl > 0 ? (vx * fx + vz * fz) / (sp * fl) : 1.0f;
        if (cosang < 0) {
            I.backwards_ticks++;
            if (!G.backwards) I.spins++;
            G.backwards = true;
        } else if (cosang > 0.7071f) {
            G.backwards = false;
        }
    }
    const float heat = CAR_F(c, K_WALL_HEAT);
    const bool wall = heat > G.wall_heat + 0.5f;
    if (wall) {
        I.wall_ticks++;
        if (!G.on_wall) I.walls++;
    }
    G.on_wall = wall;
    G.wall_heat = heat;
    const float tt = CAR_F(c, K_TELEPORT_TIME);
    if (G.ticks > 1 && tt > G.teleport_time + 0.5f) I.teleports++;
    G.teleport_time = tt;
}
static float g_times[1000];
static int g_laps;

// AIDisplayLearnMode (gx_2d.cpp) calls this as its race ends, while the race's lap records still exist (DoRace frees
// them before it returns)
void tune_learn_done() {
    g_racing = false;
    g_laps = 0;
    for (int lap = 1; lap < 1000; lap++) {
        const uint8_t* rec = RecordGetLapResult(0, lap);
        if (!rec) break;
        float t;
        memcpy(&t, rec + 4, 4);
        if (!(t > 0)) break;
        g_times[g_laps++] = t;
    }
}
#ifdef _WIN32
// a progress line every 5 s while the race runs (reads the race's state from another thread: a rough look only)
static DWORD WINAPI watch(void*) {
    int last = g_tick;
    while (g_racing) {
        for (int i = 0; i < 5 && g_racing; i++) vpos_Sleep(100);
        if (!g_racing) break;
        const int t = g_tick;
        const uint8_t* c0 = *(uint8_t* const*)0x00520c18;
        float v = 0, x = 0, z = 0;
        if (c0) {
            const float* vel = (const float*)(c0 + 0x234);
            v = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
            x = *(const float*)(c0 + 0x5c);
            z = *(const float*)(c0 + 0x64);
        }
        logf("tune: ... physics tick %d (%.1fx real time), game state %d, car 0 at (%.0f, %.0f) %.1f m/s", t,
             (t - last) * 0.016 / 0.5, GetGameState(), x, z, v);
        last = t;
    }
    return 0;
}
#endif

#define g_hacks_on (*(volatile uint8_t*)0x004e52e4)   // the Options menu's hacks (pave the world, no walls, boosts...)

#define Iat(p, off) (*(int32_t*)((uint8_t*)(p) + (off)))

static bool g_on;
static char g_car[64], g_track[64], g_out[MAX_PATH];
static int g_reverse, g_strength, g_driver;
static bool g_test;                              // [tune] test=handling: the scripted handling tests
static bool g_trace;                             // [tune] trace=1: the tests' state every 0.5 s in the log
static int g_trace_ticks = 31;                   // [tune] trace_ticks=: ... every this many ticks instead
static bool g_player_aids;                       // [tune] aids=player: the tests with the player's own aids
static int g_over_realism = -1, g_over_tc = -1, g_over_abs = -1, g_over_yaw = -1;   // [tune] realism= ... (tests)

void tune_install(const char* ini) {
    vpos_GetPrivateProfileStringA("tune", "car", "", g_car, sizeof g_car, ini);
    g_on = g_car[0] != 0;
    if (!g_on) return;
    if (!build_is_v10()) {                       // the addresses here are v1.0's
        logf("tune: off -- the tune race needs the v1.0 race.exe");
        g_on = false;
        return;
    }
    vpos_GetPrivateProfileStringA("tune", "track", "0", g_track, sizeof g_track, ini);
    vpos_GetPrivateProfileStringA("tune", "out", "tune-results.txt", g_out, sizeof g_out, ini);
    g_strength = (int)vpos_GetPrivateProfileIntA("tune", "strength", 2, ini);
    g_driver = (int)vpos_GetPrivateProfileIntA("tune", "driver", 0, ini);
    g_reverse = vpos_GetPrivateProfileIntA("tune", "reverse", 0, ini) != 0;
    char test[32];
    vpos_GetPrivateProfileStringA("tune", "test", "", test, sizeof test, ini);
    g_test = !_stricmp(test, "handling");
    g_trace = vpos_GetPrivateProfileIntA("tune", "trace", 0, ini) != 0;
    g_trace_ticks = (int)vpos_GetPrivateProfileIntA("tune", "trace_ticks", 31, ini);
    if (g_trace_ticks < 1) g_trace_ticks = 1;
    char aids[32];
    vpos_GetPrivateProfileStringA("tune", "aids", "off", aids, sizeof aids, ini);
    g_player_aids = !_stricmp(aids, "player");
    // single aids, over whatever aids= gives (-1: leave it): to find which one does what
    g_over_realism = (int)vpos_GetPrivateProfileIntA("tune", "realism", -1, ini);
    g_over_tc = (int)vpos_GetPrivateProfileIntA("tune", "traction_control", -1, ini);
    g_over_abs = (int)vpos_GetPrivateProfileIntA("tune", "abs", -1, ini);
    g_over_yaw = (int)vpos_GetPrivateProfileIntA("tune", "yaw_control", -1, ini);
    logf("tune: on -- car %s, track %s%s, strength %d, driver %d", g_car, g_track, g_reverse ? " reversed" : "",
         g_strength, g_driver);
}

bool tune_active() { return g_on; }

// the track's slot: a number 0..7, or a name matching GetTrackName's (case ignored); -1 if neither
static int track_slot() {
    char* end;
    long n = strtol(g_track, &end, 10);
    if (*g_track && !*end) return n >= 0 && n < 8 ? (int)n : -1;
    for (int i = 0; i < 8; i++) {
        const char* name = GetTrackName(i);
        if (name && !_stricmp(name, g_track)) return i;
    }
    return -1;
}

// one tune race: `car` driven by the strength's driver on the track slot; a results line appended
static void tune_race(int slot, int strength, int driver) {
    void* lounge = *g_lounge;
    const char* track = GetTrackName(slot);
    const uint8_t* d = LOUNGE_VFN(lounge, 4, const uint8_t*, int, int)(lounge, 0, strength, driver);
    const char* dname = *(const char* const*)(d + 0x210);
    logf("tune: %s on %s%s, driven by %s (strength %d, driver %d)", g_car, track, g_reverse ? " reversed" : "", dname,
         strength, driver);

    // the World, as generate_notes_for builds it: one AI car, `car`, in a 20-lap race the learning ends
    alignas(4) uint8_t world[0xcd4];
    int args[1] = {driver};
    *g_notes_data = 0;
    g_laps = 0;
    memset(&g_inc, 0, sizeof g_inc);
    World_ctor(world, 0);
    strncpy((char*)world + 8, track, 0x1f);
    world[8 + 0x1f] = 0;
    Iat(world, 0xca8) = 1;                       // the car count
    Iat(world, 0x28) = 1;                        // the car: an AI driver
    strncpy((char*)world + 0x2c, dname, 12);     // +4 the driver's name (13 bytes)
    world[0x2c + 12] = 0;
    strncpy((char*)world + 0x39, g_car, 34);     // +0x11 the car (35 bytes)
    world[0x39 + 34] = 0;
    Iat(world, 0xcac) = 2;                       // GameOptions: realism
    Iat(world, 0xcb0) = 1;                       // game time
    Iat(world, 0xcb4) = 0;                       // weather
    Iat(world, 0xcb8) = 0;                       // field: the pack
    Iat(world, 0xcbc) = 3;                       // race type
    Iat(world, 0xcc0) = 0x14;                    // laps
    Iat(world, 0xcc4) = 0;
    Iat(world, 0xcc8) = 0;
    Iat(world, 0xccc) = strength;
    world[0xcd0] = 0;                            // damage off
    world[0xcd1] = (uint8_t)g_reverse;
    // the race runs on the stock rules whatever the player's hacks are (pave the world turns every surface into road):
    // the hacks off for the race, put back after
    const uint8_t hacks = g_hacks_on;
    g_hacks_on = 0;
    AISetDriverMap(args, 1);
    g_ai_realism = -1;
    if (g_player_aids) {
        volatile int32_t r = 2;
        ((void(__cdecl*)(const char*, const char*, volatile int32_t*))0x004713e0)("GAME", "realism", &r);   // OptionsGetI
        g_ai_realism = r;
        logf("tune: the AI car races at the player's realism, %d", (int)r);
    }
    rmarin((int)(TUNE_SEED >> 16), (int)(TUNE_SEED & 0xffff));   // the race's random numbers, the same every run
    LoadRace(world);
    const DWORD t0 = vpos_GetTickCount();
    *g_learn_mode = 1;
    g_racing = true;
#ifdef _WIN32
    CloseHandle(CreateThread(0, 0, watch, 0, 0, 0));
#endif
    DoRace(world, 0, 0);
    g_racing = false;
    *g_learn_mode = 0;
    const double wall = (vpos_GetTickCount() - t0) / 1000.0;

    float* const times = g_times;
    const int laps = g_laps;
    UnloadRace(world);
    const int realism = g_ai_realism >= 0 ? (int)g_ai_realism : 2;
    g_ai_realism = -1;
    g_hacks_on = hacks;
    AIResetDriverMap();
    if (*g_notes_data) MemFree(*g_notes_data);   // the learned notes: not kept
    *g_notes_data = 0;

    float best = 0;
    double settled = 0;
    for (int i = 0; i < laps; i++)
        if (!best || times[i] < best) best = times[i];
    const int k = laps < 3 ? laps : 3;
    for (int i = laps - k; i < laps; i++) settled += times[i];
    if (k) settled /= k;
    // the incidents in total, and in the last 3 laps (the settled laps: the learning's crashes are mostly early).
    // The AI car's counter is the lap it's on, so lap record n (1-based) was driven with the counter at n.
    LapIncidents all = {}, last = {};
    for (int l = 0; l < MAX_LAPS; l++) {
        const LapIncidents& L = g_inc.lap[l];
        LapIncidents* to[2] = {&all, l > laps - k && l <= laps ? &last : 0};
        for (LapIncidents* t : to) {
            if (!t) continue;
            t->ticks += L.ticks;
            t->offroad_ticks += L.offroad_ticks;
            t->backwards_ticks += L.backwards_ticks;
            t->spins += L.spins;
            t->wall_ticks += L.wall_ticks;
            t->walls += L.walls;
            t->teleports += L.teleports;
        }
    }
    char per_lap[1024];
    int pn = 0;
    for (int l = 0; l <= laps + 1 && l < MAX_LAPS && pn < (int)sizeof per_lap - 40; l++) {
        const LapIncidents& L = g_inc.lap[l];
        pn += _snprintf(per_lap + pn, sizeof per_lap - pn, " %d:%d/%.0f/%d/%d", l, L.spins, L.offroad_ticks * 0.016,
                        L.walls, L.teleports);
    }
    logf("tune: %d laps in %.1f s, best %.3f, settled %.3f; last %d laps: %d spins, %.1f s off road, %d wall contacts, "
         "%d teleports; in all: %d, %.1f s, %d, %d", laps, wall, best, settled, k, last.spins, last.offroad_ticks * 0.016,
         last.walls, last.teleports, all.spins, all.offroad_ticks * 0.016, all.walls, all.teleports);
    logf("tune: per lap (spins/off-road s/walls/teleports):%s", per_lap);

    FILE* f = fopen(g_out, "a");
    if (!f) {
        logf("tune: can't write %s", g_out);
        return;
    }
    fprintf(f, "status=%s\tcar=%s\ttrack=%s\treverse=%d\tstrength=%d\tdriver=%d\tname=%s\tlaps=%d\tbest=%.3f\tsettled=%.3f"
               "\trealism=%d\tspins=%d\tbackwards=%.2f\toffroad=%.2f\twalls=%d\twall_time=%.2f\tteleports=%d"
               "\tall_spins=%d\tall_offroad=%.2f\tall_walls=%d\tall_teleports=%d\traced=%.1f\twall=%.1f\ttimes=",
            g_inc.timed_out ? "timeout" : laps ? "ok" : "no-laps", g_car, track, g_reverse, strength, driver, dname, laps, best, settled, realism,
            last.spins,
            last.backwards_ticks * 0.016, last.offroad_ticks * 0.016, last.walls, last.wall_ticks * 0.016, last.teleports,
            all.spins, all.offroad_ticks * 0.016, all.walls, all.teleports, all.ticks * 0.016, wall);
    for (int i = 0; i < laps; i++) fprintf(f, i ? ",%.3f" : "%.3f", times[i]);
    fprintf(f, "\n");
    fclose(f);
}

// ---- the handling tests ([tune] test=handling): a player car on the test pad, driven by script --------------------
//
// tune_handling (main thread) builds a race on "hell" (the test pad, vrmod-experiments experiments/tune/
// build_testpad.py) with one PLAYER car -- no input device is read: the controls are g_tune_ctl, which the physics'
// DriverGetSteering / Throttle / Braking return while it's on (phys_task.cpp), after the controller settings and the
// game's steering filter (the "square" option, the speed scaling), so every car gets the same commands whatever the
// player's controller is set to. The car's own steering rate limit still applies. The race runs at realism 2 (no
// stabilising forces), the car set (on the car, every tick; the options are never written) to auto shifting and
// clutch, with traction control, ABS and yaw control off -- or, aids=player, as the player's options have them.
//
// test_tick (physics thread, every tick) runs the tests in order, teleporting the car between them (Car::Teleport,
// through the vtable as the game does), then stops the race. Headings and yaw rates come from the car's frame (row 2
// forward), speeds from its world velocity (horizontal). Steering's sign is calibrated first: +1 is whichever way a
// positive command turns the car.
//   accel    straight lane, full throttle from rest: 0-60, 0-100 mph, the quarter mile (time, speed), top speed,
//            and the wobble: the yaw rate's RMS above 10 m/s (a car going straight should hardly yaw at all), and
//            the scraping: seconds the body touched the ground (its scrape energy rising), from what speed
//            (the most it reaches in 9.55 km or 150 s; top_time near accel_time = still accelerating); heading held on the lane by a small steering correction
//   brake60 / brake100   up to just over the speed, then throttle off and full brake with the steering fixed at 0:
//            the distance from the moment it passes 60 / 100 mph down to 0.5 m/s, how far it turned (degrees) and
//            drifted sideways (m) while stopping
//   skidpad  a 50 m circle held by steering on heading and radius, the speed creeping up 0.25 m/s every second: the
//            best lateral g held for 2 s (within 2.5 m of the circle, slip under 15 degrees), its speed, and how it
//            goes at the limit -- "push" (runs wide, 8 m off the circle: understeer) or "spin" (slip over 35
//            degrees: oversteer), "none" if it never lets go
//   liftoff  on the circle at 90% of the skidpad's best speed, held for 3 s, then the throttle shut and the steering
//            frozen for 2 s: the yaw rate's peak against before (1.0 = no change; more = the tail steps out), the
//            peak slip angle, and whether it spun
//   step50 / step80   straight at 50 / 80 mph, held 2 s, then a step of 0.15 steering with the throttle frozen, 3 s:
//            the yaw rate's rise time (to 90% of steady), overshoot (peak / steady - 1), the steady lateral g, the
//            final slip angle, and whether it spun (slip over 60 degrees)
TuneControls g_tune_ctl;

static const float DT = 0.016f, MPH = 0.44704f, G = 9.81f;
static const int K_POS = 0x5c;                          // Car: frame.pos
static const float PI_F = 3.14159265f;

// the pad (game frame)
static const float LANE_X = 200.0f, STEP_X = -150.0f, LANE_Z0 = 4800.0f;   // both lanes run from Z 4800 toward -Z
static const float PAD_CX = -150.0f, PAD_CZ = -700.0f, PAD_R = 50.0f;

enum Phase { T_WAIT, T_CAL, T_ACCEL, T_BRAKE60, T_BRAKE100, T_SKIDPAD, T_LIFTOFF, T_STEP50, T_STEP80, T_DONE };
static const char* const k_phase_name[] = {"wait", "cal", "accel", "brake60", "brake100", "skidpad", "liftoff",
                                           "step50", "step80", "done"};

struct StepResult { float rise, overshoot, lat_g, slip; int spun; };
struct TestResults {
    float t60, t100, qm_time, qm_speed, top_speed;
    float top_time, accel_time;  // when the top speed came (s from the start), and how long the run was
    float scrape_time, scrape_speed;   // seconds of the run the body scraped the ground; the speed it started at (mph)
    float wobble;                // the yaw rate's RMS (rad/s) on the straight above 10 m/s: a car that won't run straight
    float brake60_dist, brake60_turn, brake60_drift, brake100_dist, brake100_turn, brake100_drift;
    float skid_g, skid_speed;
    const char* skid_limit;
    float lift_yaw_gain, lift_slip;
    int lift_spun;
    StepResult step[2];
    int steer_sign;
    const char* failed;          // a test that couldn't run (the car left the pad...), else 0
};
static TestResults g_res;
struct TestAids { int32_t tc, abs, yaw; };
static TestAids g_aids;                           // the run's (tune_handling)

struct TestState {
    Phase phase;
    int sub;                     // the phase's own step
    int t;                       // ticks in the step
    float heading_prev;
    float x0, z0, dist;          // where the measured run started; distance driven since
    float aux[8];
    double wob_sum;
    int wob_n;
    float scrape_prev;
    // the skidpad's 2 s window
    float ring[125];
    int ring_n, held;
    float tangent_sign;
    float best_v;
    float steer_hist[32];
    int steer_n;
    float yr_hist[190];          // the step's yaw rates
    int total;                   // ticks since the tests began (TEST_CAP_TICKS ends them)
};
static TestState g_ts;
static const int TEST_CAP_TICKS = 75000;          // 20 minutes of physics: the tests stop wherever they are

static float wrapf(float a) {
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

struct CarView {
    uint8_t* c;
    float x, z, vx, vz, v, heading, vheading, slip;
};
static CarView view(uint8_t* c) {
    CarView cv;
    cv.c = c;
    cv.x = CAR_F(c, K_POS);
    cv.z = CAR_F(c, K_POS + 8);
    cv.vx = CAR_F(c, K_VEL);
    cv.vz = CAR_F(c, K_VEL + 8);
    cv.v = sqrtf(cv.vx * cv.vx + cv.vz * cv.vz);
    const float* m = (const float*)(c + K_ROT);
    cv.heading = atan2f(m[6], m[8]);
    cv.vheading = cv.v > 1.0f ? atan2f(cv.vx, cv.vz) : cv.heading;
    cv.slip = cv.v > 3.0f ? wrapf(cv.vheading - cv.heading) : 0.0f;
    return cv;
}

struct TP3 { float x, y, z; };
static void teleport(uint8_t* c, float x, float z, float dx, float dz) {
    const TP3 pos = {x, 1.0f, z};
    const float dir[2] = {dx, dz};
    ((void(__fastcall*)(void*, int, const TP3*, const float*))((*(void***)c)[0x40 / 4]))(c, 0, &pos, dir);
}

// steering toward a heading: the error times a gain, through the calibrated sign
static float steer_to(const CarView& cv, float want, float gain, float limit) {
    return clampf((float)g_res.steer_sign * gain * wrapf(want - cv.heading), -limit, limit);
}
// holding a straight lane: steer_to with the gain falling as 1 / speed above 15.6 m/s (35 mph), the speed the game's
// own steering filter starts cutting a player's steering at -- a fixed gain at 250 mph sets the car weaving
static float steer_lane(const CarView& cv, float want) {
    const float k = cv.v > 15.6f ? 15.6f / cv.v : 1.0f;
    return steer_to(cv, want, 1.5f * k, 0.3f);
}
// the heading that keeps the car on a lane along -Z at X = lane_x (pulled back 0.02 rad a metre, at most 0.3)
static float lane_heading(const CarView& cv, float lane_x) {
    const float pull = clampf(0.02f * (cv.x - lane_x), -0.3f, 0.3f);   // off to +X: head a little toward -X
    return atan2f(-pull, -1.0f);
}
static float throttle_for(const CarView& cv, float vt) { return clampf(0.2f + 0.6f * (vt - cv.v), 0.0f, 1.0f); }

static void set_ctl(float steer, float throttle, float brake) {
    g_tune_ctl.steer = steer;
    g_tune_ctl.throttle = throttle;
    g_tune_ctl.brake = brake;
}
static void next_phase(Phase p) {
    g_ts.phase = p;
    g_ts.sub = 0;
    g_ts.t = -1;                 // test_tick adds one after the step runs: the phase starts at 0
    logf("tune: test %s", k_phase_name[p]);
}
// the first step of most tests: put the car somewhere and let it settle (1.5 s, brakes on)
static bool placed(uint8_t* c, float x, float z, float dx, float dz) {
    if (g_ts.t == 0) teleport(c, x, z, dx, dz);
    set_ctl(0.0f, 0.0f, 1.0f);
    return g_ts.t >= 94;
}
static void sub_next() {
    g_ts.sub++;
    g_ts.t = -1;                 // test_tick adds one after the step runs
}

static void test_tick() {
    uint8_t* c = (uint8_t*)g_cars[0];
    if (!c) return;
    // the car's gearbox and aids, set on the car (as PlayCar's constructor sets them from the options): automatic
    // shifting and clutch, and the run's traction control / ABS / yaw control
    typedef void(__fastcall* CarSetI_t)(void*, int, int32_t);
    ((CarSetI_t)0x004395e0)(c, 0, g_aids.tc);         // Car::SetTractionControl
    ((CarSetI_t)0x00439600)(c, 0, g_aids.abs);        // Car::SetABSBraking
    *(int32_t*)(c + 0x4f8) = g_aids.yaw;              // yaw control
    c[0xec8] = 1;                                     // PlayCar: auto shift
    c[0xec9] = 1;                                     //          auto clutch
    c[0x534] = 1;                                     // Car: auto clutch
    TestState& S = g_ts;
    TestResults& R = g_res;
    const CarView cv = view(c);
    const float yaw_rate = wrapf(cv.heading - S.heading_prev) / DT;
    S.heading_prev = cv.heading;
    if (S.phase != T_DONE && ++S.total > TEST_CAP_TICKS) {   // a car that can't do a test must not hang the game
        if (!R.failed) R.failed = k_phase_name[S.phase];
        logf("tune: test %s: 20 minutes of physics and the tests aren't done: stopped", k_phase_name[S.phase]);
        next_phase(T_DONE);
    }
    if (S.phase != T_WAIT && S.phase != T_DONE && S.sub > 0 && (fabsf(cv.x) > 295.0f || fabsf(cv.z) > 4895.0f)) {
        if (!R.failed) R.failed = k_phase_name[S.phase];
        logf("tune: test %s: the car left the pad at (%.0f, %.0f)", k_phase_name[S.phase], cv.x, cv.z);
        next_phase((Phase)(S.phase + 1));
    }

    if (g_trace && S.phase != T_WAIT && S.t % g_trace_ticks == 0)
        logf("tune: trace %s.%d t %.2f pos %.1f %.1f v %.2f head %.3f slip %.3f gear %d rpm %.0f thr %.2f brk %.2f st %.3f wy %.4f",
             k_phase_name[S.phase], S.sub, S.t * DT, cv.x, cv.z, cv.v, cv.heading, cv.slip, *(const int32_t*)(c + 0xd68),
             CAR_F(c, 0xc0c), CAR_F(c, 0xde0), CAR_F(c, 0xde4), CAR_F(c, 0x500), CAR_F(c, 0x244));
    switch (S.phase) {
    case T_WAIT:
        set_ctl(0.0f, 0.0f, 1.0f);
        if (*(const int32_t*)(c + K_RACE_STATE) == 2) next_phase(T_CAL);
        break;

    case T_CAL:                  // which way does a positive steering command turn it?
        if (S.sub == 0) {
            if (placed(c, LANE_X, LANE_Z0, 0.0f, -1.0f)) sub_next();
        } else if (S.sub == 1) {
            set_ctl(0.0f, throttle_for(cv, 15.0f), 0.0f);
            if (cv.v > 14.5f) {
                S.aux[0] = cv.heading;
                sub_next();
            } else if (S.t * DT > 30.0f) {   // it can't get to 15 m/s: without the steering's sign nothing else runs
                R.failed = "cal";
                logf("tune: test cal: no 15 m/s in 30 s (%.1f m/s): stopped", cv.v);
                next_phase(T_DONE);
            }
        } else {
            set_ctl(0.2f, throttle_for(cv, 15.0f), 0.0f);
            if (S.t >= 31) {
                R.steer_sign = wrapf(cv.heading - S.aux[0]) > 0 ? 1 : -1;
                logf("tune: steering sign %d (heading %+.3f rad in 0.5 s)", R.steer_sign, wrapf(cv.heading - S.aux[0]));
                next_phase(T_ACCEL);
            }
        }
        break;

    case T_ACCEL:
        if (S.sub == 0) {
            if (placed(c, LANE_X, LANE_Z0, 0.0f, -1.0f)) {
                S.x0 = cv.x;
                S.z0 = cv.z;
                sub_next();
            }
        } else {
            set_ctl(steer_lane(cv, lane_heading(cv, LANE_X)), 1.0f, 0.0f);
            const float t = S.t * DT;
            const float d = sqrtf((cv.x - S.x0) * (cv.x - S.x0) + (cv.z - S.z0) * (cv.z - S.z0));
            if (!R.t60 && cv.v >= 60 * MPH) R.t60 = t;
            if (!R.t100 && cv.v >= 100 * MPH) R.t100 = t;
            if (!R.qm_time && d >= 402.34f) {
                R.qm_time = t;
                R.qm_speed = cv.v / MPH;
            }
            if (cv.v / MPH > R.top_speed) {
                R.top_speed = cv.v / MPH;
                R.top_time = t;
            }
            const float scrape = CAR_F(c, 0xe40);  // Car scrape_energy: what the body's collisions summed (the sparks)
            if (S.t > 0 && cv.v > 10.0f && scrape > S.scrape_prev + 1e-3f) {
                if (!R.scrape_time) R.scrape_speed = cv.v / MPH;
                R.scrape_time += DT;
            }
            S.scrape_prev = scrape;
            if (cv.v > 10.0f) {
                const float wy = CAR_F(c, 0x244);    // the angular velocity's up component
                S.wob_sum += (double)wy * wy;
                S.wob_n++;
            }
            if (t >= 150.0f || cv.z < -4750.0f) {
                R.accel_time = t;
                R.wobble = S.wob_n ? (float)sqrt(S.wob_sum / S.wob_n) : 0.0f;
                next_phase(T_BRAKE60);
            }
        }
        break;

    case T_BRAKE60:
    case T_BRAKE100: {
        const float vt = (S.phase == T_BRAKE60 ? 60.0f : 100.0f) * MPH;
        if (S.sub == 0) {
            if (placed(c, LANE_X, LANE_Z0, 0.0f, -1.0f)) sub_next();
        } else if (S.sub == 1) {             // up to speed
            set_ctl(steer_lane(cv, lane_heading(cv, LANE_X)), 1.0f, 0.0f);
            if (cv.v >= vt + 1.0f) sub_next();
            if (S.t * DT > 40.0f) {          // it can't get there
                if (!R.failed) R.failed = k_phase_name[S.phase];
                next_phase((Phase)(S.phase + 1));
            }
        } else {                             // full brake, steering fixed
            set_ctl(0.0f, 0.0f, 1.0f);
            if (S.sub == 2 && cv.v <= vt) {  // the measured stop starts here
                S.x0 = cv.x;
                S.z0 = cv.z;
                S.aux[0] = cv.heading;
                S.sub = 3;
            }
            if (S.sub == 3 && cv.v < 0.5f) {
                const float dx = cv.x - S.x0, dz = cv.z - S.z0;
                const float dist = sqrtf(dx * dx + dz * dz);
                const float turn = fabsf(wrapf(cv.heading - S.aux[0])) * 180.0f / PI_F;
                const float drift = fabsf(dx);       // the lane runs along Z: sideways is X
                if (S.phase == T_BRAKE60) {
                    R.brake60_dist = dist;
                    R.brake60_turn = turn;
                    R.brake60_drift = drift;
                } else {
                    R.brake100_dist = dist;
                    R.brake100_turn = turn;
                    R.brake100_drift = drift;
                }
                next_phase((Phase)(S.phase + 1));
            } else if (S.t * DT > 30.0f) {
                if (!R.failed) R.failed = k_phase_name[S.phase];
                next_phase((Phase)(S.phase + 1));
            }
        }
        break;
    }

    case T_SKIDPAD:
    case T_LIFTOFF: {
        const bool lift = S.phase == T_LIFTOFF;
        if (S.sub == 0) {
            if (lift && R.skid_speed <= 0) {  // no skidpad result to work from
                next_phase(T_STEP50);
                break;
            }
            if (placed(c, PAD_CX + PAD_R, PAD_CZ, 0.0f, -1.0f)) {
                S.tangent_sign = 1.0f;       // from (C.x + R, C.z) the tangent (rz, -rx) / d points along -Z
                S.ring_n = S.held = 0;
                S.best_v = 0;
                S.steer_n = 0;
                sub_next();
            }
            break;
        }
        // hold the circle: the tangent, pulled toward the radius
        const float rx = cv.x - PAD_CX, rz = cv.z - PAD_CZ;
        const float d = sqrtf(rx * rx + rz * rz);
        const float tx = S.tangent_sign * rz / d, tz = S.tangent_sign * -rx / d;
        const float pull = clampf(0.05f * (d - PAD_R), -0.6f, 0.6f);
        const float want = atan2f(tx - pull * rx / d, tz - pull * rz / d);
        const float t = S.t * DT;
        const bool on_circle = fabsf(d - PAD_R) < 2.5f && fabsf(cv.slip) < 15.0f * PI_F / 180.0f;
        S.held = on_circle ? S.held + 1 : 0;
        if (!lift) {
            const float vt = 8.0f + 0.25f * t;
            set_ctl(steer_to(cv, want, 2.5f, 1.0f), throttle_for(cv, vt), cv.v > vt + 2.0f ? 0.3f : 0.0f);
            S.ring[S.ring_n++ % 125] = cv.v * cv.v / d;
            if (S.held >= 125 && S.ring_n >= 125) {
                float sum = 0;
                for (float a : S.ring) sum += a;
                const float g = sum / 125.0f / G;
                if (g > R.skid_g) {
                    R.skid_g = g;
                    R.skid_speed = cv.v;
                }
            }
            const char* limit = 0;
            if (d - PAD_R > 8.0f) limit = "push";
            else if (fabsf(cv.slip) > 35.0f * PI_F / 180.0f) limit = "spin";
            else if (vt > 85.0f || t > 150.0f) limit = "none";
            if (limit) {
                R.skid_limit = limit;
                logf("tune: skidpad: %.3f g at %.1f m/s, limit %s", R.skid_g, R.skid_speed, limit);
                next_phase(T_LIFTOFF);
            }
        } else if (S.sub == 1) {            // settle on the circle at 90% of the best speed, built up 1 m/s a second
            const float vt = fminf(0.9f * R.skid_speed, 8.0f + 1.0f * t);   // (full throttle onto the circle spins
                                                                             //  a strong car: the skidpad ramps too)
            const float st = steer_to(cv, want, 2.5f, 1.0f);
            set_ctl(st, throttle_for(cv, vt), cv.v > vt + 2.0f ? 0.3f : 0.0f);
            S.steer_hist[S.steer_n++ % 32] = st;
            S.ring[S.ring_n++ % 125] = yaw_rate;
            if (8.0f + 1.0f * t >= 0.9f * R.skid_speed && S.held >= 188) {
                float sum = 0, ysum = 0;
                for (float a : S.steer_hist) sum += a;
                for (int k = 0; k < 31; k++) ysum += fabsf(S.ring[(S.ring_n - 1 - k + 125) % 125]);
                S.aux[0] = sum / 32.0f;      // the steering, frozen
                S.aux[1] = ysum / 31.0f;     // the yaw rate before
                S.aux[2] = 0;                // its peak after
                S.aux[3] = 0;                // the peak slip
                sub_next();
            } else if (t > 60.0f) {
                if (!R.failed) R.failed = "liftoff";
                next_phase(T_STEP50);
            }
        } else {                             // lift: throttle shut, steering frozen, 2 s
            set_ctl(S.aux[0], 0.0f, 0.0f);
            if (fabsf(yaw_rate) > S.aux[2]) S.aux[2] = fabsf(yaw_rate);
            if (fabsf(cv.slip) > S.aux[3]) S.aux[3] = fabsf(cv.slip);
            if (fabsf(cv.slip) > 60.0f * PI_F / 180.0f) R.lift_spun = 1;
            if (S.t >= 125) {
                R.lift_yaw_gain = S.aux[1] > 0 ? S.aux[2] / S.aux[1] : 0;
                R.lift_slip = S.aux[3] * 180.0f / PI_F;
                next_phase(T_STEP50);
            }
        }
        break;
    }

    case T_STEP50:
    case T_STEP80: {
        const int k = S.phase == T_STEP50 ? 0 : 1;
        const float vt = (k ? 80.0f : 50.0f) * MPH;
        StepResult& r = R.step[k];
        if (S.sub == 0) {
            if (placed(c, STEP_X, LANE_Z0, 0.0f, -1.0f)) sub_next();
        } else if (S.sub == 1) {             // to speed, then held 2 s
            set_ctl(steer_lane(cv, lane_heading(cv, STEP_X)), throttle_for(cv, vt), 0.0f);
            S.aux[0] = fabsf(cv.v - vt) < 0.5f ? S.aux[0] + 1 : 0;
            if (S.aux[0] >= 125) {
                S.aux[1] = g_tune_ctl.throttle;   // the throttle, frozen
                S.aux[2] = 0;                      // the yaw rate's peak
                S.aux[3] = 0;                      // its steady sum
                S.aux[4] = 0;                      // the rise time
                S.aux[5] = 0;
                sub_next();
            } else if (S.t * DT > 40.0f) {
                if (!R.failed) R.failed = k_phase_name[S.phase];
                next_phase((Phase)(S.phase + 1));
            }
        } else {                             // the step, 3 s
            set_ctl((float)R.steer_sign * 0.15f, S.aux[1], 0.0f);
            const float yr = fabsf(yaw_rate);
            if (yr > S.aux[2]) S.aux[2] = yr;
            if (S.t < 190) S.yr_hist[S.t] = yr;   // the yaw rate's history, for the rise time
            if (S.t >= 125) S.aux[3] += yr;  // the last second
            if (fabsf(cv.slip) > 60.0f * PI_F / 180.0f) r.spun = 1;
            if (S.t >= 187) {
                const float steady = S.aux[3] / 62.0f;
                r.overshoot = steady > 0 ? S.aux[2] / steady - 1.0f : 0;
                r.lat_g = cv.v * steady / G;
                r.slip = fabsf(cv.slip) * 180.0f / PI_F;
                r.rise = 0;
                for (int i = 0; i < 188; i++)
                    if (S.yr_hist[i] >= 0.9f * steady) {
                        r.rise = i * DT;
                        break;
                    }
                next_phase((Phase)(S.phase + 1));
            }
        }
        break;
    }

    case T_DONE:
        set_ctl(0.0f, 0.0f, 1.0f);
        if (S.t == 1) {
            void* deity = *(void**)0x005218ac;
            ((void(__fastcall*)(void*, int))((*(void***)deity)[0x08 / 4]))(deity, 0);   // Deity::StopRace
        }
        break;
    }
    S.t++;
}

typedef void(__cdecl* OptGetI_t)(const char*, const char*, volatile int32_t*);
static const OptGetI_t OptionsGetI = (OptGetI_t)0x004713e0;
typedef void(__cdecl* LoadSetup_t)(void*, const char*, const char*);
static const LoadSetup_t CarFileLoadDefaultSetup = (LoadSetup_t)0x00465630;

static void tune_handling() {
    const char* track = "hell";
    logf("tune: handling tests -- %s on %s", g_car, track);
    alignas(4) uint8_t world[0xcd4];
    memset(&g_res, 0, sizeof g_res);
    memset(&g_ts, 0, sizeof g_ts);
    World_ctor(world, 0);
    strncpy((char*)world + 8, track, 0x1f);
    world[8 + 0x1f] = 0;
    Iat(world, 0xca8) = 1;                       // one car
    Iat(world, 0x28) = 0;                        // the player
    strncpy((char*)world + 0x2c, "Test", 12);
    strncpy((char*)world + 0x39, g_car, 34);
    world[0x39 + 34] = 0;
    Iat(world, 0x28 + 0xc0) = ~0;                // ~paint job: the first
    alignas(4) uint8_t setup[0xd4];              // the car's default setup, as the menus give it
    memset(setup, 0, sizeof setup);
    CarFileLoadDefaultSetup(setup, g_car, track);
    memcpy(world + 0x28 + 0x34, setup, 0x8c);
    volatile int32_t realism = 2;                // realism 2: no stabilising forces
    if (g_player_aids) OptionsGetI("GAME", "realism", &realism);
    if (g_over_realism >= 0) realism = g_over_realism;
    Iat(world, 0xcac) = realism;
    Iat(world, 0xcb0) = 1;
    Iat(world, 0xcb4) = 0;
    Iat(world, 0xcb8) = 0;
    Iat(world, 0xcbc) = 3;
    Iat(world, 0xcc0) = 0x14;
    Iat(world, 0xcc4) = 0;                       // event 0: the RaceDeity
    Iat(world, 0xcc8) = 0;
    Iat(world, 0xccc) = 2;
    world[0xcd0] = 0;                            // damage off
    world[0xcd1] = 0;
    const uint8_t hacks = g_hacks_on;
    g_hacks_on = 0;
    // the aids the test car gets (test_tick sets them on it every tick; the player's options are only read)
    g_aids = {0, 0, 0};
    if (g_player_aids) {
        volatile int32_t tc = 1, abs_on = 1, yaw = 0;   // PlayCar's defaults, as its constructor reads them
        OptionsGetI("CONTROL", "traction_control", &tc);
        OptionsGetI("CONTROL", "abs_braking", &abs_on);
        OptionsGetI("CONTROL", "yaw_control", &yaw);
        g_aids = {tc, abs_on, yaw};
    }
    if (g_over_tc >= 0) g_aids.tc = g_over_tc;
    if (g_over_abs >= 0) g_aids.abs = g_over_abs;
    if (g_over_yaw >= 0) g_aids.yaw = g_over_yaw;
    rmarin((int)(TUNE_SEED >> 16), (int)(TUNE_SEED & 0xffff));
    LoadRace(world);
    const DWORD t0 = vpos_GetTickCount();
    g_tune_ctl.on = true;
    *g_learn_mode = 1;                           // the learn screen's loop: physics flat out, no 3D
    g_racing = true;
#ifdef _WIN32
    CloseHandle(CreateThread(0, 0, watch, 0, 0, 0));
#endif
    DoRace(world, 0, 0);
    g_racing = false;
    *g_learn_mode = 0;
    g_tune_ctl.on = false;
    const double wall = (vpos_GetTickCount() - t0) / 1000.0;
    UnloadRace(world);
    g_hacks_on = hacks;
    if (*g_notes_data) MemFree(*g_notes_data);
    *g_notes_data = 0;

    const TestResults& R = g_res;
    const bool done = g_ts.phase == T_DONE;
    logf("tune: handling: %s in %.1f s%s%s", done ? "done" : "NOT finished", wall, R.failed ? ", failed at " : "",
         R.failed ? R.failed : "");
    FILE* f = fopen(g_out, "a");
    if (!f) {
        logf("tune: can't write %s", g_out);
        return;
    }
    const int32_t tc = g_aids.tc, abs_on = g_aids.abs, yaw = g_aids.yaw;
    fprintf(f, "status=%s\tcar=%s\ttest=handling\taids=%s\trealism=%d\ttraction_control=%d\tabs=%d\tyaw_control=%d"
               "\tfailed=%s\tsteer_sign=%d"
               "\twobble=%.4f\tt60=%.2f\tt100=%.2f\tqm_time=%.2f\tqm_speed=%.1f\ttop_speed=%.1f\ttop_time=%.1f\taccel_time=%.1f\tscrape_time=%.2f\tscrape_speed=%.1f"
               "\tbrake60=%.1f\tbrake60_turn=%.2f\tbrake60_drift=%.2f\tbrake100=%.1f\tbrake100_turn=%.2f\tbrake100_drift=%.2f"
               "\tskid_g=%.3f\tskid_speed=%.1f\tskid_limit=%s\tlift_yaw_gain=%.2f\tlift_slip=%.1f\tlift_spun=%d",
            done ? "ok" : "unfinished", g_car, g_player_aids ? "player" : "off", (int)realism, (int)tc, (int)abs_on,
            (int)yaw, R.failed ? R.failed : "-", R.steer_sign, R.wobble, R.t60, R.t100, R.qm_time,
            R.qm_speed, R.top_speed, R.top_time, R.accel_time, R.scrape_time, R.scrape_speed, R.brake60_dist, R.brake60_turn, R.brake60_drift, R.brake100_dist, R.brake100_turn,
            R.brake100_drift, R.skid_g, R.skid_speed, R.skid_limit ? R.skid_limit : "-", R.lift_yaw_gain, R.lift_slip,
            R.lift_spun);
    for (int k = 0; k < 2; k++)
        fprintf(f, "\tstep%d_rise=%.2f\tstep%d_overshoot=%.2f\tstep%d_lat_g=%.3f\tstep%d_slip=%.1f\tstep%d_spun=%d",
                k ? 80 : 50, R.step[k].rise, k ? 80 : 50, R.step[k].overshoot, k ? 80 : 50, R.step[k].lat_g,
                k ? 80 : 50, R.step[k].slip, k ? 80 : 50, R.step[k].spun);
    fprintf(f, "\twall=%.1f\n", wall);
    fclose(f);
}

void tune_run() {
    if (g_test) {
        g_test_mode = true;
        tune_handling();
        g_test_mode = false;
        return;
    }
    void* lounge = *g_lounge;
    for (int s = 0; s < 8; s++) {
        const int n = LOUNGE_VFN(lounge, 0, int, int)(lounge, 0, s);
        const char* sn = GetAIStrengthString(s);
        logf("tune: strength %d (%s): %d drivers", s, sn ? sn : "?", n);
    }
    const int slot = track_slot();
    if (slot < 0) {
        logf("tune: nothing run -- track '%s' isn't a slot 0..7 or a track's name", g_track);
        return;
    }
    const int count = g_strength >= 0 && g_strength <= 2 ? LOUNGE_VFN(lounge, 0, int, int)(lounge, 0, g_strength) : 0;
    if (g_driver < 0 || g_driver >= count) {
        logf("tune: nothing run -- strength %d has %d drivers (driver %d asked; strengths 0 Easy, 1 Normal, 2 Hard)",
             g_strength, count, g_driver);
        return;
    }
    // one race a start: a second race in the same start doesn't repeat the first (something carries over), so a tune
    // race always runs in a fresh game
    tune_race(slot, g_strength, g_driver);
}
