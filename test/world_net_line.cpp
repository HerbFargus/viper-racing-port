// world_net_line.cpp -- the multiplayer transports (hook/net_socket.cpp, net_line.cpp, net_linepkt.cpp: socket.obj,
// line.obj, linechek.obj, linepkt.obj, tapidbg.obj, crc.obj) against the originals, outside the game (docs/PORTING.md,
// step 3). Stage N1, group A.
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_net_line.cpp
//        /Fo%TEMP%\wnl\ /Fe%TEMP%\wnl\world_net_line.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_net_line.exe [scenarios per kind] [seed]
//     (and with /DVP_NET_FIXES: the fix build, below)
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does (a child process with the range reserved before its heap
// exists). Its KERNEL32 imports are the real functions; every import these functions use is a fake here, stdcall with the
// real argument count, that logs the call (strings and buffers by content, pointers by where they point: the arena, the
// image, or "a stack") and answers from a scripted world, the same in every pass:
//   * winsock: sockets, bind / setsockopt / ioctlsocket (their arguments logged; a sockaddr_in without its sin_zero, which
//     the game never writes), datagrams in (recvfrom: scripted packets, senders, short buffers, WSAEWOULDBLOCK and other
//     errors) and out, gethostname / gethostbyname (1..4 addresses: the fourth runs past UDPSocket's three slots, as the
//     original does), inet_addr, the async lookup (its hostent written into the game's buffer; the reply sent through
//     async_msg_hook later, success or error) and its cancel, IPX's getsockopt;
//   * a COM port pair wired as a null-modem cable (what COM1 writes, COM2 reads, and back), with line noise in the
//     packetizer scenarios (bytes dropped, flipped, stray 0xf0s), short reads, ClearCommError's counts and failures, the
//     modem lines (DCD up and down), overlapped writes pending / done / abandoned / failed, GetCommProperties' port type,
//     Get / SetCommState, SetCommTimeouts, CreateFile results (opens, in use, absent, other errors);
//   * TAPI 1.4: lineInitialize (devices, failures), version negotiation, device caps (voice / data-modem bits, names,
//     the driver key whose AttachedTo names the modem's COM port), lineOpen, lineGetID's VARSTRING (binary or not, a 0
//     handle), the device configuration set and read back (echoed, changed, failing), lineMakeCall / lineAnswer /
//     lineDrop / lineDeallocateCall request ids and errors, lineGetCallInfo's rate; and scripted events through tapi_cb:
//     every LINE_CALLSTATE (offered as owner or not, connected, disconnected...), LINE_REPLY for each request id the line
//     holds (success or failure) and for none, LINE_CLOSE, unknown messages, a message with no instance;
//   * the registry (the autodial value: REG_BINARY on or off, another type, failures; AttachedTo).
// The game functions these call outside the group are stubs too (LogReport / LogPanic -- formatted and logged, the panic
// returns --, MemAlloc from a deterministic heap in the arena, operator delete, Random, PTimeNow, the Win32 and Task
// helpers, Xlator, atexit); the game's C runtime (sprintf, strchr, strtoul, strncpy) runs from the image.
//
// A scenario is a random script of calls on real objects the game's own factories build: UDP and IPX sockets (create,
// send, receive, host lookups sync and async, cancel, the accessors, delete), two DirectLines on the null-modem pair with
// LineSockets and LineCheckers on both ends (the ping exchange to LineOk, framed packets both ways through the noise,
// packets to itself and broadcasts through the local queue until it overflows, the send blocks until they run out), TAPI
// modems (enumerate, open, call / answer / reset under scripted events, delete), LineBegin / LineEnd, kill_thread, and the
// small functions on random arguments (CRC16, init_dcb, the address compares, lds_str, LineStatusText...). Each scenario
// runs three times from the same start -- the game's .data and the arena restored, the same world: once with the
// originals, once with each top-level call made to its rewrite ("isolated": its callees still the originals), once with
// every rewrite hooked into the image ("chain"). The passes must agree on every call's result, the log (every fake API
// and stub call in order, with its arguments), the game's .data and the arena. In the originals' pass, each call whose
// footprint isn't replay_only is checked to write only inside it.
//
// Built with /DVP_NET_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Multiplayer").
// A fix that changes what happens logs "FIX <what>" where it does (VP_FIX_HIT); a rewrite pass with such a line must agree
// with the originals' up to it, and after it only end cleanly (no fault); such scenarios are counted per fix. The one fix
// that changes every call it's in -- get_tapiline_port closing its registry key -- logs that RegCloseKey apart, and those
// lines are left out of the comparison. Then directed_fix_tests: for each fix, the bad case on the originals (the fault,
// overrun, hang -- on a thread with a time limit -- or wrong result shown) and on the rewrite (clean, the result the fix
// promises), and the boundary case that still fits on both, compared. Knobs the directed tests set make the fakes give
// exact answers (g_dt_*); the random scenarios leave them off. Without it (VP_FAITHFUL) every pass must be identical.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>
#include <tuple>
#include <utility>
#include <type_traits>
#ifndef VP_NET_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#define NET_FIXES 0
#else
#define NET_FIXES 1
#endif
#include "../hook/port.h"

// ---- the registered rewrites --------------------------------------------------------------------------------------------
struct Reg {
    uint32_t at; void* fn; const char* name; void (*fp)(Footprint&, const uint32_t*); bool thiscall; Reg* next;
    long hits_top = 0, hits_chain = 0;     // calls made to it by the scenarios / reaching it in the chain pass
    static Reg*& head() { static Reg* h; return h; }
    Reg(uint32_t a, void* f, const char* n, void (*p)(Footprint&, const uint32_t*), bool t)
        : at(a), fn(f), name(n), fp(p), thiscall(t), next(head()) { head() = this; }
};
template <typename T> static T conv(uint32_t v) {
    if constexpr (std::is_pointer_v<T>) return (T)(uintptr_t)v;
    else return (T)v;
}
template <typename F> struct FpInv;
template <typename R, typename... A> struct FpInv<R(__fastcall*)(A...)> {      // (self, edx, args...)
    static constexpr bool thiscall = true;
    template <void (*FP)(Footprint&, A...), size_t... I> static void go(Footprint& f, const uint32_t* a, std::index_sequence<I...>) {
        FP(f, conv<A>(I == 0 ? a[0] : I == 1 ? 0u : a[I - 1])...);
    }
    template <void (*FP)(Footprint&, A...)> static void run(Footprint& f, const uint32_t* a) {
        go<FP>(f, a, std::index_sequence_for<A...>{});
    }
};
#define VP_FPINV_CC(CC)                                                                                    \
    template <typename R, typename... A> struct FpInv<R(CC*)(A...)> {                                    \
        static constexpr bool thiscall = false;                                                          \
        template <void (*FP)(Footprint&, A...), size_t... I> static void go(Footprint& f, const uint32_t* a, std::index_sequence<I...>) { \
            FP(f, conv<A>(a[I])...);                                                                     \
        }                                                                                                \
        template <void (*FP)(Footprint&, A...)> static void run(Footprint& f, const uint32_t* a) {       \
            go<FP>(f, a, std::index_sequence_for<A...>{});                                               \
        }                                                                                                \
    };
VP_FPINV_CC(__cdecl)
VP_FPINV_CC(__stdcall)
#undef VP_FPINV_CC
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static Reg VP_CAT(reg_, NEW)(V10, (void*)&NEW, NAME, &FpInv<decltype(&NEW)>::template run<&FP>, FpInv<decltype(&NEW)>::thiscall);

void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 0x40, what); }
void Footprint::stack_ptr(void* p, const char* what) { add(p, 4, what); }

// N0's entry points (net_wsock.h): outside a session they call the game's own functions, as here
int __cdecl net_PTimeNow() { return ((int(__cdecl*)())(uintptr_t)0x00413b40)(); }
int __cdecl net_Random(int range) { return ((int(__cdecl*)(int))(uintptr_t)0x0041b6e0)(range); }
uint32_t net_async_hook_for_winsock_grab() { return 0x004adc00; }

// a fix marking where it changed what happens: a "FIX <what>" line in the log (the fix build)
static void fix_hit(const char* what);
#define VP_FIX_HIT(what) fix_hit(what)

#include "../hook/net_socket.cpp"
#include "../hook/net_line.cpp"
#include "../hook/net_linepkt.cpp"

// ---- random values, hashes ---------------------------------------------------------------------------------------------
struct Rng {
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
    bool chance(int pct) { return (int)(next() % 100) < pct; }
};
static Rng g_drv;          // the scenario driver's choices
static Rng g_world;        // the fake world's answers
static uint32_t hash_bytes(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}

// ---- the original, loaded at 0x400000 ----------------------------------------------------------------------------------
enum : uint32_t { DATA_AT = 0x004e1000, DATA_SIZE = 0xf5f2c, ARENA = 0x30000000, ARENA_SIZE = 0x100000,
                  HEAP_AT = 0x30040000 };
static std::vector<uint8_t> g_pristine;          // .data as loaded
static uint8_t* g_file;
static IMAGE_NT_HEADERS* g_nt;

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
    // KERNEL32 imports: the real functions (the game's C runtime may use them); everything else a trap (0)
    IMAGE_DATA_DIRECTORY dir = g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        const char* dll = (const char*)(base + imp->Name);
        HMODULE m = _stricmp(dll, "KERNEL32.dll") == 0 ? GetModuleHandleA("kernel32.dll") : 0;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            FARPROC p = 0;
            if (m && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                p = GetProcAddress(m, (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name);
            iat->u1.Function = (DWORD)(uintptr_t)p;
        }
    }
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_WNL_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) return 2;
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    if (!VirtualAllocEx(pi.hProcess, (void*)ARENA, ARENA_SIZE, MEM_RESERVE, PAGE_READWRITE))
        printf("couldn't reserve the arena in the test process (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
static void patch_jmp(uint32_t at, const void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}
static void set_slot(uint32_t slot, const void* fn) { *(uint32_t*)(uintptr_t)slot = (uint32_t)(uintptr_t)fn; }

// the chain: every rewrite hooked into the image
struct Save5 { uint32_t at; uint8_t b[5]; };
static std::vector<Save5> g_chain;
static uint8_t* g_thunks;                        // per rewrite: inc dword [hits_chain]; jmp rewrite
static void chain_patch() {
    if (!g_thunks) g_thunks = (uint8_t*)VirtualAlloc(0, 0x10000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    int i = 0;
    for (Reg* r = Reg::head(); r; r = r->next, i++) {
        Save5 s;
        s.at = r->at;
        memcpy(s.b, (void*)(uintptr_t)r->at, 5);
        g_chain.push_back(s);
        uint8_t* t = g_thunks + i * 16;
        t[0] = 0xff;
        t[1] = 0x05;
        const uint32_t c = (uint32_t)(uintptr_t)&r->hits_chain;
        memcpy(t + 2, &c, 4);
        patch_jmp((uint32_t)(uintptr_t)(t + 6), r->fn);
        patch_jmp(r->at, t);
    }
}
static void chain_unpatch() {
    for (size_t i = g_chain.size(); i-- > 0;) memcpy((void*)(uintptr_t)g_chain[i].at, g_chain[i].b, 5);
    g_chain.clear();
}
static Reg* reg_at(uint32_t at) {
    for (Reg* r = Reg::head(); r; r = r->next)
        if (r->at == at) return r;
    return 0;
}

// ---- the log ------------------------------------------------------------------------------------------------------------
static std::vector<std::string> g_log;
static bool g_quiet_log;                         // (the footprint check's extra pass doesn't log)
static void L(const char* fmt, ...) {
    if (g_quiet_log) return;
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    g_log.push_back(b);
}
static int g_fix_hits;
static void fix_hit(const char* what) {
    g_fix_hits++;
    L("FIX %s", what);
}
static NT_TIB* tib() { return (NT_TIB*)NtCurrentTeb(); }
// a pointer, by where it points (stack frames differ between the passes)
static std::string P(const void* p) {
    const uintptr_t a = (uintptr_t)p;
    char b[32];
    if (!a) return "0";
    if (a >= (uintptr_t)tib()->StackLimit && a < (uintptr_t)tib()->StackBase) return "stack";
    if (a >= ARENA && a < ARENA + ARENA_SIZE) { sprintf(b, "A+%x", (unsigned)(a - ARENA)); return b; }
    sprintf(b, "%x", (unsigned)a);
    return b;
}
static std::string S(const char* s) { return s ? std::string("\"") + s + "\"" : "null"; }
static std::string H(const void* p, size_t n) {
    char b[24];
    sprintf(b, "#%08x/%u", p ? hash_bytes(p, n) : 0, (unsigned)n);
    return b;
}

// ---- the fake world ----------------------------------------------------------------------------------------------------
struct Port {
    bool exists, busy;                  // CreateFile: opens / access denied (5); else not found (2) or (an error)
    uint32_t open_err;                  // !exists: GetLastError
    int peer;                           // what it writes goes to this port's rx (-1: nowhere)
    std::deque<uint8_t> rx;
    uint32_t modem;                     // GetCommModemStatus bits
    int noise;                          // % of bytes disturbed on the way in
    bool short_reads;
};
struct World {
    int clock, clock_step;              // PTimeNow moves on 0..clock_step ms a call
    uint32_t last_error, wsa_err;
    int nsock;
    Port port[6];                       // COM1..4, and two modems' ports (4, 5)
    uint32_t handle_port[0x40];         // a COM handle -> port index + 1
    int nhandles;
    uint32_t xlator_cookie;
    uint32_t task_id;
    int task_dead_after;
    // TAPI
    int ndevs;
    uint8_t devcfg[0x6c];
    bool have_cfg;
    uint32_t next_hcall;
    // the hostent winsock "owns" (gethostbyname)
    uint8_t hostent[0x200];
};
static World g_w;
// the directed tests' knobs (0: off, the random scenarios)
static int g_dt_naddr;                  // gethostname succeeds, gethostbyname gives these addresses (k_dt_addrs)
static const uint32_t k_dt_addrs[] = {0x0100000a, 0x0200000a, 0x0300000a, 0x07ffffff, 0x0500000a};
static bool g_dt_wsa_fail;              // WSAStartup fails
static bool g_dt_ok;                    // TAPI and the registry succeed
static uint32_t g_dt_vs_off, g_dt_vs_size;   // lineGetDevConfig / lineGetID: this string offset (and size) back
static uint32_t g_dt_wait;              // WaitForSingleObject's answer (+1)
static std::vector<uint32_t> g_modem_keys;   // HKLM keys opened (a modem's driver key)
static void world_reset(uint32_t seed) {
    g_w = World();
    g_modem_keys.clear();
    g_world.s = seed * 2654435761u + 0x1234567u;
    if (!g_world.s) g_world.s = 1;
    g_w.clock = 100000;
    g_w.clock_step = 400;
    g_w.task_id = 7;
    for (int i = 0; i < 6; i++) g_w.port[i].peer = -1;
}
static int port_of(uint32_t h) {
    const uint32_t i = h - 0x5000u;
    return i < 0x40u ? (int)g_w.handle_port[i] - 1 : -1;
}

// ---- stubs: the game's functions outside the group ---------------------------------------------------------------------
static std::string fmt_game(const char* fmt, va_list ap) {
    char b[1024];
    _vsnprintf(b, sizeof b, fmt, ap);
    b[sizeof b - 1] = 0;
    return b;
}
static void __cdecl st_LogReport(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    L("LogReport %s", fmt_game(fmt, ap).c_str());
    va_end(ap);
}
static void __cdecl st_LogPanic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    L("LogPanic %s", fmt_game(fmt, ap).c_str());
    va_end(ap);
}
static uint32_t g_heap;
static void* __cdecl st_MemAlloc(int n) {
    const uint32_t a = (g_heap + 15) & ~15u;
    if (a + (uint32_t)n > ARENA + ARENA_SIZE || n < 0) { L("MemAlloc %d -> 0 (out)", n); return 0; }
    if (g_world.chance(3)) { L("MemAlloc %d -> 0", n); return 0; }
    g_heap = a + (uint32_t)n;
    memset((void*)(uintptr_t)a, 0xcd, (size_t)n);
    L("MemAlloc %d -> %s", n, P((void*)(uintptr_t)a).c_str());
    return (void*)(uintptr_t)a;
}
static void __cdecl st_delete(void* p) { L("delete %s", P(p).c_str()); }
static int __cdecl st_Random(int range) {
    const int v = range > 0 ? (int)(g_world.next() % (uint32_t)range) : 0;
    const int r = g_world.chance(5) ? (g_world.chance(50) ? 0 : -1) : v;     // (LineSocket redraws 0 and -1)
    L("Random(%d) -> %d", range, r);
    return r;
}
static int __cdecl st_PTimeNow() {
    g_w.clock += g_world.range(0, g_w.clock_step);
    L("PTimeNow -> %d", g_w.clock);
    return g_w.clock;
}
static char g_errstr[64];
static const char* __cdecl st_Win32GetErrorString0() {
    sprintf(g_errstr, "err %u", g_w.last_error);
    L("Win32GetErrorString()");
    return g_errstr;
}
static const char* __cdecl st_Win32GetErrorString1(int e) {
    sprintf(g_errstr, "err %d", e);
    L("Win32GetErrorString(%d)", e);
    return g_errstr;
}
static void* __cdecl st_Win32GetWindow() { L("Win32GetWindow"); return (void*)0x1234; }
static void* __cdecl st_Win32GetAppInstance() { L("Win32GetAppInstance"); return (void*)0x400000; }
static const char* __cdecl st_Win32AppName() { L("Win32AppName"); return "Viper Racing"; }
static void __cdecl st_RegisterHook(uint32_t fn) { L("Win32RegisterMessageHook %x", fn); }
static void __cdecl st_UnRegisterHook(uint32_t fn) { L("Win32UnRegisterMessageHook %x", fn); }
static void __cdecl st_Win32Idle() { L("Win32Idle"); }
static int __cdecl st_TaskGetID() { L("TaskGetID"); return (int)g_w.task_id; }
static uint8_t __cdecl st_TaskIsDead(int id) {
    const bool dead = g_w.task_dead_after-- <= 0;
    L("TaskIsDead %d -> %d", id, dead);
    return dead;
}
static void __cdecl st_TaskSleep(int ms) { L("TaskSleep %d", ms); }
static void __cdecl st_TaskDestroy(int id) { L("TaskDestroy %d", id); }
// Xlator {key, value, cookie}: built untranslated (cookie = ~g_cookie); xlate gives "<key>" and the current cookie
static void* __fastcall st_Xlator_ctor(uint32_t* self, int, const char* key) {
    L("Xlator::Xlator %s %s", P(self).c_str(), S(key).c_str());
    self[0] = (uint32_t)(uintptr_t)key;
    self[1] = 0;
    self[2] = ~*(volatile uint32_t*)0x004eb108;
    return self;
}
static void __fastcall st_Xlator_xlate(uint32_t* self, int) {
    L("Xlator::xlate %s", P(self).c_str());
    self[1] = self[0];
    self[2] = *(volatile uint32_t*)0x004eb108;
}
static int __cdecl st_atexit(uint32_t fn) { L("atexit %x", fn); return 0; }
static void __cdecl st_purecall() { L("purecall"); RaiseException(0xe0000001, 0, 0, 0); }

// ---- fakes: winsock ------------------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct SinLog { uint16_t family, port; uint32_t addr; };
#pragma pack(pop)
static std::string sa_str(const void* sa, int len) {
    if (!sa) return "null";
    char b[96];
    const uint8_t* p = (const uint8_t*)sa;
    if (len >= 16 && p[0] == 2) {        // AF_INET: sin_zero left out (the game never writes it)
        SinLog s;
        memcpy(&s, p, 8);
        sprintf(b, "in{%u %04x %08x}/%d", s.family, s.port, s.addr, len);
    } else {
        sprintf(b, "%s/%d", H(sa, len > 0 && len <= 16 ? (size_t)len : 0).c_str(), len);
    }
    return b;
}
static int __stdcall f_WSAStartup(uint16_t ver, void* d) {
    const int r = g_dt_wsa_fail ? 10091 : g_world.chance(10) ? 10091 : 0;
    L("WSAStartup %x %s -> %d", ver, P(d).c_str(), r);
    if (d) memset(d, 0x5a, 0x190);
    return r;
}
static int __stdcall f_WSACleanup() { L("WSACleanup"); return 0; }
static int __stdcall f_WSAGetLastError() { L("WSAGetLastError -> %u", g_w.wsa_err); return (int)g_w.wsa_err; }
static uint32_t __stdcall f_socket(int af, int type, int proto) {
    const uint32_t s = g_world.chance(8) ? 0xffffffffu : 0x300u + (uint32_t)g_w.nsock++;
    if (s == 0xffffffffu) g_w.wsa_err = 10047;
    L("socket %d %d %d -> %x", af, type, proto, s);
    return s;
}
static int __stdcall f_closesocket(uint32_t s) {
    const int r = g_world.chance(10) ? -1 : 0;
    if (r) g_w.wsa_err = 10038;
    L("closesocket %x -> %d", s, r);
    return r;
}
static int __stdcall f_bind(uint32_t s, const void* sa, int len) {
    const int r = g_world.chance(8) ? -1 : 0;
    if (r) g_w.wsa_err = 10048;
    L("bind %x %s -> %d", s, sa_str(sa, len).c_str(), r);
    return r;
}
static int __stdcall f_setsockopt(uint32_t s, int level, int opt, const void* v, int len) {
    const int r = g_world.chance(6) ? -1 : 0;
    L("setsockopt %x %x %x %s %d -> %d", s, level, opt, H(v, len > 0 && len < 64 ? (size_t)len : 0).c_str(), len, r);
    return r;
}
static int __stdcall f_ioctlsocket(uint32_t s, uint32_t cmd, uint32_t* arg) {
    const int r = g_world.chance(6) ? -1 : 0;
    L("ioctlsocket %x %x %u -> %d", s, cmd, arg ? *arg : 0, r);
    return r;
}
static int __stdcall f_getsockopt(uint32_t s, int level, int opt, void* v, int* len) {
    const int r = g_world.chance(10) ? -1 : 0;
    L("getsockopt %x %x %x %d in(adapter %d) -> %d", s, level, opt, len ? *len : -1, v ? *(int*)v : -1, r);
    if (!r && v && len && *len >= 0x18) {
        uint8_t* d = (uint8_t*)v;
        for (int i = 4; i < 14; i++) d[i] = (uint8_t)g_world.next();
        if (g_world.chance(10)) *(int*)d = 3;            // (an odd driver: another adapter number back)
    }
    return r;
}
static uint16_t __stdcall f_htons(uint32_t v) { return (uint16_t)((v >> 8 & 0xff) | (v & 0xff) << 8); }
static uint16_t __stdcall f_ntohs(uint32_t v) { return (uint16_t)((v >> 8 & 0xff) | (v & 0xff) << 8); }
static int __stdcall f_sendto(uint32_t s, const void* buf, int len, int flags, const void* to, int tolen) {
    const int r = g_world.chance(6) ? -1 : len;
    if (r < 0) g_w.wsa_err = 10051;
    L("sendto %x %s %d %s -> %d", s, H(buf, len > 0 ? (size_t)len : 0).c_str(), flags, sa_str(to, tolen).c_str(), r);
    return r;
}
static int __stdcall f_recvfrom(uint32_t s, void* buf, int len, int flags, void* from, int* fromlen) {
    const int fl = fromlen ? *fromlen : -1;
    if (g_world.chance(35)) {
        g_w.wsa_err = g_world.chance(85) ? 10035 : 10054;
        L("recvfrom %x %d %d %d -> -1 (%u)", s, len, flags, fl, g_w.wsa_err);
        return -1;
    }
    const int n = g_world.range(1, 300);
    const int k = n < len ? n : len;
    for (int i = 0; i < k; i++) ((uint8_t*)buf)[i] = (uint8_t)g_world.next();
    if (from && fromlen && *fromlen > 0) {
        uint8_t sa[16];
        for (int i = 0; i < 16; i++) sa[i] = (uint8_t)g_world.next();
        sa[0] = fl == 16 ? 2 : 6;
        sa[1] = 0;
        memcpy(from, sa, (size_t)(*fromlen < 16 ? *fromlen : 16));
    }
    const int r = n > len ? -1 : n;                         // (too big: WSAEMSGSIZE)
    if (r < 0) g_w.wsa_err = 10040;
    L("recvfrom %x %d %d %d -> %d (%s)", s, len, flags, fl, r, H(buf, (size_t)k).c_str());
    return r;
}
static int __stdcall f_gethostname(char* name, int n) {
    const int r = g_dt_naddr ? 0 : g_world.chance(5) ? -1 : 0;
    L("gethostname %d -> %d", n, r);
    if (!r) strcpy(name, "vipers-pc");
    return r;
}
// a hostent in g_w.hostent: {name, aliases, type 2, length 4, addr_list} and 1..4 addresses
static void* make_hostent(uint8_t* base, int naddr) {
    uint32_t* h = (uint32_t*)base;
    uint32_t* list = h + 4;                              // 5 pointers
    uint8_t* addrs = base + 0x30;
    for (int i = 0; i < naddr; i++) {
        for (int j = 0; j < 4; j++) addrs[i * 4 + j] = (uint8_t)g_world.next();
        list[i] = (uint32_t)(uintptr_t)(addrs + i * 4);
    }
    list[naddr] = 0;
    strcpy((char*)base + 0x50, "host.example");
    h[0] = (uint32_t)(uintptr_t)(base + 0x50);
    h[1] = (uint32_t)(uintptr_t)(list + 4);              // no aliases (an empty list: list[4] is 0 for naddr < 4)
    h[2] = 0x00040002;
    h[3] = (uint32_t)(uintptr_t)list;
    if (naddr == 4) h[1] = (uint32_t)(uintptr_t)(base + 0x60), *(uint32_t*)(base + 0x60) = 0;
    return base;
}
static void* __stdcall f_gethostbyname(const char* name) {
    if (g_dt_naddr) {                                    // {name, aliases, type, list}: the list at +0x10, addresses at +0x40
        uint32_t* h = (uint32_t*)g_w.hostent;
        memset(g_w.hostent, 0, sizeof g_w.hostent);
        uint32_t* list = h + 4;
        uint32_t* addrs = h + 0x10;
        for (int i = 0; i < g_dt_naddr; i++) {
            addrs[i] = k_dt_addrs[i];
            list[i] = (uint32_t)(uintptr_t)&addrs[i];
        }
        strcpy((char*)g_w.hostent + 0x80, "vipers-pc");
        h[0] = (uint32_t)(uintptr_t)(g_w.hostent + 0x80);
        h[1] = (uint32_t)(uintptr_t)(g_w.hostent + 0x90);
        h[2] = 0x00040002;
        h[3] = (uint32_t)(uintptr_t)list;
        L("gethostbyname %s -> %d addresses (the test's)", S(name).c_str(), g_dt_naddr);
        return h;
    }
    if (g_world.chance(10)) {
        L("gethostbyname %s -> 0", S(name).c_str());
        return 0;
    }
    const int n = g_world.chance(10) ? 4 : g_world.range(1, 3);
    void* h = make_hostent(g_w.hostent, n);
    L("gethostbyname %s -> %d addresses %s", S(name).c_str(), n, H(g_w.hostent + 0x30, 16).c_str());
    return h;
}
static uint32_t __stdcall f_inet_addr(const char* s) {
    unsigned a, b, c, d;
    char x;
    uint32_t r = 0xffffffffu;
    if (s && sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &x) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
        r = a | b << 8 | c << 16 | d << 24;
    L("inet_addr %s -> %08x", S(s).c_str(), r);
    return r;
}
static uint32_t __stdcall f_WSAAsyncGetHostByName(void* w, uint32_t msg, const char* name, void* buf, int len) {
    const uint32_t h = g_world.chance(10) ? 0 : 0x7000u + (g_world.next() & 0xff);
    if (!h) g_w.wsa_err = 10055;
    L("WSAAsyncGetHostByName %s %x %s %s %d -> %x", P(w).c_str(), msg, S(name).c_str(), P(buf).c_str(), len, h);
    if (h && buf && len >= 0x100) make_hostent((uint8_t*)buf, g_world.range(1, 3));
    return h;
}
static int __stdcall f_WSACancelAsyncRequest(uint32_t h) { L("WSACancelAsyncRequest %x", h); return 0; }

// ---- fakes: KERNEL32 (COM ports, events) ---------------------------------------------------------------------------------
static uint32_t __stdcall f_GetLastError() { L("GetLastError -> %u", g_w.last_error); return g_w.last_error; }
static uint32_t __stdcall f_CreateFileA(const char* name, uint32_t access, uint32_t share, void* sec, uint32_t disp,
                                        uint32_t flags, uint32_t tmpl) {
    int i = -1;
    if (name && !_strnicmp(name, "COM", 3) && name[3] >= '1' && name[3] <= '4' && !name[4]) i = name[3] - '1';
    uint32_t h = 0xffffffffu;
    if (i >= 0 && g_w.port[i].exists && !g_w.port[i].busy && g_w.nhandles < 0x40) {
        g_w.handle_port[g_w.nhandles] = (uint32_t)i + 1;
        h = 0x5000u + (uint32_t)g_w.nhandles++;
    } else {
        g_w.last_error = i < 0 ? 2 : g_w.port[i].busy ? 5 : g_w.port[i].open_err;
    }
    L("CreateFileA %s %x %x %s %u %x %x -> %x", S(name).c_str(), access, share, P(sec).c_str(), disp, flags, tmpl, h);
    return h;
}
static int __stdcall f_CloseHandle(uint32_t h) { L("CloseHandle %x", h); return 1; }
static int __stdcall f_ReadFile(uint32_t h, void* buf, uint32_t n, uint32_t* got, void* ovl) {
    const int p = port_of(h);
    const bool header = p >= 0 && n == 3 && g_w.port[p].rx.size() >= 3;     // (see below)
    if (p < 0 || (!header && g_world.chance(3))) {
        g_w.last_error = 6;
        if (got) *got = 0;
        L("ReadFile %x %u %s -> 0", h, n, P(ovl).c_str());
        return 0;
    }
    Port& q = g_w.port[p];
    uint32_t k = n < q.rx.size() ? n : (uint32_t)q.rx.size();
    // (a short read of LinePacketizer's 3-byte header would leave the rest of it stack garbage, in the original as in the
    // rewrite: not comparable, so a header read always gets its 3 bytes -- see fail_ok)
    if (q.short_reads && k > 1 && n != 3 && g_world.chance(25)) k = (uint32_t)g_world.range(1, (int)k);
    for (uint32_t i = 0; i < k; i++) ((uint8_t*)buf)[i] = q.rx[i];
    q.rx.erase(q.rx.begin(), q.rx.begin() + k);
    *got = k;
    L("ReadFile %x %u %s -> 1 %u %s", h, n, P(ovl).c_str(), k, H(buf, k).c_str());
    return 1;
}
static void deliver(int to, const uint8_t* p, uint32_t n) {
    Port& q = g_w.port[to];
    for (uint32_t i = 0; i < n; i++) {
        if (q.noise && g_world.chance(q.noise)) {
            switch (g_world.next() % 4) {
            case 0: continue;                                       // dropped
            case 1: q.rx.push_back((uint8_t)(p[i] ^ (1u << (g_world.next() % 8)))); continue;   // flipped
            case 2: q.rx.push_back(0xf0); break;                    // a stray sync byte before it
            default: q.rx.push_back((uint8_t)g_world.next()); break; // a stray byte before it
            }
        }
        q.rx.push_back(p[i]);
    }
}
static int __stdcall f_WriteFile(uint32_t h, const void* buf, uint32_t n, uint32_t* written, void* ovl) {
    const int p = port_of(h);
    const int roll = g_world.range(0, 99);
    int r = 1;
    if (roll < 25) { r = 0; g_w.last_error = 997; }               // ERROR_IO_PENDING (it still goes)
    else if (roll < 30) { r = 0; g_w.last_error = 31; }           // a real failure (nothing goes)
    L("WriteFile %x %s %s %s -> %d", h, H(buf, n).c_str(), P(written).c_str(), P(ovl).c_str(), r);
    if (r || g_w.last_error == 997) {
        if (written && r) *written = n;
        if (p >= 0 && g_w.port[p].peer >= 0) deliver(g_w.port[p].peer, (const uint8_t*)buf, n);
    }
    return r;
}
static int __stdcall f_ClearCommError(uint32_t h, uint32_t* err, uint32_t* stat) {
    const int p = port_of(h);
    if (p < 0 || g_world.chance(4)) {
        g_w.last_error = 1117;
        L("ClearCommError %x -> 0", h);
        return 0;
    }
    if (err) *err = 0;
    stat[0] = 0;
    stat[1] = (uint32_t)g_w.port[p].rx.size();
    stat[2] = (uint32_t)g_world.range(0, 50);
    L("ClearCommError %x -> 1 in %u out %u", h, stat[1], stat[2]);
    return 1;
}
static int __stdcall f_SetCommTimeouts(uint32_t h, const uint32_t* t) {
    const int r = g_world.chance(6) ? 0 : 1;
    L("SetCommTimeouts %x {%x %x %x %x %x} -> %d", h, t[0], t[1], t[2], t[3], t[4], r);
    return r;
}
static int __stdcall f_GetCommModemStatus(uint32_t h, uint32_t* st) {
    const int p = port_of(h);
    const int r = p >= 0 && !g_world.chance(4) ? 1 : 0;
    if (r) *st = g_w.port[p].modem;
    L("GetCommModemStatus %x -> %d %x", h, r, r ? *st : 0);
    return r;
}
static int __stdcall f_GetCommProperties(uint32_t h, uint8_t* cp) {
    uint16_t len;
    uint32_t spec;
    memcpy(&len, cp, 2);
    memcpy(&spec, cp + 0x34, 4);
    const int r = g_world.chance(5) ? 0 : 1;
    const uint32_t sub = g_world.chance(10) ? 6 : 1;
    if (r) memcpy(cp + 0x18, &sub, 4);
    L("GetCommProperties %x len %x spec %x -> %d sub %u", h, len, spec, r, sub);
    return r;
}
static int __stdcall f_GetCommState(uint32_t h, uint8_t* dcb) {
    const int r = g_world.chance(5) ? 0 : 1;
    if (r) for (int i = 0; i < 0x1c; i++) dcb[i] = (uint8_t)g_world.next();
    L("GetCommState %x -> %d", h, r);
    return r;
}
static int __stdcall f_SetCommState(uint32_t h, const uint8_t* dcb) {
    const int r = g_world.chance(5) ? 0 : 1;
    L("SetCommState %x %s -> %d", h, H(dcb, 0x1c).c_str(), r);
    return r;
}
static uint32_t __stdcall f_WaitForSingleObject(uint32_t ev, uint32_t ms) {
    static const uint32_t k[] = {0, 0, 0, 0x102, 0x102, 0x80, 0xffffffffu, 5};
    const uint32_t r = g_dt_wait ? g_dt_wait - 1 : k[g_world.next() % 8];
    if (r == 0xffffffffu) g_w.last_error = 6;
    L("WaitForSingleObject %x %u -> %x", ev, ms, r);
    return r;
}
static uint32_t __stdcall f_CreateEventA(void* sec, int manual, int initial, const char* name) {
    const uint32_t e = g_world.chance(2) ? 0 : 0x6000u + (g_world.next() & 0xfff);
    L("CreateEventA %s %d %d %s -> %x", P(sec).c_str(), manual, initial, S(name).c_str(), e);
    return e;
}

// ---- fakes: ADVAPI32 ------------------------------------------------------------------------------------------------------
static int32_t __stdcall f_RegOpenKeyExA(uint32_t root, const char* key, uint32_t opt, uint32_t sam, uint32_t* hk) {
    const int32_t r = g_dt_ok ? 0 : g_world.chance(10) ? 2 : 0;
    if (!r) *hk = 0x8800u + (g_world.next() & 0xff);
    if (!r && root == 0x80000002u) g_modem_keys.push_back(*hk);
    L("RegOpenKeyExA %x %s %u %x -> %d", root, S(key).c_str(), opt, sam, r);
    return r;
}
static int32_t __stdcall f_RegQueryValueExA(uint32_t hk, const char* name, uint32_t* res, uint32_t* type, uint8_t* data, uint32_t* cb) {
    const uint32_t cap = cb ? *cb : 0;
    const int32_t r = g_dt_ok ? 0 : g_world.chance(8) ? 234 : 0;
    if (!r) {
        const bool attached = name && !strcmp(name, "AttachedTo");
        *type = g_dt_ok ? (attached ? 1 : 3) : g_world.chance(10) ? 4 : attached ? 1 : 3;
        if (attached) {
            char s[8] = "COM?";
            s[3] = (char)(g_world.chance(10) ? 'x' : '1' + g_world.range(0, 8));
            memcpy(data, s, 5);
            *cb = 5;
        } else {
            const uint32_t v = g_world.chance(10) ? 7 : g_world.next() & 1;
            memcpy(data, &v, 4);
            *cb = 4;
        }
    }
    L("RegQueryValueExA %x %s %s %u -> %d", hk, S(name).c_str(), P(res).c_str(), cap, r);
    if (!r) g_log.back() += " type " + std::to_string(*type) + " " + H(data, *cb);
    return r;
}
static int32_t __stdcall f_RegSetValueExA(uint32_t hk, const char* name, uint32_t res, uint32_t type, const uint8_t* data, uint32_t cb) {
    const int32_t r = g_world.chance(30) ? 5 : 0;
    if (r) g_w.last_error = 5;
    L("RegSetValueExA %x %s %u %u %s -> %d", hk, S(name).c_str(), res, type, H(data, cb).c_str(), r);
    return r;
}
// (a modem's driver key, which only get_tapiline_port's fix closes: logged apart, left out of the fix build's comparison)
static int32_t __stdcall f_RegCloseKey(uint32_t hk) {
    bool modem = false;
    for (uint32_t k : g_modem_keys) modem |= k == hk;
    L(modem ? "RegCloseKey(modem key) %x" : "RegCloseKey %x", hk);
    return 0;
}

// ---- fakes: TAPI32 -------------------------------------------------------------------------------------------------------
static int32_t tapi_result() {
    if (g_dt_ok) return 0;
    return g_world.chance(8) ? (int32_t)(0x80000000u | (uint32_t)g_world.range(1, 0x4b)) : 0;
}
static int32_t __stdcall f_lineInitialize(uint32_t* app, uint32_t inst, uint32_t cb, const char* name, uint32_t* ndevs) {
    const int32_t r = tapi_result();
    if (!r) {
        *app = 0x7a00;
        *ndevs = (uint32_t)g_w.ndevs;
    }
    L("lineInitialize %s %x %x %s %s -> %d", P(app).c_str(), inst, cb, S(name).c_str(), P(ndevs).c_str(), r);
    return r;
}
static int32_t __stdcall f_lineNegotiateAPIVersion(uint32_t app, uint32_t dev, uint32_t lo, uint32_t hi, uint32_t* ver, void* ext) {
    const int32_t r = g_world.chance(4) ? (int32_t)0x80000040 : 0;
    if (!r) *ver = 0x10004;
    L("lineNegotiateAPIVersion %x %u %x %x %s %s -> %d", app, dev, lo, hi, P(ver).c_str(), P(ext).c_str(), r);
    return r;
}
static int32_t __stdcall f_lineGetDevCaps(uint32_t app, uint32_t dev, uint32_t ver, uint32_t ext, uint8_t* caps) {
    uint32_t total;
    memcpy(&total, caps, 4);
    const int32_t r = tapi_result();
    L("lineGetDevCaps %x %u %x %u %u -> %d", app, dev, ver, ext, total, r);
    if (r) return r;
    memset(caps + 4, 0, 0x100);
    auto put = [&](uint32_t at, uint32_t v) { memcpy(caps + at, &v, 4); };
    char name[32];
    sprintf(name, "Modem %u", dev);
    strcpy((char*)caps + 0x200, name);
    put(0x20, (uint32_t)strlen(name) + 1);
    put(0x24, 0x200);
    put(0x34, g_world.chance(15) ? 2 : 1 | 4);                       // bearer modes
    put(0x3c, g_world.chance(15) ? 4 : 0x10 | 2);                    // media modes
    put(0xec, g_world.next());
    if (g_world.chance(85)) {                                        // the driver's key, device-specific
        put(0xe4, 0x40);
        put(0xe8, 0x280);
        put(0x280, g_world.chance(10) ? 0 : 1);
        put(0x284, 8);
        strcpy((char*)caps + 0x288, "System\\Modem\\0000");
    } else {
        put(0xe4, 0);
    }
    return 0;
}
static int32_t __stdcall f_lineOpen(uint32_t app, uint32_t dev, uint32_t* hline, uint32_t api, uint32_t ext, uint32_t inst,
                                    uint32_t priv, uint32_t media, void* params) {
    const int32_t r = tapi_result();
    if (!r) *hline = 0x9100u + dev;
    L("lineOpen %x %u %s %x %u %s %x %x %s -> %d", app, dev, P(hline).c_str(), api, ext, P((void*)(uintptr_t)inst).c_str(),
      priv, media, P(params).c_str(), r);
    return r;
}
static int32_t __stdcall f_lineGetID(uint32_t hline, uint32_t addr, uint32_t hcall, uint32_t select, uint8_t* vs, const char* cls) {
    uint32_t total;
    memcpy(&total, vs, 4);
    const int32_t r = tapi_result();
    L("lineGetID %x %u %x %u %u %s -> %d", hline, addr, hcall, select, total, S(cls).c_str(), r);
    if (r) return r;
    auto put = [&](uint32_t at, uint32_t v) { memcpy(vs + at, &v, 4); };
    put(0xc, g_dt_ok ? 4 : g_world.chance(8) ? 1 : 4);
    put(0x10, 4 + 8);
    put(0x14, g_dt_vs_off ? g_dt_vs_off : 0x18);
    // the modem's own COM port (port 4 or 5), or 0
    uint32_t h = 0;
    if (!g_world.chance(8) && g_w.nhandles < 0x40) {
        g_w.handle_port[g_w.nhandles] = 5u + (hline & 1);
        h = 0x5000u + (uint32_t)g_w.nhandles++;
    }
    put(0x18, h);
    memcpy(vs + 0x1c, "COM9", 5);
    return 0;
}
static int32_t __stdcall f_lineSetDevConfig(uint32_t dev, const uint8_t* cfg, uint32_t n, const char* cls) {
    const int32_t r = tapi_result();
    L("lineSetDevConfig %u %s %s -> %d", dev, H(cfg, n).c_str(), S(cls).c_str(), r);
    if (!r && n == 0x6c) memcpy(g_w.devcfg, cfg, 0x6c), g_w.have_cfg = true;
    return r;
}
static int32_t __stdcall f_lineGetDevConfig(uint32_t dev, uint8_t* vs, const char* cls) {
    uint32_t total;
    memcpy(&total, vs, 4);
    const int32_t r = tapi_result();
    L("lineGetDevConfig %u %u %s -> %d", dev, total, S(cls).c_str(), r);
    if (r) return r;
    auto put = [&](uint32_t at, uint32_t v) { memcpy(vs + at, &v, 4); };
    const uint32_t n = g_world.chance(10) ? 0x40 : 0x6c;
    put(0x10, g_dt_vs_off ? g_dt_vs_size : n);
    put(0x14, g_dt_vs_off ? g_dt_vs_off : 0x18);
    memcpy(vs + 0x18, g_w.devcfg, 0x6c);
    if (g_world.chance(20)) vs[0x18 + g_world.range(0, (int)n - 1)] ^= 0x10;   // the driver changed something
    return 0;
}
static int32_t __stdcall f_lineGetCallInfo(uint32_t hcall, uint8_t* ci) {
    uint32_t total;
    memcpy(&total, ci, 4);
    const int32_t r = tapi_result();
    if (!r) {
        const uint32_t rate = g_world.chance(50) ? 28800 : (uint32_t)g_world.range(0, 60000);
        memcpy(ci + 0x1c, &rate, 4);
    }
    L("lineGetCallInfo %x %u -> %d", hcall, total, r);
    return r;
}
static int32_t req_id() { return g_world.chance(15) ? (int32_t)(0x80000000u | (uint32_t)g_world.range(1, 0x40)) : g_world.range(1, 12); }
static int32_t __stdcall f_lineMakeCall(uint32_t hline, uint32_t* hcall, const char* num, uint32_t country, const uint32_t* params) {
    const int32_t r = req_id();
    if (r > 0) *hcall = g_w.next_hcall = 0xc000u + (g_world.next() & 0xff);
    L("lineMakeCall %x %s %s %u %s -> %d", hline, P(hcall).c_str(), S(num).c_str(), country, H(params, 0x70).c_str(), r);
    return r;
}
static int32_t __stdcall f_lineAnswer(uint32_t hcall, uint32_t a, uint32_t b) {
    const int32_t r = req_id();
    L("lineAnswer %x %u %u -> %d", hcall, a, b, r);
    return r;
}
static int32_t __stdcall f_lineDrop(uint32_t hcall, uint32_t a, uint32_t b) {
    const int32_t r = req_id();
    L("lineDrop %x %u %u -> %d", hcall, a, b, r);
    return r;
}
static int32_t __stdcall f_lineDeallocateCall(uint32_t hcall) {
    const int32_t r = g_world.chance(15) ? (int32_t)0x80000009 : 0;
    L("lineDeallocateCall %x -> %d", hcall, r);
    return r;
}
static int32_t __stdcall f_lineClose(uint32_t hline) { L("lineClose %x", hline); return 0; }
static int32_t __stdcall f_lineShutdown(uint32_t app) { L("lineShutdown %x", app); return 0; }

// ---- install ------------------------------------------------------------------------------------------------------------
static void install_fakes() {
    using namespace nt;
    const struct { uint32_t at; const void* to; } stubs[] = {
        {F_LogReport, (void*)st_LogReport}, {F_LogPanic, (void*)st_LogPanic}, {F_MemAlloc, (void*)st_MemAlloc},
        {F_op_delete, (void*)st_delete}, {F_Random, (void*)st_Random}, {F_PTimeNow, (void*)st_PTimeNow},
        {F_Win32GetErrorString0, (void*)st_Win32GetErrorString0}, {F_Win32GetErrorString1, (void*)st_Win32GetErrorString1},
        {F_Win32GetWindow, (void*)st_Win32GetWindow}, {F_Win32GetAppInstance, (void*)st_Win32GetAppInstance},
        {F_Win32AppName, (void*)st_Win32AppName}, {F_Win32RegisterMessageHook, (void*)st_RegisterHook},
        {F_Win32UnRegisterMessageHook, (void*)st_UnRegisterHook}, {F_Win32Idle, (void*)st_Win32Idle},
        {F_TaskGetID, (void*)st_TaskGetID}, {F_TaskIsDead, (void*)st_TaskIsDead}, {F_TaskSleep, (void*)st_TaskSleep},
        {F_TaskDestroy, (void*)st_TaskDestroy}, {F_Xlator_ctor, (void*)st_Xlator_ctor}, {F_Xlator_xlate, (void*)st_Xlator_xlate},
        {F_atexit, (void*)st_atexit}, {0x004cf350, (void*)st_purecall},
    };
    for (auto& s : stubs) patch_jmp(s.at, s.to);
    const struct { uint32_t slot; const void* fn; } slots[] = {
        {I_WSACancelAsyncRequest, (void*)f_WSACancelAsyncRequest}, {I_WSAAsyncGetHostByName, (void*)f_WSAAsyncGetHostByName},
        {I_inet_addr, (void*)f_inet_addr}, {I_WSAGetLastError, (void*)f_WSAGetLastError}, {I_gethostbyname, (void*)f_gethostbyname},
        {I_gethostname, (void*)f_gethostname}, {I_ioctlsocket, (void*)f_ioctlsocket}, {I_setsockopt, (void*)f_setsockopt},
        {I_bind, (void*)f_bind}, {I_htons, (void*)f_htons}, {I_socket, (void*)f_socket}, {I_WSAStartup, (void*)f_WSAStartup},
        {I_closesocket, (void*)f_closesocket}, {I_WSACleanup, (void*)f_WSACleanup}, {I_sendto, (void*)f_sendto},
        {I_ntohs, (void*)f_ntohs}, {I_recvfrom, (void*)f_recvfrom}, {I_getsockopt, (void*)f_getsockopt},
        {I_lineOpen, (void*)f_lineOpen}, {I_lineClose, (void*)f_lineClose}, {I_lineDeallocateCall, (void*)f_lineDeallocateCall},
        {I_lineDrop, (void*)f_lineDrop}, {I_lineGetID, (void*)f_lineGetID}, {I_lineGetDevConfig, (void*)f_lineGetDevConfig},
        {I_lineSetDevConfig, (void*)f_lineSetDevConfig}, {I_lineGetCallInfo, (void*)f_lineGetCallInfo},
        {I_lineMakeCall, (void*)f_lineMakeCall}, {I_lineAnswer, (void*)f_lineAnswer},
        {I_lineNegotiateAPIVersion, (void*)f_lineNegotiateAPIVersion}, {I_lineInitialize, (void*)f_lineInitialize},
        {I_lineShutdown, (void*)f_lineShutdown}, {I_lineGetDevCaps, (void*)f_lineGetDevCaps},
        {I_CreateFileA, (void*)f_CreateFileA}, {I_CloseHandle, (void*)f_CloseHandle}, {I_GetLastError, (void*)f_GetLastError},
        {I_ReadFile, (void*)f_ReadFile}, {I_WriteFile, (void*)f_WriteFile}, {I_GetCommState, (void*)f_GetCommState},
        {I_SetCommState, (void*)f_SetCommState}, {I_GetCommProperties, (void*)f_GetCommProperties},
        {I_GetCommModemStatus, (void*)f_GetCommModemStatus}, {I_SetCommTimeouts, (void*)f_SetCommTimeouts},
        {I_ClearCommError, (void*)f_ClearCommError}, {I_WaitForSingleObject, (void*)f_WaitForSingleObject},
        {I_CreateEventA, (void*)f_CreateEventA}, {I_RegOpenKeyExA, (void*)f_RegOpenKeyExA},
        {I_RegQueryValueExA, (void*)f_RegQueryValueExA}, {I_RegCloseKey, (void*)f_RegCloseKey},
        {I_RegSetValueExA, (void*)f_RegSetValueExA},
    };
    for (auto& s : slots) set_slot(s.slot, s.fn);
}

// ---- calling ------------------------------------------------------------------------------------------------------------
// fn with a[0..n): thiscall (ecx = a[0], the rest pushed) or not (all pushed); esp restored after (cdecl and stdcall alike);
// edx = 0 (a rewrite's unused edx)
static uint32_t __declspec(noinline) raw_call(uint32_t fn, bool thiscall, const uint32_t* a, int n) {
    uint32_t r, saved, a0 = n ? a[0] : 0;
    int first = thiscall ? 1 : 0;
    __asm {
        mov saved, esp
        mov esi, a
        mov ecx, n
    l_push:
        cmp ecx, first
        jle l_done
        dec ecx
        push dword ptr [esi + ecx*4]
        jmp l_push
    l_done:
        mov ecx, a0
        xor edx, edx
        call fn
        mov r, eax
        mov esp, saved
    }
    return r;
}

enum Mode { ORIG, ISOLATED, CHAIN };
static Mode g_mode;
static bool g_fault;
static int g_fp_violations, g_fp_checked;
static std::vector<std::string> g_fp_msgs;
static Footprint* g_fp;
static bool g_check_fp;                          // this scenario's calls get their footprints checked (the first ones)

static uint32_t g_fixed;
// the arena up to the end of what the heap has handed out (with some slack: an overrun shows)
static size_t arena_used() {
    const uint32_t end = (g_heap > HEAP_AT ? g_heap : HEAP_AT) + 0x2000;
    return end - ARENA < ARENA_SIZE ? end - ARENA : ARENA_SIZE;
}
static bool in_regions(const Footprint& f, uintptr_t a) {
    for (int i = 0; i < f.n; i++)
        if (a >= (uintptr_t)f.r[i].p && a < (uintptr_t)f.r[i].p + f.r[i].n) return true;
    return false;
}
static int seh_call(uint32_t fn, bool thiscall, const uint32_t* a, int n, uint32_t* r) {
    __try {
        *r = raw_call(fn, thiscall, a, n);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return (int)GetExceptionCode();
    }
}
static std::vector<uint8_t> g_snap_data, g_snap_arena;
static World g_snap_world;
static Rng g_snap_rng;
static uint32_t g_snap_heap;
static size_t g_snap_log;

// a call of the game function at `at` (one of the group's): the original, its rewrite (isolated) or through the hooked
// image (chain); `ret` the bits of the result that count (8, 16, 32 or 0). The originals' pass checks the footprint.
static uint32_t call(uint32_t at, std::initializer_list<uint32_t> args, int ret = 32) {
    if (g_fault) return 0;
    Reg* reg = reg_at(at);
    const bool thiscall = reg ? reg->thiscall : false;
    uint32_t a[16];
    int n = 0;
    for (uint32_t v : args) a[n++] = v;
    uint32_t fn = at;
    if (g_mode == ISOLATED && reg) fn = (uint32_t)(uintptr_t)reg->fn;
    bool check = false;
    if (g_mode == ORIG && reg) reg->hits_top++;
    if (g_mode == ORIG && reg && g_check_fp) {
        Footprint& f = *g_fp;
        f.n = 0;
        f.replay_only = 0;
        f.pure = false;
        reg->fp(f, a);
        check = !f.replay_only;
        if (check) {
            memcpy(g_snap_data.data(), (void*)(uintptr_t)DATA_AT, DATA_SIZE);
            memcpy(g_snap_arena.data(), (void*)(uintptr_t)ARENA, arena_used());
        }
    }
    uint32_t r = 0;
    const int code = seh_call(fn, thiscall, a, n, &r);
    if (ret == 8) r &= 0xff;
    else if (ret == 16) r &= 0xffff;
    else if (ret == 0) r = 0;
    if (code) {
        L("FAULT %08x in %s", (unsigned)code, reg ? reg->name : "?");
        g_fault = true;
        return 0;
    }
    if (check) {
        g_fp_checked++;
        const Footprint& f = *g_fp;
        auto scan = [&](const uint8_t* before, uintptr_t base, size_t size) {
            for (size_t i = 0; i < size; i++)
                if (before[i] != ((const uint8_t*)base)[i] && !in_regions(f, base + i)) {
                    if (g_fp_msgs.size() < 20) {
                        char m[160];
                        sprintf(m, "%s writes %08x outside its footprint", reg->name, (unsigned)(base + i));
                        g_fp_msgs.push_back(m);
                    }
                    g_fp_violations++;
                    return;
                }
        };
        scan(g_snap_data.data(), DATA_AT, DATA_SIZE);
        scan(g_snap_arena.data(), ARENA, arena_used());
    }
    L("= %s -> %x", reg ? reg->name : "?", r);
    return r;
}
// a virtual call: the slot's address, then call() (so the isolated pass calls that function's rewrite)
static uint32_t vslot(uint32_t obj, uint32_t off) { return *(uint32_t*)(uintptr_t)(*(uint32_t*)(uintptr_t)obj + off); }
static uint32_t vcall_(uint32_t obj, uint32_t off, std::initializer_list<uint32_t> rest, int ret = 32) {
    const uint32_t fn = vslot(obj, off);
    std::vector<uint32_t> v;
    v.push_back(obj);
    v.insert(v.end(), rest.begin(), rest.end());
    uint32_t a[16];
    for (size_t i = 0; i < v.size(); i++) a[i] = v[i];
    Reg* reg = reg_at(fn);
    if (reg) {
        switch (v.size()) {
        case 1: return call(fn, {a[0]}, ret);
        case 2: return call(fn, {a[0], a[1]}, ret);
        case 3: return call(fn, {a[0], a[1], a[2]}, ret);
        case 4: return call(fn, {a[0], a[1], a[2], a[3]}, ret);
        }
    }
    // a function of another group (B's deleting destructors and stubs): the image's, as thiscall
    if (g_fault) return 0;
    uint32_t r = 0;
    const int code = seh_call(fn, true, a, (int)v.size(), &r);
    if (ret == 8) r &= 0xff;
    if (code) {
        L("FAULT %08x in %x", (unsigned)code, fn);
        g_fault = true;
        return 0;
    }
    L("= %x -> %x", fn, ret ? r : 0);
    return ret ? r : 0;
}

// ---- the arena's fixed part: strings and buffers the scenarios hand the game ------------------------------------------------
static uint32_t fixed_alloc(uint32_t n) {
    const uint32_t a = (g_fixed + 15) & ~15u;
    g_fixed = a + n;
    return a;
}
static uint32_t fixed_str(const char* s) {
    const uint32_t a = fixed_alloc((uint32_t)strlen(s) + 1);
    strcpy((char*)(uintptr_t)a, s);
    return a;
}
static uint32_t fixed_bytes(int n, int fill_kind) {
    const uint32_t a = fixed_alloc((uint32_t)n + 16);
    for (int i = 0; i < n + 16; i++) ((uint8_t*)(uintptr_t)a)[i] = fill_kind ? (uint8_t)g_drv.next() : 0;
    return a;
}
static uint32_t& U(uint32_t a) { return *(uint32_t*)(uintptr_t)a; }

// ---- scenarios ----------------------------------------------------------------------------------------------------------
using namespace nt;
static void random_addr(uint32_t a, uint32_t like) {
    for (int i = 0; i < 12; i++) ((uint8_t*)(uintptr_t)a)[i] = (uint8_t)g_drv.next();
    if (like && g_drv.chance(50)) memcpy((void*)(uintptr_t)a, (void*)(uintptr_t)like, 12);
    else if (like && g_drv.chance(50)) memcpy((void*)(uintptr_t)a, (void*)(uintptr_t)like, 4);
}

// the small functions on random arguments
static void scen_small() {
    for (int k = 0; k < 40 && !g_fault; k++) {
        switch (g_drv.range(0, 14)) {
        case 0: {
            const int n = g_drv.range(-2, 300);
            const uint32_t b = fixed_bytes(n > 0 ? n : 1, 1);
            call(A_CRC16, {b, (uint32_t)n}, 16);
            break;
        }
        case 1: {
            const uint32_t d = fixed_bytes(0x1c, 1);
            call(A_init_dcb, {d, (uint32_t)g_drv.next()}, 0);
            break;
        }
        case 2: case 3: {
            const uint32_t x = fixed_bytes(12, 1), y = fixed_bytes(12, 1);
            if (g_drv.chance(50)) memcpy((void*)(uintptr_t)y, (void*)(uintptr_t)x, 12);
            if (g_drv.chance(30)) ((uint8_t*)(uintptr_t)y)[g_drv.range(0, 11)] ^= 1;
            call(g_drv.chance(50) ? A_socket_addr_eq : 0x004ae370, {x, y}, 8);
            break;
        }
        case 4: call(A_lds_str, {(uint32_t)g_drv.range(0, 11)}); break;
        case 5: call(0x004a1bc0, {(uint32_t)g_drv.range(0, 11)}); break;                  // LineStatusText
        case 6:
            U(S_TAPI_RESULT) = g_drv.chance(50) ? 0x8000004bu : g_drv.next();
            call(0x004a1130, {fixed_bytes(0x50, 1)});                                      // LineDeviceInfo::GetError
            break;
        case 7: call(A_err2str1, {g_drv.next()}); break;
        case 8: call(A_tapierr + 0x10 * (uint32_t)g_drv.range(0, 3), {g_drv.next()}); break;
        case 9:
            call(0x004a1bb0, {});                                                          // DBGLineBlocksFree
            call(0x004a1670, {}, 0);                                                       // PreLineBegin
            call(0x004a1680, {}, 0);                                                       // PreLineEnd
            call(0x004aecf0, {g_drv.next(), g_drv.next()}, 0);                             // dump_callstatedetail
            call(0x004aed00, {fixed_bytes(0x40, 1)}, 0);                                   // dump_modemsettings
            call(0x004aed10, {fixed_bytes(0x40, 1)}, 0);                                   // dump_devstatus
            call(A_tapidev_in_use, {g_drv.next()}, 8);
            call(0x004ae440, {fixed_bytes(0x40, 1), fixed_bytes(0x414, 1)}, 0);            // Socket::AsyncCancel
            {
                const uint32_t o = fixed_bytes(0x40, 1);                                   // Socket / ISocket deleting
                call(g_drv.chance(50) ? 0x004ae560 : 0x004ae580, {o, 0}, 32);             // destructors, kept
                call(g_drv.chance(50) ? 0x004a1ff0 : 0x004a1fb0, {o, 0}, 32);             // and the line devices'
            }
            break;
        case 10: {
            const uint32_t info = fixed_bytes(0x50, 1), name = fixed_str(g_drv.chance(80) ? "Modem 3" :
                "a name longer than sixty-four characters, so that strncpy leaves it unterminated");
            if (g_drv.chance(50)) call(A_init_devinfo_as_tapiline, {info, name, g_drv.next(), g_drv.next() & 0xff, g_drv.next() & 0xff}, 0);
            else call(A_init_devinfo_as_comport, {info, name, g_drv.next(), g_drv.next() & 0xff}, 0);
            break;
        }
        case 11: {
            const uint32_t buf = fixed_bytes(0x40, 0);
            if (g_drv.chance(50)) call(0x004ae5b0, {0, fixed_bytes(12, 1), buf}, 0);        // UDPSocket::MakeStr
            else call(0x004ae680, {0, fixed_bytes(12, 1), buf}, 0);                         // IPXSocket::MakeStr
            break;
        }
        case 12: {
            const uint32_t pk = fixed_bytes(0x14, 1);
            call(A_LinePacketizer_ctor, {pk, g_drv.next()});
            call(A_LinePacketizer_GetHeaderSize, {pk});
            break;
        }
        case 13: {                                                   // LineDevice's own Ok and deleting destructor
            const uint32_t d = fixed_bytes(0x10, 1);
            U(d) = VT_LineDevice;
            U(d + 4) = g_drv.chance(30) ? 0 : g_drv.next();
            call(0x004a1fa0, {d}, 8);
            call(0x004a1fb0, {d, 0}, 32);
            break;
        }
        default: {
            const uint32_t lc = fixed_bytes(0x28, 1);
            call(0x004ae9f0, {lc}, 8);                                                     // LineChecker::LineOk
            if (g_drv.chance(30)) U(lc + 0x20) = U(lc + 0x1c);
            call(0x004aec60, {lc}, 8);                                                     // LineChecker::IsServer
            break;
        }
        }
    }
}

// UDP and IPX sockets
static const char* const k_hosts[] = {"192.168.1.20", "10.0.0.1", "viper-host", "255.255.255.255", "bad.name.example", "1.2.3"};
static void socket_ops(uint32_t s, bool ipx, int n) {
    const uint32_t buf = fixed_bytes(0x200, 0), lenp = fixed_alloc(4), addr = fixed_bytes(12, 1), addr2 = fixed_bytes(12, 1);
    const uint32_t str = fixed_bytes(0x40, 0), rq = fixed_bytes(0x414, 1);
    bool pending = false;
    for (int k = 0; k < n && !g_fault; k++) {
        switch (g_drv.range(0, 12)) {
        case 0: case 1: {
            const uint32_t data = fixed_bytes(g_drv.range(1, 200), 1);
            random_addr(addr, 0);
            vcall_(s, SO_SEND, {data, (uint32_t)g_drv.range(0, 200), addr}, 0);
            break;
        }
        case 2: case 3:
            U(lenp) = (uint32_t)g_drv.range(0, 0x200);
            vcall_(s, SO_RECV, {buf, lenp, g_drv.chance(70) ? addr : 0}, 8);
            break;
        case 4: vcall_(s, SO_GET_HOST_ADDR, {fixed_str(k_hosts[g_drv.range(0, 5)]), addr}, 8); break;
        case 5:
            if (!ipx && !pending) {
                vcall_(s, SO_ASYNC_GET_HOST_ADDR, {fixed_str(k_hosts[g_drv.range(0, 5)]), rq}, 8);
                pending = U(rq + 4) == 0;
                if (pending && g_drv.chance(60)) {                   // the reply, at once
                    const uint32_t lparam = g_drv.chance(70) ? 0x100u : (10060u << 16);
                    call(A_async_msg_hook, {0xf400 + U(s + 4), U(rq), lparam}, 8);
                    pending = false;
                }
            } else if (pending) {
                if (g_drv.chance(40)) vcall_(s, SO_ASYNC_CANCEL, {rq}, 0);
                else {
                    const uint32_t slot = U(s + 4);
                    const uint32_t lparam = g_drv.chance(70) ? 0x100u : (10060u << 16);
                    call(A_async_msg_hook, {0xf400 + slot, U(rq), lparam}, 8);
                }
                pending = false;
            } else {
                vcall_(s, SO_ASYNC_GET_HOST_ADDR, {fixed_str("x"), rq}, 8);                // Socket's default
            }
            break;
        case 6: call(A_async_msg_hook, {g_drv.next() & 0xffff, g_drv.next(), g_drv.next()}, 8); break;   // not ours
        case 7:
            vcall_(s, SO_GET_MY_ADDR, {addr2}, 0);
            random_addr(addr, addr2);
            vcall_(s, SO_ADDRESS_IS_LOCAL, {addr}, 8);
            break;
        case 8: vcall_(s, SO_MAKE_STR, {addr, str}, 0); break;
        case 9: vcall_(s, SO_GET_BROADCAST_ADDR, {addr}, 0); break;
        case 10: vcall_(s, SO_GET_HEADER_SIZE, {}); vcall_(s, SO_GET_BPS, {}); break;
        case 11: vcall_(s, SO_OK, {}, 8); vcall_(s, SO_LINK_ALIVE, {}, 8); break;
        default: call(A_err2str0, {}); break;
        }
    }
    if (pending) vcall_(s, SO_ASYNC_CANCEL, {rq}, 0);
}
static void scen_sockets() {
    std::vector<uint32_t> live;
    for (int round = 0; round < 6 && !g_fault; round++) {
        const int what = g_drv.range(0, 9);
        if (what < 4 && live.size() < 3) {
            uint32_t s = 0;
            switch (g_drv.range(0, 3)) {
            case 0: s = call(0x004ae2f0, {}); break;                                       // SocketCreateUDP()
            case 1: s = call(A_SocketCreateUDP, {0x7d1}); break;
            case 2: s = call(0x004ae390, {}); break;                                       // SocketCreateIPX()
            default: s = call(A_SocketCreateIPX, {0x7d1}); break;
            }
            if (s) {
                live.push_back(s);
                socket_ops(s, U(s) == VT_IPXSocket, g_drv.range(5, 25));
            }
        } else if (what < 7 && !live.empty()) {
            const int i = g_drv.range(0, (int)live.size() - 1);
            socket_ops(live[i], U(live[i]) == VT_IPXSocket, g_drv.range(3, 15));
        } else if (what == 7) {
            call(A_set_autodial, {g_drv.next() & 1}, 8);
        } else if (!live.empty()) {
            const int i = g_drv.range(0, (int)live.size() - 1);
            vcall_(live[i], SO_DTOR, {1});
            live.erase(live.begin() + i);
        }
    }
    while (!live.empty() && !g_fault) {
        vcall_(live.back(), SO_DTOR, {1});
        live.pop_back();
    }
    // the constructors on raw memory too (a failed grab, an IPX socket's fields left as MemAlloc left them)
    if (!g_fault && g_drv.chance(50)) {
        const uint32_t m = fixed_bytes(0x40, 1);
        if (g_drv.chance(50)) call(A_UDPSocket_ctor, {m, g_drv.next()});
        else call(A_IPXSocket_ctor, {m, g_drv.next()});
        vcall_(m, SO_OK, {}, 8);
        if (U(m) == VT_UDPSocket) call(A_UDPSocket_dtor, {m}, 0);
        else call(A_IPXSocket_dtor, {m}, 0);
    }
}

// the COM ports: a LineDeviceInfo list, two DirectLines on the null-modem pair, LineSockets and LineCheckers on both ends
static void ports_setup(bool noise) {
    for (int i = 0; i < 4; i++) {
        Port& p = g_w.port[i];
        p.exists = !g_drv.chance(15) || i < 2;
        p.busy = !p.exists ? false : g_drv.chance(10) && i >= 2;
        p.open_err = g_drv.chance(70) ? 2 : 1450;
        p.noise = noise ? g_drv.range(0, 3) : 0;
        p.short_reads = g_drv.chance(40);
        p.modem = g_drv.chance(80) ? 0xb0 : 0x30;
    }
    g_w.port[0].peer = 1;
    g_w.port[1].peer = 0;
}
static bool g_flood;                             // this scenario sends to itself a lot (the local queue overflows)
static void line_traffic(uint32_t sock[2], uint32_t chk[2], uint32_t dev[2], int n, bool checker) {
    const uint32_t buf = fixed_bytes(0x140, 0), lenp = fixed_alloc(4), addr = fixed_bytes(12, 1), myaddr = fixed_bytes(12, 0);
    for (int k = 0; k < n && !g_fault; k++) {
        const int e = g_drv.range(0, 1);
        switch (g_flood && g_drv.chance(40) ? 0 : g_drv.range(0, 14)) {
        case 0: case 1: case 2: {                                    // a packet to the other end (or to itself / everyone)
            if (!sock[e]) break;
            const int to = g_drv.range(0, 99);
            const bool other = to < (g_flood ? 30 : 70);
            // (an empty packet to itself is never sent: two of them make the local queue a cycle -- alloc_pkt takes a slot
            // whose length is 0 as free, though it's queued -- and the next enqueue never ends, in the original as here)
            const int len = g_drv.range(other ? 0 : 1, 0xe0);
            const uint32_t data = fixed_bytes(len, 1);
            vcall_(sock[e], SO_GET_MY_ADDR, {myaddr}, 0);
            U(addr) = other ? ~U(myaddr) : to < 85 ? U(myaddr) : 0xffffffffu;
            vcall_(sock[e], SO_SEND, {data, (uint32_t)len, addr}, 0);
            break;
        }
        case 3: case 4: case 5:
            if (!sock[e]) break;
            U(lenp) = g_drv.chance(85) ? 0x100u : (uint32_t)g_drv.range(0, 0x100);
            vcall_(sock[e], SO_RECV, {buf, lenp, g_drv.chance(80) ? addr : 0}, 8);
            break;
        case 6:
            if (checker && chk[e]) call(0x004aea10, {chk[e]}, 0);                          // LineChecker::Tick
            break;
        case 7:
            if (checker && chk[e]) {
                call(0x004ae9f0, {chk[e]}, 8);
                call(A_LineChecker_DoneChecking, {chk[e]}, 8);
                if (U(chk[e] + 0x1c) != U(chk[e] + 0x20)) call(0x004aec60, {chk[e]}, 8);
            }
            break;
        case 8: g_w.port[e].modem ^= 0x80; break;                    // the other end's DCD drops or comes back
        case 9:
            if (dev[e]) {
                const int w = g_drv.range(0, 2);
                if (w == 0) vcall_(dev[e], LD_CALL, {fixed_str("5551234")}, 0);
                else if (w == 1) vcall_(dev[e], LD_ANSWER, {}, 0);
                else vcall_(dev[e], LD_RESET, {}, 0);
            }
            break;
        case 10:
            if (dev[e]) {
                vcall_(dev[e], LD_GET_STATUS, {});
                vcall_(dev[e], LD_DATA_AVAIL, {});
                vcall_(dev[e], LD_DATA_PENDING, {});
            }
            break;
        case 11:
            if (sock[e]) {
                vcall_(sock[e], SO_LINK_ALIVE, {}, 8);
                vcall_(sock[e], SO_GET_BPS, {});
                vcall_(sock[e], SO_GET_HEADER_SIZE, {});
                vcall_(sock[e], SO_OK, {}, 8);
                vcall_(sock[e], SO_GET_BROADCAST_ADDR, {addr}, 0);
                vcall_(sock[e], SO_ADDRESS_IS_LOCAL, {addr}, 8);
                vcall_(sock[e], SO_MAKE_STR, {addr, buf}, 0);
                vcall_(sock[e], SO_ASYNC_GET_HOST_ADDR, {buf, buf}, 8);
                if (g_drv.chance(10)) vcall_(sock[e], SO_GET_HOST_ADDR, {buf, addr}, 8);     // a panic
            }
            break;
        case 12:
            if (dev[e]) {
                vcall_(dev[e], LD_BPS, {});
                vcall_(dev[e], LD_OK, {}, 8);
                vcall_(dev[e], LD_CAN_DESTROY_SAFELY, {}, 8);
                vcall_(dev[e], LD_SHUTDOWN, {}, 0);
            }
            break;
        case 13: call(A_free_completed_sendblocks, {}, 0); call(0x004a1bb0, {}); break;
        default:
            if (dev[e]) {                                            // the device straight: a raw send and receive
                const int len = g_drv.range(1, 0x40);
                vcall_(dev[e], LD_SEND, {fixed_bytes(len, 1), (uint32_t)len}, 0);
                U(lenp) = (uint32_t)g_drv.range(0, 0x40);
                vcall_(dev[e], LD_RECV, {buf, lenp}, 8);
            }
            break;
        }
    }
}
static void scen_direct(bool checker) {
    ports_setup(!checker);
    g_flood = g_drv.chance(25);
    g_w.clock_step = g_drv.chance(50) ? 1500 : 400;             // (fast: the line checks run out of time)
    call(0x004a1420, {}, 8);                                         // LineBegin (no TAPI devices here)
    const uint32_t info = fixed_bytes(0x50 * 8, 1);
    const int count = (int)call(0x004a1690, {info, (uint32_t)g_drv.range(1, 6), 2});   // LineEnumerateDevices(COM ports)
    uint32_t dev[2] = {0, 0}, sock[2] = {0, 0}, chk[2] = {0, 0};
    for (int e = 0; e < 2 && !g_fault; e++) {
        // COM1 and COM2: the enumerated entries when they're there, else made by hand
        uint32_t entry = 0;
        for (int i = 0; i < count && i < 8; i++)
            if (U(info + 0x50u * (uint32_t)i + 0x48) == (uint32_t)(e + 1)) entry = info + 0x50u * (uint32_t)i;
        if (!entry) {
            entry = fixed_bytes(0x50, 0);
            call(A_init_devinfo_as_comport, {entry, fixed_str(e ? "COM2" : "COM1"), (uint32_t)(e + 1), 0}, 0);
        }
        dev[e] = call(A_CreateDirectLine, {entry});
        if (!dev[e]) continue;
        if (g_drv.chance(80)) vcall_(dev[e], LD_CALL, {fixed_str("")}, 0);
        if (checker) {
            chk[e] = fixed_bytes(0x28, 1);
            call(0x004ae9b0, {chk[e], dev[e]});                      // LineChecker::LineChecker
        }
        sock[e] = call(0x004ae3f0, {dev[e]});                        // SocketCreateLine (owns the device from here)
        if (!sock[e]) dev[e] = U(dev[e]) == VT_DirectLine || U(dev[e]) == VT_LineDevice ? dev[e] : 0;
    }
    line_traffic(sock, chk, dev, checker ? 160 : 120, checker);
    for (int e = 0; e < 2 && !g_fault; e++) {
        if (sock[e]) vcall_(sock[e], SO_DTOR, {1});                  // the socket deletes its packetizer and device
        else if (dev[e]) vcall_(dev[e], LD_DTOR, {1});
    }
    call(0x004a1600, {}, 0);                                         // LineEnd
}

// TAPI modems
static void tapi_event(uint32_t line) {
    static const uint32_t states[] = {1, 2, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x100, 0x200, 0x400, 0x800, 0x1000,
                                      0x2000, 0x4000, 0x8000, 0x10000, 3};
    uint32_t msg, p1 = 0, p2 = 0, p3 = 0;
    const int k = g_drv.range(0, 9);
    if (k < 5) {
        msg = 2;
        p1 = states[g_drv.range(0, 19)];
        p2 = g_drv.chance(50) ? 0x123 : 0;
        p3 = g_drv.chance(80) ? 4 : 2;
    } else if (k < 8) {
        msg = 0xc;
        const uint32_t offs[] = {0x20, 0x24, 0x28, 0x2c, 0x30};
        p1 = g_drv.chance(80) ? U(line + offs[g_drv.range(0, 4)]) : g_drv.next();
        p2 = g_drv.chance(60) ? 0 : 0x80000005u;
    } else if (k == 8) {
        msg = 3;
    } else {
        msg = g_drv.range(0, 20);
    }
    const uint32_t hdev = g_drv.chance(50) ? 0x9100 : 0xc0de;
    call(A_tapi_cb, {hdev, msg, g_drv.chance(90) ? line : 0, p1, p2, p3}, 0);
}
static void scen_tapi() {
    ports_setup(false);
    g_w.ndevs = g_drv.chance(10) ? 0 : g_drv.range(1, 3);
    g_w.port[4].modem = 0x80;
    g_w.port[5].modem = 0;
    g_w.port[4].peer = 5;                                            // the two modems' ports talk to each other
    g_w.port[5].peer = 4;
    const uint8_t ok = (uint8_t)call(0x004a1420, {}, 8);             // LineBegin
    const uint32_t info = fixed_bytes(0x50 * 8, 1);
    const int count = (int)call(0x004a1690, {info, (uint32_t)g_drv.range(0, 8), g_drv.chance(80) ? 1u : (uint32_t)g_drv.range(2, 3)});
    (void)ok;
    std::vector<uint32_t> lines;
    for (int i = 0; i < count && i < 8 && !g_fault; i++) {
        const uint32_t e = info + 0x50u * (uint32_t)i;
        if (U(e + 0x44) != A_CreateTAPILine && !g_drv.chance(20)) continue;
        const uint32_t d = call(U(e + 0x44), {e});
        if (d) lines.push_back(d);
    }
    if (lines.empty() && !g_fault) {                                 // a TAPILine on its own
        const uint32_t m = call(A_TAPILine_ctor, {fixed_bytes(0x40, 1), (uint32_t)g_drv.range(0, 3)});
        if (m) lines.push_back(m);
    }
    const uint32_t buf = fixed_bytes(0x140, 0), lenp = fixed_alloc(4);
    for (int k = 0; k < 120 && !g_fault && !lines.empty(); k++) {
        const uint32_t l = lines[(size_t)g_drv.range(0, (int)lines.size() - 1)];
        if (U(l) != VT_TAPILine && U(l) != VT_DirectLine) continue;
        switch (g_drv.chance(25) ? 0 : g_drv.range(0, 11)) {
        case 0: case 1: case 2:
            if (U(l) == VT_TAPILine) tapi_event(l);
            break;
        case 3: vcall_(l, LD_CALL, {fixed_str("5551234")}, 0); break;
        case 4: vcall_(l, LD_ANSWER, {}, 0); break;
        case 5: vcall_(l, LD_RESET, {}, 0); break;
        case 6:
            vcall_(l, LD_OK, {}, 8);
            vcall_(l, LD_BPS, {});
            vcall_(l, LD_CAN_DESTROY_SAFELY, {}, 8);
            vcall_(l, LD_GET_STATUS, {});
            break;
        case 7:
            if (U(l) == VT_TAPILine) {
                call(0x004a0810, {l}, 8);                            // detect_cd
                call(A_TAPILine_update_callbps, {l}, 0);
                call(A_TAPILine_get_handle, {l}, 8);
            }
            break;
        case 8: {
            const int len = g_drv.range(1, 0x60);
            vcall_(l, LD_SEND, {fixed_bytes(len, 1), (uint32_t)len}, 0);
            break;
        }
        case 9:
            vcall_(l, LD_DATA_AVAIL, {});
            U(lenp) = (uint32_t)g_drv.range(0, 0x100);
            vcall_(l, LD_RECV, {buf, lenp}, 8);
            vcall_(l, LD_DATA_PENDING, {});
            break;
        case 10: vcall_(l, LD_SHUTDOWN, {}, 0); break;
        default:
            call(0x004a1130, {info}, 32);                            // LineDeviceInfo::GetError
            call(0x004a1bc0, {(uint32_t)U(l + 4) % 12u});            // LineStatusText
            break;
        }
    }
    for (uint32_t l : lines)
        if (!g_fault) vcall_(l, LD_DTOR, {1});
    call(0x004a1600, {}, 0);                                         // LineEnd
    if (!g_fault && g_drv.chance(50)) call(0x004a1420, {}, 8);
}
static void scen_misc() {
    // kill_thread: a task that dies soon, late, never (12 s: a panic), or is ours
    U(S_THREAD) = g_drv.chance(80) ? 9 : 0;
    g_w.task_id = g_drv.chance(80) ? 7 : 9;
    g_w.task_dead_after = g_drv.chance(20) ? 1000 : g_drv.range(0, 5);
    call(A_kill_thread, {}, 0);
    // the send blocks without a device: allocate until they run out, poll, the free count
    if (g_drv.chance(60)) call(0x004a1420, {}, 8);                   // LineBegin (the blocks free, their events)
    for (int i = 0; i < 20 && !g_fault; i++) {
        if (g_drv.chance(70)) call(A_alloc_sendblock, {0x5000u + (uint32_t)g_drv.range(0, 3)});
        else call(A_free_completed_sendblocks, {}, 0);
    }
    call(0x004a1bb0, {});
    // winsock's count straight
    for (int i = 0; i < 6 && !g_fault; i++) {
        if (g_drv.chance(50) || U(S_GRABS) == 0) call(A_winsock_grab, {}, 8);
        else call(A_winsock_release, {}, 0);
    }
    call(A_set_autodial, {g_drv.next() & 1}, 8);
    // the TAPI pieces straight
    call(A_set_from_bytestream, {(uint32_t)g_drv.range(0, 3)}, 8);
    const uint32_t caps = fixed_bytes(0x400, 0);
    U(caps) = 0x400;
    if (!f_lineGetDevCaps(0x7a00, 1, 0x10004, 0, (uint8_t*)(uintptr_t)caps)) call(A_get_tapiline_port, {caps});
    g_log.push_back("(the scenario's own lineGetDevCaps)");
}

// ---- passes ------------------------------------------------------------------------------------------------------------
// what the originals' logs show happened, so a run says it reached the interesting paths
static const struct { const char* text; const char* what; } k_events[] = {
    {"= LineSocket::Recv -> 1", "line packets received"},
    {"LinePacketizer: packet too long", "headers rejected (length)"},
    {"user buffer too small", "packets longer than the buffer"},
    {"= LineChecker::LineOk -> 1", "line checks passed"},
    {"= LineChecker::IsServer -> 1", "line check servers"},
    {"QUEUE_N", "local queue overflows (panic)"},
    {"= UDPSocket::Recv -> 1", "UDP datagrams received"},
    {"= IPXSocket::Recv -> 1", "IPX datagrams received"},
    {"Async succeeds", "async lookups answered"},
    {"Async failed", "async lookups failed"},
    {"= TAPILine::Answer", "TAPI answers"},
    {"lineMakeCall", "TAPI calls made"},
    {"Bizarre", "offered while calling"},
    {"= CreateTAPILine -> 0", "TAPI lines that failed"},
    {"LINE_REPLY: request id does not match", "unmatched replies"},
    {"Yes, your worst nightmare", "device configurations read back changed"},
    {"Grak!", "kill_thread panics"},
    {"overlapped serial IO results in ABANDONED", "abandoned writes"},
    {"Unrecognized overlapped result", "unknown wait results (panic)"},
    {"LineSocket::GetHostAddr() -- are you kidding", "line host lookups (panic)"},
    {"socket: QVE Type", "autodial values of another type"},
};
enum { N_EVENTS = sizeof k_events / sizeof k_events[0] };
static long g_event_count[N_EVENTS];

struct PassResult { std::vector<std::string> log; std::vector<uint8_t> data, arena; };
static void run_pass(Mode mode, int kind, uint32_t seed, PassResult& out) {
    memcpy((void*)(uintptr_t)DATA_AT, g_pristine.data(), DATA_SIZE);
    memset((void*)(uintptr_t)ARENA, 0, ARENA_SIZE);
    world_reset(seed);
    g_drv.s = seed * 747796405u + 2891336453u;
    if (!g_drv.s) g_drv.s = 7;
    g_heap = HEAP_AT;
    g_fixed = ARENA + 0x100;
    g_log.clear();
    g_fault = false;
    g_mode = mode;
    if (mode == CHAIN) chain_patch();
    switch (kind) {
    case 0: scen_small(); break;
    case 1: scen_sockets(); break;
    case 2: scen_direct(false); break;
    case 3: scen_direct(true); break;
    case 4: scen_tapi(); break;
    default: scen_misc(); break;
    }
    if (mode == CHAIN) chain_unpatch();
    out.log = g_log;
    out.data.assign((uint8_t*)(uintptr_t)DATA_AT, (uint8_t*)(uintptr_t)DATA_AT + DATA_SIZE);
    out.arena.assign((uint8_t*)(uintptr_t)ARENA, (uint8_t*)(uintptr_t)ARENA + ARENA_SIZE);
}
static int compare(const char* what, const PassResult& a, const PassResult& b, int kind, uint32_t seed, bool quiet = false) {
    const size_t n = a.log.size() < b.log.size() ? a.log.size() : b.log.size();
    for (size_t i = 0; i < n; i++)
        if (a.log[i] != b.log[i]) {
            if (quiet) return 1;
            printf("  %s: kind %d seed %u: log line %u differs:\n    original: %s\n    rewrite:  %s\n", what, kind, seed, (unsigned)i,
                   a.log[i].c_str(), b.log[i].c_str());
            for (size_t j = i >= 3 ? i - 3 : 0; j < i; j++) printf("    (before: %s)\n", a.log[j].c_str());
            return 1;
        }
    if (a.log.size() != b.log.size()) {
        if (quiet) return 1;
        printf("  %s: kind %d seed %u: log lengths %u / %u (next: %s)\n", what, kind, seed, (unsigned)a.log.size(),
               (unsigned)b.log.size(), a.log.size() > n ? a.log[n].c_str() : b.log[n].c_str());
        return 1;
    }
    for (size_t i = 0; i < a.data.size(); i++)
        if (a.data[i] != b.data[i]) {
            if (quiet) return 1;
            printf("  %s: kind %d seed %u: .data differs at %08x (%02x / %02x)\n", what, kind, seed, (unsigned)(DATA_AT + i),
                   a.data[i], b.data[i]);
            return 1;
        }
    for (size_t i = 0; i < a.arena.size(); i++)
        if (a.arena[i] != b.arena[i]) {
            if (quiet) return 1;
            printf("  %s: kind %d seed %u: the arena differs at %08x (%02x / %02x)\n", what, kind, seed, (unsigned)(ARENA + i),
                   a.arena[i], b.arena[i]);
            return 1;
        }
    return 0;
}

// the fix build: a rewrite pass against the originals'. Without its fixes' marks (and the modem key's close) the same is
// the same; else the logs agree up to the first "FIX" line and the rewrite pass ends cleanly after it.
struct FixCount { std::string what; int n; };
static std::vector<FixCount> g_fixc;
static int compare_fixed(const char* what, const PassResult& a, const PassResult& b, int kind, uint32_t seed) {
    PassResult c;
    c.data = b.data;
    c.arena = b.arena;
    size_t first_fix = (size_t)-1;
    for (const std::string& l : b.log) {
        if (l.rfind("RegCloseKey(modem key)", 0) == 0) continue;
        if (l.rfind("FIX ", 0) == 0) {
            if (first_fix == (size_t)-1) first_fix = c.log.size();
            continue;
        }
        c.log.push_back(l);
    }
    if (first_fix == (size_t)-1) return compare(what, a, c, kind, seed);
    if (!compare(what, a, c, kind, seed, true)) return 0;
    for (size_t i = 0; i < first_fix; i++)
        if (i >= a.log.size() || a.log[i] != c.log[i]) {
            printf("  %s: kind %d seed %u: log line %u differs before the first fix:\n    original: %s\n    rewrite:  %s\n", what,
                   kind, seed, (unsigned)i, i < a.log.size() ? a.log[i].c_str() : "(none)", c.log[i].c_str());
            return 1;
        }
    std::string fix;
    for (const std::string& l : b.log)
        if (l.rfind("FIX ", 0) == 0) { fix = l.substr(4); break; }
    for (const std::string& l : b.log)
        if (l.rfind("FAULT", 0) == 0) {
            printf("  %s: kind %d seed %u: after the fix (%s) the rewrite pass ended with a %s\n", what, kind, seed, fix.c_str(), l.c_str());
            return 1;
        }
    bool found = false;
    for (FixCount& f : g_fixc)
        if (f.what == fix) f.n++, found = true;
    if (!found) g_fixc.push_back({fix, 1});
    return 0;
}
#if NET_FIXES
// ---- directed_fix_tests --------------------------------------------------------------------------------------------------
// Each fix's bad case run as a small pass on the originals (ORIG) and on the rewrites (ISOLATED: the top-level calls the
// rewrites, their callees the originals'), from the same start; the originals' failure shown, the rewrite's result checked;
// then the boundary case on both, compared like a scenario.
static int g_dt_bad, g_dt_n;
static void dt_check(bool ok, const char* name, const char* what) {
    g_dt_n++;
    printf("  fix test %-30s %s  %s\n", name, ok ? "ok    " : "FAILED", what);
    if (!ok) g_dt_bad++;
}
static void dt_knobs_off() {
    g_dt_naddr = 0;
    g_dt_wsa_fail = false;
    g_dt_ok = false;
    g_dt_vs_off = g_dt_vs_size = 0;
    g_dt_wait = 0;
}
static void dt_begin(Mode mode) {
    memcpy((void*)(uintptr_t)DATA_AT, g_pristine.data(), DATA_SIZE);
    memset((void*)(uintptr_t)ARENA, 0, ARENA_SIZE);
    world_reset(99);
    g_drv.s = 12345;
    g_heap = HEAP_AT;
    g_fixed = ARENA + 0x100;
    g_log.clear();
    g_fault = false;
    g_mode = mode;
    g_check_fp = false;
    g_fix_hits = 0;
}
static PassResult dt_end() {
    PassResult r;
    r.log = g_log;
    r.data.assign((uint8_t*)(uintptr_t)DATA_AT, (uint8_t*)(uintptr_t)DATA_AT + DATA_SIZE);
    r.arena.assign((uint8_t*)(uintptr_t)ARENA, (uint8_t*)(uintptr_t)ARENA + ARENA_SIZE);
    return r;
}
static bool has_line(const char* prefix) {
    for (const std::string& l : g_log)
        if (l.rfind(prefix, 0) == 0) return true;
    return false;
}
static bool faulted() { return has_line("FAULT"); }
// the boundary: the same calls on the originals and the rewrites, alike (compare's rules, the fix's marks not expected)
static bool dt_alike(void (*body)()) {
    dt_begin(ORIG);
    body();
    const PassResult o = dt_end();
    dt_begin(ISOLATED);
    body();
    const PassResult n = dt_end();
    return !compare("boundary", o, n, -1, 0) && !g_fix_hits;
}
// an original that never returns: run on a thread, stopped after `ms` (it only reads)
struct RawCall { uint32_t fn; uint32_t a[6]; int n; bool thiscall; };
static DWORD WINAPI raw_thread(void* p) {
    RawCall* r = (RawCall*)p;
    uint32_t x;
    seh_call(r->fn, r->thiscall, r->a, r->n, &x);
    return 0;
}
static bool raw_hangs(uint32_t fn, bool thiscall, std::initializer_list<uint32_t> args, int ms) {
    RawCall r = {fn, {}, 0, thiscall};
    for (uint32_t v : args) r.a[r.n++] = v;
    HANDLE t = CreateThread(0, 0x100000, raw_thread, &r, 0, 0);
    const bool hung = WaitForSingleObject(t, (DWORD)ms) == WAIT_TIMEOUT;
    if (hung) TerminateThread(t, 0);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return hung;
}
// a line device of the test's own: its DataAvail and Recv give exactly what the test says
static int g_fd_avail, g_fd_give;
static uint8_t g_fd_bytes[0x100];
static bool g_fd_full_no_nul;                    // the last Recv filled the whole buffer without a NUL
static int32_t __fastcall fd_status(void*, int) { return 2; }
static int32_t __fastcall fd_avail(void*, int) { L("fdev DataAvail -> %d", g_fd_avail); return g_fd_avail; }
static uint8_t __fastcall fd_recv(void*, int, uint8_t* buf, int32_t* n) {
    const int k = *n < g_fd_give ? *n : g_fd_give;
    memcpy(buf, g_fd_bytes, (size_t)(k > 0 ? k : 0));
    g_fd_full_no_nul = k == *n && k > 0 && !memchr(buf, 0, (size_t)k);
    L("fdev Recv %d -> %d %s", *n, k, H(buf, (size_t)(k > 0 ? k : 0)).c_str());
    *n = k;
    return k > 0;
}
static void __fastcall fd_send(void*, int, const void* p, int32_t n) { L("fdev Send %s", H(p, (size_t)n).c_str()); }
static void __fastcall fd_trap(void*, int) { L("fdev: an unexpected call"); RaiseException(0xe0000002, 0, 0, 0); }
static void* g_fd_vt[16];
static uint32_t fake_device() {
    for (int i = 0; i < 16; i++) g_fd_vt[i] = (void*)fd_trap;
    g_fd_vt[LD_GET_STATUS / 4] = (void*)fd_status;
    g_fd_vt[LD_DATA_AVAIL / 4] = (void*)fd_avail;
    g_fd_vt[LD_RECV / 4] = (void*)fd_recv;
    g_fd_vt[LD_SEND / 4] = (void*)fd_send;
    const uint32_t d = fixed_bytes(0x10, 0);
    U(d) = (uint32_t)(uintptr_t)g_fd_vt;
    return d;
}
// a UDPSocket made by hand (as the constructor leaves it, its addresses not yet read)
static uint32_t udp_by_hand(int32_t slot) {
    const uint32_t m = fixed_bytes(0x40, 0);
    U(m) = VT_UDPSocket;
    U(m + 4) = (uint32_t)slot;
    *(uint16_t*)(uintptr_t)(m + 0x30) = 0x7d1;
    *(uint8_t*)(uintptr_t)(m + 0x32) = 1;
    U(m + 0x34) = 0x300;
    return m;
}
static uint32_t line_socket_by_hand() {
    const uint32_t s = fixed_bytes(0x774, 0);
    U(s) = VT_LineSocket;
    U(s + 0x76c) = 1234;
    return s;
}
static uint32_t pkt_bytes(int n, uint8_t v) {
    const uint32_t p = fixed_bytes(n, 0);
    memset((void*)(uintptr_t)p, v, (size_t)n);
    return p;
}

static int directed_fix_tests() {
    printf("directed fix tests (the bad case on the originals, then on the rewrites; the boundary case on both, compared):\n");
    dt_knobs_off();
    char msg[256];

    // ---- UDPSocket::init_local_addrs: 4 and 5 addresses -------------------------------------------------------------------
    for (int naddr = 4; naddr <= 5; naddr++) {
        uint32_t m = 0, nl[2], port[2];
        bool fault[2], local3 = false, local4 = true;
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            g_dt_naddr = naddr;
            m = udp_by_hand(0);
            call(A_UDP_init_local_addrs, {m}, 0);
            nl[pass] = U(m + 0x2c);
            port[pass] = *(uint16_t*)(uintptr_t)(m + 0x30);
            const uint32_t a = fixed_bytes(12, 0);
            U(a) = k_dt_addrs[2];
            const bool l3 = call(0x004add10, {m, a}, 8) != 0;                       // UDPSocket::AddressIsLocal
            U(a) = k_dt_addrs[3];
            const bool l4 = call(0x004add10, {m, a}, 8) != 0;
            fault[pass] = faulted();
            if (pass) local3 = l3, local4 = l4;
            dt_knobs_off();
        }
        sprintf(msg, "%d addresses: the original %s; the rewrite keeps the first 3 (port intact, AddressIsLocal works)", naddr,
                naddr == 4 ? "took the 4th address + 1 as its count (0x08000000 local addresses: AddressIsLocal walks off the socket)"
                           : "wrote the 5th far past the socket (a fault)");
        dt_check((naddr == 5 ? fault[0] : nl[0] == 0x08000000u) && !fault[1] && nl[1] == 3 && port[1] == 0x7d1 && local3 && !local4,
                 "UDPSocket::init_local_addrs", msg);
    }
    dt_check(dt_alike([] {
                 g_dt_naddr = 3;
                 const uint32_t m = udp_by_hand(0);
                 call(A_UDP_init_local_addrs, {m}, 0);
                 const uint32_t a = fixed_bytes(12, 0);
                 U(a) = k_dt_addrs[2];
                 call(0x004add10, {m, a}, 8);
                 dt_knobs_off();
             }),
             "UDPSocket::init_local_addrs", "3 addresses: both alike");

    // ---- IPXSocket: winsock_grab fails ---------------------------------------------------------------------------------
    {
        bool closed[2], fault[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            g_dt_wsa_fail = true;
            const uint32_t m = pkt_bytes(0x40, 0xcd);                                  // as MemAlloc leaves it
            call(A_IPXSocket_ctor, {m, 0x7d1});
            call(A_IPXSocket_dtor, {m}, 0);
            closed[pass] = has_line("closesocket cdcdcdcd");
            fault[pass] = faulted();
            dt_knobs_off();
        }
        dt_check(closed[0] && !closed[1] && !fault[1], "IPXSocket::IPXSocket",
                 "winsock_grab fails: the original's destructor closed the socket field MemAlloc left (cdcdcdcd); the rewrite's is none");
    }

    // ---- async_msg_hook: no request in the slot; a late reply for another request -----------------------------------------
    {
        bool fault[2];
        uint32_t ret1 = 0;
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            U(S_ASYNC_REQS) = 0;
            const uint32_t r = call(A_async_msg_hook, {0xf400, 0x7001, 0x100}, 8);
            fault[pass] = faulted();
            if (pass) ret1 = r;
        }
        dt_check(fault[0] && !fault[1] && ret1 == 1, "async_msg_hook",
                 "a reply with its slot empty (after AsyncCancel): the original wrote through 0; the rewrite takes and ignores it");
        uint32_t st_after_old[2], st_after_new[2];
        bool fault2[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            const uint32_t rq = fixed_bytes(0x414, 0);
            U(rq) = 0x7002;                                                          // the request now in the slot
            U(rq + 4) = 0;
            make_hostent((uint8_t*)(uintptr_t)(rq + 0x14), 1);
            U(S_ASYNC_REQS) = rq;
            call(A_async_msg_hook, {0xf400, 0x7001, 0x100}, 8);                     // the cancelled one's reply, late
            st_after_old[pass] = U(rq + 4);
            call(A_async_msg_hook, {0xf400, 0x7002, 0x100}, 8);                     // its own
            st_after_new[pass] = U(rq + 4);
            fault2[pass] = faulted();
        }
        dt_check(st_after_old[0] == 2 && fault2[0] && st_after_old[1] == 0 && st_after_new[1] == 2 && !fault2[1], "async_msg_hook",
                 "a cancelled request's late reply: the original gave its answer to the new request, then crashed on the new one's "
                 "reply; the rewrite waits for the new one's");
        dt_check(dt_alike([] {
                     const uint32_t rq = fixed_bytes(0x414, 0);
                     U(rq) = 0x7002;
                     make_hostent((uint8_t*)(uintptr_t)(rq + 0x14), 2);
                     U(S_ASYNC_REQS + 4) = rq;
                     call(A_async_msg_hook, {0xf401, 0x7002, 0x100}, 8);
                     U(S_ASYNC_REQS + 8) = rq;
                     call(A_async_msg_hook, {0xf402, 0x7002, 10060u << 16}, 8);
                 }),
                 "async_msg_hook", "its own reply (success, failure): both alike");
    }

    // ---- the async request slots past 4 -------------------------------------------------------------------------------
    {
        uint32_t slot5[2], st[2], grabs[2], r4[2];
        bool asked[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            U(S_ASYNC_REQS + 0x14) = 0;
            const uint32_t m = udp_by_hand(5);
            const uint32_t rq = fixed_bytes(0x414, 0);
            call(0x004ad800, {m, fixed_str("viper-host"), rq}, 8);                  // UDPSocket::AsyncGetHostAddr
            slot5[pass] = U(S_ASYNC_REQS + 0x14);
            st[pass] = U(rq + 4);
            asked[pass] = has_line("WSAAsyncGetHostByName");
            U(S_GRABS) = 2;
            const uint32_t m9 = udp_by_hand(9);
            call(0x004ad7d0, {m9, rq}, 0);                                          // UDPSocket::AsyncCancel (slot 9: S_GRABS)
            grabs[pass] = U(S_GRABS);
            U(S_ASYNC_REQS + 0x10) = 0x5a5a5a5a;
            U(S_GRABS) = 4;
            g_dt_wsa_fail = true;                                                   // (the constructor's slot write is first)
            call(A_UDPSocket_ctor, {fixed_bytes(0x40, 0), 0x7d1});
            r4[pass] = U(S_ASYNC_REQS + 0x10);
            dt_knobs_off();
        }
        dt_check(slot5[0] != 0 && asked[0] && grabs[0] == 0 && r4[0] == 0 && slot5[1] == 0 && st[1] == 1 && !asked[1] &&
                     grabs[1] == 2 && r4[1] == 0x5a5a5a5a,
                 "UDPSocket async slots", "slots 4, 5, 9: the original wrote past the table (slot 9 zeroed winsock_grab's count) and "
                 "asked for a reply nothing takes; the rewrite fails the lookup at once and writes nothing");
        dt_check(dt_alike([] {
                     const uint32_t m = udp_by_hand(3);
                     const uint32_t rq = fixed_bytes(0x414, 0);
                     call(0x004ad800, {m, fixed_str("viper-host"), rq}, 8);
                     call(0x004ad7d0, {m, rq}, 0);
                 }),
                 "UDPSocket async slots", "slot 3: both alike");
    }

    // ---- LineSocket::enqueue: an empty packet; a long one ------------------------------------------------------------
    {
        bool hung = false;
        {
            dt_begin(ORIG);
            const uint32_t s = line_socket_by_hand(), p = pkt_bytes(0x10, 0x11);
            call(A_LineSocket_enqueue, {s, p, 0}, 0);
            call(A_LineSocket_enqueue, {s, p, 5}, 0);
            hung = raw_hangs(A_LineSocket_enqueue, true, {s, p, 5}, 300);
        }
        dt_begin(ISOLATED);
        const uint32_t s = line_socket_by_hand(), p = pkt_bytes(0x10, 0x11);
        call(A_LineSocket_enqueue, {s, p, 0}, 0);
        call(A_LineSocket_enqueue, {s, p, 5}, 0);
        call(A_LineSocket_enqueue, {s, p, 5}, 0);
        const uint32_t h = U(s + 0x770), h2 = h ? U(h + 0xe8) : 0;
        dt_check(hung && !faulted() && h && h2 && h2 != h && !U(h2 + 0xe8) && U(h + 0xe4) == 5 && U(h2 + 0xe4) == 5, "LineSocket::enqueue",
                 "an empty packet to itself, then two more: the original linked a slot to itself and the third enqueue never "
                 "ended; the rewrite drops the empty one");
        uint8_t over[2];
        uint32_t head[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            const uint32_t s2 = line_socket_by_hand(), p2 = pkt_bytes(0x100, 0xa7);
            call(A_LineSocket_enqueue, {s2, p2, 0x100}, 0);
            over[pass] = *(uint8_t*)(uintptr_t)(s2 + 0xc + 0xec);                     // the second slot's first byte
            head[pass] = U(s2 + 0x770);
        }
        dt_check(over[0] == 0xa7 && over[1] == 0 && head[1] == 0, "LineSocket::enqueue",
                 "0x100 bytes: the original wrote into the next slot; the rewrite drops it");
        dt_check(dt_alike([] {
                     const uint32_t s3 = line_socket_by_hand();
                     call(A_LineSocket_enqueue, {s3, pkt_bytes(0xe4, 0x22), 0xe4}, 0);
                     call(A_LineSocket_enqueue, {s3, pkt_bytes(1, 0x23), 1}, 0);
                     const uint32_t buf = fixed_bytes(0x100, 0), n = fixed_alloc(4);
                     U(n) = 0xe4;
                     call(A_LineSocket_dequeue, {s3, buf, n}, 8);
                     U(n) = 1;
                     call(A_LineSocket_dequeue, {s3, buf, n}, 8);
                 }),
                 "LineSocket::enqueue/dequeue", "0xe4 and 1 bytes, buffers that fit: both alike");
        // dequeue: a packet longer than the caller's buffer
        bool beyond[2];
        uint32_t ret[2], head2[2], len0[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            const uint32_t s4 = line_socket_by_hand();
            call(A_LineSocket_enqueue, {s4, pkt_bytes(0x80, 0x5c), 0x80}, 0);
            const uint32_t buf = fixed_bytes(0x100, 0), n = fixed_alloc(4);
            U(n) = 0x10;
            ret[pass] = call(A_LineSocket_dequeue, {s4, buf, n}, 8);
            beyond[pass] = *(uint8_t*)(uintptr_t)(buf + 0x7f) == 0x5c;
            head2[pass] = U(s4 + 0x770);
            len0[pass] = U(s4 + 0xc + 0xe4);
        }
        dt_check(beyond[0] && !beyond[1] && ret[1] == 0 && head2[1] == 0 && len0[1] == 0, "LineSocket::dequeue",
                 "0x80 bytes into a 0x10-byte buffer: the original wrote 0x70 past it; the rewrite drops the packet");
    }

    // ---- LineDevice::Send: a packet past the send block ---------------------------------------------------------------
    {
        uint32_t next_handle[2];
        bool wrote[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            for (int i = 0; i < 16; i++) U(S_BLOCKS + 0xfcu * (uint32_t)i) = 0xffffffffu;
            g_w.port[0].exists = true;
            g_w.handle_port[0] = 1;
            g_w.nhandles = 1;
            const uint32_t d = fixed_bytes(0x10, 0);
            U(d) = VT_DirectLine;
            U(d + 4) = 2;
            U(d + 8) = 0x5000;
            call(A_LineDevice_Send, {d, pkt_bytes(0x200, 0xa7), 0x200}, 0);
            next_handle[pass] = U(S_BLOCKS + 0xfc);
            wrote[pass] = has_line("WriteFile");
        }
        dt_check(next_handle[0] == 0xa7a7a7a7u && wrote[0] && next_handle[1] == 0xffffffffu && !wrote[1], "LineDevice::Send",
                 "0x200 bytes: the original copied them over the next send block; the rewrite drops the packet");
        dt_check(dt_alike([] {
                     for (int i = 0; i < 16; i++) U(S_BLOCKS + 0xfcu * (uint32_t)i) = 0xffffffffu;
                     g_w.port[0].exists = true;
                     g_w.handle_port[0] = 1;
                     g_w.nhandles = 1;
                     const uint32_t d = fixed_bytes(0x10, 0);
                     U(d) = VT_DirectLine;
                     U(d + 4) = 2;
                     U(d + 8) = 0x5000;
                     call(A_LineDevice_Send, {d, pkt_bytes(0xe4, 0xa7), 0xe4}, 0);
                 }),
                 "LineDevice::Send", "0xe4 bytes: both alike");
    }

    // ---- LinePacketizer::Recv: a header read short ---------------------------------------------------------------------
    {
        int st[2], len[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            const uint32_t pk = fixed_bytes(0x14, 0);
            U(pk) = fake_device();
            *(int8_t*)(uintptr_t)(pk + 4) = 1;                                       // a header next
            g_fd_avail = 3;
            g_fd_give = 1;
            g_fd_bytes[0] = 5; g_fd_bytes[1] = 0x12; g_fd_bytes[2] = 0x34;
            const uint32_t buf = fixed_bytes(0x100, 0), n = fixed_alloc(4);
            U(n) = 0x100;
            call(A_LinePacketizer_Recv, {pk, buf, n}, 8);
            st[pass] = *(int8_t*)(uintptr_t)(pk + 4);
            len[pass] = (int)U(pk + 0xc);
        }
        dt_check(st[0] == 2 && len[0] == 5 && st[1] == 0, "LinePacketizer::Recv",
                 "a 3-byte header read as 1: the original went on to a 5-byte body with its CRC from the stack; the rewrite "
                 "goes back to sync");
        dt_check(dt_alike([] {
                     const uint32_t pk = fixed_bytes(0x14, 0);
                     U(pk) = fake_device();
                     *(int8_t*)(uintptr_t)(pk + 4) = 1;
                     g_fd_avail = 3;
                     g_fd_give = 3;
                     g_fd_bytes[0] = 5; g_fd_bytes[1] = 0x12; g_fd_bytes[2] = 0x34;
                     const uint32_t buf = fixed_bytes(0x100, 0), n = fixed_alloc(4);
                     U(n) = 0x100;
                     call(A_LinePacketizer_Recv, {pk, buf, n}, 8);
                 }),
                 "LinePacketizer::Recv", "the whole header: both alike");
    }

    // ---- LineChecker::Tick: a full read without a NUL -----------------------------------------------------------------
    {
        bool no_nul = false;
        int extra = -1, got = -1;
        uint32_t peer = 1;
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            const uint32_t lc = fixed_bytes(0x28, 0);
            U(lc) = fake_device();
            U(lc + 8) = (uint32_t)g_w.clock;                                          // start: the check has 7 s
            U(lc + 0x1c) = 5;
            g_fd_give = 0x40;
            g_fd_bytes[0] = '(';
            memset(g_fd_bytes + 1, '9', 0x3f);                                       // "(999...": no ')', no NUL
            call(0x004aea10, {lc}, 0);
            if (!pass) no_nul = g_fd_full_no_nul;
            else extra = (int)U(lc + 0xc), got = (int)U(lc + 0x10), peer = U(lc + 0x20);
        }
        dt_check(no_nul && extra == 1 && got == 0 && peer == 0 && g_fix_hits, "LineChecker::Tick",
                 "a 0x40-byte reply with no NUL: the original's strchr / strtoul ran on past the buffer; the rewrite stops at "
                 "its end (no ')': a bad reply)");
        dt_check(dt_alike([] {
                     const uint32_t lc = fixed_bytes(0x28, 0);
                     U(lc) = fake_device();
                     U(lc + 8) = (uint32_t)g_w.clock;
                     U(lc + 0x1c) = 5;
                     g_fd_give = 0x40;
                     memset(g_fd_bytes, 0, 0x40);
                     strcpy((char*)g_fd_bytes, "(77)");
                     call(0x004aea10, {lc}, 0);
                 }),
                 "LineChecker::Tick", "a reply with its NUL: both alike");
    }

    // ---- the TAPI driver's offsets and sizes -------------------------------------------------------------------------
    {
        bool fault[2];
        uint32_t ret[2];
        for (int pass = 0; pass < 2; pass++) {                                    // set_from_bytestream
            dt_begin(pass ? ISOLATED : ORIG);
            g_dt_ok = true;
            g_dt_vs_off = 0x7ff00000;
            g_dt_vs_size = 0x10;
            ret[pass] = call(A_set_from_bytestream, {1}, 8);
            fault[pass] = faulted();
            dt_knobs_off();
        }
        dt_check(fault[0] && !fault[1] && ret[1] == 1, "set_from_bytestream",
                 "the configuration read back at offset 0x7ff00000: the original compared there (a fault); the rewrite skips it");
        uint32_t handle1 = 0;
        for (int pass = 0; pass < 2; pass++) {                                    // TAPILine::get_handle
            dt_begin(pass ? ISOLATED : ORIG);
            g_dt_ok = true;
            g_dt_vs_off = 0x7ff00000;
            const uint32_t t = fixed_bytes(0x40, 0);
            U(t) = VT_TAPILine;
            U(t + 8) = 0xffffffffu;
            U(t + 0xc) = 0x9100;
            ret[pass] = call(A_TAPILine_get_handle, {t}, 8);
            fault[pass] = faulted();
            if (pass) handle1 = U(t + 8);
            dt_knobs_off();
        }
        dt_check(fault[0] && !fault[1] && ret[1] == 0 && handle1 == 0xffffffffu, "TAPILine::get_handle",
                 "the handle at offset 0x7ff00000: the original read it there (a fault); the rewrite has no handle");
        bool closed[2];
        for (int pass = 0; pass < 2; pass++) {                                    // get_tapiline_port
            dt_begin(pass ? ISOLATED : ORIG);
            g_dt_ok = true;
            const uint32_t caps = fixed_bytes(0x400, 0);
            U(caps + 0xe4) = 0x40;
            U(caps + 0xe8) = 0x7ff00000;
            ret[pass] = call(A_get_tapiline_port, {caps});
            fault[pass] = faulted();
            g_fault = false;
            U(caps + 0xe8) = 0x280;                                                // and a good one: its key closed
            U(caps + 0x280) = 1;
            U(caps + 0x284) = 8;
            strcpy((char*)(uintptr_t)(caps + 0x288), "System\\Modem\\0000");
            call(A_get_tapiline_port, {caps});
            closed[pass] = has_line("RegCloseKey(modem key)");
            dt_knobs_off();
        }
        dt_check(fault[0] && !fault[1] && ret[1] == 0 && !closed[0] && closed[1], "get_tapiline_port",
                 "the device-specific part at 0x7ff00000: the original read it there (a fault); the rewrite gives 0. A good "
                 "one: the original left its key open, the rewrite closes it");
    }

    // ---- lds_str / LineStatusText outside 0..11 ------------------------------------------------------------------------
    {
        uint32_t r[2][2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            r[pass][0] = call(A_lds_str, {12});
            r[pass][1] = call(0x004a1bc0, {0xffffffffu});
        }
        dt_check(r[0][0] != 0x004fb0c0 && r[1][0] == 0x004fb0c0 && r[0][1] != 0x004fb438 && r[1][1] == 0x004fb438,
                 "lds_str / LineStatusText", "12 and -1: the originals read past their tables; the rewrites give entry 0");
        dt_check(dt_alike([] {
                     call(A_lds_str, {11});
                     call(0x004a1bc0, {11});
                     call(0x004a1bc0, {0});
                 }),
                 "lds_str / LineStatusText", "11 and 0: both alike");
    }

    // ---- enum_devices: a flag past its two lists ----------------------------------------------------------------------
    {
        bool fault[2];
        uint32_t ret1 = 1;
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            const uint32_t r = call(A_enum_devices, {fixed_bytes(0x50 * 8, 0), 8, 8});
            fault[pass] = faulted();
            if (pass) ret1 = r;
        }
        dt_check(fault[0] && !fault[1] && ret1 == 0, "enum_devices",
                 "flags 8: the original called its own argument as a function (a fault); the rewrite lists nothing");
        dt_check(dt_alike([] {
                     for (int i = 0; i < 4; i++) g_w.port[i].exists = true;
                     call(A_enum_devices, {fixed_bytes(0x50 * 8, 0), 8, 3});
                 }),
                 "enum_devices", "flags 3: both alike");
    }

    // ---- COM4's "a modem's port" flag ---------------------------------------------------------------------------------
    {
        uint32_t count[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            for (int i = 0; i < 4; i++) g_w.port[i].exists = true;
            *(uint8_t*)(uintptr_t)(S_PORT_TAPI + 4) = 1;                            // a modem on COM4, listed before
            const uint32_t info = fixed_bytes(0x50 * 8, 0), n = fixed_alloc(4);
            U(n) = 0;
            call(A_enumerate_tapi_devices, {info, 8, n}, 0);                        // (no TAPI devices: only the flags cleared)
            call(A_enumerate_comport_devices, {info, 8, n}, 0);
            count[pass] = U(n);
        }
        dt_check(count[0] == 3 && count[1] == 4, "enumerate_tapi_devices",
                 "COM4's flag left from an earlier listing: the original never offered COM4 again; the rewrite clears it");
    }

    // ---- LineEnd: a write still pending -------------------------------------------------------------------------------
    {
        bool closed[2];
        for (int pass = 0; pass < 2; pass++) {
            dt_begin(pass ? ISOLATED : ORIG);
            for (int i = 0; i < 16; i++) {
                U(S_BLOCKS + 0xfcu * (uint32_t)i) = 0xffffffffu;
                U(S_BLOCKS + 0xfcu * (uint32_t)i + 0xf8) = 0x6000u + (uint32_t)i;
            }
            U(S_BLOCKS) = 0x5000;                                                    // block 0 still writing to the port
            g_dt_wait = 0x102 + 1;
            call(0x004a1600, {}, 0);                                                 // LineEnd
            closed[pass] = has_line("CloseHandle 5000");
            dt_knobs_off();
        }
        dt_check(closed[0] && !closed[1], "LineEnd",
                 "a block still writing: the original closed the port handle it names (the device's); the rewrite doesn't");
        dt_check(dt_alike([] {
                     for (int i = 0; i < 16; i++) {
                         U(S_BLOCKS + 0xfcu * (uint32_t)i) = 0xffffffffu;
                         U(S_BLOCKS + 0xfcu * (uint32_t)i + 0xf8) = 0x6000u + (uint32_t)i;
                     }
                     call(0x004a1600, {}, 0);
                 }),
                 "LineEnd", "no block writing: both alike");
    }
    printf("directed fix tests: %d checks, %d failed\n", g_dt_n, g_dt_bad);
    return g_dt_bad;
}
#endif

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (!getenv("VP_WNL_CHILD")) return relaunch();
    const int per_kind = argc > 1 ? atoi(argv[1]) : 300;
    const uint32_t seed0 = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 1;
    char path[MAX_PATH];
    GetModuleFileNameA(0, path, MAX_PATH);
    std::string exe = "out\\race_v10.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("run from the repository root (out\\race_v10.exe)\n");
        return 2;
    }
    if (!load_race_exe(exe.c_str())) return 2;
    if (!VirtualAlloc((void*)(uintptr_t)ARENA, ARENA_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
        printf("can't commit the arena\n");
        return 2;
    }
    install_fakes();
    g_pristine.assign((uint8_t*)(uintptr_t)DATA_AT, (uint8_t*)(uintptr_t)DATA_AT + DATA_SIZE);
    g_snap_data.resize(DATA_SIZE);
    g_snap_arena.resize(ARENA_SIZE);
    g_fp = new Footprint;
    int nreg = 0;
    for (Reg* r = Reg::head(); r; r = r->next) nreg++;
    printf("world_net_line: %d rewrites, %d scenarios of each of 6 kinds from seed %u\n", nreg, per_kind, seed0);
    static const char* const kinds[] = {"small functions", "UDP / IPX sockets", "COM ports + packets (noise)",
                                        "COM ports + line check", "TAPI modems", "threads, send blocks, winsock, TAPI parts"};
    int fails = 0;
    long long calls = 0, faults = 0;
    std::vector<int> touched(0x10000);
    for (int kind = 0; kind < 6; kind++) {
        int kf = 0;
        long long kc = 0;
        for (int i = 0; i < per_kind; i++) {
            const uint32_t seed = seed0 + (uint32_t)i * 7919u + (uint32_t)kind * 1000003u;
            PassResult o, iso, ch;
            g_check_fp = i < 12;
            run_pass(ORIG, kind, seed, o);
            const bool of = g_fault;
            run_pass(ISOLATED, kind, seed, iso);
            run_pass(CHAIN, kind, seed, ch);
            for (const std::string& s : o.log)
                if (s.rfind("= ", 0) == 0) kc++;
            if (of) {
                faults++;
                for (const std::string& l : o.log)
                    if (l.rfind("FAULT", 0) == 0)
                        printf("  (kind %d seed %u: %s, %s)\n", kind, seed, l.c_str(),
                               NET_FIXES ? "in the originals' pass" : "in every pass");
            }
            for (const std::string& l : o.log)
                for (int e = 0; e < N_EVENTS; e++)
                    if (l.find(k_events[e].text) != std::string::npos) g_event_count[e]++;
            if (getenv("WNL_DUMP") && atoi(getenv("WNL_DUMP")) == (int)seed)
                for (const std::string& l : o.log) printf("    | %s\n", l.c_str());
            int f = NET_FIXES ? compare_fixed("isolated", o, iso, kind, seed) : compare("isolated", o, iso, kind, seed);
            f |= NET_FIXES ? compare_fixed("chain", o, ch, kind, seed) : compare("chain", o, ch, kind, seed);
            if (f) {
                kf++;
                if (kf > 3) break;
            }
        }
        printf("  %-45s %5d scenarios, %7lld calls: %s\n", kinds[kind], per_kind, kc, kf ? "DIFFERENCES" : "identical");
        fails += kf;
        calls += kc;
    }
    // which rewrites the scenarios reached: called by them, or only from other functions (the chain pass)
    int top = 0, nested = 0;
    std::string none;
    for (Reg* r = Reg::head(); r; r = r->next) {
        if (r->hits_top) top++;
        else if (r->hits_chain) nested++;
        else none += std::string(none.empty() ? "" : ", ") + r->name;
    }
    printf("coverage: %d rewrites called by the scenarios, %d more reached from other functions, %d never: %s\n", top, nested,
           nreg - top - nested, none.empty() ? "-" : none.c_str());
    printf("paths reached (originals' pass):");
    for (int e = 0; e < N_EVENTS; e++) printf("%s %s %ld", e ? ";" : "", k_events[e].what, g_event_count[e]);
    printf("\n");
    printf("footprints: %d checked calls, %d writes outside a footprint\n", g_fp_checked, g_fp_violations);
    for (const std::string& m : g_fp_msgs) printf("  %s\n", m.c_str());
    int fix_bad = 0;
#if NET_FIXES
    printf("fix build: rewrite passes a fix changed (alike up to it, clean after):");
    for (size_t i = 0; i < g_fixc.size(); i++) printf("%s %s %d", i ? ";" : "", g_fixc[i].what.c_str(), g_fixc[i].n);
    printf("%s\n", g_fixc.empty() ? " none" : "");
    fix_bad = directed_fix_tests();
#endif
    printf("%lld calls, %lld scenarios ended by a fault (%s); %s\n", calls, faults,
           NET_FIXES ? "the originals' pass" : "in every pass alike",
           fails || g_fp_violations || fix_bad ? "FAILED" : NET_FIXES ? "ALL IDENTICAL OR FIXED" : "ALL IDENTICAL");
    return fails || g_fp_violations || fix_bad ? 1 : 0;
}
