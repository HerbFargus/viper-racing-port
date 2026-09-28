// root_types.h -- M3 UI stage, step U3 (group A): the in-race HUD's layouts and statics (library `root`: dash.obj,
// gxdash.obj, countdwn.obj, escape.obj, splash.obj, hack.obj), shared by hook/root_dash.cpp, hook/root_screens.cpp and
// test/world_root_hud.cpp. Groups B (race flow) and C (replays, ghosts) keep their own layouts in their own headers; what
// here is another object's (CarMgrInfo, CarInfo, the car's message, LocaleInfo) is only the fields this group touches.
//
// Recovered from the v1.0 disassembly. Fields another thread writes (the physics thread's CarInfo and car message) are
// volatile, so every read happens where the original's does, at its width and in its order.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace rhud {
using uit::Edx;

// ---- other objects' structures: the fields read here ----------------------------------------------------------------
// the car's message (CarMgrInfo +0x134): the physics thread's latest state of the car
struct CarMsg {
    uint8_t _00[0x34];
    volatile uint32_t rpm;                        // +0x34 float (copied by its bits: the dial's clamp compares them)
    uint8_t _38[0x10];
    volatile int32_t gear;                        // +0x48 -1 reverse, 0 neutral, 1..7 (indexes the gear names)
    volatile float speed;                         // +0x4c m/s (times the locale's factor)
    uint8_t _50[0x19];
    volatile uint8_t flags;                       // +0x69 0x80 off the track (the reset banner), 0x40 damaged
};
static_assert(offsetof(CarMsg, gear) == 0x48 && offsetof(CarMsg, flags) == 0x69, "CarMsg");

// CarInfo (CarMgrInfo +0x138, 0x38): the race's view of a car (CarMgrUpdateCar). Floats are read as their bits where
// the original compares or copies the bits (`cmp dword [x], imm`, `mov eax, [x]`), as floats where it loads them.
struct CarInfo {
    volatile int32_t lap;                         // +0x00
    volatile int32_t _04;
    volatile int32_t place;                       // +0x08 1..
    union { volatile float lap_time; volatile uint32_t lap_time_b; };      // +0x0c the running lap
    union { volatile float best; volatile uint32_t best_b; };              // +0x10 the best lap
    union { volatile float last; volatile uint32_t last_b; };              // +0x14 the last lap
    volatile float best_speed;                    // +0x18 (a 0-60 run's speed)
    union { volatile float gap_behind; volatile uint32_t gap_behind_b; };  // +0x1c seconds to the car behind
    volatile int32_t behind;                      // +0x20 its index
    union { volatile float gap_ahead; volatile uint32_t gap_ahead_b; };    // +0x24 seconds to the car ahead
    volatile int32_t ahead;                       // +0x28 its index
    uint8_t _2c[0x0c];
};
static_assert(sizeof(CarInfo) == 0x38 && offsetof(CarInfo, ahead) == 0x28, "CarInfo");

// CarMgrInfo (0x170, 16 at 0x554090; world.obj)
struct CarMgrInfo {
    volatile int32_t type;                        // +0x000 3 a ghost
    uint8_t _004;
    char name[0x0d];                              // +0x005
    uint8_t _012[0x134 - 0x12];
    CarMsg* volatile msg;                         // +0x134
    CarInfo info;                                 // +0x138
};
static_assert(sizeof(CarMgrInfo) == 0x170 && offsetof(CarMgrInfo, msg) == 0x134 && offsetof(CarMgrInfo, info) == 0x138,
              "CarMgrInfo");

// the escape (pause) menu a race hands EscapeMenuDo (escape.obj keeps a pointer to it at 0x5059c0)
struct EscapeItem {
    const char* text;
    int32_t id;                                   // -0xa2c2a (0xfff5d3d6): no callback
};
typedef void(__cdecl* EscapeFn)();
typedef void(__cdecl* EscapeItemFn)(int32_t);
struct EscapeMenu {                               // 0x34
    EscapeItem items[4];                          // +0x00
    int32_t count;                                // +0x20
    int32_t escape_id;                            // +0x24 what Escape does
    EscapeFn begin;                               // +0x28 EscapeMenuDo, when the menu opens
    EscapeFn end;                                 // +0x2c do_callback, when it closes
    EscapeItemFn chosen;                          // +0x30 do_callback(id)
};
static_assert(sizeof(EscapeMenu) == 0x34, "EscapeMenu");

// ---- statics (v1.0) ---------------------------------------------------------------------------------------------------
enum : uint32_t {
    // dash.obj: the banners' flags, as drawn this frame (DashboardDraw3D copies the next frame's over them)
    S_SHOW = 0x00504178,             // 6 bytes: summary, damaged, reset car, award, countdown (2)
    S_SHOW_NEXT = 0x00504198,        // 6 bytes: DashboardAlwaysUpdate's
    S_REDRAW = 0x00504180,           // u8: DashboardReset -- redraw the whole dash
    S_DIRTY = 0x0050428c,            // the rectangles to copy to the screen (0xffff: everything)
    S_LAST = 0x004e3bb0,             // what the dash last drew: focus car, place, lap, (4 more), best, last, (2), time mode
    S_LAST_END = 0x004e3bd8,
    S_RECTS = 0x004e3bd8,            // {x0, y0, x1, y1}: 3 redrawn always, then 9 by S_DIRTY's bits (0x4e3c08)
    S_FLAG_RECTS = 0x004e3c08,
    S_GEAR_NAMES = 0x004e3c98,       // const char*[9]: reverse, neutral, "1".."7" (built on the first full draw)
    S_CANVAS = 0x00504250,           // the dash's canvas (status.stp's size)
    S_BACK = 0x00504140,             // its background (status.stp)
    S_CLEAR = 0x005040fc,            // colour
    S_START_SEEN = 0x004e3ba8,       // float: the physics clock when the player finished
    S_CAMERA_STEP = 0x004e3bac,      // 0, 1, 2: the camera switched once the race is over
    S_OFF_TRACK_SINCE = 0x005041e8,  // PTimeNow when the car left the track, or 0
    S_STARTED_AT = 0x005041e4,       // PTimeNow while the countdown shows
    S_BEGIN_TIME = 0x005040c4,
    S_DASH_ONCE = 0x00504304,        // DashboardUpdate's Xlators (bits 1..0x80)
    S_DASH_ONCE2 = 0x0050413c,       // ... (bit 1)
    S_DRAW_ONCE = 0x005040f4,        // TheRealDashboardDraw's (1, 2, 4, 0x10..0x80) and the gear names (8)
    S_DRAW_ONCE2 = 0x005041fc,       // ... (1, 2)
    // fonts, stamps, palettes, colours (DashboardBegin)
    S_F_EURO6 = 0x00504128, S_F_EURO9 = 0x00504184, S_F_EURO19 = 0x005041f8, S_F_LCD = 0x0050410c, S_F_EURO7 = 0x0050429c,
    S_ST_AWARD = 0x005042ec, S_ST_MPH = 0x005041a4, S_ST_RPM = 0x005042a0, S_ST_STATUS = 0x0050430c,
    S_ST_ARROW = 0x005041a0, S_ST_START = 0x005041d0, S_ST_SUMMARY = 0x005042c8,
    S_P_WHITE = 0x00504194, S_P_RED = 0x00504274, S_P_GREY = 0x00504308, S_P_GREEN = 0x005041cc,
    S_P_SUM = 0x005041f0, S_P_SUM_ME = 0x005041f4,
    S_C_NEEDLE = 0x0050411c, S_C_NEEDLE2 = 0x005041bc,
    // escape.obj
    S_ESC_SINGLE = 0x004e5230,       // SingleBegin("EscapeMenu")
    S_ESC_SEL = 0x005059bc,
    S_ESC_MENU = 0x005059c0,         // const EscapeMenu*, or 0
    // splash.obj
    S_SPLASH_CANVAS = 0x00505a20,    // the grabbed screen
    S_SPLASH_UNTIL = 0x00505a7c,     // PTimeNow of the last splash (SplashDoneLoading waits for it)
    S_SPLASH_C1 = 0x00505a48, S_SPLASH_C2 = 0x00505a10, S_SPLASH_C3 = 0x00505aa4,
    // hack.obj
    S_HACK_ON = 0x004e52e4, S_HACK_SAVED = 0x004e52e0,
    S_HACK_STATUS = 0x00505b18, S_HACK_THROTTLE = 0x00505ae0, S_HACK_GRIP = 0x00505b0c, S_HACK_GRAVITY = 0x00505b1c,
    S_HACK_HORN = 0x00505b40, S_HACK_NO_WALLS = 0x00505b14, S_HACK_PAVE = 0x00505ae8, S_HACK_CAR = 0x00505afc,
    S_SEC_GAME = 0x004db2e8,         // const char*: "GAME"
    // gxdash.obj
    S_LOD = 0x004e53bc,              // float gxLODFactor
    S_GXD_BACK = 0x00505b48, S_GXD_TEXT = 0x00505b58, S_GXD_HELP = 0x00505b44, S_GXD_ON = 0x00505b7c, S_GXD_OFF = 0x00505b70,
    // elsewhere
    S_DEITY = 0x005218ac,            // Deity*
    S_LOCALE = 0x00509354,           // const LocaleInfo*: +8 distance unit, +0xc its factor, +0x18 speed unit, +0x1c factor
    S_TIME_STRING = 0x005215d8,      // PhysicsTimeString's buffer (0x24)
    S_MAIN_DONE = 0x00504068,        // main.obj: MainIsDone's flag
    S_MAIN_CAMERA = 0x004e3764,      // main.obj: MainSetCamera's
    S_PAUSED = 0x00521608,           // PhysicsIsPaused's
    S_CARMGR = 0x00554090,           // CarMgrInfo[16]
};

// ---- the game's functions (v1.0) ----------------------------------------------------------------------------------------
enum : uint32_t {
    // dash.obj
    F_DashboardBegin = 0x00403460, F_DashboardEnd = 0x00403720, F_DashboardReset = 0x00403840,
    F_DashboardUpdate = 0x00403850, F_clear_rect = 0x004046a0, F_clear_always_dirty_rects = 0x00404730,
    F_DashboardAlwaysUpdate = 0x00404820, F_DashboardMustDraw = 0x00404a80, F_show_paused = 0x00404ad0,
    F_no_banners = 0x00404b00, F_show_countdown = 0x00404b20, F_show_resetcar = 0x00404b50, F_show_damaged = 0x00404b70,
    F_show_award = 0x00404b90, F_show_summary = 0x00404bb0, F_show_loading = 0x00404bc0,
    F_DashboardDrawDashNone = 0x00404bd0, F_TheRealDashboardDraw = 0x00404be0, F_copy_rects = 0x00405370,
    F_draw_fps = 0x004054a0, F_DashboardDraw = 0x00405540, F_DashboardDraw3D = 0x00405550,
    F_draw_analog_dial = 0x00405780,
    // countdwn.obj
    F_CountdownBegin = 0x0040ac90, F_CountdownDraw = 0x0040aca0, F_CountdownEnd = 0x0040acb0,
    // escape.obj
    F_EscapeMenuBegin = 0x0040c520, F_EscapeMenuEnd = 0x0040c540, F_EscapeMenuActive = 0x0040c560,
    F_EscapeMenuDraw = 0x0040c5a0, F_EscapeMenuKey = 0x0040c660, F_EscapeMenuDo = 0x0040c730, F_do_callback = 0x0040c780,
    // splash.obj
    F_SplashGeneric = 0x0040c9e0, F_generic_splash = 0x0040ca00, F_grab_screen = 0x0040cb60,
    F_release_screen = 0x0040cb90, F_SplashButDontGrabScreen = 0x0040cba0, F_SplashLoading = 0x0040cbc0,
    F_SplashDoneLoading = 0x0040cc40, F_SplashVictory = 0x0040cc70, F_wait_key_timeout = 0x0040cce0,
    F_mouse_hit = 0x0040cd50, F_SplashLoser = 0x0040cda0, F_SplashDNF = 0x0040ce20, F_SplashSeriesVictory = 0x0040cea0,
    F_SplashSeriesLoser = 0x0040cf20, F_SplashGenericStamp = 0x0040cfa0, F_SplashMustSeeStamp = 0x0040cfd0,
    // hack.obj
    F_HackDisable = 0x0040d350, F_HackRestore = 0x0040d370, F_HackBegin = 0x0040d380, F_HackRefresh = 0x0040d3c0,
    F_HackEnd = 0x0040d4a0, F_HackEnabled = 0x0040d4b0, F_HackShowCarStatusInfo = 0x0040d4c0,
    F_HackThrottleBoost = 0x0040d4e0, F_HackGripBoost = 0x0040d500, F_HackGravityFactor = 0x0040d520,
    F_HackHornBall = 0x0040d540, F_HackNoWalls = 0x0040d560, F_HackPaveTheWorld = 0x0040d580,
    F_HackGetCarIndex = 0x0040d5a0,
    // gxdash.obj
    F_GXDashboardDraw = 0x0040d7e0, F_GXDashboardKey = 0x0040d8c0, F_flip_option = 0x0040d9b0,
    // the rest of root (groups B, C)
    F_MainIsDone = 0x00401d50, F_MainSetCamera = 0x00401d60, F_GetCarFileNumber = 0x00406930,
    F_GhostCarIncognito = 0x0040e940,
    // elsewhere
    F_Win32Idle = 0x00412bf0, F_TaskSleep = 0x00414de0, F_SingleEnter = 0x00415000, F_SingleLeave = 0x00415070,
    F_AICarCount = 0x0041d330, F_RecordGetRaceTime = 0x0042a860, F_PhysicsGetTime = 0x0042bc80,
    F_PhysicsIsPaused = 0x0042bd20, F_PhysicsTimeString = 0x0042bf30, F_mrBeginAlphaRects = 0x0044e990,
    F_mrEndAlphaRects = 0x0044e9c0, F_mrDrawAlphaRect = 0x0044e9f0, F_gxTriangle = 0x00450760,
    F_VidWasLost = 0x00454900, F_mrEnable = 0x00457890, F_mrDisable = 0x00457b00, F_mrIsEnabled = 0x00457d80,
    F_WorldGetRaceState = 0x00462060, F_WorldGetFocusCar = 0x004626d0, F_WorldGetCameraType = 0x004626e0,
    F_WorldGetPlayerCar = 0x00462710, F_WorldGameOptions = 0x004627a0, F_CarMgrCount = 0x00464480,
    F_CarMgrGetInfo = 0x00464490, F_OptionsGetF = 0x00471350, F_OptionsGetI = 0x004713e0, F_OptionsSetI = 0x00471430,
    F_OptionsGetB = 0x00471470, F_TelemetrySetAddMeter = 0x00471b70, F_UIAddDynamicStyle = 0x0047f4a0,
    F_MultiEnabled = 0x004a23c0,
};
// the Deity's virtual methods (physics: deity.obj and its subclasses)
enum : uint32_t { VO_Deity_GetNumLaps = 0x20, VO_Deity_CarIsFinished = 0x40 };

// typed pointers for the variadic ones and those taking a float by its bits
typedef void(__cdecl* FontPrintf_v)(void*, void*, uint32_t, int32_t, int32_t, const char*, ...);
#define RH_gxFontPrintf ((FontPrintf_v)(uintptr_t)uit::F_gxFontPrintf)
typedef int(__cdecl* Sprintf_v)(char*, const char*, ...);
#define RH_sprintf ((Sprintf_v)(uintptr_t)uit::F_sprintf)
typedef const char*(__cdecl* TimeString_t)(uint32_t, uint32_t);          // (float t, by its bits; unsigned char)
#define RH_PhysicsTimeString ((TimeString_t)(uintptr_t)F_PhysicsTimeString)

// ---- helpers ----------------------------------------------------------------------------------------------------------
#define RH_G8(a) UI_G8(a)
#define RH_G16(a) UI_G16(a)
#define RH_G32(a) UI_G32(a)
#define RH_GU32(a) UI_GU32(a)
#define RH_GF(a) UI_GF(a)
#define RH_GP(a) UI_GP(void, a)
#define RH_CP(a) ((const char*)(uintptr_t)(a))
#define RH_VP(a) ((void*)(uintptr_t)(a))

// a function-local static Xlator, built the first time: the guard's bit set, the constructor, its destructor to atexit
static __forceinline void rh_xl_once(uint32_t guard, uint8_t bit, uint32_t xl, uint32_t key, uint32_t dtor) {
    if (!(RH_G8(guard) & bit)) {
        RH_G8(guard) = (uint8_t)(RH_G8(guard) | bit);
        uit::tcall<void*>(uit::F_Xlator_ctor, RH_VP(xl), RH_CP(key));
        uit::ccall<int>(uit::F_atexit, dtor);
    }
}
static __forceinline bool rh_xl_built(uint32_t guard, uint8_t bits) { return (RH_G8(guard) & bits) == bits; }
#define RH_FP_XL(f, a) UI_FP(f, a, 12, "a static Xlator")

}  // namespace rhud
