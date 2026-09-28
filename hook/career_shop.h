// career_shop.h -- M3 UI stage, step U4 (group B): the upgrade shop's and the credits' layouts, statics and addresses
// (library `career`: upgrade.obj, credits.obj; library `intro`: credits.obj, intro.obj), shared by hook/career_shop.cpp,
// hook/career_credits.cpp and test/world_career_shop.cpp. Group A's classes and structures (career_types.h) appear here
// only as the fields this group touches, prefixed Csh.
//
// Recovered from the v1.0 disassembly (UpgradeDo's and upgrade_page's frames, UpgradeCatalog's constructor, the credits'
// tables). Fields are volatile, as in ui_types.h: the rewrites read and write each one where the original does.
//
// Classes (vtable, size), UICustomControls (base 0x4db190, 0x18):
//   UpgradeSummary 0x4dfc68 0x18     the car's engine figures and its upgrades by class (UpgradeDo's left panel)
//   UpgradeCatalog 0x4dfcb8 0x234    a page of upgrades: picture, name, description, price, state (upgrade_page)
//
// CatalogItem (8): {upgrade name, stamp name or 0}; a page's list ends with a null name (the callbacks build it on their
// stack). CarUpgradeSet (the car file's viper.ugs): {count, CarUpgrade* [count]}; entry 0 is never shown (the loops
// start at 1). CarUpgrade: +0 its name, +0x40 the upgrade it requires ("" none), +0x60 its section (0 power, 2 handling,
// 1 the rest: UpgradeSummary's three lists), +0x64 the class it needs (1..4: the career's class + 1 must reach it),
// +0x68 its price. CareerInfo (0x5cf078, group A's): +0 the name, +0x18 the funds, +0x1c the upgrades bought (u8[256]),
// +0x124 the class, +0x128 the next event, +0x530 the player's place in each event (drivers[7]).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace cshop {
using uit::Edx;

struct CatalogItem {                              // 8
    const char* name;                             // +0 the upgrade (CarUpgrade's name), 0 ends the list
    const char* stamp;                            // +4 its picture, or 0
};
static_assert(sizeof(CatalogItem) == 8, "CatalogItem");

struct UpgradeCatalog : uit::UICustomControl {    // 0x234, vtable 0x4dfcb8
    uit::UIScrollAxis axis;                       // +0x18 total = count * 95 / 8, visible = h / 8, pos (the scroll bar's)
    const CatalogItem* volatile items;            // +0x24
    volatile int32_t count;                       // +0x28 the items (64 at most: the arrays below)
    void* volatile stamps[64];                    // +0x2c
    volatile int32_t index[64];                   // +0x12c each item's upgrade (-1: not in the car's set)
    volatile uint8_t pressed;                     // +0x22c
    uint8_t _22d[3];
    volatile int32_t hot;                         // +0x230 the item under the mouse, -1
};
static_assert(sizeof(UpgradeCatalog) == 0x234 && offsetof(UpgradeCatalog, index) == 0x12c &&
              offsetof(UpgradeCatalog, pressed) == 0x22c, "UpgradeCatalog");

// a CarUpgrade: the fields read here
struct CshUpgrade {
    char name[0x40];                              // +0
    char requires[0x20];                          // +0x40
    volatile int32_t section;                     // +0x60
    volatile int32_t req_class;                   // +0x64
    volatile int32_t price;                       // +0x68
};
static_assert(offsetof(CshUpgrade, section) == 0x60 && offsetof(CshUpgrade, price) == 0x68, "CshUpgrade");
struct CshUpgradeSet {
    volatile int32_t count;                       // +0
    CshUpgrade* volatile items[1];                // +4 [count]
};

// the credits: an item {type, text}. Type 0 a centred line, 3 a left-aligned one (both in the credits' style, 3 below),
// 1 a gap of 15, 2 one of 60, 4 a picture (the text is its stamp), 5 nothing; any other is -1 high and drawn as nothing
struct CreditItem {                               // 8
    volatile int32_t type;
    const char* volatile text;
};
static_assert(sizeof(CreditItem) == 8, "CreditItem");
struct RandomCreditTableItem {                    // 0xc: a block of items put in a random order (add_from_random)
    const CreditItem* volatile items;             // +0
    volatile uint32_t bytes;                      // +4
    volatile uint8_t used;                        // +8 (the rest of the dword is never written)
    uint8_t _9[3];
};
static_assert(sizeof(RandomCreditTableItem) == 0xc, "RandomCreditTableItem");

enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_CareerStatus = 0x004dfa08, VT_UpgradeSummary = 0x004dfc68,
    VT_UpgradeCatalog = 0x004dfcb8,
};

// ---- statics (v1.0) -------------------------------------------------------------------------------------------------------
enum : uint32_t {
    // group A's (career.obj)
    S_INFO = 0x005cf078,             // CareerInfo
    S_FUNDS = 0x005cf090,            // +0x18
    S_UPGRADES = 0x005cf094,         // +0x1c, u8[256]
    S_CLASS_XL_GUARD = 0x005cfe64,   // CareerGetClassName's four Xlators (bits 1..8)
    S_UPGRADE_SET = 0x005cffcc,      // CarUpgradeSet* (CareerGetUpgradeSet's answer)
    S_SEASON = 0x005cf700,           // CareerSeason* (CareerGetSeason's answer)
    // upgrade.obj
    S_UP_DESCS = 0x005d3868,         // const char*[256]: "Upgrades:<name>:Desc" translated (get_upgrade_names)
    S_UP_NAMES = 0x005d3d80,         // const char*[256]: "Upgrades:<name>:Name" translated
    S_C_FRAME_DOWN = 0x005d3864,     // colour: the hot item's frame while pressed, the items' separators
    S_C_FRAME = 0x005d3c68,          // colour: the hot item's frame
    S_MONEY = 0x005d37a0,            // money_string's buffer (0x40, LocaleMoney's)
    S_DO_ONCE = 0x005d3768,          // UpgradeDo's Xlators (bits 1..0x80)
    S_DO_ONCE2 = 0x005d3cec,         // ... (bits 1..4)
    S_PAGE_ONCE = 0x005d3d50,        // upgrade_page's "UI:Back"
    S_STOCK_ONCE = 0x005d5cfc,       // draw_class's (and Draw's) "Upgrades:Stock"
    S_UP_ONCE = 0x005d5cdc,          // UpgradeCatalog::MouseUp's (bits 1..8)
    S_DRAW_ONCE = 0x005d5cac,        // UpgradeCatalog::Draw's (bits 1..0x10)
    // career credits.obj
    S_CR_ONCE = 0x005d4204,          // CreditsDo(int)'s Xlators (bits 1, 2, 4)
    S_CR_C1 = 0x005d4208, S_CR_C2 = 0x005d420c, S_CR_C3 = 0x005d4218,   // its style's colours ($E)
    // intro credits.obj
    S_CREDITS = 0x005d45d0,          // CreditItem[0x200]: the credits as shown
    S_CREDITS_END = 0x005d55d0,
    S_CREDITS_N = 0x005d55d0,        // their count
    S_CREDITS_H = 0x005d55d4,        // their total height
    S_IC_C1 = 0x005d55dc, S_IC_C2 = 0x005d45bc, S_IC_C3 = 0x005d45ac,   // the style's colours ($E); C3 also the clear colour
};

// ---- the game's functions (v1.0) ------------------------------------------------------------------------------------------
enum : uint32_t {
    // group A's
    F_CareerGetInfo = 0x004bc470, F_CareerGetSeason = 0x004bc480, F_CareerGetUpgradeSet = 0x004bc490,
    F_CareerLog = 0x004bc4f0, F_CareerGetClassName = 0x004bd350, F_CareerGetCarName = 0x004bd4f0,
    F_CareerIsTestMode = 0x004be410,
    // elsewhere
    F_Win32Idle = 0x00412bf0, F_ResourceExists = 0x00419d10, F_LocaleMoney = 0x0041ab50,
    F_LocaleFormatShortDate = 0x0041ac10, F_Xlate = 0x0041aec0, F_Random = 0x0041b6e0, F_CarFileLoad = 0x00465100,
    F_CarFileMakeDefaultSetup = 0x00465780, F_SoundUnMuteCars = 0x00471de0, F_Sound_Toss = 0x004724c0,
    F_UIDoOkBox = 0x004793c0, F_UIDoYesNoBox = 0x00479b30, F_UIAddDynamicStyle = 0x0047f4a0,
    F_UICC_Dirty = 0x0047e820, F_UICC_AddNotification = 0x0047e840,
    F_UICC_AddItems = 0x0047e870, F_PaintKitDo = 0x004c62d0, F_TimeGetTimeOfDay = 0x004d8af0, F_stricmp = 0x004da350,
    // upgrade.obj (called by address, as the original does, so a hooked rewrite is what runs)
    F_UpgradeDo = 0x004c29b0, F_give_money = 0x004c3090, F_get_upgrade_names = 0x004c3160, F_paint_cb = 0x004c31f0,
    F_engine_cb = 0x004c3210, F_upgrade_page = 0x004c3320, F_gxHollowRect = 0x004c3570, F_can_afford = 0x004c35d0,
    F_debit_account = 0x004c35f0, F_money_string = 0x004c3600, F_transmission_cb = 0x004c3640,
    F_suspension_cb = 0x004c3700, F_wheels_cb = 0x004c37f0, F_body_cb = 0x004c38b0,
    F_UpgradeSummary_draw_class = 0x004c4150, F_UpgradeCatalog_ctor = 0x004c4300,
    // intro credits.obj
    F_CreditsDo_intro = 0x004cc600, F_fillout_credits = 0x004cc7b0, F_add_from_random = 0x004cc990,
    F_item_height = 0x004cca00, F_draw_credits = 0x004cca90, F_draw_item = 0x004ccbb0, F_mouse_hit_credits = 0x004ccc70,
};

// ---- helpers ----------------------------------------------------------------------------------------------------------------
#define CS_CP(a) ((const char*)(uintptr_t)(a))
#define CS_VP(a) ((void*)(uintptr_t)(a))
static __forceinline uint32_t cs_u(const volatile void* p) { return (uint32_t)(uintptr_t)p; }

// a function-local static Xlator, built on first use: the guard's bit set, the constructor, its destructor at exit
static __forceinline void cs_xl_once(uint32_t guard, uint8_t bit, uint32_t xl, uint32_t key, uint32_t dtor) {
    if (!(UI_G8(guard) & bit)) {
        UI_G8(guard) = (uint8_t)(UI_G8(guard) | bit);
        uit::tcall<void*>(uit::F_Xlator_ctor, CS_VP(xl), CS_CP(key));
        uit::ccall<int>(uit::F_atexit, dtor);
    }
}
static __forceinline bool cs_xl_built(uint32_t guard, uint8_t bits) { return (UI_G8(guard) & bits) == bits; }
// an Xlator refreshed as the inlined code does it (its cookie compared, xlate called if stale), then its text read
static __forceinline uint32_t cs_xlate(uint32_t xl) {
    if (UI_GU32(xl + 8) != UI_GU32(uit::S_XLATOR_COOKIE)) uit::tcall<void>(uit::F_Xlator_xlate, CS_VP(xl));
    return UI_GU32(xl + 4);
}
#define CS_FP_XL(f, a) UI_FP(f, a, 12, "a static Xlator")

// UIDialogItem::UIDialogItem (replay.obj) by address: every argument as the original pushes it
static __forceinline void cs_item_ctor(void* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                                       uint32_t text, int32_t i1c, uint32_t data, int32_t style) {
    uit::tcall<void*>(uit::F_UIDialogItem_ctor, it, type, id, x, y, w, h, text, i1c, data, style, 0u, 0u, 0u, 0u);
}
// an item the compiler inlined: its 14 dwords stored
static __forceinline void cs_item14(void* it, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    uint32_t text, uint32_t i1c, uint32_t data, uint32_t style) {
    volatile uint32_t* d = (volatile uint32_t*)it;
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text;
    d[7] = i1c; d[8] = data; d[9] = style; d[10] = 0; d[11] = 0; d[12] = 0; d[13] = 0;
}
// the end of an item list: the static item at 0x578d08, copied (rep movsd)
static __forceinline void cs_item_end(void* it) { uit::crt_copy(it, (const void*)(uintptr_t)uit::S_END_ITEM, 0x38); }
// a UIDialog {title, background, default, items, idle} written in a frame
static __forceinline void cs_dialog(void* d, uint32_t title, uint32_t bg, int32_t def, uint32_t items) {
    volatile uint32_t* v = (volatile uint32_t*)d;
    v[0] = title; v[1] = bg; v[2] = (uint32_t)def; v[3] = items; v[4] = 0;
}

// a frame laid out as the original's: F is its esp after the prologue's pushes, `off` the original's offset from it
#define CS_W(F, off) (*(volatile uint32_t*)((F) + (off)))
#define CS_B(F, off) (*(volatile uint8_t*)((F) + (off)))
#define CS_A(F, off) ((uint32_t)(uintptr_t)((F) + (off)))

// CareerGetUpgradeSet's set, called where the original calls it (each call is a read of the career's static)
static __forceinline CshUpgradeSet* cs_set() { return uit::ccall<CshUpgradeSet*>(F_CareerGetUpgradeSet); }
static __forceinline uint8_t* cs_info() { return uit::ccall<uint8_t*>(F_CareerGetInfo); }

}  // namespace cshop
