// net_core.cpp -- multiplayer stage N1, group B: the reliability layer, rewritten faithfully (library `multi`:
// delqueue.obj, datamod.obj, dataport.obj, hostent.obj, nettypes.obj; session.obj is net_session.cpp).
//
//   delqueue.obj: DelayQueue -- a pool of entries in a doubly linked list, each due PTimeNow + MultiGetPacketDelay after it
//     was queued (the "simulated latency" of a network game's packets);
//   datamod.obj: DataModerator -- a DelayQueue of whole packets (0xe4 bytes, their length, their address): Send queues,
//     Tick sends what's due through the ISocket;
//   dataport.obj: DataPort (an ISocket's packets, byte 0's high nibble the port's class: 7 unreliable) and ReliableDataPort
//     (classes 0..6: acks, reliable data -- 2 / 4 a first send, 3 / 5 a resend, 4 / 5 starting a stream --, its HostEntries
//     per peer, the packets received in order handed out by _recv);
//   hostent.obj: HostEntry -- one peer's reliable queues: sent (awaiting their ack, resent after 1.5 round trips, at least
//     250 ms, 5 times, then the host is down), waiting (at most 2 unacked), held (received out of order); the round trip
//     time measured on acks; an ack for every packet;
//   nettypes.obj: the car packet's wire form (CarNetPacket, 23 bytes) to and from CarPhysicsPacket (phys_netcar.cpp's):
//     the time as 16 bits of PTime, the orientation as an axis-angle in three 16-bit fixed-point numbers, rpm, gear,
//     steering and the flags in three bytes; screw_up (a position through the same quantisation).
//
// Written from the v1.0 disassembly: every call in the original's order, by its v1.0 address (this group's own functions
// too, so the hooked rewrite or the original is what runs), virtual calls through the vtable; the multiplayer code's
// clock as N0 patched it (net_PTimeNow, net_wsock.h: PTimeNow at the sites 0x4a40c4, 0x4a4136 (DelayQueue), 0x4a4740
// (assumable_t), 0x4b027e, 0x4b0336, 0x4b0650, 0x4b06c3, 0x4b073c (HostEntry)). The packets built in the frame
// (HostEntry::ack's 2 bytes, DataModerator's 0xf4-byte records) store exactly what the original stores: a record's bytes
// between the packet's end and its length are left as the frame had them (never sent). x87 (nettypes.obj): register
// values as double, stored ones as float, the original's grouping and constants' widths; fsin / fcos / CIacos results
// handled in small asm helpers that do exactly what the original does with them; __ftol as x87_ftol.
//
// Footprints: the pure helpers (normalize_id, screw_up, rot_convert(Quat), frame_convert(QFrame)) pure with their outputs
// (the NetAxisAngle converters write the CRT's math context and errno through CIacos); whatever sends, receives,
// allocates or frees (the pools), or logs is replay_only -- a session replay of a network game is their in-game check
// (docs/PORTING.md; test/world_net_core.cpp checks them all offline).
//
// Fixes (docs/FIXES.md, "Multiplayer"; each marked `// FIX:` in place, `VP_FIX &&`, so the faithful build is the original):
// DataModerator::Send drops a packet longer than its 0xe4-byte record; HostEntry::init_rpp copies at most 0xe4 bytes;
// ReliableDataPort::get_entry, with the host pool used up, reuses an idle host's entry for the new one, or finds none (the
// pool panicked); handle_packet / SendReliable then drop the packet (RecvAck / HoldIncomingPacket / HostEntry::
// SendReliable ran on 0); return_slot ignores a slot outside 0..7
// (get_slot's -1: it wrote the byte before the slots); HostEntry::Tick and GetEstRTLatency don't divide by a zero rtt_n;
// ReliableDataPort::Tick takes a host that went down out of the list by its real successor (host_is_down overwrites its
// link with the dead list's, so the hosts after it were skipped that tick and then lost for good: a peer dropping out
// stranded the others' reliable packets).
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "port.h"
#include "x87.h"
#include "net_core.h"
#include "net_wsock.h"

// a harness's marker for "a fix changed what happens here" (nothing in the DLL)
#ifndef VP_FIX_HIT
#define VP_FIX_HIT(what) ((void)0)
#endif

namespace {
namespace net_core {
using namespace netc;

#define D(x) ((double)(x))
static __forceinline float FB(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
template <typename T> static __forceinline T fn(uint32_t a) { return (T)(uintptr_t)a; }
static __forceinline uint8_t G8(uint32_t a) { return *(volatile uint8_t*)(uintptr_t)a; }

typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* Ptr_t)(void*);
typedef void*(__fastcall* PoolCtor_t)(void*, Edx, const char*, int, uint32_t);
typedef void(__fastcall* PoolU8_t)(void*, Edx, uint8_t);
typedef void*(__fastcall* PoolAlloc_t)(void*, Edx);
typedef void(__fastcall* PoolFree_t)(void*, Edx, void*);
typedef void(__fastcall* This_t)(void*, Edx);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef void(__cdecl* Void_t)();
typedef int(__cdecl* IntV_t)();
typedef void(__cdecl* RpiCb_t)(uint8_t, void*);
typedef void(__cdecl* TimeoutCb_t)(const void*, const void*);

static __forceinline void* pool_alloc(PoolBase* p) { return fn<PoolAlloc_t>(F_PoolBase_alloc)(p, 0); }
static __forceinline void pool_free(PoolBase* p, void* e) { fn<PoolFree_t>(F_PoolBase_free)(p, 0, e); }
#define LOG_REPORT fn<Log_t>(F_LogReport)
#define FOOD fn<Log_t>(F_FOOD)

// footprint helpers
static const char* const R_QUEUE = "the network's queues: pool entries taken and returned, the clock read";
static const char* const R_IO = "socket I/O (sends or receives through the ISocket)";
static const char* const R_HEAP = "allocates or frees";
static const char* const R_LOG = "logs (LogReport)";

// =========================================================================================================================
// delqueue.obj: DelayQueue
// =========================================================================================================================
static DelayQueue* __fastcall DelayQueue_ctor(DelayQueue* self, Edx, int count, int size) {
    self->size = size;
    self->head = 0;
    self->tail = 0;
    void* p = fn<MemAlloc_t>(F_MemAlloc)(0x20);
    if (p) self->pool = (PoolBase*)fn<PoolCtor_t>(F_PoolBase_ctor)(p, 0, (const char*)(uintptr_t)S_POOL_NAME_DQ, count, size + 0x14);
    else self->pool = 0;
    if (self->pool) fn<PoolU8_t>(F_PoolBase_OverallocCrashes)(self->pool, 0, 0);
    return self;
}
static void fp_dq_ctor(Footprint& f, DelayQueue*, Edx, int, int) { f.replay_only = R_HEAP; }
PORT_FN(0x004a4000, "DelayQueue::DelayQueue", DelayQueue_ctor, fp_dq_ctor)

static void __fastcall DelayQueue_dtor(DelayQueue* self, Edx) {
    if (!self->pool) return;
    fn<This_t>(F_PoolBase_freeall)(self->pool, 0);
    if (self->pool) NETC_VFN(self->pool, 0, void*, uint32_t)(self->pool, 0, 1);   // its deleting destructor
    self->pool = 0;
}
static void fp_dq_dtor(Footprint& f, DelayQueue*, Edx) { f.replay_only = R_HEAP; }
PORT_FN(0x004a4060, "DelayQueue::~DelayQueue", DelayQueue_dtor, fp_dq_dtor)

static uint8_t __fastcall DelayQueue_Enqueue(DelayQueue* self, Edx, const void* data, int len, uint32_t tag) {
    fn<Void_t>(F_advance_latency_idx)();
    DQEntry* e = (DQEntry*)pool_alloc(self->pool);
    if (!e) return 0;
    memcpy(e->data, data, (size_t)len);                     // rep movsd / movsb
    e->len = len;
    const int t = net_PTimeNow();                            // site 0x4a40c4
    const int d = fn<IntV_t>(F_MultiGetPacketDelay)();
    e->due = (int)((uint32_t)t + (uint32_t)d);
    e->tag = tag;
    if (self->head) self->head->prev = e;
    e->next = self->head;
    e->prev = 0;
    DQEntry* const tail = self->tail;
    self->head = e;
    if (!tail) self->tail = e;
    return 1;
}
static void fp_dq_enqueue(Footprint& f, DelayQueue*, Edx, const void*, int, uint32_t) { f.replay_only = R_QUEUE; }
PORT_FN(0x004a4090, "DelayQueue::Enqueue", DelayQueue_Enqueue, fp_dq_enqueue)

static uint8_t __fastcall DelayQueue_Dequeue(DelayQueue* self, Edx, void* buf, int* len, uint32_t* tag) {
    const int now = net_PTimeNow();                          // site 0x4a4136
    DQEntry* e = self->tail;
    if (!e || e->due > now) return 0;
    const int n = e->len;
    const uint8_t ok = *len >= n;
    if (ok) {
        *len = n;
        memcpy(buf, self->tail->data, (size_t)n);
        if (tag) *tag = self->tail->tag;
    }
    e = self->tail;
    if (self->head == e) self->head = 0;
    self->tail = e->prev;
    pool_free(self->pool, e);
    return ok;
}
static void fp_dq_dequeue(Footprint& f, DelayQueue*, Edx, void*, int*, uint32_t*) { f.replay_only = R_QUEUE; }
PORT_FN(0x004a4130, "DelayQueue::Dequeue", DelayQueue_Dequeue, fp_dq_dequeue)

// =========================================================================================================================
// datamod.obj: DataModerator
// =========================================================================================================================
typedef DelayQueue*(__fastcall* DQCtor_t)(void*, Edx, int, int);
typedef uint8_t(__fastcall* DQEnq_t)(void*, Edx, const void*, int, uint32_t);
typedef uint8_t(__fastcall* DQDeq_t)(void*, Edx, void*, int*, uint32_t*);

static DataModerator* __fastcall DataModerator_ctor(DataModerator* self, Edx, void* sock) {
    fn<DQCtor_t>(F_DelayQueue_ctor)(&self->q, 0, 0xf1, 0xf4);
    self->sock = sock;
    return self;
}
static void fp_dm_ctor(Footprint& f, DataModerator*, Edx, void*) { f.replay_only = R_HEAP; }
PORT_FN(0x004afc70, "DataModerator::DataModerator", DataModerator_ctor, fp_dm_ctor)

static void __fastcall DataModerator_dtor(DataModerator* self, Edx) {
    self->sock = 0;
    fn<This_t>(F_DelayQueue_dtor)(&self->q, 0);              // (a tail jump)
}
static void fp_dm_dtor(Footprint& f, DataModerator*, Edx) { f.replay_only = R_HEAP; }
PORT_FN(0x004afc90, "DataModerator::~DataModerator", DataModerator_dtor, fp_dm_dtor)

static uint8_t __fastcall DataModerator_Ok(DataModerator* self, Edx) { return self->sock && self->q.pool ? 1 : 0; }
static void fp_dm_ok(Footprint&, DataModerator*, Edx) {}
PORT_FN(0x004afca0, "DataModerator::Ok", DataModerator_Ok, fp_dm_ok)

// the record queued: the packet (0xe4 bytes), its length, its address. FIX: a length outside 0..0xe4 (the original copied
// it into the record regardless, overrunning the frame past 0xf4) drops the packet.
static void __fastcall DataModerator_Send(DataModerator* self, Edx, const void* pkt, int len, const SockAddr* addr) {
    uint8_t rec[0xf4];                                       // bytes len..0xe3 left as the frame had them
    if (VP_FIX && (uint32_t)len > 0xe4u) {
        VP_FIX_HIT("DataModerator::Send");
        return;
    }
    memcpy(rec, pkt, (size_t)len);
    memcpy(rec + 0xe4, &len, 4);
    memcpy(rec + 0xe8, addr, 12);
    fn<DQEnq_t>(F_DelayQueue_Enqueue)(&self->q, 0, rec, 0xf4, 0);
}
static void fp_dm_send(Footprint& f, DataModerator*, Edx, const void*, int, const SockAddr*) { f.replay_only = R_QUEUE; }
PORT_FN(0x004afcc0, "DataModerator::Send", DataModerator_Send, fp_dm_send)

static void __fastcall DataModerator_Tick(DataModerator* self, Edx) {
    uint8_t rec[0xf4];
    int len = 0xf4;
    while (fn<DQDeq_t>(F_DelayQueue_Dequeue)(&self->q, 0, rec, &len, 0)) {
        int n;
        memcpy(&n, rec + 0xe4, 4);
        NETC_VFN(self->sock, IS_Send, void, const void*, int, const void*)(self->sock, 0, rec, n, rec + 0xe8);
        len = 0xf4;
    }
}
static void fp_dm_tick(Footprint& f, DataModerator*, Edx) { f.replay_only = R_IO; }
PORT_FN(0x004afd30, "DataModerator::Tick", DataModerator_Tick, fp_dm_tick)

// =========================================================================================================================
// dataport.obj: DataPort
// =========================================================================================================================
typedef void(__fastcall* Send_t)(void*, Edx, const void*, int, const void*);
typedef uint8_t(__fastcall* Recv_t)(void*, Edx, void*, int*, void*);
typedef uint8_t(__fastcall* Handle_t)(void*, Edx, void*, int, void*);

static DataPort* __fastcall DataPort_ctor(DataPort* self, Edx, void* sock) {
    self->vtable = (void**)(uintptr_t)VT_DataPort;
    self->mod = 0;
    self->sock = sock;
    return self;
}
static void fp_dp_ctor(Footprint& f, DataPort* self, Edx, void*) { f.add(self, sizeof(DataPort), "the DataPort (constructed)"); }
PORT_FN(0x004aef60, "DataPort::DataPort", DataPort_ctor, fp_dp_ctor)

static void __fastcall DataPort_dtor(DataPort* self, Edx) {
    DataModerator* const mod = self->mod;
    self->vtable = (void**)(uintptr_t)VT_DataPort;
    if (mod) {
        fn<This_t>(F_DataModerator_dtor)(mod, 0);
        fn<Ptr_t>(F_OpDelete)(mod);
        self->mod = 0;
    }
    if (self->sock) {
        NETC_VFN(self->sock, IS_dtor, void*, uint32_t)(self->sock, 0, 1);   // the ISocket's deleting destructor
        self->sock = 0;
    }
}
static void fp_dp_dtor(Footprint& f, DataPort*, Edx) { f.replay_only = R_HEAP; }
PORT_FN(0x004aef90, "DataPort::~DataPort", DataPort_dtor, fp_dp_dtor)

static void __fastcall DataPort__send(DataPort* self, Edx, const void* pkt, int len, const SockAddr* addr) {
    NETC_VFN(self->sock, IS_Send, void, const void*, int, const void*)(self->sock, 0, pkt, len, addr);
}
static void fp_dp__send(Footprint& f, DataPort*, Edx, const void*, int, const SockAddr*) { f.replay_only = R_IO; }
PORT_FN(0x004aefd0, "DataPort::_send", DataPort__send, fp_dp__send)

static uint8_t __fastcall DataPort__recv(DataPort* self, Edx, void* pkt, int* len, SockAddr* addr) {
    return NETC_VFN(self->sock, IS_Recv, uint8_t, void*, int*, void*)(self->sock, 0, pkt, len, addr);
}
static void fp_dp__recv(Footprint& f, DataPort*, Edx, void*, int*, SockAddr*) { f.replay_only = R_IO; }
PORT_FN(0x004aeff0, "DataPort::_recv", DataPort__recv, fp_dp__recv)

static void __fastcall DataPort_Send(DataPort* self, Edx, uint8_t* pkt, int len, const SockAddr* addr) {
    pkt[0] = (uint8_t)((pkt[0] & G8(G_CLASS_MASK_DP)) | 0x70);
    fn<Send_t>(F_DataPort__send)(self, 0, pkt, len, addr);
}
static void fp_dp_send(Footprint& f, DataPort*, Edx, uint8_t*, int, const SockAddr*) { f.replay_only = R_IO; }
PORT_FN(0x004af010, "DataPort::Send", DataPort_Send, fp_dp_send)

static uint8_t __fastcall DataPort_handle_packet(DataPort*, Edx, uint8_t* pkt, int, SockAddr*) {
    const uint8_t b = pkt[0];
    if (((uint32_t)(b >> 4) & G8(G_CLASS_MASK_DP)) == 7) return 0;
    LOG_REPORT((const char*)(uintptr_t)0x004fe638, (uint32_t)b);
    return 1;
}
static void fp_dp_handle(Footprint& f, DataPort*, Edx, uint8_t*, int, SockAddr*) { f.replay_only = R_LOG; }
PORT_FN(0x004af040, "DataPort::handle_packet", DataPort_handle_packet, fp_dp_handle)

// the vtable read once: _recv's slot first, handle_packet's after the first packet
static uint8_t __fastcall DataPort_Recv(DataPort* self, Edx, void* pkt, int* len, SockAddr* addr) {
    void** const vt = self->vtable;
    const Recv_t rcv = (Recv_t)vt[DP__recv / 4];
    const int saved = *len;
    if (!rcv(self, 0, pkt, len, addr)) return 0;
    const Handle_t handle = (Handle_t)vt[DP_handle_packet / 4];
    for (;;) {
        if (!handle(self, 0, pkt, *len, addr)) return 1;
        *len = saved;
        if (!rcv(self, 0, pkt, len, addr)) return 0;
    }
}
static void fp_dp_recv(Footprint& f, DataPort*, Edx, void*, int*, SockAddr*) { f.replay_only = R_IO; }
PORT_FN(0x004af080, "DataPort::Recv", DataPort_Recv, fp_dp_recv)

static void __fastcall DataPort_GetBroadcastAddress(DataPort* self, Edx, SockAddr* addr) {
    NETC_VFN(self->sock, IS_GetBroadcastAddr, void, void*)(self->sock, 0, addr);
}
static void fp_dp_bcast(Footprint& f, DataPort*, Edx, SockAddr*) { f.replay_only = R_IO; }
PORT_FN(0x004af100, "DataPort::GetBroadcastAddress", DataPort_GetBroadcastAddress, fp_dp_bcast)

static uint8_t __fastcall DataPort_AddressIsLocal(DataPort* self, Edx, const SockAddr* addr) {
    return NETC_VFN(self->sock, IS_AddressIsLocal, uint8_t, const void*)(self->sock, 0, addr);
}
static void fp_dp_islocal(Footprint& f, DataPort*, Edx, const SockAddr*) { f.replay_only = R_IO; }
PORT_FN(0x004af110, "DataPort::AddressIsLocal", DataPort_AddressIsLocal, fp_dp_islocal)

// =========================================================================================================================
// dataport.obj: ReliableDataPort
// =========================================================================================================================
typedef HostEntry*(__fastcall* GetEntry_t)(void*, Edx, const void*, uint8_t);
typedef void(__fastcall* HESendRel_t)(void*, Edx, const void*, int, const RPI*);
typedef void(__fastcall* HERecvAck_t)(void*, Edx, const uint8_t*);
typedef uint8_t(__fastcall* HEHold_t)(void*, Edx, const uint8_t*, int);
typedef void(__fastcall* HEInit_t)(void*, Edx, const void*, void*, RPP**);
typedef uint8_t(__fastcall* U8V_t)(void*, Edx);

static ReliableDataPort* __fastcall RDP_ctor(ReliableDataPort* self, Edx, void* sock, int n) {
    ((DataPort*(__fastcall*)(void*, Edx, void*))(uintptr_t)F_DataPort_ctor)(self, 0, sock);
    fn<PoolCtor_t>(F_PoolBase_ctor)(&self->rpp_pool, 0, (const char*)(uintptr_t)S_POOL_NAME_RPP, n * 6, 0x104);
    self->rpp_pool.vtable = (void**)(uintptr_t)VT_Pool_RPP;
    fn<PoolCtor_t>(F_PoolBase_ctor)(&self->node_pool, 0, (const char*)(uintptr_t)S_POOL_NAME_NODE, n * 2, 0x38);
    self->node_pool.vtable = (void**)(uintptr_t)VT_Pool_node;
    self->dp.vtable = (void**)(uintptr_t)VT_ReliableDataPort;
    self->timeout_cb = 0;
    self->ready = 0;
    self->dead = 0;
    self->hosts = 0;
    if (fn<U8V_t>(F_DataPort_Ok)(self, 0)) fn<PoolU8_t>(F_PoolBase_OverallocCrashes)(&self->rpp_pool, 0, 0);
    return self;
}
static void fp_rdp_ctor(Footprint& f, ReliableDataPort*, Edx, void*, int) { f.replay_only = R_HEAP; }
PORT_FN(0x004af130, "ReliableDataPort::ReliableDataPort", RDP_ctor, fp_rdp_ctor)

static uint8_t __fastcall RDP_Ok(ReliableDataPort* self, Edx) { return fn<U8V_t>(F_DataPort_Ok)(self, 0); }   // (a tail jump)
static void fp_rdp_ok(Footprint&, ReliableDataPort*, Edx) {}
PORT_FN(0x004af1b0, "ReliableDataPort::Ok", RDP_Ok, fp_rdp_ok)

static void __fastcall RDP_dtor(ReliableDataPort* self, Edx) {
    HostEntry* h = self->hosts;
    self->dp.vtable = (void**)(uintptr_t)VT_ReliableDataPort;
    for (; h; h = h->next) fn<This_t>(F_HE_destroy)(h, 0);
    fn<This_t>(F_PoolBase_freeall)(&self->rpp_pool, 0);
    fn<This_t>(F_PoolBase_freeall)(&self->node_pool, 0);
    fn<This_t>(F_PoolBase_dtor)(&self->node_pool, 0);
    fn<This_t>(F_PoolBase_dtor)(&self->rpp_pool, 0);
    fn<This_t>(F_DataPort_dtor)(self, 0);
}
static void fp_rdp_dtor(Footprint& f, ReliableDataPort*, Edx) { f.replay_only = R_HEAP; }
PORT_FN(0x004af1c0, "ReliableDataPort::~ReliableDataPort", RDP_dtor, fp_rdp_dtor)

static void __fastcall RDP_SetTimeoutCB(ReliableDataPort* self, Edx, void* cb, const uint32_t* data) {
    self->timeout_cb = cb;
    self->timeout_data = *data;
}
static void fp_rdp_settimeout(Footprint& f, ReliableDataPort* self, Edx, void*, const uint32_t*) {
    f.add(&self->timeout_cb, 8, "ReliableDataPort's timeout callback");
}
PORT_FN(0x004af210, "ReliableDataPort::SetTimeoutCB", RDP_SetTimeoutCB, fp_rdp_settimeout)

// FIX: no room for another host (get_entry's 0: the node pool exhausted) drops the packet, logged as HostEntry::SendReliable
// logs a full packet pool (the original sent it through 0)
static void __fastcall RDP_SendReliable(ReliableDataPort* self, Edx, const void* pkt, int len, const SockAddr* addr,
                                        const RPI* rpi) {
    HostEntry* const h = fn<GetEntry_t>(F_RDP_get_entry)(self, 0, addr, 1);
    if (VP_FIX && !h) {
        VP_FIX_HIT("ReliableDataPort::SendReliable");
        LOG_REPORT((const char*)(uintptr_t)0x004fe7f8);      // "SendReliable: Can't alloc packet"
        return;
    }
    fn<HESendRel_t>(F_HE_SendReliable)(h, 0, pkt, len, rpi);
}
static void fp_rdp_sendrel(Footprint& f, ReliableDataPort*, Edx, const void*, int, const SockAddr*, const RPI*) { f.replay_only = R_IO; }
PORT_FN(0x004af230, "ReliableDataPort::SendReliable", RDP_SendReliable, fp_rdp_sendrel)

// FIX: get_entry gives 0 when the host pool is exhausted (every address that sends one of these packets takes a host:
// a busy LAN, or foreign packets to the port, can use them all up); the original ran RecvAck / HoldIncomingPacket on 0.
// Such a packet is dropped (taken, not handed out).
static uint8_t __fastcall RDP_handle_packet(ReliableDataPort* self, Edx, uint8_t* pkt, int len, SockAddr* addr) {
    const uint32_t cls = (uint32_t)(uint8_t)((pkt[0] >> 4) & G8(G_CLASS_MASK_HE));
    switch (cls) {
    case 0:
    case 1: {
        HostEntry* const h = fn<GetEntry_t>(F_RDP_get_entry)(self, 0, addr, 1);
        if (VP_FIX && !h) {
            VP_FIX_HIT("ReliableDataPort::handle_packet");
            return 1;
        }
        fn<HERecvAck_t>(F_HE_RecvAck)(h, 0, pkt);
        return 1;
    }
    case 2:
    case 3:
    case 4:
    case 5: {
        HostEntry* const h = fn<GetEntry_t>(F_RDP_get_entry)(self, 0, addr, 1);
        if (VP_FIX && !h) {
            VP_FIX_HIT("ReliableDataPort::handle_packet");
            return 1;
        }
        return fn<HEHold_t>(F_HE_HoldIncomingPacket)(h, 0, pkt, len);
    }
    case 6:
        return 0;
    default:
        return fn<Handle_t>(F_DataPort_handle_packet)(self, 0, pkt, len, addr);
    }
}
static void fp_rdp_handle(Footprint& f, ReliableDataPort*, Edx, uint8_t*, int, SockAddr*) { f.replay_only = R_IO; }
PORT_FN(0x004af260, "ReliableDataPort::handle_packet", RDP_handle_packet, fp_rdp_handle)

static bool on_list(HostEntry* list, HostEntry* h) {
    for (; list; list = list->next)
        if (list == h) return true;
    return false;
}
// FIX: a host that goes down in its Tick (host_is_down) has its link overwritten with the dead list's. The original then
// walked on along the dead list (the hosts after it went unticked) and, below, unlinked it through that link: every host
// listed after it was lost (never ticked again, its packets never resent or acked, its slot and pool entry kept). The
// fixed loop keeps each host's successor before its Tick and takes a host that went down out of the list at once; the
// dead loop then finds it gone and only destroys and frees it. With the host that went down last in the list (or the only
// one) both end the same.
static void __fastcall RDP_Tick(ReliableDataPort* self, Edx) {
    fn<This_t>(F_DataPort_Tick)(self, 0);
    if (VP_FIX) {
        HostEntry** pp = &self->hosts;
        for (HostEntry* h = *pp; h;) {
            HostEntry* const nx = h->next;
            fn<This_t>(F_HE_Tick)(h, 0);
            if (on_list(self->dead, h)) {
                if (h->next != nx) VP_FIX_HIT("ReliableDataPort::Tick");
                *pp = nx;
            } else {
                pp = &h->next;
            }
            h = nx;
        }
    } else {
        for (HostEntry* h = self->hosts; h; h = h->next) fn<This_t>(F_HE_Tick)(h, 0);
    }
    HostEntry* d = self->dead;
    while (d) {
        HostEntry* const nx = d->next;
        HostEntry** pp = &self->hosts;
        if (self->hosts) {
            do {
                if (d == *pp) { *pp = nx; break; }
                pp = &(*pp)->next;
            } while (*pp);
        }
        fn<This_t>(F_HE_destroy)(d, 0);
        pool_free(&self->node_pool, d);
        d = nx;
    }
    self->dead = 0;
}
static void fp_rdp_tick(Footprint& f, ReliableDataPort*, Edx) { f.replay_only = R_IO; }
PORT_FN(0x004af300, "ReliableDataPort::Tick", RDP_Tick, fp_rdp_tick)

static void __fastcall RDP_host_is_down(ReliableDataPort* self, Edx, HostEntry* h, const SockAddr* addr) {
    if (self->timeout_cb) ((TimeoutCb_t)self->timeout_cb)(addr, &self->timeout_data);
    HostEntry* const dead = self->dead;
    for (HostEntry* e = dead; e; e = e->next)
        if (h == e) return;
    h->next = dead;
    self->dead = h;
}
static void fp_rdp_hostdown(Footprint& f, ReliableDataPort*, Edx, HostEntry*, const SockAddr*) { f.replay_only = "runs the session's host-down callback"; }
PORT_FN(0x004af370, "ReliableDataPort::host_is_down", RDP_host_is_down, fp_rdp_hostdown)

static uint8_t __fastcall RDP_PacketsPending(ReliableDataPort* self, Edx) {
    for (HostEntry* h = self->hosts; h; h = h->next)
        if (h->sent || h->waiting) return 1;
    return 0;
}
static void fp_rdp_pending(Footprint&, ReliableDataPort*, Edx) {}
PORT_FN(0x004af3c0, "ReliableDataPort::PacketsPending", RDP_PacketsPending, fp_rdp_pending)

typedef uint8_t(__cdecl* AddrCmp_t)(const void*, const void*);
// FIX: a new host when the node pool is used up (2 x the channels, at least 8: every address that ever sent this port a
// reliable packet or an ack, or was sent one, keeps its host until it times out -- a dedicated server reaches it after
// that many players and browsers, a browser on a LAN with that many games) takes the place of the first idle host (nothing
// sent unacked, waiting or held), or is none (0: the packet is dropped). The original's pool panicked ("overalloc"),
// ending the game. A host made again starts fresh as any new one does (send_first / recv_first: the next packets each way
// start a stream), so the protocol carries on; only its round-trip estimate starts over.
static HostEntry* recycle_idle_host(ReliableDataPort* self, const SockAddr* addr, HostEntry**& pp) {
    for (HostEntry** q = &self->hosts; *q; q = &(*q)->next) {
        HostEntry* const h = *q;
        if (h->sent || h->waiting || h->held || on_list(self->dead, h)) continue;
        *q = h->next;                                        // out of the list
        if (pp == &h->next) pp = q;                          // (it was the last: the new one goes where it was)
        fn<This_t>(F_HE_destroy)(h, 0);                      // its slot back
        *pp = h;
        fn<HEInit_t>(F_HE_init)(h, 0, addr, self, &self->ready);
        (*pp)->next = 0;
        return h;
    }
    return 0;
}
static HostEntry* __fastcall RDP_get_entry(ReliableDataPort* self, Edx, const SockAddr* addr, uint8_t create) {
    HostEntry** pp = &self->hosts;
    if (*pp) {
        do {
            if (!fn<AddrCmp_t>(F_addr_ne)(addr, &(*pp)->addr)) break;
            pp = &(*pp)->next;
        } while (*pp);
    }
    if (VP_FIX && create && !*pp && !self->node_pool.free_list) {
        VP_FIX_HIT("ReliableDataPort::get_entry");
        return recycle_idle_host(self, addr, pp);
    }
    if (create && !*pp) {
        HostEntry* const h = (HostEntry*)pool_alloc(&self->node_pool);
        *pp = h;
        if (h) {
            fn<HEInit_t>(F_HE_init)(h, 0, addr, self, &self->ready);
            (*pp)->next = 0;
        }
    }
    return *pp;
}
static void fp_rdp_get_entry(Footprint& f, ReliableDataPort*, Edx, const SockAddr*, uint8_t) { f.replay_only = "takes a host from the pool"; }
PORT_FN(0x004af3e0, "ReliableDataPort::get_entry", RDP_get_entry, fp_rdp_get_entry)

static uint8_t __fastcall RDP__recv(ReliableDataPort* self, Edx, void* pkt, int* len, SockAddr* addr) {
    if (!self->ready) return fn<Recv_t>(F_DataPort__recv)(self, 0, pkt, len, addr);
    if (addr) memcpy(addr, &self->ready->rpi, 12);           // a received packet keeps its sender there
    const int n = self->ready->len;
    *len = n;
    memcpy(pkt, self->ready->pkt, (size_t)n);
    RPP* const r = self->ready;
    self->ready = r->next;
    pool_free(&self->rpp_pool, r);
    return 1;
}
static void fp_rdp__recv(Footprint& f, ReliableDataPort*, Edx, void*, int*, SockAddr*) { f.replay_only = R_IO; }
PORT_FN(0x004af450, "ReliableDataPort::_recv", RDP__recv, fp_rdp__recv)

// the mean round trip: rtt_sum / rtt_n. FIX: a zero count (a HostEntry constructed but never init'ed; init sets 1 and
// RecvAck only counts up) gives the sum instead of a division by zero, as does -1 with the sum at INT_MIN (an overflow)
static __forceinline int rtt_mean(int sum, int n) {
    if (VP_FIX && (n == 0 || (n == -1 && sum == INT32_MIN))) {
        VP_FIX_HIT("rtt_sum / rtt_n");
        return sum;
    }
    return sum / n;
}
static int __fastcall RDP_GetEstRTLatency(ReliableDataPort* self, Edx, const SockAddr* addr) {
    HostEntry* const h = fn<GetEntry_t>(F_RDP_get_entry)(self, 0, addr, 0);
    if (!h) return 1000;
    return rtt_mean(h->rtt_sum, h->rtt_n);
}
static void fp_rdp_latency(Footprint&, ReliableDataPort*, Edx, const SockAddr*) {}
PORT_FN(0x004af4e0, "ReliableDataPort::GetEstRTLatency", RDP_GetEstRTLatency, fp_rdp_latency)

// =========================================================================================================================
// hostent.obj: HostEntry
// =========================================================================================================================
typedef uint8_t(__fastcall* NormId_t)(void*, Edx, uint8_t);
typedef void(__fastcall* Enlist_t)(void*, Edx, RPP**, RPP*);
typedef void(__fastcall* InitRpp_t)(void*, Edx, RPP*, const void*, int, const RPI*);
typedef uint8_t(__fastcall* InList_t)(void*, Edx, RPP*, uint8_t);
typedef void(__fastcall* HERpp_t)(void*, Edx, RPP*);
typedef void(__fastcall* HEAck_t)(void*, Edx, uint8_t, uint8_t);
typedef void(__fastcall* HEList_t)(void*, Edx, RPP**);
typedef uint8_t(__fastcall* HEId_t)(void*, Edx, uint8_t);
typedef void(__fastcall* HostDown_t)(void*, Edx, HostEntry*, const void*);
typedef int(__cdecl* GetSlot_t)();
typedef void(__cdecl* RetSlot_t)(int);
typedef void(__cdecl* Assert_t)(int, const char*, ...);

static __forceinline PoolBase* rpp_pool_of(HostEntry* h) { return &h->port->rpp_pool; }

static HostEntry* __fastcall HE_ctor(HostEntry* self, Edx) {
    volatile uint32_t* d = (volatile uint32_t*)self;
    for (int i = 0; i < 13; i++) d[i] = 0;                   // rep stosd: 0x34 bytes (not the link)
    return self;
}
static void fp_he_ctor(Footprint& f, HostEntry* self, Edx) { f.add(self, 0x34, "the HostEntry (constructed)"); }
PORT_FN(0x004affe0, "HostEntry::HostEntry", HE_ctor, fp_he_ctor)

static void __fastcall HE_dtor(HostEntry* self, Edx) { fn<This_t>(F_HE_destroy)(self, 0); }   // (a tail jump)
static void fp_he_dtor(Footprint& f, HostEntry*, Edx) { f.replay_only = "returns its packets to the pool"; }
PORT_FN(0x004b0000, "HostEntry::~HostEntry", HE_dtor, fp_he_dtor)

static void __fastcall HE_init(HostEntry* self, Edx, const SockAddr* addr, ReliableDataPort* port, RPP** ready) {
    volatile uint32_t* d = (volatile uint32_t*)self;
    for (int i = 0; i < 13; i++) d[i] = 0;
    const int slot = fn<GetSlot_t>(F_get_slot)();
    self->slot = slot;
    self->rtt_sum = 1000;
    self->rtt_n = 1;
    memcpy(&self->addr, addr, 12);
    self->ready = ready;
    self->port = port;
    self->send_first = 1;
    self->recv_first = 1;
}
static void fp_he_init(Footprint& f, HostEntry* self, Edx, const SockAddr*, ReliableDataPort*, RPP**) {
    f.add(self, 0x34, "the HostEntry");
    f.add((void*)(uintptr_t)G_HOST_SLOTS, 8, "HostEntry's slots");
}
PORT_FN(0x004b0010, "HostEntry::init", HE_init, fp_he_init)

static int __cdecl get_slot_c() {
    volatile uint8_t* const s = (volatile uint8_t*)(uintptr_t)G_HOST_SLOTS;
    for (uint32_t i = 0; i < 8; i++)
        if (s[i] == 0) { s[i] = 1; return (int)i; }
    return -1;
}
static void fp_get_slot(Footprint& f) { f.add((void*)(uintptr_t)G_HOST_SLOTS, 8, "HostEntry's slots"); }
PORT_FN(0x004b0070, "get_slot", get_slot_c, fp_get_slot)

static void __fastcall HE_destroy(HostEntry* self, Edx) {
    fn<This_t>(F_HE_free_all_packets)(self, 0);
    fn<RetSlot_t>(F_return_slot)(self->slot);
}
static void fp_he_destroy(Footprint& f, HostEntry*, Edx) { f.replay_only = "returns its packets to the pool"; }
PORT_FN(0x004b0090, "HostEntry::destroy", HE_destroy, fp_he_destroy)

// FIX: a slot outside 0..7 is ignored (get_slot's -1, "none of the 8 free", a ninth host's: the original cleared the byte
// before the slots)
static void __cdecl return_slot_c(int s) {
    if (VP_FIX && (uint32_t)s >= 8u) {
        VP_FIX_HIT("return_slot");
        return;
    }
    *(volatile uint8_t*)(uintptr_t)(G_HOST_SLOTS + (uint32_t)s) = 0;
}
static void fp_return_slot(Footprint& f, int s) { f.add((void*)(uintptr_t)(G_HOST_SLOTS + (uint32_t)s), 1, "a HostEntry slot"); }
PORT_FN(0x004b00b0, "return_slot", return_slot_c, fp_return_slot)

static uint8_t __fastcall HE_normalize_id(HostEntry* self, Edx, uint8_t id) { return (uint8_t)(id - self->recv_id); }
static void fp_he_normalize(Footprint& f, HostEntry*, Edx, uint8_t) { f.pure = true; }
PORT_FN(0x004b00c0, "HostEntry::normalize_id", HE_normalize_id, fp_he_normalize)

static void __fastcall HE_enlist_at_end(HostEntry*, Edx, RPP** list, RPP* p) {
    RPP** pp = list;
    while (*pp) pp = &(*pp)->next;
    *pp = p;
    p->next = 0;
}
static void fp_he_enlist_end(Footprint& f, HostEntry*, Edx, RPP**, RPP*) { f.replay_only = "relinks the pending packets"; }
PORT_FN(0x004b00d0, "HostEntry::enlist_at_end", HE_enlist_at_end, fp_he_enlist_end)

static void __fastcall HE_enlist_by_id(HostEntry* self, Edx, RPP** list, RPP* p) {
    const uint8_t n = fn<NormId_t>(F_HE_normalize_id)(self, 0, p->pkt[1]);
    RPP** pp = list;
    if (*pp) {
        do {
            if (fn<NormId_t>(F_HE_normalize_id)(self, 0, (*pp)->pkt[1]) >= n) break;
            pp = &(*pp)->next;
        } while (*pp);
    }
    RPP* const at = *pp;
    const int cond = at && at->pkt[1] == p->pkt[1] ? 0 : 1;
    fn<Assert_t>(F_HE_ASSERT_MSG)(cond, (const char*)(uintptr_t)0x004fe7c8, (const char*)(uintptr_t)0x004fe7bc, 0xa6,
                                  (const char*)(uintptr_t)(at ? 0x004fe7a8 : 0x004fe7b0));
    p->next = *pp;
    *pp = p;
}
static void fp_he_enlist_id(Footprint& f, HostEntry*, Edx, RPP**, RPP*) { f.replay_only = "relinks the pending packets"; }
PORT_FN(0x004b0100, "HostEntry::enlist_by_id", HE_enlist_by_id, fp_he_enlist_id)

// FIX: the length is kept to the packet's 0xe4 bytes (a negative one to 0); the original copied it unchecked, past the
// pool entry (SendReliable takes the game's length as it is)
static void __fastcall HE_init_rpp(HostEntry*, Edx, RPP* r, const void* pkt, int len, const RPI* rpi) {
    r->next = 0;
    if (VP_FIX && (uint32_t)len > 0xe4u) {
        VP_FIX_HIT("HostEntry::init_rpp");
        len = len < 0 ? 0 : 0xe4;
    }
    memcpy(r->pkt, pkt, (size_t)len);
    r->len = len;
    if (rpi) memcpy(&r->rpi, rpi, 12);
    else r->rpi.fn = 0;
}
static void fp_he_init_rpp(Footprint& f, HostEntry*, Edx, RPP* r, const void*, int, const RPI*) {
    f.add(r, sizeof(RPP), "the pending packet");
}
PORT_FN(0x004b01a0, "HostEntry::init_rpp", HE_init_rpp, fp_he_init_rpp)

static uint8_t __fastcall HE_already_recd_pkt(HostEntry* self, Edx, uint8_t id) {
    if ((uint8_t)(id - self->recv_id + 0x10) <= 0x10) return 1;
    if (fn<InList_t>(F_HE_id_is_in_list)(self, 0, self->held, id)) return 1;
    return 0;
}
static void fp_he_already(Footprint&, HostEntry*, Edx, uint8_t) {}          // (reads the held list: not fuzzable)
PORT_FN(0x004b0200, "HostEntry::already_recd_pkt", HE_already_recd_pkt, fp_he_already)

static uint8_t __fastcall HE_id_is_in_list(HostEntry*, Edx, RPP* p, uint8_t id) {
    for (; p; p = p->next)
        if (p->pkt[1] == id) return 1;
    return 0;
}
static void fp_he_inlist(Footprint&, HostEntry*, Edx, RPP*, uint8_t) {}     // (follows the list: not fuzzable)
PORT_FN(0x004b0230, "HostEntry::id_is_in_list", HE_id_is_in_list, fp_he_inlist)

static void __fastcall HE_send_first_time(HostEntry* self, Edx, RPP* r) {
    fn<HERpp_t>(F_HE_send)(self, 0, r);
    FOOD((const char*)(uintptr_t)0x004fe7d4, (uint32_t)r->pkt[1]);
    r->first_sent = net_PTimeNow();                          // site 0x4b027e
}
static void fp_he_send_first(Footprint& f, HostEntry*, Edx, RPP*) { f.replay_only = R_IO; }
PORT_FN(0x004b0260, "HostEntry::send_first_time", HE_send_first_time, fp_he_send_first)

static void __fastcall HE_SendReliable(HostEntry* self, Edx, const void* pkt, int len, const RPI* rpi) {
    RPP* const r = (RPP*)pool_alloc(rpp_pool_of(self));
    if (!r) {
        LOG_REPORT((const char*)(uintptr_t)0x004fe7f8);
        return;
    }
    fn<InitRpp_t>(F_HE_init_rpp)(self, 0, r, pkt, len, rpi);
    RPP* const s = self->sent;
    if ((!s || (uint8_t)(self->send_id - s->pkt[1]) < 2) && !self->waiting) {
        const int last = s ? (int)s->pkt[1] : -1;
        FOOD((const char*)(uintptr_t)0x004fe7e4, (uint32_t)self->send_id, last);
        fn<HERpp_t>(F_HE_send_first_time)(self, 0, r);
    } else {
        fn<Enlist_t>(F_HE_enlist_at_end)(self, 0, &self->waiting, r);
    }
}
static void fp_he_sendrel(Footprint& f, HostEntry*, Edx, const void*, int, const RPI*) { f.replay_only = R_IO; }
PORT_FN(0x004b0290, "HostEntry::SendReliable", HE_SendReliable, fp_he_sendrel)

static void __fastcall HE_RecvAck(HostEntry* self, Edx, const uint8_t* pkt) {
    const int now = net_PTimeNow();                          // site 0x4b0336
    RPP** pp = &self->sent;
    if (*pp) {
        do {
            if ((*pp)->pkt[1] == pkt[1]) break;
            pp = &(*pp)->next;
        } while (*pp);
    }
    RPP* const r = *pp;
    if (r) {
        uint8_t res = 1;
        if (!((uint8_t)(pkt[0] >> 4) & G8(G_CLASS_MASK_HE))) {   // an ack of class 0: a round trip measured
            const int n = self->rtt_n;
            if (n < 0x400) {
                const int rt = (int)((uint32_t)now - (uint32_t)r->first_sent);
                if (n == 1) self->rtt_sum = (int)((uint32_t)rt * 2u);
                else self->rtt_sum = (int)((uint32_t)self->rtt_sum + (uint32_t)rt);
                self->rtt_n = n + 1;
            }
            res = 5;
        }
        if (r->rpi.fn) ((RpiCb_t)r->rpi.fn)(res, r->rpi.data);
        *pp = (*pp)->next;
        pool_free(rpp_pool_of(self), r);
    }
    FOOD((const char*)(uintptr_t)0x004fe81c, (uint32_t)pkt[1]);
}
static void fp_he_recvack(Footprint& f, HostEntry*, Edx, const uint8_t*) { f.replay_only = "returns a packet to the pool, runs its callback"; }
PORT_FN(0x004b0330, "HostEntry::RecvAck", HE_RecvAck, fp_he_recvack)

static void __fastcall HE_ack(HostEntry* self, Edx, uint8_t id, uint8_t flag) {
    uint8_t pk[2];
    pk[0] = flag == 0 ? 0x10 : 0;
    pk[1] = id;
    fn<Send_t>(F_DataPort__send)(self->port, 0, pk, 2, &self->addr);
    FOOD((const char*)(uintptr_t)0x004fe830, (uint32_t)id);
}
static void fp_he_ack(Footprint& f, HostEntry*, Edx, uint8_t, uint8_t) { f.replay_only = R_IO; }
PORT_FN(0x004b03f0, "HostEntry::ack", HE_ack, fp_he_ack)

static void __fastcall HE_delete_all(HostEntry* self, Edx, RPP** list) {
    RPP* p = *list;
    while (p) {
        RPP* const nx = p->next;
        pool_free(rpp_pool_of(self), p);
        p = nx;
    }
    *list = 0;
}
static void fp_he_delete_all(Footprint& f, HostEntry*, Edx, RPP**) { f.replay_only = "returns packets to the pool"; }
PORT_FN(0x004b0440, "HostEntry::delete_all", HE_delete_all, fp_he_delete_all)

static void __fastcall HE_free_all_packets(HostEntry* self, Edx) {
    fn<HEList_t>(F_HE_delete_all)(self, 0, &self->waiting);
    fn<HEList_t>(F_HE_delete_all)(self, 0, &self->sent);
    fn<HEList_t>(F_HE_delete_all)(self, 0, &self->held);
}
static void fp_he_free_all(Footprint& f, HostEntry*, Edx) { f.replay_only = "returns packets to the pool"; }
PORT_FN(0x004b0480, "HostEntry::free_all_packets", HE_free_all_packets, fp_he_free_all)

// 0: hand it out now (in order, or a stream's first); 1: held (out of order) or a duplicate. Every packet is acked
// unless the pool had no room to hold it.
static uint8_t __fastcall HE_HoldIncomingPacket(HostEntry* self, Edx, const uint8_t* pkt, int len) {
    uint8_t result = 0, do_ack = 1;
    const uint32_t cls = (uint32_t)(uint8_t)((pkt[0] >> 4) & G8(G_CLASS_MASK_HE));
    if ((cls & 0xfffffffeu) == 4 || self->recv_first) {
        self->recv_id = pkt[1];
        fn<This_t>(F_HE_free_all_packets)(self, 0);
        self->recv_first = 0;
    } else {
        const uint8_t nx = (uint8_t)(self->recv_id + 1);
        if (nx == pkt[1]) {
            RPP* const h = self->held;
            self->recv_id = nx;
            if (h && h->pkt[1] == pkt[1]) {
                self->held = h->next;
                pool_free(rpp_pool_of(self), h);
            }
            FOOD((const char*)(uintptr_t)0x004fe844, (uint32_t)self->recv_id);
        } else if (!fn<HEId_t>(F_HE_already_recd_pkt)(self, 0, pkt[1])) {
            FOOD((const char*)(uintptr_t)0x004fe858, (uint32_t)pkt[1]);
            result = 1;
            RPP* const r = (RPP*)pool_alloc(rpp_pool_of(self));
            if (r) {
                fn<InitRpp_t>(F_HE_init_rpp)(self, 0, r, pkt, len, 0);
                memcpy(&r->rpi, &self->addr, 12);            // a received packet keeps its sender
                fn<Enlist_t>(F_HE_enlist_by_id)(self, 0, &self->held, r);
            } else {
                do_ack = 0;
                LOG_REPORT((const char*)(uintptr_t)0x004fe86c);
            }
        } else {
            FOOD((const char*)(uintptr_t)0x004fe894, (uint32_t)pkt[1]);
            result = 1;
        }
    }
    if (do_ack) {
        const uint8_t bit = (uint8_t)(((pkt[0] & 0x10) >> 4) & G8(G_CLASS_MASK_HE));
        fn<HEAck_t>(F_HE_ack)(self, 0, pkt[1], bit == 0 ? 1 : 0);
    }
    return result;
}
static void fp_he_hold(Footprint& f, HostEntry*, Edx, const uint8_t*, int) { f.replay_only = R_IO; }
PORT_FN(0x004b04b0, "HostEntry::HoldIncomingPacket", HE_HoldIncomingPacket, fp_he_hold)

static void __fastcall HE_send(HostEntry* self, Edx, RPP* r) {
    const uint8_t id = (uint8_t)(self->send_id + 1);
    self->send_id = id;
    const uint8_t c = r->pkt[0];
    r->pkt[1] = id;
    const uint32_t cls = self->send_first ? 4u : 2u;          // a stream's first packet: class 4
    r->pkt[0] = (uint8_t)((cls << 4) | (uint32_t)(c & G8(G_CLASS_MASK_HE)));
    fn<Send_t>(F_DataPort__send)(self->port, 0, r, r->len, &self->addr);
    r->sent = net_PTimeNow();                                // site 0x4b0650
    r->sends = 1;
    fn<Enlist_t>(F_HE_enlist_at_end)(self, 0, &self->sent, r);
    self->send_first = 0;
}
static void fp_he_send(Footprint& f, HostEntry*, Edx, RPP*) { f.replay_only = R_IO; }
PORT_FN(0x004b0610, "HostEntry::send", HE_send, fp_he_send)

static void __fastcall HE_resend(HostEntry* self, Edx, RPP* r) {
    FOOD((const char*)(uintptr_t)0x004fe8a4, (uint32_t)r->pkt[1]);
    const uint32_t m = G8(G_CLASS_MASK_HE);
    r->pkt[0] = (uint8_t)((((m << 4) | m) & r->pkt[0]) | 0x10);   // class 2 -> 3, 4 -> 5
    fn<Send_t>(F_DataPort__send)(self->port, 0, r, r->len, &self->addr);
    r->sent = net_PTimeNow();                                // site 0x4b06c3
    r->sends++;
}
static void fp_he_resend(Footprint& f, HostEntry*, Edx, RPP*) { f.replay_only = R_IO; }
PORT_FN(0x004b0680, "HostEntry::resend", HE_resend, fp_he_resend)

static void __fastcall HE_Tick(HostEntry* self, Edx) {
    // the held packets that are next in order: ready
    while (self->held && (uint8_t)(self->held->pkt[1] - self->recv_id) == 1) {
        RPP* const h = self->held;
        RPP* const nx = h->next;
        h->pkt[0] = (uint8_t)((h->pkt[0] & G8(G_CLASS_MASK_HE)) | 0x60);
        fn<Enlist_t>(F_HE_enlist_at_end)(self, 0, self->ready, self->held);
        FOOD((const char*)(uintptr_t)0x004fe8b8, (uint32_t)self->held->pkt[1]);
        self->held = nx;
        self->recv_id++;
        if (!nx) break;
    }
    const int now = net_PTimeNow();                          // site 0x4b073c
    const int rtt = rtt_mean(self->rtt_sum, self->rtt_n);
    int timeout = (int)((uint32_t)rtt * 3u) / 2;
    if (!(timeout > 250)) timeout = 250;
    for (RPP* p = self->sent; p;) {
        RPP* const nx = p->next;
        if ((int)((uint32_t)now - (uint32_t)p->sent) > timeout) {
            if (p->sends <= 5) {
                fn<HERpp_t>(F_HE_resend)(self, 0, p);
            } else {
                if (p->rpi.fn) ((RpiCb_t)p->rpi.fn)(2, p->rpi.data);
                fn<HostDown_t>(F_RDP_host_is_down)(self->port, 0, self, &self->addr);
            }
        }
        p = nx;
    }
    RPP* const w = self->waiting;
    if (w) {
        RPP* const s = self->sent;
        if (!s || (uint8_t)(self->send_id - s->pkt[1]) < 2) {
            RPP* const nx = w->next;
            fn<HERpp_t>(F_HE_send_first_time)(self, 0, w);
            FOOD((const char*)(uintptr_t)0x004fe8d8, (uint32_t)self->waiting->pkt[1]);
            self->waiting = nx;
        }
    }
}
static void fp_he_tick(Footprint& f, HostEntry*, Edx) { f.replay_only = R_IO; }
PORT_FN(0x004b06e0, "HostEntry::Tick", HE_Tick, fp_he_tick)

// =========================================================================================================================
// nettypes.obj: the car packet's wire form
// =========================================================================================================================
// CarPhysicsPacket (phys_netcar.cpp): the offsets this code uses
enum : uint32_t {
    CPP_TIME = 0x04, CPP_BRAKING = 0x08, CPP_STEERING = 0x0c, CPP_GEAR = 0x14, CPP_RPM = 0x18, CPP_POS = 0x1c, CPP_ROT = 0x28,
    CPP_HORN = 0x38, CPP_TELEPORTED = 0x39, CPP_WHEELS = 0x3a,
};
// CarNetPacket: [0] u16 time, [2] NetFrame (pos 12 bytes, rot 3 x u16), [0x14] rpm, [0x15] gear / steering, [0x16] flags
enum : uint32_t { CNP_FRAME = 0x02, CNP_RPM = 0x14, CNP_GS = 0x15, CNP_FLAGS = 0x16 };

static __forceinline float ldf(const uint8_t* p, uint32_t off) { float f; memcpy(&f, p + off, 4); return f; }
static __forceinline void stf(uint8_t* p, uint32_t off, float f) { volatile float* d = (volatile float*)(p + off); *d = f; }
static __forceinline uint32_t ldu(const uint8_t* p, uint32_t off) { uint32_t u; memcpy(&u, p + off, 4); return u; }
static __forceinline void stu(uint8_t* p, uint32_t off, uint32_t u) { memcpy(p + off, &u, 4); }

static const uint32_t K_OFS = 0x4a03126f;          // 2147483.75
static const uint32_t K_2P32 = 0x4f800000;         // 4294967296
static const uint32_t K_SCALE_IN = 0x3479ffff;     // 2.3283063e-07
static const uint32_t K_STEP = 0x4a83126f;         // 4294967.5
static const uint32_t K_2M32 = 0x2f800000;         // 2.3283064e-10
static const uint32_t K_EPS = 0x34000000;          // 1.1920929e-07
static const uint32_t K_RMAX = 0x41a00000;         // 20
static const uint32_t K_RMIN = 0xc1a00000;         // -20
static const uint32_t K_2M15 = 0x38000000;         // 3.0517578e-05
static const uint32_t K_2P15 = 0x47000000;         // 32768
static float k_half = 0.5f;                         // the asm helpers' memory constants (0x4df588, 0x4df594)
static float k_eps_f = 1.1920928955078125e-07f;

// fild qword of a zero-extended dword: exact (C's unsigned -> double, even through int64, compiles to fild dword and an
// fadd of 2^32 when the sign bit is set -- an add rounded to the FPU's precision)
static __forceinline double fild_u32(uint32_t q) {
    uint64_t v = q;
    double r;
#ifdef VP_GCC
    __asm__ volatile("fild %[v]\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [v] "m"(v)
                     : VP_X87_CLOBBERS);
#else
    __asm { fild qword ptr v
            fstp r }
#endif
    return r;
}

// a position through the wire's quantisation (each coordinate offset, scaled to 32 bits, truncated, and back)
static void __cdecl screw_up_c(uint8_t* p) {
    for (uint32_t k = 0; k < 3; k++) {
        const double t = (D(ldf(p, 4 * k)) + D(FB(K_OFS))) * D(FB(K_2P32)) * D(FB(K_SCALE_IN));
        const uint32_t q = (uint32_t)x87_ftol(t);
        const double dq = fild_u32(q);
        stf(p, 4 * k, (float)(dq * D(FB(K_STEP)) * D(FB(K_2M32)) - D(FB(K_OFS))));
    }
}
static void fp_screw_up(Footprint& f, uint8_t* p) { f.pure = true; f.add(p, 12, "the position"); }
PORT_FN(0x004a4400, "screw_up", screw_up_c, fp_screw_up)

// rot_convert(Quat&, NetAxisAngle const&): past the axis' length, what the original does with fsin / fcos (their 64-bit
// results multiplied in the registers, then stored), in the original's instructions
static __declspec(noinline) void rot_q_tail(float* q, const float* axis, float sumf) {
#ifdef VP_GCC
    __asm__ volatile("mov     eax, %[axis]\n\t"
                     "mov     edx, %[q]\n\t"
                     "fld     %[sumf]\n\t"
                     "fsqrt\n\t"
                     "fld     %[half]\n\t"
                     "fmul    st, st(1)\n\t"
                     "fld     st(0)\n\t"
                     "fsin\n\t"
                     "fld     dword ptr [eax]\n\t"
                     "fdiv    st, st(3)\n\t"
                     "fmul    st, st(1)\n\t"
                     "fstp    dword ptr [edx]\n\t"
                     "fld     dword ptr [eax + 4]\n\t"
                     "fdiv    st, st(3)\n\t"
                     "fmul    st, st(1)\n\t"
                     "fstp    dword ptr [edx + 4]\n\t"
                     "fxch    st(2)\n\t"
                     "fdivr   dword ptr [eax + 8]\n\t"
                     "fmul    st, st(2)\n\t"
                     "fstp    dword ptr [edx + 8]\n\t"
                     "fcos\n\t"
                     "fstp    dword ptr [edx + 12]\n\t"
                     "fstp    st(0)"
                     :
                     : [axis] "m"(axis), [q] "m"(q), [sumf] "m"(sumf), [half] "m"(k_half)
                     : VP_X87_CLOBBERS, "eax", "edx", "memory");
#else
    __asm {
        mov     eax, axis
        mov     edx, q
        fld     sumf
        fsqrt
        fld     dword ptr [k_half]
        fmul    st, st(1)
        fld     st(0)
        fsin
        fld     dword ptr [eax]
        fdiv    st, st(3)
        fmul    st, st(1)
        fstp    dword ptr [edx]
        fld     dword ptr [eax + 4]
        fdiv    st, st(3)
        fmul    st, st(1)
        fstp    dword ptr [edx + 4]
        fxch    st(2)
        fdivr   dword ptr [eax + 8]
        fmul    st, st(2)
        fstp    dword ptr [edx + 8]
        fcos
        fstp    dword ptr [edx + 12]
        fstp    st(0)
    }
#endif
}
static void __cdecl rot_convert_q(uint8_t* q, const uint8_t* a) {
    volatile float vmax = FB(K_RMAX), vmin = FB(K_RMIN);
    const double range = D(vmax) - D(vmin);                    // 40, at run time as the original
    uint16_t w[3];
    memcpy(w, a, 6);
    float axis[3];
    axis[0] = (float)(D(w[0]) * range * D(FB(K_2M15)) + D(FB(K_RMIN)));
    axis[1] = (float)(D(w[1]) * range * D(FB(K_2M15)) + D(FB(K_RMIN)));
    const double az = D(w[2]) * range * D(FB(K_2M15)) + D(FB(K_RMIN));   // kept in the register, stored as a float too
    axis[2] = (float)az;
    const double sum = az * D(axis[2]) + D(axis[1]) * D(axis[1]) + D(axis[0]) * D(axis[0]);
    const float sumf = (float)sum;                             // fst dword
    if (!(fabs(sum) > D(FB(K_EPS)))) {
        stu(q, 0, 0);
        stu(q, 4, 0);
        stu(q, 12, 0x3f800000);
        stu(q, 8, 0);
        return;
    }
    rot_q_tail((float*)q, axis, sumf);
}
static void fp_rot_q(Footprint& f, uint8_t* q, const uint8_t*) { f.pure = true; f.add(q, 16, "the quaternion"); }
PORT_FN(0x004a4600, "rot_convert(Quat)", rot_convert_q, fp_rot_q)

static void __cdecl frame_convert_q(uint8_t* out, const uint8_t* in) {
    stu(out, 0, ldu(in, 0));
    stu(out, 4, ldu(in, 4));
    stu(out, 8, ldu(in, 8));
    ((void(__cdecl*)(void*, const void*))(uintptr_t)F_rot_convert_q)(out + 12, in + 12);
}
static void fp_frame_q(Footprint& f, uint8_t* out, const uint8_t*) { f.pure = true; f.add(out, 28, "the QFrame"); }
PORT_FN(0x004a45d0, "frame_convert(QFrame)", frame_convert_q, fp_frame_q)

// the 16-bit PTime of a packet, in full: the one nearest now
static uint32_t __cdecl assumable_t_c(uint16_t t) {
    const int now = net_PTimeNow();                          // site 0x4a4740
    uint32_t v = (uint32_t)t | ((uint32_t)now & 0xffff0000u);
    const int d = (int)(v - (uint32_t)now);
    if (d > 0x8000) return v - 0x10000;
    if (d < -0x8000) v += 0x10000;
    return v;
}
static void fp_assumable(Footprint&, uint16_t) {}
PORT_FN(0x004a4740, "assumable_t", assumable_t_c, fp_assumable)

typedef float(__cdecl* PTimeToPhys_t)(uint32_t);
typedef int(__cdecl* PhysToPTime_t)(uint32_t);            // its float argument passed by its bits (pushed from eax)

static void __cdecl MakePhysicsPacket_c(uint8_t* out, const uint8_t* in) {
    uint16_t t16;
    memcpy(&t16, in, 2);
    const uint32_t t = ((uint32_t(__cdecl*)(uint16_t))(uintptr_t)F_assumable_t)(t16);
    stf(out, CPP_TIME, fn<PTimeToPhys_t>(F_ConvertPTimeToPhysicsTime)(t));
    ((void(__cdecl*)(void*, const void*))(uintptr_t)F_frame_convert_q)(out + CPP_POS, in + CNP_FRAME);
    stf(out, CPP_RPM, (float)(D(in[CNP_RPM]) * D(FB(0x3b808081)) * D(FB(0x45fa0000))));
    const uint32_t gear = in[CNP_GS] & 7u;
    stu(out, CPP_GEAR, gear);
    if (gear == 7) stu(out, CPP_GEAR, 0xffffffffu);
    const uint8_t gs = in[CNP_GS];
    float s = (float)(D((int)((gs & 0x78u) >> 3)) * D(FB(0x3d888889)));
    if (gs & 0x80) s = -s;
    stf(out, CPP_STEERING, s);
    stu(out, CPP_BRAKING, 0x3f000000);
    if (!(in[CNP_FLAGS] & 1)) stu(out, CPP_BRAKING, 0);
    out[CPP_HORN] = (uint8_t)((in[CNP_FLAGS] & 2u) >> 1);
    out[CPP_TELEPORTED] = (uint8_t)((in[CNP_FLAGS] & 4u) >> 2);
    uint32_t bit = 1;
    for (uint32_t k = 0; k < 4; k++) {
        out[CPP_WHEELS + k] = (((uint32_t)in[CNP_FLAGS] & 0x78u) >> 3 & bit) ? 1 : 0;
        bit = (bit + bit) & 0xff;
    }
}
static void fp_make_phys(Footprint& f, uint8_t* out, const uint8_t*) { f.add(out, 0x3e, "the CarPhysicsPacket"); }
PORT_FN(0x004a44d0, "MakePhysicsPacket", MakePhysicsPacket_c, fp_make_phys)

// rot_convert(NetAxisAngle&, Quat const&): the angle (CIacos of w) and its sine as the original has them: CIacos's result
// stored as a float, fsin on the register, the sine stored as a float and compared (unrounded) with the epsilon
static __declspec(noinline) int acos_sin(float w, float* angle, float* sine) {
    int big;
#ifdef VP_GCC
    __asm__ volatile("fld     %[w]\n\t"
                     "mov     eax, 0x004cf07c\n\t"
                     "call    eax\n\t"
                     "mov     ecx, %[angle]\n\t"
                     "fst     dword ptr [ecx]\n\t"
                     "fsin\n\t"
                     "mov     ecx, %[sine]\n\t"
                     "fst     dword ptr [ecx]\n\t"
                     "fabs\n\t"
                     "fcomp   %[eps]\n\t"
                     "fnstsw  ax\n\t"
                     "xor     ecx, ecx\n\t"
                     "test    ah, 0x41\n\t"
                     "jne     done%=\n\t"
                     "mov     ecx, 1\n"
                     "done%=:\n\t"
                     "mov     %[big], ecx"
                     : [big] "=m"(big)
                     : [w] "m"(w), [angle] "m"(angle), [sine] "m"(sine), [eps] "m"(k_eps_f)
                     : VP_X87_CLOBBERS, "eax", "ecx", "edx", "cc", "memory");
#else
    __asm {
        fld     w
        mov     eax, 0x004cf07c
        call    eax
        mov     ecx, angle
        fst     dword ptr [ecx]
        fsin
        mov     ecx, sine
        fst     dword ptr [ecx]
        fabs
        fcomp   dword ptr [k_eps_f]
        fnstsw  ax
        xor     ecx, ecx
        test    ah, 0x41
        jne     done
        mov     ecx, 1
    done:
        mov     big, ecx
    }
#endif
    return big;
}
static __forceinline float clamp20(float v) {
    if (!(v >= FB(K_RMIN))) return FB(K_RMIN);
    return FB(K_RMAX) > v ? v : FB(K_RMAX);
}
static void __cdecl rot_convert_n(uint8_t* out, const uint8_t* q) {
    float angle, sine, ax, ay, az;
    if (acos_sin(ldf(q, 12), &angle, &sine)) {
        const double a2 = D(angle) * D(FB(0x40000000));
        ax = (float)(D(ldf(q, 0)) / D(sine) * a2);
        ay = (float)(D(ldf(q, 4)) / D(sine) * a2);
        az = (float)(D(ldf(q, 8)) / D(sine) * a2);
    } else {
        ax = ay = az = 0.0f;
    }
    volatile float vmax = FB(K_RMAX), vmin = FB(K_RMIN);
    const float range = (float)(D(vmax) - D(vmin));          // fstp dword: 40
    const float c[3] = {clamp20(ax), clamp20(ay), clamp20(az)};
    for (uint32_t k = 0; k < 3; k++) {
        const int32_t v = x87_ftol((D(c[k]) - D(FB(K_RMIN))) / D(range) * D(FB(K_2P15)));
        const uint16_t u = (uint16_t)v;
        memcpy(out + 2 * k, &u, 2);
    }
}
// (CIacos is the CRT's: it writes the CRT's math error context)
static void fp_crt_math(Footprint& f) {
    f.add((void*)(uintptr_t)0x005d5600, 0x100, "the CRT's math context (CIacos)");
    f.add((void*)(uintptr_t)0x00502730, 4, "errno (CIacos: a domain error)");
}
static void fp_rot_n(Footprint& f, uint8_t* out, const uint8_t*) { f.add(out, 6, "the NetAxisAngle"); fp_crt_math(f); }
PORT_FN(0x004a48e0, "rot_convert(NetAxisAngle)", rot_convert_n, fp_rot_n)

static void __cdecl frame_convert_n(uint8_t* out, const uint8_t* in) {
    stu(out, 0, ldu(in, 0));
    stu(out, 4, ldu(in, 4));
    stu(out, 8, ldu(in, 8));
    ((void(__cdecl*)(void*, const void*))(uintptr_t)F_rot_convert_n)(out + 12, in + 12);
}
static void fp_frame_n(Footprint& f, uint8_t* out, const uint8_t*) { f.add(out, 18, "the NetFrame"); fp_crt_math(f); }
PORT_FN(0x004a48b0, "frame_convert(NetFrame)", frame_convert_n, fp_frame_n)

static void __cdecl MakeNetPacket_c(uint8_t* out, const uint8_t* in) {
    const uint16_t t = (uint16_t)fn<PhysToPTime_t>(F_ConvertPhysicsTimeToPTime)(ldu(in, CPP_TIME));
    memcpy(out, &t, 2);
    ((void(__cdecl*)(void*, const void*))(uintptr_t)F_frame_convert_n)(out + CNP_FRAME, in + CPP_POS);
    const float rpm = ldf(in, CPP_RPM);
    if (D(rpm) > D(FB(0x45fa0000))) out[CNP_RPM] = 0xff;      // fcomp; test ah,0x41 (a NaN is converted)
    else out[CNP_RPM] = (uint8_t)x87_ftol(D(rpm) / D(FB(0x45fa0000)) * D(FB(0x437f0000)));
    // gear (bits 0-2), the old byte's bits 3-7 kept for now
    const uint32_t g0 = out[CNP_GS];
    const uint32_t gb = ((ldu(in, CPP_GEAR) ^ g0) & 7u) ^ g0;
    out[CNP_GS] = (uint8_t)gb;
    // steering: bits 3-6 |s| x 15 (truncated), bit 7 the sign -- written as bl << 3, so an |s| past 1 spills upwards
    const uint32_t sb = ldu(in, CPP_STEERING);
    uint32_t bl = sb > 0x80000000u ? 0x10u : 0u;
    float sf;
    memcpy(&sf, &sb, 4);
    bl = (bl | (uint32_t)x87_ftol(fabs(D(sf)) * D(FB(0x41700000)))) & 0xffu;
    const uint32_t a8 = (bl * 8u) & 0xffu;
    const uint32_t gs = (((a8 ^ gb) & 7u) ^ a8) & 0xffu;
    const uint32_t f0 = out[CNP_FLAGS];
    out[CNP_GS] = (uint8_t)gs;
    // the flags: braking (bit 0), horn (1), teleported (2), wheels broken (3-6); bit 7 the old byte's
    const uint32_t brk = (int32_t)ldu(in, CPP_BRAKING) > 0x3c23d70a ? 1u : 0u;
    uint32_t c1 = (((brk ^ f0) & 1u) ^ f0) & 0xffu;
    out[CNP_FLAGS] = (uint8_t)c1;
    const uint32_t h2 = ((uint32_t)in[CPP_HORN] * 2u) & 0xffu;
    const uint32_t c2 = (((h2 ^ c1) & 2u) ^ c1) & 0xffu;
    out[CNP_FLAGS] = (uint8_t)c2;
    const uint32_t t4 = ((uint32_t)in[CPP_TELEPORTED] << 2) & 0xffu;
    const uint32_t c3 = (((t4 ^ c2) & 4u) ^ c2) & 0xffu;
    out[CNP_FLAGS] = (uint8_t)c3;
    uint32_t wb = 0, bit = 1;
    for (uint32_t k = 0; k < 4; k++) {
        if (in[CPP_WHEELS + k] != 0) wb |= bit;
        bit = (bit + bit) & 0xff;
    }
    const uint32_t w8 = (wb * 8u) & 0xffu;
    out[CNP_FLAGS] = (uint8_t)(((w8 ^ c3) & 0x78u) ^ c3);
}
static void fp_make_net(Footprint& f, uint8_t* out, const uint8_t*) { f.add(out, 23, "the CarNetPacket"); fp_crt_math(f); }
PORT_FN(0x004a4780, "MakeNetPacket", MakeNetPacket_c, fp_make_net)

}  // namespace net_core
}  // namespace
