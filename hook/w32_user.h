// w32_user.h -- the stand-ins for race.exe's USER32, GDI32 and ADVAPI32 imports (relink stage R2a; w32_handle.h says
// how stand-ins work). Win32 ABI: __stdcall, handles and pointers-as-ints as 32-bit values, structures as pointers.
//
// What runs where (the GCC standalone, platform = SDL):
//   * the window system -- the loading window (RegisterClassA / CreateWindowExA / ShowWindow / UpdateWindow / BeginPaint /
//     EndPaint / DefWindowProcA / DestroyWindow / IsWindow, LoadBitmapA + GDI32's five for its SPLASH picture, LoadCursorA,
//     GetSystemMetrics), the single-instance check (FindWindowA / ShowWindow), app_end's cursor and focus calls
//     (ClipCursor, ShowCursor, SetCursor, SetForegroundWindow, UnhookWindowsHookEx), the last-resort MessageBoxA and the
//     clipboard -- on Windows the real thing behind the stand-in (the HWNDs are real windows: SDL's and the loading
//     window), the thread's last error carried across both ways; on Linux (R2b) SDL where SDL has it (screen size,
//     message box, cursor, clipboard), the rest stubbed until the loading window is designed (see each #else).
//     Not called with the SDL platform layer (win32.obj's window procedure, message loop and keyboard hook are
//     replaced by platform.cpp): GetMessageA, PeekMessageA, TranslateMessage, DispatchMessageA, SetWindowsHookExA,
//     CallNextHookEx, PostQuitMessage, GetForegroundWindow, SetFocus, SetCursorPos, LoadIconA -- the same forwarding.
//   * MapVirtualKeyA (KeyConvertScanKey: scan code -> virtual key, MAPVK_VSC_TO_VK): Windows' own on Windows (the
//     player's keyboard layout); on Linux the US layout's table here, which test/w32_user_test.cpp checks against
//     Windows' US layout code by code.
//   * the registry (ADVAPI32): Config\registry.ini beside race.exe (Config-2\ for [test] two_copies' second copy) on
//     every OS -- never the real registry. The game's keys:
//       HKEY_USERS\.Default\Software\Microsoft\Windows\CurrentVersion\Internet Settings, value EnableAutoDial
//         (REG_BINARY, 4 bytes) -- winsock_grab's set_autodial(0): opened KEY_READ|KEY_WRITE (0x2001f), read, and
//         written 0 if it isn't (restored at winsock_release). Absent from registry.ini (the default): the open fails
//         ERROR_FILE_NOT_FOUND and the game changes nothing, as on a modern Windows (where the open is refused).
//       HKEY_LOCAL_MACHINE\<a TAPI modem's driver key>, value AttachedTo (REG_SZ "COMn") -- get_tapiline_port, read
//         only, and only for a TAPI line device: never, with TAPI not available (w32_net.h).
//       RegFlushKey on the six predefined roots at exit (app_end): the results Windows gives.
//     registry.ini: one [section] per key, its full path from the root's name (HKEY_USERS\.Default\...); one line per
//     value, name=REG_SZ:text | REG_DWORD:0x12345678 | REG_BINARY:00 01 ff ... | REG_<n>:hex bytes; the default
//     value's name is @. Names compare without case, as Windows'. A key exists if its section or one below it does.
#pragma once
#include <stdint.h>
#if !defined(_WIN32) && !defined(__stdcall)
#define __stdcall __attribute__((stdcall))               // (R2b: the Win32 ABI on Linux; mingw defines it)
#endif

// ---- USER32 ----------------------------------------------------------------------------------------------------------
int __stdcall w32_GetSystemMetrics(int index);
uint32_t __stdcall w32_RegisterClassA(const void* wndclass);                       // ATOM, zero-extended
uint32_t __stdcall w32_CreateWindowExA(uint32_t ex_style, const char* cls, const char* title, uint32_t style, int x, int y,
                                       int w, int h, uint32_t parent, uint32_t menu, uint32_t instance, void* param);
int __stdcall w32_DestroyWindow(uint32_t hwnd);
int __stdcall w32_IsWindow(uint32_t hwnd);
int __stdcall w32_ShowWindow(uint32_t hwnd, int cmd);
int __stdcall w32_UpdateWindow(uint32_t hwnd);
uint32_t __stdcall w32_FindWindowA(const char* cls, const char* title);
uint32_t __stdcall w32_SetFocus(uint32_t hwnd);
uint32_t __stdcall w32_GetForegroundWindow(void);
int __stdcall w32_SetForegroundWindow(uint32_t hwnd);
uint32_t __stdcall w32_BeginPaint(uint32_t hwnd, void* paint);
int __stdcall w32_EndPaint(uint32_t hwnd, const void* paint);
int32_t __stdcall w32_DefWindowProcA(uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam);
uint32_t __stdcall w32_LoadIconA(uint32_t instance, const char* name);
uint32_t __stdcall w32_LoadCursorA(uint32_t instance, const char* name);
uint32_t __stdcall w32_LoadBitmapA(uint32_t instance, const char* name);
uint32_t __stdcall w32_SetCursor(uint32_t cursor);
int __stdcall w32_ShowCursor(int show);
int __stdcall w32_ClipCursor(const void* rect);
int __stdcall w32_SetCursorPos(int x, int y);
int __stdcall w32_MessageBoxA(uint32_t hwnd, const char* text, const char* caption, uint32_t type);
int __stdcall w32_GetMessageA(void* msg, uint32_t hwnd, uint32_t first, uint32_t last);
int __stdcall w32_PeekMessageA(void* msg, uint32_t hwnd, uint32_t first, uint32_t last, uint32_t remove);
int __stdcall w32_TranslateMessage(const void* msg);
int32_t __stdcall w32_DispatchMessageA(const void* msg);
void __stdcall w32_PostQuitMessage(int code);
uint32_t __stdcall w32_SetWindowsHookExA(int id, void* proc, uint32_t instance, uint32_t thread);
int __stdcall w32_UnhookWindowsHookEx(uint32_t hook);
int32_t __stdcall w32_CallNextHookEx(uint32_t hook, int code, uint32_t wparam, int32_t lparam);
uint32_t __stdcall w32_MapVirtualKeyA(uint32_t code, uint32_t type);
int __stdcall w32_OpenClipboard(uint32_t hwnd);
int __stdcall w32_CloseClipboard(void);
int __stdcall w32_EmptyClipboard(void);
uint32_t __stdcall w32_SetClipboardData(uint32_t format, uint32_t mem);
uint32_t __stdcall w32_GetClipboardData(uint32_t format);
// not imported: the C runtime's fatal-error box finds them with GetProcAddress (w32_table.cpp's lookup answers)
uint32_t __stdcall w32_GetActiveWindow(void);
uint32_t __stdcall w32_GetLastActivePopup(uint32_t hwnd);

// ---- GDI32 -----------------------------------------------------------------------------------------------------------
int __stdcall w32_BitBlt(uint32_t dc, int x, int y, int w, int h, uint32_t src, int sx, int sy, uint32_t rop);
uint32_t __stdcall w32_CreateCompatibleDC(uint32_t dc);
int __stdcall w32_DeleteDC(uint32_t dc);
uint32_t __stdcall w32_SelectObject(uint32_t dc, uint32_t obj);
int __stdcall w32_DeleteObject(uint32_t obj);

// ---- ADVAPI32 (the registry, on Config\registry.ini) -----------------------------------------------------------------
int32_t __stdcall w32_RegOpenKeyExA(uint32_t key, const char* sub, uint32_t options, uint32_t sam, uint32_t* out);
int32_t __stdcall w32_RegQueryValueExA(uint32_t key, const char* name, uint32_t* reserved, uint32_t* type, void* data,
                                       uint32_t* size);
int32_t __stdcall w32_RegSetValueExA(uint32_t key, const char* name, uint32_t reserved, uint32_t type, const void* data,
                                     uint32_t size);
int32_t __stdcall w32_RegCloseKey(uint32_t key);
int32_t __stdcall w32_RegFlushKey(uint32_t key);

// the registry file (a test sets its own; the game's: <race.exe's folder>\Config<copy suffix>\registry.ini, decided at
// the first registry call)
void w32_registry_file(const char* path);

// MapVirtualKeyA(code, MAPVK_VSC_TO_VK) on the US layout -- the Linux branch's table (exposed for the test)
uint32_t w32_us_scan_to_vk(uint32_t scan);
