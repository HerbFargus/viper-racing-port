// root_dash.cpp -- M3 UI stage, step U3 (group A): the in-race dashboard, dash.obj, rewritten faithfully (library `root`).
//
//   DashboardBegin / End (the fonts, stamps, palettes and the two canvases: the dash and its background, status.stp),
//   DashboardReset, DashboardUpdate (the dash canvas: place, lap, race / lap / test-track time, the gaps to the cars
//   ahead and behind, best and last lap, speed -- each redrawn only when it changes, its rectangle's bit set in the dirty
//   mask), clear_rect / clear_always_dirty_rects (a rectangle of the background pasted back), DashboardAlwaysUpdate (the
//   banners' flags for the next frame: summary, damaged, off the track, award, countdown; the camera switched once the
//   player is done), DashboardMustDraw and the show_* predicates, TheRealDashboardDraw (DashboardDraw / DashboardDrawDashNone:
//   the dash's dirty rectangles copied to the screen, the gear and the rev dial, the pause / countdown / reset / damage /
//   award / summary / loading banners), copy_rects, DashboardDraw3D (the banners' translucent backing rectangles),
//   draw_analog_dial (the rev needle). draw_fps, a bare `ret`, is generated (hook/root_leftover.cpp).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this object's own too, so a
// hooked rewrite is what runs), virtual calls through the Deity's vtable. The car's state (its CarMgrInfo, the CarInfo and
// message the physics thread writes) is read where the original reads it, each time it reads it, at its width: a float
// compared or copied by its bits (`cmp dword [x], imm`, `mov eax, [x]`) as its bits, one loaded as a float. x87: the
// products and sums the original formats are the x87's at the running precision (double operands of float values), the
// (30 > gap ? gap : 30) clamp keeps its NaN result (30), fsin / fcos of the needle's angle are finished in their own asm
// (x87.h's x87_sin_mul / x87_cos_mul: the full-precision result multiplied, rounded), __ftol as x87_ftol.
//
// Footprints (main thread): the draws write the canvas they draw on (its clip and pixels), the current-canvas static, the
// dash's statics they keep (the dirty mask, what was last drawn), the Xlators they refresh and PhysicsTimeString's buffer;
// while a function-local Xlator is still to be built (its destructor goes to atexit), or a banner would load its stamp
// (the pause and loading banners: generic_splash) or begin the 3D alpha rectangles (mrBeginAlphaRects can load a
// texture), the check is left to the session replays (replay_only). DashboardBegin / End load and free (replay_only).
//
// FIX CANDIDATEs (left faithful):
//   - DashboardUpdate formats the best / last lap ("%1.1f @ %1.0f %s") and the test track's distance into a 64-byte frame
//     buffer: a time or speed over about 1e25 (never in play) runs past it into the speed's buffer and the saved registers.
//   - TheRealDashboardDraw indexes the gear names with the car's gear unchecked (-1..7 in play).
//   - TheRealDashboardDraw passes translations (the reset / damaged banners, the summary's two headings) to gxFontPrintf
//     as the format: a '%' in a translation reads garbage arguments.
//   - clear_rect with no bit set loops forever (LogPanic each time); a bit past 8 indexes past the nine rectangles. Only
//     constants are passed.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "x87.h"
#include "ui_types.h"
#include "root_types.h"

namespace {
namespace rdash {
using namespace uit;
using namespace rhud;

#define D(x) ((double)(x))
#define FONT(a) RH_GP(a)
#define PAL(a) RH_GP(a)
typedef float(__cdecl* FloatInt_t)(int32_t);
typedef float(__cdecl* Float0_t)();
typedef void(__cdecl* Dial_t)(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t);   // the floats by their bits
static __forceinline void clear_rect(int32_t flag) { ccall<void>(F_clear_rect, flag); }
static __forceinline uint8_t deity_finished(int32_t car) { return vcall<uint8_t>(RH_GP(S_DEITY), VO_Deity_CarIsFinished, car); }
static __forceinline CarMgrInfo* car_info(int32_t car) { return ccall<CarMgrInfo*>(F_CarMgrGetInfo, car); }
static __forceinline int32_t game_type() { return *(volatile int32_t*)(ccall<uint8_t*>(F_WorldGameOptions) + 0x18); }
static __forceinline const uint8_t* locale() { return UI_GP(const uint8_t, S_LOCALE); }
static __forceinline float lf(const uint8_t* loc, uint32_t off) { return *(const volatile float*)(loc + off); }
static __forceinline const char* lp(const uint8_t* loc, uint32_t off) { return *(const char* const volatile*)(loc + off); }
static __forceinline uint32_t fbits(volatile float& f) { return *(volatile uint32_t*)&f; }

// the canvas and the statics a dash draw writes
static void fp_dash_canvas(Footprint& f) {
    UI_FP_CUR_CANVAS(f);
    UI_FP_CANVAS(f, S_CANVAS);
}

// =========================================================================================================================
// DashboardBegin / DashboardEnd / DashboardReset
// =========================================================================================================================
static void __cdecl DashboardBegin_n() {
    RH_GU32(S_SHOW) = 0;
    RH_GU32(S_OFF_TRACK_SINCE) = 0;
    RH_G16(S_SHOW + 4) = 0;
    RH_GU32(S_CAMERA_STEP) = 0;
    RH_GU32(S_START_SEEN) = 0;
    RH_G32(S_BEGIN_TIME) = ccall<int32_t>(F_PTimeNow);
    RH_GU32(0x00504218) = 0;
    ccall<uint8_t>(F_TelemetrySetAddMeter, 0, RH_CP(0x004e3d80), RH_VP(0x005040f8));   // elapsed_time
    ccall<uint8_t>(F_TelemetrySetAddMeter, 0, RH_CP(0x004e3d90), RH_VP(0x00504278));   // best_lap_time
    ccall<uint8_t>(F_TelemetrySetAddMeter, 0, RH_CP(0x004e3da0), RH_VP(0x00504200));   // best_lap_car
    RH_GP(S_ST_AWARD) = ccall<void*>(F_gxGetStamp, RH_CP(0x004e3db0));
    RH_GP(S_ST_MPH) = ccall<void*>(F_gxGetStamp, RH_CP(0x004e3dbc));
    RH_GP(S_ST_RPM) = ccall<void*>(F_gxGetStamp, RH_CP(0x004e3dc4));
    RH_GP(S_ST_STATUS) = ccall<void*>(F_gxGetStamp, RH_CP(0x004e3dcc));
    RH_GP(S_ST_ARROW) = ccall<void*>(F_gxGetStamp, RH_CP(0x004e3dd8));
    RH_GP(S_ST_START) = ccall<void*>(F_gxGetStamp, RH_CP(0x004e3de4));
    RH_GP(S_F_EURO6) = ccall<void*>(F_gxFontGet, RH_CP(0x004e3df0));
    RH_GP(S_F_EURO9) = ccall<void*>(F_gxFontGet, RH_CP(0x004e3dfc));
    RH_GP(S_F_EURO19) = ccall<void*>(F_gxFontGet, RH_CP(0x004e3e08));
    RH_GP(S_F_LCD) = ccall<void*>(F_gxFontGet, RH_CP(0x004e3e18));
    void* f7 = ccall<void*>(F_gxFontGet, RH_CP(0x004e3e24));
    void* st = RH_GP(S_ST_STATUS);
    RH_GP(S_F_EURO7) = f7;
    {
        const int32_t h = ccall<int32_t>(F_gxStampHeight, st);
        const int32_t w = ccall<int32_t>(F_gxStampWidth, st);
        ccall<void>(F_gxAllocCanvas, RH_VP(S_CANVAS), w, h);
    }
    st = RH_GP(S_ST_STATUS);
    {
        const int32_t h = ccall<int32_t>(F_gxStampHeight, st);
        const int32_t w = ccall<int32_t>(F_gxStampWidth, st);
        ccall<void>(F_gxAllocCanvas, RH_VP(S_BACK), w, h);
    }
    ccall<void*>(F_gxSetCanvas, RH_VP(S_BACK));
    ccall<void>(F_gxClear, RH_GU32(S_CLEAR));
    ccall<void>(F_gxDrawStamp, RH_GP(S_ST_STATUS), 0, 0, 0, (void*)0);
    void* p = ccall<void*>(F_gxPaletteCreate);
    RH_GP(S_P_WHITE) = p;
    ccall<void>(F_gxPaletteMakeGradient, p, 0xffe67fe6u, 0x18e30c63u);
    p = ccall<void*>(F_gxPaletteCreate);
    RH_GP(S_P_RED) = p;
    ccall<void>(F_gxPaletteMakeGradient, p, 0x001f001fu, 0x18e30c63u);
    p = ccall<void*>(F_gxPaletteCreate);
    RH_GP(S_P_GREY) = p;
    ccall<void>(F_gxPaletteMakeGradient, p, 0xffff7fffu, 0x18e30c63u);
    p = ccall<void*>(F_gxPaletteCreate);
    RH_GP(S_P_GREEN) = p;
    ccall<void>(F_gxPaletteMakeGradient, p, 0xffe07fe0u, 0x21041084u);
    RH_GP(S_ST_SUMMARY) = ccall<void*>(F_gxGetStamp, RH_CP(0x004e3e30));
    p = ccall<void*>(F_gxPaletteCreate);
    RH_GP(S_P_SUM) = p;
    ccall<void>(F_gxPaletteMakeGradient, p, 0xffff7fffu, 0x4a492529u);
    p = ccall<void*>(F_gxPaletteCreate);
    RH_GP(S_P_SUM_ME) = p;
    ccall<void>(F_gxPaletteMakeGradient, p, 0xffe07fe0u, 0x4a492529u);
    RH_GU32(0x004e3bb0) = 0xffffffffu;
    RH_GU32(0x004e3bb4) = 0xffffffffu;
    RH_GU32(0x004e3bb8) = 0xffffffffu;
    RH_GU32(0x004e3bbc) = 0xffffffffu;
    RH_GU32(0x004e3bc0) = 0xbf800000u;           // -1.0f
    RH_GU32(0x004e3bc4) = 0xbf800000u;
    RH_GU32(0x004e3bc8) = 0xbf800000u;
    RH_GU32(0x004e3bcc) = 0xbf800000u;
    RH_GU32(0x004e3bd0) = 0xffffffffu;
    RH_G8(S_REDRAW) = 1;
    RH_GU32(0x004e3bd4) = 0xffffffffu;
}
static void fp_DashboardBegin(Footprint& f) { f.replay_only = "loads the dash's stamps and fonts, allocates its canvases and palettes"; }
PORT_FN(0x00403460, "DashboardBegin", DashboardBegin_n, fp_DashboardBegin)

static void __cdecl DashboardEnd_n() {
    ccall<void>(F_gxFreeCanvas, RH_VP(S_CANVAS));
    ccall<void>(F_gxFreeCanvas, RH_VP(S_BACK));
    ccall<void>(F_gxForgetStamp, RH_GP(S_ST_STATUS));
    ccall<void>(F_gxForgetStamp, RH_GP(S_ST_RPM));
    ccall<void>(F_gxForgetStamp, RH_GP(S_ST_MPH));
    ccall<void>(F_gxForgetStamp, RH_GP(S_ST_ARROW));
    ccall<void>(F_gxForgetStamp, RH_GP(S_ST_AWARD));
    ccall<void>(F_gxForgetStamp, RH_GP(S_ST_START));
    ccall<void>(F_gxFontForget, RH_GP(S_F_EURO6));
    ccall<void>(F_gxFontForget, RH_GP(S_F_EURO9));
    ccall<void>(F_gxFontForget, RH_GP(S_F_EURO19));
    ccall<void>(F_gxFontForget, RH_GP(S_F_LCD));
    ccall<void>(F_gxFontForget, RH_GP(S_F_EURO7));
    ccall<void>(F_gxPaletteDestroy, RH_GP(S_P_WHITE));
    ccall<void>(F_gxPaletteDestroy, RH_GP(S_P_RED));
    ccall<void>(F_gxPaletteDestroy, RH_GP(S_P_GREY));
    ccall<void>(F_gxPaletteDestroy, RH_GP(S_P_GREEN));
    ccall<void>(F_gxPaletteDestroy, RH_GP(S_P_SUM));
    ccall<void>(F_gxPaletteDestroy, RH_GP(S_P_SUM_ME));
    ccall<void>(F_gxForgetStamp, RH_GP(S_ST_SUMMARY));
}
static void fp_DashboardEnd(Footprint& f) { f.replay_only = "frees the dash's canvases, stamps, fonts and palettes"; }
PORT_FN(0x00403720, "DashboardEnd", DashboardEnd_n, fp_DashboardEnd)

static void __cdecl DashboardReset_n() { RH_G8(S_REDRAW) = 1; }
static void fp_DashboardReset(Footprint& f) { UI_FP(f, S_REDRAW, 1, "dash.obj redraw flag"); }
PORT_FN(0x00403840, "DashboardReset", DashboardReset_n, fp_DashboardReset)

// =========================================================================================================================
// DashboardUpdate
// =========================================================================================================================
// the best / last lap's line: time (+0x10 / +0x14) against what was last drawn; the formats by the game type (3: the
// time "@" the speed, 5: a distance, else the time)
struct LapLine {
    uint32_t cur_off, last, guard, xl, key, dtor, flag, x, fmt_time, fmt_speed, fmt_speed_s, fmt_dist, fmt_dist_s;
};
static const LapLine k_best = {0x10, 0x004e3bc0, 0x00504304, 0x005041c0, 0x004e3f5c, 0x004047a0, 0x10, 0xde,
                               0x004e3f98, 0x004e3f80, 0x004e3f94, 0x004e3f6c, 0x004e3f74};
static const LapLine k_last = {0x14, 0x004e3bc4, 0x0050413c, 0x005041b0, 0x004e3fa0, 0x00404790, 0x100, 0x156,
                               0x004e3fdc, 0x004e3fc4, 0x004e3fd8, 0x004e3fb0, 0x004e3fb8};

static __forceinline void lap_line(const LapLine& L, uint8_t bit, CarInfo* ri, uint8_t redraw) {
    volatile float* cur_f = (volatile float*)((uint8_t*)ri + L.cur_off);
    volatile uint32_t* cur_b = (volatile uint32_t*)((uint8_t*)ri + L.cur_off);
    {
        const float cur = *cur_f;                               // fld; fcomp; test ah, 0x40
        const float prev = RH_GF(L.last);
        if (!(D(cur) < D(prev) || D(cur) > D(prev)) && !redraw) return;
    }
    rh_xl_once(L.guard, bit, L.xl, L.key, L.dtor);
    RH_GU32(L.last) = *cur_b;
    if (!redraw) clear_rect((int32_t)L.flag);
    if ((int32_t)*cur_b > 0x3dcccccd) {
        const int32_t type = game_type();
        char buf[64];
        if (type == 3) {
            const uint8_t* loc = locale();
            const float k = lf(loc, 0x1c);
            const float s = ri->best_speed;
            const double speed = D(k) * D(s);
            const char* unit = lp(loc, 0x18);
            const float t = *cur_f;
            RH_sprintf(buf, RH_CP(L.fmt_speed), D(t), speed, unit);
            ccall<void>(F_LocaleConvertNumeric, buf);
            RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, (int32_t)L.x, 0x32, RH_CP(L.fmt_speed_s), buf);
        } else if (type == 5) {
            const uint8_t* loc = locale();
            const float k = lf(loc, 0xc);
            const float t = *cur_f;
            RH_sprintf(buf, RH_CP(L.fmt_dist), D(k) * D(t));
            ccall<void>(F_LocaleConvertNumeric, buf);
            xlate(L.xl);
            const uint8_t* loc2 = locale();
            const char* unit = lp(loc2, 8);
            const char* text = UI_GP(const char, L.xl + 4);
            RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, (int32_t)L.x, 0x32, RH_CP(L.fmt_dist_s), text, buf, unit);
        } else {
            xlate(L.xl);
            const char* s = RH_PhysicsTimeString(*cur_b, 1);
            const char* text = UI_GP(const char, L.xl + 4);
            RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, (int32_t)L.x, 0x32, RH_CP(L.fmt_time), text, s);
        }
    }
    RH_G32(S_DIRTY) |= (int32_t)L.flag;
}

// the gap to the car ahead (-, +0x24 / +0x28, row 0x19) or behind (+, +0x1c / +0x20, row 0x25)
static __forceinline void gap_line(volatile uint32_t* gap_b, volatile float* gap_f, volatile int32_t* car, uint32_t fmt,
                                   int32_t y, uint32_t fmt_s1, uint32_t fmt_s2) {
    void* pal = (int32_t)*gap_b > 0x3f800000 ? PAL(S_P_WHITE) : PAL(S_P_GREY);
    if ((int32_t)*gap_b > 0x41f00000) pal = PAL(S_P_RED);
    const float g = *gap_f;
    const double v = 30.0 > D(g) ? D(g) : 30.0;               // fld g; fld 30; fcom st(1); test ah, 0x41 (NaN: 30)
    char buf[64];
    RH_sprintf(buf, RH_CP(fmt), v);
    ccall<void>(F_LocaleConvertNumeric, buf);
    RH_gxFontPrintf(FONT(S_F_EURO6), pal, 0xa, 0x53, y, RH_CP(fmt_s1), buf);
    CarMgrInfo* o = car_info(*car);
    RH_gxFontPrintf(FONT(S_F_EURO6), pal, 9, 0x57, y, RH_CP(fmt_s2), (const char*)o + 5);
}

static void __cdecl DashboardUpdate_n() {
    const uint8_t lost = ccall<uint8_t>(F_VidWasLost);
    const int32_t focus = ccall<int32_t>(F_WorldGetFocusCar);
    uint8_t redraw = 0;
    CarMgrInfo* info = car_info(focus);
    CarInfo* ri = &info->info;
    CarMsg* msg = info->msg;
    if (lost || RH_G8(S_REDRAW) || RH_G32(0x004e3bb0) != focus) {
        redraw = 1;
        RH_G32(0x004e3bb0) = focus;
        RH_G32(S_DIRTY) = 0xffff;
        RH_G8(S_REDRAW) = 0;
    }
    ccall<void*>(F_gxSetCanvas, RH_VP(S_CANVAS));
    if (redraw) ccall<void>(F_gxPaste, RH_VP(S_BACK), 0, 0);
    else ccall<void>(F_clear_always_dirty_rects);
    int32_t count = ccall<int32_t>(F_CarMgrCount);
    if (ccall<uint8_t>(F_GhostCarIncognito)) count--;
    int32_t place = ri->place;
    if (!(place > 1)) place = 1;
    rh_xl_once(S_DASH_ONCE, 1, 0x005042f8, 0x004e3e3c, 0x00404810);   // Dashboard:Position
    if (redraw) {
        const char* t = xlate(0x005042f8);
        RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, 0x15c, 0x12, RH_CP(0x004e3e50), t);
    }
    if (redraw || RH_G32(0x004e3bb4) != place) {
        RH_G32(0x004e3bb4) = place;
        if (!redraw) clear_rect(8);
        RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, 0x19a, 0x12, RH_CP(0x004e3e54), place, count);
        RH_G32(S_DIRTY) |= 8;
    }
    uint8_t fresh = 0;
    rh_xl_once(S_DASH_ONCE, 2, 0x00504230, 0x004e3e5c, 0x00404800);   // Dashboard:RaceTime
    if (game_type() == 0 && deity_finished(focus)) {
        // the race is over for this car: its race time
        if (RH_G32(0x004e3bd4) != 0) {
            RH_G32(0x004e3bd4) = 0;
            fresh = 1;
            RH_G32(S_DIRTY) |= 0x80;
            if (!redraw) clear_rect(0x80);
        }
        volatile float t = ccall<float>(F_RecordGetRaceTime, focus);
        if (redraw || fresh) {
            const char* x = xlate(0x00504230);
            RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_RED), 0x21, 0xe6, 0x23, RH_CP(0x004e3e70), x);
        }
        const char* s = RH_PhysicsTimeString(fbits(t), 1);
        RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_RED), 0x21, 0x156, 0x23, RH_CP(0x004e3e74), s);
    } else if (game_type() == 0) {
        // a race: lap, lap time, the gaps
        if (RH_G32(0x004e3bd4) != 1) {
            RH_G32(0x004e3bd4) = 1;
            fresh = 1;
            RH_G32(S_DIRTY) |= 0x80;
            if (!redraw) clear_rect(0x80);
        }
        rh_xl_once(S_DASH_ONCE, 4, 0x005041d8, 0x004e3e78, 0x004047f0);   // Dashboard:Lap
        int32_t lap = ri->lap;
        if (!(lap > 1)) lap = 1;
        if (redraw || fresh) {
            const char* t = xlate(0x005041d8);
            RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, 0x10a, 0x12, RH_CP(0x004e3e88), t);
        }
        if (redraw || lap != RH_G32(0x004e3bb8)) {
            RH_G32(0x004e3bb8) = lap;
            if (!redraw) clear_rect(4);
            const int32_t laps = vcall<int32_t>(RH_GP(S_DEITY), VO_Deity_GetNumLaps);
            RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, 0x12a, 0x12, RH_CP(0x004e3e8c), lap, laps);
            RH_G32(S_DIRTY) |= 4;
        }
        rh_xl_once(S_DASH_ONCE, 8, 0x00504130, 0x004e3e94, 0x004047e0);   // Dashboard:LapTime
        if ((int32_t)ri->last_b > 0x3f800000 && (int32_t)ri->lap_time_b < 0x40a00000) {
            // the first five seconds of a lap: the last lap's time, flashing
            const float lt = ri->lap_time;
            void* pal = (x87_ftol(D(lt) * 8.0) & 1) ? PAL(S_P_RED) : PAL(S_P_GREY);
            RH_G32(0x004e3bd4) = 3;
            RH_G32(S_DIRTY) |= 0x80;
            if (!redraw) clear_rect(0x80);
            const char* t = xlate(0x00504130);
            RH_gxFontPrintf(FONT(S_F_EURO9), pal, 0x22, 0x150, 0x23, RH_CP(0x004e3ea8), t);
            const char* s = RH_PhysicsTimeString(ri->last_b, 1);
            RH_gxFontPrintf(FONT(S_F_EURO9), pal, 0x21, 0x156, 0x23, RH_CP(0x004e3eac), s);
        } else {
            if (redraw || fresh) {
                const char* t = xlate(0x00504130);
                RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_GREY), 0x22, 0x150, 0x23, RH_CP(0x004e3eb0), t);
            }
            const char* s = RH_PhysicsTimeString(ri->lap_time_b, 0);
            RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_GREY), 0x21, 0x156, 0x23, RH_CP(0x004e3eb4), s);
        }
        if (count > 1) {
            const uint8_t behind = place != count && (int32_t)ri->gap_behind_b > 0x3d4ccccd;
            const uint8_t ahead = place != 1 && (int32_t)ri->gap_ahead_b > 0x3d4ccccd;
            if (ahead) gap_line(&ri->gap_ahead_b, &ri->gap_ahead, &ri->ahead, 0x004e3eb8, 0x19, 0x004e3ec0, 0x004e3ec4);
            if (behind) gap_line(&ri->gap_behind_b, &ri->gap_behind, &ri->behind, 0x004e3ec8, 0x25, 0x004e3ed0, 0x004e3ed4);
        }
    } else {
        // the test track: the try, the time or the distance
        if (RH_G32(0x004e3bd4) != 2) {
            RH_G32(0x004e3bd4) = 2;
            fresh = 1;
            RH_G32(S_DIRTY) |= 0x80;
            if (!redraw) clear_rect(0x80);
        }
        ccall<void*>(F_WorldGameOptions);
        rh_xl_once(S_DASH_ONCE, 0x10, 0x00504188, 0x004e3ed8, 0x004047d0);   // Dashboard:TestTrack:Try
        rh_xl_once(S_DASH_ONCE, 0x20, 0x005040e8, 0x004e3ef0, 0x004047c0);   // Dashboard:TestTrack:Time
        rh_xl_once(S_DASH_ONCE, 0x40, 0x00504100, 0x004e3f0c, 0x004047b0);   // Dashboard:TestTrack:DistanceAbbreviated
        if (redraw || fresh) {
            const char* t = xlate(0x00504188);
            RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, 0x10a, 0x12, RH_CP(0x004e3f34), t);
        }
        if (redraw || ri->lap != RH_G32(0x004e3bb8)) {
            RH_G32(0x004e3bb8) = ri->lap;
            if (!redraw) clear_rect(4);
            RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, 0x132, 0x12, RH_CP(0x004e3f38), (int32_t)ri->lap);
            RH_G32(S_DIRTY) |= 4;
        }
        if (game_type() != 5) {
            if (redraw || fresh) {
                const char* t = xlate(0x005040e8);
                RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_GREY), 0x21, 0xf2, 0x23, RH_CP(0x004e3f3c), t);
            }
            const bool zero60 = game_type() == 3;
            const char* s = RH_PhysicsTimeString(ri->lap_time_b, 0);
            RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_GREY), 0x21, 0x156, 0x23, RH_CP(zero60 ? 0x004e3f44 : 0x004e3f40), s);
        } else {
            if (redraw || fresh) {
                const char* t = xlate(0x00504100);
                RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_GREY), 0x21, 0xf2, 0x23, RH_CP(0x004e3f48), t);
            }
            char buf[64];
            const uint8_t* loc = locale();
            const float k = lf(loc, 0xc);
            const float d = ri->lap_time;
            RH_sprintf(buf, RH_CP(0x004e3f4c), D(k) * D(d));
            ccall<void>(F_LocaleConvertNumeric, buf);
            const uint8_t* loc2 = locale();
            const char* unit = lp(loc2, 8);
            RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_GREY), 0x21, 0x156, 0x23, RH_CP(0x004e3f54), buf, unit);
        }
    }
    lap_line(k_best, 0x80, ri, redraw);
    lap_line(k_last, 1, ri, redraw);
    // the speed
    const uint8_t* loc = locale();
    volatile float spd;
    {
        const float v = msg->speed;
        const float k = lf(loc, 0x1c);
        spd = (float)(D(v) * D(k));
    }
    if (redraw) RH_gxFontPrintf(FONT(S_F_EURO6), PAL(S_P_WHITE), 0x21, 0x230, 0x2d, RH_CP(0x004e3fe4), lp(loc, 0x18));
    char num[12];
    RH_sprintf(num, RH_CP(0x004e3fe8), x87_ftol(D(spd)));
    uint32_t n = crt_strlen(num);
    if ((int32_t)n > 3) {
        n = 3;
        crt_strcpy(num, RH_CP(0x004e3fec));                     // "999"
    }
    const int32_t w0 = ccall<int32_t>(F_gxFontStringWidth, FONT(S_F_EURO9), RH_CP(0x004e3ff0));
    const int32_t x = (int32_t)(0x22au - (uint32_t)w0 * n);
    RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_WHITE), 0x21, x, 0x2d, RH_CP(0x004e3ff4), num);
    if (redraw) RH_gxFontPrintf(FONT(S_F_EURO7), PAL(S_P_GREY), 0x21, 0x201, 0x16, RH_CP(0x004e3ff8), (const char*)info + 5);
}
static void fp_DashboardUpdate(Footprint& f) {
    if (!rh_xl_built(S_DASH_ONCE, 0xff) || !rh_xl_built(S_DASH_ONCE2, 1)) {
        f.replay_only = "the first call builds its Xlators (atexit)";
        return;
    }
    fp_dash_canvas(f);
    UI_FP(f, S_REDRAW, 1, "dash.obj redraw flag");
    UI_FP(f, S_DIRTY, 4, "dash.obj dirty rectangles");
    UI_FP(f, S_LAST, S_LAST_END - S_LAST, "what the dash last drew");
    static const uint32_t xls[] = {0x005042f8, 0x00504230, 0x005041d8, 0x00504130, 0x00504188, 0x005040e8, 0x00504100,
                                   0x005041c0, 0x005041b0};
    for (uint32_t a : xls) RH_FP_XL(f, a);
    UI_FP(f, S_TIME_STRING, 0x24, "PhysicsTimeString's buffer");
}
PORT_FN(0x00403850, "DashboardUpdate", DashboardUpdate_n, fp_DashboardUpdate)

// =========================================================================================================================
// the dirty rectangles
// =========================================================================================================================
struct Rect { volatile int32_t x0, y0, x1, y1; };
static __forceinline const Rect* rect_at(uint32_t a) { return (const Rect*)(uintptr_t)a; }

// clear_rect: the rectangle of the lowest bit set (8 at most), the background pasted into it
static void __cdecl clear_rect_n(int32_t flag) {
    uint32_t i = 0;
    int32_t v = flag;
    while (!(v & 1)) {
        v >>= 1;                                                // sar
        i++;
        if (i >= 9) ccall<void>(F_LogPanic, RH_CP(0x004e3ffc));   // "bad rect_flag"
    }
    const Rect* r = rect_at(S_FLAG_RECTS + i * 16);
    const int32_t y1 = r->y1;
    const int32_t x1 = (int32_t)(((uint32_t)r->x1 + 3) & ~3u);
    const int32_t y0 = r->y0;
    const int32_t x0 = (int32_t)((uint32_t)r->x0 & ~3u);
    ccall<void>(F_gxSetClip, RH_VP(S_CANVAS), x0, y0, x1, y1);
    ccall<void>(F_gxPaste, RH_VP(S_BACK), 0, 0);
    ccall<void>(F_gxRestoreClip, RH_VP(S_CANVAS));
}
static void fp_clear_rect(Footprint& f, int32_t flag) {
    if (!(flag & 0x1ff)) { f.replay_only = "no rectangle's bit: LogPanic"; return; }
    fp_dash_canvas(f);
}
PORT_FN(0x004046a0, "clear_rect", clear_rect_n, fp_clear_rect)

static void __cdecl clear_always_dirty_rects_n() {
    for (uint32_t a = S_RECTS; a < S_FLAG_RECTS; a += 16) {
        const Rect* r = rect_at(a);
        const int32_t y1 = r->y1;
        const int32_t y0 = r->y0;
        const int32_t x1 = (int32_t)(((uint32_t)r->x1 + 3) & ~3u);
        const int32_t x0 = (int32_t)((uint32_t)r->x0 & ~3u);
        ccall<void>(F_gxSetClip, RH_VP(S_CANVAS), x0, y0, x1, y1);
        ccall<void>(F_gxPaste, RH_VP(S_BACK), 0, 0);
        ccall<void>(F_gxRestoreClip, RH_VP(S_CANVAS));
    }
}
static void fp_clear_always_dirty_rects(Footprint& f) { fp_dash_canvas(f); }
PORT_FN(0x00404730, "clear_always_dirty_rects", clear_always_dirty_rects_n, fp_clear_always_dirty_rects)

// copy_rects: the dash's rectangles that changed (all three always, then those in the dirty mask) pasted onto the screen
// at the dash's x (centred on a screen wider than 640; -192 on 512 or 320)
static void __cdecl copy_rects_n(gxCanvas* c) {
    int32_t dx = (RH_G32(S_SCREEN_W) - 0x280) / 2;
    if (dx < 0 && (RH_G32(S_SCREEN_W) == 0x200 || RH_G32(S_SCREEN_W) == 0x140)) dx = -0xc0;
    if (RH_G32(S_DIRTY) == 0xffff) {
        if (dx) {
            const uint32_t col = RH_GU32(S_CLEAR);
            const int32_t h = RH_G32(S_CANVAS + 0xc);
            ccall<void>(F_gxRect, 0, 0, RH_G32(S_SCREEN_W), h, col);
        }
        ccall<void>(F_gxPaste, RH_VP(S_CANVAS), dx, 0);
    }
    for (uint32_t a = S_RECTS; a < S_FLAG_RECTS; a += 16) {
        const Rect* r = rect_at(a);
        const int32_t y1 = r->y1;
        const int32_t y0 = r->y0;
        const int32_t x1 = (int32_t)(((uint32_t)r->x1 + (uint32_t)dx + 3) & ~3u);
        const int32_t x0 = (int32_t)(((uint32_t)r->x0 + (uint32_t)dx) & ~3u);
        ccall<void>(F_gxSetClip, c, x0, y0, x1, y1);
        ccall<void>(F_gxPaste, RH_VP(S_CANVAS), dx, 0);
    }
    uint32_t a = S_FLAG_RECTS, bit = 1;
    for (int k = 9; k; k--, bit += bit, a += 16) {
        if (!(RH_GU32(S_DIRTY) & bit)) continue;
        const Rect* r = rect_at(a);
        const int32_t y1 = r->y1;
        const int32_t y0 = r->y0;
        const int32_t x1 = (int32_t)(((uint32_t)r->x1 + (uint32_t)dx + 3) & ~3u);
        const int32_t x0 = (int32_t)(((uint32_t)r->x0 + (uint32_t)dx) & ~3u);
        ccall<void>(F_gxSetClip, c, x0, y0, x1, y1);
        ccall<void>(F_gxPaste, RH_VP(S_CANVAS), dx, 0);
    }
    ccall<void>(F_gxRestoreClip, c);
}
static void fp_copy_rects(Footprint& f, gxCanvas* c) {
    UI_FP_CUR_CANVAS(f);
    UI_FP_CANVAS(f, c);
}
PORT_FN(0x00405370, "copy_rects", copy_rects_n, fp_copy_rects)

// =========================================================================================================================
// DashboardAlwaysUpdate: the banners for the next frame
// =========================================================================================================================
static void __cdecl DashboardAlwaysUpdate_n() {
    CarMsg* m = car_info(ccall<int32_t>(F_WorldGetFocusCar))->msg;
    RH_GU32(S_SHOW_NEXT) = 0;
    RH_G16(S_SHOW_NEXT + 4) = 0;
    const int32_t player = ccall<int32_t>(F_WorldGetPlayerCar);
    const int32_t focus = ccall<int32_t>(F_WorldGetFocusCar);
    const uint8_t mine = (uint32_t)focus - (uint32_t)player == 0;
    if (deity_finished(player)) {
        volatile float now = ccall<float>(F_PhysicsGetTime);
        if (!(RH_GU32(S_START_SEEN) & 0x7fffffffu)) RH_GU32(S_START_SEEN) = fbits(now);
        const uint8_t first = (uint32_t)car_info(ccall<int32_t>(F_WorldGetPlayerCar))->info.place - 1u == 0;
        const uint8_t alone = (uint32_t)ccall<int32_t>(F_AICarCount) - 1u == 0;
        if (!alone && first) {
            const float t = now;
            const float t0 = RH_GF(S_START_SEEN);
            if (!(D(t) - D(t0) >= 7.0)) RH_G8(S_SHOW_NEXT + 3) = (RH_GP(S_ST_AWARD) && mine) ? 1 : 0;   // the award
        }
        RH_G8(S_SHOW_NEXT) = (mine && !RH_G8(S_SHOW_NEXT + 3)) ? 1 : 0;                                // the summary
        if (RH_G32(S_CAMERA_STEP) == 0) RH_G32(S_CAMERA_STEP) = 1;
        else if (RH_G32(S_CAMERA_STEP) == 1 && !mine) {
            ccall<void>(F_MainSetCamera, 10);
            RH_G32(S_CAMERA_STEP) = 2;
        }
    }
    const int32_t now = ccall<int32_t>(F_PTimeNow);
    if (m->flags & 0x80) {
        if (RH_G32(S_OFF_TRACK_SINCE) == 0) RH_G32(S_OFF_TRACK_SINCE) = now;
    } else {
        RH_G32(S_OFF_TRACK_SINCE) = 0;
    }
    RH_G8(S_SHOW_NEXT + 2) =                                                                            // reset the car
        (mine && (m->flags & 0x80) && (int32_t)((uint32_t)now - (uint32_t)RH_G32(S_OFF_TRACK_SINCE)) > 0x7d0) ? 1 : 0;
    RH_G8(S_SHOW_NEXT + 1) = (mine && (m->flags & 0x40)) ? 1 : 0;                                      // damaged
    const int32_t rs = ccall<int32_t>(F_WorldGetRaceState);
    RH_G8(S_SHOW_NEXT + 4) = (rs > 0 && rs <= 0xb) ? 1 : 0;                                             // the countdown
    if (RH_G8(S_SHOW_NEXT + 4)) {
        const uint8_t again = RH_G8(S_SHOW_NEXT + 4);
        RH_G32(S_STARTED_AT) = now;
        if (again) {
            RH_G8(S_SHOW_NEXT + 5) = 0;
            return;
        }
    }
    RH_G8(S_SHOW_NEXT + 5) = (int32_t)((uint32_t)now - (uint32_t)RH_G32(S_STARTED_AT)) < 0x9c4 ? 1 : 0;   // just started
}
static void fp_DashboardAlwaysUpdate(Footprint& f) {
    UI_FP(f, S_SHOW_NEXT, 6, "the next frame's banners");
    UI_FP(f, S_START_SEEN, 8, "dash.obj finish time, camera step");
    UI_FP(f, S_STARTED_AT, 8, "dash.obj start and off-track times");
    UI_FP(f, S_MAIN_CAMERA, 4, "main.obj camera (MainSetCamera)");
}
PORT_FN(0x00404820, "DashboardAlwaysUpdate", DashboardAlwaysUpdate_n, fp_DashboardAlwaysUpdate)

// =========================================================================================================================
// the banners' predicates
// =========================================================================================================================
static uint8_t __cdecl DashboardMustDraw_n() {
    if (ccall<uint8_t>(F_show_paused) || ccall<uint8_t>(F_show_loading) || ccall<uint8_t>(F_show_summary) ||
        ccall<uint8_t>(F_show_damaged) || ccall<uint8_t>(F_show_award) || ccall<uint8_t>(F_show_countdown) ||
        ccall<uint8_t>(F_show_resetcar))
        return 1;
    return 0;
}
static void fp_none(Footprint&) {}
PORT_FN(0x00404a80, "DashboardMustDraw", DashboardMustDraw_n, fp_none)

static uint8_t __cdecl show_paused_n() {
    if (!ccall<uint8_t>(F_no_banners) && ccall<uint8_t>(F_PhysicsIsPaused) && !ccall<uint8_t>(F_EscapeMenuActive)) return 1;
    return 0;
}
PORT_FN(0x00404ad0, "show_paused", show_paused_n, fp_none)

static uint8_t __cdecl no_banners_n() {
    if (RH_G8(S_SHOW) || ccall<uint8_t>(F_MainIsDone)) return 1;
    return 0;
}
PORT_FN(0x00404b00, "no_banners", no_banners_n, fp_none)

static uint8_t __cdecl show_countdown_n() {
    if (!ccall<uint8_t>(F_no_banners) && (RH_G8(S_SHOW + 4) || RH_G8(S_SHOW + 5)) && ccall<int32_t>(F_WorldGetRaceState) <= 0xb)
        return 1;
    return 0;
}
PORT_FN(0x00404b20, "show_countdown", show_countdown_n, fp_none)

static uint8_t __cdecl show_resetcar_n() { return !ccall<uint8_t>(F_no_banners) && RH_G8(S_SHOW + 2) ? 1 : 0; }
PORT_FN(0x00404b50, "show_resetcar", show_resetcar_n, fp_none)
static uint8_t __cdecl show_damaged_n() { return !ccall<uint8_t>(F_no_banners) && RH_G8(S_SHOW + 1) ? 1 : 0; }
PORT_FN(0x00404b70, "show_damaged", show_damaged_n, fp_none)
static uint8_t __cdecl show_award_n() { return !ccall<uint8_t>(F_no_banners) && RH_G8(S_SHOW + 3) ? 1 : 0; }
PORT_FN(0x00404b90, "show_award", show_award_n, fp_none)
static uint8_t __cdecl show_summary_n() { return RH_G8(S_SHOW); }
PORT_FN(0x00404bb0, "show_summary", show_summary_n, fp_none)
static uint8_t __cdecl show_loading_n() { return ccall<uint8_t>(F_MainIsDone); }   // jmp MainIsDone
PORT_FN(0x00404bc0, "show_loading", show_loading_n, fp_none)

// =========================================================================================================================
// TheRealDashboardDraw
// =========================================================================================================================
static void __cdecl TheRealDashboardDraw_n(gxCanvas* c, uint8_t full) {
    CarMsg* m = car_info(ccall<int32_t>(F_WorldGetFocusCar))->msg;
    ccall<void*>(F_gxSetCanvas, c);
    if (full) {
        ccall<void>(F_draw_fps);
        rh_xl_once(S_DRAW_ONCE, 1, 0x005042b8, 0x004e400c, 0x00405530);   // Dash
        ccall<void>(F_copy_rects, c);
        const uint32_t rpm = m->rpm;
        const int32_t gear = m->gear;
        if (RH_G32(S_SCREEN_W) >= 0x280) {
            rh_xl_once(S_DRAW_ONCE, 2, 0x00504280, 0x004e402c, 0x00405520);
            rh_xl_once(S_DRAW_ONCE, 4, 0x00504208, 0x004e4058, 0x00405510);
            if (!(RH_G8(S_DRAW_ONCE) & 8)) {
                RH_G8(S_DRAW_ONCE) = (uint8_t)(RH_G8(S_DRAW_ONCE) | 8);
                const char* rev = xlate(0x00504280);
                RH_GU32(S_GEAR_NAMES) = (uint32_t)(uintptr_t)rev;
                xlate(0x00504208);
                for (uint32_t k = 0; k < 7; k++) RH_GU32(S_GEAR_NAMES + 8 + 4 * k) = 0x004e4084u + 4 * k;   // "1".."7"
                RH_GU32(S_GEAR_NAMES + 4) = RH_GU32(0x0050420c);
            }
            if (ccall<int32_t>(F_WorldGetCameraType) == 0) {
                const int32_t h = RH_G32(S_SCREEN_H);
                ccall<void>(F_gxRect, 0x14, h - 0x29, 0x28, h - 0xf, RH_GU32(S_CLEAR));
                const uint32_t name = RH_GU32(S_GEAR_NAMES + 4 + (uint32_t)gear * 4u);
                RH_gxFontPrintf(FONT(S_F_LCD), PAL(S_P_WHITE), 0xc, 0x1e, h - 0x28, RH_CP(0x004e40a0), name);
            } else if (ccall<int32_t>(F_WorldGetCameraType) != 0xc) {
                ccall<void>(F_gxDrawStamp, RH_GP(S_ST_RPM), 0x10, RH_G32(S_SCREEN_H) - 0x90, 0, (void*)0);
                const uint32_t name = RH_GU32(S_GEAR_NAMES + 4 + (uint32_t)gear * 4u);
                RH_gxFontPrintf(FONT(S_F_LCD), PAL(S_P_WHITE), 0xc, 0x50, RH_G32(S_SCREEN_H) - 0x49, RH_CP(0x004e40a4), name);
                uint32_t v = rpm;
                if ((int32_t)v < 0x43fa0000) v = 0x43fa0000;       // 500 rpm at least (compared as bits)
                ((Dial_t)(uintptr_t)F_draw_analog_dial)(0x50, RH_G32(S_SCREEN_H) - 0x50, 0x32, v, 0xbe8efa35u, 0x3a2ddc49u);
            }
        }
    }
    rh_xl_once(S_DRAW_ONCE, 0x10, 0x005042e0, 0x004e40a8, 0x00405500);
    rh_xl_once(S_DRAW_ONCE, 0x20, 0x00504220, 0x004e40c0, 0x004054f0);
    rh_xl_once(S_DRAW_ONCE, 0x40, 0x00504168, 0x004e40d8, 0x004054e0);
    if (ccall<uint8_t>(F_show_paused)) ccall<void>(F_SplashButDontGrabScreen, xlate(0x005042e0));
    if (ccall<uint8_t>(F_show_countdown)) {
        int32_t x = RH_G32(S_SCREEN_W) / 2;
        int32_t y = RH_G32(S_SCREEN_H) / 3 - 0x14;
        const int32_t rs = ccall<int32_t>(F_WorldGetRaceState);
        int32_t frame = (int32_t)(5u - (uint32_t)rs);
        if (frame < 0) frame = (int32_t)(0u - (uint32_t)frame) % 2;
        y -= ccall<int32_t>(F_gxStampHeight, RH_GP(S_ST_START)) / 2;
        x -= ccall<int32_t>(F_gxStampWidth, RH_GP(S_ST_START)) / 2;
        ccall<void>(F_gxDrawStamp, RH_GP(S_ST_START), x, y, frame, (void*)0);
    }
    if (ccall<uint8_t>(F_show_resetcar)) {
        xlate(0x005042a8);
        const char* t = UI_GP(const char, 0x005042ac);
        void* font = FONT(S_F_EURO9);
        const int32_t w = ccall<int32_t>(F_gxFontStringWidth, font, t);
        int32_t x = (int32_t)((uint32_t)RH_G32(S_SCREEN_W) - (uint32_t)(w + 0x14));
        ccall<int32_t>(F_gxFontHeight, font);
        x += 0xa;
        RH_gxFontPrintf(font, PAL(S_P_WHITE), 9, x, 0x43, t);
    }
    if (ccall<uint8_t>(F_show_damaged)) {
        xlate(0x00504110);
        const char* t = UI_GP(const char, 0x00504114);
        void* font = FONT(S_F_EURO9);
        ccall<int32_t>(F_gxFontStringWidth, font, t);
        ccall<int32_t>(F_gxFontHeight, font);
        RH_gxFontPrintf(font, PAL(S_P_WHITE), 9, 0xa, 0x43, t);
    }
    if (ccall<uint8_t>(F_show_award)) {
        const int32_t y = RH_G32(S_SCREEN_H) / 2 - 0x82;
        ccall<int32_t>(F_gxStampHeight, RH_GP(S_ST_AWARD));
        const int32_t w = RH_G32(S_SCREEN_W);
        const int32_t sw = ccall<int32_t>(F_gxStampWidth, RH_GP(S_ST_AWARD));
        ccall<void>(F_gxDrawStamp, RH_GP(S_ST_AWARD), (int32_t)((uint32_t)w - (uint32_t)sw) / 2, y, 0, (void*)0);
    }
    if (ccall<uint8_t>(F_show_summary)) {
        rh_xl_once(S_DRAW_ONCE, 0x80, 0x005040c8, 0x004e40f4, 0x004054d0);
        rh_xl_once(S_DRAW_ONCE2, 1, 0x00504290, 0x004e410c, 0x004054c0);
        const int32_t cx = RH_G32(S_SCREEN_W) / 2;
        {
            const int32_t h = RH_G32(S_SCREEN_H);
            const int32_t sh = ccall<int32_t>(F_gxStampHeight, RH_GP(S_ST_SUMMARY));
            const int32_t y = (int32_t)((uint32_t)h - (uint32_t)sh) / 2;
            const int32_t w = RH_G32(S_SCREEN_W);
            const int32_t sw = ccall<int32_t>(F_gxStampWidth, RH_GP(S_ST_SUMMARY));
            ccall<void>(F_gxDrawStamp, RH_GP(S_ST_SUMMARY), (int32_t)((uint32_t)w - (uint32_t)sw) / 2, y, 0, (void*)0);
        }
        int32_t row = (RH_G32(S_SCREEN_H) - 0xb4) / 2 - 3;
        const char* t1 = xlate(0x005040c8);
        RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_SUM_ME), 0x24, cx, row - 5, t1);
        const char* t2 = xlate(0x00504290);
        RH_gxFontPrintf(FONT(S_F_EURO9), PAL(S_P_SUM_ME), 0x24, cx, row + 0xc2, t2);
        row += 0x14;
        int32_t p = 0;
        if (ccall<int32_t>(F_CarMgrCount) > 0) {
            do {
                int32_t car = 0;
                if (ccall<int32_t>(F_CarMgrCount) > 0) {
                    do {
                        CarMgrInfo* i = car_info(car);
                        if (i->type != 3 && (uint32_t)i->info.place - (uint32_t)p == 1) {
                            void* pal = ccall<int32_t>(F_WorldGetPlayerCar) == car ? PAL(S_P_SUM_ME) : PAL(S_P_SUM);
                            RH_gxFontPrintf(FONT(S_F_EURO9), pal, 0x21, cx - 0x91, row, RH_CP(0x004e4120), (int32_t)i->info.place);
                            RH_gxFontPrintf(FONT(S_F_EURO9), pal, 0x21, cx - 0x6b, row, RH_CP(0x004e4124), (const char*)i + 5);
                            if (deity_finished(car)) {
                                volatile float t = ccall<float>(F_RecordGetRaceTime, car);
                                const char* s = RH_PhysicsTimeString(fbits(t), 1);
                                RH_gxFontPrintf(FONT(S_F_EURO9), pal, 0x21, cx + 0x41, row, RH_CP(0x004e4128), s);
                            }
                            row += 0x14;
                        }
                        car++;
                    } while (ccall<int32_t>(F_CarMgrCount) > car);
                }
                p++;
            } while (ccall<int32_t>(F_CarMgrCount) > p);
        }
    }
    if (ccall<uint8_t>(F_show_loading)) {
        rh_xl_once(S_DRAW_ONCE2, 2, 0x005040d8, 0x004e412c, 0x004054b0);
        ccall<void>(F_SplashButDontGrabScreen, xlate(0x005040d8));
    }
}
// what a draw of the banners may reach: replay_only while an Xlator is unbuilt, or when the pause or loading banner
// (generic_splash: loads load.stp) would show
static bool fp_banners(Footprint& f) {
    if (!rh_xl_built(S_DRAW_ONCE, 0xf7) || !rh_xl_built(S_DRAW_ONCE2, 3)) {
        f.replay_only = "the first call builds its Xlators (atexit)";
        return false;
    }
    const bool no_banners = RH_G8(S_SHOW) || RH_G8(S_MAIN_DONE);
    if (RH_G8(S_MAIN_DONE) || (!no_banners && RH_G8(S_PAUSED) && !RH_GP(S_ESC_MENU))) {
        f.replay_only = "the pause / loading banner loads its stamp (generic_splash)";
        return false;
    }
    return true;
}
static void fp_TheRealDashboardDraw(Footprint& f, gxCanvas* c, uint8_t) {
    if (!fp_banners(f)) return;
    UI_FP_CUR_CANVAS(f);
    UI_FP_CANVAS(f, c);
    UI_FP(f, S_GEAR_NAMES, 0x24, "the gear names");
    UI_FP(f, S_DRAW_ONCE, 1, "the gear names' guard (bit 8)");
    static const uint32_t xls[] = {0x00504280, 0x00504208, 0x005042e0, 0x005042a8, 0x00504110, 0x005040c8, 0x00504290,
                                   0x005040d8};
    for (uint32_t a : xls) RH_FP_XL(f, a);
    UI_FP(f, S_TIME_STRING, 0x24, "PhysicsTimeString's buffer");
}
PORT_FN(0x00404be0, "TheRealDashboardDraw", TheRealDashboardDraw_n, fp_TheRealDashboardDraw)

static void __cdecl DashboardDrawDashNone_n(gxCanvas* c) { ccall<void>(F_TheRealDashboardDraw, c, 0); }
static void fp_DashboardDrawDashNone(Footprint& f, gxCanvas* c) { fp_TheRealDashboardDraw(f, c, 0); }
PORT_FN(0x00404bd0, "DashboardDrawDashNone", DashboardDrawDashNone_n, fp_DashboardDrawDashNone)

static void __cdecl DashboardDraw_n(gxCanvas* c) { ccall<void>(F_TheRealDashboardDraw, c, 1); }
static void fp_DashboardDraw(Footprint& f, gxCanvas* c) { fp_TheRealDashboardDraw(f, c, 1); }
PORT_FN(0x00405540, "DashboardDraw", DashboardDraw_n, fp_DashboardDraw)

// =========================================================================================================================
// DashboardDraw3D: this frame's banners (the next frame's flags copied), and their translucent backing rectangles
// =========================================================================================================================
static void __cdecl DashboardDraw3D_n() {
    const uint32_t a = RH_GU32(S_SHOW_NEXT);
    const uint16_t b = RH_G16(S_SHOW_NEXT + 4);
    RH_GU32(S_SHOW) = a;
    uint8_t begun = 0;
    const uint8_t award = RH_G8(S_SHOW_NEXT + 3);
    RH_G16(S_SHOW + 4) = b;
    if (award) {
        const int32_t w = ccall<int32_t>(F_gxStampWidth, RH_GP(S_ST_AWARD));
        const int32_t bw = (int32_t)((uint32_t)w * 130u) / 100;
        const int32_t h = ccall<int32_t>(F_gxStampHeight, RH_GP(S_ST_AWARD));
        const int32_t y0 = RH_G32(S_SCREEN_H) / 2 - 0x8c;
        const int32_t x0 = (int32_t)((uint32_t)RH_G32(S_SCREEN_W) - (uint32_t)bw) / 2;
        const int32_t y1 = h + 0x14 + y0;
        const int32_t x1 = x0 + bw;
        begun = 1;
        ccall<void>(F_mrBeginAlphaRects);
        ccall<void>(F_mrDrawAlphaRect, x0, y0, x1, y1, 0x7f000000u);
    }
    if (RH_G8(S_SHOW)) {
        const int32_t w = RH_G32(S_SCREEN_W);
        const int32_t sw = ccall<int32_t>(F_gxStampWidth, RH_GP(S_ST_SUMMARY));
        const int32_t x0 = (int32_t)((uint32_t)w - (uint32_t)sw) / 2;
        const int32_t h = RH_G32(S_SCREEN_H);
        const int32_t sh = ccall<int32_t>(F_gxStampHeight, RH_GP(S_ST_SUMMARY));
        const int32_t y0 = (int32_t)((uint32_t)h - (uint32_t)sh) / 2;
        const int32_t x1 = x0 + ccall<int32_t>(F_gxStampWidth, RH_GP(S_ST_SUMMARY));
        const int32_t y1 = ccall<int32_t>(F_gxStampHeight, RH_GP(S_ST_SUMMARY)) + y0;
        if (!begun) ccall<void>(F_mrBeginAlphaRects);
        begun = 1;
        ccall<void>(F_mrDrawAlphaRect, x0, y0, x1, y1, 0x7f000000u);
    }
    if (!ccall<uint8_t>(F_no_banners) && RH_G8(S_SHOW + 1)) {
        xlate(0x00504110);
        void* font = FONT(S_F_EURO9);
        const int32_t x1 = ccall<int32_t>(F_gxFontStringWidth, font, UI_GP(const char, 0x00504114)) + 0x14;
        const int32_t y1 = ccall<int32_t>(F_gxFontHeight, font) + 8;
        if (!begun) ccall<void>(F_mrBeginAlphaRects);
        begun = 1;
        ccall<void>(F_mrDrawAlphaRect, 0, 0x40, x1, y1 + 0x40, 0x7f000000u);
    }
    if (!ccall<uint8_t>(F_no_banners) && RH_G8(S_SHOW + 2)) {
        xlate(0x005042a8);
        void* font = FONT(S_F_EURO9);
        const int32_t bw = ccall<int32_t>(F_gxFontStringWidth, font, UI_GP(const char, 0x005042ac)) + 0x14;
        int32_t x0 = (int32_t)((uint32_t)RH_G32(S_SCREEN_W) - (uint32_t)bw);
        const int32_t fh = ccall<int32_t>(F_gxFontHeight, font);
        x0--;
        const int32_t y1 = fh + 8;
        if (!begun) ccall<void>(F_mrBeginAlphaRects);
        begun = 1;
        ccall<void>(F_mrDrawAlphaRect, x0, 0x40, bw + x0, y1 + 0x40, 0x7f000000u);
    }
    if (begun) ccall<void>(F_mrEndAlphaRects);
}
static void fp_DashboardDraw3D(Footprint& f) {
    if (RH_G8(S_SHOW_NEXT) || RH_G8(S_SHOW_NEXT + 1) || RH_G8(S_SHOW_NEXT + 2) || RH_G8(S_SHOW_NEXT + 3)) {
        f.replay_only = "draws the banners' rectangles (mrBeginAlphaRects can load a texture)";
        return;
    }
    UI_FP(f, S_SHOW, 6, "this frame's banners");
}
PORT_FN(0x00405550, "DashboardDraw3D", DashboardDraw3D_n, fp_DashboardDraw3D)

// =========================================================================================================================
// draw_analog_dial: the needle, a triangle from the centre (x, y) out to radius r at angle v * b + a, and its two edges
// =========================================================================================================================
static void __cdecl draw_analog_dial_n(int32_t x, int32_t y, int32_t r, float v, float a, float b) {
    const double ang = D(b) * D(v) + D(a);                     // fld b; fmul v; fadd a
    const double rr = D(r);                                    // fild r
    const int32_t tx = (int32_t)((uint32_t)x + (uint32_t)x87_ftol(-x87_sin_mul(ang, rr)));   // the tip
    const int32_t ty = (int32_t)((uint32_t)y + (uint32_t)x87_ftol(x87_cos_mul(ang, rr)));
    const int32_t ax = (int32_t)((uint32_t)x87_ftol(x87_cos_mul(ang, 3.0)) + (uint32_t)x);    // the base's two corners
    const int32_t ay = (int32_t)((uint32_t)x87_ftol(x87_sin_mul(ang, 3.0)) + (uint32_t)y);
    const int32_t bx = (int32_t)((uint32_t)x + (uint32_t)x87_ftol(x87_cos_mul(ang, -3.0)));
    const int32_t by = (int32_t)((uint32_t)y + (uint32_t)x87_ftol(x87_sin_mul(ang, -3.0)));
    ccall<void>(F_gxTriangle, tx, ty, ax, ay, bx, by, RH_GU32(S_C_NEEDLE));
    ccall<void>(F_gxLine, tx, ty, ax, ay, RH_GU32(S_CLEAR));
    ccall<void>(F_gxLine, tx, ty, bx, by, RH_GU32(S_C_NEEDLE2));
}
static void fp_draw_analog_dial(Footprint& f, int32_t, int32_t, int32_t, float, float, float) { UI_FP_CUR_CANVAS(f); }
PORT_FN(0x00405780, "draw_analog_dial", draw_analog_dial_n, fp_draw_analog_dial)

}  // namespace rdash
}  // namespace
