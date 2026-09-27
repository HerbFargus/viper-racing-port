// world_krn_res.cpp -- group K4 (hook/krn_res.cpp: res.obj, locale.obj, opt.obj, tmetry.obj) against the originals,
// outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_krn_res.cpp
//        /Fo%TEMP%\k4w\ /Fe%TEMP%\k4w\world_krn_res.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//        /LARGEADDRESSAWARE
//   run:   world_krn_res.exe [rounds] [seed] [game dir]
//          (the game dir defaults to ..\game-files\installs\v1.0-RC beside the repository; it is only ever READ)
//
// Loads out\race_v10.exe at 0x400000 in a child process (the range reserved before its heap exists), resolves
// KERNEL32's imports into the game's own import slots, and puts logging stubs in the slots these functions use
// (GetLocaleInfoA, GetCurrencyFormatA, GetDateFormatA, GetFullPathNameA -- each forwards to Windows, or answers from
// a scripted fake locale -- and a CreateFileA that refuses everything, since nothing may reach the disk that way).
//
// Everything the functions call outside the group is a stub that logs its call: the Multi locks, LogReport /
// LogPanic (which, as in the game, doesn't return: it raises an exception that ends the call, in both passes),
// MemAlloc (a bump heap inside the arena, the original's 0xa3 fill and size head; now and then a null) and operator
// delete (the 0xa3 fill), ASSERT_MSG and OneShot (their arguments logged), and the file layer as a virtual file
// system: FileOpen / Close / ReadExact / Size / Read (the game's own FileReadLine runs over it) / Create / Write /
// FindFirst / FindNext / FindClose, with the original's 32-slot file table and its panics (a short read panics, as
// FileReadExact does), and FileCreateMemoryMap as a read-only copy of the file in a region of its own (a destroyed
// map becomes inaccessible, as an unmapped view). The files: the game directory's real files, read READ-ONLY from
// disk (every .res / .trk / .car / .tra / .btr / .bak archive, english.lng, trk2.sfx, options.def,
// Config\options.cfg), and synthetic ones that exist only in memory: resource sets of every shape (empty, preload
// 0 / all / beyond the count, 15- and 16-character entry names, duplicate names, long set names, bad magic,
// truncated headers, TOC blocks too small, payload heads that aren't zero), "0SER" file resources (in the resource
// directory and the user directory, with long names), languages (many, long names, bad versions, unsorted keys),
// options files (wrong versions, long lines / sections / keys / values, more than 256 items, LF-only lines).
// Nothing is written anywhere: the options file OptionsFlush writes lives in the arena.
//
// Each check: the arena (all of the heap, the stubs' state, the arguments; write-watched, so only the pages a pass
// wrote are saved and compared) and the image statics (res, locale, opt, tmetry) are kept, the ORIGINAL runs, the
// state is put back, the REWRITE runs, and the return value, the stubs' logs (every call, in order, with its
// arguments), the state and any fault / panic are compared; every byte the original changed must lie inside the
// rewrite's footprint unless it is replay_only. Then the original's result is kept and the scenario carries on.
// "isolated": the rewrite alone (its callees the originals); "chain": every rewrite of the group patched into the
// image for the rewrite's pass (ASSERT_MSG and OneShot excepted: their logging stubs stay; they are checked alone).
// No dialog can appear: SetErrorMode in the parent, the game CRT's fatal-message routines patched to exit.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>
#include <type_traits>
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#include "../hook/port.h"

struct ChainReg {
    uint32_t at; void* fn; const char* name; ChainReg* next;
    static ChainReg*& head() { static ChainReg* h; return h; }
    ChainReg(uint32_t a, void* f, const char* n) : at(a), fn(f), name(n), next(head()) { head() = this; }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN) static ChainReg VP_CAT(chain_, NEW)(V10, (void*)&NEW, NAME);

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

#include "../hook/krn_res.cpp"

// ---- random values ------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static uint32_t hash_bytes(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((uint8_t)c);
    return s;
}
static std::string rname(int len) {                                  // a name of letters, digits, '_' and '.'
    static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-";
    std::string s;
    for (int i = 0; i < len; i++) s += cs[rnd() % (sizeof cs - 1)];
    return s;
}
static std::string recase(std::string s) {
    for (char& c : s)
        if (chance(30)) c = (char)(isupper((uint8_t)c) ? tolower((uint8_t)c) : toupper((uint8_t)c));
    return s;
}

// ---- the original, loaded at 0x400000 -------------------------------------------------------------------------------
struct IatSlot { std::string name; uint32_t at; };
static std::vector<IatSlot> g_iat;
static uint32_t iat_slot(const char* name) {
    for (auto& s : g_iat)
        if (s.name == name) return s.at;
    return 0;
}
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
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize);
    // KERNEL32's imports resolved into the game's own slots (the CRT reaches GetStringTypeA, GetFullPathNameA ...)
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
            g_iat.push_back({fn, (uint32_t)(uintptr_t)&iat->u1.Function});
            FARPROC p = GetProcAddress(m, fn);
            if (p) iat->u1.Function = (DWORD)(uintptr_t)p;
        }
    }
    free(file);
    return true;
}
static int relaunch() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);   // (inherited by the child)
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
    if (code > 4) printf("the test process ended with 0x%08lx\n", code);
    return code > 4 ? 5 : (int)code;
}
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the arena: write-watched; the shadow holds its content as of the last check ---------------------------------
enum : uint32_t {
    PG = 4096, ARENA_SIZE = 176u << 20, NPAGES = ARENA_SIZE / PG,
    ARGS_OFF = 0x8000, ARGS_CAP = 0x10000,                 // a check's arguments and outputs
    METER_OFF = 0x18000,                                   // telemetry meters (floats that persist)
    WF_OFF = 0x20000, WF_CAP = 0x40000, NWF = 6,           // the writable files' contents
    HEAP_OFF = WF_OFF + NWF * WF_CAP,
    MAP_SIZE = 400u << 20,                                 // the memory maps' region (not in the arena)
};
static uint8_t* g_ar;
static uint8_t* g_sh;
static uint8_t* g_map;
static ULONG_PTR* g_ww;
static std::vector<uint32_t> ww_take() {                    // the pages written since the last call, in order
    ULONG_PTR n = NPAGES;
    DWORD gran = 0;
    std::vector<uint32_t> v;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_ar, ARENA_SIZE, (PVOID*)g_ww, &n, &gran)) {
        printf("GetWriteWatch failed (%lu)\n", GetLastError());
        ExitProcess(4);
    }
    v.reserve(n);
    for (ULONG_PTR i = 0; i < n; i++) v.push_back((uint32_t)((g_ww[i] - (ULONG_PTR)g_ar) / PG));
    std::sort(v.begin(), v.end());
    return v;
}
static void ww_sync() {                                     // what the harness wrote: into the shadow
    for (uint32_t p : ww_take()) memcpy(g_sh + (size_t)p * PG, g_ar + (size_t)p * PG, PG);
}
static uint32_t aoff(const void* p) {                       // an arena pointer as an offset (logs); else its value
    uintptr_t a = (uintptr_t)p;
    if (a >= (uintptr_t)g_ar && a < (uintptr_t)g_ar + ARENA_SIZE) return 0xa0000000u | (uint32_t)(a - (uintptr_t)g_ar);
    if (a >= (uintptr_t)g_map && a < (uintptr_t)g_map + MAP_SIZE) return 0xb0000000u | (uint32_t)(a - (uintptr_t)g_map);
    return (uint32_t)a;
}
static bool in_arena(const void* p, size_t n) {
    return (uintptr_t)p >= (uintptr_t)g_ar && (uintptr_t)p + n <= (uintptr_t)g_ar + ARENA_SIZE;
}

// the stubs' state, in the arena (so it is saved, restored and compared with everything else)
struct Slot { int32_t file, pos, open; };
struct View { uint32_t off, len, prot; };
struct Ctrl {
    uint32_t heap_top, allocs, null_at;
    int32_t next_multi;
    int32_t nslots;
    Slot slot[32];                                           // file.obj's MAX_FILES
    int32_t wf_len[NWF];                                     // -1: the file doesn't exist
    uint32_t map_top, nviews;
    View views[512];
    int32_t find_pos, nlng;
    char lng[24][0x90];                                      // what FindFirstFile("*.lng") lists
    uint8_t joy, locale_fake, currency_fake, _p;
    int32_t megs;
    char fake_lang[0x40];
    char fake_measure, fake_decimal, fake_mon, _q;
};
static_assert(sizeof(Ctrl) <= ARGS_OFF, "Ctrl");
static Ctrl* C() { return (Ctrl*)g_ar; }

static uint32_t g_args_top;
static void args_reset() { g_args_top = 0; }
static void* arg_buf(uint32_t n, int fill = 0) {
    n = (n + 15) & ~15u;
    if (g_args_top + n > ARGS_CAP) { printf("argument area full\n"); ExitProcess(4); }
    uint8_t* p = g_ar + ARGS_OFF + g_args_top;
    g_args_top += n;
    memset(p, fill, n);
    return p;
}
static char* arg_str(const std::string& s) {
    char* p = (char*)arg_buf((uint32_t)s.size() + 1);
    memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// ---- the virtual files ----------------------------------------------------------------------------------------------
struct VFile { std::string key, disk; std::vector<uint8_t> mem; bool loaded = false; int wf = -1; bool bad = false; };
static std::vector<VFile> g_vfs;
static int vfs_find(const char* path) {
    std::string k = lower(path);
    for (size_t i = 0; i < g_vfs.size(); i++)
        if (g_vfs[i].key == k) return (int)i;
    return -1;
}
static int vfs_put(const std::string& path, VFile v) {
    v.key = lower(path);
    int i = vfs_find(path.c_str());
    if (i >= 0) { g_vfs[i] = std::move(v); return i; }
    g_vfs.push_back(std::move(v));
    return (int)g_vfs.size() - 1;
}
static int vfs_disk(const std::string& path, const std::string& disk) { VFile v; v.disk = disk; return vfs_put(path, std::move(v)); }
static int vfs_mem(const std::string& path, std::vector<uint8_t> d) { VFile v; v.mem = std::move(d); v.loaded = true; return vfs_put(path, std::move(v)); }
static int vfs_wf(const std::string& path, int wf) { VFile v; v.wf = wf; v.loaded = true; return vfs_put(path, std::move(v)); }
static bool read_disk(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");                    // read-only
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t got = n ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}
static bool vfs_data(int i, const uint8_t** d, uint32_t* n) {  // the file's bytes (a writable one's live in the arena)
    if (i < 0 || i >= (int)g_vfs.size()) return false;
    VFile& v = g_vfs[i];
    if (v.wf >= 0) {
        int32_t len = C()->wf_len[v.wf];
        if (len < 0) return false;
        *d = g_ar + WF_OFF + v.wf * WF_CAP;
        *n = (uint32_t)len;
        return true;
    }
    if (!v.loaded) {
        if (!read_disk(v.disk, v.mem)) return false;
        v.loaded = true;
    }
    *d = v.mem.data();
    *n = (uint32_t)v.mem.size();
    return true;
}
static bool vfs_exists(int i) {
    if (i < 0) return false;
    if (g_vfs[i].wf >= 0) return C()->wf_len[g_vfs[i].wf] >= 0;
    return true;
}
static void vfs_unload_disk() {                             // between rounds: the disk files' bytes go
    for (VFile& v : g_vfs)
        if (!v.disk.empty() && v.loaded) { std::vector<uint8_t>().swap(v.mem); v.loaded = false; }
}

// ---- the stubs' log -----------------------------------------------------------------------------------------------------
enum {
    K_REPORT = 0x4b000001, K_PANIC, K_MBEGIN, K_MENTER, K_MLEAVE, K_MEND, K_ALLOC, K_FREE, K_OPEN, K_CLOSE, K_READX,
    K_SIZE, K_MAP, K_UNMAP, K_CREATE, K_WRITE, K_FFIRST, K_FNEXT, K_FCLOSE, K_ERRSTR, K_JOY, K_MEGS, K_ASSERT, K_ONESHOT,
    K_GLI, K_GCF, K_GDF, K_GFPN, K_CREATEFILE,
};
static std::vector<uint32_t> g_log[2];
static int g_pass;
enum { LOG_CAP = 1 << 20 };                                  // a call that logs more has run off into garbage
static void lg(uint32_t v) {
    if (g_log[g_pass].size() < LOG_CAP) g_log[g_pass].push_back(v);
}
static uint32_t safe_hash_str(const char* s, int max = 512) {  // a string that may not be one
    uint32_t h = 2166136261u;
    __try {
        for (int i = 0; i < max && s[i]; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    } __except (EXCEPTION_EXECUTE_HANDLER) { h ^= 0xbadbad; }
    return h;
}
static void lg_str(const char* s) { lg(s ? safe_hash_str(s) : 0xdeadbeef); }
// a report's arguments, read by its own format (%s hashed, %d / %x as they are)
static void lg_fmt(const char* fmt, va_list ap) {
    lg(aoff(fmt));
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        while (*p && strchr("-+ #0123456789.lh", *p)) p++;
        if (!*p) break;
        if (*p == '%') continue;
        if (*p == 's') lg_str(va_arg(ap, const char*));
        else if (*p == 'f') { double d = va_arg(ap, double); uint32_t u[2]; memcpy(u, &d, 8); lg(u[0]); lg(u[1]); }
        else lg(aoff((const void*)(uintptr_t)va_arg(ap, uint32_t)));
    }
}

static const DWORD PANIC_CODE = 0xE0564B01, OOM_CODE = 0xE0564B02;
static bool g_panic_returns;                                 // this check: LogPanic returns (the game's never does)
static void __cdecl st_log_report(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lg(K_REPORT);
    lg_fmt(fmt, ap);
    va_end(ap);
}
static void __cdecl st_log_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lg(K_PANIC);
    lg_fmt(fmt, ap);
    va_end(ap);
    if (!g_panic_returns) RaiseException(PANIC_CODE, 0, 0, 0);   // the game's LogPanic doesn't return (abend)
}
static int __cdecl st_multi_begin(const char* name) { lg(K_MBEGIN); lg_str(name); return ++C()->next_multi; }
static void __cdecl st_multi_enter(int h, const char*, int) { lg(K_MENTER); lg(h); }
static void __cdecl st_multi_leave(int h, const char*, int) { lg(K_MLEAVE); lg(h); }
static void __cdecl st_multi_end(int h, const char*, int) { lg(K_MEND); lg(h); }
static void* __cdecl st_mem_alloc(int n) {
    Ctrl* c = C();
    lg(K_ALLOC);
    lg(n);
    uint32_t k = c->allocs++;
    if (k == c->null_at) { lg(0); return 0; }
    int64_t sz = (int64_t)n + 4;
    if (sz <= 0 || (uint64_t)c->heap_top + (uint64_t)sz + 16 > ARENA_SIZE) { lg(0xbad); RaiseException(OOM_CODE, 0, 0, 0); }
    uint8_t* p = g_ar + c->heap_top;
    c->heap_top += (uint32_t)((sz + 15) & ~15);
    memset(p, 0xa3, (size_t)sz);                             // MemAlloc's fill, then the size head
    *(int32_t*)p = (int32_t)sz;
    lg(aoff(p + 4));
    return p + 4;
}
static void __cdecl st_delete(void* p) {
    lg(K_FREE);
    lg(aoff(p));
    uint8_t* q = (uint8_t*)p - 4;
    int32_t sz = *(volatile int32_t*)q;                      // MemFree reads the head (faults on a null, as the game)
    uint32_t a = sz < 0 ? (uint32_t)-sz : (uint32_t)sz;
    if (!in_arena(q, a)) { lg(0xbad); RaiseException(OOM_CODE, 0, 0, 0); }
    memset(q, 0xa3, a);                                      // MemFree's fill
}
// the file layer (file.obj): a handle is a slot + 1
static Slot* slot_of(int h) {
    if (h < 1 || h > 32 || h > C()->nslots || !C()->slot[h - 1].open) return 0;
    return &C()->slot[h - 1];
}
static int slot_alloc(int file, const char* name) {         // alloc_file: the next slot while fewer than 32, else a free one
    Ctrl* c = C();
    int k = -1;
    if (c->nslots < 32) k = c->nslots++;
    else
        for (int i = 0; i < c->nslots; i++)
            if (!c->slot[i].open) { k = i; break; }
    if (k < 0) {
        st_log_panic(S(0x004e5c90), name);                   // "file.cpp: Can't alloc file \"%s\", increase MAX_FILES"
        return 0;                                            // (alloc_file's -1: FileOpen's 0)
    }
    c->slot[k].file = file;
    c->slot[k].pos = 0;
    c->slot[k].open = 1;
    return k + 1;
}
static int __cdecl st_file_open(const char* name) {
    lg(K_OPEN);
    lg_str(name);
    int i = vfs_find(name);
    const uint8_t* d;
    uint32_t n;
    if (i < 0 || !vfs_exists(i) || !vfs_data(i, &d, &n)) { lg(0); return 0; }
    int h = slot_alloc(i, name);
    lg(h);
    return h;
}
static void __cdecl st_file_close(int* h) {
    lg(K_CLOSE);
    lg(*h);
    Slot* s = slot_of(*h);
    if (s) s->open = 0;
    *h = 0;
}
static uint8_t __cdecl st_read_exact(int h, void* buf, int n) {
    lg(K_READX);
    lg(h);
    lg(n);
    Slot* s = slot_of(h);
    const uint8_t* d;
    uint32_t size;
    if (!s || !vfs_data(s->file, &d, &size)) { st_log_panic(S(0x004e5cf4), "?", n, 6); return 0; }   // "ReadFile(\"%s\",%d) fails (0x%x)"
    if (n == 0) return 1;
    if (n < 0 || !buf) { st_log_panic(S(0x004e5cf4), g_vfs[s->file].key.c_str(), n, 0x3e6); return 0; }
    uint32_t avail = size - (uint32_t)s->pos, k = (uint32_t)n < avail ? (uint32_t)n : avail;
    memcpy(buf, d + s->pos, k);
    s->pos += (int32_t)k;
    if (k != (uint32_t)n) { st_log_panic(S(0x004e5cd4), n, g_vfs[s->file].key.c_str()); return 0; }   // "Can't read %d bytes from \"%s\""
    return 1;
}
static uint8_t __cdecl st_read(int h, void* buf, int* n) {  // FileRead (under the game's own FileReadLine)
    Slot* s = slot_of(h);
    const uint8_t* d;
    uint32_t size;
    if (!s || !vfs_data(s->file, &d, &size)) { st_log_panic(S(0x004e5d14), "?", *n, 6); return 0; }
    uint32_t avail = size - (uint32_t)s->pos, k = (uint32_t)*n < avail ? (uint32_t)*n : avail;
    memcpy(buf, d + s->pos, k);
    s->pos += (int32_t)k;
    *n = (int)k;
    return 1;
}
static int __cdecl st_file_size(int h) {
    lg(K_SIZE);
    lg(h);
    Slot* s = slot_of(h);
    const uint8_t* d;
    uint32_t size;
    if (!s || !vfs_data(s->file, &d, &size)) return -1;
    return (int)size;
}
static void views_apply() {                                 // the maps' protection, as the arena says
    DWORD old;
    for (uint32_t i = 0; i < C()->nviews && i < 512; i++)
        VirtualProtect(g_map + C()->views[i].off, C()->views[i].len, C()->views[i].prot, &old);
}
static FileMemoryMap* __cdecl st_create_map(FileMemoryMap* ret, int h) {
    lg(K_MAP);
    lg(h);
    Ctrl* c = C();
    Slot* s = slot_of(h);
    const uint8_t* d;
    uint32_t size;
    if (!s || !vfs_data(s->file, &d, &size) || c->nviews >= 512) { lg(0xbad); RaiseException(OOM_CODE, 0, 0, 0); }
    uint32_t len = (size + PG - 1) & ~(PG - 1);
    if (!len) len = PG;
    if (c->map_top + len > MAP_SIZE) { lg(0xbad); RaiseException(OOM_CODE, 0, 0, 0); }
    uint8_t* v = g_map + c->map_top;
    DWORD old;
    VirtualProtect(v, len, PAGE_READWRITE, &old);
    memcpy(v, d, size);
    memset(v + size, 0, len - size);
    VirtualProtect(v, len, PAGE_READONLY, &old);            // CreateFileMapping(PAGE_READONLY), FILE_MAP_READ
    c->views[c->nviews++] = {c->map_top, len, PAGE_READONLY};
    c->map_top += len;
    ret->ptr = v;
    ret->handle = 0x7000 + c->nviews;
    ret->fake = 0;
    lg(aoff(v));
    return ret;
}
static void __cdecl st_destroy_map(FileMemoryMap* m) {
    lg(K_UNMAP);
    lg(aoff(m->ptr));
    lg(m->fake & 0xff);
    Ctrl* c = C();
    for (uint32_t i = 0; i < c->nviews; i++)
        if (g_map + c->views[i].off == m->ptr) {
            c->views[i].prot = PAGE_NOACCESS;               // unmapped: gone
            DWORD old;
            VirtualProtect(m->ptr, c->views[i].len, PAGE_NOACCESS, &old);
        }
    m->ptr = 0;
    m->handle = 0xffffffffu;
}
static int __cdecl st_file_create(const char* name) {
    lg(K_CREATE);
    lg_str(name);
    int i = vfs_find(name);
    if (i < 0 || g_vfs[i].wf < 0) { lg(0); return 0; }       // only the harness's own writable files exist
    C()->wf_len[g_vfs[i].wf] = 0;
    int h = slot_alloc(i, name);
    lg(h);
    return h;
}
static uint8_t __cdecl st_file_write(int h, const void* buf, int n) {
    lg(K_WRITE);
    lg(h);
    lg(n);
    lg(n > 0 ? hash_bytes(buf, n) : 0);
    Slot* s = slot_of(h);
    if (!s || g_vfs[s->file].wf < 0) { st_log_panic(S(0x004e5d88), "?", n, 5); return 0; }
    int wf = g_vfs[s->file].wf;
    int32_t& len = C()->wf_len[wf];
    if (n > 0 && len + n <= (int32_t)WF_CAP) {
        memcpy(g_ar + WF_OFF + wf * WF_CAP + len, buf, n);
        len += n;
    }
    return 1;
}
static void* __cdecl st_find_first(const char* pat, char* out, int n) {
    lg(K_FFIRST);
    lg_str(pat);
    Ctrl* c = C();
    if (c->nlng <= 0) return (void*)-1;
    strncpy_o(out, c->lng[0], (uint32_t)n);
    c->find_pos = 1;
    return (void*)0x5000;
}
static uint8_t __cdecl st_find_next(void* h, char* out, int n) {
    lg(K_FNEXT);
    lg((uint32_t)(uintptr_t)h);
    Ctrl* c = C();
    if (c->find_pos >= c->nlng) return 0;
    strncpy_o(out, c->lng[c->find_pos++], (uint32_t)n);
    return 1;
}
static void __cdecl st_find_close(void* h) { lg(K_FCLOSE); lg((uint32_t)(uintptr_t)h); }
static const char* __cdecl st_error_string() { lg(K_ERRSTR); return "the stub's error"; }
static uint8_t __cdecl st_joy() { lg(K_JOY); return C()->joy; }
static int __cdecl st_megs() { lg(K_MEGS); return C()->megs; }
static void __cdecl st_assert(int cond, const char* fmt, ...) { lg(K_ASSERT); lg(cond); lg(aoff(fmt)); }
static void* __cdecl st_oneshot(void* self, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lg(K_ONESHOT);
    lg_fmt(fmt, ap);
    va_end(ap);
    return self;
}
static void __cdecl st_crt_fatal(int code) {
    printf("the game's CRT raised a fatal message (%d): stopping\n", code);
    fflush(stdout);
    ExitProcess(3);
}
// Windows through the game's import slots: logged, then Windows' answer (or the scripted fake locale's)
static const char* fake_locale(LCTYPE t) {
    static char b[4][0x40];
    Ctrl* c = C();
    switch (t) {
    case 4: return c->fake_lang;
    case 0xd: b[0][0] = c->fake_measure; b[0][1] = 0; return b[0];
    case 0xe: b[1][0] = c->fake_decimal; b[1][1] = 0; return b[1];
    case 0x16: b[2][0] = c->fake_mon; b[2][1] = 0; return b[2];
    }
    return "";
}
static int WINAPI st_GetLocaleInfoA(LCID lc, LCTYPE t, LPSTR buf, int n) {
    lg(K_GLI);
    lg(lc);
    lg(t);
    lg(n);
    int r;
    if (C()->locale_fake) {
        const char* v = fake_locale(t);
        int need = (int)strlen(v) + 1;
        if (!n) r = need;
        else if (n < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); r = 0; }
        else { memcpy(buf, v, need); r = need; }
    } else {
        r = GetLocaleInfoA(lc, t, buf, n);
    }
    lg(r);
    if (r && n) lg(hash_bytes(buf, r));
    return r;
}
static int WINAPI st_GetCurrencyFormatA(LCID lc, DWORD fl, LPCSTR v, const CURRENCYFMTA* fmt, LPSTR out, int n) {
    lg(K_GCF);
    lg(lc);
    lg(fl);
    lg_str(v);
    lg(fmt ? 1 : 0);
    lg(n);
    int r;
    if (C()->currency_fake) {                                // "EUR <number, its '.' the fake separator> EUR"
        char b[0x100];
        _snprintf(b, sizeof b, strchr(v, '.') ? "EUR %s EUR" : "EUR %s.00 EUR", v);
        b[sizeof b - 1] = 0;
        for (char* p = b; *p; p++)
            if (*p == '.') *p = C()->fake_mon;
        int need = (int)strlen(b) + 1;
        if (!n) r = need;
        else if (n < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); r = 0; }
        else { memcpy(out, b, need); r = need; }
    } else {
        r = GetCurrencyFormatA(lc, fl, v, fmt, out, n);
    }
    lg(r);
    if (r && n) lg(hash_bytes(out, r));
    return r;
}
static int WINAPI st_GetDateFormatA(LCID lc, DWORD fl, const SYSTEMTIME* st, LPCSTR fmt, LPSTR out, int n) {
    lg(K_GDF);
    lg(lc);
    lg(fl);
    lg(hash_bytes(st, sizeof *st));
    lg_str(fmt);
    lg(n);
    int r = GetDateFormatA(lc, fl, st, fmt, out, n);
    lg(r);
    if (r && n > 0) lg(hash_bytes(out, r));
    return r;
}
static DWORD WINAPI st_GetFullPathNameA(LPCSTR name, DWORD n, LPSTR buf, LPSTR* part) {
    lg(K_GFPN);
    lg_str(name);
    lg(n);
    DWORD r = GetFullPathNameA(name, n, buf, part);
    lg(r);
    return r;
}
static HANDLE WINAPI st_CreateFileA(LPCSTR name, DWORD acc, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    lg(K_CREATEFILE);
    lg_str(name);
    lg(acc);
    printf("  (CreateFileA(\"%s\") reached: refused -- the file layer is a stub here)\n", name);
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
}

static void install_stubs() {
    struct { uint32_t at; void* to; } p[] = {
        {0x004112b0, (void*)&st_log_panic}, {0x00411150, (void*)&st_log_report}, {0x004150a0, (void*)&st_multi_begin},
        {0x00415180, (void*)&st_multi_enter}, {0x004151d0, (void*)&st_multi_leave}, {0x00415220, (void*)&st_multi_end},
        {0x004140e0, (void*)&st_mem_alloc}, {0x00414390, (void*)&st_delete}, {0x00411780, (void*)&st_file_open},
        {0x00411850, (void*)&st_file_close}, {0x004118b0, (void*)&st_read_exact}, {0x00411890, (void*)&st_file_size},
        {0x00411940, (void*)&st_read}, {0x00411f30, (void*)&st_create_map}, {0x00412000, (void*)&st_destroy_map},
        {0x004115f0, (void*)&st_file_create}, {0x00411a30, (void*)&st_file_write}, {0x00411c20, (void*)&st_find_first},
        {0x00411c80, (void*)&st_find_next}, {0x00411cd0, (void*)&st_find_close}, {0x00416070, (void*)&st_error_string},
        {0x00418fc0, (void*)&st_joy}, {0x00454940, (void*)&st_megs}, {0x0041a1d0, (void*)&st_assert},
        {0x00471970, (void*)&st_assert}, {0x0041a620, (void*)&st_oneshot},
    };
    for (auto& x : p) patch_jmp(x.at, x.to);
    const uint32_t fatal[] = {0x004cf730, 0x004d45b0, 0x004d4570, 0x004d5c10, 0x004d6850, 0x004d1c70, 0x004d1c90};
    for (uint32_t a : fatal) patch_jmp(a, (void*)&st_crt_fatal);   // _amsg_exit, _NMSG_WRITE, _FF_MSGBANNER, _fptrap, __crtMessageBoxA, exit, _exit
    struct { const char* name; void* to; } q[] = {
        {"GetLocaleInfoA", (void*)&st_GetLocaleInfoA}, {"GetCurrencyFormatA", (void*)&st_GetCurrencyFormatA},
        {"GetDateFormatA", (void*)&st_GetDateFormatA}, {"GetFullPathNameA", (void*)&st_GetFullPathNameA},
        {"CreateFileA", (void*)&st_CreateFileA},
    };
    for (auto& x : q) {
        uint32_t s = iat_slot(x.name);
        if (!s) { printf("no import slot for %s\n", x.name); ExitProcess(4); }
        *(void**)(uintptr_t)s = x.to;
    }
    if (iat_slot("GetLocaleInfoA") != 0x005d74f0 || iat_slot("GetCurrencyFormatA") != 0x005d74f4 || iat_slot("GetDateFormatA") != 0x005d74f8) {
        printf("the locale imports aren't where the rewrite calls them\n");
        ExitProcess(4);
    }
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}

// the chain: every rewrite patched in (for a rewrite's pass), except the ones with logging stubs
struct ChainSave { uint32_t at; uint8_t b[5]; };
static ChainSave g_chain_save[256];
static int g_nchain;
static bool chain_skip(uint32_t at) { return at == 0x0041a1d0 || at == 0x00471970 || at == 0x0041a620; }
static void chain_on() {
    g_nchain = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        if (chain_skip(r->at)) continue;
        ChainSave& s = g_chain_save[g_nchain++];
        s.at = r->at;
        memcpy(s.b, (void*)(uintptr_t)r->at, 5);
        patch_jmp(r->at, r->fn);
    }
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}
static void chain_off() {
    for (int i = g_nchain - 1; i >= 0; i--) memcpy((void*)(uintptr_t)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_nchain = 0;
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}
static const char* name_of(uint32_t at) {
    for (ChainReg* r = ChainReg::head(); r; r = r->next)
        if (r->at == at) return r->name;
    return "?";
}

// ---- the image statics a check saves and compares -------------------------------------------------------------------
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_statics[] = {
    {0x004eae3c, 0x10, "res statics"}, {0x004eb108, 0x74, "locale cookie / unit tables"}, {0x004f53f8, 4, "options lock"},
    {0x004f56c0, 4, "telemetry lock"}, {0x00509128, 0x218, "resource directories"}, {0x00509340, 0x320, "locale"},
    {0x00559f58, 0x100, "options path"}, {0x0055a05c, 4, "options count"}, {0x0055a078, 0x1e828, "options items / telemetry"},
};
static uint32_t statics_bytes() {
    uint32_t t = 0;
    for (auto& g : g_statics) t += g.n;
    return t;
}
static void snap_statics(std::vector<uint8_t>& v) {
    v.resize(statics_bytes());
    uint8_t* p = v.data();
    for (auto& g : g_statics) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
}
static void load_statics(const std::vector<uint8_t>& v) {
    const uint8_t* p = v.data();
    for (auto& g : g_statics) { memcpy((void*)(uintptr_t)g.at, p, g.n); p += g.n; }
}
static const char* static_name(uint32_t a) {
    struct { uint32_t at, n; const char* nm; } t[] = {
        {0x004eae3c, 4, "resource lock"}, {0x004eae40, 4, "set list"}, {0x004eae44, 1, "began"}, {0x004eae48, 1, "Try flag"},
        {0x004eb108, 4, "Xlator cookie"}, {0x004eb10c, 1, "xlate warnings"}, {0x004eb110, 0x34, "metric units"},
        {0x004eb148, 0x34, "english units"}, {0x00509128, 4, "dir1 length"}, {0x00509130, 0x104, "dir1"},
        {0x00509238, 0x104, "dir2"}, {0x0050933c, 4, "dir2 length"}, {0x00509348, 0xf0, "unit Xlators"},
        {0x00509354, 4, "locale"}, {0x00509358, 0x3c, "LocaleInfo"}, {0x00509438, 0x200, "languages"},
        {0x00509638, 4, "language count"}, {0x0050963c, 4, "current language"}, {0x00509640, 4, "language resource"},
        {0x00509648, 12, "unit Xlator 12"}, {0x00559f58, 0x100, "options path"}, {0x0055a05c, 4, "options count"},
        {0x0055a078, 0x9000, "options items"}, {0x00563078, 8, "colour after the items"}, {0x00563080, 4, "metric count"},
        {0x00563084, 0x8800, "metrics"}, {0x0056b888, 4, "pending meter count"}, {0x0056b88c, 0x9000, "pending meters"},
        {0x00574890, 0x4010, "meter sets"},
    };
    for (auto& x : t)
        if (a >= x.at && a < x.at + x.n) return x.nm;
    return "?";
}

// ---- running one call both ways ---------------------------------------------------------------------------------------
template <typename F, typename... Args> static uint64_t invoke(F f, Args... a) {
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
static DWORD g_fault_code;
static uint32_t g_fault_at;
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
        _fpreset();
        return g_fault_code == PANIC_CODE ? 1 : 2;
    }
}
template <typename Fp> static int guarded_fp(Fp& fp, Footprint* f) {
    __try {
        fp(*f);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
static __declspec(noinline) void stack_fill(uint32_t pat) {
    volatile uint32_t buf[0x3000];
    for (int i = 0; i < 0x3000; i++) buf[i] = pat;
}

struct Stat { std::string name; long n, bad, faults, panics, ronly, fpbad, fpfault, fault_state, runaway; };
static std::vector<Stat> g_stats;
static Stat& stat(const std::string& nm) {
    for (Stat& s : g_stats)
        if (s.name == nm) return s;
    g_stats.push_back({nm, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    return g_stats.back();
}
static Footprint g_fp;
static std::string g_phase_name;
static std::vector<uint8_t> g_s0, g_s1, g_after;
static bool g_chain;
static long g_mismatch_total, g_checks_total;
static int g_result;                                         // the original's: 0 returned, 1 panicked, 2 faulted
static DWORD g_result_code;
static int g_pc;                                             // this check's x87 precision

template <typename Run> static int run_pass(Run& run, uint64_t* ret) {
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, g_pc == 24 ? _PC_24 : _PC_53, _MCW_PC);
    stack_fill(0x5a5a5a5au);
    views_apply();
    int k = guarded(run, ret);
    _controlfp_s(&cw, _CW_DEFAULT, 0xffffffff);
    _clearfp();
    return k;
}

static bool in_footprint(const void* p) {
    for (int k = 0; k < g_fp.n; k++)
        if ((const uint8_t*)p >= (const uint8_t*)g_fp.r[k].p && (const uint8_t*)p < (const uint8_t*)g_fp.r[k].p + g_fp.r[k].n) return true;
    return false;
}
static const char* arena_region(uint32_t off) {
    if (off < ARGS_OFF) return "stub state";
    if (off < ARGS_OFF + ARGS_CAP) return "arguments";
    if (off < WF_OFF) return "meters";
    if (off < HEAP_OFF) return "written files";
    return "heap";
}

template <typename RO, typename RN, typename FP>
static uint64_t check(const char* fname, RO ro, RN rn, FP fpf) {
    std::string name = fname;
    if (g_chain) name += " [chain]";
    g_phase_name = name;
    static int trace = getenv("VP_TRACE") ? 1 : 0;
    if (trace && g_chain) printf("  [trace] %s %s\n", g_phase, name.c_str());
    Stat& st = stat(name);
    st.n++;
    g_checks_total++;
    g_pc = chance(25) ? 24 : 53;
    g_panic_returns = chance(15);
    ww_sync();
    snap_statics(g_s0);
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    int fpfault = guarded_fp(fpf, &g_fp);
    if (fpfault) st.fpfault++;
    if (g_fp.replay_only) st.ronly++;
    // the original
    g_pass = 0;
    g_log[0].clear();
    uint64_t r0 = 0, r1 = 0;
    int k0 = run_pass(ro, &r0);
    DWORD c0 = k0 ? g_fault_code : 0;
    uint32_t a0 = g_fault_at;
    std::vector<uint32_t> d1 = ww_take();
    g_after.resize((size_t)d1.size() * PG);
    for (size_t i = 0; i < d1.size(); i++) {
        memcpy(&g_after[i * PG], g_ar + (size_t)d1[i] * PG, PG);
        memcpy(g_ar + (size_t)d1[i] * PG, g_sh + (size_t)d1[i] * PG, PG);
    }
    ww_take();
    snap_statics(g_s1);
    load_statics(g_s0);
    // the rewrite (chain: the original's entry, with every rewrite patched in)
    g_pass = 1;
    g_log[1].clear();
    int k1;
    if (g_chain) {
        chain_on();
        k1 = run_pass(ro, &r1);
        chain_off();
    } else {
        k1 = run_pass(rn, &r1);
    }
    DWORD c1 = k1 ? g_fault_code : 0;
    uint32_t a1 = g_fault_at;
    std::vector<uint32_t> d2 = ww_take();
    // compare
    bool same = k0 == k1 && c0 == c1 && (k0 != 0 || r0 == r1) && g_log[0] == g_log[1];
    bool runaway = g_log[0].size() >= LOG_CAP && g_log[1].size() >= LOG_CAP;   // both read on past the arena
    if (runaway) {
        st.runaway++;
        same = true;
    }
    std::string where;
    std::vector<uint32_t> all;
    std::merge(d1.begin(), d1.end(), d2.begin(), d2.end(), std::back_inserter(all));
    all.erase(std::unique(all.begin(), all.end()), all.end());
    bool state_same = true;
    for (uint32_t p : all) {
        auto it = std::lower_bound(d1.begin(), d1.end(), p);
        const uint8_t* want = (it != d1.end() && *it == p) ? &g_after[(size_t)(it - d1.begin()) * PG] : g_sh + (size_t)p * PG;
        const uint8_t* got = g_ar + (size_t)p * PG;
        if (memcmp(want, got, PG)) {
            uint32_t k = 0;
            while (want[k] == got[k]) k++;
            uint32_t off = p * PG + k;
            char b[200];
            sprintf(b, "arena %s +0x%x: original %02x, rewrite %02x", arena_region(off), off, want[k], got[k]);
            where = b;
            state_same = false;
            break;
        }
    }
    if (state_same) {
        std::vector<uint8_t> s1b;
        snap_statics(s1b);
        if (s1b != g_s1) {
            size_t k = 0;
            while (s1b[k] == g_s1[k]) k++;
            uint32_t base = 0;
            for (auto& g : g_statics) {
                if (k < base + g.n) {
                    uint32_t a = g.at + (uint32_t)(k - base);
                    char b[200];
                    sprintf(b, "static 0x%08x (%s): original %02x, rewrite %02x", a, static_name(a), g_s1[k], s1b[k]);
                    where = b;
                    break;
                }
                base += g.n;
            }
            state_same = false;
        }
    }
    if (k0 == 2 && k1 == 2 && c0 == c1) {
        if (!state_same) st.fault_state++;                  // a fault in both: the state at the fault is only noted
    } else if (!state_same && !runaway) {
        same = false;
    }
    // every byte the original changed lies in the footprint (unless it is replay_only)
    std::string fpwhere;
    if (k0 == 0 && !g_fp.replay_only && !fpfault) {
        for (size_t i = 0; i < d1.size() && fpwhere.empty(); i++)
            for (uint32_t k = 0; k < PG; k++) {
                const uint8_t* a = g_sh + (size_t)d1[i] * PG;
                if (g_after[i * PG + k] != a[k] && !in_footprint(g_ar + (size_t)d1[i] * PG + k)) {
                    char b[160];
                    sprintf(b, "arena %s +0x%x", arena_region(d1[i] * PG + k), d1[i] * PG + k);
                    fpwhere = b;
                    break;
                }
            }
        uint32_t base = 0;
        for (auto& g : g_statics) {
            if (!fpwhere.empty()) break;
            for (uint32_t k = 0; k < g.n; k++)
                if (g_s1[base + k] != g_s0[base + k] && !in_footprint((void*)(uintptr_t)(g.at + k))) {
                    char b[160];
                    sprintf(b, "static 0x%08x (%s)", g.at + k, static_name(g.at + k));
                    fpwhere = b;
                    break;
                }
            base += g.n;
        }
        if (!fpwhere.empty()) st.fpbad++;
    }
    if (k0 == 1) st.panics++;
    if (k0 == 2) st.faults++;
    if (!same) {
        st.bad++;
        g_mismatch_total++;
    }
    if ((!same && st.bad <= 4) || (!fpwhere.empty() && st.fpbad <= 2)) {
        printf("  %s %s (check %ld)\n", !same ? "MISMATCH" : "FOOTPRINT", name.c_str(), st.n);
        if (k0 != k1 || c0 != c1)
            printf("    outcome: original %d (%08lx at %08x), rewrite %d (%08lx at %08x)\n", k0, c0, a0, k1, c1, a1);
        if (k0 == 0 && k1 == 0 && r0 != r1) printf("    return: original %08llx, rewrite %08llx\n", r0, r1);
        if (g_log[0] != g_log[1]) {
            size_t k = 0;
            while (k < g_log[0].size() && k < g_log[1].size() && g_log[0][k] == g_log[1][k]) k++;
            printf("    stub logs: %zu / %zu entries, first difference at %zu: %08x / %08x\n", g_log[0].size(), g_log[1].size(), k,
                   k < g_log[0].size() ? g_log[0][k] : 0, k < g_log[1].size() ? g_log[1][k] : 0);
        }
        if (!where.empty()) printf("    state: %s\n", where.c_str());
        if (!fpwhere.empty()) printf("    the original wrote outside the footprint: %s\n", fpwhere.c_str());
    }
    // carry on from the original's result
    auto in_d1 = [&](uint32_t p) { return std::binary_search(d1.begin(), d1.end(), p); };
    for (uint32_t p : d2)
        if (!in_d1(p)) memcpy(g_ar + (size_t)p * PG, g_sh + (size_t)p * PG, PG);
    for (size_t i = 0; i < d1.size(); i++) {
        memcpy(g_ar + (size_t)d1[i] * PG, &g_after[i * PG], PG);
        memcpy(g_sh + (size_t)d1[i] * PG, &g_after[i * PG], PG);
    }
    ww_take();
    load_statics(g_s1);
    views_apply();
    g_result = k0;
    g_result_code = c0;
    return r0;
}
// a call to the function at `at` (the original; in a chain pass the patched entry) against `fn` (the rewrite)
template <typename F, typename FPF, typename... Args>
static uint64_t ck(uint32_t at, F fn, FPF fpf, Args... a) {
    F orig = (F)(uintptr_t)at;
    return check(name_of(at), [=]() { return invoke(orig, a...); }, [=]() { return invoke(fn, a...); },
                 [=](Footprint& f) { fpf(f, a...); });
}
// the three with logging stubs at their addresses: checked alone, with the original's own bytes put back
static uint8_t g_stub_bytes[3][5];
template <typename F, typename FPF, typename... Args>
static uint64_t ck_unstubbed(uint32_t at, F fn, FPF fpf, Args... a) {
    static const uint32_t ats[3] = {0x0041a1d0, 0x00471970, 0x0041a620};
    int k = at == ats[0] ? 0 : at == ats[1] ? 1 : 2;
    uint8_t save[5];
    memcpy(save, (void*)(uintptr_t)at, 5);
    memcpy((void*)(uintptr_t)at, g_stub_bytes[k], 5);
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
    bool chain = g_chain;
    g_chain = false;
    uint64_t r = ck(at, fn, fpf, a...);
    g_chain = chain;
    memcpy((void*)(uintptr_t)at, save, 5);
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
    return r;
}

// ---- resource files ---------------------------------------------------------------------------------------------------
static std::string g_gamedir, g_synthdir, g_userdir;
struct RealSet { std::string name; uint32_t size, hdr, count, preload; };
static std::vector<RealSet> g_real;
static std::vector<std::string> g_real_ser;                  // the game dir's 0SER files

struct Ent { std::string name; uint32_t type, ver, size; std::vector<uint8_t> payload; uint32_t reserved = 0, cookie = 0x4d474921; };
static std::vector<uint8_t> make_tsr(const std::vector<Ent>& e, uint32_t preload, int hdr_mode, uint32_t magic = 0x52535430) {
    std::vector<uint8_t> d(16 + 36 * e.size());
    uint32_t count = (uint32_t)e.size();
    for (size_t i = 0; i < e.size(); i++) {
        uint8_t* p = &d[16 + 36 * i];
        memset(p, 0, 36);
        memcpy(p, e[i].name.data(), std::min<size_t>(16, e[i].name.size()));
        memcpy(p + 16, &e[i].type, 4);
        memcpy(p + 20, &e[i].ver, 4);
        uint32_t s = (uint32_t)e[i].payload.size();
        memcpy(p + 24, &s, 4);
    }
    uint32_t hdr = (uint32_t)d.size();
    for (size_t i = 0; i < e.size(); i++) {
        uint32_t h[2] = {e[i].reserved, e[i].cookie};
        d.insert(d.end(), (uint8_t*)h, (uint8_t*)h + 8);
        d.insert(d.end(), e[i].payload.begin(), e[i].payload.end());
        if (i + 1 == preload) hdr = (uint32_t)d.size();
    }
    if (preload > count) hdr = (uint32_t)d.size();
    if (hdr_mode == 1) hdr = 16 + 36 * count / 2;                // a TOC block smaller than the TOC
    if (hdr_mode == 2) hdr = (uint32_t)d.size() + 64;            // longer than the file
    if (hdr_mode == 3) hdr = 8;                                  // smaller than the header
    uint32_t h[4] = {magic, count, preload, hdr};
    memcpy(d.data(), h, 16);
    return d;
}
static std::vector<uint8_t> make_ser(uint32_t type, uint32_t ver, const std::vector<uint8_t>& payload, uint32_t reserved = 0) {
    std::vector<uint8_t> d(20);
    uint32_t h[5] = {0x52455330, type, ver, reserved, 0x4d474921};
    memcpy(d.data(), h, 20);
    d.insert(d.end(), payload.begin(), payload.end());
    return d;
}
static const uint32_t k_types[] = {0x54455820, 0x4d4f444c, 0x53544142, 0x4c414e47, 0x42505054, 0};
static std::vector<uint8_t> rbytes(uint32_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = (uint8_t)rnd();
    return v;
}
struct SynSet { std::string name; std::vector<Ent> e; uint32_t preload; int hdr_mode; uint32_t magic; };
static SynSet make_synset(int kind) {
    SynSet s;
    s.hdr_mode = 0;
    s.magic = 0x52535430;
    int n = kind == 1 ? 0 : kind == 2 ? ri(200, 600) : ri(1, 40);
    for (int i = 0; i < n; i++) {
        Ent e;
        int len = chance(10) ? 16 : chance(10) ? 15 : chance(3) ? 0 : ri(3, 12);
        e.name = rname(len);
        if (!e.name.empty() && chance(50)) e.name.back() = 'x';
        if (i > 0 && chance(5)) e.name = recase(s.e[rnd() % i].name);   // a duplicate (the first one wins)
        e.type = k_types[rnd() % 6];
        e.ver = chance(80) ? 0 : rnd() % 5;
        e.payload = rbytes(chance(10) ? 0 : ri(1, chance(10) ? 20000 : 600));
        if (chance(3)) e.reserved = chance(50) ? 0x12345678 : 0;
        if (chance(3)) e.cookie = 0x41414141;
        s.e.push_back(e);
    }
    s.preload = kind == 3 ? n + ri(1, 5) : chance(30) ? n : chance(30) ? 0 : (uint32_t)ri(0, n);
    if (kind == 4) s.hdr_mode = ri(1, 3);
    if (kind == 5) s.magic = 0x52535431;
    return s;
}

// what the harness knows about a loaded set, for the payload check
static long g_payload_ok, g_payload_bad, g_payload_unknown, g_names16, g_names16_found;
// the entry whose data + 8 is p: its payload's offset in its file, its size, its set's file handle (0: hunted)
struct EntryInfo { uint32_t off, size; int32_t fh; char name[17]; bool hunted; };
static bool locate_entry(const uint8_t* p, EntryInfo* info) {
    __try {
        for (ResourceSetNode* n = g_res_sets; n; n = n->next)
            for (uint32_t i = 0; i < n->count && i < 100000; i++) {
                ResourceTOCEntry* e = &n->toc[i];
                if (e->data + 8 != p) continue;
                info->size = e->size & 0x7fffffff;
                info->fh = n->file.fh;
                memset(info->name, 0, sizeof info->name);
                memcpy(info->name, e->name, 16);
                info->hunted = n->name[0] == 0 && n->file.fh == 0;
                if (info->hunted) {
                    info->off = 20;                              // a file resource: its payload from byte 20
                } else {
                    uint32_t off = 16 + 36 * n->count;
                    for (uint32_t k = 0; k < i; k++) off += (n->toc[k].size & 0x7fffffff) + 8;
                    info->off = off + 8;
                }
                return true;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
static int safe_memcmp(const void* a, const void* b, size_t n) {
    __try {
        return memcmp(a, b, n) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}
static void verify_payload(const uint8_t* p) {
    if (!p) return;
    EntryInfo in;
    if (!locate_entry(p, &in)) { g_payload_unknown++; return; }
    int file = -1;
    if (in.hunted) {
        for (size_t k = 0; k < g_vfs.size() && file < 0; k++) {
            std::string base = g_vfs[k].key.substr(g_vfs[k].key.find_last_of('\\') + 1);
            if (base == lower(in.name)) file = (int)k;
        }
    } else {
        Slot* s = slot_of(in.fh);
        if (s) file = s->file;
    }
    const uint8_t* d;
    uint32_t fsize;
    if (file < 0 || g_vfs[file].bad || !vfs_data(file, &d, &fsize) || in.off + in.size > fsize) { g_payload_unknown++; return; }
    int r = safe_memcmp(p, d + in.off, in.size);
    if (r == 0) g_payload_ok++;
    else if (r == 1) {
        if (g_payload_bad++ < 5)
            printf("  (payload check: %s \"%s\" size %u at file offset %u of %s differs)\n", in.hunted ? "file resource" : "entry", in.name,
                   in.size, in.off, g_vfs[file].key.c_str());
    }
    else g_payload_unknown++;                                // (a minimized set's view: unreadable, as in the game)
}

// ---- the resource calls -----------------------------------------------------------------------------------------------
static uint8_t c_begin(const std::string& d1, const std::string& d2) {
    args_reset();
    char* a = arg_str(d1);
    char* b = arg_str(d2);
    return (uint8_t)ck(0x00419640, ResourceBegin_rw, fp_resource_begin, (const char*)a, (const char*)b);
}
static uint8_t c_load(const std::string& name, bool must) {
    args_reset();
    const char* p = arg_str(name);
    if (must) return (uint8_t)(ck(0x00419a30, ResourceSetMustLoad_rw, fp_must_load, p), g_result == 0);
    if (chance(20)) return (uint8_t)ck(0x00419720, load_resource_set_name_rw, fp_load_set, p);
    return (uint8_t)ck(0x00419710, ResourceSetAttemptLoad_rw, fp_load_set, p);
}
static void c_unload(const std::string& name) {
    args_reset();
    const char* p = arg_str(name);
    ck(0x00419bb0, ResourceSetUnload_rw, fp_set_unload, p);
}
static const uint8_t* c_get(const std::string& path, uint32_t type, int api) {
    args_reset();
    const char* p = arg_str(path);
    uint32_t* ver = (uint32_t*)arg_buf(4, 0xcc);
    int32_t* size = chance(80) ? (int32_t*)arg_buf(4, 0xcc) : 0;
    uint8_t* first = chance(70) ? (uint8_t*)arg_buf(4, 0xcc) : 0;
    uint8_t* fresh = chance(70) ? (uint8_t*)arg_buf(4, 0xcc) : 0;
    uint64_t r = 0;
    switch (api) {
    case 0: r = ck(0x00419fa0, ResourceGet_rw, fp_resource_get, p, type, ver, size, first, fresh); break;
    case 1: r = ck(0x00419ce0, ResourceTry_rw, fp_resource_get, p, type, ver, size, first, fresh); break;
    case 2: r = ck(0x00419f70, ResourceGetDiscardable_rw, fp_discardable, p, type, ver, size); break;
    default: r = ck(0x00419f40, ResourceTryDiscardable_rw, fp_discardable, p, type, ver, size); break;
    }
    const uint8_t* q = g_result == 0 ? (const uint8_t*)(uintptr_t)(uint32_t)r : 0;
    verify_payload(q);
    return q;
}
static uint8_t c_forget(const void* p) {
    return (uint8_t)ck(0x0041a450, ResourceForget_rw, fp_forget, (void*)p);
}
static uint8_t c_exists(const std::string& path) {
    args_reset();
    const char* p = arg_str(path);
    return (uint8_t)ck(0x00419d10, ResourceExists_rw, fp_resource_exists, p);
}
static void c_end() { ck(0x0041a5a0, ResourceEnd_rw, fp_resource_end); }

// a fresh start: no sets, empty heap, no open files or maps, the image statics as the game starts them
static void reset_world() {
    Ctrl* c = C();
    c->heap_top = HEAP_OFF;
    c->allocs = 0;
    c->null_at = 0xffffffffu;
    c->nslots = 0;
    memset(c->slot, 0, sizeof c->slot);
    c->map_top = 0;
    for (uint32_t i = 0; i < c->nviews; i++) c->views[i].prot = PAGE_NOACCESS;
    views_apply();
    c->nviews = 0;
    c->find_pos = 0;
    c->locale_fake = 0;
    c->currency_fake = 0;
    g_res_sets = 0;
    g_res_began = 0;
    g_res_try = 0;
    g_lang_res = 0;
    g_nlang = 0;
    g_cur_lang = -1;
    g_opt_count = 0;
    g_tm_nmetric = 0;
    g_tm_npending = 0;
    memset((void*)0x00563078, 0, 8);
    vfs_unload_disk();
}

// the sets loaded, as the list says (names as the harness asked for them)
struct Loaded { std::string name; int times; };
static std::vector<Loaded> g_loaded;
static void note_loaded(const std::string& name) {
    for (auto& l : g_loaded)
        if (!_stricmp(l.name.c_str(), name.c_str())) { l.times++; return; }
    g_loaded.push_back({name, 1});
}

// fetch every entry of every loaded set (random order, forms and APIs), with forgets, exists and maximize/minimize
struct RawReq { char set[16]; char res[17]; uint32_t type; };
static RawReq g_raw[30000];
static int collect_reqs(RawReq* out, int max) {             // every entry of every set in the list (read guarded)
    int n = 0;
    __try {
        for (ResourceSetNode* nd = g_res_sets; nd && n < max; nd = nd->next)
            for (uint32_t i = 0; i < nd->count && i < 10000 && n < max; i++) {
                RawReq& r = out[n];
                memset(&r, 0, sizeof r);
                memcpy(r.set, nd->name, 15);
                memcpy(r.res, nd->toc[i].name, 16);
                r.type = nd->toc[i].type;
                n++;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
static void fetch_all(int budget) {
    struct Req { std::string set, res; uint32_t type; };
    std::vector<Req> reqs;
    int nraw = collect_reqs(g_raw, 30000);
    for (int i = 0; i < nraw; i++) reqs.push_back({g_raw[i].set, g_raw[i].res, g_raw[i].type});
    for (size_t i = reqs.size(); i > 1; i--) std::swap(reqs[i - 1], reqs[rnd() % i]);
    std::vector<const uint8_t*> got;
    for (size_t i = 0; i < reqs.size() && (int)i < budget; i++) {
        Req& r = reqs[i];
        std::string path = chance(70) ? r.res : r.set + "/" + r.res;
        if (chance(20)) path = recase(path);
        uint32_t type = chance(3) ? r.type ^ 1 : r.type;
        if (r.res.size() == 16) g_names16++;
        const uint8_t* p = c_get(path, type, chance(50) ? 0 : ri(1, 3));
        if (r.res.size() == 16 && p) g_names16_found++;
        if (p) got.push_back(p);
        if (p && chance(25)) c_forget(p);
        if (chance(5)) c_exists(chance(50) ? path : rname(ri(1, 20)));
        if (chance(1)) {
            args_reset();
            const char* s = arg_str(r.set);
            if (chance(50)) ck(0x00419b10, ResourceSetMinimize_rw, fp_set_minimize, s);
            else ck(0x00419a80, ResourceSetMaximize_rw, fp_set_maximize, s);
        }
        if (chance(1)) {
            if (chance(50)) ck(0x00419ba0, ResourceMaximizeAll_rw, fp_all_sets);
            else ck(0x00419b60, ResourceMinimizeAll_rw, fp_all_sets);
        }
    }
    // a second fetch of some (not fresh), and forgets of what was fetched
    for (int k = 0; k < 20 && !got.empty(); k++) {
        const uint8_t* p = got[rnd() % got.size()];
        c_forget(p);
    }
}

static long g_rounds_done;
static void round_resources(int r) {
    reset_world();
    g_loaded.clear();
    // synthetic sets and files in the synthetic directory, the user directory
    std::vector<std::string> synth;
    int nsyn = ri(0, 5);
    for (int i = 0; i < nsyn; i++) {
        int kind = chance(70) ? 0 : ri(1, 5);
        SynSet s = make_synset(kind);
        int nl = chance(70) ? ri(4, 12) : chance(50) ? ri(13, 16) : ri(17, 58);
        s.name = rname(nl) + (chance(50) ? ".res" : ".trk");
        if (chance(10) && !g_real.empty()) s.name = g_real[rnd() % g_real.size()].name;   // shadows a real set
        int vi = vfs_mem(g_synthdir + s.name, make_tsr(s.e, s.preload, s.hdr_mode, s.magic));
        g_vfs[vi].bad = s.hdr_mode != 0 || s.preload > s.e.size();   // its TOC block isn't what the header says
        synth.push_back(s.name);
    }
    std::vector<std::string> hunts;
    for (int i = 0; i < 6; i++) {
        int nl = chance(60) ? ri(3, 12) : chance(50) ? ri(13, 16) : ri(17, 40);
        std::string nm = rname(nl) + ".dat";
        uint32_t type = k_types[rnd() % 6];
        std::vector<uint8_t> d = make_ser(type, chance(80) ? 0 : 1, rbytes(ri(0, 3000)), chance(5) ? 7 : 0);
        if (chance(5)) d[0] = 'X';
        if (chance(5)) d.resize(ri(0, 11));
        bool user = chance(30);
        vfs_mem((user ? g_userdir : g_synthdir) + nm, d);
        hunts.push_back((user ? "~" : chance(50) ? "*" : "") + nm);
    }
    bool swap = chance(30);
    c_begin(swap ? g_gamedir : g_synthdir, swap ? g_synthdir : g_gamedir);
    // the real sets: the next few in the rotation (by size), some synthetic, a duplicate, a missing one
    static size_t rot = 0;
    uint64_t budget = 0;
    std::vector<std::string> want;
    for (int k = 0; k < 4 && !g_real.empty(); k++) {
        const RealSet& rs = g_real[rot++ % g_real.size()];
        if (budget + rs.hdr > (90u << 20) && k) break;
        budget += rs.hdr;
        want.push_back(chance(20) ? recase(rs.name) : rs.name);
    }
    for (auto& s : synth) want.push_back(s);
    if (!want.empty() && chance(30)) want.push_back(want[rnd() % want.size()]);
    if (chance(20)) want.push_back(rname(8) + ".res");
    for (size_t i = want.size(); i > 1; i--) std::swap(want[i - 1], want[rnd() % i]);
    for (auto& w : want) {
        bool exists = vfs_find((g_synthdir + w).c_str()) >= 0 || vfs_find((g_gamedir + w).c_str()) >= 0;
        if (chance(2)) C()->null_at = C()->allocs + ri(0, 2);           // MemAlloc hands out a null
        uint8_t ok = c_load(w, exists && chance(70));
        C()->null_at = 0xffffffffu;
        if (ok && g_result == 0) note_loaded(w);
    }
    fetch_all(chance(10) ? 100000 : 1500);
    // file resources, hunted, then forgotten (twice, now and then)
    for (auto& h : hunts) {
        uint32_t type = 0;
        const uint8_t* d;
        uint32_t n;
        std::string file = h[0] == '~' ? g_userdir + h.substr(1) : g_synthdir + (h[0] == '*' ? h.substr(1) : h);
        int vi = vfs_find(file.c_str());
        if (vi >= 0 && vfs_data(vi, &d, &n) && n >= 8) memcpy(&type, d + 4, 4);
        const uint8_t* p = c_get(h, chance(90) ? type : type + 1, chance(50) ? 0 : 1);
        if (p) {
            if (chance(30)) c_get(h, type, 0);
            c_forget(p);
            if (chance(20)) c_forget(p);
        }
        c_exists(h);
    }
    // the game dir's own file resources, and names nobody has
    for (auto& s : g_real_ser)
        if (chance(30)) {
            uint32_t type = 0;
            const uint8_t* d;
            uint32_t n;
            int vi = vfs_find((g_gamedir + s).c_str());
            if (vi >= 0 && vfs_data(vi, &d, &n) && n >= 8) memcpy(&type, d + 4, 4);
            const uint8_t* p = c_get(s, type, 0);
            if (p) c_forget(p);
        }
    c_get(rname(ri(1, 20)), 0x54455820, chance(50) ? 0 : 1);
    c_get(std::string(ri(20, 31), 'q'), 0x54455820, 1);
    // the whole rotation of maximize / minimize
    if (chance(30)) ck(0x00419b60, ResourceMinimizeAll_rw, fp_all_sets);
    if (chance(30)) ck(0x00419ba0, ResourceMaximizeAll_rw, fp_all_sets);
    // unload everything (each as many times as it was loaded), and one that isn't there
    for (auto& l : g_loaded)
        for (int k = 0; k < l.times; k++) c_unload(chance(20) ? recase(l.name) : l.name);
    c_unload(rname(6) + ".res");
    c_end();
    g_rounds_done++;
}

// the limits: many sets (the file slots), long and 16-character names, a huge set
static void round_limits() {
    reset_world();
    c_begin(g_synthdir, g_gamedir);
    std::vector<std::string> names;
    int loaded = 0;
    for (int i = 0; i < 36; i++) {                           // 32 file slots: the 33rd set can't open its file
        SynSet s = make_synset(0);
        s.name = "many" + std::to_string(i) + ".res";
        vfs_mem(g_synthdir + s.name, make_tsr(s.e, s.preload, 0));
        uint8_t ok = c_load(s.name, false);
        if (ok && g_result == 0) { loaded++; names.push_back(s.name); }
        else if (g_result == 1) break;
    }
    printf("  limits: %d sets loaded before the file layer's 32 slots ran out (%s)\n", loaded,
           g_result == 1 ? "the next load panicked: \"Can't alloc file ..., increase MAX_FILES\"" : "no panic");
    for (auto& n : names) c_unload(n);
    c_begin(g_synthdir, g_gamedir);                          // (the panic left dir1 with the name appended)
    // set names of 15, 16, 20, 40 characters: loaded, looked up, unloaded (paths kept under 64 characters:
    // ResourceGet copies the whole path into a 64-byte local, whose end is its return address)
    for (int len : {15, 16, 20, 40, 59}) {
        SynSet s = make_synset(0);
        s.name = rname(len - 4) + ".res";
        vfs_mem(g_synthdir + s.name, make_tsr(s.e, s.preload, 0));
        s.e.resize(1);
        s.e[0].name = "f.t";
        s.e[0].type = 0x54455820;
        vfs_mem(g_synthdir + s.name, make_tsr(s.e, 1, 0));
        g_res_sets = 0;
        if (len == 16) {                                     // a set before it: its name's terminator is then no longer 0
            SynSet f = make_synset(0);
            vfs_mem(g_synthdir + "filler.res", make_tsr(f.e, f.preload, 0));
            c_load("filler.res", false);
        }
        uint8_t ok = c_load(s.name, false);
        uint8_t again = ok ? c_load(s.name, false) : 0;      // found in the list (one more load) or a second node?
        int nodes = 0;
        for (ResourceSetNode* n = g_res_sets; n; n = n->next) nodes++;
        const uint8_t* p = c_get(s.name + "/f.t", 0x54455820, 1);
        c_unload(s.name);
        int after1 = 0;
        for (ResourceSetNode* n = g_res_sets; n; n = n->next) after1++;
        c_unload(s.name);
        int after2 = 0;
        for (ResourceSetNode* n = g_res_sets; n; n = n->next) after2++;
        printf("  limits: a %2d-character set name: loaded %d, again %d -> %d node(s); \"set/res\" %s; after two unloads %d, %d node(s) left\n",
               len, ok, again, nodes, p ? "found" : "NOT found", after1, after2);
        g_res_sets = 0;                                          // (what the unloads couldn't find: dropped here)
    }
    // entry names of exactly 16 characters (no terminator in the TOC): found by name?
    {
        SynSet s = make_synset(0);
        s.e.resize(4);
        s.e[0].name = "sixteen_chars.tx";
        s.e[1].name = "fifteen_chars.t";
        s.e[2].name = "";
        s.e[3].name = "sixteen_chars.tx";
        for (auto& e : s.e) e.type = 0x54455820;
        s.name = "names16.res";
        s.preload = 2;
        vfs_mem(g_synthdir + s.name, make_tsr(s.e, s.preload, 0));
        c_load(s.name, false);
        const uint8_t* a = c_get("sixteen_chars.tx", 0x54455820, 1);
        const uint8_t* b = c_get("fifteen_chars.t", 0x54455820, 1);
        const uint8_t* c = c_get("names16.res/", 0x54455820, 1);
        printf("  limits: entry names -- 16 characters: %s; 15: %s; empty name (\"set/\"): %s\n", a ? "found" : "NOT found (hunted as a file)",
               b ? "found" : "not found", c ? "found" : "not found");
        c_unload(s.name);
    }
    // a resource name of 31 / 32+ characters, a path of 63 characters (ResourceGet's buffers)
    {
        SynSet s = make_synset(0);
        s.e.resize(1);
        s.e[0].name = "short.tex";
        s.e[0].type = 0x54455820;
        s.name = "buf.res";
        vfs_mem(g_synthdir + s.name, make_tsr(s.e, 1, 0));
        c_load(s.name, false);
        for (int len : {31, 32, 40, 55}) {
            std::string res = rname(len);
            c_get(res, 0x54455820, 1);
            c_exists(res);
        }
        std::string longset = std::string(40, 's') + "/short.tex";   // 51 characters: fits ResourceGet's set buffer
        const uint8_t* p = c_get(longset, 0x54455820, 1);
        uint8_t e = c_exists("buf.res/short.tex");
        uint8_t e2 = c_exists(std::string(33, 'b') + "/short.tex");
        printf("  limits: \"buf.res/short.tex\" exists %d; a 33-character set part in ResourceExists: %d; a 40-character one in ResourceGet: %s\n",
               e, e2, p ? "found" : "not found");
        c_unload(s.name);
    }
    // a big set (thousands of entries)
    {
        SynSet s;
        for (int i = 0; i < 4000; i++) {
            Ent e;
            e.name = "e" + std::to_string(i) + ".tex";
            e.type = 0x54455820;
            e.ver = 0;
            e.payload = rbytes(ri(0, 64));
            s.e.push_back(e);
        }
        s.name = "huge.res";
        vfs_mem(g_synthdir + s.name, make_tsr(s.e, 2000, 0));
        c_load(s.name, false);
        for (int i = 0; i < 200; i++) c_get(s.e[rnd() % 4000].name, 0x54455820, 1);
        c_unload(s.name);
    }
    c_end();
}

// ---- every real set alone: every entry of its TOC fetched (bare and "set/name"), its payload checked -------------------
static long g_all_sets, g_all_entries, g_all_found, g_all_verified;
static std::vector<std::string> g_all_missing;
static void round_all_real() {
    for (const RealSet& rs : g_real) {
        reset_world();
        c_begin(g_gamedir, g_synthdir);                      // the game dir first (a synthetic set may shadow the name)
        long ok0 = g_payload_ok;
        if (!c_load(rs.name, true) || g_result) { printf("  %s: didn't load\n", rs.name.c_str()); continue; }
        int nraw = collect_reqs(g_raw, 30000);
        std::vector<RawReq> reqs(g_raw, g_raw + nraw);
        for (const RawReq& r : reqs) {
            std::string path = chance(50) ? std::string(r.res) : rs.name + "/" + r.res;
            const uint8_t* p = c_get(path, r.type, chance(80) ? 0 : ri(1, 3));
            g_all_entries++;
            if (p) g_all_found++;
            else if (!g_chain && g_all_missing.size() < 400) {
                char b[160];
                sprintf(b, "%s: \"%s\" (%zu characters, type %08x) as \"%s\"%s", rs.name.c_str(), r.res, strlen(r.res), r.type,
                        path.c_str(), g_result == 1 ? " -- panicked" : g_result == 2 ? " -- faulted" : "");
                g_all_missing.push_back(b);
            }
            if (p && chance(10)) c_forget(p);
        }
        g_all_verified += g_payload_ok - ok0;
        c_unload(rs.name);
        c_end();
        g_all_sets++;
    }
}

// ---- the functions the others reach, called directly ------------------------------------------------------------------
static void round_direct() {
    reset_world();
    c_begin(g_synthdir, g_gamedir);
    // load_resource_set(int, char const*) on a file opened by hand: synthetic sets, and a real one
    for (int i = 0; i < 6; i++) {
        std::string name;
        if (i == 5 && !g_real.empty()) {
            const RealSet& rs = g_real[rnd() % g_real.size()];
            if (rs.hdr < (40u << 20)) name = rs.name;
        }
        std::string path;
        if (name.empty()) {
            SynSet s = make_synset(chance(70) ? 0 : ri(1, 5));
            name = "direct" + std::to_string(i) + ".res";
            path = g_synthdir + name;
            int vi = vfs_mem(path, make_tsr(s.e, s.preload, s.hdr_mode, s.magic));
            g_vfs[vi].bad = s.hdr_mode != 0 || s.preload > s.e.size();
        } else {
            path = g_gamedir + name;
        }
        g_panic_returns = false;
        int fh = st_file_open(path.c_str());
        args_reset();
        const char* nm = arg_str(name);
        uint8_t ok = (uint8_t)ck(0x00419840, load_resource_set_file_rw, fp_load_set_file, fh, nm);
        if (g_result == 0 && ok) note_loaded(name);
        else st_file_close(&fh);
    }
    // a set's file: unmapped, mapped again, and closed (the node keeps its handle: ResourceSetUnload closes it again)
    for (ResourceSetNode* n = g_res_sets; n; n = n->next) {
        if (chance(50)) ck(0x004195f0, FileInfo_unmap_rw, fp_file_unmap, &n->file, 0);
        if (chance(50)) ck(0x004195c0, FileInfo_map_rw, fp_file_map, &n->file, 0);
        if (chance(30)) ck(0x00419600, FileInfo_unload_rw, fp_file_unload, &n->file, 0);
    }
    ck(0x00419b70, apply_to_all_sets_rw, fp_apply_all, (void*)0x00419b10);
    ck(0x00419b70, apply_to_all_sets_rw, fp_apply_all, (void*)0x00419a80);
    fetch_all(300);
    // hunt_for_resource, destroy_hunted_resource by name and by node (and a node that isn't in the list)
    for (int i = 0; i < 12; i++) {
        std::string nm = rname(chance(80) ? ri(3, 12) : ri(13, 20)) + ".ser";
        bool user = chance(30);
        vfs_mem((user ? g_userdir : g_synthdir) + nm, make_ser(k_types[rnd() % 6], 0, rbytes(ri(0, 500))));
        args_reset();
        const char* hn = arg_str(chance(10) ? rname(5) : user ? "~" + nm : nm);
        ResourceSetNode* node = (ResourceSetNode*)(uintptr_t)(uint32_t)ck(0x0041a1e0, hunt_for_resource_rw, fp_hunt, hn);
        if (g_result || !node) continue;
        switch (ri(0, 2)) {
        case 0: ck(0x0041a4d0, destroy_hunted_name_rw, fp_destroy_name, (const char*)node->toc->name); break;
        case 1: ck(0x0041a520, destroy_hunted_node_rw, fp_destroy_node, node); break;
        default: {
            args_reset();
            ResourceSetNode* fake = (ResourceSetNode*)arg_buf(sizeof(ResourceSetNode), 0);
            fake->toc = node->toc;                           // (its entry: freed with it)
            ck(0x0041a520, destroy_hunted_node_rw, fp_destroy_node, fake);
        }
        }
    }
    args_reset();
    ck(0x0041a4d0, destroy_hunted_name_rw, fp_destroy_name, (const char*)arg_str("nobody.ser"));
    // ResourceWrite: a file resource's head, then its payload, read back as "~written.res"
    for (int i = 0; i < 3; i++) {
        g_panic_returns = false;
        int fh = st_file_create((g_userdir + "written.res").c_str());
        uint32_t type = k_types[rnd() % 6], ver = chance(70) ? 0 : rnd() % 9;
        ck(0x0041a5c0, ResourceWrite_rw, fp_resource_write, fh, type, ver);
        std::vector<uint8_t> pay = rbytes(ri(0, 900));
        if (!pay.empty()) st_file_write(fh, pay.data(), (int)pay.size());
        st_file_close(&fh);
        const uint8_t* p = c_get("~written.res", type, 0);
        if (p) c_forget(p);
    }
    for (auto& l : g_loaded)
        for (int k = 0; k < l.times; k++) c_unload(l.name);
    g_loaded.clear();
    c_end();
}

// ---- small functions alone --------------------------------------------------------------------------------------------
static void round_small() {
    reset_world();
    // split_res_path: separate buffers, and the two overlapping layouts of ResourceExists / ResourceGet
    for (int i = 0; i < 300; i++) {
        args_reset();
        std::string p;
        int parts = ri(1, 4);
        for (int k = 0; k < parts; k++) p += (k ? "/" : "") + rname(ri(0, i < 250 ? 20 : 70));
        char* path = arg_str(p);
        char* buf = (char*)arg_buf(0x400, 0x55);
        int lay = ri(0, 2);
        char* set = lay == 1 ? buf : lay == 2 ? buf + 0x20 : buf;
        char* res = lay == 1 ? buf + 0x20 : lay == 2 ? buf : buf + 0x200;
        ck(0x00419e60, split_res_path_rw, fp_split, set, res, (const char*)path);
    }
    // is_singleton, OneShot, ASSERT_MSG, FileExists
    for (int i = 0; i < 100; i++) {
        args_reset();
        ResourceTOCEntry* e = (ResourceTOCEntry*)arg_buf(0x40, 0);
        for (int k = 0; k < 0x24; k++) ((uint8_t*)e)[k] = chance(30) ? 0 : chance(20) ? '*' : (uint8_t)rnd();
        ((uint8_t*)e)[0x3f] = 0;
        ck(0x00419620, is_singleton_rw, fp_pure_entry, (const ResourceTOCEntry*)e, 0);
        void* obj = arg_buf(0x10);
        const char* fmt = arg_str(rname(10));
        ck_unstubbed(0x0041a620, OneShot_rw, fp_oneshot, obj, fmt);
        ck_unstubbed(0x0041a1d0, ASSERT_MSG_res_rw, fp_assert, (int)(rnd() & 1), fmt);
        ck_unstubbed(0x00471970, ASSERT_MSG_opt_rw, fp_assert, (int)(rnd() & 1), fmt);
        const char* nm = arg_str(chance(50) ? g_gamedir + "race.res" : g_synthdir + rname(5));
        ck(0x00419f00, FileExists_rw, fp_file_exists, nm);
    }
    // locale's fixup_res and map_compare, Xlator's constructor
    for (int i = 0; i < 100; i++) {
        args_reset();
        LangResource* r = (LangResource*)arg_buf(0x400, 0);
        int32_t n = chance(10) ? -ri(1, 5) : ri(0, 100);
        r->count = n;
        for (int k = 0; k < 100; k++) { r->pairs[k].key = (const char*)(uintptr_t)rnd(); r->pairs[k].text = (const char*)(uintptr_t)rnd(); }
        ck(0x0041aa90, fixup_res_rw, fp_fixup_lang, r);
        const char** a = (const char**)arg_buf(8);
        const char** b = (const char**)arg_buf(8);
        std::string s1 = rname(ri(0, 10)), s2 = chance(30) ? recase(s1) : rname(ri(0, 10));
        *a = arg_str(s1);
        *b = arg_str(s2);
        ck(0x0041af40, map_compare_rw, fp_none_pp, (const void*)a, (const void*)b);
        Xlator* x = (Xlator*)arg_buf(16, 0x77);
        ck(0x0041af80, Xlator_ctor_rw, fp_xlator_ctor, x, 0, (const char*)arg_str("Units:MetersAbbreviation"));
    }
}

// ---- options ---------------------------------------------------------------------------------------------------------------
static std::string g_opt_def;                                // the game's options.def
static std::string synth_options(int kind) {
    std::string s;
    int ver = kind == 1 ? 2 : 1;
    if (kind != 2) s += "version " + std::to_string(ver) + "\r\n";
    int items = kind == 3 ? ri(260, 330) : ri(5, 60);
    const char* secs[] = {"GLOBAL", "GX", "SOUND", "CONTROL", "misc"};
    for (int i = 0; i < items; i++) {
        if (chance(15) || i == 0) {
            std::string sec = chance(90) ? secs[rnd() % 5] : rname(ri(20, 31));
            s += "[" + sec + (chance(95) ? "]" : "") + "\r\n";
        }
        if (chance(5)) s += "; a comment\r\n";
        if (chance(3)) s += "\r\n";
        std::string key = chance(90) ? "key" + std::to_string(kind == 3 ? i : ri(0, 40)) : rname(ri(20, 31));
        if (chance(5)) { s += key + "\r\n"; continue; }            // no value
        std::string val;
        switch (ri(0, 5)) {
        case 0: val = std::to_string(ri(-100000, 100000)); break;
        case 1: { char b[40]; sprintf(b, "%.6f", (double)(int)rnd() / 1000.0); val = b; break; }
        case 2: val = chance(50) ? "yes" : "no"; break;
        case 3: val = rname(ri(1, 30)); break;
        case 4: val = rname(ri(60, 150)); break;             // longer than the item's 64 bytes
        default: val = "1e39"; break;
        }
        s += key + " " + val + (kind == 4 ? "\n" : "\r\n");
    }
    if (kind == 5) s += std::string(300, 'z') + " toolong\r\n";   // a line of 256+: FileReadLine gives up
    return s;
}
static std::vector<uint8_t> vec(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static void c_opt_get_set(const std::string& sec, const std::string& key) {
    args_reset();
    const char* s = arg_str(sec);
    const char* k = arg_str(key);
    switch (ri(0, 7)) {
    case 0: { float* v = (float*)arg_buf(4); *v = (float)(int)rnd() / 7.0f; ck(0x00471350, OptionsGet_f_rw, fp_get_f, s, k, v); break; }
    case 1: ck(0x004713a0, OptionsSet_f_rw, fp_set_f, s, k, (uint32_t)rnd()); break;
    case 2: { int* v = (int*)arg_buf(4); *v = (int)rnd(); ck(0x004713e0, OptionsGet_i_rw, fp_get_i, s, k, v); break; }
    case 3: ck(0x00471430, OptionsSet_i_rw, fp_set_i, s, k, (int)rnd()); break;
    case 4: { uint8_t* v = (uint8_t*)arg_buf(4); *v = (uint8_t)rnd(); ck(0x00471470, OptionsGet_b_rw, fp_get_b, s, k, v); break; }
    case 5: ck(0x004714c0, OptionsSet_b_rw, fp_set_b, s, k, (uint8_t)rnd()); break;
    case 6: {
        int n = chance(20) ? ri(1, 8) : 0x100;
        char* b = (char*)arg_buf(0x100, 0);
        std::string init = rname(ri(0, 80));
        memcpy(b, init.c_str(), init.size() + 1);
        ck(0x00471500, OptionsGet_s_rw, fp_get_s, s, k, b, n);
        break;
    }
    default: ck(0x00471560, OptionsSet_s_rw, fp_set_s, s, k, (const char*)arg_str(rname(ri(0, 90)))); break;
    }
}
static std::string g_cfg_path;                               // <user dir>options.cfg (the writable file 0)
static long g_opt_overflow_seen;
static void round_options(int r) {
    reset_world();
    // options.def: the game's, or a synthetic one; options.cfg: absent, the install's Config\options.cfg, the last
    // flush (it persists in the writable file), or synthetic
    vfs_mem("options.def", chance(70) ? vec(g_opt_def) : vec(synth_options(chance(80) ? 0 : ri(1, 5))));
    int cfg = ri(0, 3);
    std::vector<uint8_t> cfgbytes;
    if (cfg == 1) read_disk(g_gamedir + "Config\\options.cfg", cfgbytes);
    if (cfg == 3) cfgbytes = vec(synth_options(chance(50) ? 0 : ri(1, 5)));
    if (cfg == 0) C()->wf_len[0] = -1;
    if (cfg == 1 || cfg == 3) {
        uint32_t n = std::min<uint32_t>((uint32_t)cfgbytes.size(), WF_CAP);
        memcpy(g_ar + WF_OFF, cfgbytes.data(), n);
        C()->wf_len[0] = (int32_t)n;
    }
    C()->joy = (uint8_t)chance(50);
    C()->megs = chance(50) ? ri(0, 2) : ri(3, 16);
    ck(0x00470ee0, OptionsBegin_rw, fp_options_begin);
    if (g_opt_count > 0x100) g_opt_overflow_seen++;
    // gets and sets of what is there and what isn't
    for (int i = 0; i < 150; i++) {
        std::string sec, key;
        int n = g_opt_count;
        if (n > 0 && n <= 0x100 && chance(60)) {
            OptionsItem* it = &g_opt_items[rnd() % n];
            sec.assign(it->section, strnlen(it->section, 0x1f));
            key.assign(it->key, strnlen(it->key, 0x1f));
            if (chance(20)) sec = recase(sec);
        } else {
            sec = chance(80) ? "GX" : rname(ri(1, 31));
            key = chance(90) ? rname(ri(1, 20)) : rname(4) + " " + rname(3);
        }
        c_opt_get_set(sec, key);
    }
    // a module's options (a wrong version is allowed there), load_options and find_item directly
    {
        vfs_mem(g_synthdir + "module.opt", vec(synth_options(ri(0, 5))));
        args_reset();
        ck(0x004710e0, OptionsLoadModule_rw, fp_load_module, (const char*)arg_str(chance(90) ? g_synthdir + "module.opt" : g_synthdir + "none.opt"));
        args_reset();
        ck(0x004715b0, load_options_rw, fp_load_options, (const char*)arg_str(g_synthdir + "module.opt"), (uint8_t)chance(50));
        for (int i = 0; i < 20; i++) {
            args_reset();
            OptionsItem** out = (OptionsItem**)arg_buf(4);
            ck(0x00471810, find_item_rw, fp_find_item, out, (const char*)arg_str(chance(50) ? "GLOBAL" : rname(5)),
               (const char*)arg_str(chance(90) ? rname(ri(1, 12)) : "has space"));
        }
    }
    // flush (to the writable file; now and then to a path it can't make), then the end
    if (chance(10)) strcpy(g_opt_path, (g_userdir + "cannot.cfg").c_str());
    ck(0x00471110, OptionsFlush_rw, fp_options_flush);
    if (chance(50)) ck(0x00471330, OptionsEnd_rw, fp_options_end);
}

// ---- locale ---------------------------------------------------------------------------------------------------------------
static std::vector<uint8_t> make_lang(const std::string& lname, int n, uint32_t ver, bool sorted, uint32_t type = 0x4c414e47) {
    std::vector<std::string> keys;
    for (int i = 0; i < n; i++) keys.push_back(chance(20) ? "Units:" + rname(ri(3, 20)) : rname(ri(1, 30)));
    if (sorted) std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    std::vector<uint8_t> p(0x44 + 8 * (size_t)n);
    memset(p.data(), 0, p.size());
    memcpy(p.data(), lname.c_str(), std::min<size_t>(lname.size() + 1, 0x40));
    if (lname.size() >= 0x40) memcpy(p.data(), lname.c_str(), 0x40);
    int32_t cnt = n;
    memcpy(&p[0x40], &cnt, 4);
    for (int i = 0; i < n; i++) {
        uint32_t ko = (uint32_t)p.size();
        p.insert(p.end(), keys[i].begin(), keys[i].end());
        p.push_back(0);
        uint32_t vo = (uint32_t)p.size();
        std::string t = rname(ri(0, 40));
        p.insert(p.end(), t.begin(), t.end());
        p.push_back(0);
        memcpy(&p[0x44 + 8 * i], &ko, 4);
        memcpy(&p[0x48 + 8 * i], &vo, 4);
    }
    return make_ser(type, ver, p);
}
static long g_xlate_keys;
static int lang_keys(const char** out, int max) {           // the current language's keys (read guarded)
    int n = 0;
    __try {
        LangResource* lr = g_lang_res;
        if (lr)
            for (int32_t i = 0; i < lr->count && n < max; i++) {
                const char* k = lr->pairs[i].key;
                volatile size_t len = strlen(k);
                (void)len;
                out[n++] = k;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
static void round_locale(int r) {
    reset_world();
    // the languages: english.lng (the game's), and synthetic ones in the resource directory
    Ctrl* c = C();
    c->nlng = 0;
    auto add_lng = [&](const std::string& n) { if (c->nlng < 24) { strncpy(c->lng[c->nlng], n.c_str(), 0x8f); c->nlng++; } };
    if (chance(90)) add_lng("english.lng");
    int extra = chance(80) ? ri(0, 4) : ri(5, 10);
    for (int i = 0; i < extra; i++) {
        std::string file = rname(ri(3, chance(90) ? 12 : 40)) + ".lng";
        std::string lname = chance(90) ? rname(ri(3, 12)) : rname(ri(30, 60));   // (64+: the next LangInfo runs into this one's file name, and ResourceGet's 64-byte path buffer overflows)
        int kind = ri(0, 19);
        vfs_mem(g_synthdir + file, make_lang(lname, ri(0, 120), kind == 0 ? 3 : 0, kind != 1, kind == 2 ? 0x54455820 : 0x4c414e47));
        add_lng(file);
    }
    if (chance(5)) add_lng("missing.lng");
    // Windows' locale: the real one, or scripted
    c->locale_fake = (uint8_t)chance(50);
    c->currency_fake = (uint8_t)chance(50);
    strcpy(c->fake_lang, chance(50) ? "English" : chance(50) ? "Deutsch" : rname(6).c_str());
    c->fake_measure = chance(50) ? '0' : '1';
    c->fake_decimal = chance(50) ? '.' : ',';
    c->fake_mon = chance(50) ? '.' : ',';
    // options with (sometimes) a language
    std::string opts = "version 1\r\n[GLOBAL]\r\nplayer_name Player\r\n";
    if (chance(50)) opts += "language " + std::string(chance(70) ? "English" : rname(5)) + "\r\n";
    vfs_mem("options.def", vec(opts));
    c->wf_len[0] = -1;
    c->joy = 0;
    c->megs = 8;
    c_begin(g_synthdir, g_gamedir);
    ck(0x00470ee0, OptionsBegin_rw, fp_options_begin);
    if (chance(30)) ck(0x0041a910, enumerate_language_resources_rw, fp_enum_langs);
    ck(0x0041a7f0, LocaleBegin_rw, fp_locale_begin);
    for (int i = 0; i < 3 && c->nlng > 0; i++) {
        args_reset();
        LangResource* lr = (LangResource*)(uintptr_t)(uint32_t)ck(0x0041aa10, get_resource_rw, fp_get_resource,
                                                                  (const char*)arg_str(c->lng[rnd() % c->nlng]));
        if (g_result == 0 && lr) c_forget(lr);
    }
    for (int i = 0; i < 3; i++) ck(0x0041acc0, LocaleSetLang_rw, fp_set_lang, ri(-2, 9));
    // every key of the language, and keys it doesn't have
    std::vector<std::string> keys;
    static const char* raw[5000];
    int nk = lang_keys(raw, 5000);
    for (int i = 0; i < nk; i++) keys.push_back(raw[i]);
    for (auto& k : keys) {
        args_reset();
        const char* p = arg_str(chance(10) ? recase(k) : k);
        ck(0x0041aec0, Xlate_rw, fp_none_s, p);
        if (chance(20)) ck(0x0041af60, CouldXlate_rw, fp_none_s, p);
        if (chance(10)) ck(0x0041aef0, lookup_rw, fp_none_s, p);
        g_xlate_keys++;
    }
    for (int i = 0; i < 40; i++) {
        args_reset();
        const char* p = arg_str(chance(50) ? "Units:" + rname(ri(1, 20)) : rname(ri(0, 40)));
        ck(0x0041aec0, Xlate_rw, fp_none_s, p);
        ck(0x0041af60, CouldXlate_rw, fp_none_s, p);
    }
    // the unit Xlators and the cache
    for (int i = 0; i < 10; i++) {
        switch (ri(0, 3)) {
        case 0: ck(0x0041afa0, Xlator_InvalidateCache_rw, fp_invalidate); break;
        case 1: ck(0x0041ad20, reset_units_rw, fp_reset_units); break;
        default: {
            uint32_t at = k_units[rnd() % 12][0];
            ck(0x0041afb0, Xlator_xlate_rw, fp_xlate, (Xlator*)(uintptr_t)at, 0);
        }
        }
    }
    for (int i = -2; i < 10; i++) ck(0x0041ac90, LocaleGetLang_rw, fp_none_i, i);
    // money, numbers, dates
    const int amounts[] = {0, 1, 5, 99, 100, 105, 150, 1234567, -5, -105, -150, INT_MIN, INT_MAX};
    for (int i = 0; i < 40; i++) {
        args_reset();
        char* out = (char*)arg_buf(0x80, 0x33);
        int a = i < 13 ? amounts[i] : (int)rnd() >> ri(0, 30);
        ck(0x0041ab50, LocaleMoney_rw, fp_money, out, a, (uint8_t)(rnd() & 1));
        char* num = arg_str(chance(50) ? "3.14159" : rname(ri(0, 20)) + "." + rname(3) + ".");
        ck(0x0041abe0, LocaleConvertNumeric_rw, fp_convert_numeric, num);
        char* date = (char*)arg_buf(0x80, 0x44);
        int n = chance(80) ? 0x40 : ri(0, 6);
        int d = chance(90) ? ri(1, 28) : ri(-3, 40), m = chance(90) ? ri(1, 12) : ri(0, 20), y = chance(90) ? ri(1980, 2030) : ri(0, 70000);
        ck(0x0041ac10, LocaleFormatShortDate_rw, fp_short_date, date, n, d, m, y);
    }
    // another language while one is loaded (a panic), the end, a language again, the end
    args_reset();
    ck(0x0041aab0, set_lang_by_name_rw, fp_set_lang_name, (const char*)arg_str(chance(50) ? "English" : rname(5)));
    ck(0x0041ab00, LocaleEnd_rw, fp_locale_end);
    if (g_nlang > 0) {
        ck(0x0041acc0, LocaleSetLang_rw, fp_set_lang, ri(-1, g_nlang));
        args_reset();
        ck(0x0041aec0, Xlate_rw, fp_none_s, (const char*)arg_str("Units:MetersAbbreviation"));
        ck(0x0041ab00, LocaleEnd_rw, fp_locale_end);
    }
    ck(0x00471110, OptionsFlush_rw, fp_options_flush);
    c_end();
}

// ---- telemetry ---------------------------------------------------------------------------------------------------------------
static void round_telemetry(int r) {
    reset_world();
    ck(0x004719a0, TelemetryBegin_rw, fp_tm_begin);
    float* meters = (float*)(g_ar + METER_OFF);
    int big = chance(10);
    int ops = big ? 1400 : 300;
    for (int i = 0; i < ops; i++) {
        args_reset();
        std::string nm = big ? "m" + std::to_string(i) : "metric" + std::to_string(ri(0, 40));
        switch (ri(0, 4)) {
        case 0: ck(0x00471a00, TelemetryCreateMetric_rw, fp_tm_create, (const char*)arg_str(nm)); break;
        case 1:
        case 2: ck(0x00471b70, TelemetrySetAddMeter_rw, fp_tm_add, ri(0, 3), (const char*)arg_str(nm), &meters[ri(0, 1023)]); break;
        case 3: if (g_tm_nmetric > 0 && g_tm_nmetric <= 0x200) ck(0x00471b00, TelemetryUpdateMetric_rw, fp_tm_update, ri(0, g_tm_nmetric - 1), (uint32_t)rnd()); break;
        default: ck(0x00471b20, TelemetrySetUpdate_rw, fp_tm_set_update, ri(0, 3)); break;
        }
    }
    ck(0x004719e0, TelemetryEnd_rw, fp_tm_end);
}

// ---- the evidence for the fix candidates ---------------------------------------------------------------------------------
static void evidence() {
    printf("evidence (the originals' own behaviour):\n");
    // LocaleMoney's "%d.%d"
    reset_world();
    C()->currency_fake = 0;
    args_reset();
    char* out = (char*)arg_buf(0x80);
    ck(0x0041ab50, LocaleMoney_rw, fp_money, out, 105, (uint8_t)1);
    printf("  LocaleMoney(105 cents): \"%s\"   (\"%%d.%%d\" gives 1.5 for 1.05)\n", out);
    ck(0x0041ab50, LocaleMoney_rw, fp_money, out, -50, (uint8_t)1);
    printf("  LocaleMoney(-50 cents): \"%s\"   (the sign of an amount under 1 is lost)\n", out);
    // without cents, the text is cut at the last currency decimal point: a symbol after the amount goes with it
    C()->currency_fake = 1;
    C()->fake_mon = ',';
    *g_mon_sep = ',';
    ck(0x0041ab50, LocaleMoney_rw, fp_money, out, 150, (uint8_t)0);
    printf("  LocaleMoney(150) where Windows gives \"EUR 150,00 EUR\": \"%s\"\n", out);
    C()->currency_fake = 0;
    // '*' names are hunted again every time; two file resources of one base name; a set maximized twice
    {
        reset_world();
        c_begin(g_synthdir, g_gamedir);
        vfs_mem(g_synthdir + "dup.dat", make_ser(0x54455820, 0, rbytes(64)));
        vfs_mem(g_userdir + "dup.dat", make_ser(0x54455820, 0, rbytes(64)));
        const uint8_t* a = c_get("*dup.dat", 0x54455820, 0);
        const uint8_t* b = c_get("*dup.dat", 0x54455820, 0);
        int nodes = 0;
        for (ResourceSetNode* n = g_res_sets; n; n = n->next) nodes++;
        printf("  \"*dup.dat\" fetched twice: %d hunted set(s), the same data: %s\n", nodes, a == b ? "yes" : "no");
        c_forget(a);                                             // forgets the OLDER one...
        bool a_alive = false, b_alive = false;
        for (ResourceSetNode* n = g_res_sets; n; n = n->next) {
            if (n->toc && n->toc->data + 8 == a) a_alive = true;
            if (n->toc && n->toc->data + 8 == b) b_alive = true;
        }
        printf("  forgetting the older of the two frees %s\n",
               !a_alive && b_alive ? "it" : a_alive && !b_alive ? "the NEWER one (whose pointer now dangles; the older leaks)" : "?");
        const uint8_t* u = c_get("~dup.dat", 0x54455820, 0);
        printf("  \"~dup.dat\": its entry is named \"%.16s\" (the '~' kept)\n", u && g_res_sets ? g_res_sets->toc->name : "?");
        // a set maximized twice: mapped twice, the first map never released
        SynSet s = make_synset(0);
        s.name = "maxi.res";
        vfs_mem(g_synthdir + s.name, make_tsr(s.e, 0, 0));
        c_load(s.name, false);
        uint32_t v0 = C()->nviews;
        args_reset();
        const char* nm = arg_str(s.name);
        ck(0x00419a80, ResourceSetMaximize_rw, fp_set_maximize, nm);
        ck(0x00419a80, ResourceSetMaximize_rw, fp_set_maximize, nm);
        uint32_t live = 0;
        for (uint32_t i = 0; i < C()->nviews; i++) live += C()->views[i].prot != PAGE_NOACCESS;
        printf("  a loaded set maximized twice: %u more maps of its file, %u live\n", C()->nviews - v0, live);
        c_unload(s.name);
        live = 0;
        for (uint32_t i = 0; i < C()->nviews; i++) live += C()->views[i].prot != PAGE_NOACCESS;
        printf("  ... and after its unload: %u map(s) still live\n", live);
        c_end();
    }
    // more than 256 options items
    reset_world();
    vfs_mem("options.def", vec(synth_options(3)));
    C()->wf_len[0] = -1;
    C()->megs = 8;
    ck(0x00470ee0, OptionsBegin_rw, fp_options_begin);
    printf("  options: %d items loaded into a 256-item table; the telemetry after it now reads: colour 0x%08x, metric count %d\n",
           (int)g_opt_count, *(uint32_t*)0x00563078, (int)g_tm_nmetric);
    // 513 metrics
    reset_world();
    ck(0x004719a0, TelemetryBegin_rw, fp_tm_begin);
    for (int i = 0; i < 513; i++) {
        args_reset();
        ck(0x00471a00, TelemetryCreateMetric_rw, fp_tm_create, (const char*)arg_str("abcdefgh" + std::to_string(i)));
    }
    printf("  telemetry: after 513 metrics the pending-meter count reads %d (0x%08x)\n", (int)g_tm_npending, (uint32_t)g_tm_npending);
    // 9 languages; a long language name
    reset_world();
    C()->nlng = 0;
    for (int i = 0; i < 9; i++) {
        std::string f = "lang" + std::to_string(i) + ".lng";
        vfs_mem(g_synthdir + f, make_lang(i == 2 ? std::string(40, 'L') : "Lang" + std::to_string(i), 3, 0, true));
        strcpy(C()->lng[C()->nlng++], f.c_str());
    }
    c_begin(g_synthdir, g_gamedir);
    ck(0x0041a910, enumerate_language_resources_rw, fp_enum_langs);
    printf("  languages: 9 .lng files -> language count reads %d (0x%08x); current %d; LangInfo[2].file now \"%.31s\"\n",
           (int)g_nlang, (uint32_t)g_nlang, (int)g_cur_lang, g_langs[2].file);
    // a line-feed-only options file
    reset_world();
    vfs_mem("options.def", vec("version 1\n[GX]\ndraw_distance 0.5\nvideo_mode 2\n"));
    C()->wf_len[0] = -1;
    ck(0x00470ee0, OptionsBegin_rw, fp_options_begin);
    printf("  options.def with LF-only lines: %s (FileReadLine cuts the character before each '\\n')\n",
           g_result == 1 ? "LogPanic (\"version 1\" read as \"version \")" : "loaded");
    // a section name of 40 characters
    reset_world();
    vfs_mem("options.def", vec("version 1\r\n[" + std::string(40, 'S') + "]\r\nfirst 1\r\nsecond 2\r\n"));
    ck(0x00470ee0, OptionsBegin_rw, fp_options_begin);
    printf("  a 40-character section: items \"%.40s\":%s, \"%.40s\":%s\n", g_opt_items[0].section, g_opt_items[0].key,
           g_opt_items[1].section, g_opt_items[1].key);
    c_end();
}

// ---- main ---------------------------------------------------------------------------------------------------------------------
static const char* g_phase = "setup";
static LONG WINAPI unhandled(EXCEPTION_POINTERS* e) {
    printf("UNHANDLED exception %08lx at %08x (reading/writing %08x) during %s / %s, pass %d\n", e->ExceptionRecord->ExceptionCode,
           (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress,
           e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0, g_phase, g_phase_name.c_str(), g_pass);
    fflush(stdout);
    ExitProcess(6);
}
int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    SetUnhandledExceptionFilter(unhandled);
    if (!getenv("VP_WORLD_CHILD")) return relaunch();
    int rounds = argc > 1 ? atoi(argv[1]) : 40;
    g_rng = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 0x2545f491u;
    if (!g_rng) g_rng = 1;
    char exe[MAX_PATH];
    GetModuleFileNameA(0, exe, MAX_PATH);
    std::string root = exe;
    root = root.substr(0, root.find_last_of('\\'));
    // the repository: this program's source tree (the build dir can be anywhere): out\race_v10.exe from the cwd
    if (!load_race_exe("out\\race_v10.exe")) return 2;
    char full[MAX_PATH];
    GetFullPathNameA(argc > 3 ? argv[3] : "..\\game-files\\installs\\v1.0-RC", MAX_PATH, full, 0);
    g_gamedir = full;
    if (g_gamedir.back() != '\\') g_gamedir += '\\';
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    g_synthdir = std::string(tmp) + "vp_k4_virtual\\res\\";       // (nothing is ever created there: the files are in memory)
    g_userdir = std::string(tmp) + "vp_k4_virtual\\user\\";
    g_cfg_path = g_userdir + "options.cfg";
    // the arena, its shadow, the maps' region
    g_ar = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    g_sh = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_map = (uint8_t*)VirtualAlloc(0, MAP_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    g_ww = (ULONG_PTR*)malloc(sizeof(ULONG_PTR) * NPAGES);
    if (!g_ar || !g_sh || !g_map || !g_ww) { printf("can't allocate the arena\n"); return 2; }
    // the game's files: every archive (0TSR), every file resource (0SER), options.def
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_gamedir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no game directory at %s\n", g_gamedir.c_str()); return 2; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string nm = fd.cFileName, path = g_gamedir + nm;
        vfs_disk(path, path);
        FILE* f = fopen(path.c_str(), "rb");
        uint32_t hd[4] = {0, 0, 0, 0};
        if (f) { fread(hd, 4, 4, f); fclose(f); }
        if (hd[0] == 0x52535430 && fd.nFileSizeLow >= 16) g_real.push_back({nm, fd.nFileSizeLow, hd[3], hd[1], hd[2]});
        if (hd[0] == 0x52455330) g_real_ser.push_back(nm);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::vector<uint8_t> def;
    if (!read_disk(g_gamedir + "options.def", def)) { printf("no options.def in %s\n", g_gamedir.c_str()); return 2; }
    g_opt_def.assign(def.begin(), def.end());
    vfs_disk(g_gamedir + "Config\\options.cfg", g_gamedir + "Config\\options.cfg");
    vfs_wf(g_cfg_path, 0);
    vfs_wf(g_userdir + "written.res", 1);
    std::sort(g_real.begin(), g_real.end(), [](const RealSet& a, const RealSet& b) { return a.name < b.name; });
    printf("game dir %s: %zu resource sets, %zu file resources\n", g_gamedir.c_str(), g_real.size(), g_real_ser.size());
    // the image: stubs, the CRT's floating point, the static initialisers the harness needs, the user directory
    memcpy(g_stub_bytes[0], (void*)0x0041a1d0, 5);
    memcpy(g_stub_bytes[1], (void*)0x00471970, 5);
    memcpy(g_stub_bytes[2], (void*)0x0041a620, 5);
    install_stubs();
    ((void(__cdecl*)())0x004ce150)();                             // _cfltcvt_init
    const uint32_t inits[] = {0x0041a660, 0x0041a680, 0x0041a6a0, 0x0041a6c0, 0x0041a6e0, 0x0041a700, 0x0041a720,
                              0x0041a740, 0x0041a760, 0x0041a780, 0x0041a7a0, 0x0041a7c0, 0x0041a7e0};
    for (uint32_t a : inits) ((void(__cdecl*)())(uintptr_t)a)();   // locale's $E5..$E41: `locale`, the 12 unit Xlators
    strcpy((char*)0x00507f48, g_userdir.c_str());                 // Win32GetUserDirectory's buffer
    C()->wf_len[0] = -1;
    for (int i = 1; i < NWF; i++) C()->wf_len[i] = -1;
    ww_sync();
    int nfn = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) nfn++;
    printf("%d rewrites; %d rounds, seed 0x%08x\n", nfn, rounds, g_rng);
    uint32_t seed = g_rng;
    const char* modes = argc > 4 ? argv[4] : "both";
    for (int mode = 0; mode < 2; mode++) {
        if ((mode == 0 && !strcmp(modes, "chain")) || (mode == 1 && !strcmp(modes, "isolated"))) continue;
        g_chain = mode == 1;
        g_rng = seed;
        printf("-- %s --\n", g_chain ? "chain" : "isolated");
        g_phase = "small";
        round_small();
        g_phase = "limits";
        round_limits();
        g_phase = "direct";
        round_direct();
        g_phase = "every real set";
        round_all_real();
        for (int r = 0; r < rounds; r++) {
            g_phase = "resources";
            round_resources(r);
            g_phase = "options";
            if (r % 2 == 0) round_options(r);
            g_phase = "locale";
            if (r % 2 == 1) round_locale(r);
            g_phase = "telemetry";
            if (r % 4 == 0) round_telemetry(r);
        }
        g_phase = "evidence";
        if (!g_chain) evidence();
    }
    printf("\n%-44s %8s %6s %6s %6s %6s %6s %s\n", "function", "checks", "differ", "panics", "faults", "ronly", "fp", "(fault-state, runaway)");
    std::sort(g_stats.begin(), g_stats.end(), [](const Stat& a, const Stat& b) { return a.name < b.name; });
    for (auto& s : g_stats)
        printf("%-44s %8ld %6ld %6ld %6ld %6ld %6ld %ld %ld\n", s.name.c_str(), s.n, s.bad, s.panics, s.faults, s.ronly, s.fpbad, s.fault_state,
               s.runaway);
    printf("\npayloads verified against the files: %ld (differ %ld, not located %ld); 16-character entry names fetched %ld, found %ld\n",
           g_payload_ok, g_payload_bad, g_payload_unknown, g_names16, g_names16_found);
    printf("language keys translated: %ld; options overflow rounds: %ld\n", g_xlate_keys, g_opt_overflow_seen);
    printf("every real set alone: %ld set loads, %ld entries fetched, %ld found, %ld payloads verified\n", g_all_sets, g_all_entries,
           g_all_found, g_all_verified);
    for (auto& m : g_all_missing) printf("  not found: %s\n", m.c_str());
    printf("%ld checks, %ld differ\n", g_checks_total, g_mismatch_total);
    return g_mismatch_total ? 1 : 0;
}
