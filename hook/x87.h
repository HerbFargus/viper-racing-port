// x87.h -- the few FPU instructions the 1998 physics uses directly, for rewrites that must match it bit for
// bit. The physics thread runs the x87 in single precision (physics_thread -> ExceptSinglePrecision(1),
// every tick), so each operation rounds to a float's 24-bit mantissa; this DLL is built /arch:IA32, so
// its own float arithmetic runs on the same x87 in the same mode. What a rewrite must reproduce is the
// order of operations (the grouping of each sum), the width of each constant (a double constant in a
// multiply is not a float one), and these instructions, which only the x87 has.
#pragma once
#include <stdint.h>
#include "compiler.h"

// A value the original keeps in an x87 register has the register's exponent range: single precision
// rounds only the mantissa, so a sum of squares past 3.4e38 still has a square root, and a product below
// 1e-38 isn't flushed to zero. A value it stores to memory ("fstp dword") is a float. So in a rewrite, a
// register value is a double -- double's exponent range covers anything float arithmetic reaches, and the
// x87 still rounds every operation to 24 bits -- and a stored value is a float. A function whose original
// returns an unrounded register value returns double: the same ST0 to its callers.
static __forceinline double x87_sqrt(double x) {
    double r;
#ifdef VP_GCC
    __asm__ volatile("fld %1\n\tfsqrt\n\tfstp %0"
                     : "=m"(r)
                     : "m"(x)
                     : VP_X87_CLOBBERS);
#else
    __asm { fld x
            fsqrt
            fstp r }
#endif
    return r;
}

// fsin, fcos and fpatan are NOT rounded by precision control: they return a full 64-bit mantissa, which
// no C type holds (MSVC's long double is a double). So a transcendental result never passes through a C
// variable: the helpers below finish what the original does with it inside the same asm block -- store
// it to a float, or multiply it (the product IS rounded to 24 bits, so a double holds it exactly). Any
// other sequence gets its own small asm helper where it's used (see phys_tire.cpp, phys_engine.cpp).
static __forceinline float x87_sin_f(double x) {             // fsin; fstp dword
    float r;
#ifdef VP_GCC
    __asm__ volatile("fld %1\n\tfsin\n\tfstp %0"
                     : "=m"(r)
                     : "m"(x)
                     : VP_X87_CLOBBERS);
#else
    __asm { fld x
            fsin
            fstp r }
#endif
    return r;
}

static __forceinline float x87_cos_f(double x) {             // fcos; fstp dword
    float r;
#ifdef VP_GCC
    __asm__ volatile("fld %1\n\tfcos\n\tfstp %0"
                     : "=m"(r)
                     : "m"(x)
                     : VP_X87_CLOBBERS);
#else
    __asm { fld x
            fcos
            fstp r }
#endif
    return r;
}

static __forceinline double x87_sin_mul(double x, double k) { // fsin; fmul k
    double r;
#ifdef VP_GCC
    __asm__ volatile("fld %1\n\tfsin\n\tfmul %2\n\tfstp %0"
                     : "=m"(r)
                     : "m"(x), "m"(k)
                     : VP_X87_CLOBBERS);
#else
    __asm { fld x
            fsin
            fmul k
            fstp r }
#endif
    return r;
}

static __forceinline double x87_cos_mul(double x, double k) { // fcos; fmul k
    double r;
#ifdef VP_GCC
    __asm__ volatile("fld %1\n\tfcos\n\tfmul %2\n\tfstp %0"
                     : "=m"(r)
                     : "m"(x), "m"(k)
                     : VP_X87_CLOBBERS);
#else
    __asm { fld x
            fcos
            fmul k
            fstp r }
#endif
    return r;
}

// __ftol (0x4cf108): fistp qword with the rounding set to chop; the low dword. Out of range or NaN gives the
// integer indefinite 0x8000000000000000 -- so 0, where C's (int) would give 0x80000000.
static __forceinline int32_t x87_ftol(double x) {
    int32_t lo;
    uint16_t cw, chop;
    int64_t q;
#ifdef VP_GCC
    __asm__ volatile("fld %3\n\tfnstcw %0\n\tmov ax, %0\n\tor ah, 0x0c\n\tmov %1, ax\n\tfldcw %1\n\tfistp %2\n\tfldcw %0"
                     : "=m"(cw), "=m"(chop), "=m"(q)
                     : "m"(x)
                     : VP_X87_CLOBBERS, "eax", "cc");
#else
    __asm { fld x
            fnstcw cw
            mov ax, cw
            or ah, 0x0c
            mov chop, ax
            fldcw chop
            fistp qword ptr q
            fldcw cw }
#endif
    lo = (int32_t)q;
    return lo;
}

// Comparisons. The 1998 compiler tests the FPU's condition flags directly, and each test decides what a
// NaN does. A rewrite writes the same test with the same NaN result:
//   fcomp b; test ah,1   (C0)      a < b, or unordered      ->  !(a >= b)
//   fcomp b; test ah,0x41 (C0|C3)  a <= b, or unordered     ->  !(a > b)
//   fcomp b; test ah,0x40 (C3)     a == b, or unordered     ->  !(a < b || a > b)
//   fcomp b; test ah,5; jp (C0|C2) ordered and a >= b       ->  a >= b
// (C's own < <= == are false for a NaN; /fp:precise keeps that, so these forms are exact.)

// the engine's own small types
struct P3 { float x, y, z; };
struct M3 { float m[9]; };                  // row-major 3x3 (the engine's Matrix)
struct Q4 { float x, y, z, w; };            // the engine's Quat
