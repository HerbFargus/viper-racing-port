// phys_drivetrain.cpp -- M3 3.3 group P1: the drivetrain parts (physics:carpart.obj), rewritten.
//
// The drivetrain is a graph of TorqueInput objects: Engine -> Clutch -> Transmission -> Differential(s) ->
// Wheels. A TorqueInput has two virtual methods, GetRPM (vtable +0) and ApplyTorque (+4); each part's
// ApplyTorque passes torque on to the next part's through the vtable, so those calls stay virtual here (the
// other groups' rewrites of Engine / Wheel, hooked at their addresses, are what run). Every part is embedded
// in the Car (Engine +3068, Clutch +3196, Transmission +3224, centre/front/rear diffs +3332/+3364/+3396,
// wheels +1364), and each part's owner (Setup's PhobDyno) is that Car.
//
// Written from the v1.0 disassembly: the same sums in the same grouping, register values as double, stored
// values as float, every call in the original's order with the same arguments, and floats the original moves
// with integer instructions copied as bits. GetRPM returns the unrounded ST0 (double) as the originals do.
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
#define U32(x) (*(uint32_t*)&(x))                   // a float field, as the bits an integer move copies

// the unused edx of a __thiscall received as __fastcall (an int, as in phys_sphere.cpp: test/fuzz.h sizes it)
typedef int Edx;

static inline uint32_t bits(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static inline float as_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// ---- constants, exactly as the original's (several aren't the float nearest their decimal) --------------
static const float k_rpm_to_rad = 0x1.aceeap-4f;    // 0x3dd67750  0.10471976 (2 pi / 60)
static const float k_dt = 0x1.0624dep-6f;           // 0x3c83126f  0.016 (the physics step)
static const float k_one_over_dt = 0x1.f3fffep+5f;  // 0x4279ffff  62.499996 (not 62.5)
static const float k_clutch_stiff = 0x1.47ae14p-9f; // 0x3b23d70a  0.0025
static const float k_keep = 0x1.b33334p-1f;         // 0x3f59999a  0.85
static const float k_new = 0x1.33333p-3f;           // 0x3e199998  0.14999998 (not 0.15f, 0x3e19999a)
static const float k_ramp = 0x1.0624dep-3f;         // 0x3e03126f  0.128 (auto-clutch step)
static const float k_rev_frac = 0x1.666666p-1f;     // 0x3f333333  0.7
static const float k_rpm_scale = 0x1.ed7292p-14f;   // 0x38f6b949  1/8500
static const float k_ratio_eps = 0x1p-23f;          // 0x34000000  FLT_EPSILON
static const uint32_t k_one_bits = 0x3f800000u;     // 1.0f, as stored with integer moves
static const uint32_t k_half_bits = 0x3f000000u;    // 0.5f
static const uint32_t k_point2_bits = 0x3e4ccccdu;  // 0.2f
static const uint32_t k_point4_bits = 0x3ecccccdu;  // 0.4f
static const uint32_t k_400_bits = 0x43c80000u;     // 400.0f
static const uint32_t k_minus400_bits = 0xc3c80000u;// -400.0f

// ---- layouts (anonymous: other groups may define their own Engine until these move to phys_types.h) ------
namespace {

// vtable slots, in bytes: 0 GetRPM() -> float (ST0), 4 ApplyTorque(float)
struct TorqueInput { void** vtable; };

// Differential (32 bytes, vtable 0x4dc818): splits torque x ratio / 2 to each side, plus a limited-slip term
struct Differential : TorqueInput {
    TorqueInput* input_a;              // +0x04  left wheel / front diff
    TorqueInput* input_b;              // +0x08  right wheel / rear diff
    float rpm_a;                       // +0x0c  ApplyTorque's cache of input_a->GetRPM()
    float rpm_b;                       // +0x10
    PhobDyno* owner;                   // +0x14  Setup arg 1 (the car)
    float lock_stiffness;              // +0x18  Setup arg 3: N m per rpm of a/b difference
    float ratio;                       // +0x1c  Setup arg 2: final drive (1 for the centre diff)
};
static_assert(offsetof(Differential, owner) == 0x14 && offsetof(Differential, ratio) == 0x1c &&
              sizeof(Differential) == 32, "Differential");

// Clutch (28 bytes, no vtable): couples input (the engine) and output (the gearbox, or a plane's propeller)
struct Clutch {
    float engagement;                  // +0x00  0..1, SetClutch
    TorqueInput* input;                // +0x04  Connect arg 1: the Engine
    TorqueInput* output;               // +0x08  Connect arg 2: the Transmission (a plane's Propeller)
    PhobDyno* owner;                   // +0x0c  Setup arg 1
    float max_torque;                  // +0x10  Setup arg 2
    float stiffness;                   // +0x14  max_torque x 0.0025: torque per rpm of slip
    float torque;                      // +0x18  smoothed: 0.15 new + 0.85 old
};
static_assert(offsetof(Clutch, owner) == 0x0c && offsetof(Clutch, torque) == 0x18 && sizeof(Clutch) == 28, "Clutch");

// Shaft (24 bytes, vtable 0x4dc820): a spinning inertia whose reaction torque goes into the owner
struct Shaft : TorqueInput {
    PhobDyno* owner;                   // +0x04  Setup arg 1
    float inertia;                     // +0x08  kg m^2, Setup arg 2
    float inv_inertia;                 // +0x0c  1 / inertia
    float rpm;                         // +0x10
    float drag;                        // +0x14  N m per rpm, Setup arg 3
};
static_assert(offsetof(Shaft, rpm) == 0x10 && sizeof(Shaft) == 24, "Shaft");

// Engine (128 bytes, vtable 0x4dc830; ported by another group): only the fields the gearbox reads
struct Engine : Shaft {
    uint8_t _18[0x2c - 0x18];          // +0x18  block ThermalMass (part)
    float throttle;                    // +0x2c  SetThrottle
    float idle_throttle;               // +0x30
    float idle_rpm;                    // +0x34
    float smoothed_throttle;           // +0x38
    float redline;                     // +0x3c
    uint8_t cranking;                  // +0x40
    uint8_t stalled;                   // +0x41
    uint8_t _42[128 - 0x42];
};
static_assert(offsetof(Engine, idle_rpm) == 0x34 && offsetof(Engine, redline) == 0x3c &&
              offsetof(Engine, stalled) == 0x41 && sizeof(Engine) == 128, "Engine");

// Transmission (108 bytes, derives Shaft, vtable 0x4dc828)
struct Transmission : Shaft {
    float ratio;                       // +0x18  gear_ratios[engaged_gear-1], -gear_ratios[0] in reverse, 0 in N
    int32_t engaged_gear;              // +0x1c  the gear in mesh (-1 R, 0 N, 1..)
    int32_t num_gears;                 // +0x20  Setup arg 4
    int32_t gear;                      // +0x24  requested (SetGear)
    float input_rpm;                   // +0x28  ApplyTorque: output rpm x ratio (own rpm in neutral)
    float gear_ratios[8];              // +0x2c  indexed gear-1 (read as [this + 0x28 + 4 gear])
    TorqueInput* output;               // +0x4c  a Differential
    Engine* engine;                    // +0x50  Setup arg 6
    Clutch* clutch_unit;               // +0x54  Setup arg 7
    uint8_t in_gear, _59[3];           // +0x58  engaged_gear != 0
    float clutch;                      // +0x5c  driver clutch (SetClutch)
    float clutch_applied;              // +0x60  what Update hands the Clutch
    float throttle;                    // +0x64  driver throttle (SetThrottle)
    int32_t shift_state;               // +0x68  auto-clutch state machine, 0..5 (ctor 4)
};
static_assert(offsetof(Transmission, ratio) == 0x18 && offsetof(Transmission, input_rpm) == 0x28 &&
              offsetof(Transmission, gear_ratios) == 0x2c && offsetof(Transmission, output) == 0x4c &&
              offsetof(Transmission, in_gear) == 0x58 && offsetof(Transmission, shift_state) == 0x68 &&
              sizeof(Transmission) == 108, "Transmission");

}  // namespace

// ---- the game's functions these call (non-virtual calls by address) --------------------------------------
typedef void*(__fastcall* ShaftCtor_t)(void*, void*);
typedef void(__fastcall* ShaftSetup_t)(void*, void*, PhobDyno*, uint32_t inertia, uint32_t drag);
typedef double(__fastcall* ShaftGetRPM_t)(void*, void*);
typedef void(__fastcall* ShaftApplyTorque_t)(void*, void*, uint32_t torque);
typedef double(__fastcall* ShaftGetInertialTorque_t)(void*, void*, uint32_t rpm);
typedef void(__fastcall* PhobDynoApplyTorque_t)(PhobDyno*, void*, const P3*);
typedef void(__fastcall* EngineSetThrottle_t)(void*, void*, uint32_t);
typedef double(__fastcall* EngineGetNetTorque_t)(void*, void*, float rpm, float throttle);
typedef void(__fastcall* ClutchSetClutch_t)(void*, void*, uint32_t);
typedef void(__fastcall* TransmissionSetGear_t)(void*, void*, int);
static const ShaftCtor_t ShaftCtorOrig = (ShaftCtor_t)0x00445e20;
static const ShaftSetup_t ShaftSetupOrig = (ShaftSetup_t)0x00445e40;
static const ShaftGetRPM_t ShaftGetRPMOrig = (ShaftGetRPM_t)0x00445ea0;
static const ShaftApplyTorque_t ShaftApplyTorqueOrig = (ShaftApplyTorque_t)0x00445eb0;
static const ShaftGetInertialTorque_t ShaftGetInertialTorqueOrig = (ShaftGetInertialTorque_t)0x00445e80;
static const PhobDynoApplyTorque_t PhobDynoApplyTorque = (PhobDynoApplyTorque_t)0x00444d20;
static const EngineSetThrottle_t EngineSetThrottle = (EngineSetThrottle_t)0x004467f0;
static const EngineGetNetTorque_t EngineGetNetTorque = (EngineGetNetTorque_t)0x004469d0;
static const ClutchSetClutch_t ClutchSetClutchOrig = (ClutchSetClutch_t)0x00445e10;
static const TransmissionSetGear_t TransmissionSetGearOrig = (TransmissionSetGear_t)0x00446000;

// virtual calls through the part's own vtable
static inline double GetRPM(TorqueInput* t) { return VFN(t, 0, double)(t, 0); }
static inline void ApplyTorque(TorqueInput* t, float torque) { VFN(t, 4, void, float)(t, 0, torque); }
static inline void ApplyTorqueBits(TorqueInput* t, uint32_t torque) { VFN(t, 4, void, uint32_t)(t, 0, torque); }

// ---- footprints ------------------------------------------------------------------------------------------
// The parts are embedded in the Car (the base class ends at 3768; every part lies below that), so a part
// whose owner is the car is covered by f.object(car). Anything reached that lies outside it -- a plane's
// Propeller (heap, 36 bytes, vtable 0x4dc8c0), or a part never set up -- is added on its own.
enum { CAR_SIZE = 3768, PROPELLER_SIZE = 36 };
static bool in_car(const PhobDyno* car, const void* p) {
    return car && (const uint8_t*)p >= (const uint8_t*)car && (const uint8_t*)p < (const uint8_t*)car + CAR_SIZE;
}
static void fp_owner(Footprint& f, PhobDyno* car, void* self, uint32_t size, const char* what) {
    if (car) f.object(car, "car");
    if (!in_car(car, self)) f.add(self, size, what);
}
static void fp_next(Footprint& f, PhobDyno* car, TorqueInput* next) {
    if (!next || in_car(car, next)) return;
    if (next->vtable == (void**)0x004dc8c0) f.add(next, PROPELLER_SIZE, "propeller");
    else f.object(next, "part");
}

// ==== Differential ==========================================================================================

// Differential::Differential (0x445c00)
static Differential* __fastcall Differential_Ctor(Differential* self, Edx) {
    self->vtable = (void**)0x004dc818;
    self->input_b = 0;
    self->input_a = 0;
    U32(self->ratio) = 0;
    U32(self->lock_stiffness) = 0;
    U32(self->rpm_b) = 0;
    U32(self->rpm_a) = 0;
    self->owner = 0;
    return self;
}
static void fp_diff_self(Footprint& f, Differential* self, Edx) { f.add(self, sizeof(Differential), "differential"); }
PORT_FN(0x00445c00, "Differential::Differential", Differential_Ctor, fp_diff_self)

// Differential::Setup(PhobDyno*, float ratio, float lock_stiffness) (0x445c20): bit copies
static void __fastcall Differential_Setup(Differential* self, Edx, PhobDyno* owner, float ratio, float lock) {
    self->owner = owner;
    memcpy(&self->ratio, &ratio, 4);
    memcpy(&self->lock_stiffness, &lock, 4);
}
static void fp_diff_setup(Footprint& f, Differential* self, Edx, PhobDyno*, float, float) {
    f.add(self, sizeof(Differential), "differential");
}
PORT_FN(0x00445c20, "Differential::Setup", Differential_Setup, fp_diff_setup)

// Differential::Connect(a, b) (0x445c40)
static void __fastcall Differential_Connect(Differential* self, Edx, TorqueInput* a, TorqueInput* b) {
    self->input_a = a;
    self->input_b = b;
}
static void fp_diff_connect(Footprint& f, Differential* self, Edx, TorqueInput*, TorqueInput*) {
    f.add(self, sizeof(Differential), "differential");
}
PORT_FN(0x00445c40, "Differential::Connect", Differential_Connect, fp_diff_connect)

// Differential::GetRPM (0x445c60, virtual): ((b + a) x ratio) x 0.5, a stored, b and the result in ST0
static double __fastcall Differential_GetRPM(Differential* self, Edx) {
    float a = (float)GetRPM(self->input_a);
    double b = GetRPM(self->input_b);
    return ((b + a) * self->ratio) * 0.5f;
}
static void fp_diff_getrpm(Footprint&, Differential*, Edx) {}   // writes nothing; the inputs' GetRPM are const
PORT_FN(0x00445c60, "Differential::GetRPM", Differential_GetRPM, fp_diff_getrpm)

// Differential::ApplyTorque(float) (0x445c90, virtual): each side gets torque x ratio / 2; the lock term
// (rpm_b - rpm_a) x stiffness goes to a (unrounded, from the register) and comes off b (rounded, as stored)
static void __fastcall Differential_ApplyTorque(Differential* self, Edx, float torque) {
    self->rpm_a = (float)GetRPM(self->input_a);
    self->rpm_b = (float)GetRPM(self->input_b);
    float half = (float)((D(self->ratio) * torque) * 0.5f);
    double lock = (D(self->rpm_b) - self->rpm_a) * self->lock_stiffness;
    float lock_f = (float)lock;
    ApplyTorque(self->input_a, (float)(lock + half));
    ApplyTorque(self->input_b, (float)(D(half) - lock_f));
}
static void fp_diff_apply(Footprint& f, Differential* self, Edx, float) {
    fp_owner(f, self->owner, self, sizeof(Differential), "differential");
    fp_next(f, self->owner, self->input_a);
    fp_next(f, self->owner, self->input_b);
}
PORT_FN(0x00445c90, "Differential::ApplyTorque", Differential_ApplyTorque, fp_diff_apply)

// ==== Clutch ================================================================================================

// Clutch::Clutch (0x445d00): max_torque and stiffness are left as they were
static Clutch* __fastcall Clutch_Ctor(Clutch* self, Edx) {
    self->output = 0;
    self->input = 0;
    self->owner = 0;
    U32(self->engagement) = 0;
    U32(self->torque) = 0;
    return self;
}
static void fp_clutch_self(Footprint& f, Clutch* self, Edx) { f.add(self, sizeof(Clutch), "clutch"); }
PORT_FN(0x00445d00, "Clutch::Clutch", Clutch_Ctor, fp_clutch_self)

// Clutch::Setup(PhobDyno*, float max_torque) (0x445d20)
static void __fastcall Clutch_Setup(Clutch* self, Edx, PhobDyno* owner, float max_torque) {
    float stiffness = (float)(D(max_torque) * k_clutch_stiff);
    self->owner = owner;
    memcpy(&self->max_torque, &max_torque, 4);
    self->stiffness = stiffness;
}
static void fp_clutch_setup(Footprint& f, Clutch* self, Edx, PhobDyno*, float) {
    f.add(self, sizeof(Clutch), "clutch");
}
PORT_FN(0x00445d20, "Clutch::Setup", Clutch_Setup, fp_clutch_setup)

// Clutch::Connect(input, output) (0x445d40)
static void __fastcall Clutch_Connect(Clutch* self, Edx, TorqueInput* input, TorqueInput* output) {
    self->input = input;
    self->output = output;
}
static void fp_clutch_connect(Footprint& f, Clutch* self, Edx, TorqueInput*, TorqueInput*) {
    f.add(self, sizeof(Clutch), "clutch");
}
PORT_FN(0x00445d40, "Clutch::Connect", Clutch_Connect, fp_clutch_connect)

// Clutch::Update (0x445d60): slip torque (output rpm - input rpm) x stiffness, clamped to
// +-max_torque x engagement, smoothed, and applied as engagement^2 x torque: + to the input, - to the output
static void __fastcall Clutch_Update(Clutch* self, Edx) {
    float limit = (float)(D(self->max_torque) * self->engagement);
    TorqueInput* input = self->input;                   // read before the output's GetRPM
    float out_rpm = (float)GetRPM(self->output);
    double slip = (D(out_rpm) - GetRPM(input)) * self->stiffness;
    float t = (float)slip;
    if (slip > limit) {                                 // fcom; test ah,0x41; jne
        t = limit;
    } else {
        double neg = -D(limit);
        float neg_f = (float)neg;
        if (neg > t) t = neg_f;                          // fcom [t]; test ah,0x41; jne
    }
    double smoothed = D(self->torque) * k_keep + D(t) * k_new;
    self->torque = (float)smoothed;
    float applied = (float)(smoothed * (D(self->engagement) * self->engagement));
    ApplyTorqueBits(self->input, bits(applied));
    ApplyTorque(self->output, (float)-D(applied));
}
static void fp_clutch_update(Footprint& f, Clutch* self, Edx) {
    fp_owner(f, self->owner, self, sizeof(Clutch), "clutch");
    fp_next(f, self->owner, self->input);
    fp_next(f, self->owner, self->output);
}
PORT_FN(0x00445d60, "Clutch::Update", Clutch_Update, fp_clutch_update)

// Clutch::SetClutch(float) (0x445e10)
static void __fastcall Clutch_SetClutch(Clutch* self, Edx, float engagement) {
    memcpy(&self->engagement, &engagement, 4);
}
static void fp_clutch_set(Footprint& f, Clutch* self, Edx, float) { f.add(self, sizeof(Clutch), "clutch"); f.pure = true; }
PORT_FN(0x00445e10, "Clutch::SetClutch", Clutch_SetClutch, fp_clutch_set)

// ==== Shaft =================================================================================================

// Shaft::Shaft (0x445e20)
static Shaft* __fastcall Shaft_Ctor(Shaft* self, Edx) {
    self->vtable = (void**)0x004dc820;
    U32(self->drag) = 0;
    U32(self->rpm) = 0;
    U32(self->inv_inertia) = 0;
    U32(self->inertia) = 0;
    self->owner = 0;
    return self;
}
static void fp_shaft_self(Footprint& f, Shaft* self, Edx) { f.add(self, sizeof(Shaft), "shaft"); }
PORT_FN(0x00445e20, "Shaft::Shaft", Shaft_Ctor, fp_shaft_self)

// Shaft::Setup(PhobDyno*, float inertia, float drag) (0x445e40)
static void __fastcall Shaft_Setup(Shaft* self, Edx, PhobDyno* owner, float inertia, float drag) {
    float inv = (float)(1.0f / D(inertia));
    memcpy(&self->inertia, &inertia, 4);
    self->owner = owner;
    memcpy(&self->drag, &drag, 4);
    self->inv_inertia = inv;
}
static void fp_shaft_setup(Footprint& f, Shaft* self, Edx, PhobDyno*, float, float) {
    f.add(self, sizeof(Shaft), "shaft");
}
PORT_FN(0x00445e40, "Shaft::Setup", Shaft_Setup, fp_shaft_setup)

// Shaft::GetInertialTorque(float rpm) (0x445e80): sets rpm (bits) and returns -(drag x rpm) in ST0
static double __fastcall Shaft_GetInertialTorque(Shaft* self, Edx, float rpm) {
    double t = D(self->drag) * rpm;
    memcpy(&self->rpm, &rpm, 4);
    return -t;
}
static void fp_shaft_inertial(Footprint& f, Shaft* self, Edx, float) { f.add(self, sizeof(Shaft), "shaft"); f.pure = true; }
PORT_FN(0x00445e80, "Shaft::GetInertialTorque", Shaft_GetInertialTorque, fp_shaft_inertial)

// Shaft::GetRPM (0x445ea0, virtual)
static double __fastcall Shaft_GetRPM(Shaft* self, Edx) { return self->rpm; }
static void fp_shaft_getrpm(Footprint& f, Shaft*, Edx) { f.pure = true; }
PORT_FN(0x00445ea0, "Shaft::GetRPM", Shaft_GetRPM, fp_shaft_getrpm)

// Shaft::ApplyTorque(float) (0x445eb0, virtual): rpm += (T - drag rpm) / I / (2 pi / 60) x dt; the reaction
// (the change in angular momentum over dt) goes into the owner about its frame's row 2 (the spin axis)
static void __fastcall Shaft_ApplyTorque(Shaft* self, Edx, float torque) {
    double old_rpm = self->rpm;
    PhobDyno* owner = self->owner;
    self->rpm = (float)((((D(torque) - D(self->drag) * self->rpm) * self->inv_inertia) / k_rpm_to_rad) * k_dt +
                        self->rpm);
    P3 axis;
    memcpy(&axis, &owner->frame.rot.m[6], 12);          // copied with integer moves
    double k = (((D(self->rpm) - old_rpm) * k_rpm_to_rad) * self->inertia) * k_one_over_dt;
    P3 t;
    t.x = (float)(D(axis.x) * k);
    t.y = (float)(D(axis.y) * k);
    t.z = (float)(k * axis.z);
    PhobDynoApplyTorque(owner, 0, &t);
}
static void fp_shaft_apply(Footprint& f, Shaft* self, Edx, float) { fp_owner(f, self->owner, self, sizeof(Shaft), "shaft"); }
PORT_FN(0x00445eb0, "Shaft::ApplyTorque", Shaft_ApplyTorque, fp_shaft_apply)

// Shaft::GetDragTorque(float rpm) (0x445f50): drag x rpm in ST0
static double __fastcall Shaft_GetDragTorque(Shaft* self, Edx, float rpm) { return D(self->drag) * rpm; }
static void fp_shaft_drag(Footprint& f, Shaft*, Edx, float) { f.pure = true; }
PORT_FN(0x00445f50, "Shaft::GetDragTorque", Shaft_GetDragTorque, fp_shaft_drag)

// ==== Transmission ==========================================================================================

// Transmission::Transmission (0x445f60): the gear ratios, engine, clutch_unit and throttle are left as they were
static Transmission* __fastcall Transmission_Ctor(Transmission* self, Edx) {
    ShaftCtorOrig(self, 0);
    self->vtable = (void**)0x004dc828;
    U32(self->ratio) = 0;
    self->num_gears = 0;
    self->engaged_gear = 0;
    U32(self->input_rpm) = 0;
    self->output = 0;
    self->in_gear = 0;
    U32(self->clutch_applied) = 0;
    U32(self->clutch) = 0;
    self->shift_state = 4;
    self->gear = 0;
    return self;
}
static void fp_trans_self(Footprint& f, Transmission* self, Edx) { f.add(self, sizeof(Transmission), "transmission"); }
PORT_FN(0x00445f60, "Transmission::Transmission", Transmission_Ctor, fp_trans_self)

// Transmission::Connect(output) (0x445fa0)
static void __fastcall Transmission_Connect(Transmission* self, Edx, TorqueInput* output) { self->output = output; }
static void fp_trans_connect(Footprint& f, Transmission* self, Edx, TorqueInput*) {
    f.add(self, sizeof(Transmission), "transmission");
}
PORT_FN(0x00445fa0, "Transmission::Connect", Transmission_Connect, fp_trans_connect)

// Transmission::Setup(owner, inertia, drag, num_gears, ratios, engine, clutch) (0x445fb0): the ratios are
// copied (integer moves) without a bound -- the car setup counts at most 7
static void __fastcall Transmission_Setup(Transmission* self, Edx, PhobDyno* owner, float inertia, float drag,
                                          int num_gears, const float* ratios, Engine* engine, Clutch* clutch) {
    ShaftSetupOrig(self, 0, owner, bits(inertia), bits(drag));
    self->num_gears = num_gears;
    if (num_gears > 0) {
        const uint32_t* src = (const uint32_t*)ratios;
        uint32_t* dst = (uint32_t*)self->gear_ratios;
        int n = num_gears;
        do { *dst++ = *src++; } while (--n);
    }
    self->engine = engine;
    self->clutch_unit = clutch;
}
static void fp_trans_setup(Footprint& f, Transmission* self, Edx, PhobDyno*, float, float, int num_gears, const float*,
                           Engine*, Clutch*) {
    uint32_t end = 0x2c + 4 * (uint32_t)(num_gears > 0 ? num_gears : 0);
    f.add(self, end > sizeof(Transmission) ? end : sizeof(Transmission), "transmission");
}
PORT_FN(0x00445fb0, "Transmission::Setup", Transmission_Setup, fp_trans_setup)

// Transmission::SetGear(int) (0x446000)
static void __fastcall Transmission_SetGear(Transmission* self, Edx, int gear) { self->gear = gear; }
static void fp_trans_setgear(Footprint& f, Transmission* self, Edx, int) { f.add(self, sizeof(Transmission), "transmission"); f.pure = true; }
PORT_FN(0x00446000, "Transmission::SetGear", Transmission_SetGear, fp_trans_setgear)

// the ratio slot the original indexes as [this + 0x28 + 4 gear] (gear_ratios[gear - 1], unchecked)
static inline uint32_t& ratio_slot(Transmission* self, int gear) { return *(uint32_t*)((uint8_t*)self + 0x28 + 4 * gear); }

// Transmission::Update(bool automatic) (0x446010): the auto-clutch state machine, then the throttle to the
// Engine, the clutch to the Clutch, and the ratio of the gear in mesh
static void __fastcall Transmission_Update(Transmission* self, Edx, uint8_t automatic) {
    uint32_t throttle = U32(self->throttle);                       // [esp+4]
    int g = self->gear;
    uint32_t r = g > 0 ? ratio_slot(self, g) : U32(self->ratio);
    float out_rpm = (float)(GetRPM(self->output) * as_float(r));  // the requested gear's input speed
    float eng_rpm = (float)GetRPM(self->engine);
    Engine* engine = self->engine;                                 // ecx from here to SetThrottle
    float redline = engine->redline;                               // (copied with integer moves)
    float idle = engine->idle_rpm;
    float idle_1000 = (float)(D(idle) + 1000.0f);
    if (!automatic) {
        self->engaged_gear = self->gear;
        U32(self->clutch_applied) = U32(self->clutch);
        self->shift_state = 0;
    } else {
        int gear = self->gear;                                     // edx
        if (gear <= 1 && self->engaged_gear != gear && self->shift_state == 4) {
            self->shift_state = 0;
            self->engaged_gear = gear;
        }
        if (engine->stalled) self->shift_state = 0;
        float rev_limit = (float)(D(redline) * k_rev_frac);
        switch ((uint32_t)self->shift_state) {
        case 0:                                                     // settled: follow the driver's clutch
            U32(self->clutch_applied) = U32(self->clutch);
            if (gear != self->engaged_gear) {
                if (self->engaged_gear > 0 && gear > 0) {
                    self->shift_state = 1;
                } else {
                    U32(self->clutch_applied) = 0;
                    self->shift_state = 4;
                    self->engaged_gear = gear;
                }
            }
            break;
        case 1:                                                     // clutch out
            self->clutch_applied = (float)(D(self->clutch_applied) - k_ramp);
            if (self->engaged_gear < gear && (int32_t)U32(self->clutch_applied) < (int32_t)k_half_bits)
                self->engaged_gear = 0;
            if (U32(self->clutch_applied) > 0x80000000u) {          // below -0 (bits, unsigned)
                U32(self->clutch_applied) = 0;
                self->shift_state = 3;
                if (self->engaged_gear <= gear) self->shift_state = 2;
            }
            throttle = 0;
            break;
        case 2:                                                     // upshift through neutral
            throttle = 0;
            U32(self->clutch_applied) = 0;
            self->shift_state = 4;
            break;
        case 3:                                                     // downshift: blip to match revs
            throttle = k_one_bits;
            self->engaged_gear = 0;
            U32(self->clutch_applied) = k_one_bits;
            if (eng_rpm > out_rpm || !(redline > out_rpm)) {       // test ah,0x41; je (twice)
                U32(self->clutch_applied) = 0;
                self->shift_state = 4;
            }
            break;
        case 4:                                                     // clutch back in
            self->clutch_applied = (float)(D(self->clutch_applied) + k_ramp);
            self->engaged_gear = gear;
            if ((int32_t)U32(self->clutch_applied) > (int32_t)k_one_bits) {
                U32(self->clutch_applied) = k_one_bits;
                self->shift_state = 0;
            }
            if (gear > 1) throttle = 0;
            break;
        case 5:                                                     // never set here
            if (eng_rpm >= rev_limit || (int32_t)throttle <= (int32_t)k_point4_bits) {   // test ah,1; je
                self->shift_state = 4;
            } else if (!(eng_rpm > idle_1000)) {                    // test ah,0x41; jne
                U32(self->clutch_applied) = 0;
            } else {
                U32(self->clutch_applied) = k_point2_bits;
            }
            break;
        default:
            break;
        }
        // anti-stall: below idle + 1000, cap the clutch at (rpm - idle) / 1000 (unless blipping)
        if (!(eng_rpm >= idle_1000) && self->shift_state != 3) {   // test ah,1; je
            double q = (D(eng_rpm) - idle) / (D(idle_1000) - idle);
            throttle = U32(self->throttle);
            double m = !(0.0 >= q) ? q : 0.0;                        // fcom st(1); test ah,1
            double ca = self->clutch_applied;
            double res = (ca > m) ? m : ca;                          // fcom st(1); test ah,0x41; je
            self->clutch_applied = (float)res;
        }
    }
    EngineSetThrottle(engine, 0, throttle);
    ClutchSetClutchOrig(self->clutch_unit, 0, U32(self->clutch_applied));
    int e = self->engaged_gear;
    if (e > 0) U32(self->ratio) = ratio_slot(self, e);
    else if (e < 0) self->ratio = (float)-D(self->gear_ratios[0]);
    else U32(self->ratio) = 0;
    self->in_gear = e != 0;
}
static void fp_trans_update(Footprint& f, Transmission* self, Edx, uint8_t) {
    fp_owner(f, self->owner, self, sizeof(Transmission), "transmission");
    if (!in_car(self->owner, self->engine)) f.object(self->engine, "engine");
    if (!in_car(self->owner, self->clutch_unit)) f.add(self->clutch_unit, sizeof(Clutch), "clutch");
}
PORT_FN(0x00446010, "Transmission::Update", Transmission_Update, fp_trans_update)

// Transmission::SetGearAuto (0x446300): pick the gear with the most wheel torque at the current speed (full
// throttle net torque x ratio, zero past redline - 200), with an upshift allowance on the lower gears
static void __fastcall Transmission_SetGearAuto(Transmission* self, Edx) {
    uint32_t best = 0;                                              // [esp+0x24]
    int pick = self->engaged_gear;                                  // edi
    if (pick <= 0 && self->gear <= 0) { TransmissionSetGearOrig(self, 0, 1); return; }
    if (self->gear != pick) return;
    if (pick == 0 && self->gear != 0) return;                       // (can't happen: gear == pick here)
    if (!(fabs(D(self->ratio)) > k_ratio_eps)) { TransmissionSetGearOrig(self, 0, 1); return; }
    Engine* engine = self->engine;
    float redline = engine->redline;                                // (copied with integer moves)
    GetRPM(engine);                                                 // result discarded (fstp st(0))
    float speed_rpm = (float)(D(self->input_rpm) / self->ratio);
    engine = self->engine;
    if (engine->idle_rpm > self->input_rpm) { TransmissionSetGearOrig(self, 0, 1); return; }   // test ah,0x41; jne
    uint32_t thr = U32(engine->smoothed_throttle);
    int i = 0;
    if (self->num_gears > 0) {
        float limit = (float)(D(redline) - 200.0f);
        do {
            if ((int32_t)thr > (int32_t)k_one_bits) thr = k_one_bits;
            float rpm = speed_rpm;
            float allowance = (float)((((D(as_float(thr)) * 500.0f) + 500.0f) * redline) * k_rpm_scale);
            if (self->engaged_gear > i + 1) rpm = (float)(D(speed_rpm) + allowance);
            double net_raw = EngineGetNetTorque(self->engine, 0, (float)(D(self->gear_ratios[i]) * rpm), 1.0f);
            float net = (float)(net_raw * self->gear_ratios[i]);
            if (D(self->gear_ratios[i]) * rpm > limit) U32(net) = 0;   // test ah,0x41; jne
            if (net >= as_float(best)) {                                // test ah,1; jne: a NaN skips
                best = U32(net);
                pick = i + 1;
            }
            i = i + 1;
        } while (self->num_gears > i);
    }
    if (self->shift_state == 0 || pick <= 0) TransmissionSetGearOrig(self, 0, pick);
}
static void fp_trans_auto(Footprint& f, Transmission* self, Edx) {
    fp_owner(f, self->owner, self, sizeof(Transmission), "transmission");   // SetGear; GetRPM/GetNetTorque are const
}
PORT_FN(0x00446300, "Transmission::SetGearAuto", Transmission_SetGearAuto, fp_trans_auto)

// Transmission::SetClutch(float) (0x4464a0)
static void __fastcall Transmission_SetClutch(Transmission* self, Edx, float clutch) { memcpy(&self->clutch, &clutch, 4); }
static void fp_trans_setclutch(Footprint& f, Transmission* self, Edx, float) { f.add(self, sizeof(Transmission), "transmission"); f.pure = true; }
PORT_FN(0x004464a0, "Transmission::SetClutch", Transmission_SetClutch, fp_trans_setclutch)

// Transmission::SetThrottle(float) (0x4464b0)
static void __fastcall Transmission_SetThrottle(Transmission* self, Edx, float throttle) { memcpy(&self->throttle, &throttle, 4); }
static void fp_trans_setthrottle(Footprint& f, Transmission* self, Edx, float) { f.add(self, sizeof(Transmission), "transmission"); f.pure = true; }
PORT_FN(0x004464b0, "Transmission::SetThrottle", Transmission_SetThrottle, fp_trans_setthrottle)

// Transmission::GetRPM (0x4464c0, virtual): in gear, the output's rpm x ratio; in neutral the output is
// still asked (and ignored) and the gearbox's own shaft speed is returned (Shaft::GetRPM, called directly)
static double __fastcall Transmission_GetRPM(Transmission* self, Edx) {
    if (self->in_gear) return GetRPM(self->output) * self->ratio;
    GetRPM(self->output);
    return ShaftGetRPMOrig(self, 0);
}
static void fp_trans_getrpm(Footprint&, Transmission*, Edx) {}  // writes nothing
PORT_FN(0x004464c0, "Transmission::GetRPM", Transmission_GetRPM, fp_trans_getrpm)

// Transmission::ApplyTorque(float) (0x4464f0, virtual): in gear, the gearbox spins with the output; its
// inertial torque (clamped to +-400, the lower clamp on the bits) is added and passed on x ratio. In neutral
// the output gets 0 and the gearbox's own shaft takes the torque.
static void __fastcall Transmission_ApplyTorque(Transmission* self, Edx, float torque) {
    if (self->in_gear) {
        self->input_rpm = (float)(GetRPM(self->output) * self->ratio);
        double inertial = ShaftGetInertialTorqueOrig(self, 0, U32(self->input_rpm));
        float t = (float)inertial;
        if (inertial > 400.0f) U32(t) = k_400_bits;                  // fcom; test ah,0x41; jne
        else if (U32(t) > k_minus400_bits) U32(t) = k_minus400_bits; // cmp bits; jbe
        ApplyTorque(self->output, (float)((D(t) + torque) * self->ratio));
    } else {
        self->input_rpm = (float)ShaftGetRPMOrig(self, 0);
        ApplyTorqueBits(self->output, 0);
        ShaftApplyTorqueOrig(self, 0, bits(torque));
    }
}
static void fp_trans_apply(Footprint& f, Transmission* self, Edx, float) {
    fp_owner(f, self->owner, self, sizeof(Transmission), "transmission");
    fp_next(f, self->owner, self->output);
}
PORT_FN(0x004464f0, "Transmission::ApplyTorque", Transmission_ApplyTorque, fp_trans_apply)
