// career_main.cpp -- M3 UI stage, step U4 (group A): the career's core, rewritten faithfully (library `career`: career.obj,
// season.obj; the screens -- chooser.obj, events.obj, postseas.obj, ranking.obj, testing.obj -- are career_screens.cpp).
//
//   career.obj: CareerDo (the career's loop: the chooser, then a career until it's left) and career_main (a slot loaded,
//   its class's season, the log, then the career's menu until Back: events, testing, the upgrade shop, the ranking, save);
//   do_event (one event: the World built from the season's event, qualifying -- the AI's times from their skills and a
//   random factor, sorted --, the race, the points and purses, the season's end: the rank's award and prize, a new class);
//   the career's menu (career_menu and its callbacks), the career file (career_load / career_save: a 16-byte header and
//   the CareerInfo xor'd with 0xa4, its bytes' sum as the checksum), the getters, the awards, the ranking (qsort by points,
//   then by the latest results, then by name), CareerStatus (the menus' week / funds / season lines).
//   season.obj: CareerSeasonGet (the resource, every prize checked for a translation), could_xlate_money, CareerSeasonForget.
//
// Written from the v1.0 disassembly as the U2 / U3 files are: every call in the original's order by its v1.0 address,
// virtual calls through the vtable, the inline string code as the original runs it (ui_types.h). A screen's item list is
// built in a frame laid out as the original's (F: esp after the prologue's pushes), an item the original builds with
// UIDialogItem's constructor built with it, one it writes in place written with the same dwords, so the pointers the
// dialog keeps into the frame are to the same locals. do_event keeps its four Worlds, the Results and the event in its
// frame at the original's offsets. x87: register values double, stored ones float, the original's grouping and constants'
// widths; comparisons with the original's NaN outcome; a float the original moves with integer instructions moved by its
// bits.
//
// Footprints (main thread): the getters write nothing; the setters their statics (the CareerInfo, the funds, the awards
// row, the file name's buffer, the ranks); CareerStatus::Callback its texts and its Xlator. replay_only: whatever loads,
// saves, allocates or frees (the career file, the season resource, the log, the styles, notifications, widgets), builds a
// function-local Xlator the first time (atexit), and whatever runs a screen or a dialog (CareerDo, career_main, do_event,
// career_menu and its callbacks, CareerDestroySlot) -- the session replay is their in-game check.
//
// Fixes (// FIX:, docs/FIXES.md "Career"; none reached with the stock game, English text and a normal user directory):
// get_career_filename gives "" for a user directory too long for its 0x108-byte buffer (the career files then can't be
// read or made), and career_save doesn't make a career directory too long for its 0x104 bytes; set_class keeps the class's
// name to its 0x30-byte static (47 characters); CareerStatus::Callback keeps its week line to 63 characters;
// driver_compare compares a driver's name of 256 or more characters (a modded drivers.res) where it is instead of copying
// it into its frame's 0x100 bytes. Every other input gives the original's bits.
//
// FIX CANDIDATEs (left faithful, marked in place; "ordinary play" = the stock game, English, a normal user directory):
// CareerGiveAward indexes the awards by the class unchecked (a damaged career file); do_event and sort_carlist index by
// the driver map and the event unchecked (a damaged career file, a season of more than 32 events); credit_account reads
// before the Results when no car is the player's (never: the map always holds 7); CareerSeasonGet reads a missing season
// resource through 0 (a missing file); CareerLog formats into 0x400 bytes (a player name is 13 characters: not reached);
// career_menu's CareerStatus (as the other screens') is added before its texts are formatted, its StaticTexts copying
// stack garbage (every time; harmless unless the garbage runs 256+ bytes).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "career_types.h"
#include "fix_paths.h"

namespace {
namespace career_main {
using namespace uit;
using namespace car;

#define D(x) ((double)(x))
static __forceinline float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
typedef void(__cdecl* Log_v)(const char*, ...);
#define CAREER_LOG ((Log_v)(uintptr_t)F_CareerLog)
typedef int(__cdecl* Sprintf_v)(char*, const char*, ...);
#define SPRINTF ((Sprintf_v)(uintptr_t)uit::F_sprintf)
// an item written in place: its 14 dwords (the original writes every one)
static __forceinline void put14(uint8_t* F, uint32_t off, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w,
                                uint32_t h, uint32_t text, uint32_t i1c, uint32_t data, uint32_t style) {
    volatile uint32_t* d = (volatile uint32_t*)(F + off);
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text; d[7] = i1c; d[8] = data; d[9] = style;
    d[10] = 0; d[11] = 0; d[12] = 0; d[13] = 0;
}
static __forceinline void end_item(uint8_t* F, uint32_t off) { crt_copy(F + off, (const void*)(uintptr_t)S_END_ITEM, 0x38); }
static __forceinline void copy_dwords(void* d, const void* s, uint32_t n) { crt_copy(d, s, n * 4); }   // rep movsd
static __forceinline void zero_dwords(uint32_t at, uint32_t n) {                                      // rep stosd
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)at;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

// ---- footprint helpers --------------------------------------------------------------------------------------------------
static void fp_none(Footprint&) {}
static void fp_none_i(Footprint&, int32_t) {}
static void fp_screen(Footprint& f) { f.replay_only = "runs the career's screens (dialogs, the races, the career file)"; }
static void fp_screen_i(Footprint& f, int32_t) { f.replay_only = "runs a screen or a dialog of the career"; }
#define FP_INFO(f) UI_FP(f, S_INFO, S_INFO_SIZE, "the CareerInfo")

// =========================================================================================================================
// the getters
// =========================================================================================================================
static uint8_t* __cdecl CareerGetInfo_c() { return (uint8_t*)(uintptr_t)S_INFO; }
PORT_FN(0x004bc470, "CareerGetInfo", CareerGetInfo_c, fp_none)
static const uint8_t* __cdecl CareerGetSeason_c() { return UI_GP(const uint8_t, S_SEASON); }
PORT_FN(0x004bc480, "CareerGetSeason", CareerGetSeason_c, fp_none)
static const void* __cdecl CareerGetUpgradeSet_c() { return UI_GP(const void, S_UPGRADE_SET); }
PORT_FN(0x004bc490, "CareerGetUpgradeSet", CareerGetUpgradeSet_c, fp_none)
static const char* __cdecl CareerGetCarName_c() { return CA_CP(0x004ff794); }                    // "viper"
PORT_FN(0x004bd4f0, "CareerGetCarName", CareerGetCarName_c, fp_none)

// CareerGiveAward: the award into the class's row's first empty slot; a full row shifts down, the newest last
// FIX CANDIDATE: the class indexes the rows unchecked (0..3; a damaged career file writes elsewhere). Not in ordinary play.
static void __cdecl CareerGiveAward_c(int32_t award) {
    volatile int32_t* row = (volatile int32_t*)(uintptr_t)(S_AWARDS + ((uint32_t)CA_G32(S_CLASS) << 5));
    for (int32_t i = 0; i < 8; i++)
        if (row[i] == 0) { row[i] = award; return; }
    for (int32_t i = 0; i < 7; i++) row[i] = row[i + 1];
    row[7] = award;
}
static void fp_CareerGiveAward(Footprint& f, int32_t) {
    UI_FP(f, S_AWARDS + ((uint32_t)CA_G32(S_CLASS) << 5), 0x20, "the class's awards");
}
PORT_FN(0x004bc4a0, "CareerGiveAward", CareerGiveAward_c, fp_CareerGiveAward)

// CareerLog: a line to c:\career.log (when it opened), CR LF appended. Variadic: registered with 32 dwords after the
// format (ui_dialog.cpp's Va: on x86 __cdecl the caller's arguments lie in order on the stack, so the va_list is &v).
// FIX CANDIDATE: formats into 0x400 bytes unbounded (its texts: the player's 13-character name, track names). Not reached.
struct Va {
    uint32_t w[32];
    Va() = default;
    explicit Va(uint32_t) {}
};
static void __cdecl CareerLog_c(const char* fmt, Va v) {
    char buf[0x400];
    if (CA_G32(S_LOG) == 0) return;
    ccall<int>(F_vsprintf, (char*)buf, fmt, (void*)&v);
    char* e = buf + crt_strlen(buf);
    *(volatile uint16_t*)e = *(const volatile uint16_t*)(uintptr_t)0x004ff5e8;                  // CR LF
    *(volatile uint8_t*)(e + 2) = *(const volatile uint8_t*)(uintptr_t)0x004ff5ea;
    const uint32_t n = crt_strlen(buf);
    ccall<uint8_t>(F_FileWrite, CA_G32(S_LOG), (const void*)buf, (int32_t)n);
}
static void fp_CareerLog(Footprint& f, const char*, Va) { f.replay_only = "writes the career's log file"; }
PORT_FN(0x004bc4f0, "CareerLog", CareerLog_c, fp_CareerLog)

// =========================================================================================================================
// CareerDo, career_main
// =========================================================================================================================
static void __cdecl CareerDo_c() {
    ccall<void>(F_HackDisable);
    uint8_t done = 0;
    // FIX: the career log went to the root of C: ("c:\career.log", the literal at 0x4ff5ec), which isn't writable without
    // elevation on NT 6+, so there was none; vrmod's writepaths.py makes the literal "log\career.log", relative to the
    // current directory. It is <race.exe's folder>\log\career.log now, the folder made, like the other logs
    // (fix_paths.h: log-2\ for the second copy under [test] two_copies). A folder too long for the file table's 0x100-byte
    // names keeps the literal.
    const char* log_name = CA_CP(0x004ff5ec);
    char log_path[0x100];
    if (VP_FIX && vp_log_path(log_path, sizeof log_path, "career.log")) log_name = log_path;
    CA_G32(S_LOG) = ccall<int32_t>(F_FileAppend, log_name);
    do {
        const int32_t slot = ccall<int32_t>(F_CareerChooser);
        if (slot < 0) done = 1;
        else ccall<void>(F_career_main, slot);
    } while (!done);
    if (CA_G32(S_LOG) != 0) ccall<void>(uit::F_FileClose, (int32_t*)(uintptr_t)S_LOG);
    ccall<void>(F_HackRestore);
}
PORT_FN(0x004bc570, "CareerDo", CareerDo_c, fp_screen)

static void __cdecl career_main_c(int32_t slot) {
    alignas(8) uint8_t F[0x74];                                     // sub esp, 0x68; 3 pushes
    CA_GU32(S_CAREER_UPGRADES) = S_UPGRADES_IN_INFO;
    CA_G32(S_SLOT) = slot;
    CA_G8(S_5CFE58) = 0;
    CA_G32(S_SEASON) = 0;
    zero_dwords(S_INFO, 0x19d);
    zero_dwords(S_INFO_SAVED, 0x19d);
    if (!ccall<uint8_t>(F_career_load, slot, (void*)(uintptr_t)S_INFO)) return;
    copy_dwords((void*)(uintptr_t)S_INFO_SAVED, (const void*)(uintptr_t)S_INFO, 0x19d);
    ccall<void>(F_set_class, CA_G32(S_CLASS));
    CAREER_LOG(CA_CP(0x004ff5fc));                                   // "-----..."
    CAREER_LOG(CA_CP(0x004ff62c), (uint32_t)S_INFO, CA_G32(S_CLASS) + 1);   // "Career: %s  class: %d"
    ccall<void>(F_TimeGetTimeOfDay, (void*)(F + 0x10));
    {
        const uint32_t a = CA_W(F, 0x10) & 0xffff;
        const uint32_t b = *(volatile uint16_t*)(F + 0x12);
        const uint32_t c = CA_W(F, 0x14) & 0xffff;
        ccall<void>(F_LocaleFormatShortDate, (char*)(F + 0x24), 0x50, c, b, a);
    }
    CAREER_LOG(CA_CP(0x004ff644), (const char*)(F + 0x24));           // "Date: %s"
    CA_B(F, 0xf) = 0;
    if (ccall<uint8_t>(F_career_menu, (uint8_t*)(F + 0xf))) {
        do {
            if (CA_B(F, 0xf) != 0) {
                ccall<void>(F_TestingDo);
            } else if (ccall<uint8_t>(F_EventsDo)) {
                for (;;) {
                    const int32_t ev = CA_G32(S_EVENT);
                    if (*UI_GP(volatile int32_t, S_SEASON) <= ev) {
                        UI_LogPanic(CA_CP(0x004ff650));                   // "Can't race--out of events"
                    } else if (!ccall<uint8_t>(F_do_event, ev)) {
                        break;
                    }
                    if (!ccall<uint8_t>(F_EventsDo)) break;
                }
            }
        } while (ccall<uint8_t>(F_career_menu, (uint8_t*)(F + 0xf)));
    }
    if (CA_G32(S_SEASON) != 0) ccall<void>(F_CareerSeasonForget, UI_GP(void, S_SEASON));
    CA_G32(S_CAREER_UPGRADES) = 0;
    ccall<void>(F_AIResetDriverMap);
}
PORT_FN(0x004bc5d0, "career_main", career_main_c, fp_screen_i)

// =========================================================================================================================
// do_event
// =========================================================================================================================
// the qualifying World's and the practice World's player: the first car of the player's type (0) after the first moved
// into slot 0 (F + w: a World in the frame)
static __forceinline void player_first(uint8_t* F, uint32_t w) {
    int32_t i = 1;
    if ((int32_t)CA_W(F, w + 0xca8) > 1) {
        for (; i < (int32_t)CA_W(F, w + 0xca8); i++)
            if (CA_W(F, w + 0x28 + 0xc8u * (uint32_t)i) == 0) {
                copy_dwords(F + w + 0x28, F + w + 0x28 + 0xc8u * (uint32_t)i, 0x32);
                break;
            }
    }
}

// FIX CANDIDATE: the driver map's values and the event index the results are filed under go unchecked (a damaged career
// file, a season of more than 32 events). Not in ordinary play.
static uint8_t __cdecl do_event_c(int32_t ev) {
    alignas(8) uint8_t F[0x34d4];                                   // sub esp, 0x34c4 (alloca_probe); 4 pushes
    // F+0x10..0x13 flags (garage first, done, raced, the result), +0x14 a time, +0x18 the random factor, +0x1c the player's
    // qualifying time, +0x20 a skill, +0x24 the track's base time, +0x28 the event (0x58), +0x80 the prizes ({points, purse}
    // x 8), +0xc0 the Results (0xc4), +0x184 the World, +0xe58 the qualifying World, +0x1b2c the practice World, +0x2800
    // the race's World
    CA_G8(S_NEW_CLASS) = 0;
    if (ccall<uint8_t>(F_CareerIsTestMode) && ccall<uint8_t>(F_ScanDown, 0xfe)) {
        const int32_t next = CA_G32(S_EVENT) + 1;
        if (*UI_GP(volatile int32_t, S_SEASON) > next) {           // test mode, the key held: the event skipped
            CA_G32(S_EVENT) = next;
            CA_G32(S_WEEK) = CA_G32(S_WEEK) + 1;
            CA_G8(S_QUALIFIED) = 0;
            ccall<void>(F_update_ranking);
            CA_G32(0x005cf5a4) = CA_G32(0x005cf5a4) + 1;             // the player's points
            return 1;
        }
    }
    {
        const int32_t e = CA_G32(S_EVENT);
        const uint8_t* season = UI_GP(uint8_t, S_SEASON);
        copy_dwords(F + 0x28, season + (uint32_t)e * 0x58u + 0x24, 0x16);
    }
    tcall<void*>(F_World_ctor, (void*)(F + 0x184));
    ccall<void>(F_get_options, (void*)(F + 0xe30));                 // the World's GameOptions
    crt_strcpy((char*)(F + 0x18c), (const char*)(F + 0x28));      // the World's track: the event's
    {
        const uint8_t dmg = CA_G8(S_DAMAGE);
        const uint32_t laps = CA_W(F, 0x3c);
        const uint8_t rev = CA_B(F, 0x38);
        CA_B(F, 0xe54) = dmg;                                        // World +0xcd0 damage
        CA_W(F, 0xe44) = laps;                                       // +0xcc0 laps
        CA_B(F, 0xe55) = rev;                                        // +0xcd1 reversed
        CA_W(F, 0xe40) = (uint32_t)CA_G32(S_CLASS) + 4;              // +0xcbc AI strength
        CA_W(F, 0xe50) = (uint32_t)CA_G32(S_CLASS) + 3;              // +0xccc
    }
    ccall<void>(F_CareerUnload);
    if (CA_G8(S_QUALIFIED) == 0) {
        for (uint32_t i = 0; i < 8; i++) {
            CA_GU32(S_DRIVER_MAP + 4 * i) = i;
            CA_GU32(S_QUAL_TIMES + 4 * i) = 0;
        }
    }
    ccall<void>(F_AIResetDriverMap);
    ccall<void>(F_GenerateCarList, (void*)(F + 0x184), CA_CP(0x004ff66c), 7);   // "viper"
    crt_strcpy((char*)(F + 0x728), CA_CP(S_INFO));                  // car 7's driver: the player's name
    CA_W(F, 0x7e4) = 0xfffffff7u;                                    // car 7's paint job
    ccall<void>(F_LoadRace, (void*)(F + 0x184));
    CA_B(F, 0x11) = 0;
    CA_B(F, 0x12) = 0;
    CA_B(F, 0x13) = 1;
    CA_B(F, 0x10) = 0;
    do {
        copy_dwords(F + 0x2800, F + 0x184, 0x335);
        ccall<void>(F_AIResetDriverMap);
        if (CA_G8(S_QUALIFIED) != 0) {
            ccall<void>(F_sort_carlist, (void*)(F + 0x2800));
            ccall<void>(F_AISetDriverMap, (void*)(uintptr_t)S_DRIVER_MAP, 8);
        }
        for (uint32_t k = 0; k < 8; k++) {                           // the prizes: the event's points and purse
            const uint32_t pts = CA_W(F, 0x60 + 4 * k);
            const uint32_t purse = CA_W(F, 0x40 + 4 * k);
            CA_W(F, 0x80 + 8 * k) = pts;
            CA_W(F, 0x84 + 8 * k) = purse;
        }
        {
            const uint32_t times = CA_G8(S_QUALIFIED) != 0 ? S_QUAL_TIMES : 0u;
            const uint32_t qual = (times & 0xffffff00u) | (CA_G8(S_QUALIFIED) == 0 ? 1u : 0u);   // (al; eax's upper bytes)
            const int32_t r = ccall<int32_t>(F_PreRaceDo, CA_A(F, 0x2800), CA_W(F, 0x10), (uint32_t)S_UPGRADES_IN_INFO,
                                             qual, times, CA_A(F, 0x80));
            if (r == 0) {
                CA_B(F, 0x11) = 1;
            } else if (r == 1) {
                CA_B(F, 0x10) = 0;
                if (CA_G8(S_QUALIFIED) != 0) {                      // the race
                    if (!ccall<uint8_t>(F_ScanDown, 0xfe)) {
                        const int32_t t0 = ccall<int32_t>(uit::F_PTimeNow);
                        ccall<void>(F_DoRace, (void*)(F + 0x2800), (void*)(F + 0xc0), 0);
                        const int32_t t1 = ccall<int32_t>(uit::F_PTimeNow);
                        CA_G32(S_SECONDS) = CA_G32(S_SECONDS) + (int32_t)((uint32_t)t1 - (uint32_t)t0) / 1000;
                    } else {
                        UI_LogReport(CA_CP(0x004ff674), 1);          // "faking results for place %d"
                        ccall<void>(F_fake_results, 1, (void*)(F + 0xc0));
                    }
                    CA_B(F, 0x12) = 1;
                    CA_B(F, 0x11) = 1;
                } else {                                             // qualifying: the player alone, one lap
                    copy_dwords(F + 0xe58, F + 0x2800, 0x335);
                    player_first(F, 0xe58);
                    CA_W(F, 0x1b00) = 1;                             // the count
                    CA_W(F, 0x1b18) = 1;                             // laps
                    CA_W(F, 0x1b10) = 5;                             // the race type
                    CA_W(F, 0xcc) = 0x447a0000u;                     // the player's time: 1000
                    if (!ccall<uint8_t>(F_ScanDown, 0xfe))
                        ccall<void>(F_DoRace, (void*)(F + 0xe58), (void*)(F + 0xc0), (uint32_t)F_null_postrace);
                    CA_W(F, 0x1c) = CA_W(F, 0xcc);
                    {
                        typedef void(__cdecl * LogD_t)(const char*, const char*, double);
                        ((LogD_t)(uintptr_t)F_CareerLog)(CA_CP(0x004ff690), (const char*)(F + 0xe60),
                                                         (double)*(volatile float*)(F + 0x1c));   // "Qualify: %s %1.1f"
                    }
                    const int32_t tn = ccall<int32_t>(F_GetTrackNumber, (const char*)(F + 0xe60));
                    const int32_t t = (CA_B(F, 0x1b29) != 0 ? 8 : 0) + tn;   // reversed: the second 8
                    const uint8_t* info = ccall<const uint8_t*>(F_AIGetTrackInfo, t);
                    CA_W(F, 0x24) = *(const volatile uint32_t*)(info + 0x10);
                    for (int32_t i = 0; i < 8; i++) {
                        CA_W(F, 0x14) = 0xbf800000u;                 // -1
                        const int32_t r2 = ccall<int32_t>(F_Random, 0xc9) - 0x64;
                        CA_W(F, 0x18) = (uint32_t)r2;
                        const double factor = D(r2) * D(bits_f(0x38d1b717)) + D(1.0f);   // fild; fmul; fadd
                        *(volatile float*)(F + 0x18) = (float)factor;
                        if (i == 7) {
                            CA_W(F, 0x14) = CA_W(F, 0x1c);           // the player's own time
                        } else {
                            const uint8_t* drv = ccall<const uint8_t*>(F_CareerGetDriver, i);
                            const int32_t skill = *(const volatile int32_t*)(drv + (uint32_t)t * 0x18u + 0x98);
                            CA_W(F, 0x20) = (uint32_t)skill;
                            if (skill > 0)
                                *(volatile float*)(F + 0x14) = (float)((D(*(volatile float*)(F + 0x24)) +
                                                                        D(*(volatile float*)(F + 0x20))) *
                                                                       D(*(volatile float*)(F + 0x18)));
                        }
                        if (CA_W(F, 0x14) > 0x80000000u) CA_W(F, 0x14) = 0x43960000u;   // negative: 300
                        CA_GU32(S_QUAL_TIMES + 4 * (uint32_t)i) = CA_W(F, 0x14);
                    }
                    for (uint32_t a = S_QUAL_TIMES; a < S_DRIVER_MAP; a += 4)   // sorted by time (the map with them)
                        for (uint32_t b = a + 4; b < S_DRIVER_MAP; b += 4)
                            if (D(UI_GF(a)) > D(UI_GF(b))) {
                                const uint32_t m = CA_GU32(a + 0x20), n = CA_GU32(b + 0x20);
                                CA_GU32(a + 0x20) = n;
                                CA_GU32(b + 0x20) = m;
                                const uint32_t tb = CA_GU32(b), ta = CA_GU32(a);
                                CA_GU32(a) = tb;
                                CA_GU32(b) = ta;
                            }
                    CA_G8(S_QUALIFIED) = 1;
                }
            } else if (r == 2) {                                     // practice: the player alone
                copy_dwords(F + 0x1b2c, F + 0x2800, 0x335);
                player_first(F, 0x1b2c);
                CA_W(F, 0x27d4) = 1;
                CA_W(F, 0x27e4) = 4;
                ccall<void>(F_DoRace, (void*)(F + 0x1b2c), 0, 0);
                CA_B(F, 0x10) = 1;
            }
        }
        ccall<void>(F_AIResetDriverMap);
    } while (CA_B(F, 0x11) == 0);
    ccall<void>(F_UnloadRace, (const void*)(F + 0x184));
    if (CA_B(F, 0x12) != 0) {
        ccall<void>(F_credit_account, (const void*)(F + 0x28), (const void*)(F + 0xc0), (void*)(uintptr_t)S_DRIVER_MAP);
        for (uint32_t i = 0; i < 8; i++) {
            const int32_t d = CA_G32(S_DRIVER_MAP + 4 * i);
            const int32_t place = (int32_t)CA_W(F, 0xc4 + 0xc * i);
            if (d == 7) CAREER_LOG(CA_CP(0x004ff6a4), (const char*)(F + 0x18c), place);   // "Race: %s place: %d"
            const int32_t pm1 = (int32_t)((uint32_t)place - 1u);
            if (pm1 >= 0 && pm1 < 8)
                CA_G32(0x005cf1ec + (uint32_t)d * 0x88u) = CA_G32(0x005cf1ec + (uint32_t)d * 0x88u) + (int32_t)CA_W(F, 0x5c + 4 * (uint32_t)place);
            const uint32_t pl = CA_W(F, 0xc4 + 0xc * i);
            CA_GU32(0x005cf1f0 + ((uint32_t)d * 0x22u + (uint32_t)ev) * 4u) = pl;
        }
        ccall<void>(F_update_ranking);
        CA_G32(S_EVENT) = CA_G32(S_EVENT) + 1;
        CA_G32(S_WEEK) = CA_G32(S_WEEK) + 1;
        CA_G8(S_QUALIFIED) = 0;
    }
    ccall<void>(F_CareerReload);
    if (CA_B(F, 0x12) != 0) {
        if (*UI_GP(volatile int32_t, S_SEASON) <= CA_G32(S_EVENT)) {   // the season is over
            const int32_t rank = CA_G32(0x005cf5a0);                   // the player's
            ccall<void>(F_CareerGiveAward, rank == 0 ? 1 : rank == 1 ? 2 : rank == 2 ? 3 : 4);
            const int32_t prize = *(volatile int32_t*)(UI_GP(uint8_t, S_SEASON) + (uint32_t)rank * 4u + 4);
            CA_G32(S_FUNDS) = CA_G32(S_FUNDS) + prize;
            ccall<void>(F_PostSeasonDo, rank);
            zero_dwords(S_DRIVERS, 0x110);
            CAREER_LOG(CA_CP(0x004ff6b8), rank + 1);                   // "Season: rank %d"
            if (rank == 0) {
                const int32_t cls = CA_G32(S_CLASS) + 1;
                if (cls < 4) {                                         // won: the next class
                    CA_G32(S_CLASS) = cls;
                    ccall<void>(F_set_class, cls);
                    for (uint32_t i = 0, a = S_DRIVERS; a < S_DRIVERS_END; a += 0x88, i++) CA_GU32(a) = i;
                    ccall<void>(F_CareerGiveAward, 5);
                    CA_G8(S_NEW_CLASS) = 1;
                }
            }
            CA_G32(S_SEASONS) = CA_G32(S_SEASONS) + 1;
            CA_B(F, 0x13) = 0;
            CA_G32(S_EVENT) = 0;
            CA_G32(S_WEEK) = 0;
            for (uint32_t a = 0x005cf1ec; a < 0x005cf62c; a += 0x88) CA_GU32(a) = 0;
        } else {
            ccall<void>(F_PostSeasonDo, -1);
            CA_B(F, 0x13) = 0;
        }
    }
    return CA_B(F, 0x13);
}
PORT_FN(0x004bc760, "do_event", do_event_c, fp_screen_i)

// fake_results: the player at `place`, the others in order around it
static void __cdecl fake_results_c(int32_t place, uint8_t* res) {
    volatile int32_t* p = (volatile int32_t*)(res + 4);
    for (int32_t i = 0; i < 8; i++, p += 3) {
        if (CA_G32(S_DRIVER_MAP + 4 * (uint32_t)i) == 7) *p = place;
        else if (i - place == -1) *p = 8;
        else *p = i + 1;
    }
}
static void fp_fake_results(Footprint& f, int32_t, uint8_t* res) { f.add(res + 4, 0x60, "the Results' places"); }
PORT_FN(0x004bced0, "fake_results", fake_results_c, fp_fake_results)

// sort_carlist: the World's first 8 cars in the driver map's order (a copy of the 8, then each from it)
// FIX CANDIDATE: a map value past 7 reads past the copy (a damaged career file). Not in ordinary play.
static void __cdecl sort_carlist_c(uint8_t* w) {
    alignas(8) uint8_t F[0x64c];                                    // sub esp, 0x640; 3 pushes
    copy_dwords(F + 0xc, w + 0x28, 0x190);
    uint8_t* dst = w + 0x28;
    for (uint32_t a = S_DRIVER_MAP; a < S_DRIVERS; a += 4, dst += 0xc8)
        copy_dwords(dst, F + 0xc + (uint32_t)CA_G32(a) * 0xc8u, 0x32);
}
static void fp_sort_carlist(Footprint& f, uint8_t* w) { f.add(w + 0x28, 0x640, "the World's cars"); }
PORT_FN(0x004bcf20, "sort_carlist(career.obj)", sort_carlist_c, fp_sort_carlist)

// =========================================================================================================================
// CareerStatus
// =========================================================================================================================
static void __fastcall CareerStatus_Added(CareerStatus* self, Edx) {
    alignas(8) uint8_t F[0x194];                                    // sub esp, 0x188; 3 pushes
    ca_xl_once(0x005cfe74, 0x01, 0x005cffd0, 0x004ff6c8, 0x004bd220);  // Career:Status:Player
    ca_xl_once(0x005cfe74, 0x02, 0x005cfe38, 0x004ff6e0, 0x004bd210);  // Career:Status:Funds
    put14(F, 0xc, 0xe, 0, 0x59, 0x1b1, 0, 0, 0x004ff6f4, 0, 0, 0);    // "funds.stp"
    put14(F, 0x44, 5, 0, 0x190, 0x14, 0, 0, ca_xlate(0x005cffd0), 0, 0, 0xc);
    ca_item_ctor(F + 0x7c, 5, 0, 0x1ea, 0x14, 0, 0, S_INFO, 0, 0, 0xc);   // the player's name
    put14(F, 0xb4, 5, 0, 0x190, 0x28, 0, 0, ca_u(self->week), 0, 0, 0xc);
    put14(F, 0xec, 5, 0, 0x6e, 0x1bb, 0, 0, ca_xlate(0x005cfe38), 0, 0, 0xc);
    ca_item_ctor(F + 0x124, 5, 0, 0x118, 0x1bb, 0, 0, ca_u(self->funds), 0, 0, 0xd);
    end_item(F, 0x15c);
    tcall<void>(F_UICC_AddItems, self, (void*)(F + 0xc));
}
static void fp_CareerStatus_Added(Footprint& f, CareerStatus*, Edx) { f.replay_only = "builds widgets (_UIAddItems allocates, loads stamps)"; }
PORT_FN(0x004bcf80, "CareerStatus::Added", CareerStatus_Added, fp_CareerStatus_Added)

static void __fastcall CareerStatus_Create(CareerStatus* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, 0, (const void*)(uintptr_t)S_INFO, (uint32_t)S_INFO_SIZE);
    vcall<void>(self, 0x10, 0, (const void*)0);                                                   // Callback(0, 0)
}
static void fp_CareerStatus_Create(Footprint& f, CareerStatus*, Edx) { f.replay_only = "adds a notification (allocates)"; }
PORT_FN(0x004bd230, "CareerStatus::Create", CareerStatus_Create, fp_CareerStatus_Create)

static void __fastcall CareerStatus_Callback(CareerStatus* self, Edx, int32_t, const void*) {
    ccall<void>(F_LocaleMoney, (char*)self->funds, CA_G32(S_FUNDS), 0);
    ca_xl_once(0x005d002c, 0x01, 0x005d0020, 0x004ff700, 0x004bd300);  // Career:Status:Week
    ca_xlate(0x005d0020);
    const int32_t week = CA_G32(S_WEEK) + 1;
    // FIX: "<Week> <n>" (Career:Status:Week's translation) was formatted into the 0x40-byte week line unbounded: a
    // translation of about 60 characters ran over the funds line (formatted just before, so the career's menus showed the
    // week's tail as the funds), then the season's, then past the control into the screen's frame it lives in (the
    // CareerStatus is on each screen's stack). The line keeps its first 63 characters (the control may be the original
    // screens', so it can't grow). A line that fits is formatted as before.
    const char* wk = UI_GP(const char, 0x005d0024);
    if (VP_FIX && ca_sd_long(wk, week, 0x40)) ca_fix_sd(self->week, 0x40, 0x004ff714, wk, week, false);
    else SPRINTF(self->week, CA_CP(0x004ff714), wk, week);                                   // "%s %d"
    SPRINTF(self->season, CA_CP(0x004ff71c), CA_G32(S_SEASONS) + 1);                // "%d"
}
static void fp_CareerStatus_Callback(Footprint& f, CareerStatus* self, Edx, int32_t, const void*) {
    if (!ca_xl_built(0x005d002c, 0x01)) { f.replay_only = "builds its function-local Xlator (atexit)"; return; }
    f.add(self->week, 0xc0, "CareerStatus's texts");
    CA_FP_XL(f, 0x005d0020);
}
PORT_FN(0x004bd260, "CareerStatus::Callback", CareerStatus_Callback, fp_CareerStatus_Callback)

// =========================================================================================================================
// the drivers, the class
// =========================================================================================================================
static const uint8_t* __cdecl CareerGetDriver_c(int32_t i) {
    const void* lounge = UI_GP(const void, S_LOUNGE);
    return vcall<const uint8_t*>(lounge, 4, CA_G32(S_CLASS) + 3, i);                          // Lounge->Get(strength, i)
}
PORT_FN(0x004bd310, "CareerGetDriver", CareerGetDriver_c, fp_none_i)
static const char* __cdecl CareerGetDriverName_c(int32_t i) {
    if (i == 7) return CA_CP(S_INFO);
    return *(const char* const volatile*)(ccall<const uint8_t*>(F_CareerGetDriver, i) + 0x210);
}
PORT_FN(0x004bd330, "CareerGetDriverName", CareerGetDriverName_c, fp_none_i)

static const uint32_t k_xl_class[4] = {0x005d0038, 0x005cfe28, 0x005cf068, 0x005cf6f0};
static const char* __cdecl CareerGetClassName_c(int32_t cls) {
    ca_xl_once(0x005cfe64, 0x01, 0x005d0038, 0x004ff720, 0x004bd4e0);  // Career:ClassName:Amateur
    ca_xl_once(0x005cfe64, 0x02, 0x005cfe28, 0x004ff73c, 0x004bd4d0);  // Club
    ca_xl_once(0x005cfe64, 0x04, 0x005cf068, 0x004ff754, 0x004bd4c0);  // Pro
    ca_xl_once(0x005cfe64, 0x08, 0x005cf6f0, 0x004ff76c, 0x004bd4b0);  // GT
    if ((uint32_t)cls > 3) {
        UI_LogPanic(CA_CP(0x004ff780), cls);                          // "Unknown class %d"
        return 0;
    }
    return CA_CP(ca_xlate(k_xl_class[cls]));
}
static void fp_CareerGetClassName(Footprint& f, int32_t) {
    if (!ca_xl_built(0x005cfe64, 0x0f)) { f.replay_only = "builds its function-local Xlators (atexit)"; return; }
    for (uint32_t a : k_xl_class) CA_FP_XL(f, a);
}
PORT_FN(0x004bd350, "CareerGetClassName", CareerGetClassName_c, fp_CareerGetClassName)

// get_options: the options' GAME values, then the career's own over them
static void __cdecl get_options_c(uint8_t* o) {
    for (uint32_t i = 0; i < 10; i++) ((volatile uint32_t*)o)[i] = 0;
    const char* sec = *(const char* const volatile*)(uintptr_t)0x004df9ec;                     // "GAME"
    ccall<void>(F_OptionsGetI, sec, CA_CP(0x004ff79c), (void*)o);                             // realism
    ccall<void>(F_OptionsGetB, sec, CA_CP(0x004ff7a4), (void*)(o + 0x24));                    // damage_on
    ccall<void>(F_OptionsGetB, sec, CA_CP(0x004ff7b0), (void*)(o + 0x25));                    // is_reversed
    ccall<void>(F_OptionsGetI, sec, CA_CP(0x004ff7bc), (void*)(o + 4));                       // time_of_day
    ccall<void>(F_OptionsGetI, sec, CA_CP(0x004ff7c8), (void*)(o + 8));                       // weather
    ccall<void>(F_OptionsGetI, sec, CA_CP(0x004ff7d0), (void*)(o + 0x10));                    // race_type
    ccall<void>(F_OptionsGetI, sec, CA_CP(0x004ff7dc), (void*)(o + 0xc));                     // field
    ccall<void>(F_OptionsGetI, sec, CA_CP(0x004ff7e4), (void*)(o + 0x1c));                    // gc_type
    *(volatile int32_t*)o = CA_G32(S_REALISM);
    *(volatile int32_t*)(o + 0xc) = 1;
    *(volatile uint8_t*)(o + 0x24) = 0;
    *(volatile uint8_t*)(o + 0x25) = 0;
    *(volatile int32_t*)(o + 4) = 1;
    *(volatile int32_t*)(o + 0x1c) = 0;
    *(volatile int32_t*)(o + 8) = 0;
    *(volatile int32_t*)(o + 0x14) = 3;
    *(volatile int32_t*)(o + 0x18) = 0;
}
static void fp_get_options(Footprint& f, uint8_t* o) { f.add(o, 0x28, "the GameOptions"); }
PORT_FN(0x004bd510, "get_options", get_options_c, fp_get_options)

// =========================================================================================================================
// the career file
// =========================================================================================================================
static uint8_t __cdecl CareerLoadSlot_c(int32_t slot, uint8_t* info) { return ccall<uint8_t>(F_career_load, slot, (void*)info); }
static void fp_slot_io(Footprint& f, int32_t, uint8_t*) { f.replay_only = "reads or writes a career file"; }
PORT_FN(0x004bd600, "CareerLoadSlot", CareerLoadSlot_c, fp_slot_io)
static void __cdecl CareerSaveSlot_c(int32_t slot, uint8_t* info) { ccall<void>(F_career_save, slot, (void*)info); }
PORT_FN(0x004bd620, "CareerSaveSlot", CareerSaveSlot_c, fp_slot_io)

static void __cdecl CareerDestroySlot_c(int32_t slot) {
    ca_xl_once(0x005d0034, 0x01, 0x005cffa0, 0x004ff7ec, 0x004bd740);  // Career:AbandonCareerDialog:Title
    ca_xl_once(0x005d0034, 0x02, 0x005cf750, 0x004ff810, 0x004bd730);  // ...:Message
    ca_xlate(0x005cf750);
    ca_xlate(0x005cffa0);
    const uint32_t msg = CA_GU32(0x005cf754);
    const uint32_t title = CA_GU32(0x005cffa4);
    if (ccall<uint8_t>(F_UIDoYesNoBox, title, msg, 0))
        ccall<uint8_t>(F_FileRemove, ccall<const char*>(F_get_career_filename, slot));
}
PORT_FN(0x004bd640, "CareerDestroySlot", CareerDestroySlot_c, fp_screen_i)

// FIX: "<user directory>career\career<slot>.dat" was formatted into the 0x108-byte buffer unbounded, so a user directory
// of 246 or more characters ran it into the Xlators after it. Cut, the name would no longer be the career's file (and past
// MAX_PATH no file can be opened by it anyway), so a name that doesn't fit is given as "": the career files can't be read
// (the chooser lists the slots empty), made (career_save reports "Can't create career file" to the log, as for any file it
// can't make) or removed. (The port's user directory, race.exe's folder's Config\, is kept to 200 characters by the
// kernel's fix; the original's is a short literal.) A name that fits is formatted as before.
static const char* __cdecl get_career_filename_c(int32_t slot) {
    const char* dir = ccall<const char*>(F_Win32GetUserDirectory);
    if (VP_FIX && ui_strnlen(dir, 0x107) + 0x11 + ca_dec_len(slot) > 0x107) {             // "career\career" ".dat": 17
        *(volatile char*)(uintptr_t)S_FILENAME = 0;
        return CA_CP(S_FILENAME);
    }
    SPRINTF((char*)(uintptr_t)S_FILENAME, CA_CP(0x004ff834), dir, slot);                      // "%scareer\career%d.dat"
    return CA_CP(S_FILENAME);
}
static void fp_get_career_filename(Footprint& f, int32_t) { UI_FP(f, S_FILENAME, 0x108, "get_career_filename's buffer"); }
PORT_FN(0x004bd700, "get_career_filename", get_career_filename_c, fp_get_career_filename)

// =========================================================================================================================
// career_menu and its callbacks
// =========================================================================================================================
// FIX CANDIDATE (and EventsDo's, testing_menu's, PostSeasonDo's, RankingDo's): the CareerStatus on the stack is added to the
// dialog before its Callback formats its texts, so the StaticTexts it adds copy (strcpy, unbounded) whatever the stack held
// there; the first Update copies the real texts before anything is drawn. A garbage run of 256+ non-zero bytes would overrun
// a StaticText's 0x100-byte text. Ordinary play reaches it every time the screen opens; whether it overruns is the stack's.
static uint8_t __cdecl career_menu_c(uint8_t* testing) {
    alignas(8) uint8_t F[0x39c];                                    // sub esp, 0x394; 2 pushes
    // F+8 the UIDialog, +0x1c the items (0x38 each), +0x284 the CareerSummary, +0x2c0 awards.stp, +0x2c4 the CareerStatus
    ccall<void>(F_ResourceSetMustLoad, CA_CP(0x004ff84c));                                    // "viper.car"
    CA_GU32(S_UPGRADE_SET) = ccall<uint32_t>(F_CarFileGetUpgradeSet, CA_CP(0x004ff858));      // "viper.ugs"
    ca_xl_once(0x005cf704, 0x01, 0x005cfe68, 0x004ff864, 0x004be070);  // Titles:Career
    CA_W(F, 0x2d8) = 0;                                              // the CareerStatus: its widget, its vtable
    CA_W(F, 0x2c4) = VT_CareerStatus;
    CA_W(F, 0x2c0) = ccall<uint32_t>(uit::F_gxGetStamp, CA_CP(0x004ffa10));                  // "awards.stp"
    tcall<void*>(F_CareerSummary_ctor, (void*)(F + 0x284), (const void*)(uintptr_t)S_INFO);
    ca_xl_once(0x005cf704, 0x02, 0x005cfe18, 0x004ff874, 0x004be060);  // Career:MainMenu:Events
    ca_xl_once(0x005cf704, 0x04, 0x005d0000, 0x004ff88c, 0x004be050);  // ...:Ranking
    ca_xl_once(0x005cf704, 0x08, 0x005cffe0, 0x004ff8a4, 0x004be040);  // ...:Testing
    ca_xl_once(0x005cf704, 0x10, 0x005cf058, 0x004ff8bc, 0x004be030);  // ...:Upgrades
    ca_xl_once(0x005cf704, 0x20, 0x005cffb0, 0x004ff8d8, 0x004be020);  // ...:Save
    ca_xl_once(0x005cf704, 0x40, 0x005cfff0, 0x004ff8f0, 0x004be010);  // UI:Back
    put14(F, 0x1c, 0x17, 0, 0, 0, 0, 0, 0x004e4db8, 0, CA_A(F, 0x2c4), 0);                    // the CareerStatus
    ca_item_ctor(F + 0x54, 0x17, 0, 0xbb, 0xc1, 0xa5, 0x86, 0x004e4db8, 0, CA_A(F, 0x284), 0);   // the CareerSummary
    put14(F, 0x8c, 2, 0xfffffffeu, 0x1ef, 0x8e, 0, 0, ca_xlate(0x005cfe18), 0, 0, 2);          // Events: -2
    put14(F, 0xc4, 2, 0, 0x1ef, 0xbf, 0, 0, ca_xlate(0x005d0000), 0, F_ranking_cb, 2);          // Ranking
    {
        const char* t = CA_CP(ca_xlate(0x005d0000));
        const uint32_t k = (uint16_t)ccall<int>(uit::F_tolower, (int)*(const volatile signed char*)t);
        ca_item_ctor(F + 0xfc, 4, 0, (int32_t)k, 0, 0, 0, 0, 0, F_ranking_cb, 0);              // its hot key
    }
    ca_item_ctor(F + 0x134, 2, 0x21, 0x1ef, 0xf0, 0, 0, ca_xlate(0x005cffe0), 0, 0, 2);       // Testing: 0x21
    put14(F, 0x16c, 2, 0, 0x1ef, 0x121, 0, 0, ca_xlate(0x005cf058), 0, F_upgrade_cb, 2);        // Upgrades
    {
        const char* t = CA_CP(ca_xlate(0x005cf058));
        const uint32_t k = (uint16_t)ccall<int>(uit::F_tolower, (int)*(const volatile signed char*)t);
        ca_item_ctor(F + 0x1a4, 4, 0, (int32_t)k, 0, 0, 0, 0, 0, F_upgrade_cb, 0);
    }
    ca_item_ctor(F + 0x1dc, 2, 0, 0x1ef, 0x152, 0, 0, ca_xlate(0x005cffb0), 0, F_save_cb, 2);   // Save
    put14(F, 0x214, 2, 0xffffffffu, 0x13, 0x1a2, 0, 0, ca_xlate(0x005cfff0), 0, F_back_cb, 6);  // Back: -1
    end_item(F, 0x24c);
    CA_W(F, 8) = ca_xlate(0x005cfe68);                               // the UIDialog
    CA_W(F, 0x14) = CA_A(F, 0x1c);
    CA_W(F, 0xc) = 0x004ff8f8;                                       // "career.stp"
    CA_W(F, 0x10) = 0xfffffffeu;
    CA_W(F, 0x18) = F_career_menu_idle;
    const int32_t r = ccall<int32_t>(uit::F_UIDoDialog, (const void*)(F + 8), UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H),
                                     -999, -999, 1);
    ccall<void>(F_CarFileForgetUpgradeSet, UI_GP(void, S_UPGRADE_SET));
    CA_GU32(S_UPGRADE_SET) = 0;
    ccall<void>(F_ResourceSetUnload, CA_CP(0x004ff904));                                      // "viper.car"
    *(volatile uint8_t*)testing = (uint8_t)(r == 0x21 ? 1 : 0);
    tcall<void>(F_CareerSummary_dtor, (void*)(F + 0x284));
    ccall<void>(uit::F_gxForgetStamp, CA_W(F, 0x2c0));
    return (uint8_t)(r != -1 ? 1 : 0);
}
static void fp_career_menu(Footprint& f, uint8_t*) { f.replay_only = "runs the career's menu (a dialog)"; }
PORT_FN(0x004bd750, "career_menu", career_menu_c, fp_career_menu)

static uint8_t __cdecl upgrade_cb_c(int32_t) { ccall<void>(F_UpgradeDo); return 0; }
PORT_FN(0x004bdde0, "upgrade_cb", upgrade_cb_c, fp_screen_i)

static uint8_t __cdecl save_cb_c(int32_t) {
    ccall<void>(F_career_save, CA_G32(S_SLOT), (void*)(uintptr_t)S_INFO);
    copy_dwords((void*)(uintptr_t)S_INFO_SAVED, (const void*)(uintptr_t)S_INFO, 0x19d);
    ca_xl_once(0x005cf748, 0x01, 0x005cff90, 0x004ff910, 0x004bded0);  // Career:SaveOkDialog:Title
    ca_xl_once(0x005cf748, 0x02, 0x005cfe08, 0x004ff92c, 0x004bdec0);  // ...:Message
    ca_xlate(0x005cfe08);
    ca_xlate(0x005cff90);
    const uint32_t msg = CA_GU32(0x005cfe0c);
    const uint32_t title = CA_GU32(0x005cff94);
    ccall<void>(F_UIDoOkBox, title, msg);
    return 0;
}
PORT_FN(0x004bddf0, "save_cb(career.obj)", save_cb_c, fp_screen_i)

static uint8_t __cdecl ranking_cb_c(int32_t) { ccall<void>(F_RankingDo); return 0; }
PORT_FN(0x004bdee0, "ranking_cb", ranking_cb_c, fp_screen_i)

// back_cb: leave; a career changed since it was saved asks first (yes: save, no: leave, cancel: stay)
static uint8_t __cdecl back_cb_c(int32_t) {
    ca_xl_once(0x005cf6fc, 0x01, 0x005cffc0, 0x004ff948, 0x004bdfe0);  // Career:SaveCareerDialog:Title
    ca_xl_once(0x005cf6fc, 0x02, 0x005cfe48, 0x004ff968, 0x004bdfd0);  // ...:Message
    if (!crt_memcmp_ne((const void*)(uintptr_t)S_INFO, (const void*)(uintptr_t)S_INFO_SAVED, S_INFO_SIZE)) return 1;
    ca_xlate(0x005cfe48);
    ca_xlate(0x005cffc0);
    const uint32_t msg = CA_GU32(0x005cfe4c);
    const uint32_t title = CA_GU32(0x005cffc4);
    const int32_t r = ccall<int32_t>(F_UIDoYesNoCancelBox, title, msg);
    if (r == -3) return 1;
    if (r == -2) {
        ccall<uint8_t>(F_save_cb, 0);
        return 1;
    }
    return 0;
}
PORT_FN(0x004bdef0, "back_cb", back_cb_c, fp_screen_i)

static uint8_t __cdecl career_menu_idle_c(int32_t*) {
    if (CA_G8(S_NEW_CLASS) != 0) ccall<void>(F_NewClassDo);
    CA_G8(S_NEW_CLASS) = 0;
    return 0;
}
static void fp_career_menu_idle(Footprint& f, int32_t*) {
    if (CA_G8(S_NEW_CLASS) != 0) { f.replay_only = "shows the new class's screen (NewClassDo)"; return; }
    UI_FP(f, S_NEW_CLASS, 1, "career.obj new-class flag");
}
PORT_FN(0x004bdff0, "career_menu_idle", career_menu_idle_c, fp_career_menu_idle)

// career_load: the header ('FNIC', 2, 0x674, the sum), the body, un-xor'd, its sum checked
static uint8_t __cdecl career_load_c(int32_t slot, uint8_t* info) {
    alignas(8) uint8_t F[0x1c];                                     // sub esp, 0x14; 2 pushes: +8 the file, +0xc the header
    uint8_t ok = 0;
    CA_W(F, 8) = (uint32_t)ccall<int32_t>(uit::F_FileOpen, ccall<const char*>(F_get_career_filename, slot));
    if (CA_W(F, 8) == 0) return 0;
    CA_W(F, 0xc) = 0; CA_W(F, 0x10) = 0; CA_W(F, 0x14) = 0; CA_W(F, 0x18) = 0;
    if (!ccall<uint8_t>(F_FileReadExact, CA_W(F, 8), (void*)(F + 0xc), 0x10)) {
        UI_LogReport(CA_CP(0x004ff9b4));                              // "Can't read career header"
    } else if (CA_W(F, 0xc) != 0x43494e46u || CA_W(F, 0x10) != 2 || CA_W(F, 0x14) != 0x674) {
        UI_LogReport(CA_CP(0x004ff998));                              // "Bad or corrupt career file"
    } else if (ccall<uint8_t>(F_FileReadExact, CA_W(F, 8), (void*)info, 0x674)) {
        ccall<void>(F_xor_block, (void*)info, 0x674, 0xa4);
        if (ccall<uint32_t>(F_checksum, (const void*)info, 0x674) == CA_W(F, 0x18)) ok = 1;
        else UI_LogReport(CA_CP(0x004ff988));                         // "checksum fails"
    }
    ccall<void>(uit::F_FileClose, (int32_t*)(F + 8));
    return ok;
}
PORT_FN(0x004be080, "career_load", career_load_c, fp_slot_io)

// FIX: "<user directory>career" was formatted into the frame's 0x104 bytes unbounded: a user directory of 254 or more
// characters ran it into the body's buffer after it (written afterwards, so nothing was lost; a longer directory would
// have run on past the frame). Such a directory isn't made (at over MAX_PATH it can't be); the career's file name doesn't
// fit get_career_filename's buffer either (its fix gives ""), so creating the file fails and the save is skipped with the
// game's "Can't create career file" report, as for any file it can't make. A directory name that fits is made as before.
static void __cdecl career_save_c(int32_t slot, uint8_t* info) {
    alignas(8) uint8_t F[0x794];                                    // sub esp, 0x78c; 2 pushes: +8 the file, +0xc the header,
                                                                    // +0x1c the directory, +0x120 the body
    const char* dir = ccall<const char*>(F_Win32GetUserDirectory);
    if (!(VP_FIX && ca_longer(dir, 0x103 - 6))) {                    // (FIX: above) "career": 6
        SPRINTF((char*)(F + 0x1c), CA_CP(0x004ff9d0), dir);         // "%scareer"
        ccall<uint8_t>(F_FileCreateDirectory, (const char*)(F + 0x1c));
    }
    const char* name = ccall<const char*>(F_get_career_filename, slot);
    CA_W(F, 8) = (uint32_t)ccall<int32_t>(F_FileCreate, name);
    if (CA_W(F, 8) == 0) {
        UI_LogReport(CA_CP(0x004ff9dc), name);                        // "Can't create career file %s"
        return;
    }
    CA_W(F, 0xc) = 0x43494e46u;
    CA_W(F, 0x10) = 2;
    CA_W(F, 0x14) = 0x674;
    CA_W(F, 0x18) = ccall<uint32_t>(F_checksum, (const void*)info, 0x674);
    copy_dwords(F + 0x120, info, 0x19d);
    ccall<void>(F_xor_block, (void*)(F + 0x120), 0x674, 0xa4);
    ccall<uint8_t>(F_FileWrite, CA_W(F, 8), (const void*)(F + 0xc), 0x10);
    ccall<uint8_t>(F_FileWrite, CA_W(F, 8), (const void*)(F + 0x120), 0x674);
    ccall<void>(uit::F_FileClose, (int32_t*)(F + 8));
}
PORT_FN(0x004be170, "career_save", career_save_c, fp_slot_io)

// =========================================================================================================================
// the ranking
// =========================================================================================================================
static void __cdecl update_ranking_c() {
    alignas(8) int32_t order[8];
    for (int32_t i = 0; i < 8; i++) ((volatile int32_t*)order)[i] = i;
    ccall<void>(F_qsort, (void*)order, 8, 4, F_driver_compare);
    for (int32_t i = 0; i < 8; i++) {
        const int32_t d = ((volatile int32_t*)order)[i];
        CA_G32(S_DRIVERS + (uint32_t)d * 0x88u) = i;
    }
}
static void fp_update_ranking(Footprint& f) { UI_FP(f, S_DRIVERS, 0x440, "the drivers' records (their ranks)"); }
PORT_FN(0x004be280, "update_ranking", update_ranking_c, fp_update_ranking)

// driver_compare (qsort's): more points first; then the later results (from event 31 down), fewer first; then the names
static int32_t __cdecl driver_compare_c(const int32_t* pa, const int32_t* pb) {
    alignas(8) uint8_t F[0x210];                                    // sub esp, 0x200; 4 pushes: +0x10 b's name, +0x110 a's
    const int32_t a = *(const volatile int32_t*)pa;
    const int32_t b = *(const volatile int32_t*)pb;
    const int32_t pts_a = CA_G32(0x005cf1ec + (uint32_t)a * 0x88u);
    const int32_t pts_b = CA_G32(0x005cf1ec + (uint32_t)b * 0x88u);
    if (pts_b != pts_a) return (int32_t)((uint32_t)pts_b - (uint32_t)pts_a);
    for (int32_t k = 31; k >= 0; k--) {
        const int32_t ra = CA_G32(0x005cf1f0 + (uint32_t)a * 0x88u + 4u * (uint32_t)k);
        const int32_t rb = CA_G32(0x005cf1f0 + (uint32_t)b * 0x88u + 4u * (uint32_t)k);
        if (rb != ra) return (int32_t)((uint32_t)ra - (uint32_t)rb);
    }
    // FIX: the two drivers' names (the lounge's, from drivers.res, or the player's) were copied into 0x100 bytes of the frame
    // each with no limit before being compared, so a name of 256 or more characters (a modded drivers.res) ran the first
    // over the second and the second over the frame's saved registers and return address. Such a name isn't copied: it's
    // compared where it is, which gives the order the copies would have given. A name that fits is copied as before.
    const char* na = ccall<const char*>(F_CareerGetDriverName, a);
    if (!(VP_FIX && ca_longer(na, 0xff))) {
        crt_strcpy((char*)(F + 0x110), na);
        na = (const char*)(F + 0x110);
    }
    const char* nb = ccall<const char*>(F_CareerGetDriverName, b);
    if (!(VP_FIX && ca_longer(nb, 0xff))) {
        crt_strcpy((char*)(F + 0x10), nb);
        nb = (const char*)(F + 0x10);
    }
    return ccall<int32_t>(F_stricmp, na, nb);
}
static void fp_driver_compare(Footprint&, const int32_t*, const int32_t*) {}
PORT_FN(0x004be2d0, "driver_compare", driver_compare_c, fp_driver_compare)

static void __cdecl CareerUnload_c() {
    ccall<void>(F_CareerSeasonForget, UI_GP(void, S_SEASON));
    CA_G32(S_SEASON) = 0;
}
static void fp_season_io(Footprint& f) { f.replay_only = "loads or frees the season (a resource)"; }
PORT_FN(0x004be3e0, "CareerUnload", CareerUnload_c, fp_season_io)
static void __cdecl CareerReload_c() { ccall<void>(F_set_class, CA_G32(S_CLASS)); }
PORT_FN(0x004be400, "CareerReload", CareerReload_c, fp_season_io)

static uint8_t __cdecl CareerIsTestMode_c() { return (uint8_t)(crt_strcmp_ne(CA_CP(S_INFO), CA_CP(0x004ff9f8)) == 0 ? 1 : 0); }   // "TEST"
PORT_FN(0x004be410, "CareerIsTestMode", CareerIsTestMode_c, fp_none)

// set_class: the class, its season (season<n>.ssn), its name
// FIX: the class's name (Career:ClassName:<class>'s translation) was copied into a 0x30-byte static (0x5cf760) unbounded:
// a translation of 48 or more characters ran into the CareerInfo's saved copy after it (0x5cf790, what back_cb compares
// with the live one), so leaving the career's menu asked to save a career that hadn't changed; at about 1,700 characters
// it ran past that into the Xlators. It keeps its first 47 characters. Nothing reads the copy (the screens ask
// CareerGetClassName) and it isn't in the career file, so the cut changes nothing a player sees or saves. One that fits
// is copied as before.
static void __cdecl set_class_c(int32_t cls) {
    alignas(8) uint8_t F[0x28];                                     // sub esp, 0x20; 2 pushes: +8 the season's name
    CA_G32(S_CLASS) = cls;
    SPRINTF((char*)(F + 8), CA_CP(0x004ffa00), cls);                // "season%d.ssn"
    void* old = UI_GP(void, S_SEASON);
    if (old) ccall<void>(F_CareerSeasonForget, old);
    CA_GU32(S_SEASON) = ccall<uint32_t>(F_CareerSeasonGet, (const char*)(F + 8));
    const char* cn = ccall<const char*>(F_CareerGetClassName, CA_G32(S_CLASS));
    if (VP_FIX) ui_copy_bounded((char*)(uintptr_t)S_CLASS_NAME, cn, 0x30);                  // (FIX: above)
    else crt_strcpy((char*)(uintptr_t)S_CLASS_NAME, cn);
}
static void fp_set_class(Footprint& f, int32_t) { f.replay_only = "loads the class's season (a resource)"; }
PORT_FN(0x004be450, "set_class", set_class_c, fp_set_class)

static uint32_t __cdecl checksum_c(const uint8_t* p, int32_t n) {
    uint32_t sum = 0;
    const volatile uint8_t* q = p;
    while (n-- > 0) sum += *q++;
    return sum;
}
static void fp_checksum(Footprint&, const uint8_t*, int32_t) {}
PORT_FN(0x004be4d0, "checksum", checksum_c, fp_checksum)

static void __cdecl xor_block_c(uint8_t* p, int32_t n, uint8_t k) {
    volatile uint8_t* q = p;
    while (n-- > 0) { *q = (uint8_t)(*q ^ k); q++; }
}
static void fp_xor_block(Footprint& f, uint8_t* p, int32_t n, uint8_t) {
    if (n > 0 && n <= 0x100000) f.add(p, (uint32_t)n, "the block");
}
PORT_FN(0x004be500, "xor_block", xor_block_c, fp_xor_block)

// credit_account: the player's purse (the map's 7: the last car it's at) by its place
// FIX CANDIDATE: no car the player's reads the entry before the Results (-1). Not in ordinary play (the map holds 7).
static void __cdecl credit_account_c(const uint8_t* event, const uint8_t* res, const int32_t* map) {
    int32_t me = -1;
    for (int32_t i = 0; i < 8; i++)
        if (((const volatile int32_t*)map)[i] == 7) me = i;
    const int32_t place = *(const volatile int32_t*)(res + (uint32_t)me * 0xcu + 4);
    if (place >= 1 && place <= 8)
        CA_G32(S_FUNDS) = CA_G32(S_FUNDS) + *(const volatile int32_t*)(event + (uint32_t)place * 4u + 0x14);
}
static void fp_credit_account(Footprint& f, const uint8_t*, const uint8_t*, const int32_t*) { UI_FP(f, S_FUNDS, 4, "the funds"); }
PORT_FN(0x004be520, "credit_account", credit_account_c, fp_credit_account)

// =========================================================================================================================
// season.obj
// =========================================================================================================================
// CareerSeasonGet: the resource; every prize (the season's 8, each event's 8 purses) must have a translation
// FIX CANDIDATE: a season resource that can't be loaded is read through 0 (a missing file). Not in ordinary play.
static uint8_t* __cdecl CareerSeasonGet_c(const char* name) {
    alignas(8) uint8_t F[0x18];                                     // sub esp, 8; 4 pushes: +0x10 the size, +0x14 (int)
    uint8_t* s = ccall<uint8_t*>(F_ResourceGet, name, 0x53454153u, (void*)(F + 0x10), (void*)(F + 0x14), 0, 0);   // 'SEAS'
    for (uint32_t i = 0; i < 8; i++) {
        const volatile int32_t* p = (const volatile int32_t*)(s + 4 * i + 4);
        if (!ccall<uint8_t>(F_could_xlate_money, *p))
            UI_LogPanic(CA_CP(0x004ffc58), i, *p);                    // "Can't xlate season prize %d: $%d"
    }
    for (int32_t e = 0; *(volatile int32_t*)s > e; e++)
        for (uint32_t k = 0; k < 8; k++) {
            const volatile int32_t* p = (const volatile int32_t*)(s + ((uint32_t)e * 0x16u + k) * 4u + 0x3c);
            if (!ccall<uint8_t>(F_could_xlate_money, *p))
                UI_LogPanic(CA_CP(0x004ffc7c), e, k, *p);             // "Can't xlate Event %d prize %d: $%d"
        }
    return s;
}
static void fp_CareerSeasonGet(Footprint& f, const char*) { f.replay_only = "loads the season (a resource)"; }
PORT_FN(0x004bfd90, "CareerSeasonGet", CareerSeasonGet_c, fp_CareerSeasonGet)

static uint8_t __cdecl could_xlate_money_c(int32_t v) {
    char buf[0x100];
    SPRINTF(buf, CA_CP(0x004ffca0), v);                              // "Career:Money:%d"
    return ccall<uint8_t>(F_CouldXlate, (const char*)buf);
}
PORT_FN(0x004bfe40, "could_xlate_money", could_xlate_money_c, fp_none_i)

static void __cdecl CareerSeasonForget_c(void* s) { ccall<uint8_t>(F_ResourceForget, s); }
static void fp_CareerSeasonForget(Footprint& f, void*) { f.replay_only = "frees the season (a resource)"; }
PORT_FN(0x004bfe70, "CareerSeasonForget", CareerSeasonForget_c, fp_CareerSeasonForget)

}  // namespace career_main
}  // namespace
