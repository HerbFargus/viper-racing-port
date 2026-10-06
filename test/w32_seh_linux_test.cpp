// w32_seh_linux_test.cpp -- the native Linux build's structured exception handling (hook/w32_seh.cpp's #else, relink
// stage R2b) against what Windows does: the thread block behind fs, frames on the fs:[0] chain, RaiseException
// continued, RtlUnwind (handlers called with EXCEPTION_UNWINDING, unlinked, registers kept), faults (SIGSEGV, SIGFPE,
// SIGTRAP) as Windows' exception records and CONTEXTs and resumed with a changed CONTEXT, int3 through a vectored
// handler (standalone.cpp's trap, both ways: skip it, and return to the caller of a filled function), the x87's
// unmasked exceptions (divide by zero, invalid, overflow, stack fault: codes, ExceptionAddress at the x87
// instruction, the FPU state in FloatSave and back), divide errors told apart as Windows does, nested exceptions
// (EXCEPTION_NESTED_CALL), non-continuable ones, a CreateThread thread's own block and x87 0x27f, the guarded read,
// and the end of the chain (the top-level filter: execute handler -> unwind + ExitProcess; continue; no filter ->
// the process ends with the code) in child processes. Prints one line per failure; exit code = failures.
//
//   tools/test_seh_linux.sh   (WSL: builds with build_linux.sh's flags, runs)
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../hook/w32_kernel.h"
#include "../hook/w32_seh.h"

static int g_fails, g_checks;
static const char* g_section = "";
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                \
            g_fails++;                                                                                                \
            printf("FAIL [%s] line %d: %s -- ", g_section, __LINE__, #cond);                                         \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            fflush(stdout);                                                                                           \
        }                                                                                                             \
    } while (0)

struct Reg {
    Reg* next;
    void* handler;
};
typedef EXCEPTION_DISPOSITION(__cdecl* Handler)(EXCEPTION_RECORD*, void*, CONTEXT*, void*);
static void push_frame(Reg* r, Handler h) {
    r->handler = (void*)h;
    r->next = (Reg*)(uintptr_t)__readfsdword(0);
    __writefsdword(0, (unsigned long)(uintptr_t)r);
}
static void pop_frame(Reg* r) { __writefsdword(0, (unsigned long)(uintptr_t)r->next); }
static uint32_t fs0() { return (uint32_t)__readfsdword(0); }
static uint16_t fpu_cw() {
    uint16_t cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    return cw;
}
static uint16_t fpu_sw() {
    uint16_t sw;
    __asm__ __volatile__("fnstsw %0" : "=m"(sw));
    return sw;
}

// ---- the log of handler calls, for the order checks -------------------------------------------------------------------
static char g_log[512];
static void note(const char* s) {
    if (strlen(g_log) + strlen(s) + 2 < sizeof g_log) {
        strcat(g_log, s);
        strcat(g_log, " ");
    }
}

// ---- asm helpers: RaiseException / RtlUnwind keep the caller's registers ----------------------------------------------
extern "C" { uint32_t vp_test_saved_esp; ULONG WINAPI RemoveVectoredExceptionHandler(PVOID); }
extern "C" int raise_keeps_regs();
extern "C" int unwind_keeps_regs();
__asm__(R"(
    .text
    .globl raise_keeps_regs
raise_keeps_regs:
    push ebp
    push ebx
    push esi
    push edi
    mov dword ptr [vp_test_saved_esp], esp
    mov ebx, 0x11111111
    mov esi, 0x22222222
    mov edi, 0x33333333
    mov ebp, 0x44444444
    push 0
    push 0
    push 0
    push 0xE0000001
    call _Z18w32_RaiseExceptionjjjPKj
    xor eax, eax
    cmp ebx, 0x11111111
    jne rkr_out
    cmp esi, 0x22222222
    jne rkr_out
    cmp edi, 0x33333333
    jne rkr_out
    cmp ebp, 0x44444444
    jne rkr_out
    cmp esp, dword ptr [vp_test_saved_esp]
    jne rkr_out
    mov eax, 1
rkr_out:
    mov esp, dword ptr [vp_test_saved_esp]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

    .globl unwind_keeps_regs
unwind_keeps_regs:
    push ebp
    push ebx
    push esi
    push edi
    mov dword ptr [vp_test_saved_esp], esp
    mov ebx, 0x11111111
    mov esi, 0x22222222
    mov edi, 0x33333333
    mov ebp, 0x44444444
    push 0x5555
    push 0
    push 0
    push dword ptr fs:[0]
    call _Z13w32_RtlUnwindPvS_S_S_
    cmp eax, 0x5555
    mov eax, 0
    jne ukr_out
    cmp ebx, 0x11111111
    jne ukr_out
    cmp esi, 0x22222222
    jne ukr_out
    cmp edi, 0x33333333
    jne ukr_out
    cmp ebp, 0x44444444
    jne ukr_out
    cmp esp, dword ptr [vp_test_saved_esp]
    jne ukr_out
    mov eax, 1
ukr_out:
    mov esp, dword ptr [vp_test_saved_esp]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
)");

// ---- 1. the thread block ------------------------------------------------------------------------------------------------
static void test_tib() {
    g_section = "tib";
    int local = 0;
    CHECK(fs0() == 0xffffffffu, "fs:[0] %08x", fs0());
    const uint32_t self = (uint32_t)__readfsdword(0x18);
    CHECK(self != 0 && *(uint32_t*)(uintptr_t)(self + 0x18) == self, "self %08x", self);
    const uint32_t base = (uint32_t)__readfsdword(4), limit = (uint32_t)__readfsdword(8);
    CHECK(limit < (uint32_t)(uintptr_t)&local && (uint32_t)(uintptr_t)&local < base, "stack %08x..%08x, a local at %p",
          limit, base, (void*)&local);
    CHECK(__readfsdword(0x24) == w32_GetCurrentThreadId(), "tid %lu", __readfsdword(0x24));
    CHECK(__readfsdword(0x20) == (unsigned long)getpid(), "pid %lu", __readfsdword(0x20));
    const uint32_t peb = (uint32_t)__readfsdword(0x30);
    CHECK(peb && *(uint32_t*)(uintptr_t)(peb + 8) == 0x400000, "peb %08x", peb);
    w32_seh_thread_begin();                                    // idempotent: the same block
    CHECK(__readfsdword(0x18) == self, "a second begin made a new block");
}

// ---- 2. RaiseException, continued -------------------------------------------------------------------------------------
static EXCEPTION_RECORD g_seen_rec;
static uint32_t g_seen_frame;
static EXCEPTION_DISPOSITION __cdecl h_continue(EXCEPTION_RECORD* r, void* frame, CONTEXT* c, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    note("continue");
    g_seen_rec = *r;
    g_seen_frame = (uint32_t)(uintptr_t)frame;
    (void)c;
    return ExceptionContinueExecution;
}
static EXCEPTION_DISPOSITION __cdecl h_search(EXCEPTION_RECORD* r, void*, CONTEXT*, void*) {
    note(r->ExceptionFlags & EXCEPTION_UNWIND ? "search-unwind" : "search");
    return ExceptionContinueSearch;
}
static void test_raise() {
    g_section = "raise";
    Reg outer, inner;
    push_frame(&outer, h_continue);
    push_frame(&inner, h_search);
    g_log[0] = 0;
    const ULONG_PTR args[3] = {7, 8, 9};
    w32_RaiseException(0xE0000005, 0, 3, (const uint32_t*)args);
    CHECK(!strcmp(g_log, "search continue "), "handlers: %s", g_log);
    CHECK(g_seen_rec.ExceptionCode == 0xE0000005 && g_seen_rec.ExceptionFlags == 0 && g_seen_rec.NumberParameters == 3 &&
              g_seen_rec.ExceptionInformation[2] == 9,
          "record %08x %x %u", (unsigned)g_seen_rec.ExceptionCode, (unsigned)g_seen_rec.ExceptionFlags,
          (unsigned)g_seen_rec.NumberParameters);
    CHECK(g_seen_frame == (uint32_t)(uintptr_t)&outer, "frame %08x", g_seen_frame);
    CHECK(fs0() == (uint32_t)(uintptr_t)&inner, "fs:[0] after %08x", fs0());
    // 20 arguments: 15 kept; no array: none
    w32_RaiseException(0xE0000006, 0, 20, (const uint32_t*)args);   // (reads past args: as Windows, 15 of them)
    CHECK(g_seen_rec.NumberParameters == 15, "params %u", (unsigned)g_seen_rec.NumberParameters);
    w32_RaiseException(0xE0000006, 0, 3, 0);
    CHECK(g_seen_rec.NumberParameters == 0, "params %u", (unsigned)g_seen_rec.NumberParameters);
    // the Win32 name standalone.cpp calls
    RaiseException(0xE0000007, 0, 0, 0);
    CHECK(g_seen_rec.ExceptionCode == 0xE0000007, "RaiseException: %08x", (unsigned)g_seen_rec.ExceptionCode);
    CHECK(raise_keeps_regs() == 1, "ebx/esi/edi/ebp/esp not as before RaiseException");
    pop_frame(&inner);
    pop_frame(&outer);
    CHECK(fs0() == 0xffffffffu, "fs:[0] %08x", fs0());
}

// ---- 3. RtlUnwind: the __except of an outer frame -----------------------------------------------------------------------
static jmp_buf g_jb;
static uint32_t g_unwind_fs0;
static int g_inner_unwound;
static EXCEPTION_DISPOSITION __cdecl h_inner(EXCEPTION_RECORD* r, void*, CONTEXT*, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWINDING) {
        g_inner_unwound++;
        note("inner-unwind");
    } else {
        note("inner");
    }
    return ExceptionContinueSearch;
}
static EXCEPTION_DISPOSITION __cdecl h_except(EXCEPTION_RECORD* r, void* frame, CONTEXT*, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) {
        note("except-unwind");                                 // (never: RtlUnwind stops at its target)
        return ExceptionContinueSearch;
    }
    if (r->ExceptionCode != 0xE0000010) return ExceptionContinueSearch;
    note("except");
    g_seen_rec = *r;
    w32_RtlUnwind(frame, 0, 0, 0);
    g_unwind_fs0 = fs0();
    longjmp(g_jb, 1);
}
// (frames of a deeper function: below the caller's on the stack, as every real __try's)
__attribute__((noinline)) static void raise_in_frame(Handler h, uint32_t code, uint32_t flags) {
    Reg inner;
    push_frame(&inner, h);
    w32_RaiseException(code, flags, 0, 0);
    CHECK(0, "RaiseException %08x returned", code);
    pop_frame(&inner);
}
static void test_unwind() {
    g_section = "unwind";
    Reg outer;
    g_log[0] = 0;
    g_inner_unwound = 0;
    push_frame(&outer, h_except);
    if (!setjmp(g_jb)) raise_in_frame(h_inner, 0xE0000010, 0);
    CHECK(!strcmp(g_log, "inner except inner-unwind "), "handlers: %s", g_log);
    CHECK(g_unwind_fs0 == (uint32_t)(uintptr_t)&outer, "fs:[0] after RtlUnwind %08x (outer %p)", g_unwind_fs0, (void*)&outer);
    pop_frame(&outer);
    CHECK(fs0() == 0xffffffffu, "fs:[0] %08x", fs0());
    CHECK(unwind_keeps_regs() == 1, "RtlUnwind to the head: eax not the return value, or a register changed");
}

// ---- 4. faults resumed with a changed CONTEXT ---------------------------------------------------------------------------
static volatile uint32_t g_target;
static uint32_t g_av_info[2];
static uint32_t g_fault_cw;
static EXCEPTION_DISPOSITION __cdecl h_fixup(EXCEPTION_RECORD* r, void*, CONTEXT* c, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return ExceptionContinueSearch;
    g_av_info[0] = r->ExceptionInformation[0];
    g_av_info[1] = r->ExceptionInformation[1];
    g_fault_cw = fpu_cw();
    if (c->Eax == 0x10) c->Eax = (DWORD)(uintptr_t)&g_target;  // the write: retried at a good address
    else c->Ecx = (DWORD)(uintptr_t)&g_target;                 // the read
    return ExceptionContinueExecution;
}
static EXCEPTION_DISPOSITION __cdecl h_av_except(EXCEPTION_RECORD* r, void* frame, CONTEXT*, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return ExceptionContinueSearch;
    g_seen_rec = *r;
    w32_RtlUnwind(frame, 0, 0, 0);
    longjmp(g_jb, 1);
}
static void test_faults() {
    g_section = "fault";
    Reg f;
    push_frame(&f, h_fixup);
    g_target = 0;
    __asm__ __volatile__("mov eax, 0x10\n\tmov dword ptr [eax], 0x1234" : : : "eax", "memory");
    CHECK(g_target == 0x1234, "the write wasn't retried: %08x", g_target);
    CHECK(g_av_info[0] == 1 && g_av_info[1] == 0x10, "write: info %u %08x", g_av_info[0], g_av_info[1]);
    CHECK(g_fault_cw == 0x27f, "the handler ran with x87 control word %04x", g_fault_cw);
    uint32_t v;
    __asm__ __volatile__("mov ecx, 0x20\n\tmov eax, dword ptr [ecx]" : "=a"(v) : : "ecx", "memory");
    CHECK(v == 0x1234, "the read wasn't retried: %08x", v);
    CHECK(g_av_info[0] == 0 && g_av_info[1] == 0x20, "read: info %u %08x", g_av_info[0], g_av_info[1]);
    pop_frame(&f);
    // a fault in a __try: the __except runs (RtlUnwind + jump), and the next fault is caught the same way
    for (int round = 0; round < 3; round++) {
        Reg t;
        push_frame(&t, h_av_except);
        if (!setjmp(g_jb)) {
            __asm__ __volatile__("mov dword ptr [%0], 1" : : "r"(0x30 + round) : "memory");   // (a barrier: no CHECK hoisted above)
            CHECK(0, "no fault");
        }
        CHECK(g_seen_rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_seen_rec.ExceptionInformation[1] == 0x30u + round,
              "round %d: %08x at %08x", round, (unsigned)g_seen_rec.ExceptionCode, (unsigned)g_seen_rec.ExceptionInformation[1]);
        CHECK(fs0() == (uint32_t)(uintptr_t)&t, "round %d: fs:[0] %08x", round, fs0());
        pop_frame(&t);
    }
    // execute: a jump to an unmapped page -> info[0] 8? (no: NX off the map is a read of the instruction: Windows says 8
    // only for a DEP refusal of mapped memory) -- a mapped no-exec page
    void* page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(page, 0xc3, 4096);
    {
        Reg t;
        push_frame(&t, h_av_except);
        if (!setjmp(g_jb)) {
            ((void (*)())page)();
            CHECK(0, "executed a non-executable page");
        }
        CHECK(g_seen_rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_seen_rec.ExceptionInformation[0] == 8 &&
                  g_seen_rec.ExceptionInformation[1] == (ULONG_PTR)(uintptr_t)page,
              "exec: %08x info %u %08x", (unsigned)g_seen_rec.ExceptionCode, (unsigned)g_seen_rec.ExceptionInformation[0],
              (unsigned)g_seen_rec.ExceptionInformation[1]);
        pop_frame(&t);
    }
    munmap(page, 4096);
}

// ---- 5. int3 through a vectored handler ---------------------------------------------------------------------------------
static uint32_t g_bp_addr, g_bp_eip;
static int g_bp_mode;                                          // 0: skip the int3; 1: return to the caller (standalone's)
static int g_frame_saw_bp;
static LONG __stdcall veh_int3(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    note("veh");
    g_bp_addr = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    g_bp_eip = e->ContextRecord->Eip;
    CONTEXT* c = e->ContextRecord;
    if (g_bp_mode == 0) {
        c->Eip += 1;
    } else {
        const uint32_t* sp = (const uint32_t*)(uintptr_t)c->Esp;
        c->Eip = sp[0];
        c->Esp += 4;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
static LONG __stdcall veh_second(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) note("veh2");
    return EXCEPTION_CONTINUE_SEARCH;
}
static EXCEPTION_DISPOSITION __cdecl h_no_bp(EXCEPTION_RECORD* r, void*, CONTEXT*, void*) {
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT) g_frame_saw_bp++;
    return ExceptionContinueSearch;
}
static void test_int3() {
    g_section = "int3";
    void* h2 = AddVectoredExceptionHandler(0, veh_second);
    void* h1 = AddVectoredExceptionHandler(1, veh_int3);       // first: before veh_second
    CHECK(h1 && h2, "AddVectoredExceptionHandler");
    Reg f;
    push_frame(&f, h_no_bp);
    g_log[0] = 0;
    g_bp_mode = 0;
    uint32_t at;
    __asm__ __volatile__("mov %0, offset vp_test_int3_here\n\tvp_test_int3_here: int3" : "=r"(at) : : "memory");
    CHECK(!strcmp(g_log, "veh "), "handlers: %s", g_log);
    CHECK(g_bp_addr == at && g_bp_eip == at, "ExceptionAddress %08x, Eip %08x, the int3 at %08x", g_bp_addr, g_bp_eip, at);
    CHECK(g_frame_saw_bp == 0, "a frame handler saw a breakpoint the vectored handler continued");
    // standalone.cpp's self-test: call a function filled with int3, the handler returns to the caller
    uint8_t* code = (uint8_t*)mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(code, 0xcc, 4096);
    g_bp_mode = 1;
    ((void(__cdecl*)(void*))code)(0);
    CHECK(g_bp_addr == (uint32_t)(uintptr_t)code, "filled function: ExceptionAddress %08x, code at %p", g_bp_addr, (void*)code);
    munmap(code, 4096);
    CHECK(RemoveVectoredExceptionHandler(h1) == 1, "remove");
    pop_frame(&f);
    CHECK(RemoveVectoredExceptionHandler(h2) == 1, "remove");
    CHECK(RemoveVectoredExceptionHandler(h2) == 0, "remove twice");
}

// ---- 6. the x87's unmasked exceptions -----------------------------------------------------------------------------------
static uint32_t g_fp_code, g_fp_addr, g_fp_ctx_cw, g_fp_ctx_sw, g_fp_live_cw, g_fp_live_sw, g_fp_eip;
static EXCEPTION_DISPOSITION __cdecl h_fpu(EXCEPTION_RECORD* r, void*, CONTEXT* c, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    g_fp_code = r->ExceptionCode;
    g_fp_addr = (uint32_t)(uintptr_t)r->ExceptionAddress;
    g_fp_eip = c->Eip;
    g_fp_ctx_cw = c->FloatSave.ControlWord & 0xffff;
    g_fp_ctx_sw = c->FloatSave.StatusWord & 0xffff;
    g_fp_live_cw = fpu_cw();
    g_fp_live_sw = fpu_sw();
    CHECK(c->ContextFlags & CONTEXT_FLOATING_POINT & 0xff, "no FPU state in the CONTEXT");
    c->FloatSave.StatusWord &= ~0x80ffu;                       // what a handler that continues must do (as on Windows)
    c->FloatSave.ControlWord = 0x27f;
    return ExceptionContinueExecution;
}
static void test_x87() {
    g_section = "x87";
    Reg f;
    push_frame(&f, h_fpu);
    // divide by zero, ZM unmasked: reported at the next x87 instruction, ExceptionAddress the fdiv's
    uint32_t at;
    uint16_t after_cw;
    uint16_t cw = 0x27b;
    __asm__ __volatile__(
        "fldcw %2\n\t"
        "fld1\n\t"
        "fldz\n\t"
        "mov %0, offset vp_test_fdiv_here\n\t"
        "vp_test_fdiv_here: fdivp st(1), st\n\t"
        "fwait\n\t"
        "fnstcw %1\n\t"
        "fninit\n\t"
        : "=&r"(at), "=m"(after_cw)
        : "m"(cw)
        : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    {
        uint16_t d = 0x27f;
        __asm__ __volatile__("fldcw %0" : : "m"(d));
    }
    CHECK(g_fp_code == EXCEPTION_FLT_DIVIDE_BY_ZERO, "code %08x", g_fp_code);
    CHECK(g_fp_addr == at, "ExceptionAddress %08x, the fdiv at %08x", g_fp_addr, at);
    CHECK(g_fp_eip != at && g_fp_eip - at < 8, "Eip %08x (the fwait after %08x)", g_fp_eip, at);
    CHECK(g_fp_ctx_cw == 0x27b && (g_fp_ctx_sw & 0x84) == 0x84, "FloatSave cw %04x sw %04x", g_fp_ctx_cw, g_fp_ctx_sw);
    CHECK(g_fp_live_cw == 0x27b && (g_fp_live_sw & 0x80bf) == 0, "the dispatcher's FPU: cw %04x sw %04x", g_fp_live_cw,
          g_fp_live_sw);
    CHECK((after_cw & 0xffff) == 0x27f, "continued with control word %04x (the handler set 0x27f)", after_cw & 0xffff);

    // invalid operation: sqrt(-1), IM unmasked
    cw = 0x27e;
    g_fp_code = 0;
    __asm__ __volatile__(
        "fldcw %1\n\t"
        "fld1\n\t"
        "fchs\n\t"
        "mov %0, offset vp_test_fsqrt_here\n\t"
        "vp_test_fsqrt_here: fsqrt\n\t"
        "fwait\n\t"
        "fninit\n\t"
        : "=&r"(at)
        : "m"(cw)
        : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    {
        uint16_t d = 0x27f;
        __asm__ __volatile__("fldcw %0" : : "m"(d));
    }
    CHECK(g_fp_code == EXCEPTION_FLT_INVALID_OPERATION && g_fp_addr == at, "sqrt(-1): %08x at %08x (fsqrt at %08x)",
          g_fp_code, g_fp_addr, at);

    // overflow: a double that doesn't fit, OM unmasked
    cw = 0x277;
    g_fp_code = 0;
    double out = 0;
    static const double big = 1e300;
    __asm__ __volatile__(
        "fldcw %2\n\t"
        "fld %3\n\t"
        "fmul st, st\n\t"
        "mov %0, offset vp_test_fst_here\n\t"
        "vp_test_fst_here: fstp %1\n\t"
        "fwait\n\t"
        "fninit\n\t"
        : "=&r"(at), "=m"(out)
        : "m"(cw), "m"(big)
        : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    {
        uint16_t d = 0x27f;
        __asm__ __volatile__("fldcw %0" : : "m"(d));
    }
    CHECK(g_fp_code == EXCEPTION_FLT_OVERFLOW && g_fp_addr == at, "overflow: %08x at %08x (fstp at %08x)", g_fp_code,
          g_fp_addr, at);

    // stack fault: a ninth push, IM unmasked -> STATUS_FLOAT_STACK_CHECK
    cw = 0x27e;
    g_fp_code = 0;
    __asm__ __volatile__(
        "fninit\n\t"
        "fldcw %0\n\t"
        "fld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\t"
        "fld1\n\t"
        "fwait\n\t"
        "fninit\n\t"
        :
        : "m"(cw)
        : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    {
        uint16_t d = 0x27f;
        __asm__ __volatile__("fldcw %0" : : "m"(d));
    }
    CHECK(g_fp_code == EXCEPTION_FLT_STACK_CHECK, "stack fault: %08x", g_fp_code);
    pop_frame(&f);
    CHECK(fpu_cw() == 0x27f, "cw %04x", fpu_cw());
}

// ---- 7. divide errors: zero vs overflow, as Windows decodes them -----------------------------------------------------
static uint32_t g_div_code;
static EXCEPTION_DISPOSITION __cdecl h_div(EXCEPTION_RECORD* r, void*, CONTEXT* c, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    g_div_code = r->ExceptionCode;
    c->Eip = c->Esi;                                           // (each test puts its resume address in esi)
    return ExceptionContinueExecution;
}
static void test_divide() {
    g_section = "divide";
    Reg f;
    push_frame(&f, h_div);
    __asm__ __volatile__("mov esi, offset vp_d1\n\tmov eax, 1\n\txor edx, edx\n\txor ecx, ecx\n\tdiv ecx\nvp_d1:"
                         : : : "eax", "ecx", "edx", "esi", "memory");
    CHECK(g_div_code == EXCEPTION_INT_DIVIDE_BY_ZERO, "div ecx (0): %08x", g_div_code);
    g_div_code = 0;
    __asm__ __volatile__("mov esi, offset vp_d2\n\tmov eax, 0x80000000\n\tcdq\n\tmov ecx, -1\n\tidiv ecx\nvp_d2:"
                         : : : "eax", "ecx", "edx", "esi", "memory");
    CHECK(g_div_code == EXCEPTION_INT_OVERFLOW, "idiv INT_MIN/-1: %08x", g_div_code);
    static uint32_t ops[2] = {5, 0};
    g_div_code = 0;
    __asm__ __volatile__("mov esi, offset vp_d3\n\tmov ebx, %0\n\tmov eax, 1\n\txor edx, edx\n\tdiv dword ptr [ebx + 4]\nvp_d3:"
                         : : "r"(ops) : "eax", "ebx", "edx", "esi", "memory");
    CHECK(g_div_code == EXCEPTION_INT_DIVIDE_BY_ZERO, "div [ebx+4] (0): %08x", g_div_code);
    g_div_code = 0;
    __asm__ __volatile__("mov esi, offset vp_d4\n\tmov ebx, %0\n\tmov eax, 0\n\tmov edx, 7\n\tdiv dword ptr [ebx]\nvp_d4:"
                         : : "r"(ops) : "eax", "ebx", "edx", "esi", "memory");
    CHECK(g_div_code == EXCEPTION_INT_OVERFLOW, "div [ebx] (7:0 / 5): %08x", g_div_code);
    g_div_code = 0;
    __asm__ __volatile__("mov esi, offset vp_d5\n\tmov ax, 0x100\n\txor cl, cl\n\tdiv cl\nvp_d5:"
                         : : : "eax", "ecx", "esi", "memory");
    CHECK(g_div_code == EXCEPTION_INT_DIVIDE_BY_ZERO, "div cl (0): %08x", g_div_code);
    pop_frame(&f);
}

// ---- 8. nested exceptions -------------------------------------------------------------------------------------------------
static uint32_t g_nested_flags_inner, g_nested_flags_outer;
static int g_nested_inner_calls;
static EXCEPTION_DISPOSITION __cdecl h_nest_inner(EXCEPTION_RECORD* r, void*, CONTEXT* c, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    g_nested_inner_calls++;
    if (r->ExceptionCode == 0xE0000020) {
        // a fault while handling: dispatched through the guard frame -> NESTED_CALL for the frames up to this one
        __asm__ __volatile__("mov esi, offset vp_test_n1\n\tmov dword ptr ds:[0x40], 1\nvp_test_n1:" : : : "esi", "memory");
        return ExceptionContinueSearch;
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        g_nested_flags_inner = r->ExceptionFlags;
        c->Eip = c->Esi;                                       // past the faulting mov
        return ExceptionContinueExecution;
    }
    return ExceptionContinueSearch;
}
static EXCEPTION_DISPOSITION __cdecl h_nest_outer(EXCEPTION_RECORD* r, void*, CONTEXT*, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    if (r->ExceptionCode == 0xE0000020) {
        g_nested_flags_outer = r->ExceptionFlags;
        return ExceptionContinueExecution;
    }
    return ExceptionContinueSearch;
}
static void test_nested() {
    g_section = "nested";
    Reg outer, inner;
    push_frame(&outer, h_nest_outer);
    push_frame(&inner, h_nest_inner);
    g_nested_flags_inner = g_nested_flags_outer = 0xdead;
    w32_RaiseException(0xE0000020, 0, 0, 0);
    CHECK(g_nested_flags_inner == EXCEPTION_NESTED_CALL, "the fault inside the handler: flags %x", g_nested_flags_inner);
    CHECK(g_nested_flags_outer == 0, "the first exception at the outer frame: flags %x", g_nested_flags_outer);
    CHECK(g_nested_inner_calls == 2, "inner handler calls %d", g_nested_inner_calls);
    pop_frame(&inner);
    pop_frame(&outer);
}

// ---- 9. non-continuable -----------------------------------------------------------------------------------------------------
static EXCEPTION_DISPOSITION __cdecl h_nc_inner(EXCEPTION_RECORD* r, void*, CONTEXT*, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    if (r->ExceptionCode == 0xE0000030) return ExceptionContinueExecution;   // not allowed
    return ExceptionContinueSearch;
}
static uint32_t g_nc_code, g_nc_cause;
static EXCEPTION_DISPOSITION __cdecl h_nc_outer(EXCEPTION_RECORD* r, void* frame, CONTEXT*, void*) {
    if (r->ExceptionFlags & EXCEPTION_UNWIND) return ExceptionContinueSearch;
    if (r->ExceptionCode != STATUS_NONCONTINUABLE_EXCEPTION) return ExceptionContinueSearch;
    g_nc_code = r->ExceptionCode;
    g_nc_cause = r->ExceptionRecord ? r->ExceptionRecord->ExceptionCode : 0;
    w32_RtlUnwind(frame, 0, 0, 0);
    longjmp(g_jb, 1);
}
static void test_noncontinuable() {
    g_section = "noncontinuable";
    Reg outer;
    push_frame(&outer, h_nc_outer);
    if (!setjmp(g_jb)) raise_in_frame(h_nc_inner, 0xE0000030, EXCEPTION_NONCONTINUABLE);
    CHECK(g_nc_code == STATUS_NONCONTINUABLE_EXCEPTION && g_nc_cause == 0xE0000030, "%08x caused by %08x", g_nc_code,
          g_nc_cause);
    CHECK(fs0() == (uint32_t)(uintptr_t)&outer, "fs:[0] %08x", fs0());
    pop_frame(&outer);
}

// ---- 10. a CreateThread thread ---------------------------------------------------------------------------------------------
static uint32_t g_main_teb;
static uint32_t __attribute__((stdcall)) thread_proc(void*) {
    g_section = "thread";
    int local;
    CHECK(fs0() == 0xffffffffu, "fs:[0] %08x", fs0());
    const uint32_t self = (uint32_t)__readfsdword(0x18);
    CHECK(self && self != g_main_teb, "the thread's block %08x (main's %08x)", self, g_main_teb);
    CHECK((uint32_t)__readfsdword(8) < (uint32_t)(uintptr_t)&local && (uint32_t)(uintptr_t)&local < (uint32_t)__readfsdword(4),
          "stack bounds");
    CHECK(__readfsdword(0x24) == w32_GetCurrentThreadId(), "tid");
    CHECK(fpu_cw() == 0x27f, "the thread's x87 control word %04x", fpu_cw());
    test_raise();
    test_faults();
    test_unwind();
    g_section = "thread";
    return 77;
}
static void test_thread() {
    g_main_teb = (uint32_t)__readfsdword(0x18);
    uint16_t cw = 0x37f;                                       // the creator's isn't the new thread's
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    uint32_t id = 0;
    const uint32_t h = w32_CreateThread(0, 0, thread_proc, 0, 0, &id);
    cw = 0x27f;
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    g_section = "thread";
    CHECK(h != 0, "CreateThread");
    CHECK(w32_WaitForSingleObject(h, 10000) == 0, "the thread didn't end");
    uint32_t code = 0;
    w32_GetExitCodeThread(h, &code);
    CHECK(code == 77, "exit code %u", code);
    CHECK((uint32_t)__readfsdword(0x18) == g_main_teb, "main's block changed");
    // another thread after the first ended (its block freed): fine too
    const uint32_t h2 = w32_CreateThread(0, 0, thread_proc, 0, 0, &id);
    CHECK(h2 && w32_WaitForSingleObject(h2, 10000) == 0, "a second thread");
}

// ---- 11. the guarded read --------------------------------------------------------------------------------------------------
static void test_readable() {
    g_section = "readable";
    int local = 5;
    uint16_t cw = 0x27f;
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    CHECK(w32_seh_readable(&local, sizeof local), "a local");
    CHECK(!w32_seh_readable((void*)0x10, 4), "page 0");
    CHECK(fpu_cw() == 0x27f, "control word after a failed probe %04x", fpu_cw());
    CHECK(w32_seh_readable((void*)0x10, 0), "0 bytes");
    void* p = mmap(0, 8192, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap((char*)p + 4096, 4096);
    CHECK(w32_seh_readable(p, 4096), "a page");
    CHECK(!w32_seh_readable((char*)p + 4000, 200), "across into an unmapped page");
    munmap(p, 4096);
    // after the probes, faults still dispatch
    test_faults();
}

// ---- 12. the end of the chain: the top-level filter, in a child process ---------------------------------------------------
static int g_pipe;
static void say(const char* s) { (void)!write(g_pipe, s, strlen(s)); }
static LONG __stdcall filter_execute(EXCEPTION_POINTERS* e) {
    say(e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "filter " : "filter? ");
    return EXCEPTION_EXECUTE_HANDLER;
}
static LONG __stdcall filter_continue(EXCEPTION_POINTERS* e) {
    say("filter ");
    e->ContextRecord->Eip = e->ContextRecord->Esi;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static EXCEPTION_DISPOSITION __cdecl h_note_unwind(EXCEPTION_RECORD* r, void*, CONTEXT*, void*) {
    say(r->ExceptionFlags & EXCEPTION_UNWINDING ? "unwound " : "searched ");
    return ExceptionContinueSearch;
}
static void at_exit_note() { say("atexit "); }
// run fn in a child; its exit status and what it said
static void child(const char* name, void (*fn)(), int want_status, const char* want_said) {
    g_section = name;
    int fds[2];
    if (pipe(fds)) return;
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        g_pipe = fds[1];
        fn();
        say("returned ");
        _exit(0);
    }
    close(fds[1]);
    char said[256];
    int n = 0, k;
    while (n < 255 && (k = (int)read(fds[0], said + n, 255 - n)) > 0) n += k;
    said[n] = 0;
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    const int status = WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
    CHECK(status == want_status, "exit status %d, wanted %d", status, want_status);
    CHECK(!strcmp(said, want_said), "said \"%s\", wanted \"%s\"", said, want_said);
}
static void c_filter_execute() {
    atexit(at_exit_note);
    w32_SetUnhandledExceptionFilter((void*)filter_execute);
    Reg f;
    push_frame(&f, h_note_unwind);
    *(volatile int*)(uintptr_t)0x50 = 1;
}
static void c_filter_continue() {
    w32_SetUnhandledExceptionFilter((void*)filter_continue);
    __asm__ __volatile__("mov esi, offset vp_c1\n\tmov dword ptr ds:[0x50], 1\nvp_c1:" : : : "esi", "memory");
    say("resumed ");
}
static void c_no_filter() {
    atexit(at_exit_note);
    Reg f;
    push_frame(&f, h_note_unwind);
    __asm__ __volatile__("int3");
}
static void c_raise_unhandled() {
    void* old = w32_SetUnhandledExceptionFilter((void*)filter_execute);
    CHECK(old == 0, "a filter before");
    void* prev = w32_SetUnhandledExceptionFilter(0);           // (the game's restore at exit)
    if (prev != (void*)filter_execute) say("wrong-previous ");
    w32_RaiseException(0xE0000042, 0, 0, 0);
}
static uint32_t __attribute__((stdcall)) thread_fault(void*) {
    *(volatile int*)(uintptr_t)0x60 = 1;
    return 0;
}
static void c_thread_unhandled() {
    w32_SetUnhandledExceptionFilter((void*)filter_execute);
    uint32_t id;
    const uint32_t h = w32_CreateThread(0, 0, thread_fault, 0, 0, &id);
    w32_WaitForSingleObject(h, 10000);
    say("not-ended ");
}
static void test_unhandled() {
    // ExitProcess(code) after the global unwind: exit status = the code's low byte (0xC0000005 -> 5); atexit runs
    child("unhandled: filter executes", c_filter_execute, 5, "searched filter unwound atexit ");
    child("unhandled: filter continues", c_filter_continue, 0, "filter resumed returned ");
    // no filter: the process ends at once (no exit work), with the code (0x80000003 -> 3)
    child("unhandled: no filter", c_no_filter, 3, "searched ");
    child("unhandled: raised, no filter", c_raise_unhandled, 0x42, "");
    child("unhandled: in a thread", c_thread_unhandled, 5, "filter ");
}

int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    w32_seh_thread_begin();                                    // (the loader's call)
    uint16_t cw = 0x27f;                                       // (and its control word)
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    printf("-- test_tib (%d failed so far)\n", g_fails);
    test_tib();
    printf("-- test_raise (%d failed so far)\n", g_fails);
    test_raise();
    printf("-- test_unwind (%d failed so far)\n", g_fails);
    test_unwind();
    printf("-- test_faults (%d failed so far)\n", g_fails);
    test_faults();
    printf("-- test_int3 (%d failed so far)\n", g_fails);
    test_int3();
    printf("-- test_x87 (%d failed so far)\n", g_fails);
    test_x87();
    printf("-- test_divide (%d failed so far)\n", g_fails);
    test_divide();
    printf("-- test_nested (%d failed so far)\n", g_fails);
    test_nested();
    printf("-- test_noncontinuable (%d failed so far)\n", g_fails);
    test_noncontinuable();
    printf("-- test_thread (%d failed so far)\n", g_fails);
    test_thread();
    printf("-- test_readable (%d failed so far)\n", g_fails);
    test_readable();
    printf("-- test_unhandled (%d failed so far)\n", g_fails);
    test_unhandled();
    printf("w32_seh_linux_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails;
}
