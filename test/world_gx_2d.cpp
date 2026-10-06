// world_gx_2d.cpp -- group G1a's rewrites (hook/gx_2d.cpp: the software 2D canvas, fonts, palettes, bitmaps, the debug
// graphs and the debug screens drawn with them) against the originals, on random canvases, outside the game
// (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_gx_2d.cpp
//        /Fo<dir>\ /Fe<dir>\world_gx_2d.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_gx_2d.exe [worlds] [seed]
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS -- the rewrites as the game has them: every random
//   world must still equal the original (canvases up to 96 rows), and then gxTriangle / fill_line's fix (docs/FIXES.md,
//   "Drawing") on canvases 600 to 3000 rows tall against vrmod's tablefix + needlefix race.exe (test/vrmod_image.h, the
//   same 2048-row tables and bounds), and against the stock original where that survives (up to 1024 rows).
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. First the pure
// pixel converters are run exhaustively (every 16-bit input; millions of 32-bit and blend inputs; every get_cvt_fn
// pair). Then each world builds, in an arena: eight canvases of random format (4444 / 555 / 565 / 8888), size, pitch
// (odd paddings included) and clip rectangle (sometimes empty or inverted), their pixel buffers filled with random
// bytes and surrounded by guard space; a stamp with random RLE rows, palettes and user data (and its resource form
// with row offsets); a font on it; palettes; a graph; the debug screens' statics, a fake car, a Deity with a vtable of
// loggers, a fake BMP / canvas file and resource. The current canvas is one of the eight. One function (random args:
// coordinates off the canvases, overlapping source and destination, aliased out-pointers, odd scales) runs as the
// ORIGINAL, the arena and the image's whole .data/.bss are saved, the world is restored and the REWRITE runs; the
// arena (every pixel byte), .data/.bss, the return value and the stubs' call logs must match, and every byte the
// original changed must lie in the rewrite's footprint unless it's replay_only (all of it runs on the main thread: no
// statics are exempt). The FPU runs at 24 or 53 bits, in 30% of the worlds with overflow and divide-by-zero unmasked.
//
// Stubbed (a jump to a logger): _SingleEnter / _SingleLeave, MemAlloc (a bump heap), MemFree, operator delete,
// LogPanic / LogReport (formatted by the host), sprintf / vsprintf (the host's), File* and ResourceGet / Forget (a
// scripted file and resource), the Windows file calls through the game's import slots, and every callee outside this
// group (the world, the terrain, projection, the ideal line, the mouse and keys, the physics, options, the UI, the
// screen lock / flip). This group's own functions and the C runtime's strncpy / strchr / _CIacos / __ftol run as the
// game's code, in both passes (each rewrite is checked against its original with the same callees).
#define _CRT_SECURE_NO_WARNINGS
#if defined(__GNUC__) && !defined(__clang__)
#define _WIN32_WINNT 0x0A00               // (mingw: GetCurrentThreadStackLimits)
#endif
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>
#include <utility>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

#ifndef FIX_TESTS
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes"; the fixes: /DFIX_TESTS)
#endif
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
// a pointer that may be left aimed at the function's own stack frame: listed, and compared as "same value, or both on
// this thread's stack" (the emulation's rule)
static void* g_stack_ptr[8];
static int g_stack_ptrs;
void Footprint::stack_ptr(void* p, const char* what) {
    add(p, 4, what);
    if (g_stack_ptrs < 8) g_stack_ptr[g_stack_ptrs++] = p;
}

#include "../hook/gx_2d.cpp"

// ---- random numbers --------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static int irange(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits_h(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float wildf() {
    uint32_t r = rnd(), k = r % 10;
    float s = (r & 0x100) ? -1.0f : 1.0f;
    switch (k) {
    case 0: return bitsf(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return s * INFINITY;
    case 2: return s * range(1e30f, 3e38f);
    case 3: return s * range(1e-38f, 1e-30f);
    case 4: return (r & 0x200) ? 0.0f : -0.0f;
    case 5: return s * range(1e6f, 1e9f);
    default: return s * range(0.0f, 100.0f);
    }
}

// ---- the original, loaded at 0x400000 ---------------------------------------------------------------------------
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

// ---- the arena ---------------------------------------------------------------------------------------------------
enum : uint32_t {
    ARENA_BYTES = 0x400000,
    A_CANV = 0x0000,               // 8 canvases, 0x40 apart
    A_ARGS = 0x1000,               // small arguments (ints, strings, points)
    A_TABLES = 0x2000,             // fill_line's two tables (256 ints each)
    A_PAL = 0x3000,                // two palettes (0x400)
    A_FONT = 0x4000,               // a gxFont (0x608)
    A_GRAPH = 0x4800,              // GraphInfo
    A_STR = 0x4900,                // strings (track names, ...)
    A_MISC = 0x6000,               // the debug screens' objects (car info, the engine, a Deity and its vtable, records)
    A_RES = 0x8000,                // a canvas resource (header + pixels), 0x8000
    A_STAMP = 0x10000,             // the stamp, pointers resolved (0x20000)
    A_STAMP2 = 0x30000,            // the same stamp as a fresh resource (offsets), 0x20000
    A_HEAP = 0x50000, A_HEAP_END = 0x150000,
    A_FILE = 0x150000,             // the scripted file's bytes (0x10000)
    A_PIX = 0x160000,              // 8 pixel buffers, 0x20000 apart, each at +0x8000
    A_SCREEN = A_PIX + 7 * 0x20000 + 0x8000,
};
static uint8_t* g_arena;
static uint8_t* g_arena_snap;
static uint8_t* g_arena_after;
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_snap;
static uint8_t* g_data_after;
static uint8_t* AR(uint32_t off) { return g_arena + off; }
static gxCanvas* CV(int i) { return (gxCanvas*)AR(A_CANV + 0x40 * i); }
static bool in_arena(const void* p, uint32_t n) { return (const uint8_t*)p >= g_arena && (const uint8_t*)p + n <= g_arena + ARENA_BYTES && n < ARENA_BYTES; }
// a pointer as logged: an arena offset, an address in the game's image, or 'STAK' (a stack local: each pass's differs)
static uint32_t rel(const void* p) {
    if (!p) return 0;
    if (in_arena(p, 1)) return (uint32_t)((const uint8_t*)p - g_arena) | 0xa0000000u;
    const uint32_t a = (uint32_t)(uintptr_t)p;
    if (a >= 0x400000 && a < 0x600000) return a;
    return 'STAK';
}

// ---- the stubs' log and script -----------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 16 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void log_bytes(const void* p, int n) { for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, (const uint8_t*)p + i, n - i < 4 ? n - i : 4); log_word(w); } }
static void log_str(const char* s) {
    if (!s) { log_word(0xdeadbeef); return; }
    size_t n = strnlen(s, 0x1000);
    log_word((uint32_t)n);
    log_bytes(s, (int)n);
}

struct Script {
    uint32_t alloc_fail;           // bit per MemAlloc call: NULL
    int res_kind;                  // ResourceGet: 0 none, 1 the stamp (fresh), 2 the stamp (loaded), 3 the canvas resource
    uint32_t res_version;
    uint8_t res_fresh;
    int file_open;                 // FileOpen / FileCreate / CreateFileA: 0 fail
    uint32_t read_fail;            // bit per FileReadExact / ReadFile call: fail
    int focus;                     // WorldGetFocusCar
    int game_state[40];            // GetGameState per call
    uint8_t key_hit[40];           // KeyHit per call
    uint16_t key_get[40];          // KeyGet per call
    uint8_t grab[40];              // gxGrabScreen per call
    uint8_t mouse_ok; int32_t mouse_ev[4];
    float terrain;
    uint8_t proj_ok[8]; float proj[8][3];
    float elapsed;
    int32_t laps; uint8_t abort_race;
    uint8_t opt_bool; char opt_str[2][24];
    int32_t dialog_result; uint32_t dialog_toggle;
    uint8_t lap_null;
};
static Script g_script;
static uint8_t* g_heap;
static int g_alloc_calls, g_read_calls, g_game_calls, g_hit_calls, g_get_calls, g_grab_calls, g_proj_calls;
static uint32_t g_file_pos;

static void* __cdecl stub_MemAlloc(int n) {
    log_word('MALC'); log_word((uint32_t)n);
    const int k = g_alloc_calls++;
    if (k < 32 && ((g_script.alloc_fail >> k) & 1)) { log_word(0); return 0; }
    if (n < 0 || (uint32_t)n > (uint32_t)(AR(A_HEAP_END) - g_heap)) { log_word(1); return 0; }
    uint8_t* p = g_heap;
    g_heap += ((uint32_t)n + 7) & ~7u;
    log_word(rel(p));
    return p;
}
static void __cdecl stub_MemFree(void* p) { log_word('MFRE'); log_word(rel(p)); }
static void __cdecl stub_delete(void* p) { log_word('DELE'); log_word(rel(p)); }
static void __cdecl stub_single(int h, const char* file, int line) { log_word('SNGL'); log_word((uint32_t)h); log_word((uint32_t)(uintptr_t)file); log_word((uint32_t)line); }
static void log_fmt(uint32_t tag, const char* fmt, va_list ap) {
    char buf[0x1000];
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    buf[sizeof buf - 1] = 0;
    log_word(tag); log_word((uint32_t)(uintptr_t)fmt); log_str(buf);
}
static void __cdecl stub_panic(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_fmt('PANC', fmt, ap); va_end(ap); }
static void __cdecl stub_report(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_fmt('REPT', fmt, ap); va_end(ap); }
static int __cdecl stub_vsprintf(char* buf, const char* fmt, va_list ap) {
    int r = vsprintf(buf, fmt, ap);
    log_word('VSPR'); log_word((uint32_t)(uintptr_t)fmt); log_str(buf);
    return r;
}
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    log_word('SPRF'); log_word((uint32_t)(uintptr_t)fmt); log_str(buf);
    return r;
}
// files
static int __cdecl stub_FileOpen(const char* name) { log_word('FOPN'); log_str(name); g_file_pos = 0; return g_script.file_open; }
static int __cdecl stub_FileCreate(const char* name) { log_word('FCRE'); log_str(name); return g_script.file_open; }
static uint8_t __cdecl stub_FileReadExact(int fh, void* buf, int n) {
    log_word('FRDX'); log_word((uint32_t)fh); log_word(rel(buf)); log_word((uint32_t)n);
    const int k = g_read_calls++;
    if (k < 32 && ((g_script.read_fail >> k) & 1)) return 0;
    if (n < 0 || g_file_pos + (uint32_t)n > 0x10000 || !in_arena(buf, (uint32_t)n)) return 0;
    memcpy(buf, AR(A_FILE) + g_file_pos, n);
    g_file_pos += n;
    return 1;
}
static bool g_mask_bmp_header;
static uint8_t __cdecl stub_FileWrite(int fh, const void* p, int n) {
    log_word('FWRT'); log_word((uint32_t)fh); log_word((uint32_t)n);
    if (n > 0 && n <= 0x10000) {
        uint8_t tmp[0x100];
        if (g_mask_bmp_header && n == 0x28) {          // bmp_write's uninitialised biYPelsPerMeter
            memcpy(tmp, p, 0x28);
            memset(tmp + 0x1c, 0, 4);
            log_bytes(tmp, n);
        } else log_bytes(p, n);
    }
    return 1;
}
static void __cdecl stub_FileClose(int& fh) { log_word('FCLS'); log_word((uint32_t)fh); fh = 0; }
static void* __stdcall stub_CreateFileA(const char* name, uint32_t acc, uint32_t share, void* sec, uint32_t disp, uint32_t attr, void* tmpl) {
    log_word('CRFA'); log_str(name); log_word(acc); log_word(share); log_word((uint32_t)(uintptr_t)sec); log_word(disp); log_word(attr);
    log_word((uint32_t)(uintptr_t)tmpl);
    g_file_pos = 0;
    return g_script.file_open ? (void*)(uintptr_t)g_script.file_open : (void*)(intptr_t)-1;
}
static int __stdcall stub_ReadFile(void* h, void* buf, uint32_t n, uint32_t* got, void* ov) {
    log_word('RDFL'); log_word((uint32_t)(uintptr_t)h); log_word(rel(buf)); log_word(n); log_word(rel(got)); log_word((uint32_t)(uintptr_t)ov);
    const int k = g_read_calls++;
    if (k < 32 && ((g_script.read_fail >> k) & 1)) { if (got) *got = 0; return 0; }
    uint32_t m = n;
    if (g_file_pos + m > 0x10000) m = 0x10000 - g_file_pos;
    if (!in_arena(buf, m) && (uint32_t)(uintptr_t)buf < 0x5d0000) m = 0;                 // (the bmp statics are in .data)
    memcpy(buf, AR(A_FILE) + g_file_pos, m);
    g_file_pos += m;
    if (got) *got = m;
    return 1;
}
static int __stdcall stub_CloseHandle(void* h) { log_word('CLSH'); log_word((uint32_t)(uintptr_t)h); return 1; }
// resources
static void* __cdecl stub_ResourceGet(const char* name, uint32_t type, uint32_t& version, int* size, uint8_t* p5, uint8_t* fresh) {
    log_word('RGET'); log_str(name); log_word(type); log_word((uint32_t)(uintptr_t)p5);
    version = g_script.res_version;
    if (size) *size = 0x1234;
    if (fresh) *fresh = g_script.res_fresh;
    switch (g_script.res_kind) {
    case 1: return AR(A_STAMP2);
    case 2: return AR(A_STAMP);
    case 3: return AR(A_RES);
    default: return 0;
    }
}
static uint8_t __cdecl stub_ResourceForget(void* p) { log_word('RFGT'); log_word(rel(p)); return 1; }

// the debug screens' callees
static int __cdecl stub_WorldGetFocusCar() { log_word('FOCS'); return g_script.focus; }
static uint8_t __cdecl stub_MouseGetEvent(int32_t* ev) {
    log_word('MGEV'); log_word(rel(ev));
    if (g_script.mouse_ok) memcpy(ev, g_script.mouse_ev, 16);
    return g_script.mouse_ok;
}
static void __cdecl stub_void(const char*) {}
static void __cdecl stub_MouseCenter() { log_word('MCTR'); }
static float __cdecl stub_TerrainGetHeight(uint32_t x, uint32_t z) { log_word('TERH'); log_word(x); log_word(z); return g_script.terrain; }
static uint8_t __cdecl stub_mrProjectPoint(const float* in, float* out) {
    log_word('MPRJ'); log_bytes(in, 12); log_word(rel(out));
    const int k = g_proj_calls++ & 7;
    if (g_script.proj_ok[k]) memcpy(out, g_script.proj[k], 12);
    return g_script.proj_ok[k];
}
static uint8_t __cdecl stub_project(const float* in, float* out) {
    log_word('PRJ2'); log_bytes(in, 8); log_word(rel(out));
    const int k = g_proj_calls++ & 7;
    if (g_script.proj_ok[k]) memcpy(out, g_script.proj[k], 8);
    return g_script.proj_ok[k];
}
static void __fastcall stub_IdealLine_Draw(void* self, int, const float* p, uint32_t flag) {
    log_word('ILDR'); log_word(rel(self)); log_bytes(p, 8); log_word(flag & 0xff);
}
static void __cdecl stub_log_int(int32_t v) { log_word('LGI1'); log_word((uint32_t)v); }
static void __cdecl stub_PhysicsSetSpeed(int32_t v) { log_word('PSPD'); log_word((uint32_t)v); }
static void __cdecl stub_TaskSleep(int32_t v) { log_word('SLEP'); log_word((uint32_t)v); }
static void __cdecl stub_PhysicsUnpause() { log_word('PUNP'); }
static void __cdecl stub_SoundMuteCars() { log_word('SMUT'); }
static void __cdecl stub_PhysicsPause() { log_word('PPAU'); }
static void __cdecl stub_PhysicsAbortRace() { log_word('PABT'); }
static void __cdecl stub_Win32Idle() { log_word('IDLE'); }
static void __cdecl stub_KeyClear() { log_word('KCLR'); }
static void __cdecl stub_gxReleaseScreen() { log_word('RELS'); }
static void __cdecl stub_gxFlip() { log_word('FLIP'); }
static int __cdecl stub_GetGameState() { log_word('GSTA'); const int k = g_game_calls++; return k < 40 ? g_script.game_state[k] : 3; }
static uint8_t __cdecl stub_KeyHit() { log_word('KHIT'); const int k = g_hit_calls++; return k < 40 ? g_script.key_hit[k] : 1; }
static uint16_t __cdecl stub_KeyGet() { log_word('KGET'); const int k = g_get_calls++; return k < 40 ? g_script.key_get[k] : 0x1b; }
static uint8_t __cdecl stub_gxGrabScreen(gxCanvas* c) {
    log_word('GRAB'); log_word(rel(c));
    const int k = g_grab_calls++;
    const uint8_t ok = k < 40 ? g_script.grab[k] : 1;
    if (ok) {
        c->format = 4; c->flags = 0x80; c->pixels = AR(A_SCREEN); c->w = 96; c->h = 80; c->pitch = 96 * 2;
        c->cx0 = 0; c->cy0 = 0; c->cx1 = 96; c->cy1 = 80;
    }
    return ok;
}
static void* __cdecl stub_CarMgrGetInfo(int32_t i) { log_word('CMGI'); log_word((uint32_t)i); return AR(A_MISC); }
static float __cdecl stub_WorldGetElapsedTime() { log_word('ELAP'); return g_script.elapsed; }
static const char* __cdecl stub_PhysicsTimeString(uint32_t t, uint32_t flag) { log_word('PTSR'); log_word(t); log_word(flag & 0xff); return (const char*)AR(A_STR + 0x200); }
static const uint8_t* __cdecl stub_RecordGetLapResult(int32_t a, int32_t lap) {
    log_word('RLAP'); log_word((uint32_t)a); log_word((uint32_t)lap);
    if (g_script.lap_null && lap == 3) return 0;
    uint8_t* r = AR(A_MISC + 0x1000);
    *(float*)(r + 4) = 60.0f + (float)lap;
    return r;
}
static int32_t __fastcall stub_deity_laps(void*, int, int32_t a) { log_word('DLAP'); log_word((uint32_t)a); return g_script.laps; }
static uint8_t __fastcall stub_deity_abort(void*, int, int32_t a) { log_word('DABT'); log_word((uint32_t)a); return g_script.abort_race; }
static void __cdecl stub_OptionsGetBool(const char* sec, const char* key, uint8_t* v) {
    log_word('OPGB'); log_str(sec); log_str(key); log_word(rel(v));
    *v = g_script.opt_bool;
}
static void __cdecl stub_OptionsGetStr(const char* sec, const char* key, char* v, int32_t n) {
    log_word('OPGS'); log_str(sec); log_str(key); log_word((uint32_t)n); log_str(v);
    const char* s = g_script.opt_str[n > 12 ? 1 : 0];
    if (n > 0) { strncpy(v, s, n - 1); v[n - 1] = 0; }
}
static void __cdecl stub_OptionsSetStr(const char* sec, const char* key, const char* v) { log_word('OPSS'); log_str(sec); log_str(key); log_str(v); }
static void __cdecl stub_OptionsSetBool(const char* sec, const char* key, uint32_t v) { log_word('OPSB'); log_str(sec); log_str(key); log_word(v & 0xff); }
static const char* __cdecl stub_GetTrackName(int32_t i) { log_word('TRKN'); log_word((uint32_t)i); return (const char*)AR(A_STR + 0x20 * (i & 7)); }
static const char* __cdecl stub_GetAIStrengthString(int32_t i) { log_word('AISS'); log_word((uint32_t)i); return (const char*)AR(A_STR + 0x100 + 0x10 * (i & 7)); }
// UIDoDialog: the dialog and its 28 items logged (pointers relative to the dialog, so either frame compares), some
// check boxes toggled through their data pointers
static int32_t __cdecl stub_UIDoDialog(const uint32_t* dlg, int32_t w, int32_t h, int32_t x, int32_t y, int32_t mode) {
    log_word('UIDD'); log_word(dlg[0]); log_word(dlg[1]); log_word(dlg[2]); log_word(dlg[4]);
    log_word(w); log_word(h); log_word(x); log_word(y); log_word(mode);
    const uint32_t base = (uint32_t)(uintptr_t)dlg;
    const uint8_t* items = (const uint8_t*)(uintptr_t)dlg[3];
    log_word((uint32_t)(uintptr_t)items - base);
    for (int i = 0; i < 28; i++) {
        const uint32_t* it = (const uint32_t*)(items + 0x38 * i);
        for (int k = 0; k < 14; k++) {
            uint32_t v = it[k];
            if (k == 8 && v >= base - 0x200 && v < base + 0x800) v = (v - base) | 0x50000000u;
            log_word(v);
        }
        if (it[0] == 0xb && it[8] && ((g_script.dialog_toggle >> i) & 1)) *(uint8_t*)(uintptr_t)it[8] ^= 1;
    }
    return g_script.dialog_result;
}
static void __cdecl stub_check_learning(uint8_t* tracks, uint8_t* strengths) { log_word('CHKL'); log_bytes(tracks, 16); log_bytes(strengths, 8); }
static int32_t __cdecl stub_UIStyleWidth(int32_t style, const char* s) { log_word('UISW'); log_word((uint32_t)style); log_str(s); return (int32_t)strlen(s) * 7 + style; }
static void __cdecl stub_UIStyleDraw(int32_t style, int32_t x, int32_t y, const char* s, uint32_t f) {
    log_word('UISD'); log_word((uint32_t)style); log_word((uint32_t)x); log_word((uint32_t)y); log_str(s); log_word(f);
}
// the graph's function: a fixed wiggle of x, sometimes NaN
static float __cdecl plot_fn(float x, void* ctx) {
    log_word('PLOT'); log_word(fbits_h(x)); log_word(rel(ctx));
    const uint32_t k = ctx ? *(const uint32_t*)ctx : 0;
    if (k == 7 && x > 0.5f) return bitsf(0x7fc00000);
    return (float)(sin((double)x * 3.0 + k) * 2.0 + x * 0.25);
}

// the game's CRT must never put up a modal box: its fatal paths end the test quietly
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- building the world -------------------------------------------------------------------------------------------
static const int8_t k_formats[4] = {1, 3, 4, 5};
static int bpp(int8_t f) { return f == 5 ? 4 : 2; }
// a canvas of a given format (0: random), its buffer random bytes; the clip usually the canvas, sometimes a sub-rectangle,
// empty or inverted
static void make_canvas(int i, int8_t fmt) {
    gxCanvas* c = CV(i);
    if (!fmt) fmt = k_formats[rnd() % 4];
    c->format = fmt;
    c->flags = (uint8_t)(chance(50) ? 0x80 : 0x82);
    c->_pad = (uint16_t)rnd();
    const int w = chance(5) ? irange(1, 3) : irange(1, 96);
    const int h = chance(5) ? irange(1, 3) : irange(1, 96);
    static const int pads[] = {0, 0, 0, 2, 4, 6, 1, 3, 8, 12};
    const int pad = pads[rnd() % 10];
    c->w = w;
    c->h = h;
    c->pitch = w * bpp(fmt) + pad;
    c->pixels = AR(A_PIX + 0x20000 * i + 0x8000);
    const int k = rnd() % 10;
    if (k < 5) { c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = h; }
    else if (k < 9) {
        c->cx0 = irange(0, w - 1); c->cx1 = irange(c->cx0 + 1, w);
        c->cy0 = irange(0, h - 1); c->cy1 = irange(c->cy0 + 1, h);
    } else { c->cx0 = irange(0, w); c->cx1 = irange(0, w); c->cy0 = irange(0, h); c->cy1 = irange(0, h); }
    uint32_t* px = (uint32_t*)(c->pixels - 0x8000);
    for (int j = 0; j < 0x20000 / 4; j++) px[j] = rnd();
}
static int8_t fmt16() { const int k = rnd() % 10; return k < 5 ? 4 : k < 9 ? 3 : 1; }

// a stamp: w x h, count frames, random RLE rows (runs >= 1 mostly, some zero-length ones between), palettes, user data.
// At A_STAMP with row pointers; at A_STAMP2 the same with offsets from its data (a fresh resource).
static void make_stamp() {
    uint8_t* s = AR(A_STAMP);
    const int w = irange(1, 40), h = irange(1, 40), count = irange(1, 4);
    int32_t* hd = (int32_t*)s;
    hd[0] = w; hd[1] = h; hd[2] = irange(-6, w + 2); hd[3] = irange(-6, h + 2); hd[4] = count;
    for (int i = 0x14; i < 0x814; i += 4) *(uint32_t*)(s + i) = rnd();
    uint8_t** rows = (uint8_t**)(s + 0x81c);
    uint8_t* data = (uint8_t*)(rows + h * count);
    *(uint8_t***)(s + 0x814) = rows;
    *(uint8_t**)(s + 0x818) = data;
    uint8_t* p = data;
    for (int r = 0; r < h * count; r++) {
        rows[r] = p;
        int col = 0;
        while (col < w) {
            int run = chance(90) ? irange(1, 20) : chance(50) ? irange(1, 127) : 0;
            if (chance(50)) {
                *p++ = (uint8_t)(0x80 | run);
                for (int k = 0; k < run; k++) *p++ = (uint8_t)rnd();
            } else *p++ = (uint8_t)run;
            col += run;
        }
        if (chance(30)) *p++ = (uint8_t)irange(1, 5);                   // (a run past the end)
    }
    // the fresh resource: offsets from the data
    uint8_t* s2 = AR(A_STAMP2);
    const uint32_t bytes = (uint32_t)(p - s);
    memcpy(s2, s, bytes < 0x20000 ? bytes : 0x20000);
    uint32_t* rows2 = (uint32_t*)(s2 + 0x81c);
    for (int r = 0; r < h * count; r++) rows2[r] = (uint32_t)(rows[r] - data);
    *(uint32_t*)(s2 + 0x814) = rnd();
    *(uint32_t*)(s2 + 0x818) = rnd();
}
static void make_font() {
    uint8_t* f = AR(A_FONT);
    *(uint8_t**)f = chance(95) ? AR(A_STAMP) : 0;
    const int count = *(int32_t*)(AR(A_STAMP) + 0x10);
    int16_t* fr = (int16_t*)(f + 4);
    int16_t* adv = (int16_t*)(f + 0x204);
    int16_t* xo = (int16_t*)(f + 0x404);
    for (int i = 0; i < 256; i++) {
        fr[i] = (int16_t)(chance(30) ? -1 : irange(-1, count));
        adv[i] = (int16_t)irange(-2, 14);
        xo[i] = (int16_t)irange(-3, 6);
    }
    *(int32_t*)(f + 0x604) = irange(0, 20);
}
static void rand_str(char* s, int maxlen) {
    static const char chars[] = "abcdefghijklmnop XYZ0123456789.-#%\x7f\x80\xff\x01\t";
    const int n = irange(0, maxlen);
    for (int i = 0; i < n; i++) s[i] = chars[rnd() % (sizeof chars - 1)];
    s[n] = 0;
}

enum : uint32_t {
    X_CANVAS = 0x004efbf8, X_SCREEN_FORMAT = 0x00522b20, X_SCR_W = 0x005228f4, X_SCR_H = 0x005228d4, X_DEITY = 0x005218ac,
};
static const uint32_t k_colors[] = {0x509988, 0x509968, 0x5099c8, 0x509970, 0x509974, 0x5099d8, 0x520abc, 0x520b04, 0x520bf0,
                                    0x521088, 0x520ca0, 0x5214f8, 0x5214fc, 0x521508, 0x52152c, 0x521520, 0x52151c, 0x522a88,
                                    0x522a98, 0x522a44};
static void setf(uint32_t a, float f) { memcpy((void*)(uintptr_t)a, &f, 4); }
static void set32(uint32_t a, uint32_t v) { memcpy((void*)(uintptr_t)a, &v, 4); }

static void setup_world() {
    for (int i = 0; i < 8; i++) make_canvas(i, 0);
    uint32_t* w = (uint32_t*)AR(A_ARGS);
    for (int i = 0; i < 0x1000 / 4; i++) w[i] = rnd() % 200 - 50;
    for (int i = 0; i < 0x1000 / 4; i++) ((uint32_t*)AR(A_TABLES))[i] = rnd();
    for (int i = 0; i < 0x800 / 4; i++) ((uint32_t*)AR(A_PAL))[i] = rnd();
    make_stamp();
    make_font();
    for (int i = 0; i < 8; i++) rand_str((char*)AR(A_STR + 0x20 * i), 20);
    for (int i = 0; i < 8; i++) rand_str((char*)AR(A_STR + 0x100 + 0x10 * i), 12);
    rand_str((char*)AR(A_STR + 0x200), 12);
    // the heap
    memset(AR(A_HEAP), 0xcd, 0x1000);
    g_heap = AR(A_HEAP);
    // the car info / engine / records / world / deity
    uint8_t* info = AR(A_MISC);
    for (int i = 0; i < 0x400; i += 4) setf((uint32_t)(uintptr_t)(info + i), range(-50.0f, 50.0f));
    rand_str((char*)info + 5, 10);
    rand_str((char*)info + 0x12, 10);
    uint8_t* eng = AR(A_MISC + 0x400);
    for (int i = 0; i < 0x200; i += 4) setf((uint32_t)(uintptr_t)(eng + i), range(-3.0f, 3.0f));
    *(uint8_t**)(info + 0x134) = eng;
    *(int32_t*)(eng + 0x48) = irange(-2, 6);
    setf((uint32_t)(uintptr_t)(eng + 0x184), range(250.0f, 400.0f));
    setf((uint32_t)(uintptr_t)(eng + 0x188), range(250.0f, 400.0f));
    for (int t = 0; t < 4; t++) setf((uint32_t)(uintptr_t)(eng + 0xa8 + 0x38 * t), range(250.0f, 400.0f));
    if (chance(5)) setf((uint32_t)(uintptr_t)(eng + 0x9c + 0x38 * (rnd() % 4)), wildf());
    uint8_t* deity = AR(A_MISC + 0x800);
    void** vt = (void**)AR(A_MISC + 0x900);
    *(void***)deity = vt;
    for (int i = 0; i < 32; i++) vt[i] = (void*)0x00000bad;
    vt[0x40 / 4] = (void*)&stub_deity_abort;
    vt[0x48 / 4] = (void*)&stub_deity_laps;
    set32(X_DEITY, (uint32_t)(uintptr_t)deity);
    uint8_t* world = AR(A_MISC + 0x2000);
    rand_str((char*)world + 8, 16);
    world[0xcd1] = (uint8_t)chance(50);
    // the image's statics
    G8(X_SCREEN_FORMAT) = (uint8_t)(chance(80) ? 4 : chance(50) ? 3 : k_formats[rnd() % 4]);
    set32(X_SCR_W, chance(80) ? 96 : irange(-3, 700));
    set32(X_SCR_H, chance(80) ? 80 : irange(-3, 500));
    for (uint32_t a : k_colors) set32(a, rnd());
    for (int i = 0; i < 8; i++) set32(0x509920 + 4 * i, chance(80) ? (uint32_t)(uintptr_t)AR(A_MISC + 0x3000 + 0x40 * i) : 0);
    set32(0x509984, (uint32_t)(uintptr_t)AR(A_MISC + 0x3000));
    set32(0x5099c4, rnd() % 40);
    for (uint32_t a = 0x5099b8; a < 0x5099c4; a += 4) setf(a, range(-100.0f, 100.0f));
    G8(0x4eb674) = (uint8_t)chance(50);
    set32(0x4eb670, rnd() % 5);
    for (int i = 0; i < 8; i++) rand_str((char*)(uintptr_t)(0x5219e0 + 0x50 * i), 30);
    G8(0x5099a4) = (uint8_t)chance(70);
    setf(0x5099a8, range(-100.0f, 100.0f)); setf(0x5099ac, range(-100.0f, 100.0f)); setf(0x5099a0, range(-10.0f, 10.0f));
    for (uint32_t a : {0x509910u, 0x509914u, 0x509978u, 0x50997cu, 0x509960u, 0x509964u}) setf(a, range(-100.0f, 100.0f));
    set32(0x520ae8, rnd() % 1000); set32(0x520aec, rnd() % 1000); set32(0x4ec9dc, rnd() % 100); set32(0x521084, rnd() % 100);
    {   // a rotation (with noise) for the camera dashboard
        const float a = range(-3.1f, 3.1f), b = range(-1.5f, 1.5f);
        const float m[9] = {cosf(a), 0, -sinf(a), sinf(a) * sinf(b), cosf(b), cosf(a) * sinf(b), sinf(a) * cosf(b), -sinf(b), cosf(a) * cosf(b)};
        for (int i = 0; i < 9; i++) setf(0x520c70 + 4 * i, m[i] + (chance(20) ? range(-0.01f, 0.01f) : 0.0f));
        if (chance(10)) for (int i = 0; i < 9; i++) setf(0x520c70 + 4 * i, i % 4 == 0 ? 1.0f : 0.0f);
    }
    for (uint32_t a : {0x520c94u, 0x520c98u, 0x520c9cu, 0x55334cu, 0x553350u, 0x553354u}) setf(a, range(-1000.0f, 1000.0f));
    G8(0x4ecce4) = (uint8_t)chance(50); G8(0x4ecce8) = (uint8_t)chance(50); G8(0x520ad0) = 0;
    set32(X_CANVAS, (uint32_t)(uintptr_t)CV(rnd() % 8));
    // the script
    memset(&g_script, 0, sizeof g_script);
    g_script.alloc_fail = chance(5) ? rnd() : 0;
    g_script.res_kind = chance(90) ? 1 + (int)(rnd() % 3) : 0;
    g_script.res_version = chance(90) ? 3 : rnd() % 5;
    g_script.res_fresh = (uint8_t)chance(60);
    g_script.file_open = chance(90) ? 0x77 : 0;
    g_script.read_fail = chance(10) ? rnd() : 0;
    g_script.focus = chance(90) ? (int)(rnd() % 8) : (int)(rnd() % 4);
    for (int i = 0; i < 40; i++) {
        g_script.game_state[i] = chance(60) ? 3 : 2;
        g_script.key_hit[i] = (uint8_t)chance(30);
        g_script.key_get[i] = (uint16_t)(chance(40) ? 0x1b : chance(40) ? 0x31 : 'a');
        g_script.grab[i] = (uint8_t)chance(85);
    }
    g_script.key_hit[20] = 1;
    g_script.key_get[irange(0, 12)] = 0x1b;
    g_script.mouse_ok = (uint8_t)chance(60);
    g_script.mouse_ev[0] = 6; g_script.mouse_ev[1] = irange(0, 640); g_script.mouse_ev[2] = irange(0, 480); g_script.mouse_ev[3] = rnd() % 4;
    g_script.terrain = chance(95) ? range(-10.0f, 10.0f) : wildf();
    for (int i = 0; i < 8; i++) {
        g_script.proj_ok[i] = (uint8_t)chance(80);
        for (int k = 0; k < 3; k++) g_script.proj[i][k] = chance(97) ? range(-20.0f, 120.0f) : wildf();
    }
    g_script.elapsed = range(0.0f, 500.0f);
    g_script.laps = irange(0, 30);
    g_script.abort_race = (uint8_t)chance(10);
    g_script.opt_bool = (uint8_t)(chance(50) ? 0 : chance(80) ? 1 : rnd());
    rand_str(g_script.opt_str[0], 10);
    strcpy(g_script.opt_str[1], chance(50) ? "0101010110101010" : "11");
    g_script.dialog_result = chance(80) ? irange(0, 3) : -1;
    g_script.dialog_toggle = rnd();
    g_script.lap_null = (uint8_t)chance(20);
}

// ---- the functions under test ---------------------------------------------------------------------------------------
#define KINDS(X)                                                                                                        \
    X(gfx_begin_n, 2) X(gfx_end_n, 2) X(gx_reset_n, 2) X(gxBuildCanvas_n, 5) X(gxAllocCanvas3_n, 5) X(gxAllocCanvas4_n, 5) \
    X(gxFreeCanvas_n, 5) X(gxCanvasLoad_n, 10) X(gxCanvasUnload_n, 3) X(gxCanvasRead_n, 15) X(gxCanvasWrite_n, 8)       \
    X(gxCanvasGet_n, 8) X(gxCanvasForget_n, 2) X(gxSetClip_n, 10) X(gxRestoreClip_n, 5) X(gxSetCanvas_n, 5)            \
    X(adjust_to_clip_n, 20) X(in_clip_n, 10) X(shrink_rect_n, 20) X(gxClear_n, 15) X(gxClearNoAlpha_n, 8)              \
    X(clear_noalpha_16_n, 2) X(clear_noalpha_32_n, 8) X(calc_address_32_n, 3) X(clear_16_n, 15) X(calc_address_16_n, 3) \
    X(clear_32_n, 8) X(clear_32_32_n, 8) X(gxPoint_n, 15) X(gxXORPoint_n, 10) X(point_16_n, 10) X(point_32_n, 10)      \
    X(gxGetPixel_n, 10) X(read_point_16_n, 10) X(read_point_32_n, 10) X(xor_point_16_n, 5) X(xor_point_32_n, 5)          \
    X(gxRect_n, 20) X(gxTriangle_n, 40) X(fill_line_n, 30) X(rect_16_n, 20) X(rect_32_n, 15) X(rect_4444_n, 15)          \
    X(gxCopy_n, 30) X(gxAlphaBlit_n, 30) X(copy_16_to_16_n, 40) X(copy_32_to_32_n, 20) X(copy_16_to_32_n, 20)            \
    X(copy_32_to_16_n, 20) X(gxCut_n, 10) X(gxPaste_n, 10) X(gxPasteAlpha_n, 10) X(gxPasteDouble_n, 20)                  \
    X(gfxReduceTo555_n, 10) X(gxPasteZoom_n, 25) X(paste_zoom_16_16_n, 30) X(get_cvt_fn_n, 5) X(paste_zoom_32_16_n, 25) \
    X(paste_zoom_32_32_n, 25) X(alpha_16_to_16_n, 30) X(alpha_32_to_16_n, 20) X(alpha_32_to_32_n, 20) X(gxLine_n, 40)    \
    X(gxBrushLine_n, 20) X(gxXORLine_n, 30) X(gxCircle_n, 20) X(gxMirror_n, 15) X(gxFlip_n, 15) X(gxRotate90_n, 10)      \
    X(rotate_90_16_n, 10) X(point_16_16_n, 5) X(get_point_16_n, 5) X(rotate_90_32_n, 10) X(point_32_32_n, 5)             \
    X(get_point_32_n, 5) X(gxGetStamp_n, 10) X(gxForgetStamp_n, 2) X(gxDrawStamp_n, 40) X(gxMakeBrush_n, 20)             \
    X(gxStampHitTest_n, 5) X(gxStampWidth_n, 2) X(gxStampHeight_n, 2) X(gxStampCount_n, 2) X(gxStampHotSpot_n, 5)       \
    X(gxStampGetUserData_n, 3) X(gxPaletteCreate_n, 2) X(gxPaletteDestroy_n, 2) X(gxPaletteSetColor_n, 3)               \
    X(gxPaletteGetColor_n, 3) X(gxPaletteMakeGradient_n, 15) X(gxText_n, 15) X(gxTextCentered_n, 10)                     \
    X(gxTextWidth_n, 3) X(gxTextHeight_n, 1) X(draw_char_n, 10) X(gxFontGet_n, 10) X(gxFontForget_n, 2)                  \
    X(gxFontStringWidth_n, 5) X(gxFontStringWidthN_n, 5) X(gxFontMaxCharWidth_n, 2) X(gxFontAscent_n, 2)                \
    X(gxFontDescent_n, 2) X(gxFontHeight_n, 2) X(gxFontPrintf_n, 25) X(gxFontPrintN_n, 10) X(pal_build_system_n, 3)     \
    X(gxPaletteGrab_n, 2) X(convert_palette_n, 8) X(gxPaletteConvert565ToScreen_n, 5) X(gxPaletteConvert565_n, 5)       \
    X(GraphDrawLinePlot_n, 20) X(GraphDrawBoundingBox_n, 5) X(GraphDrawAxes_n, 20) X(trim_zeros_n, 10)                   \
    X(get_graph_coords_n, 20) X(gxReadBMP_n, 5) X(gxWriteBMP_n, 5) X(bmp_read_n, 15) X(bmp_write_n, 10)                  \
    X(AIDashboardDraw_n, 10) X(draw_status_n, 15) X(draw_focus_iline_n, 3) X(draw_mouse_cursor_n, 5) X(draw_target_n, 8) \
    X(AIDisplayLearnMode_n, 8) X(learn_draw_n, 3) X(clear_screen_n, 3) X(learn_dialog_n, 8)                              \
    X(CameraDashboardDraw_n, 15) X(PhysicsDashboardDraw_n, 15) X(draw_tire_dash_n, 20)
#define K_ENUM(fn, w) K_##fn,
enum Kind { KINDS(K_ENUM) N_KINDS };
#define K_NAME(fn, w) #fn,
static const char* const kind_names[N_KINDS] = {KINDS(K_NAME)};
#define K_W(fn, w) w,
static const int kind_weights[N_KINDS] = {KINDS(K_W)};

struct Args {
    gxCanvas* c; gxCanvas* d;
    int32_t i[8];
    uint32_t u[4];
    float f[4];
    char* s;
    void* p;
    int32_t* pi[6];
    uint32_t va[16];
};
static Args g_args;
static int32_t coord() {
    const int k = rnd() % 20;
    if (k < 15) return irange(-12, 105);
    if (k < 19) return irange(-60, 160);
    return irange(-1500, 1500);
}
static gxCanvas* canvas_fmt(int8_t fmt) {                      // a canvas of this format (remade)
    const int i = rnd() % 8;
    make_canvas(i, fmt);
    return CV(i);
}
static void set_cur(gxCanvas* c) { set32(X_CANVAS, (uint32_t)(uintptr_t)c); }
static gxCanvas* cur() { return GP(gxCanvas, X_CANVAS); }

// two canvases of these formats (0: random); the same one (overlapping blits) where the formats allow, sometimes
static void pair(int8_t fs, int8_t fd) {
    const int i = rnd() % 8;
    int j = rnd() % 8;
    make_canvas(i, fs);
    const bool same = (fs == fd || !fd) && chance(25);
    if (same) j = i;
    else {
        while (j == i) j = rnd() % 8;
        make_canvas(j, fd);
    }
    g_args.c = CV(i);
    g_args.d = CV(j);
}
static void blit_args() {                                       // sx0 sy0 sx1 sy1 dx dy
    Args& g = g_args;
    const int k = rnd() % 10;
    if (k < 6) {
        g.i[0] = irange(-8, g.c->w); g.i[1] = irange(-8, g.c->h);
        g.i[2] = g.i[0] + irange(-2, g.c->w + 8); g.i[3] = g.i[1] + irange(-2, g.c->h + 8);
    } else {
        g.i[0] = coord(); g.i[1] = coord(); g.i[2] = coord(); g.i[3] = coord();
    }
    g.i[4] = irange(-40, g.d->w + 10);
    g.i[5] = irange(-40, g.d->h + 10);
}
static void clip_src_reads() {                                  // (the zoom reads stay inside the arena)
    for (int i = 0; i < 4; i++) {
        float& s = g_args.f[i];
        if (!(s >= 0.25f && s <= 8.0f)) s = 1.0f;
    }
}

static void random_args(Kind k) {
    Args& g = g_args;
    memset(&g, 0, sizeof g);
    g.c = CV(rnd() % 8);
    g.d = CV(rnd() % 8);
    for (int i = 0; i < 8; i++) g.i[i] = coord();
    for (int i = 0; i < 4; i++) g.u[i] = rnd();
    for (int i = 0; i < 4; i++) g.f[i] = chance(20) ? (float)irange(1, 4) * 0.5f : range(0.25f, 5.0f);
    g.s = (char*)AR(A_ARGS + 0x800);
    rand_str(g.s, 40);
    g.p = AR(A_PAL);
    int32_t* ints = (int32_t*)AR(A_ARGS);
    for (int i = 0; i < 6; i++) g.pi[i] = ints + (chance(20) ? rnd() % 6 : i);    // sometimes aliased
    for (int i = 0; i < 16; i++) g.va[i] = rnd() % 1000;
    switch (k) {
    case K_gxAllocCanvas3_n: case K_gxAllocCanvas4_n:
        g.i[0] = irange(0, 64); g.i[1] = irange(0, 64); g.i[2] = k_formats[rnd() % 4]; break;
    case K_gxBuildCanvas_n: g.i[2] = irange(0, 100); g.i[3] = irange(0, 100); g.i[4] = irange(-4, 400); break;
    case K_gxCanvasRead_n: {
        uint8_t* f = AR(A_FILE);
        uint32_t hdr[3] = {0x52455330, 0x43414e56, 1};
        if (chance(10)) hdr[rnd() % 3] ^= 1u << (rnd() % 32);
        memcpy(f, hdr, 12);
        uint32_t tag[2] = {rnd(), 0x4d474921};
        if (chance(10)) tag[1] = rnd();
        memcpy(f + 12, tag, 8);
        gxCanvas fc = *g.c;
        if (chance(15)) ((uint8_t*)&fc)[rnd() % 2] ^= 1;
        if (chance(10)) fc.w++;
        if (chance(10)) fc.h--;
        if (chance(10)) fc.pitch += 2;
        memcpy(f + 20, &fc, 0x24);
        for (int i = 56; i < 0x10000; i++) f[i] = (uint8_t)rnd();
        break;
    }
    case K_gxSetClip_n: g.i[0] = irange(-5, 100); g.i[1] = irange(-5, 100); g.i[2] = irange(-5, 110); g.i[3] = irange(-5, 110); break;
    case K_clear_noalpha_32_n: case K_clear_32_n: case K_clear_32_32_n: case K_point_32_n: case K_read_point_32_n:
    case K_xor_point_32_n: case K_rect_32_n: case K_point_32_32_n:
        set_cur(canvas_fmt(5)); break;
    case K_clear_16_n: case K_point_16_n: case K_read_point_16_n: case K_xor_point_16_n: case K_rect_16_n: case K_rect_4444_n:
    case K_point_16_16_n: case K_gxDrawStamp_n:
        set_cur(canvas_fmt(fmt16())); break;
    case K_fill_line_n: g.p = AR(A_TABLES); break;
    case K_gxTriangle_n:
        if (chance(30)) { g.i[4] = g.i[0] + (g.i[2] - g.i[0]) * 2; g.i[5] = g.i[1] + (g.i[3] - g.i[1]) * 2; }   // collinear
        break;
    case K_gxCopy_n: case K_gxAlphaBlit_n: pair(0, 0); blit_args(); break;
    case K_copy_16_to_16_n: pair(chance(80) ? 4 : fmt16(), chance(50) ? 4 : fmt16()); blit_args(); break;
    case K_copy_32_to_32_n: case K_alpha_32_to_32_n: pair(5, 5); blit_args(); break;
    case K_copy_16_to_32_n: pair(chance(90) ? (chance(50) ? 3 : 4) : 1, 5); blit_args(); break;
    case K_copy_32_to_16_n: case K_alpha_32_to_16_n: pair(5, chance(90) ? (chance(50) ? 3 : 4) : 1); blit_args(); break;
    case K_alpha_16_to_16_n: pair(chance(80) ? 1 : fmt16(), chance(90) ? (chance(50) ? 3 : 4) : 1); blit_args(); break;
    case K_gxCut_n: case K_gxPaste_n: case K_gxPasteAlpha_n: g.i[0] = irange(-40, 110); g.i[1] = irange(-40, 110); break;
    case K_gxPasteDouble_n: {
        pair(fmt16(), fmt16());
        set_cur(g.d);
        g.i[0] = irange(-40, 110); g.i[1] = irange(-40, 110);
        break;
    }
    case K_gfxReduceTo555_n: { const int i = rnd() % 8; make_canvas(i, fmt16()); g.c = CV(i); break; }
    case K_gxPasteZoom_n: pair(0, 0); set_cur(g.d); clip_src_reads(); g.i[0] = irange(-60, 110); g.i[1] = irange(-60, 110); break;
    case K_paste_zoom_16_16_n: pair(fmt16(), fmt16()); set_cur(g.d); clip_src_reads(); g.i[0] = irange(-60, 110); g.i[1] = irange(-60, 110); break;
    case K_paste_zoom_32_16_n: pair(5, chance(95) ? (chance(50) ? 3 : 4) : 1); set_cur(g.d); clip_src_reads(); g.i[0] = irange(-60, 110); g.i[1] = irange(-60, 110); break;
    case K_paste_zoom_32_32_n: pair(5, 5); set_cur(g.d); clip_src_reads(); g.i[0] = irange(-60, 110); g.i[1] = irange(-60, 110); break;
    case K_get_cvt_fn_n: g.i[0] = chance(80) ? k_formats[rnd() % 4] : (int8_t)rnd(); g.i[1] = chance(80) ? k_formats[rnd() % 4] : (int8_t)rnd(); break;
    case K_gxBrushLine_n:
        g.d = CV(rnd() % 8);
        for (int i = 0; i < 4; i++) g.i[i] = irange(-20, 110);
        break;
    case K_gxCircle_n: g.i[2] = chance(90) ? irange(0, 60) : irange(-5, 200); break;
    case K_rotate_90_16_n: { const int i = rnd() % 8; make_canvas(i, fmt16()); g.c = CV(i); break; }
    case K_rotate_90_32_n: { const int i = rnd() % 8; make_canvas(i, 5); g.c = CV(i); break; }
    case K_gxMakeBrush_n: { const int i = rnd() % 8; make_canvas(i, chance(90) ? 5 : 0); g.c = CV(i); g.i[0] = irange(-1, 5); break; }
    default: break;
    }
    switch (k) {                                                // (after the formats are set)
    case K_gxDrawStamp_n: case K_gxStampHitTest_n: case K_gxStampWidth_n: case K_gxStampHeight_n: case K_gxStampCount_n:
    case K_gxStampHotSpot_n: case K_gxStampGetUserData_n:
        g.p = chance(95) ? AR(A_STAMP) : 0;
        g.i[2] = chance(80) ? irange(0, 3) : irange(-2, 6);
        g.i[3] = chance(90) ? irange(-10, 50) : coord();
        break;
    case K_gxPaletteMakeGradient_n: case K_gxPaletteSetColor_n: case K_gxPaletteGetColor_n:
        g.i[0] = irange(0, 255); break;
    case K_gxFontStringWidthN_n: g.i[0] = irange(-2, 50); break;
    case K_gxFontPrintf_n: case K_gxFontPrintN_n: {
        static const char* const fmts[] = {"%s", "%d:%d %s", "x=%x y=%u", "", "plain", "%c%c%c", "%5d|%-4d|%s"};
        char* f = (char*)AR(A_ARGS + 0xc00);
        strcpy(f, fmts[rnd() % 7]);
        g.va[0] = (uint32_t)(uintptr_t)g.s;
        if (strstr(f, "%d:%d")) { g.va[0] = rnd() % 100; g.va[1] = rnd() % 100; g.va[2] = (uint32_t)(uintptr_t)g.s; }
        if (strstr(f, "%5d")) g.va[2] = (uint32_t)(uintptr_t)g.s;
        if (strstr(f, "%c")) { g.va[0] = 'A' + rnd() % 20; g.va[1] = rnd() % 256; g.va[2] = ' '; }
        g.u[0] = chance(70) ? (1u << (rnd() % 7)) | (chance(30) ? 1u << (rnd() % 7) : 0) : rnd() % 0x80;
        g.i[2] = irange(0, 60);                                  // (n < 0 or >= 256 overruns the stack: FIX CANDIDATE)
        g.p = chance(50) ? AR(A_PAL) : 0;
        break;
    }
    case K_convert_palette_n: G8(X_SCREEN_FORMAT) = (uint8_t)(chance(90) ? (chance(50) ? 3 : 4) : 5); break;
    case K_gxPaletteConvert565ToScreen_n: G8(X_SCREEN_FORMAT) = (uint8_t)(chance(90) ? (chance(50) ? 3 : 4) : 5); break;
    case K_gxPaletteConvert565_n: g.i[0] = chance(90) ? (chance(50) ? 3 : 4) : (int8_t)rnd(); break;
    case K_gxPaletteGrab_n: g.i[0] = chance(70) ? -1 : (int16_t)rnd(); break;
    case K_GraphDrawLinePlot_n: case K_GraphDrawBoundingBox_n: case K_GraphDrawAxes_n: case K_get_graph_coords_n: {
        int32_t* gi = (int32_t*)AR(A_GRAPH);
        gi[0] = irange(-10, 40); gi[1] = irange(-10, 40); gi[2] = irange(20, 110); gi[3] = irange(20, 110);
        if (chance(20)) std::swap(gi[1], gi[3]);
        gi[4] = chance(10) ? irange(-2, 2) : irange(2, 40);
        float* gf = (float*)(gi + 5);
        gf[0] = range(-10.0f, 5.0f); gf[1] = chance(5) ? gf[0] : gf[0] + range(0.1f, 20.0f);
        gf[2] = range(-10.0f, 5.0f); gf[3] = chance(5) ? gf[2] : gf[2] + range(0.1f, 20.0f);
        if (chance(3)) gf[rnd() % 4] = wildf();
        *(uint32_t*)AR(A_GRAPH + 0x40) = rnd() % 8;
        g.p = chance(80) ? AR(A_GRAPH + 0x40) : 0;
        g.i[0] = irange(-2, 12); g.i[1] = irange(-2, 12);
        g.f[0] = range(-15.0f, 25.0f); g.f[1] = range(-15.0f, 25.0f);
        if (chance(5)) g.f[rnd() % 2] = wildf();
        break;
    }
    case K_trim_zeros_n: {
        static const char* const nums[] = {"12.500", "3.", "0.000", "100", "1.2e+05", ".000", "7.25", "-0.10", "10.0.0", "5.050"};
        strcpy(g.s, nums[rnd() % 10]);
        break;
    }
    case K_gxReadBMP_n: case K_bmp_read_n: {
        const int i = rnd() % 8;
        make_canvas(i, chance(90) ? (int8_t)irange(1, 4) : 5);
        g.c = CV(i);
        uint8_t* f = AR(A_FILE);
        const int w = chance(90) ? g.c->w : irange(1, 50), h = chance(90) ? g.c->h : irange(1, 50);
        uint8_t fh[14] = {'B', 'M'};
        if (chance(5)) fh[1] = 'X';
        const uint32_t size = 54 + (uint32_t)(w * h * 3) + (chance(20) ? rnd() % 16 : 0), off = 54;
        memcpy(fh + 2, &size, 4);
        memcpy(fh + 10, &off, 4);
        memcpy(f, fh, 14);
        int32_t ih[10] = {0x28, w, h, 0x180001, 0, 0, 0, 0, 0, 0};
        if (chance(5)) ih[3] = 0x100001;
        if (chance(5)) ih[4] = 1;
        memcpy(f + 14, ih, 0x28);
        for (int j = 54; j < 0x10000; j++) f[j] = (uint8_t)(chance(10) ? 0 : rnd());
        break;
    }
    case K_gxWriteBMP_n: case K_bmp_write_n: g.c = CV(rnd() % 8); break;
    case K_draw_tire_dash_n:
        g.i[0] = irange(0, 96); g.i[1] = irange(0, 80);
        g.f[0] = range(-2.0f, 2.0f); g.f[1] = range(-2.0f, 2.0f); g.f[2] = range(-0.5f, 2.0f); g.f[3] = range(250.0f, 400.0f);
        if (chance(5)) g.f[rnd() % 4] = wildf();
        break;
    case K_AIDisplayLearnMode_n: case K_learn_dialog_n: case K_clear_screen_n:
        for (int i = 0; i < 40; i++) g_script.grab[i] = i == 0 ? (uint8_t)chance(70) : 1;
        break;
    default: break;
    }
}

// ---- calling a kind: its footprint, the original, or the rewrite -----------------------------------------------------
enum Mode { M_FP, M_ORIG, M_NEW };
static Mode g_mode;
static Footprint* g_fpp;
template <typename F, typename... X> static uint64_t inv(F f, X... x) {
    using R = decltype(f(x...));
    if constexpr (std::is_void_v<R>) {
        f(x...);
        return 0;
    } else {
        R r = f(x...);
        uint64_t u = 0;
        memcpy(&u, &r, sizeof(R) < 8 ? sizeof(R) : 8);
        return u;
    }
}
#define RUN(fn, ...) (g_mode == M_FP ? (fpof_##fn(*g_fpp, __VA_ARGS__), 0ull) \
                      : g_mode == M_ORIG ? inv((decltype(&fn))addr_##fn, __VA_ARGS__) : inv(&fn, __VA_ARGS__))
#define RUN0(fn) (g_mode == M_FP ? (fpof_##fn(*g_fpp), 0ull) : g_mode == M_ORIG ? inv((decltype(&fn))addr_##fn) : inv(&fn))

static uint64_t call_kind(Kind k) {
    Args& g = g_args;
    gxCanvas* c = g.c;
    gxCanvas* d = g.d;
    const int32_t* i = g.i;
    const uint32_t* u = g.u;
    const float* f = g.f;
    int32_t** pi = g.pi;
    const uint32_t* va = g.va;
    const gxStamp* st_ = (const gxStamp*)g.p;
    FontHdr* font = (u[3] % 10) ? (FontHdr*)AR(A_FONT) : 0;
    const GraphInfo* gi = (const GraphInfo*)AR(A_GRAPH);
    int32_t* tl = (int32_t*)AR(A_TABLES);
    const void* pal = (u[1] & 1) ? AR(A_PAL) : 0;
    switch (k) {
    case K_gfx_begin_n: return RUN0(gfx_begin_n);
    case K_gfx_end_n: return RUN0(gfx_end_n);
    case K_gx_reset_n: return RUN0(gx_reset_n);
    case K_gxBuildCanvas_n: return RUN(gxBuildCanvas_n, c, (int8_t)(u[0] % 6), (char*)d->pixels, i[2], i[3], i[4]);
    case K_gxAllocCanvas3_n: return RUN(gxAllocCanvas3_n, c, i[0], i[1]);
    case K_gxAllocCanvas4_n: return RUN(gxAllocCanvas4_n, c, i[0], i[1], (int8_t)i[2]);
    case K_gxFreeCanvas_n: return RUN(gxFreeCanvas_n, c);
    case K_gxCanvasLoad_n: return RUN(gxCanvasLoad_n, c, (const char*)g.s);
    case K_gxCanvasUnload_n: return RUN(gxCanvasUnload_n, c);
    case K_gxCanvasRead_n: return RUN(gxCanvasRead_n, c, (const char*)g.s);
    case K_gxCanvasWrite_n: return RUN(gxCanvasWrite_n, (const gxCanvas*)c, (const char*)g.s);
    case K_gxCanvasGet_n: return RUN(gxCanvasGet_n, (const char*)g.s);
    case K_gxCanvasForget_n: return RUN(gxCanvasForget_n, c);
    case K_gxSetClip_n: return RUN(gxSetClip_n, c, i[0], i[1], i[2], i[3]);
    case K_gxRestoreClip_n: return RUN(gxRestoreClip_n, c);
    case K_gxSetCanvas_n: return RUN(gxSetCanvas_n, c);
    case K_adjust_to_clip_n: return RUN(adjust_to_clip_n, c, pi[0], pi[1], pi[2], pi[3]);
    case K_in_clip_n: return RUN(in_clip_n, c, i[0], i[1]);
    case K_shrink_rect_n: return RUN(shrink_rect_n, c, *pi[0], *pi[1], *pi[2], *pi[3], *pi[4], *pi[5]);
    case K_gxClear_n: return RUN(gxClear_n, u[0]);
    case K_gxClearNoAlpha_n: return RUN(gxClearNoAlpha_n, u[0]);
    case K_clear_noalpha_16_n: return RUN(clear_noalpha_16_n, u[0]);
    case K_clear_noalpha_32_n: return RUN(clear_noalpha_32_n, u[0]);
    case K_calc_address_32_n: return RUN(calc_address_32_n, c, i[0], i[1]);
    case K_clear_16_n: return RUN(clear_16_n, u[0]);
    case K_calc_address_16_n: return RUN(calc_address_16_n, c, i[0], i[1]);
    case K_clear_32_n: return RUN(clear_32_n, u[0]);
    case K_clear_32_32_n: return RUN(clear_32_32_n, u[0]);
    case K_gxPoint_n: return RUN(gxPoint_n, i[0], i[1], u[0]);
    case K_gxXORPoint_n: return RUN(gxXORPoint_n, i[0], i[1]);
    case K_point_16_n: return RUN(point_16_n, i[0], i[1], u[0]);
    case K_point_32_n: return RUN(point_32_n, i[0], i[1], u[0]);
    case K_gxGetPixel_n: return RUN(gxGetPixel_n, i[0], i[1]);
    case K_read_point_16_n: return RUN(read_point_16_n, i[0], i[1]);
    case K_read_point_32_n: return RUN(read_point_32_n, i[0], i[1]);
    case K_xor_point_16_n: return RUN(xor_point_16_n, i[0], i[1]);
    case K_xor_point_32_n: return RUN(xor_point_32_n, i[0], i[1]);
    case K_gxRect_n: return RUN(gxRect_n, i[0], i[1], i[2], i[3], u[0]);
    case K_gxTriangle_n: return RUN(gxTriangle_n, i[0], i[1], i[2], i[3], i[4], i[5], u[0]);
    case K_fill_line_n: return RUN(fill_line_n, i[0], i[1], i[2], i[3], tl, tl + 0x100);
    case K_rect_16_n: return RUN(rect_16_n, i[0], i[1], i[2], i[3], u[0]);
    case K_rect_32_n: return RUN(rect_32_n, i[0], i[1], i[2], i[3], u[0]);
    case K_rect_4444_n: return RUN(rect_4444_n, i[0], i[1], i[2], i[3], u[0]);
    case K_gxCopy_n: return RUN(gxCopy_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_gxAlphaBlit_n: return RUN(gxAlphaBlit_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_copy_16_to_16_n: return RUN(copy_16_to_16_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_copy_32_to_32_n: return RUN(copy_32_to_32_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_copy_16_to_32_n: return RUN(copy_16_to_32_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_copy_32_to_16_n: return RUN(copy_32_to_16_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_gxCut_n: return RUN(gxCut_n, c, i[0], i[1]);
    case K_gxPaste_n: return RUN(gxPaste_n, c, i[0], i[1]);
    case K_gxPasteAlpha_n: return RUN(gxPasteAlpha_n, c, i[0], i[1]);
    case K_gxPasteDouble_n: return RUN(gxPasteDouble_n, c, i[0], i[1]);
    case K_gfxReduceTo555_n: return RUN(gfxReduceTo555_n, *c);
    case K_gxPasteZoom_n: return RUN(gxPasteZoom_n, c, i[0], i[1], f[0], f[1]);
    case K_paste_zoom_16_16_n: return RUN(paste_zoom_16_16_n, c, i[0], i[1], f[0], f[1]);
    case K_get_cvt_fn_n: return RUN(get_cvt_fn_n, (int8_t)i[0], (int8_t)i[1]);
    case K_paste_zoom_32_16_n: return RUN(paste_zoom_32_16_n, c, i[0], i[1], f[0], f[1]);
    case K_paste_zoom_32_32_n: return RUN(paste_zoom_32_32_n, c, i[0], i[1], f[0], f[1]);
    case K_alpha_16_to_16_n: return RUN(alpha_16_to_16_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_alpha_32_to_16_n: return RUN(alpha_32_to_16_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_alpha_32_to_32_n: return RUN(alpha_32_to_32_n, c, i[0], i[1], i[2], i[3], d, i[4], i[5]);
    case K_gxLine_n: return RUN(gxLine_n, i[0], i[1], i[2], i[3], u[0]);
    case K_gxBrushLine_n: return RUN(gxBrushLine_n, i[0], i[1], i[2], i[3], d);
    case K_gxXORLine_n: return RUN(gxXORLine_n, i[0], i[1], i[2], i[3]);
    case K_gxCircle_n: return RUN(gxCircle_n, i[0], i[1], i[2], u[0]);
    case K_gxMirror_n: return RUN(gxMirror_n, c);
    case K_gxFlip_n: return RUN(gxFlip_n, c);
    case K_gxRotate90_n: return RUN(gxRotate90_n, c);
    case K_rotate_90_16_n: return RUN(rotate_90_16_n, c);
    case K_point_16_16_n: return RUN(point_16_16_n, i[0], i[1], (uint16_t)u[0]);
    case K_get_point_16_n: return RUN(get_point_16_n, c, i[0], i[1]);
    case K_rotate_90_32_n: return RUN(rotate_90_32_n, c);
    case K_point_32_32_n: return RUN(point_32_32_n, i[0], i[1], u[0]);
    case K_get_point_32_n: return RUN(get_point_32_n, c, i[0], i[1]);
    case K_gxGetStamp_n: return RUN(gxGetStamp_n, (const char*)g.s);
    case K_gxForgetStamp_n: return RUN(gxForgetStamp_n, (gxStamp*)st_);
    case K_gxDrawStamp_n: return RUN(gxDrawStamp_n, st_, i[0], i[1], i[2], pal);
    case K_gxMakeBrush_n: return RUN(gxMakeBrush_n, c, (const gxStamp*)AR(A_STAMP), u[0], i[0]);
    case K_gxStampHitTest_n: return RUN(gxStampHitTest_n, st_, i[3], i[4]);
    case K_gxStampWidth_n: return RUN(gxStampWidth_n, st_);
    case K_gxStampHeight_n: return RUN(gxStampHeight_n, st_);
    case K_gxStampCount_n: return RUN(gxStampCount_n, st_);
    case K_gxStampHotSpot_n: return RUN(gxStampHotSpot_n, st_, pi[0], pi[1]);
    case K_gxStampGetUserData_n: return RUN(gxStampGetUserData_n, st_, i[2]);
    case K_gxPaletteCreate_n: return RUN0(gxPaletteCreate_n);
    case K_gxPaletteDestroy_n: return RUN(gxPaletteDestroy_n, (void*)AR(A_PAL));
    case K_gxPaletteSetColor_n: return RUN(gxPaletteSetColor_n, (void*)AR(A_PAL), i[0], u[0]);
    case K_gxPaletteGetColor_n: return RUN(gxPaletteGetColor_n, (const void*)AR(A_PAL), i[0]);
    case K_gxPaletteMakeGradient_n: return RUN(gxPaletteMakeGradient_n, (void*)AR(A_PAL), u[0], u[1]);
    case K_gxText_n: return RUN(gxText_n, i[0], i[1], (const char*)g.s, u[0]);
    case K_gxTextCentered_n: return RUN(gxTextCentered_n, i[0], i[1], (const char*)g.s, u[0]);
    case K_gxTextWidth_n: return RUN(gxTextWidth_n, (const char*)g.s);
    case K_gxTextHeight_n: return RUN0(gxTextHeight_n);
    case K_draw_char_n: return RUN(draw_char_n, i[0], i[1], (int32_t)(0x20 + u[1] % 0x60) | (int32_t)(u[2] & 0xffffff00u), u[0]);
    case K_gxFontGet_n: return RUN(gxFontGet_n, (const char*)g.s);
    case K_gxFontForget_n: return RUN(gxFontForget_n, font);
    case K_gxFontStringWidth_n: return RUN(gxFontStringWidth_n, (const FontHdr*)font, (const char*)g.s);
    case K_gxFontStringWidthN_n: return RUN(gxFontStringWidthN_n, (const FontHdr*)font, (const char*)g.s, i[0]);
    case K_gxFontMaxCharWidth_n: return RUN(gxFontMaxCharWidth_n, (const FontHdr*)font);
    case K_gxFontAscent_n: return RUN(gxFontAscent_n, (const FontHdr*)font);
    case K_gxFontDescent_n: return RUN(gxFontDescent_n, (const FontHdr*)font);
    case K_gxFontHeight_n: return RUN(gxFontHeight_n, (const FontHdr*)font);
    case K_gxFontPrintf_n: return RUN(gxFontPrintf_n, (const FontHdr*)font, (const void*)g.p, u[0], i[0], i[1], (const char*)AR(A_ARGS + 0xc00),
                                      va[0], va[1], va[2], va[3], va[4], va[5], va[6], va[7], va[8], va[9], va[10], va[11], va[12],
                                      va[13], va[14], va[15]);
    case K_gxFontPrintN_n: return RUN(gxFontPrintN_n, (const FontHdr*)font, (const void*)g.p, u[0], i[0], i[1], (const char*)g.s, i[2]);
    case K_pal_build_system_n: return RUN0(pal_build_system_n);
    case K_gxPaletteGrab_n: return RUN(gxPaletteGrab_n, (int16_t)i[0]);
    case K_convert_palette_n: return RUN(convert_palette_n, (const uint8_t*)AR(A_ARGS + 0x100), (uint16_t*)AR(A_PAL));
    case K_gxPaletteConvert565ToScreen_n: return RUN(gxPaletteConvert565ToScreen_n, (uint16_t*)AR(A_PAL));
    case K_gxPaletteConvert565_n: return RUN(gxPaletteConvert565_n, (uint16_t*)AR(A_PAL), (int8_t)i[0]);
    case K_GraphDrawLinePlot_n: return RUN(GraphDrawLinePlot_n, gi, (PlotFn_t)(void*)&plot_fn, g.p, u[0]);
    case K_GraphDrawBoundingBox_n: return RUN(GraphDrawBoundingBox_n, gi, u[0]);
    case K_GraphDrawAxes_n: return RUN(GraphDrawAxes_n, gi, i[0], i[1], u[0]);
    case K_trim_zeros_n: return RUN(trim_zeros_n, g.s);
    case K_get_graph_coords_n: return RUN(get_graph_coords_n, gi, f[0], f[1], pi[0], pi[1]);
    case K_gxReadBMP_n: return RUN(gxReadBMP_n, (const char*)g.s, c);
    case K_gxWriteBMP_n: return RUN(gxWriteBMP_n, (const char*)g.s, c);
    case K_bmp_read_n: return RUN(bmp_read_n, (const char*)g.s, c);
    case K_bmp_write_n: return RUN(bmp_write_n, (const char*)g.s, c);
    case K_AIDashboardDraw_n: return RUN(AIDashboardDraw_n, c);
    case K_draw_status_n: return RUN0(draw_status_n);
    case K_draw_focus_iline_n: return RUN0(draw_focus_iline_n);
    case K_draw_mouse_cursor_n: return RUN0(draw_mouse_cursor_n);
    case K_draw_target_n: return RUN0(draw_target_n);
    case K_AIDisplayLearnMode_n: return RUN(AIDisplayLearnMode_n, (const uint8_t*)AR(A_MISC + 0x2000));
    case K_learn_draw_n: return RUN0(learn_draw_n);
    case K_clear_screen_n: return RUN0(clear_screen_n);
    case K_learn_dialog_n: return RUN0(learn_dialog_n);
    case K_CameraDashboardDraw_n: return RUN(CameraDashboardDraw_n, c);
    case K_PhysicsDashboardDraw_n: return RUN(PhysicsDashboardDraw_n, c);
    case K_draw_tire_dash_n: { Point2Df v = {f[0], f[1]}; return RUN(draw_tire_dash_n, i[0], i[1], v, f[2], f[3]); }
    default: return 0;
    }
}
// replay-only functions that leave the current canvas on their stack: masked (clear_screen declares it: stack_ptr)
static bool mask_canvas_ptr(Kind k) { return k == K_AIDisplayLearnMode_n || k == K_learn_dialog_n; }
static bool on_stack(uint32_t a) {
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    return a >= lo && a < hi;
}

// ---- running a pass ---------------------------------------------------------------------------------------------------
static uint32_t g_fault_eip, g_fault_code, g_fault_addr;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_addr = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static unsigned g_pc;
static bool g_unmask;
static int run_guarded(Kind k, Mode m, uint64_t* ret) {
    g_log.n = 0;
    g_heap = AR(A_HEAP);
    g_alloc_calls = g_read_calls = g_game_calls = g_hit_calls = g_get_calls = g_grab_calls = g_proj_calls = 0;
    g_file_pos = 0;
    g_mode = m;
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
        *ret = call_kind(k);
        __asm__ volatile("fwait" ::: VP_X87_CLOBBERS, "memory");
    }, fault_filter)) { fault = 1; }
#else
    __try {
        *ret = call_kind(k);
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
static bool in_footprint(const Footprint& fp, const uint8_t* p) {
    for (int i = 0; i < fp.n; i++)
        if (p >= (const uint8_t*)fp.r[i].p && p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}
static const char* where(uint32_t a, char* buf) {
    uint32_t base = (uint32_t)(uintptr_t)g_arena;
    if (a >= base && a < base + ARENA_BYTES) { sprintf(buf, "arena+0x%x", a - base); return buf; }
    sprintf(buf, "0x%08x", a);
    return buf;
}

// ---- the pure converters, exhaustively --------------------------------------------------------------------------------
static int g_pure_checks, g_pure_bad;
template <typename R, typename A> static void check_pure(const char* name, R(__cdecl* nw)(A), uint32_t orig, uint32_t a) {
    const R ro = ((R(__cdecl*)(A))orig)((A)a);
    const R rn = nw((A)a);
    g_pure_checks++;
    if (memcmp(&ro, &rn, sizeof(R))) {
        if (g_pure_bad++ < 20) printf("  PURE MISMATCH %s(%08x): original %08x rewrite %08x\n", name, a, (uint32_t)ro, (uint32_t)rn);
    }
}
static void pure_tests() {
    for (uint32_t v = 0; v < 0x10000; v++) {
        const uint32_t hi = rnd() & 0xffff0000u;
        check_pure("cvt_555_8888", cvt_555_8888_n, addr_cvt_555_8888_n, v | hi);
        check_pure("cvt_565_8888", cvt_565_8888_n, addr_cvt_565_8888_n, v | hi);
        check_pure("cvt_identity", cvt_identity_n, addr_cvt_identity_n, v);
        check_pure("cvt_565_555", cvt_565_555_n, addr_cvt_565_555_n, v);
        check_pure("cvt_555_565", cvt_555_565_n, addr_cvt_555_565_n, v);
        check_pure("cvt_4444_565", cvt_4444_565_n, addr_cvt_4444_565_n, v);
        check_pure("cvt_4444_555", cvt_4444_555_n, addr_cvt_4444_555_n, v);
    }
    for (int n = 0; n < 2000000; n++) {
        const uint32_t p = rnd();
        check_pure("cvt_8888_555", cvt_8888_555_n, addr_cvt_8888_555_n, p);
        check_pure("cvt_8888_565", cvt_8888_565_n, addr_cvt_8888_565_n, p);
    }
    for (int n = 0; n < 2000000; n++) {
        const uint16_t dv = (uint16_t)rnd();
        uint32_t sv = rnd();
        if (n % 8 == 0) sv &= 0x00ffffff;
        if (n % 8 == 1) sv |= 0xff000000;
        const uint16_t o5 = ((Blend_t)addr_alpha_blend_8888_to_555_n)(dv, sv), n5 = alpha_blend_8888_to_555_n(dv, sv);
        const uint16_t o6 = ((Blend_t)addr_alpha_blend_8888_to_565_n)(dv, sv), n6 = alpha_blend_8888_to_565_n(dv, sv);
        g_pure_checks += 2;
        if (o5 != n5 && g_pure_bad++ < 20) printf("  PURE MISMATCH alpha_blend_8888_to_555(%04x, %08x): %04x / %04x\n", dv, sv, o5, n5);
        if (o6 != n6 && g_pure_bad++ < 20) printf("  PURE MISMATCH alpha_blend_8888_to_565(%04x, %08x): %04x / %04x\n", dv, sv, o6, n6);
    }
    // get_cvt_fn: every (from, to) pair (the log compares its panics)
    for (int a = -128; a < 128; a++)
        for (int b = -128; b < 128; b++) {
            g_log.n = 0;
            const uint32_t ro = (uint32_t)(uintptr_t)((GetCvt_t)addr_get_cvt_fn_n)((int8_t)a, (int8_t)b);
            const uint32_t lo = g_log.n;
            g_log.n = 0;
            const uint32_t rn = (uint32_t)(uintptr_t)get_cvt_fn_n((int8_t)a, (int8_t)b);
            g_pure_checks++;
            if (ro != rn || lo != g_log.n) { if (g_pure_bad++ < 20) printf("  PURE MISMATCH get_cvt_fn(%d, %d): %08x / %08x\n", a, b, ro, rn); }
        }
}

// ---- main --------------------------------------------------------------------------------------------------------
enum : uint32_t { CMP_BYTES = A_PIX + 8 * 0x20000 };
#ifdef FIX_TESTS
#include "vrmod_image.h"
// gxTriangle / fill_line on a tall canvas: the fixed rewrite against vrmod's patched original (2048-row tables, both
// bounds), pixel for pixel, and against the stock original up to 1024 rows. A canvas 16 pixels wide (565), a triangle
// with corners off every edge; the canvas's buffer is compared whole.
static int g_fix_fail;
typedef void(__cdecl* Tri_t)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t);
static int tri_guarded(Tri_t f, const int32_t* v, uint32_t c) {
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
        f(v[0], v[1], v[2], v[3], v[4], v[5], c);
    }, fault_filter)) { return 1; }
#else
    __try {
        f(v[0], v[1], v[2], v[3], v[4], v[5], c);
    } __except (fault_filter(GetExceptionInformation())) { return 1; }
#endif
    return 0;
}
static void fix_tests() {
    enum { W = 16, HMAX = 3100 };
    const uint32_t bytes = W * 2 * HMAX;
    uint8_t* px = (uint8_t*)VirtualAlloc(0, bytes, MEM_COMMIT, PAGE_READWRITE);
    uint8_t* start = (uint8_t*)malloc(bytes);
    uint8_t* after_orig = (uint8_t*)malloc(bytes);
    static gxCanvas tall;
    const int heights[] = {600, 1024, 1025, 1080, 1100, 1536, 2047, 2048, 2049, 2100, 3000};
    int vs_vrmod = 0, vs_stock = 0, rows_past = 0;
    for (int hi = 0; hi < (int)(sizeof heights / sizeof heights[0]); hi++) {
        const int H = heights[hi];
        for (int t = 0; t < 400; t++) {
            tall.format = 4;
            tall.flags = 0x80;
            tall.pixels = px;
            tall.w = W;
            tall.h = H;
            tall.pitch = W * 2;
            tall.cx0 = 0; tall.cy0 = 0; tall.cx1 = W; tall.cy1 = H;
            set_cur(&tall);
            int32_t v[6];
            for (int k = 0; k < 3; k++) {
                v[2 * k] = irange(-8, W + 8);
                v[2 * k + 1] = chance(70) ? irange(H - 120, H + 40) : irange(-40, H + 40);   // mostly near the bottom
            }
            if (H > 1024) for (int k = 1; k < 6; k += 2) if (v[k] > 1024) rows_past++;
            const uint32_t c = rnd();
            for (uint32_t i = 0; i < bytes; i += 4) { const uint32_t r = rnd(); memcpy(px + i, &r, 4); }
            memcpy(start, px, bytes);
            g_log.n = 0;
            vrmod_apply();
            const int fo = tri_guarded((Tri_t)0x00450760, v, c);
            vrmod_unapply();
            memcpy(after_orig, px, bytes);
            memcpy(px, start, bytes);
            g_log.n = 0;
            const int fn = tri_guarded(&gxTriangle_n, v, c);
            vs_vrmod++;
            if (fo || fn || memcmp(after_orig, px, bytes)) {
                uint32_t k = 0;
                while (k < bytes && after_orig[k] == px[k]) k++;
                if (g_fix_fail++ < 10)
                    printf("  FIX gxTriangle, %d rows: (%d,%d) (%d,%d) (%d,%d): vrmod's original %s, the rewrite %s; first differing row %d\n",
                           H, v[0], v[1], v[2], v[3], v[4], v[5], fo ? "faulted" : "ran", fn ? "faulted" : "ran",
                           k < bytes ? (int)(k / (W * 2)) : -1);
                continue;
            }
            if (H <= 1024) {                               // the stock original survives: the same pixels
                memcpy(px, start, bytes);
                const int fs = tri_guarded((Tri_t)0x00450760, v, c);
                vs_stock++;
                if (fs || memcmp(after_orig, px, bytes)) {
                    if (g_fix_fail++ < 10) printf("  FIX gxTriangle, %d rows: the stock original %s\n", H, fs ? "faulted" : "differs");
                }
            }
        }
    }
    printf("fix: gxTriangle / fill_line on canvases 600 to 3000 rows: %d triangles against vrmod's tablefix + needlefix "
           "(%d corners past row 1024), %d also against the stock original\n", vs_vrmod, rows_past, vs_stock);
    VirtualFree(px, 0, MEM_RELEASE);
    free(start);
    free(after_orig);
}
#endif

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 50000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const char* only = getenv("VP_ONLY");
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_gx_2d.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(0x004d6850, (void*)&stub_crt_msgbox);        // __crtMessageBoxA
    ((void(__cdecl*)())0x004ce150)();                      // the CRT's floating point, as its start-up leaves it
    struct { uint32_t at; void* to; } stubs[] = {
        {0x004140e0, (void*)&stub_MemAlloc}, {0x00414300, (void*)&stub_MemFree}, {0x00414390, (void*)&stub_delete},
        {0x00415000, (void*)&stub_single}, {0x00415070, (void*)&stub_single}, {0x004112b0, (void*)&stub_panic},
        {0x00411150, (void*)&stub_report}, {0x004cf0a0, (void*)&stub_sprintf}, {0x004cf7c0, (void*)&stub_vsprintf},
        {0x00411780, (void*)&stub_FileOpen}, {0x004115f0, (void*)&stub_FileCreate}, {0x004118b0, (void*)&stub_FileReadExact},
        {0x00411a30, (void*)&stub_FileWrite}, {0x00411850, (void*)&stub_FileClose}, {0x00419fa0, (void*)&stub_ResourceGet},
        {0x0041a450, (void*)&stub_ResourceForget},
        {0x004626d0, (void*)&stub_WorldGetFocusCar}, {0x004144f0, (void*)&stub_MouseGetEvent}, {0x00414620, (void*)&stub_MouseCenter},
        {0x00465ae0, (void*)&stub_TerrainGetHeight}, {0x0044f200, (void*)&stub_mrProjectPoint}, {0x00421110, (void*)&stub_project},
        {0x00421c30, (void*)&stub_IdealLine_Draw}, {0x0042bd30, (void*)&stub_PhysicsSetSpeed}, {0x00414de0, (void*)&stub_TaskSleep},
        {0x0042bcf0, (void*)&stub_PhysicsUnpause}, {0x00471dd0, (void*)&stub_SoundMuteCars}, {0x0042bcc0, (void*)&stub_PhysicsPause},
        {0x0042bc00, (void*)&stub_PhysicsAbortRace}, {0x00412bf0, (void*)&stub_Win32Idle}, {0x00413c50, (void*)&stub_KeyClear},
        {0x0044e210, (void*)&stub_gxReleaseScreen}, {0x0044e180, (void*)&stub_gxFlip}, {0x0040a3d0, (void*)&stub_GetGameState},
        {0x00413cc0, (void*)&stub_KeyHit}, {0x00413c90, (void*)&stub_KeyGet}, {0x0044e1c0, (void*)&stub_gxGrabScreen},
        {0x00464490, (void*)&stub_CarMgrGetInfo}, {0x00462050, (void*)&stub_WorldGetElapsedTime},
        {0x0042bf30, (void*)&stub_PhysicsTimeString}, {0x0042a940, (void*)&stub_RecordGetLapResult},
        {0x00471470, (void*)&stub_OptionsGetBool}, {0x00471500, (void*)&stub_OptionsGetStr}, {0x00471560, (void*)&stub_OptionsSetStr},
        {0x004714c0, (void*)&stub_OptionsSetBool}, {0x00406810, (void*)&stub_GetTrackName},
        {0x004061a0, (void*)&stub_GetAIStrengthString}, {0x00478ff0, (void*)&stub_UIDoDialog},
        {0x004241c0, (void*)&stub_check_learning}, {0x0047f910, (void*)&stub_UIStyleWidth}, {0x0047fa40, (void*)&stub_UIStyleDraw},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    *(void**)0x005d7554 = (void*)&stub_CreateFileA;       // the game's import slots
    *(void**)0x005d753c = (void*)&stub_ReadFile;
    *(void**)0x005d7548 = (void*)&stub_CloseHandle;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_snap = (uint8_t*)malloc(CMP_BYTES);
    g_arena_after = (uint8_t*)malloc(CMP_BYTES);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);

    {
        unsigned cw;
        _controlfp_s(&cw, _PC_53, _MCW_PC);
        pure_tests();
        printf("pure converters: %d checks, %d differ\n", g_pure_checks, g_pure_bad);
    }

    static Footprint fp;
    int differ = 0, faults = 0, fault_both = 0, fp_bad = 0, replay_only = 0, unmasked_runs = 0;
    int per[N_KINDS] = {0}, per_bad[N_KINDS] = {0}, per_fault[N_KINDS] = {0};
    long long pixel_bytes_changed = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weights[i];
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), kk = 0;
        while (pick >= kind_weights[kk]) pick -= kind_weights[kk++];
        Kind kind = (Kind)kk;
        if (only) {
            kind = N_KINDS;
            for (int i = 0; i < N_KINDS; i++) if (!strcmp(kind_names[i], only)) kind = (Kind)i;
            if (kind == N_KINDS) { printf("no kind %s\n", only); return 2; }
        }
        if (getenv("VP_TRACE")) printf("world %d: %s\n", it, kind_names[kind]);
        setup_world();
        random_args(kind);
        g_pc = chance(50) ? _PC_53 : _PC_24;
        g_unmask = chance(30);
        if (g_unmask) unmasked_runs++;
        g_mask_bmp_header = kind == K_bmp_write_n || kind == K_gxWriteBMP_n;
        fp.n = 0;
        fp.replay_only = 0;
        fp.pure = false;
        g_fpp = &fp;
        g_stack_ptrs = 0;
        g_mode = M_FP;
        call_kind(kind);
        if (kind == K_clear_screen_n)            // a surface it locks itself: the emulation compares it at Unlock
            fp.add(AR(A_SCREEN), 96 * 80 * 2, "the locked page (compared at Unlock)");
        if (fp.replay_only) replay_only++;
        memcpy(g_arena_snap, g_arena, CMP_BYTES);
        memcpy(g_data_snap, DATA, DATA_BYTES);
        uint64_t ro = 0, rn = 0;
        const int fo = run_guarded(kind, M_ORIG, &ro);
        const uint32_t fo_code = g_fault_code, fo_eip = g_fault_eip;
        memcpy(g_arena_after, g_arena, CMP_BYTES);
        memcpy(g_data_after, DATA, DATA_BYTES);
        g_log_orig = g_log;
        memcpy(g_arena, g_arena_snap, CMP_BYTES);
        memcpy(DATA, g_data_snap, DATA_BYTES);
        const int fn = run_guarded(kind, M_NEW, &rn);
        per[kind]++;
        if (fo || fn) {
            faults++;
            per_fault[kind]++;
            if (fo && fn) { fault_both++; if (fault_both <= 5) printf("  (world %d, %s: both faulted, %08x at %08x / %08x)\n", it, kind_names[kind], fo_code, fo_eip, g_fault_eip); }
            if (fo != fn) {
                printf("  world %d (%s): %s / %s; fault %08x at %08x (original's %08x at %08x), address %08x\n", it, kind_names[kind],
                       fo ? "original faulted" : "original ran", fn ? "rewrite faulted" : "rewrite ran", g_fault_code, g_fault_eip,
                       fo_code, fo_eip, g_fault_addr);
                differ++; per_bad[kind]++;
            }
            continue;
        }
        if (mask_canvas_ptr(kind)) memcpy(DATA + (X_CANVAS - 0x4e1000), g_data_after + (X_CANVAS - 0x4e1000), 4);
        for (int s = 0; s < g_stack_ptrs; s++) {
            uint8_t* now = (uint8_t*)g_stack_ptr[s];
            uint8_t* was = g_data_after + (now - DATA);
            uint32_t a, b;
            memcpy(&a, was, 4);
            memcpy(&b, now, 4);
            if (a != b && on_stack(a) && on_stack(b)) memcpy(now, was, 4);
            else if (a != b) printf("  STACK POINTER world %d (%s): %08x / %08x\n", it, kind_names[kind], a, b);
        }
        bool same = true;
        char buf[64];
        for (uint32_t i = 0; i < CMP_BYTES; i += 4)
            if (memcmp(g_arena_after + i, g_arena + i, 4)) {
                uint32_t x, y; memcpy(&x, g_arena_after + i, 4); memcpy(&y, g_arena + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
                printf("    %s original %08x rewrite %08x\n", where((uint32_t)(uintptr_t)(g_arena + i), buf), x, y);
                same = false;
                break;
            }
        for (uint32_t i = 0; i < DATA_BYTES; i += 4)
            if (memcmp(g_data_after + i, DATA + i, 4)) {
                uint32_t x, y; memcpy(&x, g_data_after + i, 4); memcpy(&y, DATA + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
                printf("    %s original %08x rewrite %08x\n", where((uint32_t)(uintptr_t)(DATA + i), buf), x, y);
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
            printf("    (pc %s, exceptions %s)\n", g_pc == _PC_24 ? "24" : "53", g_unmask ? "unmasked" : "masked");
            if (differ >= 30) { printf("stopping after 30 differing worlds\n"); break; }
        }
        // the original's writes against the footprint
        for (uint32_t i = 0; i < CMP_BYTES; i += 64)
            if (memcmp(g_arena_after + i, g_arena_snap + i, 64))
                for (uint32_t j = i; j < i + 64; j++) if (g_arena_after[j] != g_arena_snap[j]) pixel_bytes_changed++;
        if (!fp.replay_only) {
            bool bad = false;
            for (uint32_t i = 0; i < CMP_BYTES && !bad; i += 64) {
                if (!memcmp(g_arena_after + i, g_arena_snap + i, 64)) continue;
                for (uint32_t j = i; j < i + 64 && !bad; j++)
                    if (g_arena_after[j] != g_arena_snap[j] && !in_footprint(fp, g_arena + j)) {
                        printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kind_names[kind], where((uint32_t)(uintptr_t)(g_arena + j), buf));
                        bad = true;
                    }
            }
            for (uint32_t i = 0; i < DATA_BYTES && !bad; i++)
                if (g_data_after[i] != g_data_snap[i] && !in_footprint(fp, DATA + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kind_names[kind], where(0x4e1000 + i, buf));
                    bad = true;
                }
            fp_bad += bad;
        }
    }
    printf("%d worlds (%d with overflow / divide-by-zero unmasked): %d differ, %d faulted (%d in both), %d changed bytes outside "
           "the footprint, %d replay-only; %lld bytes changed by the originals\n",
           iterations, unmasked_runs, differ, faults, fault_both, fp_bad, replay_only, pixel_bytes_changed);
    printf("per function (worlds / differing / faulted in both):\n");
    for (int i = 0; i < N_KINDS; i++) printf("  %-32s %6d / %d / %d\n", kind_names[i], per[i], per_bad[i], per_fault[i]);
#ifdef FIX_TESTS
    fix_tests();
    printf("fix tests: %d failures\n", g_fix_fail);
    if (g_fix_fail) return 1;
#endif
    return differ || fp_bad || g_pure_bad ? 1 : 0;
}
