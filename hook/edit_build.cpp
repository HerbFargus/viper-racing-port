// edit_build.cpp -- M3 UI stage, step U5 (group B): the model tool's model builder, rewritten faithfully (library `edit`,
// modbuild.obj: ModBuilderCreate ... ModBuilderGetNumSurfaces, BuildInfo / BuildGeometry and their info_add_*, the
// selection's texture coordinates, the importers -- DXF (its group reader dxf_*, 3DFACE and polyface POLYLINE), mrModelInfo,
// .mod, .3ds --, the face and vertex normals, ModBuilderVertex's constructor). Layouts and addresses: edit_build.h; the
// 3DS reader itself is edit_3ds.cpp.
//
// The builder is the model tool's editable model: vertices (welded within 0.005), triangles with an edge flag per side
// (faceted or smoothed), and surfaces, each a texture with its own texture points (a vertex and u, v) and texture triangles
// (three texture points on one of the builder's triangles). BuildInfo turns it into the mrModelInfo the tool draws: per
// textured triangle its three corners with vertex normals (make_vertex_normals: each vertex's face normals weighted by the
// corner's angle) bent flat across the faceted edges, then the untextured triangles as one more surface; BuildGeometry
// gives every triangle its own flat-shaded corners (the tool's wire views).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own too, so a
// hooked rewrite is what runs), the compiler's inline string copies as it runs them (ui_types.h's crt_*). x87: a register
// value is a double, a stored one a float, the original's grouping and constants (the weld 2.5e-5 squared, the normals'
// 2^-23), divisions through __adjust_fdiv as plain divisions; floats the original moves as integers (positions, normals,
// texture coordinates) are moved as their bits (edit_build.h).
//
// Footprints (main thread). The builder's geometry is bounded by its own capacities (ModBuilderCreate's): fp_builder lists
// the builder, its vertices, triangles and surfaces and its mrModelInfo's arrays; a texture function lists its surface's
// arrays; the info_add_* their one record. Pure: the comparisons (is_triangle_same, edge_compare), make_vertex and the
// constructor. replay_only: whatever allocates or frees (Create, Destroy, Clear, Copy, dup_surface, AddSurface, ImportMI
// which adds surfaces) or reads a file (the importers, the DXF reader) -- the session replay (the model tool's import
// buttons, its undo copies).
//
// FIX CANDIDATEs (left faithful, marked in place; "editor" = an editor user can reach it):
//   editor: a DXF polyface face's edge flags (and every triangle's 16th byte) are never set -- stack garbage, so the import's
//     faceting is random (and a session replay of a polyface import may differ from the original's); a polyface mesh of
//     more than 4096 vertices overruns do_polyline's index table (on the stack); a .3ds of more than 4096 vertices or
//     triangles or 64 objects overruns Import3DS's arrays (on the stack); ModBuilderImportMOD's name has 254 characters
//     of room; a texture name of 16 or more characters runs over its surface (SetSurfaceTexture, BuildGeometry's name).
//   not reached by the tool (its builders are 4096 x 4096): ModBuilderCreate and the surfaces' allocations unchecked (out
//     of memory); BuildInfo's table of used triangles holds 4096 (a builder of more), and its untextured surface lands one
//     past the info's array when every surface is used; ModBuilderCopy into a smaller builder; the accessors' indices
//     unchecked where the original doesn't check them.
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "x87.h"
#include "edit_build.h"

namespace {
namespace edit_build {
using namespace ebld;
using uit::crt_strlen; using uit::crt_copy; using uit::iabs;

static const double EPS = (double)1.1920928955078125e-07f;     // 0x34000000 (2^-23)
static const double WELD = (double)(float)2.499999936844688e-05f;     // 0x37d1b717

// callees, by their v1.0 addresses
static __forceinline void* MemAlloc(int32_t n) { return ccall<void*>(F_MemAlloc, n); }
static __forceinline void Delete(void* p) { ccall<void>(F_Delete, p); }
static __forceinline double VectorLength(const volatile void* v) { return ccall<double>(F_VectorLength, (const void*)v); }
static __forceinline void VectorSub(volatile void* o, const volatile void* a, const volatile void* b) {
    ccall<void>(F_VectorSub, (void*)o, (const void*)a, (const void*)b);
}
static __forceinline void VectorNormalize(volatile void* v) { ccall<void>(F_VectorNormalize, (void*)v); }
static __forceinline double DotProduct(const volatile void* a, const volatile void* b) {
    return ccall<double>(F_DotProduct, (const void*)a, (const void*)b);
}
static __forceinline void ModBuilderClear(ModBuilder* mb) { ccall<void>(F_ModBuilderClear, mb); }
static __forceinline int32_t ModBuilderAddVertex(ModBuilder* mb, const volatile void* p) {
    return ccall<int32_t>(F_ModBuilderAddVertex, mb, (const void*)p);
}
static __forceinline int32_t ModBuilderAddTriangle(ModBuilder* mb, const volatile void* t) {
    return ccall<int32_t>(F_ModBuilderAddTriangle, mb, (const void*)t);
}
static __forceinline int32_t ModBuilderAddSurface(ModBuilder* mb) { return ccall<int32_t>(F_ModBuilderAddSurface, mb); }
static __forceinline void get_face_normal(ModBuilder* mb, volatile void* n, int32_t t) {
    ccall<void>(F_get_face_normal, mb, (void*)n, t);
}
static __forceinline uint8_t edge_compare(int32_t a, int32_t b, int32_t c, int32_t d) { return ccall<uint8_t>(F_edge_compare, a, b, c, d); }
static __forceinline int32_t info_add_surface(MrModelInfo* info, const char* name, uint32_t a, uint32_t b, uint32_t g) {
    return ccall<int32_t>(F_info_add_surface, info, name, a, b, g);
}
static __forceinline int32_t info_add_vertex(MrModelInfo* info, const volatile void* p, const volatile void* n, uint32_t u, uint32_t v) {
    return ccall<int32_t>(F_info_add_vertex, info, (const void*)p, (const void*)n, u, v);   // u, v: the floats' bits
}
static __forceinline int32_t info_add_triangle(MrModelInfo* info, int32_t a, int32_t b, int32_t c) {
    return ccall<int32_t>(F_info_add_triangle, info, a, b, c);
}
static __forceinline uint8_t dxf_next_group(int32_t fd) { return ccall<uint8_t>(F_dxf_next_group, fd); }
static __forceinline uint8_t dxf_read_item(int32_t fd) { return ccall<uint8_t>(F_dxf_read_item, fd); }
static __forceinline uint8_t dxf_next_item(int32_t fd) { return ccall<uint8_t>(F_dxf_next_item, fd); }
static __forceinline volatile float* make_vertex(volatile void* out, uint32_t x, uint32_t y, uint32_t z) {   // the floats' bits
    return ccall<volatile float*>(F_make_vertex, (void*)out, x, y, z);
}
static __forceinline int32_t ModBuilderAddTexturePoint(ModBuilder* mb, int32_t s, int32_t v, uint32_t u, uint32_t w) {
    return ccall<int32_t>(F_ModBuilderAddTexturePoint, mb, s, v, u, w);
}
static __forceinline uint8_t ModBuilderImportMI(ModBuilder* mb, MrModelInfo* info) { return ccall<uint8_t>(F_ModBuilderImportMI, mb, info); }
static __forceinline int32_t stricmp_(const char* a, const char* b) { return ccall<int32_t>(F_stricmp, a, b); }

// the surface record i of the builder, as the original indexes it (i * 0x2c)
static __forceinline MbSurface* surf(ModBuilder* mb, int32_t i) { return (MbSurface*)((uint8_t*)mb->surfs + i * 0x2c); }
// 1 / VectorLength(v), then v scaled by it: x, y as v * r, z as r * v (the original's operand order)
static __forceinline void scale_by_inverse_length(volatile float* v) {
    const double r = 1.0 / VectorLength(v);
    eb_st(&v[0], (double)v[0] * r);
    eb_st(&v[1], (double)v[1] * r);
    eb_st(&v[2], r * (double)v[2]);
}

// ---- footprints -------------------------------------------------------------------------------------------------------------
static void fp_builder(Footprint& f, ModBuilder* mb) {
    f.add(mb, sizeof(ModBuilder), "builder");
    f.add(mb->verts, (uint32_t)mb->maxverts * 0x18, "builder vertices");
    f.add(mb->tris, (uint32_t)mb->maxtris * 0x10, "builder triangles");
    f.add(mb->surfs, (uint32_t)mb->maxsurfs * 0x2c, "builder surfaces");
    f.add(mb->info.verts, (uint32_t)mb->maxverts * 0x20, "info vertices");
    f.add(mb->info.surfs, (uint32_t)(mb->maxsurfs + 1) * 0x20, "info surfaces");     // (+1: BuildInfo's untextured one)
    f.add(mb->info.tris, (uint32_t)mb->maxtris * 8, "info triangles");
}
static void fp_surface(Footprint& f, ModBuilder* mb, int32_t s) {
    f.add(mb, sizeof(ModBuilder), "builder");
    if (s < 0 || s >= mb->maxsurfs) return;
    MbSurface* const S = surf(mb, s);
    f.add(S, sizeof(MbSurface), "surface");
    f.add(S->points, (uint32_t)S->maxpoints * 0xc, "texture points");
    f.add(S->tris, (uint32_t)S->maxtris * 0x10, "texture triangles");
}
static const char* const ALLOCS = "allocates or frees (MemAlloc / delete)";
static const char* const READS = "reads a file (an import)";

// ==== create / destroy / copy ===============================================================================================
// ModBuilderCreate: a builder for ntris triangles (3 x ntris vertices) and nsurfs surfaces, empty
static ModBuilder* __cdecl ModBuilderCreate_n(int32_t ntris, int32_t nsurfs) {
    // FIX CANDIDATE: no allocation is checked but the vertices' (out of memory)
    ModBuilder* const mb = (ModBuilder*)MemAlloc(0x4c);
    mb->maxverts = ntris * 3;
    mb->nverts = 0;
    int32_t n = mb->maxverts;
    uint8_t* const v = (uint8_t*)MemAlloc(n * 0x18);
    if (v) {
        uint8_t* p = v;
        for (n--; n >= 0; n--) {
            tcall<void*>(F_ModBuilderVertex_ctor, p);
            p += 0x18;
        }
        mb->verts = (MbVertex*)v;
    } else {
        mb->verts = 0;
    }
    mb->maxtris = ntris;
    mb->ntris = 0;
    mb->tris = (MbTriangle*)MemAlloc(mb->maxtris << 4);
    mb->maxsurfs = nsurfs;
    mb->nsurfs = 0;
    mb->surfs = (MbSurface*)MemAlloc(mb->maxsurfs * 0x2c);
    mb->info.nverts = 0;
    mb->info.verts = (MrVertex*)MemAlloc(mb->maxverts << 5);
    mb->info.nsurfs = 0;
    mb->info.surfs = (MrSurface*)MemAlloc(mb->maxsurfs << 5);
    mb->info.ntris = 0;
    mb->info.tris = (MrTriangle*)MemAlloc(mb->maxtris << 3);
    mb->info.n18 = 0;
    mb->info.p1c = 0;
    mb->info.n20 = 0;
    mb->info.p24 = 0;
    return mb;
}
static void fp_ModBuilderCreate(Footprint& f, int32_t, int32_t) { f.replay_only = ALLOCS; }
PORT_FN(0x004b7050, "ModBuilderCreate", ModBuilderCreate_n, fp_ModBuilderCreate)

static void __cdecl ModBuilderDestroy_n(ModBuilder* mb) {
    Delete(mb->verts);
    Delete(mb->tris);
    for (int32_t i = 0; mb->nsurfs > i;) {
        void* const t = surf(mb, i)->tris;
        i++;
        Delete(t);
        Delete(surf(mb, i - 1)->points);
    }
    Delete(mb->surfs);
    Delete(mb->info.verts);
    Delete(mb->info.surfs);
    Delete(mb->info.tris);
    Delete(mb);
}
static void fp_alloc_mb(Footprint& f, ModBuilder*) { f.replay_only = ALLOCS; }
PORT_FN(0x004b7140, "ModBuilderDestroy", ModBuilderDestroy_n, fp_alloc_mb)

// ModBuilderClear: no vertices, triangles or surfaces (the surfaces' arrays freed)
static void __cdecl ModBuilderClear_n(ModBuilder* mb) {
    mb->nverts = 0;
    mb->ntris = 0;
    for (int32_t i = 0; mb->nsurfs > i;) {
        void* const t = surf(mb, i)->tris;
        i++;
        Delete(t);
        Delete(surf(mb, i - 1)->points);
    }
    mb->nsurfs = 0;
}
PORT_FN(0x004b71d0, "ModBuilderClear", ModBuilderClear_n, fp_alloc_mb)

// ModBuilderCopy: dst cleared, then src's vertices, triangles and surfaces (each surface's arrays duplicated)
static void __cdecl ModBuilderCopy_n(ModBuilder* dst, const ModBuilder* src) {
    ModBuilderClear(dst);
    // FIX CANDIDATE: dst's capacities aren't checked (the tool copies between builders of one size)
    for (int32_t i = 0; src->nverts > i; i++) {
        volatile uint32_t* d = (volatile uint32_t*)((uint8_t*)dst->verts + i * 0x18);
        const volatile uint32_t* s = (const volatile uint32_t*)((uint8_t*)src->verts + i * 0x18);
        for (int k = 0; k < 6; k++) d[k] = s[k];
    }
    dst->nverts = src->nverts;
    for (int32_t i = 0; src->ntris > i;) {
        const volatile uint32_t* s = (const volatile uint32_t*)((uint8_t*)src->tris + i * 0x10);
        volatile uint32_t* d = (volatile uint32_t*)((uint8_t*)dst->tris + i * 0x10);
        i++;
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    }
    dst->ntris = src->ntris;
    for (int32_t i = 0; src->nsurfs > i;) {
        const void* const s = (const uint8_t*)src->surfs + i * 0x2c;
        i++;
        ccall<void>(F_dup_surface, (void*)((uint8_t*)dst->surfs + (i - 1) * 0x2c), s);
    }
    dst->nsurfs = src->nsurfs;
}
static void fp_ModBuilderCopy(Footprint& f, ModBuilder*, const ModBuilder*) { f.replay_only = ALLOCS; }
PORT_FN(0x004b7220, "ModBuilderCopy", ModBuilderCopy_n, fp_ModBuilderCopy)

// dup_surface: the surface copied, with arrays of its own (of its capacities) holding src's points and triangles
static void __cdecl dup_surface_n(MbSurface* d, const MbSurface* s) {
    crt_copy(d, s, 0x2c);
    d->points = (MbPoint*)MemAlloc(d->maxpoints * 0xc);
    d->tris = (MbTexTri*)MemAlloc(d->maxtris << 4);
    for (int32_t i = 0; s->npoints > i;) {
        const volatile uint32_t* a = (const volatile uint32_t*)((const uint8_t*)s->points + i * 0xc);
        volatile uint32_t* b = (volatile uint32_t*)((uint8_t*)d->points + i * 0xc);
        i++;
        b[0] = a[0]; b[1] = a[1]; b[2] = a[2];
    }
    for (int32_t i = 0; s->ntris > i;) {
        const volatile uint32_t* a = (const volatile uint32_t*)((const uint8_t*)s->tris + i * 0x10);
        volatile uint32_t* b = (volatile uint32_t*)((uint8_t*)d->tris + i * 0x10);
        i++;
        b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; b[3] = a[3];
    }
}
static void fp_dup_surface(Footprint& f, MbSurface*, const MbSurface*) { f.replay_only = ALLOCS; }
PORT_FN(0x004b72d0, "dup_surface", dup_surface_n, fp_dup_surface)

// ==== vertices and triangles ================================================================================================
// ModBuilderAddVertex: the index of a vertex within 0.005 of p (squared distance below 2.5e-5, or a NaN), else p added;
// -1 if full
static int32_t __cdecl ModBuilderAddVertex_n(ModBuilder* mb, const EbP3* p) {
    volatile float d[3];
    const int32_t n = mb->nverts;
    for (int32_t i = 0; i < n; i++) {
        const MbVertex* const v = (const MbVertex*)((uint8_t*)mb->verts + i * 0x18);
        eb_st(&d[0], (double)v->x - p->x);
        eb_st(&d[1], (double)v->y - p->y);
        eb_st(&d[2], (double)v->z - p->z);
        if (!(((double)d[1] * d[1] + (double)d[2] * d[2]) + (double)d[0] * d[0] >= WELD)) return i;
    }
    if (mb->maxverts > n) {
        MbVertex* const v = (MbVertex*)((uint8_t*)mb->verts + n * 0x18);
        eb_cp3(&v->x, &p->x);
        const int32_t r = mb->nverts;
        mb->nverts = r + 1;
        return r;
    }
    EB_Log(EB_CP(0x004ff05c));                                                                  // "Too many vertices"
    return -1;
}
static void fp_ModBuilderAddVertex(Footprint& f, ModBuilder* mb, const EbP3*) { fp_builder(f, mb); }
PORT_FN(0x004b7370, "ModBuilderAddVertex", ModBuilderAddVertex_n, fp_ModBuilderAddVertex)

static uint8_t __cdecl ModBuilderGetVertex_n(ModBuilder* mb, int32_t i, EbP3* out) {
    if (i < 0 || mb->nverts <= i) return 0;
    const MbVertex* const v = (const MbVertex*)((uint8_t*)mb->verts + i * 0x18);
    eb_cp3(&out->x, &v->x);
    return 1;
}
static void fp_ModBuilderGetVertex(Footprint& f, ModBuilder*, int32_t, EbP3* out) { f.add(out, 12, "out"); }
PORT_FN(0x004b7430, "ModBuilderGetVertex", ModBuilderGetVertex_n, fp_ModBuilderGetVertex)

// ModBuilderAddTriangle: the index of the same triangle (the same three vertices in order), else it added; -1 if full
static int32_t __cdecl ModBuilderAddTriangle_n(ModBuilder* mb, const MbTriangle* t) {
    for (int32_t i = 0; mb->ntris > i; i++)
        if (ccall<uint8_t>(F_is_triangle_same, (const void*)((uint8_t*)mb->tris + i * 0x10), (const void*)t)) return i;
    const int32_t n = mb->ntris;
    if (mb->maxtris <= n) return -1;
    volatile uint32_t* const d = (volatile uint32_t*)((uint8_t*)mb->tris + n * 0x10);
    const volatile uint32_t* const s = (const volatile uint32_t*)t;
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    const int32_t r = mb->ntris;
    mb->ntris = r + 1;
    return r;
}
static void fp_ModBuilderAddTriangle(Footprint& f, ModBuilder* mb, const MbTriangle*) { fp_builder(f, mb); }
PORT_FN(0x004b7470, "ModBuilderAddTriangle", ModBuilderAddTriangle_n, fp_ModBuilderAddTriangle)

static uint8_t __cdecl is_triangle_same_n(const MbTriangle* a, const MbTriangle* b) {
    return a->v[0] == b->v[0] && b->v[1] == a->v[1] && b->v[2] == a->v[2];
}
static void fp_is_triangle_same(Footprint& f, const MbTriangle*, const MbTriangle*) { f.pure = true; }
PORT_FN(0x004b74f0, "is_triangle_same", is_triangle_same_n, fp_is_triangle_same)

static uint8_t __cdecl ModBuilderGetTriangle_n(ModBuilder* mb, int32_t i, MbTriangle* out) {
    if (i < 0 || mb->ntris <= i) return 0;
    const volatile uint32_t* const s = (const volatile uint32_t*)((uint8_t*)mb->tris + i * 0x10);
    volatile uint32_t* const d = (volatile uint32_t*)out;
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    return 1;
}
static void fp_ModBuilderGetTriangle(Footprint& f, ModBuilder*, int32_t, MbTriangle* out) { f.add(out, 0x10, "out"); }
PORT_FN(0x004b7520, "ModBuilderGetTriangle", ModBuilderGetTriangle_n, fp_ModBuilderGetTriangle)

static void __cdecl ModBuilderSetTriangle_n(ModBuilder* mb, int32_t i, const MbTriangle* t) {
    if (i < 0 || mb->ntris <= i) {
        EB_Log(EB_CP(0x004ff070), i);                                                           // "Set: bad triangle: %d"
        return;
    }
    const volatile uint32_t* const s = (const volatile uint32_t*)t;
    volatile uint32_t* const d = (volatile uint32_t*)((uint8_t*)mb->tris + i * 0x10);
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
}
static void fp_ModBuilderSetTriangle(Footprint& f, ModBuilder* mb, int32_t, const MbTriangle*) { fp_builder(f, mb); }
PORT_FN(0x004b7560, "ModBuilderSetTriangle", ModBuilderSetTriangle_n, fp_ModBuilderSetTriangle)

// smoothing: every edge (All) or the edge a-b of every triangle (Edge) smoothed (0) or faceted (1)
static void __cdecl ModBuilderSmoothAll_n(ModBuilder* mb) {
    for (int32_t i = 0; mb->ntris > i;) {
        i++;
        ((MbTriangle*)((uint8_t*)mb->tris + (i - 1) * 0x10))->edge[0] = 0;
        ((MbTriangle*)((uint8_t*)mb->tris + (i - 1) * 0x10))->edge[1] = 0;
        ((MbTriangle*)((uint8_t*)mb->tris + (i - 1) * 0x10))->edge[2] = 0;
    }
}
static void fp_mb(Footprint& f, ModBuilder* mb) { fp_builder(f, mb); }
PORT_FN(0x004b75b0, "ModBuilderSmoothAll", ModBuilderSmoothAll_n, fp_mb)
static void __cdecl ModBuilderFacetAll_n(ModBuilder* mb) {
    for (int32_t i = 0; mb->ntris > i;) {
        i++;
        ((MbTriangle*)((uint8_t*)mb->tris + (i - 1) * 0x10))->edge[0] = 1;
        ((MbTriangle*)((uint8_t*)mb->tris + (i - 1) * 0x10))->edge[1] = 1;
        ((MbTriangle*)((uint8_t*)mb->tris + (i - 1) * 0x10))->edge[2] = 1;
    }
}
PORT_FN(0x004b7690, "ModBuilderFacetAll", ModBuilderFacetAll_n, fp_mb)
static __forceinline void set_edge(ModBuilder* mb, int32_t a, int32_t b, uint8_t to) {
    for (int32_t i = 0; mb->ntris > i; i++) {
        MbTriangle* const t = (MbTriangle*)((uint8_t*)mb->tris + i * 0x10);
        if (edge_compare(t->v[0], t->v[1], a, b)) t->edge[0] = to;
        if (edge_compare(t->v[1], t->v[2], a, b)) t->edge[1] = to;
        if (edge_compare(t->v[0], t->v[2], a, b)) t->edge[2] = to;
    }
}
static void __cdecl ModBuilderSmoothEdge_n(ModBuilder* mb, int32_t a, int32_t b) { set_edge(mb, a, b, 0); }
static void fp_mb_ii(Footprint& f, ModBuilder* mb, int32_t, int32_t) { fp_builder(f, mb); }
PORT_FN(0x004b75f0, "ModBuilderSmoothEdge", ModBuilderSmoothEdge_n, fp_mb_ii)
static void __cdecl ModBuilderFacetEdge_n(ModBuilder* mb, int32_t a, int32_t b) { set_edge(mb, a, b, 1); }
PORT_FN(0x004b76d0, "ModBuilderFacetEdge", ModBuilderFacetEdge_n, fp_mb_ii)

// every vertex moved by d / scaled by s (each axis)
static void __cdecl ModBuilderTranslateModel_n(ModBuilder* mb, const EbP3* d) {
    for (int32_t i = 0; mb->nverts > i;) {
        MbVertex* const v = (MbVertex*)((uint8_t*)mb->verts + i * 0x18);
        i++;
        eb_st(&v->x, (double)d->x + v->x);
        eb_st(&v->y, (double)d->y + v->y);
        eb_st(&v->z, (double)v->z + d->z);
    }
}
static void fp_mb_p(Footprint& f, ModBuilder* mb, const EbP3*) { fp_builder(f, mb); }
PORT_FN(0x004b7770, "ModBuilderTranslateModel", ModBuilderTranslateModel_n, fp_mb_p)
static void __cdecl ModBuilderScaleModel_n(ModBuilder* mb, const EbP3* s) {
    for (int32_t i = 0; mb->nverts > i;) {
        MbVertex* v = (MbVertex*)((uint8_t*)mb->verts + i * 0x18);
        i++;
        eb_st(&v->x, (double)s->x * v->x);
        v = (MbVertex*)((uint8_t*)mb->verts + (i - 1) * 0x18);
        eb_st(&v->y, (double)v->y * s->y);
        v = (MbVertex*)((uint8_t*)mb->verts + (i - 1) * 0x18);
        eb_st(&v->z, (double)v->z * s->z);
    }
}
PORT_FN(0x004b77b0, "ModBuilderScaleModel", ModBuilderScaleModel_n, fp_mb_p)

// ==== surfaces ==============================================================================================================
// ModBuilderAddSurface: a surface with no texture ("") and empty arrays (of the builder's capacities); -1 if full
static int32_t __cdecl ModBuilderAddSurface_n(ModBuilder* mb) {
    const int32_t n = mb->nsurfs;
    if (mb->maxsurfs <= n) {
        EB_Log(EB_CP(0x004ff0a8));                                                              // "Too many surfaces"
        return -1;
    }
    MbSurface* const s = surf(mb, n);
    s->maxpoints = mb->maxverts;
    s->npoints = 0;
    s->points = (MbPoint*)MemAlloc(s->maxpoints * 0xc);
    s->maxtris = mb->maxtris;
    s->ntris = 0;
    s->tris = (MbTexTri*)MemAlloc(s->maxtris << 4);
    crt_copy(s->name, EB_CP(0x004ff0a4), crt_strlen(EB_CP(0x004ff0a4)) + 1);                     // ""
    s->mat0 = 0;
    s->mat1 = 0;
    s->group = 0;
    const int32_t r = mb->nsurfs;
    mb->nsurfs = r + 1;
    return r;
}
PORT_FN(0x004b7800, "ModBuilderAddSurface", ModBuilderAddSurface_n, fp_alloc_mb)

static uint8_t __cdecl ModBuilderIsSurface_n(ModBuilder* mb, int32_t i) { return i >= 0 && mb->nsurfs > i; }
static void fp_none_i(Footprint&, ModBuilder*, int32_t) {}
PORT_FN(0x004b78c0, "ModBuilderIsSurface", ModBuilderIsSurface_n, fp_none_i)

// FIX CANDIDATE: a texture name of 16 or more characters runs over the surface's fields (and the next surface)
static void __cdecl ModBuilderSetSurfaceTexture_n(ModBuilder* mb, int32_t i, const char* name) {
    const uint32_t n = crt_strlen(name) + 1;
    crt_copy(surf(mb, i)->name, name, n);
}
static void fp_ModBuilderSetSurfaceTexture(Footprint& f, ModBuilder* mb, int32_t i, const char* name) {
    f.add(surf(mb, i)->name, crt_strlen(name) + 1, "surface name");
}
PORT_FN(0x004b78e0, "ModBuilderSetSurfaceTexture", ModBuilderSetSurfaceTexture_n, fp_ModBuilderSetSurfaceTexture)

static void __cdecl ModBuilderGetSurfaceTexture_n(ModBuilder* mb, int32_t i, char* out) {
    const char* const s = surf(mb, i)->name;
    crt_copy(out, s, crt_strlen(s) + 1);
}
static void fp_ModBuilderGetSurfaceTexture(Footprint& f, ModBuilder* mb, int32_t i, char* out) {
    f.add(out, crt_strlen(surf(mb, i)->name) + 1, "out");
}
PORT_FN(0x004b7920, "ModBuilderGetSurfaceTexture", ModBuilderGetSurfaceTexture_n, fp_ModBuilderGetSurfaceTexture)

static void __cdecl ModBuilderSetSurfaceMaterial_n(ModBuilder* mb, int32_t i, uint8_t a, uint8_t b) {
    surf(mb, i)->mat0 = a;
    surf(mb, i)->mat1 = b;
}
static void fp_ModBuilderSetSurfaceMaterial(Footprint& f, ModBuilder* mb, int32_t i, uint8_t, uint8_t) {
    f.add((void*)&surf(mb, i)->mat0, 2, "surface material");
}
PORT_FN(0x004b7960, "ModBuilderSetSurfaceMaterial", ModBuilderSetSurfaceMaterial_n, fp_ModBuilderSetSurfaceMaterial)

static void __cdecl ModBuilderGetSurfaceMaterial_n(ModBuilder* mb, int32_t i, uint8_t* a, uint8_t* b) {
    *(volatile uint8_t*)a = surf(mb, i)->mat0;
    *(volatile uint8_t*)b = surf(mb, i)->mat1;
}
static void fp_ModBuilderGetSurfaceMaterial(Footprint& f, ModBuilder*, int32_t, uint8_t* a, uint8_t* b) {
    f.add(a, 1, "out a");
    f.add(b, 1, "out b");
}
PORT_FN(0x004b7990, "ModBuilderGetSurfaceMaterial", ModBuilderGetSurfaceMaterial_n, fp_ModBuilderGetSurfaceMaterial)

static void __cdecl ModBuilderSetSurfaceGroup_n(ModBuilder* mb, int32_t i, int16_t g) { surf(mb, i)->group = g; }
static void fp_ModBuilderSetSurfaceGroup(Footprint& f, ModBuilder* mb, int32_t i, int16_t) {
    f.add((void*)&surf(mb, i)->group, 2, "surface group");
}
PORT_FN(0x004b79c0, "ModBuilderSetSurfaceGroup", ModBuilderSetSurfaceGroup_n, fp_ModBuilderSetSurfaceGroup)

static void __cdecl ModBuilderGetSurfaceGroup_n(ModBuilder* mb, int32_t i, int16_t* g) { *(volatile int16_t*)g = surf(mb, i)->group; }
static void fp_ModBuilderGetSurfaceGroup(Footprint& f, ModBuilder*, int32_t, int16_t* g) { f.add(g, 2, "out"); }
PORT_FN(0x004b79e0, "ModBuilderGetSurfaceGroup", ModBuilderGetSurfaceGroup_n, fp_ModBuilderGetSurfaceGroup)

// ==== texture triangles and points ===========================================================================================
// ModBuilderAddTextureTriangle: triangle `tri` mapped by three of surface s's texture points; -1 if the surface is full or
// the triangle is already on a surface (any)
static int32_t __cdecl ModBuilderAddTextureTriangle_n(ModBuilder* mb, int32_t s, int32_t tri, int32_t p0, int32_t p1, int32_t p2) {
    MbSurface* const base = mb->surfs;
    const int32_t off = s * 0x2c;
    MbSurface* const S = (MbSurface*)((uint8_t*)base + off);
    const int32_t n = S->ntris;
    if (S->maxtris <= n) {
        EB_Log(EB_CP(0x004ff0e0));                                                              // "Too many texture triangles"
        return -1;
    }
    MbTexTri* const d = (MbTexTri*)((uint8_t*)S->tris + n * 0x10);
    const int32_t ns = mb->nsurfs;
    const MbSurface* k = base;
    for (int32_t i = 0; i < ns; i++, k++) {
        const int32_t kn = k->ntris;
        if (kn <= 0) continue;
        const MbTexTri* e = k->tris;
        for (int32_t j = 0; j < kn; j++, e++)
            if (e->tri == tri) {
                EB_Log(EB_CP(0x004ff0bc), s, i);                                               // "Duplicate triangle: surfs %d and %d"
                return -1;
            }
    }
    d->tri = tri;
    d->p[0] = p0;
    d->p[1] = p1;
    d->p[2] = p2;
    volatile int32_t* const c = &((MbSurface*)((uint8_t*)mb->surfs + off))->ntris;
    const int32_t r = *c;
    *c = r + 1;
    return r;
}
static void fp_ModBuilderAddTextureTriangle(Footprint& f, ModBuilder* mb, int32_t s, int32_t, int32_t, int32_t, int32_t) {
    fp_surface(f, mb, s);
}
PORT_FN(0x004b7a00, "ModBuilderAddTextureTriangle", ModBuilderAddTextureTriangle_n, fp_ModBuilderAddTextureTriangle)

static uint8_t __cdecl ModBuilderGetTextureTriangle_n(ModBuilder* mb, int32_t s, int32_t i, int32_t* tri, int32_t* p0, int32_t* p1,
                                                      int32_t* p2) {
    if (i < 0) return 0;
    const MbSurface* const S = surf(mb, s);
    if (S->ntris <= i) return 0;
    const MbTexTri* const e = (const MbTexTri*)((uint8_t*)S->tris + i * 0x10);
    *(volatile int32_t*)tri = e->tri;
    *(volatile int32_t*)p0 = e->p[0];
    *(volatile int32_t*)p1 = e->p[1];
    *(volatile int32_t*)p2 = e->p[2];
    return 1;
}
static void fp_ModBuilderGetTextureTriangle(Footprint& f, ModBuilder*, int32_t, int32_t, int32_t* a, int32_t* b, int32_t* c, int32_t* d) {
    f.add(a, 4, "out tri"); f.add(b, 4, "out p0"); f.add(c, 4, "out p1"); f.add(d, 4, "out p2");
}
PORT_FN(0x004b7ae0, "ModBuilderGetTextureTriangle", ModBuilderGetTextureTriangle_n, fp_ModBuilderGetTextureTriangle)

// ModBuilderDeleteTextureTriangle: the last texture triangle moved into i's place
static void __cdecl ModBuilderDeleteTextureTriangle_n(ModBuilder* mb, int32_t s, int32_t i) {
    if (i < 0) return;
    volatile int32_t* c = &surf(mb, s)->ntris;
    const int32_t n = *c;
    if (i >= n) return;
    *c = n - 1;
    const MbSurface* const S = surf(mb, s);
    const int32_t m = S->ntris;
    if (m <= 0) return;
    MbTexTri* const t = S->tris;
    const volatile uint32_t* const a = (const volatile uint32_t*)((uint8_t*)t + m * 0x10);
    volatile uint32_t* const b = (volatile uint32_t*)((uint8_t*)t + i * 0x10);
    b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; b[3] = a[3];
}
static void fp_ModBuilderDeleteTextureTriangle(Footprint& f, ModBuilder* mb, int32_t s, int32_t) { fp_surface(f, mb, s); }
PORT_FN(0x004b7b40, "ModBuilderDeleteTextureTriangle", ModBuilderDeleteTextureTriangle_n, fp_ModBuilderDeleteTextureTriangle)

// ModBuilderAddTexturePoint: the index of surface s's point on vertex v within 2^-23 of (u, v) (or NaN), else it added;
// -1 if full
static int32_t __cdecl ModBuilderAddTexturePoint_n(ModBuilder* mb, int32_t s, int32_t vert, float u, float v) {
    const int32_t off = s * 0x2c;
    MbSurface* const S = (MbSurface*)((uint8_t*)mb->surfs + off);
    const int32_t n = S->npoints;
    if (n > 0) {
        const double du = u, dv = v;
        const MbPoint* p = S->points;
        for (int32_t i = 0; i < n; i++, p++) {
            if (p->vert != vert) continue;
            if (fabs((double)p->u - du) > EPS) continue;
            if (!(fabs((double)p->v - dv) > EPS)) return i;
        }
    }
    if (S->maxpoints <= n) {
        EB_Log(EB_CP(0x004ff0fc));                                                              // "Too many texture points"
        return -1;
    }
    MbPoint* const d = (MbPoint*)((uint8_t*)S->points + n * 0xc);
    d->vert = vert;
    eb_put(&d->u, *(const volatile uint32_t*)&u);
    eb_put(&d->v, *(const volatile uint32_t*)&v);
    volatile int32_t* const c = &((MbSurface*)((uint8_t*)mb->surfs + off))->npoints;
    const int32_t r = *c;
    *c = r + 1;
    return r;
}
static void fp_ModBuilderAddTexturePoint(Footprint& f, ModBuilder* mb, int32_t s, int32_t, float, float) { fp_surface(f, mb, s); }
PORT_FN(0x004b7ba0, "ModBuilderAddTexturePoint", ModBuilderAddTexturePoint_n, fp_ModBuilderAddTexturePoint)

static uint8_t __cdecl ModBuilderGetTexturePoint_n(ModBuilder* mb, int32_t s, int32_t i, int32_t* vert, float* u, float* v) {
    if (i < 0) return 0;
    const MbSurface* const S = surf(mb, s);
    if (S->npoints <= i) return 0;
    const MbPoint* const p = (const MbPoint*)((uint8_t*)S->points + i * 0xc);
    *(volatile int32_t*)vert = p->vert;
    eb_cp(u, &p->u);
    eb_cp(v, &p->v);
    return 1;
}
static void fp_ModBuilderGetTexturePoint(Footprint& f, ModBuilder*, int32_t, int32_t, int32_t* a, float* b, float* c) {
    f.add(a, 4, "out vertex"); f.add(b, 4, "out u"); f.add(c, 4, "out v");
}
PORT_FN(0x004b7c70, "ModBuilderGetTexturePoint", ModBuilderGetTexturePoint_n, fp_ModBuilderGetTexturePoint)

// ModBuilderDeleteTexturePoint: false if a texture triangle uses it; else the last point moved into its place (and the
// triangles that used the last one renumbered)
static uint8_t __cdecl ModBuilderDeleteTexturePoint_n(ModBuilder* mb, int32_t s, int32_t i) {
    MbSurface* const S = surf(mb, s);
    if (i < 0) return 0;
    int32_t n = S->npoints;
    if (n <= i) return 0;
    {
        const int32_t nt = S->ntris;
        const MbTexTri* t = S->tris;
        for (int32_t k = 0; k < nt; k++, t++)
            if (t->p[0] == i || t->p[1] == i || t->p[2] == i) return 0;
    }
    const int32_t last = n * 3 - 3;
    n--;
    S->npoints = n;
    {
        MbPoint* const pts = S->points;
        const volatile uint32_t* const a = (const volatile uint32_t*)((uint8_t*)pts + last * 4);
        volatile uint32_t* const b = (volatile uint32_t*)((uint8_t*)pts + i * 0xc);
        b[0] = a[0]; b[1] = a[1]; b[2] = a[2];
    }
    for (int32_t k = 0; S->ntris > k; k++) {
        if (((MbTexTri*)((uint8_t*)S->tris + k * 0x10))->p[0] == S->npoints) ((MbTexTri*)((uint8_t*)S->tris + k * 0x10))->p[0] = i;
        if (((MbTexTri*)((uint8_t*)S->tris + k * 0x10))->p[1] == S->npoints) ((MbTexTri*)((uint8_t*)S->tris + k * 0x10))->p[1] = i;
        if (((MbTexTri*)((uint8_t*)S->tris + k * 0x10))->p[2] == S->npoints) ((MbTexTri*)((uint8_t*)S->tris + k * 0x10))->p[2] = i;
    }
    return 1;
}
static void fp_ModBuilderDeleteTexturePoint(Footprint& f, ModBuilder* mb, int32_t s, int32_t) { fp_surface(f, mb, s); }
PORT_FN(0x004b7cc0, "ModBuilderDeleteTexturePoint", ModBuilderDeleteTexturePoint_n, fp_ModBuilderDeleteTexturePoint)

static void __cdecl ModBuilderMoveTexturePoint_n(ModBuilder* mb, int32_t s, int32_t i, float u, float v) {
    MbPoint* const p = (MbPoint*)((uint8_t*)surf(mb, s)->points + i * 0xc);
    eb_put(&p->u, *(const volatile uint32_t*)&u);
    eb_put(&p->v, *(const volatile uint32_t*)&v);
}
static void fp_ModBuilderMoveTexturePoint(Footprint& f, ModBuilder* mb, int32_t s, int32_t i, float, float) {
    f.add((uint8_t*)surf(mb, s)->points + i * 0xc + 4, 8, "texture point");
}
PORT_FN(0x004b7d90, "ModBuilderMoveTexturePoint", ModBuilderMoveTexturePoint_n, fp_ModBuilderMoveTexturePoint)

static int32_t __cdecl ModBuilderGetNumSurfaces_n(ModBuilder* mb) { return mb->nsurfs; }
static void fp_none_mb(Footprint&, ModBuilder*) {}
PORT_FN(0x004b7dc0, "ModBuilderGetNumSurfaces", ModBuilderGetNumSurfaces_n, fp_none_mb)

// ==== BuildInfo ===============================================================================================================
// c = n x e (n the face's normal), unit length (or 0 if shorter than 2^-23): the in-plane normal of an edge e
static __forceinline void edge_normal(volatile float* c, const volatile float* n, const volatile float* e, const volatile float* zero) {
    eb_st(&c[0], (double)n[2] * e[1] - (double)n[1] * e[2]);
    eb_st(&c[1], (double)n[0] * e[2] - (double)n[2] * e[0]);
    eb_st(&c[2], (double)n[1] * e[0] - (double)n[0] * e[1]);
    if (!(fabs(x87_sqrt(((double)c[1] * c[1] + (double)c[2] * c[2]) + (double)c[0] * c[0])) > EPS)) eb_cp3(c, zero);
    else scale_by_inverse_length(c);
}
// q less its component along c
static __forceinline void flatten(volatile float* q, const volatile float* c) {
    const double d = -(((double)q[1] * c[1] + (double)q[2] * c[2]) + (double)q[0] * c[0]);
    eb_st(&q[0], (double)c[0] * d + q[0]);
    eb_st(&q[1], (double)c[1] * d + q[1]);
    eb_st(&q[2], d * (double)c[2] + q[2]);
}
// q unit length, or `up` if shorter than 2^-23
static __forceinline void unit_or(volatile float* q, const volatile float* up) {
    if (!(fabs(x87_sqrt(((double)q[1] * q[1] + (double)q[2] * q[2]) + (double)q[0] * q[0])) > EPS)) eb_cp3(q, up);
    else scale_by_inverse_length(q);
}

// ModBuilderBuildInfo: the builder as an mrModelInfo (its own): each textured surface's triangles with their vertex
// normals (bent flat across faceted edges) and texture points, then the triangles no surface maps, untextured
static const MrModelInfo* __cdecl ModBuilderBuildInfo_n(ModBuilder* mb) {
    volatile uint8_t used[0x1000];                     // F+0xd8: the triangles a surface maps
    volatile float N[3];                               // F+0x1c the face's normal
    volatile float E[3];                               // F+0x10 an edge
    volatile float Q0[3], Q2[3], Q1[3];                // F+0x28, +0x34, +0x40 the corners' normals
    volatile float C1[3], C2[3], C3[3];                // F+0x4c, +0x58, +0x64 the edges' in-plane normals
    volatile float P1[3], P2[3], P0[3];                // F+0x9c, +0xa8, +0xb4 the corners
    volatile float Z1[3], Z2[3], Z3[3], UP[3];         // F+0x84, +0x90, +0xcc zeros; F+0xc0 (0, 1, 0)
    MrModelInfo* const info = &mb->info;
    {
        const int32_t n = mb->ntris;
        // FIX CANDIDATE: 4096 triangles at most (a builder of more -- the tool's have 4096)
        if (n > 0)
            for (int32_t k = 0; k < n; k++) used[k] = 0;
    }
    ccall<void>(F_make_vertex_normals, mb);
    int32_t s = 0;
    info->ntris = 0;
    info->nverts = 0;
    info->nsurfs = 0;
    if (mb->nsurfs > 0) {
        int32_t soff = 0;
        do {
            MbSurface* const S = (MbSurface*)((uint8_t*)mb->surfs + soff);
            if (S->ntris != 0) {
                info_add_surface(info, S->name, S->mat0, S->mat1, (uint32_t)(uint16_t)S->group);
                int32_t t = 0;
                if (S->ntris > 0) {
                    eb_put(&Z1[0], 0); eb_put(&Z1[1], 0); eb_put(&Z1[2], 0);
                    eb_put(&Z2[0], 0); eb_put(&Z2[1], 0); eb_put(&Z2[2], 0);
                    eb_put(&Z3[0], 0); eb_put(&Z3[1], 0); eb_put(&Z3[2], 0);
                    eb_put(&UP[0], 0); eb_put(&UP[1], 0x3f800000); eb_put(&UP[2], 0);
                    int32_t toff = 0;
                    do {
                        const MbTexTri* const tt = (const MbTexTri*)((uint8_t*)S->tris + toff);
                        const MbTriangle* const T = (const MbTriangle*)((uint8_t*)mb->tris + tt->tri * 0x10);
                        const MbVertex* V = mb->verts;
                        used[tt->tri] = 1;
                        eb_cp3(P0, &((const MbVertex*)((uint8_t*)V + T->v[0] * 0x18))->x);
                        eb_cp3(P1, &((const MbVertex*)((uint8_t*)V + T->v[1] * 0x18))->x);
                        eb_cp3(P2, &((const MbVertex*)((uint8_t*)V + T->v[2] * 0x18))->x);
                        get_face_normal(mb, N, tt->tri);
                        // each edge's in-plane normal
                        eb_st(&E[0], (double)P0[0] - P1[0]); eb_st(&E[1], (double)P0[1] - P1[1]); eb_st(&E[2], (double)P0[2] - P1[2]);
                        edge_normal(C1, N, E, Z1);
                        eb_st(&E[0], (double)P1[0] - P2[0]); eb_st(&E[1], (double)P1[1] - P2[1]); eb_st(&E[2], (double)P1[2] - P2[2]);
                        edge_normal(C2, N, E, Z2);
                        eb_st(&E[0], (double)P2[0] - P0[0]); eb_st(&E[1], (double)P2[1] - P0[1]); eb_st(&E[2], (double)P2[2] - P0[2]);
                        edge_normal(C3, N, E, Z3);
                        // the corners' vertex normals, flattened across the faceted edges
                        V = mb->verts;
                        eb_cp3(Q0, &((const MbVertex*)((uint8_t*)V + T->v[0] * 0x18))->nx);
                        eb_cp3(Q1, &((const MbVertex*)((uint8_t*)V + T->v[1] * 0x18))->nx);
                        eb_cp3(Q2, &((const MbVertex*)((uint8_t*)V + T->v[2] * 0x18))->nx);
                        if (T->edge[0]) { flatten(Q0, C1); flatten(Q1, C1); }
                        if (T->edge[1]) { flatten(Q1, C2); flatten(Q2, C2); }
                        if (T->edge[2]) { flatten(Q2, C3); flatten(Q0, C3); }
                        if (T->edge[0] && T->edge[2] && T->edge[1]) { eb_cp3(Q0, N); eb_cp3(Q1, N); eb_cp3(Q2, N); }
                        unit_or(Q0, UP);
                        unit_or(Q1, UP);
                        unit_or(Q2, UP);
                        if (T->edge[0] && T->edge[1] && T->edge[2]) {
                            eb_st(&E[0], (double)Q0[0] - N[0]); eb_st(&E[1], (double)Q0[1] - N[1]); eb_st(&E[2], (double)Q0[2] - N[2]);
                            if (((double)E[1] * E[1] + (double)E[2] * E[2]) + (double)E[0] * E[0] > EPS)
                                EB_Log(EB_CP(0x004ff114), (double)Q0[0], (double)Q0[1], (double)Q0[2], (double)N[0], (double)N[1],
                                       (double)N[2]);                                            // "Normal1... %4.3f,%4.3f,%4.3f vs ..."
                        }
                        const MbPoint* const pts = S->points;
                        const MbPoint* const p2 = (const MbPoint*)((uint8_t*)pts + tt->p[2] * 0xc);
                        const MbPoint* const p1 = (const MbPoint*)((uint8_t*)pts + tt->p[1] * 0xc);
                        const MbPoint* const p0 = (const MbPoint*)((uint8_t*)pts + tt->p[0] * 0xc);
                        const int32_t i2 = info_add_vertex(info, (uint8_t*)mb->verts + T->v[2] * 0x18, Q2, eb_bits(&p2->u), eb_bits(&p2->v));
                        const int32_t i1 = info_add_vertex(info, (uint8_t*)mb->verts + T->v[1] * 0x18, Q1, eb_bits(&p1->u), eb_bits(&p1->v));
                        const int32_t i0 = info_add_vertex(info, (uint8_t*)mb->verts + T->v[0] * 0x18, Q0, eb_bits(&p0->u), eb_bits(&p0->v));
                        info_add_triangle(info, i0, i1, i2);
                        t++;
                        toff += 0x10;
                    } while (S->ntris > t);
                }
            }
            soff += 0x2c;
            s++;
        } while (mb->nsurfs > s);
    }
    // the triangles no surface maps: one more surface, untextured
    volatile uint8_t added = 0;
    for (int32_t i = 0; mb->ntris > i; i++) {
        if (used[i] != 0) continue;
        if (added == 0) {
            added = 1;
            // FIX CANDIDATE: with every one of the info's surfaces used (nsurfs == the builder's capacity) this one lands
            // past its array
            info_add_surface(info, EB_CP(0x004ff148), 0, 0, 0);                                     // ""
        }
        const MbTriangle* const T = (const MbTriangle*)((uint8_t*)mb->tris + i * 0x10);
        const MbVertex* const V = mb->verts;
        const MbVertex* const a = (const MbVertex*)((uint8_t*)V + T->v[2] * 0x18);
        const MbVertex* const b = (const MbVertex*)((uint8_t*)V + T->v[1] * 0x18);
        const MbVertex* const c = (const MbVertex*)((uint8_t*)V + T->v[0] * 0x18);
        const int32_t i2 = info_add_vertex(info, a, &a->nx, 0, 0x3f800000);
        const int32_t i1 = info_add_vertex(info, b, &b->nx, 0x3f800000, 0);
        const int32_t i0 = info_add_vertex(info, c, &c->nx, 0, 0);
        info_add_triangle(info, i0, i1, i2);
    }
    return info;
}
static void fp_ModBuilderBuildInfo(Footprint& f, ModBuilder* mb) {
    fp_builder(f, mb);
    f.add((void*)(uintptr_t)S_FACE_WARNED, 1, "get_face_normal's warning");
}
PORT_FN(0x004b7dd0, "ModBuilderBuildInfo", ModBuilderBuildInfo_n, fp_ModBuilderBuildInfo)

// info_add_surface: a surface after the info's last, starting at its vertex and triangle counts
static int32_t __cdecl info_add_surface_n(MrModelInfo* info, const char* name, uint8_t a, uint8_t b, int16_t g) {
    const int32_t k = info->nsurfs;
    info->nsurfs = k + 1;
    {
        const uint32_t n = crt_strlen(name) + 1;
        crt_copy(info->surfs[k].name, name, n);
    }
    info->surfs[k].type = a;
    info->surfs[k].b11 = b;
    info->surfs[k].group = g;
    info->surfs[k].v0 = (int16_t)info->nverts;
    info->surfs[k].v1 = (int16_t)info->nverts;
    info->surfs[k].t0 = (int16_t)info->ntris;
    info->surfs[k].t1 = (int16_t)info->ntris;
    return k;
}
static void fp_info_add_surface(Footprint& f, MrModelInfo* info, const char* name, uint8_t, uint8_t, int16_t) {
    f.add(info, sizeof(MrModelInfo), "info");
    f.add(&info->surfs[info->nsurfs], 0x20, "info surface");
    const uint32_t n = crt_strlen(name) + 1;
    if (n > 16) f.add(info->surfs[info->nsurfs].name, n, "info surface name (long)");
}
PORT_FN(0x004b89d0, "info_add_surface", info_add_surface_n, fp_info_add_surface)

// info_add_vertex: the index of the last surface's vertex with all of p, n, u, v equal (or NaN), else a vertex added
// (at its end)
static int32_t __cdecl info_add_vertex_n(MrModelInfo* info, const EbP3* p, const EbP3* n, float u, float v) {
    MrSurface* const S = (MrSurface*)((uint8_t*)info->surfs + info->nsurfs * 0x20 - 0x20);
    int32_t k = S->v0;
    const int32_t e = S->v1;
    if (k < e) {
        const double du = u, dv = v;
        for (; k < e; k++) {
            const MrVertex* const w = (const MrVertex*)((uint8_t*)info->verts + k * 0x20);
            if ((double)p->x < w->x || (double)p->x > w->x) continue;
            if ((double)p->y < w->y || (double)p->y > w->y) continue;
            if ((double)p->z < w->z || (double)p->z > w->z) continue;
            if ((double)w->nx < n->x || (double)w->nx > n->x) continue;
            if ((double)n->y < w->ny || (double)n->y > w->ny) continue;
            if ((double)n->z < w->nz || (double)n->z > w->nz) continue;
            if ((double)w->u < du || (double)w->u > du) continue;
            if (dv < w->v || dv > w->v) continue;
            return k;
        }
    }
    MrVertex* const w = (MrVertex*)((uint8_t*)info->verts + k * 0x20);
    eb_cp3(&w->x, &p->x);
    eb_cp3(&w->nx, &n->x);
    eb_put(&w->u, *(const volatile uint32_t*)&u);
    eb_put(&w->v, *(const volatile uint32_t*)&v);
    S->v1 = (int16_t)(k + 1);
    info->nverts = k + 1;
    return k;
}
static void fp_info_add_vertex(Footprint& f, MrModelInfo* info, const EbP3*, const EbP3*, float, float) {
    f.add(info, sizeof(MrModelInfo), "info");
    MrSurface* const S = (MrSurface*)((uint8_t*)info->surfs + info->nsurfs * 0x20 - 0x20);
    f.add((void*)&S->v1, 2, "info surface's end");
    const int32_t k = S->v0 >= S->v1 ? S->v0 : S->v1;
    f.add((uint8_t*)info->verts + k * 0x20, 0x20, "info vertex");
}
PORT_FN(0x004b8a60, "info_add_vertex", info_add_vertex_n, fp_info_add_vertex)

// info_add_triangle: a triangle at the last surface's end
static int32_t __cdecl info_add_triangle_n(MrModelInfo* info, int32_t a, int32_t b, int32_t c) {
    MrSurface* const S = (MrSurface*)((uint8_t*)info->surfs + info->nsurfs * 0x20 - 0x20);
    const int16_t k = S->t1;
    const int16_t k1 = (int16_t)(k + 1);
    S->t1 = k1;
    MrTriangle* const t = (MrTriangle*)((uint8_t*)info->tris + (int32_t)k * 8);
    info->ntris = k1;
    t->v[0] = (int16_t)a;
    t->v[1] = (int16_t)b;
    t->v[2] = (int16_t)c;
    return k;
}
static void fp_info_add_triangle(Footprint& f, MrModelInfo* info, int32_t, int32_t, int32_t) {
    f.add(info, sizeof(MrModelInfo), "info");
    MrSurface* const S = (MrSurface*)((uint8_t*)info->surfs + info->nsurfs * 0x20 - 0x20);
    f.add((void*)&S->t1, 2, "info surface's end");
    f.add((uint8_t*)info->tris + (int32_t)S->t1 * 8, 6, "info triangle");
}
PORT_FN(0x004b8b80, "info_add_triangle", info_add_triangle_n, fp_info_add_triangle)

// ModBuilderBuildGeometry: the builder as one surface `name`, each triangle with corners of its own (u, v (0, 0), (0, 1),
// (1, 0)) and its face's normal
static const MrModelInfo* __cdecl ModBuilderBuildGeometry_n(ModBuilder* mb, const char* name) {
    volatile float N[3];
    MrModelInfo* const info = &mb->info;
    info->ntris = 0;
    info->nverts = 0;
    info->nsurfs = 1;
    info->surfs->type = 0;
    info->surfs->b11 = 0;
    info->surfs->group = 0;
    info->surfs->t0 = 0;
    info->surfs->v0 = 0;
    {
        // FIX CANDIDATE: a name of 16 or more characters runs over the surface's fields
        const uint32_t n = crt_strlen(name) + 1;
        crt_copy(info->surfs->name, name, n);
    }
    for (int32_t i = 0; mb->ntris > i;) {
        const int32_t nt = info->ntris + 1;
        info->ntris = nt;
        MrTriangle* const t = (MrTriangle*)((uint8_t*)info->tris + nt * 8 - 8);
        const int32_t off = i * 0x10;
        t->v[0] = (int16_t)info->nverts;
        info->nverts = info->nverts + 1;
        MrVertex* const a = (MrVertex*)((uint8_t*)info->verts + (int32_t)t->v[0] * 0x20);
        eb_cp(&a->x, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[0] * 0x18))->x);
        eb_cp(&a->y, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[0] * 0x18))->y);
        eb_cp(&a->z, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[0] * 0x18))->z);
        eb_put(&a->u, 0);
        eb_put(&a->v, 0);
        t->v[1] = (int16_t)info->nverts;
        info->nverts = info->nverts + 1;
        MrVertex* const b = (MrVertex*)((uint8_t*)info->verts + (int32_t)t->v[1] * 0x20);
        eb_cp(&b->x, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[1] * 0x18))->x);
        eb_cp(&b->y, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[1] * 0x18))->y);
        eb_cp(&b->z, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[1] * 0x18))->z);
        eb_put(&b->u, 0);
        eb_put(&b->v, 0x3f800000);
        t->v[2] = (int16_t)info->nverts;
        info->nverts = info->nverts + 1;
        MrVertex* const c = (MrVertex*)((uint8_t*)info->verts + (int32_t)t->v[2] * 0x20);
        eb_cp(&c->x, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[2] * 0x18))->x);
        eb_cp(&c->y, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[2] * 0x18))->y);
        eb_cp(&c->z, &((const MbVertex*)((uint8_t*)mb->verts + ((const MbTriangle*)((uint8_t*)mb->tris + off))->v[2] * 0x18))->z);
        eb_put(&c->u, 0x3f800000);
        eb_put(&c->v, 0);
        get_face_normal(mb, N, i);
        i++;
        eb_cp3(&a->nx, N);
        eb_cp3(&b->nx, N);
        eb_cp3(&c->nx, N);
    }
    info->surfs->t1 = (int16_t)info->ntris;
    info->surfs->v1 = (int16_t)info->nverts;
    return info;
}
static void fp_ModBuilderBuildGeometry(Footprint& f, ModBuilder* mb, const char* name) {
    fp_builder(f, mb);
    f.add(mb->info.surfs, crt_strlen(name) + 1, "info surface name");
    f.add((void*)(uintptr_t)S_FACE_WARNED, 1, "get_face_normal's warning");
}
PORT_FN(0x004b8bd0, "ModBuilderBuildGeometry", ModBuilderBuildGeometry_n, fp_ModBuilderBuildGeometry)

// ==== the selection (texture coordinates as markers) ===========================================================================
static void __cdecl ModBuilderDeselectAll_n(MrModelInfo* info) {
    for (int32_t i = 0; info->ntris > i;) {
        const MrTriangle* const t = (const MrTriangle*)((uint8_t*)info->tris + i * 8);
        i++;
        const int32_t a = t->v[0], b = t->v[1], c = t->v[2];
        eb_put(&((MrVertex*)((uint8_t*)info->verts + a * 0x20))->u, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + a * 0x20))->v, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + b * 0x20))->u, 0x3f800000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + b * 0x20))->v, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + c * 0x20))->u, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + c * 0x20))->v, 0x3f800000);
    }
}
// the vertices the triangles name, as one span (the lowest to the highest)
static void fp_info_span(Footprint& f, MrModelInfo* info, int32_t t0, int32_t t1) {
    int32_t lo = 0x7fffffff, hi = -0x7fffffff;
    for (int32_t i = t0; i < t1; i++)
        for (int k = 0; k < 3; k++) {
            const int32_t v = info->tris[i].v[k];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    if (lo <= hi) f.add((uint8_t*)info->verts + lo * 0x20, (uint32_t)(hi - lo + 1) * 0x20, "info vertices");
}
static void fp_ModBuilderDeselectAll(Footprint& f, MrModelInfo* info) { fp_info_span(f, info, 0, info->ntris); }
PORT_FN(0x004b8e10, "ModBuilderDeselectAll", ModBuilderDeselectAll_n, fp_ModBuilderDeselectAll)

// ModBuilderSelectTriangle: the triangle's corners marked (mode 1, 2: selected; else as deselected)
static void __cdecl ModBuilderSelectTriangle_n(MrModelInfo* info, int32_t t, int32_t mode) {
    if (t < 0 || info->ntris <= t) return;
    const MrTriangle* const tr = (const MrTriangle*)((uint8_t*)info->tris + t * 8);
    const int32_t b = tr->v[1], a = tr->v[0], c = tr->v[2];
    MrVertex* const va = (MrVertex*)((uint8_t*)info->verts + a * 0x20);
    if (mode == 1) {
        eb_put(&va->u, 0x3f000000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + a * 0x20))->v, 0x3f000000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + b * 0x20))->u, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + b * 0x20))->v, 0x3f800000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + c * 0x20))->u, 0x3f800000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + c * 0x20))->v, 0x3f800000);
    } else if (mode == 2) {
        eb_put(&va->u, 0x3f000000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + a * 0x20))->v, 0x3f000000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + b * 0x20))->u, 0x3f800000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + b * 0x20))->v, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + c * 0x20))->u, 0x3f800000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + c * 0x20))->v, 0x3f800000);
    } else {
        eb_put(&va->u, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + a * 0x20))->v, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + b * 0x20))->u, 0x3f800000);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + b * 0x20))->v, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + c * 0x20))->u, 0);
        eb_put(&((MrVertex*)((uint8_t*)info->verts + c * 0x20))->v, 0x3f800000);
    }
}
static void fp_ModBuilderSelectTriangle(Footprint& f, MrModelInfo* info, int32_t t, int32_t) {
    if (t < 0 || info->ntris <= t) return;
    for (int k = 0; k < 3; k++) f.add((uint8_t*)info->verts + info->tris[t].v[k] * 0x20 + 0x18, 8, "info vertex u, v");
}
PORT_FN(0x004b8e90, "ModBuilderSelectTriangle", ModBuilderSelectTriangle_n, fp_ModBuilderSelectTriangle)

// ==== DXF =======================================================================================================================
// ModBuilderImportDXF: a DXF's 3DFACEs and polyface POLYLINEs into the builder (cleared; one surface first if asked)
static uint8_t __cdecl ModBuilderImportDXF_n(ModBuilder* mb, const char* name, uint8_t add_surface) {
    volatile int32_t fd = ccall<int32_t>(F_FileOpen, name);
    if (fd == 0) {
        EB_Log(EB_CP(0x004ff160), name);                                                        // "can't open %s"
        return 0;
    }
    ModBuilderClear(mb);
    if (add_surface) ModBuilderAddSurface(mb);
    while (dxf_next_group(fd)) {
        if (stricmp_(EB_CP(S_DXF_GROUP), EB_CP(0x004ff14c)) == 0) ccall<void>(F_do_polyline, mb, (int32_t)fd);   // "POLYLINE"
        else if (stricmp_(EB_CP(S_DXF_GROUP), EB_CP(0x004ff158)) == 0) ccall<void>(F_do_3dface, mb, (int32_t)fd); // "3DFACE"
    }
    ccall<void>(F_FileClose, (int32_t*)&fd);
    return 1;
}
static void fp_ModBuilderImportDXF(Footprint& f, ModBuilder*, const char*, uint8_t) { f.replay_only = READS; }
PORT_FN(0x004b8fa0, "ModBuilderImportDXF", ModBuilderImportDXF_n, fp_ModBuilderImportDXF)

// do_3dface: a 3DFACE's first three corners (x = -10, y = 30, z = -20 group codes; the fourth corner ignored) as a
// triangle, every edge smoothed
static void __cdecl do_3dface_n(ModBuilder* mb, int32_t fd) {
    volatile uint32_t fr[16];                          // F+0 z[3], +0xc y[3], +0x18 x[3], +0x24 the triangle, +0x34 a point
    for (int k = 0; k < 9; k++) fr[k] = 0;
    for (;;) {
        const uint32_t e = (uint32_t)(EB_G32(S_DXF_CODE) - 10);
        if (e <= 0x16) {
            // the original's jump table: 10..12 -> x, 20..22 -> z, 30..32 -> y, the rest nothing
            const uint32_t sel = e <= 2 ? 0 : (e >= 10 && e <= 12) ? 1 : (e >= 20 && e <= 22) ? 2 : 3;
            if (sel == 0) *(volatile float*)&fr[6 + EB_G32(S_DXF_CODE) % 10] = (float)-(double)EB_GF(S_DXF_FLOAT);
            else if (sel == 1) *(volatile float*)&fr[0 + EB_G32(S_DXF_CODE) % 10] = (float)-(double)EB_GF(S_DXF_FLOAT);
            else if (sel == 2) {
                const int32_t k = EB_G32(S_DXF_CODE) % 10;
                fr[3 + k] = EB_GU32(S_DXF_FLOAT);
            }
        }
        if (!dxf_next_item(fd)) break;
    }
    volatile uint8_t* const edges = (volatile uint8_t*)&fr[12];
    edges[2] = 0; edges[1] = 0; edges[0] = 0;                        // (the 16th byte never set)
    fr[9] = (uint32_t)ModBuilderAddVertex(mb, make_vertex(&fr[13], fr[6], fr[3], fr[0]));
    fr[10] = (uint32_t)ModBuilderAddVertex(mb, make_vertex(&fr[13], fr[7], fr[4], fr[1]));
    fr[11] = (uint32_t)ModBuilderAddVertex(mb, make_vertex(&fr[13], fr[8], fr[5], fr[2]));
    ModBuilderAddTriangle(mb, &fr[9]);
}
static void fp_dxf_mb(Footprint& f, ModBuilder*, int32_t) { f.replay_only = READS; }
PORT_FN(0x004b9070, "do_3dface", do_3dface_n, fp_dxf_mb)

// make_vertex: a DXF point to metres (x 0.01)
static EbP3* __cdecl make_vertex_n(EbP3* out, float x, float y, float z) {
    eb_st(&out->x, (double)x * (double)(float)0.009999999776482582f);
    eb_st(&out->y, (double)y * (double)(float)0.009999999776482582f);
    eb_st(&out->z, (double)z * (double)(float)0.009999999776482582f);
    return out;
}
static void fp_make_vertex(Footprint& f, EbP3* out, float, float, float) { f.add(out, 12, "out"); f.pure = true; }
PORT_FN(0x004b91e0, "make_vertex", make_vertex_n, fp_make_vertex)

// do_polyline: a polyface mesh (flag 64): its VERTEX records -- positions (flags 64; inches: x = -10, y = 20, z = 30)
// then faces (indices 71, 73, 72, from 1)
static void __cdecl do_polyline_n(ModBuilder* mb, int32_t fd) {
    // the original's frame (from F+0x10), so the triangle lies as deep in the stack as the original's: its edge flags are
    // never set -- whatever the stack held there (deep below the caller: the same in both, as far as the stack is the same)
    struct Frame {
        volatile int32_t a73, a72;                     // F+0x10 the face's corners (73, 72)
        int32_t _18[2];
        volatile float pz, py, px;                     // F+0x20 the vertex
        int32_t _2c[2];
        volatile uint32_t tri[4];                      // F+0x34
        volatile float pt[3];                          // F+0x44
        uint8_t _50[0x4000];
        volatile int32_t idx[0x1000];                  // F+0x4050 the vertices' builder indices
    };
    static_assert(offsetof(Frame, tri) == 0x24 && offsetof(Frame, idx) == 0x4040 && sizeof(Frame) == 0x8040, "do_polyline's frame");
    Frame fr;
    volatile int32_t* const idx = fr.idx;
    volatile uint32_t* const tri = fr.tri;
    volatile float* const pt = fr.pt;
    volatile float& px = fr.px;
    volatile float& py = fr.py;
    volatile float& pz = fr.pz;
    volatile int32_t& a73 = fr.a73;
    volatile int32_t& a72 = fr.a72;
    int32_t flags = 0, nverts = 0, nfaces = 0;
    do {
        const int32_t c = EB_G32(S_DXF_CODE);
        if (c == 0x46) flags = EB_G32(S_DXF_INT);
        else if (c == 0x47) nverts = EB_G32(S_DXF_INT);
        else if (c == 0x48) nfaces = EB_G32(S_DXF_INT);
    } while (dxf_next_item(fd));
    if (!(flags & 0x40)) {
        EB_Log(EB_CP(0x004ff170));                                                              // "Only polyface meshes supported!"
        return;
    }
    int32_t nv = 0;
    const int32_t total = nverts + nfaces;
    for (int32_t g = 0; g < total; g++) {
        if (!dxf_next_group(fd)) {
            EB_Log(EB_CP(0x004ff26c));                                                          // "Ran out out groups!"
            return;
        }
        if (stricmp_(EB_CP(S_DXF_GROUP), EB_CP(0x004ff190)) != 0) {                             // "VERTEX"
            EB_Log(EB_CP(0x004ff250), EB_CP(S_DXF_GROUP));                                      // "Expecting VERTEX, got %s"
            continue;
        }
        int32_t a71 = 0, vflags = 0;
        a73 = 0; a72 = 0;
        eb_put(&px, 0); eb_put(&py, 0); eb_put(&pz, 0);
        do {
            const uint32_t e = (uint32_t)(EB_G32(S_DXF_CODE) - 10);
            if (e <= 0x40) {
                // the original's jump table: 10 20 30 -> 0 1 2, 70..74 -> 3..7, the rest nothing
                const uint32_t sel = e == 0 ? 0 : e == 10 ? 1 : e == 20 ? 2 : (e >= 60 && e <= 64) ? e - 57 : 8;
                switch (sel) {
                case 0: eb_st(&px, (double)EB_GF(S_DXF_FLOAT) * (double)-(float)2.5399999618530273f); break;
                case 1: eb_st(&py, (double)EB_GF(S_DXF_FLOAT) * (double)(float)2.5399999618530273f); break;
                case 2: eb_st(&pz, (double)EB_GF(S_DXF_FLOAT) * (double)(float)2.5399999618530273f); break;
                case 3: vflags = EB_G32(S_DXF_INT); break;
                case 4: a71 = iabs(EB_G32(S_DXF_INT)); break;
                case 5: a72 = iabs(EB_G32(S_DXF_INT)); break;
                case 6: a73 = iabs(EB_G32(S_DXF_INT)); break;
                case 7: EB_Log(EB_CP(0x004ff198)); break;                                       // "Quad!!"
                default: break;
                }
            }
        } while (dxf_next_item(fd));
        if (!(vflags & 0x80)) EB_Log(EB_CP(0x004ff1a0));                                        // "Vertex not part of polyface mesh!"
        if (vflags & 0x40) {
            if (nverts <= nv) {
                EB_Log(EB_CP(0x004ff1c4), nv + 1, nverts);                                      // "Too many vertices: %d (expecting %d)"
                return;
            }
            volatile float* const p = make_vertex(pt, eb_bits(&px), eb_bits(&py), eb_bits(&pz));
            nv++;
            // FIX CANDIDATE: 4096 vertices at most (a polyface mesh of more overruns the table, on the stack)
            idx[nv - 1] = ModBuilderAddVertex(mb, p);
        } else {
            if (nfaces <= 0) {
                EB_Log(EB_CP(0x004ff1ec), 1, nfaces);                                           // "Too many faces: %d (expecting %d)"
                return;
            }
            if (a71 <= 0 || a73 <= 0 || a72 <= 0) {
                EB_Log(EB_CP(0x004ff22c));                                                      // "Vertex less than or equal to zero"
                return;
            }
            if (nv < a71 || nv < a73 || a72 > nv) {
                EB_Log(EB_CP(0x004ff210), nv);                                                  // "vertex out of range (%d)"
                return;
            }
            tri[0] = (uint32_t)idx[a71 - 1];
            tri[1] = (uint32_t)idx[a73 - 1];
            tri[2] = (uint32_t)idx[a72 - 1];
            // FIX CANDIDATE: tri[3] -- the edge flags and the 16th byte -- is never set: stack garbage (random faceting)
            ModBuilderAddTriangle(mb, tri);
        }
    }
}
PORT_FN(0x004b9210, "do_polyline", do_polyline_n, fp_dxf_mb)

// ==== ModBuilderImportMI / MOD / 3DS ===========================================================================================
// ModBuilderImportMI: an mrModelInfo's triangles into the builder (its vertices welded); a textured surface (a name) becomes
// a builder surface with texture points and triangles
static uint8_t __cdecl ModBuilderImportMI_n(ModBuilder* mb, MrModelInfo* info) {
    volatile uint32_t tri[4];                          // F+0x2c (the 16th byte never set)
    volatile float P[9];                               // F+0x3c, +0x48, +0x54
    volatile int32_t tp[3];
    for (int32_t s = 0; info->nsurfs > s; s++) {
        const int32_t soff = s * 0x20;
        int32_t sf = -1;
        const uint8_t textured = *(volatile uint8_t*)((uint8_t*)info->surfs + soff) != 0;
        if (textured) {
            sf = ModBuilderAddSurface(mb);
            ccall<void>(F_ModBuilderSetSurfaceTexture, mb, sf, (const char*)((uint8_t*)info->surfs + soff));
            const MrSurface* const S = (const MrSurface*)((uint8_t*)info->surfs + soff);
            ccall<void>(F_ModBuilderSetSurfaceMaterial, mb, sf, (uint32_t)S->type, (uint32_t)S->b11);
            ccall<void>(F_ModBuilderSetSurfaceGroup, mb, sf, (uint32_t)(uint16_t)((const MrSurface*)((uint8_t*)info->surfs + soff))->group);
        }
        int32_t t = ((const MrSurface*)((uint8_t*)info->surfs + soff))->t0;
        if (((const MrSurface*)((uint8_t*)info->surfs + soff))->t1 <= t) continue;
        do {
            const int32_t toff = t * 8;
            tp[0] = -1; tp[1] = -1;
            ((volatile uint8_t*)&tri[3])[2] = 0;
            ((volatile uint8_t*)&tri[3])[1] = 0;
            ((volatile uint8_t*)&tri[3])[0] = 0;
            tp[2] = -1;
            for (int k = 0; k < 3; k++) {
                const MrVertex* const v =
                    (const MrVertex*)((uint8_t*)info->verts + (int32_t)((const MrTriangle*)((uint8_t*)info->tris + toff))->v[k] * 0x20);
                eb_cp3(&P[k * 3], &v->x);
                tri[k] = (uint32_t)ModBuilderAddVertex(mb, &P[k * 3]);
            }
            if (textured) {
                for (int k = 0; k < 3; k++) {
                    const MrVertex* const v =
                        (const MrVertex*)((uint8_t*)info->verts + (int32_t)((const MrTriangle*)((uint8_t*)info->tris + toff))->v[k] * 0x20);
                    tp[k] = ModBuilderAddTexturePoint(mb, sf, (int32_t)tri[k], eb_bits(&v->u), eb_bits(&v->v));
                }
            }
            const int32_t r = ModBuilderAddTriangle(mb, tri);
            if (textured) ccall<int32_t>(F_ModBuilderAddTextureTriangle, mb, sf, r, (int32_t)tp[0], (int32_t)tp[1], (int32_t)tp[2]);
            t++;
        } while (((const MrSurface*)((uint8_t*)info->surfs + soff))->t1 > t);
    }
    return 1;
}
static void fp_ModBuilderImportMI(Footprint& f, ModBuilder*, MrModelInfo*) { f.replay_only = ALLOCS; }
PORT_FN(0x004b95a0, "ModBuilderImportMI", ModBuilderImportMI_n, fp_ModBuilderImportMI)

// ModBuilderImportMOD: the .mod file `name` (loaded as "*name": from the file, not a resource set) into the builder
static uint8_t __cdecl ModBuilderImportMOD_n(ModBuilder* mb, const char* name) {
    char buf[0x100];
    ModBuilderClear(mb);
    buf[0] = 0x2a;
    // FIX CANDIDATE: 254 characters of room for the name (the tool's file box)
    crt_copy(buf + 1, name, crt_strlen(name) + 1);
    MrModelInfo* const info = ccall<MrModelInfo*>(F_mrModelInfoGet, (const char*)buf);
    if (!info) return 0;
    if (ModBuilderImportMI(mb, info)) {
        ccall<void>(F_mrModelInfoForget, info);
        return 1;
    }
    ccall<void>(F_mrModelInfoForget, info);
    return 0;
}
static void fp_ModBuilderImportMOD(Footprint& f, ModBuilder*, const char*) { f.replay_only = READS; }
PORT_FN(0x004b9800, "ModBuilderImportMOD", ModBuilderImportMOD_n, fp_ModBuilderImportMOD)

// ModBuilderImport3DS: a .3ds read into an mrModelInfo on the stack (room for 4096 vertices and triangles, 64 surfaces),
// then imported (the builder cleared first)
static uint8_t __cdecl ModBuilderImport3DS_n(ModBuilder* mb, const char* name) {
    struct Frame {
        volatile uint32_t box[6];                       // F+8: min x, max x, min y, max y, min z, max z (never used)
        MrModelInfo info;                               // F+0x20
        MrVertex verts[0x1000];                         // F+0x48
        MrTriangle tris[0x1000];                        // F+0x20048
        MrSurface surfs[0x40];                          // F+0x28048
    };
    static_assert(offsetof(Frame, info) == 0x18 && offsetof(Frame, verts) == 0x40 && offsetof(Frame, tris) == 0x20040 &&
                  offsetof(Frame, surfs) == 0x28040 && sizeof(Frame) == 0x28840, "Import3DS's frame (from F+8)");
    Frame fr;
    {
        volatile uint32_t* z = (volatile uint32_t*)&fr.info;
        for (int k = 0; k < 10; k++) z[k] = 0;
    }
    fr.info.verts = fr.verts;
    fr.info.tris = fr.tris;
    fr.info.surfs = fr.surfs;
    // FIX CANDIDATE: Read3DS has no bound on the arrays (a .3ds of more than 4096 vertices or triangles, or 64 objects,
    // overruns this frame)
    if (!ccall<uint8_t>(F_Read3DS, &fr.info, name)) return 0;
    fr.box[0] = 0x461c4000; fr.box[1] = 0xc61c4000; fr.box[2] = 0x461c4000;
    fr.box[3] = 0xc61c4000; fr.box[4] = 0x461c4000; fr.box[5] = 0xc61c4000;
    if (fr.info.nverts > 0) {
        const MrVertex* v = fr.info.verts;
        for (int32_t n = fr.info.nverts; n != 0; n--, v++) {
            if (!((double)v->x >= *(volatile float*)&fr.box[0])) fr.box[0] = eb_bits(&v->x);
            if ((double)v->x > *(volatile float*)&fr.box[1]) fr.box[1] = eb_bits(&v->x);
            if (!((double)v->y >= *(volatile float*)&fr.box[2])) fr.box[2] = eb_bits(&v->y);
            if ((double)v->y > *(volatile float*)&fr.box[3]) fr.box[3] = eb_bits(&v->y);
            if (!((double)v->z >= *(volatile float*)&fr.box[4])) fr.box[4] = eb_bits(&v->z);
            if ((double)v->z > *(volatile float*)&fr.box[5]) fr.box[5] = eb_bits(&v->z);
        }
    }
    ModBuilderClear(mb);
    if (!ModBuilderImportMI(mb, &fr.info)) return 0;
    EB_Log(EB_CP(0x004ff280));                                                                  // "Success 3DS import"
    return 1;
}
static void fp_ModBuilderImport3DS(Footprint& f, ModBuilder*, const char*) { f.replay_only = READS; }
PORT_FN(0x004b9890, "ModBuilderImport3DS", ModBuilderImport3DS_n, fp_ModBuilderImport3DS)

// ==== normals ==================================================================================================================
// get_face_normal: triangle t's normal ((v1 - v0) x (v2 - v0), unit; (0, 1, 0) if its length's under 2^-23; left as the
// cross product, with a warning the first time, if its squared length is)
static void __cdecl get_face_normal_n(ModBuilder* mb, EbP3* n, int32_t t) {
    volatile float A[3], B[3], sq, up[3];
    const MbTriangle* const T = (const MbTriangle*)((uint8_t*)mb->tris + t * 0x10);
    const MbVertex* const V = mb->verts;
    {
        const MbVertex* const a = (const MbVertex*)((uint8_t*)V + T->v[0] * 0x18);
        const MbVertex* const b = (const MbVertex*)((uint8_t*)V + T->v[1] * 0x18);
        eb_st(&A[0], (double)b->x - a->x);
        eb_st(&A[1], (double)b->y - a->y);
        eb_st(&A[2], (double)b->z - a->z);
    }
    {
        const MbVertex* const a = (const MbVertex*)((uint8_t*)V + T->v[0] * 0x18);
        const MbVertex* const c = (const MbVertex*)((uint8_t*)V + T->v[2] * 0x18);
        eb_st(&B[0], (double)c->x - a->x);
        eb_st(&B[1], (double)c->y - a->y);
        eb_st(&B[2], (double)c->z - a->z);
    }
    eb_st(&n->x, (double)B[2] * A[1] - (double)B[1] * A[2]);
    eb_st(&n->y, (double)B[0] * A[2] - (double)B[2] * A[0]);
    eb_st(&n->z, (double)B[1] * A[0] - (double)B[0] * A[1]);
    const double s = ((double)n->x * n->x + (double)n->y * n->y) + (double)n->z * n->z;
    sq = (float)s;
    if (!(fabs(s) > EPS)) {
        if (EB_G8(S_FACE_WARNED) == 0) {
            EB_Log(EB_CP(0x004ff294));                                                          // "get_face_normal: degenerate triangle"
            EB_G8(S_FACE_WARNED) = 1;
        }
        return;
    }
    const double len = x87_sqrt((double)sq);
    eb_put(&up[1], 0x3f800000);
    eb_put(&up[2], 0);
    eb_put(&up[0], 0);
    if (!(fabs(len) > EPS)) {
        eb_cp3(&n->x, up);
        return;
    }
    scale_by_inverse_length(&n->x);
}
static void fp_get_face_normal(Footprint& f, ModBuilder*, EbP3* n, int32_t) {
    f.add(n, 12, "out");
    f.add((void*)(uintptr_t)S_FACE_WARNED, 1, "the warning");
}
PORT_FN(0x004b99f0, "get_face_normal", get_face_normal_n, fp_get_face_normal)

// edge_compare: is a-b the edge c-d (either way round)?
static uint8_t __cdecl edge_compare_n(int32_t a, int32_t b, int32_t c, int32_t d) {
    if (c == a && d == b) return 1;
    return d == a && c == b;
}
static void fp_edge_compare(Footprint& f, int32_t, int32_t, int32_t, int32_t) { f.pure = true; }
PORT_FN(0x004b9b80, "edge_compare", edge_compare_n, fp_edge_compare)

// make_vertex_normals: each vertex's normal: its triangles' face normals, each weighted by the sine of its corner's angle,
// summed and made unit length ((0, 1, 0) if too short). The corners are found by the vertex index, their positions read
// through the index's low 16 bits.
static void __cdecl make_vertex_normals_n(ModBuilder* mb) {
    volatile float S[3], FN[3], up[3], X[3], Y[3], E1[3], E2[3];
    volatile float w;
    if (mb->nverts <= 0) return;
    eb_put(&up[0], 0); eb_put(&up[1], 0x3f800000); eb_put(&up[2], 0);
    int32_t i = 0, voff = 0;
    do {
        eb_put(&S[0], 0); eb_put(&S[1], 0); eb_put(&S[2], 0);
        for (int32_t t = 0; mb->ntris > t; t++) {
            const MbTriangle* const T = (const MbTriangle*)((uint8_t*)mb->tris + t * 0x10);
            if (T->v[0] != i && T->v[1] != i && T->v[2] != i) continue;
            get_face_normal(mb, FN, t);
            const int16_t v0 = (int16_t)T->v[0];
            double d;
            if (i == T->v[0]) {                    // E1 = P1 - P0, E2 = P2 - P0
                const int16_t v1 = *(const volatile int16_t*)&T->v[1], v2 = *(const volatile int16_t*)&T->v[2];
                const MbVertex* const V = mb->verts;
                volatile float P2[3];
                eb_cp3(P2, &((const MbVertex*)((uint8_t*)V + v2 * 0x18))->x);
                eb_cp3(X, &((const MbVertex*)((uint8_t*)V + v1 * 0x18))->x);
                eb_cp3(Y, &((const MbVertex*)((uint8_t*)V + v0 * 0x18))->x);
                VectorSub(E1, X, Y);
                VectorNormalize(E1);
                VectorSub(E2, P2, Y);
                VectorNormalize(E2);
                d = DotProduct(E1, E2);
            } else if (i == T->v[1]) {             // E1 = P0 - P1, E2 = P2 - P1
                const int16_t v1 = (int16_t)T->v[1];
                const int16_t v2 = *(const volatile int16_t*)&T->v[2];
                const MbVertex* const V = mb->verts;
                volatile float P2[3];
                eb_cp3(P2, &((const MbVertex*)((uint8_t*)V + v2 * 0x18))->x);
                eb_cp3(X, &((const MbVertex*)((uint8_t*)V + v0 * 0x18))->x);
                eb_cp3(Y, &((const MbVertex*)((uint8_t*)V + v1 * 0x18))->x);
                VectorSub(E1, X, Y);
                VectorNormalize(E1);
                VectorSub(E2, P2, Y);
                VectorNormalize(E2);
                d = DotProduct(E1, E2);
            } else {                               // E1 = P0 - P2, E2 = P1 - P2
                const int16_t v1 = (int16_t)T->v[1];
                const int16_t v2 = *(const volatile int16_t*)&T->v[2];
                const MbVertex* const V = mb->verts;
                volatile float P1[3];
                eb_cp3(P1, &((const MbVertex*)((uint8_t*)V + v1 * 0x18))->x);
                eb_cp3(X, &((const MbVertex*)((uint8_t*)V + v0 * 0x18))->x);
                eb_cp3(Y, &((const MbVertex*)((uint8_t*)V + v2 * 0x18))->x);
                VectorSub(E1, X, Y);
                VectorNormalize(E1);
                VectorSub(E2, P1, Y);
                VectorNormalize(E2);
                d = DotProduct(E1, E2);
            }
            w = (float)x87_sqrt(1.0 - d * d);
            eb_st(&S[0], (double)w * FN[0] + S[0]);
            eb_st(&S[1], (double)w * FN[1] + S[1]);
            eb_st(&S[2], (double)w * FN[2] + S[2]);
        }
        unit_or(S, up);
        MbVertex* const v = (MbVertex*)((uint8_t*)mb->verts + voff);
        voff += 0x18;
        i++;
        eb_cp3(&v->nx, S);
    } while (mb->nverts > i);
}
static void fp_make_vertex_normals(Footprint& f, ModBuilder* mb) {
    f.add(mb->verts, (uint32_t)mb->maxverts * 0x18, "builder vertices");
    f.add((void*)(uintptr_t)S_FACE_WARNED, 1, "get_face_normal's warning");
}
PORT_FN(0x004b9bc0, "make_vertex_normals", make_vertex_normals_n, fp_make_vertex_normals)

// ==== the DXF reader: a group code line, then its value line ===================================================================
// dxf_next_group: on to the next entity (group code 0): its name (DXF_GROUP), then its first item
static uint8_t __cdecl dxf_next_group_n(int32_t fd) {
    while (EB_G32(S_DXF_CODE) != 0)
        if (!dxf_read_item(fd)) return 0;
    crt_copy((void*)(uintptr_t)S_DXF_GROUP, EB_CP(S_DXF_LINE), crt_strlen(EB_CP(S_DXF_LINE)) + 1);
    EB_G32(S_DXF_CODE) = -1;
    return dxf_next_item(fd);
}
static void fp_dxf(Footprint& f, int32_t) { f.replay_only = READS; }
PORT_FN(0x004ba000, "dxf_next_group", dxf_next_group_n, fp_dxf)

// dxf_read_item: a group code (DXF_CODE) and its value (DXF_LINE, and read as a float and as an integer)
static uint8_t __cdecl dxf_read_item_n(int32_t fd) {
    char buf[0x100];
    buf[0] = *(const volatile char*)(uintptr_t)0x004ff2bc;                                      // ""
    for (int k = 1; k < 0x100; k++) ((volatile char*)buf)[k] = 0;
    if (!ccall<uint8_t>(F_FileReadLine, fd, (char*)buf, 0x100)) {
        EB_Log(EB_CP(0x004ff300));                                                              // "Couldn't read type field"
        return 0;
    }
    if (EB_sscanf(buf, EB_CP(0x004ff2c0), (void*)(uintptr_t)S_DXF_CODE) != 1) {                // "%d"
        EB_Log(EB_CP(0x004ff2e8));                                                              // "Type field has no value"
        return 0;
    }
    if (!ccall<uint8_t>(F_FileReadLine, fd, (char*)(uintptr_t)S_DXF_LINE, 0x100)) {
        EB_Log(EB_CP(0x004ff2cc));                                                              // "Couldn't read second item"
        return 0;
    }
    EB_G32(S_DXF_FLOAT) = 0;
    EB_sscanf(EB_CP(S_DXF_LINE), EB_CP(0x004ff2c4), (void*)(uintptr_t)S_DXF_FLOAT);             // "%f"
    EB_G32(S_DXF_INT) = 0;
    EB_sscanf(EB_CP(S_DXF_LINE), EB_CP(0x004ff2c8), (void*)(uintptr_t)S_DXF_INT);               // "%d"
    return 1;
}
PORT_FN(0x004ba070, "dxf_read_item", dxf_read_item_n, fp_dxf)

// dxf_next_item: the next item of this entity (false at the next entity's code 0)
static uint8_t __cdecl dxf_next_item_n(int32_t fd) {
    if (EB_G32(S_DXF_CODE) == 0) return 0;
    return dxf_read_item(fd);
}
PORT_FN(0x004ba180, "dxf_next_item", dxf_next_item_n, fp_dxf)

// ModBuilderVertex::ModBuilderVertex: nothing (returns this)
static void* __fastcall ModBuilderVertex_ctor_n(void* self, Edx) { return self; }
static void fp_ModBuilderVertex_ctor(Footprint& f, void*, Edx) { f.pure = true; }
PORT_FN(0x004ba1a0, "ModBuilderVertex::ModBuilderVertex", ModBuilderVertex_ctor_n, fp_ModBuilderVertex_ctor)

}  // namespace edit_build
}  // namespace
