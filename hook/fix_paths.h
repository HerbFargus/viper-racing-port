// fix_paths.h -- race.exe's own folder, for the fixes that keep what the game writes next to it (docs/FIXES.md):
// the user directory (krn_file.cpp, get_user_directory) and the logs (log.log: krn_file.cpp, log_file_begin;
// except.log: krn_core.cpp, ExceptBegin; timer.log: phys_task.cpp, TimerWatchdog::dump).
//
// Windows is called through the game's own import slots, as the rewrites do (a harness stubs them), and without
// <windows.h> so that any rewrite file can include this.
#pragma once
#include <stdint.h>
#include <string.h>

typedef uint32_t(__stdcall* VpGetModuleFileNameA_t)(void* module, char* buf, uint32_t size);
typedef int(__stdcall* VpCreateDirectoryA_t)(const char* dir, void* security);
#define vp_GetModuleFileNameA (*(VpGetModuleFileNameA_t volatile*)(uintptr_t)0x005d74e4)
#define vp_CreateDirectoryA (*(VpCreateDirectoryA_t volatile*)(uintptr_t)0x005d7490)

// race.exe's folder, with its trailing backslash, into out[size] (GetModuleFileNameA(NULL): wherever the game was
// started from). Its length, or 0 if GetModuleFileNameA fails or cuts the path short, or the folder doesn't fit.
static uint32_t vp_exe_dir(char* out, uint32_t size) {
    char path[260 + 1];
    const uint32_t n = vp_GetModuleFileNameA(0, path, 260);
    if (n == 0 || n >= 260) return 0;
    path[n] = 0;
    uint32_t len = 0;
    for (uint32_t i = 0; i < n; i++)
        if (path[i] == '\\' || path[i] == '/') len = i + 1;
    if (len == 0 || len + 1 > size) return 0;
    memcpy(out, path, len);
    out[len] = 0;
    return len;
}

// <race.exe's folder>\log\<name> into out[size], the log folder made if it isn't there (the name vrmod's
// writepaths patch uses: its relative "log\..." literals, started from the game's folder, name the same files).
// false if the path doesn't fit in `size` bytes (out is then unspecified).
static bool vp_log_path(char* out, uint32_t size, const char* name) {
    const uint32_t n = vp_exe_dir(out, size);
    const uint32_t nn = (uint32_t)strlen(name);
    if (n == 0 || n + 4 + nn + 1 > size) return false;
    memcpy(out + n, "log", 4);
    vp_CreateDirectoryA(out, 0);                                    // (already there: fine)
    out[n + 3] = '\\';
    memcpy(out + n + 4, name, nn + 1);
    return true;
}
