// root_replay.h -- M3 UI stage, step U3 (group C): the replay screen's and the ghost cars' layouts (library `root`:
// replay.obj, ghost.obj), shared by hook/root_replay.cpp, hook/root_ghost.cpp and test/world_root_replay.cpp.
//
// Recovered from the v1.0 disassembly (ReplayDo's and do_analysis's frames, the constructors and the Draw / MouseMove
// methods; ghost.obj's LiveGhost handling and its file header as save() builds it). Fields are volatile, as in
// ui_types.h: the rewrites read and write each one where the original does, in its order.
//
// Classes (vtable, size):
//   UICustomControl 0x4db190 0x18   the base (its do-nothing methods are replay.obj's: 0x409230..0x409350)
//   JogControl 0x4db140 0x28        the jog wheel: dragged around, it shuttles the replay (PhysReplayShuttle)
//   ReplayView 0x4db0f0 0x40        the replay screen's control: the transport, camera and car buttons (Added), the 3D
//                                   view behind (Draw3D), and a JogControl of its own at +0x18
//   GraphControl 0x4db1e0 0x18      the analysis screen's telemetry bars (PhysReplayGetTelemetry)
//   LiveGhost (no vtable) 0x23458   a ghost car's lap: 0x960 replay packets and its file's header
//   GhostFile (no vtable) 0x1cc + packets: the ".gcf" file (the header, then the packets)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace rrep {

struct RvCtl {                                    // UICustomControl's fields (ui_types.h)
    const void* volatile vtbl;                    // +0
    volatile int32_t x, y, w, h;                  // +4 the item's rectangle (_UIAddItems)
    void* volatile widget;                        // +0x14 its CustomWidget
};
static_assert(sizeof(RvCtl) == 0x18, "RvCtl");

// ---- replay.obj ---------------------------------------------------------------------------------------------------------
struct JogControl : RvCtl {                       // 0x28
    void* volatile detent;                        // +0x18 jdetent.stp (the knob, drawn on the rim)
    void* volatile wheel;                         // +0x1c jwheel.stp
    volatile uint8_t dragging;                    // +0x20 the mouse went down on it
    uint8_t _21[3];
    volatile int32_t _24;                         // +0x24 (0; never read)
};
static_assert(sizeof(JogControl) == 0x28, "JogControl");

struct ReplayView : RvCtl {                       // 0x40
    JogControl jog;                               // +0x18
};
static_assert(sizeof(ReplayView) == 0x40 && offsetof(ReplayView, jog) == 0x18, "ReplayView");

struct GraphControl : RvCtl {};                   // 0x18
static_assert(sizeof(GraphControl) == 0x18, "GraphControl");

// ---- ghost.obj ----------------------------------------------------------------------------------------------------------
enum : uint32_t { PACKET_BYTES = 0x3c, MAX_PACKETS = 0x960, HEADER_BYTES = 0x1cc, LIVE_GHOST_BYTES = 0x23458 };

// the file's header (0x1cc; save() builds it on its own stack, whose bytes 0xec..0x1c0 it never writes)
struct GhostHeader {
    volatile uint32_t version;                    // +0 gf_version(track)
    volatile int32_t max_packets;                 // +4 0x960 (the static at 0x505b88)
    volatile int32_t size;                        // +8 the file's size (0x505ba4: 0x1cc + 0x960 * 0x3c)
    volatile int32_t realism;                     // +0xc
    char track[0xd];                              // +0x10 (strcpy)
    volatile uint8_t mirror;                      // +0x1d
    uint8_t _1e[2];
    uint8_t car_entry[0xc8];                      // +0x20 GhostInfo: the car's CarListEntry ...
    volatile float lap_time;                      // +0xe8 ... and its lap time (GhostInfo is 0xcc)
    uint8_t _ec[0x1c0 - 0xec];
    volatile int32_t ticks;                       // +0x1c0 GhostData: the lap's length in ticks,
    volatile int32_t start;                       // +0x1c4   the first packet's,
    volatile int32_t count;                       // +0x1c8   and how many
};
static_assert(sizeof(GhostHeader) == 0x1cc && offsetof(GhostHeader, car_entry) == 0x20 && offsetof(GhostHeader, ticks) == 0x1c0,
              "GhostHeader");

struct LiveGhost {                                // 0x23458
    volatile uint8_t tried;                       // +0 initialize ran (a file was looked for)
    volatile uint8_t valid;                       // +1 it holds a lap
    volatile uint8_t dirty;                       // +2 a new lap: save it
    uint8_t _3;
    volatile int32_t car;                         // +4 the car it's of
    volatile int32_t best;                        // +8 its lap's ticks (0x7fffffff: none)
    uint8_t packets[MAX_PACKETS * PACKET_BYTES];  // +0xc Car::ReplayPacket[0x960]
    GhostHeader h;                                // +0x2328c the file's header
};
static_assert(sizeof(LiveGhost) == LIVE_GHOST_BYTES && offsetof(LiveGhost, h) == 0x2328c, "LiveGhost");

// the game's World (wld_world.cpp), the fields read here
enum : uint32_t {
    W_TRACK = 0x8, W_CARS = 0x28, W_NCARS = 0xca8, W_REALISM = 0xcac, W_FIELD = 0xcb8, W_RACE_TYPE = 0xcbc,
    W_EVENT = 0xcc4, W_GHOST = 0xcc8, W_MIRROR = 0xcd1, W_SIZE = 0xcd4, CAR_ENTRY = 0xc8, CL_COUNT = 0xc80,
};

// ---- vtables ---------------------------------------------------------------------------------------------------------------
enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_JogControl = 0x004db140, VT_ReplayView = 0x004db0f0, VT_GraphControl = 0x004db1e0,
};

// ---- statics (v1.0) ----------------------------------------------------------------------------------------------------------
enum : uint32_t {
    // replay.obj
    S_HAVE_REPLAY = 0x004e4850,     // u8: a replay is loaded (the player plays it)
    S_SPEED = 0x004e4854,           // float: the focus car's speed (2.25 x |v|), the analysis screen's
    S_GEAR = 0x004e4858,            // int: its CarMessage's +0x34
    S_SHUTTLE = 0x004e485c,         // int: -1 / 1 while a shuttle button is held (this frame)
    S_SHUTTLE_LAST = 0x004e4860,    // int: ... the last frame's
    S_ELAPSED = 0x004e4864,         // float: WorldGetElapsedTime (the jog wheel watches it)
    S_FRAME = 0x004e4868,           // int: replay_idle's frames
    S_BENCH_FRAMES = 0x004e486c,    // int: the benchmark's
    S_GRAPH_SCALE = 0x0050468c,     // float (1)
    S_GRAPH_ZOOM = 0x00504690,      // float (5)
    S_GRAPH_COLOR = 0x00504694,     // the bars' colour (an $E initialiser's)
    S_REPLAY_WORLD = 0x005046a8,    // World (0xcd4): the replay's
    S_GRAPH_TYPE = 0x0050538c,      // int: 0 none, 1 longitudinal, 2 lateral, 3 speed
    S_GROUP_MAIN = 0x005053a0,      // the replay screen's groups (EnterGroup's)
    S_GROUP_PAUSED = 0x005053a4,
    S_GROUP_PLAYING = 0x005053a8,
    S_CAR_NAME = 0x005053f0,        // char[0x20]: the focus car's (strncpy 0x1f)
    S_VIEW = 0x00505434,            // ReplayView*: ReplayDo's (its own stack)
    S_REPLAY_FILE = 0x00505470,     // char[0x104]: the file to load (the open box's, or auto-load's)
    S_AUTOLOAD = 0x00505594,        // u8: ReplayDo was handed a file to open: replay_idle loads it once
    S_TIME_TEXT = 0x005055b8,       // char[0x50]: the elapsed time, as text
    S_FROM_RACE = 0x0050560c,       // u8: ReplayDo was handed the race's world (not the menus' replay screen)
    S_SHUTTLE_SPEED = 0x00505610,   // float: 1 .. 10, faster while a shuttle button stays held
    S_CAMERA = 0x00505664,          // int: the camera (0 in car .. 11 blimp)
    S_REPLAY_DIR = 0x005056a0,      // char[0x108]: "<user>replay\"
    S_CAMERA_NAME = 0x005057a8,     // char[0x20]: GetCameraName's
    S_FRAME8 = 0x005057e8,          // int: every other frame, to 8
    S_GROUP_ANALYSIS = 0x0050581c,  // a group (the analysis screen hides it)
    // the function-local Xlators' guards
    S_ONCE_REPLAYDO = 0x00505584, S_ONCE_IDLE = 0x0050539c, S_ONCE_LOADDLG = 0x00505438, S_ONCE_LOAD = 0x00505824,
    S_ONCE_SAVE = 0x005053ac, S_ONCE_ANALYSIS = 0x00505608, S_ONCE_AUTOLOAD = 0x0050541c,
    // ghost.obj
    S_INCOGNITO = 0x004e53f8,       // u8: the ghost car races as the pack's (options' field != 0)
    S_TARGET = 0x004e53fc,          // int: the player's car (-1: none)
    S_GF_VERSIONS = 0x004e5400,     // uint32[8]: a ghost file's version, by track number
    S_MUNGE = 0x004e5470,           // {char const* name, char const* short}[2]
    S_REALISM_CHARS = 0x004e5644,   // "ais"
    S_NO_CAR = 0x004e5648,          // "____"
    S_GCF = 0x004e5650,             // ".gcf"
    S_GHOST_DIR_FMT = 0x004e558c,   // "%sghostcar"
    S_GHOST_PATH_FMT = 0x004e5634,  // "%sghostcar\%s"
    S_MAX_PACKETS = 0x00505b88,     // int: 0x960 ($E59)
    S_TASK = 0x00505bb8,            // the submitter's task (0: never made)
    S_SUBMIT = 0x00505bc0,          // LiveGhost*: the lap to submit (never allocated)
    S_FILE_SIZE = 0x00505ba4,       // int ($E62)
    S_BEST = 0x00505bc4,            // LiveGhost*: the best lap of all (GhostCarType 2)
    S_CURRENT = 0x00505bc8,         // LiveGhost*: the one raced against
    S_SLOTS = 0x00505bcc,           // int[16]: each car's LiveGhost (0x7fffffff: not ghostable)
    S_NGHOSTS = 0x00505c0c,         // int
    S_GHOSTS = 0x00505c10,          // LiveGhost*: [S_NGHOSTS]
};

// ---- the game's functions (v1.0) -------------------------------------------------------------------------------------------
enum : uint32_t {
    // root (other groups')
    F_move_blimp = 0x00401880, F_GetCameraName = 0x00406450, F_GetTrackNumber = 0x004068b0, F_LoadCar = 0x0040a940,
    F_UnloadCar = 0x0040a960, F_LoadCars = 0x0040a980, F_UnloadCars = 0x0040a9d0, F_SplashLoading = 0x0040cbc0,
    // kernel
    F_FileCreate = 0x004115f0, F_FileSize = 0x00411890, F_FileReadExact = 0x004118b0, F_FileWrite = 0x00411a30,
    F_FileSetWritable = 0x00411b80, F_FileRemove = 0x00411ba0, F_FileCreateDirectory = 0x00411d00,
    F_Win32GetUserDirectory = 0x00412cc0, F_ScanDown = 0x004130e0, F_ProfReset = 0x00413770,
    F_ProfResetAndReport = 0x00413780, F_MemFree = 0x00414300, F_MousePeekEvent = 0x004144b0, F_TaskDestroy = 0x00414ac0,
    F_TaskResume = 0x00414d40, F_TaskSleep = 0x00414de0, F_ResourceSetMustLoad = 0x00419a30,
    F_ResourceSetUnload = 0x00419bb0,
    // physics / world
    F_AIGetMaxGhostSubmitTicks = 0x0041d350, F_PhysicsGetTime = 0x0042bc80, F_CameraSetType = 0x0042bea0,
    F_PhysicsTimeString = 0x0042bf30, F_PhysReplayPlayBegin = 0x0042ca10, F_PhysReplayPlayEnd = 0x0042ca50,
    F_PhysReplayRewind = 0x0042ca70, F_PhysReplayPause = 0x0042cab0, F_PhysReplayResume = 0x0042cae0,
    F_PhysReplayIsPaused = 0x0042cb20, F_PhysReplayIsAtEnd = 0x0042cb30, F_PhysReplayFastForward = 0x0042cb80,
    F_PhysReplayShuttle = 0x0042cbe0, F_PhysReplayLoad = 0x0042cc80, F_PhysReplaySave = 0x0042ce50,
    F_PhysReplayGetTelemetry = 0x0042d240, F_feed_ghost_car = 0x0042d9d0, F_GhostCar_NewLap = 0x0042ddc0,
    F_mrSetView = 0x0044ecd0, F_mrModelFlush = 0x004570f0, F_WorldBeginReplay = 0x00461db0,
    F_WorldEndReplay = 0x00461f20, F_WorldSwitchToDrive = 0x00461f30, F_WorldSwitchToUI = 0x00461f40,
    F_WorldGetElapsedTime = 0x00462050, F_WorldUpdate = 0x00462100, F_WorldDraw = 0x00462200,
    F_WorldNextFocusCar = 0x00462690, F_WorldPrevFocusCar = 0x004626b0, F_WorldGetFocusCar = 0x004626d0,
    F_WorldSetFocusCar = 0x004626f0, F_WorldGetPlayerCar = 0x00462710, F_WorldGetCarEntry = 0x00462720,
    F_WorldGetTrackname = 0x00462780, F_WorldGameOptions = 0x004627a0, F_WorldFXEnable = 0x00462850,
    F_WorldFXDisable = 0x00462870, F_World_is_valid_version = 0x00462a70, F_CarMgrGetInfo = 0x00464490,
    F_TelemetryBegin = 0x004719a0, F_TelemetryEnd = 0x004719e0, F_SoundMuteCars = 0x00471dd0,
    F_SoundUnMuteCars = 0x00471de0,
    // ui
    F_UIDisableGroup = 0x00479210, F_UIEnableGroup = 0x00479240, F_UIDoOkBox = 0x004793c0,
    F_UIDoOpenFileBox = 0x0047a6e0, F_UIDoSaveFileBox = 0x0047ac30, F_UICC_Dirty = 0x0047e820,
    F_UICC_AddNotification = 0x0047e840, F_UICC_AddItems = 0x0047e870,
    // multiplayer
    F_MultiEnabled = 0x004a23c0, F_MultiCarIsHuman = 0x004a2b30,
    // the game's C runtime
    F_CIfmod = 0x004cf36a, F_ftol = 0x004cf108, F_finite = 0x004cf550, F_isnan = 0x004cf570, F_stricmp = 0x004da350,
    // this group's own (called by address, as the original does, so a hooked rewrite is what runs)
    F_ReplayDo = 0x00406bc0, F_replay_idle = 0x00407330, F_update_shuttle = 0x00407540, F_fullscreen_cb = 0x00407600,
    F_ReplayLoadDialog = 0x004078d0, F_load_cb = 0x004079d0, F_save_cb = 0x00407ed0, F_create_replay_dir = 0x004080e0,
    F_play_cb = 0x00408130, F_rewind_to_start_cb = 0x00408170, F_rewind_cb = 0x00408190, F_ff_cb = 0x004081a0,
    F_ff_to_end_cb = 0x004081b0, F_stop_cb = 0x004081d0, F_car_cb = 0x004081f0, F_CAMERA_IN_CAR_cb = 0x00408210,
    F_next_view_cb = 0x00408320, F_prev_view_cb = 0x00408350, F_player_car_cb = 0x00408370, F_next_car_cb = 0x00408390,
    F_prev_car_cb = 0x004083b0, F_do_analysis = 0x004083d0, F_benchmark_loop = 0x00409130,
    F_JogControl_ctor = 0x00409370, F_UI_BBUTTON = 0x00409d20, F_UI_BREPEATBUTTON = 0x00409d70, F_UI_CUSTOM = 0x00409dc0,
    F_munge_carname_to_4char = 0x0040dc70, F_add_ghost_car = 0x0040ddd0, F_set_ghostcar_entry = 0x0040de20,
    F_create_ghostcar_dir = 0x0040dea0, F_find_target = 0x0040ded0, F_load_old_stats = 0x0040df00,
    F_initialize = 0x0040e280, F_load = 0x0040e320, F_get_full_pathname = 0x0040e3f0, F_get_filename = 0x0040e440,
    F_pre_worldbegin_is_ghostable_car = 0x0040e4e0, F_module_ok = 0x0040e540, F_save_ghostcars = 0x0040e550,
    F_try_saving = 0x0040e5a0, F_save_type = 0x0040e5e0, F_save_path = 0x0040e650, F_gf_version = 0x0040e7a0,
    F_convert_ticks_to_time = 0x0040e7c0, F_is_savable_ghost = 0x0040e7e0, F_remove_ghostcar = 0x0040e890,
    F_GetGhostInfo = 0x0040e8f0, F_ASSERT_MSG = 0x0040eb30, F_is_ghostable_car = 0x0040eb40, F_submit_ghost = 0x0040eb70,
    F_GhostLoad = 0x0040ebe0, F_ghost_appears_valid = 0x0040ed40, F_point_is_normal = 0x0040ede0, F_VERBOSE = 0x0040eea0,
    F_LiveGhost_ctor = 0x0040eeb0, F_ReplayPacket_ctor = 0x0040eef0,
    // the atexit helpers of the function-local Xlators (their addresses, as data)
};

}  // namespace rrep
