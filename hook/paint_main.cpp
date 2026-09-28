// paint_main.cpp -- M3 UI stage, step U4 (group C): the paint kit's dialog, its buttons and its files, rewritten faithfully
// (library `paintkit`, paintkit.obj; the canvas, tools and controls are paint_kit.cpp, tga.obj paint_tga.cpp).
//
//   PaintKitDo(car, paint job): paint_begin (the car's name kept, its texture made, its model and resources loaded, the
//   3D camera), paint_main (the dialog: the car in 3D, the painting, the palette, the tools, the brushes, the buttons),
//   paint_end. The buttons: save (the painting as the user's "paint\<car>.cvs" -- a viper's "paint\paint<n>.cvs" -- and
//   the car's texture as "<car>.tex" / "paint<n>.tex"), back (asks to save a changed painting), default (the car's own
//   painting), template (a dialog: a template's three layers coloured, painted over the painting), decals (a dialog: a
//   decal picked becomes the brush), undo, flip / mirror / rotate (the cut brush), zoom, import / export ("paint\
//   paint.tga"). PaintKitInstallPaintJobs (at start-up): the viper's paint jobs and each car's own paint made into
//   textures ("~paint<n>.tex", "~<car>.tex") where the game's data has none. write_tex: a .tex file (nine mip levels of a
//   256 x 256 16-bit canvas).
//
// Written from the v1.0 disassembly, as paint_kit.cpp. The dialogs' frames are the original's layout (the item lists,
// the controls built on the stack, the buffers), in structs whose offsets are asserted; an item the original builds with
// UIDialogItem's constructor is built with it; each Xlator's text is read after its refresh, where the original reads it.
//
// Footprints (main thread): every function here either runs a dialog (paint_main, the template and decal dialogs, the
// boxes the buttons open), reads or writes files (save, back, import, export, write_tex, the paint jobs), loads or frees
// (paint_begin / paint_end, default, rotate's gxRotate90) -- replay_only, the session replay is their check -- or writes
// a few bounded things: color_brush (a brush canvas), update_template_name / next / prev (the template shown and its
// number), undo_cb (the painting's three canvases), flip / mirror (the cut brush), zoom_cb (the zoom and the scroll
// axes), make_mip (its output).
//
// FIX CANDIDATEs (left faithful, marked in place): paint_begin copies the car's name into its 32-byte static unbounded
// (a name of 32 or more characters runs over the brush number and on); default_cb formats "<car>.cvs" into 0x20 bytes
// (28 or more characters); decal_cb reads decals.tab into a viewer of 64 sets with no bound (65 or more rows overrun
// the frame), divides by each set's decal count (a 0 faults) and shows the set last shown whether or not the table still
// has it (DecalViewer::Create); the save paths are formatted into 0x104 bytes (a user
// folder of ~240 characters); PaintKitInstallPaintJobs formats car names into 0x20 bytes (26 or more characters). No
// ordinary play reaches any of them: the car list's names are short, decals.tab has a few rows with counts.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "paint_kit.h"

namespace {
namespace paint_main {
using namespace pkit;
using uit::tcall; using uit::ccall; using uit::vcall; using uit::crt_strcpy; using uit::crt_copy; using uit::crt_strlen;
using uit::UIDialogItem; using uit::UIDialog; using uit::CustomWidget; using uit::Log_t; using uit::Sprintf_t; using uit::xlate;
using uit::S_GX_CANVAS; using uit::S_SCREEN_W; using uit::S_SCREEN_H;

// ---- calls --------------------------------------------------------------------------------------------------------------------
static __forceinline gxCanvas* set_canvas(const volatile void* c) { return ccall<gxCanvas*>(F_gxSetCanvas, (void*)c); }
static __forceinline void cc_dirty(const volatile void* ctl) { tcall<void>(F_UICC_Dirty, (const void*)ctl); }
static __forceinline void paste(const volatile void* c, int32_t x, int32_t y) { ccall<void>(F_gxPaste, (void*)c, x, y); }
static __forceinline void paste_alpha(const volatile void* c, int32_t x, int32_t y) { ccall<void>(F_gxPasteAlpha, (void*)c, x, y); }
static __forceinline const char* user_dir() { return ccall<const char*>(F_Win32GetUserDirectory); }
static __forceinline void dialog(UIDialog* d, const char* title, uint32_t bg, int32_t def, const void* items) {
    volatile uint32_t* v = (volatile uint32_t*)d;
    v[0] = pk_u(title);
    v[3] = pk_u(items);
    v[1] = bg;
    v[2] = (uint32_t)def;
    v[4] = 0;
}
// the whole painting dirty (the compiler's inlined DirtyRect(0, 0, 256, 256)), then the control
static __forceinline void dirty_all(PaintKitCanvas* g) {
    if (g->dirty == 0) {
        g->dirty = 1;
        g->dx0 = 0;
        g->dy0 = 0;
        g->dx1 = 0x100;
        g->dy1 = 0x100;
    } else {
        int32_t v = g->dx0;
        if (!(v < 0)) v = 0;
        g->dx0 = v;
        v = g->dy0;
        if (!(v < 0)) v = 0;
        g->dy0 = v;
        v = g->dx1;
        if (!(v > 0x100)) v = 0x100;
        g->dx1 = v;
        v = g->dy1;
        if (!(v > 0x100)) v = 0x100;
        g->dy1 = v;
    }
    cc_dirty(g);
}

static void fp_modal_i(Footprint& f, int32_t) { f.replay_only = "runs a dialog (modal: its own input loop), or reads or writes files"; }

// =================================================================================================================================
// PaintKitDo, paint_begin, paint_end
// =================================================================================================================================
static void __cdecl PaintKitDo_n(const char* car, int32_t paint) {
    ccall<void>(F_paint_begin, car);
    ccall<void>(F_paint_main, paint);
    ccall<void>(F_paint_end, car);
}
static void fp_PaintKitDo(Footprint& f, const char*, int32_t) { f.replay_only = "the paint kit: loads, runs its dialog, saves"; }
PORT_FN(0x004c62d0, "PaintKitDo", PaintKitDo_n, fp_PaintKitDo)

// paint_begin: the car's name kept (is it the viper?), its texture made, its resources and model loaded, the camera
static void __cdecl paint_begin_n(const char* car) {
    char buf[0x104];
    // FIX CANDIDATE: the name copied into its 0x20-byte static unbounded -- 32 characters or more run over the brush
    // number (0x500980) and the rest of the paint kit's statics (the car list's names are short: ordinary play can't)
    crt_strcpy((char*)PK_P(S_CAR), car);
    UI_G8(S_IS_VIPER) = (uint8_t)(ccall<int>(F_stricmp, PK_CP(S_CAR), PK_CP(0x005009dc)) == 0 ? 1 : 0);    // "viper"
    UI_sprintf(buf, PK_CP(0x005009e4), PK_CP(S_CAR));                                                    // "%s.tex"
    UI_G32(S_TEXTURE) = ccall<int32_t>(F_gxCreateTexture, (char*)buf, (int32_t)0, (int32_t)0x100);
    UI_sprintf(buf, PK_CP(0x005009ec), PK_CP(S_CAR));                                                    // "%s.car"
    ccall<void>(F_ResourceSetMustLoad, (const char*)buf);
    ccall<void>(F_ResourceSetMustLoad, PK_CP(0x005009f4));                                               // "paintkit.res"
    UI_sprintf(buf, PK_CP(0x00500a04), PK_CP(S_CAR));                                                    // "%s0.mod"
    UI_G32(S_MODEL) = ccall<int32_t>(F_mrModelLoad, (const char*)buf);
    volatile uint32_t* cam = (volatile uint32_t*)PK_P(S_CAMERA);
    cam[1] = 0;
    cam[0] = 0x3f800000u;
    cam[2] = 0;
    cam[3] = 0;
    cam[4] = 0x3f800000u;
    cam[5] = 0;
    cam[6] = 0;
    cam[7] = 0;
    cam[8] = 0x3f800000u;
    cam[9] = 0;
    cam[10] = 0;
    cam[11] = 0;
}
static void fp_paint_begin(Footprint& f, const char*) { f.replay_only = "makes a texture, loads resources and a model"; }
PORT_FN(0x004c6300, "paint_begin", paint_begin_n, fp_paint_begin)

// paint_end: the model and the resources unloaded, the texture destroyed
static void __cdecl paint_end_n(const char* car) {
    char buf[0x104];
    ccall<void>(F_mrModelUnload, UI_G32(S_MODEL));
    ccall<void>(F_ResourceSetUnload, PK_CP(0x00500a0c));                                                 // "paintkit.res"
    UI_sprintf(buf, PK_CP(0x00500a1c), car);                                                             // "%s.car"
    ccall<void>(F_ResourceSetUnload, (const char*)buf);
    ccall<void>(F_gxDestroyTexture, UI_G32(S_TEXTURE));
}
static void fp_paint_end(Footprint& f, const char*) { f.replay_only = "unloads a model and resources, destroys a texture"; }
PORT_FN(0x004c6430, "paint_end", paint_end_n, fp_paint_end)

// =================================================================================================================================
// paint_main: the dialog
// =================================================================================================================================
struct MainFrame {                                // the original's frame from F+8 (F: esp after its pushes)
    UIDialog dlg;                                 // F+0x08
    ColorChooser cc;                              // F+0x1c
    ColorIndicator ci;                            // F+0x60
    PaintCarViewer3D viewer;                      // F+0x78
    UIDialogItem items[33];                       // F+0xd8
    PaintKitCanvas canvas;                        // F+0x810
};
static_assert(offsetof(MainFrame, cc) == 0x1c - 8 && offsetof(MainFrame, viewer) == 0x78 - 8 && offsetof(MainFrame, items) == 0xd8 - 8 &&
              offsetof(MainFrame, canvas) == 0x810 - 8 && sizeof(MainFrame) == 0x96c - 8, "paint_main's frame");

static void __cdecl paint_main_n(int32_t paint) {
    MainFrame fr;
    pk_xl_once(S_MAIN_ONCE, 0x01, 0x005d4460, 0x00500a24, 0x004c85f0);                  // "Titles:PaintKit"
    pk_xl_once(S_MAIN_ONCE, 0x02, 0x005d44d0, 0x00500a34, 0x004c85e0);                  // "UI:Back"
    pk_xl_once(S_MAIN_ONCE, 0x04, 0x005d4310, 0x00500a3c, 0x004c85d0);                  // "PaintKit:Default"
    pk_xl_once(S_MAIN_ONCE, 0x08, 0x005d4280, 0x00500a50, 0x004c85c0);                  // "PaintKit:Template"
    pk_xl_once(S_MAIN_ONCE, 0x10, 0x005d4338, 0x00500a64, 0x004c85b0);                  // "PaintKit:ImportButton"
    pk_xl_once(S_MAIN_ONCE, 0x20, 0x005d4368, 0x00500a7c, 0x004c85a0);                  // "PaintKit:ExportButton"
    tcall<PaintCarViewer3D*>(F_PaintCarViewer3D_ctor, &fr.viewer);
    tcall<PaintKitCanvas*>(F_PaintKitCanvas_ctor, &fr.canvas, paint);
    // the ColorChooser and the ColorIndicator, their constructors inlined
    fr.cc.widget = 0;
    fr.cc.vtbl = PK_P(VT_ColorChooser);
    {
        gxCanvas* bar = ccall<gxCanvas*>(F_gxCanvasGet, PK_CP(0x00500d28));              // "colorbar.cvs"
        fr.cc.bar = bar;
        const int32_t h = bar->h, w = bar->w;
        ccall<void>(F_gxAllocCanvas, &fr.cc.canvas, w, h, (int32_t)5);
    }
    fr.cc.cursor = ccall<void*>(F_gxGetStamp, PK_CP(0x00500d38));                        // "c_drop.stp"
    fr.ci.widget = 0;
    fr.ci.vtbl = PK_P(VT_ColorIndicator);
    UIDialogItem* it = fr.items;
    const void* E = PK_P(S_EMPTY);
    pk_item(&it[0], 0x17, 0, 0xf, 0x6e, 0x100, 0x100, E, 0, &fr.viewer, 0);
    pk_item(&it[1], 0x17, 0, 0x131, 0x6e, 0x100, 0x100, E, 0, &fr.canvas, 0);
    pk_item(&it[2], 0x11, 0, 0x231, 0x6e, 0xa, 0x100, E, 0, PK_P(S_VAXIS), 0);
    pk_item(&it[3], 0x12, 0, 0x131, 0x16e, 0x100, 0xa, E, 0, PK_P(S_HAXIS), 0);
    pk_item(&it[4], 0x17, 0, 0x249, 0x6e, 0x32, 0xa2, E, 0, &fr.cc, 0);
    pk_item(&it[5], 0x17, 0, 0x252, 0x4b, 0x1e, 0x1e, E, 0, &fr.ci, 0);
    // the tools (radio stamps on the tool number)
    pk_item(&it[6], 0xd, 0, 0x14, 0x4b, 0, 0, PK_P(0x00500a94), 1, PK_P(S_TOOL), 0);     // brush.stp
    pk_item(&it[7], 0xd, 0, 0x2c, 0x4b, 0, 0, PK_P(0x00500aa0), 2, PK_P(S_TOOL), 0);     // line.stp
    pk_item(&it[8], 0xd, 0, 0x44, 0x4b, 0, 0, PK_P(0x00500aac), 3, PK_P(S_TOOL), 0);     // rect.stp
    pk_item(&it[9], 0xd, 0, 0x5f, 0x4b, 0, 0, PK_P(0x00500ab8), 5, PK_P(S_TOOL), 0);     // eyedrop.stp
    pk_item(&it[10], 0xd, 0, 0x77, 0x4b, 0, 0, PK_P(0x00500ac4), 7, PK_P(S_TOOL), 0);    // selrect.stp
    pk_item(&it[11], 0xd, 0, 0x8f, 0x4b, 0, 0, PK_P(0x00500ad0), 8, PK_P(S_TOOL), 0);    // zoom.stp
    // the brushes
    pk_item(&it[12], 0xd, 0, 0xaa, 0x4b, 0, 0, PK_P(0x00500adc), 1, PK_P(S_BRUSH), 0);   // tbrush0.stp
    pk_item(&it[13], 0xd, 0, 0xc2, 0x4b, 0, 0, PK_P(0x00500ae8), 2, PK_P(S_BRUSH), 0);   // tbrush1.stp
    pk_item(&it[14], 0xd, 0, 0xda, 0x4b, 0, 0, PK_P(0x00500af4), 3, PK_P(S_BRUSH), 0);   // tbrush2.stp
    pk_item(&it[15], 0xd, 0, 0xf2, 0x4b, 0, 0, PK_P(0x00500b00), 4, PK_P(S_BRUSH), 0);   // tbrush3.stp
    // the stamp buttons and the hot keys
    pk_item(&it[16], 3, 0, 0x10a, 0x4b, 0, 0, PK_P(0x00500b0c), 0, PK_P(F_decal_cb), 0);  // paste.stp
    pk_item(&it[17], 3, 0, 0x125, 0x4b, 0, 0, PK_P(0x00500b18), 0, PK_P(F_mirror_cb), 0); // mirror.stp
    pk_item(&it[18], 3, 0, 0x13d, 0x4b, 0, 0, PK_P(0x00500b24), 0, PK_P(F_flip_cb), 0);   // flip.stp
    pk_item(&it[19], 3, 1, 0x155, 0x4b, 0, 0, PK_P(0x00500b30), 0, PK_P(F_rotate_cb), 0); // rotl.stp
    pk_item(&it[20], 3, 0, 0x16d, 0x4b, 0, 0, PK_P(0x00500b3c), 0, PK_P(F_rotate_cb), 0); // rotr.stp
    pk_item(&it[21], 3, 0, 0x188, 0x4b, 0, 0, PK_P(0x00500b48), 0, PK_P(F_undo_cb), 0);   // undo.stp
    pk_item(&it[22], 4, 0, 0x1a, 0, 0, 0, 0, 0, PK_P(F_undo_cb), 0);                       // ^Z
    pk_item(&it[23], 4, 0, 0x7a, 0, 0, 0, 0, 0, PK_P(F_zoom_cb), 0);                       // z
    pk_item(&it[24], 2, -1, 0x13, 0x1a2, 0, 0, xlate(0x005d44d0), 0, PK_P(F_exit_cb), 6);
    pk_item(&it[25], 2, 0, 0x1e5, 0x1a5, 0, 0, xlate(0x005d4310), 0, PK_P(F_default_cb), 2);
    pk_item(&it[26], 2, 0, 0x154, 0x1a5, 0, 0, xlate(0x005d4280), 0, PK_P(F_template_cb), 2);
    pk_item(&it[27], 4, 0, 0x13, 0, 0, 0, 0, 0, PK_P(F_save_cb), 0);                       // ^S
    pk_item(&it[28], 2, 0, 0x64, 0x1ae, 0, 0, xlate(0x005d4338), 0, PK_P(F_import_cb), 0);
    pk_item(&it[29], 4, 0, 9, 0, 0, 0, 0, 0, PK_P(F_import_cb), 0);                        // ^I
    pk_item(&it[30], 2, 0, 0xc8, 0x1ae, 0, 0, xlate(0x005d4368), 0, PK_P(F_export_cb), 0);
    pk_item(&it[31], 4, 0, 0x18, 0, 0, 0, 0, 0, PK_P(F_export_cb), 0);                     // ^X
    pk_item_end(&it[32]);
    dialog(&fr.dlg, xlate(0x005d4460), 0x00500b54, -1, fr.items);                          // "paintkit.stp"
    {
        const int32_t h = UI_G32(S_SCREEN_H), w = UI_G32(S_SCREEN_W);
        ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
    }
    // the controls' destructors, inlined but for the canvas's
    fr.ci.vtbl = PK_P(VT_UICustomControl);
    fr.cc.vtbl = PK_P(VT_ColorChooser);
    ccall<void>(F_gxCanvasForget, fr.cc.bar);
    ccall<void>(F_gxForgetStamp, (void*)fr.cc.cursor);
    ccall<void>(F_gxFreeCanvas, &fr.cc.canvas);
    fr.cc.vtbl = PK_P(VT_UICustomControl);
    tcall<void>(F_PaintKitCanvas_dtor, &fr.canvas);
    fr.viewer.vtbl = PK_P(VT_PaintCarViewer3D);
    ccall<void>(F_gxForgetStamp, (void*)fr.viewer.cursor);
}
static void fp_paint_main(Footprint& f, int32_t) { f.replay_only = "runs the paint kit's dialog (modal: its own input loop)"; }
PORT_FN(0x004c6490, "paint_main", paint_main_n, fp_paint_main)

// color_brush: a brush canvas filled with a colour, its alpha kept
static void __cdecl color_brush_n(gxCanvas* c, uint32_t col) {
    set_canvas(c);
    ccall<void>(F_gxClearNoAlpha, col);
}
static void fp_color_brush(Footprint& f, gxCanvas* c, uint32_t) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}
PORT_FN(0x004c6d90, "color_brush", color_brush_n, fp_color_brush)

// =================================================================================================================================
// save, back (exit), default
// =================================================================================================================================
struct SaveFrame {                                // save_cb's and exit_cb's frame from F+0x10
    gxCanvas tex;                                 // F+0x10 the grabbed texture, then the .tex's 16-bit canvas
    char path[0x104];                             // F+0x34
};
static_assert(offsetof(SaveFrame, path) == 0x34 - 0x10 && sizeof(SaveFrame) == 0x138 - 0x10, "save_cb's frame");

// the save (the compiler's copy in save_cb and exit_cb): the whole painting to the view (a viper's shading over it) and
// into the car's texture; the painting written as "<user>paint\<car>.cvs" (a viper's "paint\paint<n>.cvs"), and if that
// worked the view as "<user><car>.tex" (a viper's "<user>paint<n>.tex")
static __forceinline void save_painting(SaveFrame& fr) {
    PaintKitCanvas* g = pk_g();
    dirty_all(g);
    {
        const int32_t y1 = g->dy1, x1 = g->dx1, y0 = g->dy0, x0 = g->dx0;
        ccall<void>(F_gxSetClip, &g->view, x0, y0, x1, y1);
    }
    set_canvas(&g->view);
    paste(&g->canvas, 0, 0);
    if (UI_G8(S_IS_VIPER) != 0) paste_alpha(g->overlay, 0, 0);
    ccall<void>(F_gxRestoreClip, &g->view);
    if (ccall<uint8_t>(F_gxGrabTexture, UI_G32(S_TEXTURE), &fr.tex)) {
        set_canvas(&fr.tex);
        const int32_t y1 = g->dy1, x1 = g->dx1, y0 = g->dy0, x0 = g->dx0;
        ccall<void>(F_gxSetClip, &fr.tex, x0, y0, x1, y1);
        paste(&g->view, 0, 0);
        ccall<void>(F_gxRestoreClip, &fr.tex);
        ccall<void>(F_gxReleaseTexture);
    } else {
        UI_LogReport(PK_CP(0x00500dc4));                                                   // "Can't grab texture!"
    }
    g->dirty = 0;
    cc_dirty(g);
    UI_sprintf(fr.path, PK_CP(0x00500e10), user_dir());                                    // "%spaint"
    ccall<uint8_t>(F_FileCreateDirectory, (const char*)fr.path);
    // FIX CANDIDATE: the paths are formatted into 0x104 bytes unbounded (a user folder of ~240 characters; the game takes
    // at most 200)
    const int32_t paint = g->paint;
    if (UI_G8(S_IS_VIPER) != 0) UI_sprintf(fr.path, PK_CP(0x00500d6c), user_dir(), paint);           // "%spaint\\paint%d.cvs"
    else UI_sprintf(fr.path, PK_CP(0x00500d5c), user_dir(), PK_CP(S_CAR));                            // "%spaint\\%s.cvs"
    if (!ccall<uint8_t>(F_gxCanvasWrite, &g->canvas, (const char*)fr.path)) {
        UI_LogReport(PK_CP(0x00500de4), (const char*)fr.path);                            // "Can't create %s"
        return;
    }
    g->modified = 0;
    if (UI_G8(S_IS_VIPER) != 0) {
        const int32_t p = g->paint;
        UI_sprintf(fr.path, PK_CP(0x00500e00), user_dir(), p);                             // "%spaint%d.tex"
    } else {
        UI_sprintf(fr.path, PK_CP(0x00500df4), user_dir(), PK_CP(S_CAR));                  // "%s%s.tex"
    }
    ccall<void>(F_gxAllocCanvas, &fr.tex, (int32_t)0x100, (int32_t)0x100, (int32_t)4);
    set_canvas(&fr.tex);
    paste(&g->view, 0, 0);
    ccall<uint8_t>(F_write_tex, &fr.tex, (const char*)fr.path);
    ccall<void>(F_gxFreeCanvas, &fr.tex);
}
static uint8_t __cdecl save_cb_n(int32_t) {
    SaveFrame fr;
    save_painting(fr);
    return 0;
}
PORT_FN(0x004c6db0, "save_cb", save_cb_n, fp_modal_i)

// exit_cb (back): a changed painting asks "save changes?" -- yes saves, cancel stays
static uint8_t __cdecl exit_cb_n(int32_t) {
    SaveFrame fr;
    pk_xl_once(S_EXIT_ONCE, 1, 0x005d4440, 0x00500b64, 0x004c73f0);                       // "Titles:FileChanged"
    pk_xl_once(S_EXIT_ONCE, 2, 0x005d4290, 0x00500b78, 0x004c73e0);                       // "PaintKit:SaveChangesQuery"
    if (pk_g()->modified == 0) return 1;
    const char* q = xlate(0x005d4290);
    const char* t = xlate(0x005d4440);
    const int32_t r = ccall<int32_t>(F_UIDoYesNoCancelBox, t, q);
    if (r == -2) {
        save_painting(fr);
        return 1;
    }
    if (r == -1) return 0;
    return 1;
}
PORT_FN(0x004c7060, "exit_cb", exit_cb_n, fp_modal_i)

// default_cb: "restore the default?" -- the car's own painting (as PaintKitCanvas::Default, inlined)
static uint8_t __cdecl default_cb_n(int32_t) {
    char buf[0x20];
    pk_xl_once(S_DEFAULT_ONCE, 1, 0x005d4358, 0x00500b94, 0x004c7620);                    // "Titles:RestoreDefault"
    pk_xl_once(S_DEFAULT_ONCE, 2, 0x005d42a8, 0x00500bac, 0x004c7610);                    // "PaintKit:AreYouSure"
    const char* q = xlate(0x005d42a8);
    const char* t = xlate(0x005d4358);
    if (!ccall<uint8_t>(F_UIDoYesNoBox, t, q, (const void*)0)) return 0;
    PaintKitCanvas* g = pk_g();
    // FIX CANDIDATE: the name formatted into 0x20 bytes (the frame's return address right after them): a car name of
    // 28 characters or more (as PaintKitCanvas::Default)
    if (UI_G8(S_IS_VIPER) != 0) UI_sprintf(buf, PK_CP(0x00500da4), g->paint);              // "paint%d.cvs"
    else UI_sprintf(buf, PK_CP(0x00500d9c), PK_CP(S_CAR));                                // "%s.cvs"
    if (ccall<uint8_t>(F_ResourceExists, (const char*)buf)) {
        gxCanvas* c = ccall<gxCanvas*>(F_gxCanvasGet, (const char*)buf);
        set_canvas(&g->canvas);
        ccall<void>(F_gxClear, UI_GU32(S_CLEAR));
        paste(c, 0, 0);
        ccall<void>(F_gxCanvasForget, c);
    } else {
        set_canvas(&g->canvas);
        ccall<void>(F_gxClear, UI_GU32(S_CLEAR));
    }
    dirty_all(g);
    set_canvas(&g->undo);
    paste(&g->canvas, 0, 0);
    g->modified = 1;
    return 0;
}
PORT_FN(0x004c7400, "default_cb", default_cb_n, fp_modal_i)

// =================================================================================================================================
// the template dialog
// =================================================================================================================================
struct TemplateFrame {                            // template_cb's frame from F+0x10
    UIDialog dlg;                                 // F+0x010
    TemplatePreview preview;                      // F+0x024
    ColorChooser cc;                              // F+0x068
    ColorBucket bg_small;                         // F+0x0ac (25 x 25)
    ColorBucket bg_big;                           // F+0x0c8 (64 x 64)
    ColorIndicator ci;                            // F+0x0e4
    char name[0x20];                              // F+0x0fc
    TemplateLayer layers[3];                      // F+0x11c
    UIDialogItem items[15];                       // F+0x254
};
static_assert(offsetof(TemplateFrame, cc) == 0x68 - 0x10 && offsetof(TemplateFrame, name) == 0xfc - 0x10 &&
              offsetof(TemplateFrame, layers) == 0x11c - 0x10 && offsetof(TemplateFrame, items) == 0x254 - 0x10 &&
              sizeof(TemplateFrame) == 0x59c - 0x10, "template_cb's frame");

// template_cb: the templates found ("templt<n>.stp", 32 at most), a preview and three layers (a template's three frames)
// with their colours; OK paints the layers over the painting (saved for undo first)
static uint8_t __cdecl template_cb_n(int32_t) {
    TemplateFrame fr;
    pk_xl_once(S_TEMPLATE_ONCE, 1, 0x005d43d8, 0x00500bc0, 0x004c7ec0);                   // "UI:Ok"
    pk_xl_once(S_TEMPLATE_ONCE, 2, 0x005d4270, 0x00500bc8, 0x004c7eb0);                   // "UI:Back"
    pk_xl_once(S_TEMPLATE_ONCE, 4, 0x005d4480, 0x00500bd0, 0x004c7ea0);                   // "Titles:Template"
    pk_xl_once(S_TEMPLATE_ONCE, 8, 0x005d42b8, 0x00500be0, 0x004c7e90);                   // "Paintkit:Preview"
    for (int32_t i = 0; i < 3; i++) tcall<TemplateLayer*>(F_TemplateLayer_ctor, &fr.layers[i]);
    // the function's statics, the first time: the background starts as the left colour, the layers' colours as three
    if (!(UI_G8(S_TEMPLATE_ONCE) & 0x10)) {
        UI_G8(S_TEMPLATE_ONCE) = (uint8_t)(UI_G8(S_TEMPLATE_ONCE) | 0x10);
        UI_GU32(S_BG) = UI_GU32(S_LEFT);
    }
    if (!(UI_G8(S_TEMPLATE_ONCE) & 0x20)) {
        const uint32_t a = UI_GU32(S_C_29C), b = UI_GU32(S_C_49C), c = UI_GU32(S_WHITE);
        UI_GU32(S_LAYER_COLORS) = a;
        UI_GU32(S_LAYER_COLORS + 4) = b;
        UI_GU32(S_LAYER_COLORS + 8) = c;
        UI_G8(S_TEMPLATE_ONCE) = (uint8_t)(UI_G8(S_TEMPLATE_ONCE) | 0x20);
    }
    // the controls, their constructors inlined
    volatile uint32_t* const bg = (volatile uint32_t*)PK_P(S_BG);
    fr.bg_big.widget = 0;
    fr.bg_big.vtbl = PK_P(VT_ColorBucket);
    fr.bg_big.var = bg;
    fr.bg_small.widget = 0;
    fr.bg_small.vtbl = PK_P(VT_ColorBucket);
    fr.bg_small.var = bg;
    fr.preview.widget = 0;
    fr.preview.bucket.widget = 0;
    fr.preview.bucket.vtbl = PK_P(VT_ColorBucket);
    fr.preview.bucket.var = bg;
    fr.preview.layers = fr.layers;
    fr.preview.count = 3;
    fr.preview.vtbl = PK_P(VT_TemplatePreview);
    fr.preview.bg = bg;
    fr.preview.overlay = ccall<gxCanvas*>(F_gxCanvasGet, PK_CP(0x00500d44));                // "shade.cvs"
    fr.cc.widget = 0;
    fr.cc.vtbl = PK_P(VT_ColorChooser);
    {
        gxCanvas* bar = ccall<gxCanvas*>(F_gxCanvasGet, PK_CP(0x00500d28));              // "colorbar.cvs"
        fr.cc.bar = bar;
        const int32_t h = bar->h, w = bar->w;
        ccall<void>(F_gxAllocCanvas, &fr.cc.canvas, w, h, (int32_t)5);
    }
    {
        void* cur = ccall<void*>(F_gxGetStamp, PK_CP(0x00500d38));                         // "c_drop.stp"
        fr.cc.cursor = cur;
    }
    fr.ci.widget = 0;
    UI_GP(TemplateLayer, S_LAYERS) = fr.layers;
    fr.ci.vtbl = PK_P(VT_ColorIndicator);
    UI_G32(S_NLAYERS) = 3;
    for (int32_t i = 0; i < 3; i++) {
        fr.layers[i].bg = bg;
        fr.layers[i].color = (volatile uint32_t*)PK_P(S_LAYER_COLORS + 4 * (uint32_t)i);
        fr.layers[i].frame = i;
    }
    // the templates
    UI_G32(S_NTEMPLATES) = 0;
    do {
        UI_sprintf(fr.name, PK_CP(0x00500bf4), UI_G32(S_NTEMPLATES));                       // "templt%d.stp"
        if (!ccall<uint8_t>(F_ResourceExists, (const char*)fr.name)) break;
        void* s = ccall<void*>(F_gxGetStamp, (const char*)fr.name);
        const int32_t n = UI_G32(S_NTEMPLATES);
        UI_G32(S_NTEMPLATES) = n + 1;
        UI_GP(void, S_TEMPLATES + 4 * (uint32_t)n) = s;
    } while (UI_G32(S_NTEMPLATES) < 0x20);
    ccall<void>(F_update_template_name);
    UIDialogItem* it = fr.items;
    const void* E = PK_P(S_EMPTY);
    pk_item(&it[0], 5, 0, 0x5a, 0x5f, 0, 0, PK_P(S_TEMPLATE_NAME), 0, 0, 0x15);
    pk_item(&it[1], 3, 0, 0x47, 0x60, 0, 0, PK_P(0x00500c04), 0, PK_P(F_prev_template), 0);    // lscroll.stp
    pk_item(&it[2], 3, 0, 0x88, 0x60, 0, 0, PK_P(0x00500c10), 0, PK_P(F_next_template), 0);    // rscroll.stp
    pk_item(&it[3], 0x17, 0, 0x50, 0x78, 0x40, 0x40, E, 0, &fr.bg_big, 0);
    pk_item(&it[4], 0x17, 0, 0xa9, 0x82, 0x19, 0x19, E, 0, &fr.bg_small, 0);
    pk_item(&it[5], 0x17, 0, 0x50, 0xba, 0x40, 0x40, E, 0, &fr.layers[0], 0);
    pk_item(&it[6], 0x17, 0, 0x50, 0xfc, 0x40, 0x40, E, 0, &fr.layers[1], 0);
    pk_item(&it[7], 0x17, 0, 0x50, 0x13e, 0x40, 0x40, E, 0, &fr.layers[2], 0);
    pk_item(&it[8], 5, 0, 0x13b, 0x5f, 0, 0, xlate(0x005d42b8), 0, 0, 0xc);
    pk_item(&it[9], 0x17, 0, 0x131, 0x6e, 0x100, 0x100, E, 0, &fr.preview, 0);
    pk_item(&it[10], 0x17, 0, 0x249, 0x6e, 0x32, 0xa2, E, 0, &fr.cc, 0);
    pk_item(&it[11], 0x17, 0, 0x252, 0x4b, 0x1e, 0x1e, E, 0, &fr.ci, 0);
    pk_item(&it[12], 2, -1, 0x13, 0x1a2, 0, 0, xlate(0x005d4270), 0, 0, 6);
    pk_item(&it[13], 2, -2, 0x1e5, 0x1a5, 0, 0, xlate(0x005d43d8), 0, 0, 2);
    pk_item_end(&it[14]);
    dialog(&fr.dlg, xlate(0x005d4480), 0x00500c1c, -1, fr.items);                          // "paintkit.stp"
    int32_t r;
    {
        const int32_t h = UI_G32(S_SCREEN_H), w = UI_G32(S_SCREEN_W);
        r = ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
    }
    if (r == -2) {
        PaintKitCanvas* g = pk_g();
        set_canvas(&g->undo);
        paste(&g->canvas, 0, 0);
        g->modified = 1;
        set_canvas(&pk_g()->canvas);
        ccall<void>(F_gxClear, UI_GU32(S_BG));
        for (TemplateLayer* l = fr.layers; l < fr.layers + 3; l++) {
            gxCanvas* const dst = &pk_g()->canvas;
            // FIX CANDIDATE: the template shown indexes the stamps unchecked (none found: 0 is the stale slot)
            void* stamp = UI_GP(void, S_TEMPLATES + (uint32_t)UI_G32(S_TEMPLATE) * 4u);
            const int32_t fi = l->frame;
            if (fi < 0) continue;
            if (!(ccall<int32_t>(F_gxStampCount, stamp) > fi)) continue;
            set_canvas(dst);
            const int32_t fi2 = l->frame;
            const uint32_t col = *l->color;
            ccall<void>(F_gxMakeBrush, &l->canvas, stamp, col, fi2);
            paste_alpha(&l->canvas, 0, 0);
        }
        dirty_all(pk_g());
    }
    for (int32_t i = 0; UI_G32(S_NTEMPLATES) > i; i++) ccall<void>(F_gxForgetStamp, UI_GP(void, S_TEMPLATES + 4 * (uint32_t)i));
    // the controls' destructors, inlined but for the layers'
    fr.ci.vtbl = PK_P(VT_UICustomControl);
    {
        gxCanvas* bar = fr.cc.bar;
        fr.cc.vtbl = PK_P(VT_ColorChooser);
        ccall<void>(F_gxCanvasForget, bar);
    }
    ccall<void>(F_gxForgetStamp, (void*)fr.cc.cursor);
    ccall<void>(F_gxFreeCanvas, &fr.cc.canvas);
    {
        gxCanvas* ov = fr.preview.overlay;
        fr.cc.vtbl = PK_P(VT_UICustomControl);
        fr.preview.vtbl = PK_P(VT_TemplatePreview);
        ccall<void>(F_gxCanvasForget, ov);
    }
    fr.preview.bucket.vtbl = PK_P(VT_UICustomControl);
    fr.preview.vtbl = PK_P(VT_UICustomControl);
    fr.bg_small.vtbl = PK_P(VT_UICustomControl);
    fr.bg_big.vtbl = PK_P(VT_UICustomControl);
    for (int32_t i = 2; i >= 0; i--) tcall<void>(F_TemplateLayer_dtor, &fr.layers[i]);
    return 0;
}
PORT_FN(0x004c7630, "template_cb", template_cb_n, fp_modal_i)

// update_template_name: the template's number (from 1) for the dialog's label
static void __cdecl update_template_name_n() { UI_sprintf((char*)PK_P(S_TEMPLATE_NAME), PK_CP(0x00500c2c), iadd(UI_G32(S_TEMPLATE), 1)); }
static void fp_update_template_name(Footprint& f) { UI_FP(f, S_TEMPLATE_NAME, 0x20, "the template's number"); }
PORT_FN(0x004c7e70, "update_template_name", update_template_name_n, fp_update_template_name)

// next_template / prev_template (the dialog's arrows): wrapping
static uint8_t __cdecl next_template_n(int32_t) {
    const int32_t n = UI_G32(S_NTEMPLATES);
    UI_G32(S_TEMPLATE) = iadd(UI_G32(S_TEMPLATE), 1);
    if (!(UI_G32(S_TEMPLATE) < n)) UI_G32(S_TEMPLATE) = 0;
    ccall<void>(F_update_template_name);
    return 0;
}
static uint8_t __cdecl prev_template_n(int32_t) {
    UI_G32(S_TEMPLATE) = isub(UI_G32(S_TEMPLATE), 1);
    if (UI_G32(S_TEMPLATE) < 0) UI_G32(S_TEMPLATE) = isub(UI_G32(S_NTEMPLATES), 1);
    ccall<void>(F_update_template_name);
    return 0;
}
static void fp_template_step(Footprint& f, int32_t) {
    UI_FP(f, S_TEMPLATE, 4, "the template shown");
    UI_FP(f, S_TEMPLATE_NAME, 0x20, "the template's number");
}
PORT_FN(0x004c8dc0, "next_template", next_template_n, fp_template_step)
PORT_FN(0x004c8df0, "prev_template", prev_template_n, fp_template_step)

// =================================================================================================================================
// undo, the brush's buttons, zoom
// =================================================================================================================================
// undo_cb: the painting and its undo copy swapped (through the scratch canvas); all of it dirty
static uint8_t __cdecl undo_cb_n(int32_t) {
    PaintKitCanvas* g = pk_g();
    set_canvas(&g->redo);
    paste(&g->canvas, 0, 0);
    set_canvas(&g->canvas);
    paste(&g->undo, 0, 0);
    set_canvas(&g->undo);
    paste(&g->redo, 0, 0);
    dirty_all(g);
    return 0;
}
static void fp_undo_cb(Footprint& f, int32_t) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    if (PaintKitCanvas* g = pk_g()) {
        f.add(g, sizeof *g, "the paint canvas");
        if (void* w = g->widget) f.add(w, sizeof(CustomWidget), "its widget");
        UI_FP_CANVAS(f, &g->canvas);
        UI_FP_CANVAS(f, &g->undo);
        UI_FP_CANVAS(f, &g->redo);
    }
}
PORT_FN(0x004c7ed0, "undo_cb", undo_cb_n, fp_undo_cb)

// flip_cb / mirror_cb / rotate_cb: the cut brush (brush 0) flipped, mirrored, turned left (id 0) or right (three lefts)
static uint8_t __cdecl flip_cb_n(int32_t) {
    if (UI_G32(S_BRUSH) == 0) ccall<void>(F_gxFlip, PK_P(S_BRUSH_CANVAS));
    return 0;
}
static uint8_t __cdecl mirror_cb_n(int32_t) {
    if (UI_G32(S_BRUSH) == 0) ccall<void>(F_gxMirror, PK_P(S_BRUSH_CANVAS));
    return 0;
}
// the cut brush's pixels: all of them (the canvas's constructor's 256 x 256 x 4; its size now is at most that)
static void fp_brush_pixels(Footprint& f, int32_t) {
    if (UI_G32(S_BRUSH) != 0) return;
    if (void* p = ((const gxCanvas*)PK_P(S_BRUSH_CANVAS))->pixels) f.add(p, 0x40000, "the cut brush's pixels");
}
PORT_FN(0x004c7fd0, "flip_cb", flip_cb_n, fp_brush_pixels)
PORT_FN(0x004c7ff0, "mirror_cb", mirror_cb_n, fp_brush_pixels)
static uint8_t __cdecl rotate_cb_n(int32_t id) {
    if (UI_G32(S_BRUSH) != 0) return 0;
    if (id == 0) {
        ccall<void>(F_gxRotate90, PK_P(S_BRUSH_CANVAS));
        return 0;
    }
    for (int32_t n = 3; n; n--) ccall<void>(F_gxRotate90, PK_P(S_BRUSH_CANVAS));
    return 0;
}
static void fp_rotate_cb(Footprint& f, int32_t) { f.replay_only = "gxRotate90 allocates and frees a scratch canvas"; }
PORT_FN(0x004c8010, "rotate_cb", rotate_cb_n, fp_rotate_cb)

// zoom_cb (z): the next zoom (1..4) around the painting's centre
static uint8_t __cdecl zoom_cb_n(int32_t) {
    UI_G32(S_ZOOM_CB) = iadd(UI_G32(S_ZOOM_CB), 1);
    if (UI_G32(S_ZOOM_CB) > 4) UI_G32(S_ZOOM_CB) = 1;
    const int32_t nz = UI_G32(S_ZOOM_CB);
    PaintKitCanvas* g = pk_g();
    volatile int32_t* zp = &g->zoom;
    if (*zp == 0) *zp = 1;
    const int32_t c = (int32_t)((uint32_t)*zp << 7);
    const int32_t ey = isub(c, UI_G32(S_VSCROLL));
    const int32_t ex = isub(c, UI_G32(S_HSCROLL));
    *zp = nz;
    UI_G32(S_HAXIS) = (int32_t)((uint32_t)nz << 8);
    UI_G32(S_VAXIS) = (int32_t)((uint32_t)*zp << 8);
    UI_G32(S_HAXIS + 4) = g->w;
    UI_G32(S_VAXIS + 4) = g->h;
    UI_G32(S_HSCROLL) = isub((int32_t)((uint32_t)*zp << 7), ex);
    {
        const int32_t m = isub(UI_G32(S_HAXIS), UI_G32(S_HAXIS + 4));
        if (UI_G32(S_HSCROLL) > m) UI_G32(S_HSCROLL) = m;
    }
    if (UI_G32(S_HSCROLL) < 0) UI_G32(S_HSCROLL) = 0;
    UI_G32(S_VSCROLL) = isub((int32_t)((uint32_t)*zp << 7), ey);
    {
        const int32_t m = isub(UI_G32(S_VAXIS), UI_G32(S_VAXIS + 4));
        if (UI_G32(S_VSCROLL) > m) UI_G32(S_VSCROLL) = m;
    }
    if (UI_G32(S_VSCROLL) < 0) UI_G32(S_VSCROLL) = 0;
    return 0;
}
static void fp_zoom_cb(Footprint& f, int32_t) {
    UI_FP(f, S_ZOOM_CB, 4, "zoom_cb's zoom");
    if (PaintKitCanvas* g = pk_g()) f.add((void*)&g->zoom, 4, "the zoom");
    UI_FP(f, S_VAXIS, 12, "the vertical scroll axis");
    UI_FP(f, S_HAXIS, 12, "the horizontal scroll axis");
}
PORT_FN(0x004c84b0, "zoom_cb", zoom_cb_n, fp_zoom_cb)

// =================================================================================================================================
// the decal dialog
// =================================================================================================================================
struct DecalFrame {                               // decal_cb's frame from F+0xc
    UIDialog dlg;                                 // F+0x00c
    UIDialogItem items[8];                        // F+0x020
    char buf[0x100];                              // F+0x1e0
    DecalViewer viewer;                           // F+0x2e0
};
static_assert(offsetof(DecalFrame, buf) == 0x1e0 - 0xc && offsetof(DecalFrame, viewer) == 0x2e0 - 0xc && sizeof(DecalFrame) == 0xb10 - 0xc,
              "decal_cb's frame");

// decal_cb (paste): decals.tab's sets ("decals<n>.cvs": the column 0's count of decals, one under another), a viewer;
// a decal picked becomes the brush (tool 1, brush 0)
static uint8_t __cdecl decal_cb_n(int32_t) {
    DecalFrame fr;
    pk_xl_once(S_DECAL_ONCE, 1, 0x005d4320, 0x00500c30, 0x004c84a0);                      // "Titles:Decals"
    pk_xl_once(S_DECAL_ONCE, 2, 0x005d42c8, 0x00500c40, 0x004c8490);                      // "UI:Ok"
    pk_xl_once(S_DECAL_ONCE, 4, 0x005d4410, 0x00500c48, 0x004c8480);                      // "UI:Cancel"
    DecalViewer* const v = &fr.viewer;
    v->vtbl = PK_P(VT_DecalViewer);
    v->widget = 0;
    v->axis.total = 0;
    v->axis.visible = 0;
    v->axis.pos = 0;
    UI_GP(DecalViewer, S_DECAL_VIEWER) = v;
    v->sel = -1;
    v->drag = 0;
    void* st = ccall<void*>(F_StringTableGet, PK_CP(0x00500e40));                           // "decals.tab"
    v->count = ccall<int32_t>(F_StringTableNumRows, (const void*)st);
    if (v->count > 0) {
        int32_t i = 0;
        do {
            // FIX CANDIDATE: no bound on the sets: a decals.tab of 65 rows or more overruns the viewer (the frame)
            DecalSet* s = &v->sets[i];
            UI_sprintf(fr.buf, PK_CP(0x00500e30), i);                                        // "decals%d.cvs"
            s->canvas = ccall<gxCanvas*>(F_gxCanvasGet, (const char*)fr.buf);
            s->count = ccall<int32_t>(F_atoi, ccall<const char*>(F_StringTableGetEntry, (const void*)st, i, (int32_t)0));
            const int32_t row = i;
            i++;
            s->w = s->canvas->w;
            // FIX CANDIDATE: a set of 0 decals (a damaged decals.tab) divides by zero
            s->h = s->canvas->h / s->count;
            const char* e = ccall<const char*>(F_StringTableGetEntry, (const void*)st, row, (int32_t)1);
            UI_sprintf(fr.buf, PK_CP(0x00500e18), e);                                        // "Paintkit:DecalSet:%s"
            s->name = ccall<const char*>(F_Xlate, (const char*)fr.buf);
        } while (v->count > i);
    }
    ccall<void>(F_StringTableForget, st);
    UIDialogItem* it = fr.items;
    const void* E = PK_P(S_EMPTY);
    pk_item(&it[0], 4, -1, 0x1b, 0, 0, 0, 0, 0, 0, 0);                                       // Esc
    pk_item(&it[1], 5, 0, 0x5a, 0x1e, 0, 0, v->name, 0, 0, 0xc);
    pk_item(&it[2], 3, 0, 0x14, 0x1e, 0, 0, PK_P(0x00500c54), 0, PK_P(F_DecalViewer_PrevSet), 0);    // lscroll.stp
    pk_item(&it[3], 3, 0, 0x118, 0x1e, 0, 0, PK_P(0x00500c60), 0, PK_P(F_DecalViewer_NextSet), 0);   // rscroll.stp
    pk_item(&it[4], 0x17, 0, 0x14, 0x32, 0x10e, 0x5a, E, 0, v, 0);
    pk_item(&it[5], 0x11, 0, 0x127, 0x32, 0x14, 0x5a, E, 0, (const void*)&v->axis, 0);
    pk_item(&it[6], 2, -1, 0x70, 0x99, 0, 0, xlate(0x005d4410), 0, 0, 0);
    pk_item_end(&it[7]);
    dialog(&fr.dlg, xlate(0x005d4320), 0x00500c6c, -2, fr.items);                           // "dialog1.stp"
    const int32_t r = ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, (int32_t)0x140, (int32_t)0xc8, (int32_t)-999, (int32_t)-999,
                                     (int32_t)1);
    if (r == -2) {
        DecalSet* s = &v->sets[UI_G32(S_DECAL_SET)];
        gxCanvas* src = s->canvas;
        const int32_t w = s->w;
        UI_G32(S_BRUSH_CANVAS + 8) = w;
        UI_G32(S_BRUSH_CANVAS + 0xc) = s->h;
        UI_G32(S_BRUSH_CANVAS + 0x10) = (int32_t)((uint32_t)w << 2);
        set_canvas(PK_P(S_BRUSH_CANVAS));
        paste(src, 0, (int32_t)(0u - (uint32_t)imul(UI_G32(S_BRUSH_CANVAS + 0xc), v->sel)));
        UI_G32(S_BRUSH) = 0;
        UI_G32(S_TOOL) = 1;
    }
    v->vtbl = PK_P(VT_DecalViewer);
    for (int32_t i = 0; v->count > i; i++) ccall<void>(F_gxCanvasForget, v->sets[i].canvas);
    return 0;
}
PORT_FN(0x004c8050, "decal_cb", decal_cb_n, fp_modal_i)

// =================================================================================================================================
// the paint jobs, .tex files, import / export
// =================================================================================================================================
struct InstallFrame {                             // PaintKitInstallPaintJobs's frame from F+0x10
    gxCanvas tex;                                 // F+0x10 the 16-bit canvas written
    gxCanvas cv;                                  // F+0x34 the painting with the shading
    char cvs[0x20];                               // F+0x58 "<car>.cvs"
    char tilde[0x20];                             // F+0x78 "~<car>.tex"
    char buf[0x104];                              // F+0x98
};
static_assert(offsetof(InstallFrame, cvs) == 0x58 - 0x10 && offsetof(InstallFrame, buf) == 0x98 - 0x10 && sizeof(InstallFrame) == 0x19c - 0x10,
              "PaintKitInstallPaintJobs's frame");

// PaintKitInstallPaintJobs: if any of the viper's nine "~paint<n>.tex" is missing, each missing one made from
// "paint<n>.cvs" with the shading (as "<user>paint<n>.tex"), and every car's own "<car>.cvs" without a "~<car>.tex" made
// into "<user><car>.tex"
static void __cdecl PaintKitInstallPaintJobs_n() {
    InstallFrame fr;
    bool missing = false;
    for (int32_t i = 0; i < 9; i++) {
        UI_sprintf(fr.buf, PK_CP(0x00500c78), i);                                           // "~paint%d.tex"
        if (!ccall<uint8_t>(F_ResourceExists, (const char*)fr.buf)) {
            missing = true;
            break;
        }
    }
    if (!missing) return;
    UI_LogReport(PK_CP(0x00500c88));                                                        // "Installing paint jobs..."
    ccall<void>(F_ResourceSetMustLoad, PK_CP(0x00500ca4));                                  // "paintkit.res"
    gxCanvas* shade = ccall<gxCanvas*>(F_gxCanvasGet, PK_CP(0x00500cb4));                   // "shade.cvs"
    ccall<void>(F_gxAllocCanvas, &fr.cv, (int32_t)0x100, (int32_t)0x100, (int32_t)5);
    for (int32_t i = 0; i < 9; i++) {
        UI_sprintf(fr.buf, PK_CP(0x00500cc0), i);                                           // "~paint%d.tex"
        if (ccall<uint8_t>(F_ResourceExists, (const char*)fr.buf)) continue;
        UI_sprintf(fr.buf, PK_CP(0x00500cd0), i);                                           // "paint%d.cvs"
        gxCanvas* c = ccall<gxCanvas*>(F_gxCanvasGet, (const char*)fr.buf);
        set_canvas(&fr.cv);
        paste(c, 0, 0);
        paste_alpha(shade, 0, 0);
        ccall<void>(F_gxAllocCanvas, &fr.tex, (int32_t)0x100, (int32_t)0x100, (int32_t)4);
        set_canvas(&fr.tex);
        paste(&fr.cv, 0, 0);
        UI_sprintf(fr.buf, PK_CP(0x00500cdc), user_dir(), i);                               // "%spaint%d.tex"
        ccall<uint8_t>(F_write_tex, &fr.tex, (const char*)fr.buf);
        ccall<void>(F_gxFreeCanvas, &fr.tex);
        ccall<void>(F_gxCanvasForget, c);
    }
    int32_t k = 0;
    if (ccall<int32_t>(F_GetMaxCarFileNames) > 0) {
        do {
            const char* car = ccall<const char*>(F_GetCarFileName, k);
            // FIX CANDIDATE: the car's name formatted into 0x20 bytes: "<car>.cvs" / "~<car>.tex" of 26 or more characters
            // run over (the car list's names are short: ordinary play can't)
            UI_sprintf(fr.cvs, PK_CP(0x00500cec), car);                                     // "%s.cvs"
            if (ccall<uint8_t>(F_ResourceExists, (const char*)fr.cvs)) {
                UI_sprintf(fr.tilde, PK_CP(0x00500cf4), car);                               // "~%s.tex"
                if (!ccall<uint8_t>(F_ResourceExists, (const char*)fr.tilde)) {
                    gxCanvas* c = ccall<gxCanvas*>(F_gxCanvasGet, (const char*)fr.cvs);
                    ccall<void>(F_gxAllocCanvas, &fr.tex, (int32_t)0x100, (int32_t)0x100, (int32_t)4);
                    set_canvas(&fr.tex);
                    paste(c, 0, 0);
                    UI_sprintf(fr.buf, PK_CP(0x00500cfc), user_dir(), car);                  // "%s%s.tex"
                    ccall<uint8_t>(F_write_tex, &fr.tex, (const char*)fr.buf);
                    ccall<void>(F_gxFreeCanvas, &fr.tex);
                    ccall<void>(F_gxCanvasForget, c);
                }
            }
            k++;
        } while (ccall<int32_t>(F_GetMaxCarFileNames) > k);
    }
    ccall<void>(F_gxCanvasForget, shade);
    ccall<void>(F_gxFreeCanvas, &fr.cv);
    ccall<void>(F_ResourceSetUnload, PK_CP(0x00500d08));                                     // "paintkit.res"
}
static void fp_install(Footprint& f) { f.replay_only = "loads resources and canvases, writes .tex files"; }
PORT_FN(0x004c8600, "PaintKitInstallPaintJobs", PaintKitInstallPaintJobs_n, fp_install)

// write_tex's file image: the header (12 bytes: no wrap, no de-res, 1, 0, nine mips) and the nine mip levels, smallest
// first, each 16-bit, the small ones padded to 32 bytes
struct TexImage {                                 // write_tex's frame from F+0x24
    volatile uint8_t b0, b1, b2, b3;              // F+0x24 0, 0, 0, 1
    volatile uint32_t zero;                       // F+0x28
    volatile uint32_t mips;                       // F+0x2c 9
    uint16_t m1[0x10];                            // F+0x30 1 x 1
    uint16_t m2[0x10];                            // F+0x50 2 x 2
    uint16_t m4[0x10];                            // F+0x70 4 x 4
    uint16_t m8[0x40];                            // F+0x90
    uint16_t m16[0x100];                          // F+0x110
    uint16_t m32[0x400];                          // F+0x310
    uint16_t m64[0x1000];                         // F+0xb10
    uint16_t m128[0x4000];                        // F+0x2b10
    uint16_t m256[0x10000];                       // F+0xab10
};
static_assert(sizeof(TexImage) == 0x2aaec && offsetof(TexImage, m256) == 0xab10 - 0x24, "write_tex's image");

// write_tex: a 256 x 256 16-bit canvas as a .tex file: "0SER", TEX_TYPE, TEX_VER; 0, "!IGM"; the image
static uint8_t __cdecl write_tex_n(gxCanvas* c, const char* name) {
    struct {
        volatile int32_t fd;                      // F+0xc
        volatile uint32_t zero, igm;              // F+0x10 (the second write)
        volatile uint32_t ser, type, ver;         // F+0x18 (the first)
        TexImage img;                             // F+0x24
    } fr;
    {
        volatile uint32_t* d = (volatile uint32_t*)&fr.img;
        for (uint32_t n = sizeof(TexImage) / 4; n; n--) *d++ = 0;
    }
    fr.img.b0 = 0;
    fr.img.b1 = 0;
    fr.img.b3 = 1;
    fr.img.mips = 9;
    ccall<void>(F_make_mip, fr.img.m256, c, (int32_t)0x100);
    ccall<void>(F_make_mip, fr.img.m128, c, (int32_t)0x80);
    ccall<void>(F_make_mip, fr.img.m64, c, (int32_t)0x40);
    ccall<void>(F_make_mip, fr.img.m32, c, (int32_t)0x20);
    ccall<void>(F_make_mip, fr.img.m16, c, (int32_t)0x10);
    ccall<void>(F_make_mip, fr.img.m8, c, (int32_t)8);
    ccall<void>(F_make_mip, fr.img.m4, c, (int32_t)4);
    ccall<void>(F_make_mip, fr.img.m2, c, (int32_t)2);
    ccall<void>(F_make_mip, fr.img.m1, c, (int32_t)1);
    fr.fd = ccall<int32_t>(F_FileCreate, name);
    if (fr.fd == 0) {
        UI_LogReport(PK_CP(0x00500d18), name);                                             // "Can't create %s"
        return 0;
    }
    fr.type = UI_GU32(S_TEX_TYPE);
    fr.ver = UI_GU32(S_TEX_VER);
    fr.zero = 0;
    fr.ser = 0x52455330u;                                                                   // "0SER"
    fr.igm = 0x4d474921u;                                                                   // "!IGM"
    ccall<uint8_t>(F_FileWrite, (int32_t)fr.fd, (const void*)&fr.ser, (int32_t)0xc);
    ccall<uint8_t>(F_FileWrite, (int32_t)fr.fd, (const void*)&fr.zero, (int32_t)8);
    ccall<uint8_t>(F_FileWrite, (int32_t)fr.fd, (const void*)&fr.img, (int32_t)0x2aaec);
    ccall<void>(F_FileClose, (int32_t*)&fr.fd);
    return 1;
}
static void fp_write_tex(Footprint& f, gxCanvas*, const char*) { f.replay_only = "writes a file"; }
PORT_FN(0x004c88d0, "write_tex", write_tex_n, fp_write_tex)

// make_mip: an n x n 16-bit copy of the canvas, point-sampled (pixel (c * 256 / n, r * 256 / n))
static void __cdecl make_mip_n(uint16_t* dst, const gxCanvas* c, int32_t n) {
    if (n <= 0) return;
    volatile uint16_t* d = dst;
    int32_t row = 0;
    for (int32_t k = n;;) {
        int32_t x = 0;
        const int32_t q = row / n;
        for (int32_t m = n; m; m--) {
            const int32_t sx = x / n;
            x = iadd(x, 0x100);
            const int32_t off = iadd(imul(c->pitch, q), imul(sx, 2));
            *d++ = *(const volatile uint16_t*)(c->pixels + off);
        }
        row = iadd(row, 0x100);
        if (--k == 0) break;
    }
}
static void fp_make_mip(Footprint& f, uint16_t* dst, const gxCanvas*, int32_t n) {
    if (n <= 0) return;
    if (n > 0x400) { f.replay_only = "a mip of more than 1024 x 1024"; return; }
    f.add(dst, (uint32_t)n * (uint32_t)n * 2u, "the mip");
}
PORT_FN(0x004c8a70, "make_mip", make_mip_n, fp_make_mip)

// export_cb: the painting (a viper's with its mask pasted onto it) as "<user>paint\paint.tga"; a box if that failed
static uint8_t __cdecl export_cb_n(int32_t) {
    char buf[0x104];
    PaintKitCanvas* g = pk_g();
    pk_xl_once(S_EXPORT_ONCE, 1, 0x005d5df0, 0x00500e84, 0x004cbd10);                     // "PaintKit:Export"
    pk_xl_once(S_EXPORT_ONCE, 2, 0x005d5de0, 0x00500e6c, 0x004cbd00);                     // "PaintKit:ExportError"
    {
        const int32_t paint = g->paint;
        UI_sprintf(buf, PK_CP(0x00500e58), user_dir(), paint);                            // "%spaint\\paint.tga" (the number unused)
    }
    gxCanvas* const cv = &g->canvas;
    set_canvas(cv);
    if (UI_G8(S_IS_VIPER) != 0) {
        gxCanvas* m = ccall<gxCanvas*>(F_gxCanvasGet, PK_CP(0x00500e4c));                 // "mask.cvs"
        paste_alpha(m, 0, 0);
        ccall<void>(F_gxCanvasForget, m);
    }
    if (!ccall<uint8_t>(F_WriteTGA, cv, (const char*)buf)) {
        const char* e = xlate(0x005d5de0);
        const char* t = xlate(0x005d5df0);
        ccall<void>(F_UIDoOkBox, t, e);
    }
    return 0;
}
PORT_FN(0x004c8af0, "export_cb", export_cb_n, fp_modal_i)

// import_cb: the painting saved for undo, then read from "<user>paint\paint.tga" (all of it dirty); a box if that failed
static uint8_t __cdecl import_cb_n(int32_t) {
    char buf[0x104];
    PaintKitCanvas* g = pk_g();
    pk_xl_once(S_IMPORT_ONCE, 1, 0x005d5dd0, 0x00500eac, 0x004cbd30);                     // "PaintKit:Import"
    pk_xl_once(S_IMPORT_ONCE, 2, 0x005d5dc0, 0x00500e94, 0x004cbd20);                     // "PaintKit:ImportError"
    {
        const int32_t paint = g->paint;
        UI_sprintf(buf, PK_CP(0x00500e58), user_dir(), paint);                            // "%spaint\\paint.tga"
    }
    set_canvas(&g->undo);
    paste(&g->canvas, 0, 0);
    g->modified = 1;
    if (ccall<uint8_t>(F_ReadTGA, &g->canvas, (const char*)buf)) {
        dirty_all(g);
        g->modified = 1;
        return 0;
    }
    const char* e = xlate(0x005d5dc0);
    const char* t = xlate(0x005d5dd0);
    ccall<void>(F_UIDoOkBox, t, e);
    return 0;
}
PORT_FN(0x004c8c10, "import_cb", import_cb_n, fp_modal_i)

}  // namespace paint_main
}  // namespace
