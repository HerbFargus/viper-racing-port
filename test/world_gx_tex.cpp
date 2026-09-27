// world_gx_tex.cpp -- group G1c's rewrites (hook/gx_tex.cpp: gx.obj, texture.obj, tmap.obj, tex.obj) against the
// originals, on random texture worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_gx_tex.cpp
//        /Fo<dir>\ /Fe<dir>\world_gx_tex.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_gx_tex.exe [worlds] [seed] [game dir]
//          (the game dir defaults to ..\game-files\installs\v1.0-RC beside the repository; it is only ever READ:
//          the .tex resources of its archives are the texture data)
//
// Loads out\race_v10.exe at 0x400000 in a child process (the range reserved before its heap exists) and includes the
// rewrite file itself; every PORT_FN is registered with a generic caller for the original (its address) and the
// rewrite, and its footprint.
//
// DirectDraw / Direct3D are recording fakes behind the game's own dd.obj wrappers, which run as the originals: an
// IDirectDraw2 (CreateSurface builds a mip chain of fake surfaces with real pixel memory), IDirectDrawSurface3 (QI,
// AddRef, Release, Lock -- the pixels --, Unlock, GetAttachedSurface, SetColorKey, Blt, BltFast), IDirect3DTexture2
// (Load, GetHandle) and IDirect3DDevice2 (EnumTextureFormats -- a scripted set of pixel formats --, SetRenderState).
// Every call is logged with its arguments, and each can be scripted to fail (out of video memory, a lost surface,
// still drawing, no attached surface, E_NOINTERFACE ...). The game's 2D code (gxBuildCanvas, gxSetCanvas, gxClear,
// gxPaste, gxPasteAlpha, gfxReduceTo555), its pools (PoolBase), its CRT's stricmp / strstr and dd.obj run as the
// originals. Stubbed (logging): MemAlloc (a bump heap with the game's 0xa3 fill; scripted NULLs) / MemFree / operator
// delete, LogReport, LogPanic (which, as in the game, doesn't return: it raises), sprintf (the host's), the gx lock
// (SingleBegin / Enter / Leave / End), vid.obj's mode and page functions, mr_*, ExceptSinglePrecision, ASSERT_MSG,
// ResourceGetDiscardable / ResourceForget (the real .tex payloads of the game's archives; missing names, bad
// versions), ExitProcess (its import slot) and Win32Idle (unexpected).
//
// Each world: the image's .data/.bss back to the file's, M1 on or off (on: the texture table moved to a 1000-entry
// table in the arena by patching the 66 operands of texture_table_fields.inc and the two counts, the pools raised to
// 1024 -- as viperport.cpp's relocate_texture_table / lift_texture_limit do), random caps (mrCaps.alpha 0/1/2,
// hardware mips, the mip feature, VRAM, 3D hardware, 3Dfx), a random set of offered pixel formats; then the ORIGINAL
// code builds a texture cache: TextureBegin, TextureGet of real .tex resources (every format: opaque, keyed, alpha,
// dual; 4 to 256 texels) and of odd names (missing, no .tex, '=', .bmp / .stp, effects.tex, and names of 12..40
// characters -- the 16-character bug), TextureCreate, TextureSelect (video copies), forgets, a de-rez or a grab left
// pending. Then one registered function is picked with random arguments (ids, texture_infos, canvases over arena
// pixels, floats, names, pixel formats ...), a script of failures is drawn, and: the arena's live parts and the
// image's .data/.bss are saved, the ORIGINAL runs, the state is put back, the REWRITE runs -- "isolated" (its callees
// the originals) or "chain" (every rewrite of the group patched in) --, and the return value, the whole state, the
// stubs' and the fakes' logs and any fault are compared; every byte the original changed outside the fakes' memory
// must lie in the rewrite's footprint unless it is replay_only. The FPU runs at a random precision, in 30% of the
// worlds with overflow / divide-by-zero unmasked. At the start, the 16-character name bug is shown on the original and
// the rewrite (a scripted run of TextureGet / TextureSelect with names of 15..30 characters).
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef FIX_TESTS
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
static const bool k_ordinary = false;
#else
static const bool k_ordinary = true;        // the random rounds keep to inputs the fixes don't change
#endif
#include "../hook/port.h"

// ---- the registry: every PORT_FN with a generic caller -------------------------------------------------------------
template <typename T> static T conv(uint32_t v) {
    if constexpr (std::is_pointer_v<T>) return (T)(uintptr_t)v;
    else if constexpr (std::is_same_v<T, float>) { float f; memcpy(&f, &v, 4); return f; }
    else return (T)v;
}
template <typename R> static uint64_t ret64(R r) {
    if constexpr (std::is_pointer_v<R>) return (uint32_t)(uintptr_t)r;
    else if constexpr (sizeof(R) == 1) return (uint8_t)r;
    else return (uint32_t)r;
}
typedef uint64_t (*Caller_t)(const uint32_t*);
typedef void (*FpCaller_t)(Footprint&, const uint32_t*);
struct Reg {
    uint32_t at; const char* name; void* fn; Caller_t orig, neu; FpCaller_t fp; int nargs; Reg* next;
    static Reg*& head() { static Reg* h; return h; }
    Reg(uint32_t a, const char* n, void* f, Caller_t o, Caller_t nw, FpCaller_t p, int na)
        : at(a), name(n), fn(f), orig(o), neu(nw), fp(p), nargs(na), next(head()) { head() = this; }
};
template <typename F> struct Gen;
#define GEN_CC(CC)                                                                                                     \
    template <typename R, typename... A> struct Gen<R(CC*)(A...)> {                                                  \
        typedef R(CC* Fn)(A...);                                                                                      \
        template <size_t... I> static uint64_t run(Fn f, const uint32_t* a, std::index_sequence<I...>) {              \
            if constexpr (std::is_void_v<R>) { f(conv<A>(a[I])...); return 0; }                                      \
            else return ret64(f(conv<A>(a[I])...));                                                                   \
        }                                                                                                             \
        template <uint32_t AT> static uint64_t orig(const uint32_t* a) {                                              \
            return run((Fn)(uintptr_t)AT, a, std::index_sequence_for<A...>{});                                        \
        }                                                                                                             \
        template <Fn NEW> static uint64_t neu(const uint32_t* a) { return run(NEW, a, std::index_sequence_for<A...>{}); } \
        template <void (*FP)(Footprint&, A...), size_t... I>                                                          \
        static void fpI(Footprint& f, const uint32_t* a, std::index_sequence<I...>) { FP(f, conv<A>(a[I])...); }      \
        template <void (*FP)(Footprint&, A...)> static void fp(Footprint& f, const uint32_t* a) {                     \
            fpI<FP>(f, a, std::index_sequence_for<A...>{});                                                           \
        }                                                                                                             \
        static const int nargs = (int)sizeof...(A);                                                                   \
    };
GEN_CC(__cdecl)
GEN_CC(__fastcall)
GEN_CC(__stdcall)
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                                \
    static Reg VP_CAT(reg_, NEW)(V10, NAME, (void*)&NEW, &Gen<decltype(&NEW)>::template orig<V10>,                     \
                                 &Gen<decltype(&NEW)>::template neu<&NEW>, &Gen<decltype(&NEW)>::template fp<&FP>,     \
                                 Gen<decltype(&NEW)>::nargs);

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }
// a pointer left aimed at the function's own stack frame: listed (the comparison treats gx's current canvas so)
void Footprint::stack_ptr(void* p, const char* what) { add(p, 4, what); }
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

#include "../hook/gx_tex.cpp"

static Reg* reg_at(uint32_t at) { for (Reg* r = Reg::head(); r; r = r->next) if (r->at == at) return r; return 0; }

// ---- random numbers --------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x6a09e667u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t wildf_bits() {
    uint32_t r = rnd();
    switch (r % 10) {
    case 0: return 0x7fc00000u | (rnd() & 0x3fffff);
    case 1: return 0x7f800001u | (rnd() & 0x3fffff);
    case 2: return (r & 0x100) ? 0xff800000u : 0x7f800000u;
    case 3: return fbits(((r & 0x100) ? -1.0f : 1.0f) * (1e30f + uni() * 3e38f));
    case 4: return rnd() & 0x807fffff;
    case 5: return (r & 0x100) ? 0x80000000u : 0;
    case 6: return fbits(-uni() * 4.0f);
    case 7: return fbits(1.0f + uni() * 1e6f);
    default: return rnd();
    }
}

// ---- the original, loaded at 0x400000 ---------------------------------------------------------------------------------
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
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the arena -------------------------------------------------------------------------------------------------------
enum : uint32_t {
    A_HDR = 0x0,                // the harness's own state (bump pointers, call counters): compared, not footprinted
    A_OBJS = 0x1000,            // the fake DirectDraw / Direct3D objects (dd, d3d, device)
    A_TABLE = 0x2000,           // M1's 1000-entry texture table (0x9c40)
    A_ARGS = 0xc000,            // arguments: canvases, names, out slots, a scratch texture_info, a pixel format
    A_SCREEN = 0x14000,         // the screen VidGetPage hands out: 128 x 96 x 2
    A_CANV = 0x1a000,           // pixels for texTransfer / texAlphaBlit canvases, and texMapBits' private .tex
    A_HEAP = 0x60000,           // MemAlloc
    A_HEAP_END = 0x460000,
    A_COM = 0x460000,           // the fake surfaces and their pixels
    A_COM_END = 0x1860000,
    ARENA_BYTES = A_COM_END,
};
static uint8_t* g_arena;
static uint8_t* AR(uint32_t off) { return g_arena + off; }
static ULONG_PTR g_stack_lo, g_stack_hi;
static bool on_stack(uint32_t v) { return v >= g_stack_lo && v < g_stack_hi; }
static bool in_arena(const void* p, uint32_t n) { return (const uint8_t*)p >= g_arena && (const uint8_t*)p + n <= g_arena + ARENA_BYTES; }
struct Hdr {
    uint32_t heap, com;             // bump pointers (arena offsets)
    uint32_t n_alloc, n_create, n_lock, n_attach, n_qi, n_load, n_handle, n_key, n_blt, n_rs, n_res, n_page, n_ready;
    uint32_t next_handle;
};
static Hdr* hdr() { return (Hdr*)AR(A_HDR); }
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_pristine;

// ---- the log -----------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 16 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void lg(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void lg_str(const char* s, int max = 256) {
    char buf[260];
    int n = 0;
    __try { while (n < max && s[n]) { buf[n] = s[n]; n++; } } __except (EXCEPTION_EXECUTE_HANDLER) { lg(0xbadbad); }
    lg((uint32_t)n);
    for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, buf + i, n - i < 4 ? n - i : 4); lg(w); }
}
// the game's printf formats, logged: %s (bounded), %d / %x / %u / %c as their words
static void lg_fmt(const char* fmt, va_list ap) {
    lg((uint32_t)(uintptr_t)fmt);
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        while (*p && strchr("-+ #0123456789.l", *p)) p++;
        if (!*p) break;
        if (*p == '%') continue;
        if (*p == 's') lg_str(va_arg(ap, const char*));
        else lg(va_arg(ap, uint32_t));
    }
}

// ---- the script ------------------------------------------------------------------------------------------------------
struct Script {
    uint32_t alloc_null;       // bit k: the k-th MemAlloc of the call returns NULL
    uint32_t create_fail, create_err;   // CreateSurface
    uint32_t lock_fail, lock_err;       // Lock (err: 0 lost, 1 still drawing once, 2 other)
    uint32_t attach_err;       // GetAttachedSurface: another error than NOTFOUND
    uint32_t qi_fail;
    uint32_t load_fail, handle_fail, key_fail, blt_fail, rs_fail;
    uint32_t res_missing, res_badver;   // ResourceGetDiscardable
    uint8_t vidready, page_ok;
    uint8_t pitch_pad;         // extra bytes on every surface row
};
static Script g_script;
static Script g_quiet;         // no failures (set-up)

static bool bit(uint32_t mask, uint32_t& counter) { const uint32_t k = counter++; return k < 32 && ((mask >> k) & 1); }

static const DWORD PANIC_CODE = 0xE0474301, EXIT_CODE = 0xE0474302, ODD_CODE = 0xE0474303;

// ---- stubs ---------------------------------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) {
    lg('MALC'); lg((uint32_t)n);
    if (bit(g_script.alloc_null, hdr()->n_alloc)) { lg(0); return 0; }
    if (n < 0 || (uint32_t)n > A_HEAP_END - hdr()->heap - 16) { lg(1); return 0; }
    uint8_t* p = AR(hdr()->heap);
    hdr()->heap += ((uint32_t)n + 7) & ~7u;
    memset(p, 0xa3, (size_t)n);
    lg(hdr()->heap);
    return p;
}
static void __cdecl stub_MemFree(void* p) { lg('MFRE'); lg((uint32_t)(uintptr_t)p); }
static void __cdecl stub_delete(void* p) { lg('DELE'); lg((uint32_t)(uintptr_t)p); }
static void __cdecl stub_LogReport(const char* fmt, ...) { va_list ap; va_start(ap, fmt); lg('LREP'); lg_fmt(fmt, ap); va_end(ap); }
static void __cdecl stub_LogPanic(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); lg('PANC'); lg_fmt(fmt, ap); va_end(ap);
    RaiseException(PANIC_CODE, 0, 0, 0);
}
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    lg('SPRF'); lg_str(buf);
    return r;
}
static void __cdecl stub_assert(int c, const char* fmt, ...) { lg('ASRT'); lg((uint32_t)c); lg((uint32_t)(uintptr_t)fmt); }
static int __cdecl stub_SingleBegin(const char* name) { lg('SBEG'); lg_str(name); return 5; }
static void __cdecl stub_SingleEnter(int h, const char*, int) { lg('SENT'); lg((uint32_t)h); }
static void __cdecl stub_SingleLeave(int h, const char*, int) { lg('SLEV'); lg((uint32_t)h); }
static void __cdecl stub_SingleEnd(int h, const char*, int) { lg('SEND'); lg((uint32_t)h); }
static uint8_t __cdecl stub_vid_begin() { lg('VBEG'); return 1; }
static void __cdecl stub_gfx_begin() { lg('GBEG'); }
static void __cdecl stub_gfx_end() { lg('GEND'); }
static void __cdecl stub_vid_end() { lg('VEND'); }
static void __cdecl stub_ExceptSinglePrecision(uint8_t on) { lg('ESPR'); lg(on); }
static uint8_t __cdecl stub_VidSetMode(int mode, uint8_t flag) { lg('VSMD'); lg((uint32_t)mode); lg(flag); return 1; }
static void __cdecl stub_pal_build_system() { lg('PALB'); }
static void __cdecl stub_mr_set_mode() { lg('MRSM'); }
static void __cdecl stub_mr_restore_mode() { lg('MRRM'); }
static void __cdecl stub_VidRestoreMode() { lg('VRMD'); }
static void __cdecl stub_mr_release() { lg('MRRL'); }
static void __cdecl stub_mr_restore() { lg('MRRS'); }
static uint8_t __cdecl stub_VidReady() { lg('VRDY'); return g_script.vidready; }
static void __cdecl stub_VidFlip() { lg('VFLP'); }
static void __cdecl stub_VidReleasePage() { lg('VRPG'); }
typedef void(__cdecl* BuildCanvasFn)(void*, int8_t, void*, int, int, int);
static uint8_t __cdecl stub_VidGetPage(uint8_t* c) {
    lg('VGPG'); lg(on_stack((uint32_t)(uintptr_t)c) ? 'STAK' : (uint32_t)(uintptr_t)c);
    c[0] = 0; c[1] = 0; memset(c + 4, 0, 0x10);
    if (!g_script.page_ok) return 0;
    ((BuildCanvasFn)0x0044f880)(c, 4, AR(A_SCREEN), 128, 96, 256);
    return 1;
}
static void __cdecl stub_Win32Idle() { lg('IDLE'); RaiseException(ODD_CODE, 0, 0, 0); }
static void __stdcall stub_ExitProcess(uint32_t code) { lg('EXIT'); lg(code); RaiseException(EXIT_CODE, 0, 0, 0); }

// ---- the .tex resources ------------------------------------------------------------------------------------------------
struct TexRes { std::string name; uint8_t* data; int32_t size; uint32_t version; };
static std::vector<TexRes> g_tex;
static std::vector<std::pair<std::string, size_t>> g_alias;   // the fix tests' long names: resources of their own
static uint32_t name_hash(const char* s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)tolower((uint8_t)*s)) * 16777619u; return h; }
static const void* __cdecl stub_ResourceGetDiscardable(const char* name, uint32_t type, uint32_t* version, int32_t* size) {
    lg('RGET'); lg_str(name); lg(type);
    const bool missing = bit(g_script.res_missing, hdr()->n_res);
    const bool badver = ((g_script.res_badver >> (hdr()->n_res - 1)) & 1) != 0;
    if (missing) { lg(0); return 0; }
    const TexRes* t = 0;
    for (auto& r : g_tex) if (_stricmp(r.name.c_str(), name) == 0) { t = &r; break; }
    for (auto& a : g_alias) if (!t && _stricmp(a.first.c_str(), name) == 0) t = &g_tex[a.second];
    if (!t) {
        const uint32_t h = name_hash(name);
        if (h & 1) t = &g_tex[(h >> 1) % g_tex.size()];              // a made-up name: some texture, deterministically
    }
    if (!t) { lg(0); return 0; }
    *version = badver ? 2 : t->version;
    *size = t->size;
    lg((uint32_t)(t - &g_tex[0]));
    return t->data;
}
static uint8_t __cdecl stub_ResourceForget(void* p) {
    lg('RFGT');
    int k = -1;
    for (size_t i = 0; i < g_tex.size(); i++) if (g_tex[i].data == p) k = (int)i;
    lg((uint32_t)k); lg((uint32_t)(uintptr_t)p);
    return 1;
}

// ---- fake DirectDraw / Direct3D ----------------------------------------------------------------------------------------
struct FSurf {                  // a surface (in the arena); the IDirect3DTexture2 interface at +4
    void** vtbl;
    void** tex_vtbl;
    int32_t refs, tex_refs;
    int32_t w, h, pitch, bpp;
    uint8_t* pixels;
    FSurf* next;                // the attached mip
    int32_t locked;
    uint32_t caps, ck_lo, ck_hi, ck_flags;
    uint32_t handle;
};
static FSurf* from_tex(void* t) { return (FSurf*)((uint8_t*)t - 4); }
static uint32_t off_of(const void* p) { return in_arena(p, 1) ? (uint32_t)((const uint8_t*)p - g_arena) : (uint32_t)(uintptr_t)p; }
static const uint32_t IID_SURF3 = 0x004dcba0, IID_TEX2 = 0x004dcc70;
enum : uint32_t { DDERR_NOTFOUND = 0x887600ff, DDERR_SURFACELOST = 0x887601c2, DDERR_WASSTILLDRAWING = 0x8876021c,
                  DDERR_OUTOFVIDEOMEMORY = 0x8876017c, DDERR_GENERIC = 0x80004005, E_NOINTF = 0x80004002, DDERR_ODD = 0x88760123 };

static HRESULT __stdcall s_QI(FSurf* s, const void* iid, void** out) {
    lg('SQI_'); lg(off_of(s)); lg((uint32_t)(uintptr_t)iid);
    if (bit(g_script.qi_fail, hdr()->n_qi)) { *out = 0; lg(E_NOINTF); return (HRESULT)E_NOINTF; }
    if ((uint32_t)(uintptr_t)iid == IID_SURF3) { s->refs++; *out = s; return 0; }
    if ((uint32_t)(uintptr_t)iid == IID_TEX2) { s->tex_refs++; *out = &s->tex_vtbl; return 0; }
    *out = 0;
    return (HRESULT)E_NOINTF;
}
static ULONG __stdcall s_AddRef(FSurf* s) { lg('SADR'); lg(off_of(s)); return (ULONG)++s->refs; }
static ULONG __stdcall s_Release(FSurf* s) { lg('SREL'); lg(off_of(s)); return (ULONG)--s->refs; }
static HRESULT __stdcall s_Blt(FSurf* s, RECT* dr, FSurf* src, RECT* sr, DWORD flags, void* fx) {
    lg('SBLT'); lg(off_of(s)); lg(off_of(src)); lg(flags); lg(off_of(dr)); lg(off_of(sr)); lg(off_of(fx));
    if (bit(g_script.blt_fail, hdr()->n_blt)) return (HRESULT)DDERR_ODD;
    return 0;
}
static HRESULT __stdcall s_BltFast(FSurf* s, DWORD x, DWORD y, FSurf* src, RECT* r, DWORD flags) {
    lg('SBFT'); lg(off_of(s)); lg(x); lg(y); lg(off_of(src)); lg(flags);
    if (r) { lg((uint32_t)r->left); lg((uint32_t)r->top); lg((uint32_t)r->right); lg((uint32_t)r->bottom); }
    if (bit(g_script.blt_fail, hdr()->n_blt)) return (HRESULT)DDERR_ODD;
    return 0;
}
static HRESULT __stdcall s_GetAttached(FSurf* s, uint32_t* caps, FSurf** out) {
    lg('SGAT'); lg(off_of(s)); lg(caps ? caps[0] : 0xdead);
    if (bit(g_script.attach_err, hdr()->n_attach)) { lg(DDERR_ODD); return (HRESULT)DDERR_ODD; }
    if (!s->next) { lg(DDERR_NOTFOUND); return (HRESULT)DDERR_NOTFOUND; }
    s->next->refs++;
    *out = s->next;
    lg(off_of(s->next));
    return 0;
}
static HRESULT __stdcall s_Lock(FSurf* s, RECT* r, uint32_t* d, DWORD flags, HANDLE ev) {
    lg('SLCK'); lg(off_of(s)); lg(off_of(r)); lg(flags); lg(d ? d[0] : 0xdead);
    if (bit(g_script.lock_fail, hdr()->n_lock)) {
        const uint32_t e = g_script.lock_err % 3;
        if (e == 1 && !(hdr()->n_lock & 0x80000000u)) {              // still drawing, once: the wrapper tries again
            hdr()->n_lock |= 0x80000000u;
            lg(DDERR_WASSTILLDRAWING);
            return (HRESULT)DDERR_WASSTILLDRAWING;
        }
        if (e != 1) { lg(e == 0 ? DDERR_SURFACELOST : DDERR_ODD); return (HRESULT)(e == 0 ? DDERR_SURFACELOST : DDERR_ODD); }
    }
    hdr()->n_lock &= 0x7fffffffu;
    s->locked++;
    d[1] = 0x100f;
    d[2] = (uint32_t)s->h;
    d[3] = (uint32_t)s->w;
    d[4] = (uint32_t)s->pitch;
    d[9] = (uint32_t)(uintptr_t)s->pixels;
    return 0;
}
static HRESULT __stdcall s_SetColorKey(FSurf* s, DWORD flags, uint32_t* key) {
    lg('SKEY'); lg(off_of(s)); lg(flags); lg(key[0]); lg(key[1]);
    if (bit(g_script.key_fail, hdr()->n_key)) return (HRESULT)DDERR_ODD;
    s->ck_flags = flags; s->ck_lo = key[0]; s->ck_hi = key[1];
    return 0;
}
static HRESULT __stdcall s_Unlock(FSurf* s, void* p) { lg('SULK'); lg(off_of(s)); lg(off_of(p)); s->locked--; return 0; }
static HRESULT __stdcall t_QI(void* t, const void* iid, void** out) { lg('TQI_'); lg(off_of(t)); *out = 0; return (HRESULT)E_NOINTF; }
static ULONG __stdcall t_AddRef(void* t) { lg('TADR'); lg(off_of(t)); return (ULONG)++from_tex(t)->tex_refs; }
static ULONG __stdcall t_Release(void* t) { lg('TREL'); lg(off_of(t)); return (ULONG)--from_tex(t)->tex_refs; }
static HRESULT __stdcall t_GetHandle(void* t, void* dev, uint32_t* h) {
    lg('TGHD'); lg(off_of(t)); lg(off_of(dev));
    if (bit(g_script.handle_fail, hdr()->n_handle)) return (HRESULT)DDERR_ODD;
    FSurf* s = from_tex(t);
    if (!s->handle) s->handle = ++hdr()->next_handle;
    *h = s->handle;
    return 0;
}
static HRESULT __stdcall t_Load(void* t, void* src) {
    lg('TLOD'); lg(off_of(t)); lg(off_of(src));
    if (bit(g_script.load_fail, hdr()->n_load)) return (HRESULT)DDERR_ODD;
    return 0;
}
static void odd_slot() { lg('ODDS'); RaiseException(ODD_CODE, 0, 0, 0); }
static void* g_surf_vtbl[40];
static void* g_tex_vtbl[8];

// the device: EnumTextureFormats offers the world's formats; SetRenderState is logged
enum { NFMT = 12 };
static uint32_t g_formats[NFMT][27];
static int g_nformats;
typedef HRESULT(__stdcall* EnumCb_t)(uint32_t*, void*);
static HRESULT __stdcall d_Enum(void* dev, EnumCb_t cb, void* ctx) {
    lg('DENM'); lg(off_of(dev)); lg((uint32_t)(uintptr_t)cb);
    for (int i = 0; i < g_nformats; i++) {
        uint32_t d[27];
        memcpy(d, g_formats[i], sizeof d);
        HRESULT r = cb(d, ctx);
        lg((uint32_t)r);
        if (!r) break;
    }
    return 0;
}
static HRESULT __stdcall d_SetRenderState(void* dev, uint32_t st, uint32_t v) {
    lg('DRST'); lg(st); lg(v);
    if (bit(g_script.rs_fail, hdr()->n_rs)) return (HRESULT)DDERR_ODD;
    return 0;
}
static void* g_dev_vtbl[40];

// IDirectDraw2::CreateSurface: a mip chain of fake surfaces, pixels filled with a pattern from their address
static FSurf* new_surf(int w, int h, int bpp, uint32_t caps) {
    const uint32_t bytes = (uint32_t)(bpp / 8);
    const int pitch = (int)(((uint32_t)w * bytes + 7) & ~7u) + g_script.pitch_pad;
    const uint32_t need = (uint32_t)sizeof(FSurf) + (uint32_t)pitch * (uint32_t)h + 16;
    if (hdr()->com + need > A_COM_END) { lg('COM!'); RaiseException(ODD_CODE, 0, 0, 0); }
    FSurf* s = (FSurf*)AR(hdr()->com);
    memset(s, 0, sizeof *s);
    s->vtbl = g_surf_vtbl;
    s->tex_vtbl = g_tex_vtbl;
    s->refs = 1;
    s->w = w; s->h = h; s->pitch = pitch; s->bpp = bpp;
    s->caps = caps;
    s->pixels = (uint8_t*)s + sizeof(FSurf);
    uint32_t x = hdr()->com * 2654435761u | 1;
    for (uint32_t i = 0; i < (uint32_t)pitch * (uint32_t)h; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->pixels[i] = (uint8_t)x; }
    hdr()->com += (need + 15) & ~15u;
    return s;
}
static HRESULT __stdcall dd_CreateSurface(void* dd, uint32_t* d, FSurf** out, void* outer) {
    lg('DCSF'); lg(d[1]); lg(d[2]); lg(d[3]); lg(d[6]); lg(d[16]); lg(d[17]); lg(d[26]);
    for (int i = 18; i < 26; i++) lg(d[i]);
    if (bit(g_script.create_fail, hdr()->n_create)) {
        const uint32_t e = (g_script.create_err & 1) ? DDERR_OUTOFVIDEOMEMORY : DDERR_GENERIC;
        lg(e);
        return (HRESULT)e;
    }
    int levels = 1;
    if ((d[1] & 0x20000) && (d[26] & 0x8)) levels = (int)d[6];
    if (levels < 1 || levels > 12) levels = 1;
    int w = (int)d[3], h = (int)d[2];
    if (w < 1 || w > 4096 || h < 1 || h > 4096) { lg('BADS'); return (HRESULT)DDERR_GENERIC; }
    const int bpp = d[21] ? (int)d[21] : 16;
    FSurf* root = new_surf(w, h, bpp, d[26]);
    FSurf* prev = root;
    for (int i = 1; i < levels; i++) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        FSurf* s = new_surf(w, h, bpp, d[26]);
        prev->next = s;
        prev = s;
    }
    *out = root;
    lg(off_of(root));
    return 0;
}
static void* g_dd_vtbl[40];

// the objects the game's statics point at: ddraw { IDirectDraw2* }, IDirectDraw2 { vtbl }, d3d { ?, device },
// the device { vtbl }
static void setup_objects() {
    uint32_t* o = (uint32_t*)AR(A_OBJS);
    o[0] = (uint32_t)(uintptr_t)(o + 4);            // ddraw.dd2 -> +0x10
    o[4] = (uint32_t)(uintptr_t)g_dd_vtbl;          // IDirectDraw2
    o[8] = 0;                                       // d3d +0x20: +0 ?, +4 device
    o[9] = (uint32_t)(uintptr_t)(o + 12);
    o[12] = (uint32_t)(uintptr_t)g_dev_vtbl;        // the device at +0x30
    G32(0x004f1e88) = (uint32_t)(uintptr_t)o;       // dd
    G32(0x00522e64) = (uint32_t)(uintptr_t)(o + 8); // d3d
    G32(0x004f3a60) = (uint32_t)(uintptr_t)(o + 12);// the IDirect3DDevice2
}
static void init_vtables() {
    for (auto& v : g_surf_vtbl) v = (void*)&odd_slot;
    for (auto& v : g_tex_vtbl) v = (void*)&odd_slot;
    for (auto& v : g_dev_vtbl) v = (void*)&odd_slot;
    for (auto& v : g_dd_vtbl) v = (void*)&odd_slot;
    g_surf_vtbl[0] = (void*)&s_QI; g_surf_vtbl[1] = (void*)&s_AddRef; g_surf_vtbl[2] = (void*)&s_Release;
    g_surf_vtbl[5] = (void*)&s_Blt; g_surf_vtbl[7] = (void*)&s_BltFast; g_surf_vtbl[12] = (void*)&s_GetAttached;
    g_surf_vtbl[25] = (void*)&s_Lock; g_surf_vtbl[29] = (void*)&s_SetColorKey; g_surf_vtbl[32] = (void*)&s_Unlock;
    g_tex_vtbl[0] = (void*)&t_QI; g_tex_vtbl[1] = (void*)&t_AddRef; g_tex_vtbl[2] = (void*)&t_Release;
    g_tex_vtbl[3] = (void*)&t_GetHandle; g_tex_vtbl[5] = (void*)&t_Load;
    g_dev_vtbl[9] = (void*)&d_Enum; g_dev_vtbl[23] = (void*)&d_SetRenderState;
    g_dd_vtbl[6] = (void*)&dd_CreateSurface;
}

// the pixel formats a world offers (DDSURFACEDESC with DDSD_PIXELFORMAT)
static void make_format(uint32_t* d, uint32_t flags, uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    memset(d, 0, 27 * 4);
    d[0] = 0x6c; d[1] = 0x1000;
    d[18] = 0x20; d[19] = flags; d[21] = bits; d[22] = r; d[23] = g; d[24] = b; d[25] = a;
}
static void random_formats() {
    g_nformats = 0;
    auto add = [&](uint32_t fl, uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        if (g_nformats < NFMT) make_format(g_formats[g_nformats++], fl, bits, r, g, b, a);
    };
    if (chance(20)) add(0x20 | 0x40, 8, 0, 0, 0, 0);                             // palettised
    if (chance(93)) add(0x40, 16, 0xf800, 0x7e0, 0x1f, 0);                      // 565
    if (chance(50)) add(0x40, 16, 0x7c00, 0x3e0, 0x1f, 0);                      // x555
    if (chance(70)) add(0x41, 16, 0x7c00, 0x3e0, 0x1f, 0x8000);                 // 1555
    if (chance(70)) add(0x41, 16, 0xf00, 0xf0, 0xf, 0xf000);                    // 4444
    if (chance(30)) add(0x41, 32, 0xff0000, 0xff00, 0xff, 0xff000000u);         // 8888
    if (chance(15)) add(0x400, 16, 0, 0, 0, 0);                                 // z
    if (chance(10)) add(0x40, 24, 0xff0000, 0xff00, 0xff, 0);                   // 888
    if (chance(20)) { add(0x40, 16, 0xf800, 0x7e0, 0x1f, 0); g_formats[g_nformats - 1][1] = 0; }   // no DDSD_PIXELFORMAT
    for (int i = 0; i < g_nformats; i++) {                                      // shuffle
        int j = ri(0, g_nformats - 1);
        uint32_t t[27];
        memcpy(t, g_formats[i], sizeof t); memcpy(g_formats[i], g_formats[j], sizeof t); memcpy(g_formats[j], t, sizeof t);
    }
    if (chance(1)) g_nformats = 0;                                              // nothing (ExitProcess)
}

// the game's CRT must never put up a modal box
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d)\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- M1 ---------------------------------------------------------------------------------------------------------------
struct TtRef { uint32_t at; uint8_t off; uint32_t old; int end; };
static const TtRef k_tt_refs[] = {
#include "../hook/texture_table_fields.inc"
};
struct Patch32 { uint32_t at, old, neu; };
static std::vector<Patch32> g_m1;
static bool g_m1_on;
static void build_m1() {
    const uint32_t base = (uint32_t)(uintptr_t)AR(A_TABLE), end = base + 1000 * 0x28;
    for (auto& r : k_tt_refs)
        g_m1.push_back({r.at + r.off, r.old, r.end ? end + (r.old - 0x5256b0) : base + (r.old - 0x522fa0)});
    g_m1.push_back({0x0045a60c, 0xfa, 1000});        // tc_add: cmp edx, 250
    g_m1.push_back({0x0045a6dd, 0xfa, 1000});        // tc_lookup: cmp ebx, 250
    g_m1.push_back({0x0045fbeb, 200, 1024});         // txBegin: the sys pool
    g_m1.push_back({0x0045fc26, 0x80, 1024});        // txBegin: the vid pool
}
static void set_m1(bool on) {
    for (auto& p : g_m1) {
        uint32_t* q = (uint32_t*)(uintptr_t)p.at;
        if (*q != (on ? p.old : p.neu) && *q != (on ? p.neu : p.old)) { printf("M1 operand %08x isn't what it should be (%08x)\n", p.at, *q); ExitProcess(2); }
        *q = on ? p.neu : p.old;
    }
    g_m1_on = on;
}
static uint8_t* table_base() { return g_m1_on ? AR(A_TABLE) : (uint8_t*)0x00522fa0; }
static int table_cap() { return g_m1_on ? 1000 : 250; }
static TexEntry* entry(int i) { return (TexEntry*)(table_base() + (uint32_t)i * 0x28u); }

// ---- chain mode: every rewrite of the group patched in -----------------------------------------------------------------
struct Saved { uint32_t at; uint8_t b[5]; };
static std::vector<Saved> g_chain;
static void chain_on() {
    g_chain.clear();
    for (Reg* r = Reg::head(); r; r = r->next) {
        Saved s; s.at = r->at; memcpy(s.b, (void*)(uintptr_t)r->at, 5);
        g_chain.push_back(s);
        patch_jmp(r->at, r->fn);
    }
}
static void chain_off() { for (auto& s : g_chain) memcpy((void*)(uintptr_t)s.at, s.b, 5); g_chain.clear(); }

// ---- the world ---------------------------------------------------------------------------------------------------------
typedef void(__cdecl* V_t)();
static bool g_setup_ok;
static std::vector<std::string> g_names;        // names that went into TextureGet / Create
static const char* const k_odd_names[] = {
    "nosuch.tex", "missing1.TEX", "noext", "wall.bmp", "WALL.BMP", "logo.stp", "LOGO.STP", "effects.tex", "effectsx.tex",
    "=grey.tex", "=sky1.tex", "", "a.tex", "abcdefghijk.tex", "abcdefghijkl.tex", "abcdefghijklm.tex",
    "abcdefghijklmnop.tex", "abcdefghijklmnopq.tex", "abcdefghijklmnopqrst.tex", "abcdefghijklmnopqrstuvw.tex",
    "abcdefghijklmnopqrstuvwxyz0123.tex", "a_very_long_texture_name_indeed_x.tex",
};
static std::string pick_name_any();
static std::string pick_name() {
    for (;;) {
        std::string s = pick_name_any();
        if (!k_ordinary || s.size() <= 15) return s;
    }
}
static std::string pick_name_any() {
    if (!g_tex.empty() && chance(70)) {
        std::string s = g_tex[rnd() % g_tex.size()].name;
        if (chance(10)) for (char& c : s) c = (char)toupper((uint8_t)c);
        return s;
    }
    if (!g_names.empty() && chance(40)) return g_names[rnd() % g_names.size()];
    if (chance(70)) return k_odd_names[rnd() % (sizeof k_odd_names / sizeof k_odd_names[0])];
    std::string s;
    int n = ri(1, 40);
    for (int i = 0; i < n; i++) s += "abcdefghijklmnopqrstuvwxyz0123456789_"[rnd() % 37];
    if (chance(80)) s += ".tex";
    return s;
}
static int pick_key() { return chance(60) ? 0 : chance(50) ? ri(1, 9) : chance(50) ? 0x20 + ri(0, 8) : (int)rnd(); }
static char* put_name(const std::string& s, uint32_t slot) {    // A_ARGS + 0x400 + slot * 0x40
    char* p = (char*)AR(A_ARGS + 0x400 + slot * 0x40);
    memset(p, 0, 0x40);
    memcpy(p, s.c_str(), s.size() < 0x3f ? s.size() : 0x3f);
    return p;
}

typedef int(__cdecl* TG_t)(const char*, int);
typedef int(__cdecl* TC_t)(const char*, int, int);
typedef uint8_t(__cdecl* TS_t)(int);
typedef void(__cdecl* TI_t)(int);
typedef uint8_t(__cdecl* TB_t)(int, int, int);
typedef uint8_t(__cdecl* TGR_t)(int, void*);

static int random_id(bool in_use_mostly = true) {
    const int cap = table_cap();
    std::vector<int> used;
    for (int i = 0; i < cap; i++) if (entry(i)->in_use) used.push_back(i);
    if (in_use_mostly && !used.empty() && chance(80)) return used[rnd() % used.size()];
    if (chance(40)) return -1;
    if (chance(60)) return ri(0, cap - 1);
    return chance(50) ? cap + ri(0, 3) : -2 - ri(0, 3);
}
static TexInfo* random_info(bool vid) {
    std::vector<TexInfo*> v;
    for (TexInfo* t = GP(TexInfo, vid ? X_VID_LIST : X_SYS_LIST); t && v.size() < 64; t = t->next) v.push_back(t);
    if (!v.empty() && chance(92)) return v[rnd() % v.size()];
    std::vector<TexInfo*> o;
    for (TexInfo* t = GP(TexInfo, vid ? X_SYS_LIST : X_VID_LIST); t && o.size() < 64; t = t->next) o.push_back(t);
    if (!o.empty() && chance(70)) return o[rnd() % o.size()];
    return v.empty() ? 0 : v[0];
}
// a real texture with at least `size` texels across
static const TexRes* tex_at_least(int size, int fmt = -1) {
    std::vector<const TexRes*> v;
    for (auto& t : g_tex) {
        const int n = *(int32_t*)(t.data + 8);
        if (n < 1 || n > 9 || (1 << (n - 1)) < size) continue;
        if (fmt >= 0 && t.data[0] != fmt) continue;
        v.push_back(&t);
    }
    if (v.empty()) return 0;
    return v[rnd() % v.size()];
}

static uint32_t g_fault_eip, g_fault_code, g_fault_addr;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_addr = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
struct SetupOp { char* name; int key; bool create; int size; bool again, select; };
static bool run_setup(const SetupOp* ops, int n, bool forget, bool deres, int conserve, int base, bool grab) {
    unsigned cw;
    bool ok = true;
    __asm fninit
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    __try {
        ((TB_t)0x00459f80)(0, 0, 0);                        // TextureBegin
        for (int k = 0; k < n; k++) {
            __try {
                int id;
                if (ops[k].create) id = ((TC_t)0x0045a170)(ops[k].name, ops[k].key, ops[k].size);   // TextureCreate
                else id = ((TG_t)0x0045a0c0)(ops[k].name, ops[k].key);                                // TextureGet
                if (ops[k].again) ((TG_t)0x0045a0c0)(ops[k].name, ops[k].key);                        // a second reference
                if (ops[k].select && id != -1) ((TS_t)0x0045a4e0)(id);                                // TextureSelect
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        __try {
            if (forget) ((TI_t)0x0045a110)(random_id());                 // TextureForget
            if (deres) G8(X_DERES) = 1;
            if (conserve >= 0) GI(X_CONSERVE) = conserve;
            if (base >= 0) GI(X_CONSERVE_BASE) = base;
            if (grab) ((TGR_t)0x0045a300)(random_id(), AR(A_ARGS + 0x100));   // a grab left pending
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } __except (fault_filter(GetExceptionInformation())) { ok = false; }
    __asm fninit
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    return ok;
}
static void setup_world() {
    memcpy(DATA, g_data_pristine, DATA_BYTES);
    memset(AR(0), 0, A_HEAP);
    hdr()->heap = A_HEAP;
    hdr()->com = A_COM;
    g_names.clear();
    set_m1(chance(50));
    setup_objects();
    GI(0x005228d0) = 5;                                    // gx_sync
    GI(0x00522a1c) = ri(0, 2);                             // mrCaps.alpha
    G8(0x00522a21) = chance(70);                           // hardware mips
    G8(0x005229ed) = chance(80);                           // the mip feature
    GI(0x00522ad4) = chance(30) ? 4 : chance(50) ? 8 : 16; // VRAM (MB)
    G8(0x004f1e9c) = chance(85);                           // 3D hardware
    G8(0x00522ae8) = chance(15);                           // 3Dfx
    GI(0x00522ac8) = ri(0, 4);                             // VidReady's state
    GI(0x004eefa8) = ri(0, 4);                             // the mode
    random_formats();
    g_script = g_quiet;
    g_script.pitch_pad = chance(30) ? (uint8_t)(ri(1, 4) * 8) : 0;
    if (chance(10)) g_script.res_missing = rnd() & rnd();
    if (chance(5)) g_script.create_fail = rnd() & rnd() & rnd();
    SetupOp ops[12];
    const int n = ri(0, 12);
    for (int k = 0; k < n; k++) {
        std::string nm = pick_name();
        g_names.push_back(nm);
        ops[k].name = put_name(nm, (uint32_t)k + 2);
        ops[k].key = pick_key();
        ops[k].create = chance(12);
        ops[k].size = chance(80) ? (64 << ri(0, 2)) : ri(0, 300);
        ops[k].again = chance(20);
        ops[k].select = chance(50);
    }
    const bool forget = chance(15), deres = chance(10), grab = chance(15);
    const int conserve = chance(15) ? ri(0, 3) : -1, base = chance(10) ? ri(0, 2) : -1;
    g_setup_ok = run_setup(ops, n, forget, deres, conserve, base, grab);
    if (g_setup_ok && chance(6)) {                          // a (nearly) full table: tc_add's walk to the end, its panic
        const int cap = table_cap();
        const int keep = chance(50) ? 0 : ri(1, 3);
        int kept = 0;
        for (int i = cap - 1; i >= 0; i--) {
            TexEntry* e = entry(i);
            if (e->in_use) continue;
            if (kept < keep && chance(30)) { kept++; continue; }
            memset(e, 0, sizeof *e);
            e->in_use = 1;
            e->refs = 1;
            sprintf(e->name, "fill%03d.tex", i);
        }
    }
    Hdr* h = hdr();                                       // the call's own counters start at 0
    h->n_alloc = h->n_create = h->n_lock = h->n_attach = h->n_qi = h->n_load = h->n_handle = h->n_key = h->n_blt = 0;
    h->n_rs = h->n_res = h->n_page = h->n_ready = 0;
}

// ---- arguments ---------------------------------------------------------------------------------------------------------
static uint32_t g_args[8];
static const char* g_kind_note;
static uint8_t* canvas_arg(uint32_t slot) { uint8_t* c = AR(A_ARGS + 0x100 + slot * 0x40); memset(c, 0x5c, 0x24); return c; }
// a canvas over arena pixels for texTransfer / texAlphaBlit
static uint8_t* pixel_canvas(int w, int8_t type) {
    uint8_t* c = AR(A_ARGS + 0x200);
    memset(c, 0, 0x24);
    const int bpp = type == 5 ? 4 : 2;
    int pitch = w * bpp + (chance(30) ? 8 * ri(1, 3) : 0);
    int h = w;
    if (chance(10)) h = ri(1, w);
    uint8_t* bits = AR(A_CANV);
    for (int i = 0; i < pitch * h; i++) bits[i] = (uint8_t)rnd();
    ((BuildCanvasFn)0x0044f880)(c, type, bits, w, h, pitch);
    return c;
}
static int8_t canvas_type() { const int t = ri(0, 9); return (int8_t)(t < 4 ? 4 : t < 6 ? 3 : t < 8 ? 1 : t == 8 ? 5 : ri(0, 2)); }
static void float_args(uint32_t* a) {
    float u0 = uni(), v0 = uni(), u1 = uni(), v1 = uni();
    if (chance(20)) { u0 = (float)ri(0, 15) / 16.0f; v0 = (float)ri(0, 15) / 16.0f; u1 = u0 + 1.0f / 16.0f; v1 = v0 + 1.0f / 16.0f; }
    a[0] = fbits(u0); a[1] = fbits(v0); a[2] = fbits(u1); a[3] = fbits(v1);
    if (chance(8)) a[ri(0, 3)] = wildf_bits();
    if (chance(5)) a[0] = a[2];
}
// a sys texture_info with a .tex payload to load from (load_texmap_mip / load_texmap)
static TexInfo* info_with_tt(bool vid) {
    TexInfo* t = random_info(vid);
    if (!t) return 0;
    const int size = t->size >= 1 && t->size <= 256 ? t->size : 256;
    if (const TexRes* r = tex_at_least(size)) {
        uint8_t* tt = r->data;
        if (tt[0] == 3 && chance(50)) tt += r->size / 2;
        if (chance(90)) t->tt = tt;
    }
    return t;
}
static const TexRes* random_tt_res() { return g_tex.empty() ? 0 : &g_tex[rnd() % g_tex.size()]; }

static void grab_first() {
    const int id = random_id();
    __try { ((TGR_t)0x0045a300)(id, AR(A_ARGS + 0x100)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __asm fninit
}
static void gen_args(Reg* r) {
    uint32_t* a = g_args;
    memset(a, 0, sizeof g_args);
    g_kind_note = "";
    switch (r->at) {
    case 0x0044df20: case 0x0044e040:                      // gxSetMode / gxChangeMode
        a[0] = chance(30) ? (uint32_t)GI(0x004eefa8) : (uint32_t)ri(-1, 6);
        break;
    case 0x0044e1c0: a[0] = (uint32_t)(uintptr_t)canvas_arg(0); break;   // gxGrabScreen
    case 0x0044e240: case 0x0045a0c0: case 0x0045a6a0:      // gxGetTexture / TextureGet / tc_lookup
        a[0] = (uint32_t)(uintptr_t)put_name(pick_name(), 1);
        a[1] = (uint32_t)pick_key();
        if (chance(40) && table_cap()) {                    // an entry's own name and key
            const int id = random_id();
            if (id >= 0 && id < table_cap()) {
                memcpy((void*)(uintptr_t)a[0], entry(id)->name, 0x3f);
                ((char*)(uintptr_t)a[0])[0x3f] = 0;
                a[1] = (uint32_t)entry(id)->key;
            }
        }
        break;
    case 0x0045a5d0:                                        // tc_add
        a[0] = (uint32_t)(uintptr_t)put_name(pick_name(), 1);
        a[1] = (uint32_t)pick_key();
        a[2] = (uint32_t)(uintptr_t)AR(A_ARGS + 0x80);
        if (chance(30)) {
            const int id = random_id();
            if (id >= 0 && id < table_cap()) { memcpy((void*)(uintptr_t)a[0], entry(id)->name, 0x3f); a[1] = (uint32_t)entry(id)->key; }
        }
        break;
    case 0x0044e2c0: case 0x0045a170:                       // gxCreateTexture / TextureCreate
        a[0] = (uint32_t)(uintptr_t)put_name(pick_name(), 1);
        a[1] = (uint32_t)pick_key();
        a[2] = chance(85) ? (uint32_t)(64 << ri(0, 2)) : (uint32_t)ri(0, 300);
        break;
    case 0x0044e380: case 0x0045a240:                       // gxBlitTexture / TextureBlit
        a[0] = (uint32_t)random_id(); a[1] = (uint32_t)random_id();
        float_args(a + 2);
        break;
    case 0x0044e3d0: case 0x0045a290:                       // gxAlphaBlitTexture / TextureAlphaBlit
        a[0] = (uint32_t)(uintptr_t)put_name(chance(70) && tex_at_least(1, 2) ? tex_at_least(1, 2)->name : pick_name(), 1);
        a[1] = (uint32_t)random_id();
        break;
    case 0x0044e490: case 0x0045a300:                       // gxGrabTexture / TextureGrab
        a[0] = (uint32_t)random_id(); a[1] = (uint32_t)(uintptr_t)canvas_arg(0);
        break;
    case 0x0044e280: case 0x0044e310: case 0x0044e410: case 0x0044e450: case 0x0044e500: case 0x0044e540:
    case 0x0045a110: case 0x0045a1d0: case 0x0045a2c0: case 0x0045a2f0: case 0x0045a390: case 0x0045a3e0:
    case 0x0045a4e0: case 0x0045a550: case 0x0045a590: case 0x0045a6f0:
        a[0] = (uint32_t)random_id();
        break;
    case 0x0045a050: case 0x0045a090: case 0x0045fcb0:      // TextureReleaseAll / RestoreAll / txConserveMode
        a[0] = chance(50) ? 0 : chance(80) ? 1 : (rnd() & 0xff);
        break;
    case 0x0045fd10:                                        // txLoadSystem
        a[0] = (uint32_t)(uintptr_t)put_name(pick_name(), 1);
        a[1] = (uint32_t)(uintptr_t)AR(A_ARGS + 0x80);
        break;
    case 0x0045ff30:                                        // txCreateSystem
        a[0] = (uint32_t)(uintptr_t)AR(A_ARGS + 0x80);
        a[1] = chance(85) ? (uint32_t)(64 << ri(0, 2)) : (uint32_t)ri(0, 300);
        break;
    case 0x0045ffe0:                                        // txLoadVideo
        a[0] = (uint32_t)(uintptr_t)random_info(false); a[1] = (uint32_t)(uintptr_t)AR(A_ARGS + 0x80);
        if (!a[0]) g_kind_note = "no texture";
        break;
    case 0x004600a0: case 0x00460540: case 0x00460600: case 0x00460630: case 0x004606a0: case 0x00460520:
        a[0] = (uint32_t)(uintptr_t)random_info(chance(20));
        if (r->at == 0x00460540) a[1] = (uint32_t)(uintptr_t)canvas_arg(0);
        if (r->at == 0x00460520 && chance(20)) {           // destroy_texture_surface: a texture_info without a surface
            TexInfo* t = (TexInfo*)AR(A_ARGS + 0x40);
            memset(t, 0x77, sizeof *t);
            t->dtex = 0;
            a[0] = (uint32_t)(uintptr_t)t;
        }
        if (!a[0]) g_kind_note = "no texture";
        break;
    case 0x00460110: case 0x004601d0: case 0x00460690:      // txUnloadVideo / txSelect / unload_from_video
        a[0] = (uint32_t)(uintptr_t)random_info(chance(85));
        if (r->at == 0x004601d0 && a[0] && chance(30)) ((TexInfo*)(uintptr_t)a[0])->wrap = (uint8_t)(chance(80) ? ri(0, 3) : ri(4, 255));
        if (!a[0]) g_kind_note = "no texture";
        break;
    case 0x00460180: case 0x00460660:                       // txReloadVideo / load_into_video
        a[0] = (uint32_t)(uintptr_t)random_info(false); a[1] = (uint32_t)(uintptr_t)random_info(true);
        if (!a[0] || !a[1]) g_kind_note = "no texture";
        break;
    case 0x004601c0:                                        // txRevert
        a[0] = (uint32_t)(uintptr_t)put_name(pick_name(), 1); a[1] = (uint32_t)(uintptr_t)random_info(false);
        break;
    case 0x00460240: {                                      // txBlit
        a[0] = (uint32_t)(uintptr_t)random_info(false); a[1] = (uint32_t)(uintptr_t)random_info(false);
        float_args(a + 2);
        if (!a[0] || !a[1]) g_kind_note = "no texture";
        break;
    }
    case 0x004603b0: {                                      // create_texture_surface
        TexInfo* t = (TexInfo*)AR(A_ARGS + 0x40);
        memset(t, 0, sizeof *t);
        t->tfmt = (uint8_t)(chance(80) ? ri(0, 4) : rnd());
        t->key = (uint16_t)rnd();
        a[0] = chance(90) ? (uint32_t)(1 << ri(0, 8)) : (uint32_t)ri(0, 300);
        a[1] = (uint32_t)ri(1, 9);
        a[2] = (uint32_t)(chance(90) ? ri(0, 2) : ri(0, 5));
        a[3] = chance(50); a[4] = chance(50);
        a[5] = (uint32_t)(uintptr_t)t;
        break;
    }
    case 0x00460710: {                                      // load_texmap
        a[0] = (uint32_t)(uintptr_t)put_name(pick_name(), 1);
        a[1] = (uint32_t)(uintptr_t)info_with_tt(false);
        if (!a[1]) g_kind_note = "no texture";
        break;
    }
    case 0x00460820: a[0] = (uint32_t)(uintptr_t)info_with_tt(chance(15)); if (!a[0]) g_kind_note = "no texture"; break;
    case 0x00460980:                                        // txAlphaBlit
        a[0] = (uint32_t)(uintptr_t)put_name(chance(70) && tex_at_least(1, 2) ? tex_at_least(1, 2)->name : pick_name(), 1);
        a[1] = (uint32_t)(uintptr_t)random_info(false);
        if (!a[1]) g_kind_note = "no texture";
        break;
    case 0x004609b0: {                                      // alpha_blit_texmap
        TexInfo* t = random_info(false);
        a[0] = (uint32_t)(uintptr_t)t;
        const TexRes* res = t ? tex_at_least(t->size >= 1 && t->size <= 256 ? t->size : 1, chance(80) ? 2 : -1) : 0;
        a[1] = res ? (uint32_t)(uintptr_t)res->data : 0x00526770;
        if (!t) g_kind_note = "no texture";
        break;
    }
    case 0x00460b20: {                                      // dump_texture_format: at most 4 flags (the 0x50 buffer)
        uint32_t* pf = (uint32_t*)AR(A_ARGS + 0x300);
        static const uint32_t fl[] = {0x80, 0x400, 0x2, 0x40, 0x1, 0x800, 0x1000, 0x8, 0x10, 0x20};
        pf[0] = 0x20; pf[1] = 0;
        const int k = ri(0, 4);
        for (int i = 0; i < k; i++) pf[1] |= fl[rnd() % 10];
        if (chance(20)) pf[1] |= rnd() & 0xffffe000u;
        for (int i = 2; i < 8; i++) pf[i] = rnd();
        a[0] = (uint32_t)(uintptr_t)pf;
        break;
    }
    case 0x00460d50: {                                      // texture_format_callback
        uint32_t* d = (uint32_t*)AR(A_ARGS + 0x300);
        memcpy(d, g_formats[rnd() % NFMT], 27 * 4);
        if (chance(30)) {
            make_format(d, (uint32_t)(chance(50) ? 0x41 : 0x40), chance(80) ? 16 : 32, 0, 0, 0, 0);
            static const uint32_t m[] = {0xf800, 0x7e0, 0x1f, 0x7c00, 0x3e0, 0xf00, 0xf0, 0xf, 0xf000, 0x8000, 0};
            for (int i = 22; i < 26; i++) d[i] = chance(80) ? m[rnd() % 11] : rnd();
            if (chance(20)) d[19] |= rnd() & 0x1cb8;
            if (chance(10)) d[1] = rnd();
        }
        a[0] = (uint32_t)(uintptr_t)d; a[1] = rnd();
        break;
    }
    case 0x00460ee0: a[0] = rnd(); break;                   // count_bits
    case 0x00460f00: a[0] = rnd(); a[1] = rnd(); break;     // map_to_info
    case 0x00460f10: a[0] = rnd(); break;                   // map_to_id
    case 0x00460f20:                                        // Pool<texture_info>::scalar deleting destructor
        a[0] = G32(chance(50) ? X_SYS_POOL : X_VID_POOL); a[1] = 0; a[2] = chance(80) ? 1 : 0;
        if (!a[0]) g_kind_note = "no pool";
        break;
    case 0x00461160: case 0x00461790:                       // texGet / load_file
        a[0] = (uint32_t)(uintptr_t)put_name(pick_name(), 1);
        break;
    case 0x00461170: case 0x00461830: {                     // texForget / unload_file
        const TexRes* t = random_tt_res();
        uint8_t* p = t ? t->data : (uint8_t*)0x00526770;
        if (t && t->data[0] == 3 && chance(60)) p += t->size / 2;
        if (chance(10)) p = (uint8_t*)0x00526770;
        a[0] = (uint32_t)(uintptr_t)p;
        break;
    }
    case 0x00461180: {                                      // get_mip_level
        const TexRes* t = random_tt_res();
        a[0] = t ? (uint32_t)(uintptr_t)t->data : 0;
        a[1] = chance(85) ? (uint32_t)(1 << ri(0, 8)) : (uint32_t)ri(-5, 600);
        a[2] = (uint32_t)(uintptr_t)AR(A_ARGS + 0x80);
        break;
    }
    case 0x004613b0: case 0x00461470: {                     // texTransfer / texAlphaBlit
        const int w = chance(90) ? 1 << ri(0, 8) : ri(3, 100);
        const TexRes* t = tex_at_least(w <= 256 ? w : 256, r->at == 0x00461470 && chance(80) ? 2 : -1);
        if (!t) t = tex_at_least(w <= 256 ? w : 256);
        a[0] = t && chance(92) ? (uint32_t)(uintptr_t)t->data : 0x00526770;
        if (t && t->data[0] == 3 && chance(50)) a[0] += (uint32_t)(t->size / 2);
        a[1] = (uint32_t)(uintptr_t)pixel_canvas(w, canvas_type());
        if (!t && a[0] != 0x00526770) g_kind_note = "no texture";
        break;
    }
    case 0x00461540: {                                      // texMapBits: on a private copy
        const TexRes* t = tex_at_least(1);
        uint8_t* copy = AR(A_CANV);
        const int32_t n = t ? (t->size < 0x2aec ? t->size : 0x2aec) : 0;
        if (t) memcpy(copy, t->data, (size_t)n);
        *(int32_t*)(copy + 8) = 7;
        a[0] = chance(95) ? (uint32_t)(uintptr_t)copy : 0x00526770;
        a[1] = chance(85) ? (uint32_t)(1 << ri(0, 6)) : (uint32_t)ri(-3, 300);
        a[2] = (uint32_t)(uint8_t)(chance(85) ? (int8_t)(ri(0, 2) == 0 ? 4 : ri(0, 1) ? 3 : 1) : (int8_t)ri(0, 6));
        break;
    }
    case 0x00460aa0: case 0x0045fbd0: case 0x00459f80:      // texture_find_formats / txBegin / TextureBegin: new formats
        random_formats();
        if (chance(15)) g_nformats = ri(0, g_nformats);    // (often without the solid one: the ExitProcess)
        if (chance(50)) memset((void*)0x00526688, 0, 3 * 0x28);   // a table not yet filled (the callback only sets)
        if (r->at == 0x00459f80) { a[0] = rnd(); a[1] = rnd(); a[2] = rnd(); }
        break;
    case 0x004605e0: case 0x0045a330: case 0x0044e4d0:      // txRelease / TextureRelease / gxReleaseTexture: a grab first
        if (chance(80)) grab_first();
        break;
    default: break;                                         // no arguments
    }
}

// the call's script of failures
static void random_script() {
    g_script = g_quiet;
    g_script.vidready = (uint8_t)chance(50);
    g_script.page_ok = (uint8_t)chance(80);
    g_script.pitch_pad = 0;
    if (chance(15)) g_script.alloc_null = rnd() & rnd();
    if (chance(15)) { g_script.create_fail = rnd() & rnd(); g_script.create_err = rnd(); }
    if (chance(15)) { g_script.lock_fail = rnd() & rnd(); g_script.lock_err = rnd(); }
    if (chance(10)) g_script.attach_err = rnd() & rnd() & rnd();
    if (chance(8)) g_script.qi_fail = rnd() & rnd() & rnd();
    if (chance(10)) g_script.load_fail = rnd() & rnd();
    if (chance(10)) g_script.handle_fail = rnd() & rnd();
    if (chance(10)) g_script.key_fail = rnd() & rnd();
    if (chance(10)) g_script.blt_fail = rnd() & rnd();
    if (chance(8)) g_script.rs_fail = rnd() & rnd();
    if (chance(15)) g_script.res_missing = rnd() & rnd();
    if (chance(5)) g_script.res_badver = rnd() & rnd();
}

// ---- running one pass --------------------------------------------------------------------------------------------------
static unsigned g_pc;
static bool g_unmask;
static int run_guarded(Reg* r, bool rw, bool chain, uint64_t* ret) {
    g_log.n = 0;
    unsigned cw;
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    if (g_unmask) _controlfp_s(&cw, _MCW_EM & ~(_EM_OVERFLOW | _EM_ZERODIVIDE), _MCW_EM);
    if (rw && chain) chain_on();
    int fault = 0;
    __try {
        *ret = rw ? r->neu(g_args) : r->orig(g_args);
        __asm fwait
    } __except (fault_filter(GetExceptionInformation())) { fault = 1; }
    if (rw && chain) chain_off();
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    return fault ? (g_fault_code == PANIC_CODE ? 2 : g_fault_code == EXIT_CODE ? 3 : g_fault_code == ODD_CODE ? 4 : 1) : 0;
}

static std::vector<std::pair<uint32_t, int>> g_panics;
static int g_cov[16];
static const char* panic_text(uint32_t a, char* buf) {
    strcpy(buf, "?");
    __try { strncpy(buf, (const char*)(uintptr_t)a, 44); buf[44] = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return buf;
}
static bool safe_fp(Reg* r, Footprint& fp) {
    __try { r->fp(fp, g_args); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// ---- snapshots of the live state ---------------------------------------------------------------------------------------
struct Snap {
    std::vector<uint8_t> low, com, data;       // [0, heap) and [A_COM, com), .data/.bss
    uint32_t heap, com_end;
};
static void take(Snap& s) {
    s.heap = hdr()->heap;
    s.com_end = hdr()->com;
    s.low.assign(g_arena, g_arena + s.heap);
    s.com.assign(AR(A_COM), AR(s.com_end));
    s.data.assign(DATA, DATA + DATA_BYTES);
}
static void put(const Snap& s) {
    memcpy(g_arena, s.low.data(), s.low.size());
    memcpy(AR(A_COM), s.com.data(), s.com.size());
    memcpy(DATA, s.data.data(), DATA_BYTES);
}
static bool in_fp(const Footprint& fp, const void* p) {
    for (int i = 0; i < fp.n; i++)
        if ((const uint8_t*)p >= (const uint8_t*)fp.r[i].p && (const uint8_t*)p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}
static const char* where(const void* p, char* buf) {
    const uint8_t* q = (const uint8_t*)p;
    if (in_arena(q, 1)) {
        const uint32_t o = (uint32_t)(q - g_arena);
        const char* reg = o < A_OBJS ? "hdr" : o < A_TABLE ? "objs" : o < A_ARGS ? "table" : o < A_SCREEN ? "args" : o < A_CANV ? "screen"
                        : o < A_HEAP ? "canv" : o < A_COM ? "heap" : "com";
        sprintf(buf, "arena+0x%x (%s)", o, reg);
    } else sprintf(buf, "0x%08x", (uint32_t)(uintptr_t)p);
    return buf;
}


// ---- the fixes (built with /DFIX_TESTS) --------------------------------------------------------------------------------
// ordinary rounds: a call that would take a fix's path (an entry without a system copy) is left out
static bool sys_null(int id) {
    if (id == -1) return false;
    bool r = false;
    __try { r = entry(id)->sys == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { r = false; }
    return r;
}
static bool fix_case(const Reg* r) {
    const int a0 = (int)g_args[0], a1 = (int)g_args[1];
    switch (r->at) {
    case 0x0045a4e0: return sys_null(a0) && !entry(a0)->loaded && !G8(X_DERES);             // TextureSelect
    case 0x0045a590: case 0x0045a3e0: case 0x0044e540: case 0x0045a300: case 0x0044e490: return sys_null(a0);
    case 0x0045a240: case 0x0044e380: return a0 != -1 && a1 != -1 && (sys_null(a0) || sys_null(a1));
    case 0x0045a290: case 0x0044e3d0: return sys_null(a1);                                    // (name, id)
    default: return false;
    }
}

#ifdef FIX_TESTS
typedef int(__cdecl* Get_t)(const char*, int);
typedef int(__cdecl* Create_t)(const char*, int, int);
typedef uint8_t(__cdecl* Sel_t)(int);
typedef void(__cdecl* Id_t)(int);
typedef void(__cdecl* Byte_t2)(uint8_t);
typedef int(__cdecl* Size_t)(int);
typedef void(__cdecl* Blit_t)(int, int, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void(__cdecl* ABlit_t)(const char*, int);
typedef uint8_t(__cdecl* Grab_t)(int, void*);
static Get_t const F_Get = (Get_t)0x0045a0c0;
static Create_t const F_Create = (Create_t)0x0045a170;
static Sel_t const F_Select = (Sel_t)0x0045a4e0, F_HasAlpha = (Sel_t)0x0045a590;
static Id_t const F_Forget = (Id_t)0x0045a110, F_Destroy = (Id_t)0x0045a1d0;
static Byte_t2 const F_RestoreAll = (Byte_t2)0x0045a090;
static V_t const F_BeginFrame = (V_t)0x0045a420, F_End = (V_t)0x00459fa0;
static Size_t const F_GetSize = (Size_t)0x0045a3e0;
static Blit_t const F_Blit = (Blit_t)0x0045a240;
static ABlit_t const F_ABlit = (ABlit_t)0x0045a290;
static Grab_t const F_Grab = (Grab_t)0x0045a300;

static int g_fix_checks, g_fix_fail;
static void expect(bool ok, const char* what, int len) {
    g_fix_checks++;
    if (!ok) { g_fix_fail++; printf("  FIX FAILED (%d chars, M1 %d): %s\n", len, g_m1_on, what); }
}
template <typename F> static int guarded(F f) {                  // 0, or the exception (LogPanic's included)
    unsigned cw;
    int r = 0;
    __asm fninit
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    __try { f(); } __except (fault_filter(GetExceptionInformation())) { r = (int)g_fault_code; }
    __asm fninit
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    return r;
}
static void clean_world(bool m1) {
    memcpy(DATA, g_data_pristine, DATA_BYTES);
    memset(AR(0), 0, A_HEAP);
    hdr()->heap = A_HEAP; hdr()->com = A_COM;
    set_m1(m1);
    setup_objects();
    GI(0x005228d0) = 5;
    GI(0x00522a1c) = 2; G8(0x00522a21) = 1; G8(0x005229ed) = 1; GI(0x00522ad4) = 16; G8(0x004f1e9c) = 1; G8(0x00522ae8) = 0;
    GI(0x00522ac8) = 3;
    g_nformats = 0;
    make_format(g_formats[g_nformats++], 0x40, 16, 0xf800, 0x7e0, 0x1f, 0);
    make_format(g_formats[g_nformats++], 0x41, 16, 0xf00, 0xf0, 0xf, 0xf000);
    make_format(g_formats[g_nformats++], 0x41, 16, 0x7c00, 0x3e0, 0x1f, 0x8000);
    g_script = g_quiet;
    g_log.n = 0;
}
// the log's words from `from` on: does it hold `tag` followed by the string (as lg_str logs it, up to 256 characters)?
static bool log_has(uint32_t from, uint32_t tag, uint32_t fmt, const char* str) {
    const uint32_t n = g_log.n < LOG_MAX ? g_log.n : LOG_MAX;
    const uint32_t len = (uint32_t)strlen(str) < 256 ? (uint32_t)strlen(str) : 256;
    for (uint32_t i = from; i < n; i++) {
        if (g_log.w[i] != tag) continue;
        uint32_t j = i + 1;
        if (fmt) { if (j >= n || g_log.w[j] != fmt) continue; j++; }
        if (j >= n || g_log.w[j] != len) continue;
        if (j + 1 + (len + 3) / 4 > n || memcmp(&g_log.w[j + 1], str, len)) continue;
        return true;
    }
    return false;
}
static bool log_has_fmt(uint32_t from, uint32_t tag, uint32_t fmt) {
    const uint32_t n = g_log.n < LOG_MAX ? g_log.n : LOG_MAX;
    for (uint32_t i = from; i + 1 < n; i++) if (g_log.w[i] == tag && g_log.w[i + 1] == fmt) return true;
    return false;
}
static void long_name(char* buf, int len, char c) {          // `len` characters ending ".tex"
    for (int i = 0; i < len - 4; i++) buf[i] = (char)(c + i % 7);
    memcpy(buf + len - 4, ".tex", 5);
}
static bool entry_bytes_same(const uint8_t* a, const uint8_t* b, int keep1, int keep2) {   // but the entries named
    for (int i = 0; i < table_cap(); i++) {
        if (i == keep1 || i == keep2) continue;
        if (memcmp(a + i * 0x28, b + i * 0x28, 0x28)) return false;
    }
    return true;
}

static void fix_names(bool m1) {
    static const int lens[] = {15, 16, 17, 18, 19, 20, 21, 22, 24, 25, 28, 31, 32, 33, 40, 64, 128, 129, 200, 1000};
    static char name[1100], upper[1100];
    static uint8_t table_before[1000 * 0x28];
    for (int len : lens) {
        clean_world(m1);
        long_name(name, len, 'a');
        for (int i = 0; i <= len; i++) upper[i] = (char)toupper((uint8_t)name[i]);
        g_alias.clear();
        g_alias.push_back({name, 0});
        for (size_t t = 0; t < g_tex.size(); t++)
            if (g_tex[t].data[0] == 1 && *(int32_t*)(g_tex[t].data + 8) <= 7) { g_alias.back().second = t; break; }   // a keyed one
        chain_on();
        int id1 = -9, id2 = -9, id3 = -9, id4 = -9, id5 = -9;
        uint8_t sel = 0;
        int e = guarded([&] { ((TB_t)0x00459f80)(0, 0, 0); });
        expect(!e, "TextureBegin", len);
        memcpy(table_before, table_base(), (size_t)table_cap() * 0x28);
        const uint32_t log0 = g_log.n;
        e = guarded([&] { id1 = F_Get(name, 0x21); id2 = F_Get(name, 0x21); id5 = F_Get(upper, 0x21); id3 = F_Get(name, 0); id4 = F_Get(name, 0); });
        expect(!e, "TextureGet ran", len);
        expect(id1 >= 0 && id1 == id2 && id1 == id5, "a keyed name found again (any case)", len);
        expect(id3 >= 0 && id3 != id1 && id3 == id4, "the same name with another key: its own entry, found again", len);
        if (id1 < 0 || id3 < 0) { chain_off(); continue; }
        TexEntry* a = entry(id1);
        TexEntry* b = entry(id3);
        expect(a->key == 0x21 && b->key == 0 && a->refs == 3 && b->refs == 2, "keys and counts intact", len);
        expect(!a->loaded && !a->vid && a->sys && b->sys, "loaded / vid / sys intact", len);
        expect(entry_bytes_same(table_before, table_base(), id1, id3), "no other entry touched", len);
        expect(memchr(a->name, 0, 16) != 0, "the 16-byte field terminated", len);
        if (len < 16) expect(!strcmp(a->name, name), "a short name stored as the original stores it", len);
        else expect(a->name[0] == 1, "a long name's mark in the field", len);
        expect(log_has(log0, 'RGET', 0, name), "the resource asked for by the full name", len);
        e = guarded([&] { sel = F_Select(id1); });
        expect(!e && sel == 1 && a->loaded && a->vid, "TextureSelect loads the video copy (no crash)", len);
        // TextureRestoreAll(1) and a de-rez reload by the full name
        uint32_t log1 = g_log.n;
        e = guarded([&] { F_RestoreAll(1); });
        expect(!e && log_has(log1, 'RGET', 0, name) && !log_has_fmt(log1, 'LREP', 0x004f3f40), "TextureRestoreAll(1) reloads the full name", len);
        log1 = g_log.n;
        G8(X_DERES) = 1;
        e = guarded([&] { F_BeginFrame(); });
        expect(!e && log_has(log1, 'RGET', 0, name) && !log_has_fmt(log1, 'LREP', 0x004f3f40) && entry(id1)->sys, "a de-rez reloads the full name", len);
        // TextureCreate over the same name panics with it (cut to 128 for the log); TextureDestroy's report
        char cut[0x88];
        const char* rep_name = name;
        if (len > 0x80) { memcpy(cut, name, 0x80); memcpy(cut + 0x80, "...", 4); rep_name = cut; }
        log1 = g_log.n;
        e = guarded([&] { F_Create(name, 0x21, 64); });
        expect(e == (int)PANIC_CODE && log_has(log1, 'PANC', 0x004f32a0, rep_name), "TextureCreate of an existing long name panics with it", len);
        log1 = g_log.n;
        e = guarded([&] { F_Destroy(id3); });
        expect(e == (int)PANIC_CODE && log_has(log1, 'PANC', 0x004f32c8, rep_name), "TextureDestroy's report has the full name", len);
        // forgotten: the entry freed, then taken by a short name, then a long one again
        e = guarded([&] { for (int k = 0; k < 4; k++) F_Forget(id1); });   // (3 gets and TextureCreate's count)
        expect(!e && !entry(id1)->in_use, "TextureForget frees it", len);
        int id6 = -9, id7 = -9, id8 = -9;
        e = guarded([&] { id6 = F_Get("grey.tex", 7); id7 = F_Get(name, 0x21); id8 = F_Get(name, 0x21); });
        expect(!e && id6 == id1 && id7 >= 0 && id7 == id8 && !strcmp(entry(id6)->name, "grey.tex"), "the entry reused by a short name, the long one again", len);
        // TextureEnd reports every entry by its full name
        log1 = g_log.n;
        e = guarded([&] { F_End(); });
        expect(!e && log_has(log1, 'LREP', 0x004f3278, rep_name), "TextureEnd reports the full name", len);
        chain_off();
    }
    // keyed copies of a 16-character name (a car's damage copies: key 0, car + 1, car + 0x20)
    clean_world(m1);
    g_alias.clear();
    g_alias.push_back({"willysjeep11.tex", 0});
    chain_on();
    int k0 = -9, k1 = -9, k2 = -9, j0 = -9, j1 = -9, j2 = -9;
    int e = guarded([&] {
        ((TB_t)0x00459f80)(0, 0, 0);
        k0 = F_Get("willysjeep11.tex", 0); k1 = F_Get("willysjeep11.tex", 1); k2 = F_Get("willysjeep11.tex", 0x21);
        j0 = F_Get("willysjeep11.tex", 0); j1 = F_Get("WillysJeep11.tex", 1); j2 = F_Get("willysjeep11.tex", 0x21);
    });
    expect(!e && k0 >= 0 && k1 >= 0 && k2 >= 0 && k0 != k1 && k1 != k2 && k0 != k2 && j0 == k0 && j1 == k1 && j2 == k2,
           "willysjeep11.tex: three keyed copies, each found again", 16);
    chain_off();
}

// an entry without a system copy (its surface couldn't be made): no texture, where the original crashed
static void fix_null_sys(bool m1) {
    for (int pass = 0; pass < 2; pass++) {              // 0: the original (crashes), 1: the fixed rewrites
        clean_world(m1);
        int id = -9, other = -9;
        int e = guarded([&] { ((TB_t)0x00459f80)(0, 0, 0); other = F_Get("grey.tex", 0); });
        g_script.create_fail = 1;                       // the next CreateSurface fails
        hdr()->n_create = 0;
        e = guarded([&] { id = F_Get("sky1.tex", 3); });
        g_script.create_fail = 0;
        if (e || id < 0 || entry(id)->sys) { expect(false, "set-up: a texture whose surface failed", 0); continue; }
        if (pass) chain_on();
        uint8_t sel = 9, alpha = 9, grabbed = 9;
        int size = -9;
        const int32_t grab0 = GI(X_GRAB_ID);
        const int e1 = guarded([&] { sel = F_Select(id); });
        const int e2 = guarded([&] { alpha = F_HasAlpha(id); });
        const int e3 = guarded([&] { size = F_GetSize(id); });
        const int e4 = guarded([&] { F_Blit(id, other, 0, 0, 0x3f800000, 0x3f800000); F_Blit(other, id, 0, 0, 0x3f800000, 0x3f800000); });
        const int e5 = guarded([&] { F_ABlit("effects.tex", id); });
        const int e6 = guarded([&] { grabbed = F_Grab(id, AR(A_ARGS + 0x100)); });
        if (pass) {
            chain_off();
            expect(!e1 && sel == 0 && !G8(X_DERES), "TextureSelect: 0, no de-rez", 0);
            expect(!e2 && alpha == 0, "TextureHasAlpha: 0", 0);
            expect(!e3 && size == 4, "TextureGetSize: 4", 0);
            expect(!e4, "TextureBlit: nothing", 0);
            expect(!e5, "TextureAlphaBlit: nothing", 0);
            expect(!e6 && grabbed == 0 && GI(X_GRAB_ID) == grab0, "TextureGrab: 0, no grab", 0);
        } else {
            expect(e1 && e2 && e3 && e4 && e5 && e6, "the original crashes in each", 0);
        }
    }
}
#endif

// ---- the 16-character name bug, on the original and the rewrite --------------------------------------------------------
struct DemoOut { int id1 = -9, id2 = -9, id3 = -9, sel = -9; uint32_t key = 0, loaded = 0, sys = 0, vid = 0; const char* fault = ""; };
static void demo_run(int pass, char* p, DemoOut& o) {
    unsigned cw;
    __asm fninit
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    if (pass) chain_on();                               // the rewrite: every function of the group rewritten
    __try {
        ((TB_t)0x00459f80)(0, 0, 0);
        TG_t get = (TG_t)0x0045a0c0;
        TS_t select = (TS_t)0x0045a4e0;
        o.id1 = get(p, 0x21);
        o.key = (uint32_t)entry(o.id1)->key;
        o.loaded = entry(o.id1)->loaded;
        o.sys = (uint32_t)(uintptr_t)entry(o.id1)->sys;
        o.vid = (uint32_t)(uintptr_t)entry(o.id1)->vid;
        o.id2 = get(p, 0x21);
        o.id3 = get(p, 0);
        o.fault = "TextureSelect";
        o.sel = select(o.id1);
        o.fault = "";
    } __except (fault_filter(GetExceptionInformation())) {}
    if (pass) chain_off();
    __asm fninit
    _controlfp_s(&cw, _PC_53, _MCW_PC);
}
static void name_bug_demo() {
    printf("-- tc_add's name copy: TextureGet / TextureSelect with long names, stock table (%s) --\n",
           k_ordinary ? "the rewrite with its fix" : "the rewrite as the original");
    for (int pass = 0; pass < 2; pass++) {
        const char* who = pass ? "rewrite " : "original";
        for (int len = 15; len <= 30; len++) {
            if (!(len == 15 || len == 16 || len == 17 || len == 20 || len == 21 || len == 24 || len == 28 || len == 30)) continue;
            g_rng = 0x1234567u + (uint32_t)len;
            memcpy(DATA, g_data_pristine, DATA_BYTES);
            memset(AR(0), 0, A_HEAP);
            hdr()->heap = A_HEAP; hdr()->com = A_COM;
            set_m1(false);
            setup_objects();
            GI(0x00522a1c) = 2; G8(0x00522a21) = 1; G8(0x005229ed) = 1; GI(0x00522ad4) = 16; G8(0x004f1e9c) = 1; G8(0x00522ae8) = 0;
            g_nformats = 0;
            make_format(g_formats[g_nformats++], 0x40, 16, 0xf800, 0x7e0, 0x1f, 0);
            make_format(g_formats[g_nformats++], 0x41, 16, 0xf00, 0xf0, 0xf, 0xf000);
            make_format(g_formats[g_nformats++], 0x41, 16, 0x7c00, 0x3e0, 0x1f, 0x8000);
            g_script = g_quiet;
            std::string nm;
            for (int i = 0; i < len - 4; i++) nm += (char)('a' + i % 26);
            nm += ".tex";
            char* p = put_name(nm, 0);
            DemoOut o;
            demo_run(pass, p, o);
            const int id1 = o.id1, id2 = o.id2, id3 = o.id3, sel = o.sel;
            const uint32_t key_after = o.key, loaded = o.loaded, sys = o.sys, vid = o.vid;
            const char* fault = o.fault;
            printf("  %s len %2d: TextureGet(key 0x21) -> %d, again -> %d%s, key 0 -> %d; entry key %08x, loaded %02x, sys %s, vid %08x; select %s\n",
                   who, len, id1, id2, id1 == id2 ? " (found)" : " (NEW ENTRY)", id3, key_after, loaded,
                   sys ? (in_arena((void*)(uintptr_t)sys, 1) ? "ok" : "WILD") : "0", vid,
                   *fault ? "FAULTED" : sel == 1 ? "1" : "0");
            if (*fault) printf("      (%s: exception %08x at %08x, address %08x)\n", fault, g_fault_code, g_fault_eip, g_fault_addr);
        }
    }
}

// ---- main ----------------------------------------------------------------------------------------------------------
static bool load_textures(const char* dir) {
    static const char* const files[] = {"race.res", "heaven.trk", "Viper.car", "Rotunda.tra", "BallPit.tra", "Burninator.tra",
                                        "BoulderLab.tra", "Balls1000.tra", "common.res", "dundas.trk", "bemidji.trk"};
    int per[5][10] = {};
    size_t total = 0;
    for (const char* fn : files) {
        char path[MAX_PATH];
        sprintf(path, "%s\\%s", dir, fn);
        FILE* f = fopen(path, "rb");                        // read only
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> b((size_t)n);
        fread(b.data(), 1, (size_t)n, f);
        fclose(f);
        if (n < 16 || memcmp(b.data(), "0TSR", 4)) continue;
        uint32_t count; memcpy(&count, &b[4], 4);
        size_t off = 16 + (size_t)count * 36;
        for (uint32_t i = 0; i < count && off < b.size(); i++) {
            const uint8_t* e = &b[16 + (size_t)i * 36];
            char name[17] = {}; memcpy(name, e, 16);
            uint32_t type, ver, size; memcpy(&type, e + 16, 4); memcpy(&ver, e + 20, 4); memcpy(&size, e + 24, 4);
            size_t j = off;
            while (j + 4 <= b.size() && memcmp(&b[j], "!IGM", 4)) j++;
            if (j + 4 > b.size()) break;
            const size_t start = j + 4;
            off = start + size;
            if (type != 0x54455820 || start + size > b.size() || size < 12) continue;
            const uint8_t fm = b[start];
            int32_t lv; memcpy(&lv, &b[start + 8], 4);
            if (fm > 4 || lv < 1 || lv > 9) continue;
            if (per[fm][lv] >= (lv >= 9 ? 1 : 3) || total + size > (8u << 20)) continue;
            bool dup = false;
            for (auto& t : g_tex) if (_stricmp(t.name.c_str(), name) == 0) dup = true;
            if (dup) continue;
            per[fm][lv]++;
            total += size;
            uint8_t* d = (uint8_t*)VirtualAlloc(0, size + 0x40000, MEM_COMMIT, PAGE_READWRITE);   // room behind it: an overlong read stays mapped
            memcpy(d, &b[start], size);
            g_tex.push_back({name, d, (int32_t)size, ver});
        }
    }
    printf("%zu .tex resources (%zu bytes) from %s\n", g_tex.size(), total, dir);
    return !g_tex.empty();
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const int iterations = argc > 1 ? atoi(argv[1]) : 20000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH], dir[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_gx_tex.cpp");
    if (s) *s = 0;
    GetEnvironmentVariableA("VP_REPO", exe, MAX_PATH);     // (a copy built elsewhere: the repository's root)
    sprintf(dir, "%s\\..\\game-files\\installs\\v1.0-RC", exe);
    if (argc > 3) strcpy(dir, argv[3]);
    strcat(exe, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    if (!load_textures(dir)) { printf("no .tex resources found in %s\n", dir); return 2; }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(0x004d6850, (void*)&stub_crt_msgbox);        // __crtMessageBoxA
    ((void(__cdecl*)())0x004ce150)();                      // the CRT's floating point, as its start-up leaves it
    patch_jmp(0x004140e0, (void*)&stub_MemAlloc);
    patch_jmp(0x00414300, (void*)&stub_MemFree);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x00411150, (void*)&stub_LogReport);
    patch_jmp(0x004112b0, (void*)&stub_LogPanic);
    patch_jmp(0x004cf0a0, (void*)&stub_sprintf);
    patch_jmp(0x0041bb00, (void*)&stub_assert);            // ASSERT_MSG (a bare ret in the game)
    patch_jmp(0x00414f30, (void*)&stub_SingleBegin);
    patch_jmp(0x00415000, (void*)&stub_SingleEnter);
    patch_jmp(0x00415070, (void*)&stub_SingleLeave);
    patch_jmp(0x00415090, (void*)&stub_SingleEnd);
    patch_jmp(0x004545b0, (void*)&stub_vid_begin);
    patch_jmp(0x0044f850, (void*)&stub_gfx_begin);
    patch_jmp(0x0044f860, (void*)&stub_gfx_end);
    patch_jmp(0x00454630, (void*)&stub_vid_end);
    patch_jmp(0x00415ca0, (void*)&stub_ExceptSinglePrecision);
    patch_jmp(0x00454670, (void*)&stub_VidSetMode);
    patch_jmp(0x0045a950, (void*)&stub_pal_build_system);
    patch_jmp(0x0044e850, (void*)&stub_mr_set_mode);
    patch_jmp(0x0044e8c0, (void*)&stub_mr_restore_mode);
    patch_jmp(0x00454720, (void*)&stub_VidRestoreMode);
    patch_jmp(0x0044e8f0, (void*)&stub_mr_release);
    patch_jmp(0x0044e910, (void*)&stub_mr_restore);
    patch_jmp(0x00454850, (void*)&stub_VidReady);
    patch_jmp(0x00454880, (void*)&stub_VidFlip);
    patch_jmp(0x00454740, (void*)&stub_VidGetPage);
    patch_jmp(0x00454870, (void*)&stub_VidReleasePage);
    patch_jmp(0x00412bf0, (void*)&stub_Win32Idle);
    patch_jmp(0x00419f70, (void*)&stub_ResourceGetDiscardable);
    patch_jmp(0x0041a450, (void*)&stub_ResourceForget);
    *(uint32_t*)0x005d7478 = (uint32_t)(uintptr_t)&stub_ExitProcess;   // the import slot
    init_vtables();
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_data_pristine = (uint8_t*)malloc(DATA_BYTES);
    memcpy(g_data_pristine, DATA, DATA_BYTES);
    build_m1();
    memset(&g_quiet, 0, sizeof g_quiet);
    g_quiet.vidready = 1;
    g_quiet.page_ok = 1;

    std::vector<Reg*> regs;
    for (Reg* r = Reg::head(); r; r = r->next) regs.push_back(r);
    printf("%zu rewrites registered\n", regs.size());

    name_bug_demo();
#ifdef FIX_TESTS
    for (int m1 = 0; m1 < 2; m1++) { fix_names(m1 != 0); fix_null_sys(m1 != 0); }
    printf("fixes: %d expectations, %d failed\n", g_fix_checks, g_fix_fail);
    g_alias.clear();
#endif

    static Footprint fp;
    static Snap before, after;
    const size_t NR = regs.size();
    std::vector<int> per(NR), per_bad(NR), per_fault(NR), per_ro(NR);
    int stack_canvas = 0, fix_skipped = 0;
    GetCurrentThreadStackLimits(&g_stack_lo, &g_stack_hi);
    int differ = 0, faults = 0, fault_both = 0, fp_bad = 0, replay_only = 0, setup_bad = 0, chains = 0, m1_worlds = 0, unmasked = 0;
    long long checks = 0;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    for (int it = 0; it < iterations; it++) {
        const size_t k = rnd() % NR;
        Reg* r = regs[k];
        setup_world();
        if (!g_setup_ok) {
            setup_bad++;
            if (trace) printf("world %d: set-up failed (%08x at %08x)\n", it, g_fault_code, g_fault_eip);
            continue;
        }
        m1_worlds += g_m1_on;
        gen_args(r);
        random_script();
        if (k_ordinary && fix_case(r)) { fix_skipped++; continue; }
        g_pc = chance(50) ? _PC_24 : chance(50) ? _PC_53 : _PC_64;
        g_unmask = chance(30);
        unmasked += g_unmask;
        const bool chain = chance(40);
        chains += chain;
        if (trace) printf("world %d: %s%s\n", it, r->name, chain ? " (chain)" : "");
        fp.n = 0; fp.pure = false; fp.replay_only = 0;
        if (!safe_fp(r, fp)) fp.replay_only = "(the footprint faulted: a wild argument)";
        if (fp.replay_only) { replay_only++; per_ro[k]++; }
        take(before);
        uint64_t ro = 0, rn = 0;
        const int fo = run_guarded(r, false, false, &ro);
        const uint32_t fo_code = g_fault_code, fo_eip = g_fault_eip;
        take(after);
        g_log_orig = g_log;
        put(before);
        const int fn = run_guarded(r, true, chain, &rn);
        per[k]++;
        checks++;
        bool same = true;
        char buf[96];
        if (fo || fn) {
            faults++;
            per_fault[k]++;
            if (fo && fn) fault_both++;
            if (fo != fn || (fo == 1 && fn == 1 && g_fault_code != fo_code)) {
                printf("  world %d (%s%s): original %d (%08x at %08x), rewrite %d (%08x at %08x, address %08x)\n", it, r->name,
                       chain ? ", chain" : "", fo, fo_code, fo_eip, fn, g_fault_code, g_fault_eip, g_fault_addr);
                same = false;
            }
        }
        if (same) {
            if (hdr()->heap != after.heap || hdr()->com != after.com_end) {
                printf("  MISMATCH world %d (%s%s): heap %x / %x, surfaces %x / %x\n", it, r->name, chain ? ", chain" : "",
                       after.heap, hdr()->heap, after.com_end, hdr()->com);
                same = false;
            }
        }
        if (same) {
            auto cmp = [&](const uint8_t* a, const uint8_t* b, size_t n, const uint8_t* at) {
                for (size_t i = 0; i < n; i++)
                    if (a[i] != b[i]) {
                        printf("  MISMATCH world %d (%s%s): %s original %02x rewrite %02x\n", it, r->name, chain ? ", chain" : "",
                               where(at + i, buf), a[i], b[i]);
                        return false;
                    }
                return true;
            };
            // gx's current canvas left pointing at a canvas on the function's own (dead) frame: a stack address in both
            uint32_t* cur_o = (uint32_t*)(after.data.data() + (0x004efbf8 - 0x004e1000));
            uint32_t* cur_n = (uint32_t*)0x004efbf8;
            const uint32_t keep_o = *cur_o, keep_n = *cur_n;
            const uint32_t cur_0 = *(const uint32_t*)(before.data.data() + (0x004efbf8 - 0x004e1000));
            if (keep_o != cur_0 && on_stack(keep_o) && on_stack(keep_n) && keep_n != cur_0) { *cur_o = keep_n; stack_canvas++; }
            same = cmp(after.low.data(), g_arena, after.low.size(), g_arena) && cmp(after.com.data(), AR(A_COM), after.com.size(), AR(A_COM)) &&
                   cmp(after.data.data(), DATA, DATA_BYTES, DATA);
            *cur_o = keep_o;
            if (same && ro != rn) {
                printf("  MISMATCH world %d (%s): return %llx / %llx\n", it, r->name, (unsigned long long)ro, (unsigned long long)rn);
                same = false;
            }
            if (same && (g_log.n != g_log_orig.n || memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)))) {
                printf("  MISMATCH world %d (%s%s): the logs differ (%u / %u words)", it, r->name, chain ? ", chain" : "", g_log_orig.n, g_log.n);
                uint32_t m = g_log.n < g_log_orig.n ? g_log.n : g_log_orig.n;
                if (m > LOG_MAX) m = LOG_MAX;
                for (uint32_t i = 0; i < m; i++)
                    if (g_log.w[i] != g_log_orig.w[i]) {
                        printf(", first at %u: %08x / %08x (tags before:", i, g_log_orig.w[i], g_log.w[i]);
                        for (uint32_t j = i > 12 ? i - 12 : 0; j < i; j++) printf(" %08x", g_log_orig.w[j]);
                        printf(")");
                        break;
                    }
                printf("\n");
                same = false;
            }
        }
        if (!same) {
            differ++;
            per_bad[k]++;
            printf("    (m1 %d, pc %x, unmasked %d, fault %d/%d, note '%s')\n", g_m1_on, g_pc, g_unmask, fo, fn, g_kind_note);
            if (differ >= 40) { printf("stopping after 40 differing worlds\n"); break; }
        }
        // coverage: which calls the original's pass made, and its panics
        {
            static const uint32_t tags[] = {'SBFT', 'SBLT', 'SKEY', 'TLOD', 'TGHD', 'DCSF', 'SLCK', 'SGAT', 'DRST', 'DENM', 'RGET', 'RFGT', 'LREP', 'EXIT', 'MALC', 'SULK'};
            bool seen[sizeof tags / sizeof tags[0]] = {};
            const uint32_t n = g_log_orig.n < LOG_MAX ? g_log_orig.n : LOG_MAX;
            for (uint32_t i = 0; i < n; i++) {
                for (size_t t = 0; t < sizeof tags / sizeof tags[0]; t++) if (g_log_orig.w[i] == tags[t]) seen[t] = true;
                if (g_log_orig.w[i] == 'PANC' && i + 1 < n) {
                    const uint32_t f = g_log_orig.w[i + 1];
                    size_t j = 0;
                    while (j < g_panics.size() && g_panics[j].first != f) j++;
                    if (j == g_panics.size()) g_panics.push_back({f, 0});
                    g_panics[j].second++;
                }
            }
            for (size_t t = 0; t < sizeof tags / sizeof tags[0]; t++) g_cov[t] += seen[t];
        }
        // the original's writes against the footprint (the fakes' memory and the harness's header are DirectX's / ours)
        if (!fp.replay_only) {
            bool bad = false;
            for (size_t i = A_OBJS; i < before.low.size() && !bad; i++) {
                if (i >= A_OBJS && i < A_TABLE) continue;
                if (after.low[i] != before.low[i] && !in_fp(fp, g_arena + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, r->name, where(g_arena + i, buf));
                    bad = true;
                }
            }
            for (size_t i = before.low.size(); i < after.low.size() && !bad; i++)
                if (!in_fp(fp, g_arena + i)) {
                    printf("  FOOTPRINT world %d (%s): %s (allocated) outside it\n", it, r->name, where(g_arena + i, buf));
                    bad = true;
                }
            for (size_t i = 0; i < (size_t)DATA_BYTES && !bad; i++)
                if (after.data[i] != before.data[i] && !in_fp(fp, DATA + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, r->name, where(DATA + i, buf));
                    bad = true;
                }
            fp_bad += bad;
        }
    }
    printf("%lld checks (%d worlds with M1's table, %d chained, %d with overflow / divide-by-zero unmasked): %d differ, "
           "%d faulted or panicked (%d in both), %d changed bytes outside the footprint, %d replay-only, %d set-ups failed; "
           "%d left gx's current canvas on their own stack frame (compared as a stack address)\n",
           checks, m1_worlds, chains, unmasked, differ, faults, fault_both, fp_bad, replay_only, setup_bad, stack_canvas);
    printf("worlds whose original pass made: BltFast %d, Blt %d, SetColorKey %d, Texture Load %d, GetHandle %d, CreateSurface %d, "
           "Lock %d, GetAttachedSurface %d, SetRenderState %d, EnumTextureFormats %d, ResourceGet %d, ResourceForget %d, "
           "LogReport %d, ExitProcess %d, MemAlloc %d, Unlock %d\n", g_cov[0], g_cov[1], g_cov[2], g_cov[3], g_cov[4], g_cov[5],
           g_cov[6], g_cov[7], g_cov[8], g_cov[9], g_cov[10], g_cov[11], g_cov[12], g_cov[13], g_cov[14], g_cov[15]);
    printf("panics (LogPanic, worlds by message):");
    for (auto& pn : g_panics) { char txt[64]; printf(" [%s] %d;", panic_text(pn.first, txt), pn.second); }
    printf("\n");
    if (k_ordinary) printf("(ordinary rounds: names of 15 characters or fewer; %d calls on an entry without a system copy left out)\n", fix_skipped);
    printf("per function (checks / differing / faulted-or-panicked / replay-only):\n");
    for (size_t i = 0; i < NR; i++) printf("  %-48s %6d / %d / %d / %d\n", regs[i]->name, per[i], per_bad[i], per_fault[i], per_ro[i]);
#ifdef FIX_TESTS
    if (g_fix_fail) return 1;
#endif
    return differ || fp_bad ? 1 : 0;
}
