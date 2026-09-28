// ui_window.cpp -- M3 UI stage, step U1: widget.obj, the UI's windows, rewritten faithfully (library `ui`).
//
//   WidgetBegin / End (the window table, the mouse cursor stamp), WidgetCreateWindow / DestroyWindow (16 windows),
//   WidgetExecuteWindow (a window run modally: WidgetUpdate until WidgetExit) and WidgetUpdate (one frame: the mouse
//   event and the key to the running window, its widgets' Update, the 3D windows' Draw3D, every window drawn on the
//   grabbed screen, the cursor, the flip), the C wrappers (groups, items, default, idle function), and WidgetWindow:
//   its canvas, the widget list (256, no bound), the mouse (capture on a press, over / not over), the keyboard (the
//   focused widget, then every visible enabled one: hot keys), the focus (Tab order: IsTabStop), groups (32 bits:
//   hidden, disabled), Draw (dirty widgets' rectangles cleared to the background, four passes spreading the redraw to
//   the widgets they overlap, then each drawn clipped to itself, and the canvas pasted onto the screen), and
//   Widget::TabNext / TabOff / TabPrev / TabCycle, TitleBar::MouseDown / MouseMove (dragging the running window)
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this library's own too),
// virtual calls through the widget's vtable; loops re-read the widget count where the original does. The window
// layout is in ui_types.h.
//
// Footprints (main thread; every static written is listed). What a window does to its widgets depends on what they
// are: ui_types.h's ui_fp_reach lists what a widget's handlers can write, and a window function whose widgets include
// one that can run unbounded code (a CustomWidget, a button with a callback, CDetect's dialog, an unknown class) is
// replay_only for that call. Always replay_only: WidgetUpdate (reads the input queues, flips the screen),
// WidgetExecuteWindow (runs a window modally), and what allocates or frees (Begin / End, window creation and
// destruction, the constructor and destructor).
//
// FIX CANDIDATES (left as the original has them):
//   * WidgetWindow::AddWidget has no bound: the 257th widget's pointer is written over the count (+0x428) and past.
//   * WidgetWindow::Draw keeps a 256-byte "redraw" flag per widget on its stack: more widgets write past it.
//   * WidgetCreateWindow: with 16 windows open it panics (LogPanic) after constructing the 17th, which leaks.
//   * WidgetWindow::PrevWidget doesn't check find_widget_index's -1 (harmless: the loop starts below 0).
//   * TitleBar::MouseMove moves WidgetGetActiveWindow's window, unchecked (null while none runs).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"

namespace {
namespace ui_window {
using namespace uit;

#define ACTIVE UI_GP(WidgetWindow, S_ACTIVE)
static __forceinline WidgetWindow* volatile* windows() { return (WidgetWindow* volatile*)(uintptr_t)S_WINDOWS; }

static void fp_replay_alloc(Footprint& f) { f.replay_only = "allocates or frees (a window, a stamp)"; }

// ---- the C interface ---------------------------------------------------------------------------------------------------
// WidgetBegin: no running window, the window table cleared, the default cursor loaded
static void __cdecl WidgetBegin_n() {
    ACTIVE = 0;
    for (int i = 0; i < 16; i++) windows()[i] = 0;
    UI_GP(void, S_DEFAULT_CURSOR) = ccall<void*>(F_gxGetStamp, (const char*)0x004f7230);
    UI_GP(void, S_CURSOR) = 0;
}
PORT_FN(0x0047fe50, "WidgetBegin", WidgetBegin_n, fp_replay_alloc)

static void __cdecl WidgetEnd_n() { ccall<void>(F_gxForgetStamp, UI_GP(void, S_DEFAULT_CURSOR)); }
PORT_FN(0x0047fe90, "WidgetEnd", WidgetEnd_n, fp_replay_alloc)

// WidgetCreateWindow: a new window in the first free slot (a panic and 0 if all 16 are taken; the window leaks)
static WidgetWindow* __cdecl WidgetCreateWindow_n(const char* bg, int32_t x, int32_t y, int32_t w, int32_t h, int32_t ui3d) {
    void* p = ccall<void*>(F_MemAlloc, 0x464);
    WidgetWindow* win = 0;
    if (p) win = tcall<WidgetWindow*>(F_WidgetWindow_ctor, p, bg, x, y, w, h, ui3d);
    for (int i = 0; i < 16; i++)
        if (windows()[i] == 0) {
            windows()[i] = win;
            return win;
        }
    UI_LogPanic((const char*)0x004f724c);
    return 0;
}
static void fp_WidgetCreateWindow(Footprint& f, const char*, int32_t, int32_t, int32_t, int32_t, int32_t) { f.replay_only = "allocates a window"; }
PORT_FN(0x0047fea0, "WidgetCreateWindow", WidgetCreateWindow_n, fp_WidgetCreateWindow)

// WidgetDestroyWindow: destroyed and deleted, its slot freed (a panic if it isn't in the table)
static void __cdecl WidgetDestroyWindow_n(WidgetWindow* win) {
    for (int i = 0; i < 16; i++)
        if (windows()[i] == win) {
            if (win) {
                tcall<void>(F_WidgetWindow_dtor, win);
                ccall<void>(F_Delete, (void*)win);
            }
            windows()[i] = 0;
            return;
        }
    UI_LogPanic((const char*)0x004f7284);
}
static void fp_WidgetDestroyWindow(Footprint& f, WidgetWindow*) { f.replay_only = "frees a window and its widgets"; }
PORT_FN(0x0047ff10, "WidgetDestroyWindow", WidgetDestroyWindow_n, fp_WidgetDestroyWindow)

static void __cdecl WidgetExit_n(int32_t code) {
    UI_G8(S_EXIT) = 1;
    UI_G32(S_EXIT_CODE) = code;
}
static void fp_WidgetExit(Footprint& f, int32_t) {
    UI_FP(f, S_EXIT, 1, "widget.obj exit flag");
    UI_FP(f, S_EXIT_CODE, 4, "widget.obj exit code");
}
PORT_FN(0x0047ff60, "WidgetExit", WidgetExit_n, fp_WidgetExit)

static void __cdecl WidgetEnterGroup_n(WidgetWindow* win, uint32_t* g) { tcall<void>(F_WW_EnterGroup, win, g); }
static void fp_WidgetEnterGroup(Footprint& f, WidgetWindow* win, uint32_t* g) { f.add(win, sizeof(WidgetWindow), "window"); f.add(g, 4, "group"); }
PORT_FN(0x0047ff80, "WidgetEnterGroup", WidgetEnterGroup_n, fp_WidgetEnterGroup)

static void __cdecl WidgetLeaveGroup_n(WidgetWindow* win, uint32_t* g) { tcall<void>(F_WW_LeaveGroup, win, g); }
static void fp_WidgetLeaveGroup(Footprint& f, WidgetWindow* win, uint32_t*) { f.add(win, sizeof(WidgetWindow), "window"); }
PORT_FN(0x0047ff90, "WidgetLeaveGroup", WidgetLeaveGroup_n, fp_WidgetLeaveGroup)

static void fp_win_groups(Footprint& f, WidgetWindow* win, int32_t) { ui_fp_window_objects(f, win); }
static void __cdecl WidgetHideGroup_n(WidgetWindow* win, int32_t g) { tcall<void>(F_WW_HideGroup, win, g); }
PORT_FN(0x0047ffa0, "WidgetHideGroup", WidgetHideGroup_n, fp_win_groups)
static void __cdecl WidgetShowGroup_n(WidgetWindow* win, int32_t g) { tcall<void>(F_WW_ShowGroup, win, g); }
PORT_FN(0x0047ffb0, "WidgetShowGroup", WidgetShowGroup_n, fp_win_groups)
static void __cdecl WidgetEnableGroup_n(WidgetWindow* win, int32_t g) { tcall<void>(F_WW_EnableGroup, win, g); }
PORT_FN(0x0047ffc0, "WidgetEnableGroup", WidgetEnableGroup_n, fp_win_groups)
static void __cdecl WidgetDisableGroup_n(WidgetWindow* win, int32_t g) { tcall<void>(F_WW_DisableGroup, win, g); }
PORT_FN(0x0047ffd0, "WidgetDisableGroup", WidgetDisableGroup_n, fp_win_groups)

static WidgetWindow* __cdecl WidgetGetActiveWindow_n() { return ACTIVE; }
static void fp_none0(Footprint&) {}
PORT_FN(0x0047ffe0, "WidgetGetActiveWindow", WidgetGetActiveWindow_n, fp_none0)

// adding a widget: IsTabStop, perhaps Focus, and Added -- a CustomWidget's are menu code
static void fp_add_widget(Footprint& f, WidgetWindow* win, Widget* w) {
    if (w && (uint32_t)(uintptr_t)w->vtbl == VT_CustomWidget) { f.replay_only = "a CustomWidget's IsTabStop / Focus / Added (menu code)"; return; }
    if (w && widget_size(w) == 0x228 && (uint32_t)(uintptr_t)w->vtbl != VT_Widget) { f.replay_only = "a widget of a class outside the toolkit"; return; }
    f.add(win, sizeof(WidgetWindow), "window");
    UI_FP_WIDGET(f, w, "the widget");
}
static void __cdecl WidgetAddItem_n(WidgetWindow* win, Widget* w) { tcall<void>(F_WW_AddWidget, win, w); }
PORT_FN(0x0047fff0, "WidgetAddItem", WidgetAddItem_n, fp_add_widget)

static void __cdecl WidgetSetIdleFunction_n(WidgetWindow* win, void(__cdecl* fn)()) { win->idle = fn; }
static void fp_WidgetSetIdleFunction(Footprint& f, WidgetWindow* win, void(__cdecl*)()) { f.add((uint8_t*)win + 0x42c, 4, "window idle function"); }
PORT_FN(0x00480000, "WidgetSetIdleFunction", WidgetSetIdleFunction_n, fp_WidgetSetIdleFunction)

// WidgetUpdate: one frame of the running UI
static void __cdecl WidgetUpdate_n() {
    struct { int32_t type, x, y, _c; } ev;
    gxCanvas screen;
    ccall<void>(F_UIUpdateTime);
    if (ccall<uint8_t>(F_MouseGetEvent, (void*)&ev)) {
        UI_G32(S_MOUSE_X) = ((volatile int32_t*)&ev)[1];
        UI_G32(S_MOUSE_Y) = ((volatile int32_t*)&ev)[2];
        if (ACTIVE != 0) {
            const uint32_t t = (uint32_t)((volatile int32_t*)&ev)[0];
            if (t <= 6) switch (t) {
                case 0: tcall<void>(F_WW_MouseDown, ACTIVE, UI_G32(S_MOUSE_X), UI_G32(S_MOUSE_Y)); break;
                case 1: tcall<void>(F_WW_MouseUp, ACTIVE, UI_G32(S_MOUSE_X), UI_G32(S_MOUSE_Y)); break;
                case 2: tcall<void>(F_WW_MouseRDown, ACTIVE, UI_G32(S_MOUSE_X), UI_G32(S_MOUSE_Y)); break;
                case 3: tcall<void>(F_WW_MouseRUp, ACTIVE, UI_G32(S_MOUSE_X), UI_G32(S_MOUSE_Y)); break;
                case 6: tcall<void>(F_WW_MouseMove, ACTIVE, UI_G32(S_MOUSE_X), UI_G32(S_MOUSE_Y)); break;
                default: break;
                }
        }
    }
    bool update = true;
    if (ccall<uint8_t>(F_KeyHit)) {
        const uint16_t k = ccall<uint16_t>(F_KeyGet);
        if (ACTIVE == 0) update = false;
        else tcall<void>(F_WW_CharHit, ACTIVE, k);
    }
    if (update && ACTIVE != 0) tcall<void>(F_WW_Update, ACTIVE);
    if (UI_G8(S_EXIT) != 0) return;
    for (int i = 0; i < 16; i++) {
        WidgetWindow* w = windows()[i];
        if (w && w->ui3d == 2) {
            ccall<void>(F_mrBeginFrame);
            tcall<void>(F_WW_Draw3D, windows()[i]);
            ccall<void>(F_mrEndFrame);
        }
    }
    if (UI_G8(S_EXIT) != 0) return;
    if (!ccall<uint8_t>(F_gxGrabScreen, &screen)) return;
    for (int i = 0; i < 16; i++) {
        WidgetWindow* w = windows()[i];
        if (w) tcall<void>(F_WW_Draw, w, &screen);
    }
    if (ACTIVE != 0 && ACTIVE->ui3d == 1) {
        ccall<void>(F_gxReleaseScreen);
        ccall<void>(F_mrBeginFrame);
        if (ACTIVE != 0) tcall<void>(F_WW_Draw3D, ACTIVE);
        ccall<void>(F_mrEndFrame);
        if (!ccall<uint8_t>(F_gxGrabScreen, &screen)) return;
        ccall<gxCanvas*>(F_gxSetCanvas, &screen);
    }
    void* cur = UI_GP(void, S_CURSOR) ? UI_GP(void, S_CURSOR) : UI_GP(void, S_DEFAULT_CURSOR);
    {
        const int32_t y = UI_G32(S_MOUSE_Y), x = UI_G32(S_MOUSE_X);
        ccall<void>(F_gxDrawStamp, cur, x, y, (int32_t)0, (void*)0);
    }
    ccall<void>(F_gxReleaseScreen);
    ccall<void>(F_gxFlip);
}
static void fp_WidgetUpdate(Footprint& f) { f.replay_only = "a frame of the running UI: reads the mouse and key queues, locks and flips the screen"; }
PORT_FN(0x00480010, "WidgetUpdate", WidgetUpdate_n, fp_WidgetUpdate)

// WidgetExecuteWindow: win runs until WidgetExit (the previous window runs again after); its exit code
static int32_t __cdecl WidgetExecuteWindow_n(WidgetWindow* win) {
    int32_t state;
    ccall<void>(F_MousePeek, (int32_t*)(uintptr_t)S_MOUSE_X, (int32_t*)(uintptr_t)S_MOUSE_Y, &state);
    WidgetWindow* prev = ACTIVE;
    ACTIVE = win;
    tcall<void>(F_WW_Activate, win);
    {
        const int32_t y = UI_G32(S_MOUSE_Y);
        UI_G8(S_EXIT) = 0;
        const int32_t x = UI_G32(S_MOUSE_X);
        UI_G32(S_EXIT_CODE) = 0;
        WidgetWindow* a = ACTIVE;
        UI_GP(void, S_CURSOR) = 0;
        tcall<void>(F_WW_MouseMove, a, x, y);
    }
    while (UI_G8(S_EXIT) == 0) {
        ccall<void>(F_WidgetUpdate);
        if (ACTIVE != 0) {
            void(__cdecl * idle)() = ACTIVE->idle;
            if (idle) idle();
        }
    }
    UI_G8(S_EXIT) = 0;
    tcall<void>(F_WW_Deactivate, win);
    ACTIVE = prev;
    UI_GP(void, S_CURSOR) = 0;
    if (prev) tcall<void>(F_WW_MouseMove, prev, UI_G32(S_MOUSE_X), UI_G32(S_MOUSE_Y));
    return UI_G32(S_EXIT_CODE);
}
static void fp_WidgetExecuteWindow(Footprint& f, WidgetWindow*) { f.replay_only = "runs the window modally (its own input loop, a frame at a time)"; }
PORT_FN(0x00480260, "WidgetExecuteWindow", WidgetExecuteWindow_n, fp_WidgetExecuteWindow)

static void fp_set_default(Footprint& f, WidgetWindow* win, int32_t) { ui_fp_window_focus(f, win); }
static void __cdecl WidgetSetDefault_n(WidgetWindow* win, int32_t id) { tcall<void>(F_WW_SetDefault, win, id); }
PORT_FN(0x00480330, "WidgetSetDefault", WidgetSetDefault_n, fp_set_default)

// ---- WidgetWindow ------------------------------------------------------------------------------------------------------
static WidgetWindow* __fastcall WidgetWindow_ctor_n(WidgetWindow* self, Edx, const char* bg, int32_t x, int32_t y, int32_t w,
                                                    int32_t h, int32_t ui3d) {
    self->ui3d = ui3d;
    self->x = x;
    self->y = y;
    self->w = w;
    self->h = h;
    self->full_redraw = 1;
    self->count = 0;
    self->idle = 0;
    self->over = 0;
    self->captured = 0;
    self->focus = 0;
    self->group = 0;
    self->next_group = 1;
    self->disabled = 0;
    self->hidden = 0;
    ccall<void>(F_gxAllocCanvas, &self->canvas, w, h);
    if (bg) self->background = ccall<void*>(F_gxGetStamp, bg);
    else self->background = 0;
    return self;
}
static void fp_WidgetWindow_ctor(Footprint& f, WidgetWindow*, Edx, const char*, int32_t, int32_t, int32_t, int32_t, int32_t) {
    f.replay_only = "allocates the window's canvas, loads its background";
}
PORT_FN(0x00480340, "WidgetWindow::WidgetWindow", WidgetWindow_ctor_n, fp_WidgetWindow_ctor)

static void __fastcall WidgetWindow_dtor_n(WidgetWindow* self, Edx) {
    for (int32_t i = 0; i < self->count; i++) {
        Widget* w = self->widgets[i];
        if (w) vcall<void*>(w, 0, (uint32_t)1);
    }
    ccall<void>(F_gxFreeCanvas, &self->canvas);
    void* bg = self->background;
    if (bg) ccall<void>(F_gxForgetStamp, bg);
}
static void fp_WidgetWindow_dtor(Footprint& f, WidgetWindow*, Edx) { f.replay_only = "deletes the widgets, frees the canvas"; }
PORT_FN(0x004803f0, "WidgetWindow::~WidgetWindow", WidgetWindow_dtor_n, fp_WidgetWindow_dtor)

static void fp_win_reach(Footprint& f, WidgetWindow* win, Edx) { ui_fp_window_reach(f, win); }
static void fp_win_reach_xy(Footprint& f, WidgetWindow* win, Edx, int32_t, int32_t) { ui_fp_window_reach(f, win); }
static void fp_win_focus(Footprint& f, WidgetWindow* win, Edx) { ui_fp_window_focus(f, win); }

static void __fastcall WidgetWindow_Activate_n(WidgetWindow* self, Edx) {
    for (int32_t i = 0; i < self->count;) {
        Widget* w = self->widgets[i];
        i++;
        vcall<void>(w, 4);
    }
}
PORT_FN(0x00480440, "WidgetWindow::Activate", WidgetWindow_Activate_n, fp_win_reach)

static void __fastcall WidgetWindow_Deactivate_n(WidgetWindow* self, Edx) {
    for (int32_t i = 0; i < self->count;) {
        Widget* w = self->widgets[i];
        i++;
        vcall<void>(w, 8);
    }
}
PORT_FN(0x00480470, "WidgetWindow::Deactivate", WidgetWindow_Deactivate_n, fp_win_reach)

// a press: the widget under it captures the mouse, takes the focus if it's a tab stop, and gets MouseDown / RDown
static void __forceinline press(WidgetWindow* self, int32_t x, int32_t y, uint32_t slot) {
    if (self->captured != 0) return;
    const int32_t wx = x - self->x;
    const int32_t wy = y - self->y;
    Widget* w = 0;
    const int32_t i = tcall<int32_t>(F_WW_find_widget, self, wx, wy);
    if (i >= 0) w = self->widgets[i];
    self->captured = w;
    if (!w) return;
    if (self->focus != w && vcall<uint8_t>(w, 0x48)) tcall<void>(F_WW_set_focus, self, w);
    if (w) vcall<void>(w, slot, wx, wy);
}
static void __forceinline release(WidgetWindow* self, int32_t x, int32_t y, uint32_t slot) {
    const int32_t wx = x - self->x;
    Widget* c = self->captured;
    const int32_t wy = y - self->y;
    if (c) vcall<void>(c, slot, wx, wy);
    self->captured = 0;
}
static void __fastcall WidgetWindow_MouseDown_n(WidgetWindow* self, Edx, int32_t x, int32_t y) { press(self, x, y, 0x24); }
PORT_FN(0x004804b0, "WidgetWindow::MouseDown", WidgetWindow_MouseDown_n, fp_win_reach_xy)
static void __fastcall WidgetWindow_MouseUp_n(WidgetWindow* self, Edx, int32_t x, int32_t y) { release(self, x, y, 0x28); }
PORT_FN(0x00480520, "WidgetWindow::MouseUp", WidgetWindow_MouseUp_n, fp_win_reach_xy)
static void __fastcall WidgetWindow_MouseRDown_n(WidgetWindow* self, Edx, int32_t x, int32_t y) { press(self, x, y, 0x2c); }
PORT_FN(0x00480560, "WidgetWindow::MouseRDown", WidgetWindow_MouseRDown_n, fp_win_reach_xy)
static void __fastcall WidgetWindow_MouseRUp_n(WidgetWindow* self, Edx, int32_t x, int32_t y) { release(self, x, y, 0x30); }
PORT_FN(0x004805d0, "WidgetWindow::MouseRUp", WidgetWindow_MouseRUp_n, fp_win_reach_xy)

// the mouse moved: Over / NotOver as the widget under it changes (none while another has the mouse captured), then
// MouseMove to the one under it, or else to the captured one
static void __fastcall WidgetWindow_MouseMove_n(WidgetWindow* self, Edx, int32_t x, int32_t y) {
    const int32_t wx = x - self->x;
    const int32_t wy = y - self->y;
    Widget* w = 0;
    const int32_t i = tcall<int32_t>(F_WW_find_widget, self, wx, wy);
    if (i >= 0) w = self->widgets[i];
    Widget* cap = self->captured;
    if (cap != 0 && cap != w) w = 0;
    Widget* over = self->over;
    if (over != w) {
        if (over) vcall<void>(over, 0x3c);
        self->over = w;
        if (w) vcall<void>(w, 0x38);
    }
    if (!w) {
        w = self->captured;
        if (!w) return;
    }
    vcall<void>(w, 0x34, wx, wy);
}
PORT_FN(0x00480610, "WidgetWindow::MouseMove", WidgetWindow_MouseMove_n, fp_win_reach_xy)

// a key: the focused widget first, then every other visible, enabled one until one takes it (none while the mouse is
// captured)
static void __fastcall WidgetWindow_CharHit_n(WidgetWindow* self, Edx, uint16_t key) {
    if (self->captured != 0) return;
    Widget* fw = self->focus;
    if (fw && vcall<uint8_t>(fw, 0x44, key)) return;
    for (int32_t i = 0; i < self->count; i++) {
        Widget* w = self->widgets[i];
        if (self->focus == w) continue;
        if (w->visible == 0 || w->enabled == 0) continue;
        if (vcall<uint8_t>(w, 0x44, key)) return;
    }
}
static void fp_WidgetWindow_CharHit(Footprint& f, WidgetWindow* win, Edx, uint16_t) { ui_fp_window_reach(f, win); }
PORT_FN(0x00480690, "WidgetWindow::CharHit", WidgetWindow_CharHit_n, fp_WidgetWindow_CharHit)

// the next tab stop after the focused widget (none past the last)
static void __fastcall WidgetWindow_NextWidget_n(WidgetWindow* self, Edx) {
    Widget* fw = self->focus;
    if (!fw) return;
    int32_t i = tcall<int32_t>(F_WW_find_widget_index, self, fw);
    if (i < 0) {
        UI_LogReport((const char*)0x004f72a0);
        return;
    }
    for (;;) {
        i++;
        if (!(self->count > i)) return;
        if (vcall<uint8_t>(self->widgets[i], 0x48)) {
            tcall<void>(F_WW_set_focus, self, self->widgets[i]);
            return;
        }
    }
}
PORT_FN(0x00480700, "WidgetWindow::NextWidget", WidgetWindow_NextWidget_n, fp_win_focus)

static void __fastcall WidgetWindow_PrevWidget_n(WidgetWindow* self, Edx) {
    Widget* fw = self->focus;
    if (!fw) return;
    int32_t i = tcall<int32_t>(F_WW_find_widget_index, self, fw);
    for (;;) {
        i--;
        if (i < 0) return;
        if (vcall<uint8_t>(self->widgets[i], 0x48)) {
            tcall<void>(F_WW_set_focus, self, self->widgets[i]);
            return;
        }
    }
}
PORT_FN(0x00480760, "WidgetWindow::PrevWidget", WidgetWindow_PrevWidget_n, fp_win_focus)

static void __fastcall WidgetWindow_set_focus_n(WidgetWindow* self, Edx, Widget* w) {
    Widget* old = self->focus;
    if (w == old) return;
    if (old) vcall<void>(old, 0x20);
    self->focus = w;
    if (w) vcall<void>(w, 0x1c);
}
static void fp_set_focus(Footprint& f, WidgetWindow* win, Edx, Widget* w) {
    if (!ui_fp_window_focus(f, win)) return;
    if (w && (uint32_t)(uintptr_t)w->vtbl == VT_CustomWidget) { f.replay_only = "a CustomWidget's Focus (menu code)"; return; }
    UI_FP_WIDGET(f, w, "the widget");
}
PORT_FN(0x004807a0, "WidgetWindow::set_focus", WidgetWindow_set_focus_n, fp_set_focus)

// the topmost (last added) visible, enabled widget whose HitTest takes (x, y), or -1
static int32_t __fastcall WidgetWindow_find_widget_n(WidgetWindow* self, Edx, int32_t x, int32_t y) {
    for (int32_t i = self->count - 1; i >= 0; i--) {
        Widget* w = self->widgets[i];
        if (w->enabled == 0 || w->visible == 0) continue;
        if (vcall<uint8_t>(w, 0x4c, x, y)) return i;
    }
    return -1;
}
// (HitTest writes nothing: only BButton has its own, and a CustomWidget doesn't forward it)
static void fp_find_widget(Footprint& f, WidgetWindow* win, Edx, int32_t, int32_t) {
    const int32_t n = ui_clamp_count(win->count, 256);
    for (int32_t i = 0; i < n; i++) {
        const Widget* w = win->widgets[i];
        if (w && widget_size(w) == 0x228 && (uint32_t)(uintptr_t)w->vtbl != VT_Widget) {
            f.replay_only = "a widget of a class outside the toolkit";
            return;
        }
    }
}
PORT_FN(0x004807e0, "WidgetWindow::find_widget", WidgetWindow_find_widget_n, fp_find_widget)

static int32_t __fastcall WidgetWindow_find_widget_index_n(WidgetWindow* self, Edx, Widget* w) {
    const int32_t n = self->count;
    for (int32_t i = 0; i < n; i++)
        if (self->widgets[i] == w) return i;
    return -1;
}
static void fp_find_widget_index(Footprint&, WidgetWindow*, Edx, Widget*) {}
PORT_FN(0x00480830, "WidgetWindow::find_widget_index", WidgetWindow_find_widget_index_n, fp_find_widget_index)

// each widget's Update, then the cursor: the captured widget's, else the one under the mouse's, else none
static void __fastcall WidgetWindow_Update_n(WidgetWindow* self, Edx) {
    for (int32_t i = 0; i < self->count;) {
        Widget* w = self->widgets[i];
        i++;
        vcall<void>(w, 0x54);
    }
    Widget* c = self->captured;
    if (c) {
        UI_GP(void, S_CURSOR) = vcall<void*>(c, 0x40);
        return;
    }
    c = self->over;
    if (c) {
        UI_GP(void, S_CURSOR) = vcall<void*>(c, 0x40);
        return;
    }
    UI_GP(void, S_CURSOR) = 0;
}
static void fp_WidgetWindow_Update(Footprint& f, WidgetWindow* win, Edx) {
    if (!ui_fp_window_reach(f, win)) return;
    UI_FP(f, S_CURSOR, 4, "widget.obj cursor");
    UI_FP(f, S_UI_DT, 4, "(read) ui.obj delta t");
}
PORT_FN(0x00480860, "WidgetWindow::Update", WidgetWindow_Update_n, fp_WidgetWindow_Update)

// the window's background where the widget's rectangle is (clipped to it)
static __forceinline void clear_rect(WidgetWindow* self, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    void* bg = self->background;
    if (bg) ccall<void>(F_gxDrawStamp, bg, (int32_t)0, (int32_t)0, (int32_t)0, (void*)0);
    else ccall<void>(F_gxClear, UI_GU32(S_CLEAR_COLOR));
}
// Draw: dirty widgets' rectangles cleared (all of it on a full redraw), four passes marking the visible widgets that
// overlap a marked one, each marked (or, on a full redraw, every) visible widget drawn clipped to itself, and the
// window's canvas pasted onto `to` at the window's place
static void __fastcall WidgetWindow_Draw_n(WidgetWindow* self, Edx, gxCanvas* to) {
    volatile uint8_t mark[0x100];
    gxCanvas* c = &self->canvas;
    if (self->full_redraw != 0) clear_rect(self, c);
    for (int32_t i = 0; i < self->count; i++) {
        Widget* w = self->widgets[i];
        if (tcall<uint8_t>(F_Widget_IsDirty, w) || self->full_redraw != 0) {
            const int32_t x0 = w->x0;
            if ((int32_t)(w->x1 - x0) > 0) {
                const int32_t y1 = w->y1, y0 = w->y0;
                if ((int32_t)(y1 - y0) > 0) {
                    ccall<void>(F_gxSetClip, c, x0, y0, (int32_t)w->x1, y1);
                    clear_rect(self, c);
                    ccall<void>(F_gxRestoreClip, c);
                    mark[i] = 1;
                    continue;
                }
            }
        }
        mark[i] = 0;
    }
    const int32_t n = self->count;
    for (int pass = 4; pass != 0; pass--) {
        for (int32_t i = 0; i < n; i++) {
            Widget* a = self->widgets[i];
            if (mark[i] == 0) continue;
            for (int32_t j = 0; j < n; j++) {
                Widget* b = self->widgets[j];
                if (i == j || mark[j] != 0 || b->visible == 0) continue;
                if (!(b->x0 < a->x1)) continue;
                if (!(a->y1 > b->y0)) continue;
                if (!(a->x0 < b->x1)) continue;
                if (!(a->y0 < b->y1)) continue;
                mark[j] = 1;
            }
        }
    }
    if (n > 0) {
        int32_t i = 0;
        do {
            Widget* w = self->widgets[i];
            if ((mark[i] != 0 || self->full_redraw != 0) && w->visible != 0 && (int32_t)(w->x1 - w->x0) > 0) {
                const int32_t y1 = w->y1, y0 = w->y0;
                if ((int32_t)(y1 - y0) > 0) {
                    ccall<void>(F_gxSetClip, c, (int32_t)w->x0, y0, (int32_t)w->x1, y1);
                    vcall<void>(w, 0x14, c);
                    ccall<void>(F_gxRestoreClip, c);
                }
            }
            i++;
        } while (self->count > i);
    }
    self->full_redraw = 0;
    ccall<gxCanvas*>(F_gxSetCanvas, to);
    ccall<void>(F_gxPaste, c, (int32_t)self->x, (int32_t)self->y);
}
static void fp_WidgetWindow_Draw(Footprint& f, WidgetWindow* win, Edx, gxCanvas* to) {
    if (!ui_fp_window_focus(f, win)) return;
    UI_FP_CANVAS(f, &win->canvas);
    UI_FP_CANVAS(f, to);
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
}
PORT_FN(0x004808d0, "WidgetWindow::Draw", WidgetWindow_Draw_n, fp_WidgetWindow_Draw)

static void __fastcall WidgetWindow_Draw3D_n(WidgetWindow* self, Edx) {
    for (int32_t i = 0; i < self->count; i++) {
        Widget* w = self->widgets[i];
        if (w->visible != 0) vcall<void>(w, 0x18);
    }
}
PORT_FN(0x00480ae0, "WidgetWindow::Draw3D", WidgetWindow_Draw3D_n, fp_win_focus)

// EnterGroup: the next group bit (asserted nonzero: 32 groups at most) given out and entered
static void __fastcall WidgetWindow_EnterGroup_n(WidgetWindow* self, Edx, uint32_t* g) {
    ((Assert_t)(uintptr_t)F_WW_Assert)(self->next_group >= 1u ? 1 : 0, (const char*)0x004f72b8);
    const uint32_t bit = self->next_group;
    *(volatile uint32_t*)g = bit;
    self->next_group = self->next_group << 1;
    self->group = self->group | bit;
}
static void fp_WidgetWindow_EnterGroup(Footprint& f, WidgetWindow* win, Edx, uint32_t* g) {
    f.add(win, sizeof(WidgetWindow), "window");
    f.add(g, 4, "group");
}
PORT_FN(0x00480b20, "WidgetWindow::EnterGroup", WidgetWindow_EnterGroup_n, fp_WidgetWindow_EnterGroup)

static void __fastcall WidgetWindow_LeaveGroup_n(WidgetWindow* self, Edx, uint32_t* g) {
    const uint32_t m = ~*(volatile uint32_t*)g;
    self->group = self->group & m;
}
static void fp_WidgetWindow_LeaveGroup(Footprint& f, WidgetWindow* win, Edx, uint32_t*) { f.add(win, sizeof(WidgetWindow), "window"); }
PORT_FN(0x00480b70, "WidgetWindow::LeaveGroup", WidgetWindow_LeaveGroup_n, fp_WidgetWindow_LeaveGroup)

static void fp_groups(Footprint& f, WidgetWindow* win, Edx, uint32_t) { ui_fp_window_objects(f, win); }

// HideGroup: nothing if any of the bits is already hidden; else each widget in the groups hidden (dirty if it was
// showing)
static void __fastcall WidgetWindow_HideGroup_n(WidgetWindow* self, Edx, uint32_t g) {
    const uint32_t h = self->hidden;
    if (h & g) return;
    self->hidden = g | h;
    for (int32_t i = 0; i < self->count; i++) {
        Widget* w = self->widgets[i];
        const uint32_t m = w->groups;
        if (self->hidden & m) {
            if (w->visible != 0) w->dirty = 1;
            w->visible = 0;
        }
    }
}
PORT_FN(0x00480b90, "WidgetWindow::HideGroup", WidgetWindow_HideGroup_n, fp_groups)

// ShowGroup: nothing unless one of the bits is hidden; else those bits shown and every widget in no hidden group
// shown and dirty
static void __fastcall WidgetWindow_ShowGroup_n(WidgetWindow* self, Edx, uint32_t g) {
    const uint32_t h = self->hidden;
    if (!(h & g)) return;
    self->hidden = ~g & h;
    for (int32_t i = 0; i < self->count; i++) {
        Widget* w = self->widgets[i];
        const uint32_t m = w->groups;
        if (!(self->hidden & m)) {
            w->visible = 1;
            w->dirty = 1;
        }
    }
}
PORT_FN(0x00480bf0, "WidgetWindow::ShowGroup", WidgetWindow_ShowGroup_n, fp_groups)

static void __fastcall WidgetWindow_EnableGroup_n(WidgetWindow* self, Edx, uint32_t g) {
    const uint32_t d = self->disabled;
    if (!(d & g)) return;
    const uint32_t nd = ~g & d;
    self->disabled = nd;
    for (int32_t i = 0; i < self->count; i++) {
        Widget* w = self->widgets[i];
        const uint32_t m = w->groups;
        if (!(self->disabled & m)) {
            w->enabled = 1;
            if (w->visible != 0) w->dirty = 1;
        }
    }
}
PORT_FN(0x00480c50, "WidgetWindow::EnableGroup", WidgetWindow_EnableGroup_n, fp_groups)

static void __fastcall WidgetWindow_DisableGroup_n(WidgetWindow* self, Edx, uint32_t g) {
    const uint32_t d = self->disabled;
    if (d & g) return;
    self->disabled = g | d;
    for (int32_t i = 0; i < self->count; i++) {
        Widget* w = self->widgets[i];
        const uint32_t m = w->groups;
        if (self->disabled & m) {
            w->enabled = 0;
            if (w->visible != 0) w->dirty = 1;
        }
    }
}
PORT_FN(0x00480cc0, "WidgetWindow::DisableGroup", WidgetWindow_DisableGroup_n, fp_groups)

// AddWidget: appended (no bound), focused if it's the first tab stop, given the groups being entered, told it's added
// (through the vtable it had when it came in)
static void __fastcall WidgetWindow_AddWidget_n(WidgetWindow* self, Edx, Widget* w) {
    const int32_t n = self->count;
    void* const* vt = *(void* const* volatile*)w;
    self->widgets[n] = w;
    self->count = self->count + 1;
    typedef uint8_t(__fastcall * B_t)(Widget*, Edx);
    typedef void(__fastcall * V_t)(Widget*, Edx);
    if (((B_t)vt[0x48 / 4])(w, 0) && self->focus == 0) tcall<void>(F_WW_set_focus, self, w);
    w->groups = self->group;
    w->window = self;
    ((V_t)vt[0xc / 4])(w, 0);
}
static void fp_AddWidget(Footprint& f, WidgetWindow* win, Edx, Widget* w) { fp_add_widget(f, win, w); }
PORT_FN(0x00480d20, "WidgetWindow::AddWidget", WidgetWindow_AddWidget_n, fp_AddWidget)

// SetDefault: the focus to the first tab stop whose GetID is id
static void __fastcall WidgetWindow_SetDefault_n(WidgetWindow* self, Edx, int32_t id) {
    for (int32_t i = 0; i < self->count; i++) {
        if (vcall<uint8_t>(self->widgets[i], 0x48) && vcall<int32_t>(self->widgets[i], 0x10) == id) {
            tcall<void>(F_WW_set_focus, self, self->widgets[i]);
            return;
        }
    }
}
static void fp_WidgetWindow_SetDefault(Footprint& f, WidgetWindow* win, Edx, int32_t) { ui_fp_window_focus(f, win); }
PORT_FN(0x00480d70, "WidgetWindow::SetDefault", WidgetWindow_SetDefault_n, fp_WidgetWindow_SetDefault)

// ---- Widget's tab keys: on the running window --------------------------------------------------------------------------
static void fp_tab(Footprint& f, Widget*, Edx) { ui_fp_active_focus(f); }
static void __fastcall Widget_TabNext_n(Widget*, Edx) { tcall<void>(F_WW_NextWidget, ACTIVE); }
PORT_FN(0x00480dd0, "Widget::TabNext", Widget_TabNext_n, fp_tab)
static void __fastcall Widget_TabOff_n(Widget*, Edx) { tcall<void>(F_WW_set_focus, ACTIVE, (Widget*)0); }
PORT_FN(0x00480de0, "Widget::TabOff", Widget_TabOff_n, fp_tab)
static void __fastcall Widget_TabPrev_n(Widget*, Edx) { tcall<void>(F_WW_PrevWidget, ACTIVE); }
PORT_FN(0x00480df0, "Widget::TabPrev", Widget_TabPrev_n, fp_tab)
static void __fastcall Widget_TabCycle_n(Widget*, Edx) { tcall<void>(F_WW_NextWidget, ACTIVE); }
PORT_FN(0x00480e00, "Widget::TabCycle", Widget_TabCycle_n, fp_tab)

// ---- TitleBar: drags the running window --------------------------------------------------------------------------------
static void __fastcall TitleBar_MouseDown_n(TitleBar* self, Edx, int32_t x, int32_t y) {
    self->dragging = 1;
    self->dx = (int32_t)(0u - (uint32_t)x);
    self->dy = (int32_t)(0u - (uint32_t)y);
}
static void fp_TitleBar_MouseDown(Footprint& f, TitleBar* self, Edx, int32_t, int32_t) { f.add(self, sizeof(TitleBar), "title bar"); }
PORT_FN(0x00480e10, "TitleBar::MouseDown", TitleBar_MouseDown_n, fp_TitleBar_MouseDown)

static void __fastcall TitleBar_MouseMove_n(TitleBar* self, Edx, int32_t x, int32_t y) {
    if (self->dragging == 0) return;
    WidgetWindow* a = ACTIVE;
    const int32_t dy = self->dy;
    WidgetWindow* b = ACTIVE;
    const int32_t dx = self->dx;
    const int32_t ny = dy + a->y + y;
    b->x = b->x + (dx + x);
    a->y = ny;
}
static void fp_TitleBar_MouseMove(Footprint& f, TitleBar*, Edx, int32_t, int32_t) {
    if (ACTIVE) f.add(ACTIVE, sizeof(WidgetWindow), "the running window");
}
PORT_FN(0x00480e40, "TitleBar::MouseMove", TitleBar_MouseMove_n, fp_TitleBar_MouseMove)

#undef ACTIVE
}  // namespace ui_window
}  // namespace
