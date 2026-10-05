// world_net_session.cpp -- the session recorder's network (hook/session.cpp + hook/net_wsock.cpp, multiplayer stage N0)
// on a fake network, outside the game.
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /DSESSION_TEST /Ihook test\world_net_session.cpp hook\net_wsock.cpp
//        /Fo%TEMP%\wns\ /Fe%TEMP%\wns\world_net_session.exe
//   run:   world_net_session.exe            every check below, each run as a child process in a fresh folder in %TEMP%
//
// A small "game" runs the threads the real one does, on session.cpp and the wrappers net_wsock.cpp puts in the import
// slots (net_test_hook hands them over, with a fake winsock behind them):
//   * the main thread: frames (Win32Idle -> session_idle_begin / ops / end, or a replay's session_play_idle; Flip ->
//     session_frame), the network set up and torn down (WSAStartup, socket, bind, setsockopt, ioctlsocket, getsockopt,
//     gethostname, gethostbyname, inet_addr, WSAAsyncGetHostByName -- its reply arrives through a Win32Idle as the
//     window message does -- closesocket, WSACleanup), chat sends, Grab / Release as the menus call them, a race
//     (PhysicsStart, PhysicsGetStatePacket every frame, PhysicsStop), and each frame "drawn" from what the lobby task
//     and the physics thread have read (session_gfx), so a frame differs if their reads land elsewhere;
//   * the lobby task, as LiveMultiInfo::thread: TaskSleep(250), suspend if asked, tick (recvfrom until WSAEWOULDBLOCK,
//     PTimeNow, now and then Random, a keepalive sendto), with a task table, TaskSuspend / TaskSuspendMe / TaskResume
//     and Grab / Release as the originals do them (Grab polling for up to 3 s with Win32Idle);
//   * the physics thread: every 16 ms, in a race, the lockstep gate (session_update_gate) then MultiBGRecv / Send's
//     work (Grab if not the owner, recvfrom, PTimeNow, sendto);
//   * a peer thread (recording only) sending packets at random moments, so what each thread reads depends on timing.
// Checks:
//   net      record, then replay: IDENTICAL frames (menus and the race, in lockstep), every send compared and equal,
//            every channel fed whole, the lobby's steps let run, and NO fake-winsock call in the replay; each thread's
//            digest of everything it read the same in both runs;
//   stall    the same with a 6 s main-thread stall while the lobby waits: its lockstep dropped and re-taken at the
//            next Grab, in both runs at the same point, the frames between not compared, everything else identical;
//   diverge  replays of `net` whose main thread, then lobby task, sends one different byte: the send named, frames part;
//   copy2    copy 2 records to sessions-2\ and binds 2002; copy 1 (two_copies) sends its broadcasts to 2002 too; a
//            single copy (copy 1) replays copy 2's recording by its bare name, identically;
//   (each also sends packets with stack garbage as the game's builders leave it -- byte 1 of a broadcast service
//   request, bit 7 of a car packet's last byte, most of a "back to car choice" 0x30, an AI car's 0x33 setup's four
//   unwritten dwords -- different in every run, which
//   must not count; and the physics reads the clock straight, as TimerConditioner does, its "since the base" value
//   shown in the frames and gating its sends, which the replay must feed)
//   solorace a single-player race in a session: no network or physics-clock record, replayed identically;
//   scan     a single-player session that lists folders (FindFirstFileA / FindNextFileA / FindClose through the
//            session's slot wrappers, on the real file system): a car folder, the user directory before and after the
//            game writes a file there (an export), two scans stepped interleaved, a pattern that matches nothing, and the
//            FIX layer's own scan inside get_user_directory. The car folder is changed (a file added, one removed) before
//            the replay: the replay lists what the recording listed, IDENTICAL, without one real scan (the FIX layer's
//            stays live, unrecorded);
//   scanpart a replay of `scan` whose game lists another pattern at frame 50: it parts there, naming both patterns, and
//            every listing still ends;
//   scanold  the same recorded as the recorder before scans were (no scan records): its replay scans live, identically;
//   solo     a single-player session (no winsock call): the stream has no network or scan record; with /DSESSION_HEAD (the
//            committed session.cpp, see below) the same run writes the same bytes -- `solo` prints the files' hashes.
//   For the byte-for-byte check against the committed recorder: git show HEAD:hook/session.cpp > %TEMP%\wnh\session_head.cpp,
//   build this file with /DSESSION_HEAD /I%TEMP%\wnh (and without net_wsock.cpp) to world_net_head.exe, and run
//   `world_net_session.exe solo-compare world_net_head.exe`.
#define _CRT_SECURE_NO_WARNINGS
#ifndef SESSION_TEST
#define SESSION_TEST
#endif
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>
#include <process.h>

// byte order (winsock's htons / htonl, without linking it)
static u_short bs16(unsigned v) { return (u_short)((v >> 8 & 0xff) | (v & 0xff) << 8); }
static unsigned long bs32(unsigned long v) { return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24; }

// ---- what session.cpp needs from the rest of the DLL ---------------------------------------------------------------------
static FILE* g_logfile;
static CRITICAL_SECTION g_log_cs;
void logf(const char* fmt, ...) {
    char b[4096];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    b[sizeof b - 1] = 0;
    EnterCriticalSection(&g_log_cs);
    if (g_logfile) fprintf(g_logfile, "%s\n", b), fflush(g_logfile);
    if (getenv("WNS_VERBOSE")) printf("    | %s\n", b);
    LeaveCriticalSection(&g_log_cs);
}
int shadow_com_phase() { return 0; }
bool build_is_v10() { return true; }
bool platform_plans_gl(const char*) { return true; }
static int g_copy = 1;
static bool g_two = false;
int vp_copy() { return g_copy; }
const char* vp_copy_suffix() { return g_copy == 2 ? "-2" : ""; }
bool vp_two_copies() { return g_two; }

#ifdef SESSION_HEAD
#include "session_head.cpp"                              // the committed recorder (the solo byte-for-byte check)
void net_install(const char*) {}
#else
#include "../hook/session.cpp"
void* net_test_hook(const char* name, void* real);
#endif

// ---- the run ------------------------------------------------------------------------------------------------------------
static std::string g_root;                                // this run's folder (its viperport.ini, sessions\, Config\)
static std::string g_mode;                               // record | play
static std::string g_scenario;                           // net | stall | solo | copy2
static std::string g_fault;                              // a replay's deliberate difference: main | lobby
static volatile LONG g_real_calls;                       // calls that reached the fake winsock
static volatile bool g_quit;
static int g_failures;
static void check(bool ok, const char* what) {
    printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

// each thread's digest of what it read (record and replay must agree)
enum { R_MAIN, R_LOBBY, R_PHYS, R_N };
static uint64_t g_digest[R_N] = {1469598103934665603ull, 1469598103934665603ull, 1469598103934665603ull};
static unsigned long g_reads[R_N];
static void saw(int role, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) g_digest[role] = (g_digest[role] ^ b[i]) * 1099511628211ull;
    g_reads[role]++;
}
template <typename T> static T saw_v(int role, T v) { saw(role, &v, sizeof v); return v; }

// ---- a fake winsock (the "real" side; a replay must never reach it) -------------------------------------------------------
static CRITICAL_SECTION g_net_cs;
struct Packet { std::vector<uint8_t> b; sockaddr_in from; };
static std::deque<Packet> g_inbox;                       // the game's one socket
static __declspec(thread) int t_fake_err;
static int g_next_socket = 100;
static unsigned long g_peer_count, g_dup_to_2002, g_bound_port;
static char* g_async_buf;
static int g_async_len;
static volatile LONG g_async_due;                        // GetTickCount when the reply is due (0: none)
static HANDLE g_async_handle = (HANDLE)0x4242;

#define REAL() InterlockedIncrement(&g_real_calls)
static int WINAPI f_WSAStartup(WORD v, LPWSADATA d) { REAL(); memset(d, 0, sizeof *d); d->wVersion = v; strcpy(d->szDescription, "fake"); return 0; }
static int WINAPI f_WSACleanup() { REAL(); return 0; }
static int WINAPI f_WSAGetLastError() { REAL(); return t_fake_err; }
static SOCKET WINAPI f_socket(int, int, int) { REAL(); return (SOCKET)(g_next_socket++); }
static int WINAPI f_closesocket(SOCKET) { REAL(); return 0; }
static int WINAPI f_bind(SOCKET, const sockaddr* a, int) { REAL(); g_bound_port = bs16(((const sockaddr_in*)a)->sin_port); return 0; }
static int WINAPI f_setsockopt(SOCKET, int, int, const char*, int) { REAL(); return 0; }
static int WINAPI f_ioctlsocket(SOCKET, long, u_long* a) { REAL(); if (a) *a = 7; return 0; }
static int WINAPI f_getsockopt(SOCKET, int, int, char* v, int* n) { REAL(); if (v && n && *n >= 4) { *(int*)v = 1234; *n = 4; } return 0; }
static int WINAPI f_recvfrom(SOCKET, char* buf, int len, int, sockaddr* from, int* fromlen) {
    REAL();
    EnterCriticalSection(&g_net_cs);
    if (g_inbox.empty()) {
        LeaveCriticalSection(&g_net_cs);
        t_fake_err = WSAEWOULDBLOCK;
        return SOCKET_ERROR;
    }
    Packet p = g_inbox.front();
    g_inbox.pop_front();
    LeaveCriticalSection(&g_net_cs);
    const int n = (int)p.b.size() < len ? (int)p.b.size() : len;
    memcpy(buf, p.b.data(), n);
    if (from && fromlen && *fromlen >= (int)sizeof p.from) { memcpy(from, &p.from, sizeof p.from); *fromlen = sizeof p.from; }
    return n;
}
static void peer_send(const char* what) {
    Packet p;
    char b[96];
    _snprintf(b, sizeof b, "%s:%lu:%u", what, g_peer_count++, (unsigned)(GetTickCount() * 2654435761u) >> 7);
    p.b.assign(b, b + strlen(b) + 1);
    memset(&p.from, 0, sizeof p.from);
    p.from.sin_family = AF_INET;
    p.from.sin_port = bs16(2002);
    p.from.sin_addr.s_addr = bs32(0x0a000009);
    EnterCriticalSection(&g_net_cs);
    g_inbox.push_back(p);
    LeaveCriticalSection(&g_net_cs);
}
static int WINAPI f_sendto(SOCKET, const char* buf, int len, int, const sockaddr* to, int) {
    REAL();
    const sockaddr_in* a = (const sockaddr_in*)to;
    if (a && bs16(a->sin_port) == 2002 && a->sin_addr.s_addr == INADDR_BROADCAST) g_dup_to_2002++;
    if (len >= 5 && !memcmp(buf, "chat:", 5)) peer_send("echo");   // the host answers a chat line (soon, not now)
    return len;
}
static int WINAPI f_gethostname(char* n, int len) { REAL(); strncpy(n, "fakehost", len); return 0; }
static hostent* WINAPI f_gethostbyname(const char*) {
    REAL();
    static char* aliases[] = {(char*)"fakehost.lan", 0};
    static unsigned long a1 = bs32(0x0a000007), a2 = bs32(0xc0a80107);
    static char* addrs[] = {(char*)&a1, (char*)&a2, 0};
    static hostent h = {(char*)"fakehost", aliases, AF_INET, 4, addrs};
    return &h;
}
static unsigned long WINAPI f_inet_addr(const char*) { REAL(); return INADDR_NONE; }
static u_short WINAPI f_htons(u_short v) { return bs16(v); }
static HANDLE WINAPI f_WSAAsyncGetHostByName(HWND, u_int, const char*, char* buf, int len) {
    REAL();
    g_async_buf = buf, g_async_len = len;
    InterlockedExchange(&g_async_due, (LONG)(GetTickCount() + 40 + rand() % 60));
    return g_async_handle;
}
static int WINAPI f_WSACancelAsyncRequest(HANDLE) { REAL(); return 0; }
// the reply as winsock writes it: the hostent and what it points to, inside the caller's buffer
static void fill_async_reply() {
    hostent* h = (hostent*)g_async_buf;
    char** al = (char**)(h + 1);
    char** ad = al + 1;
    char* q = (char*)(ad + 2);
    *(unsigned long*)q = bs32(0xc0a80142);
    ad[0] = q, ad[1] = 0;
    q += 4;
    strcpy(q, "joinedhost");
    h->h_name = q;
    al[0] = 0;
    h->h_aliases = al, h->h_addrtype = AF_INET, h->h_length = 4, h->h_addr_list = ad;
}

static unsigned __stdcall peer_thread(void*) {
    srand(GetTickCount());
    while (!g_quit) {
        Sleep(3 + rand() % 37);
        peer_send("peer");
    }
    return 0;
}

#ifndef SESSION_HEAD
// ---- the game's tasks (task.obj, as krn_core.cpp has them) -----------------------------------------------------------------
static TaskInfo g_tasks[8];
static uint8_t g_lmi_obj[0x40];                          // LiveMultiInfo: +0 its live flag, +0x30 the lobby task
static uint8_t* volatile g_lmi;
enum { OWNER_LOBBY = 1, OWNER_MAIN = 2, OWNER_PHYS = 3 };
static volatile LONG g_owner;
static long g_panics;
static void __cdecl t_TaskSuspend(int id) { TaskInfo& t = g_tasks[id - 1]; if (!(t.flags & 1)) t.suspend_req = 1; }
static void __cdecl t_TaskSuspendMe() {
    for (TaskInfo& t : g_tasks)
        if (t.name && t.thread_id == GetCurrentThreadId()) { t.flags |= 1; t.suspend_req = 0; SuspendThread(t.handle); return; }
}
static void t_TaskResume(int id) {
    TaskInfo& t = g_tasks[id - 1];
    t.flags &= 0xfe;
    const DWORD r = ResumeThread(t.handle);
    if (r != 1) { g_panics++; logf("test: TaskResume: the task was %s", r == 0 ? "not suspended" : "suspended twice"); }
}
static void __cdecl t_TaskSleep(int ms) { Sleep(ms); }
#endif

// ---- the wrappers the "game" calls (net_wsock.cpp's, through net_test_hook) -----------------------------------------------
#ifndef SESSION_HEAD
static decltype(&f_WSAStartup) w_WSAStartup;
static decltype(&f_WSACleanup) w_WSACleanup, w_WSAGetLastError;
static decltype(&f_socket) w_socket;
static decltype(&f_closesocket) w_closesocket;
static decltype(&f_bind) w_bind;
static decltype(&f_setsockopt) w_setsockopt;
static decltype(&f_ioctlsocket) w_ioctlsocket;
static decltype(&f_getsockopt) w_getsockopt;
static decltype(&f_recvfrom) w_recvfrom;
static decltype(&f_sendto) w_sendto;
static decltype(&f_gethostname) w_gethostname;
static decltype(&f_gethostbyname) w_gethostbyname;
static decltype(&f_inet_addr) w_inet_addr;
static decltype(&f_WSAAsyncGetHostByName) w_WSAAsyncGetHostByName;
static decltype(&f_WSACancelAsyncRequest) w_WSACancelAsyncRequest;
static int(__cdecl* w_PTimeNow)();
static int(__cdecl* w_Random)(int);
static void(__fastcall* w_Grab)(void*, void*);
static void(__fastcall* w_Release)(void*, void*);
static void(__cdecl* w_TaskSleep)(int);
static void(__cdecl* w_TaskSuspendMe)();
static uint8_t(__cdecl* w_async_hook)(unsigned, int, int);
#endif

// the "real" PTimeNow: the main thread's goes through session.cpp's QPC hook (as the game's import slot does)
static int __cdecl r_PTimeNow() {
    LARGE_INTEGER c, f;
    h_QPC(&c);
    h_QPF(&f);
    return (int)(c.QuadPart * 1000 / (f.QuadPart ? f.QuadPart : 1));
}
static volatile LONG g_rng = 12345;
static int __cdecl r_Random(int range) {
    LONG v = InterlockedExchangeAdd(&g_rng, 0x2545f491) * 1103515245 + 12345;
    return range > 0 ? (int)((unsigned)v % (unsigned)range) : 0;
}

static void win32_idle();
#ifndef SESSION_HEAD
static int my_owner() {
    const DWORD me = GetCurrentThreadId();
    if (me == g_main) return OWNER_MAIN;
    if (g_tasks[0].name && me == g_tasks[0].thread_id) return OWNER_LOBBY;
    return OWNER_PHYS;
}
// LiveMultiInfo::Grab / Release, as the originals (0x4a2d10 / 0x4a2e50)
static void __fastcall o_grab(void* lmi, void*) {
    const int id = *(int32_t*)((uint8_t*)lmi + 0x30);
    t_TaskSuspend(id);
#ifndef SESSION_HEAD
    const int t0 = w_PTimeNow();
    while (!(g_tasks[id - 1].flags & 1) && w_PTimeNow() - t0 < 3000) {
        Sleep(25);
        if (GetCurrentThreadId() == g_main) win32_idle();
    }
#endif
    if (!(g_tasks[id - 1].flags & 1)) { g_panics++; logf("test: Grab: the lobby task didn't suspend in 3 s"); }
    InterlockedExchange(&g_owner, my_owner());
}
static void __fastcall o_release(void* lmi, void*) {
    InterlockedExchange(&g_owner, OWNER_LOBBY);
    t_TaskResume(*(int32_t*)((uint8_t*)lmi + 0x30));
}
#endif

// ---- the game's state, as the frames show it --------------------------------------------------------------------------------
struct Shown {
    uint64_t lobby_hash, race_hash, main_hash;
    uint32_t lobby_ticks, lobby_packets, race_updates, race_packets, keys, host_addr;
    int32_t lobby_clock, race_clock;
    int32_t race_since_base;                 // the physics clock since the race's base (TimerConditioner-like)
};
static Shown g_shown;
static int32_t g_race_base;
static bool g_race_base_set;
// stack garbage: the bytes a builder never writes, different in every run (and every process)
static unsigned g_garbage = GetTickCount() * 2654435761u ^ GetCurrentProcessId() * 40503u;
static uint8_t garbage() { g_garbage = g_garbage * 1103515245u + 12345u; return (uint8_t)(g_garbage >> 16); }
static volatile bool g_race_on;
static void mixin(uint64_t* h, const void* p, size_t n) {
    for (size_t i = 0; i < n; i++) *h = (*h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
}

void platform_session_apply(uint8_t op, const uint8_t* p, uint8_t n) {
    if (op == SOP_KEY_DOWN && n >= 1) g_shown.keys = g_shown.keys * 31 + p[0];
}

// async_msg_hook, as the game's: the reply's address into the game's state
static uint8_t __cdecl o_async(unsigned msg, int wparam, int lparam) {
    if (msg - 0xf400u >= 4u) return 0;
    const hostent* h = (const hostent*)g_async_buf;
    uint32_t a = 0;
    if (h && !HIWORD(lparam) && h->h_addr_list && h->h_addr_list[0]) memcpy(&a, h->h_addr_list[0], 4);
    g_shown.host_addr = a;
    saw(R_MAIN, &a, 4);
    saw(R_MAIN, &wparam, 4);
    if (h && h->h_name) saw(R_MAIN, h->h_name, strlen(h->h_name));
    return 1;
}

#ifndef SESSION_HEAD
static SOCKET g_sock = INVALID_SOCKET;
// recvfrom until it would block (the lobby's SessionMgr::Tick, the physics' MultiBGRecv)
static void drain(int role, uint64_t* h, uint32_t* count) {
    for (;;) {
        char buf[256];
        sockaddr_in from;
        int fl = sizeof from;
        const int r = saw_v(role, w_recvfrom(g_sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl));
        if (r < 0) {
            const int e = saw_v(role, w_WSAGetLastError());
            if (e != WSAEWOULDBLOCK) logf("test: recvfrom fails: %d", e);
            return;
        }
        saw(role, buf, r);
        saw(role, &from, sizeof from);
        mixin(h, buf, r);
        (*count)++;
    }
}
static void send_str(const char* s) {
    sockaddr_in to;
    memset(&to, 0, sizeof to);
    for (int z = 0; z < 8; z++) to.sin_zero[z] = (char)garbage();   // (UDPSocket::Send never writes sin_zero)
    to.sin_family = AF_INET;
    to.sin_port = bs16(2001);
    to.sin_addr.s_addr = bs32(0x0a000009);
    const int r = w_sendto(g_sock, s, (int)strlen(s) + 1, 0, (const sockaddr*)&to, sizeof to);
    saw(my_owner() == OWNER_MAIN ? R_MAIN : my_owner() == OWNER_LOBBY ? R_LOBBY : R_PHYS, &r, 4);
}

// LiveMultiInfo::thread (0x4a22c0)
static unsigned __stdcall lobby_thread(void*) {
    TaskInfo& t = g_tasks[0];
    w_TaskSleep(250);
    if (t.suspend_req) w_TaskSuspendMe();
    for (;;) {
        if (g_lmi && g_lmi[0]) {                                     // the tick
            g_shown.lobby_ticks++;
            drain(R_LOBBY, &g_shown.lobby_hash, &g_shown.lobby_packets);
            g_shown.lobby_clock = saw_v(R_LOBBY, w_PTimeNow());
            if (g_shown.lobby_ticks % 3 == 0) {
                const int rv = saw_v(R_LOBBY, w_Random(1000));
                mixin(&g_shown.lobby_hash, &rv, 4);
            }
            char ka[64];
            _snprintf(ka, sizeof ka, "ka:%u:%llx", g_shown.lobby_ticks, (unsigned long long)g_shown.lobby_hash);
            if (g_fault == "lobby" && g_shown.lobby_ticks == 6) ka[0] = 'K';   // a replay's deliberate difference
            send_str(ka);
            if (g_shown.lobby_ticks % 4 == 1) {                        // a car info "back to car choice" (0x30, user 0)
                uint8_t ci[222];
                for (int i = 0; i < 222; i++) ci[i] = garbage();      // (only 3 and 5-8 are the game's)
                ci[0] = 0x41, ci[1] = (uint8_t)g_shown.lobby_ticks, ci[2] = 7, ci[3] = 0x30;
                memset(ci + 5, 0, 4);
                sockaddr_in to;
                memset(&to, 0, sizeof to);
                for (int z = 0; z < 8; z++) to.sin_zero[z] = (char)garbage();   // (UDPSocket::Send never writes sin_zero)
                to.sin_family = AF_INET, to.sin_port = bs16(2001), to.sin_addr.s_addr = bs32(0x0a000009);
                saw_v(R_LOBBY, w_sendto(g_sock, (const char*)ci, sizeof ci, 0, (const sockaddr*)&to, sizeof to));
            }
            if (g_shown.lobby_ticks % 4 == 3) {                        // the server's car info replies (0x33): an AI car's,
                for (int human = 0; human < 2; human++) {               // then a human's
                    uint8_t ci[222];
                    for (int i = 0; i < 222; i++) ci[i] = (uint8_t)(i * 7 + g_shown.lobby_ticks + human);
                    ci[0] = 0x21, ci[1] = (uint8_t)g_shown.lobby_ticks, ci[2] = 7, ci[3] = 0x33;
                    ci[221] = (uint8_t)human;                           // the record's human flag
                    if (!human)                                         // add_ai_cars' setup: dwords +0x14 / +0x1c / +0x24 /
                        for (int b : {81, 89, 97, 105})                 // +0x2c never written (make_default_setup)
                            for (int k = 0; k < 4; k++) ci[b + k] = garbage();
                    else if (g_fault == "human33" && g_shown.lobby_ticks == 7)
                        ci[81] ^= 0x40;                                 // a human's setup is compared: a replay's difference
                    sockaddr_in to;
                    memset(&to, 0, sizeof to);
                    for (int z = 0; z < 8; z++) to.sin_zero[z] = (char)garbage();
                    to.sin_family = AF_INET, to.sin_port = bs16(2002), to.sin_addr.s_addr = bs32(0x0a000009);
                    saw_v(R_LOBBY, w_sendto(g_sock, (const char*)ci, sizeof ci, 0, (const sockaddr*)&to, sizeof to));
                }
            }
        }
        w_TaskSleep(250);
        if (t.suspend_req) w_TaskSuspendMe();
        if (t.kill) break;
    }
    t.flags |= 2;
    return 0;
}

// the physics thread: the timer's PhysTaskUpdate in a race (replay.cpp's hook: the gate first)
static unsigned __stdcall physics_thread(void*) {
    while (!g_quit) {
        const int32_t st = *g_phys_state_ptr;
        if (st == 3 || st == 6) {
            bool granted;
            if (session_update_gate(&granted)) {
                const bool clock = session_phys_clock(true);           // (replay.cpp's PhysTaskUpdate hook)
                // the physics library's own clock reads (TimerConditioner::GetTicks' resync base, PhysicsDeviation:
                // PTimeNow straight, not through a patched multiplayer call site)
                const int32_t now = r_PTimeNow();
                if (!g_race_base_set) g_race_base = now, g_race_base_set = true;
                g_shown.race_since_base = saw_v(R_PHYS, now - g_race_base);
                g_shown.race_updates++;
                if (g_scenario != "solorace") {
                    if (g_owner != OWNER_PHYS) w_Grab(g_lmi, 0);       // MultiBGRecv: owner of the network now
                    drain(R_PHYS, &g_shown.race_hash, &g_shown.race_packets);
                    g_shown.race_clock = saw_v(R_PHYS, w_PTimeNow());
                    // RaceClient::SendAll: only when the deviation (the live clock against the base) allows
                    if ((g_shown.race_since_base / 40) % 3 != 2) {
                        // a car packet as SessionMgr::Send sends it: 0x72, the session, [car index][CarNetPacket], whose
                        // last byte's bit 7 MakeNetPacket never writes
                        uint8_t car[2 + 24];
                        car[0] = 0x72, car[1] = 1, car[2] = 0;
                        for (int i = 0; i < 22; i++) car[3 + i] = (uint8_t)(g_shown.race_hash >> (i % 8 * 8)) ^ (uint8_t)i;
                        car[25] = (uint8_t)((g_shown.race_updates & 0x7f) | (garbage() & 0x80));
                        sockaddr_in to;
                        memset(&to, 0, sizeof to);
                        for (int z = 0; z < 8; z++) to.sin_zero[z] = (char)garbage();   // (UDPSocket::Send never writes sin_zero)
                        to.sin_family = AF_INET, to.sin_port = bs16(2001), to.sin_addr.s_addr = bs32(0x0a000009);
                        saw_v(R_PHYS, w_sendto(g_sock, (const char*)car, sizeof car, 0, (const sockaddr*)&to, sizeof to));
                    }
                } else {
                    g_shown.race_since_base = 0;                       // (single player: the clock isn't the race's)
                }
                session_phys_clock_restore(clock);
                if (granted) session_update_done();
            }
        }
        Sleep(16);
    }
    return 0;
}
#endif

// ---- the main thread ---------------------------------------------------------------------------------------------------------
static int g_frame_no;
static void win32_idle() {
    if (session_playing()) {
        if (session_play_idle()) return;
    }
    session_idle_begin();
    if (g_frame_no % 7 == 3) {                                       // the player's keys (deterministic)
        uint8_t k = (uint8_t)('A' + g_frame_no % 26);
        session_op(SOP_KEY_DOWN, &k, 1);
        platform_session_apply(SOP_KEY_DOWN, &k, 1);
    }
#ifndef SESSION_HEAD
    const LONG due = g_async_due;                                     // the async reply, through the window
    if (due && session_net_live() && (LONG)(GetTickCount() - (DWORD)due) >= 0) {
        InterlockedExchange(&g_async_due, 0);
        fill_async_reply();
        w_async_hook(0xf400, (int)(uintptr_t)g_async_handle, MAKELONG(16, 0));
    }
#endif
    session_idle_end();
}

static uint32_t* g_phys_state_storage;
static uint8_t g_packet[64];
static void __cdecl o_phys_start() { *g_phys_state_ptr = 3; }
static void __cdecl o_phys_stop() { *g_phys_state_ptr = 8; }
static uint8_t* __cdecl o_get_packet(void*, void*) { return g_packet; }
static char g_user_dir[MAX_PATH];
static bool g_in_user_dir;                               // (the scan scenarios: in get_user_directory)
static uint8_t __cdecl o_user_dir() {
#ifndef SESSION_HEAD
    if (g_scenario.compare(0, 4, "scan") == 0) {          // the FIX layer's migration: scans the real Config\ (live)
        g_in_user_dir = true;
        WIN32_FIND_DATAA fd;
        HANDLE h = h_FindFirstFileA((g_root + "\\Config\\fixscan*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) h_FindClose(h);
        h = h_FindFirstFileA((g_root + "\\Config\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            while (h_FindNextFileA(h, &fd)) {}
            h_FindClose(h);
        }
        g_in_user_dir = false;
    }
#endif
    return 1;
}

#ifndef SESSION_HEAD
// ---- directory scans (the scan scenarios): the real API behind session.cpp's slot wrappers, counted ------------------------
static unsigned long g_real_scans;                       // FindFirstFileA calls that reached the file system (the game's)
static unsigned long g_fix_scans;                        // ... from inside get_user_directory (the FIX layer's: always live)
static HANDLE WINAPI r_FindFirstFileA(LPCSTR p, LPWIN32_FIND_DATAA fd) {
    if (g_in_user_dir) g_fix_scans++;
    else g_real_scans++;
    return FindFirstFileA(p, fd);
}
static BOOL WINAPI r_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd) { return FindNextFileA(h, fd); }
static BOOL WINAPI r_FindClose(HANDLE h) { return FindClose(h); }

// a folder listed as the game does (FileFindFirst / Next / Close through the slots): every name, size and attribute into
// the main thread's digest, the names onto the screen; the error that ends it too
static void list_dir(const std::string& pattern) {
    WIN32_FIND_DATAA fd;
    memset(&fd, 0xcc, sizeof fd);                         // (the game's buffer: stack garbage)
    HANDLE h = h_FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = saw_v(R_MAIN, GetLastError());
        g_shown.main_hash = g_shown.main_hash * 31 + e;
        return;
    }
    do {
        saw(R_MAIN, fd.cFileName, strlen(fd.cFileName));
        saw(R_MAIN, &fd.nFileSizeLow, 4);
        saw(R_MAIN, &fd.dwFileAttributes, 4);
        mixin(&g_shown.main_hash, fd.cFileName, strlen(fd.cFileName));
    } while (h_FindNextFileA(h, &fd));
    saw_v(R_MAIN, GetLastError());
    saw_v(R_MAIN, h_FindClose(h));
}
// two scans open at once, stepped in turn
static void list_two(const std::string& a, const std::string& b) {
    WIN32_FIND_DATAA fa, fb;
    HANDLE ha = h_FindFirstFileA(a.c_str(), &fa), hb = h_FindFirstFileA(b.c_str(), &fb);
    bool ma = ha != INVALID_HANDLE_VALUE, mb = hb != INVALID_HANDLE_VALUE;
    while (ma || mb) {
        if (ma) {
            saw(R_MAIN, fa.cFileName, strlen(fa.cFileName));
            mixin(&g_shown.main_hash, fa.cFileName, strlen(fa.cFileName));
            ma = h_FindNextFileA(ha, &fa) != 0;
        }
        if (mb) {
            saw(R_MAIN, fb.cFileName, strlen(fb.cFileName));
            mixin(&g_shown.main_hash, fb.cFileName, strlen(fb.cFileName));
            mb = h_FindNextFileA(hb, &fb) != 0;
        }
    }
    if (ha != INVALID_HANDLE_VALUE) saw_v(R_MAIN, h_FindClose(ha));
    if (hb != INVALID_HANDLE_VALUE) saw_v(R_MAIN, h_FindClose(hb));
}
static void scan_frame(int f) {
    const std::string cars = g_root + "\\cars\\";
    const std::string user = g_user_dir_ptr;             // (a replay's is its own copy of the snapshot)
    if (f == 10) { list_dir(cars + "*.car"); list_dir(user + "*.cfg"); }
    if (f == 30) {                                       // the game writes a file into the user directory (an export)
        FILE* x = fopen((user + "export.cfg").c_str(), "w");
        if (x) fputs("exported\n", x), fclose(x);
    }
    if (f == 40) list_dir(user + "*");
    if (f == 50) list_dir(cars + (g_fault == "pattern" ? "*.trk" : "*.car"));
    if (f == 60) list_two(cars + "*", user + "*.cfg");
    if (f == 70) list_dir(g_root + "\\nothing-here\\*.xyz");
    if (f == 90) list_dir(cars + "*.CAR");
}
#endif

static void draw_frame() {
    // what the screen shows: the lobby's and the race's state as read, the keys, the host found
    session_gfx(SG_DRAW, &g_shown, sizeof g_shown);
    session_frame();
}

static int run_game() {
    g_main = GetCurrentThreadId();                                    // (session_install sets it too)
    g_phys_state_ptr = (volatile int32_t*)g_phys_state_storage;
#ifndef SESSION_HEAD
    g_test_tasks = g_tasks;
    g_test_lmi = &g_lmi;
    g_test_task_suspend = t_TaskSuspend;
#endif
    o_PhysicsStart = o_phys_start;
    o_PhysicsStop = o_phys_stop;
    o_PhysicsGetStatePacket = o_get_packet;
    o_get_user_directory = o_user_dir;
    o_QPC = QueryPerformanceCounter;
    o_QPF = QueryPerformanceFrequency;
#ifndef SESSION_HEAD
    o_FindFirstFileA = r_FindFirstFileA;
    o_FindNextFileA = r_FindNextFileA;
    o_FindClose = r_FindClose;
#endif
    g_user_dir_ptr = g_user_dir;
    _snprintf(g_user_dir, sizeof g_user_dir, "%s\\Config\\", g_root.c_str());
#ifndef SESSION_HEAD
    w_WSAStartup = (decltype(w_WSAStartup))net_test_hook("WSAStartup", (void*)f_WSAStartup);
    w_WSACleanup = (decltype(w_WSACleanup))net_test_hook("WSACleanup", (void*)f_WSACleanup);
    w_WSAGetLastError = (decltype(w_WSAGetLastError))net_test_hook("WSAGetLastError", (void*)f_WSAGetLastError);
    w_socket = (decltype(w_socket))net_test_hook("socket", (void*)f_socket);
    w_closesocket = (decltype(w_closesocket))net_test_hook("closesocket", (void*)f_closesocket);
    w_bind = (decltype(w_bind))net_test_hook("bind", (void*)f_bind);
    w_setsockopt = (decltype(w_setsockopt))net_test_hook("setsockopt", (void*)f_setsockopt);
    w_ioctlsocket = (decltype(w_ioctlsocket))net_test_hook("ioctlsocket", (void*)f_ioctlsocket);
    w_getsockopt = (decltype(w_getsockopt))net_test_hook("getsockopt", (void*)f_getsockopt);
    w_recvfrom = (decltype(w_recvfrom))net_test_hook("recvfrom", (void*)f_recvfrom);
    w_sendto = (decltype(w_sendto))net_test_hook("sendto", (void*)f_sendto);
    w_gethostname = (decltype(w_gethostname))net_test_hook("gethostname", (void*)f_gethostname);
    w_gethostbyname = (decltype(w_gethostbyname))net_test_hook("gethostbyname", (void*)f_gethostbyname);
    w_inet_addr = (decltype(w_inet_addr))net_test_hook("inet_addr", (void*)f_inet_addr);
    net_test_hook("htons", (void*)f_htons);
    net_test_hook("ntohs", (void*)f_htons);
    w_WSAAsyncGetHostByName = (decltype(w_WSAAsyncGetHostByName))net_test_hook("WSAAsyncGetHostByName", (void*)f_WSAAsyncGetHostByName);
    w_WSACancelAsyncRequest = (decltype(w_WSACancelAsyncRequest))net_test_hook("WSACancelAsyncRequest", (void*)f_WSACancelAsyncRequest);
    w_PTimeNow = (int(__cdecl*)())net_test_hook("PTimeNow", (void*)r_PTimeNow);
    w_Random = (int(__cdecl*)(int))net_test_hook("Random", (void*)r_Random);
    w_Grab = (void(__fastcall*)(void*, void*))net_test_hook("Grab", (void*)o_grab);
    w_Release = (void(__fastcall*)(void*, void*))net_test_hook("Release", (void*)o_release);
    w_TaskSleep = (void(__cdecl*)(int))net_test_hook("TaskSleep", (void*)t_TaskSleep);
    w_TaskSuspendMe = (void(__cdecl*)())net_test_hook("TaskSuspendMe", (void*)t_TaskSuspendMe);
    w_async_hook = (uint8_t(__cdecl*)(unsigned, int, int))net_test_hook("async_msg_hook", (void*)o_async);
#endif
    std::string ini = g_root + "\\viperport.ini";
    session_install(ini.c_str());
    if (g_session_mode == SESSION_OFF) { printf("    FAIL the session didn't start (see %s\\test.log)\n", g_root.c_str()); return 1; }
    h_get_user_directory();                                           // (the snapshot of Config\)

    const bool net = g_scenario != "solo" && g_scenario != "solorace" && g_scenario.compare(0, 4, "scan") != 0;
    HANDLE peer = 0, lobby = 0, phys = 0;
#ifndef SESSION_HEAD
    if (net && session_net_live()) peer = (HANDLE)_beginthreadex(0, 0, peer_thread, 0, 0, 0);
    if (g_scenario != "solo") phys = (HANDLE)_beginthreadex(0, 0, physics_thread, 0, 0, 0);
#endif
    const int frames = 150;
    for (g_frame_no = 0; g_frame_no < frames; g_frame_no++) {
        win32_idle();
        const int f = g_frame_no;
        if (!net) {                                                   // a single-player run: input, randoms, frames
            int v;
            if (!session_random_feed(100, &v)) { v = r_Random(100); session_random_saw(100, v); }
            g_shown.main_hash = g_shown.main_hash * 31 + v;
#ifndef SESSION_HEAD
            if (g_scenario.compare(0, 4, "scan") == 0) scan_frame(f);
            if (g_scenario == "solorace") {                           // a single-player race (in lockstep)
                if (f == 60) { g_test_network_race = false; g_race_on = true; h_PhysicsStart(); }
                if (g_race_on) h_PhysicsGetStatePacket(0, 0);
                if (f == 100) { h_PhysicsStop(); g_race_on = false; }
            }
#endif
            draw_frame();
            if (g_mode == "record" && g_scenario == "solorace") Sleep(12);
            continue;
        }
#ifndef SESSION_HEAD
        if (f == 2) {                                                 // the network set up (UDPSocket's constructor)
            WSADATA d;
            saw_v(R_MAIN, w_WSAStartup(0x101, &d));
            saw(R_MAIN, &d.wVersion, 2);
            g_sock = saw_v(R_MAIN, w_socket(AF_INET, SOCK_DGRAM, 0));
            sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_port = bs16(2001);
            saw_v(R_MAIN, w_bind(g_sock, (sockaddr*)&a, sizeof a));
            int one = 1;
            saw_v(R_MAIN, w_setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (char*)&one, 4));
            u_long nb = 1;
            saw_v(R_MAIN, w_ioctlsocket(g_sock, FIONBIO, &nb));
            saw(R_MAIN, &nb, 4);
            int ov = 0, ol = 4;
            saw_v(R_MAIN, w_getsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (char*)&ov, &ol));
            saw(R_MAIN, &ov, 4), saw(R_MAIN, &ol, 4);
            char name[128] = "";
            saw_v(R_MAIN, w_gethostname(name, sizeof name));
            saw(R_MAIN, name, strlen(name));
            const hostent* h = w_gethostbyname(name);
            if (h) {
                saw(R_MAIN, h->h_name, strlen(h->h_name));
                for (int i = 0; h->h_addr_list[i]; i++) saw(R_MAIN, h->h_addr_list[i], h->h_length);
                for (int i = 0; h->h_aliases[i]; i++) saw(R_MAIN, h->h_aliases[i], strlen(h->h_aliases[i]));
            }
            saw_v(R_MAIN, w_inet_addr("fakehost"));
        }
        if (f == 3) {                                                 // AsyncGetHostAddr: inet_addr fails, so by name
            static char buf[1024];
            g_async_buf = buf, g_async_len = sizeof buf;
            HANDLE ah = w_WSAAsyncGetHostByName(0, 0xf400, "joinedhost", buf, sizeof buf);
            saw(R_MAIN, &ah, 4);
        }
        if (f == 4) {                                                 // LiveMultiInfo's constructor: the lobby task, suspended
            memset(g_lmi_obj, 0, sizeof g_lmi_obj);
            *(int32_t*)(g_lmi_obj + 0x30) = 1;
            TaskInfo& t = g_tasks[0];
            memset(&t, 0, sizeof t);
            t.name = "LiveMultiInfo";
            unsigned tid;
            lobby = (HANDLE)_beginthreadex(0, 0, lobby_thread, 0, CREATE_SUSPENDED, &tid);
            t.handle = lobby, t.thread_id = tid;
            g_lmi = g_lmi_obj;
            InterlockedExchange(&g_owner, OWNER_LOBBY);
            ResumeThread(lobby);
            t_TaskSuspend(1);
        }
        if (f == 6) { w_Grab(g_lmi, 0); g_lmi[0] = 1; w_Release(g_lmi, 0); }   // MultiMaster: grab, live, release
        if (f == 8) {                                                 // ServiceRequestBroadcast: 255.255.255.255:2001
            sockaddr_in to;
            memset(&to, 0, sizeof to);
            for (int z = 0; z < 8; z++) to.sin_zero[z] = (char)garbage();   // (UDPSocket::Send never writes sin_zero)
            to.sin_family = AF_INET;
            to.sin_port = bs16(2001);
            to.sin_addr.s_addr = INADDR_BROADCAST;
            const uint8_t req[8] = {0x70, garbage(), 0x1e, 5, 1, 0, 0, 0};   // byte 1: never written (stack garbage)
            saw_v(R_MAIN, w_sendto(g_sock, (const char*)req, 8, 0, (const sockaddr*)&to, sizeof to));
            // broadcast_servinfo: a ServiceInfo (control type 6) -- its byte 1, the service name's tail after its NUL
            // and bytes 49-51 (the LocalService's padding after its password flag) are stack garbage
            uint8_t info[68];
            for (int i = 0; i < 68; i++) info[i] = garbage();
            info[0] = 0x70, info[2] = 0x1e, info[3] = 6;
            memcpy(info + 4, "testing", 8);
            info[36] = 0x01, info[37] = 0x10, info[38] = 0, info[39] = 0;           // the type 0x1001
            memset(info + 40, 0, 8);
            info[48] = 0;                                                           // no password
            memset(info + 52, 0, 16);
            saw_v(R_MAIN, w_sendto(g_sock, (const char*)info, 68, 0, (const sockaddr*)&to, sizeof to));
        }
        if (f >= 10 && f < 120 && f % 9 == 0 && !g_race_on) {        // chat
            char line[48];
            _snprintf(line, sizeof line, "chat:%d:%u", f, g_shown.keys);
            if (g_fault == "main" && f == 27) line[1] = 'H';
            send_str(line);
        }
        if (f == 30 || f == 44) { w_Grab(g_lmi, 0); w_Release(g_lmi, 0); }   // a menu's Grab / Release
        if (f == 36 && g_scenario == "stall") {                       // a 6 s main-thread stall: the lobby gives up waiting
            Sleep(6000);
        }
        if (f == 60) {                                                // MultiSynchronize, then the race
            w_Grab(g_lmi, 0);
            g_test_network_race = true;
            g_race_on = true;
            h_PhysicsStart();
        }
        if (g_race_on) h_PhysicsGetStatePacket(0, 0);                 // the world reads the physics' packet
        if (f == 100) {                                               // the race ends; the lobby takes over again
            h_PhysicsStop();
            g_race_on = false;
            InterlockedExchange(&g_owner, OWNER_MAIN);
            w_Release(g_lmi, 0);
        }
        if (f == 140) {                                               // ~LiveMultiInfo: TaskDestroy, then the socket closed
            g_lmi[0] = 0;
            g_tasks[0].kill = 1;
            if (g_tasks[0].flags & 1) t_TaskResume(1);
            WaitForSingleObject(lobby, INFINITE);
            saw_v(R_MAIN, w_closesocket(g_sock));
            saw_v(R_MAIN, w_WSACleanup());
        }
#endif
        g_shown.main_hash = g_shown.main_hash * 31 + (uint64_t)f;
        draw_frame();
        if (g_mode == "record") Sleep(12);                            // (about the game's frame rate; a replay runs flat out)
    }
    g_quit = true;
    if (peer) WaitForSingleObject(peer, 2000);
    if (phys) WaitForSingleObject(phys, 2000);
    session_report();
    // the run's own numbers, for the parent
    std::string out = g_root + "\\result-" + g_mode + ".txt";
    FILE* r = fopen(out.c_str(), "w");
    if (r) {
        fprintf(r, "digests %016llx %016llx %016llx\n", (unsigned long long)g_digest[0], (unsigned long long)g_digest[1],
                (unsigned long long)g_digest[2]);
        fprintf(r, "reads %lu %lu %lu\n", g_reads[0], g_reads[1], g_reads[2]);
        fprintf(r, "real_calls %ld\n", g_real_calls);
#ifndef SESSION_HEAD
        fprintf(r, "panics %ld\n", g_panics);
#endif
        fprintf(r, "compared %u\ndiffer %u\nrace_compared %u\nfirst_part %d\n", g_compared, g_differ, g_race_compared,
                (int)g_first_part);
#ifndef SESSION_HEAD
        unsigned long fed = 0, total = 0;
        for (int c = 0; c < CH_N; c++) fed += g_ch_fed[c], total += (unsigned long)g_ch[c].size();
        fprintf(r, "channel_fed %lu\nchannel_total %lu\nsends %lu\nsends_differ %lu\nsends_hashed %lu\n", fed, total, g_sends,
                g_sends_differ, g_sends_hashed);
        fprintf(r, "lobby_steps %lu\nlobby_implied %lu\nlobby_drops %lu\nlobby_free_frames %lu\n", g_lb_steps, g_lb_implied,
                g_lb_drops, g_lb_free_frames);
        fprintf(r, "lobby_ticks %u\nrace_updates %u\nbound_port %lu\ndup_to_2002 %lu\nhost_addr %08x\n", g_shown.lobby_ticks,
                g_shown.race_updates, g_bound_port, g_dup_to_2002, g_shown.host_addr);
        fprintf(r, "parted_first_what %s\n", g_first_part == UINT32_MAX ? "-" : g_first_part_what);
        fprintf(r, "real_scans %lu\nfix_scans %lu\nscans %lu\nscan_entries %lu\nscans_live %lu\nscan_ends %lu\nscans_open %u\n",
                g_real_scans, g_fix_scans, g_scans, g_scan_entries, g_scans_live, g_scan_ends, (unsigned)g_scan_handles.size());
#endif
        fclose(r);
    }
    return 0;
}

// ---- the parent: each check is a record and replays, as child processes --------------------------------------------------------
static std::string g_exe;
static std::string temp_dir(const char* tag) {
    char t[MAX_PATH];
    GetTempPathA(sizeof t, t);
    char d[MAX_PATH];
    _snprintf(d, sizeof d, "%swns-%s-%lu-%lu", t, tag, GetCurrentProcessId(), GetTickCount());
    CreateDirectoryA(d, 0);
    return d;
}
static void write_ini(const std::string& dir, const char* body) {
    FILE* f = fopen((dir + "\\viperport.ini").c_str(), "w");
    fputs(body, f);
    fclose(f);
}
static int child(const std::string& exe, const std::string& dir, const char* mode, const char* scenario, const char* extra) {
    char cmd[2048];
    _snprintf(cmd, sizeof cmd, "\"%s\" child \"%s\" %s %s %s", exe.c_str(), dir.c_str(), mode, scenario, extra ? extra : "-");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    return (int)code;
}
static std::string read_file(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static std::string field(const std::string& s, const char* key) {
    std::string k = std::string(key) + " ";
    size_t at = s.find("\n" + k);
    if (at == std::string::npos) { if (s.compare(0, k.size(), k) == 0) at = 0; else return ""; } else at++;
    size_t e = s.find('\n', at);
    std::string v = s.substr(at + k.size(), e == std::string::npos ? std::string::npos : e - at - k.size());
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}
static long num(const std::string& s, const char* key) { return strtol(field(s, key).c_str(), 0, 10); }
// the one recording in the run folder's sessions folder (suffix: "" or "-2")
static std::string session_name(const std::string& dir, const char* suffix) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\sessions" + suffix + "\\*").c_str(), &fd);
    std::string n;
    if (h == INVALID_HANDLE_VALUE) return n;
    do
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') n = fd.cFileName;
    while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
static std::string make_run(const char* tag) {
    std::string d = temp_dir(tag);
    CreateDirectoryA((d + "\\Config").c_str(), 0);
    FILE* f = fopen((d + "\\Config\\options.cfg").c_str(), "w");
    fputs("fake options\n", f);
    fclose(f);
    return d;
}

static void check_identical(const std::string& rec, const std::string& rep, bool lobby_drop) {
    check(num(rep, "real_calls") == 0, "the replay made no winsock call");
    check(field(rec, "digests") == field(rep, "digests"), ("every thread read the same values (" + field(rep, "reads") + " reads)").c_str());
    check(num(rep, "first_part") == -1 && num(rep, "differ") == 0,
          ("IDENTICAL frames: " + field(rep, "compared") + " compared, " + field(rep, "race_compared") + " of them in the race").c_str());
    check(num(rep, "race_compared") > 30, "the network race was in lockstep (its frames compared)");
    check(num(rep, "sends") > 20 && num(rep, "sends_differ") == 0, ("every send compared and equal (" + field(rep, "sends") + ")").c_str());
    check(num(rep, "channel_fed") == num(rep, "channel_total"), ("every network record fed (" + field(rep, "channel_total") + ")").c_str());
    check(num(rep, "lobby_steps") >= 3 &&num(rep, "lobby_steps") == num(rec, "lobby_steps"),
          ("the lobby's steps let run at the same Win32Idle calls (" + field(rep, "lobby_steps") + ")").c_str());
    check(num(rep, "lobby_implied") == num(rec, "lobby_implied"), "the same Grab / Release steps");
    check(num(rec, "panics") == 0 && num(rep, "panics") == 0, "no task panics (Grab never timed out, TaskResume found it suspended)");
    check(num(rec, "race_updates") > 20, ("the physics ran in the race (" + field(rec, "race_updates") + " updates)").c_str());
    check(field(rec, "host_addr") == "4201a8c0" && field(rep, "host_addr") == "4201a8c0", "the async host lookup's reply fed through Win32Idle");
    if (lobby_drop) {
        check(num(rec, "lobby_drops") == 1 && num(rep, "lobby_drops") == 1, "the lobby's lockstep dropped once in both runs");
        check(num(rep, "lobby_free_frames") > 0, ("its frames not compared until the next Grab (" + field(rep, "lobby_free_frames") + ")").c_str());
    } else {
        check(num(rec, "lobby_drops") == 0, "the lobby's lockstep held");
    }
}

static int parent() {
    printf("world_net_session: the session recorder's network, on a fake network\n");
    // net: record and replay
    printf("  net (record, replay)\n");
    std::string d = make_run("net");
    write_ini(d, "[session]\nrecord=1\n");
    if (child(g_exe, d, "record", "net", 0)) { check(false, "the recording ran"); return 1; }
    const std::string name = session_name(d, "");
    std::string body = "[session]\nplay=" + name + "\nlabel=replay\n";
    write_ini(d, body.c_str());
    if (child(g_exe, d, "play", "net", 0)) { check(false, "the replay ran"); return 1; }
    const std::string rec = read_file(d + "\\result-record.txt"), rep = read_file(d + "\\result-play.txt");
    check_identical(rec, rep, false);
    const std::string stream = read_file(d + "\\sessions\\" + name + "\\session.vps");
    check(stream.find('C') != std::string::npos && stream.find('Y') != std::string::npos, "the stream has network and lobby records");

    printf("  diverge (replays of that recording with one byte sent differently)\n");
    write_ini(d, (std::string("[session]\nplay=") + name + "\nlabel=fault-main\n").c_str());
    child(g_exe, d, "play", "net", "main");
    std::string f1 = read_file(d + "\\result-play.txt");
    check(num(f1, "sends_differ") >= 1 && num(f1, "first_part") == 27, ("the main thread's: named at frame " + field(f1, "first_part") + " -- " + field(f1, "parted_first_what")).c_str());
    std::string log = read_file(d + "\\test.log");
    check(log.find("on the main thread differs from the recording's") != std::string::npos, "the log names the send and its thread");
    check(log.find("(other bytes) at byte 1 (48, recorded 68)") != std::string::npos, "and the byte that differs, with both values");
    write_ini(d, (std::string("[session]\nplay=") + name + "\nlabel=fault-lobby\n").c_str());
    child(g_exe, d, "play", "net", "lobby");
    std::string f2 = read_file(d + "\\result-play.txt");
    log = read_file(d + "\\test.log");
    check(num(f2, "sends_differ") >= 1 && num(f2, "first_part") >= 0 &&
              log.find("on the lobby task differs from the recording's") != std::string::npos,
          ("the lobby task's: named, frames part at " + field(f2, "first_part")).c_str());
    write_ini(d, (std::string("[session]\nplay=") + name + "\nlabel=fault-human33\n").c_str());
    child(g_exe, d, "play", "net", "human33");
    std::string f3 = read_file(d + "\\result-play.txt");
    log = read_file(d + "\\test.log");
    check(num(f3, "sends_differ") >= 1 && log.find("at byte 81 (") != std::string::npos,
          "a human car's 0x33 setup byte (81) still compared, where an AI car's four setup dwords aren't");

    printf("  oldrec (recorded as the 22:22 DLL did, sin_zero garbage in the destination and the hash, packets unmasked; replayed now)\n");
    {
        std::string od = make_run("oldrec");
        write_ini(od, "[session]\nrecord=1\n");
        SetEnvironmentVariableA("WNS_OLD_SINZERO", "1");
        SetEnvironmentVariableA("WNS_OLD_NOMASK", "1");      // (and before any packet mask existed)
        child(g_exe, od, "record", "net", 0);
        SetEnvironmentVariableA("WNS_OLD_SINZERO", 0);
        SetEnvironmentVariableA("WNS_OLD_NOMASK", 0);
        write_ini(od, (std::string("[session]\nplay=") + session_name(od, "") + "\nlabel=replay\n").c_str());
        child(g_exe, od, "play", "net", 0);
        check_identical(read_file(od + "\\result-record.txt"), read_file(od + "\\result-play.txt"), false);
    }

    printf("  stall (a 6 s main-thread stall: the lobby's lockstep dropped and taken back)\n");
    d = make_run("stall");
    write_ini(d, "[session]\nrecord=1\n");
    child(g_exe, d, "record", "stall", 0);
    const std::string sname = session_name(d, "");
    write_ini(d, (std::string("[session]\nplay=") + sname + "\nlabel=replay\n").c_str());
    child(g_exe, d, "play", "stall", 0);
    check_identical(read_file(d + "\\result-record.txt"), read_file(d + "\\result-play.txt"), true);

    printf("  copy2 (copy 2 records to sessions-2\\ and binds 2002; copy 1 replays it alone)\n");
    d = make_run("copy2");
    write_ini(d, "[test]\ntwo_copies=1\n[session]\nrecord=1\n");
    child(g_exe, d, "record", "net", "copy2");
    const std::string cname = session_name(d, "-2");
    check(!cname.empty() && session_name(d, "").empty(), "copy 2's recording is in sessions-2\\");
    const std::string crec = read_file(d + "\\result-record.txt");
    check(num(crec, "bound_port") == 2002, "copy 2's socket bound to port 2002");
    write_ini(d, (std::string("[test]\ntwo_copies=1\n[session]\nplay=") + cname + "\nlabel=replay\n").c_str());
    child(g_exe, d, "play", "net", "copy1");
    const std::string crep = read_file(d + "\\result-play.txt");
    check(read_file(d + "\\test.log").find("replaying that one") != std::string::npos, "copy 1 found copy 2's recording by its bare name");
    check_identical(crec, crep, false);
    d = make_run("copy1");
    write_ini(d, "[test]\ntwo_copies=1\n[session]\nrecord=1\n");
    child(g_exe, d, "record", "net", "copy1-bcast");
    check(num(read_file(d + "\\result-record.txt"), "dup_to_2002") >= 1, "copy 1's broadcasts to 2001 go to 2002 too");
    check(num(read_file(d + "\\result-record.txt"), "bound_port") == 2001, "copy 1 binds 2001");

    printf("  scan (folder listings recorded; the replay lists them after the folder changed, without scanning)\n");
    {
        std::string sd = make_run("scan");
        CreateDirectoryA((sd + "\\cars").c_str(), 0);
        const char* const cars[] = {"alpha.car", "bravo.car", "charlie.car", "readme.txt"};
        for (const char* c : cars) {
            FILE* x = fopen((sd + "\\cars\\" + c).c_str(), "w");
            fputs(c, x);
            fclose(x);
        }
        write_ini(sd, "[session]\nrecord=1\n");
        if (child(g_exe, sd, "record", "scan", 0)) check(false, "the recording ran");
        const std::string sn = session_name(sd, "");
        const std::string srec = read_file(sd + "\\result-record.txt");
        const std::string sst = read_file(sd + "\\sessions\\" + sn + "\\session.vps");
        unsigned nd = 0, ns = 0, no = 0;
        for (size_t at = 16; at + 5 <= sst.size();) {
            const char k = sst[at];
            uint32_t len;
            memcpy(&len, &sst[at + 1], 4);
            nd += k == 'D', ns += k == 'S', no += k == 'O';
            at += 5 + len;
        }
        check(nd == 8 && no == 7 && ns > 10, ("the stream has the scans: " + std::to_string(nd) + " first, " + std::to_string(ns) +
                                                 " next, " + std::to_string(no) + " close records").c_str());
        check(sst.find("fixscan") == std::string::npos && num(srec, "fix_scans") == 2,
              "the FIX layer's scans inside get_user_directory went live, unrecorded");
        check(sst.find("<user>\\*.cfg") != std::string::npos, "a user-directory pattern is recorded as <user>\\");
        check(num(srec, "scans_open") == 0, "every recorded scan was closed");
        // the folder changes before the replay: a car added, one removed
        DeleteFileA((sd + "\\cars\\bravo.car").c_str());
        FILE* x = fopen((sd + "\\cars\\zulu.car").c_str(), "w");
        fputs("new", x);
        fclose(x);
        write_ini(sd, (std::string("[session]\nplay=") + sn + "\nlabel=replay\n").c_str());
        if (child(g_exe, sd, "play", "scan", 0)) check(false, "the replay ran");
        const std::string srep = read_file(sd + "\\result-play.txt");
        check(field(srec, "digests") == field(srep, "digests"),
              ("the replay's game read the recording's listings (" + field(srep, "reads") + " reads)").c_str());
        check(num(srep, "first_part") == -1 && num(srep, "differ") == 0 && num(srep, "compared") > 100,
              ("IDENTICAL frames: " + field(srep, "compared") + " compared").c_str());
        check(num(srep, "real_scans") == 0 && num(srec, "real_scans") == 8,
              ("no real scan in the replay (the recording made " + field(srec, "real_scans") + ")").c_str());
        check(num(srep, "fix_scans") == 2, "the FIX layer's scans stayed live in the replay");
        check(num(srep, "scans") == 8 && num(srep, "scan_entries") == num(srec, "scan_entries") && num(srep, "scans_live") == 0 &&
                  num(srep, "scan_ends") == 0 && num(srep, "scans_open") == 0,
              ("every scan fed whole (" + field(srep, "scans") + " scans, " + field(srep, "scan_entries") +
               " entries), every handle closed").c_str());

        printf("  scanpart (a replay of it whose game lists another pattern at frame 50)\n");
        write_ini(sd, (std::string("[session]\nplay=") + sn + "\nlabel=fault-pattern\n").c_str());
        child(g_exe, sd, "play", "scan", "pattern");
        const std::string fp = read_file(sd + "\\result-play.txt");
        check(num(fp, "first_part") == 50 && field(fp, "parted_first_what").find("*.trk") != std::string::npos &&
                  field(fp, "parted_first_what").find("*.car") != std::string::npos,
              ("parted at frame " + field(fp, "first_part") + ": " + field(fp, "parted_first_what")).c_str());
        check(num(fp, "real_scans") <= 1 && num(fp, "scans_open") == 0,
              ("every listing ended, every handle closed; real scans: " + field(fp, "real_scans") + " (the unmatched one, live)").c_str());

        printf("  scanold (recorded as the recorder before scans: no scan records; replayed live)\n");
        std::string od = make_run("scanold");
        CreateDirectoryA((od + "\\cars").c_str(), 0);
        for (const char* c : cars) {
            FILE* y = fopen((od + "\\cars\\" + c).c_str(), "w");
            fputs(c, y);
            fclose(y);
        }
        write_ini(od, "[session]\nrecord=1\n");
        SetEnvironmentVariableA("WNS_OLD_NOSCAN", "1");
        child(g_exe, od, "record", "scan", 0);
        SetEnvironmentVariableA("WNS_OLD_NOSCAN", 0);
        const std::string on = session_name(od, "");
        const std::string ost = read_file(od + "\\sessions\\" + on + "\\session.vps");
        bool noscan = !ost.empty();
        for (size_t at = 16; at + 5 <= ost.size();) {
            const char k = ost[at];
            uint32_t len;
            memcpy(&len, &ost[at + 1], 4);
            if (k == 'D' || k == 'S' || k == 'O') noscan = false;
            at += 5 + len;
        }
        check(noscan, "its stream has no scan record");
        write_ini(od, (std::string("[session]\nplay=") + on + "\nlabel=replay\n").c_str());
        child(g_exe, od, "play", "scan", 0);
        const std::string orec = read_file(od + "\\result-record.txt"), orep = read_file(od + "\\result-play.txt");
        check(field(orec, "digests") == field(orep, "digests") && num(orep, "first_part") == -1 && num(orep, "compared") > 100,
              ("IDENTICAL: " + field(orep, "compared") + " frames compared").c_str());
        check(num(orep, "real_scans") == 8 && num(orep, "scans") == 0, "its scans were live");
        check(read_file(od + "\\test.log").find("has no directory scans") != std::string::npos, "the log says so");
    }

    printf("  solo (a single-player session: no network records)\n");
    d = make_run("solo");
    write_ini(d, "[session]\nrecord=1\n");
    child(g_exe, d, "record", "solo", 0);
    const std::string soname = session_name(d, "");
    const std::string ss = read_file(d + "\\sessions\\" + soname + "\\session.vps");
    bool none = !ss.empty();
    for (size_t at = 16; at + 5 <= ss.size();) {                      // walk the records: no 'C', 'Y' or 'V'
        const char k = ss[at];
        uint32_t len;
        memcpy(&len, &ss[at + 1], 4);
        if (k == 'C' || k == 'Y' || k == 'V' || k == 'D' || k == 'S' || k == 'O') none = false;
        at += 5 + len;
    }
    check(none, "its stream has no network or directory-scan record");

    printf("  solorace (a single-player race in a session: no network or physics-clock records, replayed identically)\n");
    d = make_run("solorace");
    write_ini(d, "[session]\nrecord=1\n");
    child(g_exe, d, "record", "solorace", 0);
    const std::string srname = session_name(d, "");
    const std::string srs = read_file(d + "\\sessions\\" + srname + "\\session.vps");
    bool srnone = !srs.empty();
    for (size_t at = 16; at + 5 <= srs.size();) {
        const char k = srs[at];
        uint32_t len;
        memcpy(&len, &srs[at + 1], 4);
        if (k == 'C' || k == 'Y' || k == 'V') srnone = false;
        at += 5 + len;
    }
    check(srnone, "its stream has no network or physics-clock record");
    write_ini(d, (std::string("[session]\nplay=") + srname + "\nlabel=replay\n").c_str());
    child(g_exe, d, "play", "solorace", 0);
    const std::string srp = read_file(d + "\\result-play.txt");
    check(num(srp, "first_part") == -1 && num(srp, "race_compared") > 30,
          ("IDENTICAL frames: " + field(srp, "compared") + " compared, " + field(srp, "race_compared") + " in the race").c_str());
    printf("\n%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}

// solo-compare <other exe>: the same deterministic single-player run on this build and the other (the committed
// recorder, /DSESSION_HEAD): the streams and traces must be the same bytes
static int solo_compare(const char* other) {
    printf("world_net_session: a single-player session, byte for byte against %s\n", other);
    std::string a = make_run("solo-a"), b = make_run("solo-b");
    write_ini(a, "[session]\nrecord=1\n");
    write_ini(b, "[session]\nrecord=1\n");
    child(g_exe, a, "record", "solo", 0);
    child(other, b, "record", "solo", 0);
    const std::string na = session_name(a, ""), nb = session_name(b, "");
    const std::string sa = read_file(a + "\\sessions\\" + na + "\\session.vps"), sb = read_file(b + "\\sessions\\" + nb + "\\session.vps");
    const std::string ta = read_file(a + "\\sessions\\" + na + "\\record.trace"), tb = read_file(b + "\\sessions\\" + nb + "\\record.trace");
    check(!sa.empty() && sa == sb, ("the streams are the same bytes (" + std::to_string(sa.size()) + " and " + std::to_string(sb.size()) + ")").c_str());
    check(!ta.empty() && ta == tb, ("the frame traces are the same bytes (" + std::to_string(ta.size()) + " and " + std::to_string(tb.size()) + ")").c_str());
    return g_failures ? 1 : 0;
}

int main(int argc, char** argv) {
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_net_cs);
    char me[MAX_PATH];
    GetModuleFileNameA(0, me, sizeof me);
    g_exe = me;
    if (argc >= 6 && !strcmp(argv[1], "child")) {
        g_root = argv[2], g_mode = argv[3], g_scenario = argv[4];
        const std::string extra = argv[5];
        if (extra == "main" || extra == "lobby" || extra == "human33" || extra == "pattern") g_fault = extra;
        if (extra == "copy2") g_copy = 2, g_two = true;
        if (extra == "copy1" || extra == "copy1-bcast") g_copy = 1, g_two = true;
        g_logfile = fopen((g_root + "\\test.log").c_str(), g_mode == "record" ? "w" : "a");
        static uint32_t state = 0;
        g_phys_state_storage = &state;
        const int r = run_game();
#ifndef SESSION_HEAD
#endif
        if (g_logfile) fclose(g_logfile);
        return r;
    }
    if (argc >= 3 && !strcmp(argv[1], "solo-compare")) return solo_compare(argv[2]);
    return parent();
}
