// phys_dyno.cpp -- M3 3.4 group D: the physics-object base classes and the simple objects (physics:
// phobroot.obj, phobdyno.obj, corner.obj, obstacle.obj, ball.obj, checkpt.obj), rewritten.
//
// PhobRoot is every physics object: a frame, up to four collision volumes, a tick count, the replay packet
// (six 16-bit rotation-vector angles and the position) and the network message. PhobDyno is the rigid body:
// forces and impulses accumulated through the tick, then Update integrates them at the fixed 1/62.5 s step
// (gravity, the corners' ground springs, the ground volume, the averaged impulses and the queued external
// ones, the speed caps of 300 m/s and 10 rad/s, an axis-angle rotation step and the world inverse inertia).
// Obstacle, Wobble, Ball and CheckPoint are the track objects built on them.
//
// Written from the v1.0 disassembly (docs/PORTING.md): the same sums in the same grouping, register values as
// double and stored values as float, float copies made with integer moves kept as bit copies, every call
// made in the original's order with the same arguments (float arguments the original pushes with integer
// moves are passed as their bits), and the comparisons' NaN results as the original's flag tests give them.
// The three axis-angle rotation builds (fsin / fcos, whose results no C type holds) and the replay packet's
// acos / sin are asm transcriptions of the original instruction sequences.
#include <intrin.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// the unused edx of a __thiscall received as __fastcall (an int, so test/fuzz.h can size it)
typedef int Edx;

// a float's bits, read as an integer (volatile: a plain copy could go through the FPU)
static __forceinline uint32_t bits(const float& x) { return *(const volatile uint32_t*)&x; }
static __forceinline void set_bits(void* dst, uint32_t u) { *(volatile uint32_t*)dst = u; }
static __forceinline uint32_t get_bits(const void* src) { return *(const volatile uint32_t*)src; }
static __forceinline float rd_float(const void* p) { float f; memcpy(&f, p, 4); return f; }
// rep movsd: a forward dword copy, as the original makes it (overlap included)
static __forceinline void movsd(void* dst, const void* src, size_t n) {
    __movsd((unsigned long*)dst, (const unsigned long*)src, n);
}

// ---- layouts (phys_types.h has PhobRoot, Corner and PhobDyno; the rest are worked out here) ------------------
// PhobRoot fields not yet named there:  +4 always 0; +8 the constructor's second argument, an offset added to
// GetMessage's buffer; +0xc the PhobData's first dword; +0x28 a body-space point GetMessage sends in world
// space (an obstacle's is 0); +0x68 set to 1 by the constructor.
static inline uint32_t& msg_offset(PhobRoot* r) { return r->_008; }
static inline P3* message_point(PhobRoot* r) { return (P3*)&r->_028[0]; }
// PhobDyno +0x288: the acceleration Update leaves (force x inv_mass)
static inline float* accel(PhobDyno* d) { return (float*)((uint8_t*)d + 0x288); }

struct ExtImpulse {                    // PhobDyno::ExternalImpulse, 28 bytes
    P3 impulse;                        // +0   the impulse times its weight
    P3 point;                          // +0xc
    int32_t surface;                   // +0x18
};
static_assert(sizeof(ExtImpulse) == sizeof(ExternalImpulse), "ExtImpulse");
static inline ExtImpulse* ext(PhobDyno* d) { return (ExtImpulse*)d->external_impulses; }

struct Obstacle : PhobDyno {
    Frame spawn_frame;                 // +0x478 Reset puts the frame back to it
    float last_perturb_time;           // +0x4a8
    uint8_t awake, _4ad[3];            // +0x4ac
};
static_assert(offsetof(Obstacle, spawn_frame) == 0x478 && offsetof(Obstacle, awake) == 0x4ac &&
              sizeof(Obstacle) == 1200, "Obstacle");

struct Wobble : PhobDyno {
    uint32_t _478;                     // +0x478 (not touched here)
    Frame spawn_frame;                 // +0x47c the static object's frame
    int32_t _4ac;                      // +0x4ac PhobData +0x1c
    int32_t settle_timer;              // +0x4b0 20 steps after the last contact
};
static_assert(offsetof(Wobble, spawn_frame) == 0x47c && offsetof(Wobble, settle_timer) == 0x4b0 &&
              sizeof(Wobble) == 1204, "Wobble");

struct Ball : PhobDyno {
    float last_throw_time;             // +0x478
    int32_t index;                     // +0x47c its slot in the ball table (PhobData +0x34)
};
static_assert(offsetof(Ball, index) == 0x47c && sizeof(Ball) == 1152, "Ball");

struct CheckPoint : PhobRoot {
    P3 a, b;                           // +0x6c, +0x78 the gate's two ends (PhobData +8, +0x14)
};
static_assert(offsetof(CheckPoint, b) == 0x78 && sizeof(CheckPoint) == 132, "CheckPoint");

// vtables (v1.0)
static void** const VT_PhobRoot = (void**)0x004dc178;
static void** const VT_PhobStatic = (void**)0x004dc1b0;
static void** const VT_PhobDyno = (void**)0x004dc690;
static void** const VT_Obstacle = (void**)0x004dc0c8;
static void** const VT_Wobble = (void**)0x004dc108;
static void** const VT_Ball = (void**)0x004dc3b0;
static void** const VT_CheckPoint = (void**)0x004dc350;

static Ball** const g_balls = (Ball**)0x00522168;          // ball.obj: 16 slots
static void** const g_deity = (void**)0x005218ac;           // class Deity * deity
enum : uint32_t { ONE = 0x3f800000 };
enum : uint32_t { TAG_GRUP = 0x47525550 };
enum { SURFACE_WATER = 14 };

// ---- the game's functions these call (by address, so a rewrite hooked there is what runs) ----------------
typedef PhobRoot*(__fastcall* PhobRootCtor_t)(void*, void*, const uint8_t* data, void* arg);
typedef PhobDyno*(__fastcall* PhobDynoCtor_t)(void*, void*, const uint8_t* data, void* arg);
typedef void*(__fastcall* Ctor0_t)(void*, void*);
typedef void(__fastcall* Method0_t)(void*, void*);
typedef void(__fastcall* ApplyForce_t)(PhobDyno*, void*, const P3* force, const P3* point);
typedef void(__fastcall* ApplyImpulse_t)(PhobDyno*, void*, const P3* impulse, const P3* point, uint32_t weight);
typedef void(__fastcall* ApplyExternalForce_t)(PhobDyno*, void*, const P3* force, const P3* point, int surface);
typedef void(__fastcall* GetPointVelocity_t)(PhobDyno*, void*, P3* out, const P3* point);
typedef void(__fastcall* CornerUpdate_t)(Corner*, void*, PhobDyno*);
typedef void(__fastcall* CornerSetup_t)(Corner*, void*, const P3* local_pos, uint32_t stiffness, const P3* ray_origin);
typedef void(__fastcall* GetArmPosition_t)(PhobRoot*, void*, P3* out, const P3* in);
typedef void(__fastcall* UpdateReplay_t)(PhobRoot*, void*, const uint8_t* a, const uint8_t* b, uint32_t t);
typedef void(__fastcall* MakeReplayPacket_t)(PhobRoot*, void*, uint8_t* p);
typedef void(__cdecl* MatrixMulPoint_t)(P3*, const P3*, const M3*);
typedef double(__cdecl* VectorLength_t)(const P3*);
typedef void(__cdecl* MatrixMakeIdentity_t)(M3*);
typedef void(__cdecl* MatrixConcat_t)(M3*, const M3*, const M3*);
typedef void(__cdecl* MatrixMakeAxis_t)(M3*, uint32_t angle);
typedef void(__cdecl* MatrixToQuat_t)(Q4*, const M3*);
typedef void(__cdecl* CrossProduct_t)(P3*, const P3*, const P3*);
typedef void(__cdecl* VectorNormalize_t)(P3*);
typedef void(__cdecl* FrameCombine_t)(Frame*, const Frame*, const Frame*, uint32_t t);
typedef uint8_t(__cdecl* TerrainGetIntersection_t)(P3* start, P3* end, P3* hit, P3* normal, int* surface);
typedef void(__cdecl* Collide_t)(CollisionVolume*, CollisionVolume*, const P3* point, const P3* impulse,
                                 const P3* normal);
typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* OperatorDelete_t)(void*);
typedef void(__cdecl* LogReport_t)(const char*, ...);
typedef double(__cdecl* PhysicsGetTime_t)();
typedef uint8_t*(__cdecl* FindStaticObject_t)(int);
typedef int(__cdecl* HackGetCarIndex_t)();
typedef const char*(__cdecl* GetCarFileName_t)(int);
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
typedef void(__cdecl* RegisterBall_t)(Ball*, int);
typedef void(__cdecl* UnregisterBall_t)(Ball*);
typedef CollisionVolume*(__fastcall* SphereVolumeCtor_t)(void*, void*, const Frame*, uint32_t a, uint32_t b,
                                                         const P3* center, uint32_t radius, PhobDyno* owner);
typedef CollisionVolume*(__fastcall* BoxVolumeCtor_t)(void*, void*, const Frame*, uint32_t a, uint32_t b, uint32_t hx,
                                                      uint32_t hy, uint32_t hz, PhobDyno* owner);

#define PhobRootCtor ((PhobRootCtor_t)0x0043daf0)
#define PhobRootDtor ((Method0_t)0x0043db50)
#define PhobRootUpdateReplay ((UpdateReplay_t)0x0043db80)
#define PhobRootMakeReplayPacket ((MakeReplayPacket_t)0x0043df50)
#define GetArmPosition ((GetArmPosition_t)0x0043e0d0)
#define frame_combine ((FrameCombine_t)0x0043e220)
#define VectorNormalize_ ((VectorNormalize_t)0x0043e4a0)
#define PhobDynoCtor ((PhobDynoCtor_t)0x004449c0)
#define PhobDynoReset ((Method0_t)0x00444b80)
#define PhobDynoResetState ((Method0_t)0x00444b90)
#define PhobDynoApplyForce ((ApplyForce_t)0x00444c20)
#define PhobDynoApplyImpulse ((ApplyImpulse_t)0x00444d60)
#define PhobDynoGetPointVelocity ((GetPointVelocity_t)0x00444ee0)
#define PhobDynoUpdate ((Method0_t)0x00445210)
#define PhobDynoApplyExternalForce ((ApplyExternalForce_t)0x00445970)
#define CornerCtor ((Ctor0_t)0x00445990)
#define ExternalImpulseCtor ((Ctor0_t)0x004459d0)
#define CornerUpdate ((CornerUpdate_t)0x0044d900)
#define CornerSetup ((CornerSetup_t)0x0043d7f0)
#define ObstaclePerturb ((Method0_t)0x0043d3e0)
#define register_ball ((RegisterBall_t)0x004411e0)
#define unregister_ball ((UnregisterBall_t)0x00441210)
#define BallDtor ((Method0_t)0x004411f0)
#define MatrixMakeYaw ((MatrixMakeAxis_t)0x00402f80)
#define MatrixMakePitch ((MatrixMakeAxis_t)0x00402fc0)
#define MatrixMakeRoll ((MatrixMakeAxis_t)0x00403000)
#define MatrixConcat ((MatrixConcat_t)0x00403040)
#define VectorLength ((VectorLength_t)0x00429100)
#define MatrixMakeIdentity ((MatrixMakeIdentity_t)0x00429150)
#define MatrixMulPoint ((MatrixMulPoint_t)0x00429420)
#define MatrixToQuat ((MatrixToQuat_t)0x004294c0)
#define MatrixMulPointInv ((MatrixMulPoint_t)0x00436120)
#define CrossProduct ((CrossProduct_t)0x0043b120)
#define CollideContact ((Collide_t)0x0043c710)           // ::Collide: crash sounds and events
#define TerrainGetIntersection ((TerrainGetIntersection_t)0x00465be0)
#define SphereVolumeCtor ((SphereVolumeCtor_t)0x00432340)
#define CubeVolumeCtor ((SphereVolumeCtor_t)0x00432930)
#define BoxVolumeCtor ((BoxVolumeCtor_t)0x00433990)
#define MemAlloc ((MemAlloc_t)0x004140e0)
#define OperatorDelete ((OperatorDelete_t)0x00414390)
#define LogReport ((LogReport_t)0x00411150)
#define PhysicsGetTime ((PhysicsGetTime_t)0x0042bc80)
#define PhysTaskFindStaticObject ((FindStaticObject_t)0x00426d80)
#define HackGetCarIndex ((HackGetCarIndex_t)0x0040d5a0)
#define GetCarFileName ((GetCarFileName_t)0x00406910)
#define game_stricmp ((Stricmp_t)0x004da350)

// ---- footprint helpers --------------------------------------------------------------------------------------
// the splash Sound3D CollideWater updates (a volume.obj global), as phys_sphere.cpp lists it
static void fp_splash(Footprint& f) {
    if (uint8_t* snd = *(uint8_t**)0x00521f7c) f.add(snd + 8, 0x25, "splash Sound3D");
}
// an object's volumes (a sphere group's member spheres too): what CollideGround and the dtor reach
static void fp_volumes(Footprint& f, PhobRoot* self) {
    for (int i = 0; i < self->num_volumes && i < 4; i++) {
        CollisionVolume* v = self->volumes[i];
        if (!v) continue;
        f.object(v, "volume");
        if (v->type_tag == TAG_GRUP) {
            uint8_t* g = (uint8_t*)v;
            int n = *(int32_t*)(g + 0x4c);
            for (int k = 0; k < n && k < 12; k++)
                if (void* s = ((void**)(g + 0x1c))[k]) f.object(s, "group sphere");
        }
    }
}

// =============================================================================================================
// PhobRoot, PhobStatic (phobroot.obj)
// =============================================================================================================

// PhobRoot::PhobRoot(PhobData*, void*) (0x43daf0): the identity frame; no volumes (the array isn't cleared)
static PhobRoot* __fastcall PhobRoot_ctor(PhobRoot* self, Edx, const uint8_t* data, void* arg) {
    uint32_t* r = (uint32_t*)self;
    self->vtable = VT_PhobRoot;
    r[0x28 / 4] = 0;
    r[0x2c / 4] = 0;
    r[0x30 / 4] = 0;
    self->num_volumes = 0;
    uint32_t d0 = get_bits(data);
    self->tick_count = 0;
    r[0xc / 4] = d0;
    r[0x4 / 4] = 0;
    r[0x8 / 4] = (uint32_t)(uintptr_t)arg;
    r[0x3c / 4] = 0;
    r[0x40 / 4] = 0;
    r[0x38 / 4] = ONE;
    r[0x44 / 4] = 0;
    r[0x48 / 4] = ONE;
    r[0x4c / 4] = 0;
    r[0x50 / 4] = 0;
    r[0x54 / 4] = 0;
    r[0x58 / 4] = ONE;
    r[0x5c / 4] = 0;
    r[0x60 / 4] = 0;
    self->_068[0] = 1;
    r[0x64 / 4] = 0;
    return self;
}
static void fp_root_ctor(Footprint& f, PhobRoot* self, Edx, const uint8_t*, void*) {
    f.add(self, sizeof(PhobRoot), "this");
}
PORT_FN(0x0043daf0, "PhobRoot::PhobRoot", PhobRoot_ctor, fp_root_ctor)

// PhobRoot::~PhobRoot (0x43db50): deletes its volumes (each one's deleting destructor, flag 1)
static void __fastcall PhobRoot_dtor(PhobRoot* self, Edx) {
    self->vtable = VT_PhobRoot;
    for (int i = 0; self->num_volumes > i; i++) {
        CollisionVolume* v = self->volumes[i];
        if (v) VFN(v, 0, void*, unsigned)(v, 0, 1);
    }
}
static void fp_root_dtor(Footprint& f, PhobRoot*, Edx) { f.replay_only = "deletes its volumes"; }
PORT_FN(0x0043db50, "PhobRoot::~PhobRoot", PhobRoot_dtor, fp_root_dtor)

// the axis-angle rotation build of PhobRoot::UpdateReplay (0x43dce9..0x43ddac, and again at 0x43de37):
// M from the rotation vector v (|v| = L > 1e-4), instruction for instruction. fsin / fcos return a full
// 64-bit mantissa that every product here takes in unrounded, so it is asm. The stack slots the original
// reuses as temporaries are t0..t4.
static const float k_one = 1.0f;
static __declspec(noinline) void replay_axis_angle(const float* v, const float* L, float* M) {
    float t0, t1, t2, t3, t4;
    __asm {
        mov     ecx, v
        mov     edx, L
        mov     eax, M
        fld     dword ptr [edx]
        fdivr   k_one                   ; 1 / L
        fld     dword ptr [ecx]
        fmul    st, st(1)               ; ax
        fld     dword ptr [ecx + 4]
        fmul    st, st(2)               ; ay
        fxch    st(2)
        fmul    dword ptr [ecx + 8]     ; az        [az ax ay]
        fld     dword ptr [edx]
        fsin                            ; S
        fld     dword ptr [edx]
        fcos                            ; C
        fld     k_one
        fsub    st, st(1)               ; t = 1 - C  [t C S az ax ay]
        fld     st(4)
        fmul    st, st(5)
        fmul    st, st(1)
        fadd    st, st(2)
        fstp    dword ptr [eax]         ; M0
        fld     st(0)
        fmul    st, st(6)
        fmul    st, st(5)
        fstp    t1
        fld     st(2)
        fmul    st, st(4)
        fst     t0
        fadd    t1
        fstp    dword ptr [eax + 4]     ; M1
        fld     st(0)
        fmul    st, st(4)
        fst     t2
        fmul    st, st(5)
        fstp    t3
        fld     st(2)
        fmul    st, st(6)
        fst     t4
        fsubr   t3
        fstp    dword ptr [eax + 8]     ; M2
        fld     t1
        fsub    t0
        fstp    dword ptr [eax + 12]    ; M3
        fld     st(5)
        fmul    st, st(6)
        fmul    st, st(1)
        fadd    st, st(2)
        fstp    dword ptr [eax + 16]    ; M4
        fxch    st(5)
        fmul    t2
        fxch    st(2)
        fmul    st, st(4)
        fld     st(2)
        fadd    st, st(1)
        fstp    dword ptr [eax + 20]    ; M5
        fld     t3
        fadd    t4
        fstp    dword ptr [eax + 24]    ; M6
        _emit   0xdc                    ; fsub st(2), st(0)  (DC EA, as the original)
        _emit   0xea
        fxch    st(2)
        fstp    dword ptr [eax + 28]    ; M7
        fxch    st(2)
        fmul    st, st(0)
        fmul    st, st(4)
        fadd    st, st(2)
        fstp    dword ptr [eax + 32]    ; M8
        fstp    st(0)
        fstp    st(0)
        fstp    st(0)
        fstp    st(0)
    }
}

// a packet's rotation vector: three 16-bit angles, u * (2pi/16384) - 2pi
static const float k_ang_scale = 0.0003834952076431364f;    // 0x39c90fdb
static const float k_two_pi = 6.2831854820251465f;          // 0x40c90fdb
static inline float packet_angle(const uint8_t* p) {
    uint16_t u;
    memcpy(&u, p, 2);
    return (float)(D((int)u) * k_ang_scale - k_two_pi);
}
static void packet_rotation(const float* v, M3* M) {
    double L = x87_sqrt((D(v[2]) * v[2] + D(v[1]) * v[1]) + D(v[0]) * v[0]);
    float Lf = (float)L;
    if (L > 9.999999747378752e-05f) {                        // fcom; test ah,0x41; jne identity
        replay_axis_angle(v, &Lf, M->m);
    } else {
        uint32_t* m = (uint32_t*)M->m;
        m[0] = ONE; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = ONE; m[5] = 0; m[6] = 0; m[7] = 0; m[8] = ONE;
    }
}

// PhobRoot::UpdateReplay(a, b, t) (0x43db80): the frame between two packets -- each packet's position and
// rotation vector made a frame, then frame_combine -- and a tick
static void __fastcall PhobRoot_UpdateReplay(PhobRoot* self, Edx, const uint8_t* a, const uint8_t* b, float t) {
    Frame A, B;
    memcpy(&A.pos, a + 6, 12);
    memcpy(&B.pos, b + 6, 12);
    float va[3], vb[3];
    va[0] = packet_angle(a);
    va[1] = packet_angle(a + 2);
    va[2] = packet_angle(a + 4);
    vb[0] = packet_angle(b);
    vb[1] = packet_angle(b + 2);
    vb[2] = packet_angle(b + 4);
    packet_rotation(va, &A.rot);
    packet_rotation(vb, &B.rot);
    frame_combine(&self->frame, &A, &B, bits(t));
    self->tick_count++;
}
static void fp_root_update_replay(Footprint& f, PhobRoot* self, Edx, const uint8_t*, const uint8_t*, float) {
    f.object(self);
}
PORT_FN(0x0043db80, "PhobRoot::UpdateReplay", PhobRoot_UpdateReplay, fp_root_update_replay)

// acos(w) through the game's CIacos, stored; its fsin (of the unrounded acos) stored; and the flags of
// |sin| against FLT_EPSILON -- MakeReplayPacket 0x43df7d..0x43df9b in one block
static const float k_flt_eps = 1.1920928955078125e-07f;
static __declspec(noinline) int acos_then_sin(const float* w, float* h, float* s) {
    int sw;
    __asm {
        mov     eax, w
        fld     dword ptr [eax]
        mov     eax, 0x004cf07c         ; CIacos
        call    eax
        mov     ecx, h
        fst     dword ptr [ecx]
        fsin
        mov     ecx, s
        fst     dword ptr [ecx]
        fabs
        fcomp   k_flt_eps
        fnstsw  ax
        movzx   eax, ax
        mov     sw, eax
    }
    return sw;
}

// PhobRoot::MakeReplayPacket(p) (0x43df50): the position at +6; the rotation as a rotation vector (the
// frame's quaternion: 2 acos(w) / sin(acos w) times its x, y, z), each component clamped to [-2pi, 2pi]
// and sent as ftol((c + 2pi) * 16384/2pi) in 16 bits
static const float k_ang_inv = 2607.594482421875f;          // 0x4522f983
static void __fastcall PhobRoot_MakeReplayPacket(PhobRoot* self, Edx, uint8_t* p) {
    const uint32_t* pos = (const uint32_t*)&self->frame.pos;
    uint32_t* dst = (uint32_t*)(p + 6);
    dst[0] = pos[0];
    dst[1] = pos[1];
    dst[2] = pos[2];
    Q4 q;
    MatrixToQuat(&q, &self->frame.rot);
    float v[3], s;
    int sw = acos_then_sin(&q.w, &v[0], &s);                // h (acos) shares the slot v.x takes
    if (sw & 0x4100) {                                      // |sin| <= eps, or a NaN
        set_bits(&v[0], 0);
        set_bits(&v[1], 0);
        set_bits(&v[2], 0);
    } else {
        double k = (D(v[0]) * 2.0f) / s;
        v[0] = (float)(D(q.x) * k);
        v[1] = (float)(D(q.y) * k);
        v[2] = (float)(k * q.z);
    }
    for (int i = 0; i < 3; i++) {
        float c;
        if (bits(v[i]) > 0xc0c90fdbu) set_bits(&c, 0xc0c90fdbu);       // unsigned: below -2pi (or a -NaN)
        else c = (D(k_two_pi) > D(v[i])) ? v[i] : k_two_pi;             // fcom st(1); test ah,0x41
        int32_t u = x87_ftol((D(c) + k_two_pi) * k_ang_inv);
        uint16_t w = (uint16_t)u;
        memcpy(p + 2 * i, &w, 2);
    }
}
static void fp_root_make_packet(Footprint& f, PhobRoot*, Edx, uint8_t* p) { f.add(p, 0x12, "packet"); }
PORT_FN(0x0043df50, "PhobRoot::MakeReplayPacket", PhobRoot_MakeReplayPacket, fp_root_make_packet)

// PhobRoot::GetArmPosition(out, in) (0x43e0d0): in (body space) to world space, p x M + pos. With out == in
// the three rotated components are all computed before any is stored.
static void __fastcall PhobRoot_GetArmPosition(PhobRoot* self, Edx, P3* out, const P3* in) {
    const float* M = self->frame.rot.m;
    if (in == out) {
        double Y = (D(M[4]) * in->y + D(M[1]) * in->x) + D(M[7]) * in->z;
        double Z = (D(M[2]) * in->x + D(M[8]) * in->z) + D(M[5]) * in->y;
        double X = (D(M[6]) * in->z + D(M[0]) * in->x) + D(M[3]) * in->y;
        out->x = (float)X;
        out->y = (float)Y;
        out->z = (float)Z;
    } else {
        out->x = (float)((D(M[6]) * in->z + D(M[0]) * in->x) + D(M[3]) * in->y);
        out->y = (float)((D(M[4]) * in->y + D(M[1]) * in->x) + D(M[7]) * in->z);
        out->z = (float)((D(M[2]) * in->x + D(M[8]) * in->z) + D(M[5]) * in->y);
    }
    out->x = (float)(D(self->frame.pos.x) + out->x);
    out->y = (float)(D(self->frame.pos.y) + out->y);
    out->z = (float)(D(self->frame.pos.z) + out->z);
}
static void fp_arm_position(Footprint& f, PhobRoot*, Edx, P3* out, const P3*) {
    f.add(out, 12, "out");
    f.pure = true;
}
PORT_FN(0x0043e0d0, "PhobRoot::GetArmPosition", PhobRoot_GetArmPosition, fp_arm_position)

// PhobStatic::PhobStatic(PhobData*, void*) (0x43e190): the frame from the data (+8); a data +0x38 other than
// 0 is reported (and no volume made); otherwise one BoxVolume of half extents data +0x3c..0x44 (50000, 0.2)
static PhobRoot* __fastcall PhobStatic_ctor(PhobRoot* self, Edx, const uint8_t* data, void* arg) {
    PhobRootCtor(self, 0, data, arg);
    self->vtable = VT_PhobStatic;
    movsd(&self->frame, data + 8, 12);
    if (*(const int32_t*)(data + 0x38) != 0) {
        LogReport((const char*)0x004ed944);                 // "Collision volume:  Not supported"
        return self;
    }
    void* p = MemAlloc(0x30);
    CollisionVolume* v = 0;
    if (p)
        v = BoxVolumeCtor(p, 0, &self->frame, 0x47435000 /* 50000 */, 0x3e4ccccd /* 0.2 */, get_bits(data + 0x3c),
                          get_bits(data + 0x40), get_bits(data + 0x44), 0);
    self->volumes[self->num_volumes] = v;
    self->num_volumes++;
    return self;
}
static void fp_static_ctor(Footprint& f, PhobRoot*, Edx, const uint8_t*, void*) {
    f.replay_only = "allocates its BoxVolume (MemAlloc)";
}
PORT_FN(0x0043e190, "PhobStatic::PhobStatic", PhobStatic_ctor, fp_static_ctor)

// frame_combine(out, a, b, t) (0x43e220): each of the twelve floats a + (b - a) t, stored as it goes; then
// the rotation re-orthonormalised: rows 0 and 2 normalised, row 1 = normalize(row2 x row0), row 0 normalised
// again (_VectorNormalize), row 2 = row0 x row1
static void __cdecl frame_combine_rw(Frame* out, const Frame* a, const Frame* b, float t) {
    float* o = (float*)out;
    const float* x = (const float*)a;
    const float* y = (const float*)b;
    for (int i = 0; i < 12; i++) o[i] = (float)((D(y[i]) - x[i]) * t + x[i]);
    float* m = out->rot.m;
    double k = 1.0 / x87_sqrt((D(m[2]) * m[2] + D(m[1]) * m[1]) + D(m[0]) * m[0]);
    m[0] = (float)(D(m[0]) * k);
    m[1] = (float)(D(m[1]) * k);
    m[2] = (float)(k * m[2]);
    k = 1.0 / x87_sqrt((D(m[5]) * m[5] + D(m[4]) * m[4]) + D(m[3]) * m[3]);
    m[3] = (float)(D(m[3]) * k);
    m[4] = (float)(D(m[4]) * k);
    m[5] = (float)(k * m[5]);
    k = 1.0 / x87_sqrt((D(m[6]) * m[6] + D(m[7]) * m[7]) + D(m[8]) * m[8]);
    P3 r0, r1, r2;
    memcpy(&r0, &m[0], 12);
    m[6] = (float)(D(m[6]) * k);
    m[7] = (float)(D(m[7]) * k);
    m[8] = (float)(k * m[8]);
    memcpy(&r2, &m[6], 12);
    CrossProduct(&r1, &r2, &r0);
    VectorNormalize_(&r0);
    VectorNormalize_(&r1);
    CrossProduct(&r2, &r0, &r1);
    memcpy(&m[0], &r0, 12);
    memcpy(&m[3], &r1, 12);
    memcpy(&m[6], &r2, 12);
}
static void fp_frame_combine(Footprint& f, Frame* out, const Frame*, const Frame*, float) {
    f.add(out, sizeof(Frame), "out");
    f.pure = true;
}
PORT_FN(0x0043e220, "frame_combine", frame_combine_rw, fp_frame_combine)

// PhobRoot::Reset (0x43e3f0): a tick. (PhobRoot::Update 0x43e400 is a bare `ret`: not rewritten.)
static void __fastcall PhobRoot_Reset(PhobRoot* self, Edx) { self->tick_count++; }
static void fp_root_reset(Footprint& f, PhobRoot* self, Edx) {
    f.add(&self->tick_count, 2, "tick_count");
    f.pure = true;
}
PORT_FN(0x0043e3f0, "PhobRoot::Reset", PhobRoot_Reset, fp_root_reset)

// the constant getters
static int __fastcall PhobRoot_GetReplayPacketSize(PhobRoot*, Edx) { return 0x12; }
static void fp_root_getter(Footprint& f, PhobRoot*, Edx) { f.pure = true; }
PORT_FN(0x0043e410, "PhobRoot::GetReplayPacketSize", PhobRoot_GetReplayPacketSize, fp_root_getter)

static PhobDyno* __fastcall PhobRoot_GetDyno(PhobRoot*, Edx) { return 0; }
PORT_FN(0x0043e420, "PhobRoot::GetDyno", PhobRoot_GetDyno, fp_root_getter)

static uint8_t __fastcall PhobRoot_IsDynamic(PhobRoot*, Edx) { return 0; }
PORT_FN(0x0043e430, "PhobRoot::IsDynamic", PhobRoot_IsDynamic, fp_root_getter)

// (PhobStatic::GetMessage 0x43e460 is a bare `ret 4`: not rewritten)
static int __fastcall PhobStatic_GetMessageSize(PhobRoot*, Edx) { return 0; }
PORT_FN(0x0043e470, "PhobStatic::GetMessageSize", PhobStatic_GetMessageSize, fp_root_getter)

// the deleting destructors: ~PhobRoot (or ~Ball), then operator delete when flag 1 is set
static void fp_deleting_dtor(Footprint& f, PhobRoot*, Edx, unsigned) {
    f.replay_only = "deletes its volumes and frees the object";
}
static void* __fastcall PhobRoot_vector_deleting_dtor(PhobRoot* self, Edx, unsigned flags) {
    PhobRootDtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
PORT_FN(0x0043e440, "PhobRoot::`vector deleting destructor'", PhobRoot_vector_deleting_dtor, fp_deleting_dtor)

static void* __fastcall PhobStatic_vector_deleting_dtor(PhobRoot* self, Edx, unsigned flags) {
    PhobRootDtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
PORT_FN(0x0043e480, "PhobStatic::`vector deleting destructor'", PhobStatic_vector_deleting_dtor, fp_deleting_dtor)

// _VectorNormalize(v) (0x43e4a0): v / |v|, |v| summed x, y, z
static void __cdecl VectorNormalize_rw(P3* v) {
    double k = 1.0 / x87_sqrt((D(v->x) * v->x + D(v->y) * v->y) + D(v->z) * v->z);
    v->x = (float)(D(v->x) * k);
    v->y = (float)(D(v->y) * k);
    v->z = (float)(k * v->z);
}
static void fp_vector_normalize(Footprint& f, P3* v) {
    f.add(v, 12, "v");
    f.pure = true;
}
PORT_FN(0x0043e4a0, "_VectorNormalize", VectorNormalize_rw, fp_vector_normalize)

// =============================================================================================================
// PhobDyno (phobdyno.obj)
// =============================================================================================================

// PhobDyno::PhobDyno(PhobData*, void*) (0x4449c0): a PhobRoot, eight Corners, sixteen ExternalImpulses; the
// mass (data +8, lb) x 0.4545, the principal moments (data +0xc..0x14) x 0.04228; their inverses, the
// diagonal inverse inertia (body and world); no corners, no ground collision; then Reset
static PhobDyno* __fastcall PhobDyno_ctor(PhobDyno* self, Edx, const uint8_t* data, void* arg) {
    PhobRootCtor(self, 0, data, arg);
    for (int i = 0; i < 8; i++) CornerCtor(&self->corners[i], 0);
    for (int i = 0; i < 16; i++) ExternalImpulseCtor(&self->external_impulses[i], 0);
    const float* df = (const float*)data;
    self->vtable = VT_PhobDyno;
    self->collide_ground = 0;
    self->num_corners = 0;
    self->mass = (float)(D(df[2]) * 0.4544999897480011f);
    self->inertia.x = (float)(D(df[3]) * 0.042279861867427826f);
    self->inertia.y = (float)(D(df[4]) * 0.042279861867427826f);
    self->inertia.z = (float)(D(df[5]) * 0.042279861867427826f);
    self->inv_mass = (float)(1.0 / D(self->mass));
    self->inv_inertia.x = (float)(1.0 / D(self->inertia.x));
    double iy = 1.0 / D(self->inertia.y);
    uint32_t* ib = (uint32_t*)self->inv_inertia_body.m;
    ib[1] = 0; ib[2] = 0; ib[3] = 0;
    ib[4] = ONE;
    ib[5] = 0; ib[6] = 0; ib[7] = 0;
    ib[8] = ONE;
    ib[0] = ONE;
    ib[0] = get_bits(&self->inv_inertia.x);
    self->inv_inertia.y = (float)iy;
    double iz = 1.0 / D(self->inertia.z);
    ib[4] = get_bits(&self->inv_inertia.y);
    self->inv_inertia.z = (float)iz;
    ib[8] = get_bits(&self->inv_inertia.z);
    movsd(&self->inv_inertia_world, &self->inv_inertia_body, 9);
    PhobDynoReset(self, 0);
    return self;
}
static void fp_dyno_ctor(Footprint& f, PhobDyno* self, Edx, const uint8_t*, void*) {
    f.add(self, sizeof(PhobDyno), "this");
}
PORT_FN(0x004449c0, "PhobDyno::PhobDyno", PhobDyno_ctor, fp_dyno_ctor)

// PhobDyno::Reset (0x444b80, virtual): a tick, then reset
static void __fastcall PhobDyno_Reset(PhobDyno* self, Edx) {
    self->tick_count++;
    PhobDynoResetState(self, 0);
}
static void fp_dyno_self(Footprint& f, PhobDyno* self, Edx) {
    f.add(self, sizeof(PhobDyno), "dyno");
    f.pure = true;
}
PORT_FN(0x00444b80, "PhobDyno::Reset", PhobDyno_Reset, fp_dyno_self)

// PhobDyno::reset (0x444b90): still -- velocities, the force and torque, the acceleration, the impulses,
// their weight and the external-impulse queue all zero
static void __fastcall PhobDyno_reset(PhobDyno* self, Edx) {
    uint8_t* d = (uint8_t*)self;
    for (uint32_t o = 0x234; o <= 0x260; o += 4) set_bits(d + o, 0);
    for (uint32_t o = 0x288; o <= 0x2ac; o += 4) set_bits(d + o, 0);
    self->num_external_impulses = 0;
}
PORT_FN(0x00444b90, "PhobDyno::reset(private)", PhobDyno_reset, fp_dyno_self)

// PhobDyno::ApplyForce(force, point) (0x444c20): unless |force|^2 is within FLT_EPSILON of 0, the force
// and the torque (point - pos) x force are added
static void __fastcall PhobDyno_ApplyForce(PhobDyno* self, Edx, const P3* F, const P3* P) {
    double m2 = (D(F->x) * F->x + D(F->y) * F->y) + D(F->z) * F->z;
    if (!(fabs(m2) > 1.1920928955078125e-07f)) return;      // fabs; fcomp; test ah,0x41
    float r[3];
    r[0] = (float)(D(P->x) - self->frame.pos.x);
    r[1] = (float)(D(P->y) - self->frame.pos.y);
    r[2] = (float)(D(P->z) - self->frame.pos.z);
    float c[3];
    c[0] = (float)(D(F->z) * r[1] - D(F->y) * r[2]);
    c[1] = (float)(D(F->x) * r[2] - D(F->z) * r[0]);
    c[2] = (float)(D(F->y) * r[0] - D(F->x) * r[1]);
    self->force.x = (float)(D(F->x) + self->force.x);
    self->force.y = (float)(D(F->y) + self->force.y);
    self->force.z = (float)(D(F->z) + self->force.z);
    self->torque.x = (float)(D(self->torque.x) + c[0]);
    self->torque.y = (float)(D(self->torque.y) + c[1]);
    self->torque.z = (float)(D(self->torque.z) + c[2]);
}
static void fp_dyno_apply_force(Footprint& f, PhobDyno* self, Edx, const P3*, const P3*) {
    f.add(self, sizeof(PhobDyno), "dyno");
    f.pure = true;
}
PORT_FN(0x00444c20, "PhobDyno::ApplyForce", PhobDyno_ApplyForce, fp_dyno_apply_force)

// PhobDyno::ApplyTorque(torque) (0x444d20)
static void __fastcall PhobDyno_ApplyTorque(PhobDyno* self, Edx, const P3* T) {
    self->torque.x = (float)(D(T->x) + self->torque.x);
    self->torque.y = (float)(D(T->y) + self->torque.y);
    self->torque.z = (float)(D(T->z) + self->torque.z);
}
static void fp_dyno_apply_torque(Footprint& f, PhobDyno* self, Edx, const P3*) {
    f.add(self, sizeof(PhobDyno), "dyno");
    f.pure = true;
}
PORT_FN(0x00444d20, "PhobDyno::ApplyTorque", PhobDyno_ApplyTorque, fp_dyno_apply_torque)

// PhobDyno::ApplyImpulse(impulse, point, weight) (0x444d60): the weight summed; the impulse and its moment
// (point - pos) x impulse summed, each times the weight (Update divides by the sum)
static void __fastcall PhobDyno_ApplyImpulse(PhobDyno* self, Edx, const P3* I, const P3* P, float w) {
    self->impulse_weight = (float)(D(self->impulse_weight) + w);
    self->impulse.x = (float)(D(I->x) * w + self->impulse.x);
    self->impulse.y = (float)(D(I->y) * w + self->impulse.y);
    self->impulse.z = (float)(D(I->z) * w + self->impulse.z);
    float r[3];
    r[0] = (float)(D(P->x) - self->frame.pos.x);
    r[1] = (float)(D(P->y) - self->frame.pos.y);
    r[2] = (float)(D(P->z) - self->frame.pos.z);
    float c[3];
    c[0] = (float)(D(I->z) * r[1] - D(I->y) * r[2]);
    c[1] = (float)(D(I->x) * r[2] - D(I->z) * r[0]);
    c[2] = (float)(D(I->y) * r[0] - D(I->x) * r[1]);
    self->angular_impulse.x = (float)(D(c[0]) * w + self->angular_impulse.x);
    self->angular_impulse.y = (float)(D(c[1]) * w + self->angular_impulse.y);
    self->angular_impulse.z = (float)(D(c[2]) * w + self->angular_impulse.z);
}
static void fp_dyno_apply_impulse(Footprint& f, PhobDyno* self, Edx, const P3*, const P3*, float) {
    f.add(self, sizeof(PhobDyno), "dyno");
    f.pure = true;
}
PORT_FN(0x00444d60, "PhobDyno::ApplyImpulse", PhobDyno_ApplyImpulse, fp_dyno_apply_impulse)

// PhobDyno::QueueExternalImpulse(impulse, point, surface, weight) (0x444e60): kept (impulse x weight, the
// point, the surface) while fewer than 16 are queued, for Update to hand to ResolveExternalImpulse; applied
// as an impulse either way
static void __fastcall PhobDyno_QueueExternalImpulse(PhobDyno* self, Edx, const P3* I, const P3* P, int surface,
                                                     float w) {
    int n = self->num_external_impulses;
    if (n < 16) {
        ExtImpulse* e = &ext(self)[n];
        e->impulse.x = (float)(D(I->x) * w);
        e->impulse.y = (float)(D(I->y) * w);
        e->impulse.z = (float)(D(I->z) * w);
        const uint32_t* s = (const uint32_t*)P;
        uint32_t* d = (uint32_t*)&e->point;
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        e->surface = surface;
        self->num_external_impulses = self->num_external_impulses + 1;
    }
    PhobDynoApplyImpulse(self, 0, I, P, bits(w));
}
static void fp_dyno_queue(Footprint& f, PhobDyno* self, Edx, const P3*, const P3*, int, float) {
    f.add(self, sizeof(PhobDyno), "dyno");
}
PORT_FN(0x00444e60, "PhobDyno::QueueExternalImpulse", PhobDyno_QueueExternalImpulse, fp_dyno_queue)

// PhobDyno::GetPointVelocity(out, point) (0x444ee0): v + w x (point - pos). With out == &angular_velocity
// the cross product is all computed before it's stored.
static void __fastcall PhobDyno_GetPointVelocity(PhobDyno* self, Edx, P3* out, const P3* P) {
    float r[3];
    r[0] = (float)(D(P->x) - self->frame.pos.x);
    r[1] = (float)(D(P->y) - self->frame.pos.y);
    r[2] = (float)(D(P->z) - self->frame.pos.z);
    const P3& w = self->angular_velocity;
    if ((uint8_t*)out == (uint8_t*)&self->angular_velocity) {
        double cy = D(w.z) * r[0] - D(w.x) * r[2];
        double cz = D(w.x) * r[1] - D(w.y) * r[0];
        double cx = D(w.y) * r[2] - D(w.z) * r[1];
        out->x = (float)cx;
        out->y = (float)cy;
        out->z = (float)cz;
    } else {
        out->x = (float)(D(w.y) * r[2] - D(w.z) * r[1]);
        out->y = (float)(D(w.z) * r[0] - D(w.x) * r[2]);
        out->z = (float)(D(w.x) * r[1] - D(w.y) * r[0]);
    }
    out->x = (float)(D(self->velocity.x) + out->x);
    out->y = (float)(D(self->velocity.y) + out->y);
    out->z = (float)(D(self->velocity.z) + out->z);
}
static void fp_point_velocity(Footprint& f, PhobDyno*, Edx, P3* out, const P3*) {
    f.add(out, 12, "out");
    f.pure = true;
}
PORT_FN(0x00444ee0, "PhobDyno::GetPointVelocity", PhobDyno_GetPointVelocity, fp_point_velocity)

// PhobDyno::GetPointMoment(out, point) (0x444ff0): the momentum m v, plus the angular momentum about the
// point's arm, M^T (I (M (w x r))) with I the principal moments
static void __fastcall PhobDyno_GetPointMoment(PhobDyno* self, Edx, P3* out, const P3* P) {
    double m = self->mass;
    out->x = (float)(D(self->velocity.x) * m);
    out->y = (float)(D(self->velocity.y) * m);
    out->z = (float)(m * self->velocity.z);
    float r[3];
    r[0] = (float)(D(P->x) - self->frame.pos.x);
    r[1] = (float)(D(P->y) - self->frame.pos.y);
    r[2] = (float)(D(P->z) - self->frame.pos.z);
    const P3& w = self->angular_velocity;
    float c[3];
    c[0] = (float)(D(w.y) * r[2] - D(w.z) * r[1]);
    c[1] = (float)(D(w.z) * r[0] - D(w.x) * r[2]);
    c[2] = (float)(D(w.x) * r[1] - D(w.y) * r[0]);
    const float* M = self->frame.rot.m;
    float t[3];
    t[0] = (float)((D(M[1]) * c[1] + D(M[2]) * c[2]) + D(M[0]) * c[0]);
    t[1] = (float)((D(M[5]) * c[2] + D(M[4]) * c[1]) + D(M[3]) * c[0]);
    t[2] = (float)((D(M[8]) * c[2] + D(M[6]) * c[0]) + D(M[7]) * c[1]);
    t[0] = (float)(D(self->inertia.x) * t[0]);
    t[1] = (float)(D(self->inertia.y) * t[1]);
    t[2] = (float)(D(self->inertia.z) * t[2]);
    float u[3];
    u[0] = (float)((D(M[6]) * t[2] + D(M[3]) * t[1]) + D(M[0]) * t[0]);
    u[1] = (float)((D(M[1]) * t[0] + D(M[4]) * t[1]) + D(M[7]) * t[2]);
    u[2] = (float)((D(M[8]) * t[2] + D(M[5]) * t[1]) + D(M[2]) * t[0]);
    out->x = (float)(D(out->x) + u[0]);
    out->y = (float)(D(out->y) + u[1]);
    out->z = (float)(D(out->z) + u[2]);
}
static void fp_point_moment(Footprint& f, PhobDyno*, Edx, P3* out, const P3*) {
    f.add(out, 12, "out");
    f.pure = true;
}
PORT_FN(0x00444ff0, "PhobDyno::GetPointMoment", PhobDyno_GetPointMoment, fp_point_moment)

// the axis-angle rotation build of PhobDyno::Update (0x44571b..0x4457fb), instruction for instruction (its
// grouping differs from UpdateReplay's); the original's reused stack slots are t10..t1c
static __declspec(noinline) void dyno_axis_angle(const float* d, const float* L, float* M) {
    float t10, t14, t18, t1c;
    __asm {
        mov     ecx, d
        mov     edx, L
        mov     eax, M
        fld     dword ptr [edx]
        fdivr   k_one                   ; 1 / L
        fld     dword ptr [ecx]
        fmul    st, st(1)               ; ax
        fld     dword ptr [ecx + 4]
        fmul    st, st(2)               ; ay
        fxch    st(2)
        fmul    dword ptr [ecx + 8]     ; az        [az ax ay]
        fld     dword ptr [edx]
        fsin
        fld     dword ptr [edx]
        fcos
        fld     k_one
        fsub    st, st(1)               ; [t C S az ax ay]
        fld     st(4)
        fmul    st, st(5)
        fmul    st, st(1)
        fadd    st, st(2)
        fstp    dword ptr [eax]         ; M0
        fld     st(4)
        fmul    st, st(1)
        fst     t10
        fmul    st, st(6)
        fstp    t18
        fld     st(2)
        fmul    st, st(4)
        fst     t14
        fadd    t18
        fstp    dword ptr [eax + 4]     ; M1
        fld     t10
        fmul    st, st(4)
        fstp    t1c
        fld     st(2)
        fmul    st, st(6)
        fst     t10
        fsubr   t1c
        fstp    dword ptr [eax + 8]     ; M2
        fld     t18
        fsub    t14
        fstp    dword ptr [eax + 12]    ; M3
        fld     st(5)
        fmul    st, st(6)
        fmul    st, st(1)
        fadd    st, st(2)
        fstp    dword ptr [eax + 16]    ; M4
        fld     st(0)
        fmul    st, st(4)
        fmul    st, st(6)
        fstp    t14
        fxch    st(4)
        fmul    st, st(2)
        fld     t14
        fadd    st, st(1)
        fstp    dword ptr [eax + 20]    ; M5
        fld     t1c
        fadd    t10
        fstp    dword ptr [eax + 24]    ; M6
        fsubr   t14
        fstp    dword ptr [eax + 28]    ; M7
        fxch    st(2)
        fmul    st, st(0)
        fmul    st, st(3)
        fadd    st, st(2)
        fstp    dword ptr [eax + 32]    ; M8
        fstp    st(0)
        fstp    st(0)
        fstp    st(0)
        fstp    st(0)
    }
}

// PhobDyno::Update (0x445210, virtual): one 0.016 s step
static void __fastcall PhobDyno_Update(PhobDyno* self, Edx) {
    uint8_t* d = (uint8_t*)self;
    self->force.y = (float)(D(self->mass) * -9.8100004196167f + self->force.y);
    for (int i = 0; self->num_corners > i; i++) CornerUpdate(&self->corners[i], 0, self);
    if (self->num_volumes > 0 && self->volumes[0] && self->collide_ground) {
        CollisionVolume* v = self->volumes[0];
        VFN(v, 0xc, void)(v, 0);                            // CollideGround
    }
    if (fabs(D(self->impulse_weight)) > 1.1920928955078125e-07f) {     // fabs; fcomp; test ah,0x41
        double k = 1.0 / D(self->impulse_weight);
        self->impulse.x = (float)(D(self->impulse.x) * k);
        self->impulse.y = (float)(D(self->impulse.y) * k);
        self->impulse.z = (float)(D(self->impulse.z) * k);
        self->angular_impulse.x = (float)(D(self->angular_impulse.x) * k);
        self->angular_impulse.y = (float)(D(self->angular_impulse.y) * k);
        self->angular_impulse.z = (float)(k * self->angular_impulse.z);
        self->force.x = (float)(D(self->impulse.x) * 62.5f + self->force.x);
        self->force.y = (float)(D(self->impulse.y) * 62.5f + self->force.y);
        self->force.z = (float)(D(self->impulse.z) * 62.5f + self->force.z);
        self->torque.x = (float)(D(self->angular_impulse.x) * 62.5f + self->torque.x);
        self->torque.y = (float)(D(self->angular_impulse.y) * 62.5f + self->torque.y);
        self->torque.z = (float)(D(self->angular_impulse.z) * 62.5f + self->torque.z);
        for (uint32_t o = 0x294; o <= 0x2a8; o += 4) set_bits(d + o, 0);
        if (self->num_external_impulses > 0) {
            typedef void(__fastcall * Resolve_t)(PhobDyno*, void*, const P3*, const P3*, int);
            Resolve_t resolve = (Resolve_t)self->vtable[0x38 / 4];     // read once, before the loop
            int i = 0;
            do {
                double kk = 1.0 / D(self->impulse_weight);
                ExtImpulse* e = &ext(self)[i];
                i++;
                e->impulse.x = (float)(D(e->impulse.x) * kk);
                e->impulse.y = (float)(D(e->impulse.y) * kk);
                e->impulse.z = (float)(kk * e->impulse.z);
                resolve(self, 0, &e->impulse, &e->point, e->surface);
            } while (self->num_external_impulses > i);
        }
    }
    double k = D(self->inv_mass) * 0.01600000075995922f;
    set_bits(&self->impulse_weight, 0);
    self->num_external_impulses = 0;
    self->velocity.x = (float)(D(self->force.x) * k + self->velocity.x);
    self->velocity.y = (float)(D(self->force.y) * k + self->velocity.y);
    self->velocity.z = (float)(k * self->force.z + self->velocity.z);
    double im = self->inv_mass;
    float* acc = accel(self);
    acc[0] = (float)(D(self->force.x) * im);
    acc[1] = (float)(D(self->force.y) * im);
    acc[2] = (float)(im * self->force.z);
    P3& v = self->velocity;
    double s2 = (D(v.y) * v.y + D(v.z) * v.z) + D(v.x) * v.x;
    float s2f = (float)s2;
    if (s2 > 90000.0f) {                                    // fcom; test ah,0x41: 300 m/s at most
        double kv = 1.0 / x87_sqrt(s2f);
        v.x = (float)(D(v.x) * kv);
        v.y = (float)(D(v.y) * kv);
        v.z = (float)(kv * v.z);
        v.x = (float)(D(v.x) * 300.0f);
        v.y = (float)(D(v.y) * 300.0f);
        v.z = (float)(D(v.z) * 300.0f);
    }
    self->frame.pos.x = (float)(D(v.x) * 0.01600000075995922f + self->frame.pos.x);
    self->frame.pos.y = (float)(D(v.y) * 0.01600000075995922f + self->frame.pos.y);
    self->frame.pos.z = (float)(D(v.z) * 0.01600000075995922f + self->frame.pos.z);
    P3 aa;
    MatrixMulPoint(&aa, &self->torque, &self->inv_inertia_world);
    P3& w = self->angular_velocity;
    w.x = (float)(D(aa.x) * 0.01600000075995922f + w.x);
    w.y = (float)(D(aa.y) * 0.01600000075995922f + w.y);
    w.z = (float)(D(aa.z) * 0.01600000075995922f + w.z);
    double w2 = (D(w.z) * w.z + D(w.x) * w.x) + D(w.y) * w.y;
    float w2f = (float)w2;
    if (w2 > 100.0f) {                                      // 10 rad/s at most
        double kw = 1.0 / x87_sqrt(w2f);
        w.x = (float)(D(w.x) * kw);
        w.y = (float)(D(w.y) * kw);
        w.z = (float)(kw * w.z);
        w.x = (float)(D(w.x) * 10.0f);
        w.y = (float)(D(w.y) * 10.0f);
        w.z = (float)(D(w.z) * 10.0f);
    }
    P3 step;
    step.x = (float)(D(w.x) * 0.01600000075995922f);
    step.y = (float)(D(w.y) * 0.01600000075995922f);
    step.z = (float)(D(w.z) * 0.01600000075995922f);
    double L = VectorLength(&step);
    float Lf = (float)L;
    M3 R;
    if (L > 9.999999747378752e-05f) dyno_axis_angle(&step.x, &Lf, R.m);    // fcom; test ah,0x41
    else MatrixMakeIdentity(&R);
    M3* rot = &self->frame.rot;
    MatrixConcat(rot, rot, &R);
    M3 T;                                                   // the rotation, transposed (integer moves)
    movsd(&T, rot, 9);
    uint32_t* t = (uint32_t*)T.m;
    uint32_t t1 = t[1], t3 = t[3], t2 = t[2];
    t[1] = t3;
    uint32_t t5 = t[5];
    t[3] = t1;
    uint32_t t6 = t[6];
    t[2] = t6;
    t[6] = t2;
    uint32_t t7 = t[7];
    t[5] = t7;
    t[7] = t5;
    M3 T2;
    MatrixConcat(&T2, &T, &self->inv_inertia_body);
    MatrixConcat(&self->inv_inertia_world, &T2, rot);
    set_bits(&self->force.x, 0);
    set_bits(&self->force.y, 0);
    self->tick_count++;
    set_bits(&self->force.z, 0);
    set_bits(&self->torque.x, 0);
    set_bits(&self->torque.y, 0);
    set_bits(&self->torque.z, 0);
}
static void fp_dyno_update(Footprint& f, PhobDyno* self, Edx) {
    f.object(self);
    fp_volumes(f, self);
    fp_splash(f);
}
PORT_FN(0x00445210, "PhobDyno::Update", PhobDyno_Update, fp_dyno_update)

// PhobDyno::UpdateReplay(a, b, t) (0x4458d0): no queued impulses; PhobRoot's frame; the velocity between the
// packets' (+0x12)
static void __fastcall PhobDyno_UpdateReplay(PhobDyno* self, Edx, const uint8_t* a, const uint8_t* b, float t) {
    self->num_external_impulses = 0;
    PhobRootUpdateReplay(self, 0, a, b, bits(t));
    double z = (D(rd_float(b + 0x1a)) - rd_float(a + 0x1a)) * t + rd_float(a + 0x1a);
    double y = (D(rd_float(b + 0x16)) - rd_float(a + 0x16)) * t + rd_float(a + 0x16);
    double x = (D(rd_float(b + 0x12)) - rd_float(a + 0x12)) * t + rd_float(a + 0x12);
    self->velocity.x = (float)x;
    self->velocity.y = (float)y;
    self->velocity.z = (float)z;
}
static void fp_dyno_update_replay(Footprint& f, PhobDyno* self, Edx, const uint8_t*, const uint8_t*, float) {
    f.object(self);
}
PORT_FN(0x004458d0, "PhobDyno::UpdateReplay", PhobDyno_UpdateReplay, fp_dyno_update_replay)

// PhobDyno::MakeReplayPacket(p) (0x445940): PhobRoot's packet, then the velocity at +0x12
static void __fastcall PhobDyno_MakeReplayPacket(PhobDyno* self, Edx, uint8_t* p) {
    PhobRootMakeReplayPacket(self, 0, p);
    const uint32_t* s = (const uint32_t*)&self->velocity;
    uint32_t* dst = (uint32_t*)(p + 0x12);
    dst[0] = s[0];
    dst[1] = s[1];
    dst[2] = s[2];
}
static void fp_dyno_make_packet(Footprint& f, PhobDyno*, Edx, uint8_t* p) { f.add(p, 0x1e, "packet"); }
PORT_FN(0x00445940, "PhobDyno::MakeReplayPacket", PhobDyno_MakeReplayPacket, fp_dyno_make_packet)

// PhobDyno::ApplyExternalForce(force, point, surface) (0x445970, virtual): ApplyForce
static void __fastcall PhobDyno_ApplyExternalForce(PhobDyno* self, Edx, const P3* F, const P3* P, int) {
    PhobDynoApplyForce(self, 0, F, P);
}
static void fp_dyno_external_force(Footprint& f, PhobDyno* self, Edx, const P3*, const P3*, int) {
    f.add(self, sizeof(PhobDyno), "dyno");
    f.pure = true;
}
PORT_FN(0x00445970, "PhobDyno::ApplyExternalForce", PhobDyno_ApplyExternalForce, fp_dyno_external_force)

// Corner::Corner (0x445990): 1000 N/m, at the origin, not yet updated
static Corner* __fastcall Corner_ctor(Corner* self, Edx) {
    set_bits(&self->stiffness, 0x447a0000);                 // 1000.0f
    self->first_update = 1;
    set_bits(&self->local_pos.x, 0);
    set_bits(&self->local_pos.y, 0);
    set_bits(&self->local_pos.z, 0);
    set_bits(&self->prev_depth, 0);
    return self;
}
static void fp_corner_ctor(Footprint& f, Corner* self, Edx) {
    f.add(self, sizeof(Corner), "this");               // (not fuzzed: it returns this)
}
PORT_FN(0x00445990, "Corner::Corner", Corner_ctor, fp_corner_ctor)

// PhobDyno's scalar deleting destructor (0x4459b0)
static void* __fastcall PhobDyno_scalar_deleting_dtor(PhobRoot* self, Edx, unsigned flags) {
    PhobRootDtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
PORT_FN(0x004459b0, "PhobDyno::`scalar deleting destructor'", PhobDyno_scalar_deleting_dtor, fp_deleting_dtor)

// PhobDyno::ExternalImpulse::ExternalImpulse (0x4459d0): nothing to set
static ExternalImpulse* __fastcall ExternalImpulse_ctor(ExternalImpulse* self, Edx) { return self; }
static void fp_ext_ctor(Footprint&, ExternalImpulse*, Edx) {}      // (not fuzzed: it returns this)
PORT_FN(0x004459d0, "PhobDyno::ExternalImpulse::ExternalImpulse", ExternalImpulse_ctor, fp_ext_ctor)

// =============================================================================================================
// Corner (corner.obj)
// =============================================================================================================

// Corner::Update(dyno) (0x44d900): a ray from 10 m above where the corner's ray origin was last tick down
// to the corner's world position. On a hit, the depth d along the normal gives a spring force -N k d
// (k = stiffness, x0.04 on water), only 1% of it while the depth grows (prev < d), plus drag on water
// (-80 |v| v) or, elsewhere, a friction -3000 x the tangential velocity capped by 1.33 x the normal force
// (at most 2e7). Applied through ApplyExternalForce (vtable +0x34); off the water, a force over 50000 N
// makes a crash (Collide, with this corner as the "volume").
static void __fastcall Corner_Update(Corner* self, Edx, PhobDyno* dyno) {
    if (self->first_update) {
        const uint32_t* s = (const uint32_t*)&dyno->frame.pos;
        uint32_t* o = (uint32_t*)&self->ray_origin_world;
        o[0] = s[0];
        o[1] = s[1];
        uint32_t z = s[2];
        self->first_update = 0;
        o[2] = z;
    }
    P3 A;
    GetArmPosition(dyno, 0, &A, &self->local_pos);
    P3 B;
    movsd(&B, &self->ray_origin_world, 3);
    B.y = (float)(D(B.y) + 10.0f);
    P3 H, N;
    int surf;
    if (!TerrainGetIntersection(&B, &A, &H, &N, &surf)) {
        set_bits(&self->prev_depth, 0);
        GetArmPosition(dyno, 0, &self->ray_origin_world, &self->ray_origin_local);
        return;
    }
    float d = (float)-(((D(H.y) - A.y) * N.y + (D(H.z) - A.z) * N.z) + (D(H.x) - A.x) * N.x);
    float k;
    if (surf == SURFACE_WATER) k = (float)(D(self->stiffness) * 0.03999999910593033f);
    else set_bits(&k, bits(self->stiffness));
    P3 F;
    if (!(D(self->prev_depth) >= D(d))) {                   // fcomp; test ah,1: less, or a NaN
        F.x = (float)(((D(N.x) * k) * d) * -0.010000000707805157f);
        F.y = (float)(((D(N.y) * k) * d) * -0.010000000707805157f);
        F.z = (float)(((D(N.z) * k) * d) * -0.010000000707805157f);
    } else {
        F.x = (float)-((D(N.x) * k) * d);
        F.y = (float)-((D(N.y) * k) * d);
        F.z = (float)-((D(N.z) * k) * d);
    }
    P3 V;
    PhobDynoGetPointVelocity(dyno, 0, &V, &A);
    if (surf == SURFACE_WATER) {
        double s = x87_sqrt((D(V.y) * V.y + D(V.z) * V.z) + D(V.x) * V.x) * -80.0f;
        F.x = (float)(D(V.x) * s + F.x);
        F.y = (float)(D(V.y) * s + F.y);
        F.z = (float)(s * V.z + F.z);
    } else {
        double vn = -((D(N.y) * V.y + D(N.z) * V.z) + D(N.x) * V.x);
        float vnf = (float)vn;
        V.x = (float)(vn * N.x + V.x);
        V.y = (float)(D(N.y) * vnf + V.y);
        V.z = (float)(D(N.z) * vnf + V.z);
        float T[3];
        T[0] = (float)(D(V.x) * -3000.0f);
        T[1] = (float)(D(V.y) * -3000.0f);
        T[2] = (float)(D(V.z) * -3000.0f);
        float tm = (float)x87_sqrt((D(T[1]) * T[1] + D(T[2]) * T[2]) + D(T[0]) * T[0]);
        double fn = ((D(N.y) * F.y + D(N.z) * F.z) + D(N.x) * F.x) * 1.3299999237060547f;
        double lim = (D(20000000.0f) > fn) ? fn : D(20000000.0f);   // fcom st(1); test ah,0x41: a NaN gives 2e7
        float limf = (float)lim;
        if (!(lim >= D(tm))) {                              // fcom; test ah,1: less, or a NaN
            double sc = D(limf) / tm;
            T[0] = (float)(D(T[0]) * sc);
            T[1] = (float)(D(T[1]) * sc);
            T[2] = (float)(sc * T[2]);
        }
        F.x = (float)(D(T[0]) + F.x);
        F.y = (float)(D(T[1]) + F.y);
        F.z = (float)(D(T[2]) + F.z);
    }
    VFN(dyno, 0x34, void, const P3*, const P3*, int)(dyno, 0, &F, &A, surf);    // ApplyExternalForce
    set_bits(&self->prev_depth, bits(d));
    GetArmPosition(dyno, 0, &self->ray_origin_world, &self->ray_origin_local);
    if (surf != SURFACE_WATER) {
        double fm = x87_sqrt((D(F.y) * F.y + D(F.z) * F.z) + D(F.x) * F.x);
        if (fm > 50000.0f) CollideContact(0, (CollisionVolume*)self, &A, &F, &N);   // fcomp; test ah,0x41
    }
}
static void fp_corner_update(Footprint& f, Corner* self, Edx, PhobDyno* dyno) {
    f.add(self, sizeof(Corner), "corner");
    f.object(dyno, "dyno");
}
PORT_FN(0x0044d900, "Corner::Update", Corner_Update, fp_corner_update)

// =============================================================================================================
// Obstacle, Wobble, and the PhobRoot / PhobDyno / Corner members compiled into obstacle.obj
// =============================================================================================================

// Obstacle::Obstacle(PhobData*, void*) (0x43cd90): a PhobDyno, awake; the frame at data +0x18 turned by the
// yaw (data +0x24, degrees); a spring stiffness of mass x 16.35 (data +8); then by data +0x34: 0 a sphere,
// 1 a cube (radius data +0x2c / 2, colliding with the ground), 2 six corners on a box of data +0x28..0x30
// plus a sphere that doesn't touch the ground; the frame kept as the spawn frame
static Obstacle* __fastcall Obstacle_ctor(Obstacle* self, Edx, const uint8_t* data, void* arg) {
    PhobDynoCtor(self, 0, data, arg);
    self->vtable = VT_Obstacle;
    self->awake = 0;
    set_bits(&self->last_perturb_time, 0);
    ObstaclePerturb(self, 0);
    uint32_t* fr = (uint32_t*)&self->frame;
    fr[1] = 0; fr[0] = ONE; fr[2] = 0; fr[3] = 0; fr[4] = ONE; fr[5] = 0; fr[6] = 0; fr[7] = 0; fr[8] = ONE;
    fr[9] = 0; fr[10] = 0; fr[11] = 0;
    const uint32_t* dp = (const uint32_t*)(data + 0x18);
    fr[9] = dp[0];
    fr[10] = dp[1];
    fr[11] = dp[2];
    const float* df = (const float*)data;
    M3 Y, P;
    float yaw = (float)(D(df[9]) * 0.01745329238474369f);
    MatrixMakeYaw(&Y, bits(yaw));
    MatrixMakePitch(&P, 0);
    MatrixConcat(&Y, &P, &Y);
    MatrixMakeRoll(&P, 0);
    MatrixConcat(&Y, &P, &Y);
    MatrixConcat(&self->frame.rot, &Y, &self->frame.rot);
    float h = (float)(D(df[11]) * 0.5f);                    // data +0x2c: the radius
    float mp[3];                                            // the message point: (0, -h, 0), then 0
    set_bits(&mp[0], 0);
    mp[1] = (float)(D(df[11]) * -0.5f);
    set_bits(&mp[2], 0);
    movsd(message_point(self), mp, 3);
    set_bits(&mp[0], 0);
    set_bits(&mp[1], 0);
    set_bits(&mp[2], 0);
    movsd(message_point(self), mp, 3);
    float stiff = (float)(D(df[2]) * 16.350000381469727f);  // data +8
    float C[3];
    set_bits(&C[0], 0);
    set_bits(&C[1], 0);
    set_bits(&C[2], 0);
    float sx, sy, sz;                                       // data +0x28..0x30 (integer copies)
    set_bits(&sx, get_bits(data + 0x28));
    set_bits(&sy, get_bits(data + 0x2c));
    set_bits(&sz, get_bits(data + 0x30));
    int type = *(const int32_t*)(data + 0x34);
    if (type == 0 || type == 1) {
        void* p = MemAlloc(type == 0 ? 0x40 : 0x44);
        CollisionVolume* v = 0;
        if (p)
            v = (type == 0 ? SphereVolumeCtor : CubeVolumeCtor)(p, 0, &self->frame, bits(stiff), 0x3f000000, (P3*)C,
                                                                bits(h), self);
        self->volumes[self->num_volumes] = v;
        self->collide_ground = 1;
        self->num_volumes++;
    } else if (type == 2) {
        double nsx = D(sx) * -0.5f;                         // kept in a register, stored twice
        float nsxf = (float)nsx;
        float nsy = (float)(D(sy) * -0.5f);
        float nsz = (float)(D(sz) * -0.5f);
        float hsx = (float)(D(sx) * 0.5f);
        float hsz = (float)(D(sz) * 0.5f);
        float hsy = (float)(D(sy) * 0.5f);
        float zero3[3], lp[3];
        set_bits(&zero3[0], 0);
        set_bits(&zero3[1], 0);
        set_bits(&zero3[2], 0);
        // corners 0, 1 and 3 are written inline, 2, 4 and 5 through Corner::Setup, as in the original
        struct { const float* x; const float* y; const float* z; bool inl; } k[6] = {
            {&nsxf, &nsy, &nsz, true},  {&hsx, &nsy, &nsz, true}, {&nsxf, &nsy, &hsz, false},
            {&hsx, &nsy, &hsz, true},   {0, &hsy, &nsz, false},   {0, &hsy, &hsz, false}};
        float nsx2 = (float)nsx;                            // fstp of the same register
        k[2].x = &nsx2;
        for (int i = 0; i < 6; i++) {
            set_bits(&lp[0], k[i].x ? bits(*k[i].x) : 0);
            set_bits(&lp[1], bits(*k[i].y));
            set_bits(&lp[2], bits(*k[i].z));
            int n = self->num_corners;
            self->num_corners = n + 1;
            Corner* c = &self->corners[n];
            if (k[i].inl) {
                uint32_t* cw = (uint32_t*)c;
                cw[0] = bits(lp[0]);
                cw[1] = bits(lp[1]);
                cw[2] = bits(lp[2]);
                cw[6] = bits(stiff);
                cw[3] = 0;
                cw[4] = 0;
                cw[5] = 0;
                cw[7] = 0;
                cw[8] = 0;
                cw[9] = 0;
                cw[10] = 0;
            } else {
                CornerSetup(c, 0, (const P3*)lp, bits(stiff), (const P3*)zero3);
            }
        }
        void* p = MemAlloc(0x40);
        CollisionVolume* v = 0;
        if (p) v = SphereVolumeCtor(p, 0, &self->frame, bits(stiff), 0x3f000000, (P3*)C, bits(h), self);
        self->volumes[self->num_volumes] = v;
        self->collide_ground = 0;
        self->num_volumes++;
    }
    movsd(&self->spawn_frame, &self->frame, 12);
    return self;
}
static void fp_obstacle_ctor(Footprint& f, Obstacle*, Edx, const uint8_t*, void*) {
    f.replay_only = "allocates its volume (MemAlloc)";
}
PORT_FN(0x0043cd90, "Obstacle::Obstacle", Obstacle_ctor, fp_obstacle_ctor)

// (Obstacle::Update 0x43d2e0 is rewritten in viperport.cpp)

// Obstacle::ApplyExternalForce(force, point, surface) (0x43d390, virtual): a surface code of 100 or more (a
// car) wakes it; then PhobDyno's
static void __fastcall Obstacle_ApplyExternalForce(Obstacle* self, Edx, const P3* F, const P3* P, int surface) {
    if (surface >= 100) ObstaclePerturb(self, 0);
    PhobDynoApplyExternalForce(self, 0, F, P, surface);
}
static void fp_obstacle_external_force(Footprint& f, Obstacle* self, Edx, const P3*, const P3*, int) {
    f.object(self);
}
PORT_FN(0x0043d390, "Obstacle::ApplyExternalForce", Obstacle_ApplyExternalForce, fp_obstacle_external_force)

// Obstacle::Reset (0x43d3c0, virtual): back to the spawn frame, then PhobDyno's Reset
static void __fastcall Obstacle_Reset(Obstacle* self, Edx) {
    movsd(&self->frame, &self->spawn_frame, 12);
    PhobDynoReset(self, 0);
}
static void fp_obstacle_self(Footprint& f, Obstacle* self, Edx) { f.object(self); }
PORT_FN(0x0043d3c0, "Obstacle::Reset", Obstacle_Reset, fp_obstacle_self)

// Obstacle::Perturb (0x43d3e0): awake, from now
static void __fastcall Obstacle_Perturb(Obstacle* self, Edx) {
    self->awake = 1;
    self->last_perturb_time = (float)PhysicsGetTime();
}
PORT_FN(0x0043d3e0, "Obstacle::Perturb", Obstacle_Perturb, fp_obstacle_self)

// Wobble::Wobble(PhobData*, void*) (0x43d400): a PhobDyno standing in for a static object (data +0x18):
// its frame kept as the spawn frame, and its collision volume taken over (the volume's owner and frame
// pointed here)
static Wobble* __fastcall Wobble_ctor(Wobble* self, Edx, const uint8_t* data, void* arg) {
    PhobDynoCtor(self, 0, data, arg);
    self->vtable = VT_Wobble;
    set_bits(&self->_4ac, get_bits(data + 0x1c));
    uint8_t* so = PhysTaskFindStaticObject(*(const int32_t*)(data + 0x18));
    movsd(&self->spawn_frame, so, 12);
    CollisionVolume* v = *(CollisionVolume**)(so + 0x38);
    v->frame = &self->frame;
    v->owner = self;
    self->settle_timer = 0;
    movsd(&self->frame, &self->spawn_frame, 12);
    return self;
}
static void fp_wobble_ctor(Footprint& f, Wobble*, Edx, const uint8_t*, void*) {
    f.replay_only = "takes over a static object's volume (PhysTaskFindStaticObject)";
}
PORT_FN(0x0043d400, "Wobble::Wobble", Wobble_ctor, fp_wobble_ctor)

// Wobble::Reset (0x43d470, virtual)
static void __fastcall Wobble_Reset(Wobble* self, Edx) {
    movsd(&self->frame, &self->spawn_frame, 12);
    PhobDynoReset(self, 0);
    self->settle_timer = 0;
}
static void fp_wobble_self(Footprint& f, Wobble* self, Edx) { f.object(self); }
PORT_FN(0x0043d470, "Wobble::Reset", Wobble_Reset, fp_wobble_self)

// Wobble::Update (0x43d4a0, virtual): 20 steps of physics after a contact; the spin stopped at the last one
// and damped (x0.95) every step, no linear motion, the position pinned to the spawn point; and while the
// rotation's m[7]'s bits are below -0.0872's (unsigned: any positive m[7] too) m[7] is set to -0.0872 and
// the other rows rebuilt from it and normalised
static void __fastcall Wobble_Update(Wobble* self, Edx) {
    if (self->num_external_impulses > 0) self->settle_timer = 20;
    if (!(self->settle_timer > 0)) return;
    PhobDynoUpdate(self, 0);
    if (--self->settle_timer == 0) {
        set_bits(&self->angular_velocity.x, 0);
        set_bits(&self->angular_velocity.y, 0);
        set_bits(&self->angular_velocity.z, 0);
    }
    self->angular_velocity.x = (float)(D(self->angular_velocity.x) * 0.949999988079071f);
    self->angular_velocity.y = (float)(D(self->angular_velocity.y) * 0.949999988079071f);
    set_bits(&self->velocity.x, 0);
    set_bits(&self->velocity.y, 0);
    self->angular_velocity.z = (float)(D(self->angular_velocity.z) * 0.949999988079071f);
    set_bits(&self->velocity.z, 0);
    uint32_t m7 = get_bits(&self->frame.rot.m[7]);
    movsd(&self->frame.pos, &self->spawn_frame.pos, 3);
    if (m7 >= 0xbdb295eau) return;                          // cmp; jae
    float* m = self->frame.rot.m;
    set_bits(&m[7], 0xbdb295ea);                            // -0.0872
    m[0] = (float)(D(m[8]) * m[4] + D(m[5]) * 0.08720000088214874f);
    m[1] = (float)(D(m[5]) * m[6] - D(m[8]) * m[3]);
    m[2] = (float)(D(m[7]) * m[3] - D(m[4]) * m[6]);
    m[3] = (float)(D(m[2]) * m[7] - D(m[1]) * m[8]);
    m[4] = (float)(D(m[0]) * m[8] - D(m[2]) * m[6]);
    m[5] = (float)(D(m[1]) * m[6] - D(m[0]) * m[7]);
    double k = 1.0 / x87_sqrt((D(m[0]) * m[0] + D(m[2]) * m[2]) + D(m[1]) * m[1]);
    m[0] = (float)(D(m[0]) * k);
    m[1] = (float)(D(m[1]) * k);
    m[2] = (float)(k * m[2]);
    k = 1.0 / x87_sqrt((D(m[5]) * m[5] + D(m[4]) * m[4]) + D(m[3]) * m[3]);
    m[3] = (float)(D(m[3]) * k);
    m[4] = (float)(D(m[4]) * k);
    m[5] = (float)(k * m[5]);
    k = 1.0 / x87_sqrt((D(m[8]) * m[8] + D(m[6]) * m[6]) + D(m[7]) * m[7]);
    m[6] = (float)(D(m[6]) * k);
    m[7] = (float)(D(m[7]) * k);
    m[8] = (float)(k * m[8]);
}
static void fp_wobble_update(Footprint& f, Wobble* self, Edx) {
    f.object(self);
    fp_volumes(f, self);
    fp_splash(f);
}
PORT_FN(0x0043d4a0, "Wobble::Update", Wobble_Update, fp_wobble_update)

// (Wobble::ResolveExternalImpulse 0x43d6c0, PhobRoot::UpdateCommon 0x43d6d0, PhobDyno::ResolveExternalImpulse
// 0x43d850, Wobble::MakeReplayPacket 0x43d890 and Wobble::UpdateReplay 0x43d8a0 are bare returns: not rewritten)

// PhobRoot::GetMessageSize (0x43d6e0)
static int __fastcall PhobRoot_GetMessageSize(PhobRoot*, Edx) { return 0x34; }
PORT_FN(0x0043d6e0, "PhobRoot::GetMessageSize", PhobRoot_GetMessageSize, fp_root_getter)

// PhobRoot::GetMessage(buf) (0x43d6f0): at buf + the offset (+8): the tick count (16 bits), the frame (+4)
// and the message point in world space (+0x28, GetArmPosition's sum in its own grouping). With the message
// at the object itself the three rotated components are all computed before any is stored.
static void __fastcall PhobRoot_GetMessage(PhobRoot* self, Edx, uint8_t* buf) {
    uint8_t* p = (uint8_t*)(uintptr_t)(msg_offset(self) + (uint32_t)(uintptr_t)buf);
    movsd(p + 4, &self->frame, 12);
    const float* M = self->frame.rot.m;
    const float* in = (const float*)message_point(self);
    float* out = (float*)(p + 0x28);
    if (p == (uint8_t*)self) {
        double Y = (D(M[4]) * in[1] + D(M[1]) * in[0]) + D(M[7]) * in[2];
        double Z = (D(M[2]) * in[0] + D(M[8]) * in[2]) + D(M[5]) * in[1];
        double X = (D(M[6]) * in[2] + D(M[3]) * in[1]) + D(M[0]) * in[0];
        out[0] = (float)X;
        out[1] = (float)Y;
        out[2] = (float)Z;
    } else {
        out[0] = (float)((D(M[6]) * in[2] + D(M[3]) * in[1]) + D(M[0]) * in[0]);
        out[1] = (float)((D(M[4]) * in[1] + D(M[1]) * in[0]) + D(M[7]) * in[2]);
        out[2] = (float)((D(M[2]) * in[0] + D(M[8]) * in[2]) + D(M[5]) * in[1]);
    }
    out[0] = (float)(D(self->frame.pos.x) + out[0]);
    out[1] = (float)(D(self->frame.pos.y) + out[1]);
    out[2] = (float)(D(self->frame.pos.z) + out[2]);
    uint16_t tc = self->tick_count;
    memcpy(p, &tc, 2);
}
static void fp_root_get_message(Footprint& f, PhobRoot* self, Edx, uint8_t* buf) {
    f.add(buf + msg_offset(self), 0x34, "message");
}
PORT_FN(0x0043d6f0, "PhobRoot::GetMessage", PhobRoot_GetMessage, fp_root_get_message)

// PhobRoot::IsSolid (0x43d7e0)
static uint8_t __fastcall PhobRoot_IsSolid(PhobRoot*, Edx) { return 1; }
PORT_FN(0x0043d7e0, "PhobRoot::IsSolid", PhobRoot_IsSolid, fp_root_getter)

// Corner::Setup(local_pos, stiffness, ray_origin_local) (0x43d7f0): integer copies; no depth, the ray origin
// in the world at 0 (first_update is left as it is)
static void __fastcall Corner_Setup(Corner* self, Edx, const P3* lp, float stiffness, const P3* ro) {
    uint32_t* c = (uint32_t*)self;
    const uint32_t* a = (const uint32_t*)lp;
    const uint32_t* b = (const uint32_t*)ro;
    uint32_t s = bits(stiffness);
    c[0] = a[0];
    c[1] = a[1];
    c[2] = a[2];
    c[6] = s;
    c[3] = b[0];
    c[4] = b[1];
    c[5] = b[2];
    c[7] = 0;
    c[8] = 0;
    c[9] = 0;
    c[10] = 0;
}
static void fp_corner_setup(Footprint& f, Corner* self, Edx, const P3*, float, const P3*) {
    f.add(self, sizeof(Corner), "corner");
    f.pure = true;
}
PORT_FN(0x0043d7f0, "Corner::Setup", Corner_Setup, fp_corner_setup)

// PhobDyno::GetReplayPacketSize (0x43d840): PhobRoot's 0x12 and the velocity
static int __fastcall PhobDyno_GetReplayPacketSize(PhobRoot*, Edx) { return 0x1e; }
PORT_FN(0x0043d840, "PhobDyno::GetReplayPacketSize", PhobDyno_GetReplayPacketSize, fp_root_getter)

static void* __fastcall Obstacle_scalar_deleting_dtor(PhobRoot* self, Edx, unsigned flags) {
    PhobRootDtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
PORT_FN(0x0043d860, "Obstacle::`scalar deleting destructor'", Obstacle_scalar_deleting_dtor, fp_deleting_dtor)

// Wobble::GetReplayPacketSize (0x43d880): no packet
static int __fastcall Wobble_GetReplayPacketSize(PhobRoot*, Edx) { return 0; }
PORT_FN(0x0043d880, "Wobble::GetReplayPacketSize", Wobble_GetReplayPacketSize, fp_root_getter)

static void* __fastcall Wobble_vector_deleting_dtor(PhobRoot* self, Edx, unsigned flags) {
    PhobRootDtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
PORT_FN(0x0043d8b0, "Wobble::`vector deleting destructor'", Wobble_vector_deleting_dtor, fp_deleting_dtor)

// =============================================================================================================
// CheckPoint (checkpt.obj)
// =============================================================================================================

// CheckPoint::CheckPoint(PhobData*, void*) (0x440b50): the gate a (data +8) to b (data +0x14); the frame at
// its middle, row 0 along it, row 2 (-d.z, 0, d.x), row 1 = row2 x row0, each normalised; the two ends taken
// into the frame (MatrixMulPointInv, in place, the results unused); then registered with the deity (+0x34)
static CheckPoint* __fastcall CheckPoint_ctor(CheckPoint* self, Edx, const uint8_t* data, void* arg) {
    PhobRootCtor(self, 0, data, arg);
    self->vtable = VT_CheckPoint;
    movsd(&self->a, data + 8, 3);
    movsd(&self->b, data + 0x14, 3);
    float d[3];
    d[0] = (float)(D(self->b.x) - self->a.x);
    d[1] = (float)(D(self->b.y) - self->a.y);
    d[2] = (float)(D(self->b.z) - self->a.z);
    float mid[3];
    mid[0] = (float)(D(d[0]) * 0.5f + self->a.x);
    mid[1] = (float)(D(d[1]) * 0.5f + self->a.y);
    uint32_t* m = (uint32_t*)self->frame.rot.m;
    m[0] = bits(d[0]);
    m[1] = bits(d[1]);
    mid[2] = (float)(D(d[2]) * 0.5f + self->a.z);
    m[3] = 0;
    m[4] = ONE;
    m[5] = 0;
    m[2] = bits(d[2]);
    float* mf = self->frame.rot.m;
    mf[6] = (float)-D(mf[2]);
    m[8] = m[0];
    m[7] = 0;
    CrossProduct((P3*)&mf[3], (const P3*)&mf[6], (const P3*)&mf[0]);
    double k = 1.0 / x87_sqrt((D(mf[0]) * mf[0] + D(mf[1]) * mf[1]) + D(mf[2]) * mf[2]);
    mf[0] = (float)(D(mf[0]) * k);
    mf[1] = (float)(D(mf[1]) * k);
    mf[2] = (float)(k * mf[2]);
    k = 1.0 / x87_sqrt((D(mf[4]) * mf[4] + D(mf[5]) * mf[5]) + D(mf[3]) * mf[3]);
    mf[3] = (float)(D(mf[3]) * k);
    mf[4] = (float)(D(mf[4]) * k);
    mf[5] = (float)(k * mf[5]);
    k = 1.0 / x87_sqrt((D(mf[8]) * mf[8] + D(mf[6]) * mf[6]) + D(mf[7]) * mf[7]);
    mf[6] = (float)(D(mf[6]) * k);
    mf[7] = (float)(D(mf[7]) * k);
    mf[8] = (float)(k * mf[8]);
    double ax = self->a.x;                                  // loaded before the position is stored
    uint32_t* pos = (uint32_t*)&self->frame.pos;
    pos[0] = bits(mid[0]);
    pos[1] = bits(mid[1]);
    pos[2] = bits(mid[2]);
    P3 r;
    r.x = (float)(ax - self->frame.pos.x);
    r.y = (float)(D(self->a.y) - self->frame.pos.y);
    r.z = (float)(D(self->a.z) - self->frame.pos.z);
    MatrixMulPointInv(&r, &r, &self->frame.rot);
    P3 s;
    s.x = (float)(D(self->b.x) - self->frame.pos.x);
    s.y = (float)(D(self->b.y) - self->frame.pos.y);
    s.z = (float)(D(self->b.z) - self->frame.pos.z);
    MatrixMulPointInv(&s, &s, &self->frame.rot);
    void* deity = *g_deity;
    VFN(deity, 0x34, void, void*)(deity, 0, self);
    return self;
}
static void fp_checkpoint_ctor(Footprint& f, CheckPoint*, Edx, const uint8_t*, void*) {
    f.replay_only = "registers with the deity (its virtual +0x34)";
}
PORT_FN(0x00440b50, "CheckPoint::CheckPoint", CheckPoint_ctor, fp_checkpoint_ctor)

// CheckPoint::HasCrossed(p, q) (0x440db0): in x-z, did the move from p to q cross the gate a-b? q ahead of
// a and behind b along the gate, p on one side of it and q on the other (the flag tests' NaN results kept)
static uint8_t __fastcall CheckPoint_HasCrossed(CheckPoint* self, Edx, const P3* p, const P3* q) {
    float s0 = (float)(D(self->b.z) - self->a.z);
    float s20 = (float)(D(self->a.x) - self->b.x);
    float pax = (float)(D(p->x) - self->a.x);
    float paz = (float)(D(p->z) - self->a.z);
    float qax = (float)(D(q->x) - self->a.x);
    float qaz = (float)(D(q->z) - self->a.z);
    float s14 = (float)(D(self->b.x) - self->a.x);
    if (!(D(s0) * qaz + D(s14) * qax > 0.0f)) return 0;                    // test ah,0x41
    if ((D(q->x) - self->b.x) * s14 + (D(q->z) - self->b.z) * s0 >= 0.0f) return 0;   // test ah,1; je
    if (!(D(pax) * s0 + D(paz) * s20 > 0.0f)) return 0;
    if (D(s0) * qax + D(s20) * qaz >= 0.0f) return 0;
    return 1;
}
static void fp_has_crossed(Footprint& f, CheckPoint*, Edx, const P3*, const P3*) { f.pure = true; }
PORT_FN(0x00440db0, "CheckPoint::HasCrossed", CheckPoint_HasCrossed, fp_has_crossed)

static void* __fastcall CheckPoint_scalar_deleting_dtor(PhobRoot* self, Edx, unsigned flags) {
    PhobRootDtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
PORT_FN(0x00440ea0, "CheckPoint::`scalar deleting destructor'", CheckPoint_scalar_deleting_dtor, fp_deleting_dtor)

// =============================================================================================================
// Ball (ball.obj)
// =============================================================================================================

// GetBall(i) (0x4410e0): the ball table's slot (unchecked)
static Ball* __cdecl GetBall_rw(int i) { return g_balls[i]; }
static void fp_get_ball(Footprint&, int) {}
PORT_FN(0x004410e0, "GetBall", GetBall_rw, fp_get_ball)

// Ball::Ball(PhobData*, void*) (0x4410f0): a PhobDyno with one ground-colliding sphere -- radius data +0x24
// (inches) x 0.0254, spring data +0x28 x 175.197 x (data +0x2c)^2, friction data +0x30 -- at data +0x18,
// in ball slot data +0x34
static Ball* __fastcall Ball_ctor(Ball* self, Edx, const uint8_t* data, void* arg) {
    PhobDynoCtor(self, 0, data, arg);
    self->vtable = VT_Ball;
    const float* df = (const float*)data;
    float r = (float)(D(df[9]) * 0.02539999969303608f);
    double spring = (D(df[10]) * 175.19683837890625f) * (D(df[11]) * df[11]);
    uint32_t friction = get_bits(data + 0x30);
    float C[3];
    set_bits(&C[0], 0);
    set_bits(&C[1], 0);
    set_bits(&C[2], 0);
    float springf = (float)spring;
    set_bits(&self->index, get_bits(data + 0x34));
    const uint32_t* dp = (const uint32_t*)(data + 0x18);
    uint32_t* pos = (uint32_t*)&self->frame.pos;
    pos[0] = dp[0];
    pos[1] = dp[1];
    pos[2] = dp[2];
    void* p = MemAlloc(0x40);
    CollisionVolume* v = 0;
    if (p) v = SphereVolumeCtor(p, 0, &self->frame, bits(springf), friction, (P3*)C, bits(r), self);
    self->volumes[self->num_volumes] = v;
    int idx = self->index;
    self->num_volumes++;
    self->collide_ground = 1;
    set_bits(&self->last_throw_time, 0);
    register_ball(self, idx);
    return self;
}
static void fp_ball_ctor(Footprint& f, Ball*, Edx, const uint8_t*, void*) {
    f.replay_only = "allocates its SphereVolume (MemAlloc)";
}
PORT_FN(0x004410f0, "Ball::Ball", Ball_ctor, fp_ball_ctor)

// register_ball(ball, i) (0x4411e0)
static void __cdecl register_ball_rw(Ball* b, int i) { g_balls[i] = b; }
static void fp_register_ball(Footprint& f, Ball*, int i) { f.add(&g_balls[i], 4, "ball table"); }
PORT_FN(0x004411e0, "register_ball", register_ball_rw, fp_register_ball)

// Ball::~Ball (0x4411f0): out of the ball table, then ~PhobRoot
static void __fastcall Ball_dtor(Ball* self, Edx) {
    self->vtable = VT_Ball;
    unregister_ball(self);
    PhobRootDtor(self, 0);
}
static void fp_ball_dtor(Footprint& f, Ball*, Edx) { f.replay_only = "deletes its volumes"; }
PORT_FN(0x004411f0, "Ball::~Ball", Ball_dtor, fp_ball_dtor)

// unregister_ball(ball) (0x441210): its first slot in the table cleared
static void __cdecl unregister_ball_rw(Ball* b) {
    for (int i = 0; i < 16; i++)
        if (g_balls[i] == b) {
            g_balls[i] = 0;
            return;
        }
}
static void fp_unregister_ball(Footprint& f, Ball*) { f.add(g_balls, 64, "ball table"); }
PORT_FN(0x00441210, "unregister_ball", unregister_ball_rw, fp_unregister_ball)

// Ball::Update (0x441240, virtual): fallen below y = -2000 (the height's bits compared unsigned, so a -NaN
// too): held there, still; then PhobDyno's Update
static void __fastcall Ball_Update(Ball* self, Edx) {
    if (get_bits(&self->frame.pos.y) > 0xc4fa0000u) {
        set_bits(&self->frame.pos.y, 0xc4fa0000);           // -2000
        set_bits(&self->velocity.x, 0);
        set_bits(&self->velocity.y, 0);
        set_bits(&self->velocity.z, 0);
    }
    PhobDynoUpdate(self, 0);
}
static void fp_ball_update(Footprint& f, Ball* self, Edx) {
    f.object(self);
    fp_volumes(f, self);
    fp_splash(f);
}
PORT_FN(0x00441240, "Ball::Update", Ball_Update, fp_ball_update)

// Ball::Reset (0x441270, virtual): PhobDyno's, then parked at (10 x its slot, -500, 0)
static void __fastcall Ball_Reset(Ball* self, Edx) {
    PhobDynoReset(self, 0);
    set_bits(&self->frame.pos.y, 0xc3fa0000);               // -500
    set_bits(&self->frame.pos.z, 0);
    self->frame.pos.x = (float)(D(self->index) * 10.0f);
}
static void fp_ball_self(Footprint& f, Ball* self, Edx) { f.object(self); }
PORT_FN(0x00441270, "Ball::Reset", Ball_Reset, fp_ball_self)

// Ball::Throw(frame, velocity, time) (0x4412a0): at most every 2 s, and only within 0.1 s after `time`: the
// ball put at the thrower's frame and sent forward -- for the plane with the plane's velocity plus 31.1 m/s
// (and dropped 1 m along its up axis), for anything else at |velocity| + 31.1 m/s from 3.5 m + 31.1 m/s x
// the delay ahead, 0.5 m up. The spin is stopped whether or not it's thrown.
static void __fastcall Ball_Throw(Ball* self, Edx, const Frame* frame, const P3* vel, float time) {
    float now = (float)PhysicsGetTime();
    double early = D(now) - 2.0f;
    set_bits(&self->angular_velocity.x, 0);
    set_bits(&self->angular_velocity.y, 0);
    set_bits(&self->angular_velocity.z, 0);
    if (!(early > D(self->last_throw_time))) return;       // fcomp; test ah,0x41
    set_bits(&self->last_throw_time, bits(now));
    movsd(&self->frame, frame, 12);
    double dtr = D(now) - time;
    float dt = (float)dtr;
    if (!(dtr >= 0.0f)) return;                             // fcom 0; test ah,1
    if ((int32_t)bits(dt) > 0x3dcccccd) return;             // signed integer compare with 0.1f
    float* m = self->frame.rot.m;
    P3& pos = self->frame.pos;
    if (game_stricmp(GetCarFileName(HackGetCarIndex()), (const char*)0x004edb20) == 0) {   // "plane"
        movsd(&self->velocity, vel, 3);
        self->velocity.x = (float)(D(m[6]) * 31.11111068725586f + self->velocity.x);
        self->velocity.y = (float)(D(m[7]) * 31.11111068725586f + self->velocity.y);
        self->velocity.z = (float)(D(m[8]) * 31.11111068725586f + self->velocity.z);
        pos.x = (float)(D(pos.x) - m[3]);
        pos.y = (float)(D(pos.y) - m[4]);
        pos.z = (float)(D(pos.z) - m[5]);
        return;
    }
    double k = D(dt) * 31.11111068725586f + 3.5f;
    const float* fm = frame->rot.m;
    pos.x = (float)(D(fm[6]) * k + pos.x);
    pos.y = (float)(D(fm[7]) * k + pos.y);
    pos.z = (float)(k * fm[8] + pos.z);
    pos.y = (float)(D(pos.y) + 0.5f);
    double s = x87_sqrt((D(vel->z) * vel->z + D(vel->y) * vel->y) + D(vel->x) * vel->x) + 31.11111068725586f;
    self->velocity.x = (float)(D(m[6]) * s);
    self->velocity.y = (float)(D(m[7]) * s);
    self->velocity.z = (float)(s * m[8]);
}
static void fp_ball_throw(Footprint& f, Ball* self, Edx, const Frame*, const P3*, float) { f.object(self); }
PORT_FN(0x004412a0, "Ball::Throw", Ball_Throw, fp_ball_throw)

// Ball's scalar deleting destructor (0x441450): ~Ball, then operator delete when flag 1 is set
static void* __fastcall Ball_scalar_deleting_dtor(PhobRoot* self, Edx, unsigned flags) {
    BallDtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
PORT_FN(0x00441450, "Ball::`scalar deleting destructor'", Ball_scalar_deleting_dtor, fp_deleting_dtor)
