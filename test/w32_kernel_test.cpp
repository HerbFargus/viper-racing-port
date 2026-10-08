// w32_kernel_test.cpp -- the KERNEL32 / WINMM stand-ins of hook/w32_kernel.h against the real Windows API (relink
// stage R2a): each scenario runs twice, once on Windows' functions and once on the stand-ins, and the answers --
// return values, out parameters, buffers, GetLastError -- must be the same. Exhaustive where the game's inputs are a
// small world (code pages over the whole BMP, the short date of every day 1990-2030, the money texts LocaleMoney
// makes), plus the error paths. Prints one line per difference and a summary; exit code = number of failures.
//
//   MSVC:  cl /nologo /O2 /EHsc /std:c++17 /W3 test\w32_kernel_test.cpp hook\w32_handle.cpp hook\w32_mem.cpp
//          hook\w32_thread.cpp hook\w32_time.cpp hook\w32_proc.cpp hook\w32_locale.cpp hook\w32_seh.cpp
//          /Fo"test\build\\" /Fe:test\build\w32_kernel_test.exe winmm.lib user32.lib
//   GCC:   g++ -m32 -O2 -masm=intel -std=c++17 -static test/w32_kernel_test.cpp hook/w32_handle.cpp hook/w32_mem.cpp
//          hook/w32_thread.cpp hook/w32_time.cpp hook/w32_proc.cpp hook/w32_locale.cpp hook/w32_seh.cpp
//          -o test/build/w32_kernel_test_gcc.exe -lwinmm
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <atomic>
#include "../hook/w32_kernel.h"

static int g_fails, g_checks;
static std::string g_section;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fails++;                                                         \
            if (g_fails < 400) {                                               \
                printf("FAIL [%s] %s:%d: ", g_section.c_str(), __FILE__, __LINE__); \
                printf(__VA_ARGS__);                                           \
                printf("\n");                                                  \
            }                                                                  \
        }                                                                      \
    } while (0)

// the last error, set to a mark first on both sides: what each call leaves
static const DWORD MARK = 0x1234abcd;
static void mark_both() {
    SetLastError(MARK);
    w32::set_last_error(MARK);
}

// ---- the API both ways ------------------------------------------------------------------------------------------------
struct Api {
    bool real;
    uint32_t(W32K_CALL* CreateThread)(void*, uint32_t, W32ThreadProc, void*, uint32_t, uint32_t*);
    uint32_t(W32K_CALL* WaitForSingleObject)(uint32_t, uint32_t);
    int32_t (*CloseHandle)(uint32_t);
    uint32_t(W32K_CALL* SuspendThread)(uint32_t);
    uint32_t(W32K_CALL* ResumeThread)(uint32_t);
    int32_t(W32K_CALL* GetExitCodeThread)(uint32_t, uint32_t*);
    int32_t(W32K_CALL* SetThreadPriority)(uint32_t, int32_t);
    void(W32K_CALL* ExitThread)(uint32_t);
    uint32_t(W32K_CALL* GetCurrentThreadId)();
    uint32_t(W32K_CALL* GetCurrentThread)();
    uint32_t(W32K_CALL* CreateEventA)(void*, int32_t, int32_t, const char*);
    int32_t(W32K_CALL* PulseEvent)(uint32_t);
    int32_t(W32K_CALL* SetEvent)(uint32_t);
    int32_t(W32K_CALL* ResetEvent)(uint32_t);
    uint32_t(W32K_CALL* CreateSemaphoreA)(void*, int32_t, int32_t, const char*);
    int32_t(W32K_CALL* ReleaseSemaphore)(uint32_t, int32_t, int32_t*);
    uint32_t(W32K_CALL* GetLastError)();
    void(W32K_CALL* InitializeCriticalSection)(W32CriticalSection*);
    void(W32K_CALL* EnterCriticalSection)(W32CriticalSection*);
    void(W32K_CALL* LeaveCriticalSection)(W32CriticalSection*);
    void(W32K_CALL* DeleteCriticalSection)(W32CriticalSection*);
    uint32_t(W32K_CALL* HeapCreate)(uint32_t, uint32_t, uint32_t);
    int32_t(W32K_CALL* HeapDestroy)(uint32_t);
    void*(W32K_CALL* HeapAlloc)(uint32_t, uint32_t, uint32_t);
    void*(W32K_CALL* HeapReAlloc)(uint32_t, uint32_t, void*, uint32_t);
    int32_t(W32K_CALL* HeapFree)(uint32_t, uint32_t, void*);
    uint32_t(W32K_CALL* HeapSize)(uint32_t, uint32_t, const void*);
    int32_t(W32K_CALL* HeapValidate)(uint32_t, uint32_t, const void*);
};
static int32_t real_close(uint32_t h) { return CloseHandle((HANDLE)(uintptr_t)h); }
static int32_t stand_close(uint32_t h) { return w32::close(h); }
static uint32_t W32K_CALL stand_last_error() { return w32::last_error(); }   // (w32_GetLastError: w32_file.cpp's)
#define R(f) (decltype(Api::f))(void*)&::f
#define S(f) &w32_##f
static Api real_api() {
    Api a = {true, R(CreateThread), R(WaitForSingleObject), real_close, R(SuspendThread), R(ResumeThread),
             R(GetExitCodeThread), R(SetThreadPriority), R(ExitThread), R(GetCurrentThreadId), R(GetCurrentThread),
             R(CreateEventA), R(PulseEvent), R(SetEvent), R(ResetEvent), R(CreateSemaphoreA), R(ReleaseSemaphore),
             R(GetLastError), R(InitializeCriticalSection), R(EnterCriticalSection), R(LeaveCriticalSection),
             R(DeleteCriticalSection), R(HeapCreate), R(HeapDestroy), R(HeapAlloc), R(HeapReAlloc), R(HeapFree),
             R(HeapSize), R(HeapValidate)};
    return a;
}
static Api stand_api() {
    Api a = {false, S(CreateThread), S(WaitForSingleObject), stand_close, S(SuspendThread), S(ResumeThread),
             S(GetExitCodeThread), S(SetThreadPriority), S(ExitThread), S(GetCurrentThreadId), S(GetCurrentThread),
             S(CreateEventA), S(PulseEvent), S(SetEvent), S(ResetEvent), S(CreateSemaphoreA), S(ReleaseSemaphore),
             stand_last_error, S(InitializeCriticalSection), S(EnterCriticalSection), S(LeaveCriticalSection),
             S(DeleteCriticalSection), S(HeapCreate), S(HeapDestroy), S(HeapAlloc), S(HeapReAlloc), S(HeapFree),
             S(HeapSize), S(HeapValidate)};
    return a;
}
static void set_err(const Api& a, uint32_t e) {
    if (a.real) SetLastError(e);
    else w32::set_last_error(e);
}

// a scenario's observations, compared between the two runs
struct Log {
    std::vector<std::string> v;
    void add(const char* fmt, ...) {
        char b[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        v.push_back(b);
    }
};
static void compare(const char* what, const Log& r, const Log& s) {
    const size_t n = r.v.size() > s.v.size() ? r.v.size() : s.v.size();
    for (size_t i = 0; i < n; i++) {
        const std::string a = i < r.v.size() ? r.v[i] : "(none)", b = i < s.v.size() ? s.v[i] : "(none)";
        CHECK(a == b, "%s #%u: Windows \"%s\", stand-in \"%s\"", what, (unsigned)i, a.c_str(), b.c_str());
    }
}
template <class F> static void both(const char* what, F scenario) {
    Log r, s;
    Api ra = real_api(), sa = stand_api();
    scenario(ra, r);
    scenario(sa, s);
    compare(what, r, s);
}

static uint16_t fpu_cw() {
    uint16_t cw;
#if defined(_MSC_VER)
    __asm fnstcw cw
#else
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
#endif
    return cw;
}
static void set_fpu_cw(uint16_t cw) {
#if defined(_MSC_VER)
    __asm fldcw cw
#else
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
#endif
}

// ==== heap ==============================================================================================================
static void test_heap() {
    g_section = "heap";
    both("heap basics", [](const Api& a, Log& L) {
        const uint32_t h = a.HeapCreate(HEAP_NO_SERIALIZE, 0x1000, 0);
        L.add("create %d", h != 0);
        void* z = a.HeapAlloc(h, 0, 0);
        L.add("alloc0 %d size %u align8 %d", z != 0, a.HeapSize(h, 0, z), ((uintptr_t)z & 7) == 0);
        uint8_t* p = (uint8_t*)a.HeapAlloc(h, HEAP_ZERO_MEMORY, 100);
        bool zero = true;
        for (int i = 0; i < 100; i++) zero &= p[i] == 0;
        L.add("alloc100 zeroed %d size %u", zero, a.HeapSize(h, 0, p));
        for (int i = 0; i < 100; i++) p[i] = (uint8_t)i;
        uint8_t* q = (uint8_t*)a.HeapReAlloc(h, HEAP_ZERO_MEMORY, p, 5000);
        bool kept = true, grown_zero = true;
        for (int i = 0; i < 100; i++) kept &= q[i] == (uint8_t)i;
        for (int i = 100; i < 5000; i++) grown_zero &= q[i] == 0;
        L.add("realloc grow kept %d zeroed %d size %u", kept, grown_zero, a.HeapSize(h, 0, q));
        uint8_t* r = (uint8_t*)a.HeapReAlloc(h, HEAP_REALLOC_IN_PLACE_ONLY, q, 50);
        L.add("realloc shrink in place same %d size %u", r == q, a.HeapSize(h, 0, r));
        mark_both();
        L.add("validate heap %d block %d block+4 %d", a.HeapValidate(h, 0, 0), a.HeapValidate(h, 0, r),
              a.HeapValidate(h, 0, r + 4));
        set_err(a, MARK);
        L.add("free null %d err %x", a.HeapFree(h, 0, 0), a.GetLastError());
        set_err(a, MARK);
        L.add("free %d err %x", a.HeapFree(h, 0, r), a.GetLastError());
        set_err(a, MARK);
        void* big = a.HeapAlloc(h, 0, 0x7ffffff0);
        L.add("alloc huge %d err %x", big != 0, a.GetLastError());
        L.add("free z %d", a.HeapFree(h, 0, z));
        // many blocks, then destroy with them live
        for (int i = 0; i < 1000; i++) a.HeapAlloc(h, 0, (uint32_t)(i * 7 % 300));
        L.add("destroy %d", a.HeapDestroy(h));
        // a fixed-size heap's limits
        const uint32_t f = a.HeapCreate(0, 0x1000, 0x10000);
        void* b1 = a.HeapAlloc(f, 0, 0x8000);
        void* b2 = a.HeapAlloc(f, 0, 0x8000);
        void* b3 = a.HeapAlloc(f, 0, 0x80000);
        L.add("fixed heap %d: 32k %d, another 32k %d, 512k %d", f != 0, b1 != 0, b2 != 0, b3 != 0);
        a.HeapDestroy(f);
        set_err(a, MARK);
        L.add("create init>max %d err %x", a.HeapCreate(0, 0x20000, 0x10000) != 0, a.GetLastError());
    });
    // the stand-in alone: a random workload against a model, two threads at once
    for (int round = 0; round < 2; round++) {
        const uint32_t h = w32_HeapCreate(HEAP_NO_SERIALIZE, 0x1000, 0);
        std::atomic<int> bad(0);
        auto work = [&](uint32_t seed) {
            std::vector<std::pair<uint8_t*, uint32_t>> live;
            uint32_t x = seed;
            for (int i = 0; i < 20000; i++) {
                x = x * 1103515245 + 12345;
                const uint32_t op = (x >> 16) % 3, n = (x >> 8) % 2000;
                if (op == 0 || live.empty()) {
                    uint8_t* p = (uint8_t*)w32_HeapAlloc(h, 0, n);
                    memset(p, (int)(n & 0xff), n);
                    live.push_back({p, n});
                } else if (op == 1) {
                    const size_t k = x % live.size();
                    uint8_t* p = (uint8_t*)w32_HeapReAlloc(h, 0, live[k].first, n);
                    const uint32_t keep = n < live[k].second ? n : live[k].second;
                    for (uint32_t j = 0; j < keep; j++)
                        if (p[j] != (uint8_t)(live[k].second & 0xff)) { bad++; break; }
                    memset(p, (int)(n & 0xff), n);
                    live[k] = {p, n};
                } else {
                    const size_t k = x % live.size();
                    if (w32_HeapSize(h, 0, live[k].first) != live[k].second) bad++;
                    for (uint32_t j = 0; j < live[k].second; j++)
                        if (live[k].first[j] != (uint8_t)(live[k].second & 0xff)) { bad++; break; }
                    if (!w32_HeapFree(h, 0, live[k].first)) bad++;
                    live[k] = live.back();
                    live.pop_back();
                }
            }
        };
        struct Run {
            static DWORD WINAPI go(void* p) {
                (*(decltype(work)*)p)(77);
                return 0;
            }
        };
        HANDLE t = CreateThread(0, 0, Run::go, &work, 0, 0);
        work(round + 1);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        CHECK(bad == 0, "random heap workload: %d mismatches", bad.load());
        CHECK(w32_HeapValidate(h, 0, 0), "heap valid after the workload");
        CHECK(w32_HeapDestroy(h), "destroy after the workload");
    }
    // GlobalAlloc: Windows' own on Windows (the clipboard's blocks)
    const uint32_t g = w32_GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 64);
    char* gp = (char*)w32_GlobalLock(g);
    CHECK(g && gp, "GlobalAlloc / GlobalLock");
    if (gp) strcpy(gp, "clip");
    mark_both();
    CHECK(w32_GlobalUnlock(g) == 0 && w32::last_error() == 0, "GlobalUnlock to 0: FALSE, NO_ERROR");
    uint32_t gn = 0;
    CHECK(!strcmp((char*)w32::global_memory(g, &gn), "clip") && gn == 64, "global_memory");
    GlobalFree((HGLOBAL)(uintptr_t)g);
}

// ==== the machine ======================================================================================================
static void test_system() {
    g_section = "system";
    SYSTEM_INFO r;
    W32SystemInfo s;
    GetSystemInfo(&r);
    w32_GetSystemInfo(&s);
    CHECK(!memcmp(&r, &s, sizeof r), "GetSystemInfo");
    typedef DWORD(WINAPI * GV)();
    const GV gv = (GV)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetVersion");
    CHECK(gv() == w32_GetVersion(), "GetVersion %08lx vs %08x", gv(), w32_GetVersion());
    printf("  this machine: GetVersion %08lx, %lu processors, max address %p\n", gv(), r.dwNumberOfProcessors,
           r.lpMaximumApplicationAddress);
}

// ==== threads ==========================================================================================================
struct Shared {
    const Api* a;
    std::atomic<int> stage{0};
    std::atomic<int> counter{0};
    uint32_t self = 0;                           // the thread's handle (to suspend itself)
    uint32_t tid_seen = 0;
    uint16_t cw_seen = 0;
    std::atomic<bool> stop{false};
};
static uint32_t W32K_CALL th_return42(void* p) {
    Shared* s = (Shared*)p;
    s->tid_seen = s->a->GetCurrentThreadId();
    s->cw_seen = fpu_cw();
    while (s->stage.load() == 0) Sleep(1);
    return 42;
}
static uint32_t W32K_CALL th_suspend_self(void* p) {
    Shared* s = (Shared*)p;
    while (!s->self) Sleep(1);
    s->stage = 1;
    s->a->SuspendThread(s->self);                // as TaskSuspendMe
    s->stage = 2;
    return 0;
}
static uint32_t W32K_CALL th_count(void* p) {
    Shared* s = (Shared*)p;
    while (!s->stop) s->counter++;
    return 5;
}
static uint32_t W32K_CALL th_exit7(void* p) {
    Shared* s = (Shared*)p;
    s->a->ExitThread(7);
    s->stage = 99;                               // (never)
    return 0;
}
static uint32_t W32K_CALL th_priority(void* p) {
    Shared* s = (Shared*)p;
    const Api& a = *s->a;
    char b[64];
    std::string out;
    for (int pr : {15, -15, 2, -2, 0, 3, -3, 16, 1}) {
        set_err(a, MARK);
        const int32_t r = a.SetThreadPriority(a.GetCurrentThread(), pr);
        sprintf(b, "%d:%d/%x ", pr, r, r ? 0 : a.GetLastError());
        out += b;
    }
    s->counter = 0;
    strcpy((char*)p + sizeof(Shared), out.c_str());
    return 0;
}

static void test_threads() {
    g_section = "threads";
    both("create, exit code, id, x87", [](const Api& a, Log& L) {
        Shared s;
        s.a = &a;
        const uint16_t cw0 = fpu_cw();
        set_fpu_cw(0x37f);                       // the creator's control word isn't inherited (Windows: 0x27f)
        uint32_t tid = 0;
        const uint32_t h = a.CreateThread(0, 0, th_return42, &s, 0, &tid);
        set_fpu_cw(cw0);
        uint32_t code = 0;
        const int32_t r1 = a.GetExitCodeThread(h, &code);
        L.add("running: GetExitCodeThread %d code %u", r1, code);
        L.add("wait 20ms: %x", a.WaitForSingleObject(h, 20));
        s.stage = 1;
        L.add("wait: %x", a.WaitForSingleObject(h, INFINITE));
        a.GetExitCodeThread(h, &code);
        L.add("ended: code %u; tid reported = tid seen %d; new thread's x87 cw %04x", code, tid == s.tid_seen && tid,
              s.cw_seen);
        L.add("wait again: %x", a.WaitForSingleObject(h, 0));
        L.add("close %d", a.CloseHandle(h));
        set_err(a, MARK);
        L.add("closed handle: GetExitCodeThread %d err %u", a.GetExitCodeThread(h, &code), a.GetLastError());
        set_err(a, MARK);
        L.add("bad handle: wait %x err %u", a.WaitForSingleObject(0x1234567c, 0), a.GetLastError());
        set_err(a, MARK);
        L.add("null handle: wait %x err %u", a.WaitForSingleObject(0, 0), a.GetLastError());
        set_err(a, MARK);
        L.add("this process: wait 10ms %x err %x", a.WaitForSingleObject(0xffffffffu, 10), a.GetLastError());
        set_err(a, MARK);
        L.add("bad handle: suspend %x resume %x err %u", a.SuspendThread(0x1234567c), a.ResumeThread(0x1234567c),
              a.GetLastError());
    });
    both("self-suspend and resume (TaskSuspendMe / TaskResume)", [](const Api& a, Log& L) {
        Shared s;
        s.a = &a;
        const uint32_t h = a.CreateThread(0, 0, th_suspend_self, &s, 0, 0);
        s.self = h;
        while (s.stage.load() != 1) Sleep(1);
        Sleep(50);
        L.add("stage %d", s.stage.load());
        const uint32_t c1 = a.SuspendThread(h);  // session.cpp's look: its count, and put back
        const uint32_t c2 = a.ResumeThread(h);
        L.add("look: suspend says %u, resume says %u, stage %d", c1, c2, s.stage.load());
        L.add("TaskResume: ResumeThread says %u", a.ResumeThread(h));
        const uint32_t w = a.WaitForSingleObject(h, 2000);
        L.add("wait %x stage %d", w, s.stage.load());
        L.add("resume a running/ended thread: %u", a.ResumeThread(h));
        a.CloseHandle(h);
    });
    both("suspend another thread", [](const Api& a, Log& L) {
        Shared s;
        s.a = &a;
        const uint32_t h = a.CreateThread(0, 0, th_count, &s, 0, 0);
        while (s.counter.load() < 1000) Sleep(1);
        L.add("suspend %u", a.SuspendThread(h));
        L.add("again %u", a.SuspendThread(h));
        const int c0 = s.counter.load();
        Sleep(50);
        L.add("stopped %d", s.counter.load() == c0);
        L.add("resume %u", a.ResumeThread(h));
        Sleep(20);
        L.add("still stopped %d", s.counter.load() == c0);
        L.add("resume %u", a.ResumeThread(h));
        Sleep(50);
        L.add("running %d", s.counter.load() != c0);
        L.add("resume not suspended %u", a.ResumeThread(h));
        s.stop = true;
        a.WaitForSingleObject(h, INFINITE);
        uint32_t code;
        a.GetExitCodeThread(h, &code);
        L.add("code %u", code);
        a.CloseHandle(h);
    });
    both("CREATE_SUSPENDED", [](const Api& a, Log& L) {
        Shared s;
        s.a = &a;
        const uint32_t h = a.CreateThread(0, 0, th_count, &s, CREATE_SUSPENDED, 0);
        Sleep(50);
        L.add("not started %d", s.counter.load() == 0);
        L.add("resume %u", a.ResumeThread(h));
        while (s.counter.load() == 0) Sleep(1);
        s.stop = true;
        L.add("wait %x", a.WaitForSingleObject(h, 2000));
        a.CloseHandle(h);
    });
    both("ExitThread", [](const Api& a, Log& L) {
        Shared s;
        s.a = &a;
        const uint32_t h = a.CreateThread(0, 0, th_exit7, &s, 0, 0);
        L.add("wait %x", a.WaitForSingleObject(h, 2000));
        uint32_t code;
        a.GetExitCodeThread(h, &code);
        L.add("code %u stage %d", code, s.stage.load());
        a.CloseHandle(h);
    });
    both("SetThreadPriority", [](const Api& a, Log& L) {
        struct { Shared s; char out[256]; } x;
        x.s.a = &a;
        const uint32_t h = a.CreateThread(0, 0, th_priority, &x, 0, 0);
        a.WaitForSingleObject(h, INFINITE);
        L.add("%s", x.out);
        set_err(a, MARK);
        L.add("on a handle: %d", a.SetThreadPriority(h, 1));
        a.CloseHandle(h);
        set_err(a, MARK);
        L.add("bad handle: %d err %u", a.SetThreadPriority(0x1234567c, 1), a.GetLastError());
    });
    // ids: the stand-in's are Windows' own here (the port's GetCurrentThreadId calls agree with the game's)
    CHECK(w32_GetCurrentThreadId() == GetCurrentThreadId(), "GetCurrentThreadId");
    CHECK(w32_GetCurrentThread() == (uint32_t)(uintptr_t)GetCurrentThread(), "GetCurrentThread");
    CHECK(w32_GetCurrentProcess() == (uint32_t)(uintptr_t)GetCurrentProcess(), "GetCurrentProcess");
    // GetExitCodeProcess of this process; CreateProcessA: not available
    DWORD rc = 0;
    uint32_t sc = 0;
    CHECK(GetExitCodeProcess(GetCurrentProcess(), &rc) == w32_GetExitCodeProcess(w32_GetCurrentProcess(), &sc) &&
              rc == sc, "GetExitCodeProcess(self): %lu vs %u", rc, sc);
    mark_both();
    CHECK(!w32_CreateProcessA(0, (char*)"no-such-program.exe", 0, 0, 0, 0, 0, 0, 0, 0) &&
              w32::last_error() == ERROR_FILE_NOT_FOUND, "CreateProcessA: not available");
    {
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        char cmd[] = "no-such-program-vp.exe";
        SetLastError(MARK);
        const BOOL r = CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi);
        CHECK(!r && GetLastError() == ERROR_FILE_NOT_FOUND, "Windows' CreateProcessA of a missing program: %d %lu", r,
              GetLastError());
    }
    // affinity, as the C runtime's FDIV check uses it (through GetProcAddress)
    DWORD_PTR rp, rs;
    uint32_t sp, ss;
    CHECK(GetProcessAffinityMask(GetCurrentProcess(), &rp, &rs) == w32_GetProcessAffinityMask(w32_GetCurrentProcess(), &sp, &ss) &&
              rp == sp && rs == ss, "GetProcessAffinityMask");
    const uint32_t was = w32_SetThreadAffinityMask(w32_GetCurrentThread(), 1);
    CHECK(was == (uint32_t)rp, "SetThreadAffinityMask returns the old mask %x (process %lx)", was, (unsigned long)rp);
    CHECK(w32_SetThreadAffinityMask(w32_GetCurrentThread(), (uint32_t)rp) == 1, "SetThreadAffinityMask back");
}

// ==== critical sections =================================================================================================
static std::string cs_fields(const W32CriticalSection& c, uint32_t tid) {
    char b[160];
    sprintf(b, "debug %s lock %d rec %d owner %s sem %s spin %08x", c.debug_info == 0xffffffffu ? "-1" : c.debug_info ? "ptr" : "0",
            c.lock_count, c.recursion_count, c.owning_thread == tid ? "me" : c.owning_thread ? "other" : "0",
            c.lock_semaphore ? "set" : "0", c.spin_count);
    return b;
}
static uint32_t W32K_CALL th_cs_add(void* p) {
    struct X { const Api* a; W32CriticalSection* cs; int* n; };
    X* x = (X*)p;
    for (int i = 0; i < 20000; i++) {
        x->a->EnterCriticalSection(x->cs);
        x->a->EnterCriticalSection(x->cs);
        const int v = *x->n;
        *x->n = v + 1;
        x->a->LeaveCriticalSection(x->cs);
        x->a->LeaveCriticalSection(x->cs);
    }
    return 0;
}
static void test_critical_sections() {
    g_section = "critical sections";
    W32CriticalSection r, s;
    InitializeCriticalSection((CRITICAL_SECTION*)&r);
    w32_InitializeCriticalSection(&s);
    const uint32_t me = GetCurrentThreadId();
    printf("  Windows' CRITICAL_SECTION after Initialize: %s\n", cs_fields(r, me).c_str());
    // what the stand-in mirrors: the owner and the recursion count (nothing reads the others)
    EnterCriticalSection((CRITICAL_SECTION*)&r);
    EnterCriticalSection((CRITICAL_SECTION*)&r);
    w32_EnterCriticalSection(&s);
    w32_EnterCriticalSection(&s);
    CHECK(r.owning_thread == s.owning_thread && r.recursion_count == s.recursion_count,
          "entered twice: Windows %s / stand-in %s", cs_fields(r, me).c_str(), cs_fields(s, me).c_str());
    LeaveCriticalSection((CRITICAL_SECTION*)&r);
    w32_LeaveCriticalSection(&s);
    CHECK(r.owning_thread == s.owning_thread && r.recursion_count == s.recursion_count, "left once: %s / %s",
          cs_fields(r, me).c_str(), cs_fields(s, me).c_str());
    LeaveCriticalSection((CRITICAL_SECTION*)&r);
    w32_LeaveCriticalSection(&s);
    CHECK(r.owning_thread == s.owning_thread && r.recursion_count == s.recursion_count && r.lock_count == s.lock_count,
          "left: %s / %s", cs_fields(r, me).c_str(), cs_fields(s, me).c_str());
    printf("  Windows' after Delete: ");
    DeleteCriticalSection((CRITICAL_SECTION*)&r);
    w32_DeleteCriticalSection(&s);
    printf("%s\n", cs_fields(r, me).c_str());
    CHECK(!memcmp(&r, &s, sizeof r), "after Delete: Windows %s / stand-in %s", cs_fields(r, me).c_str(),
          cs_fields(s, me).c_str());
    // four threads, nested entries
    both("contention", [](const Api& a, Log& L) {
        W32CriticalSection cs;
        a.InitializeCriticalSection(&cs);
        int n = 0;
        struct X { const Api* a; W32CriticalSection* cs; int* n; } x = {&a, &cs, &n};
        uint32_t h[4];
        for (uint32_t& t : h) t = a.CreateThread(0, 0, th_cs_add, &x, 0, 0);
        for (uint32_t t : h) {
            a.WaitForSingleObject(t, INFINITE);
            a.CloseHandle(t);
        }
        L.add("count %d", n);
        a.DeleteCriticalSection(&cs);
    });
}

// ==== events, semaphores ==================================================================================================
struct Waiters {
    const Api* a;
    uint32_t ev;
    std::atomic<int> started{0}, woken{0};
};
static uint32_t W32K_CALL th_wait(void* p) {
    Waiters* w = (Waiters*)p;
    w->started++;
    if (w->a->WaitForSingleObject(w->ev, 3000) == WAIT_OBJECT_0) w->woken++;
    return 0;
}
static void test_events() {
    g_section = "events";
    for (int manual = 0; manual < 2; manual++) {
        both(manual ? "PulseEvent, manual reset" : "PulseEvent, auto reset", [manual](const Api& a, Log& L) {
            set_err(a, MARK);
            Waiters w;
            w.a = &a;
            w.ev = a.CreateEventA(0, manual, FALSE, 0);
            L.add("create err %x", a.GetLastError());
            L.add("pulse, no one waiting %d; then wait(0) %x", a.PulseEvent(w.ev), a.WaitForSingleObject(w.ev, 0));
            uint32_t h[3];
            for (uint32_t& t : h) t = a.CreateThread(0, 0, th_wait, &w, 0, 0);
            while (w.started.load() < 3) Sleep(1);
            Sleep(100);                          // (all three in their wait)
            a.PulseEvent(w.ev);
            Sleep(100);
            L.add("first pulse woke %d", w.woken.load());
            L.add("event after the pulse: wait(0) %x", a.WaitForSingleObject(w.ev, 0));
            a.PulseEvent(w.ev);
            Sleep(100);
            L.add("second pulse: %d woken", w.woken.load());
            a.SetEvent(w.ev);
            Sleep(100);
            L.add("set: %d woken; wait(0) %x", w.woken.load(), a.WaitForSingleObject(w.ev, 0));
            a.ResetEvent(w.ev);
            L.add("reset: wait(0) %x", a.WaitForSingleObject(w.ev, 0));
            for (uint32_t t : h) {
                a.WaitForSingleObject(t, INFINITE);
                a.CloseHandle(t);
            }
            a.CloseHandle(w.ev);
            set_err(a, MARK);
            L.add("pulse a closed event %d err %u", a.PulseEvent(w.ev), a.GetLastError());
        });
    }
    both("SetEvent, auto reset: one waiter at a time", [](const Api& a, Log& L) {
        const uint32_t e = a.CreateEventA(0, FALSE, TRUE, 0);
        L.add("initially set: %x then %x", a.WaitForSingleObject(e, 0), a.WaitForSingleObject(e, 0));
        a.SetEvent(e);
        a.SetEvent(e);
        L.add("set twice: %x then %x", a.WaitForSingleObject(e, 0), a.WaitForSingleObject(e, 0));
        a.CloseHandle(e);
    });
    both("named objects", [](const Api& a, Log& L) {
        char name[64];
        sprintf(name, "vp w32 test %s %lu", a.real ? "r" : "s", GetCurrentProcessId());
        set_err(a, MARK);
        const uint32_t e1 = a.CreateEventA(0, TRUE, FALSE, name);
        L.add("first %d err %u", e1 != 0, a.GetLastError());
        set_err(a, MARK);
        const uint32_t e2 = a.CreateEventA(0, FALSE, TRUE, name);   // (the existing one: its kind and state)
        L.add("second %d err %u, distinct handle %d", e2 != 0, a.GetLastError(), e1 != e2);
        a.SetEvent(e2);
        L.add("one object: set on 2, wait on 1 %x, still set (manual) %x", a.WaitForSingleObject(e1, 0),
              a.WaitForSingleObject(e1, 0));
        set_err(a, MARK);
        const uint32_t s = a.CreateSemaphoreA(0, 0, 1, name);
        L.add("semaphore of an event's name %d err %u", s != 0, a.GetLastError());
        a.CloseHandle(e1);
        a.CloseHandle(e2);
        set_err(a, MARK);
        const uint32_t e3 = a.CreateEventA(0, FALSE, FALSE, name);
        L.add("after closing both: new %d err %u", e3 != 0, a.GetLastError());
        a.CloseHandle(e3);
    });
    both("semaphores", [](const Api& a, Log& L) {
        const int32_t args[][2] = {{0, 1}, {1, 1}, {2, 1}, {-1, 1}, {0, 0}, {0, -1}, {5, 10}};
        for (auto& x : args) {
            set_err(a, MARK);
            const uint32_t s = a.CreateSemaphoreA(0, x[0], x[1], 0);
            L.add("(%d, %d): %d err %u", x[0], x[1], s != 0, a.GetLastError());
            if (s) a.CloseHandle(s);
        }
        const uint32_t s = a.CreateSemaphoreA(0, 1, 2, 0);
        L.add("wait %x %x", a.WaitForSingleObject(s, 0), a.WaitForSingleObject(s, 10));
        int32_t prev = -5;
        L.add("release 1: %d prev %d", a.ReleaseSemaphore(s, 1, &prev), prev);
        set_err(a, MARK);
        prev = -5;
        L.add("release 2 (over max): %d prev %d err %u", a.ReleaseSemaphore(s, 2, &prev), prev, a.GetLastError());
        set_err(a, MARK);
        L.add("release 0: %d err %u", a.ReleaseSemaphore(s, 0, &prev), a.GetLastError());
        a.CloseHandle(s);
    });
    // the single-instance check (start_unique_instance): a name held by ANOTHER process is seen. Here Windows' own
    // named semaphore stands for the other copy of the game.
    char name[64];
    sprintf(name, "vp w32 test unique %lu", GetCurrentProcessId());
    HANDLE other = CreateSemaphoreA(0, 0, 1, name);
    mark_both();
    const uint32_t mine = w32_CreateSemaphoreA(0, 0, 1, name);
    CHECK(mine && w32::last_error() == ERROR_ALREADY_EXISTS, "a name another process holds: %u err %u", mine,
          w32::last_error());
    w32::close(mine);
    CloseHandle(other);
    mark_both();
    const uint32_t alone = w32_CreateSemaphoreA(0, 0, 1, name);
    CHECK(alone && w32::last_error() == 0, "the name free again: err %u", w32::last_error());
    HANDLE seen = CreateSemaphoreA(0, 0, 1, name);
    CHECK(seen && GetLastError() == ERROR_ALREADY_EXISTS, "another process sees the stand-in's name");
    CloseHandle(seen);
    w32::close(alone);
}

// ==== time ===============================================================================================================
static void test_time() {
    g_section = "time";
    LARGE_INTEGER rf, rc;
    int64_t sf, sc;
    CHECK(QueryPerformanceFrequency(&rf) == w32_QueryPerformanceFrequency(&sf) && rf.QuadPart == sf, "QPF");
    QueryPerformanceCounter(&rc);
    w32_QueryPerformanceCounter(&sc);
    CHECK(sc >= rc.QuadPart && sc - rc.QuadPart < rf.QuadPart / 10, "QPC");
    SYSTEMTIME rt;
    W32SystemTime st;
    GetLocalTime(&rt);
    w32_GetLocalTime(&st);
    CHECK(rt.wYear == st.year && rt.wMonth == st.month && rt.wDay == st.day && rt.wDayOfWeek == st.day_of_week &&
              rt.wHour == st.hour && rt.wMinute == st.minute && (st.second - rt.wSecond + 60) % 60 <= 1,
          "GetLocalTime");
    const DWORD t0 = timeGetTime();
    const uint32_t t1 = w32_timeGetTime();
    CHECK(t1 - t0 < 100, "timeGetTime %lu %u", t0, t1);
    for (uint32_t id : {0u, 1u, 10u, 16u, 20u, 33u, 1000u})
        CHECK(timeKillEvent(id) == w32_timeKillEvent(id), "timeKillEvent(%u): %u vs %u", id, timeKillEvent(id),
              w32_timeKillEvent(id));
    const DWORD a = GetTickCount();
    w32_Sleep(30);
    CHECK(GetTickCount() - a >= 20, "Sleep(30)");
}

// ==== process image, environment, modules ===================================================================================
static void test_process() {
    g_section = "process";
    CHECK(!strcmp(GetCommandLineA(), w32_GetCommandLineA()), "GetCommandLineA (not told)");
    CHECK(w32_GetModuleHandleA(0) == (uint32_t)(uintptr_t)GetModuleHandleA(0), "GetModuleHandleA(NULL) (not told)");
    // told: this exe as "race.exe"
    char path[MAX_PATH];
    GetModuleFileNameA(0, path, MAX_PATH);
    const uint32_t base = (uint32_t)(uintptr_t)GetModuleHandleA(0);
    w32_set_process_image(base, path, "\"x\" -a");
    CHECK(!strcmp(w32_GetCommandLineA(), "\"x\" -a"), "GetCommandLineA (told)");
    SetLastError(MARK);
    const HMODULE rm = GetModuleHandleA(0);
    const DWORD re = GetLastError();
    w32::set_last_error(MARK);
    const uint32_t sm = w32_GetModuleHandleA(0);
    CHECK((uint32_t)(uintptr_t)rm == sm && re == w32::last_error(), "GetModuleHandleA(NULL): %p/%lx vs %x/%x", rm, re,
          sm, w32::last_error());
    const uint32_t len = (uint32_t)strlen(path);
    for (uint32_t n : {0u, 1u, 5u, len - 1, len, len + 1, (uint32_t)MAX_PATH}) {
        for (int which = 0; which < 2; which++) {
            const HMODULE m = which ? rm : 0;
            char a[MAX_PATH + 8], b[MAX_PATH + 8];
            memset(a, 'Z', sizeof a);
            memset(b, 'Z', sizeof b);
            mark_both();
            const DWORD r = GetModuleFileNameA(m, a, n);
            const uint32_t s = w32_GetModuleFileNameA((uint32_t)(uintptr_t)m, b, n);
            CHECK(r == s && GetLastError() == w32::last_error() && !memcmp(a, b, sizeof a),
                  "GetModuleFileNameA(%s, %u): %lu err %lx vs %u err %x", which ? "base" : "NULL", n, r, GetLastError(),
                  s, w32::last_error());
        }
    }
    // other modules: Windows'
    mark_both();
    CHECK((uint32_t)(uintptr_t)GetModuleHandleA("kernel32.dll") == w32_GetModuleHandleA("kernel32.dll") &&
              GetLastError() == w32::last_error(), "GetModuleHandleA(kernel32)");
    mark_both();
    CHECK(GetModuleHandleA("nope-vp.dll") == 0 && w32_GetModuleHandleA("nope-vp.dll") == 0 &&
              GetLastError() == w32::last_error(), "GetModuleHandleA(missing): err %lu vs %u", GetLastError(),
          w32::last_error());
    const uint32_t k = w32_LoadLibraryA((const char*)"KERNEL32");
    CHECK(k == (uint32_t)(uintptr_t)LoadLibraryA("KERNEL32"), "LoadLibraryA(KERNEL32)");
    mark_both();
    CHECK(!LoadLibraryA("nope-vp.dll") && !w32_LoadLibraryA("nope-vp.dll") && GetLastError() == w32::last_error(),
          "LoadLibraryA(missing): err %lu vs %u", GetLastError(), w32::last_error());
    // GetProcAddress: the C runtime's four through KERNEL32 are the stand-ins
    CHECK(w32_GetProcAddress(k, "GetCurrentProcess") == (void*)&w32_GetCurrentProcess &&
              w32_GetProcAddress(k, "GetCurrentThread") == (void*)&w32_GetCurrentThread &&
              w32_GetProcAddress(k, "GetProcessAffinityMask") == (void*)&w32_GetProcessAffinityMask &&
              w32_GetProcAddress(k, "SetThreadAffinityMask") == (void*)&w32_SetThreadAffinityMask,
          "GetProcAddress: the FDIV check's four");
    mark_both();
    CHECK(!GetProcAddress((HMODULE)(uintptr_t)k, "NoSuchFunctionVp") && !w32_GetProcAddress(k, "NoSuchFunctionVp") &&
              GetLastError() == w32::last_error(), "GetProcAddress(missing): err %lu vs %u", GetLastError(),
          w32::last_error());
    const uint32_t u = w32_LoadLibraryA("user32.dll");
    CHECK(w32_GetProcAddress(u, "MessageBoxA") == (void*)GetProcAddress((HMODULE)(uintptr_t)u, "MessageBoxA"),
          "GetProcAddress(user32, MessageBoxA): Windows' (no table registered)");
    w32_set_proc_lookup([](const char* dll, const char* name) -> void* {
        return !strcmp(dll, "USER32") && !strcmp(name, "MessageBoxA") ? (void*)&w32_GetACP : 0;
    });
    CHECK(w32_GetProcAddress(u, "MessageBoxA") == (void*)&w32_GetACP, "GetProcAddress: the registered table first");
    w32_set_proc_lookup(0);
    // the environment (Windows' own block on Windows)
    LPWCH rw = GetEnvironmentStringsW();
    uint16_t* sw = w32_GetEnvironmentStringsW();
    size_t n = 0;
    while (rw[n] || rw[n + 1]) n++;
    CHECK(!memcmp(rw, sw, (n + 2) * 2), "GetEnvironmentStringsW");
    FreeEnvironmentStringsW(rw);
    CHECK(w32_FreeEnvironmentStringsW(sw), "FreeEnvironmentStringsW");
    CHECK(w32_SetEnvironmentVariableA("VP_W32_TEST", "1") && getenv("VP_W32_TEST") == 0, "SetEnvironmentVariableA");
    char ev[8] = "";
    CHECK(GetEnvironmentVariableA("VP_W32_TEST", ev, 8) == 1 && ev[0] == '1', "SetEnvironmentVariableA: Windows sees it");
    mark_both();
    CHECK(SetEnvironmentVariableA("VP_W32_NOT_THERE", 0) == w32_SetEnvironmentVariableA("VP_W32_NOT_THERE", 0) &&
              GetLastError() == w32::last_error(), "removing a missing variable");
    // GetStartupInfoA, SetHandleCount
    STARTUPINFOA rs;
    W32StartupInfoA ss;
    GetStartupInfoA(&rs);
    w32_GetStartupInfoA(&ss);
    CHECK(!memcmp(&rs, &ss, sizeof rs), "GetStartupInfoA");
    typedef UINT(WINAPI * SHC)(UINT);
    const SHC shc = (SHC)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetHandleCount");
    for (uint32_t c : {0u, 20u, 32u, 255u, 1000u}) CHECK(shc(c) == w32_SetHandleCount(c), "SetHandleCount(%u)", c);
}

// ==== locale and strings ======================================================================================================
static void test_code_pages() {
    g_section = "code pages";
    CHECK(GetACP() == w32_GetACP() && GetOEMCP() == w32_GetOEMCP(), "GetACP/GetOEMCP");
    for (uint32_t cp : {0u, 1u, 3u, 437u, 1252u, 12345u}) {
        CPINFO r;
        W32CpInfo s;
        memset(&r, 0x55, sizeof r);
        memset(&s, 0x55, sizeof s);
        mark_both();
        const BOOL a = GetCPInfo(cp, &r);
        const int32_t b = w32_GetCPInfo(cp, &s);
        CHECK(a == b && GetLastError() == w32::last_error() && (!a || !memcmp(&r, &s, 18)), "GetCPInfo(%u)", cp);
    }
    // MultiByteToWideChar: every byte, the C runtime's flags
    for (uint32_t cp : {0u, 1u, 437u, 1252u}) {
        for (uint32_t fl : {0u, 1u, 8u, 9u}) {
            for (int b = 0; b < 256; b++) {
                const char c = (char)b;
                WCHAR rw = 0xffff;
                uint16_t sw = 0xffff;
                mark_both();
                const int r = MultiByteToWideChar(cp, fl, &c, 1, &rw, 1);
                const int s = w32_MultiByteToWideChar(cp, fl, &c, 1, &sw, 1);
                CHECK(r == s && rw == sw && GetLastError() == w32::last_error(), "MBTWC(%u, %x, %02x): %d %04x vs %d %04x",
                      cp, fl, b, r, rw, s, sw);
            }
        }
    }
    {   // strings, sizes, errors
        const char* str = "Viper \xe9t\xe9 \x80\x81\x9f!";
        struct C { uint32_t cp, fl; const char* s; int n, dn; } cases[] = {
            {0, 0, str, -1, 64}, {0, 0, str, -1, 0}, {0, 0, str, 5, 64}, {0, 0, str, -1, 3}, {0, 0, str, 0, 64},
            {0, 0x10, str, -1, 64}, {0, 3, str, -1, 64}, {0, 2, str, -1, 64}, {65001, 0, str, -1, 64}, {0, 0, 0, -1, 64},
            {0, 0, str, -2, 64}, {12345, 0, str, -1, 64},
        };
        for (const C& c : cases) {
            WCHAR rw[80];
            uint16_t sw[80];
            memset(rw, 0x11, sizeof rw);
            memset(sw, 0x11, sizeof sw);
            mark_both();
            const int r = MultiByteToWideChar(c.cp, c.fl, c.s, c.n, c.dn ? rw : 0, c.dn);
            const DWORD re = GetLastError();
            const int s = w32_MultiByteToWideChar(c.cp, c.fl, c.s, c.n, c.dn ? sw : 0, c.dn);
            const bool covered = c.cp != 65001 && c.fl != 2;
            if (!covered) {
                printf("  not covered: MultiByteToWideChar(cp %u, flags %x): Windows %d, stand-in %d err %u\n", c.cp,
                       c.fl, r, s, w32::last_error());
                continue;
            }
            CHECK(r == s && re == w32::last_error() && !memcmp(rw, sw, sizeof rw),
                  "MBTWC(%u, %x, n %d, dn %d): %d err %lu vs %d err %u", c.cp, c.fl, c.n, c.dn, r, re, s, w32::last_error());
        }
    }
    // WideCharToMultiByte: every BMP code unit, alone, with the flags the game and the C runtime pass
    int diffs = 0;
    for (uint32_t cp : {0u, 1u, 1252u, 437u}) {
        for (uint32_t fl : {0u, 0x220u, 0x400u, 0x200u}) {
            for (uint32_t u = 0; u < 0x10000; u++) {
                const WCHAR w = (WCHAR)u;
                char rb[4] = {0x55, 0x55}, sb[4] = {0x55, 0x55};
                BOOL ru = 7;
                int32_t su = 7;
                const int r = WideCharToMultiByte(cp, fl, &w, 1, rb, 4, 0, &ru);
                const int s = w32_WideCharToMultiByte(cp, fl, (const uint16_t*)&w, 1, sb, 4, 0, &su);
                if (r != s || rb[0] != sb[0] || ru != su) {
                    if (++diffs < 20)
                        CHECK(false, "WCTMB(%u, %x, U+%04x): %d %02x used %d vs %d %02x used %d", cp, fl, u, r,
                              (uint8_t)rb[0], ru, s, (uint8_t)sb[0], su);
                    else g_fails++;
                }
                g_checks++;
            }
        }
    }
    {
        const WCHAR ws[] = {'a', 0xe9, 0x20ac, 0x0101, 0xd83d, 0xde00, 'b', 0xd800, 'c', 0xdc00, 0x4e2d, 0};
        struct C { uint32_t cp, fl; int n, dn; const char* def; bool want_used; } cases[] = {
            {0, 0, -1, 64, 0, true}, {0, 0, -1, 0, 0, true}, {0, 0, 4, 64, 0, true}, {0, 0, -1, 3, 0, true},
            {0, 0x400, -1, 64, "#", true}, {0, 0x220, -1, 64, 0, false}, {0, 0x80, -1, 64, 0, true},
            {0, 0, 0, 64, 0, true}, {0, 0x10, -1, 64, 0, true}, {0, 0x40, -1, 64, 0, true}, {437, 0, -1, 64, 0, true},
            {1252, 0x1000, -1, 64, 0, true},
        };
        for (const C& c : cases) {
            char rb[80], sb[80];
            memset(rb, 0x11, sizeof rb);
            memset(sb, 0x11, sizeof sb);
            BOOL ru = 7;
            int32_t su = 7;
            mark_both();
            const int r = WideCharToMultiByte(c.cp, c.fl, ws, c.n, c.dn ? rb : 0, c.dn, c.def, c.want_used ? &ru : 0);
            const DWORD re = GetLastError();
            const int s = w32_WideCharToMultiByte(c.cp, c.fl, (const uint16_t*)ws, c.n, c.dn ? sb : 0, c.dn, c.def,
                                                  c.want_used ? &su : 0);
            CHECK(r == s && re == w32::last_error() && !memcmp(rb, sb, sizeof rb) && (!c.want_used || ru == su),
                  "WCTMB string (%u, %x, n %d, dn %d): %d err %lu used %d \"%.*s\" vs %d err %u used %d \"%.*s\"", c.cp,
                  c.fl, c.n, c.dn, r, re, ru, r > 0 ? r : 0, rb, s, w32::last_error(), su, s > 0 ? s : 0, sb);
        }
    }
    {   // WC_COMPOSITECHECK with combining marks: not covered (the C runtime converts single characters)
        const WCHAR ws[] = {'a', 0x0301, 'x', 0x0308, 0};
        char rb[16] = "", sb[16] = "";
        const int r = WideCharToMultiByte(1252, 0x200, ws, -1, rb, 16, 0, 0);
        const int s = w32_WideCharToMultiByte(1252, 0x200, (const uint16_t*)ws, -1, sb, 16, 0, 0);
        printf("  not covered: WC_COMPOSITECHECK of a + U+0301, x + U+0308: Windows %d bytes, stand-in %d\n", r, s);
    }
    // the C runtime's environment conversion, both ways round
    LPWCH env = GetEnvironmentStringsW();
    size_t n = 0;
    while (env[n] || env[n + 1]) n++;
    n += 2;
    const int r = WideCharToMultiByte(0, 0, env, (int)n, 0, 0, 0, 0);
    const int s = w32_WideCharToMultiByte(0, 0, (const uint16_t*)env, (int)n, 0, 0, 0, 0);
    std::vector<char> ra(r), sa(s > 0 ? s : 1);
    WideCharToMultiByte(0, 0, env, (int)n, ra.data(), r, 0, 0);
    w32_WideCharToMultiByte(0, 0, (const uint16_t*)env, (int)n, sa.data(), s, 0, 0);
    CHECK(r == s && !memcmp(ra.data(), sa.data(), r), "the environment block in code page 1252");
    FreeEnvironmentStringsW(env);
}

static void test_strings() {
    g_section = "LCMapString / GetStringType";
    int diffs = 0;
    for (uint32_t fl : {0x100u, 0x200u, 0x200u | 0x800u, 0x01000100u, 0x01000200u, 0x300u}) {
        for (uint32_t u = 1; u < 0x10000; u++) {
            const WCHAR w = (WCHAR)u;
            WCHAR ro = 0;
            uint16_t so = 0;
            const int r = LCMapStringW(LOCALE_USER_DEFAULT, fl, &w, 1, &ro, 1);
            const int s = w32_LCMapStringW(LOCALE_USER_DEFAULT, fl, (const uint16_t*)&w, 1, &so, 1);
            g_checks++;
            if (r != s || ro != so) {
                if (++diffs < 20) printf("FAIL [%s] LCMapStringW(%x, U+%04x): %d %04x vs %d %04x\n", g_section.c_str(), fl, u, r, ro, s, so);
                g_fails++;
            }
        }
    }
    {
        WCHAR ro[8];
        uint16_t so[8];
        mark_both();   // the C runtime's probe
        CHECK(LCMapStringW(0, 0x100, L"", 1, 0, 0) == w32_LCMapStringW(0, 0x100, (const uint16_t*)L"", 1, 0, 0),
              "LCMapStringW probe");
        for (const WCHAR* t : {L"viper gts", L"VIPER GTS", L"\x01c6" L"a \x01c4" L"B", L"o'neil mcDonald-x", L"\x00df" L"x",
                               L"abc1def 2nd x2 MiXeD lower-UPPER mIxed"}) {
            WCHAR ro[48] = {};
            uint16_t so[48] = {};
            const int r = LCMapStringW(0x400, 0x300, t, -1, ro, 48);
            const int s = w32_LCMapStringW(0x400, 0x300, (const uint16_t*)t, -1, so, 48);
            std::string a, b;
            for (int i = 0; i < 48 && (ro[i] || so[i]); i++) {
                char x[16];
                sprintf(x, " %04x/%04x", ro[i], so[i]);
                (ro[i] == so[i] ? a : b) += x;
            }
            CHECK(r == s && !memcmp(ro, so, sizeof ro), "LCMAP_TITLECASE of a string #%d: differ at%s", (int)(t[0] & 0xff),
                  b.c_str());
        }
        const WCHAR* t = L"Viper GTS";
        for (int dn : {0, 4, 8}) {
            for (uint32_t fl : {0x200u, 0u, 0x300u, 0x400u}) {
                mark_both();
                memset(ro, 0, sizeof ro);
                memset(so, 0, sizeof so);
                const int r = LCMapStringW(0x400, fl, t, -1, dn ? ro : 0, dn);
                const DWORD re = GetLastError();
                const int s = w32_LCMapStringW(0x400, fl, (const uint16_t*)t, -1, dn ? so : 0, dn);
                if (fl == 0x400) {
                    printf("  not covered: LCMapStringW(LCMAP_SORTKEY): Windows %d, stand-in %d err %u\n", r, s, w32::last_error());
                    continue;
                }
                CHECK(r == s && re == w32::last_error() && (!r || !dn || !memcmp(ro, so, r * 2)),
                      "LCMapStringW(%x, dn %d): %d err %lu vs %d err %u", fl, dn, r, re, s, w32::last_error());
            }
        }
    }
    for (uint32_t fl : {0x100u, 0x200u}) {
        for (int b = 1; b < 256; b++) {
            const char c = (char)b;
            char ro = 0, so = 0;
            const int r = LCMapStringA(LOCALE_USER_DEFAULT, fl, &c, 1, &ro, 1);
            const int s = w32_LCMapStringA(LOCALE_USER_DEFAULT, fl, &c, 1, &so, 1);
            CHECK(r == s && ro == so, "LCMapStringA(%x, %02x): %d %02x vs %d %02x", fl, b, r, (uint8_t)ro, s, (uint8_t)so);
        }
    }
    std::vector<WCHAR> all(0x10000);
    for (uint32_t u = 0; u < 0x10000; u++) all[u] = (WCHAR)u;
    for (uint32_t t : {1u, 2u, 4u}) {
        std::vector<WORD> r(0x10000);
        std::vector<uint16_t> s(0x10000);
        GetStringTypeW(t, all.data(), 0x10000, r.data());
        w32_GetStringTypeW(t, (const uint16_t*)all.data(), 0x10000, s.data());
        int bad = 0;
        for (uint32_t u = 0; u < 0x10000; u++) bad += r[u] != s[u];
        CHECK(!bad, "GetStringTypeW(%u): %d code units differ", t, bad);
        char a[256];
        for (int b = 0; b < 256; b++) a[b] = (char)b;
        WORD ra[256];
        uint16_t sa[256];
        const BOOL x = GetStringTypeA(LOCALE_USER_DEFAULT, t, a, 256, ra);
        const int32_t y = w32_GetStringTypeA(LOCALE_USER_DEFAULT, t, a, 256, sa);
        CHECK(x == y && !memcmp(ra, sa, sizeof ra), "GetStringTypeA(%u)", t);
    }
    {
        WORD r[16];
        uint16_t s[16];
        memset(r, 0x77, sizeof r);
        memset(s, 0x77, sizeof s);
        mark_both();
        const BOOL a = GetStringTypeW(CT_CTYPE1, L"Ab1 ", -1, r);
        const int32_t b = w32_GetStringTypeW(CT_CTYPE1, (const uint16_t*)L"Ab1 ", -1, s);
        CHECK(a == b && !memcmp(r, s, sizeof r) && GetLastError() == w32::last_error(), "GetStringTypeW(-1)");
        mark_both();
        const BOOL c = GetStringTypeW(3, L"Ab", 2, r);
        const int32_t d = w32_GetStringTypeW(3, (const uint16_t*)L"Ab", 2, s);
        CHECK(c == d && GetLastError() == w32::last_error(), "GetStringTypeW(bad type): %d err %lu vs %d err %u", c,
              GetLastError(), d, w32::last_error());
    }
}

static void test_locale_info() {
    g_section = "GetLocaleInfoA";
    for (uint32_t lcid : {0x400u, 0x800u, 0x409u}) {
        for (uint32_t t = 1; t < 0x1100; t++) {
            for (uint32_t extra : {0u, 0x80000000u}) {
                char r[300], s[300];
                memset(r, 0x5a, sizeof r);
                memset(s, 0x5a, sizeof s);
                mark_both();
                const int a = GetLocaleInfoA(lcid, t | extra, r, sizeof r);
                const DWORD ae = GetLastError();
                const int b = w32_GetLocaleInfoA(lcid, t | extra, s, sizeof s);
                CHECK(a == b && ae == w32::last_error() && !memcmp(r, s, sizeof r),
                      "GetLocaleInfoA(%x, %x): %d err %lu \"%s\" vs %d err %u \"%s\"", lcid, t | extra, a, ae,
                      a ? r : "", b, w32::last_error(), b ? s : "");
            }
        }
    }
    for (uint32_t t : {0x4u, 0xdu, 0xeu, 0x16u, 0x19u, 0x1bu, 0x1cu}) {
        for (int n : {0, 1, 2, 4}) {
            char r[16], s[16];
            memset(r, 0x5a, sizeof r);
            memset(s, 0x5a, sizeof s);
            mark_both();
            const int a = GetLocaleInfoA(0x400, t, n ? r : 0, n);
            const DWORD ae = GetLastError();
            const int b = w32_GetLocaleInfoA(0x400, t, n ? s : 0, n);
            CHECK(a == b && ae == w32::last_error() && !memcmp(r, s, sizeof r), "GetLocaleInfoA(%x, size %d): %d err %lu vs %d err %u",
                  t, n, a, ae, b, w32::last_error());
            memset(r, 0x5a, sizeof r);
            memset(s, 0x5a, sizeof s);
            mark_both();
            const int c = GetLocaleInfoA(0x400, t | LOCALE_RETURN_NUMBER, n ? r : 0, n);
            const DWORD ce = GetLastError();
            const int d = w32_GetLocaleInfoA(0x400, t | LOCALE_RETURN_NUMBER, n ? s : 0, n);
            CHECK(c == d && ce == w32::last_error() && !memcmp(r, s, sizeof r),
                  "GetLocaleInfoA(%x | RETURN_NUMBER, size %d): %d err %lu vs %d err %u", t, n, c, ce, d, w32::last_error());
        }
    }
}

static void currency_case(const char* v, uint32_t flags = 0, const CURRENCYFMTA* fmt = 0, int size = 0x40) {
    char r[0x80], s[0x80];
    memset(r, 0x5a, sizeof r);
    memset(s, 0x5a, sizeof s);
    mark_both();
    const int a = GetCurrencyFormatA(0x400, flags, v, fmt, size ? r : 0, size);
    const DWORD ae = GetLastError();
    const int b = w32_GetCurrencyFormatA(0x400, flags, v, fmt, size ? s : 0, size);
    CHECK(a == b && ae == w32::last_error() && !memcmp(r, s, sizeof r),
          "GetCurrencyFormatA(\"%s\", %x, fmt %d, size %d): %d err %lu \"%s\" vs %d err %u \"%s\"", v ? v : "(null)", flags,
          fmt != 0, size, a, ae, a && size ? r : "", b, w32::last_error(), b && size ? s : "");
}
static void test_currency() {
    g_section = "GetCurrencyFormatA";
    // LocaleMoney's inputs: "%d" of an amount, "%d.%d" of amount / 100 and its remainder
    char v[64];
    std::vector<int> amounts = {0, 1, 5, 9, 10, 99, 100, 101, 999, 1000, 1001, 9999, 10000, 12345, 99999, 100000,
                                999999, 1000000, 1234567, 99999999, 2147483647, -1, -5, -99, -100, -1000, -123456,
                                -2147483647, (int)0x80000000};
    for (int i = 0; i < 3000; i++) amounts.push_back((int)((uint32_t)i * 2654435761u) >> (i % 31));
    for (int a : amounts) {
        sprintf(v, "%d", a);
        currency_case(v);
        const uint32_t sgn = (uint32_t)(a >> 31);
        const int mag = (int)(((uint32_t)a ^ sgn) - sgn);
        sprintf(v, "%d.%d", a / 100, mag % 100);
        currency_case(v);
    }
    for (const char* e : {"", "-", ".", ".5", "5.", "007", "-0", "-0.001", "-0.005", "1.005", "1.004", "9.995", "0.995",
                          "999999.999", "1e5", " 5", "+5", "5-", "1..2", "1.2.3", "-.5", "00.00", "12345678901234567890",
                          "0.1234567890123", "1,000", "--5", "5 "})
        currency_case(e);
    currency_case(0);
    currency_case("1234", 0, 0, 0);
    currency_case("1234", 0, 0, 5);
    currency_case("1234", 0, 0, 10);
    currency_case("1234", 0, 0, 9);
    currency_case("1234", LOCALE_NOUSEROVERRIDE);
    currency_case("1234", 1);
    for (UINT neg = 0; neg < 17; neg++) {
        for (UINT pos = 0; pos < 5; pos++) {
            for (UINT grouping : {0u, 3u, 32u, 30u, 320u}) {
                CURRENCYFMTA f = {2, 1, grouping, (LPSTR) ",", (LPSTR) " ", neg, pos, (LPSTR) "EUR"};
                currency_case("-1234567.891", 0, &f);
                currency_case("1234567.8", 0, &f);
                if (neg == 0 && pos == 0) {
                    f.NumDigits = 0;
                    currency_case("-0.5", 0, &f);
                    f.LeadingZero = 0;
                    f.NumDigits = 3;
                    currency_case("0.5", 0, &f);
                    currency_case("0.5", LOCALE_NOUSEROVERRIDE, &f);
                }
            }
        }
    }
}

static void date_case(const SYSTEMTIME* st, uint32_t flags, const char* fmt, int size = 0x80) {
    char r[0x100], s[0x100];
    memset(r, 0x5a, sizeof r);
    memset(s, 0x5a, sizeof s);
    mark_both();
    const int a = GetDateFormatA(0x400, flags, st, fmt, size ? r : 0, size);
    const DWORD ae = GetLastError();
    const int b = w32_GetDateFormatA(0x400, flags, (const W32SystemTime*)st, fmt, size ? s : 0, size);
    CHECK(a == b && ae == w32::last_error() && !memcmp(r, s, sizeof r),
          "GetDateFormatA(%d-%d-%d %d:%d:%d.%d dow %d, %x, \"%s\", %d): %d err %lu \"%s\" vs %d err %u \"%s\"",
          st ? st->wYear : 0, st ? st->wMonth : 0, st ? st->wDay : 0, st ? st->wHour : 0, st ? st->wMinute : 0,
          st ? st->wSecond : 0, st ? st->wMilliseconds : 0, st ? st->wDayOfWeek : 0, flags, fmt ? fmt : "(null)", size,
          a, ae, a && size ? r : "", b, w32::last_error(), b && size ? s : "");
}
static void test_dates() {
    g_section = "GetDateFormatA";
    // LocaleFormatShortDate: every day 1990-2030, as the game passes it (time and day of week 0)
    for (int y = 1990; y <= 2030; y++)
        for (int m = 1; m <= 12; m++)
            for (int d = 1; d <= 31; d++) {
                SYSTEMTIME st = {};
                st.wYear = (WORD)y;
                st.wMonth = (WORD)m;
                st.wDay = (WORD)d;
                date_case(&st, DATE_SHORTDATE, 0);
            }
    const WORD bad[][8] = {{2001, 0, 0, 1}, {2001, 13, 0, 1}, {2001, 2, 0, 0}, {2001, 2, 0, 29}, {2000, 2, 0, 29},
                           {1900, 2, 0, 29}, {1600, 1, 0, 1}, {1601, 1, 0, 1}, {30827, 12, 0, 31}, {30828, 1, 0, 1},
                           {9999, 12, 0, 31}, {2001, 1, 0, 1, 24}, {2001, 1, 0, 1, 23, 60}, {2001, 1, 0, 1, 0, 0, 60},
                           {2001, 1, 0, 1, 0, 0, 0, 1000}, {2001, 1, 9, 1}, {2001, 1, 3, 1}, {65535, 65535, 65535, 65535}};
    for (const auto& b : bad) {
        SYSTEMTIME st;
        memcpy(&st, b, sizeof st);
        date_case(&st, DATE_SHORTDATE, 0);
        date_case(&st, DATE_LONGDATE, 0);
    }
    SYSTEMTIME st = {};
    st.wYear = 1998;
    st.wMonth = 9;
    st.wDay = 6;
    for (const char* f : {"dddd, MMMM d, yyyy", "ddd MMM yy", "d dd ddd dddd ddddd", "M MM MMM MMMM MMMMM",
                          "y yy yyy yyyy yyyyy yyyyyy", "g gg ggg", "'quoted''s' d 'x", "''", "d/M/y h:mm:ss tt",
                          "[%s] \\ \"", "", "MMMMd", "dMMMM", "d MMMM", "MMMM d"})
        date_case(&st, 0, f);
    for (uint32_t fl : {0u, 1u, 2u, 3u, 4u, 8u, 0x10u, 0x20u, 0x40u, 0x80u, 0x100u, 0x80000001u, 0x80000000u})
        date_case(&st, fl, 0);
    date_case(&st, 1, "d");
    date_case(&st, 0x80000000u, "d");
    date_case(&st, 1, 0, 0);
    date_case(&st, 1, 0, 8);
    date_case(&st, 1, 0, 9);
    date_case(0, 1, 0);
    st.wYear = 10000;
    date_case(&st, 0, "yyyy yy y");
    st.wYear = 1605;
    date_case(&st, 0, "yyyy yy y yyyyy");
}

// ==== exceptions: Windows' own, behind jumps =================================================================================
static void* g_addr;
static DWORD g_esp, g_frame;
static LONG CALLBACK veh(EXCEPTION_POINTERS* p) {
    if (p->ExceptionRecord->ExceptionCode != 0xe0001234) return EXCEPTION_CONTINUE_SEARCH;
    g_addr = p->ExceptionRecord->ExceptionAddress;
    g_esp = p->ContextRecord->Esp;
    return EXCEPTION_CONTINUE_EXECUTION;
}
typedef void(W32K_CALL* Raise_t)(uint32_t, uint32_t, uint32_t, const uint32_t*);
static Raise_t volatile g_raise;                 // (read at the call: one function, one frame, either target)
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void raise_via() {
    volatile uint32_t args[2] = {1, 2};          // (the caller's frame: esp is compared relative to it)
    g_frame = (DWORD)(uintptr_t)&args[0];
    g_raise(0xe0001234, 0, 2, (const uint32_t*)args);
}
static void test_seh() {
    g_section = "exceptions";
    PVOID h = AddVectoredExceptionHandler(1, veh);
    g_raise = (Raise_t)(void*)&RaiseException;
    raise_via();
    void* ra = g_addr;
    const DWORD re = g_frame - g_esp;
    g_addr = 0;
    g_raise = &w32_RaiseException;
    raise_via();
    g_esp = g_frame - g_esp;
    CHECK(g_addr == ra && g_esp == re, "RaiseException: what Windows captures (address %p, esp frame-%lx vs %p, frame-%lx)", ra,
          re, g_addr, g_esp);
    RemoveVectoredExceptionHandler(h);
    LPTOP_LEVEL_EXCEPTION_FILTER was = SetUnhandledExceptionFilter(0);
    void* got = w32_SetUnhandledExceptionFilter((void*)was);
    CHECK(got == 0 && SetUnhandledExceptionFilter(was) == was, "SetUnhandledExceptionFilter: Windows' own");
}

int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("w32_kernel_test (%s)\n",
#if defined(_MSC_VER)
           "MSVC"
#else
           "GCC"
#endif
    );
    struct { const char* name; void (*fn)(); } tests[] = {
        {"heap", test_heap}, {"system", test_system}, {"threads", test_threads},
        {"critical sections", test_critical_sections}, {"events", test_events}, {"time", test_time},
        {"process", test_process}, {"code pages", test_code_pages}, {"strings", test_strings},
        {"locale info", test_locale_info}, {"currency", test_currency}, {"dates", test_dates}, {"seh", test_seh},
    };
    for (auto& t : tests) {
        const int f0 = g_fails, c0 = g_checks;
        t.fn();
        printf("%-18s %6d checks, %d failed\n", t.name, g_checks - c0, g_fails - f0);
    }
    printf("w32_kernel_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails;
}
