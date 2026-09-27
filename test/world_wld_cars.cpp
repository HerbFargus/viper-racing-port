// world_wld_cars.cpp -- the car-file, world-object and car-list rewrites (hook/wld_cars.cpp) against the originals,
// outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_wld_cars.cpp
//        /Fo%TEMP%\w3\ /Fe%TEMP%\w3\world_wld_cars.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   (and with /DFIX_TESTS, into another folder, for the fix)
//   run:   world_wld_cars.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. Real data from the test install
// (game-files\installs\v1.0-RC, read only, never copied anywhere): the car files, setups and upgrade set of every
// .car there (viper.cf / .ccs / .ugs...), common.res's default.ccs, the tracks' aidef.ccs / <track>.ccs and AI lines,
// and the player's Config\setups\limbo.csu. The game functions around the rewrites are patched with logging stubs
// whose results come from a scripted random stream, replayed identically for the original and the rewrite:
// the resource manager (ResourceExists / Get / Try hand out copies of the real members -- now and then missing, of
// the wrong version or size, or with bytes changed -- from a heap in the arena; Forget), the files (the user
// directory, FileOpen / Size / ReadExact -- the real .csu or a changed one --, Create / Write (hashed) / Close /
// CreateDirectory), LogReport / LogPanic (their arguments by the format, strings by content), sprintf (the MSVC
// CRT's), the options, the menus (MenuDoRaceSetup, PreRaceDo, DoRace -- the worlds they get hashed, as they may be
// on the stack --, the multiplayer line, transport, scheduler, synchronisation and session object), the resource
// sets, SplashLoading, AICarSetup, AIGetDriverNameByCar, the models (mrModelLoad / Unload, GrafLookupDynoModel) and
// PhysicsCreate. The rest -- World::World, MultiGenesisInfo, the driver map and strength (AIGetCarForDriver,
// AIGetDriverForCar, AISetStrength), HackHornBall, MultiEnabled, PhysicsIsPaused, strrchr, the objects' virtuals and
// every function of these three objects called by address -- are the originals. For each call the original runs,
// the world (race.exe's .data and an arena) is restored, the rewrite runs, and the whole world, the return value and
// the stub log are compared bit for bit; the bytes the original changed must lie inside the rewrite's footprint
// (replay_only footprints are reported, not counted). Worlds alternate the x87 between single precision (the
// physics thread) and double (the main thread's usual state).
//
// Masked: the four dwords of a CarSetupData that make_default_setup leaves (+0x14, +0x1c, +0x24, +0x2c) where the
// generic setup reaches a car list or CarFileLoadSetupData's output -- they are the stack's own bytes there.
//
// The fix (port.h: VP_FIX): the plain build defines VP_FAITHFUL and checks the rewrites against the originals as
// above. Built with /DFIX_TESTS it runs the same worlds (every check must still equal the original; the fix is off
// unless the rewrite's LoadRace runs for GameBeginSingle's world), then the fix's own tests: single races with and
// without a loadable AI line (real default.ili / rdefault.ili from kenyon.trk; none; a line of no segments; only
// some drivers' notes), the original GameBeginSingle against the fixed one with LoadRace hooked to the rewrite.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include <tuple>
#include <type_traits>
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
static int g_logf_count;
static char g_logf_last[512];
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_logf_last, sizeof g_logf_last, fmt, ap);
    va_end(ap);
    g_logf_count++;
}
uint32_t A(uint32_t v10) { return v10; }
bool have(uint32_t) { return true; }
bool build_is_v10() { return true; }
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }

#include "../hook/wld_cars.cpp"

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_state = 0x2545f491u;
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

// ---- the real data: members of the install's archives (0TSR: vrmod/archive.py) ------------------------------------
static const char* k_install = "C:\\Users\\player\\Desktop\\claude-code\\game-files\\installs\\v1.0-RC\\";
struct Member { std::string name; uint32_t version; std::vector<uint8_t> data; };
static std::vector<Member> g_members;
static std::vector<uint8_t> g_csu;                          // Config\setups\limbo.csu
static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t got = fread(out.data(), 1, n, f);
    fclose(f);
    return got == (size_t)n;
}
static void load_archive(const char* file, const char* const* want) {
    std::vector<uint8_t> d;
    if (!read_file(std::string(k_install) + file, d) || d.size() < 16 || memcmp(d.data(), "0TSR", 4)) {
        printf("can't read %s from the install\n", file);
        return;
    }
    int32_t n;
    memcpy(&n, &d[4], 4);
    size_t cur = 16 + (size_t)n * 36;
    for (int i = 0; i < n; i++) {
        const uint8_t* e = &d[16 + (size_t)i * 36];
        char name[17] = {};
        memcpy(name, e, 16);
        int32_t version, size;
        memcpy(&version, e + 20, 4);
        memcpy(&size, e + 24, 4);
        if (cur + 8 + size > d.size() || memcmp(&d[cur + 4], "!IGM", 4)) break;
        std::string nm = name;
        for (auto& c : nm) c = (char)tolower((unsigned char)c);
        for (const char* const* w = want; *w; w++)
            if (strstr(nm.c_str(), *w)) {
                g_members.push_back({nm, (uint32_t)version, std::vector<uint8_t>(d.begin() + cur + 8, d.begin() + cur + 8 + size)});
                break;
            }
        cur += 8 + size;
    }
}
static const Member* find_member(const char* name) {
    std::string nm = name;
    for (auto& c : nm) c = (char)tolower((unsigned char)c);
    for (auto& m : g_members)
        if (m.name == nm) return &m;
    return 0;
}

// ---- the world ---------------------------------------------------------------------------------------------------
enum : uint32_t {
    A_WORLD = 0x0000,                                   // a World (0xcd4)
    A_WORLD2 = 0x1000,
    A_CF = 0x2000,                                      // a CarFile (0x228)
    A_OUT = 0x2400,                                     // a CarData (0x1e0)
    A_SETUP = 0x2600,                                   // a CarSetup (0xd4) / CarSetupData
    A_SETUP2 = 0x2700,
    A_UPGRADE = 0x2800,                                 // an upgrade (0x70 + pairs)
    A_UPGRADES = 0x2c00,                                // the upgrade flags (256)
    A_OBJ = 0x2d00, OBJ_N = 0x100,                      // world objects
    A_PACKET = 0x2e00, PACKET_N = 0x200,                // a physics packet
    A_STRINGS = 0x3000, STRING_N = 48, NSTRINGS = 24,   // names
    A_USERDIR = 0x3480,                                 // Win32GetUserDirectory
    A_LIVE = 0x3500,                                    // the fake LiveMultiInfo (+8 -> the session)
    A_SESSION = 0x3540,                                 // the fake session: +0 vtable, +4 players
    A_SESS_OPTS = 0x3580,                               // the options the session hands out (0x28)
    A_PHOBDATA = 0x3600,                                // a PhobData / WobbleData
    A_HEAP = 0x4000, HEAP_N = 0x8000,                   // the resources handed out
    ARENA_N = 0xc000,
};
static uint8_t* g_arena;
static uint8_t* g_data;                                // race.exe .data
static uint32_t g_data_n;
static uint8_t* g_pristine;                            // .data as loaded
static uint32_t g_heap_next;
static const char* str_at(int i) { return (const char*)(g_arena + A_STRINGS + (i % NSTRINGS) * STRING_N); }
static World* world1() { return (World*)(g_arena + A_WORLD); }
static World* world2() { return (World*)(g_arena + A_WORLD2); }

static const char* where(const void* p, char* buf) {
    uint32_t a = (uint32_t)(uintptr_t)p;
    if (p >= g_arena && p < g_arena + ARENA_N) {
        uint32_t off = (uint32_t)((const uint8_t*)p - g_arena);
        struct { uint32_t o, n; const char* name; } parts[] = {
            {A_WORLD, 0xcd4, "world"}, {A_WORLD2, 0xcd4, "world2"}, {A_CF, 0x228, "carfile"}, {A_OUT, 0x1e0, "cardata"},
            {A_SETUP, 0xd4, "setup"}, {A_SETUP2, 0xd4, "setup2"}, {A_UPGRADE, 0x400, "upgrade"}, {A_UPGRADES, 0x100, "upgrade flags"},
            {A_OBJ, OBJ_N, "object"}, {A_PACKET, PACKET_N, "packet"}, {A_STRINGS, NSTRINGS * STRING_N, "strings"},
            {A_LIVE, 0x40, "live"}, {A_SESSION, 0x40, "session"}, {A_SESS_OPTS, 0x80, "session options"},
            {A_PHOBDATA, 0x100, "phobdata"}, {A_HEAP, HEAP_N, "heap"}};
        for (auto& q : parts)
            if (off >= q.o && off < q.o + q.n) {
                if (q.o == A_WORLD || q.o == A_WORLD2) {
                    uint32_t w = off - q.o;
                    if (w >= 0x28 && w < 0x28 + 0xc80) {
                        sprintf(buf, "%s car %u +0x%x", q.name, (w - 0x28) / 0xc8, (w - 0x28) % 0xc8);
                        return buf;
                    }
                }
                sprintf(buf, "%s+0x%x", q.name, off - q.o);
                return buf;
            }
        sprintf(buf, "arena+0x%x", off);
        return buf;
    }
    sprintf(buf, ".data 0x%08x", a);
    return buf;
}

// ---- the stubs' log ------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 18 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static const char* g_running;                          // the check being run
static bool g_quiet;                                   // (fix tests) this entry isn't the original's: not logged
static void lg(uint32_t w) { if (!g_quiet && g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]++] = w; }
static void lg_str(const char* s) {
    if (!s) { lg(0xffffffffu); return; }
    size_t n = strlen(s);
    lg((uint32_t)n);
    for (size_t i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, s + i, n - i < 4 ? n - i : 4); lg(w); }
}
static void lg_varargs(const char* fmt, va_list ap) {         // each argument by its conversion; strings by content
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*hlLI", *p)) {
            if (*p == '*') lg(va_arg(ap, uint32_t));
            p++;
        }
        if (!*p) break;
        if (*p == 's') lg_str(va_arg(ap, const char*));
        else if (strchr("eEfgG", *p)) { double d = va_arg(ap, double); uint64_t u; memcpy(&u, &d, 8); lg((uint32_t)u); lg((uint32_t)(u >> 32)); }
        else lg(va_arg(ap, uint32_t));
    }
}
static uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
// a World's hash, without the car setups' dwords the generic setup leaves (the stack's own bytes)
static uint32_t world_hash(const void* w) {
    uint8_t tmp[0xcd4];
    memcpy(tmp, w, 0xcd4);
    for (int i = 0; i < 16; i++)
        for (uint32_t o : {0x14u, 0x1cu, 0x24u, 0x2cu}) memset(tmp + 0x28 + i * 0xc8 + 0x34 + o, 0, 4);
    return fnv(tmp, 0xcd4);
}
static uint32_t arena_or(const void* p) {                  // an arena pointer by offset; others as "somewhere"
    return p >= g_arena && p < g_arena + ARENA_N ? (uint32_t)((const uint8_t*)p - g_arena) : p ? 0xeeee0000u : 0;
}

// ---- stubs ----------------------------------------------------------------------------------------------------------
static int g_calls_menu, g_calls_prerace, g_calls_line, g_calls_sched;   // (per pass) bound the loops
// coverage: which paths the originals took (counted on the original's pass)
static long cov_prerace[4], cov_solo_moved, cov_sync_ok, cov_multi_resume, cov_multi_sched_live, cov_gears_cf, cov_gears_setup,
    cov_lift1, cov_lift2, cov_lift_lerp, cov_sel_set, cov_sel_unset, cov_gear_cut, cov_packet_same, cov_packet_paused, cov_packet_new;
// resources: the real members, now and then missing / wrong / changed
static bool g_fix_lines;                                   // (fix tests) the AI lines come from g_lines, off the arena
struct LineRes { std::string name; std::vector<uint8_t> data; };
static std::vector<LineRes> g_lines;
static int g_host_allocs;
static bool is_line_name(const char* n) {
    size_t l = strlen(n);
    return l > 4 && (!_stricmp(n + l - 4, ".ili") || !_stricmp(n + l - 4, ".ilq") || !_stricmp(n + l - 4, ".ilg"));
}
static void* give(const void* src, uint32_t n) {
    if (g_heap_next + n + 16 > HEAP_N) return 0;
    uint8_t* p = g_arena + A_HEAP + g_heap_next;
    memcpy(p, src, n);
    g_heap_next += (n + 15) & ~15u;
    return p;
}
static void* resource(const char* name, uint32_t type, uint32_t* version, int32_t* size, uint8_t* flag, bool may_fail) {
    if (g_fix_lines && is_line_name(name)) {                // the fix's line lookups: not the original's, not logged
        for (auto& l : g_lines)
            if (!_stricmp(l.name.c_str(), name)) {
                uint8_t* p = (uint8_t*)malloc(l.data.size() + 16);
                memcpy(p, l.data.data(), l.data.size());
                g_host_allocs++;
                *version = 3;
                *size = (int32_t)l.data.size();
                if (flag) *flag = 1;
                return p;
            }
        return 0;
    }
    lg('RES '); lg_str(name); lg(type);
    const Member* m = find_member(name);
    uint32_t r = script() % 16;
    if (!m || r == 0) {
        if (may_fail && (!m || script() % 2)) return 0;
        // a made-up resource
        uint8_t junk[0x300];
        for (auto& b : junk) b = (uint8_t)script();
        *version = script() % 3 ? 6 : script() % 8;
        *size = 0x228;
        if (flag) *flag = (uint8_t)(script() & 1);
        if (type == TYPE_UPGS) { *version = 1; memset(junk, 0, 4); if (flag) *flag = 1; }   // no upgrades
        return give(junk, sizeof junk);
    }
    std::vector<uint8_t> d = m->data;
    *version = m->version;
    *size = (int32_t)d.size();
    if (flag) *flag = 1;
    if (r == 1) *version = m->version + 1 + script() % 3;
    else if (r == 2) *size = (int32_t)d.size() + (int32_t)(script() % 9) - 4;
    else if (r <= 4 && type != TYPE_UPGS) {                 // some bytes changed (not an upgrade set's offsets)
        for (int k = 0, n = 1 + script() % 8; k < n; k++) d[script() % d.size()] = (uint8_t)script();
    } else if (r == 5 && type != TYPE_UPGS && d.size() >= 4) {   // a float made special
        uint32_t at = (script() % (uint32_t)(d.size() / 4)) * 4;
        uint32_t v = (script() & 1) ? 0x7fc00001u : (script() & 0x80000000u) | 0x34000000u;
        memcpy(&d[at], &v, 4);
    }
    return give(d.data(), (uint32_t)d.size());
}
static uint8_t __cdecl st_ResourceExists(const char* name) {
    lg('REXI'); lg_str(name);
    return (uint8_t)((find_member(name) != 0) ^ (script() % 12 == 0));
}
static void* __cdecl st_ResourceGet(const char* name, uint32_t type, uint32_t* version, int32_t* size, uint8_t* p5, uint8_t* flag) {
    lg('RGET'); lg((uint32_t)(uintptr_t)p5);
    return resource(name, type, version, size, flag, false);
}
static void* __cdecl st_ResourceTry(const char* name, uint32_t type, uint32_t* version, int32_t* size, uint8_t* p5, uint8_t* flag) {
    if (!(g_fix_lines && is_line_name(name))) { lg('RTRY'); lg((uint32_t)(uintptr_t)p5); }
    return resource(name, type, version, size, flag, true);
}
static uint8_t __cdecl st_ResourceForget(void* p) {
    if (p && !(p >= g_arena && p < g_arena + ARENA_N)) { free(p); g_host_allocs--; return 1; }   // a fix-test line
    lg('RFGT'); lg(arena_or(p));
    return 1;
}
template <uint32_t TAG> static void __cdecl st_log(const char* fmt, ...) {
    lg(TAG); lg_str(fmt);
    va_list ap;
    va_start(ap, fmt);
    lg_varargs(fmt, ap);
    va_end(ap);
}
static int __cdecl st_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    bool quiet = g_fix_lines && (!strcmp(fmt, "%x%x%x%s") || !strcmp(fmt, "%s%s%s"));   // the DSTC line names
    if (!quiet) { lg('SPRF'); lg_str(fmt); lg_str(buf); }
    return n;
}
static const char* __cdecl st_UserDir(void) { lg('UDIR'); return (const char*)(g_arena + A_USERDIR); }
static int __cdecl st_FileOpen(const char* path) { lg('FOPN'); lg_str(path); return script() % 5 ? 7 : 0; }
static int __cdecl st_FileSize(int fd) { lg('FSIZ'); lg((uint32_t)fd); return script() % 6 ? 0xd4 : (int)(script() % 0x200); }
static uint8_t __cdecl st_FileReadExact(int fd, void* buf, int n) {
    lg('FRED'); lg((uint32_t)fd); lg((uint32_t)n);
    uint8_t* b = (uint8_t*)buf;
    for (int i = 0; i < n; i++) b[i] = i < (int)g_csu.size() ? g_csu[i] : 0;
    uint32_t r = script() % 8;
    if (r == 0 && n >= 0x90) { uint32_t v = script() % 4; memcpy(b + 0x8c, &v, 4); }          // another version
    else if (r == 1 && n >= 4) { uint32_t at = (script() % (uint32_t)(n / 4)) * 4, v = script(); memcpy(b + at, &v, 4); }
    return 1;
}
static void __cdecl st_FileClose(int* fd) { lg('FCLS'); lg((uint32_t)*fd); *fd = 0; }
static uint8_t __cdecl st_FileCreateDirectory(const char* path) { lg('FDIR'); lg_str(path); return 1; }
static int __cdecl st_FileCreate(const char* path) { lg('FCRT'); lg_str(path); return script() % 4 ? 9 : 0; }
static uint8_t __cdecl st_FileWrite(int fd, const void* p, int n) {
    lg('FWRT'); lg((uint32_t)fd); lg((uint32_t)n); lg(fnv((const uint8_t*)p, (uint32_t)n));
    return (uint8_t)(script() & 1);
}
// options
static void __cdecl st_OptionsGetI(const char* sec, const char* key, int32_t* v) {
    lg('OPTI'); lg_str(sec); lg_str(key); lg((uint32_t)*v);
    uint32_t r = script() % 4;
    if (r == 0) return;
    if (!strcmp(key, "car_count")) *v = r == 1 ? (int32_t)(script() % 20) - 2 : 1 + (int32_t)(script() % 8);
    else *v = (int32_t)(script() % 12) - 2;
}
static void __cdecl st_OptionsGetS(const char* sec, const char* key, char* buf, int n) {
    lg('OPTS'); lg_str(sec); lg_str(key); lg((uint32_t)n); lg_str(buf);
    if (script() % 4 == 0) return;
    int len = (int)(script() % 16);
    if (len > n - 1) len = n - 1;
    for (int i = 0; i < len; i++) buf[i] = (char)('A' + script() % 26);
    buf[len < 0 ? 0 : len] = 0;
}
// the menus and races
static const char* const k_tracks[] = {"kenyon", "limbo", "nfield", "bemidji", "dundas", "hastings", "heaven", "uptown", "notrack"};
static const char* const k_cars[] = {"viper", "airhawk", "airhawkr", "indyjeep", "willys", "nocar"};
static uint8_t __cdecl st_MenuDoRaceSetup(char* car, char* track, uint8_t* opts) {
    lg('MRAC'); lg(arena_or(opts));
    if (++g_calls_menu > 4 || script() % 3 == 0) return 0;
    strcpy(car, k_cars[script() % 6]);
    strcpy(track, k_tracks[script() % 9]);
    for (int i = 0; i < 0x28; i++) opts[i] = (uint8_t)script();
    uint32_t s = script() % 8;
    memcpy(opts + 0x20, &s, 4);
    opts[0x25] = (uint8_t)(script() % 3 == 0);
    return 1;
}
static int32_t __cdecl st_PreRaceDo(World* w, uint32_t flag, uint8_t* p3, uint32_t p4, const float* p5, void* p6) {
    lg('PRER'); lg(world_hash(w)); lg(flag); lg(arena_or(p3)); lg(p4); lg(arena_or(p5)); lg(arena_or(p6));
    if (++g_calls_prerace > 12) return 0;
    uint32_t r = script() % 10;
    const int32_t res = r < 3 ? 0 : r < 6 ? 1 : r < 9 ? 2 : 3;
    if (g_pass == 0) {
        cov_prerace[res]++;
        if (res == 2)
            for (int32_t i = 1; i < w->cars.count && i < 16; i++)
                if (w->cars.cars[i].type == 0) { cov_solo_moved++; break; }
    }
    return res;
}
static void __cdecl st_DoRace(World* w, void* results, void* cb) { lg('DORC'); lg(world_hash(w)); lg(arena_or(results)); lg((uint32_t)(uintptr_t)cb); }
static uint8_t __cdecl st_LineBegin(void) { lg('LINB'); return ++g_calls_line <= 6 && script() % 5 != 0; }
static void __cdecl st_LineEnd(void) { lg('LINE'); }
static uint8_t __cdecl st_MenuMultiChooseTransport(uint8_t* mgi) { lg('MTRN'); lg(fnv(mgi, 0x6c)); memset(mgi, 0x11, 8); return script() % 4 != 0; }
static void* __cdecl st_LiveCreate(void) { lg('LCRT'); return script() % 5 ? g_arena + A_LIVE : 0; }
static uint8_t __cdecl st_MenuMultiScheduler(void* live, uint8_t* mgi) {
    lg('MSCH'); lg(arena_or(live)); lg(mgi ? fnv(mgi, 0x6c) : 0);
    if (g_pass == 0 && !mgi) cov_multi_sched_live++;
    if (++g_calls_sched > 6) return 0;
    return script() % 4 != 0;
}
static void __cdecl st_MultiResumeChat(void) { lg('MRCH'); if (g_pass == 0) cov_multi_resume++; }
static void __cdecl st_MultiLiveEnd(void) { lg('MLEN'); }
static void random_entry(CarListEntry& e, uint32_t (*r)()) {
    uint32_t t = r() % 10;
    e.type = t < 3 ? 0 : t < 6 ? 1 : t < 9 ? 2 : (int32_t)(r() % 6);
    int n = (int)(r() % 12);
    for (int i = 0; i < 13; i++) e.driver[i] = (char)(i < n ? 'a' + r() % 26 : 0);
    strcpy(e.car, k_cars[r() % 6]);
    for (auto& b : e.setup) b = (uint8_t)r();
    e.driver_index = (int32_t)(r() % 9) - 1;
    e.hornball = (uint8_t)(r() % 2);
}
static uint8_t __cdecl st_MultiSynchronize(void* live, CarList* cars, char* track) {
    lg('MSYN'); lg(arena_or(live));
    if (script() % 3 == 0) return 0;
    if (g_pass == 0) cov_sync_ok++;
    cars->count = 1 + (int32_t)(script() % 6);
    for (int i = 0; i < cars->count; i++) random_entry(cars->cars[i], script);
    strcpy(track, k_tracks[script() % 9]);
    return 1;
}
static void __fastcall st_LiveDtor(void* self, int) { lg('LDTR'); lg(arena_or(self)); }
static void __cdecl st_delete(void* p) { lg('DELE'); lg(arena_or(p)); }
static void __cdecl st_MultiLiveBegin(void* live) { lg('MLBG'); lg(arena_or(live)); }
static const void* __fastcall st_session_options(void* self, int) { lg('SOPT'); lg(arena_or(self)); return g_arena + A_SESS_OPTS; }
static uint8_t __fastcall st_session_is_remote(void* self, int, int32_t i) { lg('SREM'); lg(arena_or(self)); lg((uint32_t)i); return (uint8_t)(script() & 1); }
static void* g_session_vt[64];
// resource sets, models, physics
static void __cdecl st_SplashLoading(void) { lg('SPLS'); }
static int g_mustloads;                                     // (fix tests) .car resources registered
static void __cdecl st_MustLoad(const char* name) {
    lg('MUST'); lg_str(name);
    size_t l = strlen(name);
    if (l > 4 && !_stricmp(name + l - 4, ".car")) g_mustloads++;
}
static void __cdecl st_Unload(const char* name) { lg('UNLD'); lg_str(name); }
static int g_aicarsetups;
static void __cdecl st_AICarSetup(uint8_t* setup, const char* car, int32_t i, World* w) {
    lg('AICS'); lg_str(car); lg((uint32_t)i); lg(world_hash(w));
    g_aicarsetups++;
    for (int k = 0; k < 0x8c; k++) setup[k] = (uint8_t)script();
}
static void __cdecl st_NameByCar(char* buf, int32_t car) {
    lg('NAME'); lg((uint32_t)car);
    int n = 1 + (int)(script() % 11);
    for (int i = 0; i < n; i++) buf[i] = (char)('a' + script() % 26);
    buf[n] = 0;
}
static int32_t __cdecl st_ModelLoad(const char* name) { lg('MLOD'); lg_str(name); return (int32_t)(script() % 1000) - 1; }
static void __cdecl st_ModelUnload(int32_t m) { lg('MUNL'); lg((uint32_t)m); }
static int32_t __cdecl st_DynoModel(int32_t i) { lg('DYNO'); lg((uint32_t)i); return (int32_t)(script() % 100); }
static void __cdecl st_PhysicsCreate(void* data, int32_t* out) { lg('PCRT'); lg(arena_or(data)); lg(arena_or(out)); *out = (int32_t)(script() % 0x100) & ~1; }
// the fix's: the track number, the line's memory (off the arena, not logged)
static int32_t __cdecl st_GetTrackNumber(const char* name) {
    for (int i = 0; i < 8; i++) if (!_stricmp(name, k_tracks[i])) return i;
    return -1;
}
// (with room in front: an empty line's fixup_res writes seg[-1].next, before the block -- the original's own overrun)
static void* __cdecl st_MemAlloc(int n) { g_host_allocs++; return (uint8_t*)calloc(1, (n > 0 ? n : 1) + 0x100) + 0x100; }
static void __cdecl st_MemFree(void* p) { if (p) { g_host_allocs--; free((uint8_t*)p - 0x100); } }

// the game's CRT never shows a modal box here: its fatal paths log and end the test
static void __cdecl st_crt_fatal(int code) {
    printf("the game's CRT reached a fatal path (%d) in %s: ending the test\n", code, g_running ? g_running : "set-up");
    fflush(stdout);
    ExitProcess(3);
}
static int __cdecl st_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}
static void install_stubs() {
    patch_jump(0x004cf730, (void*)st_crt_fatal);           // _amsg_exit
    patch_jump(0x004d45b0, (void*)st_crt_fatal);           // _NMSG_WRITE
    patch_jump(0x004d4570, (void*)st_crt_fatal);           // _FF_MSGBANNER
    patch_jump(0x004d5c10, (void*)st_crt_fatal);           // _fptrap
    patch_jump(0x004d6850, (void*)st_crt_msgbox);          // __crtMessageBoxA
    patch_jump(0x00419d10, (void*)st_ResourceExists);
    patch_jump(0x00419fa0, (void*)st_ResourceGet);
    patch_jump(0x00419ce0, (void*)st_ResourceTry);
    patch_jump(0x0041a450, (void*)st_ResourceForget);
    patch_jump(0x00411150, (void*)st_log<'LOGR'>);
    patch_jump(0x004112b0, (void*)st_log<'PANC'>);
    patch_jump(0x004cf0a0, (void*)st_sprintf);
    patch_jump(0x00412cc0, (void*)st_UserDir);
    patch_jump(0x00411780, (void*)st_FileOpen);
    patch_jump(0x00411890, (void*)st_FileSize);
    patch_jump(0x004118b0, (void*)st_FileReadExact);
    patch_jump(0x00411850, (void*)st_FileClose);
    patch_jump(0x00411d00, (void*)st_FileCreateDirectory);
    patch_jump(0x004115f0, (void*)st_FileCreate);
    patch_jump(0x00411a30, (void*)st_FileWrite);
    patch_jump(0x004713e0, (void*)st_OptionsGetI);
    patch_jump(0x00471500, (void*)st_OptionsGetS);
    patch_jump(0x004871e0, (void*)st_MenuDoRaceSetup);
    patch_jump(0x0040f140, (void*)st_PreRaceDo);
    patch_jump(0x00401e10, (void*)st_DoRace);
    patch_jump(0x004a1420, (void*)st_LineBegin);
    patch_jump(0x004a1600, (void*)st_LineEnd);
    patch_jump(0x0048f060, (void*)st_MenuMultiChooseTransport);
    patch_jump(0x004a2b90, (void*)st_LiveCreate);
    patch_jump(0x0048be90, (void*)st_MenuMultiScheduler);
    patch_jump(0x004a2840, (void*)st_MultiResumeChat);
    patch_jump(0x004a2360, (void*)st_MultiLiveEnd);
    patch_jump(0x004a2850, (void*)st_MultiSynchronize);
    patch_jump(0x004a2c40, (void*)st_LiveDtor);
    patch_jump(0x00414390, (void*)st_delete);
    patch_jump(0x004a2370, (void*)st_MultiLiveBegin);
    patch_jump(0x0040cbc0, (void*)st_SplashLoading);
    patch_jump(0x00419a30, (void*)st_MustLoad);
    patch_jump(0x00419bb0, (void*)st_Unload);
    patch_jump(0x0041c2f0, (void*)st_AICarSetup);
    patch_jump(0x00420d40, (void*)st_NameByCar);
    patch_jump(0x004558c0, (void*)st_ModelLoad);
    patch_jump(0x00455950, (void*)st_ModelUnload);
    patch_jump(0x0046dc50, (void*)st_DynoModel);
    patch_jump(0x0042bc10, (void*)st_PhysicsCreate);
    patch_jump(0x004068b0, (void*)st_GetTrackNumber);
    patch_jump(0x004140e0, (void*)st_MemAlloc);
    patch_jump(0x00414300, (void*)st_MemFree);
    g_session_vt[0x64 / 4] = (void*)st_session_options;
    g_session_vt[0x84 / 4] = (void*)st_session_is_remote;
}

// ---- random worlds ------------------------------------------------------------------------------------------------
static void random_bytes(void* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t*)p)[i] = (uint8_t)rnd(); }
static void random_floats(void* p, uint32_t n, float lo, float hi) {
    for (uint32_t i = 0; i + 4 <= n; i += 4) { float f = sp(rf(lo, hi)); memcpy((uint8_t*)p + i, &f, 4); }
}
static void random_world(World* w) {
    random_bytes(w, sizeof(World));
    w->version = 3;
    w->size = 0xcd4;
    memset(w->track, 0, sizeof w->track);
    strcpy(w->track, k_tracks[rnd() % 9]);
    w->cars.count = rnd() % 8 ? ri(1, 8) : ri(0, 16);
    for (int i = 0; i < 16; i++) random_entry(w->cars.cars[i], rnd);
    if (rnd() % 3) {                                          // a single race's list: the player and AI cars
        for (int i = 0; i < 16; i++) w->cars.cars[i].type = 1;
        w->cars.cars[ri(0, 15)].type = 0;
    }
    w->options.strength = ri(0, 7);                          // (out of 0..7 get_paint_resource reads its own stack)
    w->options.reversed = (uint8_t)(rnd() % 3 == 0);
}
static const Member* some_member(const char* ext) {
    std::vector<const Member*> v;
    for (auto& m : g_members) if (m.name.size() > strlen(ext) && m.name.compare(m.name.size() - strlen(ext), strlen(ext), ext) == 0) v.push_back(&m);
    return v.empty() ? 0 : v[rnd() % v.size()];
}
static void build_world() {
    memcpy(g_data, g_pristine, g_data_n);
    memset(g_arena, 0, ARENA_N);
    random_world(world1());
    random_world(world2());
    // a car file, a setup, an upgrade
    if (const Member* m = some_member(".cf")) memcpy(g_arena + A_CF, m->data.data(), 0x228);
    else random_floats(g_arena + A_CF, 0x228, -100, 100);
    if (rnd() % 4 == 0) random_floats(g_arena + A_CF, 0x228, -1000, 1000);
    for (int k = 0, n = rnd() % 3 ? ri(0, 6) : ri(0, 100); k < n; k++) {
        float f = sp(rf(-500, 500));
        if (rnd() % 3 == 0) f = rf(-2e-7f, 2e-7f);             // near FLT_EPSILON
        memcpy(g_arena + A_CF + 4 * ri(0, 0x228 / 4 - 1), &f, 4);
    }
    *(int32_t*)(g_arena + A_CF + 0x108) = rnd() % 4 ? ri(3, 8) : (int32_t)rnd();
    for (uint32_t s : {A_SETUP, A_SETUP2}) {
        if (const Member* m = some_member(".ccs")) memcpy(g_arena + s, m->data.data(), 0x8c);
        if (rnd() % 3 == 0) random_floats(g_arena + s, 0x8c, -0.2f, 1.2f);
        *(int32_t*)(g_arena + s + 0x70) = rnd() % 4 ? ri(0, 2) : (int32_t)rnd();
        random_bytes(g_arena + s + 0x8c, 0x48);
    }
    uint8_t* up = g_arena + A_UPGRADE;
    random_bytes(up, 0x70);
    int np = rnd() % 8 ? ri(0, 20) : ri(0, 80);
    *(int32_t*)(up + 0x6c) = np;
    for (int j = 0; j < np; j++) { *(uint32_t*)(up + 0x70 + 8 * j) = (uint32_t)ri(0, 0x224); *(uint32_t*)(up + 0x74 + 8 * j) = rnd(); }
    for (int j = 0; j < 256; j++) g_arena[A_UPGRADES + j] = (uint8_t)(rnd() % 3 == 0);
    // objects, a packet
    random_bytes(g_arena + A_OBJ, OBJ_N);
    random_floats(g_arena + A_OBJ + 4, 0x30, -500, 500);
    static const uint32_t vts[] = {VT_WORLDOBJ, VT_MODEL, VT_WOBBLE};
    *(uint32_t*)(g_arena + A_OBJ) = vts[rnd() % 3];
    uint32_t off = (uint32_t)ri(0, 0x100) & ~1u;
    *(uint32_t*)(g_arena + A_OBJ + 0x34) = off;
    random_bytes(g_arena + A_PACKET, PACKET_N);
    random_floats(g_arena + A_PACKET + off + 4, 0x30, -500, 500);
    if (rnd() % 2) *(uint32_t*)(g_arena + A_OBJ + 0x38) = *(uint16_t*)(g_arena + A_PACKET + off);
    random_bytes(g_arena + A_PHOBDATA, 0x100);
    // the camera
    random_floats(P<void>(0x00553328), 0x30, -1.5f, 1.5f);
    random_floats(P<void>(S_CAM_POS), 12, -500, 500);
    // strings, the user directory, the multiplayer session
    for (int i = 0; i < NSTRINGS; i++) {
        char* s = (char*)g_arena + A_STRINGS + i * STRING_N;
        uint32_t r = rnd() % 8;
        if (r < 3) sprintf(s, "%s.cf", k_cars[rnd() % 6]);
        else if (r < 5) strcpy(s, k_cars[rnd() % 6]);
        else if (r < 7) strcpy(s, k_tracks[rnd() % 9]);
        else { int n = ri(0, 30); for (int k = 0; k < n; k++) s[k] = (char)("abcxyz.\\_"[rnd() % 9]); s[n] = 0; }
    }
    strcpy((char*)g_arena + A_USERDIR, rnd() % 2 ? "C:\\Viper\\" : "D:\\Games\\Viper Racing\\user\\");
    *(void**)(g_arena + A_LIVE + 8) = g_arena + A_SESSION;
    *(void**)(g_arena + A_SESSION) = g_session_vt;
    *(int32_t*)(g_arena + A_SESSION + 4) = ri(0, 8);
    random_bytes(g_arena + A_SESS_OPTS, 0x28);
    *(int32_t*)(g_arena + A_SESS_OPTS + 0x20) = ri(0, 7);
    // statics: the game state, the live session, the driver map, the flags the originals read
    *P<int32_t>(S_GAME_STATE) = ri(0, 8);
    *P<void*>(S_LIVE_MULTI) = rnd() % 3 ? 0 : g_arena + A_LIVE;
    int32_t* map = P<int32_t>(0x0051cba0);
    for (int i = 0; i < 16; i++) map[i] = i;
    if (rnd() % 4 == 0) for (int i = 15; i > 0; i--) { int k = ri(0, i); int32_t t = map[i]; map[i] = map[k]; map[k] = t; }
    *P<int32_t>(0x0051cbe0) = rnd() % 6 ? 16 : ri(0, 16);
    *P<int32_t>(S_AI_STRENGTH) = ri(0, 7);
    *P<uint32_t>(0x004fb454) = rnd() % 3 == 0;                 // MultiEnabled
    *P<uint8_t>(0x00521608) = (uint8_t)(rnd() % 3 == 0);       // PhysicsIsPaused
    *P<uint8_t>(0x004e52e4) = (uint8_t)(rnd() % 2);            // HackHornBall's two flags
    *P<uint8_t>(0x00505b40) = (uint8_t)(rnd() % 2);
    strcpy(P<char>(S_CAR_NAME), k_cars[rnd() % 6]);
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
static Mask g_masks[64];
static int g_nmasks;
static bool g_ret_is_self;
static bool g_ret_is_ptr;                               // the return value is a pointer (compared as given)
static bool g_crashed;
static bool g_trace = getenv("VP_TRACE") != 0;

static void snapshot(uint8_t* to) { memcpy(to, g_data, g_data_n); memcpy(to + g_data_n, g_arena, ARENA_N); }
static void restore(const uint8_t* from) { memcpy(g_data, from, g_data_n); memcpy(g_arena, from + g_data_n, ARENA_N); }
static uint8_t* live(uint32_t i) { return i < g_data_n ? g_data + i : g_arena + (i - g_data_n); }
static uint32_t index_of(const uint8_t* p) { return p >= g_arena && p < g_arena + ARENA_N ? g_data_n + (uint32_t)(p - g_arena) : (uint32_t)(p - g_data); }
static bool masked(const uint8_t* p) {
    for (int k = 0; k < g_nmasks; k++) if (p >= g_masks[k].p && p < g_masks[k].p + g_masks[k].n) return true;
    return false;
}
static void mask(void* p, uint32_t n) { if (g_nmasks < 64) g_masks[g_nmasks++] = {(uint8_t*)p, n}; }
static void mask_setup_junk(uint8_t* setup) { for (uint32_t o : {0x14u, 0x1cu, 0x24u, 0x2cu}) mask(setup + o, 4); }
static void mask_world_setups(World* w) { for (int i = 0; i < 16; i++) mask_setup_junk(w->cars.cars[i].setup); }


static int crash_filter(EXCEPTION_POINTERS* e, bool orig) {
    printf("CRASH in %s (%s, world %d): exception %08lx at %p\n", g_running, orig ? "original" : "rewrite", g_world,
           e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <typename Run> static uint64_t guarded(Run& run, bool orig) {
    __try {
        return run(orig);
    } __except (crash_filter(GetExceptionInformation(), orig)) {
        return 0xdeaddeaddeaddeadull;
    }
}
static void reset_pass(uint32_t seed, int pass) {
    g_script = seed;
    g_heap_next = 0;
    g_calls_menu = g_calls_prerace = g_calls_line = g_calls_sched = 0;
    g_pass = pass;
    g_nlog[pass] = 0;
}

template <typename Run, typename Fp> static void check(const char* name, Run run, Fp footprint, bool expect_crash = false) {
    Stat& st = stat(name);
    st.calls++;
    if (g_trace) printf("[%d] %s\n", g_world, name);
    snapshot(g_start);
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    footprint(g_fp);
    uint32_t seed = rnd() | 1;
    reset_pass(seed, 0);
    g_running = name;
    uint64_t ro = expect_crash ? 0 : guarded(run, true);
    if (expect_crash) {
        __try { ro = run(true); } __except (EXCEPTION_EXECUTE_HANDLER) { ro = 0xdeaddeaddeaddeadull; }
    }
    snapshot(g_after);
    restore(g_start);
    reset_pass(seed, 1);
    uint64_t rn = 0;
    if (expect_crash) {
        __try { rn = run(false); } __except (EXCEPTION_EXECUTE_HANDLER) { rn = 0xdeaddeaddeaddeadull; }
    } else {
        rn = guarded(run, false);
    }
    if (ro == 0xdeaddeaddeaddeadull || rn == 0xdeaddeaddeaddeadull) {
        st.crashes++;
        g_crashed = true;
        if ((ro == 0xdeaddeaddeaddeadull) != (rn == 0xdeaddeaddeaddeadull)) {
            printf("MISMATCH %s (world %d): only the %s crashed\n", name, g_world, ro == 0xdeaddeaddeaddeadull ? "original" : "rewrite");
            st.fails++;
        }
        restore(g_after);
        g_nmasks = 0;
        g_ret_is_self = g_ret_is_ptr = false;
        return;
    }
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
            char w[96];
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
    g_ret_is_self = g_ret_is_ptr = false;
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

static const char* some_name() { return str_at(ri(0, NSTRINGS - 1)); }
static const char* some_car() { return k_cars[rnd() % 6]; }
static const char* some_track() { return k_tracks[rnd() % 9]; }
static const char* some_cf() {
    static char buf[4][32];
    static int k;
    char* b = buf[k++ & 3];
    uint32_t r = rnd() % 10;
    if (r < 7) sprintf(b, "%s.cf", k_cars[rnd() % 6]);
    else if (r < 9) sprintf(b, "%s.CF", k_cars[rnd() % 5]);
    else strcpy(b, "cars\\viper.cf");
    return b;
}

static void run_world() {
    build_world();
    uint8_t* cf = g_arena + A_CF;
    uint8_t* out = g_arena + A_OUT;
    uint8_t* setup = g_arena + A_SETUP;
    uint8_t* obj = g_arena + A_OBJ;
    World* w = rnd() % 2 ? world1() : world2();
    g_crashed = false;
    for (int s = ri(8, 24); s-- && !g_crashed;) {
        w = rnd() % 2 ? world1() : world2();
        switch (rnd() % 48) {
        // carfile.obj
        case 0: case 1: CHECK(CarFileLoad_file, cf, some_cf(), rnd() % 2 ? g_arena + A_UPGRADES : (uint8_t*)0); break;
        case 2: CHECK(apply_upgrade, cf, (const uint8_t*)(g_arena + A_UPGRADE)); break;
        case 3: case 4: case 5: {
            const uint8_t* su = rnd() % 2 ? setup : g_arena + A_SETUP2;
            const double eps = FLT_EPSILON;
            auto fl = [](const uint8_t* p, uint32_t o) { float f; memcpy(&f, p + o, 4); return (double)f; };
            (fabs(fl(cf, 0xe8)) > eps ? cov_gears_cf : cov_gears_setup)++;
            for (uint32_t k = 0; k < 6; k++) (fabs(fl(cf, 0x60 + 4 * k)) > eps ? cov_sel_set : cov_sel_unset)++;
            int32_t s70, gears;
            memcpy(&s70, su + 0x70, 4);
            memcpy(&gears, cf + 0x108, 4);
            if (gears < 7) cov_gear_cut++;
            if (fabs(fl(cf, 0x218)) > eps && s70 == 1) cov_lift1++;
            else if (fabs(fl(cf, 0x220)) > eps && s70 == 2) cov_lift2++;
            else cov_lift_lerp++;
            CHECK(CarFileCombine, out, su, (const uint8_t*)cf);
            break;
        }
        case 6: case 7: CHECK(CarFileLoad_data, out, (const uint8_t*)setup, some_cf(), rnd() % 2 ? g_arena + A_UPGRADES : (uint8_t*)0); break;
        case 8: {
            static const char* const ugs[] = {"viper.ugs", "airhawk.ugs", "VIPER.UGS"};
            g_ret_is_ptr = true;
            CHECK(CarFileGetUpgradeSet, ugs[rnd() % 3]);
            break;
        }
        case 9: CHECK(CarFileForgetUpgradeSet, (void*)(g_arena + A_HEAP + 16 * ri(0, 64))); break;
        case 10: case 11: {
            static const char* const ccs[] = {"viper.ccs", "airhawk.ccs", "default.ccs", "aidef.ccs", "kenyon.ccs", "none.ccs", "willys.ccs"};
            CHECK(CarFileLoadSetupRes, setup, ccs[rnd() % 7]);
            break;
        }
        case 12: case 13: mask_setup_junk(setup); CHECK(CarFileLoadSetup, (CarSetup*)setup, some_car(), some_track()); break;
        case 14: case 15: mask_setup_junk(setup); CHECK(CarFileLoadSetupData, setup, some_car(), some_track()); break;
        case 16: CHECK(CarFileSaveSetup, (CarSetup*)setup, some_car(), some_track()); break;
        case 17: case 18: mask_setup_junk(setup); CHECK(CarFileLoadDefaultSetup, (CarSetup*)setup, some_car(), some_track()); break;
        case 19: CHECK(CarFileMakeDefaultSetup, (CarSetup*)setup); break;
        case 20: CHECK(make_default_setup, setup); break;
        // wob.obj
        case 21: g_ret_is_self = true; CHECK(GraphObject_ctor, (void*)obj, 0); break;
        case 22: CHECK(GraphObject_dtor, (void*)obj, 0); CHECK(GraphObject_IsAlpha, (void*)obj, 0); CHECK(GraphObject_Order, (void*)obj, 0); break;
        case 23: case 24: CHECK(FrameObject_IsVisible, (const uint8_t*)obj, 0); CHECK(FrameObject_GetZ, (const uint8_t*)obj, 0); break;
        case 25: g_ret_is_self = true; CHECK(WorldObject_ctor, obj, 0); break;
        case 26: CHECK(WorldObject_Create, obj, 0, (void*)(g_arena + A_PHOBDATA)); break;
        case 27: case 28: {                                   // (on a WorldObject: another vtable's UpdateMessage is another class's)
            uint32_t vt = *(uint32_t*)obj;
            if (vt == VT_WORLDOBJ || vt == VT_MODEL || vt == VT_WOBBLE) {
                const uint32_t tick = *(const uint16_t*)(g_arena + A_PACKET + *(uint32_t*)(obj + 0x34));
                if (tick != *(uint32_t*)(obj + 0x38)) cov_packet_new++;
                else if (*P<uint8_t>(0x00521608)) cov_packet_paused++;
                else cov_packet_same++;
                CHECK(WorldObject_ProcessPacket, obj, 0, (const uint8_t*)(g_arena + A_PACKET));
            }
            break;
        }
        case 29: CHECK(WorldObject_UpdateMessage, obj, 0, (const uint8_t*)(g_arena + A_PACKET + 2 * ri(0, 0x80))); break;
        case 30: g_ret_is_self = true; CHECK(ModelObject_ctor, obj, 0, (void*)(g_arena + A_PHOBDATA), some_name()); break;
        case 31: CHECK(ModelObject_dtor, obj, 0); break;
        case 32: g_ret_is_self = true; CHECK(WobbleObject_ctor, obj, 0, (const uint8_t*)(g_arena + A_PHOBDATA)); break;
        // game.obj
        case 33: CHECK0(GetGameState); CHECK(SetGameState, ri(-1, 9)); break;
        case 34: case 35: {
            int32_t n = rnd() % 8 ? ri(0, 7) : ri(-2, 15);
            CHECK(GenerateCarList, w, (const char*)some_car(), n);
            break;
        }
        case 36: mask_world_setups(w); CHECK(SetupCars, w); break;
        case 37: CHECK(LoadCars, w); CHECK(UnloadCars, w); break;
        case 38: CHECK(LoadTrack, w); CHECK(UnloadTrack, w); break;
        case 39: {                                            // (a name past 27 characters overruns the original's buffer)
            const char* n = some_name();
            CHECK(LoadCar, some_car());
            if (strlen(n) <= 27) CHECK(UnloadCar, n);
            break;
        }
        case 40: CHECK(apply_to_car_resource, rnd() % 2 ? (ResName_t)0x00419a30 : (ResName_t)0x00419bb0, some_car()); CHECK(get_paint_resource, ri(0, 7)); break;
        case 41: mask_world_setups(w); CHECK(LoadRace, w); CHECK(UnloadRace, w); break;
        case 42: case 43: mask_world_setups(w); CHECK(GameBeginSingle, w); CHECK(GameEndSingle, w); break;
        case 44: mask_world_setups(w); CHECK(GameBeginMulti, w); CHECK(GameEndMulti, w); break;
        case 45: CHECK0(GameDoSingle); break;
        case 46: CHECK0(GameDoMulti); break;
        default: {                                            // a car file with no '.': both write to address 0
            check(name_CarFileLoad_file,
                  [&](bool orig) {
                      auto f = orig ? (decltype(&CarFileLoad_file))(uintptr_t)addr_CarFileLoad_file : &CarFileLoad_file;
                      return invoke(f, cf, "viper", g_arena + A_UPGRADES);
                  },
                  [&](Footprint& fp) { fpof_CarFileLoad_file(fp, cf, "viper", g_arena + A_UPGRADES); }, true);
            g_crashed = false;                                    // (an expected crash: the world goes on)
            break;
        }
        }
    }
}

#ifdef FIX_TESTS
// ==== the fix tests: a single race on a track with no AI line =====================================================
static int g_fix_fail;
#define FIXCHECK(cond, ...)                                                                                     \
    do {                                                                                                        \
        if (!(cond)) {                                                                                          \
            if (g_fix_fail++ < 30) { printf("FIX FAIL: "); printf(__VA_ARGS__); printf("\n"); }                 \
        }                                                                                                       \
    } while (0)
static uint8_t g_loadrace_bytes[5];
static void hook_loadrace(bool on) {                        // as the DLL does: LoadRace's address runs the rewrite
    if (on) patch_jump(0x0040a840, (void*)&LoadRace);
    else memcpy((void*)0x0040a840, g_loadrace_bytes, 5);
}
enum LineCase { L_DEFAULT, L_REVERSED_ONLY, L_NONE, L_EMPTY, L_SOME_NOTES, L_ALL_NOTES };
static const char* const k_case_names[] = {"default.ili + rdefault.ili", "only rdefault.ili", "no line", "a line of no segments",
                                           "some drivers' notes only", "every driver's notes"};
static void set_lines(LineCase c) {
    g_lines.clear();
    const Member* def = find_member("default.ili");
    const Member* rdef = find_member("rdefault.ili");
    if (c == L_DEFAULT) { g_lines.push_back({"default.ili", def->data}); g_lines.push_back({"rdefault.ili", rdef->data}); }
    if (c == L_REVERSED_ONLY) g_lines.push_back({"rdefault.ili", rdef->data});
    if (c == L_EMPTY) {
        std::vector<uint8_t> e(def->data.begin(), def->data.begin() + 12);
        int32_t zero = 0;
        memcpy(&e[8], &zero, 4);
        g_lines.push_back({"default.ili", e});
        g_lines.push_back({"rdefault.ili", e});
    }
}
// the notes a driver has: the names IterateILIname makes for it (k = 0), found by asking the original
static void add_notes_for(int32_t driver, int32_t track, int32_t strength, const char* car) {
    char name[0x100];
    uint8_t rev;
    IterateILIname(name, &rev, 0, driver, track, strength, car);
    g_lines.push_back({name, find_member("default.ili")->data});
}
// does every AI car of the (original's) list find a line with segments? -- decided here without the rewrite:
// IterateILIname's candidates (the original), looked up in the test's own table of lines
static bool every_ai_car_has_line(const World* w) {
    g_quiet = true;
    bool all = true;
    for (int32_t i = 0; i < w->cars.count && i < 16; i++) {
        if (w->cars.cars[i].type != 1) continue;
        const int32_t track = ((int32_t(__cdecl*)(const char*, uint32_t))0x00420dc0)(w->track, w->options.reversed);
        const int32_t driver = ((int32_t(__cdecl*)(int32_t))0x00420ce0)(i);
        bool usable = false;
        char name[0x100];
        uint8_t rev;
        for (int k = 0; (k = IterateILIname(name, &rev, k, driver, track, w->options.strength, w->cars.cars[i].car)) != 5;) {
            const LineRes* hit = 0;
            for (auto& l : g_lines)
                if (!_stricmp(l.name.c_str(), name)) { hit = &l; break; }
            if (hit) {
                int32_t cnt;
                memcpy(&cnt, &hit->data[8], 4);
                usable = cnt > 0;
                break;
            }
        }
        if (!usable) all = false;
    }
    g_quiet = false;
    return all;
}
static void zero_setup_junk(World* w) {
    for (int i = 0; i < 16; i++)
        for (uint32_t o : {0x14u, 0x1cu, 0x24u, 0x2cu}) memset(w->cars.cars[i].setup + o, 0, 4);
}
static void run_fix_tests() {
    typedef uint8_t(__cdecl* Begin_t)(World*);
    const Begin_t GameBeginSingle_orig = (Begin_t)0x0040a3f0;
    memcpy(g_loadrace_bytes, (void*)0x0040a840, 5);
    g_fix_lines = true;
    long runs[6] = {}, dropped[6] = {}, identical[6] = {}, orig_ai[6] = {}, unfixable[6] = {};
    for (int it = 0; it < 6000; it++) {
        build_world();
        const LineCase c = (LineCase)(it % 6);
        World* w = world1();
        set_lines(c);
        const uint32_t seed = rnd() | 1;
        snapshot(g_start);
        // --- the original GameBeginSingle, LoadRace the original ---
        hook_loadrace(false);
        reset_pass(seed, 0);
        g_mustloads = g_aicarsetups = 0;
        uint8_t ro = 0xee;
        __try { ro = GameBeginSingle_orig(w); } __except (EXCEPTION_EXECUTE_HANDLER) { ro = 0xee; }
        if (ro != 1) { restore(g_start); continue; }          // the menu was left: nothing set up
        if (c == L_SOME_NOTES || c == L_ALL_NOTES) {          // the notes of the drivers of the track and strength chosen
            g_quiet = true;
            const int32_t track = ((int32_t(__cdecl*)(const char*, uint32_t))0x00420dc0)(w->track, w->options.reversed);
            for (int32_t d = 0; d < 7; d++)
                if (c == L_ALL_NOTES || d < 2) add_notes_for(d, track, w->options.strength, "viper");
            g_quiet = false;
            restore(g_start);
            reset_pass(seed, 0);
            g_mustloads = g_aicarsetups = 0;
            ro = GameBeginSingle_orig(w);
        }
        runs[c]++;
        const int orig_mustloads = g_mustloads, orig_aics = g_aicarsetups;
        int32_t player_orig = -1, ai_in_orig = 0;
        for (int32_t i = 0; i < w->cars.count && i < 16; i++) {
            if (w->cars.cars[i].type == 0 && player_orig < 0) player_orig = i;
            if (w->cars.cars[i].type == 1) ai_in_orig++;
        }
        if (ai_in_orig) orig_ai[c]++;
        // a single race's list the fix can make a 0-AI race of: the player and AI cars, at most 16 (a shuffled
        // driver map can leave the player out -- GenerateCarList puts it in car AIGetCarForDriver(n) --, more than 15
        // AI cars overrun the list): anything else is left as it is
        const bool fixable = player_orig >= 0 && w->cars.count <= 16;
        if (ai_in_orig && !fixable) unfixable[c]++;
        const bool expect_drop = ai_in_orig > 0 && fixable && !every_ai_car_has_line(w);
        snapshot(g_after);                                    // the original's result
        // --- the fixed GameBeginSingle, LoadRace's address running the fixed rewrite (as in the DLL) ---
        restore(g_start);
        hook_loadrace(true);
        reset_pass(seed, 1);
        g_logf_count = 0;
        g_mustloads = g_aicarsetups = 0;
        const int allocs_before = g_host_allocs;
        uint8_t rn = 0xee;
        __try { rn = GameBeginSingle(w); } __except (EXCEPTION_EXECUTE_HANDLER) { rn = 0xee; }
        hook_loadrace(false);
        FIXCHECK(rn == 1, "case %s: the fixed GameBeginSingle returned %d", k_case_names[c], rn);
        FIXCHECK(g_host_allocs == allocs_before, "case %s: the line check leaked %d allocations", k_case_names[c], g_host_allocs - allocs_before);
        FIXCHECK(s_single_race_world == 0, "the single-race world wasn't cleared");
        World wf = *w, wo = *(const World*)(g_after + g_data_n + A_WORLD);
        zero_setup_junk(&wf);
        zero_setup_junk(&wo);
        if (!expect_drop) {
            // identical to the original: the world, the rest of the arena, the statics, the log; nothing logged
            const bool same_log = g_nlog[0] == g_nlog[1] && !memcmp(g_log[0], g_log[1], 4 * g_nlog[0]);
            const bool same = !memcmp(&wf, &wo, sizeof wf) && !memcmp(g_after, g_data, g_data_n) &&
                              !memcmp(g_after + g_data_n + sizeof(World), g_arena + sizeof(World), ARENA_N - sizeof(World));
            FIXCHECK(same && same_log && g_logf_count == 0,
                     "case %s (%d AI): with a line, the fixed race differs from the original (world/statics %d, log %d, logf %d)",
                     k_case_names[c], ai_in_orig, same, same_log, g_logf_count);
            if (same && same_log && g_logf_count == 0) identical[c]++;
            else if (g_fix_fail < 8) {
                for (int i = 0; i < (g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1]); i++)
                    if (i >= g_nlog[0] || i >= g_nlog[1] || g_log[0][i] != g_log[1][i]) {
                        printf("    log[%d] of %d/%d: original %08x, fixed %08x\n", i, g_nlog[0], g_nlog[1],
                               i < g_nlog[0] ? g_log[0][i] : 0, i < g_nlog[1] ? g_log[1][i] : 0);
                        break;
                    }
            }
        } else {
            dropped[c]++;
            FIXCHECK(wf.cars.count == 1, "case %s: %d cars left (the original had %d)", k_case_names[c], wf.cars.count, wo.cars.count);
            FIXCHECK(wf.cars.cars[0].type == 0, "case %s: car 0 isn't the player", k_case_names[c]);
            // the player's entry as GenerateCarList wrote it (its setup comes later in the scripted stream than in
            // the original, where the AI cars' setups were made first)
            const CarListEntry& a = wo.cars.cars[player_orig];
            const CarListEntry& b = wf.cars.cars[0];
            FIXCHECK(!memcmp(&a, &b, 0x34) && a.driver_index == b.driver_index && a.hornball == b.hornball,
                     "case %s: the player's entry changed", k_case_names[c]);
            FIXCHECK(!memcmp(wf.track, wo.track, sizeof wf.track) && !memcmp(&wf.options, &wo.options, sizeof wf.options),
                     "case %s: the track or the options changed", k_case_names[c]);
            FIXCHECK(g_aicarsetups == 0, "case %s: %d AI cars were set up", k_case_names[c], g_aicarsetups);
            FIXCHECK(g_mustloads == 1, "case %s: %d car resources registered (the original: %d)", k_case_names[c], g_mustloads, orig_mustloads);
            FIXCHECK(g_logf_count == 1, "case %s: logged %d lines", k_case_names[c], g_logf_count);
            FIXCHECK(orig_aics == ai_in_orig, "case %s: the original set up %d of %d AI cars", k_case_names[c], orig_aics, ai_in_orig);
            if (dropped[c] == 1) printf("  (%s) logged: %s\n", k_case_names[c], g_logf_last);
        }
    }
    for (int c = 0; c < 6; c++)
        printf("fix: %-28s %5ld races set up (%ld with AI cars, %ld of them without the player or past 16 cars): "
               "%ld had AI cars dropped, %ld identical to the original\n",
               k_case_names[c], runs[c], orig_ai[c], unfixable[c], dropped[c], identical[c]);
    FIXCHECK(dropped[L_NONE] > 100 && dropped[L_EMPTY] > 100 && dropped[L_SOME_NOTES] > 20, "too few races without a line");
    FIXCHECK(dropped[L_DEFAULT] == 0 && dropped[L_ALL_NOTES] == 0, "a race with a line lost its AI cars");
    FIXCHECK(identical[L_DEFAULT] > 100 && identical[L_ALL_NOTES] > 100, "too few races with a line");

    // LoadRace for another caller (career, network: not GameBeginSingle's world): the original, with no line
    set_lines(L_NONE);
    long other = 0, other_same = 0;
    for (int it = 0; it < 1000; it++) {
        build_world();
        World* w = world1();
        uint32_t seed = rnd() | 1;
        snapshot(g_start);
        reset_pass(seed, 0);
        ((void(__cdecl*)(World*))0x0040a840)(w);
        snapshot(g_after);
        restore(g_start);
        reset_pass(seed, 1);
        g_logf_count = 0;
        if (it % 2) s_single_race_world = world2();              // another world is the single race's
        LoadRace(w);
        s_single_race_world = 0;
        other++;
        // (the player's setup dwords make_default_setup leaves are the stack's own bytes when the generic setup is used)
        zero_setup_junk((World*)(g_after + g_data_n + A_WORLD));
        zero_setup_junk(world1());
        bool same = !memcmp(g_after + g_data_n, g_arena, ARENA_N) && g_nlog[0] == g_nlog[1] &&
                    !memcmp(g_log[0], g_log[1], 4 * g_nlog[0]) && g_logf_count == 0;
        if (!same && getenv("VP_FIXDIFF"))
            for (uint32_t i = 0; i < ARENA_N; i += 4)
                if (memcmp(g_after + g_data_n + i, g_arena + i, 4)) { char b[96]; printf("  run %d: %s\n", it, where(g_arena + i, b)); }
        if (same) other_same++;
        FIXCHECK(same, "LoadRace for another caller changed the race (run %d)", it);
    }
    printf("fix: LoadRace of a world not GameBeginSingle's (career, network), no line: %ld of %ld identical to the original\n", other_same, other);
    // a list that isn't a single race's (a network car, a ghost, two players) under the flag: left alone
    long odd = 0, odd_same = 0;
    for (int it = 0; it < 1000; it++) {
        build_world();
        World* w = world1();
        w->cars.count = ri(2, 8);
        for (int i = 0; i < w->cars.count; i++) w->cars.cars[i].type = 1;
        w->cars.cars[0].type = 0;
        w->cars.cars[ri(1, w->cars.count - 1)].type = rnd() % 3 == 0 ? 0 : rnd() % 2 ? 2 : 3;
        World before = *w;
        s_single_race_world = w;
        g_logf_count = 0;
        drop_ai_without_line(w);
        s_single_race_world = 0;
        odd++;
        if (!memcmp(&before, w, sizeof before) && g_logf_count == 0) odd_same++;
    }
    FIXCHECK(odd == odd_same, "a list with a network car / ghost / second player was changed");
    printf("fix: lists with a network car, a ghost or two players, no line: %ld of %ld left alone\n", odd_same, odd);
    g_fix_lines = false;
}
#endif

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 3000;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_wld_cars.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(0x400000 + ((IMAGE_DOS_HEADER*)0x400000)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".data", 6)) { g_data = (uint8_t*)0x400000 + sec[i].VirtualAddress; g_data_n = sec[i].Misc.VirtualSize; }
    // the real data
    static const char* const car_parts[] = {".cf", ".ccs", ".ugs", 0};
    for (const char* f : {"Viper.car", "airhawk.car", "airhawkr.car", "indyjeep.car", "willys.car"}) load_archive(f, car_parts);
    static const char* const res_parts[] = {".ccs", 0};
    load_archive("common.res", res_parts);
    static const char* const trk_parts[] = {".ccs", "default.ili", 0};
    load_archive("kenyon.trk", trk_parts);
    if (!read_file(std::string(k_install) + "Config\\setups\\limbo.csu", g_csu) || g_csu.size() != 0xd4) {
        printf("can't read Config\\setups\\limbo.csu from the install\n");
        return 2;
    }
    printf("real data: %zu members (", g_members.size());
    for (size_t i = 0; i < g_members.size(); i++) printf("%s%s", i ? " " : "", g_members[i].name.c_str());
    printf(") and limbo.csu\n");
    if (!find_member("viper.cf") || !find_member("viper.ugs") || !find_member("default.ccs") || !find_member("default.ili")) return 2;
    ((void(__cdecl*)())0x004ce150)();                      // the CRT's floating point, as its start-up sets it (in .data)
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
        g_special_pct = g_world % 4 == 0 ? 0 : g_world % 4 == 1 ? 1 : 5;
        run_world();
    }
    unsigned cw;
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    int failed = 0;
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("  %-32s %7ld calls, %7ld changed the world: %s%s%s%s\n", st.name, st.calls, st.changed,
               st.fails ? "DIFFERS" : "identical", st.fp_fails ? "  FOOTPRINT MISSES" : "",
               st.fp_info ? "  (replay_only: writes outside the footprint)" : "", st.crashes ? "  (crashed, both)" : "");
        failed += st.fails || st.fp_fails;
    }
    printf("coverage: PreRaceDo go on / race / solo / unknown %ld / %ld / %ld / %ld (solo with the player moved up %ld); "
           "multi: synchronised %ld, resumed chat %ld, rescheduled a live session %ld; CarFileCombine gears from the car file / "
           "setup %ld / %ld, gears cut %ld, lift preset 1 / 2 / interpolated %ld / %ld / %ld, factors set / unset %ld / %ld; "
           "ProcessPacket new tick / same tick paused / same tick %ld / %ld / %ld\n",
           cov_prerace[0], cov_prerace[1], cov_prerace[2], cov_prerace[3], cov_solo_moved, cov_sync_ok, cov_multi_resume,
           cov_multi_sched_live, cov_gears_cf, cov_gears_setup, cov_gear_cut, cov_lift1, cov_lift2, cov_lift_lerp, cov_sel_set,
           cov_sel_unset, cov_packet_new, cov_packet_paused, cov_packet_same);
    printf("%d worlds; %d of %d functions differ or miss their footprint\n", worlds, failed, g_nstats);
#ifdef FIX_TESTS
    run_fix_tests();
    printf("fix tests: %d failures\n", g_fix_fail);
    if (g_fix_fail) failed++;
#endif
    return failed ? 1 : 0;
}
