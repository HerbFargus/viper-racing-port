// world_wld_world.cpp -- loading the world (hook/wld_world.cpp: world.obj, carmgr.obj, event.obj) against the
// originals, outside the game (docs/PORTING.md, step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_wld_world.cpp
//        /Fo%TEMP%\w2\ /Fe%TEMP%\w2\world_wld_world.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_wld_world.exe [worlds] [seed] [obt-dir]
//
// The world files: every track's track.obt, as text (one record per line), in obt-dir (default %TEMP%\w2\obt) --
// extracted read-only from the test install's .trk / .tra files and the community tracks (never copied into the
// repository). Each world takes one of them as it is, mutates one (rows dropped, duplicated, cut, re-cased,
// numbers turned to nan / inf / 1e39 / -0, comments, missing "end", text after it, fields missing) or makes one
// (many obstacles / wobbles / statics / checkpoints, odd counts, bad kinds), and a random car list and options.
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. Everything the functions call outside this group
// is a stub that logs its call and answers from a per-call script: the track and its string table (the rows above),
// the car files (CarFileLoad fills the CarData), the terrain (a height from the position's bits), the models and
// their extents, MemAlloc (a bump heap in the arena, now and then failing), the object constructors (CarObject,
// GhostCarObject, ModelObject, WobbleObject: a logging vtable, and the PhobData appended to a fake creation
// stream), PhysicsCreate (the same stream), the centre line (CenterLine, convert_meters_to_ilpos over fake
// segments, ILSeg::QuickTan), the begin / end of every other part, the physics' packet, the camera, the options,
// the Single* locks, LogReport, LogPanic (which, as in the game, doesn't return: the stub raises an exception that
// ends the call, in both passes), ASSERT_MSG, the event handlers, the CRT's sscanf (the game's own _input, or the
// host's, per world), stricmp / strchr / sprintf (the host's). The game's qsort, MatrixConcat, CrossProduct run from
// the image. This group's own functions run as the originals from the image when another calls them; a "chain"
// check instead has every rewrite of the group hooked into the image for the rewrite's pass, so the original chain
// (WorldBeginCommon -> parse_world -> parse_object -> parse_car -> ...) is compared with the rewritten one.
//
// For each call the original runs, the world (arena, image statics) and the log are kept, the world restored, the
// rewrite runs, and the world, the return value, the log and any fault / panic are compared; every byte the
// original changed must lie inside the rewrite's footprint unless it's replay_only (all of this runs on the main
// thread: no statics are saved automatically). Before each pass the stack below is filled with one pattern, so an
// uninitialised local reads the same in both. Checks run at 53- or 24-bit precision. One case can't match in a
// chain: get_start_line on a reversed track whose first usable checkpoint row has exactly 4 fields reads its
// uninitialised second z, whose value then comes from whatever the earlier callees left on the stack -- such a
// world's begin is checked alone (counted as "begins checked alone").
//
// The game's CRT runs here without its start-up: _cfltcvt_init is called (its %f converters), KERNEL32's imports are
// resolved, and its fatal-error paths (_amsg_exit, _NMSG_WRITE, _FF_MSGBANNER, _fptrap, __crtMessageBoxA) are
// stubs that end the child quietly; SetErrorMode keeps Windows' own error boxes away. Nothing may open a dialog.
// Faults both passes share are expected: e.g. more than 0x400 world objects (Balls2000) overrun the object lists
// into the counts after them, as in the game.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <tuple>
#include <type_traits>
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#include "../hook/port.h"

struct ChainReg {
    uint32_t at; void* fn; const char* name; ChainReg* next;
    static ChainReg*& head() { static ChainReg* h; return h; }
    ChainReg(uint32_t a, void* f, const char* n) : at(a), fn(f), name(n), next(head()) { head() = this; }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;                                                        \
    static ChainReg VP_CAT(chain_, NEW)(V10, (void*)&NEW, NAME);

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
uint32_t A(uint32_t v10) { return v10; }
bool have(uint32_t) { return true; }
bool build_is_v10() { return true; }

void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 0x844, what); }

#include "../hook/wld_world.cpp"
#include "vrmod_image.h"
// VP_VRMOD=1: vrmod's hornball.py tunings -- create_ball's mass and collision radius immediates -- set to random player
// values before each check of it (docs/FIXES.md, "vrmod's patches": the rewrite reads them from the code)
static bool g_vrmod;

// ---- random values ------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static int g_wno_for_crt;
static uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static uint32_t hash32(uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
static uint32_t hash_bytes(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static uint32_t hash_str(const char* s) { return s ? hash_bytes(s, strlen(s)) : 0xdead; }

// ---- the original, loaded at 0x400000 (as test/fuzz.cpp) --------------------------------------------------------
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
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, nt->OptionalHeader.SizeOfImage, MEM_COMMIT,
                                           PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) { printf("0x400000 isn't reserved for race.exe in this process\n"); return false; }
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        memcpy(base + sec[i].VirtualAddress, file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData
                                                                                         : sec[i].Misc.VirtualSize);
    // KERNEL32's imports resolved (the game's CRT -- its sscanf's character types -- reaches GetStringTypeA)
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.Size && d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        if (_stricmp(dll, "KERNEL32.dll")) continue;
        HMODULE m = GetModuleHandleA("kernel32.dll");
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            FARPROC f = GetProcAddress(m, fn);
            if (f) iat->u1.Function = (DWORD)(uintptr_t)f;
        }
    }
    free(file);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_WORLD_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) return 2;
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

// the chain: every rewrite of the group hooked into the image (for a rewrite's pass)
struct ChainSave { uint32_t at; uint8_t b[5]; };
static ChainSave g_chain_save[128];
static int g_nchain;
static void chain_patch() {
    g_nchain = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        ChainSave& s = g_chain_save[g_nchain++];
        s.at = r->at;
        memcpy(s.b, (void*)r->at, 5);
        patch_jmp(r->at, r->fn);
    }
}
static void chain_unpatch() {
    for (int i = g_nchain - 1; i >= 0; i--) memcpy((void*)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_nchain = 0;
}

// ---- the arena -------------------------------------------------------------------------------------------------------
enum {
    ARENA_SIZE = 0xC0000,
    OFF_CTRL = 0x0000, OFF_WORLD = 0x0200, OFF_MISC = 0x1000, OFF_PKTSRC = 0x2000, OFF_SEGS = 0x3000, NSEGS = 64,
    SEG_STRIDE = 0x30, OFF_OBJS = 0x4000, NOBJS = 64, OBJ_STRIDE = 0x40, OFF_STREAM = 0x5000, STREAM_CAP = 0x3fff0,
    OFF_HEAP = 0x45000, HEAP_SIZE = 0x78000,
};
// misc outputs
enum { M_FRAME = 0x000, M_POINT = 0x040, M_MATRIX = 0x050, M_TEXNAME = 0x080, M_CARINFO = 0x180, M_UPGRADES = 0x200,
       M_EVENTS = 0x300 /* a PhysicsEventStream, 0x404 */ };
static uint8_t* g_arena;

struct Ctrl {
    uint32_t heap_top;
    uint32_t mark;
    uint32_t alloc_fail;          // MemAlloc fails when (script & 511) < this
    uint32_t load_fail;           // CarFileLoad returns 0 below this (pct)
    uint32_t line_fail;           // convert_meters_to_ilpos finds no segment below this (pct)
    uint32_t line_len;            // CenterLine +0x5c (bits)
    uint32_t terrain_salt;
    uint32_t terrain_special;     // pct of special heights
    uint32_t cd_clobber;          // CarFileLoad overwrites CarData::car (pct)
    uint8_t track_ok, ai_ok, hack_nowalls, multi_human, mr_enabled, game_scanf, _p[2];
    int32_t track_number, max_focus, ghost_target, opt_value, opt_write;
    uint32_t phys_time;           // PhysicsGetTime (float bits)
    uint32_t screen_wid_next;     // ViewPreBegin changes gxScreenWid to this (0: leaves it)
    int32_t evh_ret[8];           // the event handlers' answers (-2: from the data)
};
static Ctrl* ctrl() { return (Ctrl*)(g_arena + OFF_CTRL); }
struct FakeStream { int32_t len, tail; uint8_t data[STREAM_CAP]; };
static FakeStream* stream() { return (FakeStream*)(g_arena + OFF_STREAM); }
static World* world() { return (World*)(g_arena + OFF_WORLD); }
static uint8_t* misc(uint32_t off) { return g_arena + OFF_MISC + off; }
static uint8_t* seg_at(int i) { return g_arena + OFF_SEGS + i * SEG_STRIDE; }
static uint8_t* obj_at(int i) { return g_arena + OFF_OBJS + i * OBJ_STRIDE; }

// the image statics the functions read or write, snapshotted with the arena
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_globals[] = {
    {0x004f4354, 0x1c, "world statics (0x4f4354..)"}, {0x004f48d0, 0x0c, "carmgr statics"},
    {0x004ff5a8, 4, "CareerUpgrades"}, {0x00521614, 4, "physics camera_focus"}, {0x005228f4, 4, "gxScreenWid"},
    {0x00522a1c, 4, "detail"}, {0x005512a0, 0x44f0, "world statics (0x5512a0..0x555790)"},
    {0x00557ee0, 0x84, "event table"},
};
enum { GLOBALS_BYTES = 0x1c + 0x0c + 4 + 4 + 4 + 4 + 0x44f0 + 0x84 };
static const char* global_name(uint32_t a) {
    struct { uint32_t at, n; const char* nm; } t[] = {
        {0x004f4354, 4, "player"}, {0x004f4358, 4, "focus"}, {0x004f435c, 4, "world single"}, {0x004f4364, 4, "MIRROR_X"},
        {0x005512a0, 0x1000, "wobs[]"}, {0x005522a0, 12, "blimp"}, {0x005522b0, 4, "mirror"}, {0x005522b4, 4, "cars made"},
        {0x005522bc, 1, "began"}, {0x005522c8, 4, "shadow"}, {0x005522d0, 4, "nwobs"}, {0x005522d8, 0x1000, "gobs[]"},
        {0x005532d8, 4, "track number"}, {0x005532e0, 0x30, "start line"}, {0x00553314, 1, "pkt byte"},
        {0x0055331c, 4, "frames"}, {0x00553320, 4, "specular"}, {0x00553328, 0x30, "camera"}, {0x00553358, 4, "skids"},
        {0x0055335c, 4, "camera type"}, {0x00553360, 4, "race state"}, {0x00553368, 4, "ngobs"}, {0x0055336c, 4, "smoke"},
        {0x00553378, 0xcd4, "world copy"}, {0x0055404c, 1, "fx"}, {0x00554064, 1, "replay"}, {0x0055406c, 4, "frame ms"},
        {0x00554070, 4, "ghosts"}, {0x00554078, 16, "registered"}, {0x00554090, 0x1700, "carmgr info"},
        {0x00557ee0, 0x80, "handlers"}, {0x00557f60, 4, "nhandlers"},
    };
    for (auto& x : t)
        if (a >= x.at && a < x.at + x.n) return x.nm;
    return "?";
}

// ---- the stubs' script and log ---------------------------------------------------------------------------------------
struct LogEntry { uint32_t kind, a[5], globals; };
enum { LOGN = 1 << 16 };
static LogEntry g_log[2][LOGN];
static int g_nlog[2], g_pass;
static uint32_t g_script[256];
static int g_si;
static uint32_t script() { return g_script[g_si++ & 255]; }
static uint32_t globals_hash() {
    const uint32_t at[] = {0x00553368, 0x005522d0, 0x005522b4, 0x00554070, 0x004f4358, 0x004f4354, 0x004f48d0,
                           0x00557f60, 0x00553360, 0x0055404c};
    uint32_t h = 2166136261u;
    for (uint32_t a : at) h = (h ^ *(uint32_t*)a) * 16777619u;
    h ^= hash_bytes((void*)0x005532e0, 0x30);
    h = (h ^ (uint32_t)stream()->len) * 16777619u;
    return h;
}
static void logn(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    if (g_nlog[g_pass] < LOGN) g_log[g_pass][g_nlog[g_pass]] = {kind, {a0, a1, a2, a3, a4}, globals_hash()};
    g_nlog[g_pass]++;
}
// a pointer as the log sees it: the arena and the image as they are; the stack (whose layout differs between the
// original and the rewrite) as a marker
static uint32_t P(const void* p) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    if ((uint8_t*)p >= (uint8_t*)tib->StackLimit && (uint8_t*)p < (uint8_t*)tib->StackBase) return 0x57ac0000u;
    return (uint32_t)(uintptr_t)p;
}
static uint32_t mark(uint32_t salt) {
    Ctrl* c = ctrl();
    c->mark = c->mark * 0x9e3779b1u + salt + (uint32_t)g_nlog[g_pass];
    return c->mark;
}

#define LOG_KINDS(X)                                                                                             \
    X(TRACKNO) X(OPTGET) X(S_BEGIN) X(S_ENTER) X(S_LEAVE) X(S_END) X(VIEW_PRE) X(VIEW_POST) X(VIEW_BEGIN)          \
    X(VIEW_END) X(TRACKLOAD) X(TRACKUNLOAD) X(FX_BEGIN) X(FX_END) X(REC_BEGIN) X(REC_END) X(PH_BEGIN) X(PH_START)   \
    X(PH_STOP) X(PH_END) X(AI_BEGIN) X(AI_END) X(AI_TWEAK) X(MR_FLUSH) X(CAM_SETFOCUS) X(CAM_MAXFOCUS) X(PANIC)    \
    X(REPORT) X(GH_BEGIN) X(GH_END) X(GH_SAVE) X(GH_TARGET) X(PH_TIME) X(PKT_GET) X(PKT_REL) X(PROCESS_PKT)       \
    X(MR_ENABLED) X(STRICMP) X(STRCHR) X(SSCANF) X(SPRINTF) X(ST_GET) X(ST_FORGET) X(ST_ROWS) X(ST_ENTRY)         \
    X(CARFILE) X(TERRAIN) X(MEMALLOC) X(CTOR_CAR) X(CTOR_GHOST) X(CTOR_MODEL) X(CTOR_WOBBLE) X(CL_CTOR) X(CL_DTOR) \
    X(CL_CONVERT) X(QUICKTAN) X(NOWALLS) X(MR_LOAD) X(MR_EXTENTS) X(MR_UNLOAD) X(PH_CREATE) X(MULTI_HUMAN)       \
    X(ASSERT_W) X(ASSERT_C) X(EVH) X(V_DTOR) X(V_TEX) X(V_KEY) X(V_BAD)
enum LogKind {
    L_NONE,
#define X(n) L_##n,
    LOG_KINDS(X)
#undef X
};
static const char* const g_log_names[] = {
    "?",
#define X(n) #n,
    LOG_KINDS(X)
#undef X
};

// ---- stubs -----------------------------------------------------------------------------------------------------------
#define VSTUB(NAME, KIND) static void __cdecl NAME() { logn(KIND); }
VSTUB(st_view_post, L_VIEW_POST) VSTUB(st_view_end, L_VIEW_END) VSTUB(st_track_unload, L_TRACKUNLOAD)
VSTUB(st_fx_begin, L_FX_BEGIN) VSTUB(st_fx_end, L_FX_END) VSTUB(st_rec_end, L_REC_END) VSTUB(st_ph_start, L_PH_START)
VSTUB(st_ph_stop, L_PH_STOP) VSTUB(st_ph_end, L_PH_END) VSTUB(st_ai_end, L_AI_END) VSTUB(st_mr_flush, L_MR_FLUSH)
VSTUB(st_gh_save, L_GH_SAVE) VSTUB(st_pkt_rel, L_PKT_REL)
#undef VSTUB
static void __cdecl st_view_pre() {
    logn(L_VIEW_PRE);
    if (ctrl()->screen_wid_next) *(uint32_t*)0x005228f4 = ctrl()->screen_wid_next;
}
static int __cdecl st_trackno(const char* n) { logn(L_TRACKNO, P(n), hash_str(n)); return ctrl()->track_number; }
static void __cdecl st_optget(const char* sec, const char* key, int* out) {
    logn(L_OPTGET, P(sec), P(key), P(out));
    if (ctrl()->opt_write) *out = ctrl()->opt_value + (int)(hash_str(key) & 3);
}
static int __cdecl st_s_begin(const char* n) { logn(L_S_BEGIN, P(n)); return (int)(script() & 0xff) + 1; }
static void __cdecl st_s_enter(int h, const char* f, int l) { logn(L_S_ENTER, h, P(f), l); }
static void __cdecl st_s_leave(int h, const char* f, int l) { logn(L_S_LEAVE, h, P(f), l); }
static void __cdecl st_s_end(int h, const char* f, int l) { logn(L_S_END, h, P(f), l); }
static void __cdecl st_view_begin(const void* o) { logn(L_VIEW_BEGIN, P(o)); }
static uint8_t __cdecl st_trackload(const char* n) { logn(L_TRACKLOAD, P(n), hash_str(n)); return ctrl()->track_ok; }
static void __cdecl st_rec_begin(const void* w) { logn(L_REC_BEGIN, P(w)); }
static void __cdecl st_ph_begin(const void* o, int n) { logn(L_PH_BEGIN, P(o), (uint32_t)n); }
static uint8_t __cdecl st_ai_begin(void* cl, const char* n) { logn(L_AI_BEGIN, P(cl), P(n)); return ctrl()->ai_ok; }
static void __cdecl st_ai_tweak(uint8_t* cd) {
    logn(L_AI_TWEAK, P(cd), hash_bytes(cd, 0x1e4));
    *(uint32_t*)(cd + 0x60) ^= mark(0x71);
}
static void __cdecl st_cam_setfocus(int n) { logn(L_CAM_SETFOCUS, (uint32_t)n); *(int32_t*)0x00521614 = n; }
static int __cdecl st_cam_maxfocus() { logn(L_CAM_MAXFOCUS); return ctrl()->max_focus; }
static void __cdecl st_gh_begin(const void* w) { logn(L_GH_BEGIN, P(w)); }
static void __cdecl st_gh_end(const void* w) { logn(L_GH_END, P(w)); }
static int __cdecl st_gh_target() { logn(L_GH_TARGET); return ctrl()->ghost_target; }
static double __cdecl st_ph_time() { logn(L_PH_TIME); return (double)fbits(ctrl()->phys_time) * 1.0000001; }
static uint8_t* __cdecl st_pkt_get(uint8_t* pkt, uint8_t* ev) {
    logn(L_PKT_GET, P(pkt), P(ev));
    memcpy(pkt, g_arena + OFF_PKTSRC, 0x3d8);
    memcpy(ev, misc(M_EVENTS), 0x404);
    return g_arena + OFF_PKTSRC + 0x800;
}
static void __fastcall st_process_pkt(void* self, Edx, const uint8_t* msgs) { logn(L_PROCESS_PKT, P(self), P(msgs)); }
static uint8_t __cdecl st_mr_enabled(int f) { logn(L_MR_ENABLED, (uint32_t)f); return ctrl()->mr_enabled; }
static uint8_t __cdecl st_nowalls() { logn(L_NOWALLS); return ctrl()->hack_nowalls; }
static uint8_t __cdecl st_multi_human(int car) { logn(L_MULTI_HUMAN, (uint32_t)car); return (uint8_t)((ctrl()->multi_human >> (car & 7)) & 1); }

// messages: the formats with arguments, decoded (a %s from the stack is hashed)
enum { A_NONE, A_INT, A_STR, A_DOUBLE_INT, A_INT_DOUBLE };
static int fmt_args(const char* fmt) {
    switch ((uint32_t)(uintptr_t)fmt) {
    case 0x004f4418: return A_DOUBLE_INT;   // possible fps: %f (%d)
    case 0x004f4430: case 0x004f4698: return A_INT;
    case 0x004f4614: case 0x004f4704: case 0x004f475c: case 0x004f47bc: case 0x004f482c: return A_STR;
    case 0x004f45c0: return A_INT_DOUBLE;
    default: return A_NONE;
    }
}
static void log_msg(uint32_t kind, const char* fmt, va_list ap) {
    uint32_t a = 0, b = 0, c = 0;
    switch (fmt_args(fmt)) {
    case A_INT: a = va_arg(ap, uint32_t); break;
    case A_STR: a = hash_str(va_arg(ap, const char*)); break;
    case A_DOUBLE_INT: { double d = va_arg(ap, double); memcpy(&a, &d, 4); memcpy(&b, (uint8_t*)&d + 4, 4); c = va_arg(ap, uint32_t); break; }
    case A_INT_DOUBLE: { a = va_arg(ap, uint32_t); double d = va_arg(ap, double); memcpy(&b, &d, 4); memcpy(&c, (uint8_t*)&d + 4, 4); break; }
    }
    logn(kind, P(fmt), a, b, c);
}
enum { PANIC_CODE = 0xE0005050 };
static int g_panics;
static void __cdecl st_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_msg(L_PANIC, fmt, ap);
    va_end(ap);
    RaiseException(PANIC_CODE, 0, 0, 0);          // _LogPanic -> abend: it doesn't return
}
static void __cdecl st_report(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_msg(L_REPORT, fmt, ap);
    va_end(ap);
}
static void __cdecl st_assert_w(int cond, const char* fmt, ...) { logn(L_ASSERT_W, (uint32_t)cond, P(fmt)); }
static void __cdecl st_assert_c(int cond, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t a = va_arg(ap, uint32_t), b = va_arg(ap, uint32_t), c = 0, d = 0;
    if (fmt == (const char*)0x004f4970 || fmt == (const char*)0x004f49a8) { c = va_arg(ap, uint32_t); d = va_arg(ap, uint32_t); }
    va_end(ap);
    logn(L_ASSERT_C, (uint32_t)cond, P(fmt), a, b, c ^ d * 31);
}
// the CRT
static int __cdecl st_stricmp(const char* a, const char* b) {
    int r = _stricmp(a, b);
    logn(L_STRICMP, hash_str(a), P(b), (uint32_t)(r == 0));
    return r;
}
static char* __cdecl st_strchr(const char* s, int c) {
    char* r = (char*)strchr(s, c);
    logn(L_STRCHR, hash_str(s), (uint32_t)c, r ? (uint32_t)(r - s) : 0xffffffffu);
    return r;
}
struct GameFile { char* ptr; int cnt; char* base; int flag, file, charbuf, bufsiz; char* tmpfname; };
typedef int(__cdecl* Input_t)(GameFile*, const char*, va_list);
static int __cdecl st_sscanf(const char* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r;
    if (ctrl()->game_scanf) {
        GameFile f = {(char*)s, (int)strlen(s), (char*)s, 0x49, 0, 0, 0, 0};     // as the game's sscanf builds it
        r = ((Input_t)0x004d0ee0)(&f, fmt, ap);
    } else {
        r = vsscanf(s, fmt, ap);
    }
    va_end(ap);
    logn(L_SSCANF, hash_str(s), P(fmt), (uint32_t)r);
    return r;
}
static int __cdecl st_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    logn(L_SPRINTF, P(buf), P(fmt), hash_str(buf), (uint32_t)n);
    return n;
}
// the string table: track.obt's rows
static std::vector<std::string> g_rows;
static uint8_t g_table_handle[16];
static void* __cdecl st_st_get(const char* n) { logn(L_ST_GET, P(n)); return g_table_handle; }
static void __cdecl st_st_forget(void* t) { logn(L_ST_FORGET, P(t)); }
static int __cdecl st_st_rows(const void* t) { logn(L_ST_ROWS, P(t)); return (int)g_rows.size(); }
static const char* __cdecl st_st_entry(const void* t, int row, int col) {
    logn(L_ST_ENTRY, P(t), (uint32_t)row, (uint32_t)col);
    if (row < 0 || row >= (int)g_rows.size()) return "";
    return g_rows[row].c_str();
}
// the car file: the CarData filled (all but the car index, now and then that too)
static uint8_t __cdecl st_carfile(uint8_t* cd, const void* setup, const char* file, uint8_t* up) {
    logn(L_CARFILE, P(cd), P(setup), hash_str(file), P(up));
    Ctrl* c = ctrl();
    uint32_t s = hash32(hash_str(file) ^ mark(0x33));
    bool clobber = (script() % 100) < c->cd_clobber;
    for (uint32_t k = clobber ? 0 : 8; k < 0x1e4; k += 4) {
        if (!clobber && k == 0x194) continue;
        s = hash32(s + k);
        uint32_t v = (s & 3) == 0 ? s : ubits((float)(int32_t)(s % 2000) * 0.25f);
        memcpy(cd + k, &v, 4);
    }
    return (uint8_t)((script() % 100) >= c->load_fail);
}
// the terrain: a height from the position's bits
static double __cdecl st_terrain(uint32_t x, uint32_t z) {
    Ctrl* c = ctrl();
    uint32_t h = hash32(x * 0x9e3779b1u ^ z ^ c->terrain_salt);
    logn(L_TERRAIN, x, z);
    float y = (h % 100) < c->terrain_special ? fbits(h % 3 == 0 ? 0x7fc00000u : h % 3 == 1 ? 0x7f800000u : 0xff800000u)
                                             : (float)(int32_t)(h & 0xffff) / 256.0f - 40.0f;
    return (double)y * 1.00000003;                                // (an ST0 with more than 24 bits)
}
static void* __cdecl st_memalloc(int n) {
    Ctrl* c = ctrl();
    uint32_t s = script();
    if ((s & 511) < c->alloc_fail) { logn(L_MEMALLOC, (uint32_t)n, 0); return 0; }
    uint8_t* p = g_arena + OFF_HEAP + c->heap_top;
    c->heap_top += ((uint32_t)n + 15) & ~15u;
    if (c->heap_top > HEAP_SIZE) { printf("the fake heap is full\n"); exit(3); }
    memset(p, 0xcd, (size_t)n);
    logn(L_MEMALLOC, (uint32_t)n, P(p));
    return p;
}
// a created object: the logging vtable, a sort key; its data appended to the creation stream
static void* g_objvt[16];
static void append_stream(const void* d, int32_t size, uint32_t tag) {
    FakeStream* s = stream();
    if (size < 0 || size > 0x400) size = 0x400;
    if (s->len + 8 + size > STREAM_CAP) { printf("the fake stream is full\n"); exit(3); }
    memcpy(s->data + s->len, &tag, 4);
    memcpy(s->data + s->len + 4, &s->tail, 4);
    memcpy(s->data + s->len + 8, d, (size_t)size);
    s->len += 8 + size;
    s->tail += 0x40;
}
static void make_obj(void* self) {
    uint8_t* o = (uint8_t*)self;
    *(void***)o = g_objvt;
    uint32_t m = mark(0x0b);
    *(int32_t*)(o + 8) = (int32_t)(m % 7) - 3;                  // the sort key (many ties)
    *(uint32_t*)(o + 12) = m;
}
static void* __fastcall st_ctor_car(void* self, Edx, PhobData* d, const void* e) {
    logn(L_CTOR_CAR, P(self), P(d), hash_bytes(d, 0x1e4), P(e));
    append_stream(d, 0x1e4, 'CAR ');
    make_obj(self);
    return self;
}
static void* __fastcall st_ctor_ghost(void* self, Edx, PhobData* d, const void* e) {
    logn(L_CTOR_GHOST, P(self), P(d), hash_bytes(d, 0x1e4), P(e));
    append_stream(d, 0x1e4, 'GHST');
    make_obj(self);
    return self;
}
static void* __fastcall st_ctor_model(void* self, Edx, PhobData* d, const char* model) {
    int32_t n = d->size < 0 || d->size > 0x200 ? 0x200 : d->size;
    logn(L_CTOR_MODEL, P(self), d->type, hash_bytes(d, (size_t)n), hash_str(model));
    append_stream(d, n, 'MODL');
    make_obj(self);
    return self;
}
static void* __fastcall st_ctor_wobble(void* self, Edx, PhobData* d) {
    logn(L_CTOR_WOBBLE, P(self), P(d), hash_bytes(d, 0x20));
    append_stream(d, 0x20, 'WOBL');
    make_obj(self);
    return self;
}
static void __cdecl st_ph_create(PhobData* d, int* msg) {
    int32_t n = d->size < 0 || d->size > 0x200 ? 0x200 : d->size;
    logn(L_PH_CREATE, P(d), d->type, hash_bytes(d, (size_t)n), P(msg));
    *msg = stream()->tail;
    append_stream(d, n, 'PHYS');
}
// the objects' virtuals
static void* __fastcall st_v_dtor(void* self, Edx, unsigned flags) { logn(L_V_DTOR, P(self), flags); return self; }
static void __fastcall st_v_tex(void* self, Edx) { logn(L_V_TEX, P(self)); *(uint32_t*)((uint8_t*)self + 12) = mark(0x7e); }
static int __fastcall st_v_key(void* self, Edx) { logn(L_V_KEY, P(self)); return *(int32_t*)((uint8_t*)self + 8); }
static int __fastcall st_v_bad(void* self, Edx) { logn(L_V_BAD, P(self)); return 0; }
// the centre line: fake segments
static void* __fastcall st_cl_ctor(uint8_t* self, Edx, const uint8_t* arg) {
    logn(L_CL_CTOR, P(self), P(arg));
    for (int k = 0; k < 0x70; k += 4) *(uint32_t*)(self + k) = mark(0x44);
    *(uint32_t*)(self + 0x5c) = ctrl()->line_len;
    *(uint32_t*)(self + 0x60) = 0;
    return self;
}
static void __fastcall st_cl_dtor(uint8_t* self, Edx) { logn(L_CL_DTOR, P(self), *(uint32_t*)(self + 0x5c)); }
static ILinePos* __fastcall st_cl_convert(uint8_t* self, Edx, ILinePos* ret, float m) {
    uint32_t mb = ubits(m);
    logn(L_CL_CONVERT, P(self), P(ret), mb);
    uint32_t h = hash32(mb ^ 0x51);
    if ((script() % 100) < ctrl()->line_fail) { ret->seg = 0; ret->t = 0; return ret; }
    ret->seg = seg_at((int)(h % NSEGS));
    ret->t = (h & 0x100) ? ubits((float)((h >> 9) & 0xffff) / 65536.0f) : (h & 0x200 ? 0 : 0x3f800000);
    return ret;
}
static void __fastcall st_quicktan(const uint8_t* seg, Edx, Point2D* out, float t) {
    logn(L_QUICKTAN, P(seg), P(out), ubits(t));
    uint32_t h = hash32((uint32_t)(uintptr_t)seg ^ ubits(t));
    if (h % 50 == 0) { out->x = 0; out->z = 0; return; }           // a zero tangent: 0/0
    out->x = (float)(int32_t)(h & 0xfff) / 64.0f - 32.0f;
    out->z = (float)(int32_t)((h >> 12) & 0xfff) / 64.0f - 32.0f;
}
// models
static int __cdecl st_mr_load(const char* n) { logn(L_MR_LOAD, hash_str(n)); return (int)(hash_str(n) & 0xff) + 1; }
static void __cdecl st_mr_extents(int h, float* a, float* b, float* c, float* d, float* e, float* f) {
    logn(L_MR_EXTENTS, (uint32_t)h, P(a), P(b), P(c), P(f));
    uint32_t s = hash32((uint32_t)h * 77u);
    float* o[6] = {a, b, c, d, e, f};
    for (int i = 0; i < 6; i++) {
        s = hash32(s + i);
        float v = (float)(int32_t)(s & 0x3ff) / 64.0f;
        *o[i] = (i & 1) ? v : -v;
    }
    if (h == 0x42) *a = fbits(0x7fc00000);
}
static void __cdecl st_mr_unload(int h) { logn(L_MR_UNLOAD, (uint32_t)h); }
// event handlers
template <int K> static int __cdecl st_evh(const uint8_t* data) {
    logn(L_EVH, K, P(data), data[0]);
    int r = ctrl()->evh_ret[K];
    return r == -2 ? (int)(data[0] & 7) : r;
}
static void* const k_evh[8] = {(void*)&st_evh<0>, (void*)&st_evh<1>, (void*)&st_evh<2>, (void*)&st_evh<3>,
                               (void*)&st_evh<4>, (void*)&st_evh<5>, (void*)&st_evh<6>, (void*)&st_evh<7>};

// the game's CRT must never put up a message box: its fatal-error paths end the child quietly
static void __cdecl st_crt_fatal(int code) {
    printf("the game's CRT hit a fatal error (%d) in world %d, pass %d: stopping\n", code, g_wno_for_crt, g_pass);
    ExitProcess(4);
}
static int __cdecl st_crt_msgbox(const char* text, const char* caption, unsigned type) {
    printf("the game's CRT tried a message box: %s\n", text ? text : "?");
    ExitProcess(4);
    return 0;
}

static void install_stubs() {
    struct { uint32_t at; void* to; } p[] = {
        {0x004068b0, (void*)&st_trackno}, {0x004713e0, (void*)&st_optget}, {0x00414f30, (void*)&st_s_begin},
        {0x00415000, (void*)&st_s_enter}, {0x00415070, (void*)&st_s_leave}, {0x00415090, (void*)&st_s_end},
        {0x00465ff0, (void*)&st_view_pre}, {0x004660b0, (void*)&st_view_post}, {0x004660d0, (void*)&st_view_begin},
        {0x004664e0, (void*)&st_view_end}, {0x00469500, (void*)&st_trackload}, {0x00469580, (void*)&st_track_unload},
        {0x00467460, (void*)&st_fx_begin}, {0x00467470, (void*)&st_fx_end}, {0x0042a050, (void*)&st_rec_begin},
        {0x0042a210, (void*)&st_rec_end}, {0x0042b910, (void*)&st_ph_begin}, {0x0042ba30, (void*)&st_ph_start},
        {0x0042bae0, (void*)&st_ph_stop}, {0x0042b9c0, (void*)&st_ph_end}, {0x0041c600, (void*)&st_ai_begin},
        {0x0041c650, (void*)&st_ai_end}, {0x0041c590, (void*)&st_ai_tweak}, {0x004570f0, (void*)&st_mr_flush},
        {0x0042beb0, (void*)&st_cam_setfocus}, {0x0042bee0, (void*)&st_cam_maxfocus}, {0x004112b0, (void*)&st_panic},
        {0x00411150, (void*)&st_report}, {0x0040dce0, (void*)&st_gh_begin}, {0x0040e810, (void*)&st_gh_end},
        {0x0040e510, (void*)&st_gh_save}, {0x0040e8e0, (void*)&st_gh_target}, {0x0042bc80, (void*)&st_ph_time},
        {0x0042bd70, (void*)&st_pkt_get}, {0x0042bdd0, (void*)&st_pkt_rel}, {0x00469a10, (void*)&st_process_pkt},
        {0x00457d80, (void*)&st_mr_enabled}, {0x004da350, (void*)&st_stricmp}, {0x004ce0c0, (void*)&st_strchr},
        {0x004ce190, (void*)&st_sscanf}, {0x004cf0a0, (void*)&st_sprintf}, {0x0041b210, (void*)&st_st_get},
        {0x0041b270, (void*)&st_st_forget}, {0x0041b290, (void*)&st_st_rows}, {0x0041b2a0, (void*)&st_st_entry},
        {0x00465100, (void*)&st_carfile}, {0x00465ae0, (void*)&st_terrain}, {0x004140e0, (void*)&st_memalloc},
        {0x00469e00, (void*)&st_ctor_car}, {0x0046cd20, (void*)&st_ctor_ghost}, {0x00469a70, (void*)&st_ctor_model},
        {0x00469ae0, (void*)&st_ctor_wobble}, {0x00422400, (void*)&st_cl_ctor}, {0x00422490, (void*)&st_cl_dtor},
        {0x00422710, (void*)&st_cl_convert}, {0x00426150, (void*)&st_quicktan}, {0x0040d560, (void*)&st_nowalls},
        {0x004558c0, (void*)&st_mr_load}, {0x00456ec0, (void*)&st_mr_extents}, {0x00455950, (void*)&st_mr_unload},
        {0x0042bc10, (void*)&st_ph_create}, {0x004a2b30, (void*)&st_multi_human}, {0x00462750, (void*)&st_assert_w},
        {0x00464470, (void*)&st_assert_c},
    };
    for (auto& x : p) patch_jmp(x.at, x.to);
    // the CRT's fatal paths (amsg_exit, NMSG_WRITE, FF_MSGBANNER, __crtMessageBoxA, _fptrap)
    patch_jmp(0x004cf730, (void*)&st_crt_fatal);
    patch_jmp(0x004d45b0, (void*)&st_crt_fatal);
    patch_jmp(0x004d4570, (void*)&st_crt_fatal);
    patch_jmp(0x004d5c10, (void*)&st_crt_fatal);
    patch_jmp(0x004d6850, (void*)&st_crt_msgbox);
    // floating point in the game's CRT, as its start-up leaves it (_fpmath -> _cfltcvt_init: the %f converters)
    ((void(__cdecl*)())0x004ce150)();
    for (int i = 0; i < 16; i++) g_objvt[i] = (void*)&st_v_bad;
    g_objvt[0] = (void*)&st_v_dtor;
    g_objvt[1] = (void*)&st_v_tex;
    g_objvt[7] = (void*)&st_v_key;
}

// ---- running one call both ways ----------------------------------------------------------------------------------------
template <typename F, typename... A> static uint64_t invoke(F f, A... a) {
    typedef decltype(f(a...)) R;
    if constexpr (std::is_void_v<R>) {
        f(a...);
        return 0;
    } else {
        R r = f(a...);
        uint64_t u = 0;
        memcpy(&u, &r, sizeof r);
        return u;
    }
}

struct Stat { std::string name; long calls, changed, fails, fp_fails, faults, panics, replay_only; };
static std::vector<Stat> g_stats;
static Stat& stat(const std::string& name) {
    for (Stat& s : g_stats)
        if (s.name == name) return s;
    g_stats.push_back({name, 0, 0, 0, 0, 0, 0, 0});
    return g_stats.back();
}

struct Snapshot { uint8_t arena[ARENA_SIZE]; uint8_t globals[GLOBALS_BYTES]; };
static Snapshot g_start, g_after, g_saved;
static Footprint g_fp;
static int g_wno;
static const char* g_phase = "";
static bool g_chain;                                     // the rewrite's pass runs with the whole group's rewrites hooked
static long g_mismatch_total;
static int g_verbose_faults = 1;

static void snap_globals(uint8_t* p) {
    for (const GRange& g : g_globals) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
}
static void save(Snapshot& s) {
    memcpy(s.arena, g_arena, ARENA_SIZE);
    snap_globals(s.globals);
}
static void load(const Snapshot& s) {
    memcpy(g_arena, s.arena, ARENA_SIZE);
    const uint8_t* p = s.globals;
    for (const GRange& g : g_globals) { memcpy((void*)(uintptr_t)g.at, p, g.n); p += g.n; }
}
static bool in_footprint(const void* p) {
    for (int k = 0; k < g_fp.n; k++)
        if ((const uint8_t*)p >= (const uint8_t*)g_fp.r[k].p && (const uint8_t*)p < (const uint8_t*)g_fp.r[k].p + g_fp.r[k].n) return true;
    return false;
}
static const char* region_of(uint32_t off, uint32_t* rel) {
    struct { uint32_t at; const char* name; } r[] = {
        {OFF_CTRL, "ctrl"}, {OFF_WORLD, "World"}, {OFF_MISC, "misc"}, {OFF_PKTSRC, "packet source"}, {OFF_SEGS, "segments"},
        {OFF_OBJS, "objects"}, {OFF_STREAM, "creation stream"}, {OFF_HEAP, "heap"},
    };
    int k = 0;
    for (int i = 0; i < (int)(sizeof r / sizeof *r); i++)
        if (off >= r[i].at) k = i;
    *rel = off - r[k].at;
    return r[k].name;
}

// every pass starts with the stack below it holding one pattern
static __declspec(noinline) void stack_fill(uint32_t pat) {
    volatile uint32_t buf[0x4000];
    for (int i = 0; i < 0x4000; i++) buf[i] = pat;
}
static uint64_t g_last_ret;
static int g_last_result;                                // 0 ran, 1 panicked, 2 faulted (the original's pass)
static int g_kinds[128];
static uint32_t g_fault_code, g_fault_at;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
template <typename Run> static int guarded(Run& run, uint64_t* ret) {
    __try {
        *ret = run();
        return 0;
    } __except (fault_filter(GetExceptionInformation())) {
        return g_fault_code == PANIC_CODE ? 1 : 2;
    }
}
template <typename Run, typename Fp> static void check(const char* fname, Run run, Fp footprint) {
    std::string name = fname;
    if (g_chain) name += " [chain]";
    Stat& st = stat(name);
    st.calls++;
    for (int i = 0; i < 256; i++) g_script[i] = rnd();
    const uint32_t pat = chance(50) ? rnd() : chance(50) ? 0x3f800000u : 0;
    const unsigned pc = chance(50) ? _PC_53 : _PC_24;
    save(g_start);
    g_fp.n = 0; g_fp.replay_only = 0; g_fp.pure = false;
    footprint(g_fp);
    if (g_fp.replay_only) st.replay_only++;
    unsigned cw;
    uint64_t ro = 0, rn = 0;
    // the original
    __asm fninit
    _clearfp();
    _controlfp_s(&cw, pc, _MCW_PC);
    g_pass = 0; g_nlog[0] = 0; g_si = 0;
    auto r0 = [&]() { return run(true); };
    auto r1 = [&]() { return run(false); };
    stack_fill(pat);
    int fo = guarded(r0, &ro);
    __asm fninit
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    save(g_after);
    if (memcmp(g_after.arena + sizeof(Ctrl), g_start.arena + sizeof(Ctrl), ARENA_SIZE - sizeof(Ctrl)) ||
        memcmp(g_after.globals, g_start.globals, GLOBALS_BYTES)) st.changed++;
    g_last_ret = ro;
    g_last_result = fo;
    memset(g_kinds, 0, sizeof g_kinds);
    for (int i = 0; i < g_nlog[0] && i < LOGN; i++) g_kinds[g_log[0][i].kind]++;
    // the rewrite
    load(g_start);
    g_pass = 1; g_nlog[1] = 0; g_si = 0;
    _clearfp();
    _controlfp_s(&cw, pc, _MCW_PC);
    if (g_chain) chain_patch();
    stack_fill(pat);
    int fn = guarded(r1, &rn);
    if (g_chain) chain_unpatch();
    __asm fninit
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    if (fo == 1) st.panics++;
    if (fo == 2 || fn == 2) {
        st.faults++;
        if (g_verbose_faults && st.faults <= 2) printf("fault in %s (world %d): code %08x at %08x\n", name.c_str(), g_wno, g_fault_code, g_fault_at);
        if (fo != fn) {
            g_mismatch_total++;
            if (st.fails++ < 4) printf("MISMATCH %s (world %d, %s): original %s, rewrite %s\n", name.c_str(), g_wno, g_phase,
                                       fo == 2 ? "faulted" : fo ? "panicked" : "ran", fn == 2 ? "faulted" : fn ? "panicked" : "ran");
        }
        load(g_after);
        return;
    }
    uint8_t now_globals[GLOBALS_BYTES];
    snap_globals(now_globals);
    int nlog0 = g_nlog[0] < LOGN ? g_nlog[0] : LOGN;
    bool bad = fo != fn || (fo == 0 && ro != rn) || memcmp(g_after.arena, g_arena, ARENA_SIZE) ||
               memcmp(g_after.globals, now_globals, GLOBALS_BYTES) || g_nlog[0] != g_nlog[1] ||
               memcmp(g_log[0], g_log[1], sizeof(LogEntry) * nlog0);
    if (bad) {
        g_mismatch_total++;
        if (st.fails++ < 4) {
            printf("MISMATCH %s (world %d, %s)%s\n", name.c_str(), g_wno, g_phase, fo != fn ? " (panicked in one pass only)" : "");
            if (!strncmp(fname, "parse_world", 11) || !strncmp(fname, "get_start_line", 14))
                for (auto& r : g_rows)
                    if (strstr(r.c_str(), "checkpoint")) { printf("  first checkpoint row: %s (reverse %d)\n", r.c_str(), world()->options.reverse); break; }
            if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
            int shown = 0;
            for (uint32_t i = 0; i < ARENA_SIZE && shown < 10; i += 4)
                if (memcmp(g_after.arena + i, g_arena + i, 4)) {
                    uint32_t a, b, s, rel;
                    memcpy(&a, g_after.arena + i, 4); memcpy(&b, g_arena + i, 4); memcpy(&s, g_start.arena + i, 4);
                    const char* r = region_of(i, &rel);
                    printf("  %s+0x%x: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", r, rel, s, a, fbits(a), b, fbits(b));
                    shown++;
                }
            const uint8_t* pa = g_after.globals;
            const uint8_t* pb = now_globals;
            for (const GRange& g : g_globals) {
                int k = 0;
                for (uint32_t i = 0; i < g.n && k < 4; i++)
                    if (pa[i] != pb[i]) { printf("  global %s (%s) differs at %08x: %02x vs %02x\n", g.what, global_name(g.at + i), g.at + i, pa[i], pb[i]); k++; i |= 3; }
                pa += g.n; pb += g.n;
            }
            int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
            if (nl > LOGN) nl = LOGN;
            for (int i = 0, k = 0; i < nl && k < 8; i++) {
                LogEntry* a = i < g_nlog[0] ? &g_log[0][i] : 0;
                LogEntry* b = i < g_nlog[1] ? &g_log[1][i] : 0;
                if (a && b && !memcmp(a, b, sizeof *a)) continue;
                k++;
                if (a) printf("  call %d original: %s %08x %08x %08x %08x %08x [globals %08x]\n", i, g_log_names[a->kind], a->a[0], a->a[1], a->a[2], a->a[3], a->a[4], a->globals);
                if (b) printf("  call %d rewrite:  %s %08x %08x %08x %08x %08x [globals %08x]\n", i, g_log_names[b->kind], b->a[0], b->a[1], b->a[2], b->a[3], b->a[4], b->globals);
            }
            if (g_nlog[0] != g_nlog[1]) printf("  %d calls vs %d\n", g_nlog[0], g_nlog[1]);
        }
    }
    // the footprint must cover every byte the original changed (unless the call is left to the replay)
    if (!g_fp.replay_only) {
        bool fpbad = false;
        for (uint32_t i = sizeof(Ctrl); i < ARENA_SIZE && !fpbad; i++) {
            if (g_after.arena[i] == g_start.arena[i] || in_footprint(g_arena + i)) continue;
            uint32_t rel;
            const char* r = region_of(i, &rel);
            if (st.fp_fails++ < 3) printf("FOOTPRINT %s (world %d, %s): %s+0x%x changed outside it\n", name.c_str(), g_wno, g_phase, r, rel);
            fpbad = true;
        }
        const uint8_t* pa = g_after.globals;
        const uint8_t* ps = g_start.globals;
        for (const GRange& g : g_globals) {
            for (uint32_t i = 0; i < g.n && !fpbad; i++)
                if (pa[i] != ps[i] && !in_footprint((void*)(uintptr_t)(g.at + i))) {
                    if (st.fp_fails++ < 3) printf("FOOTPRINT %s (world %d, %s): global %s (%08x) changed outside it\n", name.c_str(), g_wno, g_phase, global_name(g.at + i), g.at + i);
                    fpbad = true;
                }
            pa += g.n; ps += g.n;
        }
    }
    load(g_after);                                        // go on from the original's result
}

#define CHECK(FN, ...)                                                                                  \
    do {                                                                                                \
        auto args_ = std::make_tuple(__VA_ARGS__);                                                      \
        check(VP_CAT(name_, FN),                                                                        \
              [&](bool orig) {                                                                          \
                  auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN;                    \
                  return std::apply([&](auto... a) { return invoke(f, a...); }, args_);                 \
              },                                                                                        \
              [&](Footprint& fp) { std::apply([&](auto... a) { VP_CAT(fpof_, FN)(fp, a...); }, args_); });\
    } while (0)
#define CHECK0(FN)                                                                                      \
    check(VP_CAT(name_, FN), [&](bool orig) { return invoke(orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN); }, \
          [&](Footprint& fp) { VP_CAT(fpof_, FN)(fp); })
// the same from one starting state, once alone and once as a chain
#define CHECK_BOTH(FN, ...) do { save(g_saved); g_chain = false; CHECK(FN, __VA_ARGS__); load(g_saved); g_chain = true; CHECK(FN, __VA_ARGS__); g_chain = false; } while (0)
#define CHECK0_BOTH(FN) do { save(g_saved); g_chain = false; CHECK0(FN); load(g_saved); g_chain = true; CHECK0(FN); g_chain = false; } while (0)

// ---- world files ----------------------------------------------------------------------------------------------------------
static std::vector<std::vector<std::string>> g_tables;
static std::vector<std::string> g_table_names;
static void load_tables(const char* dir) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*.txt", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        std::vector<std::string> rows;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            rows.push_back(line);
        }
        fclose(f);
        g_tables.push_back(rows);
        g_table_names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int g_bad_kind_pct = 2;                           // bad shapes / kinds (they panic)
static std::string num(float lo, float hi) {
    char b[64];
    int k = (int)(rnd() % 1000);
    if (k < 8) return "nan";
    if (k < 14) return (rnd() & 1) ? "inf" : "-inf";
    if (k < 18) return "1e39";
    if (k < 24) return "-0";
    if (k < 28) return "1e-45";
    if (k < 32) return "x";
    snprintf(b, sizeof b, "%f", rf(lo, hi));
    return b;
}
static const char* pick(const char* const* v, int n) { return v[rnd() % (uint32_t)n]; }
static std::string gen_line() {
    char b[512];
    static const char* const shapes[] = {"ball", "cube", "prism", "BALL", "Cube", "PRISM"};
    static const char* const models[] = {"magma.mod", "monk0.mod", "boulder.mod", "barrel.mod", "a.mod", "checkpt1.mod"};
    static const char* const wobs[] = {"pole", "flap", "POLE", "Flap"};
    switch (rnd() % 12) {
    case 0: case 1:
        snprintf(b, sizeof b, chance(80) ? "obj car %s,%s" : "obj car %s, %s", num(-1500, 1500).c_str(), num(-1500, 1500).c_str());
        if (chance(3)) snprintf(b, sizeof b, "obj car");
        break;
    case 2: case 3:
        snprintf(b, sizeof b, "obj checkpoint %s %s,%s %s,%s", chance(90) ? "flag" : "g", num(-1000, 1000).c_str(),
                 num(-1000, 1000).c_str(), num(-1000, 1000).c_str(), num(-1000, 1000).c_str());
        if (chance(5)) snprintf(b, sizeof b, "obj checkpoint flag %s,%s %s", num(-1000, 1000).c_str(), num(-1000, 1000).c_str(), num(-1000, 1000).c_str());
        break;
    case 4: case 5: case 6: {
        const char* s = chance(g_bad_kind_pct) ? "sphere" : pick(shapes, 6);
        snprintf(b, sizeof b, "obj obstacle %s %s %s,%s:%s %s", s, pick(models, 6), num(-1000, 1000).c_str(), num(-1000, 1000).c_str(),
                 num(-6.3f, 6.3f).c_str(), num(1, 20000).c_str());
        if (chance(4)) snprintf(b, sizeof b, "obj obstacle %s %s %s,%s", s, pick(models, 6), num(-10, 10).c_str(), num(-10, 10).c_str());
        break;
    }
    case 7: case 8: {
        const char* w = chance(g_bad_kind_pct) ? "flag" : pick(wobs, 4);
        if (chance(90)) snprintf(b, sizeof b, "obj wobble %s %d", w, ri(-5, 3000));
        else snprintf(b, sizeof b, "obj wobble %s", w);
        break;
    }
    case 9: case 10: {
        const char* k = chance(g_bad_kind_pct) ? "ramp" : chance(80) ? "box" : "BOX";
        float s = chance(15) ? 0.0f : chance(10) ? 1e-5f : chance(10) ? 100.0f : 1.0f;
        std::string ax[3];
        for (int i = 0; i < 3; i++) {
            char t[64];
            snprintf(t, sizeof t, "%f", rf(-3.2f, 3.2f) * s);
            ax[i] = chance(3) ? num(-1, 1) : std::string(t);
        }
        snprintf(b, sizeof b, "obj static %s %s,%s,%s %s,%s,%s %s,%s,%s", k, num(-500, 500).c_str(), num(-20, 50).c_str(),
                 num(-500, 500).c_str(), ax[0].c_str(), ax[1].c_str(), ax[2].c_str(), num(0, 30).c_str(), num(0, 30).c_str(), num(0, 30).c_str());
        if (chance(3)) snprintf(b, sizeof b, "obj static box %s,%s,%s", num(0, 1).c_str(), num(0, 1).c_str(), num(0, 1).c_str());
        break;
    }
    default: {
        static const char* const odd[] = {"", "; a comment", "   ", "map test.map", "map", "ai test.vf", "AI", "foo bar",
                                          "obj", "obj thing 1 2", "OBJ CAR 1, 2", "obj CHECKPOINT flag 1,2 3,4",
                                          "obj car 1,2 ; trailing", ";obj car 5,5", "\tobj car 3,4", "Map x.map", "obj Static"};
        snprintf(b, sizeof b, "%s", pick(odd, (int)(sizeof odd / sizeof *odd)));
    }
    }
    b[255] = 0;                                         // (a line is copied into 0x100 bytes)
    return b;
}
static int g_table_kind;                                   // 0 a real one as it is, 1 mutated, 2 made
static std::vector<std::string> make_table() {
    std::vector<std::string> t;
    int src = chance(40) || g_tables.empty() ? -1 : (int)(rnd() % g_tables.size());
    g_table_kind = src < 0 ? 2 : 1;
    if (src >= 0 && chance(50)) { g_table_kind = 0; return g_tables[src]; }   // as it is
    if (src >= 0) t = g_tables[src];
    else {
        t.push_back("out\\track.txt");
        if (chance(90)) t.push_back("map test.map");
        if (chance(90)) t.push_back("ai test.vf");
        int n = chance(10) ? ri(200, 1200) : ri(0, 60);
        for (int i = 0; i < n; i++) t.push_back(gen_line());
        if (chance(85)) t.push_back(chance(10) ? "END" : "end");
        if (chance(10)) t.push_back("obj car 1,1");
    }
    int ops = ri(1, 6);
    for (int k = 0; k < ops && !t.empty(); k++) {
        int i = ri(0, (int)t.size() - 1);
        switch (rnd() % 8) {
        case 0: if (i) t.erase(t.begin() + i); break;
        case 1: t.insert(t.begin() + i, t[i]); break;
        case 2: t.insert(t.begin() + (i ? i : 1 < (int)t.size() ? 1 : 0), gen_line()); break;
        case 3: if (t[i].size() > 4) t[i].resize(ri(1, (int)t[i].size() - 1)); break;
        case 4: for (char& c : t[i]) if (chance(20)) c = (char)toupper(c); break;
        case 5: t[i] += chance(50) ? " ; comment" : " extra"; break;
        case 6: {                                                         // a number replaced
            size_t p = t[i].find_first_of("0123456789");
            if (p != std::string::npos) t[i].replace(p, 1, chance(50) ? "nan" : "1e39");
            break;
        }
        case 7: for (auto& r : t) if (r == "end") { r = "end2"; break; } break;
        }
    }
    for (auto& r : t) if (r.size() > 255) r.resize(255);
    return t;
}

// ---- building a world ---------------------------------------------------------------------------------------------------
static int g_ncars;
static void build_world() {
    World* w = world();
    memset(w, 0, sizeof *w);
    w->version = chance(95) ? 3 : ri(0, 5);
    w->size = chance(95) ? 0xcd4 : ri(0, 0x2000);
    static const char* const tracks[] = {"bemidji", "dundas", "hastings", "heaven", "kenyon", "limbo", "nfield", "uptown"};
    strcpy(w->track, pick(tracks, 8));
    int n = chance(5) ? 0 : chance(10) ? 16 : ri(1, 8);
    g_ncars = n;
    w->cars.count = n;
    static const char* const cars[] = {"viper", "airhawk", "indyjeep", "willys", "ViPeR", "sidewinder", "stingray"};
    for (int i = 0; i < 16; i++) {
        CarListEntry* e = &w->cars.e[i];
        for (int k = 0; k < (int)sizeof *e; k += 4) *(uint32_t*)((uint8_t*)e + k) = rnd();
        e->subtype = i == 0 ? (chance(90) ? 0 : ri(1, 3)) : chance(80) ? 1 : chance(50) ? 2 : chance(80) ? 3 : chance(50) ? 0 : ri(4, 9);
        if (chance(3)) strcpy(e->car, "");
        else if (chance(3)) strcpy(e->car, "abcdefghijklmnopqrstuvwxyz0123456");   // 33 + NUL: fills car[]
        else strcpy(e->car, pick(cars, 7));
        e->wheel_lock = chance(97) ? rf(0.01f, 1.01f) : fbits(rnd());
        e->ball = (uint8_t)(chance(8) ? 1 : 0);
    }
    uint8_t* o = (uint8_t*)&w->options;
    for (int k = 0; k < 0x28; k += 4) *(uint32_t*)(o + k) = rnd();
    w->options.reflection = chance(50) ? 1 : ri(0, 3);
    w->options.show_checkpoints = chance(50) ? 0 : ri(0, 2);
    w->options.reverse = (uint8_t)(chance(40) ? 1 : chance(90) ? 0 : rnd());
}
static void build_segments() {
    for (int i = 0; i < NSEGS; i++) {
        uint8_t* s = seg_at(i);
        *(uint8_t**)s = seg_at((i + 1) % NSEGS);
        float* f = (float*)(s + 4);
        for (int k = 0; k < 10; k++) f[k] = chance(2) ? fbits(rnd()) : rf(-800, 800);
    }
}
static void build_packet() {
    uint8_t* p = g_arena + OFF_PKTSRC;
    for (int k = 0; k < 0x3d8; k += 4) *(uint32_t*)(p + k) = rnd();
    for (int k = 0; k < 12; k++) ((float*)(p + 4))[k] = rf(-2, 2);
    *(int32_t*)(p + 0x34) = ri(0, 11);
    *(int32_t*)(p + 0x38) = ri(-1, 16);
    *(int32_t*)(p + 0x3c) = ri(0, 5);
    // the events: a type byte and data whose first byte the handlers may read as the length
    PhysicsEventStream* ev = (PhysicsEventStream*)misc(M_EVENTS);
    for (int k = 0; k < 0x400; k++) ev->data[k] = (uint8_t)rnd();
    int pos = 0, n = ri(0, 30);
    for (int i = 0; i < n && pos < 0x3f0; i++) {
        ev->data[pos] = (uint8_t)(chance(90) ? ri(0, 5) : rnd());
        int len = ri(0, 7);
        ev->data[pos + 1] = (uint8_t)len;
        pos += 1 + len;
    }
    ev->len = chance(90) ? (uint32_t)pos : (uint32_t)ri(0, 0x400);
}
static void setup_world() {
    memset(g_arena, 0, ARENA_SIZE);
    Ctrl* c = ctrl();
    c->mark = rnd();
    c->alloc_fail = chance(70) ? 0 : chance(70) ? 1 : 8;
    c->load_fail = chance(80) ? 0 : 20;
    c->line_fail = chance(50) ? 0 : chance(50) ? 10 : 100;
    c->line_len = chance(90) ? ubits(rf(200, 5000)) : chance(50) ? ubits(rf(0, 100)) : 0x7fc00000;
    c->terrain_salt = rnd();
    c->terrain_special = chance(90) ? 0 : 5;
    c->cd_clobber = chance(80) ? 0 : 30;
    c->track_ok = (uint8_t)(chance(95) ? 1 : 0);
    c->ai_ok = (uint8_t)(chance(95) ? 1 : 0);
    c->hack_nowalls = (uint8_t)(chance(15) ? 1 : 0);
    c->multi_human = (uint8_t)rnd();
    c->mr_enabled = (uint8_t)(chance(70) ? 1 : 0);
    c->game_scanf = (uint8_t)(chance(70) ? 1 : 0);
    c->track_number = ri(-1, 12);
    c->max_focus = chance(5) ? 0 : ri(1, 16);
    c->ghost_target = (int32_t)rnd();
    c->opt_value = ri(-1, 3);
    c->opt_write = chance(70);
    c->phys_time = ubits(rf(0, 900));
    c->screen_wid_next = chance(70) ? 0 : (uint32_t)ri(100, 4000);
    for (int i = 0; i < 8; i++) c->evh_ret[i] = chance(70) ? -2 : chance(50) ? -1 : ri(0, 9);
    stream()->len = 0;
    stream()->tail = 0;
    build_world();
    build_segments();
    build_packet();
    for (int i = 0; i < NOBJS; i++) {
        uint8_t* o = obj_at(i);
        *(void***)o = g_objvt;
        *(int32_t*)(o + 8) = ri(-3, 3);
        *(uint32_t*)(o + 12) = rnd();
    }
    // the image statics: randomised (so a missed write shows), then what WorldBeginCommon leaves
    for (const GRange& g : g_globals)
        for (uint32_t i = 0; i < g.n; i += 4) {
            uint32_t v = chance(50) ? 0 : rnd() & 0xff;
            memcpy((void*)(uintptr_t)(g.at + i), &v, g.n - i < 4 ? g.n - i : 4);
        }
    memset((void*)0x005512a0, 0, 0x1000);
    memset((void*)0x005522d8, 0, 0x1000);
    *(int32_t*)0x00553368 = 0;                             // ngobs
    *(int32_t*)0x005522d0 = 0;                             // nwobs
    *(int32_t*)0x005522b4 = 0;                             // cars made
    *(int32_t*)0x00554070 = 0;                             // ghosts
    *(int32_t*)0x004f4354 = -1;
    *(uint8_t*)0x004f4368 = (uint8_t)(chance(30) ? 1 : 0);  // parse_car: the grid from the file
    *(int32_t*)0x00522a1c = ri(0, 3);
    *(int32_t*)0x005228f4 = ri(100, 4000);
    *(int32_t*)0x0055331c = chance(50) ? ri(0, 300) : ri(301, 100000);        // frames drawn: the fps report after 300
    *(int32_t*)0x0055406c = chance(5) ? 0 : ri(1, 1000000);                   // and their time (0: a division by zero)
    *(uint8_t**)0x004ff5a8 = chance(50) ? misc(M_UPGRADES) : 0;
    memcpy((void*)0x00553378, world(), sizeof(World));    // the World copy, as WorldBeginCommon makes it
    *(uint8_t*)0x005522bc = (uint8_t)chance(90);
    *(int32_t*)0x00557f60 = 0;
    if (chance(60)) {                                      // handlers already installed
        int n = ri(1, 12);
        for (int i = 0; i < n; i++) {
            *(void**)(0x00557ee0 + 8 * i) = k_evh[ri(0, 7)];
            *(uint8_t*)(0x00557ee4 + 8 * i) = (uint8_t)ri(0, 5);
        }
        *(int32_t*)0x00557f60 = n;
    }
    *(int32_t*)0x004f48d0 = 0;
    g_bad_kind_pct = chance(70) ? 0 : chance(70) ? 2 : 10;
    g_rows = make_table();
}

// ---- coverage -------------------------------------------------------------------------------------------------------------
enum Cov {
    C_CARS, C_GHOSTS, C_LINE_PLACED, C_START_PLACED, C_OBSTACLES, C_WOBBLES, C_STATICS, C_CHECKPOINTS_OBJ, C_CHECKPOINTS_PHYS,
    C_BALLS, C_BAD_RECORDS, C_PANICS, C_REVERSE, C_EVENTS, C_TABLE_REAL, C_TABLE_MUT, C_TABLE_MADE, C_BEGIN_FULL, C_ROWS, C_UNINIT, NCOV
};
static long g_cov[NCOV];
static const char* const k_cov_names[NCOV] = {
    "cars made", "ghost cars", "centre-line placements", "terrain heights", "obstacles", "wobbles",
    "statics", "checkpoint objects", "checkpoint phobs", "balls", "bad records", "panics", "reversed start lines",
    "events handled", "real tables as they are", "mutated real tables", "made tables", "whole begins", "rows", "begins checked alone (a 4-field start checkpoint, reversed)",
};
static void count_parse() {
    g_cov[C_CARS] += g_kinds[L_CTOR_CAR] + g_kinds[L_CTOR_GHOST];
    g_cov[C_GHOSTS] += g_kinds[L_CTOR_GHOST];
    g_cov[C_LINE_PLACED] += g_kinds[L_QUICKTAN];
    g_cov[C_START_PLACED] += g_kinds[L_TERRAIN];
    g_cov[C_OBSTACLES] += g_kinds[L_MR_EXTENTS];
    g_cov[C_WOBBLES] += g_kinds[L_CTOR_WOBBLE];
    for (int i = 0; i < g_nlog[0] && i < LOGN; i++) {
        const LogEntry& e = g_log[0][i];
        if (e.kind == L_PH_CREATE && e.a[1] == 0x53544154u) g_cov[C_STATICS]++;
        if (e.kind == L_PH_CREATE && e.a[1] == 0x43484b50u) g_cov[C_CHECKPOINTS_PHYS]++;
        if (e.kind == L_CTOR_MODEL && e.a[1] == 0x43484b50u) g_cov[C_CHECKPOINTS_OBJ]++;
        if (e.kind == L_CTOR_MODEL && e.a[1] == 0x42414c4cu) g_cov[C_BALLS]++;
        if (e.kind == L_REPORT) g_cov[C_BAD_RECORDS]++;
    }
}

// ---- the phases -----------------------------------------------------------------------------------------------------------
static void per_line_checks(int max) {
    World* w = world();
    int n = (int)g_rows.size();
    for (int k = 0; k < max && n > 1; k++) {
        const char* line = g_rows[ri(1, n - 1)].c_str();
        g_phase = "line";
        if (chance(10)) *(int32_t*)0x005522b4 = ri(0, g_ncars);
        char kind[0x100] = "";
        sscanf(line, "obj %255s", kind);
        if (!_stricmp(kind, "car")) CHECK(parse_car_rw, line, (const CarList*)&w->cars);
        else if (!_stricmp(kind, "obstacle")) CHECK(parse_obstacle_rw, line);
        else if (!_stricmp(kind, "wobble")) CHECK(parse_wobble_rw, line);
        else if (!_stricmp(kind, "static")) CHECK(parse_static_rw, line);
        else if (!_stricmp(kind, "checkpoint")) CHECK(parse_checkpoint_rw, line);
        else continue;
        if (g_last_result == 1) g_cov[C_PANICS]++;
        count_parse();
        if (chance(30)) {
            if (chance(50)) CHECK(parse_object_rw, line, (const CarList*)&w->cars);
            else CHECK_BOTH(parse_object_rw, line, (const CarList*)&w->cars);
        }
    }
}

static void small_checks(int steps) {
    World* w = world();
    for (int s = 0; s < steps; s++) {
        g_phase = "small";
        switch (rnd() % 40) {
        case 0: CHECK0(WorldShowShadow_rw); CHECK0(WorldShowSkids_rw); CHECK0(WorldShowSmoke_rw); break;
        case 1: *(int32_t*)0x00554024 = ri(0, 2); *(int32_t*)(0x00554024 + 8) = ri(0, 2); CHECK0(WorldShowReflection_rw); break;
        case 2: *(int32_t*)0x00522a1c = ri(0, 3); *(int32_t*)0x00553320 = ri(-1, 3); CHECK0(WorldShowSpecular_rw); break;
        case 3: CHECK0(WorldGetElapsedTime_rw); CHECK0(WorldGetRaceState_rw); CHECK0(WorldGetFocusCar_rw); CHECK0(WorldGetCameraType_rw); CHECK0(WorldGetPlayerCar_rw); break;
        case 4: {
            static const char* const cars[] = {"viper", "VIPER", "airhawk", "", "willys"};
            int paint = chance(20) ? (int)rnd() : ri(-5, 5);
            if (chance(3)) paint = chance(50) ? INT32_MIN : INT32_MAX;
            CHECK(WorldGetCarTexture_rw, (char*)misc(M_TEXNAME), pick(cars, 5), paint);
            break;
        }
        case 5: { uint32_t* p = (uint32_t*)misc(M_POINT); p[0] = rnd(); p[1] = rnd(); p[2] = rnd(); CHECK(WorldMoveBlimp_rw, (const uint32_t*)p); break; }
        case 6: ctrl()->max_focus = chance(5) ? 0 : ri(1, 16); *(int32_t*)0x004f4358 = chance(5) ? (int)rnd() : ri(-1, 16);
                CHECK0(WorldNextFocusCar_rw); CHECK0(WorldPrevFocusCar_rw); CHECK(WorldSetFocusCar_rw, ri(-2, 18)); break;
        case 7: CHECK(WorldGetCarEntry_rw, ri(-2, 17)); CHECK0(WorldGetTrackNumber_rw); CHECK0(WorldGetTrackname_rw); CHECK0(WorldGameOptions_rw); break;
        case 8: if (*(int32_t*)0x00553368 < 0x3f0) CHECK(WorldAddGob_rw, (void*)obj_at(ri(0, NOBJS - 1))); break;
        case 9: if (*(int32_t*)0x00553368 < 0x3f0) CHECK(WorldAddWob_rw, chance(90) ? (void*)obj_at(ri(0, NOBJS - 1)) : (void*)0); break;
        case 10: CHECK0(WorldFXEnable_rw); CHECK0(WorldFXEnabled_rw); CHECK0(WorldFXDisable_rw); CHECK0(WorldFXEnabled_rw); break;
        case 11: CHECK(World_is_valid_version_rw, (const World*)w, 0);
                 CHECK(World_ctor_rw, (World*)(g_arena + OFF_HEAP + HEAP_SIZE - 0x1000), 0); break;
        case 12: CHECK(setup_cars_rw, w); if (g_last_result == 1) g_cov[C_PANICS]++; break;
        case 13: {
            Frame* f = (Frame*)misc(M_FRAME);
            CHECK(get_start_line_rw, f, (StringTable*)g_table_handle, (uint8_t)(chance(50) ? 1 : 0));
            if (g_kinds[L_PANIC]) g_cov[C_PANICS]++;
            break;
        }
        case 14: {
            Frame* f = (Frame*)misc(M_FRAME);
            CHECK(set_car_frame_rw, f, chance(10) ? (int)rnd() : ri(-3, 20));
            break;
        }
        case 15: {
            int a = ri(0, NSEGS - 1), b = ri(0, NSEGS - 1);
            float t = chance(10) ? fbits(rnd()) : rf(-0.5f, 1.5f);
            CHECK(HermiteEval_rw, (Point2D*)misc(M_POINT), (const Point2D*)(seg_at(a) + 4), (const Point2D*)(seg_at(b) + 4),
                  (const Point2D*)(seg_at(a) + 0xc), (const Point2D*)(seg_at(b) + 0xc), t);
            break;
        }
        case 16:
            if (g_vrmod) {
                float m = (float)ri(300, 60000), r = (float)ri(4, 180);
                uint32_t mb, rb;
                memcpy(&mb, &m, 4);
                memcpy(&rb, &r, 4);
                vrmod_put32(0x004636f2, chance(5) ? rnd() : mb);
                vrmod_put32(0x00463700, chance(5) ? rnd() : rb);
            }
            CHECK(create_ball_rw, ri(-1, 16));
            break;
        case 17: if (*(int32_t*)0x00553368 > 0) CHECK0_BOTH(sort_objects_rw); break;
        case 18: {
            int n = *(int32_t*)0x00553368;
            if (n > 1) {
                void** g = (void**)0x005522d8;
                CHECK(obj_sort_f_rw, (const void*)&g[ri(0, n - 1)], (const void*)&g[ri(0, n - 1)]);
            }
            break;
        }
        case 19: *(int32_t*)0x005522b0 = ri(-1, 2); *(int32_t*)0x0055335c = ri(-1, 4); CHECK0(should_draw_mirror_rw); break;
        case 20: CHECK(CarInfo_ctor_rw, (void*)misc(M_CARINFO), 0); CHECK(Point2D_ctor_rw, (void*)misc(M_POINT), 0); CHECK(CarMgrInfo_ctor_rw, (void*)0x00554090, 0); break;
        case 21: {
            float a = chance(10) ? fbits(rnd()) : rf(-7, 7), b = chance(50) ? 0.0f : rf(-7, 7), c = chance(50) ? 0.0f : rf(-7, 7);
            CHECK(MatrixMakeWorldRotation_rw, (M3*)misc(M_MATRIX), a, b, c);
            break;
        }
        // carmgr
        case 22: case 23: {
            static const char* const names[] = {"viper", "airhawk", "", "a-very-long-car-name", "x"};
            int car = chance(95) ? ri(0, 15) : ri(-1, 16);
            if (car < 0 || car > 15) break;
            CHECK(CarMgrRegisterCar_rw, car, (const void*)obj_at(ri(0, NOBJS - 1)), pick(names, 5), pick(names, 5), ri(0, 3));
            break;
        }
        case 24: CHECK0(CarMgrCount_rw); CHECK(CarMgrGetInfo_rw, ri(-2, 17)); CHECK(CarMgrGetStatusBuf_rw, ri(-2, 17)); break;
        case 25: {
            int car = ri(-1, 16);
            uint8_t* ci = misc(M_CARINFO);
            for (int k = 0; k < 0x38; k += 4) *(uint32_t*)(ci + k) = rnd();
            CHECK(CarMgrUpdateCar_rw, car, (const void*)ci);
            break;
        }
        // events
        case 26: case 27:
            if (*(int32_t*)0x00557f60 < 16) CHECK(EventInstallHandler_rw, k_evh[ri(0, 7)], (uint8_t)(chance(90) ? ri(0, 5) : rnd()));
            break;
        case 28: CHECK(EventUnInstallHandler_rw, k_evh[ri(0, 7)]); break;
        case 29: case 30: {
            build_packet();
            CHECK(EventDispatch_rw, (const PhysicsEventStream*)misc(M_EVENTS));
            g_cov[C_EVENTS] += g_kinds[L_EVH];
            break;
        }
        case 31: CHECK0(EventBegin_rw); break;
        case 32: case 33: build_packet(); if (chance(50)) CHECK0(WorldUpdate_rw); else CHECK0_BOTH(WorldUpdate_rw); g_cov[C_EVENTS] += g_kinds[L_EVH]; break;
        case 34: CHECK0(WorldInitTextures_rw); break;
        case 35: CHECK0(WorldSwitchToDrive_rw); CHECK0(WorldSwitchToUI_rw); break;
        case 36: CHECK(create_remaining_cars_rw, (const CarList*)&w->cars); break;
        case 37: CHECK0(CarMgrEnd_rw); CHECK0(CarMgrBegin_rw); break;
        default: break;
        }
    }
}

// get_start_line reads the first "obj checkpoint" row with at least 4 fields; with exactly 4, its second z is an
// uninitialised local (as the original's): reversed, the start line then depends on the stack's contents, which in
// a chain differ between the original's and the rewrite's callees -- such a world's begin is checked alone only
static int scan_quiet(const char* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r;
    if (ctrl()->game_scanf) {
        GameFile f = {(char*)s, (int)strlen(s), (char*)s, 0x49, 0, 0, 0, 0};
        r = ((Input_t)0x004d0ee0)(&f, fmt, ap);
    } else r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
static bool start_line_uninit() {
    if (!world()->options.reverse) return false;
    for (size_t i = 1; i < g_rows.size(); i++) {
        char name[256];
        float a, b, c, d;
        int n = scan_quiet(g_rows[i].c_str(), (const char*)0x004f4624, name, &a, &b, &c, &d);
        if (n >= 4) return n == 4;
    }
    return false;
}

static void run_world() {
    setup_world();
    World* w = world();
    g_cov[g_table_kind == 0 ? C_TABLE_REAL : g_table_kind == 1 ? C_TABLE_MUT : C_TABLE_MADE]++;
    g_cov[C_ROWS] += (long)g_rows.size();
    g_cov[C_REVERSE] += w->options.reverse != 0;
    // the begin: whole, or the parser from WorldBeginCommon's state
    g_phase = "begin";
    int how = ri(0, 9);
    const bool alone = start_line_uninit();
    g_cov[C_UNINIT] += alone;
    if (how < 3) {
        g_cov[C_BEGIN_FULL]++;
        if (alone) {
            if (how == 0) CHECK(WorldBeginCommon_rw, w);
            else if (how == 1) CHECK(WorldBegin_rw, w);
            else CHECK(WorldBeginReplay_rw, w);
        } else {
            if (how == 0) CHECK_BOTH(WorldBeginCommon_rw, w);
            else if (how == 1) CHECK_BOTH(WorldBegin_rw, w);
            else CHECK_BOTH(WorldBeginReplay_rw, w);
        }
    } else {
        if (chance(30)) { CHECK(setup_cars_rw, w); if (g_last_result == 1) g_cov[C_PANICS]++; }
        if (alone) CHECK(parse_world_rw, (const World*)w);
        else CHECK_BOTH(parse_world_rw, (const World*)w);
    }
    count_parse();
    if (g_last_result == 1) g_cov[C_PANICS]++;
    if (chance(30)) {                                          // WorldDraw's counters, as a race leaves them
        *(int32_t*)0x0055331c = chance(50) ? ri(0, 300) : ri(301, 100000);
        *(int32_t*)0x0055406c = chance(5) ? 0 : ri(1, 1000000);
    }
    per_line_checks(chance(20) ? 60 : 12);
    small_checks(ri(20, 60));
    // the end
    g_phase = "end";
    if (chance(60)) {
        *(int32_t*)0x0055331c = chance(30) ? ri(0, 300) : ri(301, 100000);
        *(int32_t*)0x0055406c = chance(5) ? 0 : ri(1, 1000000);
    }
    switch (rnd() % 3) {
    case 0: CHECK0_BOTH(WorldEndCommon_rw); break;
    case 1: CHECK_BOTH(WorldEnd_rw, w); break;
    default: CHECK0_BOTH(WorldEndReplay_rw); break;
    }
}

int main(int argc, char** argv) {
    // never a dialog on the desktop: no Windows error boxes, no CRT message boxes (inherited by the child)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 300;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char dir[MAX_PATH];
    if (argc > 3) snprintf(dir, sizeof dir, "%s", argv[3]);
    else { GetEnvironmentVariableA("TEMP", dir, MAX_PATH); strcat(dir, "\\w2\\obt"); }
    load_tables(dir);
    printf("%d world files from %s\n", (int)g_tables.size(), dir);
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_wld_world.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    g_vrmod = getenv("VP_VRMOD") && atoi(getenv("VP_VRMOD"));
    if (g_vrmod) puts("vrmod's hornball: create_ball's mass and radius random");
    install_stubs();
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) return 2;
    uint32_t gb = 0;
    for (const GRange& g : g_globals) gb += g.n;
    if (gb != GLOBALS_BYTES) { printf("GLOBALS_BYTES is %u, should be %u\n", (unsigned)GLOBALS_BYTES, gb); return 2; }
    for (g_wno = 0; g_wno < worlds; g_wno++) { g_wno_for_crt = g_wno; run_world(); }
    int failed = 0;
    long calls = 0;
    printf("%-44s %8s %8s %8s %8s %8s %8s %8s\n", "function", "calls", "changed", "replay", "panics", "faults", "differ", "fp-miss");
    for (Stat& st : g_stats) {
        printf("%-44s %8ld %8ld %8ld %8ld %8ld %8ld %8ld\n", st.name.c_str(), st.calls, st.changed, st.replay_only, st.panics,
               st.faults, st.fails, st.fp_fails);
        if (st.fails || st.fp_fails) failed++;
        calls += st.calls;
    }
    printf("%d worlds, %ld calls; %d of %d checks differ or escape their footprint\n", worlds, calls, failed, (int)g_stats.size());
    printf("covered:");
    for (int i = 0; i < NCOV; i++) printf("%s %s %ld", i ? "," : "", k_cov_names[i], g_cov[i]);
    printf("\n");
    return failed ? 1 : 0;
}
