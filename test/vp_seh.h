// vp_seh.h -- a guarded call for the harnesses that also builds with GCC (relink stage R1).
//
// The harnesses catch a faulting original or rewrite with MSVC's __try / __except. GCC has no __try, so a guarded
// call goes through vp_guard(): run fn(arg) and return 0, or the exception code it raised (an access violation, an
// unmasked float exception, a divide by zero, ...) with the stack unwound to the guard. Under MSVC it is __try /
// __except, as before; under GCC a vectored exception handler (first in the chain) long-jumps back to the guard.
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
};
static __thread VpGuard* t_vp_guard;
static PVOID g_vp_veh;

static LONG CALLBACK vp_guard_veh(EXCEPTION_POINTERS* ep) {
    VpGuard* g = t_vp_guard;
    const DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (!g || c == EXCEPTION_BREAKPOINT || c == EXCEPTION_SINGLE_STEP || c == 0xE06D7363u || c == 0x406D1388u ||
        c == DBG_PRINTEXCEPTION_C || c == 0x4001000Au)
        return EXCEPTION_CONTINUE_SEARCH;
    g->code = c;
    __builtin_longjmp(g->jb, 1);
}

// fn(arg) under a guard: 0, or the exception code that ended it
static __attribute__((noinline)) DWORD vp_guard(void (*fn)(void*), void* arg) {
    if (!g_vp_veh) g_vp_veh = AddVectoredExceptionHandler(1, vp_guard_veh);
    VpGuard g;
    g.code = 0;
    g.outer = t_vp_guard;
    t_vp_guard = &g;
    if (__builtin_setjmp(g.jb) == 0) {
        fn(arg);
    }
    t_vp_guard = g.outer;
    return g.code;
}
#else
static __declspec(noinline) DWORD vp_guard(void (*fn)(void*), void* arg) {
    DWORD code = 0;
    __try { fn(arg); } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return code ? code : 1; }
    return 0;
}
#endif
