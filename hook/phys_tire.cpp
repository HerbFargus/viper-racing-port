// phys_tire.cpp -- M3 3.3 group T: the tyre (physics:tire.obj) and the aerodynamics (physics:aero.obj),
// rewritten.
//
// A Tire is Pacejka's "magic formula" on a normalised combined slip, with load-dependent cornering
// stiffness (a quadratic through two .tir points) and friction (a line through two), a camber grip factor
// and longitudinal factors of the lateral ones. An Aero is lift and drag coefficients as quadratics in the
// angle of attack, fitted through three .aer points (GetQuadratic) and clamped to the angle range.
//
// Written from the v1.0 disassembly: the same sums in the same grouping, register values as double, stored
// values as float, every call made in the original's order with the same arguments (float arguments the
// original pushes with integer moves are passed as their bits). fpatan / fsin / fcos aren't rounded by the
// precision control, so each is written in a small asm helper together with the operation that consumes
// its full 64-bit result, as in the original (a store to a double in between would round twice).
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// ---- the layouts (proposed for phys_types.h) ----------------------------------------------------------------
// Tire (64 bytes, MemAlloc(0x40) in TireCreate): every field is written by one setter (setup_tire's calls)
struct Tire {
    float cornering_quad;              // +0x00 cornering stiffness (N/rad) = quad Fz^2 + lin Fz  (SetCorneringStiffness)
    float cornering_lin;               // +0x04
    float friction_slope;              // +0x08 lateral mu = slope Fz + base                    (SetLateralFriction)
    float friction_base;               // +0x0c
    float camber_stiffness_factor;     // +0x10 x cornering stiffness                           (SetCamberStiffnessFactor)
    float long_stiffness_factor;       // +0x14 x cornering stiffness                           (SetLongStiffnessFactor)
    float long_friction_factor;        // +0x18 x lateral mu                                    (SetLongFrictionFactor)
    float pacejka_b, pacejka_c, pacejka_d, pacejka_e;   // +0x1c..+0x28                         (SetMagicNumbers)
    float load_factor;                 // +0x2c car tyre width / .tir width; SetMagicNumbers sets 1 (SetLoadFactor)
    float mass;                        // +0x30 kg                                              (SetInertia)
    float inertia;                     // +0x34 kg m^2                                          (SetInertia)
    float camber_peak;                 // +0x38 sin(peak camber); SetMagicNumbers sets 0        (SetCamberGripPeak)
    float camber_peak_gain;            // +0x3c                                                 (SetCamberGripPeak)
};
static_assert(offsetof(Tire, pacejka_b) == 0x1c && offsetof(Tire, camber_peak) == 0x38 && sizeof(Tire) == 64, "Tire");

// TireDesc: the 'TIRE' resource (.tir), 0x1e dwords (TireCreate copies it with rep movsd, ecx=0x1e)
struct TireDesc {
    uint8_t _00[0x20];
    float ref_width_mm;                // +0x20 TireCreate: load factor = width / this; then overwritten by width
    float aspect_pct;                  // +0x24 overwritten by TireCreate's 2nd argument
    float diameter_in;                 // +0x28 overwritten by TireCreate's 3rd argument
    float mass_lb;                     // +0x2c x 0.4545 -> Tire.mass
    float inertia_lbft2;               // +0x30 x 0.04228 -> Tire.inertia
    float pacejka_b, pacejka_c, pacejka_d, pacejka_e;   // +0x34..+0x40
    float friction_load1_lb, friction_mu1, friction_load2_lb, friction_mu2;           // +0x44..+0x50 (lb x 4.45 = N)
    float cornering_load1_lb, cornering_stiffness1, cornering_load2_lb, cornering_stiffness2;   // +0x54..+0x60
    float camber_stiffness_factor;     // +0x64
    float long_stiffness_factor;       // +0x68
    float long_friction_factor;        // +0x6c
    float camber_peak_deg;             // +0x70
    float camber_peak_gain;            // +0x74
};
static_assert(offsetof(TireDesc, pacejka_b) == 0x34 && offsetof(TireDesc, camber_peak_gain) == 0x74 &&
              sizeof(TireDesc) == 0x78, "TireDesc");

// Aero (36 bytes: AeroGetAero(i) = 0x521d50 + 36 i, the aero.obj statics)
struct Aero {
    uint32_t _00;                      // +0x00 copied from the description (bits); not read by these functions
    float angle_min;                   // +0x04 rad: GetLift / GetDrag clamp the angle to [min, max]
    float angle_max;                   // +0x08 rad
    float lift_a, lift_b, lift_c;      // +0x0c lift = (a t^2 + b t + c) v^2 at angle t
    float drag_a, drag_b, drag_c;      // +0x18 drag, the same form
};
static_assert(offsetof(Aero, lift_a) == 0xc && offsetof(Aero, drag_c) == 0x20 && sizeof(Aero) == 36, "Aero");

// AeroDescription: what SetParams reads (+0x00..+0x2f; the rest, if any, unknown)
struct AeroDescription {
    uint32_t _00;                      // +0x00 -> Aero._00
    float angle_min_deg, angle_max_deg;   // +0x04, +0x08
    float angle_deg[3];                // +0x0c the three fitting points' angles
    float lift[3];                     // +0x18 lift coefficients there (x 0.0022528125)
    float drag[3];                     // +0x24 drag coefficients there (x 0.0022528125)
};
static_assert(offsetof(AeroDescription, drag) == 0x24 && sizeof(AeroDescription) == 48, "AeroDescription");

// the unused edx of a __thiscall received as __fastcall (an int: test/fuzz.h can't size a void*)
typedef int Edx;

static __forceinline uint32_t bits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline float from_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// the constants, from their bits (all dword: float constants)
#define FCONST(NAME, HEX) static const union { uint32_t u; float f; } NAME = {HEX}
FCONST(k_one, 0x3f800000);             // 0x4dc03c
FCONST(k_0_9, 0x3f666666);             // 0x4dc044
FCONST(k_two_pi, 0x40c90fdb);          // 0x4dc050
FCONST(k_eps, 0x34000000);             // 0x4dc054 FLT_EPSILON
FCONST(k_half, 0x3f000000);            // 0x4dc058
FCONST(k_neg_half, 0xbf000000);        // 0x4dc05c
FCONST(k_two_thirds, 0x3f2aaaab);      // 0x4dc060
FCONST(k_lbft2_to_kgm2, 0x3d2d2da6);   // 0x4dc064 0.04227986
FCONST(k_lb_to_kg, 0x3ee8b439);        // 0x4dc068 0.4545
FCONST(k_lb_to_n, 0x408e6666);         // 0x4dc06c 4.45
FCONST(k_stiff_scale, 0x437fbf43);     // 0x4dc070 255.74712
FCONST(k_deg_tire, 0x3c8efa35);        // 0x4dc074 pi/180
FCONST(k_deg_aero, 0x3c8efa35);        // 0x4dc024 pi/180
FCONST(k_aero_scale, 0x3b13a3ec);      // 0x4dc028 0.0022528125
FCONST(k_min_speed, 0x3dcccccd);       // 0x4dc030 0.1
#undef FCONST

// ---- the x87 instructions the precision control doesn't round ---------------------------------------------
// atan(y) (fld1; fpatan) times m -- the multiply rounds the full result
static __declspec(noinline) double x87_atan_mul(double y, double m) {
    double r;
#ifdef VP_GCC
    __asm__ volatile("fld %[y]\n\t"
                     "fld1\n\t"
                     "fpatan\n\t"
                     "fmul %[m]\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [y] "m"(y), [m] "m"(m)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld y
            fld1
            fpatan
            fmul m
            fstp r }
#endif
    return r;
}
// sin(atan(y) * c) * d
static __declspec(noinline) double x87_atan_mul_sin_mul(double y, double c, double d) {
    double r;
#ifdef VP_GCC
    __asm__ volatile("fld %[y]\n\t"
                     "fld1\n\t"
                     "fpatan\n\t"
                     "fmul %[c]\n\t"
                     "fsin\n\t"
                     "fmul %[d]\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [y] "m"(y), [c] "m"(c), [d] "m"(d)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld y
            fld1
            fpatan
            fmul c
            fsin
            fmul d
            fstp r }
#endif
    return r;
}
// cos(x) * m: x87_cos_mul, in x87.h
// sin(x) stored straight to a float (fsin; fstp dword)
static __declspec(noinline) float x87_sin_float(double x) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("fld %[x]\n\t"
                     "fsin\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [x] "m"(x)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld x
            fsin
            fstp r }
#endif
    return r;
}
// fld dword; fchs; fstp dword -- a float negated through the FPU (a signalling NaN comes out quiet)
static __declspec(noinline) float x87_chs_float(float x) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("fld %[x]\n\t"
                     "fchs\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [x] "m"(x)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld x
            fchs
            fstp r }
#endif
    return r;
}

// ---- the game's functions these call, by address ----------------------------------------------------------
// the Tire members GetForce and setup_tire call (non-virtual: by address, so their rewrites run when hooked)
typedef double(__fastcall* TireGet1_t)(const Tire*, void*, uint32_t);
typedef double(__fastcall* TireGetCamberGrip_t)(const Tire*, void*, uint32_t, uint32_t, uint32_t);
typedef double(__fastcall* TireGetResultant_t)(const Tire*, void*, uint32_t);
static const TireGet1_t Tire_GetCorneringStiffness_p = (TireGet1_t)0x0043bba0;
static const TireGet1_t Tire_GetLongStiffness_p = (TireGet1_t)0x0043bbc0;
static const TireGet1_t Tire_GetCamberStiffness_p = (TireGet1_t)0x0043bbe0;
static const TireGetCamberGrip_t Tire_GetCamberGripFactor_p = (TireGetCamberGrip_t)0x0043bc00;
static const TireGet1_t Tire_GetLateralFriction_p = (TireGet1_t)0x0043bc80;
static const TireGet1_t Tire_GetLongFriction_p = (TireGet1_t)0x0043bc90;
static const TireGetResultant_t Tire_GetResultant_p = (TireGetResultant_t)0x0043bcb0;

typedef void(__fastcall* SetMagicNumbers_t)(Tire*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void(__fastcall* SetBits1_t)(Tire*, void*, uint32_t);
typedef void(__fastcall* SetInertia_t)(Tire*, void*, float, float);
typedef void(__fastcall* SetLateralFriction_t)(Tire*, void*, float, uint32_t, float, uint32_t);
typedef void(__fastcall* SetCorneringStiffness_t)(Tire*, void*, float, float, float, float);
typedef void(__fastcall* SetCamberGripPeak_t)(Tire*, void*, float, uint32_t);
static const SetMagicNumbers_t Tire_SetMagicNumbers_p = (SetMagicNumbers_t)0x0043ba40;
static const SetLateralFriction_t Tire_SetLateralFriction_p = (SetLateralFriction_t)0x0043ba70;
static const SetCorneringStiffness_t Tire_SetCorneringStiffness_p = (SetCorneringStiffness_t)0x0043bab0;
static const SetBits1_t Tire_SetLoadFactor_p = (SetBits1_t)0x0043bb20;
static const SetInertia_t Tire_SetInertia_p = (SetInertia_t)0x0043bb30;
static const SetBits1_t Tire_SetCamberStiffnessFactor_p = (SetBits1_t)0x0043bb50;
static const SetCamberGripPeak_t Tire_SetCamberGripPeak_p = (SetCamberGripPeak_t)0x0043bb60;
static const SetBits1_t Tire_SetLongStiffnessFactor_p = (SetBits1_t)0x0043bb80;
static const SetBits1_t Tire_SetLongFrictionFactor_p = (SetBits1_t)0x0043bb90;

typedef TireDesc*(__cdecl* TireDescGet_t)(const char*);
typedef void(__cdecl* TireDescForget_t)(TireDesc*);
typedef void(__cdecl* SetupTire_t)(Tire*, const TireDesc*, uint32_t);
static const TireDescGet_t TireDescGet_p = (TireDescGet_t)0x0043c020;
static const TireDescForget_t TireDescForget_p = (TireDescForget_t)0x0043c070;
static const SetupTire_t setup_tire_p = (SetupTire_t)0x0043c120;

typedef void*(__cdecl* ResourceGet_t)(const char*, uint32_t, uint32_t*, int*, uint8_t*, uint8_t*);
typedef uint8_t(__cdecl* ResourceForget_t)(void*);
typedef void(__cdecl* LogPanic_t)(const char*, ...);
typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* OperatorDelete_t)(void*);
static const ResourceGet_t ResourceGet = (ResourceGet_t)0x00419fa0;
static const ResourceForget_t ResourceForget = (ResourceForget_t)0x0041a450;
static const LogPanic_t LogPanic = (LogPanic_t)0x004112b0;
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
static const OperatorDelete_t operator_delete = (OperatorDelete_t)0x00414390;
static const char* const k_tire_not_found = (const char*)0x004ed878;   // the panic's format (the name)

typedef void(__cdecl* MatrixSetRows_t)(M3*, const P3*, const P3*, const P3*);
typedef void(__cdecl* MatrixInverse_t)(M3*, const M3*);
typedef void(__cdecl* MatrixMulPoint_t)(P3*, const P3*, const M3*);
typedef void(__cdecl* GetQuadratic_t)(float*, float*, float*, float*, float*);
typedef double(__fastcall* AeroGet_t)(const Aero*, void*, uint32_t, uint32_t);
static const MatrixSetRows_t MatrixSetRows = (MatrixSetRows_t)0x0043b3d0;
static const MatrixInverse_t MatrixInverse = (MatrixInverse_t)0x0043b420;
static const MatrixMulPoint_t MatrixMulPoint = (MatrixMulPoint_t)0x00429420;       // o = p x M
static const GetQuadratic_t GetQuadratic_p = (GetQuadratic_t)0x0043b2c0;
static const AeroGet_t Aero_GetLift_p = (AeroGet_t)0x0043b780;
static const AeroGet_t Aero_GetDrag_p = (AeroGet_t)0x0043b7d0;

static void fp_pure(Footprint& f) { f.pure = true; }
#define FP_TIRE { f.add(self, sizeof(Tire), "tire"); f.pure = true; }

// ======== tire.obj ===========================================================================================

// Tire::SetMagicNumbers (0x43ba40): B C D E by integer moves; load factor 1; camber peak and gain 0
static void __fastcall Tire_SetMagicNumbers(Tire* self, Edx, float b, float c, float d, float e) {
    memcpy(&self->pacejka_b, &b, 4);
    memcpy(&self->pacejka_c, &c, 4);
    memcpy(&self->pacejka_d, &d, 4);
    memcpy(&self->pacejka_e, &e, 4);
    uint32_t one = 0x3f800000, zero = 0;
    memcpy(&self->load_factor, &one, 4);
    memcpy(&self->camber_peak, &zero, 4);
    memcpy(&self->camber_peak_gain, &zero, 4);
}
static void fp_set_magic(Footprint& f, Tire* self, Edx, float, float, float, float) FP_TIRE
PORT_FN(0x0043ba40, "Tire::SetMagicNumbers", Tire_SetMagicNumbers, fp_set_magic)

// Tire::SetLateralFriction (0x43ba70): the line through (load1, mu1), (load2, mu2); the slope stays in a
// register for the intercept
static void __fastcall Tire_SetLateralFriction(Tire* self, Edx, float load1, float mu1, float load2, float mu2) {
    double k = (D(mu2) - mu1) / (D(load2) - load1);
    self->friction_slope = (float)k;
    self->friction_base = (float)(D(mu1) - k * load1);
}
static void fp_set_lat_friction(Footprint& f, Tire* self, Edx, float, float, float, float) FP_TIRE
PORT_FN(0x0043ba70, "Tire::SetLateralFriction", Tire_SetLateralFriction, fp_set_lat_friction)

// Tire::SetCorneringStiffness (0x43bab0): quad Fz^2 + lin Fz through (load1, cs1), (load2, cs2):
// quad = (cs2/load2 - cs1/load1) / (load2 - load1), lin = cs1/load1 - quad load1 (all in registers)
static void __fastcall Tire_SetCorneringStiffness(Tire* self, Edx, float load1, float cs1, float load2, float cs2) {
    double r1 = D(cs1) / load1;
    double r2 = D(cs2) / load2;
    double q = (r2 - r1) / (D(load2) - load1);
    self->cornering_quad = (float)q;
    self->cornering_lin = (float)(r1 - q * load1);
}
static void fp_set_cornering(Footprint& f, Tire* self, Edx, float, float, float, float) FP_TIRE
PORT_FN(0x0043bab0, "Tire::SetCorneringStiffness", Tire_SetCorneringStiffness, fp_set_cornering)

// Tire::SetLoadFactor (0x43bb20)
static void __fastcall Tire_SetLoadFactor(Tire* self, Edx, float v) { memcpy(&self->load_factor, &v, 4); }
static void fp_set_load_factor(Footprint& f, Tire* self, Edx, float) FP_TIRE
PORT_FN(0x0043bb20, "Tire::SetLoadFactor", Tire_SetLoadFactor, fp_set_load_factor)

// Tire::SetInertia (0x43bb30): mass, inertia
static void __fastcall Tire_SetInertia(Tire* self, Edx, float mass, float inertia) {
    memcpy(&self->mass, &mass, 4);
    memcpy(&self->inertia, &inertia, 4);
}
static void fp_set_inertia(Footprint& f, Tire* self, Edx, float, float) FP_TIRE
PORT_FN(0x0043bb30, "Tire::SetInertia", Tire_SetInertia, fp_set_inertia)

// Tire::SetCamberStiffnessFactor (0x43bb50)
static void __fastcall Tire_SetCamberStiffnessFactor(Tire* self, Edx, float v) {
    memcpy(&self->camber_stiffness_factor, &v, 4);
}
static void fp_set_camber_stiff(Footprint& f, Tire* self, Edx, float) FP_TIRE
PORT_FN(0x0043bb50, "Tire::SetCamberStiffnessFactor", Tire_SetCamberStiffnessFactor, fp_set_camber_stiff)

// Tire::SetCamberGripPeak (0x43bb60): sin(peak angle), gain
static void __fastcall Tire_SetCamberGripPeak(Tire* self, Edx, float peak, float gain) {
    memcpy(&self->camber_peak, &peak, 4);
    memcpy(&self->camber_peak_gain, &gain, 4);
}
static void fp_set_camber_peak(Footprint& f, Tire* self, Edx, float, float) FP_TIRE
PORT_FN(0x0043bb60, "Tire::SetCamberGripPeak", Tire_SetCamberGripPeak, fp_set_camber_peak)

// Tire::SetLongStiffnessFactor (0x43bb80)
static void __fastcall Tire_SetLongStiffnessFactor(Tire* self, Edx, float v) {
    memcpy(&self->long_stiffness_factor, &v, 4);
}
static void fp_set_long_stiff(Footprint& f, Tire* self, Edx, float) FP_TIRE
PORT_FN(0x0043bb80, "Tire::SetLongStiffnessFactor", Tire_SetLongStiffnessFactor, fp_set_long_stiff)

// Tire::SetLongFrictionFactor (0x43bb90)
static void __fastcall Tire_SetLongFrictionFactor(Tire* self, Edx, float v) {
    memcpy(&self->long_friction_factor, &v, 4);
}
static void fp_set_long_friction(Footprint& f, Tire* self, Edx, float) FP_TIRE
PORT_FN(0x0043bb90, "Tire::SetLongFrictionFactor", Tire_SetLongFrictionFactor, fp_set_long_friction)

// Tire::GetCorneringStiffness (0x43bba0): (Fz Fz) quad + lin Fz, returned unrounded in ST0
static double __fastcall Tire_GetCorneringStiffness(const Tire* self, Edx, float load) {
    return (D(load) * load) * self->cornering_quad + D(self->cornering_lin) * load;
}
static void fp_get_cornering(Footprint& f, const Tire*, Edx, float) { f.pure = true; }
PORT_FN(0x0043bba0, "Tire::GetCorneringStiffness", Tire_GetCorneringStiffness, fp_get_cornering)

// Tire::GetLongStiffness (0x43bbc0): GetCorneringStiffness x long_stiffness_factor
static double __fastcall Tire_GetLongStiffness(const Tire* self, Edx, float load) {
    return Tire_GetCorneringStiffness_p(self, 0, bits(load)) * self->long_stiffness_factor;
}
static void fp_get_long_stiff(Footprint& f, const Tire*, Edx, float) { f.pure = true; }
PORT_FN(0x0043bbc0, "Tire::GetLongStiffness", Tire_GetLongStiffness, fp_get_long_stiff)

// Tire::GetCamberStiffness (0x43bbe0): GetCorneringStiffness x camber_stiffness_factor
static double __fastcall Tire_GetCamberStiffness(const Tire* self, Edx, float load) {
    return Tire_GetCorneringStiffness_p(self, 0, bits(load)) * self->camber_stiffness_factor;
}
static void fp_get_camber_stiff(Footprint& f, const Tire*, Edx, float) { f.pure = true; }
PORT_FN(0x0043bbe0, "Tire::GetCamberStiffness", Tire_GetCamberStiffness, fp_get_camber_stiff)

// Tire::GetCamberGripFactor (0x43bc00): 1 + (gain / peak) x camber, camber clamped to [-peak, peak] and
// flipped to the slip's side. The first argument (the load) is unused. The flip is an integer test of the
// slip's bits (above 0x80000000: negative and not -0, or a negative NaN) and an fld/fchs/fstp; the lower
// clamp compares the register -peak ("fcom; test ah,0x41": assign if -peak > camber) and copies its stored
// float; the upper one ("fcomp; test ah,1": assign unless peak >= camber, so also for a NaN) copies the
// field's bits.
static double __fastcall Tire_GetCamberGripFactor(const Tire* self, Edx, float load, float slip, float camber) {
    (void)load;
    if (bits(slip) > 0x80000000u) camber = x87_chs_float(camber);
    double neg_peak = -D(self->camber_peak);
    float neg_peak_f = (float)neg_peak;
    if (neg_peak > camber) memcpy(&camber, &neg_peak_f, 4);
    if (!(D(self->camber_peak) >= camber)) memcpy(&camber, &self->camber_peak, 4);
    return (D(self->camber_peak_gain) / self->camber_peak) * camber + k_one.f;
}
static void fp_get_camber_grip(Footprint& f, const Tire*, Edx, float, float, float) { f.pure = true; }
PORT_FN(0x0043bc00, "Tire::GetCamberGripFactor", Tire_GetCamberGripFactor, fp_get_camber_grip)

// Tire::GetLateralFriction (0x43bc80): slope Fz + base
static double __fastcall Tire_GetLateralFriction(const Tire* self, Edx, float load) {
    return D(self->friction_slope) * load + self->friction_base;
}
static void fp_get_lat_friction(Footprint& f, const Tire*, Edx, float) { f.pure = true; }
PORT_FN(0x0043bc80, "Tire::GetLateralFriction", Tire_GetLateralFriction, fp_get_lat_friction)

// Tire::GetLongFriction (0x43bc90): GetLateralFriction x long_friction_factor
static double __fastcall Tire_GetLongFriction(const Tire* self, Edx, float load) {
    return Tire_GetLateralFriction_p(self, 0, bits(load)) * self->long_friction_factor;
}
static void fp_get_long_friction(Footprint& f, const Tire*, Edx, float) { f.pure = true; }
PORT_FN(0x0043bc90, "Tire::GetLongFriction", Tire_GetLongFriction, fp_get_long_friction)

// Tire::GetResultant (0x43bcb0): the magic formula, D sin(C atan(B(1-E)s + E atan(Bs))), written
// ((atan(Bs) (E/B)) + (1-E) s) B -- the atans by fld1; fpatan
static double __fastcall Tire_GetResultant(const Tire* self, Edx, float s) {
    double t = x87_atan_mul(D(self->pacejka_b) * s, D(self->pacejka_e) / self->pacejka_b);
    double u = (t + (D(k_one.f) - self->pacejka_e) * s) * self->pacejka_b;
    return x87_atan_mul_sin_mul(u, self->pacejka_c, self->pacejka_d);
}
static void fp_get_resultant(Footprint& f, const Tire*, Edx, float) { f.pure = true; }
PORT_FN(0x0043bcb0, "Tire::GetResultant", Tire_GetResultant, fp_get_resultant)

// TireBegin / TireEnd (0x43bd20, 0x43bd30): empty
static void __cdecl TireBegin() {}
PORT_FN(0x0043bd20, "TireBegin", TireBegin, fp_pure)
static void __cdecl TireEnd() {}
PORT_FN(0x0043bd30, "TireEnd", TireEnd, fp_pure)

// Tire::GetForce (0x43bd40): the tyre's force from its load, lateral slip, camber, longitudinal slip and a
// grip scale (on everything but the cornering stiffness). The lateral and longitudinal slips are normalised
// by stiffness / (mu Fz) and combined into one slip S; the magic formula's resultant R is shared out by
// direction, with the lateral slip weighted by w (a cosine blend of the two stiffness-to-grip ratios).
//   out = ( ((((1/N) w R slip_lat)(1 - sign cam) + cam) mu Fz,  Fz,  (1/N) R slip_long mu_long Fz )
//   *slide = max(0, 2/3 S - 1) (a NaN kept)
// Written in the original's order: *slide, then out.x, out.z, out.y (the load's bits).
static void __fastcall Tire_GetForce(const Tire* self, Edx, P3* out, float load, float slip_lat, float camber,
                                     float slip_long, float grip, float* slide) {
    uint32_t bl = bits(load);
    float cs = (float)Tire_GetCorneringStiffness_p(self, 0, bl);
    float mu = (float)(Tire_GetLateralFriction_p(self, 0, bl) * grip);
    float k_long = (float)(Tire_GetLongStiffness_p(self, 0, bl) * grip);
    float mu_long = (float)(Tire_GetLongFriction_p(self, 0, bl) * grip);
    float k_camber = (float)(Tire_GetCamberStiffness_p(self, 0, bl) * grip);
    double g = Tire_GetCamberGripFactor_p(self, 0, bl, bits(slip_lat), bits(camber)) * D(mu);
    mu = (float)g;                                      // fst: the register goes on
    double mu_fz = g * load;
    float sy = (float)((D(cs) / mu_fz) * slip_lat);
    double cam_r = (D(k_camber) / mu_fz) * camber;
    float cam = (float)cam_r;
    if (cam_r > k_0_9.f) cam = k_0_9.f;                 // fcom 0.9; test ah,0x41: the register value
    if (bits(cam) > 0xbf666666u) cam = from_bits(0xbf666666u);   // an integer compare of the bits: below -0.9
    float sign = (int32_t)bits(slip_lat) > 0 ? 1.0f : -1.0f;      // cmp dword, 0; jg (so +0 is -1)
    double one_minus_r = D(k_one.f) - D(sign) * cam;
    float one_minus = (float)one_minus_r;
    double x = D(sy) / one_minus_r;
    double y = (D(k_long) / (D(mu_long) * load)) * slip_long;
    float S = (float)x87_sqrt(y * y + x * x);
    float R = (float)Tire_GetResultant_p(self, 0, bits(S));
    float ratio = (float)((D(mu_long) * cs) / (D(k_long) * mu));
    float w;
    if (!(fabs(D(S)) > k_two_pi.f) && fabs(D(ratio) - k_one.f) > k_eps.f)
        w = (float)(x87_cos_mul(D(S) * k_half.f, D(k_one.f) - ratio) * k_neg_half.f +
                    (D(ratio) + k_one.f) * k_half.f);
    else
        w = 1.0f;
    double n_r = x87_sqrt((D(w) * w) * (D(slip_lat) * slip_lat) + D(slip_long) * slip_long);
    float n = (float)n_r;
    float inv_n = 1.0f;
    if (fabs(n_r) > k_eps.f) inv_n = (float)(D(k_one.f) / n);
    double v = D(S) * k_two_thirds.f - k_one.f;
    *slide = !(0.0 >= v) ? (float)v : 0.0f;            // fcom st(1); test ah,1: a NaN is kept
    out->x = (float)((((((D(inv_n) * w) * R) * slip_lat) * one_minus + cam) * mu) * load);
    out->z = (float)((((D(inv_n) * R) * slip_long) * mu_long) * load);
    memcpy(&out->y, &load, 4);
}
static void fp_tire_force(Footprint& f, const Tire*, Edx, P3* out, float, float, float, float, float, float* slide) {
    f.add(out, sizeof(P3), "force");
    f.add(slide, 4, "slide");
    f.pure = true;
}
PORT_FN(0x0043bd40, "Tire::GetForce", Tire_GetForce, fp_tire_force)

// TireDescGet (0x43c020): the 'TIRE' resource by name; a panic if it's missing
static TireDesc* __cdecl TireDescGet(const char* name) {
    uint32_t size;
    int flags;
    TireDesc* d = (TireDesc*)ResourceGet(name, 0x54495245, &size, &flags, 0, 0);
    if (!d) LogPanic(k_tire_not_found, name);
    return d;
}
static void fp_desc_get(Footprint& f, const char*) { f.replay_only = "resource load"; }
PORT_FN(0x0043c020, "TireDescGet", TireDescGet, fp_desc_get)

// TireDescForget (0x43c070)
static void __cdecl TireDescForget(TireDesc* d) { ResourceForget(d); }
static void fp_desc_forget(Footprint& f, TireDesc*) { f.replay_only = "resource release"; }
PORT_FN(0x0043c070, "TireDescForget", TireDescForget, fp_desc_forget)

// TireCreate (0x43c080): a copy of the .tir description with the car's tyre size in it, the load factor
// width / the .tir's width, and a new Tire set up from them
static Tire* __cdecl TireCreate(float width, float aspect, float diameter, const char* name) {
    TireDesc* p = TireDescGet_p(name);
    TireDesc desc;
    memcpy(&desc, p, sizeof desc);                     // rep movsd
    TireDescForget_p(p);
    float load_factor = (float)(D(width) / desc.ref_width_mm);
    memcpy(&desc.ref_width_mm, &width, 4);
    memcpy(&desc.diameter_in, &diameter, 4);
    memcpy(&desc.aspect_pct, &aspect, 4);
    Tire* t = (Tire*)MemAlloc(sizeof(Tire));
    setup_tire_p(t, &desc, bits(load_factor));
    return t;
}
static void fp_tire_create(Footprint& f, float, float, float, const char*) { f.replay_only = "allocates, loads a resource"; }
PORT_FN(0x0043c080, "TireCreate", TireCreate, fp_tire_create)

// setup_tire (0x43c120): the Tire from its description, through the setters (lb -> N, lb -> kg,
// lb ft^2 -> kg m^2, degrees -> sin of the angle), each argument computed just before its call
static void __cdecl setup_tire(Tire* t, const TireDesc* d, float load_factor) {
    Tire_SetMagicNumbers_p(t, 0, bits(d->pacejka_b), bits(d->pacejka_c), bits(d->pacejka_d), bits(d->pacejka_e));
    Tire_SetLoadFactor_p(t, 0, bits(load_factor));
    Tire_SetInertia_p(t, 0, (float)(D(d->mass_lb) * k_lb_to_kg.f), (float)(D(d->inertia_lbft2) * k_lbft2_to_kgm2.f));
    Tire_SetLateralFriction_p(t, 0, (float)(D(d->friction_load1_lb) * k_lb_to_n.f), bits(d->friction_mu1),
                              (float)(D(d->friction_load2_lb) * k_lb_to_n.f), bits(d->friction_mu2));
    Tire_SetCorneringStiffness_p(t, 0, (float)(D(d->cornering_load1_lb) * k_lb_to_n.f),
                                 (float)(D(d->cornering_stiffness1) * k_stiff_scale.f),
                                 (float)(D(d->cornering_load2_lb) * k_lb_to_n.f),
                                 (float)(D(d->cornering_stiffness2) * k_stiff_scale.f));
    Tire_SetCamberStiffnessFactor_p(t, 0, bits(d->camber_stiffness_factor));
    Tire_SetCamberGripPeak_p(t, 0, x87_sin_float(D(d->camber_peak_deg) * k_deg_tire.f), bits(d->camber_peak_gain));
    Tire_SetLongStiffnessFactor_p(t, 0, bits(d->long_stiffness_factor));
    Tire_SetLongFrictionFactor_p(t, 0, bits(d->long_friction_factor));
}
static void fp_setup_tire(Footprint& f, Tire* t, const TireDesc*, float) {
    f.add(t, sizeof(Tire), "tire");
    f.pure = true;
}
PORT_FN(0x0043c120, "setup_tire", setup_tire, fp_setup_tire)

// TireDestroy (0x43c230)
static void __cdecl TireDestroy(Tire* t) { operator_delete(t); }
static void fp_tire_destroy(Footprint& f, Tire*) { f.replay_only = "frees"; }
PORT_FN(0x0043c230, "TireDestroy", TireDestroy, fp_tire_destroy)

// ======== aero.obj ===========================================================================================

// AeroBegin / AeroEnd (0x43b1e0, 0x43b1f0): empty
static void __cdecl AeroBegin() {}
PORT_FN(0x0043b1e0, "AeroBegin", AeroBegin, fp_pure)
static void __cdecl AeroEnd() {}
PORT_FN(0x0043b1f0, "AeroEnd", AeroEnd, fp_pure)

// AeroGetAero (0x43b200): the i'th of the aero.obj statics (no bounds check)
static const Aero* __cdecl AeroGetAero(int i) { return (const Aero*)(uintptr_t)(0x00521d50 + (uint32_t)i * 36u); }
static void fp_get_aero(Footprint& f, int) { f.pure = true; }
PORT_FN(0x0043b200, "AeroGetAero", AeroGetAero, fp_get_aero)

// GetQuadratic (0x43b2c0): a b c with y = a x^2 + b x + c through three points: M = rows (x_i^2, x_i, 1)
// (MatrixSet), transposed by integer moves, inverted (MatrixInverse), and (a b c) = y x inverse
// (MatrixMulPoint). x is read first, y after the inverse; the outputs are written o0, o1, o2 as bits.
static void __cdecl GetQuadratic(float* o0, float* o1, float* o2, float* const x, float* const y) {
    P3 r0, r1, r2;
    uint32_t one = 0x3f800000;
    r2.x = (float)(D(x[2]) * x[2]);
    memcpy(&r2.y, &x[2], 4);
    memcpy(&r2.z, &one, 4);
    r1.x = (float)(D(x[1]) * x[1]);
    memcpy(&r1.y, &x[1], 4);
    memcpy(&r1.z, &one, 4);
    r0.x = (float)(D(x[0]) * x[0]);
    memcpy(&r0.y, &x[0], 4);
    memcpy(&r0.z, &one, 4);
    M3 m;
    MatrixSetRows(&m, &r0, &r1, &r2);
    uint32_t* u = (uint32_t*)m.m;                       // transpose: bit swaps
    uint32_t t;
    t = u[1]; u[1] = u[3]; u[3] = t;
    t = u[2]; u[2] = u[6]; u[6] = t;
    t = u[5]; u[5] = u[7]; u[7] = t;
    M3 inv;
    MatrixInverse(&inv, &m);
    P3 yy, r;
    memcpy(&yy, y, 12);
    MatrixMulPoint(&r, &yy, &inv);
    memcpy(o0, &r.x, 4);
    memcpy(o1, &r.y, 4);
    memcpy(o2, &r.z, 4);
}
static void fp_get_quadratic(Footprint& f, float* o0, float* o1, float* o2, float*, float*) {
    f.add(o0, 4, "a");
    f.add(o1, 4, "b");
    f.add(o2, 4, "c");
    f.pure = true;
}
PORT_FN(0x0043b2c0, "GetQuadratic", GetQuadratic, fp_get_quadratic)

// Aero::SetParams (0x43b210): angles to radians, coefficients scaled, and the two quadratics fitted
static void __fastcall Aero_SetParams(Aero* self, Edx, const AeroDescription* d) {
    memcpy(&self->_00, &d->_00, 4);
    self->angle_min = (float)(D(d->angle_min_deg) * k_deg_aero.f);
    self->angle_max = (float)(D(d->angle_max_deg) * k_deg_aero.f);
    float angle[3], lift[3], drag[3];
    for (int i = 0; i < 3; i++) {
        angle[i] = (float)(D(d->angle_deg[i]) * k_deg_aero.f);
        lift[i] = (float)(D(d->lift[i]) * k_aero_scale.f);
        drag[i] = (float)(D(d->drag[i]) * k_aero_scale.f);
    }
    GetQuadratic_p(&self->lift_a, &self->lift_b, &self->lift_c, angle, lift);
    GetQuadratic_p(&self->drag_a, &self->drag_b, &self->drag_c, angle, drag);
}
static void fp_aero_set_params(Footprint& f, Aero* self, Edx, const AeroDescription*) {
    f.add(self, sizeof(Aero), "aero");
    f.pure = true;
}
PORT_FN(0x0043b210, "Aero::SetParams", Aero_SetParams, fp_aero_set_params)

// Aero::GetLift / GetDrag (0x43b780, 0x43b7d0): the angle clamped to [min, max] (the lower bound if
// min > angle; the upper unless max >= angle, so also for a NaN; both copied as bits), then
// ((t t) a + b t + c) (v v)
static __forceinline double aero_quadratic(const Aero* self, float v, float t, const float* abc) {
    if (D(self->angle_min) > t) memcpy(&t, &self->angle_min, 4);
    if (!(D(self->angle_max) >= t)) memcpy(&t, &self->angle_max, 4);
    return (((D(t) * t) * abc[0] + D(abc[1]) * t) + abc[2]) * (D(v) * v);
}
static double __fastcall Aero_GetLift(const Aero* self, Edx, float v, float angle) {
    return aero_quadratic(self, v, angle, &self->lift_a);
}
static void fp_aero_lift(Footprint& f, const Aero*, Edx, float, float) { f.pure = true; }
PORT_FN(0x0043b780, "Aero::GetLift", Aero_GetLift, fp_aero_lift)

static double __fastcall Aero_GetDrag(const Aero* self, Edx, float v, float angle) {
    return aero_quadratic(self, v, angle, &self->drag_a);
}
static void fp_aero_drag(Footprint& f, const Aero*, Edx, float, float) { f.pure = true; }
PORT_FN(0x0043b7d0, "Aero::GetDrag", Aero_GetDrag, fp_aero_drag)

// Aero::GetForce (0x43b630): out = lift along the frame's row 1 plus drag along the velocity. The velocity
// (a P3DBase by value) arrives as its three floats. speed = max(|v|, 0.1) (the register |v| kept unless
// 0.1 >= it); the angle of attack = pitch - (row 1 . v) / speed; forward = -(row 2 . v).
// lift = -(GetLift(|forward|, angle) x sign(forward)), drag = GetDrag(speed, angle) / speed.
// out.x is zeroed first, and each out component is stored before the next frame element is read (as in
// the original, which matters only if out overlaps the frame).
static void __fastcall Aero_GetForce(const Aero* self, Edx, P3* out, float vx, float vy, float vz,
                                     const Frame* frame, float pitch) {
    double s = x87_sqrt((D(vz) * vz + D(vy) * vy) + D(vx) * vx);
    float speed = !(D(k_min_speed.f) >= s) ? (float)s : k_min_speed.f;
    const float* M = frame->rot.m;
    float angle = (float)(D(pitch) - ((D(M[5]) * vz + D(M[3]) * vx) + D(M[4]) * vy) / speed);
    double fwd_r = -((D(M[7]) * vy + D(M[6]) * vx) + D(M[8]) * vz);
    float fwd = (float)fwd_r;
    float sign = fwd_r > 0.0f ? 1.0f : -1.0f;          // fcom 0.0; test ah,0x41: the register value
    float abs_fwd = (float)fabs(D(fwd));
    uint32_t zero = 0;
    memcpy(&out->x, &zero, 4);
    double lift = -(Aero_GetLift_p(self, 0, bits(abs_fwd), bits(angle)) * sign);
    out->x = (float)(D(frame->rot.m[3]) * lift);
    out->y = (float)(D(frame->rot.m[4]) * lift);
    out->z = (float)(lift * frame->rot.m[5]);
    double drag = Aero_GetDrag_p(self, 0, bits(speed), bits(angle)) / speed;
    out->x = (float)(D(vx) * drag + out->x);
    out->y = (float)(D(vy) * drag + out->y);
    out->z = (float)(drag * vz + out->z);
}
static void fp_aero_force(Footprint& f, const Aero*, Edx, P3* out, float, float, float, const Frame*, float) {
    f.add(out, sizeof(P3), "force");
    f.pure = true;
}
PORT_FN(0x0043b630, "Aero::GetForce", Aero_GetForce, fp_aero_force)
