// menu_sched.h -- multiplayer stage N3 (group B): the layouts of msched.obj's controls (the lobby), its statics, and the
// game functions hook/menu_sched.cpp and test/world_menu_sched.cpp share.
//
// Read off the v1.0 disassembly (out/types.json has next to nothing here): each size is the room MultiDo's frame gives the
// object, or MultiMaster's MemAlloc for MultiRaceCfg; every one is static_assert'ed. Every field is volatile: the rewrites
// read and write each one where the original does, in its order. The toolkit (UICustomControl, UIDialogItem) is
// hook/ui_types.h's; the multiplayer objects the lobby talks to (LiveMultiInfo, RaceClient, RaceServer, SessionMgr) are
// reached through their vtables and by v1.0 address, never looked into but for the few fields named here.
//
//   ChatControl 0xd4 (vtable 0x4de758)        the user list and the chat line (a ChatWindow inside, at +0xa8)
//   ChatWindow 0x20 (0x4de7a8)                 the chat scrollback
//   MultiMaster 0x98 (0x4de7f8)                the lobby's state machine: its buttons' groups, the host's proposal
//   MultiCarChooser 0x160 (0x4de848)           the car (a CarViewer3D inside, at +0x3c), Prev / Next / Setup
//   MultiTrackChooser 0x16c (0x4de898)         the track (the host's arrows), its picture and name
//   MultiRaceCfg 0x8c (0x4de8e8)               the host's race settings (MemAlloc'd by MultiMaster for the host)
//   MultiRaceInfo 0xb0 (0x4de938)              the proposal as text (everybody's)
//   LiveMultiKiller 0x1c (0x4de988)            MultiDo's: ends the live game (or grabs it back) when the dialog closes
//   MultiGenesisInfo                           what the Multiplayer screen hands MenuMultiScheduler: group A's
//                                              (hook/menu_multi.h), read by its offsets here
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "net_race.h"
#include "menu_multi.h"

namespace msc {
using namespace uit;

struct ChatWindow : UICustomControl {             // 0x20, vtable 0x4de7a8
    void* volatile client;                        // +0x18 IRaceClient*
    const void* volatile last;                    // +0x1c the scrollback's newest line when last drawn
};
static_assert(sizeof(ChatWindow) == 0x20, "ChatWindow");

struct ChatControl : UICustomControl {            // 0xd4, vtable 0x4de758
    volatile uint8_t dragging;                    // +0x18 the mouse is down on the user list
    uint8_t _19[3];
    volatile int32_t sel;                         // +0x1c the user picked to whisper to (an index), -1 none
    void* volatile client;                        // +0x20 IRaceClient*
    char text[0x80];                              // +0x24 the chat line (an Input's buffer, watched)
    void* volatile stamp;                         // +0xa4 'multi_s.stp' (a user's status)
    ChatWindow window;                            // +0xa8
    volatile int32_t users;                       // +0xc8 the user count when last seen
    volatile int32_t sel_id;                      // +0xcc the picked user's id
    volatile uint32_t grp_whisper;                // +0xd0 the Whisper button's group
};
static_assert(sizeof(ChatControl) == 0xd4 && offsetof(ChatControl, text) == 0x24 && offsetof(ChatControl, window) == 0xa8,
              "ChatControl");

struct MultiMaster : UICustomControl {            // 0x98, vtable 0x4de7f8
    uint8_t proposal[0x3c];                       // +0x18 NetProposal: +4 the cars are slaved (the host's choice: IROC),
                                                  //       +8 NetRaceInfo (0x34; the track at +0x28 = proposal +0x30)
    void* volatile cfg;                           // +0x54 MultiRaceCfg* (the host's), or 0
    void* volatile client;                        // +0x58 IRaceClient*
    void* volatile server;                        // +0x5c IRaceServer* (the host's), or 0
    void* volatile sm;                            // +0x60 SessionMgr*
    void* volatile lmi;                           // +0x64 LiveMultiInfo*
    volatile uint32_t grp_running;                // +0x68 the groups: the lobby (chat) state
    volatile uint32_t grp_carchoice;              // +0x6c choosing a car
    volatile uint32_t grp_preconn;                // +0x70 waiting for the connection
    volatile uint32_t grp_approve;                // +0x74 the Approve button
    volatile uint32_t grp_car;                    // +0x78 Cancel car / Race
    volatile uint32_t grp_track;                  // +0x7c Cancel track (waiting for the others' cars)
    volatile uint32_t grp_glint;                  // +0x80 the Approve button's glint
    volatile int32_t glint_frame;                 // +0x84
    volatile int32_t config_time;                 // +0x88 PTimeNow when the host last changed the race (0: sent)
    volatile int32_t approve_time;                // +0x8c PTimeNow when approval became possible (the glint after 5 s)
    volatile int32_t last_state;                  // +0x90 the client's state last frame
    volatile uint8_t done;                        // +0x94 the dialog ends (Idle)
    volatile uint8_t approved;                    // +0x95
    volatile uint8_t config_dirty;                // +0x96 the host's settings aren't sent yet
    uint8_t _97;
};
static_assert(sizeof(MultiMaster) == 0x98 && offsetof(MultiMaster, cfg) == 0x54 && offsetof(MultiMaster, grp_running) == 0x68 &&
              offsetof(MultiMaster, done) == 0x94, "MultiMaster");

struct MultiCarChooser : UICustomControl {        // 0x160, vtable 0x4de848
    volatile uint8_t arrows;                      // +0x18 Prev / Next shown
    volatile uint8_t garage;                      // +0x19 Setup shown
    char name[0x22];                              // +0x1a the car's friendly name
    uint8_t viewer[0x110];                        // +0x3c CarViewer3D (group C's): its car's index at +0x18
    volatile uint32_t grp_arrows;                 // +0x14c
    volatile uint32_t grp_garage;                 // +0x150
    volatile uint32_t grp_all;                    // +0x154
    volatile uint32_t grp_text_a;                 // +0x158 shown when neither arrows nor Setup are
    volatile uint32_t grp_text_b;                 // +0x15c shown otherwise
};
static_assert(sizeof(MultiCarChooser) == 0x160 && offsetof(MultiCarChooser, viewer) == 0x3c &&
              offsetof(MultiCarChooser, grp_arrows) == 0x14c, "MultiCarChooser");
enum : uint32_t { CV_CAR = 0x18 };                // CarViewer3D: the car's index

struct MultiTrackChooser : UICustomControl {      // 0x16c, vtable 0x4de898
    char text[0x40];                              // +0x18 the track's name (and " - " the reversed text)
    char race_text[0x24];                         // +0x58 set_race_number's
    volatile int32_t count;                       // +0x7c GetTrackCount() - 2
    volatile int32_t track;                       // +0x80
    volatile int32_t saved;                       // +0x84 the MULTI "track" option (read, saved, set by Prev / Next)
    char names[16][13];                           // +0x88 the tracks' names (count of them, unbounded)
    void* volatile stamp;                         // +0x158 the track's picture ('<name>.stp')
    void* volatile frame;                         // +0x15c 'carfrm.stp'
    volatile uint8_t host;                        // +0x160 the arrows (the host only)
    uint8_t _161[3];
    volatile uint32_t grp_arrows;                 // +0x164
    volatile uint32_t grp_track;                  // +0x168
};
static_assert(sizeof(MultiTrackChooser) == 0x16c && offsetof(MultiTrackChooser, count) == 0x7c &&
              offsetof(MultiTrackChooser, names) == 0x88 && offsetof(MultiTrackChooser, stamp) == 0x158 &&
              offsetof(MultiTrackChooser, grp_arrows) == 0x164, "MultiTrackChooser");

struct MultiRaceCfg : UICustomControl {           // 0x8c, vtable 0x4de8e8 (MemAlloc 0x8c)
    // +0x18 NetRaceInfo (0x34, watched)
    volatile int32_t realism;                     // +0x18
    volatile int32_t time_of_day;                 // +0x1c
    volatile int32_t weather;                     // +0x20
    volatile int32_t i24;                         // +0x24 (3)
    volatile int32_t race_type;                   // +0x28
    volatile int32_t laps;                        // +0x2c
    volatile int32_t i30, i34;                    // +0x30
    volatile int32_t strength;                    // +0x38 opponent_strength
    volatile uint8_t damage;                      // +0x3c damage_on
    volatile uint8_t reversed;                    // +0x3d
    uint8_t _3e[2];
    volatile int32_t i40;                         // +0x40
    volatile int32_t opponents;                   // +0x44 (the AI cars)
    volatile int32_t i48;                         // +0x48
    volatile uint8_t iroc;                        // +0x4c the cars are slaved (watched)
    uint8_t _4d[3];
    volatile float opp_f;                         // +0x50 the AI cars' slider (watched)
    volatile float type_f;                        // +0x54 the race type's slider (watched)
    char opp_text[0x10];                          // +0x58 "Multi:AI_Cars: <n>"
    char laps_text[0x18];                         // +0x68 "GameOptions:Laps: <n>"
    volatile uint32_t grp_opp;                    // +0x80 the opponents' strength (shown with AI cars)
    volatile uint32_t grp_all;                    // +0x84
    volatile uint32_t grp_b;                      // +0x88
};
static_assert(sizeof(MultiRaceCfg) == 0x8c && offsetof(MultiRaceCfg, opponents) == 0x44 && offsetof(MultiRaceCfg, iroc) == 0x4c &&
              offsetof(MultiRaceCfg, opp_text) == 0x58 && offsetof(MultiRaceCfg, grp_opp) == 0x80, "MultiRaceCfg");

struct MultiRaceInfo : UICustomControl {          // 0xb0, vtable 0x4de938
    char title[0x10];                             // +0x18 "Multi:Enabled" (the cars slaved) or ""
    char realism[0x10];                           // +0x28
    char damage[0x10];                            // +0x38
    char time[0x10];                              // +0x48
    char weather[0x10];                           // +0x58
    char type[0x18];                              // +0x68 "<race type>: <n> <Lap(s)>"
    char count[0x10];                             // +0x80 the AI cars (NumberString)
    char strength[0x10];                          // +0x90
    volatile uint32_t grp_info;                   // +0xa0
    volatile uint32_t grp_all;                    // +0xa4
    volatile uint32_t grp_opp;                    // +0xa8
    volatile int32_t gc_type;                     // +0xac the MULTI "gc_type" option (the ghost car)
};
static_assert(sizeof(MultiRaceInfo) == 0xb0 && offsetof(MultiRaceInfo, grp_info) == 0xa0 && offsetof(MultiRaceInfo, gc_type) == 0xac,
              "MultiRaceInfo");

struct LiveMultiKiller : UICustomControl {        // 0x1c, vtable 0x4de988 (MultiDo's frame)
    void* volatile lmi;                           // +0x18
};
static_assert(sizeof(LiveMultiKiller) == 0x1c, "LiveMultiKiller");

// LiveMultiInfo (net_race.h's nr::LiveMultiInfo): the fields the lobby reads and writes (+0x10: the game's name, 32 bytes)
enum : uint32_t { LMI_LIVE = 0x00, LMI_SERVER = 0x04, LMI_CLIENT = 0x08, LMI_SM = 0x0c, LMI_NAME = 0x10 };
static_assert(offsetof(nr::LiveMultiInfo, live) == LMI_LIVE && offsetof(nr::LiveMultiInfo, server) == LMI_SERVER &&
              offsetof(nr::LiveMultiInfo, client) == LMI_CLIENT && offsetof(nr::LiveMultiInfo, sm) == LMI_SM &&
              offsetof(nr::LiveMultiInfo, task) == LMI_NAME + 0x20, "LiveMultiInfo");
// RaceClient (net_race.h): the fields read directly: its state, the reason it went down
enum : uint32_t { RC_STATE = 0x04, RC_REASON = 0x08 };
static_assert(offsetof(nr::RaceClient, state) == RC_STATE && offsetof(nr::RaceClient, reason) == RC_REASON, "RaceClient");
// MultiGenesisInfo (group A's, hook/menu_multi.h): +0 ISocket*, +4 the host, +5 named (the client connects by the name at
// +0x18; else to the RemoteService there), +6 the password, +0x18 the service / the game's name
enum : uint32_t {
    MGI_SOCKET = offsetof(mmu::MultiGenesisInfo, sock), MGI_HOST = offsetof(mmu::MultiGenesisInfo, is_server),
    MGI_BY_NAME = offsetof(mmu::MultiGenesisInfo, named), MGI_PASSWORD = offsetof(mmu::MultiGenesisInfo, password),
    MGI_NAME = offsetof(mmu::MultiGenesisInfo, service),
};
static_assert(MGI_SOCKET == 0 && MGI_HOST == 4 && MGI_BY_NAME == 5 && MGI_PASSWORD == 6 && MGI_NAME == 0x18, "MultiGenesisInfo");
// NetProposal / NetRaceInfo offsets
enum : uint32_t {
    NP_IROC = 0x04, NP_RACE = 0x08, NP_TRACK = 0x30,
    NRI_REALISM = 0x00, NRI_TIME = 0x04, NRI_WEATHER = 0x08, NRI_TYPE = 0x10, NRI_LAPS = 0x14, NRI_STRENGTH = 0x20,
    NRI_DAMAGE = 0x24, NRI_REVERSED = 0x25, NRI_TRACK = 0x28, NRI_COUNT = 0x2c,
};

// ---- vtables ---------------------------------------------------------------------------------------------------------------
enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_ChatControl = 0x004de758, VT_ChatWindow = 0x004de7a8, VT_MultiMaster = 0x004de7f8,
    VT_MultiCarChooser = 0x004de848, VT_MultiTrackChooser = 0x004de898, VT_MultiRaceCfg = 0x004de8e8,
    VT_MultiRaceInfo = 0x004de938, VT_LiveMultiKiller = 0x004de988,
};
// IRaceClient's slots (net_race.h's names; the lobby's use)
enum : uint32_t {
    C_TICK = 0x04, C_ANY_USER_CHANGES = 0x10, C_GET_USER_STATUS = 0x14, C_COUNT_USERS = 0x18, C_FIRST_USER = 0x1c,
    C_NEXT_USER = 0x20, C_NTH_USER = 0x24, C_MY_USER_ID = 0x28, C_MAX_NAME_LENGTH = 0x2c, C_GET_USERNAME = 0x30,
    C_SPEAK = 0x38, C_WHISPER = 0x3c, C_GET_CHAT_SCROLLBACK = 0x40, C_PROCEED_TO_CHOOSE_CAR = 0x44, C_PROCEED_TO_RACE = 0x48,
    C_BACK_TO_CHOOSE_CAR = 0x4c, C_I_AM_HOLDING_THINGS_UP = 0x5c, C_GET_CAR = 0x60, C_GET_RACE_INFO = 0x64,
    C_GET_PROPOSAL = 0x68,
};
// IRaceServer's slots (RaceServer's vtable 0x4df720) the lobby calls
enum : uint32_t {
    S_TICK = 0x08, S_PROPOSE = 0x10, S_ALL_APPROVED = 0x14, S_APPROVE = 0x18, S_UNAPPROVE = 0x1c, S_SET_CAR = 0x20,
    S_SERVICE_ID = 0x30,
};

// ---- the game's functions (v1.0) -------------------------------------------------------------------------------------------
enum : uint32_t {
    // root (the front end)
    F_GetRealismString = 0x00405e40, F_GetRaceTypeString = 0x00405ec0, F_GetWeatherString = 0x004060a0,
    F_GetGameTimeString = 0x00406120, F_GetAIStrengthString = 0x004061a0, F_GetLapCountFromType = 0x004065f0,
    F_GetTrackCount = 0x00406800, F_GetTrackName = 0x00406810, F_GetTrackFriendlyName = 0x00406830,
    F_GetCarFileName = 0x00406910, F_GetMaxCarFileNames = 0x00406920, F_GetCarFileNumber = 0x00406930,
    F_HackEnabled = 0x0040d4b0, F_HackGetCarIndex = 0x0040d5a0,
    // kernel / useful
    F_ResourceSetMustLoad = 0x00419a30, F_ResourceSetUnload = 0x00419bb0,
    // state: the options
    F_OptionsGetI = 0x004713e0, F_OptionsSetI = 0x00471430, F_OptionsGetB = 0x00471470, F_OptionsSetB = 0x004714c0,
    // ui
    F_UIDoDratBox = 0x00479280, F_UIDoYesNoBox = 0x00479b30, F_CreateMultiString = 0x0047b410,
    F_UICC_Dirty = 0x0047e820, F_UICC_AddNotification = 0x0047e840, F_UICC_RemoveNotification = 0x0047e860,
    F_UICC_AddItems = 0x0047e870,
    // menus outside msched.obj
    F_MenuDoOptions = 0x004819a0, F_MenuEditCar = 0x004911a0,
    // group C's CarViewer3D
    F_CarViewer3D_ctor = 0x0049bf50, F_CarViewer3D_dtor = 0x0049c020, F_CarViewer3D_Next = 0x0049cdd0,
    F_CarViewer3D_SetCar = 0x0049cdf0, F_CarViewer3D_GetPaintJob = 0x0049ce30, F_CarViewer3D_Prev = 0x0049ce40,
    F_CarViewer3D_GetFriendlyName = 0x0049ce70, F_CarViewer3D_GetName = 0x0049ce90,
    // the multi library (rewritten in N1 / N2, called by address as the original does)
    F_MultiLiveEnd = 0x004a2360, F_MultiEnabled = 0x004a23c0, F_MultiResumeChat = 0x004a2840,
    F_LMI_Grab = 0x004a2d10, F_LMI_AmOwner = 0x004a2e10, F_LMI_Release = 0x004a2e50,
    F_SM_Tick = 0x004a50d0, F_SM_SetConnectPassword = 0x004a7570, F_CreateSessionMgr = 0x004a7670,
    F_GetClientStatusString = 0x004a7bf0, F_CreateRaceClient_rs = 0x004aa240, F_CreateRaceClient_name = 0x004aa290,
    F_CreateRaceClient_id = 0x004aa2e0, F_ClientDisconnectString = 0x004aa330, F_CreateRaceServer = 0x004ad0b0,
    // msched.obj (called by address, as the original does, so a hooked rewrite is what runs)
    F_ChatControl_ctor = 0x00487d50, F_ChatControl_dtor = 0x00487dc0, F_ChatControl_speak = 0x00487eb0,
    F_break_chatline = 0x00487ef0, F_ChatControl_whisper = 0x00487f40, F_ChatControl_keep_user_focused = 0x004880e0,
    F_ChatWindow_ctor = 0x00488360, F_ChatWindow_dtor = 0x00488380,
    F_MultiMaster_ctor = 0x004884c0, F_MultiMaster_dtor = 0x00488590, F_MultiMaster_Tick = 0x004885d0,
    F_MultiMaster_tick = 0x004885e0, F_MultiMaster_update_glint = 0x00488810, F_MultiMaster_car_changed = 0x00488890,
    F_MultiMaster_config_changed = 0x004888c0, F_MultiMaster_exit_button = 0x00488910, F_MultiMaster_back_to_x = 0x004889e0,
    F_MultiMaster_approve = 0x00488a30, F_MultiMaster_race = 0x00488a80,
    F_sf_hang = 0x00489260, F_sf_noconn = 0x00489280, F_sf_preconn = 0x004893b0, F_i_running = 0x004893c0,
    F_sf_running = 0x004894b0, F_i_proceeding = 0x004894e0, F_sf_proceeding = 0x00489530, F_sf_carchoice = 0x00489540,
    F_MultiMaster_SlaveCar = 0x00489650, F_i_carchoice = 0x004896a0, F_sf_carwaiting = 0x004896b0,
    F_i_carwaiting = 0x004896f0, F_sf_carget = 0x00489700, F_MultiMaster_hide_everything = 0x00489710,
    F_MCC_set_arrows = 0x004897c0, F_MCC_set_garage = 0x004897f0, F_MCC_update_text_loc = 0x00489820,
    F_MCC_update_car = 0x00489870, F_MultiCarChooser_ctor = 0x004898b0, F_MultiCarChooser_dtor = 0x00489900,
    F_options_cb = 0x00489940, F_MCC_SetupCar = 0x004899b0,
    F_MultiTrackChooser_ctor = 0x00489d60, F_MultiTrackChooser_dtor = 0x00489e60, F_MTC_fillout = 0x00489ec0,
    F_MTC_set_race_number = 0x0048a1d0, F_NumberString = 0x0048a210, F_MTC_set_text_from_race = 0x0048a7a0,
    F_MTC_set_text_from_proposal = 0x0048a850, F_MTC_set_track_p = 0x0048a860, F_MTC_set_track = 0x0048a880,
    F_MTC_next = 0x0048a930, F_MTC_prev = 0x0048a960, F_MCC_PrevCar = 0x0048a990, F_MCC_NextCar = 0x0048a9f0,
    F_MultiRaceCfg_ctor = 0x0048aa50, F_MultiRaceCfg_dtor = 0x0048abf0, F_MRC_fillout = 0x0048af00,
    F_MultiRaceInfo_ctor = 0x0048b3c0, F_MultiRaceInfo_dtor = 0x0048b420, F_MRI_UpdateProposal = 0x0048b490,
    F_MRI_update_race = 0x0048b500, F_MultiDo = 0x0048bf60, F_MultiPostRaceDo = 0x0048c380, F_create_server = 0x0048c390,
    F_create_client = 0x0048c3c0, F_ChatControl_Speak = 0x0048c440, F_MultiMaster_Race = 0x0048c4a0,
    F_MultiMaster_BackToX = 0x0048c4b0, F_MultiMaster_Approve = 0x0048c4c0, F_MTC_Prev = 0x0048c520,
    F_MTC_Next = 0x0048c530, F_UI_SLIDER = 0x0048c570, F_MultiMaster_Idle = 0x0048c660,
    F_MultiMaster_ExitButton = 0x0048c670,
};

// ---- statics (v1.0) --------------------------------------------------------------------------------------------------------
enum : uint32_t {
    S_SEC_MULTI = 0x004de73c,                     // char*: "MULTI"
    S_SEC_MGAME = 0x004de740,                     // char*: "GAME"
    S_SHOWHIDE = 0x004f83d0,                      // void (*)(unsigned)[2]: UIHideGroup, UIShowGroup
    S_EMPTY_STR = 0x004e4db8,                     // ""
    S_CHAT_G = 0x0057a158,                        // ChatControl::g
    S_MASTER_G = 0x0057a030,                      // MultiMaster::g
    S_CARCH_G = 0x0057a044,                       // MultiCarChooser::g
    S_TRACKCH_G = 0x0057a1c0,                     // MultiTrackChooser::g
    S_RACECFG_G = 0x0057a02c,                     // MultiRaceCfg::g
    S_RACEINFO_G = 0x00579fb4,                    // MultiRaceInfo::g
    S_POSTRACE_LMI = 0x00579f90,                  // PostRace::lmi
    S_COL_FE4 = 0x00579fe4,                       // MultiTrackChooser::Draw's bar colour
    S_COL_1D4 = 0x0057a1d4,                       // a colour the text styles test against itself
    S_GHOST_MULTI = 0x0057a048,                   // MultiRaceInfo::Added's multi string (the ghost car types)
    // the function-local statics' guards
    S_CHAT_ONCE = 0x0057a0f0, S_EXIT_ONCE = 0x0057a000, S_MASTER_ONCE = 0x0057a2cc, S_NOCONN_ONCE = 0x0057a154,
    S_CARCH_ONCE = 0x00579f6c, S_NUM_ONCE = 0x0057a008, S_NUM_ONCE2 = 0x0057a0bc, S_RACECFG_ONCE = 0x0057a2d0,
    S_RACE_ONCE = 0x0057a1e4, S_RACEINFO_ONCE = 0x0057a230, S_MULTIDO_ONCE = 0x0057a134,
};

// NumberString's fifteen Xlators ("Number:Zero" .. "Number:Fourteen"), in its order
static const uint32_t k_number_xl[15] = {
    0x00579f50, 0x00579f10, 0x0057a280, 0x0057a290, 0x00579fc8, 0x0057a1c8, 0x0057a160, 0x0057a1b0,
    0x00579f60, 0x0057a190, 0x00579fb8, 0x0057a1a0, 0x0057a2b0, 0x0057a0a8, 0x00579ff0,
};

}  // namespace msc
