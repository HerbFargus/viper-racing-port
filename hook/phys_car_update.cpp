// phys_car_update.cpp -- M3 3.4 group C1: the Car's per-tick update (physics:car.obj), rewritten.
//
// Car::Update is the car's whole tick: the grid hold, the post-reset lowering, the out-of-bounds teleport,
// the starter, the drivetrain (gearbox, clutch, engine, differentials through the wheels), the brake split
// and the yaw-control brakes, the four Wheel::Updates, the brake / engine heat every 8th step, the
// yaw-stabilising force 70 m ahead (realism 0 and 1), the anti-roll bars, the air velocity and body drag,
// the lift and spoiler drag at each axle (and a plane's wings), PhobDyno's integration, the draft point,
// the g-meters, the lap top speed, the damage timeout and the deity's per-car update. UpdateCommon is the
// per-frame side: the sounds and the status string. With them: apply_yaw_control, UpdateDraft, UpdateHeat,
// loc_is_out_of_bounds, WheelsSkidding, AnyWheelsAreDamaged, ApplySuspensionForce, the control setters,
// HelpOut and SuperHelpOut.
//
// Written from the v1.0 disassembly, block by block: the same sums in the same grouping, values the
// original keeps in x87 registers as doubles and what it stores as floats, every call in the original's
// order with the same arguments (floats it passes with integer moves passed as their bits), and every
// integer compare of a float's bits done on the bits. Every callee -- the wheels, the drivetrain parts,
// PhobDyno's helpers, the Car methods of the other car.obj groups, the sounds, the deity -- is called by
// address or through its vtable exactly as the original does, so whichever version is hooked there is
// what runs. Nothing is inlined.
//
// In the comments, [+0xNN] is a local's offset in the original's frame (after its register pushes).
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// the unused edx of a __thiscall received as __fastcall (an int, as in phys_sphere.cpp: test/fuzz.h sizes it)
typedef int Edx;

// a float's bits, for the original's integer compares (cmp dword ptr [x], imm) and integer copies
static __forceinline int32_t I(const float& x) { int32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline uint32_t Ub(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
#define COPY4(dst, src) memcpy(&(dst), &(src), 4)  // an integer move of a float

// ---- layouts (anonymous: the other car.obj / wheel.obj groups define their own until these move to
// phys_types.h) ------------------------------------------------------------------------------------------------
namespace {

// Wheel (wheel.obj, 420 bytes, vtable 0x4dca78), embedded 4x in Car at +0x554; only what this group touches
// is named (the full layout is in phys_wheel_update.cpp)
struct Wheel {
    void** vtable;                     // +0x000
    uint8_t _004[0x20 - 0x04];
    uint8_t broken, _021[3];           // +0x020
    uint8_t _024[0xa4 - 0x24];
    float aligning_torque;             // +0x0a4 steering feedback
    uint8_t _0a8[0xdc - 0xa8];
    float steer_input;                 // +0x0dc rad
    float brake_input;                 // +0x0e0 -1..1 (negative: the e-brake locks it)
    float drive_torque;                // +0x0e4
    float compression;                 // +0x0e8 m
    uint8_t _0ec[0x11c - 0xec];
    float slide;                       // +0x11c
    uint8_t fx_flags;                  // +0x120 0x20: off-road
    uint8_t _121[3];
    uint8_t _124[0x158 - 0x124];
    float anti_roll_force;             // +0x158
    int32_t traction_control;          // +0x15c
    uint8_t _160[4];
    int32_t abs_mode;                  // +0x164
    uint8_t on_ground, _169[3];        // +0x168
    uint8_t _16c[0x184 - 0x16c];
    void* tire_sound;                  // +0x184 TireSound*
    uint8_t _188[0x1a4 - 0x188];
};
static_assert(offsetof(Wheel, broken) == 0x20 && offsetof(Wheel, aligning_torque) == 0xa4 &&
              offsetof(Wheel, steer_input) == 0xdc && offsetof(Wheel, compression) == 0xe8 &&
              offsetof(Wheel, slide) == 0x11c && offsetof(Wheel, anti_roll_force) == 0x158 &&
              offsetof(Wheel, on_ground) == 0x168 && offsetof(Wheel, tire_sound) == 0x184 &&
              sizeof(Wheel) == 0x1a4, "Wheel");

// the drivetrain parts (carpart.obj; layouts in phys_drivetrain.cpp / phys_engine.cpp): only what's read here
struct Engine {                        // 128 bytes, vtable 0x4dc830: slot 0 GetRPM
    void** vtable;
    uint8_t _04[0x40 - 0x04];
    uint8_t cranking;                  // +0x40
    uint8_t stalled;                   // +0x41
    uint8_t _42[128 - 0x42];
};
static_assert(sizeof(Engine) == 128, "Engine");
struct Clutch {                        // 28 bytes, no vtable
    float engagement;                  // +0x00
    uint8_t _04[28 - 4];
};
static_assert(sizeof(Clutch) == 28, "Clutch");
struct Transmission {                  // 108 bytes, vtable 0x4dc828
    void** vtable;
    uint8_t _04[0x1c - 0x04];
    int32_t engaged_gear;              // +0x1c
    uint8_t _20[4];
    int32_t gear;                      // +0x24 requested
    uint8_t _28[0x4c - 0x28];
    void* output;                      // +0x4c a Differential (TorqueInput: slot 0 GetRPM)
    uint8_t _50[108 - 0x50];
};
static_assert(offsetof(Transmission, engaged_gear) == 0x1c && offsetof(Transmission, output) == 0x4c &&
              sizeof(Transmission) == 108, "Transmission");
struct Wing {                          // 60 bytes, no vtable (phys_engine.cpp)
    uint8_t _00[0x38];
    float angle;                       // +0x38 rad, set here each tick
};
static_assert(sizeof(Wing) == 60, "Wing");

// a sound (Sound3D 64 bytes / SoundDash 60, both over SoundBase): what UpdateCommon writes. The names of the
// flag bytes are this file's guesses from how they're set (an "on" sound gets 0x29 and 0x2b, an "off" one
// 0x2a; 0x2c / 0x2d mark a changed volume / pitch for the mixer).
struct SoundFx {
    uint8_t _00[4];
    float pitch;                       // +0x04
    float volume;                      // +0x08
    uint8_t _0c[0x29 - 0x0c];
    uint8_t on;                        // +0x29
    uint8_t off;                       // +0x2a
    uint8_t keep;                      // +0x2b
    uint8_t volume_changed;            // +0x2c
    uint8_t pitch_changed;             // +0x2d
    uint8_t _2e[64 - 0x2e];
};
static_assert(offsetof(SoundFx, on) == 0x29 && offsetof(SoundFx, pitch_changed) == 0x2d, "SoundFx");

// EngineSound (248 bytes): its samples' Sound3Ds, for the footprint
struct EngineSound {
    uint8_t _00[0xd4];                 // samples[7] at +0x10 (28 bytes each)
    SoundFx* sample_sound[7];          // +0xd4
    SoundFx* idle_sound;               // +0xf0
    int32_t num_samples;               // +0xf4
};
static_assert(offsetof(EngineSound, idle_sound) == 0xf0 && sizeof(EngineSound) == 248, "EngineSound");

// Car (car.obj, 3768 bytes, vtable 0x4dbfd0). Names from out/types.tsv; a field not touched here is padding.
struct Car : PhobDyno {
    void* body_volume;                 // +0x478 SphereGroupVolume*
    uint8_t _47c[0x488 - 0x47c];
    float last_damage_time;            // +0x488
    uint8_t damaged;                   // +0x48c
    uint8_t _48d[0x4f4 - 0x48d];
    int32_t realism;                   // +0x4f4 0 / 1: the yaw-stabilising force; 2: none
    int32_t yaw_control;               // +0x4f8 0 off, 1 gentle (0.5), else 5
    int32_t race_state;                // +0x4fc 0 held on the grid, 2 racing, 3 finished
    float steering;                    // +0x500 -1..1
    uint8_t _504[4];
    float pitch;                       // +0x508 plane
    float opacity;                     // +0x50c
    int32_t car_index;                 // +0x510
    uint8_t _514[0x534 - 0x514];
    uint8_t auto_clutch;               // +0x534
    uint8_t _535[3];
    float drag_long;                   // +0x538 local z
    float drag_lat;                    // +0x53c local x
    float drag_vert;                   // +0x540 local y
    float front_lift;                  // +0x544
    float rear_lift;                   // +0x548
    float front_spoiler_drag;          // +0x54c
    float rear_spoiler_drag;           // +0x550
    Wheel wheels[4];                   // +0x554 0,1 front; 0 and 2 on the -x side
    uint8_t is_plane;                  // +0xbe4
    uint8_t _be5[3];
    void* propeller;                   // +0xbe8
    Wing* wing_left;                   // +0xbec
    Wing* wing_right;                  // +0xbf0
    Wing* elevator;                    // +0xbf4
    Wing* rudder;                      // +0xbf8
    Engine engine;                     // +0xbfc
    Clutch clutch_unit;                // +0xc7c
    Transmission transmission;         // +0xc98
    uint8_t diffs[3 * 32];             // +0xd04 centre, front, rear Differential
    int32_t gear;                      // +0xd64
    int32_t engaged_gear;              // +0xd68
    int32_t shift_sound_gear;          // +0xd6c
    float perceived_rpm;               // +0xd70
    uint8_t _d74[4];
    int32_t heat_step;                 // +0xd78
    P3 front_axle;                     // +0xd7c body frame
    P3 rear_axle;                      // +0xd88
    P3 center_of_pressure;             // +0xd94
    uint8_t _da0[0xdcc - 0xda0];
    float max_steer_angle;             // +0xdcc rad
    float rpm_to_speed;                // +0xdd0
    float front_anti_roll;             // +0xdd4 N/m
    float rear_anti_roll;              // +0xdd8
    float speed;                       // +0xddc m/s (the gearbox output's)
    float throttle;                    // +0xde0
    float braking;                     // +0xde4
    float ebrake;                      // +0xde8
    float clutch;                      // +0xdec
    P3 air_velocity;                   // +0xdf0 body frame
    P3 draft_velocity;                 // +0xdfc world
    P3 draft_point;                    // +0xe08 world
    float draft_radius;                // +0xe14
    uint8_t _e18[0xe28 - 0xe18];
    uint8_t horn;                      // +0xe28
    uint8_t _e29[0xe30 - 0xe29];
    float lap_top_speed;               // +0xe30
    uint8_t _e34[0xe3d - 0xe34];
    uint8_t lowering;                  // +0xe3d
    uint8_t _e3e[2];
    float scrape_energy;               // +0xe40
    float teleport_time;               // +0xe44
    uint8_t _e48;
    uint8_t starter_on;                // +0xe49
    uint8_t _e4a[2];
    SoundFx* start_sound;              // +0xe4c
    EngineSound* engine_sound;         // +0xe50
    SoundFx* shift_sound;              // +0xe54 SoundDash
    SoundFx* road_sound;               // +0xe58
    SoundFx* road_sound_2;             // +0xe5c
    SoundFx* scrape_sound;             // +0xe60
    SoundFx* horn_sound;               // +0xe64
    float horn_volume;                 // +0xe68
    uint8_t _e6c[0xea4 - 0xe6c];
    float lat_g;                       // +0xea4
    float long_g;                      // +0xea8
    float vert_g;                      // +0xeac
    float steer_feedback;              // +0xeb0
    float aero_g;                      // +0xeb4
};
static_assert(offsetof(Car, realism) == 0x4f4 && offsetof(Car, steering) == 0x500 && offsetof(Car, drag_long) == 0x538 &&
              offsetof(Car, wheels) == 0x554 && offsetof(Car, is_plane) == 0xbe4 && offsetof(Car, engine) == 0xbfc &&
              offsetof(Car, clutch_unit) == 0xc7c && offsetof(Car, transmission) == 0xc98 && offsetof(Car, gear) == 0xd64 &&
              offsetof(Car, heat_step) == 0xd78 && offsetof(Car, front_axle) == 0xd7c && offsetof(Car, max_steer_angle) == 0xdcc &&
              offsetof(Car, throttle) == 0xde0 && offsetof(Car, air_velocity) == 0xdf0 && offsetof(Car, draft_radius) == 0xe14 &&
              offsetof(Car, horn) == 0xe28 && offsetof(Car, lap_top_speed) == 0xe30 && offsetof(Car, lowering) == 0xe3d &&
              offsetof(Car, starter_on) == 0xe49 && offsetof(Car, start_sound) == 0xe4c && offsetof(Car, horn_volume) == 0xe68 &&
              offsetof(Car, lat_g) == 0xea4 && offsetof(Car, aero_g) == 0xeb4 && sizeof(Car) == 3768, "Car");

// PhobDyno's acceleration (+0x288, the padding in phys_types.h): what the g-meters read
static __forceinline const P3& acceleration(const Car* c) { return *(const P3*)((const uint8_t*)c + 0x288); }

// RaceDeity (2012 bytes, vtable 0x4dc588): per-car RaceInfo (0x74 bytes) from +0x38, whose CenterLine
// (112 bytes) is at +0x6c; only for the footprint
struct RaceDeity;
static const uint32_t VT_RaceDeity = 0x004dc588;

}  // namespace

// ---- constants, exactly as the original's -------------------------------------------------------------------
static const float k_lower_pull = -0x1.1a872cp+2f;       // 0xc08d4396  -4.4145002 (x mass x vertical speed, while lowering)
static const float k_yaw_gain = 0x1.99999ap-4f;          // 0x3dcccccd  0.1 (realism 1)
static const float k_yaw_gain0 = 0x1.ccccccp-1f;         // 0x3f666666  0.9 (realism 0)
static const float k_point9 = 0x1.ccccccp-1f;            // 0x3f666666
static const float k_1point3 = 0x1.4ccccccp+0f;          // 0x3fa66666  1.3
static const float k_w_scale = 2.5f;
static const float k_w_min1 = 0x1.99999ap-3f;            // 0x3e4ccccd  0.2
static const float k_w_min0 = 0x1.99999ap-2f;            // 0x3ecccccd  0.4
static const float k_ahead = 70.0f;
static const float k_w_damp = -0x1.99999ap-5f;           // 0xbd4ccccd  -0.05
static const float k_v2_lim = 0x1.638e38p+7f;            // 0x4331c71c  177.77777 (13.33 m/s squared)
static const float k_v2_scale = 0x1.70a3d8p-8f;          // 0x3bb851ec  0.005625
static const float k_point99 = 0x1.fae148p-1f;           // 0x3f7d70a4  0.99
static const double k_ahead_dot = 0.98;                  // a double constant (qword)
static const float k_yaw_w = -140.0f;
static const float k_aero_g = -0x1.a1887ap-4f;           // 0xbdd0c43d  -0.10193679 (1 / -9.81)
static const float k_elev = -0x1.f46bbap-4f;             // 0xbdfa35dd  -0.12217305 (-7 deg)
static const float k_ail = 0x1.657184p-4f;               // 0x3db2b8c2  0.08726646 (5 deg)
static const float k_wing0 = 0x1.1df46ap-5f;             // 0x3d0efa35  0.034906585 (2 deg)
static const float k_l2_min = 0x1.0624dep-10f;           // 0x3a83126f  0.001
static const float k_g_scale = 0x1.4e06c8p-7f;           // 0x3c270364  0.010193679 (1 / 9.81)
static const float k_g_keep = 0x1.ccccccp-1f;            // 0x3f666666  0.9
static const float k_yaw_min = 0x1.c71c72p-1f;           // 0x3f638e39  0.8888889 m/s (the yaw control is off below it)
static const float k_road_vol = 0x1.26e978p-11f;         // 0x3a1374bc  0.0005625
static const float k_road_pitch = 0x1.99999ap-6f;        // 0x3ccccccd  0.025
static const float k_horn_up = 0x1.0624dep-1f;           // 0x3f03126f  0.512
static const float k_horn_down = 0x1.0624dep-3f;         // 0x3e03126f  0.128
static const float k_scrape = 0x1.d74526p-13f;           // 0x396ba293  0.0002247191
static const float k_point7 = 0x1.666666p-1f;            // 0x3f333333  0.7
static const float k_steer_step = 0x1.47ae14p-4f;        // 0x3da3d70a  0.08
static const float k_help_force = 40000.0f;
// constants the original stores or compares with integer moves
static const uint32_t k_one_bits = 0x3f800000u;
static const uint32_t k_point9_bits = 0x3f666666u;
static const uint32_t k_point7_bits = 0x3f333333u;
static const uint32_t k_shift_low_bits = 0x3e99999au;    // 0.3
static const uint32_t k_shift_high_bits = 0x3f666667u;   // 0.90000004 (not 0.9f)
static const uint32_t k_help_force_bits = 0x471c4000u;

// ---- the functions these call, by address ----------------------------------------------------------------------
typedef void(__fastcall* Method0_t)(void*, Edx);
typedef void(__fastcall* MethodU32_t)(void*, Edx, uint32_t);           // a float passed as bits
typedef void(__fastcall* MethodU8_t)(void*, Edx, uint8_t);
typedef void(__fastcall* MethodInt_t)(void*, Edx, int);

typedef void(__fastcall* Force_t)(PhobDyno*, Edx, const P3*, const P3*);
typedef void(__fastcall* ArmPos_t)(PhobRoot*, Edx, P3*, const P3*);
typedef void(__cdecl* MulPoint_t)(P3*, const P3*, const M3*);
typedef void(__cdecl* VecAdd_t)(P3*, const P3*, const P3*);
typedef uint8_t(__cdecl* Flag_t)();
typedef double(__cdecl* Time_t)();                                     // returns ST0
typedef int(__cdecl* Int_t)();
typedef uint8_t(__cdecl* OutOfBounds_t)(const P3*);
typedef char*(__cdecl* StatusBuf_t)(int);
typedef void*(__cdecl* GetBall_t)(int);
typedef void(__fastcall* BallThrow_t)(void*, Edx, const Frame*, const P3*, float);
typedef double(__cdecl* TerrainHeight_t)(uint32_t, uint32_t);          // (x, z) as bits; returns ST0

static const Method0_t Car_post_teleport_fadein = (Method0_t)0x00439aa0;
static const Method0_t Car_reset_damage = (Method0_t)0x00439e50;
static const OutOfBounds_t loc_is_out_of_bounds_o = (OutOfBounds_t)0x00438660;   // (this file's rewrite is hooked there)
static const MethodU32_t Car_SetClutch_o = (MethodU32_t)0x004396f0;
static const MethodU32_t Car_SetBraking_o = (MethodU32_t)0x004396e0;
static const MethodU32_t Car_SetThrottle_o = (MethodU32_t)0x004396d0;
static const MethodU32_t Car_UpdateHeat_o = (MethodU32_t)0x00438910;
typedef int(__fastcall* WheelsSkidding_t)(Car*, Edx);
static const WheelsSkidding_t Car_WheelsSkidding_a = (WheelsSkidding_t)0x00439f30;
static const Method0_t Engine_Crank = (Method0_t)0x00446800;
static const MethodU32_t Transmission_SetThrottle = (MethodU32_t)0x004464b0;
static const MethodU32_t Transmission_SetClutch = (MethodU32_t)0x004464a0;
static const MethodU8_t Transmission_Update = (MethodU8_t)0x00446010;
static const MethodInt_t Transmission_SetGear = (MethodInt_t)0x00446000;
static const Method0_t Transmission_SetGearAuto = (Method0_t)0x00446300;
static const MethodU32_t Clutch_SetClutch = (MethodU32_t)0x00445e10;
static const Method0_t Clutch_Update = (Method0_t)0x00445d60;
typedef void(__fastcall* WheelUpdate_t)(Wheel*, Edx, Car*);
static const WheelUpdate_t Wheel_Update = (WheelUpdate_t)0x00448a60;
static const WheelUpdate_t Wheel_UpdateCommon = (WheelUpdate_t)0x0044a630;
static const MethodU32_t Wheel_UpdateHeat = (MethodU32_t)0x0044a440;
static const MethodU32_t Engine_UpdateHeat = (MethodU32_t)0x00446970;
static const Method0_t Wing_Update = (Method0_t)0x00447070;
static const Method0_t PhobDyno_Update = (Method0_t)0x00445210;           // the base's, called non-virtually
static const Force_t PhobDyno_ApplyForce = (Force_t)0x00444c20;
static const ArmPos_t PhobDyno_GetPointVelocity = (ArmPos_t)0x00444ee0;
static const ArmPos_t PhobRoot_GetArmPosition = (ArmPos_t)0x0043e0d0;
static const MulPoint_t MatrixMulPoint = (MulPoint_t)0x00429420;          // o = p x M     (body -> world)
static const MulPoint_t MatrixMulPointInv = (MulPoint_t)0x00436120;       // o = p x M^T   (world -> body)
static const VecAdd_t VectorAdd = (VecAdd_t)0x0043b0f0;
static const Flag_t PhysicsIsDamageOn = (Flag_t)0x0042bd60;
static const Time_t PhysicsGetTime = (Time_t)0x0042bc80;
static const Flag_t HackHornBall = (Flag_t)0x0040d540;
static const Int_t WorldGetFocusCar = (Int_t)0x004626d0;
static const StatusBuf_t CarMgrGetStatusBuf = (StatusBuf_t)0x00464560;
static const MethodU8_t EngineSound_Update = (MethodU8_t)0x00472c60;
static const GetBall_t GetBall = (GetBall_t)0x004410e0;
static const BallThrow_t Ball_Throw = (BallThrow_t)0x004412a0;
static const TerrainHeight_t TerrainGetHeightXZ = (TerrainHeight_t)0x00465ae0;
// globals
#define g_deity (*(void**)0x005218ac)                     // class Deity* deity
#define g_camera_type (*(volatile int32_t*)0x00521600)
#define g_blimp_frame ((const Frame*)0x00521050)
#define g_splash_sound (*(uint8_t**)0x00521f7c)           // the CollideWater splash Sound3D (volume.obj)

// ==== loc_is_out_of_bounds (0x438660): +-5000 m in x and z, -1000..3000 m in y, by the bits ==================
static uint8_t __cdecl loc_is_out_of_bounds(const P3* p) {
    if (Ub(p->x) >= 0xc59c4000u || I(p->x) >= 0x459c4000) return 1;      // x <= -5000 (or a negative NaN), x >= 5000
    if (Ub(p->y) >= 0xc47a0000u || I(p->y) >= 0x453b8000) return 1;      // y <= -1000, y >= 3000
    if (Ub(p->z) >= 0xc59c4000u || I(p->z) >= 0x459c4000) return 1;
    return 0;
}
static void fp_out_of_bounds(Footprint& f, const P3*) { f.pure = true; }
PORT_FN(0x00438660, "loc_is_out_of_bounds", loc_is_out_of_bounds, fp_out_of_bounds)

// ==== Car::WheelsSkidding (0x439f30): bit i set when wheel i's slide > 1.45 ==================================
static int __fastcall Car_WheelsSkidding(Car* self, Edx) {
    int r = I(self->wheels[3].slide) > 0x3fb9999a ? 1 : 0;
    r += r;
    r |= I(self->wheels[2].slide) > 0x3fb9999a ? 1 : 0;
    r += r;
    r |= I(self->wheels[1].slide) > 0x3fb9999a ? 1 : 0;
    r += r;
    r |= I(self->wheels[0].slide) > 0x3fb9999a ? 1 : 0;
    return r;
}
static void fp_wheels_skidding(Footprint& f, Car*, Edx) { f.pure = true; }
PORT_FN(0x00439f30, "Car::WheelsSkidding", Car_WheelsSkidding, fp_wheels_skidding)

// ==== Car::AnyWheelsAreDamaged (0x43b060) ======================================================================
static uint8_t __fastcall Car_AnyWheelsAreDamaged(Car* self, Edx) {
    for (int i = 0; i < 4; i++)
        if (self->wheels[i].broken) return 1;
    return 0;
}
static void fp_any_damaged(Footprint& f, Car*, Edx) { f.pure = true; }
PORT_FN(0x0043b060, "Car::AnyWheelsAreDamaged", Car_AnyWheelsAreDamaged, fp_any_damaged)

// ==== Car::ApplySuspensionForce (0x437660): straight to PhobDyno::ApplyForce ==================================
static void __fastcall Car_ApplySuspensionForce(Car* self, Edx, const P3* force, const P3* point) {
    PhobDyno_ApplyForce(self, 0, force, point);
}
static void fp_susp_force(Footprint& f, Car* self, Edx, const P3*, const P3*) { f.object(self, "car"); }
PORT_FN(0x00437660, "Car::ApplySuspensionForce", Car_ApplySuspensionForce, fp_susp_force)

// ==== the control setters ======================================================================================
static void __fastcall Car_SetTractionControl(Car* self, Edx, int mode) {
    for (int i = 0; i < 4; i++) self->wheels[i].traction_control = mode;
}
static void fp_set_tc(Footprint& f, Car*, Edx, int) { f.pure = true; }
PORT_FN(0x004395e0, "Car::SetTractionControl", Car_SetTractionControl, fp_set_tc)

static void __fastcall Car_SetABSBraking(Car* self, Edx, int mode) {
    for (int i = 0; i < 4; i++) self->wheels[i].abs_mode = mode;
}
static void fp_set_abs(Footprint& f, Car*, Edx, int) { f.pure = true; }
PORT_FN(0x00439600, "Car::SetABSBraking", Car_SetABSBraking, fp_set_abs)

// SetSteering (0x439620): the change is limited to 0.08 a step, the result to -1..1, then the steered pair's
// steer_input is max_steer_angle x steering (a plane steers its rear wheels, the other way)
static void __fastcall Car_SetSteering(Car* self, Edx, float s) {
    const double d = D(s) - self->steering;
    float step = (float)d;                              // [+0]
    if (d > k_steer_step) step = k_steer_step;          // test ah,0x41
    else if (Ub(step) > 0xbda3d70au) step = -k_steer_step;   // below -0.08 (or a negative NaN), by the bits
    const double ns = D(self->steering) + step;
    float nsf = (float)ns;                              // the argument's slot
    if (ns > 1.0f) nsf = Fb(k_one_bits);
    if (Ub(nsf) > 0xbf800000u) nsf = Fb(0xbf800000u);
    const double a = D(self->max_steer_angle) * nsf;
    if (!self->is_plane) {
        self->wheels[0].steer_input = (float)a;
        self->wheels[1].steer_input = (float)a;
    } else {
        self->wheels[2].steer_input = (float)-a;
        self->wheels[3].steer_input = (float)-a;
    }
    COPY4(self->steering, nsf);
}
static void fp_set_steering(Footprint& f, Car*, Edx, float) { f.pure = true; }
PORT_FN(0x00439620, "Car::SetSteering", Car_SetSteering, fp_set_steering)

// the plain stores (each an integer move of the float's bits)
static void __fastcall Car_SetThrottle(Car* self, Edx, uint32_t v) { memcpy(&self->throttle, &v, 4); }
static void fp_set_throttle(Footprint& f, Car*, Edx, uint32_t) { f.pure = true; }
PORT_FN(0x004396d0, "Car::SetThrottle", Car_SetThrottle, fp_set_throttle)
static void __fastcall Car_SetBraking(Car* self, Edx, uint32_t v) { memcpy(&self->braking, &v, 4); }
static void fp_set_braking(Footprint& f, Car*, Edx, uint32_t) { f.pure = true; }
PORT_FN(0x004396e0, "Car::SetBraking", Car_SetBraking, fp_set_braking)
static void __fastcall Car_SetClutch(Car* self, Edx, uint32_t v) { memcpy(&self->clutch, &v, 4); }
static void fp_set_clutch(Footprint& f, Car*, Edx, uint32_t) { f.pure = true; }
PORT_FN(0x004396f0, "Car::SetClutch", Car_SetClutch, fp_set_clutch)
static void __fastcall Car_SetEBrake(Car* self, Edx, uint32_t v) { memcpy(&self->ebrake, &v, 4); }
static void fp_set_ebrake(Footprint& f, Car*, Edx, uint32_t) { f.pure = true; }
PORT_FN(0x00439700, "Car::SetEBrake", Car_SetEBrake, fp_set_ebrake)
static void __fastcall Car_SetPitch(Car* self, Edx, uint32_t v) { memcpy(&self->pitch, &v, 4); }
static void fp_set_pitch(Footprint& f, Car*, Edx, uint32_t) { f.pure = true; }
PORT_FN(0x0043b050, "Car::SetPitch", Car_SetPitch, fp_set_pitch)

// the gear setters go through the Transmission (by address) and mirror its requested gear
static void __fastcall Car_SetGear(Car* self, Edx, int gear) {
    Transmission_SetGear(&self->transmission, 0, gear);
    self->gear = self->transmission.gear;
}
static void fp_set_gear(Footprint& f, Car* self, Edx, int) { f.object(self, "car"); }
PORT_FN(0x00439710, "Car::SetGear", Car_SetGear, fp_set_gear)
static void __fastcall Car_SetGearAuto(Car* self, Edx) {
    Transmission_SetGearAuto(&self->transmission, 0);
    self->gear = self->transmission.gear;
}
static void fp_set_gear_auto(Footprint& f, Car* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00439740, "Car::SetGearAuto", Car_SetGearAuto, fp_set_gear_auto)
static void __fastcall Car_SetGearAutoAI(Car* self, Edx) {          // the same body as SetGearAuto
    Transmission_SetGearAuto(&self->transmission, 0);
    self->gear = self->transmission.gear;
}
static void fp_set_gear_auto_ai(Footprint& f, Car* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00439760, "Car::SetGearAutoAI", Car_SetGearAutoAI, fp_set_gear_auto_ai)

// SetHorn (0x43afb0): with the horn-ball hack on, a rising edge throws the car's ball -- from the car (its
// frame and velocity), or from the blimp's frame at rest when the camera is the blimp's (type 11)
static void __fastcall Car_SetHorn(Car* self, Edx, uint8_t on) {
    const uint8_t hack = HackHornBall();
    if (hack && self->horn != on && on) {
        if (void* ball = GetBall(self->car_index)) {
            if (g_camera_type != 0xb) {
                const float t = (float)PhysicsGetTime();
                Ball_Throw(ball, 0, &self->frame, &self->velocity, t);
            } else {
                P3 zero;                                // [+0xc]
                zero.x = 0.0f; zero.y = 0.0f; zero.z = 0.0f;
                const float t = (float)PhysicsGetTime();
                Ball_Throw(ball, 0, g_blimp_frame, &zero, t);
            }
        }
    }
    self->horn = on;
}
// the car; and the ball it throws (a PhobDyno, sized by its class)
static void fp_set_horn(Footprint& f, Car* self, Edx, uint8_t on) {
    f.object(self, "car");
    if (HackHornBall() && self->horn != on && on)
        if (void* ball = GetBall(self->car_index)) f.object(ball, "ball");
}
PORT_FN(0x0043afb0, "Car::SetHorn", Car_SetHorn, fp_set_horn)

// ==== Car::UpdateHeat (0x438910): the brakes and the engine, every 8th step ===================================
static void __fastcall Car_UpdateHeat(Car* self, Edx, uint32_t dt) {
    for (int i = 0; i < 4; i++) Wheel_UpdateHeat(&self->wheels[i], 0, dt);
    Engine_UpdateHeat(&self->engine, 0, dt);
}
static void fp_update_heat(Footprint& f, Car* self, Edx, uint32_t) { f.object(self, "car"); }
PORT_FN(0x00438910, "Car::UpdateHeat", Car_UpdateHeat, fp_update_heat)

// ==== Car::UpdateDraft (0x438830): the other car's draft, if we're within its draft radius of its point ========
static void __fastcall Car_UpdateDraft(Car* self, Edx, Car* other) {
    if (!VFN(other, 0x30, uint8_t)(other, 0)) return;   // IsSolid
    P3 d;                                               // [+8]
    d.x = (float)(D(other->draft_point.x) - self->frame.pos.x);
    d.y = (float)(D(other->draft_point.y) - self->frame.pos.y);
    d.z = (float)(D(other->draft_point.z) - self->frame.pos.z);
    const float d2 = (float)((D(d.y) * d.y + D(d.z) * d.z) + D(d.x) * d.x);   // [+8]
    const double r2 = D(other->draft_radius) * other->draft_radius;
    const float r2f = (float)r2;                        // [+0x14]
    if (!(r2 > d2)) return;                             // test ah,0x41
    const double k = (D(r2f) - d2) / r2f;
    self->draft_velocity.x = (float)(D(other->velocity.x) * k + self->draft_velocity.x);
    self->draft_velocity.y = (float)(D(other->velocity.y) * k + self->draft_velocity.y);
    self->draft_velocity.z = (float)(k * other->velocity.z + self->draft_velocity.z);
}
static void fp_update_draft(Footprint& f, Car* self, Edx, Car* other) {
    f.object(self, "car");
    f.object(other, "other car");                       // only read; listed as the caller expects
}
PORT_FN(0x00438830, "Car::UpdateDraft", Car_UpdateDraft, fp_update_draft)

// ==== Car::apply_yaw_control (0x4386a0) ========================================================================
// tan(steer) / wheelbase: fptan's full-mantissa tangent divided in the same asm block (the quotient IS
// rounded to 24 bits, so a double holds it exactly). For |steer| >= 2^63 fptan pushes nothing: the original
// then pops one register more than it pushed and its result comes from an x87 stack underflow -- stale
// register contents, not reproducible; unreachable (steering is clamped to +-1, max_steer_angle < 1 rad).
static __forceinline double yaw_tan_ratio(double steer, double wheelbase) {
    double r;
    __asm { fld steer
            fptan
            fstp st(0)
            fld wheelbase
            fdivp st(1), st(0)
            fstp r }
    return r;
}
// Brakes single wheels toward the bicycle model's yaw rate (v tan(steer) / wheelbase), once the car moves
// faster than 0.889 m/s: too little yaw brakes an inside wheel, too much an outside one. brakes[] is the
// front-left, front-right, rear-left, rear-right brake the caller then hands to the wheels.
static void __fastcall Car_apply_yaw_control(Car* self, Edx, float* const brakes) {
    const int mode = self->yaw_control;
    if (mode == 0) return;
    const double speed = x87_sqrt((D(self->velocity.y) * self->velocity.y + D(self->velocity.z) * self->velocity.z) +
                                  D(self->velocity.x) * self->velocity.x);
    float wy;                                           // [+0xc] the yaw rate, an integer copy
    COPY4(wy, self->angular_velocity.y);
    const double steer = D(self->max_steer_angle) * self->steering;
    const double wheelbase = D(self->front_axle.z) - self->rear_axle.z;
    const float target = (float)(yaw_tan_ratio(steer, wheelbase) * speed);   // [+0]
    // (fstp pops the target; the fcomp that follows tests what's left on the stack: the speed)
    if (!(speed > k_yaw_min)) return;                   // test ah,0x41
    float g_in, g_out;                                  // [+4], [+8]
    if (mode == 1) { g_in = 0.5f; g_out = 5.0f; }
    else { g_in = 5.0f; g_out = 5.0f; }
    const float d = (float)(D(target) - wy);            // [+0] how much yaw is missing
    if (I(wy) > 0) {                                    // turning left (the bits: positive, not zero)
        if (I(d) > 0) {                                 // not enough: brake the rear-right... (wheel 3)
            const double v = D(d) * g_in;
            brakes[3] = (float)(!(D(brakes[3]) >= v) ? v : D(brakes[3]));   // max; a NaN takes v
        } else {                                        // too much: the front-left (wheel 0)
            const double v = -(D(d) * g_out);
            brakes[0] = (float)(!(D(brakes[0]) >= v) ? v : D(brakes[0]));
        }
    } else {
        const double e_r = -D(d);
        const float e = (float)e_r;
        if (!(e_r > 0.0f)) {                            // test ah,0x41: e <= 0 or unordered -> wheel 1
            const double v = -(D(e) * g_out);
            brakes[1] = (float)(!(D(brakes[1]) >= v) ? v : D(brakes[1]));
        } else {                                        // wheel 2
            const double v = D(e) * g_in;
            brakes[2] = (float)(!(D(brakes[2]) >= v) ? v : D(brakes[2]));
        }
    }
}
// (not marked pure: the fuzzer's random cars reach the fptan range case above, where the original's result
// isn't a function of its inputs; test/world_car.cpp covers it with extreme values short of that)
static void fp_yaw_control(Footprint& f, Car* self, Edx, float* const brakes) {
    f.object(self, "car");                              // read only; its this
    f.add(brakes, 16, "brakes");
}
PORT_FN(0x004386a0, "Car::apply_yaw_control", Car_apply_yaw_control, fp_yaw_control)

// ==== Car::Update (0x437680) ===================================================================================
static void __fastcall Car_Update(Car* self, Edx) {
    const float* M = self->frame.rot.m;                 // rows: 0 right, 1 up, 2 forward
    const P3& pos = self->frame.pos;                    // ebp
    const P3& vel = self->velocity;

    // ---- held on the grid: clutch out, brakes on, no horizontal velocity
    if (self->race_state == 0) {
        Car_SetClutch_o(self, 0, 0);
        Car_SetBraking_o(self, 0, k_one_bits);
        self->velocity.x = 0.0f;
        self->velocity.z = 0.0f;
    }

    // ---- after a reset: lowered on to its wheels; done once three touch; meanwhile a damped drop
    if (self->lowering) {
        const int n = (int)self->wheels[1].on_ground + self->wheels[3].on_ground + self->wheels[0].on_ground +
                      self->wheels[2].on_ground;
        if (n >= 3) VFN(self, 0x50, void)(self, 0);     // DoneLowering
        P3 above;                                       // [+0x48] 2 m above the centre, body space
        above.x = 0.0f; above.y = 2.0f; above.z = 0.0f;
        P3 at;                                          // [+0x1c]
        PhobRoot_GetArmPosition(self, 0, &at, &above);
        P3 v;                                           // [+0x10]
        PhobDyno_GetPointVelocity(self, 0, &v, &at);
        P3 F;                                           // [+0x3c]
        F.x = 0.0f;
        F.y = (float)((D(self->mass) * v.y) * k_lower_pull);
        F.z = 0.0f;
        PhobDyno_ApplyForce(self, 0, &F, &at);
    }

    Car_post_teleport_fadein(self, 0);
    if (loc_is_out_of_bounds_o(&pos)) {
        void* deity = g_deity;
        VFN(deity, 0x3c, uint8_t, int, Car*)(deity, 0, self->car_index, self);   // TeleportToLine
    }
    self->steer_feedback = 0.0f;

    // ---- the starter: a stalled engine with the clutch out cranks; cranking means full throttle
    if (self->engine.stalled && I(self->clutch_unit.engagement) < 0x3c23d70a) Engine_Crank(&self->engine, 0);
    if (self->engine.cranking) Car_SetThrottle_o(self, 0, k_one_bits);

    // ---- the drivetrain
    Transmission_SetThrottle(&self->transmission, 0, Ub(self->throttle));
    Transmission_SetClutch(&self->transmission, 0, Ub(self->clutch));
    Transmission_Update(&self->transmission, 0, self->auto_clutch);
    void* out = self->transmission.output;
    self->speed = (float)(VFN(out, 0, double)(out, 0) * self->rpm_to_speed);
    const uint8_t plane = self->is_plane;
    self->starter_on = self->engine.cranking;
    if (plane && I(self->perceived_rpm) > 0x44fa0000) Clutch_SetClutch(&self->clutch_unit, 0, k_one_bits);   // above 2000 rpm
    Clutch_Update(&self->clutch_unit, 0);

    // ---- brakes: the fronts get the pedal, the rears the greater of pedal and e-brake; then the yaw control
    // has its say; an e-brake past 0.1 locks the rears (a negative input)
    const double rear = !(D(self->ebrake) >= self->braking) ? D(self->braking) : D(self->ebrake);   // test ah,1
    const float rearf = (float)rear;                    // [+0x10]
    float brakes[4];                                    // [+0x78]
    COPY4(brakes[0], self->braking);
    COPY4(brakes[1], self->braking);
    COPY4(brakes[2], rearf);
    COPY4(brakes[3], rearf);
    VFN(self, 0x4c, void, float*)(self, 0, brakes);     // apply_yaw_control
    if (I(self->ebrake) > 0x3dcccccd) {
        brakes[2] = (float)-D(brakes[2]);
        brakes[3] = (float)-D(brakes[3]);
    }
    for (int i = 0; i < 4; i++) COPY4(self->wheels[i].brake_input, brakes[i]);

    // ---- the wheels
    for (int i = 0; i < 4; i++) Wheel_Update(&self->wheels[i], 0, self);

    // ---- heat, every 8th step (0.128 s)
    if (++self->heat_step >= 8) {
        self->heat_step = 0;
        Car_UpdateHeat_o(self, 0, 0x3e03126fu);
    }
    self->perceived_rpm = (float)VFN(&self->engine, 0, double)(&self->engine, 0);   // Engine's GetRPM
    self->steer_feedback = (float)(D(self->wheels[0].aligning_torque) + self->wheels[1].aligning_torque);
    self->gear = self->transmission.gear;
    self->engaged_gear = self->transmission.engaged_gear;

    // ---- the yaw-stabilising force: applied 70 m ahead of the car, along its right axis, against the yaw
    // rate and with the steering; only with three wheels down, both fronts whole, and moving forward
    const int realism = self->realism;
    if (realism == 0 || realism == 1) {
        const P3& av = self->angular_velocity;
        float w_up = (float)((D(av.x) * M[3] + D(av.z) * M[5]) + D(av.y) * M[4]);   // [+0x48] / [+0x34]
        P3 P;                                           // [+0x10] the point ahead
        P3 F;                                           // [+0x1c] the force
        if (realism == 1) {
            float gain = 2.0f;                          // [+0x3c]
            const double v2 = (D(vel.y) * vel.y + D(vel.x) * vel.x) + D(vel.z) * vel.z;
            const float v2f = (float)v2;                // [+0x10]
            if (!(v2 >= 1.0f)) gain = (float)(D(v2f) * 2.0f);   // test ah,1
            if (I(self->wheels[0].slide) > 0x3dcccccd) gain = (float)(D(gain) * k_point9);
            if (I(self->wheels[1].slide) > 0x3dcccccd) gain = (float)(D(gain) * k_point9);
            if (I(self->wheels[2].slide) > 0x3dcccccd) gain = (float)(D(gain) * k_1point3);
            if (I(self->wheels[3].slide) > 0x3dcccccd) gain = (float)(D(gain) * k_1point3);
            double r = fabs(D(w_up)) * k_w_scale;
            r = !(k_w_min1 >= r) ? r : D(k_w_min1);     // max(r, 0.2); a NaN stays
            r = 1.0f > r ? r : 1.0;                     // min(r, 1); a NaN gives 1
            const double dist = r * k_ahead;
            const float distf = (float)dist;            // [+0x1c]
            P.x = (float)(dist * M[6] + pos.x);
            P.y = (float)(D(M[7]) * distf + pos.y);
            P.z = (float)(D(M[8]) * distf + pos.z);
            P3 V;                                       // [+0x1c] the velocity, integer copies
            memcpy(&V, &vel, 12);
            // (the original compares |V|^2 with 1 here and never uses the result)
            (void)((D(V.y) * V.y + D(V.z) * V.z) + D(V.x) * V.x);
            const float g = (float)(((D(self->max_steer_angle) * self->steering) * gain) * k_yaw_gain);   // [+0x1c]
            const double w2 = D(w_up) * k_w_damp + g;
            w_up = (float)w2;                           // [+0x48]
            F.x = (float)((w2 * M[0]) * 1000.0f);
            F.y = (float)((D(M[1]) * w2) * 1000.0f);
            F.z = (float)((D(M[2]) * w2) * 1000.0f);
        } else {
            float gain = 1.0f;                          // [+0x3c]
            if (I(self->wheels[0].slide) > 0x3dcccccd) gain = Fb(k_point9_bits);
            if (I(self->wheels[1].slide) > 0x3dcccccd) gain = (float)(D(gain) * k_point9);
            if (I(self->wheels[2].slide) > 0x3dcccccd) w_up = (float)(D(w_up) * k_1point3);
            if (I(self->wheels[3].slide) > 0x3dcccccd) w_up = (float)(D(w_up) * k_1point3);
            double r = fabs(D(w_up)) * k_w_scale;
            r = !(k_w_min0 >= r) ? r : D(k_w_min0);     // max(r, 0.4)
            r = 1.0f > r ? r : 1.0;                     // min(r, 1)
            float dist = (float)r;                      // [+0x38]
            const double v2 = (D(vel.y) * vel.y + D(vel.x) * vel.x) + D(vel.z) * vel.z;
            const float v2f = (float)v2;                // [+0x10]
            if (!(v2 >= k_v2_lim)) {                    // below 13.3 m/s both fade with the speed squared
                const double q = D(v2f) * k_v2_scale;
                dist = (float)(D(dist) * q);
                gain = (float)(q * gain);
            }
            P.x = (float)(D(M[6]) * k_ahead + pos.x);
            P.y = (float)(D(M[7]) * k_ahead + pos.y);
            P.z = (float)(D(M[8]) * k_ahead + pos.z);
            P3 V;                                       // [+0x1c] the velocity's direction
            memcpy(&V, &vel, 12);
            const double v2c = (D(V.z) * V.z + D(V.y) * V.y) + D(V.x) * V.x;
            const float v2cf = (float)v2c;              // [+0x48]
            if (v2c > 1.0f) {                           // test ah,0x41
                const double k = 1.0f / x87_sqrt(D(v2cf));
                V.x = (float)(D(V.x) * k);
                V.y = (float)(D(V.y) * k);
                V.z = (float)(k * V.z);
            }
            const float g = (float)(((D(self->max_steer_angle) * self->steering) * gain) * k_yaw_gain0);   // [+0x48]
            dist = (float)(D(dist) * k_point99);
            const double dot = (D(M[8]) * V.z + D(M[6]) * V.x) + D(M[7]) * V.y;   // how straight it's going
            float gf = g;
            if (!(dot >= k_ahead_dot)) {                // test ah,1: sliding (or a NaN): less of both
                w_up = (float)(D(w_up) * k_w_min1);
                gf = (float)(D(g) * 0.5f);
            }
            const double s = D(w_up) * k_yaw_w + D(gf) * 1000.0f;
            F.x = (float)((D(M[0]) * dist) * s);
            F.y = (float)((D(M[1]) * dist) * s);
            F.z = (float)((D(M[2]) * dist) * s);
        }
        const int n = (int)self->wheels[1].on_ground + self->wheels[3].on_ground + self->wheels[0].on_ground +
                      self->wheels[2].on_ground;
        bool ok = n > 2;
        if (self->wheels[0].broken != 0 || self->wheels[1].broken != 0) ok = false;
        if (ok) {
            const double fwd = (D(M[6]) * vel.x + D(vel.y) * M[7]) + D(M[8]) * vel.z;
            if (fwd > 0.1f) PhobDyno_ApplyForce(self, 0, &F, &P);   // test ah,0x41
        }
    }

    // ---- anti-roll bars: each pair's compression difference, as a force on each wheel, while one is down
    {
        float a = (float)((D(self->wheels[0].compression) - self->wheels[1].compression) * self->front_anti_roll);   // [+0x10]
        if (!self->wheels[0].on_ground && !self->wheels[1].on_ground) a = 0.0f;
        self->wheels[1].anti_roll_force = (float)-D(a);
        COPY4(self->wheels[0].anti_roll_force, a);
        float b = (float)((D(self->wheels[2].compression) - self->wheels[3].compression) * self->rear_anti_roll);
        if (!self->wheels[2].on_ground && !self->wheels[3].on_ground) b = 0.0f;
        self->wheels[3].anti_roll_force = (float)-D(b);
        COPY4(self->wheels[2].anti_roll_force, b);
    }

    // ---- the air: the velocity less the draft, in the body frame; body drag at the centre of pressure
    P3 R;                                               // [+0x54] air velocity, world
    R.x = (float)(D(vel.x) - self->draft_velocity.x);
    R.y = (float)(D(vel.y) - self->draft_velocity.y);
    R.z = (float)(D(vel.z) - self->draft_velocity.z);
    P3 V0;                                              // [+0x10] the velocity, integer copies
    memcpy(&V0, &vel, 12);
    P3 cp;                                              // [+0x88] the centre of pressure, world
    MatrixMulPoint(&cp, &self->center_of_pressure, &self->frame.rot);
    cp.x = (float)(D(pos.x) + cp.x);
    cp.y = (float)(D(pos.y) + cp.y);
    cp.z = (float)(D(pos.z) + cp.z);
    MatrixMulPointInv(&self->air_velocity, &R, &self->frame.rot);
    {
        const int n = (int)self->wheels[1].on_ground + self->wheels[3].on_ground + self->wheels[0].on_ground +
                      self->wheels[2].on_ground;
        P3 Fd;                                          // [+0x94] the drag force, world
        if (n < 2) {
            // airborne: the drag ellipsoid's magnitude along the air velocity, against it
            const double vy = (D(M[4]) * R.y + D(M[5]) * R.z) + D(M[3]) * R.x;
            const double vz = (D(M[8]) * R.z + D(M[6]) * R.x) + D(M[7]) * R.y;
            const double vx = (D(M[1]) * R.y + D(M[2]) * R.z) + D(M[0]) * R.x;
            const double t1 = ((D(self->drag_vert) * self->drag_vert) * vy) * vy;
            const double t2 = ((D(self->drag_long) * self->drag_long) * vz) * vz;
            const double t3 = ((D(self->drag_lat) * self->drag_lat) * vx) * vx;
            const double mag = x87_sqrt((t1 + t2) + t3);
            Fd.x = (float)-(D(R.x) * mag);
            Fd.y = (float)-(D(R.y) * mag);
            Fd.z = (float)-(mag * R.z);
        } else {
            // on the ground: |v| v per axis in the body frame, turned to the world
            const P3& a = self->air_velocity;
            P3 G;                                       // [+0x94] body
            G.z = (float)-((fabs(D(a.z)) * a.z) * self->drag_long);
            G.y = (float)-((fabs(D(a.y)) * a.y) * self->drag_vert);
            G.x = (float)-((fabs(D(a.x)) * a.x) * self->drag_lat);
            MatrixMulPoint(&Fd, &G, &self->frame.rot); // [+0xac]
        }
        PhobDyno_ApplyForce(self, 0, &Fd, &cp);
    }

    // ---- lift (with the forward speed squared) and spoiler drag (|v| v) at each axle; the aero g
    {
        const double fwd = (D(M[8]) * V0.z + D(M[6]) * V0.x) + D(M[7]) * V0.y;
        self->aero_g = 0.0f;
        const float spd = (float)x87_sqrt((D(V0.y) * V0.y + D(V0.z) * V0.z) + D(V0.x) * V0.x);   // [+0x48]
        // (the original tests fwd < 0 into al here and never uses it)
        const double fwd2 = fwd * fwd;
        const float fwd2f = (float)fwd2;                // [+0x1c]
        double lift = fwd2 * self->front_lift;
        double sd = -(D(self->front_spoiler_drag) * spd);
        P3 A;                                           // [+0x60] the force
        A.x = (float)(D(M[3]) * lift + D(V0.x) * sd);
        A.y = (float)(D(M[4]) * lift + D(V0.y) * sd);
        A.z = (float)(lift * M[5] + sd * V0.z);
        P3 at;                                          // [+0xa0] the axle, world
        MatrixMulPoint(&at, &self->front_axle, &self->frame.rot);
        VectorAdd(&at, &at, &pos);
        PhobDyno_ApplyForce(self, 0, &A, &at);
        self->aero_g = (float)((D(self->inv_mass) * A.y) * k_aero_g + self->aero_g);
        lift = D(self->rear_lift) * fwd2f;
        sd = -(D(self->rear_spoiler_drag) * spd);
        A.x = (float)(D(M[3]) * lift + D(V0.x) * sd);
        A.y = (float)(D(M[4]) * lift + D(V0.y) * sd);
        A.z = (float)(lift * M[5] + sd * V0.z);
        MatrixMulPoint(&at, &self->rear_axle, &self->frame.rot);
        VectorAdd(&at, &at, &pos);
        PhobDyno_ApplyForce(self, 0, &A, &at);
        self->aero_g = (float)((D(self->inv_mass) * A.y) * k_aero_g + self->aero_g);
    }

    // ---- a plane's control surfaces: ailerons 2 deg +- 5 deg x steering, the elevator -7 deg x pitch
    if (plane) {
        const double e = D(self->pitch) * k_elev;
        const double s = D(self->steering) * k_ail;
        self->wing_left->angle = (float)(D(k_wing0) + s);
        self->wing_right->angle = (float)(D(k_wing0) - s);
        self->elevator->angle = (float)e;
        self->rudder->angle = 0.0f;
        Wing_Update(self->wing_left, 0);
        Wing_Update(self->wing_right, 0);
        Wing_Update(self->elevator, 0);
        Wing_Update(self->rudder, 0);
    }

    // ---- integrate
    PhobDyno_Update(self, 0);

    // ---- the draft point: half a draft radius behind the rear axle; the draft velocity is spent
    self->draft_velocity.x = 0.0f;
    self->draft_velocity.y = 0.0f;
    self->draft_velocity.z = 0.0f;
    MatrixMulPoint(&self->draft_point, &self->rear_axle, &self->frame.rot);
    VectorAdd(&self->draft_point, &self->draft_point, &pos);
    {
        const double h = D(self->draft_radius) * -0.5f;
        self->draft_point.x = (float)(D(M[6]) * h + self->draft_point.x);
        self->draft_point.y = (float)(D(M[7]) * h + self->draft_point.y);
        self->draft_point.z = (float)(h * M[8] + self->draft_point.z);
    }
    // (the compiler's alias checks between its own locals are left out)

    // ---- the g-meters: the acceleration on the horizontal right and forward axes and the body's up, in g,
    // low-passed 0.9
    {
        P3 L;                                           // [+0x28] horizontal right: (fwd.z, 0, -fwd.x)
        COPY4(L.x, M[8]);
        L.y = 0.0f;
        L.z = (float)-D(M[6]);
        const double l2 = (D(L.y) * L.y + D(L.z) * L.z) + D(L.x) * L.x;
        const float l2f = (float)l2;                    // [+0x10]
        if (l2 > k_l2_min) {                            // test ah,0x41
            const double k = 1.0f / x87_sqrt(D(l2f));
            L.x = (float)(D(L.x) * k);
            L.y = (float)(D(L.y) * k);
            L.z = (float)(k * L.z);
            P3 Fh;                                      // [+0x6c] horizontal forward
            Fh.x = (float)-D(L.z);
            Fh.y = 0.0f;
            COPY4(Fh.z, L.x);
            const P3& acc = acceleration(self);
            self->lat_g = (float)(((D(acc.y) * L.y + D(acc.z) * L.z) + D(acc.x) * L.x) * k_g_scale + D(self->lat_g) * k_g_keep);
            self->long_g = (float)(((D(acc.y) * Fh.y + D(acc.x) * Fh.x) + D(acc.z) * L.x) * k_g_scale + D(self->long_g) * k_g_keep);
            self->vert_g = (float)(((D(acc.z) * M[5] + D(acc.y) * M[4]) + D(acc.x) * M[3]) * k_g_scale + D(self->vert_g) * k_g_keep);
        }
    }

    // ---- the lap's top speed: all four wheels down and none skidding
    {
        const int n = (int)self->wheels[1].on_ground + self->wheels[3].on_ground + self->wheels[0].on_ground +
                      self->wheels[2].on_ground;
        if (n == 4 && !Car_WheelsSkidding_a(self, 0)) {
            const double sp = x87_sqrt((D(vel.y) * vel.y + D(vel.x) * vel.x) + D(vel.z) * vel.z);
            const double lts = self->lap_top_speed;
            self->lap_top_speed = (float)(!(lts >= sp) ? sp : lts);   // test ah,1: max, a NaN takes the speed
        }
    }

    // ---- with damage off, dents heal 2 s after they're made
    if (!PhysicsIsDamageOn() && self->damaged) {
        const double t = PhysicsGetTime();
        if (!((D(self->last_damage_time) + 2.0f) >= t)) Car_reset_damage(self, 0);   // fcompp; test ah,1
    }
    void* deity = g_deity;
    VFN(deity, 0x38, void, int, Car*)(deity, 0, self->car_index, self);   // UpdateCar
}
// It writes the car and, through the callees: the splash Sound3D CollideWater may play (PhobDyno::Update, a
// volume.obj global), a plane's four wings, and the deity's per-car race record with its CenterLine
// (RaceDeity::UpdateCar). Left to the replay: an out-of-bounds teleport (Car::Teleport and the deity's
// TeleportToLine), the damage reset (rebuilds the models), a deity that isn't a RaceDeity, and the lap events
// RaceDeity::UpdateCar raises (GhostNewBestLap, RecordRaceOver: the ghost and record globals) plus the
// CenterLine's checkpoint-time array (+0x60) -- neither can be told in advance, and they lie outside this
// footprint (see the report). Random and the controls are inputs, fed automatically.
static void fp_car_update(Footprint& f, Car* self, Edx) {
    f.object(self, "car");
    if (uint8_t* snd = g_splash_sound) f.add(snd + 8, 0x25, "splash Sound3D");
    if (self->is_plane) {
        f.add(self->wing_left, sizeof(Wing), "wing_left");
        f.add(self->wing_right, sizeof(Wing), "wing_right");
        f.add(self->elevator, sizeof(Wing), "elevator");
        f.add(self->rudder, sizeof(Wing), "rudder");
    }
    uint8_t* deity = (uint8_t*)g_deity;
    if (!deity || *(uint32_t*)deity != VT_RaceDeity) { f.replay_only = "the deity isn't a RaceDeity (size unknown)"; return; }
    f.add(deity, 2012, "deity");
    if ((uint32_t)self->car_index < 16u) {
        uint8_t* info = deity + 0x38 + 0x74 * self->car_index;
        if (uint8_t* line = *(uint8_t**)(info + 0x6c)) f.add(line, 112, "centre line");
    }
    if (loc_is_out_of_bounds_o(&self->frame.pos)) { f.replay_only = "out of bounds: teleports through the deity"; return; }
    if (!PhysicsIsDamageOn() && self->damaged && !((D(self->last_damage_time) + 2.0f) >= PhysicsGetTime()))
        f.replay_only = "reset_damage rebuilds the models";
}
PORT_FN(0x00437680, "Car::Update", Car_Update, fp_car_update)

// ==== Car::UpdateCommon (0x4391b0): the sounds and the status string, each frame ===============================
static void __fastcall Car_UpdateCommon(Car* self, Edx) {
    if (self->engine_sound) EngineSound_Update(self->engine_sound, 0, self->engine.stalled);

    // ---- the starter motor
    if (SoundFx* s = self->start_sound) {
        if (self->starter_on) {
            if (I(s->volume) != 0x3f333333) { s->volume_changed = 1; s->volume = Fb(k_point7_bits); }
            s->on = 1;
            s->keep = 0;
            s->off = 0;
        } else s->off = 1;
    }

    // ---- road noise: road2 off-road (two wheels or more), else road1; only the focus car's is heard, its
    // volume from the airspeed squared, road1's pitch from the airspeed
    int off_road = 0;
    for (int i = 0; i < 4; i++)
        if (self->wheels[i].fx_flags & 0x20) off_road++;
    const P3& a = self->air_velocity;
    if (off_road >= 2 && self->road_sound_2) {
        if (SoundFx* s = self->road_sound) s->off = 1;
        if (self->road_sound_2) {
            if (WorldGetFocusCar() == self->car_index) {
                SoundFx* s = self->road_sound_2;
                const float share = (float)(D(off_road) * 0.25f);   // [+0xc]
                if (I(s->pitch) != (int32_t)k_one_bits) { s->pitch_changed = 1; s->pitch = Fb(k_one_bits); }
                const double q = ((D(a.x) * a.x + D(a.z) * a.z) + D(a.y) * a.y) * k_road_vol;
                const double m = 1.0f > q ? q : 1.0;    // test ah,0x41: min, a NaN gives 1
                const double v = m * share;
                const float vf = (float)v;              // [+0xc]
                if (v < s->volume || v > s->volume) { s->volume_changed = 1; COPY4(s->volume, vf); }   // test ah,0x40
                s->off = 0;
                s->keep = 1;
                s->on = 1;
            } else self->road_sound_2->off = 1;
        }
    } else {
        if (SoundFx* s = self->road_sound_2) s->off = 1;
        if (self->road_sound) {
            if (WorldGetFocusCar() == self->car_index) {
                SoundFx* s = self->road_sound;
                const double p = x87_sqrt((D(a.x) * a.x + D(a.z) * a.z) + D(a.y) * a.y) * k_road_pitch;
                const double pm = !(D(k_point9) >= p) ? p : D(k_point9);   // test ah,1: max, a NaN stays
                const float pmf = (float)pm;            // [+0xc]
                if (pm < s->pitch || pm > s->pitch) { s->pitch_changed = 1; COPY4(s->pitch, pmf); }
                const double q = ((D(a.x) * a.x + D(a.z) * a.z) + D(a.y) * a.y) * k_road_vol;
                const double m = 1.0f > q ? q : 1.0;
                const float mf = (float)m;
                if (m < s->volume || m > s->volume) { s->volume_changed = 1; COPY4(s->volume, mf); }
                s->off = 0;
                s->keep = 1;
                s->on = 1;
            } else self->road_sound->off = 1;
        }
    }

    // ---- the horn: its volume ramps up 0.512 a frame, down 0.128
    if (SoundFx* s = self->horn_sound) {
        double v;
        if (self->horn) {
            v = D(self->horn_volume) + k_horn_up;
            v = 1.0f > v ? v : 1.0;                     // test ah,0x41: min
        } else {
            v = D(self->horn_volume) - k_horn_down;
            v = !(0.0f >= v) ? v : 0.0;                 // test ah,1: max
        }
        self->horn_volume = (float)v;
        if (I(self->horn_volume) > 0x3d4ccccd) {
            const double hv = self->horn_volume;
            if (hv < s->volume || hv > s->volume) { s->volume_changed = 1; COPY4(s->volume, self->horn_volume); }
            s->off = 0;
            s->keep = 1;
            s->on = 1;
        } else s->off = 1;
    }

    // ---- scraping: the volume from the root of the energy the collisions summed, which is spent
    if (I(self->scrape_energy) > 0x3c23d70a) {
        if (SoundFx* s = self->scrape_sound) {
            const double r = x87_sqrt(D(self->scrape_energy)) * k_scrape;
            const double m = 1.0f > r ? r : 1.0;        // test ah,0x41: min
            const double v = m * k_point7;
            const float vf = (float)v;                  // [+0xc]
            if (v < s->volume || v > s->volume) { s->volume_changed = 1; COPY4(s->volume, vf); }
            s->off = 0;
            s->keep = 1;
            s->on = 1;
        }
        self->scrape_energy = 0.0f;
    } else if (SoundFx* s = self->scrape_sound) s->off = 1;

    // ---- the tyres
    for (int i = 0; i < 4; i++) Wheel_UpdateCommon(&self->wheels[i], 0, self);

    // ---- a gear change: the shift sound, quieter into neutral
    const int eg = self->engaged_gear;
    if (self->shift_sound_gear != eg) {
        SoundFx* s = self->shift_sound;
        self->shift_sound_gear = eg;
        if (s) {
            const float v = Fb(eg == 0 ? k_shift_low_bits : k_shift_high_bits);
            if (D(v) < s->volume || D(v) > s->volume) { s->volume_changed = 1; COPY4(s->volume, v); }
            s->on = 1;
            s->keep = 0;
            s->off = 0;
        }
    }

    // ---- the status string, for the car manager
    if (self->car_index >= 0) {
        char* buf = CarMgrGetStatusBuf(self->car_index);
        VFN(self, 0x24, void, char*)(self, 0, buf);     // MakeStatusString
    }
}
// The car; the sounds it drives (their pitch, volume and flags: +4..+0x2d), the EngineSound and its samples'
// Sound3Ds, the wheels' tyre sounds (as phys_wheel.cpp lists them), and the car's status string in the car
// manager's static table (368 bytes each at 0x5540c2) -- listed rather than relying on the thread's statics,
// as this runs from the frame side.
static void fp_update_common(Footprint& f, Car* self, Edx) {
    f.object(self, "car");
    if (EngineSound* es = self->engine_sound) {
        f.add(es, sizeof(EngineSound), "engine sound");
        for (int i = 0; i < es->num_samples && i < 7; i++)
            if (SoundFx* s = es->sample_sound[i]) f.add((uint8_t*)s + 4, 0x2a, "engine sample Sound3D");
        if (SoundFx* s = es->idle_sound) f.add((uint8_t*)s + 4, 0x2a, "engine idle Sound3D");
    }
    SoundFx* sounds[6] = {self->start_sound, self->shift_sound, self->road_sound, self->road_sound_2,
                          self->scrape_sound, self->horn_sound};
    for (SoundFx* s : sounds)
        if (s) f.add((uint8_t*)s + 4, 0x2a, "Sound3D");
    for (int i = 0; i < 4; i++) {
        uint8_t* ts = (uint8_t*)self->wheels[i].tire_sound;
        if (!ts) continue;
        const uint32_t vt = *(const uint32_t*)ts;
        if (vt == 0x004dd7b0) {                         // RealTireSound: itself and its Sound3D (+0x34)
            f.add(ts, 0x3c, "tire sound");
            if (uint8_t* s3d = *(uint8_t**)(ts + 0x34)) f.add(s3d + 4, 0x2a, "tire Sound3D");
        } else if (vt == 0x004dd7a0) f.add(ts, 0x10, "tire sound");   // DummyTireSound
    }
    if (self->car_index >= 0) f.add((uint8_t*)0x005540c2 + 368 * self->car_index, 368, "status string");
}
PORT_FN(0x004391b0, "Car::UpdateCommon", Car_UpdateCommon, fp_update_common)

// ==== Car::HelpOut (0x439780): a shove for a stuck car ========================================================
// Upside down (up.y < -0.8): 40 kN along the right axis 1 m above the centre, and back the other way 3 m
// lower -- a flip. On its side (up.y < 0.2): 40 kN along the negated up axis 1 m above. Else 40 kN straight
// up, 3 m ahead. Not while lowering, held on the grid, or finished.
static void __fastcall Car_HelpOut(Car* self, Edx) {
    if (self->lowering) return;
    const int rs = self->race_state;
    if (rs == 0 || rs == 3) return;
    const float* M = self->frame.rot.m;
    P3 P;                                               // [+0x10] the point, integer copies of the position
    memcpy(&P, &self->frame.pos, 12);
    P3 U;                                               // [+4] the force
    if (Ub(M[4]) > 0xbf4ccccdu) {                       // upside down (or a negative NaN), by the bits
        P.y = (float)(D(P.y) + 1.0f);
        memcpy(&U, &M[0], 12);
        const double k = 1.0f / x87_sqrt((D(U.y) * U.y + D(U.z) * U.z) + D(U.x) * U.x);
        U.x = (float)(D(U.x) * k);
        U.y = (float)(D(U.y) * k);
        U.z = (float)(k * U.z);
        U.x = (float)(D(U.x) * k_help_force);
        U.y = (float)(D(U.y) * k_help_force);
        U.z = (float)(D(U.z) * k_help_force);
        PhobDyno_ApplyForce(self, 0, &U, &P);
        P.y = (float)(D(P.y) - 4.0f);
        U.x = (float)-D(U.x);
        U.z = (float)-D(U.z);
        PhobDyno_ApplyForce(self, 0, &U, &P);
        return;
    }
    if (I(M[4]) < 0x3e4ccccd) {                         // on its side
        P.y = (float)(D(P.y) + 1.0f);
        memcpy(&U, &M[3], 12);
        U.x = (float)-D(U.x);
        U.z = (float)-D(U.z);
        const double k = 1.0f / x87_sqrt((D(U.y) * U.y + D(U.z) * U.z) + D(U.x) * U.x);
        U.x = (float)(D(U.x) * k);
        U.y = (float)(D(U.y) * k);
        U.z = (float)(k * U.z);
        U.x = (float)(D(U.x) * k_help_force);
        U.y = (float)(D(U.y) * k_help_force);
        U.z = (float)(D(U.z) * k_help_force);
        PhobDyno_ApplyForce(self, 0, &U, &P);
        return;
    }
    P.x = (float)(D(M[6]) * 3.0f + P.x);
    P.y = (float)(D(M[7]) * 3.0f + P.y);
    U.x = 0.0f;
    U.y = Fb(k_help_force_bits);
    U.z = 0.0f;
    P.z = (float)(D(M[8]) * 3.0f + P.z);
    PhobDyno_ApplyForce(self, 0, &U, &P);
}
static void fp_help_out(Footprint& f, Car* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00439780, "Car::HelpOut", Car_HelpOut, fp_help_out)

// ==== Car::SuperHelpOut (0x439a00): set upright and still, 2 m above the ground where it stands ===============
static void __fastcall Car_SuperHelpOut(Car* self, Edx) {
    P3 P;                                               // [+0] integer copies of the position
    memcpy(&P, &self->frame.pos, 12);
    float* m = self->frame.rot.m;
    m[0] = Fb(k_one_bits); m[1] = 0.0f; m[2] = 0.0f;
    m[3] = 0.0f; m[4] = Fb(k_one_bits); m[5] = 0.0f;
    m[6] = 0.0f; m[7] = 0.0f; m[8] = Fb(k_one_bits);
    self->frame.pos.x = 0.0f; self->frame.pos.y = 0.0f; self->frame.pos.z = 0.0f;
    self->velocity.x = 0.0f; self->velocity.y = 0.0f; self->velocity.z = 0.0f;
    self->angular_velocity.x = 0.0f; self->angular_velocity.y = 0.0f; self->angular_velocity.z = 0.0f;
    const double h = TerrainGetHeightXZ(Ub(P.x), Ub(P.z));
    P.y = (float)(h + 2.0f);
    memcpy(&self->frame.pos, &P, 12);
}
// the car; TerrainGetHeight only reads the track
static void fp_super_help_out(Footprint& f, Car* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00439a00, "Car::SuperHelpOut", Car_SuperHelpOut, fp_super_help_out)
