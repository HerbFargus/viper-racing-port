// Linux build only (tools/build_linux.sh: -I hook/linux_inc): <winsock.h> -- winsock 1.1's types and constants at
// Windows' 32-bit layouts (net_wsock.cpp records and replays the game's WSOCK32 calls in them; the stand-ins'
// own winsock, w32_net.cpp, sits on POSIX sockets and keeps its own copies of these layouts). Not the system's
// <sys/socket.h> / <netdb.h>: winsock's hostent has short h_addrtype / h_length, and a SOCKET is a u_int.
#pragma once
#include "../win32_compat.h"

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef u_int SOCKET;
#define INVALID_SOCKET (SOCKET)(~0)
#define SOCKET_ERROR (-1)
// (fd_set and struct timeval: glibc's <sys/types.h>, which <stdlib.h> brings in, has its own; winsock's select()
// isn't called by the port)
struct hostent {
    char* h_name;
    char** h_aliases;
    short h_addrtype;
    short h_length;
    char** h_addr_list;
#define h_addr h_addr_list[0]
};
struct in_addr {
    union {
        struct {
            u_char s_b1, s_b2, s_b3, s_b4;
        } S_un_b;
        struct {
            u_short s_w1, s_w2;
        } S_un_w;
        u_long S_addr;
    } S_un;
#define s_addr S_un.S_addr
};
struct sockaddr {
    u_short sa_family;
    char sa_data[14];
};
struct sockaddr_in {
    short sin_family;
    u_short sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};
#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN 128
typedef struct WSAData {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSASYS_STATUS_LEN + 1];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char* lpVendorInfo;
} WSADATA, *LPWSADATA;

#define AF_UNSPEC 0
#define AF_INET 2
#define AF_IPX 6
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define INADDR_ANY (u_long)0x00000000
#define INADDR_LOOPBACK 0x7f000001
#define INADDR_BROADCAST (u_long)0xffffffff
#define INADDR_NONE 0xffffffff
#define SOL_SOCKET 0xffff
#define SO_DEBUG 0x0001
#define SO_REUSEADDR 0x0004
#define SO_KEEPALIVE 0x0008
#define SO_DONTROUTE 0x0010
#define SO_BROADCAST 0x0020
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
#define SO_ERROR 0x1007
#define SO_TYPE 0x1008
#define IOCPARM_MASK 0x7f
#define IOC_OUT 0x40000000
#define IOC_IN 0x80000000
#define _IOR(x, y, t) (IOC_OUT | (((long)sizeof(t) & IOCPARM_MASK) << 16) | ((x) << 8) | (y))
#define _IOW(x, y, t) (IOC_IN | (((long)sizeof(t) & IOCPARM_MASK) << 16) | ((x) << 8) | (y))
#define FIONREAD _IOR('f', 127, u_long)
#define FIONBIO _IOW('f', 126, u_long)
#define MAXGETHOSTSTRUCT 1024

#define WSABASEERR 10000
#define WSAEINTR (WSABASEERR + 4)
#define WSAEBADF (WSABASEERR + 9)
#define WSAEACCES (WSABASEERR + 13)
#define WSAEFAULT (WSABASEERR + 14)
#define WSAEINVAL (WSABASEERR + 22)
#define WSAEMFILE (WSABASEERR + 24)
#define WSAEWOULDBLOCK (WSABASEERR + 35)
#define WSAEINPROGRESS (WSABASEERR + 36)
#define WSAEALREADY (WSABASEERR + 37)
#define WSAENOTSOCK (WSABASEERR + 38)
#define WSAEDESTADDRREQ (WSABASEERR + 39)
#define WSAEMSGSIZE (WSABASEERR + 40)
#define WSAEPROTOTYPE (WSABASEERR + 41)
#define WSAENOPROTOOPT (WSABASEERR + 42)
#define WSAEPROTONOSUPPORT (WSABASEERR + 43)
#define WSAESOCKTNOSUPPORT (WSABASEERR + 44)
#define WSAEOPNOTSUPP (WSABASEERR + 45)
#define WSAEPFNOSUPPORT (WSABASEERR + 46)
#define WSAEAFNOSUPPORT (WSABASEERR + 47)
#define WSAEADDRINUSE (WSABASEERR + 48)
#define WSAEADDRNOTAVAIL (WSABASEERR + 49)
#define WSAENETDOWN (WSABASEERR + 50)
#define WSAENETUNREACH (WSABASEERR + 51)
#define WSAENETRESET (WSABASEERR + 52)
#define WSAECONNABORTED (WSABASEERR + 53)
#define WSAECONNRESET (WSABASEERR + 54)
#define WSAENOBUFS (WSABASEERR + 55)
#define WSAEISCONN (WSABASEERR + 56)
#define WSAENOTCONN (WSABASEERR + 57)
#define WSAESHUTDOWN (WSABASEERR + 58)
#define WSAETIMEDOUT (WSABASEERR + 60)
#define WSAECONNREFUSED (WSABASEERR + 61)
#define WSAEHOSTUNREACH (WSABASEERR + 65)
#define WSASYSNOTREADY (WSABASEERR + 91)
#define WSAVERNOTSUPPORTED (WSABASEERR + 92)
#define WSANOTINITIALISED (WSABASEERR + 93)
#define WSAHOST_NOT_FOUND (WSABASEERR + 1001)
#define WSATRY_AGAIN (WSABASEERR + 1002)
#define WSANO_RECOVERY (WSABASEERR + 1003)
#define WSANO_DATA (WSABASEERR + 1004)

static_assert(sizeof(struct hostent) == 16 && offsetof(struct hostent, h_addr_list) == 12, "hostent (winsock)");
static_assert(sizeof(struct sockaddr_in) == 16 && sizeof(struct sockaddr) == 16, "sockaddr_in");
static_assert(sizeof(WSADATA) == 400 && offsetof(WSADATA, iMaxSockets) == 390 && offsetof(WSADATA, lpVendorInfo) == 396,
              "WSADATA (32-bit)");
