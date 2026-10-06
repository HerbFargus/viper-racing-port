// wld_terrain.cpp -- M3 "the world", group W1: the terrain queries, rewritten. world:terrain.obj (the
// TerrainGet* family the physics asks every tick), world:bpp.obj (the collision tree over the XZ plane and its
// loader), world:bsp.obj (the 3D BSP and its sphere query) and world:track.obj (loading a track's three files).
//
// Written from the v1.0 disassembly, faithful (no fixes): the same sums in the same grouping, register values as
// double and stored values as float, every call made by address in the original's order, float moves the original
// does with integer instructions as bit copies, and each comparison with the original's NaN outcome -- including
// the ones it makes on a float's bits as an integer (`cmp dword [x], 0; jg`, `cmp dword [x], 0x80000000; ja`).
//
// Layouts (offsets from the disassembly; vrmod's bpp.py documents the .bpp file the same way):
//   bpp_header (a 'BPPT' resource, version 2; bpp.obj's `header` 0x4f50c0) +0 triangle count, +4 node count,
//     +8 the triangles, +0xc the nodes, +0x10 the root node -- in the file +8/+0xc are 0 and +0x10 the root's
//     index; load_file makes all three pointers: triangles at +0x14, nodes after them, the root among them.
//   bpp_tri (56)   +0 normal, +0xc d (n.v + d = 0), +0x10/+0x1c/+0x28 the vertices, +0x34 the surface code
//     (int16, sign-extended: 0 road, 10 grass, 14 water, ...), +0x36 unused.
//   bpp_node (28)  +0 a, +4 b, +8 c: the line a*x + b*z + c over XZ; +0xc the triangle on the <= side and +0x10
//     on the > side (read where that side's child is null: a leaf is not a node); +0x14 child on the <= side,
//     +0x18 on the > side (in the file node indices, -1 none; fixup_tree makes them pointers, null none).
//   BPPFinder (20, a segment query on the stack of BPPHit) +0 the triangle being tested (index), +4 the output
//     point, +8 the triangle hit, +0xc the segment's start, +0x10 its end.
//   bsp_node (12; bsp.obj's `head` 0x4f5348, a 'BSPT' resource, version 0) +0 its triangle, +4 the child in
//     front of the triangle's plane, +8 behind it (in the file byte offsets from the start, 0 none; fixup_nodes
//     adds the base).
//   bsp_tri (0x5c used) +0 visited (intersect_tree's mark), +4 normal, +0x10 d, +0x14/+0x20/+0x2c the vertices,
//     +0x38/+0x44/+0x50 each edge's outward normal (edge i runs from vertex i to i+1).
//   Every stock track's track.bsp is the same 108 bytes: one node, one triangle 200 km across at y = 0.
//
// Who calls what (v1.0): the BPP side is the ground. TerrainGetSphereIntersection (the sphere and cube volumes),
// TerrainGetHeight(x, z, point, normal, surface) (Wheel::Update, update_camera), TerrainGetIntersection with a
// surface (Corner::Update) and TerrainGetHeight(x, z) (the AI, Car::SuperHelpOut, RaceDeity::TeleportToLine) run
// on the physics thread; the same queries also run on the main thread -- TerrainGetHeight(x, z) for the map and
// debug overlays (draw_status, draw_mouse_cursor, project, IdealLine::Draw) and when a world is set up (parse_car,
// parse_obstacle, parse_checkpoint, create_ball, set_car_frame_from_dlatlong), TerrainGetHeight(point) for the
// shadows, TerrainGetIntersection for the reflections and draw_physics_tris. So these list the one static they
// write -- bpp_find's call counter, 0x4f50c8 -- in their footprints (a shadow check on the main thread can see the
// physics thread bump it at the same moment: a debug counter nothing reads). Nothing calls the BSP queries
// (BSPHit, BSPHitSphere) and nothing but itself calls intersect_tree: dead in v1.0, rewritten all the same and
// checked offline (test/world_wld_terrain.cpp). Their statics (debug counters, the hit list, the best sphere
// contact) are in bsp.obj's .bss and are listed in the footprints. TerrainBegin / TerrainEnd (a profiler timer),
// TrackLoad / TrackUnload / track_begin / track_end and the loaders run on the main thread and are replay_only.
//
// Not rewritten: TrackDraw (draws: the graphics stage), bpp_test (a bare `ret`), the $E initialisers.
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

static __forceinline int32_t I(const float& x) { int32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline uint32_t Ub(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }      // an integer move of a float
// An fst / fstp dword the original always makes, kept even where the float then goes unused: rounding a register
// value past FLT_MAX into a float is an overflow, which the physics thread unmasks (a fault), so the store has to
// happen where the original's does. (The compiler would drop a dead conversion.)
static __forceinline float st(double d) {
    volatile float f = (float)d;
    return f;
}
static __forceinline void cp12(void* d, const void* s) {                        // three of them, x, y, z
    const uint32_t* a = (const uint32_t*)s;
    uint32_t* b = (uint32_t*)d;
    uint32_t x = a[0], y = a[1], z = a[2];
    b[0] = x;
    b[1] = y;
    b[2] = z;
}

// ---- the structures -------------------------------------------------------------------------------------------
struct bpp_tri {
    P3 n;                              // +0
    float d;                           // +0xc
    P3 v[3];                           // +0x10, +0x1c, +0x28
    int16_t surface;                   // +0x34
    uint16_t _36;
};
static_assert(sizeof(bpp_tri) == 56 && offsetof(bpp_tri, surface) == 0x34, "bpp_tri");

struct bpp_node {
    float a, b, c;                     // +0 a*x + b*z + c
    int32_t tri_le, tri_gt;            // +0xc, +0x10
    bpp_node* le;                      // +0x14
    bpp_node* gt;                      // +0x18
};
static_assert(sizeof(bpp_node) == 28, "bpp_node");

struct bpp_header {
    int32_t n_tris, n_nodes;           // +0, +4
    bpp_tri* tris;                     // +8
    bpp_node* nodes;                   // +0xc
    bpp_node* root;                    // +0x10 (the root's index, in the file)
};
static_assert(sizeof(bpp_header) == 0x14, "bpp_header");

struct BPPFinder {
    int32_t tri;                       // +0
    P3* out;                           // +4
    bpp_tri* hit;                      // +8
    P3* p0;                            // +0xc
    P3* p1;                            // +0x10
};
static_assert(sizeof(BPPFinder) == 20, "BPPFinder");

struct bsp_tri {
    uint8_t visited, _1[3];            // +0
    P3 n;                              // +4
    float d;                           // +0x10
    P3 v[3];                           // +0x14, +0x20, +0x2c
    P3 e[3];                           // +0x38, +0x44, +0x50
};
static_assert(sizeof(bsp_tri) == 0x5c && offsetof(bsp_tri, e) == 0x38, "bsp_tri");

struct bsp_node {
    bsp_tri* tri;                      // +0
    bsp_node* front;                   // +4
    bsp_node* back;                    // +8
};
static_assert(sizeof(bsp_node) == 12, "bsp_node");

// ---- statics (v1.0) --------------------------------------------------------------------------------------------
#define G(T, a) (*(T*)(uintptr_t)(a))
enum : uint32_t {
    S_TERRAIN_TIMER = 0x004f4c78,     // int: TerrainBegin's profiler timer
    S_BPP_TYPE = 0x004f50b8,          // 'BPPT'
    S_BPP_VER = 0x004f50bc,           // 2
    S_BPP_HEADER = 0x004f50c0,        // bpp_header* header
    S_BPP_FINDS = 0x004f50c8,         // int: bpp_find calls (a debug counter)
    S_TRACK_GRAF = 0x00557fc8,        // int: track_begin's GrafLoad handle
    S_BSP_TYPE = 0x004f5340,          // 'BSPT'
    S_BSP_VER = 0x004f5344,           // 0
    S_BSP_HEAD = 0x004f5348,          // bsp_node* head
    // bsp.obj's debug counters and query state, 0x4f534c..0x4f538b (16 ints):
    S_BSP_LISTED = 0x004f534c,        // intersect_tree's visited list: the count
    S_BSP_HITS = 0x004f5350,          // BSPHit calls
    S_BSP_LISTED_SUM = 0x004f5354,    // total listed
    S_BSP_CROSSED = 0x004f5358,       // segments split at a plane
    S_BSP_ONE_SIDE = 0x004f535c,      // nodes passed on one side
    S_BSP_ON_PLANE = 0x004f5360,      // an endpoint on a plane
    S_SPH_CALLS = 0x004f5364,         // BSPHitSphere calls
    S_SPH_NODES_SUM = 0x004f5368,
    S_SPH_NODES_MAX = 0x004f536c,
    S_SPH_HITS_MAX = 0x004f5370,
    S_SPH_HITS = 0x004f5374,          // this query's add_intersection calls
    S_SPH_CALLS_HIT = 0x004f5378,
    S_SPH_HITS_SUM = 0x004f537c,
    S_SPH_NODES = 0x004f5380,         // this query's nodes
    S_SPH_TRIS = 0x004f5384,          // this query's triangles tested
    S_SPH_TRIS_SUM = 0x004f5388,
    S_SPH_BEST_N = 0x00559dc8,        // P3: the nearest contact's normal
    S_BSP_LIST = 0x00559de0,          // bsp_tri*[64]: intersect_tree's visited triangles
    S_BSP_OUT = 0x00559eec,           // P3*: BSPHit's output point
    S_BSP_HIT = 0x00559ef0,           // bsp_tri*: BSPHit's triangle
    S_SPH_BEST_D2 = 0x00559ef4,       // float: the nearest contact's squared distance
    S_SPH_BEST_P = 0x00559f18,        // P3: the nearest contact
    S_SPH_BEST_TRI = 0x00559f28,      // bsp_tri*: its triangle
};
// the game's strings
static const char* const STR_TERRAIN = (const char*)0x004f4c7c;         // "Terrain"
static const char* const STR_NFIELD = (const char*)0x004f4ee8;          // "nfield"
static const char* const STR_HEAVEN = (const char*)0x004f4ef0;          // "heaven"
static const char* const STR_TRACK_GRF = (const char*)0x004f4ef8;       // "track.grf"
static const char* const STR_TRACK_BSP = (const char*)0x004f4f04;       // "track.bsp"
static const char* const STR_TRACK_BPP = (const char*)0x004f4f10;       // "track.bpp"
static const char* const STR_BAD_BPP_VER = (const char*)0x004f50d0;     // "bad bpp version"
static const char* const STR_BPP_NOT_FOUND = (const char*)0x004f50e0;   // "bpp not found: %s"
static const char* const STR_BAD_BSP_VER = (const char*)0x004f53d4;     // "bad bsp version"
static const char* const STR_BSP_NOT_FOUND = (const char*)0x004f53e4;   // "bsp not found: %s"
static const char* const STR_BAD_NODE = (const char*)0x004f538c;        // "bad node"
static const char* const STR_OVERFLOW = (const char*)0x004f5398;        // "possible overflow"
static const char* const STR_BANG = (const char*)0x004f53ac;            // "!"
static const char* const STR_IMPOSSIBLE = (const char*)0x004f53b0;      // "impossible! no sphere intersection"

// ---- the game's functions these call ---------------------------------------------------------------------------
typedef int(__cdecl* MultiBegin_t)(const char*);
typedef void(__cdecl* MultiEnd_t)(int, const char*, int);
typedef bpp_tri*(__cdecl* BPPHitXZ_t)(P3*, P3*);
typedef bpp_tri*(__cdecl* BPPHit_t)(P3*, P3*, P3*);
typedef uint8_t(__cdecl* TerrainGetHeightPS_t)(P3*, int32_t*);
typedef uint8_t(__cdecl* TerrainGetHeight5_t)(uint32_t x, uint32_t z, P3*, P3*, int32_t*);
typedef uint8_t(__cdecl* GetTerrainDisplacement_t)(const P3*, const P3*, P3*, P3*, int);
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
typedef void(__cdecl* ViewSetSky_t)(uint32_t, uint32_t);
typedef void(__cdecl* Void_t)();
typedef void(__cdecl* GrafSetMinLOD_t)(uint32_t);
typedef int(__cdecl* GrafLoad_t)(const char*);
typedef void(__cdecl* GrafUnload_t)(int);
typedef void(__cdecl* Load_t)(const char*);
typedef void*(__cdecl* ResourceGet_t)(const char*, uint32_t type, uint32_t* version, int32_t* size, uint8_t*,
                                      uint8_t* fresh);
typedef uint8_t(__cdecl* ResourceForget_t)(void*);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef bpp_header*(__cdecl* BppLoadFile_t)(const char*);
typedef void(__cdecl* FixupTree_t)(bpp_node*, bpp_node*, int);
typedef bpp_tri*(__fastcall* BPPFind_t)(BPPFinder*, Edx);
typedef uint8_t(__fastcall* BPPTest_t)(BPPFinder*, Edx);
typedef uint8_t(__fastcall* BPPTestPoly_t)(BPPFinder*, Edx, int32_t);
typedef int32_t(__fastcall* BPPFindNode_t)(BPPFinder*, Edx, bpp_node*);
typedef int32_t(__cdecl* BppFindPoint_t)(P3*);
typedef bsp_node*(__cdecl* BspLoadFile_t)(const char*);
typedef void(__cdecl* FixupNodes_t)(bsp_node*, int32_t);
typedef uint8_t(__cdecl* BspSegment_t)(P3*, P3*, bsp_node*);
typedef uint8_t(__cdecl* BspPointInPoly_t)(P3*, bsp_tri*);
typedef uint8_t(__cdecl* BspIntersectPlane_t)(P3*, P3*, bsp_tri*, P3*, float*);
typedef uint8_t(__cdecl* SphereTree_t)(P3*, uint32_t r, bsp_node*);
typedef void(__cdecl* CalcSphere_t)(P3*, uint32_t r, bsp_tri*);
typedef void(__cdecl* AddIntersection_t)(P3*, bsp_tri*, P3*, P3*);
typedef void(__cdecl* ProjectToPlane_t)(P3*, P3*, P3*);
typedef void(__cdecl* VectorSub_t)(P3*, const P3*, const P3*);
typedef double(__cdecl* DotProduct_t)(const P3*, const P3*);                  // ST0
typedef void(__cdecl* VectorAddScaled_t)(P3*, const P3*, const P3*, float);
typedef double(__cdecl* VectorLength_t)(const P3*);                           // ST0

#define FN(T, a) ((T)(uintptr_t)(a))
#define MultiBegin FN(MultiBegin_t, 0x004150a0)
#define MultiEnd FN(MultiEnd_t, 0x00415220)
#define TerrainGetHeightPS FN(TerrainGetHeightPS_t, 0x00465b50)
#define TerrainGetHeight5 FN(TerrainGetHeight5_t, 0x00465c40)
#define GetTerrainDisplacement FN(GetTerrainDisplacement_t, 0x00465d80)
#define Stricmp FN(Stricmp_t, 0x004da350)
#define ViewSetSky FN(ViewSetSky_t, 0x00466870)
#define TrackBegin FN(Void_t, 0x00469590)
#define TrackEnd FN(Void_t, 0x004695d0)
#define GrafSetMinLOD FN(GrafSetMinLOD_t, 0x0046dfb0)
#define GrafLoad FN(GrafLoad_t, 0x0046db30)
#define GrafUnload FN(GrafUnload_t, 0x0046dc20)
#define BSPLoad FN(Load_t, 0x0046fb50)
#define BSPUnload FN(Void_t, 0x0046fbc0)
#define BPPLoad FN(Load_t, 0x0046d070)
#define BPPUnload FN(Void_t, 0x0046d090)
#define ResourceGet FN(ResourceGet_t, 0x00419fa0)
#define ResourceForget FN(ResourceForget_t, 0x0041a450)
#define LogReport FN(Log_t, 0x00411150)
#define LogPanic FN(Log_t, 0x004112b0)
#define BppLoadFile FN(BppLoadFile_t, 0x0046d540)
#define BppUnloadFile FN(Void_t, 0x0046d600)
#define BppTest FN(Void_t, 0x0046d8e0)
#define FixupTree FN(FixupTree_t, 0x0046d610)
#define BPPFinderFind FN(BPPFind_t, 0x0046d0a0)
#define BPPHitFn FN(BPPHit_t, 0x0046d0c0)
#define BPPFinderIntersectPlane FN(BPPTest_t, 0x0046d0f0)
#define BPPFinderPointInPoly FN(BPPTest_t, 0x0046d290)
#define BPPFinderTestPoly FN(BPPTestPoly_t, 0x0046d360)
#define BPPFinderBppFind FN(BPPFindNode_t, 0x0046d390)
#define BppFindPoint FN(BppFindPoint_t, 0x0046d740)
#define BPPHitXZFn FN(BPPHitXZ_t, 0x0046d790)
#define BspLoadFile FN(BspLoadFile_t, 0x00470be0)
#define BspUnloadFile FN(Void_t, 0x00470c60)
#define FixupNodes FN(FixupNodes_t, 0x0046fb70)
#define IntersectTree FN(BspSegment_t, 0x0046fc40)
#define IntersectAll FN(BspSegment_t, 0x0046ff30)
#define BspPointInPoly FN(BspPointInPoly_t, 0x00470240)
#define BspIntersectPlane FN(BspIntersectPlane_t, 0x004702e0)
#define SphereIntersectTree FN(SphereTree_t, 0x00470560)
#define CalcSphereIntersection FN(CalcSphere_t, 0x00470620)
#define AddIntersection FN(AddIntersection_t, 0x00470b40)
#define ProjectToPlane FN(ProjectToPlane_t, 0x00470c70)
#define VectorSub FN(VectorSub_t, 0x00429120)
#define DotProduct FN(DotProduct_t, 0x0045d8e0)
#define VectorAddScaled FN(VectorAddScaled_t, 0x0045d900)
#define VectorLength FN(VectorLength_t, 0x00429100)

static inline bpp_header* bpp() { return G(bpp_header*, S_BPP_HEADER); }

// |d| < 0.001 (or a NaN) counts as on the line/plane: the float's bits become 0 (`fst; fabs; fcomp 0.001f;
// test ah,1; je keep; mov dword [d], 0`). What's returned is the stored float's bits, which the callers compare
// as integers.
static __forceinline uint32_t snap(double d) {
    float f = (float)d;
    return fabs(d) >= (float)0.001f ? Ub(f) : 0u;
}

static void fp_bpp_finds(Footprint& f) { f.add((void*)(uintptr_t)S_BPP_FINDS, 4, "bpp_find calls (0x4f50c8)"); }

// ==== terrain.obj ================================================================================================

// ---- TerrainBegin (0x465aa0) / TerrainEnd (0x465ac0): a profiler timer named "Terrain" (main thread, ViewBegin
// and ViewEnd) ----------------------------------------------------------------------------------------------------
static void __cdecl TerrainBegin() { G(int32_t, S_TERRAIN_TIMER) = MultiBegin(STR_TERRAIN); }
static void fp_terrain_begin(Footprint& f) { f.replay_only = "registers a profiler timer (MultiBegin)"; }
PORT_FN(0x00465aa0, "TerrainBegin", TerrainBegin, fp_terrain_begin)

static void __cdecl TerrainEnd() { MultiEnd(G(int32_t, S_TERRAIN_TIMER), 0, 0); }
static void fp_terrain_end(Footprint& f) { f.replay_only = "ends a profiler timer (_MultiEnd)"; }
PORT_FN(0x00465ac0, "TerrainEnd", TerrainEnd, fp_terrain_end)

// ---- TerrainGetHeight(x, z) (0x465ae0): the ground's height under (x, z), 0 off the map -------------------------
// The query point is (x, 1000, z); the height comes back through BPPHitXZ's output and is returned from memory
// (fld dword), so a float return is the same ST0.
static float __cdecl TerrainGetHeightXZ(float x, float z) {
    P3 p, out;
    cp4(&p.x, &x);
    const uint32_t k1000 = 0x447a0000;                     // 1000.0f, an integer move
    cp4(&p.y, &k1000);
    cp4(&p.z, &z);
    if (BPPHitXZFn(&p, &out)) return out.y;
    return 0.0f;
}
static void fp_terrain_height_xz(Footprint&, float, float) {}
PORT_FN(0x00465ae0, "TerrainGetHeight(x,z)", TerrainGetHeightXZ, fp_terrain_height_xz)

// ---- TerrainGetHeight(point) (0x465b30): the (point, surface) overload with a surface nobody reads -------------
static uint8_t __cdecl TerrainGetHeightP(P3* p) {
    int32_t surface;
    return TerrainGetHeightPS(p, &surface);
}
static void fp_terrain_height_p(Footprint& f, P3* p) { f.add(&p->y, 4, "point.y"); }
PORT_FN(0x00465b30, "TerrainGetHeight(point)", TerrainGetHeightP, fp_terrain_height_p)

// ---- TerrainGetHeight(point, surface) (0x465b50): the point's y set to the ground's height --------------------
static uint8_t __cdecl TerrainGetHeightPSurface(P3* p, int32_t* surface) {
    P3 out;
    bpp_tri* t = BPPHitXZFn(p, &out);
    if (!t) return 0;
    cp4(&p->y, &out.y);
    *surface = t->surface;
    return 1;
}
static void fp_terrain_height_ps(Footprint& f, P3* p, int32_t* surface) {
    f.add(&p->y, 4, "point.y");
    f.add(surface, 4, "surface");
}
PORT_FN(0x00465b50, "TerrainGetHeight(point,surface)", TerrainGetHeightPSurface, fp_terrain_height_ps)

// ---- TerrainGetIntersection (0x465b90, 0x465be0): where the segment start -> end first meets the ground --------
// The hit point comes from BPPHit; the normal is the triangle's (bit copies); get_terrain_displacement (a stub
// that returns 1) has the last word.
static uint8_t __cdecl TerrainGetIntersection4(P3* start, P3* end, P3* hit, P3* normal) {
    bpp_tri* t = BPPHitFn(start, end, hit);
    if (!t) return 0;
    cp12(normal, &t->n);
    return GetTerrainDisplacement(start, end, hit, normal, t->surface);
}
static void fp_terrain_intersection4(Footprint& f, P3*, P3*, P3* hit, P3* normal) {
    f.add(hit, 12, "hit");
    f.add(normal, 12, "normal");
    fp_bpp_finds(f);
}
PORT_FN(0x00465b90, "TerrainGetIntersection(start,end,hit,normal)", TerrainGetIntersection4, fp_terrain_intersection4)

static uint8_t __cdecl TerrainGetIntersection5(P3* start, P3* end, P3* hit, P3* normal, int32_t* surface) {
    bpp_tri* t = BPPHitFn(start, end, hit);
    if (!t) return 0;
    cp12(normal, &t->n);
    if (surface) *surface = t->surface;
    return GetTerrainDisplacement(start, end, hit, normal, t->surface);
}
static void fp_terrain_intersection5(Footprint& f, P3*, P3*, P3* hit, P3* normal, int32_t* surface) {
    f.add(hit, 12, "hit");
    f.add(normal, 12, "normal");
    if (surface) f.add(surface, 4, "surface");
    fp_bpp_finds(f);
}
PORT_FN(0x00465be0, "TerrainGetIntersection(start,end,hit,normal,surface)", TerrainGetIntersection5,
        fp_terrain_intersection5)

// ---- TerrainGetHeight(x, z, point, normal, surface) (0x465c40): the ground under (x, z) -----------------------
// The query point is (x, 0, z); BPPHitXZ writes the ground point straight into `point`.
static uint8_t __cdecl TerrainGetHeightXZPNS(float x, float z, P3* point, P3* normal, int32_t* surface) {
    P3 p;
    cp4(&p.x, &x);
    const uint32_t zero = 0;
    cp4(&p.y, &zero);
    cp4(&p.z, &z);
    bpp_tri* t = BPPHitXZFn(&p, point);
    if (!t) return 0;
    cp12(normal, &t->n);
    *surface = t->surface;
    return 1;
}
static void fp_terrain_height5(Footprint& f, float, float, P3* point, P3* normal, int32_t* surface) {
    f.add(point, 12, "point");
    f.add(normal, 12, "normal");
    f.add(surface, 4, "surface");
}
PORT_FN(0x00465c40, "TerrainGetHeight(x,z,point,normal,surface)", TerrainGetHeightXZPNS, fp_terrain_height5)

// ---- TerrainGetSphereIntersection (0x465ca0): a ball of radius r at c against the ground under its centre ------
// s = the centre's height over the ground's plane along the normal; a hit when |s| < r (a NaN s hits too), the
// contact point c - sN. The query writes the ground point over its own copy of c, and s is stored into that
// copy's x (dead).
static uint8_t __cdecl TerrainGetSphereIntersection(P3* c, float r, P3* point, P3* normal, int32_t* surface) {
    P3 Q, N;
    cp12(&Q, c);
    int32_t surf = 0;
    if (!TerrainGetHeight5(Ub(Q.x), Ub(Q.z), &Q, &N, &surf)) return 0;
    double s = ((D(c->y) - Q.y) * N.y + (D(c->z) - Q.z) * N.z) + (D(c->x) - Q.x) * N.x;
    float sf = st(s);                                       // fst [Q.x]
    if (fabs(s) >= r) return 0;                             // fcomp r; test ah,1; je: not less, and ordered
    point->x = (float)(D(c->x) - D(sf) * N.x);
    point->y = (float)(D(c->y) - D(sf) * N.y);
    point->z = (float)(D(c->z) - D(sf) * N.z);
    cp12(normal, &N);
    *surface = surf;
    return 1;
}
static void fp_terrain_sphere(Footprint& f, P3*, float, P3* point, P3* normal, int32_t* surface) {
    f.add(point, 12, "point");
    f.add(normal, 12, "normal");
    f.add(surface, 4, "surface");
}
PORT_FN(0x00465ca0, "TerrainGetSphereIntersection", TerrainGetSphereIntersection, fp_terrain_sphere)

// ---- get_terrain_displacement (0x465d80): `mov al, 1; ret` ------------------------------------------------------
static uint8_t __cdecl get_terrain_displacement(const P3*, const P3*, P3*, P3*, int) { return 1; }
static void fp_get_terrain_displacement(Footprint& f, const P3*, const P3*, P3*, P3*, int) { f.pure = true; }
PORT_FN(0x00465d80, "get_terrain_displacement", get_terrain_displacement, fp_get_terrain_displacement)

// ==== track.obj ==================================================================================================

// ---- TrackLoad (0x469500): the sky for the track (ViewSetSky(low, 200): nfield -75, heaven -40, the rest -25),
// then the track's files. Always succeeds.
static uint8_t __cdecl TrackLoad(const char* name) {
    if (!Stricmp(name, STR_NFIELD)) {
        ViewSetSky(0xc2960000 /* -75.0f */, 0x43480000 /* 200.0f */);
        TrackBegin();
        return 1;
    }
    if (!Stricmp(name, STR_HEAVEN)) ViewSetSky(0xc2200000 /* -40.0f */, 0x43480000);
    else ViewSetSky(0xc1c80000 /* -25.0f */, 0x43480000);
    TrackBegin();
    return 1;
}
static void fp_track_load(Footprint& f, const char*) { f.replay_only = "loads the track's files (GrafLoad, BSPLoad, BPPLoad)"; }
PORT_FN(0x00469500, "TrackLoad", TrackLoad, fp_track_load)

// ---- TrackUnload (0x469580): track_end ---------------------------------------------------------------------
static void __cdecl TrackUnload() { TrackEnd(); }
static void fp_track_unload(Footprint& f) { f.replay_only = "frees the track's files"; }
PORT_FN(0x00469580, "TrackUnload", TrackUnload, fp_track_unload)

// ---- track_begin (0x469590): the render LOD, then track.grf, track.bsp, track.bpp ------------------------------
static void __cdecl track_begin() {
    GrafSetMinLOD(0x40000000);                             // 2.0f
    G(int32_t, S_TRACK_GRAF) = GrafLoad(STR_TRACK_GRF);
    BSPLoad(STR_TRACK_BSP);
    BPPLoad(STR_TRACK_BPP);
}
static void fp_track_begin(Footprint& f) { f.replay_only = "loads the track's files"; }
PORT_FN(0x00469590, "track_begin", track_begin, fp_track_begin)

// ---- track_end (0x4695d0) --------------------------------------------------------------------------------------
static void __cdecl track_end() {
    BPPUnload();
    BSPUnload();
    GrafUnload(G(int32_t, S_TRACK_GRAF));
}
static void fp_track_end(Footprint& f) { f.replay_only = "frees the track's files"; }
PORT_FN(0x004695d0, "track_end", track_end, fp_track_end)

// ==== bpp.obj: the collision tree ================================================================================

// ---- BPPLoad (0x46d070) / BPPUnload (0x46d090) -----------------------------------------------------------------
static void __cdecl BPPLoadRw(const char* name) {
    G(bpp_header*, S_BPP_HEADER) = BppLoadFile(name);
    BppTest();                                             // a bare ret
}
static void fp_bpp_load(Footprint& f, const char*) { f.replay_only = "loads a resource (ResourceGet)"; }
PORT_FN(0x0046d070, "BPPLoad", BPPLoadRw, fp_bpp_load)

static void __cdecl BPPUnloadRw() { BppUnloadFile(); }
static void fp_bpp_unload(Footprint& f) { f.replay_only = "frees a resource (ResourceForget)"; }
PORT_FN(0x0046d090, "BPPUnload", BPPUnloadRw, fp_bpp_unload)

static void fp_finder(Footprint& f, BPPFinder* self) {
    f.add(self, sizeof(BPPFinder), "BPPFinder");
    if (self->out) f.add(self->out, 12, "the finder's output point");
}

// ---- BPPFinder::Find (0x46d0a0): the whole tree; the triangle hit, or null ------------------------------------
static bpp_tri* __fastcall BPPFinder_Find(BPPFinder* self, Edx) {
    if (BPPFinderBppFind(self, 0, bpp()->root) == -1) return 0;
    return self->hit;
}
static void fp_finder_find(Footprint& f, BPPFinder* self, Edx) {
    fp_finder(f, self);
    fp_bpp_finds(f);
}
PORT_FN(0x0046d0a0, "BPPFinder::Find", BPPFinder_Find, fp_finder_find)

// ---- BPPHit (0x46d0c0): the first triangle the segment p0 -> p1 meets, and where (out) -----------------------
// The finder is a local; its triangle index and hit are left uninitialised by the original (test_poly and
// intersect_plane set them before anything reads them), zeroed here.
static bpp_tri* __cdecl BPPHit(P3* p0, P3* p1, P3* out) {
    BPPFinder f;
    f.tri = 0;
    f.hit = 0;
    f.out = out;
    f.p0 = p0;
    f.p1 = p1;
    return BPPFinderFind(&f, 0);
}
static void fp_bpp_hit(Footprint& f, P3*, P3*, P3* out) {
    f.add(out, 12, "out");
    fp_bpp_finds(f);
}
PORT_FN(0x0046d0c0, "BPPHit", BPPHit, fp_bpp_hit)

// ---- BPPFinder::intersect_plane (0x46d0f0): the segment against triangle `tri`'s plane -------------------------
// An end within 0.001 of the plane (or a NaN distance) is the hit itself; otherwise the crossing at t = -d0/(n.D),
// if the segment isn't parallel (|n.D| >= 0.001) and t >= -0.0001 and t's bits <= 0x3f800347 (t <= ~1.0001).
static uint8_t __fastcall BPPFinder_intersect_plane(BPPFinder* self, Edx) {
    bpp_tri* t = bpp()->tris + self->tri;
    const P3* a = self->p0;
    double d0 = ((D(t->n.x) * a->x + D(a->z) * t->n.z) + D(a->y) * t->n.y) + t->d;
    float d0f = st(d0);
    if (!(fabs(d0) >= (float)0.001f)) {                            // test ah,1; je: a NaN is on the plane
        cp12(self->out, a);
        self->hit = t;
        return 1;
    }
    const P3* b = self->p1;
    double d1 = ((D(t->n.x) * b->x + D(b->y) * t->n.y) + D(b->z) * t->n.z) + t->d;
    if (!(fabs(d1) >= (float)0.001f)) {
        cp12(self->out, b);
        self->hit = t;
        return 1;
    }
    float dx = (float)(D(b->x) - a->x);
    float dy = (float)(D(b->y) - a->y);
    float dz = (float)(D(b->z) - a->z);
    double den = (D(t->n.x) * dx + D(t->n.y) * dy) + D(t->n.z) * dz;
    float denf = st(den);
    if (!(fabs(den) >= (float)0.001f)) return 0;
    double tt = -(D(d0f) / denf);                           // from the stored floats
    float tf = st(tt);                                      // (fcom, then fstp, then the test)
    if (!(tt >= -(float)0.0001f)) return 0;                        // fcom; test ah,1: less, or a NaN
    if (I(tf) > 0x3f800347) return 0;                       // cmp dword, 0x3f800347; jg (signed)
    P3* o = self->out;
    o->x = (float)(D(tf) * dx + self->p0->x);
    o->y = (float)(D(tf) * dy + self->p0->y);
    o->z = (float)(D(tf) * dz + self->p0->z);
    self->hit = t;
    return 1;
}
static void fp_finder_intersect_plane(Footprint& f, BPPFinder* self, Edx) { fp_finder(f, self); }
PORT_FN(0x0046d0f0, "BPPFinder::intersect_plane", BPPFinder_intersect_plane, fp_finder_intersect_plane)

// ---- BPPFinder::point_in_poly (0x46d290): is the output point inside triangle `tri`, in XZ? -------------------
// The three 2D cross products of the vertices relative to the point; inside when all are >= -0.0001 or all are
// <= 0.0001 (either winding), each tested on its float's bits: unsigned <= 0xb8d1b717, signed <= 0x38d1b717.
static uint8_t __fastcall BPPFinder_point_in_poly(BPPFinder* self, Edx) {
    const bpp_tri* t = bpp()->tris + self->tri;
    const P3* q = self->out;
    float a0 = (float)(D(t->v[0].x) - q->x);
    float b0 = (float)(D(t->v[0].z) - q->z);
    float a1 = (float)(D(t->v[1].x) - q->x);
    float b1 = (float)(D(t->v[1].z) - q->z);
    float a2 = (float)(D(t->v[2].x) - q->x);
    float b2 = (float)(D(t->v[2].z) - q->z);
    float c0 = (float)(D(a1) * b0 - D(b1) * a0);
    float c1 = (float)(D(a2) * b1 - D(b2) * a1);
    float c2 = (float)(D(b2) * a0 - D(a2) * b0);
    uint8_t in = 0;
    if ((uint32_t)I(c0) <= 0xb8d1b717u && (uint32_t)I(c1) <= 0xb8d1b717u && (uint32_t)I(c2) <= 0xb8d1b717u) in = 1;
    if (I(c0) <= 0x38d1b717 && I(c1) <= 0x38d1b717 && I(c2) <= 0x38d1b717) in = 1;
    return in;
}
static void fp_finder_point_in_poly(Footprint&, BPPFinder*, Edx) {}
PORT_FN(0x0046d290, "BPPFinder::point_in_poly", BPPFinder_point_in_poly, fp_finder_point_in_poly)

// ---- BPPFinder::test_poly (0x46d360): triangle i (-1: none) -----------------------------------------------------
static uint8_t __fastcall BPPFinder_test_poly(BPPFinder* self, Edx, int32_t i) {
    if (i == -1) return 0;
    self->tri = i;
    if (!BPPFinderIntersectPlane(self, 0)) return 0;
    return BPPFinderPointInPoly(self, 0);
}
static void fp_finder_test_poly(Footprint& f, BPPFinder* self, Edx, int32_t) { fp_finder(f, self); }
PORT_FN(0x0046d360, "BPPFinder::test_poly", BPPFinder_test_poly, fp_finder_test_poly)

// ---- BPPFinder::bpp_find (0x46d390): the segment through the tree -----------------------------------------------
// Each end's side of the node's line (|d| < 0.001 snapped to 0, then tested on the float's bits): the side of p0
// first, then the other side when p1 is on it (or on the line). A null child is a leaf: its triangle is tested.
// Returns the triangle's index, or -1.
static int32_t __fastcall BPPFinder_bpp_find(BPPFinder* self, Edx, bpp_node* node) {
    ++G(volatile int32_t, S_BPP_FINDS);
    const P3* a = self->p0;
    int32_t s0 = (int32_t)snap((D(a->z) * node->b + D(node->a) * a->x) + node->c);
    if (!(s0 > 0)) {                                        // cmp dword, 0; jg
        if (!node->le) {
            if (BPPFinderTestPoly(self, 0, node->tri_le)) return node->tri_le;
        } else {
            int32_t r = BPPFinderBppFind(self, 0, node->le);
            if (r != -1) return r;
        }
        const P3* b = self->p1;
        uint32_t s1 = snap((D(b->z) * node->b + D(node->a) * b->x) + node->c);
        if (s1 > 0x80000000u) return -1;                    // cmp dword, 0x80000000; ja: p1 strictly on the <= side
        if (!node->gt) {
            if (BPPFinderTestPoly(self, 0, node->tri_gt)) return node->tri_gt;
            return -1;
        }
        int32_t r = BPPFinderBppFind(self, 0, node->gt);
        return r != -1 ? r : -1;
    }
    if ((uint32_t)s0 > 0x80000000u) return -1;              // (dead: s0 > 0 here)
    if (!node->gt) {
        if (BPPFinderTestPoly(self, 0, node->tri_gt)) return node->tri_gt;
    } else {
        int32_t r = BPPFinderBppFind(self, 0, node->gt);
        if (r != -1) return r;
    }
    const P3* b = self->p1;
    int32_t s1 = (int32_t)snap((D(b->z) * node->b + D(node->a) * b->x) + node->c);
    if (s1 > 0) return -1;
    if (!node->le) {
        if (BPPFinderTestPoly(self, 0, node->tri_le)) return node->tri_le;
        return -1;
    }
    int32_t r = BPPFinderBppFind(self, 0, node->le);
    return r != -1 ? r : -1;
}
static void fp_finder_bpp_find(Footprint& f, BPPFinder* self, Edx, bpp_node*) {
    fp_finder(f, self);
    fp_bpp_finds(f);
}
PORT_FN(0x0046d390, "BPPFinder::bpp_find", BPPFinder_bpp_find, fp_finder_bpp_find)

// ---- load_file (0x46d540): the .bpp resource, made usable in place the first time it's loaded ------------------
static bpp_header* __cdecl bpp_load_file(const char* name) {
    uint8_t fresh;
    uint32_t version;
    int32_t size;
    bpp_header* h = (bpp_header*)ResourceGet(name, G(uint32_t, S_BPP_TYPE), &version, &size, 0, &fresh);
    if (!h) {
        LogPanic(STR_BPP_NOT_FOUND, name);
        return 0;
    }
    if (fresh) {
        if (version != G(uint32_t, S_BPP_VER)) LogPanic(STR_BAD_BPP_VER);
        uint32_t base = (uint32_t)(uintptr_t)h;
        h->tris = (bpp_tri*)(uintptr_t)(base + 0x14);
        bpp_node* nodes = (bpp_node*)(uintptr_t)(base + (uint32_t)h->n_tris * 56u + 0x14);
        h->nodes = nodes;
        bpp_node* root = (bpp_node*)(uintptr_t)((uint32_t)(uintptr_t)nodes + (uint32_t)(uintptr_t)h->root * 28u);
        h->root = root;
        FixupTree(root, h->nodes, (int32_t)((uint32_t)size - (uint32_t)h->n_tris * 56u - 0x14));
    }
    return h;
}
static void fp_bpp_load_file(Footprint& f, const char*) { f.replay_only = "loads a resource (ResourceGet)"; }
PORT_FN(0x0046d540, "load_file(bpp.obj)", bpp_load_file, fp_bpp_load_file)

// ---- unload_file (0x46d600) ------------------------------------------------------------------------------------
static void __cdecl bpp_unload_file() { ResourceForget(G(void*, S_BPP_HEADER)); }
static void fp_bpp_unload_file(Footprint& f) { f.replay_only = "frees a resource (ResourceForget)"; }
PORT_FN(0x0046d600, "unload_file(bpp.obj)", bpp_unload_file, fp_bpp_unload_file)

// ---- fixup_tree (0x46d610): child indices to pointers (-1 to null), down the <= side by recursion and the >
// side by the loop. The range check compares an index with the node section's size in BYTES (so only a wildly
// wrong index is reported), and the pointer is made whatever it says.
static void __cdecl fixup_tree(bpp_node* node, bpp_node* base, int32_t limit) {
    for (; node; node = node->gt) {
        int32_t i = (int32_t)(uintptr_t)node->le;
        if (i == -1) {
            node->le = 0;
        } else {
            if (i < 0 || i > limit) {
                LogReport((const char*)0x004f50f4);         // "------------------------"
                LogReport((const char*)0x004f5110);         // "fixup_tree: bogus data?"
                int32_t idx = (int32_t)((uint32_t)(uintptr_t)node - (uint32_t)(uintptr_t)base) / 0x1c;
                LogReport((const char*)0x004f5128, idx, i, i, limit);   // "node %d: less is outside range ..."
                LogReport((const char*)0x004f515c);
                LogPanic((const char*)0x004f5178);          // "PLEASE ALERT AN ENGINEER IMMEDIATELY. ..."
            }
            node->le = (bpp_node*)(uintptr_t)((uint32_t)(uintptr_t)base + (uint32_t)i * 28u);
        }
        int32_t j = (int32_t)(uintptr_t)node->gt;
        if (j == -1) {
            node->gt = 0;
        } else {
            if (j < 0 || j > limit) {
                LogReport((const char*)0x004f51b8);
                LogReport((const char*)0x004f51d4);
                int32_t idx = (int32_t)((uint32_t)(uintptr_t)node - (uint32_t)(uintptr_t)base) / 0x1c;
                LogReport((const char*)0x004f51ec, idx, j, j, limit);   // "... greater is outside range ..."
                LogReport((const char*)0x004f5220);
                LogPanic((const char*)0x004f523c);
            }
            node->gt = (bpp_node*)(uintptr_t)((uint32_t)(uintptr_t)base + (uint32_t)j * 28u);
        }
        FixupTree(node->le, base, limit);
    }
}
static void fp_fixup_tree(Footprint& f, bpp_node*, bpp_node*, int32_t) { f.replay_only = "rewrites a loaded tree in place"; }
PORT_FN(0x0046d610, "fixup_tree", fixup_tree, fp_fixup_tree)

// ---- bpp_find_point (0x46d740): the triangle under a point in XZ -- a descent, no backtracking ----------------
// a*x + b*z + c > 0 goes to the > side (a NaN to the <= side); a null child: that side's triangle.
static int32_t __cdecl bpp_find_point(P3* p) {
    bpp_node* n = bpp()->root;
    for (;;) {
        double v = (D(n->b) * p->z + D(n->a) * p->x) + n->c;
        if (v > 0.0) {                                      // fcomp 0; test ah,0x41; je
            if (!n->gt) return n->tri_gt;
            n = n->gt;
        } else {
            if (!n->le) return n->tri_le;
            n = n->le;
        }
    }
}
static void fp_bpp_find_point(Footprint&, P3*) {}
PORT_FN(0x0046d740, "bpp_find_point", bpp_find_point, fp_bpp_find_point)

// ---- BPPHitXZ (0x46d790): the ground under (p.x, p.z) -----------------------------------------------------------
// The point is first snapped to the millimetre (round(v / 0.001) * 0.001 through __ftol, for the tree only); a
// triangle standing on edge (|n.y| <= FLT_EPSILON, or a NaN) is no ground. out = (p.x, height, p.z), the height
// from the triangle's plane at the unsnapped point.
static __forceinline float mm(float v) {
    int32_t i = x87_ftol(D(v) / (float)0.001f + 0.5f);
    return (float)(D(i) * (float)0.001f);                          // fild; fmul 0.001f; fstp
}
static bpp_tri* __cdecl BPPHitXZ(P3* p, P3* out) {
    P3 q;
    q.x = mm(p->x);
    q.y = mm(p->y);
    q.z = mm(p->z);
    int32_t i = BppFindPoint(&q);
    if (i == -1) return 0;
    bpp_tri* t = bpp()->tris + i;
    if (!(fabs(t->n.y) > 1.1920928955078125e-07f)) return 0;   // fcomp FLT_EPSILON; test ah,0x41; je
    cp4(&out->x, &p->x);
    cp4(&out->z, &p->z);
    out->y = (float)-(((D(t->n.z) * p->z + D(p->x) * t->n.x) + t->d) / t->n.y);
    return t;
}
static void fp_bpp_hit_xz(Footprint& f, P3*, P3* out) { f.add(out, 12, "out"); }
PORT_FN(0x0046d790, "BPPHitXZ", BPPHitXZ, fp_bpp_hit_xz)

// ==== bsp.obj: the 3D BSP (dead in v1.0: nothing queries it) =====================================================

static void fp_bsp_counters(Footprint& f) { f.add((void*)(uintptr_t)S_BSP_LISTED, 0x40, "bsp.obj's counters"); }
static void fp_bsp_segment(Footprint& f) {
    fp_bsp_counters(f);
    f.add((void*)(uintptr_t)S_BSP_HIT, 4, "BSPHit's triangle");
    if (P3* o = G(P3*, S_BSP_OUT)) f.add(o, 12, "BSPHit's output point");
}
static void fp_bsp_sphere(Footprint& f) {
    fp_bsp_counters(f);
    f.add((void*)(uintptr_t)S_SPH_BEST_N, 12, "the nearest contact's normal");
    f.add((void*)(uintptr_t)S_SPH_BEST_D2, 4, "the nearest contact's distance^2");
    f.add((void*)(uintptr_t)S_SPH_BEST_P, 12, "the nearest contact");
    f.add((void*)(uintptr_t)S_SPH_BEST_TRI, 4, "the nearest contact's triangle");
}

// ---- BSPLoad (0x46fb50) / BSPUnload (0x46fbc0) -----------------------------------------------------------------
static void __cdecl BSPLoadRw(const char* name) { G(bsp_node*, S_BSP_HEAD) = BspLoadFile(name); }
static void fp_bsp_load(Footprint& f, const char*) { f.replay_only = "loads a resource (ResourceGet)"; }
PORT_FN(0x0046fb50, "BSPLoad", BSPLoadRw, fp_bsp_load)

static void __cdecl BSPUnloadRw() { BspUnloadFile(); }
static void fp_bsp_unload(Footprint& f) { f.replay_only = "frees a resource (ResourceForget)"; }
PORT_FN(0x0046fbc0, "BSPUnload", BSPUnloadRw, fp_bsp_unload)

// ---- fixup_nodes (0x46fb70): offsets to pointers (a null triangle is a LogPanic, then based anyway) -----------
static void __cdecl fixup_nodes(bsp_node* n, int32_t base) {
    for (;;) {
        if (!n->tri) LogPanic(STR_BAD_NODE);
        n->tri = (bsp_tri*)(uintptr_t)((uint32_t)(uintptr_t)n->tri + (uint32_t)base);
        if (bsp_node* f = n->front) {
            f = (bsp_node*)(uintptr_t)((uint32_t)(uintptr_t)f + (uint32_t)base);
            n->front = f;
            FixupNodes(f, base);
        }
        bsp_node* b = n->back;
        if (!b) return;
        b = (bsp_node*)(uintptr_t)((uint32_t)(uintptr_t)b + (uint32_t)base);
        n->back = b;
        n = b;
    }
}
static void fp_fixup_nodes(Footprint& f, bsp_node*, int32_t) { f.replay_only = "rewrites a loaded tree in place"; }
PORT_FN(0x0046fb70, "fixup_nodes", fixup_nodes, fp_fixup_nodes)

// ---- BSPHit (0x46fbd0): the segment p0 -> p1 through the BSP; the triangle hit (and *out), or null ------------
static bsp_tri* __cdecl BSPHit(P3* p0, P3* p1, P3* out) {
    G(int32_t, S_BSP_LISTED) = 0;
    G(bsp_tri*, S_BSP_HIT) = 0;
    G(P3*, S_BSP_OUT) = out;
    ++G(int32_t, S_BSP_HITS);
    IntersectAll(p0, p1, G(bsp_node*, S_BSP_HEAD));
    bsp_tri** list = (bsp_tri**)(uintptr_t)S_BSP_LIST;
    for (int32_t i = 0; i < G(int32_t, S_BSP_LISTED); i++) list[i]->visited = 0;
    G(int32_t, S_BSP_LISTED_SUM) += G(int32_t, S_BSP_LISTED);
    return G(bsp_tri*, S_BSP_HIT);
}
static void fp_bsp_hit(Footprint& f, P3*, P3*, P3* out) {
    fp_bsp_counters(f);
    f.add((void*)(uintptr_t)S_BSP_HIT, 4, "BSPHit's triangle");
    f.add((void*)(uintptr_t)S_BSP_OUT, 4, "BSPHit's output pointer");
    f.add(out, 12, "out");
}
PORT_FN(0x0046fbd0, "BSPHit", BSPHit, fp_bsp_hit)

// the hit: the triangle and the point, through the output pointer
static __forceinline uint8_t bsp_found(bsp_tri* t, const P3* p) {
    P3* o = G(P3*, S_BSP_OUT);
    G(bsp_tri*, S_BSP_HIT) = t;
    cp12(o, p);
    return 1;
}

// ---- intersect_tree (0x46fc40): BSPHit's first version, which also marks and lists the triangles it tried --------
// (called by nothing but itself). As intersect_all but: the triangle's plane test from each end, then down the
// near side as a loop when an end is on the plane, and at a crossing the triangle is tried only from the front
// and only once per query (its visited mark, up to 64 listed for BSPHit to clear).
static uint8_t __cdecl intersect_tree(P3* p0, P3* p1, bsp_node* node) {
    for (;;) {
        if (!node) return 0;
        bsp_tri* t = node->tri;
        uint32_t d0 = snap(((D(p0->z) * t->n.z + D(p0->y) * t->n.y) + D(t->n.x) * p0->x) + t->d);
        uint32_t d1 = snap(((D(p1->z) * t->n.z + D(p1->y) * t->n.y) + D(t->n.x) * p1->x) + t->d);
        if (d0 > 0x80000000u && d1 > 0x80000000u) {
            ++G(int32_t, S_BSP_ONE_SIDE);
            node = node->back;
            continue;
        }
        if ((int32_t)d0 > 0 && (int32_t)d1 > 0) {
            ++G(int32_t, S_BSP_ONE_SIDE);
            node = node->front;
            continue;
        }
        if ((d0 & 0x7fffffff) && (d1 & 0x7fffffff)) {       // the segment crosses the plane
            P3 P;
            ++G(int32_t, S_BSP_CROSSED);
            if (!BspIntersectPlane(p0, p1, t, &P, 0)) {
                LogReport(STR_BANG);
                return 0;
            }
            uint8_t front_first = 1;
            bsp_node* nearer = node->front;
            bsp_node* farther = node->back;
            if (d0 > 0x80000000u) {
                front_first = 0;
                nearer = node->back;
                farther = node->front;
            }
            if (IntersectTree(p0, &P, nearer)) return 1;
            if (front_first && !t->visited) {
                int32_t n = G(int32_t, S_BSP_LISTED);
                if (n < 64) {
                    G(int32_t, S_BSP_LISTED) = n + 1;
                    ((bsp_tri**)(uintptr_t)S_BSP_LIST)[n] = t;
                } else {
                    LogReport(STR_OVERFLOW);
                }
                if (BspPointInPoly(&P, t)) return bsp_found(t, &P);
                t->visited = 1;
            }
            return IntersectTree(&P, p1, farther) ? 1 : 0;
        }
        ++G(int32_t, S_BSP_ON_PLANE);
        if (!(d0 & 0x7fffffff)) {                           // p0 on the plane
            if (BspPointInPoly(p0, t)) return bsp_found(t, p0);
            if (d1 > 0x80000000u) {
                node = node->back;
                continue;
            }
            if (!((int32_t)d1 > 0)) return 0;
            node = node->front;
            continue;
        }
        if (d0 > 0x80000000u) {                             // p1 on the plane
            if (IntersectTree(p0, p1, node->back)) return 1;
        } else if ((int32_t)d0 > 0) {
            if (IntersectTree(p0, p1, node->front)) return 1;
        }
        if (BspPointInPoly(p1, t)) return bsp_found(t, p1);
        return 0;
    }
}
static void fp_intersect_tree(Footprint& f, P3*, P3*, bsp_node*) {
    f.replay_only = "marks the triangles it tries anywhere in the tree (and nothing calls it)";
}
PORT_FN(0x0046fc40, "intersect_tree", intersect_tree, fp_intersect_tree)

// ---- intersect_all (0x46ff30): the segment p0 -> p1 through the BSP ----------------------------------------------
// Both ends on one side (|d| < 0.001 counts as on the plane): that side. Across: split at the plane (a failed split
// ends the search), the near half first, then the crossing point in the triangle, then the far half. An end on the
// plane: that end in the triangle, and both sides, the other end's side first.
static uint8_t __cdecl intersect_all(P3* p0, P3* p1, bsp_node* node) {
    for (;;) {
        if (!node) return 0;
        bsp_tri* t = node->tri;
        uint32_t d0 = snap(((D(p0->y) * t->n.y + D(p0->z) * t->n.z) + D(t->n.x) * p0->x) + t->d);
        uint32_t d1 = snap(((D(p1->y) * t->n.y + D(p1->z) * t->n.z) + D(t->n.x) * p1->x) + t->d);
        if (d0 > 0x80000000u && d1 > 0x80000000u) {
            ++G(int32_t, S_BSP_ONE_SIDE);
            node = node->back;
            continue;
        }
        if ((int32_t)d0 > 0 && (int32_t)d1 > 0) {
            ++G(int32_t, S_BSP_ONE_SIDE);
            node = node->front;
            continue;
        }
        if ((d0 & 0x7fffffff) && (d1 & 0x7fffffff)) {
            P3 P;
            ++G(int32_t, S_BSP_CROSSED);
            if (!BspIntersectPlane(p0, p1, t, &P, 0)) return 0;
            bsp_node* nearer = node->front;
            bsp_node* farther = node->back;
            if (!((int32_t)d0 > 0)) {
                nearer = node->back;
                farther = node->front;
            }
            if (IntersectAll(p0, &P, nearer)) return 1;
            if (BspPointInPoly(&P, t)) return bsp_found(t, &P);
            return IntersectAll(&P, p1, farther) ? 1 : 0;
        }
        ++G(int32_t, S_BSP_ON_PLANE);
        if (!(d0 & 0x7fffffff)) {                           // p0 on the plane
            if (BspPointInPoly(p0, t)) return bsp_found(t, p0);
            if (d1 > 0x80000000u) return IntersectAll(p0, p1, node->back) || IntersectAll(p0, p1, node->front);
            if ((int32_t)d1 > 0) return IntersectAll(p0, p1, node->front) || IntersectAll(p0, p1, node->back);
            return 0;
        }
        if (d0 > 0x80000000u) {                             // p1 on the plane
            if (IntersectAll(p0, p1, node->back) || IntersectAll(p0, p1, node->front)) return 1;
        } else if ((int32_t)d0 > 0) {
            if (IntersectAll(p0, p1, node->front) || IntersectAll(p0, p1, node->back)) return 1;
        }
        if (BspPointInPoly(p1, t)) return bsp_found(t, p1);
        return 0;
    }
}
static void fp_intersect_all(Footprint& f, P3*, P3*, bsp_node*) { fp_bsp_segment(f); }
PORT_FN(0x0046ff30, "intersect_all", intersect_all, fp_intersect_all)

// ---- point_in_poly (0x470240): inside every edge (within 0.1) -- (v_i - p).e_i >= -0.1 for each edge, the
// outward normal e_i; a NaN is outside
static uint8_t __cdecl bsp_point_in_poly(P3* p, bsp_tri* t) {
    if (!(((D(t->v[0].x) - p->x) * t->e[0].x + (D(t->v[0].z) - p->z) * t->e[0].z) + (D(t->v[0].y) - p->y) * t->e[0].y
          >= -(float)0.1f))
        return 0;
    if (!(((D(t->v[1].y) - p->y) * t->e[1].y + (D(t->v[1].z) - p->z) * t->e[1].z) + (D(t->v[1].x) - p->x) * t->e[1].x
          >= -(float)0.1f))
        return 0;
    if (!(((D(t->v[2].x) - p->x) * t->e[2].x + (D(t->v[2].z) - p->z) * t->e[2].z) + (D(t->v[2].y) - p->y) * t->e[2].y
          >= -(float)0.1f))
        return 0;
    return 1;
}
static void fp_bsp_point_in_poly(Footprint& f, P3*, bsp_tri*) { f.pure = true; }
PORT_FN(0x00470240, "point_in_poly(bsp.obj)", bsp_point_in_poly, fp_bsp_point_in_poly)

// ---- intersect_plane (0x4702e0): the segment against a triangle's plane; the point (out) and t (tout), either
// may be null. An end within 0.001 of the plane (or a NaN distance) is the answer; otherwise t = -d0/(n.D), when
// |n.D| >= 0.001, t >= 0 and t's bits <= 1.0f's.
static uint8_t __cdecl bsp_intersect_plane(P3* p0, P3* p1, bsp_tri* t, P3* out, float* tout) {
    double d0 = ((D(p0->x) * t->n.x + D(p0->y) * t->n.y) + D(p0->z) * t->n.z) + t->d;
    float d0f = st(d0);
    if (!(fabs(d0) >= (float)0.001f)) {
        if (out) cp12(out, p0);
        return 1;
    }
    double d1 = ((D(p1->x) * t->n.x + D(p1->y) * t->n.y) + D(p1->z) * t->n.z) + t->d;
    if (!(fabs(d1) >= (float)0.001f)) {
        if (out) cp12(out, p1);
        return 1;
    }
    float dx = (float)(D(p1->x) - p0->x);
    float dy = (float)(D(p1->y) - p0->y);
    double dzr = D(p1->z) - p0->z;                          // fst: kept in the register
    float dz = st(dzr);
    double den = ((dzr * t->n.z) + D(t->n.y) * dy) + D(t->n.x) * dx;
    float denf = st(den);
    if (!(fabs(den) >= (float)0.001f)) return 0;
    double tt = -(D(d0f) / denf);
    float tf = st(tt);                                      // (fcom, then fstp, then the test)
    if (!(tt >= 0.0)) return 0;                             // fcom 0.0f; test ah,1: negative, or a NaN
    if (I(tf) > 0x3f800000) return 0;                       // signed: t > 1
    if (out) {
        out->x = (float)(D(tf) * dx + p0->x);
        out->y = (float)(D(tf) * dy + p0->y);
        out->z = (float)(D(tf) * dz + p0->z);
    }
    if (tout) cp4(tout, &tf);
    return 1;
}
static void fp_bsp_intersect_plane(Footprint& f, P3*, P3*, bsp_tri*, P3* out, float* tout) {
    if (out) f.add(out, 12, "out");
    if (tout) f.add(tout, 4, "t");
    f.pure = true;
}
PORT_FN(0x004702e0, "intersect_plane(bsp.obj)", bsp_intersect_plane, fp_bsp_intersect_plane)

// ---- BSPHitSphere (0x470450): the nearest contact of a ball with the BSP, if nearer than r ---------------------
static bsp_tri* __cdecl BSPHitSphere(P3* c, float r, P3* point, P3* normal) {
    float r2 = (float)(D(r) * r);
    G(int32_t, S_SPH_HITS) = 0;
    G(int32_t, S_SPH_NODES) = 0;
    cp4((void*)(uintptr_t)S_SPH_BEST_D2, &r2);
    ++G(int32_t, S_SPH_CALLS);
    G(int32_t, S_SPH_TRIS) = 0;
    SphereIntersectTree(c, Ub(r), G(bsp_node*, S_BSP_HEAD));
    if (G(int32_t, S_SPH_HITS) > 0) {
        ++G(int32_t, S_SPH_CALLS_HIT);
        G(int32_t, S_SPH_HITS_SUM) += G(int32_t, S_SPH_HITS);
    }
    if (G(int32_t, S_SPH_HITS) > G(int32_t, S_SPH_HITS_MAX)) G(int32_t, S_SPH_HITS_MAX) = G(int32_t, S_SPH_HITS);
    if (G(int32_t, S_SPH_NODES) > G(int32_t, S_SPH_NODES_MAX)) G(int32_t, S_SPH_NODES_MAX) = G(int32_t, S_SPH_NODES);
    float best = G(float, S_SPH_BEST_D2);
    G(int32_t, S_SPH_NODES_SUM) += G(int32_t, S_SPH_NODES);
    G(int32_t, S_SPH_TRIS_SUM) += G(int32_t, S_SPH_TRIS);
    if (best >= r2) return 0;                               // fcomp; test ah,1; je: not less, and ordered
    cp12(point, (void*)(uintptr_t)S_SPH_BEST_P);
    cp12(normal, (void*)(uintptr_t)S_SPH_BEST_N);
    return G(bsp_tri*, S_SPH_BEST_TRI);
}
static void fp_bsp_hit_sphere(Footprint& f, P3*, float, P3* point, P3* normal) {
    fp_bsp_sphere(f);
    f.add(point, 12, "point");
    f.add(normal, 12, "normal");
}
PORT_FN(0x00470450, "BSPHitSphere", BSPHitSphere, fp_bsp_hit_sphere)

// ---- sphere_intersect_tree (0x470560): every triangle whose plane is within r of the centre ----------------------
// (it only ever returns 0: a leaf returns 0 and a node returns what its children do)
static uint8_t __cdecl sphere_intersect_tree(P3* c, float r, bsp_node* node) {
    for (;;) {
        if (!node) return 0;
        ++G(int32_t, S_SPH_NODES);
        bsp_tri* t = node->tri;
        double d = ((D(t->n.y) * c->y + D(t->n.z) * c->z) + D(t->n.x) * c->x) + t->d;
        float df = st(d);                                   // (fcom r, then fstp, then the test)
        if (d > r) {                                        // fcom r; test ah,0x41; jne: <= or a NaN falls through
            node = node->front;
            continue;
        }
        if (-D(r) > df) {                                   // fld r; fchs; fcomp d; test ah,0x41
            node = node->back;
            continue;
        }
        ++G(int32_t, S_SPH_TRIS);
        CalcSphereIntersection(c, Ub(r), t);
        if (SphereIntersectTree(c, Ub(r), node->front)) return 1;
        if (SphereIntersectTree(c, Ub(r), node->back)) return 1;
        return 0;
    }
}
static void fp_sphere_intersect_tree(Footprint& f, P3*, float, bsp_node*) { fp_bsp_sphere(f); }
PORT_FN(0x00470560, "sphere_intersect_tree", sphere_intersect_tree, fp_sphere_intersect_tree)

// the unit vector from q to c: len by fsqrt (x87_sqrt) or VectorLength, 1/len in the register
static __forceinline void unit_from(P3* D_, const P3* c, const P3* q) {
    D_->x = (float)(D(c->x) - q->x);
    D_->y = (float)(D(c->y) - q->y);
    D_->z = (float)(D(c->z) - q->z);
}
static __forceinline void scale_by(P3* D_, double inv) {
    D_->x = (float)(D(D_->x) * inv);
    D_->y = (float)(D(D_->y) * inv);
    D_->z = (float)(inv * D_->z);
}
static __forceinline double inv_len(const P3* D_) {
    return D(1.0f) / x87_sqrt((D(D_->y) * D_->y + D(D_->z) * D_->z) + D(D_->x) * D_->x);
}

// ---- calc_sphere_intersection (0x470620): a ball against one triangle -------------------------------------------
// s_i = (v_i - c).e_i per edge (e_i the outward normal): more than r outside any edge is no contact (edge 0 lets
// a NaN through, edges 1 and 2 don't). The edges the centre is outside (s_i's bits <= 0) pick the nearest feature:
// none the plane (the centre projected onto it, the triangle's normal), one an edge (the projection projected on
// to the edge's line, the direction from it to the centre), two a vertex; all three is impossible (LogPanic).
static void __cdecl calc_sphere_intersection(P3* c, float r, bsp_tri* t) {
    float s = (float)(((D(t->v[0].y) - c->y) * t->e[0].y + (D(t->v[0].z) - c->z) * t->e[0].z) +
                      (D(t->v[0].x) - c->x) * t->e[0].x);
    double nr = -D(r);
    float nrf = (float)nr;
    if (nr > s) return;                                     // fcom s; test ah,0x41; je
    int flags = I(s) > 0 ? 0 : 1;
    double s1 = ((D(t->v[1].y) - c->y) * t->e[1].y + (D(t->v[1].z) - c->z) * t->e[1].z) + (D(t->v[1].x) - c->x) * t->e[1].x;
    s = st(s1);                                             // (fcom -r, then fstp, then the test)
    if (!(s1 >= nrf)) return;                               // fcom -r; test ah,1: less, or a NaN
    if (!(I(s) > 0)) flags |= 2;
    double s2 = ((D(t->v[2].y) - c->y) * t->e[2].y + (D(t->v[2].z) - c->z) * t->e[2].z) + (D(t->v[2].x) - c->x) * t->e[2].x;
    s = st(s2);
    if (!(s2 >= nrf)) return;
    if (!(I(s) > 0)) flags |= 4;
    P3 Q, N, V;
    cp12(&Q, c);
    cp12(&N, &t->n);
    VectorSub(&V, &Q, &t->v[0]);
    double dot = DotProduct(&N, &V);
    VectorAddScaled(&Q, &Q, &N, (float)-dot);              // Q: c projected onto the plane
    switch (flags) {
    case 0:
        AddIntersection(c, t, &Q, &N);
        return;
    case 1:
    case 2:
        ProjectToPlane(&Q, &t->v[flags - 1], &t->e[flags - 1]);
        unit_from(&V, c, &Q);
        scale_by(&V, inv_len(&V));
        AddIntersection(c, t, &Q, &V);
        return;
    case 4:
        ProjectToPlane(&Q, &t->v[2], &t->e[2]);
        unit_from(&V, c, &Q);
        scale_by(&V, D(1.0f) / VectorLength(&V));
        AddIntersection(c, t, &Q, &V);
        return;
    case 3:                                                 // edges 0 and 1: vertex 1
    case 5:                                                 // edges 0 and 2: vertex 0
    case 6: {                                               // edges 1 and 2: vertex 2
        P3* v = &t->v[flags == 3 ? 1 : flags == 5 ? 0 : 2];
        unit_from(&V, c, v);
        scale_by(&V, inv_len(&V));
        AddIntersection(c, t, v, &V);
        return;
    }
    default:
        LogPanic(STR_IMPOSSIBLE);
        return;
    }
}
static void fp_calc_sphere_intersection(Footprint& f, P3*, float, bsp_tri*) { fp_bsp_sphere(f); }
PORT_FN(0x00470620, "calc_sphere_intersection", calc_sphere_intersection, fp_calc_sphere_intersection)

// ---- add_intersection (0x470b40): keep the contact p (normal n) if it's the nearest yet (a NaN distance is) ----
static void __cdecl add_intersection(P3* c, bsp_tri* t, P3* p, P3* n) {
    ++G(int32_t, S_SPH_HITS);
    float dx = (float)(D(p->x) - c->x);
    float dy = (float)(D(p->y) - c->y);
    float dz = (float)(D(p->z) - c->z);
    double d2 = (D(dy) * dy + D(dz) * dz) + D(dx) * dx;
    float d2f = st(d2);                                     // (fcom best, then fstp, then the test)
    if (d2 >= G(float, S_SPH_BEST_D2)) return;              // fcom best; test ah,1; je
    cp4((void*)(uintptr_t)S_SPH_BEST_D2, &d2f);
    cp12((void*)(uintptr_t)S_SPH_BEST_P, p);
    G(bsp_tri*, S_SPH_BEST_TRI) = t;
    cp12((void*)(uintptr_t)S_SPH_BEST_N, n);
}
static void fp_add_intersection(Footprint& f, P3*, bsp_tri*, P3*, P3*) { fp_bsp_sphere(f); }
PORT_FN(0x00470b40, "add_intersection", add_intersection, fp_add_intersection)

// ---- load_file (0x470be0): the .bsp resource, based in place the first time it's loaded ------------------------
static bsp_node* __cdecl bsp_load_file(const char* name) {
    uint8_t fresh;
    uint32_t version;
    int32_t size;
    bsp_node* h = (bsp_node*)ResourceGet(name, G(uint32_t, S_BSP_TYPE), &version, &size, 0, &fresh);
    if (!h) {
        LogPanic(STR_BSP_NOT_FOUND, name);
        return 0;
    }
    if (fresh) {
        if (G(uint32_t, S_BSP_VER) != version) LogPanic(STR_BAD_BSP_VER);
        FixupNodes(h, (int32_t)(uintptr_t)h);
    }
    return h;
}
static void fp_bsp_load_file(Footprint& f, const char*) { f.replay_only = "loads a resource (ResourceGet)"; }
PORT_FN(0x00470be0, "load_file(bsp.obj)", bsp_load_file, fp_bsp_load_file)

// ---- unload_file (0x470c60) ------------------------------------------------------------------------------------
static void __cdecl bsp_unload_file() { ResourceForget(G(void*, S_BSP_HEAD)); }
static void fp_bsp_unload_file(Footprint& f) { f.replay_only = "frees a resource (ResourceForget)"; }
PORT_FN(0x00470c60, "unload_file(bsp.obj)", bsp_unload_file, fp_bsp_unload_file)

// ---- project_to_plane (0x470c70): p -= ((p - o).n) n, in place ---------------------------------------------------
static void __cdecl project_to_plane(P3* p, P3* o, P3* n) {
    double s = -(((D(p->z) - o->z) * n->z + (D(p->y) - o->y) * n->y) + (D(p->x) - o->x) * n->x);
    p->x = (float)(D(n->x) * s + p->x);
    p->y = (float)(D(n->y) * s + p->y);
    p->z = (float)(s * n->z + p->z);
}
static void fp_project_to_plane(Footprint& f, P3* p, P3*, P3*) {
    f.add(p, 12, "p");
    f.pure = true;
}
PORT_FN(0x00470c70, "project_to_plane", project_to_plane, fp_project_to_plane)
