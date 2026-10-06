// w32_table.h -- the stand-ins' import table (relink stage R2a): which of race.exe's imports the port implements itself,
// and where. Shared by the port DLL (hook/w32_table.cpp builds it) and the loader (loader/viperport_main.cpp fills
// race.exe's import slots from it); nothing else of either.
//
// The DLL exports one object, viperport_stand_ins. GCC build: every stand-in of hook/w32_*.cpp, one entry per import
// (by name, or by ordinal for WSOCK32's); MSVC build: no entries, so viperport.exe leaves every import as Windows'.
// The object is constant data (its entries are address constants, relocated by Windows when it maps the DLL), so the
// loader reads it as soon as the DLL is mapped -- before its DllMain installs the port, whose session recorder and
// net_wsock take the slots' values as "the real thing" and call through them: they find the stand-ins.
#pragma once
#include <stdint.h>
#if !defined(_WIN32) && !defined(__cdecl)
#define __cdecl __attribute__((cdecl))                   // (R2b: mingw defines it)
#endif

#define VP_STAND_INS_EXPORT "viperport_stand_ins"
#define VP_STAND_INS_VERSION 1

struct VpStandIn {
    const char* dll;                                  // as race.exe's import directory names it ("KERNEL32.dll"; any case)
    const char* name;                                 // the import's name, or 0 for an import by ordinal
    uint32_t ordinal;                                 // (name == 0) the ordinal
    void* fn;                                         // the stand-in (w32_<Name>, the Win32 ABI)
};

struct VpStandIns {
    uint32_t size;                                    // sizeof(VpStandIns)
    uint32_t version;                                 // VP_STAND_INS_VERSION
    uint32_t count;                                   // entries (0 in the MSVC build)
    const VpStandIn* entries;
    // one line into viperport.log (the loader's report of what it filled and what still reaches Windows); callable
    // only once the DLL's DllMain has run
    void(__cdecl* log)(const char* line);
    // who the game is, as if Windows had started race.exe (w32_kernel.h, w32_set_process_image): its image base, path
    // and command line, for the GetModuleHandleA / GetModuleFileNameA / GetCommandLineA stand-ins. Callable only once
    // the DLL's DllMain has run; until the loader has called it, those three slots keep the loader's own shims.
    void(__cdecl* set_process_image)(uint32_t base, const char* path, const char* command_line);
};

#ifndef VP_LOADER
#ifdef _WIN32
#define VP_STAND_INS_API __declspec(dllexport)
#else
#define VP_STAND_INS_API                              // R2b: one executable; the loader links the table directly
#endif
extern "C" VP_STAND_INS_API const VpStandIns viperport_stand_ins;
#endif
