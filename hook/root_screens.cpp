// root_screens.cpp -- M3 UI stage, step U3 (group A): the in-race overlays, rewritten faithfully (library `root`):
//
//   escape.obj   the escape (pause) menu a race hands EscapeMenuDo: its items drawn over the race (EscapeMenuDraw), the
//                keys (EscapeMenuKey: up / down, Enter / Space choose, Escape its escape item), do_callback (the race's
//                callbacks, the menu closed), all under its unsafe-module lock (SingleEnter / SingleLeave)
//   splash.obj   the splashes: generic_splash (a stamp, or a text in a box, on the grabbed screen or the current canvas),
//                loading / victory / loser / DNF / series, wait_key_timeout (a key, a click or the time), SplashDoneLoading,
//                SplashMustSeeStamp (a stamp with a countdown that must run out before a key ends it)
//   hack.obj     the cheats: the flags and factors from the options (HackRefresh), their getters (off in multiplayer)
//   gxdash.obj   the graphics debug dash: the LOD factor and the fog / lighting switches, and their keys
//   ghost.obj    one static initialiser the generator can't read ($E62: the ghost file's size)
//
// (countdwn.obj's three functions and HackEnd, bare `ret`s, are generated: hook/root_leftover.cpp.)
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own too, so a
// hooked rewrite is what runs); the race's escape-menu callbacks called through the menu's pointers.
//
// Footprints (main thread): the escape menu's key, draw and state functions write its two statics and the canvas; a
// callback (the race's code) or the lock's creation (SingleBegin) is replay_only. The splashes that load a stamp, grab the
// screen and flip it, or wait for input (their own loop) are replay_only -- the session replay is their check; one that
// only draws its text box on the current canvas writes the canvas. HackBegin / HackRefresh read and write the options
// (replay_only); the getters write nothing. GXDashboardDraw writes the canvas; GXDashboardKey the LOD factor, or the
// renderer's state through flip_option (replay_only).
//
// FIX CANDIDATEs (left faithful):
//   - EscapeMenuDraw / EscapeMenuKey index the menu's items with the selection unchecked (EscapeMenuKey keeps it in
//     0..count-1; a menu of more than 4 items would read past its 4).
//   - wait_key_timeout / SplashMustSeeStamp multiply the seconds by 1000 unchecked (a timeout over 2147483 s wraps).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "x87.h"
#include "ui_types.h"
#include "root_types.h"

namespace {
namespace rscreens {
using namespace uit;
using namespace rhud;

#define D(x) ((double)(x))
static __forceinline const EscapeMenu* esc_menu() { return UI_GP(const EscapeMenu, S_ESC_MENU); }
static __forceinline void esc_enter() { ccall<void>(F_SingleEnter, RH_G32(S_ESC_SINGLE), (const char*)0, 0); }
static __forceinline void esc_leave() { ccall<void>(F_SingleLeave, RH_G32(S_ESC_SINGLE), (const char*)0, 0); }
static void fp_none(Footprint&) {}

// =========================================================================================================================
// escape.obj
// =========================================================================================================================
static void __cdecl EscapeMenuBegin_n() {
    RH_G32(S_ESC_SINGLE) = ccall<int32_t>(F_SingleBegin, RH_CP(0x004e5234));   // "EscapeMenu"
    RH_GU32(S_ESC_MENU) = 0;
}
static void fp_EscapeMenuBegin(Footprint& f) { f.replay_only = "SingleBegin takes the kernel's global lock"; }
PORT_FN(0x0040c520, "EscapeMenuBegin", EscapeMenuBegin_n, fp_EscapeMenuBegin)

static void __cdecl EscapeMenuEnd_n() { ccall<void>(F_SingleEnd, RH_G32(S_ESC_SINGLE), (const char*)0, 0); }
static void fp_EscapeMenuEnd(Footprint& f) {
    const int32_t h = RH_G32(S_ESC_SINGLE);
    if (h < 1 || h > 16) { f.replay_only = "an unsafe module's handle out of the table"; return; }
    UI_FP(f, 0x00508750u + (uint32_t)h * 8u - 8u, 8, "the unsafe module's entry");
}
PORT_FN(0x0040c540, "EscapeMenuEnd", EscapeMenuEnd_n, fp_EscapeMenuEnd)

static uint8_t __cdecl EscapeMenuActive_n() {
    esc_enter();
    esc_leave();
    return RH_GU32(S_ESC_MENU) != 0 ? 1 : 0;
}
PORT_FN(0x0040c560, "EscapeMenuActive", EscapeMenuActive_n, fp_none)

// the items, centred, 60 pixels apart; the selected one in style 5's third state
static void __cdecl EscapeMenuDraw_n(gxCanvas* c) {
    if (!RH_GU32(S_ESC_MENU)) return;
    esc_enter();
    ccall<void*>(F_gxSetCanvas, c);
    const int32_t x = RH_G32(S_SCREEN_W) / 2;
    const int32_t n = esc_menu()->count;
    const int32_t y0 = (int32_t)((uint32_t)(1 - n) * 60u + (uint32_t)RH_G32(S_SCREEN_H)) / 2;
    if (n > 0) {
        int32_t y = y0;
        int32_t i = 0;
        do {
            const int32_t sel = RH_G32(S_ESC_SEL);
            const char* text = esc_menu()->items[i].text;
            const uint32_t state = (uint32_t)sel - (uint32_t)i == 0 ? 3u : 0u;
            i++;
            ccall<void>(F_UIStyleDraw, 5, x, y, text, state);
            y += 0x3c;
        } while (esc_menu()->count > i);
    }
    esc_leave();
}
static void fp_EscapeMenuDraw(Footprint& f, gxCanvas* c) {
    UI_FP_CUR_CANVAS(f);
    UI_FP_CANVAS(f, c);
}
PORT_FN(0x0040c5a0, "EscapeMenuDraw", EscapeMenuDraw_n, fp_EscapeMenuDraw)

static void __cdecl EscapeMenuKey_n(uint16_t key) {
    if (!RH_GU32(S_ESC_MENU)) return;
    esc_enter();
    switch ((uint32_t)key) {
    case 0x1b: ccall<void>(F_do_callback, esc_menu()->escape_id); break;
    case 0xd:
    case 0x20: {
        const int32_t sel = RH_G32(S_ESC_SEL);
        ccall<void>(F_do_callback, esc_menu()->items[sel].id);
        break;
    }
    case 0x126: {                                               // up
        const int32_t s = (int32_t)((uint32_t)RH_G32(S_ESC_SEL) - 1u);
        RH_G32(S_ESC_SEL) = s;
        if (s < 0) RH_G32(S_ESC_SEL) = esc_menu()->count - 1;
        break;
    }
    case 0x128: {                                               // down
        const EscapeMenu* m = esc_menu();
        RH_G32(S_ESC_SEL) = (int32_t)((uint32_t)RH_G32(S_ESC_SEL) + 1u);
        if (!(m->count > RH_G32(S_ESC_SEL))) RH_G32(S_ESC_SEL) = 0;
        break;
    }
    }
    esc_leave();
}
// do_callback's reach: the race's callbacks (unbounded) or the menu closed
static bool fp_callback(Footprint& f, const EscapeMenu* m, int32_t id) {
    if ((id != (int32_t)0xfff5d3d6 && m->chosen) || m->end) { f.replay_only = "calls the race's escape-menu callbacks"; return false; }
    UI_FP(f, S_ESC_MENU, 4, "the escape menu");
    return true;
}
static void fp_EscapeMenuKey(Footprint& f, uint16_t key) {
    const EscapeMenu* m = esc_menu();
    if (!m) return;
    if (key == 0x1b) fp_callback(f, m, m->escape_id);
    else if (key == 0xd || key == 0x20) {
        const int32_t sel = RH_G32(S_ESC_SEL);
        if (sel < 0 || sel >= 4) { f.replay_only = "the selection past the menu's items"; return; }
        fp_callback(f, m, m->items[sel].id);
    } else if (key == 0x126 || key == 0x128) UI_FP(f, S_ESC_SEL, 4, "the escape menu's selection");
}
PORT_FN(0x0040c660, "EscapeMenuKey", EscapeMenuKey_n, fp_EscapeMenuKey)

static void __cdecl EscapeMenuDo_n(const EscapeMenu* menu) {
    esc_enter();
    if (!RH_GU32(S_ESC_MENU)) {
        RH_GU32(S_ESC_SEL) = 0;
        RH_GP(S_ESC_MENU) = (void*)menu;
        const EscapeFn begin = menu->begin;
        if (begin) begin();
    }
    esc_leave();
}
static void fp_EscapeMenuDo(Footprint& f, const EscapeMenu* menu) {
    if (!RH_GU32(S_ESC_MENU) && menu->begin) { f.replay_only = "calls the race's escape-menu callback"; return; }
    UI_FP(f, S_ESC_SEL, 8, "the escape menu and its selection");
}
PORT_FN(0x0040c730, "EscapeMenuDo", EscapeMenuDo_n, fp_EscapeMenuDo)

static void __cdecl do_callback_n(int32_t id) {
    if (id != (int32_t)0xfff5d3d6) {
        const EscapeItemFn chosen = esc_menu()->chosen;
        if (chosen) chosen(id);
    }
    const EscapeFn end = esc_menu()->end;
    if (end) end();
    RH_GU32(S_ESC_MENU) = 0;
}
static void fp_do_callback(Footprint& f, int32_t id) {
    const EscapeMenu* m = esc_menu();
    if (!m) { f.replay_only = "no escape menu"; return; }
    fp_callback(f, m, id);
}
PORT_FN(0x0040c780, "do_callback", do_callback_n, fp_do_callback)

// =========================================================================================================================
// splash.obj
// =========================================================================================================================
static void __cdecl SplashGeneric_n(const char* text) { ccall<void>(F_generic_splash, (const char*)0, text, 1); }
static void fp_splash_replay(Footprint& f, const char*) { f.replay_only = "grabs the screen and flips it"; }
PORT_FN(0x0040c9e0, "SplashGeneric", SplashGeneric_n, fp_splash_replay)

// a stamp centred (and the text, if any, over it), or the text centred in a box; on the grabbed screen (then flipped)
// or the current canvas
static void __cdecl generic_splash_n(const char* stamp_name, const char* text, uint8_t grab) {
    void* stamp = 0;
    if (stamp_name) stamp = ccall<void*>(F_gxGetStamp, stamp_name);
    if (grab && !ccall<uint8_t>(F_grab_screen)) goto forget;
    if (stamp) {
        const int32_t h = RH_G32(S_SCREEN_H);
        const int32_t sh = ccall<int32_t>(F_gxStampHeight, stamp);
        const int32_t y = (int32_t)((uint32_t)h - (uint32_t)sh) / 2;
        const int32_t w = RH_G32(S_SCREEN_W);
        const int32_t sw = ccall<int32_t>(F_gxStampWidth, stamp);
        ccall<void>(F_gxDrawStamp, stamp, (int32_t)((uint32_t)w - (uint32_t)sw) / 2, y, 0, (void*)0);
        if (!text) goto release;
    } else {
        const int32_t half = (ccall<int32_t>(F_UIStyleWidth, 0xe, text) + 0x14) / 2;
        {
            const int32_t cy = RH_G32(S_SCREEN_H) / 2;
            const int32_t cx = RH_G32(S_SCREEN_W) / 2;
            ccall<void>(F_gxRect, cx - half, cy - 0xf, cx + half, cy + 0xf, RH_GU32(S_SPLASH_C1));
        }
        {
            const int32_t cy = RH_G32(S_SCREEN_H) / 2;
            const int32_t cx = RH_G32(S_SCREEN_W) / 2;
            ccall<void>(F_gxRect, cx - half + 2, cy - 0xd, cx + half - 2, cy + 0xd, RH_GU32(S_SPLASH_C2));
        }
    }
    {
        const int32_t y = RH_G32(S_SCREEN_H) / 2 + 4;
        const int32_t x = RH_G32(S_SCREEN_W) / 2;
        ccall<void>(F_UIStyleDraw, 0xe, x, y, text, 0u);
    }
release:
    if (grab) ccall<void>(F_release_screen);
forget:
    if (stamp) ccall<void>(F_gxForgetStamp, stamp);
}
static void fp_generic_splash(Footprint& f, const char* stamp_name, const char*, uint8_t grab) {
    if (stamp_name) { f.replay_only = "loads a stamp"; return; }
    if (grab) { f.replay_only = "grabs the screen and flips it"; return; }
    UI_FP_CUR_CANVAS(f);
}
PORT_FN(0x0040ca00, "generic_splash", generic_splash_n, fp_generic_splash)

static uint8_t __cdecl grab_screen_n() {
    if (ccall<uint8_t>(F_gxGrabScreen, RH_VP(S_SPLASH_CANVAS))) {
        ccall<void*>(F_gxSetCanvas, RH_VP(S_SPLASH_CANVAS));
        return 1;
    }
    return 0;
}
static void fp_grab_screen(Footprint& f) { f.replay_only = "grabs the screen (locks the back buffer)"; }
PORT_FN(0x0040cb60, "grab_screen", grab_screen_n, fp_grab_screen)

static void __cdecl release_screen_n() {
    ccall<void>(F_gxReleaseScreen);
    ccall<void>(F_gxFlip);                                      // jmp gxFlip
}
static void fp_release_screen(Footprint& f) { f.replay_only = "releases the screen and flips it"; }
PORT_FN(0x0040cb90, "release_screen", release_screen_n, fp_release_screen)

static void __cdecl SplashButDontGrabScreen_n(const char* text) {
    ccall<void>(F_generic_splash, RH_CP(0x004e5240), text, 0);   // "load.stp"
    RH_G32(S_SPLASH_UNTIL) = ccall<int32_t>(F_PTimeNow);
}
static void fp_SplashButDontGrabScreen(Footprint& f, const char*) { f.replay_only = "loads a stamp (load.stp)"; }
PORT_FN(0x0040cba0, "SplashButDontGrabScreen", SplashButDontGrabScreen_n, fp_SplashButDontGrabScreen)

static void __cdecl SplashLoading_n() {
    rh_xl_once(0x00505a80, 1, 0x00505a98, 0x004e524c, 0x0040cc30);   // Splash:Loading
    const char* t = xlate(0x00505a98);
    ccall<void>(F_generic_splash, RH_CP(0x004e525c), t, 1);       // "load.stp"
    RH_G32(S_SPLASH_UNTIL) = ccall<int32_t>(F_PTimeNow);
}
static void fp_splash0(Footprint& f) { f.replay_only = "grabs the screen, loads a stamp or waits for input (its own loop)"; }
PORT_FN(0x0040cbc0, "SplashLoading", SplashLoading_n, fp_splash0)

static void __cdecl SplashDoneLoading_n() {
    while (ccall<int32_t>(F_PTimeNow) < RH_G32(S_SPLASH_UNTIL)) {
        ccall<void>(F_TaskSleep, 0x32);
        ccall<void>(F_Win32Idle);
    }
}
PORT_FN(0x0040cc40, "SplashDoneLoading", SplashDoneLoading_n, fp_splash0)

// the end-of-race splashes: a translation in a box on the grabbed screen, then a key, a click or the time
static __forceinline void splash_wait(uint32_t guard, uint32_t xl, uint32_t key, uint32_t dtor, int32_t secs) {
    rh_xl_once(guard, 1, xl, key, dtor);
    const char* t = xlate(xl);
    ccall<void>(F_generic_splash, (const char*)0, t, 1);
    ccall<void>(F_wait_key_timeout, secs, 1);
}
static void __cdecl SplashVictory_n() { splash_wait(0x00505adc, 0x00505a58, 0x004e5268, 0x0040cd90, 10); }
PORT_FN(0x0040cc70, "SplashVictory", SplashVictory_n, fp_splash0)
static void __cdecl SplashLoser_n() { splash_wait(0x00505a50, 0x00505ad0, 0x004e5278, 0x0040ce10, 6); }
PORT_FN(0x0040cda0, "SplashLoser", SplashLoser_n, fp_splash0)
static void __cdecl SplashDNF_n() { splash_wait(0x00505acc, 0x00505a88, 0x004e5288, 0x0040ce90, 6); }
PORT_FN(0x0040ce20, "SplashDNF", SplashDNF_n, fp_splash0)
static void __cdecl SplashSeriesVictory_n() { splash_wait(0x00505a0c, 0x00505aa8, 0x004e5294, 0x0040cf10, 0x78); }
PORT_FN(0x0040cea0, "SplashSeriesVictory", SplashSeriesVictory_n, fp_splash0)
static void __cdecl SplashSeriesLoser_n() { splash_wait(0x00505a44, 0x00505a70, 0x004e52b0, 0x0040cf90, 0x78); }
PORT_FN(0x0040cf20, "SplashSeriesLoser", SplashSeriesLoser_n, fp_splash0)

// until a click, a key (if keys) or the time (none if negative); then the keys waiting are thrown away
static void __cdecl wait_key_timeout_n(int32_t secs, uint8_t keys) {
    const uint8_t no_wait = secs < 0;
    const int32_t until = (int32_t)((uint32_t)ccall<int32_t>(F_PTimeNow) + (uint32_t)secs * 1000u);
    do {
        ccall<void>(F_Win32Idle);
        if (ccall<uint8_t>(F_mouse_hit)) break;
        if (keys && ccall<uint8_t>(F_KeyHit)) break;
        if (no_wait) break;
    } while (ccall<int32_t>(F_PTimeNow) < until);
    while (ccall<uint8_t>(F_KeyHit)) ccall<uint16_t>(F_KeyGet);
}
static void fp_wait_key_timeout(Footprint& f, int32_t, uint8_t) { f.replay_only = "waits for input (its own loop)"; }
PORT_FN(0x0040cce0, "wait_key_timeout", wait_key_timeout_n, fp_wait_key_timeout)

struct MouseEvent { int32_t type, x, y, state; };
static uint8_t __cdecl mouse_hit_n() {
    MouseEvent ev;
    if (!ccall<uint8_t>(F_MouseGetEvent, &ev)) return 0;
    const int32_t t = *(volatile int32_t*)&ev.type;
    if (t == 0 || t == 2 || t == 4) return 1;                   // a button went down
    return 0;
}
static void fp_mouse_hit(Footprint& f) { f.replay_only = "takes an event from the mouse queue"; }
PORT_FN(0x0040cd50, "mouse_hit", mouse_hit_n, fp_mouse_hit)

static void __cdecl SplashGenericStamp_n(const char* name, uint8_t keys, int32_t secs) {
    ccall<void>(F_generic_splash, name, (const char*)0, 1);
    ccall<void>(F_wait_key_timeout, secs, keys);
}
static void fp_SplashGenericStamp(Footprint& f, const char*, uint8_t, int32_t) { f.replay_only = "loads a stamp, grabs the screen, waits for input"; }
PORT_FN(0x0040cfa0, "SplashGenericStamp", SplashGenericStamp_n, fp_SplashGenericStamp)

// a stamp redrawn every frame with the seconds left in euro12.fnt at (600, 456); a key or a click ends it once they run out
static void __cdecl SplashMustSeeStamp_n(const char* name, int32_t secs) {
    const int32_t t0 = ccall<int32_t>(F_PTimeNow);
    UIStyleDesc desc;
    volatile uint32_t* d = (volatile uint32_t*)&desc;
    const uint32_t c3 = RH_GU32(S_SPLASH_C3), c2 = RH_GU32(S_SPLASH_C2);
    d[0] = 0x004e52cc;                                          // "euro12.fnt"
    d[1] = 0x21;
    d[2] = c3; d[3] = 0; d[4] = c2; d[5] = 0; d[6] = 0;
    d[7] = c3; d[8] = 0; d[9] = c2; d[10] = 0; d[11] = 0; d[12] = 0;
    const int32_t until = (int32_t)((uint32_t)t0 + (uint32_t)secs * 1000u);
    const int32_t style = ccall<int32_t>(F_UIAddDynamicStyle, &desc);
    ccall<void>(F_MouseClear);
    gxCanvas screen;
    char buf[32];
    for (;;) {
        ccall<void>(F_Win32Idle);
        if (ccall<uint8_t>(F_gxGrabScreen, &screen)) {
            ccall<void*>(F_gxSetCanvas, &screen);
            ccall<void>(F_generic_splash, name, (const char*)0, 0);
            const int32_t left = (int32_t)((uint32_t)until - (uint32_t)ccall<int32_t>(F_PTimeNow)) / 1000;
            if (left >= 0) {
                RH_sprintf(buf, RH_CP(0x004e52d8), left);       // "%2.2d"
                ccall<void>(F_UIStyleDraw, style, 0x258, 0x1c8, (const char*)buf, 0u);
                if (left == 0) ccall<void>(F_KeyClear);
            }
            ccall<void>(F_gxReleaseScreen);
            ccall<void>(F_gxFlip);
        }
        if (ccall<int32_t>(F_PTimeNow) < until) continue;
        if (ccall<uint8_t>(F_KeyHit)) break;
        if (ccall<uint8_t>(F_mouse_hit)) break;
    }
    ccall<void>(F_UIRemoveStyle, style);
    ccall<void>(F_MouseClear);
    while (ccall<uint8_t>(F_KeyHit)) ccall<uint16_t>(F_KeyGet);
}
static void fp_SplashMustSeeStamp(Footprint& f, const char*, int32_t) { f.replay_only = "loads a stamp, flips the screen, waits for input"; }
PORT_FN(0x0040cfd0, "SplashMustSeeStamp", SplashMustSeeStamp_n, fp_SplashMustSeeStamp)

// =========================================================================================================================
// hack.obj
// =========================================================================================================================
static void __cdecl HackDisable_n() {
    const uint8_t on = RH_G8(S_HACK_ON);
    RH_G8(S_HACK_ON) = 0;
    RH_G8(S_HACK_SAVED) = on;
}
static void fp_HackDisable(Footprint& f) {
    UI_FP(f, S_HACK_ON, 1, "hack.obj enabled");
    UI_FP(f, S_HACK_SAVED, 1, "hack.obj saved flag");
}
PORT_FN(0x0040d350, "HackDisable", HackDisable_n, fp_HackDisable)

static void __cdecl HackRestore_n() { RH_G8(S_HACK_ON) = RH_G8(S_HACK_SAVED); }
static void fp_HackRestore(Footprint& f) { UI_FP(f, S_HACK_ON, 1, "hack.obj enabled"); }
PORT_FN(0x0040d370, "HackRestore", HackRestore_n, fp_HackRestore)

static void __cdecl HackBegin_n(uint8_t on) {
    RH_G8(S_HACK_ON) = on;
    ccall<void>(F_HackRefresh);
    const int32_t car = ccall<int32_t>(F_GetCarFileNumber, RH_CP(0x004e5320));   // "viper"
    const char* sec = UI_GP(const char, S_SEC_GAME);
    RH_G32(S_HACK_CAR) = car;
    ccall<void>(F_OptionsSetI, sec, RH_CP(0x004e5328), car);                     // car_index
}
static void fp_HackBegin(Footprint& f, uint8_t) { f.replay_only = "reads and writes the options"; }
PORT_FN(0x0040d380, "HackBegin", HackBegin_n, fp_HackBegin)

static void __cdecl HackRefresh_n() {
    RH_G8(S_HACK_STATUS) = 0;
    RH_GU32(S_HACK_THROTTLE) = 0x3f800000u;                    // 1.0f
    RH_GU32(S_HACK_GRIP) = 0x3f800000u;
    RH_GU32(S_HACK_GRAVITY) = 0x3f800000u;
    const char* sec = UI_GP(const char, S_SEC_GAME);
    RH_G8(S_HACK_HORN) = 0;
    RH_G8(S_HACK_NO_WALLS) = 0;
    RH_G8(S_HACK_PAVE) = 0;
    ccall<void>(F_OptionsGetB, sec, RH_CP(0x004e5334), RH_VP(S_HACK_STATUS));     // show_car_status_info
    ccall<void>(F_OptionsGetF, sec, RH_CP(0x004e534c), RH_VP(S_HACK_THROTTLE));   // throttle_boost
    ccall<void>(F_OptionsGetF, sec, RH_CP(0x004e535c), RH_VP(S_HACK_GRIP));       // grip_boost
    ccall<void>(F_OptionsGetF, sec, RH_CP(0x004e5368), RH_VP(S_HACK_GRAVITY));    // gravity_factor
    ccall<void>(F_OptionsGetB, sec, RH_CP(0x004e5378), RH_VP(S_HACK_HORN));       // horn_ball
    ccall<void>(F_OptionsGetB, sec, RH_CP(0x004e5384), RH_VP(S_HACK_NO_WALLS));   // no_walls
    ccall<void>(F_OptionsGetB, sec, RH_CP(0x004e5390), RH_VP(S_HACK_PAVE));       // pave_the_world
    if (RH_G8(S_HACK_ON)) ccall<void>(F_OptionsGetI, sec, RH_CP(0x004e53a0), RH_VP(S_HACK_CAR));   // car_index
}
static void fp_HackRefresh(Footprint& f) { f.replay_only = "reads the options (OptionsGet adds what's missing)"; }
PORT_FN(0x0040d3c0, "HackRefresh", HackRefresh_n, fp_HackRefresh)

static uint8_t __cdecl HackEnabled_n() { return RH_G8(S_HACK_ON); }
PORT_FN(0x0040d4b0, "HackEnabled", HackEnabled_n, fp_none)

static uint8_t __cdecl HackShowCarStatusInfo_n() { return RH_G8(S_HACK_ON) ? RH_G8(S_HACK_STATUS) : (uint8_t)0; }
PORT_FN(0x0040d4c0, "HackShowCarStatusInfo", HackShowCarStatusInfo_n, fp_none)

// a factor: the option's while the cheats are on and it's not multiplayer, else 1.0 (fld dword: the float in st0)
static __forceinline float hack_factor(uint32_t at) {
    if (RH_G8(S_HACK_ON) && !ccall<uint8_t>(F_MultiEnabled)) return RH_GF(at);
    return RH_GF(0x004db2ec);                                   // 1.0f
}
static float __cdecl HackThrottleBoost_n() { return hack_factor(S_HACK_THROTTLE); }
PORT_FN(0x0040d4e0, "HackThrottleBoost", HackThrottleBoost_n, fp_none)
static float __cdecl HackGripBoost_n() { return hack_factor(S_HACK_GRIP); }
PORT_FN(0x0040d500, "HackGripBoost", HackGripBoost_n, fp_none)
static float __cdecl HackGravityFactor_n() { return hack_factor(S_HACK_GRAVITY); }
PORT_FN(0x0040d520, "HackGravityFactor", HackGravityFactor_n, fp_none)

static uint8_t __cdecl HackHornBall_n() { return RH_G8(S_HACK_ON) && RH_G8(S_HACK_HORN) ? 1 : 0; }
PORT_FN(0x0040d540, "HackHornBall", HackHornBall_n, fp_none)
static uint8_t __cdecl HackNoWalls_n() { return RH_G8(S_HACK_ON) && RH_G8(S_HACK_NO_WALLS) ? 1 : 0; }
PORT_FN(0x0040d560, "HackNoWalls", HackNoWalls_n, fp_none)
static uint8_t __cdecl HackPaveTheWorld_n() { return RH_G8(S_HACK_ON) && RH_G8(S_HACK_PAVE) ? 1 : 0; }
PORT_FN(0x0040d580, "HackPaveTheWorld", HackPaveTheWorld_n, fp_none)

static int32_t __cdecl HackGetCarIndex_n() {
    if (ccall<uint8_t>(F_MultiEnabled)) return ccall<int32_t>(F_GetCarFileNumber, RH_CP(0x004e53ac));   // "viper"
    return RH_G32(S_HACK_CAR);
}
PORT_FN(0x0040d5a0, "HackGetCarIndex", HackGetCarIndex_n, fp_none)

// =========================================================================================================================
// gxdash.obj
// =========================================================================================================================
static void __cdecl GXDashboardDraw_n(gxCanvas* c) {
    char buf[0x50];
    ccall<void*>(F_gxSetCanvas, c);
    ccall<void>(F_gxRect, 1, 1, 0xe6, 0x22, RH_GU32(S_GXD_BACK));
    RH_sprintf(buf, RH_CP(0x004e53c0), D(RH_GF(S_LOD)));         // "LOD Factor:  %4.1f"
    ccall<void>(F_gxText, 0xa, 0xa, (const char*)buf, RH_GU32(S_GXD_TEXT));
    ccall<void>(F_gxText, 0xa, 0x14, RH_CP(0x004e53d4), RH_GU32(S_GXD_HELP));   // "[=down ]=up '=1.0"
    uint32_t col = ccall<uint8_t>(F_mrIsEnabled, 0x40) ? RH_GU32(S_GXD_ON) : RH_GU32(S_GXD_OFF);
    ccall<void>(F_gxText, 0xa0, 0xa, RH_CP(0x004e53e8), col);                    // "FOG"
    col = ccall<uint8_t>(F_mrIsEnabled, 1) ? RH_GU32(S_GXD_ON) : RH_GU32(S_GXD_OFF);
    ccall<void>(F_gxText, 0xa0, 0x14, RH_CP(0x004e53ec), col);                   // "LIGHT"
}
static void fp_GXDashboardDraw(Footprint& f, gxCanvas* c) {
    UI_FP_CUR_CANVAS(f);
    UI_FP_CANVAS(f, c);
}
PORT_FN(0x0040d7e0, "GXDashboardDraw", GXDashboardDraw_n, fp_GXDashboardDraw)

// ' 1.0, [ down 0.2, ] up 0.2 (kept to 0.2..20 by the bits: a NaN goes to 20, a negative to 0.2), f fog, l lighting
static uint8_t __cdecl GXDashboardKey_n(uint16_t key) {
    switch ((uint32_t)key) {
    case '\'': RH_GU32(S_LOD) = 0x3f800000u; break;
    case '[': RH_GF(S_LOD) = (float)(D(RH_GF(S_LOD)) - D(RH_GF(0x004db2f4))); break;   // 0.2f
    case ']': RH_GF(S_LOD) = (float)(D(RH_GF(S_LOD)) + D(RH_GF(0x004db2f4))); break;
    case 'f': ccall<void>(F_flip_option, 0x40); break;
    case 'l': ccall<void>(F_flip_option, 1); break;
    }
    if (RH_G32(S_LOD) > 0x41a00000) RH_GU32(S_LOD) = 0x41a00000u;   // 20.0f
    if (RH_G32(S_LOD) < 0x3e4ccccd) RH_GU32(S_LOD) = 0x3e4ccccdu;   // 0.2f
    return 0;
}
static void fp_GXDashboardKey(Footprint& f, uint16_t key) {
    if (key == 'f' || key == 'l') { f.replay_only = "switches a renderer feature (mrEnable / mrDisable)"; return; }
    UI_FP(f, S_LOD, 4, "gxLODFactor");
}
PORT_FN(0x0040d8c0, "GXDashboardKey", GXDashboardKey_n, fp_GXDashboardKey)

static void __cdecl flip_option_n(int32_t feature) {
    if (ccall<uint8_t>(F_mrIsEnabled, feature)) ccall<void>(F_mrDisable, feature);
    else ccall<void>(F_mrEnable, feature);
}
static void fp_flip_option(Footprint& f, int32_t) { f.replay_only = "switches a renderer feature (mrEnable / mrDisable)"; }
PORT_FN(0x0040d9b0, "flip_option", flip_option_n, fp_flip_option)

// =========================================================================================================================
// ghost.obj: the one static initialiser tools/gen_leftovers.py can't read (a computed store)
// =========================================================================================================================
// $E62: the ghost file's size, 0x1cc (its header) + 0x3c (a packet) * the packets (0x505b88)
static void __cdecl E62_ghost_n() { RH_GU32(0x00505ba4) = RH_GU32(0x00505b88) * 60u + 0x1ccu; }
static void fp_E62_ghost(Footprint& f) { f.replay_only = "a static initialiser (runs once, from the CRT's _initterm)"; }
PORT_FN(0x0040dc50, "$E62(ghost.obj)", E62_ghost_n, fp_E62_ghost)

}  // namespace rscreens
}  // namespace
