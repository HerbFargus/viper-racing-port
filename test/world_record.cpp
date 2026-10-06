// world_record.cpp -- the race-record rewrites (hook/phys_record.cpp) against the originals, outside the game
// (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_record.cpp
//        /Fo%TEMP%\wr\ /Fe%TEMP%\wr\world_record.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_record.exe [worlds] [seed]
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS (or /DVP_TEST_FIXES) -- the rewrites as the game builds them, on
//   worlds whose names fit (so they must still match the originals), then the inputs that used to overrun: long track
//   names, a long user directory, stage records outside set_best_stages' table, a short name after a '.'.
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. Each world starts from the pristine .data and
// builds, in an arena: 16 CarMgrInfos (a flag byte, names of random lengths, now and then a lower-case or high
// first letter), the game options, the user directory, a World (track name, cars, laps, realism, race type, the
// sim flag), the three record lists (capacity cars / cars x laps / cars x laps x 3, sometimes prefilled with
// random records), a RecordMgr with its RecordFile (a plausible one: each race top ten filled from the top,
// worst first, plus random flags and now and then garbage), and a "disk": the records file FileRead hands out
// (sometimes foreign: wrong size or version) and FileWrite writes -- so the bytes a save writes are compared
// byte for byte with the rest of the world. Half the worlds start from the original RecordBegin instead.
//
// Then a random race history on the physics side -- laps, stages, race-overs, official and unofficial, for 1..8
// cars (sometimes a car past the count), times with ties, NaN, inf, negative, zero and denormals -- mixed with
// the queries, and the main thread's side: create_dnfs, CheckLaps, CheckRace, set_best_stages,
// RecordConsolidate, save, the board's SetMode / Clear / GetNthBest*, dump, the constructors, RecordMgr
// creation / destruction, RecordEnd. The game functions around them are patched with logging stubs whose
// results come from a scripted stream, replayed identically for the original and the rewrite: MemAlloc
// (a bump heap in the arena, rare failures), delete, CarMgrGetInfo, CarMgrCount, TimeGetTimeOfDay,
// WorldGameOptions, MultiEnabled, MultiConsolidateRecords, LogReport / LogPanic (varargs by the format,
// doubles and strings included), Win32GetUserDirectory, FileOpen / FileRead / FileClose / FileSetWritable /
// FileRemove / FileCreate / FileWrite. memmove is the game's own CRT. For each call the original runs, the
// world is restored, the rewrite runs, and the whole world (race.exe's .data and the arena), the return value
// and the stub log are compared bit for bit; the bytes the original changed must lie inside the rewrite's
// footprint (replay_only footprints are reported, not counted). Worlds alternate the x87 between single
// precision (the physics thread) and double (the main thread's usual state).
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
#endif
#if defined(FIX_TESTS) && !defined(VP_TEST_FIXES)
#define VP_TEST_FIXES           // (the flag the other harnesses use)
#endif
#ifndef VP_TEST_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#endif
#include "../hook/port.h"

#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

// ---- what the rewrites link against, standing in for the DLL ------------------------------------------
static int g_logf_quiet, g_logf_count;              // the fix tests count the rewrites' log lines instead
void logf(const char* fmt, ...) {
    g_logf_count++;
    if (g_logf_quiet) return;
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

#include "../hook/phys_record.cpp"

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_state = 0x2545f491u;
static uint32_t rnd() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static int g_special_pct = 3;
static float special() {
    switch (rnd() % 10) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));             // quiet NaN
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));             // signalling NaN
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));          // +-inf
    case 4: return fbits(rnd() & 0x807fffffu);                          // denormal
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits(rnd() & 0x80000000u | 0x7f7fffffu);
    case 7: return -1.0f;                                               // DNF / none
    case 8: return -rf(0, 100);
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
    A_MGR = 0x0000,                                    // the RecordMgr the static points at
    A_MGR2 = 0x0040,                                   // a scratch RecordMgr (constructor, SetMode)
    A_OPTIONS = 0x0080,                                // WorldGameOptions
    A_WORLD = 0x0100, WORLD_N = 0xd00,                 // a World for RecordBegin
    A_INFO = 0x0e00, INFO_N = 16 * 0x40,               // CarMgrInfo[16] (0x40 apart here)
    A_STR = 0x1200, STR_N = 0x200,                     // the user directory, a track name
    A_RACES = 0x1400, RACES_N = 16 * 0x48,             // the race list's buffer
    A_LAPS = 0x1a00, LAPS_N = 128 * 0x48,              // the lap list's (cars x laps <= 128)
    A_STAGES = 0x3e00, STAGES_N = 384 * 0x48,          // the stage list's
    A_SCRATCH = 0xac00, SCRATCH_N = 0x3000,            // scratch records / tags / a RecordList
    A_FILE = 0xdc00, FILE_N = 0x114c8,                 // the RecordMgr's RecordFile
    A_DISK = 0x1f100, DISK_N = 0x114c8,                // the "disk": FileRead's source, FileWrite's target
    A_HEAP = 0x30600, HEAP_N = 0x50000,                // MemAlloc
    ARENA_N = 0x80600,
};
static uint8_t* g_arena;
static uint8_t* g_data;                                // race.exe .data
static uint32_t g_data_n;
static uint8_t* g_pristine;
static uint32_t g_heap_next, g_heap_mark;
static int g_ncars, g_nlaps;
static bool g_alloc_fails;                             // MemAlloc may fail (scripted)

static const char* where(const void* p, char* buf) {
    uint32_t a = (uint32_t)(uintptr_t)p;
    if (p >= g_arena && p < g_arena + ARENA_N) {
        uint32_t off = (uint32_t)((const uint8_t*)p - g_arena);
        struct { uint32_t o, n; const char* name; } parts[] = {
            {A_MGR, 0x40, "mgr"}, {A_MGR2, 0x40, "mgr2"}, {A_OPTIONS, 0x80, "options"}, {A_WORLD, WORLD_N, "world"},
            {A_INFO, INFO_N, "carinfo"}, {A_STR, STR_N, "strings"}, {A_RACES, RACES_N, "races"}, {A_LAPS, LAPS_N, "laps"},
            {A_STAGES, STAGES_N, "stages"}, {A_SCRATCH, SCRATCH_N, "scratch"}, {A_FILE, FILE_N, "file"},
            {A_DISK, DISK_N, "disk"}, {A_HEAP, HEAP_N, "heap"}};
        for (auto& q : parts)
            if (off >= q.o && off < q.o + q.n) {
                uint32_t o = off - q.o;
                if (q.o == A_FILE || q.o == A_DISK)
                    sprintf(buf, "%s+0x%x (record %d +0x%x)", q.name, o, o >= 8 ? (int)((o - 8) / 0x48) : -1, o >= 8 ? (o - 8) % 0x48 : o);
                else if (q.o == A_RACES || q.o == A_LAPS || q.o == A_STAGES)
                    sprintf(buf, "%s[%d]+0x%x", q.name, o / 0x48, o % 0x48);
                else sprintf(buf, "%s+0x%x", q.name, o);
                return buf;
            }
        sprintf(buf, "arena+0x%x", off);
        return buf;
    }
    sprintf(buf, "%s 0x%08x", a >= S_STATICS && a < S_STATICS + STATICS_N ? "record statics" : ".data", a);
    return buf;
}

// ---- the stubs' log ------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 18 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void lg(uint32_t w) { if (g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]++] = w; }
static void lg_str(const char* s) {
    if (!s) { lg(0xffffffffu); return; }
    size_t n = strlen(s);
    lg((uint32_t)n);
    for (size_t i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, s + i, n - i < 4 ? n - i : 4); lg(w); }
}
static void lg_varargs(const char* fmt, va_list ap) {         // by conversion: doubles whole, strings' contents
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') { p++; continue; }
        const char* q = p + 1;
        while (*q && !strchr("diouxXcsfeEgGp", *q)) q++;
        if (*q == 'f' || *q == 'e' || *q == 'g' || *q == 'E' || *q == 'G') {
            double d = va_arg(ap, double);
            uint64_t u;
            memcpy(&u, &d, 8);
            lg((uint32_t)u); lg((uint32_t)(u >> 32));
        } else if (*q == 's') {
            const char* s = va_arg(ap, const char*);
            lg((uint32_t)(uintptr_t)s);
            char buf[64] = {};
            if (s) strncpy(buf, s, 63);
            lg_str(buf);
        } else lg(va_arg(ap, uint32_t));
        if (*q) p = q;
    }
}

// __cdecl stubs
static void* __cdecl st_MemAlloc(int n) {
    lg('MALC'); lg((uint32_t)n);
    if ((g_alloc_fails && script() % 16 == 0) || n < 0 || g_heap_next + (uint32_t)n > HEAP_N) return 0;
    void* p = g_arena + A_HEAP + g_heap_next;
    g_heap_next += ((uint32_t)n + 15) & ~15u;
    return p;
}
static void __cdecl st_delete(void* p) { lg('DELE'); lg((uint32_t)(uintptr_t)p); }
static const uint8_t* __cdecl st_CarMgrGetInfo(int car) { lg('CGIN'); lg((uint32_t)car); return g_arena + A_INFO + (car & 15) * 0x40; }
static int __cdecl st_CarMgrCount(void) { lg('CCNT'); return g_ncars; }
static void __cdecl st_TimeGetTimeOfDay(uint8_t* tod) {
    lg('TIME');                                                     // (tod is on the caller's stack)
    for (int i = 0; i < 0x14; i++) tod[i] = (uint8_t)script();
}
static const uint8_t* __cdecl st_WorldGameOptions(void) { lg('WGOP'); return g_arena + A_OPTIONS; }
static uint8_t __cdecl st_MultiEnabled(void) { lg('MENA'); return (uint8_t)(script() % 3 == 0); }
static void __cdecl st_MultiConsolidateRecords(void) { lg('MCON'); }
template <uint32_t TAG> static void __cdecl st_log(const char* fmt, ...) {
    lg(TAG); lg_str(fmt);
    va_list ap;
    va_start(ap, fmt);
    lg_varargs(fmt, ap);
    va_end(ap);
}
static const char* g_user_dir;                          // the arena's (A_STR), or a fix test's long one
static const char* __cdecl st_UserDir(void) { lg('UDIR'); return g_user_dir ? g_user_dir : (const char*)g_arena + A_STR; }
static int __cdecl st_FileOpen(const char* path) { lg('FOPN'); lg_str(path); return script() % 5 ? 5 : 0; }
static uint8_t __cdecl st_FileRead(int fd, void* buf, int* n) {
    lg('FRED'); lg((uint32_t)fd); lg((uint32_t)(uintptr_t)buf); lg((uint32_t)*n);
    uint32_t r = script() % 8;
    if (r == 0) return 0;                                           // a failed read
    uint32_t len = r == 1 ? script() % DISK_N : DISK_N;             // a short one
    if ((uint32_t)*n < len) len = (uint32_t)*n;
    memcpy(buf, g_arena + A_DISK, len);
    *n = (int)len;
    return 1;
}
static void __cdecl st_FileClose(int* fd) { lg('FCLS'); lg((uint32_t)*fd); *fd = 0; }
static void __cdecl st_FileSetWritable(const char* path, uint8_t w) { lg('FSWR'); lg_str(path); lg(w); }
static uint8_t __cdecl st_FileRemove(const char* path) { lg('FREM'); lg_str(path); return 1; }
static int __cdecl st_FileCreate(const char* path) { lg('FCRT'); lg_str(path); return script() % 6 ? 7 : 0; }
static uint8_t __cdecl st_FileWrite(int fd, const void* p, int n) {
    lg('FWRT'); lg((uint32_t)fd); lg((uint32_t)(uintptr_t)p); lg((uint32_t)n);
    memcpy(g_arena + A_DISK, p, (uint32_t)n < DISK_N ? (uint32_t)n : DISK_N);   // compared with the world
    return 1;
}

static void install_stubs() {
    patch_jump(0x004140e0, (void*)st_MemAlloc);
    patch_jump(0x00414390, (void*)st_delete);
    patch_jump(0x00464490, (void*)st_CarMgrGetInfo);
    patch_jump(0x00464480, (void*)st_CarMgrCount);
    patch_jump(0x004d8af0, (void*)st_TimeGetTimeOfDay);
    patch_jump(0x004627a0, (void*)st_WorldGameOptions);
    patch_jump(0x004a23c0, (void*)st_MultiEnabled);
    patch_jump(0x004a2b60, (void*)st_MultiConsolidateRecords);
    patch_jump(0x00411150, (void*)st_log<'LOGR'>);
    patch_jump(0x004112b0, (void*)st_log<'PANC'>);
    patch_jump(0x00412cc0, (void*)st_UserDir);
    patch_jump(0x00411780, (void*)st_FileOpen);
    patch_jump(0x00411940, (void*)st_FileRead);
    patch_jump(0x00411850, (void*)st_FileClose);
    patch_jump(0x00411b80, (void*)st_FileSetWritable);
    patch_jump(0x00411ba0, (void*)st_FileRemove);
    patch_jump(0x004115f0, (void*)st_FileCreate);
    patch_jump(0x00411a30, (void*)st_FileWrite);
}

// ---- random worlds ------------------------------------------------------------------------------------------------
static void random_bytes(void* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t*)p)[i] = (uint8_t)rnd(); }
static void random_name(char* s, int maxlen) {
    int n = ri(0, maxlen);
    for (int k = 0; k < n; k++) s[k] = (char)(k == 0 && rnd() % 5 == 0 ? (rnd() % 2 ? 0xe9 : '1') : 'a' + rnd() % 26);
    if (n && rnd() % 2) s[0] = (char)(s[0] & 0xdf);
    s[n] = 0;
}
// a track name that takes one of RecordMgr::RecordMgr's FIX paths (it wouldn't fit the 15-byte name) is cut to
// one that doesn't in the fix build, so the random worlds stay inputs the fixed rewrites must still match on
static void fit_name(char* s) {
#ifdef VP_TEST_FIXES
    size_t n = strlen(s);
    bool ext = n >= 4 && s[n - 4] == '.';
    if (n > 14 || (!ext && n > 10)) s[10] = 0;
#else
    (void)s;
#endif
}
static float g_pool[8];                                // recent times: ties
static float a_time(float lo, float hi) {
    if (rnd() % 4 == 0) return g_pool[rnd() % 8];
    float t = sp(rf(lo, hi));
    g_pool[rnd() % 8] = t;
    return t;
}
// a plausible file record: used, a car's names, times; flags now and then with the new-record bits
static void random_file_record(uint8_t* r, bool used) {
    random_bytes(r, 0x48);
    RaceRecord* x = (RaceRecord*)r;
    random_name(x->car_name, 12);
    random_name(x->driver_name, 20);
    x->lap_time = a_time(30, 120);
    x->top_speed = a_time(20, 90);
    x->race_time = a_time(100, 1000);
    x->flags = used ? (uint8_t)(rnd() % 4 ? 0x11 : 0x11 | (rnd() & 0xe)) : (uint8_t)(rnd() % 4 ? 0 : rnd() & 0xfe);
    if (rnd() % 20 == 0) x->flags = (uint8_t)rnd();
}
static void random_file(uint8_t* f) {
    memset(f, 0, FILE_N);
    ((uint32_t*)f)[0] = 0x114c8;
    ((uint32_t*)f)[1] = 4;
    for (int z = 0; z < 12; z++) {
        uint8_t* baz = f + 8 + z * 0x1710;
        random_file_record(baz, rnd() % 3 != 0);
        random_file_record(baz + 0x48, rnd() % 3 != 0);
        for (int t = 0; t < 8; t++) {
            uint8_t* s = baz + 0x90 + t * 0x2d0;
            int used = rnd() % 3 ? ri(0, 10) : 0;
            float time = rf(900, 1000);
            for (int j = 0; j < 10; j++) {
                random_file_record(s + j * 0x48, j >= 10 - used);
                if (j >= 10 - used && rnd() % 8) { ((RaceRecord*)(s + j * 0x48))->race_time = time; time -= rf(0, 60); }
            }
        }
    }
}
// a record in a list: plausible, or garbage now and then
static void random_list_record(uint8_t* r, int kind) {
    random_bytes(r, 0x48);
    RaceRecord* x = (RaceRecord*)r;
    random_name(x->car_name, 12);
    random_name(x->driver_name, 20);
    x->car = (uint8_t)(rnd() % 8 ? ri(0, g_ncars - 1) : ri(0, 15));
    x->flags = (uint8_t)(rnd() % 3 ? 0x11 : rnd() % 2 ? 1 : 0x31);
    if (rnd() % 16 == 0) x->flags = (uint8_t)rnd();
    x->lap_time = a_time(30, 120);
    x->top_speed = a_time(20, 90);
    x->race_time = a_time(100, 1000);
    if (kind == 0) { x->lap = x->stage = 0xfa; }                         // race
    else if (kind == 1) { x->lap = (uint8_t)ri(0, g_nlaps); x->stage = 0xfa; }   // lap
    else { x->lap = (uint8_t)ri(0, g_nlaps); x->stage = (uint8_t)ri(1, 3); }   // stage (1..3: set_best_stages' table)
}
static void make_list(RecordList* l, uint32_t name, uint32_t buf, int cap, int kind) {
    l->name = P<const char>(name);
    l->recs = (RaceRecord*)(g_arena + buf);
    l->cap = cap;
    l->count = rnd() % 4 == 0 ? ri(0, cap) : 0;
    for (int i = 0; i < l->count; i++) random_list_record(g_arena + buf + i * 0x48, kind);
}

static void build_world() {
    memcpy(g_data, g_pristine, g_data_n);
    memset(g_arena, 0, ARENA_N);
    g_heap_mark = 0;
    g_ncars = ri(1, 8);
    g_nlaps = rnd() % 8 ? ri(1, 8) : ri(9, 16);
    for (int i = 0; i < 8; i++) g_pool[i] = rf(30, 120);
    for (int i = 0; i < 16; i++) {
        uint8_t* info = g_arena + A_INFO + i * 0x40;
        random_bytes(info, 0x40);
        info[4] = (uint8_t)(rnd() % 3 ? 1 : rnd() % 2 ? 0 : rnd());
        random_name((char*)info + 5, 12);
        random_name((char*)info + 0x12, rnd() % 10 ? 20 : 32);
    }
    at<int32_t>(g_arena + A_OPTIONS, 0xc) = ri(0, 6);
    {   // the user directory
        char* s = (char*)g_arena + A_STR;
        strcpy(s, "C:\\Viper\\");
        random_name(s + strlen(s), 40);
        strcat(s, "\\");
    }
    {   // a World
        uint8_t* w = g_arena + A_WORLD;
        char* name = (char*)w + 8;
        random_name(name, 10);
        if (rnd() % 3) strcat(name, rnd() % 4 ? ".trk" : ".t");
        fit_name(name);
        at<int32_t>(w, 0xca8) = g_ncars;
        at<int32_t>(w, 0xcc0) = g_nlaps;
        at<int32_t>(w, 0xcac) = ri(0, 2);
        at<int32_t>(w, 0xcbc) = ri(0, 7);
        at<uint8_t>(w, 0xcd1) = (uint8_t)(rnd() % 3 ? rnd() % 2 : rnd());
    }
    // the lists (capacities as RecordBegin makes them, bounded by the arena's buffers)
    int lap_cap = g_ncars * g_nlaps;
    if (lap_cap > 128) lap_cap = 128;
    make_list(races(), STR_RACE, A_RACES, g_ncars, 0);
    make_list(laps(), STR_LAP, A_LAPS, lap_cap, 1);
    make_list(stages(), STR_STAGE, A_STAGES, lap_cap * 3, 2);
    random_bytes(P<void>(S_DATE), 4);
    // the RecordMgr and its file; the disk
    RecordMgr* m = (RecordMgr*)(g_arena + A_MGR);
    m->dirty = (uint8_t)(rnd() % 2);
    strcpy(m->filename, "bemidji.sco");
    m->file = (RecordFile*)(g_arena + A_FILE);
    m->realism = ri(0, 5);
    m->race_type = ri(0, 7);
    *P<RecordMgr*>(S_MGR) = m;
    random_file(g_arena + A_FILE);
    random_file(g_arena + A_DISK);
    if (rnd() % 5 == 0) at<uint32_t>(g_arena + A_DISK, rnd() % 2 ? 0 : 4) ^= 1 + (rnd() & 7);   // foreign
    random_bytes(g_arena + A_SCRATCH, SCRATCH_N);
    RecordMgr* m2 = (RecordMgr*)(g_arena + A_MGR2);
    random_bytes(m2, 0x1c);
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

struct Stat { const char* name; long calls, changed, fails, fp_fails, fp_info, crashes; int first_fail_world; };
static Stat g_stats[96];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0, 0, -1};
    return g_stats[g_nstats++];
}
static uint8_t *g_start, *g_after;                      // .data then the arena
static uint32_t g_world_n;
static Footprint g_fp;
static int g_world;
static bool g_ret_is_self;                              // a constructor: its return value is its `this`
static bool g_crashed;                                  // a call crashed (both ways): the world is abandoned
static bool g_trace = getenv("VP_TRACE") != 0;

static void snapshot(uint8_t* to) { memcpy(to, g_data, g_data_n); memcpy(to + g_data_n, g_arena, ARENA_N); }
static void restore(const uint8_t* from) { memcpy(g_data, from, g_data_n); memcpy(g_arena, from + g_data_n, ARENA_N); }
static uint8_t* live(uint32_t i) { return i < g_data_n ? g_data + i : g_arena + (i - g_data_n); }

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
    g_heap_next = g_heap_mark;
    g_pass = 0; g_nlog[0] = 0;
    g_running = name;
    uint64_t ro = guarded(run, true);
    uint32_t heap_o = g_heap_next;
    snapshot(g_after);
    restore(g_start);
    g_script = seed;
    g_heap_next = g_heap_mark;
    g_pass = 1; g_nlog[1] = 0;
    uint64_t rn = guarded(run, false);
    if (ro == 0xdeaddeaddeaddeadull || rn == 0xdeaddeaddeaddeadull) { st.crashes++; g_crashed = true; }
    bool changed = false, bad = (ro != rn && !g_ret_is_self) || g_nlog[0] != g_nlog[1] ||
                               memcmp(g_log[0], g_log[1], 4 * g_nlog[0]) != 0 || heap_o != g_heap_next;
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
                char w[96];
                printf("  %s: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", where(b, w), sa, aa, fbits(aa), ba, fbits(ba));
            }
        }
    }
    if (changed) st.changed++;
    if (bad) {
        if (st.fails < 4) {
            if (shown == 0) printf("MISMATCH %s (world %d)\n", name, g_world);
            if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
            if (heap_o != g_heap_next) printf("  heap: original %x, rewrite %x\n", heap_o, g_heap_next);
            int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
            int diff = 0;
            for (int i = 0; i < nl && diff < 12; i++) {
                bool ha = i < g_nlog[0], hb = i < g_nlog[1];
                if (ha && hb && g_log[0][i] == g_log[1][i]) continue;
                printf("  log[%d] of %d/%d: original %08x, rewrite %08x\n", i, g_nlog[0], g_nlog[1], ha ? g_log[0][i] : 0, hb ? g_log[1][i] : 0);
                diff++;
            }
        }
        if (st.first_fail_world < 0) st.first_fail_world = g_world;
        st.fails++;
    }
    // the footprint must cover every byte the original changed
    for (uint32_t i = 0; i < g_world_n; i++) {
        if (g_after[i] == g_start[i]) continue;
        uint8_t* p = live(i);
        bool in = false;
        for (int k = 0; k < g_fp.n && !in; k++)
            in = p >= (uint8_t*)g_fp.r[k].p && p < (uint8_t*)g_fp.r[k].p + g_fp.r[k].n;
        if (!in) {
            char w[96];
            if (g_fp.replay_only) {
                if (st.fp_info++ < 2) printf("footprint (replay_only: %s) %s (world %d): %s changed outside it\n", g_fp.replay_only, name, g_world, where(p, w));
            } else if (st.fp_fails++ < 4) {
                printf("FOOTPRINT %s (world %d): %s changed outside it\n", name, g_world, where(p, w));
            }
            break;
        }
    }
    restore(g_after);                                  // go on from the original's result
    g_heap_mark = heap_o;
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

// ---- a race history ---------------------------------------------------------------------------------------------
static int a_car() { return rnd() % 12 ? ri(0, g_ncars - 1) : ri(0, 15); }
static int a_lap() { return rnd() % 10 ? ri(1, g_nlaps) : ri(0, 20); }
static uint32_t lap_time() { return ubits(a_time(30, 120)); }
static uint32_t speed() { return ubits(a_time(20, 90)); }
static bool stages_in_table() {                        // set_best_stages' table covers every stage record
    for (int32_t i = 0; i < stages()->count; i++) {
        const uint8_t* r = rec_at(stages(), i);
        if ((uint32_t)((int32_t)(int8_t)r[0x41] * 3 + (int32_t)r[0x42] - 1) >= 48) return false;
    }
    return true;
}
static bool have_lists() { return races()->recs && laps()->recs && stages()->recs && mgr() && mgr()->file; }

static void physics_step() {
    switch (rnd() % 24) {
    case 0: case 1: case 2: case 3: CHECK(RecordNewLap, a_car(), a_lap(), lap_time(), speed()); break;
    case 4: CHECK(RecordUnofficialNewLap, a_car(), a_lap(), lap_time(), speed()); break;
    case 5: case 6: CHECK(RecordStage, a_car(), ri(1, 3), a_lap(), lap_time()); break;        // (car, stage, lap)
    case 7: CHECK(RecordUnofficialStage, a_car(), ri(1, 3), a_lap(), lap_time()); break;
    case 8: CHECK(RecordRaceOver, a_car()); break;
    case 9: CHECK(RecordUnofficialRaceOver, a_car()); break;
    case 10: CHECK(record_new_lap, a_car(), a_lap(), lap_time(), speed(), (uint8_t)(rnd() % 2)); break;
    case 11: CHECK(record_race_over, a_car(), (uint8_t)(rnd() % 3 ? rnd() % 2 : rnd())); break;
    case 12: CHECK(record_stage, a_car(), ri(1, 3), a_lap(), lap_time(), (uint8_t)(rnd() % 2)); break;
    case 13: {
        RecordList* l = rnd() % 3 == 0 ? races() : rnd() % 2 ? laps() : stages();
        CHECK(record, l, a_car(), l == races() ? 0xfa : a_lap(), l == stages() ? ri(1, 3) : 0xfa, lap_time(), (uint8_t)(rnd() % 2));
        break;
    }
    case 14: CHECK(best_laptime_for_car, a_car()); CHECK(best_speed_for_car, a_car()); break;
    case 15: CHECK(sum_times_for_car, a_car()); break;
    case 16: CHECK(set_racetime_for_car, a_car(), lap_time()); break;
    case 17: CHECK(RecordGetRaceTime, a_car()); CHECK(RecordGetRaceResult, a_car()); break;
    case 18: CHECK(RecordGetStageResult, a_car(), a_lap(), ri(0, 3)); CHECK(RecordGetLapResult, a_car(), a_lap()); break;
    case 19: g_ret_is_self = false; CHECK(RecordMgr_init_baserecord, mgr(), 0, (RaceRecord*)(g_arena + A_SCRATCH + 4 * ri(0, 64)), a_car()); break;
    case 20: if (rnd() % 8 == 0) CHECK0(RecordReset); break;
    default: {                                         // a lap for every car, in order (the usual history)
        int lap = a_lap();
        for (int c = 0; c < g_ncars && !g_crashed; c++) {
            if (rnd() % 3 == 0) CHECK(RecordStage, c, ri(1, 3), lap, lap_time());
            CHECK(RecordNewLap, c, lap, lap_time(), speed());
            if (rnd() % 4 == 0) CHECK(RecordRaceOver, c);
        }
    }
    }
}
static void main_step() {
    RecordMgr* m = mgr();
    RecordMgr* m2 = (RecordMgr*)(g_arena + A_MGR2);
    uint8_t* scratch = g_arena + A_SCRATCH;
    switch (rnd() % 20) {
    case 0: CHECK0(create_dnfs); break;
    case 1: CHECK(RecordMgr_CheckLaps, m, 0, laps()->recs, laps()->count); break;
    case 2: CHECK(RecordMgr_CheckRace, m, 0, races()->recs, races()->count); break;
    case 3: if (stages_in_table()) CHECK0(set_best_stages); break;
    case 4: case 5: if (stages_in_table()) CHECK0(RecordConsolidate); break;
    case 6: CHECK(RecordMgr_save, m, 0); break;
    case 7: CHECK(RecordFile_clear_record_bits, m->file, 0); break;
    case 8: CHECK(RecordMgr_SetMode, m, 0, ri(0, 2), ri(0, 7), (uint8_t)(rnd() % 3 ? rnd() % 2 : rnd())); break;
    case 9: if (rnd() % 3 == 0) CHECK(RecordMgr_Clear, m, 0); break;
    case 10: CHECK(RecordMgr_GetNthBestRace, m, 0, ri(-2, 11)); CHECK(RecordMgr_GetNthBestLap, m, 0, ri(-1, 1)); CHECK(RecordMgr_GetNthBestSpeed, m, 0, ri(-1, 1)); break;
    case 11: CHECK(RecordMgr_dump, m, 0); break;
    case 12: g_ret_is_self = true; CHECK(RaceRecord_ctor, (RaceRecord*)(scratch + 4 * ri(0, 64)), 0); break;
    case 13: g_ret_is_self = true; CHECK(footag_ctor, (Footag*)(scratch + 4 * ri(0, 64)), 0); break;
    case 14: g_ret_is_self = true; CHECK(baztag_ctor, (Baztag*)(scratch + 4 * ri(0, 64)), 0); break;
    case 15: g_ret_is_self = true; CHECK(bartag_ctor, (Bartag*)scratch, 0); break;
    case 16: CHECK(RecordMgr_SetMode, m2, 0, (int)rnd(), (int)rnd(), (uint8_t)rnd()); break;
    case 17: {                                         // the board's own RecordMgr: create, use, destroy
        g_alloc_fails = rnd() % 4 == 0;
        char* name = (char*)g_arena + A_STR + 0x100;
        random_name(name, 10);
        if (rnd() % 2) strcat(name, ".trk");
        fit_name(name);
        uint32_t mark = g_heap_mark;
        CHECK(RecordMgrCreate, name);
        g_alloc_fails = false;
        RecordMgr* b = g_heap_mark > mark + 0x20 ? (RecordMgr*)(g_arena + A_HEAP + mark) : 0;   // (the manager, then its file)
        if (b && !g_crashed && b->file) {
            CHECK(RecordMgr_SetMode, b, 0, ri(0, 2), ri(0, 7), (uint8_t)(rnd() % 2));
            CHECK(RecordMgr_GetNthBestRace, b, 0, ri(0, 9));
            if (rnd() % 2) CHECK(RecordMgr_Clear, b, 0);
            CHECK(RecordMgrDestroy, b);
        }
        break;
    }
    case 18: {                                         // a constructor / destructor on the scratch manager
        char* name = (char*)g_arena + A_STR + 0x100;
        random_name(name, 10);
        if (rnd() % 2) strcat(name, rnd() % 2 ? ".trk" : ".x");
        fit_name(name);
        g_ret_is_self = true;
        CHECK(RecordMgr_ctor, m2, 0, (const char*)name);
        if (!g_crashed && m2->file) CHECK(RecordMgr_dtor, m2, 0);
        break;
    }
    default: CHECK(RecordMgr_GetNthBestRace, m, 0, ri(0, 9)); break;
    }
}

static void run_world() {
    build_world();
    g_crashed = false;
    g_alloc_fails = false;
    if (rnd() % 2) CHECK(RecordBegin, (const uint8_t*)(g_arena + A_WORLD));   // the lists and manager from the original
    if (g_crashed || !have_lists()) return;
    for (int s = ri(10, 60); s-- && !g_crashed;) {
        if (rnd() % 4) physics_step();
        else main_step();
    }
    if (g_crashed) return;
    if (stages_in_table()) CHECK0(RecordConsolidate);                          // the race ends
    if (g_crashed) return;
    if (rnd() % 2) CHECK0(RecordEnd);
}

#ifdef VP_TEST_FIXES
// ---- the fixes, on the inputs that used to overrun ------------------------------------------------------------------
// the first `tag` entry in the rewrite's log carries exactly the string `expect` (lg_str)
static bool logged_str(uint32_t tag, const char* expect) {
    const uint32_t* L = g_log[1];
    for (int i = 0; i + 1 < g_nlog[1]; i++) {
        if (L[i] != tag) continue;
        const uint32_t n = L[i + 1];
        if (n != strlen(expect)) return false;
        for (uint32_t k = 0; k < n; k += 4) {
            uint32_t w = 0;
            memcpy(&w, expect + k, n - k < 4 ? n - k : 4);
            if (L[i + 2 + k / 4] != w) return false;
        }
        return true;
    }
    return false;
}
static void rewrite_pass() { g_pass = 1; g_nlog[1] = 0; g_script = rnd() | 1; g_heap_next = g_heap_mark; }
template <typename F> static bool no_fault(F f) {
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { f(); })) { return false; }
    return true;
#else
    __try { f(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
#endif
}
typedef RecordMgr*(__fastcall* OrigMgrCtor_t)(RecordMgr*, Edx, const char*);
typedef void(__cdecl* Void0_t)(void);

static int fix_tests() {
    int failed = 0;
    static char expect[0x1000], path[0x1000], dir[0x900];
    RecordMgr* m2 = (RecordMgr*)(g_arena + A_MGR2);
    char* name = (char*)g_arena + A_STR + 0x100;

    g_logf_quiet = 1;
    g_logf_count = 0;
    // 1. RecordMgr::RecordMgr, a track name too long for the 15-byte name: "<its stem's first 10>.sco", the RecordFile
    //    pointer after the name intact, and that file name in the path it opens
    int bad = 0;
    for (int it = 0; it < 2000; it++) {
        build_world();
        const int len = ri(11, 60);
        for (int k = 0; k < len; k++) name[k] = (char)('a' + rnd() % 26);
        name[len] = 0;
        const bool ext = len > 14 && rnd() % 2;
        if (ext) memcpy(name + len - 4, ".trk", 4);
        const int stem = (ext ? len - 4 : len) < 10 ? (ext ? len - 4 : len) : 10;
        memcpy(expect, name, stem);
        strcpy(expect + stem, ".sco");
        strcpy(path, (const char*)g_arena + A_STR);
        strcat(path, expect);
        rewrite_pass();
        const bool ran = no_fault([&] { RecordMgr_ctor(m2, 0, name); });
        const bool ok = ran && !strcmp(m2->filename, expect) && (uint8_t*)m2->file == g_arena + A_HEAP && logged_str('FOPN', path);
        if (!ok && bad++ < 4) printf("  FIX RecordMgr::RecordMgr(\"%s\"): name \"%.15s\", file %p, path %s\n", name, m2->filename,
                                     (void*)m2->file, logged_str('FOPN', path) ? "ok" : "wrong");
    }
    printf("fix: RecordMgr::RecordMgr on 2000 track names of 11..60 characters: %d wrong\n", bad);
    failed += bad != 0 || g_logf_count != 2000;             // one log line each

    // 2. a user directory of 245..0x800 characters: the constructor opens and save() creates <dir><name> (cut at 0x3ff).
    //    The constructor's own save (a recreated file) goes to the rewrite, as it does in the game (the original's
    //    would overrun its stack here).
    bad = 0;
    uint8_t save_entry[5];
    memcpy(save_entry, (void*)0x0042ac20, 5);
    patch_jump(0x0042ac20, (void*)&RecordMgr_save);
    for (int it = 0; it < 1000; it++) {
        build_world();
        const int dl = it % 4 ? ri(245, 0x3f0) : ri(0x3f0, 0x800);
        strcpy(dir, "C:\\");
        for (int k = 3; k < dl - 1; k++) dir[k] = (char)('a' + rnd() % 26);
        dir[dl - 1] = '\\';
        dir[dl] = 0;
        g_user_dir = dir;
        strcpy(name, "bemidji.trk");
        strcpy(path, dir);
        strcat(path, "bemidji.sco");
        path[0x3ff] = 0;
        rewrite_pass();
        bool ok = no_fault([&] { RecordMgr_ctor(m2, 0, name); }) && logged_str('FOPN', path) && !strcmp(m2->filename, "bemidji.sco");
        m2->dirty = 1;
        rewrite_pass();
        ok = ok && no_fault([&] { RecordMgr_save(m2, 0); }) && logged_str('FCRT', path);
        g_user_dir = 0;
        if (!ok && bad++ < 4) printf("  FIX a user directory of %d characters: the records path is wrong\n", dl);
    }
    memcpy((void*)0x0042ac20, save_entry, 5);
    printf("fix: RecordMgr::RecordMgr and save with a user directory of 245..2048 characters, 1000 times: %d wrong\n", bad);
    failed += bad != 0;

    // 3. set_best_stages with stage records outside its table: they're skipped, and every other record comes out as
    //    the original leaves it when those records are moved to table slots nobody else uses (and put back after)
    bad = 0;
    int runs = 0, spoiled = 0;
    for (int it = 0; it < 3000; it++) {
        build_world();
        RecordList* l = stages();
        if (l->cap < 4) continue;
        l->count = ri(2, l->cap);
        for (int i = 0; i < l->count; i++) random_list_record(rec_at(l, i), 2);
        for (int b = ri(1, 4); b > 0; b--) {
            uint8_t* r = rec_at(l, ri(0, l->count - 1));
            switch (rnd() % 3) {
            case 0: r[0x41] = (uint8_t)(0x80 | rnd()); break;                    // a negative car
            case 1: r[0x41] = 0; r[0x42] = 0; break;                              // slot -1
            default: r[0x41] = 15; r[0x42] = (uint8_t)ri(4, 255); break;          // past slot 47
            }
        }
        auto slot = [](const uint8_t* r) { return (int32_t)(int8_t)r[0x41] * 3 + (int32_t)r[0x42] - 1; };
        bool used[48] = {};
        int nb = 0, outs[384];
        for (int i = 0; i < l->count; i++) {
            const int32_t k = slot(rec_at(l, i));
            if ((uint32_t)k < 48) used[k] = true;
            else outs[nb++] = i;
        }
        int nfree = 0, free_slots[48];
        for (int k = 0; k < 48; k++) if (!used[k]) free_slots[nfree++] = k;
        if (nb == 0 || nfree < nb) continue;
        runs++;
        spoiled += nb;
        const uint32_t bytes = (uint32_t)l->count * 0x48;
        static uint8_t start[384 * 0x48], fixed[384 * 0x48];
        memcpy(start, l->recs, bytes);
        rewrite_pass();
        bool ok = no_fault([&] { set_best_stages(); });
        memcpy(fixed, l->recs, bytes);
        memcpy(l->recs, start, bytes);
        for (int j = 0; j < nb; j++) {
            uint8_t* r = rec_at(l, outs[j]);
            r[0x41] = (uint8_t)(free_slots[j] / 3);
            r[0x42] = (uint8_t)(free_slots[j] % 3 + 1);
        }
        g_pass = 0;
        ok = ok && no_fault([&] { ((Void0_t)0x0042a510)(); });
        for (int j = 0; j < nb; j++) memcpy(rec_at(l, outs[j]), start + outs[j] * 0x48, 0x48);
        ok = ok && !memcmp(l->recs, fixed, bytes);
        if (!ok && bad++ < 4) printf("  FIX set_best_stages (world %d, %d records, %d outside the table) differs\n", it, l->count, nb);
    }
    printf("fix: set_best_stages with %d records outside its table in %d lists: %d differ\n", spoiled, runs, bad);
    failed += bad != 0 || runs == 0;

    // 5. init_baserecord with a car name filling its 13 bytes (no terminator: it runs on into the driver name) and a
    //    driver name of 0..60 characters: the record must be what the original makes from the names cut to 12 and 32
    bad = 0;
    int orig_spilled = 0;
    for (int it = 0; it < 2000; it++) {
        build_world();
        const int car = ri(0, 7);
        uint8_t* info = g_arena + A_INFO + car * 0x40;
        char* cn = (char*)info + 5;
        char* dn = (char*)info + 0x12;
        const bool long_car = rnd() % 4 != 0;
        const int dlen = ri(0, 60);
        if (long_car) for (int k = 0; k < 13; k++) cn[k] = (char)('a' + rnd() % 26);
        for (int k = 0; k < dlen; k++) dn[k] = (char)('a' + rnd() % 26);
        dn[dlen] = 0;
        if (!long_car && dlen <= 32) continue;                     // nothing to cut
        uint8_t* rec = g_arena + A_SCRATCH + 0x48 * ri(0, 8);
        static uint8_t start[0x48 * 3], fixed[0x48 * 3], cut[0x48 * 3];
        memcpy(start, rec, sizeof start);
        rewrite_pass();
        bool ok = no_fault([&] { RecordMgr_init_baserecord(mgr(), 0, (RaceRecord*)rec, car); });
        memcpy(fixed, rec, sizeof fixed);
        memcpy(rec, start, sizeof start);
        // the original on the cut names (restored after)
        const char c12 = cn[12], d32 = dn[32];
        cn[12] = 0;
        if (dlen > 32) dn[32] = 0;
        g_pass = 0;
        ok = ok && no_fault([&] { ((void(__fastcall*)(RecordMgr*, Edx, RaceRecord*, int))0x00429f80)(mgr(), 0, (RaceRecord*)rec, car); });
        cn[12] = c12;
        dn[32] = d32;
        memcpy(cut, rec, sizeof cut);
        ok = ok && !memcmp(cut, fixed, sizeof cut);
        if (!ok && bad++ < 4) printf("  FIX init_baserecord (car name %s, driver name %d): differs\n", long_car ? "13+" : "short", dlen);
        // the original on the long names, for the record: does it write past the driver name's field?
        memcpy(rec, start, sizeof start);
        g_pass = 0;
        no_fault([&] { ((void(__fastcall*)(RecordMgr*, Edx, RaceRecord*, int))0x00429f80)(mgr(), 0, (RaceRecord*)rec, car); });
        if (dlen > 32 && memcmp(rec + 0x3e, cut + 0x3e, sizeof cut - 0x3e)) orig_spilled++;
        memcpy(rec, start, sizeof start);
    }
    printf("fix: init_baserecord with names past their fields: %d wrong (the original wrote past driver_name %d times)\n", bad, orig_spilled);
    failed += bad != 0 || orig_spilled == 0;

    // 4. RecordMgr::RecordMgr with a name under 3 characters: the '.'s before the RecordMgr stay (the original clears one)
    bad = 0;
    int orig_cleared = 0;
    for (int it = 0; it < 300; it++) {
        build_world();
        const int len = it % 3;
        for (int k = 0; k < len; k++) name[k] = (char)('a' + rnd() % 26);
        name[len] = 0;
        strcpy(expect, name);
        strcat(expect, ".sco");
        memset((uint8_t*)m2 - 4, '.', 4);
        rewrite_pass();
        bool ok = no_fault([&] { RecordMgr_ctor(m2, 0, name); });
        ok = ok && !memcmp((uint8_t*)m2 - 4, "....", 4) && !strcmp(m2->filename, expect);
        if (!ok && bad++ < 4) printf("  FIX RecordMgr::RecordMgr(\"%s\") wrote before the RecordMgr\n", name);
        build_world();
        memset((uint8_t*)m2 - 4, '.', 4);
        g_pass = 0;
        g_heap_next = g_heap_mark;
        if (no_fault([&] { ((OrigMgrCtor_t)0x0042aa30)(m2, 0, name); }) && memcmp((uint8_t*)m2 - 4, "....", 4)) orig_cleared++;
    }
    printf("fix: RecordMgr::RecordMgr with names of 0..2 characters after a '.', 300 times: %d wrong (the original cleared one %d times)\n",
           bad, orig_cleared);
    failed += bad != 0 || orig_cleared != 300;
    return failed;
}
#endif

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 2000;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_record.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(0x400000 + ((IMAGE_DOS_HEADER*)0x400000)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".data", 6)) { g_data = (uint8_t*)0x400000 + sec[i].VirtualAddress; g_data_n = sec[i].Misc.VirtualSize; }
    g_pristine = (uint8_t*)malloc(g_data_n);
    memcpy(g_pristine, g_data, g_data_n);
    g_world_n = g_data_n + ARENA_N;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_start = (uint8_t*)malloc(g_world_n);
    g_after = (uint8_t*)malloc(g_world_n);
    install_stubs();
    for (g_world = 0; g_world < worlds; g_world++) {
        unsigned cw;
        _controlfp_s(&cw, g_world & 1 ? _PC_53 : _PC_24, _MCW_PC);    // the main thread's and the physics thread's modes
        g_special_pct = g_world % 4 == 0 ? 0 : g_world % 4 == 1 ? 2 : 10;
        run_world();
    }
    int failed = 0;
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("  %-44s %7ld calls, %7ld changed the world: %s%s%s%s\n", st.name, st.calls, st.changed,
               st.fails ? "DIFFERS" : "identical", st.fp_fails ? "  FOOTPRINT MISSES" : "",
               st.fp_info ? "  (replay_only: writes outside the footprint)" : "", st.crashes ? "  (crashed)" : "");
        failed += st.fails || st.fp_fails;
    }
    printf("%d worlds; %d of %d functions differ or miss their footprint\n", worlds, failed, g_nstats);
#ifdef VP_TEST_FIXES
    failed += fix_tests();
#endif
    return failed ? 1 : 0;
}
