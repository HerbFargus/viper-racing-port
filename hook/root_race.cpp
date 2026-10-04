// root_race.cpp -- M3 UI stage, step U3 (group B): the race's tables and its screens before and after, rewritten faithfully
// (library `root`: race.obj, prerace.obj, postrace.obj; main.obj, version.obj and WinMain are root_main.cpp).
//
//   race.obj: the options' names (realism, race type, ghost car type, weather, time of day, AI strength, field, event --
//   the drag events' distances in the locale's units -- and the cameras'), the laps a race type has on a track (the table
//   at 0x4e4698), the tracks' difficulty, RaceBegin / RaceEnd (the *.car list, sorted, and tracks.tab), the tracks' names,
//   friendly names (translated), texts and numbers, and the car list's lookups.
//   prerace.obj: PreRaceDo, the screen before a race (the starting grid -- a StartingGridView of CarPositions, each car's
//   model turning in 3D --, the track's map and text, the race's summary, the best times board, the career's prizes
//   (TrackPrizeInfo), the garage and race / qualify buttons), then the player's setup loaded; garage_cb.
//   postrace.obj: PostRaceDo, the screen after one (the standings -- PostRaceTable --, a car's lap times -- LapsTable, a
//   page of ten, prev / next car and page --, the replay button), until it's left.
//
// Written from the v1.0 disassembly as root_main.cpp is: every call in the original's order by its v1.0 address,
// virtual calls through the vtable, the inline string code as the original runs it (ui_types.h). The item lists the
// screens build on the stack are the original's: an item it builds with UIDialogItem's constructor is built with it, one
// it writes in place is written with the same 14 dwords, each at the same place in a frame laid out as the original's
// (so the pointers the dialog keeps into it -- the group outs, the radio buttons' variable -- are to the same locals).
// x87: register values double, stored ones float, the original's grouping and constants' widths; fsin / fcos results
// stored straight to floats (x87.h); __ftol as x87_ftol; comparisons with the original's NaN outcome.
//
// Footprints (main thread). Draws write the canvas they're given and the current-canvas static, and refresh the Xlators
// they read; a draw whose function-local Xlators aren't all built yet builds them (atexit): replay_only, as U2's. The
// string lookups refresh their Xlators (race.obj's, built by its $E initialisers) and GetEventString formats into its
// three static buffers. LapsTable's page and car steps write the table, its widget's dirty flag and the running window's
// groups (UIShowGroup / UIHideGroup). replay_only: whatever loads, allocates or frees (constructors, destructors,
// RaceBegin / RaceEnd, Create / Destroy -- notifications --, Added -- widgets --, the 3D draw), and the two screens (their
// own dialog loops, the garage, the replay, the board).
//
// Fixes (// FIX:, docs/FIXES.md "Race front end"; none reached with the stock cars, tracks and English text):
// GetEventString's three 0x20-byte texts keep to their 31 characters (a long translation); RaceBegin's car list keeps its
// names to 31 characters (a longer name is left out) and grows past 32 cars; CarPosition's texture remap names
// keep to their 16 bytes (a car name of 12+ characters), and its model and texture names have room for a 31-character
// car; PreRaceDo's track name and friendly name keep to their 64-byte statics. Every other input gives the original's bits.
// (PreRaceDo's frame is the original's whole 0x1808 bytes: the track text is word-wrapped into its last 4 KB.)
//
// FIX CANDIDATEs (left faithful, marked in place; none reached in ordinary play unless noted): the option-name lookups
// index their tables with the value unchecked (an out-of-range option reads their own stack); PreRaceDo loads the
// player's setup from the entry before the World's cars when no car is the player's (a race of AI cars only);
// LapsTable::next_car divides by the car count (0: a divide fault); PostRaceTable::Draw and PostRaceDo read a race
// result (the first; the player's) without checking it exists.
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "root_main.h"

namespace {
namespace root_race {
using namespace uit;
using namespace rootb;

#define D(x) ((double)(x))
#define CP(p) ((const char*)(uintptr_t)(p))
#define LW(F, off) (*(volatile uint32_t*)((F) + (off)))           // a dword of a frame laid out as the original's
#define LA(F, off) ((uint32_t)(uintptr_t)((F) + (off)))           // its address, as a dword
static __forceinline float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline void st_f(volatile float* p, double v) { *p = (float)v; }
static __forceinline uint32_t U(const volatile void* p) { return (uint32_t)(uintptr_t)p; }
// an item written in place: its 14 dwords
static __forceinline void item14(uint8_t* it, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 uint32_t text, uint32_t i1c, uint32_t data, uint32_t style) {
    volatile uint32_t* d = (volatile uint32_t*)it;
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text; d[7] = i1c; d[8] = data; d[9] = style;
    d[10] = 0; d[11] = 0; d[12] = 0; d[13] = 0;
}
static __forceinline void item_end(uint8_t* it) { crt_copy(it, (const void*)(uintptr_t)S_END_ITEM, 0x38); }
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
#define SPRINTF ((Sprintf_t)(uintptr_t)uit::F_sprintf)
typedef void(__cdecl* FontPrintf_t)(void*, void*, uint32_t, int32_t, int32_t, const char*, ...);
#define FPRINTF ((FontPrintf_t)(uintptr_t)uit::F_gxFontPrintf)
typedef void(__cdecl* Assert_t)(int32_t, const char*, ...);
enum : uint32_t { F_stricmp_c = 0x004da350 };

// ---- footprint helpers --------------------------------------------------------------------------------------------------
static __forceinline bool xl_built(uint32_t guard, uint8_t bits) { return (UI_G8(guard) & bits) == bits; }
#define FP_XL(f, a) UI_FP(f, a, 12, "a static Xlator")
static __forceinline void fp_draw_canvas(Footprint& f, gxCanvas* c) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}
static void fp_none(Footprint&) {}
static void fp_none_i(Footprint&, int32_t) {}
static void fp_pure0(Footprint& f) { f.pure = true; }
template <typename T> static void fp_loads0(Footprint& f, T*, Edx) { f.replay_only = "loads or frees (fonts, palettes, stamps, models)"; }
template <typename T> static void fp_dtor_flags(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object and what it holds"; }
template <typename T> static void fp_added0(Footprint& f, T*, Edx) { f.replay_only = "builds widgets (_UIAddItems allocates, loads stamps)"; }
template <typename T> static void fp_notes0(Footprint& f, T*, Edx) { f.replay_only = "adds or removes a notification (allocates, frees)"; }

// =========================================================================================================================
// race.obj
// =========================================================================================================================
// the options' names: race.obj's static Xlators, refreshed and read in the original's order, then indexed
// FIX CANDIDATE (all of them): the value indexes the table unchecked (out of range: the function's own stack)
static const char* __cdecl GetRealismString_c(int32_t v) {
    const char* t[3];
    t[0] = xlate(0x005045a8); t[1] = xlate(0x00504478); t[2] = xlate(0x00504310);
    return t[v];
}
static const uint32_t k_xl_realism[] = {0x005045a8, 0x00504478, 0x00504310};
static const uint32_t k_xl_racetype[] = {0x00504498, 0x005044a8, 0x00504358, 0x00504398, 0x005044d8, 0x005044e8, 0x00504518, 0x005045d8};
static const uint32_t k_xl_ghost[] = {0x005043d8, 0x00504388, 0x005044f8, 0x00504650, 0x005045e8};
static const uint32_t k_xl_weather[] = {0x005043f8, 0x00504588, 0x00504618};
static const uint32_t k_xl_time[] = {0x00504608, 0x005043e8, 0x00504348};
static const uint32_t k_xl_ai[] = {0x005044b8, 0x005045c8, 0x00504578};
static const uint32_t k_xl_field[] = {0x00504418, 0x00504428, 0x00504568};
static const uint32_t k_xl_event[] = {0x00504598, 0x00504508, 0x005043c8, 0x00504640, 0x00504630, 0x00504548};
static const uint32_t k_xl_camera[] = {0x00504528, 0x00504468, 0x00504538, 0x00504408, 0x00504558, 0x005043b8,
                                       0x005045b8, 0x005043a8, 0x005044c8, 0x00504488, 0x005045f8, 0x00504458};
template <const uint32_t* XL, int N> static void fp_xls(Footprint& f, int32_t) { for (int i = 0; i < N; i++) FP_XL(f, XL[i]); }
PORT_FN(0x00405e40, "GetRealismString", GetRealismString_c, (fp_xls<k_xl_realism, 3>))

static const char* __cdecl GetRaceTypeString_c(int32_t v) {
    const char* t[8];
    for (int i = 0; i < 8; i++) t[i] = xlate(k_xl_racetype[i]);
    return t[v];
}
PORT_FN(0x00405ec0, "GetRaceTypeString", GetRaceTypeString_c, (fp_xls<k_xl_racetype, 8>))
static const char* __cdecl GetGhostCarTypeString_c(int32_t v) {
    const char* t[5];
    for (int i = 0; i < 5; i++) t[i] = xlate(k_xl_ghost[i]);
    return t[v];
}
PORT_FN(0x00405fe0, "GetGhostCarTypeString", GetGhostCarTypeString_c, (fp_xls<k_xl_ghost, 5>))
static const char* __cdecl GetWeatherString_c(int32_t v) {
    const char* t[3];
    for (int i = 0; i < 3; i++) t[i] = xlate(k_xl_weather[i]);
    return t[v];
}
PORT_FN(0x004060a0, "GetWeatherString", GetWeatherString_c, (fp_xls<k_xl_weather, 3>))
static const char* __cdecl GetGameTimeString_c(int32_t v) {
    const char* t[3];
    for (int i = 0; i < 3; i++) t[i] = xlate(k_xl_time[i]);
    return t[v];
}
PORT_FN(0x00406120, "GetGameTimeString", GetGameTimeString_c, (fp_xls<k_xl_time, 3>))
static const char* __cdecl GetAIStrengthString_c(int32_t v) {       // three names, then "Career1" .. "Career5"
    const char* t[8];
    for (int i = 0; i < 3; i++) t[i] = xlate(k_xl_ai[i]);
    for (int i = 0; i < 5; i++) t[3 + i] = CP(0x004e464c + 8u * (uint32_t)i);
    return t[v];
}
PORT_FN(0x004061a0, "GetAIStrengthString", GetAIStrengthString_c, (fp_xls<k_xl_ai, 3>))
static const char* __cdecl GetFieldString_c(int32_t v) {            // three names, then "XXX" three times
    const char* t[6];
    for (int i = 0; i < 3; i++) t[i] = xlate(k_xl_field[i]);
    t[3] = CP(0x004e4674); t[4] = CP(0x004e4678); t[5] = CP(0x004e467c);
    return t[v];
}
PORT_FN(0x00406240, "GetFieldString", GetFieldString_c, (fp_xls<k_xl_field, 3>))

// FIX: GetEventString's three texts ("<name> <n>" twice, "<n> <unit>": translations, and the drags' distances in the
// locale's units) are 0x20-byte statics, formatted into unbounded: a long enough translation ran the first two into the
// Xlators after them and the third into the car list's pointer (the car choosers then read the car names from wherever
// the text's bytes pointed). A text that would pass 31 characters is formatted in a buffer of the rewrite's (the
// translation cut to 64 characters) and its first 31 kept. Any other is formatted into the static as before.
static void event_text(uint32_t dst, uint32_t fmt, const char* s, int32_t n, bool n_first) {
    if (VP_FIX && ui_strnlen(s, 0x1f) + 1 + fix_dec_len(n) > 0x1f) {
        char t[0x60], c[0x41];
        const char* sc = fix_cut(s, c, 0x40);
        if (n_first) SPRINTF(t, CP(fmt), n, sc);
        else SPRINTF(t, CP(fmt), sc, n);
        ui_copy_bounded((char*)(uintptr_t)dst, t, 0x20);
    } else if (n_first) {
        SPRINTF((char*)(uintptr_t)dst, CP(fmt), n, s);
    } else {
        SPRINTF((char*)(uintptr_t)dst, CP(fmt), s, n);
    }
}

// GetEventString: the race, the two drags (a quarter mile / 0-60 in the locale's distance), 60-0, ...; the three with
// numbers formatted into their static buffers
static const char* __cdecl GetEventString_c(int32_t v) {
    int32_t n = 0x3c;
    if (*(volatile uint8_t*)(UI_GP(uint8_t, S_LOCALE) + 0x38))
        n = x87_ftol(D(*(volatile float*)(UI_GP(uint8_t, S_LOCALE) + 0x1c)) * D(bits_f(0x41d55555)));
    event_text(S_EVENT_TEXT0, 0x004e4680, xlate(0x00504598), n, false);            // FIX: (event_text) "%s %d"
    n = 0x64;
    if (*(volatile uint8_t*)(UI_GP(uint8_t, S_LOCALE) + 0x38))
        n = x87_ftol(D(*(volatile float*)(UI_GP(uint8_t, S_LOCALE) + 0x1c)) * D(bits_f(0x4231c71c)));
    event_text(S_EVENT_TEXT1, 0x004e4688, xlate(0x00504598), n, false);            // FIX: (event_text) "%s %d"
    event_text(S_EVENT_TEXT2, 0x004e4690, xlate(0x00504508), n, true);             // FIX: (event_text) "%d %s"
    const char* t[7];
    t[0] = xlate(0x005043c8);
    t[1] = CP(S_EVENT_TEXT0);
    t[2] = CP(S_EVENT_TEXT1);
    t[3] = xlate(0x00504640);
    t[4] = xlate(0x00504630);
    t[5] = CP(S_EVENT_TEXT2);
    t[6] = xlate(0x00504548);
    return t[v];
}
static void fp_GetEventString(Footprint& f, int32_t) {
    for (uint32_t a : k_xl_event) FP_XL(f, a);
    UI_FP(f, S_EVENT_TEXT0, 0x20, "race.obj event text (quarter mile)");
    UI_FP(f, S_EVENT_TEXT1, 0x20, "race.obj event text (0-60)");
    UI_FP(f, S_EVENT_TEXT2, 0x20, "race.obj event text (60-0)");
}
PORT_FN(0x004062d0, "GetEventString", GetEventString_c, fp_GetEventString)

static const char* __cdecl GetCameraName_c(int32_t v) {
    const char* t[12];
    for (int i = 0; i < 12; i++) t[i] = xlate(k_xl_camera[i]);
    return t[v];
}
PORT_FN(0x00406450, "GetCameraName", GetCameraName_c, (fp_xls<k_xl_camera, 12>))

// GetLapCountFromType: the track's row of the laps table (8 tracks), the race type's column; a track not there panics
// FIX: a track not in the table (an add-on track under a name of its own) panicked "GetLapCountFromType: Can't match";
// every row of the table holds the same laps (1, 3, 8, 20 for the four race types), so such a track takes the first
// row's. A race type outside the row's 8 columns still reads as the original did.
static int32_t __cdecl GetLapCountFromType_c(int32_t type, const char* name) {
    int32_t i = 0;
    for (uint32_t e = S_LAP_TABLE; e < 0x004e47b8; e += 0x24, i++)
        if (ccall<int>(F_stricmp_c, name, UI_GP(const char, e)) == 0)
            return UI_G32(0x004e469c + (uint32_t)(i * 9 + type) * 4u);
    if (VP_FIX) return UI_G32(0x004e469c + (uint32_t)type * 4u);
    UI_LogPanic(CP(0x004e47fc), name);
    return -1;
}
static void fp_GetLapCountFromType(Footprint&, int32_t, const char*) {}
PORT_FN(0x004065f0, "GetLapCountFromType", GetLapCountFromType_c, fp_GetLapCountFromType)

static int32_t __cdecl GetTrackDifficulty_c(int32_t t) {
    // FIX: a track outside 0..7 (an add-on track's row past the stock 8, or the "no track" -1) read the function's own
    // stack; it now counts as medium (1)
    int32_t d[8];
    d[0] = 2; d[1] = 2; d[2] = 1; d[3] = 2; d[4] = 1; d[5] = 0; d[6] = 1; d[7] = 0;
    if (VP_FIX && (uint32_t)t > 7u) return 1;
    return d[t];
}
static void fp_GetTrackDifficulty(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x00406650, "GetTrackDifficulty", GetTrackDifficulty_c, fp_GetTrackDifficulty)

// RaceBegin: the *.car files' names (without the extension), sorted; tracks.tab
static void __cdecl RaceBegin_c() {
    char buf[0x104];
    UI_GP(void, S_CARLIST) = ccall<void*>(uit::F_MemAlloc, 0x400);
    UI_G32(S_CARLIST_N) = 0;
    uint32_t cap = 0x20;                                     // FIX: (below) the list's room, in names
    void* h = ccall<void*>(uit::F_FileFindFirst, CP(0x004e4828), (char*)buf, 0x104);
    if (h != (void*)(intptr_t)-1) {
        do {
            char* s = ccall<char*>(uit::F_strrchr, (const char*)buf, 0x5c);
            s = s ? s + 1 : buf;
            *(volatile char*)ccall<char*>(uit::F_strchr, (const char*)s, 0x2e) = 0;
            // FIX: the list holds 32 names of 31 characters (MemAlloc(0x400)), neither checked: a longer name ran into the
            // next entry (and was cut there by the next car's name), and a 33rd car ran off the heap block. A longer name
            // is left out of the list (cut, it would no longer open its files -- "<car>.car", "<car>.cf" --, which fails
            // worse), so the Hacks screen's Vehicle list doesn't show it. The 33rd car grows the list instead: a block
            // twice the size (MemAlloc), the names copied over, the old one freed (Delete), so every car the folder lists
            // goes in (RaceEnd frees whichever block is current). The entries stay 32 bytes apart: the original's
            // GetCarFileName, sort_carlist and every name buffer downstream count on 31 characters at most. Only if the
            // memory runs out (or past 0x100000 cars) is a car left out. Up to 32 cars, every call is the original's.
            if (VP_FIX && ui_strnlen(s, 0x1f) > 0x1f) continue;
            if (VP_FIX && (uint32_t)UI_G32(S_CARLIST_N) >= cap) {
                if (cap >= 0x100000u) continue;
                void* grown = ccall<void*>(uit::F_MemAlloc, (int32_t)(cap * 2u * 0x20u));
                if (!grown) continue;
                crt_copy(grown, UI_GP(void, S_CARLIST), cap * 0x20u);
                ccall<void>(uit::F_Delete, UI_GP(void, S_CARLIST));
                UI_GP(void, S_CARLIST) = grown;
                cap *= 2u;
            }
            const uint32_t n = crt_strlen(s) + 1;
            crt_copy(UI_GP(char, S_CARLIST) + ((uint32_t)UI_G32(S_CARLIST_N) << 5), s, n);
            UI_G32(S_CARLIST_N) = UI_G32(S_CARLIST_N) + 1;
        } while (ccall<uint8_t>(uit::F_FileFindNext, h, (char*)buf, 0x104));
        ccall<void>(uit::F_FileFindClose, h);
        ccall<void>(F_sort_carlist);
    }
    UI_GP(void, S_TRACK_TAB) = ccall<void*>(F_StringTableGet, CP(0x004e4830));
}
static void fp_RaceBegin(Footprint& f) { f.replay_only = "allocates the car list, reads the folder, loads tracks.tab"; }
PORT_FN(0x00406690, "RaceBegin", RaceBegin_c, fp_RaceBegin)

static void __cdecl sort_carlist_c() {
    ccall<void>(F_qsort, UI_GP(void, S_CARLIST), UI_G32(S_CARLIST_N), 0x20, F_sort_f);
}
static void fp_sort_carlist(Footprint& f) {
    const int32_t n = UI_G32(S_CARLIST_N);
    if (UI_GP(void, S_CARLIST) && n > 0 && n <= 0x8000) f.add(UI_GP(void, S_CARLIST), (uint32_t)n * 0x20u, "the car list");
}
PORT_FN(0x00406780, "sort_carlist", sort_carlist_c, fp_sort_carlist)
static int32_t __cdecl sort_f_c(const char* a, const char* b) { return ccall<int32_t>(F_stricmp_c, a, b); }
static void fp_sort_f(Footprint&, const char*, const char*) {}
PORT_FN(0x004067a0, "sort_f", sort_f_c, fp_sort_f)

static void __cdecl RaceEnd_c() {
    ccall<void>(F_StringTableForget, UI_GP(void, S_TRACK_TAB));
    void* list = UI_GP(void, S_CARLIST);
    UI_GP(void, S_TRACK_TAB) = 0;
    ccall<void>(uit::F_Delete, list);
    UI_GP(void, S_CARLIST) = 0;
    UI_G32(S_CARLIST_N) = 0;
}
static void fp_RaceEnd(Footprint& f) { f.replay_only = "frees the car list and tracks.tab"; }
PORT_FN(0x004067c0, "RaceEnd", RaceEnd_c, fp_RaceEnd)

static int32_t __cdecl GetTrackCount_c() { return ccall<int32_t>(F_StringTableNumRows, UI_GP(void, S_TRACK_TAB)); }
PORT_FN(0x00406800, "GetTrackCount", GetTrackCount_c, fp_none)
static const char* __cdecl GetTrackName_c(int32_t t) {
    return ccall<const char*>(F_StringTableGetEntry, UI_GP(void, S_TRACK_TAB), t, 0);
}
PORT_FN(0x00406810, "GetTrackName", GetTrackName_c, fp_none_i)
// the friendly name and the text: tracks.tab's key, translated by an Xlator on the stack
static const char* __cdecl GetTrackFriendlyName_c(int32_t t) {
    volatile uint32_t xl[3];
    const char* key = ccall<const char*>(F_StringTableGetEntry, UI_GP(void, S_TRACK_TAB), t, 1);
    tcall<void*>(uit::F_Xlator_ctor, (void*)xl, key);
    if (xl[2] != UI_GU32(S_XLATOR_COOKIE)) tcall<void>(uit::F_Xlator_xlate, (void*)xl);
    return (const char*)(uintptr_t)xl[1];
}
PORT_FN(0x00406830, "GetTrackFriendlyName", GetTrackFriendlyName_c, fp_none_i)
static const char* __cdecl GetTrackText_c(int32_t t) {
    volatile uint32_t xl[3];
    const char* key = ccall<const char*>(F_StringTableGetEntry, UI_GP(void, S_TRACK_TAB), t, 2);
    tcall<void*>(uit::F_Xlator_ctor, (void*)xl, key);
    if (UI_GU32(S_XLATOR_COOKIE) != xl[2]) tcall<void>(uit::F_Xlator_xlate, (void*)xl);
    return (const char*)(uintptr_t)xl[1];
}
PORT_FN(0x00406870, "GetTrackText", GetTrackText_c, fp_none_i)

// GetTrackNumber: the first track whose name starts the given one (strnicmp over the track's length); -1 if none
static int32_t __cdecl GetTrackNumber_c(const char* name) {
    const int32_t n = ccall<int32_t>(F_GetTrackCount);
    for (int32_t i = 0; i < n; i++) {
        const char* e = ccall<const char*>(F_StringTableGetEntry, UI_GP(void, S_TRACK_TAB), i, 0);
        if (ccall<int>(F_strnicmp, e, name, crt_strlen(e)) == 0) return i;
    }
    return -1;
}
static void fp_GetTrackNumber(Footprint&, const char*) {}
PORT_FN(0x004068b0, "GetTrackNumber", GetTrackNumber_c, fp_GetTrackNumber)

static const char* __cdecl GetCarFileName_c(int32_t i) { return UI_GP(const char, S_CARLIST) + ((uint32_t)i << 5); }
PORT_FN(0x00406910, "GetCarFileName", GetCarFileName_c, fp_none_i)
static int32_t __cdecl GetMaxCarFileNames_c() { return UI_G32(S_CARLIST_N); }
PORT_FN(0x00406920, "GetMaxCarFileNames", GetMaxCarFileNames_c, fp_none)
static int32_t __cdecl GetCarFileNumber_c(const char* name) {
    int32_t i = 0;
    if (ccall<int32_t>(F_GetMaxCarFileNames) > 0) {
        do {
            if (ccall<int>(F_stricmp_c, name, ccall<const char*>(F_GetCarFileName, i)) == 0) return i;
            i++;
        } while (ccall<int32_t>(F_GetMaxCarFileNames) > i);
    }
    UI_LogPanic(CP(0x004e483c), name);
    return 0;
}
static void fp_GetCarFileNumber(Footprint&, const char*) {}
PORT_FN(0x00406930, "GetCarFileNumber", GetCarFileNumber_c, fp_GetCarFileNumber)

// =========================================================================================================================
// prerace.obj
// =========================================================================================================================
static uint8_t __cdecl garage_cb_c(int32_t) {
    return ccall<uint8_t>(F_MenuEditCar, CP(S_PR_CAR), CP(S_PR_TRACK), UI_GP(uint8_t, S_PR_ARG), CP(S_PR_FRIENDLY));
}
static void fp_garage_cb(Footprint& f, int32_t) { f.replay_only = "runs the garage (MenuEditCar)"; }
PORT_FN(0x00410340, "garage_cb", garage_cb_c, fp_garage_cb)

static void __cdecl ASSERT_MSG_c(int32_t, const char*) {}
static void fp_ASSERT_MSG(Footprint& f, int32_t, const char*) { f.pure = true; }
PORT_FN(0x00410210, "ASSERT_MSG", ASSERT_MSG_c, fp_ASSERT_MSG)

// PreRaceDo: the screen before a race; 1 to race (the dialog's -2), 2 for the garage's way out (0x29a), else 0
typedef uint8_t(__cdecl* PreRaceGarage_t)(int32_t);
static int32_t __cdecl PreRaceDo_c(uint8_t* w, uint8_t garage_first, uint8_t* car_arg, uint8_t qualify, const uint32_t* times,
                                   const int32_t* prizes) {
    alignas(8) uint8_t F[0x1808];                                   // the original's frame (sub esp, 0x17f8; 4 pushes), its offsets
    ccall<void>(F_ResourceSetMustLoad, CP(0x004e5778));
    UI_GP(uint8_t, S_PR_ARG) = car_arg;
    rb_xl_once(S_PR_GUARD0, 0x01, 0x00505e10, 0x004e5788, 0x00410330);
    rb_xl_once(S_PR_GUARD0, 0x02, 0x00505e40, 0x004e57a0, 0x00410320);
    rb_xl_once(S_PR_GUARD0, 0x04, 0x00505de0, 0x004e57b4, 0x00410310);
    rb_xl_once(S_PR_GUARD0, 0x08, 0x00505cf8, 0x004e57cc, 0x00410300);
    rb_xl_once(S_PR_GUARD0, 0x10, 0x00505c38, 0x004e57e0, 0x004102f0);
    rb_xl_once(S_PR_GUARD0, 0x20, 0x00505cc8, 0x004e57f8, 0x004102e0);
    rb_xl_once(S_PR_GUARD0, 0x40, 0x00505d80, 0x004e580c, 0x004102d0);
    rb_xl_once(S_PR_GUARD0, 0x80, 0x00505d18, 0x004e5820, 0x004102c0);
    rb_xl_once(S_PR_GUARD1, 0x01, 0x00505cb0, 0x004e5830, 0x004102b0);
    rb_xl_once(S_PR_GUARD1, 0x02, 0x00505df0, 0x004e5844, 0x004102a0);
    rb_xl_once(S_PR_GUARD1, 0x04, 0x00505e00, 0x004e5858, 0x00410290);
    rb_xl_once(S_PR_GUARD1, 0x08, 0x00505dd0, 0x004e5868, 0x00410280);
    rb_xl_once(S_PR_GUARD1, 0x10, 0x00505c90, 0x004e587c, 0x00410270);
    rb_xl_once(S_PR_GUARD1, 0x20, 0x00505ce8, 0x004e5894, 0x00410260);
    rb_xl_once(S_PR_GUARD1, 0x40, 0x00505d08, 0x004e58a8, 0x00410250);
    rb_xl_once(S_PR_GUARD1, 0x80, 0x00505cd8, 0x004e58c0, 0x00410240);
    LW(F, 0x14) = 1;                                                // the radio buttons' variable (the page shown)
    LW(F, 0x2c) = U(w + 8);
    // FIX: the track's name (the World's) and its friendly name (tracks.tab's key, translated: a mod track's or a language
    // file's) were copied into 64-byte statics unbounded, a longer friendly name running into the Xlator after it (a longer
    // track name, into another static). Each keeps its first 63 characters. (The garage and the best times board read
    // them; the World's track name holds 31, so it's only ever cut in a damaged World.) One that fits is copied as before.
    if (VP_FIX) ui_copy_bounded((char*)(uintptr_t)S_PR_TRACK, (const char*)(w + 8), 0x40);
    else crt_strcpy((char*)(uintptr_t)S_PR_TRACK, (const char*)(w + 8));
    {
        const char* fr = ccall<const char*>(F_GetTrackFriendlyName, ccall<int32_t>(F_GetTrackNumber, CP(S_PR_TRACK)));
        if (VP_FIX) ui_copy_bounded((char*)(uintptr_t)S_PR_FRIENDLY, fr, 0x40);
        else crt_strcpy((char*)(uintptr_t)S_PR_FRIENDLY, fr);
    }
    LW(F, 0x1c) = U(w + 0xcd1);
    {
        const uint32_t rev = (U(w + 0xcd1) & 0xffffff00u) | *(volatile uint8_t*)(w + 0xcd1);   // (al: the byte)
        LW(F, 0x10) = ccall<uint32_t>(F_BoardCreateControl, CP(S_PR_TRACK), *(volatile int32_t*)(w + 0xcac),
                                      *(volatile int32_t*)(w + 0xcbc), rev);
    }
    StartingGridView* grid = (StartingGridView*)ccall<void*>(uit::F_MemAlloc, 0x60);
    if (grid) {
        grid->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
        grid->widget = 0;
        grid->vtbl = (const void*)(uintptr_t)VT_StartingGridView;
        grid->world = w;
        grid->times = (const volatile uint32_t*)times;
        if (*(volatile int32_t*)(grid->world + 0xca8) > 0) {
            int32_t i = 0;
            CarPosition* volatile* out = grid->pos;
            LW(F, 0x44) = U(times);
            LW(F, 0x18) = 0;
            do {
                uint8_t* e = grid->world + LW(F, 0x18) + 0x28;
                void* cp = ccall<void*>(uit::F_MemAlloc, 0xa4);
                if (cp) {
                    if (times) LW(F, 0x518) = *(const volatile uint32_t*)(uintptr_t)LW(F, 0x44);
                    else LW(F, 0x518) = 0;
                    const int32_t drv = *(volatile int32_t*)(e + 0xc0);
                    *out = tcall<CarPosition*>(F_CarPosition_ctor, cp, (const char*)(e + 0x11), (const char*)(e + 4), drv,
                                               (uint32_t)LW(F, 0x518), i);
                } else {
                    *out = 0;
                }
                out++;
                i++;
                LW(F, 0x44) = LW(F, 0x44) + 4;
                LW(F, 0x18) = LW(F, 0x18) + 0xc8;
            } while (*(volatile int32_t*)(grid->world + 0xca8) > i);
        }
    }
    // the career's prize table: a TrackPrizeInfo in the frame (its constructor inlined: the class's vtable only)
    LW(F, 0x510) = 0;
    LW(F, 0x4fc) = VT_TrackPrizeInfo;
    LW(F, 0x514) = U(prizes);
    // the track text word-wrapped into the frame's last 4 KB (0x808 .. 0x1808: UIStyleWordWrap copies at most 4095
    // characters, its own fix), then cut to 255 characters (nothing reads it: the screen has no item for it). (Not a FIX:
    // the U3 rewrite's frame stopped at 0x910, the part it used otherwise, so a description of 264 or more characters --
    // the stock Ridge Valley's, hastings, is 276 -- ran past it; the frame is the original's size now.)
    ccall<void>(F_UIStyleWordWrap, 0xf, 0xe6, (char*)(F + 0x808),
                ccall<const char*>(F_GetTrackText, ccall<int32_t>(F_GetTrackNumber, CP(S_PR_TRACK))));
    *(volatile uint8_t*)(F + 0x907) = 0;
    rb_xl_once(S_PR_GUARD2, 0x01, 0x00505e20, 0x004e58d0, 0x00410230);
    rb_xl_once(S_PR_GUARD2, 0x02, 0x00505ca0, 0x004e58e4, 0x00410220);
    // the track's length (its centre line's), in the locale's units
    tcall<void*>(F_CenterLine_ctor, (void*)(F + 0x518), (const uint8_t*)(uintptr_t)LW(F, 0x1c));
    SPRINTF((char*)(F + 0x44), CP(0x004e58f0),
            D(*(volatile float*)(F + 0x574)) * D(*(volatile float*)(UI_GP(uint8_t, S_LOCALE) + 0x14)));
    ccall<void>(uit::F_LocaleConvertNumeric, (char*)(F + 0x44));
    {
        const char* units = *(volatile uint8_t*)(UI_GP(uint8_t, S_LOCALE) + 0x38) ? xlate(0x00505e20) : xlate(0x00505ca0);
        SPRINTF((char*)(F + 0x588), CP(0x004e58f8), (const char*)(F + 0x44), units);
    }
    tcall<void>(F_CenterLine_dtor, (void*)(F + 0x518));
    // the race's summary
    const char* dmg = *(volatile uint8_t*)(w + 0xcd0) ? xlate(0x00505cb0) : xlate(0x00505df0);
    const char* laps = *(volatile int32_t*)(w + 0xcc0) > 1 ? xlate(0x00505dd0) : xlate(0x00505e00);
    xlate(0x00505d18);
    xlate(0x00505ce8);
    xlate(0x00505c90);
    {
        const uint32_t dmg_label = UI_GU32(0x00505d1c);
        const char* realism = ccall<const char*>(F_GetRealismString, *(volatile int32_t*)(w + 0xcac));
        const uint32_t realism_label = UI_GU32(0x00505cec);
        const int32_t nlaps = *(volatile int32_t*)(w + 0xcc0);
        const uint32_t type_label = UI_GU32(0x00505c94);
        SPRINTF((char*)(F + 0x608), CP(0x004e5900), type_label, nlaps, laps, realism_label, realism, dmg_label, dmg);
    }
    // the items (0x64 .. 0x4fc): the original's, where it writes them in place and where its constructor builds them
    uint8_t* it = F + 0x64;
    item14(it + 0x000, 5, 0, 0x172, 0x14, 0, 0, LA(F, 0x608), 0, 0, 0xc);                  // the summary
    item14(it + 0x038, 1, 0, 0, 0, 0, 0, 0, 0, LA(F, 0x20), 0);                             // group 1: the grid
    item14(it + 0x070, 0xe, 0, 0x17, 0x77, 0, 0, 0x004e5910, 0, 0, 0);                      //   grid.stp
    item14(it + 0x0a8, 0x17, 0, 0x78, 0xa0, 0x1e0, 0xe6, 0x004e4db8, 0, U(grid), 0);         //   the StartingGridView
    item14(it + 0x0e0, 1, 1, 0, 0, 0, 0, 0, 0, LA(F, 0x20), 0);                             // (the group's end)
    item14(it + 0x118, 1, 0, 0, 0, 0, 0, 0, 0, LA(F, 0x24), 0);                            // group 2: the track
    rb_item_ctor(it + 0x150, 0xe, 0, 0x32, 0x8c, 0, 0, CP(0x004e591c), 0, 0, 0);            //   trackmap.stp
    item14(it + 0x188, 5, 0, 0x1d6, 0x15b, 0, 0, LA(F, 0x588), 0, 0, 0x17);                 //   its length
    rb_item_ctor(it + 0x1c0, 0x17, 0, 0x15e, 0x8c, 0x12c, 0xc8, CP(0x004e4db8), 0, F + 0x4fc, 0);   // the prizes
    item14(it + 0x1f8, 1, 1, 0, 0, 0, 0, 0, 0, LA(F, 0x24), 0);
    item14(it + 0x230, 1, 0, 0, 0, 0, 0, 0, 0, LA(F, 0x28), 0);                            // group 3: the board
    item14(it + 0x268, 0x17, 0, 0, 0, 0, 0, 0x004e4db8, 0, LW(F, 0x10), 0);
    item14(it + 0x2a0, 1, 1, 0, 0, 0, 0, 0, 0, LA(F, 0x28), 0);
    rb_item_ctor(it + 0x2d8, 0xc, 0, 0x21, 0x58, -1, 0, xlate(0x00505e10), (int32_t)LA(F, 0x20), F + 0x14, 3);
    {
        const char* t = xlate(0x00505e40);
        item14(it + 0x310, 0xc, 0, 0xad, 0x58, 0xffffffffu, 0, U(t), LA(F, 0x24), LA(F, 0x14), 3);
    }
    rb_item_ctor(it + 0x348, 0xc, 0, 0x139, 0x58, -1, 0, xlate(0x00505cc8), (int32_t)LA(F, 0x28), F + 0x14, 3);
    rb_item_ctor(it + 0x380, 2, -1, 0x13, 0x1a2, 0, 0, xlate(0x00505cd8), 0, 0, 6);        // back
    {
        const char* t = qualify ? xlate(0x00505c38) : xlate(0x00505cf8);
        item14(it + 0x3b8, 2, 0xfffffffeu, 0x1e5, 0x1a5, 0, 0, U(t), 0, 0, 2);              // race / qualify
    }
    {
        const char* t = xlate(0x00505de0);
        item14(it + 0x3f0, 2, 0x29a, 0x154, 0x1a5, 0, 0, U(t), 0, F_garage_cb, 2);          // the garage
    }
    item14(it + 0x428, 4, 0x29a, 0x67, 0, 0, 0, 0, 0, F_garage_cb, 0);                      // 'g'
    item_end(it + 0x460);
    // the player's car
    int32_t player = -1;
    UI_G8(S_PR_CAR) = 0;
    for (int32_t i = 0; *(volatile int32_t*)(w + 0xca8) > i; i++) {
        uint8_t* e = w + 0x28 + (uint32_t)i * 0xc8u;
        if (*(volatile int32_t*)e == 0) {
            crt_strcpy((char*)(uintptr_t)S_PR_CAR, (const char*)(e + 0x11));
            player = i;
        }
    }
    {
        const char* title = ccall<const char*>(F_GetTrackFriendlyName, ccall<int32_t>(F_GetTrackNumber, CP(S_PR_TRACK)));
        LW(F, 0x34) = 0x004e592c;                                   // main_t.stp
        LW(F, 0x38) = 0xfffffffeu;
        LW(F, 0x40) = 0;
        LW(F, 0x30) = U(title);
        LW(F, 0x3c) = LA(F, 0x64);
    }
    int32_t r;
    if (garage_first && ((PreRaceGarage_t)(uintptr_t)F_garage_cb)(0)) {
        r = 0x29a;
    } else {
        const int32_t hh = UI_G32(S_GX_SCREEN_H), ww = UI_G32(S_GX_SCREEN_W);
        r = ccall<int32_t>(uit::F_UIDoDialog, (const void*)(F + 0x30), ww, hh, (int32_t)-999, (int32_t)-999, (int32_t)1);
    }
    if (grid) vcall<void*>(grid, 0, (uint32_t)1);
    // FIX CANDIDATE: no car of the player's (-1) loads the setup into the 0xc8 bytes before the World's first car
    ccall<uint8_t>(F_CarFileLoadSetupData, (void*)(w + (uint32_t)player * 0xc8u + 0x5c), CP(S_PR_CAR),
                   (const char*)(uintptr_t)LW(F, 0x2c));
    ccall<void>(F_SplashLoading);
    ccall<void>(F_ResourceSetUnload, CP(0x004e5938));
    if (LW(F, 0x10)) vcall<void*>((void*)(uintptr_t)LW(F, 0x10), 0, (uint32_t)1);
    if (r == -2) return 1;
    if (r == 0x29a) return 2;
    return 0;
}
static void fp_PreRaceDo(Footprint& f, uint8_t*, uint8_t, uint8_t*, uint8_t, const uint32_t*, const int32_t*) {
    f.replay_only = "the screen before a race (its own dialog loop, the garage; loads and frees)";
}
PORT_FN(0x0040f140, "PreRaceDo", PreRaceDo_c, fp_PreRaceDo)

// CarPosition::CarPosition(car, driver, driver index, time (its bits), grid slot): the car's model with its paint job,
// turned to face the camera 4.7 in front of it; the fonts, palettes and stamps
static const double* const k_neg_half_pi = (const double*)(uintptr_t)0x004db318;
static const double* const k_zero = (const double*)(uintptr_t)0x004db320;
// FIX helper (CarPosition): the first 16 characters of s into a remap entry's 16-byte name, with no terminator -- the name
// as the original left it once the rest of its overrun was written over, and as the model reads a 16-byte name
static __forceinline void fix_name16(char* dst, const char* s) { crt_copy(dst, s, 0x10); }

static CarPosition* __fastcall CarPosition_ctor_c(CarPosition* self, Edx, const char* car, const char* driver, int32_t drv,
                                                  uint32_t time, int32_t slot) {
    char mod[VP_FIX ? 0x40 : 0x20], tex[VP_FIX ? 0x40 : 0x20];      // the frame's +0x54 and +0x34 (FIX: below)
    volatile float m[9];                                            // +0x10
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_CarPosition;
    self->car = car;
    self->index = slot;
    self->odd = (uint8_t)(slot & 1);
    self->time = time;
    self->driver = driver;
    // FIX: "<car>3.mod" and WorldGetCarTexture's "~<car>.tex" went into 0x20 bytes of the frame each, so a car name of 27
    // or more characters (a mod car's: the car list holds 31) ran over the frame's saved registers. Both have 0x40 bytes
    // now, room for any name the World's car field holds (34 characters); a longer one (a damaged World, the field
    // unterminated) is cut to 48 characters for these names. A name that fits is used as before.
    char cut[0x31];
    const char* cn = fix_cut(car, cut, 0x30);
    SPRINTF(mod, CP(0x004e598c), cn, 3);
    ccall<void>(F_WorldGetCarTexture, (char*)tex, cn, drv);
    // FIX: the remap entry's names are 16 bytes each, "<car>.tex" and the texture's name written into them unbounded: a
    // car name of 12 or more characters ran "<car>.tex" into the texture's name, and a texture name of 16 or more (a car
    // name of 11 or more: "~<car>.tex") on over the entry's value and pointer, the model and the frame. The original wrote
    // all of those again afterwards, so what the model got was each name's first 16 characters (the next field starting
    // where the name's 16 bytes end, as the model reads a 16-character name). Now a name too long for its 16 bytes is
    // written as just those 16 characters, and nothing past them is touched. A name that fits is written as before.
    if (VP_FIX && ui_strnlen(cn, 0xb) > 0xb) {
        char t[0x40];
        SPRINTF(t, CP(0x004e5984), cn);
        fix_name16((char*)self->remap_from, t);
    } else {
        SPRINTF((char*)self->remap_from, CP(0x004e5984), cn);
    }
    if (VP_FIX && ui_strnlen(tex, 0xf) > 0xf) fix_name16((char*)self->remap_to, tex);
    else crt_strcpy((char*)self->remap_to, tex);
    self->remap_20 = 0;
    self->remap_24 = 0;
    self->model = ccall<int32_t>(F_mrModelLoadRemap, (const char*)mod, (void*)self->remap_from, 1);
    volatile uint32_t* fr = (volatile uint32_t*)&self->frame;
    fr[0] = 0x3f800000; fr[1] = 0; fr[2] = 0; fr[3] = 0; fr[4] = 0x3f800000; fr[5] = 0; fr[6] = 0; fr[7] = 0;
    fr[11] = 0; fr[8] = 0x3f800000; fr[10] = 0; fr[9] = 0;
    // a yaw of -pi/2 (the frame's rotation), then a pitch of -pi/2, then a roll of 0 (the fsin / fcos results stored)
    const float c = x87_cos_f(*k_neg_half_pi), s = x87_sin_f(*k_neg_half_pi);
    self->frame.m[0] = c;
    self->frame.m[2] = -s;
    self->frame.m[6] = s;
    self->frame.m[8] = c;
    volatile uint32_t* mu = (volatile uint32_t*)m;
    mu[0] = 0x3f800000; mu[1] = 0; mu[2] = 0; mu[3] = 0; mu[6] = 0;
    m[4] = c; m[5] = s; m[7] = -s; m[8] = c;
    typedef void(__cdecl * MatConcat_t)(void*, const void*, const void*);
    ((MatConcat_t)(uintptr_t)F_MatrixConcat)((void*)&self->frame, (const void*)&self->frame, (const void*)m);
    const float c0 = x87_cos_f(*k_zero), s0 = x87_sin_f(*k_zero);
    mu[2] = 0;
    m[0] = c0; m[1] = s0; m[3] = -s0; m[4] = c0;
    mu[5] = 0; mu[6] = 0; mu[7] = 0; mu[8] = 0x3f800000;
    ((MatConcat_t)(uintptr_t)F_MatrixConcat)((void*)&self->frame, (const void*)&self->frame, (const void*)m);
    ((volatile uint32_t*)&self->frame)[9] = 0;
    ((volatile uint32_t*)&self->frame)[11] = 0x40966666;
    self->shadow = ccall<uint8_t*>(F_gxCanvasGet, CP(0x004e5978));
    self->font_small = ccall<void*>(uit::F_gxFontGet, CP(0x004e596c));
    self->font_big = ccall<void*>(uit::F_gxFontGet, CP(0x004e5960));
    void* p = ccall<void*>(uit::F_gxPaletteCreate);
    self->pal_number = p;
    ccall<void>(uit::F_gxPaletteMakeGradient, p, UI_GU32(0x00505e4c), UI_GU32(0x00505e30));
    self->pal_text = ccall<void*>(uit::F_gxPaletteCreate);
    uint32_t hi, lo;
    if (drv >= 0) {
        self->player = 0;
        lo = UI_GU32(0x00505e30);
        hi = UI_GU32(0x00505d24);
    } else {
        self->player = 1;
        lo = UI_GU32(0x00505e30);
        hi = UI_GU32(0x00505e2c);
    }
    ccall<void>(uit::F_gxPaletteMakeGradient, (void*)self->pal_text, hi, lo);
    self->numbers = ccall<void*>(uit::F_gxGetStamp, CP(0x004e5954));
    self->player_stamp = ccall<void*>(uit::F_gxGetStamp, CP(0x004e5948));
    return self;
}
static void fp_CarPosition_ctor(Footprint& f, CarPosition*, Edx, const char*, const char*, int32_t, uint32_t, int32_t) {
    f.replay_only = "loads the model, fonts, palettes, stamps";
}
PORT_FN(0x00410360, "CarPosition::CarPosition", CarPosition_ctor_c, fp_CarPosition_ctor)

// CarPosition::Draw: the player's marker, the shadow, the grid number, the driver, the time (if it has one)
static void __fastcall CarPosition_Draw_c(CarPosition* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(uit::F_gxSetCanvas, c);
    const int32_t x = self->x + self->w / 2 + 8;                    // edi
    const int32_t y = self->y + self->h / 2;                        // ebp
    const int32_t side = self->odd == 0 ? -1 : 1;                   // ebx
    if (self->player) ccall<void>(uit::F_gxDrawStamp, (void*)self->player_stamp, x - 0x30, y - 0x14, (int32_t)0, (const void*)0);
    {
        const int32_t py = y - *(volatile int32_t*)(self->shadow + 0xc) / 2 + 2;
        const int32_t px = x - *(volatile int32_t*)(self->shadow + 8) / 2 + 4;
        ccall<void>(F_gxPasteAlpha, (void*)self->shadow, px, py);
    }
    const int32_t dy = side * 35;
    {
        const int32_t n = self->index + 1;
        void* pal = self->pal_number;
        FPRINTF(self->font_big, pal, 0x12, x - 0x2b, dy + y - 2, CP(0x004e59b0), n);
    }
    {
        const bool none = !(fabs(D(bits_f(self->time))) > D(bits_f(0x34000000)));
        const char* drv = self->driver;
        const int32_t off = none ? 0 : 5;
        void* pal = self->pal_text;
        FPRINTF(self->font_small, pal, 0x11, x - 0x23, y - off + dy, CP(0x004e59ac), drv);
    }
    {
        const char* t = CP(0x004e4db8);
        if (fabs(D(bits_f(self->time))) > D(bits_f(0x34000000)))
            t = ccall<const char*>(F_PhysicsTimeString, (uint32_t)self->time, (uint32_t)0);
        void* pal = self->pal_text;
        FPRINTF(self->font_small, pal, 0x11, x - 0x20, dy + y + 5, CP(0x004e59ac), t);
    }
    const int32_t idx = self->index;
    ((Assert_t)(uintptr_t)F_ASSERT_MSG)((idx >= 0 && idx < 8) ? 1 : 0, CP(0x004e5998), idx);
}
static void fp_CarPosition_Draw(Footprint& f, CarPosition*, Edx, gxCanvas* c) { fp_draw_canvas(f, c); }
PORT_FN(0x004105f0, "CarPosition::Draw", CarPosition_Draw_c, fp_CarPosition_Draw)

// CarPosition::Draw3D: the model, in its item's view, from a camera at the origin
static void __fastcall CarPosition_Draw3D_c(CarPosition* self, Edx) {
    volatile uint32_t cam[12];
    cam[0] = 0x3f800000; cam[4] = 0x3f800000; cam[1] = 0; cam[2] = 0; cam[3] = 0; cam[5] = 0; cam[6] = 0; cam[7] = 0;
    cam[8] = 0x3f800000;
    const int32_t h = self->h, w = self->w;
    cam[9] = 0;
    const int32_t x = self->x;
    cam[10] = 0; cam[11] = 0;
    const int32_t y = self->y;
    ccall<void>(F_mrSetView_c, x, y, w, h, (uint32_t)0);
    ccall<void>(F_mrSetProjection_c, (uint32_t)0x3e4ccccd, (uint32_t)0x447a0000, (uint32_t)0x3f9c61aa);
    ccall<void>(F_mrSetCamera_c, (void*)cam);
    ccall<void>(F_mrModelDraw_c, (int32_t)self->model, (const void*)&self->frame);
}
template <typename T> static void fp_draw3d0(Footprint& f, T*, Edx) { f.replay_only = "draws a model through the 3D renderer (its state, DirectX)"; }
PORT_FN(0x00410770, "CarPosition::Draw3D", CarPosition_Draw3D_c, fp_draw3d0<CarPosition>)

static CarPosition* __fastcall CarPosition_sdd_c(CarPosition* self, Edx, uint32_t flags) {
    const int32_t model = self->model;
    self->vtbl = (const void*)(uintptr_t)VT_CarPosition;
    ccall<void>(F_mrModelUnload, model);
    ccall<void>(F_gxCanvasForget, (void*)self->shadow);
    ccall<void>(uit::F_gxFontForget, (void*)self->font_small);
    ccall<void>(uit::F_gxFontForget, (void*)self->font_big);
    ccall<void>(uit::F_gxPaletteDestroy, (void*)self->pal_text);
    ccall<void>(uit::F_gxPaletteDestroy, (void*)self->pal_number);
    ccall<void>(uit::F_gxForgetStamp, (void*)self->numbers);
    ccall<void>(uit::F_gxForgetStamp, (void*)self->player_stamp);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(uit::F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00410800, "CarPosition::scalar deleting destructor", CarPosition_sdd_c, fp_dtor_flags<CarPosition>)

// StartingGridView::Added: a CustomWidget per car (8 at most), two columns, staggered
static void __fastcall StartingGridView_Added_c(StartingGridView* self, Edx) {
    alignas(8) uint8_t F[0x84];
    int32_t n = *(volatile int32_t*)(self->world + 0xca8);
    if (n >= 8) n = 8;
    LW(F, 0x10) = (uint32_t)n;
    for (int32_t i = 0; i < n; i++) {
        const bool odd = (i & 1) != 0;
        LW(F, 0x14) = 0x17;
        LW(F, 0x18) = 0;
        LW(F, 0x24) = 0x69;
        LW(F, 0x28) = 0x69;
        LW(F, 0x2c) = 0x004e4db8;
        LW(F, 0x30) = 0;
        LW(F, 0x38) = 0; LW(F, 0x3c) = 0; LW(F, 0x40) = 0; LW(F, 0x44) = 0; LW(F, 0x48) = 0;
        LW(F, 0x1c) = (uint32_t)((odd ? 0x1e : 0) + (i / 2) * 0x70 + self->x);
        const uint32_t cp = U(self->pos[i]);
        LW(F, 0x20) = (uint32_t)((odd ? 0x5a : 0) + self->y);
        LW(F, 0x34) = cp;
        item_end(F + 0x4c);
        tcall<void>(F_UICC_AddItems_c, self, (void*)(F + 0x14));
    }
}
PORT_FN(0x00410890, "StartingGridView::Added", StartingGridView_Added_c, fp_added0<StartingGridView>)

static void __fastcall StartingGridView_Draw_c(StartingGridView*, Edx, gxCanvas* c) { ccall<gxCanvas*>(uit::F_gxSetCanvas, c); }
static void fp_StartingGridView_Draw(Footprint& f, StartingGridView*, Edx, gxCanvas*) { UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas"); }
PORT_FN(0x00410990, "StartingGridView::Draw", StartingGridView_Draw_c, fp_StartingGridView_Draw)

static StartingGridView* __fastcall StartingGridView_sdd_c(StartingGridView* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_StartingGridView;
    for (int32_t i = 0; *(volatile int32_t*)(self->world + 0xca8) > i; i++)
        if (CarPosition* p = self->pos[i]) vcall<void*>(p, 0, (uint32_t)1);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(uit::F_Delete, (void*)self);
    return self;
}
PORT_FN(0x004109a0, "StartingGridView::scalar deleting destructor", StartingGridView_sdd_c, fp_dtor_flags<StartingGridView>)

// TrackPrizeInfo::Draw: the career's prizes -- place, points, purse -- until an empty row (8 at most)
static void __fastcall TrackPrizeInfo_Draw_c(TrackPrizeInfo* self, Edx, gxCanvas* c) {
    char buf[0x40];
    rb_xl_once(S_TPI_GUARD, 1, 0x005d5860, 0x004e59dc, 0x00410c60);
    rb_xl_once(S_TPI_GUARD, 2, 0x005d5870, 0x004e59c8, 0x00410c50);
    rb_xl_once(S_TPI_GUARD, 4, 0x005d5880, 0x004e59b4, 0x00410c40);
    ccall<gxCanvas*>(uit::F_gxSetCanvas, c);
    if (!self->prizes) return;
    const int32_t x = self->x + 5;
    int32_t y = self->y + 10;
    ccall<void>(uit::F_UIStyleDraw, 0xc, x + 5, y, xlate(0x005d5860), (uint32_t)0);
    ccall<void>(uit::F_UIStyleDraw, 0xd, x + 0x96, y, xlate(0x005d5870), (uint32_t)0);
    ccall<void>(uit::F_UIStyleDraw, 0xd, x + 0xf0, y, xlate(0x005d5880), (uint32_t)0);
    ccall<void>(uit::F_gxLine, x + 5, y + 0xf, x + 0xf5, y + 0xf, UI_GU32(0x00505d24));
    y += 0x14;
    int32_t place = 0;
    for (uint32_t o = 0; o < 0x40;) {
        const volatile int32_t* p = (const volatile int32_t*)((const volatile uint8_t*)self->prizes + o);
        if (p[0] == 0 || p[1] == 0) return;
        place++;
        o += 8;
        SPRINTF(buf, CP(0x004e59b0), place);
        ccall<void>(uit::F_UIStyleDraw, 0xc, x + 5, y, (const char*)buf, (uint32_t)0);
        ccall<void>(F_LocaleMoney, (char*)buf, *(const volatile int32_t*)((const volatile uint8_t*)self->prizes + o - 4), (uint32_t)0);
        ccall<void>(uit::F_UIStyleDraw, 0xd, x + 0xf0, y, (const char*)buf, (uint32_t)0);
        SPRINTF(buf, CP(0x004e59b0), *(const volatile int32_t*)((const volatile uint8_t*)self->prizes + o - 8));
        ccall<void>(uit::F_UIStyleDraw, 0xd, x + 0x96, y, (const char*)buf, (uint32_t)0);
        y += 0x14;
    }
}
static void fp_TrackPrizeInfo_Draw(Footprint& f, TrackPrizeInfo*, Edx, gxCanvas* c) {
    if (!xl_built(S_TPI_GUARD, 7)) { f.replay_only = "the first call builds its Xlators (atexit)"; return; }
    fp_draw_canvas(f, c);
    FP_XL(f, 0x005d5860); FP_XL(f, 0x005d5870); FP_XL(f, 0x005d5880);
}
PORT_FN(0x00410a00, "TrackPrizeInfo::Draw", TrackPrizeInfo_Draw_c, fp_TrackPrizeInfo_Draw)

// the function-local Xlators' destructor helpers (they do nothing)
static void __cdecl xlh_prize_purse() {}
PORT_FN(0x00410c40, "TrackPrizeInfo::Draw::XL_Purse (destructor helper)", xlh_prize_purse, fp_pure0)
static void __cdecl xlh_prize_points() {}
PORT_FN(0x00410c50, "TrackPrizeInfo::Draw::XL_Points (destructor helper)", xlh_prize_points, fp_pure0)
static void __cdecl xlh_prize_place() {}
PORT_FN(0x00410c60, "TrackPrizeInfo::Draw::XL_Place (destructor helper)", xlh_prize_place, fp_pure0)

static TrackPrizeInfo* __fastcall TrackPrizeInfo_vdd_c(TrackPrizeInfo* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(uit::F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00410c70, "TrackPrizeInfo::vector deleting destructor", TrackPrizeInfo_vdd_c, fp_dtor_flags<TrackPrizeInfo>)

// =========================================================================================================================
// postrace.obj
// =========================================================================================================================
// LapsTable's steps: the car (then car_changed), the page
static void __fastcall LT_next_car_c(LapsTable* self, Edx) {
    const int32_t v = self->car + 1;
    self->car = v;
    // FIX CANDIDATE: a car count of 0 divides by zero
    self->car = v % self->cars;
    tcall<void>(F_LT_car_changed, self);
}
static void fp_lt_group(Footprint& f, LapsTable* self) {
    f.add(self, sizeof(LapsTable), "the laps table");
    UI_FP_WIDGET(f, (const Widget*)self->widget, "its widget");
    ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));
}
static void fp_lt_step(Footprint& f, LapsTable* self, Edx) { fp_lt_group(f, self); }
PORT_FN(0x0040aee0, "LapsTable::next_car", LT_next_car_c, fp_lt_step)
static void __fastcall LT_prev_car_c(LapsTable* self, Edx) {
    const int32_t v = (int32_t)((uint32_t)self->car - 1u);
    self->car = v;
    if (v < 0) self->car = (int32_t)((uint32_t)self->cars + (uint32_t)v);
    tcall<void>(F_LT_car_changed, self);
}
PORT_FN(0x0040af00, "LapsTable::prev_car", LT_prev_car_c, fp_lt_step)
static void __fastcall LT_next_page_c(LapsTable* self, Edx) {
    const int32_t v = (int32_t)((uint32_t)self->page + 1u);
    self->page = v;
    if (!(self->pages > v)) self->page = 0;
}
static void fp_lt_page(Footprint& f, LapsTable* self, Edx) { f.add(self, sizeof(LapsTable), "the laps table"); }
PORT_FN(0x0040af20, "LapsTable::next_page", LT_next_page_c, fp_lt_page)
static void __fastcall LT_prev_page_c(LapsTable* self, Edx) {
    const int32_t v = (int32_t)((uint32_t)self->page - 1u);
    self->page = v;
    if (v < 0) self->page = (int32_t)((uint32_t)self->pages - 1u);
}
PORT_FN(0x0040af40, "LapsTable::prev_page", LT_prev_page_c, fp_lt_page)

// car_changed: the car's laps counted; the page buttons shown when there's more than one page
static void __fastcall LT_car_changed_c(LapsTable* self, Edx) {
    tcall<void>(F_UICC_Dirty_c, self);
    self->laps = 0;
    if (ccall<const void*>(F_RecordGetLapResult, (int32_t)self->car, (int32_t)1)) {
        do {
            const int32_t l = self->laps + 1;
            self->laps = l;
            if (!ccall<const void*>(F_RecordGetLapResult, (int32_t)self->car, l + 1)) break;
        } while (true);
    }
    self->page = 0;
    self->pages = 1;
    const int32_t laps = self->laps;
    const uint32_t g = self->group;
    if (!(laps > 10)) {
        ccall<void>(F_UIHideGroup_c, g);
        return;
    }
    ccall<void>(F_UIShowGroup_c, g);
    const int32_t p = (self->laps + 9) / 10;
    self->page = 0;
    self->pages = p;
}
PORT_FN(0x0040af60, "LapsTable::car_changed", LT_car_changed_c, fp_lt_step)

// LapsTable::LapsTable(cars): the player's car (0 if none), the fonts, three palettes
static LapsTable* __fastcall LapsTable_ctor_c(LapsTable* self, Edx, int32_t cars) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->cars = cars;
    self->vtbl = (const void*)(uintptr_t)VT_LapsTable;
    self->widget = 0;
    UI_GP(LapsTable, S_LAPS_GLOBAL) = self;
    const int32_t car = ccall<int32_t>(F_WorldGetPlayerCar);
    self->car = car;
    if (car < 0) self->car = 0;
    self->laps = 0x12d687;
    self->font_small = ccall<void*>(uit::F_gxFontGet, CP(0x004e4ff8));
    self->font_big = ccall<void*>(uit::F_gxFontGet, CP(0x004e5004));
    self->pal = ccall<void*>(uit::F_gxPaletteCreate);
    {
        const uint32_t lo = UI_GU32(0x005058e0), hi = UI_GU32(0x005058b8);
        ccall<void>(uit::F_gxPaletteMakeGradient, (void*)self->pal, hi, lo);
    }
    void* p = ccall<void*>(uit::F_gxPaletteCreate);
    self->pal_best = p;
    {
        const uint32_t lo = UI_GU32(0x005058e0), hi = UI_GU32(0x005058cc);
        ccall<void>(uit::F_gxPaletteMakeGradient, p, hi, lo);
    }
    p = ccall<void*>(uit::F_gxPaletteCreate);
    self->pal_hi = p;
    {
        const uint32_t lo = UI_GU32(0x005058e0), hi = UI_GU32(0x005058bc);
        ccall<void>(uit::F_gxPaletteMakeGradient, p, hi, lo);
    }
    return self;
}
static void fp_LapsTable_ctor(Footprint& f, LapsTable*, Edx, int32_t) { f.replay_only = "loads fonts, makes palettes"; }
PORT_FN(0x0040afe0, "LapsTable::LapsTable", LapsTable_ctor_c, fp_LapsTable_ctor)

static void __fastcall LapsTable_dtor_c(LapsTable* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_LapsTable;
    UI_GP(LapsTable, S_LAPS_GLOBAL) = 0;
    ccall<void>(uit::F_gxFontForget, (void*)self->font_small);
    ccall<void>(uit::F_gxFontForget, (void*)self->font_big);
    ccall<void>(uit::F_gxPaletteDestroy, (void*)self->pal);
    ccall<void>(uit::F_gxPaletteDestroy, (void*)self->pal_best);
    ccall<void>(uit::F_gxPaletteDestroy, (void*)self->pal_hi);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
PORT_FN(0x0040b0b0, "LapsTable::~LapsTable", LapsTable_dtor_c, fp_loads0<LapsTable>)

// LapsTable::Added: next / prev car (the top corners), and in a group, prev / next page (the bottom corners)
static void __fastcall LapsTable_Added_c(LapsTable* self, Edx) {
    alignas(8) uint8_t F[0x198];
    const int32_t y = self->y;
    const int32_t x = self->x + 10;
    rb_item_ctor(F + 0x10, 3, 0, x + 0x19, y, 0, 0, CP(0x004e5010), 0, (const void*)(uintptr_t)F_LT_NextCar, 0);
    item14(F + 0x48, 3, 0, (uint32_t)x, (uint32_t)y, 0, 0, 0x004e501c, 0, F_LT_PrevCar, 0);
    item14(F + 0x80, 1, 0, 0, 0, 0, 0, 0, 0, U(&self->group), 0);
    {
        const int32_t by = self->y + self->h - 0x14;
        rb_item_ctor(F + 0xb8, 3, 0, x, by, 0, 0, CP(0x004e5028), 0, (const void*)(uintptr_t)F_LT_PrevPage, 0);
    }
    {
        const int32_t px = self->x + 0x96;
        const int32_t by = self->y + self->h - 0x14;
        item14(F + 0xf0, 3, 0, (uint32_t)px, (uint32_t)by, 0, 0, 0x004e5034, 0, F_LT_NextPage, 0);
    }
    item14(F + 0x128, 1, 1, 0, 0, 0, 0, 0, 0, U(&self->group), 0);
    item_end(F + 0x160);
    tcall<void>(F_UICC_AddItems_c, self, (void*)(F + 0x10));
}
PORT_FN(0x0040b110, "LapsTable::Added", LapsTable_Added_c, fp_added0<LapsTable>)

// LapsTable::Draw: the car's name, the headings, then a page of laps -- the time (best: highlit) and the top speed
static void __fastcall LapsTable_Draw_c(LapsTable* self, Edx, gxCanvas* c) {
    vcall<int32_t>(UI_GP(void, S_DEITY), 0x20);                     // (GetNumLaps: unused)
    ccall<gxCanvas*>(uit::F_gxSetCanvas, c);
    const int32_t x = self->x;                                      // [esp+0x18]
    int32_t y = self->y + 0x15;                                     // [esp+0x14]
    rb_xl_once(S_LT_GUARD, 0x01, 0x00505958, 0x004e5040, 0x0040b650);
    {
        const char* name = ccall<const char*>(F_CarMgrGetInfo, (int32_t)self->car) + 5;
        void* pal = self->pal;
        void* font = self->font_small;
        FPRINTF(font, pal, 9, x + 0x41, y - 0x1c, CP(0x004e5058), name);
    }
    rb_xl_once(S_LT_GUARD, 0x02, 0x00505998, 0x004e505c, 0x0040b640);
    rb_xl_once(S_LT_GUARD, 0x04, 0x00505920, 0x004e5070, 0x0040b630);
    rb_xl_once(S_LT_GUARD, 0x08, 0x005058c0, 0x004e5080, 0x0040b620);
    rb_xl_once(S_LT_GUARD, 0x10, 0x00505938, 0x004e5094, 0x0040b610);
    {
        const char* t = xlate(0x00505998);
        void* pal = self->pal_hi;
        void* font = self->font_big;
        FPRINTF(font, pal, 9, x + 0x1e, y, CP(0x004e50a4), t);
    }
    {
        void* pal = self->pal_hi;
        void* font = self->font_big;
        FPRINTF(font, pal, 0xc, x + 0xa5, y, CP(0x004e50b0), CP(0x004e50a8));
    }
    const int32_t page = self->page;
    int32_t n = self->laps;
    y += 10;
    int32_t lap = page * 10;
    if (n >= 10) n = 10;
    bool done = false;
    const int32_t last = n + lap;
    lap++;
    do {
        if (!(last + 1 > lap)) return;
        const RbRaceRecord* r = ccall<const RbRaceRecord*>(F_RecordGetLapResult, (int32_t)self->car, lap);
        {
            void* pal = self->pal;
            void* font = self->font_small;
            FPRINTF(font, pal, 0xa, x + 0x17, y, CP(0x004e50b4), lap);
        }
        if (r) {
            void* pal = (r->flags & 2) ? self->pal_best : self->pal;
            const char* s = ccall<const char*>(F_PhysicsTimeString, (uint32_t)r->lap_time, (uint32_t)1);
            FPRINTF(self->font_small, pal, 9, x + 0x28, y, CP(0x004e50b8), s);
        } else {
            done = true;
        }
        if (r && (int32_t)r->top_speed > 0) {
            void* pal = (r->flags & 4) ? self->pal_best : self->pal;
            const int32_t v = x87_ftol(D(*(volatile float*)(UI_GP(uint8_t, S_LOCALE) + 0x1c)) * D(bits_f(r->top_speed)));
            FPRINTF(self->font_small, pal, 9, x + 0x96, y, CP(0x004e50bc), v);
        }
        y += 0x14;
        lap++;
    } while (!done);
}
static void fp_LapsTable_Draw(Footprint& f, LapsTable*, Edx, gxCanvas* c) {
    if (!xl_built(S_LT_GUARD, 0x1f)) { f.replay_only = "the first call builds its Xlators (atexit)"; return; }
    fp_draw_canvas(f, c);
    FP_XL(f, 0x00505958); FP_XL(f, 0x00505998); FP_XL(f, 0x00505920); FP_XL(f, 0x005058c0); FP_XL(f, 0x00505938);
}
PORT_FN(0x0040b340, "LapsTable::Draw", LapsTable_Draw_c, fp_LapsTable_Draw)

// PostRaceDo: the screen after a race -- the standings, the laps, the replay -- until it's left
static void __cdecl PostRaceDo_c(uint8_t* w) {
    alignas(8) uint8_t F[0x2a8];
    ccall<void>(F_ResourceSetMustLoad, CP(0x004e50c0));
    {
        const int32_t pc = ccall<int32_t>(F_WorldGetPlayerCar);
        // FIX CANDIDATE: the player's race record is read without checking there is one
        if (pc != -1 && *(const volatile int32_t*)(ccall<const uint8_t*>(F_RecordGetRaceResult, pc) + 0xc) > 0) {
            if (ccall<int32_t>(F_CarMgrCount) > 1) ccall<const void*>(F_CarMgrGetInfo, 1);
            ccall<const void*>(F_CarMgrGetInfo, pc);
        }
    }
    int32_t cars = *(volatile int32_t*)(w + 0xca8);
    if (*(volatile int32_t*)(w + (uint32_t)cars * 0xc8u - 0xa0) == 3) cars--;     // a ghost last isn't in the standings
    // the PostRaceTable at +0x8c (its constructor inlined) and its two styles
    LW(F, 0xa0) = 0;
    LW(F, 0xac) = (uint32_t)cars;
    {
        const uint32_t a = UI_GU32(0x005058bc), b = UI_GU32(0x005058cc);
        LW(F, 0x24) = 0x004e518c;
        LW(F, 0x2c) = a;
        const uint32_t d = UI_GU32(0x005058dc);
        LW(F, 0x30) = b; LW(F, 0x34) = d; LW(F, 0x38) = 0; LW(F, 0x3c) = 0; LW(F, 0x40) = b; LW(F, 0x44) = a;
        LW(F, 0x48) = d; LW(F, 0x4c) = 0; LW(F, 0x50) = 0; LW(F, 0x54) = 0;
        LW(F, 0x8c) = VT_PostRaceTable;
        LW(F, 0x28) = 9;
        LW(F, 0xa4) = ccall<uint32_t>(F_UIAddDynamicStyle, (const void*)(F + 0x24));
    }
    {
        LW(F, 0x58) = 0x004e518c;
        const uint32_t a = UI_GU32(0x005058bc), b = UI_GU32(0x005058cc), d = UI_GU32(0x005058dc);
        LW(F, 0x60) = a; LW(F, 0x64) = b; LW(F, 0x68) = d; LW(F, 0x6c) = 0; LW(F, 0x70) = 0; LW(F, 0x74) = b;
        LW(F, 0x78) = a; LW(F, 0x7c) = d; LW(F, 0x80) = 0; LW(F, 0x84) = 0;
        LW(F, 0x5c) = 0xc;
        LW(F, 0x88) = 0;
        LW(F, 0xa8) = ccall<uint32_t>(F_UIAddDynamicStyle, (const void*)(F + 0x58));
    }
    tcall<void*>(F_LapsTable_ctor, (void*)(F + 0x224), cars);
    const char* friendly = ccall<const char*>(F_GetTrackFriendlyName, ccall<int32_t>(F_GetTrackNumber, (const char*)(w + 8)));
    {
        const char* realism = ccall<const char*>(F_GetRealismString, *(volatile int32_t*)(w + 0xcac));
        const char* type = ccall<const char*>(F_GetRaceTypeString, *(volatile int32_t*)(w + 0xcbc));
        SPRINTF((char*)(F + 0x268), CP(0x004e50d0), type, friendly, realism);
    }
    tcall<void*>(F_BoardCustomText_ctor, (void*)(F + 0x200), (const char*)(F + 0x268));
    rb_xl_once(S_PRD_GUARD, 0x01, 0x00505948, 0x004e50dc, 0x0040bd60);
    rb_xl_once(S_PRD_GUARD, 0x02, 0x00505900, 0x004e50f0, 0x0040bd50);
    rb_xl_once(S_PRD_GUARD, 0x04, 0x00505988, 0x004e50f8, 0x0040bd40);
    rb_xl_once(S_PRD_GUARD, 0x08, 0x005059a8, 0x004e5110, 0x0040bd30);
    rb_xl_once(S_PRD_GUARD, 0x10, 0x005058f0, 0x004e5128, 0x0040bd20);
    rb_xl_once(S_PRD_GUARD, 0x20, 0x00505910, 0x004e5144, 0x0040bd10);
    rb_xl_once(S_PRD_GUARD, 0x40, 0x00505970, 0x004e5158, 0x0040bd00);
    item14(F + 0xb0, 5, 0, 0x190, 0x16, 0, 0, U(friendly), 0, 0, 0x10);                    // the track
    item14(F + 0xe8, 0x17, 0, 0x13, 0x54, 0x142, 0x136, 0x004e4db8, 0, LA(F, 0x8c), 0);      // the standings
    item14(F + 0x120, 0x17, 0, 0x186, 0x7e, 0xcc, 0x108, 0x004e4db8, 0, LA(F, 0x224), 0);    // the laps
    item14(F + 0x158, 2, 0xffffffffu, 0x13, 0x1a2, 0, 0, U(xlate(0x00505900)), 0, 0, 6);     // back
    item14(F + 0x190, 2, 0, 0x1e5, 0x1a5, 0, 0, U(xlate(0x00505988)), 0, 0, 2);              // replay
    item_end(F + 0x1c8);
    LW(F, 0x10) = U(xlate(0x00505948));
    LW(F, 0x1c) = LA(F, 0xb0);
    LW(F, 0x20) = 0;
    LW(F, 0x14) = 0x004e516c;
    LW(F, 0x18) = 0xffffffffu;
    for (;;) {
        const int32_t hh = UI_G32(S_GX_SCREEN_H), ww = UI_G32(S_GX_SCREEN_W);
        const int32_t r = ccall<int32_t>(uit::F_UIDoDialog, (const void*)(F + 0x10), ww, hh, (int32_t)-999, (int32_t)-999, (int32_t)1);
        if (r == 0) ccall<void>(F_ReplayDo, (void*)w);
        else if (r == 1)
            ccall<void>(F_BoardDo, (const char*)(w + 8), *(volatile int32_t*)(w + 0xcac), *(volatile int32_t*)(w + 0xcbc),
                        (uint32_t) * (volatile uint8_t*)(w + 0xcd1));
        else break;
    }
    ccall<void>(F_SplashLoading);
    ccall<void>(F_ResourceSetUnload, CP(0x004e517c));
    tcall<void>(F_BoardCustomText_dtor, (void*)(F + 0x200));
    tcall<void>(F_LapsTable_dtor, (void*)(F + 0x224));
    LW(F, 0x8c) = VT_PostRaceTable;
    ccall<void>(F_UIRemoveStyle_c, (int32_t)LW(F, 0xa4));
    ccall<void>(F_UIRemoveStyle_c, (int32_t)LW(F, 0xa8));
}
static void fp_PostRaceDo(Footprint& f, uint8_t*) { f.replay_only = "the screen after a race (its own dialog loop, the replay, the board)"; }
PORT_FN(0x0040b660, "PostRaceDo", PostRaceDo_c, fp_PostRaceDo)

static void __fastcall LapsTable_Create_c(LapsTable* self, Edx) {
    tcall<void>(F_UICC_AddNotification_c, self, (int32_t)0, (const void*)&self->page, (uint32_t)4);
    tcall<void>(F_LT_car_changed, self);
}
PORT_FN(0x0040bd70, "LapsTable::Create", LapsTable_Create_c, fp_notes0<LapsTable>)
static void __fastcall LapsTable_Destroy_c(LapsTable* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification_c, self, (const void*)&self->page);
}
PORT_FN(0x0040bd90, "LapsTable::Destroy", LapsTable_Destroy_c, fp_notes0<LapsTable>)
static void __fastcall LapsTable_Callback_c(LapsTable* self, Edx, int32_t, const void*) { tcall<void>(F_UICC_Dirty_c, self); }
static void fp_LapsTable_Callback(Footprint& f, LapsTable* self, Edx, int32_t, const void*) {
    UI_FP_WIDGET(f, (const Widget*)self->widget, "its widget");
}
PORT_FN(0x0040bda0, "LapsTable::Callback", LapsTable_Callback_c, fp_LapsTable_Callback)
static LapsTable* __fastcall LapsTable_vdd_c(LapsTable* self, Edx, uint32_t flags) {
    tcall<void>(F_LapsTable_dtor, self);
    if (flags & 1) ccall<void>(uit::F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0040bdb0, "LapsTable::vector deleting destructor", LapsTable_vdd_c, fp_dtor_flags<LapsTable>)

// the buttons' callbacks: a step of LapsTable::global
static uint8_t __cdecl LT_NextCar_c(int32_t) { tcall<void>(F_LT_next_car, UI_GP(LapsTable, S_LAPS_GLOBAL)); return 0; }
static uint8_t __cdecl LT_PrevCar_c(int32_t) { tcall<void>(F_LT_prev_car, UI_GP(LapsTable, S_LAPS_GLOBAL)); return 0; }
static uint8_t __cdecl LT_NextPage_c(int32_t) { tcall<void>(F_LT_next_page, UI_GP(LapsTable, S_LAPS_GLOBAL)); return 0; }
static uint8_t __cdecl LT_PrevPage_c(int32_t) { tcall<void>(F_LT_prev_page, UI_GP(LapsTable, S_LAPS_GLOBAL)); return 0; }
static void fp_lt_cb_car(Footprint& f, int32_t) {
    LapsTable* t = UI_GP(LapsTable, S_LAPS_GLOBAL);
    if (!t) { f.replay_only = "no table (a fault)"; return; }
    fp_lt_group(f, t);
}
static void fp_lt_cb_page(Footprint& f, int32_t) {
    LapsTable* t = UI_GP(LapsTable, S_LAPS_GLOBAL);
    if (!t) { f.replay_only = "no table (a fault)"; return; }
    f.add(t, sizeof(LapsTable), "the laps table");
}
PORT_FN(0x0040bdd0, "LapsTable::NextCar", LT_NextCar_c, fp_lt_cb_car)
PORT_FN(0x0040bde0, "LapsTable::PrevCar", LT_PrevCar_c, fp_lt_cb_car)
PORT_FN(0x0040bdf0, "LapsTable::NextPage", LT_NextPage_c, fp_lt_cb_page)
PORT_FN(0x0040be00, "LapsTable::PrevPage", LT_PrevPage_c, fp_lt_cb_page)

// PostRaceTable::Draw: the title, the date, the track, the headings; then each car by place -- its name (the player's
// highlit), the race time or DNF, the best lap and top speed (highlit when they're the race's best)
static void __fastcall PostRaceTable_Draw_c(PostRaceTable* self, Edx, gxCanvas* c) {
    char buf[0x100];
    rb_xl_once(S_PRT_GUARD, 0x01, 0x005d58f0, 0x004e5220, 0x0040c2b0);
    rb_xl_once(S_PRT_GUARD, 0x02, 0x005d58c0, 0x004e5208, 0x0040c2a0);
    rb_xl_once(S_PRT_GUARD, 0x04, 0x005d58b0, 0x004e51f4, 0x0040c290);
    rb_xl_once(S_PRT_GUARD, 0x08, 0x005d5890, 0x004e51e4, 0x0040c280);
    rb_xl_once(S_PRT_GUARD, 0x10, 0x005d58d0, 0x004e51d0, 0x0040c270);
    rb_xl_once(S_PRT_GUARD, 0x20, 0x005d58e0, 0x004e51b8, 0x0040c260);
    rb_xl_once(S_PRT_GUARD, 0x40, 0x005d58a0, 0x004e51a0, 0x0040c250);
    ccall<gxCanvas*>(uit::F_gxSetCanvas, c);
    const int32_t x = self->x;                                      // ebp
    int32_t y = self->y;
    const int32_t cx = x + self->w / 2;                             // edi
    y += 0x14;
    // FIX CANDIDATE: the first race result isn't checked (none: a read of address 0 and on)
    const RbRaceRecord* r0 = ccall<const RbRaceRecord*>(F_RecordGetRaceResult, (int32_t)0);
    {
        const char* t = xlate(0x005d58a0);
        ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style_title, cx, y, t, (uint32_t)0);
    }
    y += 0x10;
    ccall<void>(F_LocaleFormatShortDate, (char*)buf, 0x100, (int32_t)r0->day, (int32_t)r0->month, (int32_t)r0->year);
    ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style_title, cx, y, (const char*)buf, (uint32_t)0);
    y += 0x18;
    {
        const char* t = ccall<const char*>(F_GetTrackFriendlyName, ccall<int32_t>(F_WorldGetTrackNumber));
        ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style_title, cx, y, t, (uint32_t)0);
    }
    y = self->y + 0x78;
    { const char* t = xlate(0x005d58b0); ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0xb, y, t, (uint32_t)0); }
    { const char* t = xlate(0x005d58c0); ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0x60, y, t, (uint32_t)0); }
    { const char* t = xlate(0x005d58d0); ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0xac, y, t, (uint32_t)0); }
    { const char* t = xlate(0x005d58e0); ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0xf8, y, t, (uint32_t)0); }
    y += 0x1e;
    for (int32_t i = 0; self->count > i; i++) {
        const int32_t car = vcall<int32_t>(UI_GP(void, S_DEITY), 0x44, i);
        const RbRaceRecord* r = ccall<const RbRaceRecord*>(F_RecordGetRaceResult, car);
        if (!r) continue;
        {
            const uint32_t hi = (ccall<int32_t>(F_WorldGetPlayerCar) - car) == 0 ? 1u : 0u;
            ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0xb, y, (const char*)r->car_name, hi);
        }
        if ((int32_t)r->race_time > 0) {
            const char* s = ccall<const char*>(F_PhysicsTimeString, (uint32_t)r->race_time, (uint32_t)1);
            ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0x60, y, s, (uint32_t)0);
        } else {
            const char* t = xlate(0x005d58f0);
            ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0x60, y, t, (uint32_t)0);
        }
        if ((int32_t)r->lap_time > 0) {
            const uint32_t hi = (r->flags & 2) ? 1u : 0u;
            const char* s = ccall<const char*>(F_PhysicsTimeString, (uint32_t)r->lap_time, (uint32_t)1);
            ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0xac, y, s, hi);
        }
        if ((int32_t)r->top_speed > 0) {
            SPRINTF(buf, CP(0x004e5198), D(*(volatile float*)(UI_GP(uint8_t, S_LOCALE) + 0x1c)) * D(bits_f(r->top_speed)));
            ccall<void>(uit::F_LocaleConvertNumeric, (char*)buf);
            const uint32_t hi = (r->flags & 4) ? 1u : 0u;
            ccall<void>(uit::F_UIStyleDraw, (int32_t)self->style, x + 0xf8, y, (const char*)buf, hi);
        }
        y += 0x10;
    }
}
static void fp_PostRaceTable_Draw(Footprint& f, PostRaceTable*, Edx, gxCanvas* c) {
    if (!xl_built(S_PRT_GUARD, 0x7f)) { f.replay_only = "the first call builds its Xlators (atexit)"; return; }
    fp_draw_canvas(f, c);
    static const uint32_t xl[] = {0x005d58f0, 0x005d58c0, 0x005d58b0, 0x005d5890, 0x005d58d0, 0x005d58e0, 0x005d58a0};
    for (uint32_t a : xl) FP_XL(f, a);
}
PORT_FN(0x0040be10, "PostRaceTable::Draw", PostRaceTable_Draw_c, fp_PostRaceTable_Draw)

static void __cdecl xlh_prt_viper() {}
PORT_FN(0x0040c250, "PostRaceTable::Draw::XL_ViperRacing (destructor helper)", xlh_prt_viper, fp_pure0)
static void __cdecl xlh_prt_maxspeed() {}
PORT_FN(0x0040c260, "PostRaceTable::Draw::XL_MaxSpeed (destructor helper)", xlh_prt_maxspeed, fp_pure0)
static void __cdecl xlh_prt_bestlap() {}
PORT_FN(0x0040c270, "PostRaceTable::Draw::XL_BestLap (destructor helper)", xlh_prt_bestlap, fp_pure0)
static void __cdecl xlh_prt_car() {}
PORT_FN(0x0040c280, "PostRaceTable::Draw::XL_Car (destructor helper)", xlh_prt_car, fp_pure0)
static void __cdecl xlh_prt_driver() {}
PORT_FN(0x0040c290, "PostRaceTable::Draw::XL_Driver (destructor helper)", xlh_prt_driver, fp_pure0)
static void __cdecl xlh_prt_racetime() {}
PORT_FN(0x0040c2a0, "PostRaceTable::Draw::XL_RaceTime (destructor helper)", xlh_prt_racetime, fp_pure0)
static void __cdecl xlh_prt_dnf() {}
PORT_FN(0x0040c2b0, "PostRaceTable::Draw::XL_DNF (destructor helper)", xlh_prt_dnf, fp_pure0)

static PostRaceTable* __fastcall PostRaceTable_sdd_c(PostRaceTable* self, Edx, uint32_t flags) {
    const int32_t s = self->style;
    self->vtbl = (const void*)(uintptr_t)VT_PostRaceTable;
    ccall<void>(F_UIRemoveStyle_c, s);
    ccall<void>(F_UIRemoveStyle_c, (int32_t)self->style_title);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(uit::F_Delete, (void*)self);
    return self;
}
static void fp_PostRaceTable_sdd(Footprint& f, PostRaceTable*, Edx, uint32_t) { f.replay_only = "removes its styles, frees"; }
PORT_FN(0x0040c2c0, "PostRaceTable::scalar deleting destructor", PostRaceTable_sdd_c, fp_PostRaceTable_sdd)

}  // namespace root_race
}  // namespace
