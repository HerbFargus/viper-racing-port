// w32_seh.cpp -- the exception stand-ins (w32_kernel.h): SetUnhandledExceptionFilter, UnhandledExceptionFilter,
// RaiseException, RtlUnwind. On Windows they are Windows' own: structured exception handling needs the OS (the
// dispatcher walks the fs:[0] chain the game's frames build, with CONTEXTs the kernel captures). Three are jumps
// straight into KERNEL32 -- no frame of their own, so what Windows captures (RaiseException's ExceptionAddress, the
// caller's registers; RtlUnwind's return context) is exactly what it captures when race.exe calls it. Linux: the #else.
//
// R2b design (Linux), for the #else below:
//   * Every thread gets a TIB of its own (a fake TEB) behind fs, by set_thread_area / modify_ldt -- fs:[0] the SEH
//     chain head (-1 at start), fs:[4]/[8] the stack bounds, fs:[0x18] the TIB's own address. The loader makes the
//     main thread's before it calls race.exe's entry; w32_CreateThread's posix_entry makes each new thread's.
//   * A fault (SIGSEGV, SIGFPE, SIGILL, SIGTRAP; the x87's unmasked exceptions arrive as SIGFPE) becomes an
//     EXCEPTION_RECORD (code from si_code: STATUS_ACCESS_VIOLATION with the read/write flag and address,
//     STATUS_FLOAT_DIVIDE_BY_ZERO / _INVALID_OPERATION / _OVERFLOW ... from the FPU status word, as Windows maps
//     them, STATUS_INTEGER_DIVIDE_BY_ZERO, STATUS_ILLEGAL_INSTRUCTION, STATUS_BREAKPOINT) and a CONTEXT (i386: the
//     general registers, eflags, segment registers, and the FPU's FLOATING_SAVE_AREA from the ucontext's fpstate);
//     the handler switches to the faulting thread's stack (sigaltstack is not enough: the game's handlers run on the
//     faulting stack below the faulting frame) and runs the dispatcher there.
//   * The dispatcher walks fs:[0] as RtlDispatchException does: each record must lie within the stack bounds, each
//     handler is called (cdecl: record, frame, context, dispatcher context) and answers ExceptionContinueExecution
//     (restore the CONTEXT -- setcontext-like, the FPU state too), ExceptionContinueSearch (next), or doesn't return
//     (the handler called RtlUnwind and jumped into its __except block). Nested exceptions get
//     EXCEPTION_NESTED_CALL as Windows'. At the end of the chain: the filter SetUnhandledExceptionFilter set (the
//     game's crash handler: it logs, reads the link map appended to race.exe and exits), else exit as Windows does.
//   * RaiseException builds the record from its arguments, captures the caller's CONTEXT itself (an asm entry: the
//     return address is ExceptionAddress, esp as after the call) and dispatches. RtlUnwind calls each handler from the
//     chain head down to the target frame with EXCEPTION_UNWINDING (EXCEPTION_EXIT_UNWIND for a null target),
//     unlinks them, sets fs:[0] to the target frame and returns to its caller with the stack as the caller left it
//     (the C runtime's _global_unwind2 relies on ebx/esi/edi/ebp coming back unchanged).
//   * UnhandledExceptionFilter(pointers): calls the filter set (if any) and returns its answer; with none,
//     EXCEPTION_CONTINUE_SEARCH (no debugger, no Windows Error Reporting) -- the caller (_XcptFilter) then lets the
//     process end.
#include "w32_kernel.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
// KERNEL32's own, for the jumps (set when the DLL loads)
void* w32_real_UnhandledExceptionFilter = (void*)&UnhandledExceptionFilter;
void* w32_real_RaiseException = (void*)&RaiseException;
void* w32_real_RtlUnwind = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "RtlUnwind");
}

void* W32K_CALL w32_SetUnhandledExceptionFilter(void* filter) {
    return (void*)SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)filter);
}

#if defined(_MSC_VER)
__declspec(naked) int32_t W32K_CALL w32_UnhandledExceptionFilter(void*) {
    __asm jmp dword ptr [w32_real_UnhandledExceptionFilter]
}
__declspec(naked) void W32K_CALL w32_RaiseException(uint32_t, uint32_t, uint32_t, const uint32_t*) {
    __asm jmp dword ptr [w32_real_RaiseException]
}
__declspec(naked) void W32K_CALL w32_RtlUnwind(void*, void*, void*, void*) {
    __asm jmp dword ptr [w32_real_RtlUnwind]
}
#else
__attribute__((naked)) int32_t W32K_CALL w32_UnhandledExceptionFilter(void*) {
    __asm__("jmp dword ptr [_w32_real_UnhandledExceptionFilter]");
}
__attribute__((naked)) void W32K_CALL w32_RaiseException(uint32_t, uint32_t, uint32_t, const uint32_t*) {
    __asm__("jmp dword ptr [_w32_real_RaiseException]");
}
__attribute__((naked)) void W32K_CALL w32_RtlUnwind(void*, void*, void*, void*) {
    __asm__("jmp dword ptr [_w32_real_RtlUnwind]");
}
#endif

#else
// ==== Linux (R2b): the dispatcher of the notes above =================================================================
// Two ways in: a fault signal (on_fault, on the thread's signal stack) copies the record and CONTEXT it built onto the
// faulting thread's own stack below its esp and returns into from_signal there -- the kernel's sigreturn puts the
// thread back on its stack, its signal mask as it was, its FPU as it was but with the exception flags cleared and the
// register stack empty (the CONTEXT has the real one) -- the way Windows' kernel hands a fault to
// KiUserExceptionDispatcher; or RaiseException / RtlUnwind, asm entries that capture their caller's registers. Then
// rtl_dispatch (RtlDispatchException), and the end: restore_context (NtContinue) or the process ends.
#include <windows.h>                                           // hook/linux_inc -> win32_compat.h
#include "vp_os.h"
#include "w32_seh.h"
#include <asm/ldt.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <atomic>
#include <mutex>

namespace {

const uint32_t CHAIN_END = 0xffffffffu;
const uint32_t STATUS_UNWIND_ = 0xC0000027u;
const uint32_t STATUS_BAD_STACK_ = 0xC0000028u;
const uint32_t STATUS_INVALID_UNWIND_TARGET_ = 0xC0000029u;
const uint32_t FLOAT_SAVE_BYTES = 108;                         // FLOATING_SAVE_AREA up to Cr0NpxState: fnsave's image

// ---- the thread block ------------------------------------------------------------------------------------------------
// One page: the NT_TIB and the TEB fields after it that Windows code reads (the port reads fs:[0] only: the 1998 C
// runtime's SEH frames and standalone.cpp's probe; the rest is there for whatever else asks). The signal stack follows
// it in the same mapping.
struct Teb {
    uint32_t exception_list;                                   // 0x00 the SEH chain head
    uint32_t stack_base;                                       // 0x04 the stack's top (exclusive)
    uint32_t stack_limit;                                      // 0x08 its bottom
    uint32_t sub_system_tib;                                   // 0x0c
    uint32_t fiber_data;                                       // 0x10
    uint32_t arbitrary_user_pointer;                           // 0x14
    uint32_t self;                                             // 0x18 this block's address
    uint32_t environment_pointer;                              // 0x1c
    uint32_t process_id;                                       // 0x20 ClientId
    uint32_t thread_id;                                        // 0x24 (the stand-ins' GetCurrentThreadId)
    uint32_t active_rpc_handle;                                // 0x28
    uint32_t thread_local_storage_pointer;                     // 0x2c
    uint32_t peb;                                              // 0x30
    uint32_t last_error_value;                                 // 0x34 (the stand-ins keep their own: w32::last_error)
};
static_assert(offsetof(Teb, self) == 0x18 && offsetof(Teb, peb) == 0x30, "TEB layout");
const size_t TEB_BYTES = 0x1000;
const size_t ALT_STACK_BYTES = 0x10000;

alignas(4096) uint8_t g_peb[0x1000];                           // ImageBaseAddress (+8): race.exe at 0x400000

thread_local Teb* t_teb;                                       // this thread's block (null: none set up)
thread_local int t_in_fault;                                   // on_fault is building a dispatch (a fault in it: not ours)
thread_local sigjmp_buf* t_probe;                              // w32_seh_readable's guarded read
thread_local int t_in_base;                                    // the top-level filter is running
thread_local CONTEXT t_resume;                                 // restore_context's copy (off every stack)

uint32_t fs0() { return (uint32_t)__readfsdword(0); }
void set_fs0(uint32_t v) { __writefsdword(0, v); }

// ---- fs: a descriptor per thread -----------------------------------------------------------------------------------
// set_thread_area: one of the GDT's TLS slots (glibc's gs takes another); the slot's number is the same in every
// thread, its contents (the base) each thread's own -- so every thread's fs holds the same selector. modify_ldt if the
// kernel has no slot free: an LDT entry per thread (the LDT is the process's), freed when the thread ends.
std::atomic<int> g_gdt_entry{-1};
std::atomic<bool> g_use_ldt{false};
std::mutex g_ldt_mu;
uint8_t g_ldt_used[8192];

user_desc describe(uint32_t base, int entry) {
    user_desc d;
    memset(&d, 0, sizeof d);
    d.entry_number = (unsigned)entry;
    d.base_addr = base;
    d.limit = TEB_BYTES - 1;                                   // byte granular, as Windows' fs (0xfff)
    d.seg_32bit = 1;
    d.useable = 1;
    return d;
}

// fs -> base; the selector, or 0 (*ldt_slot: the LDT entry taken, else 0)
uint16_t point_fs(uint32_t base, int* ldt_slot) {
    *ldt_slot = 0;
    if (!g_use_ldt.load()) {
        user_desc d = describe(base, g_gdt_entry.load());
        if (syscall(SYS_set_thread_area, &d) == 0) {
            g_gdt_entry = (int)d.entry_number;
            return (uint16_t)((d.entry_number << 3) | 3);
        }
        if (g_gdt_entry.load() < 0) g_use_ldt = true;          // (no slot for the first thread: none for any)
        else return 0;
    }
    std::lock_guard<std::mutex> lk(g_ldt_mu);
    for (int k = 1; k < 8192; k++) {
        if (g_ldt_used[k]) continue;
        user_desc d = describe(base, k);
        if (syscall(SYS_modify_ldt, 1, &d, sizeof d) != 0) return 0;
        g_ldt_used[k] = 1;
        *ldt_slot = k;
        return (uint16_t)((k << 3) | 7);
    }
    return 0;
}

void load_fs(uint16_t sel) { __asm__ __volatile__("mov fs, %w0" : : "r"((uint32_t)sel) : "memory"); }

// a thread's block, freed when it ends (not the main thread's: exit() runs thread_local destructors before atexit's,
// and the port's exit work may still raise)
struct TebOwner {
    void* map = 0;
    int ldt_slot = 0;
    ~TebOwner() {
        if (!map) return;
        stack_t ss;
        memset(&ss, 0, sizeof ss);
        ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, 0);
        t_teb = 0;
        load_fs(0);
        if (ldt_slot) {
            user_desc d;
            memset(&d, 0, sizeof d);
            d.entry_number = (unsigned)ldt_slot;
            d.read_exec_only = 1;
            d.seg_not_present = 1;
            std::lock_guard<std::mutex> lk(g_ldt_mu);
            syscall(SYS_modify_ldt, 1, &d, sizeof d);
            g_ldt_used[ldt_slot] = 0;
        }
        munmap(map, TEB_BYTES + ALT_STACK_BYTES);
    }
};
thread_local TebOwner t_owner;

// ---- the vectored handlers ---------------------------------------------------------------------------------------------
// First = nonzero: in front. Called in order for every exception (not for unwinds), before the frames.
struct Veh {
    PVECTORED_EXCEPTION_HANDLER h;
    bool removed;
};
const int MAX_VEH = 32;
std::mutex g_veh_mu;
Veh* g_veh[MAX_VEH];
int g_nveh;

// ---- the CONTEXT ---------------------------------------------------------------------------------------------------------
uint16_t seg_cs() { uint32_t v; __asm__("mov %0, cs" : "=r"(v)); return (uint16_t)v; }
uint16_t seg_ds() { uint32_t v; __asm__("mov %0, ds" : "=r"(v)); return (uint16_t)v; }
uint16_t seg_es() { uint32_t v; __asm__("mov %0, es" : "=r"(v)); return (uint16_t)v; }
uint16_t seg_fs() { uint32_t v; __asm__("mov %0, fs" : "=r"(v)); return (uint16_t)v; }
uint16_t seg_gs() { uint32_t v; __asm__("mov %0, gs" : "=r"(v)); return (uint16_t)v; }
uint16_t seg_ss() { uint32_t v; __asm__("mov %0, ss" : "=r"(v)); return (uint16_t)v; }

// NtContinue: every register from the CONTEXT (the FPU too when it has CONTEXT_FLOATING_POINT), then its eip. The two
// words below its esp take eflags and eip on the way (free stack: the target's esp is its top).
__attribute__((noreturn, noinline)) void restore_context(const CONTEXT* c) {
    CONTEXT* r = &t_resume;
    memcpy(r, c, offsetof(CONTEXT, ExtendedRegisters));
    __asm__ __volatile__(
        "cld\n\t"
        "test byte ptr [ebx], 8\n\t"                           // ContextFlags & CONTEXT_FLOATING_POINT
        "jz vp_seh_nofp%=\n\t"
        "frstor [ebx + %c[fsave]]\n\t"
        "vp_seh_nofp%=:\n\t"
        "mov esp, dword ptr [ebx + %c[esp]]\n\t"
        "push dword ptr [ebx + %c[eip]]\n\t"
        "push dword ptr [ebx + %c[efl]]\n\t"
        "mov eax, dword ptr [ebx + %c[eax]]\n\t"
        "mov ecx, dword ptr [ebx + %c[ecx]]\n\t"
        "mov edx, dword ptr [ebx + %c[edx]]\n\t"
        "mov esi, dword ptr [ebx + %c[esi]]\n\t"
        "mov edi, dword ptr [ebx + %c[edi]]\n\t"
        "mov ebp, dword ptr [ebx + %c[ebp]]\n\t"
        "mov ebx, dword ptr [ebx + %c[ebx]]\n\t"
        "popfd\n\t"
        "ret"
        :
        : "b"(r), [fsave] "i"(offsetof(CONTEXT, FloatSave)), [esp] "i"(offsetof(CONTEXT, Esp)),
          [eip] "i"(offsetof(CONTEXT, Eip)), [efl] "i"(offsetof(CONTEXT, EFlags)), [eax] "i"(offsetof(CONTEXT, Eax)),
          [ecx] "i"(offsetof(CONTEXT, Ecx)), [edx] "i"(offsetof(CONTEXT, Edx)), [esi] "i"(offsetof(CONTEXT, Esi)),
          [edi] "i"(offsetof(CONTEXT, Edi)), [ebp] "i"(offsetof(CONTEXT, Ebp)), [ebx] "i"(offsetof(CONTEXT, Ebx))
        : "memory");
    __builtin_unreachable();
}

// what the asm entries push: eflags and the general registers as the caller left them, then its return address and
// the arguments
struct EntryRegs {
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp, eflags, ret;
    uint32_t args[4];
};
// the caller's CONTEXT as RtlCaptureContext would leave it for a return from an nargs-argument __stdcall function:
// eip its return address, esp past the arguments (no FPU state: CONTEXT_FULL, as Windows captures)
void context_from_entry(CONTEXT* c, const EntryRegs* r, int nargs) {
    memset(c, 0, sizeof *c);
    c->ContextFlags = CONTEXT_FULL;
    c->Eax = r->eax;
    c->Ecx = r->ecx;
    c->Edx = r->edx;
    c->Ebx = r->ebx;
    c->Esi = r->esi;
    c->Edi = r->edi;
    c->Ebp = r->ebp;
    c->EFlags = r->eflags;
    c->Eip = r->ret;
    c->Esp = (uint32_t)(uintptr_t)&r->args[nargs];
    c->SegCs = seg_cs();
    c->SegDs = seg_ds();
    c->SegEs = seg_es();
    c->SegFs = seg_fs();
    c->SegGs = seg_gs();
    c->SegSs = seg_ss();
}

// ---- the dispatcher --------------------------------------------------------------------------------------------------
[[noreturn]] void dispatch_or_end(EXCEPTION_RECORD* rec, CONTEXT* ctx);

// RtlRaiseException of a status the dispatcher itself raises (a non-continuable exception continued, a bad
// disposition, a bad unwind target or stack): non-continuable, the record that caused it attached
[[noreturn]] __attribute__((noinline)) void raise_status(uint32_t code, EXCEPTION_RECORD* cause) {
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = code;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    rec.ExceptionRecord = cause;
    rec.ExceptionAddress = __builtin_return_address(0);
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Eip = (uint32_t)(uintptr_t)rec.ExceptionAddress;
    ctx.Esp = (uint32_t)(uintptr_t)__builtin_frame_address(0) + 8;
    ctx.Ebp = *(uint32_t*)__builtin_frame_address(0);
    ctx.EFlags = 0x202;
    ctx.SegCs = seg_cs();
    ctx.SegDs = ctx.SegEs = ctx.SegSs = seg_ds();
    ctx.SegFs = seg_fs();
    ctx.SegGs = seg_gs();
    dispatch_or_end(&rec, &ctx);
}

}  // namespace

// the handler call: cdecl handler(record, frame, context, dispatcher context), callee-saved registers kept whatever
// the handler does, esp put back (ExecuteHandler2's job)
extern "C" __attribute__((naked)) uint32_t vp_seh_call_handler(void*, void*, uint32_t, void*, void*) {
    __asm__(
        "push ebp\n\t"
        "mov ebp, esp\n\t"
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "push dword ptr [ebp + 24]\n\t"
        "push dword ptr [ebp + 20]\n\t"
        "push dword ptr [ebp + 16]\n\t"
        "push dword ptr [ebp + 12]\n\t"
        "cld\n\t"
        "call dword ptr [ebp + 8]\n\t"
        "lea esp, [ebp - 12]\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "pop ebp\n\t"
        "ret");
}

namespace {

// The frame the dispatcher puts on the chain around each handler call (RtlpExecuteHandlerForException /
// ...ForUnwind): an exception raised inside the handler finds it first and is told whose handler was running.
struct GuardFrame {
    EXCEPTION_REGISTRATION_RECORD reg;
    uint32_t establisher;
};
EXCEPTION_DISPOSITION __cdecl guard_for_exception(EXCEPTION_RECORD* r, void* frame, CONTEXT*, void* dc) {
    if (r->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    *(uint32_t*)dc = ((GuardFrame*)frame)->establisher;
    return ExceptionNestedException;
}
EXCEPTION_DISPOSITION __cdecl guard_for_unwind(EXCEPTION_RECORD* r, void* frame, CONTEXT*, void* dc) {
    if (!(r->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))) return ExceptionContinueSearch;
    *(uint32_t*)dc = ((GuardFrame*)frame)->establisher;
    return ExceptionCollidedUnwind;
}
__attribute__((noinline)) uint32_t execute_handler(EXCEPTION_RECORD* rec, uint32_t frame, CONTEXT* ctx, uint32_t* dc,
                                                   PEXCEPTION_ROUTINE guard) {
    GuardFrame g;
    g.reg.Handler = guard;
    g.establisher = frame;
    g.reg.Next = (EXCEPTION_REGISTRATION_RECORD*)(uintptr_t)fs0();
    set_fs0((uint32_t)(uintptr_t)&g);
    const uint32_t d =
        vp_seh_call_handler((void*)((EXCEPTION_REGISTRATION_RECORD*)(uintptr_t)frame)->Handler, rec, frame, ctx, dc);
    set_fs0((uint32_t)(uintptr_t)g.reg.Next);
    return d;
}

bool frame_ok(const Teb* t, uint32_t f) { return f >= t->stack_limit && f + 8 <= t->stack_base && !(f & 3); }

// RtlUnwind's walk: each frame's handler from the head down to target with the unwind flags, each unlinked; true at
// the target (CHAIN_END: the end of the chain), false at the end without it
bool unwind_walk(EXCEPTION_RECORD* rec, CONTEXT* ctx, uint32_t target) {
    const Teb* t = t_teb;
    uint32_t f = fs0();
    while (f != CHAIN_END) {
        if (f == target) return true;
        if (target && target < f) raise_status(STATUS_INVALID_UNWIND_TARGET_, rec);
        if (!frame_ok(t, f)) raise_status(STATUS_BAD_STACK_, rec);
        uint32_t dc = 0;
        const uint32_t d = execute_handler(rec, f, ctx, &dc, guard_for_unwind);
        if (d == ExceptionCollidedUnwind) f = dc;
        else if (d != ExceptionContinueSearch) raise_status(STATUS_INVALID_DISPOSITION, rec);
        f = (uint32_t)(uintptr_t)((EXCEPTION_REGISTRATION_RECORD*)(uintptr_t)f)->Next;
        set_fs0(f);
    }
    return target == CHAIN_END;
}

// The end of the chain is the frame Windows' BaseProcessStart / BaseThreadStart put under every thread:
// __except (UnhandledExceptionFilter(GetExceptionInformation())) { ExitProcess(GetExceptionCode()); }.
// true: continue execution; false: unhandled (the process ends without its exit work, as Windows' second chance).
bool base_frame(EXCEPTION_RECORD* rec, CONTEXT* ctx) {
    if (t_in_base) return false;                               // the filter itself faulted
    t_in_base = 1;
    EXCEPTION_POINTERS ep = {rec, ctx};
    const int32_t r = w32_UnhandledExceptionFilter(&ep);
    t_in_base = 0;
    if (r == EXCEPTION_CONTINUE_EXECUTION) {
        if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) raise_status(STATUS_NONCONTINUABLE_EXCEPTION, rec);
        return true;
    }
    if (r == EXCEPTION_CONTINUE_SEARCH) return false;
    // the __except block: the global unwind to its frame (every frame of the chain), then ExitProcess
    EXCEPTION_RECORD u;
    memset(&u, 0, sizeof u);
    u.ExceptionCode = STATUS_UNWIND_;
    u.ExceptionFlags = EXCEPTION_UNWINDING;
    u.ExceptionAddress = __builtin_return_address(0);
    unwind_walk(&u, ctx, CHAIN_END);
    w32_ExitProcess(rec->ExceptionCode);
    return false;
}

// RtlDispatchException: true -- continue with the (possibly changed) CONTEXT
bool rtl_dispatch(EXCEPTION_RECORD* rec, CONTEXT* ctx) {
    {
        Veh* v[MAX_VEH];
        int n;
        {
            std::lock_guard<std::mutex> lk(g_veh_mu);
            n = g_nveh;
            memcpy(v, g_veh, n * sizeof v[0]);
        }
        EXCEPTION_POINTERS ep = {rec, ctx};
        for (int i = 0; i < n; i++)
            if (!v[i]->removed && v[i]->h(&ep) == EXCEPTION_CONTINUE_EXECUTION) return true;
    }
    const Teb* t = t_teb;
    uint32_t nested = 0;
    uint32_t f = fs0();
    while (f != CHAIN_END) {
        if (!frame_ok(t, f)) {
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return false;
        }
        uint32_t dc = 0;
        const uint32_t d = execute_handler(rec, f, ctx, &dc, guard_for_exception);
        if (nested == f) {
            rec->ExceptionFlags &= ~EXCEPTION_NESTED_CALL;
            nested = 0;
        }
        switch (d) {
        case ExceptionContinueExecution:
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) raise_status(STATUS_NONCONTINUABLE_EXCEPTION, rec);
            return true;
        case ExceptionContinueSearch:
            if (rec->ExceptionFlags & EXCEPTION_STACK_INVALID) return false;
            break;
        case ExceptionNestedException:
            rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
            if (dc > nested) nested = dc;
            break;
        default:
            raise_status(STATUS_INVALID_DISPOSITION, rec);
        }
        f = (uint32_t)(uintptr_t)((EXCEPTION_REGISTRATION_RECORD*)(uintptr_t)f)->Next;
    }
    return base_frame(rec, ctx);
}

// unhandled: Windows' second chance with no debugger -- the process ends with the exception's code, no exit work
[[noreturn]] void unhandled(const EXCEPTION_RECORD* rec, const char* how) {
    fprintf(stderr, "viperport: unhandled exception %08x at %08x%s (thread %u) -- the process ends\n",
            (unsigned)rec->ExceptionCode, (unsigned)(uintptr_t)rec->ExceptionAddress, how,
            (unsigned)w32_GetCurrentThreadId());
    fflush(stderr);
    _exit((int)rec->ExceptionCode);
}

void dispatch_or_end(EXCEPTION_RECORD* rec, CONTEXT* ctx) {
    if (!t_teb) unhandled(rec, " on a thread without a TIB");
    if (rtl_dispatch(rec, ctx)) restore_context(ctx);
    unhandled(rec, (rec->ExceptionFlags & EXCEPTION_STACK_INVALID) ? " (an SEH record off the stack)" : "");
}

// ---- faults ----------------------------------------------------------------------------------------------------------
// the x87's exception as Windows reports it (the first unmasked flag, invalid before the rest)
uint32_t fpu_code(const FLOATING_SAVE_AREA& f) {
    const uint32_t s = f.StatusWord & ~(f.ControlWord & 0x3f);
    if (s & 0x01) return (s & 0x40) ? STATUS_FLOAT_STACK_CHECK : STATUS_FLOAT_INVALID_OPERATION;
    if (s & 0x02) return STATUS_FLOAT_DENORMAL_OPERAND;
    if (s & 0x04) return STATUS_FLOAT_DIVIDE_BY_ZERO;
    if (s & 0x08) return STATUS_FLOAT_OVERFLOW;
    if (s & 0x10) return STATUS_FLOAT_UNDERFLOW;
    if (s & 0x20) return STATUS_FLOAT_INEXACT_RESULT;
    return STATUS_FLOAT_INVALID_OPERATION;
}

bool is_prefix(uint8_t b) {
    return b == 0x66 || b == 0x67 || b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x26 || b == 0x2e || b == 0x36 ||
           b == 0x3e || b == 0x64 || b == 0x65;
}
uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// #DE: Windows reads the div / idiv's divisor -- zero: STATUS_INTEGER_DIVIDE_BY_ZERO; else the quotient didn't fit:
// STATUS_INTEGER_OVERFLOW
uint32_t divide_code(const greg_t* g) {
    static const int reg[8] = {REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI};
    const uint8_t* p = (const uint8_t*)(uintptr_t)g[REG_EIP];
    bool o16 = false, gs = false, fs = false;
    for (int i = 0; i < 14 && is_prefix(*p); i++, p++) {
        if (*p == 0x66) o16 = true;
        else if (*p == 0x67) return STATUS_INTEGER_DIVIDE_BY_ZERO;   // (16-bit addressing: not decoded)
        else if (*p == 0x64) fs = true;
        else if (*p == 0x65) gs = true;
    }
    if (p[0] != 0xf6 && p[0] != 0xf7) return STATUS_INTEGER_DIVIDE_BY_ZERO;
    const int size = p[0] == 0xf6 ? 1 : o16 ? 2 : 4;
    const uint8_t modrm = p[1];
    const int mod = modrm >> 6, rm = modrm & 7;
    if (((modrm >> 3) & 7) < 6) return STATUS_INTEGER_DIVIDE_BY_ZERO;
    uint32_t v;
    if (mod == 3) {
        if (size == 1) v = rm < 4 ? (uint32_t)g[reg[rm]] : (uint32_t)g[reg[rm - 4]] >> 8;
        else v = (uint32_t)g[reg[rm]];
    } else {
        if (gs) return STATUS_INTEGER_DIVIDE_BY_ZERO;
        const uint8_t* q = p + 2;
        uint32_t a = 0;
        if (rm == 4) {
            const uint8_t sib = *q++;
            const int base = sib & 7, index = (sib >> 3) & 7, scale = sib >> 6;
            if (index != 4) a += (uint32_t)g[reg[index]] << scale;
            if (base == 5 && mod == 0) {
                a += rd32(q);
                q += 4;
            } else {
                a += (uint32_t)g[reg[base]];
            }
        } else if (rm == 5 && mod == 0) {
            a = rd32(q);
            q += 4;
        } else {
            a = (uint32_t)g[reg[rm]];
        }
        if (mod == 1) a += (uint32_t)(int32_t)(int8_t)*q;
        else if (mod == 2) a += rd32(q);
        if (fs) a += t_teb->self;
        v = 0;
        memcpy(&v, (const void*)(uintptr_t)a, size);          // (the div read it just now)
    }
    if (size == 1) v &= 0xff;
    else if (size == 2) v &= 0xffff;
    return v ? STATUS_INTEGER_OVERFLOW : STATUS_INTEGER_DIVIDE_BY_ZERO;
}

// #GP: a privileged instruction is STATUS_PRIVILEGED_INSTRUCTION, anything else an access violation
bool privileged(const uint8_t* p) {
    for (int i = 0; i < 14 && is_prefix(*p); i++) p++;
    switch (p[0]) {
    case 0xf4: case 0xfa: case 0xfb:                           // hlt cli sti
    case 0x6c: case 0x6d: case 0x6e: case 0x6f:                // ins outs
    case 0xe4: case 0xe5: case 0xe6: case 0xe7:                // in out imm
    case 0xec: case 0xed: case 0xee: case 0xef:                // in out dx
        return true;
    case 0x0f:
        switch (p[1]) {
        case 0x06: case 0x08: case 0x09: case 0x20: case 0x21: case 0x22: case 0x23: case 0x30: case 0x32:
            return true;
        case 0x01: {
            const int r = (p[2] >> 3) & 7;                     // lgdt lidt lmsw invlpg
            return r == 2 || r == 3 || r == 6 || r == 7;
        }
        }
        return false;
    }
    return false;
}

struct sigaction g_old[NSIG];
const int k_fault_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};

// not a fault the game's SEH can take (no TIB on this thread, a fault while building one, a signal another process
// sent): whoever had the signal before, else its default action (the process ends as it would have without us)
void pass_on(int sig, siginfo_t* si, void* uc) {
    const struct sigaction& o = g_old[sig];
    if ((o.sa_flags & SA_SIGINFO) && o.sa_sigaction) {
        o.sa_sigaction(sig, si, uc);
        return;
    }
    if (!(o.sa_flags & SA_SIGINFO) && o.sa_handler != SIG_DFL && o.sa_handler != SIG_IGN) {
        o.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, sig);
    pthread_sigmask(SIG_UNBLOCK, &m, 0);
    raise(sig);
}

}  // namespace

// where on_fault sends the thread: called, as it were, from the faulting instruction
extern "C" __attribute__((noreturn, force_align_arg_pointer, used)) void vp_seh_from_signal(EXCEPTION_RECORD* rec,
                                                                                             CONTEXT* ctx) {
    dispatch_or_end(rec, ctx);
}

namespace {

void on_fault(int sig, siginfo_t* si, void* ucv) {
    if (t_probe && (sig == SIGSEGV || sig == SIGBUS)) siglongjmp(*t_probe, 1);
    Teb* teb = t_teb;
    if (!teb || t_in_fault || si->si_code <= 0) {
        pass_on(sig, si, ucv);
        return;
    }
    t_in_fault = 1;
    ucontext_t* uc = (ucontext_t*)ucv;
    greg_t* g = uc->uc_mcontext.gregs;
    fpregset_t fp = uc->uc_mcontext.fpregs;
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL | (fp ? CONTEXT_FLOATING_POINT : 0);
    ctx.SegGs = g[REG_GS] & 0xffff;
    ctx.SegFs = g[REG_FS] & 0xffff;
    ctx.SegEs = g[REG_ES] & 0xffff;
    ctx.SegDs = g[REG_DS] & 0xffff;
    ctx.Edi = g[REG_EDI];
    ctx.Esi = g[REG_ESI];
    ctx.Ebx = g[REG_EBX];
    ctx.Edx = g[REG_EDX];
    ctx.Ecx = g[REG_ECX];
    ctx.Eax = g[REG_EAX];
    ctx.Ebp = g[REG_EBP];
    ctx.Eip = g[REG_EIP];
    ctx.SegCs = g[REG_CS] & 0xffff;
    ctx.EFlags = g[REG_EFL];
    ctx.Esp = g[REG_ESP];
    ctx.SegSs = g[REG_SS] & 0xffff;
    if (fp) memcpy(&ctx.FloatSave, fp, FLOAT_SAVE_BYTES);      // (glibc's _libc_fpstate: fnsave's image, as Windows')
    rec.ExceptionAddress = (void*)(uintptr_t)ctx.Eip;
    const int trap = g[REG_TRAPNO];
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
        if (trap == 17 || (sig == SIGBUS && si->si_code == BUS_ADRALN)) {
            rec.ExceptionCode = STATUS_DATATYPE_MISALIGNMENT;
        } else if (trap == 5) {
            rec.ExceptionCode = STATUS_ARRAY_BOUNDS_EXCEEDED;
        } else if (trap == 13 && privileged((const uint8_t*)(uintptr_t)ctx.Eip)) {
            rec.ExceptionCode = STATUS_PRIVILEGED_INSTRUCTION;
        } else if (trap == 14 || sig == SIGBUS) {
            const uint32_t err = g[REG_ERR];
            rec.ExceptionCode = STATUS_ACCESS_VIOLATION;
            rec.NumberParameters = 2;
            rec.ExceptionInformation[0] = (err & 0x10) ? 8 : (err & 2) ? 1 : 0;   // execute (DEP), write, read
            rec.ExceptionInformation[1] = (ULONG_PTR)(uintptr_t)si->si_addr;
        } else {
            rec.ExceptionCode = STATUS_ACCESS_VIOLATION;       // a general protection fault: no address
            rec.NumberParameters = 2;
            rec.ExceptionInformation[0] = 0;
            rec.ExceptionInformation[1] = 0xffffffffu;
        }
        break;
    case SIGILL:
        rec.ExceptionCode = si->si_code == ILL_PRVOPC ? STATUS_PRIVILEGED_INSTRUCTION : STATUS_ILLEGAL_INSTRUCTION;
        break;
    case SIGFPE:
        if (trap == 0) {
            rec.ExceptionCode = divide_code(g);
        } else if (trap == 4) {
            rec.ExceptionCode = STATUS_INTEGER_OVERFLOW;
        } else if (trap == 16 && fp) {
            rec.ExceptionCode = fpu_code(ctx.FloatSave);
            rec.ExceptionAddress = (void*)(uintptr_t)ctx.FloatSave.ErrorOffset;   // the x87 instruction, not the wait
        } else {
            switch (si->si_code) {
            case FPE_INTDIV: rec.ExceptionCode = STATUS_INTEGER_DIVIDE_BY_ZERO; break;
            case FPE_INTOVF: rec.ExceptionCode = STATUS_INTEGER_OVERFLOW; break;
            case FPE_FLTDIV: rec.ExceptionCode = STATUS_FLOAT_DIVIDE_BY_ZERO; break;
            case FPE_FLTOVF: rec.ExceptionCode = STATUS_FLOAT_OVERFLOW; break;
            case FPE_FLTUND: rec.ExceptionCode = STATUS_FLOAT_UNDERFLOW; break;
            case FPE_FLTRES: rec.ExceptionCode = STATUS_FLOAT_INEXACT_RESULT; break;
            case FPE_FLTSUB: rec.ExceptionCode = STATUS_ARRAY_BOUNDS_EXCEEDED; break;
            default: rec.ExceptionCode = STATUS_FLOAT_INVALID_OPERATION; break;
            }
        }
        break;
    case SIGTRAP:
        if (trap == 3) {                                       // int3: Windows reports (and resumes at) the int3 itself
            ctx.Eip--;
            rec.ExceptionAddress = (void*)(uintptr_t)ctx.Eip;
            rec.ExceptionCode = STATUS_BREAKPOINT;
            rec.NumberParameters = 1;
        } else {
            rec.ExceptionCode = STATUS_SINGLE_STEP;
        }
        break;
    }

    // onto the faulting stack, below its esp -- or below this handler, if it runs on that stack (no signal stack)
    uintptr_t top = ctx.Esp;
    const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    if (here < top && top - here < 0x100000) top = here - 0x1000;
    const uintptr_t c_at = (top - sizeof(CONTEXT)) & ~(uintptr_t)15;
    const uintptr_t r_at = (c_at - sizeof(EXCEPTION_RECORD)) & ~(uintptr_t)15;
    const uintptr_t sp = r_at - 20;                            // [sp] eip, [sp+4] record, [sp+8] context; sp+4 16-aligned
    if (ctx.Esp <= teb->stack_base && ctx.Esp > teb->stack_limit && sp < teb->stack_limit + 0x400) {
        fprintf(stderr, "viperport: exception %08x at %08x with the stack used up (esp %08x) -- the process ends\n",
                (unsigned)rec.ExceptionCode, (unsigned)ctx.Eip, (unsigned)ctx.Esp);
        _exit((int)STATUS_STACK_OVERFLOW);
    }
    memcpy((void*)c_at, &ctx, sizeof ctx);
    memcpy((void*)r_at, &rec, sizeof rec);
    ((uint32_t*)sp)[0] = ctx.Eip;
    ((uint32_t*)sp)[1] = (uint32_t)r_at;
    ((uint32_t*)sp)[2] = (uint32_t)c_at;
    g[REG_EIP] = (greg_t)(uintptr_t)&vp_seh_from_signal;
    g[REG_ESP] = (greg_t)sp;
    g[REG_EFL] &= ~(0x100 | 0x400 | 0x40000);                  // TF, DF, AC: as a kernel entry leaves them
    if (fp) {
        fp->sw &= ~0x80ffu;                                    // no exception pending: the dispatcher's own x87 code
        fp->status &= ~0x80ffu;
        fp->tag = 0xffff;                                      // empty register stack (the CONTEXT has it)
    }
    t_in_fault = 0;
}

void install_handlers() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigfillset(&sa.sa_mask);
        for (int s : k_fault_signals) sigdelset(&sa.sa_mask, s);
        for (int s : k_fault_signals) sigaction(s, &sa, &g_old[s]);
    });
}

}  // namespace

// ---- RaiseException / RtlUnwind (asm entries: the caller's registers, then C) -----------------------------------------
// RaiseException(code, flags, nargs, args): ExceptionAddress its return address; continued, it returns to its caller
extern "C" __attribute__((noreturn, force_align_arg_pointer, used)) void vp_seh_raise_c(EntryRegs* r) {
    CONTEXT ctx;
    context_from_entry(&ctx, r, 4);
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = r->args[0];
    rec.ExceptionFlags = r->args[1] & EXCEPTION_NONCONTINUABLE;
    rec.ExceptionAddress = (void*)(uintptr_t)r->ret;
    const uint32_t* a = (const uint32_t*)(uintptr_t)r->args[3];
    uint32_t n = a ? r->args[2] : 0;
    if (n > EXCEPTION_MAXIMUM_PARAMETERS) n = EXCEPTION_MAXIMUM_PARAMETERS;
    rec.NumberParameters = n;
    for (uint32_t i = 0; i < n; i++) rec.ExceptionInformation[i] = a[i];
    dispatch_or_end(&rec, &ctx);
}

// RtlUnwind(target frame, target ip (unused on x86), record, return value): the frames above target unwound and
// unlinked, fs:[0] = target; returns to its caller with eax = the return value, every other register as it called
extern "C" __attribute__((noreturn, force_align_arg_pointer, used)) void vp_seh_unwind_c(EntryRegs* r) {
    CONTEXT ctx;
    context_from_entry(&ctx, r, 4);
    ctx.Eax = r->args[3];
    const uint32_t target = r->args[0];
    EXCEPTION_RECORD own;
    EXCEPTION_RECORD* rec = (EXCEPTION_RECORD*)(uintptr_t)r->args[2];
    if (!rec) {
        memset(&own, 0, sizeof own);
        own.ExceptionCode = STATUS_UNWIND_;
        own.ExceptionAddress = (void*)(uintptr_t)r->ret;
        rec = &own;
    }
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (!target) rec->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;
    if (!t_teb) unhandled(rec, " (RtlUnwind on a thread without a TIB)");
    if (unwind_walk(rec, &ctx, target)) restore_context(&ctx);       // (an exit unwind, target 0, walks it all)
    unhandled(rec, " (RtlUnwind past the end of the chain)");
}

static std::atomic<void*> g_top_filter{0};

void* W32K_CALL w32_SetUnhandledExceptionFilter(void* filter) { return g_top_filter.exchange(filter); }

// no debugger, no error reporting: the filter's answer, or EXCEPTION_CONTINUE_SEARCH without one
int32_t W32K_CALL w32_UnhandledExceptionFilter(void* pointers) {
    void* f = g_top_filter.load();
    if (!f) return EXCEPTION_CONTINUE_SEARCH;
    return ((LONG(__stdcall*)(EXCEPTION_POINTERS*))f)((EXCEPTION_POINTERS*)pointers);
}

#define VP_SEH_ENTRY(c_impl)                                                                                            \
    __asm__("pushfd\n\tpush ebp\n\tpush edi\n\tpush esi\n\tpush ebx\n\tpush edx\n\tpush ecx\n\tpush eax\n\t"            \
            "push esp\n\tcall " #c_impl "\n\tud2")
__attribute__((naked)) void W32K_CALL w32_RaiseException(uint32_t, uint32_t, uint32_t, const uint32_t*) {
    VP_SEH_ENTRY(vp_seh_raise_c);
}
__attribute__((naked)) void W32K_CALL w32_RtlUnwind(void*, void*, void*, void*) { VP_SEH_ENTRY(vp_seh_unwind_c); }

PVOID vpos_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) {
    std::lock_guard<std::mutex> lk(g_veh_mu);
    if (!h || g_nveh == MAX_VEH) return 0;
    Veh* v = new Veh{h, false};                                // (never freed: a dispatch may hold it)
    if (first) {
        memmove(&g_veh[1], &g_veh[0], g_nveh * sizeof g_veh[0]);
        g_veh[0] = v;
    } else {
        g_veh[g_nveh] = v;
    }
    g_nveh++;
    return v;
}

// the Win32 names standalone.cpp calls (the int3 trap, the SEH self-test)
extern "C" {
PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) {
    return vpos_AddVectoredExceptionHandler(first, h);
}
ULONG WINAPI RemoveVectoredExceptionHandler(PVOID handle) {
    std::lock_guard<std::mutex> lk(g_veh_mu);
    for (int i = 0; i < g_nveh; i++) {
        if (g_veh[i] != handle) continue;
        g_veh[i]->removed = true;
        memmove(&g_veh[i], &g_veh[i + 1], (g_nveh - i - 1) * sizeof g_veh[0]);
        g_nveh--;
        return 1;
    }
    return 0;
}
__attribute__((naked)) VOID WINAPI RaiseException(DWORD, DWORD, DWORD, const ULONG_PTR*) {
    __asm__("jmp %P0" : : "i"(&w32_RaiseException));        // (the same frame: its caller's context)
}
}

// ---- the thread block ----------------------------------------------------------------------------------------------------
void w32_seh_thread_begin() {
    if (t_teb) return;
    install_handlers();
    *(uint32_t*)(g_peb + 8) = 0x400000;
    void* map = mmap(0, TEB_BYTES + ALT_STACK_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "viperport: no memory for a thread's TIB\n");
        abort();
    }
    Teb* t = (Teb*)map;
    t->exception_list = CHAIN_END;
    t->self = (uint32_t)(uintptr_t)t;
    t->process_id = (uint32_t)getpid();
    t->thread_id = w32_GetCurrentThreadId();
    t->peb = (uint32_t)(uintptr_t)g_peb;
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        void* lo = 0;
        size_t n = 0;
        pthread_attr_getstack(&a, &lo, &n);
        pthread_attr_destroy(&a);
        t->stack_limit = (uint32_t)(uintptr_t)lo;
        t->stack_base = (uint32_t)((uintptr_t)lo + n);
    }
    if (!t->stack_base) {                                      // (unknown: this frame's neighbourhood)
        const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
        t->stack_base = (uint32_t)((here + 0x10000) & ~(uintptr_t)0xfff);
        t->stack_limit = (uint32_t)(here - 0x100000);
    }
    int ldt_slot = 0;
    const uint16_t sel = point_fs(t->self, &ldt_slot);
    if (!sel) {
        fprintf(stderr, "viperport: can't point fs at a thread's TIB (set_thread_area / modify_ldt refused)\n");
        abort();
    }
    load_fs(sel);
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = (uint8_t*)map + TEB_BYTES;
    ss.ss_size = ALT_STACK_BYTES;
    sigaltstack(&ss, 0);
    t_teb = t;
    if ((pid_t)syscall(SYS_gettid) != getpid()) {
        t_owner.map = map;
        t_owner.ldt_slot = ldt_slot;
    }
}

void w32_seh_set_stack_bounds(void* limit, void* base) {
    if (!t_teb) return;
    t_teb->stack_limit = (uint32_t)(uintptr_t)limit;
    t_teb->stack_base = (uint32_t)(uintptr_t)base;
}

bool w32_seh_readable(const void* p, size_t n) {
    if (!n) return true;
    if (!p) return false;
    install_handlers();
    uint16_t cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    sigjmp_buf jb;
    sigjmp_buf* const prev = t_probe;
    volatile bool ok = true;
    if (sigsetjmp(jb, 1) == 0) {
        t_probe = &jb;
        __asm__ __volatile__("" : : : "memory");              // (set before the reads, not merged with the reset)
        const volatile uint8_t* b = (const volatile uint8_t*)p;
        (void)b[0];
        for (uintptr_t a = ((uintptr_t)p | 0xfff) + 1; a - (uintptr_t)p < n; a += 0x1000) (void)b[a - (uintptr_t)p];
        (void)b[n - 1];
        __asm__ __volatile__("" : : : "memory");
    } else {
        ok = false;                                            // (the signal handler ran with the kernel's FPU defaults)
        __asm__ __volatile__("fnclex\n\tfldcw %0" : : "m"(cw));
    }
    t_probe = prev;
    return ok;
}
#endif
