// career_types.h -- M3 UI stage, step U4 (group A): the career's layouts, statics and addresses (library `career`:
// career.obj, chooser.obj, events.obj, postseas.obj, ranking.obj, season.obj, testing.obj), shared by
// hook/career_main.cpp, hook/career_screens.cpp and test/world_career.cpp. Groups B (the upgrade shop, the credits and
// the intro) and C (the paint kit) keep their own layouts in their own headers (career_shop.h, paint_kit.h).
//
// Recovered from the v1.0 disassembly (the constructors, the Draw / Added methods, the screens' frames, the career file's
// reads and writes). Fields are volatile, as in ui_types.h: the rewrites read and write each one where the original does.
//
// Classes (vtable, size), all UICustomControls (base 0x4db190, 0x18):
//   CareerStatus 0x4dfa08 0xd8      the week, funds and season lines of the career's menus (career_menu, EventsDo)
//   CareerSummary 0x4dfab8 0x24     a career's summary: name, class, season, week, hours, funds (two dynamic styles)
//   CareerBlurb 0x4dfa68 0x24       the chooser's summary of the selected slot (a CareerSummary, its own vtable)
//   SeasonViewer 0x4dfb10 0x20      the season's calendar (EventsDo): track, length, purse, place, points
//   TrackImage 0x4dfb68 0x1c        the testing menu's picture of the selected track (<track>.stp)
//   StandingsViewer 0x4dfbc0 0x20   the drivers' standings (PostSeasonDo, RankingDo)
//   ClickThrough 0x4dfc10 0x18      the season splashes' "click anywhere" (a mouse press sets 0x5d36b4)
//
// CareerInfo (0x674, the file career<slot>.dat's body; the live one at 0x5cf078, the last saved copy at 0x5cf790):
//   +0 the player's name, +0x10 realism, +0x14 damage on (u8), +0x18 funds, +0x1c the car's upgrades (CareerUpgrades),
//   +0x11c seasons, +0x120 the week (events entered), +0x124 the class (0..3), +0x128 the next event, +0x12c qualified (u8),
//   +0x130 the qualifying times (float[8]), +0x150 the driver map (int[8]: 7 is the player), +0x170 the drivers
//   (8 x 0x88: +0 rank, +4 points, +8 each event's place, [32]), +0x5b0 the awards (int[4][8]), +0x630 seconds raced.
// CareerSeason (the resource season<class>.ssn, 'SEAS'): +0 the events' count, +4 the season's prizes by rank (int[8]),
//   +0x24 the events (0x58 each: +0 the track, +0x10 reversed (u8), +0x14 laps, +0x18 the purse by place 1..8,
//   +0x38 the points by place 1..8).
// The career file: {'FNIC' (0x43494e46), version 2, 0x674, the checksum (the bytes' sum)} then the body xor'd with 0xa4.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace car {
using uit::Edx;

// ---- the classes ----------------------------------------------------------------------------------------------------------
struct CareerStatus : uit::UICustomControl {      // 0xd8, vtable 0x4dfa08
    char week[0x40];                              // +0x18 "<Week> <n>"
    char funds[0x40];                             // +0x58 LocaleMoney
    char season[0x40];                            // +0x98 "<n>"
};
static_assert(sizeof(CareerStatus) == 0xd8 && offsetof(CareerStatus, funds) == 0x58, "CareerStatus");

struct CareerSummary : uit::UICustomControl {     // 0x24, vtable 0x4dfab8 (CareerBlurb 0x4dfa68)
    volatile int32_t style;                       // +0x18 euro10 (labels)
    volatile int32_t style2;                      // +0x1c ruby9 (values)
    const uint8_t* volatile info;                 // +0x20 the CareerInfo shown
};
static_assert(sizeof(CareerSummary) == 0x24, "CareerSummary");

struct TwoStyles : uit::UICustomControl {         // 0x20: SeasonViewer 0x4dfb10, StandingsViewer 0x4dfbc0
    volatile int32_t style;                       // +0x18
    volatile int32_t style2;                      // +0x1c
};
static_assert(sizeof(TwoStyles) == 0x20, "TwoStyles");

struct TrackImage : uit::UICustomControl {        // 0x1c, vtable 0x4dfb68
    const int32_t* volatile track;                // +0x18 the list's selection (0x4ffe44)
};
static_assert(sizeof(TrackImage) == 0x1c, "TrackImage");

enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_CareerStatus = 0x004dfa08, VT_CareerBlurb = 0x004dfa68,
    VT_CareerSummary = 0x004dfab8, VT_SeasonViewer = 0x004dfb10, VT_TrackImage = 0x004dfb68,
    VT_StandingsViewer = 0x004dfbc0, VT_ClickThrough = 0x004dfc10,
};

// ---- statics (v1.0) -------------------------------------------------------------------------------------------------------
enum : uint32_t {
    // career.obj
    S_INFO = 0x005cf078,             // CareerInfo (0x674)
    S_INFO_SIZE = 0x674,
    S_INFO_SAVED = 0x005cf790,       // ... as last loaded or saved (back_cb compares)
    S_REALISM = 0x005cf088, S_DAMAGE = 0x005cf08c, S_FUNDS = 0x005cf090, S_UPGRADES_IN_INFO = 0x005cf094,
    S_SEASONS = 0x005cf194, S_WEEK = 0x005cf198, S_CLASS = 0x005cf19c, S_EVENT = 0x005cf1a0,
    S_QUALIFIED = 0x005cf1a4, S_QUAL_TIMES = 0x005cf1a8, S_DRIVER_MAP = 0x005cf1c8, S_DRIVERS = 0x005cf1e8,
    S_DRIVERS_END = 0x005cf628, S_AWARDS = 0x005cf628, S_SECONDS = 0x005cf6a8,
    S_SEASON = 0x005cf700,           // CareerSeason* (set_class)
    S_UPGRADE_SET = 0x005cffcc,      // CarUpgradeSet* (viper.ugs, while career_menu runs)
    S_CAREER_UPGRADES = 0x004ff5a8,  // u8* CareerUpgrades (0x5cf094 while a career runs)
    S_LOG = 0x004ff5ac,              // the career log's file (c:\career.log)
    S_SLOT = 0x005cfe78,             // the slot being played
    S_5CFE58 = 0x005cfe58,           // u8, cleared by career_main (nothing reads it)
    S_NEW_CLASS = 0x005cfe7c,        // u8: career_menu's idle shows NewClassDo once
    S_FILENAME = 0x005cfe88,         // get_career_filename's buffer (to the Xlators at 0x5cff90)
    S_CLASS_NAME = 0x005cf760,       // set_class's copy of the class's name (to the saved CareerInfo at 0x5cf790)
    // chooser.obj
    S_CH_SLOT = 0x005d016c,          // the slot selected (options GAME career_slot)
    S_CH_LIST = 0x005d0074,          // UIStringList* (the slots' names)
    S_CH_INFOS = 0x005d01b0,         // CareerInfo[8] (to 0x5d3550)
    S_CH_INFOS_END = 0x005d3550,
    S_CH_USED = 0x005d3578,          // u8[8]: the slot holds a career
    S_CH_GROUP_EMPTY = 0x005d3580,   // the group shown for an empty slot (Create)
    S_CH_GROUP_USED = 0x005d0194,    // ... for a used one (Load, Delete)
    // events.obj
    S_EV_SINGLE = 0x005d5d4c,        // SeasonViewer::Draw's Xlator guard
    // postseas.obj
    S_PS_AMOUNT = 0x005d36d4,        // PostSeasonDo's rank (-1: a race's results), for the money screens
    S_PS_RACE_MONEY = 0x005d370c,    // u8: show_race_money runs the credits once
    S_PS_CLICKED = 0x005d36b4,       // u8: ClickThrough was pressed
    S_PS_SPLASH_TIME = 0x005d371c,   // PTimeNow when the season splash opened
    // testing.obj
    S_TS_TRACK = 0x004ffe44,         // the testing menu's track (the list's selection)
    S_TS_REVERSED = 0x004ffe48,      // u8: reversed (the check box's)
    // elsewhere
    S_LOUNGE = 0x004eb8b4,           // IDriverLounge* (driver.obj): vtable +4 Get(strength, driver)
    S_SEC_GAME = 0x004ff5d8,         // "GAME"
};

// ---- the game's functions (v1.0) ------------------------------------------------------------------------------------------
enum : uint32_t {
    // career.obj
    F_CareerGetInfo = 0x004bc470, F_CareerGetSeason = 0x004bc480, F_CareerGetUpgradeSet = 0x004bc490,
    F_CareerGiveAward = 0x004bc4a0, F_CareerLog = 0x004bc4f0, F_CareerDo = 0x004bc570, F_career_main = 0x004bc5d0,
    F_do_event = 0x004bc760, F_fake_results = 0x004bced0, F_sort_carlist = 0x004bcf20, F_CareerGetDriver = 0x004bd310,
    F_CareerGetDriverName = 0x004bd330, F_CareerGetClassName = 0x004bd350, F_CareerGetCarName = 0x004bd4f0,
    F_null_postrace = 0x004bd500, F_get_options = 0x004bd510, F_CareerLoadSlot = 0x004bd600, F_CareerSaveSlot = 0x004bd620,
    F_CareerDestroySlot = 0x004bd640, F_get_career_filename = 0x004bd700, F_career_menu = 0x004bd750,
    F_upgrade_cb = 0x004bdde0, F_save_cb = 0x004bddf0, F_ranking_cb = 0x004bdee0, F_back_cb = 0x004bdef0,
    F_career_menu_idle = 0x004bdff0, F_career_load = 0x004be080, F_career_save = 0x004be170,
    F_update_ranking = 0x004be280, F_driver_compare = 0x004be2d0, F_CareerUnload = 0x004be3e0, F_CareerReload = 0x004be400,
    F_CareerIsTestMode = 0x004be410, F_set_class = 0x004be450, F_checksum = 0x004be4d0, F_xor_block = 0x004be500,
    F_credit_account = 0x004be520,
    // chooser.obj
    F_CareerChooser = 0x004be7b0, F_delete_cb = 0x004beec0, F_create_cb = 0x004beee0, F_refresh = 0x004bf710,
    F_CareerSummary_ctor = 0x004bf790, F_CareerSummary_dtor = 0x004bf880, F_CareerSummary_draw_entry = 0x004bf8b0,
    // season.obj
    F_CareerSeasonGet = 0x004bfd90, F_could_xlate_money = 0x004bfe40, F_CareerSeasonForget = 0x004bfe70,
    // events.obj
    F_EventsDo = 0x004c00a0, F_SeasonViewer_ctor = 0x004c0480,
    // testing.obj
    F_TestingDo = 0x004c0e70, F_do_race = 0x004c0ec0, F_testing_menu = 0x004c1040,
    // postseas.obj
    F_PostSeasonDo = 0x004c17e0, F_show_race_money = 0x004c1ae0, F_show_season_money = 0x004c1b00,
    F_show_season_win_splash = 0x004c1b50, F_show_winning_season_money = 0x004c1db0, F_season_splash_idle = 0x004c1dd0,
    F_NewClassDo = 0x004c1e30, F_StandingsViewer_ctor = 0x004c21c0, F_ClickThrough_Idle = 0x004c2620,
    // ranking.obj
    F_RankingDo = 0x004c5160,
    // the rest of the career (groups B)
    F_UpgradeDo = 0x004c29b0, F_CreditsDo = 0x004c5660,
    // root
    F_DoRace = 0x00401e10, F_GetRealismString = 0x00405e40, F_GetTrackName = 0x00406810,
    F_GetTrackFriendlyName = 0x00406830, F_GetTrackNumber = 0x004068b0, F_LoadRace = 0x0040a840,
    F_UnloadRace = 0x0040a870, F_GenerateCarList = 0x0040aac0, F_HackDisable = 0x0040d350, F_HackRestore = 0x0040d370,
    F_PreRaceDo = 0x0040f140,
    // kernel
    F_FileCreate = 0x004115f0, F_FileAppend = 0x004117c0, F_FileReadExact = 0x004118b0, F_FileWrite = 0x00411a30,
    F_FileRemove = 0x00411ba0, F_FileCreateDirectory = 0x00411d00, F_Win32GetUserDirectory = 0x00412cc0,
    F_ScanDown = 0x004130e0,
    // useful
    F_ResourceSetMustLoad = 0x00419a30, F_ResourceSetUnload = 0x00419bb0, F_ResourceGet = 0x00419fa0,
    F_ResourceForget = 0x0041a450, F_LocaleMoney = 0x0041ab50, F_LocaleFormatShortDate = 0x0041ac10,
    F_Xlate = 0x0041aec0, F_CouldXlate = 0x0041af60, F_Random = 0x0041b6e0,
    // ai, world
    F_AIResetDriverMap = 0x00420c30, F_AISetDriverMap = 0x00420c50, F_AIGetTrackInfo = 0x00424e40,
    F_World_ctor = 0x00462a90, F_CarFileGetUpgradeSet = 0x004651f0, F_CarFileForgetUpgradeSet = 0x00465270,
    // options
    F_OptionsGetI = 0x004713e0, F_OptionsSetI = 0x00471430, F_OptionsGetB = 0x00471470, F_OptionsGetS = 0x00471500,
    // ui
    F_UIDoOkBox = 0x004793c0, F_UIDoYesNoBox = 0x00479b30, F_UIDoYesNoCancelBox = 0x0047a0d0,
    F_UICC_Dirty = 0x0047e820, F_UICC_AddNotification = 0x0047e840, F_UICC_AddItems = 0x0047e870,
    F_UIAddDynamicStyle = 0x0047f4a0,             // (UIRemoveStyle, UIHideGroup, UIShowGroup: ui_types.h's)
    // the game's C runtime
    F_qsort = 0x004cf160, F_vsprintf = 0x004cf7c0, F_TimeGetTimeOfDay = 0x004d8af0, F_stricmp = 0x004da350,
};

// ---- helpers ----------------------------------------------------------------------------------------------------------------
#define CA_G8(a) UI_G8(a)
#define CA_G32(a) UI_G32(a)
#define CA_GU32(a) UI_GU32(a)
#define CA_CP(a) ((const char*)(uintptr_t)(a))
#define CA_VP(a) ((void*)(uintptr_t)(a))
static __forceinline uint32_t ca_u(const volatile void* p) { return (uint32_t)(uintptr_t)p; }

// UIDialogItem::UIDialogItem (replay.obj) by address: every argument as the original pushes it (the floats' bits)
static __forceinline void ca_item_ctor(void* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                                       uint32_t text, int32_t i1c, uint32_t data, int32_t style, uint32_t lo = 0,
                                       uint32_t hi = 0, uint32_t sel = 0, uint32_t s34 = 0) {
    uit::tcall<void*>(uit::F_UIDialogItem_ctor, it, type, id, x, y, w, h, text, i1c, data, style, lo, hi, sel, s34);
}
// a function-local static Xlator, built on first use: the guard's bit set, the constructor, its destructor at exit
static __forceinline void ca_xl_once(uint32_t guard, uint8_t bit, uint32_t xl, uint32_t key, uint32_t dtor) {
    if (!(UI_G8(guard) & bit)) {
        UI_G8(guard) = (uint8_t)(UI_G8(guard) | bit);
        uit::tcall<void*>(uit::F_Xlator_ctor, CA_VP(xl), CA_CP(key));
        uit::ccall<int>(uit::F_atexit, dtor);
    }
}
static __forceinline bool ca_xl_built(uint32_t guard, uint8_t bits) { return (UI_G8(guard) & bits) == bits; }
// an Xlator refreshed as the inlined code does it (cookie compared, xlate called if stale), and its text
static __forceinline uint32_t ca_xlate(uint32_t xl) {
    if (UI_GU32(xl + 8) != UI_GU32(uit::S_XLATOR_COOKIE)) uit::tcall<void>(uit::F_Xlator_xlate, CA_VP(xl));
    return UI_GU32(xl + 4);
}
#define CA_FP_XL(f, a) UI_FP(f, a, 12, "a static Xlator")

// a dword / its address in a frame laid out as the original's (F: esp after the prologue's pushes)
#define CA_W(F, off) (*(volatile uint32_t*)((F) + (off)))
#define CA_B(F, off) (*(volatile uint8_t*)((F) + (off)))
#define CA_A(F, off) ((uint32_t)(uintptr_t)((F) + (off)))

}  // namespace car
