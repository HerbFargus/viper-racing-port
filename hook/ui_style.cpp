// ui_style.cpp -- M3 UI stage, step U1: uistyle.obj, the UI's text styles, rewritten faithfully (library `ui`).
//
//   the style table (UIStyle[32] at 0x5790e0: a font, four gradient palettes by state, alignment flags, text offsets
//   normal / pressed, a stamp): UIStyleBegin / End, UIAddStyle / UIAddDynamicStyle / UIRemoveStyle, get_style; what a
//   style's text measures (UIStyleWidth / Height / GetBounds, IndexToPoint / PointToIndex: the caret) and draws
//   (UIStyleDraw: the stamp frame, then each line), and UIStyleWordWrap (8 pixels a character, whatever the style)
//
// Written from the v1.0 disassembly (tools/disasm.py): every call in the original's order by its v1.0 address (this
// file's own too, so a hooked rewrite is what runs), the compiler's inline strlen / strcpy as it runs them
// (ui_types.h), integer division as the original's signed `cdq; sub; sar` (C's /2) and `div` (unsigned).
//
// Footprints (the UI runs on the main thread: every static written is listed): the table functions allocate or free
// (fonts, palettes, stamps): replay_only. UIStyleDraw draws on the current canvas (its pixels and clip); the measures
// write only their outputs; get_style / get_width / charcount are pure.
//
// FIX CANDIDATES (left as the original has them):
//   * UIAddStyle / UIRemoveStyle / get_style / UIStyleDraw take the style index unchecked: outside 0..31 they read or
//     write past the 32-entry table (0x5790e0).
//   * UIStyleDraw indexes the four palettes with its `state` argument unchecked (a state past 3 reads the next
//     style's fields as a palette), and divides the state by the stamp's frame count: a stamp with no frames divides
//     by zero.
//   * UIStyleWordWrap copies its input into `out` with no length: the caller's buffer must hold it (the long dialog
//     boxes' 4 KB).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"

namespace {
namespace ui_style {
using namespace uit;

// ---- the table -------------------------------------------------------------------------------------------------------
// UIStyleBegin: the spare style zeroed (flags 9), copied to style 0, then 0x174 dwords from style 0 on to style 1: the
// forward copy walks into what it just wrote, so every style becomes the spare
static void __cdecl UIStyleBegin_n() {
    volatile uint32_t* z = (volatile uint32_t*)(uintptr_t)S_STYLE_ZERO;
    for (int i = 0; i < 12; i++) z[i] = 0;
    UI_GU32(S_STYLE_ZERO + 0x18) = 9;
    volatile uint32_t* d = (volatile uint32_t*)(uintptr_t)S_STYLES;
    for (int i = 0; i < 12; i++) d[i] = z[i];
    const volatile uint32_t* s = (const volatile uint32_t*)(uintptr_t)S_STYLES;
    volatile uint32_t* t = (volatile uint32_t*)(uintptr_t)(S_STYLES + 0x30);
    for (int i = 0; i < 0x174; i++) t[i] = s[i];
}
static void fp_UIStyleBegin(Footprint& f) { UI_FP(f, S_STYLE_ZERO, S_STYLES_END - S_STYLE_ZERO, "uistyle.obj styles"); }
PORT_FN(0x0047f330, "UIStyleBegin", UIStyleBegin_n, fp_UIStyleBegin)

// UIStyleEnd: a panic for each style still in use
static void __cdecl UIStyleEnd_n() {
    int32_t i = 0;
    for (uint32_t a = S_STYLES; a < S_STYLES_END; a += 0x30, i++)
        if (UI_G8(a) != 0) UI_LogPanic((const char*)0x004f71cc, i);
}
static void fp_UIStyleEnd(Footprint&) {}
PORT_FN(0x0047f370, "UIStyleEnd", UIStyleEnd_n, fp_UIStyleEnd)

// UIAddStyle: the style made from a description -- its font, four gradients (normal: c_normal..c_normal2, pressed:
// c_down..c_down2, over: c_over..c_normal2, pressed and over: c_down_over..c_down2), flags, offsets, stamp
static void __cdecl UIAddStyle_n(int32_t i, const UIStyleDesc* d) {
    UIStyle* s = ui_style_at(i);
    const volatile uint32_t* dv = (const volatile uint32_t*)d;
    s->used = 1;
    s->font = ccall<void*>(F_gxFontGet, (const char*)(uintptr_t)dv[0]);
    s->pal[0] = ccall<void*>(F_gxPaletteCreate);
    {
        const uint32_t c2 = dv[4], c1 = dv[2];
        ccall<void>(F_gxPaletteMakeGradient, (void*)s->pal[0], c1, c2);
    }
    void* p = ccall<void*>(F_gxPaletteCreate);
    s->pal[1] = p;
    {
        const uint32_t c2 = dv[9], c1 = dv[7];
        ccall<void>(F_gxPaletteMakeGradient, p, c1, c2);
    }
    p = ccall<void*>(F_gxPaletteCreate);
    s->pal[2] = p;
    {
        const uint32_t c2 = dv[4], c1 = dv[3];
        ccall<void>(F_gxPaletteMakeGradient, p, c1, c2);
    }
    p = ccall<void*>(F_gxPaletteCreate);
    s->pal[3] = p;
    {
        const uint32_t c2 = dv[9], c1 = dv[8];
        ccall<void>(F_gxPaletteMakeGradient, p, c1, c2);
    }
    s->flags = dv[1];
    s->xo = (int32_t)dv[5];
    s->yo = (int32_t)dv[6];
    s->xo_down = (int32_t)dv[10];
    s->yo_down = (int32_t)dv[11];
    const char* st = (const char*)(uintptr_t)dv[12];
    if (st) s->stamp = ccall<void*>(F_gxGetStamp, st);
    else s->stamp = 0;
}
static void fp_UIAddStyle(Footprint& f, int32_t, const UIStyleDesc*) { f.replay_only = "loads a font and a stamp, allocates palettes"; }
PORT_FN(0x0047f3a0, "UIAddStyle", UIAddStyle_n, fp_UIAddStyle)

// UIAddDynamicStyle: the first free style (a panic and -1 if there's none)
static int32_t __cdecl UIAddDynamicStyle_n(const UIStyleDesc* d) {
    int32_t i = 0;
    for (uint32_t a = S_STYLES; a < S_STYLES_END; a += 0x30, i++)
        if (UI_G8(a) == 0) {
            ccall<void>(F_UIAddStyle, i, d);
            return i;
        }
    UI_LogPanic((const char*)0x004f71e4);
    return -1;
}
static void fp_UIAddDynamicStyle(Footprint& f, const UIStyleDesc*) { f.replay_only = "loads a font and a stamp, allocates palettes"; }
PORT_FN(0x0047f4a0, "UIAddDynamicStyle", UIAddDynamicStyle_n, fp_UIAddDynamicStyle)

// UIRemoveStyle: its font, palettes and stamp released, the spare copied over it, then marked unused
static void __cdecl UIRemoveStyle_n(int32_t i) {
    UIStyle* s = ui_style_at(i);
    ((Assert_t)(uintptr_t)F_UIStyleAssert)((int)s->used, (const char*)0x004f7200, i);
    ccall<void>(F_gxFontForget, (void*)s->font);
    for (int k = 0; k < 4; k++) ccall<void>(F_gxPaletteDestroy, (void*)s->pal[k]);
    void* st = s->stamp;
    if (st) ccall<void>(F_gxForgetStamp, st);
    const volatile uint32_t* z = (const volatile uint32_t*)(uintptr_t)S_STYLE_ZERO;
    volatile uint32_t* d = (volatile uint32_t*)s;
    for (int k = 0; k < 12; k++) d[k] = z[k];
    s->used = 0;
}
static void fp_UIRemoveStyle(Footprint& f, int32_t) { f.replay_only = "frees a font, palettes and a stamp"; }
PORT_FN(0x0047f4e0, "UIRemoveStyle", UIRemoveStyle_n, fp_UIRemoveStyle)

// ---- measures -----------------------------------------------------------------------------------------------------------
// UIStyleGetBounds: the rectangle text drawn at (x, y) covers -- its stamp (by the hot spot), then each line's width
// and the lines' height, placed by the flags, at both the normal and the pressed offsets
static void __cdecl UIStyleGetBounds_n(int32_t style, const char* text, int32_t x, int32_t y, int32_t* px0, int32_t* py0,
                                       int32_t* px1, int32_t* py1) {
    UIStyle* s = ccall<UIStyle*>(F_get_style, style);
    int32_t hx, hy;                                     // the hot spot; then the text's height, the line count
    int32_t bx0 = x, by0 = y, bx1 = x, by1 = y;
    void* st = s->stamp;
    if (st) {
        ccall<void>(F_gxStampHotSpot, st, &hx, &hy);
        int32_t w = ccall<int32_t>(F_gxStampWidth, (void*)s->stamp);
        const int32_t h = ccall<int32_t>(F_gxStampHeight, (void*)s->stamp);
        int32_t a = x - hx;
        if (!(a < x)) a = x;
        bx0 = a;
        w = w - hx + x;
        if (!(w > x)) w = x;
        bx1 = w;
        a = y - hy;
        int32_t b = a;
        if (!(a < y)) b = y;
        by0 = b;
        int32_t c = h + a;
        if (!(c > y)) c = y;
        by1 = c;
    }
    const char* p = text;
    if (p) {
        const int32_t lines = ccall<int32_t>(F_charcount, p, (char)'\n') + 1;
        int32_t wmax = 0;
        hx = (int32_t)((uint32_t)ccall<int32_t>(F_gxFontHeight, (void*)s->font) * (uint32_t)lines);
        if (lines > 0) {
            hy = lines;
            do {
                const char* nl = ccall<const char*>(F_strchr, p, (int)'\n');
                const int32_t len = nl ? (int32_t)(nl - p) : (int32_t)crt_strlen(p);
                int32_t w = ccall<int32_t>(F_gxFontStringWidthN, (void*)s->font, p, len);
                if (!(w > wmax)) w = wmax;
                wmax = w;
                p = ccall<const char*>(F_strchr, p, (int)'\n');
                if (p) p++;
            } while (--hy != 0);
        }
        int32_t ox = 0, oy = 0;
        const uint8_t fl = (uint8_t)s->flags;
        if (fl & 2) ox = -wmax;
        else if (fl & 4) ox = -(wmax / 2);
        if (fl & 0x40) oy = -hx;
        else if (fl & 0x10) oy = -(hx / 2);
        else if (fl & 0x20) oy = -ccall<int32_t>(F_gxFontAscent, (void*)s->font);
        wmax += (ccall<int32_t>(F_gxFontStringWidth, (void*)s->font, (const char*)0x004f7218) + 1) / 2;
        int32_t xd = s->xo_down;
        const int32_t xn = s->xo;
        int32_t c = xd < xn ? xd : xn;
        c = c + ox + x;
        if (!(c < bx0)) c = bx0;
        bx0 = c;
        c = s->xo_down;
        if (!(c > xn)) c = xn;
        c = c + ox + wmax + x;
        if (!(c > bx1)) c = bx1;
        bx1 = c;
        const int32_t yd = s->yo_down, yn = s->yo;
        c = yd < yn ? yd : yn;
        c = c + oy + y;
        if (!(c < by0)) c = by0;
        by0 = c;
        c = yd > yn ? yd : yn;
        c = c + oy + hx + y;
        if (!(c > by1)) c = by1;
        by1 = c;
    }
    *(volatile int32_t*)px0 = bx0;
    *(volatile int32_t*)px1 = bx1;
    *(volatile int32_t*)py0 = by0;
    *(volatile int32_t*)py1 = by1;
}
static void fp_UIStyleGetBounds(Footprint& f, int32_t, const char*, int32_t, int32_t, int32_t* px0, int32_t* py0, int32_t* px1,
                                int32_t* py1) {
    f.add(px0, 4, "x0"); f.add(py0, 4, "y0"); f.add(px1, 4, "x1"); f.add(py1, 4, "y1");
}
PORT_FN(0x0047f570, "UIStyleGetBounds", UIStyleGetBounds_n, fp_UIStyleGetBounds)

// get_style: the table entry (index unchecked)
static UIStyle* __cdecl get_style_n(int32_t i) { return ui_style_at(i); }
static void fp_get_style(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x0047f7d0, "get_style", get_style_n, fp_get_style)

// charcount: how many c in s (none when c is 0)
static int32_t __cdecl charcount_n(const char* s, char c) {
    const volatile char* p = s;
    int32_t n = 0;
    if (*p == 0) return 0;
    do {
        if (*p == c) n++;
        p++;
    } while (*p != 0);
    return n;
}
static void fp_charcount(Footprint&, const char*, char) {}
PORT_FN(0x0047f7e0, "charcount", charcount_n, fp_charcount)

// UIStylePointToIndex: the caret position (0..strlen) whose point is nearest (x, y), by |dx| + |dy|; the first of
// equals
static int32_t __cdecl UIStylePointToIndex_n(int32_t style, const char* text, int32_t x, int32_t y) {
    int32_t best = 0, bestd = -1;
    int32_t px, py;
    for (int32_t i = 0;; i++) {
        if (i != 0 && ((const volatile char*)text)[i - 1] == 0) break;
        ccall<void>(F_UIStyleIndexToPoint, style, text, i, &px, &py);
        const int32_t dy = iabs((int32_t)((uint32_t)py - (uint32_t)y));
        const int32_t d = (int32_t)((uint32_t)iabs((int32_t)((uint32_t)px - (uint32_t)x)) + (uint32_t)dy);
        if (bestd < 0 || bestd > d) {
            bestd = d;
            best = i;
        }
    }
    return best;
}
static void fp_UIStylePointToIndex(Footprint&, int32_t, const char*, int32_t, int32_t) {}
PORT_FN(0x0047f800, "UIStylePointToIndex", UIStylePointToIndex_n, fp_UIStylePointToIndex)

// UIStyleIndexToPoint: where the caret before character n goes (each character measured alone; a newline goes down
// a line, back to the style's x offset)
static void __cdecl UIStyleIndexToPoint_n(int32_t style, const char* text, int32_t n, int32_t* px, int32_t* py) {
    UIStyle* s = ccall<UIStyle*>(F_get_style, style);
    char one[4];
    volatile char* vone = one;
    vone[2] = 0;
    vone[3] = 0;
    int32_t x = s->xo;
    int32_t y = s->yo;
    const volatile char* t = text;
    if (t[0] != 0) {
        int32_t i = 0;
        do {
            if (!(n > i)) break;
            const char ch = t[i];
            if (ch != '\n') {
                vone[2] = ch;
                x += ccall<int32_t>(F_gxFontStringWidth, (void*)s->font, (const char*)&one[2]);
            } else {
                y += ccall<int32_t>(F_gxFontHeight, (void*)s->font);
                x = s->xo;
            }
            i++;
        } while (t[i] != 0);
    }
    *(volatile int32_t*)px = x;
    *(volatile int32_t*)py = y;
}
static void fp_UIStyleIndexToPoint(Footprint& f, int32_t, const char*, int32_t, int32_t* px, int32_t* py) {
    f.add(px, 4, "x"); f.add(py, 4, "y");
}
PORT_FN(0x0047f880, "UIStyleIndexToPoint", UIStyleIndexToPoint_n, fp_UIStyleIndexToPoint)

// UIStyleWidth: the text's width placed by the flags, from the larger x offset; at least the stamp's width
static int32_t __cdecl UIStyleWidth_n(int32_t style, const char* text) {
    UIStyle* s = ccall<UIStyle*>(F_get_style, style);
    int32_t left = 0;
    if (!text) return ccall<int32_t>(F_gxStampWidth, (void*)s->stamp);
    int32_t right = 0;
    const int32_t w = ccall<int32_t>(F_gxFontStringWidth, (void*)s->font, text);
    const uint8_t fl = (uint8_t)s->flags;
    if (fl & 2) left = -w;
    else if (fl & 4) left = -(w / 2);
    if (fl & 1) right = w;
    else if (fl & 4) right = w / 2;
    const int32_t sw = ccall<int32_t>(F_gxStampWidth, (void*)s->stamp);
    int32_t a = s->xo_down;
    const int32_t b = s->xo;
    if (!(a > b)) a = b;
    a = a - left + right;
    return a > sw ? a : sw;
}
static void fp_UIStyleWidth(Footprint&, int32_t, const char*) {}
PORT_FN(0x0047f910, "UIStyleWidth", UIStyleWidth_n, fp_UIStyleWidth)

// UIStyleHeight: the larger y offset plus the part of a line below it (the flags); at least the stamp's height
static int32_t __cdecl UIStyleHeight_n(int32_t style, const char* text) {
    UIStyle* s = ccall<UIStyle*>(F_get_style, style);
    if (!text) return ccall<int32_t>(F_gxStampHeight, (void*)s->stamp);
    const uint8_t fl = (uint8_t)s->flags;
    int32_t below;
    if (fl & 8) below = ccall<int32_t>(F_gxFontHeight, (void*)s->font);
    else if (fl & 0x10) below = ccall<int32_t>(F_gxFontHeight, (void*)s->font) / 2;
    else {
        below = 0;
        if (fl & 0x20) below = ccall<int32_t>(F_gxFontDescent, (void*)s->font);
    }
    const int32_t sh = ccall<int32_t>(F_gxStampHeight, (void*)s->stamp);
    int32_t a = s->yo_down;
    const int32_t b = s->yo;
    if (!(a > b)) a = b;
    a += below;
    return a > sh ? a : sh;
}
static void fp_UIStyleHeight(Footprint&, int32_t, const char*) {}
PORT_FN(0x0047f9b0, "UIStyleHeight", UIStyleHeight_n, fp_UIStyleHeight)

// ---- drawing ------------------------------------------------------------------------------------------------------------
// UIStyleDraw: the stamp's frame (state mod its frame count), then each line in the state's palette, at the pressed
// offsets for an odd state; vertically centred (0x10) or bottom-aligned (0x40) as a block
static void __cdecl UIStyleDraw_n(int32_t style, int32_t x, int32_t y, const char* text, uint32_t state) {
    UIStyle* s = ccall<UIStyle*>(F_get_style, style);
    void* st = s->stamp;
    const uint8_t down = (uint8_t)(state & 1);
    void* pal = ((void* volatile*)((uint8_t*)s + 8))[state];
    if (st) {
        const uint32_t n = (uint32_t)ccall<int32_t>(F_gxStampCount, st);
        ccall<void>(F_gxDrawStamp, st, x, y, (int32_t)(state % n), (void*)0);
    }
    const char* p = text;
    if (!p) return;
    const int32_t lh = ccall<int32_t>(F_gxFontHeight, (void*)s->font);
    const int32_t lines = ccall<int32_t>(F_charcount, p, (char)'\n') + 1;
    const uint8_t fl = (uint8_t)s->flags;
    if (fl & 0x10) y += (int32_t)((uint32_t)(1 - lines) * (uint32_t)lh) / 2;
    else if (fl & 0x40) y += (int32_t)((uint32_t)(1 - lines) * (uint32_t)lh);
    if (lines <= 0) return;
    int32_t left = lines;
    do {
        const char* nl = ccall<const char*>(F_strchr, p, (int)'\n');
        const int32_t len = nl ? (int32_t)(nl - p) : (int32_t)crt_strlen(p);
        const int32_t yo = down ? s->yo_down : s->yo;
        const int32_t xo = down ? s->xo_down : s->xo;
        ccall<void>(F_gxFontPrintN, (void*)s->font, pal, (uint32_t)s->flags, x + xo, y + yo, p, len);
        y += lh;
        p = ccall<const char*>(F_strchr, p, (int)'\n');
        if (p) p++;
    } while (--left != 0);
}
static void fp_UIStyleDraw(Footprint& f, int32_t, int32_t, int32_t, const char*, uint32_t) { UI_FP_CUR_CANVAS(f); }
PORT_FN(0x0047fa40, "UIStyleDraw", UIStyleDraw_n, fp_UIStyleDraw)

// UIStyleWordWrap: in copied to out, then each line broken at its last space (or newline) before it grows past
// `width` (get_width: 8 pixels a character); a space it breaks at becomes a newline
static void __cdecl UIStyleWordWrap_n(int32_t style, int32_t width, char* out, const char* in) {
    crt_strcpy(out, in);
    volatile char* line = out;
    if (*line == 0) return;
    for (;;) {
        volatile char* brk = line;
        volatile char* e = line + 1;
        while (e[-1] != 0) {
            if (ccall<int32_t>(F_get_width, style, (const char*)line, (const char*)e) > width) break;
            const char c = *e;
            if (c == ' ' || c == 0) brk = e;
            if (c == '\n') {
                brk = e;
                break;
            }
            e++;
        }
        if (*brk == ' ') *brk = '\n';
        if (*brk == 0) return;
        line = brk + 1;
        if (*line == 0) return;
    }
}
static void fp_UIStyleWordWrap(Footprint& f, int32_t, int32_t, char* out, const char* in) {
    f.add(out, (uint32_t)strlen(in) + 1, "the wrapped text");
}
PORT_FN(0x0047fb90, "UIStyleWordWrap", UIStyleWordWrap_n, fp_UIStyleWordWrap)

// get_width: 8 pixels a character, whatever the style
static int32_t __cdecl get_width_n(int32_t, const char* a, const char* b) { return (int32_t)(((uint32_t)b - (uint32_t)a) * 8u); }
static void fp_get_width(Footprint& f, int32_t, const char*, const char*) { f.pure = true; }
PORT_FN(0x0047fc20, "get_width", get_width_n, fp_get_width)

}  // namespace ui_style
}  // namespace
