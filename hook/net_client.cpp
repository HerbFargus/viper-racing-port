// net_client.cpp -- multiplayer stage N2, group A: the race client, rewritten faithfully (library `multi`: client.obj).
//
// RaceClient (0x8cc bytes, CreateRaceClient) is every player's side of a network game, the host's too: it finds the server
// (by name: a broadcast ServiceRequest every 4 s, the service table searched -- or straight to a RemoteService or an id),
// connects a session channel (SEARCHING 5 -> CONNECTING 6 -> HELLO 8: the player's name and the track-version CRCs), and
// then follows the server's packets (session data class 1, by type, dispatch_packets): the user list and each user's info
// (asked for one at a time, make_userinfo_req) and status, chat (the scrollback ring of Pool<ChatLine>; "latency N" sets the
// packet delay), the proposal (RUNNING 9 / PROCEEDING 0xa), the race info (CAR_CHOICE 0xb), the car requests and each car's
// info (CAR_GET 0xd: make_car_req asks for one car at a time), the IROC car, new states, the sync samples (SYNCHRONIZING
// 0x10: each one stamped and sent back), the green flag (0x13 -> 0x14 -> 0x15), deity casts. The car packets of a race are
// session data class 2 (race_packet: MakePhysicsPacket into each registered NetCar); SendAll sends this machine's cars at
// most every 66 ms while the physics' deviation is under 0.1 (LocalCar::FillNetPacket, MakeNetPacket, SessionMgr::Send:
// unreliable). keep_alive pings the server every 10 s and drops the game when nothing came for 10 s in a race.
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this file's functions too, so
// the hooked rewrite or the original is what runs), virtual calls through the vtable (its own GetUsername / NextUser /
// Disconnect, the socket's LinkAlive, the deity's), the task-ownership check every public member starts with (TaskGetName,
// TaskGetID, TaskGetName, TaskGetID, ASSERT_MSG -- a bare `ret` in v1.0) and the state asserts (GetClientStatusString,
// ASSERT_MSG) in the original's order. N0's patched sites go through net_wsock.h, as the patched originals do: PTimeNow at
// 0x4a8074 (server_search), 0x4a850c (init), 0x4a85b8 (keep_alive), 0x4a9108 / 0x4a917e (SendAll), 0x4a9402
// (GetGreenflagTime), 0x4a976d (Tick), 0x4a9827 (dispatch_SyncPacket), 0x4a9dfb (race_packet) -> net_PTimeNow. Packets built
// in the frame store exactly what the original stores (net_wsock.cpp's net_send_mask lists the bytes they leave): bytes 0-2
// of every reliable packet (SessionMgr::SendReliable and HostEntry::send fill them), 0-1 of the car packet (Send's), the
// hello's name after its NUL (OptionsGet), the car info's 4, 9-12, 15-25, 58-60, 206-221 and its car name after the NUL, the
// sync reply's 8-51 (echoed from the server's packet), the chat text after smart_strncpy's NUL.
//
// The hornball (the brief's known behaviour): transPGS_CAR_CHOICE_PGS_CAR_WAITING puts HackHornBall() -- THIS machine's
// hack setting -- at byte 0xcd of the car info packet; the server relays every car's info, dispatch_NetCarInfoPacket copies
// it into the car's slot (+0xc8) and GetCarList hands it to the game in each CarList entry (+0xc4). So each car carries its
// own driver's flag here; where it leaks to another player's car is the car loading's shared ball.mod name, not this code.
//
// Footprints: whatever sends (SendReliable / Send / Disconnect / the server search), logs, allocates or frees (the chat pool),
// builds or translates the function-local Xlators the first time, runs a callee through a vtable that may send, or touches the
// physics / AI (NetCar::NewPacket, AIResetDriverMap) is replay_only; the rest list the client (or the caller's buffer) and the
// status strings' Xlators GetClientStatusString may translate.
//
// The fixes (docs/PORTING.md, "Fixes"; `// FIX:` in place, VP_FIX), each changing only what the original would crash on or
// overrun with: a user id from the wire whose low byte (its index) is past 7 is no user -- get_userdata returns 0 (it read
// up to 0x14e3 bytes past the client) and dispatch_UserInfoPacket / dispatch_RemoveUserPacket drop the packet (they wrote 21
// bytes and a dword past users[] / user_ids[]); dispatch_NetCarInfoPacket drops a reply for a car index outside 0..7 (a
// signed byte: it read car_req[-128..127] and could copy 0xd9 bytes to cars[index]); dispatch_ChatPacket leaves the packet
// delay alone for a "latency" with no space after it (atoi(0)) or a text with no NUL in the packet, and drops a line it has
// no room for when the scrollback is empty (it read the ring through 0); race_packet reads no records from a packet under 2
// bytes (~178 million); DeityCast sends nothing over the packet's 0xe0 bytes (it overran its frame); GetPacketTypeString
// gives PT_INVALID's name outside 0x20..0x3e (never called in v1.0). Left as is: NextUser skips the users that are there
// and stops at the first empty slot, so FirstUser / NextUser always give 0 (an inverted test): its one caller,
// ChatControl::keep_user_focused, then always drops the whisper target when the user list changes -- wrong, but harmless,
// and a fix would change what a normal game's chat screen does.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "net_types.h"
#include "net_race.h"
#include "net_wsock.h"

namespace {
namespace net_client {
using nt::Edx;
using nt::vcall;
using nt::tcall;
using nt::ccall;
using nt::crt_copy;
using nt::crt_strlen;
using nt::crt_zero;
using namespace nr;

// this file's own functions, by address
enum : uint32_t {
    A_GetClientStatusString = 0x004a7bf0, A_ctor_rs = 0x004a7e70, A_ctor_name = 0x004a7ed0, A_ctor_id = 0x004a7f50,
    A_Ok = 0x004a8010, A_ASSERT_MSG = 0x004a8060, A_server_search = 0x004a8070, A_connect_rs = 0x004a80a0,
    A_cartypestr = 0x004a8350, A_connect_id = 0x004a83f0, A_rt_init = 0x004a84b0, A_init = 0x004a84d0,
    A_keep_alive = 0x004a85b0, A_get_userdata = 0x004a8670, A_make_userinfo_req = 0x004a8f20, A_make_car_req = 0x004a8f80,
    A_check_status = 0x004a9020, A_dispatch_packets = 0x004a9520, A_d_Sync = 0x004a97f0, A_d_GreenFlag = 0x004a9850,
    A_d_DeityCast = 0x004a98a0, A_d_UserStatus = 0x004a98c0, A_d_NetCarInfo = 0x004a9930, A_d_Chat = 0x004a99e0,
    A_d_UserList = 0x004a9b10, A_d_UserInfo = 0x004a9b50, A_d_RemoveUser = 0x004a9bd0, A_d_IrocCar = 0x004a9c30,
    A_d_Proposal = 0x004a9c50, A_d_CommenceCarGet = 0x004a9cc0, A_d_BeginCarChoice = 0x004a9cf0, A_d_NewState = 0x004a9d60,
    A_race_packet = 0x004a9de0, A_checkPGS_SEARCHING = 0x004a9ea0, A_checkPGS_CONNECTING = 0x004a9f70,
    A_trans_CONNECTING_HELLO = 0x004a9fc0, A_trans_RACE_BEGIN_SYNCHRONIZING = 0x004aa030,
    A_trans_CAR_GET_RACE_WAITING = 0x004aa090, A_trans_GO_WAITING_GO_READY = 0x004aa0c0,
    A_trans_HELLO_RUNNING = 0x004aa0f0, A_trans_CAR_CHOICE_CAR_WAITING = 0x004aa100,
    A_trans_CAR_WAITING_CAR_CHOICE = 0x004aa1b0, A_trans_RUNNING_PROCEEDING = 0x004aa1f0,
    A_trans_INVALID_SEARCHING = 0x004aa220, A_MultiSetPacketDelay = 0x004a2b10,
};

static __forceinline RaceClient* RC(void* p) { return (RaceClient*)p; }
static __forceinline const char* S(uint32_t a) { return (const char*)(uintptr_t)a; }
static __forceinline int32_t ld32(const void* p) { int32_t v; memcpy(&v, p, 4); return v; }
static __forceinline void st32(void* p, int32_t v) { memcpy(p, &v, 4); }
static __forceinline int32_t wrap_sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static __forceinline int32_t wrap_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
// users[] and cars[] by an index the original doesn't check (the low byte of a user id, a car index): plain arithmetic
static __forceinline ClientData* user_at(RaceClient* self, uint32_t i) { return (ClientData*)((uint8_t*)self + 0x28 + i * 0x15); }
static __forceinline CarSlot* car_at(RaceClient* self, int32_t i) { return (CarSlot*)((uint8_t*)self + 0xf0 + i * 0xd9); }

typedef void(__cdecl* Assert_t)(int, const char*, ...);
#define ASSERT_MSG ((Assert_t)(uintptr_t)A_ASSERT_MSG)

// every public member's first act: is this the task that owns the client? (ASSERT_MSG is a bare `ret` in v1.0)
static __declspec(noinline) void task_check(RaceClient* self, uint32_t line, uint32_t fmt, uint32_t file) {
    char* const owner = ccall<char*>(F_TaskGetName, self->task);
    const int id = ccall<int>(F_TaskGetID);
    char* const cur = ccall<char*>(F_TaskGetName, id);
    const int id2 = ccall<int>(F_TaskGetID);
    ASSERT_MSG(id2 == self->task ? 1 : 0, S(fmt), S(file), line, cur, owner);
}
// a state assert: the state's name, then ASSERT_MSG(cond, fmt, line, the condition's text, the name)
static __declspec(noinline) void state_assert(int32_t st, int cond, uint32_t fmt, uint32_t line, uint32_t text) {
    const char* const name = ccall<const char*>(A_GetClientStatusString, st);
    ASSERT_MSG(cond, S(fmt), line, S(text), name);
}
static __forceinline void send_reliable(RaceClient* self, const void* pkt, int len) {
    tcall<void>(F_SM_SendReliable, self->sm, self->chan, pkt, len, 0u, 1u);
}
// an Xlator (+0 key, +4 text, +8 cookie): built the first time (its guard bit, then the constructor and atexit), and its
// text translated again if the language changed
static __forceinline void xl_build(uint32_t guard, uint8_t bit, uint32_t x, uint32_t key, uint32_t dtor) {
    if (NT_G8(guard) & bit) return;
    NT_G8(guard) = (uint8_t)(NT_G8(guard) | bit);
    tcall<void*>(F_Xlator_ctor, (const void*)(uintptr_t)x, S(key));
    ccall<int>(F_atexit, dtor);
}
static __forceinline const char* xl_text(uint32_t x) {
    if (NT_GU32(x + 8) != NT_GU32(G_XLATOR_COOKIE)) tcall<void>(F_Xlator_xlate, (const void*)(uintptr_t)x);
    return S(NT_GU32(x + 4));
}

static const char* const R_IO = "sends (SessionMgr::SendReliable / Send / Disconnect / the server search)";
static const char* const R_LOG = "logs (LogReport / LogPanic)";
static const char* const R_HEAP = "allocates or frees (the chat pool, the client)";
static const char* const R_XL = "builds its function-local Xlators the first time (atexit), translates";
static void fp_client(Footprint& f, void* self) { f.add(self, sizeof(RaceClient), "the client"); }
// GetClientStatusString's Xlators (built already in a game: their texts and cookies may be translated again)
static void fp_status_xl(Footprint& f) { f.add((void*)(uintptr_t)0x0057d458, 0xc4, "the client's Xlators"); }

// =========================================================================================================================
// the strings
// =========================================================================================================================
static const char* __cdecl GetClientStatusString_c(int st) {
    xl_build(G_STATUS_GUARD, 1, 0x0057d510, 0x004fc8f8, 0x004a7e60);
    xl_build(G_STATUS_GUARD, 2, 0x0057d4c8, 0x004fc91c, 0x004a7e50);
    xl_build(G_STATUS_GUARD, 4, 0x0057d500, 0x004fc938, 0x004a7e40);
    xl_build(G_STATUS_GUARD, 8, 0x0057d4b8, 0x004fc950, 0x004a7e30);
    xl_build(G_STATUS_GUARD, 0x10, 0x0057d478, 0x004fc970, 0x004a7e20);
    const char* t[0x16];
    t[0] = S(0x004fc998);                                               // PGS_INVALID
    t[1] = xl_text(0x0057d510);
    t[2] = xl_text(0x0057d4c8);
    t[3] = xl_text(0x0057d500);
    t[4] = S(0x004fc9a4);                                               // PGS_ERROR (not a state!)
    t[5] = xl_text(0x0057d4b8);
    t[6] = xl_text(0x0057d478);
    static const uint32_t k[15] = {0x004fc9c0, 0x004fc9e0, 0x004fc9ec, 0x004fc9f8, 0x004fca08, 0x004fca18, 0x004fca28,
                                   0x004fca34, 0x004fca48, 0x004fca58, 0x004fca6c, 0x004fca84, 0x004fca94, 0x004fcaa4,
                                   0x004fcabc};
    for (int i = 0; i < 15; i++) t[7 + i] = S(k[i]);
    if (st >= 0 && st < 0x16) return t[st];
    return S(0x004fcacc);
}
static void fp_xl(Footprint& f, int) { f.replay_only = R_XL; }
PORT_FN(0x004a7bf0, "GetClientStatusString", GetClientStatusString_c, fp_xl)

static const char* __cdecl cartypestr_c(int t) {
    static const uint32_t k[3] = {0x004fcc68, 0x004fcc74, 0x004fcc7c};  // PLAYER_CAR, AI_CAR, NET_CAR
    if (t >= 0 && (uint32_t)t < 3) return S(k[t]);
    return S(0x004fcc84);
}
static void fp_pure(Footprint& f, int) { f.pure = true; }
PORT_FN(0x004a8350, "cartypestr", cartypestr_c, fp_pure)

static const char* __cdecl ClientDisconnectString_c(int r) {
    xl_build(G_DISC_GUARD, 1, 0x0057d458, 0x004fd5d8, 0x004aa4e0);      // Multi:TrackMismatch
    xl_build(G_DISC_GUARD, 2, 0x0057d4a8, 0x004fd5ec, 0x004aa4d0);      // Multi:VersionMismatch
    xl_build(G_DISC_GUARD, 4, 0x0057d468, 0x004fd604, 0x004aa4c0);      // Multi:KeepaliveTimeout
    xl_build(G_DISC_GUARD, 8, 0x0057d4f0, 0x004fd61c, 0x004aa4b0);      // Multi:ClientByeBye
    const char* const s = ccall<const char*>(F_SessionDiscReasonString, r);
    if (s) return s;
    if (r >= 0x104 || r < 0x100) return s;
    const char* t[4];
    t[0] = xl_text(0x0057d458);
    t[1] = xl_text(0x0057d4a8);
    t[2] = xl_text(0x0057d468);
    t[3] = xl_text(0x0057d4f0);
    return t[r - 0x100];
}
PORT_FN(0x004aa330, "ClientDisconnectString", ClientDisconnectString_c, fp_xl)

static const char* __cdecl GetPacketTypeString_c(int t) {
    static const uint32_t k[31] = {0x004fd630, 0x004fd63c, 0x004fd648, 0x004fd658, 0x004fd668, 0x004fd67c, 0x004fd68c,
                                   0x004fd6a0, 0x004fd6ac, 0x004fd6b8, 0x004fd6c8, 0x004fd6d8, 0x004fd6e8, 0x004fd6f8,
                                   0x004fd70c, 0x004fd71c, 0x004fd730, 0x004fd740, 0x004fd758, 0x004fd76c, 0x004fd77c,
                                   0x004fd78c, 0x004fd7a0, 0x004fd7b4, 0x004fd7c0, 0x004fd7cc, 0x004fd7dc, 0x004fd7ec,
                                   0x004fd7f8, 0x004fd804, 0x004fd80c};
    // FIX: a type outside 0x20..0x3e is PT_INVALID's (the original read its table anywhere; never called in v1.0)
    if (VP_FIX && (uint32_t)(t - 0x20) >= 31u) return S(0x004fd80c);
    return S(k[t - 0x20]);
}
static void fp_packet_type_string(Footprint&, int) {}        // (not pure: an out-of-range type reads anywhere)
PORT_FN(0x004aa4f0, "GetPacketTypeString", GetPacketTypeString_c, fp_packet_type_string)

// =========================================================================================================================
// construction
// =========================================================================================================================
static void ctor_common(RaceClient* self, netc::SessionMgr* sm, uint32_t pool_name) {
    self->vtbl = (const void*)(uintptr_t)VT_IRaceClient;
    tcall<void*>(F_PoolBase_ctor, &self->pool, S(pool_name), 0x28, 0x4cu);
    self->pool.vtable = (void**)(uintptr_t)VT_Pool_ChatLine;
    self->vtbl = (const void*)(uintptr_t)VT_RaceClient;
    self->sm = sm;
}
static RaceClient* __fastcall RC_ctor_rs(RaceClient* self, Edx, netc::SessionMgr* sm, const void* rs) {
    ctor_common(self, sm, 0x004fcad4);
    if (tcall<uint8_t>(A_init, self)) self->ok = tcall<uint8_t>(A_connect_rs, self, rs);
    return self;
}
static void fp_ctor_rs(Footprint& f, RaceClient*, Edx, netc::SessionMgr*, const void*) { f.replay_only = R_HEAP; }
PORT_FN(0x004a7e70, "RaceClient::RaceClient(RemoteService)", RC_ctor_rs, fp_ctor_rs)

static RaceClient* __fastcall RC_ctor_name(RaceClient* self, Edx, netc::SessionMgr* sm, const char* name) {
    ctor_common(self, sm, 0x004fcae4);
    if (!tcall<uint8_t>(A_init, self)) return self;
    tcall<void>(A_trans_INVALID_SEARCHING, self, name);
    self->state = 5;
    netc::SessionMgr* const m = self->sm;
    if (m && tcall<uint8_t>(F_SM_Ok, m)) {
        self->ok = 1;
        return self;
    }
    self->ok = 0;
    return self;
}
static void fp_ctor_name(Footprint& f, RaceClient*, Edx, netc::SessionMgr*, const char*) { f.replay_only = R_HEAP; }
PORT_FN(0x004a7ed0, "RaceClient::RaceClient(name)", RC_ctor_name, fp_ctor_name)

static RaceClient* __fastcall RC_ctor_id(RaceClient* self, Edx, netc::SessionMgr* sm, uint32_t id) {
    ctor_common(self, sm, 0x004fcaf4);
    if (tcall<uint8_t>(A_init, self)) self->ok = tcall<uint8_t>(A_connect_id, self, id);
    return self;
}
static void fp_ctor_id(Footprint& f, RaceClient*, Edx, netc::SessionMgr*, uint32_t) { f.replay_only = R_HEAP; }
PORT_FN(0x004a7f50, "RaceClient::RaceClient(unsigned)", RC_ctor_id, fp_ctor_id)

static void __fastcall RC_dtor(RaceClient* self, Edx) {
    const int32_t ch = self->chan;
    self->vtbl = (const void*)(uintptr_t)VT_RaceClient;
    if (ch != -1 && self->sm) {
        if (tcall<uint8_t>(F_SM_GetStatus, self->sm, ch) & 1) tcall<void>(F_SM_Disconnect, self->sm, self->chan, 0x103);
        self->chan = -1;
    }
    self->sm = 0;
    self->state = 0;
    tcall<void>(F_PoolBase_freeall, &self->pool);
    tcall<void>(F_PoolBase_dtor, &self->pool);
    self->vtbl = (const void*)(uintptr_t)VT_IRaceClient;
}
static void fp_dtor(Footprint& f, RaceClient*, Edx) { f.replay_only = R_HEAP; }
PORT_FN(0x004a7fb0, "RaceClient::~RaceClient", RC_dtor, fp_dtor)

static uint8_t __fastcall RC_Ok(RaceClient* self, Edx) {
    const int32_t st = self->state;
    state_assert(st, st != 0 ? 1 : 0, 0x004fcb1c, 0x1cb, 0x004fcb04);
    netc::SessionMgr* const m = self->sm;
    if (m && tcall<uint8_t>(F_SM_Ok, m) && self->ok) return 1;
    return 0;
}
static void fp_ok(Footprint& f, RaceClient*, Edx) { fp_status_xl(f); }
PORT_FN(0x004a8010, "RaceClient::Ok", RC_Ok, fp_ok)

static void __fastcall RC_server_search(RaceClient* self, Edx) {
    const int32_t now = net_PTimeNow();                                 // site 0x4a8074
    if (self->next_search < now) {
        tcall<void>(F_SM_ServiceRequestBroadcast, self->sm, 0x1001u);
        self->next_search = wrap_add(now, 0xfa1);
    }
}
static void fp_io(Footprint& f, RaceClient*, Edx) { f.replay_only = R_IO; }
PORT_FN(0x004a8070, "RaceClient::server_search", RC_server_search, fp_io)

static uint8_t __fastcall RC_connect_rs(RaceClient* self, Edx, const void* rs) {
    const int32_t ch = tcall<int>(F_SM_UnboundConnect_rs, self->sm, rs);
    self->chan = ch;
    if (ch != -1) {
        self->state = 6;
        return 1;
    }
    self->state = 3;
    return 0;
}
static void fp_connect_rs(Footprint& f, RaceClient*, Edx, const void*) { f.replay_only = R_IO; }
PORT_FN(0x004a80a0, "RaceClient::connect(RemoteService)", RC_connect_rs, fp_connect_rs)

static uint8_t __fastcall RC_connect_id(RaceClient* self, Edx, uint32_t id) {
    const int32_t ch = tcall<int>(F_SM_UnboundConnect_id, self->sm, id);
    self->chan = ch;
    if (ch != -1) {
        self->state = 6;
        return 1;
    }
    self->state = 3;
    return 0;
}
static void fp_connect_id(Footprint& f, RaceClient*, Edx, uint32_t) { f.replay_only = R_IO; }
PORT_FN(0x004a83f0, "RaceClient::connect(unsigned)", RC_connect_id, fp_connect_id)

// =========================================================================================================================
// the getters
// =========================================================================================================================
static uint8_t __fastcall RC_CarIsHuman(RaceClient* self, Edx, int car) {
    const int32_t st = self->state;
    state_assert(st, st >= 0xf ? 1 : 0, 0x004fcb58, 0x1f8, 0x004fcb40);
    return car_at(self, car)->human;
}
static void fp_car_is_human(Footprint& f, RaceClient*, Edx, int) { fp_status_xl(f); }
PORT_FN(0x004a80e0, "RaceClient::CarIsHuman", RC_CarIsHuman, fp_car_is_human)

static int __fastcall RC_GetCar(RaceClient* self, Edx) {
    task_check(self, 0x205, 0x004fcb88, 0x004fcb7c);
    return self->iroc_car;
}
static void fp_read(Footprint&, RaceClient*, Edx) {}
PORT_FN(0x004a8130, "RaceClient::GetCar", RC_GetCar, fp_read)

static void __fastcall RC_GetCarSetup(RaceClient* self, Edx, uint8_t* setup, int car) {
    task_check(self, 0x211, 0x004fcbb8, 0x004fcbac);
    crt_copy(setup, car_at(self, car)->info + 0x34, 0x8c);
    setup[0x94] = 0;
}
static void fp_get_car_setup(Footprint& f, RaceClient*, Edx, uint8_t* setup, int) { f.add(setup, 0x95, "the CarSetup"); }
PORT_FN(0x004a8190, "RaceClient::GetCarSetup", RC_GetCarSetup, fp_get_car_setup)

static void __fastcall RC_GetCarList(RaceClient* self, Edx, uint8_t* list) {
    task_check(self, 0x220, 0x004fcbe8, 0x004fcbdc);
    const int32_t st = self->state;
    state_assert(st, st == 0xf ? 1 : 0, 0x004fcc24, 0x223, 0x004fcc0c);
    volatile int32_t* const count = (volatile int32_t*)(list + CARLIST_COUNT);
    uint8_t* e = list;
    *count = 0;
    for (int i = 0; i < 8; i++) {
        CarSlot* const c = &self->cars[i];
        if (c->user == 0) continue;
        crt_copy(e, c->info, CARLIST_ENTRY);
        int32_t type;
        if (self->me != c->user) type = 2;                              // NET_CAR
        else type = c->human ? 0 : 1;
        st32(e, type);
        e += CARLIST_ENTRY;
        *count = *count + 1;
    }
    for (int32_t i = 0; *count > i; i++) {
        const uint8_t* const en = list + i * CARLIST_ENTRY;
        const char* const ts = ccall<const char*>(A_cartypestr, ld32(en));
        NT_LogReport(S(0x004fcc48), i, ts, en + 0x11, en + 4);
    }
}
static void fp_get_car_list(Footprint& f, RaceClient*, Edx, uint8_t*) { f.replay_only = R_LOG; }
PORT_FN(0x004a8210, "RaceClient::GetCarList", RC_GetCarList, fp_get_car_list)

static uint8_t __fastcall RC_AnyUserChanges(RaceClient* self, Edx) {
    task_check(self, 0x24b, 0x004fcc98, 0x004fcc8c);
    const uint8_t r = self->user_changes;
    self->user_changes = 0;
    return r;
}
static void fp_this(Footprint& f, RaceClient* self, Edx) { fp_client(f, self); }
PORT_FN(0x004a8390, "RaceClient::AnyUserChanges", RC_AnyUserChanges, fp_this)

static void __fastcall RC_Disconnect(RaceClient* self, Edx) {
    task_check(self, 0x26b, 0x004fccc8, 0x004fccbc);
    const int32_t ch = self->chan;
    if (ch == -1) return;
    tcall<void>(F_SM_Disconnect, self->sm, ch, 0x103);
    self->chan = -1;
    self->state = 3;
}
PORT_FN(0x004a8430, "RaceClient::Disconnect", RC_Disconnect, fp_io)

static void __fastcall RC_rt_init(RaceClient* self, Edx) {
    crt_zero(self->cars, 0x1b2);
    self->last_send = 0;
}
PORT_FN(0x004a84b0, "RaceClient::rt_init", RC_rt_init, fp_this)

static uint8_t __fastcall RC_init(RaceClient* self, Edx) {
    tcall<void>(F_PoolBase_OverallocCrashes, &self->pool, 0u);
    self->task = ccall<int>(F_TaskGetID);
    self->state = 0;
    self->reason = 0;
    self->ok = 0;
    self->user_changes = 1;
    self->chan = -1;
    self->next_search = 0;
    self->last_tick = net_PTimeNow();                                   // site 0x4a850c
    self->last_ping = 0;
    self->last_race_pkt = 0;
    self->_24 = 0;
    self->has_proposal = 0;
    self->has_race_info = 0;
    self->chat = 0;
    crt_zero(self->cars, 0x1b2);
    crt_zero(self->car_req, 8);
    crt_zero(self->users, 0x2a);
    crt_zero(self->user_ids, 8);
    crt_zero(&self->iroc_car, 5);                                       // +0x874 .. +0x887
    crt_zero(&self->has_race_info, 0xe);                                // +0x83c .. +0x873
    self->iroc_car = -1;
    self->me = 0;
    tcall<void>(A_rt_init, self);
    ccall<void>(F_CreateTrackVersion, self->track_version);
    return 1;
}
static void fp_init(Footprint& f, RaceClient*, Edx) { f.replay_only = "CreateTrackVersion (the track files' CRCs)"; }
PORT_FN(0x004a84d0, "RaceClient::init", RC_init, fp_init)

static void __fastcall RC_keep_alive(RaceClient* self, Edx) {
    const int32_t now = net_PTimeNow();                                 // site 0x4a85b8
    const uint8_t racing = ccall<int>(F_GetGameState) == 1 ? 1 : 0;
    if (self->state > 0x15) {
        if (!racing) return;
    } else if (!racing) {
        netc::SessionMgr* const m = self->sm;
        const int32_t ch = self->chan;
        self->latency = tcall<int>(F_RDP_GetEstRTLatency, &m->rdp, (const void*)((uint8_t*)m->sessions + ch * 0x1c + 0xc));
        if (wrap_add(self->last_ping, 10000) >= now) return;
        uint8_t pkt[4];
        self->last_ping = now;
        pkt[3] = 0x22;                                                  // KeepAlive
        send_reliable(self, pkt, 4);
        return;
    }
    if (wrap_sub(now, self->last_race_pkt) > 10000) {
        self->state = 3;
        self->reason = 0x102;
        tcall<void>(F_SM_Disconnect, self->sm, self->chan, 0x102);
    }
}
PORT_FN(0x004a85b0, "RaceClient::keep_alive", RC_keep_alive, fp_io)

static ClientData* __fastcall RC_get_userdata(RaceClient* self, Edx, int id) {
    ASSERT_MSG((int)((uint32_t)id & 0xffffff00u), S(S_INVALID_USER));
    // FIX: an id whose index (its low byte, from the wire: chat, whisper) is past the 8 users is nobody's; the original read
    // up to 0x14e3 bytes past the client
    if (VP_FIX && ((uint32_t)id & 0xff) >= 8) return 0;
    ClientData* const p = user_at(self, (uint32_t)id & 0xff);
    return p->id == id ? p : 0;
}
static void fp_get_userdata(Footprint&, RaceClient*, Edx, int) {}
PORT_FN(0x004a8670, "RaceClient::get_userdata", RC_get_userdata, fp_get_userdata)

static const char* __fastcall RC_GetMyUsername(RaceClient* self, Edx) {
    task_check(self, 0x2e5, 0x004fccf8, 0x004fccec);
    return vcall<const char*>(self, RC_GET_USERNAME, self->me);
}
PORT_FN(0x004a86b0, "RaceClient::GetMyUsername", RC_GetMyUsername, fp_read)

static const char* __fastcall RC_GetUsername(RaceClient* self, Edx, int id) {
    task_check(self, 0x2f0, 0x004fcd28, 0x004fcd1c);
    ClientData* const p = tcall<ClientData*>(A_get_userdata, self, id);
    if (p) return p->name;
    return S(0x004fcd4c);
}
static void fp_read_i(Footprint&, RaceClient*, Edx, int) {}
PORT_FN(0x004a8710, "RaceClient::GetUsername", RC_GetUsername, fp_read_i)

static int __fastcall RC_GetUserStatus(RaceClient* self, Edx, int id) {
    task_check(self, 0x2ff, 0x004fcd64, 0x004fcd58);
    ClientData* const p = tcall<ClientData*>(A_get_userdata, self, id);
    if (p) return p->status;
    return 3;
}
PORT_FN(0x004a8780, "RaceClient::GetUserStatus", RC_GetUserStatus, fp_read_i)

static void __fastcall RC_Speak(RaceClient* self, Edx, const char* text) {
    task_check(self, 0x30e, 0x004fcd94, 0x004fcd88);
    uint8_t pkt[0x38];
    pkt[3] = 0x28;                                                      // Speak
    const int n = ccall<int>(F_smart_strncpy, (char*)pkt + 4, text, 0x32);
    send_reliable(self, pkt, n + 4);
}
static void fp_speak(Footprint& f, RaceClient*, Edx, const char*) { f.replay_only = R_IO; }
PORT_FN(0x004a87f0, "RaceClient::Speak", RC_Speak, fp_speak)

static void __fastcall RC_Whisper(RaceClient* self, Edx, const char* text, int to) {
    task_check(self, 0x321, 0x004fcdc4, 0x004fcdb8);
    ClientData* const p = tcall<ClientData*>(A_get_userdata, self, to);
    if (!p) return;
    uint8_t pkt[0x3c];
    pkt[3] = 0x29;                                                      // Whisper
    st32(pkt + 4, p->id);
    const int n = ccall<int>(F_smart_strncpy, (char*)pkt + 8, text, 0x32);
    send_reliable(self, pkt, n + 8);
}
static void fp_whisper(Footprint& f, RaceClient*, Edx, const char*, int) { f.replay_only = R_IO; }
PORT_FN(0x004a8880, "RaceClient::Whisper", RC_Whisper, fp_whisper)

static const ChatLine* __fastcall RC_GetChatScrollback(RaceClient* self, Edx) {
    task_check(self, 0x336, 0x004fcdf4, 0x004fcde8);
    return self->chat;
}
PORT_FN(0x004a8920, "RaceClient::GetChatScrollback", RC_GetChatScrollback, fp_read)

static const void* __fastcall RC_GetProposal(RaceClient* self, Edx) {
    task_check(self, 0x342, 0x004fce24, 0x004fce18);
    if (self->has_proposal) return self->proposal;
    return 0;
}
PORT_FN(0x004a8980, "RaceClient::GetProposal", RC_GetProposal, fp_read)

static void __fastcall RC_ProceedToRace(RaceClient* self, Edx, const char* car, int arg) {
    task_check(self, 0x351, 0x004fce54, 0x004fce48);
    tcall<void>(A_trans_CAR_CHOICE_CAR_WAITING, self, car, arg);
    self->state = 0xc;
}
static void fp_proceed_to_race(Footprint& f, RaceClient*, Edx, const char*, int) { f.replay_only = R_IO; }
PORT_FN(0x004a89f0, "RaceClient::ProceedToRace", RC_ProceedToRace, fp_proceed_to_race)

static void __fastcall RC_BackToChooseCar(RaceClient* self, Edx) {
    task_check(self, 0x35c, 0x004fce84, 0x004fce78);
    tcall<void>(A_trans_CAR_WAITING_CAR_CHOICE, self);
    self->state = 0xb;
}
PORT_FN(0x004a8a60, "RaceClient::BackToChooseCar", RC_BackToChooseCar, fp_io)

static void __fastcall RC_ProceedToChooseCar(RaceClient* self, Edx) {
    task_check(self, 0x367, 0x004fceb4, 0x004fcea8);
    tcall<void>(A_trans_RUNNING_PROCEEDING, self);
    self->state = 0xa;
}
PORT_FN(0x004a8ac0, "RaceClient::ProceedToChooseCar", RC_ProceedToChooseCar, fp_io)

static int __fastcall RC_CountUsers(RaceClient* self, Edx) {
    task_check(self, 0x372, 0x004fcee4, 0x004fced8);
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (self->users[i].id != 0) n++;
    return n;
}
PORT_FN(0x004a8b20, "RaceClient::CountUsers", RC_CountUsers, fp_read)

static int __fastcall RC_FirstUser(RaceClient* self, Edx, int32_t* it) {
    task_check(self, 0x382, 0x004fcf14, 0x004fcf08);
    *(volatile int32_t*)it = 0;
    return vcall<int>(self, RC_NEXT_USER, it);
}
static void fp_iter(Footprint& f, RaceClient*, Edx, int32_t* it) { f.add(it, 4, "the iterator"); }
PORT_FN(0x004a8b90, "RaceClient::FirstUser", RC_FirstUser, fp_iter)

static int __fastcall RC_NextUser(RaceClient* self, Edx, int32_t* it) {
    task_check(self, 0x38e, 0x004fcf44, 0x004fcf38);
    volatile int32_t* const p = it;
    const int32_t first = *p;
    if (first < 8) {
        const uint8_t* u = (const uint8_t*)user_at(self, (uint32_t)first);
        for (;;) {
            if (ld32(u) == 0) break;                                    // (an empty slot ends it: inverted, kept)
            u += 0x15;
            const int32_t v = *p + 1;
            *p = v;
            if (!(v < 8)) break;
        }
    }
    const int32_t c = *p;
    if (c < 8) {
        *p = c + 1;
        return user_at(self, (uint32_t)c)->id;
    }
    return 0;
}
PORT_FN(0x004a8c00, "RaceClient::NextUser", RC_NextUser, fp_iter)

static int __fastcall RC_NthUser(RaceClient* self, Edx, int n) {
    task_check(self, 0x39f, 0x004fcf74, 0x004fcf68);
    for (int i = 0; i < 8; i++) {
        if (self->users[i].id == 0) continue;
        const int k = n;
        n--;
        if (k == 0) return self->users[i].id;
    }
    return 0;
}
PORT_FN(0x004a8ca0, "RaceClient::NthUser", RC_NthUser, fp_read_i)

static int __fastcall RC_MaxNameLength(RaceClient* self, Edx) {
    task_check(self, 0x3b0, 0x004fcfa4, 0x004fcf98);
    int32_t best = 0;
    for (int i = 0; i < 8; i++) {
        if (self->users[i].id == 0) continue;
        const int32_t n = (int32_t)crt_strlen(self->users[i].name);
        if (n > best) best = n;
    }
    return best;
}
PORT_FN(0x004a8d30, "RaceClient::MaxNameLength", RC_MaxNameLength, fp_read)

static uint8_t __fastcall RC_IAmHoldingThingsUp(RaceClient* self, Edx) {
    task_check(self, 0x3c3, 0x004fcfd4, 0x004fcfc8);
    const int32_t st = self->state;
    int32_t want;
    if (st == 0xb) want = 1;
    else if (st == 9) want = 0;
    else return 0;
    uint8_t alone = 1;
    for (int i = 0; i < 8; i++) {
        const int32_t id = self->users[i].id;
        if (!id || self->me == id) continue;
        alone = 0;
        if (self->users[i].status == want) return 0;
    }
    return alone == 0 ? 1 : 0;
}
PORT_FN(0x004a8db0, "RaceClient::IAmHoldingThingsUp", RC_IAmHoldingThingsUp, fp_read)

static void __fastcall RC_DeityCast(RaceClient* self, Edx, const void* dp, int len) {
    task_check(self, 0x3ea, 0x004fd004, 0x004fcff8);
    uint8_t pkt[0xe4];
    // FIX: a cast too long for the packet (over 0xe0 bytes, or a negative length) isn't sent; the original copied it past
    // its frame
    if (VP_FIX && (uint32_t)len > 0xe0u) return;
    pkt[3] = 0x3b;                                                      // DeityCast
    crt_copy(pkt + 4, dp, (uint32_t)len);
    send_reliable(self, pkt, len + 4);
}
static void fp_deity_cast(Footprint& f, RaceClient*, Edx, const void*, int) { f.replay_only = R_IO; }
PORT_FN(0x004a8e80, "RaceClient::DeityCast", RC_DeityCast, fp_deity_cast)

// =========================================================================================================================
// requests and status
// =========================================================================================================================
static void __fastcall RC_make_userinfo_req(RaceClient* self, Edx) {
    for (int i = 0; i < 8; i++) {
        const int32_t want = self->user_ids[i];
        if (self->users[i].id == want) continue;
        if (want != 0) {
            uint8_t pkt[8];
            st32(pkt + 4, self->user_ids[i]);
            pkt[3] = 0x24;                                              // UserInfoRequest
            send_reliable(self, pkt, 8);
            return;
        }
        self->users[i].id = 0;
    }
}
PORT_FN(0x004a8f20, "RaceClient::make_userinfo_req", RC_make_userinfo_req, fp_io)

static void __fastcall RC_make_car_req(RaceClient* self, Edx) {
    const int32_t st = self->state;
    state_assert(st, st == 0xd ? 1 : 0, 0x004fd040, 0x412, 0x004fd028);
    for (int i = 0; i < 8; i++) {
        const int32_t req = self->car_req[i];
        if (req == 0 || self->cars[i].user == req) continue;
        uint8_t pkt[8];
        pkt[4] = (uint8_t)i;
        pkt[3] = 0x32;                                                  // CarInfoRequest
        send_reliable(self, pkt, 5);
        return;
    }
    tcall<void>(A_trans_CAR_GET_RACE_WAITING, self);
    self->state = 0xe;
}
PORT_FN(0x004a8f80, "RaceClient::make_car_req", RC_make_car_req, fp_io)

static void __fastcall RC_check_status(RaceClient* self, Edx) {
    if (self->state < 7) return;
    const uint8_t s = tcall<uint8_t>(F_SM_GetStatus, self->sm, self->chan);
    if (!(s & 0x80)) return;
    if (s & 1) {
        NT_LogReport(S(0x004fd084), (uint32_t)s);
        return;
    }
    self->state = 3;
    NT_LogReport(S(0x004fd064), (uint32_t)s);
    if (s & 2) {
        self->reason = 0x102;
        return;
    }
    self->reason = tcall<int>(F_SM_GetDiscReason, self->sm, self->chan);
}
static void fp_check_status(Footprint& f, RaceClient*, Edx) { f.replay_only = R_LOG; }
PORT_FN(0x004a9020, "RaceClient::check_status", RC_check_status, fp_check_status)

typedef double(__cdecl* Deviation_t)();                                 // a float in ST0, taken as the register has it
static void __fastcall RC_SendAll(RaceClient* self, Edx) {
    task_check(self, 0x445, 0x004fd0cc, 0x004fd0c0);
    tcall<void>(A_check_status, self);
    if (self->state < 0x14) return;
    const double dev = ((Deviation_t)(uintptr_t)F_PhysicsGetDeviation)();
    if (dev >= (double)(float)0.1f) return;                                    // fcomp 0.1f; test ah,1: on when less or unordered
    const int32_t now = net_PTimeNow();                                 // site 0x4a9108
    if (wrap_sub(now, self->last_send) <= 0x42) return;
    uint8_t phys[0x42];                                                 // CarPhysicsPacket
    uint8_t pkt[2 + 24 * 8];                                            // [0] / [1]: SessionMgr::Send's
    int32_t count = 0;
    for (int i = 0; i < 8; i++) {
        if (!self->cars[i].localcar) continue;
        uint8_t* const r = pkt + 2 + 24 * count;
        r[0] = (uint8_t)i;
        tcall<void>(F_LocalCar_FillNetPacket, self->cars[i].localcar, phys);
        ccall<void>(F_MakeNetPacket, r + 1, phys);
        count++;
    }
    tcall<void>(F_SM_Send, self->sm, self->chan, pkt, count * 24 + 2, 2u);
    self->last_send = net_PTimeNow();                                   // site 0x4a917e
}
PORT_FN(0x004a9090, "RaceClient::SendAll", RC_SendAll, fp_io)

static void __fastcall RC_Register_net(RaceClient* self, Edx, void* car, int idx) {
    task_check(self, 0x46f, 0x004fd0fc, 0x004fd0f0);
    const int32_t req = self->car_req[idx];
    CarSlot* const c = car_at(self, idx);
    const int32_t u = c->user;
    if (req == u && req != 0) {
        ASSERT_MSG((int)((uint32_t)u & 0xffffff00u), S(S_INVALID_USER));
        const int32_t cu = c->user;
        const int32_t uid = user_at(self, (uint32_t)u & 0xff)->id;
        if (cu == uid) {
            c->netcar = car;
            return;
        }
        NT_LogReport(S(0x004fd120), cu, uid);
        return;
    }
    NT_LogReport(S(0x004fd150), idx, u, req);
}
static void fp_register(Footprint& f, RaceClient*, Edx, void*, int) { f.replay_only = R_LOG; }
PORT_FN(0x004a91a0, "RaceClient::Register(NetCar)", RC_Register_net, fp_register)

static void __fastcall RC_Register_local(RaceClient* self, Edx, void* car, int idx) {
    task_check(self, 0x48b, 0x004fd198, 0x004fd18c);
    const int32_t* const info = ccall<const int32_t*>(F_CarMgrGetInfo, idx);
    if (*info == 3) return;
    const int32_t req = self->car_req[idx];
    CarSlot* const c = car_at(self, idx);
    const int32_t u = c->user;
    if (u == req && req != 0) {
        ASSERT_MSG((int)((uint32_t)u & 0xffffff00u), S(S_INVALID_USER));
        const int32_t cu = c->user;
        const int32_t uid = user_at(self, (uint32_t)u & 0xff)->id;
        if (cu == uid) {
            c->localcar = car;
            return;
        }
        NT_LogReport(S(0x004fd1bc), cu, uid);
        return;
    }
    NT_LogReport(S(0x004fd1ec), idx, u, req);
}
PORT_FN(0x004a9280, "RaceClient::Register(LocalCar)", RC_Register_local, fp_register)

static void __fastcall RC_GetGreenflagTime(RaceClient* self, Edx, int32_t* out) {
    task_check(self, 0x4aa, 0x004fd234, 0x004fd228);
    const int32_t st = self->state;
    state_assert(st, st == 0x14 ? 1 : 0, 0x004fd278, 0x4ac, 0x004fd258);
    const int32_t gf = self->greenflag;
    const int32_t now = net_PTimeNow();                                 // site 0x4a9402
    *(volatile int32_t*)out = wrap_sub(gf, now);
    self->state = 0x15;
}
static void fp_greenflag_time(Footprint& f, RaceClient* self, Edx, int32_t* out) {
    fp_client(f, self);
    fp_status_xl(f);
    f.add(out, 4, "the green flag's time");
}
PORT_FN(0x004a9380, "RaceClient::GetGreenflagTime", RC_GetGreenflagTime, fp_greenflag_time)

static void __fastcall RC_WorldLoaded(RaceClient* self, Edx) {
    task_check(self, 0x4b8, 0x004fd2a8, 0x004fd29c);
    tcall<void>(A_trans_GO_WAITING_GO_READY, self);
    self->state = 0x13;
}
PORT_FN(0x004a9420, "RaceClient::WorldLoaded", RC_WorldLoaded, fp_io)

static void __fastcall RC_Synchronize(RaceClient* self, Edx) {
    task_check(self, 0x4c3, 0x004fd2d8, 0x004fd2cc);
    if (self->state <= 4) return;
    tcall<void>(A_trans_RACE_BEGIN_SYNCHRONIZING, self);
    self->state = 0x10;
}
PORT_FN(0x004a9480, "RaceClient::Synchronize", RC_Synchronize, fp_io)

static void __fastcall RC_UnRegister(RaceClient* self, Edx, void* car) {
    for (int i = 0; i < 8; i++) {
        CarSlot* const c = &self->cars[i];
        if (c->localcar == car || c->netcar == car) {
            c->localcar = 0;
            c->netcar = 0;
        }
    }
}
static void fp_unregister(Footprint& f, RaceClient* self, Edx, void*) { fp_client(f, self); }
PORT_FN(0x004a94f0, "RaceClient::UnRegister", RC_UnRegister, fp_unregister)

// =========================================================================================================================
// the packets
// =========================================================================================================================
static void __fastcall RC_dispatch_packets(RaceClient* self, Edx) {
    uint8_t buf[0xe4];                                                  // RootPacket
    int32_t len = 0xe2;
    if (!tcall<uint8_t>(F_SM_Recv, self->sm, self->chan, buf, &len)) return;
    do {
        const uint32_t cls = (uint8_t)(NT_G8(G_CLASS_MASK) & buf[0]);
        if (cls == 1) {
            uint32_t fn = 0;
            switch (buf[3]) {
            case 0x23: fn = A_d_UserList; break;
            case 0x25: fn = A_d_UserInfo; break;
            case 0x26: fn = A_d_RemoveUser; break;
            case 0x27: fn = A_d_Chat; break;
            case 0x2a: fn = A_d_Proposal; break;
            case 0x2c: fn = A_d_BeginCarChoice; break;
            case 0x2d: fn = A_d_UserStatus; break;
            case 0x2e: fn = A_d_NewState; break;
            case 0x2f: fn = A_d_IrocCar; break;
            case 0x31: fn = A_d_CommenceCarGet; break;
            case 0x33: fn = A_d_NetCarInfo; break;
            case 0x37: fn = A_d_Sync; break;
            case 0x3a: fn = A_d_GreenFlag; break;
            case 0x3c: fn = A_d_DeityCast; break;
            default: break;
            }
            if (fn) tcall<void>(fn, self, buf);
            else NT_LogPanic(S(0x004fd2fc), (uint32_t)buf[0]);           // (byte 0, not the type)
        } else if (cls == 2) {
            tcall<void>(A_race_packet, self, buf, len);
        }
        len = 0xe2;
    } while (tcall<uint8_t>(F_SM_Recv, self->sm, self->chan, buf, &len));
}
PORT_FN(0x004a9520, "RaceClient::dispatch_packets", RC_dispatch_packets, fp_io)

static void __fastcall RC_Tick(RaceClient* self, Edx) {
    task_check(self, 0x515, 0x004fd330, 0x004fd324);
    const int32_t now = net_PTimeNow();                                 // site 0x4a976d
    const int32_t last = self->last_tick;
    if (last && wrap_sub(now, last) > 500) NT_LogReport(S(0x004fd354), wrap_sub(now, last), now);
    const int32_t st = self->state;
    self->last_tick = now;
    if (st >= 7) {
        tcall<void>(A_check_status, self);
        if (self->state < 7) return;
        tcall<void>(A_keep_alive, self);
        tcall<void>(A_dispatch_packets, self);
        return;
    }
    if (st == 5) tcall<void>(A_checkPGS_SEARCHING, self);
    else if (st == 6) tcall<void>(A_checkPGS_CONNECTING, self);
}
PORT_FN(0x004a9720, "RaceClient::Tick", RC_Tick, fp_io)

static void __fastcall RC_d_Sync(RaceClient* self, Edx, uint8_t* pkt) {
    const int32_t st = self->state;
    state_assert(st, st == 0x10 ? 1 : 0, 0x004fd388, 0x540, 0x004fd36c);
    const int32_t now = net_PTimeNow();                                 // site 0x4a9827
    st32(pkt + 4, wrap_sub(now, ld32(pkt + 4)));
    pkt[3] = 0x38;                                                      // the reply
    send_reliable(self, pkt, 0x34);
}
static void fp_d_io(Footprint& f, RaceClient*, Edx, uint8_t*) { f.replay_only = R_IO; }
PORT_FN(0x004a97f0, "RaceClient::dispatch_SyncPacket", RC_d_Sync, fp_d_io)

static void __fastcall RC_d_GreenFlag(RaceClient* self, Edx, const uint8_t* pkt) {
    const int32_t st = self->state;
    state_assert(st, st >= 0x13 ? 1 : 0, 0x004fd3c4, 0x550, 0x004fd3ac);
    self->state = 0x14;
    self->greenflag = ld32(pkt + 4);
}
static void fp_d_this_xl(Footprint& f, RaceClient* self, Edx, const uint8_t*) { fp_client(f, self); fp_status_xl(f); }
PORT_FN(0x004a9850, "RaceClient::dispatch_GreenFlagPacket", RC_d_GreenFlag, fp_d_this_xl)

static void __fastcall RC_d_DeityCast(RaceClient*, Edx, const uint8_t* pkt) {
    vcall<void>(*(void* volatile*)(uintptr_t)G_DEITY, DT_NET_CAST, pkt + 4);
}
static void fp_d_deity(Footprint& f, RaceClient*, Edx, const uint8_t*) { f.replay_only = "the deity's NetCast (the race's referee)"; }
PORT_FN(0x004a98a0, "RaceClient::dispatch_DeityCastPacket", RC_d_DeityCast, fp_d_deity)

static void __fastcall RC_d_UserStatus(RaceClient* self, Edx, const uint8_t* pkt) {
    self->user_changes = 1;
    for (int i = 0; i < 8; i++) self->users[i].status = pkt[4 + i];
    const int32_t me = self->me;
    ASSERT_MSG((int)((uint32_t)me & 0xffffff00u), S(S_INVALID_USER));
    if (user_at(self, (uint32_t)me & 0xff)->status == 1 && self->state == 9) self->state = 0xa;
}
static void fp_d_this(Footprint& f, RaceClient* self, Edx, const uint8_t*) { fp_client(f, self); }
PORT_FN(0x004a98c0, "RaceClient::dispatch_UserStatusPacket", RC_d_UserStatus, fp_d_this)

static void __fastcall RC_d_NetCarInfo(RaceClient* self, Edx, const uint8_t* pkt) {
    const int32_t i = (int8_t)pkt[4];
    // FIX: a reply for a car index outside 0..7 (a signed byte from the wire: no car this client asked for) is dropped; the
    // original read car_req[-128..127] and, when that matched, copied the car over cars[index]
    if (VP_FIX && (uint32_t)i >= 8u) return;
    const uint8_t* const src = pkt + 5;
    const int32_t req = ld32((const uint8_t*)self + 0x7b8 + i * 4);
    const int32_t u = ld32(src);
    if (req != u) {
        NT_LogReport(S(0x004fd444), req, u);
    } else if (req == 0) {
        NT_LogReport(S(0x004fd410), i, req);
    } else {
        CarSlot* const c = car_at(self, i);
        const int32_t cu = c->user;
        if (cu == 0) {
            crt_copy(c, src, 0xd9);
            c->localcar = 0;
            c->netcar = 0;
            tcall<void>(A_make_car_req, self);
            return;
        }
        NT_LogReport(S(0x004fd3e8), cu);
    }
    vcall<void>(self, RC_DISCONNECT);
}
static void fp_d_io_c(Footprint& f, RaceClient*, Edx, const uint8_t*) { f.replay_only = R_IO; }
PORT_FN(0x004a9930, "RaceClient::dispatch_NetCarInfoPacket", RC_d_NetCarInfo, fp_d_io_c)

static void __fastcall RC_d_Chat(RaceClient* self, Edx, const uint8_t* pkt) {
    const ClientData* const p = tcall<ClientData*>(A_get_userdata, self, ld32(pkt + 4));
    if (!p) {
        NT_LogReport(S(0x004fd49c));
        return;
    }
    ChatLine* const e = (ChatLine*)tcall<void*>(F_PoolBase_alloc, &self->pool);
    // FIX: no line to put it in (the pool's allocation failed with the scrollback still empty) drops it; the original read
    // the ring through 0
    if (VP_FIX && !e && !self->chat) return;
    if (e) {
        if (self->chat) {
            e->next = self->chat->next;
            e->prev = self->chat;
            e->next->prev = e;
            ChatLine* const back = e->next->prev;
            self->chat->next = back;
        } else {
            self->chat = e;
            e->next = e;
            ChatLine* const h = self->chat;
            h->prev = h->next;
        }
    }
    const char* const text = (const char*)pkt + 9;
    ChatLine* const line = self->chat->next;
    self->chat = line;
    ccall<int>(F_smart_strncpy, line->text, text, 0x32);
    ccall<int>(F_smart_strncpy, self->chat->name, p->name, 0xd);
    const uint8_t w = pkt[8];
    self->chat->whisper = w;
    const int32_t from = ld32(pkt + 4);
    self->chat->user = from;
    // FIX: (with the one below) a text with no NUL inside the packet (0xe4 bytes, the text from +9) isn't searched: the
    // original's strstr / strchr ran on past it
    if (VP_FIX && !memchr(text, 0, 0xe4 - 9)) return;
    if (!ccall<const char*>(F_strstr, text, S(0x004fd47c))) return;     // "latency"
    const char* const sp = ccall<const char*>(F_strchr, text, 0x20);
    // FIX: "latency" with no space after it leaves the delay alone; the original's atoi read through strchr's 0
    if (VP_FIX && !sp) return;
    const int n = ccall<int>(F_atoi, sp);
    if (n >= 1000 || n < 0) return;
    ccall<void>(A_MultiSetPacketDelay, n);
    NT_LogReport(S(0x004fd484), n);
}
static void fp_d_chat(Footprint& f, RaceClient*, Edx, const uint8_t*) { f.replay_only = R_HEAP; }
PORT_FN(0x004a99e0, "RaceClient::dispatch_ChatPacket", RC_d_Chat, fp_d_chat)

static void __fastcall RC_d_UserList(RaceClient* self, Edx, const uint8_t* pkt) {
    tcall<void>(A_trans_HELLO_RUNNING, self);
    self->state = 9;
    self->me = ld32(pkt + 4);
    crt_copy(self->user_ids, pkt + 8, 0x20);
    tcall<void>(A_make_userinfo_req, self);
}
PORT_FN(0x004a9b10, "RaceClient::dispatch_UserListPacket", RC_d_UserList, fp_d_io_c)

static void __fastcall RC_d_UserInfo(RaceClient* self, Edx, const uint8_t* pkt) {
    self->user_changes = 1;
    const int32_t id = ld32(pkt + 4);
    ASSERT_MSG((int)((uint32_t)id & 0xffffff00u), S(S_INVALID_USER));
    const uint32_t k = (uint32_t)id & 0xff;
    // FIX: a user index past 7 (the id's low byte, from the wire) is dropped; the original wrote the user's 21 bytes past
    // users[] (into the cars) and its id past user_ids[]
    if (VP_FIX && k >= 8) return;
    ClientData* const d = user_at(self, k);
    crt_zero(d, 5);
    ((volatile uint8_t*)d)[0x14] = 0;
    crt_copy(d, pkt + 4, 0x15);
    ((volatile int32_t*)((uint8_t*)self + 0xd0))[k] = ld32(pkt + 4);
    tcall<void>(A_make_userinfo_req, self);
}
PORT_FN(0x004a9b50, "RaceClient::dispatch_UserInfoPacket", RC_d_UserInfo, fp_d_io_c)

static void __fastcall RC_d_RemoveUser(RaceClient* self, Edx, const uint8_t* pkt) {
    self->user_changes = 1;
    const int32_t id = ld32(pkt + 4);
    ASSERT_MSG((int)((uint32_t)id & 0xffffff00u), S(S_INVALID_USER));
    const uint32_t k = (uint32_t)id & 0xff;
    // FIX: a user index past 7 is dropped; the original zeroed 21 bytes past users[] and a dword past user_ids[]
    if (VP_FIX && k >= 8) return;
    ((volatile int32_t*)((uint8_t*)self + 0xd0))[k] = 0;
    ClientData* const d = user_at(self, k);
    crt_zero(d, 5);
    ((volatile uint8_t*)d)[0x14] = 0;
}
static void fp_d_remove_user(Footprint& f, RaceClient* self, Edx, const uint8_t* pkt) {
    const uint32_t k = (uint32_t)ld32(pkt + 4) & 0xff;
    fp_client(f, self);
    f.add((uint8_t*)self + 0x28 + k * 0x15, 0x15, "the user (by the wire's index)");
}
PORT_FN(0x004a9bd0, "RaceClient::dispatch_RemoveUserPacket", RC_d_RemoveUser, fp_d_remove_user)

static void __fastcall RC_d_IrocCar(RaceClient* self, Edx, const uint8_t* pkt) {
    self->iroc_878 = 0;
    self->iroc_car = ld32(pkt + 4);
}
PORT_FN(0x004a9c30, "RaceClient::dispatch_IrocCarPacket", RC_d_IrocCar, fp_d_this)

static void __fastcall RC_d_Proposal(RaceClient* self, Edx, const uint8_t* pkt) {
    const int32_t st = self->state;
    state_assert(st, st == 9 || st == 0xa ? 1 : 0, 0x004fd4f0, 0x62f, 0x004fd4c0);
    self->state = 9;
    self->has_proposal = 1;
    crt_copy(self->proposal, pkt + 4, 0x3c);
    ccall<void>(F_AIResetDriverMap);
}
static void fp_d_proposal(Footprint& f, RaceClient*, Edx, const uint8_t*) { f.replay_only = "AIResetDriverMap (the AI's tables)"; }
PORT_FN(0x004a9c50, "RaceClient::dispatch_ProposalPacket", RC_d_Proposal, fp_d_proposal)

static void __fastcall RC_d_CommenceCarGet(RaceClient* self, Edx, const uint8_t* pkt) {
    self->state = 0xd;
    crt_copy(self->car_req, pkt + 4, 0x20);
    tcall<void>(A_make_car_req, self);
}
PORT_FN(0x004a9cc0, "RaceClient::dispatch_CommenceCarGetPacket", RC_d_CommenceCarGet, fp_d_io_c)

static void __fastcall RC_d_BeginCarChoice(RaceClient* self, Edx, const uint8_t* pkt) {
    const int32_t st = self->state;
    state_assert(st, st == 0xa || st == 9 ? 1 : 0, 0x004fd544, 0x64e, 0x004fd514);
    self->state = 0xb;
    self->has_race_info = 1;
    crt_copy(self->race_info, pkt + 4, 0x34);
}
PORT_FN(0x004a9cf0, "RaceClient::dispatch_BeginCarChoicePacket", RC_d_BeginCarChoice, fp_d_this_xl)

static void __fastcall RC_d_NewState(RaceClient* self, Edx, const uint8_t* pkt) {
    switch (ld32(pkt + 4)) {
    case 9:
        self->state = 9;
        tcall<void>(A_rt_init, self);
        return;
    case 0xf: self->state = 0xf; return;
    case 0x11: self->state = 0x11; return;
    case 0x12: self->state = 0x12; return;
    default: NT_LogPanic(S(0x004fd568)); return;
    }
}
static void fp_d_new_state(Footprint& f, RaceClient*, Edx, const uint8_t*) { f.replay_only = R_LOG; }
PORT_FN(0x004a9d60, "RaceClient::dispatch_NewStatePacket", RC_d_NewState, fp_d_new_state)

static void __fastcall RC_race_packet(RaceClient* self, Edx, const uint8_t* pkt, int len) {
    uint32_t n = (uint32_t)(len - 2) / 24u;
    // FIX: a packet under 2 bytes has no records (the unsigned division made ~178 million, read past the packet)
    if (VP_FIX && len < 2) n = 0;
    self->last_race_pkt = net_PTimeNow();                               // site 0x4a9dfb
    if (ccall<uint8_t>(F_PhysicsIsPaused)) return;
    if ((int32_t)n <= 0) return;
    uint8_t phys[0x40];                                                 // CarPhysicsPacket
    const uint8_t* p = pkt + 2;
    for (uint32_t k = n; k; k--, p += 24) {
        const int32_t i = (int8_t)p[0];
        if (i < 0 || i >= 8) {
            NT_LogReport(S(0x004fd580), i);
            continue;
        }
        if (!car_at(self, i)->netcar) continue;
        ccall<void>(F_MakePhysicsPacket, phys, p + 1);
        tcall<void>(F_NetCar_NewPacket, car_at(self, i)->netcar, phys);
    }
}
static void fp_race_packet(Footprint& f, RaceClient*, Edx, const uint8_t*, int) { f.replay_only = "NetCar::NewPacket (the cars)"; }
PORT_FN(0x004a9de0, "RaceClient::race_packet", RC_race_packet, fp_race_packet)

// =========================================================================================================================
// the state machine
// =========================================================================================================================
static void __fastcall RC_checkPGS_SEARCHING(RaceClient* self, Edx) {
    void* const sock = self->sm->sock;
    if (!vcall<uint8_t>(sock, 0x28)) {                                  // ISocket::LinkAlive
        self->state = 3;
        return;
    }
    tcall<void>(A_server_search, self);
    const int32_t n = self->sm->nremote;
    const uint8_t* const tab = tcall<const uint8_t*>(F_SM_GetServiceTable, self->sm);
    for (int32_t i = 0; i < n; i++) {
        const uint8_t* const e = tab + i * 0x54;
        bool hit;
        if (self->search_by_id) {
            hit = (uint32_t)(uintptr_t)self->search_name == (uint32_t)ld32(e + 0x28);
        } else {
            const uint8_t* a = (const uint8_t*)self->search_name;       // the compiler's inline strcmp: == 0
            const uint8_t* b = e;
            hit = true;
            for (;;) {
                if (*a != *b) { hit = false; break; }
                if (!*a) break;
                a++;
                b++;
            }
        }
        if (hit) {
            tcall<uint8_t>(A_connect_rs, self, e);
            return;
        }
    }
}
PORT_FN(0x004a9ea0, "RaceClient::checkPGS_SEARCHING", RC_checkPGS_SEARCHING, fp_io)

static void __fastcall RC_checkPGS_CONNECTING(RaceClient* self, Edx) {
    const uint8_t s = tcall<uint8_t>(F_SM_GetStatus, self->sm, self->chan);
    if (!(s & 0x80)) return;
    if (s & 1) {
        tcall<void>(A_trans_CONNECTING_HELLO, self);
        self->state = 8;
        return;
    }
    if (s & 4) {
        const int32_t ch = self->chan;
        netc::SessionMgr* const m = self->sm;
        self->state = 1;
        self->reason = tcall<int>(F_SM_GetDiscReason, m, ch);
        return;
    }
    self->state = 2;
}
PORT_FN(0x004a9f70, "RaceClient::checkPGS_CONNECTING", RC_checkPGS_CONNECTING, fp_io)

static void __fastcall RC_trans_CONNECTING_HELLO(RaceClient* self, Edx) {
    uint8_t pkt[0x50];
    const char* const section = *(const char* volatile*)(uintptr_t)G_GLOBAL_SECTION;
    pkt[3] = 0x21;                                                      // Hello
    ccall<void>(F_OptionsGet, section, S(0x004fd5a8), (char*)pkt + 4, 0xd);   // "player_name"
    crt_copy(pkt + 0x11, (const void*)(uintptr_t)G_HELLO_TAIL, 0x1f);
    crt_copy(pkt + 0x30, self->track_version, 0x20);
    send_reliable(self, pkt, 0x50);
}
PORT_FN(0x004a9fc0, "RaceClient::transPGS_CONNECTING_PGS_HELLO", RC_trans_CONNECTING_HELLO, fp_io)

static void __fastcall RC_trans_RACE_BEGIN_SYNCHRONIZING(RaceClient* self, Edx) {
    uint8_t pkt[0x34];
    pkt[3] = 0x38;
    st32(pkt + 4, 0x3039);
    send_reliable(self, pkt, 0x34);
}
PORT_FN(0x004aa030, "RaceClient::transPGS_RACE_BEGIN_PGS_SYNCHRONIZING", RC_trans_RACE_BEGIN_SYNCHRONIZING, fp_io)

static void __fastcall RC_trans_GREENFLAG_CAR_CHOICE(RaceClient* self, Edx, const void* info) {
    self->has_race_info = 1;
    crt_copy(self->race_info, info, 0x34);
    tcall<void>(A_rt_init, self);
}
static void fp_trans_greenflag(Footprint& f, RaceClient* self, Edx, const void*) { fp_client(f, self); }
PORT_FN(0x004aa060, "RaceClient::transPGS_GREENFLAG_PGS_CAR_CHOICE", RC_trans_GREENFLAG_CAR_CHOICE, fp_trans_greenflag)

static void __fastcall RC_trans_CAR_GET_RACE_WAITING(RaceClient* self, Edx) {
    uint8_t pkt[4];
    pkt[3] = 0x34;
    send_reliable(self, pkt, 4);
}
PORT_FN(0x004aa090, "RaceClient::transPGS_CAR_GET_PGS_RACE_WAITING", RC_trans_CAR_GET_RACE_WAITING, fp_io)

static void __fastcall RC_trans_GO_WAITING_GO_READY(RaceClient* self, Edx) {
    uint8_t pkt[4];
    pkt[3] = 0x39;
    send_reliable(self, pkt, 4);
}
PORT_FN(0x004aa0c0, "RaceClient::transPGS_GO_WAITING_PGS_GO_READY", RC_trans_GO_WAITING_GO_READY, fp_io)

static void __fastcall RC_trans_CAR_CHOICE_CAR_WAITING(RaceClient* self, Edx, const char* car, int arg) {
    uint8_t pkt[0xe0];                                                  // NetCarInfoPacket (0xde sent)
    uint8_t setup[0xd4];                                                // CarSetup
    pkt[3] = 0x30;
    const char* const track = ccall<const char*>(F_GetTrackName, ld32(self->race_info + 0x28));
    ccall<uint8_t>(F_CarFileLoadSetup, setup, car, track);
    crt_copy(pkt + 0x3d, setup, 0x8c);
    ccall<int>(F_smart_strncpy, (char*)pkt + 0x1a, car, 0x20);
    pkt[0xd] = 0x21;
    pkt[0xe] = 0;
    const uint8_t horn = ccall<uint8_t>(F_HackHornBall);               // this machine's hornball, for this car
    pkt[0xcd] = horn;
    st32(pkt + 0xc9, arg);
    st32(pkt + 5, self->me);
    send_reliable(self, pkt, 0xde);
}
static void fp_trans_car_choice(Footprint& f, RaceClient*, Edx, const char*, int) { f.replay_only = R_IO; }
PORT_FN(0x004aa100, "RaceClient::transPGS_CAR_CHOICE_PGS_CAR_WAITING", RC_trans_CAR_CHOICE_CAR_WAITING, fp_trans_car_choice)

static void __fastcall RC_trans_CAR_WAITING_CAR_CHOICE(RaceClient* self, Edx) {
    uint8_t pkt[0xe0];
    pkt[3] = 0x30;
    st32(pkt + 5, 0);
    send_reliable(self, pkt, 0xde);
}
PORT_FN(0x004aa1b0, "RaceClient::transPGS_CAR_WAITING_PGS_CAR_CHOICE", RC_trans_CAR_WAITING_CAR_CHOICE, fp_io)

static void __fastcall RC_trans_RUNNING_PROCEEDING(RaceClient* self, Edx) {
    uint8_t pkt[8];
    st32(pkt + 4, ld32(self->proposal));
    pkt[3] = 0x2b;
    send_reliable(self, pkt, 8);
}
PORT_FN(0x004aa1f0, "RaceClient::transPGS_RUNNING_PGS_PROCEEDING", RC_trans_RUNNING_PROCEEDING, fp_io)

static void __fastcall RC_trans_INVALID_SEARCHING(RaceClient* self, Edx, const char* name) {
    self->search_by_id = 0;
    self->search_name = name;
}
static void fp_trans_invalid(Footprint& f, RaceClient* self, Edx, const char*) { fp_client(f, self); }
PORT_FN(0x004aa220, "RaceClient::transPGS_INVALID_PGS_SEARCHING", RC_trans_INVALID_SEARCHING, fp_trans_invalid)

// =========================================================================================================================
// creation and the one-liners
// =========================================================================================================================
static void* create(uint32_t ctor, netc::SessionMgr* sm, uint32_t arg) {
    RaceClient* r = 0;
    void* const p = ccall<void*>(F_MemAlloc, 0x8cc);
    if (p) r = tcall<RaceClient*>(ctor, p, sm, arg);
    if (r && !tcall<uint8_t>(A_Ok, r)) {
        vcall<void*>(r, RC_DTOR, 1u);
        r = 0;
    }
    return r;
}
static void* __cdecl CreateRaceClient_rs(netc::SessionMgr* sm, const void* rs) { return create(A_ctor_rs, sm, (uint32_t)(uintptr_t)rs); }
static void fp_create_rs(Footprint& f, netc::SessionMgr*, const void*) { f.replay_only = R_HEAP; }
PORT_FN(0x004aa240, "CreateRaceClient(RemoteService)", CreateRaceClient_rs, fp_create_rs)

static void* __cdecl CreateRaceClient_name(netc::SessionMgr* sm, const char* name) { return create(A_ctor_name, sm, (uint32_t)(uintptr_t)name); }
static void fp_create_name(Footprint& f, netc::SessionMgr*, const char*) { f.replay_only = R_HEAP; }
PORT_FN(0x004aa290, "CreateRaceClient(name)", CreateRaceClient_name, fp_create_name)

static void* __cdecl CreateRaceClient_id(netc::SessionMgr* sm, uint32_t id) { return create(A_ctor_id, sm, id); }
static void fp_create_id(Footprint& f, netc::SessionMgr*, uint32_t) { f.replay_only = R_HEAP; }
PORT_FN(0x004aa2e0, "CreateRaceClient(unsigned)", CreateRaceClient_id, fp_create_id)

static void __fastcall RC_SetOwner(RaceClient* self, Edx, int task) { self->task = task; }
static void fp_set_owner(Footprint& f, RaceClient* self, Edx, int) { fp_client(f, self); }
PORT_FN(0x004aa620, "RaceClient::SetOwner", RC_SetOwner, fp_set_owner)

static int __fastcall RC_MyUserID(RaceClient* self, Edx) { return self->me; }
PORT_FN(0x004aa630, "RaceClient::MyUserID", RC_MyUserID, fp_read)

static const void* __fastcall RC_GetRaceInfo(RaceClient* self, Edx) {
    if (self->has_race_info) return self->race_info;
    return 0;
}
PORT_FN(0x004aa640, "RaceClient::GetRaceInfo", RC_GetRaceInfo, fp_read)

}  // namespace net_client
}  // namespace
