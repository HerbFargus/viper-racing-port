// phys_car.cpp -- M3 3.4 group C2: the Car's construction, damage, teleport, reset and replay (physics:car.obj),
// rewritten. Car::Update and the per-tick side (UpdateCommon, the Set* controls, draft, yaw control...) are
// another file's.
//
// A Car (3768 bytes, vtable 0x4dbfd0) is a PhobDyno with four Wheels, an Engine / Clutch / Transmission and
// three Differentials, a SphereGroupVolume of 12 spheres fitted to the body model (its collision), and the body
// model itself in five LODs: lod_models are the pristine meshes, live_models the copies that dent. The
// constructor maps every LOD vertex to its nearest LOD-0 vertex (lod_vertex_maps), so ActuallyApplyDamage dents
// LOD 0 and copies the dented positions out to the others; it also marks a 16x16 grid over the body texture's UV
// (damage_grid) for the renderer. reset_damage copies the pristine vertices back.
//
// Written from the v1.0 disassembly: the same sums in the same grouping, register values as double, stored
// values as float, float constants of the same bits, float copies the original makes with integer moves as bit
// copies, and every call made in the original's order with the same arguments -- by address, or through the
// vtable where the original makes a virtual call.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
#define FB(b) __builtin_bit_cast(float, (uint32_t)(b))   // a float constant from its bits

// the unused edx of a __thiscall received as __fastcall (an int, so test/fuzz.h can size it)
typedef int Edx;

static inline uint32_t bits(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static inline float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
#define COPY4(dst, src) memcpy(&(dst), &(src), 4)   // a float moved with integer instructions

// fld dword; fchs; fstp dword -- through the FPU (a signalling NaN comes out quiet), as the original does
static __forceinline float fpu_neg(const float* p) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[p]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "fchs\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [p] "m"(p)
                     : VP_X87_CLOBBERS, "eax", "cc", "memory");
#else
    __asm { mov eax, p
            fld dword ptr [eax]
            fchs
            fstp r }
#endif
    return r;
}
// fld dword; fstp dword -- a copy through the FPU
static __forceinline float fpu_copy(const float* p) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[p]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [p] "m"(p)
                     : VP_X87_CLOBBERS, "eax", "cc", "memory");
#else
    __asm { mov eax, p
            fld dword ptr [eax]
            fstp r }
#endif
    return r;
}

// ---- layouts (file-local, so other files' copies can't collide) --------------------------------------------
namespace {
// Wheel (wheel.obj, 0x1a4): only what car.obj touches here
struct WheelPart {
    uint8_t _000[0x20];
    uint8_t broken, _021[3];           // +0x020
    uint8_t _024[0x5c - 0x24];
    float radius;                      // +0x05c
    uint8_t _060[0xdc - 0x60];
    float steer_input;                 // +0x0dc
    uint8_t _0e0[0x1a4 - 0xe0];
};
static_assert(offsetof(WheelPart, broken) == 0x20 && offsetof(WheelPart, radius) == 0x5c &&
              offsetof(WheelPart, steer_input) == 0xdc && sizeof(WheelPart) == 0x1a4, "WheelPart");

// Engine (0x80): Shaft, block heat, throttle, curve, coolant heat, thermostat (tools/names/car.py)
struct EnginePart {
    uint8_t _00[0x10];
    float rpm;                         // +0x10
    uint8_t _14[0x24 - 0x14];
    float block_temperature;           // +0x24  (block ThermalMass +0x0c)
    uint8_t _28[0x38 - 0x28];
    float smoothed_throttle;           // +0x38
    float redline;                     // +0x3c
    uint8_t _40[0x68 - 0x40];
    float coolant_temperature;         // +0x68  (coolant ThermalMass +0x0c)
    uint8_t _6c[0x80 - 0x6c];
};
static_assert(offsetof(EnginePart, smoothed_throttle) == 0x38 && offsetof(EnginePart, coolant_temperature) == 0x68 &&
              sizeof(EnginePart) == 0x80, "EnginePart");

// Car (car.obj), 3768 bytes. Names from out/types.tsv (tools/names/car.py); _NNN not yet understood.
struct Car : PhobDyno {
    struct BodyVolume* body_volume;   // +0x478
    P3 cockpit_eye;                    // +0x47c
    float last_damage_time;            // +0x488
    uint8_t damaged;                   // +0x48c
    uint8_t _48d;                      // +0x48d  cleared by Reset and reset_damage
    uint16_t damage_grid[16];          // +0x48e
    uint16_t _4ae;
    int32_t lod_models[5];             // +0x4b0
    int32_t live_models[5];            // +0x4c4
    uint16_t* lod_vertex_maps[5];      // +0x4d8
    uint8_t look_back, _4ed[3];        // +0x4ec
    float look_side;                   // +0x4f0
    int32_t realism;                   // +0x4f4
    int32_t yaw_control;               // +0x4f8
    int32_t race_state;                // +0x4fc
    float steering;                    // +0x500
    uint32_t _504;                     // +0x504  reset zeroes it
    float pitch;                       // +0x508
    float opacity;                     // +0x50c
    int32_t car_index;                 // +0x510
    char name[32];                     // +0x514
    uint8_t auto_clutch, _535[3];      // +0x534
    float drag_long, drag_lat, drag_vert;       // +0x538
    float front_lift, rear_lift;                // +0x544
    float front_spoiler_drag, rear_spoiler_drag;// +0x54c
    WheelPart wheels[4];               // +0x554
    uint8_t is_plane, _be5[3];         // +0xbe4
    void* propeller;                   // +0xbe8
    void* wing_left;                   // +0xbec
    void* wing_right;                  // +0xbf0
    void* elevator;                    // +0xbf4
    void* rudder;                      // +0xbf8
    EnginePart engine;                 // +0xbfc
    float clutch_engagement;           // +0xc7c  Clutch +0
    uint8_t _c80[0xc98 - 0xc80];
    uint8_t transmission[0x6c];        // +0xc98
    uint8_t center_diff[0x20];         // +0xd04
    uint8_t front_diff[0x20];          // +0xd24
    uint8_t rear_diff[0x20];           // +0xd44
    int32_t gear;                      // +0xd64
    int32_t engaged_gear;              // +0xd68
    int32_t shift_sound_gear;          // +0xd6c
    float perceived_rpm;               // +0xd70
    uint8_t digital_throttle, _d75[3]; // +0xd74
    int32_t heat_step;                 // +0xd78
    P3 front_axle;                     // +0xd7c
    P3 rear_axle;                      // +0xd88
    P3 center_of_pressure;             // +0xd94
    uint8_t _da0[0xdc4 - 0xda0];
    float torque_balance;              // +0xdc4
    uint32_t _dc8;
    float max_steer_angle;             // +0xdcc
    float rpm_to_speed;                // +0xdd0
    float front_anti_roll;             // +0xdd4
    float rear_anti_roll;              // +0xdd8
    float speed;                       // +0xddc
    float throttle;                    // +0xde0
    float braking;                     // +0xde4
    float ebrake;                      // +0xde8
    float clutch;                      // +0xdec
    P3 air_velocity;                   // +0xdf0
    P3 draft_velocity;                 // +0xdfc
    P3 draft_point;                    // +0xe08
    float draft_radius;                // +0xe14
    float fuel;                        // +0xe18
    float fuel_burn_rate;              // +0xe1c
    uint32_t _e20;                     // +0xe20  reset zeroes it
    uint32_t _e24;                     // +0xe24  reset zeroes it; GetMessage +0x54
    uint8_t horn, _e29[3];             // +0xe28
    uint32_t _e2c;
    float lap_top_speed;               // +0xe30
    uint32_t _e34;
    uint32_t _e38;                     // +0xe38  reset zeroes it
    uint8_t reset_event;               // +0xe3c
    uint8_t lowering;                  // +0xe3d
    uint8_t _e3e[2];
    float scrape_energy;               // +0xe40
    float teleport_time;               // +0xe44
    uint8_t _e48;                      // +0xe48  reset zeroes it
    uint8_t starter_on;                // +0xe49
    uint8_t _e4a[2];
    void* start_sound;                 // +0xe4c
    void* engine_sound;                // +0xe50
    void* shift_sound;                 // +0xe54
    void* road_sound;                  // +0xe58
    void* road_sound_2;                // +0xe5c
    void* scrape_sound;                // +0xe60
    void* horn_sound;                  // +0xe64
    float horn_volume;                 // +0xe68
    Frame start_frame;                 // +0xe6c
    float fuel_capacity;               // +0xe9c
    float fuel_consumption;            // +0xea0
    float lat_g, long_g, vert_g;       // +0xea4
    float steer_feedback;              // +0xeb0
    float aero_g;                      // +0xeb4
};
static_assert(offsetof(Car, body_volume) == 0x478 && offsetof(Car, damage_grid) == 0x48e &&
              offsetof(Car, lod_models) == 0x4b0 && offsetof(Car, lod_vertex_maps) == 0x4d8 &&
              offsetof(Car, steering) == 0x500 && offsetof(Car, opacity) == 0x50c && offsetof(Car, name) == 0x514 &&
              offsetof(Car, drag_long) == 0x538 && offsetof(Car, wheels) == 0x554 && offsetof(Car, is_plane) == 0xbe4 &&
              offsetof(Car, engine) == 0xbfc && offsetof(Car, clutch_engagement) == 0xc7c &&
              offsetof(Car, transmission) == 0xc98 && offsetof(Car, center_diff) == 0xd04 &&
              offsetof(Car, gear) == 0xd64 && offsetof(Car, perceived_rpm) == 0xd70 && offsetof(Car, front_axle) == 0xd7c &&
              offsetof(Car, torque_balance) == 0xdc4 && offsetof(Car, speed) == 0xddc && offsetof(Car, air_velocity) == 0xdf0 &&
              offsetof(Car, draft_radius) == 0xe14 && offsetof(Car, horn) == 0xe28 && offsetof(Car, reset_event) == 0xe3c &&
              offsetof(Car, scrape_energy) == 0xe40 && offsetof(Car, start_sound) == 0xe4c &&
              offsetof(Car, horn_volume) == 0xe68 && offsetof(Car, start_frame) == 0xe6c &&
              offsetof(Car, fuel_capacity) == 0xe9c && offsetof(Car, aero_g) == 0xeb4 && sizeof(Car) == 3768, "Car");

// PhobRoot fields car.obj uses by offset: +0x04 the replay flags word, +0x08 this object's offset in the renderer's
// message buffer, +0x28 the body-space offset of the model origin from the centre of mass (Setup)
static inline uint32_t& root_flags(Car* c) { return c->_004; }
static inline uint32_t msg_offset(const Car* c) { return c->_008; }
static inline float* body_offset(Car* c) { return (float*)&c->_028[0]; }

// CarData (carfile.obj CarFileCombine's output; the PhobData the constructor is given, read as CarData by
// Setup). Units are the car file's (inches, degrees, lb, hp, ft-lb, ft^2). What car.obj reads:
struct CarData {
    uint8_t _000[0x18];                // PhobData: +0 PhobRoot +0x0c, +8 mass, +0xc.. inertia
    Frame start_frame;                 // +0x018
    float body_width;                  // +0x048 in: the hub search looks at +-0.4 of it
    uint32_t _04c;
    float track[2];                    // +0x050 in; a negative rear track makes a plane
    float wheelbase;                   // +0x058 in
    float weight_distribution;         // +0x05c % on the front axle
    float ride_height[2];              // +0x060
    float torque_max;                  // +0x068 ft-lb (PowerCurve arg 3; Clutch x 4)
    float power;                       // +0x06c hp (PowerCurve arg 1)
    float torque_rpm;                  // +0x070 (PowerCurve arg 4)
    float power_rpm;                   // +0x074 (PowerCurve arg 2)
    float idle_rpm;                    // +0x078 (Engine::Setup arg 6)
    float redline;                     // +0x07c (Engine::Setup arg 5)
    float engine_inertia;              // +0x080 lb ft^2 (x 0.04228)
    float engine_drag;                 // +0x084 ft-lb per rpm (x 1.3515)
    float fuel_consumption;            // +0x088
    float fuel_capacity;               // +0x08c gallons
    float final_drive;                 // +0x090 the axle Differentials' ratio
    float gear_ratios[7];              // +0x094 counted while > 0.01
    uint32_t _0b0;
    float torque_balance;              // +0x0b4 rear share
    float diff_stiffness[3];           // +0x0b8 front, rear, centre (x 1.35725)
    uint32_t _0c4;
    float gearbox_drag;                // +0x0c8 ft-lb per rpm (x 1.3515; 0.002 if ~0)
    uint8_t _0cc[0xe4 - 0xcc];
    float sway[2];                     // +0x0e4 lb/in
    uint8_t _0ec[0x128 - 0xec];
    float cm_height;                   // +0x128 in
    float wheel_lock;                  // +0x12c deg
    uint8_t _130[0x16c - 0x130];
    float frontal_area;                // +0x16c ft^2
    float cp_height;                   // +0x170 in, centre of pressure above the front axle
    float cp_back;                     // +0x174 in, and behind it
    float drag_coefficient[3];         // +0x178 long, lat, vert
    float lift[2];                     // +0x184 front, rear
    float spoiler_drag[2];             // +0x18c front, rear
    int32_t car_index;                 // +0x194 (PhobData)
    char name[32];                     // +0x198
    int32_t lod_models[5];             // +0x1b8
    int32_t live_models[5];            // +0x1cc
};
static_assert(offsetof(CarData, body_width) == 0x48 && offsetof(CarData, torque_max) == 0x68 &&
              offsetof(CarData, gear_ratios) == 0x94 && offsetof(CarData, torque_balance) == 0xb4 &&
              offsetof(CarData, gearbox_drag) == 0xc8 && offsetof(CarData, sway) == 0xe4 &&
              offsetof(CarData, cm_height) == 0x128 && offsetof(CarData, frontal_area) == 0x16c &&
              offsetof(CarData, car_index) == 0x194 && offsetof(CarData, lod_models) == 0x1b8 &&
              offsetof(CarData, live_models) == 0x1cc && sizeof(CarData) == 0x1e0, "CarData");

// the renderer's model (mr*): a handle is a model_info*; +0x10 its mrModelInfo
struct mrVertex { P3 pos; float _0c[3]; float u, v; };
static_assert(sizeof(mrVertex) == 32, "mrVertex");
struct mrSurface { uint8_t _00[0x18]; int16_t first_vertex, end_vertex, first_face, end_face; };
struct mrModelInfo { int32_t num_verts; mrVertex* verts; int32_t num_surfaces; mrSurface* surfaces; };

// SphereGroupVolume (phys_spheregroup.cpp): up to 12 member spheres
struct BodyVolume : CollisionVolume {
    void* spheres[12];                 // +0x1c
    int32_t num_spheres;               // +0x4c
    float bound;                       // +0x50
};
static_assert(offsetof(BodyVolume, num_spheres) == 0x4c && sizeof(BodyVolume) == 84, "BodyVolume");

// the Proxer (g_proxer): a per-pair table; entry (car, 0) holds the nearest other car's offset (+4 x, +8 z)
struct Proxer { int32_t n; uint32_t _04, _08; int32_t row_stride; uint8_t* base; };
}  // namespace

// ---- the game's functions these call ----------------------------------------------------------------------
typedef void*(__fastcall* Ctor0_t)(void*, Edx);
typedef void*(__fastcall* PhobDynoCtor_t)(void*, Edx, void* pd, void* p2);
typedef void(__cdecl* ModelGetVerts_t)(int32_t, mrVertex**, int32_t*);
typedef mrModelInfo*(__cdecl* ModelGetInfo_t)(int32_t);
typedef void(__cdecl* ModelBuild_t)(int32_t, mrModelInfo*, int32_t);
typedef void(__cdecl* ModelGetExtents_t)(int32_t, float*, float*, float*, float*, float*, float*);
typedef void*(__cdecl* MemAlloc_t)(int32_t);
typedef void(__cdecl* OperatorDelete_t)(void*);
typedef void(__fastcall* CarInt_t)(Car*, Edx, int32_t);
typedef void(__fastcall* CarVoid_t)(Car*, Edx);
typedef void(__fastcall* CarFloat_t)(Car*, Edx, float);
typedef void(__fastcall* CarSetup_t)(Car*, Edx, const CarData*);
typedef void(__cdecl* RegisterCar_t)(Car*, int32_t);
typedef Car*(__cdecl* FindCar_t)(int32_t);
typedef void*(__cdecl* EngineSoundCreate_t)(Car*);
typedef void(__fastcall* EngineSoundDtor_t)(void*, Edx);
typedef void*(__cdecl* Sound3DCreate_t)(const char*, int32_t, const P3*, const P3*);
typedef void*(__cdecl* SoundDashCreate_t)(const char*, int32_t, int32_t);
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
typedef uint8_t(__cdecl* ResourceExists_t)(const char*);
typedef void*(__cdecl* StringTableGet_t)(const char*);
typedef const char*(__cdecl* StringTableGetEntry_t)(void*, int32_t, int32_t);
typedef void(__cdecl* StringTableForget_t)(void*);
typedef double(__cdecl* Atof_t)(const char*);                              // ST0
typedef int(__cdecl* Atexit_t)(void (*)());
typedef uint8_t(__cdecl* PhysicsIsDamageOn_t)();
typedef double(__cdecl* PhysicsGetTime_t)();                                // ST0: tick x 0.016, unstored
typedef void(__cdecl* ReplayAddEvent_t)(int32_t, const void*, int32_t);
typedef void(__cdecl* ReplayInstall_t)(int32_t, void*);
typedef void(__cdecl* ReplayUninstall_t)(void*);
typedef void(__cdecl* MatMulPoint_t)(P3*, const P3*, const M3*);
typedef void(__cdecl* CrossProduct_t)(P3*, const P3*, const P3*);
typedef void(__fastcall* DynoApplyExternalForce_t)(Car*, Edx, const P3*, const P3*, int32_t);
typedef void(__fastcall* DynoPacket_t)(Car*, Edx, uint8_t*);
typedef void(__fastcall* DynoUpdateReplay_t)(Car*, Edx, const uint8_t*, const uint8_t*, uint32_t);
typedef void(__fastcall* WheelPacket_t)(WheelPart*, Edx, uint8_t*);
typedef void(__fastcall* WheelUpdateReplay_t)(WheelPart*, Edx, const uint8_t*, const uint8_t*, uint32_t, Car*);
typedef void(__fastcall* WheelMessage_t)(WheelPart*, Edx, uint8_t*, Car*);
typedef void(__fastcall* WheelSetup_t)(WheelPart*, Edx, const CarData*, Car*, int32_t, const P3*);
typedef void(__fastcall* ApplyRectDamage_t)(void);   // (cdecl; declared below with its bit-copied floats)
typedef void(__cdecl* RectDamage_t)(uint16_t*, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void(__fastcall* ActuallyApplyDamage_t)(Car*, Edx, const P3*, const P3*, uint8_t, uint8_t);
typedef void*(__fastcall* SphereGroupCtor_t)(void*, Edx, const Frame*, Car*);
typedef void(__fastcall* AddSphere_t)(BodyVolume*, Edx, const P3*, uint32_t, uint32_t, uint32_t, uint32_t,
                                      uint32_t, const P3*);
typedef void(__fastcall* PowerCurveSetup_t)(void*, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void(__fastcall* EngineSetup_t)(void*, Edx, Car*, float, uint32_t, const void*, uint32_t, uint32_t);
typedef void(__fastcall* TransmissionSetup_t)(void*, Edx, Car*, uint32_t, uint32_t, int32_t, const float*, void*, void*);
typedef void(__fastcall* ClutchSetup_t)(void*, Edx, Car*, float);
typedef void(__fastcall* DiffSetup_t)(void*, Edx, Car*, uint32_t, float);
typedef void(__fastcall* Connect2_t)(void*, Edx, void*, void*);
typedef void(__fastcall* Connect1_t)(void*, Edx, void*);
typedef void*(__fastcall* PropellerCtor_t)(void*, Edx, uint32_t, uint32_t, uint32_t);   // P3DBase by value
typedef void(__fastcall* ShaftSetup_t)(void*, Edx, Car*, uint32_t, uint32_t);
typedef void*(__fastcall* WingCtor_t)(void*, Edx, const P3*, uint32_t, uint32_t, uint32_t, int32_t, Car*);

#define PhobDyno_ctor             ((PhobDynoCtor_t)0x004449c0)
#define Wheel_ctor                ((Ctor0_t)0x00447ed0)
#define Engine_ctor               ((Ctor0_t)0x00446630)
#define Clutch_ctor               ((Ctor0_t)0x00445d00)
#define Transmission_ctor         ((Ctor0_t)0x00445f60)
#define Differential_ctor         ((Ctor0_t)0x00445c00)
#define mrModelGetVerts           ((ModelGetVerts_t)0x00457050)
#define mrModelGetInfo            ((ModelGetInfo_t)0x00457030)
#define mrModelBuild              ((ModelBuild_t)0x00455c80)
#define mrModelGetExtents         ((ModelGetExtents_t)0x00456ec0)
#define MemAlloc                  ((MemAlloc_t)0x004140e0)
#define operator_delete           ((OperatorDelete_t)0x00414390)
#define Car_SetTractionControl    ((CarInt_t)0x004395e0)
#define Car_SetABSBraking         ((CarInt_t)0x00439600)
#define Car_SetGear               ((CarInt_t)0x00439710)
#define Car_SetThrottle           ((CarFloat_t)0x004396d0)
#define Car_SetBraking            ((CarFloat_t)0x004396e0)
#define call_Car_Setup            ((CarSetup_t)0x0043a130)     // (hooked below: the rewrite runs when it's in force)
#define call_Car_reset            ((CarVoid_t)0x00439fd0)
#define call_Car_reset_damage     ((CarVoid_t)0x00439e50)
#define call_Car_reset_subordinates ((CarVoid_t)0x00439df0)
#define call_Car_ActuallyApplyDamage ((ActuallyApplyDamage_t)0x00436b40)
#define call_apply_rect_damage    ((RectDamage_t)0x004370a0)
#define PhysTaskRegisterCar       ((RegisterCar_t)0x00426d00)
#define PhysTaskFindCar           ((FindCar_t)0x00426d40)
#define EngineSound_Create        ((EngineSoundCreate_t)0x004725f0)
#define EngineSound_dtor          ((EngineSoundDtor_t)0x00472c10)
#define Sound3D_Create            ((Sound3DCreate_t)0x00472560)
#define SoundDash_Create          ((SoundDashCreate_t)0x00472510)
#define game_sprintf              ((Sprintf_t)0x004cf0a0)
#define ResourceExists            ((ResourceExists_t)0x00419d10)
#define StringTableGet            ((StringTableGet_t)0x0041b210)
#define StringTableGetEntry       ((StringTableGetEntry_t)0x0041b2a0)
#define StringTableForget         ((StringTableForget_t)0x0041b270)
#define game_atof                 ((Atof_t)0x004cfe40)
#define game_atexit               ((Atexit_t)0x004ceff0)
#define PhysicsIsDamageOn         ((PhysicsIsDamageOn_t)0x0042bd60)
#define PhysicsGetTime            ((PhysicsGetTime_t)0x0042bc80)
#define PhysReplayAddEvent        ((ReplayAddEvent_t)0x0042d8b0)       // (an output: captured by the shadow check)
#define PhysReplayInstallEventHandler ((ReplayInstall_t)0x0042d960)
#define PhysReplayUninstallEventHandler ((ReplayUninstall_t)0x0042d990)
#define MatrixMulPoint            ((MatMulPoint_t)0x00429420)
#define MatrixMulPointInv         ((MatMulPoint_t)0x00436120)
#define CrossProduct              ((CrossProduct_t)0x0043b120)
#define PhobDyno_ApplyExternalForce ((DynoApplyExternalForce_t)0x00445970)
#define PhobDyno_MakeReplayPacket ((DynoPacket_t)0x00445940)
#define PhobDyno_UpdateReplay     ((DynoUpdateReplay_t)0x004458d0)
#define PhobDyno_Reset            ((CarVoid_t)0x00444b80)
#define PhobRoot_dtor             ((CarVoid_t)0x0043db50)
#define Wheel_ResetPosition       ((Ctor0_t)0x004480c0)
#define Wheel_dtor                ((Ctor0_t)0x00448100)
#define Wheel_Setup               ((WheelSetup_t)0x00448140)
#define Wheel_MakeReplayPacket    ((WheelPacket_t)0x0044a960)
#define Wheel_UpdateReplay        ((WheelUpdateReplay_t)0x0044a770)
#define Wheel_GetMessage          ((WheelMessage_t)0x0044a650)
#define SphereGroupVolume_ctor    ((SphereGroupCtor_t)0x00433190)
#define SphereGroupVolume_AddSphere ((AddSphere_t)0x004332c0)
#define PowerCurve_Setup          ((PowerCurveSetup_t)0x00446590)
#define Engine_Setup              ((EngineSetup_t)0x00446700)
#define Transmission_Setup        ((TransmissionSetup_t)0x00445fb0)
#define Clutch_Setup              ((ClutchSetup_t)0x00445d20)
#define Differential_Setup        ((DiffSetup_t)0x00445c20)
#define Differential_Connect      ((Connect2_t)0x00445c40)
#define Transmission_Connect      ((Connect1_t)0x00445fa0)
#define Clutch_Connect            ((Connect2_t)0x00445d40)
#define Propeller_ctor            ((PropellerCtor_t)0x00446e60)
#define Shaft_Setup               ((ShaftSetup_t)0x00445e40)
#define Wing_ctor                 ((WingCtor_t)0x00447010)

static void** const VT_Car = (void**)0x004dbfd0;
static void* const HANDLER_DAMAGE = (void*)0x004374e0;      // Car::replay_damage_handler (the address, as registered)
static void* const HANDLER_RESET = (void*)0x00437640;       // Car::replay_reset_handler
static void (*const ATEXIT_ZERO_HUB)() = (void (*)())0x0043afa0;     // the static hub points' destructors
static void (*const ATEXIT_NO_DAMAGE_HUB)() = (void (*)())0x0043af90;

// the game's strings (the original passes these addresses)
static const char* const S_ROAD1 = (const char*)0x004ed7dc;     // "road1.sfx"
static const char* const S_ROAD2 = (const char*)0x004ed7e8;     // "road2.sfx"
static const char* const S_SCRAPE = (const char*)0x004ed7f4;    // "scrape.sfx"
static const char* const S_HORN_FMT = (const char*)0x004ed800;  // "%sh.sfx"
static const char* const S_HORN = (const char*)0x004ed808;      // "horn.sfx"
static const char* const S_START = (const char*)0x004ed814;     // "start.sfx"
static const char* const S_SHIFT = (const char*)0x004ed820;     // "shift1.sfx"
static const char* const S_COCKPIT_FMT = (const char*)0x004ed82c;   // "%s.car/cockpit.tab"

// Setup's two function-local statics (the hub point a wheel gets when no body vertex is near, and the one every
// wheel gets with damage off), their guard bits, and the proximity table
static volatile uint8_t* const g_hub_guard = (volatile uint8_t*)0x00521d48;
static float* const g_zero_hub = (float*)0x00521d38;
static float* const g_no_damage_hub = (float*)0x00521d28;
static Proxer* const* const g_proxer = (Proxer* const*)0x004ec480;

// ---- constants (the original's .rdata floats, by their bits) -----------------------------------------------
static constexpr float K_ONE = FB(0x3f800000);
static constexpr float K_Q15 = FB(0x38000000);         // 2^-15
static constexpr float K_32768 = FB(0x47000000);
static constexpr float K_741 = FB(0x44396aaa);         // 741.666626: the damage threshold impulse
static constexpr float K_15000 = FB(0x466a6000);
static constexpr float K_5 = FB(0x40a00000);
static constexpr float K_2 = FB(0x40000000);
static constexpr float K_M2000 = FB(0xc4fa0000);
static constexpr float K_12000 = FB(0x463b8000);
static constexpr float K_1_2 = FB(0x3f99999a);         // 1.2: steering range
static constexpr float K_0_016 = FB(0x3c83126f);
static constexpr float K_INCH = FB(0x3cd013a9);        // 0.0254
static constexpr float K_MINCH = FB(0xbcd013a9);       // -0.0254
static constexpr float K_0_4 = FB(0x3ecccccd);
static constexpr float K_0_5 = FB(0x3f000000);
static constexpr float K_0_6 = FB(0x3f19999a);
static constexpr float K_FTLB = FB(0x3facfe73);        // 1.35151517
static constexpr float K_DIFF = FB(0x3fadba5e);        // 1.35724998
static constexpr float K_AIR = FB(0x3f251eb8);         // 0.645

// ---- footprint helpers --------------------------------------------------------------------------------------
// the five live models' vertex arrays and their model_info's rebuild bytes (+0x10 the info pointer, +0x15 a flag:
// mrModelBuild(.., 4) rewrites both). A handle is its model_info; its mrModelInfo holds the count and the array.
static void fp_models(Footprint& f, Car* c) {
    for (int i = 0; i < 5; i++) {
        uint8_t* mi = (uint8_t*)(uintptr_t)c->live_models[i];
        if (!mi) continue;
        f.add(mi + 0x10, 8, "model_info");
        mrModelInfo* info = *(mrModelInfo**)(mi + 0x10);
        if (info && info->verts && info->num_verts > 0) f.add(info->verts, (uint32_t)info->num_verts * 32, "model verts");
    }
}
// the body's collision: the SphereGroupVolume and its member spheres (Reset / Update write them)
static void fp_body_volume(Footprint& f, Car* c) {
    BodyVolume* g = c->body_volume;
    if (!g) return;
    f.object(g, "body volume");
    for (int i = 0; i < g->num_spheres && i < 12; i++)
        if (g->spheres[i]) f.object(g->spheres[i], "body sphere");
}
static void fp_car_all(Footprint& f, Car* c) {
    f.object(c, "this");
    fp_models(f, c);
    fp_body_volume(f, c);
}

// =============================================================================================================
// The LOD vertex maps (Car::Car), found fast -- a performance change, not a FIX: the maps come out identical
// =============================================================================================================
// Car::Car maps every vertex j of LODs 1-4 to "its nearest" LOD-0 vertex, which ActuallyApplyDamage copies the
// dents through. The original finds it by brute force (lodmap_original, below, kept verbatim): every LOD-0 vertex
// for every LOD vertex, n0 x (n1+n2+n3+n4) steps per car, for every car on the grid. Stock that's 325 x 828; for a
// detailed mod car (the Willys jeep: 11,040 LOD-0 vertices, LODs of 7,174 / 5,762 x 3) it's ~270M a car, ~2.2
// billion for eight: a long hang at race load. lodmap_search finds the SAME entry for every vertex from a uniform grid
// over LOD 0's positions, looking only at the few vertices near each query. No VP_FIX switch (like the
// draw-performance stages, gl_table.cpp P1/P2): nothing observable changes but the time.
//
// The original's rule, reproduced, not replaced by "the true nearest": best starts at the float 1e8, near_ at 0; for
// k = 0, 1, ... in order, e_k = the double (d1*d1 + d2*d2) + d0*d0 of the float differences, and when !(e_k >= best)
// it takes k and best = (float)e_k. So a candidate wins against the float-ROUNDED previous best; ties go to the
// first k; a NaN would make every later candidate win; a query more than 1e4 from all of LOD 0 keeps 0.
//
// Why running that same rule over only the near candidates gives the same answer. Write m for the least e_k over
// all k (as computed: the same expression, so the same double), k_m for the first k having it, rnd for the round to
// float, and R for the candidates with e_k < 1e8f and e_k <= m (1 + 2^-23). Run the rule over ANY subsequence S of
// the k (in order) that contains R; it ends at the same near_ as over all of them:
//   - If m >= 1e8f, best (<= 1e8f always) never exceeds an e_k: nothing wins in either run, near_ stays 0.
//   - Else, before k_m: a k whose rnd(e_k) <= m (the class Q, empty unless rnd(m) <= m: e_k in [m, m + half an
//     ulp], all in R) can win only against a best > m, i.e. against 1e8f or rnd(e) > m for a non-Q winner; any
//     float above m is >= the next float after rnd(m), which is above every e in Q, so the FIRST Q member before
//     k_m wins, and after it best is rnd(m) <= m and nothing before k_m can win again (every e >= m). With no Q
//     member before k_m, best is still > m at k_m and k_m wins. Either way, just after k_m, best == rnd(m) and the
//     winner is determined by Q and k_m alone -- the same in both runs. Far candidates (outside R) may win and lose
//     in between; they don't change this, since what happens at the first Q member and at k_m depends only on best
//     being > m.
//   - After k_m, best == rnd(m) and stays so (a winner e < rnd(m) is >= m, so rnd(e) == rnd(m)): exactly the
//     candidates with e < rnd(m) win -- all in R, in both runs -- and the last of them, if any, is the answer.
// The grid search below puts every k with e_k <= T in S, for T = min(m' (1 + 1e-5) + 1e-30, 1e8f (1 + 1e-5)) where
// m' (>= m) is the least e it has seen (k_m is never skipped, so finally m' == m): far more than R needs. A cell is
// skipped only when a lower bound on the TRUE squared distance to anything in it, shrunk by 1e-5, minus 1e-30, still
// exceeds T. The computed e_k is the true distance with at most ~3.1e-7 relative error (each float difference
// rounded once, three products, two sums; under the physics thread's 24-bit precision or in double), plus a
// negligible absolute error for denormal differences -- so a skipped vertex has e_k > T. The cell bounds allow for
// the rounding of the cell index too (slack below). The search computes e_k with the same expression as the rule
// (lodmap_d2), and the rule itself runs over S verbatim, in k order.
//
// Faithful by construction where the bounds don't hold: lodmap_original runs instead when any coordinate of LOD 0 or
// of the LOD is NaN, infinite or beyond 1e15 (the squares then can't overflow anything), when n0 x n is under 2M (the
// brute force is instant there; stock cars always), or when the grid's memory can't be had. The u16 truncation of
// k is kept as is: past 65,535 LOD-0 vertices k wraps, in both versions alike (not fixed here).
enum { LODMAP_FAST_MIN = 2000000 };                 // n0 x n below this: the original's loop

// the original's search for one LOD (1-4), verbatim from Car::Car
static void lodmap_original(const mrVertex* v0, int32_t n0, const mrVertex* v, int32_t n, uint16_t* map) {
    for (int j = 0; j < n; j++) {
        float best = FB(0x4cbebc20);                             // 1e8
        uint16_t near_ = 0;
        for (int k = 0; k < n0; k++) {
            float d[3];
            d[0] = (float)(D(v0[k].pos.x) - v[j].pos.x);
            d[1] = (float)(D(v0[k].pos.y) - v[j].pos.y);
            d[2] = (float)(D(v0[k].pos.z) - v[j].pos.z);
            double d2 = (D(d[1]) * d[1] + D(d[2]) * d[2]) + D(d[0]) * d[0];
            const float d2s = (float)d2;
            if (!(d2 >= best)) {                                  // fcom; test ah,1
                near_ = (uint16_t)k;
                best = d2s;
            }
        }
        map[j] = near_;
    }
}

// e_k exactly as the original computes it (the same expression, rounded to double where it assigns d2)
static inline double lodmap_d2(const mrVertex& a, const mrVertex& b) {
    float d[3];
    d[0] = (float)(D(a.pos.x) - b.pos.x);
    d[1] = (float)(D(a.pos.y) - b.pos.y);
    d[2] = (float)(D(a.pos.z) - b.pos.z);
    double d2 = (D(d[1]) * d[1] + D(d[2]) * d[2]) + D(d[0]) * d[0];
    return d2;
}
static bool lodmap_bounded(const mrVertex* p, int32_t n) {          // every coordinate finite and within 1e15
    const double lim = (double)1e15;
    for (int32_t i = 0; i < n; i++)
        if (!(fabs(D(p[i].pos.x)) <= lim) || !(fabs(D(p[i].pos.y)) <= lim) || !(fabs(D(p[i].pos.z)) <= lim)) return false;
    return true;
}
static int lodmap_cmp_k(const void* a, const void* b) {
    const int32_t x = *(const int32_t*)a, y = *(const int32_t*)b;
    return x < y ? -1 : x > y;
}

// The grid search for one LOD (no work threshold: the harness calls it directly). false: not applicable (a
// coordinate out of bounds, no memory) -- map untouched, the caller runs lodmap_original.
static bool lodmap_search(const mrVertex* v0, int32_t n0, const mrVertex* v, int32_t n, uint16_t* map) {
    if (n0 < 1 || n < 1) return false;
    if (!lodmap_bounded(v0, n0) || !lodmap_bounded(v, n)) return false;
    // the grid: origin lo (LOD 0's least corner), cubic cells of side `cell`, about 8 n0 of them (a car's vertices
    // lie on its surface, so most cells are empty; on the jeeps 8 n0 was quickest, from n0 / 8 to 128 n0)
    double lo[3] = {D(v0[0].pos.x), D(v0[0].pos.y), D(v0[0].pos.z)}, hi[3] = {lo[0], lo[1], lo[2]};
    for (int32_t k = 1; k < n0; k++) {
        const double p[3] = {D(v0[k].pos.x), D(v0[k].pos.y), D(v0[k].pos.z)};
        for (int a = 0; a < 3; a++) {
            if (p[a] < lo[a]) lo[a] = p[a];
            if (p[a] > hi[a]) hi[a] = p[a];
        }
    }
    double ext[3], mx = 0;
    for (int a = 0; a < 3; a++) {
        ext[a] = hi[a] - lo[a];
        if (ext[a] > mx) mx = ext[a];
    }
    double cell = 1;
    if (mx > 0) {
        double e[3];
        for (int a = 0; a < 3; a++) e[a] = ext[a] > mx / 64 ? ext[a] : mx / 64;   // flat meshes: a floor per axis
        cell = cbrt(e[0] * e[1] * e[2] / n0) / 2;
        if (!(cell >= mx / 1000)) cell = mx / 1000;               // at most ~1000 cells an axis
        if (!(cell >= (double)1e-20)) cell = (double)1e-20;
    }
    const double inv = 1 / cell;
    cell = 1 / inv;                                               // the side the indices below are in
    // each LOD-0 vertex's cell, per axis: floor((x - lo) * inv). Monotonic in x and >= 0; the grid is sized from
    // the indices themselves, so no vertex is clamped
    int32_t* ax = (int32_t*)malloc((size_t)n0 * 3 * sizeof(int32_t));
    if (!ax) return false;
    int g[3] = {1, 1, 1};
    for (int32_t k = 0; k < n0; k++) {
        const double p[3] = {D(v0[k].pos.x), D(v0[k].pos.y), D(v0[k].pos.z)};
        for (int a = 0; a < 3; a++) {
            const int c = (int)floor((p[a] - lo[a]) * inv);
            ax[k * 3 + a] = c;
            if (c + 1 > g[a]) g[a] = c + 1;
        }
    }
    const int64_t ncells = (int64_t)g[0] * g[1] * g[2];
    int32_t* start = ncells <= (1 << 24) ? (int32_t*)calloc((size_t)ncells + 1, sizeof(int32_t)) : 0;
    int32_t* pts = (int32_t*)malloc((size_t)n0 * sizeof(int32_t));
    int32_t* cand_k = (int32_t*)malloc((size_t)n0 * sizeof(int32_t));
    double* cand_e = (double*)malloc((size_t)n0 * sizeof(double));
    if (!start || !pts || !cand_k || !cand_e) {
        free(ax); free(start); free(pts); free(cand_k); free(cand_e);
        return false;
    }
    // counting sort by cell, k ascending within a cell
    for (int32_t k = 0; k < n0; k++) {
        ax[k * 3] = (ax[k * 3] * g[1] + ax[k * 3 + 1]) * g[2] + ax[k * 3 + 2];
        start[ax[k * 3] + 1]++;
    }
    for (int64_t c = 0; c < ncells; c++) start[c + 1] += start[c];
    for (int32_t k = 0; k < n0; k++) pts[start[ax[k * 3]]++] = k;
    for (int64_t c = ncells; c > 0; c--) start[c] = start[c - 1];
    start[0] = 0;
    free(ax);

    const double shrink = 1 - (double)1e-5, tiny = (double)1e-30, grow = 1 + (double)1e-5;
    const double cap = (double)FB(0x4cbebc20) * grow;             // 1e8f: no e_k >= it ever wins
    for (int32_t j = 0; j < n; j++) {
        const double q[3] = {D(v[j].pos.x), D(v[j].pos.y), D(v[j].pos.z)};
        double cq[3], sl[3], slmax = 0, box = 0;
        int cc[3], rmax = 0;
        for (int a = 0; a < 3; a++) {
            const double off = q[a] - lo[a];
            cq[a] = floor(off * inv);                             // the query's own cell index (may be outside)
            cc[a] = cq[a] < 0 ? 0 : cq[a] > g[a] - 1 ? g[a] - 1 : (int)cq[a];   // the ring centre, in the grid
            // the cell indices' rounding: (x - lo) * inv is off by a few 1e-8 of |x - lo| at 24-bit precision
            sl[a] = (ext[a] + fabs(off) + cell) * (double)4e-6;
            if (sl[a] > slmax) slmax = sl[a];
            const int far_ = cc[a] > g[a] - 1 - cc[a] ? cc[a] : g[a] - 1 - cc[a];
            if (far_ > rmax) rmax = far_;
            double out = off < 0 ? -off : q[a] - hi[a];           // outside LOD 0's box along this axis
            out -= sl[a];
            if (out > 0) box += out * out;
        }
        int32_t nc = 0;
        double T = cap, mbest = cap;
        if (!(box * shrink - tiny > T)) {                         // else all of LOD 0 is beyond 1e4: no candidate
            for (int r = 0; r <= rmax; r++) {
                if (r >= 2) {                                     // every cell left is >= r - 1 cells away on an axis
                    const double a = (r - 1) * cell - slmax;
                    if (a > 0 && a * a * shrink - tiny > T) break;
                }
                const int x0 = cc[0] - r < 0 ? 0 : cc[0] - r, x1 = cc[0] + r > g[0] - 1 ? g[0] - 1 : cc[0] + r;
                const int y0 = cc[1] - r < 0 ? 0 : cc[1] - r, y1 = cc[1] + r > g[1] - 1 ? g[1] - 1 : cc[1] + r;
                const int z0 = cc[2] - r < 0 ? 0 : cc[2] - r, z1 = cc[2] + r > g[2] - 1 ? g[2] - 1 : cc[2] + r;
                for (int x = x0; x <= x1; x++)
                    for (int y = y0; y <= y1; y++) {
                        const bool shell = x == cc[0] - r || x == cc[0] + r || y == cc[1] - r || y == cc[1] + r;
                        for (int z = z0; z <= z1; z++) {
                            if (!shell && z != cc[2] - r && z != cc[2] + r) {   // the ring's inside: done already
                                z = cc[2] + r - 1;
                                continue;
                            }
                            // a lower bound on the true squared distance to anything in cell (x, y, z)
                            const int c[3] = {x, y, z};
                            double lb = 0;
                            for (int a = 0; a < 3; a++) {
                                const double gap = fabs(c[a] - cq[a]) - 1;
                                if (gap > 0) {
                                    const double d = gap * cell - sl[a];
                                    if (d > 0) lb += d * d;
                                }
                            }
                            if (lb * shrink - tiny > T) continue;
                            const int64_t id = ((int64_t)x * g[1] + y) * g[2] + z;
                            for (int32_t p = start[id]; p < start[id + 1]; p++) {
                                const int32_t k = pts[p];
                                const double e = lodmap_d2(v0[k], v[j]);
                                if (!(e <= T)) continue;
                                cand_k[nc] = k;
                                cand_e[nc++] = e;
                                if (e < mbest) {
                                    mbest = e;
                                    const double t = mbest * grow + tiny;
                                    T = t < cap ? t : cap;
                                }
                            }
                        }
                    }
            }
        }
        // S: the candidates within the final T, in k order; then the original's rule over them, verbatim
        int32_t ns = 0;
        for (int32_t i = 0; i < nc; i++)
            if (cand_e[i] <= T) cand_k[ns++] = cand_k[i];
        if (ns > 1) qsort(cand_k, (size_t)ns, sizeof(int32_t), lodmap_cmp_k);
        float best = FB(0x4cbebc20);                              // 1e8
        uint16_t near_ = 0;
        for (int32_t i = 0; i < ns; i++) {
            const int k = cand_k[i];
            float d[3];
            d[0] = (float)(D(v0[k].pos.x) - v[j].pos.x);
            d[1] = (float)(D(v0[k].pos.y) - v[j].pos.y);
            d[2] = (float)(D(v0[k].pos.z) - v[j].pos.z);
            double d2 = (D(d[1]) * d[1] + D(d[2]) * d[2]) + D(d[0]) * d[0];
            const float d2s = (float)d2;
            if (!(d2 >= best)) {
                near_ = (uint16_t)k;
                best = d2s;
            }
        }
        map[j] = near_;
    }
    free(start); free(pts); free(cand_k); free(cand_e);
    return true;
}

// one LOD's map, as Car::Car builds it: the grid search where the brute force would be slow, else the original
static void lodmap_build(const mrVertex* v0, int32_t n0, const mrVertex* v, int32_t n, uint16_t* map) {
    if ((int64_t)n0 * n >= LODMAP_FAST_MIN && lodmap_search(v0, n0, v, n, map)) return;
    lodmap_original(v0, n0, v, n, map);
}

// =============================================================================================================
// Car::Car (0x4364c0)
// =============================================================================================================
// The parts' constructors, Car's vtable; each LOD's vertex map (LOD 0 to itself, the others to their nearest LOD-0
// vertex, the first of equals: lodmap_build, above); the defaults; Setup and reset; register with the physics task;
// the sounds.
static Car* __fastcall Car_ctor(Car* self, Edx, CarData* pd, void* p2) {
    char buf[256];
    PhobDyno_ctor(self, 0, pd, p2);
    for (int i = 0; i < 4; i++) Wheel_ctor(&self->wheels[i], 0);
    Engine_ctor(&self->engine, 0);
    Clutch_ctor(&self->clutch_engagement, 0);
    Transmission_ctor(self->transmission, 0);
    Differential_ctor(self->center_diff, 0);
    Differential_ctor(self->front_diff, 0);
    Differential_ctor(self->rear_diff, 0);
    int32_t n0 = 0;
    self->vtable = VT_Car;
    mrVertex* v0;
    mrModelGetVerts(pd->lod_models[0], &v0, &n0);
    for (int lod = 0; lod < 5; lod++) {
        self->lod_models[lod] = pd->lod_models[lod];
        self->live_models[lod] = pd->live_models[lod];
        int32_t n = 0;
        mrVertex* v;
        mrModelGetVerts(self->lod_models[lod], &v, &n);
        uint16_t* const map = self->lod_vertex_maps[lod] = (uint16_t*)MemAlloc(n + n);
        if (lod == 0)
            for (int j = 0; j < n; j++) map[j] = (uint16_t)j;
        else
            lodmap_build(v0, n0, v, n, map);                      // the original's search, or the same maps fast
    }
    self->name[0] = 0;
    self->realism = 2;
    Car_SetTractionControl(self, 0, 0);
    Car_SetABSBraking(self, 0, 0);
    self->auto_clutch = 0;
    self->digital_throttle = 0;
    self->yaw_control = 0;
    const int32_t idx = pd->car_index;
    self->car_index = idx;
    self->heat_step = idx;
    self->opacity = 1.0f;
    memset(self->damage_grid, 0, 32);
    self->is_plane = 0;
    self->race_state = 2;
    self->scrape_energy = 0.0f;
    call_Car_Setup(self, 0, pd);
    call_Car_reset(self, 0);
    PhysTaskRegisterCar(self, self->car_index);
    const P3* vel = &self->velocity;
    const P3* pos = &self->frame.pos;
    self->engine_sound = EngineSound_Create(self);
    void* s;
    if ((s = self->road_sound = Sound3D_Create(S_ROAD1, 7, vel, pos)) != 0) *(int32_t*)((uint8_t*)s + 0x10) = self->car_index;
    if ((s = self->road_sound_2 = Sound3D_Create(S_ROAD2, 7, vel, pos)) != 0) *(int32_t*)((uint8_t*)s + 0x10) = self->car_index;
    if ((s = self->scrape_sound = Sound3D_Create(S_SCRAPE, 4, vel, pos)) != 0) *(int32_t*)((uint8_t*)s + 0x10) = self->car_index;
    game_sprintf(buf, S_HORN_FMT, self->name);
    if (!ResourceExists(buf)) game_sprintf(buf, S_HORN);
    if ((s = self->horn_sound = Sound3D_Create(buf, 5, vel, pos)) != 0) *(int32_t*)((uint8_t*)s + 0x10) = self->car_index;
    if ((s = self->start_sound = Sound3D_Create(S_START, 5, vel, pos)) != 0) *(int32_t*)((uint8_t*)s + 0x10) = self->car_index;
    uint8_t* sd = (uint8_t*)SoundDash_Create(S_SHIFT, 6, self->car_index);
    self->shift_sound = sd;
    if (sd) {
        if (*(uint32_t*)(sd + 0x30) != 0x3e4ccccdu) {          // volume 0.2 (and its changed flag)
            sd[0x34] = 1;
            *(uint32_t*)(sd + 0x30) = 0x3e4ccccdu;
        }
        sd = (uint8_t*)self->shift_sound;
        if (*(uint32_t*)(sd + 8) != 0x3f19999au) {              // 0.6
            sd[0x2c] = 1;
            *(uint32_t*)(sd + 8) = 0x3f19999au;
        }
    }
    return self;
}
static void fp_car_ctor(Footprint& f, Car*, Edx, CarData*, void*) {
    f.replay_only = "allocates (vertex maps, the body volume, tyres), registers the car and creates its sounds";
}
PORT_FN(0x004364c0, "Car::Car", Car_ctor, fp_car_ctor)

// =============================================================================================================
// Car::~Car (0x436920): a plane's parts, the sounds (the engine sound by its own destructor, the rest by their
// deleting destructors), the vertex maps, the wheels last to first, then PhobRoot's destructor
// =============================================================================================================
static void __fastcall Car_dtor(Car* self, Edx) {
    self->vtable = VT_Car;
    if (self->is_plane) {
        operator_delete(self->propeller);
        operator_delete(self->rudder);
        operator_delete(self->elevator);
        operator_delete(self->wing_left);
        operator_delete(self->wing_right);
    }
    self->car_index = -1;
    if (void* es = self->engine_sound) {
        EngineSound_dtor(es, 0);
        operator_delete(es);
        self->engine_sound = 0;
    }
    void** const slots[6] = {&self->road_sound, &self->road_sound_2, &self->scrape_sound, &self->horn_sound,
                             &self->shift_sound, &self->start_sound};
    for (int i = 0; i < 6; i++) {
        if (void* s = *slots[i]) {
            VFN(s, 0, void*, uint32_t)(s, 0, 1);
            *slots[i] = 0;
        }
    }
    for (int i = 0; i < 5; i++) operator_delete(self->lod_vertex_maps[i]);
    for (int i = 3; i >= 0; i--) Wheel_dtor(&self->wheels[i], 0);
    PhobRoot_dtor(self, 0);
}
static void fp_car_dtor(Footprint& f, Car*, Edx) { f.replay_only = "frees the parts, the sounds and the vertex maps"; }
PORT_FN(0x00436920, "Car::~Car", Car_dtor, fp_car_dtor)

// =============================================================================================================
// Car::ApplyExternalForce (0x436a90, vtable +0x34): above 2 m/s, scraping forces feed the scrape sound
// (|F|^2 x a ramp from 4 to 10 (m/s)^2), then PhobDyno's
// =============================================================================================================
static void __fastcall Car_ApplyExternalForce(Car* self, Edx, const P3* force, const P3* point, int32_t surface) {
    const P3& v = self->velocity;
    double v2 = (D(v.y) * v.y + D(v.z) * v.z) + D(v.x) * v.x;
    const float v2s = (float)v2;
    if (v2 > D(4.0f)) {                                          // fcom; test ah,0x41; jne
        float ramp = 1.0f;
        if ((int32_t)bits(v2s) < 0x41200000) ramp = (float)((D(v2s) - 4.0f) * FB(0x3e2aaaab));   // < 10.0
        self->scrape_energy = (float)(((D(force->y) * force->y + D(force->z) * force->z) + D(force->x) * force->x) * ramp +
                                      self->scrape_energy);
    }
    PhobDyno_ApplyExternalForce(self, 0, force, point, surface);
}
static void fp_car_apply_external_force(Footprint& f, Car* self, Edx, const P3*, const P3*, int32_t) {
    f.object(self, "this");
}
PORT_FN(0x00436a90, "Car::ApplyExternalForce", Car_ApplyExternalForce, fp_car_apply_external_force)

// =============================================================================================================
// apply_rect_damage (0x4370a0): the UV rectangle [u0,u1) x [v0,v1), x 15.99 and truncated, ORed into the 16x16 grid
// (column bits of a 16-bit row; a shift of 16..31 sets nothing). No bounds: the rows are the caller's to keep in range.
// =============================================================================================================
static void __cdecl apply_rect_damage(uint16_t* grid, float u0, float v0, float u1, float v1) {
    const int32_t r0 = x87_ftol(D(v0) * FB(0x417fd70a));
    const int32_t c1 = x87_ftol(D(u1) * FB(0x417fd70a));
    // (a 32-bit mask: built in 16 bits, MSVC ORs with `bts cx, ax`, which takes the bit number mod 16, where the
    // original's `shl ax, cl` shifts a count of 16..31 out altogether)
    uint32_t mask32 = 0;
    const int32_t r1 = x87_ftol(D(v1) * FB(0x417fd70a));
    const int32_t c0 = x87_ftol(D(u0) * FB(0x417fd70a));
    for (int32_t c = c0; c < c1; c++) mask32 |= 1u << (c & 31);
    const uint16_t mask = (uint16_t)mask32;
    if (r1 > r0) {
        uint16_t* p = grid + r0;
        for (int32_t n = r1 - r0; n; n--) *p++ |= mask;
    }
}
// not `pure`: the generic fuzzer's random floats would send the rows far outside its arena; test/world_car2.cpp
// fuzzes it on bounded rectangles instead. The rows it writes are exactly these:
static void fp_apply_rect_damage(Footprint& f, uint16_t* grid, float, float v0, float, float v1) {
    const int32_t r0 = x87_ftol(D(v0) * FB(0x417fd70a)), r1 = x87_ftol(D(v1) * FB(0x417fd70a));
    if (r1 > r0) f.add(grid + r0, (uint32_t)(r1 - r0) * 2, "damage rows");
}
PORT_FN(0x004370a0, "apply_rect_damage", apply_rect_damage, fp_apply_rect_damage)

// =============================================================================================================
// Car::ActuallyApplyDamage (0x436b40): dent the body at the nearest LOD-0 vertex to the hit point
// =============================================================================================================
// force (negated when `reversed`) and point are body space. The point is pushed out 30% from (0, 1, 0), snapped to
// the nearest vertex (within 1000 m) and moved 0.48 m back along the force; every vertex within 0.6 m of it moves
// along the force by min((|force| - 741.67) x 0.0027, 40) x 0.016 (unless `no_dent`), and its UV (in the first
// surface's vertex range; v in [0, 0.75) and [0.75, 1) as two rectangles) marks the damage grid. Then LOD 0 is
// rebuilt and copied through the vertex maps to LODs 1..4.
static void __fastcall Car_ActuallyApplyDamage(Car* self, Edx, const P3* force, const P3* point, uint8_t reversed,
                                               uint8_t no_dent) {
    self->damaged = 1;
    float d[3];
    memcpy(d, force, 12);
    if (reversed) {
        d[0] = fpu_neg(&d[0]);
        d[1] = fpu_neg(&d[1]);
        d[2] = fpu_neg(&d[2]);
    }
    float p[3];
    memcpy(p, point, 12);
    p[1] = (float)(D(p[1]) - K_ONE);
    p[0] = (float)(D(p[0]) * FB(0x3fa66666));
    p[1] = (float)(D(p[1]) * FB(0x3fa66666));
    p[2] = (float)(D(p[2]) * FB(0x3fa66666));
    p[1] = (float)(D(p[1]) + K_ONE);
    mrVertex* verts;
    int32_t count;
    mrModelGetVerts(self->live_models[0], &verts, &count);
    const double len = x87_sqrt((D(d[1]) * d[1] + D(d[2]) * d[2]) + D(d[0]) * d[0]);
    const float excess = (float)(len - K_741);
    float n[3];
    n[0] = (float)(D(d[0]) / len);
    n[1] = (float)(D(d[1]) / len);
    n[2] = (float)(D(d[2]) / len);
    float best = FB(0x49742400);                                 // 1e6
    memcpy(d, p, 12);
    for (int32_t i = 0; i < count; i++) {
        const float* v = &verts[i].pos.x;
        float e[3];
        e[0] = (float)(D(v[0]) - p[0]);
        e[1] = (float)(D(v[1]) - p[1]);
        e[2] = (float)(D(v[2]) - p[2]);
        double d2 = (D(e[1]) * e[1] + D(e[2]) * e[2]) + D(e[0]) * e[0];
        const float d2s = (float)d2;
        if (!(d2 >= best)) {
            best = d2s;
            memcpy(d, v, 12);
        }
    }
    memcpy(p, d, 12);
    p[0] = (float)(D(n[0]) * FB(0xbef5c290) + p[0]);             // -0.48
    p[1] = (float)(D(n[1]) * FB(0xbef5c290) + p[1]);
    p[2] = (float)(D(n[2]) * FB(0xbef5c290) + p[2]);
    const mrModelInfo* info = mrModelGetInfo(self->live_models[0]);
    const int32_t first = info->surfaces->first_vertex, end = info->surfaces->end_vertex;
    const float lo[2] = {0.0f, FB(0x3f400000)}, hi[2] = {FB(0x3f400000), 1.0f};   // 0.75
    uint16_t* const grid = self->damage_grid;
    for (int pass = 0; pass < 2; pass++) {
        float umin = 1.0f, vmin = 1.0f, umax = 0.0f, vmax = 0.0f;
        if (count > 0) {
            // the four bounds live in x87 registers across the loop
            double r_umin = umin, r_vmin = vmin, r_umax = umax, r_vmax = vmax;
            for (int32_t i = 0; i < count; i++) {
                mrVertex* vx = &verts[i];
                float e[3];
                e[0] = (float)(D(vx->pos.x) - p[0]);
                e[1] = (float)(D(vx->pos.y) - p[1]);
                e[2] = (float)(D(vx->pos.z) - p[2]);
                double d2 = (D(e[1]) * e[1] + D(e[2]) * e[2]) + D(e[0]) * e[0];
                if (d2 >= D(FB(0x3eb851ec))) continue;           // 0.36 (fcomp st(1); test ah,1)
                double kr = D(excess) * FB(0x3b30b9ef);
                float k = (float)kr;
                if (kr > D(40.0f)) k = 40.0f;
                if (!no_dent) {
                    vx->pos.x = (float)((D(n[0]) * k) * K_0_016 + vx->pos.x);
                    vx->pos.y = (float)((D(n[1]) * k) * K_0_016 + vx->pos.y);
                    vx->pos.z = (float)((D(n[2]) * k) * K_0_016 + vx->pos.z);
                }
                const float vv = vx->v;
                if (i < first || i >= end) continue;
                if (D(lo[pass]) > vv) continue;                  // test ah,0x41; je
                if (!(D(hi[pass]) > vv)) continue;               // test ah,0x41; jne
                const double x1 = D(vx->u) - (float)0.05f;
                r_umin = r_umin > x1 ? x1 : r_umin;
                const double y1 = D(vx->v) - (float)0.05f;
                r_vmin = r_vmin > y1 ? y1 : r_vmin;
                const double x2 = D(vx->u) + (float)0.05f;
                r_umax = !(r_umax >= x2) ? x2 : r_umax;
                const double y2 = D(vx->v) + (float)0.05f;
                r_vmax = !(r_vmax >= y2) ? y2 : r_vmax;
            }
            umax = (float)r_umax;
            vmin = (float)r_vmin;
            umin = (float)r_umin;
            vmax = (float)r_vmax;
        }
        call_apply_rect_damage(grid, bits(umin), bits(vmin), bits(umax), bits(vmax));
    }
    const int32_t m0 = self->live_models[0];
    mrModelBuild(m0, mrModelGetInfo(m0), 4);
    mrVertex* v0;
    int32_t n0 = 0;
    mrModelGetVerts(self->live_models[0], &v0, &n0);
    for (int lod = 1; lod < 5; lod++) {
        mrVertex* vl;
        int32_t nl = 0;
        mrModelGetVerts(self->live_models[lod], &vl, &nl);
        for (int32_t j = 0; j < nl; j++) memcpy(&vl[j].pos, &v0[self->lod_vertex_maps[lod][j]].pos, 12);
        const int32_t m = self->live_models[lod];
        mrModelBuild(m, mrModelGetInfo(m), 4);
    }
}
static void fp_car_damage(Footprint& f, Car* self, Edx, const P3*, const P3*, uint8_t, uint8_t) {
    f.object(self, "this");
    fp_models(f, self);
}
PORT_FN(0x00436b40, "Car::ActuallyApplyDamage", Car_ActuallyApplyDamage, fp_car_damage)

// =============================================================================================================
// Car::ResolveExternalImpulse (0x437120, vtable +0x38): an impulse over 741.67 (with damage on) dents the car
// =============================================================================================================
// The impulse and its point go into body space (the point relative to the model origin), are packed into a
// 14-byte replay event (5: point 3 x 8 bits over +-5 m, impulse 3 x 16 bits over +-15000, 4 zero bytes, the car)
// and handed to ApplyDamage (vtable +0x3c: Car's and NetCar's both call ActuallyApplyDamage).
static uint16_t pack_q(double center, float v, const float* two_center) {
    return (uint16_t)x87_ftol(((center + v) / *two_center) * K_32768);
}
static void __fastcall Car_ResolveExternalImpulse(Car* self, Edx, const P3* impulse, const P3* point, int32_t surface) {
    (void)surface;
    if (!PhysicsIsDamageOn()) return;
    const double m2 = (D(impulse->x) * impulse->x + D(impulse->y) * impulse->y) + D(impulse->z) * impulse->z;
    volatile float k741 = K_741;                                 // squared at run time, in single precision
    const double k2 = D(k741) * k741;
    if (k2 >= m2) return;                                        // fcompp; test ah,1; je
    self->damaged = 1;
    self->last_damage_time = (float)PhysicsGetTime();
    M3 rot;
    memcpy(&rot, &self->frame.rot, 36);
    P3 w;
    MatrixMulPoint(&w, (const P3*)body_offset(self), &self->frame.rot);
    w.x = (float)(D(self->frame.pos.x) + w.x);
    w.y = (float)(D(self->frame.pos.y) + w.y);
    w.z = (float)(D(self->frame.pos.z) + w.z);
    P3 rel;
    rel.x = (float)(D(point->x) - w.x);
    rel.y = (float)(D(point->y) - w.y);
    rel.z = (float)(D(point->z) - w.z);
    MatrixMulPointInv(&rel, &rel, &rot);
    P3 imp;
    MatrixMulPointInv(&imp, impulse, &rot);
    uint8_t pkt[14];
    const uint8_t index = (uint8_t)self->car_index;
    // the impulse: x clamps NaN to +15000, y and z to -15000 (two different compare sequences)
    float lo = fpu_neg(&K_15000);
    volatile float two = K_15000;
    float twice = (float)(D(two) * K_2);                         // 30000, computed and stored as the original does
    float v;
    if (D(lo) > imp.x) v = lo;
    else v = (float)(D(K_15000) > imp.x ? D(imp.x) : D(K_15000));
    uint16_t q = pack_q(D(K_15000), v, &twice);
    memcpy(pkt + 3, &q, 2);
    if (!(D(imp.y) >= lo)) v = lo;
    else v = (float)(D(K_15000) > imp.y ? D(imp.y) : D(K_15000));
    q = pack_q(D(K_15000), v, &twice);
    memcpy(pkt + 5, &q, 2);
    if (!(D(imp.z) >= lo)) v = lo;
    else v = (float)(D(K_15000) > imp.z ? D(imp.z) : D(K_15000));
    q = pack_q(D(K_15000), v, &twice);
    memcpy(pkt + 7, &q, 2);
    // the point: the same with 5 m, the high byte of each
    lo = fpu_neg(&K_5);
    volatile float five = K_5;
    twice = (float)(D(five) * K_2);
    if (D(lo) > rel.x) v = lo;
    else v = (float)(D(K_5) > rel.x ? D(rel.x) : D(K_5));
    pkt[0] = (uint8_t)(pack_q(D(K_5), v, &twice) >> 8);
    if (!(D(rel.y) >= lo)) v = lo;
    else v = (float)(D(K_5) > rel.y ? D(rel.y) : D(K_5));
    pkt[1] = (uint8_t)(pack_q(D(K_5), v, &twice) >> 8);
    if (!(D(rel.z) >= lo)) v = lo;
    else v = (float)(D(K_5) > rel.z ? D(rel.z) : D(K_5));
    pkt[2] = (uint8_t)(pack_q(D(K_5), v, &twice) >> 8);
    memset(pkt + 9, 0, 4);
    pkt[13] = index;
    PhysReplayAddEvent(5, pkt, 14);
    VFN(self, 0x3c, void, const P3*, const P3*, uint8_t)(self, 0, &imp, &rel, 0);
}
static void fp_car_resolve(Footprint& f, Car* self, Edx, const P3*, const P3*, int32_t) {
    f.object(self, "this");                                      // sized by the class (an AICar's own fields too)
    fp_models(f, self);                                          // ApplyDamage -> ActuallyApplyDamage
}
PORT_FN(0x00437120, "Car::ResolveExternalImpulse", Car_ResolveExternalImpulse, fp_car_resolve)

// =============================================================================================================
// Car::replay_damage_handler (0x4374e0, replay event 5): unpack ResolveExternalImpulse's event and apply the damage
// again; mode 2 (rewinding) undoes all damage instead. Returns the event's size.
// =============================================================================================================
static int __cdecl Car_replay_damage_handler(const uint8_t* ev, int32_t mode) {
    Car* car = PhysTaskFindCar((int32_t)(int8_t)ev[13]);
    if (mode == 2) {
        call_Car_reset_damage(car, 0);
        return 14;
    }
    volatile float five = K_5, fifteen = K_15000;
    const double k5 = D(five) * K_2;                             // kept in registers
    uint16_t w;
    P3 rel, imp;
    const double r2 = (double)(uint16_t)(ev[2] << 8), r1 = (double)(uint16_t)(ev[1] << 8),
                 r0 = (double)(uint16_t)(ev[0] << 8);
    const double z = (r2 * k5) * K_Q15 - K_5;
    const double y = (r1 * k5) * K_Q15 - K_5;
    rel.x = (float)((r0 * k5) * K_Q15 - K_5);
    rel.y = (float)y;
    rel.z = (float)z;
    const double k15 = D(fifteen) * K_2;
    memcpy(&w, ev + 7, 2);
    const double iz = ((double)w * k15) * K_Q15 - K_15000;
    memcpy(&w, ev + 5, 2);
    const double iy = ((double)w * k15) * K_Q15 - K_15000;
    memcpy(&w, ev + 3, 2);
    imp.x = (float)(((double)w * k15) * K_Q15 - K_15000);
    imp.y = (float)iy;
    imp.z = (float)iz;
    VFN(car, 0x3c, void, const P3*, const P3*, uint8_t)(car, 0, &imp, &rel, 0);
    return 14;
}
static void fp_replay_damage(Footprint& f, const uint8_t* ev, int32_t) {
    Car* car = PhysTaskFindCar((int32_t)(int8_t)ev[13]);
    if (car) fp_car_all(f, car);
}
PORT_FN(0x004374e0, "Car::replay_damage_handler", Car_replay_damage_handler, fp_replay_damage)

// ---- Car::replay_reset_handler (0x437640, replay event 7): the car's damage undone; 2 bytes --------------------
static int __cdecl Car_replay_reset_handler(const uint8_t* ev, int32_t) {
    call_Car_reset_damage(PhysTaskFindCar((int32_t)(int8_t)ev[0]), 0);
    return 2;
}
static void fp_replay_reset(Footprint& f, const uint8_t* ev, int32_t) {
    Car* car = PhysTaskFindCar((int32_t)(int8_t)ev[0]);
    if (car) fp_car_all(f, car);
}
PORT_FN(0x00437640, "Car::replay_reset_handler", Car_replay_reset_handler, fp_replay_reset)

// =============================================================================================================
// Car::UpdateReplay (0x438950, vtable +0x1c): the car between two 60-byte packets at t
// =============================================================================================================
// A packet (MakeReplayPacket): PhobDyno's 0x1e bytes; +0x1e perceived rpm (16 bits over -2000..12000); +0x20
// steering, +0x21 throttle, +0x22 braking (8 bits); +0x23 gear; +0x24 the wheels (5 bytes each); +0x38 flags
// (PhobRoot +4, 0x20 damaged, 0x100 horn, 0x200 reset); +0x3a opacity (0 = none recorded). A reset in the next
// packet holds the car at the first.
static inline uint16_t rd16(const uint8_t* p) { uint16_t w; memcpy(&w, p, 2); return w; }
static inline double hi8(uint8_t b) { return (double)(uint16_t)(b << 8); }
static void __fastcall Car_UpdateReplay(Car* self, Edx, const uint8_t* p0, const uint8_t* p1, uint32_t t_bits) {
    self->scrape_energy = 0.0f;
    self->starter_on = 0;
    const uint8_t hold = (rd16(p1 + 0x38) & 0x200) != 0;
    PhobDyno_UpdateReplay(self, 0, p0, hold ? p0 : p1, t_bits);
    float t;
    memcpy(&t, &t_bits, 4);
    const P3 vel = self->velocity;                               // (integer copies)
    const double v2 = (D(vel.y) * vel.y + D(vel.z) * vel.z) + D(vel.x) * vel.x;
    memcpy(&self->air_velocity, &vel, 12);
    self->speed = (float)x87_sqrt(v2);
    {
        const double a0 = (double)rd16(p0 + 0x1e);
        const double range = D(K_12000) - K_M2000;
        const double a1 = (double)rd16(p1 + 0x1e) * K_Q15;
        const double d = a1 - D(K_Q15) * a0;
        self->perceived_rpm = (float)(((d * range) * t + (a0 * range) * K_Q15) + K_M2000);
    }
    {
        const double s0 = hi8(p0[0x20]) * FB(0x3899999a);
        const double s1 = hi8(p1[0x20]) * FB(0x3899999a);
        self->steering = (float)((s0 + (s1 - s0) * t) - K_1_2);
    }
    const uint8_t o = p0[0x3a];
    if (!o) self->opacity = 1.0f;
    else if (hold) self->opacity = (float)(hi8(o) * K_Q15);
    else {
        const double o0 = hi8(o) * K_Q15;
        const double o1 = hi8(p1[0x3a]) * K_Q15;
        self->opacity = (float)(o0 + (o1 - o0) * t);
    }
    if (!self->is_plane) {
        const double s = D(self->max_steer_angle) * self->steering;
        self->wheels[2].steer_input = 0.0f;
        self->wheels[3].steer_input = 0.0f;
        self->wheels[0].steer_input = (float)s;
        self->wheels[1].steer_input = (float)s;
    } else {
        const double s = D(self->max_steer_angle) * self->steering;
        self->wheels[0].steer_input = 0.0f;
        self->wheels[1].steer_input = 0.0f;
        self->wheels[2].steer_input = (float)-s;
        self->wheels[3].steer_input = (float)-s;
    }
    {
        const double a = hi8(p0[0x21]) * K_Q15;
        const double b = hi8(p1[0x21]) * K_Q15;
        Car_SetThrottle(self, 0, (float)(a + (b - a) * t));
    }
    {
        const double a = hi8(p0[0x22]) * K_Q15;
        const double b = hi8(p1[0x22]) * K_Q15;
        Car_SetBraking(self, 0, (float)(a + (b - a) * t));
    }
    const uint16_t flags = (uint16_t)(rd16(p1 + 0x38) | rd16(p0 + 0x38));
    root_flags(self) = flags;
    self->horn = (rd16(p0 + 0x38) & 0x100) != 0;
    if (!(flags & 0x20) && self->damaged) call_Car_reset_damage(self, 0);
    for (int i = 0; i < 4; i++) Wheel_UpdateReplay(&self->wheels[i], 0, p0 + 0x24 + 5 * i, p1 + 0x24 + 5 * i, t_bits, self);
    const int32_t g = (int8_t)p0[0x23];
    self->engaged_gear = g;
    self->gear = g;
}
static void fp_car_update_replay(Footprint& f, Car* self, Edx, const uint8_t*, const uint8_t*, uint32_t) {
    fp_car_all(f, self);                                         // reset_damage: the models and the body volume
}
PORT_FN(0x00438950, "Car::UpdateReplay", Car_UpdateReplay, fp_car_update_replay)

// =============================================================================================================
// Car::MakeReplayPacket (0x438cd0, vtable +0x20): the 60-byte packet UpdateReplay reads; clears reset_event
// =============================================================================================================
// each value clamped (NaN to one end: the integer compares send negative NaNs low), scaled and truncated (__ftol)
static void __fastcall Car_MakeReplayPacket(Car* self, Edx, uint8_t* p) {
    PhobDyno_MakeReplayPacket(self, 0, p);
    const double r = VFN(self, 0x48, double)(self, 0);           // GetPerceivedRPM: ST0
    float v = (float)r;
    if (!(r >= D(K_M2000))) v = K_M2000;
    else v = (float)(D(K_12000) > v ? D(v) : D(K_12000));
    uint16_t w = (uint16_t)x87_ftol(((D(v) - K_M2000) / (D(K_12000) - K_M2000)) * K_32768);
    memcpy(p + 0x1e, &w, 2);
    if (bits(self->steering) > 0xbf99999au) v = FB(0xbf99999a);   // integer compare: below -1.2
    else v = (float)(D(K_1_2) > self->steering ? D(self->steering) : D(K_1_2));
    p[0x20] = (uint8_t)(x87_ftol((D(v) + K_1_2) * FB(0x46555555)) >> 8);
    if (bits(self->engine.smoothed_throttle) > 0x80000000u) v = 0.0f;
    else v = (float)(D(K_ONE) > self->engine.smoothed_throttle ? D(self->engine.smoothed_throttle) : D(K_ONE));
    p[0x21] = (uint8_t)(x87_ftol(D(v) * K_32768) >> 8);
    if (bits(self->braking) > 0x80000000u) v = 0.0f;
    else v = (float)(D(K_ONE) > self->braking ? D(self->braking) : D(K_ONE));
    p[0x22] = (uint8_t)(x87_ftol(D(v) * K_32768) >> 8);
    if (bits(self->opacity) > 0x80000000u) v = 0.0f;
    else v = (float)(D(K_ONE) > self->opacity ? D(self->opacity) : D(K_ONE));
    const uint8_t o = (uint8_t)(x87_ftol(D(v) * K_32768) >> 8);
    p[0x3a] = o;
    if (!o) p[0x3a] = 1;                                         // 0 means "none recorded"
    uint16_t flags = (uint16_t)root_flags(self);
    memcpy(p + 0x38, &flags, 2);
    if (self->damaged) {
        flags |= 0x20;
        memcpy(p + 0x38, &flags, 2);
    }
    if (self->horn) p[0x39] |= 1;
    if (self->reset_event) p[0x39] |= 2;
    const uint8_t g = (uint8_t)self->engaged_gear;
    self->reset_event = 0;
    p[0x23] = g;
    for (int i = 0; i < 4; i++) Wheel_MakeReplayPacket(&self->wheels[i], 0, p + 0x24 + 5 * i);
}
static void fp_car_make_packet(Footprint& f, Car* self, Edx, uint8_t* p) {
    f.object(self, "this");
    f.add(p, 0x3c, "packet");
}
PORT_FN(0x00438cd0, "Car::MakeReplayPacket", Car_MakeReplayPacket, fp_car_make_packet)

// =============================================================================================================
// Car::GetMessage (0x438ef0, vtable +0x14): the renderer's and the HUD's view of the car, at buf + PhobRoot +8
// =============================================================================================================
// +0 tick count; +4 the frame; +0x28 the model origin (frame applied to the body offset); +0x34 the perceived rpm
// (a plane: its altitude x 3.28, feet); +0x38 smoothed throttle; +0x3c steering; +0x40 braking; +0x44 clutch
// engagement; +0x48 gear; +0x4c speed (a plane: forward airspeed); +0x50 fuel; +0x54 Car +0xe24; +0x58 lat g;
// +0x5c long g; +0x60 |velocity|; +0x64 aero g; +0x69 flags (1 above 85% of the redline, 2 above 95%, 0x40 a
// wheel broken); +0x6c velocity; +0x78 the wheels (0x38 each); +0x158 opacity; +0x15c look side; +0x160 look
// back; +0x162 the damage grid; +0x184 coolant and +0x188 block temperature.
static inline void put_bits(uint8_t* m, uint32_t off, const void* src) { memcpy(m + off, src, 4); }
static void __fastcall Car_GetMessage(Car* self, Edx, uint8_t* buf) {
    uint8_t* m = buf + msg_offset(self);
    memcpy(m + 4, &self->frame, 48);
    // (the original also has an order for m == this, a compiler alias check that can't be taken: left out)
    const float* r = self->frame.rot.m;
    const float* o = body_offset(self);
    float* pos = (float*)(m + 0x28);
    pos[0] = (float)((D(r[3]) * o[1] + D(r[0]) * o[0]) + D(r[6]) * o[2]);
    pos[1] = (float)((D(r[4]) * o[1] + D(r[1]) * o[0]) + D(o[2]) * r[7]);
    pos[2] = (float)((D(r[5]) * o[1] + D(r[2]) * o[0]) + D(r[8]) * o[2]);
    pos[0] = (float)(D(self->frame.pos.x) + pos[0]);
    pos[1] = (float)(D(pos[1]) + self->frame.pos.y);
    pos[2] = (float)(D(self->frame.pos.z) + pos[2]);
    memcpy(m, &self->tick_count, 2);
    m = buf + msg_offset(self);
    float f;
    if (!self->is_plane) f = (float)VFN(self, 0x48, double)(self, 0);
    else f = (float)(D(self->frame.pos.y) * FB(0x4051d608));
    put_bits(m, 0x34, &f);
    if (self->is_plane) {
        const P3& v = self->velocity;
        f = (float)((D(r[8]) * v.z + D(v.y) * r[7]) + D(r[6]) * v.x);
        put_bits(m, 0x4c, &f);
    } else put_bits(m, 0x4c, &self->speed);
    put_bits(m, 0x3c, &self->steering);
    put_bits(m, 0x38, &self->engine.smoothed_throttle);
    put_bits(m, 0x40, &self->braking);
    put_bits(m, 0x44, &self->clutch_engagement);
    put_bits(m, 0x48, &self->engaged_gear);
    put_bits(m, 0x50, &self->fuel);
    put_bits(m, 0x54, &self->_e24);
    put_bits(m, 0x58, &self->lat_g);
    put_bits(m, 0x5c, &self->long_g);
    const P3& v = self->velocity;
    f = (float)x87_sqrt((D(v.y) * v.y + D(v.z) * v.z) + D(v.x) * v.x);
    put_bits(m, 0x60, &f);
    put_bits(m, 0x64, &self->aero_g);
    put_bits(m, 0x158, &self->opacity);
    put_bits(m, 0x15c, &self->look_side);
    m[0x160] = self->look_back;
    m[0x69] = 0;
    put_bits(m, 0x184, &self->engine.coolant_temperature);
    put_bits(m, 0x188, &self->engine.block_temperature);
    memcpy(m + 0x6c, &self->velocity, 12);
    const float red = self->engine.redline;
    if (!(D(red) * FB(0x3f59999a) >= self->perceived_rpm)) {     // 0.85: fcomp; test ah,1
        uint8_t fl = m[0x69] | 1;
        m[0x69] = fl;
        if (!(D(red) * FB(0x3f733333) >= self->perceived_rpm)) m[0x69] = fl | 2;   // 0.95
    }
    uint8_t broken = 0;
    for (int i = 0; i < 4; i++) {
        Wheel_GetMessage(&self->wheels[i], 0, m + 0x78 + 0x38 * i, self);
        if (self->wheels[i].broken) broken = 1;
    }
    if (broken) m[0x69] |= 0x40;
    memcpy(m + 0x162, self->damage_grid, 32);
}
static void fp_car_message(Footprint& f, Car* self, Edx, uint8_t* buf) {
    f.add(buf + msg_offset(self), 0x18c, "message");
}
PORT_FN(0x00438ef0, "Car::GetMessage", Car_GetMessage, fp_car_message)

// =============================================================================================================
// Car::post_teleport_fadein (0x439aa0): for 6.2 s after a teleport the car fades in (opacity (t^2/36 + 0.15) /
// 1.15, 1 from 6 s); while another car is within 5.08 m the fade is held back (teleport_time creeps forward)
// =============================================================================================================
static void __fastcall Car_post_teleport_fadein(Car* self, Edx) {
    const double dt = PhysicsGetTime() - D(self->teleport_time);
    const float dts = (float)dt;
    if (dt >= D(FB(0x40c66666))) return;                         // 6.2 (NaN goes on)
    if ((int32_t)bits(dts) < 0x40c00000)                         // integer compare: < 6.0
        self->opacity = (float)(((D(dts) * dts) * FB(0x3ce38e39) + FB(0x3e19999a)) * FB(0x3f5e9bd3));
    else self->opacity = 1.0f;
    const Proxer* px = *g_proxer;
    const uint32_t at = (uint32_t)self->car_index * (uint32_t)px->row_stride + (uint32_t)px->n * 4u +
                        (uint32_t)(uintptr_t)px->base + 0x10u;
    const float* e = *(const float* const*)(uintptr_t)at;
    if (!e) return;
    if (!(D(e[2]) * e[2] + D(e[1]) * e[1] >= D(FB(0x41ce7381))))  // 25.8064 = (5.08 m)^2
        self->teleport_time = (float)(D(self->teleport_time) + K_0_016);
}
static void fp_car_fadein(Footprint& f, Car* self, Edx) { f.object(self, "this"); }
PORT_FN(0x00439aa0, "Car::post_teleport_fadein", Car_post_teleport_fadein, fp_car_fadein)

// =============================================================================================================
// Car::Teleport (0x439b50, vtable +0x40): to pos, facing dir (x, z in the ground plane), upright and still;
// invisible (the fade-in starts), in neutral, the parts reset; replay event 7 (car, 0)
// =============================================================================================================
static void __fastcall Car_Teleport(Car* self, Edx, const P3* pos, const float* dir) {
    VFN(self->body_volume, 0x14, void)(self->body_volume, 0);
    float* R = self->frame.rot.m;
    R[0] = 1.0f; R[1] = 0.0f; R[2] = 0.0f;
    R[3] = 0.0f; R[4] = 1.0f; R[5] = 0.0f;
    R[6] = 0.0f; R[7] = 0.0f; R[8] = 1.0f;
    COPY4(R[6], dir[0]);
    R[7] = 0.0f;
    const float dz = fpu_copy(&dir[1]);
    R[8] = dz;
    R[0] = dz;
    R[2] = fpu_neg(&R[6]);
    // copies of rows 0 and 2 (integer moves), as the original keeps them on its stack
    float r1, r2, a[3], c8, c7, c[3];
    COPY4(r2, R[2]); COPY4(r1, R[1]);
    COPY4(a[0], R[0]); COPY4(a[1], R[1]); COPY4(a[2], R[2]);
    COPY4(c8, R[8]); COPY4(c7, R[7]);
    COPY4(c[0], R[6]); COPY4(c[1], R[7]); COPY4(c[2], R[8]);
    // up = row 2 x row 0 (inline)
    float u[3];
    u[0] = (float)(D(r2) * c7 - D(r1) * c8);
    u[1] = (float)(D(a[0]) * c8 - D(r2) * c[0]);
    u[2] = (float)(D(r1) * c[0] - D(a[0]) * c7);
    double k = K_ONE / x87_sqrt((D(r1) * r1 + D(r2) * r2) + D(a[0]) * a[0]);
    a[0] = (float)(D(a[0]) * k);
    a[1] = (float)(D(r1) * k);
    a[2] = (float)(k * r2);
    k = K_ONE / x87_sqrt((D(u[1]) * u[1] + D(u[2]) * u[2]) + D(u[0]) * u[0]);
    u[0] = (float)(D(u[0]) * k);
    u[1] = (float)(D(u[1]) * k);
    u[2] = (float)(k * u[2]);
    CrossProduct((P3*)c, (const P3*)a, (const P3*)u);
    memcpy(&R[0], a, 12);
    memcpy(&R[3], u, 12);
    memcpy(&R[6], c, 12);
    memcpy(&self->frame.pos, pos, 12);
    memset(&self->velocity, 0, 12);
    memset(&self->angular_velocity, 0, 12);
    self->perceived_rpm = 0.0f;
    self->opacity = 0.0f;
    self->teleport_time = (float)PhysicsGetTime();
    self->reset_event = 1;
    Car_SetGear(self, 0, 0);
    call_Car_reset_subordinates(self, 0);
    uint8_t ev[2] = {(uint8_t)self->car_index, 0};
    self->lowering = 1;
    PhysReplayAddEvent(7, ev, 2);
}
static void fp_car_teleport(Footprint& f, Car* self, Edx, const P3*, const float*) { fp_car_all(f, self); }
PORT_FN(0x00439b50, "Car::Teleport", Car_Teleport, fp_car_teleport)

// ---- Car::reset_subordinates (0x439df0): the corners re-seed, the wheels, the damage, the engine's rpm, the body --
static void __fastcall Car_reset_subordinates(Car* self, Edx) {
    for (int i = 0; i < self->num_corners; i++) self->corners[i].first_update = 1;
    for (int i = 0; i < 4; i++) Wheel_ResetPosition(&self->wheels[i], 0);
    call_Car_reset_damage(self, 0);
    BodyVolume* g = self->body_volume;
    self->engine.rpm = 0.0f;
    VFN(g, 0x14, void)(g, 0);
}
static void fp_car_reset_subordinates(Footprint& f, Car* self, Edx) { fp_car_all(f, self); }
PORT_FN(0x00439df0, "Car::reset_subordinates", Car_reset_subordinates, fp_car_reset_subordinates)

// ---- Car::reset_damage (0x439e50): the grid cleared, every LOD's pristine vertices copied back, the wheels mended --
static void __fastcall Car_reset_damage(Car* self, Edx) {
    self->_48d = 0;
    memset(self->damage_grid, 0, 32);
    self->damaged = 0;
    for (int lod = 0; lod < 5; lod++) {
        mrVertex *live, *pristine;
        int32_t n, np;
        mrModelGetVerts(self->live_models[lod], &live, &n);
        mrModelGetVerts(self->lod_models[lod], &pristine, &np);
        for (int32_t j = 0; j < n; j++) memcpy(&live[j].pos, &pristine[j].pos, 12);
        const int32_t m = self->live_models[lod];
        mrModelBuild(m, mrModelGetInfo(m), 4);
    }
    for (int i = 0; i < 4; i++) self->wheels[i].broken = 0;
    VFN(self->body_volume, 4, void)(self->body_volume, 0);
}
static void fp_car_reset_damage(Footprint& f, Car* self, Edx) { fp_car_all(f, self); }
PORT_FN(0x00439e50, "Car::reset_damage", Car_reset_damage, fp_car_reset_damage)

// ---- Car::Reset (0x439f90, vtable +4): PhobDyno's, then reset; replay event 7 (car, 0) -----------------------------
static void __fastcall Car_Reset(Car* self, Edx) {
    self->_48d = 0;
    PhobDyno_Reset(self, 0);
    call_Car_reset(self, 0);
    uint8_t ev[2] = {(uint8_t)self->car_index, 0};
    PhysReplayAddEvent(7, ev, 2);
}
static void fp_car_Reset(Footprint& f, Car* self, Edx) { fp_car_all(f, self); }
PORT_FN(0x00439f90, "Car::Reset", Car_Reset, fp_car_Reset)

// ---- Car::reset (0x439fd0): back to the start frame, lowering, visible, controls and state cleared, fuel filled ------
static void __fastcall Car_reset(Car* self, Edx) {
    self->reset_event = 1;
    memset(&self->draft_point, 0, 12);
    self->lowering = 1;
    self->last_damage_time = 0.0f;
    self->horn_volume = 0.0f;
    BodyVolume* g = self->body_volume;
    self->damaged = 0;
    self->draft_radius = 5.0f;
    self->opacity = 1.0f;
    self->teleport_time = -100.0f;
    VFN(g, 0x14, void)(g, 0);
    memcpy(&self->frame, &self->start_frame, 48);
    self->vert_g = 0.0f;
    self->long_g = 0.0f;
    self->lat_g = 0.0f;
    self->horn = 0;
    self->throttle = 0.0f;
    self->braking = 0.0f;
    self->ebrake = 0.0f;
    self->pitch = 0.0f;
    self->_504 = 0;
    self->steering = 0.0f;
    self->perceived_rpm = 0.0f;
    self->_e38 = 0;
    self->steer_feedback = 0.0f;
    self->gear = 0;
    self->clutch = 1.0f;
    self->engaged_gear = 0;
    Car_SetGear(self, 0, 0);
    self->speed = 0.0f;
    self->_e20 = 0;
    self->_e24 = 0;
    self->look_side = 0.0f;
    self->look_back = 0;
    self->_e48 = 0;
    self->starter_on = 0;
    self->draft_velocity.x = 0.0f;
    self->draft_velocity.y = 0.0f;
    self->fuel = (float)(D(self->fuel_capacity) * FB(0x40728f5c));            // 3.79 l per gallon
    self->fuel_burn_rate = (float)(D(self->fuel_consumption) * FB(0x3a89fd5c));
    self->draft_velocity.z = 0.0f;
    self->lap_top_speed = 0.0f;
    call_Car_reset_subordinates(self, 0);
}
static void fp_car_reset(Footprint& f, Car* self, Edx) { fp_car_all(f, self); }
PORT_FN(0x00439fd0, "Car::reset(private)", Car_reset, fp_car_reset)   // (the ini's keys ignore case: Car::Reset)

// =============================================================================================================
// Car::Setup (0x43a130): the car from its data
// =============================================================================================================
// The name and start frame; the cockpit eye from <name>.car/cockpit.tab; the axles and the body offset (the model
// origin from the centre of mass); 12 collision spheres fitted to the LOD-0 extents; each wheel's hub vertex (the
// nearest body vertex to the axle at +-0.4 of the body width; with damage off a static point); the wheels; the
// centre of pressure, anti-roll, steering lock; the drivetrain (engine, gearbox, clutch, front / rear / centre
// differentials by torque_balance); the aero constants; a negative rear track makes a plane (propeller, wings).
static void __fastcall Car_Setup(Car* self, Edx, const CarData* cd) {
    uint8_t pc[24];                                              // the PowerCurve Engine::Setup copies
    char buf[56];
    const size_t name_len = strlen(cd->name);
    // FIX: the car's name has 32 bytes, and the original copies the data's name whole: a name of 32 characters or
    // more (unterminated in CarData's own name[32], so it runs on into lod_models) overran the Car's fields after
    // it. It is cut to 31 characters -- which also bounds the sprintf into buf[56] below ("%s.car/cockpit.tab":
    // 31 + 18 + 1 = 50 bytes) and the constructor's horn name; Wheel::Setup then sees the 31-character name.
    if (VP_FIX && name_len > 31) {
        memcpy(self->name, cd->name, 31);
        self->name[31] = 0;
        logf("Car::Setup: the car name \"%.31s...\" is %u characters; cut to 31", cd->name, (unsigned)name_len);
    } else
        memcpy(self->name, cd->name, name_len + 1);              // (inline repne scasb / rep movs)
    memcpy(&self->start_frame, &cd->start_frame, 48);
    COPY4(self->fuel_capacity, cd->fuel_capacity);
    COPY4(self->fuel_consumption, cd->fuel_consumption);
    game_sprintf(buf, S_COCKPIT_FMT, self->name);
    if (ResourceExists(buf)) {
        void* st = StringTableGet(buf);
        self->cockpit_eye.x = (float)game_atof(StringTableGetEntry(st, 0, 1));
        self->cockpit_eye.y = (float)game_atof(StringTableGetEntry(st, 0, 2));
        self->cockpit_eye.z = (float)game_atof(StringTableGetEntry(st, 0, 3));
        StringTableForget(st);
    } else {
        self->cockpit_eye.x = FB(0xbe99999a);                    // -0.3
        self->cockpit_eye.y = FB(0x3f4ccccd);                    // 0.8
        self->cockpit_eye.z = 0.0f;
    }
    memcpy(&self->frame, &cd->start_frame, 48);
    const float half_width_in = (float)(D(cd->body_width) * K_INCH);   // (the body width in metres)
    const double wb = D(cd->wheelbase) * K_INCH;
    const double wf = D(cd->weight_distribution) * FB(0x3c23d70a);
    const double rf = D(K_ONE) - wf;
    self->front_axle.x = 0.0f;
    self->front_axle.y = (float)(D(cd->cm_height) * K_MINCH);
    self->front_axle.z = (float)(rf * wb);
    self->rear_axle.x = 0.0f;
    float off[3];
    off[0] = 0.0f;
    self->rear_axle.y = (float)(D(cd->cm_height) * K_MINCH);
    self->rear_axle.z = (float)-(wf * wb);
    off[1] = (float)(D(cd->cm_height) * K_MINCH);
    off[2] = (float)(((wf - rf) * wb) * FB(0xbf000000));
    float* bo = body_offset(self);
    memcpy(bo, off, 12);
    float minx, maxx, miny, maxy, minz, maxz;
    mrModelGetExtents(self->lod_models[0], &minx, &maxx, &miny, &maxy, &minz, &maxz);
    minx = !(D(FB(0xc0000000)) >= minx) ? fpu_copy(&minx) : FB(0xc0000000);   // max(-2, x)
    maxx = D(K_2) > maxx ? fpu_copy(&maxx) : K_2;                            // min(2, x)
    memcpy(off, bo, 12);
    minx = (float)(D(off[0]) + minx);
    maxx = (float)(D(off[0]) + maxx);
    miny = (float)(D(off[1]) + miny);
    maxy = (float)(D(off[1]) + maxy);
    minz = (float)(D(off[2]) + minz);
    maxz = (float)(D(off[2]) + maxz);
    (void)maxy;
    void* mem = MemAlloc(0x54);
    self->body_volume = mem ? (BodyVolume*)SphereGroupVolume_ctor(mem, 0, &self->frame, self) : 0;
    const float front = (float)(D(maxz) - K_0_4);
    const float low = (float)(D(miny) + K_0_4);
    const float side = (float)(D(maxx) - K_0_4);
    const double mid_r = (D(maxz) + minz) * K_0_5;
    const float mid = (float)mid_r;
    const float front_mid = (float)((mid_r + front) * K_0_5);
    const float high = (float)(D(miny) + K_0_6);
    const float cab_side = (float)(D(maxx) - K_0_6);
    const float back = (float)(D(minz) + K_0_5);
    const float mid_low = (float)(D(miny) + K_0_5);
    const float rear_side = (float)(D(maxx) - K_0_5);
    const float back_mid = (float)((D(back) + mid) * K_0_5);
    float neg_side = fpu_neg(&side);
    const P3 c_body = {FB(0x3f333333), FB(0x3dcccccd), 1.0f};    // (0.7, 0.1, 1): each sphere's compliance
    const P3 c_cab = c_body, c_rear = c_body;
    BodyVolume* g = self->body_volume;
    struct S { float x, y, z; } s;
    const uint32_t A = 0x47435000u, B = 0x3f000000u, C = 0x4f932bc8u, E = 0x2edea70cu;   // 50000, 0.5, 5e9, 1e-10
    s = {neg_side, low, front};  SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3ecccccdu, A, B, C, E, &c_body);
    g = self->body_volume;
    s = {0.0f, low, front};      SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3ecccccdu, A, B, C, E, &c_body);
    g = self->body_volume;
    s = {side, low, front};      SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3ecccccdu, A, B, C, E, &c_body);
    g = self->body_volume;
    s = {neg_side, low, front_mid}; SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3ecccccdu, A, B, C, E, &c_body);
    g = self->body_volume;
    s = {0.0f, low, front_mid};  SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3ecccccdu, A, B, C, E, &c_body);
    g = self->body_volume;
    s = {side, low, front_mid};  SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3ecccccdu, A, B, C, E, &c_body);
    s.x = fpu_neg(&cab_side); s.y = high; s.z = mid;
    g = self->body_volume;
    SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3f19999au, A, B, C, E, &c_cab);
    g = self->body_volume;
    s = {cab_side, high, mid};   SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3f19999au, A, B, C, E, &c_cab);
    neg_side = fpu_neg(&rear_side);
    g = self->body_volume;
    s = {neg_side, mid_low, back_mid}; SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3f000000u, A, B, C, E, &c_rear);
    g = self->body_volume;
    s = {rear_side, mid_low, back_mid}; SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3f000000u, A, B, C, E, &c_rear);
    g = self->body_volume;
    s = {neg_side, mid_low, back}; SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3f000000u, A, B, C, E, &c_rear);
    g = self->body_volume;
    s = {rear_side, mid_low, back}; SphereGroupVolume_AddSphere(g, 0, (P3*)&s, 0x3f000000u, A, B, C, E, &c_rear);
    self->volumes[self->num_volumes] = (CollisionVolume*)self->body_volume;
    self->num_volumes++;
    self->collide_ground = 1;
    VFN(self->body_volume, 0x1c, void, int32_t)(self->body_volume, 0, 0x6d);   // SetSurfaceType(109)

    // each wheel's hub vertex: the nearest LOD-0 body vertex to the axle point at +-0.4 of the body width
    mrVertex* verts;
    int32_t count;
    mrModelGetVerts(self->live_models[0], &verts, &count);
    const P3* hub[4];
    for (int i = 0; i < 4; i++) {
        if (!(*g_hub_guard & 1)) {
            *g_hub_guard |= 1;
            g_zero_hub[0] = 0.0f; g_zero_hub[1] = 0.0f; g_zero_hub[2] = 0.0f;
            game_atexit(ATEXIT_ZERO_HUB);
        }
        const P3* axle = (i == 0 || i == 1) ? &self->front_axle : &self->rear_axle;
        float a[3];
        memcpy(a, axle, 12);
        const float sgn = (i == 0 || i == 2) ? FB(0xbecccccd) : K_0_4;
        a[0] = (float)(D(half_width_in) * sgn);
        float best = FB(0x47c35000);                             // 100000
        const P3* near_ = (const P3*)g_zero_hub;
        for (int32_t j = 0; j < count; j++) {
            float e[3];
            e[0] = (float)(D(a[0]) - verts[j].pos.x);
            e[1] = (float)(D(a[1]) - verts[j].pos.y);
            e[2] = (float)(D(a[2]) - verts[j].pos.z);
            const double dist = x87_sqrt((D(e[1]) * e[1] + D(e[2]) * e[2]) + D(e[0]) * e[0]);
            const float dists = (float)dist;
            if (!(dist >= best)) {
                best = dists;
                near_ = &verts[j].pos;
            }
        }
        hub[i] = near_;
    }
    for (int i = 0; i < 4; i++) {
        if (!(*g_hub_guard & 2)) {
            *g_hub_guard |= 2;
            g_no_damage_hub[0] = 0.0f; g_no_damage_hub[1] = 0.0f; g_no_damage_hub[2] = 0.0f;
            game_atexit(ATEXIT_NO_DAMAGE_HUB);
        }
        if (!PhysicsIsDamageOn()) hub[i] = (const P3*)g_no_damage_hub;
        Wheel_Setup(&self->wheels[i], 0, cd, self, i, hub[i]);
    }
    memcpy(&self->center_of_pressure, &self->front_axle, 12);
    self->center_of_pressure.y = (float)(D(cd->cp_height) * K_INCH + self->center_of_pressure.y);
    self->center_of_pressure.z = (float)(D(cd->cp_back) * K_MINCH + self->center_of_pressure.z);
    self->front_anti_roll = (float)(D(cd->sway[0]) * FB(0x432f3264));
    self->rear_anti_roll = (float)(D(cd->sway[1]) * FB(0x432f3264));
    self->wheels[2].steer_input = 0.0f;
    self->wheels[3].steer_input = 0.0f;
    self->max_steer_angle = (float)(D(cd->wheel_lock) * FB(0x3c8e8a72));   // 0.0174
    COPY4(self->torque_balance, cd->torque_balance);
    int drive = 0;                                               // 0 front, 1 all (centre diff), 2 rear
    int32_t tb;
    memcpy(&tb, &cd->torque_balance, 4);
    if (tb >= 0x3c23d70a) drive = tb > 0x3f7d70a4 ? 2 : 1;       // integer compares: 0.01, 0.99
    // the gearbox's drag (0.002 if the file's is ~0) and the number of forward gears (ratios while > 0.01, up to 7)
    float gearbox_drag;
    {
        volatile float gd = cd->gearbox_drag;                    // fld; fabs; fcomp eps
        const double a = gd < 0.0f ? -D(gd) : D(gd);
        if (!(a > D(FB(0x34000000)))) gearbox_drag = FB(0x3b03126f);
        else gearbox_drag = (float)(D(cd->gearbox_drag) * K_FTLB);
    }
    int32_t gears = 0;
    while (!(D(FB(0x3c23d70a)) >= cd->gear_ratios[gears])) {     // fcom; test ah,1 (NaN counts)
        gears++;
        if (gears >= 7) break;
    }
    const float engine_drag = (float)(D(cd->engine_drag) * K_FTLB);
    PowerCurve_Setup(pc, 0, bits(cd->power), bits(cd->power_rpm), bits(cd->torque_max), bits(cd->torque_rpm),
                     bits(engine_drag));
    Engine_Setup(&self->engine, 0, self, (float)(D(cd->engine_inertia) * FB(0x3d2d2da7)), bits(engine_drag), pc,
                 bits(cd->redline), bits(cd->idle_rpm));
    Transmission_Setup(self->transmission, 0, self, 0x3e587910u, bits(gearbox_drag), gears, cd->gear_ratios,
                       &self->engine, &self->clutch_engagement);
    Clutch_Setup(&self->clutch_engagement, 0, self, (float)(D(cd->torque_max) * FB(0x40800000)));
    Differential_Setup(self->front_diff, 0, self, bits(cd->final_drive), (float)(D(cd->diff_stiffness[0]) * K_DIFF));
    Differential_Setup(self->rear_diff, 0, self, bits(cd->final_drive), (float)(D(cd->diff_stiffness[1]) * K_DIFF));
    Differential_Setup(self->center_diff, 0, self, 0x3f800000u, (float)(D(cd->diff_stiffness[2]) * K_DIFF));
    // road speed per engine rpm: the driven wheels' radius (weighted by the torque split) / final drive, rpm -> rad/s
    self->rpm_to_speed = (float)((((D(K_ONE) - self->torque_balance) * self->wheels[0].radius +
                                   D(self->wheels[2].radius) * self->torque_balance) / cd->final_drive) *
                                 FB(0x3dd67750));
    Differential_Connect(self->front_diff, 0, &self->wheels[0], &self->wheels[1]);
    Differential_Connect(self->rear_diff, 0, &self->wheels[2], &self->wheels[3]);
    Differential_Connect(self->center_diff, 0, self->front_diff, self->rear_diff);
    Transmission_Connect(self->transmission, 0,
                         drive == 0 ? (void*)self->front_diff : drive == 2 ? (void*)self->rear_diff : (void*)self->center_diff);
    Clutch_Connect(&self->clutch_engagement, 0, &self->engine, self->transmission);
    Car_SetGear(self, 0, 0);
    // aero: 0.5 rho (0.645 kg/m^3 ... x ft^2 -> m^2) x area x each coefficient
    const double area = D(cd->frontal_area) * FB(0x3dbe83e5);
    self->drag_long = (float)((D(cd->drag_coefficient[0]) * area) * K_AIR);
    self->drag_vert = (float)((D(cd->drag_coefficient[2]) * area) * FB(0x3ea51eb8));
    self->drag_lat = (float)((D(cd->drag_coefficient[1]) * area) * K_AIR);
    self->front_spoiler_drag = (float)((D(cd->spoiler_drag[0]) * area) * K_AIR);
    self->rear_spoiler_drag = (float)((D(cd->spoiler_drag[1]) * area) * K_AIR);
    self->front_lift = (float)((D(cd->lift[0]) * area) * K_AIR);
    self->rear_lift = (float)((area * cd->lift[1]) * K_AIR);
    // a plane (negative rear track: an integer compare, so -0 isn't one): a propeller on the clutch, four wings
    if (bits(cd->track[1]) > 0x80000000u) {
        self->is_plane = 1;
        void* m = MemAlloc(0x24);
        self->propeller = m ? Propeller_ctor(m, 0, 0, 0, 0x3f19999au) : 0;     // at (0, 0, 0.6)
        Shaft_Setup(self->propeller, 0, self, 0x3f587910u, 0x3b03126fu);
        P3 at;
        m = MemAlloc(0x3c);
        if (m) { at = {FB(0xc0200000), K_0_5, K_ONE};               // (-2.5, 0.5, 1)
                 self->wing_left = Wing_ctor(m, 0, &at, 0x40c00000u, 0x40200000u, 0x3db851ecu, 0, self); }
        else self->wing_left = 0;
        m = MemAlloc(0x3c);
        if (m) { at = {FB(0x40200000), K_0_5, K_ONE};               // (2.5, 0.5, 1)
                 self->wing_right = Wing_ctor(m, 0, &at, 0x40c00000u, 0x40200000u, 0x3db851ecu, 0, self); }
        else self->wing_right = 0;
        m = MemAlloc(0x3c);
        if (m) { at = {0.0f, K_0_5, FB(0xc0a00000)};                // (0, 0.5, -5)
                 self->elevator = Wing_ctor(m, 0, &at, 0x40a00000u, 0x3fc00000u, 0x3db851ecu, 0, self); }
        else self->elevator = 0;
        m = MemAlloc(0x3c);
        if (m) { at = {0.0f, K_ONE, FB(0xc0a00000)};                // (0, 1, -5), about the vertical axis
                 self->rudder = Wing_ctor(m, 0, &at, 0x40866666u, 0x3f4ccccdu, 0x3db851ecu, 1, self); }
        else self->rudder = 0;
        Clutch_Connect(&self->clutch_engagement, 0, &self->engine, self->propeller);
    }
}
static void fp_car_setup(Footprint& f, Car*, Edx, const CarData*) {
    f.replay_only = "allocates the body volume and its spheres, the tyres, a plane's parts; reads cockpit.tab";
}
PORT_FN(0x0043a130, "Car::Setup", Car_Setup, fp_car_setup)

// ---- CarBegin / CarEnd (0x43b090 / 0x43b0b0): the replay handlers, by the original's addresses (hooked above) ----
static void __cdecl Car_Begin() {
    PhysReplayInstallEventHandler(5, HANDLER_DAMAGE);
    PhysReplayInstallEventHandler(7, HANDLER_RESET);
}
static void fp_car_begin(Footprint& f) { f.add((void*)0x00521930, 32, "replay event handlers"); }
PORT_FN(0x0043b090, "CarBegin", Car_Begin, fp_car_begin)

static void __cdecl Car_End() { PhysReplayUninstallEventHandler(HANDLER_DAMAGE); }
static void fp_car_end(Footprint& f) { f.add((void*)0x00521930, 32, "replay event handlers"); }
PORT_FN(0x0043b0b0, "CarEnd", Car_End, fp_car_end)

// ---- Car::GetReplayPacketSize (0x43b0c0, vtable +0x18) --------------------------------------------------------------
static int32_t __fastcall Car_GetReplayPacketSize(Car*, Edx) { return 0x3c; }
static void fp_car_packet_size(Footprint& f, Car*, Edx) { f.pure = true; }
PORT_FN(0x0043b0c0, "Car::GetReplayPacketSize", Car_GetReplayPacketSize, fp_car_packet_size)
