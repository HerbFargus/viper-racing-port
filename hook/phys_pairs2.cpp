// phys_pairs2.cpp -- M3 3.2, group E: volume.obj's cylinder and box collision tests, rewritten.
//
// Each is written from its v1.0 disassembly: the same sums in the same grouping, the same float
// constants, values the original keeps in x87 registers as doubles and what it stores as floats.
// Divisions in the original go through the Pentium FDIV-bug workaround, so they're plain divisions here.
// The helpers they call (MatrixMulPoint, GetPointVelocity, get_impulse_magnitude, get_friction, the
// other pair tests...) are called by address, so whichever version is hooked there is what runs.
//
// collide_cylinder_box (0x435d70) is `xor al,al; ret` in v1.0 and isn't rewritten; collide_tube_box
// calls it by address.
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

static const float kEpsilon = 1.1920928955078125e-07f;   // 0x4dbcd4, FLT_EPSILON

// a float the original writes with a plain integer move (a constant, or a copy it pushes as an
// argument): its bits, so it is never loaded through the FPU
static __forceinline uint32_t bits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }

// fld / fstp dword: a value the original copies through the FPU (the clamps in collide_sphere_box)
static __forceinline float x87_float(double x) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("fld %[x]\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [x] "m"(x)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld x
            fstp r }
#endif
    return r;
}

// ---- the functions they call, by address ---------------------------------------------------------------
typedef void(__cdecl* MulPoint_t)(P3*, const P3*, const M3*);
static const MulPoint_t MatrixMulPoint = (MulPoint_t)0x00429420;          // o = p x M
static const MulPoint_t MatrixMulPointInv = (MulPoint_t)0x00436120;       // o = M p
// PhobDyno::GetPointVelocity(out, world point)
typedef void(__fastcall* GetPointVelocity_t)(PhobDyno*, void*, P3*, const P3*);
static const GetPointVelocity_t GetPointVelocity = (GetPointVelocity_t)0x00444ee0;
// PhobDyno::QueueExternalImpulse(impulse, point, surface, float) -- the float is pushed as bits
typedef void(__fastcall* QueueExternalImpulse_t)(PhobDyno*, void*, const P3*, const P3*, int, uint32_t);
static const QueueExternalImpulse_t QueueExternalImpulse = (QueueExternalImpulse_t)0x00444e60;
// Collide(a, b, point, impulse, normal): collide.obj's crash reporting
typedef void(__cdecl* Collide_t)(CollisionVolume*, CollisionVolume*, const P3*, const P3*, const P3*);
static const Collide_t Collide = (Collide_t)0x0043c710;
// get_impulse_magnitude(pos, velocity, mass, inverse inertia, point, normal, float): returns ST0
// unrounded, so double here; the two floats are pushed as bits
typedef double(__cdecl* ImpulseMagnitude_t)(const P3*, const P3*, uint32_t, const M3*, const P3*, const P3*, uint32_t);
static const ImpulseMagnitude_t get_impulse_magnitude = (ImpulseMagnitude_t)0x00435630;
// get_friction(out, point, normal, magnitude, a, b): the magnitude is pushed as bits
typedef void(__cdecl* Friction_t)(P3*, const P3*, const P3*, uint32_t, CollisionVolume*, CollisionVolume*);
static const Friction_t get_friction = (Friction_t)0x00435d80;
// ASSERT_MSG: a bare `ret` in the release build; kept, as the original calls it
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
static const AssertMsg_t ASSERT_MSG = (AssertMsg_t)0x00435d20;
// the other pair tests
typedef uint8_t(__cdecl* SphereCylinder_t)(SphereVolume*, CylinderVolume*);
typedef uint8_t(__cdecl* CylinderCylinder_t)(CylinderVolume*, CylinderVolume*);
typedef uint8_t(__cdecl* SphereBox_t)(SphereVolume*, BoxVolume*);
typedef uint8_t(__cdecl* CylinderBox_t)(CylinderVolume*, BoxVolume*);
static const SphereCylinder_t collide_sphere_cylinder = (SphereCylinder_t)0x00434690;
static const CylinderCylinder_t collide_cylinder_cylinder = (CylinderCylinder_t)0x00435070;
static const SphereBox_t collide_sphere_box = (SphereBox_t)0x00435890;
static const CylinderBox_t collide_cylinder_box = (CylinderBox_t)0x00435d70;

// CollisionVolume::ApplyForce (vtable +0x10)(force, point, the other volume's surface)
#define APPLY_FORCE(v, F, P, S) VFN(v, 0x10, void, const P3*, const P3*, int)(v, 0, F, P, S)

// a collision writes both volumes and both owners
static void fp_volume(Footprint& f, CollisionVolume* v, const char* what, const char* owner) {
    f.object(v, what);
    if (v->owner) f.object(v->owner, owner);
}

// ---- collide_sphere_cylinder (0x434690) ------------------------------------------------------------------
// The sphere's centre (from its own frame) in the cylinder's frame; a hit when it lies within the
// cylinder's length and the side surfaces overlap. The contact point is on the cylinder's side, the
// normal points out from its axis. A spring force (capped at 4450) always; an impulse when closing.
static uint8_t __cdecl collide_sphere_cylinder_rw(SphereVolume* s, CylinderVolume* c) {
    const Frame* sf = s->frame;
    P3 C;
    MatrixMulPoint(&C, &s->local_center, &sf->rot);
    C.x = sf->pos.x + C.x;
    C.y = sf->pos.y + C.y;
    C.z = sf->pos.z + C.z;
    const Frame* cf = c->frame;
    P3 p;                                                       // local to the cylinder
    p.x = C.x - cf->pos.x;
    p.y = C.y - cf->pos.y;
    p.z = C.z - cf->pos.z;
    MatrixMulPointInv(&p, &p, &cf->rot);
    if (!(c->half_length > p.z)) return 0;                      // test ah,0x41
    if (-D(c->half_length) >= p.z) return 0;                    // test ah,1; je
    double r = x87_sqrt((double)p.y * p.y + (double)p.x * p.x); // distance from the axis
    float rf = (float)r;                                        // fst; the test is on the register
    if (!(fabs(r) > kEpsilon)) return 0;
    double depth = (D(s->radius) - rf) + c->radius;
    float depthf = (float)depth;
    if (!(depth > 0.0f)) return 0;

    P3 swc, cpos, q;                                            // integer copies
    memcpy(&swc, &s->world_center, 12);
    cf = c->frame;
    memcpy(&cpos, &cf->pos, 12);
    memcpy(&q, &p, 12);
    // the contact point: p pulled in to the cylinder's radius
    double t = (D(c->radius) - depthf) / rf;
    q.x = (float)(D(p.x) * t);
    q.y = (float)(t * p.y);
    P3 W;
    MatrixMulPoint(&W, &q, &cf->rot);
    W.x = cf->pos.x + W.x;
    W.y = cf->pos.y + W.y;
    W.z = cf->pos.z + W.z;
    // the normal: p's direction off the axis, in the world
    P3 a;
    memcpy(&a, &p, 12);
    double k = 1.0f / x87_sqrt((double)p.y * p.y + (double)p.x * p.x);
    a.x = (float)(D(p.x) * k);
    a.y = (float)(k * p.y);
    a.z = 0.0f;
    cf = c->frame;
    P3 n;
    MatrixMulPoint(&n, &a, &cf->rot);

    PhobDyno* so = (PhobDyno*)s->owner;
    PhobDyno* co = (PhobDyno*)c->owner;
    P3 vs, vc;
    if (so) GetPointVelocity(so, 0, &vs, &W);
    else memset(&vs, 0, 12);
    if (co) GetPointVelocity(co, 0, &vc, &W);
    else memset(&vc, 0, 12);
    double m = ((D(c->default_a) + s->default_a) * depthf) * 0.5f;
    float mag = (float)m;
    if (m > 4450.0f) mag = 4450.0f;                             // fcom on the register; test ah,0x41
    P3 F;
    F.x = (float)(D(mag) * n.x);
    F.y = (float)(D(mag) * n.y);
    F.z = (float)(D(mag) * n.z);
    P3 dv, r1, r2;
    dv.x = vs.x - vc.x;
    dv.y = vs.y - vc.y;
    dv.z = vs.z - vc.z;
    r1.x = W.x - swc.x;
    r1.y = W.y - swc.y;
    r1.z = W.z - swc.z;
    r2.x = W.x - cpos.x;
    r2.y = W.y - cpos.y;
    r2.z = W.z - cpos.z;

    if (!(((D(n.y) * dv.y + D(n.z) * dv.z) + D(n.x) * dv.x) >= 0.0f)) {   // closing: an impulse
        double kn = 1.0f / x87_sqrt(((double)n.y * n.y + (double)n.z * n.z) + (double)n.x * n.x);
        n.x = (float)(D(n.x) * kn);
        n.y = (float)(D(n.y) * kn);
        n.z = (float)(kn * n.z);
        P3 a1, a2, w1, w2, b1, b2, sum;
        a1.x = (float)(D(r1.y) * n.z - D(n.y) * r1.z);          // r1 x n
        a1.y = (float)(D(r1.z) * n.x - D(n.z) * r1.x);
        a1.z = (float)(D(n.y) * r1.x - D(r1.y) * n.x);
        a2.x = (float)(D(r2.y) * n.z - D(r2.z) * n.y);          // r2 x n
        a2.y = (float)(D(r2.z) * n.x - D(r2.x) * n.z);
        a2.z = (float)(D(r2.x) * n.y - D(r2.y) * n.x);
        if (!so) memset(&w1, 0, 12);
        else {                                                  // inlined: a1 x the world inverse inertia
            const float* I = so->inv_inertia_world.m;
            w1.x = (float)((D(I[0]) * a1.x + D(I[6]) * a1.z) + D(I[3]) * a1.y);
            w1.y = (float)((D(I[4]) * a1.y + D(I[7]) * a1.z) + D(I[1]) * a1.x);
            w1.z = (float)((D(I[8]) * a1.z + D(I[2]) * a1.x) + D(I[5]) * a1.y);
        }
        b1.x = (float)(D(w1.y) * r1.z - D(w1.z) * r1.y);        // w1 x r1
        b1.y = (float)(D(w1.z) * r1.x - D(w1.x) * r1.z);
        b1.z = (float)(D(w1.x) * r1.y - D(w1.y) * r1.x);
        if (!co) memset(&w2, 0, 12);
        else MatrixMulPoint(&w2, &a2, &co->inv_inertia_world);
        b2.x = (float)(D(w2.y) * r2.z - D(w2.z) * r2.y);        // w2 x r2
        b2.y = (float)(D(w2.z) * r2.x - D(w2.x) * r2.z);
        b2.z = (float)(D(w2.x) * r2.y - D(w2.y) * r2.x);
        sum.x = b2.x + b1.x;
        sum.y = b2.y + b1.y;
        sum.z = b2.z + b1.z;
        float im1 = 0.0f, im2 = 0.0f;
        if (so) im1 = 1.0f / so->mass;
        if (co) im2 = 1.0f / co->mass;
        double denom = (((D(sum.z) * n.z + D(sum.y) * n.y) + D(sum.x) * n.x) + im2) + im1;
        double num = (D(n.y) * dv.y + D(n.z) * dv.z) + D(n.x) * dv.x;
        double j = -(num / denom);
        P3 J;
        J.x = (float)(D(n.x) * j);
        J.y = (float)(D(n.y) * j);
        J.z = (float)(j * n.z);
        if (so) QueueExternalImpulse(so, 0, &J, &W, c->surface_type, 0x3f800000u);
        J.x = -J.x;
        J.y = -J.y;
        J.z = -J.z;
        if (co) QueueExternalImpulse(co, 0, &J, &W, s->surface_type, 0x3f800000u);
        Collide(s, s, &W, &J, &n);                              // sic: the sphere twice
    }
    F.x = (float)(D(F.x) * 0.5f);
    F.y = (float)(D(F.y) * 0.5f);
    F.z = (float)(D(F.z) * 0.5f);
    APPLY_FORCE(s, &F, &W, c->surface_type);
    F.x = -F.x;
    F.y = -F.y;
    F.z = -F.z;
    APPLY_FORCE(c, &F, &W, s->surface_type);
    return 1;
}
static void fp_sphere_cylinder(Footprint& f, SphereVolume* s, CylinderVolume* c) {
    fp_volume(f, s, "sphere", "sphere owner");
    fp_volume(f, c, "cylinder", "cylinder owner");
}
PORT_FN(0x00434690, "collide_sphere_cylinder", collide_sphere_cylinder_rw, fp_sphere_cylinder)

// ---- collide_cylinder_cylinder (0x435070) ----------------------------------------------------------------
// The closest points of the two axes (s along A's, t along B's; the midpoint's projections when the
// axes are near parallel), each within its cylinder's length; a hit when they're closer than the radii
// sum. A spring force along the line between them, 50x stiffer when closing; no impulse.
static uint8_t __cdecl collide_cylinder_cylinder_rw(CylinderVolume* A, CylinderVolume* B) {
    P3 pa, pb, a, b;                                            // integer copies: positions, z axes
    memcpy(&pa, &A->frame->pos, 12);
    memcpy(&pb, &B->frame->pos, 12);
    memcpy(&a, &A->frame->rot.m[6], 12);
    memcpy(&b, &B->frame->rot.m[6], 12);
    P3 d;
    d.x = pb.x - pa.x;
    d.y = pb.y - pa.y;
    d.z = pb.z - pa.z;
    P3 c;                                                       // a x b
    c.x = (float)(D(b.z) * a.y - D(b.y) * a.z);
    c.y = (float)(D(b.x) * a.z - D(b.z) * a.x);
    c.z = (float)(D(b.y) * a.x - D(b.x) * a.y);
    float hA, hB;
    memcpy(&hA, &A->half_length, 4);
    memcpy(&hB, &B->half_length, 4);
    float sq = (float)((D(c.y) * c.y + D(c.z) * c.z) + D(c.x) * c.x);
    int32_t sq_bits;
    memcpy(&sq_bits, &sq, 4);
    float s, t;
    if (sq_bits < 0x3dcccccd) {                                 // cmp dword, 0.1f; jl: near parallel
        float mx = (float)((D(pb.x) + pa.x) * 0.5f);
        float my = (float)((D(pb.y) + pa.y) * 0.5f);
        double mz = (D(pb.z) + pa.z) * 0.5f;                    // fst: used from the register first
        float mzf = (float)mz;
        s = (float)(((mz - pa.z) * a.z + (D(my) - pa.y) * a.y) + (D(mx) - pa.x) * a.x);
        t = (float)(((D(mzf) - pb.z) * b.z + (D(my) - pb.y) * b.y) + (D(mx) - pb.x) * b.x);
    } else {
        if (!(fabs(D(sq)) > kEpsilon)) return 0;
        double X1 = D(d.z) * c.y - D(d.y) * c.z;                // kept in a register for both
        double X2 = D(d.y) * b.z - D(d.z) * b.y;
        double X3 = D(b.y) * c.z - D(b.z) * c.y;
        s = (float)(((X2 * c.x + X3 * d.x) + D(b.x) * X1) / sq);
        double Y1 = D(d.y) * a.z - D(d.z) * a.y;
        double Y2 = D(a.y) * c.z - D(a.z) * c.y;
        t = (float)(((Y1 * c.x + Y2 * d.x) + X1 * a.x) / sq);
    }
    if (-D(hA) > s) return 0;                                   // test ah,0x41; je
    if (D(s) > hA) return 0;
    if (-D(hB) > t) return 0;
    if (D(t) > hB) return 0;
    P3 qa, qb;                                                  // the closest points
    qa.x = (float)(D(a.x) * s + pa.x);
    qa.y = (float)(D(a.y) * s + pa.y);
    qa.z = (float)(D(a.z) * s + pa.z);
    qb.x = (float)(D(b.x) * t + pb.x);
    qb.y = (float)(D(b.y) * t + pb.y);
    qb.z = (float)(D(b.z) * t + pb.z);
    P3 dd;
    dd.x = qb.x - qa.x;
    dd.y = qb.y - qa.y;
    double dz = D(qb.z) - qa.z;                                 // fst, then squared from the register
    dd.z = (float)dz;
    double dist = x87_sqrt((dz * dd.z + D(dd.y) * dd.y) + D(dd.x) * dd.x);
    float distf = (float)dist;
    if (!(fabs(dist) > kEpsilon)) return 0;
    P3 n;
    n.x = dd.x / distf;
    n.y = dd.y / distf;
    n.z = dd.z / distf;
    double depth = (D(B->radius) + A->radius) - distf;
    float depthf = (float)depth;
    if (!(depth >= 0.0f)) return 0;                             // test ah,1; je

    PhobDyno* ao = (PhobDyno*)A->owner;
    PhobDyno* bo = (PhobDyno*)B->owner;
    P3 va, vb;
    if (ao) GetPointVelocity(ao, 0, &va, &qa);
    else memset(&va, 0, 12);
    if (bo) GetPointVelocity(bo, 0, &vb, &qb);
    else memset(&vb, 0, 12);
    double dot = ((D(vb.y) - va.y) * dd.y + (D(vb.z) - va.z) * dd.z) + (D(vb.x) - va.x) * dd.x;
    double k = (D(B->default_a) + A->default_a) * depthf;
    float mag = (float)(!(dot >= 0.0f) ? k * 50.0f : k * 0.5f);   // test ah,1: closing (or NaN)
    P3 F;
    F.x = (float)-(D(mag) * n.x);
    F.y = (float)-(D(mag) * n.y);
    F.z = (float)-(D(mag) * n.z);
    APPLY_FORCE(A, &F, &qa, B->surface_type);
    F.x = -F.x;
    F.y = -F.y;
    F.z = -F.z;
    APPLY_FORCE(B, &F, &qb, A->surface_type);
    P3 up;                                                      // integer writes
    memset(&up.x, 0, 4);
    uint32_t one = 0x3f800000u;
    memcpy(&up.y, &one, 4);
    memset(&up.z, 0, 4);
    Collide(A, B, &qa, &F, &up);
    return 1;
}
static void fp_cylinder_cylinder(Footprint& f, CylinderVolume* A, CylinderVolume* B) {
    fp_volume(f, A, "cylinder a", "cylinder a owner");
    fp_volume(f, B, "cylinder b", "cylinder b owner");
}
PORT_FN(0x00435070, "collide_cylinder_cylinder", collide_cylinder_cylinder_rw, fp_cylinder_cylinder)

// ---- collide_cylinder_tube (0x435020) --------------------------------------------------------------------
// the tube's cylinder against the cylinder (tube first); only if that misses, both end caps
static uint8_t __cdecl collide_cylinder_tube_rw(CylinderVolume* c, TubeVolume* t) {
    uint8_t hit = collide_cylinder_cylinder(t, c);
    if (hit) return 1;
    hit |= collide_sphere_cylinder(&t->cap_pos, c);
    hit |= collide_sphere_cylinder(&t->cap_neg, c);
    return hit;
}
static void fp_cylinder_tube(Footprint& f, CylinderVolume* c, TubeVolume* t) {
    fp_volume(f, c, "cylinder", "cylinder owner");
    fp_volume(f, t, "tube", "tube owner");
    if (t->cap_pos.owner && t->cap_pos.owner != t->owner) f.object(t->cap_pos.owner, "tube cap_pos owner");
    if (t->cap_neg.owner && t->cap_neg.owner != t->owner) f.object(t->cap_neg.owner, "tube cap_neg owner");
}
PORT_FN(0x00435020, "collide_cylinder_tube", collide_cylinder_tube_rw, fp_cylinder_tube)

// ---- collide_sphere_box (0x435890) -----------------------------------------------------------------------
// The sphere's centre in the box's frame, clamped to the box (mode bit 1 / 2: nothing past +z / -z);
// the clamped point is the contact. A hit when it's at least 0.05 from the centre and within the
// radius plus 0.2. The box must be static (asserted, and only the sphere's owner is used): an impulse
// with friction when get_impulse_magnitude is positive, then the spring force plus friction.
static uint8_t __cdecl collide_sphere_box_rw(SphereVolume* s, BoxVolume* b) {
    const Frame* sf = s->frame;
    PhobDyno* so = (PhobDyno*)s->owner;
    const Frame* bf = b->frame;
    ASSERT_MSG(so != 0, (const char*)0x004ed700);
    ASSERT_MSG(b->owner == 0, (const char*)0x004ed734);
    P3 C;
    MatrixMulPoint(&C, &s->local_center, &sf->rot);
    C.x = sf->pos.x + C.x;
    C.y = sf->pos.y + C.y;
    C.z = sf->pos.z + C.z;
    P3 p;                                                       // local to the box
    p.x = C.x - bf->pos.x;
    p.y = C.y - bf->pos.y;
    p.z = C.z - bf->pos.z;
    MatrixMulPointInv(&p, &p, &bf->rot);
    // the clamps: fcom; test ah,0x41 for the top (a NaN stays), test ah,1 for the bottom (a NaN
    // becomes -h); the kept value goes through the FPU
    p.x = x87_float(p.x > b->half_extent.x ? D(b->half_extent.x) : D(p.x));
    p.x = x87_float(!(p.x >= -D(b->half_extent.x)) ? -D(b->half_extent.x) : D(p.x));
    p.y = x87_float(p.y > b->half_extent.y ? D(b->half_extent.y) : D(p.y));
    p.y = x87_float(!(p.y >= -D(b->half_extent.y)) ? -D(b->half_extent.y) : D(p.y));
    int32_t mode = b->mode;
    if ((mode & 1) && !(b->half_extent.z >= p.z)) return 0;     // test ah,1; je
    if ((mode & 2) && -D(b->half_extent.z) > p.z) return 0;     // test ah,0x41; jne
    p.z = x87_float(p.z > b->half_extent.z ? D(b->half_extent.z) : D(p.z));
    p.z = x87_float(!(p.z >= -D(b->half_extent.z)) ? -D(b->half_extent.z) : D(p.z));
    P3 W;                                                       // the contact point
    MatrixMulPoint(&W, &p, &bf->rot);
    W.x = bf->pos.x + W.x;
    W.y = bf->pos.y + W.y;
    W.z = bf->pos.z + W.z;
    P3 d;
    d.x = W.x - C.x;
    d.y = W.y - C.y;
    d.z = W.z - C.z;
    double dist = x87_sqrt((D(d.y) * d.y + D(d.z) * d.z) + D(d.x) * d.x);
    float distf = (float)dist;
    if (!(dist >= (float)0.05f)) return 0;                             // 0x4dbcf0; test ah,1
    double depth = (D(s->radius) - distf) + (float)0.2f;               // 0x4dbc84
    float depthf = (float)depth;
    if (!(depth > 0.0f)) return 0;

    P3 v;
    GetPointVelocity(so, 0, &v, &W);
    // (the original compares v . d with 0 here and never tests the result)
    float mag = (float)(((D(b->default_a) + s->default_a) * depthf) * 0.5f);
    if (!(fabs(D(distf)) > kEpsilon)) return 1;                 // sic: a hit, with nothing applied
    P3 n;
    n.x = (float)-(D(d.x) / distf);
    n.y = (float)-(D(d.y) / distf);
    n.z = (float)-(D(d.z) / distf);
    P3 F;
    F.x = (float)(D(n.x) * mag);
    F.y = (float)(D(n.y) * mag);
    F.z = (float)(D(n.z) * mag);
    double imp = get_impulse_magnitude(&sf->pos, &v, bits(so->mass), &so->inv_inertia_world, &W, &n, 0u);
    float impf = (float)imp;
    if (imp > 0.0f) {                                           // fcom on ST0; test ah,0x41
        P3 J, fr;
        J.x = (float)(D(n.x) * impf);
        J.y = (float)(D(n.y) * impf);
        J.z = (float)(D(n.z) * impf);
        get_friction(&fr, &W, &n, bits(impf), s, b);
        J.x = fr.x + J.x;
        J.y = fr.y + J.y;
        J.z = fr.z + J.z;
        QueueExternalImpulse(so, 0, &J, &W, b->surface_type, 0x3f800000u);
        Collide(s, b, &W, &J, &n);
    }
    P3 fr;
    get_friction(&fr, &W, &n, bits(mag), s, b);
    F.x = F.x + fr.x;
    F.y = F.y + fr.y;
    F.z = F.z + fr.z;
    APPLY_FORCE(s, &F, &W, b->surface_type);
    return 1;
}
static void fp_sphere_box(Footprint& f, SphereVolume* s, BoxVolume* b) {
    fp_volume(f, s, "sphere", "sphere owner");
    fp_volume(f, b, "box", "box owner");
}
PORT_FN(0x00435890, "collide_sphere_box", collide_sphere_box_rw, fp_sphere_box)

// ---- collide_tube_box (0x435d30) -------------------------------------------------------------------------
// both end caps, then the cylinder (a v1.0 stub returning 0), all three always run
static uint8_t __cdecl collide_tube_box_rw(TubeVolume* t, BoxVolume* b) {
    uint8_t hit = collide_sphere_box(&t->cap_pos, b);
    hit |= collide_sphere_box(&t->cap_neg, b);
    hit |= collide_cylinder_box(t, b);
    return hit;
}
static void fp_tube_box(Footprint& f, TubeVolume* t, BoxVolume* b) {
    fp_volume(f, t, "tube", "tube owner");
    if (t->cap_pos.owner && t->cap_pos.owner != t->owner) f.object(t->cap_pos.owner, "tube cap_pos owner");
    if (t->cap_neg.owner && t->cap_neg.owner != t->owner) f.object(t->cap_neg.owner, "tube cap_neg owner");
    fp_volume(f, b, "box", "box owner");
}
PORT_FN(0x00435d30, "collide_tube_box", collide_tube_box_rw, fp_tube_box)
