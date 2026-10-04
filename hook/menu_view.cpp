// menu_view.cpp -- M3 UI stage, step U2 (group C): the choosers and viewers of the race setup screen, rewritten faithfully
// (library `menu`: trkview.obj, carview.obj, oppview.obj, optview.obj; board.obj is menu_board.cpp).
//
//   trkview.obj: TrackViewer (a track's map, friendly name and difficulty; the track steps through the order table at
//   0x4defd0, saved as option "track_no") and TrackChooser (the viewer with prev / next, the details dialog -- weather,
//   time of day, mirror, the high score board -- and the laps kept to the race type's; the test track "hell" skipped or
//   flagged). carview.obj: CarViewer3D (the car on its stand: the body and four wheels drawn in 3D, turning between five
//   poses on a Hermite curve; its .tab's stats; option "car_no", "player_paintjob") and CarChooser (paint jobs by prev /
//   next, the paint shop, the details dialog). oppview.obj: OpponentViewer (the pack, a ghost car or the clock; options
//   "opponent", "mgc_type", "sgc_type"). optview.obj: RaceOptionViewer (realism, race type, laps, AI strength and count,
//   ghost car type; two groups swapped by the race type and the event).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own too, so a
// hooked rewrite is what runs), virtual calls through the vtable, the compiler's inline string code as it runs it
// (ui_types.h). The item lists the Added / details methods build on the stack are the original's: an item the original
// builds with UIDialogItem's constructor (0x4091c0) is built with it, one it writes in place is written with the same 14
// dwords; each Xlator's text is read after its refresh, where the original reads it. x87: a register value is a double,
// a stored one a float, the original's grouping, its constants' widths, __ftol as x87_ftol, comparisons with the
// original's NaN outcome (docs/PORTING.md); floats the original passes by integer moves are passed as their bits.
//
// Footprints (main thread). Draws write the canvas they're given (its clip and pixels) and the current-canvas static;
// methods write their object and what they own. A function-local Xlator is built on its first use, which registers its
// destructor with atexit: while one of a function's is unbuilt its check is left to the session replays (as
// update_camera). replay_only: whatever loads or frees (constructors, destructors, SetTrack / set_stamp / Prev / Next,
// UpdateCar / SetModel, the 3D draws -- the renderer's own state --, OptionsGet / OptionsSet, which can add an option),
// adds or removes notifications (Create / Destroy), builds widgets (Added) or runs a dialog (details, the callbacks,
// paint's paint shop).
//
// Fixes (// FIX:, docs/FIXES.md "Menus"): the names and texts copied or formatted into fixed buffers unbounded are held to
// them -- TrackViewer::SetTrack's track name (cut to 74 characters, so "<name>.stp" fits), CarViewer3D::SetModel's model
// name (31 for the texture names; the .cf's to 251) and texture name (a bigger buffer, then the remap entry's 0x18 bytes),
// OpponentViewer::SetModel's (a name whose "<base>.car" can't fit the object's 32 bytes shows no car), SetCar's name and
// UpdateOpponent's label (their fields), UpdateOpponent's "<car>1.mod" (a bigger buffer), UpdateCar's .tab text,
// CarChooser::update_car's stat texts and RaceOptionViewer::Callback's summary (the caller's 0x20 bytes) and laps text
// (each formatted in a buffer of the rewrite's where it could pass, then cut to its field) -- and the cut at the character
// before a name's first '.' is skipped when there's none (a write to address -1) or it comes first. Every other input
// gives the original's bits.
//
// FIX CANDIDATEs (left faithful, marked in place: damaged files, stale frames, out-of-range values): TrackViewer::Draw
// indexes its three difficulty names with what GetTrackDifficulty returns (garbage for a track outside 0..7, the "no
// track" -1 among them -- FIXED in GetTrackDifficulty, root_race.cpp); the constructor clamps a too-big saved track_no but not a negative one; UpdateOpponent reads its
// label table with the opponent type unchecked; the details dialogs' static UIDialog keeps its items pointer from the
// first call (a later call from a different stack depth shows the first frame's memory).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "menu_view.h"

namespace {
namespace menu_view {
using namespace uit;
using namespace mview;

#define D(x) ((double)(x))
static __forceinline float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static __forceinline uint32_t fbits_at(const volatile float* p) { return *(const volatile uint32_t*)p; }
static __forceinline void st_f(volatile float* p, double v) { *p = (float)v; }
#define CP(p) ((const char*)(uintptr_t)(p))

// ---- footprint helpers --------------------------------------------------------------------------------------------------
static __forceinline bool xl_built(uint32_t guard, uint8_t bits) { return (UI_G8(guard) & bits) == bits; }
#define FP_XL(f, a) UI_FP(f, a, 12, "a function's static Xlator")
static __forceinline void fp_draw_canvas(Footprint& f, gxCanvas* c) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}
template <typename T> static void fp_pure0(Footprint& f, T*, Edx) { f.pure = true; }
template <typename T> static void fp_pure_canvas(Footprint& f, T*, Edx, gxCanvas*) { f.pure = true; }
template <typename T> static void fp_none0(Footprint&, T*, Edx) {}
template <typename T> static void fp_loads0(Footprint& f, T*, Edx) { f.replay_only = "loads or frees (stamps, models, options)"; }
template <typename T> static void fp_loads1(Footprint& f, T*, Edx, int32_t) { f.replay_only = "loads or frees (stamps, models, options)"; }
template <typename T> static void fp_loads_s(Footprint& f, T*, Edx, const char*) { f.replay_only = "loads or frees (stamps, models, options)"; }
template <typename T> static void fp_frees0(Footprint& f, T*, Edx) { f.replay_only = "frees (stamps, models), saves options"; }
template <typename T> static void fp_dtor_flags(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object"; }
template <typename T> static void fp_notes0(Footprint& f, T*, Edx) { f.replay_only = "adds or removes notifications (allocates, frees)"; }
template <typename T> static void fp_added0(Footprint& f, T*, Edx) { f.replay_only = "builds widgets (_UIAddItems allocates, loads stamps)"; }
template <typename T> static void fp_modal0(Footprint& f, T*, Edx) { f.replay_only = "runs a dialog (modal: its own input loop)"; }
template <typename T> static void fp_draw3d0(Footprint& f, T*, Edx) { f.replay_only = "draws models through the 3D renderer (its state, lists, DirectX)"; }
static void fp_cb_modal(Footprint& f, int32_t) { f.replay_only = "a button's callback: runs a dialog or loads"; }

// =========================================================================================================================
// trkview.obj
// =========================================================================================================================
typedef void(__cdecl* OptionsGet_t)(const char*, const char*, volatile int32_t*);
typedef void(__cdecl* OptionsSet_t)(const char*, const char*, int32_t);
#define OptionsGet ((OptionsGet_t)(uintptr_t)F_OptionsGet)
#define OptionsSet ((OptionsSet_t)(uintptr_t)F_OptionsSet)
enum : uint32_t { S_TRK_SECTION = 0x004defc8 };   // char const* "GLOBAL" (trkview's option section)

// TrackViewer::TrackViewer: option "track_no" (kept if it's below the track count), the frame stamp, SetTrack
static TrackViewer* __fastcall TrackViewer_ctor_c(TrackViewer* self, Edx) {
    volatile int32_t track;
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_TrackViewer;
    self->map = 0;
    track = 0;
    OptionsGet(UI_GP(const char, S_TRK_SECTION), CP(0x004fa278), &track);
    // FIX CANDIDATE: a negative saved track_no isn't clamped (SetTrack of it: -1 shows no track, another reads outside
    // the track table); only a corrupt options file has one
    if (!(ccall<int32_t>(F_GetTrackCount) - 1 >= track)) track = 0;
    self->frame = ccall<void*>(F_gxGetStamp, CP(0x004fa284));
    tcall<void>(F_TrackViewer_SetTrack, self, (int32_t)track);
    return self;
}
PORT_FN(0x0049ad60, "TrackViewer::TrackViewer", TrackViewer_ctor_c, fp_loads0<TrackViewer>)

// ~TrackViewer: the stamps forgotten; the track saved unless it's none
static void __fastcall TrackViewer_dtor_c(TrackViewer* self, Edx) {
    void* fr = self->frame;
    self->vtbl = (const void*)(uintptr_t)VT_TrackViewer;
    ccall<void>(F_gxForgetStamp, fr);
    if (void* m = self->map) {
        ccall<void>(F_gxForgetStamp, m);
        self->map = 0;
    }
    const int32_t t = self->track;
    if (t != -1) OptionsSet(UI_GP(const char, S_TRK_SECTION), CP(0x004fa290), t);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
PORT_FN(0x0049add0, "TrackViewer::~TrackViewer", TrackViewer_dtor_c, fp_frees0<TrackViewer>)

// TrackViewer::Draw: the map, the title bar, the difficulty (right, in a box), the name (centred), the frame
static void __fastcall TrackViewer_Draw_c(TrackViewer* self, Edx, gxCanvas* c) {
    xl_once(0x0057b840, 1, 0x0057b818, 0x004fa29c, 0x0049b050);
    xl_once(0x0057b840, 2, 0x0057b800, 0x004fa2b4, 0x0049b040);
    xl_once(0x0057b840, 4, 0x0057b888, 0x004fa2cc, 0x0049b030);
    // the frame's locals: [0] the text's width, [1..3] the three difficulties' names (the original's order)
    const char* volatile loc[4];
    loc[1] = xlate(0x0057b818);
    loc[2] = xlate(0x0057b800);
    loc[3] = xlate(0x0057b888);
    // GetTrackDifficulty's result indexes the three names unchecked: a track outside 0..7 (none, -1, among them) gave
    // garbage from its own stack, read outside the array here. FIX (in GetTrackDifficulty, root_race.cpp): such a
    // track counts as medium (1).
    const int32_t d = ccall<int32_t>(F_GetTrackDifficulty, (int32_t)self->track);
    const char* diff = loc[1 + d];
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    if (void* m = self->map) ccall<void>(F_gxDrawStamp, m, (int32_t)self->x, (int32_t)(self->y + 0x17), (int32_t)0, (const void*)0);
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t col = UI_GU32(0x0057b7ec);
        ccall<void>(F_gxRect, x, y, (int32_t)(self->w + x), y + 0x13, col);
    }
    loc[0] = (const char*)(uintptr_t)(uint32_t)ccall<int32_t>(F_UIStyleWidth, (int32_t)0x11, diff);
    {
        const int32_t h = self->h;
        const int32_t right = self->x + self->w;
        const uint32_t col = UI_GU32(0x0057b7ec);
        const int32_t hb = h - 8;
        const int32_t tw = (int32_t)(uint32_t)(uintptr_t)loc[0];
        const int32_t left = right - tw - 8;
        const int32_t bottom = h + self->y;
        ccall<void>(F_gxRect, left, (int32_t)(self->y + hb - 6), right, bottom, col);
        const int32_t ty = self->y + hb;
        const int32_t tx = self->x - tw / 2 + self->w - 5;
        ccall<void>(F_UIStyleDraw, (int32_t)0x11, tx, ty, diff, (uint32_t)2);
    }
    {
        const char* name = tcall<const char*>(F_TrackViewer_GetTrackFriendlyName, self);
        const int32_t ty = self->y + 9;
        const int32_t tx = self->x + self->w / 2;
        ccall<void>(F_UIStyleDraw, (int32_t)0x11, tx, ty, name, (uint32_t)0);
    }
    ccall<void>(F_gxDrawStamp, (void*)self->frame, (int32_t)self->x, (int32_t)self->y, (int32_t)0, (const void*)0);
}
static void fp_TrackViewer_Draw(Footprint& f, TrackViewer*, Edx, gxCanvas* c) {
    if (!xl_built(0x0057b840, 7)) { f.replay_only = "the first call builds its Xlators (atexit)"; return; }
    fp_draw_canvas(f, c);
    FP_XL(f, 0x0057b818); FP_XL(f, 0x0057b800); FP_XL(f, 0x0057b888);
}
PORT_FN(0x0049ae20, "TrackViewer::Draw", TrackViewer_Draw_c, fp_TrackViewer_Draw)

static void __fastcall TrackViewer_Create_c(TrackViewer*, Edx) {}
PORT_FN(0x0049b060, "TrackViewer::Create", TrackViewer_Create_c, fp_pure0<TrackViewer>)
static void __fastcall TrackViewer_Destroy_c(TrackViewer*, Edx) {}
PORT_FN(0x0049b070, "TrackViewer::Destroy", TrackViewer_Destroy_c, fp_pure0<TrackViewer>)

// set_stamp: the map stamp replaced
static void __fastcall TrackViewer_set_stamp_c(TrackViewer* self, Edx, const char* name) {
    if (void* m = self->map) ccall<void>(F_gxForgetStamp, m);
    self->map = ccall<void*>(F_gxGetStamp, name);
    tcall<void>(F_UICC_Dirty, self);
}
PORT_FN(0x0049b080, "TrackViewer::set_stamp", TrackViewer_set_stamp_c, fp_loads_s<TrackViewer>)

// SetTrack: -1 shows the empty map, else "<track>.stp"
static void __fastcall TrackViewer_SetTrack_c(TrackViewer* self, Edx, int32_t t) {
    char buf[0x50];
    self->track = t;
    if (t == -1) {
        tcall<void>(F_TrackViewer_set_stamp, self, CP(0x004fa2e4));
        return;
    }
    // FIX: the track's name (tracks.tab's) went into 0x50 bytes unbounded and ".stp" after it, so a name of 75 or more
    // characters overran the stack: it's cut to 74, so "<name>.stp" fits (no such map exists: none is shown, as for any
    // missing map). A name that fits is copied as before.
    {
        const char* tn = ccall<const char*>(F_GetTrackName, t);
        if (VP_FIX) ui_copy_bounded(buf, tn, sizeof buf - 5);
        else crt_strcpy(buf, tn);
    }
    char* e = buf + crt_strlen(buf);
    *(volatile uint32_t*)e = UI_GU32(0x004fa2f4);                  // ".stp" and its terminator: a dword and a byte
    *(volatile uint8_t*)(e + 4) = UI_G8(0x004fa2f8);
    tcall<void>(F_TrackViewer_set_stamp, self, (const char*)buf);
}
PORT_FN(0x0049b0b0, "TrackViewer::SetTrack", TrackViewer_SetTrack_c, fp_loads1<TrackViewer>)

// Prev / Next: the order table's neighbour
static void __fastcall TrackViewer_Prev_c(TrackViewer* self, Edx) {
    const char* order = ccall<const char*>(F_get_track_order, (int32_t)-1, ccall<const char*>(F_GetTrackName, (int32_t)self->track));
    const int32_t n = ccall<int32_t>(F_GetTrackNumber, order);
    self->track = n;
    tcall<void>(F_TrackViewer_SetTrack, self, n);
}
PORT_FN(0x0049b140, "TrackViewer::Prev", TrackViewer_Prev_c, fp_loads0<TrackViewer>)

// get_track_order: name's place in the order table, stepped by d (mod 8); a name not in it is a panic
static const char* __cdecl get_track_order_c(int32_t d, const char* name) {
    int32_t found = -1;
    int32_t i = 0;
    for (uint32_t p = S_TRACK_ORDER; p < 0x004deff0; p += 4, i++)
        if (ccall<int>(F_stricmp, name, UI_GP(const char, p)) == 0) { found = i; break; }
    if (found == -1) {
        UI_LogPanic(CP(0x004fa2fc), name);
        return 0;
    }
    return UI_GP(const char, S_TRACK_ORDER + (((uint32_t)(d + found) & 7u) << 2));
}
static void fp_get_track_order(Footprint&, int32_t, const char*) {}
PORT_FN(0x0049b170, "get_track_order", get_track_order_c, fp_get_track_order)

static void __fastcall TrackViewer_Next_c(TrackViewer* self, Edx) {
    const char* order = ccall<const char*>(F_get_track_order, (int32_t)1, ccall<const char*>(F_GetTrackName, (int32_t)self->track));
    const int32_t n = ccall<int32_t>(F_GetTrackNumber, order);
    self->track = n;
    tcall<void>(F_TrackViewer_SetTrack, self, n);
}
PORT_FN(0x0049b1e0, "TrackViewer::Next", TrackViewer_Next_c, fp_loads0<TrackViewer>)

static const char* __fastcall TrackViewer_GetTrackName_c(TrackViewer* self, Edx) {
    const int32_t t = self->track;
    if (t == -1) return CP(0x004fa32c);
    return ccall<const char*>(F_GetTrackName, t);
}
PORT_FN(0x0049b210, "TrackViewer::GetTrackName", TrackViewer_GetTrackName_c, fp_none0<TrackViewer>)
static const char* __fastcall TrackViewer_GetTrackFriendlyName_c(TrackViewer* self, Edx) {
    const int32_t t = self->track;
    if (t == -1) return CP(0x004fa330);
    return ccall<const char*>(F_GetTrackFriendlyName, t);
}
PORT_FN(0x0049b230, "TrackViewer::GetTrackFriendlyName", TrackViewer_GetTrackFriendlyName_c, fp_none0<TrackViewer>)
static const char* __fastcall TrackViewer_GetTrackText_c(TrackViewer* self, Edx) {
    const int32_t t = self->track;
    if (t == -1) return CP(0x004fa334);
    return ccall<const char*>(F_GetTrackText, t);
}
PORT_FN(0x0049b250, "TrackViewer::GetTrackText", TrackViewer_GetTrackText_c, fp_none0<TrackViewer>)

// TrackChooser::update_track: the friendly name into the caller's buffer
static void __fastcall TrackChooser_update_track_c(TrackChooser* self, Edx) {
    const char* s = tcall<const char*>(F_TrackViewer_GetTrackFriendlyName, &self->viewer);
    const uint32_t n = crt_strlen(s) + 1;
    crt_copy(self->name, s, n);
}
static void fp_TrackChooser_update_track(Footprint& f, TrackChooser* self, Edx) {
    const char* s = tcall<const char*>(F_TrackViewer_GetTrackFriendlyName, &self->viewer);
    if (s && self->name) f.add(self->name, crt_strlen(s) + 1, "the chooser's track name");
}
PORT_FN(0x0049b270, "TrackChooser::update_track", TrackChooser_update_track_c, fp_TrackChooser_update_track)

// do_board: the track's high score board
static void __fastcall TrackChooser_do_board_c(TrackChooser* self, Edx) {
    GameOptions* o = self->opts;
    const uint8_t mirror = o->mirror;
    const int32_t race_type = o->race_type;
    const int32_t realism = o->realism;
    const char* name = tcall<const char*>(F_TrackViewer_GetTrackName, &self->viewer);
    ccall<void>(F_BoardDo, name, realism, race_type, mirror);
}
PORT_FN(0x0049b2b0, "TrackChooser::do_board", TrackChooser_do_board_c, fp_modal0<TrackChooser>)

static TrackChooser* __fastcall TrackChooser_ctor_c(TrackChooser* self, Edx, uint8_t flags, volatile uint8_t* not_test, char* name,
                                                   GameOptions* opts) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    tcall<TrackViewer*>(F_TrackViewer_ctor, &self->viewer);
    self->vtbl = (const void*)(uintptr_t)VT_TrackChooser;
    self->not_test = not_test;
    self->flags = flags;
    self->name = name;
    self->opts = opts;
    UI_GP(TrackChooser, S_TRACK_CHOOSER) = self;
    return self;
}
static void fp_TrackChooser_ctor(Footprint& f, TrackChooser*, Edx, uint8_t, volatile uint8_t*, char*, GameOptions*) {
    f.replay_only = "builds a TrackViewer (loads stamps, reads options)";
}
PORT_FN(0x0049b2d0, "TrackChooser::TrackChooser", TrackChooser_ctor_c, fp_TrackChooser_ctor)

static void __fastcall TrackChooser_dtor_c(TrackChooser* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_TrackChooser;
    UI_GP(TrackChooser, S_TRACK_CHOOSER) = 0;
    tcall<void>(F_TrackViewer_dtor, &self->viewer);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
PORT_FN(0x0049b320, "TrackChooser::~TrackChooser", TrackChooser_dtor_c, fp_frees0<TrackChooser>)

// details: the track's dialog -- weather, time of day, mirror, the board button
static void __fastcall TrackChooser_details_c(TrackChooser* self, Edx) {
    UIDialogItem it[13];
    xl_once(0x0057b80c, 0x01, 0x0057b7e0, 0x004fa34c, 0x0049b8d0);
    xl_once(0x0057b80c, 0x02, 0x0057b898, 0x004fa36c, 0x0049b8c0);
    xl_once(0x0057b80c, 0x04, 0x0057b830, 0x004fa384, 0x0049b8b0);
    xl_once(0x0057b80c, 0x08, 0x0057b858, 0x004fa398, 0x0049b8a0);
    xl_once(0x0057b80c, 0x10, 0x0057b868, 0x004fa3b0, 0x0049b890);
    xl_once(0x0057b80c, 0x20, 0x0057b7d0, 0x004fa3d0, 0x0049b880);
    item_ctor(&it[0], 0xe, 0, 0x1d, 0x16, 0, 0, CP(0x004fa3d8), 0, 0, 0);
    item_ctor(&it[1], 2, -1, 0x13, 0x1a2, 0, 0, xlate(0x0057b7d0), 0, 0, 6);
    {
        const char* t = xlate(0x0057b898);
        item_ctor(&it[2], 0xb, 0, 0x104, 0x14a, 0, 0, t, 0, (const void*)&self->opts->mirror, 7);
    }
    item14(&it[3], 5, 0, 0x28, 0x168, 0, 0, xlate(0x0057b830), 0, 0, 0xb);
    for (int32_t k = 0; k < 3; k++) {
        const void* w = (const void*)&self->opts->weather;
        const char* s = ccall<const char*>(F_GetWeatherString, k);
        item_ctor(&it[4 + k], 0xc, 0, 0x32, 0x17c + 0xf * k, 0, 0, s, k, w, 8);
    }
    item_ctor(&it[7], 5, 0, 0x28, 0x11d, 0, 0, xlate(0x0057b858), 0, 0, 0xb);
    for (int32_t k = 0; k < 2; k++) {
        const void* g = (const void*)&self->opts->game_time;
        const char* s = ccall<const char*>(F_GetGameTimeString, k);
        item_ctor(&it[8 + k], 0xc, 0, 0x32, 0x131 + 0xf * k, 0, 0, s, k, g, 8);
    }
    {
        const char* s = ccall<const char*>(F_GetGameTimeString, (int32_t)2);
        item14(&it[10], 0xc, 0, 0x32, 0x14f, 0, 0, s, 2, (const void*)&self->opts->game_time, 8);
    }
    item14(&it[11], 2, 3, 0x112, 0x11c, 0, 0, xlate(0x0057b868), 0, (const void*)(uintptr_t)F_board_cb, 0);
    item_end(&it[12]);
    // FIX CANDIDATE: the static dialog's items are set once, to this call's frame; a later call from a different stack
    // depth hands UIDoDialog the first frame's memory (the race setup screen always calls it from the same depth)
    if (!(UI_G8(0x0057b80c) & 0x40)) {
        UI_GU32(0x004fa348) = 0;
        UI_G8(0x0057b80c) = (uint8_t)(UI_G8(0x0057b80c) | 0x40);
        UI_GU32(0x004fa344) = (uint32_t)(uintptr_t)&it[0];
    }
    const int32_t h = UI_G32(S_GX_SCREEN_H), w = UI_G32(S_GX_SCREEN_W);
    ccall<int32_t>(F_UIDoDialog, (const void*)(uintptr_t)S_TRK_DIALOG, w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
}
PORT_FN(0x0049b350, "TrackChooser::details", TrackChooser_details_c, fp_modal0<TrackChooser>)

static uint8_t __cdecl board_cb_c(int32_t) {
    tcall<void>(F_TrackChooser_do_board, UI_GP(TrackChooser, S_TRACK_CHOOSER));
    return 0;
}
PORT_FN(0x0049b870, "board_cb", board_cb_c, fp_cb_modal)

// TrackChooser::Added: prev / next, the viewer, the details button
static void __fastcall TrackChooser_Added_c(TrackChooser* self, Edx) {
    UIDialogItem it[5];
    xl_once(0x0057b83c, 1, 0x0057b848, 0x004fa3f4, 0x0049bb20);
    const int32_t x = self->x, y = self->y;
    item14(&it[0], 3, 1, x - 2, y + 0x9c, 0, 0, CP(0x004fa408), 0, (const void*)(uintptr_t)F_prev_track_cb, 0);
    item14(&it[1], 3, 2, x + 0x88, y + 0x9c, 0, 0, CP(0x004fa414), 0, (const void*)(uintptr_t)F_next_track_cb, 0);
    item14(&it[2], 0x17, 0, x, y, 0xb3, 0x8d, CP(0x004e4db8), 0, (const void*)&self->viewer, 0);
    const char* t = xlate(0x0057b848);
    item14(&it[3], 2, 3, self->x + 0x2a, self->y + 0x98, 0, 0, t, 0, (const void*)(uintptr_t)F_board_cb, 1);
    item_end(&it[4]);
    tcall<void>(F_UICC_AddItems, self, &it[0]);
    tcall<void>(F_TrackChooser_update_track, self);
}
PORT_FN(0x0049b8e0, "TrackChooser::Added", TrackChooser_Added_c, fp_added0<TrackChooser>)

static uint8_t __cdecl next_track_cb_c(int32_t) {
    tcall<void>(F_TrackChooser_next_track, UI_GP(TrackChooser, S_TRACK_CHOOSER));
    return 0;
}
PORT_FN(0x0049bb00, "next_track_cb", next_track_cb_c, fp_cb_modal)
static uint8_t __cdecl prev_track_cb_c(int32_t) {
    tcall<void>(F_TrackChooser_prev_track, UI_GP(TrackChooser, S_TRACK_CHOOSER));
    return 0;
}
PORT_FN(0x0049bb10, "prev_track_cb", prev_track_cb_c, fp_cb_modal)

// Create: watch the race type and the name; the race type's laps now
static void __fastcall TrackChooser_Create_c(TrackChooser* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->opts->race_type, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->name, (uint32_t)0xd);
    vcall<void>(self, 0x10, (int32_t)0, (const void*)&self->opts->race_type);
}
PORT_FN(0x0049bb30, "TrackChooser::Create", TrackChooser_Create_c, fp_notes0<TrackChooser>)
static void __fastcall TrackChooser_Destroy_c(TrackChooser* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->opts->race_type);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)self->name);
}
PORT_FN(0x0049bb70, "TrackChooser::Destroy", TrackChooser_Destroy_c, fp_notes0<TrackChooser>)

// Callback: the laps for the race type on this track (20 on the test track), and whether it's the test track
static void __fastcall TrackChooser_Callback_c(TrackChooser* self, Edx, int32_t, const void*) {
    TrackViewer* v = &self->viewer;
    if (*(const volatile char*)tcall<const char*>(F_TrackViewer_GetTrackName, v) == 0) return;
    GameOptions* o = self->opts;
    const char* n = tcall<const char*>(F_TrackViewer_GetTrackName, v);
    o->laps = ccall<int32_t>(F_GetLapCountFromType, (int32_t)o->race_type, n);
    const uint8_t t = ccall<uint8_t>(F_is_test_track, tcall<const char*>(F_TrackViewer_GetTrackName, v));
    *self->not_test = (uint8_t)(t == 0 ? 1 : 0);
    if (ccall<uint8_t>(F_is_test_track, tcall<const char*>(F_TrackViewer_GetTrackName, v))) self->opts->laps = 0x14;
}
static void fp_TrackChooser_Callback(Footprint& f, TrackChooser* self, Edx, int32_t, const void*) {
    f.add(self, sizeof *self, "this");
    if (self->opts) f.add(self->opts, sizeof(GameOptions), "the race's options");
    if (self->not_test) f.add((void*)self->not_test, 1, "the not-the-test-track flag");
}
PORT_FN(0x0049bb90, "TrackChooser::Callback", TrackChooser_Callback_c, fp_TrackChooser_Callback)

static uint8_t __cdecl is_test_track_c(const char* name) {
    const int r = ccall<int>(F_stricmp, name, CP(0x004fa420));
    return (uint8_t)(r == 0 ? 1 : 0);
}
static void fp_is_test_track(Footprint&, const char*) {}
PORT_FN(0x0049bc00, "is_test_track", is_test_track_c, fp_is_test_track)

// next_track / prev_track: with "no track" a choice (flags 2) it comes between the last and the first; the test track
// skipped (flags 4)
static void __fastcall TrackChooser_next_track_c(TrackChooser* self, Edx) {
    TrackViewer* v = &self->viewer;
    if ((self->flags & 2) && v->track != -1) {
        tcall<void>(F_TrackViewer_Next, v);
        if (v->track == 0) tcall<void>(F_TrackViewer_SetTrack, v, (int32_t)-1);
    } else {
        tcall<void>(F_TrackViewer_Next, v);
    }
    if (self->flags & 4)
        if (ccall<uint8_t>(F_is_test_track, tcall<const char*>(F_TrackViewer_GetTrackName, v))) tcall<void>(F_TrackViewer_Next, v);
    tcall<void>(F_TrackChooser_update_track, self);
}
PORT_FN(0x0049bc20, "TrackChooser::next_track", TrackChooser_next_track_c, fp_loads0<TrackChooser>)
static void __fastcall TrackChooser_prev_track_c(TrackChooser* self, Edx) {
    TrackViewer* v = &self->viewer;
    if ((self->flags & 2) && v->track == 0) tcall<void>(F_TrackViewer_SetTrack, v, (int32_t)-1);
    else tcall<void>(F_TrackViewer_Prev, v);
    if (self->flags & 4)
        if (ccall<uint8_t>(F_is_test_track, tcall<const char*>(F_TrackViewer_GetTrackName, v))) tcall<void>(F_TrackViewer_Prev, v);
    tcall<void>(F_TrackChooser_update_track, self);
}
PORT_FN(0x0049bc80, "TrackChooser::prev_track", TrackChooser_prev_track_c, fp_loads0<TrackChooser>)

static void* __fastcall TrackViewer_sdd_c(TrackViewer* self, Edx, uint32_t flags) {
    tcall<void>(F_TrackViewer_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0049bce0, "TrackViewer::vector deleting destructor", TrackViewer_sdd_c, fp_dtor_flags<TrackViewer>)
static void __fastcall TrackChooser_Draw_c(TrackChooser*, Edx, gxCanvas*) {}
PORT_FN(0x0049bd00, "TrackChooser::Draw", TrackChooser_Draw_c, fp_pure_canvas<TrackChooser>)
static void* __fastcall TrackChooser_vdd_c(TrackChooser* self, Edx, uint32_t flags) {
    tcall<void>(F_TrackChooser_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0049bd10, "TrackChooser::scalar deleting destructor", TrackChooser_vdd_c, fp_dtor_flags<TrackChooser>)

// =========================================================================================================================
// carview.obj
// =========================================================================================================================
enum : uint32_t { S_CAR_SECTION = 0x004df098 };   // char const* "GLOBAL" (carview's option section)
typedef void(__cdecl* Sprintf_v)(char*, const char*, ...);
#define game_sprintf ((Sprintf_v)(uintptr_t)F_sprintf)
// FIX helper (CarChooser::update_car): the game's sprintf into the rewrite's buffer t (0x400 bytes), then the field keeps
// what fits it (size - 1 characters and a terminator) -- the same bytes as formatting into it, for a text that fits. Every
// %s argument is fix_cut to 255 characters and a number prints at most 78 digits (a float times a float), so nothing
// passes t. The faithful build formats into the field.
template <typename... A> static __forceinline void fix_format(char* field, uint32_t size, char* t, const char* fmt, A... a) {
    if (VP_FIX) {
        game_sprintf(t, fmt, a...);
        ui_copy_bounded(field, t, size);
    } else {
        game_sprintf(field, fmt, a...);
    }
}
static __forceinline bool car_list_is_root(const CarViewer3D* v) {       // the root library's car list (read only)
    return (uint32_t)(uintptr_t)v->count == 0x00406920u && (uint32_t)(uintptr_t)v->name_of == 0x00406910u;
}

// CarViewer3D::CarViewer3D(car, count, name_of): car -1 is option "car_no" (the viper without the hack), clamped to the
// list; option "player_paintjob"; the frame stamp; UpdateCar
static CarViewer3D* __fastcall CarViewer3D_ctor_c(CarViewer3D* self, Edx, int32_t car, CarCountFn count, CarNameFn name_of) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_CarViewer3D;
    *(volatile char*)self->set = 0;
    self->car = 0;
    self->count = count;
    self->name_of = name_of;
    const char* section;
    if (car == -1) {
        section = UI_GP(const char, S_CAR_SECTION);
        OptionsGet(section, CP(0x004fa4b4), &self->car);
        if (!ccall<uint8_t>(F_HackEnabled)) self->car = ccall<int32_t>(F_GetCarFileNumber, CP(0x004fa4bc));
    } else {
        section = UI_GP(const char, S_CAR_SECTION);
        self->car = car;
    }
    self->paint = 0;
    OptionsGet(section, CP(0x004fa4c4), &self->paint);
    if (!(count() > self->car)) self->car = 0;
    self->model = 0;
    self->pose = 0;
    *(volatile uint32_t*)&self->t = 0;
    self->tab = 0;
    self->frame_stamp = ccall<void*>(F_gxGetStamp, CP(0x004fa4d4));
    tcall<void>(F_CarViewer3D_UpdateCar, self);
    return self;
}
static void fp_CarViewer3D_ctor(Footprint& f, CarViewer3D*, Edx, int32_t, CarCountFn, CarNameFn) {
    f.replay_only = "loads the car (models, its .tab), reads options";
}
PORT_FN(0x0049bf50, "CarViewer3D::CarViewer3D", CarViewer3D_ctor_c, fp_CarViewer3D_ctor)

static void __fastcall CarViewer3D_dtor_c(CarViewer3D* self, Edx) {
    const int32_t car = self->car;
    self->vtbl = (const void*)(uintptr_t)VT_CarViewer3D;
    OptionsSet(UI_GP(const char, S_CAR_SECTION), CP(0x004fa4e0), car);
    ccall<void>(F_gxForgetStamp, (void*)self->frame_stamp);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
PORT_FN(0x0049c020, "CarViewer3D::~CarViewer3D", CarViewer3D_dtor_c, fp_frees0<CarViewer3D>)

// Create: the camera at the origin, the car 5 ahead and half a unit down
static void __fastcall CarViewer3D_Create_c(CarViewer3D* self, Edx) {
    volatile uint32_t* c = (volatile uint32_t*)&self->camera;
    for (int i = 0; i < 24; i++) c[i] = (i == 0 || i == 4 || i == 8 || i == 12 || i == 16 || i == 20) ? 0x3f800000u : 0u;
    *(volatile uint32_t*)&self->body.p[1] = 0xbf000000u;
    *(volatile uint32_t*)&self->body.p[2] = 0x40a00000u;
}
static void fp_CarViewer3D_Create(Footprint& f, CarViewer3D* self, Edx) { f.add(self, sizeof *self, "this"); }
PORT_FN(0x0049c060, "CarViewer3D::Create", CarViewer3D_Create_c, fp_CarViewer3D_Create)

static void __fastcall CarViewer3D_Destroy_c(CarViewer3D* self, Edx) {
    tcall<void>(F_CarViewer3D_SetModel, self, (const char*)0);
    if (void* t = self->tab) {
        ccall<void>(F_StringTableForget, t);
        self->tab = 0;
    }
    if (*(volatile char*)self->set != 0) {
        ccall<void>(F_ResourceSetUnload, (const char*)self->set);
        *(volatile char*)self->set = 0;
    }
}
PORT_FN(0x0049c0f0, "CarViewer3D::Destroy", CarViewer3D_Destroy_c, fp_frees0<CarViewer3D>)

// Draw: the title bar, the car's name, the frame
static void __fastcall CarViewer3D_Draw_c(CarViewer3D* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t col = UI_GU32(0x0057b924);
        ccall<void>(F_gxRect, x, y, (int32_t)(self->w + x), y + 0x13, col);
    }
    {
        const char* name = tcall<const char*>(F_CarViewer3D_GetFriendlyName, self);
        const int32_t ty = self->y + 9;
        const int32_t tx = self->x + self->w / 2;
        ccall<void>(F_UIStyleDraw, (int32_t)0x11, tx, ty, name, (uint32_t)0);
    }
    ccall<void>(F_gxDrawStamp, (void*)self->frame_stamp, (int32_t)self->x, (int32_t)self->y, (int32_t)0, (const void*)0);
}
static void fp_CarViewer3D_Draw(Footprint& f, CarViewer3D*, Edx, gxCanvas* c) { fp_draw_canvas(f, c); }
PORT_FN(0x0049c130, "CarViewer3D::Draw", CarViewer3D_Draw_c, fp_CarViewer3D_Draw)

// Draw3D: the car turns from pose `pose` to the next on a Hermite curve (yaw, a world rotation, a z offset from the table
// at 0x4fa428, 12 bytes a pose); the body, then the four wheels at the CarFile's positions (inches), mirrored across
typedef double(__cdecl* DblFn0)();
typedef double(__cdecl* HermiteFn)(uint32_t);
static void __fastcall CarViewer3D_Draw3D_c(CarViewer3D* self, Edx) {
    volatile float M1[9], M2[9];
    MvFrame W;
    volatile float h, a, yaw, rot, e24, e48, e80;
    {
        const int32_t hh = self->h, ww = self->w;
        ccall<void>(F_mrSetView, (int32_t)(self->x + 6), (int32_t)(self->y + 6), ww - 0xc, hh - 0xc, (uint8_t)0);
    }
    ccall<void>(F_mrSetProjection, (uint32_t)0x3e4ccccd, (uint32_t)0x447a0000, (uint32_t)0x3f9c61aa);
    ccall<void>(F_mrSetCamera, (void*)&self->camera);
    ccall<void>(F_mrEnable, (int32_t)0x10);
    ccall<void>(F_mrEnable, (int32_t)1);
    if (UI_G32(S_MR_DEVICE) >= 2) ccall<void>(F_mrModelEnvMap, (uint8_t)1);
    else ccall<void>(F_mrEnable, (int32_t)4);
    const int32_t next = (self->pose + 1) % 5;
    h = (float)((HermiteFn)(uintptr_t)F_Hermite)(fbits_at(&self->t));
    if (fbits_at(&self->t) > 0x80000000u) *(volatile uint32_t*)&h = 0;
    MvFrame* F = &self->body;
    *(volatile uint32_t*)&F->m[1] = 0;
    *(volatile uint32_t*)&F->m[0] = 0x3f800000u;
    *(volatile uint32_t*)&F->m[4] = 0x3f800000u;
    *(volatile uint32_t*)&F->m[2] = 0;
    *(volatile uint32_t*)&F->m[3] = 0;
    *(volatile uint32_t*)&F->m[5] = 0;
    *(volatile uint32_t*)&F->m[6] = 0;
    *(volatile uint32_t*)&F->m[7] = 0;
    *(volatile uint32_t*)&F->m[8] = 0x3f800000u;
    *(volatile uint32_t*)&F->p[0] = 0;
    const int32_t cur = self->pose;
    *(volatile uint32_t*)&F->p[1] = 0;
    *(volatile uint32_t*)&F->p[2] = 0;
    const uint32_t tn = 0x004fa428u + (uint32_t)next * 12u, tc = 0x004fa428u + (uint32_t)cur * 12u;
    a = (float)((D(UI_GF(tn + 8)) - D(UI_GF(tc + 8))) * D(h) + D(UI_GF(tc + 8)));
    yaw = (float)((D(UI_GF(tn)) - D(UI_GF(tc))) * D(h) + D(UI_GF(tc)));
    ccall<void>(F_MatrixMakeYaw, (void*)M1, fbits_at(&yaw));
    ccall<void>(F_MatrixMakePitch, (void*)M2, (uint32_t)0);
    ccall<void>(F_MatrixConcat, (void*)M1, (const void*)M2, (const void*)M1);
    ccall<void>(F_MatrixMakeRoll, (void*)M2, (uint32_t)0);
    ccall<void>(F_MatrixConcat, (void*)M1, (const void*)M2, (const void*)M1);
    ccall<void>(F_MatrixConcat, (void*)F, (const void*)M1, (const void*)F);
    {
        const uint32_t tc2 = 0x004fa42cu + (uint32_t)self->pose * 12u;
        rot = (float)((D(UI_GF(tn + 4)) - D(UI_GF(tc2))) * D(h) + D(UI_GF(tc2)));
    }
    ccall<void>(F_MatrixMakeWorldRotation, (void*)M2, (uint32_t)0, fbits_at(&rot), (uint32_t)0);
    ccall<void>(F_MatrixConcat, (void*)F, (const void*)F, (const void*)M2);
    {
        const double py = D(F->p[1]) - D(bits_f(0x3f000000));
        const int32_t model = self->model;
        st_f(&F->p[1], py);
        st_f(&F->p[2], (D(bits_f(0x40900000)) - D(a)) + D(F->p[2]));
        ccall<void>(F_mrModelDraw, model, (const void*)F);
    }
    crt_copy(&W, (const void*)F, 0x30);
    const float k127 = bits_f(0x3c5013a9), k508 = bits_f(0x3d5013a9), km2 = bits_f(0xc0000000);
    e80 = (float)(D(UI_GF(0x0057b9e0)) * D(k127) + D(k508));
    e48 = (float)(D(UI_GF(0x0057b9e4)) * D(k127) + D(k508));
    e24 = (float)(D(UI_GF(0x0057b9f8)) * D(k127));
    {
        const double r = D(UI_GF(0x0057bb70)) * D(k127) - D(k508);
        st_f(&W.p[0], ((D(W.m[3]) * r + D(F->p[0])) + D(W.m[6]) * D(e24)) + D(e80) * D(W.m[0]));
        st_f(&W.p[1], ((D(W.m[4]) * r + D(F->p[1])) + D(W.m[7]) * D(e24)) + D(e80) * D(W.m[1]));
        st_f(&W.p[2], ((r * D(W.m[5]) + D(F->p[2])) + D(W.m[8]) * D(e24)) + D(e80) * D(W.m[2]));
    }
    ccall<void>(F_mrModelDraw, (int32_t)self->wheel[0], (const void*)&W);
    st_f(&W.p[0], D(e80) * D(W.m[0]) * D(km2) + D(W.p[0]));
    st_f(&W.p[1], D(e80) * D(W.m[1]) * D(km2) + D(W.p[1]));
    st_f(&W.p[2], D(e80) * D(W.m[2]) * D(km2) + D(W.p[2]));
    ccall<void>(F_mrModelDraw, (int32_t)self->wheel[0], (const void*)&W);
    {
        const double s = D(e48) + D(e80);
        st_f(&W.p[0], D(W.m[0]) * s + D(W.p[0]));
        st_f(&W.p[1], D(W.m[1]) * s + D(W.p[1]));
        st_f(&W.p[2], s * D(W.m[2]) + D(W.p[2]));
    }
    st_f(&W.p[0], D(W.m[6]) * D(e24) * D(km2) + D(W.p[0]));
    st_f(&W.p[1], D(W.m[7]) * D(e24) * D(km2) + D(W.p[1]));
    st_f(&W.p[2], D(W.m[8]) * D(e24) * D(km2) + D(W.p[2]));
    ccall<void>(F_mrModelDraw, (int32_t)self->wheel[1], (const void*)&W);
    st_f(&W.p[0], D(e48) * D(W.m[0]) * D(km2) + D(W.p[0]));
    st_f(&W.p[1], D(e48) * D(W.m[1]) * D(km2) + D(W.p[1]));
    st_f(&W.p[2], D(e48) * D(W.m[2]) * D(km2) + D(W.p[2]));
    ccall<void>(F_mrModelDraw, (int32_t)self->wheel[1], (const void*)&W);
    {
        // fcom on the unrounded sum: past 1.1 (ordered), the next pose
        const double v = ((DblFn0)(uintptr_t)F_UIDeltaT)() * D(bits_f(0x3e800000)) + D(self->t);
        st_f(&self->t, v);
        if (v > D(bits_f(0x3f8ccccd))) {
            const double u = D(self->t) - D(bits_f(0x3f8ccccd));
            self->pose = next;
            st_f(&self->t, u);
        }
    }
    if (UI_G32(S_MR_DEVICE) >= 2) ccall<void>(F_mrModelEnvMap, (uint8_t)0);
    else ccall<void>(F_mrDisable, (int32_t)4);
}
PORT_FN(0x0049c1b0, "CarViewer3D::Draw3D", CarViewer3D_Draw3D_c, fp_draw3d0<CarViewer3D>)

// Hermite: 3t^2 - 2t^3 mirrored for t < 0 (-3t^2 - 2t^3), clamped to [-1, 1] by the float's bits
static double __cdecl Hermite_c(uint32_t xb) {
    if (xb > 0xbf800000u) return D(bits_f(0xbf800000u));
    if ((int32_t)xb > 0x3f800000) return D(bits_f(0x3f800000u));
    const double x = D(bits_f(xb));
    double t = x * D(bits_f(0xc0000000u));
    if ((int32_t)xb > 0) t = t + D(bits_f(0x40400000u));
    else t = t - D(bits_f(0x40400000u));
    t = t * x;
    t = t * x;
    return t;
}
static void fp_Hermite(Footprint& f, uint32_t) { f.pure = true; }
PORT_FN(0x0049c680, "Hermite", Hermite_c, fp_Hermite)

// SetModel(name): the old models freed; "<name>.mod" with the paint job's texture remapped; the car's .cf for the
// wheels' sizes; wheel_1.mod scaled into the front and rear wheel copies
static void __fastcall CarViewer3D_SetModel_c(CarViewer3D* self, Edx, const char* name) {
    char base[0x20], tex[VP_FIX ? 0x40 : 0x20], cfn[0x100];         // FIX: (below) tex has room for "~<base>.tex"
    volatile float S[9], ext[6];
    volatile float fa, fb, fc, fd, dx, dy, dz;
    if (self->model) {
        ccall<void>(F_mrModelUnload, (int32_t)self->model);
        ccall<void>(F_mrModelDestroyCopy, (int32_t)self->wheel[0]);
        ccall<void>(F_mrModelDestroyCopy, (int32_t)self->wheel[1]);
    }
    self->model = 0;
    if (name) {
        // FIX: a model name of 32 or more characters (a mod car's name of 27 or more, "<car>0.mod") overran the stack from
        // its 0x20 bytes: it's cut to 31 for the texture names (too long to name a texture: the car shows unpainted); the
        // model and the .cf are still loaded by the whole name. The name is cut at the character before its first '.' (UpdateCar's
        // "<car>0.mod" -> "<car>"): with no '.' that wrote to address -1 (a crash), with a '.' first it wrote before
        // the buffer; neither is cut now. WorldGetCarTexture's "~<base>.tex" (up to 37 bytes) overran the 0x20-byte
        // texture name; it has 0x40. The texture name then went into the remap entry's 16-byte field unbounded, over
        // its value and pointer and past the entry (into an Xlator) from 24 characters on: it keeps to the entry's
        // 0x18 bytes, then the value and pointer are zeroed as before, so the model reads its first 16 characters, the
        // most a texture name has. ("<base>.tex", at most 36 bytes now, keeps to the entry's 0x28 as it always did for
        // a 31-character name: the texture name and the zeros go over its tail.) A name that fits is the same.
        if (VP_FIX) ui_copy_bounded(base, name, sizeof base);
        else crt_strcpy(base, name);
        {
            char* const dot = ccall<char*>(F_strchr, (const char*)base, (int)0x2e);
            if (!(VP_FIX && (dot == 0 || dot == base))) *(volatile char*)(dot - 1) = 0;
        }
        ccall<void>(F_WorldGetCarTexture, (char*)tex, (const char*)base, (int32_t)(-1 - self->paint));
        game_sprintf((char*)(uintptr_t)S_REMAP_CAR, CP(0x004fa4e8), (const char*)base);
        if (VP_FIX) ui_copy_bounded((char*)(uintptr_t)(S_REMAP_CAR + 0x10), tex, 0x18);
        else crt_strcpy((char*)(uintptr_t)(S_REMAP_CAR + 0x10), tex);
        UI_GU32(S_REMAP_CAR + 0x20) = 0;
        UI_GU32(S_REMAP_CAR + 0x24) = 0;
        self->model = ccall<int32_t>(F_mrModelLoadRemap, name, (void*)(uintptr_t)S_REMAP_CAR, (int32_t)1);
        // FIX: (above) the .cf's name: the name up to 251 characters, so "<name>.cf" fits the 0x100 bytes whether or not
        // it's cut (a name of 255 or more characters overran them); cut as above
        if (VP_FIX) ui_copy_bounded(cfn, name, sizeof cfn - 4);
        else crt_strcpy(cfn, name);
        {
            char* const dot = ccall<char*>(F_strrchr, (const char*)cfn, (int)0x2e);
            if (!(VP_FIX && (dot == 0 || dot == cfn))) *(volatile char*)(dot - 1) = 0;
        }
        *(volatile uint32_t*)(cfn + crt_strlen(cfn)) = UI_GU32(0x004fa4f0);          // ".cf" and its terminator
        if (!ccall<uint8_t>(F_CarFileLoad, (void*)(uintptr_t)S_CARFILE, (const char*)cfn, (void*)0)) UI_LogPanic(CP(0x004fa4f4), (const char*)cfn);
        const float k001 = bits_f(0x3a83126f), k1e5 = bits_f(0x3727c5ad), k127 = bits_f(0x3c5013a9), k2 = bits_f(0x40000000);
        fa = (float)(D(UI_GF(0x0057bb68)) * D(k001));
        fb = (float)((D(UI_GF(0x0057bb6c)) * D(UI_GF(0x0057bb68)) * D(k1e5) + D(UI_GF(0x0057bb70)) * D(k127)) * D(k2));
        fc = (float)(D(UI_GF(0x0057bb74)) * D(k001));
        fd = (float)((D(UI_GF(0x0057bb78)) * D(UI_GF(0x0057bb74)) * D(k1e5) + D(UI_GF(0x0057bb7c)) * D(k127)) * D(k2));
        const int32_t m = ccall<int32_t>(F_mrModelLoad, CP(0x004fa504));
        ccall<void>(F_mrModelGetExtents, m, (void*)&ext[0], (void*)&ext[1], (void*)&ext[2], (void*)&ext[3], (void*)&ext[4],
                    (void*)&ext[5]);
        dx = (float)(D(ext[1]) - D(ext[0]));
        dy = (float)(D(ext[3]) - D(ext[2]));
        *(volatile uint32_t*)&S[1] = 0;
        *(volatile uint32_t*)&S[2] = 0;
        *(volatile uint32_t*)&S[3] = 0;
        dz = (float)(D(ext[5]) - D(ext[4]));
        S[0] = (float)(D(fa) / D(dx));
        S[4] = (float)(D(fb) / D(dy));
        *(volatile uint32_t*)&S[5] = 0;
        *(volatile uint32_t*)&S[7] = 0;
        *(volatile uint32_t*)&S[6] = 0;
        S[8] = (float)(D(fb) / D(dz));
        self->wheel[0] = ccall<int32_t>(F_mrModelCopyTransform, m, (const void*)S);
        {
            const double s0 = D(fc) / D(dx);
            *(volatile uint32_t*)&S[1] = 0;
            *(volatile uint32_t*)&S[3] = 0;
            *(volatile uint32_t*)&S[5] = 0;
            *(volatile uint32_t*)&S[6] = 0;
            *(volatile uint32_t*)&S[7] = 0;
            *(volatile uint32_t*)&S[2] = 0;
            S[0] = (float)s0;
        }
        S[4] = (float)(D(fd) / D(dy));
        S[8] = (float)(D(fd) / D(dz));
        self->wheel[1] = ccall<int32_t>(F_mrModelCopyTransform, m, (const void*)S);
        ccall<void>(F_mrModelUnload, m);
    }
    *(volatile uint32_t*)&self->t = 0xbf000000u;
    self->pose = 0;
    tcall<void>(F_UICC_Dirty, self);
}
PORT_FN(0x0049c6e0, "CarViewer3D::SetModel", CarViewer3D_SetModel_c, fp_loads_s<CarViewer3D>)

// UpdateCar: the car's resource set loaded (if it changed) and its model; the stats from its .tab (row 1)
static __forceinline const char* tab_entry(CarViewer3D* self, int32_t i) {
    void* t = self->tab;
    return t ? ccall<const char*>(F_StringTableGetEntry, t, i, (int32_t)1) : CP(0x004e4db8);
}
static void __fastcall CarViewer3D_UpdateCar_c(CarViewer3D* self, Edx) {
    char car[0x20], mod[0x50], tabn[0x50];
    const int32_t c = self->car;
    if (c >= 0 && self->count() > c) {
        const char* n = self->name_of(self->car);
        game_sprintf(car, CP(0x004fa510), n);
        game_sprintf(mod, CP(0x004fa518), n, (int32_t)0);
        if (ccall<int>(F_stricmp, (const char*)car, (const char*)self->set) != 0) {
            tcall<void>(F_CarViewer3D_SetModel, self, (const char*)0);
            if (void* t = self->tab) {
                ccall<void>(F_StringTableForget, t);
                self->tab = 0;
            }
            if (*(volatile char*)self->set != 0) ccall<void>(F_ResourceSetUnload, (const char*)self->set);
            ccall<void>(F_ResourceSetMustLoad, (const char*)car);
            tcall<void>(F_CarViewer3D_SetModel, self, (const char*)mod);
            game_sprintf(tabn, CP(0x004fa524), n);
            self->tab = ccall<void*>(F_StringTableGet, (const char*)tabn);
            crt_strcpy(self->set, car);
        } else {
            tcall<void>(F_CarViewer3D_SetModel, self, (const char*)mod);
        }
        for (int32_t i = 0; i < 5; i++) st_f(&self->stat[i], ccall<double>(F_atof, tab_entry(self, i + 1)));
        // FIX: the .tab's text (entry 6, shown in the details) went into 0x20 bytes unbounded, over the stats and the car
        // list's callbacks after it: it keeps its first 31 characters. A text that fits is copied as before.
        {
            const char* e6 = tab_entry(self, 6);
            if (VP_FIX) ui_copy_bounded(self->text, e6, sizeof self->text);
            else crt_strcpy(self->text, e6);
        }
        self->istat = ccall<int32_t>(F_atoi, tab_entry(self, 7));
        for (int32_t i = 0; i < 5; i++) st_f(&self->stat2[i], ccall<double>(F_atof, tab_entry(self, i + 8)));
    } else {
        tcall<void>(F_CarViewer3D_SetModel, self, (const char*)0);
    }
    tcall<void>(F_UICC_Dirty, self);
}
PORT_FN(0x0049ca80, "CarViewer3D::UpdateCar", CarViewer3D_UpdateCar_c, fp_loads0<CarViewer3D>)

// Next / Prev: the paint job, 0..7 (C's %)
static void __fastcall CarViewer3D_Next_c(CarViewer3D* self, Edx) {
    const int32_t p = tcall<int32_t>(F_CarViewer3D_GetPaintJob, self);
    tcall<void>(F_CarViewer3D_SetPaintJob, self, (int32_t)((p + 1) % 8));
}
PORT_FN(0x0049cdd0, "CarViewer3D::Next", CarViewer3D_Next_c, fp_loads0<CarViewer3D>)
static void __fastcall CarViewer3D_SetCar_c(CarViewer3D* self, Edx, int32_t c) {
    self->car = c;
    tcall<void>(F_CarViewer3D_UpdateCar, self);
}
PORT_FN(0x0049cdf0, "CarViewer3D::SetCar", CarViewer3D_SetCar_c, fp_loads1<CarViewer3D>)
static void __fastcall CarViewer3D_SetPaintJob_c(CarViewer3D* self, Edx, int32_t p) {
    self->paint = p;
    OptionsSet(UI_GP(const char, S_CAR_SECTION), CP(0x004fa52c), p);
    tcall<void>(F_CarViewer3D_UpdateCar, self);
}
PORT_FN(0x0049ce00, "CarViewer3D::SetPaintJob", CarViewer3D_SetPaintJob_c, fp_loads1<CarViewer3D>)
static int32_t __fastcall CarViewer3D_GetPaintJob_c(CarViewer3D* self, Edx) { return self->paint; }
PORT_FN(0x0049ce30, "CarViewer3D::GetPaintJob", CarViewer3D_GetPaintJob_c, fp_pure0<CarViewer3D>)
static void __fastcall CarViewer3D_Prev_c(CarViewer3D* self, Edx) {
    const int32_t p = tcall<int32_t>(F_CarViewer3D_GetPaintJob, self);
    tcall<void>(F_CarViewer3D_SetPaintJob, self, (int32_t)((p + 7) % 8));
}
PORT_FN(0x0049ce40, "CarViewer3D::Prev", CarViewer3D_Prev_c, fp_loads0<CarViewer3D>)
static const char* __fastcall CarViewer3D_GetFriendlyName_c(CarViewer3D* self, Edx) {
    void* t = self->tab;
    if (t) return ccall<const char*>(F_StringTableGetEntry, t, (int32_t)0, (int32_t)1);
    return CP(0x004e4db8);
}
PORT_FN(0x0049ce70, "CarViewer3D::GetFriendlyName", CarViewer3D_GetFriendlyName_c, fp_none0<CarViewer3D>)
static const char* __fastcall CarViewer3D_GetName_c(CarViewer3D* self, Edx) {
    const int32_t c = self->car;
    if (c >= 0 && self->count() > c) return self->name_of(self->car);
    return CP(0x004fa53c);
}
static void fp_CarViewer3D_GetName(Footprint& f, CarViewer3D* self, Edx) {
    if (!car_list_is_root(self)) f.replay_only = "calls its car list's callbacks";
}
PORT_FN(0x0049ce90, "CarViewer3D::GetName", CarViewer3D_GetName_c, fp_CarViewer3D_GetName)

// CarChooser::update_car: the car's friendly name to the caller; the stats as text (the locale's units)
static void __fastcall CarChooser_update_car_c(CarChooser* self, Edx) {
    {
        const char* s = tcall<const char*>(F_CarViewer3D_GetFriendlyName, &self->viewer);
        const uint32_t n = crt_strlen(s) + 1;
        crt_copy(self->name, s, n);
    }
    CarViewer3D* v = &self->viewer;
    // FIX: the stats were formatted into their texts of 16 and 32 bytes unbounded, so a big stat (a mod car's .tab) or a
    // long translation of a unit ran each into the next, and the last (+0xd8) into the viewer's vtable (the next call
    // through it crashed): each is formatted in a buffer of the rewrite's and keeps what fits its text, 15 or 31
    // characters (see fix_format). A text that fits is the same bytes.
    char t[0x400], c1[0x100], c2[0x100];
    xl_once(0x0057b8a8, 1, 0x0057b8c0, 0x004fa540, 0x0049d240);
    xl_once(0x0057b8a8, 2, 0x0057b998, 0x004fa560, 0x0049d230);
    xl_once(0x0057b8a8, 4, 0x0057bc28, 0x004fa584, 0x0049d220);
    xl_once(0x0057b8a8, 8, 0x0057bc48, 0x004fa5ac, 0x0049d210);
    {
        const char* u = xlate(0x0057b8c0);
        fix_format(self->s18, sizeof self->s18, t, CP(0x004fa5c8), D(v->stat[0]), fix_cut(u, c1, 0xff));
        ccall<void>(F_LocaleConvertNumeric, (char*)self->s18);
    }
    {
        const char* u = xlate(0x0057b8c0);
        fix_format(self->s28, sizeof self->s28, t, CP(0x004fa5d4), D(v->stat[1]), fix_cut(u, c1, 0xff));
        ccall<void>(F_LocaleConvertNumeric, (char*)self->s28);
    }
    {
        const char* u = xlate(0x0057b8c0);
        fix_format(self->s38, sizeof self->s38, t, CP(0x004fa5e0), D(v->stat[2]), fix_cut(u, c1, 0xff));
        ccall<void>(F_LocaleConvertNumeric, (char*)self->s38);
    }
    {
        const uint8_t* L = locale();
        const double s = D(*(const volatile float*)(L + 0x1c)) * D(v->stat[3]);
        fix_format(self->s48, sizeof self->s48, t, CP(0x004fa5ec), s, fix_cut(*(const char* const volatile*)(L + 0x18), c1, 0xff));
    }
    {
        const uint8_t* L = locale();
        const double s = D(*(const volatile float*)(L + 0x1c)) * D(v->stat[4]);
        fix_format(self->s58, sizeof self->s58, t, CP(0x004fa5f8), s, fix_cut(*(const char* const volatile*)(L + 0x18), c1, 0xff));
    }
    if (VP_FIX) ui_copy_bounded(self->s68, v->text, sizeof self->s68);   // FIX: (above; UpdateCar keeps it to 31 now)
    else crt_strcpy(self->s68, v->text);
    {
        const uint8_t* L = locale();
        const double p = D(v->istat) * D(*(const volatile float*)(L + 0x2c));
        const char* u = *(const char* const volatile*)(L + 0x28);
        fix_format(self->s88, sizeof self->s88, t, CP(0x004fa604), x87_ftol(p), fix_cut(u, c1, 0xff));
    }
    {
        xlate(0x0057bc28);
        xlate(0x0057b998);
        fix_format(self->s98, sizeof self->s98, t, CP(0x004fa60c), D(v->stat2[0]), fix_cut(UI_GP(const char, 0x0057b99c), c1, 0xff),
                   D(v->stat2[1]), fix_cut(UI_GP(const char, 0x0057bc2c), c2, 0xff));
    }
    {
        xlate(0x0057bc28);
        xlate(0x0057bc48);
        fix_format(self->sb8, sizeof self->sb8, t, CP(0x004fa620), D(v->stat2[2]), fix_cut(UI_GP(const char, 0x0057bc4c), c1, 0xff),
                   D(v->stat2[3]), fix_cut(UI_GP(const char, 0x0057bc2c), c2, 0xff));
    }
    {
        const char* u = xlate(0x0057bc28);
        fix_format(self->sd8, sizeof self->sd8, t, CP(0x004fa634), D(v->stat2[4]), fix_cut(u, c1, 0xff));
    }
}
static void fp_CarChooser_update_car(Footprint& f, CarChooser* self, Edx) {
    if (!xl_built(0x0057b8a8, 0xf)) { f.replay_only = "the first call builds its Xlators (atexit)"; return; }
    f.add(self, sizeof *self, "this");
    const char* s = tcall<const char*>(F_CarViewer3D_GetFriendlyName, &self->viewer);
    if (s && self->name) f.add(self->name, crt_strlen(s) + 1, "the chooser's car name");
    FP_XL(f, 0x0057b8c0); FP_XL(f, 0x0057b998); FP_XL(f, 0x0057bc28); FP_XL(f, 0x0057bc48);
}
PORT_FN(0x0049cec0, "CarChooser::update_car", CarChooser_update_car_c, fp_CarChooser_update_car)

static CarChooser* __fastcall CarChooser_ctor_c(CarChooser* self, Edx, uint8_t a, char* name, int32_t car, uint8_t b,
                                               CarCountFn count, CarNameFn name_of) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    tcall<CarViewer3D*>(F_CarViewer3D_ctor, &self->viewer, car, count, name_of);
    self->vtbl = (const void*)(uintptr_t)VT_CarChooser;
    self->b1f9 = b;
    self->b1f8 = a;
    self->name = name;
    UI_GP(CarChooser, S_CAR_CHOOSER) = self;
    tcall<void>(F_CarChooser_update_car, self);
    return self;
}
static void fp_CarChooser_ctor(Footprint& f, CarChooser*, Edx, uint8_t, char*, int32_t, uint8_t, CarCountFn, CarNameFn) {
    f.replay_only = "builds a CarViewer3D (loads the car, reads options)";
}
PORT_FN(0x0049d250, "CarChooser::CarChooser", CarChooser_ctor_c, fp_CarChooser_ctor)

static void __fastcall CarChooser_dtor_c(CarChooser* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_CarChooser;
    UI_GP(CarChooser, S_CAR_CHOOSER) = 0;
    tcall<void>(F_CarViewer3D_dtor, &self->viewer);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
PORT_FN(0x0049d2c0, "CarChooser::~CarChooser", CarChooser_dtor_c, fp_frees0<CarChooser>)

// paint: the paint shop on this car and paint job; then the paint job again (reloads the car)
static void __fastcall CarChooser_paint_c(CarChooser* self, Edx) {
    CarViewer3D* v = &self->viewer;
    const int32_t p = tcall<int32_t>(F_CarViewer3D_GetPaintJob, v);
    ccall<void>(F_PaintKitDo, tcall<const char*>(F_CarViewer3D_GetName, v), p);
    tcall<void>(F_CarViewer3D_SetPaintJob, v, tcall<int32_t>(F_CarViewer3D_GetPaintJob, v));
}
PORT_FN(0x0049d2f0, "CarChooser::paint", CarChooser_paint_c, fp_modal0<CarChooser>)

// details: the car's dialog -- its stats against the locale's scale
static void __fastcall CarChooser_details_c(CarChooser* self, Edx) {
    UIDialogItem it[23];
    xl_once(0x0057bc78, 0x01, 0x0057bc38, 0x004fa640, 0x0049dde0);
    xl_once(0x0057bc78, 0x02, 0x0057b918, 0x004fa658, 0x0049ddd0);
    xl_once(0x0057bc78, 0x04, 0x0057bc18, 0x004fa670, 0x0049ddc0);
    xl_once(0x0057bc78, 0x08, 0x0057bc88, 0x004fa688, 0x0049ddb0);
    xl_once(0x0057bc78, 0x10, 0x0057b938, 0x004fa69c, 0x0049dda0);
    xl_once(0x0057bc78, 0x20, 0x0057b978, 0x004fa6b4, 0x0049dd90);
    xl_once(0x0057bc78, 0x40, 0x0057b8d0, 0x004fa6cc, 0x0049dd80);
    xl_once(0x0057bc78, 0x80, 0x0057bc60, 0x004fa6e4, 0x0049dd70);
    xl_once(0x0057bc54, 0x01, 0x0057b928, 0x004fa6fc, 0x0049dd60);
    xl_once(0x0057bc54, 0x02, 0x0057bc98, 0x004fa714, 0x0049dd50);
    xl_once(0x0057bc54, 0x04, 0x0057b8b0, 0x004fa730, 0x0049dd40);
    xl_once(0x0057bc54, 0x08, 0x0057b908, 0x004fa748, 0x0049dd30);
    {
        int32_t top = 0x3c;
        const uint8_t metric = *(const volatile uint8_t*)(locale() + 0x38);
        if (metric) top = x87_ftol(D(*(const volatile float*)(locale() + 0x1c)) * D(bits_f(0x41d55555)));
        int32_t top2 = 0x64;
        if (metric) top2 = x87_ftol(D(*(const volatile float*)(locale() + 0x1c)) * D(bits_f(0x4231c71c)));
        game_sprintf((char*)(uintptr_t)0x0057bbf8, CP(0x004fa750), top, *(const char* const volatile*)(locale() + 0x18));
        game_sprintf((char*)(uintptr_t)0x0057b948, CP(0x004fa758), top2, *(const char* const volatile*)(locale() + 0x18));
        game_sprintf((char*)(uintptr_t)0x0057b9a8, CP(0x004fa760), (const char*)self->s38, (const char*)self->s48);
    }
    item14(&it[0], 2, -1, 0x13, 0x1a2, 0, 0, xlate(0x0057b908), 0, 0, 6);
    item_ctor(&it[1], 5, 0, 0x7c, 0x60, 0, 0, xlate(0x0057bc38), 0, 0, 0x11);
    item_ctor(&it[2], 5, 0, 0x26, 0x6d, 0, 0, xlate(0x0057b918), 0, 0, 0x12);
    item_ctor(&it[3], 5, 0, 0x2d, 0x80, 0, 0, CP(0x0057bbf8), 0, 0, 0x12);
    item_ctor(&it[4], 5, 0, 0xc7, 0x80, 0, 0, self->s18, 0, 0, 0x13);
    item_ctor(&it[5], 5, 0, 0x2d, 0x91, 0, 0, CP(0x0057b948), 0, 0, 0x12);
    item_ctor(&it[6], 5, 0, 0xc7, 0x91, 0, 0, self->s28, 0, 0, 0x13);
    item_ctor(&it[7], 5, 0, 0x26, 0xa2, 0, 0, xlate(0x0057bc18), 0, 0, 0x12);
    item_ctor(&it[8], 5, 0, 0xc7, 0xb5, 0, 0, CP(0x0057b9a8), 0, 0, 0x13);
    item_ctor(&it[9], 5, 0, 0x26, 0xd7, 0, 0, xlate(0x0057bc88), 0, 0, 0x12);
    item_ctor(&it[10], 5, 0, 0xc7, 0xd7, 0, 0, self->s58, 0, 0, 0x13);
    item14(&it[11], 5, 0, 0x143, 0x88, 0, 0, xlate(0x0057b938), 0, 0, 0x11);
    item_ctor(&it[12], 5, 0, 0xe8, 0x95, 0, 0, xlate(0x0057b978), 0, 0, 0x12);
    item14(&it[13], 5, 0, 0x1a1, 0x95, 0, 0, self->s68, 0, 0, 0x14);
    item_ctor(&it[14], 5, 0, 0xe8, 0xa4, 0, 0, xlate(0x0057b8d0), 0, 0, 0x12);
    item14(&it[15], 5, 0, 0x1a1, 0xa4, 0, 0, self->s88, 0, 0, 0x14);
    item_ctor(&it[16], 5, 0, 0xe8, 0xb3, 0, 0, xlate(0x0057bc60), 0, 0, 0x12);
    item14(&it[17], 5, 0, 0x1a1, 0xb3, 0, 0, self->s98, 0, 0, 0x14);
    item14(&it[18], 5, 0, 0xe8, 0xc2, 0, 0, xlate(0x0057b928), 0, 0, 0x12);
    item_ctor(&it[19], 5, 0, 0x1a1, 0xc2, 0, 0, self->sb8, 0, 0, 0x14);
    item14(&it[20], 5, 0, 0xe8, 0xd1, 0, 0, xlate(0x0057bc98), 0, 0, 0x12);
    item_ctor(&it[21], 5, 0, 0x1a1, 0xd1, 0, 0, self->sd8, 0, 0, 0x14);
    item_end(&it[22]);
    // FIX CANDIDATE: the static dialog's title and items are set once, to this chooser's name and this call's frame; a
    // later call from a different stack depth hands UIDoDialog the first frame's memory
    if (!(UI_G8(0x0057bc54) & 0x10)) {
        UI_G8(0x0057bc54) = (uint8_t)(UI_G8(0x0057bc54) | 0x10);
        UI_GP(char, S_CAR_DIALOG) = self->name;
        UI_GU32(S_CAR_DIALOG + 0xc) = (uint32_t)(uintptr_t)&it[0];
        UI_GU32(S_CAR_DIALOG + 4) = 0x004fa768;
        UI_GU32(S_CAR_DIALOG + 8) = 0xfffffffeu;
        UI_GU32(S_CAR_DIALOG + 0x10) = 0;
    }
    const int32_t h = UI_G32(S_GX_SCREEN_H), w = UI_G32(S_GX_SCREEN_W);
    ccall<int32_t>(F_UIDoDialog, (const void*)(uintptr_t)S_CAR_DIALOG, w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
}
PORT_FN(0x0049d320, "CarChooser::details", CarChooser_details_c, fp_modal0<CarChooser>)

// CarChooser::Added: prev / next (paint jobs), the viewer, the paint button
static void __fastcall CarChooser_Added_c(CarChooser* self, Edx) {
    UIDialogItem it[5];
    xl_once(0x0057bc5c, 1, 0x0057b988, 0x004fa774, 0x0049e060);
    const int32_t x = self->x, y = self->y;
    item14(&it[0], 3, 1, x - 2, y + 0x9c, 0, 0, CP(0x004fa788), 0, (const void*)(uintptr_t)F_prev_car_cb, 0);
    item14(&it[1], 3, 2, x + 0x88, y + 0x9c, 0, 0, CP(0x004fa794), 0, (const void*)(uintptr_t)F_next_car_cb, 0);
    item14(&it[2], 0x17, 0, x, y, 0xb3, 0x8d, CP(0x004e4db8), 0, (const void*)&self->viewer, 0);
    const char* t = xlate(0x0057b988);
    item14(&it[3], 2, 3, self->x + 0x2a, self->y + 0x98, 0, 0, t, 0, (const void*)(uintptr_t)F_car_details_cb, 1);
    item_end(&it[4]);
    tcall<void>(F_UICC_AddItems, self, &it[0]);
}
PORT_FN(0x0049ddf0, "CarChooser::Added", CarChooser_Added_c, fp_added0<CarChooser>)

static uint8_t __cdecl next_car_cb_c(int32_t) {
    CarChooser* s = UI_GP(CarChooser, S_CAR_CHOOSER);
    tcall<void>(F_CarViewer3D_Next, &s->viewer);
    tcall<void>(F_CarChooser_update_car, s);
    return 0;
}
PORT_FN(0x0049e010, "next_car_cb", next_car_cb_c, fp_cb_modal)
static uint8_t __cdecl prev_car_cb_c(int32_t) {
    CarChooser* s = UI_GP(CarChooser, S_CAR_CHOOSER);
    tcall<void>(F_CarViewer3D_Prev, &s->viewer);
    tcall<void>(F_CarChooser_update_car, s);
    return 0;
}
PORT_FN(0x0049e030, "prev_car_cb", prev_car_cb_c, fp_cb_modal)
static uint8_t __cdecl car_details_cb_c(int32_t) {
    tcall<void>(F_CarChooser_paint, UI_GP(CarChooser, S_CAR_CHOOSER));
    return 0;
}
PORT_FN(0x0049e050, "car_details_cb", car_details_cb_c, fp_cb_modal)

static void* __fastcall CarViewer3D_vdd_c(CarViewer3D* self, Edx, uint32_t flags) {
    tcall<void>(F_CarViewer3D_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0049e070, "CarViewer3D::scalar deleting destructor", CarViewer3D_vdd_c, fp_dtor_flags<CarViewer3D>)
static void __fastcall CarChooser_Draw_c(CarChooser*, Edx, gxCanvas*) {}
PORT_FN(0x0049e090, "CarChooser::Draw", CarChooser_Draw_c, fp_pure_canvas<CarChooser>)
static void* __fastcall CarChooser_sdd_c(CarChooser* self, Edx, uint32_t flags) {
    tcall<void>(F_CarChooser_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0049e0a0, "CarChooser::vector deleting destructor", CarChooser_sdd_c, fp_dtor_flags<CarChooser>)

// =========================================================================================================================
// oppview.obj
// =========================================================================================================================
enum : uint32_t { S_OPP_SECTION = 0x004df190 };   // char const* "GLOBAL" (oppview's option section)

static OpponentViewer* __fastcall OpponentViewer_ctor_c(OpponentViewer* self, Edx, GameOptions* opts, volatile int32_t* out) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_OpponentViewer;
    self->widget = 0;
    self->created = 0;
    self->type = 0;
    self->opts = opts;
    UI_GP(OpponentViewer, S_OPP_VIEWER) = self;
    self->out = out;
    self->ghost = 0;
    *(volatile char*)self->car = 0;
    *(volatile char*)self->set = 0;
    self->model = 0;
    self->frame_stamp = ccall<void*>(F_gxGetStamp, CP(0x004fa7e4));
    self->clock = ccall<void*>(F_gxGetStamp, CP(0x004fa7f0));
    self->pack = ccall<void*>(F_gxGetStamp, CP(0x004fa7fc));
    return self;
}
static void fp_OpponentViewer_ctor(Footprint& f, OpponentViewer*, Edx, GameOptions*, volatile int32_t*) { f.replay_only = "loads stamps"; }
PORT_FN(0x0049e2e0, "OpponentViewer::OpponentViewer", OpponentViewer_ctor_c, fp_OpponentViewer_ctor)

static void __fastcall OpponentViewer_dtor_c(OpponentViewer* self, Edx) {
    void* fr = self->frame_stamp;
    self->vtbl = (const void*)(uintptr_t)VT_OpponentViewer;
    ccall<void>(F_gxForgetStamp, fr);
    ccall<void>(F_gxForgetStamp, (void*)self->clock);
    ccall<void>(F_gxForgetStamp, (void*)self->pack);
    UI_GP(OpponentViewer, S_OPP_VIEWER) = 0;
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
PORT_FN(0x0049e370, "OpponentViewer::~OpponentViewer", OpponentViewer_dtor_c, fp_frees0<OpponentViewer>)

// set_ghost_type: the race's ghost car type from the opponent (the pack: the saved "mgc_type"; the clock: none; a ghost
// car: the saved "sgc_type", 1 if none)
static void __fastcall OpponentViewer_set_ghost_type_c(OpponentViewer* self, Edx) {
    switch (self->type) {
    case 0: {
        const int32_t g = self->saved[0];
        self->opts->ghost = g;
        return;
    }
    case 1: self->opts->ghost = 0; return;
    case 2: {
        const int32_t g = self->saved[1];
        GameOptions* o = self->opts;
        if (g == 0) o->ghost = 1;
        else o->ghost = g;
        return;
    }
    }
}
static void fp_OpponentViewer_set_ghost_type(Footprint& f, OpponentViewer* self, Edx) {
    if (self->opts) f.add((void*)&self->opts->ghost, 4, "the race's ghost car type");
}
PORT_FN(0x0049e3c0, "OpponentViewer::set_ghost_type", OpponentViewer_set_ghost_type_c, fp_OpponentViewer_set_ghost_type)

// Create: the options, the camera and the stand, the notifications, the opponent
static void __fastcall OpponentViewer_Create_c(OpponentViewer* self, Edx) {
    self->saved[0] = 1;
    self->created = 1;
    self->saved[1] = 1;
    const char* sec = UI_GP(const char, S_OPP_SECTION);
    OptionsGet(sec, CP(0x004fa808), &self->type);
    OptionsGet(sec, CP(0x004fa814), &self->saved[0]);
    OptionsGet(sec, CP(0x004fa820), &self->saved[1]);
    tcall<void>(F_OpponentViewer_set_ghost_type, self);
    volatile uint32_t* c = (volatile uint32_t*)&self->camera;       // +0x8c .. +0xe8: the camera, the body
    static const uint8_t order[24] = {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    for (int i = 0; i < 24; i++) {
        const int k = order[i];
        c[k] = (k == 0 || k == 4 || k == 8 || k == 12 || k == 16 || k == 20) ? 0x3f800000u : 0u;
    }
    *(volatile uint32_t*)&self->body.p[1] = 0xbf000000u;
    *(volatile uint32_t*)&self->body.p[2] = 0x40a00000u;
    tcall<void>(F_UICC_AddNotification, self, (int32_t)1, (const void*)&self->opts->ghost, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->type, (uint32_t)4);
    tcall<void>(F_OpponentViewer_UpdateOpponent, self);
}
PORT_FN(0x0049e410, "OpponentViewer::Create", OpponentViewer_Create_c, fp_notes0<OpponentViewer>)

// Added: prev / next
static void __fastcall OpponentViewer_Added_c(OpponentViewer* self, Edx) {
    UIDialogItem it[3];
    const int32_t x = self->x, y = self->y;
    item14(&it[0], 3, 1, x - 2, y + 0x9c, 0, 0, CP(0x004fa82c), 0, (const void*)(uintptr_t)F_opp_prev_cb, 0);
    item14(&it[1], 3, 2, x + 0x88, y + 0x9c, 0, 0, CP(0x004fa838), 0, (const void*)(uintptr_t)F_opp_next_cb, 0);
    item_end(&it[2]);
    tcall<void>(F_UICC_AddItems, self, &it[0]);
}
PORT_FN(0x0049e560, "OpponentViewer::Added", OpponentViewer_Added_c, fp_added0<OpponentViewer>)

// Destroy: the model freed; with the pack chosen the caller's flag is set; the options saved
static void __fastcall OpponentViewer_Destroy_c(OpponentViewer* self, Edx) {
    if (const int32_t m = self->model) ccall<void>(F_mrModelUnload, m);
    self->model = 0;
    if (*(volatile char*)self->set != 0) {
        ccall<void>(F_ResourceSetUnload, (const char*)self->set);
        *(volatile char*)self->set = 0;
    }
    if (self->opts->field == 0) *self->out = 1;
    const char* sec = UI_GP(const char, S_OPP_SECTION);
    OptionsSet(sec, CP(0x004fa844), self->type);
    OptionsSet(sec, CP(0x004fa850), self->saved[0]);
    OptionsSet(sec, CP(0x004fa85c), self->saved[1]);
    self->created = 0;
}
PORT_FN(0x0049e640, "OpponentViewer::Destroy", OpponentViewer_Destroy_c, fp_frees0<OpponentViewer>)

// Callback: the opponent changed (id 0) -- the new one; the ghost type changed (id 1) -- kept for the opponent
static void __fastcall OpponentViewer_Callback_c(OpponentViewer* self, Edx, int32_t id, const void*) {
    if (id == 0) tcall<void>(F_OpponentViewer_UpdateOpponent, self);
    if (id != 1) return;
    const int32_t t = self->type;
    if (t == 0) { self->saved[0] = self->opts->ghost; return; }
    if (t == 2) self->saved[1] = self->opts->ghost;
}
static void fp_OpponentViewer_Callback(Footprint& f, OpponentViewer* self, Edx, int32_t id, const void*) {
    if (id == 0) { f.replay_only = "UpdateOpponent loads the model"; return; }
    f.add(self, sizeof *self, "this");
}
PORT_FN(0x0049e6e0, "OpponentViewer::Callback", OpponentViewer_Callback_c, fp_OpponentViewer_Callback)

// Draw: the clock (centred) or the pack; the title bar, the label, the frame
static void __fastcall OpponentViewer_Draw_c(OpponentViewer* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    const int32_t t = self->type;
    if (t == 1) {
        void* s = self->clock;
        const int32_t h = self->h;
        const int32_t sy = self->y + (h - ccall<int32_t>(F_gxStampHeight, s)) / 2 + 0xa;
        const int32_t w = self->w;
        const int32_t sx = self->x + (w - ccall<int32_t>(F_gxStampWidth, s)) / 2;
        ccall<void>(F_gxDrawStamp, s, sx, sy, (int32_t)0, (const void*)0);
    } else if (t == 0) {
        ccall<void>(F_gxDrawStamp, (void*)self->pack, (int32_t)self->x, (int32_t)(self->y + 0x17), (int32_t)0, (const void*)0);
    }
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t col = UI_GU32(0x0057bcd8);
        ccall<void>(F_gxRect, x, y, (int32_t)(self->w + x), y + 0x13, col);
    }
    {
        const char* name = tcall<const char*>(F_OpponentViewer_GetFriendlyName, self);
        const int32_t ty = self->y + 9;
        const int32_t tx = self->x + self->w / 2;
        ccall<void>(F_UIStyleDraw, (int32_t)0x11, tx, ty, name, (uint32_t)0);
    }
    ccall<void>(F_gxDrawStamp, (void*)self->frame_stamp, (int32_t)self->x, (int32_t)self->y, (int32_t)0, (const void*)0);
}
static void fp_OpponentViewer_Draw(Footprint& f, OpponentViewer*, Edx, gxCanvas* c) { fp_draw_canvas(f, c); }
PORT_FN(0x0049e730, "OpponentViewer::Draw", OpponentViewer_Draw_c, fp_OpponentViewer_Draw)

// Draw3D: the ghost car turned on its stand (half transparent), drawn again 2.2 ahead and 4.4 behind for the pack
static void __fastcall OpponentViewer_Draw3D_c(OpponentViewer* self, Edx) {
    volatile float M1[9], M2[9];
    {
        const int32_t hh = self->h, ww = self->w;
        ccall<void>(F_mrSetView, (int32_t)(self->x + 6), (int32_t)(self->y + 6), ww - 0xc, hh - 0xc, (uint8_t)0);
    }
    ccall<void>(F_mrSetProjection, (uint32_t)0x3e4ccccd, (uint32_t)0x447a0000, (uint32_t)0x3f9c61aa);
    ccall<void>(F_mrSetCamera, (void*)&self->camera);
    MvFrame* F = &self->body;
    ccall<void>(F_mrEnable, (int32_t)0x10);
    ccall<void>(F_mrEnable, (int32_t)1);
    {
        volatile uint32_t* b = (volatile uint32_t*)F;
        for (int i = 0; i < 12; i++) b[i] = (i == 0 || i == 4 || i == 8) ? 0x3f800000u : 0u;
    }
    ccall<void>(F_MatrixMakeYaw, (void*)M1, (uint32_t)0x406a9280);
    ccall<void>(F_MatrixMakePitch, (void*)M2, (uint32_t)0);
    ccall<void>(F_MatrixConcat, (void*)M1, (const void*)M2, (const void*)M1);
    ccall<void>(F_MatrixMakeRoll, (void*)M2, (uint32_t)0);
    ccall<void>(F_MatrixConcat, (void*)M1, (const void*)M2, (const void*)M1);
    ccall<void>(F_MatrixConcat, (void*)F, (const void*)M1, (const void*)F);
    ccall<void>(F_MatrixMakeWorldRotation, (void*)M2, (uint32_t)0, (uint32_t)0xbea0d97c, (uint32_t)0);
    ccall<void>(F_MatrixConcat, (void*)F, (const void*)F, (const void*)M2);
    const uint8_t ghost = self->ghost;
    st_f(&F->p[1], D(F->p[1]) - D(bits_f(0x3f000000)));
    st_f(&F->p[2], D(F->p[2]) + D(bits_f(0x40800000)));
    if (ghost != 0) ccall<void>(F_mrModelSetAlpha, (uint32_t)0x3f000000);
    if (const int32_t m = self->model) {
        ccall<void>(F_mrModelDraw, m, (const void*)F);
        if (self->opts->field == 1) {
            volatile float in[3], out[3];
            *(volatile uint32_t*)&in[0] = 0x400ccccdu;
            *(volatile uint32_t*)&in[1] = 0;
            *(volatile uint32_t*)&in[2] = 0;
            ccall<void>(F_MatrixMulPoint, (void*)out, (const void*)in, (const void*)F);
            st_f(&F->p[0], D(F->p[0]) + D(out[0]));
            st_f(&F->p[1], D(F->p[1]) + D(out[1]));
            st_f(&F->p[2], D(F->p[2]) + D(out[2]));
            ccall<void>(F_mrModelDraw, (int32_t)self->model, (const void*)F);
            *(volatile uint32_t*)&in[0] = 0xc08ccccdu;
            *(volatile uint32_t*)&in[1] = 0;
            *(volatile uint32_t*)&in[2] = 0;
            ccall<void>(F_MatrixMulPoint, (void*)out, (const void*)in, (const void*)F);
            st_f(&F->p[0], D(F->p[0]) + D(out[0]));
            st_f(&F->p[1], D(F->p[1]) + D(out[1]));
            st_f(&F->p[2], D(F->p[2]) + D(out[2]));
            ccall<void>(F_mrModelDraw, (int32_t)self->model, (const void*)F);
        }
    }
    if (self->ghost != 0) ccall<void>(F_mrModelClearAlpha);
}
PORT_FN(0x0049e820, "OpponentViewer::Draw3D", OpponentViewer_Draw3D_c, fp_draw3d0<OpponentViewer>)

// SetCar: the ghost car's name; the opponent again once created
static void __fastcall OpponentViewer_SetCar_c(OpponentViewer* self, Edx, const char* name) {
    // FIX: the name went into 0x20 bytes unbounded, over the label and the stamps after it: it keeps its first 31
    // characters (MenuDoRaceSetup's is the constant "viper": a name that fits is copied as before)
    if (VP_FIX) ui_copy_bounded(self->car, name, sizeof self->car);
    else crt_strcpy(self->car, name);
    if (self->created != 0) tcall<void>(F_OpponentViewer_UpdateOpponent, self);
}
static void fp_OpponentViewer_SetCar(Footprint& f, OpponentViewer* self, Edx, const char* name) {
    if (self->created != 0) { f.replay_only = "UpdateOpponent loads the model"; return; }
    f.add(self, sizeof *self, "this");
    if (name) f.add(self->car, crt_strlen(name) + 1, "the name (unbounded)");
}
PORT_FN(0x0049eaa0, "OpponentViewer::SetCar", OpponentViewer_SetCar_c, fp_OpponentViewer_SetCar)

// SetModel: the old model and its resource set freed; "<name>" loaded with its paint remapped, its set ("<car>.car")
static void __fastcall OpponentViewer_SetModel_c(OpponentViewer* self, Edx, const char* name) {
    char base[0x20], tex[VP_FIX ? 0x40 : 0x20];                    // FIX: (below) tex has room for "~<base>.tex"
    if (const int32_t m = self->model) ccall<void>(F_mrModelUnload, m);
    char* set = self->set;
    self->model = 0;
    if (*(volatile char*)set != 0) {
        ccall<void>(F_ResourceSetUnload, (const char*)set);
        *(volatile char*)set = 0;
    }
    // FIX: the name went into the 0x20-byte set name unbounded, was cut at the character before its first '.' ("<car>1.mod"
    // -> "<car>"; with no '.' that wrote to address -1, a crash; with a '.' first, before the object's field), and had
    // ".car" put after it: a name of 32 or more characters, or one that's 28 or more cut, overran the field into the
    // car's name after it, and one of 27 overran the 0x20-byte texture name "~<base>.tex" onto the return address. The
    // resource set must be named in full to load, so such a name shows no car, as for none (nothing loaded); a name with
    // no '.' (or one first) isn't cut; the texture name has 0x40 bytes and goes into the remap entry as CarViewer3D's
    // (it overran the entry into the viewer's colour from 24 characters on). A name that fits is the same.
    bool fits = name && *(const volatile char*)name != 0;
    if (VP_FIX && fits) fits = ui_strnlen(name, sizeof self->set - 1) <= sizeof self->set - 1;
    if (fits) {
        crt_strcpy(set, name);
        {
            char* const dot = ccall<char*>(F_strchr, (const char*)set, (int)0x2e);
            if (!(VP_FIX && (dot == 0 || dot == set))) *(volatile char*)(dot - 1) = 0;
        }
        if (VP_FIX && crt_strlen(set) > sizeof self->set - 5) {
            *(volatile char*)set = 0;
            fits = false;
        }
    }
    if (fits) {
        crt_strcpy(base, set);
        char* e = set + crt_strlen(set);
        *(volatile uint32_t*)e = UI_GU32(0x004fa868);                 // ".car" and its terminator
        *(volatile uint8_t*)(e + 4) = UI_G8(0x004fa86c);
        ccall<void>(F_ResourceSetMustLoad, (const char*)set);
        ccall<void>(F_WorldGetCarTexture, (char*)tex, (const char*)base, (int32_t)-1);
        game_sprintf((char*)(uintptr_t)S_REMAP_OPP, CP(0x004fa870), (const char*)base);
        if (VP_FIX) ui_copy_bounded((char*)(uintptr_t)(S_REMAP_OPP + 0x10), tex, 0x18);
        else crt_strcpy((char*)(uintptr_t)(S_REMAP_OPP + 0x10), tex);
        UI_GU32(S_REMAP_OPP + 0x20) = 0;
        UI_GU32(S_REMAP_OPP + 0x24) = 0;
        const int32_t m = ccall<int32_t>(F_mrModelLoad, name);
        self->model = m;
        ccall<void>(F_mrModelRemapTextures, m, (void*)(uintptr_t)S_REMAP_OPP, (int32_t)1);
    }
    tcall<void>(F_UICC_Dirty, self);
}
PORT_FN(0x0049eae0, "OpponentViewer::SetModel", OpponentViewer_SetModel_c, fp_loads_s<OpponentViewer>)

// UpdateOpponent: the label, the race's field, the model (a ghost car's is "<car>1.mod")
static void __fastcall OpponentViewer_UpdateOpponent_c(OpponentViewer* self, Edx) {
    char buf[VP_FIX ? 0x40 : 0x20];                                // FIX: (below)
    xl_once(0x0057bd0c, 1, 0x0057bcf0, 0x004fa878, 0x0049ee60);
    xl_once(0x0057bd0c, 2, 0x0057bd20, 0x004fa890, 0x0049ee50);
    xl_once(0x0057bd0c, 4, 0x0057bd10, 0x004fa8a8, 0x0049ee40);
    if (!(UI_G8(0x0057bd0c) & 8)) {
        UI_G8(0x0057bd0c) = (uint8_t)(UI_G8(0x0057bd0c) | 8);
        const char* a = xlate(0x0057bcf0);
        UI_GP(const char, S_OPP_LABELS) = a;
        UI_GP(const char, S_OPP_LABELS + 4) = xlate(0x0057bd10);
        UI_GP(const char, S_OPP_LABELS + 8) = xlate(0x0057bd20);
    }
    // FIX: the car's name and "1.mod" went into 0x20 bytes unbounded, so a name of 27 or more characters overran the stack:
    // the buffer has 0x40 bytes, and the name is read to its field's 31 characters at most, so "<car>1.mod" fits whole
    // (SetModel then shows no car for a name too long for its set). A name that fits is the same.
    if (VP_FIX) ui_copy_bounded(buf, self->car, sizeof self->car);
    else crt_strcpy(buf, self->car);
    if (buf[0] != 0) {
        char* e = buf + crt_strlen(buf);
        const uint16_t tail = UI_G16(0x004fa8c4);
        *(volatile uint32_t*)e = UI_GU32(0x004fa8c0);                 // "1.mod" and its terminator
        *(volatile uint16_t*)(e + 4) = tail;
    }
    self->ghost = 0;
    // FIX CANDIDATE: the opponent type (option "opponent") indexes the three labels unchecked
    // FIX: the label (a translation) went into 0x23 bytes unbounded, over the stamps' pointers after it (freed and drawn
    // later: a crash): it keeps its first 34 characters. A label that fits is copied as before.
    {
        const char* lbl = UI_GP(const char, S_OPP_LABELS + (uint32_t)self->type * 4u);
        if (VP_FIX) ui_copy_bounded(self->label, lbl, sizeof self->label);
        else crt_strcpy(self->label, lbl);
    }
    switch (self->type) {
    case 0:
        self->opts->field = 1;
        tcall<void>(F_OpponentViewer_SetModel, self, (const char*)0);
        break;
    case 1:
        self->opts->field = 0;
        tcall<void>(F_OpponentViewer_SetModel, self, (const char*)0);
        break;
    case 2:
        self->opts->field = 0;
        self->ghost = 1;
        tcall<void>(F_OpponentViewer_SetModel, self, (const char*)buf);
        break;
    default: UI_LogReport(CP(0x004fa8c8), (int32_t)self->type); break;
    }
    tcall<void>(F_OpponentViewer_set_ghost_type, self);
}
PORT_FN(0x0049ec50, "OpponentViewer::UpdateOpponent", OpponentViewer_UpdateOpponent_c, fp_loads0<OpponentViewer>)

static const char* __fastcall OpponentViewer_GetFriendlyName_c(OpponentViewer* self, Edx) { return self->label; }
PORT_FN(0x0049ee70, "OpponentViewer::GetFriendlyName", OpponentViewer_GetFriendlyName_c, fp_pure0<OpponentViewer>)
static void __fastcall OpponentViewer_Next_c(OpponentViewer* self, Edx) {
    const int32_t t = self->type + 1;
    self->type = t;
    if (!(t < 3)) self->type = 0;
}
static void fp_OpponentViewer_self(Footprint& f, OpponentViewer* self, Edx) { f.add(self, sizeof *self, "this"); }
PORT_FN(0x0049ee80, "OpponentViewer::Next", OpponentViewer_Next_c, fp_OpponentViewer_self)
static void __fastcall OpponentViewer_Prev_c(OpponentViewer* self, Edx) {
    const int32_t t = self->type - 1;
    self->type = t;
    if (t < 0) self->type = 2;
}
PORT_FN(0x0049eea0, "OpponentViewer::Prev", OpponentViewer_Prev_c, fp_OpponentViewer_self)
static uint8_t __cdecl OpponentViewer_next_cb_c(int32_t) {
    tcall<void>(F_OpponentViewer_Next, UI_GP(OpponentViewer, S_OPP_VIEWER));
    return 0;
}
static void fp_opp_cb(Footprint& f, int32_t) {
    if (OpponentViewer* v = UI_GP(OpponentViewer, S_OPP_VIEWER)) f.add(v, sizeof *v, "OpponentViewer::the one");
}
PORT_FN(0x0049eec0, "OpponentViewer::next_cb", OpponentViewer_next_cb_c, fp_opp_cb)
static uint8_t __cdecl OpponentViewer_prev_cb_c(int32_t) {
    tcall<void>(F_OpponentViewer_Prev, UI_GP(OpponentViewer, S_OPP_VIEWER));
    return 0;
}
PORT_FN(0x0049eed0, "OpponentViewer::prev_cb", OpponentViewer_prev_cb_c, fp_opp_cb)
static void* __fastcall OpponentViewer_sdd_c(OpponentViewer* self, Edx, uint32_t flags) {
    tcall<void>(F_OpponentViewer_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0049eee0, "OpponentViewer::vector deleting destructor", OpponentViewer_sdd_c, fp_dtor_flags<OpponentViewer>)

// =========================================================================================================================
// optview.obj
// =========================================================================================================================
enum : uint32_t { S_OPT_SECTION = 0x004df218 };   // char const* "GAME" (optview's option section)

static RaceOptionViewer* __fastcall RaceOptionViewer_ctor_c(RaceOptionViewer* self, Edx, volatile int32_t* ai_out,
                                                           volatile int32_t* out2, GameOptions* opts, volatile uint8_t* flag,
                                                           char* summary) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_RaceOptionViewer;
    self->event = 1;
    const char* sec = UI_GP(const char, S_OPT_SECTION);
    self->widget = 0;
    OptionsGet(sec, CP(0x004fa918), &self->event);
    self->flag = flag;
    self->out2 = out2;
    self->opts = opts;
    self->ai_out = ai_out;
    self->summary = summary;
    return self;
}
static void fp_RaceOptionViewer_ctor(Footprint& f, RaceOptionViewer*, Edx, volatile int32_t*, volatile int32_t*, GameOptions*,
                                     volatile uint8_t*, char*) {
    f.replay_only = "reads an option (OptionsGet can add it)";
}
PORT_FN(0x0049f120, "RaceOptionViewer::RaceOptionViewer", RaceOptionViewer_ctor_c, fp_RaceOptionViewer_ctor)

// Added: realism, mirror, race type and laps, the event (a group), AI strength and count, the ghost car type (a group)
static void __fastcall RaceOptionViewer_Added_c(RaceOptionViewer* self, Edx) {
    UIDialogItem it[36];
    xl_once(0x0057be78, 0x01, 0x0057bd88, 0x004fa924, 0x0049fda0);
    xl_once(0x0057be78, 0x02, 0x0057be60, 0x004fa938, 0x0049fd90);
    xl_once(0x0057be78, 0x04, 0x0057bdb0, 0x004fa950, 0x0049fd80);
    xl_once(0x0057be78, 0x08, 0x0057bdc0, 0x004fa96c, 0x0049fd70);
    xl_once(0x0057be78, 0x10, 0x0057bdf0, 0x004fa984, 0x0049fd60);
    xl_once(0x0057be78, 0x20, 0x0057bd58, 0x004fa998, 0x0049fd50);
    xl_once(0x0057be78, 0x40, 0x0057bd48, 0x004fa9b0, 0x0049fd40);
    xl_once(0x0057be78, 0x80, 0x0057bde0, 0x004fa9cc, 0x0049fd30);
    xl_once(0x0057bd74, 0x01, 0x0057bd98, 0x004fa9ec, 0x0049fd20);
    xl_once(0x0057bd74, 0x02, 0x0057be48, 0x004faa10, 0x0049fd10);
    xl_once(0x0057bd74, 0x04, 0x0057bd68, 0x004faa38, 0x0049fd00);
    xl_once(0x0057bd74, 0x08, 0x0057bdd0, 0x004faa4c, 0x0049fcf0);
    xlate(0x0057be48);
    xlate(0x0057bd98);
    xlate(0x0057bde0);
    ccall<void>(F_CreateMultiString, (char*)(uintptr_t)0x0057be00, UI_GP(const char, 0x0057bde4), UI_GP(const char, 0x0057bd9c),
                UI_GP(const char, 0x0057be4c), (const char*)0);
    item14(&it[0], 5, 0, 0x28, 0x136, 0, 0, xlate(0x0057bd88), 0, 0, 0xb);
    for (int32_t k = 0; k < 3; k++) {
        const void* d = (const void*)&self->opts->realism;
        const char* s = ccall<const char*>(F_GetRealismString, k);
        item_ctor(&it[1 + k], 0xc, 0, 0x32, 0x145 + 0xe * k, 0, 0, s, k, d, 8);
    }
    {
        const char* t = xlate(0x0057bdb0);
        item_ctor(&it[4], 0xb, 0, 0x28, 0x173, 0, 0, t, 0, (const void*)&self->opts->b24, 7);
    }
    item_ctor(&it[5], 1, 0, 0, 0, 0, 0, 0, 0, (const void*)&self->group[2], 0);
    item_ctor(&it[6], 5, 0, 0xf0, 0x136, 0, 0, xlate(0x0057bd58), 0, 0, 0xb);
    {
        const int32_t x = ccall<int32_t>(F_UIStyleWidth, (int32_t)0xb, xlate(0x0057bd58)) + 0xfa;
        item_ctor(&it[7], 5, 0, x, 0x137, 0, 0, self->text, 0, 0, 0xb);
    }
    for (int32_t k = 1; k <= 3; k++) {
        const void* d = (const void*)&self->opts->race_type;
        const char* s = ccall<const char*>(F_GetRaceTypeString, k);
        item_ctor(&it[7 + k], 0xc, 0, 0xfa, 0x145 + 0xe * (k - 1), 0, 0, s, k, d, 8);
    }
    {
        const char* t = xlate(0x0057be60);
        item_ctor(&it[11], 0xb, 0, 0xf0, 0x173, 0, 0, t, 0, (const void*)&self->opts->mirror, 7);
    }
    item_ctor(&it[12], 1, 1, 0, 0, 0, 0, 0, 0, (const void*)&self->group[2], 0);
    item_ctor(&it[13], 1, 0, 0, 0, 0, 0, 0, 0, (const void*)&self->group[3], 0);
    for (int32_t k = 0; k < 3; k++) {
        const void* d = (const void*)&self->opts->event;
        const int32_t e = 1 + 2 * k;
        const char* s = ccall<const char*>(F_GetEventString, e);
        item_ctor(&it[14 + k], 0xc, 0, 0xf0, 0x137 + 0xe * k, 0, 0, s, e, d, 8);
    }
    item_ctor(&it[17], 1, 1, 0, 0, 0, 0, 0, 0, (const void*)&self->group[3], 0);
    item_ctor(&it[18], 1, 0, 0, 0, 0, 0, 0, 0, (const void*)&self->group[0], 0);
    item_ctor(&it[19], 5, 0, 0x1c2, 0x136, 0, 0, xlate(0x0057bdf0), 0, 0, 0xb);
    {
        const void* d = (const void*)&self->opts->ai_strength;
        const char* s = ccall<const char*>(F_GetAIStrengthString, (int32_t)0);
        item_ctor(&it[20], 0xc, 0, 0x1cc, 0x145, 0, 0, s, 0, d, 8);
    }
    {
        const char* s = ccall<const char*>(F_GetAIStrengthString, (int32_t)1);
        item14(&it[21], 0xc, 0, 0x1cc, 0x153, 0, 0, s, 1, (const void*)&self->opts->ai_strength, 8);
    }
    {
        const void* d = (const void*)&self->opts->ai_strength;
        const char* s = ccall<const char*>(F_GetAIStrengthString, (int32_t)2);
        item_ctor(&it[22], 0xc, 0, 0x1cc, 0x161, 0, 0, s, 2, d, 8);
    }
    item_ctor(&it[23], 5, 0, 0x1c2, 0x16e, 0, 0, xlate(0x0057bd68), 0, 0, 0xc);
    item_ctor(&it[24], 6, 0, 0x21c, 0x16e, 0, 0, CP(0x004faa60), 0, (const void*)&self->ai_count, 0x15, 0x3f800000u, 0);
    item14(&it[25], 0x10, 0, 0x209, 0x16f, 0, 0, CP(0x004e4db8), 7, (const void*)&self->ai_count, 0, 0x3f800000u, 0x40e00000u);
    item_ctor(&it[26], 0x10, 0, 0x24b, 0x16f, 0, 0, CP(0x004e4db8), 7, (const void*)&self->ai_count, 1, 0x3f800000u, 0x40e00000u);
    item_ctor(&it[27], 5, 0, 0x1c2, 0x17c, 0, 0, xlate(0x0057bdd0), 0, 0, 0xc);
    item14(&it[28], 8, 0, 0x21c, 0x17c, 0, 0, CP(0x0057be00), 0, (const void*)&self->opts->ghost, 0x15);
    item_ctor(&it[29], 1, 1, 0, 0, 0, 0, 0, 0, (const void*)&self->group[0], 0);
    item_ctor(&it[30], 1, 0, 0, 0, 0, 0, 0, 0, (const void*)&self->group[1], 0);
    item_ctor(&it[31], 5, 0, 0x1c2, 0x136, 0, 0, xlate(0x0057bd48), 0, 0, 0xb);
    {
        const char* s = ccall<const char*>(F_GetGhostCarTypeString, (int32_t)1);
        item14(&it[32], 0xc, 0, 0x1cc, 0x145, 0, 0, s, 1, (const void*)&self->opts->ghost, 8);
    }
    {
        const void* d = (const void*)&self->opts->ghost;
        const char* s = ccall<const char*>(F_GetGhostCarTypeString, (int32_t)2);
        item_ctor(&it[33], 0xc, 0, 0x1cc, 0x153, 0, 0, s, 2, d, 8);
    }
    item_ctor(&it[34], 1, 1, 0, 0, 0, 0, 0, 0, (const void*)&self->group[1], 0);
    item_end(&it[35]);
    tcall<void>(F_UICC_AddItems, self, &it[0]);
}
PORT_FN(0x0049f180, "RaceOptionViewer::Added", RaceOptionViewer_Added_c, fp_added0<RaceOptionViewer>)

// Create: the AI count (option, 8 by default), the notifications, the groups now, the slider's value
static void __fastcall RaceOptionViewer_Create_c(RaceOptionViewer* self, Edx) {
    volatile int32_t* ai = self->ai_out;
    const char* sec = UI_GP(const char, S_OPT_SECTION);
    *ai = 8;
    OptionsGet(sec, CP(0x004faa68), self->ai_out);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->opts->field, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->opts->event, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->opts->ghost, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->flag, (uint32_t)1);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->opts->race_type, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->opts->laps, (uint32_t)4);
    vcall<void>(self, 0x10, (int32_t)0, (const void*)&self->opts->field);
    self->ai_count = (float)*self->ai_out;
}
PORT_FN(0x0049fdb0, "RaceOptionViewer::Create", RaceOptionViewer_Create_c, fp_notes0<RaceOptionViewer>)

// Callback: the ghost car group for a field of one (or with a ghost), else the AI group; the flag picks the laps summary
// (a race: the event parked) or the event's
static void __fastcall RaceOptionViewer_Callback_c(RaceOptionViewer* self, Edx, int32_t, const void*) {
    const int32_t f = self->opts->field;
    if (f == 1 || f == 2) {
        ccall<void>(F_UIShowGroup, (uint32_t)self->group[0]);
        ccall<void>(F_UIHideGroup, (uint32_t)self->group[1]);
    } else {
        ccall<void>(F_UIHideGroup, (uint32_t)self->group[0]);
        const bool ghost = self->opts->ghost != 0;
        const uint32_t g = self->group[1];
        if (ghost) ccall<void>(F_UIShowGroup, g);
        else ccall<void>(F_UIHideGroup, g);
    }
    xl_once(0x0057be54, 1, 0x0057be88, 0x004faa78, 0x0049ffe0);
    const bool race = *self->flag != 0;
    const uint32_t g2 = self->group[2];
    if (race) {
        ccall<void>(F_UIShowGroup, g2);
        ccall<void>(F_UIHideGroup, (uint32_t)self->group[3]);
        GameOptions* o = self->opts;
        const int32_t e = o->event;
        if (e != 0) self->event = e;
        o->event = 0;
        {
            xlate(0x0057be88);
            GameOptions* p = self->opts;
            const char* u = UI_GP(const char, 0x0057be8c);
            const int32_t laps = p->laps;
            const char* fs = ccall<const char*>(F_GetFieldString, (int32_t)p->field);
            // FIX: "<field>: <laps> <unit>" went into the caller's text unbounded -- MenuDoRaceSetup's is 0x20 bytes just
            // before this viewer in its frame, so a long translation ran into the viewer's vtable (the next call through
            // it crashed). A text that would pass 31 characters is formatted in a buffer of the rewrite's (each %s cut to
            // 64 characters) and its first 31 kept; the caller's buffer may be the original's, so 0x20 is all it's given.
            // Any other is formatted into the caller's text as before.
            if (VP_FIX && ui_strnlen(fs, 0x1f) + 2 + fix_dec_len(laps) + 1 + ui_strnlen(u, 0x1f) > 0x1f) {
                char t[0x100], c1[0x41], c2[0x41];
                game_sprintf(t, CP(0x004faa88), fix_cut(fs, c1, 0x40), laps, fix_cut(u, c2, 0x40));
                ui_copy_bounded(self->summary, t, 0x20);
            } else {
                game_sprintf(self->summary, CP(0x004faa88), fs, laps, u);
            }
        }
        {
            xlate(0x0057be88);
            const char* u = UI_GP(const char, 0x0057be8c);
            // FIX: "<laps> <unit>" into the 0x24-byte text unbounded (then the OpponentViewer, in MenuDoRaceSetup's frame):
            // as the summary's, its first 35 characters
            const int32_t laps = self->opts->laps;
            if (VP_FIX && fix_dec_len(laps) + 1 + ui_strnlen(u, 0x23) > 0x23) {
                char t[0x100], c1[0x41];
                game_sprintf(t, CP(0x004faa94), laps, fix_cut(u, c1, 0x40));
                ui_copy_bounded(self->text, t, sizeof self->text);
            } else {
                game_sprintf(self->text, CP(0x004faa94), laps, u);
            }
        }
        return;
    }
    ccall<void>(F_UIHideGroup, g2);
    ccall<void>(F_UIShowGroup, (uint32_t)self->group[3]);
    {
        GameOptions* o = self->opts;
        const int32_t e = o->event;
        if (e == 0) o->event = self->event;
        else self->event = e;
    }
    const char* es = ccall<const char*>(F_GetEventString, (int32_t)self->opts->event);
    // FIX: the event's name (a translation) into the caller's 0x20 bytes, as the summary above: its first 31 characters
    if (VP_FIX && ui_strnlen(es, 0x1f) > 0x1f) ui_copy_bounded(self->summary, es, 0x20);
    else game_sprintf(self->summary, CP(0x004faa9c), es);
}
static void fp_RaceOptionViewer_Callback(Footprint& f, RaceOptionViewer* self, Edx, int32_t, const void*) {
    if (!xl_built(0x0057be54, 1)) { f.replay_only = "the first call builds its Xlator (atexit)"; return; }
    ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));                    // the groups shown and hidden
    f.add(self, sizeof *self, "this");
    if (self->opts) f.add(self->opts, sizeof(GameOptions), "the race's options");
    if (self->summary) f.add(self->summary, 0x100, "the caller's summary text");
    FP_XL(f, 0x0057be88);
    static const uint32_t root_xl[] = {0x00504418, 0x00504428, 0x00504568,                   // GetFieldString's
                                       0x005043c8, 0x00504508, 0x00504548, 0x00504598, 0x00504630, 0x00504640};  // GetEventString's
    for (uint32_t a : root_xl) FP_XL(f, a);
}
PORT_FN(0x0049fe60, "RaceOptionViewer::Callback", RaceOptionViewer_Callback_c, fp_RaceOptionViewer_Callback)

// Destroy: the notifications; the event kept; the AI count to the caller (0 for a race without a field), plus one; saved
static void __fastcall RaceOptionViewer_Destroy_c(RaceOptionViewer* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->opts->field);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->opts->event);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)self->flag);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->opts->race_type);
    GameOptions* o = self->opts;
    const int32_t e = o->event;
    if (e != 0) {
        self->event = e;
        *self->ai_out = 0;
    } else if (o->field != 0) {
        const int32_t n = x87_ftol(D(self->ai_count));
        *self->ai_out = n;
    } else {
        *self->ai_out = 0;
    }
    volatile int32_t* ai = self->ai_out;
    const char* sec = UI_GP(const char, S_OPT_SECTION);
    *ai = *ai + 1;
    OptionsSet(sec, CP(0x004faaa0), self->event);
    OptionsSet(sec, CP(0x004faaac), x87_ftol(D(self->ai_count)));
}
PORT_FN(0x0049fff0, "RaceOptionViewer::Destroy", RaceOptionViewer_Destroy_c, fp_notes0<RaceOptionViewer>)

static void __fastcall RaceOptionViewer_Draw_c(RaceOptionViewer*, Edx, gxCanvas*) {}
PORT_FN(0x004a0090, "RaceOptionViewer::Draw", RaceOptionViewer_Draw_c, fp_pure_canvas<RaceOptionViewer>)
// the vector deleting destructor: no destructor of its own, the base's vtable back, freed
static void* __fastcall RaceOptionViewer_vdd_c(RaceOptionViewer* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x004a00a0, "RaceOptionViewer::scalar deleting destructor", RaceOptionViewer_vdd_c, fp_dtor_flags<RaceOptionViewer>)

#undef D
#undef CP
#undef FP_XL
#undef game_sprintf
#undef OptionsGet
#undef OptionsSet
}  // namespace menu_view
}  // namespace
