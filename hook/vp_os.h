// vp_os.h -- the port's own calls to the operating system (relink stage R2a.1: on the way to a native Linux build).
//
// The port (session.cpp, viperport.cpp, platform.cpp, port.cpp, replay.cpp, net_wsock.cpp, gl_*, ddraw_gl.cpp,
// dsound_sdl.cpp, crt_start.cpp, krn_util.cpp) calls Windows for itself: its ini file, its folders and copies, the
// thread it's on, its own events and locks, patching race.exe's code, its log's clock. Each such call is written
// vpos_<Win32 name>(...) with Windows' own arguments and results (vpos_, not vp_: fix_paths.h's vp_ names are the
// game's import slots), and goes:
//   * MSVC (the shipped build): straight to Windows -- every vpos_ name below is a #define of the Win32 name, so the
//     MSVC build compiles exactly the calls it always made.
//   * GCC: through this header. Two kinds:
//       - the stand-in kind (marked S): with the stand-ins active -- the GCC standalone, viperport.exe, where race.exe's
//         import table holds the port's own Win32 functions (hook/w32_*.cpp) -- the port's call goes where the game's
//         goes: the same thread ids, handles, last error, files, clocks and window as the game sees (session.cpp
//         compares the game's thread ids and suspends the game's threads, hands the game a find handle and its last
//         error; platform.cpp hands the game its foreground window). A stand-in another agent hasn't written yet is
//         a weak reference (null): Windows' own is called, as the game's slot then still is (the loader keeps the real
//         import for any slot without a stand-in). The port's own objects of the kind (its events, its ini reads,
//         GetFileAttributesA, which the game doesn't import) are implemented here, in vp_os.cpp, portably.
//         On the dinput.dll route (race.exe's own imports: real Windows) and in a harness, Windows' own.
//       - the OS kind (marked O): what only the operating system can do -- page protection, code flushes, the module's
//         path, the vectored handler, the tick count. Windows' own on Windows (both routes); R2b fills the #else.
//     and the plain C kind (lstrcpyA, wsprintfA, ...): written in portable C.
// Windows-only features the port has (gl_dxgi.cpp's DXGI present, the two_copies named mutexes, the SDL window's
// subclassing and icon, the real dinput forward, VirtualStore, timeBeginPeriod) stay Win32 inside #ifdef _WIN32 at
// their call sites, compiled out of a Linux build.
//
// No Windows type leaks into the stand-in side: handles are the stand-ins' 32-bit values (w32_handle.h), cast at the
// boundary. Harnesses (test/world_*.cpp) include some of these files: everything the GCC side needs from vp_os.cpp or
// a stand-in is a weak reference, so a harness links without them and, with the stand-ins never active, calls Windows.
//
// VP_OS_LITE (krn_util.cpp): only vpos_GetCurrentThreadId, without <windows.h>.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "compiler.h"
// (weak only where it is used: vp_os.cpp, VP_OS_IMPL, defines them -- a weak definition in a PE object collides)
#if defined(VP_GCC) && !defined(VP_OS_IMPL)
#define VP_OS_WEAK __attribute__((weak))
#else
#define VP_OS_WEAK
#endif

// ---- vp_os.cpp (both compilers compile it; only the GCC build calls it, through the wrappers below) -------------------
// set by vp_os_init (viperport.cpp's DllMain, from vp_standalone()): the stand-ins are race.exe's imports
extern bool vp_os_on VP_OS_WEAK;
namespace vpos {
unsigned long thread_id() VP_OS_WEAK;
}
#ifndef VP_OS_LITE
#ifdef _WIN32
#include <windows.h>
#else
#include <windows.h>     // R2b: the Linux build's is hook/linux_inc/windows.h -> win32_compat.h (Windows' types and layouts)
#endif
// the stand-ins are race.exe's imports (vp_standalone(), GCC): the S kind goes through them from here (viperport.cpp)
void vp_os_init(bool stand_ins) VP_OS_WEAK;
// at exit (the log): the S kind's calls that went to Windows because a stand-in wasn't there
void vp_os_report() VP_OS_WEAK;
namespace vpos {
DWORD last_error() VP_OS_WEAK;
void set_last_error(DWORD e) VP_OS_WEAK;
DWORD suspend_thread(HANDLE h) VP_OS_WEAK;
DWORD resume_thread(HANDLE h) VP_OS_WEAK;
DWORD wait(HANDLE h, DWORD ms) VP_OS_WEAK;
BOOL close_handle(HANDLE h) VP_OS_WEAK;
void sleep(DWORD ms) VP_OS_WEAK;
void exit_process(UINT code) VP_OS_WEAK;
HANDLE create_event(BOOL manual, BOOL initial, const char* name) VP_OS_WEAK;
BOOL set_event(HANDLE h) VP_OS_WEAK;
void cs_init(CRITICAL_SECTION* cs) VP_OS_WEAK;
void cs_enter(CRITICAL_SECTION* cs) VP_OS_WEAK;
void cs_leave(CRITICAL_SECTION* cs) VP_OS_WEAK;
BOOL create_directory(const char* path) VP_OS_WEAK;
BOOL copy_file(const char* from, const char* to, BOOL fail_if_exists) VP_OS_WEAK;
HANDLE find_first(const char* pattern, WIN32_FIND_DATAA* fd) VP_OS_WEAK;
BOOL find_next(HANDLE h, WIN32_FIND_DATAA* fd) VP_OS_WEAK;
BOOL find_close(HANDLE h) VP_OS_WEAK;
DWORD file_attributes(const char* path) VP_OS_WEAK;
void local_time(SYSTEMTIME* t) VP_OS_WEAK;
UINT profile_int(const char* section, const char* key, INT dflt, const char* file) VP_OS_WEAK;
DWORD profile_string(const char* section, const char* key, const char* dflt, char* out, DWORD n, const char* file)
    VP_OS_WEAK;
int mb_to_wide(UINT cp, DWORD flags, const char* s, int n, WCHAR* w, int wn) VP_OS_WEAK;
int wide_to_mb(UINT cp, DWORD flags, const WCHAR* w, int wn, char* s, int n, const char* dflt, BOOL* used)
    VP_OS_WEAK;
int system_metrics(int i) VP_OS_WEAK;
HWND foreground_window() VP_OS_WEAK;
BOOL clip_cursor(const RECT* r) VP_OS_WEAK;
HICON load_icon(HINSTANCE inst, const char* name) VP_OS_WEAK;
}  // namespace vpos
#endif  // VP_OS_LITE

#ifndef VP_GCC
// ---- MSVC: Windows itself --------------------------------------------------------------------------------------------
#define vpos_GetCurrentThreadId GetCurrentThreadId
#ifndef VP_OS_LITE
#define vpos_GetLastError GetLastError
#define vpos_SetLastError SetLastError
#define vpos_SuspendThread SuspendThread
#define vpos_ResumeThread ResumeThread
#define vpos_WaitForSingleObject WaitForSingleObject
#define vpos_CloseHandle CloseHandle
#define vpos_Sleep Sleep
#define vpos_ExitProcess ExitProcess
#define vpos_CreateEventA CreateEventA
#define vpos_SetEvent SetEvent
#define vpos_InitializeCriticalSection InitializeCriticalSection
#define vpos_EnterCriticalSection EnterCriticalSection
#define vpos_LeaveCriticalSection LeaveCriticalSection
#define vpos_CreateDirectoryA CreateDirectoryA
#define vpos_CopyFileA CopyFileA
#define vpos_FindFirstFileA FindFirstFileA
#define vpos_FindNextFileA FindNextFileA
#define vpos_FindClose FindClose
#define vpos_GetFileAttributesA GetFileAttributesA
#define vpos_GetLocalTime GetLocalTime
#define vpos_GetPrivateProfileIntA GetPrivateProfileIntA
#define vpos_GetPrivateProfileStringA GetPrivateProfileStringA
#define vpos_MultiByteToWideChar MultiByteToWideChar
#define vpos_WideCharToMultiByte WideCharToMultiByte
#define vpos_GetSystemMetrics GetSystemMetrics
#define vpos_GetForegroundWindow GetForegroundWindow
#define vpos_ClipCursor ClipCursor
#define vpos_LoadIconA LoadIconA
#define vpos_GetTickCount GetTickCount
#define vpos_VirtualProtect VirtualProtect
#define vpos_VirtualQuery VirtualQuery
#define vpos_VirtualAlloc VirtualAlloc
#define vpos_flush_code(p, n) FlushInstructionCache(GetCurrentProcess(), (p), (n))
#define vpos_IsBadReadPtr IsBadReadPtr
#define vpos_AddVectoredExceptionHandler AddVectoredExceptionHandler
#define vpos_GetModuleHandleA GetModuleHandleA
#define vpos_GetModuleFileNameA GetModuleFileNameA
#define vpos_lstrcpyA lstrcpyA
#define vpos_lstrcatA lstrcatA
#define vpos_lstrcpynA lstrcpynA
#define vpos_wsprintfA wsprintfA
#endif  // VP_OS_LITE

#else
// ---- GCC ----------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
#define VP_OS_WIN(...) __VA_ARGS__;
#else
#define VP_OS_WIN(...) __builtin_unreachable();      // R2b: no Windows -- the stand-in kind is always active
#endif

#if defined(VP_OS_IMPL)
inline bool vp_os_si() { return vp_os_on; }
#elif defined(_WIN32)
inline bool vp_os_si() { return &vp_os_on != nullptr && vp_os_on; }
#else
inline bool vp_os_si() { return true; }
#endif

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif
// S: the game's thread ids (session.cpp compares them with the ids the game's tasks recorded)
inline unsigned long vpos_GetCurrentThreadId() {
    if (vp_os_si()) return vpos::thread_id();
    VP_OS_WIN(return ::GetCurrentThreadId())
}

#ifndef VP_OS_LITE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// S: the last error the game reads (session.cpp's directory-scan hooks hand the game theirs)
inline DWORD vpos_GetLastError() {
    if (vp_os_si()) return vpos::last_error();
    VP_OS_WIN(return ::GetLastError())
}
inline void vpos_SetLastError(DWORD e) {
    if (vp_os_si()) return vpos::set_last_error(e);
    VP_OS_WIN(::SetLastError(e))
}
// S: the game's threads (session.cpp: is the lobby task really suspended)
inline DWORD vpos_SuspendThread(HANDLE h) {
    if (vp_os_si()) return vpos::suspend_thread(h);
    VP_OS_WIN(return ::SuspendThread(h))
}
inline DWORD vpos_ResumeThread(HANDLE h) {
    if (vp_os_si()) return vpos::resume_thread(h);
    VP_OS_WIN(return ::ResumeThread(h))
}
// S: any handle -- the game's (a task's thread) or the port's own events
inline DWORD vpos_WaitForSingleObject(HANDLE h, DWORD ms) {
    if (vp_os_si()) return vpos::wait(h, ms);
    VP_OS_WIN(return ::WaitForSingleObject(h, ms))
}
inline BOOL vpos_CloseHandle(HANDLE h) {
    if (vp_os_si()) return vpos::close_handle(h);
    VP_OS_WIN(return ::CloseHandle(h))
}
inline void vpos_Sleep(DWORD ms) {
    if (vp_os_si()) return vpos::sleep(ms);
    VP_OS_WIN(::Sleep(ms))
}
[[noreturn]] inline void vpos_ExitProcess(UINT code) {
    if (vp_os_si()) vpos::exit_process(code);
    VP_OS_WIN(::ExitProcess(code))
    __builtin_unreachable();
}
// S: the port's own events (unnamed; vp_os.cpp: an object in the stand-ins' handle table, so vpos_WaitForSingleObject
// and vpos_CloseHandle take it as they take the game's)
inline HANDLE vpos_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name) {
    if (vp_os_si()) return vpos::create_event(manual, initial, name);
    VP_OS_WIN(return ::CreateEventA(sa, manual, initial, name))
}
inline BOOL vpos_SetEvent(HANDLE h) {
    if (vp_os_si()) return vpos::set_event(h);
    VP_OS_WIN(return ::SetEvent(h))
}
// S: the port's own locks, as the game's
inline void vpos_InitializeCriticalSection(CRITICAL_SECTION* cs) {
    if (vp_os_si()) return vpos::cs_init(cs);
    VP_OS_WIN(::InitializeCriticalSection(cs))
}
inline void vpos_EnterCriticalSection(CRITICAL_SECTION* cs) {
    if (vp_os_si()) return vpos::cs_enter(cs);
    VP_OS_WIN(::EnterCriticalSection(cs))
}
inline void vpos_LeaveCriticalSection(CRITICAL_SECTION* cs) {
    if (vp_os_si()) return vpos::cs_leave(cs);
    VP_OS_WIN(::LeaveCriticalSection(cs))
}
// S: files and folders (the session's run folders and its snapshot of the user folder, the race recorder's, the
// renderer's captures, copy 2's log folder)
inline BOOL vpos_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa) {
    if (vp_os_si()) return vpos::create_directory(path);
    VP_OS_WIN(return ::CreateDirectoryA(path, sa))
}
inline BOOL vpos_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    if (vp_os_si()) return vpos::copy_file(from, to, fail_if_exists);
    VP_OS_WIN(return ::CopyFileA(from, to, fail_if_exists))
}
inline HANDLE vpos_FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* fd) {
    if (vp_os_si()) return vpos::find_first(pattern, fd);
    VP_OS_WIN(return ::FindFirstFileA(pattern, fd))
}
inline BOOL vpos_FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd) {
    if (vp_os_si()) return vpos::find_next(h, fd);
    VP_OS_WIN(return ::FindNextFileA(h, fd))
}
inline BOOL vpos_FindClose(HANDLE h) {
    if (vp_os_si()) return vpos::find_close(h);
    VP_OS_WIN(return ::FindClose(h))
}
// S (the port's own: the game doesn't import it): is this a folder / does it exist
inline DWORD vpos_GetFileAttributesA(LPCSTR path) {
    if (vp_os_si()) return vpos::file_attributes(path);
    VP_OS_WIN(return ::GetFileAttributesA(path))
}
// S: the date and time (the log's clock, the run folders' names)
inline void vpos_GetLocalTime(SYSTEMTIME* t) {
    if (vp_os_si()) return vpos::local_time(t);
    VP_OS_WIN(::GetLocalTime(t))
}
// S (the port's own: vp_os.cpp reads the ini as Windows does): viperport.ini
inline UINT vpos_GetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT dflt, LPCSTR file) {
    if (vp_os_si()) return vpos::profile_int(section, key, dflt, file);
    VP_OS_WIN(return ::GetPrivateProfileIntA(section, key, dflt, file))
}
inline DWORD vpos_GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR dflt, LPSTR out, DWORD n, LPCSTR file) {
    if (vp_os_si()) return vpos::profile_string(section, key, dflt, out, n, file);
    VP_OS_WIN(return ::GetPrivateProfileStringA(section, key, dflt, out, n, file))
}
// S: code pages (platform.cpp: typed text, UTF-8 -> the game's ANSI)
inline int vpos_MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR w, int wn) {
    if (vp_os_si()) return vpos::mb_to_wide(cp, flags, s, n, w, wn);
    VP_OS_WIN(return ::MultiByteToWideChar(cp, flags, s, n, w, wn))
}
inline int vpos_WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int wn, LPSTR s, int n, LPCSTR dflt, LPBOOL used) {
    if (vp_os_si()) return vpos::wide_to_mb(cp, flags, w, wn, s, n, dflt, used);
    VP_OS_WIN(return ::WideCharToMultiByte(cp, flags, w, wn, s, n, dflt, used))
}
// S: the window side the game shares (platform.cpp, in place of the game's own window code)
inline int vpos_GetSystemMetrics(int i) {
    if (vp_os_si()) return vpos::system_metrics(i);
    VP_OS_WIN(return ::GetSystemMetrics(i))
}
inline HWND vpos_GetForegroundWindow() {
    if (vp_os_si()) return vpos::foreground_window();
    VP_OS_WIN(return ::GetForegroundWindow())
}
inline BOOL vpos_ClipCursor(const RECT* r) {
    if (vp_os_si()) return vpos::clip_cursor(r);
    VP_OS_WIN(return ::ClipCursor(r))
}
inline HICON vpos_LoadIconA(HINSTANCE inst, LPCSTR name) {
    if (vp_os_si()) return vpos::load_icon(inst, name);
    VP_OS_WIN(return ::LoadIconA(inst, name))
}

// ---- O: the operating system's (Windows now; R2b: the #else) --------------------------------------------------------------
#ifdef _WIN32
inline DWORD vpos_GetTickCount() { return ::GetTickCount(); }
inline BOOL vpos_VirtualProtect(LPVOID p, SIZE_T n, DWORD prot, PDWORD old) { return ::VirtualProtect(p, n, prot, old); }
inline SIZE_T vpos_VirtualQuery(LPCVOID p, MEMORY_BASIC_INFORMATION* m, SIZE_T n) { return ::VirtualQuery(p, m, n); }
inline LPVOID vpos_VirtualAlloc(LPVOID p, SIZE_T n, DWORD type, DWORD prot) { return ::VirtualAlloc(p, n, type, prot); }
inline void vpos_flush_code(const void* p, SIZE_T n) { ::FlushInstructionCache(::GetCurrentProcess(), p, n); }
inline BOOL vpos_IsBadReadPtr(const void* p, UINT_PTR n) { return ::IsBadReadPtr(p, n); }
inline PVOID vpos_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) {
    return ::AddVectoredExceptionHandler(first, h);
}
// the port asks only for race.exe's image (0) and modules it loaded itself (gl_dxgi.cpp, gl_table.cpp: Windows-only)
inline HMODULE vpos_GetModuleHandleA(LPCSTR name) { return ::GetModuleHandleA(name); }
// 0: the process's executable (viperport.exe standalone, race.exe on the dinput.dll route); the DLL's own handle: its
// path (viperport.ini and viperport.log beside it)
inline DWORD vpos_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) { return ::GetModuleFileNameA(m, out, n); }
#else
// R2b: GetTickCount -- clock_gettime(CLOCK_MONOTONIC) in ms, as a DWORD that wraps; VirtualProtect -- mprotect on the
// pages covering [p, p+n) (PAGE_* -> PROT_*), the old protection from a table the port keeps (or /proc/self/maps);
// VirtualQuery -- the same table / /proc/self/maps for State (the ELF loader's mapping of race.exe is MEM_COMMIT);
// VirtualAlloc -- mmap(PROT_READ|WRITE|EXEC, MAP_PRIVATE|MAP_ANONYMOUS), below 4 GB (a 32-bit process: always);
// vpos_flush_code -- nothing (x86 keeps instruction and data caches coherent); IsBadReadPtr -- a guarded read
// (sigsetjmp in a SIGSEGV handler) or write(pipe) returning EFAULT; AddVectoredExceptionHandler -- sigaction for
// SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGTRAP building EXCEPTION_RECORD + CONTEXT from the ucontext (the same layout
// the standalone's int3 handler and the fault logger read: Eip, Esp, ExceptionInformation[0..1]);
// GetModuleHandleA(0) -- race.exe's base, 0x400000 (the ELF loader maps it there); GetModuleFileNameA -- the
// executable's path (/proc/self/exe) for both 0 and the port's own module (one ELF), in Windows form (backslashes)
// through the path layer (w32_path.h).
// The Linux definitions are in hook/vp_os_linux.cpp (vp_os_linux.h: the region table, the modules, the fopen wrapper);
// vpos_AddVectoredExceptionHandler is w32_seh.cpp's.
DWORD vpos_GetTickCount();
BOOL vpos_VirtualProtect(LPVOID p, SIZE_T n, DWORD prot, PDWORD old);
SIZE_T vpos_VirtualQuery(LPCVOID p, MEMORY_BASIC_INFORMATION* m, SIZE_T n);
LPVOID vpos_VirtualAlloc(LPVOID p, SIZE_T n, DWORD type, DWORD prot);
inline void vpos_flush_code(const void*, SIZE_T) {}      // x86: instruction fetch sees the stores
BOOL vpos_IsBadReadPtr(const void* p, UINT_PTR n);
PVOID vpos_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
HMODULE vpos_GetModuleHandleA(LPCSTR name);
DWORD vpos_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n);
#endif

// ---- plain C (Windows' semantics) ------------------------------------------------------------------------------------------
inline LPSTR vpos_lstrcpyA(LPSTR to, LPCSTR from) { return strcpy(to, from); }
inline LPSTR vpos_lstrcatA(LPSTR to, LPCSTR from) { return strcat(to, from); }
// at most n - 1 characters and the terminator (n <= 0: nothing); to
inline LPSTR vpos_lstrcpynA(LPSTR to, LPCSTR from, int n) {
    if (n <= 0) return to;
    int i = 0;
    for (; i < n - 1 && from[i]; i++) to[i] = from[i];
    to[i] = 0;
    return to;
}
// wsprintfA: the C formats the port uses (%08lx, %lu, %s, %d), at most 1024 bytes with the terminator, as Windows'
inline int vpos_wsprintfA(LPSTR out, LPCSTR fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, 1024, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : n > 1023 ? 1023 : n;
}
#endif  // VP_OS_LITE
#endif  // VP_GCC
