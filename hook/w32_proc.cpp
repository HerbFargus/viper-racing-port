// w32_proc.cpp -- the process-image, environment and module stand-ins (w32_kernel.h).
//
// Who the game is: the loader says (w32_set_process_image) -- race.exe's image base, its path, its command line, as if
// Windows had started race.exe (the same answers as loader/viperport_main.cpp's own race_GetModuleHandleA /
// race_GetModuleFileNameA / race_GetCommandLineA shims). Every other module is the OS's: on Windows the real DLLs; on
// Linux the DLLs race.exe imports are names only (a pseudo-handle each, for GetModuleHandleA / LoadLibraryA /
// GetProcAddress).
// What the game loads: the C runtime's FDIV check LoadLibraryA("KERNEL32") and GetProcAddress's
// GetProcessAffinityMask, GetCurrentProcess, SetThreadAffinityMask, GetCurrentThread (once a run); its message box,
// only on a fatal C runtime error: LoadLibraryA("user32.dll") and MessageBoxA, GetActiveWindow, GetLastActivePopup.
// GetProcAddress gives the stand-ins for those (agent C's table through w32_set_proc_lookup, else this file's
// KERNEL32/WINMM list), so the game reaches no Windows function by that road either.
//
// The environment (the C runtime reads it once, at start: GetEnvironmentStringsW, then WideCharToMultiByte twice,
// FreeEnvironmentStringsW), GetStartupInfoA (the C runtime: inherited handles, WinMain's nCmdShow): Windows' own on
// Windows; on Linux built from environ (UTF-8 -> UTF-16, and on to code page 1252 with Windows' best fit for the A
// form) and a zeroed STARTUPINFOA.
#include "w32_kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
extern char** environ;
#endif

namespace {

const uint32_t ERR_MOD_NOT_FOUND = 126, ERR_PROC_NOT_FOUND = 127;

uint32_t g_base;                                 // race.exe's image base (0: not told)
std::string g_path, g_cmdline;
W32ProcLookup g_lookup;

bool same_ci(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return false;
    }
    return *a == *b;
}
// "C:\Windows\System32\KERNEL32.DLL" / "kernel32.dll" / "KERNEL32" -> "KERNEL32" (upper case)
std::string dll_name(const char* path) {
    const char* s = path;
    for (const char* p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') s = p + 1;
    std::string n(s);
    const size_t dot = n.rfind('.');
    if (dot != std::string::npos && same_ci(n.c_str() + dot, ".dll")) n.resize(dot);
    for (char& c : n)
        if (c >= 'a' && c <= 'z') c -= 32;
    return n;
}

#ifdef _WIN32
// the Windows call's last error, if it set one, becomes the stand-ins' (calls that succeed mostly leave it alone);
// Windows' own is put back as it was
struct ErrorProbe {
    static const DWORD MARK = 0x5ca1ab1e;
    DWORD was;
    ErrorProbe() : was(GetLastError()) { SetLastError(MARK); }
    void pass() {
        const DWORD e = GetLastError();
        if (e != MARK) w32::set_last_error(e);
        else SetLastError(was);
    }
};
#else
// race.exe's DLLs, by name: module handle 0x70000000 + 0x10000 * index (a name, not memory)
const char* const k_dlls[] = {"KERNEL32", "USER32", "GDI32", "ADVAPI32", "WINMM", "WSOCK32", "TAPI32", "DDRAW",
                              "DSOUND", "DINPUT"};
const int N_DLLS = sizeof k_dlls / sizeof k_dlls[0];
uint32_t pseudo_module(const char* name) {
    const std::string n = dll_name(name);
    for (int i = 0; i < N_DLLS; i++)
        if (n == k_dlls[i]) return 0x70000000u + 0x10000u * (uint32_t)i;
    return 0;
}
const char* pseudo_name(uint32_t m) {
    const uint32_t i = (m - 0x70000000u) / 0x10000u;
    return m >= 0x70000000u && (m & 0xffff) == 0 && i < (uint32_t)N_DLLS ? k_dlls[i] : 0;
}

// UTF-8 -> UTF-16 (a malformed sequence: U+FFFD, as Windows' own conversion) and back
std::vector<uint16_t> utf16_of(const char* s) {
    std::vector<uint16_t> w;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        uint32_t c = *p, need = 0;
        if (c < 0x80) need = 0;
        else if ((c & 0xe0) == 0xc0) { c &= 0x1f; need = 1; }
        else if ((c & 0xf0) == 0xe0) { c &= 0x0f; need = 2; }
        else if ((c & 0xf8) == 0xf0) { c &= 0x07; need = 3; }
        else { w.push_back(0xfffd); p++; continue; }
        p++;
        uint32_t k = 0;
        for (; k < need && (*p & 0xc0) == 0x80; k++, p++) c = (c << 6) | (*p & 0x3f);
        if (k < need || c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) { w.push_back(0xfffd); continue; }
        if (c >= 0x10000) {
            c -= 0x10000;
            w.push_back((uint16_t)(0xd800 + (c >> 10)));
            w.push_back((uint16_t)(0xdc00 + (c & 0x3ff)));
        } else {
            w.push_back((uint16_t)c);
        }
    }
    return w;
}
std::string utf8_of(const uint16_t* w, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (w[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xc0 | (c >> 6)); s += (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { s += (char)(0xe0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3f)); s += (char)(0x80 | (c & 0x3f)); }
        else { s += (char)(0xf0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3f)); s += (char)(0x80 | ((c >> 6) & 0x3f)); s += (char)(0x80 | (c & 0x3f)); }
    }
    return s;
}
// code page 1252 text (the game's) -> UTF-8 (Linux's)
std::string utf8_of_ansi(const char* a) {
    const int32_t n = w32_MultiByteToWideChar(0, 0, a, -1, 0, 0);
    std::vector<uint16_t> w(n > 0 ? n : 1);
    w32_MultiByteToWideChar(0, 0, a, -1, w.data(), (int32_t)w.size());
    return utf8_of(w.data(), n > 0 ? (size_t)n - 1 : 0);
}
// the environment block, UTF-16: "name=value\0" ... "\0"
std::vector<uint16_t> env_block_w() {
    std::vector<uint16_t> b;
    for (char** e = environ; e && *e; e++) {
        std::vector<uint16_t> w = utf16_of(*e);
        b.insert(b.end(), w.begin(), w.end());
        b.push_back(0);
    }
    if (b.empty()) b.push_back(0);
    b.push_back(0);
    return b;
}
#endif

}  // namespace

void w32_set_process_image(uint32_t base, const char* path, const char* command_line) {
    g_base = base;
    g_path = path ? path : "";
    g_cmdline = command_line ? command_line : "";
}
void w32_set_proc_lookup(W32ProcLookup lookup) { g_lookup = lookup; }

// ---- the command line, the environment ------------------------------------------------------------------------------
char* W32K_CALL w32_GetCommandLineA() {
    if (g_base) return (char*)g_cmdline.c_str();
#ifdef _WIN32
    return GetCommandLineA();
#else
    static std::string cl;                       // (not told: this process's own, from /proc)
    static std::once_flag once;
    std::call_once(once, [] {
        FILE* f = fopen("/proc/self/cmdline", "rb");
        if (!f) return;
        std::string a;
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (c) { a += (char)c; continue; }
            if (!cl.empty()) cl += ' ';
            cl += a.find(' ') != std::string::npos ? "\"" + a + "\"" : a;
            a.clear();
        }
        fclose(f);
    });
    return (char*)cl.c_str();
#endif
}

#ifdef _WIN32
char* W32K_CALL w32_GetEnvironmentStrings() {
    typedef char*(WINAPI * Get_t)();
    static const Get_t get = (Get_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetEnvironmentStrings");
    ErrorProbe p;
    char* r = get();
    p.pass();
    return r;
}
uint16_t* W32K_CALL w32_GetEnvironmentStringsW() {
    ErrorProbe p;
    uint16_t* r = (uint16_t*)GetEnvironmentStringsW();
    p.pass();
    return r;
}
int32_t W32K_CALL w32_FreeEnvironmentStringsA(char* block) {
    ErrorProbe p;
    const BOOL r = FreeEnvironmentStringsA(block);
    p.pass();
    return r;
}
int32_t W32K_CALL w32_FreeEnvironmentStringsW(uint16_t* block) {
    ErrorProbe p;
    const BOOL r = FreeEnvironmentStringsW((LPWCH)block);
    p.pass();
    return r;
}
int32_t W32K_CALL w32_SetEnvironmentVariableA(const char* name, const char* value) {
    ErrorProbe p;
    const BOOL r = SetEnvironmentVariableA(name, value);
    p.pass();
    return r;
}
void W32K_CALL w32_GetStartupInfoA(W32StartupInfoA* si) {
    static_assert(sizeof(STARTUPINFOA) == sizeof(W32StartupInfoA), "STARTUPINFOA");
    GetStartupInfoA((STARTUPINFOA*)si);
}
#else
char* W32K_CALL w32_GetEnvironmentStrings() {
    const std::vector<uint16_t> w = env_block_w();
    const int32_t n = w32_WideCharToMultiByte(0, 0, w.data(), (int32_t)w.size(), 0, 0, 0, 0);
    char* a = (char*)malloc(n > 0 ? n : 1);
    if (a) w32_WideCharToMultiByte(0, 0, w.data(), (int32_t)w.size(), a, n, 0, 0);
    return a;
}
uint16_t* W32K_CALL w32_GetEnvironmentStringsW() {
    const std::vector<uint16_t> w = env_block_w();
    uint16_t* b = (uint16_t*)malloc(w.size() * 2);
    if (b) memcpy(b, w.data(), w.size() * 2);
    return b;
}
int32_t W32K_CALL w32_FreeEnvironmentStringsA(char* block) {
    free(block);
    return 1;
}
int32_t W32K_CALL w32_FreeEnvironmentStringsW(uint16_t* block) {
    free(block);
    return 1;
}
int32_t W32K_CALL w32_SetEnvironmentVariableA(const char* name, const char* value) {
    if (!name || !*name || strchr(name + 1, '=')) {
        w32::set_last_error(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    const std::string n = utf8_of_ansi(name);
    if (!value) {                                // (removing one that isn't there succeeds, as Windows)
        unsetenv(n.c_str());
        return 1;
    }
    setenv(n.c_str(), utf8_of_ansi(value).c_str(), 1);
    return 1;
}
void W32K_CALL w32_GetStartupInfoA(W32StartupInfoA* si) {
    memset(si, 0, sizeof *si);                   // (no STARTF_ flags: WinMain gets SW_SHOWDEFAULT)
    si->cb = sizeof *si;
}
#endif

// Win32 has no handle limit to set: it returns its argument
uint32_t W32K_CALL w32_SetHandleCount(uint32_t count) { return count; }

// ---- modules -------------------------------------------------------------------------------------------------------
uint32_t W32K_CALL w32_GetModuleHandleA(const char* name) {
    if (!name) {
#ifdef _WIN32
        if (!g_base) return (uint32_t)(uintptr_t)GetModuleHandleA(0);
#endif
        return g_base ? g_base : 0x400000u;     // (the last error untouched, as Windows')
    }
#ifdef _WIN32
    ErrorProbe p;
    HMODULE m = GetModuleHandleA(name);
    p.pass();
    return (uint32_t)(uintptr_t)m;
#else
    const uint32_t m = pseudo_module(name);
    if (!m) w32::set_last_error(ERR_MOD_NOT_FOUND);
    return m;
#endif
}

uint32_t W32K_CALL w32_GetModuleFileNameA(uint32_t module, char* buf, uint32_t size) {
    if ((!module || module == g_base) && g_base) {   // race.exe: its path, as the loader's shim gives it
        if (!size) {
            w32::set_last_error(w32::ERR_INSUFFICIENT_BUFFER);
            return 0;
        }
        const uint32_t len = (uint32_t)g_path.size();
        if (len >= size) {                       // truncated and terminated, as Windows (XP and later)
            memcpy(buf, g_path.c_str(), size - 1);
            buf[size - 1] = 0;
            w32::set_last_error(w32::ERR_INSUFFICIENT_BUFFER);
            return size;
        }
        memcpy(buf, g_path.c_str(), len + 1);
        w32::set_last_error(0);
        return len;
    }
#ifdef _WIN32
    ErrorProbe p;
    const DWORD r = GetModuleFileNameA((HMODULE)(uintptr_t)module, buf, size);
    p.pass();
    return r;
#else
    w32::set_last_error(ERR_MOD_NOT_FOUND);      // (a DLL here is a name, not a file)
    return 0;
#endif
}

uint32_t W32K_CALL w32_LoadLibraryA(const char* name) {
#ifdef _WIN32
    ErrorProbe p;
    HMODULE m = LoadLibraryA(name);
    p.pass();
    return (uint32_t)(uintptr_t)m;
#else
    const uint32_t m = name ? pseudo_module(name) : 0;
    if (!m) w32::set_last_error(ERR_MOD_NOT_FOUND);
    return m;
#endif
}

void* W32K_CALL w32_GetProcAddress(uint32_t module, const char* name) {
    const bool by_ordinal = (uintptr_t)name < 0x10000;
    std::string dll;
#ifdef _WIN32
    char path[MAX_PATH];
    if (!by_ordinal && GetModuleFileNameA((HMODULE)(uintptr_t)module, path, MAX_PATH)) dll = dll_name(path);
#else
    if (const char* n = pseudo_name(module)) dll = n;
    else if (module && module == g_base) dll = "RACE";
    else {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
#endif
    if (!by_ordinal && !dll.empty()) {
        if (g_lookup)
            if (void* f = g_lookup(dll.c_str(), name)) return f;
        if (void* f = w32_kernel_proc(dll.c_str(), name)) return f;
    }
#ifdef _WIN32
    ErrorProbe p;
    FARPROC f = GetProcAddress((HMODULE)(uintptr_t)module, name);
    p.pass();
    return (void*)f;
#else
    w32::set_last_error(ERR_PROC_NOT_FOUND);     // (an ordinal, or a function no stand-in implements)
    return 0;
#endif
}

// ---- this area's stand-ins by name (GetProcAddress's fallback; agent C's table may use it too) -------------------------
void* w32_kernel_proc(const char* dll, const char* name) {
    struct Entry { const char* name; void* fn; };
#define E(n) {#n, (void*)&w32_##n}
    static const Entry kernel32[] = {
        E(HeapCreate), E(HeapDestroy), E(HeapAlloc), E(HeapReAlloc), E(HeapFree), E(HeapSize), E(HeapValidate),
        E(GlobalAlloc), E(GlobalLock), E(GlobalUnlock), E(GlobalFree), E(GlobalSize), E(GetSystemInfo), E(GetVersion),
        E(CreateThread), E(ExitThread), E(GetExitCodeThread), E(SuspendThread), E(ResumeThread), E(SetThreadPriority),
        E(GetCurrentThread), E(GetCurrentThreadId), E(GetCurrentProcess), E(ExitProcess), E(CreateProcessA),
        E(GetExitCodeProcess), E(Sleep), E(InitializeCriticalSection), E(EnterCriticalSection),
        E(LeaveCriticalSection), E(DeleteCriticalSection), E(CreateEventA), E(PulseEvent), E(SetEvent), E(ResetEvent),
        E(CreateSemaphoreA), E(ReleaseSemaphore), E(WaitForSingleObject),
        E(GetProcessAffinityMask), E(SetThreadAffinityMask), E(QueryPerformanceCounter),
        E(QueryPerformanceFrequency), E(GetLocalTime), E(GetCommandLineA), E(GetEnvironmentStrings),
        E(GetEnvironmentStringsW), E(FreeEnvironmentStringsA), E(FreeEnvironmentStringsW),
        E(SetEnvironmentVariableA), E(GetStartupInfoA), E(SetHandleCount), E(GetModuleHandleA),
        E(GetModuleFileNameA), E(LoadLibraryA), E(GetProcAddress), E(GetACP), E(GetOEMCP), E(GetCPInfo),
        E(MultiByteToWideChar), E(WideCharToMultiByte), E(LCMapStringA), E(LCMapStringW), E(GetStringTypeA),
        E(GetStringTypeW), E(GetLocaleInfoA), E(GetCurrencyFormatA), E(GetDateFormatA),
        E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter), E(RaiseException), E(RtlUnwind),
    };
    static const Entry winmm[] = {E(timeGetTime), E(timeKillEvent)};
#undef E
    const Entry* t;
    size_t n;
    if (same_ci(dll, "KERNEL32")) { t = kernel32; n = sizeof kernel32 / sizeof *t; }
    else if (same_ci(dll, "WINMM")) { t = winmm; n = sizeof winmm / sizeof *t; }
    else return 0;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(t[i].name, name)) return t[i].fn;
    return 0;
}
