// net_wsock.cpp -- multiplayer stage N0: the game's winsock, through one layer, for the session recorder and for two
// copies of the game on one PC.
//
// race.exe imports 18 functions from WSOCK32.dll, by ordinal (socket.obj: UDPSocket / IPXSocket, winsock_grab /
// release, err2str). Their import slots are pointed at the wrappers here (found by ordinal or name in the running
// exe's import table, so any build), when a session records or replays ([session]) or [test] two_copies=1 is set;
// otherwise nothing is touched.
//
//   * no session: straight through to winsock;
//   * recording (session.cpp): straight through, and every value the game reads back is recorded on the channel of
//     the thread that read it (session_net_put): return values, recvfrom's bytes and sender, WSAGetLastError,
//     gethostname, gethostbyname's hostent, inet_addr, getsockopt, the async lookup's handle and, when its message
//     arrives through the window (async_msg_hook, which WSAAsyncGetHostByName's reply is posted to), the hostent it
//     filled in and the message (an op of that Win32Idle call). Every sendto is an output (session_net_sent);
//   * replaying: NO real winsock call at all. The recording's values are fed; a read the recording doesn't have fails
//     the way a quiet network does (recvfrom: WSAEWOULDBLOCK; socket: INVALID_SOCKET), and a send "goes".
// htons / ntohs are arithmetic, computed here.
//
// For the session's threads (session.cpp, THE NETWORK), v1.0 only, the multiplayer code is also patched at its call
// sites (each checked to be the stock call before it's changed): PTimeNow and Random in the multi library (the clock
// and random numbers the lobby task and the physics thread read: recorded on their channels), LiveMultiInfo::Grab and
// Release (the lobby's lockstep), the lobby task's two TaskSleep(250) and two TaskSuspendMe calls (its gates), and the
// message hook winsock_grab registers (async_msg_hook).
//
// Two copies on one PC ([test] two_copies=1; viperport.cpp decides which copy this is): both copies' sockets would
// bind port 2001 (UDPSocket's constructor, for host and client alike), so copy 2's bind is moved to 2002. Nothing
// else changes: the socket keeps 2001 as its own port, so copy 2's discovery broadcast (255.255.255.255:2001) and a
// host typed by name or address (UDPSocket::GetHostAddr: the address, port = the socket's own 2001) both reach copy 1;
// and a host answers the request's source (SessionMgr::recv_servreq replies to recvfrom's sender, recv_connreq keeps
// the sender as the session's address), so copy 1 hosting and copy 2 joining needs nothing more. Copy 1's broadcasts
// (its own server search, broadcast_servinfo / servdown) go to :2001 only, so copy 1's are also sent to :2002: with
// that, copy 1 can find a game copy 2 hosts in the LAN list too (by typed address it still can't: that goes to :2001).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "viperport.h"
#include "session.h"
#include "net_wsock.h"

namespace {

// ---- the real winsock (the import slots' first values) ---------------------------------------------------------------
typedef int(WINAPI* WSAStartup_t)(WORD, LPWSADATA);
typedef int(WINAPI* Int0_t)(void);
typedef int(WINAPI* Close_t)(SOCKET);
typedef SOCKET(WINAPI* Socket_t)(int, int, int);
typedef u_short(WINAPI* Short_t)(u_short);
typedef int(WINAPI* Sendto_t)(SOCKET, const char*, int, int, const struct sockaddr*, int);
typedef int(WINAPI* Recvfrom_t)(SOCKET, char*, int, int, struct sockaddr*, int*);
typedef int(WINAPI* Ioctl_t)(SOCKET, long, u_long*);
typedef int(WINAPI* Hostname_t)(char*, int);
typedef struct hostent*(WINAPI* Hostbyname_t)(const char*);
typedef unsigned long(WINAPI* InetAddr_t)(const char*);
typedef HANDLE(WINAPI* AsyncHost_t)(HWND, u_int, const char*, char*, int);
typedef int(WINAPI* Cancel_t)(HANDLE);
typedef int(WINAPI* Getsockopt_t)(SOCKET, int, int, char*, int*);
typedef int(WINAPI* Bind_t)(SOCKET, const struct sockaddr*, int);
typedef int(WINAPI* Setsockopt_t)(SOCKET, int, int, const char*, int);

WSAStartup_t o_WSAStartup;
Int0_t o_WSACleanup, o_WSAGetLastError;
Close_t o_closesocket;
Socket_t o_socket;
Short_t o_htons, o_ntohs;
Sendto_t o_sendto;
Recvfrom_t o_recvfrom;
Ioctl_t o_ioctlsocket;
Hostname_t o_gethostname;
Hostbyname_t o_gethostbyname;
InetAddr_t o_inet_addr;
AsyncHost_t o_WSAAsyncGetHostByName;
Cancel_t o_WSACancelAsyncRequest;
Getsockopt_t o_getsockopt;
Bind_t o_bind;
Setsockopt_t o_setsockopt;

// ---- the game's side (v1.0) --------------------------------------------------------------------------------------------
typedef int(__cdecl* IntV_t)(void);
typedef int(__cdecl* IntI_t)(int);
typedef void(__cdecl* VoidI_t)(int);
typedef void(__cdecl* VoidV_t)(void);
typedef void(__thiscall* This_t)(void*);
typedef uint8_t(__cdecl* MsgHook_t)(unsigned, int, int);
#ifdef SESSION_TEST
IntV_t o_PTimeNow;                                       // the harness's
IntI_t o_Random;
This_t o_Grab, o_Release;
VoidI_t o_TaskSleep;
VoidV_t o_TaskSuspendMe;
MsgHook_t o_async_hook;
#else
const IntV_t o_PTimeNow = (IntV_t)0x00413b40;
const IntI_t o_Random = (IntI_t)0x0041b6e0;              // Random(int)
const This_t o_Grab = (This_t)0x004a2d10;                // LiveMultiInfo::Grab
const This_t o_Release = (This_t)0x004a2e50;             // LiveMultiInfo::Release
const VoidI_t o_TaskSleep = (VoidI_t)0x00414de0;
const VoidV_t o_TaskSuspendMe = (VoidV_t)0x00414df0;
const MsgHook_t o_async_hook = (MsgHook_t)0x004adc00;    // async_msg_hook (socket.obj)
#endif
enum : uint16_t { GAME_PORT = 2001, COPY2_PORT = 2002 }; // UDPSocket(0x7d1): host and client alike

// which of the call sites net_install patched (the net_ entry points do what those sites do)
bool g_site_clock, g_site_random, g_site_lobby, g_site_hook;

// ---- state -----------------------------------------------------------------------------------------------------------------
CRITICAL_SECTION g_cs;
struct Role { SOCKET s; int role; };
Role g_roles[64];
int g_nroles, g_next_role;
struct Req { HANDLE h; char* buf; int len; };             // WSAAsyncGetHostByName's requests: where the reply goes
Req g_reqs[16];
__declspec(thread) int t_err;                             // a replay: the error of a failure made up here
__declspec(thread) char t_hostent[1024];                  // a replay: gethostbyname's (winsock's is per thread too)
long g_moved_binds, g_dup_broadcasts;

inline bool live() { return session_net_live(); }
inline bool recording() { return session_net_recording(); }

template <typename T> bool take(uint8_t op, T* v) {
    const uint8_t* p;
    uint32_t n;
    if (!session_net_take(op, &p, &n) || n < sizeof(T)) return false;
    memcpy(v, p, sizeof(T));
    return true;
}
template <typename T> void put(uint8_t op, const T& v) { session_net_put(op, &v, sizeof v); }

void fail(int err) { t_err = err; }

int role_of(SOCKET s) {
    EnterCriticalSection(&g_cs);
    int r = 255;
    for (int i = 0; i < g_nroles; i++)
        if (g_roles[i].s == s) { r = g_roles[i].role; break; }
    LeaveCriticalSection(&g_cs);
    return r;
}
void role_add(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    EnterCriticalSection(&g_cs);
    int i = 0;
    while (i < g_nroles && g_roles[i].s != s) i++;
    if (i == g_nroles && g_nroles < 64) g_nroles++;
    if (i < 64) g_roles[i] = {s, g_next_role++ & 0xff};
    LeaveCriticalSection(&g_cs);
}

Req* req_find(HANDLE h) {
    for (Req& r : g_reqs)
        if (r.buf && r.h == h) return &r;
    return 0;
}
void req_add(HANDLE h, char* buf, int len) {
    if (!h) return;
    EnterCriticalSection(&g_cs);
    Req* r = req_find(h);
    for (int i = 0; !r && i < 16; i++)
        if (!g_reqs[i].buf) r = &g_reqs[i];
    if (!r) r = &g_reqs[0];
    *r = {h, buf, len};
    LeaveCriticalSection(&g_cs);
}

// ---- a hostent, flattened: u16 addrtype, u16 length, name, aliases, addresses (the pointers rebuilt on the way back) ----
struct Out {
    uint8_t* p;
    size_t n, cap;
    bool ok = true;
    void bytes(const void* v, size_t k) {
        if (n + k > cap) { ok = false; return; }
        memcpy(p + n, v, k);
        n += k;
    }
    void u16(unsigned v) { uint16_t w = (uint16_t)v; bytes(&w, 2); }
    void str(const char* s) {
        const size_t k = s ? strnlen(s, 255) : 0;
        u16((unsigned)k);
        bytes(s ? s : "", k);
    }
};
size_t flatten(const struct hostent* h, uint8_t* out, size_t cap) {
    if (!h) return 0;
    Out o{out, 0, cap};
    o.u16((uint16_t)h->h_addrtype);
    const unsigned len = h->h_length > 0 && h->h_length <= 16 ? (unsigned)h->h_length : 0;
    o.u16(len);
    o.str(h->h_name);
    unsigned na = 0, nd = 0;
    while (h->h_aliases && h->h_aliases[na] && na < 16) na++;
    while (h->h_addr_list && h->h_addr_list[nd] && nd < 16) nd++;
    o.u16(na);
    for (unsigned i = 0; i < na; i++) o.str(h->h_aliases[i]);
    o.u16(nd);
    for (unsigned i = 0; i < nd; i++) o.bytes(h->h_addr_list[i], len);
    return o.ok ? o.n : 0;
}
// into buf (cap bytes) as winsock lays it out: the hostent, its pointer lists, then the addresses and the strings
bool unflatten(const uint8_t* p, size_t n, char* buf, size_t cap) {
    size_t at = 0;
    auto u16 = [&](unsigned* v) { if (at + 2 > n) return false; uint16_t w; memcpy(&w, p + at, 2); at += 2; *v = w; return true; };
    unsigned type, len, k;
    if (!u16(&type) || !u16(&len) || !u16(&k) || at + k > n) return false;
    const uint8_t* name = p + at;
    const unsigned nlen = k;
    at += k;
    unsigned na, nd;
    if (!u16(&na) || na > 16) return false;
    const uint8_t* alias[16];
    unsigned alen[16];
    for (unsigned i = 0; i < na; i++) {
        if (!u16(&alen[i]) || at + alen[i] > n) return false;
        alias[i] = p + at;
        at += alen[i];
    }
    if (!u16(&nd) || nd > 16 || at + (size_t)nd * len > n) return false;
    const uint8_t* addrs = p + at;
    size_t need = sizeof(struct hostent) + (na + 1) * 4 + (nd + 1) * 4 + (size_t)nd * len + nlen + 1;
    for (unsigned i = 0; i < na; i++) need += alen[i] + 1;
    if (need > cap) return false;
    struct hostent* h = (struct hostent*)buf;
    char** al = (char**)(buf + sizeof *h);
    char** ad = al + na + 1;
    char* q = (char*)(ad + nd + 1);
    for (unsigned i = 0; i < nd; i++, q += len) {
        memcpy(q, addrs + (size_t)i * len, len);
        ad[i] = q;
    }
    ad[nd] = 0;
    h->h_name = q;
    memcpy(q, name, nlen);
    q[nlen] = 0;
    q += nlen + 1;
    for (unsigned i = 0; i < na; i++) {
        memcpy(q, alias[i], alen[i]);
        q[alen[i]] = 0;
        al[i] = q;
        q += alen[i] + 1;
    }
    al[na] = 0;
    h->h_aliases = al;
    h->h_addrtype = (short)type;
    h->h_length = (short)len;
    h->h_addr_list = ad;
    return true;
}

// ---- the bytes of a packet the game never writes -------------------------------------------------------------------
// Several of the game's packets are built in a stack buffer the builder writes only in part, so some bytes on the
// wire are whatever the stack held: they differ from run to run (and between the original and a rewrite) and say
// nothing. A session leaves exactly those out of the send comparison and the frame hash (what is sent is unchanged).
// The wire format (socket.obj / dataport.obj / session.obj / raceclnt.obj / racesrvr.obj, v1.0):
//   byte 0: the port's class (high nibble) and the sender's flags (low nibble, 0xf mask at 0x57d62c / 0x57d708):
//           7 = unreliable (DataPort::Send 0x4af010 ORs 0x70); 2 / 4 = reliable first send, 3 / 5 = a resend
//           (HostEntry::send 0x4b0610, resend 0x4b0680); a 2-byte packet of class 0 / 1 is an ack (HostEntry::ack).
//           Low nibble 0 with byte 2 = 0x1e: a session-control packet, byte 3 its type; low nibble 1: reliable session
//           data (SessionMgr::SendReliable 0x4a6420 writes [0] and [2] = the session, HostEntry::send [1] = the
//           sequence id), byte 3 the game packet's type; low nibble 2 + class 7: SessionMgr::Send 0x4a6370 ([1] = the
//           session), the car packets of RaceClient / RaceServer::SendAll.
// What isn't written (each builder's stores read off its disassembly; tools: disasm.py / xrefs.py):
//   * unreliable session control (ServiceRequestBroadcast 0x4a5620 type 5, broadcast_servdown 0x4a5a90 type 7,
//     broadcast_servinfo 0x4a7100 type 6, recv_chandata's reply 0x4a6bcc type 4): byte 1 -- only a reliable send
//     (HostEntry::send) fills it with the sequence id;
//   * ServiceInfo (control type 6, 68 bytes: SessionMgr::make_servinfo_packet 0x4a7000 for broadcast_servinfo,
//     recv_servreq's reply and the physics thread's re-offer): bytes 4-51 are the first 48 bytes of the LocalService
//     (rep movsd 0x4a7025), which OfferService 0x4a58f0 copied from RaceServer::RaceServer's stack (0x4aaae0): there
//     the name at +0 is smart_strncpy'd (32 bytes, so after its NUL: whatever followed the source), +0x20 the type
//     (0x1001), +0x24 MULTI_VERSION, +0x2c a byte (password or not, 0x4aab4d), and +0x2d..+0x2f never -- packet
//     bytes 49-51 (seen in game: "at byte 49 (11, recorded ed), 50 (00, 6e), 51 (00, 74)"), and the name's tail;
//   * ConnectRequest (SessionMgr::Connect 0x4a5d90, control type 0): the 16-byte password at 9 is strcpy'd (repne
//     scasb / rep movs): the bytes after its NUL;
//   * car packets (2 + 24 per car: [car index][CarNetPacket, 23 bytes]): MakeNetPacket 0x4a4780 sets byte 0x16 of
//     the CarNetPacket bit by bit (bits 0-6: 0x4a4853 / 0x4a4861 / 0x4a4879 / 0x4a489b) and never bit 7 -- so bit 7 of
//     each car's last byte (RaceServer::SendAll forwards stored copies of received ones: masked alike);
//   * game packets, by type (byte 3):
//       0x21 hello (transPGS_CONNECTING_PGS_HELLO 0x4a9fc0): the 13-byte player name at 4 (OptionsGet), after its NUL;
//       0x27 chat relayed (RaceServer::dispatch_Speak / WhisperPacket 0x4ab580 / 0x4ab620), 0x28 speak (RaceClient::
//            Speak 0x4a87f0), 0x29 whisper (0x4a8880): the 50-byte text at 9 / 4 / 8 by smart_strncpy 0x4aa9e0, which
//            copies 49 bytes whatever the string's length (its loop tests the pointer, not the byte): after the NUL;
//       0x30 car info: with user id 0 at 5 (transPGS_CAR_WAITING_PGS_CAR_CHOICE 0x4aa1b0, 222 bytes) only 3 and 5-8
//            are written: 4 and 9-221 aren't; otherwise (transPGS_CAR_CHOICE_PGS_CAR_WAITING 0x4aa100) bytes 4, 9-12,
//            15-25, 58-60 and 206-221 aren't, and the 32-byte car name at 26 (smart_strncpy) after its NUL;
//       0x33 car info reply (RaceServer::dispatch_NetCarInfoRequestPacket 0x4ab980, 222 bytes: [4] the car, 5-221 the
//            server's ServerNetCarInfo record 0..0xd8, 217-220 then the round trip): an AI car's record (its human flag,
//            record +0xd8 = byte 221, is 0: add_ai_cars 0x4aafc6; every car already in gets 1 there, 0x4aae97) holds a
//            setup (record +0x38 = byte 61) copied from add_ai_cars' stack (rep movsd 0x4aaf99 from [esp+0x1c]), which
//            CarFileMakeDefaultSetup 0x465780 -> make_default_setup 0x4657d0 fills all but its dwords +0x14, +0x1c, +0x24
//            and +0x2c: bytes 81-84, 89-92, 97-100 and 105-108 (seen in game: "at byte 81 (58, recorded 10), 82 (0b, e7),
//            83 (78, 1a), 89 (f4, 64), 90 (e6, ff), 97 (30, 12)" -- recorded 0x001ae710 / 0x001aff64: stack addresses).
//            A human car's setup (its 0x30, or the IROC car's) is written whole and stays compared;
//       0x37 / 0x38 sync (transPGS_RACE_BEGIN_PGS_SYNCHRONIZING 0x4aa030 writes 3-7 of 52; the server's 0x37 and the
//            client's 0x38 replies echo the rest): 8-51;
//       0x3b deity cast (RaceDeity::record_newlap / newstage 0x443520 / 0x4435b0 build a 12-byte DeityPacket and
//            leave its byte 11; RaceClient::DeityCast puts it at 4) and 0x3c (the server's relay): byte 15.
//   Everything else the game sends is written whole by its builder (checked the same way: the other session-control
//   and game packets). What is copied in from objects (a proposal, a setup, a user's name) is compared as it is.
void mask_cstr(const uint8_t* p, int n, uint8_t* m, int at, int len) {
    for (int i = at; i < at + len && i < n; i++)
        if (!p[i]) {
            for (int j = i + 1; j < at + len && j < n; j++) m[j] = 0;
            return;
        }
}
void mask_range(uint8_t* m, int n, int from, int to) {
    for (int i = from; i <= to && i < n; i++) m[i] = 0;
}

}  // namespace

void net_send_mask(const uint8_t* p, int n, uint8_t* m) {
    if (n < 4) return;                                                // (acks: 2 bytes, written whole)
    const int cls = p[0] >> 4, flags = p[0] & 0xf;
    if (flags == 0 && p[2] == 0x1e) {                                 // session control
        if (cls == 7) m[1] = 0;
        if (p[3] == 0) mask_cstr(p, n, m, 9, 16);
        if (p[3] == 6) {
            mask_cstr(p, n, m, 4, 32);
            mask_range(m, n, 49, 51);
        }
        return;
    }
    if (cls == 7 && flags == 2) {                                     // the car packets
        for (int r = 2; r + 24 <= n; r += 24) m[r + 23] = 0x7f;
        return;
    }
    if (flags != 1 || cls < 2 || cls > 5) return;                     // reliable game packets
    switch (p[3]) {
    case 0x21: mask_cstr(p, n, m, 4, 13); break;
    case 0x27: mask_cstr(p, n, m, 9, 50); break;
    case 0x28: mask_cstr(p, n, m, 4, 50); break;
    case 0x29: mask_cstr(p, n, m, 8, 50); break;
    case 0x30: {
        uint32_t user = 0;
        if (n >= 9) memcpy(&user, p + 5, 4);
        mask_range(m, n, 4, 4);
        if (!user) {
            mask_range(m, n, 9, n - 1);
        } else {
            mask_range(m, n, 9, 12);
            mask_range(m, n, 15, 25);
            mask_cstr(p, n, m, 26, 32);
            mask_range(m, n, 58, 60);
            mask_range(m, n, 206, 221);
        }
        break;
    }
    case 0x33:
        if (n >= 222 && p[221] == 0) {                                // an AI car: its setup's four unwritten dwords
            mask_range(m, n, 81, 84);
            mask_range(m, n, 89, 92);
            mask_range(m, n, 97, 100);
            mask_range(m, n, 105, 108);
        }
        break;
    case 0x37: case 0x38: mask_range(m, n, 8, 51); break;
    case 0x3b: case 0x3c: mask_range(m, n, 15, 15); break;
    }
}

namespace {

// ---- the wrappers (WINAPI, as imported) ---------------------------------------------------------------------------------
int WINAPI h_WSAStartup(WORD ver, LPWSADATA d) {
    if (!live()) {
        const uint8_t* p;
        uint32_t n;
        if (!session_net_take(NOP_STARTUP, &p, &n) || n < 4) return WSASYSNOTREADY;
        int r;
        memcpy(&r, p, 4);
        if (d && n >= 4 + sizeof *d) memcpy(d, p + 4, sizeof *d);
        return r;
    }
    const int r = o_WSAStartup(ver, d);
    if (recording()) {
        uint8_t b[4 + sizeof(WSADATA)];
        memcpy(b, &r, 4);
        if (d) memcpy(b + 4, d, sizeof *d);
        else memset(b + 4, 0, sizeof(WSADATA));
        session_net_put(NOP_STARTUP, b, sizeof b);
    }
    return r;
}

int WINAPI h_WSACleanup() {
    int r = 0;
    if (!live()) { take(NOP_CLEANUP, &r); return r; }
    r = o_WSACleanup();
    if (recording()) put(NOP_CLEANUP, r);
    return r;
}

int WINAPI h_WSAGetLastError() {
    int r;
    if (!live()) {
        if (!take(NOP_LASTERROR, &r)) r = t_err;
        return r;
    }
    r = o_WSAGetLastError();
    if (recording()) put(NOP_LASTERROR, r);
    return r;
}

SOCKET WINAPI h_socket(int af, int type, int proto) {
    uint32_t s;
    if (!live()) {
        if (!take(NOP_SOCKET, &s)) { fail(WSAENETDOWN); return INVALID_SOCKET; }
    } else {
        s = (uint32_t)o_socket(af, type, proto);
        if (recording()) put(NOP_SOCKET, s);
    }
    role_add((SOCKET)s);
    return (SOCKET)s;
}

int WINAPI h_closesocket(SOCKET s) {
    int r = 0;
    if (!live()) { take(NOP_CLOSE, &r); return r; }
    r = o_closesocket(s);
    if (recording()) put(NOP_CLOSE, r);
    return r;
}

int WINAPI h_bind(SOCKET s, const struct sockaddr* a, int n) {
    int r = 0;
    if (!live()) { if (!take(NOP_BIND, &r)) { fail(WSAENETDOWN); r = SOCKET_ERROR; } return r; }
    struct sockaddr_in moved;
    // two copies on one PC: copy 2's sockets on 2002 (the socket's own port stays 2001; see the top)
    if (vp_copy() == 2 && a && n >= (int)sizeof moved && a->sa_family == AF_INET &&
        ((const struct sockaddr_in*)a)->sin_port == (u_short)((GAME_PORT >> 8) | (GAME_PORT & 0xff) << 8)) {
        memcpy(&moved, a, sizeof moved);
        moved.sin_port = (u_short)((COPY2_PORT >> 8) | (COPY2_PORT & 0xff) << 8);
        a = (const struct sockaddr*)&moved;
        if (InterlockedIncrement(&g_moved_binds) == 1)
            logf("net: two_copies: copy 2 binds its game socket to port %u (copy 1 has %u)", COPY2_PORT, GAME_PORT);
    }
    r = o_bind(s, a, n);
    if (recording()) put(NOP_BIND, r);
    return r;
}

int WINAPI h_setsockopt(SOCKET s, int level, int opt, const char* v, int n) {
    int r = 0;
    if (!live()) { take(NOP_SETSOCKOPT, &r); return r; }
    r = o_setsockopt(s, level, opt, v, n);
    if (recording()) put(NOP_SETSOCKOPT, r);
    return r;
}

#pragma pack(push, 1)
struct IoctlRec { int32_t r; uint32_t arg; };
struct RecvHead { int32_t r, fromlen; uint8_t from[16]; };
#pragma pack(pop)

int WINAPI h_ioctlsocket(SOCKET s, long cmd, u_long* arg) {
    IoctlRec x = {0, 0};
    if (!live()) {
        if (take(NOP_IOCTL, &x) && arg) *arg = x.arg;
        return x.r;
    }
    x.r = o_ioctlsocket(s, cmd, arg);
    x.arg = arg ? (uint32_t)*arg : 0;
    if (recording()) put(NOP_IOCTL, x);
    return x.r;
}

int WINAPI h_getsockopt(SOCKET s, int level, int opt, char* v, int* n) {
    if (!live()) {
        const uint8_t* p;
        uint32_t k;
        if (!session_net_take(NOP_GETSOCKOPT, &p, &k) || k < 8) { fail(WSAENETDOWN); return SOCKET_ERROR; }
        int32_t r, len;
        memcpy(&r, p, 4);
        memcpy(&len, p + 4, 4);
        if (n) {
            const int got = (int)k - 8 < len ? (int)k - 8 : len;
            if (v && got > 0) memcpy(v, p + 8, got < *n ? got : *n);
            *n = len;
        }
        return r;
    }
    const int cap = n ? *n : 0;
    const int r = o_getsockopt(s, level, opt, v, n);
    if (recording()) {
        int32_t len = n ? *n : 0;
        const int keep = v && len > 0 ? (len < cap ? len : cap) : 0;
        uint8_t b[8 + 256];
        memcpy(b, &r, 4);
        memcpy(b + 4, &len, 4);
        const int k = keep < 256 ? keep : 256;
        if (k > 0) memcpy(b + 8, v, k);
        session_net_put(NOP_GETSOCKOPT, b, 8 + (k > 0 ? k : 0));
    }
    return r;
}

int WINAPI h_recvfrom(SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen) {
    if (!live()) {
        const uint8_t* p;
        uint32_t n;
        if (!session_net_take(NOP_RECVFROM, &p, &n) || n < sizeof(RecvHead)) {
            fail(WSAEWOULDBLOCK);                                     // nothing came
            return SOCKET_ERROR;
        }
        RecvHead h;
        memcpy(&h, p, sizeof h);
        if (h.r > 0 && buf) {
            const int k = (int)(n - sizeof h) < h.r ? (int)(n - sizeof h) : h.r;
            memcpy(buf, p + sizeof h, k < len ? k : len);
        }
        if (fromlen && h.fromlen >= 0) {
            if (from) memcpy(from, h.from, (*fromlen < 16 ? *fromlen : 16));
            *fromlen = h.fromlen;
        }
        return h.r;
    }
    const int r = o_recvfrom(s, buf, len, flags, from, fromlen);
    if (recording()) {
        RecvHead h;
        memset(&h, 0, sizeof h);
        h.r = r;
        h.fromlen = fromlen ? *fromlen : -1;
        if (from && fromlen && *fromlen > 0) memcpy(h.from, from, *fromlen < 16 ? *fromlen : 16);
        const int k = r > 0 && buf ? (r < len ? r : len) : 0;
        uint8_t stack[sizeof h + 1024];
        uint8_t* b = (size_t)k + sizeof h <= sizeof stack ? stack : (uint8_t*)malloc(sizeof h + k);
        if (b) {
            memcpy(b, &h, sizeof h);
            if (k) memcpy(b + sizeof h, buf, k);
            session_net_put(NOP_RECVFROM, b, sizeof h + k);
            if (b != stack) free(b);
        }
    }
    return r;
}

int WINAPI h_sendto(SOCKET s, const char* buf, int len, int flags, const struct sockaddr* to, int tolen) {
    int r = len;
    if (live()) {
        r = o_sendto(s, buf, len, flags, to, tolen);
        // two copies on one PC: copy 1's broadcasts to :2001 reach copy 2 at :2002 too (not recorded: the game's own
        // send is the one above)
        if (vp_two_copies() && vp_copy() == 1 && to && tolen >= (int)sizeof(struct sockaddr_in) && to->sa_family == AF_INET) {
            const struct sockaddr_in* a = (const struct sockaddr_in*)to;
            if (a->sin_addr.s_addr == INADDR_BROADCAST && a->sin_port == (u_short)((GAME_PORT >> 8) | (GAME_PORT & 0xff) << 8)) {
                struct sockaddr_in b = *a;
                b.sin_port = (u_short)((COPY2_PORT >> 8) | (COPY2_PORT & 0xff) << 8);
                o_sendto(s, buf, len, flags, (const struct sockaddr*)&b, sizeof b);
                if (InterlockedIncrement(&g_dup_broadcasts) == 1)
                    logf("net: two_copies: copy 1's broadcasts to port %u go to copy 2's %u too", GAME_PORT, COPY2_PORT);
            }
        }
    }
    if (g_session_mode == SESSION_OFF) return r;
    // what's compared and hashed: the packet with the bytes the game never writes left out (net_send_mask)
    uint8_t stack[512];
    uint8_t* m = len <= (int)sizeof stack ? stack : (uint8_t*)malloc(len > 0 ? len : 1);
    if (!m) return session_net_sent((uint8_t)role_of(s), to, tolen, buf, len, r);
    for (int i = 0; i < len; i++) m[i] = 0xff;
#ifdef SESSION_TEST
    if (!getenv("WNS_OLD_NOMASK"))                                    // (the harness: a recording made before the masks)
#endif
    net_send_mask((const uint8_t*)buf, len, m);
    for (int i = 0; i < len; i++) m[i] &= (uint8_t)buf[i];
    r = session_net_sent((uint8_t)role_of(s), to, tolen, m, len, r);
    if (m != stack) free(m);
    return r;
}

int WINAPI h_gethostname(char* name, int n) {
    if (!live()) {
        const uint8_t* p;
        uint32_t k;
        if (!session_net_take(NOP_HOSTNAME, &p, &k) || k < 4) { fail(WSAENETDOWN); return SOCKET_ERROR; }
        int r;
        memcpy(&r, p, 4);
        if (name && n > 0) {
            const int got = (int)k - 4 < n - 1 ? (int)k - 4 : n - 1;
            memcpy(name, p + 4, got);
            name[got] = 0;
        }
        return r;
    }
    const int r = o_gethostname(name, n);
    if (recording()) {
        uint8_t b[4 + 256];
        memcpy(b, &r, 4);
        const size_t k = r == 0 && name ? strnlen(name, n < 256 ? n : 256) : 0;
        if (k) memcpy(b + 4, name, k);
        session_net_put(NOP_HOSTNAME, b, 4 + k);
    }
    return r;
}

struct hostent* WINAPI h_gethostbyname(const char* name) {
    if (!live()) {
        const uint8_t* p;
        uint32_t k;
        if (!session_net_take(NOP_HOSTBYNAME, &p, &k) || !k) { fail(WSAHOST_NOT_FOUND); return 0; }
        if (!unflatten(p, k, t_hostent, sizeof t_hostent)) { fail(WSANO_RECOVERY); return 0; }
        return (struct hostent*)t_hostent;
    }
    struct hostent* h = o_gethostbyname(name);
    if (recording()) {
        uint8_t b[1024];
        const size_t k = flatten(h, b, sizeof b);
        session_net_put(NOP_HOSTBYNAME, b, k);                        // (empty: no such host)
    }
    return h;
}

unsigned long WINAPI h_inet_addr(const char* cp) {
    uint32_t a = INADDR_NONE;
    if (!live()) { take(NOP_INET_ADDR, &a); return a; }
    a = o_inet_addr(cp);
    if (recording()) put(NOP_INET_ADDR, a);
    return a;
}

u_short WINAPI h_htons(u_short v) { return (u_short)(v >> 8 | v << 8); }
u_short WINAPI h_ntohs(u_short v) { return (u_short)(v >> 8 | v << 8); }

HANDLE WINAPI h_WSAAsyncGetHostByName(HWND w, u_int msg, const char* name, char* buf, int len) {
    uint32_t h = 0;
    if (!live()) {
        if (!take(NOP_ASYNC_HOST, &h)) fail(WSAENETDOWN);
    } else {
        h = (uint32_t)(uintptr_t)o_WSAAsyncGetHostByName(w, msg, name, buf, len);
        if (recording()) put(NOP_ASYNC_HOST, h);
    }
    req_add((HANDLE)(uintptr_t)h, buf, len);
    return (HANDLE)(uintptr_t)h;
}

int WINAPI h_WSACancelAsyncRequest(HANDLE h) {
    int r = 0;
    if (!live()) take(NOP_ASYNC_CANCEL, &r);
    else {
        r = o_WSACancelAsyncRequest(h);
        if (recording()) put(NOP_ASYNC_CANCEL, r);
    }
    EnterCriticalSection(&g_cs);
    if (Req* q = req_find(h)) q->buf = 0;
    LeaveCriticalSection(&g_cs);
    return r;
}

// ---- the async lookup's reply: async_msg_hook (socket.obj), registered by winsock_grab through the window -------------------
#pragma pack(push, 1)
struct AsyncRec { uint32_t msg, wparam, lparam, hlen; };
#pragma pack(pop)

uint8_t __cdecl h_async_hook(unsigned msg, int wparam, int lparam) {
    const bool ours = msg - 0xf400u < 4u;                             // AsyncGetHostAddr's 0xf400 + request
    if (ours && !live()) return 0;                                    // a replay: only the recording's replies
    if (ours && recording() && session_net_main()) {
        uint8_t b[sizeof(AsyncRec) + 1024];
        AsyncRec a = {msg, (uint32_t)wparam, (uint32_t)lparam, 0};
        EnterCriticalSection(&g_cs);
        Req* q = req_find((HANDLE)(uintptr_t)(uint32_t)wparam);
        if (q && !HIWORD(lparam)) a.hlen = (uint32_t)flatten((const struct hostent*)q->buf, b + sizeof a, 1024);
        LeaveCriticalSection(&g_cs);
        memcpy(b, &a, sizeof a);
        session_net_put(NOP_ASYNC_REPLY, b, sizeof a + a.hlen);
        session_net_async_event();                                    // where it arrived: this Win32Idle
    }
    return o_async_hook(msg, wparam, lparam);
}

// ---- the multiplayer code's call sites (a session, v1.0) --------------------------------------------------------------------
int __cdecl h_PTimeNow() {
    // (the main thread's is session.cpp's; so is the physics task's in a network race, at the clock: session_phys_clock)
    if (g_session_mode == SESSION_OFF || session_net_main() || session_phys_clock_on()) return o_PTimeNow();
    int v;
    if (!live()) {
        if (take(NOP_CLOCK, &v)) return v;
        return o_PTimeNow();
    }
    v = o_PTimeNow();
    put(NOP_CLOCK, v);
    return v;
}

int __cdecl h_Random(int range) {
    int v = o_Random(range);                                          // (always: the generator moves on as it did)
    if (g_session_mode == SESSION_OFF || !session_net_lobby()) return v;   // (the main thread's and the physics': theirs)
    if (!live()) {
        int f;
        if (take(NOP_RANDOM, &f)) v = f;
        return v;
    }
    put(NOP_RANDOM, v);
    return v;
}

void __fastcall h_Grab(void* lmi, void*) {
    session_lobby_grab(lmi, false);
    o_Grab(lmi);
    session_lobby_grab(lmi, true);
}
void __fastcall h_Release(void* lmi, void*) {
    session_lobby_release(lmi, false);
    o_Release(lmi);
    session_lobby_release(lmi, true);
}
void __cdecl h_lobby_sleep(int ms) { session_lobby_sleep(ms, o_TaskSleep); }
void __cdecl h_lobby_suspend_me() { session_lobby_suspend_me(o_TaskSuspendMe); }

#ifndef SESSION_TEST
// ---- installing --------------------------------------------------------------------------------------------------------------
struct Imp { uint16_t ord; const char* name; void* to; void** orig; bool done; };
Imp g_imps[] = {
    {116, "WSACleanup", (void*)h_WSACleanup, (void**)&o_WSACleanup},
    {3, "closesocket", (void*)h_closesocket, (void**)&o_closesocket},
    {115, "WSAStartup", (void*)h_WSAStartup, (void**)&o_WSAStartup},
    {23, "socket", (void*)h_socket, (void**)&o_socket},
    {9, "htons", (void*)h_htons, (void**)&o_htons},
    {20, "sendto", (void*)h_sendto, (void**)&o_sendto},
    {17, "recvfrom", (void*)h_recvfrom, (void**)&o_recvfrom},
    {15, "ntohs", (void*)h_ntohs, (void**)&o_ntohs},
    {12, "ioctlsocket", (void*)h_ioctlsocket, (void**)&o_ioctlsocket},
    {57, "gethostname", (void*)h_gethostname, (void**)&o_gethostname},
    {52, "gethostbyname", (void*)h_gethostbyname, (void**)&o_gethostbyname},
    {111, "WSAGetLastError", (void*)h_WSAGetLastError, (void**)&o_WSAGetLastError},
    {10, "inet_addr", (void*)h_inet_addr, (void**)&o_inet_addr},
    {103, "WSAAsyncGetHostByName", (void*)h_WSAAsyncGetHostByName, (void**)&o_WSAAsyncGetHostByName},
    {108, "WSACancelAsyncRequest", (void*)h_WSACancelAsyncRequest, (void**)&o_WSACancelAsyncRequest},
    {7, "getsockopt", (void*)h_getsockopt, (void**)&o_getsockopt},
    {2, "bind", (void*)h_bind, (void**)&o_bind},
    {21, "setsockopt", (void*)h_setsockopt, (void**)&o_setsockopt},
};
const int N_IMPS = sizeof g_imps / sizeof g_imps[0];

void write_slot(void** slot, void* to) {
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = to;
    VirtualProtect(slot, 4, old, &old);
}

// race.exe's wsock32 slots (by ordinal, as v1.0 imports them, or by name), any build
int patch_imports() {
    uint8_t* base = (uint8_t*)GetModuleHandleA(0);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), "WSOCK32.dll") != 0) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            for (Imp& m : g_imps) {
                bool hit;
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) hit = IMAGE_ORDINAL(names->u1.Ordinal) == m.ord;
                else hit = !strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, m.name);
                if (!hit || m.done) continue;
                *m.orig = (void*)(uintptr_t)iat->u1.Function;
                write_slot((void**)&iat->u1.Function, m.to);
                m.done = true;
                n++;
                break;
            }
        }
    }
    return n;
}

// a call or jmp rel32 at `at` to `from`, pointed at `to` (false: not the stock instruction there)
bool patch_rel32(uint32_t at, uint32_t from, void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    if (p[0] != 0xe8 && p[0] != 0xe9) return false;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    if (at + 5 + (uint32_t)rel != from) return false;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    rel = (int32_t)((uint32_t)(uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

// generated from out/race_v10.exe (every call to these in the multi library; Grab / Release from anywhere)
const uint32_t k_ptimenow_sites[] = {
    0x004a23a0, 0x004a2489, 0x004a2519, 0x004a260d, 0x004a2c6c, 0x004a2c81, 0x004a2ca7, 0x004a2d31, 0x004a2d3c, 0x004a2d6a,
    0x004a40c4, 0x004a4136, 0x004a4740, 0x004a5264, 0x004a5672, 0x004a8074, 0x004a850c, 0x004a85b8, 0x004a9108, 0x004a917e,
    0x004a9402, 0x004a976d, 0x004a9827, 0x004a9dfb, 0x004aac13, 0x004aadeb, 0x004ab3ee, 0x004abb0d, 0x004abd4e, 0x004abdfb,
    0x004abf0a, 0x004ac597, 0x004ac6a7, 0x004ac774, 0x004aca9e, 0x004acaaf, 0x004acb4b, 0x004acd14, 0x004ae9bf, 0x004aea37,
    0x004aec47, 0x004b027e, 0x004b0336, 0x004b0650, 0x004b06c3, 0x004b073c,
};
const uint32_t k_random_sites[] = {0x004a5a7e, 0x004ad4df, 0x004ae9cf};
const uint32_t k_grab_sites[] = {
    0x0048853b, 0x00488ab1, 0x00488b2c, 0x00489959, 0x004899a0, 0x004899c9, 0x00489a19, 0x0048a9a9, 0x0048a9d1,
    0x0048aa09, 0x0048aa31, 0x0048bf98, 0x0048c62b, 0x004a2472, 0x004a276f, 0x004a27af, 0x004a284a, 0x004a2865,
};
const uint32_t k_release_sites[] = {
    0x0048855c, 0x00488abe, 0x00489966, 0x004899d6, 0x0048a9b6, 0x0048aa16, 0x0048bf9f, 0x004a2395, 0x004a25bd,
    0x004a2791, 0x004a27d1,
};
const uint32_t k_sleep_sites[] = {0x004a22cf, 0x004a231b};      // LiveMultiInfo::thread's TaskSleep(250)s
const uint32_t k_suspend_sites[] = {0x004a22e0, 0x004a232c};    // and its TaskSuspendMe()s
enum : uint32_t { PUSH_ASYNC_HOOK = 0x004adb11 };               // winsock_grab: push async_msg_hook

int patch_sites(const uint32_t* at, size_t n, uint32_t from, void* to, const char* what) {
    int ok = 0;
    for (size_t i = 0; i < n; i++)
        if (patch_rel32(at[i], from, to)) ok++;
        else logf("net: NOT patching the call to %s at %08x: it isn't the stock call", what, at[i]);
    return ok;
}

bool patch_push_hook() {
    uint8_t* p = (uint8_t*)(uintptr_t)PUSH_ASYNC_HOOK;
    uint32_t imm;
    memcpy(&imm, p + 1, 4);
    if (p[0] != 0x68 || imm != 0x004adc00) return false;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    imm = (uint32_t)(uintptr_t)h_async_hook;
    memcpy(p + 1, &imm, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}
#endif

}  // namespace

void net_install(const char* ini) {
    (void)ini;
    const bool session = g_session_mode != SESSION_OFF;
    if (!session && !vp_two_copies()) return;
    InitializeCriticalSection(&g_cs);
#ifndef SESSION_TEST
    const int n = patch_imports();
    if (n != N_IMPS) {
        logf("net: %d of race.exe's %d wsock32 imports found%s", n, N_IMPS,
             n ? "" : " -- NOT hooking the network (a session won't record it, two copies would clash on port 2001)");
        if (!n) return;
    }
    if (session) {
        if (!build_is_v10()) {
            logf("net: the multiplayer code's clock, random numbers and lobby task are only hooked on v1.0");
        } else {
            const size_t np = sizeof k_ptimenow_sites / 4, nr = sizeof k_random_sites / 4, ng = sizeof k_grab_sites / 4,
                         nl = sizeof k_release_sites / 4;
            int ok = patch_sites(k_ptimenow_sites, np, 0x00413b40, (void*)h_PTimeNow, "PTimeNow");
            g_site_clock = ok > 0;
            const int okr = patch_sites(k_random_sites, nr, 0x0041b6e0, (void*)h_Random, "Random");
            g_site_random = okr > 0;
            ok += okr;
            // the lobby's gates first: Grab / Release wait for a lobby at a gate, so they're hooked only with all four
            int gates = 0;
            for (int i = 0; i < 2; i++) {
                const uint8_t* a = (const uint8_t*)(uintptr_t)k_sleep_sites[i];
                const uint8_t* b = (const uint8_t*)(uintptr_t)k_suspend_sites[i];
                int32_t ra, rb;
                memcpy(&ra, a + 1, 4);
                memcpy(&rb, b + 1, 4);
                gates += a[0] == 0xe8 && k_sleep_sites[i] + 5 + (uint32_t)ra == 0x00414de0;
                gates += b[0] == 0xe8 && k_suspend_sites[i] + 5 + (uint32_t)rb == 0x00414df0;
            }
            if (gates == 4) {
                ok += patch_sites(k_sleep_sites, 2, 0x00414de0, (void*)h_lobby_sleep, "TaskSleep (the lobby task)");
                ok += patch_sites(k_suspend_sites, 2, 0x00414df0, (void*)h_lobby_suspend_me, "TaskSuspendMe (the lobby task)");
                ok += patch_sites(k_grab_sites, ng, 0x004a2d10, (void*)h_Grab, "LiveMultiInfo::Grab");
                ok += patch_sites(k_release_sites, nl, 0x004a2e50, (void*)h_Release, "LiveMultiInfo::Release");
                g_site_lobby = true;
            } else {
                logf("net: LiveMultiInfo::thread isn't stock -- the lobby task is NOT in lockstep (the menus' frames will part)");
            }
            const bool hook = patch_push_hook();
            g_site_hook = hook;
            if (!hook) logf("net: NOT hooking async_msg_hook (winsock_grab isn't stock): host lookups by name won't be recorded");
            logf("net: the multiplayer code patched at %d of %u call sites (its clock, random numbers, Grab / Release, the"
                 " lobby task's gates)%s", ok, (unsigned)(np + nr + ng + nl + 4), hook ? ", and its winsock message hook" : "");
        }
    }
#endif
    logf("net: the wsock32 imports go through net_wsock.cpp -- %s", g_session_mode == SESSION_RECORD ? "recorded"
         : session ? "a replay: no real socket call, the recording's values are fed" : "straight through (two_copies)");
}

// ---- the patched sites, for the rewrites of the functions that hold them (net_wsock.h) ---------------------------------------
#ifdef SESSION_TEST
#define VP_SITE(flag) true                                    // the harness: every site "patched"
#else
#define VP_SITE(flag) (flag)
#endif
int __cdecl net_PTimeNow() { return VP_SITE(g_site_clock) ? h_PTimeNow() : o_PTimeNow(); }
int __cdecl net_Random(int range) { return VP_SITE(g_site_random) ? h_Random(range) : o_Random(range); }
void __fastcall net_Grab(void* lmi, void*) {
    if (VP_SITE(g_site_lobby)) h_Grab(lmi, 0);
    else o_Grab(lmi);
}
void __fastcall net_Release(void* lmi, void*) {
    if (VP_SITE(g_site_lobby)) h_Release(lmi, 0);
    else o_Release(lmi);
}
void __cdecl net_lobby_sleep(int ms) {
    if (VP_SITE(g_site_lobby)) h_lobby_sleep(ms);
    else o_TaskSleep(ms);
}
void __cdecl net_lobby_suspend_me() {
    if (VP_SITE(g_site_lobby)) h_lobby_suspend_me();
    else o_TaskSuspendMe();
}
uint32_t net_async_hook_for_winsock_grab() {
    return VP_SITE(g_site_hook) ? (uint32_t)(uintptr_t)h_async_hook : (uint32_t)(uintptr_t)o_async_hook;
}
#undef VP_SITE

#ifdef SESSION_TEST
// test/world_net_session.cpp: the "real" side (a fake winsock, the game's functions) in, the wrapper out
void* net_test_hook(const char* name, void* real) {
    struct T { const char* n; void** o; void* h; } t[] = {
        {"WSAStartup", (void**)&o_WSAStartup, (void*)h_WSAStartup}, {"WSACleanup", (void**)&o_WSACleanup, (void*)h_WSACleanup},
        {"WSAGetLastError", (void**)&o_WSAGetLastError, (void*)h_WSAGetLastError}, {"socket", (void**)&o_socket, (void*)h_socket},
        {"closesocket", (void**)&o_closesocket, (void*)h_closesocket}, {"bind", (void**)&o_bind, (void*)h_bind},
        {"setsockopt", (void**)&o_setsockopt, (void*)h_setsockopt}, {"ioctlsocket", (void**)&o_ioctlsocket, (void*)h_ioctlsocket},
        {"getsockopt", (void**)&o_getsockopt, (void*)h_getsockopt}, {"recvfrom", (void**)&o_recvfrom, (void*)h_recvfrom},
        {"sendto", (void**)&o_sendto, (void*)h_sendto}, {"gethostname", (void**)&o_gethostname, (void*)h_gethostname},
        {"gethostbyname", (void**)&o_gethostbyname, (void*)h_gethostbyname}, {"inet_addr", (void**)&o_inet_addr, (void*)h_inet_addr},
        {"htons", (void**)&o_htons, (void*)h_htons}, {"ntohs", (void**)&o_ntohs, (void*)h_ntohs},
        {"WSAAsyncGetHostByName", (void**)&o_WSAAsyncGetHostByName, (void*)h_WSAAsyncGetHostByName},
        {"WSACancelAsyncRequest", (void**)&o_WSACancelAsyncRequest, (void*)h_WSACancelAsyncRequest},
        {"PTimeNow", (void**)&o_PTimeNow, (void*)h_PTimeNow}, {"Random", (void**)&o_Random, (void*)h_Random},
        {"Grab", (void**)&o_Grab, (void*)h_Grab}, {"Release", (void**)&o_Release, (void*)h_Release},
        {"TaskSleep", (void**)&o_TaskSleep, (void*)h_lobby_sleep}, {"TaskSuspendMe", (void**)&o_TaskSuspendMe, (void*)h_lobby_suspend_me},
        {"async_msg_hook", (void**)&o_async_hook, (void*)h_async_hook},
    };
    for (T& e : t)
        if (!strcmp(e.n, name)) { *e.o = real; return e.h; }
    return 0;
}
#endif

void net_play_async() {
    const uint8_t* p;
    uint32_t n;
    if (!session_net_take(NOP_ASYNC_REPLY, &p, &n) || n < sizeof(AsyncRec)) {
        logf("session: the recording has a winsock reply here, but no record of it on the main thread");
        return;
    }
    AsyncRec a;
    memcpy(&a, p, sizeof a);
    EnterCriticalSection(&g_cs);
    Req* q = req_find((HANDLE)(uintptr_t)a.wparam);
    if (q && a.hlen && sizeof a + a.hlen <= n) unflatten(p + sizeof a, a.hlen, q->buf, (size_t)q->len);
    LeaveCriticalSection(&g_cs);
    o_async_hook(a.msg, (int)a.wparam, (int)a.lparam);
}
