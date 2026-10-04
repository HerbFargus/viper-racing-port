// net_race.h -- multiplayer stage N2: the shared race-logic layouts (library `multi`: client.obj, multi.obj; server.obj's
// own layouts are hook/net_server.h's). Read off the v1.0 disassembly (out/types.json has next to nothing here); every
// one is static_assert'ed. hook/net_client.cpp and net_multi.cpp rewrite the code; test/world_net_client.cpp builds them.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "net_core.h"

namespace nr {

// ---- LiveMultiInfo (multi.obj): the live network game, 0x3c bytes (LiveMultiInfo::Create) --------------------------------
struct LiveMultiInfo {
    uint8_t live;                      // +0x00  the lobby task ticks the session (thread) while set
    uint8_t _01[3];
    void* server;                      // +0x04  IRaceServer* (the host's), or 0
    void* client;                      // +0x08  IRaceClient*
    netc::SessionMgr* sm;              // +0x0c
    uint8_t _10;                       // +0x10  (cleared by the constructor and destructor)
    uint8_t _11[0x1f];
    int32_t task;                      // +0x30  the lobby task (TaskCreate "Multiplayer", LiveMultiInfo::thread)
    int32_t owner;                     // +0x34  the task that holds it (Grab / Release / SetOwner)
    int32_t multi;                     // +0x38  its critical section (MultiBegin "LiveMultiInfo")
};
static_assert(offsetof(LiveMultiInfo, sm) == 0xc && offsetof(LiveMultiInfo, task) == 0x30 &&
              offsetof(LiveMultiInfo, multi) == 0x38 && sizeof(LiveMultiInfo) == 0x3c, "LiveMultiInfo");

#pragma pack(push, 1)
// ClientData: one user of the game as the client knows it, 0x15 bytes (UserInfoPacket's body)
struct ClientData {
    int32_t id;                        // +0x00  the user's id: (a serial << 8) | its slot; 0 = empty
    char name[13];                     // +0x04
    int32_t status;                    // +0x11  UserStatus (UserStatusPacket: 0 / 1 ...; 3 = unknown)
};
static_assert(sizeof(ClientData) == 0x15, "ClientData");

// a car of the race as the client knows it, 0xd9 bytes: NetCarInfoPacket's body from its byte 5 (dispatch_NetCarInfoPacket
// copies 0xd9 bytes), then the cars registered for it
struct CarSlot {
    int32_t user;                      // +0x00  its user (0: empty)
    uint8_t info[0xc8];                // +0x04  what GetCarList hands the game (a CarList entry, its first dword
                                       //        overwritten with the car's type): the setup at +0x34 (packet byte 0x3d),
                                       //        the car's argument at +0xc0, its hornball at +0xc4 (packet byte 0xcd)
    void* netcar;                      // +0xcc  NetCar* (Register)
    void* localcar;                    // +0xd0  LocalCar* (Register)
    uint8_t _d4[4];
    uint8_t human;                     // +0xd8  CarIsHuman
};
static_assert(offsetof(CarSlot, netcar) == 0xcc && offsetof(CarSlot, localcar) == 0xd0 && offsetof(CarSlot, human) == 0xd8 &&
              sizeof(CarSlot) == 0xd9, "CarSlot");

// ChatLine (Pool<ChatLine>, 0x4c bytes): the chat scrollback, a ring
struct ChatLine {
    int32_t user;                      // +0x00
    char text[0x32];                   // +0x04
    char name[13];                     // +0x36
    uint8_t whisper;                   // +0x43  (ChatPacket byte 8)
    ChatLine* prev;                    // +0x44
    ChatLine* next;                    // +0x48
};
static_assert(offsetof(ChatLine, whisper) == 0x43 && offsetof(ChatLine, prev) == 0x44 && sizeof(ChatLine) == 0x4c, "ChatLine");
#pragma pack(pop)

// ---- RaceClient (client.obj), 0x8cc bytes (CreateRaceClient) ------------------------------------------------------------------
struct RaceClient {
    const void* vtbl;                  // +0x000  0x4df650 (IRaceClient's 0x4df5c0 before and after)
    int32_t state;                     // +0x004  ClientStatus (PGS_: 0 invalid ... 5 searching, 6 connecting, 8 hello,
                                       //         9 running, 0xa proceeding, 0xb car choice, 0xc car waiting, 0xd car get,
                                       //         0xe race waiting, 0xf, 0x10 synchronizing, 0x11, 0x12, 0x13 go ready,
                                       //         0x14 green flag, 0x15 racing)
    int32_t reason;                    // +0x008  why it went down (ClientDisconnectString)
    uint8_t ok;                        // +0x00c
    uint8_t _0d[3];
    int32_t me;                        // +0x010  this user's id (UserListPacket)
    netc::SessionMgr* sm;              // +0x014
    int32_t chan;                      // +0x018  the session's channel (-1 none)
    int32_t last_send;                 // +0x01c  SendAll's PTimeNow
    int32_t last_ping;                 // +0x020  keep_alive's
    int32_t _24;                       // +0x024  (zeroed by init)
    ClientData users[8];               // +0x028
    int32_t user_ids[8];               // +0x0d0  UserListPacket's
    CarSlot cars[8];                   // +0x0f0
    int32_t car_req[8];                // +0x7b8  CommenceCarGetPacket's: the user each car slot should get
    ChatLine* chat;                    // +0x7d8  the newest line of the ring
    netc::PoolBase pool;               // +0x7dc  Pool<ChatLine> (vtable 0x4df6dc): 0x28 of 0x4c
    uint8_t has_proposal;              // +0x7fc
    uint8_t _7fd[3];
    uint8_t proposal[0x3c];            // +0x800  NetProposal (the track at +0x30)
    uint8_t has_race_info;             // +0x83c
    uint8_t _83d[3];
    uint8_t race_info[0x34];           // +0x840  NetRaceInfo (the track at +0x28)
    int32_t iroc_car;                  // +0x874  GetCar (-1)
    uint8_t iroc_878;                  // +0x878
    uint8_t _879[0xf];
    int32_t next_search;               // +0x888  server_search's next broadcast
    uint8_t search_by_id;              // +0x88c  checkPGS_SEARCHING: match the service id (else the name)
    uint8_t _88d[3];
    const char* search_name;           // +0x890  (or the id)
    int32_t greenflag;                 // +0x894  GreenFlagPacket's PTime
    uint8_t user_changes;              // +0x898  AnyUserChanges
    uint8_t _899[3];
    int32_t latency;                   // +0x89c  GetEstRTLatency to the server
    int32_t last_tick;                 // +0x8a0  Tick's PTimeNow (the lag report)
    int32_t last_race_pkt;             // +0x8a4  race_packet's (keep_alive's timeout)
    int32_t track_version[8];          // +0x8a8  CreateTrackVersion
    int32_t task;                      // +0x8c8  the task that owns it (SetOwner)
};
static_assert(offsetof(RaceClient, users) == 0x28 && offsetof(RaceClient, user_ids) == 0xd0 && offsetof(RaceClient, cars) == 0xf0 &&
              offsetof(RaceClient, car_req) == 0x7b8 && offsetof(RaceClient, chat) == 0x7d8 && offsetof(RaceClient, pool) == 0x7dc &&
              offsetof(RaceClient, has_proposal) == 0x7fc && offsetof(RaceClient, proposal) == 0x800 &&
              offsetof(RaceClient, has_race_info) == 0x83c && offsetof(RaceClient, race_info) == 0x840 &&
              offsetof(RaceClient, iroc_car) == 0x874 && offsetof(RaceClient, iroc_878) == 0x878 &&
              offsetof(RaceClient, next_search) == 0x888 && offsetof(RaceClient, search_name) == 0x890 &&
              offsetof(RaceClient, greenflag) == 0x894 && offsetof(RaceClient, user_changes) == 0x898 &&
              offsetof(RaceClient, last_tick) == 0x8a0 && offsetof(RaceClient, track_version) == 0x8a8 &&
              offsetof(RaceClient, task) == 0x8c8 && sizeof(RaceClient) == 0x8cc, "RaceClient");

// CarList (the game's: GetCarList fills it): 8 entries of 0xc8 bytes, the count at +0xc80
enum : uint32_t { CARLIST_ENTRY = 0xc8, CARLIST_COUNT = 0xc80 };

// ---- vtables ------------------------------------------------------------------------------------------------------------------
enum : uint32_t { VT_RaceClient = 0x004df650, VT_IRaceClient = 0x004df5c0, VT_Pool_ChatLine = 0x004df6dc };
// IRaceClient's slots (byte offsets)
enum : uint32_t {
    RC_DTOR = 0x00, RC_TICK = 0x04, RC_SET_OWNER = 0x08, RC_DISCONNECT = 0x0c, RC_ANY_USER_CHANGES = 0x10,
    RC_GET_USER_STATUS = 0x14, RC_COUNT_USERS = 0x18, RC_FIRST_USER = 0x1c, RC_NEXT_USER = 0x20, RC_NTH_USER = 0x24,
    RC_MY_USER_ID = 0x28, RC_MAX_NAME_LENGTH = 0x2c, RC_GET_USERNAME = 0x30, RC_GET_MY_USERNAME = 0x34, RC_SPEAK = 0x38,
    RC_WHISPER = 0x3c, RC_GET_CHAT_SCROLLBACK = 0x40, RC_PROCEED_TO_CHOOSE_CAR = 0x44, RC_PROCEED_TO_RACE = 0x48,
    RC_BACK_TO_CHOOSE_CAR = 0x4c, RC_SYNCHRONIZE = 0x50, RC_WORLD_LOADED = 0x54, RC_GET_GREENFLAG_TIME = 0x58,
    RC_I_AM_HOLDING_THINGS_UP = 0x5c, RC_GET_CAR = 0x60, RC_GET_RACE_INFO = 0x64, RC_GET_PROPOSAL = 0x68,
    RC_GET_CAR_LIST = 0x6c, RC_GET_CAR_SETUP = 0x70, RC_REGISTER_NETCAR = 0x74, RC_REGISTER_LOCALCAR = 0x78,
    RC_UNREGISTER = 0x7c, RC_SEND_ALL = 0x80, RC_CAR_IS_HUMAN = 0x84, RC_DEITY_CAST = 0x88,
};
// IRaceServer's slots (RaceServer's vtable 0x4df720), the ones multi.obj calls
enum : uint32_t {
    RS_DTOR = 0x00, RS_SET_OWNER = 0x04, RS_TICK = 0x08, RS_SEND_ALL = 0x0c, RS_EVERYONE_SYNCHRONIZED = 0x24,
    RS_RESTART_RACE = 0x28, RS_GO_BACK_TO_CHAT = 0x2c,
};
// Deity's slots (the race's referee, `deity` 0x5218ac)
enum : uint32_t { DT_START_RACE_AT = 0x04, DT_UPDATE = 0x08, DT_CAR_IS_FINISHED = 0x40, DT_NET_CAST = 0x60 };

// ---- game functions outside client.obj / multi.obj (v1.0) --------------------------------------------------------------------
enum : uint32_t {
    F_LogReport = 0x00411150, F_LogPanic = 0x004112b0, F_MemAlloc = 0x004140e0, F_op_delete = 0x00414390,
    F_PTimeNow = 0x00413b40, F_Win32Idle = 0x00412bf0,
    F_TaskCreate = 0x004149e0, F_TaskDestroy = 0x00414ac0, F_TaskSetPriority = 0x00414cb0, F_TaskSuspend = 0x00414d00,
    F_TaskResume = 0x00414d40, F_TaskShouldISuspend = 0x00414d90, F_TaskShouldIDie = 0x00414db0, F_TaskGetID = 0x00414dd0,
    F_TaskSleep = 0x00414de0, F_TaskSuspendMe = 0x00414df0, F_TaskIsSuspended = 0x00414e40, F_TaskGetName = 0x00414ec0,
    F_MultiBegin = 0x004150a0, F_MultiEnter = 0x00415180, F_MultiLeave = 0x004151d0, F_MultiEnd = 0x00415220,
    F_Xlator_ctor = 0x0041af80, F_Xlator_xlate = 0x0041afb0, F_atexit = 0x004ceff0,
    F_PoolBase_ctor = 0x0041ba30, F_PoolBase_dtor = 0x0041bac0, F_PoolBase_OverallocCrashes = 0x0041bb10,
    F_PoolBase_alloc = 0x0041bb20, F_PoolBase_freeall = 0x0041bb80,
    F_strchr = 0x004ce0c0, F_atoi = 0x004cf990, F_strstr = 0x004cf9a0,
    F_GetTrackName = 0x00406810, F_GetGameState = 0x0040a3d0, F_SplashGeneric = 0x0040c9e0, F_HackHornBall = 0x0040d540,
    F_GhostCarIncognito = 0x0040e940, F_AIResetDriverMap = 0x00420c30, F_PhysTaskRestart = 0x00426b20,
    F_PhysicsGetDeviation = 0x00426e90, F_ConvertPTimeToPhysicsTime = 0x00426ea0, F_PhysicsAbortRace = 0x0042bc00,
    F_PhysicsIsPaused = 0x0042bd20, F_NetCar_NewPacket = 0x0043f9c0, F_LocalCar_FillNetPacket = 0x00444580,
    F_gxFlip = 0x0044e180, F_gxGrabScreen = 0x0044e1c0, F_gxReleaseScreen = 0x0044e210, F_gxSetCanvas = 0x0044fe30,
    F_gxClear = 0x0044ffd0, F_CarMgrCount = 0x00464480, F_CarMgrGetInfo = 0x00464490, F_CarFileLoadSetup = 0x00465320,
    F_OptionsGet = 0x00471500,
    // nettypes.obj / session.obj / dataport.obj (N1)
    F_MakePhysicsPacket = 0x004a44d0, F_MakeNetPacket = 0x004a4780, F_SM_Ok = 0x004a4f30, F_SM_dtor = 0x004a4f40,
    F_SM_CanDestroySafely = 0x004a4fc0, F_SM_Tick = 0x004a50d0, F_SM_GetServiceTable = 0x004a55c0,
    F_SM_ServiceRequestBroadcast = 0x004a5620, F_SM_UnboundConnect_id = 0x004a5f70, F_SM_UnboundConnect_rs = 0x004a5fe0,
    F_SM_Disconnect = 0x004a60a0, F_SM_Shutdown = 0x004a6260, F_SM_Send = 0x004a6370, F_SM_SendReliable = 0x004a6420,
    F_SM_Recv = 0x004a64c0, F_SM_GetStatus = 0x004a66e0, F_SM_GetDiscReason = 0x004a75e0,
    F_SessionDiscReasonString = 0x004a76d0, F_RDP_GetEstRTLatency = 0x004af4e0,
    // server.obj (group B), by address
    F_smart_strncpy = 0x004aa9e0, F_CreateTrackVersion = 0x004aaac0,
};

// ---- statics (v1.0) -----------------------------------------------------------------------------------------------------------
enum : uint32_t {
    G_DEITY = 0x005218ac,              // Deity* deity
    G_XLATOR_COOKIE = 0x004eb108,      // Xlator::g_cookie
    // multi.obj
    G_WORLD_LIVE = 0x004fb448,         // byte: MultiWorldIsLive / IsDead
    G_PACKET_DELAY = 0x004fb44c,       // int (0x6e)
    G_MONITOR = 0x004fb450,            // byte
    G_LMI = 0x004fb454,                // LiveMultiInfo*: MultiEnabled
    G_TROUBLE_T = 0x004fb458,          // int multi_trouble_t (MultiTurtle)
    G_SYNC_GUARD = 0x0057d1c8,         // byte: MultiSynchronize's two function-local Xlators (bits 1, 2)
    G_RESTART = 0x0057d1d0,            // byte: MultiRestartRace asked (the server's RestartRace at the next BGRecv)
    G_LMI_THREAD = 0x0057d1f4,         // LiveMultiInfo* the lobby task ticks (its constructor's)
    G_XL_DISCONNECTED = 0x0057d200,    // an Xlator (an $E's): MultiFGTick's / MultiSynchronize's "lost" screen
    G_XL_WAITING = 0x0057d210,         // MultiSynchronize's ("Multi:Waiting")
    G_XL_SYNCHRONIZING = 0x0057d228,   // ("Multi:Synchronizing")
    G_PRE_RACE = 0x0057d234,           // byte: BGRecv ran the lobby's Tick (state <= 4): FGTick aborts the race
    G_ESCAPE = 0x0057d240,             // byte: MultiMenuEscape
    G_TROUBLE = 0x0057d258,            // byte: escaped without a server (the client disconnected)
    G_AUTOEND = 0x0057d25c,            // int: autoend_race's PTime (every car finished: 8 s more)
    // client.obj
    G_CLASS_MASK = 0x0057d488,         // byte: the packet-class mask dispatch_packets ANDs byte 0 with (an $E's)
    G_STATUS_GUARD = 0x0057d4d8,       // byte: GetClientStatusString's five Xlators
    G_DISC_GUARD = 0x0057d4c4,         // byte: ClientDisconnectString's four
    G_HELLO_TAIL = 0x004fd818,         // 31 bytes the hello packet carries after the name
    G_GLOBAL_SECTION = 0x004df5b4,     // char*: "GLOBAL" (OptionsGet's section)
};
// ClientStatus strings and asserts (client.obj's .data)
enum : uint32_t { S_INVALID_USER = 0x004fc8e4 };

}  // namespace nr
