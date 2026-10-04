// net_types.h -- multiplayer stage N1, group A: the layouts and helpers of the transports (library `multi`: socket.obj,
// line.obj, linechek.obj, linepkt.obj, tapidbg.obj, crc.obj), shared by hook/net_socket.cpp, net_line.cpp and
// net_linepkt.cpp (and open to group B's net_core.h: the socket and line-device interfaces are what DataPort and
// SessionMgr talk to). Layouts are read off the v1.0 disassembly (types.json has next to nothing here); every one is
// static_assert'ed.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nt {

typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

// ---- memory and calls -----------------------------------------------------------------------------------------------
#define NT_G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define NT_G16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define NT_G32(a) (*(volatile int32_t*)(uintptr_t)(a))
#define NT_GU32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define NT_CP(a) ((const char*)(uintptr_t)(a))
// a Windows / winsock / TAPI import through race.exe's own import slot, read at the call (the original's
// `call dword ptr [slot]`, or its `call thunk` whose thunk is `jmp dword ptr [slot]`): N0's net_wsock.cpp points the
// wsock32 slots at its recorder, and a harness stubs any slot
#define NT_IAT(T, slot) (*(T volatile*)(uintptr_t)(slot))

// a virtual call: the slot at byte `off` of obj's vtable (read now), received as __thiscall
template <typename R, typename... A> static __forceinline R vcall(const void* obj, uint32_t off, A... a) {
    typedef R(__fastcall * Fn)(const void*, Edx, A...);
    return ((Fn)(*(void* const* volatile*)obj)[off / 4])(obj, 0, a...);
}
// the same through a vtable the caller read earlier (the original keeps it in a register across a call)
template <typename R, typename... A> static __forceinline R vtcall(const void* vt, uint32_t off, const void* obj, A... a) {
    typedef R(__fastcall * Fn)(const void*, Edx, A...);
    return ((Fn)((void* const*)vt)[off / 4])(obj, 0, a...);
}
static __forceinline const void* vtbl_of(const void* obj) { return *(const void* const volatile*)obj; }
// a __thiscall game function by address
template <typename R, typename... A> static __forceinline R tcall(uint32_t fn, const void* self, A... a) {
    typedef R(__fastcall * Fn)(const void*, Edx, A...);
    return ((Fn)(uintptr_t)fn)(self, 0, a...);
}
// a __cdecl game function by address
template <typename R, typename... A> static __forceinline R ccall(uint32_t fn, A... a) {
    typedef R(__cdecl * Fn)(A...);
    return ((Fn)(uintptr_t)fn)(a...);
}
// a __stdcall game function by address (tapi_cb)
template <typename R, typename... A> static __forceinline R scall(uint32_t fn, A... a) {
    typedef R(__stdcall * Fn)(A...);
    return ((Fn)(uintptr_t)fn)(a...);
}
// the variadic ones (the game's log and C runtime)
typedef void(__cdecl* NtLog_t)(const char*, ...);
typedef int(__cdecl* NtSprintf_t)(char*, const char*, ...);
#define NT_LogReport ((nt::NtLog_t)(uintptr_t)0x00411150)
#define NT_LogPanic ((nt::NtLog_t)(uintptr_t)0x004112b0)
#define NT_sprintf ((nt::NtSprintf_t)(uintptr_t)0x004cf0a0)
#define NT_VERBOSE ((nt::NtLog_t)(uintptr_t)0x004aec20)          // linechek.obj: a bare `ret`
#define NT_TAPI_VERBOSE ((nt::NtLog_t)(uintptr_t)0x004a2030)     // line.obj: a bare `ret`

// The compiler's inline string code, as the original runs it: strlen is `repne scasb`; strcpy and memcpy copy n / 4
// dwords (`rep movsd`) then n % 4 bytes (`rep movsb`), forward, one unit at a time
static __forceinline uint32_t crt_strlen(const char* s) {
    const volatile char* p = s;
    uint32_t n = 0;
    while (p[n]) n++;
    return n;
}
static __forceinline void crt_copy(void* d, const void* s, uint32_t n) {
    volatile uint32_t* dd = (volatile uint32_t*)d;
    const volatile uint32_t* sd = (const volatile uint32_t*)s;
    for (uint32_t i = n >> 2; i; i--) *dd++ = *sd++;
    volatile uint8_t* db = (volatile uint8_t*)dd;
    const volatile uint8_t* sb = (const volatile uint8_t*)sd;
    for (uint32_t i = n & 3; i; i--) *db++ = *sb++;
}
static __forceinline void crt_strcpy(char* d, const char* s) { crt_copy(d, s, crt_strlen(s) + 1); }
static __forceinline void crt_zero(void* d, uint32_t dwords) {               // rep stosd with eax = 0
    volatile uint32_t* p = (volatile uint32_t*)d;
    for (uint32_t i = 0; i < dwords; i++) p[i] = 0;
}

// ---- game functions outside group A (v1.0) ---------------------------------------------------------------------------
enum : uint32_t {
    F_LogReport = 0x00411150, F_LogPanic = 0x004112b0,
    F_MemAlloc = 0x004140e0, F_op_delete = 0x00414390,
    F_PTimeNow = 0x00413b40, F_Random = 0x0041b6e0,
    F_Win32GetErrorString0 = 0x00416070,            // (void): GetLastError's text
    F_Win32GetErrorString1 = 0x00416080,            // (int)
    F_Win32GetWindow = 0x00412be0, F_Win32GetAppInstance = 0x00412bc0, F_Win32AppName = 0x00412cb0,
    F_Win32RegisterMessageHook = 0x00412d10, F_Win32UnRegisterMessageHook = 0x00412d40, F_Win32Idle = 0x00412bf0,
    F_TaskGetID = 0x00414dd0, F_TaskIsDead = 0x00414e60, F_TaskSleep = 0x00414de0, F_TaskDestroy = 0x00414ac0,
    F_Xlator_ctor = 0x0041af80, F_Xlator_xlate = 0x0041afb0, F_atexit = 0x004ceff0,
    F_strchr = 0x004ce0c0, F_strtoul = 0x004d01a0, F_strncpy = 0x004cf3a0,
    S_XLATOR_COOKIE = 0x004eb108,                   // Xlator::g_cookie
};

// ---- group A's own functions (called by address, so a hooked rewrite or the original is what runs) -------------------
enum : uint32_t {
    // crc.obj
    A_CRC16 = 0x004a3d70,
    // linepkt.obj
    A_LinePacketizer_ctor = 0x004af7d0, A_LinePacketizer_dtor = 0x004af7f0, A_LinePacketizer_Send = 0x004af810,
    A_LinePacketizer_GetHeaderSize = 0x004af870, A_LinePacketizer_Recv = 0x004af880,
    // linechek.obj
    A_LineChecker_DoneChecking = 0x004aec30,
    // tapidbg.obj
    A_tapierr = 0x004aecb0, A_tapimsg2str = 0x004aecc0, A_tapicallstate2str = 0x004aecd0, A_pst_type = 0x004aece0,
    // socket.obj
    A_local_pkt_ctor = 0x004ae530, A_LineSocket_dtor = 0x004ad500, A_LineSocket_alloc_pkt = 0x004ad540,
    A_LineSocket_dequeue = 0x004ad6f0, A_LineSocket_enqueue = 0x004ad760,
    A_err2str0 = 0x004ad8b0, A_err2str1 = 0x004ad8c0, A_UDP_init_local_addrs = 0x004ad8e0, A_UDPSocket_ctor = 0x004ad970,
    A_winsock_grab = 0x004adad0, A_set_autodial = 0x004adb30, A_async_msg_hook = 0x004adc00,
    A_UDPSocket_dtor = 0x004adc80, A_winsock_release = 0x004adcd0, A_IPXSocket_ctor = 0x004adf00,
    A_IPXSocket_dtor = 0x004ae060, A_IPX_init_local_addrs = 0x004ae220, A_SocketCreateUDP = 0x004ae300,
    A_socket_addr_eq = 0x004ae350, A_SocketCreateIPX = 0x004ae3a0, A_LineSocket_ctor = 0x004ad450,
    // line.obj
    A_LineDevice_Send = 0x004a00e0, A_dump_blocks = 0x004a0190, A_LineDevice_GetStatus = 0x004a02f0,
    A_cfg_com_timeouts = 0x004a0300, A_TAPILine_ctor = 0x004a0350, A_TAPILine_dtor = 0x004a0420,
    A_TAPILine_get_handle = 0x004a04a0, A_TAPILine_cfg_modem = 0x004a0540, A_set_from_bytestream = 0x004a0550,
    A_TAPILine_update_callbps = 0x004a0840, A_TAPILine_update_callstate = 0x004a08c0, A_TAPILine_line_close = 0x004a0aa0,
    A_TAPILine_message = 0x004a0ac0, A_DirectLine_ctor = 0x004a0e50, A_DirectLine_update_status = 0x004a0ee0,
    A_DirectLine_check_lines = 0x004a0f50, A_DirectLine_dtor = 0x004a0fa0, A_DirectLine_port_is_capable = 0x004a0fe0,
    A_DirectLine_cfg_port = 0x004a1060, A_init_dcb = 0x004a11f0, A_reclaim_all_sendblocks = 0x004a12b0,
    A_free_completed_sendblocks = 0x004a12c0, A_alloc_sendblock = 0x004a1360, A_lds_str = 0x004a13b0,
    A_tapi_cb = 0x004a1530, A_kill_thread = 0x004a1580, A_enum_devices = 0x004a16b0,
    A_enumerate_tapi_devices = 0x004a1730, A_tapidev_in_use = 0x004a1860, A_get_tapiline_port = 0x004a1870,
    A_init_devinfo_as_tapiline = 0x004a1960, A_enumerate_comport_devices = 0x004a19b0,
    A_init_devinfo_as_comport = 0x004a1aa0, A_CreateDirectLine = 0x004a1af0, A_CreateTAPILine = 0x004a1b60,
};

// ---- vtables (v1.0 .rdata) -------------------------------------------------------------------------------------------
enum : uint32_t {
    VT_Socket = 0x004df7a0, VT_LineSocket = 0x004df768, VT_ISocket = 0x004df7d8, VT_UDPSocket = 0x004df810,
    VT_IPXSocket = 0x004df848,
    VT_LineDevice = 0x004df280, VT_TAPILine = 0x004df2b8, VT_ILineDevice = 0x004df2f0, VT_DirectLine = 0x004df320,
};
// ISocket's slots (byte offsets)
enum : uint32_t {
    SO_DTOR = 0x00, SO_SEND = 0x04, SO_RECV = 0x08, SO_ASYNC_GET_HOST_ADDR = 0x0c, SO_ASYNC_CANCEL = 0x10,
    SO_GET_HOST_ADDR = 0x14, SO_MAKE_STR = 0x18, SO_ADDRESS_IS_LOCAL = 0x1c, SO_GET_MY_ADDR = 0x20,
    SO_GET_BROADCAST_ADDR = 0x24, SO_LINK_ALIVE = 0x28, SO_GET_HEADER_SIZE = 0x2c, SO_GET_BPS = 0x30, SO_OK = 0x34,
};
// ILineDevice's slots
enum : uint32_t {
    LD_SHUTDOWN = 0x00, LD_CAN_DESTROY_SAFELY = 0x04, LD_DTOR = 0x08, LD_BPS = 0x0c, LD_GET_STATUS = 0x10,
    LD_SEND = 0x14, LD_RECV = 0x18, LD_DATA_PENDING = 0x1c, LD_DATA_AVAIL = 0x20, LD_CALL = 0x24, LD_RESET = 0x28,
    LD_ANSWER = 0x2c, LD_OK = 0x30,
};

// ---- layouts --------------------------------------------------------------------------------------------------------
// socket_addr: 12 bytes. UDP: the IPv4 address (network order) at +0, the port (host order) at +4. IPX: the network
// number +0..3, the node +4..9, the socket (host order) at +0xa. A line: an id at +0 (the peer's is ~ours, -1 broadcast).
union SocketAddr {
    uint8_t b[12];
    uint16_t w[6];
    uint32_t d[3];
};
static_assert(sizeof(SocketAddr) == 12, "socket_addr");

// AsyncSocketRequestBlock: a host lookup by name (UDPSocket::AsyncGetHostAddr, answered through async_msg_hook)
struct AsyncReq {
    uint32_t handle;                   // +0x00  WSAAsyncGetHostByName's handle (-1 after AsyncCancel)
    int32_t status;                    // +0x04  0 pending, 1 failed, 2 done
    SocketAddr addr;                   // +0x08  the answer (port = the socket's)
    uint8_t hostent[0x400];            // +0x14  winsock's buffer: a hostent (h_addr_list at +0xc = block +0x20)
};
static_assert(offsetof(AsyncReq, addr) == 8 && offsetof(AsyncReq, hostent) == 0x14 && sizeof(AsyncReq) == 0x414, "AsyncReq");

struct UDPSocket {                     // 0x38 (SocketCreateUDP)
    const void* vtbl;                  // +0x00
    int32_t slot;                      // +0x04  its async request slot: winsock_grab's count when it was made (0..3)
    SocketAddr local[3];               // +0x08  this host's addresses (init_local_addrs: NOT bounded to 3)
    int32_t nlocal;                    // +0x2c
    int16_t port;                      // +0x30
    uint8_t ok;                        // +0x32
    uint8_t pad33;
    uint32_t sock;                     // +0x34  SOCKET (-1 none)
};
static_assert(offsetof(UDPSocket, nlocal) == 0x2c && offsetof(UDPSocket, port) == 0x30 && offsetof(UDPSocket, ok) == 0x32 &&
              offsetof(UDPSocket, sock) == 0x34 && sizeof(UDPSocket) == 0x38, "UDPSocket");

struct IPXSocket {                     // 0x34 (SocketCreateIPX)
    const void* vtbl;                  // +0x00
    SocketAddr local[3];               // +0x04
    int32_t nlocal;                    // +0x28  (always 1)
    int16_t port;                      // +0x2c
    uint8_t ok;                        // +0x2e
    uint8_t pad2f;
    uint32_t sock;                     // +0x30
};
static_assert(offsetof(IPXSocket, nlocal) == 0x28 && offsetof(IPXSocket, port) == 0x2c && offsetof(IPXSocket, ok) == 0x2e &&
              offsetof(IPXSocket, sock) == 0x30 && sizeof(IPXSocket) == 0x34, "IPXSocket");

struct LocalPkt {                      // LineSocket::local_pkt: a packet to this machine itself (0xec)
    uint8_t data[0xe4];
    int32_t len;                       // +0xe4  0: free
    LocalPkt* next;                    // +0xe8
};
static_assert(offsetof(LocalPkt, len) == 0xe4 && sizeof(LocalPkt) == 0xec, "local_pkt");

struct LinePacketizer {                // 0x14
    void* dev;                         // +0x00  ILineDevice* (owned: the destructor deletes it)
    int8_t state;                      // +0x04  0 sync, 1 header, 2 body (3 after destruction: a panic)
    uint8_t pad5[3];
    uint8_t count;                     // +0x08  sync bytes (0xf0) seen
    uint8_t pad9[3];
    int32_t len;                       // +0x0c  the body's length
    uint16_t crc;                      // +0x10  its CRC16
    uint16_t pad12;
};
static_assert(offsetof(LinePacketizer, count) == 8 && offsetof(LinePacketizer, len) == 0xc &&
              offsetof(LinePacketizer, crc) == 0x10 && sizeof(LinePacketizer) == 0x14, "LinePacketizer");

struct LineSocket {                    // 0x774 (SocketCreateLine)
    const void* vtbl;                  // +0x000
    LinePacketizer* pk;                // +0x004
    void* dev;                         // +0x008  ILineDevice*
    LocalPkt pkts[8];                  // +0x00c
    uint32_t id;                       // +0x76c  this end's address (Random(10000000), never 0 or -1)
    LocalPkt* head;                    // +0x770  the packets to itself, oldest first
};
static_assert(offsetof(LineSocket, pkts) == 0xc && offsetof(LineSocket, id) == 0x76c && offsetof(LineSocket, head) == 0x770 &&
              sizeof(LineSocket) == 0x774, "LineSocket");

struct LineChecker {                   // 0x28: two ends of a serial / modem line count "(n)" pings at each other
    void* dev;                         // +0x00  ILineDevice*
    int32_t next_send;                 // +0x04  PTimeNow of the next ping
    int32_t start;                     // +0x08  (moved on 500 ms for each ping that went unanswered)
    int32_t extra;                     // +0x0c  bad replies (each gives 500 ms more; at most 40)
    int32_t got;                       // +0x10  replies parsed
    int32_t sent;                      // +0x14
    int32_t in_seq;                    // +0x18  replies that followed the last one
    uint32_t my_seq;                   // +0x1c  Random(100000), then counts up
    uint32_t peer_seq;                 // +0x20
    uint8_t toggle;                    // +0x24  read every other tick
    uint8_t pad25[3];
};
static_assert(offsetof(LineChecker, my_seq) == 0x1c && offsetof(LineChecker, toggle) == 0x24 && sizeof(LineChecker) == 0x28,
              "LineChecker");

struct LineDevice {                    // 0xc (DirectLine)
    const void* vtbl;                  // +0x00
    int32_t status;                    // +0x04  LineDeviceStatus (0..11; 2 = connected)
    uint32_t handle;                   // +0x08  the COM port (-1 none)
};
static_assert(sizeof(LineDevice) == 0xc, "LineDevice");

struct TAPILine {                      // 0x40
    const void* vtbl;                  // +0x00
    int32_t status;                    // +0x04
    uint32_t handle;                   // +0x08  the modem's COM handle (lineGetID "comm/datamodem")
    uint32_t hline;                    // +0x0c
    uint32_t callstate;                // +0x10  the last LINECALLSTATE_
    uint8_t connected;                 // +0x14  a call is up (or offered)
    uint8_t pad15[3];
    uint32_t hcall;                    // +0x18
    int32_t rate;                      // +0x1c  LINECALLINFO dwRate (BPS() is it / 10)
    int32_t req_call;                  // +0x20  lineMakeCall's request id
    int32_t req_answer;                // +0x24  lineAnswer's
    int32_t req_drop;                  // +0x28  lineDrop's
    int32_t req_2c;                    // +0x2c  (only compared)
    int32_t req_30;                    // +0x30  (only compared)
    const char* number;                // +0x34  Call's
    uint32_t devid;                    // +0x38
    uint8_t ok;                        // +0x3c
    uint8_t pad3d[3];
};
static_assert(offsetof(TAPILine, hcall) == 0x18 && offsetof(TAPILine, number) == 0x34 && offsetof(TAPILine, ok) == 0x3c &&
              sizeof(TAPILine) == 0x40, "TAPILine");

struct SendBlock {                     // line.obj's 16 overlapped writes (0xfc)
    uint32_t handle;                   // +0x00  the port written to (-1 free)
    uint8_t data[0xe4];                // +0x04
    uint32_t ovl[4];                   // +0xe8  OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh
    uint32_t event;                    // +0xf8  OVERLAPPED.hEvent (LineBegin)
};
static_assert(offsetof(SendBlock, ovl) == 0xe8 && offsetof(SendBlock, event) == 0xf8 && sizeof(SendBlock) == 0xfc, "SendBlock");

struct LineDeviceInfo {                // 0x50: LineEnumerateDevices' entries
    uint8_t flag0;                     // +0x00  TAPI: a LINEDEVCAPS flag (+0xec & 8); a COM port: 0
    uint8_t flag1;                     // +0x01  TAPI: tapidev_in_use; a COM port: access denied (in use)
    char name[0x40];                   // +0x02  (strncpy 0x40, then [0x41] = 0)
    uint8_t pad42[2];
    uint32_t create;                   // +0x44  CreateTAPILine / CreateDirectLine
    int32_t id;                        // +0x48  the TAPI device id / the COM port's number
    uint32_t pad4c;
};
static_assert(offsetof(LineDeviceInfo, create) == 0x44 && offsetof(LineDeviceInfo, id) == 0x48 && sizeof(LineDeviceInfo) == 0x50,
              "LineDeviceInfo");

// ---- statics (v1.0) --------------------------------------------------------------------------------------------------
enum : uint32_t {
    // socket.obj
    S_ASYNC_REQS = 0x0057d570,         // AsyncReq*[4], by UDPSocket::slot
    S_WSA_OK = 0x0057d568,             // byte: WSAStartup succeeded
    S_GRABS = 0x0057d594,              // winsock_grab's count
    S_ERRBUF = 0x0057d5a0,             // err2str's "0x%x" (0x20 bytes)
    S_AUTODIAL = 0x0057d5c0,           // byte: set_autodial(0) turned it off (winsock_release turns it back on)
    S_AUTODIAL_KEY = 0x004df760, S_AUTODIAL_VALUE = 0x004df764,   // char* (.data)
    // line.obj
    S_THREAD = 0x0057bec8,             // a task kill_thread waits for and destroys
    S_WRITTEN = 0x0057be9c,            // WriteFile's lpNumberOfBytesWritten
    S_PORT_TAPI = 0x0057c150,          // byte[5]: COM n (1..4) belongs to a TAPI modem
    S_APIVER = 0x0057c168, S_NDEVS = 0x0057c16c, S_HLINEAPP = 0x0057c170,
    S_TAPI_OK = 0x0057c174,            // byte
    S_TAPI_RESULT = 0x0057c184,        // the last TAPI result
    S_BLOCKS = 0x0057c1c8, S_BLOCKS_END = 0x0057d188,   // SendBlock[16]
    S_BLOCKS_FREE = 0x004faabc,        // DBGLineBlocksFree (.data, 16)
    S_DATAMODEM = 0x004df278,          // char*: "comm/datamodem"
};

// ---- import slots (v1.0 IAT) -----------------------------------------------------------------------------------------
enum : uint32_t {
    // WSOCK32
    I_WSACancelAsyncRequest = 0x005d771c, I_WSAAsyncGetHostByName = 0x005d7718, I_inet_addr = 0x005d7714,
    I_WSAGetLastError = 0x005d7710, I_gethostbyname = 0x005d770c, I_gethostname = 0x005d7708, I_ioctlsocket = 0x005d7704,
    I_setsockopt = 0x005d7728, I_bind = 0x005d7724, I_htons = 0x005d76f4, I_socket = 0x005d76f0, I_WSAStartup = 0x005d76ec,
    I_closesocket = 0x005d76e8, I_WSACleanup = 0x005d76e4, I_sendto = 0x005d76f8, I_ntohs = 0x005d7700,
    I_recvfrom = 0x005d76fc, I_getsockopt = 0x005d7720,
    // TAPI32
    I_lineOpen = 0x005d763c, I_lineClose = 0x005d7638, I_lineDeallocateCall = 0x005d7634, I_lineDrop = 0x005d7630,
    I_lineGetID = 0x005d762c, I_lineGetDevConfig = 0x005d7628, I_lineSetDevConfig = 0x005d7624,
    I_lineGetCallInfo = 0x005d7620, I_lineMakeCall = 0x005d761c, I_lineAnswer = 0x005d7618,
    I_lineNegotiateAPIVersion = 0x005d7614, I_lineInitialize = 0x005d7610, I_lineShutdown = 0x005d760c,
    I_lineGetDevCaps = 0x005d7608,
    // KERNEL32
    I_CreateFileA = 0x005d7554, I_CloseHandle = 0x005d7548, I_GetLastError = 0x005d7540, I_ReadFile = 0x005d753c,
    I_WriteFile = 0x005d7538, I_GetCommState = 0x005d7510, I_SetCommState = 0x005d750c, I_GetCommProperties = 0x005d7508,
    I_GetCommModemStatus = 0x005d7504, I_SetCommTimeouts = 0x005d7500, I_ClearCommError = 0x005d74fc,
    I_WaitForSingleObject = 0x005d7480, I_CreateEventA = 0x005d7564,
    // ADVAPI32
    I_RegOpenKeyExA = 0x005d7418, I_RegQueryValueExA = 0x005d7414, I_RegCloseKey = 0x005d741c, I_RegSetValueExA = 0x005d7420,
};
// their types, as the game calls them (handles and pointers as 32-bit values)
typedef int(__stdcall* WSAStartup_f)(uint16_t, void*);
typedef int(__stdcall* Int0_f)(void);
typedef int(__stdcall* Int1_f)(uint32_t);
typedef uint32_t(__stdcall* Socket_f)(int, int, int);
typedef uint16_t(__stdcall* Short_f)(uint32_t);      // htons / ntohs: the pushed dword (only its low word counts)
typedef int(__stdcall* Bind_f)(uint32_t, const void*, int);
typedef int(__stdcall* Setsockopt_f)(uint32_t, int, int, const void*, int);
typedef int(__stdcall* Getsockopt_f)(uint32_t, int, int, void*, int*);
typedef int(__stdcall* Ioctl_f)(uint32_t, uint32_t, uint32_t*);
typedef int(__stdcall* Sendto_f)(uint32_t, const void*, int, int, const void*, int);
typedef int(__stdcall* Recvfrom_f)(uint32_t, void*, int, int, void*, int*);
typedef int(__stdcall* Hostname_f)(char*, int);
typedef void*(__stdcall* Hostbyname_f)(const char*);
typedef uint32_t(__stdcall* InetAddr_f)(const char*);
typedef uint32_t(__stdcall* AsyncHost_f)(void*, uint32_t, const char*, void*, int);
typedef int32_t(__stdcall* Tapi1_f)(uint32_t);
typedef int32_t(__stdcall* Tapi2_f)(uint32_t, void*);
typedef int32_t(__stdcall* Tapi3_f)(uint32_t, uint32_t, uint32_t);
typedef int32_t(__stdcall* LineOpen_f)(uint32_t, uint32_t, uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*);
typedef int32_t(__stdcall* LineGetID_f)(uint32_t, uint32_t, uint32_t, uint32_t, void*, const char*);
typedef int32_t(__stdcall* LineDevConfig_f)(uint32_t, void*, const char*);
typedef int32_t(__stdcall* LineSetDevConfig_f)(uint32_t, const void*, uint32_t, const char*);
typedef int32_t(__stdcall* LineMakeCall_f)(uint32_t, uint32_t*, const char*, uint32_t, const void*);
typedef int32_t(__stdcall* LineNegotiate_f)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, void*);
typedef int32_t(__stdcall* LineInitialize_f)(uint32_t*, uint32_t, uint32_t, const char*, uint32_t*);
typedef int32_t(__stdcall* LineGetDevCaps_f)(uint32_t, uint32_t, uint32_t, uint32_t, void*);
typedef uint32_t(__stdcall* CreateFileA_f)(const char*, uint32_t, uint32_t, void*, uint32_t, uint32_t, uint32_t);
typedef int(__stdcall* Handle_f)(uint32_t);
typedef uint32_t(__stdcall* Dword0_f)(void);
typedef int(__stdcall* ReadFile_f)(uint32_t, void*, uint32_t, uint32_t*, void*);
typedef int(__stdcall* WriteFile_f)(uint32_t, const void*, uint32_t, uint32_t*, void*);
typedef int(__stdcall* HandlePtr_f)(uint32_t, void*);
typedef int(__stdcall* ClearCommError_f)(uint32_t, uint32_t*, void*);
typedef uint32_t(__stdcall* Wait_f)(uint32_t, uint32_t);
typedef uint32_t(__stdcall* CreateEventA_f)(void*, int, int, const char*);
typedef int32_t(__stdcall* RegOpenKeyExA_f)(uint32_t, const char*, uint32_t, uint32_t, uint32_t*);
typedef int32_t(__stdcall* RegQueryValueExA_f)(uint32_t, const char*, uint32_t*, uint32_t*, void*, uint32_t*);
typedef int32_t(__stdcall* RegSetValueExA_f)(uint32_t, const char*, uint32_t, uint32_t, const void*, uint32_t);
typedef int32_t(__stdcall* RegCloseKey_f)(uint32_t);

}  // namespace nt
