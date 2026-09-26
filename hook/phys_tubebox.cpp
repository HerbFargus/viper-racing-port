// phys_tubebox.cpp -- M3 3.2 group C: volume.obj's cylinders, tubes and boxes, rewritten.
//
// A TubeVolume is a CylinderVolume (radius, half length along local z) carrying two SphereVolumes, its end
// caps at (0,0,+h) and (0,0,-h); everything it does goes through the caps. A BoxVolume is a half extent
// about its owner's frame. Each function is written from its v1.0 disassembly. The constructors call the
// base constructors by address (they're rewritten and hooked on their own), and the float arguments they
// only copy are copied as bits, as the original's integer moves do: a copy through the FPU would quieten a
// signalling NaN. The base constructors' float parameters are declared as their bits (uint32_t) here for
// the same reason; on the stack a float and a uint32_t are the same four bytes.
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// a float's bits, read from its memory (passing the float by value would load it through the FPU)
static __forceinline uint32_t bits_of(const float* p) { uint32_t u; memcpy(&u, p, 4); return u; }
#define bits(lv) bits_of(&(lv))
static __forceinline void set_bits(float* p, uint32_t u) { memcpy(p, &u, 4); }

// the type tags (CollisionVolume::type_tag)
enum : uint32_t { TAG_BOX = 0x424f5820, TAG_SPHR = 0x53504852, TAG_TUBE = 0x54554245 };

// the vtables the constructors install
enum : uint32_t { VT_CYLINDER = 0x004dbd98, VT_TUBE = 0x004dbdb8, VT_BOX = 0x004dbdd8 };

// the base constructors and the collision tests, by address
typedef CollisionVolume*(__fastcall* CollisionVolumeCtor_t)(void* self, void* edx, const Frame* frame, uint32_t tag,
                                                            uint32_t a, uint32_t b, PhobDyno* owner);
typedef SphereVolume*(__fastcall* SphereVolumeCtor_t)(void* self, void* edx, const Frame* frame, uint32_t a,
                                                      uint32_t b, const P3* center, uint32_t radius, PhobDyno* owner);
typedef SphereVolume*(__fastcall* SphereVolumeCtorFrame_t)(void* self, void* edx, const Frame* frame);
typedef CylinderVolume*(__fastcall* CylinderVolumeCtor_t)(void* self, void* edx, const Frame* frame, uint32_t tag,
                                                          uint32_t a, uint32_t b, uint32_t radius, uint32_t half_length,
                                                          PhobDyno* owner);
typedef CylinderVolume*(__fastcall* CylinderVolumeCtorFrame_t)(void* self, void* edx, const Frame* frame);
static const CollisionVolumeCtor_t CollisionVolume_ctor = (CollisionVolumeCtor_t)0x004322e0;
static const SphereVolumeCtor_t SphereVolume_ctor = (SphereVolumeCtor_t)0x00432340;
static const SphereVolumeCtorFrame_t SphereVolume_ctor_frame = (SphereVolumeCtorFrame_t)0x004323a0;
static const CylinderVolumeCtor_t CylinderVolume_ctor = (CylinderVolumeCtor_t)0x00433600;
static const CylinderVolumeCtorFrame_t CylinderVolume_ctor_frame = (CylinderVolumeCtorFrame_t)0x00433640;

typedef uint8_t(__cdecl* collide_tube_box_t)(TubeVolume*, BoxVolume*);
typedef uint8_t(__cdecl* collide_sphere_tube_t)(SphereVolume*, TubeVolume*);
typedef uint8_t(__cdecl* collide_tube_tube_t)(TubeVolume*, TubeVolume*);
typedef uint8_t(__cdecl* collide_sphere_box_t)(SphereVolume*, BoxVolume*);
static const collide_tube_box_t collide_tube_box = (collide_tube_box_t)0x00435d30;
static const collide_sphere_tube_t collide_sphere_tube = (collide_sphere_tube_t)0x00434580;
static const collide_tube_tube_t collide_tube_tube = (collide_tube_tube_t)0x004345d0;
static const collide_sphere_box_t collide_sphere_box = (collide_sphere_box_t)0x00435890;
typedef void(__cdecl* LogReport_t)(const char*, ...);
static const LogReport_t LogReport = (LogReport_t)0x00411150;

// a collision writes both volumes and both owners
static void fp_collide(Footprint& f, CollisionVolume* self, CollisionVolume* other) {
    f.object(self, "this");
    f.object(other, "other");
    if (self->owner) f.object(self->owner, "this->owner");
    if (other->owner) f.object(other->owner, "other->owner");
}

// ---- CylinderVolume -------------------------------------------------------------------------------------------

// CylinderVolume::CylinderVolume(frame, tag, a, b, radius, half_length, owner) (0x433600)
static CylinderVolume* __fastcall CylinderVolume_CtorFull(CylinderVolume* self, void*, const Frame* frame, uint32_t tag,
                                                          float a, float b, float radius, float half_length,
                                                          PhobDyno* owner) {
    CollisionVolume_ctor(self, 0, frame, tag, bits(a), bits(b), owner);
    self->vtable = (void**)VT_CYLINDER;
    set_bits(&self->radius, bits(radius));
    set_bits(&self->half_length, bits(half_length));
    return self;
}
static void fp_cylinder_ctor_full(Footprint& f, CylinderVolume* self, void*, const Frame*, uint32_t, float, float, float,
                                  float, PhobDyno*) {
    f.add(self, sizeof(CylinderVolume), "this");
}
PORT_FN(0x00433600, "CylinderVolume::CylinderVolume(full)", CylinderVolume_CtorFull, fp_cylinder_ctor_full)

// CylinderVolume::CylinderVolume(frame) (0x433640): tagged 'TUBE', a = 1, b = 0.6, no owner; the radius and
// half length are left as they were
static CylinderVolume* __fastcall CylinderVolume_CtorFrame(CylinderVolume* self, void*, const Frame* frame) {
    CollisionVolume_ctor(self, 0, frame, TAG_TUBE, 0x3f800000u /* 1.0f */, 0x3f19999au /* 0.6f */, 0);
    self->vtable = (void**)VT_CYLINDER;
    return self;
}
static void fp_cylinder_ctor_frame(Footprint& f, CylinderVolume* self, void*, const Frame*) {
    f.add(self, sizeof(CylinderVolume), "this");
}
PORT_FN(0x00433640, "CylinderVolume::CylinderVolume(frame)", CylinderVolume_CtorFrame, fp_cylinder_ctor_frame)

// CylinderVolume::GetCenter (0x4336a0): a volume.obj static (0x521cc0), whatever the cylinder
static const P3* __fastcall CylinderVolume_GetCenter(CylinderVolume*, void*) {
    return (const P3*)0x00521cc0;
}
static void fp_cylinder_get_center(Footprint& f, CylinderVolume*, void*) { f.pure = true; }
PORT_FN(0x004336a0, "CylinderVolume::GetCenter", CylinderVolume_GetCenter, fp_cylinder_get_center)

// ---- TubeVolume -----------------------------------------------------------------------------------------------

// TubeVolume::TubeVolume(frame, a, b, radius, half_length, owner) (0x4336b0): the cylinder, then the caps at
// (0,0,+h) -- h's bits -- and (0,0,-h) -- h through fld/fchs/fstp
static TubeVolume* __fastcall TubeVolume_CtorFull(TubeVolume* self, void*, const Frame* frame, float a, float b,
                                                  float radius, float half_length, PhobDyno* owner) {
    CylinderVolume_ctor(self, 0, frame, TAG_TUBE, bits(a), bits(b), bits(radius), bits(half_length), owner);
    P3 c;
    set_bits(&c.x, 0);
    set_bits(&c.y, 0);
    set_bits(&c.z, bits(half_length));
    SphereVolume_ctor(&self->cap_pos, 0, frame, bits(a), bits(b), &c, bits(radius), owner);
    set_bits(&c.x, 0);
    set_bits(&c.y, 0);
    c.z = (float)-D(half_length);
    SphereVolume_ctor(&self->cap_neg, 0, frame, bits(a), bits(b), &c, bits(radius), owner);
    self->vtable = (void**)VT_TUBE;
    set_bits(&self->radius, bits(radius));
    set_bits(&self->half_length, bits(half_length));
    return self;
}
static void fp_tube_ctor_full(Footprint& f, TubeVolume* self, void*, const Frame*, float, float, float, float,
                              PhobDyno*) {
    f.add(self, sizeof(TubeVolume), "this");
}
PORT_FN(0x004336b0, "TubeVolume::TubeVolume(full)", TubeVolume_CtorFull, fp_tube_ctor_full)

// TubeVolume::TubeVolume(frame) (0x4337a0): the caps' centres from the half length the object already holds
// (the frame constructor of CylinderVolume doesn't set it)
static TubeVolume* __fastcall TubeVolume_CtorFrame(TubeVolume* self, void*, const Frame* frame) {
    CylinderVolume_ctor_frame(self, 0, frame);
    SphereVolume_ctor_frame(&self->cap_pos, 0, frame);
    SphereVolume_ctor_frame(&self->cap_neg, 0, frame);
    uint32_t h = bits(self->half_length);
    self->vtable = (void**)VT_TUBE;
    set_bits(&self->cap_pos.local_center.x, 0);
    set_bits(&self->cap_pos.local_center.y, 0);
    set_bits(&self->cap_pos.local_center.z, h);
    float nh = (float)-D(self->half_length);
    set_bits(&self->cap_neg.local_center.x, 0);
    set_bits(&self->cap_neg.local_center.y, 0);
    self->cap_neg.local_center.z = nh;
    return self;
}
static void fp_tube_ctor_frame(Footprint& f, TubeVolume* self, void*, const Frame*) {
    f.add(self, sizeof(TubeVolume), "this");
}
PORT_FN(0x004337a0, "TubeVolume::TubeVolume(frame)", TubeVolume_CtorFrame, fp_tube_ctor_frame)

// TubeVolume::Setup(a, b, radius, half_length) (0x433830): the same values into the tube and both caps
static void __fastcall TubeVolume_Setup(TubeVolume* self, void*, float a, float b, float radius, float half_length) {
    uint32_t A = bits(a), B = bits(b), R = bits(radius), H = bits(half_length);
    set_bits(&self->default_a, A);
    set_bits(&self->default_b, B);
    set_bits(&self->radius, R);
    set_bits(&self->half_length, H);
    set_bits(&self->cap_pos.default_a, A);
    set_bits(&self->cap_pos.default_b, B);
    set_bits(&self->cap_pos.radius, R);
    set_bits(&self->cap_pos.local_center.x, 0);
    set_bits(&self->cap_pos.local_center.y, 0);
    set_bits(&self->cap_pos.local_center.z, H);
    float nh = (float)-D(half_length);
    set_bits(&self->cap_neg.default_b, B);
    set_bits(&self->cap_neg.radius, R);
    set_bits(&self->cap_neg.default_a, A);
    set_bits(&self->cap_neg.local_center.x, 0);
    set_bits(&self->cap_neg.local_center.y, 0);
    self->cap_neg.local_center.z = nh;
}
static void fp_tube_setup(Footprint& f, TubeVolume* self, void*, float, float, float, float) {
    f.object(self, "this");
    f.pure = true;
}
PORT_FN(0x00433830, "TubeVolume::Setup", TubeVolume_Setup, fp_tube_setup)

// TubeVolume::GetExtents (0x4338e0): min from the +h cap, max from the -h cap (each cap's other output goes
// to a scratch vector). So they bound the whole tube only where the +h cap's centre is at or below the -h
// cap's on every world axis; that's the original's behaviour, kept.
static void __fastcall TubeVolume_GetExtents(TubeVolume* self, void*, P3* mn, P3* mx) {
    P3 unused_max, unused_min;
    VFN(&self->cap_pos, 24, void, P3*, P3*)(&self->cap_pos, 0, mn, &unused_max);
    VFN(&self->cap_neg, 24, void, P3*, P3*)(&self->cap_neg, 0, &unused_min, mx);
}
static void fp_tube_get_extents(Footprint& f, TubeVolume*, void*, P3* mn, P3* mx) {
    f.add(mn, sizeof(P3), "min");
    f.add(mx, sizeof(P3), "max");
}
PORT_FN(0x004338e0, "TubeVolume::GetExtents", TubeVolume_GetExtents, fp_tube_get_extents)

// TubeVolume::Collide (0x433920): by the other volume's type; the return values are dropped
static void __fastcall TubeVolume_Collide(TubeVolume* self, void*, CollisionVolume* other) {
    uint32_t tag = other->type_tag;
    if (tag == TAG_BOX) collide_tube_box(self, (BoxVolume*)other);
    else if (tag == TAG_SPHR) collide_sphere_tube((SphereVolume*)other, self);
    else if (tag == TAG_TUBE) collide_tube_tube(self, (TubeVolume*)other);
    else LogReport((const char*)0x004ed6dc, tag);    // "Unsupported collision type" (Tube...)
}
static void fp_tube_collide(Footprint& f, TubeVolume* self, void*, CollisionVolume* other) {
    fp_collide(f, self, other);
}
PORT_FN(0x00433920, "TubeVolume::Collide", TubeVolume_Collide, fp_tube_collide)

// TubeVolume::CollideGround (0x433980) is a bare `ret`: not rewritten.

// TubeVolume::Update (0x436230): the caps' own Updates (their world centres)
static void __fastcall TubeVolume_Update(TubeVolume* self, void*) {
    VFN(&self->cap_pos, 4, void)(&self->cap_pos, 0);
    VFN(&self->cap_neg, 4, void)(&self->cap_neg, 0);
}
static void fp_tube_update(Footprint& f, TubeVolume* self, void*) { f.object(self, "this"); }
PORT_FN(0x00436230, "TubeVolume::Update", TubeVolume_Update, fp_tube_update)

// ---- BoxVolume ------------------------------------------------------------------------------------------------

// BoxVolume::BoxVolume(frame, a, b, hx, hy, hz, owner) (0x433990): the half extent copied, mode 0, and
// max_half_extent the largest of the three, picked on the FPU (fcom; test ah,1 -- "less or unordered")
static BoxVolume* __fastcall BoxVolume_CtorFull(BoxVolume* self, void*, const Frame* frame, float a, float b, float hx,
                                                float hy, float hz, PhobDyno* owner) {
    CollisionVolume_ctor(self, 0, frame, TAG_BOX, bits(a), bits(b), owner);
    self->vtable = (void**)VT_BOX;
    set_bits(&self->half_extent.x, bits(hx));
    set_bits(&self->half_extent.y, bits(hy));
    self->mode = 0;
    set_bits(&self->half_extent.z, bits(hz));
    double m = !(D(hx) >= D(hy)) ? D(hy) : D(hx);
    m = !(D(hz) >= m) ? m : D(hz);
    self->max_half_extent = (float)m;
    return self;
}
static void fp_box_ctor_full(Footprint& f, BoxVolume* self, void*, const Frame*, float, float, float, float, float,
                             PhobDyno*) {
    f.add(self, sizeof(BoxVolume), "this");
}
PORT_FN(0x00433990, "BoxVolume::BoxVolume(full)", BoxVolume_CtorFull, fp_box_ctor_full)

// BoxVolume::BoxVolume(frame) (0x433a10): tagged 'BOX ', a = 1, b = 0.6, no owner; nothing else set
static BoxVolume* __fastcall BoxVolume_CtorFrame(BoxVolume* self, void*, const Frame* frame) {
    CollisionVolume_ctor(self, 0, frame, TAG_BOX, 0x3f800000u /* 1.0f */, 0x3f19999au /* 0.6f */, 0);
    self->vtable = (void**)VT_BOX;
    return self;
}
static void fp_box_ctor_frame(Footprint& f, BoxVolume* self, void*, const Frame*) {
    f.add(self, sizeof(BoxVolume), "this");
}
PORT_FN(0x00433a10, "BoxVolume::BoxVolume(frame)", BoxVolume_CtorFrame, fp_box_ctor_frame)

// BoxVolume::Collide (0x433a40): spheres and tubes; a box doesn't collide with a box (nor logs anything)
static void __fastcall BoxVolume_Collide(BoxVolume* self, void*, CollisionVolume* other) {
    uint32_t tag = other->type_tag;
    if (tag == TAG_SPHR) collide_sphere_box((SphereVolume*)other, self);
    else if (tag == TAG_TUBE) collide_tube_box((TubeVolume*)other, self);
}
static void fp_box_collide(Footprint& f, BoxVolume* self, void*, CollisionVolume* other) { fp_collide(f, self, other); }
PORT_FN(0x00433a40, "BoxVolume::Collide", BoxVolume_Collide, fp_box_collide)

// BoxVolume::GetExtents (0x433a80): the world bounds of the eight corners. min and max start as the frame's
// position; each corner (+-hx, +-hy, +-hz, x fastest, + first) is rotated as a row vector (MatrixMulPoint's
// sums) and offset by the position, then folded in. The positive components are the half extent's bits; each
// negative one is a single fld/fchs, stored. The inlined multiply has an in-place branch (output == corner)
// that can't be taken -- the output is its own local -- so only the other branch is here.
static void __fastcall BoxVolume_GetExtents(BoxVolume* self, void*, P3* mn, P3* mx) {
    uint32_t X = bits(self->half_extent.x), Y = bits(self->half_extent.y), Z = bits(self->half_extent.z);
    float nx = (float)-D(self->half_extent.x);
    float ny = (float)-D(self->half_extent.y);
    float nz = (float)-D(self->half_extent.z);
    uint32_t NX = bits(nx), NY = bits(ny), NZ = bits(nz);
    const uint32_t corner_bits[8][3] = {{X, Y, Z},   {NX, Y, Z},   {X, NY, Z},   {NX, NY, Z},
                                        {X, Y, NZ},  {NX, Y, NZ},  {X, NY, NZ},  {NX, NY, NZ}};
    float corner[8][3];
    memcpy(corner, corner_bits, sizeof corner);

    // min = max = the position, copied as integers (the frame pointer is read again for each)
    const uint32_t* pos = (const uint32_t*)&self->frame->pos;
    uint32_t* o = (uint32_t*)mn;
    o[0] = pos[0]; o[1] = pos[1]; o[2] = pos[2];
    pos = (const uint32_t*)&self->frame->pos;
    o = (uint32_t*)mx;
    o[0] = pos[0]; o[1] = pos[1]; o[2] = pos[2];

    for (int i = 0; i < 8; i++) {
        const Frame* fr = self->frame;
        const float* M = fr->rot.m;
        float x = corner[i][0], y = corner[i][1], z = corner[i][2];
        float wx = (float)((D(M[6]) * z + D(M[3]) * y) + D(M[0]) * x);
        float wy = (float)((D(M[4]) * y + D(M[1]) * x) + D(M[7]) * z);
        float wz = (float)((D(M[8]) * z + D(M[5]) * y) + D(M[2]) * x);
        wx = (float)(D(fr->pos.x) + wx);
        wy = (float)(D(fr->pos.y) + wy);
        wz = (float)(D(fr->pos.z) + wz);
        // min: fcom; test ah,0x41 -- the corner when it's "less, equal or unordered"
        mn->x = (float)(!(D(wx) > D(mn->x)) ? D(wx) : D(mn->x));
        mn->y = (float)(!(D(wy) > D(mn->y)) ? D(wy) : D(mn->y));
        mn->z = (float)(!(D(wz) > D(mn->z)) ? D(wz) : D(mn->z));
        // max: fcom; test ah,1 -- the old max when the corner is "less or unordered"
        mx->x = (float)(!(D(wx) >= D(mx->x)) ? D(mx->x) : D(wx));
        mx->y = (float)(!(D(wy) >= D(mx->y)) ? D(mx->y) : D(wy));
        mx->z = (float)(!(D(wz) >= D(mx->z)) ? D(mx->z) : D(wz));
    }
}
static void fp_box_get_extents(Footprint& f, BoxVolume*, void*, P3* mn, P3* mx) {
    f.add(mn, sizeof(P3), "min");
    f.add(mx, sizeof(P3), "max");
}
PORT_FN(0x00433a80, "BoxVolume::GetExtents", BoxVolume_GetExtents, fp_box_get_extents)
