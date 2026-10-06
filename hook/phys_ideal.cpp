// phys_ideal.cpp -- M3 3.5 group A4: the AI's racing line, rewritten. ai:ideal.obj (IdealLine and its
// ConstIdealLine / MutableIdealLine / CenterLine flavours, the line resource loader, the debug drawing),
// ai:idealres.obj (ILSeg's Hermite helpers) and ai:track.obj (AIGetTrackInfo).
//
// An ideal line is a closed loop of ILSegs (68 bytes each): a node p, its Hermite tangent v, a target speed,
// a corridor width, the segment's length and the distance from the head. A position on it is an ILinePos
// {seg, t}, t in 0..1 along the cubic Hermite curve from seg->p to seg->next->p. The "bead" is the IdealLine's
// own ILinePos for one car: update_car_info records the car's point and moves the bead after it
// (advance_bead: Newton steps along the tangent, up to 10, following next / stepping back one segment);
// get_rabbit_position looks a distance ahead of the bead; the CenterLine (one per car, built by RaceDeity
// from track.ild or dlatlong.ili) adds per-segment passing times, the lateral offset and the lap cookie
// ((segment index << 16) | t * 65535).
//
// Layouts (offsets from the disassembly; the recovered types agree where they name a field):
//   ILSeg (68)          +0 next, +4 p, +0xc v, +0x14 width, +0x18 speed, +0x1c curvature, +0x20 length,
//                       +0x24 dist, +0x28 index (int16), +0x2a checkpoint (byte), +0x34 (sign picks
//                       __DrawLine's colour), +0x3c (fixup_res clears 0xfeedbeef here)
//   IdealLineRes        +0 magic -2, +4 ILSeg size 0x44, +8 count, +0xc the ILSegs (the .ild/.ili 'NILI'
//                       resource, version 3; fixup_res relinks it in place, reversed on request)
//   IdealLine (0x38)    vtable 0x4db6d0: +4 bead {seg, t}, +0xc rabbit point, +0x14 rabbit valid,
//                       +0x18 car point, +0x20 car direction, +0x28 reset pending, +0x2c head,
//                       +0x30 bead on line, +0x34 segment count
//   ConstIdealLine      (0x3c) vtable 0x4db6e8: +0x38 the IdealLineRes (MemAlloc'd, owned)
//   MutableIdealLine    (0x58) vtable 0x4db6e0: +0x38 Pool<ILSeg> (PoolBase, vtable 0x4db6d8, 512 x 68 bytes)
//   CenterLine (0x70)   vtable 0x4db6f0, a ConstIdealLine: +0x3c bead point, +0x44 tangent, +0x4c normal
//                       (tan.z, -tan.x), +0x54 lateral offset, +0x58 its change, +0x5c total length,
//                       +0x60 per-segment times (float[count]), +0x64 the car index update was given,
//                       +0x68 started (the bead has reached the head), +0x6c the next segment to time
//
// Threads: the AI (AICar, update_car_info / get_rabbit_position...) and the deity (CenterLine::update, reset,
// the conversions) run on the physics thread; Draw / __DrawLine / project are the main thread's debug overlay
// (they draw with gxCircle / gxRect: replay_only). Loading a line (ILineTry, load_res, the loads, the
// constructors that load) and the MutableIdealLine's pool (alloc / free) are replay_only.
//
// Fixes (port.h: VP_FIX; the harness builds VP_FAITHFUL for the original's behaviour, bit for bit):
//  * the lost bead (the stock AI crash): reset_bead_position leaves the bead NULL when get_nearest_bead finds
//    nothing (a NaN car position, a teleport, a point alongside no segment), and advance_bead, CenterLine::update,
//    update_2d_data, time_between and get_car_dlong_meters/cookie read it unchecked. Now the bead is put back on
//    the line where it's lost -- at the point of the line nearest the car (fix_find_bead) -- and every reader
//    here does the same before it reads. (This replaces vrmod's race.exe patch of advance_bead: head, t = 0.)
//  * a t that can't be stepped down a segment at a time (infinite or >= 2^24: a zero or near-zero segment
//    length) hung advance_bead, get_rabbit_position and get_nearest_bead: it now moves on one node, t = 0.
//  * QuickTan's 0/0 (zero tangents: a two-node line's) gives the chord's direction instead of NaN.
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
static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }      // an integer move of a float

// fld dword [src]; fchs; fstp dword [dst] -- a negation through the FPU (a signalling NaN comes out quiet)
static __forceinline void fneg_store(float* dst, const float* src) {
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[src]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "fchs\n\t"
                     "mov eax, %[dst]\n\t"
                     "fstp dword ptr [eax]"
                     :
                     : [src] "m"(src), [dst] "m"(dst)
                     : VP_X87_CLOBBERS, "eax", "cc", "memory");
#else
    __asm { mov eax, src
            fld dword ptr [eax]
            fchs
            mov eax, dst
            fstp dword ptr [eax] }
#endif
}
// fld dword [src]; fstp dword [dst] -- a copy through the FPU
static __forceinline void fpu_copy(float* dst, const float* src) {
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[src]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "mov eax, %[dst]\n\t"
                     "fstp dword ptr [eax]"
                     :
                     : [src] "m"(src), [dst] "m"(dst)
                     : VP_X87_CLOBBERS, "eax", "cc", "memory");
#else
    __asm { mov eax, src
            fld dword ptr [eax]
            mov eax, dst
            fstp dword ptr [eax] }
#endif
}

// ---- layouts -----------------------------------------------------------------------------------------------------
namespace {

struct Point2D { float x, z; };

struct ILSeg {
    ILSeg* next;                       // +0x00 the loop
    Point2D p;                         // +0x04 the node
    Point2D v;                         // +0x0c its Hermite tangent (per segment: not a unit vector)
    float width;                       // +0x14 corridor half-width
    float speed;                       // +0x18 target speed
    float curvature;                   // +0x1c
    float length;                      // +0x20 metres to the next node
    float dist;                        // +0x24 metres from the head
    int16_t index;                     // +0x28 position in the loop
    uint8_t checkpoint;                // +0x2a the checkpoint this node is in (0: none)
    uint8_t _2b;
    uint32_t _2c, _30;
    float _34;                         // +0x34 __DrawLine: negative -> the other colour
    uint32_t _38;
    uint32_t _3c;                      // +0x3c fixup_res: 0xfeedbeef here is cleared
    uint32_t _40;
};
static_assert(offsetof(ILSeg, length) == 0x20 && offsetof(ILSeg, checkpoint) == 0x2a && sizeof(ILSeg) == 0x44, "ILSeg");

struct ILinePos { ILSeg* seg; float t; };

struct IdealLineRes {                  // the 'NILI' resource (version 3), relinked in place by fixup_res
    int32_t magic;                     // +0 -2
    int32_t seg_size;                  // +4 0x44
    int32_t count;                     // +8
    // +0xc: count ILSegs
};
static __forceinline ILSeg* res_seg(const IdealLineRes* r, int32_t i) {
    return (ILSeg*)((uint8_t*)r + 0xc + i * 0x44);
}

struct IdealLine {                     // 0x38 bytes, vtable 0x4db6d0
    void** vtable;
    ILSeg* bead_seg;                   // +0x04 the bead: the car's place on the line (ILinePos)
    float bead_t;                      // +0x08
    Point2D rabbit;                    // +0x0c get_rabbit_position's last point (Draw)
    uint8_t rabbit_valid;              // +0x14
    uint8_t _15[3];
    Point2D car_pos;                   // +0x18 update_car_info
    Point2D car_dir;                   // +0x20
    uint8_t reset_pending;             // +0x28 the next update_car_info finds the bead afresh
    uint8_t _29[3];
    ILSeg* head;                       // +0x2c
    uint8_t bead_on_line;              // +0x30 advance_bead: 0 once the car is > 150 m from the bead
    uint8_t _31[3];
    int32_t num_segs;                  // +0x34 load_res
};
static_assert(offsetof(IdealLine, car_pos) == 0x18 && offsetof(IdealLine, head) == 0x2c && sizeof(IdealLine) == 0x38, "IdealLine");

struct ConstIdealLine : IdealLine {    // 0x3c bytes, vtable 0x4db6e8
    IdealLineRes* res;                 // +0x38
};
static_assert(sizeof(ConstIdealLine) == 0x3c, "ConstIdealLine");

struct PoolBase { void** vtable; uint8_t _04[0x1c]; };
struct MutableIdealLine : IdealLine {  // 0x58 bytes, vtable 0x4db6e0
    PoolBase pool;                     // +0x38 Pool<ILSeg>, 512 nodes
};
static_assert(sizeof(MutableIdealLine) == 0x58, "MutableIdealLine");

struct CenterLine : ConstIdealLine {   // 0x70 bytes, vtable 0x4db6f0
    Point2D pos;                       // +0x3c the bead's point
    Point2D tan;                       // +0x44 the unit tangent there
    Point2D normal;                    // +0x4c (tan.z, -tan.x)
    float lat;                         // +0x54 the car's offset along the normal
    float lat_delta;                   // +0x58
    float total;                       // +0x5c the loop's length
    float* times;                      // +0x60 when the car last passed each node
    int32_t car_index;                 // +0x64 update's second argument
    uint8_t started;                   // +0x68 the bead has reached the head since reset
    uint8_t _69[3];
    ILSeg* time_seg;                   // +0x6c the next node to stamp
};
static_assert(offsetof(CenterLine, lat) == 0x54 && offsetof(CenterLine, time_seg) == 0x6c && sizeof(CenterLine) == 0x70, "CenterLine");

}  // namespace

// ---- constants ---------------------------------------------------------------------------------------------------
static const float k_eps = Fb(0x3a03126fu);                              // 0.0005
static const float k_cookie_scale = Fb(0x37800080u);                      // 1.5259022e-05 (~1/65535)
static const float k_wrong_way_sq = Fb(0x409e0653u);                      // 4.938272 (2.2222^2)
static const uint32_t k_far = 0x461c4000u;                                // 10000.0: get_nearest_bead's reach
static const uint32_t k_flt_max = 0x7f7fffffu;
static const uint32_t k_nili = 0x494c494eu;                               // 'NILI'
static const uint32_t k_ten = 0x41200000u;                                // 10.0

// ---- the functions they call, by address -------------------------------------------------------------------------
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* MemFree_t)(void*);
static const MemFree_t MemFree = (MemFree_t)0x00414300;
static const MemFree_t op_delete = (MemFree_t)0x00414390;
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
static const AssertMsg_t ASSERT_MSG_o = (AssertMsg_t)0x00422700;        // a bare ret in v1.0: kept, as called
typedef const void*(__cdecl* ResTry_t)(const char*, uint32_t, uint32_t*, int32_t*);   // (name, tag, version&, size*)
static const ResTry_t ResourceTryDiscardable = (ResTry_t)0x00419f40;
typedef uint8_t(__cdecl* ResForget_t)(void*);
static const ResForget_t ResourceForget = (ResForget_t)0x0041a450;
typedef void*(__cdecl* Memmove_t)(void*, const void*, uint32_t);
static const Memmove_t memmove_o = (Memmove_t)0x004cf400;                // the CRT's, in race.exe
typedef double(__cdecl* TerrainGetHeight_t)(uint32_t, uint32_t);        // (x, z) as bits; ST0
static const TerrainGetHeight_t TerrainGetHeight = (TerrainGetHeight_t)0x00465ae0;
typedef uint8_t(__cdecl* ProjectPoint_t)(const P3*, P3*);
static const ProjectPoint_t mrProjectPoint = (ProjectPoint_t)0x0044f200;
typedef void(__cdecl* GxCircle_t)(int, int, int, uint32_t);
static const GxCircle_t gxCircle = (GxCircle_t)0x00452e80;
typedef void(__cdecl* GxRect_t)(int, int, int, int, uint32_t);
static const GxRect_t gxRect = (GxRect_t)0x004506e0;
typedef void*(__fastcall* PoolCtor_t)(void*, Edx, const char*, int, uint32_t);
static const PoolCtor_t PoolBase_ctor = (PoolCtor_t)0x0041ba30;
typedef void(__fastcall* PoolDtor_t)(void*, Edx);
static const PoolDtor_t PoolBase_dtor = (PoolDtor_t)0x0041bac0;
typedef void*(__fastcall* PoolAlloc_t)(void*, Edx);
static const PoolAlloc_t PoolBase_alloc = (PoolAlloc_t)0x0041bb20;
typedef void(__fastcall* PoolFree_t)(void*, Edx, void*);
static const PoolFree_t PoolBase_free = (PoolFree_t)0x0041bb60;
typedef int(__cdecl* FileCreate_t)(const char*);
static const FileCreate_t FileCreate = (FileCreate_t)0x004115f0;
typedef void(__cdecl* ResourceWrite_t)(int, uint32_t, uint32_t);
static const ResourceWrite_t ResourceWrite = (ResourceWrite_t)0x0041a5c0;
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
static const FileWrite_t FileWrite = (FileWrite_t)0x00411a30;
typedef void(__cdecl* FileClose_t)(int*);
static const FileClose_t FileClose = (FileClose_t)0x00411850;
typedef const uint8_t*(__cdecl* WorldGameOptions_t)();                  // +0x25: reverse the line
static const WorldGameOptions_t WorldGameOptions = (WorldGameOptions_t)0x004627a0;
typedef double(__cdecl* PhysicsGetTime_t)();                            // physics_tick * 0.016f, ST0
static const PhysicsGetTime_t PhysicsGetTime = (PhysicsGetTime_t)0x0042bc80;
// the profiler hooks advance_bead calls through (function-pointer globals)
typedef void(__cdecl* PrOverhead_t)();
typedef int(__cdecl* ProfStart_t)(const char*);
typedef void(__cdecl* ProfStop_t)(int);
static PrOverhead_t* const i_pr_overhead_begin = (PrOverhead_t*)0x004e642c;
static PrOverhead_t* const i_pr_overhead_end = (PrOverhead_t*)0x004e6430;
static ProfStart_t* const i_prof_start = (ProfStart_t*)0x004e6434;
static ProfStop_t* const i_prof_stop = (ProfStop_t*)0x004e6438;
static uint8_t* const g_draw_head_flag = (uint8_t*)0x004ec2a0;          // Draw: "this node is the head"
static const uint32_t* const g_col_node = (const uint32_t*)0x00520ab4;  // ideal.obj's colours
static const uint32_t* const g_col_bead = (const uint32_t*)0x00520a74;
static const uint32_t* const g_col_neg = (const uint32_t*)0x00520a7c;
static const uint32_t* const g_col_line = (const uint32_t*)0x00520a9c;

// ideal.obj / idealres.obj themselves, by address (so each callee's own hook is what runs)
typedef IdealLine*(__fastcall* ILCtor_t)(void*, Edx);
static const ILCtor_t IdealLine_ctor_o = (ILCtor_t)0x00421180;
static const ILCtor_t ConstIdealLine_ctor_o = (ILCtor_t)0x00422370;
typedef void(__fastcall* ILVoid_t)(void*, Edx);
static const ILVoid_t IdealLine_dtor_o = (ILVoid_t)0x004211a0;
static const ILVoid_t IdealLine_init_o = (ILVoid_t)0x004211b0;
static const ILVoid_t IdealLine_reset_bead_position_o = (ILVoid_t)0x00421520;
static const ILVoid_t IdealLine_advance_bead_o = (ILVoid_t)0x00421840;
static const ILVoid_t ILSeg_DrawLine_o = (ILVoid_t)0x00422a80;
static const ILVoid_t MIL_clear_o = (ILVoid_t)0x00421f60;
static const ILVoid_t ConstIdealLine_dtor_o = (ILVoid_t)0x00422390;
static const ILVoid_t CenterLine_reset_o = (ILVoid_t)0x004224c0;
static const ILVoid_t CenterLine_update_2d_data_o = (ILVoid_t)0x00422970;
typedef IdealLineRes*(__cdecl* ILineTry_t)(const char*, uint8_t);
static const ILineTry_t ILineTry_o = (ILineTry_t)0x004211d0;
typedef void(__cdecl* FixupRes_t)(IdealLineRes*, uint8_t);
static const FixupRes_t fixup_res_o = (FixupRes_t)0x004212b0;
typedef IdealLineRes*(__fastcall* LoadRes_t)(void*, Edx, const char*, uint8_t);
static const LoadRes_t IdealLine_load_res_o = (LoadRes_t)0x00421470;
typedef int(__fastcall* ILInt_t)(void*, Edx);
static const ILInt_t IdealLine_segloop_count_o = (ILInt_t)0x004214a0;
typedef void(__fastcall* UpdateCarInfo_t)(void*, Edx, const Point2D*, const Point2D*);
static const UpdateCarInfo_t IdealLine_update_car_info_o = (UpdateCarInfo_t)0x004214c0;
typedef ILSeg*(__fastcall* NodeAt_t)(void*, Edx, const Point2D*);
static const NodeAt_t IdealLine_nearest_node_to_o = (NodeAt_t)0x00421540;
static const NodeAt_t IdealLine_get_nearest_pair_o = (NodeAt_t)0x004215e0;
typedef double(__fastcall* NearestBead_t)(void*, Edx, const Point2D*, ILinePos*, uint32_t);   // float in ST0
static const NearestBead_t IdealLine_get_nearest_bead_o = (NearestBead_t)0x004216f0;
typedef ILinePos*(__fastcall* BeadPos_t)(void*, Edx, ILinePos*);        // struct return: hidden pointer
static const BeadPos_t IdealLine_get_actual_bead_position_o = (BeadPos_t)0x00421c10;
typedef void(__fastcall* CopyRes_t)(void*, Edx, IdealLineRes*);
static const CopyRes_t MIL_copy_res_to_loop_o = (CopyRes_t)0x00421fd0;
typedef void(__fastcall* DelSeg_t)(void*, Edx, ILSeg*);
static const DelSeg_t MIL_delete_segment_o = (DelSeg_t)0x00422140;
typedef uint8_t(__fastcall* Load_t)(void*, Edx, const char*, uint8_t);
static const Load_t ConstIdealLine_load_o = (Load_t)0x004223c0;
typedef double(__fastcall* CLDouble_t)(void*, Edx);
static const CLDouble_t CenterLine_calculate_lat_o = (CLDouble_t)0x00422a50;
typedef double(__fastcall* IlposToMeters_t)(void*, Edx, const ILinePos*);
static const IlposToMeters_t CenterLine_convert_ilpos_to_meters_o = (IlposToMeters_t)0x00422780;
typedef uint32_t(__fastcall* IlposToCookie_t)(void*, Edx, const ILinePos*);
static const IlposToCookie_t CenterLine_convert_ilpos_to_cookie_o = (IlposToCookie_t)0x00422670;
typedef ILinePos*(__fastcall* CookieToIlpos_t)(void*, Edx, ILinePos*, uint32_t);
static const CookieToIlpos_t CenterLine_convert_cookie_to_ilpos_o = (CookieToIlpos_t)0x00422620;
typedef ILinePos*(__fastcall* MetersToIlpos_t)(void*, Edx, ILinePos*, uint32_t);   // the float as bits
static const MetersToIlpos_t CenterLine_convert_meters_to_ilpos_o = (MetersToIlpos_t)0x00422710;
typedef uint8_t(__cdecl* Project_t)(const Point2D*, Point2D*);
static const Project_t project_o = (Project_t)0x00421110;
typedef void(__fastcall* SegEval_t)(const ILSeg*, Edx, Point2D*, uint32_t);   // t as bits
static const SegEval_t ILSeg_QuickEval_o = (SegEval_t)0x00426120;
static const SegEval_t ILSeg_QuickTan_o = (SegEval_t)0x00426150;

// ---- shared pieces -------------------------------------------------------------------------------------------------
// The Hermite basis the compiler inlined everywhere (advance_bead, get_rabbit_position, Draw, update_2d_data),
// each factor in a register: h00 = (2t-3)t^2+1, h01 = (-2t+3)t^2, h10 = ((t-2)t+1)t, h11 = (t-1)t^2. The
// callers then sum p*h00, v*h10, next.v*h11, next.p*h01 in their own orders (kept at each site).
struct H4 { double h00, h01, h10, h11; };
static __forceinline H4 hermite(float t) {
    H4 h;
    h.h00 = (D(t) * 2.0f - 3.0f) * t * t + 1.0f;
    h.h01 = (D(t) * -2.0f + 3.0f) * t * t;
    h.h10 = ((D(t) - 2.0f) * t + 1.0f) * t;
    h.h11 = (D(t) - 1.0f) * t * t;
    return h;
}

// every node of a loop (a footprint that doesn't know which node the call will touch)
static void fp_loop(Footprint& f, const IdealLine* l) {
    ILSeg* h = l->head;
    ILSeg* s = h;
    for (int n = 0; s && n < 4096; n++) {
        f.add(s, sizeof(ILSeg), "ILSeg");
        s = s->next;
        if (s == h) break;
    }
}
static void fp_centerline(Footprint& f, CenterLine* c) {
    f.add(c, sizeof(CenterLine), "CenterLine");
    if (c->res && c->times && c->res->count > 0) f.add(c->times, (uint32_t)c->res->count * 4, "CenterLine times");
}

// ---- the fixes' helpers (VP_FIX; only ever reached on the inputs the original crashes or hangs on) --------------
enum { FIX_MAX_NODES = 0x10000 };                     // a bound on any walk round a loop (the Pool has 512 nodes)
static __forceinline bool fix_finite(float x) { return (Ub(x) & 0x7f800000u) != 0x7f800000u; }

// is s one of the line's nodes?
static bool fix_in_loop(const IdealLine* l, const ILSeg* s) {
    const ILSeg* h = l->head;
    const ILSeg* q = h;
    for (int n = 0; q && n < FIX_MAX_NODES; n++) {
        if (q == s) return true;
        q = q->next;
        if (q == h) break;
    }
    return false;
}

// The point of the line nearest pt, measured the way the line's own nearest-search steps (get_nearest_bead's
// QuickEval): along each segment's chord, the foot of the perpendicular clamped to its ends. False if no node
// gives a distance (all of them non-finite).
static bool fix_nearest_on_line(const IdealLine* l, const Point2D* pt, ILinePos* out) {
    const ILSeg* h = l->head;
    const ILSeg* s = h;
    bool found = false;
    double best = 0.0;
    for (int n = 0; s && n < FIX_MAX_NODES; n++) {
        const ILSeg* nx = s->next;
        if (!nx) break;
        const double ax = s->p.x, az = s->p.z;
        const double dx = D(nx->p.x) - ax, dz = D(nx->p.z) - az;
        const double qx = D(pt->x) - ax, qz = D(pt->z) - az;
        const double ll = dx * dx + dz * dz;
        double t = 0.0;
        if (ll > 0.0 && ll <= DBL_MAX) {
            t = (qx * dx + qz * dz) / ll;
            if (!(t > 0.0)) t = 0.0;
            else if (t > 1.0) t = 1.0;
        }
        const double ex = qx - t * dx, ez = qz - t * dz;
        const double d = ex * ex + ez * ez;
        if (d == d && (!found || d < best)) {
            best = d;
            out->seg = (ILSeg*)s;
            out->t = (float)t;
            found = true;
        }
        s = nx;
        if (s == h) break;
    }
    return found;
}

// Put a lost (NULL) bead back on the line, so the car carries on from where it is: at the point nearest pt; with
// no point to go by (pt not finite), on the segment it was on before (prev, if it's this line's; its t if that's
// in 0..1, else 0), or else at the head, t = 0. Nothing without a head (no line).
static void fix_find_bead(IdealLine* l, const Point2D* pt, ILSeg* prev, uint32_t prev_t) {
    if (!l->head) return;
    ILinePos p;
    if (fix_finite(pt->x) && fix_finite(pt->z) && fix_nearest_on_line(l, pt, &p)) {
        l->bead_seg = p.seg;
        l->bead_t = p.t;
        return;
    }
    if (prev && fix_in_loop(l, prev)) {
        float t = Fb(prev_t);
        if (!(t >= 0.0f && t <= 1.0f)) t = 0.0f;
        l->bead_seg = prev;
        l->bead_t = t;
        return;
    }
    l->bead_seg = l->head;
    l->bead_t = 0.0f;
}

// QuickTan's degenerate case (the blended tangent has no length): the direction of the chord to the next node --
// the curve's own direction when both tangents are zero (a two-node line's); across coincident nodes, the next
// chord that has a length; (0, 1) if no two nodes differ
static void fix_chord_tan(const ILSeg* self, Point2D* out) {
    const ILSeg* a = self;
    for (int n = 0; a && n < FIX_MAX_NODES; n++) {
        const ILSeg* b = a->next;
        if (!b) break;
        const float dx = (float)(D(b->p.x) - a->p.x);
        const float dz = (float)(D(b->p.z) - a->p.z);
        const double l = x87_sqrt(D(dz) * dz + D(dx) * dx);
        if (l > 0.0 && l <= DBL_MAX) {
            out->x = (float)(D(dx) / l);
            out->z = (float)(D(dz) / l);
            return;
        }
        a = b;
        if (a == self) break;
    }
    out->x = 0.0f;
    out->z = 1.0f;
}

// ===================================================================================================================
// ideal.obj
// ===================================================================================================================

// project (0x421110): a ground point (x, z) -> the screen, 0.1 m above the terrain
static uint8_t __cdecl project(const Point2D* pt, Point2D* out) {
    P3 in, o;
    uint32_t x, z;
    cp4(&x, &pt->x);
    cp4(&z, &pt->z);
    cp4(&in.x, &x);
    cp4(&in.z, &z);
    in.y = (float)(TerrainGetHeight(x, z) + 0.1f);
    if (!mrProjectPoint(&in, &o)) return 0;
    cp4(&out->x, &o.x);
    cp4(&out->z, &o.y);
    return 1;
}
static void fp_project(Footprint& f, const Point2D*, Point2D* out) { f.add(out, 8, "out"); }
PORT_FN(0x00421110, "project", project, fp_project)

// IdealLine::IdealLine (0x421180)
static IdealLine* __fastcall IdealLine_ctor(IdealLine* self, Edx) {
    const uint32_t zero = 0;
    self->bead_seg = 0;
    cp4(&self->bead_t, &zero);
    self->vtable = (void**)0x004db6d0;
    IdealLine_init_o(self, 0);
    return self;
}
static void fp_il_ctor(Footprint& f, IdealLine* self, Edx) { f.add(self, sizeof(IdealLine), "IdealLine"); }
PORT_FN(0x00421180, "IdealLine::IdealLine", IdealLine_ctor, fp_il_ctor)

// IdealLine::~IdealLine (0x4211a0)
static void __fastcall IdealLine_dtor(IdealLine* self, Edx) { self->vtable = (void**)0x004db6d0; }
static void fp_il_dtor(Footprint& f, IdealLine* self, Edx) { f.add(self, 4, "IdealLine vtable"); f.pure = true; }
PORT_FN(0x004211a0, "IdealLine::~IdealLine", IdealLine_dtor, fp_il_dtor)

// IdealLine::init (0x4211b0)
static void __fastcall IdealLine_init(IdealLine* self, Edx) {
    const uint32_t zero = 0;
    self->reset_pending = 1;
    self->bead_on_line = 0;
    cp4(&self->bead_t, &zero);
    self->bead_seg = 0;
    self->rabbit_valid = 0;
    cp4(&self->car_pos.x, &zero);
    cp4(&self->car_pos.z, &zero);
    self->head = 0;
    self->num_segs = 0;
}
static void fp_il_init(Footprint& f, IdealLine* self, Edx) { f.add(self, sizeof(IdealLine), "IdealLine"); f.pure = true; }
PORT_FN(0x004211b0, "IdealLine::init", IdealLine_init, fp_il_init)

// ILineTry (0x4211d0): load the 'NILI' resource, check it, copy it to the heap and fix it up
static IdealLineRes* __cdecl ILineTry(const char* name, uint8_t reverse) {
    uint32_t version;
    int32_t size;
    const void* r = ResourceTryDiscardable(name, k_nili, &version, &size);
    if (!r) return 0;
    bool ok = false;
    const int32_t* h = (const int32_t*)r;
    if (version == 3) {
        int32_t magic = h[0];
        if (magic == -2) {
            int32_t seg_size = h[1];
            if (seg_size == 0x44) {
                void* p = MemAlloc(size);
                memcpy(p, r, (uint32_t)size);            // rep movsd / movsb
                ResourceForget((void*)r);
                ok = true;
                fixup_res_o((IdealLineRes*)p, reverse);
                r = p;
            } else {
                LogReport((const char*)0x004ec2dc, seg_size, 0x44);
            }
        } else {
            LogReport((const char*)0x004ec328, magic, -2, magic > 0 ? (const char*)0x004ec318 : (const char*)0x004ec324);
        }
    } else {
        LogReport((const char*)0x004ec364, version, 3);
    }
    return ok ? (IdealLineRes*)r : 0;
}
static void fp_ilinetry(Footprint& f, const char*, uint8_t) { f.replay_only = "loads a line resource (MemAlloc)"; }
PORT_FN(0x004211d0, "ILineTry", ILineTry, fp_ilinetry)

// fixup_res (0x4212b0): link the loaded nodes into a loop; reversed on request (the order, the lengths shifted
// one node, the distances summed afresh, the tangents negated, the checkpoints renumbered from 1)
static void __cdecl fixup_res(IdealLineRes* res, uint8_t reverse) {
    int32_t count = res->count;
    // FIX: a line with no nodes has nothing to link. The original still closes the loop, writing the "last"
    // node's next pointer just before the array (and, reversed, memmoves -0x44 bytes): heap corruption.
    if (VP_FIX && count <= 0) return;
    if (reverse) {
        uint8_t tmp[0x44];
        int32_t half = count / 2;                        // cdq; sub; sar: toward zero
        for (int32_t i = 0; i < half; i++) {
            ILSeg* a = res_seg(res, i);
            ILSeg* b = res_seg(res, count - i - 1);
            memcpy(tmp, a, 0x44);
            memcpy(a, b, 0x44);
            memcpy(b, tmp, 0x44);
        }
        // rotate right by one: the old last node (the old head, reversed) becomes the head again
        memcpy(tmp, res_seg(res, count - 1), 0x44);
        memmove_o(res_seg(res, 1), res_seg(res, 0), (uint32_t)(count * 0x44 - 0x44));
        memcpy(res_seg(res, 0), tmp, 0x44);
        uint32_t first_len;
        cp4(&first_len, &res_seg(res, 0)->length);
        float acc;
        const uint32_t zero = 0;
        cp4(&acc, &zero);
        int32_t last = 0;
        if (count - 1 > 0) {
            last = count - 1;
            for (int32_t k = 0; k < count - 1; k++) {
                ILSeg* a = res_seg(res, k);
                ILSeg* b = res_seg(res, k + 1);
                cp4(&a->dist, &acc);                     // mov [edx-0x40], edi
                fpu_copy(&a->length, &b->length);        // fld [edx]; fst [edx-0x44]
                acc = (float)(D(a->length) + acc);       // fadd [acc]; fstp [acc]
            }
        }
        ILSeg* l = res_seg(res, last);
        cp4(&l->dist, &acc);
        cp4(&l->length, &first_len);
        for (int32_t k = 0; k < count; k++) {
            ILSeg* s = res_seg(res, k);
            fneg_store(&s->v.x, &s->v.x);
            fneg_store(&s->v.z, &s->v.z);
        }
        uint8_t* cpp = &res_seg(res, 1)->checkpoint;
        uint32_t cur = *cpp;
        if ((int32_t)cur < 5 && (int32_t)cur > 0) {
            uint32_t n = 1;
            res_seg(res, 0)->checkpoint = 1;
            if (count > 1) {
                for (int32_t k = count - 1; k != 0; k--) {
                    uint32_t c = *cpp;
                    if (cur != c) {
                        cur = c;
                        n++;
                    }
                    *cpp = (uint8_t)n;
                    cpp += 0x44;
                }
            }
        }
    }
    for (int32_t k = 0; k < count - 1; k++) {
        ILSeg* a = res_seg(res, k);
        a->next = res_seg(res, k + 1);
        if (a->_3c == 0xfeedbeefu) a->_3c = 0;
    }
    res_seg(res, count - 1)->next = res_seg(res, 0);   // (before the array when count <= 0)
    for (int32_t k = 0; k < count; k++) res_seg(res, k)->index = (int16_t)k;
}
static void fp_fixup_res(Footprint& f, IdealLineRes* res, uint8_t) {
    if (res->count < 1) {
        if (!VP_FIX) f.replay_only = "an empty line: writes outside it";   // fixed: writes nothing
        return;
    }
    f.add(res, 0xc + (uint32_t)res->count * 0x44, "IdealLineRes");
}
PORT_FN(0x004212b0, "fixup_res", fixup_res, fp_fixup_res)

// IdealLine::load_res (0x421470)
static IdealLineRes* __fastcall IdealLine_load_res(IdealLine* self, Edx, const char* name, uint8_t reverse) {
    IdealLineRes* r = ILineTry_o(name, reverse);
    if (r) self->num_segs = r->count;
    return r;
}
static void fp_load_res(Footprint& f, IdealLine*, Edx, const char*, uint8_t) { f.replay_only = "loads a line resource"; }
PORT_FN(0x00421470, "IdealLine::load_res", IdealLine_load_res, fp_load_res)

// IdealLine::segloop_count (0x4214a0)
static int __fastcall IdealLine_segloop_count(IdealLine* self, Edx) {
    ILSeg* h = self->head;
    if (!h) return 0;
    int n = 0;
    ILSeg* s = h;
    do {
        s = s->next;
        n++;
    } while (s != h);
    return n;
}
static void fp_segloop_count(Footprint&, IdealLine*, Edx) {}
PORT_FN(0x004214a0, "IdealLine::segloop_count", IdealLine_segloop_count, fp_segloop_count)

// IdealLine::update_car_info (0x4214c0): record the car, move the bead
static void __fastcall IdealLine_update_car_info(IdealLine* self, Edx, const Point2D* pos, const Point2D* dir) {
    if (!self->head) return;
    uint32_t a, b;
    cp4(&a, &pos->x);
    cp4(&b, &pos->z);
    cp4(&self->car_pos.x, &a);
    cp4(&self->car_pos.z, &b);
    uint8_t pending = self->reset_pending;
    cp4(&a, &dir->x);
    cp4(&b, &dir->z);
    cp4(&self->car_dir.x, &a);
    cp4(&self->car_dir.z, &b);
    if (pending) {
        IdealLine_reset_bead_position_o(self, 0);
        self->reset_pending = 0;
    } else {
        IdealLine_advance_bead_o(self, 0);
    }
}
static void fp_update_car_info(Footprint& f, IdealLine* self, Edx, const Point2D*, const Point2D*) {
    f.add(self, sizeof(IdealLine), "IdealLine");
}
PORT_FN(0x004214c0, "IdealLine::update_car_info", IdealLine_update_car_info, fp_update_car_info)

// IdealLine::reset_to_head (0x421510)
static void __fastcall IdealLine_reset_to_head(IdealLine* self, Edx) {
    const uint32_t zero = 0;
    cp4(&self->bead_t, &zero);
    self->bead_seg = self->head;
}
static void fp_reset_to_head(Footprint& f, IdealLine* self, Edx) { f.add(self, sizeof(IdealLine), "IdealLine"); f.pure = true; }
PORT_FN(0x00421510, "IdealLine::reset_to_head", IdealLine_reset_to_head, fp_reset_to_head)

// IdealLine::reset_bead_position (0x421520): find the bead afresh near the car (within 10 km)
static void __fastcall IdealLine_reset_bead_position(IdealLine* self, Edx) {
    ILSeg* prev = self->bead_seg;
    const uint32_t prev_t = Ub(self->bead_t);
    self->bead_seg = 0;
    IdealLine_get_nearest_bead_o(self, 0, &self->car_pos, (ILinePos*)&self->bead_seg, k_far);   // fstp st(0)
    // FIX: get_nearest_bead found nothing (the car's position NaN, a teleport alongside no segment, a Newton walk
    // past 10 km) and the original leaves the bead NULL, for advance_bead and the AI to crash on: put it back at
    // the point of the line nearest the car (NaN: where it was, or the head).
    if (VP_FIX && !self->bead_seg) fix_find_bead(self, &self->car_pos, prev, prev_t);
}
static void fp_reset_bead_position(Footprint& f, IdealLine* self, Edx) { f.add(self, sizeof(IdealLine), "IdealLine"); }
PORT_FN(0x00421520, "IdealLine::reset_bead_position", IdealLine_reset_bead_position, fp_reset_bead_position)

// IdealLine::nearest_node_to (0x421540): the node nearest a point (the first of equals)
static ILSeg* __fastcall IdealLine_nearest_node_to(IdealLine* self, Edx, const Point2D* pt) {
    ILSeg* h = self->head;
    if (!h) return 0;
    ILSeg* s = h->next;
    ILSeg* best = h;
    float dx = (float)(D(h->p.x) - pt->x);
    double dz = D(h->p.z) - pt->z;
    float dzf = (float)dz;
    float bestd = (float)(dz * dzf + D(dx) * dx);
    for (; s != h; s = s->next) {
        float sx = (float)(D(s->p.x) - pt->x);
        double sz = D(s->p.z) - pt->z;
        float szf = (float)sz;
        double d = sz * szf + D(sx) * sx;
        float df = (float)d;
        if (!(d >= bestd)) {                              // fcom; test ah,1
            cp4(&bestd, &df);
            best = s;
        }
    }
    return best;
}
static void fp_node_at(Footprint&, IdealLine*, Edx, const Point2D*) {}
PORT_FN(0x00421540, "IdealLine::nearest_node_to", IdealLine_nearest_node_to, fp_node_at)

// IdealLine::get_nearest_pair (0x4215e0): the nearest segment the point lies alongside (ahead of its start,
// behind its end); 0 if none
static ILSeg* __fastcall IdealLine_get_nearest_pair(IdealLine* self, Edx, const Point2D* pt) {
    ILSeg* h = self->head;
    if (!h) return 0;
    if (h->next == h) return h;
    ILSeg* best = 0;
    float bestd = Fb(k_flt_max);
    const double zero = 0.0f;                             // fld [0.0]: kept in ST0 through the loop
    ILSeg* s = h;
    ILSeg* n;
    do {
        float ax = (float)(D(pt->x) - s->p.x);
        float az = (float)(D(pt->z) - s->p.z);
        n = s->next;
        float bx = (float)(D(pt->x) - n->p.x);
        double bz = D(pt->z) - n->p.z;
        float bzf = (float)bz;
        double dn = bz * bzf + D(bx) * bx;
        double ds = D(az) * az + D(ax) * ax;
        float m = (float)(ds > dn ? dn : ds);
        double dotn = D(n->v.z) * bzf + D(n->v.x) * bx;
        double dots = D(s->v.x) * ax + D(s->v.z) * az;
        if (dots > zero && !(dotn >= zero) && bestd > m) {
            cp4(&bestd, &m);
            best = s;
        }
        s = n;
    } while (n != h);
    return best;
}
PORT_FN(0x004215e0, "IdealLine::get_nearest_pair", IdealLine_get_nearest_pair, fp_node_at)

// IdealLine::get_nearest_bead (0x4216f0): slide pos along the line to the foot of the point (Newton steps on the
// tangent, up to 10); returns the metres travelled. pos->seg = 0 on giving up (beyond max_dist, or behind where
// it started in the same segment)
static float __fastcall IdealLine_get_nearest_bead(IdealLine* self, Edx, const Point2D* pt, ILinePos* pos, float max_dist) {
    if (!pos->seg) {
        ILSeg* s = IdealLine_get_nearest_pair_o(self, 0, pt);
        const uint32_t zero = 0;
        cp4(&pos->t, &zero);
        pos->seg = s;
    }
    if (!pos->seg) return 0.0f;
    ILinePos saved;
    saved.seg = pos->seg;
    cp4(&saved.t, &pos->t);
    float acc = (float)-(D(pos->seg->length) * pos->t);
    for (int i = 0; i < 10; i++) {
        Point2D e, tan;
        ILSeg_QuickEval_o(pos->seg, 0, &e, Ub(pos->t));
        ILSeg_QuickTan_o(pos->seg, 0, &tan, Ub(pos->t));
        double d = (D(pt->x) - e.x) * tan.x + (D(pt->z) - e.z) * tan.z;
        float df = (float)d;
        if (!(fabs(d) >= k_eps)) goto done;
        pos->t = (float)(D(df) / pos->seg->length + pos->t);
        if (I(pos->t) > 0x3f800000) {                     // signed: t > 1 (or a positive NaN)
            do {
                ILSeg* s = pos->seg;
                double a = D(s->length) + acc;
                acc = (float)a;
                if (a > max_dist) {
                    pos->seg = 0;
                    goto done;
                }
                pos->seg = s->next;
                const float tm = pos->t - 1.0f;
                // FIX: a t that 1 can't be taken from (infinite or >= 2^24: a zero or near-zero segment length; or
                // NaN) looped forever where the lengths don't reach max_dist (zero, or max_dist NaN): on at the
                // next node's start
                if (VP_FIX && !(tm < pos->t)) {
                    const uint32_t zero = 0;
                    cp4(&pos->t, &zero);
                    break;
                }
                pos->t = tm;
            } while (I(pos->t) > 0x3f800000);
        }
        if (Ub(pos->t) > 0x80000000u) {                   // below -0
            const uint32_t zero = 0;
            cp4(&pos->t, &zero);
            goto done;
        }
    }
done:
    if (pos->seg) acc = (float)(D(pos->seg->length) * pos->t + acc);
    if (saved.seg == pos->seg && !(pos->t >= saved.t)) pos->seg = 0;
    return acc;
}
static void fp_get_nearest_bead(Footprint& f, IdealLine*, Edx, const Point2D*, ILinePos* pos, float) { f.add(pos, 8, "ILinePos"); }
PORT_FN(0x004216f0, "IdealLine::get_nearest_bead", IdealLine_get_nearest_bead, fp_get_nearest_bead)

// IdealLine::advance_bead (0x421840): follow the car along the line -- up to 10 Newton steps of the bead's t
// on the tangent, moving on to the next node past t = 1 and back one node below 0 (once; 0 at the head).
// Off the line (the car > 150 m from the bead, or no way back) clears bead_on_line. The original reads the bead
// unchecked: NULL is the stock AI crash (fixed below).
static void __fastcall IdealLine_advance_bead(IdealLine* self, Edx) {
    (*i_pr_overhead_begin)();
    int prof = (*i_prof_start)((const char*)0x004ec388);     // "adv_bead"
    (*i_pr_overhead_end)();
    ILSeg* prev = 0;
    ILSeg* s = self->bead_seg;
    // FIX: the lost bead (NULL: reset_bead_position after a NaN position or a teleport) is the stock AI crash,
    // s->next read unchecked: put it back at the point of the line nearest the car first. (A line with no nodes at
    // all is left as the original has it; update_car_info never calls this for one.)
    if (VP_FIX && !s && self->head) {
        fix_find_bead(self, &self->car_pos, 0, 0);
        s = self->bead_seg;
    }
    float t;
    cp4(&t, &self->bead_t);
    uint8_t off = 0;
    for (int i = 0; i < 10; i++) {
        H4 h = hermite(t);
        ILSeg* n = s->next;
        float px = (float)(((D(s->p.x) * h.h00 + D(s->v.x) * h.h10) + D(n->v.x) * h.h11) + D(n->p.x) * h.h01);
        float pz = (float)(((D(s->p.z) * h.h00 + D(n->v.z) * h.h11) + D(n->p.z) * h.h01) + D(s->v.z) * h.h10);
        Point2D tan;
        ILSeg_QuickTan_o(s, 0, &tan, Ub(t));
        float dx = (float)(D(self->car_pos.x) - px);
        double dz = D(self->car_pos.z) - pz;
        float dzf = (float)dz;
        if (dz * dzf + D(dx) * dx > 22500.0f) {
            off = 1;
            break;
        }
        double dot = D(dzf) * tan.z + D(dx) * tan.x;
        float dotf = (float)dot;
        if (!(fabs(dot) >= k_eps)) break;
        double r = D(dotf) / s->length + t;
        t = (float)r;
        if (r > 1.0f) {
            do {
                const float tm = t - 1.0f;
                prev = s;
                s = s->next;
                // FIX: a t that 1 can't be taken from (infinite or >= 2^24: a zero-length node's dot / 0) looped
                // forever: on at the next node's start (the bead passes a zero-length node)
                if (VP_FIX && !(tm < t)) {
                    t = 0.0f;
                    break;
                }
                t = tm;
            } while (t > 1.0f);
        }
        if (Ub(t) > 0x80000000u) {                        // below -0: back one node
            if (!prev) {
                const uint32_t zero = 0;
                cp4(&t, &zero);
                off = 1;
                break;
            }
            double r2 = D(t) + 1.0f;
            s = prev;
            prev = 0;
            t = (float)r2;
            if (!(r2 >= 0.0f)) {
                const uint32_t zero = 0;
                cp4(&t, &zero);
            }
        }
    }
    self->bead_seg = s;
    cp4(&self->bead_t, &t);
    self->bead_on_line = off < 1 ? 1 : 0;
    (*i_pr_overhead_begin)();
    (*i_prof_stop)(prof);
    (*i_pr_overhead_end)();
}
static void fp_advance_bead(Footprint& f, IdealLine* self, Edx) { f.add(self, sizeof(IdealLine), "IdealLine"); }
PORT_FN(0x00421840, "IdealLine::advance_bead", IdealLine_advance_bead, fp_advance_bead)

// IdealLine::get_rabbit_position (0x421a70): the point dist metres (of the bead's segment) ahead of the bead,
// and the direction there scaled by the interpolated target speed
static void __fastcall IdealLine_get_rabbit_position(IdealLine* self, Edx, Point2D* pos, Point2D* dir, float dist, ILinePos* out) {
    ILSeg* s = self->bead_seg;
    if (!s) {
        const uint32_t zero = 0, one = 0x3f800000u;
        cp4(&pos->z, &zero);
        cp4(&pos->x, &zero);
        cp4(&dir->z, &one);
        cp4(&dir->x, &zero);
        return;
    }
    double r = D(dist) / s->length + self->bead_t;
    float t = (float)r;
    if (r > 1.0f) {
        do {
            const float tm = t - 1.0f;
            s = s->next;
            // FIX: a t that 1 can't be taken from (infinite or >= 2^24: dist over a zero-length node) looped
            // forever: the rabbit goes to the next node's start
            if (VP_FIX && !(tm < t)) {
                t = 0.0f;
                break;
            }
            t = tm;
        } while (t > 1.0f);
    }
    H4 h = hermite(t);
    ILSeg* n = s->next;
    pos->x = (float)(((D(s->p.x) * h.h00 + D(s->v.x) * h.h10) + D(n->v.x) * h.h11) + D(n->p.x) * h.h01);
    pos->z = (float)(((D(n->v.z) * h.h11 + D(n->p.z) * h.h01) + D(s->v.z) * h.h10) + D(s->p.z) * h.h00);
    ILSeg_QuickTan_o(s, 0, dir, Ub(t));
    if (out) {
        out->seg = s;
        cp4(&out->t, &t);
    }
    n = s->next;
    double sp = (1.0f - D(t)) * s->speed + D(n->speed) * t;
    dir->x = (float)(D(dir->x) * sp);
    dir->z = (float)(sp * dir->z);
    uint32_t a, b;
    cp4(&a, &pos->x);
    cp4(&b, &pos->z);
    cp4(&self->rabbit.x, &a);
    cp4(&self->rabbit.z, &b);
    self->rabbit_valid = 1;
}
static void fp_get_rabbit_position(Footprint& f, IdealLine* self, Edx, Point2D* pos, Point2D* dir, float, ILinePos* out) {
    f.add(self, sizeof(IdealLine), "IdealLine");
    f.add(pos, 8, "pos");
    f.add(dir, 8, "dir");
    if (out) f.add(out, 8, "ILinePos");
}
PORT_FN(0x00421a70, "IdealLine::get_rabbit_position", IdealLine_get_rabbit_position, fp_get_rabbit_position)

// IdealLine::get_actual_bead_position (0x421c10)
static ILinePos* __fastcall IdealLine_get_actual_bead_position(IdealLine* self, Edx, ILinePos* ret) {
    uint32_t t;
    cp4(&t, &self->bead_t);
    ret->seg = self->bead_seg;
    cp4(&ret->t, &t);
    return ret;
}
static void fp_bead_position(Footprint& f, IdealLine*, Edx, ILinePos* ret) { f.add(ret, 8, "ILinePos"); }
PORT_FN(0x00421c10, "IdealLine::get_actual_bead_position", IdealLine_get_actual_bead_position, fp_bead_position)

// a ground point to the screen for Draw: TerrainGetHeight + 0.1, then mrProjectPoint
static __forceinline uint8_t draw_project(uint32_t x, uint32_t z, P3* o) {
    P3 in;
    double hgt = TerrainGetHeight(x, z) + 0.1f;
    cp4(&in.x, &x);
    in.y = (float)hgt;
    cp4(&in.z, &z);
    return mrProjectPoint(&in, o);
}

// IdealLine::Draw (0x421c30): the debug overlay -- a circle on the node nearest `pos`, the segments (unless
// no_lines), the rabbit and the bead
static void __fastcall IdealLine_Draw(IdealLine* self, Edx, const Point2D* pos, uint8_t no_lines) {
    ILSeg* s = self->head;
    ILSeg* nearest = IdealLine_nearest_node_to_o(self, 0, pos);
    if (s) {
        do {
            if (s == nearest) {
                P3 o;
                if (draw_project(Ub(s->p.x), Ub(s->p.z), &o)) {
                    uint32_t col = *g_col_node;
                    int y = x87_ftol(o.y);
                    int x = x87_ftol(o.x);
                    gxCircle(x, y, 5, col);
                }
            }
            *g_draw_head_flag = self->head == s ? 1 : 0;
            if (!no_lines) ILSeg_DrawLine_o(s, 0);
            s = s->next;
        } while (self->head != s);
    }
    if (self->rabbit_valid) {
        P3 o;
        if (draw_project(Ub(self->rabbit.x), Ub(self->rabbit.z), &o)) {
            uint32_t col = *g_col_bead;
            int y = x87_ftol(o.y);
            int x = x87_ftol(o.x);
            gxCircle(x, y, 5, col);
        }
    }
    ILSeg* b = self->bead_seg;
    if (b) {
        H4 h = hermite(self->bead_t);
        ILSeg* n = b->next;
        float z = (float)(((D(n->p.z) * h.h01 + D(n->v.z) * h.h11) + D(b->p.z) * h.h00) + D(b->v.z) * h.h10);
        float x = (float)(((D(b->v.x) * h.h10 + D(b->p.x) * h.h00) + D(n->v.x) * h.h11) + D(n->p.x) * h.h01);
        P3 o;
        if (draw_project(Ub(x), Ub(z), &o)) {
            uint32_t col = *g_col_bead;
            int sy = x87_ftol(o.y);
            int sx = x87_ftol(o.x);
            gxCircle(sx, sy, 4, col);
        }
    }
}
static void fp_draw(Footprint& f, IdealLine*, Edx, const Point2D*, uint8_t) { f.replay_only = "draws (gxCircle, __DrawLine)"; }
PORT_FN(0x00421c30, "IdealLine::Draw", IdealLine_Draw, fp_draw)

// MutableIdealLine::MutableIdealLine (0x421e60): an empty line with a pool of 512 nodes
static MutableIdealLine* __fastcall MIL_ctor(MutableIdealLine* self, Edx) {
    IdealLine_ctor_o(self, 0);
    PoolBase_ctor(&self->pool, 0, (const char*)0x004ec394, 0x200, 0x44);   // "IdealLine's Pool"
    self->pool.vtable = (void**)0x004db6d8;              // Pool<ILSeg>
    self->vtable = (void**)0x004db6e0;
    return self;
}
static void fp_mil_ctor(Footprint& f, MutableIdealLine*, Edx) { f.replay_only = "allocates the node pool"; }
PORT_FN(0x00421e60, "MutableIdealLine::MutableIdealLine", MIL_ctor, fp_mil_ctor)

// MutableIdealLine::~MutableIdealLine (0x421e90)
static void __fastcall MIL_dtor(MutableIdealLine* self, Edx) {
    self->vtable = (void**)0x004db6e0;
    MIL_clear_o(self, 0);
    PoolBase_dtor(&self->pool, 0);
    IdealLine_dtor_o(self, 0);
}
static void fp_mil_dtor(Footprint& f, MutableIdealLine*, Edx) { f.replay_only = "frees the node pool"; }
PORT_FN(0x00421e90, "MutableIdealLine::~MutableIdealLine", MIL_dtor, fp_mil_dtor)

// MutableIdealLine::save (0x421eb0): the line editor's save. Its 12-byte header is written from a local of
// which only the count is ever set: the magic and node size it should carry are whatever was on the stack
// (the original's own bug, not reproducible bit for bit -- the file is a replay_only output)
static void __fastcall MIL_save(MutableIdealLine* self, Edx, const char* name) {
    if (!self->head) {
        LogReport((const char*)0x004ec3b8);
        return;
    }
    int fh = FileCreate(name);
    if (!fh) {
        LogReport((const char*)0x004ec3a8, name);
        return;
    }
    ResourceWrite(fh, k_nili, 3);
    uint32_t hdr[3];
    hdr[2] = (uint32_t)IdealLine_segloop_count_o(self, 0);
    FileWrite(fh, hdr, 12);
    ILSeg* s = self->head;
    do {
        FileWrite(fh, s, 0x44);
        s = s->next;
    } while (self->head != s);
    FileClose(&fh);
}
static void fp_mil_save(Footprint& f, MutableIdealLine*, Edx, const char*) { f.replay_only = "writes a file"; }
PORT_FN(0x00421eb0, "MutableIdealLine::save", MIL_save, fp_mil_save)

// MutableIdealLine::clear (0x421f60)
static void __fastcall MIL_clear(MutableIdealLine* self, Edx) {
    ILSeg* s = self->head;
    if (!s) return;
    ILSeg* n;
    do {
        n = s->next;
        PoolBase_free(&self->pool, 0, s);
        s = n;
    } while (self->head != n);
    self->head = 0;
}
static void fp_mil_clear(Footprint& f, MutableIdealLine*, Edx) { f.replay_only = "frees nodes to the pool"; }
PORT_FN(0x00421f60, "MutableIdealLine::clear", MIL_clear, fp_mil_clear)

// MutableIdealLine::load (0x421f90)
static uint8_t __fastcall MIL_load(MutableIdealLine* self, Edx, const char* name, uint8_t reverse) {
    IdealLineRes* r = IdealLine_load_res_o(self, 0, name, reverse);
    if (!r) return 0;
    MIL_copy_res_to_loop_o(self, 0, r);
    MemFree(r);
    return 1;
}
static void fp_mil_load(Footprint& f, MutableIdealLine*, Edx, const char*, uint8_t) { f.replay_only = "loads a line resource"; }
PORT_FN(0x00421f90, "MutableIdealLine::load", MIL_load, fp_mil_load)

// MutableIdealLine::copy_res_to_loop (0x421fd0): each resource node into a pool node, linked in a loop.
// (Out of nodes: LogPanic, and the same node is tried again.)
static void __fastcall MIL_copy_res_to_loop(MutableIdealLine* self, Edx, IdealLineRes* res) {
    ILSeg* first = res_seg(res, 0);
    ILSeg* src = first;
    ILSeg** link = &self->head;
    do {
        ILSeg* p = (ILSeg*)PoolBase_alloc(&self->pool, 0);
        *link = p;
        if (p) {
            memcpy(p, src, 0x44);
            (*link)->next = self->head;
            link = (ILSeg**)*link;                        // &p->next
            src = src->next;
        } else {
            LogPanic((const char*)0x004ec3d4);
        }
    } while (src != first);
}
static void fp_copy_res(Footprint& f, MutableIdealLine*, Edx, IdealLineRes*) { f.replay_only = "takes nodes from the pool"; }
PORT_FN(0x00421fd0, "MutableIdealLine::copy_res_to_loop", MIL_copy_res_to_loop, fp_copy_res)

// MutableIdealLine::modify_speed_everywhere (0x422040)
static void __fastcall MIL_modify_speed_everywhere(MutableIdealLine* self, Edx, float k) {
    ILSeg* s = self->head;
    if (!s) return;
    do {
        s->speed = (float)(D(s->speed) * k);
        s = s->next;
    } while (self->head != s);
}
static void fp_mil_everywhere(Footprint& f, MutableIdealLine* self, Edx, float) { fp_loop(f, self); }
PORT_FN(0x00422040, "MutableIdealLine::modify_speed_everywhere", MIL_modify_speed_everywhere, fp_mil_everywhere)

// MutableIdealLine::modify_vscale_everywhere (0x422060)
static void __fastcall MIL_modify_vscale_everywhere(MutableIdealLine* self, Edx, float k) {
    ILSeg* s = self->head;
    if (!s) return;
    do {
        s->v.x = (float)(D(s->v.x) * k);
        s->v.z = (float)(D(s->v.z) * k);
        s = s->next;
    } while (self->head != s);
}
PORT_FN(0x00422060, "MutableIdealLine::modify_vscale_everywhere", MIL_modify_vscale_everywhere, fp_mil_everywhere)

// MutableIdealLine::delete_control_near (0x422090)
static void __fastcall MIL_delete_control_near(MutableIdealLine* self, Edx, const Point2D* pt) {
    ILSeg* s = IdealLine_nearest_node_to_o(self, 0, pt);
    if (s) MIL_delete_segment_o(self, 0, s);
}
static void fp_mil_delete_near(Footprint& f, MutableIdealLine*, Edx, const Point2D*) { f.replay_only = "frees a node to the pool"; }
PORT_FN(0x00422090, "MutableIdealLine::delete_control_near", MIL_delete_control_near, fp_mil_delete_near)

// MutableIdealLine::modify_speed_at (0x4220b0)
static void __fastcall MIL_modify_speed_at(MutableIdealLine* self, Edx, const Point2D* pt, float speed) {
    ILSeg* s = IdealLine_nearest_node_to_o(self, 0, pt);
    if (s) cp4(&s->speed, &speed);
}
static void fp_mil_at_f(Footprint& f, MutableIdealLine* self, Edx, const Point2D*, float) { fp_loop(f, self); }
PORT_FN(0x004220b0, "MutableIdealLine::modify_speed_at", MIL_modify_speed_at, fp_mil_at_f)

// MutableIdealLine::modify_v_at (0x4220d0)
static void __fastcall MIL_modify_v_at(MutableIdealLine* self, Edx, const Point2D* pt, const Point2D* v, float speed) {
    ILSeg* s = IdealLine_nearest_node_to_o(self, 0, pt);
    if (!s) return;
    uint32_t a, b;
    cp4(&a, &v->x);
    cp4(&b, &v->z);
    cp4(&s->v.x, &a);
    cp4(&s->v.z, &b);
    cp4(&s->speed, &speed);
}
static void fp_mil_v_at(Footprint& f, MutableIdealLine* self, Edx, const Point2D*, const Point2D*, float) { fp_loop(f, self); }
PORT_FN(0x004220d0, "MutableIdealLine::modify_v_at", MIL_modify_v_at, fp_mil_v_at)

// MutableIdealLine::modify_wid_at (0x422100)
static void __fastcall MIL_modify_wid_at(MutableIdealLine* self, Edx, const Point2D* pt, float dw) {
    ILSeg* s = IdealLine_nearest_node_to_o(self, 0, pt);
    if (s) s->width = (float)(D(s->width) + dw);
}
PORT_FN(0x00422100, "MutableIdealLine::modify_wid_at", MIL_modify_wid_at, fp_mil_at_f)

// MutableIdealLine::delete_last_node (0x422120)
static void __fastcall MIL_delete_last_node(MutableIdealLine* self, Edx) {
    ILSeg* h = self->head;
    if (!h || h->next == h) return;
    ILSeg* s = h;
    do s = s->next;
    while (s->next != h);
    MIL_delete_segment_o(self, 0, s);
}
static void fp_mil_delete_last(Footprint& f, MutableIdealLine*, Edx) { f.replay_only = "frees a node to the pool"; }
PORT_FN(0x00422120, "MutableIdealLine::delete_last_node", MIL_delete_last_node, fp_mil_delete_last)

// MutableIdealLine::delete_segment (0x422140): unlink a node (not checked to be in the loop) and free it
static void __fastcall MIL_delete_segment(MutableIdealLine* self, Edx, ILSeg* s) {
    ILSeg* h = self->head;
    if (s == h) {
        ILSeg* n = h->next;
        if (n == h) {
            PoolBase_free(&self->pool, 0, h);
            self->head = 0;
            return;
        }
        self->head = n;
    }
    ILSeg* p = self->head;
    while (p->next != s) p = p->next;
    p->next = s->next;
    PoolBase_free(&self->pool, 0, s);
}
static void fp_mil_delete_seg(Footprint& f, MutableIdealLine*, Edx, ILSeg*) { f.replay_only = "frees a node to the pool"; }
PORT_FN(0x00422140, "MutableIdealLine::delete_segment", MIL_delete_segment, fp_mil_delete_seg)

// MutableIdealLine::get_speed_at (0x422190) / get_wid_at (0x4221b0): -1 on an empty line
static float __fastcall MIL_get_speed_at(MutableIdealLine* self, Edx, const Point2D* pt) {
    ILSeg* s = IdealLine_nearest_node_to_o(self, 0, pt);
    return s ? s->speed : -1.0f;
}
static void fp_mil_get_at(Footprint&, MutableIdealLine*, Edx, const Point2D*) {}
PORT_FN(0x00422190, "MutableIdealLine::get_speed_at", MIL_get_speed_at, fp_mil_get_at)

static float __fastcall MIL_get_wid_at(MutableIdealLine* self, Edx, const Point2D* pt) {
    ILSeg* s = IdealLine_nearest_node_to_o(self, 0, pt);
    return s ? s->width : -1.0f;
}
PORT_FN(0x004221b0, "MutableIdealLine::get_wid_at", MIL_get_wid_at, fp_mil_get_at)

// MutableIdealLine::snap_to (0x4221d0): move the nearest node to the point
static void __fastcall MIL_snap_to(MutableIdealLine* self, Edx, const Point2D* pt) {
    ILSeg* s = IdealLine_nearest_node_to_o(self, 0, pt);
    if (!s) return;
    uint32_t a, b;
    cp4(&a, &pt->x);
    cp4(&b, &pt->z);
    cp4(&s->p.x, &a);
    cp4(&s->p.z, &b);
}
static void fp_mil_snap(Footprint& f, MutableIdealLine* self, Edx, const Point2D*) { fp_loop(f, self); }
PORT_FN(0x004221d0, "MutableIdealLine::snap_to", MIL_snap_to, fp_mil_snap)

// MutableIdealLine::autostop (0x422200): more than 5 nodes, and the last within 50 m of the head
static uint8_t __fastcall MIL_autostop(MutableIdealLine* self, Edx) {
    int n = 0;
    ILSeg* h = self->head;
    ILSeg* s = h;
    if (s && s->next != h) {
        do {
            n++;
            s = s->next;
        } while (s->next != h);
    }
    if (n <= 4) return 0;
    float dx = (float)(D(s->p.x) - h->p.x);
    double dz = D(s->p.z) - h->p.z;
    float dzf = (float)dz;
    double d = x87_sqrt(dz * dzf + D(dx) * dx);
    return !(d >= 50.0f) ? 1 : 0;
}
static void fp_mil_autostop(Footprint&, MutableIdealLine*, Edx) {}
PORT_FN(0x00422200, "MutableIdealLine::autostop", MIL_autostop, fp_mil_autostop)

// MutableIdealLine::add_next_control (0x422260): a new node after the last
static void __fastcall MIL_add_next_control(MutableIdealLine* self, Edx, const Point2D* pt, float width, int checkpoint) {
    ILSeg* p = (ILSeg*)PoolBase_alloc(&self->pool, 0);
    if (!p) {
        LogReport((const char*)0x004ec3e4);
        return;
    }
    uint32_t a, b;
    const uint32_t zero = 0;
    cp4(&a, &pt->x);
    cp4(&b, &pt->z);
    cp4(&p->p.x, &a);
    cp4(&p->p.z, &b);
    cp4(&p->width, &width);
    p->checkpoint = (uint8_t)checkpoint;
    cp4(&p->v.x, &k_ten);
    cp4(&p->v.z, &zero);
    ILSeg* h = self->head;
    if (h) {
        ILSeg* e = h;
        if (h->next != h) {
            do e = e->next;
            while (e->next != h);
        }
        e->next = p;
        p->next = self->head;
    } else {
        self->head = p;
        p->next = p;
    }
}
static void fp_mil_add_next(Footprint& f, MutableIdealLine*, Edx, const Point2D*, float, int) { f.replay_only = "takes a node from the pool"; }
PORT_FN(0x00422260, "MutableIdealLine::add_next_control", MIL_add_next_control, fp_mil_add_next)

// MutableIdealLine::add_control_near (0x4222e0): a new node after the one nearest the point
static void __fastcall MIL_add_control_near(MutableIdealLine* self, Edx, const Point2D* pt, float width) {
    ILSeg* p = (ILSeg*)PoolBase_alloc(&self->pool, 0);
    if (!p) {
        LogReport((const char*)0x004ec400);
        return;
    }
    uint32_t a, b;
    const uint32_t zero = 0;
    cp4(&a, &pt->x);
    cp4(&b, &pt->z);
    cp4(&p->p.x, &a);
    cp4(&p->p.z, &b);
    cp4(&p->width, &width);
    cp4(&p->v.x, &k_ten);
    cp4(&p->v.z, &zero);
    ILSeg* h = self->head;
    if (!h) {
        self->head = p;
        p->next = p;
        return;
    }
    if (h->next == h) {
        h->next = p;
        p->next = self->head;
        return;
    }
    ILSeg* nn = IdealLine_nearest_node_to_o(self, 0, pt);
    p->next = nn->next;
    nn->next = p;
}
static void fp_mil_add_near(Footprint& f, MutableIdealLine*, Edx, const Point2D*, float) { f.replay_only = "takes a node from the pool"; }
PORT_FN(0x004222e0, "MutableIdealLine::add_control_near", MIL_add_control_near, fp_mil_add_near)

// ConstIdealLine::ConstIdealLine (0x422370)
static ConstIdealLine* __fastcall ConstIdealLine_ctor(ConstIdealLine* self, Edx) {
    IdealLine_ctor_o(self, 0);
    self->vtable = (void**)0x004db6e8;
    self->res = 0;
    return self;
}
static void fp_cil_ctor(Footprint& f, ConstIdealLine* self, Edx) { f.add(self, sizeof(ConstIdealLine), "ConstIdealLine"); }
PORT_FN(0x00422370, "ConstIdealLine::ConstIdealLine", ConstIdealLine_ctor, fp_cil_ctor)

// ConstIdealLine::~ConstIdealLine (0x422390)
static void __fastcall ConstIdealLine_dtor(ConstIdealLine* self, Edx) {
    IdealLineRes* r = self->res;
    self->vtable = (void**)0x004db6e8;
    if (r) {
        MemFree(r);
        self->res = 0;
    }
    IdealLine_dtor_o(self, 0);
}
static void fp_cil_dtor(Footprint& f, ConstIdealLine*, Edx) { f.replay_only = "frees the line (MemFree)"; }
PORT_FN(0x00422390, "ConstIdealLine::~ConstIdealLine", ConstIdealLine_dtor, fp_cil_dtor)

// ConstIdealLine::load (0x4223c0): the resource is the line (its nodes linked in place)
static uint8_t __fastcall ConstIdealLine_load(ConstIdealLine* self, Edx, const char* name, uint8_t reverse) {
    if (self->res) MemFree(self->res);
    IdealLineRes* r = IdealLine_load_res_o(self, 0, name, reverse);
    self->res = r;
    if (r) self->head = res_seg(r, 0);
    return r != 0;
}
static void fp_cil_load(Footprint& f, ConstIdealLine*, Edx, const char*, uint8_t) { f.replay_only = "loads a line resource"; }
PORT_FN(0x004223c0, "ConstIdealLine::load", ConstIdealLine_load, fp_cil_load)

// CenterLine::CenterLine (0x422400): track.ild, or dlatlong.ili, reversed as the options say
static CenterLine* __fastcall CenterLine_ctor(CenterLine* self, Edx, const uint8_t* reverse) {
    ConstIdealLine_ctor_o(self, 0);
    self->vtable = (void**)0x004db6f0;
    uint8_t rev = reverse ? *reverse : WorldGameOptions()[0x25];
    if (!ConstIdealLine_load_o(self, 0, (const char*)0x004ec41c, rev))          // "track.ild"
        ConstIdealLine_load_o(self, 0, (const char*)0x004ec428, rev);           // "dlatlong.ili"
    IdealLineRes* r = self->res;
    self->times = 0;
    self->time_seg = 0;
    self->started = 0;
    if (r && r->count) {
        ILSeg* last = res_seg(r, r->count - 1);
        self->total = (float)(D(last->dist) + last->length);
        self->times = (float*)MemAlloc(r->count * 4);
        CenterLine_reset_o(self, 0);
    }
    return self;
}
static void fp_cl_ctor(Footprint& f, CenterLine*, Edx, const uint8_t*) { f.replay_only = "loads the centre line, allocates"; }
PORT_FN(0x00422400, "CenterLine::CenterLine", CenterLine_ctor, fp_cl_ctor)

// CenterLine::~CenterLine (0x422490)
static void __fastcall CenterLine_dtor(CenterLine* self, Edx) {
    float* t = self->times;
    self->vtable = (void**)0x004db6f0;
    if (t) {
        op_delete(t);
        self->times = 0;
    }
    ConstIdealLine_dtor_o(self, 0);
}
static void fp_cl_dtor(Footprint& f, CenterLine*, Edx) { f.replay_only = "frees the centre line"; }
PORT_FN(0x00422490, "CenterLine::~CenterLine", CenterLine_dtor, fp_cl_dtor)

// CenterLine::reset (0x4224c0)
static void __fastcall CenterLine_reset(CenterLine* self, Edx) {
    IdealLine_reset_bead_position_o(self, 0);
    self->reset_pending = 1;
    for (int32_t i = 0; i < self->res->count; i++) {
        double tm = PhysicsGetTime() + 1000.0f;
        self->times[i] = (float)tm;
    }
    self->lat = (float)CenterLine_calculate_lat_o(self, 0);
    const uint32_t zero = 0;
    self->started = 0;
    cp4(&self->lat_delta, &zero);
}
static void fp_cl_reset(Footprint& f, CenterLine* self, Edx) { fp_centerline(f, self); }
PORT_FN(0x004224c0, "CenterLine::reset", CenterLine_reset, fp_cl_reset)

// CenterLine::update (0x422510): move the bead after the car, stamp the nodes it passed, the lateral offset;
// returns whether the bead changed checkpoint. (The original reads bead_seg unchecked, before and after.)
static uint8_t __fastcall CenterLine_update(CenterLine* self, Edx, const Point2D* pos, int car_index) {
    self->car_index = car_index;
    if (!self->head) return 0;
    float now = (float)PhysicsGetTime();
    // FIX: a lost (NULL) bead -- CenterLine::reset from a position alongside no segment (a line that doesn't claim
    // the origin at the grid), a NaN position -- was read here (the checkpoint) and crashed: put it back at the
    // point of the line nearest the car first
    if (VP_FIX && !self->bead_seg) fix_find_bead(self, pos, 0, 0);
    uint32_t cp0 = self->bead_seg->checkpoint;
    Point2D zero;
    const uint32_t z = 0;
    cp4(&zero.x, &z);
    cp4(&zero.z, &z);
    IdealLine_update_car_info_o(self, 0, pos, &zero);
    ILSeg* b = self->bead_seg;
    // FIX: the same after update_car_info (its reset_bead_position, were that left to the original)
    if (VP_FIX && !b) {
        fix_find_bead(self, pos, 0, 0);
        b = self->bead_seg;
    }
    uint8_t changed = (uint32_t)b->checkpoint != cp0 ? 1 : 0;
    if (!self->started) {
        if (self->head == b) {
            self->started = 1;
            self->time_seg = b;
        }
    } else if (self->time_seg != b) {
        do {
            int32_t k = (int32_t)((uint32_t)(uintptr_t)self->time_seg - (uint32_t)(uintptr_t)self->res - 0xc) / 0x44;
            cp4(&self->times[k], &now);
            self->time_seg = self->time_seg->next;
        } while (self->bead_seg != self->time_seg);
    }
    CenterLine_update_2d_data_o(self, 0);
    float old;
    cp4(&old, &self->lat);
    double lat = CenterLine_calculate_lat_o(self, 0);
    self->lat = (float)lat;
    self->lat_delta = (float)(lat - old);
    return changed;
}
static void fp_cl_update(Footprint& f, CenterLine* self, Edx, const Point2D*, int) { fp_centerline(f, self); }
PORT_FN(0x00422510, "CenterLine::update", CenterLine_update, fp_cl_update)

// CenterLine::get_car_dlong_meters (0x4225e0) / get_car_dlong_cookie (0x422600): the bead's
static double __fastcall CenterLine_get_car_dlong_meters(CenterLine* self, Edx) {
    // FIX: a lost (NULL) bead crashed convert_ilpos_to_meters (seg->length): put it back at the point of the line
    // nearest the car. (No line at all is left as the original has it.)
    if (VP_FIX && !self->bead_seg && self->head) fix_find_bead(self, &self->car_pos, 0, 0);
    ILinePos p;
    p.seg = self->bead_seg;
    cp4(&p.t, &self->bead_t);
    return CenterLine_convert_ilpos_to_meters_o(self, 0, &p);
}
static void fp_cl_read(Footprint&, CenterLine*, Edx) {}
static void fp_cl_read_bead(Footprint& f, CenterLine* self, Edx) { f.add(self, sizeof(IdealLine), "IdealLine (a lost bead found)"); }
PORT_FN(0x004225e0, "CenterLine::get_car_dlong_meters", CenterLine_get_car_dlong_meters, fp_cl_read_bead)

static uint32_t __fastcall CenterLine_get_car_dlong_cookie(CenterLine* self, Edx) {
    // FIX: a lost (NULL) bead gave a cookie from a garbage node index (the NULL's offset from the resource), which
    // the deity turns back into a node pointer: put it back at the point of the line nearest the car. (No line at
    // all is left as the original has it.)
    if (VP_FIX && !self->bead_seg && self->head) fix_find_bead(self, &self->car_pos, 0, 0);
    ILinePos p;
    p.seg = self->bead_seg;
    cp4(&p.t, &self->bead_t);
    return CenterLine_convert_ilpos_to_cookie_o(self, 0, &p);
}
PORT_FN(0x00422600, "CenterLine::get_car_dlong_cookie", CenterLine_get_car_dlong_cookie, fp_cl_read_bead)

// CenterLine::convert_cookie_to_ilpos (0x422620)
static ILinePos* __fastcall CenterLine_convert_cookie_to_ilpos(CenterLine* self, Edx, ILinePos* ret, uint32_t cookie) {
    ILSeg* s = (ILSeg*)((uint8_t*)self->res + (cookie >> 16) * 0x44 + 0xc);
    double f = (double)(int32_t)(cookie & 0xffff);       // fild qword: exact
    ret->seg = s;
    ret->t = (float)(f * k_cookie_scale);
    return ret;
}
static void fp_cl_to_ilpos(Footprint& f, CenterLine*, Edx, ILinePos* ret, uint32_t) { f.add(ret, 8, "ILinePos"); }
PORT_FN(0x00422620, "CenterLine::convert_cookie_to_ilpos", CenterLine_convert_cookie_to_ilpos, fp_cl_to_ilpos)

// CenterLine::convert_ilpos_to_cookie (0x422670): (node index << 16) | t * 65535
static uint32_t __fastcall CenterLine_convert_ilpos_to_cookie(CenterLine* self, Edx, const ILinePos* pos) {
    int32_t idx = (int32_t)((uint32_t)(uintptr_t)pos->seg - (uint32_t)(uintptr_t)self->res - 0xc) / 0x44;
    ASSERT_MSG_o(Ub(pos->t) <= 0x80000000u ? 1 : 0, (const char*)0x004ec448, D(pos->t));
    ASSERT_MSG_o(I(pos->t) <= 0x3f800001 ? 1 : 0, (const char*)0x004ec464, D(pos->t));
    int32_t f = x87_ftol(D(pos->t) * 65535.0f);
    return ((uint32_t)f & 0xffffu) | ((uint32_t)idx << 16);
}
static void fp_cl_from_ilpos(Footprint&, CenterLine*, Edx, const ILinePos*) {}
PORT_FN(0x00422670, "CenterLine::convert_ilpos_to_cookie", CenterLine_convert_ilpos_to_cookie, fp_cl_from_ilpos)

// ASSERT_MSG (0x422700) is a bare ret in v1.0: not rewritten (it's kept, called above as the original does)

// CenterLine::convert_meters_to_ilpos (0x422710): the node whose distance is the last below m
static ILinePos* __fastcall CenterLine_convert_meters_to_ilpos(CenterLine* self, Edx, ILinePos* ret, float m) {
    if (!self->res) {
        const uint32_t zero = 0;
        cp4(&ret->t, &zero);
        ret->seg = 0;
        return ret;
    }
    ILSeg* h = self->head;
    ILSeg* s = h;
    if (h->next != h) {
        for (;;) {
            ILSeg* n = s->next;
            if (!(m > n->dist)) break;
            s = n;
            if (n->next == h) break;
        }
    }
    float t = (float)((D(m) - s->dist) / s->length);
    ret->seg = s;
    ret->t = t;
    return ret;
}
static void fp_cl_m_to_ilpos(Footprint& f, CenterLine*, Edx, ILinePos* ret, float) { f.add(ret, 8, "ILinePos"); }
PORT_FN(0x00422710, "CenterLine::convert_meters_to_ilpos", CenterLine_convert_meters_to_ilpos, fp_cl_m_to_ilpos)

// CenterLine::convert_ilpos_to_meters (0x422780)
static double __fastcall CenterLine_convert_ilpos_to_meters(CenterLine* self, Edx, const ILinePos* pos) {
    ILSeg* s = pos->seg;
    return D(pos->t) * s->length + s->dist;
}
PORT_FN(0x00422780, "CenterLine::convert_ilpos_to_meters", CenterLine_convert_ilpos_to_meters, fp_cl_from_ilpos)

// CenterLine::convert_cookie_to_meters (0x4227a0) / convert_meters_to_cookie (0x4227d0)
static double __fastcall CenterLine_convert_cookie_to_meters(CenterLine* self, Edx, uint32_t cookie) {
    ILinePos tmp;
    ILinePos* p = CenterLine_convert_cookie_to_ilpos_o(self, 0, &tmp, cookie);
    return CenterLine_convert_ilpos_to_meters_o(self, 0, p);
}
static void fp_cl_cookie_m(Footprint&, CenterLine*, Edx, uint32_t) {}
PORT_FN(0x004227a0, "CenterLine::convert_cookie_to_meters", CenterLine_convert_cookie_to_meters, fp_cl_cookie_m)

static uint32_t __fastcall CenterLine_convert_meters_to_cookie(CenterLine* self, Edx, float m) {
    ILinePos tmp;
    ILinePos* p = CenterLine_convert_meters_to_ilpos_o(self, 0, &tmp, Ub(m));
    return CenterLine_convert_ilpos_to_cookie_o(self, 0, p);
}
static void fp_cl_m_cookie(Footprint&, CenterLine*, Edx, float) {}
PORT_FN(0x004227d0, "CenterLine::convert_meters_to_cookie", CenterLine_convert_meters_to_cookie, fp_cl_m_cookie)

// CenterLine::get_ilpos_at_point (0x422800)
static ILinePos* __fastcall CenterLine_get_ilpos_at_point(CenterLine* self, Edx, ILinePos* ret, const Point2D* pt) {
    ILinePos tmp;
    tmp.seg = 0;
    const uint32_t zero = 0;
    cp4(&tmp.t, &zero);
    if (self->res) IdealLine_get_nearest_bead_o(self, 0, pt, &tmp, k_far);
    ret->seg = tmp.seg;
    cp4(&ret->t, &tmp.t);
    return ret;
}
static void fp_cl_at_point(Footprint& f, CenterLine*, Edx, ILinePos* ret, const Point2D*) { f.add(ret, 8, "ILinePos"); }
PORT_FN(0x00422800, "CenterLine::get_ilpos_at_point", CenterLine_get_ilpos_at_point, fp_cl_at_point)

// CenterLine::time_between (0x422840): how long ago `other` was where this car's bead is (its node times,
// interpolated), or the difference of their times at a shared node
static double __fastcall CenterLine_time_between(CenterLine* self, Edx, const CenterLine* other) {
    IdealLineRes* r = self->res;
    if (!r) return 0.0f;
    // FIX: either car's lost (NULL) bead was read (its index) and crashed: put it back at the point of that car's
    // line nearest it. (A line with no nodes at all is left as the original has it.)
    if (VP_FIX && !self->bead_seg && self->head) fix_find_bead(self, &self->car_pos, 0, 0);
    if (VP_FIX && !other->bead_seg && other->head) fix_find_bead((CenterLine*)other, &other->car_pos, 0, 0);
    int32_t a = self->bead_seg->index - 1;
    int32_t b = other->bead_seg->index - 1;
    if (a < 0) a = r->count - 1;
    if (b < 0) b = r->count - 1;
    if (b == a) return D(self->times[a]) - other->times[a];
    const float* ot = other->times;
    double now = PhysicsGetTime();
    double w = (1.0f - D(self->bead_t)) * ot[a];
    int32_t k = (a + 1) % r->count;
    double interp = w + D(ot[k]) * self->bead_t;
    return now - interp;
}
static void fp_cl_time_between(Footprint& f, CenterLine* self, Edx, const CenterLine* other) {
    f.add(self, sizeof(IdealLine), "IdealLine (a lost bead found)");
    f.add((void*)other, sizeof(IdealLine), "other IdealLine (a lost bead found)");
}
PORT_FN(0x00422840, "CenterLine::time_between", CenterLine_time_between, fp_cl_time_between)

// CenterLine::wrong_way (0x4228d0): moving faster than 2.2 m/s against the line's tangent at the bead
static uint8_t __fastcall CenterLine_wrong_way(CenterLine* self, Edx, const P3* vel) {
    ILSeg* s = self->bead_seg;
    if (!s || !self->head) return 0;
    double sq = (D(vel->y) * vel->y + D(vel->z) * vel->z) + D(vel->x) * vel->x;
    if (!(sq >= k_wrong_way_sq)) return 0;
    Point2D tan;
    ILSeg_QuickTan_o(s, 0, &tan, Ub(self->bead_t));
    double d = D(vel->z) * tan.z + D(vel->x) * tan.x;
    return !(d >= 0.0f) ? 1 : 0;
}
static void fp_cl_wrong_way(Footprint&, CenterLine*, Edx, const P3*) {}
PORT_FN(0x004228d0, "CenterLine::wrong_way", CenterLine_wrong_way, fp_cl_wrong_way)

// CenterLine::get_next_checkpoint (0x422950)
static int __fastcall CenterLine_get_next_checkpoint(CenterLine* self, Edx) {
    ILSeg* s = self->bead_seg;
    if (s && self->head && s->checkpoint) return s->checkpoint;
    return 0;
}
PORT_FN(0x00422950, "CenterLine::get_next_checkpoint", CenterLine_get_next_checkpoint, fp_cl_read)

// CenterLine::update_2d_data (0x422970): the bead's point, tangent and normal
static void __fastcall CenterLine_update_2d_data(CenterLine* self, Edx) {
    ILinePos p;
    IdealLine_get_actual_bead_position_o(self, 0, &p);
    ILSeg* s = p.seg;
    // FIX: a lost (NULL) bead was read (s->next) and crashed: put it back at the point of the line nearest the
    // car. (No line at all is left as the original has it.)
    if (VP_FIX && !s && self->head) {
        fix_find_bead(self, &self->car_pos, 0, 0);
        s = p.seg = self->bead_seg;
        cp4(&p.t, &self->bead_t);
    }
    H4 h = hermite(p.t);
    ILSeg* n = s->next;
    self->pos.x = (float)(((D(s->p.x) * h.h00 + D(s->v.x) * h.h10) + D(n->v.x) * h.h11) + D(n->p.x) * h.h01);
    self->pos.z = (float)(((D(n->v.z) * h.h11 + D(n->p.z) * h.h01) + D(s->p.z) * h.h00) + D(s->v.z) * h.h10);
    ILSeg_QuickTan_o(s, 0, &self->tan, Ub(p.t));
    uint32_t tz;
    cp4(&tz, &self->tan.z);
    fneg_store(&self->normal.z, &self->tan.x);
    cp4(&self->normal.x, &tz);
}
static void fp_cl_update_2d(Footprint& f, CenterLine* self, Edx) { f.add(self, sizeof(CenterLine), "CenterLine"); }
PORT_FN(0x00422970, "CenterLine::update_2d_data", CenterLine_update_2d_data, fp_cl_update_2d)

// CenterLine::calculate_lat (0x422a50): the car's offset along the normal (ST0, unrounded); 0 off the line
static double __fastcall CenterLine_calculate_lat(CenterLine* self, Edx) {
    if (!self->bead_on_line) return 0.0f;
    return (D(self->car_pos.x) - self->pos.x) * self->normal.x + (D(self->car_pos.z) - self->pos.z) * self->normal.z;
}
static void fp_cl_calculate_lat(Footprint& f, CenterLine*, Edx) { f.pure = true; }
PORT_FN(0x00422a50, "CenterLine::calculate_lat", CenterLine_calculate_lat, fp_cl_calculate_lat)

// ILSeg::__DrawLine (0x422a80): a box on the node, then ten dots along a blend toward the next node
// (the weights (t*t)*B + 1 and (t*t)*A walk from -+2.8 by 0.2 a dot: not the Hermite curve)
static void __fastcall ILSeg_DrawLine(ILSeg* self, Edx) {
    Point2D p0, p1, scr;
    cp4(&p0.x, &self->p.x);
    cp4(&p0.z, &self->p.z);
    ILSeg* n = self->next;
    cp4(&p1.x, &n->p.x);
    cp4(&p1.z, &n->p.z);
    uint8_t any = project_o(&p1, &scr);
    if (project_o(&p0, &scr)) {
        uint32_t col = Ub(self->_34) > 0x80000000u ? *g_col_neg : *g_col_line;
        int y3 = x87_ftol(D(scr.z) + 3.0f);
        int x3 = x87_ftol(D(scr.x) + 3.0f);
        int y = x87_ftol(scr.z);
        any = 1;
        int x = x87_ftol(scr.x);
        gxRect(x, y, x3, y3, col);
    }
    if (!any) return;
    float t = Fb(0x3dcccccdu);                            // 0.1
    float a = Fb(0x40333333u);                            // 2.8
    float b = Fb(0xc0333333u);                            // -2.8
    double tr;
    do {
        n = self->next;
        double c0 = D(t) * t * b + 1.0f;
        double c1 = D(t) * t * a;
        double c2 = ((D(t) - 2.0f) * t + 1.0f) * t;
        double c3 = (D(t) - 1.0f) * t * t;
        Point2D pt, o;
        pt.x = (float)(((D(n->v.x) * c3 + D(n->p.x) * c1) + D(self->v.x) * c2) + D(self->p.x) * c0);
        pt.z = (float)(((c3 * n->v.z + D(n->p.z) * c1) + D(self->v.z) * c2) + D(self->p.z) * c0);
        if (project_o(&pt, &o)) {
            uint32_t col = *g_col_line;
            int y2 = x87_ftol(D(o.z) + 2.0f);
            int x2 = x87_ftol(D(o.x) + 2.0f);
            int y = x87_ftol(o.z);
            int x = x87_ftol(o.x);
            gxRect(x, y, x2, y2, col);
        }
        a = (float)(D(a) - Fb(0x3e4ccccdu));              // 0.2
        b = (float)(D(b) + Fb(0x3e4ccccdu));
        tr = D(t) + Fb(0x3dcccccdu);                      // 0.1
        t = (float)tr;
    } while (!(tr >= Fb(0x3f8ccccdu)));                   // 1.1
}
static void fp_drawline(Footprint& f, ILSeg*, Edx) { f.replay_only = "draws (gxRect)"; }
PORT_FN(0x00422a80, "ILSeg::__DrawLine", ILSeg_DrawLine, fp_drawline)

// The five deleting destructors (0x422c80 IdealLine, 0x422ca0 Pool<ILSeg>, 0x422cc0 MutableIdealLine,
// 0x422ce0 ConstIdealLine, 0x422d00 CenterLine) and the $E initialisers are not rewritten.

// ===================================================================================================================
// track.obj
// ===================================================================================================================

// AIGetTrackInfo (0x424e40): the track's entry in the 16-entry table at 0x4ec6c0 (20 bytes each)
static const void* __cdecl AIGetTrackInfo(int track) {
    return (const void*)(uintptr_t)(0x004ec6c0u + (uint32_t)(track & 15) * 20u);
}
static void fp_track_info(Footprint& f, int) { f.pure = true; }
PORT_FN(0x00424e40, "AIGetTrackInfo", AIGetTrackInfo, fp_track_info)

// ===================================================================================================================
// idealres.obj
// ===================================================================================================================

// ILSeg::QuickEval (0x426120): the straight line between the nodes (not the curve)
static void __fastcall ILSeg_QuickEval(const ILSeg* self, Edx, Point2D* out, float t) {
    const ILSeg* n = self->next;
    out->x = (float)((D(n->p.x) - self->p.x) * t + self->p.x);
    n = self->next;
    out->z = (float)((D(n->p.z) - self->p.z) * t + self->p.z);
}
static void fp_seg_eval(Footprint& f, const ILSeg*, Edx, Point2D* out, float) { f.add(out, 8, "out"); }
PORT_FN(0x00426120, "ILSeg::QuickEval", ILSeg_QuickEval, fp_seg_eval)

// ILSeg::QuickTan (0x426150): the tangents blended linearly, normalised
static void __fastcall ILSeg_QuickTan(const ILSeg* self, Edx, Point2D* out, float t) {
    const ILSeg* n = self->next;
    out->x = (float)((D(n->v.x) - self->v.x) * t + self->v.x);
    n = self->next;
    out->z = (float)((D(n->v.z) - self->v.z) * t + self->v.z);
    double len = x87_sqrt(D(out->z) * out->z + D(out->x) * out->x);
    // FIX: a tangent with no length (both nodes' tangents zero -- a two-node line's, when a tangent is made from the
    // next node minus the previous, the same node -- or blending to zero) was 0/0: NaN, which the AI steers by. The
    // chord's direction instead (the zero-tangent curve's own); likewise for a NaN or infinite length.
    if (VP_FIX && !(len > 0.0 && len <= DBL_MAX)) {
        fix_chord_tan(self, out);
        return;
    }
    out->x = (float)(D(out->x) / len);
    out->z = (float)(D(out->z) / len);
}
PORT_FN(0x00426150, "ILSeg::QuickTan", ILSeg_QuickTan, fp_seg_eval)

// ILSeg::__Curvature (0x4261c0): the change of the unit tangent over the segment's length, the length summed
// over 1000 steps of a blend (weights (B*s)*s + 1 and (A*s)*s, A and B walking from +-2.998 by 0.002)
static double __fastcall ILSeg_Curvature(const ILSeg* self, Edx) {
    float ux, uz, wx, wz;
    cp4(&ux, &self->v.x);
    cp4(&uz, &self->v.z);
    double l1 = x87_sqrt(D(uz) * uz + D(ux) * ux);
    const ILSeg* n = self->next;
    cp4(&wx, &n->v.x);
    cp4(&wz, &n->v.z);
    ux = (float)(D(ux) / l1);
    uz = (float)(D(uz) / l1);
    double l2 = x87_sqrt(D(wz) * wz + D(wx) * wx);
    wx = (float)(D(wx) / l2);
    wz = (float)(D(wz) / l2);
    float dux = (float)(D(wx) - ux);
    float duz = (float)(D(wz) - uz);
    float prevx, prevz;
    cp4(&prevx, &self->p.x);
    cp4(&prevz, &self->p.z);
    float s = Fb(0x3a83126fu);                            // 0.001
    float a = Fb(0x403fdf3bu);                            // 2.998
    float b = Fb(0xc03fdf3bu);                            // -2.998
    const uint32_t zero = 0;
    float z0;
    cp4(&z0, &zero);
    double len = z0;                                      // fld [0]: summed in a register
    double sr;
    do {
        double c0 = D(b) * s * s + 1.0f;
        double c1 = D(a) * s * s;
        double c2 = ((D(s) - 2.0f) * s + 1.0f) * s;
        double c3 = (D(s) - 1.0f) * s * s;
        float x = (float)(((D(n->v.x) * c3 + D(self->v.x) * c2) + D(n->p.x) * c1) + D(self->p.x) * c0);
        float z = (float)(((c3 * n->v.z + c2 * self->v.z) + c1 * n->p.z) + c0 * self->p.z);
        float dx = (float)(D(x) - prevx);
        double dz = D(z) - prevz;
        float dzf = (float)dz;
        double step = x87_sqrt(dz * dzf + D(dx) * dx);
        cp4(&prevx, &x);
        cp4(&prevz, &z);
        len = len + step;
        a = (float)(D(a) - Fb(0x3b03126fu));              // 0.002
        b = (float)(D(b) + Fb(0x3b03126fu));
        sr = D(s) + Fb(0x3a83126fu);                      // 0.001
        s = (float)sr;
    } while (!(sr >= Fb(0x3f8020c4u)));                   // 1.001
    float lenf = (float)len;
    return x87_sqrt(D(duz) * duz + D(dux) * dux) / lenf;
}
static void fp_seg_read(Footprint&, const ILSeg*, Edx) {}
PORT_FN(0x004261c0, "ILSeg::__Curvature", ILSeg_Curvature, fp_seg_read)

// ILSeg::__Curv (0x426400): which way the segment turns (the tangents at 0.1 and 0.9 crossed): 1 if > 0
static int __fastcall ILSeg_Curv(const ILSeg* self, Edx) {
    Point2D a, b;
    ILSeg_QuickTan_o(self, 0, &a, 0x3dcccccdu);           // 0.1
    ILSeg_QuickTan_o(self, 0, &b, 0x3f666666u);           // 0.9
    double c = -D(b.x) * a.z + D(b.z) * a.x;
    return c > 0.0f ? 1 : 0;
}
static void fp_seg_curv(Footprint&, const ILSeg*, Edx) {}
PORT_FN(0x00426400, "ILSeg::__Curv", ILSeg_Curv, fp_seg_curv)
