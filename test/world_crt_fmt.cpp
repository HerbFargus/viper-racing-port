// world_crt_fmt.cpp -- stage LIBC, group A (hook/crt_float.cpp, hook/crt_fmt.cpp) against the originals, outside the game
// (docs/PORTING.md, steps 2 and 3): the printf / scanf engines and their front ends, the float conversions and the
// _LDBL12 arithmetic, strtol / atof / atoi, _ftol and the 64-bit helpers, the FPU control routines and the x87
// transcendental dispatchers.
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_crt_fmt.cpp
//        /Fo%TEMP%\crtw\ /Fe%TEMP%\crtw\world_crt_fmt.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_crt_fmt.exe [rounds] [seed] [family-filter]
//          (a round is a few thousand calls spread over every family; VP_CRT_VERBOSE=1 prints each family's count)
//
// Loads out\race_v10.exe at 0x400000 as test/fuzz.cpp does (KERNEL32's imports resolved for the CRT's code), runs the
// CRT's _cfltcvt_init (its %e/%f/%g converters) and nothing else of its start-up. Each check builds its inputs in an
// arena, runs the original (from the image), keeps its results -- the return registers, ST0 and the FPU's status /
// control / tag words, any fault, the arena and the C runtime's statics (its .data 0x5024a0..0x503c00 and bss
// 0x5d5500..0x5d6f2c) -- restores everything and runs the rewrite, and compares. Every check is made in one of two ways
// at random: "isolated" (the rewrite called directly; what it calls by address is the original in the image) and
// "chain" (every rewrite of the group hooked into the image for the rewrite's pass). The FPU starts each pass from
// fninit and a random control word: 24-, 53- or 64-bit precision, any rounding, mostly masked, sometimes with invalid,
// zero-divide and overflow unmasked (the physics thread's mode).
//
// The families: sprintf / vsprintf / _output / fprintf / printf on random formats (every flag, width and precision
// form, h / l / I64 / w sizes, every conversion, '*' arguments, doubles from every class: zeros, denormals, specials,
// halfway cases, every exponent) and "chaos" formats (random spec characters in any order, safe arguments); sscanf /
// _input on formats built with matching (and not) input text; strgtold12 / _fltin / atof / _atodbl / _atoflt / _fassign;
// strtoxl / strtoul / atol / atoi; _fltout / ___dtold / $I10_OUTPUT / _fptostr / _cftoe / _cftof / _cftog / _cfltcvt /
// _cropzeros / _forcdecpt / _positive; the intrncvt and mantold / tenpow helpers; __ftol, __allmul, __alldiv,
// __aulldiv, __aullrem; _control87 / _controlfp / _clearfp / _statfp / _clrfp / _ctrlfp / _set_statfp /
// _setdefaultprecision / _abstract_cw / _hw_cw / _abstract_sw; _finite / _isnan / _set_exp / _decomp; acos / asin /
// atan / atan2 / fmod and their _CI forms (through the dispatchers _ctrandisp1/2, _cintrindisp1/2, _trandisp1/2,
// _fload).
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
#include <algorithm>
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define VP_CRT_HARNESS              // (no shadow-mode redirection: rewrites are registered below)
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
void Footprint::object(void* obj, const char* what) { add(obj, 0x20, what); }

#include "../hook/crt_float.cpp"
#include "../hook/crt_fmt.cpp"

typedef uint32_t u32;
typedef uint64_t u64;

// ---- random values ---------------------------------------------------------------------------------------------------
static u32 g_rng = 0x2545f491u;
static u32 rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static u64 rnd64() { return ((u64)rnd() << 32) | rnd(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (u32)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }

static u64 dbits(double d) { u64 u; memcpy(&u, &d, 8); return u; }
static double bitsd(u64 u) { double d; memcpy(&d, &u, 8); return d; }

// a double from every class the conversions treat differently
static u64 rand_double() {
    switch (rnd() % 16) {
    case 0: return rnd64();                                                       // any bits
    case 1: {                                                                     // specials
        static const u64 sp[] = {0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                 0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff0000000000001ull,
                                 0x7ff4000000000000ull, 0xfff0000000000001ull, 0x0000000000000001ull,
                                 0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull,
                                 0x3ff0000000000000ull, 0xbff0000000000000ull, 0x3fe0000000000000ull,
                                 0x7fffffffffffffffull, 0xffffffffffffffffull, 0x8000000000000001ull};
        return sp[rnd() % (sizeof sp / sizeof sp[0])];
    }
    case 2: return rnd64() & 0x800fffffffffffffull;                               // denormals
    case 3: {                                                                     // halfway cases: (n + 0.5) / 10^k
        const double n = (double)(rnd() % 100000);
        const int k = ri(0, 12);
        return dbits(((n + 0.5) / pow(10.0, k)) * (chance(50) ? 1 : -1));
    }
    case 4: {                                                                     // 9s that round up
        const int k = ri(1, 17);
        double v = pow(10.0, k) - (chance(50) ? 0.5 : 1.0);
        v /= pow(10.0, ri(0, 20));
        return dbits(chance(30) ? -v : v);
    }
    case 5: {                                                                     // decimal values
        const double m = (double)(rnd64() % 1000000000000ull);
        return dbits(m * pow(10.0, ri(-330, 300)) * (chance(50) ? 1 : -1));
    }
    case 6: return dbits((double)(int32_t)rnd() / (double)(1 << ri(0, 30)));       // small integers and fractions
    case 7: return dbits((double)ri(-1000, 1000));
    case 8: return ((u64)ri(0, 2047) << 52) | (rnd64() & 0x800fffffffffffffull);  // any exponent
    case 9: return dbits(pow(10.0, ri(-310, 308)) * (chance(50) ? 1 : -1));         // powers of ten
    case 10: return dbits(ldexp(1.0, ri(-1074, 1023)));                            // powers of two
    case 11: return dbits((double)ri(0, 99999) / 1000.0);                         // game-like values
    case 12: return dbits((double)(float)((double)(int32_t)rnd() / 65536.0));     // floats widened
    default: return ((u64)ri(0x3c0, 0x440) << 52) | (rnd64() & 0x800fffffffffffffull);   // around 1
    }
}
// an 80-bit value: from a double mostly, else any bits
static void rand_ext(uint8_t out[10]) {
    if (chance(80)) {
        const double d = bitsd(rand_double());
        __asm { fld qword ptr d }                               // (an exact load; a signalling NaN quietened -- also tested raw)
        uint8_t* o = out;
        __asm { mov eax, o
                fstp tbyte ptr [eax] }
        if (chance(10)) { u64 m = rnd64(); memcpy(out, &m, 8); }
    } else {
        u64 m = rnd64();
        memcpy(out, &m, 8);
        const uint16_t e = (uint16_t)(chance(50) ? rnd() : (0x3fff + ri(-80, 80)) | (rnd() & 0x8000));
        memcpy(out + 8, &e, 2);
    }
}

// ---- the original, loaded at 0x400000 (as test/fuzz.cpp) ------------------------------------------------------------
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
            FARPROC fp = GetProcAddress(m, fn);
            if (fp) iat->u1.Function = (DWORD)(uintptr_t)fp;
        }
    }
    free(file);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_CRTW_CHILD", "1");
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

// ---- the chain: every rewrite of the group hooked into the image ---------------------------------------------------------
struct ChainSave { u32 at; uint8_t b[5]; };
static ChainSave g_chain_save[256];
static int g_nchain;
static void patch_jmp(u32 at, void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    const int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}
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
static void* rewrite_of(u32 v10) {
    for (ChainReg* r = ChainReg::head(); r; r = r->next)
        if (r->at == v10) return r->fn;
    printf("no rewrite registered at %08x\n", v10);
    exit(3);
}

// ---- the arena and the statics -----------------------------------------------------------------------------------------
enum { ARENA_SIZE = 0x100000 };
static uint8_t* g_arena;              // at a fixed address (allocated once)
static u32 g_used;
static uint8_t* aalloc(u32 n, u32 align = 4) {
    g_used = (g_used + align - 1) & ~(align - 1);
    uint8_t* p = g_arena + g_used;
    g_used += n;
    if (g_used > ARENA_SIZE) { printf("arena overflow\n"); exit(3); }
    return p;
}
static void arena_reset() { g_used = 0; }
struct GRange { u32 at, n; };
static const GRange g_statics[] = {{0x005024a0, 0x1760}, {0x005d5500, 0x1a2c}};
enum { STATICS_BYTES = 0x1760 + 0x1a2c };
static void save_statics(uint8_t* to) {
    for (const GRange& r : g_statics) { memcpy(to, (void*)(uintptr_t)r.at, r.n); to += r.n; }
}
static void load_statics(const uint8_t* from) {
    for (const GRange& r : g_statics) { memcpy((void*)(uintptr_t)r.at, from, r.n); from += r.n; }
}
static u32 static_addr(u32 off) {
    for (const GRange& r : g_statics) {
        if (off < r.n) return r.at + off;
        off -= r.n;
    }
    return 0;
}

// ---- calling: a thunk that sets up the FPU, pushes dwords (and FPU-stack operands), calls, and captures ------------------
static u32 g_args[96];
static int g_nargs;
static u32 g_fn;
static u32 g_eax, g_edx, g_esp_save;
static uint16_t g_cw_in, g_sw_out, g_cw_out;
static const uint16_t g_cw_masked = 0x037f;
static uint8_t g_st_in[2][10];
static int g_nst_in, g_st_ret;
static uint8_t g_st_out[10];
static uint8_t g_fenv[28];

static __declspec(naked) void call_thunk() {
    __asm {
        pushad
        mov g_esp_save, esp
        fninit
        fldcw g_cw_in
        cmp g_nst_in, 2
        jb one_in
        lea eax, g_st_in
        fld tbyte ptr [eax + 10]
    one_in:
        cmp g_nst_in, 1
        jb no_in
        lea eax, g_st_in
        fld tbyte ptr [eax]
    no_in:
        mov ecx, g_nargs
    push_loop:
        test ecx, ecx
        jz go
        dec ecx
        lea eax, g_args
        push dword ptr [eax + ecx * 4]
        jmp push_loop
    go:
        call dword ptr g_fn
        mov g_eax, eax
        mov g_edx, edx
        fnstsw g_sw_out
        fnstcw g_cw_out
        fnclex
        fldcw g_cw_masked
        cmp g_st_ret, 0
        je no_st
        lea eax, g_st_out
        fstp tbyte ptr [eax]
    no_st:
        fnstenv g_fenv
        fninit
        mov esp, g_esp_save
        popad
        ret
    }
}
static u32 run_guarded() {
    __try {
        call_thunk();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const u32 code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        _clearfp();
        _fpreset();
        return code;
    }
    return 0;
}

struct Result {
    u32 fault, eax, edx;
    uint16_t sw, cw, tw;
    uint8_t st[10];
};
static Result capture(u32 fault) {
    Result r;
    memset(&r, 0, sizeof r);
    r.fault = fault;
    if (fault) return r;
    r.eax = g_eax;
    r.edx = g_edx;
    r.sw = g_sw_out;
    r.cw = g_cw_out;
    memcpy(&r.tw, g_fenv + 8, 2);
    memcpy(r.st, g_st_out, 10);
    return r;
}

// what a check compares in the return registers
enum RetKind { RET_NONE, RET_EAX, RET_EDX_EAX, RET_AX, RET_ST0, RET_AL };

struct Stats { const char* name; long checks, chain, failed; };
static std::vector<Stats> g_stats;
static Stats* g_cur;
static long g_total_fail;
static bool g_stop_on_fail = true;
static std::string g_desc;             // the current check's inputs, for a report

static uint8_t* g_arena_save;
static uint8_t* g_arena_orig;
static uint8_t g_statics_save[STATICS_BYTES], g_statics_orig[STATICS_BYTES], g_statics_new[STATICS_BYTES];

static u32 rand_cw() {
    static const uint16_t pcs[] = {0x000, 0x200, 0x300};               // 24, 53, 64 bits
    uint16_t cw = 0x3f | pcs[rnd() % 3] | (uint16_t)((rnd() & 3) << 10);
    if (chance(15)) cw &= (uint16_t)~(rnd() & 0x0d);                   // sometimes IM / ZM / OM unmasked
    return cw | 0x40;
}

static void hexdump(const char* tag, const uint8_t* p, u32 n) {
    printf("      %s:", tag);
    for (u32 i = 0; i < n; i++) printf(" %02x", p[i]);
    printf("\n");
}

// One check: the original at v10 (image unpatched), then the rewrite (directly, or the chain), from the same state.
static bool g_trace;
static bool check(u32 v10, RetKind rk, bool fpu_args_from_st = false) {
    (void)fpu_args_from_st;
    const bool chain = chance(50);
    if (g_trace) printf("  [%s %s] %s\n", g_cur->name, chain ? "chain" : "isolated", g_desc.c_str());
    g_cur->checks++;
    if (chain) g_cur->chain++;
    g_cw_in = (uint16_t)rand_cw();
    const u32 used = g_used;
    memcpy(g_arena_save, g_arena, used);
    save_statics(g_statics_save);

    g_st_ret = rk == RET_ST0;
    g_fn = v10;
    memset(g_st_out, 0, 10);
    const Result ro = capture(run_guarded());
    memcpy(g_arena_orig, g_arena, used);
    save_statics(g_statics_orig);

    memcpy(g_arena, g_arena_save, used);
    load_statics(g_statics_save);
    memset(g_st_out, 0, 10);
    if (chain) {
        chain_patch();
        g_fn = v10;
    } else {
        g_fn = (u32)(uintptr_t)rewrite_of(v10);
    }
    const Result rn = capture(run_guarded());
    if (chain) chain_unpatch();
    save_statics(g_statics_new);

    bool bad = false;
    char why[256] = "";
    if (ro.fault != rn.fault) { bad = true; sprintf(why, "fault: original %08x, rewrite %08x", ro.fault, rn.fault); }
    else if (!ro.fault) {
        bool rbad = false;
        switch (rk) {
        case RET_EAX: rbad = ro.eax != rn.eax; break;
        case RET_EDX_EAX: rbad = ro.eax != rn.eax || ro.edx != rn.edx; break;
        case RET_AX: rbad = (uint16_t)ro.eax != (uint16_t)rn.eax; break;
        case RET_AL: rbad = (uint8_t)ro.eax != (uint8_t)rn.eax; break;
        case RET_ST0: rbad = memcmp(ro.st, rn.st, 10) != 0; break;
        default: break;
        }
        if (rbad) { bad = true; sprintf(why, "return: original eax %08x edx %08x, rewrite eax %08x edx %08x", ro.eax, ro.edx, rn.eax, rn.edx); }
        else if (ro.sw != rn.sw || ro.cw != rn.cw || ro.tw != rn.tw) {
            bad = true;
            sprintf(why, "FPU: original sw %04x cw %04x tw %04x, rewrite sw %04x cw %04x tw %04x", ro.sw, ro.cw, ro.tw, rn.sw, rn.cw, rn.tw);
        }
    }
    if (!bad && memcmp(g_arena_orig, g_arena, used)) {
        bad = true;
        u32 i = 0;
        while (g_arena_orig[i] == g_arena[i]) i++;
        sprintf(why, "arena +%#x: original %02x, rewrite %02x", i, g_arena_orig[i], g_arena[i]);
    }
    if (!bad && memcmp(g_statics_orig, g_statics_new, STATICS_BYTES)) {
        bad = true;
        u32 i = 0;
        while (g_statics_orig[i] == g_statics_new[i]) i++;
        sprintf(why, "static %08x: original %02x, rewrite %02x", static_addr(i), g_statics_orig[i], g_statics_new[i]);
    }
    if (bad) {
        g_cur->failed++;
        g_total_fail++;
        if (g_cur->failed <= 3) {
            printf("  MISMATCH %s (%s, cw %04x): %s\n", g_cur->name, chain ? "chain" : "isolated", g_cw_in, why);
            if (!g_desc.empty()) printf("      inputs: %s\n", g_desc.c_str());
            if (rk == RET_ST0 && !ro.fault) { hexdump("ST0 original", ro.st, 10); hexdump("ST0 rewrite ", rn.st, 10); }
        }
    }
    // carry on from the original's state
    memcpy(g_arena, g_arena_orig, used);
    load_statics(g_statics_orig);
    return !bad;
}

static void family(const char* name) {
    for (Stats& s : g_stats)
        if (!strcmp(s.name, name)) { g_cur = &s; return; }
    g_stats.push_back({name, 0, 0, 0});
    g_cur = &g_stats.back();
}
static void set_args(std::initializer_list<u32> a) {
    g_nargs = 0;
    for (u32 v : a) g_args[g_nargs++] = v;
    g_nst_in = 0;
}
static u32 P(const void* p) { return (u32)(uintptr_t)p; }

// ---- strings ---------------------------------------------------------------------------------------------------------------
static char* astr(const std::string& s) {
    char* p = (char*)aalloc((u32)s.size() + 1, 1);
    memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
static std::string rand_text(int maxlen, const char* alphabet = 0) {
    std::string s;
    const int n = ri(0, maxlen);
    for (int i = 0; i < n; i++) {
        if (alphabet) s += alphabet[rnd() % strlen(alphabet)];
        else s += (char)(chance(85) ? ri(0x20, 0x7e) : ri(0x01, 0xff));
    }
    return s;
}
// a number as text, in many shapes (for strtod / scanf / atof / strtol)
static std::string rand_number_text(bool fp) {
    std::string s;
    if (chance(20)) s += rand_text(3, " \t\n\r\v\f");
    if (chance(30)) s += chance(50) ? '-' : '+';
    if (chance(10)) s += chance(50) ? '-' : '+';
    if (!fp && chance(20)) s += chance(50) ? "0x" : "0X";
    if (chance(15)) s += std::string((size_t)ri(1, 30), '0');
    const int nd = chance(10) ? ri(20, 60) : ri(0, 19);
    for (int i = 0; i < nd; i++) s += (char)(fp ? '0' + ri(0, 9) : "0123456789abcdefABCDEFzZ"[rnd() % 24]);
    if (fp) {
        if (chance(60)) {
            s += '.';
            const int nf = chance(10) ? ri(20, 40) : ri(0, 12);
            for (int i = 0; i < nf; i++) s += (char)('0' + ri(0, 9));
        }
        if (chance(40)) {
            s += "eEdD"[rnd() % 4];
            if (chance(50)) s += chance(50) ? '-' : '+';
            if (chance(10)) s += std::string((size_t)ri(1, 8), '0');
            const int ne = chance(10) ? ri(4, 9) : ri(0, 3);
            for (int i = 0; i < ne; i++) s += (char)('0' + ri(0, 9));
        }
    }
    if (chance(5)) {                                    // a printed double, exactly
        char b[64];
        sprintf(b, "%.*g", ri(1, 20), bitsd(rand_double()));
        s = b;
    }
    if (chance(30)) s += rand_text(4, " x.e+-9Ab\t");
    return s;
}

// =====================================================================================================================
// printf
// =====================================================================================================================
struct Fmt { std::string text; std::vector<u32> args; };

// a printf format and its arguments. chaos: random spec characters anywhere, every argument a small integer (safe as
// any conversion's argument but a pointer -- so no %s / %n / %S in chaos formats)
static Fmt rand_printf(bool chaos) {
    Fmt f;
    const int nconv = ri(0, 10);
    bool text_set = false;
    for (int k = 0; k < nconv; k++) {
        if (chance(50)) {
            std::string lit = rand_text(8);
            for (char& c : lit) if (c == '%') c = '!'; else if (c == 'B') c = 'b';
            f.text += lit;
        }
        if (chance(5)) { f.text += "%%"; continue; }
        f.text += '%';
        if (chaos) {
            const int n = ri(0, 8);
            int run = 0;                                      // (at most two digits in a row: a precision past the
            for (int i = 0; i < n; i++) {                     //  512-byte buffer overruns it, in both)
                char c = "-+ #0123456789.*hlwIL6F4N"[rnd() % 25];
                run = (c >= '0' && c <= '9') ? run + 1 : c == '*' ? 2 : 0;   // (and none after a '*')
                if (run > 2) { c = '.'; run = 0; }
                f.text += c;
            }
            f.text += "cCdiouxXeEfgGpZw%"[rnd() % 17];       // (no B: an unknown type prints the last text)
            continue;
        }
        const int nflags = chance(50) ? 0 : ri(1, 4);
        for (int i = 0; i < nflags; i++) f.text += "-+ #0"[rnd() % 5];
        // the conversion first, so width / precision can be bounded for it
        static const char types[] = "cCdiouxXeEfgGnpsSB";
        char type = types[rnd() % (sizeof types - 1)];
        if (type == 'B' && !text_set) type = 'd';
        const bool fl = type == 'e' || type == 'E' || type == 'f' || type == 'g' || type == 'G';
        const bool in = strchr("diouxXp", type) != 0;
        int wkind = rnd() % 10;
        if (wkind < 4) {
        } else if (wkind < 8) {
            f.text += std::to_string(chance(90) ? ri(0, 40) : ri(41, 700));
        } else {
            f.text += '*';
            f.args.push_back((u32)(chance(80) ? ri(-40, 40) : ri(-700, 700)));
        }
        const int pmax = fl ? 150 : in ? 300 : 1000;
        const int pkind = rnd() % 10;
        if (pkind < 4) {
        } else if (pkind < 5) {
            f.text += '.';
        } else if (pkind < 8) {
            f.text += '.';
            f.text += std::to_string(chance(80) ? ri(0, 20) : ri(0, pmax));
        } else {
            f.text += ".*";
            f.args.push_back((u32)(chance(80) ? ri(-5, 20) : ri(-50, pmax)));
        }
        // sizes (some meaningless for the type: they're ignored or change it)
        bool i64 = false, wide = false;
        switch (rnd() % 12) {
        case 0: f.text += 'h'; break;
        case 1: f.text += 'l'; wide = true; break;
        case 2: f.text += 'w'; break;                         // (w is an ordinary character in this CRT's table:
                                                              //  it ends the conversion, the type is plain text)
        case 3: if (in) { f.text += "I64"; i64 = true; } break;
        case 4: f.text += 'L'; break;
        case 5: f.text += 'F'; break;
        case 6: f.text += 'N'; break;
        case 7: f.text += 'I'; break;                         // (I without 64: the I is printed, the rest is text)
        default: break;
        }
        if (f.text.back() == 'I' || f.text.back() == 'w') {   // the rest is plain text now: no argument
            f.text += type;
            continue;
        }
        f.text += type;
        switch (type) {
        case 'c':
        case 'C':
            f.args.push_back(chance(80) ? (u32)ri(0, 0x1ff) : rnd());
            text_set = true;
            break;
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'p': {
            u32 v = chance(70) ? rnd() : (u32)ri(-1000, 1000);
            if (chance(5)) v = chance(50) ? 0x80000000u : 0;
            f.args.push_back(v);
            if (i64) f.args.push_back(chance(50) ? rnd() : (v & 0x80000000u ? 0xffffffffu : 0));
            text_set = true;
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            const u64 d = rand_double();
            f.args.push_back((u32)d);
            f.args.push_back((u32)(d >> 32));
            text_set = true;
            break;
        }
        case 'n': {
            uint8_t* slot = aalloc(8);
            memset(slot, 0xa5, 8);
            f.args.push_back(P(slot));
            break;
        }
        case 's':
        case 'S': {
            const bool w = type == 'S' || wide;
            if (chance(8)) { f.args.push_back(0); text_set = true; break; }
            if (w) {
                const int n = ri(0, 40);
                uint16_t* ws = (uint16_t*)aalloc((u32)(n + 1) * 2, 2);
                for (int i = 0; i < n; i++) ws[i] = (uint16_t)(chance(90) ? ri(1, 0xff) : ri(1, 0xffff));
                ws[n] = 0;
                f.args.push_back(P(ws));
            } else {
                f.args.push_back(P(astr(rand_text(chance(90) ? 40 : 300))));
            }
            text_set = true;
            break;
        }
        case 'B':
            break;                                            // an unknown conversion: no argument
        }
    }
    if (chance(50)) {
        std::string lit = rand_text(8);
        for (char& c : lit) if (c == '%') c = '!'; else if (c == 'B') c = 'b';
        f.text += lit;
    }
    if (chaos) {                                              // plenty of small integers for whatever it consumes
        f.args.clear();
        for (int i = 0; i < 64; i++) f.args.push_back((u32)ri(0, 40));
    }
    return f;
}
static std::string describe_fmt(const Fmt& f) {
    std::string d = "\"";
    for (char c : f.text) {
        if ((uint8_t)c < 0x20 || (uint8_t)c >= 0x7f) { char b[8]; sprintf(b, "\\x%02x", (uint8_t)c); d += b; }
        else d += c;
    }
    d += "\" args";
    char b[16];
    for (size_t i = 0; i < f.args.size() && i < 24; i++) { sprintf(b, " %x", f.args[i]); d += b; }
    return d;
}

static void test_printf() {
    arena_reset();
    const bool chaos = chance(15);
    Fmt f = rand_printf(chaos);
    char* fmt = astr(f.text);
    g_desc = describe_fmt(f);
    const u32 OUTN = 0xc000;
    uint8_t* out = aalloc(OUTN, 4);
    memset(out, 0x5a, OUTN);
    // the va_list block, in the arena
    u32* va = (u32*)aalloc((u32)(f.args.size() + 4) * 4);
    for (size_t i = 0; i < f.args.size(); i++) va[i] = f.args[i];
    switch (rnd() % 5) {
    case 0:
    case 1: {
        family("sprintf");
        g_nargs = 0;
        g_args[g_nargs++] = P(out);
        g_args[g_nargs++] = P(fmt);
        for (u32 a : f.args) if (g_nargs < 96) g_args[g_nargs++] = a;
        g_nst_in = 0;
        check(0x004cf0a0, RET_EAX);
        break;
    }
    case 2:
        family("vsprintf");
        set_args({P(out), P(fmt), P(va)});
        check(0x004cf7c0, RET_EAX);
        break;
    case 3: {                                                 // _output into a buffered stream, and fprintf
        CrtFile* s = (CrtFile*)aalloc(sizeof(CrtFile));
        s->ptr = s->base = (char*)out;
        s->cnt = s->bufsiz = (int32_t)OUTN - 16;
        s->flag = CRT_IOWRT | CRT_IOMYBUF;
        s->file = 5;                                          // (lowio isn't set up: a flush would fail cleanly)
        s->charbuf = 0;
        s->tmpfname = 0;
        if (chance(50)) {
            family("output");
            set_args({P(s), P(fmt), P(va)});
            check(0x004d22b0, RET_EAX);
        } else {
            family("fprintf");
            g_nargs = 0;
            g_args[g_nargs++] = P(s);
            g_args[g_nargs++] = P(fmt);
            for (u32 a : f.args) if (g_nargs < 96) g_args[g_nargs++] = a;
            g_nst_in = 0;
            check(0x004d02a0, RET_EAX);
        }
        break;
    }
    default: {                                                // printf: stdout pointed at the arena
        family("printf");
        CrtFile* so = (CrtFile*)(uintptr_t)CRT_STDOUT_VA;
        CrtFile saved = *so;
        so->ptr = so->base = (char*)out;
        so->cnt = so->bufsiz = (int32_t)OUTN - 16;
        so->flag = CRT_IOWRT | CRT_IOMYBUF;
        so->file = 5;
        g_nargs = 0;
        g_args[g_nargs++] = P(fmt);
        for (u32 a : f.args) if (g_nargs < 96) g_args[g_nargs++] = a;
        g_nst_in = 0;
        check(0x004d01c0, RET_EAX);
        *so = saved;
        break;
    }
    }
}

// =====================================================================================================================
// scanf
// =====================================================================================================================
static void test_sscanf() {
    arena_reset();
    std::string fmt, input;
    std::vector<u32> args;
    const int nconv = ri(0, 8);
    for (int k = 0; k < nconv; k++) {
        if (chance(30)) {                                     // white space / literal text, matching or not
            std::string lit = rand_text(4, " \t\nabc:,;=xyz");
            fmt += lit;
            input += chance(80) ? lit : rand_text(4, " \t\nabc:,;=xyz");
        }
        if (chance(5)) { fmt += "%%"; input += chance(80) ? "%" : "?"; continue; }
        fmt += '%';
        const bool suppress = chance(15);
        if (suppress) fmt += '*';
        int width = 0;
        if (chance(30)) { width = chance(10) ? 0 : ri(1, 40); fmt += std::to_string(width); }
        static const char* sizes[] = {"", "", "", "h", "l", "L", "I64", "w", "F", "N", "I", "hl"};
        const char* sz = sizes[rnd() % 12];
        fmt += sz;
        static const char types[] = "cCdiouxXpeEfgGsSn[[%y";
        const char type = types[rnd() % (sizeof types - 1)];
        if (type == '[') {
            std::string set;
            if (chance(30)) set += '^';
            if (chance(10)) set += ']';
            const int n = ri(0, 6);
            for (int i = 0; i < n; i++) {
                if (chance(30)) {
                    set += (char)ri(0x21, 0x7e);
                    set += '-';
                    set += (char)ri(0x21, 0xfe);              // (never 0xff: such a range loops forever, as the original)
                } else {
                    char c = (char)ri(0x20, 0xfe);
                    if (c == ']') c = 'q';
                    set += c;
                }
            }
            fmt += '[';
            fmt += set;
            // a ']' first (after any '^') is a member: one more closes the set (else the scan runs past the format's end
            // looking for one -- the original's, too)
            if (set.empty() || set == "^") fmt += ']';
            fmt += ']';
            input += rand_text(20, "abcdefqrsxyz0123 -]^");
        } else {
            fmt += type;
            switch (type) {
            case 'c': case 'C': input += rand_text(width ? width + 2 : 3); break;
            case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'p': input += rand_number_text(false); break;
            case 'e': case 'E': case 'f': case 'g': case 'G': input += rand_number_text(true); break;
            case 's': case 'S': input += rand_text(30); break;
            case 'n': break;
            default: input += rand_text(3); break;
            }
        }
        if (chance(50)) input += rand_text(2, " \t\n");
    }
    if (chance(10)) fmt += rand_text(3, " \t");
    // The engine's last-read character starts as stack garbage: a format whose first directive reads nothing (empty, or
    // %n first) would end on it. Such formats get a literal first.
    {
        size_t i = 0;
        while (i < fmt.size() && strchr(" \t\n", fmt[i])) i++;
        bool reads = i < fmt.size();
        if (reads && fmt[i] == '%') {
            size_t j = i + 1;
            while (j < fmt.size() && strchr("*0123456789hlLIwFN", fmt[j])) j++;
            if (j >= fmt.size() || fmt[j] == 'n') reads = false;
        }
        if (!reads) {
            fmt = "k" + fmt;
            input = "k" + input;
        }
    }
    // every argument a pointer to its own slot (whatever a conversion takes it as)
    const int nptr = (int)std::count(fmt.begin(), fmt.end(), '%') + 2;
    for (int i = 0; i < nptr; i++) {
        uint8_t* slot = aalloc(0x400, 8);
        memset(slot, 0xc3, 0x400);
        args.push_back(P(slot));
    }
    char* pf = astr(fmt);
    char* pi = astr(input);
    g_desc = "format \"" + fmt + "\" input \"" + input + "\"";
    for (char& c : g_desc) if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7e) c = '?';
    if (chance(70)) {
        family("sscanf");
        g_nargs = 0;
        g_args[g_nargs++] = P(pi);
        g_args[g_nargs++] = P(pf);
        for (u32 a : args) if (g_nargs < 96) g_args[g_nargs++] = a;
        g_nst_in = 0;
        check(0x004ce190, RET_EAX);
    } else {
        family("input");
        CrtFile* s = (CrtFile*)aalloc(sizeof(CrtFile));
        s->flag = CRT_IOREAD | CRT_IOSTRG | CRT_IOMYBUF;
        s->ptr = s->base = pi;
        s->cnt = (int32_t)strlen(pi);
        u32* va = (u32*)aalloc((u32)args.size() * 4 + 16);
        for (size_t i = 0; i < args.size(); i++) va[i] = args[i];
        set_args({P(s), P(pf), P(va)});
        check(0x004d0ee0, RET_EAX);
    }
}

// =====================================================================================================================
// string -> number
// =====================================================================================================================
static void test_strtonum() {
    arena_reset();
    const std::string t = rand_number_text(chance(50));
    char* s = astr(t);
    g_desc = "\"" + t + "\"";
    for (char& c : g_desc) if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7e) c = '?';
    const char** endp = (const char**)aalloc(8);
    *endp = (const char*)(uintptr_t)0x11111111;
    switch (rnd() % 11) {
    case 0: {
        family("strtoxl");
        static const int bases[] = {0, 2, 8, 10, 16, 36, 1, 37, -1, 3, 7, 25};
        set_args({P(s), chance(80) ? P(endp) : 0, (u32)bases[rnd() % 12], (u32)(rnd() & 3)});
        check(0x004cff50, RET_EAX);
        break;
    }
    case 1: {
        family("strtoul");
        static const int bases[] = {0, 2, 8, 10, 16, 36, 1, 37, -1};
        set_args({P(s), chance(80) ? P(endp) : 0, (u32)bases[rnd() % 9]});
        check(0x004d01a0, RET_EAX);
        break;
    }
    case 2: family("atol"); set_args({P(s)}); check(0x004cf8e0, RET_EAX); break;
    case 3: family("atoi"); set_args({P(s)}); check(0x004cf990, RET_EAX); break;
    case 4: family("atof"); set_args({P(s)}); check(0x004cfe40, RET_ST0); break;
    case 5: family("fltin"); set_args({P(s), (u32)t.size(), 0, 0}); check(0x004d4d40, RET_EAX); break;
    case 6: {
        family("strgtold12");
        uint8_t* ld = aalloc(12);
        memset(ld, 0xee, 12);
        set_args({P(ld), P(endp), P(s), (u32)(rnd() & 1), (u32)ri(-30, 30) * (chance(50) ? 1u : 0u),
                  (u32)ri(-30, 30) * (chance(50) ? 1u : 0u), (u32)(rnd() & 1)});
        check(0x004d6a20, RET_EAX);
        break;
    }
    case 7: {
        family("atodbl");
        uint8_t* d = aalloc(8);
        memset(d, 0xee, 8);
        set_args({P(d), P(s)});
        check(0x004d60c0, RET_EAX);
        break;
    }
    case 8: {
        family("atoflt");
        uint8_t* d = aalloc(8);
        memset(d, 0xee, 8);
        set_args({P(d), P(s)});
        check(0x004d6100, RET_EAX);
        break;
    }
    case 9: {
        family("fassign");
        uint8_t* d = aalloc(8);
        memset(d, 0xee, 8);
        set_args({(u32)(rnd() & 1) * (chance(50) ? 1u : 7u), P(d), P(s)});
        check(0x004d0aa0, RET_NONE);
        break;
    }
    default: {
        family("hextodec");
        set_args({(u32)(chance(80) ? (int)"0123456789abcdefABCDEF"[rnd() % 22] : ri(-1, 300))});
        check(0x004d1b80, RET_EAX);
        break;
    }
    }
}

// =====================================================================================================================
// the float formatting pieces
// =====================================================================================================================
static CrtStrflt* make_strflt() {
    // a STRFLT as _fltout makes them: sign, decpt, a digit string
    CrtStrflt* p = (CrtStrflt*)aalloc(sizeof(CrtStrflt));
    p->sign = chance(50) ? '-' : ' ';
    p->decpt = ri(-320, 320);
    p->flag = 1;
    std::string m;
    const int n = ri(0, 21);
    for (int i = 0; i < n; i++) m += (char)('0' + ri(0, 9));
    if (chance(20)) m = std::string((size_t)ri(1, 21), '9');
    p->mantissa = astr(m);
    return p;
}
static void test_cvt() {
    arena_reset();
    const u64 d = rand_double();
    char b[64];
    sprintf(b, "double %016llx", (unsigned long long)d);
    g_desc = b;
    uint8_t* pd = aalloc(8);
    memcpy(pd, &d, 8);
    char* buf = (char*)aalloc(0x400, 4);
    memset(buf, 0x77, 0x400);
    const int ndec = chance(70) ? ri(0, 20) : ri(-3, 150);
    switch (rnd() % 14) {
    case 0: family("fltout"); set_args({(u32)d, (u32)(d >> 32)}); check(0x004d61d0, RET_EAX); break;
    case 1: {
        family("dtold");
        uint8_t* ld = aalloc(12);
        memset(ld, 0xee, 12);
        set_args({P(ld), P(pd)});
        check(0x004d6240, RET_NONE);
        break;
    }
    case 2: {
        family("I10_OUTPUT");
        uint8_t ext[10];
        rand_ext(ext);
        u32 w[3] = {0, 0, 0};
        memcpy(w, ext, 10);
        CrtFos* fos = (CrtFos*)aalloc(sizeof(CrtFos) + 8);
        memset(fos, 0x99, sizeof(CrtFos) + 8);
        set_args({w[0], w[1], w[2], (u32)(chance(80) ? ri(0, 25) : ri(-5, 60)), (u32)(rnd() & 3), P(fos)});
        char bb[96];
        sprintf(bb, "ld %08x %08x %04x", w[0], w[1], w[2] & 0xffff);
        g_desc = bb;
        check(0x004d7b40, RET_EAX);
        break;
    }
    case 3: {
        family("fptostr");
        CrtStrflt* p = make_strflt();
        set_args({P(buf), (u32)(chance(80) ? ri(0, 30) : ri(-5, 300)), P(p)});
        check(0x004d6140, RET_NONE);
        break;
    }
    case 4: family("cftoe"); set_args({P(pd), P(buf), (u32)ndec, (u32)(rnd() & 1)}); check(0x004d0af0, RET_EAX); break;
    case 5: family("cftof"); set_args({P(pd), P(buf), (u32)ndec}); check(0x004d0c30, RET_EAX); break;
    case 6: family("cftog"); set_args({P(pd), P(buf), (u32)(ndec > 0 ? ndec : 1), (u32)(rnd() & 1)}); check(0x004d0d30, RET_EAX); break;
    case 7: {
        family("cfltcvt");
        static const char fmts[] = "eEfgGxa";
        set_args({P(pd), P(buf), (u32)fmts[rnd() % 7], (u32)(ndec > 0 ? ndec : 1), (u32)(rnd() & 1)});
        check(0x004d0e40, RET_EAX);
        break;
    }
    case 8: {                                             // cropzeros / forcdecpt on what the converters produce
        char tmp[512];
        sprintf(tmp, (const char*)"%.*e", ri(0, 12), bitsd(d));
        std::string s = tmp;
        if (chance(50)) { sprintf(tmp, "%.*f", ri(0, 8), fmod(bitsd(d), 1e12)); s = tmp; }
        if (chance(20)) s = rand_text(20, "0123456789.eE+-");
        strcpy(buf, s.c_str());
        g_desc = "\"" + s + "\"";
        if (chance(50)) { family("cropzeros"); set_args({P(buf)}); check(0x004d0a20, RET_NONE); }
        else { family("forcdecpt"); set_args({P(buf)}); check(0x004d09b0, RET_NONE); }
        break;
    }
    case 9: family("positive"); set_args({P(pd)}); check(0x004d0a80, RET_EAX); break;
    case 10: {
        family("shift");
        strcpy(buf, rand_text(40).c_str());
        set_args({P(buf), (u32)ri(0, 20)});
        check(0x004d0eb0, RET_NONE);
        break;
    }
    case 11: {                                            // the %g pair with g_fmt set (as _cftog leaves the state)
        family("cftoe_g/cftof_g");
        CrtStrflt* p = make_strflt();
        CRT_G(CrtStrflt*, 0x005d55f8) = p;
        CRT_G(int32_t, 0x00502714) = chance(50) ? ndec : ri(-5, 20);
        strcpy(buf + 1, "123456789012345678901234");
        if (chance(50)) { set_args({P(pd), P(buf), (u32)ndec, (u32)(rnd() & 1)}); check(0x004d0de0, RET_EAX); }
        else { set_args({P(pd), P(buf), (u32)ndec}); check(0x004d0e10, RET_EAX); }
        break;
    }
    case 12: family("finite"); set_args({(u32)d, (u32)(d >> 32)}); check(0x004cf550, RET_EAX); break;
    default: family("isnan"); set_args({(u32)d, (u32)(d >> 32)}); check(0x004cf570, RET_EAX); break;
    }
}

// =====================================================================================================================
// the _LDBL12 helpers
// =====================================================================================================================
static void rand_ld12(uint8_t* p) {
    u64 a = rnd64();
    u32 b = rnd();
    memcpy(p, &a, 8);
    memcpy(p + 8, &b, 4);
    switch (rnd() % 6) {
    case 0: { const uint16_t e = (uint16_t)(0x3fff + ri(-200, 200) | (rnd() & 0x8000)); memcpy(p + 10, &e, 2); p[9] |= 0x80; break; }
    case 1: { const uint16_t e = (uint16_t)(rnd() & 0x8000); memcpy(p + 10, &e, 2); break; }          // denormal / zero
    case 2: memset(p, 0, 10); break;
    case 3: { const uint16_t e = (uint16_t)(0x7fff | (rnd() & 0x8000)); memcpy(p + 10, &e, 2); break; }
    case 4: { const uint16_t e = (uint16_t)(0x3fff + ri(-17000, 17000)); memcpy(p + 10, &e, 2); p[9] |= 0x80; break; }
    default: break;
    }
}
static void test_ld12() {
    arena_reset();
    uint8_t* a = aalloc(16);
    uint8_t* b = aalloc(16);
    rand_ld12(a);
    rand_ld12(b);
    memset(a + 12, 0x31, 4);
    memset(b + 12, 0x32, 4);
    char bb[128];
    sprintf(bb, "a %016llx%08x b %016llx%08x", *(unsigned long long*)(a + 4), *(u32*)a, *(unsigned long long*)(b + 4), *(u32*)b);
    g_desc = bb;
    switch (rnd() % 17) {
    case 0: family("ld12mul"); set_args({P(a), P(b)}); check(0x004d7ee0, RET_NONE); break;
    case 1: {
        family("multtenpow12");
        const int pw = chance(70) ? ri(-400, 400) : ri(-6000, 6000);
        set_args({P(a), (u32)pw, (u32)(rnd() & 1)});
        check(0x004d8190, RET_NONE);
        break;
    }
    case 2: family("ld12tod"); set_args({P(a), P(b)}); check(0x004d6080, RET_EAX); break;
    case 3: family("ld12tof"); set_args({P(a), P(b)}); check(0x004d60a0, RET_EAX); break;
    case 4: family("ld12cvt"); set_args({P(a), P(b), chance(50) ? 0x005036b8u : 0x005036d0u}); check(0x004d5eb0, RET_EAX); break;
    case 5: family("add_12"); set_args({P(a), P(b)}); check(0x004d7960, RET_NONE); break;
    case 6: family("shl_12"); set_args({P(a)}); check(0x004d79d0, RET_NONE); break;
    case 7: family("shr_12"); set_args({P(a)}); check(0x004d7a10, RET_NONE); break;
    case 8: family("addl"); set_args({rnd(), chance(50) ? rnd() : (u32)ri(0, 3), P(b)}); check(0x004d7930, RET_EAX); break;
    case 9: {
        family("mtold12");
        const int n = ri(1, 25);
        char* m = (char*)aalloc(32, 1);
        for (int i = 0; i < n; i++) m[i] = (char)ri(0, 9);
        m[ri(0, n - 1)] = (char)ri(1, 9);
        if (chance(10)) m[0] = 10;                         // (strgtold's rounding can make a "10" digit)
        set_args({P(m), (u32)n, P(a)});
        check(0x004d7a50, RET_NONE);
        break;
    }
    case 10: family("ZeroTail"); set_args({P(a), (u32)ri(0, 95)}); check(0x004d5c20, RET_EAX); break;
    case 11: family("IncMan"); set_args({P(a), (u32)ri(0, 95)}); check(0x004d5c90, RET_EAX); break;
    case 12: family("RoundMan"); set_args({P(a), (u32)ri(1, 64)}); check(0x004d5d00, RET_EAX); break;
    case 13: family("ShrMan"); set_args({P(a), (u32)ri(0, 95)}); check(0x004d5e00, RET_NONE); break;
    case 14: family("CopyMan"); set_args({P(a), P(b)}); check(0x004d5db0, RET_NONE); break;
    case 15: family("FillZeroMan"); set_args({P(a)}); check(0x004d5dd0, RET_NONE); break;
    default: {
        family("IsZeroMan");
        if (chance(50)) memset(a, 0, 12);
        set_args({P(a)});
        check(0x004d5de0, RET_EAX);
        break;
    }
    }
}

// =====================================================================================================================
// integers and the FPU
// =====================================================================================================================
static u64 rand_u64_edge() {
    switch (rnd() % 6) {
    case 0: return rnd64();
    case 1: return rnd();
    case 2: return (u64)(int64_t)(int32_t)rnd();
    case 3: return rnd64() >> ri(0, 63);
    case 4: { static const u64 e[] = {0, 1, 2, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull,
                                      0xffffffffull, 0x100000000ull, 10, 16, 8}; return e[rnd() % 11]; }
    default: return (u64)ri(-1000, 1000);
    }
}
static void test_int() {
    arena_reset();
    u64 a = rand_u64_edge(), b = rand_u64_edge();
    char bb[96];
    switch (rnd() % 5) {
    case 0: family("allmul"); set_args({(u32)a, (u32)(a >> 32), (u32)b, (u32)(b >> 32)}); check(0x004d6300, RET_EDX_EAX); break;
    case 1:
        if (!b) b = 3;
        family("alldiv");
        set_args({(u32)a, (u32)(a >> 32), (u32)b, (u32)(b >> 32)});
        check(0x004cfea0, RET_EDX_EAX);
        break;
    case 2:
        if (!b) b = 7;
        family("aulldiv");
        set_args({(u32)a, (u32)(a >> 32), (u32)b, (u32)(b >> 32)});
        check(0x004d6750, RET_EDX_EAX);
        break;
    case 3:
        if (!b) b = 11;
        family("aullrem");
        set_args({(u32)a, (u32)(a >> 32), (u32)b, (u32)(b >> 32)});
        check(0x004d67c0, RET_EDX_EAX);
        break;
    default: {
        family("ftol");
        set_args({});
        g_nst_in = 1;
        rand_ext(g_st_in[0]);
        sprintf(bb, "x %016llx %04x", *(unsigned long long*)g_st_in[0], *(uint16_t*)(g_st_in[0] + 8));
        g_desc = bb;
        check(0x004cf108, RET_EDX_EAX);
        break;
    }
    }
}

static void test_fpu() {
    arena_reset();
    const u32 abstr = chance(50) ? rnd() : (rnd() & 0x000f031f) | (rnd() & 0x80000);
    const u32 mask = chance(50) ? rnd() : 0xffffffffu;
    g_desc.clear();
    switch (rnd() % 12) {
    case 0: family("control87"); set_args({abstr, mask}); check(0x004cfa00, RET_EAX); break;
    case 1: family("controlfp"); set_args({abstr, mask}); check(0x004cfa40, RET_EAX); break;
    case 2: family("clearfp"); set_args({}); g_nst_in = 0; check(0x004cf9e0, RET_EAX); break;
    case 3: family("setdefaultprecision"); set_args({}); check(0x004d0820, RET_NONE); break;
    case 4: family("statfp"); set_args({}); check(0x004d2f70, RET_EAX); break;
    case 5: family("clrfp"); set_args({}); check(0x004d2f90, RET_EAX); break;
    case 6: family("ctrlfp"); set_args({rnd() & 0xffff, rnd() & 0xffff}); check(0x004d2fb0, RET_EAX); break;
    case 7: family("set_statfp"); set_args({rnd() & 0x3f}); check(0x004d2ff0, RET_NONE); break;
    case 8: family("abstract_cw"); set_args({rnd()}); check(0x004cfa60, RET_EAX); break;
    case 9: family("hw_cw"); set_args({abstr}); check(0x004cfb10, RET_AX); break;
    case 10: family("abstract_sw"); set_args({rnd()}); check(0x004cfba0, RET_EAX); break;
    default: {
        const u64 d = rand_double();
        if (chance(50)) {
            family("set_exp");
            set_args({(u32)d, (u32)(d >> 32), (u32)ri(-1100, 1100)});
            check(0x004d3050, RET_ST0);
        } else {
            family("decomp");
            u32* pe = (u32*)aalloc(4);
            *pe = 0xdddddddd;
            set_args({(u32)d, (u32)(d >> 32), P(pe)});
            check(0x004d3090, RET_ST0);
        }
        break;
    }
    }
}

// the transcendentals: the C forms (doubles on the stack) and the _CI forms (the FPU stack), through the dispatchers
static void test_tran() {
    arena_reset();
    const u64 x = rand_double(), y = rand_double();
    char bb[96];
    sprintf(bb, "x %016llx y %016llx", (unsigned long long)x, (unsigned long long)y);
    g_desc = bb;
    switch (rnd() % 10) {
    case 0: family("acos"); set_args({(u32)x, (u32)(x >> 32)}); check(0x004cf05a, RET_ST0); break;
    case 1: family("asin"); set_args({(u32)x, (u32)(x >> 32)}); check(0x004cf050, RET_ST0); break;
    case 2: family("atan"); set_args({(u32)x, (u32)(x >> 32)}); check(0x004cf061, RET_ST0); break;
    case 3: family("atan2"); set_args({(u32)x, (u32)(x >> 32), (u32)y, (u32)(y >> 32)}); check(0x004cf068, RET_ST0); break;
    case 4: family("fmod"); set_args({(u32)x, (u32)(x >> 32), (u32)y, (u32)(y >> 32)}); check(0x004cf360, RET_ST0); break;
    default: {
        set_args({});
        rand_ext(g_st_in[0]);
        rand_ext(g_st_in[1]);
        static const struct { const char* n; u32 at; int ops; } ci[] = {
            {"CIacos", 0x004cf07c, 1}, {"CIasin", 0x004cf072, 1}, {"CIatan", 0x004cf083, 1},
            {"CIatan2", 0x004cf08a, 2}, {"CIfmod", 0x004cf36a, 2}};
        const int k = (int)(rnd() % 5);
        family(ci[k].n);
        g_nst_in = ci[k].ops;
        sprintf(bb, "st0 %016llx %04x st1 %016llx %04x", *(unsigned long long*)g_st_in[0], *(uint16_t*)(g_st_in[0] + 8),
                *(unsigned long long*)g_st_in[1], *(uint16_t*)(g_st_in[1] + 8));
        g_desc = bb;
        check(ci[k].at, RET_ST0);
        break;
    }
    }
}

// ---- main ------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_CRTW_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int rounds = argc > 1 ? atoi(argv[1]) : 100;
    if (argc > 2) g_rng = (u32)strtoul(argv[2], 0, 0) | 1;
    const char* filter = argc > 3 ? argv[3] : 0;
    g_trace = getenv("VP_CRT_TRACE") != 0;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_crt_fmt.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    g_arena = (uint8_t*)VirtualAlloc((void*)0x20000000, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_arena_save = (uint8_t*)malloc(ARENA_SIZE);
    g_arena_orig = (uint8_t*)malloc(ARENA_SIZE);
    if (!g_arena) { printf("no arena\n"); return 2; }
    ((void(__cdecl*)())0x004ce150)();                     // _cfltcvt_init: the %e/%f/%g converters into _cfltcvt_tab
    int n = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) n++;
    printf("world_crt_fmt: %d rewrites registered; %d rounds, seed %08x\n", n, rounds, g_rng);
    if (const char* one = getenv("VP_CRT_ONE_PRINTF")) {      // one format, chaos arguments, both ways
        family("one");
        g_trace = true;
        for (int pass = 0; pass < 2; pass++) {
            arena_reset();
            char* fmt = astr(one);
            uint8_t* out = aalloc(0x4000);
            memset(out, 0x5a, 0x4000);
            g_nargs = 0;
            g_args[g_nargs++] = P(out);
            g_args[g_nargs++] = P(fmt);
            for (int i = 0; i < 64; i++) g_args[g_nargs++] = (u32)(i % 41);
            g_nst_in = 0;
            g_cw_in = 0x37f;
            g_st_ret = 0;
            g_fn = pass ? (u32)(uintptr_t)rewrite_of(0x004cf0a0) : 0x004cf0a0;
            const u32 fault = run_guarded();
            printf("%s: fault %08x eax %d \"%.200s\"\n", pass ? "rewrite " : "original", fault, g_eax, (char*)out);
        }
        return 0;
    }
    struct Fam { const char* name; void (*fn)(); int weight; } fams[] = {
        {"printf", test_printf, 30}, {"scanf", test_sscanf, 20}, {"strtonum", test_strtonum, 15},
        {"cvt", test_cvt, 15}, {"ld12", test_ld12, 10}, {"int", test_int, 5}, {"fpu", test_fpu, 3}, {"tran", test_tran, 4},
    };
    int total_w = 0;
    for (const Fam& f : fams) total_w += f.weight;
    for (int r = 0; r < rounds; r++) {
        for (int k = 0; k < 1000; k++) {
            int w = (int)(rnd() % (u32)total_w);
            const Fam* f = fams;
            while (w >= f->weight) { w -= f->weight; f++; }
            if (filter && !strstr(f->name, filter)) continue;
            f->fn();
            g_desc.clear();
        }
        if (getenv("VP_CRT_VERBOSE") && (r + 1) % 10 == 0) printf("  round %d: %ld mismatches so far\n", r + 1, g_total_fail);
    }
    long checks = 0;
    for (const Stats& st : g_stats) {
        checks += st.checks;
        printf("  %-20s %9ld checks (%ld chained)  %s\n", st.name, st.checks, st.chain,
               st.failed ? "DIFFERS" : "identical to the original");
        if (st.failed) printf("  %-20s %9ld mismatches\n", "", st.failed);
    }
    printf("%ld checks, %ld mismatches\n", checks, g_total_fail);
    return g_total_fail ? 1 : 0;
}
