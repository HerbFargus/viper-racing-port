// phys_sphere.cpp -- M3 group A: the sphere collision volumes (physics:volume.obj), rewritten.
//
// A SphereVolume is a ball fixed to its owner's frame: Update caches its world centre; CollideGround pushes
// it out of the terrain with a spring (default_a, N/m of depth) plus capped friction (default_b), queues the
// contact impulse on the owner, and floats it on water (code 14) with drag, a slow bob and buoyancy. A
// CubeVolume is the same ball tested at the eight corners of a cube instead. Written from the v1.0
// disassembly: the same sums in the same grouping, register values as double, stored values as float, and
// every call made in the original's order with the same arguments (float arguments the original pushes with
// integer moves are passed as their bits).
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// ---- fields not yet named in phys_types.h (proposed there) ------------------------------------------------
static inline uint8_t& in_water(SphereVolume* s) { return s->_38; }             // +0x38 floating on water
static inline float& water_level(SphereVolume* s) { return *(float*)&s->_3c; } // +0x3c water surface y
static inline float& half_extent(CubeVolume* c) { return c->_40; }               // +0x40 sqrt(r*r/2)

// the unused edx of a __thiscall received as __fastcall. It stands in for the usual void*: test/fuzz.h can't
// size a void* argument (sizeof(void)), and an int is the same register either way.
typedef int Edx;

static inline uint32_t bits(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }

// ---- the game's functions these call ---------------------------------------------------------------------
typedef CollisionVolume*(__fastcall* CollisionVolumeCtor_t)(CollisionVolume*, void*, const Frame*, uint32_t,
                                                             uint32_t, uint32_t, PhobDyno*);
typedef SphereVolume*(__fastcall* SphereVolumeCtor_t)(SphereVolume*, void*, const Frame*, uint32_t, uint32_t,
                                                       const P3*, uint32_t, PhobDyno*);
typedef uint8_t(__cdecl* TerrainGetSphereIntersection_t)(P3*, uint32_t radius, P3* point, P3* normal, int* surface);
typedef void(__fastcall* GetPointVelocity_t)(PhobDyno*, void*, P3* out, const P3* point);
typedef void(__cdecl* CollideWater_t)(CollisionVolume*, P3* point, const P3* velocity);
typedef double(__cdecl* GetImpulseMagnitude_t)(const P3* pos, const P3* vel, uint32_t mass, const M3* inv_inertia,
                                               const P3* point, const P3* normal, uint32_t restitution);
typedef void(__fastcall* QueueExternalImpulse_t)(PhobDyno*, void*, const P3*, const P3*, int, float);
typedef void(__cdecl* Collide_t)(CollisionVolume*, CollisionVolume*, const P3* point, const P3* impulse,
                                 const P3* normal);
typedef void(__fastcall* PhobDynoApplyForce_t)(PhobDyno*, void*, const P3* force, const P3* point);
typedef double(__cdecl* PhysicsGetTime_t)();
typedef void(__cdecl* LogReport_t)(const char*, ...);
typedef uint8_t(__cdecl* CollidePair_t)(SphereVolume*, CollisionVolume*);
typedef uint8_t(__cdecl* CollideSphereSphere_t)(SphereVolume*, CollisionVolume*, CollisionVolume*, CollisionVolume*);

static const CollisionVolumeCtor_t CollisionVolumeCtor = (CollisionVolumeCtor_t)0x004322e0;
static const SphereVolumeCtor_t SphereVolumeCtor = (SphereVolumeCtor_t)0x00432340;
static const TerrainGetSphereIntersection_t TerrainGetSphereIntersection = (TerrainGetSphereIntersection_t)0x00465ca0;
static const GetPointVelocity_t GetPointVelocity = (GetPointVelocity_t)0x00444ee0;
static const CollideWater_t CollideWater = (CollideWater_t)0x0043c950;
static const GetImpulseMagnitude_t get_impulse_magnitude = (GetImpulseMagnitude_t)0x00435630;
static const QueueExternalImpulse_t QueueExternalImpulse = (QueueExternalImpulse_t)0x00444e60;
static const Collide_t CollideContact = (Collide_t)0x0043c710;          // ::Collide: crash sounds and events
static const PhobDynoApplyForce_t PhobDynoApplyForce = (PhobDynoApplyForce_t)0x00444c20;
static const PhysicsGetTime_t PhysicsGetTime = (PhysicsGetTime_t)0x0042bc80;
static const LogReport_t LogReport = (LogReport_t)0x00411150;
static const CollidePair_t collide_sphere_box = (CollidePair_t)0x00435890;
static const CollideSphereSphere_t collide_sphere_sphere = (CollideSphereSphere_t)0x00433cc0;
static const CollidePair_t collide_sphere_tube = (CollidePair_t)0x00434580;

enum : uint32_t { TAG_SPHR = 0x53504852, TAG_BOX = 0x424f5820, TAG_TUBE = 0x54554245, TAG_GRUP = 0x47525550 };
enum { SURFACE_WATER = 14 };
static void** const VT_CollisionVolume = (void**)0x004dbcf8;
static void** const VT_SphereVolume = (void**)0x004dbd18;
static void** const VT_CubeVolume = (void**)0x004dbd38;

// the splash Sound3D CollideWater updates (+8 volume, +0x29..+0x2c flags), through a volume.obj global
static void fp_splash(Footprint& f) {
    if (uint8_t* snd = *(uint8_t**)0x00521f7c) f.add(snd + 8, 0x25, "splash Sound3D");
}

// ---- CollisionVolume::CollisionVolume (0x4322e0) ------------------------------------------------------------
// every argument is stored with integer moves; the surface type starts at 100
static CollisionVolume* __fastcall CollisionVolume_ctor(CollisionVolume* self, Edx, const Frame* frame,
                                                        uint32_t tag, float a, float b, PhobDyno* owner) {
    self->vtable = VT_CollisionVolume;
    self->frame = frame;
    self->owner = owner;
    self->type_tag = tag;
    memcpy(&self->default_a, &a, 4);
    self->surface_type = 100;
    memcpy(&self->default_b, &b, 4);
    return self;
}
static void fp_cv_ctor(Footprint& f, CollisionVolume* self, Edx, const Frame*, uint32_t, float, float, PhobDyno*) {
    f.add(self, sizeof(CollisionVolume), "this");
}
PORT_FN(0x004322e0, "CollisionVolume::CollisionVolume", CollisionVolume_ctor, fp_cv_ctor)

// ---- CollisionVolume::ApplyForce (0x432320): passes the force to the owner's own ApplyForce (vtable +0x34) --
static void __fastcall CollisionVolume_ApplyForce(CollisionVolume* self, Edx, const P3* force, const P3* point,
                                                  int surface) {
    if (PhobRoot* owner = self->owner) VFN(owner, 0x34, void, const P3*, const P3*, int)(owner, 0, force, point, surface);
}
static void fp_cv_apply_force(Footprint& f, CollisionVolume* self, Edx, const P3*, const P3*, int) {
    if (self->owner) f.object(self->owner, "owner");
}
PORT_FN(0x00432320, "CollisionVolume::ApplyForce", CollisionVolume_ApplyForce, fp_cv_apply_force)

// ---- CollisionVolume::SetSurfaceType (0x436090) --------------------------------------------------------------
// (the other base-class stubs -- Update 0x436050, CollideGround 0x436060, Reset 0x436070 -- are a bare `ret`,
// and GetExtents 0x436080 is the M1 broad-phase's marker; none is rewritten)
static void __fastcall CollisionVolume_SetSurfaceType(CollisionVolume* self, Edx, int surface) {
    self->surface_type = surface;
}
static void fp_cv_set_surface(Footprint& f, CollisionVolume* self, Edx, int) {
    f.add(&self->surface_type, 4, "surface_type");
    f.pure = true;
}
PORT_FN(0x00436090, "CollisionVolume::SetSurfaceType", CollisionVolume_SetSurfaceType, fp_cv_set_surface)

// ---- SphereVolume::SphereVolume (0x432340): a 'SPHR' of radius r centred at `center` in the owner's frame --
static SphereVolume* __fastcall SphereVolume_ctor(SphereVolume* self, Edx, const Frame* frame, float a, float b,
                                                  const P3* center, float radius, PhobDyno* owner) {
    CollisionVolumeCtor(self, 0, frame, TAG_SPHR, bits(a), bits(b), owner);
    self->vtable = VT_SphereVolume;
    const uint32_t* c = (const uint32_t*)center;           // integer moves, in the original's order
    uint32_t* lc = (uint32_t*)&self->local_center;
    lc[0] = c[0];
    lc[1] = c[1];
    lc[2] = c[2];
    memset(&self->world_center, 0, 12);
    memcpy(&self->radius, &radius, 4);
    self->_38 = 0;
    self->_3c = 0;
    return self;
}
static void fp_sv_ctor(Footprint& f, SphereVolume* self, Edx, const Frame*, float, float, const P3*, float,
                       PhobDyno*) {
    f.add(self, sizeof(SphereVolume), "this");
}
PORT_FN(0x00432340, "SphereVolume::SphereVolume", SphereVolume_ctor, fp_sv_ctor)

// ---- SphereVolume::SphereVolume(Frame const*) (0x4323a0): defaults 1.0 / 0.6, no owner; the radius and the
// local centre are left unset
static SphereVolume* __fastcall SphereVolume_ctor_frame(SphereVolume* self, Edx, const Frame* frame) {
    CollisionVolumeCtor(self, 0, frame, TAG_SPHR, 0x3f800000 /* 1.0f */, 0x3f19999a /* 0.6f */, 0);
    self->vtable = VT_SphereVolume;
    memset(&self->world_center, 0, 12);
    self->_38 = 0;
    self->_3c = 0;
    return self;
}
static void fp_sv_ctor_frame(Footprint& f, SphereVolume* self, Edx, const Frame*) {
    f.add(self, sizeof(SphereVolume), "this");
}
PORT_FN(0x004323a0, "SphereVolume::SphereVolume(frame)", SphereVolume_ctor_frame, fp_sv_ctor_frame)

// ---- SphereVolume::Update (0x4323e0): world_center = the local centre rotated by the frame (p x M, as
// MatrixMulPoint) plus frame.pos; the rotated centre is stored first, then the position added to the floats
static void __fastcall SphereVolume_Update(SphereVolume* self, Edx) {
    const Frame* fr = self->frame;
    const float* M = fr->rot.m;
    const P3& l = self->local_center;
    P3& w = self->world_center;
    w.x = (float)((D(M[6]) * l.z + D(M[3]) * l.y) + D(M[0]) * l.x);
    w.y = (float)((D(M[4]) * l.y + D(M[1]) * l.x) + D(M[7]) * l.z);
    w.z = (float)((D(M[8]) * l.z + D(M[5]) * l.y) + D(M[2]) * l.x);
    w.x = fr->pos.x + w.x;
    w.y = fr->pos.y + w.y;
    w.z = fr->pos.z + w.z;
}
static void fp_sv_update(Footprint& f, SphereVolume* self, Edx) { f.object(self); }
PORT_FN(0x004323e0, "SphereVolume::Update", SphereVolume_Update, fp_sv_update)

// ---- SphereVolume::Reset (0x432450): off the water ----------------------------------------------------------
static void __fastcall SphereVolume_Reset(SphereVolume* self, Edx) { in_water(self) = 0; }
static void fp_sv_reset(Footprint& f, SphereVolume* self, Edx) {
    f.add(&in_water(self), 1, "in_water");
    f.pure = true;
}
PORT_FN(0x00432450, "SphereVolume::Reset", SphereVolume_Reset, fp_sv_reset)

// ---- SphereVolume::Collide (0x432460): dispatch on the other volume's FourCC; a group ('GRUP') is asked to
// collide itself with this sphere. The callees' results fall through in eax but Collide is void.
static void __fastcall SphereVolume_Collide(SphereVolume* self, Edx, CollisionVolume* other) {
    uint32_t tag = other->type_tag;
    if (tag == TAG_GRUP) VFN(other, 8, void, CollisionVolume*)(other, 0, self);
    else if (tag == TAG_BOX) collide_sphere_box(self, other);
    else if (tag == TAG_SPHR) collide_sphere_sphere(self, other, 0, 0);
    else if (tag == TAG_TUBE) collide_sphere_tube(self, other);
    else LogReport((const char*)0x004ed6a4, tag);          // "Unsupported collision type: Sphere-%x"
}
static void fp_sv_collide(Footprint& f, SphereVolume* self, Edx, CollisionVolume* other) {
    if (other->type_tag == TAG_GRUP) {
        f.replay_only = "a sphere group collides each of its member spheres, which aren't enumerated here";
        return;
    }
    f.object(self);
    f.object(other, "other");
    if (self->owner) f.object(self->owner, "owner");
    if (other->owner && other->owner != self->owner) f.object(other->owner, "other's owner");
}
PORT_FN(0x00432460, "SphereVolume::Collide", SphereVolume_Collide, fp_sv_collide)

// ---- the water, shared by the sphere's and the cube's CollideGround ---------------------------------------
// Landing on water (code 14): the first contact splashes (CollideWater, with the owner's velocity at the
// contact point) and marks the volume floating; every contact records the surface height (a bit copy).
static void water_contact(SphereVolume* self, PhobDyno* owner, P3* point) {
    if (!in_water(self)) {
        P3 v;
        GetPointVelocity(owner, 0, &v, point);
        CollideWater(self, point, &v);
        in_water(self) = 1;
    }
    memcpy(&water_level(self), &point->y, 4);
}

// fmod(a, 2pi) by the game's own CIfmod (0x4cf36a, through the CRT's intrinsic dispatcher, so its special
// cases are the game's), then fsin, then + 1.0f -- in one block, because fsin isn't rounded by the precision
// control: its full 64-bit result is what the add rounds, as in the original.
static const double k_two_pi = 6.2831854820251465;       // the qword constant: (double)(float)2pi
static const float k_one = 1.0f;
static __declspec(noinline) double wave_sin_plus_one(double a) {
    double r;
#ifdef VP_GCC
    __asm__ volatile("fld %[a]\n\t"
                     "fld %[k_two_pi]\n\t"
                     "mov eax, 0x004cf36a\n\t"
                     "call eax\n\t"
                     "fsin\n\t"
                     "fadd %[k_one]\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [a] "m"(a), [k_two_pi] "m"(k_two_pi), [k_one] "m"(k_one)
                     : VP_X87_CLOBBERS, "eax", "ecx", "edx", "cc", "memory");
#else
    __asm {
        fld     a
        fld     k_two_pi
        mov     eax, 0x004cf36a
        call    eax
        fsin
        fadd    k_one
        fstp    r
    }
#endif
    return r;
}

// While floating: depth h of the ball's bottom under the (bobbing) surface; if it's out, it's no longer
// floating. Otherwise drag against the centre's velocity, -(pi r^2)(10|v| + 500) v, and buoyancy up,
// min(h, 2r)(1000 + 0.1h)(4/3 pi r^3 * 0.4)(9.81), both applied at the centre. (Both originals compute
// |v|^2 as (y^2 + z^2) + x^2 or (z^2 + y^2) + x^2: the same bits.)
static void water_float(SphereVolume* self, PhobDyno* owner) {
    float h = (float)((D(water_level(self)) - self->world_center.y) + self->radius);
    P3 u;
    GetPointVelocity(owner, 0, &u, &self->world_center);
    double t = PhysicsGetTime();
    double a = D(t) * (float)3.14159274f + self->world_center.x;
    double hr = wave_sin_plus_one(a) * (float)0.0254f + h;
    h = (float)hr;
    if (!(hr > 0.0)) {                                      // fcom 0; test ah,0x41
        in_water(self) = 0;
        return;
    }
    double r2 = D(self->radius) * self->radius;
    float vol = (float)((D(self->radius) * r2) * (float)1.67551613f);
    double s = x87_sqrt((D(u.y) * u.y + D(u.z) * u.z) + D(u.x) * u.x);
    double c = s * 10.0f + 500.0f;
    double nk = -(c * (r2 * (float)3.14159274f));
    P3 drag;
    drag.x = (float)(D(u.x) * nk);
    drag.y = (float)(D(u.y) * nk);
    drag.z = (float)(nk * u.z);
    PhobDynoApplyForce(owner, 0, &drag, &self->world_center);
    double b = D(h) * (float)0.1f + 1000.0f;
    double two_r = D(self->radius) * 2.0f;
    double m = (D(h) > two_r) ? two_r : D(h);               // fcom; test ah,0x41: a NaN keeps h
    P3 lift;
    lift.x = 0.0f;
    lift.y = (float)(((m * b) * vol) * (float)9.81f);
    lift.z = 0.0f;
    PhobDynoApplyForce(owner, 0, &lift, &self->world_center);
}

// The contact response at `point` with depth d, the terrain normal n and the owner's velocity there v:
// spring force along n (a = default_a * d) plus friction -1000 (sphere) or -50 (cube) times the tangential
// velocity, capped at default_b * a. Written out in each caller: the two differ in which values are stored.

// ---- SphereVolume::CollideGround (0x4324d0) ---------------------------------------------------------------
static void __fastcall SphereVolume_CollideGround(SphereVolume* self, Edx) {
    PhobDyno* owner = (PhobDyno*)self->owner;
    P3 C;                                                   // the centre, copied (integer moves)
    memcpy(&C, &self->world_center, 12);
    P3 P, N;
    int surf;
    if (TerrainGetSphereIntersection(&C, bits(self->radius), &P, &N, &surf)) {
        if (surf == SURFACE_WATER) {
            water_contact(self, owner, &P);
        } else {
            in_water(self) = 0;
            float d = (float)(D(self->radius) -
                              (((D(C.z) - P.z) * N.z + (D(C.y) - P.y) * N.y) + (D(C.x) - P.x) * N.x));
            P3 V;
            GetPointVelocity(owner, 0, &V, &P);
            double A = D(self->default_a) * d;              // kept in a register
            double vn = (D(N.z) * V.z + D(N.y) * V.y) + D(N.x) * V.x;
            P3 F;
            F.x = (float)(D(N.x) * A);
            F.y = (float)(D(N.y) * A);
            F.z = (float)(D(N.z) * A);
            P3 T;
            T.x = (float)((D(V.x) - D(N.x) * vn) * -1000.0);    // fld 1000.0f; fchs
            T.y = (float)((D(V.y) - D(N.y) * vn) * -1000.0);
            T.z = (float)((D(V.z) - vn * N.z) * -1000.0);
            float mag = (float)x87_sqrt((D(T.y) * T.y + D(T.z) * T.z) + D(T.x) * T.x);
            double lim = A * self->default_b;
            float limf = (float)lim;
            // FIX: the depth d is never tested here: TerrainGetSphereIntersection hits when |s| < r, s the centre's
            // height over the terrain plane along N, and gives P = C - sN, so d = r - (C - P).N is r - s|N|^2 plus
            // the rounding of P -- a little below 0 when the ball barely touches (s within a few ulps of r, or |N|
            // a hair over 1). Then lim < 0, and with no tangential velocity (a ball at rest, or moving straight
            // along N: mag == 0) the original divided lim by 0: a zero-divide fault on the physics thread, which
            // unmasks it (ExceptDiv0Crashes(1)). No tangential velocity: no friction, T stays 0.
            if (!(lim >= mag) && !(VP_FIX && mag == 0.0f)) {   // fcom; test ah,1: less, or a NaN
                double k = D(limf) / mag;                   // from the stored limit
                T.x = (float)(D(T.x) * k);
                T.y = (float)(D(T.y) * k);
                T.z = (float)(k * T.z);
            }
            F.x = (float)(D(T.x) + F.x);
            F.y = (float)(D(T.y) + F.y);
            F.z = (float)(D(T.z) + F.z);
            VFN(self, 0x10, void, const P3*, const P3*, int)(self, 0, &F, &P, surf);
            double J = get_impulse_magnitude(&owner->frame.pos, &V, bits(owner->mass), &owner->inv_inertia_world,
                                             &P, &N, 0);
            float Jf = (float)J;
            if (J > 0.0) {                                  // fcom 0; test ah,0x41; jne skip
                P3 I;
                I.x = (float)(D(N.x) * Jf);
                I.y = (float)(D(N.y) * Jf);
                I.z = (float)(D(N.z) * Jf);
                float w = !(d >= (float)0.001f) ? (float)0.001f : d;      // fcom; test ah,1: a NaN depth gives 0.001
                QueueExternalImpulse(owner, 0, &I, &P, surf, w);
                CollideContact(self, 0, &P, &I, &N);
            }
        }
    }
    if (in_water(self)) water_float(self, owner);
}
static void fp_sv_collide_ground(Footprint& f, SphereVolume* self, Edx) {
    f.object(self);
    if (self->owner) f.object(self->owner, "owner");
    fp_splash(f);
}
PORT_FN(0x004324d0, "SphereVolume::CollideGround", SphereVolume_CollideGround, fp_sv_collide_ground)

// ---- SphereVolume::GetExtents (0x4328b0): the centre -/+ the radius, from copies (safe for any aliasing) --
static void __fastcall SphereVolume_GetExtents(SphereVolume* self, Edx, P3* mn, P3* mx) {
    float r[3], c[3];
    memcpy(&r[0], &self->radius, 4);
    memcpy(&r[1], &self->radius, 4);
    memcpy(&r[2], &self->radius, 4);
    memcpy(c, &self->world_center, 12);
    mn->x = c[0] - r[0];
    mn->y = c[1] - r[1];
    mn->z = c[2] - r[2];
    mx->x = r[0] + c[0];
    mx->y = r[1] + c[1];
    mx->z = r[2] + c[2];
}
static void fp_sv_get_extents(Footprint& f, SphereVolume*, Edx, P3* mn, P3* mx) {
    f.add(mn, 12, "min");
    f.add(mx, 12, "max");
    f.pure = true;
}
PORT_FN(0x004328b0, "SphereVolume::GetExtents", SphereVolume_GetExtents, fp_sv_get_extents)

// ---- CubeVolume::CubeVolume (0x432930): a sphere, plus the half-extent sqrt(r*r*0.5) ------------------------
static CubeVolume* __fastcall CubeVolume_ctor(CubeVolume* self, Edx, const Frame* frame, float a, float b,
                                              const P3* center, float radius, PhobDyno* owner) {
    SphereVolumeCtor(self, 0, frame, bits(a), bits(b), center, bits(radius), owner);
    self->vtable = VT_CubeVolume;
    half_extent(self) = (float)x87_sqrt((D(radius) * radius) * 0.5f);
    return self;
}
static void fp_cube_ctor(Footprint& f, CubeVolume* self, Edx, const Frame*, float, float, const P3*, float,
                         PhobDyno*) {
    f.add(self, sizeof(CubeVolume), "this");
}
PORT_FN(0x00432930, "CubeVolume::CubeVolume", CubeVolume_ctor, fp_cube_ctor)

// ---- CubeVolume::CollideGround (0x432980) ------------------------------------------------------------------
// One terrain test for the bounding sphere; then each corner of the cube (+-s on each axis, x fastest) whose
// depth along the normal is positive gets the contact response at its world position. The depth is measured
// with the corner in the cube's local axes (unrotated) against the contact point relative to the centre, as
// in the original; only the force's point of application is rotated. No crash-sound Collide() here.
static void __fastcall CubeVolume_CollideGround(CubeVolume* self, Edx) {
    PhobDyno* owner = (PhobDyno*)self->owner;
    P3 C;
    memcpy(&C, &self->world_center, 12);
    P3 P, N;
    int surf;
    if (TerrainGetSphereIntersection(&C, bits(self->radius), &P, &N, &surf)) {
        if (surf == SURFACE_WATER) {
            water_contact(self, owner, &P);
        } else {
            double sd = half_extent(self);
            float sp = (float)sd, sm = (float)-sd;
            in_water(self) = 0;
            float corner[8][3];
            for (int k = 0; k < 8; k++) {
                corner[k][0] = (k & 1) ? sp : sm;
                corner[k][1] = (k & 2) ? sp : sm;
                corner[k][2] = (k & 4) ? sp : sm;
            }
            P3 rel;                                         // the contact point from the centre
            rel.x = P.x - C.x;
            rel.y = P.y - C.y;
            rel.z = P.z - C.z;
            for (int k = 0; k < 8; k++) {
                const float* c = corner[k];
                double dd = ((D(c[1]) - rel.y) * N.y + (D(c[2]) - rel.z) * N.z) + (D(c[0]) - rel.x) * N.x;
                float d = (float)dd;
                if (!(dd > 0.0)) continue;                  // fcom 0; test ah,0x41; jne next
                const float* M = self->frame->rot.m;
                P3 O;       // the corner in world axes (the original's inlined o == p alias branch is dead here)
                O.x = (float)((D(M[6]) * c[2] + D(M[3]) * c[1]) + D(M[0]) * c[0]);
                O.y = (float)((D(M[4]) * c[1] + D(M[1]) * c[0]) + D(M[7]) * c[2]);
                O.z = (float)((D(M[8]) * c[2] + D(M[5]) * c[1]) + D(M[2]) * c[0]);
                P3 Q;
                Q.x = (float)(D(C.x) + O.x);
                Q.y = (float)(D(C.y) + O.y);
                Q.z = (float)(D(C.z) + O.z);
                double A = D(self->default_a) * d;
                float Af = (float)A;                        // fst: x uses the register, y and z the float
                P3 F;
                F.x = (float)(A * N.x);
                F.y = (float)(D(Af) * N.y);
                F.z = (float)(D(Af) * N.z);
                P3 V;
                GetPointVelocity(owner, 0, &V, &Q);
                double vn = (D(V.y) * N.y + D(V.z) * N.z) + D(V.x) * N.x;
                // 1000.0f * -0.05f, rounded to 24 bits, is exactly -50
                P3 T;
                T.x = (float)((D(V.x) - D(N.x) * vn) * -50.0);
                T.y = (float)((D(V.y) - D(N.y) * vn) * -50.0);
                T.z = (float)((D(V.z) - vn * N.z) * -50.0);
                float mag = (float)x87_sqrt((D(T.y) * T.y + D(T.z) * T.z) + D(T.x) * T.x);
                double lim = D(self->default_b) * Af;
                float limf = (float)lim;
                // (No fix needed for the zero divide below, unlike SphereVolume::CollideGround's: a corner gets here
                // only with dd > 0, so d >= 0 (a positive dd can't round below +0), Af = default_a * d >= 0 and lim
                // >= 0 for the volumes' non-negative default_a / default_b; then mag == 0 fails the test, lim >= 0.)
                if (!(lim >= mag)) {                        // fcom; test ah,1
                    double kk = D(limf) / mag;
                    T.x = (float)(D(T.x) * kk);
                    T.y = (float)(D(T.y) * kk);
                    T.z = (float)(kk * T.z);
                }
                F.x = (float)(D(F.x) + T.x);
                F.y = (float)(D(F.y) + T.y);
                F.z = (float)(D(F.z) + T.z);
                VFN(self, 0x10, void, const P3*, const P3*, int)(self, 0, &F, &Q, surf);
                double J = get_impulse_magnitude(&owner->frame.pos, &V, bits(owner->mass),
                                                 &owner->inv_inertia_world, &Q, &N, 0);
                float Jf = (float)J;
                if (!(J > 0.0)) continue;
                P3 I;
                I.x = (float)(D(Jf) * N.x);
                I.y = (float)(D(Jf) * N.y);
                I.z = (float)(D(Jf) * N.z);
                float w = !(d >= (float)0.001f) ? (float)0.001f : d;
                QueueExternalImpulse(owner, 0, &I, &Q, surf, w);
            }
        }
    }
    if (in_water(self)) water_float(self, owner);
}
static void fp_cube_collide_ground(Footprint& f, CubeVolume* self, Edx) {
    f.object(self);
    if (self->owner) f.object(self->owner, "owner");
    fp_splash(f);
}
PORT_FN(0x00432980, "CubeVolume::CollideGround", CubeVolume_CollideGround, fp_cube_collide_ground)
