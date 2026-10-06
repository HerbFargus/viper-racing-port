// w32_user.cpp -- the stand-ins for race.exe's USER32, GDI32 and ADVAPI32 imports (w32_user.h says what runs where).
//
// The window system is Windows' own on Windows, behind the stand-in: each forwarded call carries the stand-ins' last error
// (w32_handle.h) into the thread's real one before the call and back after it, so GetLastError -- a stand-in -- reads
// what the real call left, as when the game called Windows directly. The functions are looked up by name in their
// system DLL at the first call (gdi32 isn't linked into the port DLL, and the build scripts stay as they are).
// The registry is portable C++ on an ini file (std::string, stdio), the same on every OS.
#define _CRT_SECURE_NO_WARNINGS
#include "w32_user.h"
#include "w32_handle.h"
#include "w32_kernel.h"                                   // the clipboard's memory: GlobalAlloc / GlobalLock
#include "fix_paths.h"
#include "w32_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <map>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include "SDL.h"
#endif

namespace {

#ifdef _WIN32
// ---- Windows behind the stand-in -------------------------------------------------------------------------------------
FARPROC sys_proc(const char* dll, const char* name) {
    HMODULE m = GetModuleHandleA(dll);
    if (!m) m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, name) : 0;
}
// the stand-ins' last error into the thread's real one for the call, and what the call left back out
struct Bridge {
    Bridge() { SetLastError(w32::last_error()); }
    ~Bridge() { w32::set_last_error(GetLastError()); }
};
// `Name`'s real function from `dll`, looked up once, as f_Name
#define SYS(dll, Name, Ret, Params) \
    static Ret(WINAPI* const f_##Name) Params = (Ret(WINAPI*) Params)(void*)sys_proc(dll, #Name)
#define U32(Name, Ret, Params) SYS("user32.dll", Name, Ret, Params)
#define GDI(Name, Ret, Params) SYS("gdi32.dll", Name, Ret, Params)
#define H(v) ((void*)(uintptr_t)(v))                 // a 32-bit handle as Windows' pointer-sized one
#define V(p) ((uint32_t)(uintptr_t)(p))
#else
// ---- R2b (Linux): the windows, on SDL -------------------------------------------------------------------------------------
// The game's windows are SDL windows behind fake HWNDs (the window table below, in the #else of USER32):
//   * the loading window (make_loading_window / splash_proc, krn_file.cpp): RegisterClassA keeps the class's window
//     procedure; CreateWindowExA makes a hidden borderless SDL window and sends it WM_CREATE (-1: destroyed again, 0
//     returned, as Windows does), ShowWindow shows it, UpdateWindow sends the pending WM_PAINT; BeginPaint's DC is the
//     window's SDL surface, EndPaint presents it. LoadBitmapA reads the bitmap resource (SPLASH: 300x373, 24-bit) from
//     race.exe's .rsrc in the mapped image; CreateCompatibleDC / SelectObject / BitBlt (SRCCOPY) / DeleteDC /
//     DeleteObject are SDL surfaces and blits. DestroyWindow sends WM_DESTROY and destroys the SDL window.
//   * the game's window: platform.cpp's SDL window, adopted (w32_adopt_window) -- its HWND is what the game holds; it
//     has no window procedure (SDL's events go through platform.cpp's handle()), and DestroyWindow (app_end) hides it.
//   * the single-instance check: start_unique_instance's semaphore is a lock file (w32_thread.cpp); its FindWindowA
//     finds this process's windows by class and title, and another process's game window through a lock file each
//     window with a class and a title holds ($XDG_RUNTIME_DIR, else /tmp: viperport-window-<class>-<title>, flock'd for
//     the window's life) -- a fake HWND that ShowWindow can't restore (one process can't raise another's SDL window).
//   * GetForegroundWindow / GetActiveWindow: the window of ours with the keyboard focus; SetForegroundWindow raises it.
// All of it on the thread that made the windows (the game's main thread), as Windows' windows belong to theirs.
#endif

// ---- the registry: Config\registry.ini ---------------------------------------------------------------------------------
std::mutex g_reg_mu;
std::string g_reg_path;                                // the file; empty: decided at the first call
bool g_reg_loaded;
std::vector<std::string> g_lines;                      // the file as it is (comments and order kept on writes)

enum : uint32_t {
    HKCR = 0x80000000u, HKCU = 0x80000001u, HKLM = 0x80000002u, HKU = 0x80000003u, HKPD = 0x80000004u,
    HKCC = 0x80000005u, HKDD = 0x80000006u,
    KEY_QUERY_VALUE_ = 1, KEY_SET_VALUE_ = 2,
    ERROR_MORE_DATA_ = 234, ERROR_BADKEY_ = 1010,
};
const char* const k_roots[] = {"HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS",
                               "HKEY_PERFORMANCE_DATA", "HKEY_CURRENT_CONFIG", "HKEY_DYN_DATA"};

struct RegKey : w32::Object {
    RegKey(const std::string& p, uint32_t s) : Object(w32::Kind::Other), path(p), sam(s) {}
    std::string path;                                  // "HKEY_USERS\.Default\Software\..."
    uint32_t sam;                                      // the access it was opened with
};

bool same(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}
// a line's section name ("" if it isn't a [section] line)
std::string section_of(const std::string& line) {
    const std::string t = trim(line);
    if (t.size() < 2 || t[0] != '[' || t[t.size() - 1] != ']') return "";
    return trim(t.substr(1, t.size() - 2));
}

// the file as the OS opens it (the path layer, w32_path.h: on Linux the install folder is C:\, names any case)
std::string host_path() {
    w32::path::Resolved r;
    uint32_t err;
    return w32::path::resolve(g_reg_path.c_str(), r, &err) ? r.host : g_reg_path;
}

void reg_load() {
    if (g_reg_loaded) return;
    g_reg_loaded = true;
    if (g_reg_path.empty()) {                          // <race.exe's folder>\Config<suffix>\registry.ini
        char dir[300];
        const uint32_t n = vp_exe_dir(dir, sizeof dir - 40);
        if (n) g_reg_path = std::string(dir) + "Config" + vp_g_copy_suffix + "\\registry.ini";
        else g_reg_path = "Config\\registry.ini";
    }
    FILE* f = fopen(host_path().c_str(), "rb");
    if (!f) return;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    fclose(f);
    size_t at = 0;
    while (at < all.size()) {
        size_t e = all.find('\n', at);
        if (e == std::string::npos) e = all.size();
        std::string line = all.substr(at, e - at);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        g_lines.push_back(line);
        at = e + 1;
    }
}

bool reg_save() {
    FILE* f = fopen(host_path().c_str(), "wb");
    if (!f) return false;
    for (const std::string& l : g_lines) fprintf(f, "%s\r\n", l.c_str());
    return fclose(f) == 0;
}

// does the key exist (its section, or one below it)?
bool key_exists(const std::string& path) {
    if (path.find('\\') == std::string::npos) return true;      // a root
    for (const std::string& l : g_lines) {
        const std::string s = section_of(l);
        if (s.empty()) continue;
        if (same(s, path)) return true;
        if (s.size() > path.size() && s[path.size()] == '\\' && same(s.substr(0, path.size()), path)) return true;
    }
    return false;
}

// the line of value `name` in section `path` (or -1); *end: the line after the section's last value line
int find_value(const std::string& path, const std::string& name, int* end) {
    int in = -1, found = -1, last = -1;
    for (int i = 0; i < (int)g_lines.size(); i++) {
        const std::string s = section_of(g_lines[i]);
        if (!s.empty() || (trim(g_lines[i]).size() && trim(g_lines[i])[0] == '[')) {
            in = same(s, path) ? i : -1;
            if (in >= 0) last = i;
            continue;
        }
        if (in < 0) continue;
        const std::string t = trim(g_lines[i]);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        last = i;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (found < 0 && same(trim(t.substr(0, eq)), name)) found = i;
    }
    if (end) *end = last < 0 ? -1 : last + 1;
    return found;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
std::vector<uint8_t> parse_hex(const std::string& s) {
    std::vector<uint8_t> v;
    int hi = -1;
    for (char c : s) {
        const int h = hexval(c);
        if (h < 0) { hi = -1; continue; }
        if (hi < 0) hi = h;
        else { v.push_back((uint8_t)(hi * 16 + h)); hi = -1; }
    }
    return v;
}
// a value line's type and bytes (false: not one we can read)
bool parse_value(const std::string& line, uint32_t* type, std::vector<uint8_t>* data) {
    const size_t eq = line.find('=');
    const std::string v = trim(line.substr(eq + 1));
    const size_t colon = v.find(':');
    if (colon == std::string::npos) return false;
    const std::string t = v.substr(0, colon), rest = v.substr(colon + 1);
    if (same(t, "REG_SZ")) {
        *type = 1;
        data->assign(rest.begin(), rest.end());
        data->push_back(0);
    } else if (same(t, "REG_DWORD")) {
        *type = 4;
        const uint32_t d = (uint32_t)strtoul(trim(rest).c_str(), 0, 0);
        data->assign((const uint8_t*)&d, (const uint8_t*)&d + 4);
    } else if (same(t, "REG_BINARY")) {
        *type = 3;
        *data = parse_hex(rest);
    } else if (t.size() > 4 && same(t.substr(0, 4), "REG_") && isdigit((unsigned char)t[4])) {
        *type = (uint32_t)strtoul(t.c_str() + 4, 0, 10);
        *data = parse_hex(rest);
    } else {
        return false;
    }
    return true;
}
std::string format_value(const std::string& name, uint32_t type, const uint8_t* p, uint32_t n) {
    char tmp[16];
    std::string out = name + "=";
    bool text = type == 1 && n >= 1 && p[n - 1] == 0;
    for (uint32_t i = 0; text && i + 1 < n; i++)
        if (!p[i] || p[i] == '\r' || p[i] == '\n') text = false;
    if (text) return out + "REG_SZ:" + std::string((const char*)p, n - 1);
    if (type == 4 && n == 4) {
        uint32_t d;
        memcpy(&d, p, 4);
        snprintf(tmp, sizeof tmp, "0x%08x", d);
        return out + "REG_DWORD:" + tmp;
    }
    if (type == 3) out += "REG_BINARY:";
    else { snprintf(tmp, sizeof tmp, "REG_%u:", type); out += tmp; }
    for (uint32_t i = 0; i < n; i++) {
        snprintf(tmp, sizeof tmp, i ? " %02x" : "%02x", p[i]);
        out += tmp;
    }
    return out;
}

// the key behind a handle: a predefined root, or one RegOpenKeyExA opened. False: not a key.
bool key_of(uint32_t h, std::string* path, uint32_t* sam) {
    if (h >= HKCR && h <= HKDD) {
        *path = k_roots[h - HKCR];
        *sam = 0xf003f;                                // (a root takes any access)
        return true;
    }
    std::shared_ptr<w32::Object> o = w32::get(h, w32::Kind::Other);
    RegKey* k = o ? dynamic_cast<RegKey*>(o.get()) : 0;
    if (!k) return false;
    *path = k->path;
    *sam = k->sam;
    return true;
}

#ifndef _WIN32
// ---- the clipboard's memory (Linux: SDL's clipboard holds text, the game's blocks are the kernel stand-ins') ------------
// the text behind one of the game's global memory handles (CF_TEXT: up to its terminator)
std::string global_text(uint32_t mem) {
    std::string s;
    if (const char* p = (const char*)w32_GlobalLock(mem)) {
        s = p;
        w32_GlobalUnlock(mem);
    }
    return s;
}
// a new moveable global block (the game's kind) holding text
uint32_t text_global(const std::string& s) {
    const uint32_t h = w32_GlobalAlloc(0x2002, (uint32_t)s.size() + 1);      // GMEM_MOVEABLE | GMEM_DDESHARE
    if (!h) return 0;
    if (char* p = (char*)w32_GlobalLock(h)) {
        memcpy(p, s.c_str(), s.size() + 1);
        w32_GlobalUnlock(h);
    }
    return h;
}
#endif

}  // namespace

// =========================================================================================================================
// USER32
// =========================================================================================================================
#ifdef _WIN32

int __stdcall w32_GetSystemMetrics(int index) {
    U32(GetSystemMetrics, int, (int));
    Bridge b;
    return f_GetSystemMetrics(index);
}
uint32_t __stdcall w32_RegisterClassA(const void* wc) {
    U32(RegisterClassA, ATOM, (const WNDCLASSA*));
    Bridge b;
    return f_RegisterClassA((const WNDCLASSA*)wc);
}
uint32_t __stdcall w32_CreateWindowExA(uint32_t ex_style, const char* cls, const char* title, uint32_t style, int x, int y,
                                       int w, int h, uint32_t parent, uint32_t menu, uint32_t instance, void* param) {
    U32(CreateWindowExA, HWND, (DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID));
    Bridge b;
    return V(f_CreateWindowExA(ex_style, cls, title, style, x, y, w, h, (HWND)H(parent), (HMENU)H(menu),
                               (HINSTANCE)H(instance), param));
}
int __stdcall w32_DestroyWindow(uint32_t hwnd) {
    U32(DestroyWindow, BOOL, (HWND));
    Bridge b;
    return f_DestroyWindow((HWND)H(hwnd));
}
int __stdcall w32_IsWindow(uint32_t hwnd) {
    U32(IsWindow, BOOL, (HWND));
    Bridge b;
    return f_IsWindow((HWND)H(hwnd));
}
int __stdcall w32_ShowWindow(uint32_t hwnd, int cmd) {
    U32(ShowWindow, BOOL, (HWND, int));
    Bridge b;
    return f_ShowWindow((HWND)H(hwnd), cmd);
}
int __stdcall w32_UpdateWindow(uint32_t hwnd) {
    U32(UpdateWindow, BOOL, (HWND));
    Bridge b;
    return f_UpdateWindow((HWND)H(hwnd));
}
uint32_t __stdcall w32_FindWindowA(const char* cls, const char* title) {
    U32(FindWindowA, HWND, (LPCSTR, LPCSTR));
    Bridge b;
    return V(f_FindWindowA(cls, title));
}
uint32_t __stdcall w32_SetFocus(uint32_t hwnd) {
    U32(SetFocus, HWND, (HWND));
    Bridge b;
    return V(f_SetFocus((HWND)H(hwnd)));
}
uint32_t __stdcall w32_GetForegroundWindow(void) {
    U32(GetForegroundWindow, HWND, (void));
    Bridge b;
    return V(f_GetForegroundWindow());
}
int __stdcall w32_SetForegroundWindow(uint32_t hwnd) {
    U32(SetForegroundWindow, BOOL, (HWND));
    Bridge b;
    return f_SetForegroundWindow((HWND)H(hwnd));
}
uint32_t __stdcall w32_BeginPaint(uint32_t hwnd, void* paint) {
    U32(BeginPaint, HDC, (HWND, LPPAINTSTRUCT));
    Bridge b;
    return V(f_BeginPaint((HWND)H(hwnd), (LPPAINTSTRUCT)paint));
}
int __stdcall w32_EndPaint(uint32_t hwnd, const void* paint) {
    U32(EndPaint, BOOL, (HWND, const PAINTSTRUCT*));
    Bridge b;
    return f_EndPaint((HWND)H(hwnd), (const PAINTSTRUCT*)paint);
}
int32_t __stdcall w32_DefWindowProcA(uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam) {
    U32(DefWindowProcA, LRESULT, (HWND, UINT, WPARAM, LPARAM));
    Bridge b;
    return (int32_t)f_DefWindowProcA((HWND)H(hwnd), msg, wparam, lparam);
}
uint32_t __stdcall w32_LoadIconA(uint32_t instance, const char* name) {
    U32(LoadIconA, HICON, (HINSTANCE, LPCSTR));
    Bridge b;
    return V(f_LoadIconA((HINSTANCE)H(instance), name));
}
uint32_t __stdcall w32_LoadCursorA(uint32_t instance, const char* name) {
    U32(LoadCursorA, HCURSOR, (HINSTANCE, LPCSTR));
    Bridge b;
    return V(f_LoadCursorA((HINSTANCE)H(instance), name));
}
uint32_t __stdcall w32_LoadBitmapA(uint32_t instance, const char* name) {
    U32(LoadBitmapA, HBITMAP, (HINSTANCE, LPCSTR));
    Bridge b;
    return V(f_LoadBitmapA((HINSTANCE)H(instance), name));
}
uint32_t __stdcall w32_SetCursor(uint32_t cursor) {
    U32(SetCursor, HCURSOR, (HCURSOR));
    Bridge b;
    return V(f_SetCursor((HCURSOR)H(cursor)));
}
int __stdcall w32_ShowCursor(int show) {
    U32(ShowCursor, int, (BOOL));
    Bridge b;
    return f_ShowCursor(show);
}
int __stdcall w32_ClipCursor(const void* rect) {
    U32(ClipCursor, BOOL, (const RECT*));
    Bridge b;
    return f_ClipCursor((const RECT*)rect);
}
int __stdcall w32_SetCursorPos(int x, int y) {
    U32(SetCursorPos, BOOL, (int, int));
    Bridge b;
    return f_SetCursorPos(x, y);
}
int __stdcall w32_MessageBoxA(uint32_t hwnd, const char* text, const char* caption, uint32_t type) {
    U32(MessageBoxA, int, (HWND, LPCSTR, LPCSTR, UINT));
    Bridge b;
    return f_MessageBoxA((HWND)H(hwnd), text, caption, type);
}
int __stdcall w32_GetMessageA(void* msg, uint32_t hwnd, uint32_t first, uint32_t last) {
    U32(GetMessageA, BOOL, (LPMSG, HWND, UINT, UINT));
    Bridge b;
    return f_GetMessageA((LPMSG)msg, (HWND)H(hwnd), first, last);
}
int __stdcall w32_PeekMessageA(void* msg, uint32_t hwnd, uint32_t first, uint32_t last, uint32_t remove) {
    U32(PeekMessageA, BOOL, (LPMSG, HWND, UINT, UINT, UINT));
    Bridge b;
    return f_PeekMessageA((LPMSG)msg, (HWND)H(hwnd), first, last, remove);
}
int __stdcall w32_TranslateMessage(const void* msg) {
    U32(TranslateMessage, BOOL, (const MSG*));
    Bridge b;
    return f_TranslateMessage((const MSG*)msg);
}
int32_t __stdcall w32_DispatchMessageA(const void* msg) {
    U32(DispatchMessageA, LRESULT, (const MSG*));
    Bridge b;
    return (int32_t)f_DispatchMessageA((const MSG*)msg);
}
void __stdcall w32_PostQuitMessage(int code) {
    U32(PostQuitMessage, void, (int));
    Bridge b;
    f_PostQuitMessage(code);
}
uint32_t __stdcall w32_SetWindowsHookExA(int id, void* proc, uint32_t instance, uint32_t thread) {
    U32(SetWindowsHookExA, HHOOK, (int, HOOKPROC, HINSTANCE, DWORD));
    Bridge b;
    return V(f_SetWindowsHookExA(id, (HOOKPROC)proc, (HINSTANCE)H(instance), thread));
}
int __stdcall w32_UnhookWindowsHookEx(uint32_t hook) {
    U32(UnhookWindowsHookEx, BOOL, (HHOOK));
    Bridge b;
    return f_UnhookWindowsHookEx((HHOOK)H(hook));
}
int32_t __stdcall w32_CallNextHookEx(uint32_t hook, int code, uint32_t wparam, int32_t lparam) {
    U32(CallNextHookEx, LRESULT, (HHOOK, int, WPARAM, LPARAM));
    Bridge b;
    return (int32_t)f_CallNextHookEx((HHOOK)H(hook), code, wparam, lparam);
}
// the player's keyboard layout, as before (the US table below is Linux's)
uint32_t __stdcall w32_MapVirtualKeyA(uint32_t code, uint32_t type) {
    U32(MapVirtualKeyA, UINT, (UINT, UINT));
    Bridge b;
    return f_MapVirtualKeyA(code, type);
}

// The clipboard: Windows' own. Its memory is the game's GlobalAlloc blocks, which are Windows' own HGLOBALs on Windows
// (w32_kernel.h), so they pass straight through: SetClipboardData hands the game's block to Windows, which owns it
// from then on, and GetClipboardData's block is Windows', which the game's GlobalLock locks.
int __stdcall w32_OpenClipboard(uint32_t hwnd) {
    U32(OpenClipboard, BOOL, (HWND));
    Bridge b;
    return f_OpenClipboard((HWND)H(hwnd));
}
int __stdcall w32_CloseClipboard(void) {
    U32(CloseClipboard, BOOL, (void));
    Bridge b;
    return f_CloseClipboard();
}
int __stdcall w32_EmptyClipboard(void) {
    U32(EmptyClipboard, BOOL, (void));
    Bridge b;
    return f_EmptyClipboard();
}
uint32_t __stdcall w32_SetClipboardData(uint32_t format, uint32_t mem) {
    U32(SetClipboardData, HANDLE, (UINT, HANDLE));
    Bridge b;
    return V(f_SetClipboardData(format, H(mem)));
}
uint32_t __stdcall w32_GetClipboardData(uint32_t format) {
    U32(GetClipboardData, HANDLE, (UINT));
    Bridge b;
    return V(f_GetClipboardData(format));
}
// (not imported: the C runtime's fatal-error message box finds them with GetProcAddress -- w32_kernel.h)
uint32_t __stdcall w32_GetActiveWindow(void) {
    U32(GetActiveWindow, HWND, (void));
    Bridge b;
    return V(f_GetActiveWindow());
}
uint32_t __stdcall w32_GetLastActivePopup(uint32_t hwnd) {
    U32(GetLastActivePopup, HWND, (HWND));
    Bridge b;
    return V(f_GetLastActivePopup((HWND)H(hwnd)));
}

// =========================================================================================================================
// GDI32
// =========================================================================================================================
int __stdcall w32_BitBlt(uint32_t dc, int x, int y, int w, int h, uint32_t src, int sx, int sy, uint32_t rop) {
    GDI(BitBlt, BOOL, (HDC, int, int, int, int, HDC, int, int, DWORD));
    Bridge b;
    return f_BitBlt((HDC)H(dc), x, y, w, h, (HDC)H(src), sx, sy, rop);
}
uint32_t __stdcall w32_CreateCompatibleDC(uint32_t dc) {
    GDI(CreateCompatibleDC, HDC, (HDC));
    Bridge b;
    return V(f_CreateCompatibleDC((HDC)H(dc)));
}
int __stdcall w32_DeleteDC(uint32_t dc) {
    GDI(DeleteDC, BOOL, (HDC));
    Bridge b;
    return f_DeleteDC((HDC)H(dc));
}
uint32_t __stdcall w32_SelectObject(uint32_t dc, uint32_t obj) {
    GDI(SelectObject, HGDIOBJ, (HDC, HGDIOBJ));
    Bridge b;
    return V(f_SelectObject((HDC)H(dc), H(obj)));
}
int __stdcall w32_DeleteObject(uint32_t obj) {
    GDI(DeleteObject, BOOL, (HGDIOBJ));
    Bridge b;
    return f_DeleteObject(H(obj));
}

#else  // R2b (Linux): the windows on SDL (see the top); SDL's screen size, message box, cursor and clipboard

namespace {

uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
bool is_int_resource(const char* name) { return (uintptr_t)name < 0x10000; }

// SDL's video, started by the first call that needs it (the loading window comes before platform.cpp's window)
bool video() {
    static int state;                                  // 0 not tried, 1 up, -1 failed
    if (SDL_WasInit(SDL_INIT_VIDEO)) return true;
    if (!state) state = SDL_InitSubSystem(SDL_INIT_VIDEO) == 0 ? 1 : -1;
    return state > 0;
}

// ---- race.exe's resources, read from its mapped image -----------------------------------------------------------------
// The data of resource (type, name) in the image at `instance` -- race.exe's base, the only module there is (its headers
// are mapped, as Windows maps them) -- or null. name: an integer resource, "#<n>", or a name (any case, as Windows').
const uint8_t* find_resource(uint32_t instance, uint32_t type, const char* name, uint32_t* size) {
    if (!instance || instance != w32_GetModuleHandleA(0)) return 0;
    const uint8_t* b = (const uint8_t*)(uintptr_t)instance;
    if (b[0] != 'M' || b[1] != 'Z') return 0;
    const uint32_t pe = rd32(b + 0x3c);
    if (pe > 0x800 || memcmp(b + pe, "PE\0\0", 4) != 0) return 0;
    const uint8_t* opt = b + pe + 24;
    if (rd16(opt) != 0x10b || rd32(opt + 92) <= 2) return 0;      // PE32, with a resource directory entry
    const uint32_t image = rd32(opt + 56), root = rd32(opt + 96 + 16), rsize = rd32(opt + 96 + 20);
    if (!root || rsize < 16 || root > image || rsize > image - root) return 0;
    const uint8_t* r = b + root;
    uint32_t want_id = 0;
    std::string want_name;
    if (is_int_resource(name)) want_id = (uint32_t)(uintptr_t)name;
    else if (name[0] == '#') want_id = (uint32_t)strtoul(name + 1, 0, 10);
    else for (const char* p = name; *p; p++) want_name += (char)toupper((unsigned char)*p);
    // one level of the tree: the entry with that id (or name; any: the first), its offset in the directory
    auto pick = [&](uint32_t dir, bool any, uint32_t id, const std::string& nm, uint32_t* out) -> bool {
        if (dir > rsize - 16) return false;
        const uint32_t named = rd16(r + dir + 12), ids = rd16(r + dir + 14);
        if (16 + (uint64_t)(named + ids) * 8 > rsize - dir) return false;
        for (uint32_t i = 0; i < named + ids; i++) {
            const uint8_t* e = r + dir + 16 + i * 8;
            const uint32_t n = rd32(e), off = rd32(e + 4);
            bool match = any;
            if (!match && (n & 0x80000000u) && !nm.empty()) {
                const uint32_t so = n & 0x7fffffffu;
                if (so <= rsize - 2) {
                    const uint32_t len = rd16(r + so);
                    if (len == nm.size() && so + 2 + (uint64_t)len * 2 <= rsize) {
                        match = true;
                        for (uint32_t k = 0; k < len && match; k++) {
                            const uint16_t c = rd16(r + so + 2 + k * 2);
                            match = c < 0x80 && toupper(c) == (unsigned char)nm[k];
                        }
                    }
                }
            } else if (!match && !(n & 0x80000000u) && nm.empty()) {
                match = n == id;
            }
            if (match) { *out = off; return true; }
        }
        return false;
    };
    uint32_t a, c, d;
    if (!pick(0, false, type, std::string(), &a) || !(a & 0x80000000u)) return 0;
    if (!pick(a & 0x7fffffffu, false, want_id, want_name, &c) || !(c & 0x80000000u)) return 0;
    if (!pick(c & 0x7fffffffu, true, 0, std::string(), &d) || (d & 0x80000000u) || d > rsize - 16) return 0;
    const uint32_t rva = rd32(r + d), n = rd32(r + d + 4);
    if (rva > image || n > image - rva) return 0;
    *size = n;
    return b + rva;
}

// A device-independent bitmap (BITMAPINFOHEADER, its colour table, its bits) as a 32-bit ARGB SDL surface; an icon's
// (height doubled: the colour bits, then the 1-bit AND mask) with the mask as its alpha. Null if it isn't one we read.
SDL_Surface* dib_surface(const uint8_t* p, uint32_t n, bool icon) {
    if (n < 40) return 0;
    const uint32_t hs = rd32(p);
    if (hs < 40 || hs > n) return 0;
    const int32_t w = (int32_t)rd32(p + 4), h_all = (int32_t)rd32(p + 8);
    const uint32_t bpp = rd16(p + 14), comp = rd32(p + 16), used = rd32(p + 32);
    const bool top_down = h_all < 0;
    const int32_t h = (top_down ? -h_all : h_all) / (icon ? 2 : 1);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) return 0;
    if (!(comp == 0 || (comp == 3 && (bpp == 16 || bpp == 32)))) return 0;      // BI_RGB, BI_BITFIELDS
    uint32_t masks[3] = {bpp == 16 ? 0x7c00u : 0xff0000u, bpp == 16 ? 0x03e0u : 0xff00u, bpp == 16 ? 0x001fu : 0xffu};
    uint32_t at = hs;
    if (comp == 3) {
        if (hs == 40) {
            if (at + 12 > n) return 0;
            at += 12;
        }
        for (int i = 0; i < 3; i++) masks[i] = rd32(p + 40 + i * 4);
    }
    const uint32_t colours = bpp <= 8 ? (used && used < (1u << bpp) ? used : 1u << bpp) : used;
    const uint8_t* pal = p + at;
    if ((uint64_t)at + colours * 4 > n) return 0;
    at += colours * 4;
    const uint32_t stride = ((uint32_t)w * bpp + 31) / 32 * 4, mstride = ((uint32_t)w + 31) / 32 * 4;
    if ((uint64_t)at + (uint64_t)stride * h + (icon ? (uint64_t)mstride * h : 0) > n) return 0;
    const uint8_t* bits = p + at;
    const uint8_t* mask = bits + (uint64_t)stride * h;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return 0;
    auto field = [](uint32_t v, uint32_t m) -> uint32_t {   // a bitfield scaled to 8 bits
        if (!m) return 0;
        int sh = 0, len = 0;
        while (!((m >> sh) & 1)) sh++;
        while (sh + len < 32 && ((m >> (sh + len)) & 1)) len++;
        const uint32_t x = (v & m) >> sh;
        return len >= 8 ? x >> (len - 8) : (x * 255 + ((1u << len) - 1) / 2) / ((1u << len) - 1);
    };
    bool any_alpha = false;
    for (int32_t y = 0; y < h; y++) {
        const int32_t sy = top_down ? y : h - 1 - y;          // DIB rows: bottom-up unless the height is negative
        const uint8_t* row = bits + (uint64_t)stride * sy;
        uint32_t* out = (uint32_t*)((uint8_t*)s->pixels + (uint64_t)s->pitch * y);
        for (int32_t x = 0; x < w; x++) {
            uint32_t argb;
            if (bpp <= 8) {
                const uint32_t bit = (uint32_t)x * bpp;
                const uint32_t ix = (row[bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                const uint8_t* q = ix < colours ? pal + ix * 4 : pal;
                argb = colours ? 0xff000000u | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0] : 0xff000000u;
            } else if (bpp == 24) {
                const uint8_t* q = row + x * 3;
                argb = 0xff000000u | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
            } else {
                const uint32_t v = bpp == 16 ? rd16(row + x * 2) : rd32(row + x * 4);
                argb = 0xff000000u | field(v, masks[0]) << 16 | field(v, masks[1]) << 8 | field(v, masks[2]);
                if (bpp == 32 && comp == 0 && icon) {           // a 32-bit icon's own alpha
                    argb = (argb & 0xffffffu) | (v & 0xff000000u);
                    any_alpha = any_alpha || (v >> 24);
                }
            }
            out[x] = argb;
        }
    }
    if (icon && !any_alpha)                                     // the AND mask: 1 = transparent
        for (int32_t y = 0; y < h; y++) {
            const uint8_t* row = mask + (uint64_t)mstride * (top_down ? y : h - 1 - y);
            uint32_t* out = (uint32_t*)((uint8_t*)s->pixels + (uint64_t)s->pitch * y);
            for (int32_t x = 0; x < w; x++)
                if ((row[x / 8] >> (7 - x % 8)) & 1) out[x] &= 0x00ffffffu;
        }
    return s;
}

// ---- GDI: bitmaps and DCs ---------------------------------------------------------------------------------------------
enum class Gdi { Bitmap, WindowDC, MemoryDC };
struct GdiObject {
    Gdi kind;
    SDL_Surface* bitmap;                               // Bitmap: its pixels
    uint32_t hwnd;                                     // WindowDC: the window BeginPaint gave it for
    uint32_t selected;                                 // MemoryDC: the bitmap selected into it
};
std::map<uint32_t, GdiObject> g_gdi;
uint32_t g_next_gdi = 0x0c010010u;
const uint32_t STOCK_BITMAP = 0x0185000fu;             // the 1x1 bitmap a new memory DC holds (never drawn)

uint32_t gdi_add(const GdiObject& o) {
    const uint32_t h = g_next_gdi;
    g_next_gdi += 0x10;
    g_gdi[h] = o;
    return h;
}
GdiObject* gdi(uint32_t h, Gdi kind) {
    auto it = g_gdi.find(h);
    return it != g_gdi.end() && it->second.kind == kind ? &it->second : 0;
}

// ---- the windows --------------------------------------------------------------------------------------------------------
typedef int32_t(__stdcall* WndProc_t)(uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam);
struct Class { std::string name; uint32_t atom, proc, instance; };
struct Win {
    SDL_Window* sdl = 0;
    std::string cls, title;
    uint32_t atom = 0, proc = 0;                       // the class's window procedure (0: platform.cpp's window)
    bool adopted = false, shown = false, paint = false;
    int lock = -1;                                     // the lock file another process's FindWindowA sees
};
std::vector<Class> g_classes;
std::map<uint32_t, Win> g_wins;
uint32_t g_next_hwnd = 0x00020010u, g_next_atom = 0xc000u;
const uint32_t FOREIGN_HWND = 0x7ffe0010u;             // FindWindowA's answer for another process's window

const Class* find_class(const char* cls) {
    for (const Class& c : g_classes)
        if (is_int_resource(cls) ? c.atom == (uint32_t)(uintptr_t)cls : same(c.name, cls)) return &c;
    return 0;
}
Win* win(uint32_t hwnd) {
    auto it = g_wins.find(hwnd);
    return it != g_wins.end() ? &it->second : 0;
}
int32_t send(uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam) {
    Win* w = win(hwnd);
    const uint32_t proc = w ? w->proc : 0;
    return proc ? ((WndProc_t)(uintptr_t)proc)(hwnd, msg, wparam, lparam) : 0;
}

// the lock file of a window with this class and title ($XDG_RUNTIME_DIR, else /tmp; names lower-cased, as FindWindowA
// compares them, anything but letters and digits escaped)
std::string window_lock_path(const std::string& cls, const std::string& title) {
    const char* dir = getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(dir && *dir ? dir : "/tmp") + "/viperport-window-";
    static const char hex[] = "0123456789abcdef";
    auto add = [&](const std::string& s) {
        for (unsigned char ch : s) {
            const unsigned char c = (unsigned char)tolower(ch);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) path += (char)c;
            else { path += '_'; path += hex[c >> 4]; path += hex[c & 15]; }
        }
    };
    add(cls.substr(0, 100));
    path += '-';
    add(title.substr(0, 100));
    return path;
}
int window_lock(const std::string& cls, const std::string& title) {
    if (cls.empty() || title.empty()) return -1;
    const int fd = open(window_lock_path(cls, title).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; }
    return fd;
}
bool window_held_elsewhere(const char* cls, const char* title) {
    const int fd = open(window_lock_path(cls, title).c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!held) flock(fd, LOCK_UN);
    close(fd);
    return held;
}

uint32_t window_add(Win&& w) {
    const uint32_t h = g_next_hwnd;
    g_next_hwnd += 0x10;
    w.lock = window_lock(w.cls, w.title);
    g_wins[h] = std::move(w);
    return h;
}
void window_remove(uint32_t hwnd) {
    Win* w = win(hwnd);
    if (!w) return;
    if (w->lock >= 0) close(w->lock);
    if (w->sdl && !w->adopted) SDL_DestroyWindow(w->sdl);
    g_wins.erase(hwnd);
}

}  // namespace

int __stdcall w32_GetSystemMetrics(int index) {
    SDL_DisplayMode m;
    if ((index == 0 || index == 1) && video() && SDL_GetDesktopDisplayMode(0, &m) == 0) return index == 0 ? m.w : m.h;
    return 0;                                          // (SM_CXSCREEN / SM_CYSCREEN are all the game asks)
}

// WNDCLASSA (40 bytes): style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground,
// lpszMenuName, lpszClassName
uint32_t __stdcall w32_RegisterClassA(const void* wndclass) {
    const uint8_t* wc = (const uint8_t*)wndclass;
    const char* name = wc ? (const char*)(uintptr_t)rd32(wc + 36) : 0;
    if (!name || is_int_resource(name) || !*name) { w32::set_last_error(w32::ERR_INVALID_PARAMETER); return 0; }
    if (find_class(name)) { w32::set_last_error(1410); return 0; }   // ERROR_CLASS_ALREADY_EXISTS
    g_classes.push_back({name, g_next_atom++, rd32(wc + 4), rd32(wc + 16)});
    return g_classes.back().atom;
}

uint32_t __stdcall w32_CreateWindowExA(uint32_t ex_style, const char* cls, const char* title, uint32_t style, int x, int y,
                                       int w, int h, uint32_t parent, uint32_t menu, uint32_t instance, void* param) {
    const Class* c = cls ? find_class(cls) : 0;
    if (!c) { w32::set_last_error(1407); return 0; }   // ERROR_CANNOT_FIND_WND_CLASS
    if (!video()) { w32::set_last_error(w32::ERR_NOT_SUPPORTED); return 0; }
    const int k_default = (int)0x80000000;             // CW_USEDEFAULT
    Uint32 flags = SDL_WINDOW_HIDDEN;
    if ((style & 0x00c00000u) != 0x00c00000u) flags |= SDL_WINDOW_BORDERLESS;   // no WS_CAPTION (WS_POPUP)
    if (ex_style & 0x80u) flags |= SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_UTILITY;  // WS_EX_TOOLWINDOW
    if (ex_style & 0x08u) flags |= SDL_WINDOW_ALWAYS_ON_TOP;                       // WS_EX_TOPMOST
    SDL_Window* s = SDL_CreateWindow(title ? title : "", x == k_default ? (int)SDL_WINDOWPOS_UNDEFINED : x,
                                     y == k_default ? (int)SDL_WINDOWPOS_UNDEFINED : y,
                                     w == k_default ? 640 : w > 0 ? w : 1, h == k_default ? 480 : h > 0 ? h : 1, flags);
    if (!s) { w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY); return 0; }
    Win wn;
    wn.sdl = s;
    wn.cls = c->name;
    wn.title = title ? title : "";
    wn.atom = c->atom;
    wn.proc = c->proc;
    const uint32_t hwnd = window_add(std::move(wn));
    // WM_CREATE with its CREATESTRUCTA (48 bytes): lpCreateParams, hInstance, hMenu, hwndParent, cy, cx, y, x, style,
    // lpszName, lpszClass, dwExStyle; -1 refuses the window (destroyed again: WM_DESTROY, then 0)
    const uint32_t cs[12] = {(uint32_t)(uintptr_t)param, instance, menu, parent, (uint32_t)h, (uint32_t)w, (uint32_t)y,
                             (uint32_t)x, style, (uint32_t)(uintptr_t)title, (uint32_t)(uintptr_t)cls, ex_style};
    if (send(hwnd, 0x0001, 0, (int32_t)(uintptr_t)cs) == -1) {       // WM_CREATE
        send(hwnd, 0x0002, 0, 0);                                     // WM_DESTROY
        window_remove(hwnd);
        w32::set_last_error(1400);                                    // ERROR_INVALID_WINDOW_HANDLE (as Windows says)
        return 0;
    }
    if (style & 0x10000000u) w32_ShowWindow(hwnd, 5);                 // WS_VISIBLE: SW_SHOW
    return hwnd;
}

int __stdcall w32_DestroyWindow(uint32_t hwnd) {
    Win* w = win(hwnd);
    if (!w) { w32::set_last_error(1400); return 0; }
    if (w->adopted) {
        SDL_HideWindow(w->sdl);                        // (platform.cpp's: SDL and the renderer keep it to the end)
    } else {
        send(hwnd, 0x0002, 0, 0);                      // WM_DESTROY
    }
    window_remove(hwnd);
    return 1;
}
int __stdcall w32_IsWindow(uint32_t hwnd) { return hwnd == FOREIGN_HWND || win(hwnd) ? 1 : 0; }
// the previous visibility, as Windows returns it
int __stdcall w32_ShowWindow(uint32_t hwnd, int cmd) {
    if (hwnd == FOREIGN_HWND) return 0;                // (another process's window: one process can't show another's)
    Win* w = win(hwnd);
    if (!w) { w32::set_last_error(1400); return 0; }
    const bool was = w->shown;
    switch (cmd) {
    case 0: SDL_HideWindow(w->sdl); w->shown = false; break;                      // SW_HIDE
    case 2: case 6: case 7: case 11:                                               // the minimised ones
        SDL_ShowWindow(w->sdl); SDL_MinimizeWindow(w->sdl); w->shown = true; break;
    case 3: SDL_ShowWindow(w->sdl); SDL_MaximizeWindow(w->sdl); w->shown = true; break;   // SW_MAXIMIZE
    case 9: SDL_ShowWindow(w->sdl); SDL_RestoreWindow(w->sdl); w->shown = true; break;    // SW_RESTORE
    default: SDL_ShowWindow(w->sdl); w->shown = true; break;
    }
    if (!was && w->shown && w->proc) w->paint = true;  // newly visible: its WM_PAINT is pending
    return was ? 1 : 0;
}
int __stdcall w32_UpdateWindow(uint32_t hwnd) {
    Win* w = win(hwnd);
    if (!w) { w32::set_last_error(1400); return 0; }
    if (w->paint) {
        w->paint = false;
        send(hwnd, 0x000f, 0, 0);                      // WM_PAINT
    }
    return 1;
}
uint32_t __stdcall w32_FindWindowA(const char* cls, const char* title) {
    for (auto& kv : g_wins) {
        const Win& w = kv.second;
        if (cls && !(is_int_resource(cls) ? w.atom && w.atom == (uint32_t)(uintptr_t)cls : same(w.cls, cls))) continue;
        if (title && !same(w.title, title)) continue;
        return kv.first;
    }
    if (cls && title && !is_int_resource(cls) && window_held_elsewhere(cls, title)) return FOREIGN_HWND;
    return 0;
}
uint32_t __stdcall w32_GetForegroundWindow(void) {
    SDL_Window* f = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardFocus() : 0;
    if (f)
        for (auto& kv : g_wins)
            if (kv.second.sdl == f) return kv.first;
    return 0;
}
uint32_t __stdcall w32_SetFocus(uint32_t hwnd) {
    const uint32_t before = w32_GetForegroundWindow();
    Win* w = win(hwnd);
    if (hwnd && !w) { w32::set_last_error(1400); return 0; }
    if (w) SDL_RaiseWindow(w->sdl);
    return before;
}
int __stdcall w32_SetForegroundWindow(uint32_t hwnd) {
    Win* w = win(hwnd);
    if (!w) return 0;
    SDL_RaiseWindow(w->sdl);
    return 1;
}
// PAINTSTRUCT (64 bytes): hdc, fErase, rcPaint, fRestore, fIncUpdate, rgbReserved[32]. The DC draws on the window's SDL
// surface, erased first with black (the loading window's class brush, COLOR_WINDOWFRAME)
uint32_t __stdcall w32_BeginPaint(uint32_t hwnd, void* paint) {
    Win* w = win(hwnd);
    if (!w || !paint) { w32::set_last_error(1400); return 0; }
    w->paint = false;
    SDL_Surface* s = SDL_GetWindowSurface(w->sdl);
    if (s) SDL_FillRect(s, 0, SDL_MapRGB(s->format, 0, 0, 0));
    const uint32_t dc = gdi_add({Gdi::WindowDC, 0, hwnd, 0});
    uint32_t ps[16] = {dc, 0, 0, 0, s ? (uint32_t)s->w : 0, s ? (uint32_t)s->h : 0};
    memcpy(paint, ps, sizeof ps);
    return dc;
}
int __stdcall w32_EndPaint(uint32_t hwnd, const void* paint) {
    Win* w = win(hwnd);
    if (paint) {
        const uint32_t dc = rd32((const uint8_t*)paint);
        if (gdi(dc, Gdi::WindowDC)) g_gdi.erase(dc);
    }
    if (w) SDL_UpdateWindowSurface(w->sdl);
    return 1;
}
int32_t __stdcall w32_DefWindowProcA(uint32_t, uint32_t msg, uint32_t, int32_t) {
    return msg == 0x0081 ? 1 : 0;                      // WM_NCCREATE: go on; everything else: done
}
uint32_t __stdcall w32_LoadIconA(uint32_t, const char*) { return 0x10030; }   // (unused: platform.cpp's w32_resource_icon)
uint32_t __stdcall w32_LoadCursorA(uint32_t, const char*) { return 0x10040; }
uint32_t __stdcall w32_LoadBitmapA(uint32_t instance, const char* name) {
    uint32_t n = 0;
    const uint8_t* p = name ? find_resource(instance, 2, name, &n) : 0;     // RT_BITMAP
    if (!p) { w32::set_last_error(1814); return 0; }                         // ERROR_RESOURCE_NAME_NOT_FOUND
    SDL_Surface* s = dib_surface(p, n, false);
    if (!s) { w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY); return 0; }
    return gdi_add({Gdi::Bitmap, s, 0, 0});
}

// ---- for platform.cpp (w32_user.h) --------------------------------------------------------------------------------------
uint32_t w32_adopt_window(SDL_Window* window, const char* cls, const char* title) {
    for (auto& kv : g_wins)
        if (kv.second.sdl == window) return kv.first;
    Win w;
    w.sdl = window;
    w.cls = cls ? cls : "";
    w.title = title ? title : "";
    w.adopted = true;
    w.shown = (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) != 0;
    return window_add(std::move(w));
}

SDL_Surface* w32_resource_icon(uint32_t instance, uint32_t id) {
    uint32_t n = 0;
    const uint8_t* g = find_resource(instance, 14, (const char*)(uintptr_t)id, &n);   // RT_GROUP_ICON
    if (!g || n < 6 || rd16(g + 2) != 1) return 0;
    const uint32_t count = rd16(g + 4);
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < count && 6 + (i + 1) * 14 <= n; i++) {          // GRPICONDIRENTRY: 14 bytes
        const uint8_t* e = g + 6 + i * 14;
        const int size = e[0] ? e[0] : 256, bits = rd16(e + 6);
        const int score = (size >= 32 ? 1000 - size : size) * 64 + bits;    // 32x32 first, then the most colours
        if (score > best_score) best = (int)i, best_score = score;
    }
    if (best < 0) return 0;
    const uint32_t icon_id = rd16(g + 6 + best * 14 + 12);
    const uint8_t* p = find_resource(instance, 3, (const char*)(uintptr_t)icon_id, &n);   // RT_ICON
    return p ? dib_surface(p, n, true) : 0;
}
uint32_t __stdcall w32_SetCursor(uint32_t) { return 0; }
int __stdcall w32_ShowCursor(int show) {
    static int count = 0;                              // Windows' display count
    count += show ? 1 : -1;
    return count;
}
int __stdcall w32_ClipCursor(const void*) { return 1; }  // (the SDL platform layer clips the mouse to its window)
int __stdcall w32_SetCursorPos(int, int) { return 1; }
int __stdcall w32_MessageBoxA(uint32_t, const char* text, const char* caption, uint32_t type) {
    const uint32_t icon = type & 0xf0;
    SDL_ShowSimpleMessageBox(icon == 0x10 ? SDL_MESSAGEBOX_ERROR : icon == 0x30 ? SDL_MESSAGEBOX_WARNING
                                                                                 : SDL_MESSAGEBOX_INFORMATION,
                             caption ? caption : "Error", text ? text : "", 0);
    return 1;                                          // IDOK (the game's boxes are MB_OK)
}
int __stdcall w32_GetMessageA(void*, uint32_t, uint32_t, uint32_t) { return 0; }
int __stdcall w32_PeekMessageA(void*, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int __stdcall w32_TranslateMessage(const void*) { return 0; }
int32_t __stdcall w32_DispatchMessageA(const void*) { return 0; }
void __stdcall w32_PostQuitMessage(int) {}
uint32_t __stdcall w32_SetWindowsHookExA(int, void*, uint32_t, uint32_t) { return 0x10060; }
int __stdcall w32_UnhookWindowsHookEx(uint32_t) { return 1; }
int32_t __stdcall w32_CallNextHookEx(uint32_t, int, uint32_t, int32_t) { return 0; }
uint32_t __stdcall w32_MapVirtualKeyA(uint32_t code, uint32_t type) {
    if (type == 1) return w32_us_scan_to_vk(code);    // MAPVK_VSC_TO_VK: all the game asks
    if (type == 0)                                     // MAPVK_VK_TO_VSC: the first scan code with that key
        for (uint32_t s = 1; s < 0x80; s++)
            if (w32_us_scan_to_vk(s) == code) return s;
    return 0;
}
// the clipboard: SDL's (UTF-8; the game's text is ANSI -- Latin-1 here, R2b: the game's code page)
static bool g_clip_open;
int __stdcall w32_OpenClipboard(uint32_t) {
    if (g_clip_open) { w32::set_last_error(5); return 0; }      // ERROR_ACCESS_DENIED: already open
    g_clip_open = true;
    return 1;
}
int __stdcall w32_CloseClipboard(void) {
    if (!g_clip_open) { w32::set_last_error(1418); return 0; }  // ERROR_CLIPBOARD_NOT_OPEN
    g_clip_open = false;
    return 1;
}
int __stdcall w32_EmptyClipboard(void) {
    if (!g_clip_open) { w32::set_last_error(1418); return 0; }
    SDL_SetClipboardText("");
    return 1;
}
uint32_t __stdcall w32_SetClipboardData(uint32_t format, uint32_t mem) {
    if (!g_clip_open) { w32::set_last_error(1418); return 0; }
    if (format != 1 || !mem) return 0;
    const std::string s = global_text(mem);
    std::string u;
    for (unsigned char c : s) {
        if (c < 0x80) u += (char)c;
        else { u += (char)(0xc0 | (c >> 6)); u += (char)(0x80 | (c & 0x3f)); }
    }
    SDL_SetClipboardText(u.c_str());
    return mem;
}
uint32_t __stdcall w32_GetClipboardData(uint32_t format) {
    if (!g_clip_open || format != 1) return 0;
    char* t = SDL_GetClipboardText();
    if (!t || !*t) { SDL_free(t); return 0; }
    std::string s;
    for (const unsigned char* p = (const unsigned char*)t; *p; p++) {
        if (*p < 0x80) s += (char)*p;
        else if ((*p & 0xe0) == 0xc0 && p[1]) { s += (char)(((p[0] & 0x1f) << 6) | (p[1] & 0x3f)); p++; }
        else { s += '?'; while ((p[1] & 0xc0) == 0x80) p++; }
    }
    SDL_free(t);
    return text_global(s);
}

uint32_t __stdcall w32_GetActiveWindow(void) { return w32_GetForegroundWindow(); }
uint32_t __stdcall w32_GetLastActivePopup(uint32_t hwnd) { return hwnd; }

// GDI32: a DC is a window's surface (BeginPaint) or a memory DC with a bitmap selected; BitBlt copies (SRCCOPY) or
// fills (BLACKNESS / WHITENESS) -- the loading window's one blit
static SDL_Surface* dc_surface(uint32_t dc) {
    auto it = g_gdi.find(dc);
    if (it == g_gdi.end()) return 0;
    if (it->second.kind == Gdi::WindowDC) {
        Win* w = win(it->second.hwnd);
        return w ? SDL_GetWindowSurface(w->sdl) : 0;
    }
    if (it->second.kind == Gdi::MemoryDC) {
        GdiObject* b = gdi(it->second.selected, Gdi::Bitmap);
        return b ? b->bitmap : 0;
    }
    return 0;
}
int __stdcall w32_BitBlt(uint32_t dc, int x, int y, int w, int h, uint32_t src, int sx, int sy, uint32_t rop) {
    SDL_Surface* d = dc_surface(dc);
    if (!d) { w32::set_last_error(w32::ERR_INVALID_HANDLE); return 0; }
    SDL_Rect dr = {x, y, w, h};
    if (rop == 0x00000042u || rop == 0x00ff0062u)                  // BLACKNESS, WHITENESS
        return SDL_FillRect(d, &dr, rop == 0x42u ? SDL_MapRGB(d->format, 0, 0, 0) : SDL_MapRGB(d->format, 255, 255, 255)) == 0;
    SDL_Surface* s = dc_surface(src);
    if (rop != 0x00cc0020u || !s) {                                // SRCCOPY only
        w32::set_last_error(rop != 0x00cc0020u ? w32::ERR_NOT_SUPPORTED : w32::ERR_INVALID_HANDLE);
        return 0;
    }
    SDL_Rect sr = {sx, sy, w, h};
    SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
    return SDL_BlitSurface(s, &sr, d, &dr) == 0;
}
uint32_t __stdcall w32_CreateCompatibleDC(uint32_t) { return gdi_add({Gdi::MemoryDC, 0, 0, STOCK_BITMAP}); }
int __stdcall w32_DeleteDC(uint32_t dc) {
    auto it = g_gdi.find(dc);
    if (it == g_gdi.end() || it->second.kind == Gdi::Bitmap) { w32::set_last_error(w32::ERR_INVALID_HANDLE); return 0; }
    g_gdi.erase(it);
    return 1;
}
// a bitmap into a memory DC: the one it held before (a new DC's: the stock 1x1 bitmap)
uint32_t __stdcall w32_SelectObject(uint32_t dc, uint32_t obj) {
    GdiObject* d = gdi(dc, Gdi::MemoryDC);
    if (!d || !(obj == STOCK_BITMAP || gdi(obj, Gdi::Bitmap))) { w32::set_last_error(w32::ERR_INVALID_HANDLE); return 0; }
    const uint32_t was = d->selected;
    d->selected = obj;
    return was;
}
int __stdcall w32_DeleteObject(uint32_t obj) {
    GdiObject* b = gdi(obj, Gdi::Bitmap);
    if (!b) return obj == STOCK_BITMAP ? 1 : 0;        // (a stock object: "deleted", as Windows answers)
    SDL_FreeSurface(b->bitmap);
    g_gdi.erase(obj);
    return 1;
}

#endif

// MapVirtualKeyA(scan, MAPVK_VSC_TO_VK) on the US layout (kbdus: 0x409), scan codes 0-255 (read off Windows; the test
// compares every one)
uint32_t w32_us_scan_to_vk(uint32_t scan) {
    static const uint8_t k_vk[0x80] = {
        0x00, 0x1b, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0xbd, 0xbb, 0x08, 0x09,
        0x51, 0x57, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49, 0x4f, 0x50, 0xdb, 0xdd, 0x0d, 0x11, 0x41, 0x53,
        0x44, 0x46, 0x47, 0x48, 0x4a, 0x4b, 0x4c, 0xba, 0xde, 0xc0, 0x10, 0xdc, 0x5a, 0x58, 0x43, 0x56,
        0x42, 0x4e, 0x4d, 0xbc, 0xbe, 0xbf, 0x10, 0x6a, 0x12, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
        0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x24, 0x26, 0x21, 0x6d, 0x25, 0x0c, 0x27, 0x6b, 0x23,
        0x28, 0x22, 0x2d, 0x2e, 0x2c, 0x00, 0xe2, 0x7a, 0x7b, 0x0c, 0xee, 0xf1, 0xea, 0xf9, 0xf5, 0xf3,
        0x00, 0x00, 0xfb, 0x2f, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0xed,
        0x00, 0xe9, 0x00, 0xc1, 0x00, 0x00, 0x87, 0x00, 0x00, 0x00, 0x00, 0xeb, 0x09, 0x00, 0xc2, 0x00,
    };
    return scan < 0x80 ? k_vk[scan] : 0;
}

// =========================================================================================================================
// ADVAPI32: the registry on Config\registry.ini (every OS)
// =========================================================================================================================
void w32_registry_file(const char* path) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    g_reg_path = path ? path : "";
    g_reg_loaded = false;
    g_lines.clear();
}

int32_t __stdcall w32_RegOpenKeyExA(uint32_t key, const char* sub, uint32_t, uint32_t sam, uint32_t* out) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    reg_load();
    std::string path;
    uint32_t parent_sam;
    if (!out) return w32::ERR_INVALID_PARAMETER;
    if (!key_of(key, &path, &parent_sam)) { *out = 0; return w32::ERR_INVALID_HANDLE; }
    if (sub && *sub) {
        if (sub[0] == '\\') { *out = 0; return w32::ERR_FILE_NOT_FOUND; }
        std::string s = sub;
        while (!s.empty() && s[s.size() - 1] == '\\') s.erase(s.size() - 1);
        path += "\\" + s;
    }
    if (!key_exists(path)) {
        *out = 0;
        return w32::ERR_FILE_NOT_FOUND;
    }
    const w32::Handle h = w32::add(std::make_shared<RegKey>(path, sam));
    if (!h) { *out = 0; return w32::ERR_NOT_ENOUGH_MEMORY; }
    *out = h;
    return 0;
}

int32_t __stdcall w32_RegQueryValueExA(uint32_t key, const char* name, uint32_t* reserved, uint32_t* type, void* data,
                                       uint32_t* size) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    reg_load();
    std::string path;
    uint32_t sam;
    if (!key_of(key, &path, &sam)) return w32::ERR_INVALID_HANDLE;
    if (reserved || (data && !size)) return w32::ERR_INVALID_PARAMETER;
    if (!(sam & KEY_QUERY_VALUE_)) return w32::ERR_ACCESS_DENIED;
    const int line = find_value(path, name && *name ? name : "@", 0);
    uint32_t t = 0;
    std::vector<uint8_t> v;
    if (line < 0 || !parse_value(g_lines[line], &t, &v)) {
        if (type) *type = 0;                           // (REG_NONE, as Windows leaves it)
        return w32::ERR_FILE_NOT_FOUND;
    }
    if (type) *type = t;
    if (!size) return 0;
    const uint32_t n = (uint32_t)v.size();
    if (data && *size < n) {
        *size = n;
        return ERROR_MORE_DATA_;
    }
    if (data && n) memcpy(data, v.data(), n);
    *size = n;
    return 0;
}

int32_t __stdcall w32_RegSetValueExA(uint32_t key, const char* name, uint32_t, uint32_t type, const void* data,
                                     uint32_t size) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    reg_load();
    std::string path;
    uint32_t sam;
    if (!key_of(key, &path, &sam)) return w32::ERR_INVALID_HANDLE;
    if (!(sam & KEY_SET_VALUE_)) return w32::ERR_ACCESS_DENIED;
    if (path.find('\\') == std::string::npos) return w32::ERR_ACCESS_DENIED;   // (a value on a root itself: not ours)
    if (size && !data) return 998;                     // ERROR_NOACCESS
    const std::string nm = name && *name ? name : "@";
    const std::string line = format_value(nm, type, (const uint8_t*)data, size);
    int end;
    const int at = find_value(path, nm, &end);
    if (at >= 0) g_lines[at] = line;
    else if (end >= 0) g_lines.insert(g_lines.begin() + end, line);
    else {
        if (!g_lines.empty() && !trim(g_lines.back()).empty()) g_lines.push_back("");
        g_lines.push_back("[" + path + "]");
        g_lines.push_back(line);
    }
    return reg_save() ? 0 : w32::ERR_ACCESS_DENIED;
}

int32_t __stdcall w32_RegCloseKey(uint32_t key) {
    if (key >= HKCR && key <= HKDD) return 0;
    std::shared_ptr<w32::Object> o = w32::get(key, w32::Kind::Other);
    if (!o || !dynamic_cast<RegKey*>(o.get())) return w32::ERR_INVALID_HANDLE;
    w32::close(key);
    return 0;
}

// the file is written at every RegSetValueExA: flushing has nothing left to do. HKEY_DYN_DATA (Windows 9x's) is
// ERROR_CALL_NOT_IMPLEMENTED, as Windows says; HKEY_PERFORMANCE_DATA flushes fine.
int32_t __stdcall w32_RegFlushKey(uint32_t key) {
    if (key == HKDD) return w32::ERR_CALL_NOT_IMPLEMENTED;
    std::lock_guard<std::mutex> lk(g_reg_mu);
    std::string path;
    uint32_t sam;
    return key_of(key, &path, &sam) ? 0 : w32::ERR_INVALID_HANDLE;
}
