// standalone.h -- viperport.exe (loader/viperport_main.cpp) runs the v1.0 game on the port's code alone, and this is
// the whole of its interface with the port DLL (hook/standalone.cpp).
//
// The loader maps the user's own v1.0 race.exe at 0x400000 the way Windows would, resolves its imports into its IAT
// itself, points GetModuleHandle(NULL) at the image (the PEB's ImageBaseAddress), and only then loads the port DLL --
// the same dinput.dll file, by path -- with VP_STANDALONE_ENV set. The DLL's DllMain installs exactly as on the
// dinput.dll route (M1, port_install, session, replay, platform: every import slot it redirects is already resolved),
// except that standalone every rewrite is new (no shadow, no original) and the platform is SDL + OpenGL + SDL audio
// whatever viperport.ini says, because the original code they would fall back on is gone. Then the loader points
// race.exe's DINPUT import at the DLL's DirectInputCreateA and calls the one export, viperport_standalone():
//   - every inventory function the install hooked keeps its 5-byte jmp and the rest of its code becomes int3; every
//     one it didn't hook becomes int3 entirely; data in .text (switch tables, import-library data, padding) and the
//     bytes the rewrites read at run time stay (standalone.inc, tools/gen_standalone.py);
//   - a vectored handler turns an int3 reached in race.exe's .text into "original code reached at ..." in
//     viperport.log, and ends the game;
//   - VP_SA_RUN: the game starts at race.exe's entry point, 0x4cf5a0 (_WinMainCRTStartup -- its rewrite, hooked
//     there like every other), with the loader's own command line (GetCommandLineA: the arguments pass through);
//     VP_SA_CHECK: the same install, fill and handler, verified and reported line by line, and nothing started.
// The loader depends on nothing else of the DLL, and the DLL on nothing of the loader but this struct.
#pragma once
#include <stdint.h>

#define VP_STANDALONE_VERSION 1
#define VP_STANDALONE_ENV "VIPERPORT_STANDALONE"     // set (to the version) while the loader loads the DLL
#define VP_STANDALONE_EXPORT "viperport_standalone"

enum { VP_SA_RUN = 0, VP_SA_CHECK = 1 };

struct VpStandaloneArgs {
    uint32_t size;                                    // sizeof(VpStandaloneArgs)
    uint32_t version;                                 // VP_STANDALONE_VERSION
    uint32_t mode;                                    // VP_SA_RUN or VP_SA_CHECK
    const char* exe_path;                             // the race.exe mapped at 0x400000
    const uint8_t* file;                              // its bytes as read from disk (the audit's "before")
    uint32_t file_size;
    uint32_t stock;                                   // 1: the file is byte for byte the stock v1.0 race.exe
    void(__cdecl* out)(const char* line);             // --check: one line of the report (the loader prints it)
};

// returns: VP_SA_CHECK -- 0 if every check passed, else the number of failures; VP_SA_RUN -- only if the game can't
// start (the reason was logged and passed to out), else the game ends the process itself
typedef int(__cdecl* VpStandalone_t)(const VpStandaloneArgs* args);

// viperport.exe --probe: "will the standalone run this race.exe?" in a fraction of a second. The loader maps race.exe
// at 0x400000 (nothing resolved, nothing filled) and loads the DLL with VP_PROBE_ENV set, which makes its DllMain do
// nothing at all -- no viperport.log, no install. The export then identifies the build from the image's headers and runs
// the stock check (port_check_stock) the install would run. Returns 0: every rewritten function is stock v1.0 or one of
// vrmod's patches the rewrites take over (line: a summary); n > 0: n functions are patched in a way the port doesn't
// know, and would stay original, which the standalone refuses (line: their names); -1: not v1.0; -2: the loader and the
// DLL are from different builds (version).
#define VP_PROBE_ENV "VIPERPORT_PROBE"
#define VP_PROBE_EXPORT "viperport_probe"
typedef int(__cdecl* VpProbe_t)(uint32_t version, char* line, uint32_t line_size);

#ifndef VP_LOADER
// the DLL side: is this process viperport.exe's? (decided once, when the DLL loads, from VP_STANDALONE_ENV)
bool vp_standalone();
#endif
