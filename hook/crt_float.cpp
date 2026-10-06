// crt_float.cpp -- M3 stage LIBC, group A: the C runtime's number conversions, its 80-bit (_LDBL12) arithmetic, the
// x87 wrappers and the 64-bit integer helpers, rewritten. The objects: intrncvt (the _LDBL12 -> double / float packers),
// strgtold (the string -> _LDBL12 state machine every strtod / atof / scanf %f goes through), mantold (the 96-bit
// mantissa helpers), tenpow (the 80-bit multiply and the powers of ten), x10fout ($I10_OUTPUT: 80-bit -> decimal
// digits), cfout (_fltout, __dtold), cfin (_fltin), _fptostr, cvt (printf's %e / %f / %g formatting and the
// _cfltcvt_tab routines), atof, atox (atol / atoi), strtol (strtoxl / strtoul), fp8, util (_set_exp / _decomp), ieeemisc
// (_finite / _isnan), ieee87 (_control87 and friends), fpctrl, ftol, llmul / lldiv / ulldiv / ullrem, and the x87
// transcendental dispatchers (87ctriga, 87cdisp, 87disp, 87fmod: acos / fmod and their _CI forms).
//
// Written from the v1.0 disassembly (the CRT is Microsoft's ~1997 LIBC.lib; the shapes follow its sources, the details
// -- every quirk -- the binary): faithful, so float formatting and parsing, rounding, errno and the FPU state come out
// bit for bit as the 1998 code's. Checked offline by test/world_crt_fmt.cpp, original against rewrite, on random and
// edge inputs.
//
// Two kinds of rewrite:
//  * C, for what the CRT wrote in C and computes with integers (the conversions: all of the _LDBL12 work is integer
//    arithmetic, so a C rewrite gets the same bits). The 1998 compiler's dead stores to its own frame are left out.
//  * assembly (naked), for what the CRT wrote in assembly or with inline x87 code -- _ftol, the 64-bit helpers, the
//    transcendental dispatchers (which share their caller's ebp frame and take their table in edx and their operands on
//    the FPU stack), _control87, the fpctrl routines, _set_exp / _decomp, _positive, _finite / _isnan: the original's
//    instructions, so the FPU sees exactly the same operations. Calls go through memory slots holding the v1.0
//    addresses (`call dword ptr [a_x]`), so no register changes that the original's `call rel32` wouldn't.
//
// Calls between CRT routines go to their v1.0 addresses (so whichever of the original or the rewrite is installed
// runs). Global state stays at its v1.0 address (crt_types.h). Function-pointer tables the CRT dispatches through stay
// where they are: _cfltcvt_tab (0x5026f8), the transcendental jump tables (_OP_ACOSjmptab ... 0x502780, 0x502630),
// and so does the code those tables point at -- the x87 kernels after _CIfmod (_fFMOD, 0x4cf374) and in 87triga.obj
// (0x4d1e60..), and 87disp's result routines after _trandisp2 (_rttospopde ... 0x4d2e93..0x4d2f69): only ever jumped
// to through the tables, they run as the original's bytes.
//
// Shadow mode: a routine whose arguments or result are in registers or on the FPU stack can't go through the C
// checking wrapper; in shadow mode it stays original (CRT_SHADOW_ORIGINAL below). The C ones are checked normally.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "crt_types.h"

// ---- shadow mode for the register-convention routines -------------------------------------------------------------
// The shadow wrapper is C: it would disturb the FPU stack, edx and ebp these routines take their inputs in. So their
// wrapper slot is a jump to the trampoline: in shadow mode they run as the original (as a function that branches in
// its first bytes does). Not in the offline builds, which register rewrites differently.
#if defined(VP_FUZZ) || defined(VP_CRT_HARNESS)
#define CRT_SHADOW_ORIGINAL(NEW)
#else
#ifdef VP_GCC
#define CRT_SHADOW_ORIGINAL(NEW)                                                                                   \
    static __declspec(naked) void VP_CAT(shadow_orig_, NEW)() {                                                    \
        __asm__ volatile("jmp dword ptr [%c0]" : : "i"(&VP_CAT(port_, NEW).orig));                                \
    }                                                                                                              \
    namespace { const bool VP_CAT(shadow_orig_set_, NEW) = (VP_CAT(port_, NEW).shadow = (void*)&VP_CAT(shadow_orig_, NEW), true); }
#else
#define CRT_SHADOW_ORIGINAL(NEW)                                                                                   \
    static __declspec(naked) void VP_CAT(shadow_orig_, NEW)() {                                                    \
        __asm { jmp dword ptr [VP_CAT(port_, NEW).orig] }                                                           \
    }                                                                                                              \
    namespace { const bool VP_CAT(shadow_orig_set_, NEW) = (VP_CAT(port_, NEW).shadow = (void*)&VP_CAT(shadow_orig_, NEW), true); }
#endif
#endif

namespace {

// ---- calls by v1.0 address ------------------------------------------------------------------------------------------
#define CRT_CALL(T, va) ((T)(uintptr_t)(va))
typedef uint32_t U32;

struct CrtMan { U32 m[3]; };                    // the 96-bit work mantissa of intrncvt / mantold (three dwords)
struct CrtFmtDesc { int32_t max_exp, min_exp, precision, exp_width, format_width, bias; };   // FpFormatDescriptor
struct D64 {                                    // a double passed by value, kept as bits (rule 6: no FPU copies)
    U32 lo, hi;
    D64() = default;
    explicit D64(U32 v) : lo(v), hi(v * 0x9e3779b9u) {}  // (test/fuzz.h builds arguments with a cast)
};
struct Ld10Arg {                                // an _LDOUBLE passed by value (12 bytes on the stack)
    U32 lo, hi;
    uint16_t exp, pad;
    Ld10Arg() = default;
    explicit Ld10Arg(U32 v) : lo(v * 0x85ebca6bu), hi(v * 0x9e3779b9u | 0x80000000u), exp((uint16_t)(0x3fff + v - 32)), pad(0) {}
};

static_assert(sizeof(CrtLd12) == 12 && sizeof(Ld10Arg) == 12 && sizeof(D64) == 8, "sizes");

// the routines this file calls, at their v1.0 addresses
enum : U32 {
    A_ADDL = 0x004d7930, A_ADD_12 = 0x004d7960, A_SHL_12 = 0x004d79d0, A_SHR_12 = 0x004d7a10, A_MTOLD12 = 0x004d7a50,
    A_I10_OUTPUT = 0x004d7b40, A_LD12MUL = 0x004d7ee0, A_MULTTENPOW12 = 0x004d8190,
    A_ZEROTAIL = 0x004d5c20, A_INCMAN = 0x004d5c90, A_ROUNDMAN = 0x004d5d00, A_COPYMAN = 0x004d5db0,
    A_FILLZEROMAN = 0x004d5dd0, A_ISZEROMAN = 0x004d5de0, A_SHRMAN = 0x004d5e00, A_LD12CVT = 0x004d5eb0,
    A_LD12TOD = 0x004d6080, A_LD12TOF = 0x004d60a0, A_ATODBL = 0x004d60c0, A_ATOFLT = 0x004d6100,
    A_FPTOSTR = 0x004d6140, A_FLTOUT = 0x004d61d0, A_DTOLD = 0x004d6240, A_STRGTOLD12 = 0x004d6a20,
    A_FLTIN = 0x004d4d40, A_ATOL = 0x004cf8e0, A_STRTOXL = 0x004cff50,
    A_CFTOE = 0x004d0af0, A_CFTOF = 0x004d0c30, A_CFTOG = 0x004d0d30, A_CFTOE_G = 0x004d0de0, A_CFTOF_G = 0x004d0e10,
    A_SHIFT = 0x004d0eb0,
    A_ISCTYPE = 0x004d47a0, A_TOLOWER = 0x004cfd50, A_TOUPPER = 0x004d4df0, A_MEMMOVE = 0x004cf400,
};
#define c_addl          CRT_CALL(int(__cdecl*)(U32, U32, U32*), A_ADDL)
#define c_add_12        CRT_CALL(void(__cdecl*)(CrtLd12*, CrtLd12*), A_ADD_12)
#define c_shl_12        CRT_CALL(void(__cdecl*)(CrtLd12*), A_SHL_12)
#define c_shr_12        CRT_CALL(void(__cdecl*)(CrtLd12*), A_SHR_12)
#define c_mtold12       CRT_CALL(void(__cdecl*)(const char*, U32, CrtLd12*), A_MTOLD12)
#define c_I10_OUTPUT    CRT_CALL(int(__cdecl*)(Ld10Arg, int, U32, CrtFos*), A_I10_OUTPUT)
#define c_ld12mul       CRT_CALL(void(__cdecl*)(CrtLd12*, const CrtLd12*), A_LD12MUL)
#define c_multtenpow12  CRT_CALL(void(__cdecl*)(CrtLd12*, int, U32), A_MULTTENPOW12)
#define c_ZeroTail      CRT_CALL(int(__cdecl*)(CrtMan*, int), A_ZEROTAIL)
#define c_IncMan        CRT_CALL(int(__cdecl*)(CrtMan*, int), A_INCMAN)
#define c_RoundMan      CRT_CALL(int(__cdecl*)(CrtMan*, int), A_ROUNDMAN)
#define c_CopyMan       CRT_CALL(void(__cdecl*)(CrtMan*, const CrtMan*), A_COPYMAN)
#define c_FillZeroMan   CRT_CALL(void(__cdecl*)(CrtMan*), A_FILLZEROMAN)
#define c_IsZeroMan     CRT_CALL(int(__cdecl*)(const CrtMan*), A_ISZEROMAN)
#define c_ShrMan        CRT_CALL(void(__cdecl*)(CrtMan*, int), A_SHRMAN)
#define c_ld12cvt       CRT_CALL(int(__cdecl*)(const CrtLd12*, void*, const CrtFmtDesc*), A_LD12CVT)
#define c_ld12tod       CRT_CALL(int(__cdecl*)(const CrtLd12*, D64*), A_LD12TOD)
#define c_ld12tof       CRT_CALL(int(__cdecl*)(const CrtLd12*, U32*), A_LD12TOF)
#define c_atodbl        CRT_CALL(int(__cdecl*)(D64*, const char*), A_ATODBL)
#define c_atoflt        CRT_CALL(int(__cdecl*)(U32*, const char*), A_ATOFLT)
#define c_fptostr       CRT_CALL(void(__cdecl*)(char*, int, CrtStrflt*), A_FPTOSTR)
#define c_fltout        CRT_CALL(CrtStrflt*(__cdecl*)(D64), A_FLTOUT)
#define c_dtold         CRT_CALL(void(__cdecl*)(CrtLd10*, const D64*), A_DTOLD)
#define c_strgtold12    CRT_CALL(U32(__cdecl*)(CrtLd12*, const char**, const char*, int, int, int, int), A_STRGTOLD12)
#define c_fltin         CRT_CALL(CrtFlt*(__cdecl*)(const char*, int, int, int), A_FLTIN)
#define c_atol          CRT_CALL(int32_t(__cdecl*)(const char*), A_ATOL)
#define c_strtoxl       CRT_CALL(U32(__cdecl*)(const char*, const char**, int, int), A_STRTOXL)
#define c_cftoe         CRT_CALL(char*(__cdecl*)(D64*, char*, int, int), A_CFTOE)
#define c_cftof         CRT_CALL(char*(__cdecl*)(D64*, char*, int), A_CFTOF)
#define c_cftog         CRT_CALL(char*(__cdecl*)(D64*, char*, int, int), A_CFTOG)
#define c_cftoe_g       CRT_CALL(char*(__cdecl*)(D64*, char*, int, int), A_CFTOE_G)
#define c_cftof_g       CRT_CALL(char*(__cdecl*)(D64*, char*, int), A_CFTOF_G)
#define c_shift         CRT_CALL(void(__cdecl*)(char*, int), A_SHIFT)
#define c_isctype       CRT_CALL(int(__cdecl*)(int, int), A_ISCTYPE)
#define c_tolower       CRT_CALL(int(__cdecl*)(int), A_TOLOWER)
#define c_toupper       CRT_CALL(int(__cdecl*)(int), A_TOUPPER)
#define c_memmove       CRT_CALL(void*(__cdecl*)(void*, const void*, size_t), A_MEMMOVE)

// the same addresses as memory operands for the assembly rewrites' calls and jumps
const U32 a_ctrandisp1 = 0x004d20f5, a_ctrandisp2 = 0x004d1f84, a_cintrindisp1 = 0x004d1f4e,
          a_cintrindisp2 = 0x004d1f10, a_trandisp1 = 0x004d2da0, a_trandisp2 = 0x004d2e07, a_fload = 0x004d2121,
          a_87except = 0x004d6510, a_abstract_cw = 0x004cfa60, a_hw_cw = 0x004cfb10, a_abstract_sw = 0x004cfba0,
          a_control87 = 0x004cfa00, a_controlfp = 0x004cfa40, a_set_exp = 0x004d3050;

// the CRT's statics this file writes (v1.0 addresses)
enum : U32 {
    G_FMT = 0x00502710,            // cvt.obj: _cftog's "formatting %g" flag (a char)
    G_MAGNITUDE = 0x00502714,      // cvt.obj: %g's decimal exponent
    G_ROUND_EXPANSION = 0x00502718,// cvt.obj: %g's rounding grew a digit (a char)
    G_EXP_STR = 0x0050271c,        // cvt.obj: "e+000"
    G_PFLT = 0x005d55f8,           // cvt.obj: %g's STRFLT*
    G_FOS = 0x005d5730,            // cfout.obj: _fltout's FOS (0x1a bytes)
    G_STRFLT = 0x005d5750,         // cfout.obj: _fltout's STRFLT (0x10)
    G_PFLTIN = 0x00503424,         // cfin.obj: the FLT* _fltin returns (-> 0x5d5718)
    G_DBLFMT = 0x005036b8,         // intrncvt.obj: DoubleFormat
    G_FLTFMT = 0x005036d0,         // intrncvt.obj: FloatFormat
    G_POW10POS = 0x00503758,       // tenpow: _pow10pos - 0x60 (the loop adds 0x54 before indexing 1..7)
    G_POW10NEG = 0x005038b8,       // _pow10neg - 0x60
    G_QNAN = 0x00503744, G_INF = 0x0050374c, G_IND = 0x00503754, G_SNAN = 0x0050375c,   // "1#QNAN" ... (x10fout)
};
#define G8(va)  CRT_G(uint8_t, va)
#define G32(va) CRT_G(U32, va)

static inline U32 rd32(const void* p) { U32 v; memcpy(&v, p, 4); return v; }
static inline void wr32(void* p, U32 v) { memcpy(p, &v, 4); }
static inline uint16_t rd16(const void* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void wr16(void* p, uint16_t v) { memcpy(p, &v, 2); }

// isdigit / isspace as the 1998 compiler inlines them: _isctype when __mb_cur_max > 1, else the _pctype table
static inline int crt_isctype_inl(int c, int mask) {
    if (crt_mb_cur_max > 1) return c_isctype(c, mask);
    return crt_pctype[c] & mask;
}

// ---- footprint helpers ----------------------------------------------------------------------------------------------
static void fp_none(Footprint&) {}
static void fp_pure(Footprint& f) { f.pure = true; }
const char* const k_asm_reason = "register / FPU-stack convention: checked offline (test/world_crt_fmt.cpp)";

}  // namespace

// =====================================================================================================================
// mantold.obj: the 96-bit mantissa arithmetic
// =====================================================================================================================
// ___addl (0x4d7930): *sum = x + y; the carry out (either operand above the sum)
static int __cdecl crt_addl(U32 x, U32 y, U32* sum) {
    const U32 r = x + y;
    const int carry = (r < x || r < y) ? 1 : 0;
    *sum = r;
    return carry;
}
static void fp_addl(Footprint& f, U32, U32, U32* sum) { f.pure = true; f.add(sum, 4, "sum"); }
PORT_FN(0x004d7930, "addl", crt_addl, fp_addl)

// ___add_12 (0x4d7960): x += y, 96 bits, the carries through ___addl
static void __cdecl crt_add_12(CrtLd12* x, CrtLd12* y) {
    U32* xs = (U32*)x;
    const U32* ys = (const U32*)y;
    if (c_addl(xs[0], ys[0], &xs[0])) {
        if (c_addl(xs[1], 1, &xs[1])) xs[2]++;
    }
    if (c_addl(xs[1], ys[1], &xs[1])) xs[2]++;
    c_addl(xs[2], ys[2], &xs[2]);
}
static void fp_ld12_xy(Footprint& f, CrtLd12* x, CrtLd12*) { f.pure = true; f.add(x, 12, "x"); }
PORT_FN(0x004d7960, "add_12", crt_add_12, fp_ld12_xy)

// ___shl_12 (0x4d79d0) / ___shr_12 (0x4d7a10): 96 bits one place left / right
static void __cdecl crt_shl_12(CrtLd12* p) {
    U32* s = (U32*)p;
    const U32 c0 = (s[0] & 0x80000000u) >> 31, c1 = (s[1] & 0x80000000u) >> 31;
    s[0] = s[0] << 1;
    s[1] = (s[1] << 1) | c0;
    s[2] = (s[2] << 1) | c1;
}
static void fp_ld12_p(Footprint& f, CrtLd12* p) { f.pure = true; f.add(p, 12, "ld12"); }
PORT_FN(0x004d79d0, "shl_12", crt_shl_12, fp_ld12_p)

static void __cdecl crt_shr_12(CrtLd12* p) {
    U32* s = (U32*)p;
    const U32 c2 = (s[2] & 1) << 31, c1 = (s[1] & 1) << 31;
    s[2] = s[2] >> 1;
    s[1] = (s[1] >> 1) | c2;
    s[0] = (s[0] >> 1) | c1;
}
PORT_FN(0x004d7a10, "shr_12", crt_shr_12, fp_ld12_p)

// ___mtold12 (0x4d7a50): manlen BCD digits (one per byte) into a normalised _LDBL12. Zero digits never come here
// (__strgtold12 strips them); an all-zero mantissa would loop forever, as the original's does.
static void __cdecl crt_mtold12(const char* manptr, U32 manlen, CrtLd12* ld) {
    uint16_t expn = 0x404e;
    U32* s = (U32*)ld;
    s[0] = s[1] = s[2] = 0;
    while (manlen) {
        CrtLd12 tmp;
        memcpy(&tmp, ld, 12);
        c_shl_12(ld);                                     // *2
        manlen--;
        manptr++;
        c_shl_12(ld);                                     // *4
        c_add_12(ld, &tmp);                               // *5
        c_shl_12(ld);                                     // *10
        U32 d[3] = {(U32)(int32_t)(signed char)manptr[-1], 0, 0};
        c_add_12(ld, (CrtLd12*)d);
    }
    while (s[2] == 0) {                                   // normalise by 16 bits
        expn = (uint16_t)(expn - 0x10);
        s[2] = s[1] >> 16;
        s[1] = (s[1] << 16) | (s[0] >> 16);
        s[0] <<= 16;
    }
    while (!(((const uint8_t*)ld)[9] & 0x80)) {           // then bit by bit
        expn--;
        c_shl_12(ld);
    }
    wr16((uint8_t*)ld + 10, expn);
}
static void fp_mtold12(Footprint& f, const char*, U32, CrtLd12* ld) { f.add(ld, 12, "ld12"); }
PORT_FN(0x004d7a50, "mtold12", crt_mtold12, fp_mtold12)

// =====================================================================================================================
// tenpow.obj: the 80-bit multiply and the powers of ten
// =====================================================================================================================
// ___ld12mul (0x4d7ee0): *px *= *py, 80 bits (the words of both mantissas, partial products summed into a 96-bit
// result with the carries counted a word up). The carry slot above the top dword is the loop counter's: the original's
// frame puts `i` there, so a carry out of the top would bump the outer loop (it can't happen: two 80-bit mantissas'
// product never reaches it) -- the work area here has the same shape.
static void __cdecl crt_ld12mul(CrtLd12* px, const CrtLd12* py) {
    uint8_t* x = (uint8_t*)px;
    const uint8_t* y = (const uint8_t*)py;
    uint16_t cx = rd16(x + 10), dx = rd16(y + 10);
    const uint16_t sign = (uint16_t)((dx ^ cx) & 0x8000);
    cx &= 0x7fff;
    dx &= 0x7fff;
    uint16_t expsum = (uint16_t)(cx + dx);
    if (cx >= 0x7fff || dx >= 0x7fff || expsum > 0xbffd) {           // overflow: infinity
        wr32(x + 4, 0);
        wr32(x + 0, 0);
        wr32(x + 8, (sign ? 0u : 0x80000000u) - 0x8000u);
        return;
    }
    if (expsum <= 0x3fbf) {                                           // underflow: zero
        wr32(x + 8, 0);
        wr32(x + 4, 0);
        wr32(x + 0, 0);
        return;
    }
    if (cx == 0) {
        expsum++;
        if (!(rd32(x + 8) & 0x7fffffff) && rd32(x + 4) == 0 && rd32(x + 0) == 0) {
            wr16(x + 10, 0);
            return;
        }
    }
    if (dx == 0) {
        expsum++;
        if (!(rd32(y + 8) & 0x7fffffff) && rd32(y + 4) == 0 && rd32(y + 0) == 0) {
            wr32(x + 8, 0);
            wr32(x + 4, 0);
            wr32(x + 0, 0);
            return;
        }
    }
    // the work area: the 12-byte product, then the outer loop counter (as the original's frame)
    alignas(4) uint8_t w[16];
    memset(w, 0, 12);
    U32 roffs = 0;
    wr32(w + 12, 0);                                                  // i
    for (; (int32_t)rd32(w + 12) < 5; wr32(w + 12, rd32(w + 12) + 1), roffs += 2) {
        const U32 i = rd32(w + 12);
        U32 poffx = i * 2, poffy = 8;
        for (int32_t j = 5 - (int32_t)i; j > 0; j--) {
            const U32 prod = (U32)rd16(x + poffx) * (U32)rd16(y + poffy);
            U32 acc = rd32(w + roffs);
            if (c_addl(acc, prod, &acc)) {
                wr32(w + roffs, acc);
                wr16(w + roffs + 4, (uint16_t)(rd16(w + roffs + 4) + 1));
            } else {
                wr32(w + roffs, acc);
            }
            poffx += 2;
            poffy -= 2;
        }
    }
    expsum = (uint16_t)(expsum - 0x3ffe);
    if ((int16_t)expsum > 0) {
        while (!(rd32(w + 8) & 0x80000000u)) {                        // normalise
            c_shl_12((CrtLd12*)w);
            expsum--;
            if ((int16_t)expsum <= 0) break;
        }
    }
    if ((int16_t)expsum <= 0) {
        expsum--;
        U32 sticky;
        if ((int16_t)expsum < 0) {                                    // denormal: shift right, keeping a sticky bit
            uint16_t bx = (uint16_t)(0 - expsum);
            expsum = (uint16_t)(expsum + bx);
            sticky = rd32(w + 0);
            do {
                if (rd16(w + 0) & 1) sticky++;
                c_shr_12((CrtLd12*)w);
                bx--;
            } while (bx != 0);
        } else {
            sticky = rd32(w + 0);
        }
        if (sticky) w[0] |= 1;
    }
    if (rd16(w + 0) > 0x8000) {                                       // round
        if (rd32(w + 2) == 0xffffffffu) {
            wr32(w + 2, 0);
            if (rd32(w + 6) == 0xffffffffu) {
                wr32(w + 6, 0);
                if (rd16(w + 10) == 0xffff) {
                    wr16(w + 10, 0x8000);
                    expsum++;
                } else {
                    wr16(w + 10, (uint16_t)(rd16(w + 10) + 1));
                }
            } else {
                wr32(w + 6, rd32(w + 6) + 1);
            }
        } else {
            wr32(w + 2, rd32(w + 2) + 1);
        }
    }
    if (expsum >= 0x7fff) {                                           // overflow after rounding: infinity
        wr32(x + 4, 0);
        wr32(x + 0, 0);
        wr32(x + 8, (sign ? 0u : 0x80000000u) - 0x8000u);
        return;
    }
    wr16(x + 0, rd16(w + 2));
    wr32(x + 2, rd32(w + 4));
    wr32(x + 6, rd32(w + 8));
    wr16(x + 10, (uint16_t)(sign | expsum));
}
static void fp_ld12mul(Footprint& f, CrtLd12* px, const CrtLd12*) { f.pure = true; f.add(px, 12, "px"); }
PORT_FN(0x004d7ee0, "ld12mul", crt_ld12mul, fp_ld12mul)

// ___multtenpow12 (0x4d8190): *pld12 *= 10^pow, from the tables 10^1..10^7, 10^8.., 10^64.. (three bits of pow per
// step); an entry whose low word has its top bit set is used "unrounded" (its mantissa less one). mult12 == 0 clears
// the extended word first.
static void __cdecl crt_multtenpow12(CrtLd12* pld12, int pow, U32 mult12) {
    U32 tab = G_POW10POS;
    if (pow == 0) return;
    if (pow < 0) {
        pow = -pow;
        tab = G_POW10NEG;
    }
    if (!mult12) wr16(pld12, 0);
    while (pow) {
        tab += 0x54;
        const int last3 = pow & 7;
        pow >>= 3;
        if (last3) {
            const CrtLd12* pentry = (const CrtLd12*)(uintptr_t)(tab + (U32)last3 * 12);
            CrtLd12 unround;
            if (rd16(pentry) >= 0x8000) {
                memcpy(&unround, pentry, 12);
                wr32(unround.b + 2, rd32(unround.b + 2) - 1);
                pentry = &unround;
            }
            c_ld12mul(pld12, pentry);
        }
    }
}
static void fp_multtenpow12(Footprint& f, CrtLd12* p, int, U32) { f.pure = true; f.add(p, 12, "ld12"); }
PORT_FN(0x004d8190, "multtenpow12", crt_multtenpow12, fp_multtenpow12)

// =====================================================================================================================
// x10fout.obj: $I10_OUTPUT -- an 80-bit real to decimal digits
// =====================================================================================================================
// ndigits digits (at most 21) into fos, rounded; with output_flags & 1 (the %f form) ndigits counts from the decimal
// point. Returns 1, or 0 for the specials ("1#SNAN", "1#IND", "1#INF", "1#QNAN" from the CRT's .data).
static int __cdecl crt_I10_OUTPUT(Ld10Arg ld, int ndigits, U32 output_flags, CrtFos* fos) {
    static const uint8_t one_tenth[12] = {0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xfb, 0x3f};
    CrtLd12 ld_one_tenth;
    memcpy(&ld_one_tenth, one_tenth, 12);
    int retval = 1;
    const uint16_t sign = (uint16_t)(ld.exp & 0x8000);
    const uint16_t expn = (uint16_t)(ld.exp & 0x7fff);
    fos->sign = sign ? '-' : ' ';
    if (expn == 0 && ld.hi == 0 && ld.lo == 0) {
        fos->sign = ' ';
        fos->exp = 0;
        fos->ManLen = 1;
        fos->man[0] = '0';
        fos->man[1] = 0;
        return 1;
    }
    if (expn == 0x7fff) {
        fos->exp = 1;
        if (!(ld.hi == 0x80000000u && ld.lo == 0) && !(ld.hi & 0x40000000u)) {
            memcpy(fos->man, (const void*)(uintptr_t)G_SNAN, 7);           // "1#SNAN"
            fos->ManLen = 6;
        } else if (sign && ld.hi == 0xc0000000u && ld.lo == 0) {
            memcpy(fos->man, (const void*)(uintptr_t)G_IND, 6);            // "1#IND"
            fos->ManLen = 5;
        } else if (ld.hi == 0x80000000u && ld.lo == 0) {
            memcpy(fos->man, (const void*)(uintptr_t)G_INF, 6);            // "1#INF"
            fos->ManLen = 5;
        } else {
            memcpy(fos->man, (const void*)(uintptr_t)G_QNAN, 7);           // "1#QNAN"
            fos->ManLen = 6;
        }
        return 0;
    }
    // the decimal exponent's estimate: (the top bits of the binary exponent and mantissa) * log10(2), in 16.16
    const U32 est = 77u * ((U32)(expn >> 8) + 2u * (ld.hi >> 24)) + 19728u * (U32)expn - 0x134312f4u;
    int16_t r = (int16_t)((int32_t)est >> 16);
    CrtLd12 tmp;
    wr16(tmp.b + 0, 0);
    wr32(tmp.b + 2, ld.lo);
    wr32(tmp.b + 6, ld.hi);
    wr16(tmp.b + 10, expn);
    c_multtenpow12(&tmp, -(int)r, 1);
    if (rd16(tmp.b + 10) >= 0x3fff) {
        r = (int16_t)(r + 1);
        c_ld12mul(&tmp, &ld_one_tenth);
    }
    fos->exp = r;
    int nd = ndigits;
    if (output_flags & 1) {
        nd = ndigits + (int)r;
        if (nd <= 0) {
            fos->sign = ' ';
            fos->exp = 0;
            fos->ManLen = 1;
            fos->man[0] = '0';
            fos->man[1] = 0;
            return 1;
        }
    }
    if (nd > 21) nd = 21;
    int32_t ex = (int32_t)rd16(tmp.b + 10) - 0x3ffe;
    wr16(tmp.b + 10, 0);
    for (int k = 0; k < 8; k++) c_shl_12(&tmp);
    if (ex < 0) {
        ex = (-ex) & 0xff;
        for (; ex > 0; ex--) c_shr_12(&tmp);
    }
    char* const p = fos->man;
    char* q = p;
    nd++;
    while (nd > 0) {
        q++;
        CrtLd12 copy;
        memcpy(&copy, &tmp, 12);
        c_shl_12(&tmp);
        c_shl_12(&tmp);
        c_add_12(&tmp, &copy);
        c_shl_12(&tmp);
        nd--;
        q[-1] = (char)(tmp.b[11] + '0');
        tmp.b[11] = 0;
    }
    const char last = q[-1];
    q -= 2;                                                           // the last kept digit
    if (last >= '5') {                                                // round up
        while (q >= p && *q == '9') {
            *q = '0';
            q--;
        }
        if (q < p) {
            fos->exp = (int16_t)(fos->exp + 1);
            q++;
        }
        (*q)++;
    } else {
        while (q >= p && *q == '0') q--;
        if (q < p) {
            fos->exp = 0;
            p[0] = '0';
            fos->sign = ' ';
            fos->ManLen = 1;
            p[1] = 0;
            return 1;
        }
    }
    const int8_t len = (int8_t)((uint8_t)((uintptr_t)q - (uintptr_t)fos) - 3);
    fos->ManLen = (char)len;
    *((char*)fos + 4 + len) = 0;
    return retval;
}
static void fp_I10_OUTPUT(Footprint& f, Ld10Arg, int, U32, CrtFos* fos) { f.pure = true; f.add(fos, sizeof(CrtFos), "fos"); }
PORT_FN(0x004d7b40, "$I10_OUTPUT", crt_I10_OUTPUT, fp_I10_OUTPUT)

// =====================================================================================================================
// intrncvt.obj: _LDBL12 to double / float, rounding the 96-bit mantissa (three dwords, the top bit first)
// =====================================================================================================================
// __ZeroTail (0x4d5c20): are the mantissa's bits from nbit on all zero?
static int __cdecl crt_ZeroTail(CrtMan* man, int nbit) {
    int nl = nbit / 32;
    const int nb = 31 - nbit % 32;
    const U32 bitmask = ~(0xffffffffu << (nb & 31));
    if (man->m[nl] & bitmask) return 0;
    for (nl++; nl < 3; nl++)
        if (man->m[nl]) return 0;
    return 1;
}
static void fp_ZeroTail(Footprint& f, CrtMan*, int) { f.pure = true; }
PORT_FN(0x004d5c20, "ZeroTail", crt_ZeroTail, fp_ZeroTail)

// __IncMan (0x4d5c90): add one at bit nbit; the carry out of the top
static int __cdecl crt_IncMan(CrtMan* man, int nbit) {
    int nl = nbit / 32;
    const int nb = 31 - nbit % 32;
    const U32 one = 1u << (nb & 31);
    int carry = c_addl(man->m[nl], one, &man->m[nl]);
    for (nl--; nl >= 0 && carry; nl--) carry = c_addl(man->m[nl], 1, &man->m[nl]);
    return carry;
}
static void fp_man_int(Footprint& f, CrtMan* man, int) { f.pure = true; f.add(man, 12, "man"); }
PORT_FN(0x004d5c90, "IncMan", crt_IncMan, fp_man_int)

// __RoundMan (0x4d5d00): round to `precision` bits -- up only when the next bit is set AND something below it is (an
// exact half is truncated); clear the rest. Returns IncMan's carry.
static int __cdecl crt_RoundMan(CrtMan* man, int precision) {
    int retval = 0;
    int nl = precision / 32;
    const int nb = 31 - precision % 32;
    if (man->m[nl] & (1u << (nb & 31))) {
        if (!c_ZeroTail(man, precision + 1)) retval = c_IncMan(man, precision - 1);
    }
    man->m[nl] &= 0xffffffffu << (nb & 31);
    for (nl++; nl < 3; nl++) man->m[nl] = 0;
    return retval;
}
PORT_FN(0x004d5d00, "RoundMan", crt_RoundMan, fp_man_int)

// __CopyMan (0x4d5db0): dst = src
static void __cdecl crt_CopyMan(CrtMan* dst, const CrtMan* src) {
    for (int k = 0; k < 3; k++) dst->m[k] = src->m[k];
}
static void fp_CopyMan(Footprint& f, CrtMan* dst, const CrtMan*) { f.pure = true; f.add(dst, 12, "man"); }
PORT_FN(0x004d5db0, "CopyMan", crt_CopyMan, fp_CopyMan)

// __FillZeroMan (0x4d5dd0)
static void __cdecl crt_FillZeroMan(CrtMan* man) { man->m[0] = man->m[1] = man->m[2] = 0; }
static void fp_man(Footprint& f, CrtMan* man) { f.pure = true; f.add(man, 12, "man"); }
PORT_FN(0x004d5dd0, "FillZeroMan", crt_FillZeroMan, fp_man)

// __IsZeroMan (0x4d5de0)
static int __cdecl crt_IsZeroMan(const CrtMan* man) {
    for (int k = 0; k < 3; k++)
        if (man->m[k]) return 0;
    return 1;
}
static void fp_IsZeroMan(Footprint& f, const CrtMan*) { f.pure = true; }
PORT_FN(0x004d5de0, "IsZeroMan", crt_IsZeroMan, fp_IsZeroMan)

// __ShrMan (0x4d5e00): the mantissa nbit places right
static void __cdecl crt_ShrMan(CrtMan* man, int nbit) {
    const int nl = nbit / 32;
    const int nb = nbit % 32;
    const U32 mask = ~(0xffffffffu << (nb & 31));
    U32 carry = 0;
    for (int i = 0; i < 3; i++) {
        const U32 tmp = man->m[i] & mask;
        man->m[i] = (man->m[i] >> (nb & 31)) | carry;
        carry = tmp << ((32 - nb) & 31);
    }
    for (int i = 2; i >= 0; i--) man->m[i] = (nl > i) ? 0 : man->m[i - nl];
}
PORT_FN(0x004d5e00, "ShrMan", crt_ShrMan, fp_man_int)

// __ld12cvt (0x4d5eb0): an _LDBL12 to the format's bits (round to its precision; denormals, overflow to infinity,
// underflow to zero). Returns 0, 1 (overflow) or 2 (underflow / denormal).
static int __cdecl crt_ld12cvt(const CrtLd12* pld12, void* d, const CrtFmtDesc* format) {
    const uint8_t* b = pld12->b;
    const uint16_t ax = rd16(b + 10);
    int32_t exp = (int32_t)(ax & 0x7fff) - 0x3fff;
    const U32 sign = ax & 0x8000;
    CrtMan man, saved;
    man.m[0] = rd32(b + 6);
    man.m[1] = rd32(b + 2);
    man.m[2] = (U32)rd16(b + 0) << 16;
    int retval;
    if (exp == -0x3fff) {
        exp = 0;
        if (c_IsZeroMan(&man)) {
            retval = 0;
        } else {
            c_FillZeroMan(&man);
            retval = 2;
        }
    } else {
        c_CopyMan(&saved, &man);
        if (c_RoundMan(&man, format->precision)) exp++;
        if (format->min_exp - format->precision > exp) {
            c_FillZeroMan(&man);
            exp = 0;
            retval = 2;
        } else if (exp <= format->min_exp) {
            c_CopyMan(&man, &saved);
            c_ShrMan(&man, format->min_exp - exp);
            c_RoundMan(&man, format->precision);
            c_ShrMan(&man, format->exp_width + 1);
            exp = 0;
            retval = 2;
        } else if (exp >= format->max_exp) {
            c_FillZeroMan(&man);
            man.m[0] |= 0x80000000u;
            c_ShrMan(&man, format->exp_width);
            exp = format->bias + format->max_exp;
            retval = 1;
        } else {
            exp += format->bias;
            man.m[0] &= 0x7fffffffu;
            c_ShrMan(&man, format->exp_width);
            retval = 0;
        }
    }
    U32 hi = (U32)exp << ((31 - format->exp_width) & 31);
    hi |= sign ? 0x80000000u : 0;
    hi |= man.m[0];
    if (format->format_width == 64) {
        wr32((uint8_t*)d + 4, hi);
        wr32(d, man.m[1]);
    } else if (format->format_width == 32) {
        wr32(d, hi);
    }
    return retval;
}
static void fp_ld12cvt(Footprint& f, const CrtLd12*, void* d, const CrtFmtDesc* fmt) {
    // pure for the CRT's two formats (immutable descriptors); a random one isn't something to fuzz
    f.pure = (uintptr_t)fmt == G_DBLFMT || (uintptr_t)fmt == G_FLTFMT;
    f.add(d, (uintptr_t)fmt == G_FLTFMT ? 4 : 8, "result");
}
PORT_FN(0x004d5eb0, "ld12cvt", crt_ld12cvt, fp_ld12cvt)

// __ld12tod (0x4d6080) / __ld12tof (0x4d60a0)
static int __cdecl crt_ld12tod(const CrtLd12* pld12, D64* d) {
    return c_ld12cvt(pld12, d, (const CrtFmtDesc*)(uintptr_t)G_DBLFMT);
}
static void fp_ld12tod(Footprint& f, const CrtLd12*, D64* d) { f.pure = true; f.add(d, 8, "double"); }
PORT_FN(0x004d6080, "ld12tod", crt_ld12tod, fp_ld12tod)

static int __cdecl crt_ld12tof(const CrtLd12* pld12, U32* d) {
    return c_ld12cvt(pld12, d, (const CrtFmtDesc*)(uintptr_t)G_FLTFMT);
}
static void fp_ld12tof(Footprint& f, const CrtLd12*, U32* d) { f.pure = true; f.add(d, 4, "float"); }
PORT_FN(0x004d60a0, "ld12tof", crt_ld12tof, fp_ld12tof)

// __atodbl (0x4d60c0) / __atoflt (0x4d6100): a string to a double / float (no scale, no implicit exponent)
static int __cdecl crt_atodbl(D64* d, const char* str) {
    CrtLd12 ld12;
    const char* end;
    c_strgtold12(&ld12, &end, str, 0, 0, 0, 0);
    return c_ld12tod(&ld12, d);
}
static void fp_atodbl(Footprint& f, D64* d, const char*) { f.add(d, 8, "double"); }
PORT_FN(0x004d60c0, "atodbl", crt_atodbl, fp_atodbl)

static int __cdecl crt_atoflt(U32* d, const char* str) {
    CrtLd12 ld12;
    const char* end;
    c_strgtold12(&ld12, &end, str, 0, 0, 0, 0);
    return c_ld12tof(&ld12, d);
}
static void fp_atoflt(Footprint& f, U32* d, const char*) { f.add(d, 4, "float"); }
PORT_FN(0x004d6100, "atoflt", crt_atoflt, fp_atoflt)

// =====================================================================================================================
// strgtold.obj: ___strgtold12 -- a string to an _LDBL12
// =====================================================================================================================
// The state machine of the CRT's strgtold.c: [whitespace] [sign] digits [. digits] [(e|E|d|D) [sign] digits]. At most
// 25 mantissa digits are kept (a 26th and on only move the exponent); 25 are rounded to 24 -- on the 24th digit itself
// (the original's off-by-one). Exponents past +-5200 give infinity / zero. Returns SLD_* flags: 1 underflow, 2
// overflow, 4 no digits; *p_end_ptr is where the number ended (the string itself when there were no digits).
static U32 __cdecl crt_strgtold12(CrtLd12* pld12, const char** p_end_ptr, const char* str, int mult12, int scale,
                                  int decpt, int implicit_E) {
    char man[28];
    char* manp = man;
    uint16_t sign = 0;
    U32 manlen = 0;
    int found_digit = 0, found_decpoint = 0, found_exponent = 0, overflow = 0, underflow = 0;
    int32_t exp_adj = 0;
    U32 retflags = 0;
    int32_t expsign = 1;
    int32_t exp = 0;
    const char* p = str;
    const char* savedp = str;
    const char dp = crt_decimal_point;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int state = 0;
    signed char c;
#define DIGIT(ch) crt_isctype_inl((uint8_t)(ch), CRT_DIGIT)
    while (state != 10) {
        c = (signed char)*p++;
        switch (state) {
        case 0:
            if (c >= '1' && c <= '9') { state = 3; p--; break; }
            if (c == dp) { state = 5; break; }
            switch (c) {
            case '+': sign = 0; state = 2; break;
            case '-': sign = 0x8000; state = 2; break;
            case '0': state = 1; break;
            default: state = 10; p--; break;
            }
            break;
        case 1:
            found_digit = 1;
            if (c >= '1' && c <= '9') { state = 3; p--; break; }
            if (c == dp) { state = 4; break; }
            switch (c) {
            case '+': case '-': p--; state = 11; break;
            case '0': state = 1; break;
            case 'D': case 'E': case 'd': case 'e': state = 6; break;
            default: state = 10; p--; break;
            }
            break;
        case 2:
            if (c >= '1' && c <= '9') { state = 3; p--; break; }
            if (c == dp) { state = 5; break; }
            if (c == '0') { state = 1; break; }
            state = 10;
            p = savedp;
            break;
        case 3:
            found_digit = 1;
            while (DIGIT(c)) {
                if (manlen < 25) {
                    manlen++;
                    *manp++ = (char)(c - '0');
                } else {
                    exp_adj++;
                }
                c = (signed char)*p++;
            }
            if (c == dp) { state = 4; break; }
            switch (c) {
            case '+': case '-': p--; state = 11; break;
            case 'D': case 'E': case 'd': case 'e': state = 6; break;
            default: state = 10; p--; break;
            }
            break;
        case 4:
            found_digit = 1;
            found_decpoint = 1;
            if (manlen == 0) {
                while (c == '0') {
                    c = (signed char)*p++;
                    exp_adj--;
                }
            }
            while (DIGIT(c)) {
                if (manlen < 25) {
                    manlen++;
                    exp_adj--;
                    *manp++ = (char)(c - '0');
                }
                c = (signed char)*p++;
            }
            switch (c) {
            case '+': case '-': p--; state = 11; break;
            case 'D': case 'E': case 'd': case 'e': state = 6; break;
            default: state = 10; p--; break;
            }
            break;
        case 5:
            found_decpoint = 1;
            if (DIGIT(c)) { state = 4; p--; break; }
            state = 10;
            p = savedp;
            break;
        case 6:
            savedp = p - 2;
            if (c >= '1' && c <= '9') { state = 9; p--; break; }
            switch (c) {
            case '+': state = 7; break;
            case '-': expsign = -1; state = 7; break;
            case '0': state = 8; break;
            default: state = 10; p = savedp; break;
            }
            break;
        case 7:
            if (c >= '1' && c <= '9') { state = 9; p--; break; }
            if (c == '0') { state = 8; break; }
            state = 10;
            p = savedp;
            break;
        case 8:
            found_exponent = 1;
            while (c == '0') c = (signed char)*p++;
            if (c >= '1' && c <= '9') { state = 9; p--; break; }
            state = 10;
            p--;
            break;
        case 9:
            found_exponent = 1;
            exp = 0;
            while (DIGIT(c)) {
                exp = exp * 10 + (c - '0');
                if (exp > 0x1450) {
                    exp = 0x1451;
                    break;
                }
                c = (signed char)*p++;
            }
            while (DIGIT(c)) c = (signed char)*p++;
            state = 10;
            p--;
            break;
        case 11:
            if (implicit_E) {
                savedp = p - 1;
                switch (c) {
                case '+': state = 7; break;
                case '-': expsign = -1; state = 7; break;
                default: state = 10; p = savedp; break;
                }
            } else {
                state = 10;
                p--;
            }
            break;
        default:
            break;
        }
    }
#undef DIGIT
    *p_end_ptr = p;
    uint16_t r_w0 = 0, r_exp = 0;
    U32 r_lo = 0, r_hi = 0;
    if (found_digit) {
        if (manlen > 24) {
            if (man[23] >= 5) man[23]++;
            manp--;
            exp_adj++;
            manlen = 24;
        }
        if (manlen) {
            manp--;
            while (*manp == 0) {
                manp--;
                manlen--;
                exp_adj++;
            }
            CrtLd12 tmp;
            c_mtold12(man, manlen, &tmp);
            if (expsign < 0) exp = -exp;
            exp += exp_adj;
            if (!found_exponent) exp += scale;
            if (!found_decpoint) exp -= decpt;
            if (exp > 0x1450) {
                overflow = 1;
            } else if (exp < -0x1450) {
                underflow = 1;
            } else {
                c_multtenpow12(&tmp, exp, (U32)mult12);
                r_w0 = rd16(tmp.b + 0);
                r_lo = rd32(tmp.b + 2);
                r_exp = rd16(tmp.b + 10);
                r_hi = rd32(tmp.b + 6);
            }
        }
    }
    if (!found_digit) {
        r_w0 = 0; r_hi = 0; r_exp = 0; r_lo = 0;
        retflags = 4;
    } else if (overflow) {
        r_exp = 0x7fff; r_hi = 0x80000000u; r_w0 = 0; r_lo = 0;
        retflags = 2;
    } else if (underflow) {
        r_w0 = 0; r_hi = 0; r_exp = 0; r_lo = 0;
        retflags = 1;
    }
    wr32(pld12->b + 2, r_lo);
    wr16(pld12->b + 0, r_w0);
    wr32(pld12->b + 6, r_hi);
    wr16(pld12->b + 10, (uint16_t)(sign | r_exp));
    return retflags;
}
static void fp_strgtold12(Footprint& f, CrtLd12* pld12, const char** p_end_ptr, const char*, int, int, int, int) {
    f.add(pld12, 12, "ld12");
    f.add(p_end_ptr, 4, "end");
}
PORT_FN(0x004d6a20, "strgtold12", crt_strgtold12, fp_strgtold12)

// =====================================================================================================================
// cfout.obj: _fltout, ___dtold; _fptostr.obj
// =====================================================================================================================
// ___dtold (0x4d6240): a double to an x87 extended (denormals normalised; the sign of a zero is dropped)
static void __cdecl crt_dtold(CrtLd10* pld, const D64* px) {
    const uint16_t dx = rd16((const uint8_t*)px + 6);
    uint16_t si = (uint16_t)((dx & 0x7ff0) >> 4);
    U32 top = 0x80000000u;
    const uint16_t sgn = (uint16_t)(dx & 0x8000);
    const U32 hi20 = px->hi & 0xfffff;
    const U32 lo = px->lo;
    uint8_t* d = pld->b;
    if (si == 0) {
        if (hi20 == 0 && lo == 0) {
            wr16(d + 8, 0);
            wr32(d + 4, 0);
            wr32(d + 0, 0);
            return;
        }
        si = (uint16_t)(si + 0x3c01);
        top = 0;
    } else if (si == 0x7ff) {
        si = 0x7fff;
    } else {
        si = (uint16_t)(si + 0x3c00);
    }
    U32 mhi = (hi20 << 11) | (lo >> 21) | top;
    U32 mlo = lo << 11;
    wr32(d + 0, mlo);
    wr32(d + 4, mhi);
    while (!(mhi & 0x80000000u)) {
        si--;
        mhi = (mhi << 1) | (mlo >> 31);
        mlo <<= 1;
        wr32(d + 0, mlo);
        wr32(d + 4, mhi);
    }
    wr16(d + 8, (uint16_t)(si | sgn));
}
static void fp_dtold(Footprint& f, CrtLd10* pld, const D64*) { f.pure = true; f.add(pld, 10, "ld"); }
PORT_FN(0x004d6240, "dtold", crt_dtold, fp_dtold)

// _fltout (0x4d61d0): a double to 17 significant digits, into cfout.obj's static FOS / STRFLT (returned)
static CrtStrflt* __cdecl crt_fltout(D64 x) {
    CrtLd10 ld;
    c_dtold(&ld, &x);
    Ld10Arg a;
    memcpy(&a, &ld, 10);
    a.pad = 0;                                          // (the original pushes a stack dword here: never read)
    CrtFos* fos = (CrtFos*)(uintptr_t)G_FOS;
    const int flag = c_I10_OUTPUT(a, 17, 0, fos);
    CrtStrflt* s = (CrtStrflt*)(uintptr_t)G_STRFLT;
    s->flag = flag;
    s->mantissa = fos->man;
    s->sign = (int32_t)fos->sign;
    s->decpt = (int32_t)fos->exp;
    return s;
}
static void fp_fltout(Footprint& f, D64) {
    f.add((void*)(uintptr_t)G_FOS, sizeof(CrtFos), "_fltout's FOS");
    f.add((void*)(uintptr_t)G_STRFLT, sizeof(CrtStrflt), "_fltout's STRFLT");
}
PORT_FN(0x004d61d0, "fltout", crt_fltout, fp_fltout)

// _fptostr (0x4d6140): `digits` digits of pflt's mantissa into buf (a leading '0' kept for the carry, then dropped
// unless rounding reached it -- in which case decpt grows)
static void __cdecl crt_fptostr(char* buf, int digits, CrtStrflt* pflt) {
    const char* mantissa = pflt->mantissa;
    char* p = buf + 1;
    *buf = '0';
    while (digits > 0) {
        *p++ = *mantissa ? *mantissa++ : '0';
        digits--;
    }
    *p = 0;
    if (digits >= 0 && (signed char)*mantissa >= '5') {
        p--;
        while (*p == '9') *p-- = '0';
        (*p)++;
    }
    if (*buf == '1') {
        pflt->decpt++;
    } else {
        memmove(buf, buf + 1, strlen(buf + 1) + 1);
    }
}
static void fp_fptostr(Footprint& f, char* buf, int digits, CrtStrflt* pflt) {
    f.add(buf, (uint32_t)(digits > 0 ? digits : 0) + 2, "buffer");
    f.add(&pflt->decpt, 4, "decpt");
}
PORT_FN(0x004d6140, "fptostr", crt_fptostr, fp_fptostr)

// =====================================================================================================================
// cfin.obj: _fltin; atof.obj
// =====================================================================================================================
// _fltin (0x4d4d40): a string to a double, into cfin.obj's static FLT (through the pointer at 0x503424)
static CrtFlt* __cdecl crt_fltin(const char* str, int len, int scale, int decpt) {
    (void)len; (void)scale; (void)decpt;
    CrtLd12 ld12;
    const char* end;
    D64 dval;
    int32_t retflags = 0;
    const U32 flags = c_strgtold12(&ld12, &end, str, 0, 0, 0, 0);
    if (flags & 4) {
        dval.lo = 0;
        dval.hi = 0;
        retflags = 0x200;
    } else {
        const int r = c_ld12tod(&ld12, &dval);
        if ((flags & 2) || r == 1) retflags = 0x80;
        if ((flags & 1) || r == 2) retflags |= 0x100;
    }
    CrtFlt* pflt = *(CrtFlt* volatile*)(uintptr_t)G_PFLTIN;
    pflt->flags = retflags;
    pflt->nbytes = (int32_t)(end - str);
    memcpy(&pflt->dval, &dval, 8);
    return pflt;
}
static void fp_fltin(Footprint& f, const char*, int, int, int) {
    f.add(*(CrtFlt* volatile*)(uintptr_t)G_PFLTIN, sizeof(CrtFlt), "_fltin's FLT");
}
PORT_FN(0x004d4d40, "fltin", crt_fltin, fp_fltin)

// atof (0x4cfe40): past the white space, _fltin's double (returned in ST0, loaded from the FLT)
static double __cdecl crt_atof(const char* nptr) {
    const uint8_t* s = (const uint8_t*)nptr;
    while (crt_isctype_inl(*s, CRT_SPACE)) s++;
    const CrtFlt* r = c_fltin((const char*)s, (int)strlen((const char*)s), 0, 0);
    return *(const volatile double*)&r->dval;
}
static void fp_atof(Footprint& f, const char*) {
    f.add(*(CrtFlt* volatile*)(uintptr_t)G_PFLTIN, sizeof(CrtFlt), "_fltin's FLT");
}
PORT_FN(0x004cfe40, "atof", crt_atof, fp_atof)

// =====================================================================================================================
// atox.obj: atol / atoi (no overflow check: the sum wraps)
// =====================================================================================================================
static int32_t __cdecl crt_atol(const char* nptr) {
    const uint8_t* s = (const uint8_t*)nptr;
    while (crt_isctype_inl(*s, CRT_SPACE)) s++;
    int c = *s++;
    const int sign = c;
    if (c == '-' || c == '+') c = *s++;
    U32 total = 0;
    while (crt_isctype_inl(c, CRT_DIGIT)) {
        total = 10 * total + (U32)(c - '0');
        c = *s++;
    }
    return sign == '-' ? (int32_t)(0u - total) : (int32_t)total;
}
static void fp_atol(Footprint&, const char*) {}
PORT_FN(0x004cf8e0, "atol", crt_atol, fp_atol)

static int32_t __cdecl crt_atoi(const char* nptr) { return c_atol(nptr); }
PORT_FN(0x004cf990, "atoi", crt_atoi, fp_atol)

// =====================================================================================================================
// strtol.obj: strtoxl / strtoul
// =====================================================================================================================
// strtoxl (0x4cff50): flags 1 = unsigned (strtoul), 2 = negative, 4 = overflow, 8 = a digit was read. Bases 0 and
// 2..36 (0: from the prefix); out of range or no digits: 0 and *endptr = nptr. Overflow: ERANGE and the limit.
static U32 __cdecl crt_strtoxl(const char* nptr, const char** endptr, int ibase, int flags) {
    const uint8_t* p = (const uint8_t*)nptr;
    U32 number = 0;
    uint8_t c = *p++;
    while (crt_isctype_inl(c, CRT_SPACE)) c = *p++;
    if (c == '-') {
        c = *p++;
        flags |= 2;
    } else if (c == '+') {
        c = *p++;
    }
    if (ibase < 0 || ibase == 1 || ibase > 36) {
        if (endptr) *endptr = nptr;
        return 0;
    }
    if (ibase == 0) {
        if (c != '0') ibase = 10;
        else if (*p == 'x' || *p == 'X') ibase = 16;
        else ibase = 8;
    }
    if (ibase == 16 && c == '0' && (*p == 'x' || *p == 'X')) {
        c = p[1];
        p += 2;
    }
    const U32 maxval = 0xffffffffu / (U32)ibase;
    for (;;) {
        U32 digval;
        if (crt_isctype_inl(c, CRT_DIGIT)) {
            digval = (U32)((int)(signed char)c - '0');
        } else if (crt_isctype_inl(c, CRT_ALPHA)) {
            digval = (U32)(c_toupper((int)(signed char)c) - 'A' + 10);
        } else {
            break;
        }
        if (digval >= (U32)ibase) break;
        flags |= 8;
        if (number < maxval || (number == maxval && digval <= 0xffffffffu % (U32)ibase)) {
            number = number * (U32)ibase + digval;
        } else {
            flags |= 4;
        }
        c = *p++;
    }
    p--;
    if (!(flags & 8)) {
        if (endptr) p = (const uint8_t*)nptr;
        number = 0;
    } else if ((flags & 4) ||
               (!(flags & 1) && (((flags & 2) && number > 0x80000000u) || (!(flags & 2) && number > 0x7fffffffu)))) {
        crt_errno = CRT_ERANGE;
        if (flags & 1) number = 0xffffffffu;
        else if (flags & 2) number = 0x80000000u;
        else number = 0x7fffffffu;
    }
    if (endptr) *endptr = (const char*)p;
    if (flags & 2) number = 0u - number;
    return number;
}
static void fp_strtoxl(Footprint& f, const char*, const char** endptr, int, int) {
    if (endptr) f.add(endptr, 4, "endptr");
    f.add((void*)(uintptr_t)CRT_ERRNO_VA, 4, "errno");
}
PORT_FN(0x004cff50, "strtoxl", crt_strtoxl, fp_strtoxl)

// strtoul (0x4d01a0)
static U32 __cdecl crt_strtoul(const char* nptr, const char** endptr, int ibase) {
    return c_strtoxl(nptr, endptr, ibase, 1);
}
static void fp_strtoul(Footprint& f, const char* n, const char** e, int b) { fp_strtoxl(f, n, e, b, 1); }
PORT_FN(0x004d01a0, "strtoul", crt_strtoul, fp_strtoul)

// =====================================================================================================================
// cvt.obj: printf's floating-point formatting (reached through _cfltcvt_tab) and its %g state
// =====================================================================================================================
#define g_fmt           G8(G_FMT)
#define g_magnitude     CRT_G(int32_t, G_MAGNITUDE)
#define g_round_exp     G8(G_ROUND_EXPANSION)
#define g_pflt          CRT_G(CrtStrflt*, G_PFLT)

// _forcdecpt (0x4d09b0): '#' with no decimals -- put the decimal point after the digits (before an exponent)
static void __cdecl crt_forcdecpt(char* buffer) {
    char* p = buffer;
    if (c_tolower((int)(signed char)*p) != 'e') {
        do {
            p++;
        } while (crt_isctype_inl((int)(signed char)*p, CRT_DIGIT));   // (a signed index: the original's movsx)
    }
    char hold = *p;
    *p++ = crt_decimal_point;
    char stored;
    do {
        const char next = *p;
        stored = hold;
        *p = stored;
        hold = next;
        p++;
    } while (stored);
}
static void fp_buffer_str(Footprint& f, char* buffer) { f.add(buffer, (uint32_t)strlen(buffer) + 2, "buffer"); }
PORT_FN(0x004d09b0, "forcdecpt", crt_forcdecpt, fp_buffer_str)

// _cropzeros (0x4d0a20): %g without '#' -- drop the fraction's trailing zeros (and a bare decimal point)
static void __cdecl crt_cropzeros(char* buf) {
    const char dp = crt_decimal_point;
    char* p = buf;
    while (*p && *p != dp) p++;
    if (!*p) return;
    char* q = p + 1;
    if (*q) {
        while (*q != 'e' && *q != 'E') {
            q++;
            if (!*q) break;
        }
    }
    char* stop = q;
    q--;
    while (*q == '0') q--;
    if (*q == dp) q--;
    char ch;
    do {
        ch = *stop++;
        *++q = ch;
    } while (ch);
}
PORT_FN(0x004d0a20, "cropzeros", crt_cropzeros, fp_buffer_str)

// _positive (0x4d0a80): !(0.0 > *arg) -- fldz; fcomp qword [arg]; test ah,0x41 (NaN and zeros are "positive")
static __declspec(naked) int __cdecl crt_positive(const D64*) {
#ifdef VP_GCC
    __asm__ volatile(
        "fldz\n\t"
        "mov eax, dword ptr [esp + 4]\n\t"
        "fcomp qword ptr [eax]\n\t"
        "fnstsw ax\n\t"
        "test ah, 0x41\n\t"
        "mov eax, 1\n\t"
        "jne done%=\n\t"
        "%{load%} xor eax, eax\n\t"
        "done%=:\n\t"
        "ret"
        : :);
#else
    __asm {
        fldz
        mov eax, dword ptr [esp + 4]
        fcomp qword ptr [eax]
        fnstsw ax
        test ah, 0x41
        mov eax, 1
        jne done
        xor eax, eax
    done:
        ret
    }
#endif
}
static void fp_positive(Footprint& f, const D64*) { f.pure = true; }
PORT_FN(0x004d0a80, "positive", crt_positive, fp_positive)

// _fassign (0x4d0aa0): scanf's %f store -- a double (flag) or a float from the text, moved with integer instructions
static void __cdecl crt_fassign(int flag, char* argument, char* number) {
    if (flag) {
        D64 d;
        c_atodbl(&d, number);
        wr32(argument, d.lo);
        wr32(argument + 4, d.hi);
    } else {
        U32 fl;
        c_atoflt(&fl, number);
        wr32(argument, fl);
    }
}
static void fp_fassign(Footprint& f, int flag, char* argument, char*) { f.add(argument, flag ? 8 : 4, "argument"); }
PORT_FN(0x004d0aa0, "fassign", crt_fassign, fp_fassign)

// _cftoe (0x4d0af0): d.ddde+xxx (ndec decimals); with g_fmt (from _cftog) the digits are already in the buffer
static char* __cdecl crt_cftoe(D64* pvalue, char* buf, int ndec, int caps) {
    CrtStrflt* pflt;
    if (g_fmt) {
        pflt = g_pflt;
        c_shift(buf + (pflt->sign == '-' ? 1 : 0), ndec > 0 ? 1 : 0);
    } else {
        pflt = c_fltout(*pvalue);
        c_fptostr(buf + (pflt->sign == '-' ? 1 : 0) + (ndec > 0 ? 1 : 0), ndec + 1, pflt);
    }
    char* p = buf;
    if (pflt->sign == '-') *p++ = '-';
    if (ndec > 0) {
        *p = p[1];
        p++;
        *p = crt_decimal_point;
    }
    char* e = p + ndec + (g_fmt == 0 ? 1 : 0);
    memcpy(e, (const void*)(uintptr_t)G_EXP_STR, 6);                 // "e+000" from cvt.obj's .data
    p += ndec + (g_fmt == 0 ? 1 : 0);
    if (caps) *p = 'E';
    p++;
    if (*pflt->mantissa != '0') {
        int32_t exp = pflt->decpt - 1;
        if (exp < 0) {
            exp = -exp;
            *p = '-';
        }
        p++;
        if (exp >= 100) {
            *p = (char)(*p + exp / 100);
            exp %= 100;
        }
        p++;
        if (exp >= 10) {
            *p = (char)(*p + exp / 10);
            exp %= 10;
        }
        p[1] = (char)(p[1] + exp);
    }
    return buf;
}
static void fp_cvt_statics(Footprint& f) {
    f.add((void*)(uintptr_t)G_FOS, sizeof(CrtFos), "_fltout's FOS");
    f.add((void*)(uintptr_t)G_STRFLT, sizeof(CrtStrflt), "_fltout's STRFLT");
    f.add((void*)(uintptr_t)G_FMT, 12, "cvt.obj's %g state");
    f.add((void*)(uintptr_t)G_PFLT, 4, "cvt.obj's STRFLT*");
}
// a conversion writes at most a sign, 309 + ndec digits, the point and an exponent
static uint32_t cvt_bound(int ndec) { return 352u + (uint32_t)(ndec > 0 ? ndec : 0); }
static void fp_cftoe(Footprint& f, D64*, char* buf, int ndec, int) { fp_cvt_statics(f); f.add(buf, cvt_bound(ndec), "buffer"); }
PORT_FN(0x004d0af0, "cftoe", crt_cftoe, fp_cftoe)

// _cftof (0x4d0c30): ddd.ddd (ndec decimals)
static char* __cdecl crt_cftof(D64* pvalue, char* buf, int ndec) {
    CrtStrflt* pflt;
    if (g_fmt) {
        pflt = g_pflt;
        if (g_magnitude == ndec) {
            char* q = buf + (pflt->sign == '-' ? 1 : 0) + g_magnitude;
            q[0] = '0';
            q[1] = 0;
        }
    } else {
        pflt = c_fltout(*pvalue);
        c_fptostr(buf + (pflt->sign == '-' ? 1 : 0), pflt->decpt + ndec, pflt);
    }
    char* p = buf;
    if (pflt->sign == '-') *p++ = '-';
    if (pflt->decpt <= 0) {
        c_shift(p, 1);
        *p++ = '0';
    } else {
        p += pflt->decpt;
    }
    if (ndec > 0) {
        c_shift(p, 1);
        *p++ = crt_decimal_point;
        int32_t i = pflt->decpt;
        if (i < 0) {
            if (g_fmt) {
                i = -i;
            } else {
                i = -i;
                if (i >= ndec) i = ndec;
            }
            c_shift(p, i);
            memset(p, '0', (size_t)(U32)i);
        }
    }
    return buf;
}
static void fp_cftof(Footprint& f, D64*, char* buf, int ndec) { fp_cvt_statics(f); f.add(buf, cvt_bound(ndec), "buffer"); }
PORT_FN(0x004d0c30, "cftof", crt_cftof, fp_cftof)

// _cftog (0x4d0d30): %g -- %e when the exponent is below -4 or not below the precision, else %f; the digits once
static char* __cdecl crt_cftog(D64* pvalue, char* buf, int ndec, int caps) {
    CrtStrflt* pflt = c_fltout(*pvalue);
    g_pflt = pflt;
    g_magnitude = pflt->decpt - 1;
    char* p = buf + (g_pflt->sign == '-' ? 1 : 0);
    c_fptostr(p, ndec, g_pflt);
    pflt = g_pflt;
    const uint8_t grew = (pflt->decpt - 1 > g_magnitude) ? 1 : 0;
    g_round_exp = grew;
    const int32_t mag = pflt->decpt - 1;
    g_magnitude = mag;
    if (mag < -4 || mag >= ndec) return c_cftoe_g(pvalue, buf, ndec, caps);
    if (grew) {
        while (*p++) {}
        p[-2] = 0;
    }
    return c_cftof_g(pvalue, buf, ndec);
}
static void fp_cftog(Footprint& f, D64*, char* buf, int ndec, int) { fp_cvt_statics(f); f.add(buf, cvt_bound(ndec), "buffer"); }
PORT_FN(0x004d0d30, "cftog", crt_cftog, fp_cftog)

// _cftoe_g (0x4d0de0) / _cftof_g (0x4d0e10): the two with g_fmt set around them
static char* __cdecl crt_cftoe_g(D64* pvalue, char* buf, int ndec, int caps) {
    g_fmt = 1;
    char* r = c_cftoe(pvalue, buf, ndec, caps);
    g_fmt = 0;
    return r;
}
PORT_FN(0x004d0de0, "cftoe_g", crt_cftoe_g, fp_cftoe)

static char* __cdecl crt_cftof_g(D64* pvalue, char* buf, int ndec) {
    g_fmt = 1;
    char* r = c_cftof(pvalue, buf, ndec);
    g_fmt = 0;
    return r;
}
PORT_FN(0x004d0e10, "cftof_g", crt_cftof_g, fp_cftof)

// _cfltcvt (0x4d0e40): by format: e / E, f, anything else %g
static char* __cdecl crt_cfltcvt(D64* arg, char* buffer, int format, int precision, int caps) {
    if (format == 'e' || format == 'E') return c_cftoe(arg, buffer, precision, caps);
    if (format == 'f') return c_cftof(arg, buffer, precision);
    return c_cftog(arg, buffer, precision, caps);
}
static void fp_cfltcvt(Footprint& f, D64*, char* buf, int, int ndec, int) { fp_cvt_statics(f); f.add(buf, cvt_bound(ndec), "buffer"); }
PORT_FN(0x004d0e40, "cfltcvt", crt_cfltcvt, fp_cfltcvt)

// _shift (0x4d0eb0): the string dist places right (memmove, terminator included)
static void __cdecl crt_shift(char* s, int dist) {
    if (dist) c_memmove(s + dist, s, strlen(s) + 1);
}
static void fp_shift(Footprint& f, char* s, int dist) { f.add(s, (uint32_t)strlen(s) + 1 + (uint32_t)(dist > 0 ? dist : 0), "string"); }
PORT_FN(0x004d0eb0, "shift", crt_shift, fp_shift)

// =====================================================================================================================
// ieeemisc.obj, util.obj: _finite, _isnan, _set_exp, _decomp (the original's instructions)
// =====================================================================================================================
static __declspec(naked) int __cdecl crt_finite(D64) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov ax, word ptr [esp + 0xa]\n\t"
        "and ax, 0x7ff0\n\t"
        "sub ax, 0x7ff0\n\t"
        "cmp ax, 1\n\t"
        "%{load%} sbb eax, eax\n\t"
        "inc eax\n\t"
        "ret"
        : :);
#else
    __asm {
        mov ax, word ptr [esp + 0xa]
        and ax, 0x7ff0
        sub ax, 0x7ff0
        cmp ax, 1
        sbb eax, eax
        inc eax
        ret
    }
#endif
}
static void fp_d64(Footprint& f, D64) { f.pure = true; }
PORT_FN(0x004cf550, "finite", crt_finite, fp_d64)

static __declspec(naked) int __cdecl crt_isnan(D64) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov ax, word ptr [esp + 0xa]\n\t"
        "and ax, 0x7ff8\n\t"
        "cmp ax, 0x7ff0\n\t"
        "jne not_inf_class%=\n\t"
        "test dword ptr [esp + 8], 0x7ffff\n\t"
        "jne yes%=\n\t"
        "cmp dword ptr [esp + 4], 0\n\t"
        "jne yes%=\n\t"
        "not_inf_class%=:\n\t"
        "cmp ax, 0x7ff8\n\t"
        "je yes%=\n\t"
        "%{load%} xor eax, eax\n\t"
        "ret\n\t"
        "yes%=:\n\t"
        "mov eax, 1\n\t"
        "ret"
        : :);
#else
    __asm {
        mov ax, word ptr [esp + 0xa]
        and ax, 0x7ff8
        cmp ax, 0x7ff0
        jne not_inf_class
        test dword ptr [esp + 8], 0x7ffff
        jne yes
        cmp dword ptr [esp + 4], 0
        jne yes
    not_inf_class:
        cmp ax, 0x7ff8
        je yes
        xor eax, eax
        ret
    yes:
        mov eax, 1
        ret
    }
#endif
}
PORT_FN(0x004cf570, "isnan", crt_isnan, fp_d64)

// _set_exp (0x4d3050): x with its exponent field replaced by exp + 0x3fe; returned in ST0
static __declspec(naked) double __cdecl crt_set_exp(D64, int) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov eax, dword ptr [esp + 8]\n\t"
        "mov ecx, dword ptr [esp + 4]\n\t"
        "sub esp, 8\n\t"
        "mov dword ptr [esp + 4], eax\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "add ax, 0x3fe\n\t"
        "mov dword ptr [esp], ecx\n\t"
        "shl ax, 4\n\t"
        "mov cx, word ptr [esp + 0x12]\n\t"
        "and cx, 0x800f\n\t"
        "%{load%} or ax, cx\n\t"
        "mov word ptr [esp + 6], ax\n\t"
        "fld qword ptr [esp]\n\t"
        "add esp, 8\n\t"
        "ret"
        : :);
#else
    __asm {
        mov eax, dword ptr [esp + 8]
        mov ecx, dword ptr [esp + 4]
        sub esp, 8
        mov dword ptr [esp + 4], eax
        mov eax, dword ptr [esp + 0x14]
        add ax, 0x3fe
        mov dword ptr [esp], ecx
        shl ax, 4
        mov cx, word ptr [esp + 0x12]
        and cx, 0x800f
        or ax, cx
        mov word ptr [esp + 6], ax
        fld qword ptr [esp]
        add esp, 8
        ret
    }
#endif
}
static void fp_set_exp(Footprint& f, D64, int) { f.pure = true; }
PORT_FN(0x004d3050, "set_exp", crt_set_exp, fp_set_exp)

// _decomp (0x4d3090): frexp's core -- the mantissa in [0.5, 1) (ST0) and the exponent (*pexp); denormals normalised
static __declspec(naked) double __cdecl crt_decomp(D64, int*) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov eax, dword ptr [esp + 8]\n\t"
        "sub esp, 8\n\t"
        "and eax, 0x7fffffff\n\t"
        "push esi\n\t"
        "or eax, dword ptr [esp + 0x10]\n\t"
        "push edi\n\t"
        "jne nonzero%=\n\t"
        "%{load%} xor esi, esi\n\t"
        "mov dword ptr [esp + 8], esi\n\t"
        "mov dword ptr [esp + 0xc], esi\n\t"
        "jmp done%=\n\t"
        "nonzero%=:\n\t"
        "mov si, word ptr [esp + 0x1a]\n\t"
        "and si, 0x7ff0\n\t"
        "jne normal%=\n\t"
        "test dword ptr [esp + 0x18], 0xfffff\n\t"
        "jne denormal%=\n\t"
        "cmp dword ptr [esp + 0x14], 0\n\t"
        "je normal%=\n\t"
        "denormal%=:\n\t"
        "fldz\n\t"
        "fcomp qword ptr [esp + 0x14]\n\t"
        "mov esi, 0xfffffc03\n\t"
        "fnstsw ax\n\t"
        "test ah, 0x41\n\t"
        "mov eax, 1\n\t"
        "je neg_done%=\n\t"
        "%{load%} xor eax, eax\n\t"
        "neg_done%=:\n\t"
        "test byte ptr [esp + 0x1a], 0x10\n\t"
        "jne shifted%=\n\t"
        "mov ecx, 0x80000000\n\t"
        "mov edx, 1\n\t"
        "mov edi, 0x10\n\t"
        "shl_loop%=:\n\t"
        "shl dword ptr [esp + 0x18], 1\n\t"
        "test dword ptr [esp + 0x14], ecx\n\t"
        "je no_carry%=\n\t"
        "or dword ptr [esp + 0x18], edx\n\t"
        "no_carry%=:\n\t"
        "dec esi\n\t"
        "shl dword ptr [esp + 0x14], 1\n\t"
        "test word ptr [esp + 0x1a], di\n\t"
        "je shl_loop%=\n\t"
        "shifted%=:\n\t"
        "and word ptr [esp + 0x1a], 0xffef\n\t"
        "test eax, eax\n\t"
        "je positive_value%=\n\t"
        "or byte ptr [esp + 0x1b], 0x80\n\t"
        "positive_value%=:\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "push 0\n\t"
        "mov ecx, dword ptr [esp + 0x18]\n\t"
        "push eax\n\t"
        "push ecx\n\t"
        "call dword ptr [%c0]\n\t"
        "fstp qword ptr [esp + 0x14]\n\t"
        "add esp, 0xc\n\t"
        "jmp done%=\n\t"
        "normal%=:\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "push 0\n\t"
        "shr si, 4\n\t"
        "push eax\n\t"
        "movsx esi, si\n\t"
        "mov ecx, dword ptr [esp + 0x1c]\n\t"
        "sub esi, 0x3fe\n\t"
        "push ecx\n\t"
        "call dword ptr [%c0]\n\t"
        "fstp qword ptr [esp + 0x14]\n\t"
        "add esp, 0xc\n\t"
        "done%=:\n\t"
        "fld qword ptr [esp + 8]\n\t"
        "mov eax, dword ptr [esp + 0x1c]\n\t"
        "pop edi\n\t"
        "mov dword ptr [eax], esi\n\t"
        "pop esi\n\t"
        "add esp, 8\n\t"
        "ret"
        : : "i"(&a_set_exp));
#else
    __asm {
        mov eax, dword ptr [esp + 8]
        sub esp, 8
        and eax, 0x7fffffff
        push esi
        or eax, dword ptr [esp + 0x10]
        push edi
        jne nonzero
        xor esi, esi
        mov dword ptr [esp + 8], esi
        mov dword ptr [esp + 0xc], esi
        jmp done
    nonzero:
        mov si, word ptr [esp + 0x1a]
        and si, 0x7ff0
        jne normal
        test dword ptr [esp + 0x18], 0xfffff
        jne denormal
        cmp dword ptr [esp + 0x14], 0
        je normal
    denormal:
        fldz
        fcomp qword ptr [esp + 0x14]
        mov esi, 0xfffffc03
        fnstsw ax
        test ah, 0x41
        mov eax, 1
        je neg_done
        xor eax, eax
    neg_done:
        test byte ptr [esp + 0x1a], 0x10
        jne shifted
        mov ecx, 0x80000000
        mov edx, 1
        mov edi, 0x10
    shl_loop:
        shl dword ptr [esp + 0x18], 1
        test dword ptr [esp + 0x14], ecx
        je no_carry
        or dword ptr [esp + 0x18], edx
    no_carry:
        dec esi
        shl dword ptr [esp + 0x14], 1
        test word ptr [esp + 0x1a], di
        je shl_loop
    shifted:
        and word ptr [esp + 0x1a], 0xffef
        test eax, eax
        je positive_value
        or byte ptr [esp + 0x1b], 0x80
    positive_value:
        mov eax, dword ptr [esp + 0x18]
        push 0
        mov ecx, dword ptr [esp + 0x18]
        push eax
        push ecx
        call dword ptr [a_set_exp]
        fstp qword ptr [esp + 0x14]
        add esp, 0xc
        jmp done
    normal:
        mov eax, dword ptr [esp + 0x18]
        push 0
        shr si, 4
        push eax
        movsx esi, si
        mov ecx, dword ptr [esp + 0x1c]
        sub esi, 0x3fe
        push ecx
        call dword ptr [a_set_exp]
        fstp qword ptr [esp + 0x14]
        add esp, 0xc
    done:
        fld qword ptr [esp + 8]
        mov eax, dword ptr [esp + 0x1c]
        pop edi
        mov dword ptr [eax], esi
        pop esi
        add esp, 8
        ret
    }
#endif
}
static void fp_decomp(Footprint& f, D64, int* pexp) { f.add(pexp, 4, "exponent"); }
PORT_FN(0x004d3090, "decomp", crt_decomp, fp_decomp)

// =====================================================================================================================
// ieee87.obj, fp8.obj: _clearfp, _control87, _controlfp, the control / status word translations
// =====================================================================================================================
// _abstract_cw (0x4cfa60): the x87 control word -> the _MCW_* abstraction
static U32 __cdecl crt_abstract_cw(uint16_t cw) {
    U32 r = 0;
    if (cw & 0x01) r = 0x10;
    if (cw & 0x04) r |= 0x08;
    if (cw & 0x08) r |= 0x04;
    if (cw & 0x10) r |= 0x02;
    if (cw & 0x20) r |= 0x01;
    if (cw & 0x02) r |= 0x80000;
    switch (cw & 0xc00) {
    case 0x400: r |= 0x100; break;
    case 0x800: r |= 0x200; break;
    case 0xc00: r |= 0x300; break;
    }
    switch (cw & 0x300) {
    case 0x000: r |= 0x20000; break;
    case 0x200: r |= 0x10000; break;
    }
    if (cw & 0x1000) r |= 0x40000;
    return r;
}
static void fp_cw(Footprint& f, uint16_t) { f.pure = true; }
PORT_FN(0x004cfa60, "abstract_cw", crt_abstract_cw, fp_cw)

// _hw_cw (0x4cfb10): back to the x87 control word (only ax is defined: the original never sets eax's top half)
static uint16_t __cdecl crt_hw_cw(U32 abstr) {
    uint16_t r = 0;
    if (abstr & 0x10) r = 1;
    if (abstr & 0x08) r |= 4;
    if (abstr & 0x04) r |= 8;
    if (abstr & 0x02) r |= 0x10;
    if (abstr & 0x01) r |= 0x20;
    if (abstr & 0x80000) r |= 2;
    switch (abstr & 0x300) {
    case 0x100: r |= 0x400; break;
    case 0x200: r |= 0x800; break;
    case 0x300: r |= 0xc00; break;
    }
    switch (abstr & 0x30000) {
    case 0x00000: r |= 0x300; break;
    case 0x10000: r |= 0x200; break;
    }
    if (abstr & 0x40000) r |= 0x1000;
    return r;
}
static void fp_u32(Footprint& f, U32) { f.pure = true; }
PORT_FN(0x004cfb10, "hw_cw", crt_hw_cw, fp_u32)

// _abstract_sw (0x4cfba0): the x87 status word's exception flags -> _SW_*
static U32 __cdecl crt_abstract_sw(uint16_t sw) {
    U32 r = 0;
    if (sw & 0x01) r = 0x10;
    if (sw & 0x04) r |= 0x08;
    if (sw & 0x08) r |= 0x04;
    if (sw & 0x10) r |= 0x02;
    if (sw & 0x20) r |= 0x01;
    if (sw & 0x02) r |= 0x80000;
    return r;
}
PORT_FN(0x004cfba0, "abstract_sw", crt_abstract_sw, fp_cw)

// _clearfp (0x4cf9e0): the status word's flags (abstracted), then fnclex
static __declspec(naked) U32 __cdecl crt_clearfp() {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 4\n\t"
        "fnstsw word ptr [esp + 2]\n\t"
        "fnclex\n\t"
        "mov eax, dword ptr [esp + 2]\n\t"
        "push eax\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 8\n\t"
        "ret"
        : : "i"(&a_abstract_sw));
#else
    __asm {
        sub esp, 4
        fnstsw word ptr [esp + 2]
        fnclex
        mov eax, dword ptr [esp + 2]
        push eax
        call dword ptr [a_abstract_sw]
        add esp, 8
        ret
    }
#endif
}
static void fp_fpu_state(Footprint& f) { f.replay_only = k_asm_reason; }
PORT_FN(0x004cf9e0, "clearfp", crt_clearfp, fp_fpu_state)
CRT_SHADOW_ORIGINAL(crt_clearfp)

// _control87 (0x4cfa00): (old & ~mask) | (new & mask), loaded into the FPU; returns it (abstracted)
static __declspec(naked) U32 __cdecl crt_control87(U32, U32) {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 4\n\t"
        "push esi\n\t"
        "wait\n\t"
        "fnstcw word ptr [esp + 6]\n\t"
        "mov eax, dword ptr [esp + 6]\n\t"
        "push eax\n\t"
        "call dword ptr [%c0]\n\t"
        "mov ecx, dword ptr [esp + 0x14]\n\t"
        "add esp, 4\n\t"
        "%{load%} mov esi, ecx\n\t"
        "and ecx, dword ptr [esp + 0xc]\n\t"
        "not esi\n\t"
        "%{load%} and esi, eax\n\t"
        "%{load%} or esi, ecx\n\t"
        "push esi\n\t"
        "call dword ptr [%c1]\n\t"
        "mov word ptr [esp + 8], ax\n\t"
        "add esp, 4\n\t"
        "fldcw word ptr [esp + 4]\n\t"
        "%{load%} mov eax, esi\n\t"
        "pop esi\n\t"
        "add esp, 4\n\t"
        "ret"
        : : "i"(&a_abstract_cw), "i"(&a_hw_cw));
#else
    __asm {
        sub esp, 4
        push esi
        wait
        fnstcw word ptr [esp + 6]
        mov eax, dword ptr [esp + 6]
        push eax
        call dword ptr [a_abstract_cw]
        mov ecx, dword ptr [esp + 0x14]
        add esp, 4
        mov esi, ecx
        and ecx, dword ptr [esp + 0xc]
        not esi
        and esi, eax
        or esi, ecx
        push esi
        call dword ptr [a_hw_cw]
        mov word ptr [esp + 8], ax
        add esp, 4
        fldcw word ptr [esp + 4]
        mov eax, esi
        pop esi
        add esp, 4
        ret
    }
#endif
}
static void fp_control87(Footprint& f, U32, U32) { f.replay_only = k_asm_reason; }
PORT_FN(0x004cfa00, "control87", crt_control87, fp_control87)
CRT_SHADOW_ORIGINAL(crt_control87)

// _controlfp (0x4cfa40): _control87 with _EM_DENORMAL kept out of the mask
static __declspec(naked) U32 __cdecl crt_controlfp(U32, U32) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov eax, dword ptr [esp + 8]\n\t"
        "mov ecx, dword ptr [esp + 4]\n\t"
        "and eax, 0xfff7ffff\n\t"
        "push eax\n\t"
        "push ecx\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 8\n\t"
        "ret"
        : : "i"(&a_control87));
#else
    __asm {
        mov eax, dword ptr [esp + 8]
        mov ecx, dword ptr [esp + 4]
        and eax, 0xfff7ffff
        push eax
        push ecx
        call dword ptr [a_control87]
        add esp, 8
        ret
    }
#endif
}
PORT_FN(0x004cfa40, "controlfp", crt_controlfp, fp_control87)
CRT_SHADOW_ORIGINAL(crt_controlfp)

// _setdefaultprecision (0x4d0820): _controlfp(_PC_53, _MCW_PC)
static __declspec(naked) void __cdecl crt_setdefaultprecision() {
#ifdef VP_GCC
    __asm__ volatile(
        "push 0x30000\n\t"
        "push 0x10000\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 8\n\t"
        "ret"
        : : "i"(&a_controlfp));
#else
    __asm {
        push 0x30000
        push 0x10000
        call dword ptr [a_controlfp]
        add esp, 8
        ret
    }
#endif
}
PORT_FN(0x004d0820, "setdefaultprecision", crt_setdefaultprecision, fp_fpu_state)
CRT_SHADOW_ORIGINAL(crt_setdefaultprecision)

// =====================================================================================================================
// fpctrl.obj: _statfp, _clrfp, _ctrlfp, _set_statfp
// =====================================================================================================================
static __declspec(naked) int __cdecl crt_statfp() {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 4\n\t"
        "wait\n\t"
        "fnstsw word ptr [esp + 2]\n\t"
        "movsx eax, word ptr [esp + 2]\n\t"
        "add esp, 4\n\t"
        "ret"
        : :);
#else
    __asm {
        sub esp, 4
        wait
        fnstsw word ptr [esp + 2]
        movsx eax, word ptr [esp + 2]
        add esp, 4
        ret
    }
#endif
}
PORT_FN(0x004d2f70, "statfp", crt_statfp, fp_fpu_state)
CRT_SHADOW_ORIGINAL(crt_statfp)

static __declspec(naked) int __cdecl crt_clrfp() {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 4\n\t"
        "fnstsw word ptr [esp + 2]\n\t"
        "fnclex\n\t"
        "movsx eax, word ptr [esp + 2]\n\t"
        "add esp, 4\n\t"
        "ret"
        : :);
#else
    __asm {
        sub esp, 4
        fnstsw word ptr [esp + 2]
        fnclex
        movsx eax, word ptr [esp + 2]
        add esp, 4
        ret
    }
#endif
}
PORT_FN(0x004d2f90, "clrfp", crt_clrfp, fp_fpu_state)
CRT_SHADOW_ORIGINAL(crt_clrfp)

static __declspec(naked) int __cdecl crt_ctrlfp(U32, U32) {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 4\n\t"
        "wait\n\t"
        "fnstcw word ptr [esp]\n\t"
        "mov ax, word ptr [esp + 0xc]\n\t"
        "mov edx, dword ptr [esp + 8]\n\t"
        "%{load%} mov cx, ax\n\t"
        "not cx\n\t"
        "%{load%} and dx, ax\n\t"
        "and cx, word ptr [esp]\n\t"
        "%{load%} or cx, dx\n\t"
        "mov word ptr [esp + 2], cx\n\t"
        "fldcw word ptr [esp + 2]\n\t"
        "movsx eax, word ptr [esp]\n\t"
        "add esp, 4\n\t"
        "ret"
        : :);
#else
    __asm {
        sub esp, 4
        wait
        fnstcw word ptr [esp]
        mov ax, word ptr [esp + 0xc]
        mov edx, dword ptr [esp + 8]
        mov cx, ax
        not cx
        and dx, ax
        and cx, word ptr [esp]
        or cx, dx
        mov word ptr [esp + 2], cx
        fldcw word ptr [esp + 2]
        movsx eax, word ptr [esp]
        add esp, 4
        ret
    }
#endif
}
PORT_FN(0x004d2fb0, "ctrlfp", crt_ctrlfp, fp_control87)
CRT_SHADOW_ORIGINAL(crt_ctrlfp)

// _set_statfp (0x4d2ff0): raise the given exceptions by doing operations that cause them
static __declspec(naked) void __cdecl crt_set_statfp(U32) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov ecx, dword ptr [esp + 4]\n\t"
        "sub esp, 0xc\n\t"
        "test cl, 1\n\t"
        "je no_inv%=\n\t"
        "fld tbyte ptr ds:[0x502880]\n\t"
        "fistp dword ptr [esp]\n\t"
        "wait\n\t"
        "no_inv%=:\n\t"
        "test cl, 8\n\t"
        "je no_ovf%=\n\t"
        "wait\n\t"
        "fnstsw ax\n\t"
        "fld tbyte ptr ds:[0x502880]\n\t"
        "fstp qword ptr [esp + 4]\n\t"
        "wait\n\t"
        "wait\n\t"
        "fnstsw ax\n\t"
        "no_ovf%=:\n\t"
        "test cl, 0x10\n\t"
        "je no_unf%=\n\t"
        "fld tbyte ptr ds:[0x502890]\n\t"
        "fstp qword ptr [esp + 4]\n\t"
        "wait\n\t"
        "no_unf%=:\n\t"
        "test cl, 4\n\t"
        "je no_zdiv%=\n\t"
        "fldz\n\t"
        "fld1\n\t"
        "fdivrp st(1), st\n\t"
        "fstp st(0)\n\t"
        "wait\n\t"
        "no_zdiv%=:\n\t"
        "test cl, 0x20\n\t"
        "je no_inexact%=\n\t"
        "fldpi\n\t"
        "fstp qword ptr [esp + 4]\n\t"
        "wait\n\t"
        "no_inexact%=:\n\t"
        "add esp, 0xc\n\t"
        "ret"
        : :);
#else
    __asm {
        mov ecx, dword ptr [esp + 4]
        sub esp, 0xc
        test cl, 1
        je no_inv
        fld tbyte ptr ds:[0x502880]
        fistp dword ptr [esp]
        wait
    no_inv:
        test cl, 8
        je no_ovf
        wait
        fnstsw ax
        fld tbyte ptr ds:[0x502880]
        fstp qword ptr [esp + 4]
        wait
        wait
        fnstsw ax
    no_ovf:
        test cl, 0x10
        je no_unf
        fld tbyte ptr ds:[0x502890]
        fstp qword ptr [esp + 4]
        wait
    no_unf:
        test cl, 4
        je no_zdiv
        fldz
        fld1
        fdivrp st(1), st                        // (de f1, the original's bytes)
        fstp st(0)
        wait
    no_zdiv:
        test cl, 0x20
        je no_inexact
        fldpi
        fstp qword ptr [esp + 4]
        wait
    no_inexact:
        add esp, 0xc
        ret
    }
#endif
}
static void fp_set_statfp(Footprint& f, U32) { f.replay_only = k_asm_reason; }
PORT_FN(0x004d2ff0, "set_statfp", crt_set_statfp, fp_set_statfp)
CRT_SHADOW_ORIGINAL(crt_set_statfp)

// =====================================================================================================================
// ftol.obj, llmul.obj, lldiv.obj, ulldiv.obj, ullrem.obj
// =====================================================================================================================
// __ftol (0x4cf108): ST0 to a 64-bit integer, truncating (fistp qword with the rounding set to chop): edx:eax
static __declspec(naked) void __cdecl crt_ftol() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "add esp, -0xc\n\t"
        "wait\n\t"
        "fnstcw word ptr [ebp - 2]\n\t"
        "wait\n\t"
        "mov ax, word ptr [ebp - 2]\n\t"
        "or ah, 0xc\n\t"
        "mov word ptr [ebp - 4], ax\n\t"
        "fldcw word ptr [ebp - 4]\n\t"
        "fistp qword ptr [ebp - 0xc]\n\t"
        "fldcw word ptr [ebp - 2]\n\t"
        "mov eax, dword ptr [ebp - 0xc]\n\t"
        "mov edx, dword ptr [ebp - 8]\n\t"
        "leave\n\t"
        "ret"
        : :);
#else
    __asm {
        push ebp
        mov ebp, esp
        add esp, -0xc
        wait
        fnstcw word ptr [ebp - 2]
        wait
        mov ax, word ptr [ebp - 2]
        or ah, 0xc
        mov word ptr [ebp - 4], ax
        fldcw word ptr [ebp - 4]
        fistp qword ptr [ebp - 0xc]
        fldcw word ptr [ebp - 2]
        mov eax, dword ptr [ebp - 0xc]
        mov edx, dword ptr [ebp - 8]
        leave
        ret
    }
#endif
}
static void fp_asm(Footprint& f) { f.replay_only = k_asm_reason; }
PORT_FN(0x004cf108, "ftol", crt_ftol, fp_asm)
CRT_SHADOW_ORIGINAL(crt_ftol)

// __allmul (0x4d6300): 64 x 64 -> 64, the callee pops its 16 bytes of arguments
static __declspec(naked) void __cdecl crt_allmul() {
#ifdef VP_GCC
    __asm__ volatile(
        "mov eax, dword ptr [esp + 8]\n\t"
        "mov ecx, dword ptr [esp + 0x10]\n\t"
        "%{load%} or ecx, eax\n\t"
        "mov ecx, dword ptr [esp + 0xc]\n\t"
        "jne hard%=\n\t"
        "mov eax, dword ptr [esp + 4]\n\t"
        "mul ecx\n\t"
        "ret 0x10\n\t"
        "hard%=:\n\t"
        "push ebx\n\t"
        "mul ecx\n\t"
        "%{load%} mov ebx, eax\n\t"
        "mov eax, dword ptr [esp + 8]\n\t"
        "mul dword ptr [esp + 0x14]\n\t"
        "%{load%} add ebx, eax\n\t"
        "mov eax, dword ptr [esp + 8]\n\t"
        "mul ecx\n\t"
        "%{load%} add edx, ebx\n\t"
        "pop ebx\n\t"
        "ret 0x10"
        : :);
#else
    __asm {
        mov eax, dword ptr [esp + 8]
        mov ecx, dword ptr [esp + 0x10]
        or ecx, eax
        mov ecx, dword ptr [esp + 0xc]
        jne hard
        mov eax, dword ptr [esp + 4]
        mul ecx
        ret 0x10
    hard:
        push ebx
        mul ecx
        mov ebx, eax
        mov eax, dword ptr [esp + 8]
        mul dword ptr [esp + 0x14]
        add ebx, eax
        mov eax, dword ptr [esp + 8]
        mul ecx
        add edx, ebx
        pop ebx
        ret 0x10
    }
#endif
}
PORT_FN(0x004d6300, "allmul", crt_allmul, fp_asm)
CRT_SHADOW_ORIGINAL(crt_allmul)

// __alldiv (0x4cfea0): signed 64 / 64 (a divisor whose high dword is non-zero: the shift-down estimate and one fix-up)
static __declspec(naked) void __cdecl crt_alldiv() {
#ifdef VP_GCC
    __asm__ volatile(
        "push edi\n\t"
        "push esi\n\t"
        "push ebx\n\t"
        "%{load%} xor edi, edi\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "%{load%} or eax, eax\n\t"
        "jge a_pos%=\n\t"
        "inc edi\n\t"
        "mov edx, dword ptr [esp + 0x10]\n\t"
        "neg eax\n\t"
        "neg edx\n\t"
        "sbb eax, 0\n\t"
        "mov dword ptr [esp + 0x14], eax\n\t"
        "mov dword ptr [esp + 0x10], edx\n\t"
        "a_pos%=:\n\t"
        "mov eax, dword ptr [esp + 0x1c]\n\t"
        "%{load%} or eax, eax\n\t"
        "jge b_pos%=\n\t"
        "inc edi\n\t"
        "mov edx, dword ptr [esp + 0x18]\n\t"
        "neg eax\n\t"
        "neg edx\n\t"
        "sbb eax, 0\n\t"
        "mov dword ptr [esp + 0x1c], eax\n\t"
        "mov dword ptr [esp + 0x18], edx\n\t"
        "b_pos%=:\n\t"
        "%{load%} or eax, eax\n\t"
        "jne hard%=\n\t"
        "mov ecx, dword ptr [esp + 0x18]\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "%{load%} xor edx, edx\n\t"
        "div ecx\n\t"
        "%{load%} mov ebx, eax\n\t"
        "mov eax, dword ptr [esp + 0x10]\n\t"
        "div ecx\n\t"
        "%{load%} mov edx, ebx\n\t"
        "jmp sign%=\n\t"
        "hard%=:\n\t"
        "%{load%} mov ebx, eax\n\t"
        "mov ecx, dword ptr [esp + 0x18]\n\t"
        "mov edx, dword ptr [esp + 0x14]\n\t"
        "mov eax, dword ptr [esp + 0x10]\n\t"
        "shr_loop%=:\n\t"
        "shr ebx, 1\n\t"
        "rcr ecx, 1\n\t"
        "shr edx, 1\n\t"
        "rcr eax, 1\n\t"
        "%{load%} or ebx, ebx\n\t"
        "jne shr_loop%=\n\t"
        "div ecx\n\t"
        "%{load%} mov esi, eax\n\t"
        "mul dword ptr [esp + 0x1c]\n\t"
        "%{load%} mov ecx, eax\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "mul esi\n\t"
        "%{load%} add edx, ecx\n\t"
        "jb fix%=\n\t"
        "cmp edx, dword ptr [esp + 0x14]\n\t"
        "ja fix%=\n\t"
        "jb ok%=\n\t"
        "cmp eax, dword ptr [esp + 0x10]\n\t"
        "jbe ok%=\n\t"
        "fix%=:\n\t"
        "dec esi\n\t"
        "ok%=:\n\t"
        "%{load%} xor edx, edx\n\t"
        "%{load%} mov eax, esi\n\t"
        "sign%=:\n\t"
        "dec edi\n\t"
        "jne done%=\n\t"
        "neg edx\n\t"
        "neg eax\n\t"
        "sbb edx, 0\n\t"
        "done%=:\n\t"
        "pop ebx\n\t"
        "pop esi\n\t"
        "pop edi\n\t"
        "ret 0x10"
        : :);
#else
    __asm {
        push edi
        push esi
        push ebx
        xor edi, edi
        mov eax, dword ptr [esp + 0x14]
        or eax, eax
        jge a_pos
        inc edi
        mov edx, dword ptr [esp + 0x10]
        neg eax
        neg edx
        sbb eax, 0
        mov dword ptr [esp + 0x14], eax
        mov dword ptr [esp + 0x10], edx
    a_pos:
        mov eax, dword ptr [esp + 0x1c]
        or eax, eax
        jge b_pos
        inc edi
        mov edx, dword ptr [esp + 0x18]
        neg eax
        neg edx
        sbb eax, 0
        mov dword ptr [esp + 0x1c], eax
        mov dword ptr [esp + 0x18], edx
    b_pos:
        or eax, eax
        jne hard
        mov ecx, dword ptr [esp + 0x18]
        mov eax, dword ptr [esp + 0x14]
        xor edx, edx
        div ecx
        mov ebx, eax
        mov eax, dword ptr [esp + 0x10]
        div ecx
        mov edx, ebx
        jmp sign
    hard:
        mov ebx, eax
        mov ecx, dword ptr [esp + 0x18]
        mov edx, dword ptr [esp + 0x14]
        mov eax, dword ptr [esp + 0x10]
    shr_loop:
        shr ebx, 1
        rcr ecx, 1
        shr edx, 1
        rcr eax, 1
        or ebx, ebx
        jne shr_loop
        div ecx
        mov esi, eax
        mul dword ptr [esp + 0x1c]
        mov ecx, eax
        mov eax, dword ptr [esp + 0x18]
        mul esi
        add edx, ecx
        jb fix
        cmp edx, dword ptr [esp + 0x14]
        ja fix
        jb ok
        cmp eax, dword ptr [esp + 0x10]
        jbe ok
    fix:
        dec esi
    ok:
        xor edx, edx
        mov eax, esi
    sign:
        dec edi
        jne done
        neg edx
        neg eax
        sbb edx, 0
    done:
        pop ebx
        pop esi
        pop edi
        ret 0x10
    }
#endif
}
PORT_FN(0x004cfea0, "alldiv", crt_alldiv, fp_asm)
CRT_SHADOW_ORIGINAL(crt_alldiv)

// __aulldiv (0x4d6750): unsigned 64 / 64
static __declspec(naked) void __cdecl crt_aulldiv() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebx\n\t"
        "push esi\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "%{load%} or eax, eax\n\t"
        "jne hard%=\n\t"
        "mov ecx, dword ptr [esp + 0x14]\n\t"
        "mov eax, dword ptr [esp + 0x10]\n\t"
        "%{load%} xor edx, edx\n\t"
        "div ecx\n\t"
        "%{load%} mov ebx, eax\n\t"
        "mov eax, dword ptr [esp + 0xc]\n\t"
        "div ecx\n\t"
        "%{load%} mov edx, ebx\n\t"
        "jmp done%=\n\t"
        "hard%=:\n\t"
        "%{load%} mov ecx, eax\n\t"
        "mov ebx, dword ptr [esp + 0x14]\n\t"
        "mov edx, dword ptr [esp + 0x10]\n\t"
        "mov eax, dword ptr [esp + 0xc]\n\t"
        "shr_loop%=:\n\t"
        "shr ecx, 1\n\t"
        "rcr ebx, 1\n\t"
        "shr edx, 1\n\t"
        "rcr eax, 1\n\t"
        "%{load%} or ecx, ecx\n\t"
        "jne shr_loop%=\n\t"
        "div ebx\n\t"
        "%{load%} mov esi, eax\n\t"
        "mul dword ptr [esp + 0x18]\n\t"
        "%{load%} mov ecx, eax\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "mul esi\n\t"
        "%{load%} add edx, ecx\n\t"
        "jb fix%=\n\t"
        "cmp edx, dword ptr [esp + 0x10]\n\t"
        "ja fix%=\n\t"
        "jb ok%=\n\t"
        "cmp eax, dword ptr [esp + 0xc]\n\t"
        "jbe ok%=\n\t"
        "fix%=:\n\t"
        "dec esi\n\t"
        "ok%=:\n\t"
        "%{load%} xor edx, edx\n\t"
        "%{load%} mov eax, esi\n\t"
        "done%=:\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "ret 0x10"
        : :);
#else
    __asm {
        push ebx
        push esi
        mov eax, dword ptr [esp + 0x18]
        or eax, eax
        jne hard
        mov ecx, dword ptr [esp + 0x14]
        mov eax, dword ptr [esp + 0x10]
        xor edx, edx
        div ecx
        mov ebx, eax
        mov eax, dword ptr [esp + 0xc]
        div ecx
        mov edx, ebx
        jmp done
    hard:
        mov ecx, eax
        mov ebx, dword ptr [esp + 0x14]
        mov edx, dword ptr [esp + 0x10]
        mov eax, dword ptr [esp + 0xc]
    shr_loop:
        shr ecx, 1
        rcr ebx, 1
        shr edx, 1
        rcr eax, 1
        or ecx, ecx
        jne shr_loop
        div ebx
        mov esi, eax
        mul dword ptr [esp + 0x18]
        mov ecx, eax
        mov eax, dword ptr [esp + 0x14]
        mul esi
        add edx, ecx
        jb fix
        cmp edx, dword ptr [esp + 0x10]
        ja fix
        jb ok
        cmp eax, dword ptr [esp + 0xc]
        jbe ok
    fix:
        dec esi
    ok:
        xor edx, edx
        mov eax, esi
    done:
        pop esi
        pop ebx
        ret 0x10
    }
#endif
}
PORT_FN(0x004d6750, "aulldiv", crt_aulldiv, fp_asm)
CRT_SHADOW_ORIGINAL(crt_aulldiv)

// __aullrem (0x4d67c0): unsigned 64 % 64
static __declspec(naked) void __cdecl crt_aullrem() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebx\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "%{load%} or eax, eax\n\t"
        "jne hard%=\n\t"
        "mov ecx, dword ptr [esp + 0x10]\n\t"
        "mov eax, dword ptr [esp + 0xc]\n\t"
        "%{load%} xor edx, edx\n\t"
        "div ecx\n\t"
        "mov eax, dword ptr [esp + 8]\n\t"
        "div ecx\n\t"
        "%{load%} mov eax, edx\n\t"
        "%{load%} xor edx, edx\n\t"
        "jmp done%=\n\t"
        "hard%=:\n\t"
        "%{load%} mov ecx, eax\n\t"
        "mov ebx, dword ptr [esp + 0x10]\n\t"
        "mov edx, dword ptr [esp + 0xc]\n\t"
        "mov eax, dword ptr [esp + 8]\n\t"
        "shr_loop%=:\n\t"
        "shr ecx, 1\n\t"
        "rcr ebx, 1\n\t"
        "shr edx, 1\n\t"
        "rcr eax, 1\n\t"
        "%{load%} or ecx, ecx\n\t"
        "jne shr_loop%=\n\t"
        "div ebx\n\t"
        "%{load%} mov ecx, eax\n\t"
        "mul dword ptr [esp + 0x14]\n\t"
        "xchg ecx, eax\n\t"
        "mul dword ptr [esp + 0x10]\n\t"
        "%{load%} add edx, ecx\n\t"
        "jb fix%=\n\t"
        "cmp edx, dword ptr [esp + 0xc]\n\t"
        "ja fix%=\n\t"
        "jb ok%=\n\t"
        "cmp eax, dword ptr [esp + 8]\n\t"
        "jbe ok%=\n\t"
        "fix%=:\n\t"
        "sub eax, dword ptr [esp + 0x10]\n\t"
        "sbb edx, dword ptr [esp + 0x14]\n\t"
        "ok%=:\n\t"
        "sub eax, dword ptr [esp + 8]\n\t"
        "sbb edx, dword ptr [esp + 0xc]\n\t"
        "neg edx\n\t"
        "neg eax\n\t"
        "sbb edx, 0\n\t"
        "done%=:\n\t"
        "pop ebx\n\t"
        "ret 0x10"
        : :);
#else
    __asm {
        push ebx
        mov eax, dword ptr [esp + 0x14]
        or eax, eax
        jne hard
        mov ecx, dword ptr [esp + 0x10]
        mov eax, dword ptr [esp + 0xc]
        xor edx, edx
        div ecx
        mov eax, dword ptr [esp + 8]
        div ecx
        mov eax, edx
        xor edx, edx
        jmp done
    hard:
        mov ecx, eax
        mov ebx, dword ptr [esp + 0x10]
        mov edx, dword ptr [esp + 0xc]
        mov eax, dword ptr [esp + 8]
    shr_loop:
        shr ecx, 1
        rcr ebx, 1
        shr edx, 1
        rcr eax, 1
        or ecx, ecx
        jne shr_loop
        div ebx
        mov ecx, eax
        mul dword ptr [esp + 0x14]
        xchg ecx, eax
        mul dword ptr [esp + 0x10]
        add edx, ecx
        jb fix
        cmp edx, dword ptr [esp + 0xc]
        ja fix
        jb ok
        cmp eax, dword ptr [esp + 8]
        jbe ok
    fix:
        sub eax, dword ptr [esp + 0x10]
        sbb edx, dword ptr [esp + 0x14]
    ok:
        sub eax, dword ptr [esp + 8]
        sbb edx, dword ptr [esp + 0xc]
        neg edx
        neg eax
        sbb edx, 0
    done:
        pop ebx
        ret 0x10
    }
#endif
}
PORT_FN(0x004d67c0, "aullrem", crt_aullrem, fp_asm)
CRT_SHADOW_ORIGINAL(crt_aullrem)

// =====================================================================================================================
// 87ctriga.obj, 87fmod.obj: the entry stubs -- the operation's jump table in edx, into the dispatchers. The C forms
// (asin...) take doubles on the stack; the _CI forms take ST0 (ST1, ST0 for two operands).
// =====================================================================================================================
#ifdef VP_GCC
#define CRT_TRAN_STUB(NAME, TABLE, TARGET)                                                                         \
    static __declspec(naked) void __cdecl NAME() {                                                                 \
        __asm__ volatile("mov edx, " #TABLE "\n\t"                                                                 \
                         "jmp dword ptr [%c0]"                                                                     \
                         : : "i"(&TARGET));                                                                        \
    }
#else
#define CRT_TRAN_STUB(NAME, TABLE, TARGET)                                                                         \
    static __declspec(naked) void __cdecl NAME() {                                                                 \
        __asm { mov edx, TABLE }                                                                                   \
        __asm { jmp dword ptr [TARGET] }                                                                           \
    }
#endif
CRT_TRAN_STUB(crt_asin, 0x00502780, a_ctrandisp1)
PORT_FN(0x004cf050, "asin", crt_asin, fp_asm)
CRT_SHADOW_ORIGINAL(crt_asin)
CRT_TRAN_STUB(crt_acos, 0x005027a0, a_ctrandisp1)
PORT_FN(0x004cf05a, "acos", crt_acos, fp_asm)
CRT_SHADOW_ORIGINAL(crt_acos)
CRT_TRAN_STUB(crt_atan, 0x005027c0, a_ctrandisp1)
PORT_FN(0x004cf061, "atan", crt_atan, fp_asm)
CRT_SHADOW_ORIGINAL(crt_atan)
CRT_TRAN_STUB(crt_atan2, 0x005027e0, a_ctrandisp2)
PORT_FN(0x004cf068, "atan2", crt_atan2, fp_asm)
CRT_SHADOW_ORIGINAL(crt_atan2)
CRT_TRAN_STUB(crt_CIasin, 0x00502780, a_cintrindisp1)
PORT_FN(0x004cf072, "CIasin", crt_CIasin, fp_asm)
CRT_SHADOW_ORIGINAL(crt_CIasin)
CRT_TRAN_STUB(crt_CIacos, 0x005027a0, a_cintrindisp1)
PORT_FN(0x004cf07c, "CIacos", crt_CIacos, fp_asm)
CRT_SHADOW_ORIGINAL(crt_CIacos)
CRT_TRAN_STUB(crt_CIatan, 0x005027c0, a_cintrindisp1)
PORT_FN(0x004cf083, "CIatan", crt_CIatan, fp_asm)
CRT_SHADOW_ORIGINAL(crt_CIatan)
CRT_TRAN_STUB(crt_CIatan2, 0x005027e0, a_cintrindisp2)
PORT_FN(0x004cf08a, "CIatan2", crt_CIatan2, fp_asm)
CRT_SHADOW_ORIGINAL(crt_CIatan2)
CRT_TRAN_STUB(crt_fmod, 0x00502630, a_ctrandisp2)
PORT_FN(0x004cf360, "fmod", crt_fmod, fp_asm)
CRT_SHADOW_ORIGINAL(crt_fmod)
CRT_TRAN_STUB(crt_CIfmod, 0x00502630, a_cintrindisp2)
PORT_FN(0x004cf36a, "CIfmod", crt_CIfmod, fp_asm)
CRT_SHADOW_ORIGINAL(crt_CIfmod)

// =====================================================================================================================
// 87cdisp.obj: the dispatchers. A frame of 0x2a0 bytes the 87disp routines share through ebp: the saved control word
// at ebp-0xa4, the error class at ebp-0x90, the operands at ebp-0x86 / -0x7e, the result at ebp-0x76, the table at
// ebp-0x94 (an _exception record for _87except at ebp-0x8e). The common exit (ctrandisp2's tail, 0x4d1fbe / 0x4d1fc5):
// the result's error checks (overflow / underflow rescaled by 2^+-1536, or the error class trandisp set), then
// _87except, and the caller's control word back.
// =====================================================================================================================
// the second entry (0x4d1fc5): the _CI dispatchers set 0x5d5608 themselves and come in here
static __declspec(naked) void crt_cdisp_tail_fc5() {
#ifdef VP_GCC
    __asm__ volatile(
        "cmp dword ptr ds:[0x5024b4], 0\n\t"
        "jne restore%=\n\t"
        "fst qword ptr ds:[0x5d5600]\n\t"
        "mov al, byte ptr [ebp - 0x90]\n\t"
        "%{load%} or al, al\n\t"
        "je check_status%=\n\t"
        "cmp al, 0xff\n\t"
        "je check_range%=\n\t"
        "cmp al, 0xfe\n\t"
        "je check_range%=\n\t"
        "%{load%} or al, al\n\t"
        "je restore%=\n\t"
        "movsx eax, al\n\t"
        "mov dword ptr [ebp - 0x8e], eax\n\t"
        "jmp raise%=\n\t"
        "check_status%=:\n\t"
        "mov ax, word ptr [ebp - 0xa4]\n\t"
        "and ax, 0x20\n\t"
        "jne restore%=\n\t"
        "wait\n\t"
        "fnstsw ax\n\t"
        "and ax, 0x20\n\t"
        "je restore%=\n\t"
        "mov dword ptr [ebp - 0x8e], 8\n\t"
        "jmp raise%=\n\t"
        "restore%=:\n\t"
        "fldcw word ptr [ebp - 0xa4]\n\t"
        "wait\n\t"
        "ret\n\t"
        "check_range%=:\n\t"
        "mov ax, word ptr ds:[0x5d5606]\n\t"
        "and ax, 0x7ff0\n\t"
        "%{load%} or ax, ax\n\t"
        "je underflow%=\n\t"
        "cmp ax, 0x7ff0\n\t"
        "je overflow%=\n\t"
        "jmp check_status%=\n\t"
        "underflow%=:\n\t"
        "mov dword ptr [ebp - 0x8e], 4\n\t"
        "fld qword ptr ds:[0x4e01e8]\n\t"
        "fxch st(1)\n\t"
        "fscale\n\t"
        "fstp st(1)\n\t"
        "fld st(0)\n\t"
        "fabs\n\t"
        "fcomp qword ptr ds:[0x4e01d8]\n\t"
        "wait\n\t"
        "fnstsw ax\n\t"
        "sahf\n\t"
        "jae raise%=\n\t"
        "fmul qword ptr ds:[0x4e01f8]\n\t"
        "jmp raise%=\n\t"
        "overflow%=:\n\t"
        "mov dword ptr [ebp - 0x8e], 3\n\t"
        "fld qword ptr ds:[0x4e01e0]\n\t"
        "fxch st(1)\n\t"
        "fscale\n\t"
        "fstp st(1)\n\t"
        "fld st(0)\n\t"
        "fabs\n\t"
        "fcomp qword ptr ds:[0x4e01d0]\n\t"
        "wait\n\t"
        "fnstsw ax\n\t"
        "sahf\n\t"
        "jbe raise%=\n\t"
        "fmul qword ptr ds:[0x4e01f0]\n\t"
        "raise%=:\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "mov ebx, dword ptr [ebp - 0x94]\n\t"
        "inc ebx\n\t"
        "mov dword ptr [ebp - 0x8a], ebx\n\t"
        "cmp byte ptr ds:[0x5d5608], 0\n\t"
        "jne have_args%=\n\t"
        "cld\n\t"
        "lea esi, [ebp + 8]\n\t"
        "lea edi, [ebp - 0x86]\n\t"
        "movs dword ptr es:[edi], dword ptr [esi]\n\t"
        "movs dword ptr es:[edi], dword ptr [esi]\n\t"
        "cmp byte ptr [ebx + 0xc], 1\n\t"
        "je have_args%=\n\t"
        "lea esi, [ebp + 0x10]\n\t"
        "lea edi, [ebp - 0x7e]\n\t"
        "movs dword ptr es:[edi], dword ptr [esi]\n\t"
        "movs dword ptr es:[edi], dword ptr [esi]\n\t"
        "have_args%=:\n\t"
        "fstp qword ptr [ebp - 0x76]\n\t"
        "lea eax, [ebp - 0x8e]\n\t"
        "lea ebx, [ebp - 0xa4]\n\t"
        "push ebx\n\t"
        "push eax\n\t"
        "mov ebx, dword ptr [ebp - 0x94]\n\t"
        "mov al, byte ptr [ebx + 0xe]\n\t"
        "movsx eax, al\n\t"
        "push eax\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0xc\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "fld qword ptr [ebp - 0x76]\n\t"
        "jmp restore%="
        : : "i"(&a_87except));
#else
    __asm {
        cmp dword ptr ds:[0x5024b4], 0
        jne restore
        fst qword ptr ds:[0x5d5600]
        mov al, byte ptr [ebp - 0x90]
        or al, al
        je check_status
        cmp al, 0xff
        je check_range
        cmp al, 0xfe
        je check_range
        or al, al
        je restore
        movsx eax, al
        mov dword ptr [ebp - 0x8e], eax
        jmp raise
    check_status:
        mov ax, word ptr [ebp - 0xa4]
        and ax, 0x20
        jne restore
        wait
        fnstsw ax
        and ax, 0x20
        je restore
        mov dword ptr [ebp - 0x8e], 8
        jmp raise
    restore:
        fldcw word ptr [ebp - 0xa4]
        wait
        ret
    check_range:
        mov ax, word ptr ds:[0x5d5606]
        and ax, 0x7ff0
        or ax, ax
        je underflow
        cmp ax, 0x7ff0
        je overflow
        jmp check_status
    underflow:
        mov dword ptr [ebp - 0x8e], 4
        fld qword ptr ds:[0x4e01e8]
        fxch st(1)
        fscale
        fstp st(1)
        fld st(0)
        fabs
        fcomp qword ptr ds:[0x4e01d8]
        wait
        fnstsw ax
        sahf
        jae raise
        fmul qword ptr ds:[0x4e01f8]
        jmp raise
    overflow:
        mov dword ptr [ebp - 0x8e], 3
        fld qword ptr ds:[0x4e01e0]
        fxch st(1)
        fscale
        fstp st(1)
        fld st(0)
        fabs
        fcomp qword ptr ds:[0x4e01d0]
        wait
        fnstsw ax
        sahf
        jbe raise
        fmul qword ptr ds:[0x4e01f0]
    raise:
        push esi
        push edi
        mov ebx, dword ptr [ebp - 0x94]
        inc ebx
        mov dword ptr [ebp - 0x8a], ebx
        cmp byte ptr ds:[0x5d5608], 0
        jne have_args
        cld
        lea esi, [ebp + 8]
        lea edi, [ebp - 0x86]
        movs dword ptr es:[edi], dword ptr [esi]
        movs dword ptr es:[edi], dword ptr [esi]
        cmp byte ptr [ebx + 0xc], 1
        je have_args
        lea esi, [ebp + 0x10]
        lea edi, [ebp - 0x7e]
        movs dword ptr es:[edi], dword ptr [esi]
        movs dword ptr es:[edi], dword ptr [esi]
    have_args:
        fstp qword ptr [ebp - 0x76]
        lea eax, [ebp - 0x8e]
        lea ebx, [ebp - 0xa4]
        push ebx
        push eax
        mov ebx, dword ptr [ebp - 0x94]
        mov al, byte ptr [ebx + 0xe]
        movsx eax, al
        push eax
        call dword ptr [a_87except]
        add esp, 0xc
        pop edi
        pop esi
        fld qword ptr [ebp - 0x76]
        jmp restore
    }
#endif
}
// the first entry (0x4d1fbe): the C dispatchers, which clear 0x5d5608 (the fall-through into 0x4d1fc5 is a jump here)
static __declspec(naked) void crt_cdisp_tail_fbe() {
#ifdef VP_GCC
    __asm__ volatile(
        "mov byte ptr ds:[0x5d5608], 0\n\t"
        "jmp %P0"
        : : "i"(&crt_cdisp_tail_fc5));
#else
    __asm {
        mov byte ptr ds:[0x5d5608], 0
        jmp crt_cdisp_tail_fc5
    }
#endif
}

// _cintrindisp2 (0x4d1f10): the _CI two-operand dispatcher (ST1, ST0)
static __declspec(naked) void crt_cintrindisp2() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "add esp, 0xfffffd60\n\t"
        "push ebx\n\t"
        "wait\n\t"
        "fnstcw word ptr [ebp - 0xa4]\n\t"
        "wait\n\t"
        "cmp dword ptr ds:[0x5036ec], 0\n\t"
        "je save_args%=\n\t"
        "go%=:\n\t"
        "call dword ptr [%c0]\n\t"
        "mov byte ptr ds:[0x5d5608], 1\n\t"
        "call %P1\n\t"
        "pop ebx\n\t"
        "leave\n\t"
        "ret\n\t"
        "save_args%=:\n\t"
        "fxch st(1)\n\t"
        "fst qword ptr [ebp - 0x86]\n\t"
        "fxch st(1)\n\t"
        "fst qword ptr [ebp - 0x7e]\n\t"
        "jmp go%="
        : : "i"(&a_trandisp2), "i"(&crt_cdisp_tail_fc5));
#else
    __asm {
        push ebp
        mov ebp, esp
        add esp, 0xfffffd60
        push ebx
        wait
        fnstcw word ptr [ebp - 0xa4]
        wait
        cmp dword ptr ds:[0x5036ec], 0
        je save_args
    go:
        call dword ptr [a_trandisp2]
        mov byte ptr ds:[0x5d5608], 1
        call crt_cdisp_tail_fc5
        pop ebx
        leave
        ret
    save_args:
        fxch st(1)
        fst qword ptr [ebp - 0x86]
        fxch st(1)
        fst qword ptr [ebp - 0x7e]
        jmp go
    }
#endif
}
PORT_FN(0x004d1f10, "cintrindisp2", crt_cintrindisp2, fp_asm)
CRT_SHADOW_ORIGINAL(crt_cintrindisp2)

// _cintrindisp1 (0x4d1f4e): the _CI one-operand dispatcher (ST0)
static __declspec(naked) void crt_cintrindisp1() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "add esp, 0xfffffd60\n\t"
        "push ebx\n\t"
        "wait\n\t"
        "fnstcw word ptr [ebp - 0xa4]\n\t"
        "cmp dword ptr ds:[0x5036ec], 0\n\t"
        "je save_arg%=\n\t"
        "go%=:\n\t"
        "call dword ptr [%c0]\n\t"
        "mov byte ptr ds:[0x5d5608], 1\n\t"
        "call %P1\n\t"
        "pop ebx\n\t"
        "leave\n\t"
        "ret\n\t"
        "save_arg%=:\n\t"
        "fst qword ptr [ebp - 0x86]\n\t"
        "jmp go%="
        : : "i"(&a_trandisp1), "i"(&crt_cdisp_tail_fc5));
#else
    __asm {
        push ebp
        mov ebp, esp
        add esp, 0xfffffd60
        push ebx
        wait
        fnstcw word ptr [ebp - 0xa4]
        cmp dword ptr ds:[0x5036ec], 0
        je save_arg
    go:
        call dword ptr [a_trandisp1]
        mov byte ptr ds:[0x5d5608], 1
        call crt_cdisp_tail_fc5
        pop ebx
        leave
        ret
    save_arg:
        fst qword ptr [ebp - 0x86]
        jmp go
    }
#endif
}
PORT_FN(0x004d1f4e, "cintrindisp1", crt_cintrindisp1, fp_asm)
CRT_SHADOW_ORIGINAL(crt_cintrindisp1)

// _ctrandisp2 (0x4d1f84): the C two-operand dispatcher (two doubles on the stack, loaded by _fload)
static __declspec(naked) void crt_ctrandisp2() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "add esp, 0xfffffd60\n\t"
        "push ebx\n\t"
        "push dword ptr [ebp + 0xc]\n\t"
        "push dword ptr [ebp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 8\n\t"
        "push dword ptr [ebp + 0x14]\n\t"
        "push dword ptr [ebp + 0x10]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 8\n\t"
        "wait\n\t"
        "fnstcw word ptr [ebp - 0xa4]\n\t"
        "call dword ptr [%c1]\n\t"
        "call %P2\n\t"
        "pop ebx\n\t"
        "leave\n\t"
        "ret"
        : : "i"(&a_fload), "i"(&a_trandisp2), "i"(&crt_cdisp_tail_fbe));
#else
    __asm {
        push ebp
        mov ebp, esp
        add esp, 0xfffffd60
        push ebx
        push dword ptr [ebp + 0xc]
        push dword ptr [ebp + 8]
        call dword ptr [a_fload]
        add esp, 8
        push dword ptr [ebp + 0x14]
        push dword ptr [ebp + 0x10]
        call dword ptr [a_fload]
        add esp, 8
        wait
        fnstcw word ptr [ebp - 0xa4]
        call dword ptr [a_trandisp2]
        call crt_cdisp_tail_fbe
        pop ebx
        leave
        ret
    }
#endif
}
PORT_FN(0x004d1f84, "ctrandisp2", crt_ctrandisp2, fp_asm)
CRT_SHADOW_ORIGINAL(crt_ctrandisp2)

// _ctrandisp1 (0x4d20f5): the C one-operand dispatcher
static __declspec(naked) void crt_ctrandisp1() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "add esp, 0xfffffd60\n\t"
        "push ebx\n\t"
        "push dword ptr [ebp + 0xc]\n\t"
        "push dword ptr [ebp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 8\n\t"
        "wait\n\t"
        "fnstcw word ptr [ebp - 0xa4]\n\t"
        "call dword ptr [%c1]\n\t"
        "call %P2\n\t"
        "pop ebx\n\t"
        "leave\n\t"
        "ret"
        : : "i"(&a_fload), "i"(&a_trandisp1), "i"(&crt_cdisp_tail_fbe));
#else
    __asm {
        push ebp
        mov ebp, esp
        add esp, 0xfffffd60
        push ebx
        push dword ptr [ebp + 0xc]
        push dword ptr [ebp + 8]
        call dword ptr [a_fload]
        add esp, 8
        wait
        fnstcw word ptr [ebp - 0xa4]
        call dword ptr [a_trandisp1]
        call crt_cdisp_tail_fbe
        pop ebx
        leave
        ret
    }
#endif
}
PORT_FN(0x004d20f5, "ctrandisp1", crt_ctrandisp1, fp_asm)
CRT_SHADOW_ORIGINAL(crt_ctrandisp1)

// _fload (0x4d2121): a double onto the FPU stack -- a NaN / infinity rebuilt as an extended with the low dword left
// unshifted (the original's quirk), so a signalling NaN arrives signalling
static __declspec(naked) void crt_fload() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "add esp, -0xc\n\t"
        "push ebx\n\t"
        "mov ax, word ptr [ebp + 0xe]\n\t"
        "%{load%} mov bx, ax\n\t"
        "and ax, 0x7ff0\n\t"
        "cmp ax, 0x7ff0\n\t"
        "jne plain%=\n\t"
        "or bx, 0x7fff\n\t"
        "mov word ptr [ebp - 2], bx\n\t"
        "mov eax, dword ptr [ebp + 0xc]\n\t"
        "mov ebx, dword ptr [ebp + 8]\n\t"
        "shld eax, ebx, 0xb\n\t"
        "mov dword ptr [ebp - 6], eax\n\t"
        "mov dword ptr [ebp - 0xa], ebx\n\t"
        "fld tbyte ptr [ebp - 0xa]\n\t"
        "jmp done%=\n\t"
        "plain%=:\n\t"
        "fld qword ptr [ebp + 8]\n\t"
        "done%=:\n\t"
        "pop ebx\n\t"
        "leave\n\t"
        "ret"
        : :);
#else
    __asm {
        push ebp
        mov ebp, esp
        add esp, -0xc
        push ebx
        mov ax, word ptr [ebp + 0xe]
        mov bx, ax
        and ax, 0x7ff0
        cmp ax, 0x7ff0
        jne plain
        or bx, 0x7fff
        mov word ptr [ebp - 2], bx
        mov eax, dword ptr [ebp + 0xc]
        mov ebx, dword ptr [ebp + 8]
        shld eax, ebx, 0xb
        mov dword ptr [ebp - 6], eax
        mov dword ptr [ebp - 0xa], ebx
        fld tbyte ptr [ebp - 0xa]
        jmp done
    plain:
        fld qword ptr [ebp + 8]
    done:
        pop ebx
        leave
        ret
    }
#endif
}
PORT_FN(0x004d2121, "fload", crt_fload, fp_asm)
CRT_SHADOW_ORIGINAL(crt_fload)

// =====================================================================================================================
// 87disp.obj: _trandisp1 / _trandisp2 -- classify the operand(s) with fxam (through the 0x50286d class table), set the
// precision control (64 bits, or the caller's for the table's kind 5), and jump through the operation's table to the
// kernel or the special-case routine. They run in the dispatcher's frame (ebp) with the table in edx.
// =====================================================================================================================
static __declspec(naked) void crt_trandisp1() {
#ifdef VP_GCC
    __asm__ volatile(
        "cmp byte ptr [edx + 0xe], 5\n\t"
        "jne full%=\n\t"
        "mov bx, word ptr [ebp - 0xa4]\n\t"
        "or bh, 2\n\t"
        "and bh, 0xfe\n\t"
        "mov bl, 0x3f\n\t"
        "jmp set_cw%=\n\t"
        "full%=:\n\t"
        "mov bx, 0x133f\n\t"
        "set_cw%=:\n\t"
        "mov word ptr [ebp - 0xa2], bx\n\t"
        "fldcw word ptr [ebp - 0xa2]\n\t"
        "mov ebx, 0x50286d\n\t"
        "fxam\n\t"
        "mov dword ptr [ebp - 0x94], edx\n\t"
        "wait\n\t"
        "fnstsw word ptr [ebp - 0xa0]\n\t"
        "mov byte ptr [ebp - 0x90], 0\n\t"
        "wait\n\t"
        "mov cl, byte ptr [ebp - 0x9f]\n\t"
        "shl cl, 1\n\t"
        "sar cl, 1\n\t"
        "rol cl, 1\n\t"
        "%{load%} mov al, cl\n\t"
        "and al, 0xf\n\t"
        "xlatb\n\t"
        "movsx eax, al\n\t"
        "and ecx, 0x404\n\t"
        "%{load%} mov ebx, edx\n\t"
        "%{load%} add ebx, eax\n\t"
        "add ebx, 0x10\n\t"
        "jmp dword ptr [ebx]"
        : :);
#else
    __asm {
        cmp byte ptr [edx + 0xe], 5
        jne full
        mov bx, word ptr [ebp - 0xa4]
        or bh, 2
        and bh, 0xfe
        mov bl, 0x3f
        jmp set_cw
    full:
        mov bx, 0x133f
    set_cw:
        mov word ptr [ebp - 0xa2], bx
        fldcw word ptr [ebp - 0xa2]
        mov ebx, 0x50286d
        fxam
        mov dword ptr [ebp - 0x94], edx
        wait
        fnstsw word ptr [ebp - 0xa0]
        mov byte ptr [ebp - 0x90], 0
        wait
        mov cl, byte ptr [ebp - 0x9f]
        shl cl, 1
        sar cl, 1
        rol cl, 1
        mov al, cl
        and al, 0xf
        xlatb
        movsx eax, al
        and ecx, 0x404
        mov ebx, edx
        add ebx, eax
        add ebx, 0x10
        jmp dword ptr [ebx]
    }
#endif
}
PORT_FN(0x004d2da0, "trandisp1", crt_trandisp1, fp_asm)
CRT_SHADOW_ORIGINAL(crt_trandisp1)

static __declspec(naked) void crt_trandisp2() {
#ifdef VP_GCC
    __asm__ volatile(
        "cmp byte ptr [edx + 0xe], 5\n\t"
        "jne full%=\n\t"
        "mov bx, word ptr [ebp - 0xa4]\n\t"
        "or bh, 2\n\t"
        "and bh, 0xfe\n\t"
        "mov bl, 0x3f\n\t"
        "jmp set_cw%=\n\t"
        "full%=:\n\t"
        "mov bx, 0x133f\n\t"
        "set_cw%=:\n\t"
        "mov word ptr [ebp - 0xa2], bx\n\t"
        "fldcw word ptr [ebp - 0xa2]\n\t"
        "mov ebx, 0x50286d\n\t"
        "fxam\n\t"
        "mov dword ptr [ebp - 0x94], edx\n\t"
        "wait\n\t"
        "fnstsw word ptr [ebp - 0xa0]\n\t"
        "mov byte ptr [ebp - 0x90], 0\n\t"
        "fxch st(1)\n\t"
        "mov cl, byte ptr [ebp - 0x9f]\n\t"
        "fxam\n\t"
        "wait\n\t"
        "fnstsw word ptr [ebp - 0xa0]\n\t"
        "fxch st(1)\n\t"
        "mov ch, byte ptr [ebp - 0x9f]\n\t"
        "shl ch, 1\n\t"
        "sar ch, 1\n\t"
        "rol ch, 1\n\t"
        "%{load%} mov al, ch\n\t"
        "and al, 0xf\n\t"
        "xlatb\n\t"
        "%{load%} mov ah, al\n\t"
        "shl cl, 1\n\t"
        "sar cl, 1\n\t"
        "rol cl, 1\n\t"
        "%{load%} mov al, cl\n\t"
        "and al, 0xf\n\t"
        "xlatb\n\t"
        "shl ah, 1\n\t"
        "shl ah, 1\n\t"
        "%{load%} or al, ah\n\t"
        "movsx eax, al\n\t"
        "and ecx, 0x404\n\t"
        "%{load%} mov ebx, edx\n\t"
        "%{load%} add ebx, eax\n\t"
        "add ebx, 0x10\n\t"
        "jmp dword ptr [ebx]"
        : :);
#else
    __asm {
        cmp byte ptr [edx + 0xe], 5
        jne full
        mov bx, word ptr [ebp - 0xa4]
        or bh, 2
        and bh, 0xfe
        mov bl, 0x3f
        jmp set_cw
    full:
        mov bx, 0x133f
    set_cw:
        mov word ptr [ebp - 0xa2], bx
        fldcw word ptr [ebp - 0xa2]
        mov ebx, 0x50286d
        fxam
        mov dword ptr [ebp - 0x94], edx
        wait
        fnstsw word ptr [ebp - 0xa0]
        mov byte ptr [ebp - 0x90], 0
        fxch st(1)
        mov cl, byte ptr [ebp - 0x9f]
        fxam
        wait
        fnstsw word ptr [ebp - 0xa0]
        fxch st(1)
        mov ch, byte ptr [ebp - 0x9f]
        shl ch, 1
        sar ch, 1
        rol ch, 1
        mov al, ch
        and al, 0xf
        xlatb
        mov ah, al
        shl cl, 1
        sar cl, 1
        rol cl, 1
        mov al, cl
        and al, 0xf
        xlatb
        shl ah, 1
        shl ah, 1
        or al, ah
        movsx eax, al
        and ecx, 0x404
        mov ebx, edx
        add ebx, eax
        add ebx, 0x10
        jmp dword ptr [ebx]
    }
#endif
}
PORT_FN(0x004d2e07, "trandisp2", crt_trandisp2, fp_asm)
CRT_SHADOW_ORIGINAL(crt_trandisp2)

// =====================================================================================================================
// 87triga.obj: the asin / acos / atan / atan2 kernels and their special-case routines (0x4d1e60..0x4d1f01). They have
// no inventory entry (the map names only __fFATN2 and __rtpiby2; the inventory folds the bytes into heap_alloc), and
// are only ever jumped to through the operations' tables (_OP_ASINjmptab ... at 0x502780..0x5027ff, which stay), in
// the dispatcher's frame with the operands on the FPU stack: each entry is hooked at its own address. The originals
// fall into one another; here each carries its own copy of what it falls into (the fpatan tail, the asin / acos
// reduction 0x4d1e9c), and jumps to 87disp's result routines (0x4d2ea6, 0x4d2f46, 0x4d2f48, 0x4d2f63) by address.
// =====================================================================================================================
namespace {
const U32 a_rtindfpop = 0x004d2f46, a_rtindfnpop = 0x004d2f48, a_rtzeronpop = 0x004d2ea6,
          a_rtchsifneg = 0x004d2f63, a_rtpiby2 = 0x004d1ecc, a_triga_ef8 = 0x004d1ef8;
}
// 0x4d1e9c (called by the asin / acos kernels): |x|, then sqrt((1 - x)(1 + x)) -- or, when that is negative (|x| > 1),
// drop the return address and leave through _rtindfpop (the indefinite result)
static __declspec(naked) void crt_triga_sqrt1mx2() {
#ifdef VP_GCC
    __asm__ volatile(
        "fabs\n\t"
        "fld st(0)\n\t"
        "fld st(0)\n\t"
        "fld1\n\t"
        "fsubrp st(1), st\n\t"
        "fxch st(1)\n\t"
        "fld1\n\t"
        "faddp st(1), st\n\t"
        "fmulp st(1), st\n\t"
        "ftst\n\t"
        "wait\n\t"
        "fnstsw word ptr [ebp - 0xa0]\n\t"
        "wait\n\t"
        "test byte ptr [ebp - 0x9f], 1\n\t"
        "jne negative%=\n\t"
        "%{load%} xor ch, ch\n\t"
        "fsqrt\n\t"
        "ret\n\t"
        "negative%=:\n\t"
        "pop eax\n\t"
        "jmp dword ptr [%c0]"
        : : "i"(&a_rtindfpop));
#else
    __asm {
        fabs
        fld st(0)
        fld st(0)
        fld1
        fsubrp st(1), st
        fxch st(1)
        fld1
        faddp st(1), st
        fmulp st(1), st
        ftst
        wait
        fnstsw word ptr [ebp - 0xa0]
        wait
        test byte ptr [ebp - 0x9f], 1
        jne negative
        xor ch, ch
        fsqrt
        ret
    negative:
        pop eax
        jmp dword ptr [a_rtindfpop]
    }
#endif
}
// the shared tail (0x4d1e8b): fpatan, then pi - result when cl, the sign when ch
#define CRT_TRIGA_FPATAN_TAIL                                                                                      \
    __asm { fpatan }                                                                                               \
    __asm { or cl, cl }                                                                                            \
    __asm { je no_pi }                                                                                             \
    __asm { fldpi }                                                                                                \
    __asm { fsubrp st(1), st }                                                                                     \
    __asm { no_pi: }                                                                                               \
    __asm { or ch, ch }                                                                                            \
    __asm { je no_chs }                                                                                            \
    __asm { fchs }                                                                                                 \
    __asm { no_chs: }                                                                                              \
    __asm { ret }
#ifdef VP_GCC
#define CRT_TRIGA_FPATAN_TAIL_GNU                                                                                  \
    "fpatan\n\t"                                                                                                   \
    "%{load%} or cl, cl\n\t"                                                                                       \
    "je no_pi%=\n\t"                                                                                               \
    "fldpi\n\t"                                                                                                    \
    "fsubrp st(1), st\n\t"                                                                                         \
    "no_pi%=:\n\t"                                                                                                 \
    "%{load%} or ch, ch\n\t"                                                                                       \
    "je no_chs%=\n\t"                                                                                              \
    "fchs\n\t"                                                                                                     \
    "no_chs%=:\n\t"                                                                                                \
    "ret"
#endif

// asin (0x4d1e60): atan2(x, sqrt(1 - x^2)), the signs swapped into ch
static __declspec(naked) void crt_triga_asin() {
#ifdef VP_GCC
    __asm__ volatile(
        "call %P0\n\t"
        "xchg cl, ch\n\t"
        CRT_TRIGA_FPATAN_TAIL_GNU
        : : "i"(&crt_triga_sqrt1mx2));
#else
    __asm {
        call crt_triga_sqrt1mx2
        xchg cl, ch
    }
    CRT_TRIGA_FPATAN_TAIL
#endif
}
PORT_FN(0x004d1e60, "87triga:asin", crt_triga_asin, fp_asm)
CRT_SHADOW_ORIGINAL(crt_triga_asin)

// acos (0x4d1e69): atan2(sqrt(1 - x^2), x)
static __declspec(naked) void crt_triga_acos() {
#ifdef VP_GCC
    __asm__ volatile(
        "call %P0\n\t"
        "fxch st(1)\n\t"
        CRT_TRIGA_FPATAN_TAIL_GNU
        : : "i"(&crt_triga_sqrt1mx2));
#else
    __asm {
        call crt_triga_sqrt1mx2
        fxch st(1)
    }
    CRT_TRIGA_FPATAN_TAIL
#endif
}
PORT_FN(0x004d1e69, "87triga:acos", crt_triga_acos, fp_asm)
CRT_SHADOW_ORIGINAL(crt_triga_acos)

// atan (0x4d1e72): atan2(|x|, 1), the sign from cl
static __declspec(naked) void crt_triga_atan() {
#ifdef VP_GCC
    __asm__ volatile(
        "fabs\n\t"
        "fld1\n\t"
        "%{load%} mov ch, cl\n\t"
        "%{load%} xor cl, cl\n\t"
        CRT_TRIGA_FPATAN_TAIL_GNU
        : :);
#else
    __asm {
        fabs
        fld1
        mov ch, cl
        xor cl, cl
    }
    CRT_TRIGA_FPATAN_TAIL
#endif
}
PORT_FN(0x004d1e72, "87triga:atan", crt_triga_atan, fp_asm)
CRT_SHADOW_ORIGINAL(crt_triga_atan)

// __fFATN2 (0x4d1e7c): atan2 on the magnitudes (error class 0xfe: the result checked for underflow), signs after
static __declspec(naked) void crt_fFATN2() {
#ifdef VP_GCC
    __asm__ volatile(
        "mov byte ptr [ebp - 0x90], 0xfe\n\t"
        "fabs\n\t"
        "fxch st(1)\n\t"
        "fabs\n\t"
        "fxch st(1)\n\t"
        CRT_TRIGA_FPATAN_TAIL_GNU
        : :);
#else
    __asm {
        mov byte ptr [ebp - 0x90], 0xfe
        fabs
        fxch st(1)
        fabs
        fxch st(1)
    }
    CRT_TRIGA_FPATAN_TAIL
#endif
}
PORT_FN(0x004d1e7c, "fFATN2", crt_fFATN2, fp_asm)
CRT_SHADOW_ORIGINAL(crt_fFATN2)

// __rtpiby2 (0x4d1ecc): the operand replaced by pi/2 (87disp's 80-bit constant)
static __declspec(naked) void crt_rtpiby2() {
#ifdef VP_GCC
    __asm__ volatile(
        "fstp st(0)\n\t"
        "fld tbyte ptr ds:[0x50285a]\n\t"
        "ret"
        : :);
#else
    __asm {
        fstp st(0)
        fld tbyte ptr ds:[0x50285a]
        ret
    }
#endif
}
PORT_FN(0x004d1ecc, "rtpiby2", crt_rtpiby2, fp_asm)
CRT_SHADOW_ORIGINAL(crt_rtpiby2)

// 0x4d1ed5 (atan2's table): drop one operand; with cl, the other too and +-pi (ch); else _rtzeronpop
static __declspec(naked) void crt_triga_atan2_pi() {
#ifdef VP_GCC
    __asm__ volatile(
        "fstp st(0)\n\t"
        "%{load%} or cl, cl\n\t"
        "je zero%=\n\t"
        "fstp st(0)\n\t"
        "fldpi\n\t"
        "%{load%} or ch, ch\n\t"
        "je done%=\n\t"
        "fchs\n\t"
        "done%=:\n\t"
        "ret\n\t"
        "zero%=:\n\t"
        "jmp dword ptr [%c0]"
        : : "i"(&a_rtzeronpop));
#else
    __asm {
        fstp st(0)
        or cl, cl
        je zero
        fstp st(0)
        fldpi
        or ch, ch
        je done
        fchs
    done:
        ret
    zero:
        jmp dword ptr [a_rtzeronpop]
    }
#endif
}
PORT_FN(0x004d1ed5, "87triga:atan2 pi", crt_triga_atan2_pi, fp_asm)
CRT_SHADOW_ORIGINAL(crt_triga_atan2_pi)

// 0x4d1eeb (atan2's table): drop one operand, the indefinite
static __declspec(naked) void crt_triga_atan2_ind() {
#ifdef VP_GCC
    __asm__ volatile(
        "fstp st(0)\n\t"
        "jmp dword ptr [%c0]"
        : : "i"(&a_rtindfnpop));
#else
    __asm {
        fstp st(0)
        jmp dword ptr [a_rtindfnpop]
    }
#endif
}
PORT_FN(0x004d1eeb, "87triga:atan2 indefinite", crt_triga_atan2_ind, fp_asm)
CRT_SHADOW_ORIGINAL(crt_triga_atan2_ind)

// 0x4d1ef2 (atan / atan2's tables): drop one operand, the sign from ch, into 0x4d1ef8
static __declspec(naked) void crt_triga_atan2_piby2() {
#ifdef VP_GCC
    __asm__ volatile(
        "fstp st(0)\n\t"
        "%{load%} mov cl, ch\n\t"
        "jmp dword ptr [%c0]"
        : : "i"(&a_triga_ef8));
#else
    __asm {
        fstp st(0)
        mov cl, ch
        jmp dword ptr [a_triga_ef8]
    }
#endif
}
PORT_FN(0x004d1ef2, "87triga:atan2 pi/2", crt_triga_atan2_piby2, fp_asm)
CRT_SHADOW_ORIGINAL(crt_triga_atan2_piby2)

// 0x4d1ef8 (atan's table): +-pi/2 -- _rtpiby2, then _rtchsifneg
static __declspec(naked) void crt_triga_piby2_signed() {
#ifdef VP_GCC
    __asm__ volatile(
        "call dword ptr [%c0]\n\t"
        "jmp dword ptr [%c1]"
        : : "i"(&a_rtpiby2), "i"(&a_rtchsifneg));
#else
    __asm {
        call dword ptr [a_rtpiby2]
        jmp dword ptr [a_rtchsifneg]
    }
#endif
}
PORT_FN(0x004d1ef8, "87triga:pi/2 signed", crt_triga_piby2_signed, fp_asm)
CRT_SHADOW_ORIGINAL(crt_triga_piby2_signed)
