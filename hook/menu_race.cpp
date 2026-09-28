// menu_race.cpp -- M3 UI stage, step U2 (group B): mrace.obj, rewritten faithfully (library `menu`).
//
//   MenuDoRaceMenu (the main menu: Single Race, Career, Multiplayer, Options, Replay, Quit, the paint kit's hot key, the
//   player's name, the build string), MenuDoRaceSetup (the single race's setup: the car and track choosers, the
//   opponents and the race options -- group C's classes, called by address -- and GameOptions read from and written back
//   to the options), and the main menu's two callbacks, replay_cb and paintkit_cb. (DoFlyout, a bare `ret`, is generated:
//   hook/menu_leftover.cpp.)
//
// Written from the v1.0 disassembly, as hook/menu_options.cpp: every call in the original's order by its v1.0 address,
// item lists in a frame laid out as the original's (its offsets from the top), UIDialogItem's constructor called where
// the original calls it and every dword stored where it stores an item inline.
//
// Footprints (main thread): all replay_only -- the two menus run dialogs (modal) and read and write the options (and the
// setup builds and destroys the choosers, which load cars and tracks); replay_cb runs the replay loader's dialog;
// paintkit_cb the paint kit.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "menu_options.h"

namespace {
namespace menu_race {
using namespace uit;
using namespace mob;

template <uint32_t N> struct Frame {
    alignas(4) uint8_t b[N];
    __forceinline void* at(uint32_t n) { return b + N - n; }
    __forceinline volatile uint32_t& d(uint32_t n) { return *(volatile uint32_t*)(b + N - n); }
    __forceinline volatile uint8_t& c(uint32_t n) { return *(volatile uint8_t*)(b + N - n); }
};
#define G(a) UI_GU32(a)
#define IC(n, ...) item_ctor(fr.at(n), __VA_ARGS__)
static __forceinline void raw(void* p, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t text,
                              uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo, uint32_t hi, uint32_t sel, uint32_t s34) {
    volatile uint32_t* d = (volatile uint32_t*)p;
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text;
    d[7] = i1c; d[8] = data; d[9] = style; d[10] = lo; d[11] = hi; d[12] = sel; d[13] = s34;
}
#define RAW(n, ...) raw(fr.at(n), __VA_ARGS__)

// =====================================================================================================================
// MenuDoRaceMenu
// =====================================================================================================================
// Its frame: 0x438 the player's name (16 bytes), 0x428 the items (18 and the end). The dialog is the static at 0x4f8100
// (no title, main.stp, no default), whose item list is set once, to the first call's frame.
// FIX CANDIDATE: that list pointer (0x4f810c) is never set again -- a later call from a deeper or shallower stack would
// run the old frame's contents; the game calls it from one place, at one depth.
// FIX CANDIDATE: the default name (the Xlator's text) is copied into the 16-byte buffer unbounded; a translation longer
// than 15 characters runs into the item list, which is built over it afterwards (the stock text is short).
static int32_t __cdecl MenuDoRaceMenu_n() {
    Frame<0x438> fr;
    once_xl(S_RACEMENU_ONCE, 1, 0x00579e20, 0x004f8114, 0x004871d0);
    once_xl(S_RACEMENU_ONCE, 2, 0x00579d90, 0x004f8130, 0x004871c0);
    once_xl(S_RACEMENU_ONCE, 4, 0x00579eb8, 0x004f8148, 0x004871b0);
    once_xl(S_RACEMENU_ONCE, 8, 0x00579df8, 0x004f8160, 0x004871a0);
    once_xl(S_RACEMENU_ONCE, 0x10, 0x00579e40, 0x004f8174, 0x00487190);
    once_xl(S_RACEMENU_ONCE, 0x20, 0x00579f00, 0x004f818c, 0x00487180);
    once_xl(S_RACEMENU_ONCE, 0x40, 0x00579e10, 0x004f81a0, 0x00487170);
    once_xl(S_RACEMENU_ONCE, 0x80, 0x00579d68, 0x004f81b4, 0x00487160);
    xl(0x00579e20);
    crt_strcpy((char*)fr.at(0x438), UI_GP(const char, 0x00579e24));
    const char* global = UI_GP(const char, S_SEC_RACE_GLOBAL);
    ccall<void>(F_OptionsGetS, global, (const char*)0x004f81c4, (char*)fr.at(0x438), (int32_t)0xd);
    IC(0x428, 0xe, 0, 0x10, 0xe, 0, 0, 0x004f81d0, 0, 0, 0, 0, 0, 0, 0);
    xl(0x00579eb8);
    RAW(0x3f0, 2, 0, 0x1ef, 0x8e, 0, 0, G(0x00579ebc), 0, 0, 2, 0, 0, 0, 0);
    xl(0x00579df8);
    IC(0x3b8, 2, 2, 0x1ef, 0xbf, 0, 0, G(0x00579dfc), 0, 0, 2, 0, 0, 0, 0);
    IC(0x380, 4, 2, 0x63, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    IC(0x348, 4, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    IC(0x310, 4, 7, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    IC(0x2d8, 4, 0, 0x70, 0, 0, 0, 0, 0, F_paintkit_cb, 0, 0, 0, 0, 0);
    xl(0x00579e40);
    RAW(0x2a0, 2, 1, 0x1ef, 0xf0, 0, 0, G(0x00579e44), 0, 0, 2, 0, 0, 0, 0);
    xl(0x00579f00);
    IC(0x268, 2, 4, 0x1ef, 0x121, 0, 0, G(0x00579f04), 0, F_replay_cb, 2, 0, 0, 0, 0);
    xl(0x00579e10);
    RAW(0x230, 2, 3, 0x1ef, 0x152, 0, 0, G(0x00579e14), 0, 0, 2, 0, 0, 0, 0);
    IC(0x1f8, 4, 3, 0x6f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    xl(0x00579d68);
    IC(0x1c0, 2, (uint32_t)-1, 0x13, 0x1a2, 0, 0, G(0x00579d6c), 0, 0, 6, 0, 0, 0, 0);
    IC(0x188, 4, 0, 0xd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    IC(0x150, 0xe, 0, 0x63, 0x1b7, 0, 0, 0x004f81dc, 0, 0, 0, 0, 0, 0, 0);
    IC(0x118, 9, 0, 0x67, 0x1ba, 0x78, 0x10, 1, (uint32_t)-0xd, U(fr.at(0x438)), 9, 0, 0, 0, 0);
    const char* build = ccall<const char*>(F_VersionGetBuildString);
    IC(0xe0, 5, 0, 0x14, 0x46, 0, 0, U(build), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0xa8, 5, 0, 0x32, 0x169, 0, 0, 0x004f81e8, 0, 0, 0xb, 0, 0, 0, 0);
    IC(0x70, 5, 0, 0x32, 0x178, 0, 0, 0x004f8200, 0, 0, 0xc, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    {
        const uint8_t g = UI_G8(S_RACEMENU_DLG_ONCE);
        if (!(g & 1)) {
            UI_G8(S_RACEMENU_DLG_ONCE) = (uint8_t)(g | 1);
            UI_GU32(S_RACEMENU_DLG + 0xc) = U(fr.at(0x428));
            UI_GU32(S_RACEMENU_DLG + 0x10) = 0;
        }
    }
    const int32_t r = ccall<int32_t>(F_UIDoDialog, (const void*)(uintptr_t)S_RACEMENU_DLG, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H),
                                     (int32_t)-999, (int32_t)-999, (int32_t)1);
    ccall<void>(F_OptionsSetS, global, (const char*)0x004f8260, (const char*)fr.at(0x438));
    if (r < -2 || r > -1) return r;
    return 8;
}
static void fp_MenuDoRaceMenu(Footprint& f) { f.replay_only = "the main menu: runs a dialog (modal), reads and writes the options"; }
PORT_FN(0x00486aa0, "MenuDoRaceMenu", MenuDoRaceMenu_n, fp_MenuDoRaceMenu)

// =====================================================================================================================
// MenuDoRaceSetup
// =====================================================================================================================
// Its frame: 0x592 the track chooser's flag, 0x591 the result, 0x590 the car count, 0x58c the UIDialog, 0x578 the items
// (6 and the end), 0x3f0 the TrackChooser (its TrackViewer at 0x3d8), 0x394 a 32-byte text (RaceOptionViewer's),
// 0x374 the RaceOptionViewer, 0x304 the OpponentViewer, 0x200 the CarChooser (its CarViewer3D at 0x118).
static uint8_t __cdecl MenuDoRaceSetup_n(char* car, char* track, void* game) {
    Frame<0x594> fr;
    GameOptionsB* go = (GameOptionsB*)game;
    UI_GU32(S_RACESETUP_DF4) = 1;
    fr.c(0x592) = 1;
    tcall<void*>(F_TrackChooser_ctor, fr.at(0x3f0), (uint32_t)1, fr.at(0x592), (char*)(uintptr_t)S_RACESETUP_TRACK, game);
    int32_t carno = ccall<int32_t>(F_GetCarFileNumber, (const char*)0x004f826c);
    if (ccall<uint8_t>(F_HackEnabled)) carno = ccall<int32_t>(F_HackGetCarIndex);
    tcall<void*>(F_CarChooser_ctor, fr.at(0x200), (uint32_t)0, (char*)(uintptr_t)S_RACESETUP_CAR, carno, (uint32_t)0,
                 (uint32_t)F_GetMaxCarFileNames, (uint32_t)F_GetCarFileName);
    tcall<void*>(F_OpponentViewer_ctor, fr.at(0x304), game, fr.at(0x590));
    tcall<void*>(F_RaceOptionViewer_ctor, fr.at(0x374), fr.at(0x590), (void*)((uint8_t*)game + 0x20), game, fr.at(0x592),
                 fr.at(0x394));
    tcall<void>(F_OpponentViewer_SetCar, fr.at(0x304), (const char*)0x004f8274);
    go->opponent_strength = 0;
    for (int i = 0; i < 10; i++) ((volatile uint32_t*)game)[i] = 0;     // rep stosd
    const char* s = UI_GP(const char, S_SEC_RACE_GAME);
    opt_get_i(s, 0x004f827c, &go->realism);
    opt_get_b(s, 0x004f8284, &go->damage_on);
    opt_get_b(s, 0x004f8290, &go->is_reversed);
    opt_get_i(s, 0x004f829c, &go->time_of_day);
    opt_get_i(s, 0x004f82a8, &go->weather);
    opt_get_i(s, 0x004f82b0, &go->race_type);
    opt_get_i(s, 0x004f82bc, &go->opponent_strength);
    opt_get_i(s, 0x004f82d0, &go->field);
    go->_18 = 0;
    if (!(UI_G8(S_RACESETUP_ONCE) & 1)) {
        UI_G8(S_RACESETUP_ONCE) = (uint8_t)(UI_G8(S_RACESETUP_ONCE) | 1);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x00579d78, (const char*)0x004f82d8);
        ccall<int>(F_atexit, (uint32_t)0x004879b0);
    }
    static const uint32_t xls[5][3] = {{0x00579ec8, 0x004f82ec, 0x004879a0}, {0x00579e30, 0x004f82fc, 0x00487990},
                                       {0x00579ea0, 0x004f830c, 0x00487980}, {0x00579e90, 0x004f831c, 0x00487970},
                                       {0x00579ed8, 0x004f832c, 0x00487960}};
    for (int k = 0; k < 5; k++) {
        const uint8_t bit = (uint8_t)(2u << k);
        if (!(UI_G8(S_RACESETUP_ONCE) & bit)) {
            UI_G8(S_RACESETUP_ONCE) = (uint8_t)(UI_G8(S_RACESETUP_ONCE) | bit);
            tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)xls[k][0], (const char*)(uintptr_t)xls[k][1]);
            ccall<int>(F_atexit, xls[k][2]);
        }
    }
    RAW(0x578, 0x17, 0, 0x19, 0x52, 0, 0, S_EMPTY, 0, U(fr.at(0x200)), 0, 0, 0, 0, 0);
    RAW(0x540, 0x17, 0, 0xe6, 0x52, 0, 0, S_EMPTY, 0, U(fr.at(0x3f0)), 0, 0, 0, 0, 0);
    RAW(0x508, 0x17, 0, 0x1b4, 0x52, 0xb3, 0x8d, S_EMPTY, 0, U(fr.at(0x304)), 0, 0, 0, 0, 0);
    RAW(0x4d0, 0x17, 0, 0xd2, 0, 0, 0, S_EMPTY, 0, U(fr.at(0x374)), 0, 0, 0, 0, 0);
    xl(0x00579e30);
    RAW(0x498, 2, (uint32_t)-2, 0x1e5, 0x1a5, 0, 0, G(0x00579e34), 0, 0, 2, 0, 0, 0, 0);
    xl(0x00579ed8);
    RAW(0x460, 2, (uint32_t)-1, 0x13, 0x1a2, 0, 0, G(0x00579edc), 0, 0, 6, 0, 0, 0, 0);
    item_end(fr.at(0x428));
    xl(0x00579d78);
    fr.d(0x58c) = G(0x00579d7c);
    fr.d(0x580) = U(fr.at(0x578));
    fr.d(0x588) = 0x004f8334;
    fr.d(0x584) = 0xfffffffeu;
    fr.d(0x57c) = 0;
    fr.c(0x591) = 0;
    ccall<void>(F_DoFlyout, (const char*)0x004f8340, (uint32_t)1);
    const int32_t r = ccall<int32_t>(F_UIDoDialog, fr.at(0x58c), UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H), (int32_t)-999,
                                     (int32_t)-999, (int32_t)1);
    if (r != -2) {
        ccall<void>(F_DoFlyout, (const char*)0x004f8354, (uint32_t)0);
    } else {
        crt_strcpy(car, tcall<const char*>(F_CarViewer3D_GetName, fr.at(0x118)));
        const char* name = tcall<const char*>(F_TrackViewer_GetTrackName, fr.at(0x3d8));
        if (ccall<int>(F_strnicmp, name, (const char*)0x004f834c, (uint32_t)6) == 0) {
            const int32_t t = ccall<int32_t>(F_PTimeNow);
            const int32_t n = ccall<int32_t>(F_GetTrackCount) - 2;
            // FIX CANDIDATE: a random track divides by the track count - 2 (a crash with only two tracks) and a
            // negative time gives a negative index; the stock game has more tracks, and PTimeNow starts at 0
            name = ccall<const char*>(F_GetTrackName, t % n);
        } else {
            name = tcall<const char*>(F_TrackViewer_GetTrackName, fr.at(0x3d8));
        }
        crt_strcpy(track, name);
        fr.c(0x591) = 1;
    }
    opt_set_i(s, 0x004f8360, go->field);
    opt_set_i(s, 0x004f8368, go->realism);
    opt_set_b(s, 0x004f8370, go->damage_on);
    opt_set_b(s, 0x004f837c, go->is_reversed);
    opt_set_i(s, 0x004f8388, go->time_of_day);
    opt_set_i(s, 0x004f8394, go->weather);
    opt_set_i(s, 0x004f839c, go->race_type);
    if (go->_18 != 0) go->_1c = 0;
    opt_set_i(s, 0x004f83a8, go->opponent_strength);
    opt_set_i(s, 0x004f83bc, (int32_t)fr.d(0x590));
    fr.d(0x374) = VT_UICustomControl;                                   // ~RaceOptionViewer, inlined
    tcall<void>(F_OpponentViewer_dtor, fr.at(0x304));
    tcall<void>(F_CarChooser_dtor, fr.at(0x200));
    tcall<void>(F_TrackChooser_dtor, fr.at(0x3f0));
    return fr.c(0x591);
}
static void fp_MenuDoRaceSetup(Footprint& f, char*, char*, void*) {
    f.replay_only = "the race setup: runs a dialog (modal), builds the choosers (loads cars and tracks), reads and writes the options";
}
PORT_FN(0x004871e0, "MenuDoRaceSetup", MenuDoRaceSetup_n, fp_MenuDoRaceSetup)

// the main menu's Replay button: the replay loader's dialog; a replay chosen ends the menu
static uint8_t __cdecl replay_cb_n(int32_t) { return ccall<uint8_t>(F_ReplayLoadDialog) < 1 ? 0 : 1; }
static void fp_replay_cb(Footprint& f, int32_t) { f.replay_only = "runs the replay loader's dialog (modal)"; }
PORT_FN(0x004879c0, "replay_cb", replay_cb_n, fp_replay_cb)

// the paint kit's hot key ('p'): the paint kit on the viper
static uint8_t __cdecl paintkit_cb_n(int32_t) {
    ccall<void>(F_PaintKitDo, (const char*)0x004f83c8, (int32_t)0);
    return 0;
}
static void fp_paintkit_cb(Footprint& f, int32_t) { f.replay_only = "runs the paint kit"; }
PORT_FN(0x004879d0, "paintkit_cb", paintkit_cb_n, fp_paintkit_cb)

#undef RAW
#undef IC
#undef G
}  // namespace menu_race
}  // namespace
