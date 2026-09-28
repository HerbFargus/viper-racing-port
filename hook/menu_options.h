// menu_options.h -- M3 UI stage, step U2 (group B): the layouts of moptions.obj's and mmixer.obj's controls, the menus'
// statics, and the helpers hook/menu_options.cpp, hook/menu_race.cpp and test/world_menu_options.cpp share.
//
// Recovered from the v1.0 disassembly: each size is the room the original's frame gives the object (MenuDoOptions
// builds all five tabs on its stack) or its allocation, static_assert'd. Every field is volatile: the rewrites read and
// write each one where the original does, in its order. The toolkit (UICustomControl, the widgets, UIDialogItem) is
// hook/ui_types.h's, read-only.
//
//   UICustomControl 0x18 (ui_types.h)       vtable, x, y, w, h, widget
//   MixerChooser 0x28 (mmixer.obj)           the sound driver's radio buttons (one per mixer)
//   GfxOptionsControl 0x48                   the Graphics tab
//   SoundOptionsControl 0xa8                 the Sound tab (a MixerChooser inside, at +0x80)
//   ControlTestControl 0x38                  the live test of the controls (inside ControlsOptionsControl, at +0x6c)
//   ControlsOptionsControl 0xa4              the Controls tab
//   AidsOptionsControl 0x28                  the Driving Aids tab
//   HackOptionsControl 0x48                  the Hacks tab (a UIStringList of the cars inside, at +0x28)
//   Meter 0x1c / SteerMeter 0x1c             adjust_ctls's three bars
//   GameOptionsB                             root's GameOptions: only the fields MenuDoRaceSetup touches
// Group C's classes that MenuDoRaceSetup builds on its stack (TrackChooser, CarChooser, OpponentViewer, RaceOptionViewer)
// are called by address (F_*) and never looked into, but for RaceOptionViewer's vtable, which its inlined destructor
// puts back.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"

namespace mob {
using namespace uit;

// ---- the classes ----------------------------------------------------------------------------------------------------------
struct Control8 { volatile uint32_t a, b; };      // physics' struct Control (8 bytes: ControlGet / ControlSet)

struct MixerChooser : UICustomControl {           // 0x28, vtable 0x4def70
    int32_t* volatile sel;                        // +0x18 the mixer chosen (SoundOptionsControl +0x24)
    volatile int32_t cur;                         // +0x1c the one in force
    uint32_t* volatile grp_caps;                  // +0x20 a group shown when the mixer's caps' first byte is set
    uint32_t* volatile grp_real;                  // +0x24 a group shown unless it's the null mixer
};
static_assert(sizeof(MixerChooser) == 0x28, "MixerChooser");

struct GfxOptionsControl : UICustomControl {      // 0x48, vtable 0x4de4b0 (the options' GX section)
    volatile int32_t video_mode;                  // +0x18
    volatile uint8_t filtering;                   // +0x1c
    uint8_t _1d[3];
    volatile float draw_distance;                 // +0x20
    volatile float detail_level;                  // +0x24
    volatile uint8_t sky, road, car, wall;        // +0x28 textures
    volatile int32_t smoke;                       // +0x2c
    volatile int32_t shadow;                      // +0x30
    volatile int32_t skids;                       // +0x34
    volatile uint8_t fog, lighting;               // +0x38
    uint8_t _3a[2];
    volatile int32_t specular;                    // +0x3c
    volatile uint8_t mipmap;                      // +0x40
    uint8_t _41[3];
    volatile int32_t mirror;                      // +0x44
};
static_assert(sizeof(GfxOptionsControl) == 0x48, "GfxOptionsControl");

struct SoundOptionsControl : UICustomControl {    // 0xa8, vtable 0x4de500 (the SOUND section)
    volatile uint32_t grp_caps;                   // +0x18 MixerChooser's groups
    volatile uint32_t grp_real;                   // +0x1c
    volatile uint8_t cd_audio;                    // +0x20
    volatile uint8_t spotter;                     // +0x21
    uint8_t _22[2];
    volatile int32_t mixer;                       // +0x24
    volatile float volume;                        // +0x28
    volatile float music_volume;                  // +0x2c
    volatile float quality;                       // +0x30 (an int, as a float for the slider)
    volatile float fidelity;                      // +0x34 0 (8-bit) or 1 (16-bit)
    char quality_text[0x20];                      // +0x38
    char fidelity_text[0x20];                     // +0x58
    volatile float volume_set;                    // +0x78 the volume last handed to the mixer
    volatile uint8_t restart;                     // +0x7c the sound must restart
    uint8_t _7d[3];
    MixerChooser mixers;                          // +0x80
};
static_assert(sizeof(SoundOptionsControl) == 0xa8 && offsetof(SoundOptionsControl, mixers) == 0x80, "SoundOptionsControl");

struct ControlTestControl : UICustomControl {     // 0x38, vtable 0x4de550
    volatile float steer;                         // +0x18
    volatile float throttle;                      // +0x1c
    volatile float brake;                         // +0x20
    volatile float clutch;                        // +0x24
    volatile float timer;                         // +0x28 the controls saved every 0.25 s
    void* volatile ball;                          // +0x2c 'ball.stp'
    void* volatile back;                          // +0x30 'control.stp'
    struct ControlsOptionsControl* volatile parent;   // +0x34
};
static_assert(sizeof(ControlTestControl) == 0x38, "ControlTestControl");

struct ControlsOptionsControl : UICustomControl { // 0xa4, vtable 0x4de5a0 (the CONTROL section)
    volatile uint8_t force_feedback;              // +0x18
    volatile uint8_t game_pad;                    // +0x19
    uint8_t _1a[2];
    Control8 ctl[10];                             // +0x1c steer left, right, throttle, braking, e-brake, reverse, clutch,
                                                  //       upshift, downshift, horn
    ControlTestControl test;                      // +0x6c
};
static_assert(sizeof(ControlsOptionsControl) == 0xa4 && offsetof(ControlsOptionsControl, test) == 0x6c, "ControlsOptionsControl");

struct AidsOptionsControl : UICustomControl {     // 0x28, vtable 0x4de5f0 (the CONTROL section)
    volatile uint8_t auto_shifting;               // +0x18
    volatile uint8_t auto_clutch;                 // +0x19 (the constructor leaves it as it found it)
    uint8_t _1a[2];
    volatile int32_t traction;                    // +0x1c
    volatile int32_t abs;                         // +0x20
    volatile int32_t yaw;                         // +0x24
};
static_assert(sizeof(AidsOptionsControl) == 0x28, "AidsOptionsControl");

struct HackOptionsControl : UICustomControl {     // 0x48, vtable 0x4de640 (the GAME section)
    volatile uint8_t show_status;                 // +0x18
    volatile uint8_t horn_ball;                   // +0x19
    volatile uint8_t no_walls;                    // +0x1a
    volatile uint8_t pave;                        // +0x1b
    volatile float throttle_boost;                // +0x1c
    volatile float grip_boost;                    // +0x20
    volatile float gravity;                       // +0x24
    UIStringList cars;                            // +0x28
    volatile int32_t car_index;                   // +0x3c
    Control8 help;                                // +0x40
};
static_assert(sizeof(HackOptionsControl) == 0x48 && offsetof(HackOptionsControl, cars) == 0x28, "HackOptionsControl");

struct Meter : UICustomControl {                  // 0x1c, vtable 0x4de690 (SteerMeter 0x4de6e0)
    const float* volatile value;                  // +0x18
};
static_assert(sizeof(Meter) == 0x1c, "Meter");

// root's GameOptions (front end): the fields MenuDoRaceSetup reads and writes (it clears the first 0x28 bytes)
struct GameOptionsB {
    volatile int32_t realism;                     // +0
    volatile int32_t time_of_day;                 // +4
    volatile int32_t weather;                     // +8
    volatile int32_t field;                       // +0xc
    volatile int32_t race_type;                   // +0x10
    volatile int32_t _14;
    volatile int32_t _18;                         // +0x18 (0 here; nonzero clears +0x1c on the way out)
    volatile int32_t _1c;
    volatile int32_t opponent_strength;           // +0x20
    volatile uint8_t damage_on;                   // +0x24
    volatile uint8_t is_reversed;                 // +0x25
};
static_assert(offsetof(GameOptionsB, opponent_strength) == 0x20 && offsetof(GameOptionsB, damage_on) == 0x24, "GameOptionsB");

// ---- vtables ---------------------------------------------------------------------------------------------------------------
enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_GfxOptions = 0x004de4b0, VT_SoundOptions = 0x004de500,
    VT_ControlTest = 0x004de550, VT_ControlsOptions = 0x004de5a0, VT_AidsOptions = 0x004de5f0,
    VT_HackOptions = 0x004de640, VT_Meter = 0x004de690, VT_SteerMeter = 0x004de6e0, VT_MixerChooser = 0x004def70,
};

// ---- the game's functions (v1.0) -------------------------------------------------------------------------------------------
enum : uint32_t {
    // root (the front end)
    F_GetTrackCount = 0x00406800, F_GetTrackName = 0x00406810, F_GetCarFileName = 0x00406910,
    F_GetMaxCarFileNames = 0x00406920, F_GetCarFileNumber = 0x00406930, F_ReplayLoadDialog = 0x004078d0,
    F_ReplayBenchmark = 0x00408f90, F_HackRefresh = 0x0040d3c0, F_HackEnabled = 0x0040d4b0,
    F_HackGetCarIndex = 0x0040d5a0,
    // kernel
    F_VersionGetBuildString = 0x00410cb0, F_JoyGetName = 0x00418fd0,
    // physics: the driver's controls
    F_DriverBegin = 0x00441490, F_DriverEnd = 0x00441550, F_DriverRefresh = 0x00441560, F_DriverUpdate = 0x004417c0,
    F_DriverGetSteering = 0x00441df0, F_DriverGetThrottle = 0x00441e60, F_DriverGetBraking = 0x00441e70,
    F_DriverGetClutch = 0x00441e80, F_DriverIsSteeringDigital = 0x00441f60, F_DriverIsBrakeDigital = 0x00441fa0,
    F_DriverIsThrottleDigital = 0x00441fc0, F_ControlGet = 0x0044b430, F_ControlSet = 0x0044b480,
    // gx
    F_VidIsModeSupported = 0x00454910, F_VidHas3DHW = 0x00454930, F_VidGetMegsRam = 0x00454940,
    // state: the options
    F_OptionsLoadModule = 0x004710e0, F_OptionsFlush = 0x00471110, F_OptionsGetF = 0x00471350,
    F_OptionsSetF = 0x004713a0, F_OptionsGetI = 0x004713e0, F_OptionsSetI = 0x00471430, F_OptionsGetB = 0x00471470,
    F_OptionsSetB = 0x004714c0, F_OptionsGetS = 0x00471500, F_OptionsSetS = 0x00471560,
    // sound
    F_SoundRestart = 0x00471d60, F_SoundToss = 0x004724c0, F_MixerCount = 0x00473840, F_MixerGet = 0x00473850,
    F_MixerSetDefault = 0x00473860, F_MixerGetDefault = 0x00473870, F_MixerGetVolume = 0x004739d0,
    F_MixerSetVolume = 0x004739e0, F_MixerSetQuality = 0x00473a20, F_MixerGetQualityString = 0x00473a40,
    F_MixerGetQuality = 0x00473b70,
    // ui
    F_UIDoYesNoBox = 0x00479b30, F_CreateMultiString = 0x0047b410, F_UICC_Dirty = 0x0047e820,
    F_UICC_AddNotification = 0x0047e840, F_UICC_RemoveNotification = 0x0047e860, F_UICC_AddItems = 0x0047e870,
    // paintkit
    F_PaintKitDo = 0x004c62d0,
    // the CRT
    F_strnicmp = 0x004da3e0,
    // moptions.obj (called by address, as the original does, so a hooked rewrite is what runs)
    F_CTC_FinalizeParent = 0x004818c0, F_run_benchmark = 0x00482080, F_adjust_ctls = 0x004821e0,
    F_adjust_idle = 0x00482ce0, F_get_control_tweaks = 0x00482e00, F_tune_default = 0x00482ed0,
    F_Gfx_ctor = 0x00483050, F_Gfx_Finalize = 0x00483eb0, F_Sound_ctor = 0x00484010, F_CTC_dtor = 0x00484a80,
    F_Controls_ctor = 0x00484dd0, F_default_ctl = 0x004858b0, F_Controls_Finalize = 0x00485960,
    F_Aids_ctor = 0x00485aa0, F_Aids_Finalize = 0x00485fa0, F_Hack_ctor = 0x00486040, F_Hack_Finalize = 0x00486170,
    // mrace.obj
    F_DoFlyout = 0x00487950, F_replay_cb = 0x004879c0, F_paintkit_cb = 0x004879d0,
    // mmixer.obj
    F_MixerChooser_ctor = 0x0049a950,
    // group C's classes (trkview / carview / oppview / optview), by address
    F_TrackViewer_GetTrackName = 0x0049b210, F_TrackChooser_ctor = 0x0049b2d0, F_TrackChooser_dtor = 0x0049b320,
    F_CarViewer3D_GetName = 0x0049ce90, F_CarChooser_ctor = 0x0049d250, F_CarChooser_dtor = 0x0049d2c0,
    F_OpponentViewer_ctor = 0x0049e2e0, F_OpponentViewer_dtor = 0x0049e370, F_OpponentViewer_SetCar = 0x0049eaa0,
    F_RaceOptionViewer_ctor = 0x0049f120,
};

// ---- statics (v1.0) --------------------------------------------------------------------------------------------------------
enum : uint32_t {
    // the options' section names (char* each)
    S_SEC_GLOBAL = 0x004de470, S_SEC_GX = 0x004de474, S_SEC_SOUND = 0x004de478, S_SEC_CONTROL = 0x004de47c,
    S_SEC_GAME = 0x004de480, S_SEC_RACE_GLOBAL = 0x004de730, S_SEC_RACE_GAME = 0x004de734,
    S_EMPTY = 0x004e4db8,                         // ""
    // moptions.obj
    S_GFX_OPTIONS = 0x004f72e0,                   // GfxOptionsControl*: MenuDoOptions's (run_benchmark saves it)
    S_BENCH_FPS = 0x004f72dc,                     // float: the benchmark's frames per second
    S_CONTROLS_OPTIONS = 0x00579b0c,              // ControlsOptionsControl*: MenuDoOptions's (adjust_ctls saves it)
    S_CONTROLS_GLOBAL = 0x00579964,               // ControlsOptionsControl::global (default_ctl's)
    S_HACK_GROUP = 0x00579a5c,                    // the Hacks tab's group
    S_OPTIONS_ONCE = 0x005797cc,                  // bit 0: MenuDoOptions's title Xlator
    S_ADJUST_ONCE = 0x00579c08, S_ADJUST_ONCE2 = 0x00579ba0,   // adjust_ctls's Xlators
    S_TUNE_ONCE = 0x00579ca4,                     // tune_default's
    S_SOUND_STR_ONCE = 0x005d5c4c,                // update_sound_strings's (8-bit / 16-bit)
    S_SOUND_ADDED_ONCE = 0x005d5c0c,              // SoundOptionsControl::Added's
    S_HACK_ADDED_ONCE = 0x005d5bac,               // HackOptionsControl::Added's
    S_MIRROR_OPTS = 0x00579800, S_SPECULAR_OPTS = 0x00579cb8,  // multi strings (Gfx / Aids)
    S_AIDS_OPTS1 = 0x005797e0, S_AIDS_OPTS2 = 0x00579d08, S_AIDS_OPTS3 = 0x00579b80,
    // the adjust dialog's values (floats, a byte)
    S_ADJ_THROTTLE_SENS = 0x00579900, S_ADJ_THROTTLE_RANGE = 0x00579830, S_ADJ_THROTTLE_SPEED = 0x00579854,
    S_ADJ_NONLINEAR = 0x00579c1c, S_ADJ_STEER_SENS = 0x00579d44, S_ADJ_STEER_RANGE = 0x005799e4,
    S_ADJ_STEER_SPEED = 0x00579a2c, S_ADJ_BRAKE_SENS = 0x00579904, S_ADJ_BRAKE_RANGE = 0x005797c4,
    S_ADJ_BRAKE_SPEED = 0x0057982c, S_ADJ_834 = 0x00579834,
    S_ADJ_THROTTLE = 0x00579c04, S_ADJ_BRAKE = 0x00579c20, S_ADJ_STEER = 0x00579864,   // the meters' values
    // colours (the $E initialisers')
    S_COL_974 = 0x00579974, S_COL_BF4 = 0x00579bf4,
    // mrace.obj
    S_RACEMENU_ONCE = 0x00579eb0, S_RACEMENU_DLG_ONCE = 0x00579da4, S_RACEMENU_DLG = 0x004f8100,
    S_RACESETUP_ONCE = 0x00579d88, S_RACESETUP_DF4 = 0x00579df4, S_RACESETUP_TRACK = 0x00579e50,
    S_RACESETUP_CAR = 0x00579db0,
};

// ---- memory --------------------------------------------------------------------------------------------------------------------
#define MO_G8(a) UI_G8(a)
#define MO_G32(a) UI_GU32(a)
#define MO_GF(a) UI_GF(a)
static __forceinline uint32_t U(const volatile void* p) { return (uint32_t)(uintptr_t)p; }

// an Xlator refreshed (the cookie compared, Xlator::xlate called when stale), no value read
static __forceinline void xl(uint32_t x) {
    if (UI_GU32(x + 8) != UI_GU32(S_XLATOR_COOKIE)) tcall<void>(F_Xlator_xlate, (void*)(uintptr_t)x);
}
// a function-local static Xlator's guard: `test [guard], bit; jne; or ...; Xlator::Xlator; atexit(helper)`
static __forceinline void once_xl(uint32_t guard, uint8_t bit, uint32_t x, uint32_t key, uint32_t helper) {
    const uint8_t g = UI_G8(guard);
    if (g & bit) return;
    UI_G8(guard) = (uint8_t)(g | bit);
    tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)x, (const char*)(uintptr_t)key);
    ccall<int>(F_atexit, helper);
}
// the menus' colour-pair test for a text style: (a == b) ? 0xb : 0xc (`sub; cmp 1; sbb; add 0xc`)
static __forceinline uint32_t sty(uint32_t a, uint32_t b) { return UI_GU32(a) - UI_GU32(b) == 0 ? 0xbu : 0xcu; }
static __forceinline uint32_t sty1(uint32_t a) { const uint32_t v = UI_GU32(a); return v - v == 0 ? 0xbu : 0xcu; }

// UIDialogItem::UIDialogItem (replay.obj), by address: all 14 dwords as the original pushes them
static __forceinline void item_ctor(void* it, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    uint32_t text, uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo, uint32_t hi,
                                    uint32_t sel, uint32_t s34) {
    tcall<void*>(F_UIDialogItem_ctor, it, type, id, x, y, w, h, text, i1c, data, style, lo, hi, sel, s34);
}
// the end of a list: the static item at 0x578d08, copied (rep movsd)
static __forceinline void item_end(void* it) { crt_copy(it, (const void*)(uintptr_t)S_END_ITEM, 0x38); }
static __forceinline void add_items(void* self, void* items) { tcall<void>(F_UICC_AddItems, self, items); }

// the options, by address (a float travels as its bits, as the original pushes it)
static __forceinline void opt_get_f(const char* sec, uint32_t key, volatile void* p) { ccall<void>(F_OptionsGetF, sec, (const char*)(uintptr_t)key, (void*)p); }
static __forceinline void opt_get_i(const char* sec, uint32_t key, volatile void* p) { ccall<void>(F_OptionsGetI, sec, (const char*)(uintptr_t)key, (void*)p); }
static __forceinline void opt_get_b(const char* sec, uint32_t key, volatile void* p) { ccall<void>(F_OptionsGetB, sec, (const char*)(uintptr_t)key, (void*)p); }
static __forceinline void opt_set_f(const char* sec, uint32_t key, uint32_t bits) { ccall<void>(F_OptionsSetF, sec, (const char*)(uintptr_t)key, bits); }
static __forceinline void opt_set_i(const char* sec, uint32_t key, int32_t v) { ccall<void>(F_OptionsSetI, sec, (const char*)(uintptr_t)key, v); }
// a byte argument: the original pushes the whole register (`mov al, [x]; push eax`), whose upper bytes are whatever eax
// held; the callee reads only the byte
static __forceinline void opt_set_b(const char* sec, uint32_t key, uint8_t v) { ccall<void>(F_OptionsSetB, sec, (const char*)(uintptr_t)key, (uint32_t)v); }
static __forceinline void control_get(volatile void* c, uint32_t key) { ccall<void>(F_ControlGet, (void*)c, (const char*)(uintptr_t)key); }
static __forceinline void control_set(const volatile void* c, uint32_t key) { ccall<void>(F_ControlSet, (const void*)c, (const char*)(uintptr_t)key); }
static __forceinline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// ---- footprints ----------------------------------------------------------------------------------------------------------
// the current canvas made `c`, and what a draw clipped to it writes
static void mo_fp_draw(Footprint& f, const void* c) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}
// a UICustomControl's widget (Dirty: the widget's flag)
static void mo_fp_dirty(Footprint& f, const UICustomControl* c) {
    if (c && c->widget) f.add((void*)c->widget, sizeof(CustomWidget), "its widget");
}

}  // namespace mob
