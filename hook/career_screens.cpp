// career_screens.cpp -- M3 UI stage, step U4 (group A): the career's screens, rewritten faithfully (library `career`:
// chooser.obj, events.obj, postseas.obj, ranking.obj, testing.obj; the core -- career.obj, season.obj -- is career_main.cpp).
//
//   chooser.obj: CareerChooser (the eight slots as radio buttons, the selected one's summary, Create / Load / Delete), the
//   new career's dialog (create_cb: name, realism, damage), refresh (every slot's career file read), CareerSummary (name,
//   class, season, week, hours, funds; its CareerBlurb variant follows the selected slot and shows its groups).
//   events.obj: EventsDo (the season's calendar, Enter / Back), SeasonViewer (track, length, purse, place, points, totals).
//   postseas.obj: PostSeasonDo (the standings after a race or a season, the money screens behind it), the season's winning
//   splash, NewClassDo (the advancement certificate), StandingsViewer, ClickThrough (a click anywhere closes a splash).
//   ranking.obj: RankingDo (the standings from the career's menu).
//   testing.obj: TestingDo (the testing menu, then a practice race, until Back; in assembly, its frame exactly the
//   original's -- see there), do_race, testing_menu (the tracks' list, the selected track's picture -- TrackImage --,
//   reversed).
//
// Written as career_main.cpp is (see there): calls by their v1.0 address in the original's order, virtual calls through the
// vtable, the inline string code as the original runs it, every screen's item list and UIDialog in a frame laid out as
// the original's, items built with UIDialogItem's constructor where the original does and written in place (all 14
// dwords) where it does. The destructors the original inlines into a screen (the CareerStatus, SeasonViewer and
// StandingsViewer on its stack) are inlined the same way: the vtable stores and the styles removed.
//
// Footprints (main thread). Draws write the canvas they're given and the current-canvas static, and refresh the Xlators they
// read; a draw whose function-local Xlators aren't all built builds them (atexit): replay_only. CareerBlurb::Callback writes
// its control, its widget (dirty) and the running window's groups; the callbacks' Dirty its widget; ClickThrough's mouse
// the clicked flag. replay_only: the screens (their own dialog loops), the constructors and destructors (styles: fonts
// loaded and freed; notifications), refresh (reads the career files), TrackImage::Draw (loads a stamp), and the money and
// splash callbacks that run the credits or a dialog.
//
// Fixes (// FIX:, docs/FIXES.md "Career"; none reached with the stock game and English text): create_cb cuts the default
// player name (a translation) to its 16-byte buffer; testing_menu doesn't test a track whose name (tracks.tab's) is too
// long for TestingDo's 32-byte buffer (Test leaves as Back does), and do_race doesn't race one too long for the World's 32
// bytes (it returns at once); TrackImage::Draw doesn't load a track's picture whose name (76+ characters) doesn't fit its
// 0x50 bytes; EventsDo's two texts, SeasonViewer::Draw's and StandingsViewer::Draw's "<n> <unit>" texts keep to their
// 0x100 bytes (255 characters of a long translation). Every other input gives the original's bits.
//
// FIX CANDIDATEs (left faithful, marked in place; "ordinary play" = the stock game, English): SeasonViewer::Draw indexes
// the purse and the place names by the player's place unchecked (a damaged career file); EventsDo, testing_menu,
// PostSeasonDo and RankingDo add their CareerStatus before its texts are formatted (career_main.cpp: its StaticTexts copy
// stack garbage, every time; harmless unless it runs 256+ bytes); testing_menu's list's change count is uninitialised
// (harmless). CareerChooser lists slot 1 twice (a second radio button on top of the first: harmless).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "career_types.h"

namespace {
namespace career_screens {
using namespace uit;
using namespace car;

typedef int(__cdecl* Sprintf_v)(char*, const char*, ...);
#define SPRINTF ((Sprintf_v)(uintptr_t)uit::F_sprintf)
static __forceinline void put14(uint8_t* F, uint32_t off, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w,
                                uint32_t h, uint32_t text, uint32_t i1c, uint32_t data, uint32_t style, uint32_t sel = 0) {
    volatile uint32_t* d = (volatile uint32_t*)(F + off);
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text; d[7] = i1c; d[8] = data; d[9] = style;
    d[10] = 0; d[11] = 0; d[12] = sel; d[13] = 0;
}
static __forceinline void end_item(uint8_t* F, uint32_t off) { crt_copy(F + off, (const void*)(uintptr_t)S_END_ITEM, 0x38); }
static __forceinline void zero_dwords(void* at, uint32_t n) {
    volatile uint32_t* p = (volatile uint32_t*)at;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}
// a UIStyleDesc in a frame: its 13 dwords, in order
static __forceinline void put_desc(uint8_t* F, uint32_t off, uint32_t font, uint32_t flags, uint32_t cn, uint32_t co,
                                   uint32_t cn2, uint32_t xo, uint32_t yo, uint32_t cd, uint32_t cdo, uint32_t cd2, uint32_t xod,
                                   uint32_t yod, uint32_t stamp) {
    volatile uint32_t* d = (volatile uint32_t*)(F + off);
    d[0] = font; d[1] = flags; d[2] = cn; d[3] = co; d[4] = cn2; d[5] = xo; d[6] = yo; d[7] = cd; d[8] = cdo; d[9] = cd2;
    d[10] = xod; d[11] = yod; d[12] = stamp;
}
static __forceinline int32_t add_style(uint8_t* F, uint32_t off) { return ccall<int32_t>(F_UIAddDynamicStyle, (const void*)(F + off)); }
static __forceinline void style_draw(int32_t style, int32_t x, int32_t y, uint32_t text, uint32_t flags) {
    ccall<void>(uit::F_UIStyleDraw, style, x, y, CA_CP(text), flags);
}
static __forceinline int32_t do_dialog(uint8_t* dlg, int32_t w, int32_t h) {
    return ccall<int32_t>(uit::F_UIDoDialog, (const void*)dlg, w, h, -999, -999, 1);
}
static __forceinline uint8_t* info() { return ccall<uint8_t*>(F_CareerGetInfo); }
static __forceinline const uint8_t* season() { return ccall<const uint8_t*>(F_CareerGetSeason); }
#define I32(p, off) (*(volatile int32_t*)((const uint8_t*)(p) + (off)))

// ---- footprint helpers --------------------------------------------------------------------------------------------------
static void fp_screen(Footprint& f) { f.replay_only = "runs a screen of the career (a dialog)"; }
static void fp_screen_i(Footprint& f, int32_t) { f.replay_only = "runs a screen or a dialog of the career"; }
static void fp_screen_pi(Footprint& f, int32_t*) { f.replay_only = "runs a screen or a dialog of the career"; }
template <typename T> static void fp_styles0(Footprint& f, T*, Edx) { f.replay_only = "adds or removes dynamic styles (loads, frees fonts)"; }
template <typename T> static void fp_dtor_flags(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object and its styles"; }
template <typename T> static void fp_notes0(Footprint& f, T*, Edx) { f.replay_only = "adds notifications (allocates)"; }
static __forceinline void fp_draw_canvas(Footprint& f, gxCanvas* c) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}
// Dirty: the control's widget's flag
template <typename T> static void fp_dirty(Footprint& f, T* self, Edx, int32_t, const void*) {
    if (self->widget) UI_FP_WIDGET(f, self->widget, "the control's widget");
}

// =========================================================================================================================
// chooser.obj
// =========================================================================================================================
static int32_t __cdecl CareerChooser_c() {
    alignas(8) uint8_t F[0x4a4];                                    // sub esp, 0x494; 4 pushes
    // F+0x10 the UIDialog, +0x24 the slots' style, +0x58 the CareerBlurb, +0x7c the items
    const char* game = *(const char* const volatile*)(uintptr_t)0x004dfa5c;                     // "GAME"
    CA_G32(S_CH_SLOT) = 0;
    ccall<void>(F_OptionsGetI, game, CA_CP(0x004ffa54), (void*)(uintptr_t)S_CH_SLOT);        // career_slot
    tcall<void*>(F_CareerSummary_ctor, (void*)(F + 0x58), (const void*)(uintptr_t)S_CH_INFOS);
    CA_W(F, 0x58) = VT_CareerBlurb;
    {
        void* m = ccall<void*>(uit::F_MemAlloc, 0x14);
        if (m) CA_GU32(S_CH_LIST) = tcall<uint32_t>(uit::F_UIStringList_ctor, m, 8, 0xe);
        else CA_GU32(S_CH_LIST) = 0;
    }
    {
        const uint32_t c78 = CA_GU32(0x005d0078), cd4 = CA_GU32(0x005d00d4), cfc = CA_GU32(0x005d00fc), cc4 = CA_GU32(0x005d00c4);
        put_desc(F, 0x24, 0x004ffa6c, 0x21, c78, cd4, cfc, 8, 0x3d, cc4, cc4, cfc, 8, 0x3d, 0x004ffa78);   // ruby9, license.stp
    }
    ca_xl_once(0x005d00ac, 0x01, 0x005d00b8, 0x004ffa84, 0x004bf700);  // UI:Back
    ca_xl_once(0x005d00ac, 0x02, 0x005d3588, 0x004ffa8c, 0x004bf6f0);  // Career:Create
    ca_xl_once(0x005d00ac, 0x04, 0x005d00f0, 0x004ffa9c, 0x004bf6e0);  // Career:Load
    ca_xl_once(0x005d00ac, 0x08, 0x005d0178, 0x004ffaa8, 0x004bf6d0);  // Career:Delete
    ca_xl_once(0x005d00ac, 0x10, 0x005d0160, 0x004ffab8, 0x004bf6c0);  // Career:Title
    const int32_t style = add_style(F, 0x24);
    const uint32_t sel = S_CH_SLOT;
    int32_t r;
    for (;;) {
        // the slots: radio buttons on the selection, their names the CareerInfos' (slot 1's is there twice, as the original)
        ca_item_ctor(F + 0x7c, 0xc, 0, 0x13, 0x4a, 0, 0, 0x005d01b0, 0, sel, style);
        ca_item_ctor(F + 0xb4, 0xc, 0, 0x92, 0x4a, 0, 0, 0x005d0824, 1, sel, style);
        ca_item_ctor(F + 0xec, 0xc, 0, 0x13, 0x9f, 0, 0, 0x005d0e98, 2, sel, style);
        ca_item_ctor(F + 0x124, 0xc, 0, 0x92, 0x9f, 0, 0, 0x005d150c, 3, sel, style);
        ca_item_ctor(F + 0x15c, 0xc, 0, 0x13, 0xf4, 0, 0, 0x005d1b80, 4, sel, style);
        put14(F, 0x194, 0xc, 0, 0x92, 0xf4, 0, 0, 0x005d21f4, 5, sel, (uint32_t)style);
        ca_item_ctor(F + 0x1cc, 0xc, 0, 0x13, 0x149, 0, 0, 0x005d2868, 6, sel, style);
        put14(F, 0x204, 0xc, 0, 0x92, 0x149, 0, 0, 0x005d2edc, 7, sel, (uint32_t)style);
        ca_item_ctor(F + 0x23c, 0xc, 0, 0x92, 0x4a, 0, 0, 0x005d0824, 1, sel, style);
        ca_item_ctor(F + 0x274, 0x17, 0, 0x139, 0xc1, 0xa5, 0x86, 0x004e4db8, 0, CA_A(F, 0x58), 0);   // the CareerBlurb
        put14(F, 0x2ac, 2, 0xffffffffu, 0x13, 0x1a2, 0, 0, ca_xlate(0x005d00b8), 0, 0, 6);            // Back: -1
        ca_item_ctor(F + 0x2e4, 1, 0, 0, 0, 0, 0, 0, 0, S_CH_GROUP_EMPTY, 0);                         // an empty slot's group
        ca_item_ctor(F + 0x31c, 2, -2, 0x1e5, 0x1a5, 0, 0, ca_xlate(0x005d3588), 0, F_create_cb, 2);  // Create
        ca_item_ctor(F + 0x354, 1, 1, 0, 0, 0, 0, 0, 0, S_CH_GROUP_EMPTY, 0);
        ca_item_ctor(F + 0x38c, 1, 0, 0, 0, 0, 0, 0, 0, S_CH_GROUP_USED, 0);                          // a used slot's group
        put14(F, 0x3c4, 2, 0xfffffffeu, 0x1e5, 0x1a5, 0, 0, ca_xlate(0x005d00f0), 0, 0, 2);           // Load: -2
        ca_item_ctor(F + 0x3fc, 2, -0x80, 0x154, 0x1a5, 0, 0, ca_xlate(0x005d0178), 0, F_delete_cb, 2);   // Delete: -0x80
        ca_item_ctor(F + 0x434, 1, 1, 0, 0, 0, 0, 0, 0, S_CH_GROUP_USED, 0);
        end_item(F, 0x46c);
        CA_W(F, 0x10) = ca_xlate(0x005d0160);
        CA_W(F, 0x1c) = CA_A(F, 0x7c);
        CA_W(F, 0x14) = 0x004ffac8;                                  // "chooser.stp"
        CA_W(F, 0x18) = 0xfffffffeu;
        CA_W(F, 0x20) = 0;
        ccall<void>(F_refresh);
        r = do_dialog(F + 0x10, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H));
        if (r != -0x80) break;                                        // Delete: the chooser again
    }
    ccall<void>(F_UIRemoveStyle, style);
    void* list = UI_GP(void, S_CH_LIST);
    if (list) {
        tcall<void>(uit::F_UIStringList_dtor, list);
        ccall<void>(uit::F_Delete, list);
    }
    ccall<void>(F_OptionsSetI, *(const char* const volatile*)(uintptr_t)0x004dfa5c, CA_CP(0x004ffad4), CA_G32(S_CH_SLOT));
    if (r == -2) {
        const int32_t slot = CA_G32(S_CH_SLOT);
        tcall<void>(F_CareerSummary_dtor, (void*)(F + 0x58));
        return slot;
    }
    tcall<void>(F_CareerSummary_dtor, (void*)(F + 0x58));
    return -1;
}
PORT_FN(0x004be7b0, "CareerChooser", CareerChooser_c, fp_screen)

static uint8_t __cdecl delete_cb_c(int32_t) {
    ccall<void>(F_CareerDestroySlot, CA_G32(S_CH_SLOT));
    ccall<void>(F_refresh);
    return 1;
}
PORT_FN(0x004beec0, "delete_cb(chooser.obj)", delete_cb_c, fp_screen_i)

// create_cb: the new career's dialog; Create with a name: the slot's CareerInfo made and saved
// FIX: the default name (Career:DefaultPlayerName's translation) was copied into the frame's 16-byte name unbounded. A
// translation over 15 characters ran into the dialog and its items, which are built over it afterwards, so with no saved
// player_name to replace it the name was left unterminated: the name field showed the dialog's bytes after it, and Create
// saved them into the new career's name; about 600 characters reached the return address. It's cut to 15 characters, as
// the main menu's default name is (menu_race.cpp). One that fits is copied as before.
static uint8_t __cdecl create_cb_c(int32_t) {
    alignas(8) uint8_t F[0x26c];                                    // sub esp, 0x25c; 4 pushes
    // F+0x13 damage (u8), +0x14 realism, +0x18 the name (0x10), +0x28 the UIDialog, +0x3c the items
    uint8_t* slot = (uint8_t*)(uintptr_t)(S_CH_INFOS + (uint32_t)CA_G32(S_CH_SLOT) * S_INFO_SIZE);
    ca_xl_once(0x005d00b0, 0x01, 0x005d00e0, 0x004ffae0, 0x004bf6b0);  // Titles:NewCareer
    ca_xl_once(0x005d00b0, 0x02, 0x005d0128, 0x004ffaf4, 0x004bf6a0);  // Career:New:PlayerName
    ca_xl_once(0x005d00b0, 0x04, 0x005d00c8, 0x004ffb0c, 0x004bf690);  // Career:New:Realism
    ca_xl_once(0x005d00b0, 0x08, 0x005d0058, 0x004ffb20, 0x004bf680);  // Career:New:Create
    ca_xl_once(0x005d00b0, 0x10, 0x005d0048, 0x004ffb34, 0x004bf670);  // UI:Back
    ca_xl_once(0x005d00b0, 0x20, 0x005d0068, 0x004ffb3c, 0x004bf660);  // Career:DefaultPlayerName
    ca_xl_once(0x005d00b0, 0x40, 0x005d0188, 0x004ffb58, 0x004bf650);  // UI:Cancel
    ca_xl_once(0x005d00b0, 0x80, 0x005d0080, 0x004ffb64, 0x004bf640);  // Career:Damage
    CA_W(F, 0x14) = 1;
    CA_B(F, 0x13) = 1;
    if (VP_FIX) ui_copy_bounded((char*)(F + 0x18), CA_CP(ca_xlate(0x005d0068)), 0x10);      // (FIX: above)
    else crt_strcpy((char*)(F + 0x18), CA_CP(ca_xlate(0x005d0068)));
    ccall<void>(F_OptionsGetS, *(const char* const volatile*)(uintptr_t)0x004dfa58, CA_CP(0x004ffb74), (char*)(F + 0x18), 0xd);
    {
        const char* game = *(const char* const volatile*)(uintptr_t)0x004dfa5c;
        ccall<void>(F_OptionsGetI, game, CA_CP(0x004ffb80), (void*)(F + 0x14));             // realism
        ccall<void>(F_OptionsGetB, game, CA_CP(0x004ffb88), (void*)(F + 0x13));             // damage_on
    }
    ca_item_ctor(F + 0x3c, 5, 0, 0x32, 0x1e, 0, 0, ca_xlate(0x005d0128), 0, 0, 0xb);           // Player name
    put14(F, 0x74, 9, 0, 0x3c, 0x32, 0x78, 0x10, 1, 0xd, CA_A(F, 0x18), 9);                  // the name's text field
    put14(F, 0xac, 5, 0, 0x32, 0x46, 0, 0, ca_xlate(0x005d00c8), 0, 0, 0xb);                   // Realism
    put14(F, 0xe4, 0xc, 0, 0x3c, 0x57, 0, 0, ccall<uint32_t>(F_GetRealismString, 0), 0, CA_A(F, 0x14), 8);
    put14(F, 0x11c, 0xc, 0, 0x3c, 0x66, 0, 0, ccall<uint32_t>(F_GetRealismString, 1), 1, CA_A(F, 0x14), 8);
    put14(F, 0x154, 0xc, 0, 0x3c, 0x75, 0, 0, ccall<uint32_t>(F_GetRealismString, 2), 2, CA_A(F, 0x14), 8);
    put14(F, 0x18c, 0xb, 0, 0x3c, 0x87, 0, 0, ca_xlate(0x005d0080), 0, CA_A(F, 0x13), 7);      // Damage
    ca_item_ctor(F + 0x1c4, 2, -2, 9, 0x99, 0, 0, ca_xlate(0x005d0058), 0, 0, 0);            // Create: -2
    put14(F, 0x1fc, 2, 0xffffffffu, 0xd7, 0x99, 0, 0, ca_xlate(0x005d0188), 0, 0, 0);          // Cancel: -1
    end_item(F, 0x234);
    CA_W(F, 0x28) = ca_xlate(0x005d00e0);
    CA_W(F, 0x34) = CA_A(F, 0x3c);
    CA_W(F, 0x38) = 0;
    CA_W(F, 0x2c) = 0x004ffb94;                                      // "dialog1.stp"
    CA_W(F, 0x30) = 0xfffffffeu;
    if (do_dialog(F + 0x28, 0x140, 0xc8) != -2 || CA_B(F, 0x18) == 0) return 0;
    zero_dwords(slot, 0x19d);
    crt_strcpy((char*)slot, (const char*)(F + 0x18));
    *(volatile uint8_t*)(slot + 0x1c) = 1;
    I32(slot, 0x10) = (int32_t)CA_W(F, 0x14);
    *(volatile uint8_t*)(slot + 0x14) = CA_B(F, 0x13);
    for (int32_t i = 0; i < 8; i++) I32(slot, 0x170 + 0x88 * (uint32_t)i) = i;
    ccall<void>(F_CareerSaveSlot, CA_G32(S_CH_SLOT), (void*)slot);
    return 1;
}
PORT_FN(0x004beee0, "create_cb", create_cb_c, fp_screen_i)

// refresh: every slot's career file read; the list of names ("(Empty)" for a slot without one)
static void __cdecl refresh_c() {
    volatile int32_t* list = UI_GP(volatile int32_t, S_CH_LIST);
    list[2] = 0;                                                     // the count
    list[3] = list[3] + 1;                                           // changes
    uint32_t e = S_CH_INFOS;
    for (uint32_t i = 0; e < S_CH_INFOS_END; e += S_INFO_SIZE, i++) {
        if (ccall<uint8_t>(F_CareerLoadSlot, (int32_t)i, (void*)(uintptr_t)e)) {
            tcall<void>(uit::F_UIStringList_AddEntry, UI_GP(void, S_CH_LIST), CA_CP(e));
            CA_G8(S_CH_USED + i) = 1;
        } else {
            tcall<void>(uit::F_UIStringList_AddEntry, UI_GP(void, S_CH_LIST), CA_CP(0x004ffba0));   // "(Empty)"
            zero_dwords((void*)(uintptr_t)e, 0x19d);
            CA_G8(S_CH_USED + i) = 0;
        }
    }
}
PORT_FN(0x004bf710, "refresh", refresh_c, fp_screen)

// ---- CareerSummary ----------------------------------------------------------------------------------------------------------
static CareerSummary* __fastcall CareerSummary_ctor(CareerSummary* self, Edx, const uint8_t* inf) {
    alignas(8) uint8_t F[0x6c];                                     // sub esp, 0x68; 1 push: +4 the labels' style, +0x38 the values'
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->info = inf;
    self->vtbl = (const void*)(uintptr_t)VT_CareerSummary;
    self->widget = 0;
    {
        const uint32_t cc4 = CA_GU32(0x005d00c4), cd4 = CA_GU32(0x005d00d4), cfc = CA_GU32(0x005d00fc);
        put_desc(F, 4, 0x004ffba8, 0x21, cc4, cd4, cfc, 0, 0, cc4, cc4, cfc, 0, 0, 0);       // euro10
        const uint32_t c78 = CA_GU32(0x005d0078);
        put_desc(F, 0x38, 0x004ffbb4, 0x21, c78, cd4, cfc, 0, 0, cc4, cc4, cfc, 0, 0, 0);    // ruby9
    }
    self->style = add_style(F, 4);
    self->style2 = add_style(F, 0x38);
    return self;
}
static void fp_CareerSummary_ctor(Footprint& f, CareerSummary*, Edx, const uint8_t*) { f.replay_only = "adds two dynamic styles (loads fonts)"; }
PORT_FN(0x004bf790, "CareerSummary::CareerSummary", CareerSummary_ctor, fp_CareerSummary_ctor)

static void __fastcall CareerSummary_dtor(CareerSummary* self, Edx) {
    const int32_t s = self->style;
    self->vtbl = (const void*)(uintptr_t)VT_CareerSummary;
    ccall<void>(F_UIRemoveStyle, s);
    ccall<void>(F_UIRemoveStyle, self->style2);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
PORT_FN(0x004bf880, "CareerSummary::~CareerSummary", CareerSummary_dtor, fp_styles0<CareerSummary>)

// draw_entry: the label in the first style, and the value after it in the second if the career has a name
static void __fastcall CareerSummary_draw_entry(CareerSummary* self, Edx, const char* label, const char* value, int32_t y) {
    const int32_t x = self->x + 5;
    const int32_t w = ccall<int32_t>(uit::F_UIStyleWidth, self->style, label);
    const int32_t x2 = x + w + 8;
    style_draw(self->style, x, y, ca_u(label), 0);
    if (*(const volatile uint8_t*)self->info != 0) style_draw(self->style2, x2, y, ca_u(value), 0);
}
static void fp_CareerSummary_draw_entry(Footprint& f, CareerSummary*, Edx, const char*, const char*, int32_t) {
    fp_draw_canvas(f, UI_GP(gxCanvas, S_GX_CANVAS));
}
PORT_FN(0x004bf8b0, "CareerSummary::draw_entry", CareerSummary_draw_entry, fp_CareerSummary_draw_entry)

static const uint32_t k_xl_summary[6] = {0x005d0090, 0x005d0140, 0x005d01a0, 0x005d00a0, 0x005d3550, 0x005d0150};
static void __fastcall CareerSummary_Draw(CareerSummary* self, Edx, gxCanvas* c) {
    alignas(8) uint8_t F[0x110];                                    // sub esp, 0x100; 4 pushes: +0x10 the text
    ca_xl_once(0x005d00b4, 0x01, 0x005d0090, 0x004ffbc0, 0x004bfc70);  // Career:Summary:Name
    ca_xl_once(0x005d00b4, 0x02, 0x005d0140, 0x004ffbd4, 0x004bfc60);  // ...:Class
    ca_xl_once(0x005d00b4, 0x04, 0x005d01a0, 0x004ffbec, 0x004bfc50);  // ...:Season
    ca_xl_once(0x005d00b4, 0x08, 0x005d00a0, 0x004ffc04, 0x004bfc40);  // ...:Week
    ca_xl_once(0x005d00b4, 0x10, 0x005d3550, 0x004ffc18, 0x004bfc30);  // ...:Hours
    ca_xl_once(0x005d00b4, 0x20, 0x005d0150, 0x004ffc30, 0x004bfc20);  // ...:Funds
    ccall<gxCanvas*>(uit::F_gxSetCanvas, c);
    int32_t y = self->y + 0xf;
    ca_xlate(0x005d0090);
    tcall<void>(F_CareerSummary_draw_entry, self, UI_GP(const char, 0x005d0094), (const char*)self->info, y);
    {
        const int32_t x = self->x;
        const uint32_t col = CA_GU32(0x005d0120);
        const int32_t x1 = self->w + x - 3;
        ccall<void>(uit::F_gxLine, x + 3, y + 4, x1, y + 4, col);
    }
    y += 0x19;
    ca_xlate(0x005d0140);
    {
        const char* cls = ccall<const char*>(F_CareerGetClassName, I32(self->info, 0x124));
        tcall<void>(F_CareerSummary_draw_entry, self, UI_GP(const char, 0x005d0144), cls, y);
    }
    y += 0x14;
    SPRINTF((char*)(F + 0x10), CA_CP(0x004ffc48), I32(self->info, 0x11c) + 1);            // "%d" the season
    ca_xlate(0x005d01a0);
    tcall<void>(F_CareerSummary_draw_entry, self, UI_GP(const char, 0x005d01a4), (const char*)(F + 0x10), y);
    y += 0x14;
    SPRINTF((char*)(F + 0x10), CA_CP(0x004ffc4c), I32(self->info, 0x120) + 1);            // "%d" the week
    ca_xlate(0x005d00a0);
    tcall<void>(F_CareerSummary_draw_entry, self, UI_GP(const char, 0x005d00a4), (const char*)(F + 0x10), y);
    y += 0x14;
    {
        const int32_t s = I32(self->info, 0x630);                                              // seconds raced
        SPRINTF((char*)(F + 0x10), CA_CP(0x004ffc50), s / 0xe10, (s / 0x3c) % 0x3c);            // "%d:%02d"
    }
    ca_xlate(0x005d3550);
    tcall<void>(F_CareerSummary_draw_entry, self, UI_GP(const char, 0x005d3554), (const char*)(F + 0x10), y);
    y += 0x14;
    ccall<void>(F_LocaleMoney, (char*)(F + 0x10), I32(self->info, 0x18), 0);
    ca_xlate(0x005d0150);
    tcall<void>(F_CareerSummary_draw_entry, self, UI_GP(const char, 0x005d0154), (const char*)(F + 0x10), y);
}
static void fp_CareerSummary_Draw(Footprint& f, CareerSummary*, Edx, gxCanvas* c) {
    if (!ca_xl_built(0x005d00b4, 0x3f) || !ca_xl_built(0x005cfe64, 0x0f)) {   // (and CareerGetClassName's)
        f.replay_only = "builds function-local Xlators (atexit)";
        return;
    }
    fp_draw_canvas(f, c);
    for (uint32_t a : k_xl_summary) CA_FP_XL(f, a);
    static const uint32_t k_xl_class[4] = {0x005d0038, 0x005cfe28, 0x005cf068, 0x005cf6f0};   // the class's name
    for (uint32_t a : k_xl_class) CA_FP_XL(f, a);
}
PORT_FN(0x004bf910, "CareerSummary::Draw", CareerSummary_Draw, fp_CareerSummary_Draw)

// ---- CareerBlurb ------------------------------------------------------------------------------------------------------------
static void __fastcall CareerBlurb_Create(CareerSummary* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, 0, (const void*)(uintptr_t)S_CH_SLOT, 4u);
    tcall<void>(F_UICC_AddNotification, self, 0, (const void*)(uintptr_t)S_CH_INFOS, 0x33a0u);
    vcall<void>(self, 0x10, 0, (const void*)0);
}
PORT_FN(0x004bfc80, "CareerBlurb::Create", CareerBlurb_Create, fp_notes0<CareerSummary>)

// Callback: the selected slot's CareerInfo shown; its groups: Load / Delete for a career, Create for an empty slot
static void __fastcall CareerBlurb_Callback(CareerSummary* self, Edx, int32_t, const void*) {
    self->info = (const uint8_t*)(uintptr_t)(S_CH_INFOS + (uint32_t)CA_G32(S_CH_SLOT) * S_INFO_SIZE);
    tcall<void>(F_UICC_Dirty, self);
    const int32_t slot = CA_G32(S_CH_SLOT);
    const uint32_t empty = CA_GU32(S_CH_GROUP_EMPTY);
    if (CA_G8(S_CH_USED + (uint32_t)slot) != 0) {
        ccall<void>(F_UIHideGroup, empty);
        ccall<void>(F_UIShowGroup, CA_GU32(S_CH_GROUP_USED));
    } else {
        ccall<void>(F_UIShowGroup, empty);
        ccall<void>(F_UIHideGroup, CA_GU32(S_CH_GROUP_USED));
    }
}
static void fp_CareerBlurb_Callback(Footprint& f, CareerSummary* self, Edx, int32_t, const void*) {
    f.add(self, sizeof(CareerSummary), "the CareerBlurb");
    if (self->widget) UI_FP_WIDGET(f, self->widget, "its widget");
    ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));
}
PORT_FN(0x004bfcc0, "CareerBlurb::Callback", CareerBlurb_Callback, fp_CareerBlurb_Callback)

// =========================================================================================================================
// events.obj
// =========================================================================================================================
// FIX CANDIDATE: its CareerStatus is added before its texts are formatted (career_main.cpp, career_menu).
// (FIX: its two texts, below.)
static uint8_t __cdecl EventsDo_c() {
    alignas(8) uint8_t F[0x430];                                    // sub esp, 0x424; 3 pushes
    // F+0xc the UIDialog, +0x20 the items, +0x138 the SeasonViewer, +0x158 the CareerStatus, +0x230 / +0x330 two texts
    tcall<void*>(F_SeasonViewer_ctor, (void*)(F + 0x138));
    CA_W(F, 0x16c) = 0;
    CA_W(F, 0x158) = VT_CareerStatus;
    ca_xl_once(0x005d360c, 0x01, 0x005d35d8, 0x004ffce8, 0x004c0470);  // UI:Back
    ca_xl_once(0x005d360c, 0x02, 0x005d35a0, 0x004ffcf0, 0x004c0460);  // Career:Summary:Season
    ca_xl_once(0x005d360c, 0x04, 0x005d35c0, 0x004ffd08, 0x004c0450);  // Career:Summary:Week
    ca_xl_once(0x005d360c, 0x08, 0x005d3610, 0x004ffd1c, 0x004c0440);  // Career:Events:Enter
    // FIX: "<Season> <n>" and "<Week> <n>" (Career:Summary's translations) were formatted into two 0x100-byte texts of the
    // frame unbounded, the second of them the frame's last bytes, so a translation of about 250 characters ran
    // over the saved registers and the return address. Each keeps its first 255 characters (nothing reads them). A text
    // that fits is formatted as before.
    ca_xlate(0x005d35a0);
    {
        const int32_t n = I32(info(), 0x11c) + 1;
        const char* s = UI_GP(const char, 0x005d35a4);
        if (VP_FIX && ca_sd_long(s, n, 0x100)) ca_fix_sd((char*)(F + 0x330), 0x100, 0x004ffd30, s, n, false);
        else SPRINTF((char*)(F + 0x330), CA_CP(0x004ffd30), s, n);                             // "%s %d"
    }
    ca_xlate(0x005d35c0);
    {
        const int32_t n = I32(info(), 0x120) + 1;
        const char* s = UI_GP(const char, 0x005d35c4);
        if (VP_FIX && ca_sd_long(s, n, 0x100)) ca_fix_sd((char*)(F + 0x230), 0x100, 0x004ffd38, s, n, false);
        else SPRINTF((char*)(F + 0x230), CA_CP(0x004ffd38), s, n);
    }
    put14(F, 0x20, 0x17, 0, 0, 0, 0, 0, 0x004e4db8, 0, CA_A(F, 0x158), 0);                   // the CareerStatus
    put14(F, 0x58, 0x17, 0, 0x32, 0x78, 0x208, 0x122, 0x004e4db8, 0, CA_A(F, 0x138), 0);      // the SeasonViewer
    ca_item_ctor(F + 0x90, 2, -2, 0x1e5, 0x1a5, 0, 0, ca_xlate(0x005d3610), 0, 0, 2);         // Enter: -2
    put14(F, 0xc8, 2, 0xffffffffu, 0x13, 0x1a2, 0, 0, ca_xlate(0x005d35d8), 0, 0, 6);          // Back: -1
    end_item(F, 0x100);
    CA_W(F, 0xc) = ccall<uint32_t>(F_CareerGetClassName, I32(info(), 0x124));
    CA_W(F, 0x1c) = 0;
    CA_W(F, 0x18) = CA_A(F, 0x20);
    CA_W(F, 0x10) = 0x004ffd40;                                      // "calendar.stp"
    CA_W(F, 0x14) = 0xfffffffeu;
    const int32_t r = do_dialog(F + 0xc, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H));
    const int32_t s = (int32_t)CA_W(F, 0x150);
    CA_W(F, 0x158) = VT_UICustomControl;                             // the inlined destructors
    CA_W(F, 0x138) = VT_SeasonViewer;
    ccall<void>(F_UIRemoveStyle, s);
    ccall<void>(F_UIRemoveStyle, (int32_t)CA_W(F, 0x154));
    return (uint8_t)(r == -2 ? 1 : 0);
}
PORT_FN(0x004c00a0, "EventsDo", EventsDo_c, fp_screen)

static TwoStyles* __fastcall SeasonViewer_ctor(TwoStyles* self, Edx) {
    alignas(8) uint8_t F[0x70];                                     // sub esp, 0x68; 2 pushes: +8, +0x3c the styles
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_SeasonViewer;
    const uint32_t a = CA_GU32(0x005d35ac), b = CA_GU32(0x005d35b0);
    put_desc(F, 8, 0x004ffd5c, 0x21, a, b, 0xb5b65ad6u, 0, 0, a, a, 0x8c714631u, 0, 0, 0);   // euro10
    const uint32_t c = CA_GU32(0x005d3598), d = CA_GU32(0x005d35d0);
    put_desc(F, 0x3c, 0x004ffd50, 0x21, c, b, 0xb5b65ad6u, 0, 0, a, a, d, 0, 0, 0);           // ruby9
    self->style = add_style(F, 8);
    self->style2 = add_style(F, 0x3c);
    return self;
}
PORT_FN(0x004c0480, "SeasonViewer::SeasonViewer", SeasonViewer_ctor, fp_styles0<TwoStyles>)

static void __fastcall SeasonViewer_Callback(TwoStyles* self, Edx, int32_t, const void*) { tcall<void>(F_UICC_Dirty, self); }
PORT_FN(0x004c0570, "SeasonViewer::Callback", SeasonViewer_Callback, fp_dirty<TwoStyles>)

// SeasonViewer::Draw: the header, then each event (the next highlighted): the track, its laps, and for those run the
// player's purse, place and points; then the totals
// FIX CANDIDATE: the player's place (from the career file) indexes the purse (from 1) and the place names unchecked (a
// damaged career file reads past them). Not in ordinary play.
// FIX: SeasonViewer::Draw formats "<n> <Laps>" and "<n> <Pts>" (Career:Season's translations) into the frame's last 0x100
// bytes (+0x88), and a translation of about 250 characters ran over its saved registers and return address. The text
// keeps its first 255 characters (it's only drawn). One that fits is formatted as before.
static __forceinline void season_text(uint8_t* F, int32_t n, const char* s) {
    if (VP_FIX && ca_sd_long(s, n, 0x100)) ca_fix_sd((char*)(F + 0x88), 0x100, 0x004ffd68, s, n, true);
    else SPRINTF((char*)(F + 0x88), CA_CP(0x004ffd68), n, s);                                 // "%d %s"
}
static const uint32_t k_xl_season[7] = {0x005d5d90, 0x005d5db0, 0x005d5da0, 0x005d5d50, 0x005d5d60, 0x005d5d70, 0x005d5d80};
static void __fastcall SeasonViewer_Draw(TwoStyles* self, Edx, gxCanvas* c) {
    alignas(8) uint8_t F[0x188];                                    // sub esp, 0x178; 4 pushes
    // F+0x10 x0, +0x14 the event, +0x18 its CareerEvent, +0x1c/+0x40/+0x20/+0x28/+0x24 the columns (track, length, purse,
    // place, points), +0x2c the event * 22, +0x30 the event * 4, +0x34 the event * 0x58, +0x38 the purse's total, +0x3c the
    // points', +0x40 the English place names (then the length column), +0x64 their translations, +0x88 a text
    ccall<gxCanvas*>(uit::F_gxSetCanvas, c);
    int32_t y = self->y + 0x23;
    CA_W(F, 0x10) = (uint32_t)(self->x + 0x19);
    CA_W(F, 0x3c) = 0;
    static const uint32_t k_places[9] = {0x004ffe40, 0x004ffe3c, 0x004ffe38, 0x004ffe34, 0x004ffe30, 0x004ffe2c, 0x004ffe28,
                                         0x004ffe24, 0x004ffe20};   // "DNF", "1st" .. "8th"
    for (uint32_t k = 0; k < 9; k++) CA_W(F, 0x40 + 4 * k) = k_places[k];
    CA_W(F, 0x38) = 0;
    for (uint32_t k = 0; k < 9; k++) {
        SPRINTF((char*)(F + 0x88), CA_CP(0x004ffe0c), CA_CP(CA_W(F, 0x40 + 4 * k)));          // "Career:Season:%s"
        CA_W(F, 0x64 + 4 * k) = ccall<uint32_t>(F_Xlate, (const char*)(F + 0x88));
    }
    ca_xl_once(0x005d5d4c, 0x01, 0x005d5d90, 0x004ffdf8, 0x004c0c00);  // Career:Season:Laps
    ca_xl_once(0x005d5d4c, 0x02, 0x005d5db0, 0x004ffdd8, 0x004c0bf0);  // ...:PointsAbbreviated
    ca_xl_once(0x005d5d4c, 0x04, 0x005d5da0, 0x004ffdc4, 0x004c0be0);  // ...:Total
    ca_xl_once(0x005d5d4c, 0x08, 0x005d5d50, 0x004ffdb0, 0x004c0bd0);  // ...:Track
    ca_xl_once(0x005d5d4c, 0x10, 0x005d5d60, 0x004ffd98, 0x004c0bc0);  // ...:Length
    ca_xl_once(0x005d5d4c, 0x20, 0x005d5d70, 0x004ffd84, 0x004c0bb0);  // ...:Purse
    ca_xl_once(0x005d5d4c, 0x40, 0x005d5d80, 0x004ffd70, 0x004c0ba0);  // ...:Place
    const int32_t x0 = (int32_t)CA_W(F, 0x10);
    ca_xlate(0x005d5d50);
    CA_W(F, 0x1c) = (uint32_t)(x0 + 0xa);
    style_draw(self->style, x0 + 0xa, y, CA_GU32(0x005d5d54), 1);
    ca_xlate(0x005d5d60);
    CA_W(F, 0x40) = (uint32_t)(x0 + 0xa0);
    style_draw(self->style, x0 + 0xa0, y, CA_GU32(0x005d5d64), 1);
    ca_xlate(0x005d5d70);
    CA_W(F, 0x20) = (uint32_t)(x0 + 0xf5);
    style_draw(self->style, x0 + 0xf5, y, CA_GU32(0x005d5d74), 1);
    ca_xlate(0x005d5d80);
    CA_W(F, 0x28) = (uint32_t)(x0 + 0x159);
    style_draw(self->style, x0 + 0x159, y, CA_GU32(0x005d5d84), 1);
    ca_xlate(0x005d5db0);
    CA_W(F, 0x24) = (uint32_t)(x0 + 0x1a4);
    {
        const int32_t yy = y;
        y += 0x14;
        style_draw(self->style, x0 + 0x1a4, yy, CA_GU32(0x005d5db4), 1);
    }
    CA_W(F, 0x14) = 0;
    if (*(const volatile int32_t*)season() > 0) {
        CA_W(F, 0x2c) = 0; CA_W(F, 0x30) = 0; CA_W(F, 0x34) = 0;
        do {
            {
                const uint8_t* s = season();
                CA_W(F, 0x18) = CA_W(F, 0x34) + ca_u(s) + 0x24;
            }
            if (I32(info(), 0x128) == (int32_t)CA_W(F, 0x14)) {      // the next event: highlighted
                const uint32_t col = CA_GU32(0x005d3598);
                ccall<void>(uit::F_gxRect, (int32_t)CA_W(F, 0x10) - 8, y - 5, (int32_t)CA_W(F, 0x10) - 1, y + 2, col);
            }
            const int32_t tn = ccall<int32_t>(F_GetTrackNumber, CA_CP(CA_W(F, 0x18)));
            const uint32_t fl = (I32(info(), 0x128) - (int32_t)CA_W(F, 0x14)) == 0 ? 2u : 0u;
            {
                const char* name = ccall<const char*>(F_GetTrackFriendlyName, tn);
                style_draw(self->style, (int32_t)CA_W(F, 0x1c), y, ca_u(name), fl);
            }
            ca_xlate(0x005d5d90);
            season_text(F, I32(CA_W(F, 0x18), 0x14), UI_GP(const char, 0x005d5d94));        // FIX: (season_text) "%d %s"
            style_draw(self->style, (int32_t)CA_W(F, 0x40), y, CA_A(F, 0x88), fl);
            if (I32(info(), 0x128) > (int32_t)CA_W(F, 0x14)) {       // run: the player's purse, place, points
                const int32_t place = *(volatile int32_t*)(info() + CA_W(F, 0x30) + 0x530);
                int32_t purse = 0;
                if ((int32_t)((uint32_t)place - 1u) >= 0) purse = I32(CA_W(F, 0x18), (uint32_t)place * 4u + 0x14);
                CA_W(F, 0x38) = CA_W(F, 0x38) + (uint32_t)purse;
                ccall<void>(F_LocaleMoney, (char*)(F + 0x88), purse, 0);
                style_draw(self->style2, (int32_t)CA_W(F, 0x20), y, CA_A(F, 0x88), fl);
                style_draw(self->style2, (int32_t)CA_W(F, 0x28), y, CA_W(F, 0x64 + (uint32_t)place * 4u), 0);
                int32_t pts = 0;
                const int32_t pm1 = (int32_t)((uint32_t)place - 1u);
                if (pm1 >= 0 && pm1 < 8) {
                    const uint32_t k = CA_W(F, 0x2c);
                    pts = *(const volatile int32_t*)(season() + (k + (uint32_t)place) * 4u + 0x58);
                }
                CA_W(F, 0x3c) = CA_W(F, 0x3c) + (uint32_t)pts;
                ca_xlate(0x005d5db0);
                season_text(F, pts, UI_GP(const char, 0x005d5db4));                           // FIX: (season_text)
                style_draw(self->style2, (int32_t)CA_W(F, 0x24), y, CA_A(F, 0x88), 0);
            }
            y += 0x14;
            CA_W(F, 0x14) = CA_W(F, 0x14) + 1;
            CA_W(F, 0x2c) = CA_W(F, 0x2c) + 0x16;
            CA_W(F, 0x30) = CA_W(F, 0x30) + 4;
            CA_W(F, 0x34) = CA_W(F, 0x34) + 0x58;
        } while (*(const volatile int32_t*)season() > (int32_t)CA_W(F, 0x14));
    }
    y = self->y + 0xeb;
    {
        const uint32_t col = CA_GU32(0x005d35ac);
        ccall<void>(uit::F_gxLine, (int32_t)CA_W(F, 0x10) + 5, y - 0xe, (int32_t)CA_W(F, 0x10) + 0x1f9, y - 0xe, col);
    }
    ca_xlate(0x005d5da0);
    style_draw(self->style, (int32_t)CA_W(F, 0x1c), y, CA_GU32(0x005d5da4), 0);
    ccall<void>(F_LocaleMoney, (char*)(F + 0x88), (int32_t)CA_W(F, 0x38), 0);
    style_draw(self->style2, (int32_t)CA_W(F, 0x20), y, CA_A(F, 0x88), 0);
    ca_xlate(0x005d5db0);
    season_text(F, (int32_t)CA_W(F, 0x3c), UI_GP(const char, 0x005d5db4));                   // FIX: (season_text)
    style_draw(self->style2, (int32_t)CA_W(F, 0x24), y, CA_A(F, 0x88), 0);
}
static void fp_SeasonViewer_Draw(Footprint& f, TwoStyles*, Edx, gxCanvas* c) {
    if (!ca_xl_built(0x005d5d4c, 0x7f)) { f.replay_only = "builds its function-local Xlators (atexit)"; return; }
    fp_draw_canvas(f, c);
    for (uint32_t a : k_xl_season) CA_FP_XL(f, a);
}
PORT_FN(0x004c0580, "SeasonViewer::Draw", SeasonViewer_Draw, fp_SeasonViewer_Draw)

// the deleting destructors with the destructor inlined (SeasonViewer's and StandingsViewer's)
static __forceinline void* two_styles_delete(TwoStyles* self, uint32_t vt, uint32_t flags) {
    const int32_t s = self->style;
    self->vtbl = (const void*)(uintptr_t)vt;
    ccall<void>(F_UIRemoveStyle, s);
    ccall<void>(F_UIRemoveStyle, self->style2);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(uit::F_Delete, (void*)self);
    return self;
}
static void* __fastcall SeasonViewer_sdd(TwoStyles* self, Edx, uint32_t flags) { return two_styles_delete(self, VT_SeasonViewer, flags); }
PORT_FN(0x004c0c10, "SeasonViewer::scalar deleting destructor", SeasonViewer_sdd, fp_dtor_flags<TwoStyles>)

// =========================================================================================================================
// testing.obj
// =========================================================================================================================
// TestingDo: in assembly, its frame exactly the original's (sub esp, 0x24: +3 reversed (u8), +4 the track (0x20)), so the
// menu and the race run at the original's depths. The menu's list (a UIStringList on its stack) leaves its change count
// uninitialised (UIStringList's constructor doesn't set it) and a list box watches it: the second menu reads what the
// race's frame left there, which only the original's layout reproduces. (Calls by address through eax, free at each.)
static __declspec(naked) void __cdecl TestingDo_c() {
    __asm {
        sub esp, 0x24
        lea eax, [esp + 3]
        lea ecx, [esp + 4]
        push eax
        push ecx
        mov eax, 0x004c1040                                          // testing_menu(track, &reversed)
        call eax
        add esp, 8
        test al, al
        je done
    again:
        mov eax, dword ptr [esp + 3]                                 // (a dword read from +3: the u8 argument)
        lea ecx, [esp + 4]
        push eax
        push ecx
        mov eax, 0x004c0ec0                                          // do_race(track, reversed)
        call eax
        lea ecx, [esp + 0xb]
        lea edx, [esp + 0xc]
        add esp, 8
        push ecx
        push edx
        mov eax, 0x004c1040
        call eax
        add esp, 8
        test al, al
        jne again
    done:
        add esp, 0x24
        ret
    }
}
PORT_FN(0x004c0e70, "TestingDo", TestingDo_c, fp_screen)

// do_race: a practice race on the track, the career's car and class
// FIX: the track's name was copied into the World's 32 bytes unbounded, so a name of 32 or more characters ran into its
// cars (the car list is written over it afterwards, leaving the World's track unterminated for the race's loading). Cut,
// the name would no longer find the track's files, and the
// World can't hold it, so there's no race: do_race returns at once and the testing menu comes back. (testing_menu's own
// fix already keeps such a name from reaching here.) A name that fits races as before.
static void __cdecl do_race_c(const char* track, uint32_t rev) {
    alignas(8) uint8_t F[0xce4];                                    // sub esp, 0xcd8; 3 pushes: +0xf done (u8), +0x10 the World
    if (VP_FIX && ca_longer(track, 0x1f)) return;
    tcall<void*>(F_World_ctor, (void*)(F + 0x10));
    crt_strcpy((char*)(F + 0x18), track);
    CA_W(F, 0xcbc) = (uint32_t)I32(info(), 0x10);                    // realism
    {
        const uint8_t dmg = *(volatile uint8_t*)(info() + 0x14);
        CA_B(F, 0xce0) = dmg;                                        // damage
        CA_B(F, 0xce1) = (uint8_t)rev;                               // reversed
    }
    CA_W(F, 0xcd0) = 0x14;                                           // laps
    CA_W(F, 0xccc) = (uint32_t)I32(info(), 0x124) + 4;               // AI strength
    CA_W(F, 0xcc8) = 4;                                              // the race type
    CA_W(F, 0xcdc) = (uint32_t)I32(info(), 0x124) + 3;
    ccall<void>(F_CareerUnload);
    ccall<void>(F_AIResetDriverMap);
    ccall<void>(F_GenerateCarList, (void*)(F + 0x10), ccall<const char*>(F_CareerGetCarName), 0);
    crt_strcpy((char*)(F + 0x3c), (const char*)info());              // car 0's driver: the player
    CA_W(F, 0xf8) = 0xfffffff7u;                                     // its paint job
    ccall<void>(F_LoadRace, (void*)(F + 0x10));
    uint32_t garage = 0;                                             // (ebx: its low byte set)
    CA_B(F, 0xf) = 0;
    do {
        const int32_t r = ccall<int32_t>(F_PreRaceDo, CA_A(F, 0x10), garage, ca_u(info() + 0x1c), 0u, 0u, 0u);
        if (r == 1) {
            garage &= 0xffffff00u;
            ccall<void>(F_DoRace, (void*)(F + 0x10), 0, 0);
        } else if (r == 2) {
            garage = (garage & 0xffffff00u) | 1u;
            ccall<void>(F_DoRace, (void*)(F + 0x10), 0, 0);
        } else {
            CA_B(F, 0xf) = 1;
        }
    } while (CA_B(F, 0xf) == 0);
    ccall<void>(F_UnloadRace, (const void*)(F + 0x10));
    ccall<void>(F_CareerReload);
}
static void fp_do_race(Footprint& f, const char*, uint32_t) { f.replay_only = "runs a practice race (the pre-race screen, the race)"; }
PORT_FN(0x004c0ec0, "do_race", do_race_c, fp_do_race)

// FIX: Test copied the track's name (tracks.tab's, one of its first eight rows) into the caller's buffer, TestingDo's 32
// bytes, unbounded: a name of 32 or more characters ran over TestingDo's return address (a crash on leaving the testing
// menu). Cut, the name would no longer find the track's files, and the World a race is loaded from holds 31 characters,
// so such a track can't be tested: Test on it leaves the menu as Back does (back to the career's menu; the reversed flag
// is set first, as before, and read by nothing then). A name that fits is copied as before.
// FIX CANDIDATE: its CareerStatus is added before its texts are formatted (career_main.cpp, career_menu), and its list's
// change count is left uninitialised (UIStringList's constructor doesn't set it; the list box only compares it:
// harmless). Both every time the menu opens.
static uint8_t __cdecl testing_menu_c(char* track, uint8_t* rev) {
    alignas(8) uint8_t F[0x2ac];                                    // sub esp, 0x2a4; 2 pushes
    // F+8 the UIDialog, +0x1c the tracks' list (UIStringList), +0x30 the TrackImage, +0x4c the items, +0x1d4 the CareerStatus
    ca_xl_once(0x005d3658, 0x01, 0x005d3668, 0x004ffe84, 0x004c1500);  // Career:TestingMenu:Reversed
    ca_xl_once(0x005d3658, 0x02, 0x005d3648, 0x004ffea0, 0x004c14f0);  // ...:Title
    ca_xl_once(0x005d3658, 0x04, 0x005d3680, 0x004ffebc, 0x004c14e0);  // ...:Test
    ca_xl_once(0x005d3658, 0x08, 0x005d3628, 0x004ffed4, 0x004c14d0);  // UI:Back
    tcall<void*>(uit::F_UIStringList_ctor, (void*)(F + 0x1c), 8, 0x20);
    CA_W(F, 0x30) = VT_TrackImage;
    CA_W(F, 0x48) = S_TS_TRACK;
    CA_W(F, 0x44) = 0;
    for (int32_t i = 0; i < 8; i++)
        tcall<void>(uit::F_UIStringList_AddEntry, (void*)(F + 0x1c), ccall<const char*>(F_GetTrackFriendlyName, i));
    CA_W(F, 0x1d4) = VT_CareerStatus;
    CA_W(F, 0x1e8) = 0;
    ca_item_ctor(F + 0x4c, 0x17, 0, 0, 0, 0, 0, 0x004e4db8, 0, CA_A(F, 0x1d4), 0);            // the CareerStatus
    put14(F, 0x84, 0x14, 0, 0x3c, 0x96, 0x96, 0x96, 0, 0, CA_A(F, 0x1c), 0, S_TS_TRACK);       // the tracks' list
    put14(F, 0xbc, 0x17, 0, 0x190, 0x96, 0x96, 0x96, 0x004e4db8, 0, CA_A(F, 0x30), 0);         // the TrackImage
    put14(F, 0xf4, 0xb, 0, 0x3c, 0x136, 0, 0, ca_xlate(0x005d3668), 0, S_TS_REVERSED, 7);      // Reversed
    put14(F, 0x12c, 2, 0xfffffffeu, 0x1e5, 0x1a5, 0, 0, ca_xlate(0x005d3680), 0, 0, 2);        // Test: -2
    ca_item_ctor(F + 0x164, 2, -1, 0x13, 0x1a2, 0, 0, ca_xlate(0x005d3628), 0, 0, 6);         // Back: -1
    end_item(F, 0x19c);
    CA_W(F, 8) = ca_xlate(0x005d3648);
    CA_W(F, 0x14) = CA_A(F, 0x4c);
    CA_W(F, 0xc) = 0x004ffedc;                                       // "main_t.stp"
    CA_W(F, 0x10) = 0xfffffffeu;
    CA_W(F, 0x18) = 0;
    if (do_dialog(F + 8, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H)) == -2) {
        *(volatile uint8_t*)rev = CA_G8(S_TS_REVERSED);
        const char* name = ccall<const char*>(F_GetTrackName, CA_G32(S_TS_TRACK));
        if (!(VP_FIX && ca_longer(name, 0x1f))) {                    // (FIX: above; too long: as Back, below)
            crt_strcpy(track, name);
            CA_W(F, 0x1d4) = VT_UICustomControl;
            CA_W(F, 0x30) = VT_UICustomControl;
            tcall<void>(uit::F_UIStringList_dtor, (void*)(F + 0x1c));
            return 1;
        }
    }
    CA_W(F, 0x1d4) = VT_UICustomControl;
    CA_W(F, 0x30) = VT_UICustomControl;
    tcall<void>(uit::F_UIStringList_dtor, (void*)(F + 0x1c));
    return 0;
}
static void fp_testing_menu(Footprint& f, char*, uint8_t*) { f.replay_only = "runs the testing menu (a dialog)"; }
PORT_FN(0x004c1040, "testing_menu", testing_menu_c, fp_testing_menu)

static void __fastcall TrackImage_Create(TrackImage* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, 0, (const void*)self->track, 4u);
}
PORT_FN(0x004c1510, "TrackImage::Create", TrackImage_Create, fp_notes0<TrackImage>)
static void __fastcall TrackImage_Callback(TrackImage* self, Edx, int32_t, const void*) { tcall<void>(F_UICC_Dirty, self); }
PORT_FN(0x004c1520, "TrackImage::Callback", TrackImage_Callback, fp_dirty<TrackImage>)

// FIX: "<track>.stp" (tracks.tab's name) was formatted into 0x50 bytes of the frame unbounded, so a name of 76 or more
// characters ran over the saved registers and the return address. Cut, the name would no longer be the track's picture
// (U2's track viewer cuts it, finding no picture), so a picture whose name doesn't fit isn't loaded or drawn: the testing
// menu shows no picture for that track, as for any track without one (gxGetStamp finds none; drawing none draws
// nothing). A name that fits is loaded and drawn as before.
static void __fastcall TrackImage_Draw(TrackImage* self, Edx, gxCanvas* c) {
    alignas(8) uint8_t F[0x58];                                     // sub esp, 0x50; 2 pushes: +8 the stamp's name
    ccall<gxCanvas*>(uit::F_gxSetCanvas, c);
    const char* tn = ccall<const char*>(F_GetTrackName, *(const volatile int32_t*)self->track);
    if (VP_FIX && ca_longer(tn, 0x4b)) return;                      // (FIX: above) 75 + ".stp" + 0 = 0x50
    SPRINTF((char*)(F + 8), CA_CP(0x004ffee8), tn);                // "%s.stp"
    void* st = ccall<void*>(uit::F_gxGetStamp, (const char*)(F + 8));
    ccall<void>(uit::F_gxDrawStamp, (const void*)st, self->x, self->y, 0, (const void*)0);
    ccall<void>(uit::F_gxForgetStamp, st);
}
static void fp_TrackImage_Draw(Footprint& f, TrackImage*, Edx, gxCanvas*) { f.replay_only = "loads and frees the track's stamp"; }
PORT_FN(0x004c1530, "TrackImage::Draw", TrackImage_Draw, fp_TrackImage_Draw)

// =========================================================================================================================
// postseas.obj
// =========================================================================================================================
// PostSeasonDo: the standings after a race (-1) or a season (the player's rank); Back runs the money screens
// FIX CANDIDATE: its CareerStatus is added before its texts are formatted (career_main.cpp, career_menu).
static void __cdecl PostSeasonDo_c(int32_t rank) {
    alignas(8) uint8_t F[0x1fc];                                    // sub esp, 0x1ec; 4 pushes
    // F+0x10 the UIDialog, +0x24 the items, +0x104 the StandingsViewer, +0x124 the CareerStatus
    ca_xl_once(0x005d3704, 0x01, 0x005d3730, 0x004fff28, 0x004c1e20);  // UI:Back
    const uint8_t season_end = rank >= 0 ? 1 : 0;
    CA_G8(S_PS_RACE_MONEY) = 1;
    CA_G32(S_PS_AMOUNT) = rank;
    tcall<void*>(F_StandingsViewer_ctor, (void*)(F + 0x104));
    CA_W(F, 0x138) = 0;
    put14(F, 0x24, 0x17, 0, 0, 0, 0, 0, 0x004e4db8, 0, CA_A(F, 0x124), 0);                   // the CareerStatus
    CA_W(F, 0x124) = VT_CareerStatus;
    put14(F, 0x5c, 2, 0xffffffffu, 0x13, 0x1a2, 0, 0, ca_xlate(0x005d3730), 0, F_show_season_money, 6);   // Back
    put14(F, 0x94, 0x17, 0, 0x32, 0x78, 0x208, 0x122, 0x004e4db8, 0, CA_A(F, 0x104), 0);     // the StandingsViewer
    end_item(F, 0xcc);
    ca_xl_once(0x005d3704, 0x02, 0x005d36e0, 0x004fff30, 0x004c1e10);  // Career:PostSeason:Title:SeasonResults
    ca_xl_once(0x005d3704, 0x04, 0x005d3710, 0x004fff58, 0x004c1e00);  // ...:RaceResults
    CA_W(F, 0x10) = season_end ? ca_xlate(0x005d36e0) : ca_xlate(0x005d3710);
    CA_W(F, 0x1c) = CA_A(F, 0x24);
    CA_W(F, 0x14) = 0x004fff7c;                                      // "calendar.stp"
    CA_W(F, 0x18) = 0xfffffffeu;
    CA_W(F, 0x20) = F_show_race_money;
    do_dialog(F + 0x10, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H));
    if (season_end && rank == 0) ccall<void>(F_show_season_win_splash);
    CA_W(F, 0x124) = VT_UICustomControl;                             // the inlined destructors
    CA_W(F, 0x104) = VT_StandingsViewer;
    ccall<void>(F_UIRemoveStyle, (int32_t)CA_W(F, 0x11c));
    ccall<void>(F_UIRemoveStyle, (int32_t)CA_W(F, 0x120));
}
PORT_FN(0x004c17e0, "PostSeasonDo", PostSeasonDo_c, fp_screen_i)

static uint8_t __cdecl show_race_money_c(int32_t*) {
    if (CA_G8(S_PS_RACE_MONEY) != 0) {
        CA_G8(S_PS_RACE_MONEY) = 0;
        ccall<void>(F_CreditsDo, -1);
    }
    return 0;
}
static void fp_show_race_money(Footprint& f, int32_t*) {
    if (CA_G8(S_PS_RACE_MONEY) != 0) { f.replay_only = "runs the race's money screen (CreditsDo)"; return; }
}
PORT_FN(0x004c1ae0, "show_race_money", show_race_money_c, fp_show_race_money)

// show_season_money (Back): at the season's end, the season's money screen -- unless it's the champion's (its splash runs
// it) and the class isn't the last
static uint8_t __cdecl show_season_money_c(int32_t) {
    const int32_t ev = I32(info(), 0x128);
    if (ev >= *(const volatile int32_t*)season()) {
        if (CA_G32(S_PS_AMOUNT) != 0 || I32(info(), 0x124) + 1 >= 4) ccall<void>(F_CreditsDo, CA_G32(S_PS_AMOUNT));
    }
    return 1;
}
static void fp_show_season_money(Footprint& f, int32_t) { f.replay_only = "may run the season's money screen (CreditsDo)"; }
PORT_FN(0x004c1b00, "show_season_money", show_season_money_c, fp_show_season_money)

// show_season_win_splash: the champion's splash ("<class> Champion"), closed by a click, a key or ten seconds
static void __cdecl show_season_win_splash_c() {
    alignas(8) uint8_t F[0x1f8];                                    // sub esp, 0x1e8; 4 pushes
    // F+0x10 the UIDialog, +0x24 the style, +0x58 the ClickThrough, +0x70 the items
    CA_G32(S_PS_SPLASH_TIME) = ccall<int32_t>(uit::F_PTimeNow);
    put_desc(F, 0x24, 0x004fff8c, 0xc, 0x18a30c43u, 0x18a30c43u, 0x4a892549u, 0, 0, 0x18a30c43u, 0x18a30c43u, 0x4a892549u, 0, 0, 0);
    const int32_t style = add_style(F, 0x24);                                                    // engr.fnt
    const int32_t h = ccall<int32_t>(uit::F_UIStyleHeight, style, CA_CP(0x004fff98));          // "HI"
    const int32_t q = (0x3d - h * 2) / 3;
    CA_W(F, 0x6c) = 0;                                               // the ClickThrough: its widget, its vtable
    CA_G8(S_PS_CLICKED) = 0;
    CA_W(F, 0x58) = VT_ClickThrough;
    const int32_t y1 = q + 0x16e;
    const int32_t y2 = h + q * 2 + 0x16e;
    ca_item_ctor(F + 0x70, 0x17, 0, 0, 0, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H), 0x004e4db8, 0, CA_A(F, 0x58), 0);
    ca_item_ctor(F + 0xa8, 5, 0, 0x91, y1, 0, 0, ccall<uint32_t>(F_CareerGetClassName, I32(info(), 0x124)), 0, 0, style);
    ca_item_ctor(F + 0xe0, 5, 0, 0x91, y2, 0, 0, 0x004fff9c, 0, 0, style);                    // "Champion"
    ca_item_ctor(F + 0x118, 4, -1, 0x1b, 0, 0, 0, 0, 0, F_show_winning_season_money, 0);      // Esc
    ca_item_ctor(F + 0x150, 4, -2, 0x20, 0, 0, 0, 0, 0, F_show_winning_season_money, 0);      // space
    put14(F, 0x188, 4, 0xfffffffeu, 0xd, 0, 0, 0, 0, 0, F_show_winning_season_money, 0);        // Enter
    end_item(F, 0x1c0);
    CA_W(F, 0x10) = 0x004fffa8;                                      // ""
    CA_W(F, 0x1c) = CA_A(F, 0x70);
    CA_W(F, 0x14) = 0x004fffac;                                      // "seaswin.stp"
    CA_W(F, 0x18) = 0xfffffffeu;
    CA_W(F, 0x20) = F_season_splash_idle;
    do_dialog(F + 0x10, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H));
    ccall<void>(F_UIRemoveStyle, style);
}
PORT_FN(0x004c1b50, "show_season_win_splash", show_season_win_splash_c, fp_screen)

static uint8_t __cdecl show_winning_season_money_c(int32_t) {
    ccall<void>(F_CreditsDo, CA_G32(S_PS_AMOUNT));
    return 1;
}
PORT_FN(0x004c1db0, "show_winning_season_money", show_winning_season_money_c, fp_screen_i)

// season_splash_idle: after a click or ten seconds, the season's money screen (and the splash ends)
static uint8_t __cdecl season_splash_idle_c(int32_t*) {
    if (CA_G8(S_PS_CLICKED) == 0 &&
        (int32_t)((uint32_t)ccall<int32_t>(uit::F_PTimeNow) - (uint32_t)CA_G32(S_PS_SPLASH_TIME)) <= 0x2710)
        return 0;
    return ccall<uint8_t>(F_show_winning_season_money, 0);
}
static void fp_season_splash_idle(Footprint& f, int32_t*) { f.replay_only = "may run the season's money screen (CreditsDo)"; }
PORT_FN(0x004c1dd0, "season_splash_idle", season_splash_idle_c, fp_season_splash_idle)

// NewClassDo: the advancement certificate ("Congratulations", "You have advanced to", the class)
static void __cdecl NewClassDo_c() {
    alignas(8) uint8_t F[0x230];                                    // sub esp, 0x220; 4 pushes
    // F+0x10 the first style, +0x14 the UIDialog, +0x28 / +0x5c the styles' descriptions, +0x90 the ClickThrough, +0xa8 the items
    ca_xl_once(0x005d36b0, 0x01, 0x005d36c8, 0x004fffb8, 0x004c21b0);  // Career:Advancement:Congratulations
    ca_xl_once(0x005d36b0, 0x02, 0x005d36f0, 0x004fffdc, 0x004c21a0);  // ...:YouHaveAdvancedTo
    CA_W(F, 0xa4) = 0;
    CA_G8(S_PS_CLICKED) = 0;
    put_desc(F, 0x5c, 0x00500004, 0xc, 0x6ac63566u, 0x6ac63566u, 0xd69a6b5au, 0, 0, 0x6ac63566u, 0x6ac63566u, 0xd69a6b5au, 0, 0, 0);
    put_desc(F, 0x28, 0x00500014, 0xc, 0x6ac63566u, 0x6ac63566u, 0xd69a6b5au, 0, 0, 0x6ac63566u, 0x6ac63566u, 0xd69a6b5au, 0, 0, 0);
    CA_W(F, 0x90) = VT_ClickThrough;
    CA_W(F, 0x10) = (uint32_t)add_style(F, 0x28);                   // lcallig.fnt
    const int32_t style2 = add_style(F, 0x5c);                      // slcallig.fnt
    ca_item_ctor(F + 0xa8, 0x17, 0, 0, 0, 0x140, 0xf9, 0x004e4db8, 0, CA_A(F, 0x90), 0);
    ca_item_ctor(F + 0xe0, 5, 0, 0xa0, 0x3e, 0, 0, ca_xlate(0x005d36c8), 0, 0, (int32_t)CA_W(F, 0x10));
    ca_item_ctor(F + 0x118, 5, 0, 0xa0, 0x7c, 0, 0, ca_xlate(0x005d36f0), 0, 0, style2);
    put14(F, 0x150, 5, 0, 0xa0, 0x9b, 0, 0, ccall<uint32_t>(F_CareerGetClassName, I32(info(), 0x124)), 0, 0, (uint32_t)style2);
    ca_item_ctor(F + 0x188, 4, -1, 0x1b, 0, 0, 0, 0, 0, 0, 0);                                 // Esc: -1
    put14(F, 0x1c0, 4, 0xfffffffeu, 0xd, 0, 0, 0, 0, 0, 0, 0);                                 // Enter: -2
    end_item(F, 0x1f8);
    CA_W(F, 0x1c) = 0xfffffffeu;
    CA_W(F, 0x20) = CA_A(F, 0xa8);
    CA_W(F, 0x14) = 0x00500020;                                      // ""
    CA_W(F, 0x18) = 0x00500024;                                      // "seascert.stp"
    CA_W(F, 0x24) = F_ClickThrough_Idle;
    do_dialog(F + 0x14, 0x140, 0xf9);
    ccall<void>(F_UIRemoveStyle, (int32_t)CA_W(F, 0x10));
    ccall<void>(F_UIRemoveStyle, style2);
}
PORT_FN(0x004c1e30, "NewClassDo", NewClassDo_c, fp_screen)

static TwoStyles* __fastcall StandingsViewer_ctor(TwoStyles* self, Edx) {
    alignas(8) uint8_t F[0x6c];                                     // sub esp, 0x68; 1 push: +4, +0x38 the styles
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_StandingsViewer;
    const uint32_t a = CA_GU32(0x005d36ac), b = CA_GU32(0x005d36b8), c = CA_GU32(0x005d36a8), d = CA_GU32(0x005d36dc);
    put_desc(F, 4, 0x004ffd5c, 0x21, a, b, 0xb5b75ad7u, 0, 0, a, a, 0x8c724632u, 0, 0, 0);   // euro10
    put_desc(F, 0x38, 0x004ffd50, 0x21, c, b, 0xb5b65ad6u, 0, 0, a, a, d, 0, 0, 0);           // ruby9
    self->style = add_style(F, 4);
    self->style2 = add_style(F, 0x38);
    return self;
}
PORT_FN(0x004c21c0, "StandingsViewer::StandingsViewer", StandingsViewer_ctor, fp_styles0<TwoStyles>)

// StandingsViewer::Draw: rank, driver, points, the drivers in rank order (a rank shown once; the player highlighted)
static const uint32_t k_xl_standings[4] = {0x005d5d40, 0x005d5d10, 0x005d5d20, 0x005d5d30};
static void __fastcall StandingsViewer_Draw(TwoStyles* self, Edx, gxCanvas* c) {
    alignas(8) uint8_t F[0x120];                                    // sub esp, 0x110; 4 pushes
    // F+0x10 x0, +0x14 the rank, +0x18 the last rank shown, +0x1c the driver's record, +0x20 a text
    ca_xl_once(0x005d5d0c, 0x01, 0x005d5d40, 0x0050007c, 0x004c2580);  // Career:Standings:PointsAbbreviated
    ca_xl_once(0x005d5d0c, 0x02, 0x005d5d10, 0x00500064, 0x004c2570);  // ...:Rank
    ca_xl_once(0x005d5d0c, 0x04, 0x005d5d20, 0x0050004c, 0x004c2560);  // ...:Driver
    ca_xl_once(0x005d5d0c, 0x08, 0x005d5d30, 0x00500034, 0x004c2550);  // ...:Points
    ccall<gxCanvas*>(uit::F_gxSetCanvas, c);
    int32_t y = self->y + 0x23;
    CA_W(F, 0x10) = (uint32_t)(self->x + 0x28);
    CA_W(F, 0x18) = 0xfffffc19u;
    ca_xlate(0x005d5d10);
    style_draw(self->style, (int32_t)CA_W(F, 0x10) + 5, y, CA_GU32(0x005d5d14), 1);
    ca_xlate(0x005d5d20);
    style_draw(self->style, (int32_t)CA_W(F, 0x10) + 0x37, y, CA_GU32(0x005d5d24), 1);
    ca_xlate(0x005d5d30);
    {
        const int32_t yy = y;
        y += 0x14;
        style_draw(self->style, (int32_t)CA_W(F, 0x10) + 0x12c, yy, CA_GU32(0x005d5d34), 1);
    }
    CA_W(F, 0x14) = 0;
    do {
        for (uint32_t d = 0, o = 0; o < 0x440; o += 0x88, d++) {
            CA_W(F, 0x1c) = ca_u(info()) + 0x170 + o;
            const int32_t rk = I32(CA_W(F, 0x1c), 0);
            if (rk != (int32_t)CA_W(F, 0x14)) continue;
            if ((int32_t)CA_W(F, 0x18) != rk) {
                CA_W(F, 0x18) = (uint32_t)rk;
                SPRINTF((char*)(F + 0x20), CA_CP(0x004e59b0), rk + 1);                          // "%d"
                style_draw(self->style, (int32_t)CA_W(F, 0x10) + 5, y, CA_A(F, 0x20), 0);
            }
            const uint32_t fl = d == 7 ? 2u : 0u;
            const char* name = ccall<const char*>(F_CareerGetDriverName, (int32_t)d);
            style_draw(self->style, (int32_t)CA_W(F, 0x10) + 0x37, y, ca_u(name), fl);
            ca_xlate(0x005d5d40);
            {
                // FIX: "<points> <Pts>" (Career:Standings:PointsAbbreviated's translation) went into the frame's last
                // 0x100 bytes unbounded: a translation of about 250 characters ran over the saved registers and the
                // return address. The text keeps its first 255 characters (it's only drawn); one that fits is as before.
                const char* ab = UI_GP(const char, 0x005d5d44);
                const int32_t pts = I32(CA_W(F, 0x1c), 4);
                if (VP_FIX && ca_sd_long(ab, pts, 0x100)) ca_fix_sd((char*)(F + 0x20), 0x100, 0x004ffd68, ab, pts, true);
                else SPRINTF((char*)(F + 0x20), CA_CP(0x004ffd68), pts, ab);                     // "%d %s"
            }
            {
                const int32_t yy = y;
                y += 0x14;
                style_draw(self->style, (int32_t)CA_W(F, 0x10) + 0x12c, yy, CA_A(F, 0x20), 0);
            }
        }
        CA_W(F, 0x14) = CA_W(F, 0x14) + 1;
    } while ((int32_t)CA_W(F, 0x14) < 8);
}
static void fp_StandingsViewer_Draw(Footprint& f, TwoStyles*, Edx, gxCanvas* c) {
    if (!ca_xl_built(0x005d5d0c, 0x0f)) { f.replay_only = "builds its function-local Xlators (atexit)"; return; }
    fp_draw_canvas(f, c);
    for (uint32_t a : k_xl_standings) CA_FP_XL(f, a);
}
PORT_FN(0x004c22b0, "StandingsViewer::Draw", StandingsViewer_Draw, fp_StandingsViewer_Draw)

static void* __fastcall StandingsViewer_sdd(TwoStyles* self, Edx, uint32_t flags) { return two_styles_delete(self, VT_StandingsViewer, flags); }
PORT_FN(0x004c2590, "StandingsViewer::scalar deleting destructor", StandingsViewer_sdd, fp_dtor_flags<TwoStyles>)

static void __fastcall ClickThrough_MouseDown(uit::UICustomControl*, Edx, int32_t, int32_t) { CA_G8(S_PS_CLICKED) = 1; }
static void fp_ClickThrough_mouse(Footprint& f, uit::UICustomControl*, Edx, int32_t, int32_t) { UI_FP(f, S_PS_CLICKED, 1, "postseas.obj clicked flag"); }
PORT_FN(0x004c25e0, "ClickThrough::MouseDown", ClickThrough_MouseDown, fp_ClickThrough_mouse)
static void __fastcall ClickThrough_MouseRDown(uit::UICustomControl*, Edx, int32_t, int32_t) { CA_G8(S_PS_CLICKED) = 1; }
PORT_FN(0x004c25f0, "ClickThrough::MouseRDown", ClickThrough_MouseRDown, fp_ClickThrough_mouse)
static uint8_t __cdecl ClickThrough_Idle(int32_t*) { return CA_G8(S_PS_CLICKED); }
static void fp_ClickThrough_Idle(Footprint&, int32_t*) {}
PORT_FN(0x004c2620, "ClickThrough::Idle", ClickThrough_Idle, fp_ClickThrough_Idle)

// =========================================================================================================================
// ranking.obj
// =========================================================================================================================
// FIX CANDIDATE: its CareerStatus is added before its texts are formatted (career_main.cpp, career_menu).
static void __cdecl RankingDo_c() {
    alignas(8) uint8_t F[0x234];                                    // sub esp, 0x22c; 2 pushes
    // F+8 the UIDialog, +0x1c the items, +0xfc the StandingsViewer, +0x11c / +0x13c two texts, +0x15c the CareerStatus
    ca_xl_once(0x005d41b8, 0x01, 0x005d4198, 0x00500874, 0x004c5430);  // UI:Back
    ca_xl_once(0x005d41b8, 0x02, 0x005d41f0, 0x0050087c, 0x004c5420);  // Career:Ranking:Title
    tcall<void*>(F_StandingsViewer_ctor, (void*)(F + 0xfc));
    CA_W(F, 0x15c) = VT_CareerStatus;
    CA_W(F, 0x170) = 0;
    SPRINTF((char*)(F + 0x13c), CA_CP(0x00500894), I32(info(), 0x120) + 1);                   // "%d"
    SPRINTF((char*)(F + 0x11c), CA_CP(0x00500898), I32(info(), 0x11c) + 1);
    put14(F, 0x1c, 0x17, 0, 0, 0, 0, 0, 0x004e4db8, 0, CA_A(F, 0x15c), 0);                   // the CareerStatus
    put14(F, 0x54, 0x17, 0, 0x32, 0x78, 0x208, 0x122, 0x004e4db8, 0, CA_A(F, 0xfc), 0);       // the StandingsViewer
    put14(F, 0x8c, 2, 0xffffffffu, 0x13, 0x1a2, 0, 0, ca_xlate(0x005d4198), 0, 0, 6);          // Back: -1
    end_item(F, 0xc4);
    CA_W(F, 8) = ca_xlate(0x005d41f0);
    CA_W(F, 0x14) = CA_A(F, 0x1c);
    CA_W(F, 0xc) = 0x0050089c;                                       // "calendar.stp"
    CA_W(F, 0x10) = 0xfffffffeu;
    CA_W(F, 0x18) = 0;
    do_dialog(F + 8, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H));
    CA_W(F, 0x15c) = VT_UICustomControl;                             // the inlined destructors
    CA_W(F, 0xfc) = VT_StandingsViewer;
    ccall<void>(F_UIRemoveStyle, (int32_t)CA_W(F, 0x114));
    ccall<void>(F_UIRemoveStyle, (int32_t)CA_W(F, 0x118));
}
PORT_FN(0x004c5160, "RankingDo", RankingDo_c, fp_screen)

}  // namespace career_screens
}  // namespace
