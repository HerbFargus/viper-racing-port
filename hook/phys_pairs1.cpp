// phys_pairs1.cpp -- M3 3.2, group D: the sphere-sphere contact at the heart of every ball and car
// collision, the tube pairs built on it, and the two impulse/friction helpers (all physics:volume.obj).
//
// Written from the v1.0 disassembly as phys_math.cpp is: the same sums in the same grouping, the same
// float constants, values the original keeps in x87 registers as doubles and values it stores as floats.
// Divisions go through the Pentium FDIV workaround in the original (`__adjust_fdiv ? __adj_fdiv_* :
// fdiv`), a plain division on any CPU without that bug. Calls to other game functions go by address (so a
// rewrite hooked there is what runs), in the original's order with its arguments.
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// ---- the game functions these call -------------------------------------------------------------------------
typedef void(__fastcall* GetPointVelocity_t)(PhobDyno*, void*, P3* out, const P3* point);
typedef void(__fastcall* QueueExternalImpulse_t)(PhobDyno*, void*, const P3* impulse, const P3* point,
                                                  int surface, float weight);
typedef void(__cdecl* MatrixMulPoint_t)(P3*, const P3*, const M3*);
typedef void(__cdecl* Collide_t)(CollisionVolume*, CollisionVolume*, const P3* point, const P3* impulse,
                                 const P3* normal);
typedef uint8_t(__cdecl* collide_sphere_sphere_t)(SphereVolume*, SphereVolume*, CollisionVolume*,
                                                   CollisionVolume*);
typedef uint8_t(__cdecl* collide_sphere_cylinder_t)(SphereVolume*, CylinderVolume*);
typedef uint8_t(__cdecl* collide_cylinder_tube_t)(CylinderVolume*, TubeVolume*);
typedef uint8_t(__cdecl* collide_sphere_tube_t)(SphereVolume*, TubeVolume*);
static const GetPointVelocity_t GetPointVelocity = (GetPointVelocity_t)0x00444ee0;         // PhobDyno::
static const QueueExternalImpulse_t QueueExternalImpulse = (QueueExternalImpulse_t)0x00444e60;   // PhobDyno::
static const MatrixMulPoint_t MatrixMulPointFn = (MatrixMulPoint_t)0x00429420;
static const Collide_t CollideFn = (Collide_t)0x0043c710;                                    // collide.obj
static const collide_sphere_sphere_t collide_sphere_sphere = (collide_sphere_sphere_t)0x00433cc0;
static const collide_sphere_cylinder_t collide_sphere_cylinder = (collide_sphere_cylinder_t)0x00434690;
static const collide_cylinder_tube_t collide_cylinder_tube = (collide_cylinder_tube_t)0x00435020;
static const collide_sphere_tube_t collide_sphere_tube = (collide_sphere_tube_t)0x00434580;

// a volume's ApplyForce (vtable +0x10): CollisionVolume's passes it to the owner's ApplyExternalForce
#define APPLY_FORCE(v) VFN(v, 0x10, void, const P3*, const P3*, int)

// a collision writes both volumes and both owners (their force, torque and impulse queue are embedded;
// Car/AICar/Obstacle::ApplyExternalForce write only their own object)
static void fp_volume(Footprint& f, CollisionVolume* v, const char* what, const char* owner) {
    if (!v) return;
    f.object(v, what);
    if (v->owner) f.object(v->owner, owner);
}

// ---- collide_sphere_sphere (0x433cc0) --------------------------------------------------------------------------
// Two spheres, by their cached world centres. n is the unit vector from b's centre to a's, p the contact
// point (halfway through the overlap, on the line of centres). A penalty spring, k = min(4450, the mean
// stiffness x the overlap), always pushes them apart through each volume's ApplyForce, at half strength;
// if they're closing at p, an inelastic impulse along n (its y scaled by 0.2 and renormalised, so
// contacts barely lift or sink anything) goes first, queued on each owner, and Collide() makes the sound
// and replay events, naming ca/cb if given (the compound volumes the spheres belong to) or else a/b.
//
// Faithful quirks: n is divided out by the distance BEFORE the tests (coincident centres give 0/0 = NaN,
// then leave at the distance test -- so no fix is needed: the NaN never leaves, and 0/0 is an invalid operation,
// which the physics thread keeps masked; it unmasks only zero-divide and overflow, ExceptDiv0Crashes(1)); the overlap test is an integer test of the stored float's bits
// (overlap <= 0 as an int: +0, any negative, a negative NaN -- a positive NaN goes on); the impulse is
// queued on a with b's surface type and on b with a's, as are the forces.
static uint8_t __cdecl collide_sphere_sphere_rw(SphereVolume* a, SphereVolume* b, CollisionVolume* ca,
                                                CollisionVolume* cb) {
    PhobDyno* oa = (PhobDyno*)a->owner;
    PhobDyno* ob = (PhobDyno*)b->owner;
    P3 pa, pb;                                          // the centres, copied as integers
    memcpy(&pa, &a->world_center, 12);
    memcpy(&pb, &b->world_center, 12);
    float dx = (float)(D(pa.x) - pb.x);
    float dy = (float)(D(pa.y) - pb.y);
    double dzr = D(pa.z) - pb.z;                        // fst: stored, and squared from the register
    float dz = (float)dzr;
    double dist = x87_sqrt((dzr * dz + D(dy) * dy) + D(dx) * dx);
    float distf = (float)dist;                          // fst: the register value goes on into the overlap
    float overlap = (float)((D(b->radius) - dist) + a->radius);
    P3 n;
    n.x = (float)(D(dx) / distf);
    n.y = (float)(D(dy) / distf);
    n.z = (float)(D(dz) / distf);
    int32_t overlap_bits;                               // cmp dword [overlap], 0; jle
    memcpy(&overlap_bits, &overlap, 4);
    if (overlap_bits <= 0) return 0;
    if (!(fabs(D(distf)) > 1.1920928955078125e-07f)) return 0;   // test ah,0x41: <=, or unordered

    // the contact point, half the overlap in from a's surface
    double h = ((D(a->radius) - b->radius) + distf) * -0.5f;
    P3 p;
    p.x = (float)(D(n.x) * h + pa.x);
    p.y = (float)(D(n.y) * h + pa.y);
    p.z = (float)(h * n.z + pa.z);
    P3 va, vb;
    if (oa) GetPointVelocity(oa, 0, &va, &p);
    else memset(&va, 0, 12);
    if (ob) GetPointVelocity(ob, 0, &vb, &p);
    else memset(&vb, 0, 12);

    // the spring, capped: fcom 4450; test ah,0x41 -- replaced only when strictly greater
    double kr = ((D(b->default_a) + a->default_a) * overlap) * 0.5f;
    float k = (float)kr;
    if (kr > 4450.0f) k = 4450.0f;
    P3 F;
    F.x = (float)(D(n.x) * k);
    F.y = (float)(D(n.y) * k);
    F.z = (float)(D(n.z) * k);

    P3 rv, ra, rb;                                      // relative velocity; p from each centre
    rv.x = (float)(D(va.x) - vb.x);
    rv.y = (float)(D(va.y) - vb.y);
    rv.z = (float)(D(va.z) - vb.z);
    ra.x = (float)(D(p.x) - pa.x);
    ra.y = (float)(D(p.y) - pa.y);
    ra.z = (float)(D(p.z) - pa.z);
    rb.x = (float)(D(p.x) - pb.x);
    rb.y = (float)(D(p.y) - pb.y);
    rb.z = (float)(D(p.z) - pb.z);
    double closing = (D(rv.y) * n.y + D(rv.z) * n.z) + D(rv.x) * n.x;
    if (!(closing >= 0.0f)) {                           // test ah,1: <, or unordered
        n.y = (float)(D(n.y) * (float)0.2f);
        double inv = 1.0f / x87_sqrt((D(n.z) * n.z + D(n.y) * n.y) + D(n.x) * n.x);
        n.x = (float)(n.x * inv);
        n.y = (float)(n.y * inv);
        n.z = (float)(inv * n.z);
        P3 c1, c2;                                      // ra x n, rb x n
        c1.x = (float)(D(n.z) * ra.y - D(n.y) * ra.z);
        c1.y = (float)(D(n.x) * ra.z - D(n.z) * ra.x);
        c1.z = (float)(D(n.y) * ra.x - D(n.x) * ra.y);
        c2.x = (float)(D(n.z) * rb.y - D(n.y) * rb.z);
        c2.y = (float)(D(n.x) * rb.z - D(n.z) * rb.x);
        c2.z = (float)(D(n.y) * rb.x - D(n.x) * rb.y);
        // w1 = c1 through a's world inverse inertia: MatrixMulPoint, inlined (its sums, x then y then z)
        P3 w1, w2;
        if (!oa) memset(&w1, 0, 12);
        else {
            const float* M = oa->inv_inertia_world.m;
            w1.x = (float)((D(M[6]) * c1.z + D(M[3]) * c1.y) + D(M[0]) * c1.x);
            w1.y = (float)((D(M[1]) * c1.x + D(M[7]) * c1.z) + D(M[4]) * c1.y);
            w1.z = (float)((D(M[5]) * c1.y + D(M[2]) * c1.x) + D(M[8]) * c1.z);
        }
        P3 t1, t2;                                      // w1 x ra, w2 x rb
        t1.x = (float)(D(w1.y) * ra.z - D(w1.z) * ra.y);
        t1.y = (float)(D(w1.z) * ra.x - D(w1.x) * ra.z);
        t1.z = (float)(D(w1.x) * ra.y - D(w1.y) * ra.x);
        if (!ob) memset(&w2, 0, 12);
        else MatrixMulPointFn(&w2, &c2, &ob->inv_inertia_world);   // for b, the call
        t2.x = (float)(D(w2.y) * rb.z - D(w2.z) * rb.y);
        t2.y = (float)(D(w2.z) * rb.x - D(w2.x) * rb.z);
        t2.z = (float)(D(w2.x) * rb.y - D(w2.y) * rb.x);
        P3 s;
        s.x = (float)(D(t2.x) + t1.x);
        s.y = (float)(D(t2.y) + t1.y);
        s.z = (float)(D(t2.z) + t1.z);
        float inv_ma, inv_mb;                           // 0 (integer) for a volume without an owner
        if (!oa) memset(&inv_ma, 0, 4);
        else inv_ma = (float)(1.0f / D(oa->mass));
        if (!ob) memset(&inv_mb, 0, 4);
        else inv_mb = (float)(1.0f / D(ob->mass));
        double den = (((D(n.y) * s.y + D(n.z) * s.z) + D(n.x) * s.x) + inv_mb) + inv_ma;
        double num = (D(rv.y) * n.y + D(rv.z) * n.z) + D(rv.x) * n.x;
        double j = -(num / den);                        // fdivrp: st(1) = st(0) / st(1); fchs
        P3 I;
        I.x = (float)(D(n.x) * j);
        I.y = (float)(D(n.y) * j);
        I.z = (float)(j * n.z);
        if (oa) QueueExternalImpulse(oa, 0, &I, &p, b->surface_type, 1.0f);
        I.x = -I.x;
        I.y = -I.y;
        I.z = -I.z;
        if (ob) QueueExternalImpulse(ob, 0, &I, &p, a->surface_type, 1.0f);
        if (ca) CollideFn(ca, cb, &p, &I, &n);
        else CollideFn(a, b, &p, &I, &n);
    }
    F.x = (float)(D(F.x) * 0.5f);
    F.y = (float)(D(F.y) * 0.5f);
    F.z = (float)(D(F.z) * 0.5f);
    APPLY_FORCE(a)(a, 0, &F, &p, b->surface_type);
    F.x = -F.x;
    F.y = -F.y;
    F.z = -F.z;
    APPLY_FORCE(b)(b, 0, &F, &p, a->surface_type);
    return 1;
}
static void fp_collide_sphere_sphere(Footprint& f, SphereVolume* a, SphereVolume* b, CollisionVolume*,
                                     CollisionVolume*) {
    fp_volume(f, a, "a", "a->owner");
    fp_volume(f, b, "b", "b->owner");
}
PORT_FN(0x00433cc0, "collide_sphere_sphere", collide_sphere_sphere_rw, fp_collide_sphere_sphere)

// ---- collide_sphere_tube (0x434580) ----------------------------------------------------------------------------
// a tube is a cylinder with a sphere at each end: the cylinder first; if it misses, both end caps (both
// always tried, the results or-ed)
static uint8_t __cdecl collide_sphere_tube_rw(SphereVolume* s, TubeVolume* t) {
    if (collide_sphere_cylinder(s, t)) return 1;
    uint8_t r = collide_sphere_sphere(s, &t->cap_pos, 0, 0);
    r |= collide_sphere_sphere(s, &t->cap_neg, 0, 0);
    return r;
}
static void fp_collide_sphere_tube(Footprint& f, SphereVolume* s, TubeVolume* t) {
    fp_volume(f, s, "sphere", "sphere->owner");
    fp_volume(f, t, "tube", "tube->owner");        // its caps are embedded
}
PORT_FN(0x00434580, "collide_sphere_tube", collide_sphere_tube_rw, fp_collide_sphere_tube)

// ---- collide_tube_tube (0x4345d0) ------------------------------------------------------------------------------
// a bounding test on the owners' positions (the sum of both radii and half-lengths, squared, must exceed
// the squared distance: fcompp; test ah,0x41), then a's cylinder against b, then each of a's caps against b,
// stopping at the first hit
static uint8_t __cdecl collide_tube_tube_rw(TubeVolume* a, TubeVolume* b) {
    const P3* pa = &a->frame->pos;
    const P3* pb = &b->frame->pos;
    float dx = (float)(D(pa->x) - pb->x);
    float dy = (float)(D(pa->y) - pb->y);
    float dz = (float)(D(pa->z) - pb->z);
    double reach = ((D(a->radius) + b->radius) + a->half_length) + b->half_length;
    double d2 = (D(dz) * dz + D(dy) * dy) + D(dx) * dx;
    if (!(reach * reach > d2)) return 0;
    uint8_t r = collide_cylinder_tube(a, b);
    if (r) return 1;
    r |= collide_sphere_tube(&a->cap_pos, b);
    if (r) return 1;
    r |= collide_sphere_tube(&a->cap_neg, b);
    return r;
}
static void fp_collide_tube_tube(Footprint& f, TubeVolume* a, TubeVolume* b) {
    fp_volume(f, a, "a", "a->owner");
    fp_volume(f, b, "b", "b->owner");
}
PORT_FN(0x004345d0, "collide_tube_tube", collide_tube_tube_rw, fp_collide_tube_tube)

// ---- get_impulse_magnitude (0x435630) --------------------------------------------------------------------------
// The impulse along a contact normal for one body: pos its centre of mass, vel the contact point's
// velocity, mass, inv_inertia (world), point the contact, normal (not yet unit), restitution e:
//   j = -(1 + e)(n.v) / (1/m + n.((I^-1 (r x n)) x r)),  r = point - pos,
// 0 if the point isn't closing (n.v >= 0 on the unnormalised normal) or the denominator is within 1e-7 of
// 0; capped at 100000 (a NaN gives 100000). Returned unrounded in ST0.
static double __cdecl get_impulse_magnitude_rw(const P3* pos, const P3* vel, float mass, const M3* inv_inertia,
                                               const P3* point, const P3* normal, float restitution) {
    P3 v;
    memcpy(&v, vel, 12);                                // integer copies, as the original
    P3 r;
    r.x = (float)(D(point->x) - pos->x);
    r.y = (float)(D(point->y) - pos->y);
    r.z = (float)(D(point->z) - pos->z);
    P3 n;
    memcpy(&n, normal, 12);
    double closing = (D(normal->y) * v.y + D(normal->z) * v.z) + D(normal->x) * v.x;
    if (closing >= 0.0f) return 0.0f;                   // test ah,1; je: >=, ordered
    double k = 1.0f / x87_sqrt((D(n.z) * n.z + D(n.y) * n.y) + D(n.x) * n.x);
    n.x = (float)(n.x * k);
    n.y = (float)(n.y * k);
    n.z = (float)(k * n.z);
    P3 c;                                               // r x n
    c.x = (float)(D(n.z) * r.y - D(n.y) * r.z);
    c.y = (float)(D(n.x) * r.z - D(n.z) * r.x);
    c.z = (float)(D(n.y) * r.x - D(n.x) * r.y);
    P3 w;
    MatrixMulPointFn(&w, &c, inv_inertia);
    P3 t;                                               // w x r
    t.x = (float)(D(r.z) * w.y - D(r.y) * w.z);
    t.y = (float)(D(r.x) * w.z - D(r.z) * w.x);
    t.z = (float)(D(r.y) * w.x - D(r.x) * w.y);
    double den = ((D(n.y) * t.y + D(n.z) * t.z) + D(n.x) * t.x) + 1.0f / D(mass);
    float denf = (float)den;                            // fst: the division uses the stored value
    if (!(fabs(den) > (float)1e-7f)) return 0.0f;              // test ah,0x41: <=, or unordered
    double num = (D(n.y) * v.y + D(n.z) * v.z) + D(n.x) * v.x;
    double j = -((num * (D(restitution) + 1.0f)) / denf);
    if (100000.0f > j) return j;                        // fcom; test ah,0x41; je: 100000 > j, ordered
    return 100000.0f;
}
static void fp_impulse_magnitude(Footprint& f, const P3*, const P3*, float, const M3*, const P3*, const P3*,
                                 float) { f.pure = true; }
PORT_FN(0x00435630, "get_impulse_magnitude", get_impulse_magnitude_rw, fp_impulse_magnitude)

// ---- get_friction (0x435d80) -----------------------------------------------------------------------------------
// The friction force at a contact: the relative velocity of the two owners at the point (an owner-less
// volume counts as still), its component along the (unit) normal removed, times -1000; limited to
// mu x the mean of the two volumes' friction (default_b) -- scaled down, not clipped per axis.
static void __cdecl get_friction_rw(P3* out, const P3* point, const P3* normal, float mu, CollisionVolume* va,
                                    CollisionVolume* vb) {
    PhobDyno* oa = (PhobDyno*)va->owner;
    PhobDyno* ob = (PhobDyno*)vb->owner;
    P3 v1, v2;
    if (oa) GetPointVelocity(oa, 0, &v1, point);
    else memset(&v1, 0, 12);
    if (ob) GetPointVelocity(ob, 0, &v2, point);
    else memset(&v2, 0, 12);
    float rx = (float)(D(v1.x) - v2.x);
    float ry = (float)(D(v1.y) - v2.y);
    float rz = (float)(D(v1.z) - v2.z);
    double dot = (D(normal->y) * ry + D(normal->z) * rz) + D(normal->x) * rx;
    rx = (float)(rx - D(normal->x) * dot);
    ry = (float)(ry - D(normal->y) * dot);
    rz = (float)(rz - dot * normal->z);
    const double k = -1000.0f;                          // fld 1000; fchs
    float F[3];
    F[0] = (float)(rx * k);
    F[1] = (float)(ry * k);
    double fz = k * rz;                                 // fst: squared as register x stored
    F[2] = (float)fz;
    float mag = (float)x87_sqrt((fz * F[2] + D(F[1]) * F[1]) + D(F[0]) * F[0]);
    double limit = ((D(vb->default_b) + va->default_b) * mu) * 0.5f;
    float limitf = (float)limit;                        // compared from the register, divided stored
    if (!(limit >= mag)) {                              // fcom; test ah,1: <, or unordered
        double s = D(limitf) / mag;
        F[0] = (float)(F[0] * s);
        F[1] = (float)(F[1] * s);
        F[2] = (float)(s * F[2]);
    }
    memcpy(out, F, 12);                                 // integer moves
}
static void fp_get_friction(Footprint& f, P3* out, const P3*, const P3*, float, CollisionVolume*,
                            CollisionVolume*) {
    f.add(out, 12, "out");                              // reads the owners (GetPointVelocity), writes out
}
PORT_FN(0x00435d80, "get_friction", get_friction_rw, fp_get_friction)
