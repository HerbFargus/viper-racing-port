// compiler.h -- the two compilers the port builds with (relink stage R1).
//
// MSVC (the shipped build, and the harnesses' oracle) and GCC (mingw-w64 i686, on the way to native Linux). GCC's
// -fexcess-precision=standard rounds a float or double at every assignment, cast and return, as MSVC /fp:precise does
// on the x87 -- the rule every rewrite was checked under. But GCC reads a floating LITERAL at long-double precision
// (x * 0.4f multiplies by 0.4L, MSVC by the float 0.4f): an inexact literal in arithmetic is written cast to its own
// type, (float)0.4f / (double)0.98 -- a no-op for MSVC. (Clang was tried first: LLVM's x87 code never strips the
// excess precision, and no flag changes that; 24 of 62 harnesses differed.)
//
// VP_GCC: the GCC build. It has no MSVC __asm { }: each asm block has a GNU asm version beside it (-masm=intel), the
// same instructions on the same operands, under #ifdef VP_GCC.
// VP_X87_CLOBBERS: every x87 register, in a GNU asm block's clobbers -- MSVC never keeps a value on the x87 stack
// across an __asm block, and with these GCC doesn't either.
// VP_ASM_CALLS (and VP_ASM_CALLS_INLINE in place of __forceinline): on a function that isn't naked whose asm calls out
// (`call eax`). MSVC treats an __asm block as clobbering every general register; a compiler that takes only what the
// asm names doesn't see what the callee destroys (eax, ecx, edx, the x87 stack) -- clang kept rmarin's loop counter
// in ecx across _CIfmod. Not inlined, the caller follows the calling convention. MSVC's build is unchanged.
// VP_OPAQUE(v): GCC forgets what it knows of the variable v (it is in memory, any value) -- for a value the original
// keeps as an operand where GCC, knowing it, would fold it into another instruction: `s * x` with s known to be -1
// becomes fchs, which flips a NaN's sign where the original's fmul keeps it. Also: v is computed and stored right
// there even when nothing uses it -- a divide the original does (and faults in, with the exception unmasked) where
// GCC would drop or move it. Nothing for MSVC.
// VP_KEEP(c): the value c (a constant), which GCC can't see through -- `x * VP_KEEP(-0.5f)` stays an fmul by -0.5
// where GCC would compute x * 0.5 once and fchs it for the other use. (c) for MSVC.
#pragma once
#if defined(__GNUC__) && !defined(__clang__)
#define VP_GCC 1
#define VP_X87_CLOBBERS "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"
#define VP_ASM_CALLS __attribute__((noinline))
#define VP_ASM_CALLS_INLINE __attribute__((noinline))
#define VP_OPAQUE(v) __asm__ volatile("" : "+m"(v))
template <class T> static inline __attribute__((always_inline)) T vp_keep(T v) { __asm__("" : "+m"(v)); return v; }
#define VP_KEEP(c) vp_keep(c)
#else
#define VP_ASM_CALLS
#define VP_ASM_CALLS_INLINE __forceinline
#define VP_OPAQUE(v) ((void)0)
#define VP_KEEP(c) (c)
#endif
