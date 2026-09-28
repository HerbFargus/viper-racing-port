// ui_widget.cpp -- M3 UI stage, step U1: the widget classes, rewritten faithfully (library `ui`: _widget.obj, and the
// widget virtuals of ui.obj).
//
//   Widget (the base: rectangle, over / focus / visible / enabled flags, dirty, value notifications -- a copy of each
//   watched value, compared every Update, Callback(id, ptr) when it changed), StyleWidget (text in a UIStyle: Draw,
//   UpdateText fitting the rectangle to the text's bounds), Button / MenuButton, BButton (a stamp), CheckBox,
//   StampRoll, RadioButton (optionally showing its group while selected), BRadioButton, ScrollButtonBase (auto-repeat
//   while held: 0.3 s, then faster) with ArrowButton (steps a float) and ScrollButton (steps a scroll axis),
//   StaticText, LineWidget, FStaticText, Numeric / IntNumeric (sprintf a watched value), Multi (cycles strings),
//   ListBox / DropList, Input (a text field: caret, insert, delete, home / end / arrows), CDetect (a control
//   binding, detected in a modal dialog), Slider, Decal, ScrollBar, UICustomControl's helpers; and ui.obj's: the
//   Widget defaults, HotKey, Button's handlers, CustomWidget (everything forwarded to a UICustomControl), TitleBar
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this library's own too),
// virtual calls through the object's vtable (a tail jump `jmp [eax+n]` as a call), the compiler's inline string code
// as it runs it (ui_types.h). x87: a register value is a double, a stored one a float, the original's grouping, its
// constants' widths (0.3 as the bits it copies, 0.005f, 0.5f, 1.0f, 2.0f), __ftol as x87_ftol, comparisons with the
// original's NaN outcome (docs/PORTING.md). Division as the original's: `idiv` (C's /), `cdq; sub; sar` (C's /2).
//
// Footprints (main thread): a widget's handlers write the widget and what ui_types.h's ui_fp_reach lists (the values
// it edits, its notification copies, WidgetExit's statics, the running window for the tab keys, radio groups and the
// title bar); draws add the canvas (its clip and pixels) and the current-canvas static. replay_only: what allocates,
// frees or loads (the constructors that load a stamp or add a notification, the destructors, AddNotification /
// RemoveNotification), what calls unbounded code (a button's callback, CustomWidget -> UICustomControl, CDetect's
// dialog and its idle function), per call where it depends on the object (a button without a callback is checked).
//
// FIX CANDIDATES (left as the original has them):
//   * Widget::AddNotification has no bound on its 32 entries: the 33rd writes over the count (+0x21c) and the dirty
//     flag, and past the object.
//   * Multi's constructor has no bound on its 8 strings: a ninth is written past the object.
//   * Numeric / IntNumeric sprintf into an 80-byte buffer; DropList::Draw sprintf's "%-*s `" with the list's width in
//     characters into an 80-byte stack buffer (a list wider than ~600 pixels overruns it).
//   * Input::CharHit: a buffer of width * lines == 0 takes characters without end (max - 1 wraps unsigned); a cursor
//     below 0 (never set by the widget) writes before the buffer.
//   * Input::UpdateTable (never called) has no bound on its 512 line starts.
//   * ArrowButton::Do divides by (steps - 1) and by the step: 1 step, or lo == hi, gives inf / NaN (the value becomes
//     0 or NaN). Slider::MouseMove and Draw divide by steps (idiv: 0 steps faults) and steps - 1; ListBox divides by
//     its row height; ScrollBar by the axis's total (0: inf, ftol -> 0).
//   * FStaticText::Draw (no vtable: never called) passes its text as a printf format with no arguments.
//   * BRadioButton::MouseDown writes *var unchecked (its constructor already needs var).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"

namespace {
namespace ui_widget {
using namespace uit;

#define D(x) ((double)(x))
static __forceinline float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static __forceinline void copy_f(volatile float* d, const volatile float* s) {            // mov eax, [s]; mov [d], eax
    *(volatile uint32_t*)d = *(const volatile uint32_t*)s;
}
static __forceinline void dirty_if_visible(Widget* w) { if (w->visible != 0) w->dirty = 1; }
// a button's press: the callback (if any) says whether it exits; WidgetExit(id)
static __forceinline void fire(UICallback cb, int32_t id, const volatile int32_t* idp) {
    if (cb) {
        if (!cb(id)) return;
    }
    ccall<void>(F_WidgetExit, (int32_t)*idp);
}

// ---- footprints (templates: each takes the rewrite's own `this` type) ------------------------------------------------------
template <typename T> static void fp_pure0(Footprint& f, T*, Edx) { f.pure = true; }
template <typename T> static void fp_none0(Footprint&, T*, Edx) {}
template <typename T> static void fp_none_key(Footprint&, T*, Edx, uint16_t) {}
template <typename T> static void fp_self0(Footprint& f, T* w, Edx) { UI_FP_WIDGET(f, w, "this"); }
template <typename T> static void fp_self_xy(Footprint& f, T* w, Edx, int32_t, int32_t) { UI_FP_WIDGET(f, w, "this"); }
template <typename T> static void fp_reach0(Footprint& f, T* w, Edx) { ui_fp_reach(f, w); }
template <typename T> static void fp_reach_xy(Footprint& f, T* w, Edx, int32_t, int32_t) { ui_fp_reach(f, w); }
template <typename T> static void fp_reach_key(Footprint& f, T* w, Edx, uint16_t) { ui_fp_reach(f, w); }
template <typename T> static void fp_reach_cb(Footprint& f, T* w, Edx, int32_t, const void*) { ui_fp_reach(f, w); }
template <typename T> static void fp_self_cb(Footprint& f, T* w, Edx, int32_t, const void*) { UI_FP_WIDGET(f, w, "this"); }
template <typename T> static void fp_draw(Footprint& f, T* w, Edx, gxCanvas* c) {
    UI_FP_WIDGET(f, w, "this");
    UI_FP_CANVAS(f, c);
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
}
template <typename T> static void fp_custom0(Footprint& f, T*, Edx) { f.replay_only = "forwards to a UICustomControl (menu code)"; }
template <typename T> static void fp_custom_xy(Footprint& f, T*, Edx, int32_t, int32_t) { f.replay_only = "forwards to a UICustomControl (menu code)"; }
template <typename T> static void fp_custom_cb(Footprint& f, T*, Edx, int32_t, const void*) { f.replay_only = "forwards to a UICustomControl (menu code)"; }
template <typename T> static void fp_custom_key(Footprint& f, T*, Edx, uint16_t) { f.replay_only = "forwards to a UICustomControl (menu code)"; }
template <typename T> static void fp_custom_draw(Footprint& f, T*, Edx, gxCanvas*) { f.replay_only = "forwards to a UICustomControl (menu code)"; }
template <typename T> static void fp_frees0(Footprint& f, T*, Edx) { f.replay_only = "frees (notification copies, a stamp)"; }
template <typename T> static void fp_dtor_flags(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the widget"; }

// =====================================================================================================================
// ui.obj: the defaults, HotKey, Button's handlers, CustomWidget, TitleBar
// =====================================================================================================================
static int32_t __fastcall Widget_GetID_n(Widget*, Edx) { return -1; }
PORT_FN(0x0047b4a0, "Widget::GetID", Widget_GetID_n, fp_pure0)

static void __fastcall Widget_Focus_n(Widget* self, Edx) {
    const uint8_t vis = self->visible;
    self->focus = 1;
    if (vis != 0) self->dirty = 1;
}
PORT_FN(0x0047b4c0, "Widget::Focus", Widget_Focus_n, fp_self0)

static void __fastcall Widget_UnFocus_n(Widget* self, Edx) {
    self->focus = 0;
    dirty_if_visible(self);
}
PORT_FN(0x0047b4e0, "Widget::UnFocus", Widget_UnFocus_n, fp_self0)

static void __fastcall Widget_Over_n(Widget* self, Edx) { self->over = 1; }
PORT_FN(0x0047b550, "Widget::Over", Widget_Over_n, fp_self0)
static void __fastcall Widget_NotOver_n(Widget* self, Edx) { self->over = 0; }
PORT_FN(0x0047b560, "Widget::NotOver", Widget_NotOver_n, fp_self0)

static void* __fastcall Widget_GetCursor_n(Widget*, Edx) { return 0; }
PORT_FN(0x0047b570, "Widget::GetCursor", Widget_GetCursor_n, fp_none0)

static uint8_t __fastcall Widget_HitTest_n(Widget* self, Edx, int32_t x, int32_t y) {
    if (self->x0 > x) return 0;
    if (self->x1 <= x) return 0;
    if (self->y0 > y) return 0;
    if (self->y1 <= y) return 0;
    return 1;
}
static void fp_hittest(Footprint& f, Widget*, Edx, int32_t, int32_t) { f.pure = true; }
PORT_FN(0x0047b590, "Widget::HitTest", Widget_HitTest_n, fp_hittest)

static void __fastcall Widget_Callback_n(Widget* self, Edx, int32_t, const void*) { dirty_if_visible(self); }
PORT_FN(0x0047b5c0, "Widget::Callback", Widget_Callback_n, fp_self_cb)

// HotKey::CharHit: its key fires it
static uint8_t __fastcall HotKey_CharHit_n(HotKey* self, Edx, uint16_t key) {
    if (self->key != key) return 0;
    fire(self->cb, self->id, &self->id);
    return 1;
}
static void fp_HotKey_CharHit(Footprint& f, HotKey* self, Edx, uint16_t) { ui_fp_reach(f, self); }
PORT_FN(0x0047b5d0, "HotKey::CharHit", HotKey_CharHit_n, fp_HotKey_CharHit)

static void __fastcall Button_MouseDown_n(Button* self, Edx, int32_t, int32_t) {
    const uint8_t vis = self->visible;
    self->pressed = 1;
    if (vis != 0) self->dirty = 1;
}
PORT_FN(0x0047b640, "Button::MouseDown", Button_MouseDown_n, fp_self_xy)

// Button::MouseUp: released over it fires it (not a label: id -66666)
static void __fastcall Button_MouseUp_n(Button* self, Edx, int32_t, int32_t) {
    dirty_if_visible(self);
    self->pressed = 0;
    if (self->over == 0) return;
    const int32_t id = self->id;
    if (id == (int32_t)0xfffefb96) return;
    UICallback cb = self->cb;
    if (cb) {
        if (!cb(id)) return;
    }
    ccall<void>(F_WidgetExit, (int32_t)self->id);
}
PORT_FN(0x0047b660, "Button::MouseUp", Button_MouseUp_n, fp_reach_xy)

static void __fastcall Button_Over_n(Button* self, Edx) {
    dirty_if_visible(self);
    self->over = 1;
}
PORT_FN(0x0047b6c0, "Button::Over", Button_Over_n, fp_self0)
static void __fastcall Button_NotOver_n(Button* self, Edx) {
    dirty_if_visible(self);
    self->over = 0;
}
PORT_FN(0x0047b6e0, "Button::NotOver", Button_NotOver_n, fp_self0)

// Button::Update: an auto-repeat button fires every frame while held over
static void __fastcall Button_Update_n(Button* self, Edx) {
    if (self->repeat == 0 || self->pressed == 0 || self->over == 0) return;
    fire(self->cb, self->id, &self->id);
}
PORT_FN(0x0047b710, "Button::Update", Button_Update_n, fp_reach0)

static const char* __fastcall Button_GetText_n(Button* self, Edx) { return self->text; }
PORT_FN(0x0047b760, "Button::GetText", Button_GetText_n, fp_none0)

// Button::GetState: 1 for a label; over: 2, pressed too: 3; else 0
static uint32_t __fastcall Button_GetState_n(Button* self, Edx) {
    if (self->id == (int32_t)0xfffefb96) return 1;
    if (self->over == 0) return 0;
    return self->pressed != 0 ? 3u : 2u;
}
static void fp_Button_GetState(Footprint& f, Button*, Edx) { f.pure = true; }
PORT_FN(0x0047b770, "Button::GetState", Button_GetState_n, fp_Button_GetState)

// CustomWidget: forwarded to its UICustomControl
#define CTRL(self) (((CustomWidget*)(self))->ctrl)
static void __fastcall CustomWidget_Activate_n(CustomWidget* self, Edx) { vcall<void>(CTRL(self), 4); }
PORT_FN(0x0047b7c0, "CustomWidget::Activate", CustomWidget_Activate_n, fp_custom0)
static void __fastcall CustomWidget_Deactivate_n(CustomWidget* self, Edx) { vcall<void>(CTRL(self), 8); }
PORT_FN(0x0047b7d0, "CustomWidget::Deactivate", CustomWidget_Deactivate_n, fp_custom0)
static void __fastcall CustomWidget_Added_n(CustomWidget* self, Edx) { vcall<void>(CTRL(self), 0xc); }
PORT_FN(0x0047b7e0, "CustomWidget::Added", CustomWidget_Added_n, fp_custom0)
static void __fastcall CustomWidget_Update_n(CustomWidget* self, Edx) {
    tcall<void>(F_Widget_Update, self);
    vcall<void>(CTRL(self), 0x14);
}
PORT_FN(0x0047b7f0, "CustomWidget::Update", CustomWidget_Update_n, fp_custom0)
static void __fastcall CustomWidget_MouseDown_n(CustomWidget* self, Edx, int32_t x, int32_t y) { vcall<void>(CTRL(self), 0x20, x, y); }
PORT_FN(0x0047b810, "CustomWidget::MouseDown", CustomWidget_MouseDown_n, fp_custom_xy)
static void __fastcall CustomWidget_MouseUp_n(CustomWidget* self, Edx, int32_t x, int32_t y) { vcall<void>(CTRL(self), 0x24, x, y); }
PORT_FN(0x0047b830, "CustomWidget::MouseUp", CustomWidget_MouseUp_n, fp_custom_xy)
static void __fastcall CustomWidget_MouseRDown_n(CustomWidget* self, Edx, int32_t x, int32_t y) { vcall<void>(CTRL(self), 0x2c, x, y); }
PORT_FN(0x0047b850, "CustomWidget::MouseRDown", CustomWidget_MouseRDown_n, fp_custom_xy)
static void __fastcall CustomWidget_MouseRUp_n(CustomWidget* self, Edx, int32_t x, int32_t y) { vcall<void>(CTRL(self), 0x30, x, y); }
PORT_FN(0x0047b870, "CustomWidget::MouseRUp", CustomWidget_MouseRUp_n, fp_custom_xy)
static void* __fastcall CustomWidget_GetCursor_n(CustomWidget* self, Edx) { return vcall<void*>(CTRL(self), 0x44); }
PORT_FN(0x0047b890, "CustomWidget::GetCursor", CustomWidget_GetCursor_n, fp_custom0)
static void __fastcall CustomWidget_Callback_n(CustomWidget* self, Edx, int32_t id, const void* p) { vcall<void>(CTRL(self), 0x10, id, p); }
PORT_FN(0x0047b8a0, "CustomWidget::Callback", CustomWidget_Callback_n, fp_custom_cb)
static void __fastcall CustomWidget_MouseMove_n(CustomWidget* self, Edx, int32_t x, int32_t y) { vcall<void>(CTRL(self), 0x28, x, y); }
PORT_FN(0x0047b8c0, "CustomWidget::MouseMove", CustomWidget_MouseMove_n, fp_custom_xy)
static void __fastcall CustomWidget_Over_n(CustomWidget* self, Edx) {
    self->over = 1;
    vcall<void>(CTRL(self), 0x34);
}
PORT_FN(0x0047b8e0, "CustomWidget::Over", CustomWidget_Over_n, fp_custom0)
static void __fastcall CustomWidget_NotOver_n(CustomWidget* self, Edx) {
    self->over = 0;
    vcall<void>(CTRL(self), 0x38);
}
PORT_FN(0x0047b8f0, "CustomWidget::NotOver", CustomWidget_NotOver_n, fp_custom0)
static void __fastcall CustomWidget_Focus_n(CustomWidget* self, Edx) {
    const uint8_t vis = self->visible;
    self->focus = 1;
    if (vis != 0) self->dirty = 1;
    vcall<void>(CTRL(self), 0x3c);
}
PORT_FN(0x0047b900, "CustomWidget::Focus", CustomWidget_Focus_n, fp_custom0)
static void __fastcall CustomWidget_UnFocus_n(CustomWidget* self, Edx) {
    self->focus = 0;
    dirty_if_visible(self);
    vcall<void>(CTRL(self), 0x40);
}
PORT_FN(0x0047b920, "CustomWidget::UnFocus", CustomWidget_UnFocus_n, fp_custom0)
// CustomWidget::CharHit: the control first while focused, then Widget::CharHit
static uint8_t __fastcall CustomWidget_CharHit_n(CustomWidget* self, Edx, uint16_t key) {
    if (self->focus != 0) {
        if (vcall<uint8_t>(CTRL(self), 0x48, key)) return 1;
    }
    if (tcall<uint8_t>(F_Widget_CharHit, self, key)) return 1;
    return 0;
}
PORT_FN(0x0047b940, "CustomWidget::CharHit", CustomWidget_CharHit_n, fp_custom_key)
static uint8_t __fastcall CustomWidget_IsTabStop_n(CustomWidget* self, Edx) { return vcall<uint8_t>(CTRL(self), 0x4c); }
PORT_FN(0x0047b980, "CustomWidget::IsTabStop", CustomWidget_IsTabStop_n, fp_custom0)
static void __fastcall CustomWidget_Draw_n(CustomWidget* self, Edx, gxCanvas* c) { vcall<void>(CTRL(self), 0x18, c); }
PORT_FN(0x0047b990, "CustomWidget::Draw", CustomWidget_Draw_n, fp_custom_draw)
static void __fastcall CustomWidget_Draw3D_n(CustomWidget* self, Edx) { vcall<void>(CTRL(self), 0x1c); }
PORT_FN(0x0047b9b0, "CustomWidget::Draw3D", CustomWidget_Draw3D_n, fp_custom0)
#undef CTRL

static void __fastcall TitleBar_MouseUp_n(TitleBar* self, Edx, int32_t, int32_t) { self->dragging = 0; }
static void fp_TitleBar_MouseUp(Footprint& f, TitleBar* self, Edx, int32_t, int32_t) { f.add(self, sizeof(TitleBar), "title bar"); }
PORT_FN(0x0047b9e0, "TitleBar::MouseUp", TitleBar_MouseUp_n, fp_TitleBar_MouseUp)

// =====================================================================================================================
// _widget.obj
// =====================================================================================================================
// ---- Widget -----------------------------------------------------------------------------------------------------------
static Widget* __fastcall Widget_ctor_n(Widget* self, Edx, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    self->vtbl = (const void*)(uintptr_t)VT_Widget;
    self->visible = 1;
    self->enabled = 1;
    self->dirty = 1;
    self->groups = 0;
    self->over = 0;
    self->nnotes = 0;
    self->x0 = x0;
    self->x1 = x1;
    self->focus = 0;
    self->window = 0;
    self->y0 = y0;
    self->y1 = y1;
    return self;
}
static void fp_Widget_ctor(Footprint& f, Widget* self, Edx, int32_t, int32_t, int32_t, int32_t) { f.add(self, sizeof(Widget), "this"); }
PORT_FN(0x0047bc30, "Widget::Widget", Widget_ctor_n, fp_Widget_ctor)

// ~Widget: the notifications' copies freed
static void __fastcall Widget_dtor_n(Widget* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_Widget;
    for (int32_t i = 0; i < self->nnotes;) {
        void* c = self->notes[i].copy;
        i++;
        ccall<void>(F_Delete, c);
    }
}
PORT_FN(0x0047bc80, "Widget::~Widget", Widget_dtor_n, fp_frees0)

static void __fastcall Widget_Move_n(Widget* self, Edx, int32_t x, int32_t y) {
    const int32_t w = self->x1 - self->x0;
    const int32_t h = self->y1 - self->y0;
    self->x0 = x;
    self->x1 = x + w;
    self->y0 = y;
    self->y1 = y + h;
}
PORT_FN(0x0047bcc0, "Widget::Move", Widget_Move_n, fp_self_xy)

static void __fastcall Widget_Resize_n(Widget* self, Edx, int32_t w, int32_t h) {
    self->x1 = self->x0 + w;
    self->y1 = self->y0 + h;
}
PORT_FN(0x0047bcf0, "Widget::Resize", Widget_Resize_n, fp_self_xy)

static uint8_t __fastcall Widget_CharHit_n(Widget*, Edx, uint16_t) { return 0; }
static void fp_Widget_CharHit(Footprint& f, Widget*, Edx, uint16_t) { f.pure = true; }
PORT_FN(0x0047bd20, "Widget::CharHit", Widget_CharHit_n, fp_Widget_CharHit)

static uint8_t __fastcall Widget_IsDirty_n(Widget* self, Edx) {
    if (self->dirty == 0) return 0;
    self->dirty = 0;
    return 1;
}
PORT_FN(0x0047bd30, "Widget::IsDirty", Widget_IsDirty_n, fp_self0)

// Widget::Update: each watched value compared with its copy; a changed one copied (byte by byte) and Callback(id, ptr)
static void __fastcall Widget_Update_n(Widget* self, Edx) {
    volatile int32_t i = 0;
    if (self->nnotes <= 0) return;
    Notification* n = self->notes;
    do {
        if (crt_memcmp_ne(n->ptr, n->copy, n->size)) {
            crt_copy_bytes(n->copy, n->ptr, n->size);
            vcall<void>(self, 0x50, (int32_t)n->id, (const void*)n->ptr);
        }
        n++;
        i = i + 1;
    } while (self->nnotes > i);
}
PORT_FN(0x0047bd50, "Widget::Update", Widget_Update_n, fp_reach0)

// AddNotification: a copy of *ptr (size bytes) kept to compare against (no bound on the 32 entries)
static void __fastcall Widget_AddNotification_n(Widget* self, Edx, int32_t id, const void* ptr, uint32_t size) {
    void* copy = ccall<void*>(F_MemAlloc, (int32_t)size);
    self->notes[self->nnotes].id = id;
    self->notes[self->nnotes].ptr = ptr;
    self->notes[self->nnotes].size = size;
    self->notes[self->nnotes].copy = copy;
    crt_copy(copy, ptr, size);
    self->nnotes = self->nnotes + 1;
}
static void fp_Widget_AddNotification(Footprint& f, Widget*, Edx, int32_t, const void*, uint32_t) { f.replay_only = "allocates the value's copy"; }
PORT_FN(0x0047bdc0, "Widget::AddNotification", Widget_AddNotification_n, fp_Widget_AddNotification)

// RemoveNotification: its copy freed and the later ones moved down (a panic if it isn't watched)
static void __fastcall Widget_RemoveNotification_n(Widget* self, Edx, const void* ptr) {
    const int32_t n = self->nnotes;
    int32_t i = 0;
    for (; i < n; i++)
        if (self->notes[i].ptr == ptr) break;
    if (i >= n) {
        UI_LogPanic((const char*)0x004f7124);
        return;
    }
    ccall<void>(F_Delete, (void*)self->notes[i].copy);
    const int32_t m = self->nnotes - 1;
    self->nnotes = m;
    ccall<void*>(F_memmove, (void*)&self->notes[i], (const void*)&self->notes[i + 1], (uint32_t)(m - i) << 4);
}
static void fp_Widget_RemoveNotification(Footprint& f, Widget*, Edx, const void*) { f.replay_only = "frees the value's copy"; }
PORT_FN(0x0047be30, "Widget::RemoveNotification", Widget_RemoveNotification_n, fp_Widget_RemoveNotification)

// ---- StyleWidget ------------------------------------------------------------------------------------------------------
static StyleWidget* __fastcall StyleWidget_ctor_n(StyleWidget* self, Edx, int32_t x, int32_t y, int32_t style) {
    tcall<Widget*>(F_Widget_ctor, self, x, y, x, y);
    self->vtbl = (const void*)(uintptr_t)VT_StyleWidget;
    self->tx = x;
    self->ty = y;
    self->style = style;
    return self;
}
static void fp_StyleWidget_ctor(Footprint& f, StyleWidget* self, Edx, int32_t, int32_t, int32_t) { f.add(self, sizeof(StyleWidget), "this"); }
PORT_FN(0x0047beb0, "StyleWidget::StyleWidget", StyleWidget_ctor_n, fp_StyleWidget_ctor)

// StyleWidget::Draw: its text in its style, in the state GetState gives (through the vtable it has on entry)
static void __fastcall StyleWidget_Draw_n(StyleWidget* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    void* const* vt = *(void* const* volatile*)self;
    typedef uint32_t(__fastcall * U_t)(StyleWidget*, Edx);
    typedef const char*(__fastcall * T_t)(StyleWidget*, Edx);
    const uint32_t state = ((U_t)vt[0x58 / 4])(self, 0);
    const char* text = ((T_t)vt[0x5c / 4])(self, 0);
    ccall<void>(F_UIStyleDraw, (int32_t)self->style, (int32_t)self->tx, (int32_t)self->ty, text, state);
}
PORT_FN(0x0047bef0, "StyleWidget::Draw", StyleWidget_Draw_n, fp_draw)

// UpdateText: the rectangle grown to take in the text's bounds
static void __fastcall StyleWidget_UpdateText_n(StyleWidget* self, Edx) {
    int32_t bx0, by0, bx1, by1;
    const char* text = vcall<const char*>(self, 0x5c);
    ccall<void>(F_UIStyleGetBounds, (int32_t)self->style, text, (int32_t)self->tx, (int32_t)self->ty, &bx0, &by0, &bx1, &by1);
    int32_t a = self->x0;
    if (!(a < bx0)) a = bx0;
    const int32_t nx0 = a;
    a = self->x1;
    if (!(a > bx1)) a = bx1;
    const int32_t nx1 = a;
    a = self->y0;
    if (!(a < by0)) a = by0;
    const int32_t ny0 = a;
    a = self->y1;
    if (!(a > by1)) a = by1;
    const int32_t ny1 = a;
    tcall<void>(F_Widget_Move, self, nx0, ny0);
    tcall<void>(F_Widget_Resize, self, nx1 - nx0, ny1 - ny0);
}
PORT_FN(0x0047bf40, "StyleWidget::UpdateText", StyleWidget_UpdateText_n, fp_self0)

static uint32_t __fastcall StyleWidget_GetState_n(StyleWidget*, Edx) { return 0; }
PORT_FN(0x0047e8b0, "StyleWidget::GetState", StyleWidget_GetState_n, fp_pure0)

// ---- Button -----------------------------------------------------------------------------------------------------------
static Button* __fastcall Button_ctor_n(Button* self, Edx, int32_t id, int32_t x, int32_t y, const char* text, UICallback cb,
                                        int32_t style, uint8_t keys, uint8_t repeat) {
    tcall<StyleWidget*>(F_StyleWidget_ctor, self, x, y, style);
    self->id = id;
    self->cb = cb;
    self->vtbl = (const void*)(uintptr_t)VT_Button;
    self->keys = keys;
    self->repeat = repeat;
    self->b235 = 0;
    crt_strcpy(self->text, text);
    self->pressed = 0;
    tcall<void>(F_StyleWidget_UpdateText, self);
    return self;
}
static void fp_Button_ctor(Footprint& f, Button* self, Edx, int32_t, int32_t, int32_t, const char*, UICallback, int32_t, uint8_t, uint8_t) {
    f.add(self, sizeof(Button), "this");
}
PORT_FN(0x0047c000, "Button::Button", Button_ctor_n, fp_Button_ctor)

// Button::CharHit (while focused, not a label): space / return press it, the Tab arrows (0x126 / 0x128) move the focus
static uint8_t __fastcall Button_CharHit_n(Button* self, Edx, uint16_t key) {
    if (vcall<int32_t>(self, 0x10) != (int32_t)0xfffefb96 && self->focus != 0) {
        const uint8_t keys = self->keys;
        if (keys != 0 && (key == 0x20 || key == 0xd)) {
            fire(self->cb, self->id, &self->id);
            dirty_if_visible(self);
            return 1;
        }
        if (keys != 0) {
            if (key == 0x126) {
                tcall<void>(F_Widget_TabPrev, self);
                return 1;
            }
            if (key == 0x128) {
                tcall<void>(F_Widget_TabNext, self);
                return 1;
            }
        }
    }
    return tcall<uint8_t>(F_Widget_CharHit, self, key);
}
static void fp_Button_CharHit(Footprint& f, Button* self, Edx, uint16_t) {
    if (!ui_fp_reach(f, self)) return;
    if (self->keys) ui_fp_active_focus(f);
}
PORT_FN(0x0047c090, "Button::CharHit", Button_CharHit_n, fp_Button_CharHit)

// ---- BButton ----------------------------------------------------------------------------------------------------------
static BButton* __fastcall BButton_ctor_n(BButton* self, Edx, int32_t id, int32_t x, int32_t y, const char* stamp, UICallback cb,
                                          uint8_t keys, uint8_t repeat) {
    int32_t hx, hy;
    tcall<Widget*>(F_Widget_ctor, self, x, y, x, y);
    self->id = id;
    self->cb = cb;
    self->vtbl = (const void*)(uintptr_t)VT_BButton;
    self->keys = keys;
    self->flash = 0;
    self->repeat = repeat;
    void* st = ccall<void*>(F_gxGetStamp, stamp);
    self->stamp = st;
    const int32_t h = ccall<int32_t>(F_gxStampHeight, st);
    const int32_t w = ccall<int32_t>(F_gxStampWidth, st);
    tcall<void>(F_Widget_Resize, self, w, h);
    ccall<void>(F_gxStampHotSpot, (void*)self->stamp, &hx, &hy);
    const int32_t ny = self->y0 - hy;
    tcall<void>(F_Widget_Move, self, (int32_t)(self->x0 - hx), ny);
    self->pressed = 0;
    return self;
}
static void fp_BButton_ctor(Footprint& f, BButton*, Edx, int32_t, int32_t, int32_t, const char*, UICallback, uint8_t, uint8_t) {
    f.replay_only = "loads a stamp";
}
PORT_FN(0x0047c140, "BButton::BButton", BButton_ctor_n, fp_BButton_ctor)

// BButton::Draw: frame 0 / 1 (pressed while over, or flashed once), +2 while focused (with more than 2 frames), 4
// while disabled (with 5 or more)
static void __fastcall BButton_Draw_n(BButton* self, Edx, gxCanvas* c) {
    int32_t frame = 0;
    uint8_t down;
    if ((self->over != 0 && self->pressed != 0) || self->flash != 0) down = 1;
    else down = 0;
    self->flash = 0;
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    if (self->focus != 0 && ccall<int32_t>(F_gxStampCount, (void*)self->stamp) > 2) frame = 2;
    void* st = self->stamp;
    frame += down >= 1 ? 1 : 0;
    if (ccall<int32_t>(F_gxStampCount, st) >= 5 && self->enabled == 0) frame = 4;
    ccall<void>(F_gxDrawStamp, (void*)self->stamp, (int32_t)self->x0, (int32_t)self->y0, frame, (void*)0);
}
PORT_FN(0x0047c200, "BButton::Draw", BButton_Draw_n, fp_draw)

static uint8_t __fastcall BButton_HitTest_n(BButton* self, Edx, int32_t x, int32_t y) {
    const int32_t x0 = self->x0;
    if (x < x0) return 0;
    if (self->x1 <= x) return 0;
    const int32_t y0 = self->y0;
    if (y < y0) return 0;
    if (self->y1 <= y) return 0;
    return ccall<uint8_t>(F_gxStampHitTest, (void*)self->stamp, x - x0, y - y0);
}
static void fp_BButton_HitTest(Footprint&, BButton*, Edx, int32_t, int32_t) {}
PORT_FN(0x0047c2a0, "BButton::HitTest", BButton_HitTest_n, fp_BButton_HitTest)

static uint8_t __fastcall BButton_CharHit_n(BButton* self, Edx, uint16_t key) {
    if (self->focus != 0) {
        const uint8_t keys = self->keys;
        if (keys != 0 && (key == 0xd || key == 0x20)) {
            fire(self->cb, self->id, &self->id);
            dirty_if_visible(self);
            return 1;
        }
        if (keys != 0) {
            if (key == 0x126) {
                tcall<void>(F_Widget_TabPrev, self);
                return 1;
            }
            if (key == 0x128) {
                tcall<void>(F_Widget_TabNext, self);
                return 1;
            }
        }
    }
    return tcall<uint8_t>(F_Widget_CharHit, self, key);
}
static void fp_BButton_CharHit(Footprint& f, BButton* self, Edx, uint16_t) {
    if (!ui_fp_reach(f, self)) return;
    if (self->keys) ui_fp_active_focus(f);
}
PORT_FN(0x0047c2f0, "BButton::CharHit", BButton_CharHit_n, fp_BButton_CharHit)

static void __fastcall BButton_MouseDown_n(BButton* self, Edx, int32_t, int32_t) {
    const uint8_t vis = self->visible;
    self->pressed = 1;
    if (vis != 0) self->dirty = 1;
}
PORT_FN(0x0047e900, "BButton::MouseDown", BButton_MouseDown_n, fp_self_xy)

static void __fastcall BButton_MouseUp_n(BButton* self, Edx, int32_t, int32_t) {
    dirty_if_visible(self);
    self->pressed = 0;
    if (self->over == 0) return;
    fire(self->cb, self->id, &self->id);
}
PORT_FN(0x0047e920, "BButton::MouseUp", BButton_MouseUp_n, fp_reach_xy)

static void __fastcall BButton_Over_n(BButton* self, Edx) {
    dirty_if_visible(self);
    self->over = 1;
}
PORT_FN(0x0047e970, "BButton::Over", BButton_Over_n, fp_self0)
static void __fastcall BButton_NotOver_n(BButton* self, Edx) {
    dirty_if_visible(self);
    self->over = 0;
}
PORT_FN(0x0047e990, "BButton::NotOver", BButton_NotOver_n, fp_self0)

static void __fastcall BButton_Update_n(BButton* self, Edx) {
    if (self->repeat == 0 || self->pressed == 0 || self->over == 0) return;
    fire(self->cb, self->id, &self->id);
}
PORT_FN(0x0047e9c0, "BButton::Update", BButton_Update_n, fp_reach0)

// the deleting destructors with an inlined destructor that forgets a stamp (BButton, BRadioButton, Slider, ScrollBar)
static __forceinline void* stamp_dtor(Widget* self, uint32_t vt, void* stamp, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)vt;
    ccall<void>(F_gxForgetStamp, stamp);
    tcall<void>(F_Widget_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void* __fastcall BButton_ddtor_n(BButton* self, Edx, uint32_t flags) { return stamp_dtor(self, VT_BButton, self->stamp, flags); }
PORT_FN(0x0047ea10, "BButton::scalar deleting destructor", BButton_ddtor_n, fp_dtor_flags)

// ---- CheckBox ---------------------------------------------------------------------------------------------------------
static CheckBox* __fastcall CheckBox_ctor_n(CheckBox* self, Edx, int32_t x, int32_t y, const char* text, uint8_t* value, int32_t style) {
    tcall<StyleWidget*>(F_StyleWidget_ctor, self, x, y, style);
    self->vtbl = (const void*)(uintptr_t)VT_CheckBox;
    crt_strcpy(self->text, text);
    self->value = value;
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)value, (uint32_t)1);
    tcall<void>(F_StyleWidget_UpdateText, self);
    return self;
}
static void fp_CheckBox_ctor(Footprint& f, CheckBox*, Edx, int32_t, int32_t, const char*, uint8_t*, int32_t) { f.replay_only = "adds a notification (allocates)"; }
PORT_FN(0x0047c390, "CheckBox::CheckBox", CheckBox_ctor_n, fp_CheckBox_ctor)

static uint8_t __fastcall CheckBox_CharHit_n(CheckBox* self, Edx, uint16_t key) { return tcall<uint8_t>(F_Widget_CharHit, self, key); }
PORT_FN(0x0047c400, "CheckBox::CharHit", CheckBox_CharHit_n, fp_none_key)

static void __fastcall CheckBox_MouseDown_n(CheckBox* self, Edx, int32_t, int32_t) {
    volatile uint8_t* v = self->value;
    *v = *v < 1 ? 1 : 0;
}
static void fp_CheckBox_MouseDown(Footprint& f, CheckBox* self, Edx, int32_t, int32_t) { f.add(self->value, 1, "check box value"); }
PORT_FN(0x0047ea50, "CheckBox::MouseDown", CheckBox_MouseDown_n, fp_CheckBox_MouseDown)

static void __fastcall CheckBox_Over_n(CheckBox* self, Edx) {
    dirty_if_visible(self);
    self->over = 1;
}
PORT_FN(0x0047ea80, "CheckBox::Over", CheckBox_Over_n, fp_self0)
static void __fastcall CheckBox_NotOver_n(CheckBox* self, Edx) {
    dirty_if_visible(self);
    self->over = 0;
}
PORT_FN(0x0047eaa0, "CheckBox::NotOver", CheckBox_NotOver_n, fp_self0)

static const char* __fastcall CheckBox_GetText_n(CheckBox* self, Edx) { return self->text; }
PORT_FN(0x0047eac0, "CheckBox::GetText", CheckBox_GetText_n, fp_none0)

// CheckBox::GetState: 1 checked, | 2 over
static uint32_t __fastcall CheckBox_GetState_n(CheckBox* self, Edx) {
    const uint32_t on = *(const volatile uint8_t*)self->value < 1 ? 0u : 1u;
    const uint32_t ov = self->over < 1 ? 0u : 2u;
    return on | ov;
}
PORT_FN(0x0047ead0, "CheckBox::GetState", CheckBox_GetState_n, fp_none0)

// ---- StampRoll --------------------------------------------------------------------------------------------------------
static StampRoll* __fastcall StampRoll_ctor_n(StampRoll* self, Edx, int32_t x, int32_t y, const char* stamp, int32_t* frame) {
    tcall<Widget*>(F_Widget_ctor, self, x, y, x, y);
    self->vtbl = (const void*)(uintptr_t)VT_StampRoll;
    self->frame = frame;
    if (frame) tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)frame, (uint32_t)4);
    void* st = ccall<void*>(F_gxGetStamp, stamp);
    self->stamp = st;
    const int32_t h = ccall<int32_t>(F_gxStampHeight, st);
    const int32_t w = ccall<int32_t>(F_gxStampWidth, st);
    tcall<void>(F_Widget_Resize, self, w, h);
    return self;
}
static void fp_StampRoll_ctor(Footprint& f, StampRoll*, Edx, int32_t, int32_t, const char*, int32_t*) { f.replay_only = "loads a stamp, adds a notification"; }
PORT_FN(0x0047c410, "StampRoll::StampRoll", StampRoll_ctor_n, fp_StampRoll_ctor)

// StampRoll::Draw: frame *frame (nothing while it's negative), or 0 without one
static void __fastcall StampRoll_Draw_n(StampRoll* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    int32_t* fr = self->frame;
    if (fr) {
        const int32_t n = *(volatile int32_t*)fr;
        if (n < 0) return;
        ccall<void>(F_gxDrawStamp, (void*)self->stamp, (int32_t)self->x0, (int32_t)self->y0, n, (void*)0);
        return;
    }
    ccall<void>(F_gxDrawStamp, (void*)self->stamp, (int32_t)self->x0, (int32_t)self->y0, (int32_t)0, (void*)0);
}
PORT_FN(0x0047c480, "StampRoll::Draw", StampRoll_Draw_n, fp_draw)

static void __fastcall StampRoll_dtor_n(StampRoll* self, Edx) {
    void* st = self->stamp;
    self->vtbl = (const void*)(uintptr_t)VT_StampRoll;
    ccall<void>(F_gxForgetStamp, st);
    tcall<void>(F_Widget_dtor, self);
}
PORT_FN(0x0047c4e0, "StampRoll::~StampRoll", StampRoll_dtor_n, fp_frees0)

// ---- RadioButton ------------------------------------------------------------------------------------------------------
static RadioButton* __fastcall RadioButton_ctor_n(RadioButton* self, Edx, int32_t x, int32_t y, const char* text, int32_t value,
                                                  int32_t* var, int32_t style, uint8_t groups_mode) {
    tcall<StyleWidget*>(F_StyleWidget_ctor, self, x, y, style);
    self->vtbl = (const void*)(uintptr_t)VT_RadioButton;
    crt_strcpy(self->text, text);
    self->var = var;
    int32_t v = value;
    if (groups_mode) v = *(volatile int32_t*)(uintptr_t)(uint32_t)value;
    self->value = v;
    self->groups_mode = groups_mode;
    if (var) tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)var, (uint32_t)4);
    tcall<void>(F_StyleWidget_UpdateText, self);
    return self;
}
static void fp_RadioButton_ctor(Footprint& f, RadioButton* self, Edx, int32_t, int32_t, const char*, int32_t, int32_t* var, int32_t, uint8_t) {
    if (var) f.replay_only = "adds a notification (allocates)";
    else f.add(self, sizeof(RadioButton), "this");
}
PORT_FN(0x0047c510, "RadioButton::RadioButton", RadioButton_ctor_n, fp_RadioButton_ctor)

static uint8_t __fastcall RadioButton_CharHit_n(RadioButton* self, Edx, uint16_t key) { return tcall<uint8_t>(F_Widget_CharHit, self, key); }
PORT_FN(0x0047c5a0, "RadioButton::CharHit", RadioButton_CharHit_n, fp_none_key)

// RadioButton::Callback: in group mode, its group shown while it's selected, hidden otherwise
static void __fastcall RadioButton_Callback_n(RadioButton* self, Edx, int32_t, const void*) {
    if (self->groups_mode != 0) {
        const int32_t v = self->value;
        if (*(volatile int32_t*)self->var == v) ccall<void>(F_UIShowGroup, (uint32_t)v);
        else ccall<void>(F_UIHideGroup, (uint32_t)v);
    }
    dirty_if_visible(self);
}
PORT_FN(0x0047c5b0, "RadioButton::Callback", RadioButton_Callback_n, fp_reach_cb)

static void __fastcall RadioButton_Activate_n(RadioButton* self, Edx) { vcall<void>(self, 0x50, (int32_t)0, (const void*)0); }
PORT_FN(0x0047eb30, "RadioButton::Activate", RadioButton_Activate_n, fp_reach0)

static void __fastcall RadioButton_Over_n(RadioButton* self, Edx) {
    dirty_if_visible(self);
    self->over = 1;
}
PORT_FN(0x0047eb50, "RadioButton::Over", RadioButton_Over_n, fp_self0)
static void __fastcall RadioButton_NotOver_n(RadioButton* self, Edx) {
    dirty_if_visible(self);
    self->over = 0;
}
PORT_FN(0x0047eb70, "RadioButton::NotOver", RadioButton_NotOver_n, fp_self0)

static void __fastcall RadioButton_MouseDown_n(RadioButton* self, Edx, int32_t, int32_t) {
    int32_t* v = self->var;
    if (v) *(volatile int32_t*)v = self->value;
}
static void fp_RadioButton_MouseDown(Footprint& f, RadioButton* self, Edx, int32_t, int32_t) { if (self->var) f.add(self->var, 4, "radio variable"); }
PORT_FN(0x0047eb90, "RadioButton::MouseDown", RadioButton_MouseDown_n, fp_RadioButton_MouseDown)

static const char* __fastcall RadioButton_GetText_n(RadioButton* self, Edx) { return self->text; }
PORT_FN(0x0047ebb0, "RadioButton::GetText", RadioButton_GetText_n, fp_none0)

static uint32_t __fastcall RadioButton_GetState_n(RadioButton* self, Edx) {
    uint32_t on = 0;
    const int32_t* v = self->var;
    if (v) {
        const int32_t x = *(const volatile int32_t*)v;
        on = 1;
        if (self->value != x) on = 0;
    }
    const uint32_t ov = self->over < 1 ? 0u : 2u;
    return ov | on;
}
PORT_FN(0x0047ebc0, "RadioButton::GetState", RadioButton_GetState_n, fp_none0)

// ---- BRadioButton -----------------------------------------------------------------------------------------------------
static BRadioButton* __fastcall BRadioButton_ctor_n(BRadioButton* self, Edx, int32_t x, int32_t y, const char* stamp, int32_t value,
                                                    int32_t* var) {
    int32_t hx, hy;
    tcall<Widget*>(F_Widget_ctor, self, x, y, x, y);
    self->vtbl = (const void*)(uintptr_t)VT_BRadioButton;
    self->var = var;
    self->stamp = ccall<void*>(F_gxGetStamp, stamp);
    void* st = self->stamp;
    self->value = value;
    const int32_t h = ccall<int32_t>(F_gxStampHeight, st);
    const int32_t w = ccall<int32_t>(F_gxStampWidth, st);
    tcall<void>(F_Widget_Resize, self, w, h);
    ccall<void>(F_gxStampHotSpot, (void*)self->stamp, &hx, &hy);
    const int32_t ny = self->y0 - hy;
    tcall<void>(F_Widget_Move, self, (int32_t)(self->x0 - hx), ny);
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)var, (uint32_t)4);
    return self;
}
static void fp_BRadioButton_ctor(Footprint& f, BRadioButton*, Edx, int32_t, int32_t, const char*, int32_t, int32_t*) { f.replay_only = "loads a stamp, adds a notification"; }
PORT_FN(0x0047c5f0, "BRadioButton::BRadioButton", BRadioButton_ctor_n, fp_BRadioButton_ctor)

static uint8_t __fastcall BRadioButton_CharHit_n(BRadioButton* self, Edx, uint16_t key) { return tcall<uint8_t>(F_Widget_CharHit, self, key); }
PORT_FN(0x0047c6a0, "BRadioButton::CharHit", BRadioButton_CharHit_n, fp_none_key)

// BRadioButton::Draw: frame 1 while selected, else 0
static void __fastcall BRadioButton_Draw_n(BRadioButton* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    tcall<void>(F_Widget_Draw, self, c);
    const int32_t d = *(volatile int32_t*)self->var - self->value;
    const int32_t y0 = self->y0, x0 = self->x0;
    ccall<void>(F_gxDrawStamp, (void*)self->stamp, x0, y0, (int32_t)(d == 0 ? 1 : 0), (void*)0);
}
PORT_FN(0x0047c6b0, "BRadioButton::Draw", BRadioButton_Draw_n, fp_draw)

static void __fastcall BRadioButton_MouseDown_n(BRadioButton* self, Edx, int32_t, int32_t) {
    const int32_t v = self->value;
    *(volatile int32_t*)self->var = v;
}
static void fp_BRadioButton_MouseDown(Footprint& f, BRadioButton* self, Edx, int32_t, int32_t) { f.add(self->var, 4, "radio variable"); }
PORT_FN(0x0047ec20, "BRadioButton::MouseDown", BRadioButton_MouseDown_n, fp_BRadioButton_MouseDown)

static void* __fastcall BRadioButton_ddtor_n(BRadioButton* self, Edx, uint32_t flags) { return stamp_dtor(self, VT_BRadioButton, self->stamp, flags); }
PORT_FN(0x0047ec40, "BRadioButton::scalar deleting destructor", BRadioButton_ddtor_n, fp_dtor_flags)

// ---- ScrollButtonBase / ArrowButton / ScrollButton ----------------------------------------------------------------------
static ScrollButtonBase* __fastcall ScrollButtonBase_ctor_n(ScrollButtonBase* self, Edx, int32_t x, int32_t y, void* stamp) {
    const int32_t h = ccall<int32_t>(F_gxStampHeight, stamp);
    const int32_t w = ccall<int32_t>(F_gxStampWidth, stamp);
    tcall<Widget*>(F_Widget_ctor, self, x, y, x + w, y + h);
    const uint32_t rate = UI_GU32(0x004ddc68);              // 0.3f, copied as its bits
    self->vtbl = (const void*)(uintptr_t)VT_ScrollButtonBase;
    self->stamp = stamp;
    self->pressed = 0;
    *(volatile uint32_t*)&self->timer = 0;
    *(volatile uint32_t*)&self->rate = rate;
    return self;
}
static void fp_ScrollButtonBase_ctor(Footprint& f, ScrollButtonBase* self, Edx, int32_t, int32_t, void*) { f.add(self, sizeof(ScrollButtonBase), "this"); }
PORT_FN(0x0047c700, "ScrollButtonBase::ScrollButtonBase", ScrollButtonBase_ctor_n, fp_ScrollButtonBase_ctor)

static void __fastcall ScrollButtonBase_Draw_n(ScrollButtonBase* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    int32_t frame = 0;
    if (self->pressed != 0 && self->over != 0) frame = 1;
    ccall<void>(F_gxDrawStamp, (void*)self->stamp, (int32_t)self->x0, (int32_t)self->y0, frame, (void*)0);
}
PORT_FN(0x0047c760, "ScrollButtonBase::Draw", ScrollButtonBase_Draw_n, fp_draw)

// ScrollButtonBase::Update: held over it, the timer runs down by the frame's time; each time it passes zero (at most
// 11 a frame) Do() and the rate shortens (r' = 1 / (1/r + 2)); then the timer restarts at the rate. Let go: timer 0,
// rate 0.3.
static void __fastcall ScrollButtonBase_Update_n(ScrollButtonBase* self, Edx) {
    if (self->pressed == 0 || self->over == 0) {
        *(volatile uint32_t*)&self->timer = 0;
        *(volatile uint32_t*)&self->rate = UI_GU32(0x004ddc68);
        return;
    }
    const float dt = ccall<float>(F_UIDeltaT);
    const double t = D(self->timer) - D(dt);                // fsubr [timer]
    const bool below = !(t >= D(bits_f(0x00000000)));       // fcom 0.0f; test ah, 1
    self->timer = (float)t;
    if (!below) return;
    int32_t n = 10;
    for (;;) {
        if (!(*(volatile uint32_t*)&self->timer > 0x80000000u)) break;     // an integer compare of the bits
        self->timer = (float)(D(self->timer) + D(self->rate));
        vcall<void>(self, 0x58);                                         // Do
        double r = D(bits_f(0x3f800000)) / D(self->rate);               // fld rate; fdivr 1.0f
        n--;
        r = r + D(bits_f(0x40000000));                                  // fadd 2.0f
        r = D(bits_f(0x3f800000)) / r;                                  // fdivr 1.0f
        self->rate = (float)r;
        if (n < 0) break;
    }
    copy_f(&self->timer, &self->rate);
}
static void fp_ScrollButtonBase_Update(Footprint& f, ScrollButtonBase* self, Edx) { ui_fp_reach(f, self); }
PORT_FN(0x0047c7b0, "ScrollButtonBase::Update", ScrollButtonBase_Update_n, fp_ScrollButtonBase_Update)

static ArrowButton* __fastcall ArrowButton_ctor_n(ArrowButton* self, Edx, int32_t x, int32_t y, float* value, uint32_t lo, uint32_t hi,
                                                  int32_t steps, int32_t dir, void* stamp) {
    tcall<ScrollButtonBase*>(F_ScrollButtonBase_ctor, self, x, y, stamp);
    self->vtbl = (const void*)(uintptr_t)VT_ArrowButton;
    self->value = value;
    *(volatile uint32_t*)&self->lo = lo;
    *(volatile uint32_t*)&self->hi = hi;
    self->steps = steps;
    self->dir = dir;
    return self;
}
static void fp_ArrowButton_ctor(Footprint& f, ArrowButton* self, Edx, int32_t, int32_t, float*, uint32_t, uint32_t, int32_t, int32_t, void*) {
    f.add(self, sizeof(ArrowButton), "this");
}
PORT_FN(0x0047c8a0, "ArrowButton::ArrowButton", ArrowButton_ctor_n, fp_ArrowButton_ctor)

// ArrowButton::Do: one step ((hi - lo) / (steps - 1)) in its direction, snapped to a whole number of steps (+0.5 then
// __ftol: rounds half up for positive values), clamped to lo..hi (lo also for NaN)
static void __fastcall ArrowButton_Do_n(ArrowButton* self, Edx) {
    double step = D(self->hi) - D(self->lo);
    const int32_t sm1 = self->steps - 1;
    float* v = self->value;
    step = step / D(sm1);
    double t = D(self->dir) * step;
    t = t + D(*(volatile float*)v);
    *(volatile float*)v = (float)t;
    volatile float* v2 = self->value;
    double q = D(*v2) / step;
    q = q + D(bits_f(0x3f000000));
    const int32_t k = x87_ftol(q);
    *v2 = (float)(step * D(k));
    volatile float* v3 = self->value;
    if (!(D(*v3) >= D(self->lo))) copy_f(v3, &self->lo);
    volatile float* v4 = self->value;
    if (D(*v4) > D(self->hi)) copy_f(v4, &self->hi);
}
static void fp_ArrowButton_Do(Footprint& f, ArrowButton* self, Edx) { f.add(self->value, 4, "the value"); }
PORT_FN(0x0047c900, "ArrowButton::Do", ArrowButton_Do_n, fp_ArrowButton_Do)

static ScrollButton* __fastcall ScrollButton_ctor_n(ScrollButton* self, Edx, int32_t x, int32_t y, UIScrollAxis* axis, int32_t dir, void* stamp) {
    tcall<ScrollButtonBase*>(F_ScrollButtonBase_ctor, self, x, y, stamp);
    self->vtbl = (const void*)(uintptr_t)VT_ScrollButton;
    self->dir = dir;
    self->axis = axis;
    return self;
}
static void fp_ScrollButton_ctor(Footprint& f, ScrollButton* self, Edx, int32_t, int32_t, UIScrollAxis*, int32_t, void*) {
    f.add(self, sizeof(ScrollButton), "this");
}
PORT_FN(0x0047c9c0, "ScrollButton::ScrollButton", ScrollButton_ctor_n, fp_ScrollButton_ctor)

// ScrollButton::Do: the axis's position one down (dir 1) or up, kept in 0..total - visible
static void __fastcall ScrollButton_Do_n(ScrollButton* self, Edx) {
    UIScrollAxis* a = self->axis;
    int32_t p;
    if (self->dir == 1) p = a->pos + 1;
    else p = a->pos - 1;
    a->pos = p;
    const int32_t m = a->total - a->visible;
    if (!(m >= p)) a->pos = m;
    if (a->pos < 0) a->pos = 0;
}
static void fp_ScrollButton_Do(Footprint& f, ScrollButton* self, Edx) { f.add(self->axis, sizeof(UIScrollAxis), "the axis"); }
PORT_FN(0x0047ca00, "ScrollButton::Do", ScrollButton_Do_n, fp_ScrollButton_Do)

static void __fastcall ScrollButtonBase_MouseDown_n(ScrollButtonBase* self, Edx, int32_t, int32_t) {
    *(volatile uint32_t*)&self->timer = 0;
    const uint8_t vis = self->visible;
    self->pressed = 1;
    if (vis != 0) self->dirty = 1;
}
PORT_FN(0x0047ec90, "ScrollButtonBase::MouseDown", ScrollButtonBase_MouseDown_n, fp_self_xy)

static void __fastcall ScrollButtonBase_MouseUp_n(ScrollButtonBase* self, Edx, int32_t, int32_t) {
    self->pressed = 0;
    dirty_if_visible(self);
}
PORT_FN(0x0047ecc0, "ScrollButtonBase::MouseUp", ScrollButtonBase_MouseUp_n, fp_self_xy)

static void __fastcall ScrollButtonBase_Over_n(ScrollButtonBase* self, Edx) {
    *(volatile uint32_t*)&self->timer = 0;
    const uint8_t vis = self->visible;
    self->over = 1;
    if (vis != 0) self->dirty = 1;
}
PORT_FN(0x0047ece0, "ScrollButtonBase::Over", ScrollButtonBase_Over_n, fp_self0)

static void __fastcall ScrollButtonBase_NotOver_n(ScrollButtonBase* self, Edx) {
    self->over = 0;
    dirty_if_visible(self);
}
PORT_FN(0x0047ed00, "ScrollButtonBase::NotOver", ScrollButtonBase_NotOver_n, fp_self0)

// ---- StaticText, LineWidget, FStaticText --------------------------------------------------------------------------------
static StaticText* __fastcall StaticText_ctor_n(StaticText* self, Edx, int32_t x, int32_t y, const char* text, int32_t style) {
    tcall<StyleWidget*>(F_StyleWidget_ctor, self, x, y, style);
    self->vtbl = (const void*)(uintptr_t)VT_StaticText;
    self->src = text;
    crt_strcpy(self->text, text);
    tcall<void>(F_StyleWidget_UpdateText, self);
    return self;
}
static void fp_StaticText_ctor(Footprint& f, StaticText* self, Edx, int32_t, int32_t, const char*, int32_t) { f.add(self, sizeof(StaticText), "this"); }
PORT_FN(0x0047ca60, "StaticText::StaticText", StaticText_ctor_n, fp_StaticText_ctor)

// StaticText::Update: copied again (and refitted) when the source string changed
static void __fastcall StaticText_Update_n(StaticText* self, Edx) {
    tcall<void>(F_Widget_Update, self);
    if (crt_strcmp_ne(self->src, self->text)) {
        crt_strcpy(self->text, self->src);
        tcall<void>(F_StyleWidget_UpdateText, self);
        dirty_if_visible(self);
    }
}
PORT_FN(0x0047cac0, "StaticText::Update", StaticText_Update_n, fp_reach0)

static const char* __fastcall StaticText_GetText_n(StaticText* self, Edx) { return self->src; }
PORT_FN(0x0047ed80, "StaticText::GetText", StaticText_GetText_n, fp_none0)

static LineWidget* __fastcall LineWidget_ctor_n(LineWidget* self, Edx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t hh = h;
    if (!(hh > 1)) hh = 1;
    const int32_t y1 = y + hh;
    int32_t ww = w;
    if (!(ww > 1)) ww = 1;
    const int32_t x1 = ww + x;
    tcall<Widget*>(F_Widget_ctor, self, x, y, x1, y1);
    self->vtbl = (const void*)(uintptr_t)VT_LineWidget;
    self->rx0 = x;
    self->ry0 = y;
    self->rx1 = x1;
    self->ry1 = y1;
    self->color = color;
    return self;
}
static void fp_LineWidget_ctor(Footprint& f, LineWidget* self, Edx, int32_t, int32_t, int32_t, int32_t, uint32_t) { f.add(self, sizeof(LineWidget), "this"); }
PORT_FN(0x0047cb40, "LineWidget::LineWidget", LineWidget_ctor_n, fp_LineWidget_ctor)

static void __fastcall LineWidget_Draw_n(LineWidget* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    ccall<void>(F_gxRect, (int32_t)self->rx0, (int32_t)self->ry0, (int32_t)self->rx1, (int32_t)self->ry1, (uint32_t)self->color);
}
PORT_FN(0x0047cbc0, "LineWidget::Draw", LineWidget_Draw_n, fp_draw)

static void __fastcall FStaticText_Draw_n(FStaticText* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    const char* fmt = self->fmt;
    const int32_t y = self->y0, x = self->x0;
    ((FontPrintf_t)(uintptr_t)F_gxFontPrintf)((void*)self->font, (void*)0, 9u, x, y, fmt);
}
static void fp_FStaticText_Draw(Footprint& f, FStaticText* self, Edx, gxCanvas* c) {
    f.add(self, sizeof(FStaticText), "this");
    UI_FP_CANVAS(f, c);
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
}
PORT_FN(0x0047cc00, "FStaticText::Draw", FStaticText_Draw_n, fp_FStaticText_Draw)

// ---- Numeric / IntNumeric / Multi ---------------------------------------------------------------------------------------
static void fp_numeric_ctor(Footprint& f, Numeric*, Edx, int32_t, int32_t, const void*, uint32_t, uint32_t, const char*, int32_t) {
    f.replay_only = "adds a notification (allocates)";
}
static Numeric* __fastcall Numeric_ctor_n(Numeric* self, Edx, int32_t x, int32_t y, const void* value, uint32_t scale, uint32_t offset,
                                          const char* format, int32_t style) {
    tcall<StyleWidget*>(F_StyleWidget_ctor, self, x, y, style);
    self->vtbl = (const void*)(uintptr_t)VT_Numeric;
    self->value = value;
    *(volatile uint32_t*)&self->scale = scale;
    *(volatile uint32_t*)&self->offset = offset;
    self->text[0] = 0;
    self->format = format;
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, value, (uint32_t)4);
    tcall<void>(F_Numeric_update_text, self);
    return self;
}
PORT_FN(0x0047cc40, "Numeric::Numeric", Numeric_ctor_n, fp_numeric_ctor)

// Numeric::update_text: sprintf(format, *value * scale + offset), the locale's decimal point, refitted, dirty
static void __fastcall Numeric_update_text_n(Numeric* self, Edx) {
    double d = D(*(const volatile float*)self->value) * D(self->scale);
    d = d + D(self->offset);
    UI_sprintf(self->text, self->format, d);
    ccall<void>(F_LocaleConvertNumeric, (char*)self->text);
    tcall<void>(F_StyleWidget_UpdateText, self);
    dirty_if_visible(self);
}
PORT_FN(0x0047ccb0, "Numeric::update_text", Numeric_update_text_n, fp_self0)

static void __fastcall Numeric_Callback_n(Numeric* self, Edx, int32_t, const void*) { tcall<void>(F_Numeric_update_text, self); }
PORT_FN(0x0047cd10, "Numeric::Callback", Numeric_Callback_n, fp_self_cb)

static Numeric* __fastcall IntNumeric_ctor_n(Numeric* self, Edx, int32_t x, int32_t y, const void* value, uint32_t scale, uint32_t offset,
                                             const char* format, int32_t style) {
    tcall<StyleWidget*>(F_StyleWidget_ctor, self, x, y, style);
    self->vtbl = (const void*)(uintptr_t)VT_IntNumeric;
    self->value = value;
    *(volatile uint32_t*)&self->scale = scale;
    *(volatile uint32_t*)&self->offset = offset;
    self->text[0] = 0;
    self->format = format;
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, value, (uint32_t)4);
    tcall<void>(F_IntNumeric_update_text, self);
    return self;
}
PORT_FN(0x0047cd20, "IntNumeric::IntNumeric", IntNumeric_ctor_n, fp_numeric_ctor)

// IntNumeric::update_text: sprintf(format, (int)(*value * scale + offset + 0.5))
static void __fastcall IntNumeric_update_text_n(Numeric* self, Edx) {
    double d = D(*(const volatile int32_t*)self->value) * D(self->scale);
    d = d + D(self->offset);
    d = d + D(bits_f(0x3f000000));
    const int32_t n = x87_ftol(d);
    UI_sprintf(self->text, self->format, n);
    ccall<void>(F_LocaleConvertNumeric, (char*)self->text);
    tcall<void>(F_StyleWidget_UpdateText, self);
    dirty_if_visible(self);
}
PORT_FN(0x0047cd90, "IntNumeric::update_text", IntNumeric_update_text_n, fp_self0)

static void __fastcall IntNumeric_Callback_n(Numeric* self, Edx, int32_t, const void*) { tcall<void>(F_IntNumeric_update_text, self); }
PORT_FN(0x0047cdf0, "IntNumeric::Callback", IntNumeric_Callback_n, fp_self_cb)

static const char* __fastcall Numeric_GetText_n(Numeric* self, Edx) { return self->text; }
PORT_FN(0x0047edd0, "Numeric::GetText", Numeric_GetText_n, fp_none0)
static const char* __fastcall IntNumeric_GetText_n(Numeric* self, Edx) { return self->text; }
PORT_FN(0x0047ee00, "IntNumeric::GetText", IntNumeric_GetText_n, fp_none0)

// Multi: strings one after another, ended by an empty one
static Multi* __fastcall Multi_ctor_n(Multi* self, Edx, int32_t x, int32_t y, int32_t* var, const char* strings, int32_t style) {
    tcall<StyleWidget*>(F_StyleWidget_ctor, self, x, y, style);
    self->vtbl = (const void*)(uintptr_t)VT_Multi;
    self->var = var;
    self->count = 0;
    const char* p = strings;
    while (*(const volatile char*)p != 0) {
        self->options[self->count] = p;
        self->count = self->count + 1;
        p += crt_strlen(p) + 1;
    }
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)var, (uint32_t)4);
    tcall<void>(F_Multi_update_text, self);
    return self;
}
static void fp_Multi_ctor(Footprint& f, Multi*, Edx, int32_t, int32_t, int32_t*, const char*, int32_t) { f.replay_only = "adds a notification (allocates)"; }
PORT_FN(0x0047ce00, "Multi::Multi", Multi_ctor_n, fp_Multi_ctor)

static void __fastcall Multi_MouseDown_n(Multi* self, Edx, int32_t, int32_t) {
    const int32_t last = self->count - 1;
    volatile int32_t* v = self->var;
    const int32_t cur = *v;
    if (last <= cur) *v = 0;
    else *v = cur + 1;
}
static void fp_Multi_MouseDown(Footprint& f, Multi* self, Edx, int32_t, int32_t) { f.add(self->var, 4, "multi variable"); }
PORT_FN(0x0047ce80, "Multi::MouseDown", Multi_MouseDown_n, fp_Multi_MouseDown)

static void __fastcall Multi_Callback_n(Multi* self, Edx, int32_t, const void*) { tcall<void>(F_Multi_update_text, self); }
PORT_FN(0x0047ceb0, "Multi::Callback", Multi_Callback_n, fp_self_cb)

// Multi::update_text: the selected string ("???" out of range), refitted, dirty
static void __fastcall Multi_update_text_n(Multi* self, Edx) {
    const int32_t i = *(const volatile int32_t*)self->var;
    const char* s;
    if (i >= 0 && self->count > i) s = self->options[i];
    else s = (const char*)0x004f714c;
    crt_strcpy(self->text, s);
    tcall<void>(F_StyleWidget_UpdateText, self);
    dirty_if_visible(self);
}
PORT_FN(0x0047cec0, "Multi::update_text", Multi_update_text_n, fp_self0)

static const char* __fastcall Multi_GetText_n(Multi* self, Edx) { return self->text; }
PORT_FN(0x0047ee30, "Multi::GetText", Multi_GetText_n, fp_none0)

// ---- ListBox / DropList ---------------------------------------------------------------------------------------------------
static ListBox* __fastcall ListBox_ctor_n(ListBox* self, Edx, int32_t x, int32_t y, int32_t w, int32_t h, const UIStringList* list,
                                          int32_t* sel, UIScrollAxis* axis) {
    tcall<Widget*>(F_Widget_ctor, self, x, y, x + w, y + h);
    self->vtbl = (const void*)(uintptr_t)VT_ListBox;
    self->dragging = 0;
    self->scroll = 0;
    self->row_h = ccall<int32_t>(F_UIStyleHeight, (int32_t)10, (const char*)0x004f7150) + 2;
    self->list = list;
    self->sel = sel;
    self->cols = w / 8;
    self->axis = axis;
    self->rows = h / self->row_h;
    if (axis) {
        axis->total = list->count;
        axis->visible = self->rows;
        tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)axis, (uint32_t)0xc);
    }
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)sel, (uint32_t)4);
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)((const uint8_t*)list + 0xc), (uint32_t)4);
    return self;
}
static void fp_ListBox_ctor(Footprint& f, ListBox*, Edx, int32_t, int32_t, int32_t, int32_t, const UIStringList*, int32_t*, UIScrollAxis*) {
    f.replay_only = "adds notifications (allocates)";
}
PORT_FN(0x0047cf30, "ListBox::ListBox", ListBox_ctor_n, fp_ListBox_ctor)

// ListBox::MouseMove while dragging: the row under the mouse selected, clamped to the list and the visible rows (past
// an end: scroll -1 / 1, stepped by Update)
static void __fastcall ListBox_MouseMove_n(ListBox* self, Edx, int32_t, int32_t y) {
    self->scroll = 0;
    if (self->dragging == 0) return;
    dirty_if_visible(self);
    int32_t top = 0;
    UIScrollAxis* a = self->axis;
    if (a) top = a->pos;
    int32_t r = top + (y - self->y0 - 2) / self->row_h;
    if (r < 0) r = 0;
    const UIStringList* l = self->list;
    const int32_t n = l->count;
    if (r >= n) r = n - 1;
    if (a) {
        const int32_t p = a->pos;
        if (r < p) {
            self->scroll = -1;
            r = p;
        }
        const int32_t e = a->visible + a->pos;
        if (r >= e) {
            self->scroll = 1;
            r = e - 1;
        }
    }
    if (r < 0) return;
    if (!(l->count > r)) return;
    *(volatile int32_t*)self->sel = r;
}
PORT_FN(0x0047d010, "ListBox::MouseMove", ListBox_MouseMove_n, fp_reach_xy)

// ListBox::Update: Widget::Update, then a scroll step while dragging past an end (the selection follows the edge)
static void __fastcall ListBox_Update_n(ListBox* self, Edx) {
    tcall<void>(F_Widget_Update, self);
    UIScrollAxis* a = self->axis;
    if (!a) return;
    const int32_t s = self->scroll;
    if (s == 0) return;
    const int32_t p = a->pos + s;
    a->pos = p;
    const int32_t m = a->total - a->visible;
    if (!(m >= p)) a->pos = m;
    if (a->pos < 0) a->pos = 0;
    const bool up = self->scroll < 0;
    UIScrollAxis* b = self->axis;
    if (up) {
        const int32_t v = b->pos;
        *(volatile int32_t*)self->sel = v;
        return;
    }
    const int32_t v = b->visible + b->pos - 1;
    *(volatile int32_t*)self->sel = v;
}
PORT_FN(0x0047d0c0, "ListBox::Update", ListBox_Update_n, fp_reach0)

// ListBox::Draw: the visible rows from the axis's position (the selected one on a highlight bar), in style 10
static void __fastcall ListBox_Draw_n(ListBox* self, Edx, gxCanvas* c) {
    int32_t top = 0;
    UIScrollAxis* a = self->axis;
    if (a) top = a->pos;
    tcall<void>(F_Widget_Draw, self, c);
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    for (int32_t i = 0; self->rows > i;) {
        if (!tcall<char*>(F_UIStringList_GetEntry, (const void*)self->list, top + i)) return;
        const uint8_t on = (*(volatile int32_t*)self->sel - (top + i)) == 0 ? 1 : 0;
        if (on) {
            const int32_t rh = self->row_h, y0 = self->y0;
            const uint32_t col = UI_GU32(S_C_HILITE);
            ccall<void>(F_gxRect, (int32_t)self->x0 + 2, i * rh + y0 + 2, (int32_t)self->x1 - 2, (i + 1) * rh + y0 + 2, col);
        }
        const char* t = tcall<char*>(F_UIStringList_GetEntry, (const void*)self->list, top + i);
        const int32_t y = self->row_h * i + self->y0 + 2;
        i++;
        ccall<void>(F_UIStyleDraw, (int32_t)10, (int32_t)self->x0 + 2, y, t, (uint32_t)(on >= 1 ? 1 : 0));
    }
}
PORT_FN(0x0047d130, "ListBox::Draw", ListBox_Draw_n, fp_draw)

static void __fastcall ListBox_MouseDown_n(ListBox* self, Edx, int32_t x, int32_t y) {
    self->scroll = 0;
    self->dragging = 1;
    vcall<void>(self, 0x34, x, y);
}
PORT_FN(0x0047ee70, "ListBox::MouseDown", ListBox_MouseDown_n, fp_reach_xy)

static void __fastcall ListBox_MouseUp_n(ListBox* self, Edx, int32_t, int32_t) {
    self->dragging = 0;
    self->scroll = 0;
}
PORT_FN(0x0047eea0, "ListBox::MouseUp", ListBox_MouseUp_n, fp_self_xy)

static DropList* __fastcall DropList_ctor_n(DropList* self, Edx, int32_t x, int32_t y, int32_t cols, int32_t rows, const UIStringList* list,
                                            int32_t* sel) {
    tcall<ListBox*>(F_ListBox_ctor, self, x, y, cols << 3, rows << 3, list, sel, (UIScrollAxis*)0);
    int32_t w = self->x1 - self->x0;
    const int32_t h = self->y1 - self->y0;
    self->vtbl = (const void*)(uintptr_t)VT_DropList;
    self->open_w = w;
    w += 0x12;
    self->open_h = h;
    const int32_t th = ccall<int32_t>(F_gxTextHeight) + 2;
    tcall<void>(F_Widget_Resize, self, w, th);
    return self;
}
static void fp_DropList_ctor(Footprint& f, DropList*, Edx, int32_t, int32_t, int32_t, int32_t, const UIStringList*, int32_t*) { f.replay_only = "adds notifications (allocates)"; }
PORT_FN(0x0047d220, "DropList::DropList", DropList_ctor_n, fp_DropList_ctor)

// DropList::Draw: while open (dragging), the list drawn below the field at its open size; then the field: a box, the
// selected entry, and a "`" after it
static void __fastcall DropList_Draw_n(DropList* self, Edx, gxCanvas* c) {
    char buf[0x50];
    if (self->dragging != 0) {
        tcall<void>(F_Widget_Move, self, (int32_t)self->x0, (int32_t)(self->y1 + 3));
        const int32_t h = self->y1 - self->y0;
        const int32_t w = self->x1 - self->x0;
        tcall<void>(F_Widget_Resize, self, (int32_t)self->open_w, (int32_t)self->open_h);
        tcall<void>(F_ListBox_Draw, self, c);
        tcall<void>(F_Widget_Resize, self, w, h);
        const int32_t ny = self->y0 + self->y0 - self->y1 - 3;
        tcall<void>(F_Widget_Move, self, (int32_t)self->x0, ny);
    }
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    tcall<void>(F_Widget_Draw, self, c);
    ccall<void>(F_gxRect, (int32_t)self->x0, (int32_t)self->y0, (int32_t)self->x1, (int32_t)self->y1, UI_GU32(S_C_HILITE));
    const char* s = tcall<char*>(F_UIStringList_GetEntry, (const void*)self->list, (int32_t)*(volatile int32_t*)self->sel);
    if (!s) s = (const char*)0x004f7154;
    UI_sprintf(buf, (const char*)0x004f7158, (int32_t)self->cols, s);
    ccall<void>(F_gxText, (int32_t)self->x0 + 1, (int32_t)self->y0 + 1, (const char*)buf, UI_GU32(S_C_TEXT));
}
PORT_FN(0x0047d290, "DropList::Draw", DropList_Draw_n, fp_draw)

static void __fastcall DropList_MouseDown_n(DropList* self, Edx, int32_t x, int32_t y) {
    const int32_t y0 = self->y0;
    self->scroll = 0;
    const int32_t d = y0 - self->y1;
    self->dragging = 1;
    vcall<void>(self, 0x34, x, d + y - 3);
}
PORT_FN(0x0047eee0, "DropList::MouseDown", DropList_MouseDown_n, fp_reach_xy)

static void __fastcall DropList_MouseMove_n(DropList* self, Edx, int32_t x, int32_t y) {
    const int32_t d = self->y0 - self->y1 + y - 3;
    tcall<void>(F_ListBox_MouseMove, self, x, d);
}
PORT_FN(0x0047ef10, "DropList::MouseMove", DropList_MouseMove_n, fp_reach_xy)

// ---- Input / CDetect --------------------------------------------------------------------------------------------------
static Input* __fastcall Input_ctor_n(Input* self, Edx, int32_t x, int32_t y, int32_t w, int32_t h, char* buf, int32_t width, int32_t lines,
                                      int32_t style, uint8_t clear) {
    tcall<Widget*>(F_Widget_ctor, self, x, y, x + w, y + h + 3);
    self->vtbl = (const void*)(uintptr_t)VT_Input;
    if (clear) *(volatile char*)buf = 0;
    self->buf = buf;
    self->style = style;
    self->width = iabs(width);
    self->lines = lines;
    if (lines == 1 && width > 0) self->scroll = ccall<void*>(F_gxGetStamp, (const char*)0x004f7160);
    else self->scroll = 0;
    self->_228 = 0;
    self->caret = ccall<void*>(F_gxGetStamp, (const char*)0x004f716c);
    self->cursor = (int32_t)crt_strlen(buf);
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)buf, (uint32_t)((uint32_t)self->width * (uint32_t)self->lines));
    return self;
}
static void fp_Input_ctor(Footprint& f, Input*, Edx, int32_t, int32_t, int32_t, int32_t, char*, int32_t, int32_t, int32_t, uint8_t) {
    f.replay_only = "loads stamps, adds a notification (allocates)";
}
PORT_FN(0x0047d3a0, "Input::Input", Input_ctor_n, fp_Input_ctor)

static void __fastcall Input_dtor_n(Input* self, Edx) {
    void* caret = self->caret;
    self->vtbl = (const void*)(uintptr_t)VT_Input;
    ccall<void>(F_gxForgetStamp, caret);
    void* sc = self->scroll;
    if (sc) ccall<void>(F_gxForgetStamp, sc);
    tcall<void>(F_Widget_dtor, self);
}
PORT_FN(0x0047d480, "Input::~Input", Input_dtor_n, fp_frees0)

// Input::Draw: a one-line field's frame stamp (its left two thirds from the left, its right two thirds from the right),
// the text (state 1 while focused), and the caret (a 10-pixel line) while focused; clipped to the field
static void __fastcall Input_Draw_n(Input* self, Edx, gxCanvas* c) {
    int32_t hx, hy;
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    ccall<void>(F_gxSetClip, c, (int32_t)self->x0, (int32_t)self->y0, (int32_t)self->x1, (int32_t)self->y1);
    tcall<void>(F_Widget_Draw, self, c);
    void* sc = self->scroll;
    if (sc) {
        ccall<void>(F_gxStampHotSpot, sc, &hx, &hy);
        int32_t x0 = self->x0;
        const int32_t y0 = self->y0;
        const int32_t y1 = self->y1;
        int32_t x1 = self->x1;
        hx += x0;
        hy += y0;
        ccall<void>(F_gxSetClip, c, x0, y0, (int32_t)((x1 - x0) * 2) / 3 + x0, y1);
        ccall<void>(F_gxDrawStamp, (void*)self->scroll, hx, hy, (int32_t)0, (void*)0);
        ccall<void>(F_gxRestoreClip, c);
        x1 = self->x1;
        x0 = self->x0;
        ccall<void>(F_gxSetClip, c, (x1 - x0) / 3 + x0, (int32_t)self->y0, x1, (int32_t)self->y1);
        void* st = self->scroll;
        const int32_t right = self->x1;
        const int32_t sw = ccall<int32_t>(F_gxStampWidth, st);
        ccall<void>(F_gxDrawStamp, st, right - sw - (int32_t)self->x0 + hx - 2, hy, (int32_t)0, (void*)0);
        ccall<void>(F_gxRestoreClip, c);
    }
    ccall<void>(F_gxSetClip, c, (int32_t)self->x0, (int32_t)self->y0, (int32_t)self->x1, (int32_t)self->y1);
    ccall<void>(F_UIStyleDraw, (int32_t)self->style, (int32_t)self->x0, (int32_t)self->y0, (const char*)self->buf,
                (uint32_t)(self->focus >= 1 ? 1 : 0));
    if (self->focus != 0) {
        ccall<void>(F_UIStyleIndexToPoint, (int32_t)self->style, (const char*)self->buf, (int32_t)self->cursor, &hx, &hy);
        const int32_t y = self->y0 + hy;
        const int32_t x = self->x0 + hx;
        ccall<void>(F_gxLine, x, y, x, y + 0xa, UI_GU32(S_C_CARET));
    }
    ccall<void>(F_gxRestoreClip, c);
}
PORT_FN(0x0047d4c0, "Input::Draw", Input_Draw_n, fp_draw)

// Input::UpdateTable (never called): each line's start as the text wraps at the field's width (at the last space, or
// at the character that doesn't fit), measured a character at a time in the field's style; the rest of the lines' starts
// (to `lines`) at the end
static void __fastcall Input_UpdateTable_n(Input* self, Edx) {
    char one[4];
    volatile char* vone = one;
    const int32_t len = (int32_t)crt_strlen(self->buf);
    if (len < self->cursor) self->cursor = (int32_t)crt_strlen(self->buf);
    int32_t k = 0, i = 0;
    volatile int32_t* entry = self->line_start;
    vone[2] = 0;
    vone[3] = 0;
    int32_t width = 0;
    *entry = 0;
    const volatile char* p = self->buf;
    int32_t space = -1;
    if (*p != 0) {
        do {
            vone[2] = *p;
            width += ccall<int32_t>(F_UIStyleWidth, (int32_t)self->style, (const char*)&one[2]);
            if (*p == ' ') space = i;
            else if (!((int32_t)(self->x1 - self->x0) >= width)) {
                if (space >= 0) {
                    i = space + 1;
                    p = self->buf + i;
                    vone[2] = *p;
                }
                k++;
                const int32_t w1 = ccall<int32_t>(F_UIStyleWidth, (int32_t)self->style, (const char*)&one[2]);
                entry++;
                space = -1;
                width = w1;
                *entry = i;
            }
            p++;
            i++;
        } while (*p != 0);
    }
    volatile int32_t* e = &self->line_start[k + 1];
    k++;
    *e = i;
    if (self->lines >= k) {
        do {
            *e = i;
            e++;
            k++;
        } while (self->lines >= k);
    }
}
PORT_FN(0x0047d670, "Input::UpdateTable", Input_UpdateTable_n, fp_self0)

// Input::GetLine (never called): line `row` copied out (strncpy, then a terminator); an empty string and a report for a
// bad row
static void __fastcall Input_GetLine_n(Input* self, Edx, int32_t row, char* out) {
    if (row >= 0 && self->lines > row) {
        const int32_t s = self->line_start[row];
        ccall<char*>(F_strncpy, out, (const char*)(self->buf + s), (uint32_t)(self->line_start[row + 1] - s));
        const int32_t n = self->line_start[row + 1] - self->line_start[row];
        *(volatile char*)(out + n) = 0;
        return;
    }
    *(volatile char*)out = 0;
    UI_LogReport((const char*)0x004f7178, row);
}
static void fp_Input_GetLine(Footprint& f, Input* self, Edx, int32_t row, char* out) {
    if (row >= 0 && self->lines > row && row < 0x1ff) {
        const int32_t n = self->line_start[row + 1] - self->line_start[row];
        if (n >= 0 && n < 0x100000) { f.add(out, (uint32_t)n + 1, "the line"); return; }
    }
    f.add(out, 1, "the line");
}
PORT_FN(0x0047d790, "Input::GetLine", Input_GetLine_n, fp_Input_GetLine)

static void __fastcall Input_MouseDown_n(Input* self, Edx, int32_t x, int32_t y) {
    const int32_t dy = y - self->y0;
    const int32_t dx = x - self->x0;
    self->cursor = ccall<int32_t>(F_UIStylePointToIndex, (int32_t)self->style, (const char*)self->buf, dx, dy);
    dirty_if_visible(self);
}
PORT_FN(0x0047d800, "Input::MouseDown", Input_MouseDown_n, fp_self_xy)

static void __fastcall Input_Callback_n(Input* self, Edx, int32_t, const void*) {
    dirty_if_visible(self);
    vcall<uint8_t>(self, 0x44, (uint32_t)0);
}
PORT_FN(0x0047d840, "Input::Callback", Input_Callback_n, fp_reach_cb)

// Input::CharHit (while focused): End 0x123, Home 0x124, Left 0x125, Right 0x127, Delete 0x12e, Backspace 8; other
// keys below 0x100 from space up are inserted at the caret while the text is shorter than width * lines - 1; the rest go
// to Widget::CharHit
static uint8_t __fastcall Input_CharHit_n(Input* self, Edx, uint16_t key) {
    if ((int32_t)crt_strlen(self->buf) < self->cursor) self->cursor = (int32_t)crt_strlen(self->buf);
    const uint32_t max = (uint32_t)self->width * (uint32_t)self->lines;
    if (self->focus == 0) return 0;
    const uint8_t vis = self->visible;
    if (vis != 0) self->dirty = 1;
    const uint32_t k = key;
    if (k > 0x123) {
        switch (k) {
        case 0x124:                                              // Home
            self->cursor = 0;
            if (vis != 0) self->dirty = 1;
            return 1;
        case 0x125: {                                            // Left
            const int32_t c = self->cursor;
            if (c <= 0) return 1;
            self->cursor = c - 1;
            if (vis != 0) self->dirty = 1;
            return 1;
        }
        case 0x127:                                              // Right
            if (!(crt_strlen(self->buf) > (uint32_t)self->cursor)) return 1;
            self->cursor = self->cursor + 1;
            dirty_if_visible(self);
            return 1;
        case 0x12e:                                              // Delete
            if (crt_strlen(self->buf) != 0 && crt_strlen(self->buf) > (uint32_t)self->cursor) {
                int32_t c = self->cursor;
                if (self->buf[c] != 0) {
                    do {
                        volatile char* q = self->buf + c;
                        c++;
                        q[0] = q[1];
                    } while (self->buf[c] != 0);
                }
            }
            dirty_if_visible(self);
            return 1;
        default: break;
        }
    } else if (k == 0x123) {                                     // End
        const int32_t n = (int32_t)crt_strlen(self->buf);
        self->cursor = n;
        dirty_if_visible(self);
        return 1;
    } else if (k == 8) {                                         // Backspace
        if (crt_strlen(self->buf) != 0) {
            int32_t c = self->cursor;
            if (c > 0) {
                c--;
                self->cursor = c;
                if (self->buf[c] != 0) {
                    do {
                        volatile char* q = self->buf + c;
                        c++;
                        q[0] = q[1];
                    } while (self->buf[c] != 0);
                }
            }
        }
        dirty_if_visible(self);
        return 1;
    }
    // printable
    if ((key & 0x100) || key <= 0x1f) return tcall<uint8_t>(F_Widget_CharHit, self, key);
    if (crt_strlen(self->buf) < max - 1) {
        int32_t c = (int32_t)crt_strlen(self->buf) + 1;
        if (self->cursor < c) {
            do {
                volatile char* b = self->buf;
                c--;
                b[c + 1] = b[c];
            } while (self->cursor < c);
        }
        self->buf[self->cursor] = (char)(uint8_t)key;
        self->cursor = self->cursor + 1;
    }
    dirty_if_visible(self);
    return 1;
}
PORT_FN(0x0047d860, "Input::CharHit", Input_CharHit_n, fp_reach_key)

static void __fastcall Input_Over_n(Input* self, Edx) {
    dirty_if_visible(self);
    self->over = 1;
}
PORT_FN(0x0047ef60, "Input::Over", Input_Over_n, fp_self0)
static void __fastcall Input_NotOver_n(Input* self, Edx) {
    dirty_if_visible(self);
    self->over = 0;
}
PORT_FN(0x0047ef80, "Input::NotOver", Input_NotOver_n, fp_self0)

static void* __fastcall Input_GetCursor_n(Input* self, Edx) { return self->caret; }
PORT_FN(0x0047efa0, "Input::GetCursor", Input_GetCursor_n, fp_none0)

static CDetect* __fastcall CDetect_ctor_n(CDetect* self, Edx, int32_t x, int32_t y, int32_t w, int32_t h, const char* prompt, void* control,
                                          int32_t style) {
    tcall<Input*>(F_Input_ctor, self, x, y, w, h, (char*)self->text, (int32_t)0x18, (int32_t)1, style, (uint32_t)1);
    self->vtbl = (const void*)(uintptr_t)VT_CDetect;
    self->prompt = prompt;
    self->control = control;
    ccall<void>(F_ControlToDisplayString, (char*)self->text, (int32_t)0x18, control);
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)control, (uint32_t)8);
    return self;
}
static void fp_CDetect_ctor(Footprint& f, CDetect*, Edx, int32_t, int32_t, int32_t, int32_t, const char*, void*, int32_t) {
    f.replay_only = "loads stamps, adds notifications (allocates)";
}
PORT_FN(0x0047db50, "CDetect::CDetect", CDetect_ctor_n, fp_CDetect_ctor)

static void __fastcall CDetect_Callback_n(CDetect* self, Edx, int32_t, const void*) {
    ccall<void>(F_ControlToDisplayString, (char*)self->text, (int32_t)0x18, (void*)self->control);
    dirty_if_visible(self);
}
PORT_FN(0x0047dbc0, "CDetect::Callback", CDetect_Callback_n, fp_self_cb)

// CDetect::MouseUp: the detect dialog (modal: cdetect_idle waits for a control); OK copies the new binding's text
static void __fastcall CDetect_MouseUp_n(CDetect* self, Edx, int32_t, int32_t) {
    if (!(UI_G8(S_CD_ONCE) & 1)) {
        UI_G8(S_CD_ONCE) = UI_G8(S_CD_ONCE) | 1;
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x00579050, (const char*)0x004f718c);
        ccall<int>(F_atexit, (uint32_t)F_cdetect_atexit);
    }
    ccall<void>(F_ControlDetectInit);
    crt_strcpy((char*)(uintptr_t)S_CD_TEXT, self->buf);
    UI_GP(void, S_CD_CONTROL) = self->control;
    const char* title = xlate(0x00579050);
    if (ccall<uint8_t>(F_UIDoOkCancelBox3, title, (const char*)self->prompt, (uint32_t)F_cdetect_idle))
        ccall<char*>(F_strncpy, (char*)self->buf, (const char*)(uintptr_t)S_CD_TEXT, (uint32_t)((uint32_t)self->width * (uint32_t)self->lines));
    tcall<void>(F_Widget_TabOff, self);
}
static void fp_CDetect_MouseUp(Footprint& f, CDetect*, Edx, int32_t, int32_t) { f.replay_only = "runs the detect dialog (modal)"; }
PORT_FN(0x0047dbf0, "CDetect::MouseUp", CDetect_MouseUp_n, fp_CDetect_MouseUp)

// cdetect_idle: the controls polled; one pressed ends the dialog (-2, OK) with its name
static uint8_t __cdecl cdetect_idle_n(int32_t* code) {
    ccall<void>(F_ControlUpdate, (uint32_t)1);
    if (!ccall<uint8_t>(F_ControlDetect, UI_GP(void, S_CD_CONTROL))) return 0;
    ccall<void>(F_ControlToDisplayString, (char*)(uintptr_t)S_CD_TEXT, (int32_t)0x18, UI_GP(void, S_CD_CONTROL));
    ccall<void>(F_KeyClear);
    *(volatile int32_t*)code = -2;
    return 1;
}
static void fp_cdetect_idle(Footprint& f, int32_t*) { f.replay_only = "reads the controls (keys, joystick)"; }
PORT_FN(0x0047dcc0, "cdetect_idle", cdetect_idle_n, fp_cdetect_idle)

static uint8_t __fastcall CDetect_CharHit_n(CDetect* self, Edx, uint16_t key) { return tcall<uint8_t>(F_Widget_CharHit, self, key); }
PORT_FN(0x0047efd0, "CDetect::CharHit", CDetect_CharHit_n, fp_none_key)

// ---- Slider -----------------------------------------------------------------------------------------------------------
static Slider* __fastcall Slider_ctor_n(Slider* self, Edx, int32_t x, int32_t y, int32_t w, int32_t h, float* value, uint32_t lo, uint32_t hi,
                                        int32_t steps) {
    int32_t hx, hy;
    tcall<Widget*>(F_Widget_ctor, self, x, y, x + w, y + h);
    self->vtbl = (const void*)(uintptr_t)VT_Slider;
    self->value = value;
    *(volatile uint32_t*)&self->lo = lo;
    *(volatile uint32_t*)&self->hi = hi;
    self->dragging = 0;
    self->steps = iabs(steps);
    self->neg = steps < 0 ? 1 : 0;
    self->knob = ccall<void*>(F_gxGetStamp, (const char*)0x004f71a8);
    const int32_t y0 = self->y0;
    const int32_t x1 = self->x1;
    const int32_t x0 = self->x0;
    self->ty = y0;
    self->tx = x0;
    self->tw = x1 - x0;
    self->th = self->y1 - y0;
    ccall<void>(F_gxStampHotSpot, (void*)self->knob, &hx, &hy);
    self->tx = self->tx + hx;
    self->ty = self->ty + hy;
    const int32_t kh = ccall<int32_t>(F_gxStampHeight, (void*)self->knob);
    int32_t nh = self->y1 - self->y0 + hy;
    if (!(nh > kh)) nh = kh;
    tcall<void>(F_Widget_Resize, self, (int32_t)(hx * 2 - self->x0 + self->x1 - 1), nh);
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)value, (uint32_t)4);
    return self;
}
static void fp_Slider_ctor(Footprint& f, Slider*, Edx, int32_t, int32_t, int32_t, int32_t, float*, uint32_t, uint32_t, int32_t) {
    f.replay_only = "loads a stamp, adds a notification (allocates)";
}
PORT_FN(0x0047dd20, "Slider::Slider", Slider_ctor_n, fp_Slider_ctor)

// Slider::MouseMove while dragging (near the track): the value at the mouse, snapped to a step (+0.005), in lo..hi
static void __fastcall Slider_MouseMove_n(Slider* self, Edx, int32_t x, int32_t y) {
    if (self->dragging == 0) return;
    if (!tcall<uint8_t>(F_Slider_is_hot_area, self, x, y)) return;
    const double range = D(self->hi) - D(self->lo);
    const int32_t tw = self->tw;
    const int32_t steps = self->steps;
    const int32_t dx = x - self->tx;
    const double sm1 = D(steps - 1);
    const int32_t span = tw - tw / steps;
    double t = D(dx) / D(span);
    t = t * range;
    t = t / range;
    t = t * sm1;
    t = t + D(bits_f(0x3ba3d70a));                         // 0.005f
    const int32_t k = x87_ftol(t);
    double v = D(k) / sm1;                                  // fidivr
    v = range * v;
    v = v + D(self->lo);
    volatile float out = (float)v;
    if (!(v >= D(self->lo))) copy_f(&out, &self->lo);
    if (!(D(self->hi) >= D(out))) copy_f(&out, &self->hi);
    copy_f(self->value, &out);
}
static void fp_Slider_MouseMove(Footprint& f, Slider* self, Edx, int32_t, int32_t) { f.add(self->value, 4, "slider value"); }
PORT_FN(0x0047de40, "Slider::MouseMove", Slider_MouseMove_n, fp_Slider_MouseMove)

static uint8_t __fastcall Slider_is_hot_area_n(Slider* self, Edx, int32_t x, int32_t y) {
    const int32_t tx = self->tx;
    if (tx - 0x14 > x) return 0;
    if (self->tw + tx + 0x14 <= x) return 0;
    const int32_t ty = self->ty;
    if (ty - 0x14 > y) return 0;
    if (self->th + ty + 0x14 <= y) return 0;
    return 1;
}
static void fp_Slider_is_hot_area(Footprint& f, Slider*, Edx, int32_t, int32_t) { f.pure = true; }
PORT_FN(0x0047df60, "Slider::is_hot_area", Slider_is_hot_area_n, fp_Slider_is_hot_area)

static uint8_t __fastcall Slider_CharHit_n(Slider* self, Edx, uint16_t key) { return tcall<uint8_t>(F_Widget_CharHit, self, key); }
PORT_FN(0x0047dfb0, "Slider::CharHit", Slider_CharHit_n, fp_none_key)

// Slider::Draw: the knob stamp as a track (its left and right thirds), then a bar per step, lit up to the value
static void __fastcall Slider_Draw_n(Slider* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    tcall<void>(F_Widget_Draw, self, c);
    const uint32_t lit = UI_GU32(S_C_SLIDER);
    int32_t n = self->steps;
    const uint32_t dim = 0x294514a5u;
    const uint8_t neg = self->neg;
    if (neg) n--;
    volatile float off = neg ? bits_f(0x3f000000) : bits_f(0xbf000000);
    {
        const int32_t x0 = self->x0;
        ccall<void>(F_gxSetClip, c, x0, (int32_t)self->y0, (int32_t)((self->x1 - x0) * 2) / 3 + x0, (int32_t)self->y1);
    }
    ccall<void>(F_gxDrawStamp, (void*)self->knob, (int32_t)self->tx, (int32_t)self->ty, (int32_t)0, (void*)0);
    ccall<void>(F_gxRestoreClip, c);
    {
        const int32_t x0 = self->x0;
        ccall<void>(F_gxSetClip, c, (int32_t)(self->x1 - x0) / 3 + x0, (int32_t)self->y0, (int32_t)self->x1, (int32_t)self->y1);
    }
    {
        const int32_t ty = self->ty, x1 = self->x1;
        const int32_t sw = ccall<int32_t>(F_gxStampWidth, (void*)self->knob);
        ccall<void>(F_gxDrawStamp, (void*)self->knob, x1 - sw - (int32_t)self->x0 + (int32_t)self->tx - 2, ty, (int32_t)0, (void*)0);
    }
    ccall<void>(F_gxRestoreClip, c);
    if (n <= 0) return;
    int32_t i = 0;
    do {
        const int32_t sm1 = self->steps - 1;
        double t = D(i) + D(off);
        const double range = D(self->hi) - D(self->lo);
        t = t * range;
        t = t / D(sm1);
        t = t + D(self->lo);
        const uint32_t col = t > D(*(volatile float*)self->value) ? dim : lit;
        const int32_t tw = self->tw;
        const int32_t j = i + 1;
        const int32_t yb = self->th + self->ty;
        const int32_t x1 = self->tx + (int32_t)((uint32_t)tw * (uint32_t)j) / n - 1;
        const int32_t y0 = self->ty;
        const int32_t x0 = self->tx + (int32_t)((uint32_t)tw * (uint32_t)i) / n;
        ccall<void>(F_gxRect, x0, y0, x1, yb, col);
        i = j;
    } while (i < n);
}
PORT_FN(0x0047dfc0, "Slider::Draw", Slider_Draw_n, fp_draw)

static void __fastcall Slider_MouseDown_n(Slider* self, Edx, int32_t x, int32_t y) {
    self->dragging = 1;
    vcall<void>(self, 0x34, x, y);
}
static void fp_Slider_MouseDown(Footprint& f, Slider* self, Edx, int32_t, int32_t) { f.add(self, sizeof(Slider), "this"); f.add(self->value, 4, "slider value"); }
PORT_FN(0x0047f010, "Slider::MouseDown", Slider_MouseDown_n, fp_Slider_MouseDown)

static void __fastcall Slider_MouseUp_n(Slider* self, Edx, int32_t, int32_t) { self->dragging = 0; }
PORT_FN(0x0047f030, "Slider::MouseUp", Slider_MouseUp_n, fp_self_xy)

static void __fastcall Slider_Over_n(Slider* self, Edx) {
    dirty_if_visible(self);
    self->over = 1;
}
PORT_FN(0x0047f040, "Slider::Over", Slider_Over_n, fp_self0)
static void __fastcall Slider_NotOver_n(Slider* self, Edx) {
    dirty_if_visible(self);
    self->over = 0;
}
PORT_FN(0x0047f060, "Slider::NotOver", Slider_NotOver_n, fp_self0)

static void* __fastcall Slider_ddtor_n(Slider* self, Edx, uint32_t flags) { return stamp_dtor(self, VT_Slider, self->knob, flags); }
PORT_FN(0x0047f080, "Slider::vector deleting destructor", Slider_ddtor_n, fp_dtor_flags)

// ---- Decal ------------------------------------------------------------------------------------------------------------
static void __fastcall Decal_Draw_n(Decal* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    ccall<void>(F_gxPaste, (void*)self->canvas, (int32_t)self->x0, (int32_t)self->y0);
}
static void fp_Decal_Draw(Footprint& f, Decal*, Edx, gxCanvas* c) {
    UI_FP_CANVAS(f, c);
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
}
PORT_FN(0x0047e1a0, "Decal::Draw", Decal_Draw_n, fp_Decal_Draw)

// ---- ScrollBar --------------------------------------------------------------------------------------------------------
static ScrollBar* __fastcall ScrollBar_ctor_n(ScrollBar* self, Edx, int32_t x, int32_t y, int32_t w, int32_t h, UIScrollAxis* axis, int32_t type) {
    int32_t hx, hy;
    tcall<Widget*>(F_Widget_ctor, self, x, y, x + w, y + h);
    self->vtbl = (const void*)(uintptr_t)VT_ScrollBar;
    self->stamp = ccall<void*>(F_gxGetStamp, type == 0 ? (const char*)0x004f71b4 : (const char*)0x004f71c0);
    ccall<void>(F_gxStampHotSpot, (void*)self->stamp, &hx, &hy);
    const int32_t ny = self->y0 + hy;
    tcall<void>(F_Widget_Move, self, (int32_t)(self->x0 + hx), ny);
    if (type != 0) {
        const int32_t sh = ccall<int32_t>(F_gxStampHeight, (void*)self->stamp);
        const int32_t nh = sh - hy * 2;
        tcall<void>(F_Widget_Resize, self, (int32_t)(self->x1 - hx * 2 - self->x0), nh);
    } else {
        const int32_t nh = self->y1 - hy * 2 - self->y0;
        const int32_t sw = ccall<int32_t>(F_gxStampWidth, (void*)self->stamp);
        tcall<void>(F_Widget_Resize, self, sw - hx * 2, nh);
    }
    self->axis = axis;
    self->type = type;
    tcall<void>(F_Widget_AddNotification, self, (int32_t)0, (const void*)axis, (uint32_t)0xc);
    self->dragging = 0;
    return self;
}
static void fp_ScrollBar_ctor(Footprint& f, ScrollBar*, Edx, int32_t, int32_t, int32_t, int32_t, UIScrollAxis*, int32_t) {
    f.replay_only = "loads a stamp, adds a notification (allocates)";
}
PORT_FN(0x0047e1d0, "ScrollBar::ScrollBar", ScrollBar_ctor_n, fp_ScrollBar_ctor)

// ScrollBar::MouseDown: dragging; where the thumb's top was, minus the mouse
static void __fastcall ScrollBar_MouseDown_n(ScrollBar* self, Edx, int32_t x, int32_t y) {
    self->dragging = 1;
    UIScrollAxis* a = self->axis;
    if (self->type == 0) {
        double t = D((int32_t)(self->y1 - self->y0)) / D(a->total);
        t = t * D(a->pos);
        self->grab_y = x87_ftol(t) - y;
        return;
    }
    double t = D((int32_t)(self->x1 - self->x0)) / D(a->total);
    t = t * D(a->pos);
    self->grab_x = x87_ftol(t) - x;
}
PORT_FN(0x0047e2d0, "ScrollBar::MouseDown", ScrollBar_MouseDown_n, fp_self_xy)

// ScrollBar::MouseMove while dragging: the axis's position from the mouse, kept in 0..total - visible
static void __fastcall ScrollBar_MouseMove_n(ScrollBar* self, Edx, int32_t x, int32_t y) {
    if (self->dragging == 0) return;
    UIScrollAxis* a = self->axis;
    const double total = D(a->total);
    int32_t p;
    if (self->type == 0) {
        const int32_t h = self->y1 - self->y0;
        const int32_t m = self->grab_y + y;
        p = x87_ftol(total / D(h) * D(m));
    } else {
        const int32_t w = self->x1 - self->x0;
        const int32_t m = self->grab_x + x;
        p = x87_ftol(total / D(w) * D(m));
    }
    a->pos = p;
    const int32_t lim = a->total - a->visible;
    if (!(lim >= p)) a->pos = lim;
    if (a->pos < 0) a->pos = 0;
}
static void fp_ScrollBar_MouseMove(Footprint& f, ScrollBar* self, Edx, int32_t, int32_t) { f.add(self->axis, sizeof(UIScrollAxis), "the axis"); }
PORT_FN(0x0047e380, "ScrollBar::MouseMove", ScrollBar_MouseMove_n, fp_ScrollBar_MouseMove)

static void __fastcall ScrollBar_MouseUp_n(ScrollBar* self, Edx, int32_t, int32_t) { self->dragging = 0; }
PORT_FN(0x0047e460, "ScrollBar::MouseUp", ScrollBar_MouseUp_n, fp_self_xy)

// ScrollBar::Draw: the bar's stamp as a track (two halves, clipped), then (while the list doesn't all fit) the thumb: a
// rectangle with a light and a dark edge each side
static void __fastcall ScrollBar_Draw_n(ScrollBar* self, Edx, gxCanvas* c) {
    int32_t hx, hy;
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    ccall<void>(F_gxStampHotSpot, (void*)self->stamp, &hx, &hy);
    if (self->type == 0) {
        {
            const int32_t y0 = self->y0;
            const int32_t mid = (int32_t)(hy * 2 - y0 + self->y1) / 2 + y0;
            ccall<void>(F_gxSetClip, c, (int32_t)self->x0 - hx, y0 - hy, (int32_t)(hx * 2 + self->x1), mid);
        }
        ccall<void>(F_gxDrawStamp, (void*)self->stamp, (int32_t)self->x0, (int32_t)self->y0, (int32_t)0, (void*)0);
        {
            const int32_t hy2 = hy * 2;
            const int32_t y1 = self->y1, y0 = self->y0;
            const int32_t mid = (int32_t)(y1 - y0 + hy2) / 2 - hy + y0;
            ccall<void>(F_gxSetClip, c, (int32_t)self->x0 - hx, mid, (int32_t)(hx * 2 + self->x1), hy2 + y1);
        }
        void* st = self->stamp;
        const int32_t sh = ccall<int32_t>(F_gxStampHeight, st);
        ccall<void>(F_gxDrawStamp, st, (int32_t)self->x0, (int32_t)(hy * 2 - sh + self->y1), (int32_t)0, (void*)0);
    } else {
        {
            const int32_t x0 = self->x0;
            const int32_t mid = (int32_t)(hx * 2 - x0 + self->x1) / 2 + x0;
            ccall<void>(F_gxSetClip, c, x0 - hx, (int32_t)self->y0 - hy, mid, (int32_t)(hy * 2 + self->y1));
        }
        ccall<void>(F_gxDrawStamp, (void*)self->stamp, (int32_t)self->x0, (int32_t)self->y0, (int32_t)0, (void*)0);
        {
            const int32_t hy2 = hy * 2;
            const int32_t x1 = self->x1, x0 = self->x0;
            const int32_t yb = self->y1 + hy2;
            const int32_t mid = (int32_t)(x1 - x0 + hy2) / 2 - hx + x0;     // (the original adds 2 hy here)
            ccall<void>(F_gxSetClip, c, mid, (int32_t)self->y0 - hy, x1 + hx * 2, yb);
        }
        void* st = self->stamp;
        const int32_t y0 = self->y0;
        const int32_t sw = ccall<int32_t>(F_gxStampWidth, st);
        ccall<void>(F_gxDrawStamp, st, (int32_t)(hx * 2 - sw + self->x1), y0, (int32_t)0, (void*)0);
    }
    ccall<void>(F_gxRestoreClip, c);
    UIScrollAxis* a = self->axis;
    const int32_t vis = a->visible;
    const int32_t total = a->total;
    if (vis >= total) return;
    if (self->type == 0) {
        const int32_t left = self->x0 + 1;
        int32_t right = self->x1;
        const double sc = D((int32_t)(self->y1 - self->y0)) / D(a->total);
        const int32_t k1 = x87_ftol(D(a->pos) * sc);
        right--;
        const int32_t top = self->y0 + k1 + 1;
        const int32_t k2 = x87_ftol(sc * D(vis + a->pos));
        const int32_t bottom = self->y0 + k2 - 1;
        ccall<void>(F_gxRect, left, top, right, bottom, UI_GU32(S_C_HILITE));
        ccall<void>(F_gxRect, left, bottom - 1, right, bottom, UI_GU32(S_C_BAR_LIGHT));
        ccall<void>(F_gxRect, right - 1, top, right, bottom, UI_GU32(S_C_BAR_LIGHT));
        ccall<void>(F_gxRect, left, top, right, top + 1, UI_GU32(S_C_BAR_DARK));
        ccall<void>(F_gxRect, left, top, left + 1, bottom, UI_GU32(S_C_BAR_DARK));
        return;
    }
    const int32_t top = self->y0 + 1;
    int32_t bottom = self->y1;
    const double sc = D((int32_t)(self->x1 - self->x0)) / D(total);
    bottom--;
    const int32_t k1 = x87_ftol(D(a->pos) * sc);
    const int32_t left = self->x0 + k1 + 1;
    const int32_t k2 = x87_ftol(sc * D(a->pos + vis));
    const int32_t right = self->x0 + k2 - 1;
    ccall<void>(F_gxRect, left, top, right, bottom, UI_GU32(S_C_HILITE));
    ccall<void>(F_gxRect, left, bottom - 1, right, bottom, UI_GU32(S_C_BAR_LIGHT));
    ccall<void>(F_gxRect, right - 1, top, right, bottom, UI_GU32(S_C_BAR_LIGHT));
    ccall<void>(F_gxRect, left, top, right, top + 1, UI_GU32(S_C_BAR_DARK));
    ccall<void>(F_gxRect, left, top, left + 1, bottom, UI_GU32(S_C_BAR_DARK));
}
PORT_FN(0x0047e470, "ScrollBar::Draw", ScrollBar_Draw_n, fp_draw)

static void* __fastcall ScrollBar_ddtor_n(ScrollBar* self, Edx, uint32_t flags) { return stamp_dtor(self, VT_ScrollBar, self->stamp, flags); }
PORT_FN(0x0047f0d0, "ScrollBar::scalar deleting destructor", ScrollBar_ddtor_n, fp_dtor_flags)

// ---- UICustomControl's helpers (on its CustomWidget) ------------------------------------------------------------------------
static void __fastcall UICustomControl_Dirty_n(UICustomControl* self, Edx) {
    Widget* w = (Widget*)self->widget;
    if (w && w->visible != 0) w->dirty = 1;
}
static void fp_UICustomControl_Dirty(Footprint& f, UICustomControl* self, Edx) { if (self->widget) f.add(self->widget, sizeof(CustomWidget), "its widget"); }
PORT_FN(0x0047e820, "UICustomControl::Dirty", UICustomControl_Dirty_n, fp_UICustomControl_Dirty)

static void __fastcall UICustomControl_AddNotification_n(UICustomControl* self, Edx, int32_t id, const void* p, uint32_t size) {
    tcall<void>(F_Widget_AddNotification, (void*)self->widget, id, p, size);
}
static void fp_UICustomControl_AddNotification(Footprint& f, UICustomControl*, Edx, int32_t, const void*, uint32_t) { f.replay_only = "allocates the value's copy"; }
PORT_FN(0x0047e840, "UICustomControl::AddNotification", UICustomControl_AddNotification_n, fp_UICustomControl_AddNotification)

static void __fastcall UICustomControl_RemoveNotification_n(UICustomControl* self, Edx, const void* p) {
    tcall<void>(F_Widget_RemoveNotification, (void*)self->widget, p);
}
static void fp_UICustomControl_RemoveNotification(Footprint& f, UICustomControl*, Edx, const void*) { f.replay_only = "frees the value's copy"; }
PORT_FN(0x0047e860, "UICustomControl::RemoveNotification", UICustomControl_RemoveNotification_n, fp_UICustomControl_RemoveNotification)

static void __fastcall UICustomControl_AddItems_n(UICustomControl* self, Edx, UIDialogItem* items) {
    ccall<void>(F_UIAddItems, (void*)self->widget->window, items);
}
static void fp_UICustomControl_AddItems(Footprint& f, UICustomControl*, Edx, UIDialogItem*) { f.replay_only = "creates widgets (allocates)"; }
PORT_FN(0x0047e870, "UICustomControl::AddItems", UICustomControl_AddItems_n, fp_UICustomControl_AddItems)

#undef D
}  // namespace ui_widget
}  // namespace
