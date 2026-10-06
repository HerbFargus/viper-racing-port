// world_snd_mgr.cpp -- sound stage S1 group A's rewrites (hook/snd_mgr.cpp: the sound objects, the SoundManager,
// SoundBegin / End, the sound resources) against the originals, on random worlds, outside the game (docs/PORTING.md,
// checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_snd_mgr.cpp
//        /Fo<dir>\ /Fe<dir>\world_snd_mgr.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_snd_mgr.exe [worlds] [seed]        (VP_TRACE=1: one line per world)
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. Every world is a
// SoundManager built in an arena the way the game leaves one: its 0x600-byte sound table, 0..200 sounds (Sound, Sound3D,
// SoundDash, the game's own vtables, so their virtuals are the original code) sorted by class into the 8 per-class runs,
// the two listener buffers, the destroy lists, a pending async create; and group C's side faked: every sound's ISound
// is a recording fake (a vtable of loggers holding a status that Play / Stop / Mute / UnMute change, a Can3D answer),
// and the manager's IMixer is one too (GetCaps from a random MixerCaps, Begin's answer scripted, CreateSound handing out
// fresh fake ISounds or NULL, Update logging the listener it's given). Then everything is randomised: flags, volumes,
// frequencies, pans, positions and velocities (near, far, within 10 m, within 0.1 m, on the listener), the listener's
// frame (upside down too), the focus car, the mixer quality (0..2, as MixerSetQuality clamps it), the threads (main or
// BG task), and a share of the floats set to extreme values (NaN, infinities, denormals, huge).
//
// Stubbed (a jump to a logger): MemAlloc (a bump heap in the arena, sometimes NULL), operator delete, LogPanic,
// LogReport, TaskSleep (logs, and runs one BG tick -- the ORIGINAL SoundManager::Update, as the BG thread would --
// so the main thread's waits end; a world that would wait forever raises an exception after 64 sleeps), TaskGetID (the
// world's thread), MultiBegin / MultiEnter / MultiLeave / MultiEnd, BGHook / BGUnhook, MixerBegin / MixerEnd /
// MixerGet / MixerGetDefault / MixerGetQuality, ResourceGet (a scripted 'SFX0' in the arena, or none, old format or
// new) / ResourceForget, EngineSound's constructor and destructor (group B's). Everything else runs as the game's
// code: strncpy, memmove, MatrixMulPoint, WorldGetFocusCar, this group's own functions where another calls them.
//
// Each world picks a function (or a sequence: frames of update() with the owners poking their sounds between them);
// the arena and the image's whole .data/.bss are saved, the original runs, the result is kept, the world restored,
// the rewrite runs; the arena, .data/.bss, the return value and the stubs' logs (every virtual call on the fakes, with
// its arguments' bits) are compared, and every byte the original changed must lie in the rewrite's footprint unless
// it's replay_only (no statics are exempt: the physics-thread save doesn't cover the sound library). The FPU runs at a
// random precision (24 or 53 bits), in 30% of the worlds with overflow and divide-by-zero unmasked (docs/PORTING.md
// 10a); each pass starts and ends with fninit. The game's CRT fatal paths and message box are stubbed.
//
// Not covered: SoundClassString with a negative class (reads the original's stack: a FIX CANDIDATE); real DirectSound
// (group C's; update()'s mixer frame is the fake's Update here).
//
// Fixes (docs/PORTING.md, "Fixes"): built as above plus /DVP_SND_FIXES, the rewrites have their fixes on. A world in
// which a fix branch fires is counted, not compared (it is meant to differ); every other world must still match the
// original bit for bit. Then directed tests run each fix on the input that used to fail (a divide-by-zero, a NULL
// manager, a lock left held, a garbage SoundDash flag, a short old-format resource, a full or lifted sound table, a
// class outside 0..7, a 33rd sound to destroy when stopped) and check what it does now, and that the inputs beside it
// still give the original's bits.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

#ifndef VP_SND_FIXES                // (built with /DVP_SND_FIXES: the fixes on, and tested -- see main)
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#endif
static int g_fix_fired;             // fix branches the rewrite took in this pass
#define SNDM_FIX_FIRED() (++g_fix_fired)
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

#include "../hook/snd_mgr.cpp"

// ---- random numbers --------------------------------------------------------------------------------------
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float wildf() {
    uint32_t r = rnd(), k = r % 12;
    float s = (r & 0x100) ? -1.0f : 1.0f;
    switch (k) {
    case 0: return bitsf(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return bitsf(0x7f800001u | (rnd() & 0x3fffff));        // a signalling NaN
    case 2: return s * INFINITY;
    case 3: return s * range(1e30f, 3e38f);
    case 4: return s * range(1e-38f, 1e-30f);
    case 5: return bitsf(rnd() & 0x807fffff);                      // a denormal
    case 6: return (r & 0x200) ? 0.0f : -0.0f;
    case 7: return s * range(1e6f, 1e9f);
    case 8: return s;
    default: return bitsf(rnd());
    }
}
static void setf(void* p, float f) { memcpy(p, &f, 4); }
static void wild_at(void* p) { setf(p, wildf()); }

// ---- the original, loaded at 0x400000 ---------------------------------------------------------------------
static bool load_race_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* file = (uint8_t*)malloc(n);
    fread(file, 1, n, f);
    fclose(f);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(file + ((IMAGE_DOS_HEADER*)file)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x362de68c) { printf("%s isn't the v1.0 race.exe\n", path); return false; }
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, nt->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) { printf("0x400000 isn't reserved for race.exe in this process\n"); return false; }
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        memcpy(base + sec[i].VirtualAddress, file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData
                                                                                         : sec[i].Misc.VirtualSize);
    free(file);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_WORLD_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) { printf("can't start the test process\n"); return 2; }
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the world: an arena, and the image's .data/.bss ------------------------------------------------------------
enum : uint32_t {
    ARENA_BYTES = 0x20000,
    A_HEAP = 0x00000, A_HEAP_END = 0x08000,  // MemAlloc's bump heap
    A_MGR = 0x08000,                         // the SoundManager (0x180)
    A_MGR2 = 0x08200,                        // a constructor's target (0x180)
    A_LIST = 0x08400,                        // its sound table (0x600, and room past it)
    A_SOUNDS = 0x09000,                      // sounds, 0x40 apart (256)
    A_ISOUNDS = 0x0d000,                     // fake ISounds, 0x40 apart: one per sound, then the mixer's pool
    A_ISVT = 0x15000,                        // the fake ISound vtable (13 slots)
    A_MXVT = 0x15040,                        // the fake IMixer vtable (7 slots)
    A_MIXER = 0x15080,                       // the fake IMixer (0x10)
    A_CAPS = 0x150a0,                        // its MixerCaps (12)
    A_OBJ = 0x15100,                         // a sound constructor's target (0x100)
    A_P3 = 0x15400,                          // Sound3D positions and velocities (24 bytes per sound)
    A_ARGS = 0x17000,                        // arguments: a Listener, its frame, its velocity, names
    A_RES = 0x18000,                         // ResourceGet's 'SFX0'
};
enum { MAX_SOUNDS = 256, N_POOL = 64 };
enum : uint32_t { G_LIST = 0x000, G_FRAME = 0x040, G_VEL = 0x080, G_NAME = 0x100, G_NAME2 = 0x140 };
static uint8_t* g_arena;
static uint8_t* g_arena_snap;
static uint8_t* g_arena_after;
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_snap;
static uint8_t* g_data_after;
static uint8_t* AR(uint32_t off) { return g_arena + off; }
static uint8_t* ARG(uint32_t off) { return g_arena + A_ARGS + off; }
static uint8_t* sound_at(int i) { return AR(A_SOUNDS) + 0x40 * i; }
static uint8_t* isound_at(int i) { return AR(A_ISOUNDS) + 0x40 * i; }
static uint8_t* MGR() { return AR(A_MGR); }

// ---- the stubs' log and script ------------------------------------------------------------------------------------
enum { LOG_MAX = 32768 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static bool in_arena(const void* p, uint32_t n) { return (const uint8_t*)p >= g_arena && (const uint8_t*)p + n <= g_arena + ARENA_BYTES; }
static bool readable(const void* p) { return in_arena(p, 16) || ((uint32_t)p >= 0x401000 && (uint32_t)p < 0x5d0000); }
static void log_str(const char* s) {                       // at most 16 characters, where the pointer can be read
    if (!readable(s)) { log_word('BADS'); log_word((uint32_t)s); return; }
    uint32_t w = 0;
    int i = 0;
    for (; i < 16 && s[i]; i++) { w = w * 31 + (uint8_t)s[i]; }
    log_word((uint32_t)i); log_word(w);
}

struct Script {
    uint32_t alloc_fail;           // bit per MemAlloc call: NULL
    uint32_t create_fail;          // bit per IMixer::CreateSound call: NULL
    uint8_t begin_ok;              // IMixer::Begin's answer
    int32_t quality;               // MixerGetQuality
    uint8_t res_found;             // ResourceGet finds it
    uint32_t res_version;          // its version (0: old format, truncated)
    uint32_t engine_ok;            // EngineSound's +0xf4 after its constructor
    uint8_t can3d_new;             // Can3D of the ISounds the mixer creates
    int32_t task;                  // TaskGetID: 1 the main thread, 2 the BG task
};
static Script g_script;
static uint8_t* g_heap;             // the bump pointer
static uint8_t* g_heap_mark;        // where a pass starts
static int g_alloc_calls, g_create_calls, g_pool_next, g_sleeps;
static int32_t g_task;
static bool g_in_tick;
enum { BG_TASK = 2, MAIN_TASK = 1 };

static void* __cdecl stub_MemAlloc(int n) {
    log_word('MALC'); log_word((uint32_t)n);
    const int k = g_alloc_calls++;
    if (k < 32 && ((g_script.alloc_fail >> k) & 1)) { log_word(0); return 0; }
    if (n < 0 || (uint32_t)n > (uint32_t)(AR(A_HEAP_END) - g_heap)) { log_word(1); return 0; }
    uint8_t* p = g_heap;
    g_heap += ((uint32_t)n + 7) & ~7u;
    log_word((uint32_t)(p - g_arena));
    return p;
}
static void __cdecl stub_delete(void* p) { log_word('DELE'); log_word((uint32_t)p); }
// the formats' own arguments only (what follows them on the stack differs between the passes)
static void log_fmt_args(const char* fmt, va_list ap) {
    switch ((uint32_t)fmt) {
    case 0x004f5b20: log_word(va_arg(ap, uint32_t)); log_word(va_arg(ap, uint32_t)); break;   // playing > max_sounds
    case 0x004f5b5c: log_word(va_arg(ap, uint32_t)); log_word(va_arg(ap, uint32_t)); log_word(va_arg(ap, uint32_t)); break;
    case 0x004f5b7c: case 0x004f65f8: case 0x004f65d8: log_word(va_arg(ap, uint32_t)); break;   // a name
    default: break;
    }
}
static void __cdecl stub_panic(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_word('PANC'); log_word((uint32_t)fmt); log_fmt_args(fmt, ap);
    va_end(ap);
}
static void __cdecl stub_report(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_word('REPT'); log_word((uint32_t)fmt); log_fmt_args(fmt, ap);
    va_end(ap);
}
static int __cdecl stub_task_id() { log_word('TGID'); log_word((uint32_t)g_task); return g_task; }
static void __cdecl stub_sleep(int ms) {
    log_word('SLEP'); log_word((uint32_t)ms);
    if (++g_sleeps > 64) RaiseException(0xe0000001u, 0, 0, 0);          // it would wait forever
    if (g_in_tick) return;
    g_in_tick = true;                                                  // the BG thread gets a tick meanwhile
    const int32_t t = g_task;
    g_task = BG_TASK;
    ((void(__cdecl*)())0x00473e60)();                                  // SoundManager::Update, the original
    g_task = t;
    g_in_tick = false;
}
static int __cdecl stub_multi_begin(const char* name) { log_word('MBGN'); log_str(name); return 0x55; }
static void __cdecl stub_multi_enter(int id, const char* f, int l) { log_word('MENT'); log_word((uint32_t)id); log_word((uint32_t)f); log_word((uint32_t)l); }
static void __cdecl stub_multi_leave(int id, const char* f, int l) { log_word('MLEV'); log_word((uint32_t)id); log_word((uint32_t)f); log_word((uint32_t)l); }
static void __cdecl stub_multi_end(int id, const char* f, int l) { log_word('MEND'); log_word((uint32_t)id); log_word((uint32_t)f); log_word((uint32_t)l); }
static void __cdecl stub_bghook(uint32_t fn, int prio) { log_word('BGHK'); log_word(fn); log_word((uint32_t)prio); }
static void __cdecl stub_bgunhook(uint32_t fn) { log_word('BGUN'); log_word(fn); }
static void __cdecl stub_mixer_begin(uint8_t b) { log_word('XBEG'); log_word(b); }
static void __cdecl stub_mixer_end(uint8_t b) { log_word('XEND'); log_word(b); }
static void* __cdecl stub_mixer_get(int i) { log_word('XGET'); log_word((uint32_t)i); return AR(A_MIXER); }
static int __cdecl stub_mixer_default() { log_word('XDEF'); return 0; }
static int32_t __cdecl stub_mixer_quality() { log_word('XQUA'); return g_script.quality; }
static void* __cdecl stub_resource_get(const char* name, uint32_t type, uint32_t* ver, int* p4, uint8_t* p5, uint8_t* p6) {
    log_word('RGET'); log_str(name); log_word(type); log_word((uint32_t)p4); log_word((uint32_t)p6);
    *ver = g_script.res_version;
    if (p5) *p5 = 0;
    return g_script.res_found ? AR(A_RES) : 0;
}
static uint8_t __cdecl stub_resource_forget(void* p) { log_word('RFGT'); log_word((uint32_t)p); return 1; }
static void* __fastcall stub_engine_ctor(uint8_t* self, int, void* car) {
    log_word('ENGC'); log_word((uint32_t)self); log_word((uint32_t)car);
    *(uint32_t*)(self + 0xf4) = g_script.engine_ok;
    return self;
}
static void __fastcall stub_engine_dtor(uint8_t* self, int) { log_word('ENGD'); log_word((uint32_t)self); }

// ---- the fakes: group C's ISound and IMixer ---------------------------------------------------------------------------
// ISound: +4 int status (GetStatus), +8 u8 looped, +0x10 frequency bits, +0x14 volume bits, +0x18 u8 Can3D,
// +0x1c pan bits, +0x20 / +0x24 Set3D's pointers, +0x28 calls, +0x3c 0xdead when deleted
static void is_log(uint32_t tag, uint8_t* s) { log_word(tag); log_word((uint32_t)s); }
static void* __fastcall is_dtor(uint8_t* s, int, uint32_t fl) { is_log('IDEL', s); log_word(fl); *(uint32_t*)(s + 0x3c) = 0xdead; return s; }
static uint8_t __fastcall is_ok(uint8_t* s, int) { is_log('IOK ', s); return 1; }
static void __fastcall is_play(uint8_t* s, int) { is_log('PLAY', s); *(int32_t*)(s + 4) = 1; s[8] = 0; }
static void __fastcall is_playloop(uint8_t* s, int) { is_log('PLLP', s); *(int32_t*)(s + 4) = 1; s[8] = 1; }
static void __fastcall is_stop(uint8_t* s, int) { is_log('STOP', s); *(int32_t*)(s + 4) = 3; }
static int32_t __fastcall is_status(uint8_t* s, int) { is_log('GSTA', s); return *(int32_t*)(s + 4); }
static void __fastcall is_mute(uint8_t* s, int) { is_log('MUTE', s); if (*(int32_t*)(s + 4) == 1) *(int32_t*)(s + 4) = 2; }
static void __fastcall is_unmute(uint8_t* s, int) { is_log('UNMU', s); if (*(int32_t*)(s + 4) == 2) *(int32_t*)(s + 4) = 1; }
static void __fastcall is_freq(uint8_t* s, int, uint32_t b) { is_log('SFRQ', s); log_word(b); *(uint32_t*)(s + 0x10) = b; }
static void __fastcall is_vol(uint8_t* s, int, uint32_t b) { is_log('SVOL', s); log_word(b); *(uint32_t*)(s + 0x14) = b; }
static uint8_t __fastcall is_can3d(uint8_t* s, int) { is_log('CN3D', s); return s[0x18]; }
static void __fastcall is_pan(uint8_t* s, int, uint32_t b) { is_log('SPAN', s); log_word(b); *(uint32_t*)(s + 0x1c) = b; }
static void __fastcall is_set3d(uint8_t* s, int, uint32_t v, uint32_t p) { is_log('S3D ', s); log_word(v); log_word(p); *(uint32_t*)(s + 0x20) = v; *(uint32_t*)(s + 0x24) = p; }
static void init_isound(uint8_t* s, int32_t status, uint8_t can3d) {
    memset(s, 0, 0x40);
    *(uint8_t**)s = AR(A_ISVT);
    *(int32_t*)(s + 4) = status;
    s[0x18] = can3d;
}
// IMixer: +4 Update calls
static const char* __fastcall mx_name(uint8_t*, int) { return "fake"; }
static void* __fastcall mx_caps(uint8_t* m, int) { log_word('CAPS'); log_word((uint32_t)m); return AR(A_CAPS); }
static uint8_t __fastcall mx_isnull(uint8_t*, int) { log_word('ISNL'); return 0; }
static uint8_t __fastcall mx_begin(uint8_t* m, int) { log_word('BEGN'); log_word((uint32_t)m); return g_script.begin_ok; }
static void* __fastcall mx_create(uint8_t* m, int, const char* name, int loc) {
    log_word('CRSN'); log_word((uint32_t)m); log_str(name); log_word((uint32_t)loc);
    const int k = g_create_calls++;
    if (k < 32 && ((g_script.create_fail >> k) & 1)) { log_word(0); return 0; }
    if (g_pool_next >= N_POOL) { log_word(1); return 0; }
    uint8_t* s = isound_at(MAX_SOUNDS + g_pool_next++);
    init_isound(s, 3, g_script.can3d_new);
    log_word((uint32_t)s);
    return s;
}
static void __fastcall mx_update(uint8_t* m, int, const uint8_t* l) {
    log_word('MUPD'); log_word((uint32_t)m); log_word(in_arena(l, 12) ? (uint32_t)l : 'STAK');   // (update's is a local)
    (*(uint32_t*)(m + 4))++;
    if (!l) return;                                                    // (always a live Listener)
    const uint32_t* w = (const uint32_t*)l;
    log_word(w[0]); log_word(w[1]); log_word(w[2]);
    if (readable((void*)w[1])) for (int i = 0; i < 12; i++) log_word(((const uint32_t*)w[1])[i]);
    if (readable((void*)w[2])) for (int i = 0; i < 3; i++) log_word(((const uint32_t*)w[2])[i]);
}
static void __fastcall mx_end(uint8_t* m, int) { log_word('XEND'); log_word((uint32_t)m); }

// the game's CRT must never put up a modal box: its fatal paths end the test quietly
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- building the world -----------------------------------------------------------------------------------------
enum : uint32_t { X_FOCUS = 0x004f4358 };
struct World {
    int n;                         // sounds in the table
    int nstray;                    // sounds made but not in it (after the n)
    int cls_of[MAX_SOUNDS];
};
static World g_w;
static const uint32_t k_vts[3] = {VT_SOUND, VT_SOUND3D, VT_SOUNDDASH};
static void rnd_frame(float* f, float spread) {
    // a random rotation (a unit quaternion's matrix), sometimes the identity; upside down sometimes
    float q[4], n = 0;
    for (int i = 0; i < 4; i++) { q[i] = range(-1.0f, 1.0f); n += q[i] * q[i]; }
    n = sqrtf(n) + 1e-6f;
    for (int i = 0; i < 4; i++) q[i] /= n;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                        2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    for (int i = 0; i < 9; i++) f[i] = m[i];
    if (chance(8)) { for (int i = 0; i < 9; i++) f[i] = 0; f[0] = f[4] = f[8] = 1.0f; }
    if (chance(5)) f[4] = -fabsf(f[4]);                                 // upside down
    if (chance(3)) f[4] = -0.0f;
    for (int i = 9; i < 12; i++) f[i] = range(-spread, spread);
}
static void make_sound(int i, int cls, uint32_t vt) {
    uint8_t* s = sound_at(i);
    memset(s, 0xcd, 0x40);
    *(uint32_t*)s = vt;
    setf(s + 4, chance(80) ? range(0.5f, 2.0f) : range(0.0f, 20.0f));
    setf(s + 8, chance(20) ? range(0.0f, 0.06f) : chance(5) ? -range(0.0f, 1.0f) : range(0.0f, 1.0f));
    if (chance(4)) *(uint32_t*)(s + 8) = 0x3d4ccccd + (rnd() % 3) - 1;       // at CanBeHeard's threshold
    uint8_t* is = isound_at(i);
    const uint32_t k = rnd() % 20;
    init_isound(is, k < 7 ? 1 : k < 10 ? 2 : k < 19 ? 3 : (int32_t)(rnd() % 6) - 1, chance(10));
    *(uint8_t**)(s + 0xc) = is;
    *(int32_t*)(s + 0x10) = chance(80) ? (int32_t)(rnd() % 8) : -100;
    sprintf((char*)s + 0x14, "snd%03d.sfx", i);
    if (chance(5)) memcpy(s + 0x14, "twelve_chars", 12);
    s[0x20] = 0;
    *(int32_t*)(s + 0x24) = cls;
    for (int f = 0x28; f <= 0x2d; f++) s[f] = chance(30) ? 1 : 0;
    if (chance(3)) s[0x28 + rnd() % 6] = (uint8_t)rnd();
    if (vt == VT_SOUND3D) {
        s[0x30] = chance(70) ? 1 : 0;
        setf(s + 0x34, chance(80) ? range(0.0f, 1.0f) : range(0.0f, 0.1f));
        uint8_t* p = AR(A_P3) + 24 * i;
        *(uint8_t**)(s + 0x38) = p + 12;
        *(uint8_t**)(s + 0x3c) = p;
    } else {
        setf(s + 0x30, range(-1.0f, 1.0f));
        s[0x34] = chance(50) ? 1 : 0;
        s[0x38] = (uint8_t)(chance(80) ? rnd() % 2 : rnd());
    }
}
// Sound3D positions around the listener
static void place_sounds() {
    const uint8_t* mgr = MGR();
    const int32_t idx = *(int32_t*)(mgr + 0x50) == 1 ? 1 : 0;
    const float* lp = (const float*)(mgr + 0x58 + 0x40 * idx + 0x24);
    for (int i = 0; i < g_w.n + g_w.nstray; i++) {
        float* p = (float*)(AR(A_P3) + 24 * i);
        const uint32_t k = rnd() % 20;
        const float r = k < 8 ? range(0.0f, 200.0f) : k < 12 ? range(150.0f, 400.0f) : k < 16 ? range(0.0f, 10.0f)
                      : k < 18 ? range(0.0f, 0.12f) : 0.0f;
        const float a = range(-3.2f, 3.2f), e = range(-0.5f, 0.5f);
        p[0] = lp[0] + r * cosf(a) * cosf(e); p[1] = lp[1] + r * sinf(e); p[2] = lp[2] + r * sinf(a) * cosf(e);
        if (k == 19 && chance(50)) { p[0] = lp[0]; p[1] = lp[1]; p[2] = lp[2]; }
        for (int c = 3; c < 6; c++) p[c] = chance(80) ? range(-60.0f, 60.0f) : range(-400.0f, 400.0f);
    }
}
static void build_tables(int count_override = -1) {
    uint8_t* mgr = MGR();
    uint8_t** list = (uint8_t**)AR(A_LIST);
    int at = 0;
    for (int k = 0; k < 8; k++) {
        *(uint8_t***)(mgr + 0xc + 8 * k) = list + at;
        int c = 0;
        for (int i = 0; i < g_w.n; i++) if (g_w.cls_of[i] == k) { list[at++] = sound_at(i); c++; }
        *(int32_t*)(mgr + 0x10 + 8 * k) = c;
    }
    *(int32_t*)(mgr + 8) = at;
    if (count_override > at) {                                    // a full table: the last class padded with a repeat
        const int extra = count_override - at;
        for (int i = 0; i < extra; i++) list[at + i] = g_w.n ? sound_at(0) : AR(A_OBJ);
        *(int32_t*)(mgr + 8) = count_override;
        *(int32_t*)(mgr + 0x10 + 8 * 7) += extra;
    }
}
static void random_world() {
    memset(g_arena, 0, ARENA_BYTES);
    memset(AR(A_HEAP), 0xcd, A_HEAP_END - A_HEAP);
    memset(AR(A_OBJ), 0xcd, 0x100);
    memset(AR(A_MGR2), 0xcd, 0x180);
    g_heap = AR(A_HEAP);
    g_heap_mark = g_heap;
    // the fakes' vtables and the mixer
    void** vt = (void**)AR(A_ISVT);
    void* isfn[13] = {(void*)&is_dtor, (void*)&is_ok, (void*)&is_play, (void*)&is_playloop, (void*)&is_stop, (void*)&is_status,
                      (void*)&is_mute, (void*)&is_unmute, (void*)&is_freq, (void*)&is_vol, (void*)&is_can3d, (void*)&is_pan,
                      (void*)&is_set3d};
    for (int i = 0; i < 13; i++) vt[i] = isfn[i];
    void** mvt = (void**)AR(A_MXVT);
    void* mxfn[7] = {(void*)&mx_name, (void*)&mx_caps, (void*)&mx_isnull, (void*)&mx_begin, (void*)&mx_create,
                     (void*)&mx_update, (void*)&mx_end};
    for (int i = 0; i < 7; i++) mvt[i] = mxfn[i];
    *(uint8_t**)AR(A_MIXER) = AR(A_MXVT);
    uint8_t* caps = AR(A_CAPS);
    caps[0] = (uint8_t)(chance(60) ? 1 : chance(50) ? 0 : rnd());
    *(int32_t*)(caps + 4) = chance(50) ? 0 : (int32_t)(rnd() % 4);
    *(int32_t*)(caps + 8) = chance(50) ? 0 : 1;
    // the script
    g_script.alloc_fail = chance(8) ? rnd() & rnd() : 0;
    g_script.create_fail = chance(15) ? rnd() & rnd() : 0;
    g_script.begin_ok = chance(85) ? 1 : 0;
    g_script.quality = (int32_t)(rnd() % 3);                    // (MixerSetQuality clamps it: 3 would hang mute_sounds)
    g_script.res_found = chance(90);
    g_script.res_version = chance(30) ? 0 : 1;
    g_script.engine_ok = chance(85) ? 1 : 0;
    g_script.can3d_new = chance(10);
    g_script.task = chance(70) ? BG_TASK : MAIN_TASK;
    // the sounds
    const int n = chance(80) ? (int)(rnd() % 24) : chance(80) ? (int)(rnd() % 80) : (int)(rnd() % 200);
    g_w.n = n;
    g_w.nstray = 8;
    for (int i = 0; i < n + g_w.nstray; i++) {
        const uint32_t k = rnd() % 16;
        const int cls = k < 2 ? 0 : k < 3 ? 1 : k < 6 ? 2 : k < 10 ? 3 : k < 12 ? 4 : k < 13 ? 5 : k < 14 ? 6 : 7;
        g_w.cls_of[i] = cls;
        const bool is3d = cls >= 2 && cls != 6;
        const uint32_t vtb = is3d && chance(75) ? VT_SOUND3D : chance(30) ? VT_SOUNDDASH : chance(80) ? VT_SOUND : k_vts[rnd() % 3];
        make_sound(i, cls, vtb);
    }
    // the manager
    uint8_t* mgr = MGR();
    memset(mgr, 0, 0x180);
    mgr[0] = chance(90) ? 1 : 0;
    mgr[1] = chance(30) ? 1 : 0;
    *(uint8_t**)(mgr + 4) = AR(A_LIST);
    build_tables();
    mgr[0x4c] = chance(20) ? 1 : 0;
    *(int32_t*)(mgr + 0x50) = chance(8) ? -1 : (int32_t)(rnd() % 2);
    for (int b = 0; b < 2; b++) {
        uint8_t* li = mgr + 0x54 + 0x40 * b;
        *(int32_t*)li = chance(85) ? (int32_t)(rnd() % 8) : -100;
        rnd_frame((float*)(li + 4), 500.0f);
        for (int c = 0; c < 3; c++) setf(li + 0x34 + 4 * c, chance(80) ? range(-60.0f, 60.0f) : range(-300.0f, 300.0f));
    }
    *(uint8_t**)(mgr + 0xd4) = AR(A_MIXER);
    int nd = chance(60) ? (int)(rnd() % 6) : chance(90) ? (int)(rnd() % 33) : 32;
    if (nd > n + g_w.nstray) nd = n + g_w.nstray;
    {                                                   // distinct sounds (Toss makes a new one each time)
        int order[MAX_SOUNDS], m = n + g_w.nstray;
        for (int i = 0; i < m; i++) order[i] = i;
        for (int i = m - 1; i > 0; i--) { const int j = (int)(rnd() % (uint32_t)(i + 1)); const int t = order[i]; order[i] = order[j]; order[j] = t; }
        for (int i = 0; i < nd && i < 32; i++) *(uint8_t**)(mgr + 0xd8 + 4 * i) = sound_at(order[i % m]);
    }
    *(int32_t*)(mgr + 0x158) = nd;
    if (chance(20)) {
        uint8_t* is = isound_at(MAX_SOUNDS + N_POOL);
        init_isound(is, 3, 0);
        *(uint8_t**)(mgr + 0x15c) = chance(90) ? is : 0;
        *(int32_t*)(mgr + 0x160) = 1;
    }
    if (chance(15)) {
        strcpy((char*)ARG(G_NAME2), "async.sfx");
        *(uint8_t**)(mgr + 0x164) = ARG(G_NAME2);
        *(int32_t*)(mgr + 0x168) = (int32_t)(rnd() % 8);
        const bool done = chance(30);                   // requested, or created and not yet collected
        *(uint8_t**)(mgr + 0x16c) = done && chance(90) ? isound_at(MAX_SOUNDS + N_POOL + 1) : 0;
        if (done) init_isound(isound_at(MAX_SOUNDS + N_POOL + 1), 3, 0);
        mgr[0x170] = done ? 1 : 0;
    }
    mgr[0x174] = chance(10) ? 1 : 0;
    *(int32_t*)(mgr + 0x178) = 0x55;
    *(int32_t*)(mgr + 0x17c) = chance(85) ? BG_TASK : chance(50) ? 0 : MAIN_TASK;
    place_sounds();
    // the statics
    *(uint8_t**)S_MGR = mgr;
    *(uint8_t**)S_GLOBAL = mgr;
    const uint32_t k = rnd() % 20;
    *(uint32_t*)S_UPDATE_F = k < 15 ? F_MGR_UPDATE : k < 16 ? 0 : k < 17 ? F_MIXER_BEGIN : k < 18 ? F_MIXER_END : F_DESTROY_ISOUNDS;
    *(uint8_t*)S_RESTART = (uint8_t)(chance(80) ? 0 : chance(50) ? 1 : rnd());
    *(int32_t*)X_FOCUS = n && chance(50) ? *(int32_t*)(sound_at((int)(rnd() % n)) + 0x10) : (int32_t)(rnd() % 8);
    // the resource
    *(int32_t*)AR(A_RES) = chance(20) ? (int32_t)(rnd() % 0x800) : (int32_t)(rnd() % 0x10000);
    // a listener argument, from the manager's or its own
    uint8_t* la = ARG(G_LIST);
    if (chance(60)) {
        const int b = (int)(rnd() % 2);
        *(int32_t*)la = *(int32_t*)(mgr + 0x54 + 0x40 * b);
        *(uint8_t**)(la + 4) = mgr + 0x58 + 0x40 * b;
        *(uint8_t**)(la + 8) = mgr + 0x88 + 0x40 * b;
    } else {
        *(int32_t*)la = chance(85) ? (int32_t)(rnd() % 8) : -100;
        rnd_frame((float*)ARG(G_FRAME), 500.0f);
        for (int c = 0; c < 3; c++) setf(ARG(G_VEL) + 4 * c, range(-80.0f, 80.0f));
        *(uint8_t**)(la + 4) = ARG(G_FRAME);
        *(uint8_t**)(la + 8) = ARG(G_VEL);
    }
    strcpy((char*)ARG(G_NAME), chance(80) ? "engine01.sfx" : "a_rather_long_name.sfx");
}
static void wildify() {
    const int n = 1 + rnd() % 4;
    uint8_t* mgr = MGR();
    for (int i = 0; i < n; i++) {
        const int w = rnd() % 6;
        const int tot = g_w.n + g_w.nstray;
        if (w == 0 && tot) { uint8_t* s = sound_at((int)(rnd() % tot)); static const uint8_t offs[] = {4, 8, 0x30, 0x34}; wild_at(s + offs[rnd() % 4]); }
        else if (w == 1 && tot) wild_at(AR(A_P3) + 24 * (rnd() % tot) + 4 * (rnd() % 6));
        else if (w == 2) wild_at(mgr + 0x58 + 0x40 * (rnd() % 2) + 4 * (rnd() % 15));
        else if (w == 3) wild_at(ARG(G_FRAME) + 4 * (rnd() % 12));
        else if (w == 4) wild_at(ARG(G_VEL) + 4 * (rnd() % 3));
        else if (tot) { uint8_t* s = sound_at((int)(rnd() % tot)); if (*(uint32_t*)s == VT_SOUND3D) wild_at(s + 0x34); }
    }
}

// ---- the functions under test --------------------------------------------------------------------------------------
#define KINDS(X)                                                                                                         \
    X(K_STATIC_INIT, "$E initialisers", 30) X(K_SOUND_BEGIN, "SoundBegin", 30) X(K_SOUND_RESTART, "SoundRestart", 20)    \
    X(K_SOUND_END, "SoundEnd", 20) X(K_MUTE_CARS, "SoundMuteCars", 10) X(K_UNMUTE_CARS, "SoundUnMuteCars", 10)          \
    X(K_FLUSH_ASYNC, "SoundFlushAsync", 20) X(K_SET_LISTENER_C, "SoundSetListener", 15) X(K_STOP_CAR_C, "SoundStopCar", 15) \
    X(K_CLASS_STRING, "SoundClassString", 10) X(K_CLICK, "SoundClick", 20) X(K_SB_CTOR, "SoundBase::SoundBase", 15)      \
    X(K_SB_DTOR, "SoundBase::~SoundBase", 30) X(K_SB_CANHEAR, "SoundBase::CanBeHeard", 20)                               \
    X(K_SB_UPDSTATUS, "SoundBase::UpdateStatus", 30) X(K_SB_UPDSOUND, "SoundBase::UpdateSound", 60)                      \
    X(K_SB_UPD, "SoundBase::update_sound", 20) X(K_S3_UPDSTATUS, "Sound3D::update_status", 60)                           \
    X(K_ATTEN, "attenuation", 20) X(K_S3_UPD, "Sound3D::update_sound", 100) X(K_S3_CTOR, "Sound3D::Sound3D", 30)         \
    X(K_S3_CANHEAR, "Sound3D::CanBeHeard", 20) X(K_S_CTOR, "Sound::Sound", 30) X(K_S_UPD, "Sound::update_sound", 20)     \
    X(K_SD_CTOR, "SoundDash::SoundDash", 20) X(K_SD_UPD, "SoundDash::update_sound", 20)                                  \
    X(K_SD_CANHEAR, "SoundDash::CanBeHeard", 15) X(K_TOSS, "Sound::Toss", 30) X(K_SD_CREATE, "SoundDash::Create", 20)    \
    X(K_S3_CREATE, "Sound3D::Create", 20) X(K_S_CREATE, "Sound::Create", 20) X(K_ENGINE_CREATE, "EngineSound::Create", 15) \
    X(K_SB_UPDSTATUS_BASE, "SoundBase::update_status", 5) X(K_SDTOR, "the deleting destructors", 40)                     \
    X(K_UPDATE_STATIC, "SoundManager::Update", 40) X(K_TABLE_CTOR, "SoundManager::SoundTable::SoundTable", 5)            \
    X(K_SM_CTOR, "SoundManager::SoundManager", 30) X(K_VERIFY_ENT, "verify_entitlements", 3)                             \
    X(K_MIXER_BEGIN, "SoundManager::mixer_begin", 10) X(K_MIXER_END, "SoundManager::mixer_end", 10)                      \
    X(K_FG_MIXER_BEGIN, "SoundManager::fg_mixer_begin", 15) X(K_FG_MIXER_END, "SoundManager::fg_mixer_end", 15)          \
    X(K_SM_DTOR, "SoundManager::~SoundManager", 30) X(K_ASSERT, "ASSERT_MSG(soundmgr.obj)", 3)                           \
    X(K_STOPCAR, "SoundManager::StopCar", 15) X(K_SETLISTENER, "SoundManager::SetListener", 15)                         \
    X(K_SM_UPDATE, "SoundManager::update", 120) X(K_DESTROY_SOUNDS, "SoundManager::destroy_sounds", 20)                  \
    X(K_DESTROY_ISOUNDS, "SoundManager::destroy_isounds", 15) X(K_CHECK_DESTROY, "SoundManager::check_destroy_lists", 40) \
    X(K_MUTE_SOUNDS, "SoundManager::mute_sounds", 100) X(K_VERIFY_SANITY, "SoundManager::verify_sanity", 20)             \
    X(K_SORT3D, "SoundManager::sort_3D_soundtable", 50) X(K_HAS_PRIORITY, "sound3D_has_priority", 40)                    \
    X(K_SORT_SOUNDS, "SoundManager::sort_sounds", 40) X(K_VERIFY_TABLES, "SoundManager::verify_soundtables", 3)          \
    X(K_ADD_SOUND, "SoundManager::add_sound", 40) X(K_ADD_S, "SoundManager::Add(Sound)", 15)                             \
    X(K_ADD_S3, "SoundManager::Add(Sound3D)", 15) X(K_IS_3D, "SoundManager::soundclass_is_3d", 5)                        \
    X(K_REMOVE, "SoundManager::Remove", 40) X(K_FLUSH, "SoundManager::FlushAsync", 10)                                   \
    X(K_DWS, "SoundManager::DestroyWhenStopped", 20) X(K_CREATE_SOUND, "SoundManager::create_sound", 30)                 \
    X(K_FG_CREATE, "SoundManager::fg_create_sound", 20) X(K_CREATE, "SoundManager::Create", 20)                          \
    X(K_CREATE3D, "SoundManager::Create3D", 20) X(K_DESTROY, "SoundManager::Destroy", 30)                                \
    X(K_LINFO_CTOR, "SoundManager::listener_info::listener_info", 3) X(K_RES_GET, "SoundResourceGet", 20)                \
    X(K_RES_FORGET, "SoundResourceForget", 5) X(K_SEQ_FRAMES, "[frames] update, SetListener, StopCar, MuteCars", 60)
#define X_ENUM(k, name, w) k,
#define X_NAME(k, name, w) name,
#define X_WEIGHT(k, name, w) w,
enum Kind { KINDS(X_ENUM) N_KINDS };
static const char* const kind_names[N_KINDS] = {KINDS(X_NAME)};
static const int kind_weights[N_KINDS] = {KINDS(X_WEIGHT)};

// the static initialisers: the rewrite and the original's address
struct EInit { void(__cdecl* rw)(); uint32_t addr; };
static const EInit k_einits[] = {
    {sound_E1_rw, 0x00471ca0}, {sound_E2_rw, 0x00471c90}, {soundmgr_E1_rw, 0x00473c10}, {soundmgr_E2_rw, 0x00473c00},
    {soundmgr_E5_rw, 0x00473c30}, {soundmgr_E6_rw, 0x00473c20}, {soundmgr_E8_rw, 0x00473c50}, {soundmgr_E9_rw, 0x00473c40},
    {soundmgr_E11_rw, 0x00473c70}, {soundmgr_E12_rw, 0x00473c60}, {soundmgr_E14_rw, 0x00473c90}, {soundmgr_E15_rw, 0x00473c80},
    {soundmgr_E17_rw, 0x00473cb0}, {soundmgr_E18_rw, 0x00473ca0}, {soundmgr_E20_rw, 0x00473cd0}, {soundmgr_E21_rw, 0x00473cc0},
    {soundmgr_E23_rw, 0x00473cf0}, {soundmgr_E24_rw, 0x00473ce0}, {soundmgr_E26_rw, 0x00473d10}, {soundmgr_E27_rw, 0x00473d00},
    {soundmgr_E29_rw, 0x00473d30}, {soundmgr_E30_rw, 0x00473d20}, {soundmgr_E32_rw, 0x00473d50}, {soundmgr_E33_rw, 0x00473d40},
    {soundmgr_E35_rw, 0x00473d70}, {soundmgr_E36_rw, 0x00473d60}, {soundmgr_E38_rw, 0x00473d90}, {soundmgr_E39_rw, 0x00473d80},
    {soundmgr_E41_rw, 0x00473db0}, {soundmgr_E42_rw, 0x00473da0}, {soundmgr_E44_rw, 0x00473dd0}, {soundmgr_E45_rw, 0x00473dc0},
    {soundmgr_E47_rw, 0x00473df0}, {soundmgr_E48_rw, 0x00473de0}, {soundmgr_E50_rw, 0x00473e10}, {soundmgr_E51_rw, 0x00473e00},
    {soundmgr_E55_rw, 0x00473e30}, {soundmgr_E56_rw, 0x00473e20}, {soundmgr_E58_rw, 0x00473e50}, {soundmgr_E59_rw, 0x00473e40},
    {soundres_E1_rw, 0x004771f0}, {soundres_E2_rw, 0x004771e0}};
enum { N_EINITS = sizeof k_einits / sizeof k_einits[0] };
typedef void*(__fastcall* Sdtor_t)(uint8_t*, Edx, uint32_t);
static const Sdtor_t k_sdtor_rw[4] = {SoundBase_sdtor_rw, Sound3D_sdtor_rw, Sound_sdtor_rw, SoundDash_sdtor_rw};
static const uint32_t k_sdtor_addr[4] = {0x00472650, 0x00472670, 0x00472690, 0x004726b0};

struct Args {
    uint8_t* self;                 // a sound, or a target
    uint8_t* p;                    // a second sound / an ISound / a table / the mixer
    const char* name;
    int i, j;
    float f;
    uint32_t u;
};
static Args g_args;
static bool g_dws_full;             // DestroyWhenStopped on a full list (the fixed one destroys the sound at once)
static uint8_t* any_listed() { return g_w.n ? sound_at((int)(rnd() % g_w.n)) : sound_at(g_w.n + (int)(rnd() % g_w.nstray)); }
static uint8_t* any_sound() { return chance(90) ? any_listed() : sound_at(g_w.n + (int)(rnd() % g_w.nstray)); }
static uint8_t* any_of(uint32_t vt) {
    int cand[MAX_SOUNDS], m = 0;
    for (int i = 0; i < g_w.n + g_w.nstray; i++) if (*(uint32_t*)sound_at(i) == vt) cand[m++] = i;
    if (m) return sound_at(cand[rnd() % m]);
    const int i = g_w.n + (int)(rnd() % g_w.nstray);           // make one
    make_sound(i, g_w.cls_of[i], vt);
    return sound_at(i);
}
static int rnd_class() { return chance(92) ? (int)(rnd() % 8) : chance(50) ? -1 : 8 + (int)(rnd() % 2); }
static void random_args(Kind k) {
    memset(&g_args, 0, sizeof g_args);
    g_dws_full = false;
    g_args.name = (const char*)ARG(G_NAME);
    uint8_t* mgr = MGR();
    g_task = g_script.task;
    switch (k) {
    case K_STATIC_INIT: g_args.i = (int)(rnd() % N_EINITS); break;
    case K_SOUND_BEGIN: case K_SOUND_RESTART: case K_SOUND_END:
        if (chance(10)) *(uint8_t**)S_MGR = 0;
        g_task = MAIN_TASK;
        if (k != K_SOUND_BEGIN) *(uint32_t*)S_UPDATE_F = F_MGR_UPDATE;
        mgr[0x174] = 0;
        break;
    case K_MUTE_CARS: case K_UNMUTE_CARS: case K_SET_LISTENER_C: case K_STOP_CAR_C: case K_FLUSH_ASYNC:
        if (chance(2)) *(uint8_t**)S_MGR = 0;                       // (faults, in both)
        g_args.i = chance(80) ? (int)(rnd() % 8) : -100;
        break;
    case K_CLASS_STRING: g_args.i = (int)(rnd() % 20); break;
    case K_IS_3D: g_args.i = (int)(rnd() % 20) - 5; break;
    case K_CLICK: case K_TOSS: case K_S_CREATE: case K_S3_CREATE: case K_SD_CREATE: case K_ENGINE_CREATE:
    case K_S3_CTOR: case K_S_CTOR: case K_SD_CTOR:
        g_args.self = AR(A_OBJ);
        g_args.i = k == K_TOSS && chance(30) ? -(int)(rnd() % 8) : rnd_class();
        g_args.j = chance(80) ? (int)(rnd() % 8) : -100;
        g_args.p = AR(A_P3) + 24 * (rnd() % (g_w.n + g_w.nstray));
        if (chance(90)) { *(uint32_t*)S_UPDATE_F = F_MGR_UPDATE; if (*(int32_t*)(mgr + 0x50) == -1) *(int32_t*)(mgr + 0x50) = 0; }
        *(uint32_t*)(mgr + 0x164) = 0; *(uint32_t*)(mgr + 0x16c) = 0;          // (no other request in flight)
        break;
    case K_SB_CTOR: g_args.self = AR(A_OBJ); g_args.i = (int)rnd(); if (chance(20)) g_args.name = (const char*)ARG(G_NAME2); strcpy((char*)ARG(G_NAME2), "exactly12chr_and_more"); break;
    case K_SB_DTOR: case K_SDTOR:
        g_args.self = chance(85) ? any_listed() : any_sound();
        if (chance(25)) *(uint32_t*)(g_args.self + 0xc) = 0;
        g_args.i = (int)(rnd() % 4);
        g_args.u = chance(45) ? 1 : chance(80) ? 0 : rnd();
        break;
    case K_SB_CANHEAR: case K_SB_UPDSTATUS: case K_SB_UPDSOUND: case K_SB_UPD: case K_S3_CANHEAR: case K_SB_UPDSTATUS_BASE:
        g_args.self = any_sound();
        g_task = BG_TASK;
        break;
    case K_S3_UPDSTATUS: case K_S3_UPD: {
        g_args.self = any_of(VT_SOUND3D);
        g_task = BG_TASK;
        if (chance(50)) {                                   // placed around the listener argument itself
            const float* lp = (const float*)(*(uint8_t**)(ARG(G_LIST) + 4) + 0x24);
            float* p = *(float**)(g_args.self + 0x3c);
            const uint32_t kk = rnd() % 10;
            const float r = kk < 4 ? range(0.0f, 10.0f) : kk < 6 ? range(0.0f, 0.15f) : kk < 7 ? 0.0f : range(0.0f, 250.0f);
            const float a = range(-3.2f, 3.2f), e = range(-0.5f, 0.5f);
            p[0] = lp[0] + r * cosf(a) * cosf(e); p[1] = lp[1] + r * sinf(e); p[2] = lp[2] + r * sinf(a) * cosf(e);
            if (chance(5)) { float* v = *(float**)(g_args.self + 0x38); const float* lv = *(const float**)(ARG(G_LIST) + 8);
                             for (int c = 0; c < 3; c++) v[c] = lv[c] + range(-400.0f, 400.0f); }   // near the speed of sound
        }
        break;
    }
    case K_S_UPD: g_args.self = chance(70) ? any_of(VT_SOUND) : any_of(VT_SOUNDDASH); break;
    case K_SD_UPD: case K_SD_CANHEAR: g_args.self = any_of(VT_SOUNDDASH); break;
    case K_ATTEN: g_args.f = chance(85) ? range(-50.0f, 250.0f) : wildf(); break;
    case K_UPDATE_STATIC: case K_SM_UPDATE: case K_MUTE_SOUNDS: case K_VERIFY_SANITY: case K_SORT_SOUNDS: case K_CHECK_DESTROY:
    case K_DESTROY_ISOUNDS: case K_DESTROY_SOUNDS: case K_FLUSH: case K_SEQ_FRAMES:
        g_task = BG_TASK;                                          // (the BG thread's)
        if (k != K_SM_UPDATE && k != K_UPDATE_STATIC) *(int32_t*)(mgr + 0x17c) = BG_TASK;
        if (k == K_DESTROY_SOUNDS || k == K_FLUSH) g_task = MAIN_TASK;
        if (k == K_SEQ_FRAMES) { *(uint32_t*)S_UPDATE_F = F_MGR_UPDATE; g_args.u = rnd(); }
        break;
    case K_TABLE_CTOR: case K_LINFO_CTOR: g_args.self = AR(A_OBJ); break;
    case K_SM_CTOR: g_args.self = AR(A_MGR2); g_task = MAIN_TASK; break;
    case K_MIXER_BEGIN: case K_MIXER_END: case K_VERIFY_TABLES: break;
    case K_FG_MIXER_BEGIN: case K_FG_MIXER_END: g_task = MAIN_TASK; break;
    case K_SM_DTOR: g_task = MAIN_TASK; break;
    case K_ASSERT: g_args.i = (int)(rnd() % 3); break;
    case K_STOPCAR: g_args.i = chance(80) ? (int)(rnd() % 8) : -100; break;
    case K_SETLISTENER: break;
    case K_SORT3D: g_args.p = mgr + 0xc + 8 * (rnd() % 8); break;
    case K_HAS_PRIORITY: {
        g_args.self = any_sound(); g_args.p = any_sound();
        if (chance(40)) { *(int32_t*)(g_args.self + 0x24) = 3; *(int32_t*)(g_args.p + 0x24) = 3; }
        if (chance(30)) *(int32_t*)X_FOCUS = *(int32_t*)((chance(50) ? g_args.self : g_args.p) + 0x10);
        if (chance(10)) memcpy(g_args.p + 0x34, g_args.self + 0x34, 4);
        break;
    }
    case K_ADD_SOUND: case K_ADD_S: case K_ADD_S3: {
        const int i = g_w.n + (int)(rnd() % g_w.nstray);
        g_args.self = sound_at(i);
        *(int32_t*)(g_args.self + 0x24) = chance(95) ? (int)(rnd() % 8) : chance(50) ? -1 : 8;
        if (chance(5)) build_tables(384);
        break;
    }
    case K_REMOVE: g_args.self = chance(85) ? any_listed() : sound_at(g_w.n + (int)(rnd() % g_w.nstray)); break;
    case K_DWS:
        g_args.self = any_sound();
        if (chance(10)) *(int32_t*)(mgr + 0x158) = chance(50) ? 32 : -1;
        if ((uint32_t)*(int32_t*)(mgr + 0x158) >= 32) {                // full (here or from the world)
            // 32 distinct sounds listed, none of them the new one (Toss hands over a sound it has just made, and never
            // lists one twice); more strays made where the world has fewer than 33
            while (g_w.n + g_w.nstray < 33) {
                const int i = g_w.n + g_w.nstray++;
                g_w.cls_of[i] = 0;
                make_sound(i, 0, VT_SOUND);
            }
            const int m = g_w.n + g_w.nstray;
            int k = 0;
            for (int j = (int)(rnd() % (uint32_t)m), c = 0; c < m && k < 32; c++, j = (j + 1) % m)
                if (sound_at(j) != g_args.self) *(uint8_t**)(mgr + 0xd8 + 4 * k++) = sound_at(j);
            // and a BG thread that gets on with it, whose id the manager knows: the fixed one deletes the sound, whose
            // Destroy deletes its mixer sound on the BG thread or hands it to update() from the main thread
            *(uint32_t*)S_UPDATE_F = F_MGR_UPDATE;
            if (*(int32_t*)(mgr + 0x50) == -1) *(int32_t*)(mgr + 0x50) = 0;
            *(int32_t*)(mgr + 0x17c) = BG_TASK;
            g_dws_full = true;
        }
        break;
    case K_CREATE_SOUND: case K_FG_CREATE: case K_CREATE: case K_CREATE3D:
        g_args.i = rnd_class();
        if (k != K_CREATE_SOUND) {
            if (chance(90)) { *(uint32_t*)S_UPDATE_F = F_MGR_UPDATE; if (*(int32_t*)(mgr + 0x50) == -1) *(int32_t*)(mgr + 0x50) = 0; }
            *(uint32_t*)(mgr + 0x164) = 0; *(uint32_t*)(mgr + 0x16c) = 0;
            g_task = k == K_FG_CREATE ? MAIN_TASK : chance(50) ? BG_TASK : MAIN_TASK;
        }
        break;
    case K_DESTROY:
        g_args.p = chance(85) ? isound_at(MAX_SOUNDS + N_POOL + 2) : 0;
        if (g_args.p) init_isound(g_args.p, 3, 0);
        g_task = chance(50) ? BG_TASK : MAIN_TASK;
        if (g_task == MAIN_TASK && chance(90)) *(uint32_t*)S_UPDATE_F = chance(50) ? F_MGR_UPDATE : F_DESTROY_ISOUNDS;
        break;
    case K_RES_GET: g_args.name = (const char*)ARG(G_NAME); break;
    case K_RES_FORGET: g_args.p = AR(A_RES); break;
    default: break;
    }
    // what runs on the main thread and waits for the BG thread: mostly a BG thread that gets on with it (update() runs,
    // with the manager's BG task id known); the rest wait forever (an exception after 64 sleeps, in both passes)
    const bool keeps_update_f = k == K_UPDATE_STATIC || k == K_SM_UPDATE || k == K_MIXER_BEGIN || k == K_MIXER_END ||
                                k == K_FG_MIXER_BEGIN || k == K_FG_MIXER_END || k == K_SM_DTOR || k == K_SOUND_BEGIN;
    if (g_task == MAIN_TASK && !keeps_update_f && (chance(92) || g_dws_full)) {
        *(uint32_t*)S_UPDATE_F = F_MGR_UPDATE;
        if (*(int32_t*)(mgr + 0x50) == -1) *(int32_t*)(mgr + 0x50) = 0;
        *(int32_t*)(mgr + 0x17c) = BG_TASK;
    }
    if ((k == K_SM_UPDATE || k == K_UPDATE_STATIC) && *(int32_t*)(mgr + 0x17c) == MAIN_TASK) *(int32_t*)(mgr + 0x17c) = BG_TASK;
}

// the sequence: frames of update() with the owners poking their sounds between them (the same in both passes)
static void seq_frames(bool rw) {
    uint8_t* mgr = MGR();
    uint32_t r = g_args.u | 1;
    auto next = [&]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; };
    for (int t = 0; t < 30; t++) {
        const int n = *(int32_t*)(mgr + 8);
        for (int e = 0; e < 3 && n > 0 && n < 400; e++) {
            uint8_t* s = (*(uint8_t***)(mgr + 4))[next() % (uint32_t)n];
            if (!in_arena(s, 0x40)) continue;
            const uint32_t w = next() % 8;
            if (w < 3) s[0x29 + w] = (uint8_t)(next() & 1);             // play / stop / loop requests
            else if (w == 3) { s[0x2c] = 1; setf(s + 8, (float)(next() % 1000) / 999.0f); }
            else if (w == 4) { s[0x2d] = 1; setf(s + 4, 0.5f + (float)(next() % 1000) / 500.0f); }
            else if (w == 5 && *(uint32_t*)s == VT_SOUND3D) {
                float* p = *(float**)(s + 0x3c);
                if (in_arena(p, 12)) for (int c = 0; c < 3; c++) p[c] += (float)((int)(next() % 200) - 100) * 0.05f;
            } else if (w == 6) {
                uint8_t* is = *(uint8_t**)(s + 0xc);
                if (in_arena(is, 0x40) && *(uint8_t**)is == AR(A_ISVT)) *(int32_t*)(is + 4) = 1 + (int32_t)(next() % 3);
            }
        }
        const uint32_t w = next() % 16;
        if (w == 0) rw ? SM_SetListener_rw(mgr, 0, (const Listener*)ARG(G_LIST)) : ((decltype(&SM_SetListener_rw))0x004741e0)(mgr, 0, (const Listener*)ARG(G_LIST));
        else if (w == 1) rw ? SM_StopCar_rw(mgr, 0, (int)(next() % 8)) : ((decltype(&SM_StopCar_rw))0x004741a0)(mgr, 0, (int)(next() % 8));
        else if (w == 2) rw ? SoundMuteCars_rw() : ((decltype(&SoundMuteCars_rw))0x00471dd0)();
        else if (w == 3) rw ? SoundUnMuteCars_rw() : ((decltype(&SoundUnMuteCars_rw))0x00471de0)();
        else if (w == 4) mgr[0x4c] = 1;
        rw ? SM_update_rw(mgr, 0) : ((decltype(&SM_update_rw))0x00474230)(mgr, 0);
    }
}

#define CALL(fn, ...) (rw ? fn(__VA_ARGS__) : ((decltype(&fn))VP_CAT(addr_, fn))(__VA_ARGS__))
#define CALLV(fn, ...) do { if (rw) fn(__VA_ARGS__); else ((decltype(&fn))VP_CAT(addr_, fn))(__VA_ARGS__); } while (0)
static uint64_t P(const void* p) { return (uint64_t)(uint32_t)p; }
static uint64_t call_kind(Kind k, bool rw) {
    const Args& g = g_args;
    uint8_t* mgr = MGR();
    const Listener* L = (const Listener*)ARG(G_LIST);
    switch (k) {
    case K_STATIC_INIT: if (rw) k_einits[g.i].rw(); else ((void(__cdecl*)())k_einits[g.i].addr)(); return 0;
    case K_SOUND_BEGIN: return CALL(SoundBegin_rw);
    case K_SOUND_RESTART: CALLV(SoundRestart_rw); return 0;
    case K_SOUND_END: CALLV(SoundEnd_rw); return 0;
    case K_MUTE_CARS: CALLV(SoundMuteCars_rw); return 0;
    case K_UNMUTE_CARS: CALLV(SoundUnMuteCars_rw); return 0;
    case K_FLUSH_ASYNC: CALLV(SoundFlushAsync_rw); return 0;
    case K_SET_LISTENER_C: CALLV(SoundSetListener_rw, L); return 0;
    case K_STOP_CAR_C: CALLV(SoundStopCar_rw, g.i); return 0;
    case K_CLASS_STRING: return P(CALL(SoundClassString_rw, g.i));
    case K_CLICK: CALLV(SoundClick_rw); return 0;
    case K_SB_CTOR: return P(CALL(SoundBase_ctor_rw, g.self, 0, g.name, g.i));
    case K_SB_DTOR: CALLV(SoundBase_dtor_rw, g.self, 0); return 0;
    case K_SB_CANHEAR: return CALL(SoundBase_CanBeHeard_rw, g.self, 0);
    case K_SB_UPDSTATUS: CALLV(SoundBase_UpdateStatus_rw, g.self, 0, L); return 0;
    case K_SB_UPDSOUND: CALLV(SoundBase_UpdateSound_rw, g.self, 0, L); return 0;
    case K_SB_UPD: CALLV(SoundBase_update_sound_rw, g.self, 0, L); return 0;
    case K_S3_UPDSTATUS: CALLV(Sound3D_update_status_rw, g.self, 0, L); return 0;
    case K_ATTEN: { double d = CALL(attenuation_rw, g.f); uint64_t u; memcpy(&u, &d, 8); return u; }
    case K_S3_UPD: CALLV(Sound3D_update_sound_rw, g.self, 0, L); return 0;
    case K_S3_CTOR: return P(CALL(Sound3D_ctor_rw, g.self, 0, g.name, g.i, g.p + 12, g.p));
    case K_S3_CANHEAR: return CALL(Sound3D_CanBeHeard_rw, g.self, 0);
    case K_S_CTOR: return P(CALL(Sound_ctor_rw, g.self, 0, g.name, g.i));
    case K_S_UPD: CALLV(Sound_update_sound_rw, g.self, 0, L); return 0;
    case K_SD_CTOR: return P(CALL(SoundDash_ctor_rw, g.self, 0, g.name, g.i, g.j));
    case K_SD_UPD: CALLV(SoundDash_update_sound_rw, g.self, 0, L); return 0;
    case K_SD_CANHEAR: return CALL(SoundDash_CanBeHeard_rw, g.self, 0);
    case K_TOSS: CALLV(Sound_Toss_rw, g.name, g.i); return 0;
    case K_SD_CREATE: return P(CALL(SoundDash_Create_rw, g.name, g.i, g.j));
    case K_S3_CREATE: return P(CALL(Sound3D_Create_rw, g.name, g.i, g.p + 12, g.p));
    case K_S_CREATE: return P(CALL(Sound_Create_rw, g.name, g.i));
    case K_ENGINE_CREATE: return P(CALL(EngineSound_Create_rw, (void*)ARG(G_VEL)));
    case K_SB_UPDSTATUS_BASE: CALLV(SoundBase_update_status_rw, g.self, 0, L); return 0;
    case K_SDTOR: return P(rw ? k_sdtor_rw[g.i](g.self, 0, g.u) : ((Sdtor_t)k_sdtor_addr[g.i])(g.self, 0, g.u));
    case K_UPDATE_STATIC: CALLV(SoundManager_Update_rw); return 0;
    case K_TABLE_CTOR: return P(CALL(SoundTable_ctor_rw, g.self, 0));
    case K_SM_CTOR: return P(CALL(SoundManager_ctor_rw, g.self, 0, (void*)AR(A_MIXER)));
    case K_VERIFY_ENT: CALLV(verify_entitlements_rw); return 0;
    case K_MIXER_BEGIN: CALLV(SM_mixer_begin_rw, mgr, 0); return 0;
    case K_MIXER_END: CALLV(SM_mixer_end_rw, mgr, 0); return 0;
    case K_FG_MIXER_BEGIN: CALLV(SM_fg_mixer_begin_rw, mgr, 0); return 0;
    case K_FG_MIXER_END: CALLV(SM_fg_mixer_end_rw, mgr, 0); return 0;
    case K_SM_DTOR: CALLV(SoundManager_dtor_rw, mgr, 0); return 0;
    case K_ASSERT: CALLV(ASSERT_MSG_rw, g.i, "x"); return 0;
    case K_STOPCAR: CALLV(SM_StopCar_rw, mgr, 0, g.i); return 0;
    case K_SETLISTENER: CALLV(SM_SetListener_rw, mgr, 0, L); return 0;
    case K_SM_UPDATE: CALLV(SM_update_rw, mgr, 0); return 0;
    case K_DESTROY_SOUNDS: CALLV(SM_destroy_sounds_rw, mgr, 0); return 0;
    case K_DESTROY_ISOUNDS: CALLV(SM_destroy_isounds_rw, mgr, 0); return 0;
    case K_CHECK_DESTROY: CALLV(SM_check_destroy_lists_rw, mgr, 0); return 0;
    case K_MUTE_SOUNDS: CALLV(SM_mute_sounds_rw, mgr, 0); return 0;
    case K_VERIFY_SANITY: CALLV(SM_verify_sanity_rw, mgr, 0); return 0;
    case K_SORT3D: CALLV(SM_sort_3D_rw, mgr, 0, g.p); return 0;
    case K_HAS_PRIORITY: return CALL(sound3D_has_priority_rw, g.self, g.p);
    case K_SORT_SOUNDS: CALLV(SM_sort_sounds_rw, mgr, 0, L); return 0;
    case K_VERIFY_TABLES: CALLV(SM_verify_soundtables_rw, mgr, 0); return 0;
    case K_ADD_SOUND: CALLV(SM_add_sound_rw, mgr, 0, g.self); return 0;
    case K_ADD_S: CALLV(SM_Add_Sound_rw, mgr, 0, g.self); return 0;
    case K_ADD_S3: CALLV(SM_Add_Sound3D_rw, mgr, 0, g.self); return 0;
    case K_IS_3D: return CALL(SM_soundclass_is_3d_rw, mgr, 0, g.i);
    case K_REMOVE: CALLV(SM_Remove_rw, mgr, 0, g.self); return 0;
    case K_FLUSH: CALLV(SM_FlushAsync_rw, mgr, 0); return 0;
    case K_DWS: CALLV(SM_DestroyWhenStopped_rw, mgr, 0, g.self); return 0;
    case K_CREATE_SOUND: return P(CALL(SM_create_sound_rw, mgr, 0, g.name, g.i));
    case K_FG_CREATE: return P(CALL(SM_fg_create_sound_rw, mgr, 0, g.name, g.i));
    case K_CREATE: return P(CALL(SM_Create_rw, mgr, 0, g.name, g.i));
    case K_CREATE3D: return P(CALL(SM_Create3D_rw, mgr, 0, g.name, g.i));
    case K_DESTROY: CALLV(SM_Destroy_rw, mgr, 0, g.p); return 0;
    case K_LINFO_CTOR: return P(CALL(listener_info_ctor_rw, g.self, 0));
    case K_RES_GET: return P(CALL(SoundResourceGet_rw, g.name));
    case K_RES_FORGET: CALLV(SoundResourceForget_rw, g.p); return 0;
    case K_SEQ_FRAMES: seq_frames(rw); return 0;
    default: return 0;
    }
}
static bool returns_byte(Kind k) {
    return k == K_SOUND_BEGIN || k == K_SB_CANHEAR || k == K_S3_CANHEAR || k == K_SD_CANHEAR || k == K_HAS_PRIORITY || k == K_IS_3D;
}
static void footprint_of(Kind k, Footprint& fp) {
    fp.n = 0; fp.replay_only = 0; fp.pure = false;
    const Args& g = g_args;
    uint8_t* mgr = MGR();
    const Listener* L = (const Listener*)ARG(G_LIST);
    switch (k) {
    case K_STATIC_INIT: fp_static_init(fp); break;
    case K_SOUND_BEGIN: fpof_SoundBegin_rw(fp); break;
    case K_SOUND_RESTART: fpof_SoundRestart_rw(fp); break;
    case K_SOUND_END: fpof_SoundEnd_rw(fp); break;
    case K_MUTE_CARS: fpof_SoundMuteCars_rw(fp); break;
    case K_UNMUTE_CARS: fpof_SoundUnMuteCars_rw(fp); break;
    case K_FLUSH_ASYNC: fpof_SoundFlushAsync_rw(fp); break;
    case K_SET_LISTENER_C: fpof_SoundSetListener_rw(fp, L); break;
    case K_STOP_CAR_C: fpof_SoundStopCar_rw(fp, g.i); break;
    case K_CLASS_STRING: fpof_SoundClassString_rw(fp, g.i); break;
    case K_CLICK: fpof_SoundClick_rw(fp); break;
    case K_SB_CTOR: fpof_SoundBase_ctor_rw(fp, g.self, 0, g.name, g.i); break;
    case K_SB_DTOR: fpof_SoundBase_dtor_rw(fp, g.self, 0); break;
    case K_SB_CANHEAR: fpof_SoundBase_CanBeHeard_rw(fp, g.self, 0); break;
    case K_SB_UPDSTATUS: fpof_SoundBase_UpdateStatus_rw(fp, g.self, 0, L); break;
    case K_SB_UPDSOUND: fpof_SoundBase_UpdateSound_rw(fp, g.self, 0, L); break;
    case K_SB_UPD: fpof_SoundBase_update_sound_rw(fp, g.self, 0, L); break;
    case K_S3_UPDSTATUS: fpof_Sound3D_update_status_rw(fp, g.self, 0, L); break;
    case K_ATTEN: fpof_attenuation_rw(fp, g.f); break;
    case K_S3_UPD: fpof_Sound3D_update_sound_rw(fp, g.self, 0, L); break;
    case K_S3_CTOR: fpof_Sound3D_ctor_rw(fp, g.self, 0, g.name, g.i, g.p + 12, g.p); break;
    case K_S3_CANHEAR: fpof_Sound3D_CanBeHeard_rw(fp, g.self, 0); break;
    case K_S_CTOR: fpof_Sound_ctor_rw(fp, g.self, 0, g.name, g.i); break;
    case K_S_UPD: fpof_Sound_update_sound_rw(fp, g.self, 0, L); break;
    case K_SD_CTOR: fpof_SoundDash_ctor_rw(fp, g.self, 0, g.name, g.i, g.j); break;
    case K_SD_UPD: fpof_SoundDash_update_sound_rw(fp, g.self, 0, L); break;
    case K_SD_CANHEAR: fpof_SoundDash_CanBeHeard_rw(fp, g.self, 0); break;
    case K_TOSS: fpof_Sound_Toss_rw(fp, g.name, g.i); break;
    case K_SD_CREATE: fpof_SoundDash_Create_rw(fp, g.name, g.i, g.j); break;
    case K_S3_CREATE: fpof_Sound3D_Create_rw(fp, g.name, g.i, g.p + 12, g.p); break;
    case K_S_CREATE: fpof_Sound_Create_rw(fp, g.name, g.i); break;
    case K_ENGINE_CREATE: fpof_EngineSound_Create_rw(fp, ARG(G_VEL)); break;
    case K_SB_UPDSTATUS_BASE: fpof_SoundBase_update_status_rw(fp, g.self, 0, L); break;
    case K_SDTOR: fpof_SoundBase_sdtor_rw(fp, g.self, 0, g.u); break;
    case K_UPDATE_STATIC: fpof_SoundManager_Update_rw(fp); break;
    case K_TABLE_CTOR: fpof_SoundTable_ctor_rw(fp, g.self, 0); break;
    case K_SM_CTOR: fpof_SoundManager_ctor_rw(fp, g.self, 0, AR(A_MIXER)); break;
    case K_VERIFY_ENT: fpof_verify_entitlements_rw(fp); break;
    case K_MIXER_BEGIN: fpof_SM_mixer_begin_rw(fp, mgr, 0); break;
    case K_MIXER_END: fpof_SM_mixer_end_rw(fp, mgr, 0); break;
    case K_FG_MIXER_BEGIN: fpof_SM_fg_mixer_begin_rw(fp, mgr, 0); break;
    case K_FG_MIXER_END: fpof_SM_fg_mixer_end_rw(fp, mgr, 0); break;
    case K_SM_DTOR: fpof_SoundManager_dtor_rw(fp, mgr, 0); break;
    case K_ASSERT: fpof_ASSERT_MSG_rw(fp, g.i, "x"); break;
    case K_STOPCAR: fpof_SM_StopCar_rw(fp, mgr, 0, g.i); break;
    case K_SETLISTENER: fpof_SM_SetListener_rw(fp, mgr, 0, L); break;
    case K_SM_UPDATE: fpof_SM_update_rw(fp, mgr, 0); break;
    case K_DESTROY_SOUNDS: fpof_SM_destroy_sounds_rw(fp, mgr, 0); break;
    case K_DESTROY_ISOUNDS: fpof_SM_destroy_isounds_rw(fp, mgr, 0); break;
    case K_CHECK_DESTROY: fpof_SM_check_destroy_lists_rw(fp, mgr, 0); break;
    case K_MUTE_SOUNDS: fpof_SM_mute_sounds_rw(fp, mgr, 0); break;
    case K_VERIFY_SANITY: fpof_SM_verify_sanity_rw(fp, mgr, 0); break;
    case K_SORT3D: fpof_SM_sort_3D_rw(fp, mgr, 0, g.p); break;
    case K_HAS_PRIORITY: fpof_sound3D_has_priority_rw(fp, g.self, g.p); break;
    case K_SORT_SOUNDS: fpof_SM_sort_sounds_rw(fp, mgr, 0, L); break;
    case K_VERIFY_TABLES: fpof_SM_verify_soundtables_rw(fp, mgr, 0); break;
    case K_ADD_SOUND: fpof_SM_add_sound_rw(fp, mgr, 0, g.self); break;
    case K_ADD_S: fpof_SM_Add_Sound_rw(fp, mgr, 0, g.self); break;
    case K_ADD_S3: fpof_SM_Add_Sound3D_rw(fp, mgr, 0, g.self); break;
    case K_IS_3D: fpof_SM_soundclass_is_3d_rw(fp, mgr, 0, g.i); break;
    case K_REMOVE: fpof_SM_Remove_rw(fp, mgr, 0, g.self); break;
    case K_FLUSH: fpof_SM_FlushAsync_rw(fp, mgr, 0); break;
    case K_DWS: fpof_SM_DestroyWhenStopped_rw(fp, mgr, 0, g.self); break;
    case K_CREATE_SOUND: fpof_SM_create_sound_rw(fp, mgr, 0, g.name, g.i); break;
    case K_FG_CREATE: fpof_SM_fg_create_sound_rw(fp, mgr, 0, g.name, g.i); break;
    case K_CREATE: fpof_SM_Create_rw(fp, mgr, 0, g.name, g.i); break;
    case K_CREATE3D: fpof_SM_Create3D_rw(fp, mgr, 0, g.name, g.i); break;
    case K_DESTROY: fpof_SM_Destroy_rw(fp, mgr, 0, g.p); break;
    case K_LINFO_CTOR: fpof_listener_info_ctor_rw(fp, g.self, 0); break;
    case K_RES_GET: fpof_SoundResourceGet_rw(fp, g.name); break;
    case K_RES_FORGET: fpof_SoundResourceForget_rw(fp, g.p); break;
    default: fp.replay_only = "a sequence of calls"; break;
    }
}
static bool in_footprint(const Footprint& fp, const uint8_t* p) {
    for (int i = 0; i < fp.n; i++)
        if (p >= (const uint8_t*)fp.r[i].p && p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}

static uint32_t g_fault_eip, g_fault_code, g_fault_addr;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_addr = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static unsigned g_pc;
static bool g_unmask;               // overflow and divide-by-zero unmasked (docs/PORTING.md 10a): a dropped store shows
static int32_t g_world_task;
static int run_guarded(Kind k, bool rw, uint64_t* ret) {
    g_log.n = 0;
    g_fix_fired = 0;
    g_heap = g_heap_mark;
    g_alloc_calls = g_create_calls = g_pool_next = g_sleeps = 0;
    g_task = g_world_task;
    g_in_tick = false;
    unsigned cw;
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    if (g_unmask) _controlfp_s(&cw, _MCW_EM & ~(_EM_OVERFLOW | _EM_ZERODIVIDE), _MCW_EM);
    int fault = 0;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
        *ret = call_kind(k, rw);
        __asm__ volatile("fwait" ::: VP_X87_CLOBBERS, "memory");
    }, fault_filter)) { fault = 1; }
#else
    __try {
        *ret = call_kind(k, rw);
        __asm fwait
    } __except (fault_filter(GetExceptionInformation())) { fault = 1; }
#endif
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    return fault;
}

static const char* where(uint32_t a, char* buf) {
    uint32_t base = (uint32_t)(uintptr_t)g_arena;
    if (a >= base && a < base + ARENA_BYTES) { sprintf(buf, "arena+0x%x", a - base); return buf; }
    sprintf(buf, "0x%08x", a);
    return buf;
}

#ifdef VP_SND_FIXES
// ---- the fixes, each on the input that used to fail --------------------------------------------------------------------
static uint32_t g_t_fault;
static int guarded(void (*fn)(), bool unmask, int32_t task) {
    g_log.n = 0;
    g_fix_fired = 0;
    g_heap = g_heap_mark;
    g_alloc_calls = g_create_calls = g_pool_next = g_sleeps = 0;
    g_task = task;
    g_in_tick = false;
    unsigned cw;
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, _PC_24, _MCW_PC);                              // (the BG thread's precision)
    if (unmask) _controlfp_s(&cw, _MCW_EM & ~(_EM_OVERFLOW | _EM_ZERODIVIDE), _MCW_EM);
    int fault = 0;
    g_t_fault = 0;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
        fn();
        __asm__ volatile("fwait" ::: VP_X87_CLOBBERS, "memory");
    }, fault_filter)) { fault = 1; g_t_fault = g_fault_code; }
#else
    __try {
        fn();
        __asm fwait
    } __except (fault_filter(GetExceptionInformation())) { fault = 1; g_t_fault = g_fault_code; }
#endif
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    return fault;
}
static int count_tag(uint32_t tag) { int n = 0; for (uint32_t i = 0; i < g_log.n && i < LOG_MAX; i++) n += g_log.w[i] == tag; return n; }
static uint32_t last_after(uint32_t tag, uint32_t k) {           // the k-th word after the last `tag` (0: none)
    uint32_t v = 0;
    for (uint32_t i = 0; i + k < g_log.n && i + k < LOG_MAX; i++) if (g_log.w[i] == tag) v = g_log.w[i + k];
    return v;
}
static void snap() { memcpy(g_arena_snap, g_arena, ARENA_BYTES); memcpy(g_data_snap, DATA, DATA_BYTES); }
static void restore() { memcpy(g_arena, g_arena_snap, ARENA_BYTES); memcpy(DATA, g_data_snap, DATA_BYTES); }
static void keep_after() { memcpy(g_arena_after, g_arena, ARENA_BYTES); memcpy(g_data_after, DATA, DATA_BYTES); g_log_orig = g_log; }
static bool same_as_after() {                                     // arena, .data/.bss and the log as the original left them
    return !memcmp(g_arena_after, g_arena, ARENA_BYTES) && !memcmp(g_data_after, DATA, DATA_BYTES) && g_log.n == g_log_orig.n &&
           !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
}
static bool unchanged() { return !memcmp(g_arena_snap, g_arena, ARENA_BYTES) && !memcmp(g_data_snap, DATA, DATA_BYTES); }
struct Saved5 { uint32_t at; uint8_t b[5]; };
static void hook_to(Saved5& s, uint32_t at, void* to) { s.at = at; memcpy(s.b, (void*)at, 5); patch_jmp(at, to); }
static void unhook(const Saved5& s) { memcpy((void*)s.at, s.b, 5); }

static uint8_t* g_t_s;
static uint8_t* g_t_ret;
static const Listener* TL() { return (const Listener*)ARG(G_LIST); }
static const char* TN() { return (const char*)ARG(G_NAME); }
// (a) Doppler
static void t_dop_o() { ((decltype(&Sound3D_update_sound_rw))0x00472150)(g_t_s, 0, TL()); }
static void t_dop_n() { Sound3D_update_sound_rw(g_t_s, 0, TL()); }
// (b) no manager
static void t_mute_o() { ((void(__cdecl*)())0x00471dd0)(); }
static void t_mute_n() { SoundMuteCars_rw(); }
static void t_unmute_o() { ((void(__cdecl*)())0x00471de0)(); }
static void t_unmute_n() { SoundUnMuteCars_rw(); }
static void t_flush_o() { ((void(__cdecl*)())0x00471df0)(); }
static void t_flush_n() { SoundFlushAsync_rw(); }
static void t_setl_o() { ((void(__cdecl*)(const Listener*))0x00471e00)(TL()); }
static void t_setl_n() { SoundSetListener_rw(TL()); }
static void t_stop_o() { ((void(__cdecl*)(int))0x00471e20)(3); }
static void t_stop_n() { SoundStopCar_rw(3); }
static void t_sctor_o() { ((decltype(&Sound_ctor_rw))0x004723c0)(AR(A_OBJ), 0, TN(), 3); }
static void t_sctor_n() { Sound_ctor_rw(AR(A_OBJ), 0, TN(), 3); }
static void t_s3ctor_o() { ((decltype(&Sound3D_ctor_rw))0x00472340)(AR(A_OBJ), 0, TN(), 3, AR(A_P3) + 12, AR(A_P3)); }
static void t_s3ctor_n() { Sound3D_ctor_rw(AR(A_OBJ), 0, TN(), 3, AR(A_P3) + 12, AR(A_P3)); }
static void t_sdctor_n() { SoundDash_ctor_rw(AR(A_OBJ), 0, TN(), 6, 2); }
static void t_sdctor_o() { ((decltype(&SoundDash_ctor_rw))0x00472440)(AR(A_OBJ), 0, TN(), 6, 2); }
static void t_screate_o() { g_t_ret = ((decltype(&Sound_Create_rw))0x004725b0)(TN(), 3); }
static void t_screate_n() { g_t_ret = Sound_Create_rw(TN(), 3); }
static void t_s3create_n() { g_t_ret = Sound3D_Create_rw(TN(), 3, AR(A_P3) + 12, AR(A_P3)); }
static void t_sdcreate_n() { g_t_ret = SoundDash_Create_rw(TN(), 6, 2); }
static void t_toss_n() { Sound_Toss_rw(TN(), 0); }
static void t_dtor_o() { ((decltype(&SoundBase_dtor_rw))0x00471f20)(g_t_s, 0); }
static void t_dtor_n() { SoundBase_dtor_rw(g_t_s, 0); }
// (c) update's lock
static void t_upd_o() { ((decltype(&SM_update_rw))0x00474230)(MGR(), 0); }
static void t_upd_n() { SM_update_rw(MGR(), 0); }
static void t_frames_o() { for (int i = 0; i < 3; i++) ((decltype(&SM_update_rw))0x00474230)(MGR(), 0); }
// (f) resources
static void t_res_o() { g_t_ret = (uint8_t*)((decltype(&SoundResourceGet_rw))0x00477200)(TN()); }
static void t_res_n() { g_t_ret = (uint8_t*)SoundResourceGet_rw(TN()); }
// (g), (h) the table
static void t_add_o() { ((decltype(&SM_add_sound_rw))0x00474810)(MGR(), 0, g_t_s); }
static void t_add_n() { SM_add_sound_rw(MGR(), 0, g_t_s); }
static void t_rem_n() { SM_Remove_rw(MGR(), 0, g_t_s); }
static void t_smctor_n() { SoundManager_ctor_rw(AR(A_MGR2), 0, AR(A_MIXER)); }
// (i) DestroyWhenStopped
static void t_dws_o() { ((decltype(&SM_DestroyWhenStopped_rw))0x004749b0)(MGR(), 0, g_t_s); }
static void t_dws_n() { SM_DestroyWhenStopped_rw(MGR(), 0, g_t_s); }

static uint32_t g_big[1024 + 16];                                  // a lifted sound table (1024) and room past it

// a world with the manager ready on the BG thread (creating directly), nothing in flight
static void calm_world(bool no_sounds) {
    random_world();
    uint8_t* mgr = MGR();
    if (no_sounds) { g_w.n = 0; build_tables(); }
    mgr[0] = 1; mgr[1] = 0; mgr[0x4c] = 0; mgr[0x174] = 0;
    *(int32_t*)(mgr + 0x50) = 0;
    *(int32_t*)(mgr + 0x17c) = BG_TASK;
    *(uint32_t*)(mgr + 0x164) = 0; *(uint32_t*)(mgr + 0x16c) = 0;
    *(int32_t*)(mgr + 0x160) = 0;
    *(uint32_t*)S_UPDATE_F = F_MGR_UPDATE;
    g_script.alloc_fail = 0; g_script.create_fail = 0;
}

static int directed_fix_tests() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) printf("  fix test: %s FAILED\n", what); bad += !ok; };
    int dop_orig_faults = 0, dop_cases = 0, dop_near_same = 0, null_orig_faults = 0, res_short = 0, res_same = 0;
    for (int round = 0; round < 40 && bad < 20; round++) {
        // (a) Doppler at exactly the speed of sound, exceptions unmasked (the BG thread's state)
        calm_world(false);
        g_t_s = any_of(VT_SOUND3D);
        g_t_s[0x30] = 1;
        setf(g_t_s + 4, range(0.5f, 2.0f));
        init_isound(*(uint8_t**)(g_t_s + 0xc), 1, 0);                  // (Can3D 0: the software path)
        float* fr = (float*)ARG(G_FRAME);
        memset(fr, 0, 48);
        fr[0] = fr[4] = fr[8] = 1.0f;
        for (int c = 9; c < 12; c++) fr[c] = (float)(int)(rnd() % 200) - 100.0f;
        float* lv = (float*)ARG(G_VEL);
        for (int c = 0; c < 3; c++) lv[c] = (float)(int)(rnd() % 60) - 30.0f;
        *(int32_t*)ARG(G_LIST) = 0;
        *(uint8_t**)(ARG(G_LIST) + 4) = ARG(G_FRAME);
        *(uint8_t**)(ARG(G_LIST) + 8) = ARG(G_VEL);
        float* pos = *(float**)(g_t_s + 0x3c);
        float* vel = *(float**)(g_t_s + 0x38);
        pos[0] = fr[9] + 16.0f; pos[1] = fr[10]; pos[2] = fr[11];      // 16 m along x: the unit vector is exact
        vel[0] = lv[0] - bitsf(0x43a9fc29); vel[1] = lv[1]; vel[2] = lv[2];
        if (fbits(vel[0] - lv[0]) == 0xc3a9fc29u) {                     // (exact for these values)
            dop_cases++;
            snap();
            const int fo = guarded(t_dop_o, true, BG_TASK);
            dop_orig_faults += fo && g_t_fault == EXCEPTION_FLT_DIVIDE_BY_ZERO;
            const uint32_t ocode = g_t_fault;
            restore();
            const int fn = guarded(t_dop_n, true, BG_TASK);
            check(fo && ocode == EXCEPTION_FLT_DIVIDE_BY_ZERO && !fn && g_fix_fired == 1 && last_after('SFRQ', 2) == 0x7f800000u,
                  "(a) Doppler at the speed of sound: the original divides by zero, the rewrite hands SetFrequency +inf");
            // beside it: a closing speed one step off, and a few near ones: the original's bits
            const float v0 = vel[0];
            for (int k = 0; k < 4; k++) {
                restore();
                vel[0] = k == 0 ? bitsf(fbits(v0) + 1) : v0 + range(-2.0f, 2.0f);
                snap();
                const int o2 = guarded(t_dop_o, true, BG_TASK);
                keep_after();
                restore();
                const int n2 = guarded(t_dop_n, true, BG_TASK);
                if (o2) continue;                                       // (a near value can land on 0 too)
                const bool ok = !n2 && g_fix_fired == 0 && same_as_after();
                check(ok, "(a) Doppler beside the speed of sound: the original's bits");
                dop_near_same += ok;
            }
        }

        // (b) no manager: the entry points
        void (*const eo[5])() = {t_mute_o, t_unmute_o, t_flush_o, t_setl_o, t_stop_o};
        void (*const en[5])() = {t_mute_n, t_unmute_n, t_flush_n, t_setl_n, t_stop_n};
        for (int e = 0; e < 5; e++) {
            calm_world(false);
            *(uint8_t**)S_MGR = 0;
            snap();
            const int fo = guarded(eo[e], false, MAIN_TASK);
            null_orig_faults += fo;
            restore();
            const int fn = guarded(en[e], false, MAIN_TASK);
            check(fo && !fn && unchanged() && g_log.n == 0, "(b) an entry point with no manager: the original faults, the rewrite does nothing");
        }
        // (b) the constructors: no mixer sound, nothing asked of the manager
        {
            void (*const co[2])() = {t_sctor_o, t_s3ctor_o};
            void (*const cn[3])() = {t_sctor_n, t_s3ctor_n, t_sdctor_n};
            for (int e = 0; e < 3; e++) {
                calm_world(false);
                *(uint8_t**)S_MGR = 0;
                memset(AR(A_OBJ), 0xcd, 0x100);
                Saved5 h;
                if (e < 2) {
                    const int fo = guarded(co[e], false, MAIN_TASK);
                    null_orig_faults += fo;
                    check(fo != 0, "(b) a sound's constructor with no manager: the original faults");
                    memset(AR(A_OBJ), 0xcd, 0x100);
                } else {
                    hook_to(h, 0x004723c0, (void*)&Sound_ctor_rw);      // SoundDash's base: the fixed one
                }
                const int fn = guarded(cn[e], false, MAIN_TASK);
                if (e == 2) unhook(h);
                check(!fn && *(uint32_t*)(AR(A_OBJ) + 0xc) == 0 && count_tag('TGID') == 0 && count_tag('CRSN') == 0 && count_tag('MENT') == 0,
                      "(b) a sound's constructor with no manager: no mixer sound, the manager left alone");
            }
        }
        // (b) the factories and Toss, with the fixed constructors and destructor in place: NULL, the sound freed
        {
            calm_world(false);
            *(uint8_t**)S_MGR = 0;
            snap();
            const int fo = guarded(t_screate_o, false, MAIN_TASK);
            null_orig_faults += fo;
            check(fo != 0, "(b) Sound::Create with no manager: the original faults");
            Saved5 h[4];
            hook_to(h[0], 0x004723c0, (void*)&Sound_ctor_rw);
            hook_to(h[1], 0x00472340, (void*)&Sound3D_ctor_rw);
            hook_to(h[2], 0x00472440, (void*)&SoundDash_ctor_rw);
            hook_to(h[3], 0x00471f20, (void*)&SoundBase_dtor_rw);
            void (*const fns[3])() = {t_screate_n, t_s3create_n, t_sdcreate_n};
            for (int e = 0; e < 3; e++) {
                restore();
                g_t_ret = (uint8_t*)1;
                const int fn = guarded(fns[e], false, MAIN_TASK);
                check(!fn && g_t_ret == 0 && count_tag('MALC') == 1 && count_tag('DELE') == 1 && count_tag('CRSN') == 0,
                      "(b) Create with no manager: the sound is made, deleted, and NULL returned");
            }
            restore();
            const int ft = guarded(t_toss_n, false, MAIN_TASK);
            check(!ft && count_tag('DELE') == 1 && count_tag('MENT') == 0 && count_tag('PANC') == 0, "(b) Toss with no manager: nothing played or listed");
            for (int k = 3; k >= 0; k--) unhook(h[k]);
        }
        // (b) a sound outliving its manager
        if (g_w.n) {
            calm_world(false);
            g_t_s = any_listed();
            *(uint8_t**)S_MGR = 0;
            snap();
            const int fo = guarded(t_dtor_o, false, MAIN_TASK);
            null_orig_faults += fo;
            restore();
            const int fn = guarded(t_dtor_n, false, MAIN_TASK);
            check(fo && !fn && *(uint32_t*)(g_t_s + 0xc) == 0 && g_log.n == 0, "(b) ~SoundBase with no manager: let go of, nothing called");
        }

        // (c) update() with update_f cleared: the lock let go of
        {
            calm_world(false);
            *(uint32_t*)S_UPDATE_F = 0;
            snap();
            guarded(t_upd_o, false, BG_TASK);
            const int oe = count_tag('MENT'), ol = count_tag('MLEV');
            keep_after();
            restore();
            guarded(t_upd_n, false, BG_TASK);
            check(oe == 1 && ol == 0 && count_tag('MENT') == 1 && count_tag('MLEV') == 1 && !memcmp(g_arena_after, g_arena, ARENA_BYTES),
                  "(c) update() with update_f 0: the original keeps the lock, the rewrite lets go of it");
        }

        // (e) SoundDash +0x38: garbage 0 left the dash sound silent for good; 1 lets its first update decide
        {
            calm_world(true);
            g_script.quality = 2;
            *(int32_t*)(MGR() + 0x54) = 2;                              // the listener is car 2, the dash's car
            memset(AR(A_OBJ), 0, 0x100);                                // (the garbage: 0)
            snap();
            const int fo = guarded(t_sdctor_o, false, BG_TASK);
            keep_after();
            restore();
            const int fn = guarded(t_sdctor_n, false, BG_TASK);
            const uint8_t o38 = g_arena_after[A_OBJ + 0x38], n38 = g_arena[A_OBJ + 0x38];
            g_arena_after[A_OBJ + 0x38] = n38;
            check(!fo && !fn && o38 == 0 && n38 == 1 && same_as_after(), "(e) SoundDash: +0x38 starts at 1, everything else the original's");
            // three frames with a play request, heard from its own car: the original's stays muted, the fixed one plays
            int32_t st[2] = {-1, -1};
            for (int pass = 0; pass < 2; pass++) {
                restore();
                guarded(pass ? t_sdctor_n : t_sdctor_o, false, BG_TASK);
                uint8_t* sd = AR(A_OBJ);
                sd[0x29] = 1; sd[0x2a] = 0; sd[0x2b] = 0;
                setf(sd + 8, 1.0f);
                guarded(t_frames_o, false, BG_TASK);
                uint8_t* is = *(uint8_t**)(sd + 0xc);
                st[pass] = in_arena(is, 0x40) ? *(int32_t*)(is + 4) : -2;
            }
            check(st[0] == 2 && st[1] == 1, "(e) SoundDash heard from its own car: the original's (garbage 0) never plays, the fixed one does");
        }

        // (f) an old-format resource shorter than 0x800 bytes
        {
            static const uint32_t lens[] = {0, 1, 0x7fe, 0x7ff, 0x800, 0x801, 0x1234, 0x10000};
            for (uint32_t L : lens) {
                calm_world(false);
                g_script.res_found = 1;
                g_script.res_version = 0;
                *(uint32_t*)AR(A_RES) = L;
                snap();
                guarded(t_res_o, false, MAIN_TASK);
                keep_after();
                restore();
                guarded(t_res_n, false, MAIN_TASK);
                const uint32_t got = *(uint32_t*)AR(A_RES);
                if (L < 0x800) {
                    check(*(uint32_t*)(g_arena_after + A_RES) == L - 0x800 && got == 0 && g_t_ret == AR(A_RES) && g_fix_fired == 1,
                          "(f) a short old-format resource: length 0, not wrapped negative");
                    res_short++;
                } else {
                    check(g_fix_fired == 0 && same_as_after() && got == L - 0x800, "(f) a long old-format resource: cut as before");
                    res_same++;
                }
            }
        }

        // (h) a class outside 0..7: clamped on the way in, found the same way on the way out
        {
            static const int32_t bad_cls[] = {-1, 8, 100, (int32_t)0x80000000, 0x7fffffff};
            for (int32_t c : bad_cls) {
                calm_world(false);
                g_t_s = sound_at(g_w.n);                                // a stray: not in the table
                *(int32_t*)(g_t_s + 0x24) = c;
                uint8_t* mgr = MGR();
                static uint8_t mgr0[0x180], list0[0x600];
                memcpy(mgr0, mgr, 0x180);
                const int32_t n0 = *(int32_t*)(mgr + 8);
                memcpy(list0, AR(A_LIST), 4 * n0);
                const int cc = c < 0 ? 0 : 7;
                const int32_t nc0 = *(int32_t*)(mgr + 0x10 + 8 * cc);
                const int f1 = guarded(t_add_n, false, BG_TASK);
                uint8_t** run = *(uint8_t***)(mgr + 0xc + 8 * cc);
                const bool in = *(int32_t*)(mgr + 8) == n0 + 1 && *(int32_t*)(mgr + 0x10 + 8 * cc) == nc0 + 1 && run[0] == g_t_s;
                const int f2 = guarded(t_rem_n, false, BG_TASK);
                check(!f1 && !f2 && in && count_tag('PANC') == 0 && !memcmp(mgr0, mgr, 0x180) && !memcmp(list0, AR(A_LIST), 4 * n0),
                      "(h) a class outside 0..7: added to the nearest class's run, removed again, the tables as before");
            }
            calm_world(false);                                          // the original, class 8: its "run" is the listener index
            g_t_s = sound_at(g_w.n);
            *(int32_t*)(g_t_s + 0x24) = 8;
            *(int32_t*)(MGR() + 0x4c) = 0;
            check(guarded(t_add_o, false, BG_TASK) != 0, "(h) the original, class 8: faults");
        }

        // (i) DestroyWhenStopped past 32: the sound destroyed, no panic -- on the BG thread and on the main thread
        for (int thread = 0; thread < 2; thread++) {
            calm_world(false);
            if (!g_w.n) continue;
            g_t_s = any_listed();
            uint8_t* is = *(uint8_t**)(g_t_s + 0xc);
            for (int k = 0; k < 32; k++) {                              // 32 tossed sounds still playing
                uint8_t* p = sound_at(g_w.n + k % g_w.nstray);
                *(uint8_t**)(MGR() + 0xd8 + 4 * k) = p;
                *(int32_t*)(*(uint8_t**)(p + 0xc) + 4) = 1;
            }
            *(int32_t*)(MGR() + 0x158) = 32;
            snap();
            guarded(t_dws_o, false, thread ? MAIN_TASK : BG_TASK);
            const int op = count_tag('PANC');
            restore();
            const int fn = guarded(t_dws_n, false, thread ? MAIN_TASK : BG_TASK);
            const bool ok = op == 1 && !fn && count_tag('PANC') == 0 && *(int32_t*)(MGR() + 0x158) == 32 &&
                            *(uint32_t*)(is + 0x3c) == 0xdead && count_tag('DELE') == 1;
            if (!ok) printf("    (%s thread: original panics %d; rewrite fault %d (%08x), panics %d, count %d, ISound %08x, deletes %d)\n",
                            thread ? "main" : "BG", op, fn, g_t_fault, count_tag('PANC'), *(int32_t*)(MGR() + 0x158),
                            *(uint32_t*)(is + 0x3c), count_tag('DELE'));
            check(ok, "(i) a 33rd sound to destroy when stopped: destroyed at once, no panic");
        }
    }

    // (g) the sound table: its capacity from the constructor's operand; stock 384, lifted 1024
    {
        const uint32_t op0 = *(uint32_t*)SM_TABLE_BYTES_OP;
        check(op0 == 0x600 && sound_capacity() == 384, "(g) the stock operand: 0x600, 384 sounds");
        for (int lifted = 0; lifted < 2; lifted++) {
            const int32_t cap = lifted ? 1024 : 384;
            if (lifted) *(uint32_t*)SM_TABLE_BYTES_OP = 0x1000;
            for (int full = 0; full < 2; full++) {
                calm_world(true);
                uint8_t* mgr = MGR();
                uint32_t* list = lifted ? g_big : (uint32_t*)AR(A_LIST);
                const int32_t n = full ? cap : cap - 1;
                for (int32_t i = 0; i < n; i++) list[i] = (uint32_t)(uintptr_t)sound_at(0);
                for (int i = 0; i < 16; i++) list[cap + i] = 0x5e5e5e5e;
                *(uint32_t**)(mgr + 4) = list;
                for (int k = 0; k < 8; k++) { *(uint32_t**)(mgr + 0xc + 8 * k) = list; *(int32_t*)(mgr + 0x10 + 8 * k) = 0; }
                *(int32_t*)(mgr + 0x10 + 8 * 7) = n;
                *(int32_t*)(mgr + 8) = n;
                g_t_s = sound_at(1);
                *(int32_t*)(g_t_s + 0x24) = 3;
                snap();
                static uint32_t list_snap[1024 + 16];
                memcpy(list_snap, list, 4 * (cap + 16));
                const int fo = guarded(t_add_o, false, BG_TASK);
                const bool o_over = memcmp(list_snap + cap, list + cap, 64) != 0;
                restore();
                memcpy(list, list_snap, 4 * (cap + 16));
                const int fn = guarded(t_add_n, false, BG_TASK);
                const bool n_over = memcmp(list_snap + cap, list + cap, 64) != 0;
                if (full) {
                    check(!fo && o_over && !fn && !n_over && *(int32_t*)(mgr + 8) == cap && g_fix_fired == 1,
                          lifted ? "(g) a full lifted table (1024): the original writes past it, the rewrite adds nothing"
                                 : "(g) a full stock table (384): the original writes past it, the rewrite adds nothing");
                } else {
                    check(!fn && !n_over && *(int32_t*)(mgr + 8) == cap && list[0] == (uint32_t)(uintptr_t)g_t_s && g_fix_fired == 0,
                          lifted ? "(g) the lifted table's last slot (the 1024th): added" : "(g) the stock table's last slot (the 384th): added");
                }
                // a sound made with the table full is no sound (not created); one slot short, it is made and added
                restore();
                memcpy(list, list_snap, 4 * (cap + 16));
                *(uint8_t**)S_MGR = mgr;
                memset(AR(A_OBJ), 0xcd, 0x100);
                const int fc = guarded(t_sctor_n, false, BG_TASK);
                check(!fc && (full ? *(uint32_t*)(AR(A_OBJ) + 0xc) == 0 && count_tag('CRSN') == 0
                                   : *(uint32_t*)(AR(A_OBJ) + 0xc) != 0 && count_tag('CRSN') == 1 && *(int32_t*)(mgr + 8) == cap),
                      full ? "(g) Sound::Sound with the table full: no sound" : "(g) Sound::Sound with one slot left: made and added");
                check(memcmp(list_snap + cap, list + cap, 64) == 0, "(g) nothing written past the table");
            }
            if (lifted) {                                               // the constructor allocates the operand's size
                calm_world(false);
                memset(AR(A_MGR2), 0xcd, 0x180);
                const int f = guarded(t_smctor_n, false, MAIN_TASK);
                bool saw = false;
                for (uint32_t i = 0; i + 1 < g_log.n; i++) if (g_log.w[i] == 'MALC' && g_log.w[i + 1] == 0x1000) saw = true;
                check(!f && saw, "(g) SoundManager::SoundManager with the lifted operand allocates 0x1000 bytes");
            }
            *(uint32_t*)SM_TABLE_BYTES_OP = op0;
        }
    }
    printf("directed fix tests (40 rounds): %s -- at the speed of sound the original divided by zero %d / %d times, the "
           "rewrite never; %d Doppler values beside it matched; with no manager the original faulted %d times; old-format "
           "resources: %d short made empty, %d cut as before\n",
           bad ? "FAILED" : "all passed", dop_orig_faults, dop_cases, dop_near_same, null_orig_faults, res_short, res_same);
    return bad;
}
#endif

// ---- main ----------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 100000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_snd_mgr.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(0x004d6850, (void*)&stub_crt_msgbox);        // __crtMessageBoxA
    ((void(__cdecl*)())0x004ce150)();                      // the CRT's floating point, as its start-up leaves it
    patch_jmp(0x004140e0, (void*)&stub_MemAlloc);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x004112b0, (void*)&stub_panic);
    patch_jmp(0x00411150, (void*)&stub_report);
    patch_jmp(0x00414dd0, (void*)&stub_task_id);
    patch_jmp(0x00414de0, (void*)&stub_sleep);
    patch_jmp(0x004150a0, (void*)&stub_multi_begin);
    patch_jmp(0x00415180, (void*)&stub_multi_enter);
    patch_jmp(0x004151d0, (void*)&stub_multi_leave);
    patch_jmp(0x00415220, (void*)&stub_multi_end);
    patch_jmp(0x004189f0, (void*)&stub_bghook);
    patch_jmp(0x00418ac0, (void*)&stub_bgunhook);
    patch_jmp(0x00473750, (void*)&stub_mixer_begin);
    patch_jmp(0x00473880, (void*)&stub_mixer_end);
    patch_jmp(0x00473850, (void*)&stub_mixer_get);
    patch_jmp(0x00473870, (void*)&stub_mixer_default);
    patch_jmp(0x00473b70, (void*)&stub_mixer_quality);
    patch_jmp(0x00419fa0, (void*)&stub_resource_get);
    patch_jmp(0x0041a450, (void*)&stub_resource_forget);
    patch_jmp(0x004728f0, (void*)&stub_engine_ctor);
    patch_jmp(0x00472c10, (void*)&stub_engine_dtor);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_snap = (uint8_t*)malloc(ARENA_BYTES);
    g_arena_after = (uint8_t*)malloc(ARENA_BYTES);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);

    static Footprint fp;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    int unmasked_runs = 0;
    int differ = 0, faults = 0, fault_both = 0, fp_bad = 0, replay_only = 0, wild_runs = 0, wild_differ = 0;
    int per[N_KINDS] = {0}, per_bad[N_KINDS] = {0}, per_fault[N_KINDS] = {0}, per_fix[N_KINDS] = {0};
    int fix_worlds = 0, fix_saved = 0, fix_faulted = 0, fix_both_faulted = 0;
#ifdef VP_SND_FIXES
    const int directed_bad = directed_fix_tests();
#else
    const int directed_bad = 0;
#endif
    long long log_words = 0;
    // coverage, from the original's results
    int c_panic = 0, c_report = 0, c_sleep = 0, c_hang = 0, c_create = 0, c_idel = 0, c_play = 0, c_stop = 0, c_mute = 0,
        c_unmute = 0, c_set3d = 0, c_pan = 0, c_muted_some = 0, c_unmuted_some = 0, c_s3_near = 0, c_s3_close = 0,
        c_s3_in = 0, c_s3_out = 0, c_prio_focus = 0, c_alloc_null = 0, c_mupd = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weights[i];
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), kk = 0;
        while (pick >= kind_weights[kk]) pick -= kind_weights[kk++];
        const Kind kind = (Kind)kk;
        random_world();
        random_args(kind);
        g_world_task = g_task;
        const bool wild = chance(10);
        if (wild) { wildify(); wild_runs++; }
        g_pc = chance(50) ? _PC_53 : _PC_24;
        g_unmask = chance(30);
        if (g_unmask) unmasked_runs++;
        if (trace) printf("world %d: %s\n", it, kind_names[kind]);
#if defined(__GNUC__) && !defined(__clang__)
        if (vp_try([&] { footprint_of(kind, fp); })) {
            printf("  FOOTPRINT world %d (%s): the footprint function faulted\n", it, kind_names[kind]);
            fp_bad++;
            continue;
        }
#else
        __try { footprint_of(kind, fp); } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("  FOOTPRINT world %d (%s): the footprint function faulted\n", it, kind_names[kind]);
            fp_bad++;
            continue;
        }
#endif
        if (fp.replay_only) replay_only++;
        memcpy(g_arena_snap, g_arena, ARENA_BYTES);
        memcpy(g_data_snap, DATA, DATA_BYTES);
        uint64_t ro = 0, rn = 0;
        int fo = run_guarded(kind, false, &ro);
        const uint32_t fo_code = g_fault_code, fo_eip = g_fault_eip;
        memcpy(g_arena_after, g_arena, ARENA_BYTES);
        memcpy(g_data_after, DATA, DATA_BYTES);
        g_log_orig = g_log;
        memcpy(g_arena, g_arena_snap, ARENA_BYTES);
        memcpy(DATA, g_data_snap, DATA_BYTES);
        int fn = run_guarded(kind, true, &rn);
        per[kind]++;
        log_words += g_log_orig.n;
        if (g_fix_fired) {                                           // a fix changed this world: counted, not compared
            fix_worlds++;
            per_fix[kind]++;
            if (fo && !fn) fix_saved++;
            if (fo && fn) fix_both_faulted++;                       // (from causes no fix covers, as in the original)
            if (fn && !fo) {
                fix_faulted++;
                printf("  (world %d, %s: a fix fired and the rewrite faulted where the original didn't, %08x at %08x)\n", it,
                       kind_names[kind], g_fault_code, g_fault_eip);
            }
            continue;
        }
        if (fo || fn) {
            faults++;
            per_fault[kind]++;
            if (fo && fn) {
                fault_both++;
                if (fo_code == 0xe0000001u) c_hang++;
                if (trace) printf("  (both faulted: %08x at %08x, address %08x)\n", fo_code, fo_eip, g_fault_addr);
                if (fo_code != g_fault_code) {
                    printf("  world %d (%s): both faulted, differently: %08x at %08x / %08x at %08x\n", it, kind_names[kind], fo_code, fo_eip, g_fault_code, g_fault_eip);
                    differ++; per_bad[kind]++;
                }
                continue;
            }
            printf("  world %d (%s): %s / %s; fault %08x at %08x (original's %08x at %08x), address %08x\n", it, kind_names[kind],
                   fo ? "original faulted" : "original ran", fn ? "rewrite faulted" : "rewrite ran", g_fault_code, g_fault_eip,
                   fo_code, fo_eip, g_fault_addr);
            differ++; per_bad[kind]++;
            if (differ >= 30) { printf("stopping after 30 differing worlds\n"); break; }
            continue;
        }
        if (returns_byte(kind)) { ro &= 0xff; rn &= 0xff; }
        bool same = true;
        char buf[64];
        for (uint32_t i = 0; i < ARENA_BYTES; i += 4)
            if (memcmp(g_arena_after + i, g_arena + i, 4)) {
                uint32_t x, y; memcpy(&x, g_arena_after + i, 4); memcpy(&y, g_arena + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
                printf("    %s original %08x (%.9g) rewrite %08x (%.9g)\n", where((uint32_t)(uintptr_t)(g_arena + i), buf), x, bitsf(x), y, bitsf(y));
                same = false;
                break;
            }
        for (uint32_t i = 0; i < DATA_BYTES; i += 4)
            if (memcmp(g_data_after + i, DATA + i, 4)) {
                uint32_t x, y; memcpy(&x, g_data_after + i, 4); memcpy(&y, DATA + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
                printf("    %s original %08x (%.9g) rewrite %08x (%.9g)\n", where((uint32_t)(uintptr_t)(DATA + i), buf), x, bitsf(x), y, bitsf(y));
                same = false;
                break;
            }
        if (ro != rn) {
            if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
            printf("    return: original %016llx rewrite %016llx\n", (unsigned long long)ro, (unsigned long long)rn);
            same = false;
        }
        if (g_log.n != g_log_orig.n || memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX))) {
            if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
            printf("    the stub logs differ: %u / %u words\n", g_log_orig.n, g_log.n);
            uint32_t m = g_log.n < g_log_orig.n ? g_log.n : g_log_orig.n;
            if (m > LOG_MAX) m = LOG_MAX;
            for (uint32_t i = 0; i < m; i++) if (g_log.w[i] != g_log_orig.w[i]) { printf("    first at %u: %08x / %08x\n", i, g_log_orig.w[i], g_log.w[i]); break; }
            same = false;
        }
        if (!same) {
            differ++; per_bad[kind]++;
            if (wild) wild_differ++;
            printf("    (pc %s, wild %d, exceptions %s)\n", g_pc == _PC_24 ? "24" : "53", wild, g_unmask ? "unmasked" : "masked");
            if (differ >= 30) { printf("stopping after 30 differing worlds\n"); break; }
        }
        // the original's writes against the footprint
        if (!fp.replay_only) {
            bool bad = false;
            for (uint32_t i = 0; i < ARENA_BYTES && !bad; i++)
                if (g_arena_after[i] != g_arena_snap[i] && !in_footprint(fp, g_arena + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kind_names[kind], where((uint32_t)(uintptr_t)(g_arena + i), buf));
                    bad = true;
                }
            for (uint32_t i = 0; i < DATA_BYTES && !bad; i++)
                if (g_data_after[i] != g_data_snap[i] && !in_footprint(fp, DATA + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kind_names[kind], where(0x4e1000 + i, buf));
                    bad = true;
                }
            fp_bad += bad;
        }
        // coverage
        auto count = [&](uint32_t tag) { int c = 0; for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) if (g_log_orig.w[i] == tag) c++; return c; };
        c_panic += count('PANC') > 0; c_report += count('REPT') > 0; c_sleep += count('SLEP') > 0; c_create += count('CRSN') > 0;
        c_idel += count('IDEL') > 0; c_play += count('PLAY') + count('PLLP') > 0; c_stop += count('STOP') > 0;
        c_mute += count('MUTE') > 0; c_unmute += count('UNMU') > 0; c_set3d += count('S3D ') > 0; c_pan += count('SPAN') > 0;
        c_mupd += count('MUPD') > 0;
        if (g_script.alloc_fail && count('MALC')) c_alloc_null++;
        if (kind == K_MUTE_SOUNDS) {
            bool m1 = false, m0 = false;
            for (int i = 0; i < g_w.n; i++) {
                const uint32_t o = (uint32_t)(sound_at(i) - g_arena) + 0x28;
                if (g_arena_after[o] == 1 && g_arena_snap[o] != 1) m1 = true;
                if (g_arena_after[o] == 0 && g_arena_snap[o] != 0) m0 = true;
            }
            c_muted_some += m1; c_unmuted_some += m0;
        }
        if (kind == K_S3_UPD) {
            const uint8_t* s0 = g_arena_snap + (g_args.self - g_arena);
            if (s0[0x30]) {
                const float* p = (const float*)(g_arena_snap + (*(uint8_t**)(s0 + 0x3c) - g_arena));
                const float* lp = (const float*)(*(uint8_t**)(ARG(G_LIST) + 4) + 0x24);
                const float d = sqrtf((p[0] - lp[0]) * (p[0] - lp[0]) + (p[1] - lp[1]) * (p[1] - lp[1]) + (p[2] - lp[2]) * (p[2] - lp[2]));
                if (d < 0.1f) c_s3_close++; else if (d < 10.0f) c_s3_near++;
            }
        }
        if (kind == K_S3_UPDSTATUS) (g_arena_after[(g_args.self - g_arena) + 0x30] ? c_s3_in : c_s3_out)++;
        if (kind == K_HAS_PRIORITY && *(int32_t*)(g_args.self + 0x24) == 3 && *(int32_t*)(g_args.p + 0x24) == 3 &&
            (*(int32_t*)(g_args.self + 0x10) == *(int32_t*)X_FOCUS || *(int32_t*)(g_args.p + 0x10) == *(int32_t*)X_FOCUS)) c_prio_focus++;
    }
    long long checks = 0;
    for (int i = 0; i < N_KINDS; i++) checks += per[i];
    printf("%d worlds (%d wild, %d with overflow / divide-by-zero unmasked): %d differ (%d of them wild), %d faulted (%d in both, %d of those waits that never end), %d changed bytes outside the footprint, %d replay-only; %lld logged words compared\n",
           iterations, wild_runs, unmasked_runs, differ, wild_differ, faults, fault_both, c_hang, fp_bad, replay_only, log_words);
    printf("per function (worlds / differing / faulted in both):\n");
    for (int i = 0; i < N_KINDS; i++) printf("  %-52s %6d / %d / %d\n", kind_names[i], per[i], per_bad[i], per_fault[i]);
    printf("coverage (worlds): LogPanic %d, LogReport %d, slept %d, CreateSound %d, ISound deleted %d, Play %d, Stop %d, Mute %d, UnMute %d, Set3D %d, SetPanning %d, mixer Update %d, NULL allocations %d\n",
           c_panic, c_report, c_sleep, c_create, c_idel, c_play, c_stop, c_mute, c_unmute, c_set3d, c_pan, c_mupd, c_alloc_null);
    printf("mute_sounds: muted some %d, unmuted some %d; Sound3D::update_sound within 10 m %d, within 0.1 m %d; update_status in range %d / out %d; has_priority focus-car path %d\n",
           c_muted_some, c_unmuted_some, c_s3_near, c_s3_close, c_s3_in, c_s3_out, c_prio_focus);
#ifdef VP_SND_FIXES
    printf("fixes: %d worlds a fix changed (the original faulted and the rewrite didn't in %d; both faulted in %d; the rewrite alone faulted in %d); "
           "every other world compared as above. Per function:\n", fix_worlds, fix_saved, fix_both_faulted, fix_faulted);
    for (int i = 0; i < N_KINDS; i++) if (per_fix[i]) printf("  %-52s %6d\n", kind_names[i], per_fix[i]);
#endif
    return differ || fp_bad || directed_bad || fix_faulted ? 1 : 0;
}
