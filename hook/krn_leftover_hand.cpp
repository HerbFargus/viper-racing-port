// krn_leftover_hand.cpp -- M3 UI stage, step U0: the leftover functions of the finished libraries that fit none of
// tools/gen_leftovers.py's shapes, rewritten by hand (the rest are generated into krn_leftover.cpp). Written from the
// v1.0 disassembly (tools/disasm.py), faithful (docs/PORTING.md): every call in the original's order with its
// arguments, game functions by their v1.0 address, the game's own strings and constants, stores through volatile.
//
//   dsounderr2str (ds.obj)             a DirectSound error's name: LIVE (wave.obj calls it on its error paths), the one
//                                      ds.obj function kept -- the rest of ds.obj / ds3d_x.obj is the dead hardware mixer
//   QuarterCarControl::Draw            the suspension rig's debug dashboard (carpart.obj): axes, the load plot, and
//                                      four lines of text (fixed: a large value no longer overruns the line's buffer)
//   TubeVolume::scalar deleting destructor   the inlined destructor stores the base vtable through eax, three times
//   CollisionVolume::GetExtents        the base class's stub: LogPanic("GetExtents not defined"). viperport.cpp's
//                                      broad phase compares vtable slots with its ADDRESS (0x436080); a hook there
//                                      leaves the address, and so the comparison, as it was
//   TrackDraw (track.obj)              GrafDraw(the track's graf)
//
// Test: test/world_leftover.cpp (every function here and in krn_leftover.cpp, original against rewrite).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "x87.h"

typedef int LhEdx;                                  // the unused edx of a __thiscall received as __fastcall
#define LH_G32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define LH_F32(a) (*(volatile float*)(uintptr_t)(a))
#define LH_FN(T, a) ((T)(uintptr_t)(a))
#define LH_S(a) ((const char*)(uintptr_t)(a))       // one of the game's own strings

static __forceinline float lh_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

typedef void(__cdecl* LhLog_t)(const char*, ...);
typedef void(__cdecl* LhPtr_t)(void*);
typedef void(__cdecl* LhInt_t)(int);
typedef void*(__cdecl* LhSetCanvas_t)(void*);
typedef void(__cdecl* LhAxes_t)(const void*, int, int, uint32_t);
typedef void(__cdecl* LhLinePlot_t)(const void*, uint32_t, void*, uint32_t);
typedef void(__fastcall* LhDamperSetup_t)(void*, LhEdx, float, float, float, float);
typedef float(__fastcall* LhDampingRate_t)(void*, LhEdx, float);
typedef int(__cdecl* LhSprintf_t)(char*, const char*, ...);
typedef void(__cdecl* LhText_t)(int, int, const char*, uint32_t);
#define lh_LogPanic LH_FN(LhLog_t, 0x004112b0)
#define lh_OpDelete LH_FN(LhPtr_t, 0x00414390)
#define lh_GrafDraw LH_FN(LhInt_t, 0x0046dc70)
#define lh_gxSetCanvas LH_FN(LhSetCanvas_t, 0x0044fe30)
#define lh_GraphDrawAxes LH_FN(LhAxes_t, 0x004d9420)
#define lh_GraphDrawLinePlot LH_FN(LhLinePlot_t, 0x004d9290)
#define lh_DamperSetup LH_FN(LhDamperSetup_t, 0x00446a90)
#define lh_GetDampingRate LH_FN(LhDampingRate_t, 0x00446ae0)
#define lh_sprintf LH_FN(LhSprintf_t, 0x004cf0a0)
#define lh_gxText LH_FN(LhText_t, 0x00453bd0)

// ---- dsounderr2str (0x4756b0): an HRESULT's DSERR_ name, "<unknown>" for anything else --------------------------------
// The original is a compiled switch (signed compares, then a byte-indexed jump table for 0x8878001e..0x887800aa); the
// mapping below is its table, read from the exe: every other value lands on "<unknown>" (0x4f5ea0).
static const char* __cdecl dsounderr2str_h(int32_t err) {
    switch ((uint32_t)err) {
    case 0x80004001u: return LH_S(0x004f5e0c);     // DSERR_UNSUPPORTED
    case 0x80004005u: return LH_S(0x004f5dc0);     // DSERR_GENERIC
    case 0x80040110u: return LH_S(0x004f5e4c);     // DSERR_NOAGGREGATION
    case 0x8007000eu: return LH_S(0x004f5de8);     // DSERR_OUTOFMEMORY
    case 0x80070057u: return LH_S(0x004f5d98);     // DSERR_INVALIDPARAM
    case 0x8878000au: return LH_S(0x004f5d70);     // DSERR_ALLOCATED
    case 0x8878001eu: return LH_S(0x004f5d80);     // DSERR_CONTROLUNAVAIL
    case 0x88780032u: return LH_S(0x004f5dac);     // DSERR_INVALIDCALL
    case 0x88780046u: return LH_S(0x004f5dd0);     // DSERR_PRIOLEVELNEEDED
    case 0x88780064u: return LH_S(0x004f5dfc);     // DSERR_BADFORMAT
    case 0x88780078u: return LH_S(0x004f5e20);     // DSERR_NODRIVER
    case 0x88780082u: return LH_S(0x004f5e30);     // DSERR_ALREADYINITIALIZED
    case 0x88780096u: return LH_S(0x004f5e60);     // DSERR_BUFFERLOST
    case 0x887800a0u: return LH_S(0x004f5e74);     // DSERR_OTHERAPPHASPRIO
    case 0x887800aau: return LH_S(0x004f5e8c);     // DSERR_UNINITIALIZED
    default: return LH_S(0x004f5ea0);              // <unknown>
    }
}
static void fp_dsounderr2str_h(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x004756b0, "dsounderr2str", dsounderr2str_h, fp_dsounderr2str_h)

// ---- QuarterCarControl::Draw (0x447620, virtual) ------------------------------------------------------------------------
// QuarterCarControl (0x408c bytes): +0x04/+0x08 the control's x / y, +0x0c/+0x10 its width / height, +0x18 the sprung
// mass, +0x1c the unsprung mass, +0x20 the spring rate (set here: +0x4060 * 175.19683, lb/in -> N/m), +0x24 the tyre
// rate, +0x28 the Damper* the ratios are read from, +0x4060 the spring setting, +0x4064 / +0x4068 the damper settings,
// +0x406c / +0x4070 the best grip and where (reset here, then found by graph_fn as the plot runs), +0x4074 the best
// contact, +0x4078 the rig's own Damper (4 floats, set from the settings).
namespace {
struct LhGraphInfo {                                // what GraphDrawAxes / GraphDrawLinePlot read: 0x24 bytes, all set
    int32_t x0, y0, x1, y1;
    int32_t ticks;                                  // 0x28
    uint32_t xmin, xmax, ymin, ymax;                // floats, as bits: 0, 200, 0, 1
};
}   // namespace
static_assert(sizeof(LhGraphInfo) == 0x24, "LhGraphInfo");
static void __fastcall QuarterCarControl_Draw_h(uint8_t* self, LhEdx, void* canvas) {
    const float k_rate = lh_bits(0x432f3264);       // 175.19684f (0x4dc7c0, 0x4dc7c4)
    const float k_half = lh_bits(0x3f000000);       // 0.5f (0x4dc6e8)
    // FIX: the original formats each line into 80 bytes with the game's unbounded sprintf, and a large value overruns
    // them onto the stack: a ratio's %1.2f of a double runs to 313 characters (a zero mass or rate makes it infinite or
    // huge), Best Grip's three to about 150. The buffer is made big enough for the longest any of the four formats can
    // print (under 0x160), so every line is formatted and drawn whole, as the original drew it when it survived; a line
    // that fits in 80 bytes is the same either way. gxText's points are clipped, so a long line runs off the canvas.
    char text[VP_FIX ? 0x400 : 0x50];
    LH_F32(self + 0x20) = *(float*)(self + 0x4060) * k_rate;
    lh_gxSetCanvas(canvas);
    const int32_t x = *(int32_t*)(self + 4), y = *(int32_t*)(self + 8);
    LhGraphInfo g;
    g.x0 = x + 0x28;
    g.y0 = y + 0x14;
    g.x1 = *(int32_t*)(self + 0xc) + x - 0xf;
    g.ticks = 0x28;
    g.xmin = 0;
    g.ymin = 0;
    g.xmax = 0x43480000u;                           // 200.0f
    g.ymax = 0x3f800000u;                           // 1.0f
    g.y1 = *(int32_t*)(self + 0x10) + y - 0x14;
    lh_GraphDrawAxes(&g, 5, 5, LH_G32(0x00522438));
    LH_G32(self + 0x4070) = 0;
    LH_G32(self + 0x406c) = 0x461c4000u;            // 10000.0f
    lh_GraphDrawLinePlot(&g, 0x004478b0u, self, LH_G32(0x00522458));   // QuarterCarControl::graph_fn, the original's
    const float a = *(float*)(self + 0x4068) * k_rate, b = *(float*)(self + 0x4064) * k_rate;
    lh_DamperSetup(self + 0x4078, 0, b, b, a, a);
    // "Body damping ratio": the mean of the rates at +1 and -1 m/s over 2 sqrt(k m)
    void* damper = *(void**)(self + 0x28);
    volatile float r1 = lh_GetDampingRate(damper, 0, lh_bits(0x3f800000));
    double mean = ((double)lh_GetDampingRate(damper, 0, lh_bits(0xbf800000)) + (double)r1) * (double)k_half;
    double km = (double)*(float*)(self + 0x18) * (double)*(float*)(self + 0x20);
    lh_sprintf(text, LH_S(0x004edef4), mean / x87_sqrt(km) * (double)k_half);
    lh_gxText(0x3c, 0x28, text, LH_G32(0x00522434));
    // "Wheel damping ratio": the same over 2 sqrt((spring + tyre) unsprung mass)
    damper = *(void**)(self + 0x28);
    r1 = lh_GetDampingRate(damper, 0, lh_bits(0x3f800000));
    mean = ((double)lh_GetDampingRate(damper, 0, lh_bits(0xbf800000)) + (double)r1) * (double)k_half;
    km = ((double)*(float*)(self + 0x24) + (double)*(float*)(self + 0x20)) * (double)*(float*)(self + 0x1c);
    lh_sprintf(text, LH_S(0x004eded8), mean / x87_sqrt(km) * (double)k_half);
    lh_gxText(0x3c, 0x3c, text, LH_G32(0x00522434));
    // "Best Grip: %1.3f = %1.1f%% (at %1.0f)"
    const double at = (double)*(float*)(self + 0x4070);
    const double pct = ((double)lh_bits(0x3f800000) - (double)*(float*)(self + 0x406c)) * (double)lh_bits(0x42c80000);
    lh_sprintf(text, LH_S(0x004edeb0), (double)*(float*)(self + 0x406c), pct, at);
    lh_gxText(0x3c, 0x50, text, LH_G32(0x00522434));
    lh_sprintf(text, LH_S(0x004ede9c), (double)*(float*)(self + 0x4074));   // "Best Contact: %1.2f"
    lh_gxText(0x3c, 0x64, text, LH_G32(0x00522434));
}
static void fp_qcc_draw_h(Footprint& f, uint8_t*, LhEdx, void*) { f.replay_only = "draws the debug dashboard (graphs and text)"; }
PORT_FN(0x00447620, "QuarterCarControl::Draw", QuarterCarControl_Draw_h, fp_qcc_draw_h)

// ---- TubeVolume::scalar deleting destructor (0x436250) ---------------------------------------------------------------------
// the inlined ~TubeVolume: the CollisionVolume vtable (0x4dbcf8) into the base and both embedded end caps (+0x64, +0x24,
// +0, in that order), then operator delete if bit 0 of the flags is set
static void* __fastcall TubeVolume_sdtor_h(uint8_t* self, LhEdx, uint32_t flags) {
    LH_G32(self + 0x64) = 0x004dbcf8u;
    LH_G32(self + 0x24) = 0x004dbcf8u;
    LH_G32(self) = 0x004dbcf8u;
    if (flags & 1) lh_OpDelete(self);
    return self;
}
static void fp_tube_sdtor_h(Footprint& f, uint8_t*, LhEdx, uint32_t) { f.replay_only = "frees the volume"; }
PORT_FN(0x00436250, "TubeVolume::scalar deleting destructor", TubeVolume_sdtor_h, fp_tube_sdtor_h)

// ---- CollisionVolume::GetExtents (0x436080, virtual): a volume class that doesn't define it ------------------------------
static void __fastcall CollisionVolume_GetExtents_h(void*, LhEdx, void*, void*) { lh_LogPanic(LH_S(0x004ed77c)); }
static void fp_cv_getextents_h(Footprint& f, void*, LhEdx, void*, void*) { f.replay_only = "LogPanic: \"GetExtents not defined\""; }
PORT_FN(0x00436080, "CollisionVolume::GetExtents", CollisionVolume_GetExtents_h, fp_cv_getextents_h)

// ---- TrackDraw (0x4695f0) -------------------------------------------------------------------------------------------------
static void __cdecl TrackDraw_h() { lh_GrafDraw((int)LH_G32(0x00557fc8)); }
static void fp_track_draw_h(Footprint& f) { f.replay_only = "draws the track (GrafDraw)"; }
PORT_FN(0x004695f0, "TrackDraw", TrackDraw_h, fp_track_draw_h)
