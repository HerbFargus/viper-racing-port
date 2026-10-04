// net_core.h -- multiplayer stage N1, group B: the layouts of the reliability layer and the session manager (library
// `multi`: dataport.obj, hostent.obj, datamod.obj, delqueue.obj, nettypes.obj, session.obj), read off the v1.0
// disassembly (out/types.json has next to nothing for this code). net_core.cpp and net_session.cpp rewrite them; the
// harness test/world_net_core.cpp builds them. Group A's transports (ISocket and its kin) are hook/net_types.h's; here an
// ISocket is only its vtable, by the slots the code below calls.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace netc {

typedef int Edx;                                   // the unused edx of a __thiscall received as __fastcall

// ---- the game's functions this code calls (v1.0) -------------------------------------------------------------------
enum : uint32_t {
    F_MemAlloc = 0x004140e0, F_OpDelete = 0x00414390,
    F_PoolBase_ctor = 0x0041ba30, F_PoolBase_dtor = 0x0041bac0, F_PoolBase_OverallocCrashes = 0x0041bb10,
    F_PoolBase_alloc = 0x0041bb20, F_PoolBase_free = 0x0041bb60, F_PoolBase_freeall = 0x0041bb80,
    F_LogReport = 0x00411150, F_LogPanic = 0x004112b0,
    F_TaskGetID = 0x00414dd0, F_TaskGetName = 0x00414ec0,
    F_Xlator_ctor = 0x0041af80, F_Xlator_xlate = 0x0041afb0, F_atexit = 0x004ceff0,
    F_memmove = 0x004cf400, F_strncpy = 0x004cf3a0, F_stricmp = 0x004da350, F_sprintf = 0x004cf0a0,
    F_CIacos = 0x004cf07c,
    F_ConvertPTimeToPhysicsTime = 0x00426ea0, F_ConvertPhysicsTimeToPTime = 0x00426ed0,
    F_MultiGetPacketDelay = 0x004a2b00,                 // multi.obj (N2)
    F_addr_eq = 0x004ae350, F_addr_ne = 0x004ae370,      // socket.obj (group A): operator== / != (socket_addr)
    // this group's own, called by address so the hooked rewrite or the original is what runs
    F_DelayQueue_ctor = 0x004a4000, F_DelayQueue_dtor = 0x004a4060, F_DelayQueue_Enqueue = 0x004a4090,
    F_advance_latency_idx = 0x004a4120, F_DelayQueue_Dequeue = 0x004a4130,
    F_assumable_t = 0x004a4740, F_frame_convert_q = 0x004a45d0, F_rot_convert_q = 0x004a4600,
    F_frame_convert_n = 0x004a48b0, F_rot_convert_n = 0x004a48e0,
    F_DataPort_ctor = 0x004aef60, F_DataPort_Ok = 0x004aef80, F_DataPort_dtor = 0x004aef90, F_DataPort__send = 0x004aefd0,
    F_DataPort__recv = 0x004aeff0, F_DataPort_Send = 0x004af010, F_DataPort_handle_packet = 0x004af040,
    F_DataPort_Recv = 0x004af080, F_DataPort_Tick = 0x004af120,
    F_RDP_dtor = 0x004af1c0, F_RDP_SetTimeoutCB = 0x004af210, F_RDP_SendReliable = 0x004af230,
    F_RDP_host_is_down = 0x004af370, F_RDP_PacketsPending = 0x004af3c0, F_RDP_get_entry = 0x004af3e0,
    F_RDP_GetEstRTLatency = 0x004af4e0, F_RDP_ctor = 0x004af130,
    F_DataModerator_dtor = 0x004afc90,
    F_HE_init = 0x004b0010, F_get_slot = 0x004b0070, F_HE_destroy = 0x004b0090, F_return_slot = 0x004b00b0,
    F_HE_normalize_id = 0x004b00c0, F_HE_enlist_at_end = 0x004b00d0, F_HE_enlist_by_id = 0x004b0100,
    F_HE_ASSERT_MSG = 0x004b0190, F_HE_init_rpp = 0x004b01a0, F_HE_already_recd_pkt = 0x004b0200,
    F_HE_id_is_in_list = 0x004b0230, F_HE_send_first_time = 0x004b0260, F_HE_SendReliable = 0x004b0290,
    F_HE_RecvAck = 0x004b0330, F_HE_ack = 0x004b03f0, F_HE_delete_all = 0x004b0440, F_HE_free_all_packets = 0x004b0480,
    F_HE_HoldIncomingPacket = 0x004b04b0, F_HE_send = 0x004b0610, F_HE_resend = 0x004b0680, F_HE_Tick = 0x004b06e0,
    F_FOOD = 0x004b0800,
    // session.obj
    F_SM_ctor = 0x004a4d20, F_sess_rdp_cb = 0x004a4ed0, F_sess_host_timeout_cb = 0x004a4ef0, F_SM_init_rpi = 0x004a4f10,
    F_SM_Ok = 0x004a4f30, F_SM_dtor = 0x004a4f40, F_SM_ASSERT_MSG = 0x004a5050, F_async_msg = 0x004a5320,
    F_servreq_cb = 0x004a5560, F_SM_OfferService = 0x004a58f0, F_unique_service_id = 0x004a5a70,
    F_SM_broadcast_servdown = 0x004a5a90, F_SM_withdraw_service = 0x004a5b30, F_SM_Connect_rs = 0x004a5d90,
    F_SM_Connect_id = 0x004a5ea0, F_SM_toss_data = 0x004a6050, F_SM_Disconnect = 0x004a60a0,
    F_SM_DisconnectAll = 0x004a62f0, F_SM_GetNextStatus2 = 0x004a6760, F_SM_find_range = 0x004a6860,
    F_SM_find_empty_session = 0x004a68d0, F_SM_recv_goaway = 0x004a6900, F_SM_dispatch = 0x004a6990,
    F_SM_recv_chandata = 0x004a6a90, F_SM_recv_connreq = 0x004a6c20, F_SM_recv_connreject = 0x004a6d60,
    F_SM_recv_connaccept = 0x004a6e40, F_SM_recv_disconreq = 0x004a6f40, F_SM_make_servinfo_packet = 0x004a7000,
    F_SM_broadcast_servinfo = 0x004a7100, F_SM_recv_servreq = 0x004a7140, F_SM_recv_servinfo = 0x004a71a0,
    F_SM_recv_servdown = 0x004a7290, F_SM_hostdown_cb = 0x004a7320, F_SM_reliable_cb = 0x004a73d0,
    F_SM_send_connaccept = 0x004a74c0, F_SM_send_connreject = 0x004a7530, F_stat_dump = 0x004a7650,
};

// ---- statics (v1.0) ----------------------------------------------------------------------------------------------------
enum : uint32_t {
    G_CLASS_MASK_DP = 0x0057d62c,      // byte: the port-class mask DataPort reads (0xf, set by an $E)
    G_CLASS_MASK_HE = 0x0057d708,      // byte: the same for HostEntry / ReliableDataPort
    G_CHAN_MASK = 0x0057d360,          // byte: SessionMgr's "this is channel data" mask on byte 0
    G_HOST_SLOTS = 0x0057d718,         // 8 bytes: HostEntry's slots (get_slot / return_slot)
    G_SERVICE_ID = 0x004fbbd4,         // unique_service_id's counter
    G_XLATOR_COOKIE = 0x004eb108,      // Xlator::g_cookie
    // async_msg's six function-local Xlators and their guard; SessionDiscReasonString's seven and theirs
    G_ASYNC_GUARD = 0x0057d444, G_DISC_GUARD = 0x0057d3cc,
    S_POOL_NAME_DQ = 0x004fbb88, S_POOL_NAME_SESS = 0x004fbc10, S_POOL_NAME_RPP = 0x004fe674, S_POOL_NAME_NODE = 0x004fe690,
};

// vtables
enum : uint32_t {
    VT_DataPort = 0x004df880, VT_ReliableDataPort = 0x004df898, VT_Pool_RPP = 0x004df8ac, VT_Pool_node = 0x004df894,
    VT_Pool_SessionData = 0x004df5b0,
};

// ISocket's slots (socket.obj; byte offsets into its vtable)
enum : uint32_t {
    IS_dtor = 0x00, IS_Send = 0x04, IS_Recv = 0x08, IS_AsyncGetHostAddr = 0x0c, IS_AsyncCancel = 0x10,
    IS_GetHostAddr = 0x14, IS_MakeStr = 0x18, IS_AddressIsLocal = 0x1c, IS_GetMyAddr = 0x20, IS_GetBroadcastAddr = 0x24,
    IS_LinkAlive = 0x28,
};
// DataPort's slots
enum : uint32_t { DP_Ok = 0x00, DP_Tick = 0x04, DP_dtor = 0x08, DP_handle_packet = 0x0c, DP__recv = 0x10 };

#define NETC_VFN(obj, off, R, ...) ((R(__fastcall*)(void*, netc::Edx, ##__VA_ARGS__))((*(void***)(obj))[(off) / 4]))

struct SockAddr { uint8_t b[12]; };                // union socket_addr: 12 bytes, compared bytewise

// PoolBase (useful:pool.obj), 0x20 bytes
struct PoolBase {
    void** vtable;           // +0x00
    uint8_t crashes, _5[3];  // +0x04  OverallocCrashes
    const char* name;        // +0x08
    void* block;             // +0x0c  MemAlloc(count * size)
    void* free_list;         // +0x10
    uint32_t size;           // +0x14  an element's
    int32_t count;           // +0x18
    int32_t used;            // +0x1c
};
static_assert(sizeof(PoolBase) == 0x20, "PoolBase");

// ---- delqueue.obj: DelayQueue, 16 bytes ---------------------------------------------------------------------------------
struct DQEntry {
    DQEntry* next;           // +0x00  towards the tail (older)
    DQEntry* prev;           // +0x04  towards the head (newer)
    int32_t len;             // +0x08
    uint32_t tag;            // +0x0c
    int32_t due;             // +0x10  PTimeNow + MultiGetPacketDelay
    uint8_t data[1];         // +0x14  the queue's element size
};
struct DelayQueue {
    PoolBase* pool;          // +0x00  MemAlloc'd, (count, size + 0x14)
    DQEntry* head;           // +0x04  the newest
    DQEntry* tail;           // +0x08  the oldest: Dequeue's
    int32_t size;            // +0x0c
};
static_assert(sizeof(DelayQueue) == 0x10 && offsetof(DQEntry, data) == 0x14, "DelayQueue");

// ---- datamod.obj: DataModerator, 0x14 bytes -------------------------------------------------------------------------------
struct DataModerator {
    DelayQueue q;            // +0x00  (0xf1 entries of 0xf4 bytes: a packet's 0xe4, its length, its address)
    void* sock;              // +0x10  ISocket*
};
static_assert(sizeof(DataModerator) == 0x14, "DataModerator");

// ---- dataport.obj --------------------------------------------------------------------------------------------------------
// ReliablePacketInfo: a callback and its 8 bytes (generic_data), copied into the pending packet
struct RPI {
    void* fn;                // +0x00  void (*)(uint8_t result, RPUserData&)
    uint8_t data[8];         // +0x04
};
static_assert(sizeof(RPI) == 12, "RPI");

// ReliablePendingPacket, 0x104 bytes (Pool<ReliablePendingPacket>)
struct RPP {
    uint8_t pkt[0xe4];       // +0x000  the packet: [0] class << 4 | flags, [1] its sequence id
    int32_t len;             // +0x0e4
    RPP* next;               // +0x0e8
    RPI rpi;                 // +0x0ec  (a received one: the sender's address here instead)
    int32_t sent;            // +0x0f8  PTimeNow at the last send
    int32_t first_sent;      // +0x0fc  PTimeNow at the first send
    int32_t sends;           // +0x100
};
static_assert(sizeof(RPP) == 0x104 && offsetof(RPP, next) == 0xe8 && offsetof(RPP, sends) == 0x100, "RPP");

struct DataPort {
    void** vtable;           // +0x00  0x4df880
    void* sock;              // +0x04  ISocket*
    DataModerator* mod;      // +0x08
};
static_assert(sizeof(DataPort) == 0xc, "DataPort");

struct HostEntry;
struct ReliableDataPort {
    DataPort dp;             // +0x00  vtable 0x4df898
    RPP* ready;              // +0x0c  received packets ready to hand out (in order)
    PoolBase rpp_pool;       // +0x10  Pool<ReliablePendingPacket> (vtable 0x4df8ac): n * 6 of 0x104
    PoolBase node_pool;      // +0x30  Pool<ReliableDataPort::node> (vtable 0x4df894): n * 2 HostEntries of 0x38
    HostEntry* hosts;        // +0x50
    HostEntry* dead;         // +0x54  hosts that timed out, destroyed at the next Tick
    void* timeout_cb;        // +0x58  void (*)(socket_addr const*, generic_data const*)
    uint32_t timeout_data;   // +0x5c
};
static_assert(sizeof(ReliableDataPort) == 0x60, "ReliableDataPort");

// ---- hostent.obj: HostEntry (a ReliableDataPort::node), 0x38 bytes ----------------------------------------------------------
struct HostEntry {
    int32_t slot;            // +0x00  get_slot
    SockAddr addr;           // +0x04
    ReliableDataPort* port;  // +0x10
    RPP** ready;             // +0x14  &port->ready
    int32_t rtt_n;           // +0x18  round trips measured (1 at first)
    int32_t rtt_sum;         // +0x1c  their sum, ms (1000 at first)
    RPP* sent;               // +0x20  sent, waiting for their ack
    RPP* waiting;            // +0x24  not sent yet (at most 2 unacked)
    uint8_t send_id;         // +0x28
    uint8_t send_first;      // +0x29  the next send starts a stream (class 4)
    uint8_t _2a[2];
    RPP* held;               // +0x2c  received out of order, by id
    uint8_t recv_id;         // +0x30
    uint8_t recv_first;      // +0x31
    uint8_t _32[2];
    HostEntry* next;         // +0x34  (the node's link)
};
static_assert(sizeof(HostEntry) == 0x38 && offsetof(HostEntry, held) == 0x2c && offsetof(HostEntry, next) == 0x34,
              "HostEntry");

// ---- session.obj ----------------------------------------------------------------------------------------------------------
// SessionData (Pool<SessionData>, 0xf4 bytes): a channel's received packet, in two lists
struct SessionData {
    SessionData* next;       // +0x00  all sessions' packets, in arrival order (SessionMgr +0x64)
    SessionData* snext;      // +0x04  this session's
    int32_t sess;            // +0x08
    int32_t len;             // +0x0c
    uint8_t data[0xe4];      // +0x10
};
static_assert(sizeof(SessionData) == 0xf4, "SessionData");

// SessionInfo, 0x1c bytes: one channel
struct SessionInfo {
    uint32_t service;        // +0x00  the local service it's bound to (0: free)
    SessionData* queue;      // +0x04
    int32_t reason;          // +0x08  why it went down (GetDiscReason)
    SockAddr addr;           // +0x0c
    uint8_t remote;          // +0x18  the other end's channel number
    uint8_t flags;           // +0x19  1 connected, 2/4/8 down/closing, 0x10 connecting, 0x80 news for GetNextStatus
    uint8_t _1a[2];
};
static_assert(sizeof(SessionInfo) == 0x1c, "SessionInfo");

// RemoteService, 0x54 bytes: a service found (ServiceInfo's 64 bytes from 4, then where it is)
struct RemoteService {
    uint8_t info[0x28];      // +0x00  name (32), type +0x20, version +0x24
    uint32_t id;             // +0x28
    uint8_t info2[0x14];     // +0x2c  password flag, range, sessions, stats (make_servinfo_packet's)
    int32_t latency;         // +0x40  GetEstRTLatency to it
    SockAddr addr;           // +0x44
    uint8_t fresh, _51[3];   // +0x50  answered the latest request
};
static_assert(sizeof(RemoteService) == 0x54 && offsetof(RemoteService, addr) == 0x44, "RemoteService");

// LocalService, 0x48 bytes: a service offered
struct LocalService {
    uint8_t name[0x20];      // +0x00
    uint32_t type;           // +0x20
    uint32_t version;        // +0x24
    uint32_t id;             // +0x28
    uint8_t has_password;    // +0x2c
    uint8_t _2d[3];
    int16_t lo, hi;          // +0x30  its sessions: [lo, hi)
    char password[0x10];     // +0x34
    uint8_t refuse;          // +0x44  RefuseConnections
    uint8_t _45[3];
};
static_assert(sizeof(LocalService) == 0x48 && offsetof(LocalService, lo) == 0x30 && offsetof(LocalService, refuse) == 0x44,
              "LocalService");

// AsyncServiceRequestBlock (the caller's): what the async lookup needs (Socket's AsyncSocketRequestBlock first)
enum : uint32_t { ASRB_STATE = 0x04, ASRB_ADDR = 0x08, ASRB_PORT = 0x0c, ASRB_TYPE = 0x414, ASRB_MSG = 0x418 };

struct SessionMgr {
    int32_t status_at;           // +0x00  GetNextStatus's round robin (-1)
    int32_t request_time;        // +0x04  the last broadcast request's PTimeNow
    uint8_t* async;              // +0x08  the pending AsyncServiceRequestBlock
    int16_t async_port;          // +0x0c
    uint8_t _0e[2];
    uint32_t request_type;       // +0x10
    SessionInfo* sessions;       // +0x14
    int32_t nsessions;           // +0x18
    RemoteService* remote;       // +0x1c
    int32_t nremote_max;         // +0x20
    int32_t nremote;             // +0x24
    LocalService* local;         // +0x28
    int32_t nlocal_max;          // +0x2c
    int32_t nlocal;              // +0x30
    char password[0x10];         // +0x34
    PoolBase data_pool;          // +0x44  Pool<SessionData> (vtable 0x4df5b0)
    SessionData* data;           // +0x64  every session's received packets
    ReliableDataPort rdp;        // +0x68
    void* sock;                  // +0xc8  ISocket*
    uint8_t ok, _cd[3];          // +0xcc
    int32_t dropped;             // +0xd0  stat_tag: packets dropped (no pool entry)
    int32_t task;                // +0xd4  the task that made it (TaskGetID)
};
static_assert(sizeof(SessionMgr) == 0xd8 && offsetof(SessionMgr, data_pool) == 0x44 && offsetof(SessionMgr, rdp) == 0x68 &&
              offsetof(SessionMgr, sock) == 0xc8, "SessionMgr");

// ---- nettypes.obj -----------------------------------------------------------------------------------------------------------
// CarNetPacket, 23 bytes on the wire: [0] u16 time (PTime, low 16 bits), [2] NetFrame (pos 12, rot 6 = 3 x u16), [0x14]
// rpm (u8, x 8000 / 255), [0x15] gear (bits 0-2, 7 = reverse) | steering (bits 3-6: |s| x 15, bit 7 its sign), [0x16]
// braking (bit 0), horn (1), teleported (2), wheels broken (3-6); bit 7 never written (stack garbage)
// CarPhysicsPacket: phys_netcar.cpp's (0x3e bytes, packed)
}  // namespace netc
