// vp_seh.h -- a guarded call for the harnesses that also builds with GCC (relink stage R1).
//
// The harnesses catch a faulting original or rewrite with MSVC's __try / __except. GCC has no __try, so a guarded
// call goes through vp_guard(): run fn(arg) and return 0, or the exception code it raised (an access violation, an
// unmasked float exception, a divide by zero, ...) with the stack unwound to the guard. Under MSVC it is __try /
// __except, as before; under GCC a vectored exception handler (last in the chain, so a harness's own vectored
// handlers still run before it, as before any __except) long-jumps back to the guard. vp_guard_f / vp_try(body,
// filter) take the __except filter too (run at the fault, before the unwind; EXCEPTION_CONTINUE_SEARCH asks the next
// guard out). (A frame-based SEH handler inside the guarded code -- an fs:[0] frame the code installs itself -- would
// run after this one, not before as under MSVC.)
// The guard is per thread and nests (an inner guard handles its own faults); exceptions outside any guard go on to
// the other handlers as before. The filter (MSVC's __except expression) is EXCEPTION_EXECUTE_HANDLER for every code
// except breakpoints, single steps and the C++ exception code, which are passed on.
#pragma once
#include <windows.h>
#include <stdint.h>

#if defined(__GNUC__) && !defined(__clang__)
struct VpGuard {
    void* jb[5];                       // __builtin_setjmp's buffer
    volatile DWORD code;
    VpGuard* outer;
    int (*filter)(EXCEPTION_POINTERS*, void*);   // the __except expression (null: EXCEPTION_EXECUTE_HANDLER)
    void* fctx;
};
static __thread VpGuard* t_vp_guard;
static PVOID g_vp_veh;

static LONG CALLBACK vp_guard_veh(EXCEPTION_POINTERS* ep) {
    const DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_BREAKPOINT || c == EXCEPTION_SINGLE_STEP || c == 0xE06D7363u || c == 0x406D1388u ||
        c == DBG_PRINTEXCEPTION_C || c == 0x4001000Au)
        return EXCEPTION_CONTINUE_SEARCH;
    // innermost guard first, as SEH asks the innermost __except first; a filter's EXCEPTION_CONTINUE_SEARCH asks the next
    for (VpGuard* g = t_vp_guard; g; g = g->outer) {
        const int f = g->filter ? g->filter(ep, g->fctx) : EXCEPTION_EXECUTE_HANDLER;
        if (f == EXCEPTION_CONTINUE_EXECUTION) return EXCEPTION_CONTINUE_EXECUTION;
        if (f == EXCEPTION_CONTINUE_SEARCH) continue;
        g->code = c;
        __builtin_longjmp(g->jb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// fn(arg) under a guard whose filter is filter(ep, fctx) (MSVC's __except (filter(GetExceptionInformation())), run
// before the unwind): 0, or the exception code that ended it
static __attribute__((noinline)) DWORD vp_guard_f(void (*fn)(void*), void* arg, int (*filter)(EXCEPTION_POINTERS*, void*),
                                                  void* fctx) {
    // last in the chain: every vectored handler the harness adds runs first, as they all run before any __except
    if (!g_vp_veh) g_vp_veh = AddVectoredExceptionHandler(0, vp_guard_veh);
    VpGuard g;
    g.code = 0;
    g.outer = t_vp_guard;
    g.filter = filter;
    g.fctx = fctx;
    t_vp_guard = &g;
    if (__builtin_setjmp(g.jb) == 0) {
        fn(arg);
    }
    t_vp_guard = g.outer;
    return g.code;
}
// fn(arg) under a guard: 0, or the exception code that ended it
static __attribute__((noinline)) DWORD vp_guard(void (*fn)(void*), void* arg) { return vp_guard_f(fn, arg, nullptr, nullptr); }

// The harnesses' __try bodies as lambdas (captures by reference): vp_try(body) is __try { body(); }
// __except (EXCEPTION_EXECUTE_HANDLER), vp_try(body, filter) is __except (filter(GetExceptionInformation())).
// Both return 0, or the exception code (GetExceptionCode() in the handler).
template <typename F> static inline DWORD vp_try(const F& body) {
    return vp_guard([](void* p) { (*(const F*)p)(); }, (void*)&body);
}
template <typename F, typename X> static inline DWORD vp_try(const F& body, const X& filter) {
    return vp_guard_f([](void* p) { (*(const F*)p)(); }, (void*)&body,
                      [](EXCEPTION_POINTERS* ep, void* x) { return (int)(*(const X*)x)(ep); }, (void*)&filter);
}
#else
static __declspec(noinline) DWORD vp_guard(void (*fn)(void*), void* arg) {
    DWORD code = 0;
    __try { fn(arg); } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return code ? code : 1; }
    return 0;
}
#endif
