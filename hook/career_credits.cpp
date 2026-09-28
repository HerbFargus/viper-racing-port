// career_credits.cpp -- M3 UI stage, step U4 (group B): the career's prize screen (library `career`: credits.obj), the
// game's credits and the intro (library `intro`: credits.obj, intro.obj), rewritten faithfully.
//
//   career credits.obj: CreditsDo(int) -- the money a race or a season paid: "Career:Money:<amount>" translated, the
//   date, the prize as the locale writes money, "for place <n>" / "for rank <n>" (a translated format), in a dialog of
//   cour14 texts on credit.stp with Ok and Return; shown only when the prize is above 0. intro credits.obj: CreditsDo()
//   -- the credits rolling over credits.stp (euro12, their own style) until they've all gone by, a key or a click; the
//   music (*trk2.sfx) once the first frame is up --, fillout_credits (the fixed blocks and six blocks in a random order
//   copied into the table; their total height), add_from_random, item_height, draw_credits (the scroll: 30 pixels a
//   second after a second's pause, in a 520 x 400 window), draw_item, mouse_hit. intro.obj: IntroPlayVideo (a bare `ret`:
//   the intro video was cut).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own too, so a
// hooked rewrite is what runs), the compiler's inline copies (rep movsd / movsb) as it runs them (ui_types.h). The two
// CreditsDo frames are laid out as the originals' (a byte array addressed by the original's offsets): the style's
// description, the dialog, its items, the texts and the canvas where the original puts them. Integer arithmetic wraps at
// 32 bits where the original's does (the credits' height times 1000, the screen's centre); divisions are the original's
// signed ones (idiv, the compiler's halving).
//
// Footprints (main thread). replay_only: both CreditsDo (a dialog / their own loop, a style added and removed), mouse_hit
// (takes a mouse event), item_height / draw_item of a picture and draw_credits / fillout_credits where the table has one
// (they load its stamp; the game's own tables have none). Shadow-checkable: fillout_credits and add_from_random (the table,
// its count and height; add_from_random the blocks' used flags; Random's numbers are fed to the rewrite), item_height
// (nothing), draw_credits (the canvas's clip, the current canvas), draw_item (the current canvas). IntroPlayVideo is pure.
//
// FIX CANDIDATEs (left faithful, marked in place): add_from_random never ends when a block is already used or Random can't
// reach one, and it copies past the table's 512 items unchecked (over the count) -- the only caller hands it six fresh
// blocks after 8 items: ordinary play can't; fillout_credits panics only after it has filled more than the table's 512
// items (its blocks are fixed: 328).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "career_shop.h"

namespace {
namespace career_credits {
using namespace uit;
using namespace cshop;

// =========================================================================================================================
// career credits.obj
// =========================================================================================================================
// the prize a race (which < 0: the event just run, the player's place in it) or a season (which: the rank) paid. The frame
// (esp after the prologue): +0x10 the UIDialog, +0x24 the style's description, +0x58 the time of day, +0x6c the items (8),
// +0x22c the money (0x40), +0x26c the line under it (0x100), +0x36c "Career:Money:<n>" (0x100), +0x46c the date (0x100)
static void __cdecl CreditsDo_career_n(int32_t which) {
    alignas(8) uint8_t fr[0x56c];
    uint8_t* const F = fr;
    const int32_t ev = (int32_t)((uint32_t)*(const volatile int32_t*)(cs_info() + 0x128) - 1u);
    if (ev < 0) UI_LogPanic(CS_CP(0x005008e4));                                   // "event < 0"
    int32_t prize = 0;
    const uint8_t* event = (const uint8_t*)ccall<void*>(F_CareerGetSeason) + (uint32_t)ev * 0x58u + 0x24u;
    const int32_t place = *(const volatile int32_t*)(cs_info() + (uint32_t)ev * 4u + 0x530u);
    if (which >= 0) prize = *(const volatile int32_t*)((const uint8_t*)ccall<void*>(F_CareerGetSeason) + (uint32_t)which * 4u + 4u);
    else if ((int32_t)((uint32_t)place - 1u) >= 0) prize = *(const volatile int32_t*)(event + (uint32_t)place * 4u + 0x14u);
    UI_sprintf((char*)(F + 0x36c), CS_CP(0x005008f0), prize);                    // "Career:Money:%d"
    const uint32_t money_text = ccall<uint32_t>(F_Xlate, (const char*)(F + 0x36c));
    cs_xl_once(S_CR_ONCE, 1, 0x005d4248, 0x00500900, 0x004c5c10);                // Career:Credits:RacePrize
    cs_xl_once(S_CR_ONCE, 2, 0x005d4238, 0x0050091c, 0x004c5c00);                // Career:Credits:SeasonPrize
    cs_xl_once(S_CR_ONCE, 4, 0x005d4220, 0x00500938, 0x004c5bf0);                // UI:Ok
    ccall<void>(F_TimeGetTimeOfDay, (void*)(F + 0x58));
    {
        const uint32_t year = CS_W(F, 0x58) & 0xffffu;
        const uint32_t month = *(const volatile uint16_t*)(F + 0x5a);
        const uint32_t day = CS_W(F, 0x5c) & 0xffffu;
        ccall<void>(F_LocaleFormatShortDate, (char*)(F + 0x46c), (int32_t)0x100, day, month, year);
    }
    ccall<void>(F_LocaleMoney, (char*)(F + 0x22c), prize, (uint32_t)0);
    {
        uint32_t t;
        int32_t n;
        if (which < 0) {
            t = cs_xlate(0x005d4248);
            n = place;
        } else {
            t = cs_xlate(0x005d4238);
            n = which + 1;
        }
        UI_sprintf((char*)(F + 0x26c), CS_CP(t), n);
    }
    {
        const uint32_t c1 = UI_GU32(S_CR_C1), c2 = UI_GU32(S_CR_C2), c3 = UI_GU32(S_CR_C3);
        volatile uint32_t* d = (volatile uint32_t*)(F + 0x24);
        d[0] = 0x00500940u;                                                      // cour14.fnt
        d[1] = 0x21; d[2] = c1; d[3] = c2; d[4] = c3; d[5] = 0; d[6] = 0; d[7] = c1; d[8] = c1; d[9] = c3;
        d[10] = 0; d[11] = 0; d[12] = 0;
    }
    const int32_t style = ccall<int32_t>(F_UIAddDynamicStyle, (const void*)(F + 0x24));
    const uint32_t info = cs_u(cs_info());
    cs_item14(F + 0x6c, 5, 0, 0x4b, 0x57, 0, 0, info, 0, 0, (uint32_t)style);                 // the player's name
    cs_item14(F + 0xa4, 5, 0, 0x16f, 0x40, 0, 0, CS_A(F, 0x46c), 0, 0, (uint32_t)style);       // the date
    cs_item14(F + 0xdc, 5, 0, 0x37, 0x9a, 0, 0, CS_A(F, 0x26c), 0, 0, (uint32_t)style);        // what it's for
    cs_item14(F + 0x114, 5, 0, 0x1ab, 0x58, 0, 0, CS_A(F, 0x22c), 0, 0, (uint32_t)style);      // the money
    cs_item14(F + 0x14c, 5, 0, 0x25, 0x69, 0, 0, money_text, 0, 0, (uint32_t)style);          // in words
    cs_item_ctor(F + 0x184, 2, -1, 0x177, 0x82, 0, 0, cs_xlate(0x005d4220), 0, 0, 0);
    cs_item14(F + 0x1bc, 4, (uint32_t)-1, 0xd, 0, 0, 0, 0, 0, 0, 0);                         // Return
    cs_item_end(F + 0x1f4);
    cs_dialog(F + 0x10, 0x0050094cu, 0x00500950u, -2, CS_A(F, 0x6c));                          // "", credit.stp
    if (prize > 0)
        ccall<int32_t>(F_UIDoDialog, (const void*)(F + 0x10), (int32_t)0x1f6, (int32_t)0xbb, (int32_t)-999, (int32_t)-999, (int32_t)1);
    ccall<void>(F_UIRemoveStyle, style);
}
static void fp_CreditsDo_career(Footprint& f, int32_t) { f.replay_only = "adds a style (loads a font), runs a dialog (modal)"; }
PORT_FN(0x004c5660, "CreditsDo(int)", CreditsDo_career_n, fp_CreditsDo_career)

// =========================================================================================================================
// intro credits.obj
// =========================================================================================================================
// the credits over credits.stp (or the clear colour) until they've gone by, a key or a click. The frame (esp after the
// prologue): +0x13 the music started, +0x14 the style's description, +0x48 the screen's canvas
static void __cdecl CreditsDo_intro_n() {
    alignas(8) uint8_t fr[0x6c];
    uint8_t* const F = fr;
    void* const stamp = ccall<void*>(F_gxGetStamp, CS_CP(0x00502424));             // credits.stp
    {
        const uint32_t c2 = UI_GU32(S_IC_C2), c1 = UI_GU32(S_IC_C1), c3 = UI_GU32(S_IC_C3);
        volatile uint32_t* d = (volatile uint32_t*)(F + 0x14);
        d[0] = 0x00502430u;                                                      // euro12.fnt
        d[1] = 0xc; d[2] = c1; d[3] = c2; d[4] = c3;
        for (int k = 5; k < 13; k++) d[k] = 0;
    }
    const int32_t style = ccall<int32_t>(F_UIAddDynamicStyle, (const void*)(F + 0x14));
    ccall<void>(F_fillout_credits, style);
    int32_t t0 = ccall<int32_t>(F_PTimeNow);
    ccall<void>(F_KeyClear);
    ccall<void>(F_MouseClear);
    CS_B(F, 0x13) = 0;
    gxCanvas* const cv = (gxCanvas*)(F + 0x48);
    for (;;) {
        uint8_t done = 0;
        ccall<void>(F_Win32Idle);
        if (ccall<uint8_t>(F_gxGrabScreen, cv)) {
            ccall<gxCanvas*>(F_gxSetCanvas, cv);
            const uint32_t col = UI_GU32(S_IC_C3);
            if (stamp) {
                const int32_t sh = UI_G32(S_SCREEN_H);
                const int32_t y = (int32_t)((uint32_t)sh - (uint32_t)ccall<int32_t>(F_gxStampHeight, (const void*)stamp)) / 2;
                const int32_t sw = UI_G32(S_SCREEN_W);
                const int32_t x = (int32_t)((uint32_t)sw - (uint32_t)ccall<int32_t>(F_gxStampWidth, (const void*)stamp)) / 2;
                ccall<void>(F_gxDrawStamp, (const void*)stamp, x, y, (int32_t)0, (const void*)0);
            } else {
                ccall<void>(F_gxClear, col);
            }
            const int32_t t = (int32_t)((uint32_t)ccall<int32_t>(F_PTimeNow) - (uint32_t)t0);
            done = ccall<uint8_t>(F_draw_credits, cv, t, style);
            ccall<void>(F_gxReleaseScreen);
            ccall<void>(F_gxFlip);
            if (CS_B(F, 0x13) == 0) {
                CS_B(F, 0x13) = 1;
                ccall<void>(F_SoundUnMuteCars);
                if (ccall<uint8_t>(F_ResourceExists, CS_CP(0x0050243c)))            // *trk2.sfx
                    ccall<void>(F_Sound_Toss, CS_CP(0x0050243c), (int32_t)-1);
                t0 = ccall<int32_t>(F_PTimeNow);
            }
        }
        if (done) break;
        if (ccall<uint8_t>(F_KeyHit)) break;
        if (ccall<uint8_t>(F_mouse_hit_credits)) break;
    }
    if (stamp) ccall<void>(F_gxForgetStamp, stamp);
    ccall<void>(F_UIRemoveStyle, style);
    ccall<void>(F_KeyClear);
    ccall<void>(F_MouseClear);
}
static void fp_CreditsDo_intro(Footprint& f) { f.replay_only = "runs its own loop (input, the screen, the music), adds a style"; }
PORT_FN(0x004cc600, "CreditsDo(void)", CreditsDo_intro_n, fp_CreditsDo_intro)

// the table: the heading block (8 items), six blocks in a random order, then the fixed blocks; the count checked, the
// total height summed
static void __cdecl fillout_credits_n(int32_t style) {
    RandomCreditTableItem t[6];
    UI_GU32(S_CREDITS_N) = 0;
    UI_GU32(S_CREDITS_H) = 0;
    crt_copy(CS_VP(S_CREDITS), CS_VP(0x005010a8), 0x40);
    static const uint32_t blocks[6][2] = {{0x005010e8, 0x30}, {0x00501118, 0x38}, {0x00501150, 0x38},
                                          {0x005011b8, 0x30}, {0x005011e8, 0x38}, {0x00501188, 0x30}};
    for (int k = 0; k < 6; k++) { t[k].bytes = blocks[k][1]; t[k].used = 0; }
    UI_G32(S_CREDITS_N) += 8;
    for (int k = 0; k < 6; k++) t[k].items = (const CreditItem*)(uintptr_t)blocks[k][0];
    ccall<void>(F_add_from_random, (void*)t, (int32_t)6);
    static const uint32_t fixed[7][2] = {{0x00501830, 0x7e}, {0x00501798, 0x26}, {0x00501240, 0xfe}, {0x00501a58, 0x58},
                                         {0x00501a28, 0xc}, {0x00501708, 0x24}, {0x00501220, 8}};
    for (int k = 0; k < 7; k++) {
        const uint32_t n = UI_GU32(S_CREDITS_N);
        crt_copy(CS_VP(n * 8u + S_CREDITS), CS_VP(fixed[k][0]), fixed[k][1] * 4u);
        UI_G32(S_CREDITS_N) += (int32_t)(fixed[k][1] / 2u);
    }
    // FIX CANDIDATE: the check comes after the copies (a table of more than 512 items has already overrun it); the blocks
    // are the game's own, 328 items in all
    if (UI_GU32(S_CREDITS_N) > 0x200u) UI_LogPanic(CS_CP(0x00502448));          // "Whoo! Increase credits table size!"
    int32_t i = 0;
    if (UI_G32(S_CREDITS_N) > i) {
        const uint8_t* p = (const uint8_t*)(uintptr_t)S_CREDITS;
        do {
            i++;
            const int32_t h = ccall<int32_t>(F_item_height, (const void*)p, style);
            p += 8;
            UI_G32(S_CREDITS_H) += h;
        } while (i < UI_G32(S_CREDITS_N));
    }
}
// (a picture among the blocks makes item_height load its stamp)
static bool credits_have_picture(const uint32_t (*src)[2], int n, bool dwords) {
    for (int k = 0; k < n; k++) {
        const uint32_t items = dwords ? src[k][1] / 2u : src[k][1] / 8u;
        for (uint32_t j = 0; j < items; j++)
            if (*(const volatile int32_t*)(uintptr_t)(src[k][0] + j * 8u) == 4) return true;
    }
    return false;
}
static void fp_fillout_credits(Footprint& f, int32_t) {
    static const uint32_t blocks[7][2] = {{0x005010a8, 0x40}, {0x005010e8, 0x30}, {0x00501118, 0x38}, {0x00501150, 0x38},
                                          {0x005011b8, 0x30}, {0x005011e8, 0x38}, {0x00501188, 0x30}};
    static const uint32_t fixed[7][2] = {{0x00501830, 0x7e}, {0x00501798, 0x26}, {0x00501240, 0xfe}, {0x00501a58, 0x58},
                                         {0x00501a28, 0xc}, {0x00501708, 0x24}, {0x00501220, 8}};
    if (credits_have_picture(blocks, 7, false) || credits_have_picture(fixed, 7, true)) {
        f.replay_only = "a picture among the credits (item_height loads its stamp)";
        return;
    }
    UI_FP(f, S_CREDITS, 0x1008, "the credits' table, count and height");        // (the blocks fill 0xa40 bytes)
}
PORT_FN(0x004cc7b0, "fillout_credits", fillout_credits_n, fp_fillout_credits)

// n blocks copied in a random order (Random(100) % n, again while the block drawn is used)
static void __cdecl add_from_random_n(RandomCreditTableItem* items, int32_t n) {
    if (n <= 0) return;
    int32_t left = n;
    do {
        int32_t r;
        // FIX CANDIDATE: never ends when a block is already used, or when Random can't reach an unused one (n over 100);
        // and a table already near its 512 items is overrun (its count first). The only caller hands it six fresh blocks
        // after 8 items
        do {
            r = ccall<int32_t>(F_Random, (int32_t)100) % n;
        } while (*(const volatile uint8_t*)((const uint8_t*)items + (uint32_t)r * 12u + 8u) == 1);
        RandomCreditTableItem* it = (RandomCreditTableItem*)((uint8_t*)items + (uint32_t)r * 12u);
        it->used = 1;
        const uint32_t bytes = it->bytes;
        const uint32_t cnt = UI_GU32(S_CREDITS_N);
        crt_copy(CS_VP(cnt * 8u + S_CREDITS), (const void*)it->items, bytes);
        UI_G32(S_CREDITS_N) += (int32_t)(it->bytes >> 3);
    } while (--left != 0);
}
static void fp_add_from_random(Footprint& f, RandomCreditTableItem* items, int32_t n) {
    if (n <= 0) return;
    if (n > 0x1000) { f.replay_only = "too many blocks to bound"; return; }
    uint64_t total = 0;
    for (int32_t k = 0; k < n; k++) total += items[k].bytes;
    const uint32_t start = UI_GU32(S_CREDITS_N) * 8u + S_CREDITS;
    if (total > 0x100000) { f.replay_only = "blocks too big to bound"; return; }
    if ((uint64_t)start + total > S_CREDITS_N || start < S_CREDITS) {
        f.replay_only = "the copies reach past the table (its count among what they overwrite)";
        return;
    }
    f.add(items, (uint32_t)n * 12u, "the blocks' used flags");
    UI_FP(f, S_CREDITS_N, 4, "the credits' count");
    if (total) UI_FP(f, start, (uint32_t)total, "the credits' table");
}
PORT_FN(0x004cc990, "add_from_random", add_from_random_n, fp_add_from_random)

static int32_t __cdecl item_height_n(const CreditItem* it, int32_t style) {
    switch ((uint32_t)it->type) {
    case 0:
    case 3:
        return ccall<int32_t>(F_UIStyleHeight, style, (const char*)it->text) + 3;
    case 1:
        return 0xf;
    case 2:
        return 0x3c;
    case 4: {
        int32_t h = 0;
        if (void* s = ccall<void*>(F_gxGetStamp, (const char*)it->text)) {
            h = ccall<int32_t>(F_gxStampHeight, (const void*)s);
            ccall<void>(F_gxForgetStamp, s);
        }
        return h;
    }
    case 5:
        return 0;
    }
    return -1;
}
static void fp_item_height(Footprint& f, const CreditItem* it, int32_t) {
    if (it->type == 4) f.replay_only = "a picture: loads its stamp";
}
PORT_FN(0x004cca00, "item_height", item_height_n, fp_item_height)

// the credits t ms in: done (1) once they've all gone by and 6 s more; else 30 pixels a second after the first second,
// within 520 x 400 in the screen's middle
static uint8_t __cdecl draw_credits_n(gxCanvas* c, int32_t t, int32_t style) {
    const int32_t q = (int32_t)(UI_GU32(S_CREDITS_H) * 1000u) / 0x1e;
    if ((int32_t)((uint32_t)q + 0x1770u) < t) return 1;
    int32_t s = (int32_t)((uint32_t)t - 0x3e8u);
    if (s < 0) s = 0;
    else if (q < s) s = q;
    const int32_t top = (int32_t)((uint32_t)UI_G32(S_SCREEN_H) - 0x190u) / 2;
    const int32_t left = (int32_t)((uint32_t)UI_G32(S_SCREEN_W) - 0x208u) / 2;
    ccall<void>(F_gxSetClip, c, left, top, (int32_t)((uint32_t)left + 0x208u), (int32_t)((uint32_t)top + 0x190u));
    s = (int32_t)((uint32_t)s + (uint32_t)s);
    int32_t y = (int32_t)(0u - (uint32_t)s * 15u) / 1000;
    y = (int32_t)((uint32_t)y + 0xc8u);
    if (y < 0x190) {
        const uint8_t* p = (const uint8_t*)(uintptr_t)S_CREDITS;
        int32_t i = 0;
        while (i < UI_G32(S_CREDITS_N)) {
            const int32_t h = ccall<int32_t>(F_item_height, (const void*)p, style);
            if ((int32_t)(0u - (uint32_t)h) < y) {
                const int32_t yy = (int32_t)((uint32_t)top + (uint32_t)y);
                ccall<void>(F_draw_item, UI_G32(S_SCREEN_W) / 2, yy, (const void*)p, style);
            }
            y = (int32_t)((uint32_t)y + (uint32_t)h);
            p += 8;
            i++;
            if (y >= 0x190) break;
        }
    }
    ccall<void>(F_gxRestoreClip, c);
    return 0;
}
static void fp_draw_credits(Footprint& f, gxCanvas* c, int32_t, int32_t) {
    int32_t n = UI_G32(S_CREDITS_N);
    if (n > 0x200) n = 0x200;
    for (int32_t k = 0; k < n; k++)
        if (UI_G32(S_CREDITS + (uint32_t)k * 8u) == 4) { f.replay_only = "a picture among the credits (loads its stamp)"; return; }
    UI_FP_CANVAS(f, c);
    UI_FP_CUR_CANVAS(f);
}
PORT_FN(0x004cca90, "draw_credits", draw_credits_n, fp_draw_credits)

// an item at (x, y): a line centred (0) or left-aligned (3), a picture centred (4); nothing for the rest
static void __cdecl draw_item_n(int32_t x, int32_t y, const CreditItem* it, int32_t style) {
    switch ((uint32_t)it->type) {
    case 0:
        ccall<void>(F_UIStyleDraw, style, x, y, (const char*)it->text, (uint32_t)2);
        return;
    case 3:
        ccall<void>(F_UIStyleDraw, style, x, y, (const char*)it->text, (uint32_t)0);
        return;
    case 4:
        if (void* s = ccall<void*>(F_gxGetStamp, (const char*)it->text)) {
            const int32_t w = ccall<int32_t>(F_gxStampWidth, (const void*)s);
            ccall<void>(F_gxDrawStamp, (const void*)s, (int32_t)((uint32_t)x - (uint32_t)(w / 2)), y, (int32_t)0, (const void*)0);
            ccall<void>(F_gxForgetStamp, s);
        }
        return;
    }
}
static void fp_draw_item(Footprint& f, int32_t, int32_t, const CreditItem* it, int32_t) {
    if (it->type == 4) { f.replay_only = "a picture: loads its stamp"; return; }
    UI_FP_CUR_CANVAS(f);
}
PORT_FN(0x004ccbb0, "draw_item", draw_item_n, fp_draw_item)

struct MouseEvent { int32_t type, x, y, state; };
static uint8_t __cdecl mouse_hit_n() {
    MouseEvent ev;
    if (!ccall<uint8_t>(F_MouseGetEvent, &ev)) return 0;
    const int32_t t = *(volatile int32_t*)&ev.type;
    if (t == 0 || t == 2 || t == 4) return 1;                   // a button went down
    return 0;
}
static void fp_mouse_hit(Footprint& f) { f.replay_only = "takes an event from the mouse queue"; }
PORT_FN(0x004ccc70, "mouse_hit(credits.obj)", mouse_hit_n, fp_mouse_hit)

// =========================================================================================================================
// intro.obj
// =========================================================================================================================
static void __cdecl IntroPlayVideo_n(const char*) {}
static void fp_IntroPlayVideo(Footprint& f, const char*) { f.pure = true; }
PORT_FN(0x004cccd0, "IntroPlayVideo", IntroPlayVideo_n, fp_IntroPlayVideo)

}  // namespace career_credits
}  // namespace
