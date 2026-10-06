// phys_wheel_update.cpp -- M3 3.3 group W1: Wheel::Update (physics:wheel.obj, 0x448a60), rewritten.
//
// The whole per-tick wheel model: spin integration, brake / ABS / traction control, the gyroscopic torque
// of the spinning wheel, the suspension (terrain ray, spring, damper, bump stop, anti-roll, road noise,
// bump-stop impulse), the surface's grip and drag, the tyre's combined-slip forces (the Tire getters and
// the Pacejka resultant), the aligning torque, and the chassis / drag / slide-stabilising forces.
//
// Written from the v1.0 disassembly, block by block: the same sums in the same grouping, values the
// original keeps in x87 registers as doubles and what it stores as floats, every call in the original's
// order with the same arguments (floats it passes with integer moves passed as their bits), and every
// integer compare of a float's bits done on the bits. Divisions go through the Pentium FDIV-bug
// workaround in the original, so they're plain divisions here. Callees -- update_wheel_position, the
// Tire getters, Damper::GetDampingRate, PhobDyno's force helpers, TerrainGetHeight, Random -- are called
// by address, so whichever version is hooked there is what runs. Wheel::Update makes no virtual calls.
//
// The original's frame is 0xfc bytes of locals; the comments give each local's offset in that frame
// (after its four register pushes) where it helps to follow the disassembly.
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

// ---- layouts (anonymous: the other wheel.obj / tire.obj groups may define their own until these move to
// phys_types.h) ------------------------------------------------------------------------------------------------
namespace {

// Damper (carpart.obj, 16 bytes): GetDampingRate(v) = bump_slope v + bump_base (v > 0), else
// rebound_base - rebound_slope v; never below 0
struct Damper { float bump_slope, bump_base, rebound_slope, rebound_base; };
static_assert(sizeof(Damper) == 16, "Damper");

// Tire (tire.obj, 64 bytes, no vtable)
struct Tire {
    float cornering_quad, cornering_lin;         // +0x00 stiffness = quad Fz^2 + lin Fz
    float friction_slope, friction_base;         // +0x08 lateral mu = slope Fz + base
    float camber_stiffness_factor;               // +0x10
    float long_stiffness_factor;                 // +0x14
    float long_friction_factor;                  // +0x18
    float pacejka_b, pacejka_c, pacejka_d, pacejka_e;   // +0x1c
    float load_factor;                           // +0x2c
    float mass, inertia;                         // +0x30
    float camber_peak, camber_peak_gain;         // +0x38
};
static_assert(sizeof(Tire) == 64, "Tire");

// Wheel (wheel.obj, 420 bytes, vtable 0x4dca78), embedded 4x in Car at +0x554. Names from out/types.tsv.
struct Wheel {
    void** vtable;                     // +0x000
    P3 droop_offset;                   // +0x004 body-space hub position at full droop, minus the hub vertex
    P3* hub_vertex;                    // +0x010 the body-mesh vertex nearest the hub (moves with dents)
    P3 hub_vertex_rest;                // +0x014 its position at Setup
    uint8_t broken, _021[3];           // +0x020
    Damper damper;                     // +0x024
    float inertia;                     // +0x034 kg m^2
    float inv_inertia;                 // +0x038
    float spring_rate;                 // +0x03c N/m
    float spring_preload;              // +0x040
    float travel;                      // +0x044 m
    float static_deflection;           // +0x048
    float bump_stop_rate;              // +0x04c
    float brake_torque_max;            // +0x050
    float roll_resist_speed;           // +0x054
    float roll_resist_static;          // +0x058
    float radius;                      // +0x05c
    float static_camber;               // +0x060
    float static_toe;                  // +0x064
    float steer_quadratic;             // +0x068
    float bump_steer;                  // +0x06c
    float bump_camber;                 // +0x070
    float toe_compliance;              // +0x074
    float camber_compliance;           // +0x078
    float caster;                      // +0x07c
    P3 susp_axis;                      // +0x080 body space
    Tire* tire;                        // +0x08c
    float tire_load_factor;            // +0x090
    float grip_scale;                  // +0x094
    float stiffness_scale;             // +0x098
    float road_noise_quad;             // +0x09c
    float road_noise_lin;              // +0x0a0
    float aligning_torque;             // +0x0a4
    float surface_speed;               // +0x0a8 omega x radius
    float spin_angle;                  // +0x0ac
    P3 heading;                        // +0x0b0 body-space rolling direction
    P3 replay_velocity;                // +0x0bc
    P3 droop_point_world;              // +0x0c8
    float steer_angle;                 // +0x0d4
    float camber;                      // +0x0d8
    float steer_input;                 // +0x0dc
    float brake_input;                 // +0x0e0
    float drive_torque;                // +0x0e4
    float compression;                 // +0x0e8
    float lat_force_ratio;             // +0x0ec
    float long_force_ratio;            // +0x0f0
    P3 tire_force;                     // +0x0f4
    P3 chassis_tire_force;             // +0x100
    float lat_force_filtered;          // +0x10c
    P3 susp_axis_world;                // +0x110
    float slide;                       // +0x11c
    uint8_t fx_flags;                  // +0x120 1/2 squeal, 4 spray, 0x20 off-road, 0x40 broken
    uint8_t surface_class, _122[2];    // +0x121 0 pavement / airborne, 5 off-road
    float omega;                       // +0x124 rad/s
    float rpm;                         // +0x128
    float brake_power;                 // +0x12c
    P3 contact_point;                  // +0x130
    float prev_omega;                  // +0x13c
    P3 prev_axle;                      // +0x140
    uint32_t _14c[3];                  // +0x14c (not touched here)
    float anti_roll_force;             // +0x158
    int32_t traction_control;          // +0x15c
    uint8_t digital_throttle, _161[3]; // +0x160
    int32_t abs_mode;                  // +0x164
    uint8_t on_ground, _169[3];        // +0x168
    uint32_t _16c;                     // +0x16c (not touched here)
    float lat_slip;                    // +0x170
    float long_slip;                   // +0x174
    float susp_force;                  // +0x178
    float drag_torque;                 // +0x17c
    float long_slip_raw;               // +0x180
    void* tire_sound;                  // +0x184
    float brake_cooling;               // +0x188
    float brake_heat_capacity;         // +0x18c
    float inv_brake_heat_capacity;     // +0x190
    float brake_temp;                  // +0x194
    float heat_accum;                  // +0x198
    int32_t surface;                   // +0x19c terrain surface code
    uint8_t is_remote, _1a1[3];        // +0x1a0
};
static_assert(offsetof(Wheel, damper) == 0x24 && offsetof(Wheel, tire) == 0x8c &&
              offsetof(Wheel, droop_point_world) == 0xc8 && offsetof(Wheel, slide) == 0x11c &&
              offsetof(Wheel, omega) == 0x124 && offsetof(Wheel, prev_axle) == 0x140 &&
              offsetof(Wheel, anti_roll_force) == 0x158 && offsetof(Wheel, on_ground) == 0x168 &&
              offsetof(Wheel, tire_sound) == 0x184 && offsetof(Wheel, surface) == 0x19c &&
              sizeof(Wheel) == 0x1a4, "Wheel");

// Car (car.obj, 3768 bytes, vtable 0x4dbfd0): only what Wheel::Update reads
struct Car : PhobDyno {
    uint8_t _478[0x4f4 - 0x478];
    int32_t realism;                   // +0x4f4 0 = the slide-stabilising force is on
    uint8_t _4f8[0x554 - 0x4f8];
    Wheel wheels[4];                   // +0x554
    uint8_t _be4[3768 - 0xbe4];
};
static_assert(offsetof(Car, realism) == 0x4f4 && offsetof(Car, wheels) == 0x554 && sizeof(Car) == 3768, "Car");

}  // namespace

// ---- constants, exactly as the original's (several aren't the float nearest their decimal) --------------
static const float k_broken_dist2 = 0x1.47ae16p-9f;    // 0x3b23d70b  0.0025000002 (5 cm, squared)
static const float k_dt_half = 0x1.0624dep-7f;         // 0x3c03126f  0.008 (half a step: the spin angle)
static const float k_two_pi = 0x1.921fb6p+2f;          // 0x40c90fdb
static const float k_min_load = 0x1.bcfffep+8f;        // 0x43de7fff  444.99997 (100 lbf)
static const float k_brake_slip = 0x1.433334p+3f;      // 0x4121999a  10.1
static const float k_minus_ninth = -0x1.c71c72p-4f;    // 0xbde38e39
static const float k_tc_target = 0x1.674c5ap-2f;       // 0x3eb3a62d  0.3508772
static const float k_tc_slow = 0x1.aaaaaap+2f;         // 0x40d55555  6.6666665 m/s
static const float k_tc_ramp0 = 0x1.8e38e4p+1f;        // 0x40471c72  3.1111112
static const float k_tc_ramp = 0x1.200002p-2f;         // 0x3e900001  0.28125003
static const float k_normal_tol = 0x1.a36e2ep-14f;     // 0x38d1b717  1e-4
static const float k_inch = 0x1.a02752p-6f;            // 0x3cd013a9  0.0254
static const float k_one_over_dt = 0x1.f3fffep+5f;     // 0x4279ffff  62.499996 (not 62.5)
static const float k_two_thirds = 0x1.555556p-1f;      // 0x3f2aaaab
static const float k_eps = 0x1p-23f;                   // 0x34000000  FLT_EPSILON
static const float k_align_lat = -0x1.1d0934p-14f;     // 0xb88e849a  -6.795787e-05
static const float k_align_long = 0x1.1d0934p-14f;     // 0x388e849a  6.795787e-05
static const float k_vel_scale = 0x1.1b91b8p-6f;       // 0x3c8dc8dc  0.017307691 (1/57.78)
static const float k_rad_to_rpm = 0x1.3193d6p+3f;      // 0x4118c9eb  9.549296
// constants the original stores with integer moves
static const uint32_t k_abs_target = 0x3f8e38e4u;      // 1.1111112
static const uint32_t k_abs_target_1 = 0x3faaaaabu;    // 1.3333334 (abs_mode 1)
static const uint32_t k_break_impulse = 0x4534c7ffu;   // 2892.4998 (compared as bits)

// ---- the functions it calls, by address -----------------------------------------------------------------
typedef uint8_t(__cdecl* Flag_t)();
static const Flag_t PhysicsIsDamageOn = (Flag_t)0x0042bd60;
static const Flag_t HackPaveTheWorld = (Flag_t)0x0040d580;
// PhobDyno::GetPointVelocity(out, world point)
typedef void(__fastcall* GetPointVelocity_t)(PhobDyno*, Edx, P3*, const P3*);
static const GetPointVelocity_t GetPointVelocity = (GetPointVelocity_t)0x00444ee0;
typedef void(__cdecl* MulPoint_t)(P3*, const P3*, const M3*);
static const MulPoint_t MatrixMulPoint = (MulPoint_t)0x00429420;          // o = p x M
// PhobRoot::GetArmPosition(out world, body-space point)
typedef void(__fastcall* GetArmPosition_t)(PhobRoot*, Edx, P3*, const P3*);
static const GetArmPosition_t GetArmPosition = (GetArmPosition_t)0x0043e0d0;
typedef void(__fastcall* ApplyTorque_t)(PhobDyno*, Edx, const P3*);
static const ApplyTorque_t PhobDyno_ApplyTorque = (ApplyTorque_t)0x00444d20;
typedef void(__fastcall* ApplyForce_t)(PhobDyno*, Edx, const P3*, const P3*);
static const ApplyForce_t PhobDyno_ApplyForce = (ApplyForce_t)0x00444c20;
typedef void(__fastcall* ApplyImpulse_t)(PhobDyno*, Edx, const P3*, const P3*, float);
static const ApplyImpulse_t PhobDyno_ApplyImpulse = (ApplyImpulse_t)0x00444d60;
typedef void(__fastcall* ApplySuspensionForce_t)(Car*, Edx, const P3*, const P3*);
static const ApplySuspensionForce_t Car_ApplySuspensionForce = (ApplySuspensionForce_t)0x00437660;
// TerrainGetHeight(x, z, hit, normal, surface): the coordinates are pushed as bits
typedef uint8_t(__cdecl* TerrainGetHeight_t)(uint32_t, uint32_t, P3*, P3*, int32_t*);
static const TerrainGetHeight_t TerrainGetHeight = (TerrainGetHeight_t)0x00465c40;
// ASSERT_MSG (wheel.obj's copy): a bare `ret` in the release build; kept, as the original calls it
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
static const AssertMsg_t ASSERT_MSG = (AssertMsg_t)0x0044a430;
static const char* const k_bad_normal = (const char*)0x004edf6c;   // "Bad normal (len = %f)"
typedef int(__cdecl* Random_t)(int);
static const Random_t Random = (Random_t)0x0041b6e0;
// get_impulse_magnitude(pos, velocity, mass, inverse inertia, point, normal, restitution): returns ST0
typedef double(__cdecl* ImpulseMagnitude_t)(const P3*, const P3*, uint32_t, const M3*, const P3*, const P3*, uint32_t);
static const ImpulseMagnitude_t get_impulse_magnitude = (ImpulseMagnitude_t)0x00435630;
typedef void(__fastcall* UpdateWheelPosition_t)(Wheel*, Edx, Car*);
static const UpdateWheelPosition_t update_wheel_position = (UpdateWheelPosition_t)0x0044a490;
// Damper::GetDampingRate(v), Tire's getters (Fz): the float argument is pushed as bits; each returns ST0
typedef double(__fastcall* DampingRate_t)(const Damper*, Edx, uint32_t);
static const DampingRate_t Damper_GetDampingRate = (DampingRate_t)0x00446ae0;
typedef double(__fastcall* TireGet_t)(const Tire*, Edx, uint32_t);
static const TireGet_t Tire_GetCorneringStiffness = (TireGet_t)0x0043bba0;
static const TireGet_t Tire_GetLongStiffness = (TireGet_t)0x0043bbc0;
static const TireGet_t Tire_GetCamberStiffness = (TireGet_t)0x0043bbe0;
static const TireGet_t Tire_GetLateralFriction = (TireGet_t)0x0043bc80;
static const TireGet_t Tire_GetLongFriction = (TireGet_t)0x0043bc90;
static const TireGet_t Tire_GetResultant = (TireGet_t)0x0043bcb0;
typedef double(__fastcall* CamberGrip_t)(const Tire*, Edx, uint32_t, uint32_t, uint32_t);
static const CamberGrip_t Tire_GetCamberGripFactor = (CamberGrip_t)0x0043bc00;

// ==== Wheel::Update (0x448a60) ===============================================================================
static void __fastcall Wheel_Update(Wheel* w, Edx, Car* car) {
    const float* M = car->frame.rot.m;
    const float radius = w->radius;                     // +0x38, an integer copy
    P3 N;                                               // +0x4c the ground normal: up, unless the terrain says
    N.x = 0.0f; N.z = 0.0f; N.y = 1.0f;
    w->surface_speed = (float)(D(w->omega) * radius);

    // ---- a hub vertex dented more than 5 cm from where it was breaks the wheel
    if (PhysicsIsDamageOn()) {
        const P3* hv = w->hub_vertex;
        float dx = (float)(D(hv->x) - w->hub_vertex_rest.x);
        float dy = (float)(D(hv->y) - w->hub_vertex_rest.y);
        float dz = (float)(D(hv->z) - w->hub_vertex_rest.z);
        if ((D(dy) * dy + D(dz) * dz) + D(dx) * dx > k_broken_dist2) w->broken = 1;   // test ah,0x41
    }

    // ---- the spin angle (half a step's worth: it's drawn between ticks), wrapped by its bits
    w->spin_angle = (float)(D(w->omega) * k_dt_half + w->spin_angle);
    if (I(w->spin_angle) > 0x40c90fdb) w->spin_angle = (float)(D(w->spin_angle) - k_two_pi);
    if (Ub(w->spin_angle) > 0xc0c90fdbu) w->spin_angle = (float)(D(w->spin_angle) + k_two_pi);

    P3 V;                                               // +0xac velocity at last tick's contact point
    GetPointVelocity(car, 0, &V, &w->contact_point);
    P3 H;                                               // +0x58 the rolling direction, world
    MatrixMulPoint(&H, &w->heading, &car->frame.rot);
    // (the original then compares |surface_speed| with 1 and surface_speed with 0 and ignores the result)

    // ---- brakes: |input| x max (20 % of max on a broken wheel), ABS-limited on long slip
    const double abs_brake_r = fabs(D(w->brake_input));
    const float abs_brake = (float)abs_brake_r;         // +0xc4
    const float long_slip_prev = w->long_slip;          // +0x8c, an integer copy
    const uint8_t broken = w->broken;                   // cl, held to the drive torque below
    float brake;                                        // +0x24
    if (broken) brake = (float)(D(w->brake_torque_max) * (float)0.2f);
    else brake = (float)(abs_brake_r * w->brake_torque_max);
    if (w->abs_mode != 0 && I(w->brake_input) > 0 && !broken && I(w->surface_speed) > 0x3f800000) {
        const float target = Fb(w->abs_mode == 1 ? k_abs_target_1 : k_abs_target);
        const double d = D(target) - long_slip_prev;
        const float df = (float)d;
        if (!(d > 0.0f)) brake = 0.0f;                  // test ah,0x41
        else {
            const double lim = D(df) * 100000.0f;
            if (D(brake) > lim) brake = (float)lim;     // the smaller (a NaN keeps the brake torque)
        }
    }

    // ---- the torque against the spin: rolling resistance, brake, and last tick's (averaged)
    const double load_r = fabs(D(w->susp_force));
    float load = (float)load_r;                         // +0x2c
    if (!(load_r >= k_min_load)) load = k_min_load;     // test ah,1: a NaN takes the minimum
    const double aw = fabs(D(w->omega));
    const float absw = (float)aw;                       // +0x28 |omega|
    const double slip_lim = aw * brake * k_brake_slip;  // a slow wheel can't take the full brake torque
    const double applied = D(brake) > slip_lim ? slip_lim : D(brake);
    const float applied_f = (float)applied;             // +0x10
    w->brake_power = (float)(applied * absw);
    double rr = D(absw) * (float)0.1f;
    rr = 1.0f > rr ? rr : 1.0f;                         // min(|omega| x 0.1, 1); a NaN gives 1
    const double r_static = fabs(rr * w->roll_resist_static * load);
    const double r_speed = fabs(D(w->roll_resist_speed) * w->omega * radius * load);
    const float td = (float)((((r_static + r_speed) + applied_f) + w->drag_torque) * 0.5f);
    w->drag_torque = td;
    float torque = I(w->omega) > 0 ? (float)-D(td) : td;   // +0x7c the net torque on the wheel

    // ---- below 9 rad/s a braked wheel locks: the drag fades out and the spin is pulled down
    float lock = 0.0f;                                  // +0xe4
    if (I(absw) < 0x41100000) {
        double v = (D(absw) * k_minus_ninth + 1.0f) * abs_brake * 1.5f;
        v = 1.0f > v ? v : 1.0f;
        lock = (float)v;
        torque = (float)((1.0f - v) * torque);
        w->omega = (float)((D(lock) * -(float)0.2f + 1.0f) * w->omega);
    }

    // ---- drive torque, as a force at the rim; traction control / the digital throttle's slip limit
    float drive = (float)(D(w->drive_torque) / w->radius);   // +0x2c
    if (broken) drive = 0.0f;
    const bool digital = w->digital_throttle != 0 && w->traction_control == 0;
    if ((digital || w->traction_control != 0) && I(drive) > 0 && I(w->surface_speed) > 0x3f800000) {
        const float target = digital ? 2.0f : k_tc_target;
        const float lat_weight = w->traction_control == 2 ? 5.0f : 0.0f;
        const double e = x87_sqrt(D(w->lat_slip) * w->lat_slip * lat_weight + D(long_slip_prev) * long_slip_prev) - target;
        float excess;                                   // +0xc4
        if (digital) {
            const float ef = (float)e;
            // the forward speed: the digital throttle's limit ramps in from 3.1 to 6.7 m/s
            const double fwd = (D(car->velocity.y) * M[7] + D(M[8]) * car->velocity.z) + D(M[6]) * car->velocity.x;
            const float fwdf = (float)fwd;
            if (!(fwd >= k_tc_slow)) {                  // test ah,1
                double q = (D(fwdf) - k_tc_ramp0) * k_tc_ramp;
                q = !(0.0f >= q) ? q : 0.0f;            // max(q, 0); a NaN stays
                excess = (float)(q * ef);
            } else excess = ef;
        } else excess = (float)e;
        if (I(excess) > 0) {
            const double r = (D(excess) * -(float)0.1f + 1.0f) * drive;
            drive = (float)(!(0.0f >= r) ? r : 0.0f);
        }
    }
    torque = (float)(D(torque) + drive);

    // ---- where the wheel hangs at full droop, in the world
    P3 Up;                                              // +0x64 the body's up axis (row 1), integer copies
    memcpy(&Up, &M[3], 12);
    P3 local;                                           // +0x100
    const P3* hv = w->hub_vertex;
    local.x = (float)(D(w->droop_offset.x) + hv->x);
    local.y = (float)(D(w->droop_offset.y) + hv->y);
    local.z = (float)(D(hv->z) + w->droop_offset.z);
    GetArmPosition(car, 0, &w->droop_point_world, &local);
    w->surface = 0;

    // ---- the spinning wheel's gyroscopic torque: the change in its angular momentum since last tick
    P3 A;                                               // +0xd0 the axle, up x heading
    A.x = (float)(D(Up.z) * H.y - D(Up.y) * H.z);
    A.y = (float)(D(Up.x) * H.z - D(Up.z) * H.x);
    A.z = (float)(D(Up.y) * H.x - D(Up.x) * H.y);
    const double om = w->omega;
    P3 W;                                               // +0xa0 the spin vector
    W.x = (float)(D(A.x) * om);
    W.y = (float)(D(A.y) * om);
    W.z = (float)(om * A.z);
    const double npo = -D(w->prev_omega);
    P3 G;                                               // +0x80
    G.x = (float)(D(w->prev_axle.x) * npo + W.x);
    memcpy(&w->prev_omega, &w->omega, 4);
    G.y = (float)(D(w->prev_axle.y) * npo + W.y);
    G.z = (float)(npo * w->prev_axle.z + W.z);
    // (the original also stores W x G to its frame, and never reads it)
    const double gk = D(w->inertia) * 12.5f;
    G.x = (float)(D(G.x) * gk);
    G.y = (float)(D(G.y) * gk);
    G.z = (float)(gk * G.z);
    PhobDyno_ApplyTorque(car, 0, &G);
    w->aligning_torque = 0.0f;
    memcpy(&w->prev_axle, &A, 12);
    w->susp_force = 0.0f;

    // ---- the ground under the wheel: compression into the travel
    float comp = 0.0f;                                  // +0x1c
    uint8_t down = 0;                                   // bl
    if (I(M[4]) > 0) {                                  // right way up
        P3 hit;                                         // +0xf4
        if (TerrainGetHeight(Ub(w->droop_point_world.x), Ub(w->droop_point_world.z), &hit, &N, &w->surface) &&
            (D(Up.z) * N.z + D(Up.y) * N.y) + D(Up.x) * N.x > (float)0.1f && w->surface != 14) {
            if (HackPaveTheWorld()) w->surface = 0;
            const double len = x87_sqrt((D(N.y) * N.y + D(N.z) * N.z) + D(N.x) * N.x);
            ASSERT_MSG(!(fabs(len - 1.0f) >= k_normal_tol) ? 1 : 0, k_bad_normal, len);
            memcpy(&w->contact_point, &hit, 12);
            const P3& dp = w->droop_point_world;
            const double pen = ((D(hit.y) - dp.y) * Up.y + (D(hit.z) - dp.z) * Up.z) + (D(hit.x) - dp.x) * Up.x;
            comp = (float)pen;
            if (!(pen >= 0.0f)) comp = 0.0f;            // test ah,1: below the droop point (or NaN): off
            else {
                // the ground's slope softens it: x clamp(2 |up . n| - 0.5, 0, 1)
                double c = fabs((D(Up.z) * N.z + D(Up.y) * N.y) + D(Up.x) * N.x) * 2.0f - 0.5f;
                c = !(0.0f >= c) ? c : 0.0f;
                c = 1.0f > c ? c : 1.0f;
                comp = (float)(c * comp);
                const double lim = D(w->travel) + 3.0f;
                const float limf = (float)lim;
                if (!(lim > comp)) comp = limf;         // test ah,0x41
                down = 1;
            }
        }
    }
    w->on_ground = down;

    // ---- spring, damper, bump stop, anti-roll
    const float speed = (float)((D(comp) - w->compression) * 62.5f);   // +0x2c compression speed
    const double bs_r = D(w->travel) - k_inch;
    const float bs = (float)bs_r;
    float bump;                                         // +0x28
    if (!(bs_r >= comp)) bump = (float)((D(comp) - bs) * w->bump_stop_rate);   // test ah,1
    else bump = 0.0f;
    const float rate = (float)Damper_GetDampingRate(&w->damper, 0, Ub(speed));   // +0x24
    const float spring = (float)(D(w->spring_rate) * comp);                     // +0x10
    const double f = (((D(rate) * speed + w->spring_preload) + w->anti_roll_force) + spring) + bump;
    float F;                                            // +0x20 the suspension force
    if (w->broken) F = (float)(D(spring) * -(float)0.9f + (float)f);   // a broken wheel keeps 10 % of its spring
    else F = (float)f;

    // ---- road noise: a random +- kick, by speed, by surface
    const double vfwd = (D(M[6]) * V.x + D(M[8]) * V.z) + D(M[7]) * V.y;
    const float vf = (float)vfwd;                       // +0x10
    const float av = (float)fabs(vfwd);                 // +0x2c
    const double nq = D(vf) * vf * w->road_noise_quad + D(w->road_noise_lin) * av;
    const double nm = 1.0f > D(av) ? D(av) : 1.0f;
    const float noise = (float)(nq * nm);
    const float sgn = Random(2) ? 1.0f : -1.0f;
    float kick = (float)(D(sgn) * noise * F);           // +0x10
    if (!w->is_remote && w->surface == 10) kick = (float)(D(kick) * 9.0f);   // bumpy
    else if (w->surface != 0) kick = (float)(D(kick) * 3.0f);              // off-road
    else kick = 0.0f;                                                       // road
    F = (float)(D(kick) * (float)0.3f + F);
    w->susp_force = F;

    // ---- hitting the bump stop hard: an impulse along the ground normal (breaks the wheel past 2892.5)
    if (I(bump) > 0x3c23d70a && !w->broken) {
        const double mag = get_impulse_magnitude(&car->frame.pos, &V, Ub(car->mass), &car->inv_inertia_world,
                                                 &w->droop_point_world, &N, 0);
        const float magf = (float)mag;                  // +0x2c
        if (mag > (float)0.01f) {                              // test ah,0x41
            if (I(magf) > (int32_t)k_break_impulse && PhysicsIsDamageOn()) w->broken = 1;
            P3 J;                                       // +0x10
            J.x = (float)(D(N.x) * magf);
            J.y = (float)(D(N.y) * magf);
            J.z = (float)(D(N.z) * magf);
            PhobDyno_ApplyImpulse(car, 0, &J, &w->droop_point_world, (float)(D(comp) * 1000.0f));
        }
    }

    // ---- a wheel pulled down (or off the ground) hangs where spring and damper balance
    uint8_t air = 0;                                    // bl
    if (Ub(F) > 0x80000000u || !w->on_ground) {
        const double num = D(w->compression) * rate * k_one_over_dt - w->anti_roll_force;
        F = 0.0f;
        air = 1;
        const double den = D(rate) * k_one_over_dt + w->spring_rate;
        w->on_ground = 0;
        comp = (float)(num / den);
    }
    if (I(F) > 0x468b1000) F = 17800.0f;
    w->compression = comp;
    update_wheel_position(w, 0, car);
    P3 S;                                               // +0x3c
    S.x = (float)(D(Up.x) * F);
    S.y = (float)(D(Up.y) * F);
    S.z = (float)(D(Up.z) * F);
    Car_ApplySuspensionForce(car, 0, &S, &w->droop_point_world);
    if (air) F = 0.0f;

    // ---- the surface: grip and drag, and the sound / particle flags
    float grip = 1.0f;                                  // +0x1c
    float sdrag = 0.0f;                                 // +0x78
    w->fx_flags = 0x20;
    w->surface_class = 1;
    switch ((uint32_t)w->surface) {                     // a jump table over 0..23; anything else is default
    case 0:                                             // road
        w->surface_class = 0;
        if (I(w->slide) > 0x3f666666) w->fx_flags = 0x22;
        if (I(w->slide) > 0x3f333333) w->fx_flags |= 1;
        w->fx_flags &= 0xdf;
        break;
    case 10: case 11: case 13: case 15:
        w->surface_class = 5;
        if (I(w->slide) > 0x3fc00000) w->fx_flags = 0x24;
        grip = Fb(0x3f4ccccdu);                         // 0.8
        sdrag = 2.5f;
        break;
    case 12:
        w->surface_class = 5;
        grip = Fb(0x3f666666u);                         // 0.9
        sdrag = Fb(0x3dcccccdu);                        // 0.1
        break;
    case 20: case 23:
        w->surface_class = 5;
        if (I(w->slide) > 0x3fc00000) w->fx_flags = 0x24;
        grip = Fb(0x3f666666u);
        sdrag = Fb(0x3dcccccdu);
        break;
    case 21:
        w->surface_class = 5;
        w->fx_flags = 0x24;
        grip = Fb(0x3f666666u);
        sdrag = Fb(0x3e4ccccdu);                        // 0.2
        break;
    default:
        if (I(w->slide) > 0x3fc00000) w->fx_flags = 0x24;
        w->surface_class = 5;
        grip = Fb(0x3f666666u);
        sdrag = Fb(0x3dcccccdu);
        break;
    }
    if (air || !w->on_ground) {
        w->lat_force_ratio = 0.0f;
        w->long_force_ratio = 0.0f;
        w->slide = 0.0f;
        w->surface_class = 0;
        w->fx_flags = 0;
    }

    // ---- the tyre's normal load: the suspension force plus last tick's tyre force, along the normal
    double ld = ((D(Up.y) * F + w->chassis_tire_force.y) * N.y + (D(Up.z) * F + w->chassis_tire_force.z) * N.z) +
                (D(Up.x) * F + w->chassis_tire_force.x) * N.x;
    ld = !(1.0f >= ld) ? ld : 1.0f;                     // at least 1 N
    const float Fz = (float)(ld / w->tire_load_factor); // +0x48
    float scale = 1.0f;                                 // +0x74 slip scale at walking pace
    GetPointVelocity(car, 0, &V, &w->droop_point_world);
    P3 L;                                               // +0x8c the lateral axis, normal x heading
    L.x = (float)(D(N.y) * H.z - D(N.z) * H.y);
    L.y = (float)(D(N.z) * H.x - D(N.x) * H.z);
    L.z = (float)(D(N.x) * H.y - D(N.y) * H.x);
    const float vlat = (float)((D(L.z) * V.z + D(L.y) * V.y) + D(L.x) * V.x);    // +0x10
    float vlong = (float)((D(V.z) * H.z + D(V.y) * H.y) + D(V.x) * H.x);        // +0x24
    const float avl = (float)fabs(D(vlong));            // +0x20
    const float vlong0 = vlong;                         // +0x2c, an integer copy
    const double vmin_r = D(grip) * 10.0f;
    const float vmin = (float)vmin_r;                   // +0x28
    if (vmin_r > avl) {                                 // test ah,0x41: below 10 x grip m/s
        if (I(vlong) > 0) vlong = vmin;
        else vlong = (float)(D(grip) * -10.0f);
        scale = (float)(fabs(D(vlong)) / vmin);
    }

    // ---- slips
    const double avlong = fabs(D(vlong));
    const float slip_angle = (float)(D(vlat) / avlong); // +0x98
    const float camber_eff = (float)((D(w->camber) - ((D(L.y) * Up.y + D(L.z) * Up.z) + D(L.x) * Up.x)) * scale);  // +0x28
    float lsr = (float)-((D(w->omega) * radius - vlong0) / avlong);   // +0x70 longitudinal slip ratio

    const float cs = (float)(Tire_GetCorneringStiffness(w->tire, 0, Ub(Fz)) * w->stiffness_scale);   // +0x24
    float mu_lat = (float)(Tire_GetLateralFriction(w->tire, 0, Ub(Fz)) * w->grip_scale * grip);       // +0xbc
    const float ls = (float)(Tire_GetLongStiffness(w->tire, 0, Ub(Fz)) * w->stiffness_scale);        // +0x38
    const float mu_long = (float)(Tire_GetLongFriction(w->tire, 0, Ub(Fz)) * w->grip_scale * grip);  // +0xc0
    const float cam_st = (float)Tire_GetCamberStiffness(w->tire, 0, Ub(Fz));                        // +0x10
    const double mu_lat_r = Tire_GetCamberGripFactor(w->tire, 0, Ub(Fz), Ub(slip_angle), Ub(camber_eff)) * mu_lat;
    mu_lat = (float)mu_lat_r;
    const double mufz_r = mu_lat_r * Fz;
    const float mufz = (float)mufz_r;                   // +0x1c peak lateral force
    float lat_norm = (float)(D(cs) / mufz_r * slip_angle);        // +0xdc normalised lateral slip
    float cam_norm = (float)(D(cam_st) / mufz * camber_eff);      // +0x9c normalised camber thrust
    const float sa_sign = I(slip_angle) > 0 ? 1.0f : -1.0f;       // +0x10
    if (I(cam_norm) > 0x3f666666) cam_norm = Fb(0x3f666666u);
    if (Ub(cam_norm) > 0xbf666666u) cam_norm = Fb(0xbf666666u);
    const double den_r = 1.0f - D(sa_sign) * cam_norm;
    const float den = (float)den_r;                     // +0x2c
    const float lat_eff = (float)(D(lat_norm) / den_r); // +0x28
    if (I(lsr) > 0x41a00000) lsr = 20.0f;
    if (Ub(lsr) > 0xc1a00000u) lsr = -20.0f;
    float long_norm = (float)(D(ls) / (D(mu_long) * Fz) * lsr * scale);   // +0xe0
    if (air || !w->on_ground) {
        long_norm = 0.0f;
        lat_norm = 0.0f;
        w->lat_slip = 0.0f;
        w->long_slip = 0.0f;
        w->susp_force = 0.0f;
    }

    // ---- filtered slips (quicker above 10 m/s) and the combined slip
    const bool fast = I(avl) > 0x41200000;
    const float blend = fast ? (float)0.4f : (float)0.6f;             // +0xb8
    const float lat_new = fast ? 0.0f : (float)0.2f;           // +0x10 (the lateral filter takes 1 - this of the new)
    w->long_slip_raw = long_norm;
    // (VP_KEEP: GCC would fold 1 - 0.2f / 1 - 0.4f exactly; the original's fsub rounds at the physics thread's 24 bits)
    const double nlat = (1.0f - D(VP_KEEP(lat_new))) * lat_norm + D(w->lat_slip) * blend;
    const double nlong = (1.0f - D(VP_KEEP(blend))) * long_norm + D(w->long_slip) * blend;
    w->lat_slip = (float)nlat;
    w->long_slip = (float)nlong;
    const double comb = x87_sqrt(D(lat_eff) * lat_eff + nlong * nlong);
    const float combf = (float)comb;                    // +0x20
    if (w->on_ground) {
        const double s = comb * k_two_thirds - 1.0f;
        w->slide = (float)(!(0.0f >= s) ? s : 0.0f);
    } else w->slide = 0.0f;

    // ---- the Pacejka resultant, shared between the two directions
    const float res = (float)Tire_GetResultant(w->tire, 0, Ub(combf));        // +0x28
    const float ratio = (float)((D(mu_long) * cs) / (D(ls) * mu_lat));        // +0x10
    float bk;                                           // +0x38 blends the lateral slip's weight
    if (fabs(D(combf)) > k_two_pi) bk = 1.0f;           // test ah,0x41
    else if (!(fabs(D(ratio) - 1.0f) > k_eps)) bk = 1.0f;
    else bk = (float)(x87_cos_mul(D(combf) * 0.5f, 1.0f - D(ratio)) * -0.5f + (D(ratio) + 1.0f) * 0.5f);   // 0x449d23: fcos, then fmulp by (1 - ratio)
    const double mag = x87_sqrt((D(bk) * bk) * (D(slip_angle) * slip_angle) + D(lsr) * lsr);
    const float magf = (float)mag;                      // +0x10
    float inv = 1.0f;                                   // +0x24
    if (fabs(mag) > k_eps) inv = (float)(1.0f / D(magf));
    const float Fy = (float)((((((D(res) * slip_angle) * inv) * bk) * den + cam_norm) * scale) * Fz * mu_lat);   // +0x38
    const float Fx = (float)(((((D(res) * lsr) * inv) * scale) * Fz) * mu_long);                               // +0x2c
    const float side = Ub(w->droop_offset.x) > 0x80000000u ? -1.0f : 1.0f;   // +0x10 left / right

    // ---- aligning torque (steering feedback), and the force ratios
    w->aligning_torque = (float)(D(Fy) * k_align_lat + w->aligning_torque);
    w->aligning_torque = (float)(D(w->aligning_torque) / (D(w->slide) + 1.0f));
    w->aligning_torque = (float)(D(side) * Fx * k_align_long + w->aligning_torque);
    if (w->on_ground) {
        w->lat_force_ratio = (float)(D(Fy) / mufz);
        w->long_force_ratio = (float)(D(Fx) / (D(Fz) * mu_long));
    }
    w->lat_force_filtered = (float)(D(w->lat_force_filtered) * (float)0.9f + D(Fy) * (float)0.1f);

    // ---- the tyre force in the world, capped at 10,000 N; its rolling part acts back on the wheel
    const double a = -(D(w->tire_load_factor) * Fy);
    const double b = -(D(w->tire_load_factor) * Fx);
    P3 T;                                               // +0x3c
    T.x = (float)(D(L.x) * a + D(H.x) * b);
    T.y = (float)(D(L.y) * a + D(H.y) * b);
    const double tz = a * L.z + b * H.z;
    T.z = (float)tz;
    const double t2 = (tz * T.z + D(T.y) * T.y) + D(T.x) * T.x;
    const float t2f = (float)t2;                        // +0x10
    if (t2 > 100000000.0f) {                            // test ah,0x41
        const double k = 1.0f / x87_sqrt(D(t2f));
        T.x = (float)(D(T.x) * k);
        T.y = (float)(D(T.y) * k);
        T.z = (float)(k * T.z);
        T.x = (float)(D(T.x) * 10000.0f);
        T.y = (float)(D(T.y) * 10000.0f);
        T.z = (float)(D(T.z) * 10000.0f);
    }
    torque = (float)(D(torque) - ((D(T.y) * H.y + D(T.z) * H.z) + D(T.x) * H.x));

    // the part along the suspension axis goes into the spring (next tick's load), not the chassis
    const P3& sa = w->susp_axis_world;
    const double nd = -((D(sa.z) * T.z + D(sa.y) * T.y) + D(sa.x) * T.x);
    w->chassis_tire_force.x = (float)(D(sa.x) * nd + T.x);
    w->chassis_tire_force.y = (float)(D(sa.y) * nd + T.y);
    w->chassis_tire_force.z = (float)(nd * sa.z + T.z);

    // the velocity at the wheel, scaled (1 at 57.8 m/s) and capped at unit length: the surface drag's direction
    P3 Vn;                                              // +0x10
    Vn.x = (float)(D(V.x) * k_vel_scale);
    Vn.y = (float)(D(V.y) * k_vel_scale);
    const double vz = D(V.z) * k_vel_scale;
    Vn.z = (float)vz;
    const double v2 = (vz * Vn.z + D(Vn.y) * Vn.y) + D(Vn.x) * Vn.x;
    const float v2f = (float)v2;                        // +0x2c
    if (v2 > 1.0f) {                                    // test ah,0x41
        const double k = 1.0f / x87_sqrt(D(v2f));
        Vn.x = (float)(D(Vn.x) * k);
        Vn.y = (float)(D(Vn.y) * k);
        Vn.z = (float)(k * Vn.z);
    }
    memcpy(&w->tire_force, &T, 12);
    P3 C;                                               // +0x3c, integer copies
    memcpy(&C, &w->chassis_tire_force, 12);
    PhobDyno_ApplyForce(car, 0, &C, &w->droop_point_world);

    // ---- the surface's drag (sand, grass...), applied 30 % of the way from the centre to the wheel
    P3 Dg;                                              // +0x3c
    Dg.x = (float)-(((D(Vn.x) * scale) * Fz) * sdrag);
    Dg.y = (float)-(((D(Vn.y) * scale) * Fz) * sdrag);
    Dg.z = (float)-(((D(Vn.z) * scale) * Fz) * sdrag);
    const P3& pos = car->frame.pos;
    P3 Pd;                                              // +0xc4
    Pd.x = (float)(D(w->droop_point_world.x) * (float)0.3f + D(pos.x) * (float)0.7f);
    Pd.y = (float)(D(w->droop_point_world.y) * (float)0.3f + D(pos.y) * (float)0.7f);
    Pd.z = (float)(D(w->droop_point_world.z) * (float)0.3f + D(pos.z) * (float)0.7f);
    PhobDyno_ApplyForce(car, 0, &Dg, &Pd);

    // ---- realism 0: a sliding wheel pushes the car sideways back, and slows it, at its centre
    if (w->on_ground && car->realism == 0) {
        double c = D(w->slide) - 0.5f;
        c = !(0.0f >= c) ? c : 0.0f;
        c = c * 0.5f;
        c = 1.0f > c ? c : 1.0f;
        const float cf = (float)c;                      // +0x2c
        const float cap = w->surface != 0 ? Fb(0x3dcccccdu) : 1.0f;   // +0x10
        const float gain = D(cf) > cap ? cap : cf;      // +0x2c
        const double q = ((D(M[0]) * Dg.x + D(M[2]) * Dg.z) + D(M[1]) * Dg.y) * gain;
        const float qf = (float)q;                      // +0x2c
        P3 Q;                                           // +0x10
        Q.x = (float)(q * M[0] * N.y);
        Q.y = (float)(D(M[1]) * qf * N.y);
        Q.z = (float)(D(M[2]) * qf * N.y);
        P3 Vc;                                          // +0x2c, integer copies
        memcpy(&Vc, &car->velocity, 12);
        const double c2 = (D(Vc.y) * Vc.y + D(Vc.z) * Vc.z) + D(Vc.x) * Vc.x;
        const float c2f = (float)c2;                    // +0x28
        if (c2 > (float)0.1f) {                                // test ah,0x41
            const double k = 1.0f / x87_sqrt(D(c2f));
            Vc.x = (float)(D(Vc.x) * k);
            Vc.y = (float)(D(Vc.y) * k);
            Vc.z = (float)(k * Vc.z);
            const double wv = fabs((D(M[2]) * Q.z + D(M[0]) * Q.x) + D(M[1]) * Q.y) * -2.5f;
            Q.x = (float)(D(Vc.x) * wv + Q.x);
            Q.y = (float)(D(Vc.y) * wv + Q.y);
            Q.z = (float)(wv * Vc.z + Q.z);
        }
        PhobDyno_ApplyForce(car, 0, &Q, &car->frame.pos);
    }

    // ---- integrate the spin
    const double dom = (1.0f - D(lock)) * w->inv_inertia * torque * (float)0.016f;
    w->fx_flags |= w->broken ? 0x40 : 0;
    w->omega = (float)(dom + w->omega);
    w->rpm = (float)(D(w->omega) * k_rad_to_rpm);
}

// It writes the wheel and, through PhobDyno's force / torque / impulse helpers, the car it belongs to
// (the wheel is embedded in it). Random(2) (road noise) is an input, fed automatically; TerrainGetHeight,
// HackPaveTheWorld and PhysicsIsDamageOn only read. Nothing outside the car is written.
static void fp_wheel_update(Footprint& f, Wheel* w, Edx, Car* car) {
    f.object(car, "car");
    const uint8_t* p = (const uint8_t*)w;
    if (p < (const uint8_t*)car || p >= (const uint8_t*)car + sizeof(Car)) f.add(w, sizeof(Wheel), "wheel");
}
PORT_FN(0x00448a60, "Wheel::Update", Wheel_Update, fp_wheel_update)
