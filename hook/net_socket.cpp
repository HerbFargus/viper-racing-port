// net_socket.cpp -- multiplayer stage N1, group A: the transports behind ISocket, rewritten (library `multi`: socket.obj).
//
//   UDPSocket: the LAN and Internet game. Non-blocking, SO_BROADCAST, bound to its port (2001 for host and client alike);
//   Send / Recv are sendto / recvfrom; a host is looked up by address or name (GetHostAddr: inet_addr, gethostbyname),
//   or asynchronously (AsyncGetHostAddr: WSAAsyncGetHostByName, answered through the window: async_msg_hook, which
//   winsock_grab registers, fills the request block in). init_local_addrs keeps this host's addresses (AddressIsLocal).
//   winsock_grab / winsock_release count the sockets: the first starts winsock 1.1 and turns the system's autodial off
//   (set_autodial: a registry value), the last cleans up and turns it back on.
//   IPXSocket: the same over IPX (fails on modern Windows: kept as it is).
//   LineSocket: a serial or modem line (line.obj's ILineDevice) through a LinePacketizer; it has two addresses, its own
//   random id and the other end (~id); a packet to itself (or a broadcast) is queued locally (8 slots).
//   SocketCreateUDP / IPX / Line: allocate, construct, and delete again unless Ok.
//
// Written from the v1.0 disassembly: every call in the original's order, by its v1.0 address (socket.obj's own too, so a
// hooked rewrite or the original is what runs) or through the vtable as the original makes it (a vtable the original
// keeps in a register across a call is read once, as there). winsock is called through race.exe's own import slots, read
// at the call (N0's net_wsock.cpp points them at the session recorder; nothing here calls winsock directly). Two of N0's
// patched call sites are in these functions: LineSocket's constructor draws its id through net_Random (k_random_sites
// 0x4ad4df) and winsock_grab registers the hook net_async_hook_for_winsock_grab names (PUSH_ASYNC_HOOK 0x4adb11: N0's
// h_async_hook in a session, async_msg_hook otherwise). The stack locals the original hands winsock half-written (a
// sockaddr_in's sin_zero, which N0's recorder masks) are left unwritten here too. Faithful: init_local_addrs writes past
// its three slots on a host with more than three IPv4 addresses, as the original does (a FIX candidate, not applied).
//
// Footprints: whatever calls winsock, the registry, allocates or frees, or logs on every call is replay_only (a session
// replay of a LAN game is their in-game check: the recorder hands the rewrites what winsock gave the original); the
// accessors and the address compares are pure or write only their output. test/world_net_line.cpp checks them all
// offline against the originals on a fake winsock.
//
// Also here: Socket::AsyncCancel (ret 4), UDPSocket / IPXSocket::LinkAlive (always 1) and the five deleting destructors
// (LineSocket, Socket, ISocket, UDPSocket, IPXSocket). Left to tools/gen_leftovers.py --lib multi (group B's
// net_leftover.cpp): socket.obj's $E initialisers.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "net_types.h"
#include "net_wsock.h"

namespace {
namespace net_socket {
using namespace nt;

#define WS(T, slot) NT_IAT(T, slot)
static __forceinline const char* err2str() { return ccall<const char*>(A_err2str0); }

#pragma pack(push, 1)
struct SockaddrIn {                    // a sockaddr_in as the original builds it (zero[] never written)
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};
struct SockaddrIpx {                   // a sockaddr_ipx (14 bytes) and the two the stack has after it
    uint16_t family;
    uint8_t net[4];
    uint8_t node[6];
    uint16_t socket;
    uint8_t after[2];
};
struct IpxAddressData {                // IPX_ADDRESS_DATA (getsockopt IPX_ADDRESS)
    int32_t adapter;
    uint8_t net[4];
    uint8_t node[6];
    uint8_t wan, status;
    int32_t maxpkt, linkspeed;
};
#pragma pack(pop)
static_assert(sizeof(SockaddrIn) == 16 && sizeof(SockaddrIpx) == 16 && sizeof(IpxAddressData) == 0x18, "winsock locals");

template <typename T> static void fp_pure(Footprint& f, T*, Edx) { f.pure = true; }
template <typename T> static void fp_pure_addr(Footprint& f, T*, Edx, const SocketAddr*) { f.pure = true; }
// writes nothing, but walks its count of local addresses (not fuzzed: a random count runs off any arena)
template <typename T> static void fp_reads_addr(Footprint&, T*, Edx, const SocketAddr*) {}
template <typename T> static void fp_out_addr(Footprint& f, T*, Edx, SocketAddr* a) {
    f.pure = true;
    f.add(a, sizeof(SocketAddr), "the address");
}

// =========================================================================================================================
// LineSocket
// =========================================================================================================================
static LineSocket* __fastcall LineSocket_ctor(LineSocket* self, Edx, void* dev) {
    self->vtbl = (const void*)(uintptr_t)VT_Socket;
    for (int32_t i = 0; i < 8; i++) tcall<void*>(A_local_pkt_ctor, &self->pkts[i]);
    self->vtbl = (const void*)(uintptr_t)VT_LineSocket;
    self->head = 0;
    self->dev = dev;
    void* p = ccall<void*>(F_MemAlloc, (int32_t)0x14);
    self->pk = p ? tcall<LinePacketizer*>(A_LinePacketizer_ctor, p, dev) : 0;
    LinePacketizer* pk = self->pk;
    if (!pk) return self;
    if (pk->dev == 0) {
        tcall<void>(A_LinePacketizer_dtor, pk);
        ccall<void>(F_op_delete, (void*)pk);
        self->pk = 0;
        return self;
    }
    int32_t id;
    do {
        id = net_Random(10000000);                  // N0's site 0x4ad4df
        self->id = (uint32_t)id;
    } while (id == 0 || id == -1);
    return self;
}
static void fp_LineSocket_ctor(Footprint& f, LineSocket*, Edx, void*) { f.replay_only = "allocates its packetizer"; }
PORT_FN(0x004ad450, "LineSocket::LineSocket", LineSocket_ctor, fp_LineSocket_ctor)

static void __fastcall LineSocket_dtor(LineSocket* self, Edx) {
    LinePacketizer* pk = self->pk;
    self->vtbl = (const void*)(uintptr_t)VT_LineSocket;
    self->dev = 0;
    if (pk) {
        tcall<void>(A_LinePacketizer_dtor, pk);
        ccall<void>(F_op_delete, (void*)pk);
    }
    self->pk = 0;
    self->vtbl = (const void*)(uintptr_t)VT_ISocket;
}
static void fp_LineSocket_dtor(Footprint& f, LineSocket*, Edx) { f.replay_only = "frees its packetizer and line device"; }
PORT_FN(0x004ad500, "LineSocket::~LineSocket", LineSocket_dtor, fp_LineSocket_dtor)

// alloc_pkt: the first free local packet (length 0), or 0
static LocalPkt* __fastcall LineSocket_alloc_pkt(LineSocket* self, Edx) {
    for (int32_t i = 0; i < 8; i++)
        if (*(volatile int32_t*)&self->pkts[i].len == 0) return &self->pkts[i];
    return 0;
}
static void fp_LineSocket_alloc_pkt(Footprint&, LineSocket*, Edx) {}   // (writes nothing; not pure: it returns a pointer into this)
PORT_FN(0x004ad540, "LineSocket::alloc_pkt", LineSocket_alloc_pkt, fp_LineSocket_alloc_pkt)

// Send(p, n, to): on a live link, to the other end unless it's to ourselves; to ourselves or a broadcast, queued here too
static void __fastcall LineSocket_Send(LineSocket* self, Edx, const void* p, int32_t n, const SocketAddr* to) {
    if (!vcall<uint8_t>(self, SO_LINK_ALIVE)) return;
    if (to->d[0] != self->id) tcall<void>(A_LinePacketizer_Send, self->pk, p, n);
    const uint32_t a = *(const volatile uint32_t*)&to->d[0];
    if (self->id == a || a == 0xffffffffu) tcall<void>(A_LineSocket_enqueue, self, p, n);
}
static void fp_LineSocket_Send(Footprint& f, LineSocket*, Edx, const void*, int32_t, const SocketAddr*) {
    f.replay_only = "sends on the line";
}
PORT_FN(0x004ad570, "LineSocket::Send", LineSocket_Send, fp_LineSocket_Send)

// Recv(buf, &n, from): a packet from the line (from = ~id), else one queued for ourselves (from = id)
static uint8_t __fastcall LineSocket_Recv(LineSocket* self, Edx, void* buf, int32_t* n, SocketAddr* from) {
    if (!vcall<uint8_t>(self, SO_LINK_ALIVE)) return 0;
    if (from) {
        from->d[0] = 0;
        from->d[1] = 0;
        from->d[2] = 0;
    }
    if (tcall<uint8_t>(A_LinePacketizer_Recv, self->pk, buf, n)) {
        if (from) from->d[0] = ~self->id;
        return 1;
    }
    if (from) from->d[0] = self->id;
    return tcall<uint8_t>(A_LineSocket_dequeue, self, buf, n);
}
static void fp_LineSocket_Recv(Footprint& f, LineSocket*, Edx, void*, int32_t*, SocketAddr*) { f.replay_only = "reads the line"; }
PORT_FN(0x004ad5d0, "LineSocket::Recv", LineSocket_Recv, fp_LineSocket_Recv)

static void __fastcall LineSocket_MakeStr(LineSocket* self, Edx, const SocketAddr* a, char* out) {
    const uint32_t v = a->d[0];
    if (self->id == v) crt_strcpy(out, NT_CP(0x004fe20c));          // "<local>"
    else if (v == 0xffffffffu) crt_strcpy(out, NT_CP(0x004fe214));  // "<broadcast>"
    else crt_strcpy(out, NT_CP(0x004fe220));                        // "<remote>"
}
static void fp_LineSocket_MakeStr(Footprint& f, LineSocket*, Edx, const SocketAddr*, char* out) { f.add(out, 12, "the string"); }
PORT_FN(0x004ad650, "LineSocket::MakeStr", LineSocket_MakeStr, fp_LineSocket_MakeStr)

// dequeue(buf, &n): the oldest local packet (its length unchecked against the caller's buffer), freed
static uint8_t __fastcall LineSocket_dequeue(LineSocket* self, Edx, void* buf, int32_t* n) {
    LocalPkt* p = self->head;
    if (!p) return 0;
    crt_copy(buf, p->data, (uint32_t)p->len);
    *n = self->head->len;
    LocalPkt* h = self->head;
    self->head = h->next;
    h->len = 0;
    return 1;
}
static void fp_LineSocket_dequeue(Footprint& f, LineSocket* self, Edx, void* buf, int32_t* n) {
    f.add(self, sizeof(LineSocket), "the socket");
    f.add(n, 4, "the length");
    if (self->head && self->head->len > 0) f.add(buf, (uint32_t)self->head->len, "the buffer");
}
PORT_FN(0x004ad6f0, "LineSocket::dequeue", LineSocket_dequeue, fp_LineSocket_dequeue)

// enqueue(p, n): into a free slot (its length unchecked against the slot's 0xe4), at the end of the queue; none free panics
static void __fastcall LineSocket_enqueue(LineSocket* self, Edx, const void* p, int32_t n) {
    LocalPkt* k = tcall<LocalPkt*>(A_LineSocket_alloc_pkt, self);
    if (!k) {
        NT_LogPanic(NT_CP(0x004fe22c), (int32_t)8);
        return;
    }
    crt_copy(k->data, p, (uint32_t)n);
    k->len = n;
    k->next = 0;
    LocalPkt* volatile* at = &self->head;
    while (*at) at = &(*at)->next;
    *at = k;
}
static void fp_LineSocket_enqueue(Footprint& f, LineSocket* self, Edx, const void*, int32_t) {
    bool free = false;
    for (int i = 0; i < 8; i++) free |= self->pkts[i].len == 0;
    if (!free) f.replay_only = "panics (no free local packet)";
    f.add(self, sizeof(LineSocket), "the socket");
}
PORT_FN(0x004ad760, "LineSocket::enqueue", LineSocket_enqueue, fp_LineSocket_enqueue)

static uint8_t __fastcall LineSocket_Ok(LineSocket* self, Edx) { return self->pk != 0 ? 1 : 0; }
PORT_FN(0x004ae460, "LineSocket::Ok", LineSocket_Ok, fp_pure<LineSocket>)

static uint8_t __fastcall LineSocket_GetHostAddr(LineSocket*, Edx, const char*, SocketAddr*) {
    NT_LogPanic(NT_CP(0x004fe51c));
    return 0;
}
static void fp_LineSocket_GetHostAddr(Footprint& f, LineSocket*, Edx, const char*, SocketAddr*) { f.replay_only = "panics"; }
PORT_FN(0x004ae470, "LineSocket::GetHostAddr", LineSocket_GetHostAddr, fp_LineSocket_GetHostAddr)

static uint8_t __fastcall LineSocket_AddressIsLocal(LineSocket* self, Edx, const SocketAddr* a) {
    return self->id - a->d[0] == 0 ? 1 : 0;
}
PORT_FN(0x004ae490, "LineSocket::AddressIsLocal", LineSocket_AddressIsLocal, fp_pure_addr<LineSocket>)

static void __fastcall LineSocket_GetBroadcastAddr(LineSocket*, Edx, SocketAddr* a) {
    a->d[0] = 0;
    a->d[1] = 0;
    a->d[2] = 0;
    a->d[0] = 0xffffffffu;
}
PORT_FN(0x004ae4b0, "LineSocket::GetBroadcastAddr", LineSocket_GetBroadcastAddr, fp_out_addr<LineSocket>)

static void __fastcall LineSocket_GetMyAddr(LineSocket* self, Edx, SocketAddr* a) {
    a->d[0] = 0;
    a->d[1] = 0;
    a->d[2] = 0;
    a->d[0] = self->id;
}
PORT_FN(0x004ae4d0, "LineSocket::GetMyAddr", LineSocket_GetMyAddr, fp_out_addr<LineSocket>)

static int32_t __fastcall LineSocket_GetBPS(LineSocket* self, Edx) { return vcall<int32_t>(self->dev, LD_BPS); }
static void fp_LineSocket_GetBPS(Footprint&, LineSocket*, Edx) {}
PORT_FN(0x004ae4f0, "LineSocket::GetBPS", LineSocket_GetBPS, fp_LineSocket_GetBPS)

static int32_t __fastcall LineSocket_GetHeaderSize(LineSocket* self, Edx) {
    return tcall<int32_t>(A_LinePacketizer_GetHeaderSize, self->pk);
}
static void fp_LineSocket_GetHeaderSize(Footprint&, LineSocket*, Edx) {}
PORT_FN(0x004ae500, "LineSocket::GetHeaderSize", LineSocket_GetHeaderSize, fp_LineSocket_GetHeaderSize)

static uint8_t __fastcall LineSocket_LinkAlive(LineSocket* self, Edx) {
    return vcall<int32_t>(self->dev, LD_GET_STATUS) - 2 == 0 ? 1 : 0;
}
static void fp_LineSocket_LinkAlive(Footprint& f, LineSocket*, Edx) {
    f.replay_only = "the device's GetStatus reads the COM port's modem lines (DirectLine)";
}
PORT_FN(0x004ae510, "LineSocket::LinkAlive", LineSocket_LinkAlive, fp_LineSocket_LinkAlive)

static LocalPkt* __fastcall local_pkt_ctor(LocalPkt* self, Edx) {
    self->len = 0;
    return self;
}
static void fp_local_pkt_ctor(Footprint& f, LocalPkt* self, Edx) {
    // (not pure: it returns this, which the fuzzer would compare as a value)
    f.add(self, sizeof(LocalPkt), "the packet");
}
PORT_FN(0x004ae530, "LineSocket::local_pkt::local_pkt", local_pkt_ctor, fp_local_pkt_ctor)

// =========================================================================================================================
// UDPSocket
// =========================================================================================================================
static void __fastcall UDPSocket_AsyncCancel(UDPSocket* self, Edx, AsyncReq* rq) {
    NT_GU32(S_ASYNC_REQS + (uint32_t)self->slot * 4) = 0;
    WS(Int1_f, I_WSACancelAsyncRequest)(rq->handle);
    rq->handle = 0xffffffffu;
}
static void fp_UDPSocket_AsyncCancel(Footprint& f, UDPSocket*, Edx, AsyncReq*) { f.replay_only = "cancels a winsock request"; }
PORT_FN(0x004ad7d0, "UDPSocket::AsyncCancel", UDPSocket_AsyncCancel, fp_UDPSocket_AsyncCancel)

// AsyncGetHostAddr(name, rq): an address is answered at once (status 2); a name is asked of winsock, the answer posted
// to the window as message 0xf400 + slot (async_msg_hook); a failure to ask is logged (status 1). Always 1.
static uint8_t __fastcall UDPSocket_AsyncGetHostAddr(UDPSocket* self, Edx, const char* name, AsyncReq* rq) {
    rq->addr.d[0] = 0;
    rq->addr.d[1] = 0;
    rq->addr.d[2] = 0;
    rq->addr.w[2] = (uint16_t)self->port;
    const uint32_t ip = WS(InetAddr_f, I_inet_addr)(name);
    if (ip != 0xffffffffu) {
        rq->addr.d[0] = ip;
        rq->status = 2;
        return 1;
    }
    const uint32_t msg = (uint32_t)self->slot + 0xf400;
    void* w = ccall<void*>(F_Win32GetWindow);
    const uint32_t h = WS(AsyncHost_f, I_WSAAsyncGetHostByName)(w, msg, name, rq->hostent, 0x400);
    rq->handle = h;
    if (h) {
        rq->status = 0;
        NT_GU32(S_ASYNC_REQS + (uint32_t)self->slot * 4) = (uint32_t)(uintptr_t)rq;
        return 1;
    }
    rq->status = 1;
    NT_LogReport(NT_CP(0x004fe260), err2str());
    return 1;
}
static void fp_UDPSocket_AsyncGetHostAddr(Footprint& f, UDPSocket*, Edx, const char*, AsyncReq*) { f.replay_only = "asks winsock"; }
PORT_FN(0x004ad800, "UDPSocket::AsyncGetHostAddr", UDPSocket_AsyncGetHostAddr, fp_UDPSocket_AsyncGetHostAddr)

static const char* __cdecl err2str0_c() { return ccall<const char*>(A_err2str1, WS(Int0_f, I_WSAGetLastError)()); }
static void fp_err2str0(Footprint& f) { f.replay_only = "reads WSAGetLastError"; }
PORT_FN(0x004ad8b0, "err2str", err2str0_c, fp_err2str0)

static const char* __cdecl err2str1_c(int32_t e) {
    NT_sprintf((char*)(uintptr_t)S_ERRBUF, NT_CP(0x004fe27c), e);       // "0x%x"
    return NT_CP(S_ERRBUF);
}
static void fp_err2str1(Footprint& f, int32_t) { f.add((void*)(uintptr_t)S_ERRBUF, 0x20, "err2str's buffer"); }
PORT_FN(0x004ad8c0, "err2str(int)", err2str1_c, fp_err2str1)

// init_local_addrs: this host's addresses from gethostbyname(gethostname()) -- every one, into slots for three
static void __fastcall UDP_init_local_addrs(UDPSocket* self, Edx) {
    char name[0x80];
    const uintptr_t me = (uintptr_t)self;
    self->nlocal = 0;
    crt_zero(self->local, 9);
    if (WS(Hostname_f, I_gethostname)(name, 0x80) != 0) {
        self->ok = 0;
        return;
    }
    const uint8_t* h = (const uint8_t*)WS(Hostbyname_f, I_gethostbyname)(name);
    if (!h) {
        self->ok = 0;
        return;
    }
    uint32_t* const* list = *(uint32_t* const* const*)(h + 0xc);
    if (*(uint32_t* const volatile*)list == 0) return;
    do {
        const uint16_t port = NT_G16(me + 0x30);
        int32_t n = NT_G32(me + 0x2c);
        NT_G8(me + 0x32) = 1;
        NT_G16(me + (uint32_t)n * 12 + 0xc) = port;            // (past local[2]: the port, then the count)
        list++;
        const uint32_t ip = *list[-1];
        n = NT_G32(me + 0x2c);
        NT_GU32(me + (uint32_t)n * 12 + 8) = ip;
        NT_G32(me + 0x2c) = NT_G32(me + 0x2c) + 1;
    } while (*(uint32_t* const volatile*)list);
}
static void fp_UDP_init_local_addrs(Footprint& f, UDPSocket*, Edx) { f.replay_only = "asks winsock (gethostname)"; }
PORT_FN(0x004ad8e0, "UDPSocket::init_local_addrs", UDP_init_local_addrs, fp_UDP_init_local_addrs)

// UDPSocket(port): winsock grabbed, a datagram socket bound to the port, broadcast and non-blocking; each failure logged
static UDPSocket* __fastcall UDPSocket_ctor(UDPSocket* self, Edx, uint32_t port) {
    self->vtbl = (const void*)(uintptr_t)VT_Socket;
    self->port = (int16_t)port;
    self->vtbl = (const void*)(uintptr_t)VT_UDPSocket;
    self->sock = 0xffffffffu;
    self->ok = 0;
    const int32_t slot = NT_G32(S_GRABS);
    self->slot = slot;
    NT_GU32(S_ASYNC_REQS + (uint32_t)slot * 4) = 0;
    if (!ccall<uint8_t>(A_winsock_grab)) return self;
    const uint32_t s = WS(Socket_f, I_socket)(2, 2, 0);
    self->sock = s;
    if (s == 0xffffffffu) {
        NT_LogReport(NT_CP(0x004fe2e0), err2str());
        return self;
    }
    SockaddrIn sa;
    sa.addr = 0;
    sa.family = 2;
    sa.port = WS(Short_f, I_htons)((uint32_t)(uint16_t)self->port);
    if (WS(Bind_f, I_bind)(self->sock, &sa, 0x10) != 0) {
        NT_LogReport(NT_CP(0x004fe2cc), err2str());
        return self;
    }
    uint32_t one = 1;
    if (WS(Setsockopt_f, I_setsockopt)(self->sock, 0xffff, 0x20, &one, 4) != 0) {
        NT_LogReport(NT_CP(0x004fe2a8), err2str());
        return self;
    }
    one = 1;
    if (WS(Ioctl_f, I_ioctlsocket)(self->sock, 0x8004667e, &one) != 0) {
        NT_LogReport(NT_CP(0x004fe284), err2str());
        return self;
    }
    self->ok = 1;
    tcall<void>(A_UDP_init_local_addrs, self);
    return self;
}
static void fp_UDPSocket_ctor(Footprint& f, UDPSocket*, Edx, uint32_t) { f.replay_only = "opens a socket"; }
PORT_FN(0x004ad970, "UDPSocket::UDPSocket", UDPSocket_ctor, fp_UDPSocket_ctor)

// winsock_grab: the first starts winsock 1.1, turns autodial off and registers the async lookups' message hook
static uint8_t __cdecl winsock_grab_c() {
    uint8_t wsadata[0x190];
    const int32_t c = NT_G32(S_GRABS);
    NT_G32(S_GRABS) = c + 1;
    if (c == 0) {
        const int r = WS(WSAStartup_f, I_WSAStartup)(0x101, wsadata);
        NT_G8(S_WSA_OK) = (uint32_t)r < 1u ? 1 : 0;
        NT_G8(S_AUTODIAL) = ccall<uint8_t>(A_set_autodial, (uint32_t)0);
        ccall<void>(F_Win32RegisterMessageHook, net_async_hook_for_winsock_grab());   // N0's site 0x4adb11
    }
    return NT_G8(S_WSA_OK);
}
static void fp_winsock_grab(Footprint& f) { f.replay_only = "starts winsock"; }
PORT_FN(0x004adad0, "winsock_grab", winsock_grab_c, fp_winsock_grab)

// set_autodial(on): HKEY_USERS\.Default\...\Internet Settings EnableAutoDial (REG_BINARY) set to `on` if it isn't;
// 1 if it was changed
static uint8_t __cdecl set_autodial_c(uint32_t on) {
    uint32_t data, hkey, cb, type;
    uint8_t r = 0;
    if (NT_IAT(RegOpenKeyExA_f, I_RegOpenKeyExA)(0x80000003u, NT_CP(NT_GU32(S_AUTODIAL_KEY)), 0, 0x2001f, &hkey) != 0) return 0;
    const char* value = NT_CP(NT_GU32(S_AUTODIAL_VALUE));
    cb = 4;
    if (NT_IAT(RegQueryValueExA_f, I_RegQueryValueExA)(hkey, value, 0, &type, &data, &cb) == 0) {
        if (type == 3) {
            const uint32_t want = (uint8_t)on;
            if (data != want) {
                data = want;
                if (NT_IAT(RegSetValueExA_f, I_RegSetValueExA)(hkey, value, 0, 3, &data, cb) == 0) r = 1;
                else NT_LogReport(NT_CP(0x004fe2f8), ccall<const char*>(F_Win32GetErrorString0));
            }
        } else {
            NT_LogReport(NT_CP(0x004fe328), type);
        }
    }
    NT_IAT(RegCloseKey_f, I_RegCloseKey)(hkey);
    return r;
}
static void fp_set_autodial(Footprint& f, uint32_t) { f.replay_only = "reads and writes the registry"; }
PORT_FN(0x004adb30, "set_autodial", set_autodial_c, fp_set_autodial)

// async_msg_hook(msg, wparam, lparam): the reply to AsyncGetHostAddr's request (message 0xf400 + slot): the first
// address (status 2) or the error (status 1), logged; the slot freed. Other messages: 0.
static uint8_t __cdecl async_msg_hook_c(uint32_t msg, int32_t wparam, int32_t lparam) {
    (void)wparam;
    const uint32_t i = msg - 0xf400;
    if (i >= 4) return 0;
    const uint32_t slot = S_ASYNC_REQS + i * 4;
    AsyncReq* rq = (AsyncReq*)(uintptr_t)NT_GU32(slot);
    const uint16_t err = (uint16_t)((uint32_t)lparam >> 16);
    if (err == 0) {
        NT_LogReport(NT_CP(0x004fe340), (uint32_t)(uint16_t)lparam);
        rq->status = 2;
        uint32_t* const* list = *(uint32_t* const* const*)(rq->hostent + 0xc);
        rq->addr.d[0] = *list[0];
    } else {
        const char* e = ccall<const char*>(A_err2str1, (uint32_t)err);
        NT_LogReport(NT_CP(0x004fe358), e);
        rq->status = 1;
    }
    NT_GU32(slot) = 0;
    return 1;
}
static void fp_async_msg_hook(Footprint& f, uint32_t msg, int32_t, int32_t) {
    if (msg - 0xf400u < 4u) f.replay_only = "takes winsock's reply (logged)";
}
PORT_FN(0x004adc00, "async_msg_hook", async_msg_hook_c, fp_async_msg_hook)

static void __fastcall UDPSocket_dtor(UDPSocket* self, Edx) {
    const uint32_t s = self->sock;
    self->vtbl = (const void*)(uintptr_t)VT_UDPSocket;
    if (s != 0xffffffffu) {
        if (WS(Int1_f, I_closesocket)(s) != 0) NT_LogReport(NT_CP(0x004fe36c), err2str());
        self->sock = 0xffffffffu;
    }
    ccall<void>(A_winsock_release);
    self->ok = 0;
    self->vtbl = (const void*)(uintptr_t)VT_ISocket;
}
static void fp_UDPSocket_dtor(Footprint& f, UDPSocket*, Edx) { f.replay_only = "closes the socket"; }
PORT_FN(0x004adc80, "UDPSocket::~UDPSocket", UDPSocket_dtor, fp_UDPSocket_dtor)

// winsock_release: the last cleans winsock up, turns autodial back on if grab turned it off, and unregisters the hook
// (async_msg_hook: N0 doesn't patch this push, so in a session its h_async_hook stays registered, as with the original)
static void __cdecl winsock_release_c() {
    const int32_t c = NT_G32(S_GRABS) - 1;
    NT_G32(S_GRABS) = c;
    if (c != 0) return;
    WS(Int0_f, I_WSACleanup)();
    if (NT_G8(S_AUTODIAL)) ccall<uint8_t>(A_set_autodial, (uint32_t)1);
    NT_G8(S_AUTODIAL) = 0;
    ccall<void>(F_Win32UnRegisterMessageHook, (uint32_t)A_async_msg_hook);
}
static void fp_winsock_release(Footprint& f) { f.replay_only = "cleans winsock up"; }
PORT_FN(0x004adcd0, "winsock_release", winsock_release_c, fp_winsock_release)

static uint8_t __fastcall UDPSocket_AddressIsLocal(UDPSocket* self, Edx, const SocketAddr* a) {
    for (int32_t i = 0; i < self->nlocal; i++)
        if (a->d[0] == self->local[i].d[0]) return 1;
    return 0;
}
PORT_FN(0x004add10, "UDPSocket::AddressIsLocal", UDPSocket_AddressIsLocal, fp_reads_addr<UDPSocket>)

static void __fastcall UDPSocket_Send(UDPSocket* self, Edx, const void* p, int32_t n, const SocketAddr* to) {
    SockaddrIn sa;
    sa.family = 2;
    sa.addr = to->d[0];
    sa.port = WS(Short_f, I_htons)((uint32_t)to->w[2]);
    if (WS(Sendto_f, I_sendto)(self->sock, p, n, 0, &sa, 0x10) == -1) NT_LogReport(NT_CP(0x004fe388), err2str());
}
static void fp_UDPSocket_Send(Footprint& f, UDPSocket*, Edx, const void*, int32_t, const SocketAddr*) { f.replay_only = "sendto"; }
PORT_FN(0x004add40, "UDPSocket::Send", UDPSocket_Send, fp_UDPSocket_Send)

// Recv(buf, &n, from): one datagram (n in: the buffer's size, out: the length) and its sender; nothing waiting is quiet,
// another error logged
static uint8_t __fastcall UDPSocket_Recv(UDPSocket* self, Edx, void* buf, int32_t* n, SocketAddr* from) {
    int32_t fromlen;
    SockaddrIn sa;
    sa.family = 2;
    fromlen = 0x10;
    const int r = WS(Recvfrom_f, I_recvfrom)(self->sock, buf, *n, 0, &sa, &fromlen);
    if (r == -1) {
        const int e = WS(Int0_f, I_WSAGetLastError)();
        if (e != 10035) NT_LogReport(NT_CP(0x004fe39c), ccall<const char*>(A_err2str1, e));
        return 0;
    }
    *n = r;
    if (from) {
        uint32_t port;
        memcpy(&port, (const uint8_t*)&sa + 2, 4);                   // sin_port and the low half of sin_addr
        from->d[0] = 0;
        from->d[1] = 0;
        from->d[2] = 0;
        from->d[0] = sa.addr;
        from->w[2] = WS(Short_f, I_ntohs)(port);
    }
    return 1;
}
static void fp_UDPSocket_Recv(Footprint& f, UDPSocket*, Edx, void*, int32_t*, SocketAddr*) { f.replay_only = "recvfrom"; }
PORT_FN(0x004addb0, "UDPSocket::Recv", UDPSocket_Recv, fp_UDPSocket_Recv)

// GetHostAddr(name, a): a dotted address, else the name's first address (every one logged); the port is the socket's
static uint8_t __fastcall UDPSocket_GetHostAddr(UDPSocket* self, Edx, const char* name, SocketAddr* a) {
    a->d[0] = 0;
    a->d[1] = 0;
    a->d[2] = 0;
    const uint32_t ip = WS(InetAddr_f, I_inet_addr)(name);
    if (ip != 0xffffffffu) {
        a->d[0] = ip;
        a->w[2] = (uint16_t)self->port;
        return 1;
    }
    const uint8_t* h = (const uint8_t*)WS(Hostbyname_f, I_gethostbyname)(name);
    if (!h) return 0;
    a->d[0] = ***(uint32_t* const* const*)(h + 0xc);
    const uint8_t* const* list = *(const uint8_t* const* const*)(h + 0xc);
    while (*(const uint8_t* const volatile*)list) {
        const uint8_t* b = *list++;
        NT_LogReport(NT_CP(0x004fe3b4), name, (uint32_t)b[0], (uint32_t)b[1], (uint32_t)b[2], (uint32_t)b[3]);
    }
    a->w[2] = (uint16_t)self->port;
    return 1;
}
static void fp_UDPSocket_GetHostAddr(Footprint& f, UDPSocket*, Edx, const char*, SocketAddr*) { f.replay_only = "asks winsock"; }
PORT_FN(0x004ade50, "UDPSocket::GetHostAddr", UDPSocket_GetHostAddr, fp_UDPSocket_GetHostAddr)

static uint8_t __fastcall UDPSocket_Ok(UDPSocket* self, Edx) { return self->ok; }
PORT_FN(0x004ae5a0, "UDPSocket::Ok", UDPSocket_Ok, fp_pure<UDPSocket>)

static void __fastcall UDPSocket_MakeStr(UDPSocket*, Edx, const SocketAddr* a, char* out) {
    NT_sprintf(out, NT_CP(0x004fe54c), (uint32_t)a->b[0], (uint32_t)a->b[1], (uint32_t)a->b[2], (uint32_t)a->b[3]);
}
static void fp_UDPSocket_MakeStr(Footprint& f, UDPSocket*, Edx, const SocketAddr*, char* out) { f.add(out, 16, "the string"); }
PORT_FN(0x004ae5b0, "UDPSocket::MakeStr", UDPSocket_MakeStr, fp_UDPSocket_MakeStr)

static void __fastcall UDPSocket_GetMyAddr(UDPSocket* self, Edx, SocketAddr* a) {
    a->d[0] = self->local[0].d[0];
    a->d[1] = self->local[0].d[1];
    a->d[2] = self->local[0].d[2];
}
PORT_FN(0x004ae5e0, "UDPSocket::GetMyAddr", UDPSocket_GetMyAddr, fp_out_addr<UDPSocket>)

static void __fastcall UDPSocket_GetBroadcastAddr(UDPSocket* self, Edx, SocketAddr* a) {
    a->d[0] = 0;
    a->d[1] = 0;
    a->d[2] = 0;
    a->d[0] = 0xffffffffu;
    a->w[2] = (uint16_t)self->port;
}
PORT_FN(0x004ae600, "UDPSocket::GetBroadcastAddr", UDPSocket_GetBroadcastAddr, fp_out_addr<UDPSocket>)

static int32_t __fastcall UDPSocket_GetHeaderSize(UDPSocket*, Edx) { return 0x26; }
PORT_FN(0x004ae620, "UDPSocket::GetHeaderSize", UDPSocket_GetHeaderSize, fp_pure<UDPSocket>)
static int32_t __fastcall UDPSocket_GetBPS(UDPSocket*, Edx) { return 0xb40; }
PORT_FN(0x004ae630, "UDPSocket::GetBPS", UDPSocket_GetBPS, fp_pure<UDPSocket>)

// =========================================================================================================================
// IPXSocket
// =========================================================================================================================
static IPXSocket* __fastcall IPXSocket_ctor(IPXSocket* self, Edx, uint32_t port) {
    self->port = (int16_t)port;
    self->vtbl = (const void*)(uintptr_t)VT_IPXSocket;
    self->ok = 0;
    if (!ccall<uint8_t>(A_winsock_grab)) {
        NT_LogReport(NT_CP(0x004fe450));
        return self;
    }
    const uint32_t s = WS(Socket_f, I_socket)(6, 2, 0x3e8);
    self->sock = s;
    if (s == 0xffffffffu) {
        NT_LogReport(NT_CP(0x004fe434), err2str());
        return self;
    }
    SockaddrIpx sa;
    sa.family = 6;
    memset(sa.net, 0, 4);
    memset(sa.node, 0, 4);
    memset(sa.node + 4, 0, 2);
    sa.socket = WS(Short_f, I_htons)((uint32_t)(uint16_t)self->port);
    if (WS(Bind_f, I_bind)(self->sock, &sa, 0xe) != 0) {
        NT_LogReport(NT_CP(0x004fe41c), err2str());
        return self;
    }
    uint32_t one = 1;
    if (WS(Setsockopt_f, I_setsockopt)(self->sock, 0xffff, 0x20, &one, 4) != 0) {
        NT_LogReport(NT_CP(0x004fe3f4), err2str());
        return self;
    }
    one = 1;
    if (WS(Ioctl_f, I_ioctlsocket)(self->sock, 0x8004667e, &one) != 0) {
        NT_LogReport(NT_CP(0x004fe3cc), err2str());
        return self;
    }
    self->ok = 1;
    tcall<void>(A_IPX_init_local_addrs, self);
    return self;
}
static void fp_IPXSocket_ctor(Footprint& f, IPXSocket*, Edx, uint32_t) { f.replay_only = "opens a socket"; }
PORT_FN(0x004adf00, "IPXSocket::IPXSocket", IPXSocket_ctor, fp_IPXSocket_ctor)

static void __fastcall IPXSocket_dtor(IPXSocket* self, Edx) {
    const uint32_t s = self->sock;
    self->vtbl = (const void*)(uintptr_t)VT_IPXSocket;
    if (s != 0xffffffffu) {
        if (WS(Int1_f, I_closesocket)(s) != 0) NT_LogReport(NT_CP(0x004fe470), err2str());
        self->sock = 0xffffffffu;
    }
    ccall<void>(A_winsock_release);
    self->ok = 0;
    self->vtbl = (const void*)(uintptr_t)VT_ISocket;
}
static void fp_IPXSocket_dtor(Footprint& f, IPXSocket*, Edx) { f.replay_only = "closes the socket"; }
PORT_FN(0x004ae060, "IPXSocket::~IPXSocket", IPXSocket_dtor, fp_IPXSocket_dtor)

static uint8_t __fastcall IPXSocket_AddressIsLocal(IPXSocket* self, Edx, const SocketAddr* a) {
    for (int32_t i = 0; i < self->nlocal; i++)
        if (memcmp(a, &self->local[i], 10) == 0) return 1;
    return 0;
}
PORT_FN(0x004ae0b0, "IPXSocket::AddressIsLocal", IPXSocket_AddressIsLocal, fp_reads_addr<IPXSocket>)

static void __fastcall IPXSocket_Send(IPXSocket* self, Edx, const void* p, int32_t n, const SocketAddr* to) {
    SockaddrIpx sa;
    sa.family = 6;
    memcpy(sa.net, &to->d[0], 4);
    memcpy(sa.node, &to->d[1], 4);
    memcpy(sa.node + 4, &to->w[4], 2);
    uint32_t port;
    memcpy(&port, &to->w[5], 2);
    port &= 0xffff;
    sa.socket = WS(Short_f, I_htons)(port);
    if (WS(Sendto_f, I_sendto)(self->sock, p, n, 0, &sa, 0xe) == -1) NT_LogReport(NT_CP(0x004fe48c), err2str());
}
static void fp_IPXSocket_Send(Footprint& f, IPXSocket*, Edx, const void*, int32_t, const SocketAddr*) { f.replay_only = "sendto"; }
PORT_FN(0x004ae0f0, "IPXSocket::Send", IPXSocket_Send, fp_IPXSocket_Send)

static uint8_t __fastcall IPXSocket_Recv(IPXSocket* self, Edx, void* buf, int32_t* n, SocketAddr* from) {
    int32_t fromlen;
    SockaddrIpx sa;
    sa.family = 6;
    fromlen = 0xe;
    const int r = WS(Recvfrom_f, I_recvfrom)(self->sock, buf, *n, 0, &sa, &fromlen);
    if (r == -1) {
        const int e = WS(Int0_f, I_WSAGetLastError)();
        if (e != 10035) NT_LogReport(NT_CP(0x004fe4a4), ccall<const char*>(A_err2str1, e));
        return 0;
    }
    *n = r;
    if (from) {
        uint32_t node, net, sock;
        uint16_t node45;
        memcpy(&node, sa.node, 4);
        from->d[0] = 0;
        from->d[1] = 0;
        from->d[2] = 0;
        memcpy(&net, sa.net, 4);
        memcpy(&node45, sa.node + 4, 2);
        from->d[0] = net;
        memcpy(&sock, &sa.socket, 4);                                 // the socket and the 2 stack bytes after it
        from->d[1] = node;
        from->w[4] = node45;
        from->w[5] = WS(Short_f, I_ntohs)(sock);
    }
    return 1;
}
static void fp_IPXSocket_Recv(Footprint& f, IPXSocket*, Edx, void*, int32_t*, SocketAddr*) { f.replay_only = "recvfrom"; }
PORT_FN(0x004ae170, "IPXSocket::Recv", IPXSocket_Recv, fp_IPXSocket_Recv)

// init_local_addrs: adapter 0's address (getsockopt IPX_ADDRESS); Ok if it was read
static void __fastcall IPX_init_local_addrs(IPXSocket* self, Edx) {
    const uintptr_t me = (uintptr_t)self;
    IpxAddressData d;
    int32_t optlen;
    self->nlocal = 0;
    crt_zero(self->local, 9);
    self->nlocal = 1;
    crt_zero(&d, 6);
    d.adapter = 0;
    for (;;) {
        optlen = 0x18;
        if (WS(Getsockopt_f, I_getsockopt)(self->sock, 0x3e8, 0x4007, &d, &optlen) != 0) {
            NT_LogReport(NT_CP(0x004fe4bc), err2str());
            break;
        }
        int32_t a = d.adapter;
        uint32_t net, node;
        uint16_t node45;
        memcpy(&net, d.net, 4);
        NT_GU32(me + (uint32_t)a * 12 + 4) = net;
        memcpy(&node, d.node, 4);
        memcpy(&node45, d.node + 4, 2);
        NT_GU32(me + (uint32_t)a * 12 + 8) = node;
        NT_G16(me + (uint32_t)a * 12 + 0xc) = node45;
        a = d.adapter;
        const uint16_t port = NT_G16(me + 0x2c);
        d.adapter = d.adapter + 1;
        NT_G16(me + (uint32_t)a * 12 + 0xe) = port;
        if (!(NT_G32(me + 0x28) > d.adapter)) break;
    }
    self->ok = NT_G32(me + 0x28) == d.adapter ? 1 : 0;
}
static void fp_IPX_init_local_addrs(Footprint& f, IPXSocket*, Edx) { f.replay_only = "asks winsock (getsockopt)"; }
PORT_FN(0x004ae220, "IPXSocket::init_local_addrs", IPX_init_local_addrs, fp_IPX_init_local_addrs)

static uint8_t __fastcall IPXSocket_Ok(IPXSocket* self, Edx) { return self->ok; }
PORT_FN(0x004ae670, "IPXSocket::Ok", IPXSocket_Ok, fp_pure<IPXSocket>)

static void __fastcall IPXSocket_MakeStr(IPXSocket*, Edx, const SocketAddr* a, char* out) {
    NT_sprintf(out, NT_CP(0x004fe558), a->d[0], (uint32_t)a->b[4], (uint32_t)a->b[5], (uint32_t)a->b[6], (uint32_t)a->b[7],
               (uint32_t)a->b[8], (uint32_t)a->b[9]);
}
static void fp_IPXSocket_MakeStr(Footprint& f, IPXSocket*, Edx, const SocketAddr*, char* out) { f.add(out, 32, "the string"); }
PORT_FN(0x004ae680, "IPXSocket::MakeStr", IPXSocket_MakeStr, fp_IPXSocket_MakeStr)

static uint8_t __fastcall IPXSocket_GetHostAddr(IPXSocket*, Edx, const char*, SocketAddr*) { return 0; }
static void fp_IPXSocket_GetHostAddr(Footprint& f, IPXSocket*, Edx, const char*, SocketAddr*) { f.pure = true; }
PORT_FN(0x004ae6c0, "IPXSocket::GetHostAddr", IPXSocket_GetHostAddr, fp_IPXSocket_GetHostAddr)

static void __fastcall IPXSocket_GetBroadcastAddr(IPXSocket* self, Edx, SocketAddr* a) {
    a->d[0] = 0;
    a->d[1] = 0;
    a->d[2] = 0;
    a->d[1] = 0xffffffffu;
    a->w[4] = 0xffff;
    a->d[0] = 0;
    a->w[5] = (uint16_t)self->port;
}
PORT_FN(0x004ae6d0, "IPXSocket::GetBroadcastAddr", IPXSocket_GetBroadcastAddr, fp_out_addr<IPXSocket>)

static void __fastcall IPXSocket_GetMyAddr(IPXSocket* self, Edx, SocketAddr* a) {
    a->d[0] = self->local[0].d[0];
    a->d[1] = self->local[0].d[1];
    a->d[2] = self->local[0].d[2];
}
PORT_FN(0x004ae700, "IPXSocket::GetMyAddr", IPXSocket_GetMyAddr, fp_out_addr<IPXSocket>)

static int32_t __fastcall IPXSocket_GetHeaderSize(IPXSocket*, Edx) { return 0x28; }
PORT_FN(0x004ae720, "IPXSocket::GetHeaderSize", IPXSocket_GetHeaderSize, fp_pure<IPXSocket>)
static int32_t __fastcall IPXSocket_GetBPS(IPXSocket*, Edx) { return 0xb40; }
PORT_FN(0x004ae730, "IPXSocket::GetBPS", IPXSocket_GetBPS, fp_pure<IPXSocket>)

// =========================================================================================================================
// the factories, the address compares, Socket's defaults
// =========================================================================================================================
// allocate, construct, and keep it only if Ok (else its deleting destructor, through the vtable read before Ok)
static void* create_checked(uint32_t size, uint32_t ctor, uint32_t arg) {
    void* p = ccall<void*>(F_MemAlloc, (int32_t)size);
    void* s = 0;
    if (p) s = tcall<void*>(ctor, p, arg);
    if (!s) return 0;
    const void* vt = vtbl_of(s);
    if (vtcall<uint8_t>(vt, SO_OK, s)) return s;
    vtcall<void*>(vt, SO_DTOR, s, 1u);
    return 0;
}

static void* __cdecl SocketCreateUDP0_c() { return ccall<void*>(A_SocketCreateUDP, (uint32_t)0); }
static void fp_create0(Footprint& f) { f.replay_only = "allocates, opens a socket"; }
PORT_FN(0x004ae2f0, "SocketCreateUDP", SocketCreateUDP0_c, fp_create0)

static void* __cdecl SocketCreateUDP_c(uint32_t port) { return create_checked(0x38, A_UDPSocket_ctor, port); }
static void fp_create1(Footprint& f, uint32_t) { f.replay_only = "allocates, opens a socket"; }
PORT_FN(0x004ae300, "SocketCreateUDP(short)", SocketCreateUDP_c, fp_create1)

static uint8_t __cdecl socket_addr_eq_c(const SocketAddr* a, const SocketAddr* b) { return memcmp(a, b, 12) == 0 ? 1 : 0; }
static void fp_addr2(Footprint& f, const SocketAddr*, const SocketAddr*) { f.pure = true; }
PORT_FN(0x004ae350, "operator==(socket_addr)",socket_addr_eq_c, fp_addr2)

static uint8_t __cdecl socket_addr_ne_c(const SocketAddr* a, const SocketAddr* b) {
    return ccall<uint8_t>(A_socket_addr_eq, a, b) < 1 ? 1 : 0;
}
PORT_FN(0x004ae370, "operator!=(socket_addr)",socket_addr_ne_c, fp_addr2)

static void* __cdecl SocketCreateIPX0_c() { return ccall<void*>(A_SocketCreateIPX, (uint32_t)0); }
PORT_FN(0x004ae390, "SocketCreateIPX", SocketCreateIPX0_c, fp_create0)

static void* __cdecl SocketCreateIPX_c(uint32_t port) { return create_checked(0x34, A_IPXSocket_ctor, port); }
PORT_FN(0x004ae3a0, "SocketCreateIPX(short)", SocketCreateIPX_c, fp_create1)

static void* __cdecl SocketCreateLine_c(void* dev) { return create_checked(0x774, A_LineSocket_ctor, (uint32_t)(uintptr_t)dev); }
static void fp_create_line(Footprint& f, void*) { f.replay_only = "allocates"; }
PORT_FN(0x004ae3f0, "SocketCreateLine", SocketCreateLine_c, fp_create_line)

static uint8_t __fastcall Socket_AsyncGetHostAddr(void*, Edx, const char*, AsyncReq*) { return 0; }
static void fp_Socket_AsyncGetHostAddr(Footprint& f, void*, Edx, const char*, AsyncReq*) { f.pure = true; }
PORT_FN(0x004ae450, "Socket::AsyncGetHostAddr", Socket_AsyncGetHostAddr, fp_Socket_AsyncGetHostAddr)

static void __fastcall Socket_AsyncCancel(void*, Edx, AsyncReq*) {}
static void fp_Socket_AsyncCancel(Footprint& f, void*, Edx, AsyncReq*) { f.pure = true; }
PORT_FN(0x004ae440, "Socket::AsyncCancel", Socket_AsyncCancel, fp_Socket_AsyncCancel)
static uint8_t __fastcall UDPSocket_LinkAlive(UDPSocket*, Edx) { return 1; }
PORT_FN(0x004ae640, "UDPSocket::LinkAlive", UDPSocket_LinkAlive, fp_pure<UDPSocket>)
static uint8_t __fastcall IPXSocket_LinkAlive(IPXSocket*, Edx) { return 1; }
PORT_FN(0x004ae740, "IPXSocket::LinkAlive", IPXSocket_LinkAlive, fp_pure<IPXSocket>)

// the deleting destructors: the destructor (by address; Socket's and ISocket's inlined: the vtable), then free if asked
template <typename T> static void fp_deleting(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "destroys the socket (and frees it)"; }
template <typename T> static void fp_deleting_inline(Footprint& f, T* self, Edx, uint32_t flags) {
    if (flags & 1) f.replay_only = "frees the socket";
    else f.add(self, 4, "the vtable");
}
static void* __fastcall LineSocket_deleting_dtor(LineSocket* self, Edx, uint32_t flags) {
    tcall<void>(A_LineSocket_dtor, self);
    if (flags & 1) ccall<void>(F_op_delete, (void*)self);
    return self;
}
PORT_FN(0x004ae540, "LineSocket::vector deleting destructor", LineSocket_deleting_dtor, fp_deleting<LineSocket>)
static void* __fastcall Socket_deleting_dtor(void* self, Edx, uint32_t flags) {
    *(const void**)self = (const void*)(uintptr_t)VT_ISocket;
    if (flags & 1) ccall<void>(F_op_delete, self);
    return self;
}
PORT_FN(0x004ae560, "Socket::vector deleting destructor", Socket_deleting_dtor, fp_deleting_inline<void>)
static void* __fastcall ISocket_deleting_dtor(void* self, Edx, uint32_t flags) {
    *(const void**)self = (const void*)(uintptr_t)VT_ISocket;
    if (flags & 1) ccall<void>(F_op_delete, self);
    return self;
}
PORT_FN(0x004ae580, "ISocket::scalar deleting destructor", ISocket_deleting_dtor, fp_deleting_inline<void>)
static void* __fastcall UDPSocket_deleting_dtor(UDPSocket* self, Edx, uint32_t flags) {
    tcall<void>(A_UDPSocket_dtor, self);
    if (flags & 1) ccall<void>(F_op_delete, (void*)self);
    return self;
}
PORT_FN(0x004ae650, "UDPSocket::scalar deleting destructor", UDPSocket_deleting_dtor, fp_deleting<UDPSocket>)
static void* __fastcall IPXSocket_deleting_dtor(IPXSocket* self, Edx, uint32_t flags) {
    tcall<void>(A_IPXSocket_dtor, self);
    if (flags & 1) ccall<void>(F_op_delete, (void*)self);
    return self;
}
PORT_FN(0x004ae750, "IPXSocket::scalar deleting destructor", IPXSocket_deleting_dtor, fp_deleting<IPXSocket>)

#undef WS
}  // namespace net_socket
}  // namespace
