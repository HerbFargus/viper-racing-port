// msvc_compat.h -- what MSVC (and mingw's GCC) give every source file without an #include, for the native Linux build
// (relink stage R2b). tools/build_linux.sh force-includes it (g++ -include msvc_compat.h) into every file, as mingw's
// GCC predefines __stdcall & co. and its _mingw.h (pulled in by any C header) defines __forceinline & co.; the Windows
// builds never see it.
//   * the calling conventions (__cdecl, __stdcall, __fastcall, __thiscall: GCC's i386 attributes -- the same ABI as
//     Windows' for every argument the port passes), __forceinline, __declspec(naked / noinline / thread / dllexport /
//     dllimport / align(n) / noreturn / selectany / novtable), __int8..__int64
//   * MSVC's intrinsics the port calls (mingw's <intrin.h>): __debugbreak, _ReturnAddress, _AddressOfReturnAddress,
//     __readfsdword / __writefsdword (fs: is the thread's TIB on Linux too -- agent C's w32_seh.cpp sets it up),
//     __stosb / __stosd / __movsb / __movsw / __movsd (rep stos / rep movs, as MSVC emits them), __cpuid, __rdtsc,
//     _mm_pause,
//     _InterlockedExchange / _InterlockedCompareExchange / _InterlockedIncrement / _InterlockedDecrement, _alloca
//   * the MSVC C runtime names the port calls that glibc spells differently: _snprintf / _vsnprintf with MSVCRT's
//     semantics (mingw links msvcrt.dll's: on overflow no terminator and -1, exactly count characters and no
//     terminator when it just fits), _stricmp / _strnicmp. (The Windows-only CRT calls -- _get_osfhandle, _commit,
//     _chsize_s, _mkdir ... -- stay in #ifdef _WIN32 code.) Only C headers that define no short macros are included
//     here (no <unistd.h>: its W_OK / R_OK would collide with the port's own names).
// Nothing here is a Windows API type: those are win32_compat.h's (hook/linux_inc/windows.h includes it in place of
// <windows.h>), so a file that doesn't include <windows.h> on Windows doesn't see them on Linux either.
// (It also declares vp_r2b_todo, linux_todo.h, for R2b's placeholders.)
//
// What isn't Windows': wchar_t is 32-bit here (the port's 16-bit wide strings are WCHAR = uint16_t, never wchar_t);
// long double is the x87's 80 bits, as mingw's (MSVC's is double); and three i386 System V ABI points differ from
// Windows' -- checked over the whole port (R2b agent A's report: every class's size and alignment under mingw and
// Linux GCC compared, -Waggregate-return):
//   - a double or 64-bit integer inside a struct is aligned to 4 here, 8 on Windows: no layout the game shares differs
//     (VP_WIN_ALIGN8 where win32_compat.h needs Windows' alignment: LARGE_INTEGER);
//   - a function returning a small struct returns it through a hidden pointer here, in eax:edx on Windows: no function
//     the game calls, or that calls the game, returns a struct;
//   - Linux GCC assumes a 16-byte aligned stack at every call; the game's code keeps only 4 (system libraries built
//     with SSE -- SDL2 -- may need the port to realign: -mstackrealign; see the report).
#pragma once
#if defined(_WIN32)
#error "msvc_compat.h is for the Linux build only"
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <alloca.h>
#include <x86intrin.h>                                  // __rdtsc, _mm_pause (as mingw's <intrin.h>)

// ---- calling conventions and declaration specifiers -------------------------------------------------------------------
#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
#ifndef __stdcall
#define __stdcall __attribute__((stdcall))
#endif
#ifndef __fastcall
#define __fastcall __attribute__((fastcall))
#endif
#ifndef __thiscall
#define __thiscall __attribute__((thiscall))
#endif
#ifndef _cdecl
#define _cdecl __cdecl
#endif
#ifndef _stdcall
#define _stdcall __stdcall
#endif
#ifndef _fastcall
#define _fastcall __fastcall
#endif
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
// __declspec(x) -> __vp_declspec_x: each one the port uses, as mingw's GCC reads it
#define __declspec(x) __vp_declspec_##x
#define __vp_declspec_naked __attribute__((naked))
#define __vp_declspec_noinline __attribute__((noinline))
#define __vp_declspec_noreturn __attribute__((noreturn))
#define __vp_declspec_thread __thread
#define __vp_declspec_dllexport __attribute__((visibility("default")))
#define __vp_declspec_dllimport
#define __vp_declspec_selectany __attribute__((weak))
#define __vp_declspec_novtable
#define __vp_declspec_align(n) __attribute__((aligned(n)))
#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long

// ---- intrinsics (MSVC's <intrin.h>; mingw's has the same, as inline asm) ---------------------------------------------------
#define __debugbreak() __asm__ __volatile__("int3")
#define _ReturnAddress() __builtin_return_address(0)
#define _AddressOfReturnAddress() ((void*)((char*)__builtin_frame_address(0) + sizeof(void*)))
#define _alloca(n) __builtin_alloca(n)
static inline __attribute__((always_inline)) unsigned long __readfsdword(unsigned long offset) {
    unsigned long v;
    __asm__ __volatile__("mov %0, DWORD PTR fs:[%1]" : "=r"(v) : "r"(offset) : "memory");
    return v;
}
static inline __attribute__((always_inline)) void __writefsdword(unsigned long offset, unsigned long v) {
    __asm__ __volatile__("mov DWORD PTR fs:[%0], %1" : : "r"(offset), "r"(v) : "memory");
}
static inline __attribute__((always_inline)) void __stosb(unsigned char* d, unsigned char v, size_t n) {
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}
static inline __attribute__((always_inline)) void __stosd(unsigned long* d, unsigned long v, size_t n) {
    __asm__ __volatile__("rep stosd" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}
static inline __attribute__((always_inline)) void __movsb(unsigned char* d, const unsigned char* s, size_t n) {
    __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static inline __attribute__((always_inline)) void __movsw(unsigned short* d, const unsigned short* s, size_t n) {
    __asm__ __volatile__("rep movsw" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static inline __attribute__((always_inline)) void __movsd(unsigned long* d, const unsigned long* s, size_t n) {
    __asm__ __volatile__("rep movsd" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static inline __attribute__((always_inline)) void __cpuid(int regs[4], int leaf) {
    __asm__ __volatile__("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]) : "a"(leaf), "c"(0));
}
// the Interlocked intrinsics: a full barrier each (lock-prefixed), as MSVC's
static inline long _InterlockedExchange(volatile long* p, long v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline long _InterlockedCompareExchange(volatile long* p, long v, long cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
static inline long _InterlockedIncrement(volatile long* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline long _InterlockedDecrement(volatile long* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline long _InterlockedExchangeAdd(volatile long* p, long v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

// ---- the MSVC C runtime's names --------------------------------------------------------------------------------------------
// _vsnprintf / _snprintf as msvcrt.dll's: the characters written, or -1 when the output didn't fit (count characters
// written, no terminator); exactly count characters: count returned, no terminator
static inline int _vsnprintf(char* buf, size_t count, const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, count, fmt, ap);
    if (n >= 0 && (size_t)n >= count && count) {
        char* full = (char*)malloc((size_t)n + 1);
        if (full) {
            vsnprintf(full, (size_t)n + 1, fmt, ap2);
            memcpy(buf, full, count);
            free(full);
        }
    }
    va_end(ap2);
    if (n < 0) return -1;
    if ((size_t)n > count) return -1;
    return n;
}
static inline int _snprintf(char* buf, size_t count, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static inline int _snprintf(char* buf, size_t count, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, count, fmt, ap);
    va_end(ap);
    return n;
}
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

// ---- R2b's placeholders (vp_r2b_todo), visible everywhere in the Linux build until agents B/C/D replace them -------------
#include "linux_todo.h"
