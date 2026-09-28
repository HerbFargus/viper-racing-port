// menu_board.cpp -- M3 UI stage, step U2 (group C): the high score board, rewritten faithfully (library `menu`: board.obj).
//
//   BoardCustomText (a line of text, right-aligned in euro12.fnt), RaceResultTable (the records' columns: its legend and
//   one record's row -- race time, car, best lap, top speed in the locale's unit, date), HighScoreBoardTable (a RecordMgr's
//   best lap, best speed and six best races), BoardControl (the table in a CustomWidget, for the post-race screen:
//   BoardCreateControl), BoardDo (the board as its own dialog, with a Clear button: clear_cb).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own too, so a
// hooked rewrite is what runs), virtual calls through the vtable, the text drawn through gxFontPrintf with the original's
// arguments (a translated string passed as the format where the original passes it so). A record's times and speed are
// tested by their bits (`cmp dword [x], 0`: a positive float, a positive NaN too); the speed is the locale's factor times
// the record's, the x87 product formatted as a double. BoardDo builds its dialog, the table and the text on its stack as the
// original does (the items it writes in place with the same 14 dwords, the one it builds with UIDialogItem's constructor
// with it).
//
// Footprints (main thread): the draws write the canvas they're given (its clip and pixels), the current-canvas static,
// PhysicsTimeString's buffer and the Xlators they refresh; while one of their function-local Xlators is unbuilt (its
// destructor goes to atexit) the check is left to the session replays. replay_only: the constructors and destructors
// (fonts, palettes), BoardCreateControl (allocates, loads the records), BoardDo (a dialog), clear_cb (clears the records),
// BoardControl::Added (builds a widget), the deleting destructors.
//
// Fixes (// FIX:, docs/FIXES.md "Menus"): BoardCustomText::Draw passes its text to gxFontPrintf as the format, as the
// original, unless the game's C runtime would read an argument for it (nobody passes one: a '%' conversion or '*' in a
// translation), when it's printed as it is ("%s"); BoardDo's "<race type>: <realism>" keeps to its 0x40 bytes. Every
// other input gives the original's bits.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "menu_view.h"

namespace {
namespace menu_board {
using namespace uit;
using namespace mview;

#define D(x) ((double)(x))
#define CP(p) ((const char*)(uintptr_t)(p))
typedef void(__cdecl* FontPrintf_v)(void*, void*, uint32_t, int32_t, int32_t, const char*, ...);
#define gxFontPrintf ((FontPrintf_v)(uintptr_t)F_gxFontPrintf)
typedef void(__cdecl* Sprintf_v)(char*, const char*, ...);
#define game_sprintf ((Sprintf_v)(uintptr_t)F_sprintf)
static __forceinline bool xl_built(uint32_t guard, uint8_t bits) { return (UI_G8(guard) & bits) == bits; }
#define FP_XL(f, a) UI_FP(f, a, 12, "a function's static Xlator")
enum : uint32_t { S_TIME_STRING = 0x005215d8 };   // PhysicsTimeString's buffer (0x24)

template <typename T> static void fp_pure_canvas(Footprint& f, T*, Edx, gxCanvas*) { f.pure = true; }
template <typename T> static void fp_fonts0(Footprint& f, T*, Edx) { f.replay_only = "loads or frees a font and palettes"; }
template <typename T> static void fp_dtor_flags(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object"; }

// =========================================================================================================================
// BoardCustomText
// =========================================================================================================================
static BoardCustomText* __fastcall BoardCustomText_ctor_c(BoardCustomText* self, Edx, const char* text) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->text = text;
    self->vtbl = (const void*)(uintptr_t)VT_BoardCustomText;
    self->widget = 0;
    self->font = ccall<void*>(F_gxFontGet, CP(0x004f90dc));
    self->pal = ccall<void*>(F_gxPaletteCreate);
    ccall<void>(F_gxPaletteMakeGradient, (void*)self->pal, UI_GU32(0x0057a6f0), UI_GU32(0x0057a604));
    return self;
}
static void fp_BoardCustomText_ctor(Footprint& f, BoardCustomText*, Edx, const char*) { f.replay_only = "loads a font, creates a palette"; }
PORT_FN(0x0048fd50, "BoardCustomText::BoardCustomText", BoardCustomText_ctor_c, fp_BoardCustomText_ctor)

static void __fastcall BoardCustomText_dtor_c(BoardCustomText* self, Edx) {
    void* pal = self->pal;
    self->vtbl = (const void*)(uintptr_t)VT_BoardCustomText;
    ccall<void>(F_gxPaletteDestroy, pal);
    ccall<void>(F_gxFontForget, (void*)self->font);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
PORT_FN(0x0048fdb0, "BoardCustomText::~BoardCustomText", BoardCustomText_dtor_c, fp_fonts0<BoardCustomText>)

// FIX helper (BoardCustomText::Draw): whether the game's printf (output.obj, 0x4d22b0), given s as its format, would read an
// argument. Its state machine run over s, from its own table (.rdata 0x4e0200: a character's class in the low nibble of
// [c - ' '] for ' '..'x', else 0; the next state in the high nibble of [class * 8 + state]): a '*' taken as a width or
// precision (states 3, 5) reads one; so does reaching a conversion (state 7) -- every one reads an argument but 'B', which
// prints the stale locals instead. In the size state an 'I' skips a following "64", else it's printed and the state is
// back to normal, as output does. (Its double-byte lead-byte step is left out: the game never leaves the "C" locale, whose
// table has no lead bytes.)
static bool fix_format_reads_args(const char* s) {
    enum : uint32_t { T = 0x004e0200 };
    uint32_t state = 0;
    for (const volatile char* p = s; *p; p++) {
        const int32_t c = (int8_t)*p;
        const uint32_t cls = c < 0x20 || c > 0x78 ? 0u : (uint32_t)(UI_G8(T + (uint32_t)(c - 0x20)) & 0xf);
        state = (uint32_t)(UI_G8(T + cls * 8 + state) >> 4) & 7u;
        if ((state == 3 || state == 5) && c == '*') return true;
        if (state == 7) return true;
        if (state == 6 && c == 'I') {
            if (p[1] == '6' && p[2] == '4') p += 2;
            else state = 0;
        }
    }
    return false;
}

// Draw: the text right-aligned at the item's right edge, its top at the item's
static void __fastcall BoardCustomText_Draw_c(BoardCustomText* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    // FIX: the text (BoardDo's "<race type>: <realism>", PostRaceDo's "<race type> : <track>: <realism>" -- translations
    // and tracks.tab's names) is gxFontPrintf's format and nothing else is passed, so a '%' conversion or '*' in it read
    // arguments from the stack (garbage; "%s" or "%n" could crash). Such a text is printed as it is, through "%s". Every
    // other text is the format as before, so it prints exactly as it did ("%%" as '%').
    if (VP_FIX && fix_format_reads_args((const char*)self->text))
        gxFontPrintf((void*)self->font, (void*)self->pal, 0xa, self->x + self->w, (int32_t)self->y, CP(0x004f1e84), (const char*)self->text);
    else
        gxFontPrintf((void*)self->font, (void*)self->pal, 0xa, self->x + self->w, (int32_t)self->y, (const char*)self->text);
}
static void fp_BoardCustomText_Draw(Footprint& f, BoardCustomText*, Edx, gxCanvas* c) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}
PORT_FN(0x0048fde0, "BoardCustomText::Draw", BoardCustomText_Draw_c, fp_BoardCustomText_Draw)

// =========================================================================================================================
// RaceResultTable
// =========================================================================================================================
static RaceResultTable* __fastcall RaceResultTable_ctor_c(RaceResultTable* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_RaceResultTable;
    self->font = ccall<void*>(F_gxFontGet, CP(0x004f90e8));
    self->pal[0] = ccall<void*>(F_gxPaletteCreate);
    ccall<void>(F_gxPaletteMakeGradient, (void*)self->pal[0], UI_GU32(0x0057a6c4), UI_GU32(0x0057a604));
    {
        void* p = ccall<void*>(F_gxPaletteCreate);
        self->pal[1] = p;
        const uint32_t b = UI_GU32(0x0057a604), a = UI_GU32(0x0057a64c);
        ccall<void>(F_gxPaletteMakeGradient, p, a, b);
    }
    {
        void* p = ccall<void*>(F_gxPaletteCreate);
        self->pal[2] = p;
        const uint32_t b = UI_GU32(0x0057a604), a = UI_GU32(0x0057a6f0);
        ccall<void>(F_gxPaletteMakeGradient, p, a, b);
    }
    return self;
}
PORT_FN(0x0048fe20, "RaceResultTable::RaceResultTable", RaceResultTable_ctor_c, fp_fonts0<RaceResultTable>)

static void __fastcall RaceResultTable_dtor_c(RaceResultTable* self, Edx) {
    void* p2 = self->pal[2];
    self->vtbl = (const void*)(uintptr_t)VT_RaceResultTable;
    ccall<void>(F_gxPaletteDestroy, p2);
    ccall<void>(F_gxPaletteDestroy, (void*)self->pal[1]);
    ccall<void>(F_gxPaletteDestroy, (void*)self->pal[0]);
    ccall<void>(F_gxFontForget, (void*)self->font);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->font = 0;
}
PORT_FN(0x0048feb0, "RaceResultTable::~RaceResultTable", RaceResultTable_dtor_c, fp_fonts0<RaceResultTable>)

// draw_legend(y, date, race): the columns' titles (the race time's only with `race`, the date's only with `date`)
static void __fastcall RaceResultTable_draw_legend_c(RaceResultTable* self, Edx, int32_t y, uint8_t date, uint8_t race) {
    const int32_t x = self->x;
    xl_once(0x0057a6c0, 0x01, 0x0057a670, 0x004f90f4, 0x00490250);
    xl_once(0x0057a6c0, 0x02, 0x0057a690, 0x004f910c, 0x00490240);
    xl_once(0x0057a6c0, 0x04, 0x0057a628, 0x004f9120, 0x00490230);
    xl_once(0x0057a6c0, 0x08, 0x0057a6b0, 0x004f9130, 0x00490220);
    xl_once(0x0057a6c0, 0x10, 0x0057a5f8, 0x004f9144, 0x00490210);
    xl_once(0x0057a6c0, 0x20, 0x0057a608, 0x004f915c, 0x00490200);
    xl_once(0x0057a6c0, 0x40, 0x0057a6c8, 0x004f9170, 0x004901f0);
    xl_once(0x0057a6c0, 0x80, 0x0057a618, 0x004f9180, 0x004901e0);
    if (race != 0) {
        const char* t = xlate(0x0057a670);
        gxFontPrintf((void*)self->font, (void*)self->pal[0], 9, x + 0x1e, y, t);
    }
    {
        const char* t = xlate(0x0057a690);
        gxFontPrintf((void*)self->font, (void*)self->pal[0], 9, x + 0x82, y, t);
    }
    {
        const char* t = xlate(0x0057a6b0);
        gxFontPrintf((void*)self->font, (void*)self->pal[0], 9, x + 0xe6, y, t);
    }
    {
        const char* t = xlate(0x0057a6c8);
        gxFontPrintf((void*)self->font, (void*)self->pal[0], 0xc, x + 0x159, y, t);
    }
    {
        const char* t = xlate(0x0057a618);
        gxFontPrintf((void*)self->font, (void*)self->pal[0], 0xc, x + 0x159, y + 0xc, t);
    }
    if (date != 0) {
        const char* t = xlate(0x0057a608);
        gxFontPrintf((void*)self->font, (void*)self->pal[0], 9, x + 0x1b3, y, t);
    }
}
static const uint32_t k_legend_xl[] = {0x0057a670, 0x0057a690, 0x0057a628, 0x0057a6b0, 0x0057a5f8, 0x0057a608, 0x0057a6c8, 0x0057a618};
static void fp_RaceResultTable_draw_legend(Footprint& f, RaceResultTable*, Edx, int32_t, uint8_t, uint8_t) {
    if (!xl_built(0x0057a6c0, 0xff)) { f.replay_only = "the first call builds its Xlators (atexit)"; return; }
    UI_FP_CUR_CANVAS(f);
    for (uint32_t a : k_legend_xl) FP_XL(f, a);
}
PORT_FN(0x0048ff00, "RaceResultTable::draw_legend", RaceResultTable_draw_legend_c, fp_RaceResultTable_draw_legend)

// draw_element(canvas, y, record, date, race, unused): a record's row; a time or speed of 0 or less isn't shown (an empty
// race time shows "--"-style text); the best of each highlit (flags 8, 2, 4); the car's name clipped to its column
typedef const char*(__cdecl* TimeString_t)(uint32_t, uint8_t);
static void __fastcall RaceResultTable_draw_element_c(RaceResultTable* self, Edx, gxCanvas* c, int32_t y, const MvRaceRecord* r,
                                                      uint8_t date, uint8_t race, uint8_t) {
    char buf[0x40];
    xl_once(0x0057a614, 1, 0x0057a710, 0x004f9194, 0x00490480);
    const int32_t x = self->x;
    if (race != 0) {
        if (*(const volatile int32_t*)&r->race_time > 0) {
            void* pal = (r->flags & 8) ? self->pal[2] : self->pal[1];
            const char* s = ((TimeString_t)(uintptr_t)F_PhysicsTimeString)(*(const volatile uint32_t*)&r->race_time, 1);
            gxFontPrintf((void*)self->font, pal, 9, x + 0x1e, y, CP(0x004f91a4), s);
        } else {
            const char* t = xlate(0x0057a710);
            gxFontPrintf((void*)self->font, (void*)self->pal[1], 9, x + 0x1e, y, t);
        }
    }
    ccall<void>(F_gxSetClip, c, x + 0x82, y - 0xf, x + 0xd8, y + 0xf);
    gxFontPrintf((void*)self->font, (void*)self->pal[1], 9, x + 0x82, y, CP(0x004f91a8), (const char*)r->car_name);
    ccall<void>(F_gxRestoreClip, c);
    if (*(const volatile int32_t*)&r->lap_time > 0) {
        void* pal = (r->flags & 2) ? self->pal[2] : self->pal[1];
        const char* s = ((TimeString_t)(uintptr_t)F_PhysicsTimeString)(*(const volatile uint32_t*)&r->lap_time, 1);
        gxFontPrintf((void*)self->font, pal, 9, x + 0xe6, y, CP(0x004f91ac), s);
    }
    if (*(const volatile int32_t*)&r->top_speed > 0) {
        void* pal = (r->flags & 4) ? self->pal[2] : self->pal[1];
        const double v = D(*(const volatile float*)(locale() + 0x1c)) * D(r->top_speed);
        game_sprintf(buf, CP(0x004f91b0), v);
        ccall<void>(F_LocaleConvertNumeric, (char*)buf);
        gxFontPrintf((void*)self->font, pal, 9, x + 0x140, y, CP(0x004f91b8), (const char*)buf);
    }
    if (date != 0) {
        const int32_t year = r->year;
        const int32_t month = r->month, day = r->day;
        ccall<void>(F_LocaleFormatShortDate, (char*)buf, (int32_t)0x40, day, month, year);
        gxFontPrintf((void*)self->font, (void*)self->pal[1], 9, x + 0x1b3, y, CP(0x004f91bc), (const char*)buf);
    }
}
static void fp_RaceResultTable_draw_element(Footprint& f, RaceResultTable*, Edx, gxCanvas* c, int32_t, const MvRaceRecord*, uint8_t,
                                            uint8_t, uint8_t) {
    if (!xl_built(0x0057a614, 1)) { f.replay_only = "the first call builds its Xlator (atexit)"; return; }
    UI_FP_CUR_CANVAS(f);
    UI_FP_CANVAS(f, c);
    FP_XL(f, 0x0057a710);
    UI_FP(f, S_TIME_STRING, 0x24, "PhysicsTimeString's buffer");
}
PORT_FN(0x00490260, "RaceResultTable::draw_element", RaceResultTable_draw_element_c, fp_RaceResultTable_draw_element)

// =========================================================================================================================
// HighScoreBoardTable, BoardControl, BoardDo
// =========================================================================================================================
// Draw: the legend; the best lap, the best speed (titled), then the six best races
static void __fastcall HighScoreBoardTable_Draw_c(HighScoreBoardTable* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    const int32_t x = self->x;
    int32_t y = self->y;
    xl_once(0x0057a67c, 1, 0x0057a6a0, 0x004f91c0, 0x00490680);
    tcall<void>(F_draw_legend, self, y, (uint8_t)1, (uint8_t)0);
    y += 0x12;
    const int32_t tx = x + 5;
    {
        const char* t = xlate(0x0057a6a0);
        gxFontPrintf((void*)self->font, (void*)self->pal[2], 9, tx, y, t);
    }
    y += 0x12;
    if (const void* r = tcall<const void*>(F_RecordMgr_GetNthBestLap, (const void*)self->mgr, (int32_t)0))
        tcall<void>(F_draw_element, self, c, y, r, (uint8_t)1, (uint8_t)0, (uint8_t)0);
    y += 0x24;
    xl_once(0x0057a67c, 2, 0x0057a6d8, 0x004f91d8, 0x00490670);
    {
        const char* t = xlate(0x0057a6d8);
        gxFontPrintf((void*)self->font, (void*)self->pal[2], 9, tx, y, t);
    }
    y += 0x12;
    if (const void* r = tcall<const void*>(F_RecordMgr_GetNthBestSpeed, (const void*)self->mgr, (int32_t)0))
        tcall<void>(F_draw_element, self, c, y, r, (uint8_t)1, (uint8_t)0, (uint8_t)0);
    y += 0x24;
    xl_once(0x0057a67c, 4, 0x0057a640, 0x004f91f4, 0x00490660);
    {
        const char* t = xlate(0x0057a640);
        gxFontPrintf((void*)self->font, (void*)self->pal[2], 9, tx, y, t);
    }
    y += 0x12;
    for (int32_t i = 0; i < 6; i++) {
        if (const void* r = tcall<const void*>(F_RecordMgr_GetNthBestRace, (const void*)self->mgr, i))
            tcall<void>(F_draw_element, self, c, y, r, (uint8_t)1, (uint8_t)1, (uint8_t)(i - 9 == 0 ? 0 : 1));
        y += 0x10;
    }
}
static void fp_HighScoreBoardTable_Draw(Footprint& f, HighScoreBoardTable*, Edx, gxCanvas* c) {
    if (!xl_built(0x0057a67c, 7) || !xl_built(0x0057a6c0, 0xff) || !xl_built(0x0057a614, 1)) {
        f.replay_only = "the first call builds its Xlators (atexit)";
        return;
    }
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
    FP_XL(f, 0x0057a6a0); FP_XL(f, 0x0057a6d8); FP_XL(f, 0x0057a640); FP_XL(f, 0x0057a710);
    for (uint32_t a : k_legend_xl) FP_XL(f, a);
    UI_FP(f, S_TIME_STRING, 0x24, "PhysicsTimeString's buffer");
}
PORT_FN(0x00490490, "HighScoreBoardTable::Draw", HighScoreBoardTable_Draw_c, fp_HighScoreBoardTable_Draw)

// BoardCreateControl: the post-race board: the track's records in this mode, the table
static void* __cdecl BoardCreateControl_c(const char* track, int32_t realism, int32_t race_type, uint8_t mirror) {
    BoardControl* bc = ccall<BoardControl*>(F_MemAlloc, (int32_t)0x24);
    if (!bc) return 0;
    bc->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    bc->widget = 0;
    bc->vtbl = (const void*)(uintptr_t)VT_BoardControl;
    const int32_t t = ccall<int32_t>(F_GetTrackNumber, track);
    bc->track = t;
    bc->mgr = ccall<const void*>(F_RecordMgrCreate, ccall<const char*>(F_GetTrackName, t));
    tcall<void>(F_RecordMgr_SetMode, (const void*)bc->mgr, realism, race_type, mirror);
    HighScoreBoardTable* tb = ccall<HighScoreBoardTable*>(F_MemAlloc, (int32_t)0x2c);
    if (tb) {
        const void* mgr = bc->mgr;
        tcall<void*>(F_RaceResultTable_ctor, tb);
        tb->vtbl = (const void*)(uintptr_t)VT_HighScoreBoardTable;
        UI_GP(HighScoreBoardTable, S_BOARD_TABLE) = tb;
        tb->mgr = mgr;
        bc->table = tb;
        return bc;
    }
    bc->table = 0;
    return bc;
}
static void fp_BoardCreateControl(Footprint& f, const char*, int32_t, int32_t, uint8_t) { f.replay_only = "allocates, loads the records"; }
PORT_FN(0x00490690, "BoardCreateControl", BoardCreateControl_c, fp_BoardCreateControl)

// BoardDo: the track's records in this mode as a dialog: the mode's title, the table, Clear and Done
static void __cdecl BoardDo_c(const char* track, int32_t realism, int32_t race_type, uint8_t mirror) {
    struct {
        UIDialog dlg;
        UIDialogItem it[5];
        HighScoreBoardTable table;
        BoardCustomText text;
        char title[0x40];
    } fr;
    const int32_t t = ccall<int32_t>(F_GetTrackNumber, track);
    const void* mgr = ccall<const void*>(F_RecordMgrCreate, ccall<const char*>(F_GetTrackName, t));
    UI_GP(const void, S_BOARD_MGR) = mgr;
    tcall<void>(F_RecordMgr_SetMode, mgr, realism, race_type, mirror);
    const char* friendly = ccall<const char*>(F_GetTrackFriendlyName, t);
    xl_once(0x0057a66c, 1, 0x0057a5e8, 0x004f9220, 0x00490b30);
    {
        const char* rs = ccall<const char*>(F_GetRealismString, realism);
        const char* ts = ccall<const char*>(F_GetRaceTypeString, race_type);
        // FIX: "<race type>: <realism>" (translations) went into 0x40 bytes unbounded at the end of the frame, so a longer
        // one ran onto the return address: a title that would pass 63 characters is formatted in a buffer of the
        // rewrite's (each name cut to 64 characters) and its first 63 kept. Any other is formatted into the title as
        // before.
        if (VP_FIX && ui_strnlen(ts, 0x3f) + 2 + ui_strnlen(rs, 0x3f) > 0x3f) {
            char t[0x100], c1[0x41], c2[0x41];
            game_sprintf(t, CP(0x004f923c), fix_cut(ts, c1, 0x40), fix_cut(rs, c2, 0x40));
            ui_copy_bounded(fr.title, t, sizeof fr.title);
        } else {
            game_sprintf(fr.title, CP(0x004f923c), ts, rs);
        }
    }
    {
        const void* m = UI_GP(const void, S_BOARD_MGR);
        tcall<void*>(F_RaceResultTable_ctor, &fr.table);
        UI_GP(HighScoreBoardTable, S_BOARD_TABLE) = &fr.table;
        fr.table.mgr = m;
        fr.table.vtbl = (const void*)(uintptr_t)VT_HighScoreBoardTable;
        tcall<void*>(F_BoardCustomText_ctor, &fr.text, (const char*)fr.title);
    }
    xl_once(0x0057a66c, 2, 0x0057a680, 0x004f9244, 0x00490b20);
    xl_once(0x0057a66c, 4, 0x0057a700, 0x004f9250, 0x00490b10);
    xl_once(0x0057a66c, 8, 0x0057a658, 0x004f9264, 0x00490b00);
    item14(&fr.it[0], 0x17, 0, 0xfa, 0x1c, 0x168, 0x10, CP(0x004e4db8), 0, (const void*)&fr.text, 0);
    item14(&fr.it[1], 0x17, 0, 0x28, 0x82, 0x230, 0x136, CP(0x004e4db8), 0, (const void*)&fr.table, 0);
    item14(&fr.it[2], 2, 0, 0x1e5, 0x1a5, 0, 0, xlate(0x0057a700), 0, (const void*)(uintptr_t)F_clear_cb, 2);
    item_ctor(&fr.it[3], 2, -1, 0x13, 0x1a2, 0, 0, xlate(0x0057a658), 0, 0, 6);
    item_end(&fr.it[4]);
    {
        volatile uint32_t* d = (volatile uint32_t*)&fr.dlg;
        d[0] = (uint32_t)(uintptr_t)friendly;
        d[3] = (uint32_t)(uintptr_t)&fr.it[0];
        const int32_t h = UI_G32(S_GX_SCREEN_H), w = UI_G32(S_GX_SCREEN_W);
        d[4] = 0;
        d[1] = 0x004f926c;
        d[2] = 0xffffffffu;
        ccall<int32_t>(F_UIDoDialog, (const void*)&fr.dlg, w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
    }
    ccall<void>(F_RecordMgrDestroy, UI_GP(const void, S_BOARD_MGR));
    tcall<void>(F_BoardCustomText_dtor, &fr.text);
    UI_GP(HighScoreBoardTable, S_BOARD_TABLE) = 0;
    fr.table.vtbl = (const void*)(uintptr_t)VT_HighScoreBoardTable;
    tcall<void>(F_RaceResultTable_dtor, &fr.table);
}
static void fp_BoardDo(Footprint& f, const char*, int32_t, int32_t, uint8_t) { f.replay_only = "runs a dialog (modal), loads the records"; }
PORT_FN(0x00490740, "BoardDo", BoardDo_c, fp_BoardDo)

// clear_cb: the Clear button -- the records emptied, the table redrawn
static uint8_t __cdecl clear_cb_c(int32_t) {
    tcall<void>(F_RecordMgr_Clear, UI_GP(const void, S_BOARD_MGR));
    tcall<void>(F_UICC_Dirty, UI_GP(HighScoreBoardTable, S_BOARD_TABLE));
    return 0;
}
static void fp_clear_cb(Footprint& f, int32_t) { f.replay_only = "clears the track's records (RecordMgr::Clear)"; }
PORT_FN(0x00490ae0, "clear_cb", clear_cb_c, fp_clear_cb)

static void* __fastcall BoardCustomText_vdd_c(BoardCustomText* self, Edx, uint32_t flags) {
    tcall<void>(F_BoardCustomText_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00490b40, "BoardCustomText::vector deleting destructor", BoardCustomText_vdd_c, fp_dtor_flags<BoardCustomText>)
static void* __fastcall RaceResultTable_vdd_c(RaceResultTable* self, Edx, uint32_t flags) {
    tcall<void>(F_RaceResultTable_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00490b60, "RaceResultTable::vector deleting destructor", RaceResultTable_vdd_c, fp_dtor_flags<RaceResultTable>)
// HighScoreBoardTable's: no longer the one shown; RaceResultTable's destructor
static void* __fastcall HighScoreBoardTable_sdd_c(HighScoreBoardTable* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_HighScoreBoardTable;
    UI_GP(HighScoreBoardTable, S_BOARD_TABLE) = 0;
    tcall<void>(F_RaceResultTable_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00490b80, "HighScoreBoardTable::scalar deleting destructor", HighScoreBoardTable_sdd_c, fp_dtor_flags<HighScoreBoardTable>)

// BoardControl::Added: the table in a CustomWidget of its own
static void __fastcall BoardControl_Added_c(BoardControl* self, Edx) {
    UIDialogItem it[2];
    item14(&it[0], 0x17, 0, 0x28, 0x82, 0x230, 0x136, CP(0x004e4db8), 0, (const void*)self->table, 0);
    item_end(&it[1]);
    tcall<void>(F_UICC_AddItems, self, &it[0]);
}
static void fp_BoardControl_Added(Footprint& f, BoardControl*, Edx) { f.replay_only = "builds a widget (_UIAddItems allocates)"; }
PORT_FN(0x00490bb0, "BoardControl::Added", BoardControl_Added_c, fp_BoardControl_Added)
static void __fastcall BoardControl_Draw_c(BoardControl*, Edx, gxCanvas*) {}
PORT_FN(0x00490c30, "BoardControl::Draw", BoardControl_Draw_c, fp_pure_canvas<BoardControl>)
// the scalar deleting destructor: the table deleted through its vtable, the records freed
static void* __fastcall BoardControl_sdd_c(BoardControl* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_BoardControl;
    if (HighScoreBoardTable* t = self->table) vcall<void*>(t, 0, (uint32_t)1);
    ccall<void>(F_RecordMgrDestroy, (const void*)self->mgr);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00490c40, "BoardControl::scalar deleting destructor", BoardControl_sdd_c, fp_dtor_flags<BoardControl>)

#undef D
#undef CP
#undef FP_XL
#undef gxFontPrintf
#undef game_sprintf
}  // namespace menu_board
}  // namespace
