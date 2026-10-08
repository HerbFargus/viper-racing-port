// w32_net.h -- the stand-ins for race.exe's WSOCK32 and TAPI32 imports and KERNEL32's serial-port functions (relink
// stage R2a; w32_handle.h says how stand-ins work). Win32 ABI: __stdcall, handles and pointers-as-ints 32-bit.
//
// WSOCK32 (18, imported by ordinal; socket.obj's UDPSocket / IPXSocket): BSD sockets underneath -- on Windows winsock 2
// (ws2_32.dll, the same sockets wsock32 hands out: wsock32's exports are ws2_32's), on Linux POSIX sockets with
// Windows' numbers, structures and error codes put back (the #else parts). Every failure leaves its WSA error as the
// thread's last error (w32_handle.h), which WSAGetLastError -- and GetLastError, as on Windows -- return. inet_addr,
// htons and ntohs are computed here on every OS. IPX (socket(AF_IPX = 6, ...)): not available, as on a Windows without
// IPX -- INVALID_SOCKET, WSAEAFNOSUPPORT. WSAAsyncGetHostByName: winsock's own on Windows (its reply is posted to the
// game's window, where SDL's message hook hands it to the game's message hooks: net_wsock.cpp's async_msg_hook); on
// Linux a thread that looks the name up, writes winsock's hostent layout into the caller's buffer and hands the reply
// to w32_set_message_poster's function (R2b: the SDL platform layer, which calls the game's message hooks).
//
// TAPI32 (14): not available, on every OS -- TAPI as Windows has it on a PC with no modem: lineInitialize succeeds with
// no line devices, and every call on a device, line or call fails as TAPI fails it then (LINEERR_BADDEVICEID,
// LINEERR_INVALLINEHANDLE, LINEERR_INVALCALLHANDLE, ...), so the game lists no modem. test/w32_user_test.cpp compares
// each against the real TAPI32 of a PC without one.
// Serial (KERNEL32: GetCommState, SetCommState, SetCommTimeouts, GetCommModemStatus, GetCommProperties,
// ClearCommError): no COM ports. The game reaches them only with a COM port CreateFileA opened ("COM1".."COM4", which
// fail ERROR_FILE_NOT_FOUND without one -- w32_file.cpp's CreateFileA) or a TAPI modem's (none), so they fail as Windows
// fails them on any handle that isn't a COM port: a disk file ERROR_INVALID_PARAMETER, a console ERROR_INVALID_FUNCTION,
// any other object or an unknown handle ERROR_INVALID_HANDLE (checked against Windows; w32_net.cpp).
#pragma once
#include <stdint.h>
#if !defined(_WIN32) && !defined(__stdcall)
#define __stdcall __attribute__((stdcall))               // (R2b: the Win32 ABI on Linux; mingw defines it)
#endif

// ---- WSOCK32 (ordinal) -------------------------------------------------------------------------------------------------
int __stdcall w32_WSAStartup(uint16_t version, void* wsadata);                            // #115
int __stdcall w32_WSACleanup(void);                                                       // #116
int __stdcall w32_WSAGetLastError(void);                                                  // #111
uint32_t __stdcall w32_socket(int af, int type, int protocol);                            // #23
int __stdcall w32_closesocket(uint32_t s);                                                // #3
int __stdcall w32_bind(uint32_t s, const void* addr, int len);                            // #2
int __stdcall w32_setsockopt(uint32_t s, int level, int name, const void* value, int len);   // #21
int __stdcall w32_getsockopt(uint32_t s, int level, int name, void* value, int* len);     // #7
int __stdcall w32_ioctlsocket(uint32_t s, int32_t cmd, uint32_t* arg);                    // #12
int __stdcall w32_sendto(uint32_t s, const void* buf, int len, int flags, const void* to, int tolen);   // #20
int __stdcall w32_recvfrom(uint32_t s, void* buf, int len, int flags, void* from, int* fromlen);     // #17
uint16_t __stdcall w32_htons(uint16_t v);                                                 // #9
uint16_t __stdcall w32_ntohs(uint16_t v);                                                 // #15
uint32_t __stdcall w32_inet_addr(const char* text);                                       // #10
int __stdcall w32_gethostname(char* name, int len);                                       // #57
void* __stdcall w32_gethostbyname(const char* name);                                      // #52
uint32_t __stdcall w32_WSAAsyncGetHostByName(uint32_t hwnd, uint32_t msg, const char* name, char* buf, int buflen);   // #103
int __stdcall w32_WSACancelAsyncRequest(uint32_t request);                                // #108

// R2b: who delivers an async lookup's reply (PostMessage's job on Windows) -- the platform layer, to the game's message
// hooks. Unused on Windows.
typedef void (*W32MessagePoster)(uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam);
void w32_set_message_poster(W32MessagePoster post);
// the POSIX branch's plain-C++ parts, on every OS (the test compares them with Windows'): WSAStartup's WSADATA, and a
// hostent laid out in a buffer as winsock lays it out (its size; 0 if it doesn't fit cap, *need says how much would)
void w32_wsadata_fill(uint16_t version, void* wsadata);
uint32_t w32_hostent_lay(char* buf, uint32_t cap, const char* name, const uint32_t* addrs, uint32_t n, uint32_t* need);

// ---- TAPI32 ----------------------------------------------------------------------------------------------------------
int32_t __stdcall w32_lineInitialize(uint32_t* line_app, uint32_t instance, void* callback, const char* app,
                                     uint32_t* devices);
int32_t __stdcall w32_lineShutdown(uint32_t line_app);
int32_t __stdcall w32_lineNegotiateAPIVersion(uint32_t line_app, uint32_t device, uint32_t lo, uint32_t hi,
                                              uint32_t* version, void* ext_id);
int32_t __stdcall w32_lineGetDevCaps(uint32_t line_app, uint32_t device, uint32_t version, uint32_t ext_version,
                                     void* caps);
int32_t __stdcall w32_lineOpen(uint32_t line_app, uint32_t device, uint32_t* line, uint32_t version, uint32_t ext_version,
                               uint32_t callback_instance, uint32_t privileges, uint32_t media_modes, void* params);
int32_t __stdcall w32_lineClose(uint32_t line);
int32_t __stdcall w32_lineMakeCall(uint32_t line, uint32_t* call, const char* dest, uint32_t country, const void* params);
int32_t __stdcall w32_lineAnswer(uint32_t call, const char* user_info, uint32_t size);
int32_t __stdcall w32_lineDrop(uint32_t call, const char* user_info, uint32_t size);
int32_t __stdcall w32_lineDeallocateCall(uint32_t call);
int32_t __stdcall w32_lineGetCallInfo(uint32_t call, void* info);
int32_t __stdcall w32_lineGetID(uint32_t line, uint32_t address, uint32_t call, uint32_t select, void* id,
                                const char* device_class);
int32_t __stdcall w32_lineGetDevConfig(uint32_t device, void* config, const char* device_class);
int32_t __stdcall w32_lineSetDevConfig(uint32_t device, const void* config, uint32_t size, const char* device_class);

// ---- KERNEL32: the serial port -----------------------------------------------------------------------------------------
int __stdcall w32_GetCommState(uint32_t file, void* dcb);
int __stdcall w32_SetCommState(uint32_t file, void* dcb);
int __stdcall w32_SetCommTimeouts(uint32_t file, void* timeouts);
int __stdcall w32_GetCommModemStatus(uint32_t file, uint32_t* status);
int __stdcall w32_GetCommProperties(uint32_t file, void* props);
int __stdcall w32_ClearCommError(uint32_t file, uint32_t* errors, void* stat);
