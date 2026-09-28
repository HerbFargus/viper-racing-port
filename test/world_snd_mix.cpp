// world_snd_mix.cpp -- the sound mixers (hook/snd_mix.cpp: mixer.obj, softmxr.obj, fastmix.obj, wave.obj) against the
// originals, outside the game (docs/PORTING.md, step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_snd_mix.cpp
//        /Fo%TEMP%\s1c\ /Fe%TEMP%\s1c\world_snd_mix.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//     the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS -- the rewrites as the game builds them
//   run:   world_snd_mix.exe [episodes] [seed] [leaf-iterations]
//          (environment: VP_S1C_VERBOSE=1 prints each episode, =2 each check; VP_S1C_GAME=<dir> the game install the
//          .sfx resources are read from, default ..\game-files\installs\v1.0-RC next to the repository)
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. Everything the group calls outside itself is a stub
// that logs its call and answers from a per-check script: LogReport / LogPanic (which raises, ending the call in both
// passes), MemAlloc (a bump heap in the arena, now and then failing), operator delete, TaskSleep, Win32GetWindow,
// Win32Idle, the options (OptionsGet answers from the script, OptionsSet logs), atexit, the Xlator's lookup, the sound
// resources (SoundResourceGet hands out real .sfx resources read from the game install -- the 4 KB padded data -- or
// mod-like ones: short, odd, zero-length, unpadded and ending at a no-access page), SoundBegin / SoundEnd, the keys
// (KeyDisableAll, ScanHit, ScanDown) and DirectSoundCreate (the import thunk). DirectSound itself is a pair of
// recording fakes: IDirectSound (SetCooperativeLevel, GetCaps, CreateSoundBuffer, Release) and IDirectSoundBuffer
// (GetCaps, GetStatus, GetCurrentPosition, Lock / Unlock over a real ring in the arena, Play, Stop, SetFormat,
// Restore, Release), each call logged with its arguments (structures by content, the Unlock with a hash of the bytes
// it releases), each answer scripted: cursors anywhere in the ring, failures, DSERR_BUFFERLOST, Lock's two behaviours
// at the ring's end (real DirectSound: an offset of exactly the size is DSERR_INVALIDPARAM; the emulation wraps it).
// wave.obj's rewrites (step S2) call the audio core (hook/audio_core.h) instead of the vtables: the harness implements
// the core over the SAME fakes -- a handle is the fake object (its vtable is checked, so a bad handle faults as a call
// through it would), each core function runs that object's fake method directly, DirectSoundCreate's stub for
// audio::create -- so both ways log to the one op log, and the original (through the fake COM) and the rewrite
// (through the core) are compared on the op sequence, the answers, the statics and the ring's bytes as before. Each
// check also counts which way every DirectSound op came: none through the core in an original's pass, and in a
// chain's rewrite pass (every rewrite hooked) every one through the core, none through a vtable.
// BagBase, Xlator, dsounderr2str, strncmp and the CRT's ftol run from the image.
//
// Each check runs the original, keeps the arena, the image statics and the log, restores them, runs the rewrite and
// compares the return value, the arena, the statics, the log and any fault / panic; every byte the original changed
// must lie inside the rewrite's footprint unless it's replay_only. A dword that holds a stack address in both passes
// (music_test's SoftMixer lives on its stack) counts as equal. Checks run at 24- or 53-bit precision, a quarter of them
// with the overflow, divide-by-zero and denormal exceptions unmasked (the BG thread's mode). Each is made twice from
// one state: "isolated" (the group's other functions run as the originals) and "chain" (every rewrite of the group
// hooked into the image for the rewrite's pass). Then the leaf loops run long outside the check machinery: fastmix,
// nullmix, fastout, fastout_8, setup_channel (the x87 gains), the setters and max_data_from_cursors, millions of calls
// each on values chosen round the clamps and wraps.
//
// The fixes (port.h: VP_FIX): the plain build defines VP_FAITHFUL and checks the rewrites against the originals as
// above. Built with /DFIX_TESTS it compiles the fixed rewrites and first runs the fix tests: mod-like sounds (unpadded,
// short, empty, ending at a no-access page) played one-shot and looped at every rate through the fixed mixer, which
// must not fault and must mix exactly what the original mixes from the same data with the game's padding added (zeros
// after a one-shot, the data again after a loop); every stock .sfx the same way against the original on itself; a
// missing resource; a failed allocation; the bag's capacity with M1's patch (1024) and without (256). Then the same
// random worlds, kept to ordinary inputs (stock sounds, positions inside the data, the steps SetFrequency allows, no
// failed allocation or missing resource), must all still equal the original.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include <tuple>
#include <type_traits>
#ifndef FIX_TESTS
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
static const bool k_ordinary = false;
#else
static const bool k_ordinary = true;        // the random rounds keep to inputs the fixes don't change
#endif
#define VP_PORT_NEEDS_AUDIO(NEW)          // PORT_FN_AUDIO: no PortFn to mark here (ChainReg below replaces PORT_FN)
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
void Footprint::object(void* obj, const char* what) { add(obj, 0x40, what); }

#include "../hook/snd_mix.cpp"

typedef int Edx;
#define G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define G16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define G32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define HFN(T, a) ((T)(uintptr_t)(a))

// ---- random values ------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x5eed51c1u;
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
static uint32_t hash_bytes(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static uint32_t hash_str(const char* s, size_t max = 0x400) { return s ? hash_bytes(s, strnlen(s, max)) : 0xdead; }
static uint32_t special_bits() {
    switch (rnd() % 14) {
    case 0: return 0;
    case 1: return 0x80000000u;
    case 2: return 0x7f800000u;
    case 3: return 0xff800000u;
    case 4: return 0x7fc00000u | (rnd() & 0x3fffff);
    case 5: return 0xffc00000u | (rnd() & 0x3fffff);
    case 6: return 0x7f800001u | (rnd() & 0x3ffffe);           // signalling NaN
    case 7: return rnd() & 0x807fffffu;                         // denormal
    case 8: return 0x7f7fffffu;
    case 9: return ubits(rf(-1e30f, 1e30f));
    case 10: return ubits(rf(-1e-30f, 1e-30f));
    case 11: return 0x3f800000u + ri(-2, 2);
    case 12: return 0x3ca3d70au + ri(-2, 2);                    // 0.02
    default: return rnd();
    }
}
// a float for a field: usually sensible, now and then anything
static uint32_t field_float(float lo, float hi, int special_pct) {
    if (chance(special_pct)) return special_bits();
    if (chance(8)) return ubits(chance(50) ? lo : hi);
    return ubits(rf(lo, hi));
}

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
    free(file);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_S1C_CHILD", "1");
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
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// the chain: every rewrite of the group hooked into the image (for a rewrite's pass)
struct ChainSave { uint32_t at; uint8_t b[5]; };
static ChainSave g_chain_save[256];
static int g_nchain;
static void chain_patch() {
    g_nchain = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        ChainSave& s = g_chain_save[g_nchain++];
        s.at = r->at;
        memcpy(s.b, (void*)(uintptr_t)r->at, 5);
        patch_jmp(r->at, r->fn);
    }
}
static void chain_unpatch() {
    for (int i = g_nchain - 1; i >= 0; i--) memcpy((void*)(uintptr_t)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_nchain = 0;
}

// ---- the arena -------------------------------------------------------------------------------------------------------
enum {
    ARENA_SIZE = 0x80000,
    OFF_CTRL = 0x0000, OFF_COM = 0x0400, OFF_OUT = 0x0800, OFF_STR = 0x0c00, OFF_MIX = 0x1000, OFF_ITEMS = 0x1400,
    OFF_SOUNDS = 0x2000, OFF_BUF = 0x8000, OFF_RING = 0x10000, RING_MAX = 0x10000, OFF_HEAP = 0x28000,
    HEAP_SIZE = 0x58000,
};
static uint8_t* g_arena;
static uint8_t* at(uint32_t off) { return g_arena + off; }

struct Ctrl {
    uint32_t heap_top, mark, alloc_fail, next_id;
    uint32_t hr_fail;              // pct of DirectSound calls that fail
    uint32_t lost;                 // pct of Lock / Unlock calls that report DSERR_BUFFERLOST
    uint32_t lock_real;            // Lock at offset >= size: 1 DSERR_INVALIDPARAM (real DirectSound), 0 wrap (emulation)
    uint32_t caps_flags;           // DSCAPS dwFlags
    uint32_t ring_size;            // what the buffer's GetCaps / Lock use
    uint32_t status_playing;       // pct GetStatus says playing
    uint32_t play, write;          // GetCurrentPosition's cursors
    uint32_t cursor_step;          // added to both after each GetCurrentPosition
    uint32_t opt_set;              // pct OptionsGet overwrites the value
    uint32_t lookup_hit;           // pct the Xlator lookup finds a string
    uint32_t scan_hits_left;       // ScanHit returns 0 this many more times
    uint32_t scan_down;            // pct ScanDown says held
    uint32_t res_fail;             // pct SoundResourceGet returns 0
    uint32_t create_fail;          // pct DirectSoundCreate fails
    uint32_t bogus_name;           // the fake mixer's name: 0 ok, 1 "Dream 9407, ..."
    uint32_t fake_begin;           // the fake mixer's Begin result
};
static Ctrl* ctrl() { return (Ctrl*)at(OFF_CTRL); }

// the image statics the group reads or writes, snapshotted with the arena
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_globals[] = {
    {0x00578a30, 0x2a0, "mixer / softmxr / wave statics 0x578a30..0x578cd0"},
    {0x004f5930, 8, "effects volume, quality"},
    {0x004f638c, 8, "wave flags"},
    {0x004f65d4, 4, "fastmix first flag"},
    {0x004eb108, 8, "Xlator cookie, report flag"},
};
enum { GLOBALS_BYTES = 0x2a0 + 8 + 8 + 4 + 8 };
static const char* global_name(uint32_t a) {
    struct { uint32_t at, n; const char* nm; } t[] = {
        {0x00578a30, 12, "quality Xlator 1"}, {0x00578a40, 12, "NullMixer name Xlator"}, {0x00578a50, 12, "quality Xlator 2"},
        {0x00578a60, 24, "mixers[]"}, {0x00578a78, 4, "mixer count"}, {0x00578a7c, 4, "default mixer"},
        {0x00578a80, 12, "quality Xlator 0"}, {0x00578a90, 1, "quality guard"}, {0x00578b20, 12, "SoftMixer name Xlator"},
        {0x00578b30, 12, "SoftMixer caps"}, {0x00578b3c, 1, "SoftMixer name guard"}, {0x00578c90, 4, "DirectSound"},
        {0x00578c94, 4, "lock n1"}, {0x00578c98, 4, "lock n2"}, {0x00578c9c, 4, "buffer"}, {0x00578ca0, 4, "buffer size"},
        {0x00578ca8, 18, "WAVEFORMATEX"}, {0x00578cc0, 4, "lock p1"}, {0x00578cc4, 4, "lock p2"}, {0x00578cc8, 4, "write offset"},
        {0x004f5930, 4, "effects volume"}, {0x004f5934, 4, "quality"}, {0x004f638c, 1, "wave ok"}, {0x004f6390, 1, "wave resync"},
        {0x004f65d4, 1, "fastmix first"}, {0x004eb108, 4, "Xlator cookie"},
    };
    for (auto& x : t)
        if (a >= x.at && a < x.at + x.n) return x.nm;
    return "?";
}

// ---- the sound resources -----------------------------------------------------------------------------------------------
struct Res { uint8_t* p; uint32_t bytes; std::string name; bool real; };   // bytes: the payload (data and padding)
static std::vector<Res> g_res;
static std::vector<int> g_safe;              // the ordinary rounds' resources: stock, data longer than a block's reach
static uint8_t* g_res_block;
static uint32_t g_res_used;
static bool load_res_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("  can't open %s\n", path.c_str()); return false; }
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (d.size() < 16 || memcmp(d.data(), "0TSR", 4)) return false;
    uint32_t n, off;
    memcpy(&n, &d[4], 4);
    off = 16 + 36 * n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = &d[16 + 36 * i];
        uint32_t sz;
        memcpy(&sz, e + 24, 4);
        if (!memcmp(e + 16, "0XFS", 4) && off + 8 + sz <= d.size() && g_res_used + sz + 16 < 0x400000) {
            uint8_t* p = g_res_block + g_res_used;
            memcpy(p, &d[off + 8], sz);
            g_res.push_back({p, sz, std::string((const char*)e, strnlen((const char*)e, 16)), true});
            g_res_used = (g_res_used + sz + 15) & ~15u;
        }
        off += sz + 8;
    }
    return true;
}
// a mod-like resource: `samples` 16-bit samples, no padding, ending at a no-access page (an overread faults)
static void add_synthetic(uint32_t samples, bool loop_pad, const char* what) {
    const uint32_t bytes = 0x16 + samples * 2 + (loop_pad ? 0x1000 : 0);
    const uint32_t pages = (bytes + 0xfff) / 0x1000;
    uint8_t* b = (uint8_t*)VirtualAlloc(0, (pages + 1) * 0x1000, MEM_COMMIT, PAGE_READWRITE);
    uint8_t* p = b + pages * 0x1000 - bytes;
    const uint32_t len = samples * 2;
    memcpy(p, &len, 4);
    const uint8_t wfx[18] = {1, 0, 1, 0, 0x22, 0x56, 0, 0, 0x44, 0xac, 0, 0, 2, 0, 16, 0, 0, 0};
    memcpy(p + 4, wfx, 18);
    for (uint32_t i = 0; i < samples + (loop_pad ? 0x800 : 0); i++) {
        int16_t v = (int16_t)(chance(10) ? (chance(50) ? 32767 : -32768) : (int)(rnd() & 0xffff));
        memcpy(p + 0x16 + 2 * i, &v, 2);
    }
    DWORD old;
    VirtualProtect(b + pages * 0x1000, 0x1000, PAGE_NOACCESS, &old);
    VirtualProtect(b, pages * 0x1000, PAGE_READONLY, &old);
    g_res.push_back({p, bytes, what, false});
}
static uint8_t* pick_res() { return k_ordinary ? g_res[g_safe[rnd() % g_safe.size()]].p : g_res[rnd() % g_res.size()].p; }
// res.obj's set list, as the SoftSound constructor's fix walks it (0x4eae40: nodes {next +0x10, toc +0x14, count
// +0x1c}; entries 0x24 {size +0x18, data +0x20: the resource - 8}), holding every resource above; its Multi (0x4eae3c)
// made non-zero (MultiEnter / MultiLeave are stubs)
static uint8_t g_set_node[0x3c];
static std::vector<uint8_t> g_set_toc;
static void build_res_set() {
    g_set_toc.assign(g_res.size() * 0x24, 0);
    for (size_t i = 0; i < g_res.size(); i++) {
        uint8_t* e = &g_set_toc[i * 0x24];
        memcpy(e, g_res[i].name.c_str(), std::min<size_t>(16, g_res[i].name.size()));
        const uint32_t type = 0x53465830, size = g_res[i].bytes | 0x80000000u, data = (uint32_t)(uintptr_t)(g_res[i].p - 8);
        memcpy(e + 0x10, &type, 4);
        memcpy(e + 0x18, &size, 4);
        memcpy(e + 0x20, &data, 4);
    }
    const uint32_t toc = (uint32_t)(uintptr_t)g_set_toc.data(), n = (uint32_t)g_res.size();
    memset(g_set_node, 0, sizeof g_set_node);
    memcpy(g_set_node + 0x14, &toc, 4);
    memcpy(g_set_node + 0x1c, &n, 4);
    G32(0x004eae3c) = 1;
    G32(0x004eae40) = (uint32_t)(uintptr_t)g_set_node;
    for (size_t i = 0; i < g_res.size(); i++) {
        uint32_t len;
        memcpy(&len, g_res[i].p, 4);
        if (g_res[i].real && len > 0x1000) g_safe.push_back((int)i);
    }
}

// ---- the stubs' script and log ---------------------------------------------------------------------------------------
struct LogEntry { uint32_t kind, a[6]; };
enum { LOGN = 1 << 15 };
static LogEntry g_log[2][LOGN];
static int g_nlog[2], g_pass;
static uint32_t g_script[1024];
static int g_si;
static uint32_t script() { return g_script[g_si++ & 1023]; }
static void logn(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0, uint32_t a5 = 0) {
    if (g_nlog[g_pass] < LOGN) g_log[g_pass][g_nlog[g_pass]] = {kind, {a0, a1, a2, a3, a4, a5}};
    g_nlog[g_pass]++;
}
static bool on_stack(const void* p) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    return (uint8_t*)p >= (uint8_t*)tib->StackLimit && (uint8_t*)p < (uint8_t*)tib->StackBase;
}
// a pointer as the log sees it: the arena and the image as they are; the stack as a marker
static uint32_t P(const void* p) { return on_stack(p) ? 0x57ac0000u : (uint32_t)(uintptr_t)p; }
static uint32_t mark(uint32_t salt) {
    Ctrl* c = ctrl();
    c->mark = c->mark * 0x9e3779b1u + salt + (uint32_t)g_nlog[g_pass];
    return c->mark;
}

#define LOG_KINDS(X)                                                                                             \
    X(REPORT) X(PANIC) X(MEMALLOC) X(DELETE) X(SLEEP) X(GETWIN) X(IDLE) X(OPT_GETI) X(OPT_GETF) X(OPT_SETI)       \
    X(OPT_SETF) X(ATEXIT) X(LOOKUP) X(RES_GET) X(RES_FORGET) X(SOUND_BEGIN) X(SOUND_END) X(KEY_DISABLE) X(SCAN_HIT) \
    X(SCAN_DOWN) X(DS_CREATE) X(DS_RELEASE) X(DS_CREATEBUF) X(DS_GETCAPS) X(DS_COOP) X(B_RELEASE) X(B_GETCAPS)      \
    X(B_GETPOS) X(B_STATUS) X(B_LOCK) X(B_PLAY) X(B_SETFORMAT) X(B_STOP) X(B_UNLOCK) X(B_RESTORE) X(COM_BAD)        \
    X(MX_NAME) X(MX_BEGIN) X(MX_END) X(MX_BAD)
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
// messages: the formats' arguments decoded ('s' a string: its pointer and a hash; 'd' a number)
static const char* fmt_kinds(uint32_t fmt) {
    switch (fmt) {
    case 0x004f59d8: case 0x004f59f0: case 0x004f640c: case 0x004f64a8: case 0x004f6500: case 0x004f6568:
    case 0x004f65bc: case 0x004eb3e4: case 0x00503d98: case 0x00503db4: return "s";
    case 0x004f5cac: case 0x004f6480: return "d";
    case 0x004f6394: case 0x004f63e4: return "dd";
    default: return "";
    }
}
static void log_msg(uint32_t kind, const char* fmt, va_list ap) {
    uint32_t a[4] = {0, 0, 0, 0};
    const char* k = fmt_kinds((uint32_t)(uintptr_t)fmt);
    for (int i = 0; k[i] && i < 2; i++) {
        const uint32_t v = va_arg(ap, uint32_t);
        if (k[i] == 's') { a[2 * i] = P((void*)(uintptr_t)v); a[2 * i + 1] = hash_str((const char*)(uintptr_t)v); }
        else a[2 * i] = v;
    }
    logn(kind, P(fmt), a[0], a[1], a[2], a[3]);
}
enum { PANIC_CODE = 0xE0005050, BAD_CODE = 0xE0000BAD };
static void __cdecl st_report(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_msg(L_REPORT, fmt, ap);
    va_end(ap);
}
static void __cdecl st_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_msg(L_PANIC, fmt, ap);
    va_end(ap);
    RaiseException(PANIC_CODE, 0, 0, 0);
}
static void* __cdecl st_memalloc(int n) {
    Ctrl* c = ctrl();
    const uint32_t s = script();
    uint8_t* r = 0;
    if ((s % 100) >= c->alloc_fail && n >= 0 && (uint32_t)n <= HEAP_SIZE && c->heap_top + (uint32_t)n + 8 <= HEAP_SIZE) {
        r = at(OFF_HEAP + c->heap_top);
        c->heap_top = (c->heap_top + (uint32_t)(n ? n : 8) + 7) & ~7u;
        uint32_t m = mark(n);
        for (int i = 0; i < n; i++) { r[i] = (uint8_t)(m >> 24); m = m * 1103515245u + 12345u; }
    }
    logn(L_MEMALLOC, (uint32_t)n, P(r));
    return r;
}
static void __cdecl st_delete(void* p) { logn(L_DELETE, P(p)); }
static void __cdecl st_sleep(int ms) { logn(L_SLEEP, (uint32_t)ms); }
static void* __cdecl st_getwin() { logn(L_GETWIN); return (void*)(uintptr_t)0x00beef00; }
static void __cdecl st_idle() { logn(L_IDLE); }
static void __cdecl st_opt_geti(const char* sec, const char* key, int32_t* v) {
    const uint32_t s = script();
    logn(L_OPT_GETI, P(sec), hash_str(sec), P(key), P(v), (uint32_t)*v);
    if ((s % 100) < ctrl()->opt_set) {
        static const int32_t vals[] = {0, 1, 2, 3, -1, 5, 6, 7, 0x7fffffff, (int32_t)0x80000000};
        *v = (s >> 8) % 3 == 0 ? (int32_t)script() : vals[(s >> 12) % 10];
    }
}
static void __cdecl st_opt_getf(const char* sec, const char* key, uint32_t* v) {
    const uint32_t s = script();
    logn(L_OPT_GETF, P(sec), hash_str(sec), P(key), P(v), *v);
    if ((s % 100) < ctrl()->opt_set) {
        static const uint32_t sp[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00001u, 0x7f800001u,
                                      0x00000001u, 0x807fffffu, 0x3f800000u, 0x3f800001u, 0x7f7fffffu};
        const uint32_t t = script();
        *v = (s >> 8) % 3 == 0 ? ((s >> 10) & 1 ? t : sp[t % 12]) : ubits((float)((s >> 12) % 1000) / 700.0f - 0.2f);
    }
}
static void __cdecl st_opt_seti(const char* sec, const char* key, int32_t v) { logn(L_OPT_SETI, P(sec), hash_str(sec), P(key), (uint32_t)v); }
static void __cdecl st_opt_setf(const char* sec, const char* key, uint32_t v) { logn(L_OPT_SETF, P(sec), hash_str(sec), P(key), v); }
static int __cdecl st_atexit(uint32_t fn) { logn(L_ATEXIT, fn); return 0; }
static const char* __cdecl st_lookup(const char* key) {
    const uint32_t s = script();
    const char* r = (s % 100) < ctrl()->lookup_hit ? (const char*)at(OFF_STR + 0x40 * ((s >> 8) % 4)) : 0;
    logn(L_LOOKUP, P(key), hash_str(key), P(r));
    return r;
}
static uint8_t* g_force_res;                // the fix tests: the resource SoundResourceGet hands out
static uint8_t* __cdecl st_res_get(const char* name) {
    const uint32_t s = script();
    uint8_t* r = (s % 100) < ctrl()->res_fail ? 0
                 : g_force_res                ? g_force_res
                 : k_ordinary                 ? g_res[g_safe[(s >> 8) % g_safe.size()]].p
                                              : g_res[(s >> 8) % g_res.size()].p;
    logn(L_RES_GET, P(name), hash_str(name), P(r));
    return r;
}
static void __cdecl st_res_forget(void* r) { logn(L_RES_FORGET, P(r)); }
static void __cdecl st_multi(int32_t, const char*, int32_t) {}   // MultiEnter / MultiLeave (the fix's set walk)
static uint8_t __cdecl st_sound_begin() { logn(L_SOUND_BEGIN); return 1; }
static void __cdecl st_sound_end() { logn(L_SOUND_END); }
static void __cdecl st_key_disable() { logn(L_KEY_DISABLE); }
static uint8_t __cdecl st_scan_hit(uint32_t k) {
    Ctrl* c = ctrl();
    const uint8_t r = c->scan_hits_left ? (c->scan_hits_left--, 0) : 1;
    logn(L_SCAN_HIT, k & 0xff, r);
    return r;
}
static uint8_t __cdecl st_scan_down(uint32_t k) {
    const uint8_t r = (script() % 100) < ctrl()->scan_down;
    logn(L_SCAN_DOWN, k & 0xff, r);                  // an unsigned char: the caller's upper bytes are garbage
    return r;
}

// ---- DirectSound: recording fakes ------------------------------------------------------------------------------------
struct FakeCom { void** vtbl; uint32_t id, refs, kind; };
static void* g_ds_vtbl[24];
static void* g_buf_vtbl[24];
static FakeCom* com_obj(int i) { return (FakeCom*)at(OFF_COM + 16 * i); }
static int g_next_buf;
static uint32_t ds_hr(uint32_t lost_codes = 0) {
    const uint32_t s = script();
    if (lost_codes && (s % 100) < ctrl()->lost) return 0x88780096u;          // DSERR_BUFFERLOST
    if (((s >> 8) % 100) >= ctrl()->hr_fail) return 0;
    static const uint32_t codes[] = {0x80004005u, 0x88780032u, 0x80070057u, 0x88780078u, 0x8007000eu, 0x88780096u, 1u};
    return codes[(s >> 16) % 7];
}
static long __stdcall st_ds_create(void* guid, FakeCom** out, void* outer) {
    const uint32_t s = script();
    const uint32_t hr = (s % 100) < ctrl()->create_fail ? 0x88780078u : 0;   // DSERR_NODRIVER
    logn(L_DS_CREATE, P(guid), P(out), P(outer), hr);
    *out = hr ? 0 : com_obj(0);
    return (long)hr;
}
static unsigned long __stdcall st_release(FakeCom* self) {
    logn(self->kind ? L_B_RELEASE : L_DS_RELEASE, self->id, --self->refs);
    return self->refs;
}
static long __stdcall st_ds_createbuf(FakeCom* self, const uint32_t* desc, FakeCom** out, void* outer) {
    const uint32_t hr = ds_hr();
    const uint32_t wfx = desc[4];
    logn(L_DS_CREATEBUF, self->id, hash_bytes(desc, 16), P((void*)(uintptr_t)wfx),
         wfx ? hash_bytes((void*)(uintptr_t)wfx, 18) : 0, P(out) ^ P(outer), hr);
    if (!hr) *out = com_obj(1 + (g_next_buf++ & 1));
    return (long)hr;
}
static long __stdcall st_ds_getcaps(FakeCom* self, uint32_t* caps) {
    const uint32_t hr = ds_hr();
    logn(L_DS_GETCAPS, self->id, caps[0], hash_bytes(caps, 0x60), hr);
    if (!hr) {
        uint32_t m = mark(0x60);
        for (int i = 2; i < 0x18; i++) { caps[i] = m; m = m * 1103515245u + 12345u; }
        caps[1] = ctrl()->caps_flags;
    }
    return (long)hr;
}
static long __stdcall st_ds_coop(FakeCom* self, void* hwnd, uint32_t level) {
    const uint32_t hr = ds_hr();
    logn(L_DS_COOP, self->id, P(hwnd), level, hr);
    return (long)hr;
}
static long __stdcall st_b_getcaps(FakeCom* self, uint32_t* bc) {
    const uint32_t hr = ds_hr();
    logn(L_B_GETCAPS, self->id, bc[0], hash_bytes(bc, 0x14), hr);
    if (!hr) { bc[1] = 0x8; bc[2] = ctrl()->ring_size; bc[3] = 0; bc[4] = 0; }
    return (long)hr;
}
static long __stdcall st_b_getpos(FakeCom* self, uint32_t* play, uint32_t* write) {
    const uint32_t hr = ds_hr();
    Ctrl* c = ctrl();
    logn(L_B_GETPOS, self->id, P(play), P(write), hr);
    if (!hr) {
        if (play) *play = c->play;
        if (write) *write = c->write;
        c->play = (c->play + c->cursor_step) % (c->ring_size ? c->ring_size : 1);
        c->write = (c->write + c->cursor_step) % (c->ring_size ? c->ring_size : 1);
    }
    return (long)hr;
}
static long __stdcall st_b_status(FakeCom* self, uint32_t* st) {
    const uint32_t hr = ds_hr();
    const uint32_t s = script();
    logn(L_B_STATUS, self->id, P(st), hr);
    if (!hr) *st = ((s % 100) < ctrl()->status_playing ? 1u : 0u) | (s & 0x1c);
    return (long)hr;
}
static long __stdcall st_b_lock(FakeCom* self, uint32_t off, uint32_t n, void** p1, uint32_t* n1, void** p2, uint32_t* n2, uint32_t fl) {
    Ctrl* c = ctrl();
    uint32_t hr = ds_hr(1);
    const uint32_t size = c->ring_size;
    if (!hr && (n > size || (off >= size && c->lock_real))) hr = 0x80070057u;   // DSERR_INVALIDPARAM
    logn(L_B_LOCK, self->id ^ fl << 8, off, n, P(p1) ^ P(n1) * 3, P(p2) ^ P(n2) * 3, hr);
    if (hr) return (long)hr;
    if (size) off %= size;
    const uint32_t first = size - off < n ? size - off : n;
    *p1 = at(OFF_RING + off);
    *n1 = first;
    if (p2) *p2 = first < n ? at(OFF_RING) : 0;
    if (n2) *n2 = first < n ? n - first : 0;
    return 0;
}
static long __stdcall st_b_play(FakeCom* self, uint32_t r1, uint32_t r2, uint32_t fl) {
    const uint32_t hr = ds_hr();
    logn(L_B_PLAY, self->id, r1, r2, fl, hr);
    return (long)hr;
}
static long __stdcall st_b_setformat(FakeCom* self, const void* wfx) {
    const uint32_t hr = ds_hr();
    logn(L_B_SETFORMAT, self->id, P(wfx), wfx ? hash_bytes(wfx, 18) : 0, hr);
    return (long)hr;
}
static long __stdcall st_b_stop(FakeCom* self) { const uint32_t hr = ds_hr(); logn(L_B_STOP, self->id, hr); return (long)hr; }
static bool in_ring(const void* p, uint32_t n) {
    return (const uint8_t*)p >= at(OFF_RING) && n <= RING_MAX && (const uint8_t*)p + n <= at(OFF_RING) + RING_MAX;
}
static long __stdcall st_b_unlock(FakeCom* self, void* p1, uint32_t n1, void* p2, uint32_t n2) {
    const uint32_t hr = ds_hr(1);
    const uint32_t h1 = in_ring(p1, n1) ? hash_bytes(p1, n1) : 0xbad1;
    const uint32_t h2 = !p2 ? 0 : in_ring(p2, n2) ? hash_bytes(p2, n2) : 0xbad2;
    logn(L_B_UNLOCK, self->id, P(p1) ^ n1 * 31, P(p2) ^ n2 * 31, h1, h2, hr);
    return (long)hr;
}
static long __stdcall st_b_restore(FakeCom* self) { const uint32_t hr = ds_hr(); logn(L_B_RESTORE, self->id, hr); return (long)hr; }
static void __cdecl st_com_bad() { logn(L_COM_BAD); RaiseException(BAD_CODE, 0, 0, 0); }

// ---- the audio core (hook/audio_core.h) over the same fakes -------------------------------------------------------------
// the rewrites' way in: a handle is the fake object (as the real core's handle is its COM object); its vtable is read and
// must be of the kind -- a null or wild handle faults on that read as the original's call through it faults
static long g_core_calls[2];                        // DirectSound ops that came through the core, by pass
static long g_core_total, g_com_total_new;          // over all checks: rewrite passes' ops through the core / a vtable
static FakeCom* core_obj(void* h, void** vtbl) {
    FakeCom* o = (FakeCom*)h;
    if (o->vtbl != vtbl) st_com_bad();
    g_core_calls[g_pass]++;
    return o;
}
namespace audio {
long create(Device* out) { g_core_calls[g_pass]++; return st_ds_create(0, (FakeCom**)out, 0); }
long set_cooperative_level(Device ds, void* hwnd, unsigned long level) { return st_ds_coop(core_obj(ds, g_ds_vtbl), hwnd, level); }
long device_caps(Device ds, _DSCAPS* caps) { return st_ds_getcaps(core_obj(ds, g_ds_vtbl), (uint32_t*)caps); }
long create_buffer(Device ds, const _DSBUFFERDESC* desc, Buffer* out) {
    return st_ds_createbuf(core_obj(ds, g_ds_vtbl), (const uint32_t*)desc, (FakeCom**)out, 0);
}
unsigned long release(Device ds) { return st_release(core_obj(ds, g_ds_vtbl)); }
long set_format(Buffer b, const tWAVEFORMATEX* f) { return st_b_setformat(core_obj(b, g_buf_vtbl), f); }
long buffer_caps(Buffer b, _DSBCAPS* caps) { return st_b_getcaps(core_obj(b, g_buf_vtbl), (uint32_t*)caps); }
long play(Buffer b, unsigned long r1, unsigned long prio, unsigned long flags) { return st_b_play(core_obj(b, g_buf_vtbl), r1, prio, flags); }
long stop(Buffer b) { return st_b_stop(core_obj(b, g_buf_vtbl)); }
long status(Buffer b, unsigned long* s) { return st_b_status(core_obj(b, g_buf_vtbl), (uint32_t*)s); }
long restore(Buffer b) { return st_b_restore(core_obj(b, g_buf_vtbl)); }
long position(Buffer b, unsigned long* play, unsigned long* write) {
    return st_b_getpos(core_obj(b, g_buf_vtbl), (uint32_t*)play, (uint32_t*)write);
}
long lock(Buffer b, unsigned long offset, unsigned long bytes, void** p1, unsigned long* n1, void** p2, unsigned long* n2,
          unsigned long flags) {
    return st_b_lock(core_obj(b, g_buf_vtbl), offset, bytes, p1, (uint32_t*)n1, p2, (uint32_t*)n2, flags);
}
long unlock(Buffer b, void* p1, unsigned long n1, void* p2, unsigned long n2) { return st_b_unlock(core_obj(b, g_buf_vtbl), p1, n1, p2, n2); }
unsigned long release(Buffer b) { return st_release(core_obj(b, g_buf_vtbl)); }
}  // namespace audio
// the DirectSound ops a pass logged (the fakes' calls, whichever way they came)
static long ds_ops(int pass) {
    const int n = g_nlog[pass] < LOGN ? g_nlog[pass] : LOGN;
    long k = 0;
    for (int i = 0; i < n; i++) k += g_log[pass][i].kind >= L_DS_CREATE && g_log[pass][i].kind <= L_B_RESTORE;
    return k;
}

// a fake IMixer for MixerAddMixer (its methods __thiscall: received as __fastcall)
static void* g_mx_vtbl[8];
static const char* __fastcall st_mx_name(FakeCom* self, Edx) {
    logn(L_MX_NAME, self->id);
    return (const char*)at(OFF_STR + 0x100 + 0x40 * (ctrl()->bogus_name & 1));
}
static uint8_t __fastcall st_mx_begin(FakeCom* self, Edx) { logn(L_MX_BEGIN, self->id); return (uint8_t)ctrl()->fake_begin; }
static void __fastcall st_mx_end(FakeCom* self, Edx) { logn(L_MX_END, self->id); }
static void __fastcall st_mx_bad(FakeCom*, Edx) { logn(L_MX_BAD); RaiseException(BAD_CODE, 0, 0, 0); }

static void install_stubs() {
    struct { uint32_t at; void* to; } p[] = {
        {0x00411150, (void*)&st_report}, {0x004112b0, (void*)&st_panic}, {0x004140e0, (void*)&st_memalloc},
        {0x00414390, (void*)&st_delete}, {0x00414de0, (void*)&st_sleep}, {0x00412be0, (void*)&st_getwin},
        {0x00412bf0, (void*)&st_idle}, {0x004713e0, (void*)&st_opt_geti}, {0x00471350, (void*)&st_opt_getf},
        {0x00471430, (void*)&st_opt_seti}, {0x004713a0, (void*)&st_opt_setf}, {0x004ceff0, (void*)&st_atexit},
        {0x0041aef0, (void*)&st_lookup}, {0x00477200, (void*)&st_res_get}, {0x00477270, (void*)&st_res_forget},
        {0x00471cb0, (void*)&st_sound_begin}, {0x00471d90, (void*)&st_sound_end}, {0x00413d90, (void*)&st_key_disable},
        {0x00413100, (void*)&st_scan_hit}, {0x004130e0, (void*)&st_scan_down}, {0x004cccec, (void*)&st_ds_create},
        {0x00415180, (void*)&st_multi}, {0x004151d0, (void*)&st_multi},
    };
    for (auto& x : p) patch_jmp(x.at, x.to);
    for (int i = 0; i < 24; i++) g_ds_vtbl[i] = g_buf_vtbl[i] = (void*)&st_com_bad;
    g_ds_vtbl[2] = (void*)&st_release;
    g_ds_vtbl[3] = (void*)&st_ds_createbuf;
    g_ds_vtbl[4] = (void*)&st_ds_getcaps;
    g_ds_vtbl[6] = (void*)&st_ds_coop;
    g_buf_vtbl[2] = (void*)&st_release;
    g_buf_vtbl[3] = (void*)&st_b_getcaps;
    g_buf_vtbl[4] = (void*)&st_b_getpos;
    g_buf_vtbl[9] = (void*)&st_b_status;
    g_buf_vtbl[11] = (void*)&st_b_lock;
    g_buf_vtbl[12] = (void*)&st_b_play;
    g_buf_vtbl[14] = (void*)&st_b_setformat;
    g_buf_vtbl[18] = (void*)&st_b_stop;
    g_buf_vtbl[19] = (void*)&st_b_unlock;
    g_buf_vtbl[20] = (void*)&st_b_restore;
    for (int i = 0; i < 8; i++) g_mx_vtbl[i] = (void*)&st_mx_bad;
    g_mx_vtbl[0] = (void*)&st_mx_name;
    g_mx_vtbl[3] = (void*)&st_mx_begin;
    g_mx_vtbl[6] = (void*)&st_mx_end;
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
static int g_eno;
static const char* g_phase = "";
static bool g_chain;
static long g_mismatch_total, g_checks;
static bool g_abort;                                     // a pass faulted: the episode's state is gone
static bool g_verbose;
static volatile long g_check_serial;
static volatile const char* g_check_name = "";

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
        {OFF_CTRL, "ctrl"}, {OFF_COM, "com"}, {OFF_OUT, "outputs"}, {OFF_STR, "strings"}, {OFF_MIX, "mixer"},
        {OFF_ITEMS, "bag items"}, {OFF_SOUNDS, "sounds"}, {OFF_BUF, "buffers"}, {OFF_RING, "ring"}, {OFF_HEAP, "heap"},
    };
    int k = 0;
    for (int i = 0; i < (int)(sizeof r / sizeof *r); i++)
        if (off >= r[i].at) k = i;
    *rel = off - r[k].at;
    return r[k].name;
}
// two arenas equal, but a dword that holds a stack address in both (the passes' frames differ) counts as equal
static bool arena_differs(const uint8_t* a, const uint8_t* b, uint32_t* where) {
    if (!memcmp(a, b, ARENA_SIZE)) return false;
    for (uint32_t i = 0; i < ARENA_SIZE; i += 4) {
        if (!memcmp(a + i, b + i, 4)) continue;
        uint32_t x, y;
        memcpy(&x, a + i, 4);
        memcpy(&y, b + i, 4);
        if (on_stack((void*)(uintptr_t)x) && on_stack((void*)(uintptr_t)y)) continue;
        *where = i;
        return true;
    }
    return false;
}

static __declspec(noinline) void stack_fill(uint32_t pat) {
    volatile uint32_t buf[0x4000];
    for (int i = 0; i < 0x4000; i++) buf[i] = pat;
}
static uint32_t g_fault_code, g_fault_at;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    __asm fninit                                       // an unmasked x87 exception is still pending
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
// the BG thread's mode: ExceptDiv0Crashes(1) unmasks overflow, divide-by-zero and denormal
static void fpu_mode(unsigned pc, bool unmask) {
    unsigned cw;
    __asm fninit
    _clearfp();
    _controlfp_s(&cw, pc, _MCW_PC);
    if (unmask) _controlfp_s(&cw, _EM_INEXACT | _EM_UNDERFLOW | _EM_INVALID, _MCW_EM);
}
static void fpu_reset() {
    unsigned cw;
    __asm fninit
    _clearfp();
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
}

// ---- coverage -------------------------------------------------------------------------------------------------------------
#define COVS(X) X(grab_ok) X(grab_wrapped) X(grab_pos_at_end) X(grab_underrun) X(grab_queued_over_T) X(grab_n_zero)      \
    X(grab_resync) X(grab_status_fail) X(grab_not_playing) X(grab_pos_fail) X(grab_lost_relocked) X(grab_lost_fail)    \
    X(grab_lock_fail) X(grab_offset_is_size) X(grab_pos_eq_play) X(grab_write_behind_play) X(release_lost)           \
    X(update_ok) X(update_fail_stops) X(mix_8bit) X(mix_16bit) X(mix_one_shot_ended) X(mix_loop_wrapped) \
    X(mix_muted) X(mix_overread_fault) X(begin_primary) X(begin_secondary) X(begin_fail) X(create_fail)                \
    X(add_bogus) X(add_too_many) X(add_begin_fail) X(add_ok) X(music_notes) X(xlate_called) X(bag_panic) X(null_res)
enum {
#define X(n) C_##n,
    COVS(X)
#undef X
    NCOV
};
static const char* const k_cov_names[] = {
#define X(n) #n,
    COVS(X)
#undef X
};
static long g_cov[NCOV];

// what the original's pass did, from its log
static void cover_from_log(const char* fname) {
    const int n = g_nlog[0] < LOGN ? g_nlog[0] : LOGN;
    const LogEntry* l = g_log[0];
    bool unlocked = false;
    for (int i = 0; i < n; i++) {
        const LogEntry& e = l[i];
        const LogEntry* nx = i + 1 < n ? &l[i + 1] : 0;
        switch (e.kind) {
        case L_B_STATUS:
            if (e.a[2]) g_cov[C_grab_status_fail]++;
            else if (nx && nx->kind == L_B_PLAY) g_cov[C_grab_not_playing]++;
            break;
        case L_B_GETPOS: if (e.a[3]) g_cov[C_grab_pos_fail]++; break;
        case L_B_LOCK:
            if (e.a[5] == 0x88780096u) {
                int j = i + 1;
                while (j < n && l[j].kind != L_B_LOCK && l[j].kind != L_REPORT && l[j].kind != L_B_RESTORE) j++;
                bool relocked = false;
                for (int k = i + 1; k < n && k < i + 6; k++)
                    if (l[k].kind == L_B_LOCK) { relocked = l[k].a[5] == 0; break; }
                if (relocked) g_cov[C_grab_lost_relocked]++;
                else g_cov[C_grab_lost_fail]++;
            } else if (e.a[5] && !(i > 0 && l[i - 1].kind == L_B_PLAY)) {
                g_cov[C_grab_lock_fail]++;
            }
            break;
        case L_B_UNLOCK: unlocked = true; if (e.a[5] == 0x88780096u) g_cov[C_release_lost]++; break;
        case L_B_SETFORMAT: if (!e.a[3]) g_cov[C_begin_primary]++; break;
        case L_DS_CREATE: if (e.a[3]) g_cov[C_create_fail]++; break;
        case L_REPORT:
            switch (e.a[0]) {
            case 0x004f63bc: g_cov[C_begin_secondary]++; break;
            case 0x004f63e4: g_cov[C_begin_fail]++; break;
            case 0x004f59d8: g_cov[C_add_bogus]++; break;
            case 0x004f59f0: g_cov[C_add_too_many]++; break;
            }
            break;
        case L_MX_BEGIN:
            if (nx && nx->kind == L_MX_END) g_cov[C_add_ok]++;
            else if (nx && nx->kind == L_DELETE) g_cov[C_add_begin_fail]++;
            break;
        case L_PANIC: if (e.a[0] == 0x00503d98 || e.a[0] == 0x00503db4) g_cov[C_bag_panic]++; break;
        case L_RES_GET: if (!e.a[2]) g_cov[C_null_res]++; break;
        }
    }
    if (!strcmp(fname, "SoftMixer::Update") && !unlocked) g_cov[C_update_fail_stops]++;
}

template <typename Run, typename Fp> static void check(const char* fname, Run run, Fp footprint) {
    std::string name = fname;
    if (g_chain) name += " [chain]";
    if (g_verbose) printf("  %s\n", name.c_str());
    Stat& st = stat(name);
    st.calls++;
    g_checks++;
    g_check_serial++;
    g_check_name = fname;
    for (int i = 0; i < 1024; i++) g_script[i] = rnd();
    const uint32_t pat = chance(50) ? rnd() : chance(50) ? 0x3f800000u : 0;
    const unsigned pc = chance(50) ? _PC_53 : _PC_24;
    const bool unmask = chance(25);
    save(g_start);
    g_fp.n = 0; g_fp.replay_only = 0; g_fp.pure = false;
    footprint(g_fp);
    if (g_fp.replay_only) st.replay_only++;
    uint64_t ro = 0, rn = 0;
    // the original
    fpu_mode(pc, unmask);
    g_pass = 0; g_nlog[0] = 0; g_si = 0; g_next_buf = 0;
    g_core_calls[0] = g_core_calls[1] = 0;
    auto r0 = [&]() { return run(true); };
    auto r1 = [&]() { return run(false); };
    stack_fill(pat);
    int fo = guarded(r0, &ro);
    const uint32_t fcode0 = g_fault_code, fat0 = g_fault_at;
    fpu_reset();
    if (!g_chain) cover_from_log(fname);
    save(g_after);
    if (memcmp(g_after.arena + sizeof(Ctrl), g_start.arena + sizeof(Ctrl), ARENA_SIZE - sizeof(Ctrl)) ||
        memcmp(g_after.globals, g_start.globals, GLOBALS_BYTES)) st.changed++;
    // the rewrite
    load(g_start);
    g_pass = 1; g_nlog[1] = 0; g_si = 0; g_next_buf = 0;
    fpu_mode(pc, unmask);
    if (g_chain) chain_patch();
    stack_fill(pat);
    int fn = guarded(r1, &rn);
    const uint32_t fcode1 = g_fault_code;
    if (g_chain) chain_unpatch();
    fpu_reset();
    {
        // which way the DirectSound ops came: the original's never through the core; a chain's rewrite pass all of them
        const long ops1 = ds_ops(1);
        g_core_total += g_core_calls[1];
        g_com_total_new += ops1 - g_core_calls[1];
        const bool logs_whole = g_nlog[0] <= LOGN && g_nlog[1] <= LOGN;
        if (logs_whole && (g_core_calls[0] || (g_chain && g_core_calls[1] != ops1))) {
            g_mismatch_total++;
            if (st.fails++ < 4)
                printf("PATH %s (episode %d, %s): the original's pass made %ld DirectSound ops through the core; the rewrite's "
                       "%ld of %ld\n", name.c_str(), g_eno, g_phase, g_core_calls[0], g_core_calls[1], ops1);
        }
    }
    if (fo == 1) st.panics++;
    if (fo == 2 && fcode0 == 0xC0000005 && fat0 >= 0x477090 && fat0 < 0x477100) g_cov[C_mix_overread_fault]++;
    if (fo == 2 || fn == 2) {
        st.faults++;
        g_abort = true;
        if (st.faults <= 2 && g_verbose) printf("  (fault in %s, episode %d, %s: code %08x at %08x)\n", name.c_str(), g_eno, g_phase, fcode0, fat0);
        if (fo != fn || fcode0 != fcode1) {
            g_mismatch_total++;
            if (st.fails++ < 4) printf("MISMATCH %s (episode %d, %s): original %s (%08x at %08x), rewrite %s (%08x)\n", name.c_str(), g_eno,
                                       g_phase, fo == 2 ? "faulted" : fo ? "panicked" : "ran", fcode0, fat0,
                                       fn == 2 ? "faulted" : fn ? "panicked" : "ran", fcode1);
        }
        load(g_after);
        return;
    }
    uint8_t now_globals[GLOBALS_BYTES];
    snap_globals(now_globals);
    int nlog0 = g_nlog[0] < LOGN ? g_nlog[0] : LOGN;
    uint32_t where = 0;
    const bool arena_bad = arena_differs(g_after.arena, g_arena, &where);
    // a returned stack address (none expected) would differ between the passes
    bool bad = fo != fn || (fo == 0 && ro != rn) || arena_bad || memcmp(g_after.globals, now_globals, GLOBALS_BYTES) ||
               g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], sizeof(LogEntry) * nlog0);
    if (bad) {
        g_mismatch_total++;
        if (st.fails++ < 4) {
            printf("MISMATCH %s (episode %d, %s, pc %s%s)%s\n", name.c_str(), g_eno, g_phase, pc == _PC_24 ? "24" : "53",
                   unmask ? ", unmasked" : "", fo != fn ? " (panicked in one pass only)" : "");
            if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
            if (arena_bad) {
                int shown = 0;
                for (uint32_t i = where; i < ARENA_SIZE && shown < 10; i += 4)
                    if (memcmp(g_after.arena + i, g_arena + i, 4)) {
                        uint32_t a, b, s, rel;
                        memcpy(&a, g_after.arena + i, 4); memcpy(&b, g_arena + i, 4); memcpy(&s, g_start.arena + i, 4);
                        const char* r = region_of(i, &rel);
                        printf("  %s+0x%x: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", r, rel, s, a, fbits(a), b, fbits(b));
                        shown++;
                    }
            }
            const uint8_t* pa = g_after.globals;
            const uint8_t* pb = now_globals;
            for (const GRange& g : g_globals) {
                int k = 0;
                for (uint32_t i = 0; i < g.n && k < 6; i++)
                    if (pa[i] != pb[i]) { printf("  global %s differs at %08x: %02x vs %02x\n", global_name(g.at + i), g.at + i, pa[i], pb[i]); k++; }
                pa += g.n; pb += g.n;
            }
            int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
            if (nl > LOGN) nl = LOGN;
            for (int i = 0, k = 0; i < nl && k < 6; i++) {
                LogEntry* a = i < g_nlog[0] ? &g_log[0][i] : 0;
                LogEntry* b = i < g_nlog[1] ? &g_log[1][i] : 0;
                if (a && b && !memcmp(a, b, sizeof *a)) continue;
                k++;
                if (a) printf("  call %d original: %s %08x %08x %08x %08x %08x %08x\n", i, g_log_names[a->kind], a->a[0], a->a[1], a->a[2], a->a[3], a->a[4], a->a[5]);
                if (b) printf("  call %d rewrite:  %s %08x %08x %08x %08x %08x %08x\n", i, g_log_names[b->kind], b->a[0], b->a[1], b->a[2], b->a[3], b->a[4], b->a[5]);
            }
            if (g_nlog[0] != g_nlog[1]) printf("  %d calls vs %d\n", g_nlog[0], g_nlog[1]);
        }
    }
    if (!g_fp.replay_only) {
        bool fpbad = false;
        for (uint32_t i = sizeof(Ctrl); i < ARENA_SIZE && !fpbad; i++) {
            if (g_after.arena[i] == g_start.arena[i] || in_footprint(g_arena + i)) continue;
            // the fakes' own state (COM refcounts), the ring behind a Lock (the Unlock record covers it) and the stubs'
            // heap and control block are theirs, not the function's
            if (i >= OFF_COM && i < OFF_OUT) continue;
            if (i >= OFF_RING && i < OFF_RING + RING_MAX) continue;
            uint32_t rel;
            const char* r = region_of(i, &rel);
            if (st.fp_fails++ < 3) printf("FOOTPRINT %s (episode %d, %s): %s+0x%x changed outside it\n", name.c_str(), g_eno, g_phase, r, rel);
            fpbad = true;
        }
        const uint8_t* pa = g_after.globals;
        const uint8_t* ps = g_start.globals;
        for (const GRange& g : g_globals) {
            for (uint32_t i = 0; i < g.n && !fpbad; i++)
                if (pa[i] != ps[i] && !in_footprint((void*)(uintptr_t)(g.at + i))) {
                    if (st.fp_fails++ < 3) printf("FOOTPRINT %s (episode %d, %s): global %s (%08x) changed outside it\n", name.c_str(), g_eno, g_phase, global_name(g.at + i), g.at + i);
                    fpbad = true;
                }
            pa += g.n; ps += g.n;
        }
        if (fpbad) g_mismatch_total++;
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
#define CB(FN, ...) do { if (g_abort) break; save(g_saved); g_chain = false; CHECK(FN, __VA_ARGS__); if (g_abort) break; load(g_saved); g_chain = true; CHECK(FN, __VA_ARGS__); g_chain = false; } while (0)
#define CB0(FN) do { if (g_abort) break; save(g_saved); g_chain = false; CHECK0(FN); if (g_abort) break; load(g_saved); g_chain = true; CHECK0(FN); g_chain = false; } while (0)

// =====================================================================================================================
// the world
// =====================================================================================================================
static void fill_rand(void* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t*)p)[i] = (uint8_t)rnd(); }
static SoftMixerO* mixer() { return (SoftMixerO*)at(OFF_MIX); }
static Bag* bag() { return (Bag*)at(OFF_MIX + 0x20); }
static SoftSoundO* sound(int i) { return (SoftSoundO*)at(OFF_SOUNDS + 0x40 * i); }
static FakeCom* the_buf() { return com_obj(1); }

static void new_episode() {
    memset(g_arena, 0, ARENA_SIZE);
    fill_rand(at(OFF_RING), RING_MAX);
    fill_rand(at(OFF_BUF), OFF_RING - OFF_BUF);
    Ctrl* c = ctrl();
    c->alloc_fail = chance(70) ? 0 : ri(1, 30);
    c->hr_fail = chance(40) ? 0 : ri(1, 40);
    c->lost = chance(50) ? 0 : ri(1, 30);
    c->lock_real = chance(50);
    c->caps_flags = chance(60) ? 0xf : rnd() & 0x3ff;
    c->status_playing = chance(50) ? 100 : ri(0, 100);
    c->opt_set = ri(0, 100);
    c->lookup_hit = ri(0, 100);
    c->res_fail = chance(90) ? 0 : ri(1, 20);
    c->create_fail = chance(70) ? 0 : ri(1, 60);
    c->bogus_name = chance(20);
    c->fake_begin = chance(70);
    c->scan_down = ri(0, 60);
    for (int i = 0; i < 8; i++) {
        FakeCom* o = com_obj(i);
        o->vtbl = i == 0 ? g_ds_vtbl : i < 3 ? g_buf_vtbl : g_mx_vtbl;
        o->id = (uint32_t)i;
        o->refs = 10;
        o->kind = i == 0 ? 0 : 1;
    }
    // the Xlators' translated strings, and the fake mixers' names
    for (int i = 0; i < 4; i++) sprintf((char*)at(OFF_STR + 0x40 * i), "translated %d %08x", i, rnd());
    strcpy((char*)at(OFF_STR + 0x100), "Software mixer");
    strcpy((char*)at(OFF_STR + 0x140), chance(50) ? "Dream 9407, SAM9407" : "Dream 9407,");
    // statics: junk, then what each area needs
    fill_rand((void*)(uintptr_t)0x00578a30, 0x2a0);
    G32(0x00578a78) = ri(0, 6);                                         // mixer count
    G32(0x00578a7c) = chance(80) ? ri(0, 6) : rnd();
    for (int i = 0; i < 6; i++) G32(0x00578a60 + 4 * i) = (uint32_t)(uintptr_t)com_obj(3 + i % 5);
    G8(0x00578a90) = (uint8_t)(chance(60) ? 7 : rnd() & 7);
    G8(0x00578b3c) = (uint8_t)(chance(60) ? 1 : rnd() & 1);
    G32(0x004eb108) = chance(50) ? 0 : rnd();                           // the Xlator cookie
    for (uint32_t x : {0x00578a30u, 0x00578a40u, 0x00578a50u, 0x00578a80u, 0x00578b20u}) {
        G32(x) = 0x004f5970;                                            // key: a string in the image
        G32(x + 4) = (uint32_t)(uintptr_t)at(OFF_STR + 0x40 * (rnd() % 4));   // its last translation
        G32(x + 8) = chance(60) ? G32(0x004eb108) : rnd();
    }
    G8(0x004eb10c) = (uint8_t)chance(50);
    G32(0x004f5930) = field_float(0.0f, 1.0f, 10);
    G32(0x004f5934) = chance(80) ? ri(0, 2) : rnd();
    G8(0x004f638c) = (uint8_t)chance(50);
    G8(0x004f6390) = (uint8_t)chance(20);
    G8(0x004f65d4) = (uint8_t)chance(50);
    G32(0x00578c90) = (uint32_t)(uintptr_t)com_obj(0);
    G32(0x00578c9c) = (uint32_t)(uintptr_t)com_obj(1);
    static const uint32_t sizes[] = {0x4000, 0x4000, 0x4000, 0x8000, 0x10000, 0x1000, 0x2000};
    uint32_t S = chance(85) ? sizes[rnd() % 7] : chance(50) ? (uint32_t)ri(0x40, 0x10000) & ~3u : (uint32_t)ri(1, 0x10000);
    G32(0x00578ca0) = S;
    c->ring_size = chance(90) ? S : (uint32_t)ri(0x100, RING_MAX) & ~3u;
    static const uint32_t avgs[] = {88200, 44100, 88200, 44100, 176400, 22050, 11025, 0, 49};
    G32(0x00578cb0) = chance(90) ? avgs[rnd() % 9] : rnd() % 400000;
}

// a SoftMixer with a bag of sounds in various states
static void build_mixer(int nsounds) {
    SoftMixerO* m = mixer();
    Bag* b = bag();
    m->vtbl = (void*)(uintptr_t)0x004dd8c0;
    m->eight_bit = (uint8_t)chance(50);
    m->ok = 1;
    m->sounds = b;
    b->items = (void**)at(OFF_ITEMS);
    b->name = (const char*)(uintptr_t)0x004f5c8c;
    b->count = nsounds;
    b->max = 0x100;
    b->panic = 1;
    for (int i = 0; i < nsounds; i++) {
        SoftSoundO* s = sound(i);
        b->items[i] = s;
        uint8_t* r = pick_res();
        uint32_t bytes;
        memcpy(&bytes, r, 4);
        s->vtbl = (void*)(uintptr_t)0x004dd8e0;
        s->len = bytes >> 1;
        s->start = (uint32_t)(uintptr_t)(r + 0x16);
        s->mixer = m;
        s->res = r;
        s->looped = (uint8_t)chance(40);
        s->playing = (uint8_t)(chance(80) ? 1 : chance(80) ? 0 : rnd());
        s->muted = (uint8_t)(chance(80) ? 0 : chance(80) ? 1 : rnd());
        s->vol = field_float(0.0f, 1.0f, 4);
        s->pan = field_float(-1.0f, 1.0f, 4);
        s->ratio = field_float(0.02f, chance(70) ? 2.0f : 16.0f, 4);
        if (k_ordinary) s->ratio = chance(5) ? 0x7fc00000u : field_float(0.02f, chance(70) ? 2.0f : 16.0f, 0);   // (SetFrequency's)
        s->_24 = rnd();
        s->_28 = rnd();
        // the current position: in the data, near its end, at it, or (rarely) past it
        const uint32_t len2 = s->len * 2;
        uint32_t off = chance(60) ? (len2 ? (rnd() % len2) & ~1u : 0) : chance(60) ? (len2 > 0x300 ? len2 - (rnd() % 0x300 & ~1u) : 0) : len2 + (rnd() % 0x100 & ~1u);
        if (k_ordinary && off >= len2) off = len2 - 2;                   // (inside the data)
        s->ch.src = s->start + off;
        s->ch.frac = rnd() & (chance(90) || k_ordinary ? 0xffff : 0xffffffff);
        s->ch.lg = (int32_t)rnd();
        s->ch.rg = (int32_t)rnd();
        s->ch.step = k_ordinary ? rnd() % 0x100001 : rnd();              // (at most 16.0: SetFrequency's clamp)
#ifdef FIX_TESTS
        note_extent(s, r, s->start, s->len);                             // (as the fixed constructor records it)
#endif
    }
}

// cursors for the next WaveGrab: around the write offset, the ring's end, equal, behind
static void set_cursors() {
    Ctrl* c = ctrl();
    const uint32_t S = G32(0x00578ca0);
    const uint32_t rs = c->ring_size ? c->ring_size : 1;
    uint32_t pos = G32(0x00578cc8);
    switch (rnd() % 10) {
    case 0: pos = 0; break;
    case 1: pos = S; break;                                             // a Lock that ended at the ring's end
    case 2: pos = (uint32_t)ri(0, (int)S) & ~3u; break;
    case 3: pos = rnd(); break;
    default: break;                                                     // as the last WaveGrab left it
    }
    if (pos > 0x100000 && chance(80)) pos = (uint32_t)ri(0, (int)S) & ~3u;
    G32(0x00578cc8) = pos;
    const uint32_t lead = chance(50) ? 0x1b8 * (G32(0x00578cb0) >= 88200 ? 2 : 1) : chance(50) ? (uint32_t)ri(0, 0x800) & ~3u : rnd() % rs;
    switch (rnd() % 8) {
    case 0: c->play = pos % rs; break;                                  // play == pos
    case 1: c->play = (pos + rs - (uint32_t)ri(0, 0x400)) % rs; break;  // just behind pos
    case 2: c->play = (pos + (uint32_t)ri(0, 0x400)) % rs; break;       // just ahead (an underrun)
    case 3: c->play = rs - (uint32_t)ri(0, 0x40) % rs; break;           // at the ring's end
    default: c->play = rnd() % rs; break;
    }
    c->play &= chance(90) ? ~3u : ~0u;
    c->write = chance(90) ? (c->play + lead) % rs : rnd() % (rs + 4);
    c->cursor_step = chance(70) ? 0 : (uint32_t)ri(0, 0x200) & ~3u;
}

static void wave_grab_coverage(uint8_t ok) {
    // from the original's outcome
    const uint32_t S = G32(0x00578ca0);
    if (ok) {
        g_cov[C_grab_ok]++;
        if (G32(0x00578cc4)) g_cov[C_grab_wrapped]++;
        if (G32(0x00578cc8) == S) g_cov[C_grab_pos_at_end]++;
    }
}
static void wave_grab_inputs_coverage() {
    Ctrl* c = ctrl();
    const uint32_t S = G32(0x00578ca0);
    uint32_t pos = G8(0x004f6390) ? c->write : G32(0x00578cc8);
    if (G8(0x004f6390)) g_cov[C_grab_resync]++;
    const uint32_t play = c->play, write = c->write;
    uint32_t avail = play >= pos ? play - pos : play - pos + S;
    if ((S >> 1) > avail) { g_cov[C_grab_underrun]++; pos = write; avail = write < play ? play - write : play - write + S; }
    if (play == pos) g_cov[C_grab_pos_eq_play]++;
    if (write < play) g_cov[C_grab_write_behind_play]++;
    const uint32_t queued = play < pos ? pos - play : S - play + pos;
    const uint32_t T = G32(0x00578cb0) / 50 * 2;
    uint32_t n = T < queued ? 0x200 : (avail < T - queued ? avail : T - queued);
    if (T < queued) g_cov[C_grab_queued_over_T]++;
    if (!(n & ~3u)) { g_cov[C_grab_n_zero]++; pos = write; }
    if (pos >= c->ring_size) g_cov[C_grab_offset_is_size]++;
}

static uint8_t* out_ptr(int i) { return at(OFF_OUT + 4 * i); }

// ---- episodes ---------------------------------------------------------------------------------------------------------------
static void wave_episode() {
    g_phase = "wave";
    const int n = ri(20, 80);
    for (int k = 0; k < n && !g_abort; k++) {
        switch (rnd() % 16) {
        default: {                                                      // WaveGrab, the heart
            set_cursors();
            wave_grab_inputs_coverage();
            uint8_t** p1 = (uint8_t**)out_ptr(0);
            uint32_t* n1 = (uint32_t*)out_ptr(1);
            uint8_t** p2 = (uint8_t**)out_ptr(2);
            uint32_t* n2 = (uint32_t*)out_ptr(3);
            CB(WaveGrab_rw, p1, n1, p2, n2);
            if (!g_abort) wave_grab_coverage(G32(0x00578cc0) != 0);
            if (!g_abort && chance(70)) {
                // what the mixer would write, then the release
                const uint32_t a = G32(0x00578cc0), b = G32(0x00578cc4);
                if (in_ring((void*)(uintptr_t)a, G32(0x00578c94))) fill_rand((void*)(uintptr_t)a, G32(0x00578c94));
                if (b && in_ring((void*)(uintptr_t)b, G32(0x00578c98))) fill_rand((void*)(uintptr_t)b, G32(0x00578c98));
                CB0(WaveRelease_rw);
            }
            break;
        }
        case 0: CB(max_data_from_cursors_rw, rnd() % 0x10000, rnd() % 0x10000); break;
        case 1: CB(max_data_from_cursors_rw, rnd(), rnd()); break;
        case 2: CB(setup_wf_rw, chance(80) ? ri(0, 3) : (int32_t)rnd(), chance(80) ? ri(0, 2) : (int32_t)rnd()); break;
        case 3: CB0(WaveRelease_rw); break;
        case 4: {
            G32(0x00578cc0) = (uint32_t)(uintptr_t)at(OFF_RING + (rnd() % 0x8000));
            G32(0x00578c94) = rnd() % 0x2000;
            G32(0x00578cc4) = chance(50) ? 0 : (uint32_t)(uintptr_t)at(OFF_RING);
            G32(0x00578c98) = rnd() % 0x2000;
            CB0(WaveRelease_rw);
            break;
        }
        case 5: {
            const int32_t f = chance(85) ? ri(0, 3) : (int32_t)rnd(), ch = chance(85) ? ri(0, 2) : (int32_t)rnd();
            switch (rnd() % 4) {
            case 0: CB(WaveBegin_rw, f, ch); break;
            case 1: CB(create_primary_buffer_rw, f, ch); break;
            case 2: CB(create_secondary_buffer_rw, f, ch); break;
            default: CB0(WaveEnd_rw); break;
            }
            break;
        }
        }
    }
}

// the ordinary rounds: a block mixed straight from do_run / fastmix / nullmix (no setup_run first, as mix_bytes makes
// it) starts inside a built sound's data, so it and the next setup_run stay where the game's own blocks go
static long g_ordinary_skipped;
static bool ordinary_place(const SoftSoundO* s) {
    bool known = false;
    for (int k : g_safe) known |= g_res[k].p == s->res;
    return known && s->ch.src >= s->start && s->ch.src - s->start < s->len * 2 && s->ch.step <= 0x100000 && s->ch.frac <= 0xffff;
}
static void mix_episode() {
    g_phase = "mix";
    if (k_ordinary) ctrl()->alloc_fail = ctrl()->res_fail = 0;          // (CreateSound's and SoftSound's fixes)
    build_mixer(chance(10) ? 0 : chance(80) ? ri(1, 12) : ri(13, 40));
    SoftMixerO* m = mixer();
    const int n = ri(10, 40);
    for (int k = 0; k < n && !g_abort; k++) {
        const int ns = bag()->count;
        SoftSoundO* s = ns > 0 ? sound(ri(0, ns - 1)) : sound(0);
        switch (rnd() % 20) {
        case 0: case 1: case 2: case 3: {                               // mix_bytes into an output buffer
            uint32_t bytes = chance(70) ? (uint32_t)ri(0, 0x2000) & ~3u : chance(50) ? (uint32_t)ri(0, 0x2000) : (uint32_t)ri(0, 8);
            if (bytes > 0x4000) bytes = 0x4000;
            uint8_t* dst = at(OFF_BUF + (rnd() % 0x100) * 4);
            if (m->eight_bit) g_cov[C_mix_8bit]++; else g_cov[C_mix_16bit]++;
            CB(mix_bytes_rw, m, 0, dst, bytes);
            break;
        }
        case 4: case 5: case 6: {                                       // the whole tick: grab, mix, release
            set_cursors();
            if (m->eight_bit != (G16(0x00578cb6) == 8)) {}
            CB(SoftMixer_Update_rw, m, 0, (const void*)0);
            if (!g_abort) { if (G32(0x00578cc0)) g_cov[C_update_ok]++; }
            break;
        }
        case 7: if (ns > 0) CB(setup_channel_rw, s, 0); break;
        case 8: if (ns > 0) { if (s->looped) g_cov[C_mix_loop_wrapped]++; else g_cov[C_mix_one_shot_ended]++; CB(setup_run_rw, s, 0); } break;
        case 9: if (ns > 0) {
                int32_t blk = chance(90) || k_ordinary ? ri(1, 128) : ri(129, 256);
                if (s->muted) g_cov[C_mix_muted]++;
                if (k_ordinary && !ordinary_place(s)) { g_ordinary_skipped++; break; }
                CB(do_run_rw, s, 0, (void*)at(OFF_BUF), blk);
            }
            break;
        case 10: if (ns > 0) {
                switch (rnd() % 7) {
                case 0: CB(SoftSound_Play_rw, s, 0); break;
                case 1: CB(SoftSound_PlayLooped_rw, s, 0); break;
                case 2: CB(SoftSound_Stop_rw, s, 0); break;
                case 3: CB(SoftSound_Mute_rw, s, 0); break;
                case 4: CB(SoftSound_UnMute_rw, s, 0); break;
                case 5: CB(SoftSound_GetStatus_rw, s, 0); break;
                default: CB(SoftSound_Set3D_rw, s, 0, (const void*)0, (const void*)0); break;
                }
            }
            break;
        case 11: if (ns > 0) {
                switch (rnd() % 3) {
                case 0: CB(SoftSound_SetFrequency_rw, s, 0, field_float(0.0f, 20.0f, 30)); break;
                case 1: CB(SoftSound_SetVolume_rw, s, 0, field_float(-0.5f, 1.5f, 30)); break;
                default: CB(SoftSound_SetPanning_rw, s, 0, field_float(-1.5f, 1.5f, 30)); break;
                }
            }
            break;
        case 12: {                                                      // a sound made, and its end
            const int32_t loc = ri(0, 3);
            CB(SoftMixer_CreateSound_rw, m, 0, (const char*)(uintptr_t)0x004f5cfc, loc);
            if (!g_abort && bag()->count > ns) {
                SoftSoundO* ns_s = (SoftSoundO*)bag()->items[bag()->count - 1];
                if (ns_s && chance(50)) CB(SoftSound_vdtor_rw, ns_s, 0, (uint32_t)(chance(80) ? 1 : ri(0, 3)));
            }
            break;
        }
        case 13: if (ns > 0) CB(SoftMixer_RemoveSound_rw, m, 0, chance(90) ? (void*)s : (void*)sound(45)); break;
        case 14: if (ns > 0 && chance(30)) CB(SoftSound_dtor_rw, s, 0); break;
        case 15: {                                                      // a constructor on a fresh block
            SoftSoundO* t = sound(50 + (int)(rnd() % 8));
            fill_rand(t, 0x40);
            CB(SoftSound_ctor_rw, t, 0, (const char*)(uintptr_t)0x004f5cfc, m);
            break;
        }
        case 16: {                                                      // the effects volume and quality
            switch (rnd() % 4) {
            case 0: CB(MixerSetVolume_rw, field_float(-0.5f, 1.5f, 40)); break;
            case 1: CB0(MixerGetVolume_rw); break;
            case 2: CB(MixerSetQuality_rw, chance(70) ? ri(-1, 3) : (int32_t)rnd()); break;
            default: CB0(MixerGetQuality_rw); break;
            }
            break;
        }
        case 17: if (ns > 0) {
                int32_t blk = ri(1, 128);
                if (k_ordinary && !ordinary_place(s)) { g_ordinary_skipped++; break; }
                MixChannel* ch = &s->ch;
                if (chance(50)) CB(fastmix_rw, ch, (void*)at(OFF_BUF), blk);
                else CB(nullmix_rw, ch, (void*)at(OFF_BUF), (SndCount)blk);
            }
            break;
        default: {
            int32_t cnt = ri(1, 256);
            int32_t* acc = (int32_t*)at(OFF_BUF);
            for (int i = 0; i < cnt; i++) acc[i] = chance(50) ? (int32_t)rnd() : ri(-0x900000, 0x900000);
            if (chance(50)) CB(fastout_rw, (void*)acc, (void*)at(OFF_BUF + 0x1000), (SndCount)cnt);
            else CB(fastout_8_rw, (void*)acc, (void*)at(OFF_BUF + 0x1000), (SndCount)cnt);
            break;
        }
        }
    }
}

static void list_episode() {
    g_phase = "list";
    const int n = ri(10, 40);
    for (int k = 0; k < n && !g_abort; k++) {
        switch (rnd() % 22) {
        case 0: CB(MixerBegin_rw, (uint8_t)chance(50)); break;
        case 1: CB(MixerEnd_rw, (uint8_t)chance(50)); break;
        case 2: {
            FakeCom* fm = com_obj(3 + (int)(rnd() % 5));
            CB(MixerAddMixer_rw, (void*)fm);
            break;
        }
        case 3: CB0(MixerCount_rw); break;
        case 4: CB(MixerGet_rw, chance(90) ? ri(0, 7) : ri(-4, 12)); break;
        case 5: CB(MixerSetDefault_rw, chance(80) ? ri(0, 6) : (int32_t)rnd()); break;
        case 6: CB0(MixerGetDefault_rw); break;
        case 7: CB(SoundLocString_rw, (SndLoc)ri(0, 3)); break;
        case 8: CB(MixerGetQualityString_rw, ri(0, 2)); g_cov[C_xlate_called]++; break;
        case 9: CB(NullMixer_GetName_rw, (void*)com_obj(3), 0); break;
        case 10: CB(NullMixer_CreateSound_rw, (void*)com_obj(3), 0, (const char*)(uintptr_t)0x004f5cfc, ri(0, 3)); break;
        case 11: CB(SoftMixer_GetName_rw, (void*)mixer(), 0); break;
        case 12: CB(SoftMixer_GetCaps_rw, (void*)mixer(), 0); break;
        case 13: {
            SoftMixerO* m = (SoftMixerO*)at(OFF_MIX + 0x80);
            fill_rand(m, sizeof *m);
            CB(SoftMixer_ctor_rw, m, 0);
            break;
        }
        case 14: {
            SoftMixerO* m = (SoftMixerO*)at(OFF_MIX + 0x80);
            m->vtbl = (void*)(uintptr_t)0x004dd8c0;
            m->ok = 1;
            m->sounds = (Bag*)(chance(80) ? (void*)bag() : (void*)0);
            if (chance(50)) CB(SoftMixer_Begin_rw, m, 0);
            else CB(SoftMixer_End_rw, m, 0);
            break;
        }
        case 15: CB0(SoftMixerCreateMixers_rw); break;
        case 16: {
            switch (rnd() % 10) {
            case 0: CB0(mixer_E1_rw); break;
            case 1: CB0(mixer_E2_rw); break;
            case 2: CB0(mixer_E7_rw); break;
            case 3: CB0(mixer_E8_rw); break;
            case 4: CB0(softmxr_E1_rw); break;
            case 5: CB0(softmxr_E2_rw); break;
            case 6: CB0(wave_E1_rw); break;
            case 7: CB0(wave_E2_rw); break;
            case 8: CB0(fastmix_E1_rw); break;
            default: CB0(fastmix_E2_rw); break;
            }
            break;
        }
        case 17: G32(0x004eb108) = rnd(); break;                             // the language moved on
        case 18: G8(0x00578a90) = (uint8_t)(rnd() & 7); G8(0x00578b3c) = (uint8_t)(rnd() & 1); break;
        case 19: G32(0x00578a78) = chance(80) ? ri(0, 6) : ri(0, 8); break;
        default: {                                                      // SoftMixer::Begin and the probe, whole
            build_mixer(ri(0, 4));
            CB0(SoftMixerCreateMixers_rw);
            break;
        }
        }
    }
}

// music_test: the organ, for a few ticks
static void music_episode() {
    g_phase = "music";
    build_mixer(0);
    ctrl()->scan_hits_left = chance(20) ? 0 : (uint32_t)ri(1, 6);
    ctrl()->scan_down = ri(0, 50);
    ctrl()->alloc_fail = chance(80) || k_ordinary ? 0 : ri(1, 10);
    if (k_ordinary) ctrl()->res_fail = 0;
    set_cursors();
    g_cov[C_music_notes] += ctrl()->scan_hits_left;
    CB0(music_test_rw);
}

static void run_episode() {
    g_abort = false;
    new_episode();
    switch (rnd() % 10) {
    case 0: case 1: case 2: wave_episode(); break;
    case 3: case 4: case 5: case 6: mix_episode(); break;
    case 7: case 8: list_episode(); break;
    default: if (chance(40)) music_episode(); else mix_episode(); break;
    }
}

// =====================================================================================================================
// the leaf loops, long: original and rewrite on the same inputs, outside the check machinery
// =====================================================================================================================
struct LeafStat { const char* name; long long calls, bad; };
static long long g_fix_leaf;                        // (FIX_TESTS: setup_run calls in the fix's case, checked apart)
static LeafStat g_leaf[] = {{"fastmix", 0, 0}, {"nullmix", 0, 0}, {"fastout", 0, 0}, {"fastout_8", 0, 0},
                            {"setup_channel", 0, 0}, {"SoftSound::SetFrequency", 0, 0}, {"SoftSound::SetVolume", 0, 0},
                            {"MixerSetVolume", 0, 0}, {"max_data_from_cursors", 0, 0}, {"setup_run", 0, 0},
                            {"setup_wf", 0, 0}, {"SoftSound::GetStatus", 0, 0}};
static int32_t acc_value() {
    switch (rnd() % 12) {
    case 0: return 0x7fffff + ri(-2, 2);
    case 1: return -0x800000 + ri(-2, 2);
    case 2: return 0x7fffffff - ri(0, 2);
    case 3: return (int32_t)0x80000000 + ri(0, 2);
    case 4: return (int32_t)rnd();
    case 5: return 0;
    default: return ri(-0xa00000, 0xa00000);
    }
}
static int32_t gain_value() {
    switch (rnd() % 10) {
    case 0: return 0;
    case 1: return 0xffff;
    case 2: return 0x10000;
    case 3: return (int32_t)rnd();
    case 4: return -ri(0, 0x20000);
    case 5: return 0x7fffffff;
    case 6: return (int32_t)0x80000000;
    default: return ri(0, 0xffff);
    }
}
static uint32_t step_value() {
    switch (rnd() % 8) {
    case 0: return 0x10000;
    case 1: return 0x100000;
    case 2: return 0x51e;                                                // 0.02
    case 3: return rnd();
    case 4: return 0;
    default: return (uint32_t)ri(0x51e, 0x100000);
    }
}
static void leaf_bad(int k, const char* what) {
    if (g_leaf[k].bad++ < 3) printf("LEAF MISMATCH %s: %s\n", g_leaf[k].name, what);
}
typedef void(__cdecl* Mix_t)(void*, void*, int32_t);
static void leaf_runs(long long iterations) {
    static int16_t src[0x10000 + 0x8000];
    static int32_t acc0[0x800], acc1[0x800];
    static uint16_t out0[0x810], out1[0x810];
    for (int i = 0; i < (int)(sizeof src / 2); i++)
        src[i] = (int16_t)(chance(5) ? (chance(50) ? 32767 : -32768) : chance(5) ? ri(-2, 2) : (int)(rnd() & 0xffff));
    unsigned cw;
    g_check_name = "the leaf loops";
    for (long long it = 0; it < iterations; it++) {
        g_check_serial++;                                // the watchdog: still going
        const unsigned pc = (it & 1) ? _PC_24 : _PC_53;
        _controlfp_s(&cw, pc, _MCW_PC);
        // fastmix / nullmix
        {
            MixChannel c0, c1;
            c0.src = (uint32_t)(uintptr_t)&src[ri(0, 0x10000)];
            c0.lg = gain_value();
            c0.rg = gain_value();
            c0.step = step_value();
            if (c0.step > 0x100000 && chance(95)) c0.step &= 0xfffff;
            c0.frac = chance(90) ? rnd() & 0xffff : rnd();
            const int32_t n = chance(90) ? ri(1, 128) : ri(129, 0x3ff);
            // the reads stay inside src: at most n * step / 65536 samples on
            const uint64_t reach = ((uint64_t)(c0.frac) + (uint64_t)c0.step * (uint64_t)n) >> 16;
            if ((uint8_t*)(uintptr_t)c0.src + reach * 2 + 4 > (uint8_t*)(src + sizeof src / 2)) c0.src = (uint32_t)(uintptr_t)src;
            if (reach > 0x8000) { c0.step &= 0xffff; }
            c1 = c0;
            for (int i = 0; i < 2 * n; i++) acc0[i] = acc_value();
            memcpy(acc1, acc0, 8 * n);
            G8(0x004f65d4) = (uint8_t)chance(50);
            const uint8_t f0 = G8(0x004f65d4);
            ((Mix_t)0x00477090)(&c0, acc0, n);
            const uint8_t fa = G8(0x004f65d4);
            G8(0x004f65d4) = f0;
            fastmix_rw(&c1, acc1, n);
            g_leaf[0].calls++;
            if (memcmp(&c0, &c1, sizeof c0) || memcmp(acc0, acc1, 8 * n) || fa != G8(0x004f65d4)) leaf_bad(0, "state or accumulator");
            c1 = c0;
            MixChannel c2 = c0;
            ((Mix_t)0x00477100)(&c1, acc0, n);
            nullmix_rw(&c2, acc0, (SndCount)n);
            g_leaf[1].calls++;
            if (memcmp(&c1, &c2, sizeof c1)) leaf_bad(1, "state");
        }
        // fastout / fastout_8
        {
            const int32_t n = chance(90) ? ri(1, 256) : ri(257, 0x7ff);
            for (int i = 0; i < n; i++) acc0[i] = acc_value();
            for (int i = 0; i < n + 2; i++) out0[i] = out1[i] = (uint16_t)rnd();
            ((Mix_t)0x00477140)(acc0, out0, n);
            fastout_rw(acc0, out1, (SndCount)n);
            g_leaf[2].calls++;
            if (memcmp(out0, out1, 2 * (n + 2))) leaf_bad(2, "output");
            ((Mix_t)0x00477190)(acc0, out0, n);
            fastout_8_rw(acc0, out1, (SndCount)n);
            g_leaf[3].calls++;
            if (memcmp(out0, out1, 2 * (n + 2))) leaf_bad(3, "output");
        }
        // setup_channel: the x87 gains (masked exceptions here; the checks cover the unmasked mode)
        {
            SoftSoundO s0;
            memset(&s0, 0, sizeof s0);
            s0.vol = field_float(0.0f, 1.0f, 15);
            s0.pan = chance(10) ? ubits(chance(50) ? -0.0f : 0.0f) : field_float(-1.0f, 1.0f, 15);
            if (chance(10)) s0.pan = ubits(rf(-1e6f, 1e6f));
            s0.ratio = field_float(0.02f, 16.0f, 15);
            s0.ch.lg = (int32_t)rnd();
            G32(0x004f5930) = field_float(0.0f, 1.0f, 15);
            SoftSoundO s1 = s0;
            HFN(void(__fastcall*)(void*, Edx), 0x00474f20)(&s0, 0);
            setup_channel_rw(&s1, 0);
            g_leaf[4].calls++;
            if (memcmp(&s0, &s1, sizeof s0)) {
                char b[160];
                sprintf(b, "vol %08x pan %08x ratio %08x ev %08x: %08x %08x %08x vs %08x %08x %08x", s0.vol, s0.pan, s0.ratio,
                        G32(0x004f5930), s0.ch.lg, s0.ch.rg, s0.ch.step, s1.ch.lg, s1.ch.rg, s1.ch.step);
                leaf_bad(4, b);
            }
        }
        // the setters, on bit patterns
        {
            SoftSoundO s0, s1;
            memset(&s0, 0, sizeof s0);
            memset(&s1, 0, sizeof s1);
            const uint32_t v = chance(50) ? special_bits() : rnd();
            HFN(void(__fastcall*)(void*, Edx, uint32_t), 0x00475280)(&s0, 0, v);
            SoftSound_SetFrequency_rw(&s1, 0, v);
            g_leaf[5].calls++;
            if (memcmp(&s0, &s1, sizeof s0)) leaf_bad(5, "ratio");
            HFN(void(__fastcall*)(void*, Edx, uint32_t), 0x004752c0)(&s0, 0, v);
            SoftSound_SetVolume_rw(&s1, 0, v);
            g_leaf[6].calls++;
            if (memcmp(&s0, &s1, sizeof s0)) leaf_bad(6, "volume");
            HFN(void(__cdecl*)(uint32_t), 0x004739e0)(v);
            const uint32_t e0 = G32(0x004f5930);
            MixerSetVolume_rw(v);
            g_leaf[7].calls++;
            if (e0 != G32(0x004f5930)) leaf_bad(7, "effects volume");
        }
        // max_data_from_cursors, setup_run, setup_wf, GetStatus
        {
            G32(0x00578ca0) = chance(50) ? 0x4000 : rnd();
            const uint32_t a = chance(50) ? rnd() % 0x4001 : rnd(), b = chance(50) ? rnd() % 0x4001 : chance(20) ? a : rnd();
            g_leaf[8].calls++;
            if (HFN(uint32_t(__cdecl*)(uint32_t, uint32_t), 0x00476ff0)(a, b) != max_data_from_cursors_rw(a, b)) leaf_bad(8, "value");
            SoftSoundO s0;
            fill_rand(&s0, sizeof s0);
            if (chance(50)) s0.ch.src = s0.start + (s0.len * 2 + (uint32_t)ri(-4, 4));
            s0.playing = (uint8_t)(chance(70) ? 1 : chance(50) ? 0 : rnd());
            SoftSoundO s1 = s0;
            const SoftSoundO in = s0;
            HFN(void(__fastcall*)(void*, Edx), 0x00474fd0)(&s0, 0);
            setup_run_rw(&s1, 0);
            g_leaf[9].calls++;
            // the fix's case: a playing loop at or past its end that one step back leaves outside its data
            const uint32_t tw = in.len + in.len;
            if (k_ordinary && in.playing && in.looped && !(in.start + tw > in.ch.src) && in.ch.src - tw - in.start >= tw) {
                g_fix_leaf++;
                if (s1.ch.src != (tw ? in.start + (in.ch.src - in.start) % tw : in.start)) leaf_bad(9, "the fix's step back into the loop");
                s1.ch.src = s0.ch.src;
            }
            if (memcmp(&s0, &s1, sizeof s0)) leaf_bad(9, "state");
            g_leaf[11].calls++;
            if (HFN(int32_t(__fastcall*)(void*, Edx), 0x00475240)(&s0, 0) != SoftSound_GetStatus_rw(&s1, 0)) leaf_bad(11, "status");
            const int32_t f = chance(70) ? ri(-1, 4) : (int32_t)rnd(), ch = chance(70) ? ri(-1, 3) : (int32_t)rnd();
            uint8_t w0[18], junk[18];
            fill_rand(junk, 18);
            memcpy((void*)(uintptr_t)0x00578ca8, junk, 18);
            HFN(void(__cdecl*)(int32_t, int32_t), 0x00476b80)(f, ch);
            memcpy(w0, (void*)(uintptr_t)0x00578ca8, 18);
            memcpy((void*)(uintptr_t)0x00578ca8, junk, 18);
            setup_wf_rw(f, ch);
            g_leaf[10].calls++;
            if (memcmp(w0, (void*)(uintptr_t)0x00578ca8, 18)) leaf_bad(10, "WAVEFORMATEX");
        }
    }
    _controlfp_s(&cw, _PC_53, _MCW_PC);
}

// =====================================================================================================================
// the fixes (built with /DFIX_TESTS)
// =====================================================================================================================
#ifdef FIX_TESTS
static int g_fix_checks, g_fix_fail;
static void expect(bool ok, const char* what, const char* res = "", int a = 0, int b = 0) {
    g_fix_checks++;
    if (!ok && g_fix_fail++ < 40) printf("  FIX FAILED: %s (%s, %d, %d)\n", what, res, a, b);
}
template <typename F> static uint32_t fguard(F f) {                 // 0, or the exception (LogPanic's included)
    unsigned cw;
    uint32_t r = 0;
    __asm fninit
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    __try { f(); } __except (fault_filter(GetExceptionInformation())) { r = g_fault_code; }
    fpu_reset();
    return r;
}
typedef void(__fastcall* MixBytes_t)(void*, Edx, uint8_t*, uint32_t);
typedef void*(__fastcall* Ctor_t)(void*, Edx, const char*, void*);
typedef void*(__fastcall* Create_t)(void*, Edx, const char*, int32_t);
typedef uint8_t(__fastcall* Begin_t)(void*, Edx);

// a clean world: the SoftMixer with an empty bag (256), effects volume 1, nothing failing
static void fix_world(bool eight) {
    new_episode();
    Ctrl* c = ctrl();
    c->alloc_fail = c->hr_fail = c->lost = c->res_fail = c->create_fail = 0;
    c->caps_flags = 0xf;
    c->status_playing = 100;
    SoftMixerO* m = mixer();
    Bag* b = bag();
    m->vtbl = (void*)(uintptr_t)0x004dd8c0;
    m->eight_bit = (uint8_t)eight;
    m->ok = 1;
    m->sounds = b;
    b->items = (void**)at(OFF_ITEMS);
    b->name = (const char*)(uintptr_t)0x004f5c8c;
    b->count = 0;
    b->max = 0x100;
    b->panic = 1;
    G32(0x004f5930) = 0x3f800000;
    g_pass = 0;
    g_nlog[0] = 0;
    g_force_res = 0;
}
// sound 0 on resource r, in the bag: made by the fixed constructor (its extent noted) or set up as the original's
// constructor leaves it; then playing (looped or not) from byte `off` of its data at `ratio`
static SoftSoundO* one_sound(uint8_t* r, bool ctor, bool looped, uint32_t ratio, uint32_t off) {
    SoftSoundO* s = sound(0);
    memset(s, 0, sizeof *s);
    if (ctor) {
        g_force_res = r;
        SoftSound_ctor_rw(s, 0, (const char*)(uintptr_t)0x004f5cfc, mixer());
        g_force_res = 0;
    } else {
        uint32_t bytes;
        memcpy(&bytes, r, 4);
        s->vtbl = (void*)(uintptr_t)0x004dd8e0;
        s->mixer = mixer();
        s->res = r;
        s->start = (uint32_t)(uintptr_t)(r + 0x16);
        s->len = bytes >> 1;
        s->vol = 0x3f800000;
        s->ratio = 0x3f800000;
    }
    bag()->items[0] = s;
    bag()->count = 1;
    s->ratio = ratio;
    s->playing = 1;
    s->looped = (uint8_t)looped;
    s->ch.src = s->start + off;
    s->ch.frac = 0;
    return s;
}
enum { MIX_CALLS = 12, MIX_BYTES = 0x1000 };
static uint8_t g_out0[MIX_CALLS * MIX_BYTES], g_out1[MIX_CALLS * MIX_BYTES];
static uint32_t run_mix(bool chain, uint8_t* out, int calls = MIX_CALLS) {
    memset(out, 0xcd, (size_t)calls * MIX_BYTES);
    if (chain) chain_patch();
    const uint32_t e = fguard([&] {
        for (int k = 0; k < calls; k++) ((MixBytes_t)0x00475040)(mixer(), 0, out + k * MIX_BYTES, MIX_BYTES);
    });
    if (chain) chain_unpatch();
    return e;
}
// resource r with the game's padding carried on for 1 MB: its payload as it is, then silence (a one-shot) or its data
// again from the start (a loop) -- what the fixed mixer reads past r's end
static uint8_t* g_twin;
enum { TWIN_EXTRA = 0x100000 };
static uint8_t* make_twin(const Res& R, bool looped) {
    uint32_t bytes;
    memcpy(&bytes, R.p, 4);
    const uint32_t twice = bytes & ~1u, have = R.bytes - 0x16;
    memcpy(g_twin, R.p, R.bytes);
    uint8_t* d = g_twin + 0x16;
    for (uint32_t k = have; k < have + TWIN_EXTRA; k++) d[k] = looped && twice ? d[k % twice] : 0;
    return g_twin;
}

// a sound's reads stay inside its resource; what it mixes is the original's with the game's padding
static void fix_overread() {
    static const uint32_t ratios[] = {0x3ca3d70a, 0x3f000000, 0x3f800000, 0x3fc00000, 0x40000000, 0x40e9999a, 0x41800000};
    int runs = 0, orig_faults = 0, orig_runs = 0, short_loops = 0;
    for (size_t i = 0; i < g_res.size(); i++) {
        const Res& R = g_res[i];
        uint32_t bytes;
        memcpy(&bytes, R.p, 4);
        const uint32_t twice = bytes & ~1u;
        const bool padded = R.bytes - 0x16 >= twice + 0x1000;             // the game's 4096 bytes after the data
        for (int looped = 0; looped < 2; looped++)
            for (uint32_t ratio : ratios)
                for (int eight = 0; eight < 2; eight++)
                    for (int near_end = 0; near_end < 2; near_end++) {
                        const uint32_t off = near_end && twice > 0x3000 ? (twice - 0x3000 + (rnd() & 0xffe)) & ~1u : 0;
                        runs++;
                        const char* nm = R.name.c_str();
                        if (padded && (!looped || twice > 4066)) {
                            // a padded sound, as the game's: the original's bits exactly
                            fix_world(eight != 0);
                            one_sound(R.p, false, looped != 0, ratio, off);
                            const uint32_t e0 = run_mix(false, g_out0);
                            const SoftSoundO s0 = *sound(0);
                            fix_world(eight != 0);
                            one_sound(R.p, true, looped != 0, ratio, off);
                            const uint32_t e1 = run_mix(true, g_out1);
                            const SoftSoundO s1 = *sound(0);
                            expect(!e0 && !e1, "padded: both run", nm, looped, (int)ratio);
                            expect(!memcmp(g_out0, g_out1, sizeof g_out0), "padded: the original's output", nm, looped, (int)ratio);
                            expect(!memcmp(&s0, &s1, sizeof s0), "padded: the original's sound state", nm, looped, (int)ratio);
                        } else if (padded) {
                            // a stock sound too short to loop (a one-shot's data played looped): the original reads on
                            // past its padding; the fixed one stays inside
                            short_loops++;
                            fix_world(eight != 0);
                            SoftSoundO* s = one_sound(R.p, true, true, ratio, off);
                            const uint32_t e1 = run_mix(true, g_out1);
                            expect(!e1 && s->ch.src - s->start < twice + 0x1000, "a short stock loop: runs, stays in its resource", nm, 0, (int)ratio);
                        } else {
                            // a mod-like sound: the original on its data with the padding carried on
                            fix_world(eight != 0);
                            one_sound(make_twin(R, looped != 0), false, looped != 0, ratio, off);
                            const uint32_t e0 = run_mix(false, g_out0);
                            const SoftSoundO s0 = *sound(0);
                            fix_world(eight != 0);
                            one_sound(R.p, true, looped != 0, ratio, off);
                            const uint32_t e1 = run_mix(true, g_out1);
                            const SoftSoundO s1 = *sound(0);
                            expect(!e0, "the twin: the original runs", nm, looped, (int)ratio);
                            expect(!e1, "unpadded: the fixed mixer runs", nm, looped, (int)ratio);
                            expect(!memcmp(g_out0, g_out1, sizeof g_out0), "unpadded: the original's output with the padding", nm, looped, (int)ratio);
                            const uint32_t o0 = s0.ch.src - s0.start, o1 = s1.ch.src - s1.start;
                            expect(s0.playing == s1.playing && s0.muted == s1.muted && s0.ch.frac == s1.ch.frac && s0.ch.step == s1.ch.step &&
                                       s0.ch.lg == s1.ch.lg && s0.ch.rg == s1.ch.rg,
                                   "unpadded: the original's state", nm, looped, (int)ratio);
                            expect(looped ? (twice ? o0 % twice == o1 % twice : true) : o0 == o1, "unpadded: the same place (in the loop)", nm, looped, (int)ratio);
                            if (looped) expect(o1 < twice + 0x1000 || !twice, "unpadded: a loop stays in its data", nm, (int)o1, (int)ratio);
                            // and the original on the unpadded data itself
                            fix_world(eight != 0);
                            one_sound(R.p, false, looped != 0, ratio, off);
                            orig_runs++;
                            if (run_mix(false, g_out0) == 0xC0000005) orig_faults++;
                        }
                    }
    }
    printf("  over-reads: %d runs (%d stock sounds looped too short to loop); the original faulted on %d of %d unpadded runs\n",
           runs, short_loops, orig_faults, orig_runs);
}

// a missing resource: an empty, silent sound
static void fix_missing_resource() {
    for (int eight = 0; eight < 2; eight++) {
        fix_world(eight != 0);
        ctrl()->res_fail = 100;
        SoftSoundO* s = sound(0);
        memset(s, 0, sizeof *s);
        const uint32_t e0 = fguard([&] { ((Ctor_t)0x00475160)(s, 0, (const char*)(uintptr_t)0x004f5cfc, mixer()); });
        expect(e0 == 0xC0000005, "the original's constructor faults on a missing resource");
        memset(s, 0, sizeof *s);
        const uint32_t e1 = fguard([&] { SoftSound_ctor_rw(s, 0, (const char*)(uintptr_t)0x004f5cfc, mixer()); });
        expect(!e1 && !s->res && !s->len && s->vtbl == (void*)(uintptr_t)0x004dd8e0, "the fixed constructor: an empty sound");
        bag()->items[0] = s;
        bag()->count = 1;
        for (int looped = 0; looped < 2; looped++) {
            if (looped) SoftSound_PlayLooped_rw(s, 0); else SoftSound_Play_rw(s, 0);
            s->ratio = 0x41800000;
            const uint32_t e = run_mix(true, g_out1, 3);
            bool silent = true;
            for (int k = 0; k < 3 * MIX_BYTES; k++) silent &= g_out1[k] == (eight ? 0x80 : 0);
            expect(!e && silent, "mixed: silence", "", eight, looped);
            expect(s->playing == looped, "a one-shot stops, a loop plays on", "", eight, looped);
        }
        const int n0 = g_nlog[0];
        const uint32_t e2 = fguard([&] { SoftSound_dtor_rw(s, 0); });
        bool forgot = false;
        for (int k = n0; k < g_nlog[0] && k < LOGN; k++) forgot |= g_log[0][k].kind == L_RES_FORGET;
        expect(!e2 && !forgot && bag()->count == 0, "the destructor: out of the bag, no resource to forget");
    }
}

// a failed allocation: nothing added to the bag
static void fix_failed_alloc() {
    fix_world(false);
    ctrl()->alloc_fail = 100;
    void* r = (void*)1;
    uint32_t e = fguard([&] { r = ((Create_t)0x00474e30)(mixer(), 0, (const char*)(uintptr_t)0x004f5cfc, 3); });
    expect(!e && !r && bag()->count == 1 && !bag()->items[0], "the original adds a null sound");
    e = run_mix(false, g_out0, 1);
    expect(e == 0xC0000005, "and the original's mix faults on it");
    fix_world(false);
    ctrl()->alloc_fail = 100;
    r = (void*)1;
    e = fguard([&] { r = SoftMixer_CreateSound_rw(mixer(), 0, (const char*)(uintptr_t)0x004f5cfc, 3); });
    expect(!e && !r && bag()->count == 0, "the fixed one returns null and adds nothing");
    e = run_mix(true, g_out1, 1);
    expect(!e, "and its mix runs");
}

// the bag's capacity: M1's operand (0x474d79), 256 stock, 1024 lifted
static void fix_bag_capacity() {
    const uint32_t stock = G32(0x00474d79);
    for (int m1 = 0; m1 < 2; m1++) {
        const int32_t cap = m1 ? 0x400 : 0x100;
        G32(0x00474d79) = (uint32_t)cap;                                 // as M1 patches it
        fix_world(false);
        SoftMixerO* m0 = (SoftMixerO*)at(OFF_MIX + 0x80);
        SoftMixerO* m = (SoftMixerO*)at(OFF_MIX + 0xc0);
        memset(m0, 0, sizeof *m0);
        memset(m, 0, sizeof *m);
        SoftMixer_ctor_rw(m0, 0);
        SoftMixer_ctor_rw(m, 0);
        uint8_t ok0 = 0, ok1 = 0;
        uint32_t e = fguard([&] { ok0 = ((Begin_t)0x00474d40)(m0, 0); });
        expect(!e && ok0 && m0->sounds && m0->sounds->max == cap, "the original's Begin: the operand's capacity", "", m1, 0);
        e = fguard([&] { ok1 = SoftMixer_Begin_rw(m, 0); });
        expect(!e && ok1 && m->sounds && m->sounds->max == cap, "the rewrite's Begin: the operand's capacity", "", m1, 0);
        if (e || !ok1 || !m->sounds) continue;
        chain_patch();
        int made = 0;
        e = fguard([&] {
            for (int k = 0; k < cap; k++) {
                if (!((Create_t)0x00474e30)(m, 0, (const char*)(uintptr_t)0x004f5cfc, 3)) break;
                made++;
            }
        });
        chain_unpatch();
        expect(!e && made == cap && m->sounds->count == cap, "CreateSound up to the capacity", "", m1, made);
        for (int k = 0; k < cap; k += 7) {
            SoftSoundO* s = (SoftSoundO*)m->sounds->items[k];
            s->playing = 1;
            s->looped = (uint8_t)(k & 1);
        }
        chain_patch();
        e = fguard([&] { ((MixBytes_t)0x00475040)(m, 0, g_out1, MIX_BYTES); });
        chain_unpatch();
        expect(!e, "mixing them all", "", m1, 0);
        chain_patch();
        e = fguard([&] { ((Create_t)0x00474e30)(m, 0, (const char*)(uintptr_t)0x004f5cfc, 3); });
        chain_unpatch();
        expect(e == PANIC_CODE, "one past the capacity: the bag's panic, as before", "", m1, 0);
    }
    G32(0x00474d79) = stock;
}

static void fix_tests() {
    g_twin = (uint8_t*)VirtualAlloc(0, 0x400000 + TWIN_EXTRA, MEM_COMMIT, PAGE_READWRITE);
    g_abort = false;
    fix_overread();
    fix_missing_resource();
    fix_failed_alloc();
    fix_bag_capacity();
    g_force_res = 0;
    printf("fixes: %d expectations, %d failed\n", g_fix_checks, g_fix_fail);
}
#endif

// the watchdog: a check that runs for more than a minute is reported and the harness ends itself
static DWORD WINAPI watchdog(void*) {
    long last = -1, same = 0;
    for (;;) {
        Sleep(1000);
        const long now = g_check_serial;
        same = now == last ? same + 1 : 0;
        last = now;
        if (same >= 60) {
            printf("HUNG in %s (episode %d, %s, pass %d, %s): a check ran for a minute\n", (const char*)g_check_name, g_eno, g_phase,
                   g_pass, g_chain ? "chain" : "isolated");
            fflush(stdout);
            ExitProcess(6);
        }
    }
}
static LONG WINAPI unhandled(EXCEPTION_POINTERS* e) {
    printf("UNHANDLED exception %08x at %08x (episode %d, %s)\n", (unsigned)e->ExceptionRecord->ExceptionCode,
           (unsigned)(uintptr_t)e->ExceptionRecord->ExceptionAddress, g_eno, g_phase);
    fflush(stdout);
    ExitProcess(4);
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_S1C_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    SetUnhandledExceptionFilter(unhandled);
    CreateThread(0, 0, watchdog, 0, 0, 0);
    const int episodes = argc > 1 ? atoi(argv[1]) : 3000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const long long leaf = argc > 3 ? atoll(argv[3]) : 2000000;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_snd_mix.cpp");
    if (!s) { printf("build with /FC\n"); return 2; }
    std::string root(exe, s);
    strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_stubs();
    // the game's .sfx resources, read-only (the mixer only reads them)
    std::string game = getenv("VP_S1C_GAME") ? getenv("VP_S1C_GAME") : root + "\\..\\game-files\\installs\\v1.0-RC";
    g_res_block = (uint8_t*)VirtualAlloc(0, 0x400000 + 0x1000, MEM_COMMIT, PAGE_READWRITE);
    for (const char* f : {"race.res", "common.res", "viper.car", "ui.res", "exotic.car", "plane.car", "sedan.car", "sports.car"}) load_res_file(game + "\\" + f);
    const size_t nreal = g_res.size();
    if (!nreal) { printf("no .sfx resources found under %s\n", game.c_str()); return 2; }
    DWORD old;
    VirtualProtect(g_res_block, 0x400000, PAGE_READONLY, &old);
    // mod-like resources: short, odd, empty, unpadded against a no-access page; one padded loop
    for (uint32_t n : {0u, 1u, 2u, 17u, 100u, 127u, 128u, 1000u, 2047u, 2048u, 4096u})
        add_synthetic(n, false, "synthetic");
    add_synthetic(3000, true, "synthetic padded");
    printf("%d .sfx resources from %s, %d mod-like\n", (int)nreal, game.c_str(), (int)(g_res.size() - nreal));
    build_res_set();
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE + 0x1000, MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) return 2;
    VirtualProtect(g_arena + ARENA_SIZE, 0x1000, PAGE_NOACCESS, &old);
    uint32_t gb = 0;
    for (const GRange& g : g_globals) gb += g.n;
    if (gb != GLOBALS_BYTES) { printf("GLOBALS_BYTES is %u, should be %u\n", (unsigned)GLOBALS_BYTES, gb); return 2; }
    int nreg = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) nreg++;
    printf("%d rewrites registered\n", nreg);
#ifdef FIX_TESTS
    fix_tests();
#endif
    const bool verbose = GetEnvironmentVariableA("VP_S1C_VERBOSE", 0, 0) != 0;
    g_verbose = verbose && getenv("VP_S1C_VERBOSE")[0] == '2';
    for (g_eno = 0; g_eno < episodes; g_eno++) {
        if (verbose) printf("episode %d\n", g_eno);
        run_episode();
    }
    int failed = 0;
    long calls = 0;
    printf("%-52s %7s %7s %7s %7s %7s %7s %7s\n", "function", "calls", "changed", "replay", "panics", "faults", "differ", "fp-miss");
    std::vector<Stat> sorted = g_stats;
    std::sort(sorted.begin(), sorted.end(), [](const Stat& a, const Stat& b) { return a.name < b.name; });
    for (Stat& st : sorted) {
        printf("%-52s %7ld %7ld %7ld %7ld %7ld %7ld %7ld\n", st.name.c_str(), st.calls, st.changed, st.replay_only, st.panics,
               st.faults, st.fails, st.fp_fails);
        if (st.fails || st.fp_fails) failed++;
        calls += st.calls;
    }
    int unchecked = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        bool seen = false;
        for (Stat& st : g_stats) if (st.name == r->name) seen = true;
        if (!seen) { printf("  never checked: %s\n", r->name); unchecked++; }
    }
    printf("%d episodes, %ld checks; %d of %d functions differ or escape their footprint; %d rewrites never checked\n", episodes, calls,
           failed, (int)g_stats.size(), unchecked);
    printf("the rewrites' DirectSound ops: %ld through the audio core, %ld through a vtable (isolated checks: the group's other "
           "functions run as the originals)\n", g_core_total, g_com_total_new);
    printf("covered:");
    for (int i = 0; i < NCOV; i++) printf("%s %s %ld", i ? "," : "", k_cov_names[i], g_cov[i]);
    printf("\n");
    leaf_runs(leaf);
    long long lcalls = 0, lbad = 0;
    for (auto& l : g_leaf) {
        printf("  leaf %-26s %12lld calls, %lld differ\n", l.name, l.calls, l.bad);
        lcalls += l.calls;
        lbad += l.bad;
    }
    printf("leaf loops: %lld calls, %lld differ\n", lcalls, lbad);
    if (k_ordinary) printf("(ordinary rounds: stock sounds over 4 KB, positions inside the data, steps up to 16.0, no failed allocation or "
                           "missing resource; %ld direct blocks from outside the data left out; %lld setup_run leaf calls in the fix's "
                           "case checked against it apart)\n", g_ordinary_skipped, g_fix_leaf);
#ifdef FIX_TESTS
    if (g_fix_fail) return 1;
#endif
    return failed || lbad ? 1 : 0;
}
