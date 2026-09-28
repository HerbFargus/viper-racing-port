// paint_kit.cpp -- M3 UI stage, step U4 (group C): the paint kit's canvas, its tools and its controls, rewritten faithfully
// (library `paintkit`, paintkit.obj; the dialog, the callbacks and the files are paint_main.cpp, tga.obj paint_tga.cpp).
//
//   The tools (Mode and its subclasses, embedded in the PaintKitCanvas): the pencil (a one-pixel line), the brush (one of
//   brush.stp's four canvases -- recoloured to the button's colour -- or a cut selection, stamped along the drag), the
//   shapes (line, rectangle, triangle: an XOR outline while dragging, drawn in the button's colour on release; the
//   selection rectangle cuts the painting into the brush), the eye dropper and the zoom. Every tool works on the one
//   PaintKitCanvas (the static at 0x5d4334), whichever object it's called on. The PaintKitCanvas: the painting, its undo
//   copy, the view (what is drawn, zoomed and scrolled by two scroll axes), the dirty rectangle Update copies to the car's
//   texture; its mouse handlers turn the mouse into painting pixels and hand it to the tool the radio buttons chose.
//   The controls: PaintCarViewer3D (the car, turned by dragging), ColorChooser, ColorIndicator, ColorBucket,
//   TemplateLayer / TemplatePreview (template_cb's), DecalViewer (decal_cb's).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own too, so a
// hooked rewrite is what runs), virtual calls through the vtable (a vtable entry the original loads once and calls twice
// is loaded once), the compiler's inline code as it runs it (its inlined copies of Mode::Dirty and DirtyRect, the string
// copies). x87: a register value is a double, a stored one a float, the original's grouping and constants' widths,
// __ftol as x87_ftol, fsin / fcos straight to a float (x87.h); floats the original moves or compares as integers are
// moved and compared as their bits.
//
// Footprints (main thread). A tool or the canvas can write the paint canvas (the object, its widget, its four canvases'
// pixels), the brush canvases (the cut selection's, brush.stp's four), the current-canvas static and the paint kit's
// statics (tool, brush, colours, scroll axes) -- fp_paint; a tool of a class outside the paint kit (a vtable it doesn't
// know) is left to the session replays. replay_only: whatever loads, allocates or frees (constructors, destructors that
// forget stamps or canvases, Default, the templates' draws, which allocate a scratch canvas), adds notifications or builds
// widgets (Create, Added), draws through the 3D renderer (PaintCarViewer3D::Draw3D) or grabs the car's texture
// (PaintKitCanvas::Update: the renderer's surfaces).
//
// Fixes (// FIX:, docs/FIXES.md "Paint kit"; none reached with the stock cars, English text and a game folder of ordinary
// length): the PaintKitCanvas's "<user directory>paint\<car>.cvs" isn't built when too long for its 0x104 bytes (the
// default painting is shown); PaintKitCanvas::Default's "<car>.cvs" has room for a 31-character car; DecalViewer's set
// name keeps to its 0x100 bytes (a long translation). Every other input gives the original's bits.
//
// FIX CANDIDATEs (left faithful, marked in place): a tool number outside 0..8 panics and calls through a null tool (the
// radio buttons set only 1..8); a brush number outside 0..4 reads outside the brush table (only the radio buttons set
// it); BrushMode::MouseDrag's and LineMode::DrawShape's brush line offsets y by half the brush's WIDTH (the brushes are
// square: no effect); the templates' draws index the stamps with the template shown unchecked (no templates found);
// DecalViewer's divisions by a decal's size or count (a damaged decals.tab: 0 faults) and the set shown (a static that
// outlives the dialog) unchecked against the sets read (a decals.tab of fewer rows, or none: an unset set, a crash). None
// is reached in ordinary play (the stock decals.tab and templates).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "paint_kit.h"

namespace {
namespace paint_kit {
using namespace pkit;
using uit::tcall; using uit::ccall; using uit::vcall; using uit::crt_strcpy; using uit::crt_copy; using uit::crt_strlen;
using uit::UIDialogItem; using uit::CustomWidget; using uit::Log_t; using uit::Sprintf_t; using uit::S_GX_CANVAS;

#define D(x) ((double)(x))
static __forceinline uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// ---- calls --------------------------------------------------------------------------------------------------------------------
static __forceinline gxCanvas* set_canvas(const volatile void* c) { return ccall<gxCanvas*>(F_gxSetCanvas, (void*)c); }
static __forceinline void cc_dirty(const volatile void* ctl) { tcall<void>(F_UICC_Dirty, (const void*)ctl); }
static __forceinline void paste(const volatile void* c, int32_t x, int32_t y) { ccall<void>(F_gxPaste, (void*)c, x, y); }
static __forceinline void paste_alpha(const volatile void* c, int32_t x, int32_t y) { ccall<void>(F_gxPasteAlpha, (void*)c, x, y); }
// a call through a vtable the original loaded once (the entry read at the call, or already read: `fn`)
typedef void(__fastcall* ShapeFn)(const void*, Edx, int32_t, int32_t, int32_t, int32_t, int32_t);
typedef void(__fastcall* VoidFn)(const void*, Edx);
static __forceinline const uint32_t* vt_of(const volatile void* obj) { return *(const uint32_t* const volatile*)obj; }
static __forceinline uint32_t vt_entry(const uint32_t* vt, uint32_t off) { return ((const volatile uint32_t*)vt)[off / 4]; }

// the brush table's entry n (read whatever n is: the original indexes it unchecked)
static __forceinline gxCanvas* brush_at(int32_t n) { return UI_GP(gxCanvas, S_BRUSHES + (uint32_t)n * 4u); }
static __forceinline int32_t cw(const gxCanvas* c) { return c->w; }
static __forceinline int32_t ch(const gxCanvas* c) { return c->h; }

// the dirty rectangle grown to take in x0..x1, y0..y1 (the compiler's inlined Mode::Dirty / DirtyRect), then the control
// marked dirty
static __forceinline void dirty_rect(PaintKitCanvas* g, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (g->dirty == 0) {
        g->dirty = 1;
        g->dx0 = x0;
        g->dy0 = y0;
        g->dx1 = x1;
        g->dy1 = y1;
    } else {
        int32_t v = g->dx0;
        if (!(v < x0)) v = x0;
        g->dx0 = v;
        v = g->dy0;
        if (!(v < y0)) v = y0;
        g->dy0 = v;
        v = g->dx1;
        if (!(v > x1)) v = x1;
        g->dx1 = v;
        v = g->dy1;
        if (!(v > y1)) v = y1;
        g->dy1 = v;
    }
    cc_dirty(g);
}

// the tool the radio buttons chose (the switch every canvas handler inlines: 5 and 6 are both the eye dropper); an unknown
// one is a panic, and the original then calls through a null tool
static PkMode* mode_for(PaintKitCanvas* self) {
    switch ((uint32_t)UI_G32(S_TOOL)) {
    case 0: return (PkMode*)&self->pencil;
    case 1: return (PkMode*)&self->brush;
    case 2: return (PkMode*)&self->line;
    case 3: return (PkMode*)&self->rect;
    case 4: return (PkMode*)&self->triangle;
    case 5: case 6: return (PkMode*)&self->eyedrop;
    case 7: return (PkMode*)&self->selrect;
    case 8: return (PkMode*)&self->zoom_mode;
    }
    // FIX CANDIDATE: a tool outside 0..8 -- LogPanic, then a call through address 0 (only the radio buttons, 1..8, and
    // Update, 6 or the saved one, set it: ordinary play can't)
    UI_LogPanic(PK_CP(0x00500db0), UI_G32(S_TOOL));                      // "Unknown mode: %d"
    return 0;
}
// a call to a tool's method: its vtable read at the call (a null tool faults there, as the original)
static __forceinline void mode_call(PkMode* volatile m, uint32_t off, int32_t x, int32_t y) {
    PkMode* p = m;
    typedef void(__fastcall * Fn)(const void*, Edx, int32_t, int32_t);
    ((Fn)(uintptr_t)vt_entry(vt_of(p), off))(p, 0, x, y);
}

// ---- footprints ---------------------------------------------------------------------------------------------------------------
static __forceinline void fp_cur(Footprint& f) { UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas"); }
static __forceinline void fp_canvas(Footprint& f, const volatile void* c) { UI_FP_CANVAS(f, (const gxCanvas*)c); }
static __forceinline void fp_widget(Footprint& f, const volatile void* ctl) {
    if (void* w = ((const PkCtl*)ctl)->widget) f.add(w, sizeof(CustomWidget), "its widget");
}
static uint32_t mode_size(const volatile void* m) {
    switch ((uint32_t)(uintptr_t)((const PkMode*)m)->vtbl) {
    case VT_Mode: return 4;
    case VT_PencilMode: case VT_ShapeMode: case VT_LineMode: case VT_RectMode: case VT_TriangleMode: case VT_SelRectMode: return 0x14;
    case VT_BrushMode: return 0x10;
    case VT_EyeDropMode: case VT_ZoomMode: return 8;
    }
    return 0;
}
// the cut brush: its canvas, and all of its pixels (the constructor's 256 x 256 x 4: SelRectMode and decal_cb give it any
// size up to that, whatever it is now)
static void fp_brush(Footprint& f) {
    UI_FP(f, S_BRUSH_CANVAS, sizeof(gxCanvas), "the cut brush");
    if (void* p = ((const gxCanvas*)PK_P(S_BRUSH_CANVAS))->pixels) f.add(p, 0x40000, "the cut brush's pixels");
}
// what the tools and the canvas's handlers can write (see the top)
static void fp_paint(Footprint& f) {
    fp_cur(f);
    if (PaintKitCanvas* g = pk_g()) {
        f.add(g, sizeof *g, "the paint canvas");
        fp_widget(f, g);
        fp_canvas(f, &g->canvas);
        fp_canvas(f, &g->view);
        fp_canvas(f, &g->undo);
        fp_canvas(f, &g->redo);
    }
    fp_brush(f);
    for (uint32_t p = S_BRUSHES + 4; p < S_BRUSHES_END; p += 4) fp_canvas(f, UI_GP(gxCanvas, p));
    UI_FP(f, S_BRUSH, 4, "the brush");
    UI_FP(f, S_TOOL, 4, "the tool");
    UI_FP(f, S_LEFT, 4, "the left colour");
    UI_FP(f, S_RIGHT, 4, "the right colour");
    UI_FP(f, S_VAXIS, 12, "the vertical scroll axis");
    UI_FP(f, S_HAXIS, 12, "the horizontal scroll axis");
}
// a tool and what it can reach; false (replay_only) for a tool of a class the paint kit doesn't have
static bool fp_mode(Footprint& f, const volatile void* m) {
    const uint32_t n = mode_size(m);
    if (!n) { f.replay_only = "a tool of a class outside the paint kit"; return false; }
    f.add((void*)m, n, "the tool");
    fp_paint(f);
    return true;
}
template <typename T> static void fp_mode_xy(Footprint& f, T* self, Edx, int32_t, int32_t) { fp_mode(f, self); }
template <typename T> static void fp_mode0(Footprint& f, T* self, Edx) { fp_mode(f, self); }
template <typename T> static void fp_mode_shape(Footprint& f, T* self, Edx, int32_t, int32_t, int32_t, int32_t, int32_t) { fp_mode(f, self); }
template <typename T> static void fp_pure_xy(Footprint& f, T*, Edx, int32_t, int32_t) { f.pure = true; }
template <typename T> static void fp_pure0(Footprint& f, T*, Edx) { f.pure = true; }
template <typename T> static void fp_dtor(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object (and forgets its stamp)"; }
// the canvas's handlers: the object, the tool the switch picks, and what that reaches
static void fp_canvas_tool(Footprint& f, PaintKitCanvas* self) {
    if ((uint32_t)UI_G32(S_TOOL) > 8u) { f.replay_only = "an unknown tool: LogPanic, then a call through a null tool"; return; }
    f.add(self, sizeof *self, "this");
    fp_widget(f, self);
    fp_canvas(f, &self->view);
    fp_mode(f, mode_for(self));
}

// =================================================================================================================================
// Mode and the tools
// =================================================================================================================================
// Mode::Dirty: the paint canvas's dirty rectangle takes in the rectangle (whichever object it's called on)
static void __fastcall Mode_Dirty_n(PkMode*, Edx, int32_t x0, int32_t y0, int32_t x1, int32_t y1) { dirty_rect(pk_g(), x0, y0, x1, y1); }
static void fp_Mode_Dirty(Footprint& f, PkMode*, Edx, int32_t, int32_t, int32_t, int32_t) {
    if (PaintKitCanvas* g = pk_g()) { f.add(g, sizeof *g, "the paint canvas"); fp_widget(f, g); }
}
PORT_FN(0x004c5ed0, "Mode::Dirty", Mode_Dirty_n, fp_Mode_Dirty)

// Mode::SetCanvas: the painting is the current canvas
static void __fastcall Mode_SetCanvas_n(PkMode*, Edx) { set_canvas(&pk_g()->canvas); }
static void fp_Mode_SetCanvas(Footprint& f, PkMode*, Edx) { fp_cur(f); }
PORT_FN(0x004c5f80, "Mode::SetCanvas", Mode_SetCanvas_n, fp_Mode_SetCanvas)

// Mode::SaveForUndo: the painting copied to the undo canvas; the painting is modified
static void __fastcall Mode_SaveForUndo_n(PkMode*, Edx) {
    PaintKitCanvas* g = pk_g();
    set_canvas(&g->undo);
    paste(&g->canvas, 0, 0);
    g->modified = 1;
}
static void fp_Mode_SaveForUndo(Footprint& f, PkMode*, Edx) {
    fp_cur(f);
    if (PaintKitCanvas* g = pk_g()) { f.add(g, sizeof *g, "the paint canvas"); fp_canvas(f, &g->undo); }
}
PORT_FN(0x004c5fa0, "Mode::SaveForUndo", Mode_SaveForUndo_n, fp_Mode_SaveForUndo)

// BrushMode::Draw: the cut selection (brush 0) drawn under the mouse as a preview, and its rectangle dirty
static void __fastcall BrushMode_Draw_n(BrushMode*, Edx, int32_t x, int32_t y) {
    if (UI_G32(S_BRUSH) > 0) return;
    const int32_t hw = UI_G32(S_BRUSH_CANVAS + 8) / 2;
    const int32_t hh = UI_G32(S_BRUSH_CANVAS + 0xc) / 2;
    const int32_t x0 = isub(x, hw), y0 = isub(y, hh);
    paste_alpha(PK_P(S_BRUSH_CANVAS), x0, y0);
    const int32_t y1 = iadd(y, isub(UI_G32(S_BRUSH_CANVAS + 0xc), hh));
    const int32_t x1 = iadd(x, isub(UI_G32(S_BRUSH_CANVAS + 8), hw));
    dirty_rect(pk_g(), x0, y0, x1, y1);
}
PORT_FN(0x004c5fd0, "BrushMode::Draw", BrushMode_Draw_n, fp_mode_xy<BrushMode>)

static void __fastcall BrushMode_MouseMove_n(BrushMode*, Edx, int32_t, int32_t) { cc_dirty(pk_g()); }
static void fp_BrushMode_MouseMove(Footprint& f, BrushMode*, Edx, int32_t, int32_t) { if (PaintKitCanvas* g = pk_g()) fp_widget(f, g); }
PORT_FN(0x004c60c0, "BrushMode::MouseMove", BrushMode_MouseMove_n, fp_BrushMode_MouseMove)

// ZoomMode::MouseDown / MouseRDown: one step in (at most 5) or out (at least 1), the point clicked kept where it is, the
// scroll axes set for the new zoom (their positions held to 0..total - visible)
static __forceinline void zoom_step(int32_t x, int32_t y, bool in) {
    volatile int32_t* zp = &pk_g()->zoom;
    const int32_t z0 = *zp;
    int32_t nz;
    if (in) {
        nz = z0 + 1;
        if (!(nz <= 5)) nz = 5;
    } else {
        nz = z0 - 1;
        if (!(nz >= 1)) nz = 1;
    }
    PaintKitCanvas* g2 = pk_g();
    if (z0 == 0) *zp = 1;
    const int32_t z = *zp;
    const int32_t ex = isub(imul(z, x), UI_G32(S_HSCROLL));
    const int32_t ey = isub(imul(y, z), UI_G32(S_VSCROLL));
    *zp = nz;
    UI_G32(S_HAXIS) = (int32_t)((uint32_t)nz << 8);
    UI_G32(S_VAXIS) = (int32_t)((uint32_t)*zp << 8);
    const int32_t w = g2->w;
    UI_G32(S_HAXIS + 4) = w;
    UI_G32(S_VAXIS + 4) = g2->h;
    UI_G32(S_HSCROLL) = isub(imul(*zp, x), ex);
    {
        const int32_t m = isub(UI_G32(S_HAXIS), w);
        if (UI_G32(S_HSCROLL) > m) UI_G32(S_HSCROLL) = m;
    }
    if (UI_G32(S_HSCROLL) < 0) UI_G32(S_HSCROLL) = 0;
    UI_G32(S_VSCROLL) = isub(imul(*zp, y), ey);
    {
        const int32_t m = isub(UI_G32(S_VAXIS), UI_G32(S_VAXIS + 4));
        if (UI_G32(S_VSCROLL) > m) UI_G32(S_VSCROLL) = m;
    }
    if (UI_G32(S_VSCROLL) < 0) UI_G32(S_VSCROLL) = 0;
}
static void __fastcall ZoomMode_MouseDown_n(CursorMode*, Edx, int32_t x, int32_t y) { zoom_step(x, y, true); }
static void __fastcall ZoomMode_MouseRDown_n(CursorMode*, Edx, int32_t x, int32_t y) { zoom_step(x, y, false); }
static void fp_zoom(Footprint& f, CursorMode*, Edx, int32_t, int32_t) {
    if (PaintKitCanvas* g = pk_g()) f.add((void*)&g->zoom, 4, "the zoom");
    UI_FP(f, S_VAXIS, 12, "the vertical scroll axis");
    UI_FP(f, S_HAXIS, 12, "the horizontal scroll axis");
}
PORT_FN(0x004c60d0, "ZoomMode::MouseDown", ZoomMode_MouseDown_n, fp_zoom)
PORT_FN(0x004c61c0, "ZoomMode::MouseRDown", ZoomMode_MouseRDown_n, fp_zoom)

// the tools that do nothing (Mode's), and the cursors
static void __fastcall Mode_nothing_up(PkMode*, Edx, int32_t, int32_t) {}
static void __fastcall Mode_nothing_rup(PkMode*, Edx, int32_t, int32_t) {}
static void __fastcall Mode_nothing_drag(PkMode*, Edx, int32_t, int32_t) {}
static void __fastcall Mode_nothing_move(PkMode*, Edx, int32_t, int32_t) {}
static void __fastcall Mode_nothing_draw(PkMode*, Edx, int32_t, int32_t) {}
static void __fastcall Mode_nothing_down(PkMode*, Edx, int32_t, int32_t) {}
static void __fastcall Mode_nothing_rdown(PkMode*, Edx, int32_t, int32_t) {}
PORT_FN(0x004c9140, "Mode::MouseUp", Mode_nothing_up, fp_pure_xy<PkMode>)
PORT_FN(0x004c9150, "Mode::MouseRUp", Mode_nothing_rup, fp_pure_xy<PkMode>)
PORT_FN(0x004c9160, "Mode::MouseDrag", Mode_nothing_drag, fp_pure_xy<PkMode>)
PORT_FN(0x004c9170, "Mode::MouseMove", Mode_nothing_move, fp_pure_xy<PkMode>)
PORT_FN(0x004c9180, "Mode::Draw", Mode_nothing_draw, fp_pure_xy<PkMode>)
PORT_FN(0x004c92d0, "Mode::MouseDown", Mode_nothing_down, fp_pure_xy<PkMode>)
PORT_FN(0x004c92e0, "Mode::MouseRDown", Mode_nothing_rdown, fp_pure_xy<PkMode>)
static void* __fastcall Mode_GetCursor_n(PkMode*, Edx) { return 0; }
PORT_FN(0x004c9190, "Mode::GetCursor", Mode_GetCursor_n, fp_pure0<PkMode>)
static void* __fastcall PencilMode_GetCursor_n(PencilMode* self, Edx) { return self->cursor; }
static void fp_cursor_mode(Footprint& f, PencilMode*, Edx) { f.pure = true; }
PORT_FN(0x004c91a0, "PencilMode::GetCursor", PencilMode_GetCursor_n, fp_cursor_mode)
static void* __fastcall ZoomMode_GetCursor_n(CursorMode* self, Edx) { return self->cursor; }
static void* __fastcall EyeDropMode_GetCursor_n(CursorMode* self, Edx) { return self->cursor; }
static void fp_cursor_mode8(Footprint& f, CursorMode*, Edx) { f.pure = true; }
PORT_FN(0x004c9a00, "ZoomMode::GetCursor", ZoomMode_GetCursor_n, fp_cursor_mode8)
PORT_FN(0x004c9ad0, "EyeDropMode::GetCursor", EyeDropMode_GetCursor_n, fp_cursor_mode8)

// PencilMode::MouseDown / MouseRDown: the painting saved for undo, the line starts here in the button's colour
static __forceinline void pencil_down(PencilMode* self, int32_t x, int32_t y, uint32_t color_at) {
    tcall<void>(F_Mode_SaveForUndo, self);
    self->x = x;
    self->y = y;
    self->color = UI_GU32(color_at);
    mode_call(self, MO_MouseDrag, x, y);
}
static void __fastcall PencilMode_MouseDown_n(PencilMode* self, Edx, int32_t x, int32_t y) { pencil_down(self, x, y, S_LEFT); }
static void __fastcall PencilMode_MouseRDown_n(PencilMode* self, Edx, int32_t x, int32_t y) { pencil_down(self, x, y, S_RIGHT); }
static void fp_pencil_xy(Footprint& f, PencilMode* self, Edx, int32_t, int32_t) { fp_mode(f, self); }
PORT_FN(0x004c91b0, "PencilMode::MouseDown", PencilMode_MouseDown_n, fp_pencil_xy)
PORT_FN(0x004c91e0, "PencilMode::MouseRDown", PencilMode_MouseRDown_n, fp_pencil_xy)

// PencilMode::MouseDrag: a line from the last point, the point itself, the rectangle dirty
static void __fastcall PencilMode_MouseDrag_n(PencilMode* self, Edx, int32_t x, int32_t y) {
    tcall<void>(F_Mode_SetCanvas, self);
    {
        const uint32_t c = self->color;
        const int32_t oy = self->y, ox = self->x;
        ccall<void>(F_gxLine, ox, oy, x, y, c);
    }
    ccall<void>(F_gxPoint, x, y, (uint32_t)self->color);
    const int32_t oy = self->y, ox = self->x;
    const int32_t y1 = iadd(y > oy ? y : oy, 1);
    const int32_t x1 = iadd(x > ox ? x : ox, 1);
    const int32_t y0 = y < oy ? y : oy;
    const int32_t x0 = x < ox ? x : ox;
    tcall<void>(F_Mode_Dirty, self, x0, y0, x1, y1);
    self->x = x;
    self->y = y;
}
PORT_FN(0x004c9210, "PencilMode::MouseDrag", PencilMode_MouseDrag_n, fp_pencil_xy)

// the deleting destructors: the cursor stamp forgotten (the tools that have one), Mode's vtable, the object freed if asked
static __forceinline void* mode_dtor(PkMode* self, uint32_t vt, uint32_t flags, bool stamp) {
    if (stamp) {
        void* c = ((CursorMode*)self)->cursor;
        self->vtbl = PK_P(vt);
        ccall<void>(F_gxForgetStamp, c);
    }
    self->vtbl = PK_P(VT_Mode);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void* __fastcall PencilMode_sdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, VT_PencilMode, fl, true); }
static void* __fastcall Mode_sdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, 0, fl, false); }
static void* __fastcall ShapeMode_sdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, 0, fl, false); }
static void* __fastcall ZoomMode_sdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, VT_ZoomMode, fl, true); }
static void* __fastcall EyeDropMode_vdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, VT_EyeDropMode, fl, true); }
static void* __fastcall BrushMode_sdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, 0, fl, false); }
static void* __fastcall LineMode_vdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, 0, fl, false); }
static void* __fastcall RectMode_vdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, 0, fl, false); }
static void* __fastcall TriangleMode_vdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, 0, fl, false); }
static void* __fastcall SelRectMode_vdd_n(PkMode* self, Edx, uint32_t fl) { return mode_dtor(self, 0, fl, false); }
// the ones without a stamp write only their vtable unless they free
template <typename T> static void fp_dtor_vt(Footprint& f, T* self, Edx, uint32_t fl) {
    if (fl & 1) { f.replay_only = "frees the object"; return; }
    f.add(self, 4, "its vtable");
}
PORT_FN(0x004c9290, "PencilMode::vector deleting destructor", PencilMode_sdd_n, fp_dtor<PkMode>)
PORT_FN(0x004c92f0, "Mode::vector deleting destructor", Mode_sdd_n, fp_dtor_vt<PkMode>)
PORT_FN(0x004c9680, "ShapeMode::vector deleting destructor", ShapeMode_sdd_n, fp_dtor_vt<PkMode>)
PORT_FN(0x004c9a10, "ZoomMode::vector deleting destructor", ZoomMode_sdd_n, fp_dtor<PkMode>)
PORT_FN(0x004c9b40, "EyeDropMode::scalar deleting destructor", EyeDropMode_vdd_n, fp_dtor<PkMode>)
PORT_FN(0x004caad0, "BrushMode::vector deleting destructor", BrushMode_sdd_n, fp_dtor_vt<PkMode>)
PORT_FN(0x004caaf0, "LineMode::scalar deleting destructor", LineMode_vdd_n, fp_dtor_vt<PkMode>)
PORT_FN(0x004cab10, "RectMode::scalar deleting destructor", RectMode_vdd_n, fp_dtor_vt<PkMode>)
PORT_FN(0x004cab30, "TriangleMode::scalar deleting destructor", TriangleMode_vdd_n, fp_dtor_vt<PkMode>)
PORT_FN(0x004cab50, "SelRectMode::scalar deleting destructor", SelRectMode_vdd_n, fp_dtor_vt<PkMode>)

// BrushMode::MouseDown / MouseRDown: the stroke starts here in the button's colour (saved for undo), then a drag
static __forceinline void brush_down(BrushMode* self, int32_t x, int32_t y, uint32_t color_at) {
    self->x = x;
    self->y = y;
    self->color = UI_GU32(color_at);
    tcall<void>(F_Mode_SaveForUndo, self);
    mode_call(self, MO_MouseDrag, x, y);
}
static void __fastcall BrushMode_MouseDown_n(BrushMode* self, Edx, int32_t x, int32_t y) { brush_down(self, x, y, S_LEFT); }
static void __fastcall BrushMode_MouseRDown_n(BrushMode* self, Edx, int32_t x, int32_t y) { brush_down(self, x, y, S_RIGHT); }
static void fp_brush_xy(Footprint& f, BrushMode* self, Edx, int32_t, int32_t) { fp_mode(f, self); }
PORT_FN(0x004c9310, "BrushMode::MouseDown", BrushMode_MouseDown_n, fp_brush_xy)
PORT_FN(0x004c9340, "BrushMode::MouseRDown", BrushMode_MouseRDown_n, fp_brush_xy)

// BrushMode::MouseDrag: brush.stp's brushes (1..3) recoloured and stamped at a click, or along a line from the last point
// (the 4th is only stamped); the cut selection (0) stamped where the mouse is when it hasn't moved; the rectangle dirty
static void __fastcall BrushMode_MouseDrag_n(BrushMode* self, Edx, int32_t x, int32_t y) {
    const int32_t bi = UI_G32(S_BRUSH);
    // FIX CANDIDATE: a brush outside 0..4 reads outside the table (only the radio buttons, 1..4, and SelRectMode, 0, set it)
    gxCanvas* const b = brush_at(bi);
    int32_t py;                                              // the stamp's y (the original's ecx at 0x4c941d)
    if (bi > 0) {
        if (self->x == x && self->y == y) {
            ccall<void>(F_color_brush, b, (uint32_t)self->color);
            tcall<void>(F_Mode_SetCanvas, self);
            py = y;
            goto stamp;
        }
        tcall<void>(F_Mode_SetCanvas, self);
        if (UI_G32(S_BRUSH) == 4) {
            py = y;
            goto stamp;
        }
        {
            // FIX CANDIDATE: the line's y is offset by half the brush's width, not its height (brush.stp's brushes are
            // square: no difference in play)
            const int32_t hw = cw(b) / 2;
            const int32_t sy = self->y;
            const int32_t hh = ch(b) / 2;
            const int32_t oy = isub(sy, hh);
            const int32_t ox = isub(self->x, hw);
            ccall<void>(F_gxBrushLine, isub(x, hw), isub(y, hw), ox, oy, b);
        }
        goto dirty;
    }
    tcall<void>(F_Mode_SetCanvas, self);
    if (!(self->x == x && self->y == y)) goto dirty;
    py = y;
stamp:
    {
        const int32_t sy = isub(py, ch(b) / 2);
        const int32_t sx = isub(x, cw(b) / 2);
        paste_alpha(b, sx, sy);
    }
dirty:
    {
        const int32_t oy = self->y;
        const int32_t hh = ch(b) / 2;
        const int32_t ox = self->x;
        const int32_t hw = cw(b) / 2;
        const int32_t y1 = iadd(isub(y > oy ? y : oy, hh), ch(b));
        const int32_t x1 = iadd(isub(x > ox ? x : ox, hw), cw(b));
        const int32_t y0 = isub(y < oy ? y : oy, hh);
        const int32_t x0 = isub(x < ox ? x : ox, hw);
        tcall<void>(F_Mode_Dirty, self, x0, y0, x1, y1);
    }
    self->x = x;
    self->y = y;
}
PORT_FN(0x004c9370, "BrushMode::MouseDrag", BrushMode_MouseDrag_n, fp_brush_xy)

// ShapeMode::ShapeMode
static ShapeMode* __fastcall ShapeMode_ctor_n(ShapeMode* self, Edx) {
    self->vtbl = PK_P(VT_ShapeMode);
    self->y1 = 0;
    self->x1 = 0;
    self->y0 = 0;
    self->x0 = 0;
    return self;
}
static void fp_ShapeMode_ctor(Footprint& f, ShapeMode* self, Edx) { f.add(self, sizeof *self, "this (constructed)"); }
PORT_FN(0x004c94c0, "ShapeMode::ShapeMode", ShapeMode_ctor_n, fp_ShapeMode_ctor)

// ShapeMode::MouseDown / MouseRDown: saved for undo, the shape starts (and ends) here
static __forceinline void shape_down(ShapeMode* self, int32_t x, int32_t y) {
    tcall<void>(F_Mode_SaveForUndo, self);
    self->x1 = x;
    self->x0 = x;
    self->y1 = y;
    self->y0 = y;
}
static void __fastcall ShapeMode_MouseDown_n(ShapeMode* self, Edx, int32_t x, int32_t y) { shape_down(self, x, y); }
static void __fastcall ShapeMode_MouseRDown_n(ShapeMode* self, Edx, int32_t x, int32_t y) { shape_down(self, x, y); }
static void fp_shape_xy(Footprint& f, ShapeMode* self, Edx, int32_t, int32_t) { fp_mode(f, self); }
PORT_FN(0x004c94e0, "ShapeMode::MouseDown", ShapeMode_MouseDown_n, fp_shape_xy)
PORT_FN(0x004c9500, "ShapeMode::MouseRDown", ShapeMode_MouseRDown_n, fp_shape_xy)

// ShapeMode::MouseDrag: the old outline XORed away, the new one XORed in (each rectangle dirty) -- if the mouse moved
static void __fastcall ShapeMode_MouseDrag_n(ShapeMode* self, Edx, int32_t x, int32_t y) {
    if (self->x1 == x && self->y1 == y) return;
    tcall<void>(F_Mode_SetCanvas, self);
    const uint32_t* vt = vt_of(self);
    {
        const int32_t y1 = self->y1, x1 = self->x1, y0 = self->y0, x0 = self->x0;
        ((ShapeFn)(uintptr_t)vt_entry(vt, MO_DrawShape))(self, 0, x0, y0, x1, y1, 0);
    }
    ((VoidFn)(uintptr_t)vt_entry(vt, MO_dirty_shape))(self, 0);
    {
        const int32_t y0 = self->y0, x0 = self->x0;
        self->x1 = x;
        self->y1 = y;
        ((ShapeFn)(uintptr_t)vt_entry(vt, MO_DrawShape))(self, 0, x0, y0, x, y, 0);
    }
    ((VoidFn)(uintptr_t)vt_entry(vt, MO_dirty_shape))(self, 0);
}
PORT_FN(0x004c9520, "ShapeMode::MouseDrag", ShapeMode_MouseDrag_n, fp_shape_xy)

// ShapeMode::MouseUp / MouseRUp: the outline XORed away, the shape drawn in the button's colour (1 left, 2 right)
static __forceinline void shape_up(ShapeMode* self, int32_t x, int32_t y, int32_t how) {
    tcall<void>(F_Mode_SetCanvas, self);
    const uint32_t* vt = vt_of(self);
    const ShapeFn draw = (ShapeFn)(uintptr_t)vt_entry(vt, MO_DrawShape);
    {
        const int32_t y1 = self->y1, x1 = self->x1, y0 = self->y0, x0 = self->x0;
        draw(self, 0, x0, y0, x1, y1, 0);
    }
    const VoidFn dirty = (VoidFn)(uintptr_t)vt_entry(vt, MO_dirty_shape);
    dirty(self, 0);
    {
        const int32_t y0 = self->y0;
        self->x1 = x;
        self->y1 = y;
        const int32_t x0 = self->x0;
        draw(self, 0, x0, y0, x, y, how);
    }
    dirty(self, 0);
}
static void __fastcall ShapeMode_MouseUp_n(ShapeMode* self, Edx, int32_t x, int32_t y) { shape_up(self, x, y, 1); }
static void __fastcall ShapeMode_MouseRUp_n(ShapeMode* self, Edx, int32_t x, int32_t y) { shape_up(self, x, y, 2); }
PORT_FN(0x004c9580, "ShapeMode::MouseUp", ShapeMode_MouseUp_n, fp_shape_xy)
PORT_FN(0x004c95e0, "ShapeMode::MouseRUp", ShapeMode_MouseRUp_n, fp_shape_xy)

// ShapeMode::dirty_shape: the shape's bounding rectangle (x1, y1 inclusive)
static void __fastcall ShapeMode_dirty_shape_n(ShapeMode* self, Edx) {
    const int32_t x1 = self->x1, y1 = self->y1, x0 = self->x0, y0 = self->y0;
    const int32_t by = iadd(y1 > y0 ? y1 : y0, 1);
    const int32_t bx = iadd(x0 > x1 ? x0 : x1, 1);
    const int32_t ty = y1 < y0 ? y1 : y0;
    const int32_t tx = x0 < x1 ? x0 : x1;
    tcall<void>(F_Mode_Dirty, self, tx, ty, bx, by);
}
static void fp_shape0(Footprint& f, ShapeMode* self, Edx) { fp_mode(f, self); }
PORT_FN(0x004c9640, "ShapeMode::dirty_shape", ShapeMode_dirty_shape_n, fp_shape0)

// LineMode::DrawShape: an XOR line (0), or the brush along the line in the button's colour (1 left, else right)
static void __fastcall LineMode_DrawShape_n(ShapeMode* self, Edx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t how) {
    if (how == 0) {
        ccall<void>(F_gxXORLine, x0, y0, x1, y1);
        return;
    }
    const int32_t bi = UI_G32(S_BRUSH);
    gxCanvas* const b = brush_at(bi);                                   // FIX CANDIDATE: a brush outside 0..4 (as above)
    if (bi > 0) ccall<void>(F_color_brush, b, how == 1 ? UI_GU32(S_LEFT) : UI_GU32(S_RIGHT));
    tcall<void>(F_Mode_SetCanvas, self);
    const int32_t hw = cw(b) / 2;
    const int32_t hh = ch(b) / 2;
    // FIX CANDIDATE: the start's y offset by half the width (as BrushMode::MouseDrag's)
    ccall<void>(F_gxBrushLine, isub(x0, hw), isub(y0, hw), isub(x1, hw), isub(y1, hh), b);
}
PORT_FN(0x004c96a0, "LineMode::DrawShape", LineMode_DrawShape_n, fp_mode_shape<ShapeMode>)

// LineMode::dirty_shape: the line's rectangle grown by the brush
static void __fastcall LineMode_dirty_shape_n(ShapeMode* self, Edx) {
    const int32_t bi = UI_G32(S_BRUSH);
    const int32_t y1 = self->y1;
    gxCanvas* const b = brush_at(bi);
    const int32_t x0 = self->x0, x1 = self->x1;
    const int32_t hh = ch(b) / 2;
    const int32_t hw = cw(b) / 2;
    int32_t y0 = self->y0;
    const int32_t by = iadd(isub(y0 > y1 ? y0 : y1, hh), ch(b));
    const int32_t bx = iadd(isub(x0 > x1 ? x0 : x1, hw), cw(b));
    y0 = self->y0;
    const int32_t ty = isub(y0 < y1 ? y0 : y1, hh);
    const int32_t tx = isub(x0 < x1 ? x0 : x1, hw);
    tcall<void>(F_Mode_Dirty, self, tx, ty, bx, by);
}
PORT_FN(0x004c9740, "LineMode::dirty_shape", LineMode_dirty_shape_n, fp_mode0<ShapeMode>)

// RectMode::DrawShape: an XOR outline (0; nothing when it has no width or height), or a filled rectangle
static void __fastcall RectMode_DrawShape_n(ShapeMode*, Edx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t how) {
    if (how == 0) {
        if (x1 == x0 || y0 == y1) return;
        const int32_t l = x1 < x0 ? x1 : x0;
        const int32_t t = y0 < y1 ? y0 : y1;
        const int32_t r = isub(x1 > x0 ? x1 : x0, 1);
        const int32_t bt = isub(y0 > y1 ? y0 : y1, 1);
        ccall<void>(F_gxXORLine, l, t, r, t);
        ccall<void>(F_gxXORLine, r, t, r, bt);
        ccall<void>(F_gxXORLine, r, bt, l, bt);
        ccall<void>(F_gxXORLine, l, bt, l, t);
        return;
    }
    const uint32_t c = how == 1 ? UI_GU32(S_LEFT) : UI_GU32(S_RIGHT);
    const int32_t by = y0 > y1 ? y0 : y1;
    const int32_t bx = x1 > x0 ? x1 : x0;
    const int32_t ty = y0 < y1 ? y0 : y1;
    const int32_t tx = x1 < x0 ? x1 : x0;
    ccall<void>(F_gxRect, tx, ty, bx, by, c);
}
PORT_FN(0x004c97c0, "RectMode::DrawShape", RectMode_DrawShape_n, fp_mode_shape<ShapeMode>)

// SelRectMode::DrawShape: an XOR outline (0), or the rectangle (held to the painting) cut into the brush canvas, made
// opaque, and made the brush (tool 1, brush 0)
static void __fastcall SelRectMode_DrawShape_n(ShapeMode*, Edx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t how) {
    if (how == 0) {
        if (x1 == x0 || y1 == y0) return;
        const int32_t l = x1 < x0 ? x1 : x0;
        const int32_t t = y1 < y0 ? y1 : y0;
        const int32_t r = isub(x1 > x0 ? x1 : x0, 1);
        const int32_t bt = isub(y1 > y0 ? y1 : y0, 1);
        ccall<void>(F_gxXORLine, l, t, r, t);
        ccall<void>(F_gxXORLine, r, t, r, bt);
        ccall<void>(F_gxXORLine, r, bt, l, bt);
        ccall<void>(F_gxXORLine, l, bt, l, t);
        return;
    }
    int32_t l = x1 < x0 ? x1 : x0;
    if (!(l > 0)) l = 0;
    int32_t t = y1 < y0 ? y1 : y0;
    if (!(t > 0)) t = 0;
    int32_t w = x1 > x0 ? x1 : x0;
    if (!(w < 0x100)) w = 0x100;
    w = isub(w, l);
    int32_t h = y1 > y0 ? y1 : y0;
    if (!(h < 0x100)) h = 0x100;
    h = isub(h, t);
    if (w > 0x100) w = 0x100;
    if (h > 0x100) h = 0x100;
    if (w <= 0 || h <= 0) return;
    gxCanvas* const bc = (gxCanvas*)PK_P(S_BRUSH_CANVAS);
    bc->pitch = (int32_t)((uint32_t)w << 2);
    bc->w = w;
    bc->format = 5;
    bc->h = h;
    ccall<void>(F_gxCut, bc, l, t);
    volatile uint32_t* p = (volatile uint32_t*)bc->pixels;
    for (int32_t n = imul(h, w); n > 0; n--) *p++ |= 0xff000000u;
    UI_G32(S_BRUSH) = 0;
    UI_G32(S_TOOL) = 1;
}
PORT_FN(0x004c98a0, "SelRectMode::DrawShape", SelRectMode_DrawShape_n, fp_mode_shape<ShapeMode>)

// TriangleMode::DrawShape: an XOR outline (0), or a filled right triangle: (x0,y0) (x1,y0) (x0,y1)
static void __fastcall TriangleMode_DrawShape_n(ShapeMode*, Edx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t how) {
    if (how == 0) {
        ccall<void>(F_gxXORLine, x0, y0, x1, y0);
        ccall<void>(F_gxXORLine, x1, y0, x0, y1);
        ccall<void>(F_gxXORLine, x0, y1, x0, y0);
        return;
    }
    const uint32_t c = how == 1 ? UI_GU32(S_LEFT) : UI_GU32(S_RIGHT);
    ccall<void>(F_gxTriangle, x0, y0, x1, y0, x0, y1, c);
}
PORT_FN(0x004c9a50, "TriangleMode::DrawShape", TriangleMode_DrawShape_n, fp_mode_shape<ShapeMode>)

// EyeDropMode: the colour under the click becomes the button's; letting go goes back to the tool before
static void __fastcall EyeDropMode_MouseDown_n(CursorMode* self, Edx, int32_t x, int32_t y) {
    tcall<void>(F_Mode_SetCanvas, self);
    UI_GU32(S_LEFT) = ccall<uint32_t>(F_gxGetPixel, x, y);
}
static void __fastcall EyeDropMode_MouseRDown_n(CursorMode* self, Edx, int32_t x, int32_t y) {
    tcall<void>(F_Mode_SetCanvas, self);
    UI_GU32(S_RIGHT) = ccall<uint32_t>(F_gxGetPixel, x, y);
}
static void fp_eyedrop(Footprint& f, CursorMode*, Edx, int32_t, int32_t) {
    fp_cur(f);
    UI_FP(f, S_LEFT, 4, "the left colour");
    UI_FP(f, S_RIGHT, 4, "the right colour");
}
PORT_FN(0x004c9ae0, "EyeDropMode::MouseDown", EyeDropMode_MouseDown_n, fp_eyedrop)
PORT_FN(0x004c9b00, "EyeDropMode::MouseRDown", EyeDropMode_MouseRDown_n, fp_eyedrop)
static void __fastcall EyeDropMode_MouseUp_n(CursorMode*, Edx, int32_t, int32_t) { UI_G32(S_TOOL) = UI_G32(S_SAVED_TOOL); }
static void __fastcall EyeDropMode_MouseRUp_n(CursorMode*, Edx, int32_t, int32_t) { UI_G32(S_TOOL) = UI_G32(S_SAVED_TOOL); }
static void fp_eyedrop_up(Footprint& f, CursorMode*, Edx, int32_t, int32_t) { UI_FP(f, S_TOOL, 4, "the tool"); }
PORT_FN(0x004c9b20, "EyeDropMode::MouseUp", EyeDropMode_MouseUp_n, fp_eyedrop_up)
PORT_FN(0x004c9b30, "EyeDropMode::MouseRUp", EyeDropMode_MouseRUp_n, fp_eyedrop_up)

// =================================================================================================================================
// PaintKitCanvas
// =================================================================================================================================
// PaintKitCanvas::PaintKitCanvas(paint): the tools, the four canvases and the brushes, the cursors; the painting read from
// the user's paint folder ("<car>.cvs", a viper's "paint<n>.cvs"), or the default one
static PaintKitCanvas* __fastcall PaintKitCanvas_ctor_n(PaintKitCanvas* self, Edx, int32_t paint) {
    char buf[0x104];
    self->vtbl = PK_P(VT_UICustomControl);
    self->pencil.vtbl = PK_P(VT_PencilMode);
    self->widget = 0;
    self->pencil.cursor = ccall<void*>(F_gxGetStamp, PK_CP(0x00500d8c));                  // "c_pencil.stp"
    self->pencil.y = 0;
    self->pencil.x = 0;
    self->line.y1 = 0;
    self->line.x1 = 0;
    self->line.y0 = 0;
    self->line.x0 = 0;
    self->brush.vtbl = PK_P(VT_BrushMode);
    self->line.vtbl = PK_P(VT_ShapeMode);
    self->line.vtbl = PK_P(VT_LineMode);
    self->eyedrop.vtbl = PK_P(VT_EyeDropMode);
    self->eyedrop.cursor = ccall<void*>(F_gxGetStamp, PK_CP(0x00500d38));                 // "c_drop.stp"
    self->rect.y1 = 0;
    self->rect.x1 = 0;
    self->rect.y0 = 0;
    self->rect.x0 = 0;
    self->triangle.y1 = 0;
    self->triangle.x1 = 0;
    self->triangle.y0 = 0;
    self->triangle.x0 = 0;
    self->rect.vtbl = PK_P(VT_ShapeMode);
    self->rect.vtbl = PK_P(VT_RectMode);
    self->triangle.vtbl = PK_P(VT_ShapeMode);
    self->triangle.vtbl = PK_P(VT_TriangleMode);
    tcall<ShapeMode*>(F_ShapeMode_ctor, &self->selrect);
    self->selrect.vtbl = PK_P(VT_SelRectMode);
    self->zoom_mode.vtbl = PK_P(VT_ZoomMode);
    self->zoom_mode.cursor = ccall<void*>(F_gxGetStamp, PK_CP(0x00500d80));               // "c_zoom.stp"
    self->vtbl = PK_P(VT_PaintKitCanvas);
    self->zoom = 1;
    self->over = 0;
    self->modified = 0;
    UI_GP(PaintKitCanvas, S_CANVAS) = self;
    self->dirty = 0;
    self->dy0 = 0;
    self->dx0 = 0;
    self->dy1 = 0;
    self->dx1 = 0;
    ccall<void>(F_gxAllocCanvas, &self->canvas, (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    ccall<void>(F_gxAllocCanvas, &self->undo, (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    ccall<void>(F_gxAllocCanvas, &self->redo, (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    ccall<void>(F_gxAllocCanvas, &self->view, (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    ccall<void>(F_gxAllocCanvas, PK_P(S_BRUSH_CANVAS), (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    UI_GU32(S_BRUSHES) = S_BRUSH_CANVAS;
    for (int32_t i = 0; i < 4; i++) {
        UI_sprintf(buf, PK_CP(0x00500d50), i);                                             // "brush%d.cvs"
        UI_GP(gxCanvas, S_BRUSHES + 4 + 4 * (uint32_t)i) = ccall<gxCanvas*>(F_gxCanvasGet, (const char*)buf);
    }
    UI_G32(S_BRUSH_CANVAS + 0xc) = 0;
    UI_G32(S_BRUSH_CANVAS + 8) = 0;
    set_canvas(&self->canvas);
    ccall<void>(F_gxClear, UI_GU32(S_CLEAR));
    self->down = 0;
    self->cursor = ccall<void*>(F_gxGetStamp, PK_CP(0x004fefb0));                          // "cross.stp"
    UI_G32(S_TOOL) = 1;
    UI_G32(S_BRUSH) = 1;
    UI_G32(S_SAVED_TOOL) = 1;
    self->overlay = ccall<gxCanvas*>(F_gxCanvasGet, PK_CP(0x00500d44));
    self->modified = 0;
    self->paint = paint;
    // FIX: the user directory and the car's name were formatted into the frame's 0x104 bytes unbounded: a user directory
    // of about 240 characters (the DLL's is at most 200; a longer game folder keeps the original's literal) ran over the
    // frame. A path too long for them isn't built, and the painting isn't read -- no file of that name can be opened --:
    // the car's default painting is shown, as when the player has saved none. A path that fits is built as before.
    bool too_long = false;
    if (UI_G8(S_IS_VIPER) != 0) {
        const char* u = ccall<const char*>(F_Win32GetUserDirectory);
        too_long = VP_FIX && pk_path_too_long(u, 15 + pk_dec_len(paint));
        if (!too_long) UI_sprintf(buf, PK_CP(0x00500d6c), u, paint);                      // "%spaint\\paint%d.cvs"
    } else {
        const char* u = ccall<const char*>(F_Win32GetUserDirectory);
        too_long = VP_FIX && pk_path_too_long(u, 10 + pk_len(PK_CP(S_CAR), PK_PATH_MAX));
        if (!too_long) UI_sprintf(buf, PK_CP(0x00500d5c), u, PK_CP(S_CAR));               // "%spaint\\%s.cvs"
    }
    set_canvas(&self->canvas);
    ccall<void>(F_gxClear, UI_GU32(S_CLEAR));
    if (!too_long && ccall<uint8_t>(F_gxCanvasRead, &self->canvas, (const char*)buf)) {
        tcall<void>(F_PaintKitCanvas_DirtyRect, self, (int32_t)0, (int32_t)0, (int32_t)0x100, (int32_t)0x100);
        set_canvas(&self->undo);
        paste(&self->canvas, 0, 0);
        self->modified = 0;
    } else {
        tcall<void>(F_PaintKitCanvas_Default, self);
    }
    return self;
}
static void fp_PaintKitCanvas_ctor(Footprint& f, PaintKitCanvas*, Edx, int32_t) { f.replay_only = "allocates canvases, loads stamps, canvases and the painting"; }
PORT_FN(0x004c8e10, "PaintKitCanvas::PaintKitCanvas", PaintKitCanvas_ctor_n, fp_PaintKitCanvas_ctor)

// PaintKitCanvas::Default: the car's default painting ("<car>.cvs", a viper's "paint<n>.cvs", in the game's data) or
// black; all of it dirty; the undo copy; modified
static void __fastcall PaintKitCanvas_Default_n(PaintKitCanvas* self, Edx) {
    // FIX: "<car>.cvs" went into 0x20 bytes of the frame (the original's: its return address right after them), so a car
    // name of 28 to 31 characters (S_CAR holds 31) overran it. The name has 0x40 bytes now, room for it whole -- cut, it
    // would name another painting or none. A name too long even for them (S_CAR unterminated, a damaged static) is taken as
    // a car with no default painting: the painting is cleared, as for any such car. A name that fits is used as before.
    char buf[VP_FIX ? 0x40 : 0x20];
    bool named = true;
    if (UI_G8(S_IS_VIPER) != 0) UI_sprintf(buf, PK_CP(0x00500da4), self->paint);          // "paint%d.cvs"
    else if (VP_FIX && pk_len(PK_CP(S_CAR), sizeof buf - 5) > sizeof buf - 5) named = false;
    else UI_sprintf(buf, PK_CP(0x00500d9c), PK_CP(S_CAR));                                // "%s.cvs"
    if (named && ccall<uint8_t>(F_ResourceExists, (const char*)buf)) {
        gxCanvas* c = ccall<gxCanvas*>(F_gxCanvasGet, (const char*)buf);
        set_canvas(&self->canvas);
        ccall<void>(F_gxClear, UI_GU32(S_CLEAR));
        paste(c, 0, 0);
        ccall<void>(F_gxCanvasForget, c);
    } else {
        set_canvas(&self->canvas);
        ccall<void>(F_gxClear, UI_GU32(S_CLEAR));
    }
    dirty_rect(self, 0, 0, 0x100, 0x100);
    set_canvas(&self->undo);
    paste(&self->canvas, 0, 0);
    self->modified = 1;
}
static void fp_PaintKitCanvas_Default(Footprint& f, PaintKitCanvas*, Edx) { f.replay_only = "loads the default painting (a resource canvas)"; }
PORT_FN(0x004c9b80, "PaintKitCanvas::Default", PaintKitCanvas_Default_n, fp_PaintKitCanvas_Default)

// ~PaintKitCanvas: the canvases freed, the brushes and the overlay forgotten, the tools' stamps forgotten (their inlined
// destructors: each tool's vtable back to Mode's)
static __forceinline void paintkit_canvas_dtor(PaintKitCanvas* self) {
    gxCanvas* ov = self->overlay;
    self->vtbl = PK_P(VT_PaintKitCanvas);
    ccall<void>(F_gxCanvasForget, ov);
    ccall<void>(F_gxFreeCanvas, &self->canvas);
    ccall<void>(F_gxFreeCanvas, &self->undo);
    ccall<void>(F_gxFreeCanvas, &self->redo);
    ccall<void>(F_gxFreeCanvas, &self->view);
    ccall<void>(F_gxFreeCanvas, PK_P(S_BRUSH_CANVAS));
    for (uint32_t p = S_BRUSHES + 4; p < S_BRUSHES_END; p += 4) ccall<void>(F_gxCanvasForget, UI_GP(gxCanvas, p));
    ccall<void>(F_gxForgetStamp, (void*)self->cursor);
    {
        void* c = self->zoom_mode.cursor;
        self->zoom_mode.vtbl = PK_P(VT_ZoomMode);
        ccall<void>(F_gxForgetStamp, c);
    }
    self->zoom_mode.vtbl = PK_P(VT_Mode);
    {
        void* c = self->eyedrop.cursor;
        self->selrect.vtbl = PK_P(VT_Mode);
        self->triangle.vtbl = PK_P(VT_Mode);
        self->eyedrop.vtbl = PK_P(VT_EyeDropMode);
        self->rect.vtbl = PK_P(VT_Mode);
        ccall<void>(F_gxForgetStamp, c);
    }
    self->eyedrop.vtbl = PK_P(VT_Mode);
    {
        void* c = self->pencil.cursor;
        self->line.vtbl = PK_P(VT_Mode);
        self->brush.vtbl = PK_P(VT_Mode);
        self->pencil.vtbl = PK_P(VT_PencilMode);
        ccall<void>(F_gxForgetStamp, c);
    }
    self->pencil.vtbl = PK_P(VT_Mode);
    self->vtbl = PK_P(VT_UICustomControl);
}
static void __fastcall PaintKitCanvas_dtor_n(PaintKitCanvas* self, Edx) { paintkit_canvas_dtor(self); }
static void fp_PaintKitCanvas_dtor(Footprint& f, PaintKitCanvas*, Edx) { f.replay_only = "frees canvases, forgets stamps and canvases"; }
PORT_FN(0x004c9ce0, "PaintKitCanvas::~PaintKitCanvas", PaintKitCanvas_dtor_n, fp_PaintKitCanvas_dtor)
static void* __fastcall PaintKitCanvas_sdd_n(PaintKitCanvas* self, Edx, uint32_t fl) {
    paintkit_canvas_dtor(self);
    if (fl & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void fp_PaintKitCanvas_sdd(Footprint& f, PaintKitCanvas*, Edx, uint32_t) { f.replay_only = "frees canvases, forgets stamps and canvases"; }
PORT_FN(0x004ca9b0, "PaintKitCanvas::vector deleting destructor", PaintKitCanvas_sdd_n, fp_PaintKitCanvas_sdd)

// PaintKitCanvas::Create: the two scroll axes watched; zoom 1 around the painting's centre
static void __fastcall PaintKitCanvas_Create_n(PaintKitCanvas* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, PK_P(S_VAXIS), (uint32_t)0xc);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, PK_P(S_HAXIS), (uint32_t)0xc);
    if (self->zoom == 0) self->zoom = 1;
    const int32_t c = (int32_t)((uint32_t)self->zoom << 7);
    const int32_t ey = isub(c, UI_G32(S_VSCROLL));
    const int32_t ex = isub(c, UI_G32(S_HSCROLL));
    self->zoom = 1;
    UI_G32(S_HAXIS) = 0x100;
    UI_G32(S_VAXIS) = (int32_t)((uint32_t)self->zoom << 8);
    UI_G32(S_HAXIS + 4) = self->w;
    UI_G32(S_VAXIS + 4) = self->h;
    UI_G32(S_HSCROLL) = isub((int32_t)((uint32_t)self->zoom << 7), ex);
    {
        const int32_t m = isub(0x100, UI_G32(S_HAXIS + 4));
        if (UI_G32(S_HSCROLL) > m) UI_G32(S_HSCROLL) = m;
    }
    if (UI_G32(S_HSCROLL) < 0) UI_G32(S_HSCROLL) = 0;
    UI_G32(S_VSCROLL) = isub((int32_t)((uint32_t)self->zoom << 7), ey);
    {
        const int32_t m = isub(UI_G32(S_VAXIS), UI_G32(S_VAXIS + 4));
        if (UI_G32(S_VSCROLL) > m) UI_G32(S_VSCROLL) = m;
    }
    if (UI_G32(S_VSCROLL) < 0) UI_G32(S_VSCROLL) = 0;
}
static void fp_notes(Footprint& f, PaintKitCanvas*, Edx) { f.replay_only = "adds notifications (allocates)"; }
PORT_FN(0x004c9df0, "PaintKitCanvas::Create", PaintKitCanvas_Create_n, fp_notes)

static void __fastcall PaintKitCanvas_Callback_n(PaintKitCanvas* self, Edx, int32_t, const void*) { cc_dirty(self); }
static void fp_PaintKitCanvas_Callback(Footprint& f, PaintKitCanvas* self, Edx, int32_t, const void*) { fp_widget(f, self); }
PORT_FN(0x004c9ef0, "PaintKitCanvas::Callback", PaintKitCanvas_Callback_n, fp_PaintKitCanvas_Callback)
static void __fastcall PaintKitCanvas_Over_n(PaintKitCanvas* self, Edx) {
    cc_dirty(self);
    self->over = 1;
}
static void __fastcall PaintKitCanvas_NotOver_n(PaintKitCanvas* self, Edx) {
    cc_dirty(self);
    self->over = 0;
}
static void fp_PaintKitCanvas_over(Footprint& f, PaintKitCanvas* self, Edx) {
    f.add(self, sizeof *self, "this");
    fp_widget(f, self);
}
PORT_FN(0x004c9f00, "PaintKitCanvas::Over", PaintKitCanvas_Over_n, fp_PaintKitCanvas_over)
PORT_FN(0x004c9f20, "PaintKitCanvas::NotOver", PaintKitCanvas_NotOver_n, fp_PaintKitCanvas_over)

// PaintKitCanvas::Draw: the tool's preview (while the mouse is over it) drawn on the view, the view pasted zoomed and
// scrolled (zoom / width * 256: the view's 256 pixels fill the control's width at zoom 1)
static void __fastcall PaintKitCanvas_Draw_n(PaintKitCanvas* self, Edx, gxCanvas* c) {
    const float s = (float)(D(self->zoom) / D(self->w) * D(256.0f));
    set_canvas(&self->view);
    ccall<void>(F_gxRestoreClip, &self->view);
    if (self->over != 0) {
        PkMode* volatile m = mode_for(self);
        const int32_t my = self->my, mx = self->mx;
        mode_call(m, MO_Draw, mx, my);
    }
    set_canvas(c);
    const uint32_t sb = f_bits(s);
    const int32_t y = isub(self->y, UI_G32(S_VSCROLL));
    const int32_t x = isub(self->x, UI_G32(S_HSCROLL));
    ccall<void>(F_gxPasteZoom, &self->view, x, y, sb, sb);
}
static void fp_PaintKitCanvas_Draw(Footprint& f, PaintKitCanvas* self, Edx, gxCanvas* c) {
    if (self->over != 0) {
        fp_canvas_tool(f, self);
        if (f.replay_only) return;
    }
    f.add(self, sizeof *self, "this");
    fp_cur(f);
    fp_canvas(f, &self->view);
    fp_canvas(f, c);
}
PORT_FN(0x004c9f40, "PaintKitCanvas::Draw", PaintKitCanvas_Draw_n, fp_PaintKitCanvas_Draw)

// PaintKitCanvas::GetCursor: the tool's, or the cross
static void* __fastcall PaintKitCanvas_GetCursor_n(PaintKitCanvas* self, Edx) {
    PkMode* volatile m = mode_for(self);
    PkMode* p = m;
    typedef void*(__fastcall * Fn)(const void*, Edx);
    void* r = ((Fn)(uintptr_t)vt_entry(vt_of(p), MO_GetCursor))(p, 0);
    if (!r) r = self->cursor;
    return r;
}
static void fp_PaintKitCanvas_GetCursor(Footprint& f, PaintKitCanvas* self, Edx) {
    if ((uint32_t)UI_G32(S_TOOL) > 8u) { f.replay_only = "an unknown tool: LogPanic, then a call through a null tool"; return; }
    if (!mode_size(mode_for(self))) f.replay_only = "a tool of a class outside the paint kit";
}
PORT_FN(0x004ca060, "PaintKitCanvas::GetCursor", PaintKitCanvas_GetCursor_n, fp_PaintKitCanvas_GetCursor)

// the mouse in the painting's pixels: (the mouse - the control + the scroll) / the zoom, each through __ftol
static __forceinline int32_t to_paint(int32_t v, int32_t z) { return x87_ftol(D(v) / D(z)); }

// PaintKitCanvas::MouseDown / MouseUp / MouseRDown / MouseRUp: the point handed to the tool; a button held or let go
static __forceinline void canvas_button(PaintKitCanvas* self, int32_t x, int32_t y, uint32_t off, uint8_t down) {
    const int32_t ox = self->x;
    const int32_t z = self->zoom;
    x = isub(x, ox);
    const int32_t tx = iadd(UI_G32(S_HSCROLL), x);
    y = isub(y, self->y);
    const int32_t px = to_paint(tx, z);
    const int32_t ty = iadd(UI_G32(S_VSCROLL), y);
    const int32_t py = to_paint(ty, z);
    self->mx = px;
    self->my = py;
    mode_call(mode_for(self), off, px, py);
    self->down = down;
}
static void __fastcall PaintKitCanvas_MouseDown_n(PaintKitCanvas* self, Edx, int32_t x, int32_t y) { canvas_button(self, x, y, MO_MouseDown, 1); }
static void __fastcall PaintKitCanvas_MouseUp_n(PaintKitCanvas* self, Edx, int32_t x, int32_t y) { canvas_button(self, x, y, MO_MouseUp, 0); }
static void __fastcall PaintKitCanvas_MouseRDown_n(PaintKitCanvas* self, Edx, int32_t x, int32_t y) { canvas_button(self, x, y, MO_MouseRDown, 1); }
// ... MouseRUp keeps the mouse's control coordinates, not the painting's, as the last point (the original's quirk)
static void __fastcall PaintKitCanvas_MouseRUp_n(PaintKitCanvas* self, Edx, int32_t x, int32_t y) {
    const int32_t ox = self->x;
    self->mx = x;
    const int32_t y0 = y;
    x = isub(x, ox);
    const int32_t z = self->zoom;
    const int32_t oy = self->y;
    self->my = y0;
    y = isub(y, oy);
    const int32_t tx = iadd(UI_G32(S_HSCROLL), x);
    const int32_t px = to_paint(tx, z);
    const int32_t ty = iadd(UI_G32(S_VSCROLL), y);
    const int32_t py = to_paint(ty, z);
    mode_call(mode_for(self), MO_MouseRUp, px, py);
    self->down = 0;
}
static void fp_canvas_mouse(Footprint& f, PaintKitCanvas* self, Edx, int32_t, int32_t) { fp_canvas_tool(f, self); }
PORT_FN(0x004ca100, "PaintKitCanvas::MouseDown", PaintKitCanvas_MouseDown_n, fp_canvas_mouse)
PORT_FN(0x004ca230, "PaintKitCanvas::MouseUp", PaintKitCanvas_MouseUp_n, fp_canvas_mouse)
PORT_FN(0x004ca360, "PaintKitCanvas::MouseRDown", PaintKitCanvas_MouseRDown_n, fp_canvas_mouse)
PORT_FN(0x004ca490, "PaintKitCanvas::MouseRUp", PaintKitCanvas_MouseRUp_n, fp_canvas_mouse)

// PaintKitCanvas::MouseMove: the point kept; a drag while a button is held; the tool's MouseMove
static void __fastcall PaintKitCanvas_MouseMove_n(PaintKitCanvas* self, Edx, int32_t x, int32_t y) {
    const int32_t ox = self->x;
    const int32_t z = self->zoom;
    x = isub(x, ox);
    const int32_t tx = iadd(UI_G32(S_HSCROLL), x);
    y = isub(y, self->y);
    const int32_t px = to_paint(tx, z);
    const int32_t ty = iadd(UI_G32(S_VSCROLL), y);
    const int32_t py = to_paint(ty, z);
    self->mx = px;
    self->my = py;
    if (self->down != 0) mode_call(mode_for(self), MO_MouseDrag, px, py);
    mode_call(mode_for(self), MO_MouseMove, px, py);
}
PORT_FN(0x004ca5d0, "PaintKitCanvas::MouseMove", PaintKitCanvas_MouseMove_n, fp_canvas_mouse)

// PaintKitCanvas::DirtyRect
static void __fastcall PaintKitCanvas_DirtyRect_n(PaintKitCanvas* self, Edx, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    dirty_rect(self, x0, y0, x1, y1);
}
static void fp_PaintKitCanvas_DirtyRect(Footprint& f, PaintKitCanvas* self, Edx, int32_t, int32_t, int32_t, int32_t) {
    f.add(self, sizeof *self, "this");
    fp_widget(f, self);
}
PORT_FN(0x004ca7a0, "PaintKitCanvas::DirtyRect", PaintKitCanvas_DirtyRect_n, fp_PaintKitCanvas_DirtyRect)

// PaintKitCanvas::Update: the right mouse... key 0xfe held turns on the eye dropper (tool 6) while no button is down, and
// back; the dirty rectangle of the painting (and a viper's template) copied to the view and into the car's texture
static void __fastcall PaintKitCanvas_Update_n(PaintKitCanvas* self, Edx) {
    struct { gxCanvas tex; } fr;
    if (UI_G32(S_TOOL) != 5 && UI_G32(S_TOOL) != 6) UI_G32(S_SAVED_TOOL) = UI_G32(S_TOOL);
    if (ccall<uint8_t>(F_ScanDown, (uint32_t)0xfe)) {
        if (self->down == 0 && UI_G32(S_TOOL) != 7) UI_G32(S_TOOL) = 6;
    } else if (self->down == 0 && UI_G32(S_TOOL) == 6) {
        UI_G32(S_TOOL) = UI_G32(S_SAVED_TOOL);
    }
    if (self->dirty == 0) return;
    {
        const int32_t y1 = self->dy1, x1 = self->dx1, y0 = self->dy0, x0 = self->dx0;
        ccall<void>(F_gxSetClip, &self->view, x0, y0, x1, y1);
    }
    set_canvas(&self->view);
    paste(&self->canvas, 0, 0);
    if (UI_G8(S_IS_VIPER) != 0) paste_alpha(self->overlay, 0, 0);
    ccall<void>(F_gxRestoreClip, &self->view);
    if (ccall<uint8_t>(F_gxGrabTexture, UI_G32(S_TEXTURE), &fr.tex)) {
        set_canvas(&fr.tex);
        const int32_t y1 = self->dy1, x1 = self->dx1, y0 = self->dy0, x0 = self->dx0;
        ccall<void>(F_gxSetClip, &fr.tex, x0, y0, x1, y1);
        paste(&self->view, 0, 0);
        ccall<void>(F_gxRestoreClip, &fr.tex);
        ccall<void>(F_gxReleaseTexture);
    } else {
        UI_LogReport(PK_CP(0x00500dc4));                                                   // "Can't grab texture!"
    }
    self->dirty = 0;
    cc_dirty(self);
}
static void fp_PaintKitCanvas_Update(Footprint& f, PaintKitCanvas*, Edx) {
    f.replay_only = "copies into the car's texture (grabs and releases the renderer's surface)";
}
PORT_FN(0x004ca840, "PaintKitCanvas::Update", PaintKitCanvas_Update_n, fp_PaintKitCanvas_Update)

// =================================================================================================================================
// PaintCarViewer3D
// =================================================================================================================================
typedef void(__cdecl* MatMake_t)(void*, uint32_t);                  // MatrixMakeYaw / Pitch / Roll (float by its bits)
typedef void(__cdecl* MatConcat_t)(void*, const void*, const void*);
#define MatrixMakeYaw ((MatMake_t)(uintptr_t)F_MatrixMakeYaw)
#define MatrixMakePitch ((MatMake_t)(uintptr_t)F_MatrixMakePitch)
#define MatrixMakeRoll ((MatMake_t)(uintptr_t)F_MatrixMakeRoll)
#define MatrixConcat ((MatConcat_t)(uintptr_t)F_MatrixConcat)

// the car's frame: turned by -yaw about y (the yaw matrix written inline, fsin / fcos straight to floats), then by the
// pitch about x; its matrices in the original's local order (A the yaw and the concatenations, B the part matrix)
static __forceinline void viewer_turn(PaintCarViewer3D* self, double ang) {
    struct { volatile float a[9]; volatile uint32_t t; volatile float b[9]; } fr;
    const float c = x87_cos_f(ang);
    const float s = x87_sin_f(ang);
    fr.a[0] = c;
    fr.a[1] = 0.0f;
    fr.a[2] = -s;
    fr.a[3] = 0.0f;
    *(volatile uint32_t*)&fr.a[4] = 0x3f800000u;
    fr.a[5] = 0.0f;
    fr.a[6] = s;
    fr.a[7] = 0.0f;
    fr.a[8] = c;
    void* A = (void*)fr.a;
    void* B = (void*)fr.b;
    void* F = (void*)&self->body;
    MatrixMakePitch(B, 0);
    MatrixConcat(A, B, A);
    MatrixMakeRoll(B, 0);
    MatrixConcat(A, B, A);
    MatrixConcat(F, A, F);
    fr.t = f_bits(-(float)self->pitch);
    MatrixMakeYaw(A, 0);
    MatrixMakePitch(B, fr.t);
    MatrixConcat(A, A, B);
    MatrixMakeRoll(B, 0);
    MatrixConcat(A, A, B);
    MatrixConcat(F, F, A);
}

// PaintCarViewer3D::PaintCarViewer3D: the frame 4 units ahead, turned 120 degrees and tipped 25; the hand cursor
static PaintCarViewer3D* __fastcall PaintCarViewer3D_ctor_n(PaintCarViewer3D* self, Edx) {
    self->vtbl = PK_P(VT_UICustomControl);
    self->widget = 0;
    self->vtbl = PK_P(VT_PaintCarViewer3D);
    self->my = 0;
    self->mx = 0;
    self->drag = 0;
    volatile uint32_t* m = (volatile uint32_t*)&self->body;
    m[1] = 0;
    m[0] = 0x3f800000u;
    m[2] = 0; m[3] = 0; m[4] = 0x3f800000u; m[5] = 0; m[6] = 0; m[7] = 0; m[8] = 0x3f800000u; m[9] = 0; m[10] = 0;
    m[11] = 0x40800000u;                                                   // 4.0
    self->yaw_b = 0x40060a92u;                                             // 2.0944 (120 degrees)
    self->pitch_b = 0x3edf66f3u;                                           // 0.4363 (25 degrees)
    viewer_turn(self, *(const volatile double*)(uintptr_t)0x004dfd28);    // -2.094395086169243
    self->cursor = ccall<void*>(F_gxGetStamp, PK_CP(0x00500dd8));         // "c_hand.stp"
    return self;
}
static void fp_PaintCarViewer3D_ctor(Footprint& f, PaintCarViewer3D*, Edx) { f.replay_only = "loads a stamp"; }
PORT_FN(0x004cab70, "PaintCarViewer3D::PaintCarViewer3D", PaintCarViewer3D_ctor_n, fp_PaintCarViewer3D_ctor)

static void* __fastcall PaintCarViewer3D_GetCursor_n(PaintCarViewer3D* self, Edx) { return self->cursor; }
static void fp_viewer_pure(Footprint& f, PaintCarViewer3D*, Edx) { f.pure = true; }
PORT_FN(0x004cacf0, "PaintCarViewer3D::GetCursor", PaintCarViewer3D_GetCursor_n, fp_viewer_pure)
static void __fastcall PaintCarViewer3D_Draw_n(PaintCarViewer3D*, Edx, gxCanvas* c) { set_canvas(c); }
static void fp_PaintCarViewer3D_Draw(Footprint& f, PaintCarViewer3D*, Edx, gxCanvas*) { fp_cur(f); }
PORT_FN(0x004cad00, "PaintCarViewer3D::Draw", PaintCarViewer3D_Draw_n, fp_PaintCarViewer3D_Draw)

// PaintCarViewer3D::Draw3D: the car's model in its frame, from the paint kit's camera, lit and environment-mapped
static void __fastcall PaintCarViewer3D_Draw3D_n(PaintCarViewer3D* self, Edx) {
    {
        const int32_t h = self->h, y = self->y, w = self->w, x = self->x;
        ccall<void>(F_mrSetView, x, y, w, h, (uint32_t)0);
    }
    ccall<void>(F_mrModelSetClipDist, (uint32_t)0, (uint32_t)0x42c80000u);             // 0, 100
    ccall<void>(F_mrSetProjection, (uint32_t)0x3dcccccdu, (uint32_t)0x42c80000u, (uint32_t)0x3f9c61aau);   // 0.1, 100, 1.2217
    ccall<void>(F_mrSetCamera, PK_P(S_CAMERA));
    ccall<void>(F_mrPushState);
    ccall<void>(F_mrEnable, (int32_t)8);
    ccall<void>(F_mrEnable, (int32_t)1);
    ccall<void>(F_mrEnable, (int32_t)4);
    ccall<void>(F_mrModelDraw, UI_G32(S_MODEL), (const void*)&self->body);
    ccall<void>(F_mrModelEnvMap, (uint32_t)0);
    ccall<void>(F_mrPopState);
}
static void fp_PaintCarViewer3D_Draw3D(Footprint& f, PaintCarViewer3D*, Edx) { f.replay_only = "draws a model through the 3D renderer (its state, lists, DirectX)"; }
PORT_FN(0x004cad10, "PaintCarViewer3D::Draw3D", PaintCarViewer3D_Draw3D_n, fp_PaintCarViewer3D_Draw3D)

static void __fastcall PaintCarViewer3D_MouseDown_n(PaintCarViewer3D* self, Edx, int32_t x, int32_t y) {
    self->mx = x;
    self->my = y;
    self->drag = 1;
}
static void __fastcall PaintCarViewer3D_MouseUp_n(PaintCarViewer3D* self, Edx, int32_t, int32_t) { self->drag = 0; }
static void fp_viewer_xy(Footprint& f, PaintCarViewer3D* self, Edx, int32_t, int32_t) { f.add(self, sizeof *self, "this"); }
PORT_FN(0x004cadb0, "PaintCarViewer3D::MouseDown", PaintCarViewer3D_MouseDown_n, fp_viewer_xy)
PORT_FN(0x004cadd0, "PaintCarViewer3D::MouseUp", PaintCarViewer3D_MouseUp_n, fp_viewer_xy)

// PaintCarViewer3D::MouseMove: while dragging, the yaw and pitch step by the mouse's movement (pi / 512 a pixel), the yaw
// wrapped to -pi..pi and the pitch held to -pi/2..pi/2 (compared as their bits: signed above, unsigned below), and the
// frame made again
static void __fastcall PaintCarViewer3D_MouseMove_n(PaintCarViewer3D* self, Edx, int32_t x, int32_t y) {
    if (self->drag == 0) return;
    const float k = UI_GF(0x004dfd3c);                                     // 0.0061359233 (pi / 512)
    {
        volatile int32_t t = isub(x, self->mx);
        self->yaw = (float)(D(t) * D(k) + D(self->yaw));
        t = isub(y, self->my);
        const bool wrap_down = (int32_t)self->yaw_b > (int32_t)0x40490fdb;
        self->pitch = (float)(D(t) * D(k) + D(self->pitch));
        if (wrap_down) self->yaw = (float)(D(self->yaw) - D(UI_GF(0x004dfd44)));      // 2 pi
    }
    if ((uint32_t)self->yaw_b > 0xc0490fdbu) self->yaw = (float)(D(self->yaw) + D(UI_GF(0x004dfd44)));
    if ((int32_t)self->pitch_b > (int32_t)0x3fc90fdb) self->pitch_b = 0x3fc90fdbu;
    if ((uint32_t)self->pitch_b > 0xbfc90fdbu) self->pitch_b = 0xbfc90fdbu;
    volatile uint32_t* m = (volatile uint32_t*)&self->body;
    m[0] = 0x3f800000u; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 0x3f800000u; m[5] = 0; m[6] = 0; m[7] = 0;
    m[10] = 0; m[9] = 0; m[8] = 0x3f800000u;
    m[11] = 0x40800000u;
    viewer_turn(self, -D(self->yaw));
    self->mx = x;
    self->my = y;
}
PORT_FN(0x004cade0, "PaintCarViewer3D::MouseMove", PaintCarViewer3D_MouseMove_n, fp_viewer_xy)

static void __fastcall PaintCarViewer3D_Update_n(PaintCarViewer3D*, Edx) {}
PORT_FN(0x004cafc0, "PaintCarViewer3D::Update", PaintCarViewer3D_Update_n, fp_viewer_pure)

static void* __fastcall PaintCarViewer3D_vdd_n(PaintCarViewer3D* self, Edx, uint32_t fl) {
    void* c = self->cursor;
    self->vtbl = PK_P(VT_PaintCarViewer3D);
    ccall<void>(F_gxForgetStamp, c);
    self->vtbl = PK_P(VT_UICustomControl);
    if (fl & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
template <typename T> static void fp_ctl_dtor(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object (and forgets its stamps or canvases)"; }
PORT_FN(0x004cafd0, "PaintCarViewer3D::scalar deleting destructor", PaintCarViewer3D_vdd_n, fp_ctl_dtor<PaintCarViewer3D>)

// =================================================================================================================================
// ColorChooser, ColorIndicator, ColorBucket
// =================================================================================================================================
template <typename T> static void fp_ctl_notes(Footprint& f, T*, Edx) { f.replay_only = "adds notifications (allocates)"; }
template <typename T> static void fp_ctl_dirty(Footprint& f, T* self, Edx, int32_t, const void*) { fp_widget(f, self); }
template <typename T> static void fp_ctl_draw(Footprint& f, T*, Edx, gxCanvas* c) {
    fp_cur(f);
    fp_canvas(f, c);
}

// ColorChooser::Create: the hue watched; the palette made now
static void __fastcall ColorChooser_Create_n(ColorChooser* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, PK_P(S_HUE), (uint32_t)4);
    vcall<void>(self, CC_Callback, (int32_t)0, (const void*)0);
}
PORT_FN(0x004cb010, "ColorChooser::Create", ColorChooser_Create_n, fp_ctl_notes<ColorChooser>)

// ColorChooser::Callback: the palette: the hue, with colorbar.cvs's shades over it
static void __fastcall ColorChooser_Callback_n(ColorChooser* self, Edx, int32_t, const void*) {
    set_canvas(&self->canvas);
    ccall<void>(F_gxClear, UI_GU32(S_HUE));
    paste_alpha(self->bar, 0, 0);
    cc_dirty(self);
}
static void fp_ColorChooser_Callback(Footprint& f, ColorChooser* self, Edx, int32_t, const void*) {
    fp_cur(f);
    fp_canvas(f, &self->canvas);
    fp_widget(f, self);
}
PORT_FN(0x004cb030, "ColorChooser::Callback", ColorChooser_Callback_n, fp_ColorChooser_Callback)

// ColorChooser::MouseDown / MouseRDown: a click in the palette (x 17..47) takes its colour for the button; one in the hue
// strip (x 2..11) sets the hue (and the button's colour) and marks the strip there
static void __fastcall ColorChooser_MouseDown_n(ColorChooser* self, Edx, int32_t x, int32_t y) {
    const int32_t px = isub(x, self->x);
    const int32_t py = isub(y, self->y);
    set_canvas(&self->canvas);
    if (px >= 0x11 && px < 0x30) {
        UI_GU32(S_LEFT) = ccall<uint32_t>(F_gxGetPixel, px, py);
        return;
    }
    if (px >= 2 && px < 0xc) {
        const uint32_t c = ccall<uint32_t>(F_gxGetPixel, px, py);
        UI_GU32(S_HUE) = c;
        UI_GU32(S_LEFT) = c;
        UI_G32(S_HUE_Y) = py;
    }
}
static void __fastcall ColorChooser_MouseRDown_n(ColorChooser* self, Edx, int32_t x, int32_t y) {
    set_canvas(&self->canvas);
    const int32_t px = isub(x, self->x);
    const int32_t py = isub(y, self->y);
    if (px >= 0x11 && px < 0x30) {
        UI_GU32(S_RIGHT) = ccall<uint32_t>(F_gxGetPixel, px, py);
        return;
    }
    if (px >= 2 && px < 0xc) {
        const uint32_t c = ccall<uint32_t>(F_gxGetPixel, px, py);
        UI_GU32(S_HUE) = c;
        UI_GU32(S_RIGHT) = c;
        UI_G32(S_HUE_Y) = py;
    }
}
static void fp_ColorChooser_mouse(Footprint& f, ColorChooser*, Edx, int32_t, int32_t) {
    fp_cur(f);
    UI_FP(f, S_LEFT, 4, "the left colour");
    UI_FP(f, S_RIGHT, 4, "the right colour");
    UI_FP(f, S_HUE, 4, "the hue");
    UI_FP(f, S_HUE_Y, 4, "the hue's mark");
}
PORT_FN(0x004cb070, "ColorChooser::MouseDown", ColorChooser_MouseDown_n, fp_ColorChooser_mouse)
PORT_FN(0x004cb0e0, "ColorChooser::MouseRDown", ColorChooser_MouseRDown_n, fp_ColorChooser_mouse)

// ColorChooser::Draw: the palette, and the hue's mark (a white line across the strip)
static void __fastcall ColorChooser_Draw_n(ColorChooser* self, Edx, gxCanvas* c) {
    set_canvas(c);
    {
        const int32_t y = self->y, x = self->x;
        paste(&self->canvas, x, y);
    }
    if (UI_G32(S_HUE_Y) < 0) return;
    const int32_t y0 = self->y;
    const int32_t x = self->x;
    const int32_t y = iadd(y0, UI_G32(S_HUE_Y));
    const uint32_t white = UI_GU32(S_WHITE);
    ccall<void>(F_gxLine, iadd(x, 2), y, iadd(x, 0xc), y, white);
}
static void fp_ColorChooser_Draw(Footprint& f, ColorChooser* self, Edx, gxCanvas* c) { fp_ctl_draw(f, self, 0, c); }
PORT_FN(0x004cb150, "ColorChooser::Draw", ColorChooser_Draw_n, fp_ColorChooser_Draw)

static void* __fastcall ColorChooser_GetCursor_n(ColorChooser* self, Edx) { return self->cursor; }
static void fp_ColorChooser_pure(Footprint& f, ColorChooser*, Edx) { f.pure = true; }
PORT_FN(0x004cb1b0, "ColorChooser::GetCursor", ColorChooser_GetCursor_n, fp_ColorChooser_pure)

static void* __fastcall ColorChooser_sdd_n(ColorChooser* self, Edx, uint32_t fl) {
    gxCanvas* bar = self->bar;
    self->vtbl = PK_P(VT_ColorChooser);
    ccall<void>(F_gxCanvasForget, bar);
    ccall<void>(F_gxForgetStamp, (void*)self->cursor);
    ccall<void>(F_gxFreeCanvas, &self->canvas);
    self->vtbl = PK_P(VT_UICustomControl);
    if (fl & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x004cb1c0, "ColorChooser::vector deleting destructor", ColorChooser_sdd_n, fp_ctl_dtor<ColorChooser>)

// ColorIndicator: the two colours watched; the right one's square behind the left one's
static void __fastcall ColorIndicator_Create_n(ColorIndicator* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, PK_P(S_LEFT), (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, PK_P(S_RIGHT), (uint32_t)4);
}
PORT_FN(0x004cb210, "ColorIndicator::Create", ColorIndicator_Create_n, fp_ctl_notes<ColorIndicator>)
static void __fastcall ColorIndicator_Callback_n(ColorIndicator* self, Edx, int32_t, const void*) { cc_dirty(self); }
PORT_FN(0x004cb240, "ColorIndicator::Callback", ColorIndicator_Callback_n, fp_ctl_dirty<ColorIndicator>)
static void __fastcall ColorIndicator_Draw_n(ColorIndicator* self, Edx, gxCanvas* c) {
    const int32_t sw = imul(self->w, 3) / 4;
    const int32_t sh = imul(self->h, 3) / 4;
    set_canvas(c);
    {
        const int32_t y1 = iadd(self->y, self->h);
        const int32_t x1 = iadd(self->x, self->w);
        const uint32_t col = UI_GU32(S_RIGHT);
        ccall<void>(F_gxRect, isub(x1, sw), isub(y1, sh), x1, y1, col);
    }
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t col = UI_GU32(S_LEFT);
        ccall<void>(F_gxRect, x, y, iadd(sw, x), iadd(sh, y), col);
    }
}
PORT_FN(0x004cb250, "ColorIndicator::Draw", ColorIndicator_Draw_n, fp_ctl_draw<ColorIndicator>)
static void* __fastcall ColorIndicator_vdd_n(ColorIndicator* self, Edx, uint32_t fl) {
    self->vtbl = PK_P(VT_UICustomControl);
    if (fl & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
template <typename T> static void fp_ctl_dtor_vt(Footprint& f, T* self, Edx, uint32_t fl) {
    if (fl & 1) { f.replay_only = "frees the object"; return; }
    f.add(self, 4, "its vtable");
}
PORT_FN(0x004cb820, "ColorIndicator::vector deleting destructor", ColorIndicator_vdd_n, fp_ctl_dtor_vt<ColorIndicator>)

// ColorBucket: its colour watched; a click takes the left colour; a grey frame around its colour
static void __fastcall ColorBucket_Create_n(ColorBucket* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->var, (uint32_t)4);
}
PORT_FN(0x004cb2d0, "ColorBucket::Create", ColorBucket_Create_n, fp_ctl_notes<ColorBucket>)
static void __fastcall ColorBucket_Callback_n(ColorBucket* self, Edx, int32_t, const void*) { cc_dirty(self); }
PORT_FN(0x004cb2e0, "ColorBucket::Callback", ColorBucket_Callback_n, fp_ctl_dirty<ColorBucket>)
static void __fastcall ColorBucket_MouseDown_n(ColorBucket* self, Edx, int32_t, int32_t) {
    const uint32_t c = UI_GU32(S_LEFT);
    *self->var = c;
}
static void fp_ColorBucket_MouseDown(Footprint& f, ColorBucket* self, Edx, int32_t, int32_t) { f.add((void*)self->var, 4, "the bucket's colour"); }
PORT_FN(0x004cb2f0, "ColorBucket::MouseDown", ColorBucket_MouseDown_n, fp_ColorBucket_MouseDown)
static void __fastcall ColorBucket_Draw_n(ColorBucket* self, Edx, gxCanvas* c) {
    set_canvas(c);
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t grey = UI_GU32(S_GREY);
        const int32_t by = iadd(self->h, y);
        const int32_t bx = iadd(self->w, x);
        ccall<void>(F_gxRect, x, y, bx, by, grey);
    }
    {
        const int32_t x = self->x;
        volatile uint32_t* v = self->var;
        const int32_t y = self->y;
        const uint32_t col = *v;
        const int32_t by = isub(iadd(self->h, y), 1);
        const int32_t bx = isub(iadd(self->w, x), 1);
        ccall<void>(F_gxRect, iadd(x, 1), iadd(y, 1), bx, by, col);
    }
}
PORT_FN(0x004cb300, "ColorBucket::Draw", ColorBucket_Draw_n, fp_ctl_draw<ColorBucket>)
static void* __fastcall ColorBucket_vdd_n(ColorBucket* self, Edx, uint32_t fl) {
    self->vtbl = PK_P(VT_UICustomControl);
    if (fl & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x004cb360, "ColorBucket::vector deleting destructor", ColorBucket_vdd_n, fp_ctl_dtor_vt<ColorBucket>)

// =================================================================================================================================
// TemplateLayer, TemplatePreview
// =================================================================================================================================
// TemplateLayer::TemplateLayer: its bucket, the overlay, its canvas (no frame yet: -1)
static TemplateLayer* __fastcall TemplateLayer_ctor_n(TemplateLayer* self, Edx) {
    self->vtbl = PK_P(VT_UICustomControl);
    self->widget = 0;
    self->bucket.widget = 0;
    self->bucket.var = 0;
    self->vtbl = PK_P(VT_TemplateLayer);
    self->bucket.vtbl = PK_P(VT_UICustomControl);
    self->bucket.vtbl = PK_P(VT_ColorBucket);
    self->frame = -1;
    self->overlay = ccall<gxCanvas*>(F_gxCanvasGet, PK_CP(0x00500d44));
    ccall<void>(F_gxAllocCanvas, &self->canvas, (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    return self;
}
static void fp_TemplateLayer_ctor(Footprint& f, TemplateLayer*, Edx) { f.replay_only = "loads a canvas, allocates one"; }
PORT_FN(0x004cb380, "TemplateLayer::TemplateLayer", TemplateLayer_ctor_n, fp_TemplateLayer_ctor)

static __forceinline void template_layer_dtor(TemplateLayer* self) {
    self->vtbl = PK_P(VT_TemplateLayer);
    ccall<void>(F_gxFreeCanvas, &self->canvas);
    ccall<void>(F_gxCanvasForget, self->overlay);
    self->vtbl = PK_P(VT_UICustomControl);
    self->bucket.vtbl = PK_P(VT_UICustomControl);
}
static void __fastcall TemplateLayer_dtor_n(TemplateLayer* self, Edx) { template_layer_dtor(self); }
static void fp_TemplateLayer_dtor(Footprint& f, TemplateLayer*, Edx) { f.replay_only = "frees its canvas, forgets the overlay"; }
PORT_FN(0x004cb3e0, "TemplateLayer::~TemplateLayer", TemplateLayer_dtor_n, fp_TemplateLayer_dtor)

// TemplateLayer::Added: its bucket to its right, showing its colour
static void __fastcall TemplateLayer_Added_n(TemplateLayer* self, Edx) {
    struct { UIDialogItem it; UIDialogItem end; } fr;
    self->bucket.var = self->color;
    volatile uint32_t* d = (volatile uint32_t*)&fr.it;
    d[0] = 0x17;
    const int32_t x = iadd(iadd(self->x, self->w), 0x19);
    d[1] = 0;
    d[2] = (uint32_t)x;
    d[4] = 0x19;
    const int32_t y = iadd(self->y, 0xa);
    d[5] = 0x19;
    d[3] = (uint32_t)y;
    d[7] = 0;
    d[8] = pk_u(&self->bucket);
    d[9] = 0;
    d[10] = 0;
    d[11] = 0;
    d[12] = 0;
    d[13] = 0;
    d[6] = S_EMPTY;
    pk_item_end(&fr.end);
    tcall<void>(F_UICC_AddItems, self, &fr.it);
}
static void fp_TemplateLayer_Added(Footprint& f, TemplateLayer*, Edx) { f.replay_only = "builds a widget (_UIAddItems allocates)"; }
PORT_FN(0x004cb410, "TemplateLayer::Added", TemplateLayer_Added_n, fp_TemplateLayer_Added)

// TemplateLayer::MouseDown: its colour becomes the left one
static void __fastcall TemplateLayer_MouseDown_n(TemplateLayer* self, Edx, int32_t, int32_t) {
    const uint32_t c = UI_GU32(S_LEFT);
    *self->color = c;
}
static void fp_TemplateLayer_MouseDown(Footprint& f, TemplateLayer* self, Edx, int32_t, int32_t) { f.add((void*)self->color, 4, "the layer's colour"); }
PORT_FN(0x004cb4a0, "TemplateLayer::MouseDown", TemplateLayer_MouseDown_n, fp_TemplateLayer_MouseDown)

// TemplateLayer::Create: its colour, the background and the template shown watched
static void __fastcall TemplateLayer_Create_n(TemplateLayer* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->color, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->bg, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, PK_P(S_TEMPLATE), (uint32_t)4);
}
PORT_FN(0x004cb4b0, "TemplateLayer::Create", TemplateLayer_Create_n, fp_ctl_notes<TemplateLayer>)
static void __fastcall TemplateLayer_Callback_n(TemplateLayer* self, Edx, int32_t, const void*) { cc_dirty(self); }
PORT_FN(0x004cb4f0, "TemplateLayer::Callback", TemplateLayer_Callback_n, fp_ctl_dirty<TemplateLayer>)

// TemplateLayer::Draw: its frame of the template made a brush in its colour, over the background (and a viper's overlay),
// drawn at a quarter size
static void __fastcall TemplateLayer_Draw_n(TemplateLayer* self, Edx, gxCanvas* c) {
    struct { gxCanvas tmp; } fr;
    // FIX CANDIDATE: the template shown indexes the stamps unchecked (none found: template_cb's frame and prev_template's
    // -1 read what is before or after them)
    void* stamp = UI_GP(void, S_TEMPLATES + (uint32_t)UI_G32(S_TEMPLATE) * 4u);
    const int32_t fi = self->frame;
    if (fi < 0) return;
    if (!(ccall<int32_t>(F_gxStampCount, stamp) > fi)) return;
    {
        const int32_t fr_i = self->frame;
        const uint32_t col = *self->color;
        ccall<void>(F_gxMakeBrush, &self->canvas, stamp, col, fr_i);
    }
    ccall<void>(F_gxAllocCanvas, &fr.tmp, (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    set_canvas(&fr.tmp);
    ccall<void>(F_gxClear, (uint32_t)*self->bg);
    paste_alpha(&self->canvas, 0, 0);
    if (UI_G8(S_IS_VIPER) != 0) paste_alpha(self->overlay, 0, 0);
    set_canvas(c);
    {
        const int32_t y = self->y, x = self->x;
        ccall<void>(F_gxPasteZoom, &fr.tmp, x, y, (uint32_t)0x3e800000u, (uint32_t)0x3e800000u);    // 0.25
    }
    ccall<void>(F_gxFreeCanvas, &fr.tmp);
}
template <typename T> static void fp_template_draw(Footprint& f, T*, Edx, gxCanvas*) { f.replay_only = "allocates and frees a scratch canvas"; }
PORT_FN(0x004cb500, "TemplateLayer::Draw", TemplateLayer_Draw_n, fp_template_draw<TemplateLayer>)

static void* __fastcall TemplateLayer_vdd_n(TemplateLayer* self, Edx, uint32_t fl) {
    template_layer_dtor(self);
    if (fl & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x004cb5f0, "TemplateLayer::vector deleting destructor", TemplateLayer_vdd_n, fp_ctl_dtor<TemplateLayer>)

// TemplatePreview::Create: the background, the template shown and each layer's colour watched
static void __fastcall TemplatePreview_Create_n(TemplatePreview* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->bg, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, PK_P(S_TEMPLATE), (uint32_t)4);
    for (int32_t i = 0; i < self->count; i++) {
        const void* col = (const void*)((TemplateLayer*)((uint8_t*)self->layers + 0x68 * (uint32_t)i))->color;
        tcall<void>(F_UICC_AddNotification, self, (int32_t)0, col, (uint32_t)4);
    }
}
PORT_FN(0x004cb640, "TemplatePreview::Create", TemplatePreview_Create_n, fp_ctl_notes<TemplatePreview>)
static void __fastcall TemplatePreview_Callback_n(TemplatePreview* self, Edx, int32_t, const void*) { cc_dirty(self); }
PORT_FN(0x004cb690, "TemplatePreview::Callback", TemplatePreview_Callback_n, fp_ctl_dirty<TemplatePreview>)
// TemplatePreview::Added: nothing (an empty item list)
static void __fastcall TemplatePreview_Added_n(TemplatePreview* self, Edx) {
    struct { UIDialogItem end; } fr;
    pk_item_end(&fr.end);
    tcall<void>(F_UICC_AddItems, self, &fr.end);
}
static void fp_TemplatePreview_Added(Footprint& f, TemplatePreview*, Edx) { f.replay_only = "_UIAddItems (an empty list)"; }
PORT_FN(0x004cb6a0, "TemplatePreview::Added", TemplatePreview_Added_n, fp_TemplatePreview_Added)

// TemplatePreview::Draw: the layers, each a brush in its colour, over the background (and a viper's overlay), full size
static void __fastcall TemplatePreview_Draw_n(TemplatePreview* self, Edx, gxCanvas* c) {
    struct { volatile int32_t i; gxCanvas tmp; } fr;
    ccall<void>(F_gxAllocCanvas, &fr.tmp, (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    set_canvas(&fr.tmp);
    ccall<void>(F_gxClear, (uint32_t)*self->bg);
    fr.i = 0;
    for (uint32_t off = 0; self->count > fr.i; off += 0x68, fr.i = fr.i + 1) {
        TemplateLayer* l = (TemplateLayer*)((uint8_t*)self->layers + off);
        // FIX CANDIDATE: the template shown indexes the stamps unchecked (as TemplateLayer::Draw)
        void* stamp = UI_GP(void, S_TEMPLATES + (uint32_t)UI_G32(S_TEMPLATE) * 4u);
        if (l->frame < 0) continue;
        if (!(ccall<int32_t>(F_gxStampCount, stamp) > l->frame)) continue;
        set_canvas(&fr.tmp);
        const int32_t fi = l->frame;
        const uint32_t col = *l->color;
        ccall<void>(F_gxMakeBrush, &l->canvas, stamp, col, fi);
        paste_alpha(&l->canvas, 0, 0);
    }
    if (UI_G8(S_IS_VIPER) != 0) paste_alpha(self->overlay, 0, 0);
    set_canvas(c);
    {
        const int32_t y = self->y, x = self->x;
        paste(&fr.tmp, x, y);
    }
    ccall<void>(F_gxFreeCanvas, &fr.tmp);
}
PORT_FN(0x004cb6d0, "TemplatePreview::Draw", TemplatePreview_Draw_n, fp_template_draw<TemplatePreview>)

static void* __fastcall TemplatePreview_vdd_n(TemplatePreview* self, Edx, uint32_t fl) {
    gxCanvas* ov = self->overlay;
    self->vtbl = PK_P(VT_TemplatePreview);
    ccall<void>(F_gxCanvasForget, ov);
    self->vtbl = PK_P(VT_UICustomControl);
    self->bucket.vtbl = PK_P(VT_UICustomControl);
    if (fl & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x004cb7e0, "TemplatePreview::vector deleting destructor", TemplatePreview_vdd_n, fp_ctl_dtor<TemplatePreview>)

// =================================================================================================================================
// DecalViewer
// =================================================================================================================================
// FIX: the set's name (a translation, "Paintkit:DecalSet:<name>") was copied into the viewer's 0x100 bytes for the title
// unbounded: one of 256 characters or more ran over the viewer's drag flag and scroll axis and on past the viewer (decal_cb's
// frame). It keeps its first 255 characters: it is only shown. A name that fits is copied as before.
static __forceinline void decal_name(DecalViewer* v, const DecalSet* s) {
    if (VP_FIX) uit::ui_copy_bounded(v->name, s->name, sizeof v->name);
    else crt_strcpy(v->name, s->name);
}
// the set shown: its name copied for the title, no decal picked, the scroll axis for its rows
static __forceinline void decal_show(DecalViewer* v) {
    DecalSet* s = &v->sets[UI_G32(S_DECAL_SET)];
    decal_name(v, s);
    v->sel = -1;
    v->axis.total = s->rows;
    // FIX CANDIDATE: a set whose decals are 0 high (a canvas smaller than its count) divides by zero
    v->axis.visible = v->h / s->h;
    v->axis.pos = 0;
    {
        const int32_t d = isub(v->axis.total, v->axis.visible);
        if (d < 0) v->axis.pos = d;
    }
    if (v->axis.pos < 0) v->axis.pos = 0;
    cc_dirty(v);
}
// DecalViewer::NextSet / PrevSet (the buttons): the next / previous set, wrapping
static uint8_t __cdecl DecalViewer_NextSet_n(int32_t) {
    DecalViewer* v = UI_GP(DecalViewer, S_DECAL_VIEWER);
    UI_G32(S_DECAL_SET) = iadd(UI_G32(S_DECAL_SET), 1);
    const int32_t n = UI_G32(S_DECAL_SET);
    if (!(v->count > n)) UI_G32(S_DECAL_SET) = 0;
    decal_show(UI_GP(DecalViewer, S_DECAL_VIEWER));
    return 0;
}
static uint8_t __cdecl DecalViewer_PrevSet_n(int32_t) {
    UI_G32(S_DECAL_SET) = isub(UI_G32(S_DECAL_SET), 1);
    if (UI_G32(S_DECAL_SET) < 0) UI_G32(S_DECAL_SET) = isub(UI_GP(DecalViewer, S_DECAL_VIEWER)->count, 1);
    decal_show(UI_GP(DecalViewer, S_DECAL_VIEWER));
    return 0;
}
static void fp_decal_set(Footprint& f, int32_t) {
    UI_FP(f, S_DECAL_SET, 4, "the decal set");
    if (DecalViewer* v = UI_GP(DecalViewer, S_DECAL_VIEWER)) {
        f.add(v, sizeof *v, "the decal viewer");
        fp_widget(f, v);
    }
}
PORT_FN(0x004cb840, "DecalViewer::NextSet", DecalViewer_NextSet_n, fp_decal_set)
PORT_FN(0x004cb910, "DecalViewer::PrevSet", DecalViewer_PrevSet_n, fp_decal_set)

// DecalViewer::Create: the pick and the scroll axis watched; each set's grid for the viewer's width; the set shown
// FIX CANDIDATE: the set shown (S_DECAL_SET, kept from the last time) isn't checked against the sets decal_cb read: a
// decals.tab with fewer rows than that, or none, shows a set never filled in (decal_cb's frame: a crash on its name)
static void __fastcall DecalViewer_Create_n(DecalViewer* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->sel, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->axis, (uint32_t)0xc);
    for (int32_t i = 0; self->count > i; i++) {
        DecalSet* s = &self->sets[i];
        // FIX CANDIDATE: a set whose decals are 0 wide (a damaged decals<n>.cvs) divides by zero, and one wider than the
        // viewer gives 0 columns, divided by next
        const int32_t cols = self->w / s->w;
        s->cols = cols;
        s->rows = iadd(s->count, isub(cols, 1)) / cols;
    }
    DecalSet* s = &self->sets[UI_G32(S_DECAL_SET)];
    decal_name(self, s);                                               // FIX: (decal_name) held to its 0x100 bytes
    self->sel = -1;
    const int32_t rows = s->rows;
    self->axis.total = rows;
    const int32_t vis = self->h / s->h;                                // FIX CANDIDATE: 0-high decals (as decal_show)
    self->axis.visible = vis;
    self->axis.pos = 0;
    if (isub(rows, vis) < 0) self->axis.pos = isub(rows, vis);
    if (self->axis.pos < 0) self->axis.pos = 0;
    cc_dirty(self);
}
static void fp_DecalViewer_Create(Footprint& f, DecalViewer*, Edx) { f.replay_only = "adds notifications (allocates)"; }
PORT_FN(0x004cb9d0, "DecalViewer::Create", DecalViewer_Create_n, fp_DecalViewer_Create)

static void __fastcall DecalViewer_Callback_n(DecalViewer* self, Edx, int32_t, const void*) { cc_dirty(self); }
static void fp_DecalViewer_Callback(Footprint& f, DecalViewer* self, Edx, int32_t, const void*) { fp_widget(f, self); }
PORT_FN(0x004cbac0, "DecalViewer::Callback", DecalViewer_Callback_n, fp_DecalViewer_Callback)

// DecalViewer::MouseUp: the pick at the mouse; a decal picked ends the dialog (-2, its OK)
static void __fastcall DecalViewer_MouseUp_n(DecalViewer* self, Edx, int32_t x, int32_t y) {
    vcall<void>(self, CC_MouseMove, x, y);
    self->drag = 0;
    if (self->sel >= 0) ccall<void>(F_WidgetExit, (int32_t)-2);
}
static void __fastcall DecalViewer_MouseDown_n(DecalViewer* self, Edx, int32_t x, int32_t y) {
    self->drag = 1;
    vcall<void>(self, CC_MouseMove, x, y);
}
static void fp_DecalViewer_button(Footprint& f, DecalViewer* self, Edx, int32_t, int32_t) {
    if ((uint32_t)(uintptr_t)self->vtbl != VT_DecalViewer) { f.replay_only = "a viewer of another class (its MouseMove)"; return; }
    f.add(self, sizeof *self, "this");
    UI_FP(f, uit::S_EXIT, 1, "widget.obj exit flag");
    UI_FP(f, uit::S_EXIT_CODE, 4, "widget.obj exit code");
}
PORT_FN(0x004cbad0, "DecalViewer::MouseUp", DecalViewer_MouseUp_n, fp_DecalViewer_button)
PORT_FN(0x004cbb10, "DecalViewer::MouseDown", DecalViewer_MouseDown_n, fp_DecalViewer_button)

// DecalViewer::MouseMove: while dragging, the decal under the mouse (in the grid, scrolled), or -1
static void __fastcall DecalViewer_MouseMove_n(DecalViewer* self, Edx, int32_t x, int32_t y) {
    if (self->drag == 0) return;
    const int32_t vx = self->x;
    self->sel = -1;
    if (x < vx) return;
    if (!(iadd(self->w, vx) > x)) return;
    DecalSet* s = &self->sets[UI_G32(S_DECAL_SET)];
    const int32_t h = s->h;
    // FIX CANDIDATE: a set's 0-high or 0-wide decals divide by zero (a damaged decals.tab / .cvs)
    const int32_t row = isub(iadd(y, imul(self->axis.pos, h)), self->y) / h;
    const int32_t rc = imul(row, s->cols);
    const int32_t col = isub(x, vx) / s->w;
    const int32_t k = iadd(col, rc);
    if (k < 0) return;
    if (!(s->count > k)) return;
    self->sel = k;
}
static void fp_DecalViewer_MouseMove(Footprint& f, DecalViewer* self, Edx, int32_t, int32_t) { f.add((void*)&self->sel, 4, "the pick"); }
PORT_FN(0x004cbb30, "DecalViewer::MouseMove", DecalViewer_MouseMove_n, fp_DecalViewer_MouseMove)

// DecalViewer::Draw: the set's decals in a grid from the scroll position, each clipped to its cell, the picked one on
// yellow
static void __fastcall DecalViewer_Draw_n(DecalViewer* self, Edx, gxCanvas* c) {
    struct { volatile int32_t col, k; } fr;
    int32_t by = self->y;
    int32_t bx = self->x;
    set_canvas(c);
    DecalSet* s = &self->sets[UI_G32(S_DECAL_SET)];
    by = isub(by, imul(self->axis.pos, s->h));
    fr.col = 0;
    fr.k = 0;
    if (!(s->count > 0)) return;
    do {
        const int32_t vy = self->y;
        if (by >= vy) {
            const int32_t bot = iadd(s->h, by);
            if (iadd(self->h, vy) > bot) {
                ccall<void>(F_gxSetClip, c, bx, by, iadd(s->w, bx), bot);
                if (self->sel == fr.k) {
                    const uint32_t yel = UI_GU32(S_YELLOW);
                    const int32_t b2 = iadd(s->h, by);
                    const int32_t r2 = iadd(s->w, bx);
                    ccall<void>(F_gxRect, bx, by, r2, b2, yel);
                }
                const int32_t py = isub(by, imul(s->h, fr.k));
                paste_alpha(s->canvas, bx, py);
                ccall<void>(F_gxRestoreClip, c);
            }
        }
        bx = iadd(bx, s->w);
        fr.col = fr.col + 1;
        if (!(s->cols > fr.col)) {
            bx = self->x;
            by = iadd(by, s->h);
            fr.col = 0;
        }
        fr.k = fr.k + 1;
    } while (s->count > fr.k);
}
static void fp_DecalViewer_Draw(Footprint& f, DecalViewer*, Edx, gxCanvas* c) {
    fp_cur(f);
    fp_canvas(f, c);
}
PORT_FN(0x004cbbb0, "DecalViewer::Draw", DecalViewer_Draw_n, fp_DecalViewer_Draw)

static void* __fastcall DecalViewer_sdd_n(DecalViewer* self, Edx, uint32_t fl) {
    self->vtbl = PK_P(VT_DecalViewer);
    for (int32_t i = 0; self->count > i; i++) ccall<void>(F_gxCanvasForget, self->sets[i].canvas);
    self->vtbl = PK_P(VT_UICustomControl);
    if (fl & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void fp_DecalViewer_sdd(Footprint& f, DecalViewer*, Edx, uint32_t) { f.replay_only = "forgets the sets' canvases (frees)"; }
PORT_FN(0x004cbcb0, "DecalViewer::scalar deleting destructor", DecalViewer_sdd_n, fp_DecalViewer_sdd)

// the four function-local Xlators' atexit destructors of PaintKitCanvas::Export / Import (inlined into export_cb /
// import_cb): bare `ret`s
static void __cdecl xl_export_error_n() {}
static void __cdecl xl_export_n() {}
static void __cdecl xl_import_error_n() {}
static void __cdecl xl_import_n() {}
static void fp_xl_pure(Footprint& f) { f.pure = true; }
PORT_FN(0x004cbd00, "PaintKitCanvas::Export::XL_ExportError (destructor helper)", xl_export_error_n, fp_xl_pure)
PORT_FN(0x004cbd10, "PaintKitCanvas::Export::XL_Export (destructor helper)", xl_export_n, fp_xl_pure)
PORT_FN(0x004cbd20, "PaintKitCanvas::Import::XL_ImportError (destructor helper)", xl_import_error_n, fp_xl_pure)
PORT_FN(0x004cbd30, "PaintKitCanvas::Import::XL_Import (destructor helper)", xl_import_n, fp_xl_pure)

#undef D
}  // namespace paint_kit
}  // namespace
