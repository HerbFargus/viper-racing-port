// world_aidriver.cpp -- the AI driver rewrites (hook/phys_aidriver.cpp) against the originals, outside the game
// (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_aidriver.cpp
//        /Fo%TEMP%\wa\ /Fe%TEMP%\wa\world_aidriver.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_aidriver.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and runs driver.obj's own static initialisers
// ($E62, the 41 RandomAIDriver objects, $E188), so RandomAIDriver::str and the driver objects are the game's.
// Each world starts from that .data and randomises: the random-driver counts, the driver map and its count,
// the AI strength, a lounge (vtable, resource, 8 tables pointing into the resource or at the random drivers,
// counts, offset), the resource (a version-2 AIDriverResource2 with sane counts and named drivers), the config
// dialog's tab / live driver / indices / strings / clipboard, and an arena of scratch objects. The game
// functions around them are patched with logging stubs whose results come from a scripted random stream,
// replayed identically for the original and the rewrite: Random, GetAIStrengthString, GetTrackName,
// GetTrackNumber, LogReport / LogPanic (their varargs by the format), ResourceTry (version, size, fix-up flag
// and the resource's counts scripted), ResourceForget, File*, ResourceWrite (the written bytes hashed, the
// original's stack junk masked), sprintf (the MSVC CRT's, into the same buffer), UIDoDratBox, UIDoDialog (the
// dialog and its 94 items logged), OptionsGet / Set, CreateIGhostCarViewer (a fake viewer), learn_dialog,
// MemAlloc / delete, atexit, PhysicsTimeString, WorldGameOptions / WorldGetTrackNumber. The rest (UIDialogItem,
// memmove, AIGetStrength, the lounge's and the drivers' virtuals, and every function of driver.obj called by
// address) are the originals. For each call the original runs, the world is restored, the rewrite runs, and the
// whole world, the return value and the stub log are compared bit for bit; the bytes the original changed must
// lie inside the rewrite's footprint (replay_only footprints are reported, not counted). Worlds alternate the
// x87 between single precision (the physics thread) and double (the main thread's usual state).
//
// The fixes (port.h: VP_FIX): the plain build defines VP_FAITHFUL and checks the rewrites against the originals as
// above. Built with /DFIX_TESTS it compiles the fixed rewrites and runs the same worlds (sane counts: every one must
// still equal the original), then gives AIDriverLounge::fixup_res and copy_res resources with more than 16 drivers
// in a strength: the originals write past the bucket / the tables (shown), the fixed ones only the 16 there are.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <tuple>
#include <type_traits>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#include <initializer_list>                    // (a range-for over a braced list)
#endif
#ifndef FIX_TESTS
#define VP_FAITHFUL                                 // the original's behaviour, bit for bit (the fixes: /DFIX_TESTS)
#endif
#include "../hook/port.h"

#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

// ---- what the rewrites link against, standing in for the DLL ------------------------------------------
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
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }

#include "../hook/phys_aidriver.cpp"

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_state = 0x5bd1e995u;
static uint32_t rnd() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static int g_special_pct = 3;
static float special() {
    switch (rnd() % 8) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));
    case 4: return fbits(rnd() & 0x807fffffu);
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits(rnd() & 0x80000000u | 0x7f7fffffu);
    default: return fbits(rnd());
    }
}
static float sp(float x) { return (int)(rnd() % 100) < g_special_pct ? special() : x; }

static uint32_t g_script = 1;                              // the stubs' stream: reset to the same seed for both passes
static uint32_t script() {
    g_script ^= g_script << 13;
    g_script ^= g_script >> 17;
    g_script ^= g_script << 5;
    return g_script;
}

// ---- the original, loaded at 0x400000 (as test/fuzz.cpp) ---------------------------------------------------
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
static void patch_jump(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uint32_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the world ---------------------------------------------------------------------------------------------------
enum : uint32_t {
    A_LOUNGE = 0x0000,                                 // the lounge the statics point at
    A_LOUNGE2 = 0x0400,                                // a scratch lounge (constructor, destructor)
    A_SCRATCH = 0x0800, SCRATCH_N = 0x2400,            // scratch objects (skills, drivers, buckets, a RandomAIDriver)
    A_RES = 0x3000, RES_N = 0x11020,                   // the resource ResourceTry hands out
    A_STRINGS = 0x14100, STRING_N = 32,                // 16 fake strings
    A_BUF = 0x14400,                                   // a name buffer
    A_MAPIN = 0x14500,                                 // a driver map to set (32 ints)
    A_OPTIONS = 0x14600,                               // WorldGameOptions
    A_VIEWER = 0x14700,                                // the fake ghost-car viewer
    A_OUT = 0x14800,                                   // an AIDriver2 out
    A_RDRV = 0x14c00,                                  // a RandomAIDriver to construct (registered: left alone after)
    A_HEAP = 0x15000, HEAP_N = 0x1000,                 // MemAlloc
    ARENA_N = 0x16000,
};
static uint8_t* g_arena;
static uint8_t* g_data;                                // race.exe .data
static uint32_t g_data_n;
static uint8_t* g_pristine;                            // .data after the static initialisers
static const char* fake_string(int i) { return (const char*)(g_arena + A_STRINGS + (i & 15) * STRING_N); }
static uint32_t g_heap_next;

static const char* where(const void* p, char* buf) {
    uint32_t a = (uint32_t)(uintptr_t)p;
    if (p >= g_arena && p < g_arena + ARENA_N) {
        uint32_t off = (uint32_t)((const uint8_t*)p - g_arena);
        struct { uint32_t o, n; const char* name; } parts[] = {
            {A_LOUNGE, 0x400, "lounge"}, {A_LOUNGE2, 0x400, "scratch lounge"}, {A_SCRATCH, SCRATCH_N, "scratch"},
            {A_RES, RES_N, "resource"}, {A_STRINGS, 16 * STRING_N, "strings"}, {A_BUF, 0x100, "buf"},
            {A_MAPIN, 0x100, "map in"}, {A_OPTIONS, 0x100, "options"}, {A_VIEWER, 0x100, "viewer"},
            {A_OUT, 0x400, "out"}, {A_RDRV, 0x400, "random driver"}, {A_HEAP, HEAP_N, "heap"}};
        for (auto& q : parts)
            if (off >= q.o && off < q.o + q.n) { sprintf(buf, "%s+0x%x", q.name, off - q.o); return buf; }
        sprintf(buf, "arena+0x%x", off);
        return buf;
    }
    const char* what = a >= S_TAB && a < S_TAB + TAB_SIZE ? "tab" :
                       a >= S_LIVE_BASE && a < S_LIVE_BASE + LIVE_SIZE ? "live" :
                       a >= S_STR && a < S_STR + 0x220 ? "str" :
                       a >= S_MAP && a < S_MAP + 0x44 ? "map" :
                       a >= S_CLIP && a < S_CLIP + 0x220 ? "clip" : ".data";
    sprintf(buf, "%s 0x%08x", what, a);
    return buf;
}

// ---- the stubs' log ------------------------------------------------------------------------------------------------
enum { LOG_MAX = 65536 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void lg(uint32_t w) { if (g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]++] = w; }
static void lg_str(const char* s) {
    if (!s) { lg(0xffffffffu); return; }
    size_t n = strlen(s);
    lg((uint32_t)n);
    for (size_t i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, s + i, n - i < 4 ? n - i : 4); lg(w); }
}
static void lg_varargs(const char* fmt, va_list ap) {         // one dword per conversion in the format
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') { p++; continue; }
        lg(va_arg(ap, uint32_t));
    }
}
static uint32_t fnv(const uint8_t* p, uint32_t n, const uint8_t* skip) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) if (!skip || !skip[i]) h = (h ^ p[i]) * 16777619u;
    return h;
}
// the bytes of a written resource the original leaves as its stack's junk: SkillInfo0's +0x0c, +0x18, +0x3b,
// SkillInfo2's car_name past "viper", and the padding after the name
static uint8_t g_skip1[0xe020], g_skip2[0x11020];
static void make_skips() {
    for (int b = 0; b < 8; b++)
        for (int j = 0; j < 16; j++) {
            uint8_t* d1 = g_skip1 + b * 0x1c04 + j * 0x1c0;
            memset(d1 + 0xc, 1, 4); memset(d1 + 0x18, 1, 4); d1[0x3b] = 1; memset(d1 + 0x1b4, 1, 12);
            uint8_t* d2 = g_skip2 + b * 0x2204 + j * 0x220;
            memset(d2 + 0xc, 1, 4); memset(d2 + 0x18, 1, 4); d2[0x3b] = 1; memset(d2 + 0x76, 1, 0x1a); memset(d2 + 0x214, 1, 12);
        }
}

// __cdecl stubs
static int __cdecl st_Random(int n) { lg('RAND'); lg((uint32_t)n); return n > 0 ? (int)(script() % (uint32_t)n) : 0; }
static const char* __cdecl st_GetAIStrengthString(int i) { lg('GASS'); lg((uint32_t)i); return fake_string(i); }
static const char* __cdecl st_GetTrackName(int i) { lg('GTNM'); lg((uint32_t)i); return fake_string(i + 5); }
static int __cdecl st_GetTrackNumber(const char* s) { lg('GTNU'); lg_str(s); return (int)(script() % 9); }
template <uint32_t TAG> static void __cdecl st_log(const char* fmt, ...) {
    lg(TAG); lg_str(fmt);
    va_list ap;
    va_start(ap, fmt);
    lg_varargs(fmt, ap);
    va_end(ap);
}
static void* __cdecl st_ResourceTry(const char* name, uint32_t type, uint32_t* version, int32_t* size, uint8_t* p5, uint8_t* flag) {
    lg('RTRY'); lg_str(name); lg(type); lg((uint32_t)(uintptr_t)p5);
    uint32_t r = script() % 10;
    if (r == 0) return 0;
    uint32_t v = r < 6 ? 2 : r == 6 ? 0 : r == 7 ? 1 : script() % 5;
    int32_t sz = v == 2 ? (script() % 3 ? 0x11020 : script() % 2 ? 0x660c : (int32_t)(script() % 0x20000)) : (int32_t)(script() % 0x20000);
    *version = v;
    *size = sz;
    *flag = (uint8_t)(script() % 3 == 0);
    uint8_t* res = g_arena + A_RES;                   // sane counts where this version keeps them
    for (int b = 0; b < 8; b++) {
        uint32_t off = v == 0 ? 0x7c0 + b * 0x7c4 : v == 1 ? 0x1c00 + b * 0x1c04 : 0x2200 + b * 0x2204;
        if (v > 2 || off + 4 > RES_N) continue;
        *(int32_t*)(res + off) = (int32_t)(script() % 17);
    }
    return res;
}
static uint8_t __cdecl st_ResourceForget(void* p) { lg('RFGT'); lg((uint32_t)(uintptr_t)p); return 1; }
static void __cdecl st_ResourceWrite(int fd, uint32_t type, uint32_t v) { lg('RWRT'); lg((uint32_t)fd); lg(type); lg(v); }
static int __cdecl st_FileCreate(const char* name) { lg('FCRT'); lg_str(name); return script() % 4 ? 7 : 0; }
static uint8_t __cdecl st_FileWrite(int fd, const void* p, int n) {
    lg('FWRT'); lg((uint32_t)fd); lg((uint32_t)n);
    lg(fnv((const uint8_t*)p, (uint32_t)n, n == 0xe020 ? g_skip1 : n == 0x11020 ? g_skip2 : 0));
    return (uint8_t)(script() & 1);
}
static void __cdecl st_FileClose(int* fd) { lg('FCLS'); lg((uint32_t)*fd); *fd = 0; }
static int __cdecl st_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    lg('SPRF'); lg_str(fmt); lg_str(buf);
    return n;
}
static void __cdecl st_UIDoDratBox(const char* title, const char* text, void* cb) { lg('DRAT'); lg_str(title); lg_str(text); lg((uint32_t)(uintptr_t)cb); }
static int __cdecl st_UIDoDialog(const UIDialog* d, int w, int h, int x, int y, int mode) {
    lg('DLG '); lg((uint32_t)w); lg((uint32_t)h); lg((uint32_t)x); lg((uint32_t)y); lg((uint32_t)mode);
    lg(d->title); lg(d->subtitle); lg((uint32_t)d->_08); lg(d->idle);
    for (int k = 0; k < 94; k++)
        for (int i = 0; i < 14; i++) lg(d->items[k].a[i]);
    return (int)(script() % 3);
}
static void __cdecl st_OptionsGetI(const char* sec, const char* key, int32_t* v) {
    lg('OPTI'); lg_str(sec); lg_str(key); lg((uint32_t)(uintptr_t)v); lg((uint32_t)*v);
    if (script() & 1) *v = (int32_t)(script() % 20) - 2;
}
static void __cdecl st_OptionsSetI(const char* sec, const char* key, int v) { lg('OSET'); lg_str(sec); lg_str(key); lg((uint32_t)v); }
static void __cdecl st_learn_dialog(void) { lg('LRND'); }
static void* __fastcall st_viewer_delete(void* self, int, int flag) { lg('VDEL'); lg((uint32_t)(uintptr_t)self); lg((uint32_t)flag); return self; }
static void* g_viewer_vt[4] = {(void*)st_viewer_delete};
static void* __cdecl st_CreateIGhostCarViewer(int32_t* a, int32_t* b, int32_t* c) {
    lg('VIEW'); lg((uint32_t)(uintptr_t)a); lg((uint32_t)(uintptr_t)b); lg((uint32_t)(uintptr_t)c);
    return script() % 4 ? g_arena + A_VIEWER : 0;
}
static int __cdecl st_atexit(void* fn) { lg('ATEX'); lg((uint32_t)(uintptr_t)fn); return 0; }
static void* __cdecl st_MemAlloc(int n) {
    lg('MALC'); lg((uint32_t)n);
    if (script() % 8 == 0 || g_heap_next + (uint32_t)n > HEAP_N) return 0;
    void* p = g_arena + A_HEAP + g_heap_next;
    g_heap_next += ((uint32_t)n + 15) & ~15u;
    return p;
}
static void __cdecl st_delete(void* p) { lg('DELE'); lg((uint32_t)(uintptr_t)p); }
static const char* __cdecl st_PhysicsTimeString(uint32_t bits, uint32_t flag) { lg('PTST'); lg(bits); lg(flag); return fake_string((int)(bits >> 3)); }
static const uint8_t* __cdecl st_WorldGameOptions(void) { lg('WGOP'); return g_arena + A_OPTIONS; }
static int __cdecl st_WorldGetTrackNumber(void) { lg('WGTN'); return (int)(script() % 8); }

static void install_stubs() {
    patch_jump(0x0041b6e0, (void*)st_Random);
    patch_jump(0x004061a0, (void*)st_GetAIStrengthString);
    patch_jump(0x00406810, (void*)st_GetTrackName);
    patch_jump(0x004068b0, (void*)st_GetTrackNumber);
    patch_jump(0x00411150, (void*)st_log<'LOGR'>);
    patch_jump(0x004112b0, (void*)st_log<'PANC'>);
    patch_jump(0x00419ce0, (void*)st_ResourceTry);
    patch_jump(0x0041a450, (void*)st_ResourceForget);
    patch_jump(0x0041a5c0, (void*)st_ResourceWrite);
    patch_jump(0x004115f0, (void*)st_FileCreate);
    patch_jump(0x00411a30, (void*)st_FileWrite);
    patch_jump(0x00411850, (void*)st_FileClose);
    patch_jump(0x004cf0a0, (void*)st_sprintf);
    patch_jump(0x00479280, (void*)st_UIDoDratBox);
    patch_jump(0x00478ff0, (void*)st_UIDoDialog);
    patch_jump(0x004713e0, (void*)st_OptionsGetI);
    patch_jump(0x00471430, (void*)st_OptionsSetI);
    patch_jump(0x004238a0, (void*)st_learn_dialog);
    patch_jump(0x00426050, (void*)st_CreateIGhostCarViewer);
    patch_jump(0x004ceff0, (void*)st_atexit);
    patch_jump(0x004140e0, (void*)st_MemAlloc);
    patch_jump(0x00414390, (void*)st_delete);
    patch_jump(0x0042bf30, (void*)st_PhysicsTimeString);
    patch_jump(0x004627a0, (void*)st_WorldGameOptions);
    patch_jump(0x00462760, (void*)st_WorldGetTrackNumber);
}

// ---- random worlds ------------------------------------------------------------------------------------------------
static void random_bytes(void* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t*)p)[i] = (uint8_t)rnd(); }
static void random_floats(void* p, uint32_t n, float lo, float hi) {
    for (uint32_t i = 0; i + 4 <= n; i += 4) { float f = sp(rf(lo, hi)); memcpy((uint8_t*)p + i, &f, 4); }
}
static int32_t& str_count(int i) { return *P<int32_t>(S_STR_COUNT0 + 0x44 * i); }
static void random_driver2(uint8_t* d, bool named) {                  // an AIDriver2 with plausible fields
    random_floats(d, 0x220, -3, 3);
    *(int32_t*)(d + 4) = rnd() % 4 ? ri(0, 6) : (int32_t)rnd();
    *(int16_t*)(d + 0xc) = (int16_t)(rnd() % 3 ? ri(0, 200) : (int)rnd());
    *(int16_t*)(d + 0xe) = (int16_t)(rnd() % 3 ? ri(0, 100) : (int)rnd());
    for (int b = 0; b < 4; b++) d[0x38 + b] = (uint8_t)rnd();
    int n = ri(0, 31);
    for (int k = 0; k < 32; k++) d[0x70 + k] = (char)(k < n ? 'a' + rnd() % 26 : 0);
    *(const char**)(d + 0x210) = named ? fake_string(ri(0, 15)) : (const char*)(uintptr_t)rnd();
}

static void build_world() {
    memcpy(g_data, g_pristine, g_data_n);
    memset(g_arena, 0, ARENA_N);
    // the random drivers: now and then fewer of them
    for (int i = 0; i < 8; i++)
        if (rnd() % 6 == 0) str_count(i) = ri(0, str_count(i));
    // the driver map and the strength
    int32_t* map = P<int32_t>(S_MAP);
    for (int i = 0; i < 16; i++) map[i] = i;
    for (int i = 15; i > 0; i--) { int k = ri(0, i); int32_t t = map[i]; map[i] = map[k]; map[k] = t; }
    if (rnd() % 6 == 0) for (int i = 0; i < 16; i++) map[i] = ri(0, 20);
    *P<int32_t>(S_MAP_N) = rnd() % 4 ? 16 : ri(0, 16);
    *P<int32_t>(S_AI_STRENGTH) = ri(0, 7);
    // the resource: a version-2 AIDriverResource2, every driver named
    uint8_t* res = g_arena + A_RES;
    for (int b = 0; b < 8; b++) {
        for (int j = 0; j < 16; j++) random_driver2(res + b * 0x2204 + j * 0x220, true);
        *(int32_t*)(res + b * 0x2204 + 0x2200) = rnd() % 8 ? ri(1, 16) : ri(0, 16);
    }
    // the lounge
    for (uint8_t* l : {g_arena + A_LOUNGE, g_arena + A_LOUNGE2}) {
        AIDriverLounge* lo = (AIDriverLounge*)l;
        lo->vtable = P<void*>(VT_LOUNGE);
        lo->res = res;
        for (int i = 0; i < 8; i++) {
            int n = rnd() % 10 ? ri(1, 16) : 0;
            lo->tables[i].count = n;
            for (int j = 0; j < 16; j++) {
                bool random_driver = rnd() % 3 == 0 && str_count(i) > 0;
                lo->tables[i].d[j] = random_driver ? *P<AIDriver2*>(S_STR + 4 * (17 * i + j % str_count(i)))
                                                   : (AIDriver2*)(res + i * 0x2204 + j * 0x220);
            }
        }
        lo->offset = rnd() % 4 ? ri(0, 40) : (int32_t)(rnd() & 0x7fffffff);
    }
    *P<void*>(S_LOUNGE_IMPL) = g_arena + A_LOUNGE;
    *P<void*>(S_LOUNGE) = g_arena + A_LOUNGE;
    // the config dialog's state
    for (int b = 0; b < 8; b++) {
        for (int j = 0; j < 16; j++) random_driver2(P<uint8_t>(S_TAB + b * 0x2204 + j * 0x220), rnd() % 2 != 0);
        tab_count(b) = rnd() % 6 ? ri(1, 16) : 0;
    }
    random_floats(P<void>(S_LIVE_BASE), 0x250, -2, 2);                  // views, live track, live driver
    random_driver2(P<uint8_t>(S_LIVE), true);
    for (int k = 0; k < 3; k++) *P<float>(S_LIVE_B38 + 4 * k) = sp(rf(-0.2f, 1.2f));
    *P<float>(S_LIVE_F04) = sp(rf(-2, 10));
    *P<float>(S_LIVE_F0C) = sp(rnd() % 8 ? rf(0, 200) : rf(-1e5f, 1e5f));
    *P<float>(S_LIVE_F0E) = sp(rnd() % 8 ? rf(0, 100) : rf(-1e5f, 1e5f));
    cur_str() = rnd() % 40 ? ri(0, 7) : ri(-1, 9);
    cur_car() = rnd() % 40 ? ri(0, 16) : ri(-2, 18);
    cur_trk() = rnd() % 40 ? ri(0, 15) : ri(-1, 17);
    for (uint32_t s = S_STRENGTH_TEXT; s < S_LIVE_BASE + LIVE_SIZE; s += 64) {
        int n = ri(0, 40);
        for (int k = 0; k < 64; k++) P<char>(s)[k] = (char)(k < n ? 'A' + rnd() % 26 : 0);
    }
    *P<uint8_t>(S_CLIP_GUARD) = (uint8_t)rnd();
    *P<uint8_t>(S_HAVE_CLIP) = (uint8_t)(rnd() % 3);
    random_driver2(P<uint8_t>(S_CLIP), true);
    // the arena's odds and ends
    random_bytes(g_arena + A_SCRATCH, SCRATCH_N);
    for (int i = 0; i < 16; i++) {
        char* s = (char*)g_arena + A_STRINGS + i * STRING_N;
        int n = ri(0, 24);
        for (int k = 0; k < n; k++) s[k] = (char)('a' + rnd() % 26);
        s[n] = 0;
    }
    random_bytes(g_arena + A_BUF, 0x100);
    for (int i = 0; i < 32; i++) ((int32_t*)(g_arena + A_MAPIN))[i] = rnd() % 5 ? ri(0, 15) : ri(-3, 20);
    random_bytes(g_arena + A_OPTIONS, 0x40);
    *(void**)(g_arena + A_VIEWER) = g_viewer_vt;
    random_bytes(g_arena + A_OUT, 0x220);
}

// ---- running one call both ways ------------------------------------------------------------------------------------
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

struct Stat { const char* name; long calls, changed, fails, fp_fails, fp_info, crashes; };
static Stat g_stats[96];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0, 0};
    return g_stats[g_nstats++];
}
static uint8_t *g_start, *g_after;                      // .data then the arena
static uint32_t g_world_n;
static Footprint g_fp;
static int g_world;
struct Mask { uint8_t* p; uint32_t n; };                // bytes the compare ignores (the original's stack junk)
static Mask g_masks[16];
static int g_nmasks;
static bool g_ret_is_self;
static bool g_crashed;                                  // a call crashed (both ways): the world is abandoned                              // a constructor: its return value is its `this`
static bool g_trace = getenv("VP_TRACE") != 0;         // VP_TRACE=1: name each check as it starts

static void snapshot(uint8_t* to) { memcpy(to, g_data, g_data_n); memcpy(to + g_data_n, g_arena, ARENA_N); }
static void restore(const uint8_t* from) { memcpy(g_data, from, g_data_n); memcpy(g_arena, from + g_data_n, ARENA_N); }
static uint8_t* live(uint32_t i) { return i < g_data_n ? g_data + i : g_arena + (i - g_data_n); }
static uint32_t index_of(const uint8_t* p) { return p >= g_arena && p < g_arena + ARENA_N ? g_data_n + (uint32_t)(p - g_arena) : (uint32_t)(p - g_data); }
static bool masked(const uint8_t* p) {
    for (int k = 0; k < g_nmasks; k++) if (p >= g_masks[k].p && p < g_masks[k].p + g_masks[k].n) return true;
    return false;
}

static const char* g_running;
static int crash_filter(EXCEPTION_POINTERS* e, bool orig) {
    printf("CRASH in %s (%s, world %d): exception %08lx at %p\n", g_running, orig ? "original" : "rewrite", g_world,
           e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
#if defined(__GNUC__) && !defined(__clang__)
template <typename Run> static uint64_t guarded(Run& run, bool orig) {
    uint64_t r = 0;
    if (vp_try([&] { r = run(orig); }, [&](EXCEPTION_POINTERS* e) { return crash_filter(e, orig); }))
        return 0xdeaddeaddeaddeadull;
    return r;
}
#else
template <typename Run> static uint64_t guarded(Run& run, bool orig) {
    __try {
        return run(orig);
    } __except (crash_filter(GetExceptionInformation(), orig)) {
        return 0xdeaddeaddeaddeadull;
    }
}
#endif

template <typename Run, typename Fp> static void check(const char* name, Run run, Fp footprint) {
    Stat& st = stat(name);
    st.calls++;
    if (g_trace) printf("[%d] %s\n", g_world, name);
    snapshot(g_start);
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    footprint(g_fp);
    uint32_t seed = rnd() | 1;
    g_script = seed;
    g_heap_next = 0;
    g_pass = 0; g_nlog[0] = 0;
    g_running = name;
    uint64_t ro = guarded(run, true);
    snapshot(g_after);
    restore(g_start);
    g_script = seed;
    g_heap_next = 0;
    g_pass = 1; g_nlog[1] = 0;
    uint64_t rn = guarded(run, false);
    if (ro == 0xdeaddeaddeaddeadull || rn == 0xdeaddeaddeaddeadull) { st.crashes++; g_crashed = true; }
    for (int k = 0; k < g_nmasks; k++)                 // the junk bytes: the original's
        for (uint32_t i = 0; i < g_masks[k].n; i++) g_masks[k].p[i] = g_after[index_of(g_masks[k].p + i)];
    bool changed = false, bad = (ro != rn && !g_ret_is_self) || g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], 4 * g_nlog[0]) != 0;
    int shown = 0;
    for (uint32_t i = 0; i < g_world_n; i += 4) {
        const uint8_t* a = g_after + i;
        const uint8_t* s = g_start + i;
        const uint8_t* b = live(i);
        if (memcmp(a, s, 4)) changed = true;
        if (memcmp(a, b, 4)) {
            if (!bad && st.fails < 4) printf("MISMATCH %s (world %d)\n", name, g_world);
            bad = true;
            if (st.fails < 4 && shown++ < 12) {
                uint32_t sa, aa, ba;
                memcpy(&sa, s, 4); memcpy(&aa, a, 4); memcpy(&ba, b, 4);
                char w[64];
                printf("  %s: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", where(b, w), sa, aa, fbits(aa), ba, fbits(ba));
            }
        }
    }
    if (changed) st.changed++;
    if (bad) {
        if (st.fails < 4) {
            if (shown == 0) printf("MISMATCH %s (world %d)\n", name, g_world);
            if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
            int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
            int diff = 0;
            for (int i = 0; i < nl && diff < 12; i++) {
                bool ha = i < g_nlog[0], hb = i < g_nlog[1];
                if (ha && hb && g_log[0][i] == g_log[1][i]) continue;
                printf("  log[%d] of %d/%d: original %08x, rewrite %08x\n", i, g_nlog[0], g_nlog[1], ha ? g_log[0][i] : 0, hb ? g_log[1][i] : 0);
                diff++;
            }
        }
        st.fails++;
    }
    // the footprint must cover every byte the original changed
    for (uint32_t i = 0; i < g_world_n; i++) {
        if (g_after[i] == g_start[i]) continue;
        uint8_t* p = live(i);
        bool in = masked(p);
        for (int k = 0; k < g_fp.n && !in; k++)
            in = p >= (uint8_t*)g_fp.r[k].p && p < (uint8_t*)g_fp.r[k].p + g_fp.r[k].n;
        if (!in) {
            char w[64];
            if (g_fp.replay_only) {
                if (st.fp_info++ < 2) printf("footprint (replay_only) %s (world %d): %s changed outside it\n", name, g_world, where(p, w));
            } else if (st.fp_fails++ < 4) {
                printf("FOOTPRINT %s (world %d): %s changed outside it\n", name, g_world, where(p, w));
            }
            break;
        }
    }
    restore(g_after);                                  // go on from the original's result
    g_nmasks = 0;
    g_ret_is_self = false;
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
    check(VP_CAT(name_, FN),                                                                            \
          [&](bool orig) { auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN; return invoke(f); }, \
          [&](Footprint& fp) { VP_CAT(fpof_, FN)(fp); })

static void mask(void* p, uint32_t n) { if (g_nmasks < 16) g_masks[g_nmasks++] = {(uint8_t*)p, n}; }
static void mask_default_driver2(uint8_t* d) {         // an AIDriver2 built on the stack and copied whole
    mask(d + 0xc, 4); mask(d + 0x18, 4); mask(d + 0x3b, 1); mask(d + 0x76, 0x1a); mask(d + 0x214, 12);
}
static RandomAIDriver* some_random_driver() {          // one of the game's own, through str
    for (int tries = 0; tries < 16; tries++) {
        int i = ri(0, 7);
        if (str_count(i) > 0) return (RandomAIDriver*)(*P<uint8_t*>(S_STR + 4 * (17 * i + ri(0, str_count(i) - 1))) - 4);
    }
    return (RandomAIDriver*)(g_arena + A_SCRATCH);
}

static void run_world() {
    build_world();
    uint8_t* scratch = g_arena + A_SCRATCH;
    AIDriverLounge* lounge = (AIDriverLounge*)(g_arena + A_LOUNGE);
    AIDriverLounge* lounge2 = (AIDriverLounge*)(g_arena + A_LOUNGE2);
    g_crashed = false;
    for (int s = ri(10, 30); s-- && !g_crashed;) {
        switch (rnd() % 64) {
        // the skill constructors (scratch memory)
        case 0: g_ret_is_self = true; CHECK(TrackSkillInfo1_ctor, (TrackSkillInfo1*)(scratch + 4 * ri(0, 64)), 0); break;
        case 1: g_ret_is_self = true; CHECK(TrackSkillInfo2_ctor, (TrackSkillInfo2*)(scratch + 4 * ri(0, 64)), 0); break;
        case 2: g_ret_is_self = true; CHECK(SkillInfo0_ctor, (SkillInfo0*)(scratch + 4 * ri(0, 64)), 0); break;
        case 3: g_ret_is_self = true; CHECK(SkillInfo1_ctor, (SkillInfo1*)(scratch + 4 * ri(0, 64)), 0); break;
        case 4: g_ret_is_self = true; CHECK(SkillInfo2_ctor, (SkillInfo2*)(scratch + 4 * ri(0, 64)), 0); break;
        case 5: g_ret_is_self = true; CHECK(AIDriverTable_ctor, (AIDriverTable*)(scratch + 4 * ri(0, 64)), 0); break;
        case 6: g_ret_is_self = true; CHECK(AIDriver1_ctor, (AIDriver1*)(scratch + 4 * ri(0, 64)), 0); break;
        case 7: g_ret_is_self = true; CHECK(AIDriver2_ctor, (AIDriver2*)(scratch + 4 * ri(0, 64)), 0); break;
        case 8: g_ret_is_self = true; CHECK(AIDriverBucket1_ctor, (void*)(scratch + 4 * ri(0, 64)), 0); break;
        case 9: g_ret_is_self = true; CHECK(AIDriverBucket2_ctor, (void*)(scratch + 4 * ri(0, 16)), 0); break;
        case 10: CHECK(copy_driver_01, (AIDriver1*)scratch, (const AIDriver0*)(g_arena + A_RES + 4 * ri(0, 64))); break;
        case 11: CHECK(copy_driver_12, (AIDriver2*)scratch, (const AIDriver1*)(g_arena + A_RES + 4 * ri(0, 64))); break;
        // the lounge
        case 12: CHECK(Lounge_copy_res, lounge, 0); break;
        case 13: CHECK(Lounge_fixup_res, lounge, 0); break;
        case 14: case 15: CHECK(Lounge_copy_random_drivers, lounge, 0); break;
        case 16: case 17: case 18: case 19: CHECK(Lounge_Count, lounge, 0, ri(0, 7)); break;
        case 20: case 21: {
            int i = ri(0, 7);
            if (lounge->tables[i].count > 0) CHECK(Lounge_Get, lounge, 0, i, rnd() % 4 ? ri(0, 20) : (int32_t)(rnd() & 0x3fffffff));
            break;
        }
        case 22: CHECK(Lounge_Count, lounge, 0, ri(0, 7)); break;
        case 23: {
            uint32_t v = (uint32_t)(rnd() % 4 ? ri(0, 1) : ri(2, 9));
            for (int b = 0; b < 8; b++)                   // sane counts where this version keeps them
                if (v < 2) *(int32_t*)(g_arena + A_RES + (v == 0 ? 0x7c0 + b * 0x7c4 : 0x1c00 + b * 0x1c04)) = ri(0, 16);
            CHECK(convert_obsolete_resource, (void*)(g_arena + A_RES), v);
            break;
        }
        case 24: CHECK(convert_resource_2OLD, (void*)(g_arena + A_RES)); break;
        case 25: CHECK(save_resource1, (const char*)0x004ebe70, (const void*)(g_arena + A_RES)); break;
        case 26: CHECK(save_resource2, (const char*)0x004ec218, (const void*)P<void>(S_TAB)); break;
        // the random drivers
        case 27: { int i = ri(0, 7); CHECK(RandomAIDriver_Get, i, ri(0, 17)); break; }
        case 28: g_ret_is_self = true; CHECK(RandomAIDriver_ctor, (RandomAIDriver*)(g_arena + A_RDRV), 0, ri(0, 7), fake_string(ri(0, 15))); break;
        case 29: CHECK(RandomAIDriver_init, some_random_driver(), 0); break;
        case 30: CHECK(EasyAIDriver_init, some_random_driver(), 0); break;
        case 31: CHECK(MedAIDriver_init, some_random_driver(), 0); break;
        case 32: CHECK(HardAIDriver_init, some_random_driver(), 0); break;
        case 33: case 34: {
            float lo = sp(rnd() % 2 ? rf(0, 3) : rf(-1000, 1000)), hi = sp(rnd() % 2 ? lo + rf(0, 3) : rf(-1000, 1000));
            CHECK(random_real, lo, hi);
            break;
        }
        case 35: CHECK0(RandomAIDriver_Initialize); break;
        // the config dialog
        case 36: CHECK(copypaste_cb, 0); break;
        case 37: CHECK(idle, (int32_t*)0); break;
        case 38: CHECK0(update_lap_string); break;
        case 39: CHECK0(copy_lounge_to_tab); break;
        case 40: CHECK0(cfg_update_strings); break;
        case 41: if ((uint32_t)cur_str() < 8 && (uint32_t)cur_car() <= 16) CHECK0(copy_tab_to_live); break;
        case 42: CHECK(copy_to_live, (const AIDriver2*)(rnd() % 2 ? g_arena + A_OUT : P<uint8_t>(S_TAB + 0x220 * ri(0, 100)))); break;
        case 43: CHECK(copy_from_live, (AIDriver2*)(g_arena + A_OUT)); break;
        case 44: {
            int32_t s = cur_str(), n = (uint32_t)s < 8 ? tab_count(s) : 0;
            if ((uint32_t)s >= 8 || (uint32_t)cur_car() > 16 || (uint32_t)n > 16) break;
            if (n > 0 && n != cur_car()) {                // the freed slot gets the original's stack junk
                mask_default_driver2(tab_driver(s, n - 1));
                int32_t car = cur_car() != 0 && n - 1 <= cur_car() ? n - 2 : cur_car();
                if (car == n - 1) {                       // ... and becomes the live driver
                    mask_default_driver2(P<uint8_t>(S_LIVE));
                    mask(P<void>(S_LIVE_F0C), 8);         // the views of its shorts at +0x0c, +0x0e
                }
            }
            CHECK(delete_cb, 0);
            break;
        }
        case 45: CHECK(copypaste_cb, (int)(rnd() % 3)); break;
        case 46: if ((uint32_t)cur_str() < 8 && (uint32_t)cur_car() <= 16) CHECK(undo_cb, 0); break;
        case 47: if ((uint32_t)cur_str() < 8 && (uint32_t)cur_car() <= 16) CHECK(save_cb, 0); break;
        case 48: if ((uint32_t)cur_str() < 8 && (uint32_t)cur_car() <= 16) CHECK0(copy_live_to_tab); break;
        case 49: if ((uint32_t)cur_str() < 8 && (uint32_t)cur_car() <= 16) CHECK(prev_car, 0); break;
        case 50: if ((uint32_t)cur_str() < 8 && (uint32_t)cur_car() <= 16) CHECK(next_car, 0); break;
        case 51: CHECK(next_trk, 0); break;
        case 52: CHECK(prev_trk, 0); break;
        case 53: if ((uint32_t)cur_str() < 8 && (uint32_t)cur_car() <= 16) CHECK(prev_str, 0); break;
        case 54: if ((uint32_t)cur_str() < 8 && (uint32_t)cur_car() <= 16) CHECK(next_str, 0); break;
        // start-up, the map, the getters
        case 55: case 56: CHECK(AIGetDriverForCar, ri(0, 15)); break;
        case 57: CHECK0(AIResetDriverMap); break;
        case 58: CHECK(AISetDriverMap, (int32_t*)(g_arena + A_MAPIN), rnd() % 4 ? 16 : ri(0, 17)); break;
        case 59: CHECK(verify_driver_map, (int32_t*)(g_arena + A_MAPIN), ri(-1, 32)); break;
        case 60: CHECK(AIGetDriverForCar, ri(0, 15)); CHECK(AIGetCarForDriver, ri(-1, 21)); break;
        case 61: {
            int32_t car = ri(0, 15);
            if (P<int32_t>(S_MAP)[car] >= 0) {
                int32_t s = *P<int32_t>(S_AI_STRENGTH);
                if (lounge->tables[s].count > 0) {
                    CHECK(AIGetSkill, car);
                    CHECK(AIGetDriverNameByCar, (char*)(g_arena + A_BUF), car);
                }
            }
            break;
        }
        case 62: CHECK(AIGetTrackNumber_name, fake_string(ri(0, 15)), rnd()); CHECK(AIGetTrackNumber_int, ri(-5, 20), rnd()); break;
        default: CHECK0(AIGetTrackNumber_world); CHECK(AITrackIsReversed, ri(-3, 20)); break;
        }
    }
    // last, what replaces the lounge or its resource
    if (g_crashed) return;
    switch (rnd() % 10) {
    case 0: CHECK(Lounge_Reload, lounge, 0); break;
    case 1: CHECK(Lounge_load_res, lounge, 0); break;
    case 2: CHECK(Lounge_load_res_name, lounge, 0, (const char*)(rnd() % 2 ? 0x004ebdfc : 0x004ebe0c)); break;
    case 3: g_ret_is_self = true; CHECK(Lounge_ctor, lounge2, 0); break;
    case 4: CHECK(Lounge_dtor, lounge2, 0); break;
    case 5: CHECK0(AIDriverBegin); break;
    case 6: CHECK0(AIDriverEnd); break;
    default: CHECK(AIDriverConfig, 0); break;
    }
}

#ifdef FIX_TESTS
// ==== the fix tests: more than 16 drivers in a strength =============================================================
static int g_fix_fail;
#define FIXCHECK(cond, ...)                                                                                     \
    do {                                                                                                        \
        if (!(cond)) {                                                                                          \
            if (g_fix_fail++ < 30) { printf("FIX FAIL: "); printf(__VA_ARGS__); printf("\n"); }                 \
        }                                                                                                       \
    } while (0)
static void run_fix_tests() {
    static uint8_t before[ARENA_N];
    AIDriverLounge* lounge = (AIDriverLounge*)(g_arena + A_LOUNGE);
    uint8_t* res = g_arena + A_RES;
    long orig_over_fix = 0, orig_over_copy = 0, runs = 0;
    for (int it = 0; it < 3000; it++) {
        build_world();
        int32_t cnt[8];
        for (int b = 0; b < 8; b++) {
            cnt[b] = rnd() % 3 ? ri(0, 16) : ri(17, it % 2 ? 20 : 40);
            *(int32_t*)(res + b * 0x2204 + 0x2200) = cnt[b];
        }
        if (it % 2) cnt[7] = *(int32_t*)(res + 7 * 0x2204 + 0x2200) = ri(17, 20);   // (the last bucket too: past the resource)
        runs++;
        memcpy(before, g_arena, ARENA_N);
        // the originals (only where their overrun stays inside the arena: up to 20 in the last bucket)
        if (cnt[7] <= 20) {
            ((decltype(&Lounge_fixup_res))(uintptr_t)0x0041d660)(lounge, 0);
            bool outside = false;
            for (int b = 0; b < 8; b++)
                for (int j = 16; j < cnt[b]; j++) outside = true;
            if (outside) orig_over_fix++;
            memcpy(g_arena, before, ARENA_N);
            ((decltype(&Lounge_copy_res))(uintptr_t)0x0041d600)(lounge, 0);
            if (memcmp(g_arena + A_LOUNGE + 0x228, before + A_LOUNGE + 0x228, 0x400 - 0x228) || outside) orig_over_copy++;
            memcpy(g_arena, before, ARENA_N);
        }
        // the fixed fixup_res: names on each strength's first min(count, 16) drivers, from its own 16; nothing else
        Lounge_fixup_res(lounge, 0);
        for (int b = 0; b < 8; b++) {
            const int n = cnt[b] > 16 ? 16 : cnt[b];
            for (int j = 0; j < 16; j++) {
                const uint32_t at = A_RES + b * 0x2204 + j * 0x220 + 0x210;
                const uint32_t want = j < n ? k_driver_names[b * 16 + j] : *(const uint32_t*)(before + at);
                FIXCHECK(*(const uint32_t*)(g_arena + at) == want, "fixup_res: strength %d driver %d (count %d)", b, j, cnt[b]);
            }
        }
        for (uint32_t i = 0; i < ARENA_N; i++) {
            if (g_arena[i] == before[i]) continue;
            const bool name_slot = i >= A_RES && i < A_RES + 0x11020 && ((i - A_RES) % 0x2204) < 0x2200 && ((i - A_RES) % 0x2204) % 0x220 >= 0x210 &&
                                   ((i - A_RES) % 0x2204) % 0x220 < 0x214;
            FIXCHECK(name_slot, "fixup_res wrote arena+0x%x, not a driver's name", i);
            if (!name_slot) break;
        }
        // the fixed copy_res: each table's first min(count, 16) drivers and that count; nothing past the tables
        memcpy(g_arena, before, ARENA_N);
        Lounge_copy_res(lounge, 0);
        for (int b = 0; b < 8; b++) {
            const int n = cnt[b] > 16 ? 16 : cnt[b];
            FIXCHECK(lounge->tables[b].count == n, "copy_res: strength %d's count %d (resource %d)", b, lounge->tables[b].count, cnt[b]);
            for (int j = 0; j < n; j++)
                FIXCHECK((uint8_t*)lounge->tables[b].d[j] == res + b * 0x2204 + j * 0x220, "copy_res: strength %d driver %d", b, j);
        }
        FIXCHECK(!memcmp(g_arena + A_LOUNGE + 0x228, before + A_LOUNGE + 0x228, ARENA_N - (A_LOUNGE + 0x228)) &&
                     !memcmp(g_arena, before, A_LOUNGE + 8),
                 "copy_res wrote outside the lounge's tables");
    }
    printf("more than 16 drivers: %ld resources; the originals wrote past the bucket in %ld (fixup_res) and past a "
           "table in %ld (copy_res); the fixed ones never\n", runs, orig_over_fix, orig_over_copy);
    FIXCHECK(orig_over_fix > 500 && orig_over_copy > 500, "too few overrunning resources");
}
#endif

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 2000;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_aidriver.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(0x400000 + ((IMAGE_DOS_HEADER*)0x400000)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".data", 6)) { g_data = (uint8_t*)0x400000 + sec[i].VirtualAddress; g_data_n = sec[i].Misc.VirtualSize; }
    // driver.obj's static initialisers: the 8 tables, the 41 random drivers, the config dialog's buckets
    ((void(__cdecl*)())0x0041d5e0)();
    for (uint32_t a = 0x0041e470; a <= 0x0041ebf0; a += 0x30) ((void(__cdecl*)())a)();
    ((void(__cdecl*)())0x0041f010)();
    int total = 0;
    for (int i = 0; i < 8; i++) total += str_count(i);
    printf("random drivers registered: %d (", total);
    for (int i = 0; i < 8; i++) printf("%s%d", i ? " " : "", str_count(i));
    printf(")\n");
    g_pristine = (uint8_t*)malloc(g_data_n);
    memcpy(g_pristine, g_data, g_data_n);
    g_world_n = g_data_n + ARENA_N;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_start = (uint8_t*)malloc(g_world_n);
    g_after = (uint8_t*)malloc(g_world_n);
    make_skips();
    install_stubs();
    for (g_world = 0; g_world < worlds; g_world++) {
        unsigned cw;
        _controlfp_s(&cw, g_world & 1 ? _PC_53 : _PC_24, _MCW_PC);    // the main thread's and the physics thread's modes
        g_special_pct = g_world % 4 == 0 ? 0 : g_world % 4 == 1 ? 1 : 5;
        run_world();
    }
    int failed = 0;
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("  %-36s %7ld calls, %7ld changed the world: %s%s%s%s\n", st.name, st.calls, st.changed,
               st.fails ? "DIFFERS" : "identical", st.fp_fails ? "  FOOTPRINT MISSES" : "",
               st.fp_info ? "  (replay_only: writes outside the footprint)" : "", st.crashes ? "  (crashed)" : "");
        failed += st.fails || st.fp_fails;
    }
    printf("%d worlds; %d of %d functions differ or miss their footprint\n", worlds, failed, g_nstats);
#ifdef FIX_TESTS
    run_fix_tests();
    printf("fix tests: %d failures\n", g_fix_fail);
    if (g_fix_fail) failed++;
#endif
    return failed ? 1 : 0;
}
