// root_main.cpp -- M3 UI stage, step U3 (group B): the game's outer loop and the race loop, rewritten faithfully (library
// `root`: main.obj and version.obj; and WinMain, kernel win32.obj -- the other 13 functions left there are the platform
// ones the SDL layer replaces). race.obj, prerace.obj and postrace.obj are root_race.cpp.
//
//   WinMain: the dedicated-server switch, one instance, the kernel, the user directory, the command line, AppMain.
//   AppProcessArgs: -location "( <track> <base64 frame> )" (the blimp), -tri, -grid, -1 (no secondary display),
//   -d<dir> (change directory), -u b/d/r (the programmers' test modes), -z. AppMain: app_begin, race() until the menu
//   quits, the credits, app_end -- or one of the test modes (dave_b: a race at uptown; blimp_jump: the blimp at
//   -location's track; rich_g: start-up and shut-down only; dave_p: nothing). race(): the main menu's choice.
//   DoRace: one race -- the race camera (option "race_camera"), the world, the dashboard, the escape menu, the keys the
//   race uses, game_loop (every frame: the camera, WorldUpdate / WorldDraw, the dashboard, the escape menu, the
//   countdown, until the game state says the race is over ten times), the post-race screen, the results.
//   process_keys: the escape menu, the dashboard's keys, pause, the dashboards (1..6), the cameras (F1..F10, 0x171..),
//   the focus car. move_blimp: the free-flying blimp camera (the numeric keypad; F10 copies it to the TV camera).
//   The escape menus (four static EscapeMenus, filled on first use) and their callbacks; the matrix helpers
//   (MatrixMakeYaw / Pitch / Roll, MatrixConcat), used across the game; the blimp frame's compact form (base64 on the
//   clipboard, -location's argument).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own too, so a
// hooked rewrite is what runs), function pointers and virtual calls as the original makes them, the compiler's inline
// string code as it runs it (ui_types.h). x87: a register value is a double, a stored one a float, the original's
// grouping, its constants' widths (docs/PORTING.md); fsin / fcos results stored straight to floats (x87.h), and the two
// places a transcendental result is computed with further (convert_frame's rotation matrix, from fsin / fcos; the other
// convert_frame's angle, from the CRT's acos then fsin) are the original's instruction sequences in asm.
//
// Footprints (main thread). The matrix helpers and convert_frame(Frame&, ...) write only their output (pure); the other
// convert_frame also the CRT's acos scratch and errno. The small state
// changes list their statics (the camera, the pause and restart flags, the dashboard type, the blimp's vectors and
// frame -- and the TV camera blimp_to_tv writes). replay_only: whatever runs the game or a part of it (WinMain, AppMain,
// app_begin / app_end, race, DoRace, game_loop, the test modes), changes the keys the kernel passes (disable_keys,
// enable_keys, set_dash_type -- DashboardReset --, the escape menu's enter / leave), the physics' run state (pause,
// restart) or the multiplayer's, runs a menu (process_keys: the escape menu, the dashboards' keys), touches the
// clipboard (copy_blimp_to_clipboard) or the directory (AppProcessArgs), or builds a function-local static on its first
// call (move_blimp's vectors register their destructors with atexit, process_keys' Xlators).
//
// FIX CANDIDATEs (left faithful, marked in place): AppProcessArgs' -d<dir> writes a terminator through strchr's result
// unchecked (a -d with no space after it, last on the command line, writes to address 0); setup_blimp_jump's sscanf
// reads the track's name into a static buffer unbounded, and blimp_jump copies it into its World's 0x20 bytes; DoRace's
// Results holds 16 cars, the World's count unchecked, and it reads each car's race record without checking there is one.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "root_main.h"

namespace {
namespace root_main {
using namespace uit;
using namespace rootb;

#define D(x) ((double)(x))
#define CP(p) ((const char*)(uintptr_t)(p))
static __forceinline float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline void st_f(volatile float* p, double v) { *p = (float)v; }
enum : uint32_t { F_atoi_c = 0x004cf990 };
static __forceinline DashType* dash_at(int32_t d) { return (DashType*)(uintptr_t)(S_DASH_TABLE + (uint32_t)d * 0x14u); }

// ---- footprint helpers --------------------------------------------------------------------------------------------------
static void fp_none(Footprint&) {}
static void fp_pure(Footprint& f) { f.pure = true; }
#define RO(name, why) static void name(Footprint& f) { f.replay_only = why; }
RO(fp_runs_game, "runs the game (every subsystem's start-up, the menus, races)")
RO(fp_begin_end, "starts or shuts down every subsystem (graphics, sound, options, resources, the UI)")
RO(fp_race_loop, "runs a race (the world, the physics, the dashboard, the post-race screen: its own loop)")
RO(fp_keys, "enables or disables the kernel's keys (and resets the dashboard)")
RO(fp_physics_state, "pauses, unpauses or restarts the physics or the multiplayer race")

// =========================================================================================================================
// win32.obj
// =========================================================================================================================
typedef int(__stdcall* IsWindow_t)(void*);
typedef int(__stdcall* DestroyWindow_t)(void*);

// WinMain: "-dedicated[:port]" runs the server alone; otherwise the loading window, the kernel, the user directory, the
// command line and the game
static int __stdcall WinMain_c(void* inst, void* prev, char* cmd, int show) {
    (void)prev; (void)show;
    uint32_t port = 0;                                              // ebx (bx: atoi's low word)
    UI_G8(S_W32_EC0) = 0;
    UI_GP(void, S_W32_INSTANCE) = inst;
    UI_GP(char, S_W32_ARGS) = cmd;
    const char* ded = ccall<const char*>(F_strstr, (const char*)cmd, CP(0x004e5f80));
    const uint8_t is_ded = ded != 0 ? 1 : 0;
    UI_G8(S_W32_DEDICATED) = is_ded;
    if (is_ded && *(const volatile char*)(ded + 10) == ':')
        port = (uint16_t)ccall<int32_t>(F_atoi_c, ded + 11);
    if (ccall<uint8_t>(F_start_unique_instance)) {
        if (ccall<uint8_t>(F_check_for_canary_launch)) {
            if (UI_G8(S_W32_DEDICATED)) {
                if (ccall<uint8_t>(F_KernelBegin)) {
                    ccall<void>(F_MultiDedicatedServer, port);
                    ccall<void>(F_KernelEnd);
                }
            } else {
                ccall<void>(F_make_loading_window);
                ccall<uint8_t>(F_vxdLoad);
                if (ccall<uint8_t>(F_KernelBegin)) {
                    if (ccall<uint8_t>(F_get_user_directory)) {
                        if (crt_strlen(CP(S_W32_USERDIR)) > 0xc8)
                            UI_LogReport(CP(0x004e5f8c), crt_strlen(CP(S_W32_USERDIR)));
                        if (ccall<uint8_t>(F_AppProcessArgs, cmd)) {
                            if (UI_GP(void, S_W32_SPLASH) &&
                                (*(IsWindow_t volatile*)(uintptr_t)IAT_IsWindow)(UI_GP(void, S_W32_SPLASH)))
                                (*(DestroyWindow_t volatile*)(uintptr_t)IAT_DestroyWindow)(UI_GP(void, S_W32_SPLASH));
                            ccall<void>(F_AppMain);
                        }
                    }
                    ccall<void>(F_KernelEnd);
                }
                ccall<void>(F_vxdUnload);
            }
        }
        ccall<void>(F_end_unique_instance);
    }
    return 0;
}
static void fp_WinMain(Footprint& f, void*, void*, char*, int) { f.replay_only = "runs the game"; }
PORT_FN(0x00412260, "WinMain", WinMain_c, fp_WinMain)

// =========================================================================================================================
// version.obj
// =========================================================================================================================
static const char* __cdecl VersionGetBuildString_c() { return CP(S_VERSION); }
PORT_FN(0x00410cb0, "VersionGetBuildString", VersionGetBuildString_c, fp_pure)
static void __cdecl rcfunc_is_internal_c() {}
PORT_FN(0x00410cc0, "rcfunc_is_internal", rcfunc_is_internal_c, fp_pure)

// =========================================================================================================================
// main.obj
// =========================================================================================================================
// AppProcessArgs: the switches, each a '-' or '/' and a word; a character that isn't a switch is skipped
static uint8_t __cdecl AppProcessArgs_c(char* args) {
    UI_G8(S_Z_FLAG) = 0;
    char* p = args;                                                 // esi
    if (p && *(volatile char*)p) {
        do {
            const int32_t c = (int8_t) * (volatile char*)p;
            if (c == '-' || c == '/') {
                char* q = p + 1;                                    // edi
                if (ccall<int>(F_strncmp, q, CP(0x004e39b4), 8) == 0) {
                    UI_G32(S_APP_MODE) = 4;
                    ccall<void>(F_setup_blimp_jump, (const char*)(p + 9));
                } else if (ccall<int>(F_strncmp, q, CP(0x004e39c0), 3) == 0) {
                    UI_G8(S_TRI) = 1;
                } else if (ccall<int>(F_strncmp, q, CP(0x004e39c4), 4) == 0) {
                    UI_G8(S_GRID) = 1;
                } else {
                    p = q;
                    const int8_t ch = *(volatile int8_t*)q;
                    const int32_t k = ((ch >= 'A' && ch <= 'Z') ? ch + 0x20 : ch) - 0x31;
                    if ((uint32_t)k <= 0x49) {
                        switch (k) {                                // (the original's byte table: '1', 'd', 'u', 'z')
                        case '1' - 0x31:
                            ccall<void>(F_gxDisableSecondary);
                            break;
                        case 'd' - 0x31: {                          // -d<dir>
                            char* dir = q + 1;
                            // FIX CANDIDATE: no space after the directory (-d last on the command line) makes strchr
                            // return 0, and the terminator is written to address 0
                            char* sp = ccall<char*>(uit::F_strchr, (const char*)dir, 0x20);
                            *(volatile char*)sp = 0;
                            p = sp;
                            if (!ccall<uint8_t>(F_FileChangeDir, (const char*)dir)) UI_LogPanic(CP(0x004e39cc), dir);
                            break;
                        }
                        case 'u' - 0x31: {                          // -u b / d / r
                            p = q + 1;
                            const int8_t f8 = *(volatile int8_t*)p;
                            const int32_t fc = (int32_t)f8;
                            const int32_t lc = (f8 >= 'A' && f8 <= 'Z') ? fc + 0x20 : fc;
                            if (lc == 'b') UI_G32(S_APP_MODE) = 1;
                            else if (lc == 'd') UI_G32(S_APP_MODE) = 2;
                            else if (lc == 'r') UI_G32(S_APP_MODE) = 3;
                            else UI_LogReport(CP(0x004e39e4), fc);
                            break;
                        }
                        case 'z' - 0x31:
                            UI_G8(S_Z_FLAG) = 1;
                            break;
                        }
                    }
                }
            }
            p++;
        } while (*(volatile char*)p);
    }
    return 1;
}
static void fp_AppProcessArgs(Footprint& f, char*) { f.replay_only = "start-up: changes the directory, turns off the secondary display"; }
PORT_FN(0x00401280, "AppProcessArgs", AppProcessArgs_c, fp_AppProcessArgs)

// AppMain: the game, or a programmer's test mode
static void __cdecl AppMain_c() {
    ccall<void>(F_UsefulBegin);
    switch ((uint32_t)UI_G32(S_APP_MODE)) {
    case 0:
        ccall<void>(F_app_begin);
        while (ccall<uint8_t>(F_race)) {}
        ccall<void>(F_CreditsDo);
        ccall<void>(F_app_end);
        break;
    case 1: ccall<void>(F_dave_b); break;
    case 2: ccall<void>(F_dave_p); break;
    case 3: ccall<void>(F_rich_g); break;
    case 4: ccall<void>(F_blimp_jump); break;
    }
    ccall<void>(F_UsefulEnd);
}
PORT_FN(0x00401460, "AppMain", AppMain_c, fp_runs_game)

// app_begin: graphics, the intro, the blimp's frame (identity), options, locale, sound, the resources, the races' tables,
// the cheats, the UI, the racing lines, the paint jobs, the AI drivers
static void __cdecl app_begin_c() {
    ccall<void>(F_gxBegin);
    ccall<void>(F_mrBegin);
    ccall<void>(F_gxSetMode, 2);
    ccall<void>(F_IntroPlayVideo, CP(0x004e3a0c));
    volatile uint32_t* fr = (volatile uint32_t*)(uintptr_t)S_BLIMP_FRAME;
    fr[0] = 0x3f800000; fr[1] = 0; fr[2] = 0; fr[3] = 0; fr[4] = 0x3f800000; fr[5] = 0;
    fr[6] = 0; fr[7] = 0; fr[8] = 0x3f800000; fr[9] = 0; fr[10] = 0; fr[11] = 0;
    ccall<void>(F_OptionsBegin);
    ccall<void>(F_LocaleBegin);
    ccall<uint8_t>(F_SoundBegin);
    ccall<void>(F_ResourceSetMustLoad, CP(0x004e3a18));
    ccall<void>(F_ResourceSetMustLoad, CP(0x004e3a24));
    ccall<void>(F_ResourceSetMustLoad, CP(0x004e3a2c));
    ccall<void>(F_RaceBegin);
    ccall<void>(F_HackBegin, 1);
    ccall<void>(uit::F_UIBegin);
    ccall<void>(F_PreLineBegin);
    ccall<void>(F_PaintKitInstallPaintJobs);
    ccall<void>(F_AIDriverBegin);
    ccall<void>(F_Win32RestrictCursor);
}
PORT_FN(0x004014d0, "app_begin", app_begin_c, fp_begin_end)

static void __cdecl app_end_c() {
    ccall<void>(F_AIDriverEnd);
    ccall<void>(F_PreLineEnd);
    ccall<void>(uit::F_UIEnd);
    ccall<void>(F_HackEnd);
    ccall<void>(F_RaceEnd);
    ccall<void>(F_ResourceSetUnload, CP(0x004e3a38));
    ccall<void>(F_ResourceSetUnload, CP(0x004e3a44));
    ccall<void>(F_SoundEnd);
    ccall<void>(F_ResourceSetUnload, CP(0x004e3a4c));
    ccall<void>(F_gxRestoreMode);
    ccall<void>(F_mrEnd);
    ccall<void>(F_gxEnd);
    ccall<void>(F_LocaleEnd);
    ccall<void>(F_OptionsEnd);
}
PORT_FN(0x004015a0, "app_end", app_end_c, fp_begin_end)

// setup_blimp_jump: -location's "( <track> <base64 CompactFrame> )"; a track without a frame starts at the identity
static void __cdecl setup_blimp_jump_c(const char* arg) {
    char b64[0x100];                                                // (the frame's local at +0x18 of 0x118)
    uint8_t cf[0x18];
    UI_G8(S_BLIMP_TRACK) = 0;
    // FIX CANDIDATE: sscanf's %s writes the track's name into the static buffer at 0x503fb0 unbounded (the command line)
    typedef int(__cdecl * Sscanf_t)(const char*, const char*, ...);
    if (((Sscanf_t)(uintptr_t)F_sscanf)(arg, CP(0x004e3a58), (char*)(uintptr_t)S_BLIMP_TRACK, b64) == 2 &&
        ccall<uint8_t>(F_Base64ToMem, (void*)cf, 0x18, (const char*)b64)) {
        ccall<void>(F_convert_frame_in, (void*)(uintptr_t)S_BLIMP_START, (const void*)cf);
        return;
    }
    if (UI_G8(S_BLIMP_TRACK)) {
        UI_LogReport(CP(0x004e3a64));
        volatile uint32_t* fr = (volatile uint32_t*)(uintptr_t)S_BLIMP_START;
        fr[0] = 0x3f800000; fr[1] = 0; fr[2] = 0; fr[3] = 0; fr[4] = 0x3f800000; fr[5] = 0;
        fr[6] = 0; fr[7] = 0; fr[8] = 0x3f800000; fr[9] = 0; fr[10] = 0; fr[11] = 0;
        return;
    }
    UI_LogPanic(CP(0x004e3a9c), arg);
}
static void fp_setup_blimp_jump(Footprint& f, const char* arg) {
    UI_FP(f, S_BLIMP_TRACK, arg ? crt_strlen(arg) + 1 : 1, "main.obj -location's track (sscanf %s)");
    UI_FP(f, S_BLIMP_START, 0x30, "main.obj -location's frame");
    UI_FP(f, 0x004eb444, 1, "base64.obj: the table is still to be built");     // (Base64ToMem builds it on first use)
    UI_FP(f, 0x00509660, 0x100, "base64.obj decoding table");
}
PORT_FN(0x00401600, "setup_blimp_jump", setup_blimp_jump_c, fp_setup_blimp_jump)

// convert_frame(Frame&, CompactFrame const&): the position; the rotation from its axis * angle (Rodrigues), or the identity
// for an angle of 1e-4 or less. The matrix is built from fsin / fcos results the original keeps in registers: its own
// instruction sequence, in asm.
static const float k_one = 1.0f;
static void __cdecl convert_frame_in_c(RbFrame* dst, const RbCompactFrame* src) {
    volatile uint32_t* o = (volatile uint32_t*)dst;
    const volatile uint32_t* i = (const volatile uint32_t*)src;
    o[9] = i[0];
    o[10] = i[1];
    o[11] = i[2];
    const double a = x87_sqrt((D(src->r[2]) * D(src->r[2]) + D(src->r[1]) * D(src->r[1])) + D(src->r[0]) * D(src->r[0]));
    volatile float angf;
    st_f(&angf, a);
    if (!(a > D(bits_f(0x38d1b717)))) {
        o[0] = 0x3f800000; o[1] = 0; o[2] = 0; o[3] = 0; o[4] = 0x3f800000; o[5] = 0;
        o[6] = 0; o[7] = 0; o[8] = 0x3f800000;
        return;
    }
    volatile float t4, t8, tc, t10;                                 // the original's [esp+4] .. [esp+0x10]
    volatile float* pa = &angf;
    volatile float* p4 = &t4;
    volatile float* p8 = &t8;
    volatile float* pc = &tc;
    volatile float* p10 = &t10;
    __asm {
        mov  edx, dst
        mov  esi, src
        mov  eax, pa
        fld  dword ptr [eax]
        fdivr k_one
        fld  dword ptr [esi + 0xc]
        fmul st, st(1)
        fld  dword ptr [esi + 0x10]
        fmul st, st(2)
        fxch st(2)
        fmul dword ptr [esi + 0x14]
        fld  dword ptr [eax]
        fsin
        fld  dword ptr [eax]
        fcos
        fld  k_one
        fsub st, st(1)
        fld  st(4)
        fmul st, st(5)
        fmul st, st(1)
        fadd st, st(2)
        fstp dword ptr [edx]
        fld  st(4)
        fmul st, st(1)
        mov  eax, p4
        fst  dword ptr [eax]
        fmul st, st(6)
        mov  ecx, pc
        fstp dword ptr [ecx]
        fld  st(2)
        fmul st, st(4)
        mov  ecx, p8
        fst  dword ptr [ecx]
        mov  ecx, pc
        fadd dword ptr [ecx]
        fstp dword ptr [edx + 4]
        fld  dword ptr [eax]
        fmul st, st(4)
        mov  ecx, p10
        fstp dword ptr [ecx]
        fld  st(2)
        fmul st, st(6)
        fst  dword ptr [eax]
        fsubr dword ptr [ecx]
        fstp dword ptr [edx + 8]
        mov  ecx, pc
        fld  dword ptr [ecx]
        mov  ecx, p8
        fsub dword ptr [ecx]
        fstp dword ptr [edx + 0xc]
        fld  st(5)
        fmul st, st(6)
        fmul st, st(1)
        fadd st, st(2)
        fstp dword ptr [edx + 0x10]
        fld  st(0)
        fmul st, st(6)
        fmul st, st(4)
        fstp dword ptr [ecx]
        fxch st(4)
        fmul st, st(2)
        fld  dword ptr [ecx]
        fadd st, st(1)
        fstp dword ptr [edx + 0x14]
        mov  ecx, p10
        fld  dword ptr [ecx]
        fadd dword ptr [eax]
        fstp dword ptr [edx + 0x18]
        mov  ecx, p8
        fsubr dword ptr [ecx]
        fstp dword ptr [edx + 0x1c]
        fxch st(2)
        fmul st, st(0)
        fmul st, st(3)
        fadd st, st(2)
        fstp dword ptr [edx + 0x20]
        fstp st(0)
        fstp st(0)
        fstp st(0)
        fstp st(0)
    }
}
static void fp_convert_frame_in(Footprint& f, RbFrame* out, const RbCompactFrame*) {
    f.pure = true;
    f.add(out, sizeof(RbFrame), "the frame");
}
PORT_FN(0x004016f0, "convert_frame(Frame&,CompactFrame const&)", convert_frame_in_c, fp_convert_frame_in)

static uint8_t __cdecl is_pause_key_c(uint32_t key) {              // (unsigned short: its low word)
    const uint32_t k = key & 0xffff;
    return (k == 0x50 || k == 0x70) ? 1 : 0;
}
static void fp_is_pause_key(Footprint& f, uint32_t) { f.pure = true; }
PORT_FN(0x00401840, "is_pause_key", is_pause_key_c, fp_is_pause_key)
static uint8_t __cdecl is_pause_scankey_c(uint32_t sc) { return (sc & 0xff) == 0x19 ? 1 : 0; }
static void fp_is_pause_scankey(Footprint& f, uint32_t) { f.pure = true; }
PORT_FN(0x00401860, "is_pause_scankey", is_pause_scankey_c, fp_is_pause_scankey)

// move_blimp: the free-flying camera. Its velocity and turn rates decay by 0.7 a frame; the keypad sets them (the scan
// keys 0xfd / 0xfe pick the speed), the velocity moves the frame along its own axes, the turns (yaw, then pitch, then
// roll) rotate it; F10 (scan 0x10) copies it to the TV camera in use, once a press.
typedef void(__cdecl* MatMake_t)(void*, uint32_t);
typedef void(__cdecl* MatConcat_t)(void*, const void*, const void*);
static void __cdecl move_blimp_c() {
    volatile uint32_t rot[3], vel[3];                               // the frame's [esp+4..0xc], [esp+0x10..0x18]
    volatile uint32_t speed_tab[4];                                 // [esp+0x1c..]; [0] then holds the chosen speed
    volatile uint32_t turn_tab[4];                                  // [esp+0x40..]; [0] the chosen turn rate
    uint32_t m1[9], m0[9];                                          // [esp+0x1c] (after the table's use), [esp+0x40]
    vel[0] = 0;
    vel[1] = 0; vel[2] = 0; rot[0] = 0; rot[1] = 0; rot[2] = 0;
    uint8_t g = UI_G8(S_BLIMP_GUARD);
    if (!(g & 1)) {
        g |= 1;
        UI_G8(S_BLIMP_GUARD) = g;
        UI_GU32(S_BLIMP_VEL) = 0; UI_GU32(S_BLIMP_VEL + 4) = 0; UI_GU32(S_BLIMP_VEL + 8) = 0;
        ccall<int>(uit::F_atexit, 0x00401d40);
        g = UI_G8(S_BLIMP_GUARD);
    }
    if (!(g & 2)) {
        g |= 2;
        UI_G8(S_BLIMP_GUARD) = g;
        UI_GU32(S_BLIMP_ROT) = 0; UI_GU32(S_BLIMP_ROT + 4) = 0; UI_GU32(S_BLIMP_ROT + 8) = 0;
        ccall<int>(uit::F_atexit, 0x00401d30);
    }
    const float k07 = bits_f(0x3f333333);
    for (uint32_t a = S_BLIMP_VEL; a < S_BLIMP_VEL + 12; a += 4) st_f(&UI_GF(a), D(UI_GF(a)) * D(k07));
    for (uint32_t a = S_BLIMP_ROT; a < S_BLIMP_ROT + 12; a += 4) st_f(&UI_GF(a), D(UI_GF(a)) * D(k07));
    const uint8_t s_fd = ccall<uint8_t>(F_ScanDown, 0xfd);
    const uint8_t s_fe = ccall<uint8_t>(F_ScanDown, 0xfe);
    const uint32_t idx = ((uint32_t)s_fd + (uint32_t)s_fd) | (uint32_t)s_fe;
    speed_tab[0] = 0x3f800000; speed_tab[1] = 0x3e800000; speed_tab[2] = 0x41200000; speed_tab[3] = 0x3d4ccccd;
    turn_tab[0] = 0x3d4ccccd; turn_tab[1] = 0x3c4ccccd;
    const uint32_t sp = speed_tab[idx];
    turn_tab[2] = 0x3e19999a; turn_tab[3] = 0x3b23d70a;
    const uint32_t tr = turn_tab[idx];
    speed_tab[0] = sp;
    turn_tab[0] = tr;
    volatile float* spf = (volatile float*)&speed_tab[0];
    volatile float* trf = (volatile float*)&turn_tab[0];
    volatile float* velf = (volatile float*)vel;
    volatile float* rotf = (volatile float*)rot;
    if (ccall<uint8_t>(F_ScanDown, 0x1e)) vel[2] = speed_tab[0];
    else if (ccall<uint8_t>(F_ScanDown, 0x2c)) st_f(&velf[2], -D(*spf));
    if (ccall<uint8_t>(F_ScanDown, 0x51)) vel[0] = speed_tab[0];
    else if (ccall<uint8_t>(F_ScanDown, 0x4f)) st_f(&velf[0], -D(*spf));
    if (ccall<uint8_t>(F_ScanDown, 0x53)) st_f(&velf[1], D(*spf) * D(0.5f));
    else if (ccall<uint8_t>(F_ScanDown, 0x52)) st_f(&velf[1], D(*spf) * D(-0.5f));
    if (ccall<uint8_t>(F_ScanDown, 0x4d)) {
        if (ccall<uint8_t>(F_ScanDown, 0x4f)) st_f(&rotf[1], D(*trf) * D(0.5f));
        else rot[1] = turn_tab[0];
    } else if (ccall<uint8_t>(F_ScanDown, 0x4b)) {
        const uint8_t k = ccall<uint8_t>(F_ScanDown, 0x51);
        const double t = D(*trf);
        if (k) st_f(&rotf[1], t * D(-0.5f));
        else st_f(&rotf[1], -t);
    }
    if (ccall<uint8_t>(F_ScanDown, 0x48)) st_f(&rotf[0], D(*trf) * D(0.5f));
    else if (ccall<uint8_t>(F_ScanDown, 0x50)) st_f(&rotf[0], D(*trf) * D(-0.5f));
    if (ccall<uint8_t>(F_ScanDown, 0x47)) rot[2] = turn_tab[0];
    else if (ccall<uint8_t>(F_ScanDown, 0x49)) st_f(&rotf[2], -D(*trf));
    // a vector that isn't all (plus or minus) zero replaces the decayed one (the bits)
    if ((rot[0] & 0x7fffffff) || (rot[1] & 0x7fffffff) || (rot[2] & 0x7fffffff)) {
        const uint32_t r1 = rot[1], r0 = rot[0], r2 = rot[2];
        UI_GU32(S_BLIMP_ROT) = r0; UI_GU32(S_BLIMP_ROT + 4) = r1; UI_GU32(S_BLIMP_ROT + 8) = r2;
    }
    if ((vel[0] & 0x7fffffff) || (vel[1] & 0x7fffffff) || (vel[2] & 0x7fffffff)) {
        const uint32_t v1 = vel[1], v0 = vel[0], v2 = vel[2];
        UI_GU32(S_BLIMP_VEL) = v0; UI_GU32(S_BLIMP_VEL + 4) = v1; UI_GU32(S_BLIMP_VEL + 8) = v2;
    }
    // the velocity in the world's axes (the frame's rows), added to the position
    volatile float* fm = (volatile float*)(uintptr_t)S_BLIMP_FRAME;
    volatile float* sv = (volatile float*)(uintptr_t)S_BLIMP_VEL;
    volatile float w[3];
    st_f(&w[0], (D(sv[2]) * D(fm[6]) + D(sv[1]) * D(fm[3])) + D(sv[0]) * D(fm[0]));
    st_f(&w[1], (D(sv[2]) * D(fm[7]) + D(sv[1]) * D(fm[4])) + D(sv[0]) * D(fm[1]));
    st_f(&w[2], (D(sv[1]) * D(fm[5]) + D(sv[2]) * D(fm[8])) + D(sv[0]) * D(fm[2]));
    st_f(&fm[9], D(w[0]) + D(fm[9]));
    const uint32_t r2 = UI_GU32(S_BLIMP_ROT + 8), r0 = UI_GU32(S_BLIMP_ROT), r1 = UI_GU32(S_BLIMP_ROT + 4);
    rot[2] = r2;
    st_f(&fm[10], D(w[1]) + D(fm[10]));
    rot[0] = r0;
    st_f(&fm[11], D(w[2]) + D(fm[11]));
    ((MatMake_t)(uintptr_t)F_MatrixMakeYaw)(m0, r1);
    ((MatMake_t)(uintptr_t)F_MatrixMakePitch)(m1, rot[0]);
    ((MatConcat_t)(uintptr_t)F_MatrixConcat)(m0, m1, m0);
    ((MatMake_t)(uintptr_t)F_MatrixMakeRoll)(m1, rot[2]);
    ((MatConcat_t)(uintptr_t)F_MatrixConcat)(m0, m1, m0);
    ((MatConcat_t)(uintptr_t)F_MatrixConcat)((void*)(uintptr_t)S_BLIMP_FRAME, m0, (const void*)(uintptr_t)S_BLIMP_FRAME);
    if (ccall<uint8_t>(F_ScanDown, 0x10)) {
        if (!UI_G8(S_BLIMP_TV)) {
            UI_G8(S_BLIMP_TV) = 1;
            ccall<void>(F_blimp_to_tv);
        }
    } else {
        UI_G8(S_BLIMP_TV) = 0;
    }
}
static void fp_move_blimp(Footprint& f) {
    if ((UI_G8(S_BLIMP_GUARD) & 3) != 3) { f.replay_only = "the first call makes its vectors (atexit)"; return; }
    UI_FP(f, S_BLIMP_VEL, 12, "main.obj blimp velocity");
    UI_FP(f, S_BLIMP_ROT, 12, "main.obj blimp turn rates");
    UI_FP(f, S_BLIMP_FRAME, 0x30, "blimp_frame");
    UI_FP(f, S_BLIMP_TV, 1, "main.obj blimp F10 down");
    const int32_t cur = UI_G32(0x004ec9dc);                          // blimp_to_tv's TV camera (phys_camera.cpp)
    if (cur < UI_G32(0x00521084)) UI_FP(f, 0x00520cb8 + (uint32_t)cur * 0x1cu, 0x1c, "the TV camera in use");
}
PORT_FN(0x00401880, "move_blimp", move_blimp_c, fp_move_blimp)

static uint8_t __cdecl MainIsDone_c() { return UI_G8(S_MAIN_DONE); }
PORT_FN(0x00401d50, "MainIsDone", MainIsDone_c, fp_none)
static void __cdecl MainSetCamera_c(int32_t c) { UI_G32(S_CAMERA) = c; }
static void fp_MainSetCamera(Footprint& f, int32_t) { UI_FP(f, S_CAMERA, 4, "main.obj camera"); }
PORT_FN(0x00401d60, "MainSetCamera", MainSetCamera_c, fp_MainSetCamera)

// race: the main menu's choice, run; 0 when the menu quits (or panics)
static uint8_t __cdecl race_c() {
    const uint32_t t = ccall<uint32_t>(F_MenuDoRaceMenu);
    switch (t) {
    case 0: ccall<void>(F_GameDoSingle); return 1;
    case 1: ccall<void>(F_GameDoMulti); return 1;
    case 2: ccall<void>(F_CareerDo); return 1;
    case 3: ccall<void>(F_MenuDoOptions); return 1;
    case 4: ccall<void>(F_ReplayDo, (void*)0); return 1;
    case 5: UI_LogReport(CP(0x004e3abc)); ccall<void>(F_EditMenu); return 1;
    case 7: ccall<uint8_t>(F_AIDriverConfig, 0); return 1;
    case 8: return 0;
    }
    UI_LogPanic(CP(0x004e3ac8));
    return 0;
}
PORT_FN(0x00401d70, "race", race_c, fp_runs_game)

// DoRace: one race, start to end (the world, the dashboard, the loop, the post-race screen or the caller's), the results
typedef void(__cdecl* OptGet_t)(const char*, const char*, volatile int32_t*);
typedef void(__cdecl* OptSet_t)(const char*, const char*, int32_t);
static void __cdecl DoRace_c(uint8_t* world, int32_t* res, void(__cdecl* post)(uint8_t*)) {
    ccall<void>(F_OptionsFlush);
    volatile int32_t cam = 5;
    if (UI_G32(S_APP_MODE) != 4) {
        ((OptGet_t)(uintptr_t)0x004713e0)(UI_GP(const char, 0x004db000), CP(0x004e3ad8), &cam);
        const int32_t c = cam;
        UI_G32(S_CAMERA) = c;
        if (c == 0xc) UI_G32(S_CAMERA) = 4;
        UI_G32(S_CAMERA_SAVED) = UI_G32(S_CAMERA);
    }
    ccall<void>(F_ProfReset);
    ccall<void>(F_CountdownBegin);
    ccall<void>(F_TelemetryBegin);
    ccall<void>(F_WorldBegin, world);
    ccall<void>(F_ResourceMinimizeAll);
    ccall<void>(F_DashboardBegin);
    ccall<void>(F_EscapeMenuBegin);
    ccall<void>(F_disable_keys);
    UI_G8(S_PAUSED) = 0;
    UI_G8(S_MAIN_DONE) = 0;
    ccall<void>(F_SoundUnMuteCars);
    ccall<void>(F_Win32FlushRegistry);
    ccall<void>(F_CameraSetType, UI_G32(S_CAMERA));
    ccall<void>(F_SplashDoneLoading);
    ccall<void>(F_PhysicsMaybeRun);
    if (ccall<uint8_t>(F_AILearnMode)) ccall<void>(F_AIDisplayLearnMode, (const uint8_t*)world);
    else ccall<void>(F_game_loop);
    ccall<void>(F_SoundMuteCars);
    ccall<void>(F_MultiWorldIsDead);
    ccall<void>(F_enable_keys);
    ccall<void>(F_PhysicsPostRace);
    ccall<void>(F_DashboardEnd);
    ccall<void>(F_EscapeMenuEnd);
    ccall<void>(F_ResourceMaximizeAll);
    ccall<void>(F_WorldSwitchToUI);
    ccall<void>(F_RecordConsolidate);
    ccall<void>(F_MultiPostRace);
    ccall<void>(F_ProfResetAndReport);
    if (!ccall<uint8_t>(F_AILearnMode)) {
        if (post) post(world);
        else ccall<void>(F_PostRaceDo, world);
    }
    if (res) {
        volatile int32_t* r = res;
        for (int k = 0; k < 0x31; k++) r[k] = 0;
        const int32_t n = *(volatile int32_t*)(world + 0xca8);
        r[0] = n;
        // FIX CANDIDATE: the Results hold 16 cars, the World's count unchecked against it; and a car without a race record
        // (RecordGetRaceResult's 0) is read through a null pointer
        if (n > 0) {
            int32_t i = 0;
            volatile int32_t* e = r + 2;                            // ebp: the entry's second dword
            do {
                const uint8_t* rec = ccall<const uint8_t*>(F_RecordGetRaceResult, i);
                e[0] = *(const volatile int32_t*)(rec + 0xc);
                e[1] = *(const volatile int32_t*)(rec + 4);
                if (*(const volatile int32_t*)(rec + 0xc) > 0)
                    e[-1] = *(const volatile int32_t*)(ccall<const uint8_t*>(F_CarMgrGetInfo, i) + 0x140);
                e += 3;
                i++;
            } while (r[0] > i);
        }
    }
    ccall<void>(F_WorldEnd, world);
    ccall<void>(F_TelemetryEnd);
    ccall<void>(F_CountdownEnd);
    ccall<void>(F_SoundFlushAsync);
    const int32_t saved = UI_G32(S_CAMERA_SAVED);
    cam = saved;
    ((OptSet_t)(uintptr_t)0x00471430)(UI_GP(const char, 0x004db000), CP(0x004e3ae4), saved);
}
static void fp_DoRace(Footprint& f, uint8_t*, int32_t*, void(__cdecl*)(uint8_t*)) { f.replay_only = "runs a race (its own loop)"; }
PORT_FN(0x00401e10, "DoRace", DoRace_c, fp_DoRace)

// disable_keys: every key off but the race's own (F1..F10, 1..6, Esc, P, the arrows) and the dashboard's
static void __cdecl disable_keys_c() {
    ccall<void>(F_KeyDisableAll);
    for (uint32_t i = 0; i < 0x18; i++) ccall<void>(F_KeyEnable, UI_G8(0x004db010 + i));
    const volatile uint8_t* k = dash_at(UI_G32(S_DASH))->keys;
    if (!k) return;
    uint32_t i = 0;
    if (k[0] == 0xff) return;
    do {
        i++;
        ccall<void>(F_KeyEnable, dash_at(UI_G32(S_DASH))->keys[i - 1]);
    } while (dash_at(UI_G32(S_DASH))->keys[i] != 0xff);
}
PORT_FN(0x00401fd0, "disable_keys", disable_keys_c, fp_keys)
static void __cdecl enable_keys_c() { ccall<void>(F_KeyEnableAll); }
PORT_FN(0x00402040, "enable_keys", enable_keys_c, fp_keys)

// game_loop: a frame at a time until the game state has said "over" ten times; the physics unpaused after ten frames
static void __cdecl game_loop_c() {
    gxCanvas cv;
    int32_t warm = 10, left = 10;                                   // esi, edi
    do {
        if (ccall<int32_t>(F_GetGameState) == 3) {
            UI_G8(S_MAIN_DONE) = 1;
            left--;
        }
        if (warm < 0) {
            if (UI_G8(S_MAIN_DONE)) goto over;
            ccall<void>(F_process_keys);
        } else {
            const int32_t w = warm--;
            if (w == 0) {
                ccall<void>(F_MultiWorldIsLive);
                ccall<void>(F_PhysicsUnpause);
                ccall<void>(F_DashboardReset);
            }
        }
        if (UI_G8(S_MAIN_DONE)) {
        over:
            UI_G32(S_CAMERA) = 0xc;
            ccall<void>(F_PhysicsAbortRace);
        }
        if (UI_G8(S_RESTART)) ccall<void>(F_restart_race);
        if (UI_G32(S_CAMERA) == 0xb) ccall<void>(F_move_blimp);
        ccall<void>(F_CameraSetType, UI_G32(S_CAMERA));
        if (ccall<int32_t>(F_PhysicsGetSpeed) != 0) {
            ccall<void>(F_WorldUpdate);
            ccall<void>(uit::F_mrBeginFrame);
            {
                const uint32_t d = (uint32_t)UI_G32(S_DASH);
                ccall<void>(F_WorldDraw, (d & 0xffffff00u) | (d != 0 ? 1u : 0u));   // (al: the dashboard isn't 0)
            }
            if (auto fn = dash_at(UI_G32(S_DASH))->draw3d) fn();
            ccall<void>(uit::F_mrEndFrame);
            ccall<void>(F_TelemetrySetUpdate, 0);
            ccall<void>(F_AIFGUpdate);
            ccall<void>(F_PhysicsReadControls);
            ccall<void>(F_PhysicsMaybeRun);
            ccall<void>(F_DashboardAlwaysUpdate);
            int32_t d = UI_G32(S_DASH);
            bool draw = true;
            if (d == 4) {
                const uint8_t must = ccall<uint8_t>(F_DashboardMustDraw);
                d = UI_G32(S_DASH);
                if (!must) {
                    const uint8_t esc = ccall<uint8_t>(F_EscapeMenuActive);
                    d = UI_G32(S_DASH);
                    draw = esc != 0;
                }
            }
            if (draw) {
                if (auto fn = dash_at(d)->update) fn();
                if (ccall<uint8_t>(uit::F_gxGrabScreen, &cv)) {
                    ccall<gxCanvas*>(uit::F_gxSetCanvas, &cv);
                    if (UI_G32(S_DASH) == 0) ccall<void>(uit::F_gxSetClip, &cv, 0, 0x40, 0x280, 0x1e0);
                    ccall<void>(F_WorldDraw2D, &cv);
                    int32_t d2 = UI_G32(S_DASH);
                    if (d2 == 0) {
                        ccall<void>(uit::F_gxRestoreClip, &cv);
                        d2 = UI_G32(S_DASH);
                    }
                    if (auto fn = dash_at(d2)->draw2d) fn(&cv);
                    ccall<void>(F_CountdownDraw);
                    if (ccall<uint8_t>(F_EscapeMenuActive)) ccall<void>(F_EscapeMenuDraw, &cv);
                    ccall<void>(uit::F_gxReleaseScreen);
                }
            }
        } else {
            ccall<void>(F_PhysicsReadControls);
            ccall<void>(F_PhysicsMaybeRun);
        }
        ccall<void>(F_MultiFGTick);
        if (ccall<int32_t>(F_PhysicsGetSpeed) == 1 && warm <= 0) ccall<void>(uit::F_gxFlip);
    } while (left > 0);
}
PORT_FN(0x00402050, "game_loop", game_loop_c, fp_race_loop)

// process_keys: the escape menus built on first use; then one key
static void __cdecl process_keys_c() {
    rb_xl_once(S_ESC_GUARD, 0x01, S_XL_RESUME, 0x004e3af8, 0x00402c20);
    rb_xl_once(S_ESC_GUARD, 0x02, S_XL_RESTART, 0x004e3b0c, 0x00402c10);
    rb_xl_once(S_ESC_GUARD, 0x04, S_XL_END_RACE, 0x004e3b20, 0x00402c00);
    rb_xl_once(S_ESC_GUARD, 0x08, S_XL_DISCONNECT, 0x004e3b34, 0x00402bf0);
    if (UI_G32(S_CAMERA) != 0xc) UI_G32(S_CAMERA_SAVED) = UI_G32(S_CAMERA);
    const uint32_t k666 = 0xfff5d3d6;                               // -666666: an item that isn't a choice
    if (!(UI_G8(S_ESC_GUARD) & 0x10)) {                             // resume, restart, end race
        UI_G8(S_ESC_GUARD) = (uint8_t)(UI_G8(S_ESC_GUARD) | 0x10);
        volatile uint32_t* e = (volatile uint32_t*)(uintptr_t)S_ESC_SINGLE;
        e[0] = (uint32_t)(uintptr_t)xlate(S_XL_RESUME);
        e[1] = k666;
        e[2] = (uint32_t)(uintptr_t)xlate(S_XL_RESTART);
        e[3] = 0;
        e[4] = (uint32_t)(uintptr_t)xlate(S_XL_END_RACE);
        e[5] = 1; e[6] = 0; e[7] = 0; e[8] = 3; e[9] = k666;
        e[10] = F_escape_enter; e[11] = F_escape_leave; e[12] = F_escape_callback;
    }
    if (!(UI_G8(S_ESC_GUARD) & 0x20)) {                             // resume, end race
        UI_G8(S_ESC_GUARD) = (uint8_t)(UI_G8(S_ESC_GUARD) | 0x20);
        volatile uint32_t* e = (volatile uint32_t*)(uintptr_t)S_ESC_SOLO;
        e[0] = (uint32_t)(uintptr_t)xlate(S_XL_RESUME);
        e[1] = k666;
        const uint32_t t = (uint32_t)(uintptr_t)xlate(S_XL_END_RACE);
        e[3] = 1;
        e[2] = t;
        e[4] = 0; e[5] = 0; e[6] = 0; e[7] = 0; e[8] = 2; e[9] = k666;
        e[10] = F_escape_enter; e[11] = F_escape_leave; e[12] = F_escape_callback;
    }
    if (!(UI_G8(S_ESC_GUARD) & 0x40)) {                             // the server's: resume, restart, end race
        UI_G8(S_ESC_GUARD) = (uint8_t)(UI_G8(S_ESC_GUARD) | 0x40);
        volatile uint32_t* e = (volatile uint32_t*)(uintptr_t)S_ESC_SERVER;
        e[0] = (uint32_t)(uintptr_t)xlate(S_XL_RESUME);
        e[1] = k666;
        e[2] = (uint32_t)(uintptr_t)xlate(S_XL_RESTART);
        e[3] = 2;
        e[4] = (uint32_t)(uintptr_t)xlate(S_XL_END_RACE);
        e[5] = 3; e[6] = 0; e[7] = 0; e[9] = k666;
        e[10] = F_escape_enter; e[11] = F_escape_leave; e[12] = F_escape_callback;
        e[8] = 3;
    }
    if (!(UI_G8(S_ESC_GUARD) & 0x80)) {                             // the client's: resume, disconnect
        UI_G8(S_ESC_GUARD) = (uint8_t)(UI_G8(S_ESC_GUARD) | 0x80);
        volatile uint32_t* e = (volatile uint32_t*)(uintptr_t)S_ESC_CLIENT;
        e[0] = (uint32_t)(uintptr_t)xlate(S_XL_RESUME);
        e[1] = k666;
        const uint32_t t = (uint32_t)(uintptr_t)xlate(S_XL_DISCONNECT);
        e[3] = 3;
        e[2] = t;
        e[4] = 0; e[5] = 0; e[6] = 0; e[7] = 0; e[8] = 2; e[9] = k666;
        e[10] = F_escape_enter; e[11] = F_escape_leave; e[12] = F_escape_callback;
    }
    if (!ccall<uint8_t>(uit::F_KeyHit)) return;
    const uint16_t key = ccall<uint16_t>(uit::F_KeyGet);
    if (ccall<uint8_t>(F_EscapeMenuActive)) {
        ccall<void>(F_EscapeMenuKey, (uint32_t)key);
        return;
    }
    if (auto fn = dash_at(UI_G32(S_DASH))->key)
        if (fn(key)) return;
    if (ccall<uint8_t>(F_is_pause_key, (uint32_t)key)) {
        if (ccall<uint8_t>(F_MultiEnabled)) return;
        const uint8_t now = UI_G8(S_PAUSED) == 0 ? 1 : 0;
        UI_G8(S_PAUSED) = now;
        if (now) {
            ccall<void>(F_PhysicsPause);
            ccall<void>(F_SoundMuteCars);
        } else {
            ccall<void>(F_PhysicsUnpause);
            ccall<void>(F_SoundUnMuteCars);
        }
        return;
    }
    const uint32_t k = key;
    if (k == 0x1b) {                                                // Esc
        if (ccall<uint8_t>(F_MultiEnabled)) {
            if (ccall<uint8_t>(F_MultiIsServer)) {
                if (ccall<int32_t>(F_GetGameState) == 1) ccall<void>(F_EscapeMenuDo, (const void*)(uintptr_t)S_ESC_SERVER);
            } else {
                ccall<void>(F_EscapeMenuDo, (const void*)(uintptr_t)S_ESC_CLIENT);
            }
            return;
        }
        if (ccall<int32_t>(F_WorldGetPlayerCar) != -1) {
            const int32_t car = ccall<int32_t>(F_WorldGetPlayerCar);
            const void* deity = UI_GP(const void, S_DEITY);
            if (!vcall<uint8_t>(deity, 0x40, car)) {
                const uint8_t* go = ccall<const uint8_t*>(F_WorldGameOptions);
                if (*(const volatile int32_t*)(go + 0x10) >= 4) ccall<void>(F_EscapeMenuDo, (const void*)(uintptr_t)S_ESC_SOLO);
                else ccall<void>(F_EscapeMenuDo, (const void*)(uintptr_t)S_ESC_SINGLE);
                return;
            }
        }
        ccall<void>(F_escape_callback, 1);
        return;
    }
    if (k == 0x31) { ccall<void>(F_set_dash_type, 0); return; }
    if (k >= 0x32 && k <= 0x36) {
        static const int32_t dash_of[5] = {2, 1, 3, 4, 5};
        ccall<void>(F_set_dash_type, dash_of[k - 0x32]);
        return;
    }
    if (k == 0x121) { ccall<void>(F_WorldNextFocusCar); return; }
    if (k == 0x122) { ccall<void>(F_WorldPrevFocusCar); return; }
    if (k == 0x124) { ccall<void>(F_WorldSetFocusCar, ccall<int32_t>(F_WorldGetPlayerCar)); return; }
    if (k == 0x170) { UI_G32(S_CAMERA) = 0; return; }
    if ((k >= 0x171 && k <= 0x17a)) { UI_G32(S_CAMERA) = (int32_t)(k - 0x170); return; }
    if (k == 0x17b || k == 0x37b) {                                 // the blimp: again copies it to the clipboard
        if (UI_G32(S_CAMERA) == 0xb) ccall<uint8_t>(F_copy_blimp_to_clipboard);
        else UI_G32(S_CAMERA) = 0xb;
    }
}
static void fp_process_keys(Footprint& f) { f.replay_only = "runs the escape menu, the dashboards' keys, pause (and builds its Xlators)"; }
PORT_FN(0x00402260, "process_keys", process_keys_c, fp_process_keys)

// copy_blimp_to_clipboard: the blimp's frame as base64 text (for -location)
typedef int(__stdcall* OpenClipboard_t)(void*);
typedef int(__stdcall* EmptyClipboard_t)();
typedef void*(__stdcall* GlobalAlloc_t)(uint32_t, uint32_t);
typedef void*(__stdcall* GlobalLock_t)(void*);
typedef int(__stdcall* GlobalUnlock_t)(void*);
typedef void*(__stdcall* SetClipboardData_t)(uint32_t, void*);
typedef int(__stdcall* CloseClipboard_t)();
#define IAT(T, a) (*(T volatile*)(uintptr_t)(a))
static uint8_t __cdecl copy_blimp_to_clipboard_c() {
    uint8_t cf[0x18];
    char text[0x100];
    uint8_t ok = 0;
    ccall<void>(F_convert_frame_out, (void*)cf, (const void*)(uintptr_t)S_BLIMP_FRAME);
    if (ccall<uint8_t>(F_MemToBase64, (char*)text, 0x100, (const void*)cf, 0x18)) {
        void* hwnd = ccall<void*>(F_Win32GetWindow);
        if (IAT(OpenClipboard_t, IAT_OpenClipboard)(hwnd)) {
            if (IAT(EmptyClipboard_t, IAT_EmptyClipboard)()) {
                void* h = IAT(GlobalAlloc_t, IAT_GlobalAlloc)(0x2002, 0x100);
                if (h) {
                    void* p = IAT(GlobalLock_t, IAT_GlobalLock)(h);
                    crt_copy(p, text, 0x100);
                    ok = 1;
                    IAT(GlobalUnlock_t, IAT_GlobalUnlock)(h);
                    IAT(SetClipboardData_t, IAT_SetClipboardData)(1, h);
                }
            }
            IAT(CloseClipboard_t, IAT_CloseClipboard)();
        }
    }
    UI_LogReport(CP(0x004e3b64), ok ? CP(0x004e3b50) : CP(0x004e3b5c));
    return ok;
}
static void fp_copy_blimp_to_clipboard(Footprint& f) { f.replay_only = "writes the clipboard (allocates)"; }
PORT_FN(0x00402920, "copy_blimp_to_clipboard", copy_blimp_to_clipboard_c, fp_copy_blimp_to_clipboard)

// convert_frame(CompactFrame&, Frame const&): the position, and the rotation as its axis * angle (the quaternion from the
// matrix, then acos -- the game's CRT, whose result stays in a register for fsin: that sequence in asm)
static const float k_eps = 1.1920928955078125e-07f;                  // 0x34000000
static __forceinline bool acos_sin(const volatile float* w, volatile float* ha, volatile float* sn) {
    uint16_t sw;
    __asm {
        mov  eax, w
        fld  dword ptr [eax]
        mov  eax, 0x004cf07c                                        // _CIacos
        call eax
        mov  ecx, ha
        fst  dword ptr [ecx]
        fsin
        mov  ecx, sn
        fst  dword ptr [ecx]
        fabs
        fcomp k_eps
        fnstsw sw
    }
    return (sw & 0x4100) != 0;                                      // test ah, 0x41: |sin| <= eps, or unordered
}
static void __cdecl convert_frame_out_c(RbCompactFrame* out, const RbFrame* in) {
    volatile uint32_t* o = (volatile uint32_t*)out;
    const volatile uint32_t* ip = (const volatile uint32_t*)in;
    o[0] = ip[9];
    o[1] = ip[10];
    o[2] = ip[11];
    const volatile float* m = in->m;
    volatile float q[3], w;                                         // [esp+0x18..0x20], [esp+0x24]
    volatile float tr;
    const double t = (D(m[8]) + D(m[4])) + D(m[0]);
    st_f(&tr, t);
    if (t > D(0.0f)) {
        const double s = x87_sqrt(D(tr) + D(1.0f));
        st_f(&w, D(0.5f) * s);
        const double k = D(0.5f) / s;
        st_f(&q[0], (D(m[5]) - D(m[7])) * k);
        st_f(&q[1], (D(m[6]) - D(m[2])) * k);
        st_f(&q[2], k * (D(m[1]) - D(m[3])));
    } else {
        int32_t i = 0;
        if (m[4] > m[0]) i = 1;
        if (!(m[i * 4] >= m[8])) i = 2;
        const int32_t j = (i + 1) % 3;
        const int32_t kk = (j + 1) % 3;
        const double s = x87_sqrt((D(m[i * 4]) - (D(m[j * 4]) + D(m[kk * 4]))) + D(1.0f));
        st_f(&q[i], D(0.5f) * s);
        const double k = D(0.5f) / s;
        st_f(&w, (D(m[3 * j + kk]) - D(m[3 * kk + j])) * k);
        st_f(&q[j], (D(m[3 * i + j]) + D(m[3 * j + i])) * k);
        st_f(&q[kk], k * (D(m[3 * kk + i]) + D(m[3 * i + kk])));
    }
    volatile float ha, sn;
    if (acos_sin(&w, &ha, &sn)) {
        o[3] = 0; o[4] = 0; o[5] = 0;
        return;
    }
    const double f = (D(ha) * D(2.0f)) / D(sn);
    st_f(&out->r[0], D(q[0]) * f);
    st_f(&out->r[1], D(q[1]) * f);
    st_f(&out->r[2], f * D(q[2]));
}
static void fp_convert_frame_out(Footprint& f, RbCompactFrame* out, const RbFrame*) {
    f.add(out, sizeof(RbCompactFrame), "the compact frame");
    UI_FP(f, 0x005d5600, 9, "the C runtime's transcendental dispatch scratch (_CIacos)");
    UI_FP(f, 0x00502730, 4, "the C runtime's errno (acos out of its domain)");
}
PORT_FN(0x004029e0, "convert_frame(CompactFrame&,Frame const&)", convert_frame_out_c, fp_convert_frame_out)

static void __cdecl set_dash_type_c(int32_t t) {
    UI_G32(S_DASH) = t;
    ccall<void>(F_DashboardReset);
    ccall<void>(F_disable_keys);
}
static void fp_set_dash_type(Footprint& f, int32_t) { f.replay_only = "resets the dashboard, enables its keys"; }
PORT_FN(0x00402bd0, "set_dash_type", set_dash_type_c, fp_set_dash_type)

static void __cdecl restart_race_c() {
    UI_G8(S_RESTART) = 0;
    if (!ccall<uint8_t>(F_MultiEnabled)) ccall<void>(F_PhysicsRestart);
    else ccall<void>(F_MultiRestartRace);
}
PORT_FN(0x00402c30, "restart_race", restart_race_c, fp_physics_state)

// the escape menu's enter / leave / callback
static void __cdecl escape_enter_c() {
    if (!UI_G8(S_PAUSED) && !ccall<uint8_t>(F_MultiEnabled)) ccall<void>(F_PhysicsPause);
    ccall<void>(F_SoundMuteCars);
    ccall<void>(F_enable_keys);
}
PORT_FN(0x00402c50, "escape_enter", escape_enter_c, fp_physics_state)
static void __cdecl escape_leave_c() {
    ccall<void>(F_disable_keys);
    if (UI_G8(S_PAUSED)) return;
    if (UI_G8(S_MAIN_DONE)) return;
    if (!ccall<uint8_t>(F_MultiEnabled)) ccall<void>(F_PhysicsUnpause);
    ccall<void>(F_SoundUnMuteCars);
}
PORT_FN(0x00402c80, "escape_leave", escape_leave_c, fp_physics_state)
static void __cdecl escape_callback_c(int32_t c) {
    switch ((uint32_t)c) {
    case 0: case 2:
        UI_G8(S_RESTART) = 1;
        UI_G8(S_MAIN_DONE) = 0;
        break;
    case 1:
        UI_G8(S_MAIN_DONE) = 1;
        break;
    case 3:
        ccall<uint8_t>(F_MultiMenuEscape);
        break;
    }
}
static void fp_escape_callback(Footprint& f, int32_t c) {
    if ((uint32_t)c == 3) { f.replay_only = "the multiplayer's escape menu"; return; }
    UI_FP(f, S_RESTART, 1, "main.obj restart flag");
    UI_FP(f, S_MAIN_DONE, 1, "main.obj race over");
}
PORT_FN(0x00402cb0, "escape_callback", escape_callback_c, fp_escape_callback)

// the programmers' test modes
static void __cdecl rich_g_c() {
    ccall<void>(F_app_begin);
    ccall<void>(F_app_end);
}
PORT_FN(0x00402cf0, "rich_g", rich_g_c, fp_runs_game)
static void __cdecl dave_p_c() {}
PORT_FN(0x00402d00, "dave_p", dave_p_c, fp_pure)

// a one-car World on the stack (World::World, then the fields these two set), raced
static void test_world(uint8_t* W, const char* track, int32_t car_type, const char* car, int32_t realism, uint8_t reverse) {
    crt_strcpy((char*)(W + 8), track);
    *(volatile int32_t*)(W + 0xca8) = 1;
    *(volatile int32_t*)(W + 0x28) = car_type;
    crt_strcpy((char*)(W + 0x2c), CP(0x004e3b7c));                  // "Joe-Bob"
    crt_strcpy((char*)(W + 0x39), car);
    *(volatile int32_t*)(W + 0xcac) = realism;
    *(volatile int32_t*)(W + 0xcb0) = 1;
    *(volatile int32_t*)(W + 0xcb4) = 0;
    *(volatile int32_t*)(W + 0xcb8) = 0;
    *(volatile int32_t*)(W + 0xcbc) = 3;
    *(volatile int32_t*)(W + 0xcc0) = 0x14;
    *(volatile int32_t*)(W + 0xcc4) = 0;
    *(volatile uint8_t*)(W + 0xcd0) = 0;
    *(volatile uint8_t*)(W + 0xcd1) = reverse;
    *(volatile int32_t*)(W + 0xccc) = 0;
}
static void __cdecl blimp_jump_c() {
    alignas(4) uint8_t W[0xcd4];
    // FIX CANDIDATE: -location's track name (the command line's, any length) is copied into the World's 0x20 bytes
    UI_G32(S_CAMERA) = 0xb;
    tcall<void*>(F_World_ctor, (void*)W);
    test_world(W, CP(S_BLIMP_TRACK), 0, CP(0x004e3b84), 0, 1);
    ccall<void>(F_app_begin);
    crt_copy((void*)(uintptr_t)S_BLIMP_FRAME, (const void*)(uintptr_t)S_BLIMP_START, 0x30);
    ccall<void>(F_LoadRace, (void*)W);
    ccall<void>(F_DoRace, (void*)W, (void*)0, (void*)0);
    ccall<void>(F_UnloadRace, (const void*)W);
    ccall<void>(F_app_end);
}
PORT_FN(0x00402d10, "blimp_jump", blimp_jump_c, fp_runs_game)
static void __cdecl dave_b_c() {
    alignas(4) uint8_t W[0xcd4];
    UI_G32(S_CAMERA) = 4;
    tcall<void*>(F_World_ctor, (void*)W);
    test_world(W, CP(0x004e3b8c), 1, CP(0x004e3b9c), 2, 0);
    ccall<void>(F_app_begin);
    ccall<void>(F_LoadRace, (void*)W);
    ccall<void>(F_DoRace, (void*)W, (void*)0, (void*)0);
    ccall<void>(F_UnloadRace, (const void*)W);
    ccall<void>(F_app_end);
}
PORT_FN(0x00402e50, "dave_b", dave_b_c, fp_runs_game)

// ---- the matrix helpers ---------------------------------------------------------------------------------------------------
// MatrixMakeYaw / Pitch / Roll: a rotation about y / x / z (the angle's float passed as its bits: the original loads it)
static void __cdecl MatrixMakeYaw_c(RbMatrix* m, float a) {
    const float c = x87_cos_f(a), s = x87_sin_f(a);
    volatile uint32_t* u = (volatile uint32_t*)m->m;
    u[1] = 0; u[3] = 0; u[4] = 0x3f800000; u[5] = 0; u[7] = 0;
    m->m[0] = c;
    m->m[2] = -s;
    m->m[6] = s;
    m->m[8] = c;
}
static void fp_matrix_make(Footprint& f, RbMatrix* m, float) { f.pure = true; f.add(m, sizeof(RbMatrix), "the matrix"); }
PORT_FN(0x00402f80, "MatrixMakeYaw", MatrixMakeYaw_c, fp_matrix_make)
static void __cdecl MatrixMakePitch_c(RbMatrix* m, float a) {
    const float c = x87_cos_f(a), s = x87_sin_f(a);
    volatile uint32_t* u = (volatile uint32_t*)m->m;
    u[0] = 0x3f800000; u[3] = 0; u[2] = 0; u[1] = 0; u[6] = 0;
    m->m[4] = c;
    m->m[5] = s;
    m->m[7] = -s;
    m->m[8] = c;
}
PORT_FN(0x00402fc0, "MatrixMakePitch", MatrixMakePitch_c, fp_matrix_make)
static void __cdecl MatrixMakeRoll_c(RbMatrix* m, float a) {
    const float c = x87_cos_f(a), s = x87_sin_f(a);
    volatile uint32_t* u = (volatile uint32_t*)m->m;
    u[2] = 0; u[5] = 0; u[6] = 0; u[7] = 0; u[8] = 0x3f800000;
    m->m[0] = c;
    m->m[1] = s;
    m->m[3] = -s;
    m->m[4] = c;
}
PORT_FN(0x00403000, "MatrixMakeRoll", MatrixMakeRoll_c, fp_matrix_make)

// MatrixConcat: d = a * b. When d is a or b, everything is read before d is written; otherwise each element is stored as
// it's made (so a d overlapping a or b in part reads what's been written).
static void __cdecl MatrixConcat_c(RbMatrix* dm, const RbMatrix* am, const RbMatrix* bm) {
    volatile float* d = dm->m;
    const volatile float* a = am->m;
    const volatile float* b = bm->m;
    if (am != dm && bm != dm) {
        st_f(&d[0], (D(a[0]) * D(b[0]) + D(b[6]) * D(a[2])) + D(a[1]) * D(b[3]));
        st_f(&d[3], (D(a[5]) * D(b[6]) + D(b[3]) * D(a[4])) + D(a[3]) * D(b[0]));
        st_f(&d[6], (D(b[6]) * D(a[8]) + D(b[3]) * D(a[7])) + D(a[6]) * D(b[0]));
        st_f(&d[1], (D(a[0]) * D(b[1]) + D(a[2]) * D(b[7])) + D(a[1]) * D(b[4]));
        st_f(&d[4], (D(a[5]) * D(b[7]) + D(b[1]) * D(a[3])) + D(b[4]) * D(a[4]));
        st_f(&d[7], (D(b[1]) * D(a[6]) + D(b[7]) * D(a[8])) + D(a[7]) * D(b[4]));
        st_f(&d[2], (D(a[0]) * D(b[2]) + D(b[5]) * D(a[1])) + D(a[2]) * D(b[8]));
        st_f(&d[5], (D(b[5]) * D(a[4]) + D(b[2]) * D(a[3])) + D(a[5]) * D(b[8]));
        st_f(&d[8], (D(b[5]) * D(a[7]) + D(b[2]) * D(a[6])) + D(a[8]) * D(b[8]));
        return;
    }
    const double t3 = (D(a[5]) * D(b[6]) + D(b[3]) * D(a[4])) + D(a[3]) * D(b[0]);
    const double t6 = (D(b[6]) * D(a[8]) + D(b[3]) * D(a[7])) + D(a[6]) * D(b[0]);
    const double t1 = (D(a[0]) * D(b[1]) + D(a[2]) * D(b[7])) + D(a[1]) * D(b[4]);
    const double t4 = (D(a[5]) * D(b[7]) + D(b[1]) * D(a[3])) + D(b[4]) * D(a[4]);
    const double t7 = (D(b[1]) * D(a[6]) + D(b[7]) * D(a[8])) + D(a[7]) * D(b[4]);
    volatile float t2, t5, t8;
    st_f(&t2, (D(a[0]) * D(b[2]) + D(b[5]) * D(a[1])) + D(a[2]) * D(b[8]));
    st_f(&t5, (D(b[5]) * D(a[4]) + D(b[2]) * D(a[3])) + D(a[5]) * D(b[8]));
    st_f(&t8, (D(b[5]) * D(a[7]) + D(b[2]) * D(a[6])) + D(a[8]) * D(b[8]));
    const double t0 = (D(a[0]) * D(b[0]) + D(b[6]) * D(a[2])) + D(a[1]) * D(b[3]);
    st_f(&d[0], t0);
    st_f(&d[3], t3);
    st_f(&d[6], t6);
    st_f(&d[1], t1);
    st_f(&d[4], t4);
    st_f(&d[7], t7);
    volatile uint32_t* du = (volatile uint32_t*)d;
    du[2] = *(volatile uint32_t*)&t2;
    du[5] = *(volatile uint32_t*)&t5;
    du[8] = *(volatile uint32_t*)&t8;
}
static void fp_MatrixConcat(Footprint& f, RbMatrix* d, const RbMatrix*, const RbMatrix*) {
    f.pure = true;
    f.add(d, sizeof(RbMatrix), "the matrix");
}
PORT_FN(0x00403040, "MatrixConcat", MatrixConcat_c, fp_MatrixConcat)

}  // namespace root_main
}  // namespace
