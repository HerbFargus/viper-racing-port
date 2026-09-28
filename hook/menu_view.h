// menu_view.h -- M3 UI stage, step U2 (group C): the choosers, viewers and boards' layouts (library `menu`: carview.obj,
// trkview.obj, optview.obj, oppview.obj, board.obj), shared by hook/menu_view.cpp, hook/menu_board.cpp and
// test/world_menu_view.cpp.
//
// Recovered from the v1.0 disassembly (the constructors, the Draw / Create / Added / Callback methods, MenuDoRaceSetup's
// frame, BoardDo's). Every class is a UICustomControl (ui_types.h: vtable, the item's x / y / w / h, its CustomWidget):
// the race setup screen puts a TrackChooser, a CarChooser, an OpponentViewer and a RaceOptionViewer in its dialog; the
// high score board (BoardDo) a HighScoreBoardTable and a BoardCustomText. Fields are volatile, as in ui_types.h: the
// rewrites read and write each one where the original does, in its order.
//
// Classes (vtable, size):
//   BoardCustomText 0x4deb20 0x24       a line of text in euro12.fnt
//   RaceResultTable 0x4deb70 0x28       the records' columns (euro10.fnt, three palettes); abstract (no Draw)
//   HighScoreBoardTable 0x4dec18 0x2c   the best lap, the best speed and the six best races of a RecordMgr
//   BoardControl 0x4debc8 0x24          the table in the post-race screen (BoardCreateControl)
//   TrackViewer 0x4deff8 0x28           a track's map, name and difficulty
//   TrackChooser 0x4df048 0x5c          a TrackViewer with prev / next and a details button
//   CarViewer3D 0x4df0f0 0x110          a car turning on its stand (3D), its name, its stats (the car's .tab)
//   CarChooser 0x4df140 0x200           a CarViewer3D with prev / next (paint jobs), paint and details buttons
//   OpponentViewer 0x4df1c8 0x104       the opponent: the pack, a ghost car or the clock
//   RaceOptionViewer 0x4df228 0x70      realism, race type, laps, AI strength and count (two groups)
//   UICustomControl 0x4db190 0x18       the base (root library)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace mview {

struct MvCtl {                                    // UICustomControl's fields (ui_types.h), as the controls derive them
    const void* volatile vtbl;                    // +0
    volatile int32_t x, y, w, h;                  // +4 the item's rectangle (_UIAddItems)
    void* volatile widget;                        // +0x14 its CustomWidget
};
static_assert(sizeof(MvCtl) == 0x18, "MvCtl");

// ---- board.obj ------------------------------------------------------------------------------------------------------------
struct BoardCustomText : MvCtl {                  // 0x24
    void* volatile pal;                           // +0x18 gradient 0x57a6f0 .. 0x57a604
    void* volatile font;                          // +0x1c euro12.fnt
    const char* volatile text;                    // +0x20 (the caller's)
};
static_assert(sizeof(BoardCustomText) == 0x24, "BoardCustomText");

struct RaceResultTable : MvCtl {                  // 0x28
    void* volatile font;                          // +0x18 euro10.fnt
    void* volatile pal[3];                        // +0x1c normal (0x57a6c4), +0x20 dim (0x57a64c), +0x24 highlit (0x57a6f0)
};
static_assert(sizeof(RaceResultTable) == 0x28, "RaceResultTable");

struct HighScoreBoardTable : RaceResultTable {    // 0x2c
    const void* volatile mgr;                     // +0x28 RecordMgr const*
};
static_assert(sizeof(HighScoreBoardTable) == 0x2c, "HighScoreBoardTable");

struct BoardControl : MvCtl {                     // 0x24 (MemAlloc(0x24))
    HighScoreBoardTable* volatile table;          // +0x18 MemAlloc(0x2c)
    const void* volatile mgr;                     // +0x1c RecordMgr const* (RecordMgrCreate)
    volatile int32_t track;                       // +0x20
};
static_assert(sizeof(BoardControl) == 0x24, "BoardControl");

// a RecordMgr's record (phys_record.cpp's RaceRecord), the fields the table draws
struct MvRaceRecord {                             // 0x48
    volatile int16_t year;                        // +0 date: year, day, month
    volatile uint8_t day, month;                  // +2, +3
    volatile float lap_time;                      // +4
    volatile float top_speed;                     // +8
    volatile float race_time;                     // +0xc
    char car_name[13];                            // +0x10
    char driver_name[0x21];                       // +0x1d
    volatile int16_t index;                       // +0x3e
    volatile uint8_t flags;                       // +0x40 2: best lap, 4: best speed, 8: best race (drawn highlit)
    uint8_t _41[7];
};
static_assert(sizeof(MvRaceRecord) == 0x48 && offsetof(MvRaceRecord, flags) == 0x40, "MvRaceRecord");

// ---- trkview.obj ----------------------------------------------------------------------------------------------------------
struct TrackViewer : MvCtl {                      // 0x28
    void* volatile map;                           // +0x18 "<track>.stp" (set_stamp), or 0
    void* volatile frame;                         // +0x1c carfrm.stp
    volatile int32_t _20;                         // +0x20 (never written)
    volatile int32_t track;                       // +0x24 -1: none ("emptytrk.stp")
};
static_assert(sizeof(TrackViewer) == 0x28, "TrackViewer");

// the race's options (MenuDoRaceSetup's GameOptions, 10 dwords)
struct GameOptions {                              // 0x28
    volatile int32_t realism;                     // +0
    volatile int32_t game_time;                   // +4
    volatile int32_t weather;                     // +8
    volatile int32_t field;                       // +0xc 0 the pack, 1 a ghost car, 2 the clock (OpponentViewer)
    volatile int32_t race_type;                   // +0x10
    volatile int32_t laps;                        // +0x14
    volatile int32_t event;                       // +0x18
    volatile int32_t ghost;                       // +0x1c the ghost car's type
    volatile int32_t ai_strength;                 // +0x20
    volatile uint8_t b24, mirror;                 // +0x24, +0x25
    uint8_t _26[2];
};
static_assert(sizeof(GameOptions) == 0x28, "GameOptions");

struct TrackChooser : MvCtl {                     // 0x5c
    TrackViewer viewer;                           // +0x18 (its own vtable)
    char* volatile name;                          // +0x40 the chosen track's friendly name (the caller's buffer)
    volatile uint8_t flags;                       // +0x44 2: "no track" is a choice, 4: skip the test track
    uint8_t _45[3];
    GameOptions* volatile opts;                   // +0x48
    uint8_t _4c[0xc];
    volatile uint8_t* volatile not_test;          // +0x58 set to "the test track isn't the one chosen" (Callback)
};
static_assert(sizeof(TrackChooser) == 0x5c && offsetof(TrackChooser, opts) == 0x48 && offsetof(TrackChooser, not_test) == 0x58,
              "TrackChooser");

// ---- carview.obj ----------------------------------------------------------------------------------------------------------
struct MvFrame {                                  // 0x30: a 3x3 matrix, then the position (Frame)
    volatile float m[9];
    volatile float p[3];
};
static_assert(sizeof(MvFrame) == 0x30, "MvFrame");

typedef int(__cdecl* CarCountFn)();
typedef const char*(__cdecl* CarNameFn)(int);

struct CarViewer3D : MvCtl {                      // 0x110
    volatile int32_t car;                         // +0x18 option "car_no"
    volatile int32_t paint;                       // +0x1c option "player_paintjob" (0..7)
    char set[0x20];                               // +0x20 the car's resource set ("<car>.car"), "" when none
    void* volatile frame_stamp;                   // +0x40 carfrm.stp
    volatile int32_t model;                       // +0x44 the body (mrModelLoadRemap)
    volatile int32_t wheel[2];                    // +0x48 front, +0x4c rear: copies of wheel_1.mod, scaled
    void* volatile tab;                           // +0x50 StringTable* "<car>1.tab"
    MvFrame camera;                               // +0x54 (Create)
    MvFrame body;                                 // +0x84 turned every frame (Draw3D)
    volatile float stat[5];                       // +0xb4 the .tab's row 1, entries 1..5
    char text[0x20];                              // +0xc8 entry 6
    volatile int32_t istat;                       // +0xe8 entry 7
    volatile float stat2[5];                      // +0xec entries 8..12
    volatile float t;                             // +0x100 the turn's progress (-0.5 at first, to 1.1)
    volatile int32_t pose;                        // +0x104 the pose it turns from (0..4)
    CarCountFn volatile count;                    // +0x108
    CarNameFn volatile name_of;                   // +0x10c
};
static_assert(sizeof(CarViewer3D) == 0x110 && offsetof(CarViewer3D, camera) == 0x54 && offsetof(CarViewer3D, stat) == 0xb4 &&
              offsetof(CarViewer3D, t) == 0x100, "CarViewer3D");

struct CarChooser : MvCtl {                       // 0x200
    char s18[0x10], s28[0x10], s38[0x10], s48[0x10], s58[0x10];   // +0x18 .. the stats as text
    char s68[0x20];                               // +0x68 (the .tab's text, copied whole)
    char s88[0x10];                               // +0x88
    char s98[0x20], sb8[0x20];                    // +0x98, +0xb8
    char sd8[0x10];                               // +0xd8
    CarViewer3D viewer;                           // +0xe8
    volatile uint8_t b1f8, b1f9;                  // +0x1f8, +0x1f9 (the constructor's)
    uint8_t _1fa[2];
    char* volatile name;                          // +0x1fc the chosen car's friendly name (the caller's buffer)
};
static_assert(sizeof(CarChooser) == 0x200 && offsetof(CarChooser, viewer) == 0xe8 && offsetof(CarChooser, name) == 0x1fc,
              "CarChooser");

// ---- oppview.obj ----------------------------------------------------------------------------------------------------------
struct OpponentViewer : MvCtl {                   // 0x104
    volatile uint8_t ghost;                       // +0x18 drawn half transparent
    char set[0x20];                               // +0x19 the model's resource set
    char car[0x20];                               // +0x39 SetCar's name
    char label[0x23];                             // +0x59 what it shows ("the pack", "ghost car", "clock")
    void* volatile frame_stamp;                   // +0x7c carfrm.stp
    void* volatile clock;                         // +0x80 clock.stp
    void* volatile pack;                          // +0x84 thepack.stp
    volatile int32_t model;                       // +0x88
    MvFrame camera;                               // +0x8c
    MvFrame body;                                 // +0xbc
    volatile int32_t* volatile out;               // +0xec
    GameOptions* volatile opts;                   // +0xf0
    volatile int32_t type;                        // +0xf4 0 the pack, 1 the clock, 2 a ghost car (option "opponent")
    volatile int32_t saved[2];                    // +0xf8 "mgc_type", +0xfc "sgc_type"
    volatile uint8_t created;                     // +0x100
    uint8_t _101[3];
};
static_assert(sizeof(OpponentViewer) == 0x104 && offsetof(OpponentViewer, frame_stamp) == 0x7c && offsetof(OpponentViewer, out) == 0xec,
              "OpponentViewer");

// ---- optview.obj ----------------------------------------------------------------------------------------------------------
struct RaceOptionViewer : MvCtl {                 // 0x70: MenuDoRaceSetup's frame holds it at +0x230, the OpponentViewer at +0x2a0
    volatile uint8_t* volatile flag;              // +0x18 (constructor argument 4)
    GameOptions* volatile opts;                   // +0x1c
    volatile int32_t _20;                         // +0x20
    volatile float ai_count;                      // +0x24 the slider's value
    char* volatile summary;                       // +0x28 the caller's text (MenuDoRaceSetup's: 0x20 bytes, just before the viewer)
    volatile int32_t* volatile ai_out;            // +0x2c
    volatile int32_t* volatile out2;              // +0x30
    volatile uint32_t group[4];                   // +0x34 EnterGroup's outs
    volatile int32_t event;                       // +0x44 option "event_type"
    volatile int32_t _48;                         // +0x48
    char text[0x24];                              // +0x4c "%d %s" (laps)
};
static_assert(sizeof(RaceOptionViewer) == 0x70 && offsetof(RaceOptionViewer, text) == 0x4c, "RaceOptionViewer");

enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_BoardCustomText = 0x004deb20, VT_RaceResultTable = 0x004deb70,
    VT_BoardControl = 0x004debc8, VT_HighScoreBoardTable = 0x004dec18, VT_TrackViewer = 0x004deff8,
    VT_TrackChooser = 0x004df048, VT_CarViewer3D = 0x004df0f0, VT_CarChooser = 0x004df140,
    VT_OpponentViewer = 0x004df1c8, VT_RaceOptionViewer = 0x004df228,
};

// ---- statics (v1.0) ---------------------------------------------------------------------------------------------------------
enum : uint32_t {
    S_LOCALE = 0x00509354,          // LocaleInfo const*: +0x18 speed unit, +0x1c speed factor, +0x28 / +0x2c power, +0x38 metric
    S_BOARD_MGR = 0x004f90d8,       // RecordMgr const* (BoardDo, clear_cb)
    S_BOARD_TABLE = 0x0057a6f8,     // HighScoreBoardTable::the one being shown
    S_TRACK_CHOOSER = 0x004fa1f8,   // TrackChooser::the one
    S_CAR_CHOOSER = 0x0057bca4,     // CarChooser::glob
    S_OPP_VIEWER = 0x0057bce4,      // OpponentViewer::the one
    S_TRK_DIALOG = 0x004fa338,      // UIDialog: TrackChooser::details (items set once, to the first call's frame)
    S_CAR_DIALOG = 0x004fa468,      // UIDialog: CarChooser::details (likewise)
    S_TRACK_ORDER = 0x004defd0,     // char const*[8], to 0x4deff0
    S_GX_SCREEN_W = 0x005228f4, S_GX_SCREEN_H = 0x005228d4,
    S_MR_DEVICE = 0x00522a1c,       // the renderer's level (>= 2: env-mapped models)
    S_CARFILE = 0x0057b9c8,         // CarFile: CarViewer3D::SetModel's (wheel positions at +0x18.., +0x1a8..)
    S_REMAP_CAR = 0x0057b8e0,       // mrModelRemapInfo: CarViewer3D's texture remap (0x10 name, +0x10 name, +0x20 / +0x24 0)
    S_REMAP_OPP = 0x0057bcb0,       // ... OpponentViewer's
    S_OPP_LABELS = 0x004fa7a0,      // char const*[3]: the three labels, set once
};

// ---- the game's functions (v1.0) -------------------------------------------------------------------------------------------
enum : uint32_t {
    F_MatrixMakeYaw = 0x00402f80, F_MatrixMakePitch = 0x00402fc0, F_MatrixMakeRoll = 0x00403000,
    F_MatrixConcat = 0x00403040, F_GetRealismString = 0x00405e40, F_GetRaceTypeString = 0x00405ec0,
    F_GetGhostCarTypeString = 0x00405fe0, F_GetWeatherString = 0x004060a0, F_GetGameTimeString = 0x00406120,
    F_GetAIStrengthString = 0x004061a0, F_GetFieldString = 0x00406240, F_GetEventString = 0x004062d0,
    F_GetLapCountFromType = 0x004065f0, F_GetTrackDifficulty = 0x00406650, F_GetTrackCount = 0x00406800,
    F_GetTrackName = 0x00406810, F_GetTrackFriendlyName = 0x00406830, F_GetTrackText = 0x00406870,
    F_GetTrackNumber = 0x004068b0, F_GetCarFileNumber = 0x00406930, F_HackEnabled = 0x0040d4b0,
    F_ResourceSetMustLoad = 0x00419a30, F_ResourceSetUnload = 0x00419bb0, F_LocaleFormatShortDate = 0x0041ac10,
    F_StringTableGet = 0x0041b210, F_StringTableForget = 0x0041b270, F_StringTableGetEntry = 0x0041b2a0,
    F_MatrixMulPoint = 0x00429420, F_RecordMgrCreate = 0x0042a990, F_RecordMgrDestroy = 0x0042a9b0,
    F_RecordMgr_Clear = 0x0042ad10, F_RecordMgr_SetMode = 0x0042ad50, F_RecordMgr_GetNthBestRace = 0x0042ad80,
    F_RecordMgr_GetNthBestLap = 0x0042ade0, F_RecordMgr_GetNthBestSpeed = 0x0042ae30,
    F_PhysicsTimeString = 0x0042bf30, F_mrSetView = 0x0044ecd0, F_mrSetProjection = 0x0044ede0,
    F_mrSetCamera = 0x0044ee70, F_mrModelLoad = 0x004558c0, F_mrModelLoadRemap = 0x00455900,
    F_mrModelUnload = 0x00455950, F_mrModelRemapTextures = 0x00455980, F_mrModelCopyTransform = 0x00455aa0,
    F_mrModelDestroyCopy = 0x00455b30, F_mrModelEnvMap = 0x00455cf0, F_mrModelDraw = 0x00455d00,
    F_mrModelGetExtents = 0x00456ec0, F_mrModelSetAlpha = 0x00457090, F_mrModelClearAlpha = 0x004570b0,
    F_mrEnable = 0x00457890, F_mrDisable = 0x00457b00, F_WorldGetCarTexture = 0x00462070,
    F_MatrixMakeWorldRotation = 0x00463fe0, F_CarFileLoad = 0x004647d0, F_OptionsGet = 0x004713e0,
    F_OptionsSet = 0x00471430, F_CreateMultiString = 0x0047b410, F_UICC_Dirty = 0x0047e820,
    F_UICC_AddNotification = 0x0047e840, F_UICC_RemoveNotification = 0x0047e860, F_UICC_AddItems = 0x0047e870,
    F_PaintKitDo = 0x004c62d0, F_adj_fdiv_m32 = 0x004ce7bc, F_ftol = 0x004cf108, F_atoi = 0x004cf990,
    F_atof = 0x004cfe40, F_stricmp = 0x004da350,
    // this group's own (called by address, as the original does, so a hooked rewrite is what runs)
    F_BoardCustomText_ctor = 0x0048fd50, F_BoardCustomText_dtor = 0x0048fdb0, F_RaceResultTable_ctor = 0x0048fe20,
    F_RaceResultTable_dtor = 0x0048feb0, F_draw_legend = 0x0048ff00, F_draw_element = 0x00490260, F_BoardDo = 0x00490740,
    F_TrackViewer_ctor = 0x0049ad60, F_TrackViewer_dtor = 0x0049add0, F_TrackViewer_set_stamp = 0x0049b080,
    F_TrackViewer_SetTrack = 0x0049b0b0, F_TrackViewer_Prev = 0x0049b140, F_get_track_order = 0x0049b170,
    F_TrackViewer_Next = 0x0049b1e0, F_TrackViewer_GetTrackName = 0x0049b210,
    F_TrackViewer_GetTrackFriendlyName = 0x0049b230, F_TrackChooser_update_track = 0x0049b270,
    F_TrackChooser_do_board = 0x0049b2b0, F_TrackChooser_dtor = 0x0049b320, F_is_test_track = 0x0049bc00,
    F_TrackChooser_next_track = 0x0049bc20, F_TrackChooser_prev_track = 0x0049bc80,
    F_CarViewer3D_ctor = 0x0049bf50, F_CarViewer3D_dtor = 0x0049c020, F_Hermite = 0x0049c680,
    F_CarViewer3D_SetModel = 0x0049c6e0, F_CarViewer3D_UpdateCar = 0x0049ca80, F_CarViewer3D_Next = 0x0049cdd0,
    F_CarViewer3D_SetPaintJob = 0x0049ce00, F_CarViewer3D_GetPaintJob = 0x0049ce30, F_CarViewer3D_Prev = 0x0049ce40,
    F_CarViewer3D_GetFriendlyName = 0x0049ce70, F_CarViewer3D_GetName = 0x0049ce90,
    F_CarChooser_update_car = 0x0049cec0, F_CarChooser_dtor = 0x0049d2c0, F_CarChooser_paint = 0x0049d2f0,
    F_OpponentViewer_dtor = 0x0049e370, F_OpponentViewer_set_ghost_type = 0x0049e3c0,
    F_OpponentViewer_SetModel = 0x0049eae0, F_OpponentViewer_UpdateOpponent = 0x0049ec50,
    F_OpponentViewer_GetFriendlyName = 0x0049ee70, F_OpponentViewer_Next = 0x0049ee80,
    F_OpponentViewer_Prev = 0x0049eea0,
    // callbacks and atexit destructors the originals hand out (their addresses, as data)
    F_clear_cb = 0x00490ae0, F_board_cb = 0x0049b870, F_next_track_cb = 0x0049bb00, F_prev_track_cb = 0x0049bb10,
    F_next_car_cb = 0x0049e010, F_prev_car_cb = 0x0049e030, F_car_details_cb = 0x0049e050,
    F_opp_next_cb = 0x0049eec0, F_opp_prev_cb = 0x0049eed0,
};

// ---- helpers ---------------------------------------------------------------------------------------------------------------
// a function-local static Xlator, built on first use: the guard's bit set, the constructor, its destructor at exit
static __forceinline void xl_once(uint32_t guard, uint8_t bit, uint32_t xl, uint32_t key, uint32_t dtor) {
    if (!(UI_G8(guard) & bit)) {
        UI_G8(guard) = (uint8_t)(UI_G8(guard) | bit);
        uit::tcall<void*>(uit::F_Xlator_ctor, (void*)(uintptr_t)xl, (const char*)(uintptr_t)key);
        uit::ccall<int>(uit::F_atexit, dtor);
    }
}
// UIDialogItem::UIDialogItem (replay.obj) by address: every argument as the original pushes it (the floats' bits)
static __forceinline void item_ctor(void* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                                    const char* text, int32_t i1c, const void* data, int32_t style, uint32_t lo = 0,
                                    uint32_t hi = 0, const void* sel = 0, const char* s34 = 0) {
    uit::tcall<void*>(uit::F_UIDialogItem_ctor, it, type, id, x, y, w, h, text, i1c, data, style, lo, hi, sel, s34);
}
// an item written in place, all 14 dwords (the ones the original writes inline)
static __forceinline void item14(void* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                                 const char* text, int32_t i1c, const void* data, int32_t style, uint32_t lo = 0,
                                 uint32_t hi = 0, const void* sel = 0, const char* s34 = 0) {
    volatile uint32_t* d = (volatile uint32_t*)it;
    d[0] = (uint32_t)type;
    d[1] = (uint32_t)id;
    d[2] = (uint32_t)x;
    d[3] = (uint32_t)y;
    d[4] = (uint32_t)w;
    d[5] = (uint32_t)h;
    d[6] = (uint32_t)(uintptr_t)text;
    d[7] = (uint32_t)i1c;
    d[8] = (uint32_t)(uintptr_t)data;
    d[9] = (uint32_t)style;
    d[10] = lo;
    d[11] = hi;
    d[12] = (uint32_t)(uintptr_t)sel;
    d[13] = (uint32_t)(uintptr_t)s34;
}
// the end of an item list: the static item at 0x578d08, copied (rep movsd)
static __forceinline void item_end(void* it) { uit::crt_copy(it, (const void*)(uintptr_t)uit::S_END_ITEM, 0x38); }

// a LocaleInfo field (the static pointer read where the original reads it)
static __forceinline uint8_t* locale() { return UI_GP(uint8_t, S_LOCALE); }

// ---- the fixes' helpers (docs/PORTING.md, "Fixes"; each use is marked // FIX:) ----------------------------------------------
// s as a format's %s argument where the text is formatted in a buffer of the rewrite's: s itself, or (the fix build only)
// a copy of its first max characters in buf (max + 1 bytes) when it's longer, so what the format prints is bounded
static __forceinline const char* fix_cut(const char* s, char* buf, uint32_t max) {
    if (!VP_FIX || uit::ui_strnlen(s, max) <= max) return s;
    uit::ui_copy_bounded(buf, s, max + 1);
    return buf;
}
// the characters the game's %d prints for v (a '-' and the digits)
static __forceinline uint32_t fix_dec_len(int32_t v) {
    uint32_t n = v < 0 ? 2u : 1u;
    uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    while (u >= 10u) { u /= 10u; n++; }
    return n;
}

}  // namespace mview
