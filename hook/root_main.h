// root_main.h -- M3 UI stage, step U3 (group B): the race flow's layouts and addresses (library `root`: main.obj,
// race.obj, prerace.obj, postrace.obj, version.obj; and WinMain, kernel win32.obj), shared by hook/root_main.cpp,
// hook/root_race.cpp and test/world_root_race.cpp.
//
// Recovered from the v1.0 disassembly (the constructors, the Draw / Added methods, PreRaceDo's and PostRaceDo's frames).
// Fields are volatile, as in ui_types.h: the rewrites read and write each one where the original does, in its order.
//
// Classes (vtable, size):
//   CarPosition 0x4db3e0 0xa4          a car on the starting grid: its model turning in 3D, the grid number, the driver,
//                                      the qualifying time (PreRaceDo, one per car, built with MemAlloc(0xa4))
//   StartingGridView 0x4db340 0x60     the grid: a CustomWidget per car (up to 8) around the CarPositions (MemAlloc(0x60))
//   TrackPrizeInfo 0x4db390 0x1c       the career's prize table (place, points, purse), on PreRaceDo's stack
//   LapsTable 0x4db240 0x44            the post-race lap times of one car, a page of 10 at a time (PostRaceDo's stack)
//   PostRaceTable 0x4db298 0x24        the post-race standings (PostRaceDo's stack; its destructor is inlined there)
//   UICustomControl 0x4db190 0x18      the base (replay.obj)
// The World (0xcd4) is wld_world.cpp's: +8 the track's name, +0x28 the cars (0xc8 each: +0 type -- 0 the player, 3 a
// ghost --, +4 the driver's name, +0x11 the car, +0xc0 the AI driver or ~paint job), +0xca8 the count, +0xcac the
// GameOptions (+0 realism, +0x10 race type, +0x14 laps, +0x24 damage, +0x25 reversed).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace rootb {

struct RbCtl {                                    // UICustomControl's fields (ui_types.h)
    const void* volatile vtbl;                    // +0
    volatile int32_t x, y, w, h;                  // +4 the item's rectangle (_UIAddItems)
    void* volatile widget;                        // +0x14 its CustomWidget
};
static_assert(sizeof(RbCtl) == 0x18, "RbCtl");

struct RbMatrix { volatile float m[9]; };         // 0x24
struct RbFrame { volatile float m[9]; volatile float p[3]; };   // 0x30: rotation rows, position
struct RbCompactFrame { volatile float p[3]; volatile float r[3]; };   // 0x18: the position, the rotation axis * angle
static_assert(sizeof(RbFrame) == 0x30 && sizeof(RbCompactFrame) == 0x18, "frames");

// ---- prerace.obj ------------------------------------------------------------------------------------------------------
struct CarPosition : RbCtl {                      // 0xa4
    uint8_t* volatile shadow;                     // +0x18 gxCanvas* "shadow.cvs"
    void* volatile font_small;                    // +0x1c euro9.fnt
    void* volatile font_big;                      // +0x20 euro12.fnt
    void* volatile pal_text;                      // +0x24 gradient 0x505d24 .. 0x505e30 (the player's: 0x505e2c ..)
    void* volatile pal_number;                    // +0x28 gradient 0x505e4c .. 0x505e30
    void* volatile numbers;                       // +0x2c numbers.stp
    void* volatile player_stamp;                  // +0x30 player.stp
    const char* volatile car;                     // +0x34 the World's car name
    const char* volatile driver;                  // +0x38 the World's driver name
    volatile uint32_t time;                       // +0x3c the qualifying time (a float's bits), 0: none
    char remap_from[0x10];                        // +0x40 "<car>.tex" } mrModelRemapInfo (0x28)
    char remap_to[0x10];                          // +0x50 WorldGetCarTexture's
    volatile int32_t remap_20, remap_24;          // +0x60, +0x64 (0)
    volatile int32_t model;                       // +0x68 mrModelLoadRemap("<car>3.mod")
    RbFrame frame;                                // +0x6c turned to face the camera, 4.7 in front of it
    volatile uint8_t odd;                         // +0x9c the grid slot is odd (right-hand column)
    volatile uint8_t player;                      // +0x9d the driver index is negative (the player's paint job)
    uint8_t _9e[2];
    volatile int32_t index;                       // +0xa0 the grid slot
};
static_assert(sizeof(CarPosition) == 0xa4 && offsetof(CarPosition, remap_from) == 0x40 && offsetof(CarPosition, frame) == 0x6c &&
              offsetof(CarPosition, odd) == 0x9c && offsetof(CarPosition, index) == 0xa0, "CarPosition");

struct StartingGridView : RbCtl {                 // 0x60 (MemAlloc(0x60); the class uses 0x5c)
    CarPosition* volatile pos[16];                // +0x18 one per car (the World's count, unchecked)
    uint8_t* volatile world;                      // +0x58 World*
    const volatile uint32_t* volatile times;      // +0x5c PreRaceDo's float const* (the qualifying times), or 0
};
static_assert(offsetof(StartingGridView, world) == 0x58 && offsetof(StartingGridView, times) == 0x5c, "StartingGridView");

struct TrackPrizeInfo : RbCtl {                   // 0x1c
    const volatile int32_t* volatile prizes;      // +0x18 PrizeInfo*: 8 x {points, purse}
};
static_assert(sizeof(TrackPrizeInfo) == 0x1c, "TrackPrizeInfo");

// ---- postrace.obj -----------------------------------------------------------------------------------------------------
struct LapsTable : RbCtl {                        // 0x44
    void* volatile font_big;                      // +0x18 euro12.fnt
    void* volatile font_small;                    // +0x1c ruby9.fnt
    void* volatile pal_hi;                        // +0x20 gradient 0x5058bc .. 0x5058e0 (the headings)
    void* volatile pal;                           // +0x24 gradient 0x5058b8 .. 0x5058e0
    void* volatile pal_best;                      // +0x28 gradient 0x5058cc .. 0x5058e0 (the race's best lap / speed)
    volatile int32_t car;                         // +0x2c the car shown (WorldGetPlayerCar, at least 0)
    volatile int32_t cars;                        // +0x30 how many (the constructor's argument)
    volatile int32_t laps;                        // +0x34 the car's laps (car_changed); 0x12d687 at first
    volatile int32_t page;                        // +0x38 watched (a notification)
    volatile int32_t pages;                       // +0x3c
    volatile uint32_t group;                      // +0x40 the page buttons' group (Added's items)
};
static_assert(sizeof(LapsTable) == 0x44 && offsetof(LapsTable, car) == 0x2c && offsetof(LapsTable, group) == 0x40, "LapsTable");

struct PostRaceTable : RbCtl {                    // 0x24
    volatile int32_t style;                       // +0x18 UIAddDynamicStyle (cour14.fnt, flags 9): the columns
    volatile int32_t style_title;                 // +0x1c UIAddDynamicStyle (cour14.fnt, flags 0xc): the title lines
    volatile int32_t count;                       // +0x20 the cars (a ghost at the end not counted)
};
static_assert(sizeof(PostRaceTable) == 0x24, "PostRaceTable");

// a RecordMgr's race record (phys_record.cpp's RaceRecord): the fields these screens read
struct RbRaceRecord {                             // 0x48 (the records' stride; the class's own size is 0x44)
    volatile int16_t year;                        // +0 the date: year, day, month
    volatile uint8_t day, month;                  // +2, +3
    volatile uint32_t lap_time;                   // +4 (a float's bits)
    volatile uint32_t top_speed;                  // +8
    volatile uint32_t race_time;                  // +0xc
    char car_name[13];                            // +0x10
    char driver_name[0x21];                       // +0x1d
    volatile int16_t index;                       // +0x3e
    volatile uint8_t flags;                       // +0x40 2: best lap, 4: best speed
    uint8_t _41[7];                               // (+0x41 the car, +0x43 the lap: the lookups' keys)
};
static_assert(sizeof(RbRaceRecord) == 0x48 && offsetof(RbRaceRecord, flags) == 0x40, "RbRaceRecord");

// the dashboards (main.obj's table at 0x4e3770, 5 of them, 0x14 each)
struct DashType {
    void(__cdecl* volatile update)();             // +0 before the screen is grabbed
    void(__cdecl* volatile draw2d)(void* canvas); // +4 on the grabbed screen
    void(__cdecl* volatile draw3d)();             // +8 after WorldDraw
    uint8_t(__cdecl* volatile key)(uint32_t key); // +0xc a key it takes (nonzero: taken)
    const volatile uint8_t* volatile keys;        // +0x10 the scan keys it enables, 0xff-terminated
};
static_assert(sizeof(DashType) == 0x14, "DashType");

enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_LapsTable = 0x004db240, VT_PostRaceTable = 0x004db298,
    VT_StartingGridView = 0x004db340, VT_TrackPrizeInfo = 0x004db390, VT_CarPosition = 0x004db3e0,
};

// ---- statics (v1.0) -----------------------------------------------------------------------------------------------------
enum : uint32_t {
    // main.obj
    S_APP_MODE = 0x004e3760,        // int: 0 the game; 1 dave_b, 2 dave_p, 3 rich_g (-u b/d/r), 4 blimp_jump (-location)
    S_CAMERA = 0x004e3764,          // enum CameraType (5 at first; 0xb the blimp, 0xc the race over)
    S_DASH = 0x004e3768,            // enum DashType
    S_Z_FLAG = 0x004e376c,          // u8 (-z)
    S_DASH_TABLE = 0x004e3770,      // DashType[5]
    S_BLIMP_TV = 0x004e37e8,        // u8: the blimp's F10 (scan 0x10) is down
    S_ESC_SINGLE = 0x004e37f0,      // EscapeMenu (0x38): resume, restart, end race
    S_ESC_SOLO = 0x004e3828,        // EscapeMenu: resume, end race (race type 4 and up)
    S_ESC_SERVER = 0x004e3860,      // EscapeMenu: resume, restart, end race (multiplayer server)
    S_ESC_CLIENT = 0x004e3898,      // EscapeMenu: resume, disconnect (multiplayer client)
    S_TRI = 0x004f436c, S_GRID = 0x004f4368,   // u8: -tri, -grid (elsewhere)
    S_BLIMP_TRACK = 0x00503fb0,     // char[]: -location's track (sscanf %s, unbounded)
    S_XL_END_RACE = 0x00503fd8, S_XL_DISCONNECT = 0x00503ff8, S_XL_RESTART = 0x00504008, S_XL_RESUME = 0x00504080,
    S_CAMERA_SAVED = 0x00504004,    // the camera before the race ended (option "race_camera")
    S_BLIMP_VEL = 0x00504018,       // float[3], function-local (guard 0x504094 bit 0)
    S_BLIMP_START = 0x00504038,     // Frame: -location's
    S_MAIN_DONE = 0x00504068,       // u8: the race is over (MainIsDone)
    S_BLIMP_ROT = 0x00504070,       // float[3] (guard bit 1)
    S_RESTART = 0x0050408c,         // u8: restart the race (escape_callback 0)
    S_BLIMP_GUARD = 0x00504094,
    S_ESC_GUARD = 0x0050409c,       // bits 0..3 the four Xlators, 4..7 the four menus
    S_PAUSED = 0x005040b8,          // u8
    S_BLIMP_FRAME = 0x00521050,     // Frame blimp_frame
    // race.obj
    S_CARLIST = 0x00504340,         // char (*)[0x20]: the *.car names (MemAlloc(0x400): 32, unchecked)
    S_CARLIST_N = 0x00504344,
    S_TRACK_TAB = 0x00504624,       // StringTable* tracks.tab
    S_EVENT_TEXT0 = 0x00504368, S_EVENT_TEXT1 = 0x00504438, S_EVENT_TEXT2 = 0x00504320,   // GetEventString's buffers
    S_LAP_TABLE = 0x004e4698,       // {char const* track; int laps[8];}[8] (to 0x4e47b8)
    // prerace.obj
    S_PR_TRACK = 0x00505c48,        // char[]: the world's track name (strcpy, unbounded)
    S_PR_CAR = 0x00505d38,          // char[]: the player's car name
    S_PR_ARG = 0x00505d8c,          // PreRaceDo's third argument (garage_cb's)
    S_PR_FRIENDLY = 0x00505d90,     // char[]: the track's friendly name
    S_PR_GUARD0 = 0x00505d7c, S_PR_GUARD1 = 0x00505ddc, S_PR_GUARD2 = 0x00505d28,
    S_TPI_GUARD = 0x005d5858,
    // postrace.obj
    S_LAPS_GLOBAL = 0x005058d8,     // LapsTable::global
    S_LT_GUARD = 0x0050592c, S_PRD_GUARD = 0x005059b4, S_PRT_GUARD = 0x005d589c,
    // elsewhere
    S_LOCALE = 0x00509354,          // LocaleInfo const*: +0x14 distance factor, +0x1c speed factor, +0x38 metric
    S_DEITY = 0x005218ac,           // Deity*
    S_GX_SCREEN_W = 0x005228f4, S_GX_SCREEN_H = 0x005228d4,
    S_VERSION = 0x004e59f0,         // VersionGetBuildString's string
    // win32.obj
    S_W32_INSTANCE = 0x004e5eb4, S_W32_ARGS = 0x004e5eb8, S_W32_EC0 = 0x004e5ec0, S_W32_DEDICATED = 0x004e5ec8,
    S_W32_SPLASH = 0x004e5ecc,      // HWND: make_loading_window's
    S_W32_USERDIR = 0x00507f48,     // char[]: get_user_directory's
    // imports (the IAT slots, called through as the original does)
    IAT_OpenClipboard = 0x005d7680, IAT_EmptyClipboard = 0x005d768c, IAT_GlobalAlloc = 0x005d755c,
    IAT_GlobalLock = 0x005d7560, IAT_GlobalUnlock = 0x005d759c, IAT_SetClipboardData = 0x005d7688,
    IAT_CloseClipboard = 0x005d7684, IAT_IsWindow = 0x005d766c, IAT_DestroyWindow = 0x005d7678,
};

// ---- the game's functions (v1.0) ------------------------------------------------------------------------------------------
enum : uint32_t {
    // this group's own (called by address, as the original does, so a hooked rewrite is what runs)
    F_AppProcessArgs = 0x00401280, F_AppMain = 0x00401460, F_app_begin = 0x004014d0, F_app_end = 0x004015a0,
    F_setup_blimp_jump = 0x00401600, F_convert_frame_in = 0x004016f0, F_is_pause_key = 0x00401840,
    F_move_blimp = 0x00401880, F_race = 0x00401d70, F_DoRace = 0x00401e10, F_disable_keys = 0x00401fd0,
    F_enable_keys = 0x00402040, F_game_loop = 0x00402050, F_process_keys = 0x00402260,
    F_copy_blimp_to_clipboard = 0x00402920, F_convert_frame_out = 0x004029e0, F_set_dash_type = 0x00402bd0,
    F_restart_race = 0x00402c30, F_escape_enter = 0x00402c50, F_escape_leave = 0x00402c80,
    F_escape_callback = 0x00402cb0, F_rich_g = 0x00402cf0, F_dave_p = 0x00402d00, F_blimp_jump = 0x00402d10,
    F_dave_b = 0x00402e50, F_MatrixMakeYaw = 0x00402f80, F_MatrixMakePitch = 0x00402fc0,
    F_MatrixMakeRoll = 0x00403000, F_MatrixConcat = 0x00403040,
    F_GetRealismString = 0x00405e40, F_GetRaceTypeString = 0x00405ec0, F_RaceBegin = 0x00406690,
    F_sort_carlist = 0x00406780, F_sort_f = 0x004067a0, F_RaceEnd = 0x004067c0, F_GetTrackCount = 0x00406800,
    F_GetTrackFriendlyName = 0x00406830, F_GetTrackText = 0x00406870, F_GetTrackNumber = 0x004068b0,
    F_GetCarFileName = 0x00406910, F_GetMaxCarFileNames = 0x00406920,
    F_LT_next_car = 0x0040aee0, F_LT_prev_car = 0x0040af00, F_LT_next_page = 0x0040af20, F_LT_prev_page = 0x0040af40,
    F_LT_car_changed = 0x0040af60, F_LapsTable_ctor = 0x0040afe0, F_LapsTable_dtor = 0x0040b0b0,
    F_PostRaceDo = 0x0040b660, F_LT_NextCar = 0x0040bdd0, F_LT_PrevCar = 0x0040bde0, F_LT_NextPage = 0x0040bdf0,
    F_LT_PrevPage = 0x0040be00, F_ASSERT_MSG = 0x00410210, F_garage_cb = 0x00410340, F_CarPosition_ctor = 0x00410360,
    // the rest of the game
    F_DashboardBegin = 0x00403460, F_DashboardEnd = 0x00403720, F_DashboardReset = 0x00403840,
    F_DashboardAlwaysUpdate = 0x00404820, F_DashboardMustDraw = 0x00404a80, F_ReplayDo = 0x00406bc0,
    F_GetGameState = 0x0040a3d0, F_GameDoSingle = 0x0040a680, F_GameDoMulti = 0x0040a7e0, F_LoadRace = 0x0040a840,
    F_UnloadRace = 0x0040a870, F_CountdownBegin = 0x0040ac90, F_CountdownDraw = 0x0040aca0, F_CountdownEnd = 0x0040acb0,
    F_EscapeMenuBegin = 0x0040c520, F_EscapeMenuEnd = 0x0040c540, F_EscapeMenuActive = 0x0040c560,
    F_EscapeMenuDraw = 0x0040c5a0, F_EscapeMenuKey = 0x0040c660, F_EscapeMenuDo = 0x0040c730,
    F_SplashLoading = 0x0040cbc0, F_SplashDoneLoading = 0x0040cc40, F_HackBegin = 0x0040d380, F_HackEnd = 0x0040d4a0,
    F_FileChangeDir = 0x00411ce0, F_check_for_canary_launch = 0x004123a0, F_start_unique_instance = 0x004123b0,
    F_end_unique_instance = 0x00412420, F_get_user_directory = 0x00412430, F_make_loading_window = 0x00412490,
    F_Win32GetWindow = 0x00412be0, F_Win32FlushRegistry = 0x00412cd0, F_Win32RestrictCursor = 0x00412d80,
    F_ScanDown = 0x004130e0, F_ProfReset = 0x00413770, F_ProfResetAndReport = 0x00413780, F_KeyEnable = 0x00413cf0,
    F_KeyDisableAll = 0x00413d90, F_KeyEnableAll = 0x00413db0, F_vxdLoad = 0x00418340, F_vxdUnload = 0x00418460,
    F_KernelBegin = 0x00418620, F_KernelEnd = 0x00418710, F_UsefulBegin = 0x00419560, F_UsefulEnd = 0x00419590,
    F_ResourceSetMustLoad = 0x00419a30, F_ResourceMinimizeAll = 0x00419b60, F_ResourceMaximizeAll = 0x00419ba0,
    F_ResourceSetUnload = 0x00419bb0, F_LocaleBegin = 0x0041a7f0, F_LocaleEnd = 0x0041ab00, F_LocaleMoney = 0x0041ab50,
    F_LocaleFormatShortDate = 0x0041ac10, F_Base64ToMem = 0x0041b020,
    F_MemToBase64 = 0x0041b150, F_StringTableGet = 0x0041b210, F_StringTableForget = 0x0041b270,
    F_StringTableNumRows = 0x0041b290, F_StringTableGetEntry = 0x0041b2a0, F_AIDisplayLearnMode = 0x0041cdf0,
    F_AIFGUpdate = 0x0041d190, F_AILearnMode = 0x0041d340, F_AIDriverConfig = 0x0041f050, F_AIDriverBegin = 0x00420bd0,
    F_AIDriverEnd = 0x00420c00, F_CenterLine_ctor = 0x00422400, F_CenterLine_dtor = 0x00422490,
    F_blimp_to_tv = 0x004267b0, F_RecordConsolidate = 0x0042a280, F_RecordGetRaceResult = 0x0042a8b0,
    F_RecordGetLapResult = 0x0042a940, F_PhysicsMaybeRun = 0x0042ba20, F_PhysicsRestart = 0x0042bb50,
    F_PhysicsPostRace = 0x0042bba0, F_PhysicsAbortRace = 0x0042bc00, F_PhysicsPause = 0x0042bcc0,
    F_PhysicsUnpause = 0x0042bcf0, F_PhysicsGetSpeed = 0x0042bd40, F_PhysicsReadControls = 0x0042be10,
    F_CameraSetType = 0x0042bea0, F_PhysicsTimeString = 0x0042bf30, F_gxBegin = 0x0044deb0, F_gxEnd = 0x0044dee0,
    F_gxSetMode = 0x0044df20, F_gxRestoreMode = 0x0044e000, F_gxDisableSecondary = 0x0044e1b0, F_mrBegin = 0x0044e7e0,
    F_mrEnd = 0x0044e800, F_gxCanvasGet = 0x0044fd40, F_gxCanvasForget = 0x0044fdc0, F_gxPasteAlpha = 0x00451550,
    F_mrModelUnload = 0x00455950, F_WorldBegin = 0x00461dd0, F_WorldEnd = 0x00461ef0, F_WorldSwitchToUI = 0x00461f40,
    F_WorldUpdate = 0x00462100, F_WorldDraw = 0x00462200, F_WorldDraw2D = 0x00462570, F_WorldNextFocusCar = 0x00462690,
    F_WorldPrevFocusCar = 0x004626b0, F_WorldSetFocusCar = 0x004626f0, F_WorldGetPlayerCar = 0x00462710,
    F_WorldGetTrackNumber = 0x00462760, F_WorldGameOptions = 0x004627a0, F_World_ctor = 0x00462a90,
    F_CarMgrCount = 0x00464480, F_CarMgrGetInfo = 0x00464490, F_CarFileLoadSetupData = 0x004654b0,
    F_OptionsBegin = 0x00470ee0, F_OptionsFlush = 0x00471110, F_OptionsEnd = 0x00471330, F_TelemetryBegin = 0x004719a0,
    F_TelemetryEnd = 0x004719e0, F_TelemetrySetUpdate = 0x00471b20, F_SoundBegin = 0x00471cb0, F_SoundEnd = 0x00471d90,
    F_SoundMuteCars = 0x00471dd0, F_SoundUnMuteCars = 0x00471de0, F_SoundFlushAsync = 0x00471df0,
    F_UIAddDynamicStyle = 0x0047f4a0, F_MenuDoOptions = 0x004819a0,
    F_MenuDoRaceMenu = 0x00486aa0, F_BoardCustomText_ctor = 0x0048fd50, F_BoardCustomText_dtor = 0x0048fdb0,
    F_BoardCreateControl = 0x00490690, F_BoardDo = 0x00490740, F_MenuEditCar = 0x004911a0, F_PreLineBegin = 0x004a1670,
    F_PreLineEnd = 0x004a1680, F_MultiEnabled = 0x004a23c0, F_MultiIsServer = 0x004a23d0,
    F_MultiWorldIsLive = 0x004a2420, F_MultiWorldIsDead = 0x004a2430, F_MultiFGTick = 0x004a2690,
    F_MultiRestartRace = 0x004a2820, F_MultiPostRace = 0x004a2830, F_MultiMenuEscape = 0x004a2a80,
    F_MultiDedicatedServer = 0x004a36a0, F_EditMenu = 0x004b0a30, F_CareerDo = 0x004bc570,
    F_PaintKitInstallPaintJobs = 0x004c8600, F_CreditsDo = 0x004cc600, F_IntroPlayVideo = 0x004cccd0,
    // the game's C runtime
    F_strncmp = 0x004ce0e0, F_sscanf = 0x004ce190, F_CIacos = 0x004cf07c, F_qsort = 0x004cf160, F_strstr = 0x004cf9a0,
    F_strnicmp = 0x004da3e0,
    // the 3D renderer, the widget toolkit's UICustomControl, the styles
    F_mrSetView_c = 0x0044ecd0, F_mrSetProjection_c = 0x0044ede0, F_mrSetCamera_c = 0x0044ee70,
    F_mrModelDraw_c = 0x00455d00, F_mrModelLoadRemap = 0x00455900, F_WorldGetCarTexture = 0x00462070,
    F_UICC_Dirty_c = 0x0047e820, F_UICC_AddNotification_c = 0x0047e840, F_UICC_RemoveNotification_c = 0x0047e860,
    F_UICC_AddItems_c = 0x0047e870, F_UIRemoveStyle_c = 0x0047f4e0, F_UIHideGroup_c = 0x004791b0,
    F_UIShowGroup_c = 0x004791e0,
};
// UIDialogItem::UIDialogItem (replay.obj) by address: every argument as the original pushes it (the floats' bits)
static __forceinline void rb_item_ctor(void* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                                       const char* text, int32_t i1c, const void* data, int32_t style, uint32_t lo = 0,
                                       uint32_t hi = 0, const void* sel = 0, const char* s34 = 0) {
    uit::tcall<void*>(uit::F_UIDialogItem_ctor, it, type, id, x, y, w, h, text, i1c, data, style, lo, hi, sel, s34);
}
// a function-local static Xlator, built on first use: the guard's bit set, the constructor, its destructor at exit
static __forceinline void rb_xl_once(uint32_t guard, uint8_t bit, uint32_t xl, uint32_t key, uint32_t dtor) {
    if (!(UI_G8(guard) & bit)) {
        UI_G8(guard) = (uint8_t)(UI_G8(guard) | bit);
        uit::tcall<void*>(uit::F_Xlator_ctor, (void*)(uintptr_t)xl, (const char*)(uintptr_t)key);
        uit::ccall<int>(uit::F_atexit, dtor);
    }
}

}  // namespace rootb
