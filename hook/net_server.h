// net_server.h -- multiplayer stage N2, group B: the server's layouts (library `multi`: server.obj's RaceServer and its
// per-user ServerData, ded.obj's dedicated Server), read off the v1.0 disassembly (out/types.json has next to nothing
// here). net_server.cpp rewrites server.obj, net_ded.cpp ded.obj; test/world_net_server.cpp builds them. The race-logic
// layouts both ends share (the car info, the proposal) are written here as the server reads and writes them, in this
// header's own namespace (group A's net_race.h keeps the client's view); every one is static_assert'ed.
//
// The SessionMgr the server talks to is net_core.h's (netc::SessionMgr): the server reads its channels (+0x14), its
// reliable port (+0x68: GetEstRTLatency) and its socket (+0xc8: GetHeaderSize, GetBPS, MakeStr) directly.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nsv {

typedef int Edx;                                   // the unused edx of a __thiscall received as __fastcall

#pragma pack(push, 1)
// a sync sample (dispatch_SyncPacket): the round trip of one 0x37 / 0x38 exchange and the client's clock word
struct SyncSample {
    uint16_t rtt;                // +0  (PTimeNow - the send's, 16 bits)
    int32_t remote;              // +2  the client's reply word (packet +4)
};
static_assert(sizeof(SyncSample) == 6, "SyncSample");

// ServerData: one user's slot, 0x122 bytes (packed: the status is at +0x11)
struct ServerData {
    int32_t id;                  // +0x000  user id: index | session << 8 | serial << 16 (create_userid); 0 = empty
    char name[13];               // +0x004
    int32_t status;              // +0x011  UserStatus: 0 joined, 1 approved, 2 car chosen, 4 race ready, 5 synced, 6 go
    // ServerData::reset clears +0x15 .. +0x111
    int32_t dt;                  // +0x015  the client's clock minus the server's (sync), added to car packets' times
    int32_t sync_sent;           // +0x019  PTimeNow of the last 0x37 sent
    SyncSample samples[40];      // +0x01d
    int32_t nsamples;            // +0x10d
    uint8_t sync_acked;          // +0x111  the last 0x37 was acked (sync_rpi_cb)
    int32_t rtt;                 // +0x112  the averaged round trip
    int32_t send_interval;       // +0x116  ms between car packets to this user (calc_send_interval)
    int32_t last_send;           // +0x11a  PTimeNow of the last car packet sent (0 after sync)
    int32_t last_heard;          // +0x11e  PTimeNow of the last packet from it (keepalive)
};
static_assert(offsetof(ServerData, status) == 0x11 && offsetof(ServerData, dt) == 0x15 && offsetof(ServerData, samples) == 0x1d &&
              offsetof(ServerData, nsamples) == 0x10d && offsetof(ServerData, sync_acked) == 0x111 &&
              offsetof(ServerData, rtt) == 0x112 && offsetof(ServerData, last_heard) == 0x11e && sizeof(ServerData) == 0x122,
              "ServerData");

// NetCarInfo: a car as the client describes it (the 0x30 packet's body from byte 5; the 0x33 reply's), 0xd9 bytes
struct NetCarInfo {
    int32_t user;                // +0x00  the owner's user id (an AI car: the user next_user handed it to)
    int32_t user2;               // +0x04  (the server never reads it)
    char driver[13];             // +0x08  the owner's name (strcpy'd from its ServerData; AI cars "#!@^&*")
    char car[32];                // +0x15  the car's name (AI cars "viper")
    uint8_t _35[3];
    uint8_t setup[0x8c];         // +0x38  CarSetup (AI cars: CarFileMakeDefaultSetup)
    int32_t car_arg;             // +0xc4  the car's argument (net_race.h's CarSlot info +0xc0; AI cars: their index & 7)
    uint8_t hornball;            // +0xc8  (AI cars: 0; the server only stores and forwards it)
    uint8_t _c9[11];
    int32_t latency;             // +0xd4  NetCarInfoRequest's reply: GetEstRTLatency to the owner's session
    uint8_t human;               // +0xd8  add_ai_cars: 1 for the players' cars, 0 for its own
};
static_assert(offsetof(NetCarInfo, driver) == 8 && offsetof(NetCarInfo, car) == 0x15 && offsetof(NetCarInfo, setup) == 0x38 &&
              offsetof(NetCarInfo, car_arg) == 0xc4 && offsetof(NetCarInfo, hornball) == 0xc8 && offsetof(NetCarInfo, latency) == 0xd4 && offsetof(NetCarInfo, human) == 0xd8 &&
              sizeof(NetCarInfo) == 0xd9, "NetCarInfo");

// ServerNetCarInfo: a car and its latest car packet record, 0xf5 bytes
struct ServerNetCarInfo {
    NetCarInfo info;             // +0x00
    uint8_t rec[24];             // +0xd9  [car index][CarNetPacket, 23 bytes]: the time word at +0xda is the server's clock
    int32_t rec_time;            // +0xf1  PTimeNow when it came
};
static_assert(offsetof(ServerNetCarInfo, rec) == 0xd9 && offsetof(ServerNetCarInfo, rec_time) == 0xf1 &&
              sizeof(ServerNetCarInfo) == 0xf5, "ServerNetCarInfo");

// NetProposal: the race on offer (the 0x2a packet's body from byte 4), 0x3c bytes
struct NetProposal {
    int32_t id;                  // +0x00  RaceServer::Propose: PTimeNow (what an 0x2b approve quotes)
    int32_t iroc;                // +0x04  (RaceServer +0x10c8: tested as a byte)
    int32_t realism;             // +0x08  "arcade", "intermediate realism", "simulation"
    int32_t _0c, _10;
    int32_t _14;                 // +0x14  (the dedicated server: 3)
    int32_t race_type;           // +0x18  "sprint", "short", "long", "insane"
    int32_t laps;                // +0x1c  GetLapCountFromType
    int32_t _20, _24;
    int32_t ai_strength;         // +0x28
    uint8_t damage_on;           // +0x2c
    uint8_t is_reversed;         // +0x2d
    uint8_t _2e[2];
    int32_t track;               // +0x30
    int32_t aicars;              // +0x34
    int32_t tag;                 // +0x38  RaceServer::Propose: PTimeNow & 0xff
};
static_assert(offsetof(NetProposal, race_type) == 0x18 && offsetof(NetProposal, damage_on) == 0x2c &&
              offsetof(NetProposal, track) == 0x30 && offsetof(NetProposal, tag) == 0x38 && sizeof(NetProposal) == 0x3c, "NetProposal");

// RaceServer, 0x1198 bytes (CreateRaceServer's MemAlloc). vtable 0x4df720 (IRaceServer's 0x4df6e0 while constructing).
struct RaceServer {
    const void* vtbl;            // +0x0000
    ServerData users[8];         // +0x0004
    uint8_t _914[4];
    ServerNetCarInfo cars[8];    // +0x0918
    int32_t ncars;               // +0x10c0
    NetProposal prop;            // +0x10c4  the race on offer
    uint8_t proposing;           // +0x1100  a proposal is out (AddMe sends it to a newcomer)
    uint8_t _1101[3];
    uint8_t cars_locked;         // +0x1104  GET_CAR reached: a car can't be unchosen
    uint8_t _1105[3];
    int32_t serial;              // +0x1108  create_userid's counter
    int32_t base;                // +0x110c  the first session of the service (LocalService.lo)
    int32_t iroc_user;           // +0x1110  Propose's second argument
    uint32_t service;            // +0x1114
    void* sm;                    // +0x1118  SessionMgr*
    int32_t _111c[5];
    void* chat_cb;               // +0x1130  void (*)(void*, int user, const char*)
    void* chat_data;             // +0x1134
    uint8_t game[0x34];          // +0x1138  the proposal's +8.. as CHOOSE_CAR froze it
    int32_t track_version[8];    // +0x116c  (CreateTrackVersion: zeros; AddMe compares 32 bytes of it)
    int32_t state;               // +0x118c  RS_APPROVE 1, CLOSE_ENTRY 2, CHOOSE_CAR 3, GET_CAR 4, SYNC 5, PRESTAGE 6, STAGE 7,
                                 //          RACE 8
    int32_t last_tick;           // +0x1190
    int32_t owner;               // +0x1194  the task that may call it (SetOwner)
};
static_assert(offsetof(RaceServer, users) == 4 && offsetof(RaceServer, cars) == 0x918 && offsetof(RaceServer, ncars) == 0x10c0 &&
              offsetof(RaceServer, prop) == 0x10c4 && offsetof(RaceServer, proposing) == 0x1100 &&
              offsetof(RaceServer, cars_locked) == 0x1104 && offsetof(RaceServer, base) == 0x110c &&
              offsetof(RaceServer, sm) == 0x1118 && offsetof(RaceServer, chat_cb) == 0x1130 && offsetof(RaceServer, game) == 0x1138 &&
              offsetof(RaceServer, track_version) == 0x116c && offsetof(RaceServer, state) == 0x118c &&
              offsetof(RaceServer, owner) == 0x1194 && sizeof(RaceServer) == 0x1198, "RaceServer");

// ded.obj's Server: MultiDedicatedServer's frame (Server::global points at it), 0x44 bytes
struct Server {
    void* sm;                    // +0x00  SessionMgr*
    void* rs;                    // +0x04  IRaceServer*
    NetProposal prop;            // +0x08  the game it proposes (init_game_info, track_chat)
};
static_assert(offsetof(Server, prop) == 8 && sizeof(Server) == 0x44, "Server");
#pragma pack(pop)

// ---- IRaceServer's slots (byte offsets into the vtable) ----------------------------------------------------------------
enum : uint32_t {
    RS_DTOR = 0x00, RS_SetOwner = 0x04, RS_Tick = 0x08, RS_SendAll = 0x0c, RS_Propose = 0x10, RS_EveryoneApproved = 0x14,
    RS_ProceedToChooseCar = 0x18, RS_BackToChooseTrack = 0x1c, RS_SetIrocCar = 0x20, RS_EveryoneSynchronized = 0x24,
    RS_RestartRace = 0x28, RS_GoBackToChat = 0x2c, RS_GetService = 0x30, RS_CountUsers = 0x34, RS_SetChatCallback = 0x38,
    RS_GetPhase = 0x3c,
};
// the ISocket slots the server calls
enum : uint32_t { SOCK_MakeStr = 0x18, SOCK_GetHeaderSize = 0x2c, SOCK_GetBPS = 0x30 };

enum : uint32_t {
    VT_IRaceServer = 0x004df6e0, VT_RaceServer = 0x004df720,
    // server.obj's own functions (called by address: the hooked rewrite or the original runs)
    S_smart_strncpy = 0x004aa9e0, S_TrackCRC = 0x004aaa10, S_CreateTrackVersion = 0x004aaac0, S_ServerData_reset = 0x004aaad0,
    S_ctor = 0x004aaae0, S_dtor = 0x004aac30, S_Ok = 0x004aac80, S_everyone_in_state = 0x004aaca0,
    S_everyone_approved = 0x004aace0, S_user_disconnected = 0x004aad20, S_next_user = 0x004aad60,
    S_ASSERT_MSG = 0x004aadd0, S_check_for_dead_users = 0x004aade0, S_check_for_singleton_player = 0x004aae50,
    S_add_ai_cars = 0x004aae70, S_broadcast = 0x004aafe0, S_create_userid = 0x004ab030, S_make_unique_username = 0x004ab060,
    S_send_status_tab = 0x004ab0f0, S_send_userlist = 0x004ab130, S_rpi_cb = 0x004ab180, S_sync_rpi_cb = 0x004ab1a0,
    S_calc_send_interval = 0x004ab1d0, S_get_iroc_car = 0x004ab260, S_dispatch_AddMe = 0x004ab2c0, S_no_bogus_crcs = 0x004ab4c0,
    S_dispatch_UserInfoReq = 0x004ab4f0, S_dispatch_Speak = 0x004ab580, S_dispatch_Whisper = 0x004ab620,
    S_dispatch_Approve = 0x004ab710, S_dispatch_NetCarInfo = 0x004ab770, S_dispatch_NetCarInfoRequest = 0x004ab980,
    S_dispatch_RaceReady = 0x004abaa0, S_dispatch_Sync = 0x004abae0, S_sync_sample_cmp = 0x004abcd0,
    S_dispatch_GoReady = 0x004abcf0, S_dispatch_KeepAlive = 0x004abd30, S_deitycast = 0x004abd60, S_race_packet = 0x004abdf0,
    S_reset = 0x004abed0,
    S_APPROVE_CLOSE_ENTRY = 0x004ac170, S_CLOSE_ENTRY_APPROVE = 0x004ac1c0, S_CLOSE_ENTRY_CHOOSE_CAR = 0x004ac210,
    S_CHOOSE_CAR_APPROVE = 0x004ac310, S_CHOOSE_CAR_GET_CAR = 0x004ac3d0, S_GET_CAR_SYNC = 0x004ac480,
    S_SYNC_PRESTAGE = 0x004ac4f0, S_PRESTAGE_STAGE = 0x004ac540, S_STAGE_RACE = 0x004ac600, S_RACE_PRESTAGE = 0x004ac650,
    S_RACE_APPROVE = 0x004ac700,
    S_checkRS_APPROVE = 0x004ac7f0, S_checkRS_CLOSE_ENTRY = 0x004ac830, S_checkRS_CHOOSE_CAR = 0x004ac870,
    S_checkRS_GET_CAR = 0x004ac8c0, S_checkRS_SYNC = 0x004ac900, S_checkRS_PRESTAGE = 0x004ac960, S_checkRS_STAGE = 0x004ac9b0,
    S_checkRS_RACE = 0x004ac9d0, S_ServerNetCarInfo_ctor = 0x004ad200, S_CreateRaceServer = 0x004ad0b0,
    S_RestartRace = 0x004abf60, S_GoBackToChat = 0x004abfe0, S_ProceedToChooseCar = 0x004ac070,
    S_BackToChooseTrack = 0x004ac0f0, S_Propose = 0x004ac9e0, S_SendAll = 0x004acaf0, S_SetIrocCar = 0x004acc30,
    S_Tick = 0x004accc0, S_EveryoneSynchronized = 0x004acfe0, S_CountUsers = 0x004ad040, S_SetOwner = 0x004ad120,
    S_EveryoneApproved = 0x004ad130, S_GetService = 0x004ad150, S_SetChatCallback = 0x004ad160, S_GetPhase = 0x004ad180,
    // ded.obj's
    D_init_game_info = 0x004a3170, D_update_laps = 0x004a31c0, D_Server_Tick = 0x004a31f0, D_track_chat = 0x004a3270,
    D_contains_list = 0x004a3520, D_get_numeric = 0x004a3560, D_chat = 0x004a35f0, D_contains = 0x004a3680,
    D_ded_begin = 0x004a3950, D_ded_log_hook = 0x004a39e0, D_ded_end = 0x004a3a40, D_idle = 0x004a3a70,
    D_handle_input = 0x004a3b10, D_prompt_user = 0x004a3c90, D_spin = 0x004a3d20, D_chat_cb = 0x004a3d30,
};

// ---- the game's functions outside this group (v1.0) ----------------------------------------------------------------------
enum : uint32_t {
    F_LogReport = 0x00411150, F_LogPanic = 0x004112b0, F_LogInstallHook = 0x00411120, F_LogUninstallHook = 0x004110c0,
    F_VERBOSE = 0x0040eea0, F_VersionGetBuildString = 0x00410cb0,
    F_MemAlloc = 0x004140e0, F_op_delete = 0x00414390,
    F_TaskGetID = 0x00414dd0, F_TaskGetName = 0x00414ec0, F_TaskSleep = 0x00414de0, F_TaskCreate = 0x004149e0,
    F_TaskDestroy = 0x00414ac0, F_TaskShouldIDie = 0x00414db0,
    F_FileOpen = 0x00411780, F_FileReadExact = 0x004118b0, F_FileClose = 0x00411850,
    F_UsefulBegin = 0x00419560, F_UsefulEnd = 0x00419590, F_ResourceSetMustLoad = 0x00419a30, F_ResourceSetUnload = 0x00419bb0,
    F_RaceBegin = 0x00406690, F_RaceEnd = 0x004067c0, F_GetTrackName = 0x00406810, F_GetLapCountFromType = 0x004065f0,
    F_CarFileMakeDefaultSetup = 0x00465780,
    // multi library (N1)
    F_SocketCreateUDP = 0x004ae300, F_CreateSessionMgr = 0x004a7670, F_SM_Ok = 0x004a4f30, F_SM_dtor = 0x004a4f40,
    F_SM_CanDestroySafely = 0x004a4fc0, F_SM_Tick = 0x004a50d0, F_SM_Shutdown = 0x004a6260,
    F_SM_OfferUnboundService = 0x004a5870, F_SM_RefuseConnections = 0x004a5b80, F_SM_AcceptConnections = 0x004a5c20,
    F_SM_WithdrawService = 0x004a5cc0, F_SM_Disconnect = 0x004a60a0, F_SM_DisconnectService = 0x004a6180,
    F_SM_Send = 0x004a6370, F_SM_SendReliable = 0x004a6420, F_SM_UnboundRecv = 0x004a6590, F_SM_GetStatus = 0x004a66e0,
    F_SM_GetNextStatus = 0x004a6760, F_RDP_GetEstRTLatency = 0x004af4e0,
    // C runtime
    F_strstr = 0x004cf9a0, F_isdigit = 0x004cf760, F_isspace = 0x004cf790, F_atoi = 0x004cf990, F_tolower = 0x004cfd50,
    F_strnicmp = 0x004da3e0, F_stricmp = 0x004da350, F_sprintf = 0x004cf0a0, F_memmove = 0x004cf400, F_qsort = 0x004cf160,
    F_alldiv = 0x004cfea0,
};

// ---- statics (v1.0) -----------------------------------------------------------------------------------------------------
enum : uint32_t {
    G_SEND_CLASS_MASK = 0x0057d524,    // byte: server.obj's mask on a packet's class / flags nibbles (set by an $E)
    G_MULTI_VERSION = 0x004df354,      // int const MULTI_VERSION (0x25)
    G_PROTOCOL = 0x004fd818,           // 31 bytes AddMe compares the packet's +0x11 with: MULTI_VERSION then the game packets' sizes ($E59)
    // ded.obj
    G_HIN = 0x004fb59c, G_HOUT = 0x004fb5a0,      // the console's handles (GetStdHandle)
    G_QUIT = 0x004fb5a4,               // byte: "quit" typed
    G_CHAT_CONFIG = 0x004fb5a8,        // byte: users may change the settings through chat ("y/n", not 'n')
    G_MSG = 0x0057d2a0,                // Server::g_msg: a console line for the main thread's Server::Tick to chat
    G_GLOBAL = 0x0057d2ac,             // Server::global
};

// ---- import slots (v1.0 IAT, KERNEL32) -----------------------------------------------------------------------------------
enum : uint32_t {
    I_AllocConsole = 0x005d7520, I_GetStdHandle = 0x005d751c, I_SetConsoleMode = 0x005d7518, I_SetConsoleTitleA = 0x005d7514,
    I_WriteConsoleA = 0x005d7524, I_FreeConsole = 0x005d7528, I_ReadConsoleA = 0x005d752c, I_Sleep = 0x005d74b8,
};
typedef int(__stdcall* AllocConsole_f)(void);
typedef uint32_t(__stdcall* GetStdHandle_f)(uint32_t);
typedef int(__stdcall* SetConsoleMode_f)(uint32_t, uint32_t);
typedef int(__stdcall* SetConsoleTitleA_f)(const char*);
typedef int(__stdcall* WriteConsoleA_f)(uint32_t, const void*, uint32_t, uint32_t*, void*);
typedef int(__stdcall* ReadConsoleA_f)(uint32_t, void*, uint32_t, uint32_t*, void*);
typedef int(__stdcall* FreeConsole_f)(void);
typedef void(__stdcall* Sleep_f)(uint32_t);

}  // namespace nsv
