// phys_wheel.cpp -- M3 3.3 group W2: the wheel (physics:wheel.obj), rewritten -- everything but Wheel::Update.
//
// A Wheel is one corner of a Car (Car +0x554, four of them, 0x1a4 bytes each; 0,1 front, 2,3 rear; 0 and 2 on
// the -x side). It derives from TorqueInput (the drivetrain's torque sink: vtable GetRPM, ApplyTorque). Setup
// turns the car file's imperial numbers (CarData, lerped by CarFileCombine from the .cf and the .ccs sliders)
// into SI suspension, tyre and brake constants; update_wheel_position turns steering, bump and compliance into
// the steer and camber angles; UpdateReplay/MakeReplayPacket pack a wheel into five bytes and back.
//
// Written from the v1.0 disassembly: the same sums in the same grouping, register values as double, stored
// values as float, float constants of the same bits (checked below), float copies the original makes with
// integer moves as bit copies, and every call made in the original's order with the same arguments. Tire,
// Damper, the tyre sound and the arm-position helper are called by address (others port them).
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// a float constant with the exact bits of the original's
#define FBITS(lit, b) static_assert(__builtin_bit_cast(uint32_t, lit) == (b), #lit)

// the unused edx of a __thiscall received as __fastcall (phys_sphere.cpp: an int, so test/fuzz.h can size it)
typedef int Edx;

static inline uint32_t bits(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
#define COPY4(dst, src) memcpy(&(dst), &(src), 4)   // a float moved with integer instructions

// ---- layouts (proposed for phys_types.h; file-local until then, so other files' copies can't collide) --------
namespace {
// Damper (carpart.obj): Damper::Setup(bump_lo, bump_hi, rebound_lo, rebound_hi) fits two lines
struct Damper { float bump_slope, bump_base, rebound_slope, rebound_base; };
static_assert(sizeof(Damper) == 16, "Damper");

// Tire (tire.obj, TireCreate): only what Wheel::Setup reads is used here
struct Tire {
    float cornering_quad, cornering_lin;          // +0x00
    float friction_slope, friction_base;          // +0x08
    float camber_stiffness_factor;                // +0x10
    float long_stiffness_factor;                  // +0x14
    float long_friction_factor;                   // +0x18
    float pacejka_b, pacejka_c, pacejka_d, pacejka_e;   // +0x1c
    float load_factor;                            // +0x2c  car tyre width / .tir reference width
    float mass;                                   // +0x30  kg
    float inertia;                                // +0x34  kg m^2
    float camber_peak, camber_peak_gain;          // +0x38
};
static_assert(offsetof(Tire, load_factor) == 0x2c && sizeof(Tire) == 64, "Tire");

struct TireSound;                                 // tiresnd.obj: vtable 0 scalar-deleting dtor, 4 Update

// Wheel (wheel.obj), 0x1a4 bytes. Names from out/types.tsv (tools/names/wheel.py); _NNN not yet understood.
struct Wheel {
    void** vtable;                     // +0x000  TorqueInput: [0] GetRPM, [1] ApplyTorque
    P3 droop_offset;                   // +0x004  body space, minus the hub vertex (Setup)
    const P3* hub_vertex;              // +0x010  the body-mesh vertex nearest the hub
    P3 hub_vertex_rest;                // +0x014  *hub_vertex at Setup
    uint8_t broken, _021[3];           // +0x020
    Damper damper;                     // +0x024
    float inertia;                     // +0x034  kg m^2
    float inv_inertia;                 // +0x038
    float spring_rate;                 // +0x03c  N/m
    float spring_preload;              // +0x040
    float travel;                      // +0x044  m
    float static_deflection;           // +0x048  m
    float bump_stop_rate;              // +0x04c  N/m
    float brake_torque_max;            // +0x050
    float roll_resist_speed;           // +0x054
    float roll_resist_static;          // +0x058
    float radius;                      // +0x05c  m
    float static_camber;               // +0x060  rad
    float static_toe;                  // +0x064  rad
    float steer_quadratic;             // +0x068
    float bump_steer;                  // +0x06c  rad/m
    float bump_camber;                 // +0x070  rad/m
    float toe_compliance;              // +0x074  rad/N
    float camber_compliance;           // +0x078  rad/N
    float caster;                      // +0x07c  rad (front only)
    P3 susp_axis;                      // +0x080  body space, unit
    Tire* tire;                        // +0x08c
    float tire_load_factor;            // +0x090
    float grip_scale;                  // +0x094
    float stiffness_scale;             // +0x098
    float road_noise_quad;             // +0x09c
    float road_noise_lin;              // +0x0a0
    float aligning_torque;             // +0x0a4
    float surface_speed;               // +0x0a8
    float spin_angle;                  // +0x0ac  rad
    P3 heading;                        // +0x0b0  body space (sin steer, 0, cos steer)
    P3 replay_velocity;                // +0x0bc
    P3 droop_point_world;              // +0x0c8
    float steer_angle;                 // +0x0d4  rad
    float camber;                      // +0x0d8  rad
    float steer_input;                 // +0x0dc  rad
    float brake_input;                 // +0x0e0
    float drive_torque;                // +0x0e4  N m (ApplyTorque)
    float compression;                 // +0x0e8  m
    float lat_force_ratio;             // +0x0ec
    float long_force_ratio;            // +0x0f0
    P3 tire_force;                     // +0x0f4
    P3 chassis_tire_force;             // +0x100
    float lat_force_filtered;          // +0x10c
    P3 susp_axis_world;                // +0x110
    float slide;                       // +0x11c
    uint8_t fx_flags;                  // +0x120
    uint8_t surface_class;             // +0x121
    uint8_t _122[2];
    float omega;                       // +0x124  rad/s
    float rpm;                         // +0x128
    float brake_power;                 // +0x12c  W
    P3 contact_point;                  // +0x130
    float prev_omega;                  // +0x13c
    P3 prev_axle;                      // +0x140
    P3 _14c;                           // +0x14c  the ctor sets (0, 1, 0); nothing in wheel.obj reads it
    float anti_roll_force;             // +0x158
    int32_t traction_control;          // +0x15c
    uint8_t digital_throttle, _161[3]; // +0x160
    int32_t abs_mode;                  // +0x164
    uint8_t on_ground, _169[3];        // +0x168
    uint32_t _16c;                     // +0x16c  the ctor and ResetPosition zero it
    float lat_slip;                    // +0x170
    float long_slip;                   // +0x174
    float susp_force;                  // +0x178
    float drag_torque;                 // +0x17c
    float long_slip_raw;               // +0x180
    TireSound* tire_sound;             // +0x184
    float brake_cooling;               // +0x188  W/K
    float brake_heat_capacity;         // +0x18c  J/K
    float inv_brake_heat_capacity;     // +0x190
    float brake_temp;                  // +0x194  K
    float heat_accum;                  // +0x198  J
    int32_t surface;                   // +0x19c
    uint8_t is_remote, _1a1[3];        // +0x1a0
};
static_assert(offsetof(Wheel, damper) == 0x24 && offsetof(Wheel, susp_axis) == 0x80 &&
              offsetof(Wheel, tire) == 0x8c && offsetof(Wheel, heading) == 0xb0 &&
              offsetof(Wheel, steer_angle) == 0xd4 && offsetof(Wheel, compression) == 0xe8 &&
              offsetof(Wheel, tire_force) == 0xf4 && offsetof(Wheel, lat_force_filtered) == 0x10c &&
              offsetof(Wheel, slide) == 0x11c && offsetof(Wheel, fx_flags) == 0x120 &&
              offsetof(Wheel, omega) == 0x124 && offsetof(Wheel, contact_point) == 0x130 &&
              offsetof(Wheel, _14c) == 0x14c && offsetof(Wheel, anti_roll_force) == 0x158 &&
              offsetof(Wheel, on_ground) == 0x168 && offsetof(Wheel, lat_slip) == 0x170 &&
              offsetof(Wheel, tire_sound) == 0x184 && offsetof(Wheel, brake_temp) == 0x194 &&
              offsetof(Wheel, surface) == 0x19c && offsetof(Wheel, is_remote) == 0x1a0 && sizeof(Wheel) == 0x1a4,
              "Wheel");

// Car (car.obj), 3768 bytes: only what wheel.obj reads
struct Car : PhobDyno {
    uint8_t _478[0x510 - 0x478];
    int32_t car_index;                 // +0x510
    char name[32];                     // +0x514
    uint8_t _534[0x554 - 0x534];
    Wheel wheels[4];                   // +0x554
    uint8_t _be4[3768 - 0xbe4];
};
static_assert(offsetof(Car, car_index) == 0x510 && offsetof(Car, wheels) == 0x554 && sizeof(Car) == 3768, "Car");
// PhobRoot +0x28: a body-space offset GetMessage takes off the hub position (unnamed in types.tsv)
static inline const float* body_offset(const Car* car) { return (const float*)&car->_028[0]; }

// CarData (carfile.obj CarFileCombine's output): what Wheel::Setup reads. Pairs are [front, rear]; the units
// are the car file's (inches, degrees, lb). The .cf key each comes from is in brackets.
struct CarData {
    uint8_t _000[0x50];
    float track[2];                    // +0x50  in [ftrack, rtrack]
    float wheelbase;                   // +0x58  in [wheelbase]
    float weight_distribution;         // +0x5c  % on the front axle [weight_distribution]
    float ride_height[2];              // +0x60  in [f/rground_clearance1..2, lerped]
    uint8_t _068[0xb4 - 0x68];
    float torque_balance;              // +0xb4  rear share [torque_balance]
    uint8_t _0b8[0xcc - 0xb8];
    float springs[2];                  // +0xcc  lb/in [f/rsprings, lerped]
    float bump[2];                     // +0xd4  [f/rbump, lerped]
    float rebound[2];                  // +0xdc  [f/rrebound, lerped]
    float sway[2];                     // +0xe4  [f/rsway, lerped] (Car::Setup)
    float camber[2];                   // +0xec  deg [f/rcamber, lerped]
    float toe[2];                      // +0xf4  deg [f/rtoe, lerped]
    float caster;                      // +0xfc  deg [caster]
    float anti[2];                     // +0x100 [anti_dive, anti_squat]
    float bump_camber[2];              // +0x108 deg/in [f/rbump_camber]
    float bump_toe[2];                 // +0x110 deg/in [f/rbump_toe]
    float toe_compliance[2];           // +0x118 deg per 1000 lbf [f/rtoe_compliance]
    float camber_compliance[2];        // +0x120 deg per 1000 lbf [f/rcamber_compliance]
    float cm_height;                   // +0x128 in [cm_height]
    float wheel_lock;                  // +0x12c deg [wheel_lock x setup] (Car::Setup)
    struct { float width, aspect, rim; } tyre[2];   // +0x130 mm, %, in [f/rtyre_width/aspect/rim]
    float brake[2];                    // +0x148 [f/rbrake1..2, lerped by the brake bias]
    float grip_scale[2];               // +0x150 [f/rgrip_scale]
    float tyre_stiffness_scale[2];     // +0x158 [f/rtyre_stiffness_scale]
    uint32_t _160;                     // +0x160 CarFileCombine writes 0
    float rolling_resistance;          // +0x164 [rolling_resistance]
    float static_rolling_resistance;   // +0x168 [static_rolling_resistance]
};
static_assert(offsetof(CarData, torque_balance) == 0xb4 && offsetof(CarData, springs) == 0xcc &&
              offsetof(CarData, caster) == 0xfc && offsetof(CarData, cm_height) == 0x128 &&
              offsetof(CarData, tyre) == 0x130 && offsetof(CarData, brake) == 0x148 &&
              offsetof(CarData, static_rolling_resistance) == 0x168, "CarData");

// WheelMessage (Wheel::GetMessage; Car::GetMessage steps 0x38 per wheel): what the renderer draws
struct WheelMessage {
    P3 hub_pos;                        // +0x00
    float hub_height;                  // +0x0c
    float steer_angle, camber;         // +0x10
    float omega, spin_angle;           // +0x18
    float slide;                       // +0x20
    float lat_force_ratio, long_force_ratio;   // +0x24
    float compression_fraction;        // +0x2c  min(compression / travel, 1)
    float brake_temp;                  // +0x30
    uint8_t fx_flags, _35[3];          // +0x34
};
static_assert(offsetof(WheelMessage, compression_fraction) == 0x2c && sizeof(WheelMessage) == 56, "WheelMessage");

// Wheel::ReplayPacket: five bytes, each the high byte of a 0..32768 fixed-point value
struct WheelReplayPacket {
    uint8_t spin;                      // spin_angle, -2pi..2pi
    uint8_t omega;                     // -100..100 rad/s
    uint8_t compression;               // 0..2 m
    uint8_t slide;                     // 0..10
    uint8_t fx_flags;                  // as is (0x40 = broken)
};
static_assert(sizeof(WheelReplayPacket) == 5, "WheelReplayPacket");
}  // namespace

// ---- the game's functions these call ----------------------------------------------------------------------
typedef double(__cdecl* PhysicsGetTemperature_t)();                   // fld [global]: ST0
typedef void(__fastcall* DamperSetup_t)(Damper*, Edx, float, float, float, float);
typedef double(__cdecl* GetLoadFluctuation_t)(float, float, float, float, const Damper*, float);   // ST0, unrounded
typedef Tire*(__cdecl* TireCreate_t)(float width_mm, float aspect_pct, float diameter_in, const char* file);
typedef TireSound*(__cdecl* TireSoundCreate_t)(int car_index, int wheel, Wheel*, Car*);
typedef uint8_t(__cdecl* ResourceExists_t)(const char*);
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
typedef void(__cdecl* OperatorDelete_t)(void*);
typedef void(__fastcall* GetArmPosition_t)(Car*, Edx, P3* out, const P3* in);
typedef void(__fastcall* UpdateWheelPosition_t)(Wheel*, Edx, Car*);

static const PhysicsGetTemperature_t PhysicsGetTemperature = (PhysicsGetTemperature_t)0x0042bca0;
static const DamperSetup_t DamperSetup = (DamperSetup_t)0x00446a90;
static const GetLoadFluctuation_t GetLoadFluctuation = (GetLoadFluctuation_t)0x00446b20;
static const TireCreate_t TireCreate = (TireCreate_t)0x0043c080;
static const TireSoundCreate_t TireSoundCreate = (TireSoundCreate_t)0x004731c0;
static const ResourceExists_t ResourceExists = (ResourceExists_t)0x00419d10;
static const Sprintf_t game_sprintf = (Sprintf_t)0x004cf0a0;
static const OperatorDelete_t operator_delete = (OperatorDelete_t)0x00414390;
static const GetArmPosition_t GetArmPosition = (GetArmPosition_t)0x0043e0d0;     // PhobRoot::GetArmPosition
static const UpdateWheelPosition_t update_wheel_position = (UpdateWheelPosition_t)0x0044a490;   // (hooked below)

static void** const VT_TorqueInput = (void**)0x004dca70;
static void** const VT_Wheel = (void**)0x004dca78;
static const uint32_t VT_RealTireSound = 0x004dd7b0, VT_DummyTireSound = 0x004dd7a0;

// ---- constants (the original's .rdata floats) --------------------------------------------------------------
static const float K_2PI = 6.28318548f;             FBITS(6.28318548f, 0x40c90fdbu);
static const float K_Q15 = 3.05175781e-05f;         FBITS(3.05175781e-05f, 0x38000000u);   // 2^-15
static const float K_DEG = 0.0174532924f;           FBITS(0.0174532924f, 0x3c8efa35u);     // pi/180
static const float K_INCH = 0.0254f;                FBITS(0.0254f, 0x3cd013a9u);
FBITS(80.4f, 0x42a0cccdu);           FBITS(1.66666675f, 0x3fd55556u);   FBITS(0.1f, 0x3dcccccdu);
FBITS(0.0872664601f, 0x3db2b8c2u);   FBITS(1.96104411e-05f, 0x37a4811au);
FBITS(0.261799395f, 0x3e860a92u);    FBITS(-3.92208822e-05f, 0xb824811au);  FBITS(0.17453292f, 0x3e32b8c2u);
FBITS(0.000383495208f, 0x39c90fdbu); FBITS(2607.59448f, 0x4522f983u);
FBITS(0.01f, 0x3c23d70au);           FBITS(175.196838f, 0x432f3264u);   FBITS(4.45f, 0x408e6666u);
FBITS(2.67f, 0x402ae148u);           FBITS(0.0127f, 0x3c5013a9u);       FBITS(0.001f, 0x3a83126fu);
FBITS(4.905f, 0x409cf5c3u);          FBITS(39.370079f, 0x421d7af6u);    FBITS(0.000224719101f, 0x396ba293u);
FBITS(0.305f, 0x3e9c28f6u);          FBITS(2.225f, 0x400e6666u);        FBITS(0.50015f, 0x3f0009d5u);
FBITS(0.50085f, 0x3f0037b5u);        FBITS(1.1125f, 0x3f8e6666u);       FBITS(78.7401581f, 0x429d7af6u);
FBITS(999.999939f, 0x4479ffffu);     FBITS(9.09f, 0x411170a4u);         FBITS(26.666666f, 0x41d55555u);
FBITS(350393.688f, 0x48ab1736u);     FBITS(71.1111145f, 0x428e38e4u);   FBITS(-1.18652333e-05f, 0xb74710cau);
FBITS(711.111084f, 0x4431c71cu);     FBITS(5056.79053f, 0x459e0653u);

// ---- Wheel::Wheel (0x447ed0) ----------------------------------------------------------------------------------
// TorqueInput's vtable, then Wheel's; brakes at 290 K with 1800 J/K; a unit radius and 10 kg m^2 until Setup.
// The damper, the spring and geometry constants, the ratios and abs_mode are left unset.
static Wheel* __fastcall Wheel_ctor(Wheel* self, Edx) {
    double cap = D(900.0f) * 2.0f;                      // kept in a register for the reciprocal
    self->vtable = VT_TorqueInput;
    self->brake_temp = 290.0f;
    self->brake_heat_capacity = (float)cap;
    double inv = 1.0f / cap;
    self->vtable = VT_Wheel;
    self->broken = 0;
    self->surface = 0;
    self->is_remote = 0;
    self->tire = 0;
    self->compression = 0.0f;
    self->drive_torque = 0.0f;
    self->brake_input = 0.0f;
    self->anti_roll_force = 0.0f;
    self->rpm = 0.0f;
    self->camber = 0.0f;
    self->drag_torque = 0.0f;
    self->steer_angle = 0.0f;
    self->steer_input = 0.0f;
    self->aligning_torque = 0.0f;
    self->lat_force_filtered = 0.0f;
    self->contact_point.x = 0.0f; self->contact_point.y = 0.0f; self->contact_point.z = 0.0f;
    self->chassis_tire_force.x = 0.0f; self->chassis_tire_force.y = 0.0f; self->chassis_tire_force.z = 0.0f;
    self->susp_axis_world.x = 0.0f;
    self->heat_accum = 0.0f;
    self->susp_axis_world.z = 0.0f;
    self->lat_slip = 0.0f;
    self->long_slip = 0.0f;
    self->susp_force = 0.0f;
    self->_14c.x = 0.0f; self->_14c.y = 1.0f; self->_14c.z = 0.0f;
    self->replay_velocity.x = 0.0f; self->replay_velocity.y = 0.0f; self->replay_velocity.z = 0.0f;
    self->droop_point_world.x = 0.0f; self->droop_point_world.y = 0.0f; self->droop_point_world.z = 0.0f;
    self->_16c = 0;
    self->spin_angle = 0.0f;
    self->slide = 0.0f;
    self->fx_flags = 0;
    self->surface_class = 0;
    self->static_camber = 0.0f;
    self->static_toe = 0.0f;
    self->radius = 1.0f;
    self->inertia = 10.0f;
    self->inv_inertia = 0.1f;
    self->susp_axis_world.y = 1.0f;
    self->inv_brake_heat_capacity = (float)inv;
    self->brake_cooling = (float)(D(80.4f) * 1.66666675f);
    self->omega = 0.0f;
    self->prev_omega = 0.0f;
    self->heading.x = 0.0f; self->heading.y = 0.0f; self->heading.z = 1.0f;
    self->prev_axle.x = 0.0f; self->prev_axle.y = 0.0f; self->prev_axle.z = 0.0f;
    self->droop_offset.x = 0.0f; self->droop_offset.y = 0.0f; self->droop_offset.z = 0.0f;
    self->digital_throttle = 0;
    self->traction_control = 0;
    self->surface_speed = 0.0f;
    self->on_ground = 0;
    self->bump_stop_rate = 0.0f;
    self->tire_force.x = 0.0f; self->tire_force.y = 0.0f; self->tire_force.z = 0.0f;
    self->tire_sound = 0;
    self->hub_vertex = 0;
    return self;
}
static void fp_wheel_ctor(Footprint& f, Wheel* self, Edx) { f.add(self, sizeof(Wheel), "wheel"); }
PORT_FN(0x00447ed0, "Wheel::Wheel", Wheel_ctor, fp_wheel_ctor)

// ---- Wheel::ResetPosition (0x4480c0) ----------------------------------------------------------------------------
static void __fastcall Wheel_ResetPosition(Wheel* self, Edx) {
    self->prev_omega = 0.0f;
    self->omega = 0.0f;
    self->on_ground = 0;
    self->broken = 0;
    self->compression = 0.0f;
    self->steer_input = 0.0f;
    self->aligning_torque = 0.0f;
    self->lat_force_filtered = 0.0f;
    self->_16c = 0;
    self->anti_roll_force = 0.0f;
}
static void fp_wheel_reset(Footprint& f, Wheel* self, Edx) {
    f.add(self, sizeof(Wheel), "wheel");
    f.pure = true;
}
PORT_FN(0x004480c0, "Wheel::ResetPosition", Wheel_ResetPosition, fp_wheel_reset)

// ---- Wheel::~Wheel (0x448100): deletes the tyre sound (its scalar-deleting destructor) and frees the Tire ------
static void __fastcall Wheel_dtor(Wheel* self, Edx) {
    self->vtable = VT_Wheel;
    if (TireSound* ts = self->tire_sound) {
        VFN(ts, 0, void*, unsigned)(ts, 0, 1);
        self->tire_sound = 0;
    }
    if (Tire* t = self->tire) operator_delete(t);
}
static void fp_wheel_dtor(Footprint& f, Wheel*, Edx) { f.replay_only = "frees the Tire and the TireSound"; }
PORT_FN(0x00448100, "Wheel::~Wheel", Wheel_dtor, fp_wheel_dtor)

// ---- Wheel::Setup (0x448140) ------------------------------------------------------------------------------------
// wheel 0/1 front, 2/3 rear; 0/2 on the -x side. Imperial to SI: in x 0.0254, lb/in x 175.197, deg x pi/180.
static void __fastcall Wheel_Setup(Wheel* self, Edx, const CarData* cd, Car* car, int index, const P3* hub) {
    self->hub_vertex = hub;
    memcpy(&self->hub_vertex_rest, hub, 12);
    const bool front = index == 0 || index == 1;
    const int r = front ? 0 : 1;
    const float side = (index == 0 || index == 2) ? -1.0f : 1.0f;

    // the original's stack frame from the tyre-file name on: a 36-byte buffer, then the tyre width in metres,
    // which a 31-character car name overwrites (sprintf's terminator) before TireCreate reads it back
    uint8_t frame[40];
    char* tire_file = (char*)frame;

    const float wheelbase = (float)(D(cd->wheelbase) * K_INCH);
    const float track = (float)(fabs(D(cd->track[r])) * K_INCH);
    double wd = D(cd->weight_distribution) * 0.01f;
    const float front_share = (float)wd;
    const float rear_share = (float)(1.0f - wd);
    self->static_camber = (float)-((D(cd->camber[r]) * K_DEG) * side);
    self->static_toe = (float)-((D(cd->toe[r]) * K_DEG) * side);
    self->spring_rate = (float)(D(cd->springs[r]) * 175.196838f);
    double inch_r = D(K_INCH) / 1.0f;                   // 0.0254 / 1.0, in a register
    const float inch = (float)inch_r;
    const float bump_lo = (float)((D(cd->bump[r]) / inch_r) * 4.45f);
    const float bump_hi = (float)((D(cd->bump[r]) / inch) * 2.67f);
    const float rebound_lo = (float)((D(cd->rebound[r]) / inch) * 4.45f);
    const float rebound_hi = (float)((D(cd->rebound[r]) / inch) * 4.45f);
    DamperSetup(&self->damper, 0, bump_lo, bump_hi, rebound_lo, rebound_hi);

    COPY4(self->bump_stop_rate, self->spring_rate);
    const float ride = (float)(D(cd->ride_height[r]) * K_INCH);
    self->travel = (float)(D(ride) * 2.0f);
    COPY4(self->static_deflection, ride);
    self->radius = (float)(D(cd->tyre[r].rim) * 0.0127f);
    const float width = (float)(D(cd->tyre[r].width) * 0.001f);
    memcpy(frame + 36, &width, 4);
    double aspect_r = D(cd->tyre[r].aspect) * 0.01f;
    const float aspect = (float)aspect_r;
    self->radius = (float)(aspect_r * width + self->radius);
    const float share = front ? front_share : rear_share;
    self->spring_preload = 0.0f;
    const float corner_load = (float)((D(car->mass) * share) * 4.905f);
    // the static deflection that holds the corner load, from the middle of the travel
    double defl = (D(corner_load) - (D(self->travel) * 0.5f) * self->spring_rate) / self->spring_rate;
    self->travel = (float)(D(self->travel) + defl);
    self->static_deflection = (float)(defl + ride);
    self->steer_quadratic = 0.0f;
    const float height = (float)(D(cd->cm_height) * K_INCH + self->travel);
    const float bump_toe = (float)((D(cd->bump_toe[r]) * K_DEG) * 39.370079f);
    self->bump_camber = (float)(((D(cd->bump_camber[r]) * K_DEG) * side) * -39.370079f);
    self->bump_steer = (float)-(D(side) * bump_toe);
    if (front) self->caster = (float)(D(cd->caster) * K_DEG);
    else self->caster = 0.0f;
    self->camber_compliance = (float)((D(cd->camber_compliance[r]) * K_DEG) * -0.000224719101f);
    self->toe_compliance = (float)((D(cd->toe_compliance[r]) * K_DEG) * 0.000224719101f);
    float along;                                        // the axle's distance from the centre of mass, / wheelbase
    if (front) COPY4(along, rear_share);
    else along = (float)-D(front_share);
    self->droop_offset.x = (float)((D(side) * track) * 0.5f);
    self->droop_offset.y = (float)-D(height);
    self->droop_offset.z = (float)(D(wheelbase) * along);

    // the suspension axis: perpendicular to the arm from the centre of mass (the anti-dive/-squat share of the
    // height taken off), in the y-z plane, pointing up
    P3 arm;
    memcpy(&arm, &self->droop_offset, 12);
    arm.y = (float)((1.0f - D(cd->anti[r])) * arm.y);
    arm.x = (float)(D(arm.x) - self->droop_offset.x);
    arm.y = (float)(D(arm.y) - self->droop_offset.y);
    arm.z = (float)-D(self->droop_offset.z);
    // (the original compares `this` with two of its own stack slots here -- a compiler alias check that can't
    // be taken; left out)
    const float ny = (float)-D(arm.y);
    self->susp_axis.x = 0.0f;
    COPY4(self->susp_axis.y, arm.z);
    self->susp_axis.z = ny;
    if (bits(arm.z) > 0x80000000u) {                    // an integer compare: z < 0 (or a negative NaN)
        self->susp_axis.x = (float)-D(self->susp_axis.x);
        self->susp_axis.y = (float)-D(arm.z);
        self->susp_axis.z = (float)-D(self->susp_axis.z);
    }
    P3& a = self->susp_axis;
    double k = 1.0f / x87_sqrt((D(a.x) * a.x + D(a.z) * a.z) + D(a.y) * a.y);
    a.x = (float)(a.x * k);
    a.y = (float)(a.y * k);
    a.z = (float)(k * a.z);

    const P3* hv = self->hub_vertex;
    self->droop_offset.x = (float)(D(self->droop_offset.x) - hv->x);
    self->droop_offset.y = (float)(D(self->droop_offset.y) - hv->y);
    self->droop_offset.z = (float)(D(self->droop_offset.z) - hv->z);
    self->brake_torque_max = (float)(D(cd->brake[r]) * 4.45f);
    COPY4(self->grip_scale, cd->grip_scale[r]);
    COPY4(self->stiffness_scale, cd->tyre_stiffness_scale[r]);
    double ft_r = D(0.305f) / 1.0f;                     // 0.305 / 1.0, in a register
    const float roll_k = (float)((D(cd->rolling_resistance) / ft_r) * 2.225f);
    double half = front ? D(0.50015f) : D(0.50085f);
    self->roll_resist_speed = (float)((half / corner_load) * roll_k);
    self->roll_resist_static = (float)((D(cd->static_rolling_resistance) / corner_load) * 1.1125f);

    // the tyre: <car name>f.tir / r.tir, else the default
    game_sprintf(tire_file, "%s%s.tir", car->name, front ? "f" : "r");
    if (!ResourceExists(tire_file)) memcpy(tire_file, "def_tire.tir", 13);   // strcpy, inlined
    float width_now;
    memcpy(&width_now, frame + 36, 4);
    Tire* tire = TireCreate((float)(D(width_now) * 999.999939f), (float)(D(aspect) * 100.0f),
                            (float)(D(self->radius) * 78.7401581f), tire_file);
    self->tire = tire;
    COPY4(self->tire_load_factor, tire->load_factor);
    COPY4(self->inertia, tire->inertia);
    // a driven axle carries the drivetrain's inertia too (torque_balance compared as integer bits)
    int32_t tb;
    memcpy(&tb, &cd->torque_balance, 4);
    if (front ? tb < 0x3f666666 : tb > 0x3dcccccd)      // front: < 0.9 (not all-rear); rear: > 0.1
        self->inertia = (float)(D(self->inertia) + 10.0f);
    self->inv_inertia = (float)(1.0f / D(self->inertia));
    self->tire_sound = TireSoundCreate(car->car_index, index, self, car);

    // road noise: the load fluctuation of the quarter car at two frequencies, fitted to a line and a square
    const float unsprung = (float)(D(self->tire->mass) + 9.09f);
    const float sprung = (float)((D(car->mass) * share) * 0.5f - unsprung);
    const float lf_lo = (float)GetLoadFluctuation(sprung, unsprung, 350393.688f, self->spring_rate, &self->damper,
                                                  26.666666f);
    double lf_hi = GetLoadFluctuation(sprung, unsprung, 350393.688f, self->spring_rate, &self->damper, 71.1111145f);
    self->road_noise_quad = (float)((-26.666666f * lf_hi + D(lf_lo) * 71.1111145f) * -1.18652333e-05f);
    self->road_noise_lin = (float)((lf_hi * 711.111084f - D(lf_lo) * 5056.79053f) * -1.18652333e-05f);
}
static void fp_wheel_setup(Footprint& f, Wheel*, Edx, const CarData*, Car*, int, const P3*) {
    f.replay_only = "allocates the Tire (TireCreate) and the TireSound";
}
PORT_FN(0x00448140, "Wheel::Setup", Wheel_Setup, fp_wheel_setup)

// ---- Wheel::UpdateHeat (0x44a440): the brake's heat, gained from brake power and lost to the air --------------
static void __fastcall Wheel_UpdateHeat(Wheel* self, Edx, float dt) {
    double ambient = PhysicsGetTemperature();
    self->heat_accum = (float)(((ambient - self->brake_temp) * self->brake_cooling + self->brake_power) * dt +
                               self->heat_accum);
    double rise = D(self->inv_brake_heat_capacity) * self->heat_accum;
    self->heat_accum = 0.0f;
    self->brake_temp = (float)(rise + self->brake_temp);
}
static void fp_wheel_update_heat(Footprint& f, Wheel* self, Edx, float) { f.add(self, sizeof(Wheel), "wheel"); }
PORT_FN(0x0044a440, "Wheel::UpdateHeat", Wheel_UpdateHeat, fp_wheel_update_heat)

// ---- Wheel::update_wheel_position (0x44a490) ---------------------------------------------------------------------
// steer = toe + bump steer + compliance + (k s + 1) s; a broken wheel wobbles with its spin instead and toes out.
// camber = (static + bump + compliance) cos(steer) - caster sin(steer). The suspension axis into world space.
static void __fastcall Wheel_update_wheel_position(Wheel* self, Edx, Car* car) {
    const float side = (D(self->droop_offset.x) + self->hub_vertex_rest.x) > 0.0f ? 1.0f : -1.0f;
    const float bump = (float)(D(self->travel) * -0.5f + self->compression);   // from mid-travel
    const uint8_t broken = self->broken;
    double s;
    if (!broken) {
        s = ((D(self->steer_quadratic) * self->steer_input + 1.0f) * self->steer_input + D(self->bump_steer) * bump) +
            D(self->toe_compliance) * self->lat_force_filtered;
        s = s + self->static_toe;
    } else {
        s = x87_sin_mul(self->spin_angle, 0.0872664601f) + D(self->bump_steer) * bump;   // fsin; fmul dword
        s = s + D(self->static_toe) * 2.0f;
        s = s + D(self->lat_force_filtered) * 1.96104411e-05f;
        s = s + D(side) * 0.261799395f;
    }
    self->steer_angle = (float)s;
    self->heading.x = x87_sin_f(self->steer_angle);              // fsin; fstp dword
    self->heading.y = 0.0f;
    self->heading.z = x87_cos_f(self->steer_angle);              // fcos; fstp dword
    double c = (D(self->camber_compliance) * self->lat_force_filtered + D(self->bump_camber) * bump) +
               self->static_camber;
    self->camber = (float)(c * self->heading.z - D(self->caster) * self->heading.x);
    if (broken)
        self->camber = (float)((D(self->lat_force_filtered) * -3.92208822e-05f + self->camber) + D(side) * 0.17453292f);
    const float* m = car->frame.rot.m;
    const P3& a = self->susp_axis;
    self->susp_axis_world.x = (float)((D(m[0]) * a.x + D(m[6]) * a.z) + D(m[3]) * a.y);
    self->susp_axis_world.y = (float)((D(m[4]) * a.y + D(m[1]) * a.x) + D(m[7]) * a.z);
    self->susp_axis_world.z = (float)((D(m[5]) * a.y + D(m[8]) * a.z) + D(m[2]) * a.x);
}
static void fp_wheel_position(Footprint& f, Wheel* self, Edx, Car*) { f.add(self, sizeof(Wheel), "wheel"); }
PORT_FN(0x0044a490, "Wheel::update_wheel_position", Wheel_update_wheel_position, fp_wheel_position)

// ---- Wheel::UpdateCommon (0x44a630): the tyre sound's Update (vtable +4) ------------------------------------------
static void __fastcall Wheel_UpdateCommon(Wheel* self, Edx, Car*) {
    if (TireSound* ts = self->tire_sound) VFN(ts, 4, void)(ts, 0);
}
static void fp_wheel_update_common(Footprint& f, Wheel* self, Edx, Car*) {
    TireSound* ts = self->tire_sound;
    if (!ts) return;
    uint32_t vt = *(const uint32_t*)ts;
    if (vt == VT_RealTireSound) {
        // RealTireSound (0x3c): its pitch/volume/position sums (+0x1c..+0x24); its Sound3D (+0x34): the
        // frequency and volume (+4, +8) and the changed flags (+0x29..+0x2d)
        f.add(ts, 0x3c, "tire sound");
        if (uint8_t* s3d = *(uint8_t**)((uint8_t*)ts + 0x34)) f.add(s3d + 4, 0x2a, "tire Sound3D");
    } else if (vt == VT_DummyTireSound) {
        f.add(ts, 0x10, "tire sound");                  // its Update is a bare ret
    }
}
PORT_FN(0x0044a630, "Wheel::UpdateCommon", Wheel_UpdateCommon, fp_wheel_update_common)

// ---- Wheel::GetMessage (0x44a650): the renderer's view of the wheel ------------------------------------------------
static void __fastcall Wheel_GetMessage(Wheel* self, Edx, WheelMessage* msg, Car* car) {
    const float* body = body_offset(car);
    const P3* hv = self->hub_vertex;
    float hub[3];
    hub[0] = (float)((D(self->droop_offset.x) + hv->x) - body[0]);
    hub[2] = (float)((D(self->droop_offset.z) + hv->z) - body[2]);
    double y = (D(hv->y) + self->droop_offset.y) - body[1];
    double m = D(self->travel) > self->compression ? D(self->compression) : D(self->travel);   // min, NaN -> travel
    hub[1] = (float)(y + m);
    memcpy(&msg->hub_pos, hub, 12);
    m = D(self->travel) > self->compression ? D(self->compression) : D(self->travel);
    hv = self->hub_vertex;
    msg->hub_height = (float)(((m + hv->y) - body[1]) + self->droop_offset.y);
    COPY4(msg->steer_angle, self->steer_angle);
    COPY4(msg->camber, self->camber);
    COPY4(msg->omega, self->omega);
    COPY4(msg->spin_angle, self->spin_angle);
    COPY4(msg->slide, self->slide);
    msg->fx_flags = self->fx_flags;
    COPY4(msg->lat_force_ratio, self->lat_force_ratio);
    COPY4(msg->long_force_ratio, self->long_force_ratio);
    double q = D(self->compression) / self->travel;
    msg->compression_fraction = (float)(1.0 > q ? q : 1.0);   // min, NaN -> 1
    COPY4(msg->brake_temp, self->brake_temp);
}
static void fp_wheel_message(Footprint& f, Wheel*, Edx, WheelMessage* msg, Car*) {
    f.add(msg, sizeof(WheelMessage), "message");
}
PORT_FN(0x0044a650, "Wheel::GetMessage", Wheel_GetMessage, fp_wheel_message)

// ---- Wheel::UpdateReplay (0x44a770): between two packets at t, then the wheel's position from the car ----------
// Each byte is a value's high byte: v = b x 256 (fild), the lerp done as in the original, v0 kept in a register.
static void __fastcall Wheel_UpdateReplay(Wheel* self, Edx, const WheelReplayPacket* p0, const WheelReplayPacket* p1,
                                          float t, Car* car) {
    double c0 = (double)(p0->compression * 256), c1 = (double)(p1->compression * 256);
    self->compression = (float)((c1 - c0) * 2.0f * K_Q15 * t + 2.0f * c0 * K_Q15);
    double s0 = (double)(p0->spin * 256) * 0.000383495208f;
    double s1 = (double)(p1->spin * 256) * 0.000383495208f;
    self->spin_angle = (float)((s0 + (s1 - s0) * t) - K_2PI);
    double o0 = (double)(p0->omega * 256), o1 = (double)(p1->omega * 256);
    double orange = D(100.0f) - -100.0f;
    self->omega = (float)(((o1 * K_Q15 - K_Q15 * o0) * orange * t + o0 * orange * K_Q15) + -100.0f);
    double l0 = (double)(p0->slide * 256), l1 = (double)(p1->slide * 256);
    double lrange = D(10.0f) - 0.0f;
    self->slide = (float)(((l1 * K_Q15 - K_Q15 * l0) * lrange * t + l0 * lrange * K_Q15) + 0.0f);
    uint8_t fx = p0->fx_flags;
    self->fx_flags = fx;
    self->broken = (uint8_t)((fx & 0x40) >> 6);
    update_wheel_position(self, 0, car);
    memcpy(&self->replay_velocity, &car->velocity, 12);
    GetArmPosition(car, 0, &self->droop_point_world, &self->droop_offset);
}
static void fp_wheel_update_replay(Footprint& f, Wheel* self, Edx, const WheelReplayPacket*, const WheelReplayPacket*,
                                   float, Car*) {
    f.add(self, sizeof(Wheel), "wheel");
}
PORT_FN(0x0044a770, "Wheel::UpdateReplay", Wheel_UpdateReplay, fp_wheel_update_replay)

// ---- Wheel::MakeReplayPacket (0x44a960) --------------------------------------------------------------------------
// each value clamped (the lower bound first, NaN to one end), scaled to 0..32768, truncated (__ftol), high byte
static void __fastcall Wheel_MakeReplayPacket(const Wheel* self, Edx, WheelReplayPacket* p) {
    float v;
    if (bits(self->compression) > 0x80000000u) v = 0.0f;           // integer compare: below zero
    else v = (float)(D(2.0f) > self->compression ? D(self->compression) : D(2.0f));
    p->compression = (uint8_t)(x87_ftol((D(v) / 2.0f) * 32768.0f) >> 8);
    if (bits(self->spin_angle) > 0xc0c90fdbu) v = -K_2PI;          // integer compare: below -2pi
    else v = (float)(D(K_2PI) > self->spin_angle ? D(self->spin_angle) : D(K_2PI));
    p->spin = (uint8_t)(x87_ftol((D(v) + K_2PI) * 2607.59448f) >> 8);
    if (!(D(self->omega) >= -100.0f)) v = -100.0f;
    else v = (float)(D(100.0f) > self->omega ? D(self->omega) : D(100.0f));
    p->omega = (uint8_t)(x87_ftol(((D(v) - -100.0f) / (D(100.0f) - -100.0f)) * 32768.0f) >> 8);
    if (!(D(self->slide) >= 0.0f)) v = 0.0f;
    else v = (float)(D(10.0f) > self->slide ? D(self->slide) : D(10.0f));
    p->slide = (uint8_t)(x87_ftol(((D(v) - 0.0f) / (D(10.0f) - 0.0f)) * 32768.0f) >> 8);
    p->fx_flags = self->fx_flags;
}
static void fp_wheel_packet(Footprint& f, const Wheel*, Edx, WheelReplayPacket* p) {
    f.add(p, sizeof(WheelReplayPacket), "packet");
    f.pure = true;
}
PORT_FN(0x0044a960, "Wheel::MakeReplayPacket", Wheel_MakeReplayPacket, fp_wheel_packet)

// ---- Wheel::ApplyTorque (0x44ab20, TorqueInput vtable +4): the drivetrain's torque, moved as bits ----------------
static void __fastcall Wheel_ApplyTorque(Wheel* self, Edx, uint32_t torque) { memcpy(&self->drive_torque, &torque, 4); }
static void fp_wheel_apply_torque(Footprint& f, Wheel* self, Edx, uint32_t) {
    f.add(&self->drive_torque, 4, "drive_torque");
    f.pure = true;
}
PORT_FN(0x0044ab20, "Wheel::ApplyTorque", Wheel_ApplyTorque, fp_wheel_apply_torque)

// ---- Wheel::GetRPM (0x44ab30, TorqueInput vtable +0): fld [rpm] ----------------------------------------------------
static float __fastcall Wheel_GetRPM(const Wheel* self, Edx) { return self->rpm; }
static void fp_wheel_get_rpm(Footprint& f, const Wheel*, Edx) { f.pure = true; }
PORT_FN(0x0044ab30, "Wheel::GetRPM", Wheel_GetRPM, fp_wheel_get_rpm)
