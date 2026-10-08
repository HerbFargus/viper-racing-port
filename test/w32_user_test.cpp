// w32_user_test.cpp -- the stand-ins (hook/w32_user.*, hook/w32_net.*) against the real Windows API, on the same
// inputs, in one process: USER32 / GDI32 (forwarded: results and the last error carried both ways), MapVirtualKeyA's
// US table, the registry on registry.ini (against a scratch key under HKEY_CURRENT_USER, removed at the end), TAPI32
// "not available" (against TAPI32 on a PC with no modem), the serial-port functions, and WSOCK32 (winsock: loopback
// UDP, the error paths, inet_addr, the hostent and WSADATA layouts the Linux branch builds).
//
// Build (from the repo root) and run; the exit code is the number of FAILs:
//   MSVC: cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /I..\sdl2\SDL2-2.32.10\include test\w32_user_test.cpp
//           hook\w32_user.cpp hook\w32_net.cpp hook\w32_handle.cpp hook\w32_path.cpp /Fe%TEMP%\w32_user_test.exe /Fo%TEMP%\ (end)
//           /link user32.lib gdi32.lib advapi32.lib tapi32.lib wsock32.lib
//   GCC:  g++ -m32 -O2 -std=c++17 -fms-extensions -I../sdl2/SDL2-2.32.10/include test/w32_user_test.cpp
//           hook/w32_user.cpp hook/w32_net.cpp hook/w32_handle.cpp hook/w32_path.cpp -o %TEMP%/w32_user_test.exe -static
//           -luser32 -lgdi32 -ladvapi32 -ltapi32 -lwsock32
// It never touches the clipboard's contents, the game or the install.
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef WIN32
#define WIN32 1                                          // (tapi.h tests #if WIN32)
#endif
#include <winsock.h>
#include <windows.h>
#define TAPI_CURRENT_VERSION 0x00010004                  // (the game's TAPI 1.4; the SDK's tapi.h wants it said)
#include <tapi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "../hook/w32_handle.h"
#include "../hook/w32_user.h"
#include "../hook/w32_net.h"

static int g_fail, g_pass;
static void check(bool ok, const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    if (ok) g_pass++;
    else g_fail++;
    printf("%s  %s\n", ok ? "pass" : "FAIL", line);
}
static void info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("info  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
// a result and the last error after it, from Windows (GetLastError) or a stand-in (the stand-ins' last error)
struct R { long long v; unsigned long e; };
#define REAL(expr) ([&]() { SetLastError(0xdead); const long long v_ = (long long)(expr); return R{v_, GetLastError()}; })()
#define MINE(expr) ([&]() { w32::set_last_error(0xdead); const long long v_ = (long long)(expr); return R{v_, w32::last_error()}; })()
static void same(const char* what, R real, R mine, bool with_error = true) {
    const bool ok = real.v == mine.v && (!with_error || real.e == mine.e);
    check(ok, "%-58s Windows %lld (error %lu), stand-in %lld (error %lu)", what, real.v, real.e, mine.v, mine.e);
}
#define U32V(h) ((uint32_t)(uintptr_t)(h))

// =====================================================================================================================
// USER32 / GDI32
// =====================================================================================================================
static uint32_t g_bitmap;                                // the "SPLASH" the loading window paints
static LRESULT CALLBACK loading_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {                               // the game's loading window procedure, through the stand-ins
        PAINTSTRUCT ps;
        const uint32_t dc = w32_BeginPaint(U32V(hwnd), &ps);
        const uint32_t mem = w32_CreateCompatibleDC(dc);
        const uint32_t old = w32_SelectObject(mem, g_bitmap);
        const int blt = w32_BitBlt(dc, 0, 0, 300, 373, mem, 0, 0, SRCCOPY);
        w32_SelectObject(mem, old);
        const int del = w32_DeleteDC(mem);
        const int end = w32_EndPaint(U32V(hwnd), &ps);
        check(dc && mem && old && blt && del && end, "the loading window paints through the stand-ins (BeginPaint .. EndPaint)");
        return 0;
    }
    return w32_DefWindowProcA(U32V(hwnd), msg, (uint32_t)wp, (int32_t)lp);
}

static void test_user() {
    printf("\n== USER32 / GDI32 (forwarded) ==\n");
    same("GetSystemMetrics(SM_CXSCREEN)", REAL(GetSystemMetrics(SM_CXSCREEN)), MINE(w32_GetSystemMetrics(SM_CXSCREEN)));
    same("GetSystemMetrics(SM_CYSCREEN)", REAL(GetSystemMetrics(SM_CYSCREEN)), MINE(w32_GetSystemMetrics(SM_CYSCREEN)));
    same("LoadCursorA(0, IDC_ARROW)", REAL(U32V(LoadCursorA(0, IDC_ARROW))), MINE(w32_LoadCursorA(0, (const char*)IDC_ARROW)));
    same("LoadIconA(0, IDI_APPLICATION)", REAL(U32V(LoadIconA(0, IDI_APPLICATION))), MINE(w32_LoadIconA(0, (const char*)IDI_APPLICATION)));
    same("LoadBitmapA(exe, \"SPLASH\") (none here)", REAL(U32V(LoadBitmapA(GetModuleHandleA(0), "SPLASH"))),
         MINE(w32_LoadBitmapA(U32V(GetModuleHandleA(0)), "SPLASH")));
    same("IsWindow(bogus)", REAL(IsWindow((HWND)0x1234)), MINE(w32_IsWindow(0x1234)));
    same("DestroyWindow(bogus)", REAL(DestroyWindow((HWND)0x1234)), MINE(w32_DestroyWindow(0x1234)));
    same("ShowWindow(bogus, SW_SHOW)", REAL(ShowWindow((HWND)0x1234, SW_SHOW)), MINE(w32_ShowWindow(0x1234, SW_SHOW)));
    same("FindWindowA(no such class)", REAL(U32V(FindWindowA("viperport no such class", 0))),
         MINE(w32_FindWindowA("viperport no such class", 0)));
    same("CloseClipboard() (not open)", REAL(CloseClipboard()), MINE(w32_CloseClipboard()));
    same("EmptyClipboard() (not open)", REAL(EmptyClipboard()), MINE(w32_EmptyClipboard()));
    same("UnhookWindowsHookEx(bogus)", REAL(UnhookWindowsHookEx((HHOOK)0x1234)), MINE(w32_UnhookWindowsHookEx(0x1234)));
    same("DeleteObject(0)", REAL(DeleteObject(0)), MINE(w32_DeleteObject(0)));
    same("DeleteDC(bogus)", REAL(DeleteDC((HDC)0x1234)), MINE(w32_DeleteDC(0x1234)));
    // the last error a successful call leaves alone carries through the stand-in, both ways
    SetLastError(1234);
    const BOOL a = IsWindow(GetDesktopWindow());
    const DWORD ea = GetLastError();
    w32::set_last_error(1234);
    const int b = w32_IsWindow(U32V(GetDesktopWindow()));
    same("IsWindow(desktop) after last error 1234", R{a, ea}, R{b, w32::last_error()});

    // the game's loading window, through the stand-ins: class, window, show, paint, destroy
    HDC screen = GetDC(0);
    HBITMAP bm = CreateCompatibleBitmap(screen, 300, 373);
    ReleaseDC(0, screen);
    g_bitmap = U32V(bm);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = loading_proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = (HCURSOR)(uintptr_t)w32_LoadCursorA(0, (const char*)IDC_ARROW);
    wc.lpszClassName = "viperport w32 test";
    const uint32_t atom = w32_RegisterClassA(&wc);
    check(atom != 0 && atom < 0x10000, "RegisterClassA: an atom (0x%x), zero-extended", atom);
    const int cx = w32_GetSystemMetrics(SM_CXSCREEN), cy = w32_GetSystemMetrics(SM_CYSCREEN);
    const uint32_t w = w32_CreateWindowExA(WS_EX_TOOLWINDOW, "viperport w32 test", "MGI", WS_POPUP | WS_BORDER, (cx - 300) / 2,
                                           (cy - 373) / 2, 300, 373, 0, 0, U32V(GetModuleHandleA(0)), 0);
    check(w != 0, "CreateWindowExA: the loading window");
    same("FindWindowA(its class, \"MGI\")", REAL(U32V(FindWindowA("viperport w32 test", "MGI"))),
         MINE(w32_FindWindowA("viperport w32 test", "MGI")));
    w32_ShowWindow(w, SW_SHOW);
    check(w32_UpdateWindow(w) != 0, "UpdateWindow (paints it now)");
    MSG m;
    while (w32_PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) {
        w32_TranslateMessage(&m);
        w32_DispatchMessageA(&m);
    }
    check(w32_IsWindow(w) != 0, "IsWindow(the loading window)");
    check(w32_DestroyWindow(w) != 0 && !w32_IsWindow(w), "DestroyWindow, then IsWindow says no");
    UnregisterClassA("viperport w32 test", GetModuleHandleA(0));
    DeleteObject(bm);

    printf("\n== MapVirtualKeyA ==\n");
    int same_fwd = 0;
    for (uint32_t sc = 0; sc < 256; sc++) same_fwd += w32_MapVirtualKeyA(sc, 1) == MapVirtualKeyA(sc, 1);
    check(same_fwd == 256, "MapVirtualKeyA(scan, 1) = Windows' for all 256 scan codes (%d)", same_fwd);
    HKL us = 0, list[32];
    const int nl = GetKeyboardLayoutList(32, list);
    for (int i = 0; i < nl; i++)
        if (((uintptr_t)list[i] & 0xffff) == 0x0409 && ((uintptr_t)list[i] >> 16) == 0x0409) us = list[i];
    if (!us) {
        info("the US layout (00000409) isn't loaded here: the Linux table isn't compared");
    } else {
        int same_us = 0;
        for (uint32_t sc = 0; sc < 256; sc++) {
            const uint32_t real = MapVirtualKeyExA(sc, 1, us), mine = w32_us_scan_to_vk(sc);
            if (real == mine) same_us++;
            else printf("      scan %02x: Windows %02x, table %02x\n", sc, real, mine);
        }
        check(same_us == 256, "the US table (Linux's MapVirtualKeyA) = Windows' US layout for all 256 scan codes (%d)", same_us);
    }
}

// =====================================================================================================================
// ADVAPI32: registry.ini against the real registry
// =====================================================================================================================
static const char* const TEST_KEY = "Software\\viperport-w32-test";

static void test_registry() {
    printf("\n== ADVAPI32 (registry.ini) ==\n");
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string ini = std::string(tmp) + "viperport-w32-test-registry.ini";
    // the same key and values in both
    const uint32_t dw = 0x12345678, bin = 1;
    {
        HKEY k;
        RegCreateKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0);
        RegSetValueExA(k, "Dword", 0, REG_DWORD, (const BYTE*)&dw, 4);
        RegSetValueExA(k, "EnableAutoDial", 0, REG_BINARY, (const BYTE*)&bin, 4);
        RegSetValueExA(k, "AttachedTo", 0, REG_SZ, (const BYTE*)"COM2", 5);
        RegCloseKey(k);
        RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\viperport-w32-test\\Sub\\Deeper", 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0);
        RegCloseKey(k);
        FILE* f = fopen(ini.c_str(), "w");
        fprintf(f, "; the stand-ins' registry\n[HKEY_CURRENT_USER\\Software\\viperport-w32-test]\nDword=REG_DWORD:0x12345678\n"
                   "EnableAutoDial=REG_BINARY:01 00 00 00\nAttachedTo=REG_SZ:COM2\n\n"
                   "[HKEY_CURRENT_USER\\Software\\viperport-w32-test\\Sub\\Deeper]\n");
        fclose(f);
        w32_registry_file(ini.c_str());
    }
    HKEY rk = (HKEY)0x77;
    uint32_t mk = 0x77;
    same("RegOpenKeyExA(HKCU, test key, KEY_READ|KEY_WRITE)", R{RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0, 0x2001f, &rk), 0},
         R{w32_RegOpenKeyExA(0x80000001u, TEST_KEY, 0, 0x2001f, &mk), 0});
    HKEY rx = (HKEY)0x77;
    uint32_t mx = 0x77;
    same("RegOpenKeyExA(missing subkey) and *out", R{RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\viperport-w32-test\\Nope", 0, KEY_READ, &rx), U32V(rx)},
         R{w32_RegOpenKeyExA(0x80000001u, "Software\\viperport-w32-test\\Nope", 0, KEY_READ, &mx), mx});
    rx = (HKEY)0x77, mx = 0x77;
    same("RegOpenKeyExA(bogus parent) and *out", R{RegOpenKeyExA((HKEY)0x1234, "Software", 0, KEY_READ, &rx), U32V(rx)},
         R{w32_RegOpenKeyExA(0x1234, "Software", 0, KEY_READ, &mx), mx});
    HKEY rs;
    uint32_t ms;
    same("RegOpenKeyExA(a key with only a key below it: Sub)", R{RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\viperport-w32-test\\Sub", 0, KEY_READ, &rs), 0},
         R{w32_RegOpenKeyExA(0x80000001u, "Software\\viperport-w32-test\\Sub", 0, KEY_READ, &ms), 0});
    RegCloseKey(rs);
    w32_RegCloseKey(ms);

    struct Q { const char* name; bool data; uint32_t cb; bool type; };
    const Q qs[] = {{"Dword", true, 4, true},           {"Dword", true, 64, true},       {"Dword", false, 4, true},
                    {"Dword", true, 3, true},           {"EnableAutoDial", true, 4, true}, {"AttachedTo", true, 0x40, true},
                    {"AttachedTo", true, 4, true},      {"AttachedTo", false, 0, true},  {"attachedto", true, 0x40, false},
                    {"Missing", true, 4, true}};
    for (const Q& q : qs) {
        uint8_t rd[64], md[64];
        memset(rd, 0xcc, sizeof rd);
        memset(md, 0xcc, sizeof md);
        DWORD rt = 0x77, rcb = q.cb;
        uint32_t mt = 0x77, mcb = q.cb;
        const LONG r = RegQueryValueExA(rk, q.name, 0, q.type ? &rt : 0, q.data ? rd : 0, &rcb);
        const int32_t m = w32_RegQueryValueExA(mk, q.name, 0, q.type ? &mt : 0, q.data ? md : 0, &mcb);
        const bool data_same = r != 0 || !q.data || memcmp(rd, md, rcb) == 0;
        char what[128];
        sprintf(what, "RegQueryValueExA(%s, %s, cb %u): result, type, cb, data", q.name, q.data ? "data" : "no data", q.cb);
        check(r == m && rt == mt && rcb == mcb && data_same, "%-58s Windows %ld type %lu cb %lu, stand-in %d type %u cb %u%s", what,
              r, rt, rcb, m, mt, mcb, data_same ? "" : " (data differs)");
    }
    {
        DWORD t, d, cb = 4;
        uint32_t mt, md2, mcb = 4;
        same("RegQueryValueExA(reserved non-null)", R{RegQueryValueExA(rk, "Dword", &t, &t, (BYTE*)&d, &cb), 0},
             R{w32_RegQueryValueExA(mk, "Dword", &mt, &mt, &md2, &mcb), 0});
        same("RegQueryValueExA(data but no size)", R{RegQueryValueExA(rk, "Dword", 0, &t, (BYTE*)&d, 0), 0},
             R{w32_RegQueryValueExA(mk, "Dword", 0, &mt, &md2, 0), 0});
        same("RegQueryValueExA(bogus key)", R{RegQueryValueExA((HKEY)0x1234, "Dword", 0, &t, (BYTE*)&d, &cb), 0},
             R{w32_RegQueryValueExA(0x1234, "Dword", 0, &mt, &md2, &mcb), 0});
    }
    // set_autodial's write: EnableAutoDial = 0, read back
    const uint32_t zero = 0;
    same("RegSetValueExA(EnableAutoDial, REG_BINARY, 0)", R{RegSetValueExA(rk, "EnableAutoDial", 0, REG_BINARY, (const BYTE*)&zero, 4), 0},
         R{w32_RegSetValueExA(mk, "EnableAutoDial", 0, REG_BINARY, &zero, 4), 0});
    same("RegSetValueExA(a new value, REG_SZ)", R{RegSetValueExA(rk, "New Value", 0, REG_SZ, (const BYTE*)"text here", 10), 0},
         R{w32_RegSetValueExA(mk, "New Value", 0, REG_SZ, "text here", 10), 0});
    for (const char* name : {"EnableAutoDial", "New Value"}) {
        uint8_t rd[64] = {}, md[64] = {};
        DWORD rt = 0, rcb = sizeof rd;
        uint32_t mt = 0, mcb = sizeof md;
        const LONG r = RegQueryValueExA(rk, name, 0, &rt, rd, &rcb);
        const int32_t m = w32_RegQueryValueExA(mk, name, 0, &mt, md, &mcb);
        check(r == m && rt == mt && rcb == mcb && !memcmp(rd, md, rcb), "read back %-44s Windows %ld type %lu cb %lu, stand-in %d type %u cb %u",
              name, r, rt, rcb, m, mt, mcb);
    }
    // a key opened for reading can't be written, and one opened for writing only can't be read
    {
        HKEY rr;
        uint32_t mr;
        RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0, KEY_READ, &rr);
        w32_RegOpenKeyExA(0x80000001u, TEST_KEY, 0, KEY_READ, &mr);
        same("RegSetValueExA on a KEY_READ key", R{RegSetValueExA(rr, "X", 0, REG_DWORD, (const BYTE*)&dw, 4), 0},
             R{w32_RegSetValueExA(mr, "X", 0, REG_DWORD, &dw, 4), 0});
        RegCloseKey(rr);
        w32_RegCloseKey(mr);
        RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0, KEY_SET_VALUE, &rr);
        w32_RegOpenKeyExA(0x80000001u, TEST_KEY, 0, KEY_SET_VALUE, &mr);
        DWORD t, d, cb = 4;
        uint32_t mt, md, mcb = 4;
        same("RegQueryValueExA on a KEY_SET_VALUE key", R{RegQueryValueExA(rr, "Dword", 0, &t, (BYTE*)&d, &cb), 0},
             R{w32_RegQueryValueExA(mr, "Dword", 0, &mt, &md, &mcb), 0});
        RegCloseKey(rr);
        w32_RegCloseKey(mr);
    }
    same("RegCloseKey(the key)", R{RegCloseKey(rk), 0}, R{w32_RegCloseKey(mk), 0});
    same("RegCloseKey(it again)", R{RegCloseKey(rk), 0}, R{w32_RegCloseKey(mk), 0});
    same("RegCloseKey(HKEY_CURRENT_USER)", R{RegCloseKey(HKEY_CURRENT_USER), 0}, R{w32_RegCloseKey(0x80000001u), 0});
    // app_end's flushes, and two that aren't keys
    const uint32_t roots[] = {0x80000000u, 0x80000005u, 0x80000001u, 0x80000002u, 0x80000003u, 0x80000006u, 0x80000004u, 0x1234, 0};
    for (uint32_t r : roots) {
        char what[64];
        sprintf(what, "RegFlushKey(%08x)", r);
        same(what, R{RegFlushKey((HKEY)(uintptr_t)r), 0}, R{w32_RegFlushKey(r), 0});
    }
    // the file the stand-ins wrote
    {
        FILE* f = fopen(ini.c_str(), "r");
        char buf[1024] = "";
        const size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        buf[n] = 0;
        check(strstr(buf, "EnableAutoDial=REG_BINARY:00 00 00 00") && strstr(buf, "New Value=REG_SZ:text here") &&
                  strstr(buf, "; the stand-ins' registry"),
              "registry.ini after the writes keeps its comment and holds both values");
    }
    // the game's own key: Windows refuses HKEY_USERS\.Default to a user (or opens it for an administrator); the stand-in,
    // with no such section, says it isn't there -- set_autodial gives up either way (a nonzero result) and changes nothing
    {
        HKEY k;
        uint32_t m;
        const LONG r = RegOpenKeyExA(HKEY_USERS, ".Default\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0,
                                     0x2001f, &k);
        if (!r) RegCloseKey(k);
        const int32_t mr = w32_RegOpenKeyExA(0x80000003u, ".Default\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                                             0, 0x2001f, &m);
        info("the game's autodial key: Windows %ld, stand-in %d (the game only tests for 0: %s)", r, mr,
             (r != 0) == (mr != 0) ? "the same path" : "a different path -- this user can write HKEY_USERS\\.Default");
    }
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\viperport-w32-test\\Sub\\Deeper");
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\viperport-w32-test\\Sub");
    RegDeleteKeyA(HKEY_CURRENT_USER, TEST_KEY);
    DeleteFileA(ini.c_str());
}

// =====================================================================================================================
// TAPI32: not available, against TAPI32 with no modem
// =====================================================================================================================
static void CALLBACK tapi_cb(DWORD, DWORD, DWORD_PTR, DWORD_PTR, DWORD_PTR, DWORD_PTR) {}

// every call the game makes, with its arguments' shapes, on these handles
static void tapi_round(const char* when, HLINEAPP rapp, uint32_t mapp) {
    char what[128];
    uint8_t buf[0x400];
    memset(buf, 0, sizeof buf);
    *(DWORD*)buf = sizeof buf;
    uint8_t mbuf[0x400];
    memcpy(mbuf, buf, sizeof buf);
    DWORD rv = 0x77;
    uint32_t mv = 0x77;
    LINEEXTENSIONID ext;
    uint8_t mext[16];
#define T(name, real, mine)                    \
    sprintf(what, "%s: %s", when, name);       \
    same(what, R{(long long)(real), 0}, R{(long long)(mine), 0})
    T("lineNegotiateAPIVersion(dev 0)", lineNegotiateAPIVersion(rapp, 0, 0x10004, 0x10004, &rv, &ext),
      w32_lineNegotiateAPIVersion(mapp, 0, 0x10004, 0x10004, &mv, mext));
    T("lineNegotiateAPIVersion(bad app)", lineNegotiateAPIVersion(0x1234, 0, 0x10004, 0x10004, &rv, &ext),
      w32_lineNegotiateAPIVersion(0x1234, 0, 0x10004, 0x10004, &mv, mext));
    T("lineGetDevCaps(dev 0)", lineGetDevCaps(rapp, 0, 0x10004, 0, (LINEDEVCAPS*)buf), w32_lineGetDevCaps(mapp, 0, 0x10004, 0, mbuf));
    T("lineGetDevCaps(bad app)", lineGetDevCaps(0x1234, 0, 0x10004, 0, (LINEDEVCAPS*)buf), w32_lineGetDevCaps(0x1234, 0, 0x10004, 0, mbuf));
    HLINE rl = 0x77;
    uint32_t ml = 0x77;
    T("lineOpen(dev 0)", lineOpen(rapp, 0, &rl, 0x10004, 0, 0, LINECALLPRIVILEGE_OWNER, LINEMEDIAMODE_DATAMODEM, 0),
      w32_lineOpen(mapp, 0, &ml, 0x10004, 0, 0, LINECALLPRIVILEGE_OWNER, LINEMEDIAMODE_DATAMODEM, 0));
    check(rl == 0x77 && ml == 0x77, "%s: lineOpen leaves *line alone", when);
    T("lineOpen(bad app)", lineOpen(0x1234, 0, &rl, 0x10004, 0, 0, 1, 0x10, 0), w32_lineOpen(0x1234, 0, &ml, 0x10004, 0, 0, 1, 0x10, 0));
    T("lineClose(bogus)", lineClose(0x1234), w32_lineClose(0x1234));
    HCALL rc = 0x77;
    uint32_t mc = 0x77;
    T("lineMakeCall(bogus line)", lineMakeCall(0x1234, &rc, "555", 0, 0), w32_lineMakeCall(0x1234, &mc, "555", 0, 0));
    T("lineAnswer(bogus call)", lineAnswer(0x1234, 0, 0), w32_lineAnswer(0x1234, 0, 0));
    T("lineDrop(bogus call)", lineDrop(0x1234, 0, 0), w32_lineDrop(0x1234, 0, 0));
    T("lineDeallocateCall(bogus)", lineDeallocateCall(0x1234), w32_lineDeallocateCall(0x1234));
    T("lineGetCallInfo(bogus)", lineGetCallInfo(0x1234, (LINECALLINFO*)buf), w32_lineGetCallInfo(0x1234, mbuf));
    T("lineGetID(bogus line, comm/datamodem)", lineGetID(0x1234, 0, 0, LINECALLSELECT_LINE, (VARSTRING*)buf, "comm/datamodem"),
      w32_lineGetID(0x1234, 0, 0, 1, mbuf, "comm/datamodem"));
    T("lineGetDevConfig(dev 0)", lineGetDevConfig(0, (VARSTRING*)buf, "comm/datamodem"), w32_lineGetDevConfig(0, mbuf, "comm/datamodem"));
    T("lineSetDevConfig(dev 0)", lineSetDevConfig(0, buf, 0x6c, "comm/datamodem"), w32_lineSetDevConfig(0, mbuf, 0x6c, "comm/datamodem"));
    T("lineShutdown(bogus)", lineShutdown(0x1234), w32_lineShutdown(0x1234));
#undef T
}

static void test_tapi() {
    printf("\n== TAPI32 (not available) ==\n");
    tapi_round("before lineInitialize", 0x1234, 0x1234);
    HLINEAPP rapp = 0x77;
    uint32_t mapp = 0x77;
    DWORD rn = 0x77;
    uint32_t mn = 0x77;
    const LONG r = lineInitialize(&rapp, GetModuleHandleA(0), tapi_cb, "viperport w32 test", &rn);
    const int32_t m = w32_lineInitialize(&mapp, 0x400000, (void*)tapi_cb, "viperport w32 test", &mn);
    check(r == m && rn == mn, "lineInitialize: Windows %08lx, %lu devices (app %08lx); stand-in %08x, %u devices (app %08x)",
          (unsigned long)r, rn, (unsigned long)rapp, (unsigned)m, mn, mapp);
    if (r == 0 && rn != 0) info("this PC HAS %lu TAPI line devices: the stand-in (no modem) differs from it on purpose", rn);
    {
        HLINEAPP a2;
        uint32_t m2;
        DWORD n2;
        uint32_t mn2;
        same("lineInitialize(null app pointer)", R{lineInitialize(0, GetModuleHandleA(0), tapi_cb, "x", &n2), 0},
             R{w32_lineInitialize(0, 0x400000, (void*)tapi_cb, "x", &mn2), 0});
        same("lineInitialize(null devices pointer)", R{lineInitialize(&a2, GetModuleHandleA(0), tapi_cb, "x", 0), 0},
             R{w32_lineInitialize(&m2, 0x400000, (void*)tapi_cb, "x", 0), 0});
        same("lineInitialize(null callback)", R{lineInitialize(&a2, GetModuleHandleA(0), 0, "x", &n2), 0},
             R{w32_lineInitialize(&m2, 0x400000, 0, "x", &mn2), 0});
    }
    tapi_round("initialised", rapp, mapp);
    same("lineShutdown(the app)", R{lineShutdown(rapp), 0}, R{w32_lineShutdown(mapp), 0});
    same("lineShutdown(it again)", R{lineShutdown(rapp), 0}, R{w32_lineShutdown(mapp), 0});
    tapi_round("after lineShutdown", rapp, mapp);
}

// =====================================================================================================================
// the serial port: no COM ports
// =====================================================================================================================
static void test_serial() {
    printf("\n== serial (KERNEL32's comm functions) ==\n");
    char self[MAX_PATH];
    GetModuleFileNameA(0, self, MAX_PATH);
    HANDLE rf = CreateFileA(self, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    HANDLE re = CreateEventA(0, 0, 0, 0);
    const uint32_t mf = w32::add(std::make_shared<w32::Object>(w32::Kind::File));
    const uint32_t me = w32::add(std::make_shared<w32::Object>(w32::Kind::Event));
    struct H { const char* name; HANDLE real; uint32_t mine; };
    const H hs[] = {{"a disk file", rf, mf}, {"an event", re, me}, {"null", 0, 0}, {"INVALID_HANDLE_VALUE", INVALID_HANDLE_VALUE, 0xffffffffu},
                    {"an unknown handle", (HANDLE)0x5678, 0x5678}};
    for (const H& h : hs) {
        uint8_t rb[0x200], mb[0x200];
        memset(rb, 0x55, sizeof rb);
        memset(mb, 0x55, sizeof mb);
        DWORD rs = 0x77, rerr = 0x77;
        uint32_t ms = 0x77, merr = 0x77;
        char what[96];
#define C(nm, real, mine)                           \
    sprintf(what, "%s on %s", nm, h.name);          \
    same(what, REAL(real), MINE(mine))
        C("GetCommState", GetCommState(h.real, (DCB*)rb), w32_GetCommState(h.mine, mb));
        C("SetCommState", SetCommState(h.real, (DCB*)rb), w32_SetCommState(h.mine, mb));
        C("SetCommTimeouts", SetCommTimeouts(h.real, (COMMTIMEOUTS*)rb), w32_SetCommTimeouts(h.mine, mb));
        C("GetCommModemStatus", GetCommModemStatus(h.real, &rs), w32_GetCommModemStatus(h.mine, &ms));
        C("GetCommProperties", GetCommProperties(h.real, (COMMPROP*)rb), w32_GetCommProperties(h.mine, mb));
        C("ClearCommError", ClearCommError(h.real, &rerr, (COMSTAT*)rb), w32_ClearCommError(h.mine, &merr, mb));
#undef C
        int changed = 0, first = -1;
        for (int i = 0; i < 0x200; i++)
            if (rb[i] != mb[i]) { changed++; if (first < 0) first = i; }
        check(rs == ms && rerr == merr && !changed, "the out-parameters on %s: the same as Windows leaves them (status %lx/%x, "
              "errors %lx/%x, %d buffer bytes differ from %d)", h.name, rs, ms, rerr, merr, changed, first);
    }
    CloseHandle(rf);
    CloseHandle(re);
    w32::close(mf);
    w32::close(me);
    for (int i = 1; i <= 4; i++) {                    // enumerate_comport_devices' open, for the record
        char n[8];
        sprintf(n, "COM%d", i);
        SetLastError(0);
        HANDLE h = CreateFileA(n, 0xc0000000u, 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
        const DWORD e = GetLastError();
        info("CreateFileA(\"%s\") here: %s (error %lu)%s", n, h != INVALID_HANDLE_VALUE ? "opens" : "fails", e,
             h != INVALID_HANDLE_VALUE ? " -- a COM port this PC has; the stand-ins' CreateFileA says there is none" : "");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
}

// =====================================================================================================================
// WSOCK32
// =====================================================================================================================
static HWND g_async_wnd;
static UINT g_async_msg;
static WPARAM g_async_wp;
static LPARAM g_async_lp;
static LRESULT CALLBACK async_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m >= WM_USER + 0x100) {
        g_async_msg = m;
        g_async_wp = wp;
        g_async_lp = lp;
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}
static bool wait_async(UINT msg) {
    g_async_msg = 0;
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 10000 && g_async_msg != msg;) {
        MSG m;
        while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        Sleep(5);
    }
    return g_async_msg == msg;
}

static void test_winsock() {
    printf("\n== WSOCK32 ==\n");
    uint8_t rd[400], md[400], fd[400];
    memset(rd, 0x55, sizeof rd);
    memset(md, 0x55, sizeof md);
    memset(fd, 0x55, sizeof fd);
    const int r = WSAStartup(0x101, (WSADATA*)rd);
    const int m = w32_WSAStartup(0x101, md);
    w32_wsadata_fill(0x101, fd);
    check(r == m && !memcmp(rd, md, 400), "WSAStartup(1.1): result and all 400 bytes of WSADATA (%d, %d)", r, m);
    {
        int diff = 0;
        for (int i = 0; i < 400; i++)
            if (rd[i] != fd[i]) {
                if (diff++ < 8) printf("      WSADATA byte %d: Windows %02x, Linux branch %02x\n", i, rd[i], fd[i]);
            }
        check(!diff, "the Linux branch's WSADATA = Windows' (all 400 bytes; %d differ)", diff);
    }
    same("WSAStartup(0.0)", R{WSAStartup(0, (WSADATA*)rd), 0}, R{w32_WSAStartup(0, md), 0});

    same("socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX): IPX isn't here", REAL(socket(6, 2, 0x3e8)), MINE(w32_socket(6, 2, 0x3e8)));
    same("  WSAGetLastError after it", R{WSAGetLastError(), 0}, R{w32_WSAGetLastError(), 0});
    same("socket(AF_INET, 99, 0)", REAL(socket(2, 99, 0)), MINE(w32_socket(2, 99, 0)));

    // two UDP sockets, as UDPSocket makes them: bound, broadcast, non-blocking -- one through Windows, one the stand-ins'
    const SOCKET rs = socket(2, 2, 0);
    const uint32_t ms = w32_socket(2, 2, 0);
    check(rs != INVALID_SOCKET && ms != 0xffffffffu, "socket(AF_INET, SOCK_DGRAM, 0): both");
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    same("bind(127.0.0.1:0)", REAL(bind(rs, (sockaddr*)&a, sizeof a)), MINE(w32_bind(ms, &a, sizeof a)));
    sockaddr_in rme, mme;
    int len = sizeof rme;
    getsockname(rs, (sockaddr*)&rme, &len);
    len = sizeof mme;
    getsockname(ms, (sockaddr*)&mme, &len);
    same("bind(short length 8)", REAL(bind(rs, (sockaddr*)&a, 8)), MINE(w32_bind(ms, &a, 8)));
    {
        const SOCKET r2 = socket(2, 2, 0);
        const uint32_t m2 = w32_socket(2, 2, 0);
        same("bind(a port in use)", REAL(bind(r2, (sockaddr*)&rme, sizeof rme)), MINE(w32_bind(m2, &mme, sizeof mme)));
        closesocket(r2);
        w32_closesocket(m2);
    }
    BOOL one = 1;
    same("setsockopt(SOL_SOCKET, SO_BROADCAST, 1)", REAL(setsockopt(rs, 0xffff, 0x20, (const char*)&one, 4)),
         MINE(w32_setsockopt(ms, 0xffff, 0x20, &one, 4)));
    same("setsockopt(NSPROTO_IPX, 0x4007) on a UDP socket", REAL(setsockopt(rs, 0x3e8, 0x4007, (const char*)&one, 4)),
         MINE(w32_setsockopt(ms, 0x3e8, 0x4007, &one, 4)));
    u_long nb = 1;
    uint32_t mnb = 1;
    same("ioctlsocket(FIONBIO, 1)", REAL(ioctlsocket(rs, (long)0x8004667e, &nb)), MINE(w32_ioctlsocket(ms, (int32_t)0x8004667e, &mnb)));
    same("ioctlsocket(bogus command)", REAL(ioctlsocket(rs, 0x1234, &nb)), MINE(w32_ioctlsocket(ms, 0x1234, &mnb)));
    char buf[64];
    sockaddr_in from;
    int fl = sizeof from;
    same("recvfrom(nothing there, non-blocking)", REAL(recvfrom(rs, buf, sizeof buf, 0, (sockaddr*)&from, &fl)),
         MINE(w32_recvfrom(ms, buf, sizeof buf, 0, &from, &fl)));
    int ropt = 0, mopt = 0, rl = 4, ml = 4;
    const R g1 = REAL(getsockopt(rs, 0xffff, SO_RCVBUF, (char*)&ropt, &rl));
    const R g2 = MINE(w32_getsockopt(ms, 0xffff, SO_RCVBUF, &mopt, &ml));
    check(g1.v == g2.v && g1.e == g2.e && ropt == mopt && rl == ml, "getsockopt(SO_RCVBUF): %d (%d bytes) and %d (%d bytes)", ropt, rl, mopt, ml);
    rl = ml = 4;
    same("getsockopt(NSPROTO_IPX, 0x4007) on a UDP socket", REAL(getsockopt(rs, 0x3e8, 0x4007, (char*)&ropt, &rl)),
         MINE(w32_getsockopt(ms, 0x3e8, 0x4007, &mopt, &ml)));
    // a datagram each way, received through the other's API
    const char hello[] = "viperport says hello";
    same("sendto(the other socket)", REAL(sendto(rs, hello, sizeof hello, 0, (sockaddr*)&mme, sizeof mme)),
         MINE(w32_sendto(ms, hello, sizeof hello, 0, &rme, sizeof rme)));
    Sleep(50);
    char rb[64] = "", mb[64] = "";
    sockaddr_in rfrom = {}, mfrom = {};
    int rfl = sizeof rfrom, mfl = sizeof mfrom;
    const int rr = recvfrom(rs, rb, sizeof rb, 0, (sockaddr*)&rfrom, &rfl);
    const int mr = w32_recvfrom(ms, mb, sizeof mb, 0, &mfrom, &mfl);
    check(rr == (int)sizeof hello && mr == (int)sizeof hello && !strcmp(rb, hello) && !strcmp(mb, hello) &&
              rfrom.sin_port == mme.sin_port && mfrom.sin_port == rme.sin_port && rfl == mfl,
          "recvfrom: the datagram, its sender and the address length (%d / %d bytes)", rr, mr);
    // too long for the buffer: Windows' WSAEMSGSIZE
    sendto(rs, hello, sizeof hello, 0, (sockaddr*)&rme, sizeof rme);
    w32_sendto(ms, hello, sizeof hello, 0, &mme, sizeof mme);
    Sleep(50);
    memset(rb, 0, sizeof rb);
    memset(mb, 0, sizeof mb);
    rfl = mfl = sizeof rfrom;
    const R t1 = REAL(recvfrom(rs, rb, 8, 0, (sockaddr*)&rfrom, &rfl));
    const R t2 = MINE(w32_recvfrom(ms, mb, 8, 0, &mfrom, &mfl));
    same("recvfrom(an 8-byte buffer for 21 bytes)", t1, t2);
    check(!memcmp(rb, mb, 8), "  ... the first 8 bytes are there in both");
    rfl = mfl = 8;
    same("recvfrom(fromlen 8)", REAL(recvfrom(rs, rb, sizeof rb, 0, (sockaddr*)&rfrom, &rfl)), MINE(w32_recvfrom(ms, mb, sizeof mb, 0, &mfrom, &mfl)));
    same("closesocket", REAL(closesocket(rs)), MINE(w32_closesocket(ms)));
    same("closesocket(it again)", REAL(closesocket(rs)), MINE(w32_closesocket(ms)));
    same("sendto(a closed socket)", REAL(sendto(rs, hello, 4, 0, (sockaddr*)&rme, sizeof rme)), MINE(w32_sendto(ms, hello, 4, 0, &rme, sizeof rme)));

    // inet_addr: computed here, on every OS
    const char* const addrs[] = {"127.0.0.1", "192.168.1.20", "255.255.255.255", "0.0.0.0", "1.2.3", "1.2", "16909060", "0x7f.1",
                                 "0177.0.0.1", "010.010.010.010", "1.2.3.4 trailing", "1.2.3.4.5", "256.1.1.1", "1.2.3.256",
                                 "1.2.65535", "1.2.65536", "1.16777215", "4294967295", "4294967296", "", " 1.2.3.4", "1..2.3",
                                 "1.2.3.", "a.b.c.d", "0x", "08.1.1.1", "0xff.0xff.0xff.0xff", "1.2.3.4\t", "localhost",
                                 "viper.example.com", "1.2.3.4x", "0x100.1.1.1", "00000000001.2.3.4", "1.2.3.4\n",
                                 "1.2.3.4\r", "1.2.3.4;", "1.2.3.4\v", "0x7F000001", "0X7f.0.0.1", "1 2"};
    int same_n = 0;
    for (const char* s : addrs) {
        const unsigned long real = inet_addr(s);
        const uint32_t mine = w32_inet_addr(s);
        if (real == mine) same_n++;
        else printf("      inet_addr(\"%s\"): Windows %08lx, stand-in %08x\n", s, real, mine);
    }
    check(same_n == (int)(sizeof addrs / sizeof addrs[0]), "inet_addr: %d of %u inputs the same", same_n, (unsigned)(sizeof addrs / sizeof addrs[0]));
    check(w32_htons(0x07d1) == htons(0x07d1) && w32_ntohs(0xd107) == ntohs(0xd107), "htons / ntohs");

    char rh[256] = "", mh[256] = "";
    same("gethostname", REAL(gethostname(rh, sizeof rh)), MINE(w32_gethostname(mh, sizeof mh)));
    check(!strcmp(rh, mh), "  ... the same name (%s)", mh);
    same("gethostname(1-byte buffer)", REAL(gethostname(rh, 1)), MINE(w32_gethostname(mh, 1)));
    for (const char* name : {"localhost", (const char*)mh}) {
        const hostent* h1 = gethostbyname(name);
        std::vector<uint32_t> want;
        std::string canon;
        int16_t type = 0, length = 0;
        if (h1) {
            canon = h1->h_name;
            type = h1->h_addrtype;
            length = h1->h_length;
            for (int i = 0; h1->h_addr_list[i]; i++) want.push_back(*(const uint32_t*)h1->h_addr_list[i]);
        }
        const hostent* h2 = (const hostent*)w32_gethostbyname(name);
        bool ok = h1 && h2 && !strcmp(h2->h_name, canon.c_str()) && h2->h_addrtype == type && h2->h_length == length;
        int n2 = 0;
        for (; ok && h2->h_addr_list[n2]; n2++) ok &= n2 < (int)want.size() && *(const uint32_t*)h2->h_addr_list[n2] == want[n2];
        ok &= n2 == (int)want.size();
        check(ok, "gethostbyname(\"%s\"): name, type, length and %u addresses", name, (unsigned)want.size());
        // the Linux branch's layout of the same answer, read back through winsock's struct
        char lay[1024];
        uint32_t need = 0;
        const uint32_t size = w32_hostent_lay(lay, sizeof lay, canon.c_str(), want.data(), (uint32_t)want.size(), &need);
        const hostent* h3 = (const hostent*)lay;
        bool ok3 = size && size == need && !strcmp(h3->h_name, canon.c_str()) && h3->h_addrtype == 2 && h3->h_length == 4 && !h3->h_aliases[0];
        for (size_t i = 0; ok3 && i < want.size(); i++) ok3 &= *(const uint32_t*)h3->h_addr_list[i] == want[i];
        ok3 &= ok3 && !h3->h_addr_list[want.size()];
        check(ok3, "  ... the Linux branch's hostent layout of it reads back the same through winsock's struct (%u bytes)", size);
        check(!w32_hostent_lay(lay, need - 1, canon.c_str(), want.data(), (uint32_t)want.size(), 0), "  ... and doesn't fit in one byte less");
    }
    same("gethostbyname(\"no-such-host.invalid\")", REAL(U32V(gethostbyname("no-such-host.invalid"))),
         MINE(U32V(w32_gethostbyname("no-such-host.invalid"))));

    // the async lookup: its reply is a message to the window, as the game's message hook takes it
    WNDCLASSA wc = {};
    wc.lpfnWndProc = async_proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "viperport w32 async";
    RegisterClassA(&wc);
    g_async_wnd = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, wc.hInstance, 0);
    static char rbuf[0x400], mbuf[0x400];
    const UINT msg = WM_USER + 0x100 + 0xf400 - 0xf400 + 1;
    HANDLE rq = WSAAsyncGetHostByName(g_async_wnd, msg, "localhost", rbuf, sizeof rbuf);
    const bool rgot = rq && wait_async(msg);
    const LPARAM rlp = g_async_lp;
    const WPARAM rwp = g_async_wp;
    const uint32_t mq = w32_WSAAsyncGetHostByName(U32V(g_async_wnd), msg, "localhost", mbuf, sizeof mbuf);
    const bool mgot = mq && wait_async(msg);
    const LPARAM mlp = g_async_lp;
    const WPARAM mwp = g_async_wp;
    const hostent* ra = (const hostent*)rbuf;
    const hostent* ma = (const hostent*)mbuf;
    check(rgot && mgot && rwp == (WPARAM)rq && mwp == mq && WSAGETASYNCERROR(rlp) == WSAGETASYNCERROR(mlp) &&
              WSAGETASYNCBUFLEN(rlp) == WSAGETASYNCBUFLEN(mlp) && !WSAGETASYNCERROR(mlp) &&
              *(const uint32_t*)ra->h_addr_list[0] == *(const uint32_t*)ma->h_addr_list[0],
          "WSAAsyncGetHostByName(\"localhost\"): the reply message (handle, error %d, length %d) and the first address",
          WSAGETASYNCERROR(mlp), WSAGETASYNCBUFLEN(mlp));
    same("WSACancelAsyncRequest(a request already answered)", REAL(WSACancelAsyncRequest(rq)), MINE(w32_WSACancelAsyncRequest(mq)));
    same("WSACancelAsyncRequest(bogus)", REAL(WSACancelAsyncRequest((HANDLE)0x1234)), MINE(w32_WSACancelAsyncRequest(0x1234)));
    DestroyWindow(g_async_wnd);

    // the stand-ins share winsock's count with the real calls here (and wsock32 keeps references of its own): cleaned up
    // until winsock says it isn't started, both then fail alike
    R mc = {0, 0};
    for (int i = 0; i < 8 && mc.v == 0; i++) mc = MINE(w32_WSACleanup());
    same("WSACleanup once winsock is stopped", REAL(WSACleanup()), mc);
}

int main() {
    GetSystemMetrics(SM_CXSCREEN);                       // (the thread's first USER32 call sets the last error: done here)
    test_user();
    test_registry();
    test_tapi();
    test_serial();
    test_winsock();
    printf("\n%d passed, %d FAILED\n", g_pass, g_fail);
    return g_fail;
}
