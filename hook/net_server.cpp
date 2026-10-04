// net_server.cpp -- multiplayer stage N2, group B: the race server, rewritten faithfully (library `multi`: server.obj).
//
// RaceServer is the host's half of a network race (the host also runs its own RaceClient; the dedicated server, ded.obj in
// net_ded.cpp, runs only this). Its state machine -- RS_APPROVE 1, CLOSE_ENTRY 2, CHOOSE_CAR 3, GET_CAR 4, SYNC 5,
// PRESTAGE 6, STAGE 7, RACE 8 -- is stepped by Tick (checkRS_*: everyone in the state that moves it on; transRS_*: what each
// move sends) and by the owner's calls (ProceedToChooseCar, BackToChooseTrack, RestartRace, GoBackToChat). Tick then reads
// every packet the service has (SessionMgr::UnboundRecv): game packets by type (0x21 add me, 0x22 keepalive, 0x24 user
// info, 0x28 speak, 0x29 whisper, 0x2b approve, 0x30 car info, 0x32 car info request, 0x34 race ready, 0x38 sync, 0x39 go
// ready, 0x3b deity cast) and the unreliable car packets (race_packet: stored per car, the time word moved to the server's
// clock). SendAll (the physics thread's MultiBGSend) forwards each user the cars it doesn't drive, every calc_send_interval
// ms, the time word moved to that user's clock. dispatch_SyncPacket runs the clock sync: 40 round trips per user, sorted by
// round trip, the middle 32 averaged (the offset dt and the round trip), then status 5.
//
// Written from the v1.0 disassembly: every call in the original's order, by its v1.0 address (this object's own functions
// too, so the hooked rewrite or the original is what runs), virtual calls through the vtable; the packets built in the
// frame store exactly the bytes the original stores (bytes 0-2 are SessionMgr's; net_wsock.cpp's net_send_mask covers what
// is never written). The multiplayer clock as N0 patched it (net_PTimeNow, net_wsock.h) at the sites 0x4aac13 (the
// constructor), 0x4aadeb (check_for_dead_users), 0x4ab3ee (AddMe), 0x4abb0d (Sync), 0x4abd4e (KeepAlive), 0x4abdfb
// (race_packet), 0x4abf0a (reset), 0x4ac597 (PRESTAGE -> STAGE), 0x4ac6a7 (RACE -> PRESTAGE), 0x4ac774 (RACE -> APPROVE),
// 0x4aca9e and 0x4acaaf (Propose), 0x4acb4b (SendAll), 0x4acd14 (Tick). No x87 here; the sync's 64-bit averages call the
// CRT's __alldiv as the original does.
//
// Footprints: the helpers with what they write (pure only where a fuzzer's 4 KB arena holds the object: a RaceServer is
// 0x1198 bytes); whatever sends, logs, allocates or calls the SessionMgr is
// replay_only -- a session replay of a network game is its in-game check (test/world_net_server.cpp checks them all
// offline, over a fake SessionMgr).
//
// FIX CANDIDATEs (left faithful, marked in place): the wire's indices are used unchecked -- race_packet's car index (a
// signed byte: cars[-128..127]), NetCarInfoRequest's car index (likewise, and the session it derives from that car),
// UserInfoReq's and Whisper's user index (& 0xff: users[0..255]); race_packet with a length under 2 loops ~178 million
// times; Tick passes user_disconnected the SESSION number, not the user index (another user cleared when the service's
// sessions don't start at 0; past users[7] when the session is 8 or more); Tick calls the state table unchecked (a state
// of 0 or 9 calls address 0); a sync packet after the 40th sample writes samples[40] over nsamples; calc_send_interval
// divides by (header + 24 x cars) and by its quotient (0 when the link's bps is under that); next_user never returns when
// no user is in (add_ai_cars with AI cars and nobody in); add_ai_cars, dispatch_NetCarInfoPacket and
// transRS_CHOOSE_CAR_RS_GET_CAR don't bound the car count (more than 8 cars: past cars[7] into ncars and the proposal;
// GET_CAR's 0x31 packet past its frame); SendAll's frame holds 7 car records (8 overrun it: a user with no car of its own
// and 8 cars sending); the constructor strcpy's the password unbounded into its LocalService; TrackCRC's path is 16 bytes
// (a long track name runs into its read buffer, then the frame); NetCarInfoRequest uses get_iroc_car's result unchecked
// (0 when the IROC user has no car); AddMe only checks the first of the client's CRCs.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "net_types.h"
#include "net_core.h"
#include "net_server.h"
#include "net_wsock.h"

namespace {
namespace net_server {
using namespace nsv;
using nt::ccall;
using nt::crt_copy;
using nt::crt_strcpy;
using nt::crt_strlen;
using nt::tcall;
using nt::vcall;

typedef void(__cdecl* Log_t)(const char*, ...);
typedef void(__cdecl* Assert_t)(int, const char*, ...);
#define LOG ((Log_t)(uintptr_t)F_LogReport)
#define VERBOSE ((Log_t)(uintptr_t)F_VERBOSE)
#define ASSERT_MSG ((Assert_t)(uintptr_t)S_ASSERT_MSG)
#define CP(a) ((const char*)(uintptr_t)(a))
#define U8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define I32(a) (*(volatile int32_t*)(uintptr_t)(a))
#define WR32(p, v) do { const int32_t w_ = (int32_t)(v); memcpy((p), &w_, 4); } while (0)

static const char* const R_SEND = "sends through the SessionMgr";
static const char* const R_LOG = "logs (LogReport)";
static const char* const R_HEAP = "allocates or frees";
static const char* const R_SM = "calls the SessionMgr";
static const char* const R_FILE = "reads a file";

// ---- calls ----------------------------------------------------------------------------------------------------------------
static __forceinline void sm_send_reliable(void* sm, int sess, const void* p, int len, const void* rpi) {
    tcall<void>(F_SM_SendReliable, sm, sess, p, len, rpi, (uint8_t)1);
}
static __forceinline void sm_disconnect(void* sm, int sess, int why) { tcall<void>(F_SM_Disconnect, sm, sess, why); }
static __forceinline void broadcast(RaceServer* s, const void* p, int len) { tcall<void>(S_broadcast, s, p, len); }
static __forceinline void send_status_tab(RaceServer* s) { tcall<void>(S_send_status_tab, s); }
static __forceinline void user_disconnected(RaceServer* s, int i) { tcall<void>(S_user_disconnected, s, i); }
static __forceinline netc::SessionMgr* SM(const RaceServer* s) { return (netc::SessionMgr*)s->sm; }

// the owner check every public entry and transition starts with: ASSERT_MSG(the caller is the owner task, fmt, file, line,
// the caller's name, the owner's name) -- the names fetched first, as the original does
static __forceinline void owner_check(RaceServer* s, int line, uint32_t file, uint32_t fmt) {
    const char* on = ccall<const char*>(F_TaskGetName, s->owner);
    const int cur = ccall<int>(F_TaskGetID);
    const char* cn = ccall<const char*>(F_TaskGetName, cur);
    const int id = ccall<int>(F_TaskGetID);
    ASSERT_MSG(id - s->owner == 0 ? 1 : 0, CP(fmt), CP(file), line, cn, on);
}

// =========================================================================================================================
// helpers
// =========================================================================================================================
// copies n - 1 bytes whatever the string's length (the loop tests the source POINTER, not the byte), then a NUL; returns n
static int __cdecl smart_strncpy(char* dst, const char* src, int n) {
    volatile char* d = dst;
    const uintptr_t end = (uintptr_t)dst + (uintptr_t)n - 1;
    if (src)
        while ((uintptr_t)d < end) {
            *d++ = *(const volatile char*)src++;
            if (!src) break;
        }
    *d = 0;
    return (int)((uintptr_t)d + 1 - (uintptr_t)dst);
}
static void fp_smart_strncpy(Footprint& f, char* dst, const char*, int n) { f.add(dst, n > 0 ? (uint32_t)n : 1u, "the copy"); }
PORT_FN(0x004aa9e0, "smart_strncpy", smart_strncpy, fp_smart_strncpy)

// the sum of a track's first 0x800 bytes, from -1. FIX CANDIDATE: the path buffer is 16 bytes (the name + ".trk")
static int __cdecl TrackCRC(const char* name) {
    struct { char path[16]; uint8_t data[0x800]; } fr;       // the original's frame: the path runs into the data
    int fh;
    crt_strcpy(fr.path, name);
    {
        char* e = fr.path + crt_strlen(fr.path);
        crt_copy(e, CP(0x004fd884), 5);                      // ".trk" and its NUL
    }
    int sum = -1;
    fh = ccall<int>(F_FileOpen, (const char*)fr.path);
    if (fh) {
        if (ccall<uint8_t>(F_FileReadExact, fh, (void*)fr.data, 0x800))
            for (int i = 0; i < 0x800; i++) sum += ((volatile uint8_t*)fr.data)[i];
        ccall<void>(F_FileClose, &fh);
    }
    return sum;
}
static void fp_track_crc(Footprint& f, const char*) { f.replay_only = R_FILE; }
PORT_FN(0x004aaa10, "TrackCRC", TrackCRC, fp_track_crc)

static void __cdecl CreateTrackVersion(int32_t* v) { nt::crt_zero(v, 8); }
static void fp_ctv(Footprint& f, int32_t* v) { f.pure = true; f.add(v, 32, "the version"); }
PORT_FN(0x004aaac0, "CreateTrackVersion", CreateTrackVersion, fp_ctv)

// clears +0x15 .. +0x111: the sync state
static void __fastcall ServerData_reset(ServerData* self, Edx) {
    volatile uint8_t* p = (volatile uint8_t*)self + 0x15;
    for (int i = 0; i < 0xfd; i++) p[i] = 0;
}
static void fp_sd_reset(Footprint& f, ServerData* self, Edx) { f.pure = true; f.add((uint8_t*)self + 0x15, 0xfd, "the sync state"); }
PORT_FN(0x004aaad0, "ServerData::reset", ServerData_reset, fp_sd_reset)

// =========================================================================================================================
// construction
// =========================================================================================================================
static RaceServer* __fastcall RaceServer_ctor(RaceServer* self, Edx, void* sm, const char* name, const char* password) {
    uint8_t ls[0x48];          // the LocalService offered: name, type, version, has-password, password; the rest as the frame had it
    self->vtbl = (const void*)(uintptr_t)VT_IRaceServer;
    {
        uint8_t* c = (uint8_t*)self->cars;
        for (int i = 7; i >= 0; i--) {
            tcall<void*>(S_ServerNetCarInfo_ctor, c);
            c += 0xf5;
        }
    }
    self->vtbl = (const void*)(uintptr_t)VT_RaceServer;
    self->sm = sm;
    ccall<int>(S_smart_strncpy, (char*)ls, name, 0x20);
    WR32(ls + 0x24, I32(G_MULTI_VERSION));
    WR32(ls + 0x20, 0x1001);
    ls[0x2c] = password ? 1 : 0;
    if (password) crt_strcpy((char*)ls + 0x34, password);    // FIX CANDIDATE: unbounded (16 bytes there)
    self->service = tcall<uint32_t>(F_SM_OfferUnboundService, self->sm, (void*)ls, 8);
    self->state = 1;
    int16_t lo;
    memcpy(&lo, ls + 0x30, 2);                               // what OfferUnboundService made the service's sessions
    self->base = lo;
    self->owner = ccall<int>(F_TaskGetID);
    nt::crt_zero((uint8_t*)self + 4, 0x245);                 // the users (and the 4 bytes after them)
    nt::crt_zero(self->cars, 0x1eb);                         // the cars and ncars
    for (int i = 0; i < 5; i++) self->_111c[i] = 0;
    nt::crt_zero(self->game, 0xd);
    self->cars_locked = 0;
    self->proposing = 0;
    self->iroc_user = 0;
    self->serial = 0;
    self->chat_cb = 0;
    ccall<void>(S_CreateTrackVersion, self->track_version);
    self->last_tick = net_PTimeNow();                        // site 0x4aac13
    return self;
}
static void fp_rs_ctor(Footprint& f, RaceServer*, Edx, void*, const char*, const char*) { f.replay_only = R_SM; }
PORT_FN(0x004aaae0, "RaceServer::RaceServer", RaceServer_ctor, fp_rs_ctor)

static void __fastcall RaceServer_dtor(RaceServer* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_RaceServer;
    tcall<void>(F_SM_DisconnectService, self->sm, self->service, 1);
    tcall<void>(F_SM_WithdrawService, self->sm, self->service);
    self->sm = 0;
    self->service = 0;
    self->vtbl = (const void*)(uintptr_t)VT_IRaceServer;
}
static void fp_rs_dtor(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SM; }
PORT_FN(0x004aac30, "RaceServer::~RaceServer", RaceServer_dtor, fp_rs_dtor)

static uint8_t __fastcall RaceServer_Ok(RaceServer* self, Edx) { return self->sm && self->service ? 1 : 0; }
static void fp_rs_ok(Footprint& f, RaceServer*, Edx) { }
PORT_FN(0x004aac80, "RaceServer::Ok", RaceServer_Ok, fp_rs_ok)

// =========================================================================================================================
// the users
// =========================================================================================================================
// every user in is in state st (count: how many, up to the first that isn't)
static uint8_t __fastcall everyone_in_state(RaceServer* self, Edx, int st, int32_t* count) {
    int i = 0, n = 0;
    for (; i < 8; i++)
        if (self->users[i].id) {
            if (self->users[i].status != st) break;
            n++;
        }
    if (count) *count = n;
    return i == 8 ? 1 : 0;
}
static void fp_eis(Footprint& f, RaceServer*, Edx, int, int32_t* c) { if (c) f.add(c, 4, "the count"); }
PORT_FN(0x004aaca0, "RaceServer::everyone_in_state", everyone_in_state, fp_eis)

static uint8_t __fastcall everyone_approved(RaceServer* self, Edx, int32_t* count) {
    int i = 0, n = 0;
    for (; i < 8; i++)
        if (self->users[i].id) {
            if (self->users[i].status == 0) break;
            n++;
        }
    if (count) *count = n;
    return i == 8 ? 1 : 0;
}
static void fp_ea(Footprint& f, RaceServer*, Edx, int32_t* c) { if (c) f.add(c, 4, "the count"); }
PORT_FN(0x004aace0, "RaceServer::everyone_approved", everyone_approved, fp_ea)

// the user is gone: its slot freed, everyone told (0x26 with its id). The index unchecked.
static void __fastcall RS_user_disconnected(RaceServer* self, Edx, int idx) {
    ServerData* u = &self->users[idx];
    const int32_t id = u->id;
    if (!id) return;
    uint8_t p[8];
    WR32(p + 4, id);
    u->id = 0;
    p[3] = 0x26;
    broadcast(self, p, 8);
}
static void fp_user_disc(Footprint& f, RaceServer*, Edx, int) { f.replay_only = R_SEND; }
PORT_FN(0x004aad20, "RaceServer::user_disconnected", RS_user_disconnected, fp_user_disc)

// the id of the next user in after `id` (0: from the first), round the 8 slots. FIX CANDIDATE: never returns with nobody in.
static int __fastcall next_user(RaceServer* self, Edx, int id) {
    int i;
    if (!id) i = 0;
    else {
        i = (id & 0xff) + 1;
        ASSERT_MSG(id & (int)0xffffff00, CP(0x004fd870));
    }
    for (;;) {
        if (i == 8) i = 0;
        if (self->users[i].id != 0) break;
        i++;
    }
    return self->users[i].id;
}
static void fp_next_user(Footprint& f, RaceServer*, Edx, int) { f.add(&f, 0, "nothing"); }
PORT_FN(0x004aad60, "RaceServer::next_user", next_user, fp_next_user)

// 17 s without a word: disconnected (reason 0x102)
static void __fastcall check_for_dead_users(RaceServer* self, Edx) {
    const int limit = net_PTimeNow() - 0x4268;               // site 0x4aadeb
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        if (u->id && u->last_heard < limit) {
            sm_disconnect(self->sm, self->base + i, 0x102);
            user_disconnected(self, i);
            LOG(CP(0x004fd88c), i);
        }
    }
}
static void fp_cfdu(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004aade0, "RaceServer::check_for_dead_users", check_for_dead_users, fp_cfdu)

static void __fastcall check_for_singleton_player(RaceServer* self, Edx) {
    if (vcall<int>(self, RS_CountUsers) <= 1 && self->state > 1) tcall<void>(S_reset, self);
}
static void fp_cfsp(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004aae50, "RaceServer::check_for_singleton_player", check_for_singleton_player, fp_cfsp)

// the proposal's AI cars, in front of the players' (moved up): each handed to the next user in, named "viper" / "#!@^&*",
// the default setup. FIX CANDIDATE: the car count isn't bounded (more than 8: past cars[7]).
static void __fastcall add_ai_cars(RaceServer* self, Edx) {
    for (int i = 0; i < self->ncars; i++) self->cars[i].info.human = 1;
    int n = I32((uintptr_t)self + 0x1164);                   // game +0x2c: the proposal's aicars
    if (n <= 0) return;
    ccall<void*>(F_memmove, (void*)((uint8_t*)self + 0x918 + n * 0xf5), (void*)((uint8_t*)self + 0x918), self->ncars * 0xf5);
    int prev = 0;
    self->ncars += n;
    do {
        const int k = n - 1;
        uint8_t* car = (uint8_t*)self + 0x918 + k * 0xf5;
        const int id = tcall<int>(S_next_user, self, prev);
        prev = id;
        WR32(car, id);
        crt_strcpy((char*)car + 0x15, CP(0x004fd8bc));       // "viper"
        WR32(car + 0xc4, k & 7);                             // the car's argument
        car[0xc8] = 0;                                       // no hornball
        uint8_t setup[0x8c];
        ccall<void>(F_CarFileMakeDefaultSetup, (void*)setup);
        crt_copy(car + 0x38, setup, 0x8c);
        crt_strcpy((char*)car + 8, CP(0x004fd8c4));          // "#!@^&*"
        car[0xd8] = 0;
        n = k;
    } while (n != 0);
}
static void fp_add_ai(Footprint& f, RaceServer*, Edx) { f.replay_only = "calls CarFileMakeDefaultSetup (the game's car code)"; }
PORT_FN(0x004aae70, "RaceServer::add_ai_cars", add_ai_cars, fp_add_ai)

static void __fastcall RS_broadcast(RaceServer* self, Edx, const void* p, int len) {
    for (int i = 0; i < 8; i++)
        if (self->users[i].id) sm_send_reliable(self->sm, self->base + i, p, len, 0);
}
static void fp_broadcast(Footprint& f, RaceServer*, Edx, const void*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004aafe0, "RaceServer::broadcast", RS_broadcast, fp_broadcast)

// index | session << 8 | serial << 16
static int __fastcall create_userid(RaceServer* self, Edx, int sess) {
    const int serial = self->serial + 1;
    self->serial = serial;
    return (int)((uint32_t)(sess - self->base) | (uint32_t)serial << 16 | (uint32_t)sess << 8);
}
static void fp_cuid(Footprint& f, RaceServer* self, Edx, int) { f.add(&self->serial, 4, "the serial"); }
PORT_FN(0x004ab030, "RaceServer::create_userid", create_userid, fp_cuid)

// a name another user has: cut to 9 and "-<index + 1>" added
static void __fastcall make_unique_username(RaceServer* self, Edx, char* name, int idx) {
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        if (u->id && !ccall<int>(F_stricmp, (const char*)u->name, (const char*)name)) {
            char buf[16];
            name[9] = 0;
            ccall<int>(F_sprintf, buf, CP(0x004fd8cc), (const char*)name, idx + 1);
            crt_strcpy(name, buf);
            return;
        }
    }
}
static void fp_mun(Footprint& f, RaceServer*, Edx, char* name, int) { f.add(name, 16, "the name"); }
PORT_FN(0x004ab060, "RaceServer::make_unique_username", make_unique_username, fp_mun)

// 0x2d: every slot's status (its low byte)
static void __fastcall RS_send_status_tab(RaceServer* self, Edx) {
    uint8_t p[12];
    p[3] = 0x2d;
    for (int i = 0; i < 8; i++) p[4 + i] = (uint8_t)self->users[i].status;
    broadcast(self, p, 0xc);
}
static void fp_sst(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ab0f0, "RaceServer::send_status_tab", RS_send_status_tab, fp_sst)

// 0x23 to one session: the new user's id, then every slot's id
static void __fastcall send_userlist(RaceServer* self, Edx, int sess, int id) {
    uint8_t p[0x28];
    WR32(p + 4, id);
    p[3] = 0x23;
    for (int i = 0; i < 8; i++) WR32(p + 8 + 4 * i, self->users[i].id);
    sm_send_reliable(self->sm, sess, p, 0x28, 0);
}
static void fp_sul(Footprint& f, RaceServer*, Edx, int, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ab130, "RaceServer::send_userlist", send_userlist, fp_sul)

// the reliable send of a sync packet came back: its RPUserData is (RaceServer*, user index)
static void __cdecl rpi_cb(uint32_t result, const uint32_t* data) {
    tcall<void>(S_sync_rpi_cb, (void*)(uintptr_t)data[0], (int)data[1], result);
}
static void fp_rpi_cb(Footprint& f, uint32_t, const uint32_t* data) {
    f.add((uint8_t*)(uintptr_t)data[0] + 4 + 0x122 * data[1] + 0x111, 1, "the user's sync_acked");
}
PORT_FN(0x004ab180, "RaceServer::rpi_cb", rpi_cb, fp_rpi_cb)

static void __fastcall sync_rpi_cb(RaceServer* self, Edx, int idx, uint32_t result) {
    self->users[idx].sync_acked = (result & 5) == 5 ? 1 : 0;
}
static void fp_srpi(Footprint& f, RaceServer* self, Edx, int idx, uint32_t) { f.add(&self->users[idx].sync_acked, 1, "sync_acked"); }
PORT_FN(0x004ab1a0, "RaceServer::sync_rpi_cb", sync_rpi_cb, fp_srpi)

// with a round trip over 125 ms: 1000 / (bps / (header + 24 x the cars it gets)); else 66 ms. FIX CANDIDATE: both divisions.
static void __fastcall calc_send_interval(RaceServer* self, Edx, ServerData* u) {
    if (u->rtt > 0x7d) {
        int n = 0;
        const int nc = self->ncars;
        for (int k = 0; k < nc; k++)
            if (self->cars[k].info.user != u->id) n++;
        const int hdr = vcall<int>(SM(self)->sock, SOCK_GetHeaderSize);
        const int bps = vcall<int>(SM(self)->sock, SOCK_GetBPS);
        const int q = bps / (hdr + n * 24);
        u->send_interval = 1000 / q;
    } else {
        u->send_interval = 0x42;
    }
}
static void fp_csi(Footprint& f, RaceServer*, Edx, ServerData* u) { f.add(&u->send_interval, 4, "send_interval"); }
PORT_FN(0x004ab1d0, "RaceServer::calc_send_interval", calc_send_interval, fp_csi)

// the IROC user's own (human) car
static NetCarInfo* __fastcall get_iroc_car(RaceServer* self, Edx) {
    const int nc = self->ncars;
    for (int i = 0; i < nc; i++)
        if (self->cars[i].info.user == self->iroc_user && self->cars[i].info.human) return &self->cars[i].info;
    return 0;
}
static void fp_gic(Footprint& f, RaceServer*, Edx) { }
PORT_FN(0x004ab260, "RaceServer::get_iroc_car", get_iroc_car, fp_gic)

// =========================================================================================================================
// the packets
// =========================================================================================================================
// 0x21: a client asks in. Its track version (+0x30, 32 bytes) and protocol table (+0x11, 31 bytes: MULTI_VERSION and
// the game packets' sizes) must match.
static void __fastcall dispatch_AddMePacket(RaceServer* self, Edx, uint8_t* pkt, int sess) {
    bool ok = false;
    {
        const volatile uint8_t* a = pkt + 0x30;
        const volatile uint8_t* b = (const volatile uint8_t*)self->track_version;
        int i = 0;
        while (i < 0x20 && a[i] == b[i]) i++;
        if (i == 0x20) ok = ccall<uint8_t>(S_no_bogus_crcs, (void*)(pkt + 0x30)) != 0;
    }
    bool same = true;
    {
        const volatile uint8_t* a = pkt + 0x11;
        const volatile uint8_t* b = (const volatile uint8_t*)(uintptr_t)G_PROTOCOL;
        for (int i = 0; i < 0x1f; i++)
            if (a[i] != b[i]) { same = false; break; }
    }
    if (!ok || !same) {
        LOG(CP(0x004fd930));
        sm_disconnect(self->sm, sess, ok ? 0x101 : 0x100);
        return;
    }
    const int id = tcall<int>(S_create_userid, self, sess);
    ASSERT_MSG(id & (int)0xffffff00, CP(0x004fd870));
    const int idx = id & 0xff;
    ServerData* u = &self->users[idx];
    if (u->id) {
        LOG(CP(0x004fd8f0), idx, (const char*)u->name);
        return;
    }
    char* name = (char*)pkt + 4;
    tcall<void>(S_make_unique_username, self, name, idx);
    u->id = id;
    ccall<int>(S_smart_strncpy, (char*)u->name, (const char*)name, 0xd);
    u->status = 0;
    tcall<void>(S_send_userlist, self, sess, id);
    if (self->proposing) {
        uint8_t p[0x40];
        p[3] = 0x2a;
        crt_copy(p + 4, &self->prop, 0x3c);
        sm_send_reliable(self->sm, sess, p, 0x40, 0);
    }
    {
        uint8_t p[0x19];
        p[3] = 0x25;
        crt_copy(p + 4, u, 0x15);
        broadcast(self, p, 0x19);
    }
    u->last_heard = net_PTimeNow();                          // site 0x4ab3ee
    tcall<void>(S_ServerData_reset, u);
    char str[0x40];
    nt::crt_zero(str, 0x10);
    netc::SessionMgr* m = SM(self);
    vcall<void>(m->sock, SOCK_MakeStr, (const void*)((uint8_t*)m->sessions + sess * 0x1c + 0xc), (char*)str);
    LOG(CP(0x004fd8d8), (const char*)u->name, (const char*)str);
}
static void fp_addme(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ab2c0, "RaceServer::dispatch_AddMePacket", dispatch_AddMePacket, fp_addme)

// only the first CRC is looked at
static uint8_t __cdecl no_bogus_crcs(int32_t* crcs) {
    for (uint32_t i = 0; i < 1; i++)
        if (crcs[i] == -1) {
            LOG(CP(0x004fd954), i);
            return 0;
        }
    return 1;
}
static void fp_nbc(Footprint& f, int32_t*) { f.replay_only = R_LOG; }
PORT_FN(0x004ab4c0, "no_bogus_crcs", no_bogus_crcs, fp_nbc)

// 0x24: user (+4)'s info back (0x25). FIX CANDIDATE: the index (& 0xff) unchecked.
static void __fastcall dispatch_UserInfoReqPacket(RaceServer* self, Edx, uint8_t* pkt, int sess) {
    int32_t w;
    memcpy(&w, pkt + 4, 4);
    const int idx = w & 0xff;
    ASSERT_MSG(w & (int)0xffffff00, CP(0x004fd870));
    ServerData* u = &self->users[idx];
    if (u->id) {
        uint8_t p[0x19];
        p[3] = 0x25;
        crt_copy(p + 4, u, 0x15);
        sm_send_reliable(self->sm, sess, p, 0x19, 0);
    } else {
        LOG(CP(0x004fd964), idx);
    }
}
static void fp_uir(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ab4f0, "RaceServer::dispatch_UserInfoReqPacket", dispatch_UserInfoReqPacket, fp_uir)

// 0x28: said to everyone -- relayed as 0x27 (id, 0, the text) and handed to the chat callback
static void __fastcall dispatch_SpeakPacket(RaceServer* self, Edx, uint8_t* pkt, int sess) {
    ServerData* u = &self->users[sess - self->base];
    if (!u->id) {
        LOG(CP(0x004fd988), (const char*)u->name);
        return;
    }
    uint8_t p[0x3c];
    p[3] = 0x27;
    WR32(p + 4, u->id);
    p[8] = 0;
    const int n = ccall<int>(S_smart_strncpy, (char*)p + 9, (const char*)pkt + 4, 0x32);
    broadcast(self, p, n + 9);
    if (self->chat_cb) ((void(__cdecl*)(void*, int, const char*))self->chat_cb)(self->chat_data, u->id, (const char*)p + 9);
}
static void fp_speak(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ab580, "RaceServer::dispatch_SpeakPacket", dispatch_SpeakPacket, fp_speak)

// 0x29: said to one user (+4) -- sent it as 0x27 (id, 1, the text) on the session in the target id's second byte.
// FIX CANDIDATE: the target index (& 0xff) unchecked.
static void __fastcall dispatch_WhisperPacket(RaceServer* self, Edx, uint8_t* pkt, int sess) {
    ServerData* u = &self->users[sess - self->base];
    if (!u->id) {
        LOG(CP(0x004fd9e0), (const char*)u->name);
        return;
    }
    int32_t w;
    memcpy(&w, pkt + 4, 4);
    const int t = w & 0xff;
    ASSERT_MSG(w & (int)0xffffff00, CP(0x004fd870));
    ServerData* to = &self->users[t];
    if (!to->id) {
        LOG(CP(0x004fd9b4), (const char*)u->name, (const char*)to->name);
        return;
    }
    uint8_t p[0x3c];
    p[3] = 0x27;
    WR32(p + 4, u->id);
    p[8] = 1;
    const int n = ccall<int>(S_smart_strncpy, (char*)p + 9, (const char*)pkt + 8, 0x32);
    sm_send_reliable(self->sm, pkt[5], p, n + 9, 0);
}
static void fp_whisper(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ab620, "RaceServer::dispatch_WhisperPacket", dispatch_WhisperPacket, fp_whisper)

// 0x2b: the user approves the proposal (+4: its id)
static void __fastcall dispatch_ApprovePacket(RaceServer* self, Edx, uint8_t* pkt, int sess) {
    ServerData* u = &self->users[sess - self->base];
    if (!u->id) {
        LOG(CP(0x004fda4c));
        return;
    }
    int32_t w;
    memcpy(&w, pkt + 4, 4);
    if (w == self->prop.id) {
        u->status = 1;
        send_status_tab(self);
    } else {
        LOG(CP(0x004fda0c));
    }
}
static void fp_approve(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ab710, "RaceServer::dispatch_ApprovePacket", dispatch_ApprovePacket, fp_approve)

// 0x30: the user's car (its id at +5), or (id 0) its choice withdrawn. FIX CANDIDATE: the car count unbounded.
static void __fastcall dispatch_NetCarInfoPacket(RaceServer* self, Edx, uint8_t* pkt, int sess) {
    const int idx = sess - self->base;
    ServerData* u = &self->users[idx];
    if (u->id) {
        int32_t cid;
        memcpy(&cid, pkt + 5, 4);
        if (cid == u->id) {
            ASSERT_MSG(cid & (int)0xffffff00, CP(0x004fd870));
            if ((cid & 0xff) == idx) {
                ServerNetCarInfo* c = &self->cars[self->ncars];
                if (c->info.user == 0) {
                    crt_copy(c, pkt + 5, 0xd9);
                    crt_strcpy(self->cars[self->ncars].info.driver, u->name);
                    self->ncars++;
                    u->status = 2;
                    send_status_tab(self);
                    return;
                }
                LOG(CP(0x004fda70), (const char*)u->name);
            }
        } else if (cid == 0) {
            if (self->cars_locked) {
                LOG(CP(0x004fda98), (const char*)u->name);
                return;
            }
            u->status = 1;
            send_status_tab(self);
            for (int i = 0; i < self->ncars; i++) {
                uint8_t* c = (uint8_t*)&self->cars[i];
                if (u->id == self->cars[i].info.user) {
                    ccall<void*>(F_memmove, (void*)c, (void*)(c + 0xf5), (self->ncars - i) * 0xf5 - 0xf5);
                    volatile uint8_t* z = (volatile uint8_t*)self + 0x823 + self->ncars * 0xf5;
                    for (int k = 0; k < 0xf5; k++) z[k] = 0;
                    self->ncars--;
                }
            }
            return;
        }
    }
    LOG(CP(0x004fdac0), sess, (const char*)u->name);
    sm_disconnect(self->sm, sess, 0);
}
static void fp_nci(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ab770, "RaceServer::dispatch_NetCarInfoPacket", dispatch_NetCarInfoPacket, fp_nci)

// 0x32: car (+4, a signed byte) asked for: 0x33 with the car, the round trip to its owner's session, and for a player's
// car in an IROC race the IROC car's name and setup. FIX CANDIDATE: the car index unchecked (and so the session from the
// owner's id); get_iroc_car's 0 used.
static void __fastcall dispatch_NetCarInfoRequestPacket(RaceServer* self, Edx, uint8_t* pkt, int sess) {
    uint8_t p[0xde];
    p[3] = 0x33;
    const int ci = (int8_t)pkt[4];
    p[4] = (uint8_t)ci;
    crt_copy(p + 5, (uint8_t*)self + 0x918 + ci * 0xf5, 0xd9);
    int32_t w;
    memcpy(&w, p + 5, 4);                                    // the car's owner: its session's round trip
    ASSERT_MSG(w & (int)0xffffff00, CP(0x004fd870));
    const int s = self->base + (w & 0xff);
    netc::SessionMgr* m = SM(self);
    const int lat = tcall<int>(F_RDP_GetEstRTLatency, (void*)&m->rdp, (const void*)((uint8_t*)m->sessions + s * 0x1c + 0xc));
    WR32(p + 0xd9, lat);
    if (U8((uintptr_t)self + 0x10c8) && ((uint8_t*)self)[0x9f0 + ci * 0xf5]) {
        const uint8_t* ir = (const uint8_t*)tcall<NetCarInfo*>(S_get_iroc_car, self);
        crt_strcpy((char*)p + 0x1a, (const char*)ir + 0x15);
        crt_copy(p + 0x3d, ir + 0x38, 0x8c);
    }
    sm_send_reliable(self->sm, sess, p, 0xde, 0);
}
static void fp_ncir(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ab980, "RaceServer::dispatch_NetCarInfoRequestPacket", dispatch_NetCarInfoRequestPacket, fp_ncir)

// 0x34: race ready (status 4)
static void __fastcall dispatch_RaceReadyPacket(RaceServer* self, Edx, uint8_t*, int sess) {
    ServerData* u = &self->users[sess - self->base];
    if (u->id) u->status = 4;
    else LOG(CP(0x004fdafc));
}
static void fp_rr(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_LOG; }
PORT_FN(0x004abaa0, "RaceServer::dispatch_RaceReadyPacket", dispatch_RaceReadyPacket, fp_rr)

// 0x38: a sync reply. A first send (class 2) of an acked exchange is a sample; under 40, the next exchange (the packet
// itself back as 0x37, +4 = now); at 40, the middle 32 by round trip averaged: dt = their clock word - rtt / 2, status 5,
// and 0x2e 0x11. FIX CANDIDATE: a sync packet after the 40th sample writes samples[40] over nsamples.
static void __fastcall dispatch_SyncPacket(RaceServer* self, Edx, uint8_t* pkt, int sess) {
    const int idx = sess - self->base;
    ServerData* u = &self->users[idx];
    if (!u->id) {
        LOG(CP(0x004fdb4c));
        return;
    }
    const int now = net_PTimeNow();                          // site 0x4abb0d
    if (u->sync_acked && (uint8_t)((pkt[0] >> 4) & U8(G_SEND_CLASS_MASK)) == 2) {
        u->samples[u->nsamples].rtt = (uint16_t)((uint16_t)now - (uint16_t)u->sync_sent);
        int32_t r;
        memcpy(&r, pkt + 4, 4);
        u->samples[u->nsamples].remote = r;
        u->nsamples++;
    }
    if (u->nsamples < 0x28) {
        WR32(pkt + 4, now);
        u->sync_sent = now;
        pkt[3] = 0x37;
        uint32_t rpi[3] = {S_rpi_cb, (uint32_t)(uintptr_t)self, (uint32_t)idx};
        tcall<void>(F_SM_SendReliable, self->sm, sess, (const void*)pkt, 0x34, (const void*)rpi, (uint8_t)1);
        u->sync_acked = 0;
        return;
    }
    ccall<void>(F_qsort, (void*)u->samples, 0x28, 6, (void*)(uintptr_t)S_sync_sample_cmp);
    int64_t sr = 0, st = 0;
    for (int k = 4; k < 4 + 0x20; k++) {
        sr += (int64_t)u->samples[k].remote;
        st += (int64_t)(uint32_t)u->samples[k].rtt;
    }
    typedef int64_t(__stdcall * Alldiv_t)(int64_t, int64_t);
    const int64_t ar = ((Alldiv_t)(uintptr_t)F_alldiv)(sr, 0x20);
    const int64_t at = ((Alldiv_t)(uintptr_t)F_alldiv)(st, 0x20);
    u->dt = (int32_t)ar;
    u->rtt = (int32_t)at;
    u->dt -= (int32_t)at / 2;
    tcall<void>(S_calc_send_interval, self, u);
    u->last_send = 0;
    LOG(CP(0x004fdb28), u->rtt, u->dt, (const char*)u->name);
    u->status = 5;
    uint8_t p[8];
    p[3] = 0x2e;
    WR32(p + 4, 0x11);
    sm_send_reliable(self->sm, sess, p, 8, 0);
}
static void fp_sync(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004abae0, "RaceServer::dispatch_SyncPacket", dispatch_SyncPacket, fp_sync)

// qsort's comparator: by round trip
static int __cdecl sync_sample_cmp(const void* a, const void* b) {
    return (int)*(const uint16_t*)a - (int)*(const uint16_t*)b;
}
static void fp_ssc(Footprint& f, const void*, const void*) { f.pure = true; }
PORT_FN(0x004abcd0, "sync_sample_cmp", sync_sample_cmp, fp_ssc)

// 0x39: go ready (status 6)
static void __fastcall dispatch_GoReadyPacket(RaceServer* self, Edx, uint8_t*, int sess) {
    ServerData* u = &self->users[sess - self->base];
    if (u->id) u->status = 6;
    else LOG(CP(0x004fdb70));
}
static void fp_gr(Footprint& f, RaceServer*, Edx, uint8_t*, int) { f.replay_only = R_LOG; }
PORT_FN(0x004abcf0, "RaceServer::dispatch_GoReadyPacket", dispatch_GoReadyPacket, fp_gr)

// 0x22
static void __fastcall dispatch_KeepAlivePacket(RaceServer* self, Edx, uint8_t*, int sess) {
    ServerData* u = &self->users[sess - self->base];
    if (u->id) u->last_heard = net_PTimeNow();               // site 0x4abd4e
}
static void fp_ka(Footprint& f, RaceServer* self, Edx, uint8_t*, int sess) {
    f.add(&self->users[sess - self->base].last_heard, 4, "last_heard");
}
PORT_FN(0x004abd30, "RaceServer::dispatch_KeepAlivePacket", dispatch_KeepAlivePacket, fp_ka)

// 0x3b: a deity cast, relayed to everyone else as 0x3c
static void __fastcall deitycast(RaceServer* self, Edx, uint8_t* pkt, int sess, int len) {
    const int idx = sess - self->base;
    ServerData* u = &self->users[idx];
    if (!u->id) {
        LOG(CP(0x004fdb9c), (const char*)u->name);
        return;
    }
    pkt[3] = 0x3c;
    for (int i = 0; i < 8; i++)
        if (self->users[i].id && i != idx) sm_send_reliable(self->sm, self->base + i, pkt, len, 0);
}
static void fp_deity(Footprint& f, RaceServer*, Edx, uint8_t*, int, int) { f.replay_only = R_SEND; }
PORT_FN(0x004abd60, "RaceServer::deitycast", deitycast, fp_deity)

// the unreliable car packet: (len - 2) / 24 records [car index][CarNetPacket], each for a car the sender drives stored
// with its time word moved to the server's clock. FIX CANDIDATE: the car index (a signed byte) unchecked; a length under 2.
static void __fastcall race_packet(RaceServer* self, Edx, uint8_t* pkt, int sess, int len) {
    const int now = net_PTimeNow();                          // site 0x4abdfb
    const int idx = sess - self->base;
    ServerData* u = &self->users[idx];
    const int32_t uid = u->id;
    int n = (int)((uint32_t)(len - 2) / 24u);
    if (!uid) return;
    if (n > 0) {
        const uint8_t* r = pkt + 2;
        do {
            uint8_t* car = (uint8_t*)self + 0x918 + (int8_t)r[0] * 0xf5;
            int32_t cu;
            memcpy(&cu, car, 4);
            if (cu == uid) {
                crt_copy(car + 0xd9, r, 24);
                uint16_t t;
                memcpy(&t, r + 1, 2);
                t = (uint16_t)(t - (uint16_t)u->dt);
                memcpy(car + 0xda, &t, 2);
                WR32(car + 0xf1, now);
            }
            r += 24;
        } while (--n);
    }
    u->last_heard = now;
}
static void fp_race_packet(Footprint& f, RaceServer* self, Edx, uint8_t*, int, int) {
    f.add(self->cars, sizeof self->cars, "the cars' records");
    f.add(self->users, sizeof self->users, "last_heard");
}
PORT_FN(0x004abdf0, "RaceServer::race_packet", race_packet, fp_race_packet)

// back to RS_APPROVE: the cars cleared, the users approved no more (0x2e 9), entries open again
static void __fastcall RS_reset(RaceServer* self, Edx) {
    self->cars_locked = 0;
    self->state = 1;
    nt::crt_zero(self->cars, 0x1eb);
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        if (u->id) {
            u->status = 0;
            u->last_heard = net_PTimeNow();                  // site 0x4abf0a
        }
    }
    uint8_t p[8];
    p[3] = 0x2e;
    WR32(p + 4, 9);
    broadcast(self, p, 8);
    tcall<void>(F_SM_AcceptConnections, self->sm, self->service);
    send_status_tab(self);
}
static void fp_reset(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004abed0, "RaceServer::reset", RS_reset, fp_reset)

// =========================================================================================================================
// the owner's calls and the state machine
// =========================================================================================================================
static void __fastcall RestartRace(RaceServer* self, Edx) {
    owner_check(self, 0x550, 0x004fdbcc, 0x004fdbd8);
    const int st = self->state;
    ASSERT_MSG(st == 8 ? 1 : 0, CP(0x004fdbfc), st);
    tcall<void>(S_RACE_PRESTAGE, self);
    self->state = 6;
}
static void fp_restart(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004abf60, "RaceServer::RestartRace", RestartRace, fp_restart)

static void __fastcall GoBackToChat(RaceServer* self, Edx) {
    owner_check(self, 0x55c, 0x004fdc20, 0x004fdc2c);
    const int st = self->state;
    if (st == 1) return;
    ASSERT_MSG(st == 8 ? 1 : 0, CP(0x004fdc50), st);
    tcall<void>(S_RACE_APPROVE, self);
    self->state = 1;
}
static void fp_gbtc(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004abfe0, "RaceServer::GoBackToChat", GoBackToChat, fp_gbtc)

static void __fastcall ProceedToChooseCar(RaceServer* self, Edx) {
    owner_check(self, 0x56c, 0x004fdc78, 0x004fdc84);
    const int st = self->state;
    ASSERT_MSG(st == 2 ? 1 : 0, CP(0x004fdca8), st);
    tcall<void>(S_CLOSE_ENTRY_CHOOSE_CAR, self);
    self->state = 3;
}
static void fp_ptcc(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac070, "RaceServer::ProceedToChooseCar", ProceedToChooseCar, fp_ptcc)

static void __fastcall BackToChooseTrack(RaceServer* self, Edx) {
    owner_check(self, 0x578, 0x004fdcd4, 0x004fdce0);
    const int st = self->state;
    ASSERT_MSG(st == 3 ? 1 : 0, CP(0x004fdd04), st);
    tcall<void>(S_CHOOSE_CAR_APPROVE, self);
    self->state = 1;
}
static void fp_btct(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac0f0, "RaceServer::BackToChooseTrack", BackToChooseTrack, fp_btct)

// the transitions with nothing to do but the owner check
static void __fastcall trans_APPROVE_CLOSE_ENTRY(RaceServer* self, Edx) { owner_check(self, 0x584, 0x004fdd30, 0x004fdd3c); }
static void __fastcall trans_CLOSE_ENTRY_APPROVE(RaceServer* self, Edx) { owner_check(self, 0x58f, 0x004fdd60, 0x004fdd6c); }
static void __fastcall trans_SYNC_PRESTAGE(RaceServer* self, Edx) { owner_check(self, 0x5ee, 0x004fde50, 0x004fde5c); }
static void __fastcall trans_STAGE_RACE(RaceServer* self, Edx) { owner_check(self, 0x60f, 0x004fdec8, 0x004fded4); }
static void fp_trans_task(Footprint& f, RaceServer*, Edx) { f.add(&f, 0, "nothing (the task calls only)"); }
PORT_FN(0x004ac170, "RaceServer::transRS_APPROVE_RS_CLOSE_ENTRY", trans_APPROVE_CLOSE_ENTRY, fp_trans_task)
PORT_FN(0x004ac1c0, "RaceServer::transRS_CLOSE_ENTRY_RS_APPROVE", trans_CLOSE_ENTRY_APPROVE, fp_trans_task)
PORT_FN(0x004ac4f0, "RaceServer::transRS_SYNC_RS_PRESTAGE", trans_SYNC_PRESTAGE, fp_trans_task)
PORT_FN(0x004ac600, "RaceServer::transRS_STAGE_RS_RACE", trans_STAGE_RACE, fp_trans_task)

// entries closed: whoever hasn't approved (and is connected) is dropped (reason 4); the game frozen and sent (0x2c)
static void __fastcall trans_CLOSE_ENTRY_CHOOSE_CAR(RaceServer* self, Edx) {
    owner_check(self, 0x59a, 0x004fdd90, 0x004fdd9c);
    tcall<void>(F_SM_RefuseConnections, self->sm, self->service);
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        if (u->id && u->status == 0 && (tcall<uint8_t>(F_SM_GetStatus, self->sm, self->base + i) & 1)) {
            sm_disconnect(self->sm, self->base + i, 4);
            user_disconnected(self, i);
        }
    }
    uint8_t p[0x38];
    crt_copy(p + 4, (uint8_t*)&self->prop + 8, 0x34);
    crt_copy(self->game, p + 4, 0x34);
    p[3] = 0x2c;
    broadcast(self, p, 0x38);
}
static void fp_t_ce_cc(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac210, "RaceServer::transRS_CLOSE_ENTRY_RS_CHOOSE_CAR", trans_CLOSE_ENTRY_CHOOSE_CAR, fp_t_ce_cc)

// back to the track: everyone unapproved (status 0 in every slot), each user told (0x2e 9)
static void __fastcall trans_CHOOSE_CAR_APPROVE(RaceServer* self, Edx) {
    owner_check(self, 0x5b9, 0x004fddc0, 0x004fddcc);
    tcall<void>(F_SM_AcceptConnections, self->sm, self->service);
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        u->status = 0;
        if (u->id) {
            uint8_t p[8];
            p[3] = 0x2e;
            WR32(p + 4, 9);
            sm_send_reliable(self->sm, self->base + i, p, 8, 0);
        }
    }
    send_status_tab(self);
}
static void fp_t_cc_a(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac310, "RaceServer::transRS_CHOOSE_CAR_RS_APPROVE", trans_CHOOSE_CAR_APPROVE, fp_t_cc_a)

// the cars are in: the AI cars added, the car list sent (0x31: the cars' user ids), no unchoosing any more.
// FIX CANDIDATE: more than 8 cars overrun the packet's frame.
static void __fastcall trans_CHOOSE_CAR_GET_CAR(RaceServer* self, Edx) {
    owner_check(self, 0x5d0, 0x004fddf0, 0x004fddfc);
    tcall<void>(S_add_ai_cars, self);
    uint8_t p[0x24];
    p[3] = 0x31;
    nt::crt_zero(p + 4, 8);
    const int nc = self->ncars;
    for (int k = 0; k < nc; k++) WR32(p + 4 + 4 * k, self->cars[k].info.user);
    broadcast(self, p, 0x24);
    self->cars_locked = 1;
}
static void fp_t_cc_gc(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac3d0, "RaceServer::transRS_CHOOSE_CAR_RS_GET_CAR", trans_CHOOSE_CAR_GET_CAR, fp_t_cc_gc)

static void __fastcall trans_GET_CAR_SYNC(RaceServer* self, Edx) {
    owner_check(self, 0x5e0, 0x004fde20, 0x004fde2c);
    uint8_t p[8];
    p[3] = 0x2e;
    WR32(p + 4, 0xf);
    broadcast(self, p, 8);
}
static void fp_t_gc_s(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac480, "RaceServer::transRS_GET_CAR_RS_SYNC", trans_GET_CAR_SYNC, fp_t_gc_s)

// the start: each user gets 0x3a with the green flag's time on its own clock (now + 10 s + dt)
static void __fastcall trans_PRESTAGE_STAGE(RaceServer* self, Edx) {
    owner_check(self, 0x5f9, 0x004fde80, 0x004fde8c);
    const int go = net_PTimeNow() + 0x2710;                  // site 0x4ac597
    uint8_t p[8];
    p[3] = 0x3a;
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        if (!u->id) continue;
        LOG(CP(0x004fdeb0), (const char*)u->name, u->rtt);
        WR32(p + 4, u->dt + go);
        sm_send_reliable(self->sm, self->base + i, p, 8, 0);
    }
}
static void fp_t_ps_s(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac540, "RaceServer::transRS_PRESTAGE_RS_STAGE", trans_PRESTAGE_STAGE, fp_t_ps_s)

static void __fastcall trans_RACE_PRESTAGE(RaceServer* self, Edx) {
    owner_check(self, 0x61a, 0x004fdef8, 0x004fdf04);
    const int go = net_PTimeNow() + 0x2710;                  // site 0x4ac6a7
    uint8_t p[8];
    p[3] = 0x3a;
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        if (!u->id) continue;
        WR32(p + 4, u->dt + go);
        sm_send_reliable(self->sm, self->base + i, p, 8, 0);
    }
}
static void fp_t_r_ps(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac650, "RaceServer::transRS_RACE_RS_PRESTAGE", trans_RACE_PRESTAGE, fp_t_r_ps)

// the race is over: the cars cleared, every slot unapproved and its sync state reset, each user told (0x2e 9)
static void __fastcall trans_RACE_APPROVE(RaceServer* self, Edx) {
    owner_check(self, 0x62f, 0x004fdf28, 0x004fdf34);
    self->cars_locked = 0;
    self->ncars = 0;
    nt::crt_zero(self->cars, 0x1ea);
    const int now = net_PTimeNow();                          // site 0x4ac774
    tcall<void>(F_SM_AcceptConnections, self->sm, self->service);
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        u->last_heard = now;
        u->status = 0;
        tcall<void>(S_ServerData_reset, u);
        if (u->id) {
            uint8_t p[8];
            p[3] = 0x2e;
            WR32(p + 4, 9);
            sm_send_reliable(self->sm, self->base + i, p, 8, 0);
        }
    }
    send_status_tab(self);
}
static void fp_t_r_a(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac700, "RaceServer::transRS_RACE_RS_APPROVE", trans_RACE_APPROVE, fp_t_r_a)

static void __fastcall checkRS_APPROVE(RaceServer* self, Edx) {
    int32_t n;
    if (tcall<uint8_t>(S_everyone_approved, self, &n) && n > 1) {
        tcall<void>(S_APPROVE_CLOSE_ENTRY, self);
        self->state = 2;
    }
}
static void fp_c_a(Footprint& f, RaceServer* self, Edx) { f.add(&self->state, 4, "state"); }
PORT_FN(0x004ac7f0, "RaceServer::checkRS_APPROVE", checkRS_APPROVE, fp_c_a)

static void __fastcall checkRS_CLOSE_ENTRY(RaceServer* self, Edx) {
    int32_t n;
    if (!tcall<uint8_t>(S_everyone_approved, self, &n) || n <= 1) {
        tcall<void>(S_CLOSE_ENTRY_APPROVE, self);
        self->state = 1;
    }
}
static void fp_c_ce(Footprint& f, RaceServer* self, Edx) { f.add(&self->state, 4, "state"); }
PORT_FN(0x004ac830, "RaceServer::checkRS_CLOSE_ENTRY", checkRS_CLOSE_ENTRY, fp_c_ce)

static void __fastcall checkRS_CHOOSE_CAR(RaceServer* self, Edx) {
    int32_t n = 0;
    if (tcall<uint8_t>(S_everyone_in_state, self, 2, &n) && n > 1) {
        LOG(CP(0x004fdf58), n);
        tcall<void>(S_CHOOSE_CAR_GET_CAR, self);
        self->state = 4;
    }
}
static void fp_c_cc(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac870, "RaceServer::checkRS_CHOOSE_CAR", checkRS_CHOOSE_CAR, fp_c_cc)

static void __fastcall checkRS_GET_CAR(RaceServer* self, Edx) {
    int32_t n;
    if (tcall<uint8_t>(S_everyone_in_state, self, 4, &n) && n > 1) {
        tcall<void>(S_GET_CAR_SYNC, self);
        self->state = 5;
    }
}
static void fp_c_gc(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac8c0, "RaceServer::checkRS_GET_CAR", checkRS_GET_CAR, fp_c_gc)

// everyone synced (status 5 or more): 0x2e 0x12
static void __fastcall checkRS_SYNC(RaceServer* self, Edx) {
    int i = 0;
    for (; i < 8; i++)
        if (self->users[i].id && self->users[i].status < 5) break;
    if (i != 8) return;
    tcall<void>(S_SYNC_PRESTAGE, self);
    uint8_t p[8];
    self->state = 6;
    p[3] = 0x2e;
    WR32(p + 4, 0x12);
    broadcast(self, p, 8);
}
static void fp_c_s(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac900, "RaceServer::checkRS_SYNC", checkRS_SYNC, fp_c_s)

// (the original first runs a loop over the users whose result it never uses: left out, it only reads)
static void __fastcall checkRS_PRESTAGE(RaceServer* self, Edx) {
    if (tcall<uint8_t>(S_everyone_in_state, self, 6, (int32_t*)0)) {
        tcall<void>(S_PRESTAGE_STAGE, self);
        self->state = 7;
    }
}
static void fp_c_ps(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004ac960, "RaceServer::checkRS_PRESTAGE", checkRS_PRESTAGE, fp_c_ps)

static void __fastcall checkRS_STAGE(RaceServer* self, Edx) {
    tcall<void>(S_STAGE_RACE, self);
    self->state = 8;
}
static void fp_c_st(Footprint& f, RaceServer* self, Edx) { f.add(&self->state, 4, "state"); }
PORT_FN(0x004ac9b0, "RaceServer::checkRS_STAGE", checkRS_STAGE, fp_c_st)

// a race proposed (0x2a): user (if any) approves it already, everyone else unapproved
static void __fastcall Propose(RaceServer* self, Edx, const NetProposal* prop, int user) {
    owner_check(self, 0x6c8, 0x004fdf68, 0x004fdf74);
    self->iroc_user = user;
    for (int i = 0; i < 8; i++) self->users[i].status = 0;
    if (user) {
        ASSERT_MSG(user & (int)0xffffff00, CP(0x004fd870));
        self->users[user & 0xff].status = 1;
    }
    send_status_tab(self);
    crt_copy(&self->prop, prop, 0x3c);
    self->prop.id = net_PTimeNow();                          // site 0x4aca9e
    self->proposing = 1;
    const int t = net_PTimeNow();                            // site 0x4acaaf
    uint8_t p[0x40];
    p[3] = 0x2a;
    self->prop.tag = t & 0xff;
    crt_copy(p + 4, &self->prop, 0x3c);
    broadcast(self, p, 0x40);
}
static void fp_propose(Footprint& f, RaceServer*, Edx, const NetProposal*, int) { f.replay_only = R_SEND; }
PORT_FN(0x004ac9e0, "RaceServer::Propose", Propose, fp_propose)

// the physics thread: each user due one gets the cars it doesn't drive that moved since (unreliable), their time words on
// its clock. FIX CANDIDATE: the original's frame holds 7 records; 8 overrun it.
static void __fastcall SendAll(RaceServer* self, Edx) {
    owner_check(self, 0x6ea, 0x004fdf98, 0x004fdfa4);
    const int now = net_PTimeNow();                          // site 0x4acb4b
    for (int i = 0; i < 8; i++) {
        ServerData* u = &self->users[i];
        if (!u->id || !(now - u->last_send > u->send_interval)) continue;
        uint8_t p[2 + 8 * 24];
        int n = 0;
        uint8_t* r = p + 2;
        for (int k = 0; k < 8; k++) {
            ServerNetCarInfo* c = &self->cars[k];
            if (c->info.user == u->id || !(c->rec_time > u->last_send)) continue;
            crt_copy(r, c->rec, 24);
            n++;
            uint16_t t;
            memcpy(&t, r + 1, 2);
            t = (uint16_t)(t + (uint16_t)u->dt);
            memcpy(r + 1, &t, 2);
            r += 24;
        }
        tcall<void>(F_SM_Send, self->sm, self->base + i, (const void*)p, n * 24 + 2, (uint8_t)2);
        u->last_send = now;
    }
}
static void fp_sendall(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004acaf0, "RaceServer::SendAll", SendAll, fp_sendall)

// 0x2f: the IROC car is user's (only a warning when the proposal isn't IROC)
static void __fastcall SetIrocCar(RaceServer* self, Edx, int user, const char*) {
    owner_check(self, 0x70e, 0x004fdfc8, 0x004fdfd4);
    if (!U8((uintptr_t)self + 0x10c8)) LOG(CP(0x004fdff8));
    uint8_t p[8];
    WR32(p + 4, user);
    p[3] = 0x2f;
    broadcast(self, p, 8);
}
static void fp_sic(Footprint& f, RaceServer*, Edx, int, const char*) { f.replay_only = R_SEND; }
PORT_FN(0x004acc30, "RaceServer::SetIrocCar", SetIrocCar, fp_sic)

// the main loop's step: the lag warning, the sessions gone down, the state's check, every packet in, the dead users,
// a lone player. FIX CANDIDATEs: user_disconnected gets the session number; the state table unchecked.
static void __fastcall Tick(RaceServer* self, Edx) {
    owner_check(self, 0x722, 0x004fe038, 0x004fe044);
    const int now = net_PTimeNow();                          // site 0x4acd14
    if (self->last_tick && now - self->last_tick > 0x1f4) LOG(CP(0x004fe068), now - self->last_tick);
    self->last_tick = now;
    for (;;) {
        uint8_t st;
        const int sess = tcall<int>(F_SM_GetNextStatus, self->sm, &st, self->service);
        if (sess == -1) break;
        if (st & 1) continue;
        LOG(CP(0x004fe078), sess, (uint32_t)st);
        user_disconnected(self, sess);
    }
    const uint32_t tbl[10] = {0, S_checkRS_APPROVE, S_checkRS_CLOSE_ENTRY, S_checkRS_CHOOSE_CAR, S_checkRS_GET_CAR,
                              S_checkRS_SYNC, S_checkRS_PRESTAGE, S_checkRS_STAGE, S_checkRS_RACE, 0};
    ASSERT_MSG(1, CP(0x004fe0a0), CP(0x004fe094), 0x752);
    tcall<void>(tbl[self->state], self);
    for (;;) {
        uint8_t pkt[0xe4];
        int len = 0xe2;
        const int sess = tcall<int>(F_SM_UnboundRecv, self->sm, self->service, (void*)pkt, &len);
        if (sess == -1) break;
        const uint32_t kind = (uint8_t)(U8(G_SEND_CLASS_MASK) & pkt[0]);
        if (kind == 2) {
            tcall<void>(S_race_packet, self, (void*)pkt, sess, len);
            continue;
        }
        if (kind != 1) continue;
        uint32_t h;
        switch (pkt[3]) {
        case 0x21: h = S_dispatch_AddMe; break;
        case 0x22: h = S_dispatch_KeepAlive; break;
        case 0x24: h = S_dispatch_UserInfoReq; break;
        case 0x28: h = S_dispatch_Speak; break;
        case 0x29: h = S_dispatch_Whisper; break;
        case 0x2b: h = S_dispatch_Approve; break;
        case 0x30: h = S_dispatch_NetCarInfo; break;
        case 0x32: h = S_dispatch_NetCarInfoRequest; break;
        case 0x34: h = S_dispatch_RaceReady; break;
        case 0x38: h = S_dispatch_Sync; break;
        case 0x39: h = S_dispatch_GoReady; break;
        case 0x3b: tcall<void>(S_deitycast, self, (void*)pkt, sess, len); continue;
        default: VERBOSE(CP(0x004fe0dc), (uint32_t)pkt[0]); continue;
        }
        tcall<void>(h, self, (void*)pkt, sess);
    }
    tcall<void>(S_check_for_dead_users, self);
    tcall<void>(S_check_for_singleton_player, self);
}
static void fp_tick(Footprint& f, RaceServer*, Edx) { f.replay_only = R_SEND; }
PORT_FN(0x004accc0, "RaceServer::Tick", Tick, fp_tick)

static uint8_t __fastcall EveryoneSynchronized(RaceServer* self, Edx) {
    owner_check(self, 0x787, 0x004fe0fc, 0x004fe108);
    return self->state == 6 ? 1 : 0;
}
static void fp_es(Footprint& f, RaceServer*, Edx) { f.add(&f, 0, "nothing (the task calls only)"); }
PORT_FN(0x004acfe0, "RaceServer::EveryoneSynchronized", EveryoneSynchronized, fp_es)

static int __fastcall CountUsers(RaceServer* self, Edx) {
    owner_check(self, 0x793, 0x004fe12c, 0x004fe138);
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (self->users[i].id) n++;
    return n;
}
static void fp_cu(Footprint& f, RaceServer*, Edx) { f.add(&f, 0, "nothing (the task calls only)"); }
PORT_FN(0x004ad040, "RaceServer::CountUsers", CountUsers, fp_cu)

static void* __cdecl CreateRaceServer(void* sm, const char* name, const char* password) {
    RaceServer* s = 0;
    void* p = ccall<void*>(F_MemAlloc, 0x1198);
    if (p) s = tcall<RaceServer*>(S_ctor, p, sm, name, password);
    if (s && !tcall<uint8_t>(S_Ok, s)) {
        vcall<void*>(s, RS_DTOR, 1u);
        s = 0;
    }
    return s;
}
static void fp_crs(Footprint& f, void*, const char*, const char*) { f.replay_only = R_HEAP; }
PORT_FN(0x004ad0b0, "CreateRaceServer", CreateRaceServer, fp_crs)

static void __fastcall SetOwner(RaceServer* self, Edx, int task) { self->owner = task; }
static void fp_so(Footprint& f, RaceServer* self, Edx, int) { f.add(&self->owner, 4, "owner"); }
PORT_FN(0x004ad120, "RaceServer::SetOwner", SetOwner, fp_so)

static uint8_t __fastcall EveryoneApproved(RaceServer* self, Edx) { return self->state == 2 ? 1 : 0; }
static void fp_eapp(Footprint& f, RaceServer*, Edx) { }
PORT_FN(0x004ad130, "RaceServer::EveryoneApproved", EveryoneApproved, fp_eapp)

static uint32_t __fastcall GetService(RaceServer* self, Edx) { return self->service; }
static void fp_gs(Footprint& f, RaceServer*, Edx) { }
PORT_FN(0x004ad150, "RaceServer::GetService", GetService, fp_gs)

static void __fastcall SetChatCallback(RaceServer* self, Edx, void* cb, void* data) {
    self->chat_cb = cb;
    self->chat_data = data;
}
static void fp_scc(Footprint& f, RaceServer* self, Edx, void*, void*) { f.add(&self->chat_cb, 8, "the chat callback"); }
PORT_FN(0x004ad160, "RaceServer::SetChatCallback", SetChatCallback, fp_scc)

// Phase: 0 approving (1, 2), 1 choosing cars (3), 2 getting ready (4, 5, 6 and anything else), 3 racing (7, 8)
static int __fastcall GetPhase(RaceServer* self, Edx) {
    switch (self->state) {
    case 1: case 2: return 0;
    case 3: return 1;
    case 7: case 8: return 3;
    default: return 2;
    }
}
static void fp_gp(Footprint& f, RaceServer*, Edx) { }
PORT_FN(0x004ad180, "RaceServer::GetPhase", GetPhase, fp_gp)

#undef LOG
#undef VERBOSE
#undef ASSERT_MSG
#undef CP
#undef U8
#undef I32
#undef WR32
}  // namespace net_server
}  // namespace
