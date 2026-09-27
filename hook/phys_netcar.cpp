// phys_netcar.cpp -- M3 3.6 group T5: the network car (physics:netcar.obj), rewritten.
//
// A NetCar (3896 bytes, vtable 0x4dc2e8) is a Car driven by another machine: it isn't simulated from controls
// but steered after the CarPhysicsPackets its owner sends (LocalCar::FillNetPacket, 15 a second). It keeps the
// last three, newest first. From them it fits a parabola through the three positions (setup_interpolation: the
// velocity at each packet's send time, and the acceleration from the last three velocities, capped at 1 g); every
// tick it extrapolates where the car is now and pulls the simulated car towards it with a per-axis stiffness
// (update_pos), or teleports it there when it's more than 9.15 m off. The orientation is two slerps: from the
// quaternion extrapolated off packets 2->1 to the one off 1->0, by a blend that runs 0..1 between packets, then
// from the car's own orientation towards that by the orientation stiffness (update_o). A hard knock (an impulse
// over 80) drops the stiffness to 5% and lets it recover over 7 packet lags (update_stiffness), and damps the spin.
// With no packet for 2 s the car is abandoned: launched upwards at 12.75 m/s and frozen once it falls, fading out.
// A packet over 0.5 s old fades the car; the horn packet throws the horn ball.
//
// Outside a network game (MultiEnabled false) a NetCar is a loopback test: it sends car 0's own packets through
// a DelayQueue and follows them.
//
// Written from the v1.0 disassembly: register values as double, stored values as float, the same grouping, float
// copies the original makes with integer moves as bit copies, and every call in the original's order -- by address
// (this file's own functions too, so the hooked rewrite or the original is what runs), or through the vtable.
//
// Left out: the 37 $E functions (static initialisers; $E60 0x43f930, the packet static's destructor, a bare `ret`)
// and the deleting destructor (0x4408f0).
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <intrin.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
#define FB(b) __builtin_bit_cast(float, (uint32_t)(b))   // a float constant from its bits
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

namespace {

static inline uint32_t bits(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static inline void set_bits(float& x, uint32_t u) { memcpy(&x, &u, 4); }
template <typename T> static inline T* P(uint32_t addr) { return (T*)(uintptr_t)addr; }

// ---- layouts ----------------------------------------------------------------------------------------------------
// CarPhysicsPacket, 0x3e bytes, packed (the NetCar keeps three in a row, 0x3e apart): what LocalCar::FillNetPacket
// writes (all but recv_time) and munge_packet squeezes through the CarNetPacket wire format
#pragma pack(push, 1)
struct CarPhysicsPacket {
    float recv_time;                   // +0x00  PhysicsGetTime when it arrived (NewPacket; the sender leaves it)
    float time;                        // +0x04  the sender's PhysicsGetTime when it was made
    float braking;                     // +0x08
    float steering;                    // +0x0c
    float throttle;                    // +0x10  smoothed engine throttle (not sent over the wire: munge keeps it)
    int32_t gear;                      // +0x14
    float rpm;                         // +0x18  perceived rpm
    P3 pos;                            // +0x1c  the QFrame: position,
    Q4 rot;                            // +0x28  and orientation (x y z w)
    uint8_t horn;                      // +0x38
    uint8_t teleported;                // +0x39  the sender teleported since the last packet
    uint8_t wheel_broken[4];           // +0x3a  Wheel +0x20 per wheel
};
#pragma pack(pop)
static_assert(offsetof(CarPhysicsPacket, pos) == 0x1c && offsetof(CarPhysicsPacket, horn) == 0x38 &&
              sizeof(CarPhysicsPacket) == 0x3e, "CarPhysicsPacket");

// NetCar::Stiffness, 16 bytes (types.tsv's 4 is its first float): k[0] the orientation's slerp fraction per tick,
// k[1..3] the position / velocity pull per axis (x y z)
struct Stiffness { float k[4]; };

// Wheel (0x1a4): what this file touches
struct WheelPart {
    uint8_t _000[0x20];
    uint8_t broken, _021[3];           // +0x020
    uint8_t _024[0x19c - 0x24];
    int32_t surface;                   // +0x19c  10: bumpy
    uint8_t is_remote, _1a1[3];        // +0x1a0
};
static_assert(offsetof(WheelPart, surface) == 0x19c && sizeof(WheelPart) == 0x1a4, "WheelPart");

// NetCar: the Car fields this file uses (phys_car.cpp's names), then its own
struct NetCar {
    void** vtable;                     // +0x000
    uint8_t _004[0x38 - 0x04];
    M3 rot;                            // +0x038  frame
    P3 pos;                            // +0x05c
    uint8_t _068[0x234 - 0x68];
    P3 velocity;                       // +0x234
    P3 angular_velocity;               // +0x240
    uint8_t _24c[0x478 - 0x24c];
    uint8_t* body_volume;              // +0x478
    uint8_t _47c[0x488 - 0x47c];
    float last_damage_time;            // +0x488
    uint8_t damaged, _48d[3];          // +0x48c
    uint8_t _490[0x4c4 - 0x490];
    int32_t live_models[5];            // +0x4c4
    uint8_t _4d8[0x50c - 0x4d8];
    float opacity;                     // +0x50c
    int32_t car_index;                 // +0x510
    uint8_t _514[0x538 - 0x514];
    uint32_t aero[7];                  // +0x538  drag_long .. rear_spoiler_drag: a NetCar has none
    WheelPart wheels[4];               // +0x554
    uint8_t is_plane, _be5[3];         // +0xbe4
    void* propeller;                   // +0xbe8
    void* wings[4];                    // +0xbec  left, right, elevator, rudder
    uint8_t _bfc[0xd70 - 0xbfc];
    float perceived_rpm;               // +0xd70
    uint8_t _d74[0xe3d - 0xd74];
    uint8_t lowering;                  // +0xe3d  Car: being lowered after a teleport (DoneLowering clears it)
    uint8_t _e3e[0xec0 - 0xe3e];
    // ---- NetCar
    CarPhysicsPacket* packet_buffer;   // +0xec0  MemAlloc(3 packets); the destructor frees it
    CarPhysicsPacket* packets;         // +0xec4  the same: [0] newest, [2] oldest
    int32_t num_packets;               // +0xec8  0..3; interpolation starts with the 4th packet
    P3 vel[3];                         // +0xecc  NetCar::Delta x3: the velocity at packets 0, 1, 2
    P3 accel;                          // +0xef0  NetCar::Delta: the acceleration, at most 9.81
    Stiffness stiffness;               // +0xefc  now
    Stiffness stiffness_rest;          // +0xf0c  the resting values (0.4; 0.4 0.1 0.4)
    float hit_time;                    // +0xf1c  the last hard knock (-1000 at first)
    uint8_t settling, _f21[3];         // +0xf20  set by a teleport, cleared by DoneLowering: no steering till 0.5 s after
    float blend;                       // +0xf24  the 2->1 / 1->0 extrapolations' slerp fraction
    float blend_step;                  // +0xf28  its step per tick: reaches 1 at the next packet
    float settle_time;                 // +0xf2c  when it touched down (or teleported)
    float loop_send_time;              // +0xf30  the loopback's last send
    uint8_t abandoned, _f35[3];        // +0xf34
};
static_assert(offsetof(NetCar, velocity) == 0x234 && offsetof(NetCar, opacity) == 0x50c &&
              offsetof(NetCar, wheels) == 0x554 && offsetof(NetCar, perceived_rpm) == 0xd70 &&
              offsetof(NetCar, packet_buffer) == 0xec0 && offsetof(NetCar, vel) == 0xecc &&
              offsetof(NetCar, accel) == 0xef0 && offsetof(NetCar, stiffness) == 0xefc &&
              offsetof(NetCar, hit_time) == 0xf1c && offsetof(NetCar, blend) == 0xf24 &&
              offsetof(NetCar, abandoned) == 0xf34 && sizeof(NetCar) == 3896, "NetCar");

enum : uint32_t {
    NETCAR_VTABLE = 0x004dc2e8,
    S_DELAY_QUEUE = 0x004eda38,        // DelayQueue*  the loopback's queue (netcar.obj's static)
    S_MUNGED = 0x00522090,             // CarPhysicsPacket  munge_packet's static result
    S_MUNGED_GUARD = 0x005220d4,       // u8  bit 0: its atexit registered
    S_MULTI = 0x004fb454,              // MultiEnabled() reads it
    S_DEITY = 0x005218ac,              // class Deity* deity
    S_SPLASH_SOUND = 0x00521f7c,       // the CollideWater splash Sound3D (Car::Update's reach)
    VT_RACE_DEITY = 0x004dc588,
    S_ERRNO = 0x00502730,              // the CRT's errno
    S_CRT_TRAN = 0x005d5600,           // ctrandisp2's double and flag (CIacos, from MakeNetPacket's rot_convert)
};

}  // namespace

// ---- the game's functions these call (by v1.0 address) -----------------------------------------------------
typedef void*(__fastcall* CarCtor_t)(void*, Edx, void*, void*);
typedef void(__fastcall* SelfVoid_t)(void*, Edx);
typedef void(__fastcall* SelfU_t)(void*, Edx, uint32_t);             // a float argument moved as bits
typedef void(__fastcall* SelfI_t)(void*, Edx, int32_t);
typedef void(__fastcall* SelfB_t)(void*, Edx, unsigned char);
typedef void(__fastcall* SelfPtr_t)(void*, Edx, void*);
typedef void(__fastcall* SelfForce_t)(void*, Edx, const P3*, const P3*, int32_t);
typedef void(__fastcall* SelfDamage_t)(void*, Edx, const P3*, const P3*, uint32_t, uint32_t);
typedef double(__cdecl* Time_t)(void);                                // ST0 as the callee leaves it
typedef unsigned char(__cdecl* Flag_t)(void);
typedef int32_t(__cdecl* Int_t)(void);
typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* Delete_t)(void*);
typedef void*(__fastcall* DQCtor_t)(void*, Edx, int, int);
typedef unsigned char(__fastcall* DQEnqueue_t)(void*, Edx, const void*, int, uint32_t);
typedef unsigned char(__fastcall* DQDequeue_t)(void*, Edx, void*, int32_t*, uint32_t*);
typedef void(__cdecl* Register_t)(void*, int32_t);
typedef void(__cdecl* Unregister_t)(void*);
typedef void*(__cdecl* GetBall_t)(int32_t);
typedef void(__fastcall* BallThrow_t)(void*, Edx, const void*, const P3*, uint32_t);
typedef void*(__cdecl* FindCar_t)(int32_t);
typedef int(__cdecl* Atexit_t)(void*);
typedef void*(__cdecl* Memmove_t)(void*, const void*, uint32_t);
typedef void(__cdecl* PacketConvert_t)(void*, const void*);
typedef double(__cdecl* VectorLength_t)(const P3*);
typedef void(__cdecl* Slerp_t)(Q4*, const Q4*, const Q4*, uint32_t);
typedef uint8_t(__cdecl* OutOfBounds_t)(const P3*);

static const CarCtor_t Car_Car = (CarCtor_t)0x004364c0;
static const SelfVoid_t Car_dtor = (SelfVoid_t)0x00436920;
static const SelfVoid_t Car_Update = (SelfVoid_t)0x00437680;
static const SelfU_t Car_SetSteering = (SelfU_t)0x00439620;
static const SelfU_t Car_SetThrottle = (SelfU_t)0x004396d0;
static const SelfU_t Car_SetBraking = (SelfU_t)0x004396e0;
static const SelfU_t Car_SetClutch = (SelfU_t)0x004396f0;
static const SelfU_t Car_SetEBrake = (SelfU_t)0x00439700;
static const SelfI_t Car_SetGear = (SelfI_t)0x00439710;
static const SelfB_t Car_SetHorn = (SelfB_t)0x0043afb0;
static const SelfForce_t Car_ApplyExternalForce = (SelfForce_t)0x00436a90;
static const SelfForce_t Car_ResolveExternalImpulse = (SelfForce_t)0x00437120;
static const SelfPtr_t Car_MakeReplayPacket = (SelfPtr_t)0x00438cd0;
static const SelfDamage_t Car_ActuallyApplyDamage = (SelfDamage_t)0x00436b40;
static const SelfPtr_t LocalCar_FillNetPacket = (SelfPtr_t)0x00444580;
static const Time_t PhysicsGetTime = (Time_t)0x0042bc80;
static const Flag_t MultiEnabled = (Flag_t)0x004a23c0;
static const Flag_t PhysReplayPlayMode = (Flag_t)0x0042d340;
static const Flag_t PhysicsIsDamageOn = (Flag_t)0x0042bd60;
static const Int_t GetGameState = (Int_t)0x0040a3d0;
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
static const Delete_t operator_delete = (Delete_t)0x00414390;
static const DQCtor_t DelayQueue_ctor = (DQCtor_t)0x004a4000;
static const SelfVoid_t DelayQueue_dtor = (SelfVoid_t)0x004a4060;
static const DQEnqueue_t DelayQueue_Enqueue = (DQEnqueue_t)0x004a4090;
static const DQDequeue_t DelayQueue_Dequeue = (DQDequeue_t)0x004a4130;
static const Register_t AIRegisterNetCar = (Register_t)0x0041d2a0;
static const Register_t MultiRegisterNetCar = (Register_t)0x004a27a0;
static const Unregister_t MultiUnregisterNetCar = (Unregister_t)0x004a2800;
static const Register_t AIUnregisterCar = (Register_t)0x0041d2e0;
static const GetBall_t GetBall = (GetBall_t)0x004410e0;
static const BallThrow_t Ball_Throw = (BallThrow_t)0x004412a0;
static const FindCar_t PhysTaskFindCar = (FindCar_t)0x00426d40;
static const Atexit_t game_atexit = (Atexit_t)0x004ceff0;
static const Memmove_t crt_memmove = (Memmove_t)0x004cf400;          // the CRT's, in race.exe
static const PacketConvert_t MakeNetPacket = (PacketConvert_t)0x004a4780;       // (CarNetPacket&, const CarPhysicsPacket&)
static const PacketConvert_t MakePhysicsPacket = (PacketConvert_t)0x004a44d0;   // (CarPhysicsPacket&, const CarNetPacket&)
static const VectorLength_t VectorLength = (VectorLength_t)0x00429100;
static const Slerp_t Slerp = (Slerp_t)0x004d8eb0;
static const OutOfBounds_t loc_is_out_of_bounds = (OutOfBounds_t)0x00438660;

// this file's own functions, called by address so the hooked rewrite (or the original) is what runs
typedef P3*(__cdecl* InterpV_t)(P3*, const P3*, const P3*, const P3*);
typedef unsigned char(__cdecl* TooOld_t)(uint32_t);
typedef void*(__fastcall* ThisRet_t)(void*, Edx);
typedef unsigned char(__fastcall* NetFlag_t)(NetCar*, Edx);
typedef void(__fastcall* NetVoid_t)(NetCar*, Edx);
typedef void(__fastcall* NetPacket_t)(NetCar*, Edx, const CarPhysicsPacket*);
typedef CarPhysicsPacket*(__cdecl* Munge_t)(CarPhysicsPacket*);
typedef double(__cdecl* QuatExtrap_t)(Q4*, const CarPhysicsPacket*, const CarPhysicsPacket*, uint32_t);
typedef double(__cdecl* Bumpy_t)(WheelPart*);
typedef void(__fastcall* StiffCopy_t)(Stiffness*, Edx, const Stiffness*, uint32_t);
static const InterpV_t interpolate_vA = (InterpV_t)0x0043f260;
static const TooOld_t packet_is_too_oldA = (TooOld_t)0x0043f340;
static const ThisRet_t Delta_ctorA = (ThisRet_t)0x0043f360;
static const NetFlag_t we_are_abandonedA = (NetFlag_t)0x0043f5f0;
static const NetVoid_t do_hornA = (NetVoid_t)0x0043f640;
static const NetVoid_t fade_alphaA = (NetVoid_t)0x0043f690;
static const NetFlag_t be_abandonedA = (NetFlag_t)0x0043f6c0;
static const Munge_t munge_packetA = (Munge_t)0x0043f8e0;
static const NetPacket_t NewPacketA = (NetPacket_t)0x0043f9c0;
static const NetVoid_t setup_interpolationA = (NetVoid_t)0x0043fa50;
static const NetVoid_t update_posA = (NetVoid_t)0x0043ff40;
static const NetVoid_t teleport_to_p0A = (NetVoid_t)0x00440260;
static const NetVoid_t update_oA = (NetVoid_t)0x004402e0;
static const QuatExtrap_t get_quat_extrapolationA = (QuatExtrap_t)0x004405b0;
static const Bumpy_t wheels_on_bumpyA = (Bumpy_t)0x00440610;
static const NetVoid_t goA = (NetVoid_t)0x00440660;
static const NetVoid_t update_stiffnessA = (NetVoid_t)0x004406c0;
static const StiffCopy_t Stiffness_copyA = (StiffCopy_t)0x00440840;
static const ThisRet_t CarPhysicsPacket_ctorA = (ThisRet_t)0x004408e0;

// ---- footprint helpers ------------------------------------------------------------------------------------------
static void fp_netcar(Footprint& f, NetCar* self) {
    f.object(self, "netcar");
    if (self->packets) f.add(self->packets, 3 * sizeof(CarPhysicsPacket), "packets");   // Slerp negates their quats
}
static void fp_ball(Footprint& f, NetCar* self) {                    // do_horn / Car::SetHorn: the horn ball
    if ((uint32_t)self->car_index < 16u)
        if (void* ball = GetBall(self->car_index)) f.object(ball, "ball");
}
// Car::Teleport's reach (phys_car.cpp fp_car_all): the car, its live models, the body volume and its spheres
static void fp_car_all(Footprint& f, NetCar* self) {
    f.object(self, "netcar");
    for (int i = 0; i < 5; i++) {
        uint8_t* mi = (uint8_t*)(uintptr_t)self->live_models[i];
        if (!mi) continue;
        f.add(mi + 0x10, 8, "model_info");
        const int32_t* info = *(int32_t**)(mi + 0x10);                 // mrModelInfo: num_verts, verts
        if (info && info[1] && info[0] > 0) f.add((void*)(uintptr_t)info[1], (uint32_t)info[0] * 32, "model verts");
    }
    if (uint8_t* g = self->body_volume) {
        f.object(g, "body volume");
        int32_t n = *(int32_t*)(g + 0x4c);
        for (int i = 0; i < n && i < 12; i++)
            if (void* s = ((void**)(g + 0x1c))[i]) f.object(s, "body sphere");
    }
}
// Car::Update's reach (phys_car_update.cpp fp_car_update)
static void fp_car_update(Footprint& f, NetCar* self) {
    f.object(self, "netcar");
    if (uint8_t* snd = *P<uint8_t*>(S_SPLASH_SOUND)) f.add(snd + 8, 0x25, "splash Sound3D");
    if (self->is_plane)
        for (int i = 0; i < 4; i++) f.add(self->wings[i], 60, "wing");
    uint8_t* deity = *P<uint8_t*>(S_DEITY);
    if (!deity || *(uint32_t*)deity != VT_RACE_DEITY) { f.replay_only = "the deity isn't a RaceDeity (size unknown)"; return; }
    f.add(deity, 2012, "deity");
    if ((uint32_t)self->car_index < 16u)
        if (uint8_t* line = *(uint8_t**)(deity + 0x38 + 0x74 * self->car_index + 0x6c)) f.add(line, 112, "centre line");
    if (loc_is_out_of_bounds(&self->pos)) { f.replay_only = "out of bounds: teleports through the deity"; return; }
    if (!PhysicsIsDamageOn() && self->damaged && !((D(self->last_damage_time) + 2.0f) >= PhysicsGetTime()))
        f.replay_only = "reset_damage rebuilds the models";
}

// =============================================================================================================
// the free functions
// =============================================================================================================

// interpolate_v(a, b, t) (0x43f260): (1 - t) a + t b per axis, with t per axis; built in a temporary, then copied
// out (so ret may be a or b), and ret returned in eax
static P3* __cdecl interpolate_v_vec(P3* ret, const P3* a, const P3* b, const P3* t) {
    float x = (float)((D(1.0f) - D(t->x)) * D(a->x) + D(t->x) * D(b->x));
    float y = (float)((D(1.0f) - D(t->y)) * D(a->y) + D(t->y) * D(b->y));
    float z = (float)((D(1.0f) - D(t->z)) * D(a->z) + D(t->z) * D(b->z));
    memcpy(&ret->x, &x, 4);
    memcpy(&ret->y, &y, 4);
    memcpy(&ret->z, &z, 4);
    return ret;
}
static void fp_interpolate_v_vec(Footprint& f, P3* ret, const P3*, const P3*, const P3*) {
    f.add(ret, sizeof(P3), "out");                  // (not pure-fuzzed: it returns its output pointer; world_netcar.cpp)
}
PORT_FN(0x0043f260, "interpolate_v(vec)", interpolate_v_vec, fp_interpolate_v_vec)

// interpolate_v(a, b, t) (0x43f2d0): a (1 - t) + b t, one t
static P3* __cdecl interpolate_v_float(P3* ret, const P3* a, const P3* b, float t) {
    double k = D(1.0f) - D(t);
    float x = (float)(D(a->x) * k + D(b->x) * D(t));
    float y = (float)(D(b->y) * D(t) + D(a->y) * k);
    float z = (float)(D(b->z) * D(t) + D(a->z) * k);
    memcpy(&ret->x, &x, 4);
    memcpy(&ret->y, &y, 4);
    memcpy(&ret->z, &z, 4);
    return ret;
}
static void fp_interpolate_v_float(Footprint& f, P3* ret, const P3*, const P3*, float) { f.add(ret, sizeof(P3), "out"); }
PORT_FN(0x0043f2d0, "interpolate_v(float)", interpolate_v_float, fp_interpolate_v_float)

// packet_is_too_old(recv_time) (0x43f340): more than half a second ago (NaN: not)
static unsigned char __cdecl packet_is_too_old(float t) {
    return PhysicsGetTime() - D(t) > D(0.5f) ? 1 : 0;              // fcomp 0.5; test ah,0x41; sete
}
static void fp_packet_is_too_old(Footprint&, float) {}
PORT_FN(0x0043f340, "packet_is_too_old", packet_is_too_old, fp_packet_is_too_old)

// munge_packet(p) (0x43f8e0): the packet as the peer sees it -- squeezed into a CarNetPacket and expanded again
// into a function-local static (whose +0 and +0x10 MakePhysicsPacket never writes). The local CarNetPacket is
// left uninitialised as in the original: MakeNetPacket keeps bit 7 of +0x16 and +0x17 as it finds them, and
// MakePhysicsPacket never reads them.
static CarPhysicsPacket* __cdecl munge_packet(CarPhysicsPacket* p) {
    uint8_t g = *P<uint8_t>(S_MUNGED_GUARD);
    if (!(g & 1)) {
        *P<uint8_t>(S_MUNGED_GUARD) = (uint8_t)(g | 1);
        game_atexit((void*)0x0043f930);                             // $E60, the static's destructor (a bare ret)
    }
    uint8_t net[0x18];
    MakeNetPacket(net, p);
    MakePhysicsPacket(P<void>(S_MUNGED), net);
    return P<CarPhysicsPacket>(S_MUNGED);
}
static void fp_munge_packet(Footprint& f, CarPhysicsPacket*) {
    f.add(P<void>(S_MUNGED), sizeof(CarPhysicsPacket), "munged packet");
    f.add(P<void>(S_MUNGED_GUARD), 1, "its guard");
    f.add(P<void>(S_CRT_TRAN), 9, "the CRT's transcendental scratch (acos, in rot_convert)");
    f.add(P<void>(S_ERRNO), 4, "errno (acos out of its domain)");
    if (!(*P<uint8_t>(S_MUNGED_GUARD) & 1)) f.replay_only = "registers the static's destructor with atexit (once)";
}
PORT_FN(0x0043f8e0, "munge_packet", munge_packet, fp_munge_packet)

// get_quat_extrapolation(out, a, b, t) (0x4405b0): slerp a's orientation towards b's by where t falls on
// a.time..b.time (past 1: extrapolated); returns the fraction. Slerp negates a's quaternion in place when the
// two are more than 90 degrees apart.
static float __cdecl get_quat_extrapolation(Q4* out, CarPhysicsPacket* a, CarPhysicsPacket* b, float t) {
    float s = (float)((D(t) - D(a->time)) / (D(b->time) - D(a->time)));
    Slerp(out, &a->rot, &b->rot, bits(s));
    return s;
}
static void fp_get_quat_extrapolation(Footprint& f, Q4* out, CarPhysicsPacket* a, CarPhysicsPacket*, float) {
    f.add(out, sizeof(Q4), "out");
    f.add(&a->rot, sizeof(Q4), "a's quaternion");
    f.pure = true;
}
PORT_FN(0x004405b0, "get_quat_extrapolation", get_quat_extrapolation, fp_get_quat_extrapolation)

// wheels_on_bumpy(wheels) (0x440610): 0.2 per wheel on a bumpy (10) surface, summed on the x87; ST0 returned
static double __cdecl wheels_on_bumpy(WheelPart* w) {
    double sum = 0.0;                                               // mov [esp],0; fld [esp]
    for (int i = 0; i < 4; i++)
        if (w[i].surface == 10) sum = sum + D(0.2f);
    return sum;
}
static void fp_wheels_on_bumpy(Footprint& f, WheelPart*) { f.pure = true; }
PORT_FN(0x00440610, "wheels_on_bumpy", wheels_on_bumpy, fp_wheels_on_bumpy)

// =============================================================================================================
// small constructors
// =============================================================================================================

// NetCar::Delta::Delta (0x43f360): zero
static P3* __fastcall Delta_ctor(P3* self, Edx) {
    memset(self, 0, sizeof(P3));
    return self;
}
static void fp_delta_ctor(Footprint& f, P3* self, Edx) { f.add(self, sizeof(P3), "this"); }   // (not fuzzed: returns this)
PORT_FN(0x0043f360, "NetCar::Delta::Delta", Delta_ctor, fp_delta_ctor)

// CarPhysicsPacket::CarPhysicsPacket (0x4408e0): nothing to set
static CarPhysicsPacket* __fastcall CarPhysicsPacket_ctor(CarPhysicsPacket* self, Edx) { return self; }
static void fp_packet_ctor(Footprint&, CarPhysicsPacket*, Edx) {}  // (not fuzzed: returns this)
PORT_FN(0x004408e0, "CarPhysicsPacket::CarPhysicsPacket", CarPhysicsPacket_ctor, fp_packet_ctor)

// NetCar::Stiffness::copy(src, f) (0x440840): this = src x f, component by component (each stored before the
// next is read)
static void __fastcall Stiffness_copy(Stiffness* self, Edx, const Stiffness* src, float f) {
    for (int i = 0; i < 4; i++) self->k[i] = (float)(D(src->k[i]) * D(f));
}
static void fp_stiffness_copy(Footprint& f, Stiffness* self, Edx, const Stiffness*, float) {
    f.add(self, sizeof(Stiffness), "this");
    f.pure = true;
}
PORT_FN(0x00440840, "NetCar::Stiffness::copy", Stiffness_copy, fp_stiffness_copy)

// =============================================================================================================
// NetCar: construction
// =============================================================================================================

// NetCar::NetCar(data, p) (0x43f370): a Car with remote wheels and no aerodynamics; outside a network game it
// makes the loopback DelayQueue (8 slots of 0x3e bytes; given up if its pool couldn't be made -- read through
// the pointer even when MemAlloc failed, as the original); three zeroed packets; registers with the deity, the
// AI and the network code; the stiffness at rest.
static NetCar* __fastcall NetCar_ctor(NetCar* self, Edx, void* data, void* p) {
    Car_Car(self, 0, data, p);
    for (int i = 0; i < 3; i++) Delta_ctorA(&self->vel[i], 0);
    Delta_ctorA(&self->accel, 0);
    self->vtable = (void**)(uintptr_t)NETCAR_VTABLE;
    for (int i = 0; i < 4; i++) self->wheels[i].is_remote = 1;
    for (int i = 0; i < 7; i++) self->aero[i] = 0;
    set_bits(self->loop_send_time, 0);
    set_bits(self->hit_time, 0xc47a0000);                           // -1000
    if (!MultiEnabled()) {
        void* q = MemAlloc(0x10);
        *P<void*>(S_DELAY_QUEUE) = q ? DelayQueue_ctor(q, 0, 8, 0x3e) : 0;
        void* dq = *P<void*>(S_DELAY_QUEUE);
        if (*(uint32_t*)dq == 0) {                                  // no pool
            if (dq) {
                DelayQueue_dtor(dq, 0);
                operator_delete(dq);
            }
            *P<void*>(S_DELAY_QUEUE) = 0;
        }
    }
    CarPhysicsPacket* pk = (CarPhysicsPacket*)MemAlloc(3 * sizeof(CarPhysicsPacket));
    if (pk) {
        for (int i = 0; i < 3; i++) CarPhysicsPacket_ctorA(&pk[i], 0);
        self->packet_buffer = pk;
    } else {
        self->packet_buffer = 0;
    }
    CarPhysicsPacket* buf = self->packet_buffer;
    self->packets = buf;
    memset(buf, 0, 3 * sizeof(CarPhysicsPacket));                  // rep stosd / stosw (through null if MemAlloc failed)
    self->abandoned = 0;
    self->num_packets = 0;
    self->settling = 1;
    set_bits(self->blend, 0x3f800000);
    void* deity = *P<void*>(S_DEITY);
    VFN(deity, 0x30, void, int32_t, void*)(deity, 0, self->car_index, self);   // RaceDeity::RegisterCar
    AIRegisterNetCar(self, self->car_index);
    MultiRegisterNetCar(self, self->car_index);
    set_bits(self->stiffness.k[0], 0x3ecccccd);                     // 0.4
    set_bits(self->stiffness.k[1], 0x3ecccccd);                     // 0.4
    set_bits(self->stiffness.k[2], 0x3dcccccd);                     // 0.1 (vertical)
    set_bits(self->stiffness.k[3], 0x3ecccccd);                     // 0.4
    memcpy(&self->stiffness_rest, &self->stiffness, sizeof(Stiffness));
    return self;
}
static void fp_netcar_ctor(Footprint& f, NetCar* self, Edx, void*, void*) {
    f.add(self, sizeof(NetCar), "NetCar");
    f.add(P<void>(S_DELAY_QUEUE), 4, "loopback DelayQueue");
    f.replay_only = "constructs a Car, allocates, registers with the deity / AI / network";
}
PORT_FN(0x0043f370, "NetCar::NetCar", NetCar_ctor, fp_netcar_ctor)

// NetCar::~NetCar (0x43f570)
static void __fastcall NetCar_dtor(NetCar* self, Edx) {
    self->vtable = (void**)(uintptr_t)NETCAR_VTABLE;
    if (!MultiEnabled()) {
        if (void* dq = *P<void*>(S_DELAY_QUEUE)) {
            DelayQueue_dtor(dq, 0);
            operator_delete(dq);
        }
        *P<void*>(S_DELAY_QUEUE) = 0;
    }
    MultiUnregisterNetCar(self);
    AIUnregisterCar(self, self->car_index);
    operator_delete(self->packet_buffer);
    self->packet_buffer = 0;
    self->packets = 0;
    Car_dtor(self, 0);
}
static void fp_netcar_dtor(Footprint& f, NetCar* self, Edx) {
    f.add(self, sizeof(NetCar), "NetCar");
    f.add(P<void>(S_DELAY_QUEUE), 4, "loopback DelayQueue");
    f.replay_only = "frees the packets, the queue and the Car";
}
PORT_FN(0x0043f570, "NetCar::~NetCar", NetCar_dtor, fp_netcar_dtor)

// =============================================================================================================
// NetCar: abandonment, horn, fading
// =============================================================================================================

// NetCar::we_are_abandoned (0x43f5f0): the newest packet arrived over 2 s ago -- checked once any packet has come,
// and during the race even before (then against a zeroed packet's time 0)
static unsigned char __fastcall NetCar_we_are_abandoned(NetCar* self, Edx) {
    float now = (float)PhysicsGetTime();
    if (self->num_packets <= 0 && GetGameState() != 1) return 0;
    return D(now) - D(self->packets->recv_time) > D(2.0f) ? 1 : 0;  // fcomp 2.0; test ah,0x41
}
static void fp_net_none(Footprint&, NetCar*, Edx) {}
PORT_FN(0x0043f5f0, "NetCar::we_are_abandoned", NetCar_we_are_abandoned, fp_net_none)

// NetCar::do_horn (0x43f640): the horn went on in the newest packet: throw the horn ball from the car
static void __fastcall NetCar_do_horn(NetCar* self, Edx) {
    if (self->num_packets < 2) return;
    uint8_t h = self->packets[0].horn;
    if (self->packets[1].horn == h || !h) return;
    void* ball = GetBall(self->car_index);
    if (!ball) return;
    Ball_Throw(ball, 0, &self->rot, &self->velocity, bits(self->packets[0].time));
}
static void fp_do_horn(Footprint& f, NetCar* self, Edx) { fp_ball(f, self); }
PORT_FN(0x0043f640, "NetCar::do_horn", NetCar_do_horn, fp_do_horn)

// NetCar::fade_alpha (0x43f690): opacity down 0.016 a tick (an integer compare of its bits), to 0
static void __fastcall NetCar_fade_alpha(NetCar* self, Edx) {
    if ((int32_t)bits(self->opacity) > 0x3c83126f) self->opacity = (float)(D(self->opacity) - D(FB(0x3c83126f)));
    else set_bits(self->opacity, 0);
}
static void fp_fade_alpha(Footprint& f, NetCar* self, Edx) {
    f.add(&self->opacity, 4, "opacity");
    f.pure = true;
}
PORT_FN(0x0043f690, "NetCar::fade_alpha", NetCar_fade_alpha, fp_fade_alpha)

// NetCar::be_abandoned (0x43f6c0): fade; the first time, launch the car straight up at 12.75 m/s, no spin; while
// it rises, pedals off and it's simulated (0); once it falls (vy's bits above 0x80000000) it's frozen (1)
static unsigned char __fastcall NetCar_be_abandoned(NetCar* self, Edx) {
    fade_alphaA(self, 0);
    if (!self->abandoned) {
        set_bits(self->velocity.z, 0);
        set_bits(self->velocity.x, 0);
        set_bits(self->angular_velocity.z, 0);
        set_bits(self->angular_velocity.y, 0);
        self->abandoned = 1;
        set_bits(self->velocity.y, 0x414c0c4a);                     // 12.753
        set_bits(self->angular_velocity.x, 0);
    } else if (bits(self->velocity.y) > 0x80000000u) {
        return 1;
    }
    Car_SetBraking(self, 0, 0);
    Car_SetThrottle(self, 0, 0);
    Car_SetClutch(self, 0, 0);
    return 0;
}
static void fp_be_abandoned(Footprint& f, NetCar* self, Edx) { f.object(self, "netcar"); }
PORT_FN(0x0043f6c0, "NetCar::be_abandoned", NetCar_be_abandoned, fp_be_abandoned)

// =============================================================================================================
// NetCar: packets
// =============================================================================================================

// NetCar::NewPacket(p) (0x43f9c0): a packet no newer than the newest (by send time; a NaN is newer) is dropped;
// else the three shift down, it goes in front stamped with the arrival time, and from the fourth on the horn is
// checked and the interpolation set up again
static void __fastcall NetCar_NewPacket(NetCar* self, Edx, const CarPhysicsPacket* in) {
    if (self->num_packets != 0 && D(self->packets[0].time) >= D(in->time)) return;   // fcomp; test ah,1; je
    crt_memmove(&self->packets[1], &self->packets[0], 2 * sizeof(CarPhysicsPacket));
    uint8_t* dst = (uint8_t*)self->packets;
    __movsd((unsigned long*)dst, (const unsigned long*)in, 15);     // rep movsd; movsw
    __movsw((unsigned short*)(dst + 60), (const unsigned short*)((const uint8_t*)in + 60), 1);
    self->packets[0].recv_time = (float)PhysicsGetTime();
    int32_t n = self->num_packets;
    if (n >= 3) {
        do_hornA(self, 0);
        setup_interpolationA(self, 0);
        return;
    }
    self->num_packets = n + 1;
}
static void fp_new_packet(Footprint& f, NetCar* self, Edx, const CarPhysicsPacket*) {
    fp_netcar(f, self);
    fp_ball(f, self);
}
PORT_FN(0x0043f9c0, "NetCar::NewPacket", NetCar_NewPacket, fp_new_packet)

// NetCar::setup_interpolation (0x43fa50): the velocities move down one; the new one is the derivative at t0 of
// the parabola through the three positions (Lagrange: c_i = d/dt of its basis polynomial at t0), and the
// acceleration the derivative at t0 of the one through the three velocities, capped at 9.81. The blend steps
// back by 1 and runs to 1 again over (t0 - t1) at 60 ticks a second.
//   x uses the differences t0 - t1, t0 - t2 as registers, y and z as stored floats; each axis sums its three
//   terms in its own order.
static void __fastcall NetCar_setup_interpolation(NetCar* self, Edx) {
    crt_memmove(&self->vel[1], &self->vel[0], 2 * sizeof(P3));
    PhysicsGetTime();                                               // called; the result dropped (fstp st(0))
    const CarPhysicsPacket* p = self->packets;
    const double t0 = D(p[0].time), t1 = D(p[1].time), t2 = D(p[2].time);
    const double d01 = t0 - t1, d02 = t0 - t2;                      // registers
    const float f01 = (float)(t0 - t1), f02 = (float)(t0 - t2);    // stored (y, z and the acceleration)
    const double lead = (t0 * D(2.0f) - t1) - t2;                   // 2 t0 - t1 - t2
    // the coefficients of p2 and p1 (register or float numerators) and p0
    const double r2 = (t2 - t0) * (t2 - t1), r1 = (t1 - t2) * (t1 - t0);
    double c2 = d01 / r2, c1 = d02 / r1, c0 = lead / (d01 * d02);
    self->vel[0].x = (float)((c2 * D(p[2].pos.x) + c1 * D(p[1].pos.x)) + c0 * D(p[0].pos.x));
    c2 = D(f01) / r2; c1 = D(f02) / r1; c0 = lead / (D(f01) * D(f02));
    self->vel[0].y = (float)((c2 * D(p[2].pos.y) + c1 * D(p[1].pos.y)) + c0 * D(p[0].pos.y));
    self->vel[0].z = (float)((c0 * D(p[0].pos.z) + c2 * D(p[2].pos.z)) + c1 * D(p[1].pos.z));
    // the acceleration: the same coefficients (float numerators) on the velocities at t0, t1, t2
    self->accel.x = (float)((c0 * D(self->vel[0].x) + c2 * D(self->vel[2].x)) + c1 * D(self->vel[1].x));
    self->accel.y = (float)((c1 * D(self->vel[1].y) + c2 * D(self->vel[2].y)) + c0 * D(self->vel[0].y));
    self->accel.z = (float)((c1 * D(self->vel[1].z) + c2 * D(self->vel[2].z)) + c0 * D(self->vel[0].z));
    P3& a = self->accel;
    double m2 = (D(a.x) * D(a.x) + D(a.y) * D(a.y)) + D(a.z) * D(a.z);
    if (m2 > D(FB(0x42c078e3))) {                                   // 9.81^2: scale to 9.81
        double inv = D(1.0f) / VectorLength(&a);
        a.x = (float)(D(a.x) * inv);
        a.y = (float)(D(a.y) * inv);
        a.z = (float)(inv * D(a.z));
        a.x = (float)(D(a.x) * D(FB(0x411cf5c3)));
        a.y = (float)(D(a.y) * D(FB(0x411cf5c3)));
        a.z = (float)(D(a.z) * D(FB(0x411cf5c3)));
    }
    self->blend = (float)(D(self->blend) - D(1.0f));
    p = self->packets;
    self->blend_step = (float)((D(1.0f) - D(self->blend)) / ((D(p[0].time) - D(p[1].time)) * D(60.0f)));
}
static void fp_net_self(Footprint& f, NetCar* self, Edx) { fp_netcar(f, self); }
PORT_FN(0x0043fa50, "NetCar::setup_interpolation", NetCar_setup_interpolation, fp_net_self)

// NetCar::GetPerceivedRPM (0x43f940): in a replay, or before three packets, the Car's own; else the two newest
// packets' rpm, extrapolated by the time since the newest arrived over the gap between their send times
static double __fastcall NetCar_GetPerceivedRPM(NetCar* self, Edx) {
    if (PhysReplayPlayMode()) return self->perceived_rpm;
    if (self->num_packets < 3) return self->perceived_rpm;
    double now = PhysicsGetTime();
    const CarPhysicsPacket* p = self->packets;
    double a = now - D(p[0].recv_time);
    double b = D(p[0].time) - D(p[1].time);
    double x = (D(1.0f) - a / b) * D(p[1].rpm);
    return a * (D(p[0].rpm) / b) + x;
}
PORT_FN(0x0043f940, "NetCar::GetPerceivedRPM", NetCar_GetPerceivedRPM, fp_net_none)

// =============================================================================================================
// NetCar: steering after the packets
// =============================================================================================================

// NetCar::teleport_to_p0 (0x440260): Car::Teleport (virtual) to the newest packet's position, facing its z axis's
// (x, z) from the quaternion; settling from now
static void __fastcall NetCar_teleport_to_p0(NetCar* self, Edx) {
    const CarPhysicsPacket* p = self->packets;
    const Q4& q = p->rot;
    double y2 = D(q.y) * D(2.0f);
    double c = D(1.0f) - ((D(q.x) * D(2.0f)) * D(q.x) + D(q.y) * y2);
    double s = (D(q.z) * D(2.0f)) * D(q.x) + D(q.w) * y2;
    float dir[2] = {(float)s, (float)c};
    VFN(self, 0x40, void, const P3*, const float*)(self, 0, &p->pos, dir);
    self->settle_time = (float)PhysicsGetTime();
    self->settling = 1;
}
static void fp_net_teleport(Footprint& f, NetCar* self, Edx) {
    fp_car_all(f, self);
    if (self->packets) f.add(self->packets, 3 * sizeof(CarPhysicsPacket), "packets");
}
PORT_FN(0x00440260, "NetCar::teleport_to_p0", NetCar_teleport_to_p0, fp_net_teleport)

// NetCar::update_pos (0x43ff40): where the parabola puts the car now (and its velocity there); more than 9.15 m
// off while the stiffness is at rest: teleport; else position and velocity pulled towards it per axis
static void __fastcall NetCar_update_pos(NetCar* self, Edx) {
    double now = PhysicsGetTime();
    const CarPhysicsPacket* p = self->packets;
    double dt = now - D(p[0].time);
    P3 pred, pvel, tmp;
    pred.x = (float)(((D(self->accel.x) * dt) * D(0.5f) + D(self->vel[0].x)) * dt + D(p[0].pos.x));
    pred.y = (float)(((D(self->accel.y) * dt) * D(0.5f) + D(self->vel[0].y)) * dt + D(p[0].pos.y));
    pred.z = (float)(dt * ((D(self->accel.z) * dt) * D(0.5f) + D(self->vel[0].z)) + D(p[0].pos.z));
    // the velocity now: the parabola's derivative at `now`
    const double t0 = D(p[0].time), t1 = D(p[1].time), t2 = D(p[2].time);
    const double n2 = now * D(2.0f);
    const double k0 = ((n2 - t1) - t2) / ((t0 - t1) * (t0 - t2));
    const double r2 = (t2 - t1) * (t2 - t0), r1 = (t1 - t0) * (t1 - t2);
    const double n2t0 = n2 - t0;                                    // register (x)
    pvel.x = (float)((k0 * D(p[0].pos.x) + ((n2t0 - t1) / r2) * D(p[2].pos.x)) + ((n2t0 - t2) / r1) * D(p[1].pos.x));
    const float n2t0f = (float)(n2 - t0);                          // stored (y, z)
    pvel.y = (float)((k0 * D(p[0].pos.y) + ((D(n2t0f) - t2) / r1) * D(p[1].pos.y)) + ((D(n2t0f) - t1) / r2) * D(p[2].pos.y));
    pvel.z = (float)((k0 * D(p[0].pos.z) + ((D(n2t0f) - t2) / r1) * D(p[1].pos.z)) + ((D(n2t0f) - t1) / r2) * D(p[2].pos.z));
    float dx = (float)(D(self->pos.x) - D(pred.x));
    float dy = (float)(D(self->pos.y) - D(pred.y));
    float dz = (float)(D(self->pos.z) - D(pred.z));
    bool knocked = fabs(D(self->stiffness.k[0]) - D(self->stiffness_rest.k[0])) > D(FB(0x34000000));   // test ah,0x41
    if (!knocked) {
        double d2 = (D(dy) * D(dy) + D(dz) * D(dz)) + D(dx) * D(dx);
        if (d2 >= D(FB(0x42a771ed))) {                             // 9.15^2 (NaN: stays)
            teleport_to_p0A(self, 0);
            return;
        }
    }
    const P3* k = (const P3*)&self->stiffness.k[1];
    P3* r = interpolate_vA(&tmp, &self->pos, &pred, k);
    memcpy(&self->pos, r, sizeof(P3));
    r = interpolate_vA(&tmp, &self->velocity, &pvel, k);
    memcpy(&self->velocity, r, sizeof(P3));
}
PORT_FN(0x0043ff40, "NetCar::update_pos", NetCar_update_pos, fp_net_teleport)

// NetCar::update_o (0x4402e0): the target orientation is the slerp, by the blend, from the quaternion
// extrapolated off packets 2->1 to the one off 1->0 (the blend then steps on); the car's own orientation (its
// matrix as a quaternion) is slerped towards it by the orientation stiffness, less 0.2 per wheel on a bumpy
// surface, and written back as the matrix
static void __fastcall NetCar_update_o(NetCar* self, Edx) {
    float now = (float)PhysicsGetTime();
    Q4 qa, qb, qt, qc;
    get_quat_extrapolationA(&qa, &self->packets[1], &self->packets[0], bits(now));   // (the fractions are dropped)
    get_quat_extrapolationA(&qb, &self->packets[2], &self->packets[1], bits(now));
    Slerp(&qt, &qb, &qa, bits(self->blend));
    self->blend = (float)(D(self->blend_step) + D(self->blend));
    float s = (float)((D(1.0f) - wheels_on_bumpyA(self->wheels)) * D(self->stiffness.k[0]));
    // the matrix as a quaternion (LocalCar::FillNetPacket's conversion, without the renormalisation)
    const float* m = self->rot.m;
    float* q = &qc.x;                                               // x y z w
    double tr = (D(m[8]) + D(m[4])) + D(m[0]);
    float trf = (float)tr;                                          // fcom 0; fstp
    if (tr > 0.0) {
        double r = x87_sqrt(D(trf) + D(1.0f));
        q[3] = (float)(D(0.5f) * r);
        double inv = D(0.5f) / r;
        q[0] = (float)((D(m[5]) - D(m[7])) * inv);
        q[1] = (float)((D(m[6]) - D(m[2])) * inv);
        q[2] = (float)(inv * (D(m[1]) - D(m[3])));
    } else {
        int i = 0;
        if (D(m[4]) > D(m[0])) i = 1;                               // fcomp; test ah,0x41; jne
        if (!(D(m[4 * i]) >= D(m[8]))) i = 2;                       // fcomp; test ah,1; je
        int j = (i + 1) % 3, k = (j + 1) % 3;
        double r = x87_sqrt((D(m[4 * i]) - (D(m[4 * j]) + D(m[4 * k]))) + D(1.0f));
        q[i] = (float)(D(0.5f) * r);
        double inv = D(0.5f) / r;
        q[3] = (float)((D(m[3 * j + k]) - D(m[3 * k + j])) * inv);
        q[j] = (float)((D(m[3 * i + j]) + D(m[3 * j + i])) * inv);
        q[k] = (float)(inv * (D(m[3 * i + k]) + D(m[3 * k + i])));
    }
    Slerp(&qt, &qc, &qt, bits(s));                                  // out = b: Slerp's own business
    // back to the matrix (the products with w and the squares stay in registers; xy, xz, yz are stored)
    const double x2 = D(qt.x) * D(2.0f), y2 = D(qt.y) * D(2.0f), z2 = D(qt.z) * D(2.0f);
    const double wx = D(qt.w) * x2, wy = D(qt.w) * y2, wz = D(qt.w) * z2;
    const float xy = (float)(D(qt.x) * y2), xz = (float)(D(qt.x) * z2);
    const double xx = x2 * D(qt.x);
    const float yz = (float)(D(qt.y) * z2);
    const double yy = y2 * D(qt.y), zz = z2 * D(qt.z);
    float* o = self->rot.m;
    o[0] = (float)(D(1.0f) - (yy + zz));
    o[3] = (float)(D(xy) - wz);
    o[6] = (float)(D(xz) + wy);
    o[1] = (float)(wz + D(xy));
    o[4] = (float)(D(1.0f) - (xx + zz));
    o[7] = (float)(D(yz) - wx);
    o[2] = (float)(D(xz) - wy);
    o[5] = (float)(wx + D(yz));
    o[8] = (float)(D(1.0f) - (xx + yy));
}
PORT_FN(0x004402e0, "NetCar::update_o", NetCar_update_o, fp_net_self)

// NetCar::update_stiffness (0x4406c0): after a knock the stiffness stays down for twice the packet's lag, then
// climbs back linearly over 14 lags (0.05 x rest to rest); the spin is damped by 1 - the orientation stiffness
static void __fastcall NetCar_update_stiffness(NetCar* self, Edx) {
    double now = PhysicsGetTime();
    const CarPhysicsPacket* p = self->packets;
    double lag2 = (D(p->recv_time) - D(p->time)) * D(2.0f);
    float lag2f = (float)lag2;
    float span = (float)(lag2 * D(7.0f));
    double since = now - D(self->hit_time);
    float sincef = (float)since;
    if (since >= D(lag2f)) {                                        // fcom; test ah,1; jne (NaN: skip)
        if (D(span) + D(lag2f) > D(sincef))                         // fcomp; test ah,0x41; jne
            Stiffness_copyA(&self->stiffness, 0, &self->stiffness_rest, bits((float)((D(sincef) - D(lag2f)) / D(span))));
        else
            memcpy(&self->stiffness, &self->stiffness_rest, sizeof(Stiffness));
    }
    double k = D(1.0f) - D(self->stiffness.k[0]);
    P3& w = self->angular_velocity;
    w.x = (float)(D(w.x) * k);
    w.y = (float)(D(w.y) * k);
    w.z = (float)(k * D(w.z));
}
PORT_FN(0x004406c0, "NetCar::update_stiffness", NetCar_update_stiffness, fp_net_self)

// NetCar::go (0x440660): steer after the packets once settled for half a second; teleport when the sender did
static void __fastcall NetCar_go(NetCar* self, Edx) {
    if (!self->settling && PhysicsGetTime() - D(self->settle_time) > D(0.5f)) {
        update_stiffnessA(self, 0);
        update_posA(self, 0);
        update_oA(self, 0);
    }
    if (self->packets->teleported) {
        teleport_to_p0A(self, 0);
        self->packets->teleported = 0;
    }
}
PORT_FN(0x00440660, "NetCar::go", NetCar_go, fp_net_teleport)

// =============================================================================================================
// NetCar::Update (0x43f730)
// =============================================================================================================
// The loopback (no network game): every 1/15 s car 0's packet, munged, goes into the DelayQueue; whatever comes
// out is a new packet. Then: abandoned -> be_abandoned (and Car::Update while it rises); else with three packets,
// the packet's steering / brake / gear / horn on the Car (ebrake, clutch and throttle 0), Car::Update, the wheels'
// broken flags, and fresh -> opaque and go, stale -> fade. The loopback shows the car at 80%.
static void __fastcall NetCar_Update(NetCar* self, Edx) {
    float now = (float)PhysicsGetTime();
    if (!MultiEnabled()) {
        uint8_t out[0x40], in[0x40];
        if (D(now) - D(self->loop_send_time) > D(FB(0x3d888889))) { // 1/15 s
            memcpy(&self->loop_send_time, &now, 4);
            void* car0 = PhysTaskFindCar(0);
            LocalCar_FillNetPacket(car0, 0, out);
            CarPhysicsPacket* m = munge_packetA((CarPhysicsPacket*)out);
            DelayQueue_Enqueue(*P<void*>(S_DELAY_QUEUE), 0, m, 0x3e, 0);
        }
        int32_t size = 0x3e;                                        // in: the buffer's capacity; out: the packet's
        if (DelayQueue_Dequeue(*P<void*>(S_DELAY_QUEUE), 0, in, &size, 0)) NewPacketA(self, 0, (CarPhysicsPacket*)in);
    }
    if (we_are_abandonedA(self, 0)) {
        if (!be_abandonedA(self, 0)) Car_Update(self, 0);
    } else if (self->num_packets == 3) {
        Car_SetSteering(self, 0, bits(self->packets->steering));
        Car_SetEBrake(self, 0, 0);
        Car_SetGear(self, 0, self->packets->gear);
        Car_SetClutch(self, 0, 0);
        Car_SetBraking(self, 0, bits(self->packets->braking));
        Car_SetThrottle(self, 0, 0);
        Car_SetHorn(self, 0, self->packets->horn);
        Car_Update(self, 0);
        for (int i = 0; i < 4; i++) self->wheels[i].broken = self->packets->wheel_broken[i] ? 1 : 0;
        if (!packet_is_too_oldA(bits(self->packets->recv_time))) {
            set_bits(self->opacity, 0x3f800000);
            goA(self, 0);
        } else {
            fade_alphaA(self, 0);
        }
    }
    if (!MultiEnabled()) set_bits(self->opacity, 0x3f4ccccd);       // 0.8
}
static void fp_update(Footprint& f, NetCar* self, Edx) {
    fp_netcar(f, self);
    fp_ball(f, self);                                               // Car::SetHorn, do_horn
    fp_car_update(f, self);
    fp_car_all(f, self);                                            // go -> teleport_to_p0 -> Car::Teleport
    if (!*P<int32_t>(S_MULTI))
        f.replay_only = "the loopback: car 0's packet through a DelayQueue (allocates)";
}
PORT_FN(0x0043f730, "NetCar::Update", NetCar_Update, fp_update)

// =============================================================================================================
// NetCar: the small virtuals
// =============================================================================================================

// NetCar::ApplyExternalForce (0x4407b0): the Car's
static void __fastcall NetCar_ApplyExternalForce(NetCar* self, Edx, const P3* force, const P3* point, int32_t surface) {
    Car_ApplyExternalForce(self, 0, force, point, surface);
}
static void fp_net_force(Footprint& f, NetCar* self, Edx, const P3*, const P3*, int32_t) { f.object(self, "netcar"); }
PORT_FN(0x004407b0, "NetCar::ApplyExternalForce", NetCar_ApplyExternalForce, fp_net_force)

// NetCar::ResolveExternalImpulse (0x4407d0): a knock over 80 (squared 6400) drops the stiffness to 5% from now;
// then the Car's
static void __fastcall NetCar_ResolveExternalImpulse(NetCar* self, Edx, const P3* imp, const P3* point, int32_t surface) {
    double m2 = (D(imp->y) * D(imp->y) + D(imp->z) * D(imp->z)) + D(imp->x) * D(imp->x);
    if (m2 > D(6400.0f)) {
        self->hit_time = (float)PhysicsGetTime();
        Stiffness_copyA(&self->stiffness, 0, &self->stiffness_rest, 0x3d4ccccd);   // 0.05
    }
    Car_ResolveExternalImpulse(self, 0, imp, point, surface);
}
static void fp_net_resolve(Footprint& f, NetCar* self, Edx, const P3*, const P3*, int32_t) { fp_car_all(f, self); }
PORT_FN(0x004407d0, "NetCar::ResolveExternalImpulse", NetCar_ResolveExternalImpulse, fp_net_resolve)

// NetCar::GetPerceivedThrottle (0x440870): 0.9
static float __fastcall NetCar_GetPerceivedThrottle(NetCar*, Edx) { return FB(0x3f666666); }
static void fp_net_pure(Footprint& f, NetCar*, Edx) { f.pure = true; }
PORT_FN(0x00440870, "NetCar::GetPerceivedThrottle", NetCar_GetPerceivedThrottle, fp_net_pure)

// NetCar::MakeReplayPacket (0x440880): the Car's
static void __fastcall NetCar_MakeReplayPacket(NetCar* self, Edx, uint8_t* pkt) { Car_MakeReplayPacket(self, 0, pkt); }
static void fp_net_replay_packet(Footprint& f, NetCar* self, Edx, uint8_t* pkt) {
    f.object(self, "netcar");
    f.add(pkt, 0x3c, "packet");
}
PORT_FN(0x00440880, "NetCar::MakeReplayPacket", NetCar_MakeReplayPacket, fp_net_replay_packet)

// NetCar::ApplyDamage (0x440890): Car::ActuallyApplyDamage with its last argument 1 (the byte argument goes on as
// the whole dword the caller pushed)
static void __fastcall NetCar_ApplyDamage(NetCar* self, Edx, const P3* force, const P3* point, uint32_t flag) {
    Car_ActuallyApplyDamage(self, 0, force, point, flag, 1);
}
static void fp_net_damage(Footprint& f, NetCar* self, Edx, const P3*, const P3*, uint32_t) { fp_car_all(f, self); }
PORT_FN(0x00440890, "NetCar::ApplyDamage", NetCar_ApplyDamage, fp_net_damage)

// NetCar::GetReplayPacketSize (0x4408b0): 0x3c
static int32_t __fastcall NetCar_GetReplayPacketSize(NetCar*, Edx) { return 0x3c; }
PORT_FN(0x004408b0, "NetCar::GetReplayPacketSize", NetCar_GetReplayPacketSize, fp_net_pure)

// NetCar::DoneLowering (0x4408c0): landed after a teleport: not lowering, not settling, the half second from now
static void __fastcall NetCar_DoneLowering(NetCar* self, Edx) {
    self->lowering = 0;
    self->settling = 0;
    self->settle_time = (float)PhysicsGetTime();
}
static void fp_net_object(Footprint& f, NetCar* self, Edx) { f.object(self, "netcar"); }
PORT_FN(0x004408c0, "NetCar::DoneLowering", NetCar_DoneLowering, fp_net_object)
