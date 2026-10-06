// root_replay.cpp -- M3 UI stage, step U3 (group C): replay.obj, rewritten faithfully (library `root`).
//
//   ReplayDo: the replay screen (after a race, handed its world; or from the menus, with none: load a file), a dialog
//   of the replay's buttons -- load, save, full screen, analysis, exit -- around a ReplayView (a UICustomControl: the
//   3D view behind, Draw3D; the transport, camera and car buttons, their hot keys, the car's and camera's names, and a
//   JogControl, Added). Its idle function replay_idle drives the transport (the shuttle keys and buttons: update_shuttle
//   -> PhysReplayShuttle), the play / pause groups, the focus car's name, speed and gear, the camera's name, the
//   elapsed time. The buttons' callbacks (play, stop, rewind / fast-forward, to start / to end, the twelve cameras,
//   next / previous view and car); the full-screen view (fullscreen_cb: its own frame loop, the same keys); the load and
//   save boxes (ReplayLoadDialog, load_cb, save_cb, create_replay_dir); the analysis screen (do_analysis: the same
//   buttons, a GraphControl of the telemetry -- longitudinal, lateral g or speed -- with its zoom and scale); a replay
//   loaded straight from a file (auto_load_replay) and the replay benchmark (ReplayBenchmark, benchmark_loop). And the
//   helpers the linker kept here for everyone: UIDialogItem's constructor (every dialog in the game builds its items
//   with it), UICustomControl's empty methods and deleting destructor, UI_BBUTTON / UI_BREPEATBUTTON / UI_CUSTOM.
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this file's own too, so a
// hooked rewrite is what runs), virtual calls through the vtable, the compiler's inline string code as it runs it
// (ui_types.h). ReplayDo's and do_analysis's frames are laid out as the original's (the dialog, the controls and the
// item list at the same offsets from the frame's bottom); the items the original builds with UIDialogItem's constructor
// are built with it, the ones it writes inline are written with the same 14 dwords; each translation is read after its
// Xlator's refresh, where the original reads it. x87: register values doubles, stored ones floats, the original's
// grouping and constants' widths, __ftol as x87_ftol, comparisons with the original's NaN outcome; the jog wheel's
// fpatan / fcos / fsin and the CRT's _CIfmod run in asm blocks that finish what the original does with their results
// (docs/PORTING.md rule 9).
//
// Footprints (main thread). replay_only: whatever runs a dialog or its own loop (ReplayDo, fullscreen_cb, do_analysis,
// benchmark_loop, ReplayBenchmark, the load / save / error boxes), loads or saves a file or creates a directory (load_cb,
// save_cb, auto_load_replay, create_replay_dir, ReplayLoadDialog), builds widgets or adds notifications (ReplayView::Added,
// JogControl::Create), loads or forgets stamps (JogControl's constructor, the deleting destructors), drives the replay
// player or the sound (every PhysReplayPause / Resume / Rewind / FastForward / Shuttle caller: the physics thread reads
// that state as it runs; SoundMuteCars), updates or draws the world or sets the 3D view (ReplayView::Draw3D / Create).
// replay_idle is replay_only unless there's no replay (then it only clears the file name). The rest are shadow-checked:
// the camera and shuttle callbacks write their statics, the focus-car ones the camera's focus (0x521614, as
// wld_world.cpp lists it), the draws their canvas, the Callbacks / Update the widget they dirty, GraphControl::CharHit its
// zoom and type, the item helpers their item.
//
// FIX CANDIDATEs (left faithful, marked in place): a replay file's track name is copied unbounded into a small stack
// buffer (with ".trk") by ReplayDo, load_cb, auto_load_replay and ReplayBenchmark -- World::is_valid_version checks only
// the version and size, so a damaged file whose name isn't terminated overruns the stack; GraphControl::Draw divides by
// its width / 4 (a control narrower than 4 pixels divides by zero: the analysis screen's is 0x104).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "root_replay.h"

namespace {
namespace root_replay {
using namespace uit;
using namespace rrep;

#define D(x) ((double)(x))
#define CP(p) ((const char*)(uintptr_t)(p))
static __forceinline uint32_t U(const volatile void* p) { return (uint32_t)(uintptr_t)p; }
#define G32(a) UI_G32(a)
#define GU32(a) UI_GU32(a)
#define G8(a) UI_G8(a)
#define GF(a) UI_GF(a)
#define GP(T, a) UI_GP(T, a)
static __forceinline float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
enum : uint32_t { F_UIDoDialog_ = 0x00478ff0, F_UIUpdateTime_ = 0x004785a0, S_W = 0x005228f4, S_H = 0x005228d4 };

// ---- helpers --------------------------------------------------------------------------------------------------------------
// an Xlator refreshed (the cookie compared, Xlator::xlate called when stale)
static __forceinline void xl(uint32_t x) {
    if (GU32(x + 8) != GU32(S_XLATOR_COOKIE)) tcall<void>(F_Xlator_xlate, (void*)(uintptr_t)x);
}
static __forceinline const char* xlt(uint32_t x) { xl(x); return GP(const char, x + 4); }
// a function-local static Xlator's guard: `test [guard], bit; jne; or ...; Xlator::Xlator; atexit(helper)`
static __forceinline void once_xl(uint32_t guard, uint8_t bit, uint32_t x, uint32_t key, uint32_t helper) {
    const uint8_t g = G8(guard);
    if (g & bit) return;
    G8(guard) = (uint8_t)(g | bit);
    tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)x, CP(key));
    ccall<int>(F_atexit, helper);
}
// UIDialogItem::UIDialogItem by address: all 14 dwords as the original pushes them
static __forceinline void ictor(void* it, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t text,
                                uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo = 0, uint32_t hi = 0, uint32_t sel = 0,
                                uint32_t s34 = 0) {
    tcall<void*>(F_UIDialogItem_ctor, it, type, id, x, y, w, h, text, i1c, data, style, lo, hi, sel, s34);
}
// an item the original writes inline: all 14 dwords
static __forceinline void raw14(void* p, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t text,
                                uint32_t i1c, uint32_t data, uint32_t style) {
    volatile uint32_t* d = (volatile uint32_t*)p;
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text;
    d[7] = i1c; d[8] = data; d[9] = style; d[10] = 0; d[11] = 0; d[12] = 0; d[13] = 0;
}
static __forceinline void item_end(void* it) { crt_copy(it, (const void*)(uintptr_t)S_END_ITEM, 0x38); }
static __forceinline void set_view() { ccall<void>(F_mrSetView, 0, 0, G32(S_W), G32(S_H), (uint32_t)1); }
static __forceinline void cb0(uint32_t fn) { ccall<uint8_t>(fn, 0); }
typedef void(__cdecl* Sprintf_)(char*, const char*, ...);
// a function returning a float in st0, stored as a float (`call; fstp dword`)
static VP_ASM_CALLS_INLINE float call_f(uint32_t fn) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[fn]\n\t"
                     "call eax\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [fn] "m"(fn)
                     : VP_X87_CLOBBERS, "eax", "ecx", "edx", "cc", "memory");
#else
    __asm { mov eax, fn
            call eax
            fstp dword ptr [r] }
#endif
    return r;
}
// "<track>.trk": strcpy, then the literal's five bytes (a dword and a byte) at the end
static __forceinline void track_res(char* dst, const char* track, uint32_t lit) {
    // FIX CANDIDATE: unbounded (the caller's buffer is 0x30..0x40 bytes); a damaged replay file's unterminated track
    // name overruns it (World::is_valid_version checks only the version and the size)
    crt_strcpy(dst, track);
    char* e = dst + crt_strlen(dst);
    const uint32_t d0 = GU32(lit);
    const uint8_t b4 = G8(lit + 4);
    *(volatile uint32_t*)e = d0;
    *(volatile uint8_t*)(e + 4) = b4;
}
#define REPLAY_TRACK CP(S_REPLAY_WORLD + W_TRACK)

// ---- footprints ---------------------------------------------------------------------------------------------------------------
static void fp_draw_canvas(Footprint& f, gxCanvas* c) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}
static void fp_dirty(Footprint& f, const RvCtl* c) {
    if (c && c->widget) f.add((void*)c->widget, sizeof(CustomWidget), "its widget");
}
static void fp_cb_player(Footprint& f, int32_t) { f.replay_only = "drives the replay player (PhysReplay*: the physics thread reads it)"; }
static void fp_cb_focus(Footprint& f, int32_t) {
    if (G8(S_HAVE_REPLAY)) UI_FP(f, 0x00521614, 4, "physics camera_focus (CameraSetFocusCar)");
}
static void fp_cb_camera(Footprint& f, int32_t) { UI_FP(f, S_CAMERA, 4, "the replay's camera"); }
static void fp_cb_shuttle(Footprint& f, int32_t) { UI_FP(f, S_SHUTTLE, 4, "the shuttle button held"); }
static void fp_modal_i(Footprint& f, int32_t) { f.replay_only = "runs a dialog or its own frame loop, loads or saves a file"; }
static void fp_modal0(Footprint& f) { f.replay_only = "runs a dialog or its own frame loop, loads or saves a file"; }

// =========================================================================================================================
// ReplayDo (0x406bc0)
// =========================================================================================================================
static void __cdecl ReplayDo_n(const uint8_t* w) {
    alignas(4) uint8_t fr[0x334];                                 // the original's frame, from its bottom
#define P(o) ((void*)(fr + (o)))
#define PD(o) (*(volatile uint32_t*)(fr + (o)))
    ReplayView* view = (ReplayView*)P(0x24);
    ccall<void>(F_WorldSwitchToDrive);
    ((Sprintf_)(uintptr_t)F_sprintf)((char*)(uintptr_t)S_REPLAY_DIR, CP(0x004e48a8), ccall<const char*>(F_Win32GetUserDirectory));
    const uint8_t have = w != 0 ? 1 : 0;
    G8(S_FROM_RACE) = have;
    G8(S_HAVE_REPLAY) = have;
    const uint8_t autoload = (w == 0 && G8(S_REPLAY_FILE) != 0) ? 1 : 0;
    G8(S_AUTOLOAD) = autoload;
    if (autoload) UI_LogReport(CP(0x004e48b4), CP(S_REPLAY_FILE));    // "Please open: %s"
    ccall<void>(F_WorldFXDisable);
    ccall<void>(F_SoundMuteCars);
    if (G8(S_HAVE_REPLAY)) {
        crt_copy((void*)(uintptr_t)S_REPLAY_WORLD, w, W_SIZE);
        ccall<void>(F_PhysReplayPlayBegin);
    }
    if (!G8(S_FROM_RACE)) ccall<void>(F_ResourceSetUnload, CP(0x004e48c4));
    ccall<void>(F_ResourceSetMustLoad, CP(0x004e48cc));
    ccall<void>(F_ResourceSetMustLoad, CP(0x004e48d8));
    view->widget = 0;
    G8(S_TIME_TEXT) = 0;
    G32(S_CAMERA) = 0xa;
    view->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    tcall<void*>(F_JogControl_ctor, &view->jog);
    G32(S_FRAME8) = 0;
    view->vtbl = (const void*)(uintptr_t)VT_ReplayView;
    GU32(S_VIEW) = U(view);
    once_xl(S_ONCE_REPLAYDO, 0x01, 0x005053d0, 0x004e48e8, 0x00407320);
    once_xl(S_ONCE_REPLAYDO, 0x02, 0x00505810, 0x004e48fc, 0x00407310);
    once_xl(S_ONCE_REPLAYDO, 0x04, 0x00505680, 0x004e4910, 0x00407300);
    once_xl(S_ONCE_REPLAYDO, 0x08, 0x005053b0, 0x004e4928, 0x004072f0);
    once_xl(S_ONCE_REPLAYDO, 0x10, 0x00505440, 0x004e4940, 0x004072e0);
    raw14(P(0x64), 0x17, 0, 0x7a, 0x88, 0x114, 0xcc, 0x004e4db8, 0, U(view), 0);
    ictor(P(0x9c), 1, 0, 0, 0, 0, 0, 0, 0, S_GROUP_ANALYSIS, 0);
    ictor(P(0xd4), 2, 0, 0x181, 6, 0, 0, U(xlt(0x005053d0)), 0, F_load_cb, 4);
    ictor(P(0x10c), 1, 1, 0, 0, 0, 0, 0, 0, S_GROUP_ANALYSIS, 0);
    ictor(P(0x144), 1, 0, 0, 0, 0, 0, 0, 0, S_GROUP_MAIN, 0);
    raw14(P(0x17c), 2, 0, 0x181, 6, 0, 0, U(xlt(0x00505810)), 0, F_save_cb, 4);
    raw14(P(0x1b4), 1, 1, 0, 0, 0, 0, 0, 0, S_GROUP_MAIN, 0);
    ictor(P(0x1ec), 2, 0, 0x181, 0x1c, 0, 0, U(xlt(0x00505680)), 0, F_fullscreen_cb, 4);
    raw14(P(0x224), 2, 0xd, 0x181, 0x32, 0, 0, U(xlt(0x005053b0)), 0, 0, 4);
    raw14(P(0x25c), 2, 0xffffffffu, 0x181, 0x48, 0, 0, U(xlt(0x00505440)), 0, 0, 4);
    ictor(P(0x294), 4, 0xfffffffeu, 0x1b, 0, 0, 0, 0, 0, 0, 0);
    item_end(P(0x2cc));
    PD(0x10) = 0x004e4954;                                        // the dialog: no title, replback.stp, default -2
    PD(0x14) = 0x004e4958;
    PD(0x18) = 0xfffffffeu;
    PD(0x20) = F_replay_idle;
    PD(0x1c) = U(P(0x64));
    uint8_t done = 0;
    ccall<void>(F_SoundMuteCars);
    do {
        const int32_t x = G32(S_W) - 0x1f4;
        const int32_t r = ccall<int32_t>(F_UIDoDialog_, P(0x10), 0x1f4, 0x64, x / 2, 0x14, 2);
        const uint8_t hv = G8(S_HAVE_REPLAY);
        if (r != 0xd) done = 1;
        else if (hv) done = ccall<uint8_t>(F_do_analysis) == 0 ? 1 : 0;
    } while (!done);
    ccall<void>(F_SoundMuteCars);
    ccall<void>(F_SplashLoading);
    if (G8(S_HAVE_REPLAY)) {
        ccall<void>(F_PhysReplayPlayEnd);
        if (w == 0) {
            ccall<void>(F_WorldEndReplay);
            ccall<void>(F_TelemetryEnd);
            track_res((char*)P(0x304), REPLAY_TRACK, 0x004e4968);
            ccall<void>(F_ResourceSetUnload, (const char*)P(0x304));
            ccall<void>(F_UnloadCars, (const void*)(uintptr_t)S_REPLAY_WORLD);
        }
    }
    view->jog.vtbl = (const void*)(uintptr_t)VT_JogControl;      // ~JogControl, inline
    ccall<void>(F_gxForgetStamp, (void*)view->jog.detent);
    ccall<void>(F_gxForgetStamp, (void*)view->jog.wheel);
    ccall<void>(F_ResourceSetUnload, CP(0x004e4970));
    ccall<void>(F_ResourceSetUnload, CP(0x004e4980));
    if (!G8(S_FROM_RACE)) ccall<void>(F_ResourceSetMustLoad, CP(0x004e498c));
    ccall<void>(F_WorldSwitchToUI);
    G8(S_REPLAY_FILE) = 0;
#undef P
#undef PD
}
static void fp_ReplayDo(Footprint& f, const uint8_t*) { f.replay_only = "runs the replay screen (a dialog, loads, the replay player)"; }
PORT_FN(0x00406bc0, "ReplayDo", ReplayDo_n, fp_ReplayDo)

// =========================================================================================================================
// replay_idle (0x407330): the replay screen's idle function
// =========================================================================================================================
static uint8_t __cdecl replay_idle_n(int32_t*) {
    if (G8(S_AUTOLOAD)) {
        G8(S_AUTOLOAD) = 0;
        cb0(F_load_cb);
    }
    G8(S_REPLAY_FILE) = 0;
    if (!G8(S_HAVE_REPLAY)) return 0;
    if (!ccall<uint8_t>(F_ScanDown, (uint32_t)0xfd)) {
        if (ccall<uint8_t>(F_ScanDown, (uint32_t)0xcb)) cb0(F_rewind_cb);
        else if (ccall<uint8_t>(F_ScanDown, (uint32_t)0xcd)) cb0(F_ff_cb);
    }
    ccall<void>(F_update_shuttle);
    ccall<void>(F_move_blimp);
    if (!ccall<uint8_t>(F_PhysReplayIsPaused)) {
        ccall<void>(F_SoundUnMuteCars);
        G32(S_FRAME) = G32(S_FRAME) + 1;
        if (!(G8(S_FRAME) & 1)) G32(S_FRAME8) = G32(S_FRAME8) + 1;
        ccall<void>(F_UIEnableGroup, GU32(S_GROUP_PLAYING));
        ccall<void>(F_UIDisableGroup, GU32(S_GROUP_PAUSED));
    } else {
        ccall<void>(F_UIEnableGroup, GU32(S_GROUP_PAUSED));
        ccall<void>(F_UIDisableGroup, GU32(S_GROUP_PLAYING));
        ccall<void>(F_SoundMuteCars);
    }
    once_xl(S_ONCE_IDLE, 1, 0x00505380, 0x004e4994, 0x004075f0);
    const uint8_t* info = ccall<const uint8_t*>(F_CarMgrGetInfo, ccall<int32_t>(F_WorldGetFocusCar));
    {
        const volatile float* m = *(const volatile float* const volatile*)(info + 0x134);
        const double vx = D(m[0x6c / 4]), vz = D(m[0x74 / 4]), vy = D(m[0x70 / 4]);
        const double s = (vx * vx + vz * vz) + vy * vy;
        static const float k_225 = 2.25f;                        // 0x40100000 (0x4db0c4)
        GF(S_SPEED) = (float)(x87_sqrt(s) * D(k_225));
    }
    G32(S_GEAR) = *(const volatile int32_t*)(*(const uint8_t* const volatile*)(info + 0x134) + 0x34);
    ccall<char*>(F_strncpy, (char*)(uintptr_t)S_CAR_NAME, (const char*)(info + 5), 0x1f);
    G8(S_CAR_NAME + 0x1f) = 0;
    crt_strcpy((char*)(uintptr_t)S_CAMERA_NAME, ccall<const char*>(F_GetCameraName, G32(S_CAMERA)));
    if (G32(S_FRAME8) >= 8) G32(S_FRAME8) = 0;
    GF(S_ELAPSED) = call_f(F_WorldGetElapsedTime);
    {
        const float t = call_f(F_WorldGetElapsedTime);
        const char* s = ccall<const char*>(F_PhysicsTimeString, f_bits(t), (uint32_t)0);
        ((Sprintf_)(uintptr_t)F_sprintf)((char*)(uintptr_t)S_TIME_TEXT, CP(0x004e49a0), s);
    }
    if (char* p = ccall<char*>(F_strrchr, (char*)(uintptr_t)S_TIME_TEXT, 0x2e)) *(volatile char*)p = 0;
    return 0;
}
static void fp_replay_idle(Footprint& f, int32_t*) {
    if (G8(S_AUTOLOAD)) { f.replay_only = "loads the replay it was handed (load_cb)"; return; }
    if (G8(S_HAVE_REPLAY)) { f.replay_only = "drives the replay player (update_shuttle), the sound, the blimp"; return; }
    UI_FP(f, S_REPLAY_FILE, 1, "the replay file's name");
}
PORT_FN(0x00407330, "replay_idle", replay_idle_n, fp_replay_idle)

// ---- update_shuttle (0x407540): a shuttle button held: the replay paused and moved, faster the longer it's held --------------
static void __cdecl update_shuttle_n() {
    int32_t last;
    if (G32(S_SHUTTLE) != 0) {
        ccall<void>(F_WorldFXDisable);
        ccall<void>(F_PhysReplayPause);
        int32_t dir = G32(S_SHUTTLE);
        if (G32(S_SHUTTLE_LAST) == dir) {
            static const float k4 = 4.0f, k10 = 10.0f;
            const double t = D(ccall<float>(F_UIDeltaT)) * D(k4) + D(GF(S_SHUTTLE_SPEED));
            GF(S_SHUTTLE_SPEED) = (float)(D(k10) > t ? t : D(k10));   // fcom; test ah, 0x41: a NaN keeps 10
            dir = G32(S_SHUTTLE);
        } else {
            G32(S_SHUTTLE_SPEED) = 0x3f800000;
        }
        const double dt = D(ccall<float>(F_UIDeltaT));
        const float s = (float)((dt * D(dir)) * D(GF(S_SHUTTLE_SPEED)));
        ccall<void>(F_PhysReplayShuttle, f_bits(s));
        last = G32(S_SHUTTLE);
    } else {
        G32(S_SHUTTLE_SPEED) = 0x3f800000;
        last = 0;
    }
    G32(S_SHUTTLE_LAST) = last;
    G32(S_SHUTTLE) = 0;
}
static void fp_update_shuttle(Footprint& f) {
    if (G32(S_SHUTTLE) != 0) { f.replay_only = "drives the replay player (PhysReplayPause / Shuttle)"; return; }
    UI_FP(f, S_SHUTTLE_SPEED, 4, "shuttle speed");
    UI_FP(f, S_SHUTTLE_LAST, 4, "shuttle last");
    UI_FP(f, S_SHUTTLE, 4, "shuttle");
}
PORT_FN(0x00407540, "update_shuttle", update_shuttle_n, fp_update_shuttle)

// =========================================================================================================================
// fullscreen_cb (0x407600): the replay full screen, its own frame loop, until a click or Escape
// =========================================================================================================================
static uint8_t __cdecl fullscreen_cb_n(int32_t) {
    if (!G8(S_HAVE_REPLAY)) return 0;
    for (;;) {
        ccall<void>(F_move_blimp);
        if (ccall<uint8_t>(F_MousePeekEvent)) {
            int32_t ev[4];
            if (ccall<uint8_t>(F_MouseGetEvent, ev)) {
                const int32_t t = ev[0];
                if (t == 0 || t == 2 || t == 4) { set_view(); return 0; }
            }
        }
        if (ccall<uint8_t>(F_KeyHit)) {
            const uint32_t k = ccall<uint16_t>(F_KeyGet);
            const uint8_t have = G8(S_HAVE_REPLAY);
            if (k == 0x1b) { set_view(); return 0; }
            if (k == 0x70 || k == 0x128) {
                if (ccall<uint8_t>(F_PhysReplayIsPaused) && !ccall<uint8_t>(F_PhysReplayIsAtEnd)) {
                    ccall<void>(F_WorldFXEnable);
                    ccall<void>(F_PhysReplayResume);
                } else {
                    ccall<void>(F_PhysReplayPause);
                }
            } else if (k == 0x121) {
                if (have) ccall<void>(F_WorldNextFocusCar);
            } else if (k == 0x122) {
                if (have) ccall<void>(F_WorldPrevFocusCar);
            } else if (k == 0x124) {
                if (have) ccall<void>(F_WorldSetFocusCar, ccall<int32_t>(F_WorldGetPlayerCar));
            } else if (k == 0x325) {
                cb0(F_rewind_to_start_cb);
            } else if (k == 0x327) {
                cb0(F_ff_to_end_cb);
            } else if (k > 0x128 && k < 0x325 && k - 0x170u <= 0xbu) {
                cb0(F_CAMERA_IN_CAR_cb + 0x10u * (k - 0x170u));     // the twelve cameras' callbacks, 0x10 apart
            }
        }
        if (ccall<uint8_t>(F_ScanDown, (uint32_t)0xcb)) cb0(F_rewind_cb);
        else if (ccall<uint8_t>(F_ScanDown, (uint32_t)0xcd)) cb0(F_ff_cb);
        ccall<void>(F_update_shuttle);
        if (!ccall<uint8_t>(F_PhysReplayIsPaused)) ccall<void>(F_SoundUnMuteCars);
        else ccall<void>(F_SoundMuteCars);
        set_view();
        ccall<void>(F_mrBeginFrame);
        ccall<void>(F_WorldUpdate);
        ccall<void>(F_CameraSetType, G32(S_CAMERA));
        ccall<void>(F_WorldDraw, (uint32_t)1);
        ccall<void>(F_mrEndFrame);
        ccall<void>(F_UIUpdateTime_);
        ccall<void>(F_gxFlip);
    }
}
PORT_FN(0x00407600, "fullscreen_cb", fullscreen_cb_n, fp_modal_i)

// ---- ReplayLoadDialog (0x4078d0): the open box, into the replay file's name ---------------------------------------------------
static uint8_t __cdecl ReplayLoadDialog_n() {
    once_xl(S_ONCE_LOADDLG, 1, 0x00505460, 0x004e49a4, 0x004079c0);
    once_xl(S_ONCE_LOADDLG, 2, 0x00505450, 0x004e49bc, 0x004079b0);
    ((Sprintf_)(uintptr_t)F_sprintf)((char*)(uintptr_t)S_REPLAY_DIR, CP(0x004e49d8), ccall<const char*>(F_Win32GetUserDirectory));
    G8(S_REPLAY_FILE) = 0;
    xl(0x00505450);
    xl(0x00505460);
    const char* label = GP(const char, 0x00505454);
    const char* title = GP(const char, 0x00505464);
    if (ccall<uint8_t>(F_UIDoOpenFileBox, title, label, CP(0x004e49e4), (char*)(uintptr_t)S_REPLAY_FILE, 0x104, CP(S_REPLAY_DIR)))
        return 1;
    G8(S_REPLAY_FILE) = 0;
    return 0;
}
PORT_FN(0x004078d0, "ReplayLoadDialog", ReplayLoadDialog_n, fp_modal0)

// ---- load_cb (0x4079d0): a replay file loaded (the one handed over, else the open box's): its world, its cars, its data --------
static uint8_t __cdecl load_cb_n(int32_t) {
    int32_t fd;
    char track[0x40];
    char name[0x104];
    if (G8(S_FROM_RACE)) {
        UI_LogPanic(CP(0x004e49ec));                                  // "Can't press load button during game!"
        return 0;
    }
    name[0] = (char)G8(0x004e4a14);
    for (int i = 1; i < 0x104; i++) ((volatile char*)name)[i] = 0;
    if (G8(S_HAVE_REPLAY)) {
        ccall<void>(F_PhysReplayPause);
        ccall<void>(F_SoundMuteCars);
    }
    once_xl(S_ONCE_LOAD, 0x01, 0x00505588, 0x004e4a18, 0x00407ec0);
    once_xl(S_ONCE_LOAD, 0x02, 0x00505428, 0x004e4a30, 0x00407eb0);
    once_xl(S_ONCE_LOAD, 0x04, 0x00505670, 0x004e4a4c, 0x00407ea0);
    once_xl(S_ONCE_LOAD, 0x08, 0x005053e0, 0x004e4a6c, 0x00407e90);
    once_xl(S_ONCE_LOAD, 0x10, 0x00505690, 0x004e4aa4, 0x00407e80);
    once_xl(S_ONCE_LOAD, 0x20, 0x005055a8, 0x004e4adc, 0x00407e70);
    if (G8(S_REPLAY_FILE) != 0) crt_strcpy(name, CP(S_REPLAY_FILE));
    if (G8(S_REPLAY_FILE) == 0) {
        xl(0x00505428);
        xl(0x00505588);
        const char* label = GP(const char, 0x0050542c);
        const char* title = GP(const char, 0x0050558c);
        if (!ccall<uint8_t>(F_UIDoOpenFileBox, title, label, CP(0x004e4b08), name, 0x104, CP(S_REPLAY_DIR))) {
            UI_LogReport(CP(0x004e4b10));                             // "cancel open file box"
            return 0;
        }
    }
    fd = ccall<int32_t>(F_FileOpen, (const char*)name);
    if (!fd) {
        xl(0x005055a8);
        xl(0x00505670);
        ccall<void>(F_UIDoOkBox, GP(const char, 0x00505674), GP(const char, 0x005055ac));
        return 0;
    }
    ccall<void>(F_SplashLoading);
    if (G8(S_HAVE_REPLAY)) {
        ccall<void>(F_PhysReplayPlayEnd);
        ccall<void>(F_WorldEndReplay);
        ccall<void>(F_TelemetryEnd);
        track_res(track, REPLAY_TRACK, 0x004e4b28);
        ccall<void>(F_ResourceSetUnload, (const char*)track);
        ccall<void>(F_UnloadCars, (const void*)(uintptr_t)S_REPLAY_WORLD);
        G8(S_HAVE_REPLAY) = 0;
    }
    if (ccall<uint8_t>(F_FileReadExact, fd, (void*)(uintptr_t)S_REPLAY_WORLD, 0xcd4)) {
        const char* text;
        if (tcall<uint8_t>(F_World_is_valid_version, (const void*)(uintptr_t)S_REPLAY_WORLD)) {
            ccall<void>(F_TelemetryBegin);
            track_res(track, REPLAY_TRACK, 0x004e4b30);
            ccall<void>(F_ResourceSetMustLoad, (const char*)track);
            ccall<void>(F_LoadCars, (void*)(uintptr_t)S_REPLAY_WORLD);
            ccall<void>(F_WorldBeginReplay, (void*)(uintptr_t)S_REPLAY_WORLD);
            ccall<void>(F_PhysReplayPlayBegin);
            G8(S_HAVE_REPLAY) = 1;
            if (ccall<uint8_t>(F_PhysReplayLoad, fd)) goto close;
            UI_LogReport(CP(0x004e4b38));                             // "Couldn't load replay"
            ccall<void>(F_PhysReplayPlayEnd);
            ccall<void>(F_UnloadCars, (const void*)(uintptr_t)S_REPLAY_WORLD);
            ccall<void>(F_WorldEndReplay);
            ccall<void>(F_TelemetryEnd);
            G8(S_HAVE_REPLAY) = 0;
            xl(0x005053e0);
            xl(0x00505670);
            text = GP(const char, 0x005053e4);
        } else {
            xl(0x00505690);
            xl(0x00505670);
            text = GP(const char, 0x00505694);
        }
        ccall<void>(F_UIDoOkBox, GP(const char, 0x00505674), text);
    }
close:
    ccall<void>(F_FileClose, &fd);
    ccall<void>(F_mrModelFlush);
    set_view();
    return 0;
}
PORT_FN(0x004079d0, "load_cb", load_cb_n, fp_modal_i)

// ---- save_cb (0x407ed0): the save box ("replay.rpl"), the world and the replay's data written --------------------------------
static uint8_t __cdecl save_cb_n(int32_t) {
    int32_t fd;
    char name[0x104];
    if (!G8(S_HAVE_REPLAY)) return 0;
    ccall<void>(F_PhysReplayPause);
    ccall<void>(F_create_replay_dir);
    once_xl(S_ONCE_SAVE, 1, 0x00505658, 0x004e4b50, 0x00408120);
    once_xl(S_ONCE_SAVE, 2, 0x005053c0, 0x004e4b68, 0x00408110);
    once_xl(S_ONCE_SAVE, 4, 0x00504698, 0x004e4b84, 0x00408100);
    once_xl(S_ONCE_SAVE, 8, 0x00504660, 0x004e4ba4, 0x004080f0);
    {                                                             // "replay.rpl" (0xb bytes), the rest zeroed
        volatile uint8_t* d = (volatile uint8_t*)name;
        *(volatile uint32_t*)(d + 0) = GU32(0x004e4bcc);
        *(volatile uint32_t*)(d + 4) = GU32(0x004e4bd0);
        *(volatile uint16_t*)(d + 8) = UI_G16(0x004e4bd4);
        d[0xa] = G8(0x004e4bd6);
        for (int i = 0xb; i < 0x104; i++) d[i] = 0;
    }
    xl(0x005053c0);
    xl(0x00505658);
    const char* label = GP(const char, 0x005053c4);
    const char* title = GP(const char, 0x0050565c);
    if (!ccall<uint8_t>(F_UIDoSaveFileBox, title, label, CP(0x004e4bd8), name, 0x104, CP(S_REPLAY_DIR))) return 0;
    fd = ccall<int32_t>(F_FileCreate, (const char*)name);
    if (fd) {
        ccall<uint8_t>(F_FileWrite, fd, (const void*)(uintptr_t)S_REPLAY_WORLD, 0xcd4);
        ccall<void>(F_PhysReplaySave, fd);
        ccall<void>(F_FileClose, &fd);
        return 0;
    }
    xl(0x00504660);
    xl(0x00504698);
    ccall<void>(F_UIDoOkBox, GP(const char, 0x0050469c), GP(const char, 0x00504664));
    return 0;
}
PORT_FN(0x00407ed0, "save_cb", save_cb_n, fp_modal_i)

// ---- create_replay_dir (0x4080e0) ------------------------------------------------------------------------------------------------
static void __cdecl create_replay_dir_n() { ccall<uint8_t>(F_FileCreateDirectory, CP(S_REPLAY_DIR)); }
static void fp_create_dir(Footprint& f) { f.replay_only = "creates a directory"; }
PORT_FN(0x004080e0, "create_replay_dir", create_replay_dir_n, fp_create_dir)

// =========================================================================================================================
// the transport, camera and car callbacks
// =========================================================================================================================
static uint8_t __cdecl play_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) {
        if (ccall<uint8_t>(F_PhysReplayIsPaused) && !ccall<uint8_t>(F_PhysReplayIsAtEnd)) {
            ccall<void>(F_WorldFXEnable);
            ccall<void>(F_PhysReplayResume);
        }
        ccall<void>(F_UIDisableGroup, GU32(S_GROUP_PAUSED));
    }
    return 0;
}
PORT_FN(0x00408130, "play_cb", play_cb_n, fp_cb_player)
static uint8_t __cdecl rewind_to_start_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) {
        ccall<void>(F_WorldFXDisable);
        ccall<void>(F_PhysReplayRewind);
        ccall<void>(F_PhysReplayPause);
    }
    return 0;
}
PORT_FN(0x00408170, "rewind_to_start_cb", rewind_to_start_cb_n, fp_cb_player)
static uint8_t __cdecl rewind_cb_n(int32_t) { G32(S_SHUTTLE) = -1; return 0; }
PORT_FN(0x00408190, "rewind_cb", rewind_cb_n, fp_cb_shuttle)
static uint8_t __cdecl ff_cb_n(int32_t) { G32(S_SHUTTLE) = 1; return 0; }
PORT_FN(0x004081a0, "ff_cb", ff_cb_n, fp_cb_shuttle)
static uint8_t __cdecl ff_to_end_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) {
        ccall<void>(F_WorldFXDisable);
        ccall<void>(F_PhysReplayFastForward);
        ccall<void>(F_PhysReplayPause);
    }
    return 0;
}
PORT_FN(0x004081b0, "ff_to_end_cb", ff_to_end_cb_n, fp_cb_player)
static uint8_t __cdecl stop_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) ccall<void>(F_PhysReplayPause);
    return 0;
}
PORT_FN(0x004081d0, "stop_cb", stop_cb_n, fp_cb_player)
static uint8_t __cdecl car_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) ccall<void>(F_WorldNextFocusCar);
    return 0;
}
PORT_FN(0x004081f0, "car_cb", car_cb_n, fp_cb_focus)

// the twelve cameras' callbacks (0 in car .. 11 blimp)
static uint8_t __cdecl CAMERA_IN_CAR_cb_n(int32_t) { G32(S_CAMERA) = 0; return 0; }
PORT_FN(0x00408210, "CAMERA_IN_CAR_cb", CAMERA_IN_CAR_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_BUMPER_cb_n(int32_t) { G32(S_CAMERA) = 1; return 0; }
PORT_FN(0x00408220, "CAMERA_BUMPER_cb", CAMERA_BUMPER_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_XRAY_cb_n(int32_t) { G32(S_CAMERA) = 2; return 0; }
PORT_FN(0x00408230, "CAMERA_XRAY_cb", CAMERA_XRAY_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_REAR_cb_n(int32_t) { G32(S_CAMERA) = 3; return 0; }
PORT_FN(0x00408240, "CAMERA_REAR_cb", CAMERA_REAR_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_NEAR_CHASE_cb_n(int32_t) { G32(S_CAMERA) = 4; return 0; }
PORT_FN(0x00408250, "CAMERA_NEAR_CHASE_cb", CAMERA_NEAR_CHASE_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_CHASE_cb_n(int32_t) { G32(S_CAMERA) = 5; return 0; }
PORT_FN(0x00408260, "CAMERA_CHASE_cb", CAMERA_CHASE_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_FAR_CHASE_cb_n(int32_t) { G32(S_CAMERA) = 6; return 0; }
PORT_FN(0x00408270, "CAMERA_FAR_CHASE_cb", CAMERA_FAR_CHASE_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_REAR_CHASE_cb_n(int32_t) { G32(S_CAMERA) = 7; return 0; }
PORT_FN(0x00408280, "CAMERA_REAR_CHASE_cb", CAMERA_REAR_CHASE_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_OVERHEAD_cb_n(int32_t) { G32(S_CAMERA) = 8; return 0; }
PORT_FN(0x00408290, "CAMERA_OVERHEAD_cb", CAMERA_OVERHEAD_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_AERIAL_cb_n(int32_t) { G32(S_CAMERA) = 9; return 0; }
PORT_FN(0x004082a0, "CAMERA_AERIAL_cb", CAMERA_AERIAL_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_TV_cb_n(int32_t) { G32(S_CAMERA) = 10; return 0; }
PORT_FN(0x004082b0, "CAMERA_TV_cb", CAMERA_TV_cb_n, fp_cb_camera)
static uint8_t __cdecl CAMERA_BLIMP_cb_n(int32_t) { G32(S_CAMERA) = 11; return 0; }
PORT_FN(0x004082c0, "CAMERA_BLIMP_cb", CAMERA_BLIMP_cb_n, fp_cb_camera)

// ---- ReplayView::Draw3D (0x4082d0): the world behind the dialog --------------------------------------------------------------
static void __fastcall ReplayView_Draw3D_n(ReplayView*, Edx) {
    set_view();
    if (!G8(S_HAVE_REPLAY)) return;
    ccall<void>(F_WorldUpdate);
    ccall<void>(F_CameraSetType, G32(S_CAMERA));
    ccall<void>(F_WorldDraw, (uint32_t)1);
}
static void fp_view_draw3d(Footprint& f, ReplayView*, Edx) { f.replay_only = "sets the 3D view, updates and draws the world"; }
PORT_FN(0x004082d0, "ReplayView::Draw3D", ReplayView_Draw3D_n, fp_view_draw3d)

static uint8_t __cdecl next_view_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) {
        const int32_t c = G32(S_CAMERA) + 1;
        G32(S_CAMERA) = c;
        if (c > 0xa) G32(S_CAMERA) = 0;
    }
    return 0;
}
PORT_FN(0x00408320, "next_view_cb", next_view_cb_n, fp_cb_camera)
static uint8_t __cdecl prev_view_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) {
        const uint32_t c = GU32(S_CAMERA) - 1u;                  // dec; jns: the sign of the result
        GU32(S_CAMERA) = c;
        if ((int32_t)c < 0) G32(S_CAMERA) = 0xa;
    }
    return 0;
}
PORT_FN(0x00408350, "prev_view_cb", prev_view_cb_n, fp_cb_camera)
static uint8_t __cdecl player_car_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) ccall<void>(F_WorldSetFocusCar, ccall<int32_t>(F_WorldGetPlayerCar));
    return 0;
}
PORT_FN(0x00408370, "player_car_cb", player_car_cb_n, fp_cb_focus)
static uint8_t __cdecl next_car_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) ccall<void>(F_WorldNextFocusCar);
    return 0;
}
PORT_FN(0x00408390, "next_car_cb", next_car_cb_n, fp_cb_focus)
static uint8_t __cdecl prev_car_cb_n(int32_t) {
    if (G8(S_HAVE_REPLAY)) ccall<void>(F_WorldPrevFocusCar);
    return 0;
}
PORT_FN(0x004083b0, "prev_car_cb", prev_car_cb_n, fp_cb_focus)

// =========================================================================================================================
// do_analysis (0x4083d0): the analysis screen; 0 when its exit button (0x42) ends the replay screen too
// =========================================================================================================================
static uint8_t __cdecl do_analysis_n() {
    alignas(4) uint8_t fr[0x61c];                                 // the original's frame, from its bottom (+0x628 its top)
#define P(o) ((void*)(fr + (o) - 0xc))
#define PD(o) (*(volatile uint32_t*)(fr + (o) - 0xc))
    if (!G8(S_HAVE_REPLAY)) return 0;
    ReplayView* view = (ReplayView*)P(0x20);
    GraphControl* graph = (GraphControl*)P(0x60);
    view->widget = 0;
    view->jog.widget = 0;
    view->vtbl = (const void*)(uintptr_t)VT_UICustomControl;     // the view's constructor and its JogControl's, inline
    view->jog.vtbl = (const void*)(uintptr_t)VT_JogControl;
    view->jog.wheel = ccall<void*>(F_gxGetStamp, CP(0x004e4dc8));
    view->jog.detent = ccall<void*>(F_gxGetStamp, CP(0x004e4dbc));
    view->jog._24 = 0;
    view->jog.dragging = 0;
    graph->widget = 0;
    view->vtbl = (const void*)(uintptr_t)VT_ReplayView;
    graph->vtbl = (const void*)(uintptr_t)VT_GraphControl;
    G32(S_GRAPH_ZOOM) = 0x40a00000;                               // 5
    G32(S_GRAPH_SCALE) = 0x3f800000;                              // 1
    G32(S_GRAPH_TYPE) = 2;
    once_xl(S_ONCE_ANALYSIS, 0x01, 0x00505648, 0x004e4be0, 0x00408c70);
    once_xl(S_ONCE_ANALYSIS, 0x02, 0x005057f0, 0x004e4bf4, 0x00408c60);
    once_xl(S_ONCE_ANALYSIS, 0x04, 0x00504680, 0x004e4c08, 0x00408c50);
    once_xl(S_ONCE_ANALYSIS, 0x08, 0x00505410, 0x004e4c20, 0x00408c40);
    once_xl(S_ONCE_ANALYSIS, 0x10, 0x00504670, 0x004e4c34, 0x00408c30);
    once_xl(S_ONCE_ANALYSIS, 0x20, 0x005057d8, 0x004e4c48, 0x00408c20);
    once_xl(S_ONCE_ANALYSIS, 0x40, 0x00505578, 0x004e4c5c, 0x00408c10);
    once_xl(S_ONCE_ANALYSIS, 0x80, 0x00505628, 0x004e4c74, 0x00408c00);
    uint32_t o = 0x78;
#define IT(...) (ictor(P(o), __VA_ARGS__), o += 0x38)
    IT(0x17, 0, 0x7a, 0x88, 0x114, 0x66, 0x004e4db8, 0, U(view), 0);
    IT(1, 0, 0, 0, 0, 0, 0, 0, S_GROUP_ANALYSIS, 0);
    IT(1, 1, 0, 0, 0, 0, 0, 0, S_GROUP_ANALYSIS, 0);
    IT(1, 0, 0, 0, 0, 0, 0, 0, S_GROUP_ANALYSIS, 0);
    IT(2, 0, 0x181, 6, 0, 0, U(xlt(0x00504670)), 0, F_load_cb, 4);
    IT(1, 1, 0, 0, 0, 0, 0, 0, S_GROUP_ANALYSIS, 0);
    IT(1, 0, 0, 0, 0, 0, 0, 0, S_GROUP_MAIN, 0);
    IT(2, 0, 0x181, 6, 0, 0, U(xlt(0x005057d8)), 0, F_save_cb, 4);
    IT(1, 1, 0, 0, 0, 0, 0, 0, S_GROUP_MAIN, 0);
    IT(2, 0, 0x181, 0x1c, 0, 0, U(xlt(0x00505578)), 0, F_fullscreen_cb, 4);
    IT(2, 0xd, 0x181, 0x32, 0, 0, U(xlt(0x00505628)), 0, 0, 4);
    IT(2, 0x42, 0x181, 0x48, 0, 0, U(xlt(0x00505410)), 0, 0, 4);
    IT(4, 0xfffffffeu, 0x1b, 0, 0, 0, 0, 0, 0, 0);
    IT(0xc, 0, 0x12c, 0x73, 0, 0, U(xlt(0x005057f0)), 2, S_GRAPH_TYPE, 8);          // lateral
    IT(0xc, 0, 0x12c, 0x82, 0, 0, U(xlt(0x00505648)), 1, S_GRAPH_TYPE, 8);          // longitudinal
    IT(0xc, 0, 0x12c, 0x91, 0, 0, U(xlt(0x00504680)), 3, S_GRAPH_TYPE, 8);          // speed
    IT(6, 0, 0x1a8, 0x73, 0, 0, 0x004e4c8c, 0, S_GRAPH_ZOOM, 0x15, 0x3f800000);     // "%1.1f"
    IT(0x10, 0, 0x196, 0x73, 0, 0, 0x004e4db8, 0x14, S_GRAPH_ZOOM, 0, 0x3f000000, 0x41200000);
    IT(0x10, 0, 0x1d5, 0x73, 0, 0, 0x004e4db8, 0x14, S_GRAPH_ZOOM, 1, 0x3f000000, 0x41200000);
    IT(6, 0, 0x1a8, 0x8c, 0, 0, 0x004e4c94, 0, S_GRAPH_SCALE, 0x15, 0x42c80000);    // "%1.0f"
    IT(0x10, 0, 0x196, 0x8c, 0, 0, 0x004e4db8, 5, S_GRAPH_SCALE, 0, 0x3f800000, 0x40400000);
    IT(0x10, 0, 0x1d5, 0x8c, 0, 0, 0x004e4db8, 5, S_GRAPH_SCALE, 1, 0x3f800000, 0x40400000);
    IT(5, 0, 0x12c, 0xaa, 0, 0, S_TIME_TEXT, 0, 0, 0x15);
    IT(6, 0, 0x17c, 0xaa, 0, 0, 0x004e4c9c, 0, S_SPEED, 0x15, 0x3f800000);           // "%1.0f"
    IT(0x17, 0, 0x10, 0x6e, 0x104, 0x52, 0x004e4db8, 0, U(graph), 0);
#undef IT
    item_end(P(0x5f0));
    PD(0xc) = 0x004e4ca4;
    PD(0x10) = 0x004e4ca8;
    PD(0x14) = 0xfffffffeu;
    PD(0x1c) = F_replay_idle;
    PD(0x18) = U(P(0x78));
    ccall<void>(F_SoundUnMuteCars);
    const int32_t r = ccall<int32_t>(F_UIDoDialog_, P(0xc), 0x1f4, 0xc8, -999, 0x14, 2);
    ccall<void>(F_SoundMuteCars);
    graph->vtbl = (const void*)(uintptr_t)VT_UICustomControl;    // the destructors, inline
    view->jog.vtbl = (const void*)(uintptr_t)VT_JogControl;
    ccall<void>(F_gxForgetStamp, (void*)view->jog.detent);
    ccall<void>(F_gxForgetStamp, (void*)view->jog.wheel);
    return (uint32_t)r - 0x42u != 0 ? 1 : 0;
#undef P
#undef PD
}
PORT_FN(0x004083d0, "do_analysis", do_analysis_n, fp_modal0)

// ---- auto_load_replay (0x408c80): a replay loaded from the file named (no box); whether it is -------------------------------
static uint8_t __cdecl auto_load_replay_n(const char* file) {
    int32_t fd;
    char track[0x38];
    G8(S_HAVE_REPLAY) = 0;
    once_xl(S_ONCE_AUTOLOAD, 1, 0x00505598, 0x004e4cb8, 0x00408f80);
    once_xl(S_ONCE_AUTOLOAD, 2, 0x00505618, 0x004e4cd8, 0x00408f70);
    once_xl(S_ONCE_AUTOLOAD, 4, 0x00505638, 0x004e4d10, 0x00408f60);
    once_xl(S_ONCE_AUTOLOAD, 8, 0x005057c8, 0x004e4d48, 0x00408f50);
    fd = ccall<int32_t>(F_FileOpen, file);
    if (!fd) {
        xl(0x005057c8);
        xl(0x00505598);
        ccall<void>(F_UIDoOkBox, GP(const char, 0x0050559c), GP(const char, 0x005057cc));
        return G8(S_HAVE_REPLAY);
    }
    ccall<void>(F_SplashLoading);
    if (ccall<uint8_t>(F_FileReadExact, fd, (void*)(uintptr_t)S_REPLAY_WORLD, 0xcd4)) {
        const char* text;
        if (tcall<uint8_t>(F_World_is_valid_version, (const void*)(uintptr_t)S_REPLAY_WORLD)) {
            ccall<void>(F_TelemetryBegin);
            track_res(track, REPLAY_TRACK, 0x004e4d74);
            ccall<void>(F_ResourceSetMustLoad, (const char*)track);
            ccall<void>(F_LoadCars, (void*)(uintptr_t)S_REPLAY_WORLD);
            ccall<void>(F_WorldBeginReplay, (void*)(uintptr_t)S_REPLAY_WORLD);
            ccall<void>(F_PhysReplayPlayBegin);
            G8(S_HAVE_REPLAY) = 1;
            if (ccall<uint8_t>(F_PhysReplayLoad, fd)) {
                G8(S_HAVE_REPLAY) = 1;
                goto close;
            }
            UI_LogReport(CP(0x004e4d7c));                             // "Couldn't load replay"
            ccall<void>(F_PhysReplayPlayEnd);
            ccall<void>(F_WorldEndReplay);
            ccall<void>(F_TelemetryEnd);
            G8(S_HAVE_REPLAY) = 0;
            xl(0x00505618);
            xl(0x00505598);
            text = GP(const char, 0x0050561c);
        } else {
            xl(0x00505638);
            xl(0x00505598);
            text = GP(const char, 0x0050563c);
        }
        ccall<void>(F_UIDoOkBox, GP(const char, 0x0050559c), text);
    }
close:
    ccall<void>(F_FileClose, &fd);
    ccall<void>(F_mrModelFlush);
    set_view();
    return G8(S_HAVE_REPLAY);
}
static void fp_auto_load(Footprint& f, const char*) { f.replay_only = "loads a replay file (and may show a box)"; }
PORT_FN(0x00408c80, "auto_load_replay", auto_load_replay_n, fp_auto_load)

// ---- ReplayBenchmark (0x408f90): a replay played through as fast as it draws; frames per second -----------------------------
static float __cdecl ReplayBenchmark_n(const char* file) {
    char track[0x30];
    volatile float result;
    G8(S_HAVE_REPLAY) = 0;
    G8(S_FROM_RACE) = 0;
    G8(S_AUTOLOAD) = 0;
    ccall<void>(F_WorldSwitchToDrive);
    ((Sprintf_)(uintptr_t)F_sprintf)((char*)(uintptr_t)S_REPLAY_FILE, CP(0x004e4d94), file);
    int32_t t0 = 0, t1 = 0;
    ccall<void>(F_ResourceSetMustLoad, CP(0x004e4d98));
    G32(S_CAMERA) = 5;
    cb0(F_load_cb);
    ccall<void>(F_CameraSetType, G32(S_CAMERA));
    if (G8(S_HAVE_REPLAY)) {
        ccall<void>(F_WorldFXDisable);
        ccall<void>(F_SoundMuteCars);
        if (ccall<uint8_t>(F_PhysReplayIsPaused)) {
            ccall<void>(F_WorldFXEnable);
            ccall<void>(F_PhysReplayResume);
        }
        ccall<void>(F_TaskSleep, 0x32);
        ccall<void>(F_ProfReset);
        t0 = ccall<int32_t>(F_PTimeNow);
        G32(S_BENCH_FRAMES) = 0;
        ccall<void>(F_benchmark_loop);
        t1 = ccall<int32_t>(F_PTimeNow);
        ccall<void>(F_ProfResetAndReport);
        ccall<void>(F_SoundMuteCars);
        ccall<void>(F_SplashLoading);
        ccall<void>(F_PhysReplayPlayEnd);
        ccall<void>(F_WorldEndReplay);
        ccall<void>(F_TelemetryEnd);
        track_res(track, REPLAY_TRACK, 0x004e4da4);
        ccall<void>(F_ResourceSetUnload, (const char*)track);
        ccall<void>(F_WorldSwitchToUI);
        G8(S_REPLAY_FILE) = 0;
        ccall<void>(F_UnloadCars, (const void*)(uintptr_t)S_REPLAY_WORLD);
    }
    ccall<void>(F_ResourceSetUnload, CP(0x004e4dac));
    *(volatile uint32_t*)&result = 0;
    if (t1 > t0) {
        static const float k1000 = 1000.0f;                      // 0x447a0000 (0x4db0ec)
        const int32_t dt = (int32_t)((uint32_t)t1 - (uint32_t)t0);
        result = (float)((D(G32(S_BENCH_FRAMES)) / D(dt)) * D(k1000));
    }
    return result;
}
static void fp_bench(Footprint& f, const char*) { f.replay_only = "loads and plays a replay (its own frame loop)"; }
PORT_FN(0x00408f90, "ReplayBenchmark", ReplayBenchmark_n, fp_bench)

// ---- benchmark_loop (0x409130): frames until the replay pauses (at its end) or Escape ---------------------------------------
static void __cdecl benchmark_loop_n() {
    if (!G8(S_HAVE_REPLAY)) return;
    do {
        if (ccall<uint8_t>(F_KeyHit) && ccall<uint16_t>(F_KeyGet) == 0x1b) return;
        if (!ccall<uint8_t>(F_PhysReplayIsPaused)) ccall<void>(F_SoundUnMuteCars);
        else ccall<void>(F_SoundMuteCars);
        const int32_t w = G32(S_W), h = G32(S_H);
        G32(S_BENCH_FRAMES) = G32(S_BENCH_FRAMES) + 1;
        ccall<void>(F_mrSetView, 0, 0, w, h, (uint32_t)1);
        ccall<void>(F_mrBeginFrame);
        ccall<void>(F_WorldUpdate);
        ccall<void>(F_WorldDraw, (uint32_t)1);
        ccall<void>(F_mrEndFrame);
        ccall<void>(F_UIUpdateTime_);
        ccall<void>(F_gxFlip);
    } while (!ccall<uint8_t>(F_PhysReplayIsPaused));
}
PORT_FN(0x00409130, "benchmark_loop", benchmark_loop_n, fp_modal0)

// =========================================================================================================================
// UIDialogItem::UIDialogItem (0x4091c0): its 14 arguments, stored
// =========================================================================================================================
static void* __fastcall UIDialogItem_ctor_n(void* self, Edx, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                                           uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10,
                                           uint32_t a11, uint32_t a12, uint32_t a13) {
    volatile uint32_t* d = (volatile uint32_t*)self;
    d[0] = a0; d[1] = a1; d[2] = a2; d[3] = a3; d[4] = a4; d[5] = a5; d[6] = a6;
    d[7] = a7; d[8] = a8; d[9] = a9; d[10] = a10; d[11] = a11; d[12] = a12; d[13] = a13;
    return self;
}
static void fp_item_ctor(Footprint& f, void* self, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                         uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    f.add(self, 0x38, "the item");
}
PORT_FN(0x004091c0, "UIDialogItem::UIDialogItem", UIDialogItem_ctor_n, fp_item_ctor)

// ---- UICustomControl's empty methods (0x409230 ..) -----------------------------------------------------------------------------
static void __fastcall UICC_void0(RvCtl*, Edx) {}
static void __fastcall UICC_void0b(RvCtl*, Edx) {}
static void __fastcall UICC_void0c(RvCtl*, Edx) {}
static void __fastcall UICC_void0d(RvCtl*, Edx) {}
static void __fastcall UICC_void0e(RvCtl*, Edx) {}
static void __fastcall UICC_void0f(RvCtl*, Edx) {}
static void __fastcall UICC_void0g(RvCtl*, Edx) {}
static void __fastcall UICC_void0h(RvCtl*, Edx) {}
static void __fastcall UICC_void0i(RvCtl*, Edx) {}
static void __fastcall UICC_Callback(RvCtl*, Edx, int32_t, const void*) {}
static void __fastcall UICC_MouseDown(RvCtl*, Edx, int32_t, int32_t) {}
static void __fastcall UICC_MouseUp(RvCtl*, Edx, int32_t, int32_t) {}
static void __fastcall UICC_MouseMove(RvCtl*, Edx, int32_t, int32_t) {}
static void __fastcall UICC_MouseRDown(RvCtl*, Edx, int32_t, int32_t) {}
static void __fastcall UICC_MouseRUp(RvCtl*, Edx, int32_t, int32_t) {}
static void* __fastcall UICC_GetCursor(RvCtl*, Edx) { return 0; }
static uint8_t __fastcall UICC_CharHit(RvCtl*, Edx, uint32_t) { return 0; }
static uint8_t __fastcall UICC_IsTabStop(RvCtl*, Edx) { return 0; }
static void fp_p0(Footprint& f, RvCtl*, Edx) { f.pure = true; }
static void fp_p_cb(Footprint& f, RvCtl*, Edx, int32_t, const void*) { f.pure = true; }
static void fp_p_ii(Footprint& f, RvCtl*, Edx, int32_t, int32_t) { f.pure = true; }
static void fp_p_u(Footprint& f, RvCtl*, Edx, uint32_t) { f.pure = true; }
PORT_FN(0x00409230, "UICustomControl::Create", UICC_void0, fp_p0)
PORT_FN(0x00409240, "UICustomControl::Destroy", UICC_void0b, fp_p0)
PORT_FN(0x00409250, "UICustomControl::Added", UICC_void0c, fp_p0)
PORT_FN(0x00409260, "UICustomControl::Callback", UICC_Callback, fp_p_cb)
PORT_FN(0x00409270, "UICustomControl::Update", UICC_void0d, fp_p0)
PORT_FN(0x00409280, "UICustomControl::Draw3D", UICC_void0e, fp_p0)
PORT_FN(0x00409290, "UICustomControl::MouseDown", UICC_MouseDown, fp_p_ii)
PORT_FN(0x004092a0, "UICustomControl::MouseUp", UICC_MouseUp, fp_p_ii)
PORT_FN(0x004092b0, "UICustomControl::MouseMove", UICC_MouseMove, fp_p_ii)
PORT_FN(0x004092c0, "UICustomControl::MouseRDown", UICC_MouseRDown, fp_p_ii)
PORT_FN(0x004092d0, "UICustomControl::MouseRUp", UICC_MouseRUp, fp_p_ii)
PORT_FN(0x004092e0, "UICustomControl::Over", UICC_void0f, fp_p0)
PORT_FN(0x004092f0, "UICustomControl::NotOver", UICC_void0g, fp_p0)
PORT_FN(0x00409300, "UICustomControl::Focus", UICC_void0h, fp_p0)
PORT_FN(0x00409310, "UICustomControl::UnFocus", UICC_void0i, fp_p0)
PORT_FN(0x00409320, "UICustomControl::GetCursor", UICC_GetCursor, fp_p0)
PORT_FN(0x00409330, "UICustomControl::CharHit", UICC_CharHit, fp_p_u)
PORT_FN(0x00409340, "UICustomControl::IsTabStop", UICC_IsTabStop, fp_p0)

// ---- UICustomControl's deleting destructor (0x409350) ------------------------------------------------------------------------
static void* __fastcall UICC_delete_n(RvCtl* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void fp_UICC_delete(Footprint& f, RvCtl* self, Edx, uint32_t flags) {
    if (flags & 1) { f.replay_only = "frees the object"; return; }
    f.add(self, 4, "its vtable");
}
PORT_FN(0x00409350, "UICustomControl::`vector deleting destructor'", UICC_delete_n, fp_UICC_delete)

// =========================================================================================================================
// JogControl
// =========================================================================================================================
static JogControl* __fastcall JogControl_ctor_n(JogControl* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_JogControl;
    self->wheel = ccall<void*>(F_gxGetStamp, CP(0x004e4dc8));      // jwheel.stp
    self->detent = ccall<void*>(F_gxGetStamp, CP(0x004e4dbc));     // jdetent.stp
    self->_24 = 0;
    self->dragging = 0;
    return self;
}
static void fp_jog_stamps(Footprint& f, JogControl*, Edx) { f.replay_only = "loads its stamps"; }
PORT_FN(0x00409370, "JogControl::JogControl", JogControl_ctor_n, fp_jog_stamps)

static void __fastcall JogControl_Create_n(JogControl* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, 0, (const void*)(uintptr_t)S_ELAPSED, 4u);
}
static void fp_jog_notes(Footprint& f, JogControl*, Edx) { f.replay_only = "adds a notification (allocates its copy)"; }
PORT_FN(0x004093c0, "JogControl::Create", JogControl_Create_n, fp_jog_notes)

// MouseDown: dragging, and MouseMove at once (the test of PhysReplayIsPaused's address is always true)
static void __fastcall JogControl_MouseDown_n(JogControl* self, Edx, int32_t x, int32_t y) {
    const void* vt = self->vtbl;
    self->dragging = 1;
    typedef void(__fastcall * MM_t)(JogControl*, Edx, int32_t, int32_t);
    ((MM_t)((void* const*)vt)[0x28 / 4])(self, 0, x, y);
}
static void fp_jog_down(Footprint& f, JogControl* self, Edx, int32_t, int32_t) {
    if ((uint32_t)(uintptr_t)self->vtbl != VT_JogControl) { f.replay_only = "a JogControl subclass's MouseMove"; return; }
    if (G8(S_HAVE_REPLAY)) { f.replay_only = "MouseMove shuttles the replay (PhysReplayShuttle / Pause)"; return; }
    f.add(self, sizeof(JogControl), "this");
}
PORT_FN(0x004093d0, "JogControl::MouseDown", JogControl_MouseDown_n, fp_jog_down)

static void __fastcall JogControl_MouseUp_n(JogControl* self, Edx, int32_t, int32_t) { self->dragging = 0; }
static void fp_jog_up(Footprint& f, JogControl* self, Edx, int32_t, int32_t) { f.add(self, sizeof(JogControl), "this"); }
PORT_FN(0x004093f0, "JogControl::MouseUp", JogControl_MouseUp_n, fp_jog_up)

static const float k_5 = 5.0f;                                  // 0x40a00000 (0x4db0a0)
static const double k_1d = 1.0;                                 // 0x4db0a8
static const uint32_t k_2pi = 0x40c90fdb, k_pi = 0x40490fdb, k_inv2pi = 0x3e22f983;
// the wheel's angle, from the mouse's offset: atan2(dy, dx) - frac(time / 5) x 2pi, compared with pi (the status word) as
// it's stored: `PhysicsGetTime; fdiv 5; fild dy; fild dx; fpatan; fld 1.0; fxch x3; _CIfmod; fmul 2pi; fsubp; fcom pi; fstp`
VP_ASM_CALLS static void jog_angle(int32_t my_dy, int32_t my_dx, float* out, uint16_t* sw) {
    float a;
    uint16_t s;
#ifdef VP_GCC
    __asm__ volatile("mov eax, 0x0042bc80\n\t"
                     "call eax\n\t"
                     "fdiv %[k_5]\n\t"
                     "fild %[my_dy]\n\t"
                     "fild %[my_dx]\n\t"
                     "fpatan\n\t"
                     "fld %[k_1d]\n\t"
                     "fxch st(2)\n\t"
                     "fxch st(1)\n\t"
                     "fxch st(2)\n\t"
                     "mov eax, 0x004cf36a\n\t"
                     "call eax\n\t"
                     "fmul %[k_2pi]\n\t"
                     "fsubp st(1), st\n\t"
                     "fcom %[k_pi]\n\t"
                     "fstp %[a]\n\t"
                     "fnstsw ax\n\t"
                     "mov %[s], ax"
                     : [a] "=m"(a), [s] "=m"(s)
                     : [k_5] "m"(k_5), [my_dy] "m"(my_dy), [my_dx] "m"(my_dx), [k_1d] "m"(k_1d), [k_2pi] "m"(k_2pi), [k_pi] "m"(k_pi)
                     : VP_X87_CLOBBERS, "eax", "ecx", "edx", "cc", "memory");
#else
    __asm {
        mov eax, 0x0042bc80
        call eax
        fdiv dword ptr [k_5]
        fild dword ptr [my_dy]
        fild dword ptr [my_dx]
        fpatan
        fld qword ptr [k_1d]
        fxch st(2)
        fxch st(1)
        fxch st(2)
        mov eax, 0x004cf36a
        call eax
        fmul dword ptr [k_2pi]
        fsubp st(1), st
        fcom dword ptr [k_pi]
        fstp dword ptr [a]
        fnstsw ax
        mov s, ax
    }
#endif
    *out = a;
    *sw = s;
}
static void __fastcall JogControl_MouseMove_n(JogControl* self, Edx, int32_t mx, int32_t my) {
    if (!G8(S_HAVE_REPLAY) || !self->dragging) return;
    const int32_t dy = (int32_t)((uint32_t)my - (uint32_t)(self->h / 2) - (uint32_t)self->y);
    const int32_t dx = (int32_t)((uint32_t)mx - (uint32_t)(self->w / 2) - (uint32_t)self->x);
    volatile float a;
    float a0;
    uint16_t sw;
    jog_angle(dy, dx, &a0, &sw);
    a = a0;
    if (!(sw & 0x4100)) a = (float)(D(a) - D(bits_f(k_2pi)));  // fcom pi; test ah, 0x41: above pi
    if (*(volatile uint32_t*)&a > 0xc0490fdbu) a = (float)(D(a) + D(bits_f(k_2pi)));   // the bits, unsigned: below -pi
    const float v = (float)((D(a) * D(k_5)) * D(bits_f(k_inv2pi)));
    ccall<void>(F_WorldFXDisable);
    ccall<void>(F_PhysReplayShuttle, f_bits(v));
    ccall<void>(F_PhysReplayPause);
}
static void fp_jog_move(Footprint& f, JogControl* self, Edx, int32_t, int32_t) {
    if (G8(S_HAVE_REPLAY) && self->dragging) f.replay_only = "shuttles the replay (PhysReplayShuttle / Pause)";
}
PORT_FN(0x00409400, "JogControl::MouseMove", JogControl_MouseMove_n, fp_jog_move)

// Callback: redrawn (the test of PhysReplayIsPaused's address is always true)
static void __fastcall JogControl_Callback_n(JogControl* self, Edx, int32_t, const void*) { tcall<void>(F_UICC_Dirty, self); }
static void fp_jog_cb(Footprint& f, JogControl* self, Edx, int32_t, const void*) { fp_dirty(f, self); }
PORT_FN(0x00409510, "JogControl::Callback", JogControl_Callback_n, fp_jog_cb)

// Draw: the wheel, and the detent on its rim, turned by the time
VP_ASM_CALLS static void jog_cos_sin(float* c, float* s) {
    float v_c, v_s;
#ifdef VP_GCC
    __asm__ volatile("mov eax, 0x0042bc80\n\t"
                     "call eax\n\t"
                     "fdiv %[k_5]\n\t"
                     "fld %[k_1d]\n\t"
                     "mov eax, 0x004cf36a\n\t"
                     "call eax\n\t"
                     "fmul %[k_2pi]\n\t"
                     "fld st(0)\n\t"
                     "fcos\n\t"
                     "fstp %[v_c]\n\t"
                     "fsin\n\t"
                     "fstp %[v_s]"
                     : [v_c] "=m"(v_c), [v_s] "=m"(v_s)
                     : [k_5] "m"(k_5), [k_1d] "m"(k_1d), [k_2pi] "m"(k_2pi)
                     : VP_X87_CLOBBERS, "eax", "ecx", "edx", "cc", "memory");
#else
    __asm {
        mov eax, 0x0042bc80
        call eax
        fdiv dword ptr [k_5]
        fld qword ptr [k_1d]
        mov eax, 0x004cf36a
        call eax
        fmul dword ptr [k_2pi]
        fld st(0)
        fcos
        fstp dword ptr [v_c]
        fsin
        fstp dword ptr [v_s]
    }
#endif
    *c = v_c;
    *s = v_s;
}
static void __fastcall JogControl_Draw_n(JogControl* self, Edx, gxCanvas* cv) {
    ccall<gxCanvas*>(F_gxSetCanvas, cv);
    float c, s;
    jog_cos_sin(&c, &s);
    static const float k03 = 0.3f;                               // 0x3e99999a (0x4db0c0)
    const float r = (float)(D(self->w) * D(k03));
    ccall<void>(F_gxDrawStamp, (void*)self->wheel, (int32_t)self->x, (int32_t)self->y, 0, (const void*)0);
    const int32_t h2 = self->h / 2;
    const int32_t oy = x87_ftol(D(s) * D(r));
    const int32_t y = (int32_t)((uint32_t)h2 + (uint32_t)oy + (uint32_t)self->y);
    const int32_t ox = x87_ftol(D(r) * D(c));
    const int32_t x = (int32_t)((uint32_t)ox + (uint32_t)(self->w / 2) + (uint32_t)self->x);
    ccall<void>(F_gxDrawStamp, (void*)self->detent, x, y, 0, (const void*)0);
}
static void fp_jog_draw(Footprint& f, JogControl*, Edx, gxCanvas* c) {
    fp_draw_canvas(f, c);
    UI_FP(f, 0x005d5600, 9, "the CRT's _CIfmod dispatch: its operand (a double) and flag");
}
PORT_FN(0x00409530, "JogControl::Draw", JogControl_Draw_n, fp_jog_draw)

static void* __fastcall JogControl_delete_n(JogControl* self, Edx, uint32_t flags) {
    void* d = self->detent;
    self->vtbl = (const void*)(uintptr_t)VT_JogControl;
    ccall<void>(F_gxForgetStamp, d);
    ccall<void>(F_gxForgetStamp, (void*)self->wheel);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void fp_jog_delete(Footprint& f, JogControl*, Edx, uint32_t) { f.replay_only = "forgets its stamps (and frees)"; }
PORT_FN(0x00409600, "JogControl::`vector deleting destructor'", JogControl_delete_n, fp_jog_delete)

// =========================================================================================================================
// ReplayView
// =========================================================================================================================
static void __fastcall ReplayView_Create_n(ReplayView*, Edx) {
    set_view();
    if (G8(S_FROM_RACE)) ccall<void>(F_UIHideGroup, GU32(S_GROUP_ANALYSIS));
    else ccall<void>(F_UIHideGroup, GU32(S_GROUP_MAIN));
}
static void fp_view_create(Footprint& f, ReplayView*, Edx) { f.replay_only = "sets the 3D view (the renderer's state)"; }
PORT_FN(0x00409640, "ReplayView::Create", ReplayView_Create_n, fp_view_create)

// Added: the camera hot keys (F1..F12), the car and view choosers and their keys, the transport buttons and keys in the
// play / pause groups, the jog wheel
static void __fastcall ReplayView_Added_n(ReplayView* self, Edx) {
    UIDialogItem it[40];
    int k = 0;
#define IC(...) ictor(&it[k++], __VA_ARGS__)
#define BB(id, x, y, stamp, cb) ccall<void*>(F_UI_BBUTTON, &it[k++], (int32_t)(id), (int32_t)(x), (int32_t)(y), CP(stamp), (uint32_t)(cb))
#define BR(id, x, y, stamp, cb) ccall<void*>(F_UI_BREPEATBUTTON, &it[k++], (int32_t)(id), (int32_t)(x), (int32_t)(y), CP(stamp), (uint32_t)(cb))
    for (uint32_t c = 0; c < 12; c++) IC(4, c, 0x170 + c, 0, 0, 0, 0, 0, F_CAMERA_IN_CAR_cb + 0x10 * c, 0);
    IC(4, 0, 0x121, 0, 0, 0, 0, 0, F_car_cb, 0);
    BB(1, 0x10, 0x47, 0x004e4e2c, F_prev_car_cb);                 // lscrollr.stp
    BB(1, 0x79, 0x47, 0x004e4e1c, F_next_car_cb);                 // rscrollr.stp
    IC(4, 0, 0x122, 0, 0, 0, 0, 0, F_prev_car_cb, 0);
    IC(4, 0, 0x121, 0, 0, 0, 0, 0, F_next_car_cb, 0);
    IC(4, 0, 0x124, 0, 0, 0, 0, 0, F_player_car_cb, 0);
    IC(5, 0, 0x33, 0x50, 0, 0, S_CAR_NAME, 0, 0, 0x12);
    BB(1, 0x98, 0x47, 0x004e4e2c, F_prev_view_cb);
    BB(1, 0x101, 0x47, 0x004e4e1c, F_next_view_cb);
    IC(5, 0, 0xba, 0x50, 0, 0, S_CAMERA_NAME, 0, 0, 0x12);
    BB(5, 0x14, 0x26, 0x004e4e10, F_rewind_to_start_cb);           // rp_rws.stp
    IC(4, 0, 0x325, 0, 0, 0, 0, 0, F_rewind_to_start_cb, 0);
    BR(1, 0x46, 0x26, 0x004e4e04, F_rewind_cb);                    // rp_rw.stp
    BR(3, 0x78, 0x26, 0x004e4df8, F_ff_cb);                        // rp_ff.stp
    BB(6, 0xaa, 0x26, 0x004e4dec, F_ff_to_end_cb);                 // rp_ffe.stp
    IC(4, 0, 0x327, 0, 0, 0, 0, 0, F_ff_to_end_cb, 0);
    IC(1, 0, 0, 0, 0, 0, 0, 0, S_GROUP_PAUSED, 0);
    BB(2, 0x14, 8, 0x004e4de0, F_play_cb);                         // rp_play.stp
    IC(4, 0, 0x128, 0, 0, 0, 0, 0, F_play_cb, 0);
    IC(4, 0, 0x70, 0, 0, 0, 0, 0, F_play_cb, 0);
    IC(1, 1, 0, 0, 0, 0, 0, 0, S_GROUP_PAUSED, 0);
    IC(1, 0, 0, 0, 0, 0, 0, 0, S_GROUP_PLAYING, 0);
    BB(4, 0xaa, 8, 0x004e4dd4, F_stop_cb);                         // rp_stop.stp
    IC(4, 0, 0x128, 0, 0, 0, 0, 0, F_stop_cb, 0);
    IC(4, 0, 0x70, 0, 0, 0, 0, 0, F_stop_cb, 0);
    IC(1, 1, 0, 0, 0, 0, 0, 0, S_GROUP_PLAYING, 0);
    ccall<void*>(F_UI_CUSTOM, &it[k++], 0xe6, 0xd, 0x30, 0x30, (void*)&self->jog);
    item_end(&it[k]);
    tcall<void>(F_UICC_AddItems, self, &it[0]);
#undef IC
#undef BB
#undef BR
}
static void fp_view_added(Footprint& f, ReplayView*, Edx) { f.replay_only = "builds widgets (_UIAddItems allocates, loads stamps)"; }
PORT_FN(0x00409690, "ReplayView::Added", ReplayView_Added_n, fp_view_added)

// ---- UI_BBUTTON / UI_BREPEATBUTTON / UI_CUSTOM (0x409d20, 0x409d70, 0x409dc0): an item, returned through the hidden pointer -
static void* __cdecl UI_BBUTTON_n(void* ret, int32_t id, int32_t x, int32_t y, const char* stamp, uint32_t cb) {
    volatile uint32_t* d = (volatile uint32_t*)ret;
    d[0] = 3; d[1] = (uint32_t)id; d[2] = (uint32_t)x; d[3] = (uint32_t)y; d[4] = 0; d[5] = 0; d[6] = U(stamp); d[7] = 0;
    d[9] = 0; d[8] = cb; d[10] = 0; d[11] = 0; d[12] = 0; d[13] = 0;
    return ret;
}
static void* __cdecl UI_BREPEATBUTTON_n(void* ret, int32_t id, int32_t x, int32_t y, const char* stamp, uint32_t cb) {
    volatile uint32_t* d = (volatile uint32_t*)ret;
    d[0] = 3; d[1] = (uint32_t)id; d[2] = (uint32_t)x; d[3] = (uint32_t)y; d[4] = 0; d[5] = 0; d[6] = U(stamp);
    d[9] = 0; d[10] = 0; d[8] = cb; d[11] = 0; d[12] = 0; d[13] = 0; d[7] = 1;
    return ret;
}
static void fp_bitem(Footprint& f, void* ret, int32_t, int32_t, int32_t, const char*, uint32_t) { f.add(ret, 0x38, "the item"); }
PORT_FN(0x00409d20, "UI_BBUTTON", UI_BBUTTON_n, fp_bitem)
PORT_FN(0x00409d70, "UI_BREPEATBUTTON", UI_BREPEATBUTTON_n, fp_bitem)
static void* __cdecl UI_CUSTOM_n(void* ret, int32_t x, int32_t y, int32_t w, int32_t h, void* ctl) {
    volatile uint32_t* d = (volatile uint32_t*)ret;
    d[0] = 0x17; d[1] = 0; d[2] = (uint32_t)x; d[6] = 0x004e4db8; d[7] = 0; d[3] = (uint32_t)y; d[4] = (uint32_t)w;
    d[9] = 0; d[10] = 0; d[5] = (uint32_t)h; d[11] = 0; d[12] = 0; d[8] = U(ctl); d[13] = 0;
    return ret;
}
static void fp_citem(Footprint& f, void* ret, int32_t, int32_t, int32_t, int32_t, void*) { f.add(ret, 0x38, "the item"); }
PORT_FN(0x00409dc0, "UI_CUSTOM", UI_CUSTOM_n, fp_citem)

static void __fastcall ReplayView_Destroy_n(ReplayView*, Edx) {}
static void fp_view_p0(Footprint& f, ReplayView*, Edx) { f.pure = true; }
PORT_FN(0x00409e10, "ReplayView::Destroy", ReplayView_Destroy_n, fp_view_p0)
static void __fastcall ReplayView_Draw_n(ReplayView*, Edx, gxCanvas* c) { ccall<gxCanvas*>(F_gxSetCanvas, c); }
static void fp_view_draw(Footprint& f, ReplayView*, Edx, gxCanvas* c) { fp_draw_canvas(f, c); }
PORT_FN(0x00409e20, "ReplayView::Draw", ReplayView_Draw_n, fp_view_draw)

static void* __fastcall ReplayView_delete_n(ReplayView* self, Edx, uint32_t flags) {
    void* d = self->jog.detent;
    self->jog.vtbl = (const void*)(uintptr_t)VT_JogControl;
    ccall<void>(F_gxForgetStamp, d);
    ccall<void>(F_gxForgetStamp, (void*)self->jog.wheel);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->jog.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void fp_view_delete(Footprint& f, ReplayView*, Edx, uint32_t) { f.replay_only = "forgets its jog wheel's stamps (and frees)"; }
PORT_FN(0x00409e30, "ReplayView::`vector deleting destructor'", ReplayView_delete_n, fp_view_delete)

// =========================================================================================================================
// GraphControl
// =========================================================================================================================
static void __fastcall GraphControl_Update_n(GraphControl* self, Edx) { tcall<void>(F_UICC_Dirty, self); }
static void fp_graph_update(Footprint& f, GraphControl* self, Edx) { fp_dirty(f, self); }
PORT_FN(0x00409e80, "GraphControl::Update", GraphControl_Update_n, fp_graph_update)
static uint8_t __fastcall GraphControl_IsTabStop_n(GraphControl*, Edx) { return 1; }
static void fp_graph_p0(Footprint& f, GraphControl*, Edx) { f.pure = true; }
PORT_FN(0x00409e90, "GraphControl::IsTabStop", GraphControl_IsTabStop_n, fp_graph_p0)

// CharHit(char) -- a new virtual (slot 0x50), not UICustomControl::CharHit(unsigned short)'s override: space halves the zoom
// (below 0.5 it wraps to 4), '1'..'5' pick the graph
static uint8_t __fastcall GraphControl_CharHit_n(GraphControl*, Edx, uint32_t ch) {
    const uint32_t i = (uint32_t)((int32_t)(int8_t)(uint8_t)ch - 0x20);
    if (i > 0x15) return 0;
    if (i == 0) {
        static const float kh = 0.5f;                            // 0x3f000000 (0x4db0d8)
        const double z = D(GF(S_GRAPH_ZOOM)) * D(kh);
        GF(S_GRAPH_ZOOM) = (float)z;
        if (!(z >= D(kh))) G32(S_GRAPH_ZOOM) = 0x40800000;        // fcom; test ah, 1: below, or a NaN
        return 0;
    }
    if (i >= 0x11) G32(S_GRAPH_TYPE) = (int32_t)(i - 0x10);
    return 0;
}
static void fp_graph_char(Footprint& f, GraphControl*, Edx, uint32_t) {
    UI_FP(f, S_GRAPH_ZOOM, 4, "the graph's zoom");
    UI_FP(f, S_GRAPH_TYPE, 4, "the graph's type");
}
PORT_FN(0x00409ea0, "GraphControl::CharHit", GraphControl_CharHit_n, fp_graph_char)

// Draw: w / 4 bars, each the telemetry at its time (the zoom's span before now), up from the bottom (speed) or from the middle
static void __fastcall GraphControl_Draw_n(GraphControl* self, Edx, gxCanvas* cv) {
    const int32_t h = self->h;
    int32_t x = self->x;
    int32_t w = self->w;
    const int32_t y = self->y;
    if (!G8(S_HAVE_REPLAY)) return;
    ccall<gxCanvas*>(F_gxSetCanvas, cv);
    const float t = call_f(F_PhysicsGetTime);
    if (G32(S_GRAPH_TYPE) == 0) return;
    const int32_t n = w / 4;
    static const float k1 = 1.0f, kh = 0.5f, kq = 0.25f;
    static const uint32_t k005 = 0x3ba3d70a;                     // 0.005 (0x4db0e8)
    const double q = D(k1) / D(n);
    // FIX CANDIDATE: w / n divides by zero for a control narrower than 4 pixels (the analysis screen's is 0x104)
    int32_t bw = w / n;
    VP_OPAQUE(bw);                      // (GCC: divided here, as the original does -- n = 0 faults before the return)
    const float inv = (float)q;
    const int32_t k0 = x87_ftol(D(t) / (q * D(GF(S_GRAPH_ZOOM))));
    if (n <= 0) return;
    const int32_t bottom = (int32_t)((uint32_t)h + (uint32_t)y);
    const float base = (float)(D(k0) * D(inv));
    const float hf = (float)h;
    for (int32_t i = 0; n > i;) {
        uint8_t tel[0x14];
        volatile float v;
        *(volatile uint32_t*)&v = 0;
        const float at = (float)((((D(i) * D(inv)) - D(kh)) + D(base)) * D(GF(S_GRAPH_ZOOM)));
        if (ccall<uint8_t>(F_PhysReplayGetTelemetry, tel, f_bits(at))) {
            const int32_t ty = G32(S_GRAPH_TYPE);
            if (ty == 1) v = (float)((D(GF(S_GRAPH_SCALE)) * D(*(const volatile float*)(tel + 0x10))) * D(kq) + D(kh));
            else if (ty == 2) v = (float)((D(GF(S_GRAPH_SCALE)) * D(*(const volatile float*)(tel + 0xc))) * D(kq) + D(kh));
            else if (ty == 3) v = (float)((D(GF(S_GRAPH_SCALE)) * D(*(const volatile float*)(tel + 8))) * D(bits_f(k005)));
        }
        if (*(volatile uint32_t*)&v > 0x80000000u) *(volatile uint32_t*)&v = 0;        // below 0 (the bits, unsigned)
        if (*(volatile int32_t*)&v > 0x3f800000) *(volatile uint32_t*)&v = 0x3f800000;  // above 1 (the bits, signed)
        const int32_t top = (int32_t)((uint32_t)y + (uint32_t)x87_ftol((D(k1) - D(v)) * D(hf)));
        int32_t y0 = top, y1 = bottom;
        if (G32(S_GRAPH_TYPE) == 1 || G32(S_GRAPH_TYPE) == 2) {
            const int32_t mid = (int32_t)((uint32_t)(h / 2) + (uint32_t)y);
            y0 = top;
            y1 = mid;
            if (!(mid > top)) { y0 = mid; y1 = top; }
        }
        const uint32_t col = GU32(S_GRAPH_COLOR);
        i++;
        ccall<void>(F_gxRect, x, y0, (int32_t)((uint32_t)bw + (uint32_t)x - 1u), y1, col);
        x = (int32_t)((uint32_t)x + (uint32_t)bw);
    }
    (void)w;
}
static void fp_graph_draw(Footprint& f, GraphControl*, Edx, gxCanvas* c) { fp_draw_canvas(f, c); }
PORT_FN(0x00409f70, "GraphControl::Draw", GraphControl_Draw_n, fp_graph_draw)

static void* __fastcall GraphControl_delete_n(GraphControl* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void fp_graph_delete(Footprint& f, GraphControl* self, Edx, uint32_t flags) {
    if (flags & 1) { f.replay_only = "frees the object"; return; }
    f.add(self, 4, "its vtable");
}
PORT_FN(0x0040a170, "GraphControl::`vector deleting destructor'", GraphControl_delete_n, fp_graph_delete)

}  // namespace root_replay
}  // namespace
