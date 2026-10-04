// net_session.cpp -- multiplayer stage N1, group B: the session manager, rewritten faithfully (library `multi`:
// session.obj).
//
// SessionMgr (0xd8 bytes, CreateSessionMgr) sits on a ReliableDataPort over an ISocket. It offers services (LocalService:
// a range of channels, a type, a version, a password; unique ids from Random), answers broadcast service requests with
// ServiceInfo packets and keeps the services it hears of (RemoteService, dropped 3 s after a request they didn't answer),
// looks hosts up by name (async, through the window: AsyncServiceRequestBlock and async_msg's texts), connects channels
// (ConnectRequest with the password, ConnectAccept / ConnectReject, DisconnectRequest, GoAway for data on a dead channel),
// and moves the game's packets: Send (unreliable, class 7) and SendReliable on a connected channel, Recv / UnboundRecv
// from each channel's queue of SessionData (Pool<SessionData>), the channels' state changes for GetNextStatus. Session
// control packets: byte 0's flags 0, byte 2 0x1e, byte 3 the type (0 connreq, 1 connreject, 2 connaccept, 3 disconreq,
// 4 goaway, 5 servreq, 6 servinfo, 7 servdown); the reliable ones' callbacks (init_rpi -> sess_rdp_cb -> reliable_cb)
// move the channel's flags.
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this file's functions too,
// so the hooked rewrite or the original is what runs), virtual calls through the vtable (the ISocket's, the
// ReliableDataPort's Tick); the "owned by the task that made it" check every public function starts with (TaskGetName,
// TaskGetID, TaskGetName, TaskGetID, ASSERT_MSG) in the original's order; reads of the SessionMgr's arrays at the points
// the original reads them. The multiplayer code's clock and random numbers as N0 patched them (net_wsock.h): PTimeNow at
// the sites 0x4a5264 (Tick) and 0x4a5672 (ServiceRequestBroadcast), Random at 0x4a5a7e (unique_service_id). Packets built
// in the frame store exactly what the original stores: byte 1 of the session-control packets (HostEntry::send fills it
// for the reliable ones), the password's tail after its NUL, Tick's servreq ReliablePacketInfo's last 4 bytes and
// init_rpi's last 2 are left as the frame had them.
//
// Footprints: whatever sends, receives, allocates or frees (the pools, MemAlloc), logs, or runs a callback is replay_only
// (the session replay of a network game is their in-game check; test/world_net_core.cpp checks them offline); the getters
// read only; GetStatus / GetNextStatus write a channel's flags, SetConnectPassword the password.
//
// Fixes (docs/FIXES.md, "Multiplayer"; each marked `// FIX:` in place, `VP_FIX &&`, so the faithful build is the original):
// find_range steps past a channel in use (it looped forever on one: OfferUnboundService with any channel busy before a
// free range); async_msg answers a state outside 0..5 with its first text (SessionDiscReasonString already checked its
// 0..6); recv_chandata drops a packet longer than SessionData's 0xe4 bytes (Tick passes at most 0xe2); Recv skips the
// unlink when the channel's packet isn't on the global list (it went through 0); GetNextStatus with no channels finds
// none (it divided by 0). recv_connreject / connaccept / disconreq check the wire's channel number against the count.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "net_core.h"
#include "net_wsock.h"

// a harness's marker for "a fix changed what happens here" (nothing in the DLL)
#ifndef VP_FIX_HIT
#define VP_FIX_HIT(what) ((void)0)
#endif

namespace {
namespace net_session {
using namespace netc;

template <typename T> static __forceinline T fn(uint32_t a) { return (T)(uintptr_t)a; }
static __forceinline uint8_t G8(uint32_t a) { return *(volatile uint8_t*)(uintptr_t)a; }
static __forceinline const char* S(uint32_t a) { return (const char*)(uintptr_t)a; }
static __forceinline uint8_t* B(void* p) { return (uint8_t*)p; }
static __forceinline uint32_t ld32(const void* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static __forceinline void st32(void* p, uint32_t v) { memcpy(p, &v, 4); }

typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* Ptr_t)(void*);
typedef void*(__fastcall* PoolCtor_t)(void*, Edx, const char*, int, uint32_t);
typedef void(__fastcall* PoolU8_t)(void*, Edx, uint8_t);
typedef void*(__fastcall* PoolAlloc_t)(void*, Edx);
typedef void(__fastcall* PoolFree_t)(void*, Edx, void*);
typedef void(__fastcall* This_t)(void*, Edx);
typedef uint8_t(__fastcall* U8V_t)(void*, Edx);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef int(__cdecl* IntV_t)();
typedef char*(__cdecl* TaskName_t)(int);
typedef void(__cdecl* Assert_t)(int, const char*, ...);
typedef void*(__cdecl* Memmove_t)(void*, const void*, uint32_t);
typedef char*(__cdecl* Strncpy_t)(char*, const char*, uint32_t);
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
typedef uint8_t(__cdecl* AddrCmp_t)(const void*, const void*);
typedef const char*(__cdecl* Msg_t)(int);
typedef int(__cdecl* Atexit_t)(void*);
typedef void*(__fastcall* XCtor_t)(void*, Edx, const char*);

#define LOG_REPORT fn<Log_t>(F_LogReport)
#define LOG_PANIC fn<Log_t>(F_LogPanic)
static __forceinline void pool_free(PoolBase* p, void* e) { fn<PoolFree_t>(F_PoolBase_free)(p, 0, e); }
static __forceinline uint8_t addr_eq(const void* a, const void* b) { return fn<AddrCmp_t>(F_addr_eq)(a, b); }
static __forceinline void crt_memmove(void* d, const void* s, uint32_t n) { fn<Memmove_t>(F_memmove)(d, s, n); }
static __forceinline void str_copy(void* d, const char* s) { memcpy(d, s, strlen(s) + 1); }   // repne scasb; rep movs
static __forceinline SessionInfo* chan(SessionMgr* m, int s) { return &m->sessions[s]; }   // re-reads the array

// the ISocket's slots used here
static __forceinline uint8_t sock_link_alive(void* s) { return NETC_VFN(s, IS_LinkAlive, uint8_t)(s, 0); }
static __forceinline void sock_bcast_addr(void* s, void* a) { NETC_VFN(s, IS_GetBroadcastAddr, void, void*)(s, 0, a); }
static __forceinline void sock_make_str(void* s, const void* a, char* buf) {
    NETC_VFN(s, IS_MakeStr, void, const void*, char*)(s, 0, a, buf);
}

// this group's own, by address
typedef void(__fastcall* DPSend_t)(void*, Edx, void*, int, const void*);
typedef uint8_t(__fastcall* DPRecv_t)(void*, Edx, void*, int*, void*);
typedef void(__fastcall* RDPSendRel_t)(void*, Edx, const void*, int, const void*, const void*);
typedef int(__fastcall* Latency_t)(void*, Edx, const void*);
typedef void(__fastcall* SetCB_t)(void*, Edx, void*, const void*);
typedef void*(__fastcall* RDPCtor_t)(void*, Edx, void*, int);
typedef void(__fastcall* InitRpi_t)(void*, Edx, RPI*, uint8_t, uint8_t);
typedef void(__fastcall* SMInt_t)(void*, Edx, int);
typedef void(__fastcall* SMInt2_t)(void*, Edx, int, int);
typedef void(__fastcall* SMU32_t)(void*, Edx, uint32_t);
typedef void(__fastcall* SMLs_t)(void*, Edx, const LocalService*);
typedef void(__fastcall* SMPkt2_t)(void*, Edx, const uint8_t*, const void*);
typedef void(__fastcall* SMPkt3_t)(void*, Edx, const uint8_t*, int, const void*);
typedef void(__fastcall* SMMake_t)(void*, Edx, uint8_t*, const LocalService*);
typedef uint8_t(__fastcall* SMRange_t)(void*, Edx, int, LocalService*);
typedef uint32_t(__fastcall* SMOffer_t)(void*, Edx, const LocalService*);
typedef int(__fastcall* SMEmpty_t)(void*, Edx);
typedef void(__fastcall* SMConnRs_t)(void*, Edx, const RemoteService*, int);
typedef void(__fastcall* SMConnId_t)(void*, Edx, uint32_t, int);
typedef void(__fastcall* SMSess_t)(void*, Edx, SessionInfo*);
typedef void(__fastcall* SMReject_t)(void*, Edx, const void*, uint8_t, int);
typedef void(__fastcall* SMHostDown_t)(void*, Edx, const void*);
typedef void(__fastcall* SMRelCb_t)(void*, Edx, const uint8_t*);
typedef int(__fastcall* SMNext_t)(void*, Edx, uint8_t*, uint32_t);
typedef void*(__fastcall* SMCtor_t)(void*, Edx, void*, int, int, int);

static __forceinline void dp_send(SessionMgr* m, void* pkt, int len, const void* addr) {
    fn<DPSend_t>(F_DataPort_Send)(&m->rdp, 0, pkt, len, addr);
}
static __forceinline void rdp_send_reliable(SessionMgr* m, const void* pkt, int len, const void* addr, const void* rpi) {
    fn<RDPSendRel_t>(F_RDP_SendReliable)(&m->rdp, 0, pkt, len, addr, rpi);
}

// every public function's first act: is this the task that made the manager? (ASSERT_MSG is a bare `ret` in v1.0)
static __declspec(noinline) void task_check(SessionMgr* self, uint32_t line, uint32_t file, uint32_t fmt) {
    char* const owner = fn<TaskName_t>(F_TaskGetName)(self->task);
    const int id = fn<IntV_t>(F_TaskGetID)();
    char* const cur = fn<TaskName_t>(F_TaskGetName)(id);
    const int id2 = fn<IntV_t>(F_TaskGetID)();
    fn<Assert_t>(F_SM_ASSERT_MSG)(id2 - self->task == 0 ? 1 : 0, S(fmt), S(file), line, cur, owner);
}

static const char* const R_IO = "socket I/O (sends or receives through the ReliableDataPort / ISocket)";
static const char* const R_LOG = "logs (LogReport / LogPanic)";
static const char* const R_HEAP = "allocates or frees";
static const char* const R_XL = "builds its function-local Xlators the first time (atexit), translates";

// =========================================================================================================================
// construction
// =========================================================================================================================
static void zero_dwords(void* p, uint32_t bytes) {                     // rep stosd: bytes >> 2 dwords
    volatile uint32_t* d = (volatile uint32_t*)p;
    for (uint32_t i = 0, n = bytes >> 2; i < n; i++) d[i] = 0;
}

static SessionMgr* __fastcall SM_ctor(SessionMgr* self, Edx, void* sock, int nsess, int nremote, int nlocal) {
    int pc = nsess * 2;
    if (!(pc > 0x10)) pc = 0x10;
    fn<PoolCtor_t>(F_PoolBase_ctor)(&self->data_pool, 0, S(S_POOL_NAME_SESS), pc, 0xf4);
    self->data_pool.vtable = (void**)(uintptr_t)VT_Pool_SessionData;
    int rn = nsess;
    if (!(rn > 4)) rn = 4;
    fn<RDPCtor_t>(F_RDP_ctor)(&self->rdp, 0, sock, rn);
    self->dropped = 0;
    fn<PoolU8_t>(F_PoolBase_OverallocCrashes)(&self->data_pool, 0, 0);
    self->task = fn<IntV_t>(F_TaskGetID)();
    self->sock = sock;
    self->password[0] = 0;
    self->request_type = 0;
    self->async = 0;
    self->data = 0;
    self->remote = 0;
    self->local = 0;
    self->sessions = 0;
    self->nsessions = nsess;
    self->status_at = -1;
    self->request_time = 0;
    self->nremote_max = nremote;
    self->nremote = 0;
    self->nlocal_max = nlocal;
    self->nlocal = 0;
    self->ok = 1;
    if (nsess) {
        SessionInfo* const p = (SessionInfo*)fn<MemAlloc_t>(F_MemAlloc)((int)((uint32_t)nsess * 0x1c));
        self->sessions = p;
        if (!p) self->ok = 0;
        else zero_dwords(p, (uint32_t)self->nsessions * 0x1c);
    }
    if (self->ok && self->nremote_max) {
        RemoteService* const p = (RemoteService*)fn<MemAlloc_t>(F_MemAlloc)((int)((uint32_t)self->nremote_max * 0x54));
        self->remote = p;
        if (!p) self->ok = 0;
        else zero_dwords(p, (uint32_t)self->nremote_max * 0x54);
    }
    if (self->ok && self->nlocal_max) {
        LocalService* const p = (LocalService*)fn<MemAlloc_t>(F_MemAlloc)((int)((uint32_t)self->nlocal_max * 0x48));
        self->local = p;
        if (!p) self->ok = 0;
        else zero_dwords(p, (uint32_t)self->nlocal_max * 0x48);
    }
    uint32_t gd = (uint32_t)(uintptr_t)self;                            // generic_data: the manager
    fn<SetCB_t>(F_RDP_SetTimeoutCB)(&self->rdp, 0, (void*)(uintptr_t)F_sess_host_timeout_cb, &gd);
    return self;
}
static void fp_sm_ctor(Footprint& f, SessionMgr*, Edx, void*, int, int, int) { f.replay_only = R_HEAP; }
PORT_FN(0x004a4d20, "SessionMgr::SessionMgr", SM_ctor, fp_sm_ctor)

// RPUserData: the manager, then the packet's kind, channel and (set here) result
static void __cdecl sess_rdp_cb(uint8_t result, uint8_t* ud) {
    ud[6] = result;
    fn<SMRelCb_t>(F_SM_reliable_cb)(*(void**)ud, 0, ud + 4);
}
static void fp_sess_rdp_cb(Footprint& f, uint8_t, uint8_t*) { f.replay_only = "moves a channel's state (a reliable packet's callback)"; }
PORT_FN(0x004a4ed0, "SessionMgr::sess_rdp_cb", sess_rdp_cb, fp_sess_rdp_cb)

static void __cdecl sess_host_timeout_cb(const void* addr, const void* gd) {
    fn<SMHostDown_t>(F_SM_hostdown_cb)(*(void* const*)gd, 0, addr);
}
static void fp_sess_host_timeout(Footprint& f, const void*, const void*) { f.replay_only = "takes a host's channels down"; }
PORT_FN(0x004a4ef0, "SessionMgr::sess_host_timeout_cb", sess_host_timeout_cb, fp_sess_host_timeout)

static void __fastcall SM_init_rpi(SessionMgr* self, Edx, RPI* rpi, uint8_t kind, uint8_t sess) {
    rpi->fn = (void*)(uintptr_t)F_sess_rdp_cb;
    memcpy(rpi->data, &self, 4);
    rpi->data[5] = sess;
    rpi->data[4] = kind;
}
static void fp_sm_init_rpi(Footprint& f, SessionMgr*, Edx, RPI* rpi, uint8_t, uint8_t) { f.add(rpi, 10, "the ReliablePacketInfo"); }
PORT_FN(0x004a4f10, "SessionMgr::init_rpi", SM_init_rpi, fp_sm_init_rpi)

static uint8_t __fastcall SM_Ok(SessionMgr* self, Edx) { return self->ok; }
static void fp_sm_ok(Footprint&, SessionMgr*, Edx) {}
PORT_FN(0x004a4f30, "SessionMgr::Ok", SM_Ok, fp_sm_ok)

static void __fastcall SM_dtor(SessionMgr* self, Edx) {
    if (self->sessions) { fn<Ptr_t>(F_OpDelete)(self->sessions); self->sessions = 0; }
    if (self->remote) { fn<Ptr_t>(F_OpDelete)(self->remote); self->remote = 0; }
    if (self->local) { fn<Ptr_t>(F_OpDelete)(self->local); self->local = 0; }
    fn<This_t>(F_PoolBase_freeall)(&self->data_pool, 0);
    fn<This_t>(F_stat_dump)(&self->dropped, 0);
    self->sock = 0;
    self->ok = 0;
    fn<This_t>(F_RDP_dtor)(&self->rdp, 0);
    fn<This_t>(F_PoolBase_dtor)(&self->data_pool, 0);
}
static void fp_sm_dtor(Footprint& f, SessionMgr*, Edx) { f.replay_only = R_HEAP; }
PORT_FN(0x004a4f40, "SessionMgr::~SessionMgr", SM_dtor, fp_sm_dtor)

static uint8_t __fastcall SM_CanDestroySafely(SessionMgr* self, Edx) {
    task_check(self, 0x133, 0x004fbc70, 0x004fbc7c);
    if (fn<U8V_t>(F_SM_Ok)(self, 0)) {
        const int n = self->nsessions;
        const SessionInfo* const base = self->sessions;
        for (int i = 0; i < n; i++)
            if (base[i].flags & 1) return 0;
    }
    return fn<U8V_t>(F_RDP_PacketsPending)(&self->rdp, 0) == 0 ? 1 : 0;
}
static void fp_sm_candestroy(Footprint&, SessionMgr*, Edx) {}
PORT_FN(0x004a4fc0, "SessionMgr::CanDestroySafely", SM_CanDestroySafely, fp_sm_candestroy)

static void __fastcall SM_AsyncCancel(SessionMgr* self, Edx, uint8_t* blk) {
    task_check(self, 0x18a, 0x004fbca0, 0x004fbcac);
    if (ld32(blk + ASRB_STATE) == 0) NETC_VFN(self->sock, IS_AsyncCancel, void, void*)(self->sock, 0, blk);
    self->async = 0;
}
static void fp_sm_asynccancel(Footprint& f, SessionMgr*, Edx, uint8_t*) { f.replay_only = R_IO; }
PORT_FN(0x004a5060, "SessionMgr::AsyncCancel", SM_AsyncCancel, fp_sm_asynccancel)

// =========================================================================================================================
// the tick: the link, the reliable port, the async lookup, stale services, the packets
// =========================================================================================================================
static void __fastcall SM_Tick(SessionMgr* self, Edx) {
    task_check(self, 0x19b, 0x004fbcd0, 0x004fbcdc);
    if (!sock_link_alive(self->sock)) {
        for (int i = 0; self->nsessions > i; i++) {
            if (chan(self, i)->flags & 1) {
                LOG_REPORT(S(0x004fbd00), i);
                chan(self, i)->flags = 0x84;
                chan(self, i)->reason = 6;
                fn<SMInt_t>(F_SM_toss_data)(self, 0, i);
            }
        }
    }
    NETC_VFN(&self->rdp, DP_Tick, void)(&self->rdp, 0);
    uint8_t* const blk = self->async;
    if (blk) {
        const int32_t state = (int32_t)ld32(blk + ASRB_STATE);
        if (state == 1) {                                               // the lookup failed
            str_copy(blk + ASRB_MSG, fn<Msg_t>(F_async_msg)(1));
            self->async = 0;
        } else if (state == 2) {                                        // found: ask it for its services
            memcpy(blk + ASRB_PORT, &self->async_port, 2);
            st32(self->async + ASRB_STATE, 3);
            const char* const m = fn<Msg_t>(F_async_msg)(3);
            str_copy(self->async + ASRB_MSG, m);
            uint8_t pkt[8];                                             // byte 1 left as the frame had it
            pkt[0] = 0;
            pkt[3] = 5;
            pkt[2] = 0x1e;
            uint8_t* const a = self->async;
            memcpy(pkt + 4, a + ASRB_TYPE, 4);
            RPI rpi;                                                    // its last 4 bytes left as the frame had them
            rpi.fn = (void*)(uintptr_t)F_servreq_cb;
            uint8_t** const pa = &self->async;
            memcpy(rpi.data, &pa, 4);
            rdp_send_reliable(self, pkt, 8, a + ASRB_ADDR, &rpi);
        }
    }
    const int t = (int)((uint32_t)net_PTimeNow() - 3000u);              // site 0x4a5264
    if (self->request_time && t > self->request_time) {
        int i = 0;
        uint32_t off = 0;
        while (self->nremote > i) {
            RemoteService* const rs = (RemoteService*)(B(self->remote) + off);
            if (rs->fresh == 0) {
                const int n = --self->nremote;
                crt_memmove(rs, B(rs) + 0x54, (uint32_t)(n - i) * 0x54);
            } else {
                off += 0x54;
                i++;
            }
        }
    }
    uint8_t buf[0xe4];
    SockAddr from;
    int len = 0xe2;
    while (sock_link_alive(self->sock)) {
        if (!fn<DPRecv_t>(F_DataPort_Recv)(&self->rdp, 0, buf, &len, &from)) break;
        fn<SMPkt3_t>(F_SM_dispatch)(self, 0, buf, len, &from);
        len = 0xe2;
    }
}
static void fp_sm_tick(Footprint& f, SessionMgr*, Edx) { f.replay_only = R_IO; }
PORT_FN(0x004a50d0, "SessionMgr::Tick", SM_Tick, fp_sm_tick)

// the async lookup's six texts (function-local Xlators, built the first time)
struct XlatorDef { uint32_t obj, key, dtor; };
static void build_xlators(uint32_t guard, const XlatorDef* x, int n) {
    volatile uint8_t* const g = (volatile uint8_t*)(uintptr_t)guard;
    uint8_t al = *g;
    for (int k = 0; k < n; k++) {
        const uint8_t bit = (uint8_t)(1u << k);
        if (!(al & bit)) {
            al |= bit;
            *g = al;
            fn<XCtor_t>(F_Xlator_ctor)((void*)(uintptr_t)x[k].obj, 0, S(x[k].key));
            fn<Atexit_t>(F_atexit)((void*)(uintptr_t)x[k].dtor);
            al = *g;
        }
    }
}
// each Xlator's text, translated again if the language changed (the cookie read once, and again after each xlate)
static void texts_of(const XlatorDef* x, int n, uint32_t* out) {
    uint32_t cookie = *(volatile uint32_t*)(uintptr_t)G_XLATOR_COOKIE;
    for (int k = 0; k < n; k++) {
        const uint32_t o = x[k].obj;
        if (*(volatile uint32_t*)(uintptr_t)(o + 8) != cookie) {
            fn<This_t>(F_Xlator_xlate)((void*)(uintptr_t)o, 0);
            if (k + 1 < n) cookie = *(volatile uint32_t*)(uintptr_t)G_XLATOR_COOKIE;
        }
        out[k] = *(volatile uint32_t*)(uintptr_t)(o + 4);
    }
}
static const XlatorDef k_async_x[6] = {
    {0x0057d368, 0x004fbd2c, 0x004a5550}, {0x0057d448, 0x004fbd44, 0x004a5540}, {0x0057d3c0, 0x004fbd5c, 0x004a5530},
    {0x0057d400, 0x004fbd70, 0x004a5520}, {0x0057d390, 0x004fbd88, 0x004a5510}, {0x0057d420, 0x004fbda0, 0x004a5500},
};
// FIX: a state outside 0..5 gives the first text (the original read past its table; its callers pass a lookup's state,
// which the sockets keep to 0..5)
static const char* __cdecl async_msg_c(int i) {
    build_xlators(G_ASYNC_GUARD, k_async_x, 6);
    uint32_t t[6];
    texts_of(k_async_x, 6, t);
    if (VP_FIX && (uint32_t)i >= 6u) {
        VP_FIX_HIT("async_msg");
        i = 0;
    }
    return (const char*)(uintptr_t)t[i];
}
static void fp_async_msg(Footprint& f, int) { f.replay_only = R_XL; }
PORT_FN(0x004a5320, "async_msg", async_msg_c, fp_async_msg)

static void __cdecl servreq_cb(uint8_t result, uint8_t* ud) {
    uint8_t** const pp = *(uint8_t***)ud;
    if (!*pp) return;
    st32(*pp + ASRB_STATE, (result & 1) ? 5u : 4u);
    uint8_t* const b = *pp;
    str_copy(b + ASRB_MSG, fn<Msg_t>(F_async_msg)((int)ld32(b + ASRB_STATE)));
    *pp = 0;
}
static void fp_servreq_cb(Footprint& f, uint8_t, uint8_t*) { f.replay_only = R_XL; }
PORT_FN(0x004a5560, "servreq_cb", servreq_cb, fp_servreq_cb)

static const RemoteService* __fastcall SM_GetServiceTable(SessionMgr* self, Edx) {
    task_check(self, 0x1ef, 0x004fbdb8, 0x004fbdc4);
    return self->remote;
}
static void fp_sm_getservicetable(Footprint&, SessionMgr*, Edx) {}
PORT_FN(0x004a55c0, "SessionMgr::GetServiceTable", SM_GetServiceTable, fp_sm_getservicetable)

// =========================================================================================================================
// finding services
// =========================================================================================================================
static void __fastcall SM_ServiceRequestBroadcast(SessionMgr* self, Edx, uint32_t type) {
    task_check(self, 0x1fc, 0x004fbde8, 0x004fbdf4);
    const int now = net_PTimeNow();                                      // site 0x4a5672
    if (self->request_time && (int)((uint32_t)now - (uint32_t)self->request_time) < 4000) {
        LOG_REPORT(S(0x004fbe18));
        return;
    }
    self->request_time = now;
    uint8_t pkt[8];                                                     // byte 1 left as the frame had it
    pkt[0] = 0;
    memcpy(pkt + 4, &type, 4);
    self->request_type = type;
    pkt[3] = 5;
    pkt[2] = 0x1e;
    SockAddr bc;
    sock_bcast_addr(self->sock, &bc);
    dp_send(self, pkt, 8, &bc);
    for (int i = 0; self->nremote > i;) {
        self->remote[i].fresh = 0;
        const void* const a = &self->remote[i].addr;
        i++;
        rdp_send_reliable(self, pkt, 8, a, 0);
    }
}
static void fp_sm_srb(Footprint& f, SessionMgr*, Edx, uint32_t) { f.replay_only = R_IO; }
PORT_FN(0x004a5620, "SessionMgr::ServiceRequestBroadcast", SM_ServiceRequestBroadcast, fp_sm_srb)

// (the address found isn't used: only the type is kept)
static void __fastcall SM_ServiceRequestByHostname(SessionMgr* self, Edx, const char* name, int16_t, uint32_t type) {
    task_check(self, 0x220, 0x004fbe4c, 0x004fbe58);
    SockAddr a;
    if (NETC_VFN(self->sock, IS_GetHostAddr, uint8_t, const char*, void*)(self->sock, 0, name, &a)) self->request_type = type;
    else LOG_REPORT(S(0x004fbe7c));
}
static void fp_sm_srh(Footprint& f, SessionMgr*, Edx, const char*, int16_t, uint32_t) { f.replay_only = R_IO; }
PORT_FN(0x004a5720, "SessionMgr::ServiceRequestByHostname", SM_ServiceRequestByHostname, fp_sm_srh)

static void __fastcall SM_AsyncServiceRequestByHostname(SessionMgr* self, Edx, const char* name, int16_t port, uint32_t type,
                                                        uint8_t* blk) {
    task_check(self, 0x23d, 0x004fbe8c, 0x004fbe98);
    st32(blk + ASRB_TYPE, type);
    self->request_type = type;
    if (NETC_VFN(self->sock, IS_AsyncGetHostAddr, uint8_t, const char*, void*)(self->sock, 0, name, blk)) {
        self->async = blk;
        self->async_port = port;
    } else {
        st32(blk + ASRB_STATE, 1);
    }
    str_copy(blk + ASRB_MSG, fn<Msg_t>(F_async_msg)((int)ld32(blk + ASRB_STATE)));
}
static void fp_sm_asrh(Footprint& f, SessionMgr*, Edx, const char*, int16_t, uint32_t, uint8_t*) { f.replay_only = R_IO; }
PORT_FN(0x004a57b0, "SessionMgr::AsyncServiceRequestByHostname", SM_AsyncServiceRequestByHostname, fp_sm_asrh)

// =========================================================================================================================
// offering services
// =========================================================================================================================
static uint32_t __fastcall SM_OfferUnboundService(SessionMgr* self, Edx, LocalService* ls, int n) {
    task_check(self, 0x253, 0x004fbebc, 0x004fbec8);
    if (fn<SMRange_t>(F_SM_find_range)(self, 0, n, ls)) return fn<SMOffer_t>(F_SM_OfferService)(self, 0, ls);
    return 0;
}
static void fp_sm_offer_unbound(Footprint& f, SessionMgr*, Edx, LocalService*, int) { f.replay_only = R_IO; }
PORT_FN(0x004a5870, "SessionMgr::OfferUnboundService", SM_OfferUnboundService, fp_sm_offer_unbound)

static uint32_t __fastcall SM_OfferService(SessionMgr* self, Edx, const LocalService* ls) {
    task_check(self, 0x261, 0x004fbeec, 0x004fbef8);
    if (!(self->nlocal_max > self->nlocal)) {
        LOG_PANIC(S(0x004fbf90), self->nlocal_max);
        return 0;
    }
    memcpy(&self->local[self->nlocal], ls, 0x48);
    const uint32_t id = fn<IntV_t>(F_unique_service_id)();
    self->local[self->nlocal].id = id;
    self->local[self->nlocal].refuse = 0;
    int s = ls->lo;
    if (ls->hi > s) {
        do {
            SessionInfo* const sd = chan(self, s);
            if (sd->flags & 0x19) {
                LOG_PANIC(S(0x004fbf1c), s, (uint32_t)chan(self, s)->flags);
                return 0;
            }
            if (sd->service != 0) {
                LOG_PANIC(S(0x004fbf50), s, (uint32_t)chan(self, s)->flags);
                return 0;
            }
            s++;
            sd->service = self->local[self->nlocal].id;
        } while (ls->hi > s);
    }
    fn<SMLs_t>(F_SM_broadcast_servinfo)(self, 0, &self->local[self->nlocal]);
    const int u = self->nlocal;
    self->nlocal = u + 1;
    return self->local[u].id;
}
static void fp_sm_offer(Footprint& f, SessionMgr*, Edx, const LocalService*) { f.replay_only = R_IO; }
PORT_FN(0x004a58f0, "SessionMgr::OfferService", SM_OfferService, fp_sm_offer)

static uint32_t __cdecl unique_service_id_c() {
    volatile uint32_t* const g = (volatile uint32_t*)(uintptr_t)G_SERVICE_ID;
    uint32_t v = *g;
    if (v == 0) v = (uint32_t)net_Random(10000000);                    // site 0x4a5a7e
    v++;
    *g = v;
    return v;
}
static void fp_unique_id(Footprint& f) { f.add((void*)(uintptr_t)G_SERVICE_ID, 4, "unique_service_id's counter"); }
PORT_FN(0x004a5a70, "unique_service_id", unique_service_id_c, fp_unique_id)

static void __fastcall SM_broadcast_servdown(SessionMgr* self, Edx, uint32_t id) {
    task_check(self, 0x28c, 0x004fbfcc, 0x004fbfd8);
    uint8_t pkt[8];                                                     // byte 1 left as the frame had it
    memcpy(pkt + 4, &id, 4);
    SockAddr bc;
    pkt[0] = 0;
    pkt[3] = 7;
    pkt[2] = 0x1e;
    sock_bcast_addr(self->sock, &bc);
    dp_send(self, pkt, 8, &bc);
}
static void fp_sm_bcast_down(Footprint& f, SessionMgr*, Edx, uint32_t) { f.replay_only = R_IO; }
PORT_FN(0x004a5a90, "SessionMgr::broadcast_servdown", SM_broadcast_servdown, fp_sm_bcast_down)

static void __fastcall SM_withdraw_service(SessionMgr* self, Edx, LocalService* ls) {
    for (int s = ls->lo; ls->hi > s; s++) chan(self, s)->service = 0;
    fn<SMU32_t>(F_SM_broadcast_servdown)(self, 0, ls->id);
}
static void fp_sm_withdraw(Footprint& f, SessionMgr*, Edx, LocalService*) { f.replay_only = R_IO; }
PORT_FN(0x004a5b30, "SessionMgr::withdraw_service", SM_withdraw_service, fp_sm_withdraw)

static int find_local(SessionMgr* self, uint32_t id) {                 // the count and the array read once
    const int n = self->nlocal;
    const LocalService* const l = self->local;
    for (int i = 0; i < n; i++)
        if (l[i].id == id) return i;
    return -1;
}

static void __fastcall SM_RefuseConnections(SessionMgr* self, Edx, uint32_t id) {
    task_check(self, 0x2ad, 0x004fbffc, 0x004fc008);
    LocalService* const l = self->local;
    const int i = find_local(self, id);
    if (i < 0) {
        LOG_PANIC(S(0x004fc02c));
        return;
    }
    l[i].refuse = 1;
    fn<SMU32_t>(F_SM_broadcast_servdown)(self, 0, self->local[i].id);
}
static void fp_sm_refuse(Footprint& f, SessionMgr*, Edx, uint32_t) { f.replay_only = R_IO; }
PORT_FN(0x004a5b80, "SessionMgr::RefuseConnections", SM_RefuseConnections, fp_sm_refuse)

static void __fastcall SM_AcceptConnections(SessionMgr* self, Edx, uint32_t id) {
    task_check(self, 0x2bf, 0x004fc064, 0x004fc070);
    LocalService* const l = self->local;
    const int i = find_local(self, id);
    if (i < 0) {
        LOG_PANIC(S(0x004fc094));
        return;
    }
    l[i].refuse = 0;
    fn<SMLs_t>(F_SM_broadcast_servinfo)(self, 0, &self->local[i]);
}
static void fp_sm_accept(Footprint& f, SessionMgr*, Edx, uint32_t) { f.replay_only = R_IO; }
PORT_FN(0x004a5c20, "SessionMgr::AcceptConnections", SM_AcceptConnections, fp_sm_accept)

static void __fastcall SM_WithdrawService(SessionMgr* self, Edx, uint32_t id) {
    task_check(self, 0x2d1, 0x004fc0cc, 0x004fc0d8);
    LocalService* const l = self->local;
    const int i = find_local(self, id);
    if (i < 0) {
        LOG_REPORT(S(0x004fc0fc), id);
        return;
    }
    fn<SMLs_t>(F_SM_withdraw_service)(self, 0, &l[i]);
    const int n = --self->nlocal;
    LocalService* const at = &self->local[i];
    crt_memmove(at, B(at) + 0x48, (uint32_t)(n - i) * 0x48);
}
static void fp_sm_withdraw_pub(Footprint& f, SessionMgr*, Edx, uint32_t) { f.replay_only = R_IO; }
PORT_FN(0x004a5cc0, "SessionMgr::WithdrawService", SM_WithdrawService, fp_sm_withdraw_pub)

// =========================================================================================================================
// connecting
// =========================================================================================================================
static void __fastcall SM_Connect_rs(SessionMgr* self, Edx, const RemoteService* rs, int s) {
    task_check(self, 0x2e8, 0x004fc134, 0x004fc140);
    chan(self, s)->flags = 0x10;
    chan(self, s)->remote = 0xff;
    memcpy(&chan(self, s)->addr, &rs->addr, 12);
    uint8_t pkt[0x19];                                                  // byte 1, the password's tail: as the frame had them
    memcpy(pkt + 4, &rs->id, 4);
    pkt[8] = (uint8_t)s;
    pkt[0] = 0;
    pkt[3] = 0;
    pkt[2] = 0x1e;
    str_copy(pkt + 9, self->password);
    RPI rpi;
    fn<InitRpi_t>(F_SM_init_rpi)(self, 0, &rpi, 0, (uint8_t)s);
    rdp_send_reliable(self, pkt, 0x19, &chan(self, s)->addr, &rpi);
}
static void fp_sm_connect_rs(Footprint& f, SessionMgr*, Edx, const RemoteService*, int) { f.replay_only = R_IO; }
PORT_FN(0x004a5d90, "SessionMgr::Connect(RemoteService)", SM_Connect_rs, fp_sm_connect_rs)

// a channel to one of this machine's own services (the host's player): a RemoteService made in the frame -- the service's
// first 0x30 bytes and this machine's address (the rest as the frame had it: Connect reads only the id and the address)
static void __fastcall SM_Connect_id(SessionMgr* self, Edx, uint32_t id, int s) {
    task_check(self, 0x308, 0x004fc164, 0x004fc170);
    const LocalService* const l = self->local;
    const int i = find_local(self, id);
    if (i < 0) {
        LOG_PANIC(S(0x004fc194));
        return;
    }
    RemoteService rs;
    memcpy(&rs, &l[i], 0x30);
    NETC_VFN(self->sock, IS_GetMyAddr, void, void*)(self->sock, 0, &rs.addr);
    fn<SMConnRs_t>(F_SM_Connect_rs)(self, 0, &rs, s);
}
static void fp_sm_connect_id(Footprint& f, SessionMgr*, Edx, uint32_t, int) { f.replay_only = R_IO; }
PORT_FN(0x004a5ea0, "SessionMgr::Connect(unsigned)", SM_Connect_id, fp_sm_connect_id)

static int __fastcall SM_UnboundConnect_id(SessionMgr* self, Edx, uint32_t id) {
    task_check(self, 0x325, 0x004fc1bc, 0x004fc1c8);
    const int s = fn<SMEmpty_t>(F_SM_find_empty_session)(self, 0);
    if (s != -1) fn<SMConnId_t>(F_SM_Connect_id)(self, 0, id, s);
    return s;
}
static void fp_sm_uconnect_id(Footprint& f, SessionMgr*, Edx, uint32_t) { f.replay_only = R_IO; }
PORT_FN(0x004a5f70, "SessionMgr::UnboundConnect(unsigned)", SM_UnboundConnect_id, fp_sm_uconnect_id)

static int __fastcall SM_UnboundConnect_rs(SessionMgr* self, Edx, const RemoteService* rs) {
    task_check(self, 0x333, 0x004fc1ec, 0x004fc1f8);
    const int s = fn<SMEmpty_t>(F_SM_find_empty_session)(self, 0);
    if (s != -1) fn<SMConnRs_t>(F_SM_Connect_rs)(self, 0, rs, s);
    return s;
}
static void fp_sm_uconnect_rs(Footprint& f, SessionMgr*, Edx, const RemoteService*) { f.replay_only = R_IO; }
PORT_FN(0x004a5fe0, "SessionMgr::UnboundConnect(RemoteService)", SM_UnboundConnect_rs, fp_sm_uconnect_rs)

// a channel's received packets, out of the global list, back to the pool
static void __fastcall SM_toss_data(SessionMgr* self, Edx, int s) {
    SessionData** pp = &self->data;
    while (*pp) {
        SessionData* const e = *pp;
        if (e->sess == s) {
            *pp = e->next;
            pool_free(&self->data_pool, e);
        } else {
            pp = &e->next;
        }
    }
    self->sessions[s].queue = 0;
}
static void fp_sm_toss(Footprint& f, SessionMgr*, Edx, int) { f.replay_only = "returns packets to the pool"; }
PORT_FN(0x004a6050, "SessionMgr::toss_data", SM_toss_data, fp_sm_toss)

static void __fastcall SM_Disconnect(SessionMgr* self, Edx, int s, int reason) {
    task_check(self, 0x357, 0x004fc21c, 0x004fc228);
    uint8_t* const fl = &chan(self, s)->flags;
    if (!(*fl & 1)) return;
    *fl = 8;
    uint8_t pkt[9];                                                     // byte 1 left as the frame had it
    pkt[0] = 0;
    pkt[3] = 3;
    pkt[2] = 0x1e;
    pkt[4] = chan(self, s)->remote;
    memcpy(pkt + 5, &reason, 4);
    RPI rpi;
    fn<InitRpi_t>(F_SM_init_rpi)(self, 0, &rpi, 3, (uint8_t)s);
    rdp_send_reliable(self, pkt, 9, &chan(self, s)->addr, &rpi);
    fn<SMInt_t>(F_SM_toss_data)(self, 0, s);
}
static void fp_sm_disconnect(Footprint& f, SessionMgr*, Edx, int, int) { f.replay_only = R_IO; }
PORT_FN(0x004a60a0, "SessionMgr::Disconnect", SM_Disconnect, fp_sm_disconnect)

static void __fastcall SM_DisconnectService(SessionMgr* self, Edx, uint32_t id, int reason) {
    task_check(self, 0x376, 0x004fc24c, 0x004fc258);
    const int i = find_local(self, id);
    if (i < 0) {
        LOG_REPORT(S(0x004fc27c));
        return;
    }
    const uint32_t off = (uint32_t)i * 0x48;
    int s = ((LocalService*)(B(self->local) + off))->lo;
    if (!(((LocalService*)(B(self->local) + off))->hi > s)) return;
    do {
        if (chan(self, s)->flags & 1) fn<SMInt2_t>(F_SM_Disconnect)(self, 0, s, reason);
        s++;
    } while (((LocalService*)(B(self->local) + off))->hi > s);
}
static void fp_sm_disc_service(Footprint& f, SessionMgr*, Edx, uint32_t, int) { f.replay_only = R_IO; }
PORT_FN(0x004a6180, "SessionMgr::DisconnectService", SM_DisconnectService, fp_sm_disc_service)

static void __fastcall SM_Shutdown(SessionMgr* self, Edx) {
    task_check(self, 0x38b, 0x004fc2b0, 0x004fc2bc);
    fn<SMInt_t>(F_SM_DisconnectAll)(self, 0, 1);
    for (int i = 0; self->nlocal > i; i++) fn<SMLs_t>(F_SM_withdraw_service)(self, 0, &self->local[i]);
    self->nlocal = 0;
}
static void fp_sm_shutdown(Footprint& f, SessionMgr*, Edx) { f.replay_only = R_IO; }
PORT_FN(0x004a6260, "SessionMgr::Shutdown", SM_Shutdown, fp_sm_shutdown)

static void __fastcall SM_DisconnectAll(SessionMgr* self, Edx, int reason) {
    task_check(self, 0x39a, 0x004fc2e0, 0x004fc2ec);
    for (int i = 0; self->nsessions > i; i++)
        if (chan(self, i)->flags & 1) fn<SMInt2_t>(F_SM_Disconnect)(self, 0, i, reason);
}
static void fp_sm_disc_all(Footprint& f, SessionMgr*, Edx, int) { f.replay_only = R_IO; }
PORT_FN(0x004a62f0, "SessionMgr::DisconnectAll", SM_DisconnectAll, fp_sm_disc_all)

// =========================================================================================================================
// the channels' packets
// =========================================================================================================================
static void __fastcall SM_Send(SessionMgr* self, Edx, int s, uint8_t* pkt, int len, uint8_t b0) {
    task_check(self, 0x3aa, 0x004fc310, 0x004fc31c);
    const SessionInfo* const sd = chan(self, s);
    if (!(sd->flags & 1)) {
        LOG_REPORT(S(0x004fc340), s);
        return;
    }
    pkt[1] = sd->remote;
    pkt[0] = b0;
    dp_send(self, pkt, len, &chan(self, s)->addr);
}
static void fp_sm_send(Footprint& f, SessionMgr*, Edx, int, uint8_t*, int, uint8_t) { f.replay_only = R_IO; }
PORT_FN(0x004a6370, "SessionMgr::Send", SM_Send, fp_sm_send)

static void __fastcall SM_SendReliable(SessionMgr* self, Edx, int s, uint8_t* pkt, int len, const RPI* rpi, uint8_t b0) {
    task_check(self, 0x3c9, 0x004fc364, 0x004fc370);
    const SessionInfo* const sd = chan(self, s);
    if (!(sd->flags & 1)) return;
    pkt[2] = sd->remote;
    pkt[0] = b0;
    rdp_send_reliable(self, pkt, len, &chan(self, s)->addr, rpi);
}
static void fp_sm_sendrel(Footprint& f, SessionMgr*, Edx, int, uint8_t*, int, const RPI*, uint8_t) { f.replay_only = R_IO; }
PORT_FN(0x004a6420, "SessionMgr::SendReliable", SM_SendReliable, fp_sm_sendrel)

static uint8_t __fastcall SM_Recv(SessionMgr* self, Edx, int s, void* buf, int* len) {
    task_check(self, 0x3e5, 0x004fc394, 0x004fc3a0);
    SessionInfo* const sd = &self->sessions[s];
    if (!(sd->flags & 1)) return 0;
    SessionData* const e = sd->queue;
    if (!e) return 0;
    sd->queue = e->snext;
    SessionData** pp = &self->data;
    if (*pp) {
        for (;;) {
            SessionData* const x = *pp;
            if (e == x) break;
            pp = &x->next;
            if (!*pp) break;
        }
    }
    if (VP_FIX && !*pp) VP_FIX_HIT("SessionMgr::Recv");                // FIX: not on the global list: nothing to unlink
    else *pp = (*pp)->next;                                             // (the original went through 0)
    memcpy(buf, e->data, (size_t)e->len);
    *len = e->len;
    pool_free(&self->data_pool, e);
    return 1;
}
static void fp_sm_recv(Footprint& f, SessionMgr*, Edx, int, void*, int*) { f.replay_only = "returns a packet to the pool"; }
PORT_FN(0x004a64c0, "SessionMgr::Recv", SM_Recv, fp_sm_recv)

static int __fastcall SM_UnboundRecv(SessionMgr* self, Edx, uint32_t svc, void* buf, int* len) {
    task_check(self, 0x40d, 0x004fc3c4, 0x004fc3d0);
    SessionData** pp = &self->data;
    if (!*pp) return -1;
    SessionInfo* const base = self->sessions;
    for (;;) {
        SessionData* const e = *pp;
        const SessionInfo* const sd = &base[e->sess];
        if (sd->service == svc && (sd->flags & 1)) break;
        if (!e->next) return -1;
        pp = &e->next;
    }
    SessionData* const e = *pp;
    base[e->sess].queue = e->snext;
    *pp = (*pp)->next;
    memcpy(buf, e->data, (size_t)e->len);
    *len = e->len;
    const int s = e->sess;
    pool_free(&self->data_pool, e);
    return s;
}
static void fp_sm_urecv(Footprint& f, SessionMgr*, Edx, uint32_t, void*, int*) { f.replay_only = "returns a packet to the pool"; }
PORT_FN(0x004a6590, "SessionMgr::UnboundRecv", SM_UnboundRecv, fp_sm_urecv)

static uint8_t __fastcall SM_ChannelIsConnected(SessionMgr* self, Edx, int s) {
    task_check(self, 0x42c, 0x004fc3f4, 0x004fc400);
    return (uint8_t)(self->sessions[s].flags & 1);
}
static void fp_sm_connected(Footprint&, SessionMgr*, Edx, int) {}
PORT_FN(0x004a6670, "SessionMgr::ChannelIsConnected", SM_ChannelIsConnected, fp_sm_connected)

static uint8_t __fastcall SM_GetStatus(SessionMgr* self, Edx, int s) {
    task_check(self, 0x43c, 0x004fc424, 0x004fc430);
    uint8_t* const fl = &self->sessions[s].flags;
    const uint8_t v = *fl;
    if (v & 0x18) return 0;
    *fl = (uint8_t)(v & 1);
    return v;
}
static void fp_sm_getstatus(Footprint& f, SessionMgr* self, Edx, int s) { f.add(&self->sessions[s].flags, 1, "the channel's flags"); }
PORT_FN(0x004a66e0, "SessionMgr::GetStatus", SM_GetStatus, fp_sm_getstatus)

// FIX: no channels finds none (-1, nothing written); the original divided by the channel count
static int __fastcall SM_GetNextStatus2(SessionMgr* self, Edx, uint8_t* out, uint32_t filter) {
    task_check(self, 0x452, 0x004fc454, 0x004fc460);
    const int n = self->nsessions;
    const int at = self->status_at;
    SessionInfo* const base = self->sessions;
    if (VP_FIX && n == 0) {
        VP_FIX_HIT("SessionMgr::GetNextStatus");
        return -1;
    }
    int i = (at + 1) % n;
    const int start = i;
    uint8_t found = 0;
    for (;;) {
        const SessionInfo* const sd = &base[i];
        if ((sd->flags & 0x80) && (filter == 0 || sd->service == filter)) {
            found = 1;
            break;
        }
        i = (i + 1) % n;
        if (i == start) break;
    }
    self->status_at = i;
    if (!found) return -1;
    const uint8_t f = base[i].flags;
    *out = f;
    if (f & 0x18) {
        *out = 0;
        return i;
    }
    self->sessions[i].flags &= 1;
    return i;
}
static void fp_sm_nextstatus2(Footprint& f, SessionMgr* self, Edx, uint8_t* out, uint32_t) {
    f.add(out, 1, "the status");
    f.add(&self->status_at, 4, "the round robin");
    f.add(self->sessions, (uint32_t)self->nsessions * sizeof(SessionInfo), "the channels");
}
PORT_FN(0x004a6760, "SessionMgr::GetNextStatus(status,service)", SM_GetNextStatus2, fp_sm_nextstatus2)

static int __fastcall SM_GetNextStatus(SessionMgr* self, Edx, uint8_t* out) {
    return fn<SMNext_t>(F_SM_GetNextStatus2)(self, 0, out, 0);
}
static void fp_sm_nextstatus(Footprint& f, SessionMgr* self, Edx, uint8_t* out) { fp_sm_nextstatus2(f, self, 0, out, 0); }
PORT_FN(0x004a6850, "SessionMgr::GetNextStatus(status)", SM_GetNextStatus, fp_sm_nextstatus)

// n free channels in a row (no flags, no service). FIX: a channel with flags set (in use, connecting, going down) is
// stepped past like one with a service; the original stopped there and looped forever (it neither advanced nor failed).
static uint8_t __fastcall SM_find_range(SessionMgr* self, Edx, int n, LocalService* ls) {
    int run = 0, i = 0;
    if (!(self->nsessions > 0)) return 0;
    const SessionInfo* const base = self->sessions;
    const volatile uint8_t* fl = &base[0].flags;                      // (volatile: the endless loop stays a loop)
    for (;;) {
        if (*fl != 0) {
            run = 0;
            if (VP_FIX) {
                VP_FIX_HIT("SessionMgr::find_range");
                fl += 0x1c;
                i++;
            }
        } else {
            fl += 0x1c;
            const int k = i;
            i++;
            if (base[k].service != 0) {
                run = 0;
            } else if (++run == n) {
                ls->lo = (int16_t)(uint16_t)((uint32_t)i - (uint32_t)n);
                ls->hi = (int16_t)i;
                return 1;
            }
        }
        if (!(i < *(volatile int32_t*)&self->nsessions)) return 0;
    }
}
static void fp_sm_find_range(Footprint& f, SessionMgr*, Edx, int, LocalService* ls) { f.add(&ls->lo, 4, "the service's range"); }
PORT_FN(0x004a6860, "SessionMgr::find_range", SM_find_range, fp_sm_find_range)

static int __fastcall SM_find_empty_session(SessionMgr* self, Edx) {
    const int n = self->nsessions;
    const SessionInfo* const base = self->sessions;
    for (int i = 0; i < n; i++)
        if (base[i].flags == 0 && base[i].service == 0) return i;
    return -1;
}
static void fp_sm_find_empty(Footprint&, SessionMgr*, Edx) {}
PORT_FN(0x004a68d0, "SessionMgr::find_empty_session", SM_find_empty_session, fp_sm_find_empty)

// =========================================================================================================================
// what arrives
// =========================================================================================================================
static void __fastcall SM_recv_goaway(SessionMgr* self, Edx, const uint8_t* p, const void* addr) {
    for (int i = 0; self->nsessions > i; i++) {
        SessionInfo* const sd = chan(self, i);
        if ((sd->flags & 1) && p[4] == sd->remote && addr_eq(&sd->addr, addr)) {
            LOG_REPORT(S(0x004fc484));
            fn<SMInt_t>(F_SM_toss_data)(self, 0, i);
            chan(self, i)->flags = 0x84;
            chan(self, i)->reason = 4;
            return;
        }
    }
}
static void fp_sm_goaway(Footprint& f, SessionMgr*, Edx, const uint8_t*, const void*) { f.replay_only = R_LOG; }
PORT_FN(0x004a6900, "SessionMgr::recv_goaway", SM_recv_goaway, fp_sm_goaway)

static void __fastcall SM_dispatch(SessionMgr* self, Edx, const uint8_t* pkt, int len, const void* addr) {
    const uint8_t b0 = pkt[0];
    if (G8(G_CHAN_MASK) & b0) {
        fn<SMPkt3_t>(F_SM_recv_chandata)(self, 0, pkt, len, addr);
        return;
    }
    if (pkt[2] != 0x1e) return;
    switch (pkt[3]) {
    case 0: fn<SMPkt2_t>(F_SM_recv_connreq)(self, 0, pkt, addr); return;
    case 1: fn<SMPkt2_t>(F_SM_recv_connreject)(self, 0, pkt, addr); return;
    case 2: fn<SMPkt2_t>(F_SM_recv_connaccept)(self, 0, pkt, addr); return;
    case 3: fn<SMPkt2_t>(F_SM_recv_disconreq)(self, 0, pkt, addr); return;
    case 4: fn<SMPkt2_t>(F_SM_recv_goaway)(self, 0, pkt, addr); return;
    case 5: fn<SMPkt2_t>(F_SM_recv_servreq)(self, 0, pkt, addr); return;
    case 6: fn<SMPkt2_t>(F_SM_recv_servinfo)(self, 0, pkt, addr); return;
    case 7: fn<SMPkt2_t>(F_SM_recv_servdown)(self, 0, pkt, addr); return;
    default: LOG_PANIC(S(0x004fc4ac), (uint32_t)b0); return;
    }
}
static void fp_sm_dispatch(Footprint& f, SessionMgr*, Edx, const uint8_t*, int, const void*) { f.replay_only = R_IO; }
PORT_FN(0x004a6990, "SessionMgr::dispatch", SM_dispatch, fp_sm_dispatch)

// the 0x80-byte name of an address, as the original formats it for its logs (the first byte from a string, the rest zero)
static __forceinline void name_buf(char* buf, uint32_t first) {
    buf[0] = (char)G8(first);
    memset(buf + 1, 0, 0x7f);
}

// FIX: a packet longer than SessionData's 0xe4 bytes (or a negative length) for a connected channel is dropped; the
// original copied it unchecked (Tick passes at most 0xe2)
static void __fastcall SM_recv_chandata(SessionMgr* self, Edx, const uint8_t* pkt, int len, const void* addr) {
    const uint8_t cls = (uint8_t)((pkt[0] >> 4) & G8(G_CHAN_MASK));
    const uint8_t sb = cls < 7 ? pkt[2] : pkt[1];
    const int s = sb;
    if (!(self->nsessions > s)) {
        LOG_REPORT(S(0x004fc508), s);
        return;
    }
    const uint32_t off = (uint32_t)s * 0x1c;
    uint8_t* const fl = B(self->sessions) + off + 0x19;
    if (*fl & 0x10) *fl = 0x81;                                         // connecting: data means connected
    SessionInfo* const sd = (SessionInfo*)(B(self->sessions) + off);
    const uint8_t f = sd->flags;
    if (f & 1) {
        if (!addr_eq(addr, &sd->addr)) {
            char buf[0x80];
            name_buf(buf, 0x004fc4e4);
            sock_make_str(self->sock, addr, buf);
            LOG_REPORT(S(0x004fc4e8), buf);
            return;
        }
        if (VP_FIX && (uint32_t)len > 0xe4u) {
            VP_FIX_HIT("SessionMgr::recv_chandata");
            return;
        }
        SessionData* const e = (SessionData*)fn<PoolAlloc_t>(F_PoolBase_alloc)(&self->data_pool, 0);
        if (!e) {
            self->dropped++;
            return;
        }
        e->sess = s;
        memcpy(e->data, pkt, (size_t)len);
        e->len = len;
        e->snext = 0;
        e->next = 0;
        SessionData** q = (SessionData**)(B(self->sessions) + off + 4);
        while (*q) q = &(*q)->snext;
        *q = e;
        SessionData** g = &self->data;
        while (*g) g = &(*g)->next;
        *g = e;
    } else if (!(f & 8)) {                                              // not ours: go away
        uint8_t r[5];                                                   // byte 1 left as the frame had it
        r[0] = 0;
        r[3] = 4;
        r[2] = 0x1e;
        r[4] = sb;
        dp_send(self, r, 5, addr);
    }
}
static void fp_sm_chandata(Footprint& f, SessionMgr*, Edx, const uint8_t*, int, const void*) { f.replay_only = R_IO; }
PORT_FN(0x004a6a90, "SessionMgr::recv_chandata", SM_recv_chandata, fp_sm_chandata)

static void __fastcall SM_recv_connreq(SessionMgr* self, Edx, const uint8_t* p, const void* addr) {
    int reason = 3;
    for (int i = 0; self->nlocal > i; i++) {
        LocalService* const ls = &self->local[i];
        if (ls->id != ld32(p + 4)) continue;
        reason = 4;
        if (ls->refuse) continue;
        reason = 2;
        if (ls->has_password && fn<Stricmp_t>(F_stricmp)((const char*)p + 9, self->local[i].password) != 0) {
            LOG_REPORT(S(0x004fc530), (const char*)p + 9, self->local[i].password);
            continue;
        }
        const LocalService* const l2 = &self->local[i];
        const int hi = l2->hi;
        for (int s = l2->lo; s < hi; s++) {
            if (self->sessions[s].flags == 0) {
                SessionInfo* const sd = &self->sessions[s];
                sd->flags = 0x10;
                memcpy(&sd->addr, addr, 12);
                sd->remote = p[8];
                fn<SMSess_t>(F_SM_send_connaccept)(self, 0, sd);
                return;
            }
        }
        reason = 5;
    }
    fn<SMReject_t>(F_SM_send_connreject)(self, 0, addr, p[8], reason);
}
static void fp_sm_connreq(Footprint& f, SessionMgr*, Edx, const uint8_t*, const void*) { f.replay_only = R_IO; }
PORT_FN(0x004a6c20, "SessionMgr::recv_connreq", SM_recv_connreq, fp_sm_connreq)

static void __fastcall SM_recv_connreject(SessionMgr* self, Edx, const uint8_t* p, const void* addr) {
    const uint32_t s = p[4];
    if (!(self->nsessions > (int)s)) {
        LOG_REPORT(S(0x004fc5a8), s);
        return;
    }
    SessionInfo* const sd = &self->sessions[s];
    if (addr_eq(addr, &sd->addr)) {
        const uint8_t f = sd->flags;
        if (f == 0x10 || (f & 2)) {
            sd->flags = 0x84;
            sd->reason = (int32_t)ld32(p + 5);
        } else {
            LOG_REPORT(S(0x004fc54c), (uint32_t)self->sessions[s].flags);
        }
    } else {
        char buf[0x80];
        name_buf(buf, 0x004fc57c);
        sock_make_str(self->sock, addr, buf);
        LOG_REPORT(S(0x004fc580), s, buf);
    }
}
static void fp_sm_connreject(Footprint& f, SessionMgr*, Edx, const uint8_t*, const void*) { f.replay_only = R_LOG; }
PORT_FN(0x004a6d60, "SessionMgr::recv_connreject", SM_recv_connreject, fp_sm_connreject)

static void __fastcall SM_recv_connaccept(SessionMgr* self, Edx, const uint8_t* p, const void* addr) {
    const uint32_t s = p[5];
    if (!(self->nsessions > (int)s)) {
        LOG_REPORT(S(0x004fc640), s);
        return;
    }
    SessionInfo* const sd = &self->sessions[s];
    if (addr_eq(addr, &sd->addr)) {
        const uint8_t f = sd->flags;
        if (f == 0x10 || (f & 2)) {
            sd->flags = 0x81;
            sd->remote = p[4];
        } else {
            LOG_REPORT(S(0x004fc5d4), (uint32_t)self->sessions[s].flags);
        }
    } else {
        char buf[0x80];
        name_buf(buf, 0x004fc604);
        const uint8_t* const a = (const uint8_t*)addr;
        uint16_t aw, sw;
        memcpy(&aw, a + 4, 2);
        memcpy(&sw, sd->addr.b + 4, 2);
        fn<Sprintf_t>(F_sprintf)(buf, S(0x004fc608), ld32(a), (uint32_t)aw, ld32(sd->addr.b), (uint32_t)sw);
        LOG_REPORT(S(0x004fc618), s, buf);
    }
}
static void fp_sm_connaccept(Footprint& f, SessionMgr*, Edx, const uint8_t*, const void*) { f.replay_only = R_LOG; }
PORT_FN(0x004a6e40, "SessionMgr::recv_connaccept", SM_recv_connaccept, fp_sm_connaccept)

static void __fastcall SM_recv_disconreq(SessionMgr* self, Edx, const uint8_t* p, const void* addr) {
    const uint32_t s = p[4];
    if (!(self->nsessions > (int)s)) {
        LOG_REPORT(S(0x004fc698), s);
        return;
    }
    SessionInfo* const sd = &self->sessions[s];
    if (addr_eq(addr, &sd->addr)) {
        fn<SMInt_t>(F_SM_toss_data)(self, 0, (int)s);
        sd->flags = 0x84;
        sd->reason = (int32_t)ld32(p + 5);
    } else {
        char buf[0x80];
        name_buf(buf, 0x004fc66c);
        sock_make_str(self->sock, addr, buf);
        LOG_REPORT(S(0x004fc670), s, buf);
    }
}
static void fp_sm_disconreq(Footprint& f, SessionMgr*, Edx, const uint8_t*, const void*) { f.replay_only = R_IO; }
PORT_FN(0x004a6f40, "SessionMgr::recv_disconreq", SM_recv_disconreq, fp_sm_disconreq)

// ServiceInfo (68 bytes): the control header, the service's first 48 bytes, its channel count, how many are connected,
// their round-trip latency's max, min and mean (byte 1 left as the frame had it)
static void __fastcall SM_make_servinfo_packet(SessionMgr* self, Edx, uint8_t* pkt, const LocalService* ls) {
    pkt[0] = 0;
    pkt[3] = 6;
    pkt[2] = 0x1e;
    memcpy(pkt + 4, ls, 0x30);
    uint16_t zero = 0;
    memcpy(pkt + 0x36, &zero, 2);
    int sum = 0, max = 0, cnt = 0, min = 123456789;
    for (int s = ls->lo; ls->hi > s; s++) {
        if (self->sessions[s].flags & 1) {
            uint16_t c;
            memcpy(&c, pkt + 0x36, 2);
            c++;
            memcpy(pkt + 0x36, &c, 2);
            const int lat = fn<Latency_t>(F_RDP_GetEstRTLatency)(&self->rdp, 0, &self->sessions[s].addr);
            if (lat > max) max = lat;
            if (lat < min) min = lat;
            sum = (int)((uint32_t)sum + (uint32_t)lat);
            cnt++;
        }
    }
    const uint16_t span = (uint16_t)((uint16_t)ls->hi - (uint16_t)ls->lo);
    memcpy(pkt + 0x34, &span, 2);
    if (cnt) {
        st32(pkt + 0x38, (uint32_t)max);
        st32(pkt + 0x3c, (uint32_t)min);
        st32(pkt + 0x40, (uint32_t)(sum / cnt));
    } else {
        st32(pkt + 0x38, 0);
        st32(pkt + 0x3c, 0);
        st32(pkt + 0x40, 0);
    }
}
static void fp_sm_make_servinfo(Footprint& f, SessionMgr*, Edx, uint8_t* pkt, const LocalService*) {
    f.add(pkt, 0x44, "the ServiceInfo packet");
}
PORT_FN(0x004a7000, "SessionMgr::make_servinfo_packet", SM_make_servinfo_packet, fp_sm_make_servinfo)

static void __fastcall SM_broadcast_servinfo(SessionMgr* self, Edx, const LocalService* ls) {
    uint8_t pkt[0x44];
    SockAddr bc;
    fn<SMMake_t>(F_SM_make_servinfo_packet)(self, 0, pkt, ls);
    sock_bcast_addr(self->sock, &bc);
    dp_send(self, pkt, 0x44, &bc);
}
static void fp_sm_bcast_info(Footprint& f, SessionMgr*, Edx, const LocalService*) { f.replay_only = R_IO; }
PORT_FN(0x004a7100, "SessionMgr::broadcast_servinfo", SM_broadcast_servinfo, fp_sm_bcast_info)

static void __fastcall SM_recv_servreq(SessionMgr* self, Edx, const uint8_t* p, const void* addr) {
    uint8_t pkt[0x44];
    for (int i = 0; self->nlocal > i; i++) {
        const LocalService* const ls = &self->local[i];
        if (ls->type == ld32(p + 4) && !ls->refuse) {
            fn<SMMake_t>(F_SM_make_servinfo_packet)(self, 0, pkt, ls);
            rdp_send_reliable(self, pkt, 0x44, addr, 0);
        }
    }
}
static void fp_sm_servreq(Footprint& f, SessionMgr*, Edx, const uint8_t*, const void*) { f.replay_only = R_IO; }
PORT_FN(0x004a7140, "SessionMgr::recv_servreq", SM_recv_servreq, fp_sm_servreq)

static void __fastcall SM_recv_servinfo(SessionMgr* self, Edx, const uint8_t* p, const void* addr) {
    const uint32_t want = self->request_type;
    const uint32_t type = ld32(p + 0x24);
    if (type != want) {
        if (want) LOG_REPORT(S(0x004fc6c4), type, want);
        return;
    }
    int i = 0;
    for (; self->nremote > i; i++) {
        RemoteService* const rs = &self->remote[i];
        if (rs->id == ld32(p + 0x2c) && addr_eq(addr, &rs->addr)) break;
    }
    if (!(self->nremote_max > i)) return;
    const uint32_t off = (uint32_t)i * 0x54;
    memcpy(B(self->remote) + off, p + 4, 0x40);
    ((RemoteService*)(B(self->remote) + off))->fresh = 1;
    const int lat = fn<Latency_t>(F_RDP_GetEstRTLatency)(&self->rdp, 0, addr);
    ((RemoteService*)(B(self->remote) + off))->latency = lat;
    if (self->nremote == i) {
        memcpy(&((RemoteService*)(B(self->remote) + off))->addr, addr, 12);
        self->nremote++;
    }
}
static void fp_sm_servinfo(Footprint& f, SessionMgr*, Edx, const uint8_t*, const void*) { f.replay_only = "adds or refreshes a service found (the host table)"; }
PORT_FN(0x004a71a0, "SessionMgr::recv_servinfo", SM_recv_servinfo, fp_sm_servinfo)

static void __fastcall SM_recv_servdown(SessionMgr* self, Edx, const uint8_t* p, const void* addr) {
    for (int i = 0; self->nremote > i; i++) {
        RemoteService* const rs = &self->remote[i];
        if (rs->id != ld32(p + 4)) continue;
        if (addr_eq(addr, &rs->addr)) {
            const int n = --self->nremote;
            RemoteService* const at = &self->remote[i];
            crt_memmove(at, B(at) + 0x54, (uint32_t)(n - i) * 0x54);
            return;
        }
        LOG_REPORT(S(0x004fc6f4));
    }
}
static void fp_sm_servdown(Footprint& f, SessionMgr*, Edx, const uint8_t*, const void*) { f.replay_only = R_LOG; }
PORT_FN(0x004a7290, "SessionMgr::recv_servdown", SM_recv_servdown, fp_sm_servdown)

static void __fastcall SM_hostdown_cb(SessionMgr* self, Edx, const void* addr) {
    for (int i = 0; self->nsessions > i; i++) {
        SessionInfo* const sd = chan(self, i);
        if (sd->flags != 0 && addr_eq(&sd->addr, addr)) {
            chan(self, i)->flags = 0x82;
            fn<SMInt_t>(F_SM_toss_data)(self, 0, i);
        }
    }
    int i = 0;
    uint32_t off = 0;
    while (self->nremote > i) {
        RemoteService* const rs = (RemoteService*)(B(self->remote) + off);
        if (addr_eq(&rs->addr, addr)) {
            const int n = --self->nremote;
            RemoteService* const at = (RemoteService*)(B(self->remote) + off);
            crt_memmove(at, B(at) + 0x54, (uint32_t)(n - i) * 0x54);
        } else {
            off += 0x54;
            i++;
        }
    }
}
static void fp_sm_hostdown(Footprint& f, SessionMgr*, Edx, const void*) { f.replay_only = "returns packets to the pool"; }
PORT_FN(0x004a7320, "SessionMgr::hostdown_cb", SM_hostdown_cb, fp_sm_hostdown)

// a reliable session packet's fate: d[0] its kind (init_rpi's), d[1] the channel, d[2] the result (bit 0: acked)
static void __fastcall SM_reliable_cb(SessionMgr* self, Edx, const uint8_t* d) {
    const uint8_t s = d[1];
    const uint8_t ok = d[2] & 1;
    const uint32_t kind = d[0];
    switch (kind) {
    case 0:                                                             // the connect request
        if (!ok) self->sessions[s].flags = 0x82;
        return;
    case 2: {                                                           // the accept
        if (ok) {
            uint8_t* const fl = &self->sessions[s].flags;
            if (!(*fl & 1)) *fl = 0x81;
        } else {
            self->sessions[s].flags = 0;
        }
        return;
    }
    case 3: {                                                           // the disconnect request
        if (ok) {
            uint8_t* const fl = &self->sessions[s].flags;
            if (*fl & 9) *fl = 0x80;
        } else {
            self->sessions[s].flags = 0;
        }
        return;
    }
    case 1:
    case 5:
    case 6:
        return;
    default:                                                            // 4, and past 6
        LOG_PANIC(S(0x004fc70c), kind);
        return;
    }
}
static void fp_sm_reliable_cb(Footprint& f, SessionMgr*, Edx, const uint8_t*) { f.replay_only = "moves a channel's state (logs a bad kind)"; }
PORT_FN(0x004a73d0, "SessionMgr::reliable_cb", SM_reliable_cb, fp_sm_reliable_cb)

static void __fastcall SM_send_connaccept(SessionMgr* self, Edx, SessionInfo* sd) {
    uint8_t pkt[6];                                                     // byte 1 left as the frame had it
    pkt[0] = 0;
    pkt[3] = 2;
    pkt[2] = 0x1e;
    const int idx = (int)(B(sd) - B(self->sessions)) / 0x1c;
    pkt[5] = sd->remote;                                                // the asker's channel
    pkt[4] = (uint8_t)idx;                                              // ours
    RPI rpi;
    fn<InitRpi_t>(F_SM_init_rpi)(self, 0, &rpi, 2, (uint8_t)idx);
    rdp_send_reliable(self, pkt, 6, &sd->addr, &rpi);
}
static void fp_sm_send_accept(Footprint& f, SessionMgr*, Edx, SessionInfo*) { f.replay_only = R_IO; }
PORT_FN(0x004a74c0, "SessionMgr::send_connaccept", SM_send_connaccept, fp_sm_send_accept)

static void __fastcall SM_send_connreject(SessionMgr* self, Edx, const void* addr, uint8_t s, int reason) {
    uint8_t pkt[9];                                                     // byte 1 left as the frame had it
    memcpy(pkt + 5, &reason, 4);
    pkt[4] = s;
    pkt[0] = 0;
    pkt[3] = 1;
    pkt[2] = 0x1e;
    rdp_send_reliable(self, pkt, 9, addr, 0);
}
static void fp_sm_send_reject(Footprint& f, SessionMgr*, Edx, const void*, uint8_t, int) { f.replay_only = R_IO; }
PORT_FN(0x004a7530, "SessionMgr::send_connreject", SM_send_connreject, fp_sm_send_reject)

static void __fastcall SM_SetConnectPassword(SessionMgr* self, Edx, const char* pw) {
    task_check(self, 0x6e4, 0x004fc744, 0x004fc750);
    fn<Strncpy_t>(F_strncpy)(self->password, pw, 0x10);
    self->password[15] = 0;
}
static void fp_sm_setpw(Footprint& f, SessionMgr* self, Edx, const char*) { f.add(self->password, 0x10, "the connect password"); }
PORT_FN(0x004a7570, "SessionMgr::SetConnectPassword", SM_SetConnectPassword, fp_sm_setpw)

static int __fastcall SM_GetDiscReason(SessionMgr* self, Edx, int s) {
    task_check(self, 0x6eb, 0x004fc774, 0x004fc780);
    return self->sessions[s].reason;
}
static void fp_sm_discreason(Footprint&, SessionMgr*, Edx, int) {}
PORT_FN(0x004a75e0, "SessionMgr::GetDiscReason", SM_GetDiscReason, fp_sm_discreason)

static void __fastcall stat_dump(int32_t* self, Edx) {
    if (*self) LOG_REPORT(S(0x004fc7a4), *self);
}
static void fp_stat_dump(Footprint& f, int32_t*, Edx) { f.replay_only = R_LOG; }
PORT_FN(0x004a7650, "SessionMgr::stat_tag::dump", stat_dump, fp_stat_dump)

static SessionMgr* __cdecl CreateSessionMgr_c(void* sock, int nsess, int nremote, int nlocal) {
    SessionMgr* m = 0;
    void* const p = fn<MemAlloc_t>(F_MemAlloc)(0xd8);
    if (p) m = (SessionMgr*)fn<SMCtor_t>(F_SM_ctor)(p, 0, sock, nsess, nremote, nlocal);
    if (m && !fn<U8V_t>(F_SM_Ok)(m, 0)) {
        fn<This_t>(F_SM_dtor)(m, 0);
        fn<Ptr_t>(F_OpDelete)(m);
        m = 0;
    }
    return m;
}
static void fp_create_sm(Footprint& f, void*, int, int, int) { f.replay_only = R_HEAP; }
PORT_FN(0x004a7670, "CreateSessionMgr", CreateSessionMgr_c, fp_create_sm)

static const XlatorDef k_disc_x[7] = {
    {0x0057d410, 0x004fc7d8, 0x004a7980}, {0x0057d350, 0x004fc7f4, 0x004a7970}, {0x0057d3b0, 0x004fc814, 0x004a7960},
    {0x0057d380, 0x004fc834, 0x004a7950}, {0x0057d3d0, 0x004fc854, 0x004a7940}, {0x0057d3e0, 0x004fc874, 0x004a7930},
    {0x0057d3f0, 0x004fc890, 0x004a7920},
};
static const char* __cdecl SessionDiscReasonString_c(int r) {
    build_xlators(G_DISC_GUARD, k_disc_x, 7);
    if (r >= 7 || r < 0) return 0;
    uint32_t t[7];
    texts_of(k_disc_x, 7, t);
    return (const char*)(uintptr_t)t[r];
}
static void fp_disc_string(Footprint& f, int) { f.replay_only = R_XL; }
PORT_FN(0x004a76d0, "SessionDiscReasonString", SessionDiscReasonString_c, fp_disc_string)

}  // namespace net_session
}  // namespace
