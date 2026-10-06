// linux_todo.cpp -- the native Linux build's placeholders (relink stage R2b, agent A; linux_todo.h). Every function
// below is one agent B, C or D of R2b will provide for real; until then it prints its name and aborts, so the port
// links (tools/build_linux.sh's out-linux/viperport-link-test) and nothing silently does the wrong thing. Each owner
// deletes their stubs as they write the real thing (or moves the call site onto vp_os.h); the file ends empty.
// Besides these, vp_r2b_todo("...") calls sit inline where an #error used to mark an R2b gap (platform.cpp,
// viperport.cpp, vp_os.cpp), and w32_user.cpp builds its no-op loading-window stand-ins (-DVP_R2B_STUBS): see the
// report of agent A.
#ifndef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>                                           // hook/linux_inc -> win32_compat.h
#include "vp_os.h"
#include "w32_kernel.h"

void vp_r2b_todo(const char* what) {
    fprintf(stderr, "R2b TODO: %s -- not written yet on Linux\n", what);
    fflush(stderr);
    abort();
}

#define TODO(owner, name) vp_r2b_todo(name " (agent " owner ")")

// ---- vp_os.h's OS kind (the #else of "O: the operating system's"): agent B, vp_os.cpp ------------------------------------------
DWORD vpos_GetTickCount() { TODO("B", "vpos_GetTickCount: clock_gettime(CLOCK_MONOTONIC) in ms"); }
BOOL vpos_VirtualProtect(LPVOID, SIZE_T, DWORD, PDWORD) { TODO("B", "vpos_VirtualProtect: mprotect + the old protection"); }
SIZE_T vpos_VirtualQuery(LPCVOID, MEMORY_BASIC_INFORMATION*, SIZE_T) { TODO("B", "vpos_VirtualQuery: /proc/self/maps or a region table"); }
LPVOID vpos_VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD) { TODO("B", "vpos_VirtualAlloc: mmap"); }
BOOL vpos_IsBadReadPtr(const void*, UINT_PTR) { TODO("B", "vpos_IsBadReadPtr: a guarded read"); }
HMODULE vpos_GetModuleHandleA(LPCSTR) { TODO("B", "vpos_GetModuleHandleA: race.exe's base (0x400000)"); }
DWORD vpos_GetModuleFileNameA(HMODULE, LPSTR, DWORD) { TODO("B", "vpos_GetModuleFileNameA: /proc/self/exe in Windows form"); }
// agent C (w32_seh.cpp: signals -> EXCEPTION_POINTERS -> the vectored handlers)
PVOID vpos_AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER) { TODO("C", "vpos_AddVectoredExceptionHandler"); }

// ---- w32_seh.cpp's four stand-ins (w32_kernel.h): agent C -------------------------------------------------------------------
void* W32K_CALL w32_SetUnhandledExceptionFilter(void*) { TODO("C", "w32_SetUnhandledExceptionFilter"); }
int32_t W32K_CALL w32_UnhandledExceptionFilter(void*) { TODO("C", "w32_UnhandledExceptionFilter"); }
void W32K_CALL w32_RaiseException(uint32_t, uint32_t, uint32_t, const uint32_t*) { TODO("C", "w32_RaiseException"); }
void W32K_CALL w32_RtlUnwind(void*, void*, void*, void*) { TODO("C", "w32_RtlUnwind"); }

// ---- Windows functions the port still calls by name on Linux ---------------------------------------------------------------
// standalone.cpp (the int3 fill, its audit and self-test, the int3 handler, the game's start) and viperport.cpp
// (DllMain's work, the environment) call these directly; on Linux they become vpos_ calls (vp_os.h) or Linux code in
// the loader's integration -- agent B, except the exception ones (agent C).
extern "C" {
BOOL WINAPI VirtualProtect(LPVOID, SIZE_T, DWORD, PDWORD) { TODO("B", "VirtualProtect (standalone.cpp)"); }
SIZE_T WINAPI VirtualQuery(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T) { TODO("B", "VirtualQuery (standalone.cpp)"); }
BOOL WINAPI FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { TODO("B", "FlushInstructionCache (standalone.cpp)"); }
HANDLE WINAPI GetCurrentProcess(void) { TODO("B", "GetCurrentProcess (standalone.cpp)"); }
BOOL WINAPI GetProcessDEPPolicy(HANDLE, LPDWORD, PBOOL) { TODO("B", "GetProcessDEPPolicy (standalone.cpp)"); }
BOOL WINAPI IsBadReadPtr(const VOID*, UINT_PTR) { TODO("B", "IsBadReadPtr (standalone.cpp)"); }
HMODULE WINAPI GetModuleHandleA(LPCSTR) { TODO("B", "GetModuleHandleA (standalone.cpp)"); }
BOOL WINAPI GetModuleHandleExA(DWORD, LPCSTR, HMODULE*) { TODO("B", "GetModuleHandleExA (standalone.cpp)"); }
DWORD WINAPI GetModuleFileNameA(HMODULE, LPSTR, DWORD) { TODO("B", "GetModuleFileNameA (standalone.cpp)"); }
DWORD WINAPI GetLastError(void) { TODO("B", "GetLastError (standalone.cpp)"); }
DWORD WINAPI GetCurrentThreadId(void) { TODO("B", "GetCurrentThreadId (standalone.cpp)"); }
VOID WINAPI Sleep(DWORD) { TODO("B", "Sleep (standalone.cpp)"); }
VOID WINAPI ExitProcess(UINT) { TODO("B", "ExitProcess (standalone.cpp)"); }
LPSTR WINAPI lstrcatA(LPSTR, LPCSTR) { TODO("B", "lstrcatA (standalone.cpp)"); }
DWORD WINAPI GetEnvironmentVariableA(LPCSTR, LPSTR, DWORD) { TODO("B", "GetEnvironmentVariableA (standalone.cpp, viperport.cpp)"); }
BOOL WINAPI SetEnvironmentVariableA(LPCSTR, LPCSTR) { TODO("B", "SetEnvironmentVariableA (standalone.cpp)"); }
BOOL WINAPI DisableThreadLibraryCalls(HMODULE) { TODO("B", "DisableThreadLibraryCalls (viperport.cpp's DllMain: Windows-only)"); }
PVOID WINAPI AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER) { TODO("C", "AddVectoredExceptionHandler (standalone.cpp)"); }
VOID WINAPI RaiseException(DWORD, DWORD, DWORD, const ULONG_PTR*) { TODO("C", "RaiseException (standalone.cpp's SEH self-test)"); }
}
#endif  // !_WIN32
