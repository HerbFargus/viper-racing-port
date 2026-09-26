// phys_types.h -- the physics engine's own structures, as the v1.0 race.exe lays them out, for the M3
// rewrites. Layouts come from the recovered types (tools/recover_types.py, out/types.json) and each named
// field is checked by offset below. A field not yet understood is kept as padding (`_NNN`, by offset).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "x87.h"

// ---- geometry ----------------------------------------------------------------------------------------------
struct Frame { M3 rot; P3 pos; };                                     // rotation, then position

// ---- physics objects (only what the volumes use; phys_*.cpp add as they need) -----------------------------
struct PhobRoot {
    void** vtable;                     // +0
    uint32_t _004, _008, _00c;
    uint16_t tick_count;               // +0x10
    uint16_t _012;
    struct CollisionVolume* volumes[4];// +0x14
    int32_t num_volumes;               // +0x24
    uint32_t _028[4];                  // +0x28
    Frame frame;                       // +0x38
    uint8_t _068[4];                   // +0x68
};
static_assert(offsetof(PhobRoot, frame) == 0x38 && sizeof(PhobRoot) == 0x6c, "PhobRoot");

struct Corner { P3 local_pos, ray_origin_local; float stiffness, prev_depth; P3 ray_origin_world; uint8_t first_update, _2d[3]; };
static_assert(sizeof(Corner) == 48, "Corner");

struct ExternalImpulse { uint8_t raw[28]; };

struct PhobDyno : PhobRoot {
    Corner corners[8];                 // +0x6c
    int32_t num_corners;               // +0x1ec
    float mass;                        // +0x1f0
    P3 inertia;                        // +0x1f4
    float inv_mass;                    // +0x200
    P3 inv_inertia;                    // +0x204
    M3 inv_inertia_body;               // +0x210
    P3 velocity;                       // +0x234
    P3 angular_velocity;               // +0x240
    P3 force;                          // +0x24c
    P3 torque;                         // +0x258
    M3 inv_inertia_world;              // +0x264
    uint8_t _288[0x294 - 0x288];
    P3 impulse;                        // +0x294
    P3 angular_impulse;                // +0x2a0
    float impulse_weight;              // +0x2ac
    ExternalImpulse external_impulses[16];   // +0x2b0
    int32_t num_external_impulses;     // +0x470
    uint8_t collide_ground, _475[3];   // +0x474
};
static_assert(offsetof(PhobDyno, velocity) == 0x234 && offsetof(PhobDyno, impulse) == 0x294 &&
              offsetof(PhobDyno, num_external_impulses) == 0x470 && sizeof(PhobDyno) == 0x478, "PhobDyno");

// ---- collision volumes (volume.obj) -------------------------------------------------------------------------
// vtable slots, in bytes: 0 destructor, 4 Update, 8 Collide(other), 12 CollideGround, 16 ApplyForce,
// 20 Reset, 24 GetExtents(min, max), 28 SetSurfaceType
struct CollisionVolume {
    void** vtable;                     // +0
    PhobRoot* owner;                   // +4   (a PhobDyno for anything that moves)
    const Frame* frame;                // +8   the owner's frame
    float default_a;                   // +0xc  50000 (class default)
    float default_b;                   // +0x10 0.2
    uint32_t type_tag;                 // +0x14 FourCC: 'SPHR', 'BOX ', 'TUBE'
    int32_t surface_type;              // +0x18
};
static_assert(sizeof(CollisionVolume) == 28, "CollisionVolume");

struct SphereVolume : CollisionVolume {
    float radius;                      // +0x1c
    P3 local_center;                   // +0x20
    P3 world_center;                   // +0x2c  cached each Update
    uint8_t _38, _39[3];               // +0x38
    uint32_t _3c;                      // +0x3c
};
static_assert(offsetof(SphereVolume, world_center) == 0x2c && sizeof(SphereVolume) == 64, "SphereVolume");

struct CubeVolume : SphereVolume { float _40; };
static_assert(sizeof(CubeVolume) == 68, "CubeVolume");

struct MoveableSphereVolume : SphereVolume { uint8_t _40[140 - 64]; };
static_assert(sizeof(MoveableSphereVolume) == 140, "MoveableSphereVolume");

struct SphereGroupVolume : CollisionVolume { uint8_t _1c[84 - 28]; };
static_assert(sizeof(SphereGroupVolume) == 84, "SphereGroupVolume");

struct CylinderVolume : CollisionVolume {
    float radius;                      // +0x1c
    float half_length;                 // +0x20  along local z
};
static_assert(sizeof(CylinderVolume) == 36, "CylinderVolume");

struct TubeVolume : CylinderVolume {
    SphereVolume cap_pos;              // +0x24  centre (0,0,+h)
    SphereVolume cap_neg;              // +0x64  centre (0,0,-h)
};
static_assert(offsetof(TubeVolume, cap_neg) == 0x64 && sizeof(TubeVolume) == 164, "TubeVolume");

struct BoxVolume : CollisionVolume {
    P3 half_extent;                    // +0x1c
    float max_half_extent;             // +0x28
    int32_t mode;                      // +0x2c
};
static_assert(sizeof(BoxVolume) == 48, "BoxVolume");

// a virtual call through the object's own vtable (so a rewrite of the callee, hooked at its address, is
// what runs): VCALL(obj, byte_offset, return type, argument types...)(obj, 0, args...)
#define VFN(obj, off, R, ...) ((R(__fastcall*)(void*, void*, ##__VA_ARGS__))(((void**)((PhobRoot*)(obj))->vtable)[(off) / 4]))
