// world_crt_io.cpp -- M3 stage LIBC, group B: the C runtime's files, heap, strings, ctype / multibyte and the Pentium
// FDIV workaround (hook/crt_io.cpp, crt_heap.cpp, crt_str.cpp, crt_fdiv.cpp) against the originals, outside the game.
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_crt_io.cpp
//        /Fo%TEMP%\crtb\ /Fe%TEMP%\crtb\world_crt_io.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_crt_io.exe [scale] [seed] [only]    (scale 1 = the default counts; only: fdiv|str|ctype|sort|heap|io)
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does (a child process with the range reserved before its
// heap exists) and fills its import table itself. The C runtime's Windows calls are stubs that log every call with its
// arguments (strings and buffers hashed) and make it deterministic: the heap is this harness's own (HeapCreate /
// HeapAlloc / HeapReAlloc / HeapFree / HeapSize on a fixed arena, first-fit by address, so the same calls give the same
// addresses in every pass), file handles are virtualised (the same small numbers in every pass), paths stay in a
// sandbox directory rebuilt to the same files for every pass, GetStdHandle answers "none" (as for the game, a GUI
// program started without console handles). The locale, code-page and character-type APIs are the real ones. The
// C runtime is started the way WinMainCRTStartup does (the originals of _heap_init, _ioinit, __initmbctable,
// __initstdio), and .data is snapshotted after it: every pass starts from that.
//
// Sections (each compares the originals against the rewrites, bit for bit; most run every case three ways: the
// original, the rewrite called directly -- "isolated", what it calls is the original -- and every rewrite hooked into
// the image with the original's entry called -- "chain", the rewrites calling each other):
//   fdiv   every __adj_fdiv_* / fprem / fptan routine, fdiv_main_routine and fprem_common through them,
//          __ms_p5_test_fdiv and __chkstk, called through an assembly thunk with random x87 stacks (depth 1..8; normal,
//          denormal, unnormal, zero, infinite, NaN and pseudo values; divisors with the flawed-table mantissa patterns;
//          fprem pairs whose exponents differ by up to 100), every eax code of __adj_fdiv_r with a full stack, random
//          precision and rounding control, sometimes an exception unmasked: all eight registers, the tag and status
//          words, the control word, eax, ecx, edx, ebx and the stack pointer (or the same fault). Three ways. Then two of
//          the game's own functions that divide through the workaround (MatrixNormalize, MatrixToQuat) with
//          __adjust_fdiv forced to 1, originals vs the rewrites hooked in; __ms_p5_mp_test_fdiv's answer and calls. And
//          the entry points for comparing the generated code with the originals' instruction by instruction
//          (%TEMP%\crt_fdiv_code.txt).
//   str    strchr, strrchr, strstr, strncmp, strncpy, strncat, memmove, stricmp, strnicmp, strncnt, swap on random
//          strings (a small alphabet: case pairs, high bytes, NULs), overlapping buffers included, and a check that each
//          original writes only inside its rewrite's footprint; strncat with a count of 0 beside an unmapped page.
//   ctype  isdigit, isspace, isctype, tolower, toupper, _ismbbtype / _ismbblead, _mbctoupper, mbtowc, wctomb,
//          __crtLCMapStringA, __crtGetStringTypeA and stricmp / strnicmp's locale paths for every character
//          -1..0x10000 (every one to 0x200) and random ones, in the "C" locale and with __lc_handle / __lc_codepage /
//          __mb_cur_max set as setlocale would (1252, 932), also with the two probes forced onto their wide-character
//          paths; _setmbcp (each code page from each other's state) / __initmbctable / _getSystemCP / CPtoLCID /
//          setSBCS for the CRT's four table code pages, the system's, OEM, 1252, 437, 850, 65001, invalid ones. Three
//          ways; compared: the result, the CRT's .data and .bss, the heap and the API calls.
//   sort   qsort, shortsort, swap and bsearch on random arrays (width 1..24) with keys that repeat: the resulting ORDER
//          of equal elements, and every comparator call (its two elements, in order). Three ways.
//   heap   random malloc / calloc / realloc / free / _msize / _nh_malloc / _heap_alloc sequences: results, the heap's
//          layout and every Heap* call. Three ways.
//   io     random scripts over the streams, the low-level handles and the directories (fopen modes, fread / fwrite /
//          getc / putc / ungetc / fseek / ftell / fflush / fclose in text and binary mode, CR LF, a lone CR at a
//          buffer's end, ^Z, a file whose CR LF straddles the 4096-byte buffer read in small pieces, a pipe, _sopen flags
//          and sharing, _read / _write / _lseek / _chsize / _commit / _setmode / _isatty / _close, _getcwd / _getdcwd /
//          _chdir / _fullpath, the handle table past 32, __initstdio / __endstdio, every error path). Three ways;
//          compared: every result, the API log, the whole of .data, the heap and the sandbox's files.
// Built VP_FAITHFUL: the rewrites exactly as the originals (group B has no fixes).
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>
#define VP_FAITHFUL
#include "../hook/port.h"
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                 // vp_guard: the GCC build's __try / __except
#endif

struct ChainReg {
    uint32_t at; void* fn; const char* name; ChainReg* next;
    static ChainReg*& head() { static ChainReg* h; return h; }
    ChainReg(uint32_t a, void* f, const char* n) : at(a), fn(f), name(n), next(head()) { head() = this; }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;                                                        \
    static ChainReg VP_CAT(chain_, NEW)(V10, (void*)&NEW, NAME);
#define CRT_NO_SHADOW(NEW)

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
// footprints: recorded for the originals' write check
static Footprint* g_fp_rec;
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 0x40, what); }

#include "../hook/crt_str.cpp"
#include "../hook/crt_heap.cpp"
#include "../hook/crt_io.cpp"
#include "../hook/crt_fdiv.cpp"

// ---- random values, hashes ------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static uint32_t fnv(const void* p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static uint32_t hs(const char* s) { return s ? fnv(s, strlen(s)) : 0xdead; }
static uint32_t hws(const wchar_t* s, int n) {
    if (!s) return 0xdead;
    if (n < 0) n = (int)wcslen(s) + 1;
    return fnv(s, (size_t)n * 2);
}

// ---- the original, loaded at 0x400000 --------------------------------------------------------------------------------
enum { DATA_AT = 0x004e1000, DATA_SIZE = 0xf5f2c };
static IMAGE_NT_HEADERS* g_nt;
static uint8_t* g_file;
static bool load_race_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = (uint8_t*)malloc(n);
    fread(g_file, 1, n, f);
    fclose(f);
    g_nt = (IMAGE_NT_HEADERS*)(g_file + ((IMAGE_DOS_HEADER*)g_file)->e_lfanew);
    if (g_nt->FileHeader.TimeDateStamp != 0x362de68c) { printf("%s isn't the v1.0 race.exe\n", path); return false; }
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, g_nt->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) { printf("0x400000 isn't reserved for race.exe in this process\n"); return false; }
    memcpy(base, g_file, g_nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(g_nt);
    for (int i = 0; i < g_nt->FileHeader.NumberOfSections; i++)
        memcpy(base + sec[i].VirtualAddress, g_file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData
                                                                                         : sec[i].Misc.VirtualSize);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_CRTB_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) return 2;
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// ---- the chain: every rewrite hooked into the image ------------------------------------------------------------------
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}
struct ChainSave { uint32_t at; uint8_t b[5]; };
static ChainSave g_chain_save[512];
static int g_nchain;
// each hooked entry goes through a counting thunk (inc dword [count]; jmp rewrite): which rewrites the chain ran
static uint8_t* g_count_thunks;
static std::map<uint32_t, uint32_t*> g_chain_count;
static bool (*g_chain_only)(uint32_t);               // a filter: patch only these (0: all)
static void chain_patch() {
    g_nchain = 0;
    if (!g_count_thunks) {
        g_count_thunks = (uint8_t*)VirtualAlloc(0, 0x10000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        int k = 0;
        for (ChainReg* r = ChainReg::head(); r; r = r->next, k++) {
            uint8_t* t = g_count_thunks + 16 * k;
            uint32_t* c = (uint32_t*)(g_count_thunks + 0x8000 + 4 * k);
            g_chain_count[r->at] = c;
            t[0] = 0xff;
            t[1] = 0x05;
            const uint32_t ca = (uint32_t)(uintptr_t)c;
            memcpy(t + 2, &ca, 4);
            t[6] = 0xe9;
            const int32_t rel = (int32_t)((uintptr_t)r->fn - ((uintptr_t)t + 11));
            memcpy(t + 7, &rel, 4);
        }
    }
    int k = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next, k++) {
        if (g_chain_only && !g_chain_only(r->at)) continue;
        ChainSave& s = g_chain_save[g_nchain++];
        s.at = r->at;
        memcpy(s.b, (void*)r->at, 5);
        patch_jmp(r->at, g_count_thunks + 16 * k);
    }
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}
static void chain_unpatch() {
    for (int i = g_nchain - 1; i >= 0; i--) memcpy((void*)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_nchain = 0;
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}
static void* rewrite_of(uint32_t at) {
    for (ChainReg* r = ChainReg::head(); r; r = r->next)
        if (r->at == at) return r->fn;
    printf("no rewrite registered at %08x\n", at);
    ExitProcess(3);
    return 0;
}
static const char* name_of(uint32_t at) {
    for (ChainReg* r = ChainReg::head(); r; r = r->next)
        if (r->at == at) return r->name;
    return "?";
}

// ---- the API log ---------------------------------------------------------------------------------------------------------
static std::vector<uint32_t> g_log;
static bool g_logging = true;
static void lg(uint32_t kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0, uint32_t f = 0) {
    if (!g_logging) return;
    const uint32_t v[7] = {kind, a, b, c, d, e, f};
    g_log.insert(g_log.end(), v, v + 7);
}
static uint32_t P(const void* p) { return (uint32_t)(uintptr_t)p; }
// a pointer that may be into the caller's frame (the originals' and the rewrites' frames differ): logged as a marker
static uint32_t PS(const void* p) {
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    if (p >= tib->StackLimit && p < tib->StackBase) return 0x57ac0000u;
    return P(p);
}

// ---- the deterministic heap ------------------------------------------------------------------------------------------------
// One arena; blocks {size, cap, live, pad} + data, first-fit by address over freed blocks (split, no coalescing), new
// blocks at the top. Fresh memory reads as 0xcd. The same calls give the same addresses in every pass.
enum { HEAP_ARENA = 48u << 20 };
static uint8_t* g_heap;
static uint32_t g_heap_top, g_heap_top0, g_heap_hi;
static const HANDLE FAKE_HEAP = (HANDLE)(uintptr_t)0x5eed0000;
struct HBlk { uint32_t size, cap, live, pad; };
static void* heap_alloc_(uint32_t n, bool zero) {
    const uint32_t cap = (n + 15) & ~15u;
    if (n > HEAP_ARENA / 2) return 0;
    for (uint32_t o = 0; o < g_heap_top;) {
        HBlk* b = (HBlk*)(g_heap + o);
        if (!b->live && b->cap >= cap) {
            if (b->cap >= cap + 32) {                                   // split
                HBlk* r = (HBlk*)(g_heap + o + 16 + cap);
                r->cap = b->cap - cap - 16;
                r->size = 0;
                r->live = 0;
                b->cap = cap;
            }
            b->size = n;
            b->live = 1;
            memset(b + 1, zero ? 0 : 0xcd, b->cap);
            return b + 1;
        }
        o += 16 + b->cap;
    }
    if (g_heap_top + 16 + cap > HEAP_ARENA) return 0;
    HBlk* b = (HBlk*)(g_heap + g_heap_top);
    b->size = n;
    b->cap = cap;
    b->live = 1;
    b->pad = 0;
    g_heap_top += 16 + cap;
    if (g_heap_top > g_heap_hi) g_heap_hi = g_heap_top;
    memset(b + 1, zero ? 0 : 0xcd, cap);
    return b + 1;
}
static HBlk* heap_blk(void* p) {
    if ((uint8_t*)p < g_heap + 16 || (uint8_t*)p >= g_heap + g_heap_top) return 0;
    return (HBlk*)p - 1;
}
static HANDLE WINAPI st_HeapCreate(DWORD o, SIZE_T a, SIZE_T b) { lg(1, o, (uint32_t)a, (uint32_t)b); return FAKE_HEAP; }
static LPVOID WINAPI st_HeapAlloc(HANDLE h, DWORD fl, SIZE_T n) {
    void* p = h == FAKE_HEAP ? heap_alloc_((uint32_t)n, (fl & HEAP_ZERO_MEMORY) != 0) : 0;
    lg(2, P(h), fl, (uint32_t)n, P(p));
    return p;
}
static BOOL WINAPI st_HeapFree(HANDLE h, DWORD fl, LPVOID p) {
    HBlk* b = heap_blk(p);
    BOOL ok = h == FAKE_HEAP && b && b->live;
    if (ok) {
        b->live = 0;
        memset(p, 0xdd, b->cap);
    }
    lg(3, P(h), fl, P(p), ok);
    return ok;
}
static SIZE_T WINAPI st_HeapSize(HANDLE h, DWORD fl, LPCVOID p) {
    HBlk* b = heap_blk((void*)p);
    SIZE_T r = h == FAKE_HEAP && b && b->live ? b->size : (SIZE_T)-1;
    lg(4, P(h), fl, P(p), (uint32_t)r);
    return r;
}
static LPVOID WINAPI st_HeapReAlloc(HANDLE h, DWORD fl, LPVOID p, SIZE_T n) {
    HBlk* b = heap_blk(p);
    void* q = 0;
    if (h == FAKE_HEAP && b && b->live) {
        if (((uint32_t)n + 15 & ~15u) <= b->cap) {
            b->size = (uint32_t)n;
            q = p;
        } else if ((q = heap_alloc_((uint32_t)n, false)) != 0) {
            memcpy(q, p, b->size);
            b->live = 0;
            memset(p, 0xdd, b->cap);
        }
    }
    lg(5, P(h), fl, P(p), (uint32_t)n, P(q));
    return q;
}

// ---- file handles, virtualised ---------------------------------------------------------------------------------------------
enum { NVH = 256 };
static HANDLE g_vh[NVH];
static int g_nvh;
static HANDLE vreal(HANDLE v) {
    const uint32_t x = (uint32_t)(uintptr_t)v;
    if (x >= 0x1000 && x < 0x1000 + 4 * NVH && !(x & 3)) return g_vh[(x - 0x1000) / 4];
    return 0;
}
static void vclose_all() {
    for (int i = 0; i < g_nvh; i++)
        if (g_vh[i]) CloseHandle(g_vh[i]), g_vh[i] = 0;
    g_nvh = 0;
}
static char g_sb[MAX_PATH];                       // the sandbox
static HANDLE WINAPI st_CreateFileA(LPCSTR n, DWORD acc, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cr, DWORD fl, HANDLE t) {
    HANDLE r = INVALID_HANDLE_VALUE, h;
    char full[1024];
    if (n && GetFullPathNameA(n, sizeof full, full, 0) && !_strnicmp(full, g_sb, strlen(g_sb)) && g_nvh < NVH) {
        h = CreateFileA(n, acc, sh, sa, cr, fl, t);
        if (h != INVALID_HANDLE_VALUE) {
            g_vh[g_nvh] = h;
            r = (HANDLE)(uintptr_t)(0x1000 + 4 * g_nvh++);
        }
    } else {
        SetLastError(ERROR_ACCESS_DENIED);
    }
    const DWORD e = GetLastError();
    lg(10, hs(n), acc, sh, sa ? fnv(sa, sizeof *sa) : 0, cr, fl ^ P(r) * 31);
    SetLastError(e);
    return r;
}
static BOOL WINAPI st_ReadFile(HANDLE v, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED o) {
    HANDLE h = vreal(v);
    BOOL ok = FALSE;
    DWORD g = 0;
    if (h) ok = ReadFile(h, buf, n, &g, o);
    else SetLastError(ERROR_INVALID_HANDLE);
    if (got) *got = g;
    const DWORD e = GetLastError();
    lg(11, P(v), n, ok, g, fnv(buf, ok ? g : 0), ok ? 0 : e);
    SetLastError(e);
    return ok;
}
static BOOL WINAPI st_WriteFile(HANDLE v, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED o) {
    HANDLE h = vreal(v);
    BOOL ok = FALSE;
    DWORD w = 0;
    if (h) ok = WriteFile(h, buf, n, &w, o);
    else SetLastError(ERROR_INVALID_HANDLE);
    if (put) *put = w;
    const DWORD e = GetLastError();
    lg(12, P(v), n, ok, w, fnv(buf, n), ok ? 0 : e);
    SetLastError(e);
    return ok;
}
static DWORD WINAPI st_SetFilePointer(HANDLE v, LONG d, PLONG hi, DWORD m) {
    HANDLE h = vreal(v);
    DWORD r = INVALID_SET_FILE_POINTER;
    if (h) r = SetFilePointer(h, d, hi, m);
    else SetLastError(ERROR_INVALID_HANDLE);
    const DWORD e = GetLastError();
    lg(13, P(v), (uint32_t)d, PS(hi), m, r, r == INVALID_SET_FILE_POINTER ? e : 0);
    SetLastError(e);
    return r;
}
static BOOL WINAPI st_CloseHandle(HANDLE v) {
    HANDLE h = vreal(v);
    BOOL ok = FALSE;
    if (h) {
        ok = CloseHandle(h);
        g_vh[((uint32_t)(uintptr_t)v - 0x1000) / 4] = 0;
    } else {
        SetLastError(ERROR_INVALID_HANDLE);
    }
    const DWORD e = GetLastError();
    lg(14, P(v), ok);
    SetLastError(e);
    return ok;
}
static DWORD WINAPI st_GetFileType(HANDLE v) {
    HANDLE h = vreal(v);
    DWORD r = h ? GetFileType(h) : 0;
    if (!h) SetLastError(ERROR_INVALID_HANDLE);
    const DWORD e = GetLastError();
    lg(15, P(v), r);
    SetLastError(e);
    return r;
}
static BOOL WINAPI st_FlushFileBuffers(HANDLE v) {
    HANDLE h = vreal(v);
    BOOL ok = h ? FlushFileBuffers(h) : FALSE;
    if (!h) SetLastError(ERROR_INVALID_HANDLE);
    const DWORD e = GetLastError();
    lg(16, P(v), ok);
    SetLastError(e);
    return ok;
}
static BOOL WINAPI st_SetEndOfFile(HANDLE v) {
    HANDLE h = vreal(v);
    BOOL ok = h ? SetEndOfFile(h) : FALSE;
    if (!h) SetLastError(ERROR_INVALID_HANDLE);
    const DWORD e = GetLastError();
    lg(17, P(v), ok);
    SetLastError(e);
    return ok;
}
static HANDLE WINAPI st_GetStdHandle(DWORD n) { lg(18, n); return INVALID_HANDLE_VALUE; }
static BOOL WINAPI st_SetStdHandle(DWORD n, HANDLE h) { lg(19, n, P(h)); return TRUE; }
static UINT WINAPI st_SetHandleCount(UINT n) { lg(20, n); return n; }
static void WINAPI st_GetStartupInfoA(LPSTARTUPINFOA si) {
    memset(si, 0, sizeof *si);
    si->cb = sizeof *si;
    lg(21);
}
static DWORD WINAPI st_GetFullPathNameA(LPCSTR n, DWORD len, LPSTR buf, LPSTR* part) {
    DWORD r = GetFullPathNameA(n, len, buf, part);
    const DWORD e = GetLastError();
    lg(22, hs(n), len, PS(buf), r, r && r < len ? hs(buf) : 0);
    SetLastError(e);
    return r;
}
static DWORD WINAPI st_GetCurrentDirectoryA(DWORD len, LPSTR buf) {
    DWORD r = GetCurrentDirectoryA(len, buf);
    const DWORD e = GetLastError();
    lg(23, len, PS(buf), r, r && r < len ? hs(buf) : 0);
    SetLastError(e);
    return r;
}
static BOOL WINAPI st_SetCurrentDirectoryA(LPCSTR n) {
    BOOL ok = FALSE;
    char full[1024];
    if (n && GetFullPathNameA(n, sizeof full, full, 0) && !_strnicmp(full, g_sb, strlen(g_sb))) ok = SetCurrentDirectoryA(n);
    else SetLastError(ERROR_PATH_NOT_FOUND);
    const DWORD e = GetLastError();
    lg(24, hs(n), ok, ok ? 0 : e);
    SetLastError(e);
    return ok;
}
static BOOL WINAPI st_SetEnvironmentVariableA(LPCSTR n, LPCSTR v) { lg(25, hs(n), hs(v)); return TRUE; }
static UINT WINAPI st_GetDriveTypeA(LPCSTR n) {
    UINT r = GetDriveTypeA(n);
    lg(26, hs(n), r);
    return r;
}
static int WINAPI st_LCMapStringA(LCID l, DWORD f, LPCSTR s, int n, LPSTR d, int dn) {
    int r = LCMapStringA(l, f, s, n, d, dn);
    lg(30, l, f, fnv(s, n > 0 ? n : 0) ^ (uint32_t)n, (d != 0) ^ (uint32_t)dn, r, d && r > 0 && dn ? fnv(d, (size_t)r) : 0);
    return r;
}
static int WINAPI st_LCMapStringW(LCID l, DWORD f, LPCWSTR s, int n, LPWSTR d, int dn) {
    int r = LCMapStringW(l, f, s, n, d, dn);
    lg(31, l, f, hws(s, n), (d != 0) ^ (uint32_t)dn, r, d && r > 0 && dn ? fnv(d, (size_t)r * 2) : 0);
    return r;
}
static BOOL WINAPI st_GetStringTypeA(LCID l, DWORD t, LPCSTR s, int n, LPWORD o) {
    BOOL r = GetStringTypeA(l, t, s, n, o);
    lg(32, l, t, fnv(s, n > 0 ? n : 0) ^ (uint32_t)n, r, r && n > 0 ? fnv(o, (size_t)n * 2) : 0);
    return r;
}
static BOOL WINAPI st_GetStringTypeW(DWORD t, LPCWSTR s, int n, LPWORD o) {
    BOOL r = GetStringTypeW(t, s, n, o);
    lg(33, t, hws(s, n), (uint32_t)n, r, r && n > 0 ? fnv(o, (size_t)n * 2) : 0);
    return r;
}
static int WINAPI st_MultiByteToWideChar(UINT cp, DWORD f, LPCSTR s, int n, LPWSTR d, int dn) {
    int r = MultiByteToWideChar(cp, f, s, n, d, dn);
    lg(34, cp, f, fnv(s, n > 0 ? n : 0) ^ (uint32_t)n, (d != 0) ^ (uint32_t)dn, r, d && r > 0 ? fnv(d, (size_t)r * 2) : 0);
    return r;
}
static int WINAPI st_WideCharToMultiByte(UINT cp, DWORD f, LPCWSTR s, int n, LPSTR d, int dn, LPCSTR dc, LPBOOL used) {
    int r = WideCharToMultiByte(cp, f, s, n, d, dn, dc, used);
    lg(35, cp, f, hws(s, n), (d != 0) ^ (uint32_t)dn, r, (d && r > 0 ? fnv(d, (size_t)r) : 0) ^ (used ? *used : 7));
    return r;
}
static BOOL WINAPI st_GetCPInfo(UINT cp, LPCPINFO i) {
    BOOL r = GetCPInfo(cp, i);
    lg(36, cp, r, r ? fnv(i, 18) : 0);          // (not the struct's 2 bytes of padding)
    return r;
}
static UINT WINAPI st_GetACP() { lg(37); return GetACP(); }
static UINT WINAPI st_GetOEMCP() { lg(38); return GetOEMCP(); }
static HMODULE WINAPI st_LoadLibraryA(LPCSTR n) { lg(39, hs(n)); return LoadLibraryA(n); }
static FARPROC WINAPI st_GetProcAddress(HMODULE m, LPCSTR n) { lg(40, hs(n)); return GetProcAddress(m, n); }

struct Stub { const char* name; void* fn; };
static const Stub k_stubs[] = {
    {"HeapCreate", (void*)&st_HeapCreate}, {"HeapAlloc", (void*)&st_HeapAlloc}, {"HeapFree", (void*)&st_HeapFree},
    {"HeapSize", (void*)&st_HeapSize}, {"HeapReAlloc", (void*)&st_HeapReAlloc}, {"CreateFileA", (void*)&st_CreateFileA},
    {"ReadFile", (void*)&st_ReadFile}, {"WriteFile", (void*)&st_WriteFile}, {"SetFilePointer", (void*)&st_SetFilePointer},
    {"CloseHandle", (void*)&st_CloseHandle}, {"GetFileType", (void*)&st_GetFileType},
    {"FlushFileBuffers", (void*)&st_FlushFileBuffers}, {"SetEndOfFile", (void*)&st_SetEndOfFile},
    {"GetStdHandle", (void*)&st_GetStdHandle}, {"SetStdHandle", (void*)&st_SetStdHandle},
    {"SetHandleCount", (void*)&st_SetHandleCount}, {"GetStartupInfoA", (void*)&st_GetStartupInfoA},
    {"GetFullPathNameA", (void*)&st_GetFullPathNameA}, {"GetCurrentDirectoryA", (void*)&st_GetCurrentDirectoryA},
    {"SetCurrentDirectoryA", (void*)&st_SetCurrentDirectoryA},
    {"SetEnvironmentVariableA", (void*)&st_SetEnvironmentVariableA}, {"GetDriveTypeA", (void*)&st_GetDriveTypeA},
    {"LCMapStringA", (void*)&st_LCMapStringA}, {"LCMapStringW", (void*)&st_LCMapStringW},
    {"GetStringTypeA", (void*)&st_GetStringTypeA}, {"GetStringTypeW", (void*)&st_GetStringTypeW},
    {"MultiByteToWideChar", (void*)&st_MultiByteToWideChar}, {"WideCharToMultiByte", (void*)&st_WideCharToMultiByte},
    {"GetCPInfo", (void*)&st_GetCPInfo}, {"GetACP", (void*)&st_GetACP}, {"GetOEMCP", (void*)&st_GetOEMCP},
    {"LoadLibraryA", (void*)&st_LoadLibraryA}, {"GetProcAddress", (void*)&st_GetProcAddress},
};
static void __cdecl trap_import(uint32_t slot) {
    printf("an unexpected import was called (IAT slot %08x): stopping\n", slot);
    ExitProcess(5);
}
static int g_nstubbed, g_nreal, g_ntrapped;
static void install_iat() {
    uint8_t* base = (uint8_t*)0x400000;
    IMAGE_DATA_DIRECTORY dir = g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    uint8_t* thunks = (uint8_t*)VirtualAlloc(0, 0x10000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    int nt = 0;
    HMODULE k32m = GetModuleHandleA("kernel32.dll");
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.Size && d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        const bool k32 = _stricmp(dll, "KERNEL32.dll") == 0;
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            const char* fn = names->u1.Ordinal & IMAGE_ORDINAL_FLAG32 ? "" : (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            void* to = 0;
            if (k32)
                for (auto& s : k_stubs)
                    if (!strcmp(s.name, fn)) to = s.fn;
            if (to) g_nstubbed++;
            else if (k32 && *fn && (to = (void*)GetProcAddress(k32m, fn)) != 0) g_nreal++;
            else {
                uint8_t* t = thunks + 16 * nt++;
                const uint32_t slot = (uint32_t)(uintptr_t)&iat->u1.Function, fnp = (uint32_t)(uintptr_t)&trap_import;
                t[0] = 0x68; memcpy(t + 1, &slot, 4);
                t[5] = 0xb8; memcpy(t + 6, &fnp, 4);
                t[10] = 0xff; t[11] = 0xd0;
                to = t;
                g_ntrapped++;
            }
            iat->u1.Function = (DWORD)(uintptr_t)to;
        }
    }
}
// the CRT's fatal paths: _amsg_exit (0x4cf730) and _NMSG_WRITE end the child
static void __cdecl fatal_amsg(int code) {
    printf("the C runtime's _amsg_exit(%d) was called: stopping\n", code);
    ExitProcess(6);
}

// ---- fault guard ---------------------------------------------------------------------------------------------------------
template <typename F> static __declspec(noinline) DWORD guarded(F&& f) {
#if defined(__GNUC__) && !defined(__clang__)
    return vp_guard([](void* p) { (*(F*)p)(); }, (void*)&f);
#else
    __try {
        f();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
#endif
}

// ---- the state every pass starts from -------------------------------------------------------------------------------------
static std::vector<uint8_t> g_data0, g_heap0;
static void state_restore() {
    memcpy((void*)DATA_AT, g_data0.data(), DATA_SIZE);
    memcpy(g_heap, g_heap0.data(), g_heap0.size());
    if (g_heap_hi > g_heap_top0) memset(g_heap + g_heap_top0, 0, g_heap_hi - g_heap_top0);
    g_heap_top = g_heap_top0;
    g_log.clear();
}
struct Snap { std::vector<uint8_t> data, heap; std::vector<uint32_t> log; uint32_t heap_top; };
static void snap(Snap& s) {
    s.data.assign((uint8_t*)DATA_AT, (uint8_t*)DATA_AT + DATA_SIZE);
    s.heap.assign(g_heap, g_heap + g_heap_top);
    s.heap_top = g_heap_top;
    s.log = g_log;
}
static const char* data_name(uint32_t va) {
    static char b[64];
    struct { uint32_t a, n; const char* nm; } k[] = {
        {0x502730, 4, "errno"}, {0x502734, 4, "_doserrno"}, {0x503430, 0x280, "_iob"}, {0x5d6e14, 4, "_nhandle"},
        {0x5d6e20, 0x100, "__pioinfo"}, {0x5d5dfc, 4, "_nstream"}, {0x5d5e00, 4, "__piob"}, {0x502b00, 0x101, "_mbctype"},
        {0x502c04, 0x1c, "__mbcodepage.."}, {0x5033fc, 4, "aw_map f_use"}, {0x50373c, 4, "aw_str f_use"},
        {0x5036b0, 4, "_cflush"}, {0x503428, 8, "_stdbuf"}};
    for (auto& x : k)
        if (va >= x.a && va < x.a + x.n) { snprintf(b, sizeof b, "%s+0x%x", x.nm, va - x.a); return b; }
    snprintf(b, sizeof b, ".data %08x", va);
    return b;
}
// first difference, described (0: none)
static std::string snap_diff(const Snap& a, const Snap& b) {
    char t[256];
    for (uint32_t i = 0; i < DATA_SIZE; i++)
        if (a.data[i] != b.data[i]) {
            snprintf(t, sizeof t, "%s: %02x vs %02x", data_name(DATA_AT + i), a.data[i], b.data[i]);
            return t;
        }
    if (a.heap_top != b.heap_top) {
        snprintf(t, sizeof t, "heap top %x vs %x", a.heap_top, b.heap_top);
        return t;
    }
    for (size_t i = 0; i < a.heap.size(); i++)
        if (a.heap[i] != b.heap[i]) {
            snprintf(t, sizeof t, "heap +0x%x: %02x vs %02x", (unsigned)i, a.heap[i], b.heap[i]);
            return t;
        }
    const size_t n = std::min(a.log.size(), b.log.size());
    for (size_t i = 0; i < n; i++)
        if (a.log[i] != b.log[i]) {
            const size_t e = i / 7 * 7;
            snprintf(t, sizeof t, "API call %u: kind %u (%08x %08x %08x %08x %08x %08x) vs kind %u (%08x %08x %08x %08x %08x %08x)",
                     (unsigned)(i / 7), a.log[e], a.log[e + 1], a.log[e + 2], a.log[e + 3], a.log[e + 4], a.log[e + 5], a.log[e + 6],
                     b.log[e], b.log[e + 1], b.log[e + 2], b.log[e + 3], b.log[e + 4], b.log[e + 5], b.log[e + 6]);
            return t;
        }
    if (a.log.size() != b.log.size()) {
        snprintf(t, sizeof t, "API calls %u vs %u", (unsigned)(a.log.size() / 7), (unsigned)(b.log.size() / 7));
        return t;
    }
    return "";
}

// ---- results bookkeeping ---------------------------------------------------------------------------------------------------
struct Stat { long calls = 0, fails = 0; };
static std::map<std::string, Stat> g_stats;
static long g_failures;
static void fail(const char* section, const char* fmt, ...) {
    g_failures++;
    if (g_failures > 40) return;
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("  MISMATCH [%s] %s\n", section, b);
}

// =========================================================================================================================
// fdiv: the x87 routines
// =========================================================================================================================
struct FpuIn {
    uint8_t reg[8][10];          // st(0) .. st(depth-1)
    int32_t depth;
    uint16_t cw;
    uint32_t eax;
    uint32_t arg[2];
    int32_t nargs;               // dwords pushed before the call
};
struct FpuOut {
    uint8_t save[108];
    uint32_t eax, ecx, edx, ebx;
    int32_t esp_delta;
    DWORD fault;
};
static FpuIn g_fin;
static FpuOut g_fout;
static uint32_t g_ftarget, g_fsaved_esp, g_fesp_before, g_fesp_after, g_fr[4];
static const uint8_t k_zero10[10] = {};
static __declspec(naked) void fpu_thunk() {
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile(
        "pushad\n\t"
        "mov dword ptr [%c0], esp\n\t"
        "sub esp, 0x800\n\t"
        "mov edi, esp\n\t"
        "xor eax, eax\n\t"
        "mov ecx, 0x200\n\t"
        "cld\n\t"
        "rep stosd\n\t"
        "add esp, 0x800\n\t"
        "finit\n\t"
        "fldz\n\t"
        "fldz\n\t"
        "fldz\n\t"
        "fldz\n\t"
        "fldz\n\t"
        "fldz\n\t"
        "fldz\n\t"
        "fldz\n\t"
        "finit\n\t"
        "fldcw word ptr [%c1]\n\t"
        "mov ecx, dword ptr [%c2]\n"
        "ld%=:\n\t"
        "test ecx, ecx\n\t"
        "jz loaded%=\n\t"
        "dec ecx\n\t"
        "lea eax, [ecx + ecx * 4]\n\t"
        "fld tbyte ptr [%c3 + eax * 2]\n\t"
        "jmp ld%=\n"
        "loaded%=:\n\t"
        "mov ecx, dword ptr [%c4]\n\t"
        "cmp ecx, 2\n\t"
        "jne one%=\n\t"
        "push dword ptr [%c5 + 4]\n\t"
        "push dword ptr [%c5]\n\t"
        "jmp go%=\n"
        "one%=:\n\t"
        "cmp ecx, 1\n\t"
        "jne go%=\n\t"
        "push dword ptr [%c5]\n"
        "go%=:\n\t"
        "mov dword ptr [%c6], esp\n\t"
        "mov eax, dword ptr [%c7]\n\t"
        "mov ecx, 0x11111111\n\t"
        "mov edx, 0x22222222\n\t"
        "mov ebx, 0x33333333\n\t"
        "call dword ptr [%c8]\n\t"
        "mov dword ptr [%c9], esp\n\t"
        "mov dword ptr [%c10], eax\n\t"
        "mov dword ptr [%c10 + 4], ecx\n\t"
        "mov dword ptr [%c10 + 8], edx\n\t"
        "mov dword ptr [%c10 + 12], ebx\n\t"
        "fnsave [%c11]\n\t"
        "mov esp, dword ptr [%c0]\n\t"
        "popad\n\t"
        "ret"
        : : "i"(&g_fsaved_esp), "i"(&g_fin.cw), "i"(&g_fin.depth), "i"(&g_fin.reg), "i"(&g_fin.nargs),
            "i"(&g_fin.arg), "i"(&g_fesp_before), "i"(&g_fin.eax), "i"(&g_ftarget), "i"(&g_fesp_after), "i"(&g_fr),
            "i"(&g_fout.save));
#else
    __asm {
        pushad
        mov g_fsaved_esp, esp
        sub esp, 0x800                  // the stack below: zeros for both runs (fprem reads a dword where fnstcw
        mov edi, esp                    // stored a word, so the stack's old contents reach eax's upper half)
        xor eax, eax
        mov ecx, 0x200
        cld
        rep stosd
        add esp, 0x800
        finit
        fldz
        fldz
        fldz
        fldz
        fldz
        fldz
        fldz
        fldz
        finit                           // eight physical registers hold +0, all empty
        fldcw g_fin.cw
        mov ecx, g_fin.depth
    ld:
        test ecx, ecx
        jz loaded
        dec ecx
        lea eax, [ecx + ecx * 4]
        fld tbyte ptr g_fin.reg[eax * 2]
        jmp ld
    loaded:
        mov ecx, g_fin.nargs
        cmp ecx, 2
        jne one
        push g_fin.arg + 4
        push g_fin.arg
        jmp go
    one:
        cmp ecx, 1
        jne go
        push g_fin.arg
    go:
        mov g_fesp_before, esp
        mov eax, g_fin.eax
        mov ecx, 0x11111111
        mov edx, 0x22222222
        mov ebx, 0x33333333
        call dword ptr [g_ftarget]
        mov g_fesp_after, esp
        mov g_fr[0], eax
        mov g_fr[4], ecx
        mov g_fr[8], edx
        mov g_fr[12], ebx
        fnsave g_fout.save
        mov esp, g_fsaved_esp
        popad
        ret
    }
#endif
}
static __declspec(noinline) DWORD run_fpu_guarded() {
#if defined(__GNUC__) && !defined(__clang__)
    return vp_guard([](void*) { fpu_thunk(); }, nullptr);
#else
    __try {
        fpu_thunk();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
#endif
}
static void run_fpu(uint32_t target, FpuOut& out) {
    g_ftarget = target;
    memset(&g_fout, 0, sizeof g_fout);
    const DWORD f = run_fpu_guarded();
    unsigned int cw;
    _clearfp();
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" : : : VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _controlfp_s(&cw, _CW_DEFAULT, 0xffffffff);
    g_fout.fault = f;
    if (!f) {
        g_fout.eax = g_fr[0];
        g_fout.ecx = g_fr[1];
        g_fout.edx = g_fr[2];
        g_fout.ebx = g_fr[3];
        g_fout.esp_delta = (int32_t)(g_fesp_after - g_fesp_before);
        memset(g_fout.save + 12, 0, 16);       // the last instruction's and operand's addresses
    }
    out = g_fout;
}
static void ext_from_parts(uint8_t* r, uint64_t man, uint16_t se) { memcpy(r, &man, 8); memcpy(r + 8, &se, 2); }
// a random 80-bit operand: ordinary, around 1, tiny/huge, denormal, pseudo, zero, infinity, NaN, and divisors with the
// flawed table's mantissa bits (bits 62..59 one of 1, 4, 7, 10, 13 with 58..56 all set)
static void rnd_ext(uint8_t* r, bool divisor) {
    uint64_t man = ((uint64_t)rnd() << 32 | rnd()) | 0x8000000000000000ull;
    uint16_t se = (uint16_t)(0x3fff + ri(-40, 40));
    const int k = ri(0, 99);
    if (k < 8) se = (uint16_t)ri(1, 0x7ffe);
    else if (k < 12) se = (uint16_t)(0x3fff + ri(-16000, 16000));
    else if (k < 16) { se = 0; man &= 0x7fffffffffffffffull >> ri(0, 62); }       // denormal
    else if (k < 18) { se = 0; man |= 0x8000000000000000ull; }                    // pseudo-denormal
    else if (k < 20) { man &= 0x7fffffffffffffffull; }                            // unnormal
    else if (k < 23) { se = 0; man = 0; }
    else if (k < 25) { se = 0x7fff; man = 0x8000000000000000ull; }
    else if (k < 27) { se = 0x7fff; man |= 0xc000000000000000ull; }
    else if (k < 28) { se = 0x7fff; man = (man & 0x3fffffffffffffffull) | 0x8000000000000000ull; if (!(man << 1)) man |= 1; }
    else if (k < 30) { se = 1; }
    else if (k < 32) { se = 0x7ffe; }
    if (divisor && chance(35)) {
        static const int flaw[5] = {1, 4, 7, 10, 13};
        man &= ~(0xffull << 56);
        man |= (uint64_t)flaw[ri(0, 4)] << 59 | 7ull << 56 | 0x8000000000000000ull;
    }
    if (chance(50)) se |= 0x8000;
    ext_from_parts(r, man, se);
}
static uint32_t rnd_f32(bool divisor) {
    uint32_t u = rnd();
    const int k = ri(0, 99);
    if (k < 50) u = (u & 0x807fffffu) | (uint32_t)(127 + ri(-30, 30)) << 23;
    else if (k < 55) u &= 0x807fffffu;
    else if (k < 58) u = (u & 0x80000000u) | 0x7f800000u;
    else if (k < 60) u |= 0x7fc00000u;
    if (divisor && chance(35)) {
        static const uint32_t flaw[5] = {1, 4, 7, 10, 13};
        u = (u & ~0x007f0000u) | flaw[ri(0, 4)] << 19 | 7u << 16;
    }
    return u;
}
static uint64_t rnd_f64(bool divisor) {
    uint64_t u = (uint64_t)rnd() << 32 | rnd();
    const int k = ri(0, 99);
    if (k < 50) u = (u & 0x800fffffffffffffull) | (uint64_t)(1023 + ri(-200, 200)) << 52;
    else if (k < 55) u &= 0x800fffffffffffffull;
    else if (k < 58) u = (u & 0x8000000000000000ull) | 0x7ff0000000000000ull;
    else if (k < 60) u |= 0x7ff8000000000000ull;
    if (divisor && chance(35)) {
        static const uint64_t flaw[5] = {1, 4, 7, 10, 13};
        u = (u & ~(0xffull << 45)) | flaw[ri(0, 4)] << 48 | 7ull << 45;
    }
    return u;
}
static uint16_t rnd_cw() {
    uint16_t cw = (uint16_t)(0x7f | (uint16_t)ri(0, 3) << 8 | (uint16_t)ri(0, 3) << 10);
    if ((cw >> 8 & 3) == 1) cw &= ~0x100;                              // (01 is reserved)
    if (chance(8)) cw &= (uint16_t)~(1u << ri(0, 5));                 // an exception unmasked
    return cw;
}
static bool fpu_same(const FpuOut& a, const FpuOut& b) {
    if (a.fault || b.fault) return a.fault == b.fault;
    return !memcmp(a.save, b.save, sizeof a.save) && a.eax == b.eax && a.ecx == b.ecx && a.edx == b.edx &&
           a.ebx == b.ebx && a.esp_delta == b.esp_delta;
}
static void fpu_describe(const char* what, const FpuIn& in, const FpuOut& a, const FpuOut& b) {
    char s[512];
    int o = snprintf(s, sizeof s, "%s eax=%08x cw=%04x depth=%d args=%08x %08x; faults %lx/%lx", what, in.eax, in.cw,
                     in.depth, in.arg[0], in.arg[1], a.fault, b.fault);
    if (!a.fault && !b.fault) {
        for (int i = 0; i < 108; i++)
            if (a.save[i] != b.save[i]) { o += snprintf(s + o, sizeof s - o, "; fsave+%d %02x vs %02x", i, a.save[i], b.save[i]); break; }
        o += snprintf(s + o, sizeof s - o, "; eax %08x/%08x esp %d/%d", a.eax, b.eax, a.esp_delta, b.esp_delta);
    }
    fail("fdiv", "%s", s);
}
struct FdivFn { uint32_t at; int nargs; int kind; };   // kind: 0 two regs, 1 m32 float, 2 m64, 3 m16i, 4 m32i, 5 fdiv_r, 6 one reg
static const FdivFn k_fdiv[] = {
    {0x4ce2f7, 0, 5}, {0x4ce796, 0, 0}, {0x4ce7a9, 0, 0}, {0x4ce7bc, 1, 1}, {0x4ce808, 2, 2}, {0x4ce854, 1, 3},
    {0x4ce888, 1, 4}, {0x4ce8bc, 1, 1}, {0x4ce908, 2, 2}, {0x4ce954, 1, 3}, {0x4ce988, 1, 4}, {0x4ce9bc, 0, 0},
    {0x4ce9d1, 0, 0}, {0x4cebec, 0, 7}, {0x4ceea4, 0, 7}, {0x4cef59, 0, 7}, {0x4cef5f, 0, 7}, {0x4cef68, 0, 6},
    {0x4d0840, 0, 8},
};
static bool is_fdiv_group(uint32_t at) { return (at >= 0x4ce1e0 && at < 0x4cef70) || at == 0x4d0840 || at == 0x4cf3d0; }
// the third run of a case: every x87 routine hooked in (so the rewrites call each other), the original's entry called
static void run_fpu_chain(uint32_t at, FpuOut& out) {
    g_chain_only = is_fdiv_group;
    chain_patch();
    run_fpu(at, out);
    chain_unpatch();
    g_chain_only = 0;
}
static void test_fdiv(int iters) {
    printf("fdiv: the x87 routines, %d cases each, three ways (original, the rewrite alone, all rewrites hooked)\n", iters);
    for (const FdivFn& fn : k_fdiv) {
        const uint32_t mine = (uint32_t)(uintptr_t)rewrite_of(fn.at);
        Stat& st = g_stats[name_of(fn.at)];
        long faults = 0;
        for (int it = 0; it < iters; it++) {
            FpuIn& in = g_fin;
            memset(&in, 0, sizeof in);
            in.cw = rnd_cw();
            in.nargs = fn.nargs;
            in.eax = rnd();
            in.depth = fn.kind == 8 ? 0 : fn.kind == 6 ? ri(1, 8) : ri(fn.kind == 5 ? 1 : 2, 8);
            if (chance(50) && fn.kind != 8) in.depth = fn.kind == 6 ? 1 : 2;
            for (int i = 0; i < in.depth; i++) rnd_ext(in.reg[i], i == 0);
            if (fn.kind == 7 && in.depth >= 2) {                       // fprem: st(0) dividend, st(1) divisor
                rnd_ext(in.reg[1], true);
                if (chance(60)) {
                    uint16_t se1;
                    memcpy(&se1, in.reg[1] + 8, 2);
                    uint16_t se0 = (uint16_t)((se1 & 0x7fff) + ri(-3, 100));
                    if (se0 >= 0x7fff) se0 = 0x7ffe;
                    se0 |= (uint16_t)(chance(50) ? 0x8000 : 0);
                    memcpy(in.reg[0] + 8, &se0, 2);
                }
            }
            if (fn.kind == 1) in.arg[0] = rnd_f32(true);
            if (fn.kind == 2) { uint64_t u = rnd_f64(true); memcpy(in.arg, &u, 8); }
            if (fn.kind == 3) in.arg[0] = chance(20) ? 0 : (uint32_t)(int16_t)rnd();
            if (fn.kind == 4) in.arg[0] = chance(20) ? 0 : rnd() >> ri(0, 31);
            if (fn.kind == 5 && chance(70)) {                           // a code whose register exists
                const uint32_t reg = (uint32_t)ri(0, in.depth - 1), form = (uint32_t)ri(0, 7);
                in.eax = (in.eax & ~0x3fu) | reg << 3 | form;
            }
            const FpuIn saved = in;
            FpuOut a, b, c;
            run_fpu(fn.at, a);
            g_fin = saved;
            run_fpu(mine, b);
            g_fin = saved;
            run_fpu_chain(fn.at, c);
            st.calls++;
            if (a.fault) faults++;
            if (!fpu_same(a, b) || !fpu_same(a, c)) {
                st.fails++;
                fpu_describe(name_of(fn.at), saved, a, fpu_same(a, b) ? c : b);
            }
        }
        printf("  %-22s %8ld cases, %6ld faulted (both), %ld differ\n", name_of(fn.at), st.calls, faults, st.fails);
    }
    // __adj_fdiv_r: every code with a full stack, many times
    {
        Stat& st = g_stats["adj_fdiv_r (every code, full stack)"];
        const uint32_t mine = (uint32_t)(uintptr_t)rewrite_of(0x4ce2f7);
        for (int it = 0; it < iters / 8 + 1; it++)
            for (uint32_t code = 0; code < 64; code++) {
                FpuIn& in = g_fin;
                memset(&in, 0, sizeof in);
                in.cw = rnd_cw();
                in.depth = 8;
                for (int i = 0; i < 8; i++) rnd_ext(in.reg[i], true);
                in.eax = code | (rnd() & ~0x3fu);
                const FpuIn saved = in;
                FpuOut a, b, c;
                run_fpu(0x4ce2f7, a);
                g_fin = saved;
                run_fpu(mine, b);
                g_fin = saved;
                run_fpu_chain(0x4ce2f7, c);
                st.calls++;
                if (!fpu_same(a, b) || !fpu_same(a, c)) {
                    st.fails++;
                    fpu_describe("adj_fdiv_r (all codes)", saved, a, b);
                }
            }
        printf("  %-22s %8ld cases, %ld differ\n", "adj_fdiv_r all codes", st.calls, st.fails);
    }
    // __chkstk: eax = the frame's size
    {
        Stat& st = g_stats["chkstk"];
        const uint32_t mine = (uint32_t)(uintptr_t)rewrite_of(0x4cf3d0);
        for (int it = 0; it < iters / 4 + 64; it++) {
            memset(&g_fin, 0, sizeof g_fin);
            g_fin.cw = 0x27f;
            g_fin.eax = it < 64 ? (uint32_t)it * 0x400 : (uint32_t)ri(0, 0x30000);
            const FpuIn saved = g_fin;
            FpuOut a, b;
            run_fpu(0x4cf3d0, a);
            g_fin = saved;
            run_fpu(mine, b);
            FpuOut c;
            g_fin = saved;
            run_fpu_chain(0x4cf3d0, c);
            // eax comes back as the caller's return address: the thunk's, the same for all three
            st.calls++;
            if (!fpu_same(a, b) || !fpu_same(a, c)) {
                st.fails++;
                fpu_describe("chkstk", saved, a, fpu_same(a, b) ? c : b);
            }
        }
        printf("  %-22s %8ld cases, %ld differ\n", "chkstk", st.calls, st.fails);
    }
    // __ms_p5_mp_test_fdiv: the same answer (0 on this processor) and the same calls
    {
        Stat& st = g_stats["ms_p5_mp_test_fdiv"];
        for (int pass = 0; pass < 2; pass++) {
            g_log.clear();
            int r0 = ((int(__cdecl*)())0x4d0890)();
            std::vector<uint32_t> l0 = g_log;
            g_log.clear();
            int r1 = ((int(__cdecl*)())rewrite_of(0x4d0890))();
            st.calls++;
            if (r0 != r1 || l0 != g_log) {
                st.fails++;
                fail("fdiv", "ms_p5_mp_test_fdiv: %d vs %d, %u vs %u API calls", r0, r1, (unsigned)l0.size() / 7,
                     (unsigned)g_log.size() / 7);
            }
            if (pass == 0) printf("  ms_p5_mp_test_fdiv: %d (this processor %s the FDIV flaw)\n", r0, r0 ? "HAS" : "hasn't");
        }
        DWORD_PTR pm, sm;
        if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) SetThreadAffinityMask(GetCurrentThread(), pm);
    }
    // the game's own divides through the workaround: __adjust_fdiv forced to 1, originals vs the rewrites hooked in
    {
        Stat& st = g_stats["game divides via __adjust_fdiv=1"];
        const uint32_t saved_flag = *(uint32_t*)0x5024b8;
        *(uint32_t*)0x5024b8 = 1;
        for (int it = 0; it < iters; it++) {
            float m[2][9], q[2][4];
            for (int i = 0; i < 9; i++) {
                uint32_t u = rnd_f32(true);
                if (chance(70)) { float f = (float)((int)(rnd() % 2001) - 1000) / 400.0f; memcpy(&u, &f, 4); }
                memcpy(&m[0][i], &u, 4);
            }
            memcpy(m[1], m[0], sizeof m[0]);
            unsigned int cw;
            _controlfp_s(&cw, chance(50) ? _PC_24 : _PC_53, _MCW_PC);
            ((void(__cdecl*)(float*))0x429180)(m[0]);
            ((void(__cdecl*)(float*, float*))0x4294c0)(q[0], m[0]);
            chain_patch();
            ((void(__cdecl*)(float*))0x429180)(m[1]);
            ((void(__cdecl*)(float*, float*))0x4294c0)(q[1], m[1]);
            chain_unpatch();
            _controlfp_s(&cw, _PC_53, _MCW_PC);
            st.calls++;
            if (memcmp(m[0], m[1], sizeof m[0]) || memcmp(q[0], q[1], sizeof q[0])) {
                st.fails++;
                fail("fdiv", "MatrixNormalize / MatrixToQuat differ through the workaround (iteration %d)", it);
            }
        }
        *(uint32_t*)0x5024b8 = saved_flag;
        printf("  %-22s %8ld cases, %ld differ\n", "MatrixNormalize+ToQuat", st.calls, st.fails);
    }
}
// the generated code against the original's: written out for tools/ (the instruction streams, compared offline)
static void dump_fdiv_code(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        if (r->at < 0x4ce1e0 || (r->at >= 0x4cef70 && r->at != 0x4d0840 && r->at != 0x4cf3d0)) continue;
        fprintf(f, "%08x %08x %s\n", r->at, (uint32_t)(uintptr_t)r->fn, r->name);
    }
    for (int k = 0; k < 64; k++)
        fprintf(f, "tab %d %08x %08x\n", k, *(uint32_t*)(uintptr_t)(0x50252e + 4 * k), (uint32_t)(uintptr_t)crt_fdiv::k_fdivr_tab[k]);
    fclose(f);
}

// =========================================================================================================================
// str: the string routines
// =========================================================================================================================
enum { SA = 1024 };
static uint8_t g_sa[2][SA + 64];                    // the two arenas (original, rewrite)
static const char k_alpha[] = "aAbBzZ09 \t\r\n~@[`{\x80\xe9\xc9\xff\x7f";
static void fill_str_arena() {
    for (int i = 0; i < SA; i++) {
        const int k = ri(0, 99);
        g_sa[0][i] = k < 8 ? 0 : k < 80 ? (uint8_t)k_alpha[rnd() % (sizeof k_alpha - 1)] : (uint8_t)rnd();
    }
    g_sa[0][SA - 1] = 0;
    memset(g_sa[0] + SA, 0, 64);
    memcpy(g_sa[1], g_sa[0], sizeof g_sa[0]);
}
static uint32_t soff() { return (uint32_t)ri(0, SA - 300); }
// the originals' writes must stay inside the rewrite's footprint (arena bytes changed outside every region)
static bool fp_contains(const Footprint& f, const void* p) {
    for (int i = 0; i < f.n; i++)
        if ((const uint8_t*)p >= (const uint8_t*)f.r[i].p && (const uint8_t*)p < (const uint8_t*)f.r[i].p + f.r[i].n) return true;
    return false;
}
static uint8_t g_sa_before[SA + 64];
static void fp_check(const char* name, const Footprint& f) {
    for (int i = 0; i < SA + 64; i++)
        if (g_sa[0][i] != g_sa_before[i] && !fp_contains(f, &g_sa[0][i])) {
            fail("str", "%s: the original wrote arena +%d outside the footprint", name, i);
            g_stats[name].fails++;
            return;
        }
}
template <typename R, typename... A> static R callv(uint32_t at, A... a) { return ((R(__cdecl*)(A...))(uintptr_t)at)(a...); }
static uint32_t rel(const void* p, int w) {          // a returned pointer, as an offset into its own arena
    if (!p) return 0xffffffff;
    return (uint32_t)((const uint8_t*)p - g_sa[w]);
}
static void test_str(int iters) {
    printf("str: the string routines, %d random calls each\n", iters);
    struct T { uint32_t at; int kind; };
    static const T fns[] = {{0x4ce0c0, 0}, {0x4cf130, 0}, {0x4cf9a0, 1}, {0x4ce0e0, 2}, {0x4cf3a0, 3}, {0x4d8380, 4},
                            {0x4cf400, 5}, {0x4da350, 6}, {0x4da3e0, 7}, {0x4d4d10, 8}, {0x4cf320, 9}};
    for (const T& t : fns) {
        Stat& st = g_stats[name_of(t.at)];
        void* mine = rewrite_of(t.at);
        for (int it = 0; it < iters; it++) {
            fill_str_arena();
            const uint32_t o1 = soff(), o2 = chance(30) ? o1 + (uint32_t)ri(-8, 8) : soff();
            const uint32_t n = chance(10) ? 0 : chance(70) ? (uint32_t)ri(0, 40) : (uint32_t)ri(0, 260);
            const int c = chance(20) ? 0 : chance(70) ? (uint8_t)k_alpha[rnd() % (sizeof k_alpha - 1)] : (int)rnd();
            uint32_t r[2] = {};
            DWORD fault[2] = {};
            Footprint fp;
            memcpy(g_sa_before, g_sa[0], sizeof g_sa_before);
            for (int w = 0; w < 2; w++) {
                const uint32_t at = w ? (uint32_t)(uintptr_t)mine : t.at;
                char* s1 = (char*)g_sa[w] + o1;
                char* s2 = (char*)g_sa[w] + (o2 < SA - 270 ? o2 : o1);
                if (w == 0) {
                    switch (t.kind) {
                    case 0: crt_str::fp_strchr(fp, s1, c); break;
                    case 3: crt_str::fp_strncpy(fp, s1, s2, n); break;
                    case 4: crt_str::fp_strncat(fp, s1, s2, n); break;
                    case 5: crt_str::fp_memmove(fp, s1, s2, n); break;
                    case 9: crt_str::fp_swap(fp, s1, s2, n); break;
                    }
                }
                fault[w] = guarded([&] {
                    switch (t.kind) {
                    case 0: r[w] = rel(callv<char*>(at, (const char*)s1, c), w); break;
                    case 1: r[w] = rel(callv<char*>(at, (const char*)s1, (const char*)s2), w); break;
                    case 2: r[w] = (uint32_t)callv<int32_t>(at, (const char*)s1, (const char*)s2, n); break;
                    case 3: r[w] = rel(callv<char*>(at, s1, (const char*)s2, n), w); break;
                    case 4: r[w] = rel(callv<char*>(at, s1, (const char*)s2, n), w); break;
                    case 5: r[w] = rel(callv<void*>(at, (void*)s1, (const void*)s2, n), w); break;
                    case 6: r[w] = (uint32_t)callv<int32_t>(at, (const char*)s1, (const char*)s2); break;
                    case 7: r[w] = (uint32_t)callv<int32_t>(at, (const char*)s1, (const char*)s2, n); break;
                    case 8: r[w] = (uint32_t)callv<int32_t>(at, (const char*)s1, (int32_t)(n - 20)); break;
                    case 9: callv<void>(at, s1, s2, n); break;
                    }
                });
                if (w == 0) fp_check(name_of(t.at), fp);
            }
            st.calls++;
            if (r[0] != r[1] || fault[0] != fault[1] || memcmp(g_sa[0], g_sa[1], sizeof g_sa[0])) {
                st.fails++;
                fail("str", "%s it %d: result %08x vs %08x, fault %lx/%lx, arenas %s (o1 %u o2 %u n %u c %d)", name_of(t.at),
                     it, r[0], r[1], fault[0], fault[1], memcmp(g_sa[0], g_sa[1], sizeof g_sa[0]) ? "differ" : "same", o1,
                     o2, n, c);
            }
        }
        printf("  %-22s %9ld calls, %ld differ\n", name_of(t.at), st.calls, st.fails);
    }
    // strncat with a count of 0, next to an unmapped page: nothing copied, the NUL rewritten
    {   // (each destination 16 bytes before an unmapped page, so the copy faults there)
        uint8_t* pg = (uint8_t*)VirtualAlloc(0, 0x4000, MEM_RESERVE, PAGE_NOACCESS);
        VirtualAlloc(pg, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        VirtualAlloc(pg + 0x2000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        memset(pg, 'x', 0x1000);
        memset(pg + 0x2000, 'x', 0x1000);
        pg[0x1000 - 32] = pg[0x3000 - 32] = 0;
        DWORD f0 = guarded([&] { callv<char*>(0x4d8380, (char*)pg + 0x1000 - 40, (const char*)pg, 0u); });
        DWORD f1 = guarded([&] { callv<char*>((uint32_t)(uintptr_t)rewrite_of(0x4d8380), (char*)pg + 0x3000 - 40, (const char*)pg + 0x2000, 0u); });
        if (memcmp(pg, pg + 0x2000, 0x1000)) fail("str", "strncat(n = 0): the pages differ");
        VirtualFree(pg, 0, MEM_RELEASE);
        printf("  strncat(n = 0): original fault %lx, rewrite fault %lx\n", f0, f1);
        if (f0 != f1) fail("str", "strncat(n = 0): faults %lx vs %lx", f0, f1);
    }
}

// =========================================================================================================================
// ctype and multibyte
// =========================================================================================================================
// (w: __crtLCMapStringA / __crtGetStringTypeA forced onto their wide-character paths, f_use = 1, as on Windows 95)
struct LocaleSet { const char* name; uint32_t lc, cp, mbmax; bool w; };
static const LocaleSet k_loc[] = {{"C", 0, 0, 1, false},           {"1252", 0x409, 1252, 1, false},
                                  {"932 (mb 2)", 0x411, 932, 2, false}, {"1252 mb2", 0x409, 1252, 2, false},
                                  {"1252 W", 0x409, 1252, 1, true},  {"932 mb2 W", 0x411, 932, 2, true}};
static void set_locale(const LocaleSet& l) {
    *(uint32_t*)CRT_LC_CTYPE_VA = l.lc;
    *(uint32_t*)CRT_LC_CODEPAGE_VA = l.cp;
    *(int32_t*)CRT_MB_CUR_MAX_VA = (int32_t)l.mbmax;
    if (l.w) *(uint32_t*)CRT_AWMAP_FUSE_VA = *(uint32_t*)CRT_AWSTR_FUSE_VA = 1;
}
// one call, original then rewrite, each from the same C runtime state and heap; compares the result, the CRT's
// .data and .bss (0x5024a0..0x503a00, 0x5d5760..: the whole .data costs too much per call here), the heap and the API
// log; then carries on from the original's state (so the probes' f_use and the heap move on as in a run)
struct CrtState { uint8_t a[0x503a00 - 0x5024a0], b[DATA_AT + DATA_SIZE - 0x5d5760]; };
static CrtState g_cs[4];
static void crt_save(CrtState& c) { memcpy(c.a, (void*)0x5024a0, sizeof c.a); memcpy(c.b, (void*)0x5d5760, sizeof c.b); }
static void crt_load(const CrtState& c) { memcpy((void*)0x5024a0, c.a, sizeof c.a); memcpy((void*)0x5d5760, c.b, sizeof c.b); }
static std::string crt_diff(const CrtState& x, const CrtState& y) {
    for (uint32_t i = 0; i < sizeof x.a; i++)
        if (x.a[i] != y.a[i]) return std::string(data_name(0x5024a0 + i));
    for (uint32_t i = 0; i < sizeof x.b; i++)
        if (x.b[i] != y.b[i]) return std::string(data_name(0x5d5760 + i));
    return "";
}
// (three runs: the original, the rewrite alone, and every rewrite hooked in with the original's entry called)
static void both(const char* name, uint32_t at, const std::function<uint32_t(uint32_t)>& call) {
    Stat& st = g_stats[name];
    crt_save(g_cs[3]);
    const std::vector<uint8_t> h0(g_heap, g_heap + g_heap_top);
    const uint32_t top0 = g_heap_top;
    std::vector<uint8_t> h[3];
    std::vector<uint32_t> lg2[3];
    uint32_t top[3];
    uint32_t r[3];
    DWORD f[3];
    for (int w = 0; w < 3; w++) {
        crt_load(g_cs[3]);
        memcpy(g_heap, h0.data(), h0.size());
        g_heap_top = top0;
        g_log.clear();
        r[w] = 0;
        if (w == 2) chain_patch();
        f[w] = guarded([&] { r[w] = call(w == 1 ? (uint32_t)(uintptr_t)rewrite_of(at) : at); });
        if (w == 2) chain_unpatch();
        crt_save(g_cs[w]);
        h[w].assign(g_heap, g_heap + g_heap_top);
        top[w] = g_heap_top;
        lg2[w] = g_log;
    }
    crt_load(g_cs[0]);                                         // carry on from the original's state
    memcpy(g_heap, h[0].data(), h[0].size());
    g_heap_top = top[0];
    st.calls++;
    for (int w = 1; w < 3; w++) {
        std::string d = crt_diff(g_cs[0], g_cs[w]);
        if (d.empty() && (h[0] != h[w] || top[0] != top[w])) d = "the heap";
        if (d.empty() && lg2[0] != lg2[w]) d = "the API calls";
        if (r[0] != r[w] || f[0] != f[w] || !d.empty()) {
            st.fails++;
            fail("ctype", "%s (%s): %08x vs %08x, fault %lx/%lx %s", name, w == 1 ? "isolated" : "chain", r[0], r[w], f[0],
                 f[w], d.c_str());
            break;
        }
    }
}
static void test_ctype(int iters) {
    printf("ctype: character classes and multibyte, in six locale settings\n");
    static uint8_t buf[2][64];
    for (const LocaleSet& l : k_loc) {
        state_restore();
        set_locale(l);
        if (l.cp == 932) callv<int32_t>(0x4d3fc0, 932);            // _mbctype with 932's lead bytes (_mbctoupper's path)
        // keep this locale's state for every call (the probes' f_use change it: each pair starts from the same)
        for (int c = -1; c <= 0x10000; c += (c < 0x200 ? 1 : 0x3b)) {
            both("isdigit", 0x4cf760, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, c); });
            both("isspace", 0x4cf790, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, c); });
            both("tolower", 0x4cfd50, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, c); });
            both("toupper", 0x4d4df0, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, c); });
            const int32_t mask = (int32_t)(rnd() & 0x81ff);
            both("isctype", 0x4d47a0, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, c, mask); });
            const int32_t km = (int32_t)(rnd() & 0xf);
            const bool nul = chance(10);
            both("x_ismbbtype", 0x4d3a80, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, (uint32_t)c, mask & 0x1ff, km); });
            both("ismbblead", 0x4d3a60, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, (uint32_t)c); });
            both("mbctoupper", 0x4d48b0, [&](uint32_t at) { return callv<uint32_t>(at, (uint32_t)c); });
            both("wctomb", 0x4d66b0, [&](uint32_t at) {
                memset(buf, 0xee, sizeof buf);
                const int32_t r = callv<int32_t>(at, nul ? (char*)0 : (char*)buf[0], (uint32_t)c);
                return (uint32_t)r ^ fnv(buf[0], 8);
            });
        }
        for (int it = 0; it < iters; it++) {
            uint8_t s[8];
            for (int i = 0; i < 8; i++) s[i] = chance(15) ? 0 : (uint8_t)rnd();
            const uint32_t n = (uint32_t)ri(0, 5);
            const bool nw = chance(20), ns = chance(5);
            both("mbtowc", 0x4d6340, [&](uint32_t at) {
                uint16_t wc = 0xeeee;
                const int32_t r = callv<int32_t>(at, nw ? (uint16_t*)0 : &wc, ns ? (const char*)0 : (const char*)s, n);
                return (uint32_t)r ^ (uint32_t)wc << 8;
            });
            char a[24], b[24];
            for (int i = 0; i < 23; i++) {
                a[i] = (char)(chance(10) ? 0 : k_alpha[rnd() % (sizeof k_alpha - 1)]);
                b[i] = chance(60) ? a[i] : (char)(chance(10) ? 0 : k_alpha[rnd() % (sizeof k_alpha - 1)]);
                if (chance(30) && a[i] >= 'a' && a[i] <= 'z') b[i] = (char)(a[i] - 0x20);
            }
            a[23] = b[23] = 0;
            both("stricmp", 0x4da350, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, (const char*)a, (const char*)b); });
            const uint32_t nn = (uint32_t)ri(0, 30);
            both("strnicmp", 0x4da3e0, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, (const char*)a, (const char*)b, nn); });
            const uint32_t flags = (uint32_t)(chance(40) ? 0x100 : chance(50) ? 0x200 : chance(50) ? 0x400 : 0x100 | 0x200);
            const int32_t cs = ri(-1, 12), cd = chance(30) ? 0 : ri(1, 40);
            const uint32_t cp1 = chance(50) ? 0u : l.cp, cp2 = chance(50) ? 0u : l.cp, lc2 = chance(50) ? 0u : l.lc;
            const uint32_t it2 = (uint32_t)ri(1, 4);
            const int32_t n2 = ri(1, 12);
            both("crtLCMapStringA", 0x4d4ae0, [&](uint32_t at) {
                memset(buf, 0xee, sizeof buf);
                const int32_t r = callv<int32_t>(at, l.lc ? l.lc : 0x409u, flags, (const char*)a, cs, (char*)buf[0], cd, cp1);
                return (uint32_t)r ^ fnv(buf[0], 64);
            });
            both("crtGetStringTypeA", 0x4d68f0, [&](uint32_t at) {
                memset(buf, 0xee, sizeof buf);
                const int32_t r = callv<int32_t>(at, it2, (const char*)a, n2, (uint16_t*)buf[0], cp2, lc2);
                return (uint32_t)r ^ fnv(buf[0], 64);
            });
        }
        printf("  locale %-12s done\n", l.name);
    }
    // _setmbcp and friends: each code page from the start-up state and from another's
    static const int32_t cps[] = {932, 936, 949, 950, 1252, 437, 850, 65001, 0, -2, -3, -4, 12345, 1250, 1361, 20932, 54936};
    for (int32_t a : cps)
        for (int32_t b : cps) {
            state_restore();
            both("setmbcp", 0x4d3fc0, [&](uint32_t at) { return (uint32_t)callv<int32_t>(at, a) ^ (uint32_t)callv<int32_t>(at, b) << 8; });
        }
    state_restore();
    both("initmbctable", 0x4d4280, [&](uint32_t at) { callv<void>(at); return 0u; });
    both("setSBCS", 0x4d4250, [&](uint32_t at) { callv<void>(at); return 0u; });
    for (int32_t a : cps) {
        both("getSystemCP", 0x4d41a0, [&](uint32_t at) { return callv<uint32_t>(at, a); });
        both("CPtoLCID", 0x4d41f0, [&](uint32_t at) { return callv<uint32_t>(at, a); });
    }
    for (const char* n : {"isdigit", "isspace", "tolower", "toupper", "isctype", "x_ismbbtype", "ismbblead", "mbctoupper", "wctomb",
                          "mbtowc", "stricmp", "strnicmp", "crtLCMapStringA", "crtGetStringTypeA", "setmbcp", "initmbctable",
                          "setSBCS", "getSystemCP", "CPtoLCID"})
        printf("  %-22s %9ld calls, %ld differ\n", n, g_stats[n].calls, g_stats[n].fails);
    state_restore();
}

// =========================================================================================================================
// sort: qsort, bsearch
// =========================================================================================================================
static uint8_t* g_sort_base;
static uint32_t g_sort_width, g_sort_k;
static std::vector<uint32_t> g_cmp_log;
static int __cdecl cmp_key(const void* a, const void* b) {
    g_cmp_log.push_back((uint32_t)((const uint8_t*)a - g_sort_base) << 16 | (uint32_t)((const uint8_t*)b - g_sort_base));
    const int ka = *(const uint8_t*)a % g_sort_k, kb = *(const uint8_t*)b % g_sort_k;
    return ka - kb;
}
static int __cdecl cmp_key_bs(const void* key, const void* b) {
    g_cmp_log.push_back((uint32_t)((const uint8_t*)b - g_sort_base));
    const int ka = *(const uint8_t*)key % g_sort_k, kb = *(const uint8_t*)b % g_sort_k;
    return ka < kb ? -ri(1, 5) : ka > kb ? ri(1, 5) : 0;
}
static void test_sort(int iters) {
    printf("sort: qsort / bsearch, %d arrays each way\n", iters);
    static uint8_t arr[3][24 * 300];
    Stat& sq = g_stats["qsort"];
    Stat& sb = g_stats["bsearch"];
    for (int it = 0; it < iters; it++) {
        const uint32_t w = (uint32_t)(chance(70) ? ri(1, 8) : ri(1, 24));
        const uint32_t n = (uint32_t)(chance(50) ? ri(0, 12) : ri(0, 300));
        g_sort_k = (uint32_t)(chance(50) ? ri(1, 4) : ri(1, 256));
        for (uint32_t i = 0; i < w * n; i++) arr[0][i] = (uint8_t)rnd();
        std::vector<uint32_t> logs[3];
        memcpy(arr[1], arr[0], w * n);                      // original, isolated, chain: the same array
        memcpy(arr[2], arr[0], w * n);
        for (int m = 0; m < 3; m++) {
            g_sort_base = arr[m];
            g_cmp_log.clear();
            if (m == 2) chain_patch();
            const uint32_t at = m == 1 ? (uint32_t)(uintptr_t)rewrite_of(0x4cf160) : 0x4cf160;
            callv<void>(at, (void*)arr[m], n, w, (void*)&cmp_key);
            if (m == 2) chain_unpatch();
            logs[m] = g_cmp_log;
        }
        sq.calls++;
        for (int m = 1; m < 3; m++)
            if (memcmp(arr[0], arr[m], w * n) || logs[0] != logs[m]) {
                sq.fails++;
                fail("sort", "qsort (%s) it %d: n %u width %u keys %u: %s", m == 1 ? "isolated" : "chain", it, n, w, g_sort_k,
                     memcmp(arr[0], arr[m], w * n) ? "order differs" : "comparator calls differ");
                break;
            }
        // bsearch on the sorted array, a random key (present or not)
        uint8_t key[24];
        for (uint32_t i = 0; i < w; i++) key[i] = (uint8_t)rnd();
        const uint32_t num = n ? (uint32_t)ri(0, (int)n) : 0;
        uint32_t r[3];
        for (int m = 0; m < 3; m++) {
            g_sort_base = arr[m];
            g_cmp_log.clear();
            const uint32_t saved = g_rng;
            if (m == 2) chain_patch();
            const uint32_t at = m == 1 ? (uint32_t)(uintptr_t)rewrite_of(0x4cfca0) : 0x4cfca0;
            void* p = callv<void*>(at, (const void*)key, (const void*)arr[m], num, w, (void*)&cmp_key_bs);
            if (m == 2) chain_unpatch();
            r[m] = p ? (uint32_t)((uint8_t*)p - arr[m]) : 0xffffffff;
            logs[m] = g_cmp_log;
            if (m < 2) g_rng = saved;                   // the same comparator values each time
        }
        sb.calls++;
        if (r[0] != r[1] || r[0] != r[2] || logs[0] != logs[1] || logs[0] != logs[2]) {
            sb.fails++;
            fail("sort", "bsearch it %d: %x %x %x", it, r[0], r[1], r[2]);
        }
    }
    printf("  %-22s %9ld calls, %ld differ\n  %-22s %9ld calls, %ld differ\n", "qsort", sq.calls, sq.fails, "bsearch",
           sb.calls, sb.fails);
}

// =========================================================================================================================
// the three-pass scripts: heap, io
// =========================================================================================================================
enum { M_ORIG, M_ISO, M_CHAIN };
static int g_mode;
static const char* const k_mode[3] = {"original", "isolated", "chain"};
static uint32_t at_of(uint32_t at) { return g_mode == M_ISO ? (uint32_t)(uintptr_t)rewrite_of(at) : at; }
template <typename R, typename... A> static R cc(uint32_t at, A... a) { return ((R(__cdecl*)(A...))(uintptr_t)at_of(at))(a...); }
struct PassResult { std::vector<uint32_t> res; std::vector<std::string> ops; Snap s; std::string files; };
static PassResult g_pr[3];
static std::map<std::string, std::pair<long, long>> g_opstats;     // the originals' pass: calls, and "succeeded"
static void res(const char* op, uint32_t v) {
    g_pr[g_mode].res.push_back(v);
    g_pr[g_mode].ops.push_back(op);
    if (g_mode == M_ORIG) {
        auto& o = g_opstats[op];
        o.first++;
        if (v != 0 && v != 0xffffffffu) o.second++;
    }
}
// the script's memory: strings and buffers at fixed addresses
enum { SB_SIZE = 0x40000 };
static uint8_t* g_sbuf;
static void rm_tree(const char* dir) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char p[MAX_PATH];
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p);
            else DeleteFileA(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}
static void list_tree(const char* dir, std::string& out) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    std::vector<std::string> names;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) names.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", dir, n.c_str());
        const DWORD at = GetFileAttributesA(p);
        if (at & FILE_ATTRIBUTE_DIRECTORY) {
            out += "[" + n + "]";
            list_tree(p, out);
            continue;
        }
        FILE* f = fopen(p, "rb");
        std::string data;
        if (f) {
            char b[4096];
            size_t k;
            while ((k = fread(b, 1, sizeof b, f)) > 0) data.append(b, k);
            fclose(f);
        }
        char t[64];
        snprintf(t, sizeof t, ":%u:%08x:%lx;", (unsigned)data.size(), fnv(data.data(), data.size()), at & FILE_ATTRIBUTE_READONLY);
        out += n + t;
    }
}
static std::string g_files0[4];                    // the sandbox's starting files (contents)
static void build_sandbox(uint32_t seed) {
    SetCurrentDirectoryA("\\");
    rm_tree(g_sb);
    CreateDirectoryA(g_sb, 0);
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\sub", g_sb);
    CreateDirectoryA(p, 0);
    uint32_t save = g_rng;
    g_rng = seed * 7 + 1;
    static const char* const names[4] = {"a.txt", "b.bin", "sub\\d.txt", "z.txt"};
    for (int i = 0; i < 4; i++) {
        std::string& s = g_files0[i];
        s.clear();
        const int n = i == 3 ? 0 : chance(20) ? ri(0, 10) : ri(0, 9000);
        for (int k = 0; k < n; k++) {
            const int c = ri(0, 99);
            s += c < 50 ? (char)('a' + rnd() % 26) : c < 62 ? '\n' : c < 74 ? '\r' : c < 80 ? (char)0x1a : c < 86 ? ' ' : (char)rnd();
            if (c < 62 && c >= 50 && chance(50)) s.insert(s.size() - 1, "\r");
        }
        if (chance(30)) s += (char)0x1a;
        snprintf(p, sizeof p, "%s\\%s", g_sb, names[i]);
        FILE* f = fopen(p, "wb");
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
    {
        std::string c(4095, 'a');
        c += "\r\nb\r\r\r\nline\n\rx\r";
        c += std::string(5000, 'q') + "\r\n\x1a" + "after ^Z\r\n";
        snprintf(p, sizeof p, "%s\\cr.txt", g_sb);
        FILE* f = fopen(p, "wb");
        fwrite(c.data(), 1, c.size(), f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s\\ro.txt", g_sb);
    FILE* f = fopen(p, "wb");
    fputs("read only\r\n", f);
    fclose(f);
    SetFileAttributesA(p, FILE_ATTRIBUTE_READONLY);
    g_rng = save;
    SetCurrentDirectoryA(g_sb);
}
// handle 30: an anonymous pipe (FOPEN | FPIPE, text or binary), its data written and the writing end closed -- the
// pipe paths of _read (the pipe character, the CR at a buffer's end peeked and kept, ERROR_BROKEN_PIPE at the end)
enum { PIPE_FH = 30 };
static void make_pipe(uint32_t seed) {
    HANDLE r, w;
    if (!CreatePipe(&r, &w, 0, 0x10000)) return;
    const uint32_t save = g_rng;
    g_rng = seed * 13 + 5;
    std::string d;
    const int n = ri(0, 3000);
    for (int k = 0; k < n; k++) d += "ab\r\n\r\x1a\nzz"[ri(0, 9)];
    const uint8_t fl = (uint8_t)(0x09 | (chance(70) ? 0x80 : 0));
    g_rng = save;
    DWORD put;
    WriteFile(w, d.data(), (DWORD)d.size(), &put, 0);
    CloseHandle(w);
    g_vh[g_nvh] = r;
    CrtIoinfo* io = crt_ioinfo(PIPE_FH);
    io->osfhnd = (intptr_t)(0x1000 + 4 * g_nvh++);
    io->osfile = fl;
    io->pipech = 10;
}
static void pass_begin(int mode, uint32_t seed) {
    g_mode = mode;
    state_restore();
    vclose_all();
    make_pipe(seed);
    build_sandbox(seed);
    g_pr[mode] = PassResult();
    memset(g_sbuf, 0xab, SB_SIZE);
    g_rng = seed;
    if (mode == M_CHAIN) chain_patch();
}
static void pass_end() {
    if (g_mode == M_CHAIN) chain_unpatch();
    snap(g_pr[g_mode].s);
    vclose_all();
    SetCurrentDirectoryA("\\");
    list_tree(g_sb, g_pr[g_mode].files);
    g_pr[g_mode].s.log.push_back(fnv(g_sbuf, SB_SIZE));      // the script's buffers
}
static bool compare_passes(const char* section, int scen) {
    bool ok = true;
    for (int m = 1; m < 3; m++) {
        const PassResult& a = g_pr[0];
        const PassResult& b = g_pr[m];
        const size_t n = std::min(a.res.size(), b.res.size());
        size_t i = 0;
        while (i < n && a.res[i] == b.res[i]) i++;
        std::string d;
        if (i < n || a.res.size() != b.res.size()) {
            char t[256];
            if (i < n) snprintf(t, sizeof t, "op %u %s: %08x vs %08x (%s)", (unsigned)i, a.ops[i].c_str(), a.res[i], b.res[i], b.ops[i].c_str());
            else snprintf(t, sizeof t, "%u vs %u results", (unsigned)a.res.size(), (unsigned)b.res.size());
            d = t;
        }
        if (d.empty()) d = snap_diff(a.s, b.s);
        if (d.empty() && a.files != b.files) d = "the sandbox's files differ";
        if (!d.empty()) {
            ok = false;
            g_stats[std::string(section) + " scenarios (" + k_mode[m] + ")"].fails++;
            fail(section, "scenario %d, %s: %s", scen, k_mode[m], d.c_str());
        }
        g_stats[std::string(section) + " scenarios (" + k_mode[m] + ")"].calls++;
    }
    return ok;
}

// ---- heap ---------------------------------------------------------------------------------------------------------------
static void heap_script(int ops) {
    void* live[64] = {};
    for (int k = 0; k < ops; k++) {
        const int i = ri(0, 63);
        const int op = ri(0, 99);
        uint32_t n = chance(5) ? 0 : chance(70) ? (uint32_t)ri(1, 300) : chance(90) ? (uint32_t)ri(1, 70000) : chance(50) ? 0xffffffe0u + (uint32_t)ri(0, 31) : (uint32_t)rnd();
        if (op < 30) { live[i] = cc<void*>(0x4d1de0, n); res("malloc", P(live[i])); }
        else if (op < 40) { const uint32_t a = (uint32_t)ri(0, 20), b = chance(50) ? n / (a ? a : 1) : (uint32_t)rnd(); live[i] = cc<void*>(0x4d7170, a, b); res("calloc", P(live[i])); }
        else if (op < 60) { live[i] = cc<void*>(0x4d1d50, chance(10) ? (void*)0 : live[i], n); res("realloc", P(live[i])); }
        else if (op < 80) { cc<void>(0x4d58b0, live[i]); live[i] = 0; res("free", 0); }
        else if (op < 88) { if (live[i]) res("msize", cc<uint32_t>(0x4d1dc0, live[i])); }
        else if (op < 94) { live[i] = cc<void*>(0x4d1e00, n, 0); res("nh_malloc", P(live[i])); }
        else { live[i] = cc<void*>(0x4d1e40, n); res("heap_alloc", P(live[i])); }
        if (live[i] && chance(50)) {
            HBlk* b = heap_blk(live[i]);
            if (b && b->live) memset(live[i], k & 0xff, std::min<uint32_t>(b->size, 64));
        }
    }
}
static void test_heap(int scenarios) {
    printf("heap: %d scripts, three passes each\n", scenarios);
    int ok = 0;
    for (int s = 0; s < scenarios; s++) {
        const uint32_t seed = 0x1234567u + (uint32_t)s * 7919u;
        for (int m = 0; m < 3; m++) {
            pass_begin(m, seed);
            heap_script(ri(20, 400));
            pass_end();
        }
        if (compare_passes("heap", s)) ok++;
    }
    printf("  %d of %d heap scripts identical in both modes\n", ok, scenarios);
}

// ---- io -----------------------------------------------------------------------------------------------------------------
static const char* const k_names[] = {"a.txt", "b.bin", "sub\\d.txt", "z.txt", "new1.txt", "new2.bin", "sub", "nodir\\x.txt",
                                      "ro.txt", "", "sub\\new3", "cr.txt", "b.bin"};
static const char* const k_modes[] = {"r", "w", "a", "r+", "w+", "a+", "rb", "wb", "ab", "rt", "wt", "r+b", "w+t", "a+b",
                                      "rc", "wn", "rS", "wR", "rT", "wD", "rbc", "rx", "", "q", "r+bt", "rSR", "rcn", "rbb"};
static char* sstr(const char* s, uint32_t at) {             // a string in the script's memory
    char* p = (char*)g_sbuf + at;
    strcpy(p, s);
    return p;
}
static char* path_of(int k, uint32_t slot) {
    char t[MAX_PATH];
    const char* n = k_names[k % (sizeof k_names / sizeof *k_names)];
    if (chance(85)) snprintf(t, sizeof t, "%s\\%s", g_sb, n);
    else snprintf(t, sizeof t, "%s", n);                     // relative: from the current directory
    return sstr(t, 0x100 * slot);
}
static void io_script(int ops) {
    CrtFile* fs[8] = {};
    int32_t fh[8];
    for (int i = 0; i < 8; i++) fh[i] = -1;
    char* buf = (char*)g_sbuf + 0x10000;
    if (chance(40)) {
        char* crp = sstr((std::string(g_sb) + "\\cr.txt").c_str(), 0x500);
        const int32_t h = cc<int32_t>(0x4d7510, (const char*)crp, chance(50) ? 0x4002u : 0x4000u, 0x40, 0x180u);
        res("sopen cr", (uint32_t)h);
        for (int j = 0; j < 3000; j++) {
            const uint32_t want = chance(70) ? (uint32_t)ri(1, 7) : (uint32_t)ri(1, 5000);
            const int32_t got = cc<int32_t>(0x4d53b0, h, buf, want);
            res("read cr", (uint32_t)got ^ fnv(buf, got > 0 ? (size_t)got : 0) << 8);
            if (got <= 0) break;
            if (chance(5)) res("lseek cr", (uint32_t)cc<int32_t>(0x4d50c0, h, ri(-3, 3), 1u));
        }
        res("close cr", (uint32_t)cc<int32_t>(0x4d58d0, h));
        CrtFile* f = cc<CrtFile*>(0x4d0610, (const char*)crp, (const char*)sstr(chance(50) ? "r" : "r+", 0x600));
        res("fopen cr", P(f));
        for (int j = 0; f && j < 12000; j++) {
            int c = --f->cnt >= 0 ? (uint8_t)*f->ptr++ : cc<int32_t>(0x4d52c0, f);
            res("getc cr", (uint32_t)c);
            if (c == -1) break;
            if (chance(3)) res("ftell cr", (uint32_t)cc<int32_t>(0x4d02e0, f));
            if (chance(1)) res("ungetc cr", (uint32_t)cc<int32_t>(0x4d6450, c, f));
            if (chance(1)) res("fseek cr", (uint32_t)cc<int32_t>(0x4d0200, f, ri(-10, 10), 1));
        }
        if (f) res("fclose cr", (uint32_t)cc<int32_t>(0x4d0630, f));
    }
    if (chance(15)) {                               // past the first 32 handles: a new ioinfo block, _nhandle 64
        int32_t many[40];
        const int m = ri(30, 40);
        for (int j = 0; j < m; j++) {
            many[j] = cc<int32_t>(0x4d7510, (const char*)path_of(1, 3), 0x8000u, 0x40, 0x180u);
            res("sopen many", (uint32_t)many[j]);
        }
        for (int j = 0; j < m; j++)
            if (chance(80)) res("close many", (uint32_t)cc<int32_t>(0x4d58d0, many[j]));
    }
    auto pick = [&]() -> int32_t {
        int32_t open[8];
        int no = 0;
        for (int j = 0; j < 8; j++)
            if (fh[j] >= 0) open[no++] = fh[j];
        if (chance(12)) return PIPE_FH;
        if (no && chance(85)) return open[ri(0, no - 1)];
        return ri(-2, 70);
    };
    for (int k = 0; k < ops; k++) {
        const int op = ri(0, 199);
        const int i = ri(0, 7);
        CrtFile* s = fs[i] ? fs[i] : chance(5) ? (CrtFile*)(uintptr_t)(CRT_IOB_VA + 0x20 * (uint32_t)ri(0, 2)) : 0;
        const uint32_t n = chance(50) ? (uint32_t)ri(0, 40) : chance(80) ? (uint32_t)ri(0, 6000) : (uint32_t)ri(0, 70000);
        const uint32_t sz = chance(70) ? 1 : (uint32_t)ri(0, 9);
        if (op < 22) {
            const char* md = k_modes[ri(0, (int)(sizeof k_modes / sizeof *k_modes) - 1)];
            if (fs[i] && chance(70)) { res("fclose", (uint32_t)cc<int32_t>(0x4d0630, fs[i])); fs[i] = 0; }
            fs[i] = chance(85) ? cc<CrtFile*>(0x4d0610, (const char*)path_of(ri(0, 12), 1), (const char*)sstr(md, 0x200))
                               : cc<CrtFile*>(0x4d05e0, (const char*)path_of(ri(0, 12), 1), (const char*)sstr(md, 0x200), ri(0x0f, 0x41));
            res("fopen", P(fs[i]));
        } else if (op < 30) {
            if (s && (s->flag & 0x83)) { res("fclose", (uint32_t)cc<int32_t>(0x4d0630, s)); if (s == fs[i]) fs[i] = 0; }
        } else if (op < 48) {
            if (s && (s->flag & 0x83)) {
                const uint32_t cnt = sz ? n / sz : n;
                res("fread", cc<uint32_t>(0x4d0490, (void*)buf, sz, cnt, s));
            }
        } else if (op < 66) {
            if (s && (s->flag & 0x83)) {
                for (uint32_t j = 0; j < 64; j++) buf[0x8000 + j] = "ab\ncd\r\n\x1a\r\nxyz\n\n\re"[(j + k) % 17];
                const uint32_t cnt = sz ? std::min<uint32_t>(n, 0x7000) / sz : n;
                res("fwrite", cc<uint32_t>(0x4d06a0, (const void*)(buf + 0x8000 - (chance(50) ? 0 : 0x8000)), sz, cnt, s));
            }
        } else if (op < 82) {                       // getc / putc, the macros as the game inlines them
            if (s && (s->flag & 0x83)) {
                const int m = ri(1, 40);
                for (int j = 0; j < m; j++) {
                    if (chance(50)) {
                        int c = --s->cnt >= 0 ? (uint8_t)*s->ptr++ : cc<int32_t>(0x4d52c0, s);
                        res("getc", (uint32_t)c);
                    } else {
                        const int c = "a\n\r\x1aZ"[ri(0, 4)];
                        int r = --s->cnt >= 0 ? (uint8_t)(*s->ptr++ = (char)c) : cc<int32_t>(0x4d2160, c, s);
                        res("putc", (uint32_t)r);
                    }
                }
            }
        } else if (op < 88) {
            if (s && (s->flag & 0x83)) res("ungetc", (uint32_t)cc<int32_t>(0x4d6450, chance(10) ? -1 : ri(0, 300), s));
        } else if (op < 100) {
            if (s && (s->flag & 0x83)) {
                const int32_t off = chance(50) ? ri(-20, 20) : ri(-100, 9000);
                const int32_t wh = chance(90) ? ri(0, 2) : ri(-1, 5);
                res("fseek", (uint32_t)cc<int32_t>(0x4d0200, s, off, wh));
            }
        } else if (op < 110) {
            if (s && (s->flag & 0x83)) res("ftell", (uint32_t)cc<int32_t>(0x4d02e0, s));
        } else if (op < 116) {
            res("fflush", (uint32_t)cc<int32_t>(0x4d5180, chance(20) ? (CrtFile*)0 : s));
        } else if (op < 118) {
            res("flushall", (uint32_t)cc<int32_t>(0x4d5240));
        } else if (op < 130) {                      // the low-level handles
            static const uint32_t oflags[] = {0, 1, 2, 0x8000, 0x4000, 0x8001, 0x4002, 0x101, 0x301, 0x109, 0x502, 0x302,
                                              0x8102, 0x4302, 0x40, 0x1001, 0x20, 0x10, 0x80, 3, 0x200, 0x400, 0x600,
                                              0x4108, 0x8002, 0xc002, 0x4402};
            const uint32_t of = oflags[ri(0, (int)(sizeof oflags / sizeof *oflags) - 1)] | (chance(10) ? (uint32_t)ri(0, 3) : 0);
            const int32_t sh = chance(70) ? 0x40 : chance(90) ? 0x10 * ri(1, 4) : ri(0, 0x50);
            if (fh[i] >= 0 && chance(60)) res("close", (uint32_t)cc<int32_t>(0x4d58d0, fh[i]));
            fh[i] = cc<int32_t>(0x4d7510, (const char*)path_of(ri(0, 12), 2), of, sh, chance(70) ? 0x180u : (uint32_t)ri(0, 0x1ff));
            res("sopen", (uint32_t)fh[i]);
        } else if (op < 140) {
            const int32_t h = pick();
            const uint32_t rn = chance(40) ? (uint32_t)ri(1, 8) : std::min<uint32_t>(n, 0x8000);
            res("read", (uint32_t)cc<int32_t>(0x4d53b0, h, buf, rn));
            res("read data", fnv(buf, 0x40));
        } else if (op < 150) {
            const int32_t h = pick();
            for (uint32_t j = 0; j < 0x900; j++) buf[0x9000 + j] = "\n\nab\n\x1a\r"[(j * 7 + k) % 7];
            res("write", (uint32_t)cc<int32_t>(0x4d59e0, h, (const char*)(buf + 0x9000 + (chance(80) ? 0 : 5)), std::min<uint32_t>(n, 0x900)));
        } else if (op < 156) {
            const int32_t h = pick();
            res("lseek", (uint32_t)cc<int32_t>(0x4d50c0, h, chance(50) ? ri(-5, 50) : ri(-100, 20000), (uint32_t)(chance(90) ? ri(0, 2) : ri(0, 5))));
        } else if (op < 160) {
            const int32_t h = pick();
            res("close", (uint32_t)cc<int32_t>(0x4d58d0, h));
            if (h == fh[i]) fh[i] = -1;
        } else if (op < 163) {
            const int32_t h = pick();
            res("commit", (uint32_t)cc<int32_t>(0x4d74a0, h));
        } else if (op < 167) {
            const int32_t h = pick();
            res("chsize", (uint32_t)cc<int32_t>(0x4d8210, h, chance(50) ? ri(0, 100) : ri(-3, 12000)));
        } else if (op < 170) {
            const int32_t h = pick();
            res("setmode", (uint32_t)cc<int32_t>(0x4d83c0, h, chance(45) ? 0x8000 : chance(80) ? 0x4000 : ri(0, 0x9000)));
        } else if (op < 172) {
            res("isatty", (uint32_t)cc<int32_t>(0x4d6680, pick()));
        } else if (op < 174) {
            res("get_osfhandle", (uint32_t)cc<intptr_t>(0x4d7450, pick()));
        } else if (op < 176) {
            cc<void>(0x4d4840, chance(50) ? (uint32_t)ri(0, 300) : rnd());
            res("dosmaperr", 0);
        } else if (op < 180) {
            const int32_t len = chance(70) ? ri(1, 300) : ri(-5, 5);
            res("getcwd", P(cc<char*>(0x4d4940, chance(30) ? (char*)0 : buf + 0xb000, len)));
        } else if (op < 183) {
            const int32_t len = chance(70) ? ri(1, 300) : ri(-5, 5);
            res("getdcwd", P(cc<char*>(0x4d4960, ri(0, 27), chance(30) ? (char*)0 : buf + 0xb000, len)));
        } else if (op < 187) {
            static const char* const dirs[] = {"sub", "..", ".", "nodir", "sub\\..\\sub", "", "\\\\server\\share"};
            const char* d = chance(30) ? g_sb : dirs[ri(0, 6)];
            res("chdir", (uint32_t)cc<int32_t>(0x4cf830, (const char*)sstr(d, 0x300)));
        } else if (op < 192) {
            static const char* const ps[] = {"a.txt", "sub\\..\\b.bin", "..\\x", "", ".", "nodir\\y\\z", "C:\\Windows"};
            const uint32_t ml = chance(70) ? (uint32_t)ri(1, 300) : (uint32_t)ri(0, 30);
            res("fullpath", P(cc<char*>(0x4cfbe0, chance(30) ? (char*)0 : buf + 0xb400, chance(5) ? (const char*)0 : (const char*)sstr(ps[ri(0, 6)], 0x400), ml)));
        } else if (op < 194) {
            res("stbuf", (uint32_t)cc<int32_t>(0x4d4ee0, (CrtFile*)(uintptr_t)(CRT_IOB_VA + 0x20 * (uint32_t)ri(0, 3))));
        } else if (op < 196) {
            if (s) { cc<void>(0x4d4f80, ri(0, 1), s); res("ftbuf", 0); }
        } else if (op < 198) {
            res("getstream", P(cc<CrtFile*>(0x4d5830)));
        } else {
            res("alloc_osfhnd", (uint32_t)cc<int32_t>(0x4d7250));
        }
        if (g_mode == M_ORIG) {}
    }
    // and the end: close everything the script still has open, then every stream (fcloseall's work, by fclose)
    for (int i = 0; i < 8; i++) {
        if (fs[i] && (fs[i]->flag & 0x83)) res("fclose", (uint32_t)cc<int32_t>(0x4d0630, fs[i]));
        if (fh[i] >= 0) res("close", (uint32_t)cc<int32_t>(0x4d58d0, fh[i]));
    }
    res("flsall(0)", (uint32_t)cc<int32_t>(0x4d5250, 0));
    if (chance(20)) {                               // __initstdio again, from other _nstream values (a fresh __piob)
        static const int32_t ns[] = {0, 3, 19, 20, 100, 512, 600};
        crt_nstream = ns[ri(0, 6)];
        cc<void>(0x4d4fd0);
        res("initstdio", (uint32_t)crt_nstream ^ P(crt_piob));
    }
    if (chance(30)) {                               // __endstdio: _flushall, and _fcloseall when exiting
        CRT_G(uint8_t, CRT_EXITFLAG_VA) = (uint8_t)ri(0, 1);
        cc<void>(0x4d50a0);
        res("endstdio", 0);
    }
}
static void test_io(int scenarios) {
    printf("io: %d scripts, three passes each\n", scenarios);
    int ok = 0;
    for (int s = 0; s < scenarios; s++) {
        const uint32_t seed = 0x9e3779b9u + (uint32_t)s * 104729u;
        for (int m = 0; m < 3; m++) {
            pass_begin(m, seed);
            const DWORD f = guarded([&] { io_script(ri(10, 160)); });
            if (f) res("FAULT", f);
            pass_end();
        }
        if (compare_passes("io", s)) ok++;
    }
    printf("  %d of %d io scripts identical in both modes\n", ok, scenarios);
}

// =========================================================================================================================
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    if (!GetEnvironmentVariableA("VP_CRTB_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const double scale = argc > 1 ? atof(argv[1]) : 1.0;
    if (argc > 2) g_rng = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    const char* only = argc > 3 ? argv[3] : 0;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* sl = strstr(exe, "\\test\\world_crt_io.cpp");
    if (!sl) { printf("can't find the repository from %s\n", __FILE__); return 2; }
    strcpy(sl, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_iat();
    patch_jmp(0x4cf730, (void*)&fatal_amsg);                   // _amsg_exit (deferred original): fatal here
    g_heap = (uint8_t*)VirtualAlloc((void*)0x30000000, HEAP_ARENA, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_sbuf = (uint8_t*)VirtualAlloc((void*)0x2f000000, SB_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_heap || !g_sbuf) { printf("can't place the heap arena / the script's memory\n"); return 2; }
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    snprintf(g_sb, sizeof g_sb, "%scrtb_sb_%lu", tmp, GetCurrentProcessId());
    // the C runtime's start-up, as WinMainCRTStartup runs it (the originals)
    callv<int32_t>(0x4d4470, 1);                               // _heap_init
    callv<void>(0x4d4290);                                     // _ioinit
    callv<void>(0x4d4280);                                     // __initmbctable
    callv<void>(0x4d4fd0);                                     // __initstdio
    g_data0.assign((uint8_t*)DATA_AT, (uint8_t*)DATA_AT + DATA_SIZE);
    g_heap_top0 = g_heap_top;
    g_heap0.assign(g_heap, g_heap + g_heap_top);
    printf("imports: %d stubbed, %d real (KERNEL32), %d trapped; rewrites registered: ", g_nstubbed, g_nreal, g_ntrapped);
    int nreg = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) nreg++;
    printf("%d; start-up heap %u bytes; ACP %u; sandbox %s\n", nreg, g_heap_top0, GetACP(), g_sb);
    unsigned int cw;
    _controlfp_s(&cw, _CW_DEFAULT, 0xffffffff);
    auto want = [&](const char* s) { return !only || !strcmp(only, s); };
    auto N = [&](int n) { return std::max(1, (int)(n * scale)); };
    if (want("fdiv")) {
        test_fdiv(N(200000));
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%scrt_fdiv_code.txt", tmp);
        dump_fdiv_code(p);
        printf("  (the rewrites' and originals' entry points: %s)\n", p);
    }
    if (want("str")) test_str(N(1000000));
    if (want("ctype")) test_ctype(N(200000));
    if (want("sort")) test_sort(N(20000));
    if (want("heap")) test_heap(N(400));
    if (want("io")) test_io(N(400));
    SetCurrentDirectoryA(tmp);
    rm_tree(g_sb);
    if (!g_opstats.empty()) {
        printf("script operations (the originals' pass): calls / results other than 0 and -1\n");
        for (auto& o : g_opstats) printf("  %-16s %8ld %8ld\n", o.first.c_str(), o.second.first, o.second.second);
    }
    int never = 0;
    std::string nv;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        auto c = g_chain_count.find(r->at);
        if (!g_stats.count(r->name) && (c == g_chain_count.end() || !*c->second)) never++, nv += std::string(" ") + r->name;
    }
    if (!g_chain_count.empty()) {
        printf("calls into each rewrite in the chain passes:\n");
        int k = 0;
        for (ChainReg* r = ChainReg::head(); r; r = r->next)
            printf("  %-20s %9u%s", r->name, *g_chain_count[r->at], ++k % 4 ? "" : "\n");
        printf("\n");
    }
    printf("%ld mismatches; %d rewrites not exercised by these sections:%s\n", g_failures, never, nv.c_str());
    return g_failures ? 1 : 0;
}
