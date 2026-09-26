// x87.h -- the few FPU instructions the 1998 physics uses directly, for rewrites that must match it bit for
// bit. The physics thread runs the x87 in single precision (physics_thread -> ExceptSinglePrecision(1),
// every tick), so each operation rounds to a float's 24-bit mantissa; this DLL is built /arch:IA32, so
// its own float arithmetic runs on the same x87 in the same mode. What a rewrite must reproduce is the
// order of operations (the grouping of each sum), the width of each constant (a double constant in a
// multiply is not a float one), and these instructions, which only the x87 has.
#pragma once

// A value the original keeps in an x87 register has the register's exponent range: single precision
// rounds only the mantissa, so a sum of squares past 3.4e38 still has a square root, and a product below
// 1e-38 isn't flushed to zero. A value it stores to memory ("fstp dword") is a float. So in a rewrite, a
// register value is a double -- double's exponent range covers anything float arithmetic reaches, and the
// x87 still rounds every operation to 24 bits -- and a stored value is a float. A function whose original
// returns an unrounded register value returns double: the same ST0 to its callers.
static __forceinline double x87_sqrt(double x) {
    double r;
    __asm { fld x
            fsqrt
            fstp r }
    return r;
}

static __forceinline double x87_sin(double x) {
    double r;
    __asm { fld x
            fsin
            fstp r }
    return r;
}

static __forceinline double x87_cos(double x) {
    double r;
    __asm { fld x
            fcos
            fstp r }
    return r;
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
