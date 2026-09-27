// world_gx_dx.cpp -- group G1d (hook/gx_dx.cpp: vid.obj, dx.obj, dxstate.obj, dd.obj) against the originals, outside
// the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_gx_dx.cpp
//        /Fo%TEMP%\g1dw\ /Fe%TEMP%\g1dw\world_gx_dx.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_gx_dx.exe [rounds] [seed] [isolated|chain|both]      (from the repository root: out\race_v10.exe)
//
// Loads out\race_v10.exe at 0x400000 in a child process (the range reserved before its heap exists), resolves
// KERNEL32's imports into the game's own import slots, and points the DDRAW ones (DirectDrawCreate,
// DirectDrawEnumerateA) and ExitProcess's slot at stubs.
//
// DirectX is a set of RECORDING FAKES: DirectDraw (the v1 object DirectDrawCreate hands out and the IDirectDraw2 it
// is queried for), surfaces, Direct3D, the device, viewports, materials, textures -- objects in the arena with
// vtables of stubs. Every method called logs itself (object, slot, every argument; the structures passed in by
// content: surface descriptions, the viewport, the material, the clear rectangle, the matrix, the colour key,
// vertices and indices), then answers from a random stream kept in the arena (so both passes get the same
// answers): a result code (DD_OK, or with the round's failure rate an error -- lost surface, still drawing, out of
// video memory, not found, unknown codes...), and out-parameters (new objects, descriptions, caps, memory totals,
// handles, a Lock's pointer and pitch, TransformVertices' vertices and offscreen flag). The enumerations call their
// callback -- the game's (hardware_callback, mode_callback) or the harness's -- with random drivers / modes /
// formats. A slot the game shouldn't call raises. So the call log compares every DirectX call, in order, with its
// arguments.
//
// Everything else the functions call outside the group is a stub that logs: LogReport (its arguments read by its
// format), LogPanic and ExitProcess (they raise, ending the call in both passes), MemAlloc (a bump heap in the
// arena with the 0xa3 fill; now and then a null) and operator delete, Win32Idle, PTimeNow (a clock stepped from
// the stream), Win32GetWindow, gxBuildCanvas (it fills the canvas like the real one), TextureFlush, TextureSelect
// and TextureHasAlpha (answers from the stream). sprintf and strstr are the game's own CRT.
//
// Each check: the arena (the fakes, the heap, the world's wrappers and buffers; write-watched) and the image
// statics are kept, the ORIGINAL runs, the state is put back, the REWRITE runs, and the return value, the log, the
// state and any fault / panic / exit are compared; every byte the original changed in the game's memory (the
// statics, the heap, the world -- not the fakes' own state) must lie inside the rewrite's footprint unless it is
// replay_only. Then the original's result is kept and the round carries on. "isolated": the rewrite alone (its
// callees the originals); "chain": every rewrite of the group patched into the image for the rewrite's pass.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <type_traits>
#define VP_FAITHFUL
#include "../hook/port.h"

// ---- the registry: every PORT_FN, with a uniform caller (every argument is one stack dword) ----------------------------
struct Reg {
    uint32_t at;
    const char* name;
    void* fn;
    uint64_t (*run)(int rewrite, uint32_t at, const uint32_t* a, Footprint* f);
    int nargs;
    Reg* next;
    static Reg*& head() { static Reg* h; return h; }
    Reg(uint32_t t, const char* n, void* f, uint64_t (*r)(int, uint32_t, const uint32_t*, Footprint*), int na)
        : at(t), name(n), fn(f), run(r), nargs(na), next(head()) { head() = this; }
};
template <typename T> static T cv(uint32_t v) {
    if constexpr (std::is_pointer_v<T>) return (T)(uintptr_t)v;
    else return (T)v;
}
template <typename F> struct Inv;
#define INV_CC(CC)                                                                                                     \
    template <typename R, typename... Args> struct Inv<R(CC*)(Args...)> {                                             \
        typedef R(CC* Fn)(Args...);                                                                                    \
        enum { N = sizeof...(Args) };                                                                                  \
        template <Fn NEW, void (*FP)(Footprint&, Args...), size_t... I>                                                \
        static uint64_t go(int rw, uint32_t at, const uint32_t* a, Footprint* f, std::index_sequence<I...>) {          \
            if (f) { FP(*f, cv<Args>(a[I])...); return 0; }                                                            \
            Fn fn = rw ? NEW : (Fn)(uintptr_t)at;                                                                      \
            if constexpr (std::is_void_v<R>) { fn(cv<Args>(a[I])...); return 0; }                                     \
            else { R r = fn(cv<Args>(a[I])...); uint64_t u = 0; memcpy(&u, &r, sizeof r); return u; }                  \
        }                                                                                                              \
        template <Fn NEW, void (*FP)(Footprint&, Args...)>                                                             \
        static uint64_t run(int rw, uint32_t at, const uint32_t* a, Footprint* f) {                                    \
            return go<NEW, FP>(rw, at, a, f, std::index_sequence_for<Args...>{});                                      \
        }                                                                                                              \
    };
INV_CC(__cdecl)
INV_CC(__fastcall)
INV_CC(__stdcall)
#undef INV_CC
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                               \
    static Reg VP_CAT(reg_, NEW)(V10, NAME, (void*)&NEW, &Inv<decltype(&NEW)>::run<&NEW, &FP>, Inv<decltype(&NEW)>::N);

void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }

#include "../hook/gx_dx.cpp"

namespace hx {
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
static uint32_t fnv(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}

// ---- the original, loaded at 0x400000 -------------------------------------------------------------------------------
struct IatSlot { std::string dll, name; uint32_t at; };
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
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.Size && d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        bool k32 = !_stricmp(dll, "KERNEL32.dll");
        HMODULE m = GetModuleHandleA("kernel32.dll");
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            g_iat.push_back({dll, fn, (uint32_t)(uintptr_t)&iat->u1.Function});
            if (!k32) continue;
            FARPROC p = GetProcAddress(m, fn);
            if (p) iat->u1.Function = (DWORD)(uintptr_t)p;
        }
    }
    free(file);
    return true;
}
static int relaunch() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
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
    PG = 4096, ARENA_SIZE = 24u << 20, NPAGES = ARENA_SIZE / PG,
    SCRATCH_OFF = 0x1000, SCRATCH_SIZE = 0x7000,           // the fakes' scratch (enumerated descriptions, names)
    OBJ_OFF = 0x8000, MAX_OBJ = 0x2000, OBJ_SIZE = 32,     // the fake DirectX objects
    PIX_OFF = 0x50000, PIX_SIZE = 0x100000,                // a Lock's pixels
    GAME_OFF = 0x150000,                                   // the game's memory from here: the world, the heap
    WORLD_OFF = 0x150000, WORLD_SIZE = 0x200000,           // the world's wrappers and the checks' buffers
    HEAP_OFF = 0x350000,                                   // MemAlloc
};
static uint8_t* g_ar;
static uint8_t* g_sh;
static ULONG_PTR* g_ww;
static std::vector<uint32_t> ww_take() {
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
static void ww_sync() {
    for (uint32_t p : ww_take()) memcpy(g_sh + (size_t)p * PG, g_ar + (size_t)p * PG, PG);
}
static bool in_arena(const void* p, size_t n = 1) {
    return (uintptr_t)p >= (uintptr_t)g_ar && (uintptr_t)p + n <= (uintptr_t)g_ar + ARENA_SIZE;
}
static uint32_t aoff(const void* p) {                       // a pointer for the log: arena offset, image address, or "a local"
    uintptr_t a = (uintptr_t)p;
    if (!a) return 0;
    if (in_arena(p)) return 0xa0000000u | (uint32_t)(a - (uintptr_t)g_ar);
    if (a >= 0x400000 && a < 0x700000) return (uint32_t)a;
    return 0x5ac00000u;                                    // the stack: a local of the function (differs by pass)
}

// the stubs' state, in the arena (saved, restored and compared with everything else)
struct Ctrl {
    uint32_t fr;                 // the fakes' random stream
    uint32_t nobj;               // fake objects made
    uint32_t heap_top;           // MemAlloc's next offset
    int32_t fail_pct;            // DirectX failures (%)
    int32_t oom_pct;             // MemAlloc nulls (%)
    int32_t caps_valid;          // the device's GetCaps: a valid description
    int32_t clock;               // PTimeNow
    uint32_t scratch_top;
};
static Ctrl* C() { return (Ctrl*)g_ar; }
static uint32_t frnd() {
    uint32_t x = C()->fr;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    C()->fr = x;
    return x;
}
static void* scratch(uint32_t n) {
    n = (n + 7) & ~7u;
    if (C()->scratch_top + n > SCRATCH_SIZE) C()->scratch_top = 0;
    void* p = g_ar + SCRATCH_OFF + C()->scratch_top;
    C()->scratch_top += n;
    return p;
}

// ---- the log ----------------------------------------------------------------------------------------------------------
static std::vector<uint32_t> g_log[2];
static int g_pass;
enum { LOG_CAP = 1 << 20 };
static void lg(uint32_t v) {
    if (g_log[g_pass].size() < LOG_CAP) g_log[g_pass].push_back(v);
}
static uint32_t safe_hash(const void* p, uint32_t n) {
    if (!p) return 0xdeadbeef;
    __try {
        return fnv(p, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xbadbad; }
}
static uint32_t safe_hash_str(const char* s, int max = 512) {
    if (!s) return 0xdeadbeef;
    uint32_t h = 2166136261u;
    __try {
        for (int i = 0; i < max && s[i]; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    } __except (EXCEPTION_EXECUTE_HANDLER) { h ^= 0xbadbad; }
    return h;
}
static void lg_fmt(const char* fmt, va_list ap) {
    lg(aoff(fmt));
    lg(safe_hash_str(fmt));
    __try {
        for (const char* p = fmt; *p; p++) {
            if (*p != '%') continue;
            p++;
            while (*p && strchr("-+ #0123456789.lh", *p)) p++;
            if (!*p) break;
            if (*p == '%') continue;
            if (*p == 's') lg(safe_hash_str(va_arg(ap, const char*)));
            else if (*p == 'f' || *p == 'g' || *p == 'e') {
                double d = va_arg(ap, double);
                uint32_t u[2];
                memcpy(u, &d, 8);
                lg(u[0]);
                lg(u[1]);
            } else lg(va_arg(ap, uint32_t));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { lg(0xbadf0); }
}
enum : uint32_t {
    K_REPORT = 0x4b000001, K_PANIC, K_ALLOC, K_FREE, K_IDLE, K_TIME, K_WINDOW, K_CANVAS, K_TFLUSH, K_TSELECT, K_TALPHA,
    K_EXIT, K_DDCREATE, K_DDENUM, K_DDENUM_CB, K_MODES_CB, K_TEXFMT_CB, K_CB_RET, K_BADSLOT,
    K_METHOD = 0x4d000000,       // | kind << 8 | slot
};
static const DWORD PANIC_CODE = 0xE0564B01, EXIT_CODE = 0xE0564B03, BADSLOT_CODE = 0xE0564B04;

// ---- the fake DirectX objects ------------------------------------------------------------------------------------------
enum Kind : uint32_t { KD1 = 1, KD2, KS, KD3, KDEV, KVP, KMAT, KTEX, NKIND };
struct FakeObj { void** vtbl; uint32_t kind, id; int32_t refs; uint32_t spare[4]; };
static_assert(sizeof(FakeObj) == OBJ_SIZE, "FakeObj");
static void* g_vt[NKIND][64];
static FakeObj* new_obj(uint32_t kind) {
    uint32_t id = C()->nobj++ % MAX_OBJ;
    FakeObj* o = (FakeObj*)(g_ar + OBJ_OFF + id * OBJ_SIZE);
    o->vtbl = g_vt[kind];
    o->kind = kind;
    o->id = id;
    o->refs = 1;
    return o;
}
static bool is_obj(const void* p) {
    return (uintptr_t)p >= (uintptr_t)(g_ar + OBJ_OFF) && (uintptr_t)p < (uintptr_t)(g_ar + OBJ_OFF + MAX_OBJ * OBJ_SIZE);
}
static uint32_t oid(const void* p) { return is_obj(p) ? 0x0b000000u | ((const FakeObj*)p)->id : aoff(p); }
static void lm(FakeObj* o, uint32_t slot) {
    lg(K_METHOD | o->kind << 8 | slot);
    lg(oid(o));
}
static int32_t pick_hr() {
    if ((int32_t)(frnd() % 100) >= C()->fail_pct) return 0;
    static const uint32_t errs[] = {0x80004005, 0x887601c2, 0x8876021c, 0x8876021c, 0x8876017c, 0x887600ff, 0x80004002,
                                    0x80070057, 0x88760005, 0x887602bc, 0x12345678, 0x88760307, 0x88760121};
    return (int32_t)errs[frnd() % (sizeof errs / sizeof errs[0])];
}
static void fill_rand(void* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) ((uint8_t*)p)[i] = (uint8_t)frnd();
}

static void __stdcall f_bad(FakeObj* o) {
    lg(K_BADSLOT);
    lg(oid(o));
    RaiseException(BADSLOT_CODE, 0, 0, 0);
}
static uint32_t iid_kind(const uint32_t* iid) {
    switch (iid[0]) {
    case 0xb3a6f3e0: return KD2;                           // IID_IDirectDraw2
    case 0x6aae1ec1: return KD3;                           // IID_IDirect3D2
    case 0x6c14db81: return KS;                            // IID_IDirectDrawSurface
    case 0xda044e00: return KS;                            // IID_IDirectDrawSurface3
    case 0x93281502: return KTEX;                          // IID_IDirect3DTexture2
    }
    return 0;
}
static int32_t __stdcall f_qi(FakeObj* o, const uint32_t* iid, void** out) {
    lm(o, 0);
    lg(safe_hash(iid, 16));
    lg(aoff(out));
    int32_t hr = pick_hr();
    uint32_t k = iid_kind(iid);
    if (!hr && !k) hr = (int32_t)0x80004002;
    if (hr) {
        *out = 0;
        return hr;
    }
    *out = new_obj(k);
    return 0;
}
static uint32_t __stdcall f_addref(FakeObj* o) {
    lm(o, 1);
    return ++o->refs;
}
static uint32_t __stdcall f_release(FakeObj* o) {
    lm(o, 2);
    return --o->refs;
}
// DirectDraw 2
static int32_t __stdcall f_create_surface(FakeObj* o, const uint32_t* desc, void** out, void* unk) {
    lm(o, 6);
    lg(safe_hash(desc, 0x6c));
    lg(aoff(out));
    lg(aoff(unk));
    int32_t hr = pick_hr();
    if (hr) return hr;
    *out = new_obj(KS);
    return 0;
}
static int32_t __stdcall f_enum_modes(FakeObj* o, uint32_t flags, const uint32_t* desc, void* ctx, void* cb) {
    lm(o, 8);
    lg(flags);
    lg(safe_hash(desc, 0x6c));
    lg(aoff(ctx));
    lg(aoff(cb));
    int32_t hr = pick_hr();
    if (hr) return hr;
    static const uint32_t modes[][2] = {{320, 200}, {512, 384}, {640, 480}, {800, 600}, {1024, 768}, {1280, 1024}, {640, 400}};
    int n = (int)(frnd() % 9);
    for (int i = 0; i < n; i++) {
        uint32_t* d = (uint32_t*)scratch(0x6c);
        fill_rand(d, 0x6c);
        d[0] = 0x6c;
        uint32_t m = frnd() % 3 == 0 ? 2 : frnd() % 8;     // 640x480 often (else find_modes exits)
        d[2] = m < 7 ? modes[m][1] : frnd() % 2000;
        d[3] = m < 7 ? modes[m][0] : frnd() % 2000;
        int32_t r = ((int32_t(__stdcall*)(void*, void*))cb)(d, ctx);
        lg(K_CB_RET);
        lg((uint32_t)r);
        if (!r) break;
    }
    return 0;
}
static int32_t __stdcall f_dd_getcaps(FakeObj* o, uint32_t* hal, uint32_t* hel) {
    lm(o, 11);
    lg(aoff(hal));
    lg(hal ? safe_hash(hal, 0x16c) : 0);
    lg(aoff(hel));
    lg(hel ? safe_hash(hel, 0x16c) : 0);
    int32_t hr = pick_hr();
    if (hr) return hr;
    if (hal) {
        fill_rand(hal + 1, 0x16c - 4);
        if (frnd() % 4) hal[1] |= 1;                       // DDCAPS_3D, mostly
        else hal[1] &= ~1u;
    }
    if (hel) fill_rand(hel + 1, 0x16c - 4);
    return 0;
}
static int32_t __stdcall f_restore_mode(FakeObj* o) {
    lm(o, 19);
    return pick_hr();
}
static int32_t __stdcall f_set_coop(FakeObj* o, void* hwnd, uint32_t flags) {
    lm(o, 20);
    lg((uint32_t)(uintptr_t)hwnd);
    lg(flags);
    return pick_hr();
}
static int32_t __stdcall f_set_mode(FakeObj* o, uint32_t w, uint32_t h, uint32_t bpp, uint32_t rate, uint32_t fl) {
    lm(o, 21);
    lg(w);
    lg(h);
    lg(bpp);
    lg(rate);
    lg(fl);
    return pick_hr();
}
static int32_t __stdcall f_get_vidmem(FakeObj* o, const uint32_t* caps, uint32_t* total, uint32_t* free_) {
    lm(o, 23);
    lg(caps ? caps[0] : 0xdead);
    lg(aoff(total));
    lg(aoff(free_));
    static const uint32_t amounts[] = {0, 1000000, 1400000, 1900000, 2000000, 3000000, 3900000, 4000000, 5000000,
                                       7500000, 8000000, 12000000, 15800000, 16000000, 32000000};
    int32_t hr = pick_hr();
    // (written whatever the result: the game reads them regardless -- the original's locals would be garbage)
    *total = frnd() % 10 ? amounts[frnd() % 15] : frnd();
    *free_ = frnd() % 3 == 0 ? 0 : amounts[frnd() % 15];
    return hr;
}
// surfaces (IDirectDrawSurface and IDirectDrawSurface3 alike)
static int32_t __stdcall f_add_attached(FakeObj* o, FakeObj* s) {
    lm(o, 3);
    lg(oid(s));
    return pick_hr();
}
static int32_t __stdcall f_blt(FakeObj* o, RECT* dr, FakeObj* src, RECT* sr, uint32_t flags, void* fx) {
    lm(o, 5);
    lg(aoff(dr));
    lg(oid(src));
    lg(aoff(sr));
    lg(flags);
    lg(aoff(fx));
    return pick_hr();
}
static int32_t __stdcall f_bltfast(FakeObj* o, uint32_t x, uint32_t y, FakeObj* src, RECT* r, uint32_t flags) {
    lm(o, 7);
    lg(x);
    lg(y);
    lg(oid(src));
    lg(aoff(r));
    lg(r ? safe_hash(r, 16) : 0);
    lg(flags);
    return pick_hr();
}
static int32_t __stdcall f_flip(FakeObj* o, FakeObj* over, uint32_t flags) {
    lm(o, 11);
    lg(oid(over));
    lg(flags);
    return pick_hr();
}
static int32_t __stdcall f_get_attached(FakeObj* o, const uint32_t* caps, void** out) {
    lm(o, 12);
    lg(caps ? caps[0] : 0xdead);
    lg(aoff(out));
    int32_t hr = pick_hr();
    if (hr) return hr;
    *out = new_obj(KS);
    return 0;
}
static int32_t __stdcall f_flip_status(FakeObj* o, uint32_t flags) {
    lm(o, 18);
    lg(flags);
    return pick_hr();
}
static void fake_desc(uint32_t* d) {                        // a plausible surface description
    uint32_t size = d[0];
    fill_rand(d + 1, 0x6c - 4);
    d[0] = size;
    d[1] = (frnd() & 0x20) | (frnd() & 0x1000) | (frnd() & 0x7);
    d[5] = 1 + frnd() % 2;
    d[0x4c / 4] = frnd() & 0x40 ? 0x40 : frnd();
    d[0x54 / 4] = frnd() % 4 ? 16 : 32;
    uint32_t k = frnd() % 3;
    d[0x58 / 4] = k == 0 ? 0xf800 : k == 1 ? 0x7c00 : frnd();
    d[0x5c / 4] = k == 0 ? 0x7e0 : k == 1 ? 0x3e0 : frnd();
    d[0x60 / 4] = frnd() % 5 ? 0x1f : frnd();
    d[0x68 / 4] = frnd() & 0x4000 ? 0x4000 | (frnd() & 0x3ffff) : frnd() & 0x3bfff;
}
static int32_t __stdcall f_get_desc(FakeObj* o, uint32_t* d) {
    lm(o, 22);
    lg(aoff(d));
    lg(d ? safe_hash(d, 0x6c) : 0);
    int32_t hr = pick_hr();
    if (hr) return hr;
    fake_desc(d);
    return 0;
}
static int32_t __stdcall f_lock(FakeObj* o, RECT* r, uint32_t* d, uint32_t flags, HANDLE h) {
    lm(o, 25);
    lg(aoff(r));
    lg(aoff(d));
    lg(d ? safe_hash(d, 0x6c) : 0);
    lg(flags);
    lg((uint32_t)(uintptr_t)h);
    int32_t hr = pick_hr();
    if (hr) return hr;
    fake_desc(d);
    d[0x24 / 4] = frnd() % 8 ? (uint32_t)(uintptr_t)(g_ar + PIX_OFF + (frnd() % 0x800) * 16) : 0;   // lpSurface (now and then null)
    d[0x10 / 4] = 640 * 2 * (1 + frnd() % 2);            // lPitch
    return 0;
}
static int32_t __stdcall f_restore(FakeObj* o) {
    lm(o, 27);
    return pick_hr();
}
static int32_t __stdcall f_set_ckey(FakeObj* o, uint32_t flags, const uint32_t* key) {
    lm(o, 29);
    lg(flags);
    lg(key ? key[0] : 0xdead);
    lg(key ? key[1] : 0xdead);
    return pick_hr();
}
static int32_t __stdcall f_unlock(FakeObj* o, void* p) {
    lm(o, 32);
    lg(aoff(p));
    return pick_hr();
}
// Direct3D 2
static int32_t __stdcall f_create_material(FakeObj* o, void** out, void* unk) {
    lm(o, 5);
    lg(aoff(out));
    lg(aoff(unk));
    int32_t hr = pick_hr();
    if (hr) return hr;                                     // (out untouched: what the game does with it is the test)
    *out = new_obj(KMAT);
    return 0;
}
static int32_t __stdcall f_create_viewport(FakeObj* o, void** out, void* unk) {
    lm(o, 6);
    lg(aoff(out));
    lg(aoff(unk));
    int32_t hr = pick_hr();
    if (hr) return hr;
    *out = new_obj(KVP);
    return 0;
}
static int32_t __stdcall f_create_device(FakeObj* o, const uint32_t* iid, FakeObj* s, void** out) {
    lm(o, 8);
    lg(safe_hash(iid, 16));
    lg(oid(s));
    lg(aoff(out));
    int32_t hr = pick_hr();
    if (hr) return hr;
    *out = new_obj(KDEV);
    return 0;
}
// the device
static int32_t __stdcall f_dev_getcaps(FakeObj* o, uint32_t* hal, uint32_t* hel) {
    lm(o, 3);
    lg(aoff(hal));
    lg(hal ? safe_hash(hal, 0xcc) : 0);
    lg(aoff(hel));
    lg(hel ? safe_hash(hel, 0xcc) : 0);
    int32_t hr = pick_hr();
    if (hr) return hr;
    for (uint32_t* d : {hal, hel}) {
        if (!d) continue;
        fill_rand(d + 1, 0xcc - 4);
        if (C()->caps_valid || frnd() % 3) {
            d[1] |= 0x1c3;
            d[2] = 1 + frnd() % 3;
        }
    }
    return 0;
}
static int32_t __stdcall f_get_stats(FakeObj* o, uint32_t* st) {
    lm(o, 5);
    lg(aoff(st));
    lg(st ? st[0] : 0xdead);
    int32_t hr = pick_hr();
    if (hr) return hr;
    fill_rand(st + 1, 0x14);
    return 0;
}
static int32_t __stdcall f_add_viewport(FakeObj* o, FakeObj* v) {
    lm(o, 6);
    lg(oid(v));
    return pick_hr();
}
static int32_t __stdcall f_delete_viewport(FakeObj* o, FakeObj* v) {
    lm(o, 7);
    lg(oid(v));
    return pick_hr();
}
static int32_t __stdcall f_enum_tex(FakeObj* o, void* cb, void* ctx) {
    lm(o, 9);
    lg(aoff(cb));
    lg(aoff(ctx));
    int32_t hr = pick_hr();
    if (hr) return hr;
    int n = (int)(frnd() % 6);
    for (int i = 0; i < n; i++) {
        uint32_t* d = (uint32_t*)scratch(0x6c);
        fill_rand(d, 0x6c);
        d[0] = 0x6c;
        int32_t r = ((int32_t(__stdcall*)(void*, void*))cb)(d, ctx);
        lg(K_CB_RET);
        lg((uint32_t)r);
        if (!r) break;
    }
    return 0;
}
static int32_t __stdcall f_begin_scene(FakeObj* o) {
    lm(o, 10);
    return pick_hr();
}
static int32_t __stdcall f_end_scene(FakeObj* o) {
    lm(o, 11);
    return pick_hr();
}
static int32_t __stdcall f_set_cur_vp(FakeObj* o, FakeObj* v) {
    lm(o, 13);
    lg(oid(v));
    return pick_hr();
}
static int32_t __stdcall f_set_rs(FakeObj* o, uint32_t s, uint32_t v) {
    lm(o, 23);
    lg(s);
    lg(v);
    return pick_hr();
}
static int32_t __stdcall f_set_ls(FakeObj* o, uint32_t s, uint32_t v) {
    lm(o, 25);
    lg(s);
    lg(v);
    return pick_hr();
}
static int32_t __stdcall f_set_xf(FakeObj* o, uint32_t t, const float* m) {
    lm(o, 26);
    lg(t);
    lg(aoff(m));
    lg(m ? safe_hash(m, 0x40) : 0);
    return pick_hr();
}
static int32_t __stdcall f_dp(FakeObj* o, uint32_t pt, uint32_t vt, const void* v, uint32_t n, uint32_t fl) {
    lm(o, 29);
    lg(pt);
    lg(vt);
    lg(aoff(v));
    lg(n);
    lg(n < 0x10000 ? safe_hash(v, n * 32) : 0);
    lg(fl);
    return pick_hr();
}
static int32_t __stdcall f_dip(FakeObj* o, uint32_t pt, uint32_t vt, const void* v, uint32_t n, const uint16_t* idx, uint32_t ni,
                               uint32_t fl) {
    lm(o, 30);
    lg(pt);
    lg(vt);
    lg(aoff(v));
    lg(n);
    lg(n < 0x10000 ? safe_hash(v, n * 32) : 0);
    lg(aoff(idx));
    lg(ni);
    lg(ni < 0x10000 ? safe_hash(idx, ni * 2) : 0);
    lg(fl);
    return pick_hr();
}
// viewports
static int32_t __stdcall f_transform(FakeObj* o, uint32_t n, uint32_t* data, uint32_t flags, uint32_t* off) {
    lm(o, 6);
    lg(n);
    lg(aoff(data));
    lg(data ? safe_hash(data, 0x34) : 0);
    lg(data && data[1] ? safe_hash((void*)(uintptr_t)data[1], 0x20) : 0);   // the input vertex
    lg(flags);
    lg(aoff(off));
    int32_t hr = pick_hr();
    // (the offscreen flag written whatever the result: the original's local would be garbage)
    if (off) *off = frnd() % 3 == 0 ? 1 + frnd() % 0x3f : 0;
    if (hr) return hr;
    if (data && n == 1) {
        if (data[3]) fill_rand((void*)(uintptr_t)data[3], data[4] < 0x40 ? data[4] : 0x20);
        if (data[5]) fill_rand((void*)(uintptr_t)data[5], 0x10);
        fill_rand(data + 7, 0x34 - 0x1c);                 // clip intersection / union, extent
    }
    return 0;
}
static int32_t __stdcall f_set_bg(FakeObj* o, uint32_t h) {
    lm(o, 8);
    lg(h);
    return pick_hr();
}
static int32_t __stdcall f_clear(FakeObj* o, uint32_t n, const int32_t* rects, uint32_t flags) {
    lm(o, 12);
    lg(n);
    lg(rects ? safe_hash(rects, 16 * (n < 16 ? n : 16)) : 0);
    lg(flags);
    return pick_hr();
}
static int32_t __stdcall f_set_vp2(FakeObj* o, const uint32_t* vp) {
    lm(o, 17);
    lg(vp ? safe_hash(vp, 0x2c) : 0);
    return pick_hr();
}
// materials, textures
static int32_t __stdcall f_set_material(FakeObj* o, const uint32_t* m) {
    lm(o, 3);
    lg(aoff(m));
    lg(m ? safe_hash(m, 0x50) : 0);
    return pick_hr();
}
static int32_t __stdcall f_get_handle(FakeObj* o, FakeObj* dev, uint32_t* h) {
    lm(o, 5);
    lg(oid(dev));
    lg(aoff(h));
    int32_t hr = pick_hr();
    if (hr) return hr;
    *h = frnd();
    return 0;
}
static int32_t __stdcall f_tex_handle(FakeObj* o, FakeObj* dev, uint32_t* h) {
    lm(o, 3);
    lg(oid(dev));
    lg(aoff(h));
    int32_t hr = pick_hr();
    if (hr) return hr;
    *h = frnd();
    return 0;
}
static int32_t __stdcall f_load(FakeObj* o, FakeObj* src) {
    lm(o, 5);
    lg(oid(src));
    return pick_hr();
}
static void init_fakes() {
    for (auto& vt : g_vt)
        for (auto& s : vt) s = (void*)&f_bad;
    for (uint32_t k = 1; k < NKIND; k++) {
        g_vt[k][0] = (void*)&f_qi;
        g_vt[k][1] = (void*)&f_addref;
        g_vt[k][2] = (void*)&f_release;
    }
    g_vt[KD2][0x18 / 4] = (void*)&f_create_surface;
    g_vt[KD2][0x20 / 4] = (void*)&f_enum_modes;
    g_vt[KD2][0x2c / 4] = (void*)&f_dd_getcaps;
    g_vt[KD2][0x4c / 4] = (void*)&f_restore_mode;
    g_vt[KD2][0x50 / 4] = (void*)&f_set_coop;
    g_vt[KD2][0x54 / 4] = (void*)&f_set_mode;
    g_vt[KD2][0x5c / 4] = (void*)&f_get_vidmem;
    g_vt[KS][0x0c / 4] = (void*)&f_add_attached;
    g_vt[KS][0x14 / 4] = (void*)&f_blt;
    g_vt[KS][0x1c / 4] = (void*)&f_bltfast;
    g_vt[KS][0x2c / 4] = (void*)&f_flip;
    g_vt[KS][0x30 / 4] = (void*)&f_get_attached;
    g_vt[KS][0x48 / 4] = (void*)&f_flip_status;
    g_vt[KS][0x58 / 4] = (void*)&f_get_desc;
    g_vt[KS][0x64 / 4] = (void*)&f_lock;
    g_vt[KS][0x6c / 4] = (void*)&f_restore;
    g_vt[KS][0x74 / 4] = (void*)&f_set_ckey;
    g_vt[KS][0x80 / 4] = (void*)&f_unlock;
    g_vt[KD3][0x14 / 4] = (void*)&f_create_material;
    g_vt[KD3][0x18 / 4] = (void*)&f_create_viewport;
    g_vt[KD3][0x20 / 4] = (void*)&f_create_device;
    g_vt[KDEV][0x0c / 4] = (void*)&f_dev_getcaps;
    g_vt[KDEV][0x14 / 4] = (void*)&f_get_stats;
    g_vt[KDEV][0x18 / 4] = (void*)&f_add_viewport;
    g_vt[KDEV][0x1c / 4] = (void*)&f_delete_viewport;
    g_vt[KDEV][0x24 / 4] = (void*)&f_enum_tex;
    g_vt[KDEV][0x28 / 4] = (void*)&f_begin_scene;
    g_vt[KDEV][0x2c / 4] = (void*)&f_end_scene;
    g_vt[KDEV][0x34 / 4] = (void*)&f_set_cur_vp;
    g_vt[KDEV][0x5c / 4] = (void*)&f_set_rs;
    g_vt[KDEV][0x64 / 4] = (void*)&f_set_ls;
    g_vt[KDEV][0x68 / 4] = (void*)&f_set_xf;
    g_vt[KDEV][0x74 / 4] = (void*)&f_dp;
    g_vt[KDEV][0x78 / 4] = (void*)&f_dip;
    g_vt[KVP][0x18 / 4] = (void*)&f_transform;
    g_vt[KVP][0x20 / 4] = (void*)&f_set_bg;
    g_vt[KVP][0x30 / 4] = (void*)&f_clear;
    g_vt[KVP][0x44 / 4] = (void*)&f_set_vp2;
    g_vt[KMAT][0x0c / 4] = (void*)&f_set_material;
    g_vt[KMAT][0x14 / 4] = (void*)&f_get_handle;
    g_vt[KTEX][0x0c / 4] = (void*)&f_tex_handle;
    g_vt[KTEX][0x14 / 4] = (void*)&f_load;
}

// ---- the imports -------------------------------------------------------------------------------------------------------
static int32_t __stdcall st_dd_create(const uint32_t* guid, void** out, void* unk) {
    lg(K_DDCREATE);
    lg(aoff(guid));
    lg(guid && in_arena(guid, 16) ? fnv(guid, 16) : 0);
    lg(aoff(out));
    lg(aoff(unk));
    int32_t hr = pick_hr();
    if (hr) return hr;
    *out = new_obj(KD1);
    return 0;
}
static const char* const k_driver_names[] = {"display", "Primary Display Driver", "3dfx Voodoo2 (tm)", "Voodoo 3Dfx Interactive",
                                             "Matrox Millennium", "mm3dfx", "", "RIVA 128"};
static int32_t __stdcall st_dd_enum(void* cb, void* ctx) {
    lg(K_DDENUM);
    lg(aoff(cb));
    lg(aoff(ctx));
    int32_t hr = pick_hr();
    if (hr) return hr;
    int n = (int)(frnd() % 5);
    for (int i = 0; i < n; i++) {
        uint32_t* guid = 0;
        if (i > 0 || frnd() % 3) {
            guid = (uint32_t*)scratch(16);
            fill_rand(guid, 16);
        }
        char* desc = (char*)scratch(64);
        char* name = (char*)scratch(64);
        strcpy(desc, k_driver_names[frnd() % 8]);
        strcpy(name, k_driver_names[frnd() % 8]);
        lg(K_DDENUM_CB);
        int32_t r = ((int32_t(__stdcall*)(void*, char*, char*, void*))cb)(guid, desc, name, ctx);
        lg(K_CB_RET);
        lg((uint32_t)r);
        if (!r) break;
    }
    return 0;
}
static void __stdcall st_exit_process(uint32_t code) {
    lg(K_EXIT);
    lg(code);
    RaiseException(EXIT_CODE, 0, 0, 0);
}

// ---- the game functions around the group ----------------------------------------------------------------------------------
static void __cdecl st_log_report(const char* fmt, ...) {
    lg(K_REPORT);
    va_list ap;
    va_start(ap, fmt);
    lg_fmt(fmt, ap);
    va_end(ap);
}
static void __cdecl st_log_panic(const char* fmt, ...) {
    lg(K_PANIC);
    va_list ap;
    va_start(ap, fmt);
    lg_fmt(fmt, ap);
    va_end(ap);
    RaiseException(PANIC_CODE, 0, 0, 0);
}
static void* __cdecl st_mem_alloc(int n) {
    lg(K_ALLOC);
    lg((uint32_t)n);
    if ((int32_t)(frnd() % 100) < C()->oom_pct) return 0;
    uint32_t sz = ((uint32_t)n + 15) & ~15u;
    if (C()->heap_top + sz + 16 > ARENA_SIZE) C()->heap_top = HEAP_OFF;
    uint8_t* p = g_ar + C()->heap_top;
    memset(p, 0xa3, sz);
    C()->heap_top += sz + 16;
    return p;
}
static void __cdecl st_delete(void* p) {
    lg(K_FREE);
    lg(aoff(p));
}
static void __cdecl st_idle() { lg(K_IDLE); }
static int __cdecl st_ptime() {
    lg(K_TIME);
    C()->clock += (int32_t)(frnd() % 4) - (frnd() % 8 == 0 ? 5 : 0);
    return C()->clock;
}
static void* __cdecl st_get_window() {
    lg(K_WINDOW);
    return (void*)(uintptr_t)0x00abc123;
}
static void __cdecl st_build_canvas(uint8_t* c, char fmt, char* pix, int w, int h, int pitch) {
    lg(K_CANVAS);
    lg(aoff(c));
    lg((uint8_t)fmt);
    lg(aoff(pix));
    lg((uint32_t)w);
    lg((uint32_t)h);
    lg((uint32_t)pitch);
    c[0] = (uint8_t)fmt;
    c[1] = 0x80;
    memcpy(c + 4, &pix, 4);
    memcpy(c + 8, &w, 4);
    memcpy(c + 0xc, &h, 4);
    memcpy(c + 0x10, &pitch, 4);
}
static void __cdecl st_tex_flush() { lg(K_TFLUSH); }
static uint8_t __cdecl st_tex_select(int t) {
    lg(K_TSELECT);
    lg((uint32_t)t);
    return frnd() % 5 != 0;
}
static uint8_t __cdecl st_tex_alpha(int t) {
    lg(K_TALPHA);
    lg((uint32_t)t);
    return frnd() & 1;
}
// the harness's own callbacks (for the enumerators called directly)
static int32_t __stdcall h_texfmt_cb(void* desc, void* ctx) {
    lg(K_TEXFMT_CB);
    lg(safe_hash(desc, 0x6c));
    lg(aoff(ctx));
    return frnd() % 5 != 0;
}
static int32_t __stdcall h_modes_cb(void* desc, void* ctx) {
    lg(K_MODES_CB);
    lg(safe_hash(desc, 0x6c));
    lg(aoff(ctx));
    return frnd() % 5 != 0;
}
static int32_t __stdcall h_ddenum_cb(void* guid, char* d, char* n, void* ctx) {
    lg(K_DDENUM_CB);
    lg(guid ? safe_hash(guid, 16) : 0);
    lg(safe_hash_str(d));
    lg(safe_hash_str(n));
    lg(aoff(ctx));
    return frnd() % 3 != 0;
}
static void install_stubs() {
    patch_jmp(0x00411150, (void*)&st_log_report);
    patch_jmp(0x004112b0, (void*)&st_log_panic);
    patch_jmp(0x004140e0, (void*)&st_mem_alloc);
    patch_jmp(0x00414390, (void*)&st_delete);
    patch_jmp(0x00412bf0, (void*)&st_idle);
    patch_jmp(0x00413b40, (void*)&st_ptime);
    patch_jmp(0x00412be0, (void*)&st_get_window);
    patch_jmp(0x0044f880, (void*)&st_build_canvas);
    patch_jmp(0x0045a4c0, (void*)&st_tex_flush);
    patch_jmp(0x0045a4e0, (void*)&st_tex_select);
    patch_jmp(0x0045a590, (void*)&st_tex_alpha);
    *(uint32_t*)0x005d7478 = (uint32_t)(uintptr_t)&st_exit_process;   // imp__ExitProcess@4
    uint32_t a = iat_slot("DirectDrawCreate"), b = iat_slot("DirectDrawEnumerateA");
    if (!a || !b) { printf("the DDRAW import slots weren't found\n"); ExitProcess(2); }
    *(uint32_t*)(uintptr_t)a = (uint32_t)(uintptr_t)&st_dd_create;
    *(uint32_t*)(uintptr_t)b = (uint32_t)(uintptr_t)&st_dd_enum;
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}

// ---- the chain: every rewrite patched in (for a rewrite's pass) --------------------------------------------------------
struct ChainSave { uint32_t at; uint8_t b[5]; };
static std::vector<ChainSave> g_chain_save;
static void chain_on() {
    g_chain_save.clear();
    for (Reg* r = Reg::head(); r; r = r->next) {
        ChainSave s;
        s.at = r->at;
        memcpy(s.b, (void*)(uintptr_t)r->at, 5);
        g_chain_save.push_back(s);
        patch_jmp(r->at, r->fn);
    }
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}
static void chain_off() {
    for (size_t i = g_chain_save.size(); i-- > 0;) memcpy((void*)(uintptr_t)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_chain_save.clear();
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}

// ---- the image statics a check saves and compares -------------------------------------------------------------------
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_statics[] = {
    {0x004f1e88, 0x50, "vid statics"},        {0x004f2314, 0x1c, "dx statics"},    {0x004f3a60, 4, "the device"},
    {0x005229d8, 0x18, "dxState current"},    {0x00522a18, 0x10, "mrCaps"},        {0x00522ac8, 0x5c, "vid screen statics"},
    {0x00522e60, 0x10, "dx context / d3d / dx_result"}, {0x00522e78, 0x94, "dx error buffer"},
    {0x00522f0c, 0x84, "dx transform data"},  {0x005265f4, 4, "dxState pointer"}, {0x00526608, 0x1c, "dxState cache / force"},
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
    for (auto& g : g_statics)
        if (a >= g.at && a < g.at + g.n) return g.what;
    return "?";
}

// ---- running one call both ways ---------------------------------------------------------------------------------------
static DWORD g_fault_code;
static uint32_t g_fault_at;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
static int guarded(Reg* r, int rw, const uint32_t* a, uint64_t* ret) {
    __try {
        *ret = r->run(rw, r->at, a, 0);
        return 0;
    } __except (fault_filter(GetExceptionInformation())) {
        _fpreset();
        if (g_fault_code == PANIC_CODE) return 1;
        if (g_fault_code == EXIT_CODE) return 3;
        return 2;
    }
}
static int guarded_fp(Reg* r, const uint32_t* a, Footprint* f) {
    __try {
        r->run(0, r->at, a, f);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

struct Stat { std::string name; long n, bad, faults, panics, exits, ronly, fpbad, fpfault, fault_state; };
static std::vector<Stat> g_stats;
static Stat& stat(const std::string& nm) {
    for (Stat& s : g_stats)
        if (s.name == nm) return s;
    g_stats.push_back({nm, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    return g_stats.back();
}
static Footprint g_fp;
static std::vector<uint8_t> g_s0, g_s1, g_after;
static bool g_chain;
static long g_mismatch_total, g_checks_total, g_fp_total;
static int g_pc;
static const char* g_phase = "setup";
static std::string g_phase_name;

static int run_pass(Reg* r, int rw, const uint32_t* a, uint64_t* ret) {
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, g_pc == 24 ? _PC_24 : _PC_53, _MCW_PC);
    int k = guarded(r, rw, a, ret);
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
    if (off < SCRATCH_OFF) return "stub state";
    if (off < OBJ_OFF) return "fakes' scratch";
    if (off < PIX_OFF) return "fake objects";
    if (off < GAME_OFF) return "pixels";
    if (off < HEAP_OFF) return "world";
    return "heap";
}
static uint64_t check(Reg* r, const uint32_t* a) {
    std::string name = r->name;
    if (g_chain) name += " [chain]";
    g_phase_name = name;
    Stat& st = stat(name);
    st.n++;
    g_checks_total++;
    g_pc = chance(75) ? 24 : 53;                         // the main thread runs single precision after gxBegin
    ww_sync();
    snap_statics(g_s0);
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    int fpfault = guarded_fp(r, a, &g_fp);
    if (fpfault) st.fpfault++;
    if (g_fp.replay_only) st.ronly++;
    ww_sync();                                           // (a footprint writes nothing; to be sure)
    // the original
    g_pass = 0;
    g_log[0].clear();
    uint64_t r0 = 0, r1 = 0;
    int k0 = run_pass(r, 0, a, &r0);
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
    // the rewrite
    g_pass = 1;
    g_log[1].clear();
    int k1;
    if (g_chain) {
        chain_on();
        k1 = run_pass(r, 0, a, &r1);
        chain_off();
    } else {
        k1 = run_pass(r, 1, a, &r1);
    }
    DWORD c1 = k1 ? g_fault_code : 0;
    uint32_t a1 = g_fault_at;
    std::vector<uint32_t> d2 = ww_take();
    // compare
    bool same = k0 == k1 && c0 == c1 && (k0 != 0 || r0 == r1) && g_log[0] == g_log[1];
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
                    uint32_t ad = g.at + (uint32_t)(k - base);
                    char b[200];
                    sprintf(b, "static 0x%08x (%s): original %02x, rewrite %02x", ad, static_name(ad), g_s1[k], s1b[k]);
                    where = b;
                    break;
                }
                base += g.n;
            }
            state_same = false;
        }
    }
    if (k0 == 2 && k1 == 2 && c0 == c1) {
        if (!state_same) st.fault_state++;               // a fault in both: the state at the fault is only noted
    } else if (!state_same) {
        same = false;
    }
    // every byte the original changed in the game's memory lies in the footprint (unless replay_only)
    std::string fpwhere;
    if (k0 == 0 && !g_fp.replay_only && !fpfault) {
        for (size_t i = 0; i < d1.size() && fpwhere.empty(); i++) {
            if ((size_t)d1[i] * PG < GAME_OFF) continue;
            for (uint32_t k = 0; k < PG; k++) {
                const uint8_t* was = g_sh + (size_t)d1[i] * PG;
                if (g_after[i * PG + k] != was[k] && !in_footprint(g_ar + (size_t)d1[i] * PG + k)) {
                    char b[160];
                    sprintf(b, "arena %s +0x%x", arena_region(d1[i] * PG + k), d1[i] * PG + k);
                    fpwhere = b;
                    break;
                }
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
        if (!fpwhere.empty()) {
            st.fpbad++;
            g_fp_total++;
        }
    }
    if (k0 == 1) st.panics++;
    if (k0 == 2) st.faults++;
    if (k0 == 3) st.exits++;
    if (!same) {
        st.bad++;
        g_mismatch_total++;
    }
    if ((!same && st.bad <= 4) || (!fpwhere.empty() && st.fpbad <= 2)) {
        printf("  %s %s (check %ld, %s)\n", !same ? "MISMATCH" : "FOOTPRINT", name.c_str(), st.n, g_phase);
        if (k0 != k1 || c0 != c1)
            printf("    outcome: original %d (%08lx at %08x), rewrite %d (%08lx at %08x)\n", k0, c0, a0, k1, c1, a1);
        if (k0 == 0 && k1 == 0 && r0 != r1) printf("    return: original %08llx, rewrite %08llx\n", r0, r1);
        if (g_log[0] != g_log[1]) {
            size_t k = 0;
            while (k < g_log[0].size() && k < g_log[1].size() && g_log[0][k] == g_log[1][k]) k++;
            printf("    logs: %zu / %zu entries, first difference at %zu: %08x / %08x\n", g_log[0].size(), g_log[1].size(), k,
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
    return r0;
}
static std::map<uint32_t, Reg*> g_regs;
static uint64_t ck(uint32_t at, std::initializer_list<uint32_t> args) {
    Reg* r = g_regs.count(at) ? g_regs[at] : 0;
    if (!r) { printf("no rewrite registered at %08x\n", at); ExitProcess(2); }
    uint32_t a[12] = {0};
    int i = 0;
    for (uint32_t v : args) a[i++] = v;
    if (i != r->nargs) { printf("%s takes %d arguments, given %d\n", r->name, r->nargs, i); ExitProcess(2); }
    return check(r, a);
}

// ---- the world: the wrappers the statics point at, and buffers for arguments --------------------------------------------
static uint32_t g_world_top;
static uint32_t U(const void* p) { return (uint32_t)(uintptr_t)p; }
static uint8_t* wbuf(uint32_t n, int fill = -1) {       // a buffer in the world (random, or a fill byte)
    n = (n + 15) & ~15u;
    if (g_world_top + n > WORLD_SIZE) g_world_top = 0x10000;
    uint8_t* p = g_ar + WORLD_OFF + g_world_top;
    g_world_top += n;
    for (uint32_t i = 0; i < n; i++) p[i] = fill < 0 ? (uint8_t)rnd() : (uint8_t)fill;
    return p;
}
struct World {
    uint32_t dd, prim, back, zbuf, d3d, mat, vp;
    std::vector<uint32_t> surfs, texs, fakes_s, fakes_tex;
    uint32_t fake_d2, fake_d3, fake_dev, dxstate[4];
} W;
static uint32_t wrap1(uint32_t obj) {
    uint32_t* p = (uint32_t*)wbuf(4);
    p[0] = obj;
    return U(p);
}
static uint32_t pick(const std::vector<uint32_t>& v) { return v.empty() ? 0 : v[rnd() % v.size()]; }
static uint32_t pick_surf() { return chance(2) ? 0 : pick(W.surfs); }
static uint32_t pick_tex() { return chance(2) ? 0 : pick(W.texs); }
static uint32_t rbit() { return rnd() & 1; }
static uint32_t rbyte() { return chance(80) ? rbit() : rnd() & 0xff; }
static uint32_t rfloat() {
    float f = (float)(rnd() % 2000) / 1000.0f;
    uint32_t u;
    memcpy(&u, &f, 4);
    return chance(5) ? rnd() : u;
}
static void random_dxstate(uint8_t* s) {
    for (int i = 0; i < 0x18; i++) s[i] = (uint8_t)rbyte();
    uint32_t fog = chance(50) ? 0x8c8ca0 : rnd();
    memcpy(s + 4, &fog, 4);
    int32_t blend = chance(95) ? ri(0, 4) : ri(-1, 7);
    memcpy(s + 0x10, &blend, 4);
}
static void reset_world(int round) {
    memset(g_ar, 0, GAME_OFF);
    Ctrl* c = C();
    c->fr = rnd() | 1;
    c->nobj = 1;
    c->heap_top = HEAP_OFF;
    c->fail_pct = round % 4 == 0 ? 0 : round % 4 == 1 ? 3 : round % 4 == 2 ? 15 : 40;
    c->oom_pct = round % 5 == 4 ? 20 : 2;
    c->caps_valid = 0;
    c->clock = 1000;
    g_world_top = 0;
    W = World();
    W.fake_d2 = U(new_obj(KD2));
    W.fake_d3 = U(new_obj(KD3));
    W.fake_dev = U(new_obj(KDEV));
    for (int i = 0; i < 4; i++) W.fakes_s.push_back(U(new_obj(KS)));
    for (int i = 0; i < 3; i++) W.fakes_tex.push_back(U(new_obj(KTEX)));
    W.dd = wrap1(W.fake_d2);
    for (int i = 0; i < 5; i++) W.surfs.push_back(wrap1(U(new_obj(KS))));
    W.prim = W.surfs[0];
    W.back = W.surfs[1];
    W.zbuf = W.surfs[2];
    uint32_t* d3d = (uint32_t*)wbuf(8);
    d3d[0] = W.fake_d3;
    d3d[1] = W.fake_dev;
    W.d3d = U(d3d);
    uint32_t* mat = (uint32_t*)wbuf(8);
    mat[0] = U(new_obj(KMAT));
    mat[1] = rnd();
    W.mat = U(mat);
    W.vp = wrap1(U(new_obj(KVP)));
    for (int i = 0; i < 5; i++) {
        uint32_t* t = (uint32_t*)wbuf(16);
        t[0] = U(new_obj(KS));
        t[1] = U(new_obj(KTEX));
        t[2] = rnd();
        t[3] = chance(50) ? t[0] : 0;
        W.texs.push_back(U(t));
    }
    for (int i = 0; i < 4; i++) {
        uint8_t* s = wbuf(0x18);
        random_dxstate(s);
        W.dxstate[i] = U(s);
    }
    // the statics
    PV(V_DD) = chance(97) ? (void*)(uintptr_t)W.dd : 0;
    PV(V_PRIMARY) = (void*)(uintptr_t)(chance(97) ? W.prim : 0);
    PV(V_BACK) = (void*)(uintptr_t)(chance(97) ? W.back : 0);
    PV(V_3D) = (void*)(uintptr_t)(chance(85) ? W.back : chance(50) ? W.surfs[3] : 0);
    PV(V_ZBUF) = (void*)(uintptr_t)(chance(95) ? W.zbuf : 0);
    B8(V_HAS3DHW) = chance(85);
    B8(V_NO_SECONDARY) = chance(30);
    B8(V_ACTIVE) = chance(90);
    I32(V_CUR_MODE) = chance(70) ? ri(1, 4) : 0;
    B8(V_INITED) = (uint8_t)rbit();
    B8(V_WAS_LOST) = (uint8_t)rbit();
    for (uint32_t a = V_LOCKS; a <= V_FLIP_MAX; a += 4) I32(a) = ri(0, 50);
    B8(V_FIND_MEM) = chance(40);
    I32(V_PAGES) = ri(1, 4);
    static const int megs[] = {0, 2, 4, 8, 16, 3};
    I32(V_MEGS) = megs[rnd() % 6];
    I32(V_SCREEN_WID) = 640;
    I32(V_SCREEN_HIT) = 480;
    B8(V_IS3DFX) = (uint8_t)rbit();
    for (int i = 0; i < 6; i++) B8(V_MODE_OK + i) = chance(70);
    B8(V_AGP) = (uint8_t)rbit();
    B8(V_SCREEN_FMT) = chance(80) ? 4 : 3;
    PV(X_MATERIAL) = (void*)(uintptr_t)(chance(95) ? W.mat : 0);
    PV(X_VIEWPORT) = (void*)(uintptr_t)(chance(97) ? W.vp : 0);
    I32(X_COUNT1) = (int32_t)rnd();
    I32(X_COUNT2) = (int32_t)rnd();
    B8(X_VERTEX_ALPHA) = (uint8_t)rbit();
    I32(X_TEXTURE) = chance(40) ? -1 : ri(0, 6);
    B8(X_ALPHA_DIRTY) = (uint8_t)rbit();
    B8(X_CTX) = (uint8_t)rbit();
    PV(X_D3D) = (void*)(uintptr_t)(chance(98) ? W.d3d : 0);
    I32(X_RESULT) = 0;
    PV(D_DEVICE) = (void*)(uintptr_t)(chance(95) ? W.fake_dev : 0);
    for (uint32_t i = 0; i < 0x18; i++) B8(S_CUR + i) = (uint8_t)rbyte();
    I32(S_CUR + 0x10) = ri(0, 4);
    PV(S_PTR) = (void*)(uintptr_t)(chance(50) ? S_CUR : W.dxstate[rnd() % 4]);
    for (uint32_t i = 0; i < 0x18; i++) B8(S_CACHE + i) = (uint8_t)rbyte();
    I32(S_CACHE + 0x10) = ri(-1, 4);
    B8(S_FORCE) = (uint8_t)rbit();
    for (uint32_t i = 0; i < 0x10; i++) B8(0x522a18 + i) = (uint8_t)rbit();
    ((void(__cdecl*)())0x004587a0)();                    // init_xform_params (the original)
    ww_sync();
}

// the world repaired after the destructive calls: the statics back on the world's wrappers, their interfaces back
// (most of the time: a broken world now and then is a test too)
static void heal() {
    if (chance(8)) return;
    auto fix = [](uint32_t w, uint32_t off, uint32_t kind) {
        uint32_t* p = (uint32_t*)(uintptr_t)(w + off);
        if (!*p || !is_obj((void*)(uintptr_t)*p)) *p = U(new_obj(kind));
    };
    fix(W.dd, 0, KD2);
    for (uint32_t s : W.surfs) fix(s, 0, KS);
    fix(W.d3d, 0, KD3);
    fix(W.d3d, 4, KDEV);
    fix(W.mat, 0, KMAT);
    fix(W.vp, 0, KVP);
    for (uint32_t t : W.texs) {
        fix(t, 0, KS);
        fix(t, 4, KTEX);
    }
    PV(V_DD) = (void*)(uintptr_t)W.dd;
    PV(V_PRIMARY) = (void*)(uintptr_t)W.prim;
    PV(V_BACK) = (void*)(uintptr_t)W.back;
    PV(V_3D) = (void*)(uintptr_t)(chance(90) ? W.back : W.surfs[3]);
    PV(V_ZBUF) = (void*)(uintptr_t)W.zbuf;
    PV(X_D3D) = (void*)(uintptr_t)W.d3d;
    PV(D_DEVICE) = *(void**)(uintptr_t)(W.d3d + 4);
    PV(X_MATERIAL) = (void*)(uintptr_t)W.mat;
    PV(X_VIEWPORT) = (void*)(uintptr_t)W.vp;
    if (!PV(S_PTR) || chance(10)) PV(S_PTR) = (void*)(uintptr_t)(chance(50) ? S_CUR : W.dxstate[rnd() % 4]);
    if (I32(V_SCREEN_WID) <= 0 || I32(V_SCREEN_WID) > 1024 || chance(10)) {
        static const int32_t wh[][2] = {{512, 384}, {640, 480}, {800, 600}, {1024, 768}};
        int m = (int)(rnd() % 4);
        I32(V_SCREEN_WID) = wh[m][0];
        I32(V_SCREEN_HIT) = wh[m][1];
    }
    if (chance(70)) I32(X_RESULT) = (int32_t)(chance(50) ? rnd() : 0x88760000u + (rnd() & 0x3ff));   // a store missed shows
    if (chance(20)) B8(V_HAS3DHW) = chance(85);
    if (chance(20)) B8(V_ACTIVE) = chance(90);
    if (chance(10)) B8(V_NO_SECONDARY) = chance(30);
}

// ---- the calls: every function, with arguments from the world --------------------------------------------------------
static uint32_t rmode() { return chance(90) ? (uint32_t)ri(1, 4) : (uint32_t)ri(-1, 7); }
static uint32_t rdesc() {                                // a surface description for CreateSurface
    uint32_t* d = (uint32_t*)wbuf(0x6c);
    d[0] = 0x6c;
    d[1] = rnd() & 0x1fff;
    if (chance(50)) d[0x68 / 4] |= 0x1000;
    else d[0x68 / 4] &= ~0x1000u;
    return U(d);
}
static uint32_t rxform() {                               // a D3DTRANSFORMDATA over world buffers
    uint32_t* x = (uint32_t*)wbuf(0x34);
    x[0] = 0x34;
    x[1] = U(wbuf(0x20));
    x[2] = 0x20;
    x[3] = chance(95) ? U(wbuf(0x20)) : 0;
    x[4] = 0x20;
    x[5] = chance(90) ? U(wbuf(0x10)) : 0;
    x[6] = 0x3f;
    return U(x);
}
static uint32_t rverts(uint32_t n) { return U(wbuf(n * 32)); }
static uint32_t ridx(uint32_t n) { return U(wbuf(n * 2)); }
static const uint32_t k_msgs[] = {0x4f3b28, 0x4f3be4, 0x4f3d28, 0x4f1f54, 0x4f3d50};   // (none with a %: LogReport(msg) has no arguments)
static uint32_t rhr() {
    if (chance(30)) return 0;
    if (chance(50)) return k_err_names[rnd() % (sizeof k_err_names / sizeof k_err_names[0])].hr;
    return chance(50) ? 0x88760000u | (rnd() & 0x3ff) : rnd();
}

static void call(uint32_t at) {
    uint32_t n;
    switch (at) {
    // vid.obj
    case 0x004545b0: case 0x00454630: case 0x00454650: case 0x00454660: case 0x00454720: case 0x00454850:
    case 0x00454870: case 0x00454880: case 0x004548f0: case 0x00454900: case 0x00454930: case 0x00454940:
    case 0x00454950: case 0x00454990: case 0x004549e0: case 0x00454a90: case 0x00454c20: case 0x00454e80:
    case 0x00454f20: case 0x00455070: case 0x00455100: case 0x00455130: case 0x004553e0: case 0x00455470:
    case 0x00455480:
        ck(at, {});
        break;
    case 0x00454670: ck(at, {rmode(), rbyte()}); break;   // VidSetMode
    case 0x00454740: ck(at, {U(wbuf(0x24))}); break;      // VidGetPage
    case 0x00454910: ck(at, {(uint32_t)ri(0, 6)}); break;
    case 0x00454a00: ck(at, {chance(30) ? 0 : U(wbuf(16))}); break;
    case 0x00454ac0: {                                    // hardware_callback
        char* d = (char*)wbuf(64, 0);
        char* nm = (char*)wbuf(64, 0);
        strcpy(d, k_driver_names[rnd() % 8]);
        strcpy(nm, k_driver_names[rnd() % 8]);
        ck(at, {chance(20) ? 0 : U(wbuf(16)), U(d), U(nm), U(wbuf(4))});
        break;
    }
    case 0x00454b50: ck(at, {(uint32_t)ri(-1, 6)}); break;   // set_mode
    case 0x00454c30: ck(at, {rbyte()}); break;
    case 0x00454ca0: ck(at, {rbyte(), U(wbuf(0x20))}); break;
    case 0x00454ea0: ck(at, {U(wbuf(0x20))}); break;
    case 0x00454f50: ck(at, {pick_surf(), rbyte()}); break;
    case 0x00455120: ck(at, {rbyte()}); break;
    case 0x00455330: {                                    // mode_callback
        static const uint32_t modes[][2] = {{320, 200}, {512, 384}, {640, 480}, {800, 600}, {1024, 768}, {640, 400}, {320, 240}};
        uint32_t* d = (uint32_t*)wbuf(0x6c);
        uint32_t m = rnd() % 8;
        d[3] = m < 7 ? modes[m][0] : rnd() % 1500;
        d[2] = m < 7 ? modes[m][1] : rnd() % 1500;
        ck(at, {U(d), U(wbuf(4))});
        break;
    }
    // dx.obj
    case 0x00458240: case 0x00458260: case 0x004582a0: case 0x00458450: case 0x00458460: case 0x00458470:
    case 0x00458480: case 0x00458490: case 0x004584d0: case 0x00458500: case 0x00458530: case 0x00458550:
    case 0x004585d0: case 0x004586b0: case 0x004587a0: case 0x00458920: case 0x00458930: case 0x00458a90:
    case 0x00458c10:
        ck(at, {});
        break;
    case 0x00458270: C()->caps_valid = 1; ck(at, {chance(30) ? 0 : U(wbuf(0x10))}); C()->caps_valid = 0; break;
    case 0x004582b0: C()->caps_valid = 1; ck(at, {U(wbuf(0x10))}); C()->caps_valid = 0; break;
    case 0x004585b0: ck(at, {(uint32_t)ri(0, 640), (uint32_t)ri(0, 480), chance(3) ? 0 : (uint32_t)ri(1, 1024), (uint32_t)ri(0, 768)}); break;
    case 0x00458610: ck(at, {rfloat(), rfloat(), rfloat()}); break;
    case 0x00458750: ck(at, {chance(20) ? 0 : W.mat}); break;
    case 0x00458770: ck(at, {(uint32_t)ri(0, 640), (uint32_t)ri(0, 480), (uint32_t)ri(0, 640), (uint32_t)ri(0, 480), rbyte()}); break;
    case 0x00458800: ck(at, {U(wbuf(12)), U(wbuf(12)), rbyte()}); break;
    case 0x00458890: ck(at, {U(wbuf(4)), U(wbuf(4)), U(wbuf(4)), U(wbuf(4))}); break;
    case 0x00458970: ck(at, {rbyte()}); break;
    case 0x00458990: case 0x00458aa0: {                   // dxDPBegin / dxBeginTriangles
        int32_t t = chance(30) ? -1 : chance(40) ? I32(X_TEXTURE) : ri(0, 8);
        ck(at, {(uint32_t)t});
        break;
    }
    case 0x00458a60: case 0x00458bb0: case 0x00458be0:
        n = (uint32_t)ri(0, 40);
        ck(at, {rverts(n), n, ridx(n * 2), n * 2});
        break;
    case 0x00458b70: ck(at, {rverts(3)}); break;
    case 0x00458b90: n = (uint32_t)ri(0, 30); ck(at, {rverts(n), n}); break;
    case 0x00458c20: ck(at, {k_msgs[rnd() % 5], rhr()}); break;
    case 0x00458c50: ck(at, {rhr()}); break;
    case 0x00459630: {                                    // convert_caps
        uint32_t* d = (uint32_t*)wbuf(0xcc);
        if (chance(80)) d[1] |= 0x1c3;
        if (chance(60)) d[2] = 1 + rnd() % 3;
        ck(at, {U(d), U(wbuf(0x80))});
        break;
    }
    // dxstate.obj
    case 0x0045de20: case 0x0045def0: case 0x0045dfd0: case 0x0045e390: case 0x0045e3d0: case 0x0045e410:
        if (at == 0x0045dfd0 && chance(50)) {
            PV(S_PTR) = (void*)(uintptr_t)W.dxstate[rnd() % 4];
            random_dxstate((uint8_t*)PV(S_PTR));
            for (uint32_t i = 0; i < 0x18; i++)
                if (chance(40)) B8(S_CACHE + i) = (uint8_t)rbyte();
            if (chance(50)) I32(S_CACHE + 0x10) = ri(-1, 4);
        }
        ck(at, {});
        break;
    case 0x0045df00: {                                    // dxStateUpdate
        uint32_t s = chance(30) ? S_CUR : W.dxstate[rnd() % 4];
        if (chance(50)) random_dxstate((uint8_t*)(uintptr_t)s);
        if (chance(50)) B8(S_FORCE) = 1;
        ck(at, {s});
        break;
    }
    case 0x0045e420: case 0x0045e4e0: case 0x0045e540: case 0x0045e560: case 0x0045e5a0: case 0x0045e5c0:
    case 0x0045e680: case 0x0045e6a0: case 0x0045e6c0: case 0x0045e6e0:
        ck(at, {rbyte()});
        break;
    case 0x0045e440: case 0x0045e4a0: case 0x0045e4c0: case 0x0045e500: case 0x0045e520: case 0x0045e580:
    case 0x0045e5e0: case 0x0045e660:
        ck(at, {chance(80) ? (uint32_t)ri(0, 8) : rnd()});
        break;
    case 0x0045e700: ck(at, {rfloat()}); break;
    case 0x0045e460: ck(at, {(uint32_t)ri(0, 4), rbyte(), rbyte()}); break;
    case 0x0045e600: case 0x0045e630: ck(at, {(uint32_t)ri(0, 10), (uint32_t)ri(0, 10)}); break;
    // dd.obj
    case 0x0045e940: ck(at, {chance(50) ? 0x00454ac0u : (uint32_t)(uintptr_t)&h_ddenum_cb}); break;
    case 0x0045e950: ck(at, {chance(30) ? 0 : U(wbuf(16))}); break;
    case 0x0045ea20: ck(at, {chance(10) ? 0 : wrap1(W.fake_d2)}); break;
    case 0x0045ea40: ck(at, {U(wbuf(8, 0xa3)), 0, W.fake_d3, U(wbuf(0x50))}); break;   // dmaterial::dmaterial
    case 0x0045eaf0: {
        uint32_t* m = (uint32_t*)wbuf(8);
        m[0] = U(new_obj(KMAT));
        ww_sync();
        ck(at, {U(m), 0});
        break;
    }
    case 0x0045eb10: ck(at, {W.mat, 0, U(wbuf(0x50))}); break;
    case 0x0045eb40: ck(at, {U(wbuf(4, 0xa3)), 0, W.fake_d3, W.mat}); break;   // dviewport::dviewport
    case 0x0045ebf0: ck(at, {wrap1(U(new_obj(KVP))), 0}); break;
    case 0x0045ec20: ck(at, {W.vp, 0, (uint32_t)ri(0, 640), (uint32_t)ri(0, 480), chance(3) ? 0 : (uint32_t)ri(1, 1024), (uint32_t)ri(0, 768)}); break;
    case 0x0045ece0: ck(at, {W.vp, 0, (uint32_t)ri(0, 640), (uint32_t)ri(0, 480), (uint32_t)ri(0, 640), (uint32_t)ri(0, 480), rbyte()}); break;
    case 0x0045ed50: ck(at, {W.vp, 0, 1, rxform(), (uint32_t)ri(1, 2), chance(95) ? U(wbuf(4)) : 0}); break;
    case 0x0045ed90: ck(at, {U(wbuf(8, 0xa3)), 0, W.dd}); break;
    case 0x0045edb0: case 0x0045ee00: {
        uint32_t* d = (uint32_t*)wbuf(8);
        d[0] = U(new_obj(KD3));
        d[1] = U(new_obj(KDEV));
        ck(at, {U(d), 0});
        break;
    }
    case 0x0045edd0: case 0x0045ee70: ck(at, {W.d3d, 0, pick_surf()}); break;
    case 0x0045ee20: ck(at, {W.d3d, 0, W.dd}); break;
    case 0x0045ef20: ck(at, {W.d3d, 0, U(wbuf(0x50))}); break;
    case 0x0045ef50: {
        uint32_t* m = (uint32_t*)wbuf(8);
        m[0] = U(new_obj(KMAT));
        ck(at, {W.d3d, 0, chance(15) ? 0 : U(m)});
        break;
    }
    case 0x0045ef70: ck(at, {W.d3d, 0, W.mat}); break;
    case 0x0045efa0: ck(at, {W.d3d, 0, chance(15) ? 0 : wrap1(U(new_obj(KVP)))}); break;
    case 0x0045efc0: ck(at, {W.d3d, 0, U(wbuf(0xcc)), U(wbuf(0xcc))}); break;
    case 0x0045f000: ck(at, {W.d3d, 0, U(wbuf(0x18))}); break;
    case 0x0045f040: case 0x0045f070: ck(at, {W.d3d, 0}); break;
    case 0x0045f0a0: case 0x0045f0d0: ck(at, {W.d3d, 0, (uint32_t)ri(0, 50), rnd()}); break;
    case 0x0045f100: ck(at, {W.d3d, 0, (uint32_t)ri(1, 3), U(wbuf(0x40))}); break;
    case 0x0045f130: ck(at, {W.d3d, 0, (uint32_t)(uintptr_t)&h_texfmt_cb, U(wbuf(4))}); break;
    case 0x0045f160: ck(at, {U(wbuf(4, 0xa3)), 0, W.fake_d2}); break;
    case 0x0045f170: ck(at, {wrap1(W.fake_d2), 0}); break;
    case 0x0045f190: ck(at, {W.dd, 0, U(wbuf(0x16c)), U(wbuf(0x16c))}); break;
    case 0x0045f1c0: ck(at, {W.dd, 0, U(wbuf(4)), U(wbuf(4)), U(wbuf(4))}); break;
    case 0x0045f200: case 0x0045f2c0: ck(at, {W.dd, 0}); break;
    case 0x0045f240: ck(at, {W.dd, 0, U(wbuf(0x6c)), chance(50) ? 0x00455330u : (uint32_t)(uintptr_t)&h_modes_cb}); break;
    case 0x0045f270: ck(at, {W.dd, 0, (uint32_t)ri(320, 1024), (uint32_t)ri(200, 768), chance(80) ? 16u : 32u}); break;
    case 0x0045f2d0: case 0x0045f3a0: case 0x0045f400: ck(at, {W.dd, 0, rdesc()}); break;
    case 0x0045f3e0: ck(at, {W.dd, 0, chance(15) ? 0 : wrap1(U(new_obj(KS)))}); break;
    case 0x0045f440: {
        uint32_t* t = (uint32_t*)wbuf(16);
        t[0] = U(new_obj(KS));
        t[1] = U(new_obj(KTEX));
        ck(at, {W.dd, 0, chance(15) ? 0 : U(t)});
        break;
    }
    case 0x0045f460: ck(at, {U(wbuf(4, 0xa3)), 0, pick(W.fakes_s)}); break;
    case 0x0045f470: ck(at, {wrap1(U(new_obj(KS))), 0}); break;
    case 0x0045f490: case 0x0045f5a0: ck(at, {pick_surf(), 0, U(wbuf(0x6c))}); break;
    case 0x0045f510: case 0x0045f520: case 0x0045f580: case 0x0045f670: ck(at, {pick_surf(), 0}); break;
    case 0x0045f5d0: {
        uint32_t* c = (uint32_t*)wbuf(4);
        c[0] = chance(70) ? 4 : rnd();
        ck(at, {pick_surf(), 0, U(c)});
        break;
    }
    case 0x0045f640: case 0x0045f690: ck(at, {pick_surf(), 0, pick_surf()}); break;
    case 0x0045f6d0: ck(at, {pick_surf(), 0, pick_surf(), (uint32_t)ri(0, 256), (uint32_t)ri(0, 256), chance(30) ? 0 : U(wbuf(16))}); break;
    case 0x0045f710: ck(at, {pick_surf(), 0, rnd() & 0xffff}); break;
    case 0x0045f750: ck(at, {U(wbuf(16, 0xa3)), 0, pick(W.fakes_s)}); break;
    case 0x0045f7c0: {
        uint32_t* t = (uint32_t*)wbuf(16);
        t[0] = U(new_obj(KS));
        t[1] = U(new_obj(KTEX));
        ck(at, {U(t), 0});
        break;
    }
    case 0x0045f7e0: ck(at, {pick_tex(), 0, pick_tex()}); break;
    case 0x0045f870: case 0x0045f930: case 0x0045f940: case 0x0045f9a0: ck(at, {pick_tex(), 0}); break;
    case 0x0045f8a0: ck(at, {pick_tex(), 0, rbyte(), rbyte()}); break;
    default:
        printf("no generator for %08x\n", at);
        ExitProcess(2);
    }
}

// ---- rounds -------------------------------------------------------------------------------------------------------------
static std::vector<uint32_t> g_addrs;
static void round_all(int r) {
    reset_world(r);
    std::vector<uint32_t> order = g_addrs;
    for (size_t i = order.size(); i > 1; i--) std::swap(order[i - 1], order[rnd() % i]);
    for (uint32_t at : order) {
        heal();
        call(at);
        if (chance(3)) reset_world(r);                     // a fresh world now and then (the destructive ones)
    }
}
// the calls a frame makes, in the frame's order, on one world
static void round_frame(int r) {
    reset_world(r);
    C()->fail_pct = r % 3 == 0 ? 0 : 5;
    ww_sync();
    for (int f = 0; f < 6; f++) {
        heal();
        call(0x00458920);                                  // dxDPBeginFrame
        call(0x0045df00);                                  // dxStateUpdate
        for (int s = 0; s < 12; s++) {
            if (chance(30)) call(0x00458970);              // dxEnableVertexAlpha
            call(chance(50) ? 0x00458990 : 0x00458aa0);
            call(chance(60) ? 0x00458a60 : chance(50) ? 0x00458b70 : 0x00458be0);
            if (chance(20)) call(0x0045dfd0);
        }
        call(0x00458770);                                  // dxClearZ
        call(0x00458930);                                  // dxDPEndFrame
        call(0x00454740);                                  // VidGetPage
        call(0x00454870);                                  // VidReleasePage
        call(0x00454880);                                  // VidFlip
    }
}
// the start-up and mode-change sequences, in the game's order
static void round_setup(int r) {
    reset_world(r);
    C()->fail_pct = r % 3 == 0 ? 0 : 4;
    ww_sync();
    static const uint32_t seq[] = {0x004545b0, 0x00458240, 0x00454670, 0x00458270, 0x0045de20, 0x0045df00, 0x0045dfd0,
                                   0x00458610, 0x004585b0, 0x00458450, 0x00454670, 0x00458460, 0x00454720, 0x004582a0,
                                   0x00458260, 0x00454630};
    for (uint32_t at : seq) {
        if (chance(50)) heal();
        call(at);
    }
}
// the functions with thresholds and arithmetic, many times each, on a healed world
static void round_focus(int r) {
    reset_world(r);
    static const uint32_t fns[] = {0x00455130, 0x00454910, 0x00454670, 0x00454b50, 0x004585b0, 0x0045ec20, 0x00458800,
                                   0x004582b0, 0x00459630, 0x0045dfd0, 0x0045df00, 0x00458990, 0x00458aa0, 0x00455330,
                                   0x0045f490, 0x0045f520, 0x0045f940, 0x00454ca0, 0x00454f50, 0x0045f2d0, 0x0045f0a0,
                                   0x0045f8a0, 0x0045ea40, 0x0045eb40, 0x00454740, 0x00454880};
    for (uint32_t at : fns)
        for (int i = 0; i < 12; i++) {
            heal();
            if (i % 4 == 0) C()->fail_pct = ri(0, 50);
            if (at == 0x00455130 && chance(50)) I32(V_MEGS) = ri(0, 20);
            call(at);
        }
}
// dx_name_error on every code it names, their neighbours, and others
static void round_names() {
    for (const ErrName& e : k_err_names)
        for (int d = -1; d <= 1; d++) ck(0x00458c50, {e.hr + (uint32_t)d});
    for (uint32_t v : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x88760000u, 0x88760121u, 0x88760308u, 0x80004002u})
        ck(0x00458c50, {v});
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS* e) {
    printf("UNHANDLED exception %08lx at %08x during %s / %s, pass %d\n", e->ExceptionRecord->ExceptionCode,
           (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, g_phase, g_phase_name.c_str(), g_pass);
    fflush(stdout);
    ExitProcess(6);
}
static int main2(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 40;
    g_rng = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 0x2545f491u;
    if (!g_rng) g_rng = 1;
    const char* modes = argc > 3 ? argv[3] : "both";
    if (!load_race_exe("out\\race_v10.exe")) return 2;
    g_ar = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    g_sh = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_ww = (ULONG_PTR*)malloc(sizeof(ULONG_PTR) * NPAGES);
    if (!g_ar || !g_sh || !g_ww) { printf("can't allocate the arena\n"); return 2; }
    init_fakes();
    install_stubs();
    for (Reg* r = Reg::head(); r; r = r->next) {
        g_regs[r->at] = r;
        g_addrs.push_back(r->at);
    }
    std::sort(g_addrs.begin(), g_addrs.end());
    ww_sync();
    printf("%zu rewrites; %d rounds, seed 0x%08x\n", g_addrs.size(), rounds, g_rng);
    uint32_t seed = g_rng;
    for (int mode = 0; mode < 2; mode++) {
        if ((mode == 0 && !strcmp(modes, "chain")) || (mode == 1 && !strcmp(modes, "isolated"))) continue;
        g_chain = mode == 1;
        g_rng = seed;
        printf("-- %s --\n", g_chain ? "chain" : "isolated");
        g_phase = "names";
        reset_world(0);
        round_names();
        for (int r = 0; r < rounds; r++) {
            g_phase = "all";
            round_all(r);
            g_phase = "frame";
            round_frame(r);
            g_phase = "setup";
            round_setup(r);
            g_phase = "focus";
            round_focus(r);
        }
    }
    printf("\n%-40s %8s %6s %6s %6s %6s %6s %6s %s\n", "function", "checks", "differ", "panics", "faults", "exits", "ronly", "fp",
           "(fault-state)");
    std::sort(g_stats.begin(), g_stats.end(), [](const Stat& a, const Stat& b) { return a.name < b.name; });
    for (auto& s : g_stats)
        printf("%-40s %8ld %6ld %6ld %6ld %6ld %6ld %6ld %ld\n", s.name.c_str(), s.n, s.bad, s.panics, s.faults, s.exits, s.ronly,
               s.fpbad, s.fault_state);
    size_t unchecked = 0;
    for (uint32_t at : g_addrs) {
        bool seen = false;
        for (auto& s : g_stats)
            if (s.name == g_regs[at]->name || s.name == std::string(g_regs[at]->name) + " [chain]") seen = true;
        if (!seen) {
            printf("  never checked: %s\n", g_regs[at]->name);
            unchecked++;
        }
    }
    printf("%ld checks, %ld differ, %ld footprint misses, %zu functions never checked\n", g_checks_total, g_mismatch_total,
           g_fp_total, unchecked);
    return g_mismatch_total || g_fp_total || unchecked ? 1 : 0;
}
}  // namespace hx

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    SetUnhandledExceptionFilter(hx::unhandled);
    if (!getenv("VP_WORLD_CHILD")) return hx::relaunch();
    return hx::main2(argc, argv);
}
