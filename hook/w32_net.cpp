// w32_net.cpp -- the stand-ins for race.exe's WSOCK32 and TAPI32 imports and KERNEL32's serial-port functions
// (w32_net.h says what each does). TAPI and the serial port are portable C++ ("not available", the same on every OS);
// winsock is BSD sockets: ws2_32 on Windows, POSIX in the #else parts (R2b), with Windows' layouts and codes.
#define _CRT_SECURE_NO_WARNINGS
#include "w32_net.h"
#include "w32_handle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#endif

namespace {

// ---- Windows' numbers ------------------------------------------------------------------------------------------------
enum : int {
    WSAEINTR_ = 10004, WSAEBADF_ = 10009, WSAEACCES_ = 10013, WSAEFAULT_ = 10014, WSAEINVAL_ = 10022,
    WSAEMFILE_ = 10024, WSAEWOULDBLOCK_ = 10035, WSAEINPROGRESS_ = 10036, WSAEALREADY_ = 10037, WSAENOTSOCK_ = 10038,
    WSAEDESTADDRREQ_ = 10039, WSAEMSGSIZE_ = 10040, WSAEPROTOTYPE_ = 10041, WSAENOPROTOOPT_ = 10042,
    WSAEPROTONOSUPPORT_ = 10043, WSAESOCKTNOSUPPORT_ = 10044, WSAEOPNOTSUPP_ = 10045, WSAEPFNOSUPPORT_ = 10046,
    WSAEAFNOSUPPORT_ = 10047, WSAEADDRINUSE_ = 10048, WSAEADDRNOTAVAIL_ = 10049, WSAENETDOWN_ = 10050,
    WSAENETUNREACH_ = 10051, WSAENETRESET_ = 10052, WSAECONNABORTED_ = 10053, WSAECONNRESET_ = 10054,
    WSAENOBUFS_ = 10055, WSAEISCONN_ = 10056, WSAENOTCONN_ = 10057, WSAESHUTDOWN_ = 10058, WSAETIMEDOUT_ = 10060,
    WSAECONNREFUSED_ = 10061, WSAENAMETOOLONG_ = 10063, WSAEHOSTDOWN_ = 10064, WSAEHOSTUNREACH_ = 10065,
    WSAVERNOTSUPPORTED_ = 10092, WSANOTINITIALISED_ = 10093, WSAHOST_NOT_FOUND_ = 11001, WSATRY_AGAIN_ = 11002,
    WSANO_RECOVERY_ = 11003, WSANO_DATA_ = 11004,
};
const uint32_t INVALID_SOCKET_ = 0xffffffffu;
const int SOCKET_ERROR_ = -1;

}  // namespace

// =========================================================================================================================
// WSOCK32: computed here on every OS
// =========================================================================================================================
uint16_t __stdcall w32_htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
uint16_t __stdcall w32_ntohs(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

// inet_addr: winsock's parse -- one to four parts separated by dots, each decimal, octal (leading 0) or hex (0x); the
// last part fills the bytes the others didn't; parsing stops at white space (the rest ignored); anything else, or a part
// too large for its bytes, is INADDR_NONE. (test/w32_user_test.cpp compares a list of inputs with Windows'.)
uint32_t __stdcall w32_inet_addr(const char* s) {
    if (!s) { w32::set_last_error(WSAEFAULT_); return 0xffffffffu; }
    uint32_t part[4];
    int n = 0;
    const char* p = s;
    for (;;) {
        if (!isdigit((unsigned char)*p)) return 0xffffffffu;
        uint64_t v = 0;
        int base = 10;
        if (*p == '0') {
            base = 8;
            p++;
            if (*p == 'x' || *p == 'X') { base = 16; p++; }
        }
        bool any = base == 8;                          // ("0" alone is a zero)
        for (;; p++) {
            const int c = (unsigned char)*p;
            int d;
            if (isdigit(c)) d = c - '0';
            else if (base == 16 && isxdigit(c)) d = tolower(c) - 'a' + 10;
            else break;
            if (d >= base) return 0xffffffffu;
            v = v * (uint64_t)base + (uint64_t)d;
            if (v > 0xffffffffull) return 0xffffffffu;
            any = true;
        }
        if (!any) return 0xffffffffu;
        if (n == 4) return 0xffffffffu;
        part[n++] = (uint32_t)v;
        if (*p == '.') { p++; continue; }
        if (*p && !isspace((unsigned char)*p)) return 0xffffffffu;
        break;
    }
    uint32_t host;                                     // (in host order: a.b.c.d = a<<24 | ...)
    switch (n) {
    case 1: host = part[0]; break;
    case 2:
        if (part[0] > 0xff || part[1] > 0xffffff) return 0xffffffffu;
        host = part[0] << 24 | part[1];
        break;
    case 3:
        if (part[0] > 0xff || part[1] > 0xff || part[2] > 0xffff) return 0xffffffffu;
        host = part[0] << 24 | part[1] << 16 | part[2];
        break;
    default:
        if (part[0] > 0xff || part[1] > 0xff || part[2] > 0xff || part[3] > 0xff) return 0xffffffffu;
        host = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3];
        break;
    }
    return (host >> 24) | ((host >> 8) & 0xff00) | ((host << 8) & 0xff0000) | (host << 24);   // network order
}

// ---- the parts of the POSIX branch that are plain C++ (here for every OS: the test compares them with Windows') -----
// winsock's hostent, laid out in buf as winsock lays it out: the hostent (name, aliases, addrtype and length as shorts,
// addresses), its two pointer lists, the addresses, the strings. The size it takes (0: it doesn't fit; *need says)
namespace {
struct WinHostent { uint32_t name, aliases; int16_t addrtype, length; uint32_t addr_list; };
}
uint32_t w32_hostent_lay(char* buf, uint32_t cap, const char* name, const uint32_t* addrs, uint32_t n, uint32_t* need) {
    const uint32_t nlen = (uint32_t)strlen(name);
    const uint32_t size = (uint32_t)sizeof(WinHostent) + 4 /* no aliases */ + (n + 1) * 4 + n * 4 + nlen + 1;
    if (need) *need = size;
    if (size > cap) return 0;
    WinHostent* h = (WinHostent*)buf;
    uint32_t* aliases = (uint32_t*)(buf + sizeof *h);
    uint32_t* list = aliases + 1;
    char* q = (char*)(list + n + 1);
    for (uint32_t i = 0; i < n; i++, q += 4) {
        memcpy(q, &addrs[i], 4);
        list[i] = (uint32_t)(uintptr_t)q;
    }
    list[n] = 0;
    aliases[0] = 0;
    memcpy(q, name, nlen + 1);
    h->name = (uint32_t)(uintptr_t)q;
    h->aliases = (uint32_t)(uintptr_t)aliases;
    h->addrtype = 2;
    h->length = 4;
    h->addr_list = (uint32_t)(uintptr_t)list;
    return size;
}

// WSAStartup's WSADATA for a 1.x or 2.x request, as Windows 10/11's winsock fills it in (lpVendorInfo untouched)
void w32_wsadata_fill(uint16_t version, void* data) {
    uint8_t* d = (uint8_t*)data;
    const uint16_t ver = (version & 0xff) >= 2 ? 0x0202 : version, high = 0x0202, maxs = 32767, maxudp = 65467;
    memcpy(d, &ver, 2);
    memcpy(d + 2, &high, 2);
    strcpy((char*)d + 4, "WinSock 2.0");               // (the rest of each string's room left as it was, as Windows)
    strcpy((char*)d + 4 + 257, "Running");
    memcpy(d + 390, &maxs, 2);
    memcpy(d + 392, &maxudp, 2);
}

#ifdef _WIN32
// =========================================================================================================================
// WSOCK32 on Windows: ws2_32's sockets (looked up by name; the build scripts don't link it). The stand-ins' last error
// goes into the thread's real one for the call and what winsock leaves comes back -- WSAGetLastError reads it.
// =========================================================================================================================
namespace {
FARPROC ws2(const char* name) {
    static HMODULE m = LoadLibraryA("ws2_32.dll");
    return m ? GetProcAddress(m, name) : 0;
}
struct Bridge {
    Bridge() { SetLastError(w32::last_error()); }
    ~Bridge() { w32::set_last_error(GetLastError()); }
};
#define WS2(Name, Ret, Params) static Ret(WINAPI* const f_##Name) Params = (Ret(WINAPI*) Params)(void*)ws2(#Name)
}  // namespace

int __stdcall w32_WSAStartup(uint16_t version, void* data) {
    WS2(WSAStartup, int, (WORD, LPWSADATA));
    Bridge b;
    return f_WSAStartup(version, (LPWSADATA)data);
}
int __stdcall w32_WSACleanup(void) {
    WS2(WSACleanup, int, (void));
    Bridge b;
    return f_WSACleanup();
}
int __stdcall w32_WSAGetLastError(void) { return (int)w32::last_error(); }
uint32_t __stdcall w32_socket(int af, int type, int protocol) {
    WS2(socket, SOCKET, (int, int, int));
    Bridge b;
    return (uint32_t)f_socket(af, type, protocol);
}
int __stdcall w32_closesocket(uint32_t s) {
    WS2(closesocket, int, (SOCKET));
    Bridge b;
    return f_closesocket(s);
}
int __stdcall w32_bind(uint32_t s, const void* addr, int len) {
    WS2(bind, int, (SOCKET, const sockaddr*, int));
    Bridge b;
    return f_bind(s, (const sockaddr*)addr, len);
}
int __stdcall w32_setsockopt(uint32_t s, int level, int name, const void* value, int len) {
    WS2(setsockopt, int, (SOCKET, int, int, const char*, int));
    Bridge b;
    return f_setsockopt(s, level, name, (const char*)value, len);
}
int __stdcall w32_getsockopt(uint32_t s, int level, int name, void* value, int* len) {
    WS2(getsockopt, int, (SOCKET, int, int, char*, int*));
    Bridge b;
    return f_getsockopt(s, level, name, (char*)value, len);
}
int __stdcall w32_ioctlsocket(uint32_t s, int32_t cmd, uint32_t* arg) {
    WS2(ioctlsocket, int, (SOCKET, long, u_long*));
    Bridge b;
    return f_ioctlsocket(s, cmd, (u_long*)arg);
}
int __stdcall w32_sendto(uint32_t s, const void* buf, int len, int flags, const void* to, int tolen) {
    WS2(sendto, int, (SOCKET, const char*, int, int, const sockaddr*, int));
    Bridge b;
    return f_sendto(s, (const char*)buf, len, flags, (const sockaddr*)to, tolen);
}
int __stdcall w32_recvfrom(uint32_t s, void* buf, int len, int flags, void* from, int* fromlen) {
    WS2(recvfrom, int, (SOCKET, char*, int, int, sockaddr*, int*));
    Bridge b;
    return f_recvfrom(s, (char*)buf, len, flags, (sockaddr*)from, fromlen);
}
int __stdcall w32_gethostname(char* name, int len) {
    WS2(gethostname, int, (char*, int));
    Bridge b;
    return f_gethostname(name, len);
}
void* __stdcall w32_gethostbyname(const char* name) {
    WS2(gethostbyname, hostent*, (const char*));
    Bridge b;
    return f_gethostbyname(name);
}
uint32_t __stdcall w32_WSAAsyncGetHostByName(uint32_t hwnd, uint32_t msg, const char* name, char* buf, int buflen) {
    WS2(WSAAsyncGetHostByName, HANDLE, (HWND, u_int, const char*, char*, int));
    Bridge b;
    return (uint32_t)(uintptr_t)f_WSAAsyncGetHostByName((HWND)(uintptr_t)hwnd, msg, name, buf, buflen);
}
int __stdcall w32_WSACancelAsyncRequest(uint32_t request) {
    WS2(WSACancelAsyncRequest, int, (HANDLE));
    Bridge b;
    return f_WSACancelAsyncRequest((HANDLE)(uintptr_t)request);
}
void w32_set_message_poster(W32MessagePoster) {}

#else
// =========================================================================================================================
// WSOCK32 on POSIX sockets (R2b): Windows' structures, option numbers and error codes on the way in and out.
// A SOCKET is the file descriptor. Differences that stay (R2b's corpus and live play decide if they matter):
//   * a UDP socket on Windows reports an ICMP port-unreachable as WSAECONNRESET from the next recvfrom; Linux reports
//     nothing on an unconnected socket (IP_RECVERR off);
//   * getsockopt(SO_RCVBUF / SO_SNDBUF) reads back twice what was set (the kernel's bookkeeping);
//   * gethostbyname(this host's name) lists the adapters' IPv4 addresses (getifaddrs, not loopback), as Windows does.
// =========================================================================================================================
namespace {
int g_started;                                         // WSAStartup's count
std::mutex g_mu;
W32MessagePoster g_poster;

int wsa_of_errno(int e) {
    switch (e) {
    case EINTR: return WSAEINTR_;
    case EBADF: case ENOTSOCK: return WSAENOTSOCK_;
    case EACCES: case EPERM: return WSAEACCES_;
    case EFAULT: return WSAEFAULT_;
    case EINVAL: return WSAEINVAL_;
    case EMFILE: case ENFILE: return WSAEMFILE_;
    case EAGAIN: return WSAEWOULDBLOCK_;
    case EINPROGRESS: return WSAEINPROGRESS_;
    case EALREADY: return WSAEALREADY_;
    case EDESTADDRREQ: return WSAEDESTADDRREQ_;
    case EMSGSIZE: return WSAEMSGSIZE_;
    case EPROTOTYPE: return WSAEPROTOTYPE_;
    case ENOPROTOOPT: return WSAENOPROTOOPT_;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT_;
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT_;
    case EOPNOTSUPP: return WSAEOPNOTSUPP_;
    case EPFNOSUPPORT: return WSAEPFNOSUPPORT_;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT_;
    case EADDRINUSE: return WSAEADDRINUSE_;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL_;
    case ENETDOWN: return WSAENETDOWN_;
    case ENETUNREACH: return WSAENETUNREACH_;
    case ENETRESET: return WSAENETRESET_;
    case ECONNABORTED: return WSAECONNABORTED_;
    case ECONNRESET: case ECONNREFUSED: return WSAECONNRESET_;   // (UDP: Windows' name for a refused datagram)
    case ENOBUFS: case ENOMEM: return WSAENOBUFS_;
    case EISCONN: return WSAEISCONN_;
    case ENOTCONN: return WSAENOTCONN_;
    case ESHUTDOWN: return WSAESHUTDOWN_;
    case ETIMEDOUT: return WSAETIMEDOUT_;
    case ENAMETOOLONG: return WSAENAMETOOLONG_;
    case EHOSTDOWN: return WSAEHOSTDOWN_;
    case EHOSTUNREACH: return WSAEHOSTUNREACH_;
    default: return WSAEINVAL_;
    }
}
int fail(int wsa) {
    w32::set_last_error((uint32_t)wsa);
    return SOCKET_ERROR_;
}
int fail_errno() { return fail(wsa_of_errno(errno)); }
bool started() {
    if (g_started > 0) return true;
    w32::set_last_error(WSANOTINITIALISED_);
    return false;
}

// SOL_SOCKET options: Windows' number -> Linux's (0: not one we know)
int sol_option(int name) {
    switch (name) {
    case 0x0004: return SO_REUSEADDR;
    case 0x0008: return SO_KEEPALIVE;
    case 0x0010: return SO_DONTROUTE;
    case 0x0020: return SO_BROADCAST;
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1007: return SO_ERROR;
    case 0x1008: return SO_TYPE;
    default: return 0;
    }
}

// a Windows sockaddr (AF_INET: the same 16 bytes as Linux's sockaddr_in, family a 16-bit word) -> Linux's
bool to_sockaddr(const void* a, int len, sockaddr_in* out) {
    if (!a || len < 16) return false;
    memcpy(out, a, sizeof *out);
    return true;
}

// a name's IPv4 addresses (network order) and canonical name; 0, or a WSA error
int resolve(const char* name, std::string* canon, std::vector<uint32_t>* addrs) {
    char self[256] = "";
    gethostname(self, sizeof self - 1);
    if (!name || !*name || !strcasecmp(name, self)) {  // this host: its adapters' addresses, as Windows lists them
        *canon = self;
        ifaddrs* ifs = 0;
        if (getifaddrs(&ifs) == 0) {
            for (ifaddrs* i = ifs; i; i = i->ifa_next)
                if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET) {
                    const uint32_t a = ((sockaddr_in*)i->ifa_addr)->sin_addr.s_addr;
                    if ((ntohl(a) >> 24) != 127) addrs->push_back(a);
                }
            freeifaddrs(ifs);
        }
        if (addrs->empty()) addrs->push_back(htonl(0x7f000001));
        return 0;
    }
    addrinfo hints = {}, *res = 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_CANONNAME;
    const int r = getaddrinfo(name, 0, &hints, &res);
    if (r != 0) return r == EAI_AGAIN ? WSATRY_AGAIN_ : r == EAI_FAIL ? WSANO_RECOVERY_ : WSAHOST_NOT_FOUND_;
    *canon = res->ai_canonname ? res->ai_canonname : name;
    for (addrinfo* i = res; i; i = i->ai_next) {
        const uint32_t a = ((sockaddr_in*)i->ai_addr)->sin_addr.s_addr;
        bool dup = false;
        for (uint32_t b : *addrs) dup |= b == a;
        if (!dup) addrs->push_back(a);
    }
    freeaddrinfo(res);
    return addrs->empty() ? WSANO_DATA_ : 0;
}

struct Async {
    uint32_t handle, hwnd, msg;
    char* buf;
    int len;
    bool done, cancelled;
};
std::vector<Async> g_async;
uint32_t g_next_async = 0x1000;
}  // namespace

int __stdcall w32_WSAStartup(uint16_t version, void* data) {
    if ((version & 0xff) < 1 || data == 0) return (version & 0xff) < 1 ? WSAVERNOTSUPPORTED_ : WSAEFAULT_;
    w32_wsadata_fill(version, data);
    std::lock_guard<std::mutex> lk(g_mu);
    g_started++;
    return 0;
}
int __stdcall w32_WSACleanup(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_started) return fail(WSANOTINITIALISED_);
    g_started--;
    return 0;
}
int __stdcall w32_WSAGetLastError(void) { return (int)w32::last_error(); }
uint32_t __stdcall w32_socket(int af, int type, int protocol) {
    if (!started()) return INVALID_SOCKET_;
    if (af != 2) { fail(WSAEAFNOSUPPORT_); return INVALID_SOCKET_; }       // IPX (6) and the rest: not here
    const int t = type == 1 ? SOCK_STREAM : type == 2 ? SOCK_DGRAM : type == 3 ? SOCK_RAW : -1;
    if (t < 0) { fail(WSAESOCKTNOSUPPORT_); return INVALID_SOCKET_; }
    const int s = ::socket(AF_INET, t | SOCK_CLOEXEC, protocol);
    if (s < 0) { fail_errno(); return INVALID_SOCKET_; }
    return (uint32_t)s;
}
int __stdcall w32_closesocket(uint32_t s) {
    if (!started()) return SOCKET_ERROR_;
    return ::close((int)s) == 0 ? 0 : fail_errno();
}
int __stdcall w32_bind(uint32_t s, const void* addr, int len) {
    if (!started()) return SOCKET_ERROR_;
    sockaddr_in a;
    if (!to_sockaddr(addr, len, &a)) return fail(WSAEFAULT_);
    if (a.sin_family != AF_INET) return fail(WSAEAFNOSUPPORT_);
    return ::bind((int)s, (sockaddr*)&a, sizeof a) == 0 ? 0 : fail_errno();
}
int __stdcall w32_setsockopt(uint32_t s, int level, int name, const void* value, int len) {
    if (!started()) return SOCKET_ERROR_;
    if (!value || len < 1) return fail(WSAEFAULT_);
    if (level == 0xffff) {
        const int o = sol_option(name);
        if (!o) return fail(WSAENOPROTOOPT_);
        int v = 0;
        memcpy(&v, value, len < 4 ? len : 4);          // (a BOOL as 1-4 bytes, as Windows takes it)
        return ::setsockopt((int)s, SOL_SOCKET, o, &v, sizeof v) == 0 ? 0 : fail_errno();
    }
    if (level == 6 && name == 1) {                     // IPPROTO_TCP, TCP_NODELAY: the same numbers
        int v = 0;
        memcpy(&v, value, len < 4 ? len : 4);
        return ::setsockopt((int)s, IPPROTO_TCP, 1, &v, sizeof v) == 0 ? 0 : fail_errno();
    }
    return fail(level == 0x3e8 ? WSAEINVAL_ : WSAENOPROTOOPT_);  // (IPX's NSPROTO_IPX level: no IPX socket exists)
}
int __stdcall w32_getsockopt(uint32_t s, int level, int name, void* value, int* len) {
    if (!started()) return SOCKET_ERROR_;
    if (!value || !len || *len < 4) return fail(WSAEFAULT_);
    if (level != 0xffff || !sol_option(name)) return fail(level == 0x3e8 ? WSAEINVAL_ : WSAENOPROTOOPT_);
    int v = 0;
    socklen_t n = sizeof v;
    if (::getsockopt((int)s, SOL_SOCKET, sol_option(name), &v, &n) != 0) return fail_errno();
    if (name == 0x1008) v = v == SOCK_STREAM ? 1 : v == SOCK_DGRAM ? 2 : v == SOCK_RAW ? 3 : v;
    if (name == 0x1007) v = v ? wsa_of_errno(v) : 0;
    memcpy(value, &v, 4);
    *len = 4;
    return 0;
}
int __stdcall w32_ioctlsocket(uint32_t s, int32_t cmd, uint32_t* arg) {
    if (!started()) return SOCKET_ERROR_;
    if (!arg) return fail(WSAEFAULT_);
    if ((uint32_t)cmd == 0x8004667eu) {                // FIONBIO
        const int fl = fcntl((int)s, F_GETFL);
        if (fl < 0) return fail_errno();
        return fcntl((int)s, F_SETFL, *arg ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) == 0 ? 0 : fail_errno();
    }
    if ((uint32_t)cmd == 0x4004667fu) {                // FIONREAD
        int n = 0;
        if (ioctl((int)s, FIONREAD, &n) != 0) return fail_errno();
        *arg = (uint32_t)n;
        return 0;
    }
    return fail(WSAEINVAL_);
}
int __stdcall w32_sendto(uint32_t s, const void* buf, int len, int flags, const void* to, int tolen) {
    if (!started()) return SOCKET_ERROR_;
    sockaddr_in a;
    ssize_t r;
    if (to) {
        if (!to_sockaddr(to, tolen, &a)) return fail(WSAEFAULT_);
        r = ::sendto((int)s, buf, (size_t)len, (flags & 7) | MSG_NOSIGNAL, (sockaddr*)&a, sizeof a);
    } else {
        r = ::send((int)s, buf, (size_t)len, (flags & 7) | MSG_NOSIGNAL);
    }
    return r < 0 ? fail_errno() : (int)r;
}
int __stdcall w32_recvfrom(uint32_t s, void* buf, int len, int flags, void* from, int* fromlen) {
    if (!started()) return SOCKET_ERROR_;
    if (from && (!fromlen || *fromlen < 16)) return fail(WSAEFAULT_);
    sockaddr_in a = {};
    socklen_t n = sizeof a;
    const ssize_t r = ::recvfrom((int)s, buf, (size_t)len, (flags & 7) | MSG_TRUNC, (sockaddr*)&a, &n);
    if (r < 0) return fail_errno();
    if (from) {
        memcpy(from, &a, 16);
        *fromlen = 16;
    }
    if (r > len) return fail(WSAEMSGSIZE_);           // Windows: the datagram's first len bytes, and this error
    return (int)r;
}
int __stdcall w32_gethostname(char* name, int len) {
    if (!started()) return SOCKET_ERROR_;
    char h[256] = "";
    if (!name || ::gethostname(h, sizeof h - 1) != 0) return fail(WSAEFAULT_);
    if ((int)strlen(h) + 1 > len) return fail(WSAEFAULT_);
    memcpy(name, h, strlen(h) + 1);
    return 0;
}
void* __stdcall w32_gethostbyname(const char* name) {
    static thread_local char t_buf[1024];              // winsock's: one per thread, overwritten by the next call
    if (!started()) return 0;
    std::string canon;
    std::vector<uint32_t> addrs;
    if (const int e = resolve(name, &canon, &addrs)) { fail(e); return 0; }
    if (addrs.size() > 32) addrs.resize(32);
    if (!w32_hostent_lay(t_buf, sizeof t_buf, canon.c_str(), addrs.data(), (uint32_t)addrs.size(), 0)) { fail(WSANO_RECOVERY_); return 0; }
    return t_buf;
}
uint32_t __stdcall w32_WSAAsyncGetHostByName(uint32_t hwnd, uint32_t msg, const char* name, char* buf, int buflen) {
    if (!started()) return 0;
    if (!name || !buf || buflen < 0) { fail(WSAEFAULT_); return 0; }
    uint32_t h;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        h = g_next_async++;
        g_async.push_back({h, hwnd, msg, buf, buflen, false, false});
    }
    const std::string nm = name;
    std::thread([h, nm] {
        std::string canon;
        std::vector<uint32_t> addrs;
        int e = resolve(nm.c_str(), &canon, &addrs);
        uint32_t need = 0, hwnd = 0, msg = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (size_t i = 0; i < g_async.size(); i++) {
                Async& a = g_async[i];
                if (a.handle != h) continue;
                if (!a.cancelled) {
                    if (!e && !w32_hostent_lay(a.buf, (uint32_t)a.len, canon.c_str(), addrs.data(), (uint32_t)addrs.size(), &need)) e = WSAENOBUFS_;
                    hwnd = a.hwnd, msg = a.msg;
                    a.done = true;
                }
                if (a.cancelled) g_async.erase(g_async.begin() + i);
                break;
            }
        }
        if (msg && g_poster) g_poster(hwnd, msg, h, (int32_t)(((uint32_t)e << 16) | (need & 0xffff)));
    }).detach();
    return h;
}
int __stdcall w32_WSACancelAsyncRequest(uint32_t request) {
    if (!started()) return SOCKET_ERROR_;
    std::lock_guard<std::mutex> lk(g_mu);
    for (size_t i = 0; i < g_async.size(); i++) {
        Async& a = g_async[i];
        if (a.handle != request) continue;
        if (a.done) {                                  // (answered already: Windows says WSAEINVAL, as for no request)
            g_async.erase(g_async.begin() + i);
            return fail(WSAEINVAL_);
        }
        a.cancelled = true;
        return 0;
    }
    return fail(WSAEINVAL_);
}
void w32_set_message_poster(W32MessagePoster post) { g_poster = post; }
#endif

// =========================================================================================================================
// TAPI32: not available -- TAPI on a PC with no modem (every OS)
// =========================================================================================================================
// What real TAPI32 returns there (Windows 11, read off test/w32_user_test.cpp): a pointer it needs is checked first
// (LINEERR_INVALPOINTER), then whether any lineInitialize is live in the process (LINEERR_UNINITIALIZED), then the
// handle or device: no device id is valid (LINEERR_BADDEVICEID), no line or call handle exists (LINEERR_INVALLINEHANDLE /
// LINEERR_INVALCALLHANDLE). lineNegotiateAPIVersion checks the device before the app handle; lineGetDevCaps and
// lineOpen the app handle first. The app handles count down from 0x800003ff by 0x11, as TAPI's did here.
namespace {
enum : uint32_t {
    LINEERR_BADDEVICEID_ = 0x80000002u, LINEERR_INVALAPPHANDLE_ = 0x80000014u, LINEERR_INVALCALLHANDLE_ = 0x80000018u,
    LINEERR_INVALLINEHANDLE_ = 0x8000002bu, LINEERR_INVALPOINTER_ = 0x80000035u, LINEERR_UNINITIALIZED_ = 0x80000050u,
};
std::mutex g_tapi_mu;
std::vector<uint32_t> g_apps;                          // the live lineInitialize handles
uint32_t g_next_app = 0x800003ffu;

bool app_live(uint32_t app) {
    for (uint32_t a : g_apps)
        if (a == app) return true;
    return false;
}
// the check every call on a device, line or call makes after its pointers: is TAPI initialised in this process?
int32_t live_or(uint32_t err) {
    std::lock_guard<std::mutex> lk(g_tapi_mu);
    return (int32_t)(g_apps.empty() ? LINEERR_UNINITIALIZED_ : err);
}
}  // namespace

int32_t __stdcall w32_lineInitialize(uint32_t* line_app, uint32_t, void* callback, const char*, uint32_t* devices) {
    if (!line_app || !callback || !devices) return (int32_t)LINEERR_INVALPOINTER_;
    std::lock_guard<std::mutex> lk(g_tapi_mu);
    const uint32_t a = g_next_app;
    g_next_app -= 0x11;
    g_apps.push_back(a);
    *line_app = a;
    *devices = 0;                                      // no line devices: no modem
    return 0;
}
int32_t __stdcall w32_lineShutdown(uint32_t line_app) {
    std::lock_guard<std::mutex> lk(g_tapi_mu);
    if (g_apps.empty()) return (int32_t)LINEERR_UNINITIALIZED_;
    for (size_t i = 0; i < g_apps.size(); i++)
        if (g_apps[i] == line_app) {
            g_apps.erase(g_apps.begin() + i);
            return 0;
        }
    return (int32_t)LINEERR_INVALAPPHANDLE_;
}
int32_t __stdcall w32_lineNegotiateAPIVersion(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t* version, void* ext_id) {
    if (!version || !ext_id) return (int32_t)LINEERR_INVALPOINTER_;
    return live_or(LINEERR_BADDEVICEID_);              // (the device first: no device id is valid)
}
int32_t __stdcall w32_lineGetDevCaps(uint32_t line_app, uint32_t, uint32_t, uint32_t, void* caps) {
    if (!caps) return (int32_t)LINEERR_INVALPOINTER_;
    std::lock_guard<std::mutex> lk(g_tapi_mu);
    if (g_apps.empty()) return (int32_t)LINEERR_UNINITIALIZED_;
    return (int32_t)(app_live(line_app) ? LINEERR_BADDEVICEID_ : LINEERR_INVALAPPHANDLE_);
}
int32_t __stdcall w32_lineOpen(uint32_t line_app, uint32_t, uint32_t* line, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                               void*) {
    if (!line) return (int32_t)LINEERR_INVALPOINTER_;
    std::lock_guard<std::mutex> lk(g_tapi_mu);
    if (g_apps.empty()) return (int32_t)LINEERR_UNINITIALIZED_;
    return (int32_t)(app_live(line_app) ? LINEERR_BADDEVICEID_ : LINEERR_INVALAPPHANDLE_);
}
int32_t __stdcall w32_lineClose(uint32_t) { return live_or(LINEERR_INVALLINEHANDLE_); }
int32_t __stdcall w32_lineMakeCall(uint32_t, uint32_t* call, const char*, uint32_t, const void*) {
    if (!call) return (int32_t)LINEERR_INVALPOINTER_;
    return live_or(LINEERR_INVALLINEHANDLE_);
}
int32_t __stdcall w32_lineAnswer(uint32_t, const char*, uint32_t) { return live_or(LINEERR_INVALCALLHANDLE_); }
int32_t __stdcall w32_lineDrop(uint32_t, const char*, uint32_t) { return live_or(LINEERR_INVALCALLHANDLE_); }
int32_t __stdcall w32_lineDeallocateCall(uint32_t) { return live_or(LINEERR_INVALCALLHANDLE_); }
int32_t __stdcall w32_lineGetCallInfo(uint32_t, void* info) {
    if (!info) return (int32_t)LINEERR_INVALPOINTER_;
    return live_or(LINEERR_INVALCALLHANDLE_);
}
int32_t __stdcall w32_lineGetID(uint32_t, uint32_t, uint32_t, uint32_t, void* id, const char* device_class) {
    if (!id || !device_class) return (int32_t)LINEERR_INVALPOINTER_;
    return live_or(LINEERR_INVALLINEHANDLE_);
}
int32_t __stdcall w32_lineGetDevConfig(uint32_t, void* config, const char* device_class) {
    if (!config || !device_class) return (int32_t)LINEERR_INVALPOINTER_;
    return live_or(LINEERR_BADDEVICEID_);
}
int32_t __stdcall w32_lineSetDevConfig(uint32_t, const void* config, uint32_t, const char* device_class) {
    if (!config || !device_class) return (int32_t)LINEERR_INVALPOINTER_;
    return live_or(LINEERR_BADDEVICEID_);
}

// =========================================================================================================================
// KERNEL32: the serial port -- no COM ports (every OS)
// =========================================================================================================================
// Windows' answers on a handle that isn't a COM port (test/w32_user_test.cpp): a disk file ERROR_INVALID_PARAMETER, a
// console ERROR_INVALID_FUNCTION, any other object or an unknown handle ERROR_INVALID_HANDLE. GetCommState clears the
// DCB first (DCBlength 28, fBinary set, the rest 0) and GetCommProperties the COMMPROP (64 bytes of 0), whatever the
// handle; the other out-parameters are left as they were.
namespace {
int comm_fail(uint32_t h) {
    uint32_t e = w32::ERR_INVALID_HANDLE;
    if (std::shared_ptr<w32::Object> o = w32::get(h)) {
        if (o->kind == w32::Kind::File) e = w32::ERR_INVALID_PARAMETER;
        else if (o->kind == w32::Kind::Console) e = 1;  // ERROR_INVALID_FUNCTION
    }
    w32::set_last_error(e);
    return 0;
}
}  // namespace

int __stdcall w32_GetCommState(uint32_t file, void* dcb) {
    if (dcb) {
        memset(dcb, 0, 28);
        ((uint8_t*)dcb)[0] = 28;                       // DCBlength
        ((uint8_t*)dcb)[8] = 1;                        // fBinary
    }
    return comm_fail(file);
}
int __stdcall w32_SetCommState(uint32_t file, void*) { return comm_fail(file); }
int __stdcall w32_SetCommTimeouts(uint32_t file, void*) { return comm_fail(file); }
int __stdcall w32_GetCommModemStatus(uint32_t file, uint32_t*) { return comm_fail(file); }
int __stdcall w32_GetCommProperties(uint32_t file, void* props) {
    if (props) memset(props, 0, 64);                   // sizeof(COMMPROP)
    return comm_fail(file);
}
int __stdcall w32_ClearCommError(uint32_t file, uint32_t*, void*) { return comm_fail(file); }
