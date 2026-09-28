// root_ghost.cpp -- M3 UI stage, step U3 (group C): ghost.obj, rewritten faithfully (library `root`).
//
//   The ghost cars: at WorldBegin, GhostBegin finds the player's car (find_target), reads each ghostable car's best lap
//   from its ".gcf" file under "<user>ghostcar\" (load_old_stats: one LiveGhost per different car, and the best lap of
//   all; initialize -> load -> GhostLoad, the header checked -- version by track, size, realism, track -- and the
//   packets' positions checked for sanity by ghost_appears_valid / point_is_normal), picks the one to race against by
//   the race's ghost type, and adds it to the car list (add_ghost_car, set_ghostcar_entry: the car as its file says).
//   In the race, RaceDeity::UpdateCar hands each ghostable car's new best lap to GhostNewBestLap (the physics thread):
//   the lap's packets are copied out of the replay ring (feed_ghost_car) into its LiveGhost, offered to the ghost car
//   (GhostCar::NewLap) and kept as the best of all when it is. At WorldEnd the laps not yet saved are written back
//   (GhostSaveCars -> save_ghostcars -> try_saving -> save: the header, then the packets), and GhostEnd frees it all.
//   GhostGetTarget / GetGhostInfo / GhostGetData / GhostCarIncognito are the other libraries' view of it.
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this file's own too, so a
// hooked rewrite is what runs), the compiler's inline string code as it runs it (ui_types.h), the copies as the
// original's `rep movsd` (forward, a dword at a time), byte arguments pushed as the whole register the original pushes
// where its upper bytes are known (else the byte: every callee reads only the byte). x87 (convert_ticks_to_time,
// ghost_appears_valid, point_is_normal): register values doubles, stored ones floats, the original's grouping and its
// constants' widths, comparisons with the original's NaN outcome (docs/PORTING.md).
//
// Footprints. Main thread unless said: replay_only for whatever allocates, frees, loads or saves a file, creates a
// directory or loads a car (GhostBegin, add_ghost_car, create_ghostcar_dir, load_old_stats, initialize, load, GhostLoad,
// GhostSaveCars, save_ghostcars, try_saving, both saves, GhostEnd, remove_ghostcar, LiveGhost's constructor through
// load_old_stats only -- it writes just its object, so it's shadow-checked). The rest write their outputs and statics.
// GhostNewBestLap and GhostGetData run on the physics thread (RaceDeity::UpdateCar, GhostCar::reset): the first writes
// the car's LiveGhost and the best one (0x23458 bytes each) and the ghost car's pending lap; the lap offered for
// submission would resume a task, so a check with a submission slot is replay_only (there never is one: nothing
// allocates it).
//
// FIX CANDIDATEs (left faithful, marked in place): gf_version indexes its table by GetTrackNumber unchecked (-1 for a
// track outside the table reads the ghost target as the version); load_old_stats numbers a repeated car by its place in
// the list of cars seen, not by its LiveGhost (two cars each repeated -- a network race -- index past the ghosts);
// initialize copies as many packets as a file's header claims (a damaged file overruns the LiveGhost), and
// ghost_appears_valid reads them up to that count; the ghost type's switch reads the target's slot unchecked.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "root_replay.h"

namespace {
namespace root_ghost {
using namespace uit;
using namespace rrep;

#define D(x) ((double)(x))
#define CP(p) ((const char*)(uintptr_t)(p))
static __forceinline uint32_t U(const volatile void* p) { return (uint32_t)(uintptr_t)p; }
#define G32(a) UI_G32(a)
#define GU32(a) UI_GU32(a)
#define G8(a) UI_G8(a)
#define GP(T, a) UI_GP(T, a)
static __forceinline LiveGhost* ghost_at(int32_t slot) {       // GHOSTS + slot * 0x23458 (imul-free, as the original: wraps)
    return (LiveGhost*)(uintptr_t)(GU32(S_GHOSTS) + (uint32_t)slot * LIVE_GHOST_BYTES);
}
static __forceinline volatile int32_t& slot_of(int32_t car) { return *(volatile int32_t*)(uintptr_t)(S_SLOTS + 4u * (uint32_t)car); }
typedef void(__cdecl* Verbose_t)(const char*, ...);
#define VERBOSE_ ((Verbose_t)(uintptr_t)F_VERBOSE)
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
#define ASSERT_MSG_ ((AssertMsg_t)(uintptr_t)F_ASSERT_MSG)
static __forceinline void copy_ghost(void* d, const void* s) { crt_copy(d, s, LIVE_GHOST_BYTES); }   // rep movsd x 0x8d16

static void fp_none0(Footprint&) {}
static void fp_pure0(Footprint& f) { f.pure = true; }

// ---- munge_carname_to_4char (0x40dc70): a car's four letters in a file name ("viper" -> "vipr", "vipergt" -> "vgt"),
// else its first four (strncpy: padded with zeros, unterminated) -----------------------------------------------------------
static void __cdecl munge_carname_to_4char_n(char* dst, const char* name) {
    uint32_t e = S_MUNGE;
    int32_t i = 0;
    do {
        if (ccall<int>(F_stricmp, name, GP(const char, e)) == 0) {
            crt_strcpy(dst, GP(const char, S_MUNGE + 4 + 8u * (uint32_t)i));
            return;
        }
        e += 8;
        i++;
    } while (e < S_MUNGE + 0x10);
    ccall<char*>(F_strncpy, dst, name, 4);
}
static void fp_munge(Footprint& f, char* dst, const char*) { f.add(dst, 8, "the four letters (the table's are 3 and 4)"); }
PORT_FN(0x0040dc70, "munge_carname_to_4char", munge_carname_to_4char_n, fp_munge)

// ---- GhostBegin (0x40dce0) --------------------------------------------------------------------------------------------------
static void __cdecl GhostBegin_n(uint8_t* w) {
    for (int32_t i = 0; *(volatile int32_t*)(w + W_NCARS) > i; i++)
        if (*(volatile int32_t*)(w + W_CARS + CAR_ENTRY * (uint32_t)i) == 3) UI_LogPanic(CP(0x004e549c));
    if (*(volatile int32_t*)(w + W_EVENT) != 0) return;
    G32(S_TARGET) = -1;
    G8(S_INCOGNITO) = 0;
    ccall<void>(F_create_ghostcar_dir);
    ccall<void>(F_find_target, w);
    if (G32(S_TARGET) == -1) {
        UI_LogReport(CP(0x004e5508));                                 // "GhostBegin: how bizarre, no player car."
    } else {
        ccall<void>(F_load_old_stats, w);
        UI_LogReport(CP(0x004e54f8));                                 // "no submission"
        G32(S_SUBMIT) = 0;
        G32(S_TASK) = 0;
        if (*(volatile int32_t*)(w + W_GHOST) != 0) {
            ccall<void>(F_add_ghost_car, w);
            G8(S_INCOGNITO) = (uint8_t)(*(volatile int32_t*)(w + W_FIELD) != 0 ? 1 : 0);
        }
    }
    int32_t n = 0;
    int32_t k = *(volatile int32_t*)(w + W_NCARS);
    if (k > 0) {
        const volatile int32_t* e = (const volatile int32_t*)(w + W_CARS);
        do {
            if (*e == 3) n++;
            e = (const volatile int32_t*)((const uint8_t*)e + CAR_ENTRY);
        } while (--k);
    }
    if (n > 1) UI_LogPanic(CP(0x004e5530));
}
static void fp_GhostBegin(Footprint& f, uint8_t*) { f.replay_only = "reads the ghost files, allocates the ghosts, loads the ghost car"; }
PORT_FN(0x0040dce0, "GhostBegin", GhostBegin_n, fp_GhostBegin)

// ---- add_ghost_car (0x40ddd0): the target's entry copied to the end of the list as the ghost, its car loaded ----------------
static void __cdecl add_ghost_car_n(uint8_t* w) {
    const int32_t n = *(volatile int32_t*)(w + W_NCARS);
    uint8_t* cars = w + W_CARS;
    uint8_t* e = cars + CAR_ENTRY * (uint32_t)n;
    const int32_t t = G32(S_TARGET);
    ccall<void>(F_set_ghostcar_entry, e, cars + CAR_ENTRY * (uint32_t)t);
    ccall<void>(F_LoadCar, e + 0x11);
    *(volatile int32_t*)(cars + CL_COUNT) = *(volatile int32_t*)(cars + CL_COUNT) + 1;
}
static void fp_add_ghost_car(Footprint& f, uint8_t*) { f.replay_only = "loads the ghost's car"; }
PORT_FN(0x0040ddd0, "add_ghost_car", add_ghost_car_n, fp_add_ghost_car)

// ---- set_ghostcar_entry (0x40de20): the ghost's entry: its file's car (GetGhostInfo), else the target's; a ghost (3) -------
static void __cdecl set_ghostcar_entry_n(uint8_t* dst, const uint8_t* src) {
    uint8_t info[0xcc];                                           // GhostInfo (uninitialised when there's none, as the original)
    const uint8_t* s;
    if (ccall<uint8_t>(F_GetGhostInfo, info)) s = info;
    else s = src;
    crt_copy(dst, s, 0xc8);
    *(volatile int32_t*)dst = 3;
    VERBOSE_(CP(0x004e5568), dst + 4, dst + 0x11, src + 4);          // "ghostcar is %s in %s (target: %s)"
}
static void fp_set_ghostcar_entry(Footprint& f, uint8_t* dst, const uint8_t*) { f.add(dst, 0xc8, "the ghost's car list entry"); }
PORT_FN(0x0040de20, "set_ghostcar_entry", set_ghostcar_entry_n, fp_set_ghostcar_entry)

// ---- create_ghostcar_dir (0x40dea0): "<user>ghostcar" ------------------------------------------------------------------------
static void __cdecl create_ghostcar_dir_n() {
    char buf[0x104];
    const char* u = ccall<const char*>(F_Win32GetUserDirectory);
    UI_sprintf(buf, CP(S_GHOST_DIR_FMT), u);
    ccall<uint8_t>(F_FileCreateDirectory, buf);
}
static void fp_create_dir(Footprint& f) { f.replay_only = "creates a directory"; }
PORT_FN(0x0040dea0, "create_ghostcar_dir", create_ghostcar_dir_n, fp_create_dir)

// ---- find_target (0x40ded0): the first player car (subtype 0) ------------------------------------------------------------------
static void __cdecl find_target_n(const uint8_t* w) {
    const int32_t n = *(const volatile int32_t*)(w + W_NCARS);
    const uint8_t* e = w + W_CARS;
    int32_t i = 0;
    if (n <= i) return;
    do {
        if (*(const volatile int32_t*)e == 0) {
            G32(S_TARGET) = i;
            return;
        }
        e += CAR_ENTRY;
        i++;
    } while (n > i);
}
static void fp_find_target(Footprint& f, const uint8_t*) { UI_FP(f, S_TARGET, 4, "ghost.obj target"); }
PORT_FN(0x0040ded0, "find_target", find_target_n, fp_find_target)

// ---- load_old_stats (0x40df00) -------------------------------------------------------------------------------------------------
static void __cdecl load_old_stats_n(uint8_t* w) {
    struct Seen { const char* name; int32_t car; };
    Seen seen[16];                                                // the cars seen, in order (16 at most: the car list's)
    Seen* end = seen;
    int32_t nseen = 0;
    int32_t nghosts = 0;
    uint8_t* cars = w + W_CARS;
    for (int32_t i = 0; *(volatile int32_t*)(cars + CL_COUNT) > i; i++) {
        uint8_t* e = cars + CAR_ENTRY * (uint32_t)i;
        volatile int32_t* slot = (volatile int32_t*)(uintptr_t)(S_SLOTS + 4u * (uint32_t)i);
        const char* name = (const char*)(e + 0x11);
        if (!ccall<uint8_t>(F_pre_worldbegin_is_ghostable_car, e, i)) {
            *slot = 0x7fffffff;
            continue;
        }
        int32_t k = 0;
        uint8_t found = 0;
        if (end > seen) {
            Seen* p = seen;
            do {
                if (ccall<int>(F_stricmp, p->name, name) == 0) { found = 1; break; }
                p++;
                k++;
            } while (k < nseen);
        }
        // FIX CANDIDATE: a repeated car gets k, its place among the cars seen, which counts every car seen (the repeats
        // too) where the ghosts count only the different ones: after two repeated cars (A A B B -- a network race's
        // humans) the second repeat's slot is past the ghosts, and GhostNewBestLap / the ghost's pick use it
        if (!found) *slot = nghosts++;
        else *slot = k;
        end->name = name;
        nseen++;
        end->car = i;
        end++;
    }
    if (nghosts == 0) nghosts = 1;
    VERBOSE_(CP(0x004e5598), nghosts);                                // "ghost: different cars: %d"
    G32(S_NGHOSTS) = nghosts;
    uint8_t* arr = ccall<uint8_t*>(F_MemAlloc, (int32_t)((uint32_t)nghosts * LIVE_GHOST_BYTES));
    if (arr) {
        uint8_t* p = arr;
        for (int32_t n = nghosts - 1; n >= 0; n--) {
            tcall<void*>(F_LiveGhost_ctor, p);
            p += LIVE_GHOST_BYTES;
        }
        GU32(S_GHOSTS) = U(arr);
    } else {
        G32(S_GHOSTS) = 0;
    }
    LiveGhost* best = ccall<LiveGhost*>(F_MemAlloc, (int32_t)LIVE_GHOST_BYTES);
    if (best) {                                                   // LiveGhost::LiveGhost, inline
        uint8_t* q = (uint8_t*)best + 0xc;
        for (int32_t n = MAX_PACKETS - 1; n >= 0; n--) {
            tcall<void*>(F_ReplayPacket_ctor, q);
            q += PACKET_BYTES;
        }
        best->tried = 0;
        best->valid = 0;
        best->dirty = 0;
        best->best = 0x7fffffff;
        GU32(S_BEST) = U(best);
    } else {
        G32(S_BEST) = 0;
    }
    // the flag goes in the low byte of the dword that held the list's end (`setge cl; mov [esp+0x14], cl`); the callees
    // read only the byte
    const uint32_t flag = (U(end) & ~0xffu) | (*(volatile int32_t*)(w + W_RACE_TYPE) >= 4 ? 1u : 0u);
    LiveGhost* pick = 0;
    if (nseen > 0) {
        const char* track = (const char*)(w + W_TRACK);
        const Seen* p = seen;
        for (int32_t k = nseen; k; k--, p++) {
            LiveGhost* g = ghost_at(slot_of(p->car));
            const uint8_t mirror = *(volatile uint8_t*)(w + W_MIRROR);
            if (ccall<uint8_t>(F_initialize, g, 3, *(volatile int32_t*)(w + W_REALISM), flag, track, (uint32_t)mirror, p->name))
                if (!pick || !(pick->best <= g->best)) pick = g;
        }
    }
    {
        const uint8_t mirror = *(volatile uint8_t*)(w + W_MIRROR);
        // (`mov al, [w + 0xcd1]; push eax`: the rest of eax is the world pointer)
        const uint32_t m = (U(w) & ~0xffu) | mirror;
        LiveGhost* b = GP(LiveGhost, S_BEST);
        if (!ccall<uint8_t>(F_initialize, b, 2, *(volatile int32_t*)(w + W_REALISM), flag, (const char*)(w + W_TRACK), m,
                            (const char*)0) &&
            pick)
            copy_ghost(GP(LiveGhost, S_BEST), pick);
    }
    const uint32_t type = *(volatile uint32_t*)(w + W_GHOST);
    switch (type) {
    case 0: G32(S_CURRENT) = 0; break;
    case 1: {
        // FIX CANDIDATE: the target's slot is read unchecked (0x7fffffff for a car that isn't ghostable: the pointer is
        // garbage and the byte store faults); the target is always the first player car, which is ghostable
        LiveGhost* g = ghost_at(slot_of(G32(S_TARGET)));
        GU32(S_CURRENT) = U(g);
        g->valid = 0;
        break;
    }
    case 2: GU32(S_CURRENT) = GU32(S_BEST); break;
    case 3: GU32(S_CURRENT) = U(ghost_at(slot_of(G32(S_TARGET)))); break;
    default: UI_LogPanic(CP(0x004e55dc), type); break;               // "ghost: unrecognized case %d"
    }
    if (G32(S_CURRENT) != 0) GP(LiveGhost, S_CURRENT)->car = *(volatile int32_t*)(w + W_NCARS);
}
static void fp_load_old_stats(Footprint& f, uint8_t*) { f.replay_only = "allocates the ghosts, reads their files"; }
PORT_FN(0x0040df00, "load_old_stats", load_old_stats_n, fp_load_old_stats)

// ---- initialize (0x40e280): a LiveGhost's lap from its file, once -------------------------------------------------------------
static uint8_t __cdecl initialize_n(LiveGhost* g, int32_t type, int32_t realism, uint32_t flag, const char* track, uint32_t mirror,
                                    const char* name) {
    if (g->tried != 0) return 1;
    g->tried = 1;
    uint8_t* f = ccall<uint8_t*>(F_load, name, type, realism, flag, track, mirror);
    if (!f) return 0;
    crt_copy(&g->h, f, HEADER_BYTES);
    g->best = g->h.ticks;
    // FIX CANDIDATE: the header's packet count isn't checked against the LiveGhost's 0x960 (or the file's size): a
    // damaged file with a bigger one overruns the LiveGhost (and reads past its own buffer)
    const uint32_t n = (uint32_t)g->h.count * 60u >> 2;
    crt_copy(g->packets, f + HEADER_BYTES, n << 2);
    g->valid = 1;
    ccall<void>(F_MemFree, f);
    return 1;
}
static void fp_initialize(Footprint& f, LiveGhost*, int32_t, int32_t, uint32_t, const char*, uint32_t, const char*) {
    f.replay_only = "reads a ghost file (allocates, frees)";
}
PORT_FN(0x0040e280, "initialize", initialize_n, fp_initialize)

// ---- load (0x40e320): a ghost file for this car, track and realism (types 0 and 1 have none) ----------------------------------
static uint8_t* __cdecl load_n(const char* name, int32_t type, int32_t realism, uint32_t flag, const char* track, uint32_t mirror) {
    char path[0x100];
    uint8_t* gf = 0;
    const uint8_t ok = type == 1 ? 0 : type != 0 ? 1 : 0;
    if (!ok) return 0;
    ccall<void>(F_get_full_pathname, path, name, type, realism, flag, track, mirror);
    gf = ccall<uint8_t*>(F_GhostLoad, path, 0);
    if (!gf) return 0;
    uint8_t good = 0;
    if (ccall<int>(F_stricmp, (const char*)(gf + 0x10), track) != 0) UI_LogReport(CP(0x004e55f8));   // "... Mismatched track"
    else if (*(volatile int32_t*)(gf + 0xc) != realism) UI_LogReport(CP(0x004e5614));             // "... Mismatched realism"
    else good = 1;
    if (!good) {
        ccall<void>(F_MemFree, gf);
        gf = 0;
    }
    return gf;
}
static void fp_load(Footprint& f, const char*, int32_t, int32_t, uint32_t, const char*, uint32_t) { f.replay_only = "reads a ghost file (allocates, frees)"; }
PORT_FN(0x0040e320, "load", load_n, fp_load)

// ---- get_full_pathname (0x40e3f0): "<user>ghostcar\<file>" -----------------------------------------------------------------
static void __cdecl get_full_pathname_n(char* buf, const char* name, int32_t type, int32_t realism, uint32_t flag, const char* track,
                                        uint32_t mirror) {
    char file[0x10];
    ccall<void>(F_get_filename, file, name, type, realism, flag, track, mirror);
    const char* u = ccall<const char*>(F_Win32GetUserDirectory);
    UI_sprintf(buf, CP(S_GHOST_PATH_FMT), u, file);
}
static void fp_get_full_pathname(Footprint& f, char* buf, const char*, int32_t, int32_t, uint32_t, const char*, uint32_t) {
    // the callers' buffers are 0x100 bytes; the user directory is at most 200 characters, so the path fits
    f.add(buf, 0x100, "the path");
}
PORT_FN(0x0040e3f0, "get_full_pathname", get_full_pathname_n, fp_get_full_pathname)

// ---- get_filename (0x40e440): the track's first three letters, a letter for the realism (+2 the flag, +1 mirrored), the
// car's four ("____" for the best lap of all), ".gcf" ------------------------------------------------------------------------
static void __cdecl get_filename_n(char* buf, const char* name, int32_t type, int32_t realism, uint32_t flag, const char* track,
                                   uint32_t mirror) {
    ccall<char*>(F_strncpy, buf, track, 3);
    const uint8_t c = *(volatile uint8_t*)(uintptr_t)(S_REALISM_CHARS + (uint32_t)realism);
    *(volatile uint8_t*)(buf + 3) = (uint8_t)(c + 2u * (uint8_t)flag + (uint8_t)mirror);
    if (type == 3) ccall<void>(F_munge_carname_to_4char, buf + 4, name);
    else crt_strcpy(buf + 4, CP(S_NO_CAR));
    *(volatile char*)(buf + 8) = 0;
    // strcat of a literal the compiler knew: its five bytes (a dword and a byte) at the end
    char* e = buf + crt_strlen(buf);
    *(volatile uint32_t*)e = GU32(S_GCF);
    *(volatile uint8_t*)(e + 4) = G8(S_GCF + 4);
}
static void fp_get_filename(Footprint& f, char* buf, const char*, int32_t, int32_t, uint32_t, const char*, uint32_t) {
    f.add(buf, 0x10, "the file name");
}
PORT_FN(0x0040e440, "get_filename", get_filename_n, fp_get_filename)

// ---- pre_worldbegin_is_ghostable_car (0x40e4e0): a network race's humans, else the player's ------------------------------------
static uint8_t __cdecl pre_worldbegin_is_ghostable_car_n(const uint8_t* e, int32_t i) {
    if (ccall<uint8_t>(F_MultiEnabled) && ccall<uint8_t>(F_MultiCarIsHuman, i)) return 1;
    return *(const volatile int32_t*)e == 0 ? 1 : 0;
}
static void fp_pre_ghostable(Footprint&, const uint8_t*, int32_t) {}
PORT_FN(0x0040e4e0, "pre_worldbegin_is_ghostable_car", pre_worldbegin_is_ghostable_car_n, fp_pre_ghostable)

// ---- GhostSaveCars (0x40e510) ----------------------------------------------------------------------------------------------
static void __cdecl GhostSaveCars_n() {
    if (!ccall<uint8_t>(F_module_ok)) return;
    ccall<void>(F_save_ghostcars);
    if (const int32_t t = G32(S_TASK)) ccall<void>(F_TaskDestroy, t);
}
static void fp_saves(Footprint& f) { f.replay_only = "saves the ghost files"; }
PORT_FN(0x0040e510, "GhostSaveCars", GhostSaveCars_n, fp_saves)

// ---- module_ok (0x40e540): GhostBegin made the ghosts ----------------------------------------------------------------------
static uint8_t __cdecl module_ok_n() { return G32(S_BEST) != 0 ? 1 : 0; }
PORT_FN(0x0040e540, "module_ok", module_ok_n, fp_none0)

// ---- save_ghostcars (0x40e550) ---------------------------------------------------------------------------------------------
static void __cdecl save_ghostcars_n() {
    int32_t i = 0;
    if (G32(S_NGHOSTS) > i) {
        uint32_t off = 0;
        do {
            LiveGhost* g = (LiveGhost*)(uintptr_t)(GU32(S_GHOSTS) + off);
            i++;
            off += LIVE_GHOST_BYTES;
            ccall<void>(F_try_saving, g, 3);
        } while (i < G32(S_NGHOSTS));
    }
    ccall<void>(F_try_saving, GP(LiveGhost, S_BEST), 2);
}
PORT_FN(0x0040e550, "save_ghostcars", save_ghostcars_n, fp_saves)

// ---- try_saving (0x40e5a0): a new lap that's no worse than its file's ------------------------------------------------------
static void __cdecl try_saving_n(LiveGhost* g, int32_t type) {
    if (g->valid == 0 || g->dirty == 0) return;
    if (g->best < g->h.ticks) return;
    if (!ccall<uint8_t>(F_is_savable_ghost, (int32_t)g->car)) return;
    ccall<void>(F_save_type, g, type);
}
static void fp_try_saving(Footprint& f, LiveGhost*, int32_t) { f.replay_only = "saves a ghost file"; }
PORT_FN(0x0040e5a0, "try_saving", try_saving_n, fp_try_saving)

// ---- save (0x40e5e0): its file's path, from the race's options and the car ------------------------------------------------------
static void __cdecl save_type_n(LiveGhost* g, int32_t type) {
    char path[0x100];
    const uint8_t flag = *(const volatile int32_t*)(ccall<const uint8_t*>(F_WorldGameOptions) + 0x10) >= 4 ? 1 : 0;
    const uint8_t* o = ccall<const uint8_t*>(F_WorldGameOptions);
    // (`mov al, [o + 0x25]; push eax`: the rest of eax is the options pointer)
    const uint32_t mirror = (U(o) & ~0xffu) | *(const volatile uint8_t*)(o + 0x25);
    const char* track = ccall<const char*>(F_WorldGetTrackname);
    const int32_t realism = *(const volatile int32_t*)ccall<const uint8_t*>(F_WorldGameOptions);
    const uint8_t* e = ccall<const uint8_t*>(F_WorldGetCarEntry, (int32_t)g->car);
    ccall<void>(F_get_full_pathname, path, (const char*)(e + 0x11), type, realism, (uint32_t)flag, track, mirror);
    ccall<void>(F_save_path, g, (const char*)path);
}
PORT_FN(0x0040e5e0, "save(LiveGhost*,GhostCarType)", save_type_n, fp_try_saving)

// ---- save (0x40e650): the header, then the packets --------------------------------------------------------------------------
typedef double(__cdecl* TicksToTime_t)(int32_t);
static void __cdecl save_path_n(LiveGhost* g, const char* path) {
    int32_t fd;
    GhostHeader h;                                                // (0xec..0x1c0 never written: the stack's bytes, as the original)
    const uint8_t* pk = g->packets;
    const int32_t car = g->car;
    volatile int32_t* gd = &g->h.ticks;
    const char* track = ccall<const char*>(F_WorldGetTrackname);
    const uint8_t* e = ccall<const uint8_t*>(F_WorldGetCarEntry, car);
    h.version = ccall<uint32_t>(F_gf_version, track);
    h.max_packets = G32(S_MAX_PACKETS);
    h.size = G32(S_FILE_SIZE);
    h.realism = *(const volatile int32_t*)ccall<const uint8_t*>(F_WorldGameOptions);
    crt_strcpy(h.track, track);
    h.mirror = *(const volatile uint8_t*)(ccall<const uint8_t*>(F_WorldGameOptions) + 0x25);
    h.lap_time = (float)((TicksToTime_t)(uintptr_t)F_convert_ticks_to_time)(gd[0]);
    crt_copy(h.car_entry, e, 0xc8);
    h.ticks = gd[0];
    h.start = gd[1];
    h.count = gd[2];
    VERBOSE_(CP(0x004e5658), e + 0x11, path);                        // "SAVING car %s (%s)"
    ccall<void>(F_FileSetWritable, path, 1);
    ccall<uint8_t>(F_FileRemove, path);
    fd = ccall<int32_t>(F_FileCreate, path);
    ccall<uint8_t>(F_FileWrite, fd, (const void*)&h, 0x1cc);
    ccall<uint8_t>(F_FileWrite, fd, pk, (int32_t)((uint32_t)gd[2] * 60u));
    ccall<void>(F_FileClose, &fd);
}
static void fp_save_path(Footprint& f, LiveGhost*, const char*) { f.replay_only = "writes a ghost file"; }
PORT_FN(0x0040e650, "save(LiveGhost*,char const*)", save_path_n, fp_save_path)

// ---- gf_version (0x40e7a0): a ghost file's version, by track ----------------------------------------------------------------
static uint32_t __cdecl gf_version_n(const char* track) {
    const int32_t t = ccall<int32_t>(F_GetTrackNumber, track);
    // FIX CANDIDATE: the track number indexes the eight versions unchecked: a track outside the table (-1) reads the
    // ghost target (0x4e53fc) as its version, a number past 7 the strings after the table
    return GU32(S_GF_VERSIONS + 4u * (uint32_t)t);
}
static void fp_gf_version(Footprint&, const char*) {}
PORT_FN(0x0040e7a0, "gf_version", gf_version_n, fp_gf_version)

// ---- convert_ticks_to_time (0x40e7c0): ticks x 0.016 (the register's value) --------------------------------------------------
static const float k_tick = 0.016f;                             // 0x3c83126f (0x4db300)
static double __cdecl convert_ticks_to_time_n(int32_t t) { return D(t) * D(k_tick); }
static void fp_ticks(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x0040e7c0, "convert_ticks_to_time", convert_ticks_to_time_n, fp_ticks)

// ---- is_savable_ghost (0x40e7e0) / is_ghostable_car (0x40eb40): a human's car (CarMgrGetInfo twice, as the original) ---------
static uint8_t __cdecl is_savable_ghost_n(int32_t car) {
    ccall<const uint8_t*>(F_CarMgrGetInfo, car);
    const uint8_t* info = ccall<const uint8_t*>(F_CarMgrGetInfo, car);
    return *(const volatile uint8_t*)(info + 4) != 0 ? 1 : 0;
}
static void fp_car_pred(Footprint&, int32_t) {}
PORT_FN(0x0040e7e0, "is_savable_ghost", is_savable_ghost_n, fp_car_pred)
static uint8_t __cdecl is_ghostable_car_n(int32_t car) {
    ccall<const uint8_t*>(F_CarMgrGetInfo, car);
    const uint8_t* info = ccall<const uint8_t*>(F_CarMgrGetInfo, car);
    return *(const volatile uint8_t*)(info + 4) != 0 ? 1 : 0;
}
PORT_FN(0x0040eb40, "is_ghostable_car", is_ghostable_car_n, fp_car_pred)

// ---- GhostEnd (0x40e810) -------------------------------------------------------------------------------------------------------
static void __cdecl GhostEnd_n(uint8_t* w) {
    if (!ccall<uint8_t>(F_module_ok)) return;
    ccall<void>(F_remove_ghostcar, w + W_CARS);
    void* b = GP(void, S_BEST);
    G32(S_CURRENT) = 0;
    ccall<void>(F_Delete, b);
    void* a = GP(void, S_GHOSTS);
    G32(S_BEST) = 0;
    ccall<void>(F_Delete, a);
    G32(S_GHOSTS) = 0;
    if (G32(S_SUBMIT) != 0) ccall<void>(F_Delete, GP(void, S_SUBMIT));
    G32(S_SUBMIT) = 0;
    G32(S_TARGET) = -1;
}
static void fp_GhostEnd(Footprint& f, uint8_t*) { f.replay_only = "frees the ghosts, unloads the ghost car"; }
PORT_FN(0x0040e810, "GhostEnd", GhostEnd_n, fp_GhostEnd)

// ---- remove_ghostcar (0x40e890): the first ghost entry's car unloaded, the count one less (the entry stays) -------------------
static void __cdecl remove_ghostcar_n(uint8_t* cl) {
    const int32_t n = *(volatile int32_t*)(cl + CL_COUNT);
    int32_t i = 0;
    if (n <= i) return;
    const uint8_t* e = cl;
    do {
        if (*(const volatile int32_t*)e == 3) {
            ccall<void>(F_UnloadCar, (const char*)(cl + CAR_ENTRY * (uint32_t)i + 0x11));
            *(volatile int32_t*)(cl + CL_COUNT) = *(volatile int32_t*)(cl + CL_COUNT) - 1;
            return;
        }
        e += CAR_ENTRY;
        i++;
    } while (i < n);
}
static void fp_remove_ghostcar(Footprint& f, uint8_t*) { f.replay_only = "unloads the ghost's car"; }
PORT_FN(0x0040e890, "remove_ghostcar", remove_ghostcar_n, fp_remove_ghostcar)

// ---- GhostGetTarget (0x40e8e0) / GhostCarIncognito (0x40e940) -----------------------------------------------------------------
static int32_t __cdecl GhostGetTarget_n() { return G32(S_TARGET); }
PORT_FN(0x0040e8e0, "GhostGetTarget", GhostGetTarget_n, fp_none0)
static uint8_t __cdecl GhostCarIncognito_n() { return G8(S_INCOGNITO) != 0 ? 1 : 0; }
PORT_FN(0x0040e940, "GhostCarIncognito", GhostCarIncognito_n, fp_none0)

// ---- GetGhostInfo (0x40e8f0): the raced ghost's car and lap time, if it has a lap -------------------------------------------------
static uint8_t __cdecl GetGhostInfo_n(uint8_t* out) {
    if (G32(S_CURRENT) != 0 && GP(LiveGhost, S_CURRENT)->valid != 0) crt_copy(out, GP(LiveGhost, S_CURRENT)->h.car_entry, 0xcc);
    if (G32(S_CURRENT) != 0 && GP(LiveGhost, S_CURRENT)->valid != 0) return 1;
    return 0;
}
static void fp_GetGhostInfo(Footprint& f, uint8_t* out) { f.add(out, 0xcc, "GhostInfo"); }
PORT_FN(0x0040e8f0, "GetGhostInfo", GetGhostInfo_n, fp_GetGhostInfo)

// ---- GhostNewBestLap (0x40e950, the physics thread): a ghostable car's new best lap ----------------------------------------------
static void __cdecl GhostNewBestLap_n(int32_t car, int32_t ticks) {
    if (!ccall<uint8_t>(F_is_ghostable_car, car)) return;
    ASSERT_MSG_((uint32_t)slot_of(car) - 0x7fffffffu != 0 ? 1 : 0, CP(0x004e566c), car);     // "car %d is bogus!"
    LiveGhost* g = ghost_at(slot_of(car));
    uint8_t submit = 0;
    if (G32(S_SUBMIT) != 0 && *(const volatile int32_t*)ccall<const uint8_t*>(F_CarMgrGetInfo, car) == 0) {
        if (ccall<int32_t>(F_AIGetMaxGhostSubmitTicks) > ticks) submit = 1;
    }
    uint8_t better;
    if (g->valid == 0) better = 1;
    else better = g->h.ticks > ticks ? 1 : 0;
    {
        const char* s1 = submit ? CP(0x004e5680) : CP(0x004e5688);    // "submit" / ""
        const char* s2 = better ? CP(0x004e568c) : CP(0x004e5694);    // "bestcar" / ""
        const int32_t mx = ccall<int32_t>(F_AIGetMaxGhostSubmitTicks);
        VERBOSE_(CP(0x004e5698), car, (int32_t)slot_of(car), ticks, mx, s2, s1);   // "ghostlap: car %d idx %d ticks %d [%d] %s %s"
    }
    if (!submit && !better) return;
    if (!better) g = GP(LiveGhost, S_SUBMIT);
    g->car = car;
    g->dirty = 1;
    g->valid = 1;
    g->h.ticks = ticks;
    g->h.count = MAX_PACKETS;
    if (ticks > 0x3840) g->h.start = ticks - 0x3840;
    else g->h.start = 0;
    const int32_t back = (int32_t)((uint32_t)*(volatile int32_t*)(uintptr_t)0x0052161c - (uint32_t)ticks);   // physics_tick - ticks
    const int32_t len = (int32_t)((uint32_t)ticks - (uint32_t)g->h.start);
    const int32_t from = (int32_t)((uint32_t)g->h.start + (uint32_t)back);
    const int32_t n = ccall<int32_t>(F_feed_ghost_car, car, from, len, g->packets, &g->h.count);
    g->h.start = (int32_t)((uint32_t)g->h.start + (uint32_t)n);
    if (better) {
        ccall<void>(F_GhostCar_NewLap, &g->h.ticks, g->packets, car);
        LiveGhost* b = GP(LiveGhost, S_BEST);
        if (b->valid == 0 || b->h.ticks > g->h.ticks) copy_ghost(GP(LiveGhost, S_BEST), g);
    }
    if (submit) {
        if (GP(LiveGhost, S_SUBMIT) != g) copy_ghost(GP(LiveGhost, S_SUBMIT), g);
        ccall<void>(F_submit_ghost);
    }
}
static void fp_GhostNewBestLap(Footprint& f, int32_t car, int32_t) {
    const uint8_t* info = (const uint8_t*)(uintptr_t)(0x00554090u + 0x170u * (uint32_t)car);   // CarMgrGetInfo's
    if ((uint32_t)car >= 16u) { f.replay_only = "a car outside the car manager's sixteen"; return; }
    if (!info[4]) return;                                        // not ghostable: nothing written
    if (G32(S_SUBMIT) != 0) { f.replay_only = "a submission slot: the lap may be submitted (TaskResume)"; return; }
    const int32_t s = slot_of(car);
    if (s < 0 || s >= G32(S_NGHOSTS) || !G32(S_GHOSTS) || !G32(S_BEST)) { f.replay_only = "a car without a live ghost"; return; }
    f.add(ghost_at(s), LIVE_GHOST_BYTES, "the car's live ghost");
    f.add(GP(void, S_BEST), LIVE_GHOST_BYTES, "the best live ghost");
    if (uint8_t* gc = GP(uint8_t, 0x0052197c)) f.add(gc + 0xee4, 16, "the ghost car's pending lap (GhostCar::NewLap)");
}
PORT_FN(0x0040e950, "GhostNewBestLap", GhostNewBestLap_n, fp_GhostNewBestLap)

// ---- ASSERT_MSG (0x40eb30) / VERBOSE (0x40eea0): ghost.obj's, empty (variadic: the caller pops) --------------------------------
static void __cdecl ASSERT_MSG_n(int32_t, const char*) {}
static void fp_assert(Footprint& f, int32_t, const char*) { f.pure = true; }
PORT_FN(0x0040eb30, "ASSERT_MSG(ghost.obj)", ASSERT_MSG_n, fp_assert)
static void __cdecl VERBOSE_n(const char*) {}
static void fp_verbose(Footprint& f, const char*) { f.pure = true; }
PORT_FN(0x0040eea0, "VERBOSE(ghost.obj)", VERBOSE_n, fp_verbose)

// ---- submit_ghost (0x40eb70): the submitter's task resumed (never made: the handle is 0) ----------------------------------------
static void __cdecl submit_ghost_n() { ccall<void>(F_TaskResume, G32(S_TASK)); }
static void fp_submit(Footprint& f) { f.replay_only = "TaskResume (the thread table; LogPanic for a task that isn't suspended)"; }
PORT_FN(0x0040eb70, "submit_ghost", submit_ghost_n, fp_submit)

// ---- GhostGetData (0x40eb80, the physics thread): the raced ghost's lap: its car, its GhostData, its packets -----------------
static uint8_t __cdecl GhostGetData_n(int32_t* gd, uint8_t** packets, int32_t* car) {
    if (G32(S_CURRENT) == 0 || GP(LiveGhost, S_CURRENT)->valid == 0) return 0;
    *car = GP(LiveGhost, S_CURRENT)->car;
    LiveGhost* c = GP(LiveGhost, S_CURRENT);
    const int32_t t = c->h.ticks;
    gd[0] = t;
    gd[1] = c->h.start;
    gd[2] = c->h.count;
    *packets = GP(LiveGhost, S_CURRENT)->packets;
    return 1;
}
static void fp_GhostGetData(Footprint& f, int32_t* gd, uint8_t** packets, int32_t* car) {
    f.add(gd, 12, "GhostData");
    f.add(packets, 4, "the packets' pointer");
    f.add(car, 4, "the car");
}
PORT_FN(0x0040eb80, "GhostGetData", GhostGetData_n, fp_GhostGetData)

// ---- GhostLoad (0x40ebe0): a ".gcf" file read whole (or just its header), checked; a bad one is deleted --------------------------
static uint8_t* __cdecl GhostLoad_n(const char* path, uint8_t header_only) {
    uint8_t* gf = 0;
    int32_t fd = ccall<int32_t>(F_FileOpen, path);
    if (fd == 0) return 0;
    uint8_t ok = 0;
    int32_t size = ccall<int32_t>(F_FileSize, fd);
    if (size > 0 && G32(S_FILE_SIZE) > size) {
        if (header_only) size = HEADER_BYTES;
        gf = ccall<uint8_t*>(F_MemAlloc, size);
        if (ccall<uint8_t>(F_FileReadExact, fd, gf, size)) {
            const char* why = 0;
            if (header_only && (uint32_t)*(volatile int32_t*)(gf + 8) - GU32(S_FILE_SIZE) == 0xfffffffcu) {   // an old version, four bytes short
                size -= 0xe8;
                UI_LogReport(CP(0x004e56c4));                         // "ghostcar OLD VERSION! Converto!"
                ccall<void*>(F_memmove, gf + 0xe8, gf + 0xe4, size);
                *(volatile int32_t*)(gf + 8) = G32(S_FILE_SIZE);
            }
            if (*(volatile int32_t*)(gf + 8) != G32(S_FILE_SIZE) || *(volatile int32_t*)(gf + 4) != G32(S_MAX_PACKETS))
                why = CP(0x004e56e4);                                 // "ghostcar: Mismatched size"
            else if (!header_only && !ccall<uint8_t>(F_ghost_appears_valid, gf))
                why = CP(0x004e5700);                                 // "ghostcar: Bogus data"
            else if (ccall<uint32_t>(F_gf_version, (const char*)(gf + 0x10)) != *(volatile uint32_t*)gf)
                why = CP(0x004e5718);                                 // "ghostcar: Bad Version"
            else
                ok = 1;
            if (why) UI_LogReport(why);
        }
        if (!ok) {
            ccall<void>(F_MemFree, gf);
            gf = 0;
        }
    }
    ccall<void>(F_FileClose, &fd);
    if (!ok) {
        ccall<void>(F_FileSetWritable, path, 1);
        ccall<uint8_t>(F_FileRemove, path);
    }
    return gf;
}
static void fp_GhostLoad(Footprint& f, const char*, uint8_t) { f.replay_only = "reads a ghost file (allocates; deletes a bad one)"; }
PORT_FN(0x0040ebe0, "GhostLoad", GhostLoad_n, fp_GhostLoad)

// ---- ghost_appears_valid (0x40ed40): every packet's position a normal number, and within 10000 of the one before ---------------
static uint8_t __cdecl ghost_appears_valid_n(uint8_t* gf) {
    const uint8_t* p = gf + HEADER_BYTES;
    int32_t i = 1;
    uint8_t ok = ccall<uint8_t>(F_point_is_normal, gf + 0x1d2);
    if (!ok) return ok;
    p += 0x42;
    // FIX CANDIDATE: the count is the header's, unchecked against the file's size: a damaged file reads past its buffer
    while (*(volatile int32_t*)(gf + 0x1c8) > i) {
        if (!ccall<uint8_t>(F_point_is_normal, p)) {
            ok = 0;
        } else {
            volatile float d[3];
            d[0] = (float)(D(*(const volatile float*)(p + 0)) - D(*(const volatile float*)(p - 0x3c)));
            d[1] = (float)(D(*(const volatile float*)(p + 4)) - D(*(const volatile float*)(p - 0x38)));
            d[2] = (float)(D(*(const volatile float*)(p + 8)) - D(*(const volatile float*)(p - 0x34)));
            const double s = (D(d[1]) * D(d[1]) + D(d[2]) * D(d[2])) + D(d[0]) * D(d[0]);
            if (!!(s > D(1e8f))) ok = 0;                          // fcomp; test ah, 0x41: jne keeps it
        }
        p += 0x3c;
        i++;
        if (!ok) break;
    }
    return ok;
}
static void fp_ghost_valid(Footprint&, uint8_t*) {}
PORT_FN(0x0040ed40, "ghost_appears_valid", ghost_appears_valid_n, fp_ghost_valid)

// ---- point_is_normal (0x40ede0): x, y, z neither NaN nor infinite (the CRT's _isnan / _finite of each as a double) -------------
typedef int(__cdecl* DblPred_t)(double);
static uint8_t __cdecl point_is_normal_n(const float* p) {
    for (int k = 0; k < 3; k++) {
        const double v = D(((const volatile float*)p)[k]);
        if (((DblPred_t)(uintptr_t)F_isnan)(v) != 0) return 0;
        if (((DblPred_t)(uintptr_t)F_finite)(v) == 0) return 0;
    }
    return 1;
}
static void fp_point(Footprint&, const float*) {}
PORT_FN(0x0040ede0, "point_is_normal", point_is_normal_n, fp_point)

// ---- LiveGhost::LiveGhost (0x40eeb0) / Car::ReplayPacket::ReplayPacket (0x40eef0) ----------------------------------------------
static LiveGhost* __fastcall LiveGhost_ctor_n(LiveGhost* self, Edx) {
    uint8_t* q = self->packets;
    for (int32_t n = MAX_PACKETS - 1; n >= 0; n--) {
        tcall<void*>(F_ReplayPacket_ctor, q);
        q += PACKET_BYTES;
    }
    self->tried = 0;
    self->valid = 0;
    self->dirty = 0;
    self->best = 0x7fffffff;
    return self;
}
static void fp_LiveGhost_ctor(Footprint& f, LiveGhost* self, Edx) { f.add(self, 0xc, "this (its flags and best)"); }
PORT_FN(0x0040eeb0, "LiveGhost::LiveGhost", LiveGhost_ctor_n, fp_LiveGhost_ctor)
static void* __fastcall ReplayPacket_ctor_n(void* self, Edx) { return self; }
static void fp_ReplayPacket_ctor(Footprint& f, void*, Edx) { f.pure = true; }
PORT_FN(0x0040eef0, "Car::ReplayPacket::ReplayPacket", ReplayPacket_ctor_n, fp_ReplayPacket_ctor)

}  // namespace root_ghost
}  // namespace
