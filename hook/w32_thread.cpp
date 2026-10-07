// w32_thread.cpp -- the thread, synchronisation and process stand-ins (w32_kernel.h).
//
// Objects (w32_handle.h): a thread, an event, a semaphore -- standard C++ (std::mutex / std::condition_variable), the
// same code on Windows and Linux. What only the OS can do sits in the #ifdef _WIN32 / #else parts: starting a thread,
// stopping one in its tracks (SuspendThread), its priority and affinity, the current thread id, Sleep, a name that
// other processes see (the game's single-instance semaphore), ending the process.
//
// What the game does with them (krn_core.cpp, krn_file.cpp, net_line.cpp):
//   TaskCreate / TaskSetTimer: CreateThread(0, 0, vector, fn, 0, &id) -- the id is how current_taskid finds a task, so
//   a thread's id is GetCurrentThreadId's answer on it. TaskSuspendMe: the thread suspends ITSELF (SuspendThread on
//   its own handle); TaskResume: ResumeThread from another thread, which must say 1. session.cpp suspends another
//   thread for a moment to read its count. TaskIsDead: GetExitCodeThread (259 while it runs); TaskDestroy:
//   WaitForSingleObject(thread, INFINITE). TaskSetPriority: SetThreadPriority(GetCurrentThread(), 15).
//   sync.obj: 16 CRITICAL_SECTIONs (the game's cs_tags: 0x18 bytes each, a flag after); SyncStrobe: an auto-reset
//   event, PulseEvent from the physics thread, WaitForSingleObject(.., 15000) on the main thread -- 1008 pulses a run.
//   start_unique_instance: CreateSemaphoreA(0, 0, 1, "MGI Viper Racing 1998") and ERROR_ALREADY_EXISTS when another
//   copy of the game runs (another process: the name is the OS's too, below).
//
// PulseEvent, exactly as Windows: the threads waiting at that moment are released -- for an auto-reset event one of
// them (the longest waiting), for a manual-reset event all -- and the event is left non-signalled; with no one
// waiting it only resets. A thread that starts waiting after the pulse waits for the next. (Windows' documented
// caveat -- a waiter taken off its wait by a kernel APC misses the pulse -- has no counterpart here: no APCs.)
//
// Threads start with the x87 control word Windows gives every new thread, 0x27f (64-bit precision, round to nearest,
// all exceptions masked), whatever the creating thread's is.
#include "w32_kernel.h"
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <algorithm>
#include <vector>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include "w32_seh.h"
#endif

using w32::Handle;

namespace {

const uint32_t STILL_ACTIVE_ = 259;
const uint32_t CREATE_SUSPENDED_ = 4;
const uint32_t ERR_TOO_MANY_POSTS = 298;

typedef std::chrono::steady_clock Clock;

// wait on cv (holding lk) until pred, for ms (INFINITE_: for ever); false on a timeout
template <class Pred> bool wait_ms(std::condition_variable& cv, std::unique_lock<std::mutex>& lk, uint32_t ms, Pred pred) {
    if (ms == w32::INFINITE_) {
        cv.wait(lk, pred);
        return true;
    }
    return cv.wait_until(lk, Clock::now() + std::chrono::milliseconds(ms), pred);
}

void set_x87_default() {
    uint16_t cw = 0x27f;
#if defined(_MSC_VER)
    __asm fldcw cw
#else
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
#endif
}

// ---- threads --------------------------------------------------------------------------------------------------------
struct Thread : w32::Object {
    Thread() : w32::Object(w32::Kind::Thread) {}
    ~Thread();
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    uint32_t exit_code = STILL_ACTIVE_;
    uint32_t tid = 0;
    W32ThreadProc start = 0;
    void* param = 0;
#ifdef _WIN32
    HANDLE os = 0;
#else
    pthread_t pt;
    bool started = false;
    std::atomic<int> suspend_count{0};
    std::atomic<bool> exiting{false};
    std::mutex smu;                              // serialises other threads' SuspendThread / ResumeThread on it
    sem_t ack;                                   // the target's "I'm stopped", from its signal handler
    jmp_buf exit_to;                             // ExitThread on it: back to the start routine's frame
#endif
    uint32_t wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lk(mu);
        return wait_ms(cv, lk, ms, [this] { return done; }) ? w32::WAIT_OBJECT_0_ : w32::WAIT_TIMEOUT_;
    }
};

thread_local Thread* t_self;                     // the stand-in thread this code runs on (null: main, or not ours)
thread_local std::shared_ptr<Thread>* t_hold;    // its own reference, dropped when it ends

// the thread's function has returned (or it called ExitThread): its exit code, its handle signalled
void finish(uint32_t code) {
    Thread* t = t_self;
    {
        std::lock_guard<std::mutex> lk(t->mu);
        t->exit_code = code;
        t->done = true;
    }
    t->cv.notify_all();
    t_self = 0;
    std::shared_ptr<Thread>* hold = t_hold;
    t_hold = 0;
    delete hold;                                 // (the last reference if every handle is closed)
}

#ifdef _WIN32
Thread::~Thread() {
    if (os) CloseHandle(os);
}
DWORD WINAPI os_entry(void* p) {
    std::shared_ptr<Thread>* hold = (std::shared_ptr<Thread>*)p;
    Thread* t = hold->get();
    t_self = t;
    t_hold = hold;
    set_x87_default();                           // (Windows has: 0x27f)
    const uint32_t code = t->start(t->param);
    finish(code);
    return code;
}
#else
// ---- Linux: ids, suspension by signal, exit by longjmp -----------------------------------------------------------
// A thread id is the stand-ins' own, from a counter (multiples of 4 from 0x1004, as Windows' ids look), given to a
// thread the first time it asks -- or, for a CreateThread thread, before it starts (CreateThread reports it). It is a
// thread_local: GetCurrentThreadId is one TLS load (366k calls in 7 runs).
std::atomic<uint32_t> g_next_tid{0x1004};
thread_local uint32_t t_tid;
uint32_t new_tid() { return g_next_tid.fetch_add(4); }

// SuspendThread of another thread: SIG_SUSPEND to it; its handler says so (sem_post: async-signal-safe) and waits in
// sigsuspend until its suspend count is 0 again -- ResumeThread's SIG_RESUME wakes it to look. A thread suspending
// itself waits the same way, without the signal. SIG_RESUME is blocked on every stand-in thread except inside that
// sigsuspend, so a resume that comes between the count check and the wait is never lost.
int sig_suspend() { return SIGRTMIN + 6; }
int sig_resume() { return SIGRTMIN + 7; }
void wait_while_suspended(Thread* t) {
    sigset_t m;
    pthread_sigmask(SIG_SETMASK, 0, &m);
    sigdelset(&m, sig_resume());
    while (t->suspend_count.load() > 0) sigsuspend(&m);
}
void on_suspend(int) {
    const int saved = errno;
    Thread* t = t_self;
    if (t) {
        sem_post(&t->ack);
        wait_while_suspended(t);
    }
    errno = saved;
}
void on_resume(int) {}
void install_signals() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_flags = SA_RESTART;
        sigfillset(&sa.sa_mask);
        sa.sa_handler = on_suspend;
        sigaction(sig_suspend(), &sa, 0);
        sa.sa_handler = on_resume;
        sigaction(sig_resume(), &sa, 0);
    });
}
Thread::~Thread() {
    if (started) {
        if (pthread_equal(pt, pthread_self())) pthread_detach(pt);   // (its own last reference, at its end)
        else pthread_join(pt, 0);
    }
    sem_destroy(&ack);
}
// the stand-in threads alive now (ExitProcess stops them first, as Windows does)
std::mutex g_live_mu;
std::vector<Thread*> g_live;

void* posix_entry(void* p) {
    std::shared_ptr<Thread>* hold = (std::shared_ptr<Thread>*)p;
    Thread* t = hold->get();
    t_self = t;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        g_live.push_back(t);
    }
    t_hold = hold;
    t_tid = t->tid;
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, sig_resume());
    pthread_sigmask(SIG_BLOCK, &m, 0);
    sigemptyset(&m);
    sigaddset(&m, sig_suspend());
    pthread_sigmask(SIG_UNBLOCK, &m, 0);
    set_x87_default();                           // (a Linux thread inherits its creator's; Windows' start at 0x27f)
    w32_seh_thread_begin();                      // its TIB behind fs, its signal stack (w32_seh.cpp: SEH on Linux)
    wait_while_suspended(t);                     // CREATE_SUSPENDED
    uint32_t code;
    if (setjmp(t->exit_to)) {
        code = t->exit_code;                     // (ExitThread put it there)
    } else {
        code = t->start(t->param);
    }
    t->exiting = true;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        g_live.erase(std::remove(g_live.begin(), g_live.end(), t), g_live.end());
    }
    finish(code);
    return 0;
}
#endif

// ---- critical sections ------------------------------------------------------------------------------------------------
// The CRITICAL_SECTION's own bytes hold Windows' view (owner, recursion); the lock itself is an object its
// LockSemaphore field points to. Leave by a thread that doesn't own it does what Windows does: counts down anyway.
struct CsLock {
    std::mutex mu;
    std::condition_variable cv;
    uint32_t owner = 0;
    int32_t depth = 0;
};
CsLock* cs_lock(W32CriticalSection* cs) {
    std::atomic<uint32_t>* slot = (std::atomic<uint32_t>*)&cs->lock_semaphore;
    uint32_t v = slot->load(std::memory_order_acquire);
    if (v) return (CsLock*)(uintptr_t)v;
    CsLock* l = new CsLock;                      // (a section used without InitializeCriticalSection: made now)
    uint32_t none = 0;
    if (slot->compare_exchange_strong(none, (uint32_t)(uintptr_t)l)) return l;
    delete l;
    return (CsLock*)(uintptr_t)none;
}

// ---- events and semaphores -----------------------------------------------------------------------------------------
struct NameClaim {                               // a name other processes see (see claim_name)
#ifdef _WIN32
    HANDLE h = 0;
    ~NameClaim() { if (h) CloseHandle(h); }
#else
    int fd = -1;
    ~NameClaim() { if (fd >= 0) close(fd); }
#endif
};

struct Event : w32::Object {
    Event(bool m, bool s) : w32::Object(w32::Kind::Event), manual(m), signaled(s) {}
    std::mutex mu;
    std::condition_variable cv;
    const bool manual;
    bool signaled;
    struct Waiter { bool woken = false; };
    std::deque<Waiter*> waiting;                 // in the order they began to wait (Windows releases FIFO)
    NameClaim claim;
    uint32_t wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lk(mu);
        if (signaled) {
            if (!manual) signaled = false;
            return w32::WAIT_OBJECT_0_;
        }
        if (ms == 0) return w32::WAIT_TIMEOUT_;
        Waiter w;
        waiting.push_back(&w);
        if (wait_ms(cv, lk, ms, [&w] { return w.woken; })) return w32::WAIT_OBJECT_0_;
        for (auto i = waiting.begin(); i != waiting.end(); ++i)
            if (*i == &w) { waiting.erase(i); break; }
        return w32::WAIT_TIMEOUT_;
    }
    // (holding mu) release the first waiter, or all
    bool release_one() {
        if (waiting.empty()) return false;
        waiting.front()->woken = true;
        waiting.pop_front();
        return true;
    }
    void release_all() {
        for (Waiter* w : waiting) w->woken = true;
        waiting.clear();
    }
    void set() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (manual) {
                signaled = true;
                release_all();
            } else if (!release_one()) {
                signaled = true;
            }
        }
        cv.notify_all();
    }
    void pulse() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (manual) release_all();
            else release_one();
            signaled = false;
        }
        cv.notify_all();
    }
    void reset() {
        std::lock_guard<std::mutex> lk(mu);
        signaled = false;
    }
};

struct Semaphore : w32::Object {
    Semaphore(int32_t c, int32_t m) : w32::Object(w32::Kind::Semaphore), count(c), maximum(m) {}
    std::mutex mu;
    std::condition_variable cv;
    int32_t count;
    const int32_t maximum;
    NameClaim claim;
    uint32_t wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lk(mu);
        if (!wait_ms(cv, lk, ms, [this] { return count > 0; })) return w32::WAIT_TIMEOUT_;
        count--;
        return w32::WAIT_OBJECT_0_;
    }
};

// Named objects: one per name in this process (a second Create gets another handle to it, ERROR_ALREADY_EXISTS; a
// name taken by another kind fails, ERROR_INVALID_HANDLE, as Windows'). The name is also claimed from the OS, so
// another process -- a second copy of the game -- finds it taken: on Windows a real named object of the same kind
// held open beside the stand-in's (its state stays the stand-in's: the game only asks whether the name was free); on
// Linux a lock file in $XDG_RUNTIME_DIR (else /tmp) held with flock -- released by the OS when the process ends, as a
// Windows handle is. (Two processes creating the same name in the same instant can both see it free on Linux: an
// exclusive lock is taken, then made shared.)
std::mutex g_names_mu;
std::map<std::string, std::weak_ptr<w32::Object>> g_names;

#ifdef _WIN32
// false: the name can't be had (last error set); *existed: someone had it
bool claim_name(w32::Kind k, const char* name, bool manual, bool initial, int32_t count, int32_t maximum,
                NameClaim* c, bool* existed) {
    c->h = k == w32::Kind::Event ? CreateEventA(0, manual, initial, name) : CreateSemaphoreA(0, count, maximum, name);
    const DWORD e = GetLastError();
    if (!c->h) {
        w32::set_last_error(e);
        return false;
    }
    *existed = e == ERROR_ALREADY_EXISTS;
    return true;
}
#else
bool claim_name(w32::Kind, const char* name, bool, bool, int32_t, int32_t, NameClaim* c, bool* existed) {
    const char* dir = getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(dir && *dir ? dir : "/tmp") + "/viperport-name-";
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char* p = (const unsigned char*)name; *p && path.size() < 240; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) path += (char)*p;
        else { path += '_'; path += hex[*p >> 4]; path += hex[*p & 15]; }
    }
    c->fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (c->fd < 0) {
        *existed = false;                        // (no lock file: the name is this process's alone)
        return true;
    }
    if (flock(c->fd, LOCK_EX | LOCK_NB) == 0) {
        *existed = false;
        flock(c->fd, LOCK_SH);
    } else {
        *existed = true;
        flock(c->fd, LOCK_SH | LOCK_NB);
    }
    return true;
}
#endif

// CreateEventA / CreateSemaphoreA: make(), or the named object that exists. Last error: ERROR_ALREADY_EXISTS or 0.
template <class T, class Make>
Handle create_named(w32::Kind k, const char* name, bool manual, bool initial, int32_t count, int32_t maximum, Make make) {
    if (!name) {
        std::shared_ptr<T> o = make();
        Handle h = w32::add(o);
        w32::set_last_error(h ? 0u : (uint32_t)w32::ERR_NOT_ENOUGH_MEMORY);
        return h;
    }
    std::lock_guard<std::mutex> lk(g_names_mu);
    std::shared_ptr<w32::Object> have = g_names[name].lock();
    if (have) {
        if (have->kind != k) {
            w32::set_last_error(w32::ERR_INVALID_HANDLE);
            return 0;
        }
        Handle h = w32::add(have);
        w32::set_last_error(h ? w32::ERR_ALREADY_EXISTS : w32::ERR_NOT_ENOUGH_MEMORY);
        return h;
    }
    std::shared_ptr<T> o = make();
    bool existed = false;
    if (!claim_name(k, name, manual, initial, count, maximum, &o->claim, &existed)) return 0;
    Handle h = w32::add(o);
    if (!h) {
        w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    g_names[name] = o;
    w32::set_last_error(existed ? (uint32_t)w32::ERR_ALREADY_EXISTS : 0u);
    return h;
}

}  // namespace

// ==== threads ==========================================================================================================
uint32_t W32K_CALL w32_CreateThread(void* security, uint32_t stack_size, W32ThreadProc start, void* param,
                                    uint32_t flags, uint32_t* thread_id) {
    (void)security;
    std::shared_ptr<Thread> t = std::make_shared<Thread>();
    t->start = start;
    t->param = param;
    const Handle h = w32::add(t);
    if (!h) {
        w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    std::shared_ptr<Thread>* hold = new std::shared_ptr<Thread>(t);
#ifdef _WIN32
    DWORD tid = 0;
    HANDLE os = CreateThread(0, stack_size, os_entry, hold, flags, &tid);
    if (!os) {
        const DWORD e = GetLastError();
        delete hold;
        w32::close(h);
        w32::set_last_error(e);
        return 0;
    }
    t->os = os;
    t->tid = tid;
#else
    sem_init(&t->ack, 0, 0);
    install_signals();
    t->tid = new_tid();
    if (flags & CREATE_SUSPENDED_) t->suspend_count = 1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    // the default stack is race.exe's (and the loader's) reserve, 1 MB; asked for: rounded up to whole pages
    size_t n = stack_size ? ((size_t)stack_size + 0xfff) & ~(size_t)0xfff : 0x100000;
    if (n < (size_t)PTHREAD_STACK_MIN) n = PTHREAD_STACK_MIN;
    pthread_attr_setstacksize(&a, n);
    const int r = pthread_create(&t->pt, &a, posix_entry, hold);
    pthread_attr_destroy(&a);
    if (r) {
        delete hold;
        w32::close(h);
        w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    t->started = true;
    const uint32_t tid = t->tid;
#endif
    if (thread_id) *thread_id = tid;             // (after the thread exists, as Windows: it may already be running)
    return h;
}

void W32K_CALL w32_ExitThread(uint32_t code) {
    Thread* t = t_self;
#ifdef _WIN32
    if (t) finish(code);
    ExitThread(code);
#else
    if (t) {
        t->exit_code = code;
        longjmp(t->exit_to, 1);                  // to posix_entry, which finishes it (see the R2b notes)
    }
    pthread_exit(0);
#endif
}

int32_t W32K_CALL w32_GetExitCodeThread(uint32_t h, uint32_t* code) {
    if (h == w32::CURRENT_THREAD) {
        *code = STILL_ACTIVE_;
        return 1;
    }
    std::shared_ptr<Thread> t = w32::get_as<Thread>(h, w32::Kind::Thread);
    if (!t) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    std::lock_guard<std::mutex> lk(t->mu);
    *code = t->exit_code;
    return 1;
}

uint32_t W32K_CALL w32_SuspendThread(uint32_t h) {
#ifdef _WIN32
    HANDLE os;
    std::shared_ptr<Thread> t;
    if (h == w32::CURRENT_THREAD) {
        os = GetCurrentThread();
    } else {
        t = w32::get_as<Thread>(h, w32::Kind::Thread);
        if (!t) {
            w32::set_last_error(w32::ERR_INVALID_HANDLE);
            return 0xffffffffu;
        }
        os = t->os;
    }
    const DWORD r = SuspendThread(os);
    if (r == (DWORD)-1) w32::set_last_error(GetLastError());
    return r;
#else
    std::shared_ptr<Thread> t;
    Thread* tp = h == w32::CURRENT_THREAD ? t_self : 0;
    if (!tp) {
        t = w32::get_as<Thread>(h, w32::Kind::Thread);
        tp = t.get();
    }
    if (!tp) {                                   // (or the pseudo-handle on a thread that isn't a stand-in's)
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0xffffffffu;
    }
    if (tp == t_self) {                          // the game's way: a thread suspends itself
        const int prev = tp->suspend_count.fetch_add(1);
        wait_while_suspended(tp);
        return (uint32_t)prev;
    }
    std::lock_guard<std::mutex> lk(tp->smu);
    if (tp->exiting) {                           // (Windows: an ended thread can't be suspended)
        w32::set_last_error(w32::ERR_ACCESS_DENIED);
        return 0xffffffffu;
    }
    const int prev = tp->suspend_count.fetch_add(1);
    if (prev == 0 && pthread_kill(tp->pt, sig_suspend()) == 0) {
        for (;;) {                               // until it says it's stopped (or it's ending: signals blocked)
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            if (sem_timedwait(&tp->ack, &ts) == 0 || tp->exiting) break;
        }
    }
    return (uint32_t)prev;
#endif
}

uint32_t W32K_CALL w32_ResumeThread(uint32_t h) {
#ifdef _WIN32
    HANDLE os;
    std::shared_ptr<Thread> t;
    if (h == w32::CURRENT_THREAD) {
        os = GetCurrentThread();
    } else {
        t = w32::get_as<Thread>(h, w32::Kind::Thread);
        if (!t) {
            w32::set_last_error(w32::ERR_INVALID_HANDLE);
            return 0xffffffffu;
        }
        os = t->os;
    }
    const DWORD r = ResumeThread(os);
    if (r == (DWORD)-1) w32::set_last_error(GetLastError());
    return r;
#else
    std::shared_ptr<Thread> t;
    Thread* tp = h == w32::CURRENT_THREAD ? t_self : 0;
    if (!tp) {
        t = w32::get_as<Thread>(h, w32::Kind::Thread);
        tp = t.get();
    }
    if (!tp) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0xffffffffu;
    }
    std::lock_guard<std::mutex> lk(tp->smu);
    int prev = tp->suspend_count.load();
    while (prev > 0 && !tp->suspend_count.compare_exchange_weak(prev, prev - 1)) {}
    if (prev == 1 && !tp->exiting) pthread_kill(tp->pt, sig_resume());
    return (uint32_t)(prev > 0 ? prev : 0);
#endif
}

int32_t W32K_CALL w32_SetThreadPriority(uint32_t h, int32_t priority) {
    std::shared_ptr<Thread> t;
    if (h != w32::CURRENT_THREAD) {
        t = w32::get_as<Thread>(h, w32::Kind::Thread);
        if (!t) {
            w32::set_last_error(w32::ERR_INVALID_HANDLE);
            return 0;
        }
    }
#ifdef _WIN32
    if (!SetThreadPriority(t ? t->os : GetCurrentThread(), priority)) {
        w32::set_last_error(GetLastError());
        return 0;
    }
    return 1;
#else
    // Windows' levels: idle -15, -2..2, time-critical 15. Linux: accepted, not applied (an unprivileged process can't
    // raise a thread's priority; the game's timer threads pace themselves with Sleep)
    if (!(priority == -15 || priority == 15 || (priority >= -2 && priority <= 2))) {
        w32::set_last_error(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
#endif
}

uint32_t W32K_CALL w32_GetCurrentThread() { return w32::CURRENT_THREAD; }
uint32_t W32K_CALL w32_GetCurrentProcess() { return w32::INVALID; }

uint32_t W32K_CALL w32_GetCurrentThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();                 // (the port's own GetCurrentThreadId calls agree with the game's)
#else
    uint32_t id = t_tid;
    if (!id) t_tid = id = new_tid();
    return id;
#endif
}

// (GetLastError / SetLastError: agent A's, in w32_file.cpp -- w32::last_error)

void W32K_CALL w32_ExitProcess(uint32_t code) {
#ifdef _WIN32
    ExitProcess(code);
#else
    // Windows ends every other thread before the process's own exit work runs; exit() alone would run the port's exit
    // logs and the static destructors with the game's threads (physics, timer, sound manager) still going -- one of them
    // then hit a panic after the log had ended and faulted in the last-resort path (seen at quit on a real Linux PC).
    // So the stand-in threads are stopped first (the SuspendThread handshake, a bounded wait each), then exit().
    static std::atomic<bool> s_exiting{false};
    if (!s_exiting.exchange(true)) {
        std::lock_guard<std::mutex> lk(g_live_mu);
        for (Thread* tp : g_live) {
            if (tp == t_self || tp->exiting) continue;
            std::lock_guard<std::mutex> sl(tp->smu);
            if (tp->exiting || tp->suspend_count.fetch_add(1) != 0) continue;   // (already suspended: it stays so)
            if (pthread_kill(tp->pt, sig_suspend()) != 0) continue;
            for (int i = 0; i < 50; i++) {       // up to 0.5 s for it to say it's stopped
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 10 * 1000000;
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                if (sem_timedwait(&tp->ack, &ts) == 0 || tp->exiting) break;
            }
        }
    }
    exit((int)code);                             // atexit / static destructors: the port's exit logs (see R2b notes)
#endif
}

// not available: no program is ever started (Win32System's callers see the failure Windows gives a missing program)
int32_t W32K_CALL w32_CreateProcessA(const char*, char*, void*, void*, int32_t, uint32_t, void*, const char*, void*,
                                     void*) {
    w32::set_last_error(w32::ERR_FILE_NOT_FOUND);
    return 0;
}
int32_t W32K_CALL w32_GetExitCodeProcess(uint32_t h, uint32_t* code) {
    if (h == w32::INVALID) {                     // GetCurrentProcess(): this one, still running
        *code = STILL_ACTIVE_;
        return 1;
    }
    w32::set_last_error(w32::ERR_INVALID_HANDLE);
    return 0;
}

void W32K_CALL w32_Sleep(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);                                   // (Windows' timer resolution: dsound_sdl.cpp asks for 1 ms)
#else
    if (ms == 0) {
        sched_yield();
        return;
    }
    if (ms == w32::INFINITE_) {
        for (;;) pause();
    }
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L}, rem;
    while (nanosleep(&ts, &rem) != 0 && errno == EINTR) ts = rem;   // (a suspend signal doesn't cut it short)
#endif
}

// ==== critical sections ================================================================================================
void W32K_CALL w32_InitializeCriticalSection(W32CriticalSection* cs) {
    cs->debug_info = 0xffffffffu;                // as Windows 8+ leaves it (no debug info)
    cs->lock_count = -1;
    cs->recursion_count = 0;
    cs->owning_thread = 0;
    cs->lock_semaphore = (uint32_t)(uintptr_t) new CsLock;
    cs->spin_count = 0x020007d0;                 // as Windows leaves it (its default spin, flag bits)
}

void W32K_CALL w32_EnterCriticalSection(W32CriticalSection* cs) {
    CsLock* l = cs_lock(cs);
    const uint32_t me = w32_GetCurrentThreadId();
    std::unique_lock<std::mutex> lk(l->mu);
    if (l->owner != me) {
        l->cv.wait(lk, [l] { return l->owner == 0; });
        l->owner = me;
        l->depth = 0;
    }
    l->depth++;
    cs->owning_thread = me;
    cs->recursion_count = l->depth;
    cs->lock_count = -2;
}

void W32K_CALL w32_LeaveCriticalSection(W32CriticalSection* cs) {
    CsLock* l = cs_lock(cs);
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(l->mu);
        if (--l->depth <= 0) {
            l->depth = 0;
            l->owner = 0;
            cs->owning_thread = 0;
            cs->lock_count = -1;
            wake = true;
        }
        cs->recursion_count = l->depth;
    }
    if (wake) l->cv.notify_one();
}

void W32K_CALL w32_DeleteCriticalSection(W32CriticalSection* cs) {
    delete (CsLock*)(uintptr_t)cs->lock_semaphore;
    cs->lock_semaphore = 0;
    cs->debug_info = 0;                          // as Windows leaves a deleted one: all zero
    cs->lock_count = 0;
    cs->recursion_count = 0;
    cs->owning_thread = 0;
    cs->spin_count = 0;
}

// ==== events, semaphores, waits ========================================================================================
uint32_t W32K_CALL w32_CreateEventA(void* security, int32_t manual_reset, int32_t initial_state, const char* name) {
    (void)security;
    const bool m = manual_reset != 0, s = initial_state != 0;
    return create_named<Event>(w32::Kind::Event, name, m, s, 0, 0, [m, s] { return std::make_shared<Event>(m, s); });
}

int32_t W32K_CALL w32_PulseEvent(uint32_t h) {
    std::shared_ptr<Event> e = w32::get_as<Event>(h, w32::Kind::Event);
    if (!e) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    e->pulse();
    return 1;
}
int32_t W32K_CALL w32_SetEvent(uint32_t h) {
    std::shared_ptr<Event> e = w32::get_as<Event>(h, w32::Kind::Event);
    if (!e) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    e->set();
    return 1;
}
int32_t W32K_CALL w32_ResetEvent(uint32_t h) {
    std::shared_ptr<Event> e = w32::get_as<Event>(h, w32::Kind::Event);
    if (!e) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    e->reset();
    return 1;
}

uint32_t W32K_CALL w32_CreateSemaphoreA(void* security, int32_t initial, int32_t maximum, const char* name) {
    (void)security;
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        w32::set_last_error(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    return create_named<Semaphore>(w32::Kind::Semaphore, name, false, false, initial, maximum,
                                   [initial, maximum] { return std::make_shared<Semaphore>(initial, maximum); });
}

int32_t W32K_CALL w32_ReleaseSemaphore(uint32_t h, int32_t count, int32_t* previous) {
    std::shared_ptr<Semaphore> s = w32::get_as<Semaphore>(h, w32::Kind::Semaphore);
    if (!s) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    if (count <= 0) {
        w32::set_last_error(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(s->mu);
        if (count > s->maximum - s->count) {
            w32::set_last_error(ERR_TOO_MANY_POSTS);
            return 0;
        }
        if (previous) *previous = s->count;
        s->count += count;
    }
    s->cv.notify_all();
    return 1;
}

uint32_t W32K_CALL w32_WaitForSingleObject(uint32_t h, uint32_t ms) {
    if (h == w32::INVALID || h == w32::CURRENT_THREAD) {
        // this process / this thread: signalled only once it has ended -- never, from inside it
        if (ms == w32::INFINITE_) {
            for (;;) w32_Sleep(0x7fffffff);
        }
        w32_Sleep(ms);
        return w32::WAIT_TIMEOUT_;
    }
    std::shared_ptr<w32::Object> o = w32::get(h);
    if (!o) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return w32::WAIT_FAILED_;
    }
    return o->wait(ms);
}

// ==== affinity (the C runtime's FDIV check, through GetProcAddress) =====================================================
int32_t W32K_CALL w32_GetProcessAffinityMask(uint32_t process, uint32_t* process_mask, uint32_t* system_mask) {
#ifdef _WIN32
    DWORD_PTR p = 0, s = 0;
    if (!GetProcessAffinityMask((HANDLE)(intptr_t)(int32_t)process, &p, &s)) {
        w32::set_last_error(GetLastError());
        return 0;
    }
    *process_mask = (uint32_t)p;
    *system_mask = (uint32_t)s;
    return 1;
#else
    if (process != w32::INVALID) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    // the process's mask: the first thread's to ask, as it was then (sched_getaffinity is per thread, and
    // SetThreadAffinityMask changes the caller's)
    static uint32_t mask;
    static std::once_flag once;
    std::call_once(once, [] {
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof set, &set) == 0) {
            for (int i = 0; i < 32; i++)
                if (CPU_ISSET(i, &set)) mask |= 1u << i;
        }
    });
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    *system_mask = n >= 32 ? 0xffffffffu : (1u << n) - 1;
    *process_mask = mask ? mask : *system_mask;
    return 1;
#endif
}

uint32_t W32K_CALL w32_SetThreadAffinityMask(uint32_t thread, uint32_t mask) {
    std::shared_ptr<Thread> t;
    if (thread != w32::CURRENT_THREAD) {
        t = w32::get_as<Thread>(thread, w32::Kind::Thread);
        if (!t) {
            w32::set_last_error(w32::ERR_INVALID_HANDLE);
            return 0;
        }
    }
#ifdef _WIN32
    const DWORD_PTR r = SetThreadAffinityMask(t ? t->os : GetCurrentThread(), mask);
    if (!r) w32::set_last_error(GetLastError());
    return (uint32_t)r;
#else
    uint32_t pm = 0, sm = 0;
    w32_GetProcessAffinityMask(w32::INVALID, &pm, &sm);
    if (!mask || (mask & ~pm)) {
        w32::set_last_error(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    static thread_local uint32_t t_mask;         // this thread's mask as last set (the process's at first)
    const uint32_t was = t_mask ? t_mask : pm;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 32; i++)
        if (mask & (1u << i)) CPU_SET(i, &set);
    pthread_setaffinity_np(t ? t->pt : pthread_self(), sizeof set, &set);
    if (!t) t_mask = mask;
    return was;
#endif
}
