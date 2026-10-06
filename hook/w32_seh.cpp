// w32_seh.cpp -- the exception stand-ins (w32_kernel.h): SetUnhandledExceptionFilter, UnhandledExceptionFilter,
// RaiseException, RtlUnwind. On Windows they are Windows' own: structured exception handling needs the OS (the
// dispatcher walks the fs:[0] chain the game's frames build, with CONTEXTs the kernel captures). Three are jumps
// straight into KERNEL32 -- no frame of their own, so what Windows captures (RaiseException's ExceptionAddress, the
// caller's registers; RtlUnwind's return context) is exactly what it captures when race.exe calls it. On Linux: R2b.
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
#error "R2b: SEH on Linux -- a TIB per thread behind fs, signals -> EXCEPTION_RECORD/CONTEXT, an fs:[0] chain dispatcher, RaiseException/RtlUnwind/UnhandledExceptionFilter on it (see the notes above)"
#endif
