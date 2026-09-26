// phys_engine.cpp -- M3 3.3 group P2: the engine and the other car parts in physics:carpart.obj, rewritten.
//
// The Engine is a Shaft (a spinning inertia whose reaction torque goes into the car) with a torque curve
// (PowerCurve: a parabola through the peak-torque point, minus a linear fall-off past the power peak), a
// throttle (never quite shut: idle_throttle holds it at idle), a rev limiter, a starter, and two thermal
// masses, the block and the coolant, joined by a thermostat (a HeatTube). The Engine is embedded in the Car
// at +3068, and its owner (Setup's PhobDyno) is that Car. The Damper is a suspension damper's two-slope rate
// curve. Propeller and Wing are the hidden plane mode's parts (a car whose CarData+0x54 is negative); the
// TableTerrain and QuarterCar are the physics of the suspension test rig QuarterCarTest opens (its UI,
// QuarterCarControl, is left for the UI milestone).
//
// Written from the v1.0 disassembly: the same sums in the same grouping, register values as double, stored
// values as float, every call in the original's order with the same arguments, and floats the original moves
// with integer instructions copied as bits. Virtual calls (GetRPM, HeatTube::Update) stay virtual; the
// original's direct calls (Shaft::ApplyTorque, Shaft::GetDragTorque, get_engine_torque, ...) go to their
// v1.0 addresses, so whichever version is hooked there runs.
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// the unused edx of a __thiscall received as __fastcall (an int, as in phys_sphere.cpp: test/fuzz.h sizes it)
typedef int Edx;

static inline uint32_t bits(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }

// ---- constants, exactly as the original's -------------------------------------------------------------------
static const float k_lbft_to_nm = 0x1.59fce6p+0f;   // 0x3facfe73  1.3515152 (N m per lb ft, as the game has it)
static const float k_hp_torque = 0x1.484p+12f;      // 0x45a42000  5252: lb ft = hp x 5252 / rpm
static const float k_falloff = 0x1.0624dep-13f;     // 0x3903126f  0.000125
static const float k_rpm_to_rad = 0x1.aceeap-4f;    // 0x3dd67750  0.10471976 (2 pi / 60)
static const float k_dt = 0x1.0624dep-6f;           // 0x3c83126f  0.016 (the physics step)
static const float k_keep = 0x1.666666p-1f;         // 0x3f333333  0.7
static const float k_new = 0x1.333334p-2f;          // 0x3e99999a  0.3
static const float k_tenth = 0x1.99999ap-4f;        // 0x3dcccccd  0.1
static const float k_thermostat_open = 0x1.6d5556p+8f;   // 0x43b6aaab  365.33334 K
static const float k_thermostat_gain = 0x1.70a3d8p-3f;   // 0x3e3851ec  0.18 per K
static const float k_damper_a = 0x1.111112p-3f;     // 0x3e088889  0.13333334 (4 / 30: the two test speeds)
static const float k_damper_b = -0x1.276276p+0f;    // 0xbf93b13b  -1.1538461 (-30 / 26)
static const float k_damper_c = 0x1.3af5ecp+3f;     // 0x411d7af6  9.84252 (per inch/s -> per m/s over 4 in/s)
static const float k_fluct_a = 0x1.1c71c8p+2f;      // 0x408e38e4  4.4444447
static const float k_fluct_b = 0x1.0c6f7ap-23f;     // 0x340637bd  1.25e-07
static const float k_alpha0 = 0x1.becde6p-2f;       // 0x3edf66f3  0.43633232 (25 degrees)
static const float k_lift_slope = 0x1.e8ec8ap+1f;   // 0x40747645  3.8197186 (1 / 15 degrees)
static const float k_drag_slope = 0x1.0ced7p-3f;    // 0x3e0676b8  0.13131225
static const float k_drag_max = 0x1.99999ap-5f;     // 0x3d4ccccd  0.05
static const float k_drag_min = 0x1.89374cp-8f;     // 0x3bc49ba6  0.006
static const float k_prop_drag = -0x1.ce5602p-2f;   // 0xbee72b01  -0.45149997
static const float k_cl_max = 0x1.99999ap+0f;       // 0x3fcccccd  1.6
static const float k_air = 0x1.4a3d7p-1f;           // 0x3f251eb8  0.645 (half the air density)
static const float k_wing_area = 0x1.99999ap-3f;    // 0x3e4ccccd  0.2
static const float k_grip_ref = 0x1.f49ffep+11f;    // 0x457a4fff  4004.9998 N
static const float k_grip_slope = -0x1.b7da2ap-15f; // 0xb85bed15  -5.2434469e-05 per N
static const float k_grip_top = 0x1.30a3d8p+0f;     // 0x3f9851ec  1.19
static const float k_grip_floor = 0x1.333334p-1f;   // 0x3f19999a  0.6
static const float k_g = 0x1.39eb86p+3f;            // 0x411cf5c3  9.81
static const float k_bump_gap = 0x1.a02752p-4f;     // 0x3dd013a9  0.1016 m (4 in)

// ---- layouts (anonymous: other groups define their own partial Engine until these move to phys_types.h) ------
namespace {

// ThermalMass (20 bytes): Engine+0x18 (block, a base class of Engine: Setup's this-to-base conversion has the
// null check) and Engine+0x5c (coolant). Heat flows in through `heat`; UpdateHeat turns it into temperature.
struct ThermalMass {
    float conductance;                 // +0x00  W/K to the air
    float heat_capacity;               // +0x04  J/K
    float inv_heat_capacity;           // +0x08
    float temperature;                 // +0x0c  K
    float heat;                        // +0x10  J gathered this tick
};
static_assert(sizeof(ThermalMass) == 20, "ThermalMass");

// HeatTube (16 bytes, vtable 0x4dc838; slot 0 Update(dt)): conducts between two thermal masses
struct HeatTube {
    void** vtable;                     // +0x00
    float conductance;                 // +0x04  W/K
    ThermalMass* a;                    // +0x08
    ThermalMass* b;                    // +0x0c
};
static_assert(offsetof(HeatTube, b) == 0x0c && sizeof(HeatTube) == 16, "HeatTube");

// PowerCurve (24 bytes): T(r) = a r^2 + b r + c - max(0, r - falloff_rpm) x falloff_slope, N m
struct PowerCurve {
    float a;                           // +0x00  (T@power - T_max) / (power_rpm - torque_rpm)^2
    float b;                           // +0x04  -2 a torque_rpm + drag
    float c;                           // +0x08  a torque_rpm^2 + T_max
    float drag;                        // +0x0c  Setup arg 5 (bits)
    float falloff_rpm;                 // +0x10  Setup arg 2, the power peak (bits)
    float falloff_slope;               // +0x14  T@power x 0.000125
};
static_assert(offsetof(PowerCurve, falloff_slope) == 0x14 && sizeof(PowerCurve) == 24, "PowerCurve");

// Shaft (24 bytes, vtable 0x4dc820; slots 0 GetRPM, 4 ApplyTorque) -- phys_drivetrain.cpp ports its methods
struct Shaft {
    void** vtable;                     // +0x00
    PhobDyno* owner;                   // +0x04  Setup arg 1
    float inertia;                     // +0x08
    float inv_inertia;                 // +0x0c
    float rpm;                         // +0x10
    float drag;                        // +0x14  N m per rpm
};
static_assert(offsetof(Shaft, rpm) == 0x10 && sizeof(Shaft) == 24, "Shaft");

// Engine (128 bytes, vtable 0x4dc830: Shaft::GetRPM, Engine::ApplyTorque)
struct Engine : Shaft {
    ThermalMass block;                 // +0x18  237 x 0.625 W/K, 900 J/kgK x 80 kg, 290 K
    float throttle;                    // +0x2c  SetThrottle
    float idle_throttle;               // +0x30  Setup: the throttle whose torque equals drag at idle_rpm
    float idle_rpm;                    // +0x34  Setup arg 6
    float smoothed_throttle;           // +0x38  0.3 new + 0.7 old, each ApplyTorque
    float redline;                     // +0x3c  Setup arg 5 (ctor 6000)
    uint8_t cranking;                  // +0x40  Crank, while stalled: 80 N m starter torque
    uint8_t stalled;                   // +0x41  rpm <= 400 at the end of ApplyTorque
    uint8_t _42[2];
    PowerCurve curve;                  // +0x44  Setup arg 4, copied
    ThermalMass coolant;               // +0x5c  80.4 x 20 W/K, 4180 J/kgK x 12.8 kg, 290 K
    HeatTube thermostat;               // +0x70  block <-> coolant
};
static_assert(offsetof(Engine, block) == 0x18 && offsetof(Engine, throttle) == 0x2c &&
              offsetof(Engine, redline) == 0x3c && offsetof(Engine, stalled) == 0x41 &&
              offsetof(Engine, curve) == 0x44 && offsetof(Engine, coolant) == 0x5c &&
              offsetof(Engine, thermostat) == 0x70 && sizeof(Engine) == 128, "Engine");

// Damper (16 bytes): bump rate = slope v + base (v > 0); rebound = base - slope v; never below 0
struct Damper {
    float bump_slope;                  // +0x00
    float bump_base;                   // +0x04
    float rebound_slope;               // +0x08
    float rebound_base;                // +0x0c
};
static_assert(sizeof(Damper) == 16, "Damper");

// Propeller (36 bytes, vtable 0x4dc8c0: Shaft::GetRPM, Propeller::ApplyTorque): the plane's, at (0, 0, 0.6)
struct Propeller : Shaft {
    P3 pos;                            // +0x18  in the owner's frame (ctor arg)
};
static_assert(offsetof(Propeller, pos) == 0x18 && sizeof(Propeller) == 36, "Propeller");

// Wing (60 bytes, no vtable): a plane's wing, elevator or rudder
struct Wing {
    PhobDyno* owner;                   // +0x00  ctor arg 6
    float span;                        // +0x04  ctor arg 2 (6, 6, 5, 4.2 m)
    float chord;                       // +0x08  ctor arg 3 (2.5, 2.5, 1.5, 0.8 m)
    float _0c;                         // +0x0c  ctor arg 4 (0.09 for all four); Update doesn't read it
    P3 pos;                            // +0x10  ctor arg 1, in the owner's frame
    P3 local_velocity;                 // +0x1c  Update: the air velocity in the owner's frame (not read back)
    P3 arm;                            // +0x28  Update: pos in the world (GetArmPosition)
    int32_t axis;                      // +0x34  WingAxis: 0 lifts along the frame's row 1 (up), else row 0
    float angle;                       // +0x38  radians; Car::Update sets it each tick (ailerons, elevator)
};
static_assert(offsetof(Wing, pos) == 0x10 && offsetof(Wing, arm) == 0x28 && offsetof(Wing, angle) == 0x38 &&
              sizeof(Wing) == 60, "Wing");

// TableTerrain (0x4014 bytes, vtable 0x4dc840: 0 deleting dtor, 4 Reset, 8 GetHeight, 12 Step): a road
// profile the quarter car drives over, embedded in QuarterCarControl at +0x4c as a DefaultTerrain (vtable
// 0x4dc850, same methods): spacing 0.25 m, length 1024 m, 4096 heights (sums of sines, built by the ctor)
struct TableTerrain {
    void** vtable;                     // +0x00
    float spacing;                     // +0x04  m between table entries
    float length;                      // +0x08  the distance wraps at this
    float pos;                         // +0x0c  distance along the table
    float height;                      // +0x10  GetHeight: interpolated at pos (before the step)
    float table[4096];                 // +0x14
};
static_assert(offsetof(TableTerrain, table) == 0x14 && sizeof(TableTerrain) == 0x4014, "TableTerrain");
enum { TERRAIN_HEAD = 0x14 };          // what Reset and Step write

// QuarterCar (52 bytes): QuarterCarControl+0x18. Positions are up; the damper is QuarterCarControl+0x4078.
struct QuarterCar {
    float body_mass;                   // +0x00  kg (sprung)
    float wheel_mass;                  // +0x04  kg (unsprung)
    float spring_rate;                 // +0x08  N/m
    float tyre_rate;                   // +0x0c  N/m
    const Damper* damper;              // +0x10
    float body_pos;                    // +0x14
    float wheel_pos;                   // +0x18
    float body_vel;                    // +0x1c
    float wheel_vel;                   // +0x20
    float load_error;                  // +0x24  sum of (tyre deflection - static deflection)^2
    float grip;                        // +0x28  sum of mu(load) x load / weight
    int32_t contact_steps;             // +0x2c  steps with the tyre on the road
    int32_t steps;                     // +0x30
};
static_assert(offsetof(QuarterCar, damper) == 0x10 && offsetof(QuarterCar, steps) == 0x30 &&
              sizeof(QuarterCar) == 52, "QuarterCar");

}  // namespace

// ---- the game's functions these call (non-virtual calls by address) --------------------------------------
typedef void*(__fastcall* ShaftCtor_t)(void*, void*);
typedef void(__fastcall* ShaftSetup_t)(void*, void*, PhobDyno*, uint32_t inertia, uint32_t drag);
typedef void(__fastcall* ShaftApplyTorque_t)(void*, void*, float torque);
typedef double(__fastcall* ShaftGetDragTorque_t)(const void*, void*, uint32_t rpm);
typedef double(__fastcall* EngineTorque_t)(const void*, void*, uint32_t rpm, uint32_t throttle);
typedef double(__cdecl* PhysicsGetTemperature_t)();
typedef double(__fastcall* DamperRate_t)(const void*, void*, uint32_t v);
typedef void(__fastcall* GetArmPosition_t)(PhobDyno*, void*, P3* out, const P3* local);
typedef void(__fastcall* GetPointVelocity_t)(PhobDyno*, void*, P3* out, const P3* point);
typedef void(__cdecl* MatrixMulPointInv_t)(P3*, const P3*, const M3*);
typedef void(__fastcall* PhobDynoApplyForce_t)(PhobDyno*, void*, const P3* force, const P3* point);
static const ShaftCtor_t ShaftCtor = (ShaftCtor_t)0x00445e20;
static const ShaftSetup_t ShaftSetup = (ShaftSetup_t)0x00445e40;
static const ShaftApplyTorque_t ShaftApplyTorque = (ShaftApplyTorque_t)0x00445eb0;
static const ShaftGetDragTorque_t ShaftGetDragTorque = (ShaftGetDragTorque_t)0x00445f50;
static const EngineTorque_t EngineGetTorque = (EngineTorque_t)0x00446a00;       // Engine::get_engine_torque
static const PhysicsGetTemperature_t PhysicsGetTemperature = (PhysicsGetTemperature_t)0x0042bca0;
static const DamperRate_t DamperGetDampingRate = (DamperRate_t)0x00446ae0;
static const GetArmPosition_t GetArmPosition = (GetArmPosition_t)0x0043e0d0;   // PhobRoot::GetArmPosition
static const GetPointVelocity_t GetPointVelocity = (GetPointVelocity_t)0x00444ee0;
static const MatrixMulPointInv_t MatrixMulPointInv = (MatrixMulPointInv_t)0x00436120;
static const PhobDynoApplyForce_t PhobDynoApplyForce = (PhobDynoApplyForce_t)0x00444c20;

static void** const VT_Engine = (void**)0x004dc830;
static void** const VT_HeatTube = (void**)0x004dc838;
static void** const VT_Propeller = (void**)0x004dc8c0;

// fpatan -- atan2(y, x) -- and the 25 degree offset added in the same register, so the atan's 64-bit
// result is rounded once, by the fadd (precision control doesn't apply to fpatan), as the original's is
static __forceinline float x87_atan2_plus(float neg_y, double x, float k) {
    float r;
    __asm { fld neg_y
            fchs
            fld x
            fpatan
            fadd k
            fstp r }
    return r;
}

// __ftol (0x4cf108): x87_ftol, in x87.h

// ---- footprints ------------------------------------------------------------------------------------------
// The engine is embedded in the Car (whose base class ends at 3768), so a part owned by the car is covered by
// f.object(car); a part outside it (a plane's heap-allocated Propeller or Wing) is added on its own.
enum { CAR_SIZE = 3768 };
static void fp_car_part(Footprint& f, PhobDyno* car, void* self, uint32_t size, const char* what) {
    if (car) f.object(car, "car");
    if (!car || (uint8_t*)self < (uint8_t*)car || (uint8_t*)self >= (uint8_t*)car + CAR_SIZE) f.add(self, size, what);
}

// ==== PowerCurve ============================================================================================

// PowerCurve::Setup(power hp, power rpm, torque lb ft, torque rpm, drag) (0x446590): the parabola through
// (torque_rpm, T_max) and (power_rpm, T@power); T_max, T@power and a stay in registers, a is also stored
static void __fastcall PowerCurve_Setup(PowerCurve* self, Edx, float power, float power_rpm, float torque,
                                        float torque_rpm, float drag) {
    double t_max = D(torque) * k_lbft_to_nm;
    double t_power = D(power) / power_rpm;
    memcpy(&self->drag, &drag, 4);
    t_power = (t_power * k_hp_torque) * k_lbft_to_nm;
    double span = D(power_rpm) - torque_rpm;
    double a = (t_power - t_max) / (span * span);
    self->a = (float)a;
    self->b = (float)((D(torque_rpm) * a) * -2.0f + drag);
    memcpy(&self->falloff_rpm, &power_rpm, 4);
    self->c = (float)((D(torque_rpm) * torque_rpm) * a + t_max);
    self->falloff_slope = (float)(t_power * k_falloff);
}
static void fp_curve_setup(Footprint& f, PowerCurve* self, Edx, float, float, float, float, float) {
    f.add(self, sizeof(PowerCurve), "power curve");
    f.pure = true;
}
PORT_FN(0x00446590, "PowerCurve::Setup", PowerCurve_Setup, fp_curve_setup)

// ==== Engine ================================================================================================

// the curve's torque at rpm, before the throttle: ((a r r - max(0, r - falloff) slope) + b r) + c, in ST0
static double curve_torque(const PowerCurve& c, float rpm) {
    double over = D(rpm) - c.falloff_rpm;
    double past = !(0.0f >= over) ? over : 0.0f;    // fcom st(1); test ah,1: a NaN passes through
    return (((D(rpm) * rpm) * c.a - past * c.falloff_slope) + D(c.b) * rpm) + c.c;
}

// Engine::Engine (0x446630). Its products of constants are worked at run time in the FPU's current precision,
// as the original's are; loading the constants through volatiles keeps the compiler from folding them (in
// double) at compile time.
static volatile float kv_block_c = 900.0f, kv_block_m = 80.0f, kv_one = 1.0f, kv_block_k = 237.0f,
                      kv_block_f = 0.625f, kv_cool_c = 4180.0f, kv_cool_m = 0x1.99999ap+3f /* 12.8 */,
                      kv_cool_k = 0x1.41999ap+6f /* 80.4 */, kv_cool_f = 20.0f;
static Engine* __fastcall Engine_Ctor(Engine* self, Edx) {
    ShaftCtor(self, 0);
    double hc = D(kv_block_c) * kv_block_m;
    self->block.temperature = 290.0f;
    self->block.heat = 0.0f;
    self->block.heat_capacity = (float)hc;
    self->block.inv_heat_capacity = (float)(D(kv_one) / hc);
    self->block.conductance = (float)(D(kv_block_k) * kv_block_f);
    double hc2 = D(kv_cool_c) * kv_cool_m;
    self->coolant.temperature = 290.0f;
    self->coolant.heat = 0.0f;
    self->thermostat.vtable = VT_HeatTube;
    self->thermostat.conductance = 0.0f;
    self->thermostat.a = 0;
    self->coolant.heat_capacity = (float)hc2;
    double inv2 = D(kv_one) / hc2;
    self->vtable = VT_Engine;
    self->throttle = 0.0f;
    self->smoothed_throttle = 0.0f;
    self->redline = 6000.0f;
    self->idle_throttle = 0.0f;
    self->cranking = 0;
    self->stalled = 0;
    self->thermostat.b = 0;
    self->coolant.inv_heat_capacity = (float)inv2;
    self->coolant.conductance = (float)(D(kv_cool_k) * kv_cool_f);
    return self;
}
static void fp_engine_self(Footprint& f, Engine* self, Edx) { f.add(self, sizeof(Engine), "engine"); }
PORT_FN(0x00446630, "Engine::Engine", Engine_Ctor, fp_engine_self)

// Engine::Setup(PhobDyno*, inertia, drag, PowerCurve const&, redline, idle_rpm) (0x446700): idle_throttle is
// the throttle whose torque (with the redline scaling ApplyTorque uses) equals drag x idle_rpm
static void __fastcall Engine_Setup(Engine* self, Edx, PhobDyno* owner, float inertia, float drag,
                                    const PowerCurve* curve, float redline, float idle_rpm) {
    self->thermostat.a = self ? &self->block : 0;   // the original's this-to-base conversion
    self->thermostat.b = &self->coolant;
    memcpy(&self->curve, curve, sizeof(PowerCurve));   // rep movsd
    ShaftSetup(self, 0, owner, bits(inertia), bits(drag));
    double t = curve_torque(self->curve, idle_rpm);
    double m = !(1000.0f >= D(idle_rpm)) ? D(idle_rpm) : 1000.0f;
    double scale = D(redline) / m;
    memcpy(&self->redline, &redline, 4);
    self->rpm = 1000.0f;
    memcpy(&self->idle_rpm, &idle_rpm, 4);
    double full = t * scale;
    self->idle_throttle = (float)((D(idle_rpm) / full) * drag);
}
static void fp_engine_setup(Footprint& f, Engine* self, Edx, PhobDyno*, float, float, const PowerCurve*, float,
                            float) {
    f.add(self, sizeof(Engine), "engine");
}
PORT_FN(0x00446700, "Engine::Setup", Engine_Setup, fp_engine_setup)

// Engine::SetThrottle(float) (0x4467f0): a bit copy
static void __fastcall Engine_SetThrottle(Engine* self, Edx, float throttle) {
    memcpy(&self->throttle, &throttle, 4);
}
static void fp_engine_throttle(Footprint& f, Engine* self, Edx, float) {
    f.add(self, sizeof(Engine), "engine");
    f.pure = true;
}
PORT_FN(0x004467f0, "Engine::SetThrottle", Engine_SetThrottle, fp_engine_throttle)

// Engine::Crank (0x446800): the starter, while stalled
static void __fastcall Engine_Crank(Engine* self, Edx) {
    if (self->stalled) self->cranking = 1;
}
static void fp_engine_crank(Footprint& f, Engine* self, Edx) {
    f.add(self, sizeof(Engine), "engine");
    f.pure = true;
}
PORT_FN(0x00446800, "Engine::Crank", Engine_Crank, fp_engine_crank)

// Engine::get_engine_torque(rpm, throttle) (0x446a00, private): the curve x throttle, stored; 0 at or past the
// redline; while stalled, 80 N m if cranking, else 0
static float __fastcall Engine_GetEngineTorque(const Engine* self, Edx, float rpm, float throttle) {
    float t = (float)(curve_torque(self->curve, rpm) * throttle);
    if (!self->stalled) return self->redline > rpm ? t : 0.0f;   // fcomp; test ah,0x41: a NaN gives 0
    return self->cranking ? 80.0f : 0.0f;
}
static void fp_engine_torque(Footprint& f, const Engine*, Edx, float, float) { f.pure = true; }
PORT_FN(0x00446a00, "Engine::get_engine_torque", Engine_GetEngineTorque, fp_engine_torque)

// Engine::GetNetTorque(rpm, throttle) (0x4469d0): torque (stored) minus the shaft's drag (ST0), in ST0
static double __fastcall Engine_GetNetTorque(const Engine* self, Edx, float rpm, float throttle) {
    float t = (float)EngineGetTorque(self, 0, bits(rpm), bits(throttle));
    return D(t) - ShaftGetDragTorque(self, 0, bits(rpm));
}
static void fp_engine_net(Footprint& f, const Engine*, Edx, float, float) { f.pure = true; }
PORT_FN(0x004469d0, "Engine::GetNetTorque", Engine_GetNetTorque, fp_engine_net)

// Engine::ApplyTorque(float) (0x446810, virtual): the load torque from the gearbox plus the engine's own,
// through Shaft::ApplyTorque (called directly, as the original's base-class call is); heat into the block
// from the torque and the friction; the thermostat opens from 365.33 K over 5.6 K; stalled below 400 rpm
static void __fastcall Engine_ApplyTorque(Engine* self, Edx, float torque) {
    self->smoothed_throttle = (float)(D(self->smoothed_throttle) * k_keep + D(self->throttle) * k_new);
    float rpm = (float)VFN(self, 0, double)(self, 0);
    double open = (1.0f - D(self->idle_throttle)) * self->throttle + self->idle_throttle;
    double m = !(1000.0f >= D(rpm)) ? D(rpm) : 1000.0f;
    double x = open * (D(self->redline) / m);
    float scale = (float)(1.0f > x ? x : D(1.0f));  // fcom; test ah,0x41: a NaN gives 1
    double t = EngineGetTorque(self, 0, bits(rpm), bits(scale));
    float t_f = (float)t;
    ShaftApplyTorque(self, 0, (float)(t + torque));
    double drag = ShaftGetDragTorque(self, 0, bits(rpm));
    self->block.heat = (float)((((drag + D(t_f) * k_tenth) * rpm) * k_rpm_to_rad) * k_dt + self->block.heat);
    double open_t = (D(self->block.temperature) - k_thermostat_open) * k_thermostat_gain;
    float valve = (float)open_t;
    if (!(open_t >= 0.0f)) valve = 0.0f;            // fcom 0; test ah,1: a NaN shuts it
    else valve = 1.0f > valve ? valve : 1.0f;
    self->thermostat.conductance = (float)(D(valve) * 10000.0f);
    VFN(&self->thermostat, 0, void, float)(&self->thermostat, 0, k_dt);
    if ((int32_t)bits(rpm) > (int32_t)0x43c80000) { // an integer compare of the float's bits with 400.0f
        self->cranking = 0;
        self->stalled = 0;
    } else {
        self->stalled = 1;
    }
}
static void fp_engine_apply(Footprint& f, Engine* self, Edx, float) {
    fp_car_part(f, self->owner, self, sizeof(Engine), "engine");
}
PORT_FN(0x00446810, "Engine::ApplyTorque", Engine_ApplyTorque, fp_engine_apply)

// Engine::UpdateHeat(dt) (0x446970): each mass loses (T - ambient) x conductance x dt to the air, then its
// gathered heat becomes temperature and is cleared. The ambient temperature stays in ST0 throughout.
static void __fastcall Engine_UpdateHeat(Engine* self, Edx, float dt) {
    double ambient = PhysicsGetTemperature();
    ThermalMass& b = self->block;
    b.heat = (float)(((ambient - b.temperature) * b.conductance) * dt + b.heat);
    float h = b.heat;
    b.heat = 0.0f;
    b.temperature = (float)(D(h) * b.inv_heat_capacity + b.temperature);
    ThermalMass& c = self->coolant;
    c.heat = (float)(((ambient - c.temperature) * c.conductance) * dt + c.heat);
    h = c.heat;
    c.heat = 0.0f;
    c.temperature = (float)(D(h) * c.inv_heat_capacity + c.temperature);
}
static void fp_engine_heat(Footprint& f, Engine* self, Edx, float) { f.add(self, sizeof(Engine), "engine"); }
PORT_FN(0x00446970, "Engine::UpdateHeat", Engine_UpdateHeat, fp_engine_heat)

// ==== HeatTube ==============================================================================================

// HeatTube::Update(dt) (0x4472f0, virtual): q = ((b.T - a.T) x conductance) x dt; a gains it, b loses it
static void __fastcall HeatTube_Update(HeatTube* self, Edx, float dt) {
    ThermalMass* a = self->a;
    double q = ((D(self->b->temperature) - a->temperature) * self->conductance) * dt;
    a->heat = (float)(D(a->heat) + q);
    self->b->heat = (float)(D(self->b->heat) - q);  // b re-read after the store, as the original does
}
static void fp_heat_tube(Footprint& f, HeatTube* self, Edx, float) {
    f.add(&self->a->heat, 4, "heat tube a");
    f.add(&self->b->heat, 4, "heat tube b");
}
PORT_FN(0x004472f0, "HeatTube::Update", HeatTube_Update, fp_heat_tube)

// ==== Damper ================================================================================================

// Damper::Setup(bump @4 in/s, bump @30 in/s, rebound @4, rebound @30) (0x446a90): the line through both
// points; the base stays in the register for the slope
static void __fastcall Damper_Setup(Damper* self, Edx, float bump_slow, float bump_fast, float rebound_slow,
                                    float rebound_fast) {
    double base = (D(bump_fast) * k_damper_a - bump_slow) * k_damper_b;
    self->bump_base = (float)base;
    self->bump_slope = (float)((D(bump_slow) - base) * k_damper_c);
    base = (D(rebound_fast) * k_damper_a - rebound_slow) * k_damper_b;
    self->rebound_base = (float)base;
    self->rebound_slope = (float)((D(rebound_slow) - base) * k_damper_c);
}
static void fp_damper_setup(Footprint& f, Damper* self, Edx, float, float, float, float) {
    f.add(self, sizeof(Damper), "damper");
    f.pure = true;
}
PORT_FN(0x00446a90, "Damper::Setup", Damper_Setup, fp_damper_setup)

// Damper::GetDampingRate(v) (0x446ae0): bump when v's bits are a positive integer (v > 0, or a +NaN), else
// rebound; stored, then floored at 0 (fcom; test ah,1: a NaN passes through)
static float __fastcall Damper_GetDampingRate(const Damper* self, Edx, float v) {
    float r;
    if ((int32_t)bits(v) > 0) r = (float)(D(self->bump_slope) * v + self->bump_base);
    else r = (float)(D(self->rebound_base) - D(self->rebound_slope) * v);
    return !(0.0f >= r) ? r : 0.0f;
}
static void fp_damper_rate(Footprint& f, const Damper*, Edx, float) { f.pure = true; }
PORT_FN(0x00446ae0, "Damper::GetDampingRate", Damper_GetDampingRate, fp_damper_rate)

// GetLoadFluctuation(4 floats, Damper const*, float) (0x446b20): ((arg6 + 4.4444447) x arg4) x 1.25e-7 in
// ST0; the other arguments are unused
static double __cdecl GetLoadFluctuation(float, float, float, float x4, const Damper*, float x6) {
    return ((D(x6) + k_fluct_a) * x4) * k_fluct_b;
}
static void fp_load_fluct(Footprint& f, float, float, float, float, const Damper*, float) { f.pure = true; }
PORT_FN(0x00446b20, "GetLoadFluctuation", GetLoadFluctuation, fp_load_fluct)

// ==== Propeller (plane mode) ================================================================================

// Propeller::Propeller(P3DBase) (0x446e60): the position is passed by value -- three dwords on the stack,
// received here as their bits -- and copied with integer moves
static Propeller* __fastcall Propeller_Ctor(Propeller* self, Edx, uint32_t x, uint32_t y, uint32_t z) {
    ShaftCtor(self, 0);
    self->vtable = VT_Propeller;
    memcpy(&self->pos.x, &x, 4);
    memcpy(&self->pos.y, &y, 4);
    memcpy(&self->pos.z, &z, 4);
    return self;
}
static void fp_prop_self(Footprint& f, Propeller* self, Edx, uint32_t, uint32_t, uint32_t) {
    f.add(self, sizeof(Propeller), "propeller");
}
PORT_FN(0x00446e60, "Propeller::Propeller", Propeller_Ctor, fp_prop_self)

// Propeller::ApplyTorque(float) (0x446e90, virtual): a blade at 0.7 m sees the air at atan2(-forward speed,
// tip speed) + 25 degrees; thrust along the car's forward axis (row 2), applied at the propeller; its drag
// torque is added to the load before Shaft::ApplyTorque (called directly)
static void __fastcall Propeller_ApplyTorque(Propeller* self, Edx, float torque) {
    P3 arm;
    GetArmPosition(self->owner, 0, &arm, &self->pos);
    const PhobDyno* o = self->owner;
    const float* R = o->frame.rot.m;
    float forward = (float)((D(R[8]) * o->velocity.z + D(o->velocity.y) * R[7]) + D(R[6]) * o->velocity.x);
    float tip = (float)((VFN(self, 0, double)(self, 0) * k_rpm_to_rad) * k_keep);
    double tip_d = tip;                             // fstp qword: kept for the atan and the fabs
    float alpha = x87_atan2_plus(forward, tip_d, k_alpha0);
    float q = (float)(D(tip) * tip + D(forward) * forward);
    double cl_r = D(alpha) * k_lift_slope;
    float cl = (float)cl_r;
    if (!(cl_r >= -1.0f)) cl = -1.0f;               // fcom -1; test ah,1: a NaN gives -1
    else cl = 1.0f > cl ? cl : 1.0f;
    double cd = (D(alpha) * alpha) * k_drag_slope;
    cd = k_drag_max > cd ? cd : D(k_drag_max);      // test ah,0x41: a NaN gives 0.05
    cd = cd + k_drag_min;
    float drag = (float)(((fabs(tip_d) * cd) * tip) * k_prop_drag);
    double lift = ((D(cl) * k_cl_max) * q) * k_air;
    float lift_f = (float)lift;
    R = self->owner->frame.rot.m;
    P3 force;
    force.x = (float)(lift * R[6]);                 // x from the register, y and z from the stored lift
    force.y = (float)(D(R[7]) * lift_f);
    force.z = (float)(D(R[8]) * lift_f);
    PhobDynoApplyForce(self->owner, 0, &force, &arm);
    ShaftApplyTorque(self, 0, (float)(D(drag) + torque));
}
static void fp_prop_apply(Footprint& f, Propeller* self, Edx, float) {
    fp_car_part(f, self->owner, self, sizeof(Propeller), "propeller");
}
PORT_FN(0x00446e90, "Propeller::ApplyTorque", Propeller_ApplyTorque, fp_prop_apply)

// ==== Wing (plane mode) =====================================================================================

// Wing::Wing(P3DBase const&, span, chord, float, WingAxis, PhobDyno*) (0x447010): everything by integer moves
static Wing* __fastcall Wing_Ctor(Wing* self, Edx, const P3* pos, float span, float chord, float x0c,
                                  int32_t axis, PhobDyno* owner) {
    memcpy(&self->span, &span, 4);
    memcpy(&self->chord, &chord, 4);
    memcpy(&self->_0c, &x0c, 4);
    self->axis = axis;
    memcpy(&self->pos, pos, sizeof(P3));
    memset(&self->local_velocity, 0, sizeof(P3));
    self->owner = owner;
    memset(&self->arm, 0, sizeof(P3));
    memset(&self->angle, 0, 4);
    return self;
}
static void fp_wing_self(Footprint& f, Wing* self, Edx, const P3*, float, float, float, int32_t, PhobDyno*) {
    f.add(self, sizeof(Wing), "wing");
}
PORT_FN(0x00447010, "Wing::Wing", Wing_Ctor, fp_wing_self)

// Wing::Update (0x447070): lift along the wing's axis row from the angle of attack (angle - the air's
// component along that row), drag along -row 2 (the car's forward axis, whichever way the air comes), both
// x area x 0.645 x speed^2, applied at the wing. Lift is cut when the car moves backwards.
static void __fastcall Wing_Update(Wing* self, Edx) {
    GetArmPosition(self->owner, 0, &self->arm, &self->pos);
    P3 vel;
    GetPointVelocity(self->owner, 0, &vel, &self->arm);
    MatrixMulPointInv(&self->local_velocity, &vel, &self->owner->frame.rot);
    double speed = x87_sqrt((D(vel.y) * vel.y + D(vel.z) * vel.z) + D(vel.x) * vel.x);
    float speed_f = (float)speed;
    double n = !(1.0f >= speed) ? speed : D(1.0f);  // fcom; test ah,1: a NaN passes through
    float n_f = (float)n;
    float ux = (float)(D(vel.x) / n);               // x over the register, y and z over the stored value
    float uy = (float)(D(vel.y) / n_f);
    float uz = (float)(D(vel.z) / n_f);
    const PhobDyno* o = self->owner;
    P3 normal, fwd;
    memcpy(&normal, self->axis != 0 ? &o->frame.rot.m[0] : &o->frame.rot.m[3], sizeof(P3));
    memcpy(&fwd, &o->frame.rot.m[6], sizeof(P3));
    float aoa = (float)(D(self->angle) - ((D(normal.y) * uy + D(normal.z) * uz) + D(normal.x) * ux));
    float area = (float)((D(self->span) * self->chord) * k_wing_area);
    double cl_r = D(aoa) * k_lift_slope;
    float cl = (float)cl_r;
    if (!(cl_r >= -1.0f)) cl = -1.0f;               // fcom -1; test ah,1: a NaN gives -1
    else cl = 1.0f > cl ? cl : 1.0f;
    float lift = (float)(D(cl) * k_cl_max);
    if (!((D(fwd.y) * vel.y + D(fwd.z) * vel.z) + D(fwd.x) * vel.x >= 0.0f)) lift = 0.0f;
    double cd = (D(aoa) * aoa) * k_drag_slope;
    cd = k_drag_max > cd ? cd : D(k_drag_max);      // test ah,0x41: a NaN gives 0.05
    float drag = (float)(cd + k_drag_min);
    float speed2 = (float)(D(speed_f) * speed_f);
    P3 force;
    force.x = (float)((((-(D(fwd.x) * drag) + D(lift) * normal.x) * area) * speed2) * k_air);
    force.y = (float)((((-(D(fwd.y) * drag) + D(lift) * normal.y) * area) * speed2) * k_air);
    force.z = (float)((((-(D(fwd.z) * drag) + D(lift) * normal.z) * area) * speed2) * k_air);
    PhobDynoApplyForce(self->owner, 0, &force, &self->arm);
}
static void fp_wing_update(Footprint& f, Wing* self, Edx) {
    fp_car_part(f, self->owner, self, sizeof(Wing), "wing");
}
PORT_FN(0x00447070, "Wing::Update", Wing_Update, fp_wing_update)

// ==== the quarter-car rig (QuarterCarTest's physics) ===============================================================

// TableTerrain::Reset (0x447490, virtual)
static void __fastcall TableTerrain_Reset(TableTerrain* self, Edx) {
    memset(&self->pos, 0, 4);
    memset(&self->height, 0, 4);
}
static void fp_terrain_reset(Footprint& f, TableTerrain* self, Edx) { f.add(self, TERRAIN_HEAD, "terrain"); }
PORT_FN(0x00447490, "TableTerrain::Reset", TableTerrain_Reset, fp_terrain_reset)

// TableTerrain::GetHeight (0x447540, virtual)
static float __fastcall TableTerrain_GetHeight(TableTerrain* self, Edx) { return self->height; }
static void fp_terrain_height(Footprint&, TableTerrain*, Edx) {}
PORT_FN(0x00447540, "TableTerrain::GetHeight", TableTerrain_GetHeight, fp_terrain_height)

// TableTerrain::Step(dt, speed) (0x4474a0, virtual): the height interpolated at pos (the first index isn't
// wrapped: the original trusts pos < length), then pos advances and wraps by whole lengths
static void __fastcall TableTerrain_Step(TableTerrain* self, Edx, float dt, float speed) {
    int32_t i = x87_ftol(D(self->pos) / self->spacing);
    double h0 = self->table[i];
    int32_t j = (i + 1) & 0xfff;
    double frac = (D(self->pos) - D(i) * self->spacing) / self->spacing;
    self->height = (float)(h0 + (D(self->table[j]) - h0) * frac);
    self->pos = (float)(D(speed) * dt + self->pos);
    if (self->pos >= self->length) {                // fcomp; test ah,1 jumps over for less or a NaN
        double len = self->length;
        do self->pos = (float)(D(self->pos) - len);
        while (!(len > self->pos));                 // fcom; test ah,0x41
    }
}
static void fp_terrain_step(Footprint& f, TableTerrain* self, Edx, float, float) {
    f.add(self, TERRAIN_HEAD, "terrain");
}
PORT_FN(0x004474a0, "TableTerrain::Step", TableTerrain_Step, fp_terrain_step)

// QuarterCar::Step(dt, road height) (0x447aa0): the tyre (only pushes; counts contact steps), the load-
// sensitive grip it would give (sum), the tyre deflection's error from static (sum of squares), spring +
// damper (Damper::GetDampingRate, called directly) with a 3x bump stop 4 in past static, then an Euler step
static void __fastcall QuarterCar_Step(QuarterCar* self, Edx, float dt, float road) {
    double tyre = (D(road) - self->wheel_pos) * self->tyre_rate;
    float tyre_f = (float)tyre;
    if (!(tyre >= 0.0f)) tyre_f = 0.0f;             // fcom 0; test ah,1
    else self->contact_steps++;
    float mass = (float)(D(self->wheel_mass) + self->body_mass);
    double mu = (D(tyre_f) - k_grip_ref) * k_grip_slope + k_grip_top;
    mu = !(k_grip_floor >= mu) ? mu : D(k_grip_floor);   // test ah,1: a NaN passes through
    mu = k_grip_top > mu ? mu : D(k_grip_top);           // test ah,0x41: a NaN gives 1.19
    self->grip = (float)((mu / (D(mass) * k_g)) * tyre_f + self->grip);
    float static_defl = (float)((D(mass) / self->tyre_rate) * k_g);
    float v = (float)(D(self->wheel_vel) - self->body_vel);
    float rate = (float)DamperGetDampingRate(self->damper, 0, bits(v));
    float compression = (float)(D(self->wheel_pos) - self->body_pos);
    float spring = (float)(D(rate) * v + D(self->spring_rate) * compression);
    double stop = (D(self->body_mass) * k_g) / self->spring_rate + k_bump_gap;
    float stop_f = (float)stop;
    if (!(stop >= compression))                     // fcom; test ah,1
        spring = (float)((D(compression) - stop_f) * (D(self->spring_rate) * 3.0f) + spring);
    double wheel_acc = (D(tyre_f) - spring) + D(self->wheel_mass) * -k_g;
    self->steps++;
    double body_acc = D(self->body_mass) * -k_g + spring;
    double err = (D(self->wheel_pos) - road) + static_defl;
    self->load_error = (float)(err * err + self->load_error);
    self->wheel_vel = (float)((wheel_acc / self->wheel_mass) * dt + self->wheel_vel);
    self->body_vel = (float)((body_acc / self->body_mass) * dt + self->body_vel);
    self->wheel_pos = (float)(D(self->wheel_vel) * dt + self->wheel_pos);
    self->body_pos = (float)(D(self->body_vel) * dt + self->body_pos);
}
static void fp_quarter_car(Footprint& f, QuarterCar* self, Edx, float, float) {
    f.add(self, sizeof(QuarterCar), "quarter car");
}
PORT_FN(0x00447aa0, "QuarterCar::Step", QuarterCar_Step, fp_quarter_car)
