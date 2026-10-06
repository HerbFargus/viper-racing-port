// phys_spheregroup.cpp -- M3 3.2 group B: moveable spheres and sphere groups (physics:volume.obj), rewritten.
//
// A car's body is a SphereGroupVolume: up to 12 MoveableSphereVolumes, each a SphereVolume whose local
// centre can be pushed in (crushed) by ApplyInternalForce and is put back by Reset. The group collides as
// a whole against a quick bound (its frame position and a radius taking in every sphere), then sphere by
// sphere through the engine's collide_sphere_* functions.
//
// Each is written from its v1.0 disassembly. Float copies the original makes with integer moves (fields,
// the float arguments it passes straight on) stay bit copies here: a float copy through the FPU would
// quieten a signalling NaN. The float arguments passed through to the constructors and AddLink are handed
// on as their bits (the callee typedefs take uint32_t for them), as the original pushes them.
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// a float argument's bits, read as an integer from its stack slot (volatile: the compiler would otherwise
// load it through the FPU, as it does for a plain float copy)
static __forceinline uint32_t bits(const float& x) { return *(const volatile uint32_t*)&x; }
static __forceinline void set_bits(float* dst, uint32_t u) { *(volatile uint32_t*)dst = u; }

// ---- layouts (phys_types.h only pads these two; the same sizes, with the fields worked out below) -------
struct MoveableSphere;
struct SphereLink {                    // 8 bytes; written only by AddLink (nothing in volume.obj reads them)
    MoveableSphere* other;             // +0
    float value;                       // +4   ConnectSpheres' float
};
struct MoveableSphere : SphereVolume {
    int32_t num_links;                 // +0x40
    SphereLink links[4];               // +0x44
    P3 rest_center;                    // +0x64  the constructor's centre; Reset puts local_center back to it
    P3 compliance;                     // +0x70  per-axis scale from an internal force to a displacement
    uint32_t _7c;                      // +0x7c  (not touched by these functions)
    float crush_threshold;             // +0x80  displacements up to this length are elastic
    float crush_rate;                  // +0x84  how far past it the centre moves (times 0.016)
    uint8_t crushed, _89[3];           // +0x88  set by ApplyInternalForce, cleared by Reset
};
static_assert(offsetof(MoveableSphere, num_links) == 0x40 && offsetof(MoveableSphere, rest_center) == 0x64 &&
              offsetof(MoveableSphere, crush_threshold) == 0x80 && offsetof(MoveableSphere, crushed) == 0x88 &&
              sizeof(MoveableSphere) == sizeof(MoveableSphereVolume), "MoveableSphere");

struct SphereGroup : CollisionVolume {
    MoveableSphere* spheres[12];       // +0x1c
    int32_t num_spheres;               // +0x4c
    float bound;                       // +0x50  max over spheres of |centre| + radius (from the frame's origin)
};
static_assert(offsetof(SphereGroup, num_spheres) == 0x4c && offsetof(SphereGroup, bound) == 0x50 &&
              sizeof(SphereGroup) == sizeof(SphereGroupVolume), "SphereGroup");

// FourCC type tags ('GRUP' etc. as the compiler packs a multi-character constant)
enum : uint32_t { TAG_GRUP = 0x47525550, TAG_BOX = 0x424f5820, TAG_SPHR = 0x53504852, TAG_TUBE = 0x54554245 };

// vtables (v1.0)
#define VT_COLLISION_VOLUME ((void**)0x004dbcf8)
#define VT_MOVEABLE_SPHERE  ((void**)0x004dbd58)
#define VT_SPHERE_GROUP     ((void**)0x004dbd78)

// ---- the game functions these call (by address, so a rewrite hooked there is what runs) -----------------
typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* MatrixMulPointInv_t)(P3*, const P3*, const M3*);
typedef CollisionVolume*(__fastcall* CollisionVolumeCtor_t)(void*, void*, const Frame*, uint32_t tag,
                                                            uint32_t a, uint32_t b, PhobDyno*);
typedef SphereVolume*(__fastcall* SphereVolumeCtor_t)(void*, void*, const Frame*, uint32_t a, uint32_t b,
                                                      const P3* center, uint32_t radius, PhobDyno*);
typedef MoveableSphere*(__fastcall* MoveableSphereCtor_t)(void*, void*, const Frame*, uint32_t a, uint32_t b,
                                                          uint32_t threshold, uint32_t rate, const P3* compliance,
                                                          const P3* center, uint32_t radius, PhobDyno*);
typedef void(__fastcall* CollisionVolumeApplyForce_t)(void*, void*, const P3*, const P3*, int32_t);
typedef void(__fastcall* SphereVolumeReset_t)(void*, void*);
typedef void(__fastcall* AddLink_t)(void*, void*, MoveableSphere*, uint32_t value);
typedef uint8_t(__cdecl* CollideSphereSphere_t)(SphereVolume*, SphereVolume*, CollisionVolume*, CollisionVolume*);
typedef uint8_t(__cdecl* CollideSphereBox_t)(SphereVolume*, BoxVolume*);
typedef uint8_t(__cdecl* CollideSphereTube_t)(SphereVolume*, TubeVolume*);
#define MemAlloc                  ((MemAlloc_t)0x004140e0)
#define MatrixMulPointInv         ((MatrixMulPointInv_t)0x00436120)
#define CollisionVolume_ctor      ((CollisionVolumeCtor_t)0x004322e0)
#define SphereVolume_ctor         ((SphereVolumeCtor_t)0x00432340)
#define MoveableSphere_ctor       ((MoveableSphereCtor_t)0x00432fa0)
#define CollisionVolume_ApplyForce ((CollisionVolumeApplyForce_t)0x00432320)
#define SphereVolume_Reset        ((SphereVolumeReset_t)0x00432450)
#define MoveableSphere_AddLink    ((AddLink_t)0x00433170)
#define collide_sphere_sphere     ((CollideSphereSphere_t)0x00433cc0)
#define collide_sphere_box        ((CollideSphereBox_t)0x00435890)
#define collide_sphere_tube       ((CollideSphereTube_t)0x00434580)

// ---- footprint helpers ----------------------------------------------------------------------------------
// an owner once (a group's spheres all share the group's owner)
static void fp_owner(Footprint& f, PhobRoot* o, PhobRoot** seen, int& nseen) {
    if (!o) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == o) return;
    if (nseen < 32) seen[nseen++] = o;
    f.object(o, "owner");
}
// a group's spheres (and, for collisions, their owners)
static void fp_spheres(Footprint& f, SphereGroup* g, bool owners, PhobRoot** seen, int& nseen) {
    for (int i = 0; i < g->num_spheres && i < 12; i++) {
        MoveableSphere* s = g->spheres[i];
        if (!s) continue;
        f.object(s, "sphere");
        if (owners) fp_owner(f, s->owner, seen, nseen);
    }
}

// =========================================================================================================
// MoveableSphereVolume
// =========================================================================================================

// MoveableSphereVolume::MoveableSphereVolume (0x432fa0): a SphereVolume (frame, a, b, centre, radius, owner),
// then its own vtable, the crush parameters, the rest centre and compliance (integer copies), no links
static MoveableSphere* __fastcall MoveableSphereVolume_ctor(MoveableSphere* self, void*, const Frame* frame,
                                                            float default_a, float default_b,
                                                            float crush_threshold, float crush_rate,
                                                            const P3* compliance, const P3* center, float radius,
                                                            PhobDyno* owner) {
    SphereVolume_ctor(self, 0, frame, bits(default_a), bits(default_b), center, bits(radius), owner);
    self->vtable = VT_MOVEABLE_SPHERE;
    set_bits(&self->crush_threshold, bits(crush_threshold));
    set_bits(&self->crush_rate, bits(crush_rate));
    memcpy(&self->rest_center, center, 12);
    memcpy(&self->compliance, compliance, 12);
    self->num_links = 0;
    self->crushed = 0;
    return self;
}
static void fp_moveable_ctor(Footprint& f, MoveableSphere* self, void*, const Frame*, float, float, float, float,
                             const P3*, const P3*, float, PhobDyno*) {
    f.add(self, sizeof(MoveableSphere), "this");
}
PORT_FN(0x00432fa0, "MoveableSphereVolume::MoveableSphereVolume", MoveableSphereVolume_ctor, fp_moveable_ctor)

// MoveableSphereVolume::ApplyForce (0x433020): the force copied (integer moves) to a local, then the base
// CollisionVolume::ApplyForce, called directly (not through the vtable), on the copy
static void __fastcall MoveableSphereVolume_ApplyForce(MoveableSphere* self, void*, const P3* force, const P3* at,
                                                       int32_t surface) {
    P3 f;
    memcpy(&f, force, 12);
    CollisionVolume_ApplyForce(self, 0, &f, at, surface);
}
static void fp_moveable_apply_force(Footprint& f, MoveableSphere* self, void*, const P3*, const P3*, int32_t) {
    if (self->owner) f.object(self->owner, "owner");
}
PORT_FN(0x00433020, "MoveableSphereVolume::ApplyForce", MoveableSphereVolume_ApplyForce, fp_moveable_apply_force)

// MoveableSphereVolume::ApplyInternalForce (0x433060): s = force * compliance (per axis, stored). If s is
// longer than the crush threshold, the local centre moves along s (turned into the body frame by
// MatrixMulPointInv) by ((|s| - threshold) * rate * 0.016) / |s| of it, and the sphere is marked crushed.
static uint8_t __fastcall MoveableSphereVolume_ApplyInternalForce(MoveableSphere* self, void*, const P3* force) {
    P3 s;
    s.x = (float)(D(self->compliance.x) * force->x);
    s.y = (float)(D(force->y) * self->compliance.y);
    s.z = (float)(D(force->z) * self->compliance.z);
    float len2 = (float)((D(s.y) * s.y + D(s.z) * s.z) + D(s.x) * s.x);
    // fcomp; test ah,1; je: returns unless threshold^2 < len2 or unordered
    if (D(self->crush_threshold) * self->crush_threshold >= len2) return 0;
    P3 b;
    MatrixMulPointInv(&b, &s, &self->frame->rot);
    double len = x87_sqrt(len2);
    double k = (((len - self->crush_threshold) * self->crush_rate) * (float)0.016f) / len;   // float constant
    self->local_center.x = (float)(D(b.x) * k + self->local_center.x);
    self->local_center.y = (float)(D(b.y) * k + self->local_center.y);
    self->crushed = 1;
    self->local_center.z = (float)(k * b.z + self->local_center.z);
    return 1;
}
static void fp_moveable_internal_force(Footprint& f, MoveableSphere* self, void*, const P3*) {
    f.object(self, "this");
}
PORT_FN(0x00433060, "MoveableSphereVolume::ApplyInternalForce", MoveableSphereVolume_ApplyInternalForce,
        fp_moveable_internal_force)

// MoveableSphereVolume::Reset (0x433140): SphereVolume::Reset (directly), not crushed, centre back to rest
static void __fastcall MoveableSphereVolume_Reset(MoveableSphere* self, void*) {
    SphereVolume_Reset(self, 0);
    self->crushed = 0;
    memcpy(&self->local_center, &self->rest_center, 12);
}
static void fp_moveable_reset(Footprint& f, MoveableSphere* self, void*) { f.object(self, "this"); }
PORT_FN(0x00433140, "MoveableSphereVolume::Reset", MoveableSphereVolume_Reset, fp_moveable_reset)

// MoveableSphereVolume::AddLink (0x433170): appends (other, value); no bounds check, as in the original
static void __fastcall MoveableSphereVolume_AddLink(MoveableSphere* self, void*, MoveableSphere* other, float value) {
    self->links[self->num_links].other = other;
    set_bits(&self->links[self->num_links].value, bits(value));
    self->num_links++;
}
static void fp_moveable_add_link(Footprint& f, MoveableSphere* self, void*, MoveableSphere*, float) {
    f.object(self, "this");
}
PORT_FN(0x00433170, "MoveableSphereVolume::AddLink", MoveableSphereVolume_AddLink, fp_moveable_add_link)

// =========================================================================================================
// SphereGroupVolume
// =========================================================================================================

// SphereGroupVolume::SphereGroupVolume (0x433190): a CollisionVolume ('GRUP', defaults 0 and 0), no
// spheres, bound 0, surface type 0x69
static SphereGroup* __fastcall SphereGroupVolume_ctor(SphereGroup* self, void*, const Frame* frame, PhobDyno* owner) {
    CollisionVolume_ctor(self, 0, frame, TAG_GRUP, 0, 0, owner);
    self->vtable = VT_SPHERE_GROUP;
    memset(&self->bound, 0, 4);
    self->num_spheres = 0;
    self->surface_type = 0x69;
    return self;
}
static void fp_group_ctor(Footprint& f, SphereGroup* self, void*, const Frame*, PhobDyno*) {
    f.add(self, sizeof(SphereGroup), "this");
}
PORT_FN(0x00433190, "SphereGroupVolume::SphereGroupVolume", SphereGroupVolume_ctor, fp_group_ctor)

// SphereGroupVolume::~SphereGroupVolume (0x4331d0, the plain destructor): each sphere deleted through its
// own scalar deleting destructor (vtable slot 0, flag 1), then the CollisionVolume vtable (its destructor,
// inlined)
static void __fastcall SphereGroupVolume_dtor(SphereGroup* self, void*) {
    self->vtable = VT_SPHERE_GROUP;
    for (int i = 0; i < self->num_spheres; i++) {
        MoveableSphere* s = self->spheres[i];
        if (s) VFN(s, 0, void*, uint32_t)(s, 0, 1);
    }
    self->vtable = VT_COLLISION_VOLUME;
}
static void fp_group_dtor(Footprint& f, SphereGroup*, void*) {
    f.replay_only = "frees its spheres (their deleting destructors)";
}
PORT_FN(0x004331d0, "SphereGroupVolume::~SphereGroupVolume", SphereGroupVolume_dtor, fp_group_dtor)

// SphereGroupVolume::Update (0x433210): each sphere's Update (vtable +4)
static void __fastcall SphereGroupVolume_Update(SphereGroup* self, void*) {
    for (int i = 0; i < self->num_spheres; i++) {
        MoveableSphere* s = self->spheres[i];
        VFN(s, 4, void)(s, 0);
    }
}
static void fp_group_update(Footprint& f, SphereGroup* self, void*) {
    PhobRoot* seen[32]; int nseen = 0;
    fp_spheres(f, self, false, seen, nseen);
}
PORT_FN(0x00433210, "SphereGroupVolume::Update", SphereGroupVolume_Update, fp_group_update)

// SphereGroupVolume::GetExtents (0x433240): the frame's position -/+ the bound, from copies (integer
// moves) of both taken first
static void __fastcall SphereGroupVolume_GetExtents(SphereGroup* self, void*, P3* mn, P3* mx) {
    float b[3];
    memcpy(&b[0], &self->bound, 4);
    memcpy(&b[1], &self->bound, 4);
    memcpy(&b[2], &self->bound, 4);
    P3 p;
    memcpy(&p, &self->frame->pos, 12);
    mn->x = p.x - b[0];
    mn->y = p.y - b[1];
    mn->z = p.z - b[2];
    mx->x = b[0] + p.x;
    mx->y = b[1] + p.y;
    mx->z = b[2] + p.z;
}
static void fp_group_extents(Footprint& f, SphereGroup*, void*, P3* mn, P3* mx) {
    f.add(mn, 12, "min");
    f.add(mx, 12, "max");
}
PORT_FN(0x00433240, "SphereGroupVolume::GetExtents", SphereGroupVolume_GetExtents, fp_group_extents)

// SphereGroupVolume::AddSphere (0x4332c0): a new MoveableSphereVolume (MemAlloc(0x8c); the slot is 0 if that
// fails) on the group's frame and owner, given the group's surface type (vtable +0x1c); the bound grows to
// |centre| + radius if that's larger
static void __fastcall SphereGroupVolume_AddSphere(SphereGroup* self, void*, const P3* center, float radius,
                                                   float default_a, float default_b, float crush_threshold,
                                                   float crush_rate, const P3* compliance) {
    void* mem = MemAlloc(0x8c);
    MoveableSphere* s = 0;
    if (mem)
        s = MoveableSphere_ctor(mem, 0, self->frame, bits(default_a), bits(default_b), bits(crush_threshold),
                                bits(crush_rate), compliance, center, bits(radius), (PhobDyno*)self->owner);
    self->spheres[self->num_spheres] = s;
    s = self->spheres[self->num_spheres];
    VFN(s, 0x1c, void, int32_t)(s, 0, self->surface_type);
    double r = x87_sqrt((D(center->x) * center->x + D(center->z) * center->z) + D(center->y) * center->y) + radius;
    // fcom; test ah,0x41; jne: only when greater (ordered); compared unrounded, then stored
    if (r > self->bound) self->bound = (float)r;
    self->num_spheres++;
}
static void fp_group_add_sphere(Footprint& f, SphereGroup*, void*, const P3*, float, float, float, float, float,
                                const P3*) {
    f.replay_only = "allocates (MemAlloc) a MoveableSphereVolume";
}
PORT_FN(0x004332c0, "SphereGroupVolume::AddSphere", SphereGroupVolume_AddSphere, fp_group_add_sphere)

// SphereGroupVolume::ConnectSpheres (0x433370): links spheres i and j both ways (AddLink, directly)
static void __fastcall SphereGroupVolume_ConnectSpheres(SphereGroup* self, void*, int32_t i, int32_t j, float value) {
    MoveableSphere_AddLink(self->spheres[i], 0, self->spheres[j], bits(value));
    MoveableSphere_AddLink(self->spheres[j], 0, self->spheres[i], bits(value));
}
static void fp_group_connect(Footprint& f, SphereGroup* self, void*, int32_t i, int32_t j, float) {
    if ((uint32_t)i < 12 && self->spheres[i]) f.object(self->spheres[i], "sphere i");
    if ((uint32_t)j < 12 && j != i && self->spheres[j]) f.object(self->spheres[j], "sphere j");
}
PORT_FN(0x00433370, "SphereGroupVolume::ConnectSpheres", SphereGroupVolume_ConnectSpheres, fp_group_connect)

// SphereGroupVolume::Collide (0x4333b0), by the other volume's type tag:
//   GRUP: if the bounds overlap (sum of bounds squared > distance squared), every sphere against every one
//         of the other's, collide_sphere_sphere(mine, theirs, this, other)
//   BOX:  if the frame offset, along the box's x and z axes (rows 0 and 2 of its rotation), is within the
//         box's half extent plus the bound on both, each sphere against the box
//   SPHR: each sphere against it, collide_sphere_sphere(mine, it, 0, 0)
//   TUBE: each sphere against it
//   anything else: nothing
static void __fastcall SphereGroupVolume_Collide(SphereGroup* self, void*, CollisionVolume* other) {
    uint32_t tag = other->type_tag;
    if (tag == TAG_GRUP) {
        SphereGroup* og = (SphereGroup*)other;
        const P3& a = self->frame->pos;
        const P3& b = other->frame->pos;
        float dx = (float)(D(a.x) - b.x);
        float dy = (float)(D(a.y) - b.y);
        float dz = (float)(D(a.z) - b.z);
        double rs = D(self->bound) + og->bound;
        double d2 = (D(dy) * dy + D(dz) * dz) + D(dx) * dx;
        if (!(rs * rs > d2)) return;                        // fcompp; test ah,0x41; jne
        for (int i = 0; i < self->num_spheres; i++)
            for (int j = 0; j < og->num_spheres; j++)
                collide_sphere_sphere(self->spheres[i], og->spheres[j], self, other);
    } else if (tag == TAG_BOX) {
        BoxVolume* box = (BoxVolume*)other;
        const P3& a = self->frame->pos;
        const Frame* bf = other->frame;
        float dx = (float)(D(a.x) - bf->pos.x);
        float dy = (float)(D(a.y) - bf->pos.y);
        float dz = (float)(D(a.z) - bf->pos.z);
        const float* R = bf->rot.m;
        double ax = fabs((D(R[1]) * dy + D(R[2]) * dz) + D(R[0]) * dx);
        if (!(D(box->half_extent.x) + self->bound > ax)) return;   // fcompp; test ah,0x41; jne
        double az = fabs((D(R[6]) * dx + D(R[7]) * dy) + D(R[8]) * dz);
        if (!(D(box->half_extent.z) + self->bound > az)) return;
        for (int i = 0; i < self->num_spheres; i++)
            collide_sphere_box(self->spheres[i], box);
    } else if (tag == TAG_SPHR) {
        for (int i = 0; i < self->num_spheres; i++)
            collide_sphere_sphere(self->spheres[i], (SphereVolume*)other, 0, 0);
    } else if (tag == TAG_TUBE) {
        for (int i = 0; i < self->num_spheres; i++)
            collide_sphere_tube(self->spheres[i], (TubeVolume*)other);
    }
}
static void fp_group_collide(Footprint& f, SphereGroup* self, void*, CollisionVolume* other) {
    PhobRoot* seen[32]; int nseen = 0;
    f.object(self, "this");
    f.object(other, "other");
    fp_owner(f, self->owner, seen, nseen);
    fp_owner(f, other->owner, seen, nseen);
    fp_spheres(f, self, true, seen, nseen);
    if (other->type_tag == TAG_GRUP) fp_spheres(f, (SphereGroup*)other, true, seen, nseen);
}
PORT_FN(0x004333b0, "SphereGroupVolume::Collide", SphereGroupVolume_Collide, fp_group_collide)

// SphereGroupVolume::CollideGround (0x4335a0): each sphere's CollideGround (vtable +0xc)
static void __fastcall SphereGroupVolume_CollideGround(SphereGroup* self, void*) {
    for (int i = 0; i < self->num_spheres; i++) {
        MoveableSphere* s = self->spheres[i];
        VFN(s, 0xc, void)(s, 0);
    }
}
static void fp_group_collide_ground(Footprint& f, SphereGroup* self, void*) {
    PhobRoot* seen[32]; int nseen = 0;
    fp_owner(f, self->owner, seen, nseen);
    fp_spheres(f, self, true, seen, nseen);
}
PORT_FN(0x004335a0, "SphereGroupVolume::CollideGround", SphereGroupVolume_CollideGround, fp_group_collide_ground)

// SphereGroupVolume::Reset (0x4335d0): each sphere's Reset (vtable +0x14)
static void __fastcall SphereGroupVolume_Reset(SphereGroup* self, void*) {
    for (int i = 0; i < self->num_spheres; i++) {
        MoveableSphere* s = self->spheres[i];
        VFN(s, 0x14, void)(s, 0);
    }
}
static void fp_group_reset(Footprint& f, SphereGroup* self, void*) {
    PhobRoot* seen[32]; int nseen = 0;
    fp_spheres(f, self, false, seen, nseen);
}
PORT_FN(0x004335d0, "SphereGroupVolume::Reset", SphereGroupVolume_Reset, fp_group_reset)

// SphereGroupVolume::SetSurfaceType (0x4361c0): its own, then each sphere's (vtable +0x1c)
static void __fastcall SphereGroupVolume_SetSurfaceType(SphereGroup* self, void*, int32_t surface) {
    self->surface_type = surface;
    for (int i = 0; i < self->num_spheres; i++) {
        MoveableSphere* s = self->spheres[i];
        VFN(s, 0x1c, void, int32_t)(s, 0, surface);
    }
}
static void fp_group_set_surface(Footprint& f, SphereGroup* self, void*, int32_t) {
    PhobRoot* seen[32]; int nseen = 0;
    f.object(self, "this");
    fp_spheres(f, self, false, seen, nseen);
}
PORT_FN(0x004361c0, "SphereGroupVolume::SetSurfaceType", SphereGroupVolume_SetSurfaceType, fp_group_set_surface)
