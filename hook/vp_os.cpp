// vp_os.cpp -- the port's own calls to the operating system, the stand-in kind (vp_os.h).
//
// With the stand-ins active (the GCC standalone) the port's calls go where the game's go: the stand-ins of
// hook/w32_*.cpp, found by name. A stand-in that isn't linked (a weak reference: null) means race.exe's slot for it
// still reaches Windows (the loader keeps the real import), so the port's call goes to Windows too and is counted
// (vp_os_report) -- the port and the game stay on the same side. A handle is the stand-ins' (w32_handle.h) when the
// handle table knows it, else Windows'.
// The port's own pieces of the kind, which the game never imports, are implemented here in portable C++: its events
// (an object in the stand-ins' handle table, so a wait or a close takes it as it takes the game's), its ini reads
// (GetPrivateProfileStringA / IntA, as Windows reads a file: test/vp_os_test.cpp compares them with Windows') and
// GetFileAttributesA. Both compilers compile this file; the MSVC build never calls it.
#define _CRT_SECURE_NO_WARNINGS
#define VP_OS_IMPL                                   // (vp_os.h: these definitions aren't weak)
#include "vp_os.h"
#include "w32_handle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

void logf(const char* fmt, ...);

bool vp_os_on;

// ---- the stand-ins the port's calls use (hook/w32_*.cpp: agents A, B, C) ------------------------------------------------
// Their owners' headers, and each one used here declared again weak (the same signature), so the build links while
// one isn't written yet (see the top).
#ifdef VP_GCC
#include "w32_file.h"
#include "w32_kernel.h"
#include "w32_user.h"
#define VP_SI __attribute__((weak))
uint32_t W32K_CALL w32_GetCurrentThreadId() VP_SI;                                                     // B
uint32_t W32K_CALL w32_GetLastError() VP_SI;                                                           // A / B
uint32_t W32K_CALL w32_SuspendThread(uint32_t thread) VP_SI;                                           // B
uint32_t W32K_CALL w32_ResumeThread(uint32_t thread) VP_SI;                                            // B
uint32_t W32K_CALL w32_WaitForSingleObject(uint32_t handle, uint32_t ms) VP_SI;                        // B
uint32_t W32_STDCALL w32_CloseHandle(w32_HANDLE h) VP_SI;                                              // A
void W32K_CALL w32_Sleep(uint32_t ms) VP_SI;                                                           // B
void W32K_CALL w32_ExitProcess(uint32_t code) VP_SI;                                                   // B
void W32K_CALL w32_InitializeCriticalSection(W32CriticalSection* cs) VP_SI;                            // B
void W32K_CALL w32_EnterCriticalSection(W32CriticalSection* cs) VP_SI;                                 // B
void W32K_CALL w32_LeaveCriticalSection(W32CriticalSection* cs) VP_SI;                                 // B
uint32_t W32_STDCALL w32_CreateDirectoryA(const char* name, void* sa) VP_SI;                           // A
uint32_t W32_STDCALL w32_CopyFileA(const char* from, const char* to, uint32_t fail_if_exists) VP_SI;   // A
w32_HANDLE W32_STDCALL w32_FindFirstFileA(const char* pattern, w32::FindDataA* fd) VP_SI;              // A
uint32_t W32_STDCALL w32_FindNextFileA(w32_HANDLE h, w32::FindDataA* fd) VP_SI;                        // A
uint32_t W32_STDCALL w32_FindClose(w32_HANDLE h) VP_SI;                                                // A
void W32K_CALL w32_GetLocalTime(W32SystemTime* st) VP_SI;                                              // B
int32_t W32K_CALL w32_MultiByteToWideChar(uint32_t code_page, uint32_t flags, const char* src, int32_t src_len,
                                          uint16_t* dst, int32_t dst_len) VP_SI;                       // B
int32_t W32K_CALL w32_WideCharToMultiByte(uint32_t code_page, uint32_t flags, const uint16_t* src, int32_t src_len,
                                          char* dst, int32_t dst_len, const char* default_char,
                                          int32_t* used_default) VP_SI;                                // B
int __stdcall w32_GetSystemMetrics(int index) VP_SI;                                                   // C
uint32_t __stdcall w32_GetForegroundWindow(void) VP_SI;                                                // C
uint32_t __stdcall w32_LoadIconA(uint32_t instance, const char* name) VP_SI;                           // C
int __stdcall w32_ClipCursor(const void* rect) VP_SI;                                                  // C
static_assert(sizeof(W32CriticalSection) == sizeof(CRITICAL_SECTION), "CRITICAL_SECTION");
static_assert(sizeof(W32SystemTime) == sizeof(SYSTEMTIME), "SYSTEMTIME");
static_assert(sizeof(w32::FindDataA) == sizeof(WIN32_FIND_DATAA), "WIN32_FIND_DATAA");
#define HAVE(f) (&f != nullptr)
#else
#define HAVE(f) false                                // MSVC: never active (the macros in vp_os.h call Windows)
#endif

namespace {

// the S kind's calls that found no stand-in (went to Windows): counted per function, logged once each and at exit
enum Missing {
    M_THREAD_ID, M_LAST_ERROR, M_SUSPEND, M_RESUME, M_CLOSE, M_SLEEP, M_EXIT, M_CS, M_CREATE_DIR, M_COPY, M_FIND,
    M_LOCAL_TIME, M_MB, M_WC, M_METRICS, M_FOREGROUND, M_CLIP, M_ICON, M_N
};
const char* const k_missing[M_N] = {"GetCurrentThreadId", "GetLastError", "SuspendThread", "ResumeThread", "CloseHandle",
    "Sleep", "ExitProcess", "Enter/Leave/InitializeCriticalSection", "CreateDirectoryA", "CopyFileA",
    "FindFirstFileA/FindNextFileA/FindClose", "GetLocalTime", "MultiByteToWideChar", "WideCharToMultiByte",
    "GetSystemMetrics", "GetForegroundWindow", "ClipCursor", "LoadIconA"};
std::atomic<unsigned long> g_missing[M_N];
inline void missing(Missing m) { g_missing[m]++; }

inline uint32_t h32(HANDLE h) { return (uint32_t)(uintptr_t)h; }
inline bool is_w32(HANDLE h) { return w32::get(h32(h)) != nullptr; }

// ---- the port's own events ----------------------------------------------------------------------------------------------
// Windows' unnamed events: auto-reset (one waiter released, the event reset by it) or manual-reset (every waiter, set
// until reset). The port waits with short timeouts (2 ms polls) and never pulses; Kind::Other, so the game's
// PulseEvent / SetEvent stand-ins never take one for theirs.
struct Event : w32::Object {
    Event(bool manual_, bool initial) : w32::Object(w32::Kind::Other), manual(manual_), set(initial) {}
    std::mutex mu;
    std::condition_variable cv;
    const bool manual;
    bool set;
    uint32_t wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lk(mu);
        auto ready = [this] { return set; };
        if (ms == w32::INFINITE_) cv.wait(lk, ready);
        else if (!cv.wait_for(lk, std::chrono::milliseconds(ms), ready)) return w32::WAIT_TIMEOUT_;
        if (!manual) set = false;
        return w32::WAIT_OBJECT_0_;
    }
    void signal() {
        std::lock_guard<std::mutex> lk(mu);
        set = true;
        if (manual) cv.notify_all();
        else cv.notify_one();
    }
};

// ---- files: the OS's own open (Windows: the path as given; R2b: the path layer) -------------------------------------------
FILE* open_read(const char* path) {
#ifdef _WIN32
    return fopen(path, "rb");
#else
#error "R2b: open the Windows path through the path layer (w32_path.h: backslashes, any case, the install as the root)"
#endif
}

// ---- the ini reader: Windows' GetPrivateProfileString for a section and a key ------------------------------------------------
// What Windows does (measured, test/vp_os_test.cpp): the file is read as bytes (no BOM handling: a UTF-8 BOM makes
// the first line no section); lines end at CR, LF or CRLF; leading and trailing blanks (space, tab) of a line, of a
// section name inside [ ], of a key and of a value are dropped; text after the first ']' is ignored; a line starting
// with ';' is a comment; a line without '=' is no key; the key is everything before the first '=' (the value may hold
// '='); section and key compare case-insensitively (ASCII); the FIRST section of the name is the only one searched,
// and the first key in it wins; a value in matching quotes ("..." or '...') loses them; nothing else -- an inline
// "; comment" is part of the value.
inline bool blank(char c) { return c == ' ' || c == '\t'; }
std::string trim(const char* b, const char* e) {
    while (b < e && blank(*b)) b++;
    while (e > b && blank(e[-1])) e--;
    return std::string(b, e);
}
bool same(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

// the value of [section] key in file: true if the key was found
bool ini_lookup(const char* section, const char* key, const char* file, std::string& value) {
    FILE* f = open_read(file);
    if (!f) return false;
    std::vector<char> text;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) text.insert(text.end(), buf, buf + got);
    fclose(f);
    const std::string want_sec = trim(section, section + strlen(section));
    const std::string want_key = trim(key, key + strlen(key));
    int state = 0;                                   // 0: before the section, 1: in it, 2: past it (only the first counts)
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end && state < 2) {
        const char* ls = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        const char* le = p;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n' && (p == ls || p[-1] != '\n')) p++;
        while (ls < le && blank(*ls)) ls++;
        if (ls == le || *ls == ';') continue;
        if (*ls == '[') {
            const char* close = (const char*)memchr(ls + 1, ']', (size_t)(le - ls - 1));
            const std::string name = trim(ls + 1, close ? close : le);
            if (state == 1) state = 2;
            else if (same(name, want_sec)) state = 1;
            continue;
        }
        if (state != 1) continue;
        const char* eq = (const char*)memchr(ls, '=', (size_t)(le - ls));
        if (!eq) continue;
        if (!same(trim(ls, eq), want_key)) continue;
        value = trim(eq + 1, le);
        if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'') && value.back() == value[0])
            value = value.substr(1, value.size() - 2);
        return true;
    }
    return false;
}

// Windows' text-to-int for GetPrivateProfileIntA (RtlCharToInteger, base 0): blanks, a sign, then 0x / 0o / 0b (lower
// case only) for base 16 / 8 / 2, else base 10; digits up to the first that isn't one; 32-bit wrap-around
uint32_t profile_atoi(const char* s) {
    while (*s && (unsigned char)*s <= ' ') s++;
    bool neg = false;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    uint32_t base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'o' || s[1] == 'b')) {
        base = s[1] == 'x' ? 16 : s[1] == 'o' ? 8 : 2;
        s += 2;
    }
    uint32_t v = 0;
    for (;; s++) {
        uint32_t d;
        const char c = *s;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    return neg ? 0u - v : v;
}

}  // namespace

namespace vpos {

// ---- the S kind: through the stand-ins ------------------------------------------------------------------------------------
unsigned long thread_id() {
#ifdef VP_GCC
    if (HAVE(w32_GetCurrentThreadId)) return w32_GetCurrentThreadId();
#endif
    missing(M_THREAD_ID);
#ifdef _WIN32
    return ::GetCurrentThreadId();
#endif
}

// the game's last error: the stand-ins' (w32_handle.h) once GetLastError is one. (Called qualified, vpos::set_last_error,
// everywhere here: a w32::ERR_ argument would otherwise find w32::set_last_error by argument-dependent lookup.)
DWORD last_error() {
    if (HAVE(w32_GetLastError)) return w32::last_error();
    missing(M_LAST_ERROR);
#ifdef _WIN32
    return ::GetLastError();
#endif
}
void set_last_error(DWORD e) {
    if (HAVE(w32_GetLastError)) return w32::set_last_error(e);
#ifdef _WIN32
    ::SetLastError(e);
#endif
}

DWORD suspend_thread(HANDLE h) {
#ifdef VP_GCC
    if (is_w32(h) && HAVE(w32_SuspendThread)) return w32_SuspendThread(h32(h));
#endif
    missing(M_SUSPEND);
#ifdef _WIN32
    return ::SuspendThread(h);
#endif
}
DWORD resume_thread(HANDLE h) {
#ifdef VP_GCC
    if (is_w32(h) && HAVE(w32_ResumeThread)) return w32_ResumeThread(h32(h));
#endif
    missing(M_RESUME);
#ifdef _WIN32
    return ::ResumeThread(h);
#endif
}

// a handle of the stand-ins' (the game's, or the port's own events): the handle table's wait, as WaitForSingleObject's
// stand-in dispatches; an unknown one (closed, or Windows') the stand-in's failure when there is one, else Windows'
DWORD wait(HANDLE h, DWORD ms) {
    if (std::shared_ptr<w32::Object> o = w32::get(h32(h))) return o->wait(ms);
#ifdef VP_GCC
    if (HAVE(w32_WaitForSingleObject)) return w32_WaitForSingleObject(h32(h), ms);
#endif
#ifdef _WIN32
    return ::WaitForSingleObject(h, ms);
#endif
}
BOOL close_handle(HANDLE h) {
    if (is_w32(h)) {
#ifdef VP_GCC
        if (HAVE(w32_CloseHandle)) return w32_CloseHandle(h32(h)) ? TRUE : FALSE;
#endif
        return w32::close(h32(h)) ? TRUE : FALSE;    // (the port's own events, before A's CloseHandle: the same)
    }
#ifdef VP_GCC
    if (HAVE(w32_CloseHandle)) return w32_CloseHandle(h32(h));   // (closed already: the stand-in's failure)
#endif
    missing(M_CLOSE);
#ifdef _WIN32
    return ::CloseHandle(h);
#endif
}

void sleep(DWORD ms) {
#ifdef VP_GCC
    if (HAVE(w32_Sleep)) return w32_Sleep(ms);
#endif
    missing(M_SLEEP);
#ifdef _WIN32
    ::Sleep(ms);
#endif
}
void exit_process(UINT code) {
#ifdef VP_GCC
    if (HAVE(w32_ExitProcess)) w32_ExitProcess(code);
#endif
    missing(M_EXIT);
#ifdef _WIN32
    ::ExitProcess(code);
#endif
}

// the port's own events: unnamed (the port never names one; a name is refused, as a failed CreateEventA)
HANDLE create_event(BOOL manual, BOOL initial, const char* name) {
    if (name) {
        vpos::set_last_error(w32::ERR_NOT_SUPPORTED);
        return 0;
    }
    const w32::Handle h = w32::add(std::make_shared<Event>(manual != 0, initial != 0));
    if (!h) vpos::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
    return (HANDLE)(uintptr_t)h;
}
BOOL set_event(HANDLE h) {
    std::shared_ptr<w32::Object> o = w32::get(h32(h));
    if (Event* e = o ? dynamic_cast<Event*>(o.get()) : nullptr) {
        e->signal();
        return TRUE;
    }
    if (o) {                                         // a handle of the stand-ins', not one of these events
        vpos::set_last_error(w32::ERR_INVALID_HANDLE);
        return FALSE;
    }
#ifdef _WIN32
    return ::SetEvent(h);
#endif
}

// the port's locks: the game's critical sections (B's stand-ins take any CRITICAL_SECTION the caller owns)
void cs_init(CRITICAL_SECTION* cs) {
#ifdef VP_GCC
    if (HAVE(w32_InitializeCriticalSection)) return w32_InitializeCriticalSection((W32CriticalSection*)cs);
#endif
    missing(M_CS);
#ifdef _WIN32
    ::InitializeCriticalSection(cs);
#endif
}
void cs_enter(CRITICAL_SECTION* cs) {
#ifdef VP_GCC
    if (HAVE(w32_EnterCriticalSection)) return w32_EnterCriticalSection((W32CriticalSection*)cs);
#endif
#ifdef _WIN32
    ::EnterCriticalSection(cs);
#endif
}
void cs_leave(CRITICAL_SECTION* cs) {
#ifdef VP_GCC
    if (HAVE(w32_LeaveCriticalSection)) return w32_LeaveCriticalSection((W32CriticalSection*)cs);
#endif
#ifdef _WIN32
    ::LeaveCriticalSection(cs);
#endif
}

BOOL create_directory(const char* path) {
#ifdef VP_GCC
    if (HAVE(w32_CreateDirectoryA)) return w32_CreateDirectoryA(path, 0);
#endif
    missing(M_CREATE_DIR);
#ifdef _WIN32
    return ::CreateDirectoryA(path, 0);
#endif
}
BOOL copy_file(const char* from, const char* to, BOOL fail_if_exists) {
#ifdef VP_GCC
    if (HAVE(w32_CopyFileA)) return w32_CopyFileA(from, to, fail_if_exists);
#endif
    missing(M_COPY);
#ifdef _WIN32
    return ::CopyFileA(from, to, fail_if_exists);
#endif
}
HANDLE find_first(const char* pattern, WIN32_FIND_DATAA* fd) {
#ifdef VP_GCC
    if (HAVE(w32_FindFirstFileA)) return (HANDLE)(uintptr_t)w32_FindFirstFileA(pattern, (w32::FindDataA*)fd);
#endif
    missing(M_FIND);
#ifdef _WIN32
    return ::FindFirstFileA(pattern, fd);
#endif
}
BOOL find_next(HANDLE h, WIN32_FIND_DATAA* fd) {
#ifdef VP_GCC
    if (HAVE(w32_FindNextFileA)) return w32_FindNextFileA(h32(h), (w32::FindDataA*)fd);
#endif
#ifdef _WIN32
    return ::FindNextFileA(h, fd);
#endif
}
BOOL find_close(HANDLE h) {
#ifdef VP_GCC
    if (HAVE(w32_FindClose)) return w32_FindClose(h32(h));
#endif
#ifdef _WIN32
    return ::FindClose(h);
#endif
}

// GetFileAttributesA for what the port asks (does it exist, is it a folder): the port's own, portable. A folder is
// FILE_ATTRIBUTE_DIRECTORY, a file FILE_ATTRIBUTE_ARCHIVE (| READONLY when it can't be written); a trailing '\' or '/'
// is a folder's, as Windows takes it; missing: INVALID_FILE_ATTRIBUTES with ERROR_FILE_NOT_FOUND, or
// ERROR_PATH_NOT_FOUND when its folder is missing too.
DWORD file_attributes(const char* path) {
    if (!path || !*path) {
        vpos::set_last_error(w32::ERR_PATH_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    std::string p(path);
    bool want_dir = false;
    while (p.size() > 1 && (p.back() == '\\' || p.back() == '/') && !(p.size() == 3 && p[1] == ':')) {
        p.pop_back();
        want_dir = true;
    }
#ifdef _WIN32
    struct _stat st;
    const int r = _stat(p.c_str(), &st);
    const bool dir = r == 0 && (st.st_mode & _S_IFDIR);
    const bool ro = r == 0 && !(st.st_mode & _S_IWRITE);
#else
#error "R2b: stat() the path resolved through the path layer (w32_path.h)"
#endif
    if (r == 0 && (dir || !want_dir)) {
        return dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE | (ro ? FILE_ATTRIBUTE_READONLY : 0);
    }
    if (r == 0) {                                    // "file\": Windows says ERROR_DIRECTORY
        vpos::set_last_error(267);
        return INVALID_FILE_ATTRIBUTES;
    }
    const size_t cut = p.find_last_of("\\/");
    bool parent = true;
    if (cut != std::string::npos && cut > 0 && !(cut == 2 && p[1] == ':')) {
        struct _stat ps;
        parent = _stat(p.substr(0, cut).c_str(), &ps) == 0 && (ps.st_mode & _S_IFDIR);
    }
    vpos::set_last_error(parent ? w32::ERR_FILE_NOT_FOUND : w32::ERR_PATH_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
}

void local_time(SYSTEMTIME* t) {
#ifdef VP_GCC
    if (HAVE(w32_GetLocalTime)) return w32_GetLocalTime((W32SystemTime*)t);
#endif
    missing(M_LOCAL_TIME);
#ifdef _WIN32
    ::GetLocalTime(t);
#endif
}

// GetPrivateProfileStringA (a section and a key, both given -- the port never enumerates): the value, or the default
// with its trailing blanks dropped; at most n - 1 characters and the terminator; the count copied
DWORD profile_string(const char* section, const char* key, const char* dflt, char* out, DWORD n, const char* file) {
    if (!out || !n) return 0;
    std::string v;
    if (!section || !key || !ini_lookup(section, key, file, v)) {
        const char* d = dflt ? dflt : "";
        size_t m = strlen(d);
        while (m && d[m - 1] == ' ') m--;
        v.assign(d, m);
    }
    const DWORD c = v.size() < n ? (DWORD)v.size() : n - 1;
    memcpy(out, v.data(), c);
    out[c] = 0;
    return c;
}
// GetPrivateProfileIntA: the default when the key is missing or its value empty, else the value as Windows reads it
UINT profile_int(const char* section, const char* key, INT dflt, const char* file) {
    std::string v;
    if (!section || !key || !ini_lookup(section, key, file, v) || v.empty()) return (UINT)dflt;
    return profile_atoi(v.c_str());
}

// UTF-8 -> UTF-16 as Windows' MultiByteToWideChar(CP_UTF8, 0, ...) does: an invalid or truncated sequence is one
// U+FFFD, past U+FFFF a surrogate pair, n = -1 takes the terminator too (counted), a too-small buffer fails with
// ERROR_INSUFFICIENT_BUFFER. Only the port's own typed text (platform.cpp) is UTF-8; the game's stand-in has the game's
// code pages.
static int utf8_to_wide(const char* s, int n, WCHAR* w, int wn) {
    const unsigned char* p = (const unsigned char*)s;
    const size_t len = n < 0 ? strlen(s) + 1 : (size_t)n;
    int out = 0;
    auto put = [&](unsigned u) -> bool {
        if (wn) {
            if (out >= wn) return false;
            w[out] = (WCHAR)u;
        }
        out++;
        return true;
    };
    for (size_t i = 0; i < len;) {
        unsigned c = p[i], cp, need;
        if (c < 0x80) cp = c, need = 0;
        else if (c >= 0xC2 && c <= 0xDF) cp = c & 0x1F, need = 1;
        else if (c >= 0xE0 && c <= 0xEF) cp = c & 0x0F, need = 2;
        else if (c >= 0xF0 && c <= 0xF4) cp = c & 0x07, need = 3;
        else { if (!put(0xFFFD)) goto too_small; i++; continue; }
        size_t j = 1;
        for (; j <= need && i + j < len && (p[i + j] & 0xC0) == 0x80; j++) cp = cp << 6 | (p[i + j] & 0x3F);
        if (j <= need || (need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (need == 3 && (cp < 0x10000 || cp > 0x10FFFF))) {
            if (!put(0xFFFD)) goto too_small;
            i += j > 1 ? j : 1;
            continue;
        }
        i += need + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            if (!put(0xD800 | cp >> 10) || !put(0xDC00 | (cp & 0x3FF))) goto too_small;
        } else if (!put(cp)) {
            goto too_small;
        }
    }
    return out;
too_small:
    vpos_SetLastError(122);                                          // ERROR_INSUFFICIENT_BUFFER
    return 0;
}

int mb_to_wide(UINT cp, DWORD flags, const char* s, int n, WCHAR* w, int wn) {
    if (cp == 65001 && flags == 0) return utf8_to_wide(s, n, w, wn);   // CP_UTF8 (the port's typed text)
#ifdef VP_GCC
    if (HAVE(w32_MultiByteToWideChar)) return w32_MultiByteToWideChar(cp, flags, s, n, (uint16_t*)w, wn);
#endif
    missing(M_MB);
#ifdef _WIN32
    return ::MultiByteToWideChar(cp, flags, s, n, w, wn);
#endif
}
int wide_to_mb(UINT cp, DWORD flags, const WCHAR* w, int wn, char* s, int n, const char* dflt, BOOL* used) {
#ifdef VP_GCC
    if (HAVE(w32_WideCharToMultiByte))
        return w32_WideCharToMultiByte(cp, flags, (const uint16_t*)w, wn, s, n, dflt, (int32_t*)used);
#endif
    missing(M_WC);
#ifdef _WIN32
    return ::WideCharToMultiByte(cp, flags, w, wn, s, n, dflt, used);
#endif
}

int system_metrics(int i) {
#ifdef VP_GCC
    if (HAVE(w32_GetSystemMetrics)) return w32_GetSystemMetrics(i);
#endif
    missing(M_METRICS);
#ifdef _WIN32
    return ::GetSystemMetrics(i);
#endif
}
HWND foreground_window() {
#ifdef VP_GCC
    if (HAVE(w32_GetForegroundWindow)) return (HWND)(uintptr_t)w32_GetForegroundWindow();
#endif
    missing(M_FOREGROUND);
#ifdef _WIN32
    return ::GetForegroundWindow();
#endif
}
BOOL clip_cursor(const RECT* r) {
#ifdef VP_GCC
    if (HAVE(w32_ClipCursor)) return w32_ClipCursor(r);
#endif
    missing(M_CLIP);
#ifdef _WIN32
    return ::ClipCursor(r);
#endif
}
HICON load_icon(HINSTANCE inst, const char* name) {
#ifdef VP_GCC
    if (HAVE(w32_LoadIconA)) return (HICON)(uintptr_t)w32_LoadIconA((uint32_t)(uintptr_t)inst, name);
#endif
    missing(M_ICON);
#ifdef _WIN32
    return ::LoadIconA(inst, name);
#endif
}

}  // namespace vpos

void vp_os_init(bool stand_ins) {
    vp_os_on = stand_ins;
}

void vp_os_report() {
    if (!vp_os_on) return;
    char line[600];
    size_t w = 0;
    line[0] = 0;
    for (int i = 0; i < M_N; i++) {
        const unsigned long c = g_missing[i];
        if (!c) continue;
        const int k = snprintf(line + w, sizeof line - w, "%s%s %lu", w ? ", " : "", k_missing[i], c);
        if (k < 0 || (size_t)k >= sizeof line - w) break;
        w += (size_t)k;
    }
    if (w) logf("exit: vp_os: the port's calls that reached Windows for want of a stand-in: %s", line);
    else logf("exit: vp_os: every port call of the stand-in kind went through the stand-ins");
}
