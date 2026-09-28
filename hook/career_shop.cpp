// career_shop.cpp -- M3 UI stage, step U4 (group B): the upgrade shop, upgrade.obj, rewritten faithfully (library `career`).
//
//   UpgradeDo (the shop: the upgrades' names translated, a snapshot of what's bought, a full-screen dialog of the career
//   status, the UpgradeSummary and seven buttons -- engine, drivetrain, chassis, body, wheels, paint, back -- and the
//   test mode's '$' key, give_money), get_upgrade_names ("Upgrades:<name>:Name" / ":Desc" translated into the two
//   tables), the page callbacks engine_cb / transmission_cb / suspension_cb / wheels_cb / body_cb (each a CatalogItem list
//   on its stack and a translated title: upgrade_page) and paint_cb (the paint kit), upgrade_page (a dialog of the career
//   status, an UpgradeCatalog and Back; the catalog's stamps forgotten after), gxHollowRect, can_afford / debit_account /
//   money_string; UpgradeSummary (Create: watch the upgrades; Callback: redraw; Draw: the car file loaded with the
//   upgrades, its peak power and torque, the three lists of what's bought, the weight; draw_class; its deleting
//   destructor); UpgradeCatalog (the constructor: each item's stamp, its upgrade's index; Create: the scroll axis and
//   four notifications; Added: the scroll bar; Callback; the mouse: the hot item, a click buys -- the class, the upgrade
//   it requires, the funds checked, a yes / no box --, the right button toggles an upgrade in test mode; Draw; its
//   deleting destructor); and the nine function-local Xlators' destructor helpers (bare `ret`s).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this object's own too, so a
// hooked rewrite is what runs), virtual calls through the vtable, the compiler's inline string code as it runs it
// (ui_types.h). UpgradeDo's and upgrade_page's frames are laid out as the originals' (a byte array addressed by the
// original's offsets): the dialog, its items, the career status and the summary / catalog sit where the original puts
// them, and what the original leaves unwritten (the career status's own fields, the constructor inlined to its vtable and
// widget) is left unwritten. The item lists are built as the original builds them -- UIDialogItem's constructor where it
// calls it, the inlined items' 14 dwords --, each translated text read after its Xlator is refreshed. x87 (UpgradeSummary::
// Draw only): a float passed to sprintf is loaded and stored as a double (fld dword; fstp qword); the weight's product is
// the original's grouping, (weight * 0.4545f) * the locale's factor, its constant a float.
//
// Footprints (main thread). replay_only: UpgradeDo, upgrade_page and the page callbacks (modal dialogs), paint_cb (the
// paint kit), the constructor (loads stamps), Create (notifications: allocates), Added (builds widgets), the destructors
// (free), UpgradeSummary::Draw (loads the car file), MouseUp when it reaches a dialog (not allowed, a requirement missing,
// the yes / no or can't-afford box), a draw the first time it runs (it builds its function-local Xlators: atexit). Shadow-
// checkable: give_money and debit_account (the funds), get_upgrade_names (the two tables), gxHollowRect and draw_class
// (the current canvas; draw_class its y and the Xlator), money_string (its buffer), can_afford (nothing), the Callbacks
// (the CustomWidget's dirty flag), MouseDown / MouseMove / NotOver (the catalog), MouseRDown (the catalog and the upgrade
// it toggles), MouseUp otherwise (the catalog), UpgradeCatalog::Draw (its canvas, the current canvas, the Xlators, the
// money buffer, CareerGetClassName's Xlators).
//
// FIX CANDIDATEs (left faithful, marked in place): get_upgrade_names' 0x100-byte name buffer and its two 256-entry tables
// (a car's upgrade set with a name over 239 characters, or more than 256 upgrades: a damaged or modded viper.ugs), a
// catalog of more than 64 items (the lists are the code's own: 4 to 10), MouseRDown reading an item's index before it
// checks the item (only a hot item far outside the list, which the control's own rectangle rules out), MouseUp's
// 0x100-byte message (a requirement's translated name and message over 254 characters).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "career_shop.h"

namespace {
namespace career_shop {
using namespace uit;
using namespace cshop;

typedef void(__cdecl* Log2_t)(const char*, const char*);
#define CS_LogPanic ((Log2_t)(uintptr_t)F_LogPanic)
#define CS_CareerLog ((Log2_t)(uintptr_t)F_CareerLog)

// ---- footprint helpers ------------------------------------------------------------------------------------------------------
static void fp_modal(Footprint& f, int32_t) { f.replay_only = "runs a dialog (modal: its own input loop)"; }
static void fp_paint(Footprint& f, int32_t) { f.replay_only = "runs the paint kit (its own loop, files)"; }
static void fp_modal0(Footprint& f) { f.replay_only = "runs a dialog (modal: its own input loop)"; }
static void fp_pure0(Footprint& f) { f.pure = true; }
static __forceinline void fp_funds(Footprint& f) { UI_FP(f, S_FUNDS, 4, "the career's funds"); }
static __forceinline void fp_cur_canvas(Footprint& f) { UI_FP_CUR_CANVAS(f); }
static __forceinline void fp_draw_canvas(Footprint& f, gxCanvas* c) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}
template <typename T> static void fp_dirty(Footprint& f, T* self, Edx, int32_t, const void*) {
    if (self->widget) f.add((void*)self->widget, sizeof(CustomWidget), "its CustomWidget");
}
template <typename T> static void fp_notes(Footprint& f, T*, Edx) { f.replay_only = "adds notifications (allocates their copies)"; }
template <typename T> static void fp_frees(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "forgets its stamps, frees the control"; }

// =========================================================================================================================
// UpgradeDo and the page callbacks
// =========================================================================================================================
// the shop. The frame (esp after the prologue): +8 the UIDialog, +0x1c the items (11), +0x284 the UpgradeSummary, +0x29c
// the career status (its inlined constructor sets the vtable and the widget only), +0x374 "Upgrade:Purchased"'s text,
// +0x474 the upgrades as they were (nothing reads either copy)
static void __cdecl UpgradeDo_n() {
    alignas(8) uint8_t fr[0x574];
    uint8_t* const F = fr;
    ccall<void>(F_get_upgrade_names);
    int32_t i = 0;
    do {
        i++;
        const uint8_t* info = cs_info();
        CS_B(F, 0x473 + i) = *(const volatile uint8_t*)(info + 0x1b + i);
    } while (i < 0x100);
    cs_xl_once(S_DO_ONCE, 0x01, 0x005d3d08, 0x00500238, 0x004c3150);    // Titles:UpgradeShop
    cs_xl_once(S_DO_ONCE, 0x02, 0x005d3c80, 0x0050024c, 0x004c3140);    // Upgrade:Engine
    cs_xl_once(S_DO_ONCE, 0x04, 0x005d3d28, 0x0050025c, 0x004c3130);    // Upgrade:Chassis
    cs_xl_once(S_DO_ONCE, 0x08, 0x005d3cc0, 0x0050026c, 0x004c3120);    // Upgrade:Tires
    cs_xl_once(S_DO_ONCE, 0x10, 0x005d4188, 0x0050027c, 0x004c3110);    // Upgrade:DriveTrain
    cs_xl_once(S_DO_ONCE, 0x20, 0x005d3848, 0x00500290, 0x004c3100);    // Upgrade:Aero
    cs_xl_once(S_DO_ONCE, 0x40, 0x005d3770, 0x005002a0, 0x004c30f0);    // Upgrade:Body
    cs_xl_once(S_DO_ONCE, 0x80, 0x005d3cf8, 0x005002b0, 0x004c30e0);    // Upgrade:Purchased
    cs_xl_once(S_DO_ONCE2, 0x01, 0x005d3810, 0x005002c4, 0x004c30d0);   // Upgrade:Paint
    cs_xl_once(S_DO_ONCE2, 0x02, 0x005d3c98, 0x005002d4, 0x004c30c0);   // Upgrade:Wheels
    cs_xl_once(S_DO_ONCE2, 0x04, 0x005d3ce0, 0x005002e4, 0x004c30b0);   // UI:Back
    crt_strcpy((char*)(F + 0x374), CS_CP(cs_xlate(0x005d3cf8)));
    cs_item14(F + 0x1c, 0x17, 0, 0, 0, 0, 0, 0x004e4db8u, 0, CS_A(F, 0x29c), 0);
    CS_W(F, 0x2b0) = 0;                                          // the career status's widget, the summary's
    CS_W(F, 0x298) = 0;
    CS_W(F, 0x29c) = VT_CareerStatus;
    CS_W(F, 0x284) = VT_UpgradeSummary;
    cs_item14(F + 0x54, 0x17, 0, 0x28, 0x5f, 0x1ae, 0x10e, 0x004e4db8u, 0, CS_A(F, 0x284), 0);
    cs_item_ctor(F + 0x8c, 2, 0, 0x1ef, 0x5d, 0, 0, cs_xlate(0x005d3c80), 0, F_engine_cb, 2);
    cs_item14(F + 0xc4, 2, 0, 0x1ef, 0x8e, 0, 0, cs_xlate(0x005d4188), 0, F_transmission_cb, 2);
    cs_item_ctor(F + 0xfc, 2, 0, 0x1ef, 0xbf, 0, 0, cs_xlate(0x005d3d28), 0, F_suspension_cb, 2);
    cs_item14(F + 0x134, 2, 0, 0x1ef, 0xf0, 0, 0, cs_xlate(0x005d3770), 0, F_body_cb, 2);
    cs_item_ctor(F + 0x16c, 2, 0, 0x1ef, 0x121, 0, 0, cs_xlate(0x005d3c98), 0, F_wheels_cb, 2);
    cs_item14(F + 0x1a4, 2, 0, 0x1ef, 0x152, 0, 0, cs_xlate(0x005d3810), 0, F_paint_cb, 2);
    cs_item_ctor(F + 0x1dc, 2, -1, 0x13, 0x1a2, 0, 0, cs_xlate(0x005d3ce0), 0, 0, 6);
    cs_item14(F + 0x214, 4, 0, 0x24, 0, 0, 0, 0, 0, F_give_money, 0);     // the '$' key (test mode: 500 more)
    cs_item_end(F + 0x24c);
    cs_dialog(F + 8, cs_xlate(0x005d3d08), 0x005002ecu, -2, CS_A(F, 0x1c));   // shop.stp
    const int32_t h = UI_G32(S_SCREEN_H), w = UI_G32(S_SCREEN_W);
    ccall<int32_t>(F_UIDoDialog, (const void*)(F + 8), w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
}
PORT_FN(0x004c29b0, "UpgradeDo", UpgradeDo_n, fp_modal0)

// the '$' hot key: 500 more in test mode
static uint8_t __cdecl give_money_n(int32_t) {
    if (ccall<uint8_t>(F_CareerIsTestMode)) {
        uint8_t* info = cs_info();
        *(volatile int32_t*)(info + 0x18) += 0x1f4;
    }
    return 0;
}
static void fp_give_money(Footprint& f, int32_t) { fp_funds(f); }
PORT_FN(0x004c3090, "give_money", give_money_n, fp_give_money)

// every upgrade's translated name and description (the set's entry i into the tables' entry i)
static void __cdecl get_upgrade_names_n() {
    char buf[0x100];
    int32_t i = 0;
    if (cs_set()->count > i) {
        uint32_t e = 0;
        do {
            e += 4;
            // FIX CANDIDATE: the name is formatted into 0x100 bytes unbounded (a name over 239 characters overruns the
            // stack), and the tables hold 256 (a set of more than 256 writes past them); only a damaged or modded
            // upgrade set has either
            const uint8_t* s = (const uint8_t*)cs_set();
            i++;
            UI_sprintf(buf, CS_CP(0x005002f8), *(const volatile uint32_t*)(s + e));       // "Upgrades:%s:Name"
            UI_GU32(0x005d3d7cu + e) = ccall<uint32_t>(F_Xlate, (const char*)buf);
            s = (const uint8_t*)cs_set();
            UI_sprintf(buf, CS_CP(0x0050030c), *(const volatile uint32_t*)(s + e));       // "Upgrades:%s:Desc"
            UI_GU32(0x005d3864u + e) = ccall<uint32_t>(F_Xlate, (const char*)buf);
        } while (cs_set()->count > i);
    }
}
static void fp_get_upgrade_names_f(Footprint& f) {
    const CshUpgradeSet* s = UI_GP(CshUpgradeSet, S_UPGRADE_SET);      // (CareerGetUpgradeSet's answer, read directly)
    int32_t n = s ? s->count : 0;
    if (n < 0) n = 0;
    if (n > 0x4000) { f.replay_only = "an upgrade set too big to bound"; return; }
    UI_FP(f, S_UP_NAMES, (uint32_t)n * 4u, "the upgrades' translated names");
    UI_FP(f, S_UP_DESCS, (uint32_t)n * 4u, "the upgrades' translated descriptions");
}
PORT_FN(0x004c3160, "get_upgrade_names", get_upgrade_names_n, fp_get_upgrade_names_f)

// Paint: the paint kit on the career's car
static uint8_t __cdecl paint_cb_n(int32_t) {
    ccall<void>(F_PaintKitDo, ccall<const char*>(F_CareerGetCarName), (int32_t)8);
    return 0;
}
PORT_FN(0x004c31f0, "paint_cb", paint_cb_n, fp_paint)

// a page: its items {name, stamp} (and the list's end) on the stack, its title's Xlator built once, upgrade_page
static __forceinline void page(const uint32_t* items, uint32_t n, volatile uint32_t* arr, uint32_t guard, uint32_t xl,
                               uint32_t key, uint32_t dtor) {
    for (uint32_t k = 0; k < n; k++) arr[k] = items[k];
    arr[n] = 0;
    arr[n + 1] = 0;
    cs_xl_once(guard, 1, xl, key, dtor);
    const uint32_t t = cs_xlate(xl);
    ccall<void>(F_upgrade_page, CS_CP(t), (const void*)arr);
}
static uint8_t __cdecl engine_cb_n(int32_t) {
    static const uint32_t k[20] = {0x00500320, 0x00500334, 0x00500344, 0x00500354, 0x00500360, 0x00500370, 0x00500380,
                                   0x00500388, 0x00500398, 0x005003a8, 0x005003b8, 0x005003cc, 0x005003d8, 0x005003e4,
                                   0x005003f0, 0x005003fc, 0x00500408, 0x00500418, 0x00500424, 0x00500434};
    volatile uint32_t arr[22];
    page(k, 20, arr, 0x005d3ca8, 0x005d3758, 0x00500440, 0x004c3630);   // Upgrades:Titles:Engine
    return 0;
}
PORT_FN(0x004c3210, "engine_cb", engine_cb_n, fp_modal)

static uint8_t __cdecl transmission_cb_n(int32_t) {
    static const uint32_t k[8] = {0x0050046c, 0x00500474, 0x00500484, 0x00500494, 0x005004a4, 0x005004b8, 0x005004c4,
                                  0x005004d4};
    volatile uint32_t arr[10];
    page(k, 8, arr, 0x005d4180, 0x005d37e0, 0x005004e0, 0x004c36f0);    // Upgrades:Titles:Transmission
    return 0;
}
PORT_FN(0x004c3640, "transmission_cb", transmission_cb_n, fp_modal)

static uint8_t __cdecl suspension_cb_n(int32_t) {
    static const uint32_t k[14] = {0x00500500, 0x00500510, 0x00500520, 0x00500534, 0x00500544, 0x00500550, 0x0050055c,
                                   0x0050056c, 0x00500578, 0x00500588, 0x00500598, 0x005005a8, 0x005005b4, 0x005005c4};
    volatile uint32_t arr[16];
    page(k, 14, arr, 0x005d3834, 0x005d3838, 0x005005d0, 0x004c37e0);   // Upgrades:Titles:Suspension
    return 0;
}
PORT_FN(0x004c3700, "suspension_cb", suspension_cb_n, fp_modal)

static uint8_t __cdecl wheels_cb_n(int32_t) {
    static const uint32_t k[8] = {0x005005ec, 0x005005f8, 0x00500604, 0x00500614, 0x00500620, 0x0050062c, 0x00500638,
                                  0x00500648};
    volatile uint32_t arr[10];
    page(k, 8, arr, 0x005d3754, 0x005d3858, 0x00500654, 0x004c38a0);    // Upgrades:Titles:Wheels
    return 0;
}
PORT_FN(0x004c37f0, "wheels_cb", wheels_cb_n, fp_modal)

static uint8_t __cdecl body_cb_n(int32_t) {
    static const uint32_t k[12] = {0x0050066c, 0, 0x0050067c, 0, 0x0050068c, 0, 0x005006a0, 0, 0x005006b4, 0, 0x005006c8, 0};
    volatile uint32_t arr[14];
    page(k, 12, arr, 0x005d3ca4, 0x005d3d70, 0x005006dc, 0x004c3970);   // Upgrades:Titles:Body (no pictures)
    return 0;
}
PORT_FN(0x004c38b0, "body_cb", body_cb_n, fp_modal)

// a page of the catalog. The frame (esp after the prologue): +0xc the UIDialog, +0x20 the items (4), +0x100 the career
// status, +0x1d8 the UpgradeCatalog. After the dialog the status's vtable goes back to the base's and the catalog's
// stamps are forgotten (its vtable is left its own)
static void __cdecl upgrade_page_n(const char* title, const CatalogItem* items) {
    alignas(8) uint8_t fr[0x40c];
    uint8_t* const F = fr;
    cs_xl_once(S_PAGE_ONCE, 1, 0x005d3d18, 0x00500458, 0x004c3620);    // UI:Back
    UpgradeCatalog* cat = (UpgradeCatalog*)(F + 0x1d8);
    tcall<UpgradeCatalog*>(F_UpgradeCatalog_ctor, cat, items);
    CS_W(F, 0x114) = 0;
    cs_item14(F + 0x20, 0x17, 0, 0, 0, 0, 0, 0x004e4db8u, 0, CS_A(F, 0x100), 0);
    CS_W(F, 0x100) = VT_CareerStatus;
    cs_item14(F + 0x58, 0x17, 0, 0x2d, 0x5a, 0x217, 0x11d, 0x004e4db8u, 0, CS_A(F, 0x1d8), 0);
    cs_item14(F + 0x90, 2, (uint32_t)-1, 0x13, 0x1a2, 0, 0, cs_xlate(0x005d3d18), 0, 0, 6);
    cs_item_end(F + 0xc8);
    cs_dialog(F + 0xc, cs_u(title), 0x00500460u, -2, CS_A(F, 0x20));      // catalog.stp
    const int32_t h = UI_G32(S_SCREEN_H), w = UI_G32(S_SCREEN_W);
    ccall<int32_t>(F_UIDoDialog, (const void*)(F + 0xc), w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
    CS_W(F, 0x100) = VT_UICustomControl;
    CS_W(F, 0x1d8) = VT_UpgradeCatalog;
    for (int32_t k = 0; cat->count > k; k++)
        if (void* s = cat->stamps[k]) ccall<void>(F_gxForgetStamp, s);
}
static void fp_upgrade_page(Footprint& f, const char*, const CatalogItem*) { f.replay_only = "runs a dialog (modal), loads stamps"; }
PORT_FN(0x004c3320, "upgrade_page", upgrade_page_n, fp_upgrade_page)

// a rectangle's outline: top, right, bottom, left
static void __cdecl gxHollowRect_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t col) {
    ccall<void>(F_gxLine, x0, y0, x1, y0, col);
    ccall<void>(F_gxLine, x1, y0, x1, y1, col);
    ccall<void>(F_gxLine, x1, y1, x0, y1, col);
    ccall<void>(F_gxLine, x0, y1, x0, y0, col);
}
static void fp_gxHollowRect(Footprint& f, int32_t, int32_t, int32_t, int32_t, uint32_t) { fp_cur_canvas(f); }
PORT_FN(0x004c3570, "gxHollowRect", gxHollowRect_n, fp_gxHollowRect)

static uint8_t __cdecl can_afford_n(int32_t price) {
    const uint8_t* info = cs_info();
    return *(const volatile int32_t*)(info + 0x18) >= price ? 1 : 0;
}
static void fp_can_afford(Footprint&, int32_t) {}
PORT_FN(0x004c35d0, "can_afford", can_afford_n, fp_can_afford)

static void __cdecl debit_account_n(int32_t price) {
    uint8_t* info = cs_info();
    *(volatile int32_t*)(info + 0x18) -= price;
}
static void fp_debit_account(Footprint& f, int32_t) { fp_funds(f); }
PORT_FN(0x004c35f0, "debit_account", debit_account_n, fp_debit_account)

static const char* __cdecl money_string_n(int32_t v) {
    ccall<void>(F_LocaleMoney, CS_VP(S_MONEY), v, (uint32_t)0);
    return CS_CP(S_MONEY);
}
static void fp_money_string(Footprint& f, int32_t) { UI_FP(f, S_MONEY, 0x40, "money_string's buffer"); }
PORT_FN(0x004c3600, "money_string", money_string_n, fp_money_string)

// =========================================================================================================================
// UpgradeSummary
// =========================================================================================================================
static void __fastcall UpgradeSummary_Create_n(UICustomControl* self, Edx) {
    uint8_t* info = cs_info();
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)(info + 0x1c), (uint32_t)0x100);
}
PORT_FN(0x004c3980, "UpgradeSummary::Create", UpgradeSummary_Create_n, fp_notes<UICustomControl>)

static void __fastcall UpgradeSummary_Callback_n(UICustomControl* self, Edx, int32_t, const void*) {
    tcall<void>(F_UICC_Dirty, self);
}
PORT_FN(0x004c39a0, "UpgradeSummary::Callback", UpgradeSummary_Callback_n, fp_dirty<UICustomControl>)

// one section's list (Draw's three inlined copies of draw_class): the bought upgrades of the section from entry 1 on,
// two to a row (x1, x2), each over a black box; "Upgrades:Stock" when there's none. y is left below the list, then 15 on
struct SumState { int32_t y, n, x0, x1, x2; };
static __forceinline void summary_list(SumState& s, int32_t section) {
    uint8_t any = 0;
    int32_t i = 1;
    s.n = 0;
    if (cs_set()->count > i) {
        uint32_t e = 4;
        do {
            const CshUpgrade* u = *(CshUpgrade* const volatile*)((const uint8_t*)cs_set() + e + 4);
            if (u->section == section && *(const volatile uint8_t*)(cs_info() + i + 0x1c) != 0) {
                const int32_t odd = s.n & 1;
                const int32_t x = odd ? s.x2 : s.x1;
                volatile int32_t bx0, by0, bx1, by1;
                const char* nm = UI_GP(const char, 0x005d3d80u + e);
                ccall<void>(F_UIStyleGetBounds, (int32_t)0x12, nm, x, s.y, &bx0, &by0, &bx1, &by1);
                bx1 = bx1 - 3;
                by0 = by0 + 2;
                ccall<void>(F_gxRect, (int32_t)bx0, (int32_t)by0, (int32_t)bx1, (int32_t)by1, (uint32_t)0);
                ccall<void>(F_UIStyleDraw, (int32_t)0x12, x, s.y, UI_GP(const char, 0x005d3d80u + e), (uint32_t)0);
                if (odd) s.y += 0xa;
                any = 1;
                s.n++;
            }
            e += 4;
            i++;
        } while (cs_set()->count > i);
    }
    if (!any) {
        cs_xl_once(S_STOCK_ONCE, 1, 0x005d5d00, 0x005006f4, 0x004c42d0);     // Upgrades:Stock
        const uint32_t t = cs_xlate(0x005d5d00);
        ccall<void>(F_UIStyleDraw, (int32_t)0x12, s.x1, s.y, CS_CP(t), (uint32_t)0);
        s.y += 0xa;
    } else if (!(s.n & 1)) {
        s.y -= 0xa;
    }
    s.y += 0xf;
}
// a title (style 0xc at x0)
static __forceinline void summary_title(const SumState& s, uint32_t xl) {
    const uint32_t t = cs_xlate(xl);
    ccall<void>(F_UIStyleDraw, (int32_t)0xc, s.x0, s.y, CS_CP(t), (uint32_t)0);
}
// "%s %1.0f %s @ %1.0f %s": a figure and the rpm it peaks at, the numbers as the locale writes them
static __forceinline void summary_peak(uint8_t* F, uint32_t xl_what, uint32_t xl_unit, uint32_t val_off, uint32_t rpm_off) {
    cs_xlate(0x005d3cd0);
    cs_xlate(xl_unit);
    cs_xlate(xl_what);
    char* buf = (char*)(F + 0x38);
    const double rpm = (double)*(const volatile float*)(F + rpm_off);
    const double val = (double)*(const volatile float*)(F + val_off);
    UI_sprintf(buf, CS_CP(0x00500710), UI_GP(const char, xl_what + 4), val, UI_GP(const char, xl_unit + 4), rpm,
               UI_GP(const char, 0x005d3cd4));
    ccall<void>(F_LocaleConvertNumeric, buf);
}
// the frame (esp after the prologue): +0x10 y, +0x14 n, +0x18 x1, +0x24 x0, +0x30 x2, +0x38 the text (0x50), +0x88 the
// CarData, +0x268 the default setup
static void __fastcall UpgradeSummary_Draw_n(UICustomControl* self, Edx, gxCanvas* c) {
    alignas(8) uint8_t fr[0x33c];
    uint8_t* const F = fr;
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    SumState s;
    s.x0 = self->x + 5;
    s.y = self->y + 0xa;
    ccall<void>(F_CarFileMakeDefaultSetup, (void*)(F + 0x268));
    UI_sprintf((char*)(F + 0x38), CS_CP(0x00500728), ccall<const char*>(F_CareerGetCarName));   // "%s.cf"
    {
        uint8_t* info = cs_info();
        ccall<uint8_t>(F_CarFileLoad, (void*)(F + 0x88), (const void*)(F + 0x268), (const char*)(F + 0x38), info + 0x1c);
    }
    s.x1 = s.x0 + 5;
    s.x2 = s.x0 + 0xdc;
    summary_title(s, 0x005d3780);                                // Career:UpgradeSummary:Engine
    s.y += 0xf;
    summary_peak(F, 0x005d3800, 0x005d37f0, 0xf4, 0xfc);   // the power, its rpm
    ccall<void>(F_UIStyleDraw, (int32_t)0x12, s.x1, s.y, (const char*)(F + 0x38), (uint32_t)0);
    summary_peak(F, 0x005d3828, 0x005d3790, 0xf0, 0xf8);   // the torque, its rpm
    ccall<void>(F_UIStyleDraw, (int32_t)0x12, s.x2, s.y, (const char*)(F + 0x38), (uint32_t)0);
    s.y += 0xd;
    summary_list(s, 0);
    summary_title(s, 0x005d3cb0);
    s.y += 0x12;
    summary_list(s, 2);
    summary_title(s, 0x005d3d38);
    s.y += 0x12;
    summary_list(s, 1);
    summary_title(s, 0x005d3c70);
    s.y += 0xf;
    cs_xlate(0x005d3748);
    {
        // the weight: (the car file's figure * 0.4545f) * the locale's factor, in the locale's unit
        const uint8_t* loc = UI_GP(const uint8_t, 0x00509354);
        const volatile float* kf = (const volatile float*)(uintptr_t)0x004dfc64;
        const double wgt = ((double)*(const volatile float*)(F + 0x90) * (double)*kf) * (double)*(const volatile float*)(loc + 0x34);
        UI_sprintf((char*)(F + 0x38), CS_CP(0x00500704), UI_GP(const char, 0x005d374c), wgt,
                   *(const char* const volatile*)(loc + 0x30));
    }
    ccall<void>(F_LocaleConvertNumeric, (char*)(F + 0x38));
    ccall<void>(F_UIStyleDraw, (int32_t)0x12, s.x1, s.y, (const char*)(F + 0x38), (uint32_t)0);
    volatile int32_t y = s.y + 0xa;
    tcall<void>(F_UpgradeSummary_draw_class, self, (int32_t)3, s.x1, s.x2, (int32_t*)&y);
}
static void fp_UpgradeSummary_Draw(Footprint& f, UICustomControl*, Edx, gxCanvas*) {
    f.replay_only = "loads the car file (CarFileLoad)";
}
PORT_FN(0x004c39b0, "UpgradeSummary::Draw", UpgradeSummary_Draw_n, fp_UpgradeSummary_Draw)

// the bought upgrades of a section, as Draw's lists (two to a row from xa, xb), 3 below *y; "Upgrades:Stock" if none
static void __fastcall UpgradeSummary_draw_class_n(UICustomControl*, Edx, int32_t section, int32_t xa, int32_t xb, int32_t* py) {
    volatile int32_t* const y = py;
    uint8_t any = 0;
    int32_t n = 0;
    int32_t i = 1;
    *y += 3;
    if (cs_set()->count > 1) {
        uint32_t e = 4;
        do {
            const CshUpgrade* u = *(CshUpgrade* const volatile*)((const uint8_t*)cs_set() + e + 4);
            if (u->section == section && *(const volatile uint8_t*)(cs_info() + i + 0x1c) != 0) {
                const int32_t odd = n & 1;
                const int32_t x = odd ? xb : xa;
                volatile int32_t bx0, by0, bx1, by1;
                const char* nm = UI_GP(const char, 0x005d3d80u + e);
                ccall<void>(F_UIStyleGetBounds, (int32_t)0x12, nm, x, (int32_t)*y, &bx0, &by0, &bx1, &by1);
                bx1 = bx1 - 3;
                by0 = by0 + 2;
                ccall<void>(F_gxRect, (int32_t)bx0, (int32_t)by0, (int32_t)bx1, (int32_t)by1, (uint32_t)0);
                ccall<void>(F_UIStyleDraw, (int32_t)0x12, x, (int32_t)*y, UI_GP(const char, 0x005d3d80u + e), (uint32_t)0);
                if (odd) *y += 0xa;
                any = 1;
                n++;
            }
            e += 4;
            i++;
        } while (cs_set()->count > i);
    }
    if (!any) {
        cs_xl_once(S_STOCK_ONCE, 1, 0x005d5d00, 0x005006f4, 0x004c42d0);
        const uint32_t t = cs_xlate(0x005d5d00);
        ccall<void>(F_UIStyleDraw, (int32_t)0x12, xa, (int32_t)*y, CS_CP(t), (uint32_t)0);
        *y += 0xa;
        return;
    }
    if (!(n & 1)) *y -= 0xa;
}
static void fp_UpgradeSummary_draw_class(Footprint& f, UICustomControl*, Edx, int32_t, int32_t, int32_t, int32_t* y) {
    if (!cs_xl_built(S_STOCK_ONCE, 1)) { f.replay_only = "the first call builds its Xlator (atexit)"; return; }
    fp_cur_canvas(f);
    CS_FP_XL(f, 0x005d5d00);
    f.add(y, 4, "the caller's y");
}
PORT_FN(0x004c4150, "UpgradeSummary::draw_class", UpgradeSummary_draw_class_n, fp_UpgradeSummary_draw_class)

static void* __fastcall UpgradeSummary_sdd_n(UICustomControl* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void fp_UpgradeSummary_sdd(Footprint& f, UICustomControl*, Edx, uint32_t) { f.replay_only = "frees the control"; }
PORT_FN(0x004c42e0, "UpgradeSummary::scalar deleting destructor", UpgradeSummary_sdd_n, fp_UpgradeSummary_sdd)

// =========================================================================================================================
// UpgradeCatalog
// =========================================================================================================================
// an upgrade's index in the car's set by name (stricmp with each entry's name, from 0); a missing one is a panic, -1
static __forceinline int32_t find_upgrade(const char* name) {
    int32_t k = 0;
    if (cs_set()->count > k) {
        uint32_t e = 0;
        do {
            const char* u = *(const char* const volatile*)((const uint8_t*)cs_set() + e + 4);
            if (ccall<int>(F_stricmp, u, name) == 0) return k;
            e += 4;
            k++;
        } while (cs_set()->count > k);
    }
    CS_LogPanic(CS_CP(0x00500730), name);                        // "Can't find upgrade %s"
    return -1;
}

static UpgradeCatalog* __fastcall UpgradeCatalog_ctor_n(UpgradeCatalog* self, Edx, const CatalogItem* items) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_UpgradeCatalog;
    self->axis.total = 0;
    self->axis.visible = 0;
    self->axis.pos = 0;
    self->hot = -1;
    self->pressed = 0;
    self->count = 0;
    self->items = items;
    if (items[0].name) {
        do {
            // FIX CANDIDATE: 64 items at most (stamps and index); a longer list writes over index, pressed and hot. The
            // lists are the page callbacks' own (4 to 10)
            const int32_t n = self->count;
            if (const char* st = *(const char* const volatile*)&items[n].stamp) self->stamps[n] = ccall<void*>(F_gxGetStamp, st);
            else self->stamps[n] = 0;
            const char* nm = *(const char* const volatile*)&items[self->count].name;
            const int32_t k = find_upgrade(nm);
            self->index[self->count] = k;
            self->count = self->count + 1;
        } while (*(const char* const volatile*)&items[self->count].name);
    }
    return self;
}
static void fp_UpgradeCatalog_ctor(Footprint& f, UpgradeCatalog*, Edx, const CatalogItem*) { f.replay_only = "loads stamps"; }
PORT_FN(0x004c4300, "UpgradeCatalog::UpgradeCatalog", UpgradeCatalog_ctor_n, fp_UpgradeCatalog_ctor)

// the scroll axis (95 a row, in steps of 8), then four notifications: the upgrades, the axis, the hot item, pressed
static void __fastcall UpgradeCatalog_Create_n(UpgradeCatalog* self, Edx) {
    self->axis.total = (int32_t)((uint32_t)self->count * 95u) / 8;
    self->axis.visible = self->h / 8;
    uint8_t* info = cs_info();
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)(info + 0x1c), (uint32_t)0x100);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->axis, (uint32_t)0xc);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->hot, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->pressed, (uint32_t)1);
}
PORT_FN(0x004c4400, "UpgradeCatalog::Create", UpgradeCatalog_Create_n, fp_notes<UpgradeCatalog>)

// the scroll bar (item 0x11) at the right edge, on the axis
static void __fastcall UpgradeCatalog_Added_n(UpgradeCatalog* self, Edx) {
    UIDialogItem it[2];
    const int32_t x = self->x + self->w + 8;
    const int32_t y = self->y;
    const int32_t h = self->h;
    cs_item14(&it[0], 0x11, 0, (uint32_t)x, (uint32_t)y, 0x14, (uint32_t)h, 0x004e4db8u, 0, cs_u(&self->axis), 0);
    cs_item_end(&it[1]);
    tcall<void>(F_UICC_AddItems, self, &it[0]);
}
static void fp_UpgradeCatalog_Added(Footprint& f, UpgradeCatalog*, Edx) { f.replay_only = "builds widgets (_UIAddItems allocates)"; }
PORT_FN(0x004c4480, "UpgradeCatalog::Added", UpgradeCatalog_Added_n, fp_UpgradeCatalog_Added)

static void __fastcall UpgradeCatalog_Callback_n(UpgradeCatalog* self, Edx, int32_t, const void*) { tcall<void>(F_UICC_Dirty, self); }
PORT_FN(0x004c4510, "UpgradeCatalog::Callback", UpgradeCatalog_Callback_n, fp_dirty<UpgradeCatalog>)

// the item under a point (a row every 95, scrolled by the axis in steps of 8), if the point is within the control's x
static __forceinline void hit(UpgradeCatalog* self, int32_t sx, int32_t x, int32_t y) {
    if (x < sx) return;
    if (self->w + sx <= x) return;
    const int32_t d = (int32_t)(((uint32_t)self->axis.pos << 3) - (uint32_t)self->y);
    self->hot = (int32_t)((uint32_t)y + (uint32_t)d) / 0x5f;
}
static void __fastcall UpgradeCatalog_MouseDown_n(UpgradeCatalog* self, Edx, int32_t x, int32_t y) {
    const int32_t sx = self->x;
    self->pressed = 1;
    self->hot = -1;
    hit(self, sx, x, y);
}
static void fp_catalog_xy(Footprint& f, UpgradeCatalog* self, Edx, int32_t, int32_t) { f.add(self, sizeof(UpgradeCatalog), "the catalog"); }
PORT_FN(0x004c4520, "UpgradeCatalog::MouseDown", UpgradeCatalog_MouseDown_n, fp_catalog_xy)

static void __fastcall UpgradeCatalog_MouseMove_n(UpgradeCatalog* self, Edx, int32_t x, int32_t y) {
    const int32_t sx = self->x;
    self->hot = -1;
    hit(self, sx, x, y);
}
PORT_FN(0x004c4570, "UpgradeCatalog::MouseMove", UpgradeCatalog_MouseMove_n, fp_catalog_xy)

static void __fastcall UpgradeCatalog_NotOver_n(UpgradeCatalog* self, Edx) { self->hot = -1; }
static void fp_catalog0(Footprint& f, UpgradeCatalog* self, Edx) { f.add(self, sizeof(UpgradeCatalog), "the catalog"); }
PORT_FN(0x004c45b0, "UpgradeCatalog::NotOver", UpgradeCatalog_NotOver_n, fp_catalog0)

// test mode: the right button toggles the upgrade under the mouse
static void __fastcall UpgradeCatalog_MouseRDown_n(UpgradeCatalog* self, Edx, int32_t x, int32_t y) {
    if (!ccall<uint8_t>(F_CareerIsTestMode)) return;
    const int32_t sx = self->x;
    self->hot = -1;
    hit(self, sx, x, y);
    const int32_t h = self->hot;
    // FIX CANDIDATE: the item's index is read before the item is checked (a hot item far outside the list reads outside
    // the control); the mouse is within the control's rows, so ordinary play only reads index[-1] (stamps[63])
    const int32_t k = self->index[h];
    if (h < 0) return;
    if (self->count <= h) return;
    uint8_t* a = cs_info();
    const uint8_t* b = cs_info();
    *(volatile uint8_t*)(a + k + 0x1c) = *(const volatile uint8_t*)(b + k + 0x1c) < 1 ? 1 : 0;
}
// (the item the mouse is over, as the original works it out, and its upgrade's byte)
static int32_t fp_hot(const UpgradeCatalog* self, int32_t x, int32_t y) {
    const int32_t sx = self->x;
    if (x < sx || self->w + sx <= x) return -1;
    const int32_t d = (int32_t)(((uint32_t)self->axis.pos << 3) - (uint32_t)self->y);
    return (int32_t)((uint32_t)y + (uint32_t)d) / 0x5f;
}
static void fp_UpgradeCatalog_MouseRDown(Footprint& f, UpgradeCatalog* self, Edx, int32_t x, int32_t y) {
    f.add(self, sizeof(UpgradeCatalog), "the catalog");
    const int32_t h = fp_hot(self, x, y);
    if (h >= 0 && h < self->count && h < 64) f.add((void*)(uintptr_t)(S_UPGRADES + (uint32_t)self->index[h]), 1, "the upgrade toggled");
}
PORT_FN(0x004c45c0, "UpgradeCatalog::MouseRDown", UpgradeCatalog_MouseRDown_n, fp_UpgradeCatalog_MouseRDown)

// a click on an item not yet bought: not allowed in this class, another upgrade needed first, or bought (a yes / no box
// and the funds checked); pressed is cleared, except where the item is already bought or a box explained why not
static void __fastcall UpgradeCatalog_MouseUp_n(UpgradeCatalog* self, Edx, int32_t x, int32_t y) {
    const int32_t sx = self->x;
    self->hot = -1;
    hit(self, sx, x, y);
    const int32_t h = self->hot;
    if (self->pressed != 0 && h >= 0 && self->count > h) {
        const int32_t k = self->index[h];
        const uint32_t k4 = (uint32_t)k << 2;
        const CshUpgrade* u = *(CshUpgrade* const volatile*)((const uint8_t*)cs_set() + k4 + 4);
        if (*(const volatile uint8_t*)(cs_info() + k + 0x1c) != 0) return;
        const uint8_t allowed = *(const volatile int32_t*)(cs_info() + 0x124) + 1 >= u->req_class ? 1 : 0;
        const char* req = u->requires;
        int32_t missing = -1;
        if (*(const volatile char*)req != 0) {
            const int32_t r = find_upgrade(req);
            if (*(const volatile uint8_t*)(cs_info() + r + 0x1c) == 0) missing = r;
        }
        if (!allowed) {
            cs_xl_once(S_UP_ONCE, 1, 0x005d5cc0, 0x005007b4, 0x004c4ee0);      // Upgrade:NotAllowed
            const uint32_t t = cs_xlate(0x005d5cc0);
            ccall<void>(F_UIDoOkBox, UI_GP(const char, 0x005d3d80u + k4), CS_CP(t));
            return;
        }
        if (missing >= 0) {
            cs_xl_once(S_UP_ONCE, 2, 0x005d5cd0, 0x00500788, 0x004c4ed0);      // Upgrade:OtherUpgradeRequiredDialog:Message
            const uint32_t t = cs_xlate(0x005d5cd0);
            char buf[0x100];
            // FIX CANDIDATE: the required upgrade's name and the message go into 0x100 bytes unbounded (translations
            // over 254 characters in all)
            UI_sprintf(buf, CS_CP(0x00500780), UI_GP(const char, 0x005d3d80u + ((uint32_t)missing << 2)), CS_CP(t));
            ccall<void>(F_UIDoOkBox, UI_GP(const char, 0x005d3d80u + k4), (const char*)buf);
            return;
        }
        cs_xl_once(S_UP_ONCE, 4, 0x005d5ce0, 0x0050076c, 0x004c4ec0);          // Upgrade:BuyQuery
        if (*(const volatile uint8_t*)(cs_info() + k + 0x1c) == 0) {
            const CshUpgrade* u2 = *(CshUpgrade* const volatile*)((const uint8_t*)cs_set() + k4 + 4);
            const int32_t price = u2->price;
            cs_xl_once(S_UP_ONCE, 8, 0x005d5cf0, 0x00500758, 0x004c4eb0);      // Upgrade:CantAfford
            if (ccall<uint8_t>(F_can_afford, price)) {
                const uint32_t t = cs_xlate(0x005d5ce0);
                if (ccall<uint8_t>(F_UIDoYesNoBox, UI_GP(const char, 0x005d3d80u + k4), CS_CP(t), (const void*)0)) {
                    ccall<void>(F_debit_account, price);
                    *(volatile uint8_t*)(cs_info() + k + 0x1c) = 1;
                    CS_CareerLog(CS_CP(0x00500748), UI_GP(const char, 0x005d3d80u + k4));   // "Purchased %s"
                }
            } else {
                const uint32_t t = cs_xlate(0x005d5cf0);
                ccall<void>(F_UIDoOkBox, UI_GP(const char, 0x005d3d80u + k4), CS_CP(t));
            }
        }
    }
    self->pressed = 0;
}
// a click that reaches a box (or an unbuilt Xlator) is left to the session replays; one that doesn't writes the catalog
static void fp_UpgradeCatalog_MouseUp(Footprint& f, UpgradeCatalog* self, Edx, int32_t x, int32_t y) {
    f.add(self, sizeof(UpgradeCatalog), "the catalog");
    const int32_t h = fp_hot(self, x, y);
    if (!self->pressed || h < 0 || self->count <= h) return;
    const int32_t k = h < 64 ? self->index[h] : 0;
    if (h >= 64 || k < -1 || k > 0xff) { f.replay_only = "an item outside the catalog's arrays"; return; }
    if (*(const volatile uint8_t*)(uintptr_t)(S_UPGRADES + (uint32_t)k) != 0) return;
    f.replay_only = "a click on an item not bought: a message box, or the yes / no box and the purchase";
}
PORT_FN(0x004c4640, "UpgradeCatalog::MouseUp", UpgradeCatalog_MouseUp_n, fp_UpgradeCatalog_MouseUp)

// each item (from the scroll position, 95 apart): the hot one's frame, its picture, name, description (word-wrapped to
// 250), price, state and requirement; separators between them. Style bit 1: not allowed in this class, 2: not bought
static void __fastcall UpgradeCatalog_Draw_n(UpgradeCatalog* self, Edx, gxCanvas* c) {
    char buf[0x800];
    cs_xl_once(S_DRAW_ONCE, 0x01, 0x005d5c70, 0x0050082c, 0x004c4ea0);    // Upgrade:Price
    cs_xl_once(S_DRAW_ONCE, 0x02, 0x005d5c90, 0x00500818, 0x004c4e90);    // Upgrade:Purchased
    cs_xl_once(S_DRAW_ONCE, 0x04, 0x005d5ca0, 0x00500804, 0x004c4e80);    // Upgrade:Available
    cs_xl_once(S_DRAW_ONCE, 0x08, 0x005d5c80, 0x005007f4, 0x004c4e70);    // Upgrade:Class
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    const int32_t x = self->x;
    int32_t top = self->y;
    int32_t i = 0;
    top = (int32_t)((uint32_t)top - ((uint32_t)self->axis.pos << 3));
    const int32_t w0 = self->w;
    if (self->count > 0) {
        void* volatile* sp = &self->stamps[0];
        const int32_t xt = x + 5;
        const int32_t xr = w0 + x - 7;
        do {
            const int32_t k = *(const volatile int32_t*)((const volatile uint8_t*)sp + 0x100);
            const uint8_t* item = *(const uint8_t* const volatile*)((const uint8_t*)cs_set() + (uint32_t)k * 4u + 4);
            const int32_t rc = (*(CshUpgrade* const volatile*)((const uint8_t*)cs_set() + (uint32_t)k * 4u + 4))->req_class;
            const uint8_t allowed = rc <= *(const volatile int32_t*)(cs_info() + 0x124) + 1 ? 1 : 0;
            const uint8_t bought = *(const volatile uint8_t*)(cs_info() + k + 0x1c);
            uint32_t st = allowed ? 0u : 1u;
            if (self->hot == i && allowed) {
                const uint32_t col = self->pressed ? UI_GU32(S_C_FRAME_DOWN) : UI_GU32(S_C_FRAME);
                ccall<void>(F_gxHollowRect, x, top + 2, self->w + x - 1, top + 0x5d, col);
            }
            if (void* s = *sp) ccall<void>(F_gxDrawStamp, s, x + 0xb9, top + 5, (int32_t)0, (const void*)0);
            ccall<void>(F_UIStyleDraw, (int32_t)0xc, xt, top + 3, UI_GP(const char, S_UP_NAMES + (uint32_t)k * 4u), st);
            ccall<void>(F_UIStyleWordWrap, (int32_t)0x13, (int32_t)0xfa, buf, UI_GP(const char, S_UP_DESCS + (uint32_t)k * 4u));
            ccall<void>(F_UIStyleDraw, (int32_t)0x13, xr, top + 4, (const char*)buf, (uint32_t)0);
            cs_xlate(0x005d5c70);
            {
                const int32_t price = (*(CshUpgrade* const volatile*)((const uint8_t*)cs_set() + (uint32_t)k * 4u + 4))->price;
                const char* m = ccall<const char*>(F_money_string, price);
                UI_sprintf(buf, CS_CP(0x005007ec), UI_GP(const char, 0x005d5c74), m);       // "%s: %s"
            }
            ccall<void>(F_UIStyleDraw, (int32_t)0xc, xt, top + 0x12, (const char*)buf, st);
            if (!allowed && !bought) {
                const uint32_t t = cs_xlate(0x005d5c80);
                const int32_t cls = (*(CshUpgrade* const volatile*)((const uint8_t*)cs_set() + (uint32_t)k * 4u + 4))->req_class - 1;
                const char* cn = ccall<const char*>(F_CareerGetClassName, cls);
                UI_sprintf(buf, CS_CP(0x00500780), cn, CS_CP(t));                          // "%s %s"
            } else {
                if (*(const volatile uint8_t*)(cs_info() + k + 0x1c) == 0) st |= 2;
                if (allowed) {
                    const uint32_t t = *(const volatile uint8_t*)(cs_info() + k + 0x1c) ? cs_xlate(0x005d5c90) : cs_xlate(0x005d5ca0);
                    UI_sprintf(buf, CS_CP(0x004e59ac), CS_CP(t));                          // "%s"
                } else {
                    crt_strcpy(buf, CS_CP(0x005007e0));                                     // "Illegal!!"
                }
            }
            ccall<void>(F_UIStyleDraw, (int32_t)0xc, xt, top + 0x46, (const char*)buf, st);
            const char* req = (const char*)(item + 0x40);
            if (*(const volatile char*)req != 0) {
                cs_xl_once(S_DRAW_ONCE, 0x10, 0x005d5cb0, 0x005007c8, 0x004c4e60);    // Upgrade:RequiresPrefix
                const int32_t r = find_upgrade(req);
                const uint32_t t = cs_xlate(0x005d5cb0);
                UI_sprintf(buf, CS_CP(0x00500780), CS_CP(t), UI_GP(const char, S_UP_NAMES + (uint32_t)r * 4u));
                ccall<void>(F_UIStyleDraw, (int32_t)0x12, xt, top + 0x32, (const char*)buf, st);
            }
            i++;
            if (self->count > i) {
                const int32_t ly = top + 0x5f;
                const uint32_t col = UI_GU32(S_C_FRAME_DOWN);
                const int32_t lx = self->w + x - 5;
                ccall<void>(F_gxLine, xt, ly, lx, ly, col);
            }
            top += 0x5f;
            sp++;
        } while (self->count > i);
    }
}
static void fp_UpgradeCatalog_Draw(Footprint& f, UpgradeCatalog*, Edx, gxCanvas* c) {
    if (!cs_xl_built(S_DRAW_ONCE, 0x1f) || !cs_xl_built(S_CLASS_XL_GUARD, 0x0f)) {
        f.replay_only = "the first call builds its Xlators (atexit)";
        return;
    }
    fp_draw_canvas(f, c);
    CS_FP_XL(f, 0x005d5c70); CS_FP_XL(f, 0x005d5c80); CS_FP_XL(f, 0x005d5c90); CS_FP_XL(f, 0x005d5ca0); CS_FP_XL(f, 0x005d5cb0);
    CS_FP_XL(f, 0x005d0038); CS_FP_XL(f, 0x005cfe28); CS_FP_XL(f, 0x005cf068); CS_FP_XL(f, 0x005cf6f0);   // CareerGetClassName's
    UI_FP(f, S_MONEY, 0x40, "money_string's buffer");
}
PORT_FN(0x004c4990, "UpgradeCatalog::Draw", UpgradeCatalog_Draw_n, fp_UpgradeCatalog_Draw)

static void* __fastcall UpgradeCatalog_sdd_n(UpgradeCatalog* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_UpgradeCatalog;
    for (int32_t k = 0; self->count > k; k++)
        if (void* s = self->stamps[k]) ccall<void>(F_gxForgetStamp, s);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x004c4ef0, "UpgradeCatalog::scalar deleting destructor", UpgradeCatalog_sdd_n, fp_frees<UpgradeCatalog>)

// =========================================================================================================================
// the function-local Xlators' destructor helpers: each a bare `ret` (the $E atexit functions destroy the Xlators)
// =========================================================================================================================
static void __cdecl xlh_stock() {}
PORT_FN(0x004c42d0, "UpgradeSummary::draw_class::XL_Stock (destructor helper)", xlh_stock, fp_pure0)
static void __cdecl xlh_requires() {}
PORT_FN(0x004c4e60, "UpgradeCatalog::Draw::XL_RequiresPrefix (destructor helper)", xlh_requires, fp_pure0)
static void __cdecl xlh_class() {}
PORT_FN(0x004c4e70, "UpgradeCatalog::Draw::XL_Class (destructor helper)", xlh_class, fp_pure0)
static void __cdecl xlh_available() {}
PORT_FN(0x004c4e80, "UpgradeCatalog::Draw::XL_Available (destructor helper)", xlh_available, fp_pure0)
static void __cdecl xlh_purchased() {}
PORT_FN(0x004c4e90, "UpgradeCatalog::Draw::XL_Purchased (destructor helper)", xlh_purchased, fp_pure0)
static void __cdecl xlh_price() {}
PORT_FN(0x004c4ea0, "UpgradeCatalog::Draw::XL_Price (destructor helper)", xlh_price, fp_pure0)
static void __cdecl xlh_cant_afford() {}
PORT_FN(0x004c4eb0, "UpgradeCatalog::MouseUp::XL_CantAfford (destructor helper)", xlh_cant_afford, fp_pure0)
static void __cdecl xlh_buy_query() {}
PORT_FN(0x004c4ec0, "UpgradeCatalog::MouseUp::XL_BuyQuery (destructor helper)", xlh_buy_query, fp_pure0)
static void __cdecl xlh_message() {}
PORT_FN(0x004c4ed0, "UpgradeCatalog::MouseUp::XL_Message (destructor helper)", xlh_message, fp_pure0)
static void __cdecl xlh_not_allowed() {}
PORT_FN(0x004c4ee0, "UpgradeCatalog::MouseUp::XL_NotAllowed (destructor helper)", xlh_not_allowed, fp_pure0)

}  // namespace career_shop
}  // namespace
