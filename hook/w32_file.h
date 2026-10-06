// w32_file.h -- the stand-ins for race.exe's KERNEL32 file, folder, listing, mapping, path and console imports
// (relink stage R2a). Each is w32_<Name>, __stdcall, with the Win32 signature in 32-bit types (a HANDLE, DWORD, BOOL or
// UINT is a uint32_t); hook/w32_table.cpp puts them in race.exe's import table (the GCC standalone only).
//
// Handles are w32_handle.h's: a file, a find-file listing, a mapping, a standard stream are objects in its table, so
// CloseHandle / DuplicateHandle / GetFileType work on any of them. The paths go through w32_path.h (Windows' full-path
// rules; on Linux, the install folder as drive C:). What each returns -- the value, the out parameters, GetLastError --
// is what Windows returns for the same call; test/w32_file_test.cpp runs both side by side.
//
// Not available (they fail as Windows does when the thing isn't there): CreateMailslotA (ERROR_NOT_SUPPORTED),
// DeviceIoControl (ERROR_INVALID_FUNCTION; ERROR_INVALID_HANDLE for a bad handle). The game reaches neither on NT.
// The console set runs the real console on Windows and a terminal (stdin / stdout) on Linux.
#pragma once
#include <stdint.h>
#include <memory>
#include "w32_handle.h"

#ifndef W32_STDCALL
#if defined(_MSC_VER) || defined(__MINGW32__)
#define W32_STDCALL __stdcall
#else
#define W32_STDCALL __attribute__((stdcall))
#endif
#endif

namespace w32 {

// WIN32_FIND_DATAA, byte for byte (320 bytes)
struct FindDataA {
    uint32_t dwFileAttributes;
    uint32_t ftCreationTime[2], ftLastAccessTime[2], ftLastWriteTime[2];   // FILETIME: low, high
    uint32_t nFileSizeHigh, nFileSizeLow;
    uint32_t dwReserved0, dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
    uint16_t pad_;                                                         // (the struct's tail padding)
};
static_assert(sizeof(FindDataA) == 320, "WIN32_FIND_DATAA is 320 bytes");

// OVERLAPPED (the offset a ReadFile / WriteFile on a non-overlapped file starts at)
struct Overlapped {
    uint32_t Internal, InternalHigh, Offset, OffsetHigh, hEvent;
};

// DuplicateHandle of the pseudo-handles GetCurrentThread() / GetCurrentProcess(): a real handle to the calling thread /
// the process -- objects of the thread stand-ins (agent B's w32_kernel), which set these when they start. Null: such a
// DuplicateHandle fails with ERROR_INVALID_HANDLE.
extern std::shared_ptr<Object> (*current_thread_object)();
extern std::shared_ptr<Object> (*current_process_object)();

// how many files, listings and mappings are open (diagnostics)
uint32_t file_open_count();

}  // namespace w32

typedef uint32_t w32_HANDLE;

// files
w32_HANDLE W32_STDCALL w32_CreateFileA(const char* name, uint32_t access, uint32_t share, void* sa, uint32_t disposition,
                                       uint32_t flags, w32_HANDLE tmpl);
uint32_t W32_STDCALL w32_ReadFile(w32_HANDLE h, void* buf, uint32_t n, uint32_t* read, w32::Overlapped* ov);
uint32_t W32_STDCALL w32_WriteFile(w32_HANDLE h, const void* buf, uint32_t n, uint32_t* written, w32::Overlapped* ov);
uint32_t W32_STDCALL w32_SetFilePointer(w32_HANDLE h, int32_t distance, int32_t* distance_high, uint32_t method);
uint32_t W32_STDCALL w32_SetEndOfFile(w32_HANDLE h);
uint32_t W32_STDCALL w32_FlushFileBuffers(w32_HANDLE h);
uint32_t W32_STDCALL w32_GetFileSize(w32_HANDLE h, uint32_t* high);
uint32_t W32_STDCALL w32_GetFileType(w32_HANDLE h);
w32_HANDLE W32_STDCALL w32_GetStdHandle(uint32_t which);
uint32_t W32_STDCALL w32_SetStdHandle(uint32_t which, w32_HANDLE h);
uint32_t W32_STDCALL w32_CloseHandle(w32_HANDLE h);
uint32_t W32_STDCALL w32_DuplicateHandle(w32_HANDLE src_process, w32_HANDLE src, w32_HANDLE dst_process, w32_HANDLE* dst,
                                         uint32_t access, uint32_t inherit, uint32_t options);
uint32_t W32_STDCALL w32_DeleteFileA(const char* name);
uint32_t W32_STDCALL w32_CopyFileA(const char* from, const char* to, uint32_t fail_if_exists);
uint32_t W32_STDCALL w32_CreateDirectoryA(const char* name, void* sa);
uint32_t W32_STDCALL w32_SetFileAttributesA(const char* name, uint32_t attrs);

// paths
uint32_t W32_STDCALL w32_GetFullPathNameA(const char* name, uint32_t n, char* buf, char** file_part);
uint32_t W32_STDCALL w32_GetCurrentDirectoryA(uint32_t n, char* buf);
uint32_t W32_STDCALL w32_SetCurrentDirectoryA(const char* name);
uint32_t W32_STDCALL w32_GetDriveTypeA(const char* root);
uint32_t W32_STDCALL w32_GetTempFileNameA(const char* path, const char* prefix, uint32_t unique, char* buf);

// listings
w32_HANDLE W32_STDCALL w32_FindFirstFileA(const char* pattern, w32::FindDataA* fd);
uint32_t W32_STDCALL w32_FindNextFileA(w32_HANDLE h, w32::FindDataA* fd);
uint32_t W32_STDCALL w32_FindClose(w32_HANDLE h);

// mappings
w32_HANDLE W32_STDCALL w32_CreateFileMappingA(w32_HANDLE file, void* sa, uint32_t protect, uint32_t size_high,
                                              uint32_t size_low, const char* name);
void* W32_STDCALL w32_MapViewOfFile(w32_HANDLE mapping, uint32_t access, uint32_t offset_high, uint32_t offset_low,
                                    uint32_t n);
uint32_t W32_STDCALL w32_UnmapViewOfFile(const void* view);

// not available
w32_HANDLE W32_STDCALL w32_CreateMailslotA(const char* name, uint32_t max_message, uint32_t timeout, void* sa);
uint32_t W32_STDCALL w32_DeviceIoControl(w32_HANDLE h, uint32_t code, void* in, uint32_t in_size, void* out,
                                         uint32_t out_size, uint32_t* returned, w32::Overlapped* ov);

// the console (the dedicated server)
uint32_t W32_STDCALL w32_AllocConsole();
uint32_t W32_STDCALL w32_FreeConsole();
uint32_t W32_STDCALL w32_ReadConsoleA(w32_HANDLE h, void* buf, uint32_t n, uint32_t* read, void* control);
uint32_t W32_STDCALL w32_WriteConsoleA(w32_HANDLE h, const void* buf, uint32_t n, uint32_t* written, void* reserved);
uint32_t W32_STDCALL w32_SetConsoleMode(w32_HANDLE h, uint32_t mode);
uint32_t W32_STDCALL w32_SetConsoleTitleA(const char* title);

// the last error (no other agent's list has them: the stand-ins' w32::last_error, what the game reads after a failure)
uint32_t W32_STDCALL w32_GetLastError();
void W32_STDCALL w32_SetLastError(uint32_t e);
