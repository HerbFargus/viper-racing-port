// edit_tool.cpp -- M3 UI stage, step U5 (group A): the model tool's entry point, its dialog, its buttons and files, rewritten
// faithfully (library `edit`: edit.obj's EditMenu, modtool.obj; the two views and the maths are edit_view.cpp, the $E
// initialisers edit_leftover.cpp, the ModBuilder and the 3D Studio / DXF code group B's edit_build.cpp / edit_3ds.cpp).
//
//   EditMenu (the main menu's hidden Ctrl+E): ModTool("model.mod") -- the name is never used. ModTool: every car's resources,
//   race.res and modtool.res loaded; the two mrModelInfos, the ModBuilder and its undo copy, the model the 3D view draws; the
//   dialog ("Texture Tool": the 3D view and its modes, the texture view and its tools and locks, the zoom slider, the
//   buttons: surfaces, surface properties, translate, scale, export, import, open, save, exit, and hot keys); everything
//   freed and unloaded after. idle_func (each frame): the model's frame from the slider. rebuild_models (after every edit):
//   the surface's texture into the texture view, the title, the model's mrModelInfo (ModBuilderBuildInfo) and the
//   overlay's (the surface's geometry, or the edges' wedges: build_edge_model), the 3D model built from one of them
//   (update_model). The buttons' dialogs: translate / scale (three numbers), surface properties (texture, material,
//   alpha, smoothing group; "make template" writes the texture triangles as a .bmp), the file boxes (open / save a .mod,
//   export a .3ds, import a .dxf or a .3ds), the "save changes?" box, capture (the surfaces' texture points as overlays, and
//   the model "*<file>" if one is found), measure (the model's size in inches).
//
// Written from the v1.0 disassembly, as edit_view.cpp. The dialogs' frames are the original's layout (the item lists, the
// text buffers, the scale table), in structs whose offsets are asserted; an item the original builds with UIDialogItem's
// constructor is built with it, the few it writes inline are written field by field.
//
// Footprints (main thread): every function here runs a dialog (modal), loads or frees (resources, models, canvases, the
// builders), reads or writes files, or reaches rebuild_models (it loads the surface's texture) -- replay_only, the session
// replay is its check -- except: zoom_cb (the zoom, the scroll axes, the texture view's zoom), idle_func (the 3D view's
// distance and the model's frame), update_texture_viewer (its widget), build_edge_model / copy_model_info (the info and
// its arrays, bounded by the counts), reset, toggle_cb, switch_overlay_cb (statics, the widget).
//
// FIX CANDIDATEs (left faithful, marked in place; none reached by ordinary editing):
//   copy_model_info / build_edge_model: the model's counts copied into arrays of 0x1800 vertices, 0x200 surfaces and 0x800
//     triangles with no bound -- a model of over 2048 triangles (the builder holds 4096) overruns the heap (an editor user
//     can reach it: import or open a big model);
//   build_edge_model: a triangle on a vertex the builder doesn't have uses the last good vertex (or the stack's garbage);
//   capture_cb: no bound on a surface's texture points (0x1000 an overlay) -- a surface of more overruns the next overlay
//     and, from the tenth, the statics after the texture view;
//   export_cb / import_cb: the file name and ".3ds" / ".mod" into 0x104 bytes (a name of 0x100 characters or more without a
//     dot overruns the frame / the statics after the name);
//   translate_cb: the units index the scale table unchecked (only the radio buttons set it: 0..2);
//   set_texture_cb leaves S_TEXNAME aimed at its frame (only browse_cb, inside the dialog, uses it).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "edit_types.h"

namespace {
namespace edit_tool {
using namespace edt;
using uit::tcall; using uit::ccall; using uit::crt_copy; using uit::crt_strcpy; using uit::crt_strlen; using uit::CustomWidget;
using uit::S_SCREEN_W; using uit::S_SCREEN_H;

#define D(x) ((double)(x))
static const float k_inch = 39.370079040527344f;  // 0x4df900 (0x421d7af6)
static const float k_five = 5.0f;                 // 0x4df8d0

// ---- calls --------------------------------------------------------------------------------------------------------------------
typedef void(__cdecl* Log_t)(const char*, ...);
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
typedef int(__cdecl* Sprintf2d_t)(char*, const char*, double, double);
typedef int(__cdecl* Sprintf3d_t)(char*, const char*, double, double, double);
#define ED_LogReport ((Log_t)(uintptr_t)F_LogReport)
#define ED_LogPanic ((Log_t)(uintptr_t)F_LogPanic)
#define ED_sprintf ((Sprintf_t)(uintptr_t)F_sprintf)
static __forceinline void* bld() { return UI_GP(void, S_BUILDER); }
static __forceinline uint8_t is_surface(int32_t s) { return ccall<uint8_t>(F_ModBuilderIsSurface, bld(), s); }
static __forceinline uint8_t get_tp(int32_t s, int32_t i, void* vtx, void* u, void* v) {
    return ccall<uint8_t>(F_ModBuilderGetTexturePoint, bld(), s, i, vtx, u, v);
}
static __forceinline uint8_t get_ttri(int32_t s, int32_t i, void* t, void* a, void* b, void* c) {
    return ccall<uint8_t>(F_ModBuilderGetTextureTriangle, bld(), s, i, t, a, b, c);
}
static __forceinline void cc_dirty(const volatile void* ctl) { tcall<void>(F_UICC_Dirty, (const void*)ctl); }
static __forceinline void rebuild() { ccall<void>(F_rebuild_models); }
static __forceinline gxCanvas* set_canvas(const volatile void* c) { return ccall<gxCanvas*>(F_gxSetCanvas, (void*)c); }
static __forceinline void mat_concat(void* d, const void* a, const void* b) { ccall<void>(F_MatrixConcat, d, a, b); }
static __forceinline uint8_t file_box(uint32_t fn, uint32_t title, uint32_t type, uint32_t pattern, char* buf, int32_t n, uint32_t dir) {
    return ccall<uint8_t>(fn, ED_CP(title), ED_CP(type), ED_CP(pattern), buf, n, ED_CP(dir));
}
// strcat's inline copy of a 5-byte suffix (".car", ".3ds", ".mod": a dword and its terminator)
static __forceinline void cat5(char* s, uint32_t suffix) { crt_copy(s + crt_strlen(s), ED_P(suffix), 5); }
// a texture point's pixel in the texture view's canvas (the scroll axis's total times the coordinate, less its position)
static __forceinline int32_t tv_px(const volatile float* u) { return x87_ftol(D(UI_G32(S_HAXIS)) * D(*u) - D(UI_G32(S_HAXIS + 8))); }
static __forceinline int32_t tv_py(const volatile float* v) { return x87_ftol(D(UI_G32(S_VAXIS)) * D(*v) - D(UI_G32(S_VAXIS + 8))); }
static __forceinline void dialog(UIDialog* d, uint32_t title, uint32_t bg, int32_t def, const void* items, uint32_t idle) {
    volatile uint32_t* v = (volatile uint32_t*)d;
    v[0] = title;
    v[1] = bg;
    v[2] = (uint32_t)def;
    v[3] = ed_u(items);
    v[4] = idle;
}
// an item the original writes inline (all fourteen fields)
static __forceinline void item_inline(void* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t text,
                                      int32_t i1c, const void* data, int32_t style, uint32_t lo, uint32_t hi) {
    volatile uint32_t* v = (volatile uint32_t*)it;
    v[0] = (uint32_t)type; v[1] = (uint32_t)id; v[2] = (uint32_t)x; v[3] = (uint32_t)y; v[4] = (uint32_t)w; v[5] = (uint32_t)h;
    v[6] = text; v[7] = (uint32_t)i1c; v[8] = ed_u(data); v[9] = (uint32_t)style; v[10] = lo; v[11] = hi; v[12] = 0; v[13] = 0;
}
static __forceinline int32_t do_dialog(const UIDialog* d, int32_t w, int32_t h) {
    return ccall<int32_t>(F_UIDoDialog, d, w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
}

// ---- footprints ---------------------------------------------------------------------------------------------------------------
static void fp_modal(Footprint& f) { f.replay_only = "runs a dialog or a box (modal), reads or writes files, or edits the model (rebuild_models loads its texture)"; }
static void fp_modal_i(Footprint& f, int32_t) { fp_modal(f); }
static void fp_modal_s(Footprint& f, const char*) { fp_modal(f); }
static void fp_modal_0(Footprint& f) { fp_modal(f); }
static void fp_rebuild_i(Footprint& f, int32_t) { f.replay_only = "edits the model (save_undo allocates; rebuild_models loads its texture)"; }
static __forceinline void fp_widget(Footprint& f, const volatile void* ctl) {
    if (void* w = ((const EdCtl*)ctl)->widget) f.add(w, sizeof(CustomWidget), "its widget");
}

// =================================================================================================================================
// EditMenu, ModTool
// =================================================================================================================================
static void __cdecl EditMenu_n() { ccall<void>(F_ModTool, ED_CP(0x004fe8f4)); }    // "model.mod"
static void fp_EditMenu(Footprint& f) { f.replay_only = "runs the model tool (its dialog, loads and frees)"; }
PORT_FN(0x004b0a30, "EditMenu", EditMenu_n, fp_EditMenu)

struct ToolFrame {                                // ModTool's frame from E-0xa74
    UIDialog dlg;                                 // E-0xa74
    char buf[0x50];                               // E-0xa60 a car's "<name>.car" (FIX CANDIDATE: 0x4b characters of name)
    UIDialogItem items[46];                       // E-0xa10
};
static_assert(offsetof(ToolFrame, buf) == 0xa74 - 0xa60 && offsetof(ToolFrame, items) == 0xa74 - 0xa10 && sizeof(ToolFrame) == 0xa74, "ModTool's frame");

static void __cdecl ModTool_n(const char*) {
    ToolFrame fr;
    UIDialogItem* const it = fr.items;
    for (int32_t i = 0; ccall<int32_t>(F_GetMaxCarFileNames) > i; i++) {
        crt_strcpy(fr.buf, ccall<const char*>(F_GetCarFileName, i));
        cat5(fr.buf, 0x004fe970);                                                         // ".car"
        ccall<void>(F_ResourceSetMustLoad, (const char*)fr.buf);
    }
    ccall<void>(F_ResourceSetMustLoad, ED_CP(0x004fe978));                               // "race.res"
    ccall<void>(F_reset);
    ccall<void>(F_mrLightCalc, (uint32_t)0);
    UI_G32(S_ZOOM) = 1;
    ccall<void>(F_ResourceSetMustLoad, ED_CP(0x004fe984));                               // "modtool.res"
    UI_G8(S_MODIFIED) = 0;
    ccall<void>(F_create_model_info, ED_P(S_INFO));
    ccall<void>(F_create_model_info, ED_P(S_INFO2));
    UI_GP(void, S_BUILDER) = ccall<void*>(F_ModBuilderCreate, (int32_t)0x1000, (int32_t)0x1000);
    {
        void* u = ccall<void*>(F_ModBuilderCreate, (int32_t)0x1000, (int32_t)0x1000);
        UI_GP(void, S_UNDO) = u;
    }
    UI_G32(S_TRIANGLE) = -1;
    UI_GU32(S_SLIDER) = 0;
    UI_G32(S_MODE) = 0;
    crt_strcpy((char*)ED_P(S_FILE), ED_CP(0x004fe990));                                 // "untitled.mod"
    UI_G32(S_NEW_SURFACE) = -1;
    UI_G8(S_TITLE) = 0;
    UI_G32(S_MODEL) = ccall<int32_t>(F_mrModelCreate, ED_CP(0x004fe9a0), (int32_t)0x1000);   // "texture-model"
    ccall<void>(F_mrModelBuild, UI_G32(S_MODEL), ED_P(S_INFO), (int32_t)7);
    void* const back = ccall<void*>(F_gxGetStamp, ED_CP(0x004fe9b0));                  // "carback.stp"
    for (uint32_t c = S_CANVASES; c < S_CANVASES_END; c += sizeof(gxCanvas)) ccall<void>(F_gxAllocCanvas, ED_P(c), (int32_t)0x80, (int32_t)0x80);
    ccall<void>(F_gxAllocCanvas, ED_P(S_BG_CANVAS), (int32_t)0x80, (int32_t)0x80);
    set_canvas(ED_P(S_BG_CANVAS));
    ccall<void>(F_gxClear, UI_GU32(S_BLACK));
    ccall<void>(F_MatrixMakeIdentity, ED_P(S_CAMERA));
    UI_GU32(S_CAMERA + 0x24) = 0;
    UI_GU32(S_CAMERA + 0x28) = 0;
    UI_GU32(S_CAMERA + 0x2c) = 0;
    ccall<void>(F_MatrixMakeIdentity, ED_P(S_FRAME));
    {
        volatile uint32_t* z = (volatile uint32_t*)fr.buf;    // a zero vector, in the name buffer
        z[0] = 0;
        UI_GU32(S_FRAME + 0x24) = 0;
        UI_GU32(S_FRAME + 0x28) = 0;
        UI_GU32(S_FRAME + 0x2c) = 0;
        z[1] = 0;
        z[2] = 0;
        ccall<void>(F_VectorAdd, ED_P(S_CAMERA + 0x24), ED_P(S_CAMERA + 0x24), (const void*)fr.buf);
    }
    ModelViewer* const mv = ed_mv();
    mv->model = UI_G32(S_MODEL);
    mv->capture = 0;
    mv->back = back;
    mv->frame = (EdFrame*)ED_P(S_FRAME);
    mv->camera = (EdFrame*)ED_P(S_CAMERA);
    tcall<void>(F_MV_set_position, mv, 0u, 0u, 0u);
    mv->point = ccall<int32_t>(F_mrModelLoad, ED_CP(0x004fefd4));                       // "point.mod"
    {
        volatile uint32_t* m = (volatile uint32_t*)&mv->marker;
        m[1] = 0; m[0] = 0x3f800000u; m[2] = 0; m[3] = 0; m[4] = 0x3f800000u; m[5] = 0;
        m[6] = 0; m[7] = 0; m[8] = 0x3f800000u; m[9] = 0; m[10] = 0; m[11] = 0;
    }
    const void* E = ED_P(S_EMPTY);
    ed_item(&it[0], 0x17, 0, 0x14, 0x3c, 0x12c, 0x100, E, 0, ED_P(S_MV), 0);
    ed_item(&it[1], 0xd, 0, 0x14, 0x168, 0, 0, ED_P(0x004fe9bc), 0, ED_P(S_MODE), 0);       // tview.stp
    ed_item(&it[2], 0xd, 0, 0x2c, 0x168, 0, 0, ED_P(0x004fe9c8), 2, ED_P(S_MODE), 0);       // ttri.stp
    ed_item(&it[3], 0xd, 0, 0x44, 0x168, 0, 0, ED_P(0x004fe9d4), 1, ED_P(S_MODE), 0);       // tvert.stp
    ed_item(&it[4], 0xd, 0, 0x5c, 0x168, 0, 0, ED_P(0x004fe9e0), 3, ED_P(S_MODE), 0);       // tedge.stp
    ed_item(&it[5], 0xd, 0, 0x72, 0x168, 0, 0, ED_P(0x004fe9ec), 4, ED_P(S_MODE), 0);       // tgeom.stp
    ed_item(&it[6], 0xd, 0, 0x154, 0x168, 0, 0, ED_P(0x004fe9f8), 0, ED_P(S_TV_TOOL), 0);   // tnewp.stp
    ed_item(&it[7], 0xd, 0, 0x16c, 0x168, 0, 0, ED_P(0x004fea04), 1, ED_P(S_TV_TOOL), 0);   // tmovp.stp
    ed_item(&it[8], 0xd, 0, 0x184, 0x168, 0, 0, ED_P(0x004fea10), 2, ED_P(S_TV_TOOL), 0);   // ttrans.stp
    ed_item(&it[9], 0xd, 0, 0x19c, 0x168, 0, 0, ED_P(0x004fea1c), 3, ED_P(S_TV_TOOL), 0);   // tscale.stp
    ed_item(&it[10], 0xd, 0, 0x1b8, 0x168, 0, 0, ED_P(0x004fea28), 1, ED_P(S_LOCK), 0);     // tulock.stp
    ed_item(&it[11], 0xd, 0, 0x1d0, 0x168, 0, 0, ED_P(0x004fea34), 2, ED_P(S_LOCK), 0);     // tvlock.stp
    ed_item(&it[12], 0xd, 0, 0x1e8, 0x168, 0, 0, ED_P(0x004fea40), 3, ED_P(S_LOCK), 0);     // tuvsnap.stp
    ed_item(&it[13], 0xd, 0, 0x200, 0x168, 0, 0, ED_P(0x004fea4c), 0, ED_P(S_LOCK), 0);     // tnlock.stp
    ed_item(&it[14], 3, 1, 0x21c, 0x168, 0, 0, ED_P(0x004fea58), 0, ED_P(F_plane_proj_cb), 0);       // tplane.stp
    ed_item(&it[15], 3, 1, 0x234, 0x168, 0, 0, ED_P(0x004fea64), 0, ED_P(F_three_point_put_cb), 0);  // t3pput.stp
    ed_item(&it[16], 3, 1, 0x1e0, 0x155, 0, 0, ED_P(0x004fea70), 0, ED_P(F_switch_overlay_cb), 0);   // lscroll.stp
    ed_item(&it[17], 3, 1, 0x1f4, 0x155, 0, 0, ED_P(0x004fea7c), 0, ED_P(F_capture_cb), 0);          // capture.stp
    ed_item(&it[18], 3, 2, 0x208, 0x155, 0, 0, ED_P(0x004fea88), 0, ED_P(F_switch_overlay_cb), 0);   // rscroll.stp
    ed_item(&it[19], 0xf, 0, 0x26, 0x146, 0x108, 8, E, 0x85, ED_P(S_SLIDER), 0, 0u, 0x41a00000u);    // 0..20
    ed_item(&it[20], 0x17, 0, 0x154, 0x3c, 0x100, 0x100, E, 0, ED_P(S_TV), 0);
    ed_item(&it[21], 0x11, 0, 0x254, 0x3c, 0xa, 0x100, E, 0, ED_P(S_VAXIS), 0);
    ed_item(&it[22], 0x12, 0, 0x154, 0x13c, 0x100, 0xa, E, 0, ED_P(S_HAXIS), 0);
    ed_item(&it[23], 3, 0x42, 0x255, 0x13d, 0, 0, ED_P(0x004fea94), 0, ED_P(F_zoom_cb), 0);          // zoom.stp
    ed_item(&it[24], 5, 0, 0x12c, 0xa, 0, 0, ED_P(S_FILE), 0, 0, ed_text_style());
    ed_item(&it[25], 5, 0, 0x154, 0x14f, 0, 0, ED_P(S_TITLE), 0, 0, 0x12);
    ed_item(&it[26], 5, 0, 0x21c, 0x14f, 0, 0, ED_P(S_COORD_TEXT), 0, 0, 0x12);
    ed_item(&it[27], 3, 0x14, 0x154, 0x186, 0, 0, ED_P(0x004feaa0), 0, ED_P(F_prev_cb), 0);          // prev.stp
    ed_item(&it[28], 3, 0x15, 0x19a, 0x186, 0, 0, ED_P(0x004feaac), 0, ED_P(F_next_cb), 0);          // next.stp
    ed_item(&it[29], 3, 6, 0x1e0, 0x186, 0, 0, ED_P(0x004feab8), 0, ED_P(F_add_surface_cb), 0);      // newsurf.stp
    ed_item(&it[30], 3, 7, 0x226, 0x186, 0, 0, ED_P(0x004feac4), 0, ED_P(F_set_texture_cb), 0);      // surfprop.stp
    ed_item(&it[31], 2, 3, 0x14, 0x186, 0, 0, ED_P(0x004fead4), 0, ED_P(F_translate_cb), 0);         // "Translate"
    ed_item(&it[32], 2, 3, 0x82, 0x186, 0, 0, ED_P(0x004feae0), 0, ED_P(F_scale_cb), 0);             // "Scale"
    ed_item(&it[33], 2, 3, 0xfa, 0x186, 0, 0, ED_P(0x004feae8), 0, ED_P(F_export_cb), 0);            // "Export..."
    ed_item(&it[34], 2, 3, 0x14, 0x1b8, 0, 0, ED_P(0x004feaf4), 0, ED_P(F_import_cb), 0);            // "Import..."
    ed_item(&it[35], 4, 3, 0xe, 0, 0, 0, 0, 0, ED_P(F_import_cb), 0);                                // ^N
    ed_item(&it[36], 2, 4, 0xa0, 0x1b8, 0, 0, ED_P(0x004feb00), 0, ED_P(F_open_cb), 0);              // "Open..."
    ed_item(&it[37], 4, 4, 0xf, 0, 0, 0, 0, 0, ED_P(F_open_cb), 0);                                  // ^O
    ed_item(&it[38], 4, 4, 0xd, 0, 0, 0, 0, 0, ED_P(F_measure_cb), 0);                               // ^M (Return)
    ed_item(&it[39], 2, 5, 0x12c, 0x1b8, 0, 0, ED_P(0x004feb08), 0, ED_P(F_save_cb), 0);             // "Save..."
    ed_item(&it[40], 4, 5, 0x13, 0, 0, 0, 0, 0, ED_P(F_save_cb), 0);                                 // ^S
    ed_item(&it[41], 4, 6, 0x1a, 0, 0, 0, 0, 0, ED_P(F_undo_cb), 0);                                 // ^Z
    ed_item(&it[42], 4, 6, 0x18, 0, 0, 0, 0, 0, ED_P(F_capture_cb), 0);                              // ^X
    ed_item(&it[43], 4, 6, 9, 0, 0, 0, 0, 0, ED_P(F_toggle_cb), 0);                                  // ^I (Tab)
    ed_item(&it[44], 2, -1, 0x1b8, 0x1b8, 0, 0, ED_P(0x004feb10), 0, ED_P(F_exit_cb), 0);            // "Exit"
    ed_item_end(&it[45]);
    dialog(&fr.dlg, 0x004feb18, 0, -2, fr.items, F_idle_func);                                       // "Texture Tool"
    {
        const int32_t h = UI_G32(S_SCREEN_H), w = UI_G32(S_SCREEN_W);
        do_dialog(&fr.dlg, w, h);
    }
    ccall<void>(F_mrModelUnload, mv->point);
    if (mv->capture != 0) {
        ccall<void>(F_mrModelUnload, mv->capture);
        mv->capture = 0;
    }
    ccall<void>(F_gxForgetStamp, back);
    ccall<void>(F_gxFreeCanvas, ED_P(S_BG_CANVAS));
    for (uint32_t c = S_CANVASES; c < S_CANVASES_END; c += sizeof(gxCanvas)) ccall<void>(F_gxFreeCanvas, ED_P(c));
    ccall<void>(F_mrModelDestroy, UI_G32(S_MODEL));
    ccall<void>(F_ModBuilderDestroy, bld());
    ccall<void>(F_ModBuilderDestroy, UI_GP(void, S_UNDO));
    ccall<void>(F_destroy_model_info, ED_P(S_INFO));
    ccall<void>(F_destroy_model_info, ED_P(S_INFO2));
    ccall<void>(F_ResourceSetUnload, ED_CP(0x004feb28));                                 // "modtool.res"
    ccall<void>(F_gxFreeAllTextures);
    ccall<void>(F_mrLightCalc, (uint32_t)1);
    for (int32_t i = 0; ccall<int32_t>(F_GetMaxCarFileNames) > i; i++) {
        crt_strcpy(fr.buf, ccall<const char*>(F_GetCarFileName, i));
        cat5(fr.buf, 0x004feb34);                                                         // ".car"
        ccall<void>(F_ResourceSetUnload, (const char*)fr.buf);
    }
    ccall<void>(F_ResourceSetUnload, ED_CP(0x004feb3c));                                 // "race.res"
}
PORT_FN(0x004b1a50, "ModTool", ModTool_n, fp_modal_s)

// =================================================================================================================================
// the dialog's callbacks: zoom, surfaces, idle
// =================================================================================================================================
// zoom_cb: the next zoom (1..5); the texture view's scroll axes about the same middle
static uint8_t __cdecl zoom_cb_n(int32_t) {
    volatile float b;                                    // [esp+4]
    int32_t z = ed_add(UI_G32(S_ZOOM), 1);
    UI_G32(S_ZOOM) = z;
    if (z > 5) z = 1;
    UI_G32(S_ZOOM) = z;
    const double zf = D(UI_G32(S_ZOOM));
    const int32_t cx = ed_add(UI_G32(S_HAXIS + 8), UI_G32(S_HAXIS + 4) / 2);
    const int32_t cy = ed_add(UI_G32(S_VAXIS + 8), UI_G32(S_VAXIS + 4) / 2);
    const double a = D(cx) / D(UI_G32(S_HAXIS));
    b = (float)(D(cy) / D(UI_G32(S_VAXIS)));
    TextureViewer* const tv = ed_tv();
    tv->zoom = (float)zf;
    UI_G32(S_HAXIS) = x87_ftol(D(tv->w) * zf);
    const int32_t vt = x87_ftol(zf * D(tv->h));
    const double ah = a * D(UI_G32(S_HAXIS));
    {
        const int32_t w = tv->w, h = tv->h;
        UI_G32(S_VAXIS) = vt;
        UI_G32(S_HAXIS + 4) = w;
        UI_G32(S_VAXIS + 4) = h;
    }
    {
        const int32_t p = ed_sub(x87_ftol(ah), tv->w / 2);
        UI_G32(S_HAXIS + 8) = p;
        const int32_t m = ed_sub(UI_G32(S_HAXIS), tv->w);
        if (p > m) UI_G32(S_HAXIS + 8) = m;
        if (UI_G32(S_HAXIS + 8) < 0) UI_G32(S_HAXIS + 8) = 0;
    }
    {
        const int32_t p = ed_sub(x87_ftol(D(UI_G32(S_VAXIS)) * D(b)), tv->h / 2);
        UI_G32(S_VAXIS + 8) = p;
        const int32_t m = ed_sub(UI_G32(S_VAXIS), tv->h);
        if (p > m) UI_G32(S_VAXIS + 8) = m;
        if (UI_G32(S_VAXIS + 8) < 0) UI_G32(S_VAXIS + 8) = 0;
    }
    return 0;
}
static void fp_zoom_cb(Footprint& f, int32_t) {
    UI_FP(f, S_ZOOM, 4, "the zoom");
    UI_FP(f, S_VAXIS, 12, "the vertical scroll axis");
    UI_FP(f, S_HAXIS, 12, "the horizontal scroll axis");
    UI_FP(f, S_TV + offsetof(TextureViewer, zoom), 4, "the texture view's zoom");
}
PORT_FN(0x004b2620, "zoom_cb(modtool.obj)", zoom_cb_n, fp_zoom_cb)

static uint8_t __cdecl next_cb_n(int32_t) {
    ccall<void>(F_change_surface, ed_add(UI_G32(S_SURFACE), 1));
    return 0;
}
PORT_FN(0x004b27a0, "next_cb", next_cb_n, fp_rebuild_i)

// change_surface: the surface shown (if the model has it), the picks cleared
static void __cdecl change_surface_n(int32_t s) {
    UI_G32(S_NEW_SURFACE) = s;
    UI_G32(S_TPOINT) = -1;
    UI_G32(S_VERTEX) = -1;
    UI_G32(S_TRIANGLE) = -1;
    if (is_surface(s)) {
        UI_G32(S_SURFACE) = UI_G32(S_NEW_SURFACE);
        rebuild();
    }
    ccall<void>(F_update_texture_viewer);
}
PORT_FN(0x004b27c0, "change_surface", change_surface_n, fp_rebuild_i)

static uint8_t __cdecl prev_cb_n(int32_t) {
    ccall<void>(F_change_surface, ed_sub(UI_G32(S_SURFACE), 1));
    return 0;
}
PORT_FN(0x004b2810, "prev_cb", prev_cb_n, fp_rebuild_i)

// idle_func (each frame): the 3D view's distance from the slider, the model's frame made again
static uint8_t __cdecl idle_func_n(int32_t*) {
    ModelViewer* const mv = ed_mv();
    float ma[9], mb[9];                                  // E-0x24, E-0x48
    mv->dist_b = UI_GU32(S_SLIDER);
    {
        EdFrame* F = mv->frame;
        volatile uint32_t* m = (volatile uint32_t*)F;
        m[0] = 0x3f800000u; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 0x3f800000u; m[5] = 0;
        m[6] = 0; m[7] = 0; m[8] = 0x3f800000u; m[9] = 0; m[10] = 0; m[11] = 0;
    }
    if ((int32_t)mv->dist_b < 0x3c23d70a) mv->dist_b = 0x3c23d70au;
    {
        const double r = D(k_five) / D(mv->dist);
        EdFrame* F = mv->frame;
        F->p[2] = (float)(r + D(F->p[2]));
    }
    EdFrame* const G = mv->frame;
    const uint32_t pitch = mv->pitch_b;
    ccall<void>(F_MatrixMakeYaw, ma, 0u);
    ccall<void>(F_MatrixMakePitch, mb, pitch);
    mat_concat(ma, mb, ma);
    ccall<void>(F_MatrixMakeRoll, mb, 0u);
    mat_concat(ma, mb, ma);
    mat_concat(G, ma, G);
    const uint32_t yaw = mv->yaw_b;
    EdFrame* const H = mv->frame;
    ccall<void>(F_MatrixMakeModelRotation, mb, yaw, 0u, 0u);
    mat_concat(H, mb, H);
    return 0;
}
static void fp_idle_func(Footprint& f, int32_t*) {
    UI_FP(f, S_MV + offsetof(ModelViewer, dist), 4, "the 3D view's distance");
    if (EdFrame* F = ed_mv()->frame) f.add(F, sizeof *F, "the model's frame");
}
PORT_FN(0x004b2830, "idle_func", idle_func_n, fp_idle_func)

static void __cdecl update_texture_viewer_n() { cc_dirty(ED_P(S_TV)); }
static void fp_update_texture_viewer(Footprint& f) { fp_widget(f, ED_P(S_TV)); }
PORT_FN(0x004b2960, "update_texture_viewer", update_texture_viewer_n, fp_update_texture_viewer)

// update_model: the 3D model built from the overlay with its selection (the picked triangle; the surface's texture
// triangles) or from the model
static void __cdecl update_model_n() {
    volatile int32_t t, a, b, c;                         // E-0x10, E-0x4, E-0x8, E-0xc
    switch (UI_G32(S_MODE)) {
    case 1:
        ccall<void>(F_ModBuilderDeselectAll, ED_P(S_INFO2));
        ccall<void>(F_ModBuilderSelectTriangle, ED_P(S_INFO2), UI_G32(S_TRIANGLE), (int32_t)1);
        ccall<void>(F_set_model, ED_P(S_INFO2));
        return;
    case 2:
    case 4:
        ccall<void>(F_ModBuilderDeselectAll, ED_P(S_INFO2));
        if (is_surface(UI_G32(S_SURFACE))) {
            for (int32_t i = 0; get_ttri(UI_G32(S_SURFACE), i, (void*)&t, (void*)&c, (void*)&b, (void*)&a); ) {
                const int32_t sel = t == UI_G32(S_TRIANGLE) ? 1 : 2;
                i++;
                ccall<void>(F_ModBuilderSelectTriangle, ED_P(S_INFO2), (int32_t)t, sel);
            }
        }
        ccall<void>(F_set_model, ED_P(S_INFO2));
        return;
    case 3:
        ccall<void>(F_set_model, ED_P(S_INFO2));
        return;
    }
    ccall<void>(F_set_model, ED_P(S_INFO));
}
static void fp_update_model(Footprint& f) { f.replay_only = "builds the 3D model (mrModelBuild: the renderer's lists)"; }
PORT_FN(0x004b2970, "update_model", update_model_n, fp_update_model)

static void __cdecl set_model_n(void* info) { ccall<void>(F_mrModelBuild, UI_G32(S_MODEL), info, (int32_t)7); }
static void fp_set_model(Footprint& f, void*) { f.replay_only = "builds the 3D model (mrModelBuild: the renderer's lists)"; }
PORT_FN(0x004b2ab0, "set_model", set_model_n, fp_set_model)

// rebuild_models: the surface's texture into the texture view, the title, the coordinates' text; the model's and the
// overlay's mrModelInfos; the 3D model
static void __cdecl rebuild_models_n() {
    char name[0x50];                                     // E-0x50 (ModBuilderGetSurfaceTexture's)
    gxCanvas cv;                                         // E-0x74
    volatile int32_t vtx;                                // E-0x78
    float u, v;                                          // E-0x7c, E-0x80
    cc_dirty(ED_P(S_TV));
    if (is_surface(UI_G32(S_SURFACE))) {
        ccall<void>(F_ModBuilderGetSurfaceTexture, bld(), UI_G32(S_SURFACE), (char*)name);
        ED_sprintf((char*)ED_P(S_TITLE), ED_CP(0x004feb48), ed_add(UI_G32(S_SURFACE), 1), (const char*)name);   // "Surface %d (%s)"
        const int32_t tex = ccall<int32_t>(F_gxGetTexture, (char*)name, (int32_t)0);
        set_canvas(ED_P(S_TV + offsetof(TextureViewer, canvas)));
        UI_G32(S_TV + offsetof(TextureViewer, tex_size)) = ccall<int32_t>(F_gxGetTextureSize, tex);
        if (ccall<uint8_t>(F_gxGrabTexture, tex, &cv)) {
            ccall<void>(F_gxPasteZoom, &cv, (int32_t)0, (int32_t)0, 0x3f800000u, 0x3f800000u);
            ccall<void>(F_gxReleaseTexture);
        } else {
            ccall<void>(F_gxClear, UI_GU32(S_BLACK));
        }
        if (is_surface(UI_G32(S_SURFACE)) && get_tp(UI_G32(S_SURFACE), UI_G32(S_TPOINT), (void*)&vtx, &u, &v)) {
            const double b = D(v) * 128.0, a = D(u) * 128.0;   // (fmul by 128.0f: exact)
            ((Sprintf2d_t)(uintptr_t)F_sprintf)((char*)ED_P(S_COORD_TEXT), ED_CP(0x004fef20), a, b);   // "%5.1f %5.1f"
        } else {
            ED_sprintf((char*)ED_P(S_COORD_TEXT), ED_CP(S_EMPTY));
        }
        cc_dirty(ED_P(S_TV));
        ccall<void>(F_gxForgetTexture, tex);
    } else {
        ED_sprintf((char*)ED_P(S_TITLE), ED_CP(0x004feb58));                            // "No surface"
    }
    ccall<void>(F_copy_model_info, ED_P(S_INFO), ccall<const void*>(F_ModBuilderBuildInfo, bld()));
    const int32_t mode = UI_G32(S_MODE);
    if (mode != -1) {
        if (mode == 3) ccall<void>(F_build_edge_model, ED_P(S_INFO2));
        else ccall<void>(F_copy_model_info, ED_P(S_INFO2), ccall<const void*>(F_ModBuilderBuildGeometry, bld(), ED_CP(0x004feb64)));   // "wire_f.tex"
    }
    ccall<void>(F_update_model);
}
static void fp_rebuild_models(Footprint& f) { f.replay_only = "loads and forgets the surface's texture, builds the 3D model"; }
PORT_FN(0x004b2ad0, "rebuild_models", rebuild_models_n, fp_rebuild_models)

// build_edge_model: the overlay for edge mode -- every triangle again with texture coordinates from its edges' smoothing
// flags (the wedge texture shows which edges are smoothed), one surface "wedge.tex"
static const uint32_t k_wedge_uv[8][6] = {             // the frame's table (rewritten each triangle): 8 flag patterns x 3 (u, v)
    {0, 0, 0x3f000000, 0, 0, 0x3f000000}, {0x3f000000, 0x3f000000, 0x3f000000, 0, 0, 0x3f000000},
    {0x3f000000, 0, 0x3f800000, 0, 0x3f000000, 0x3f000000}, {0x3f800000, 0x3f000000, 0x3f800000, 0, 0x3f000000, 0x3f000000},
    {0, 0x3f000000, 0x3f000000, 0x3f000000, 0, 0x3f800000}, {0x3f000000, 0x3f800000, 0x3f000000, 0x3f000000, 0, 0x3f800000},
    {0x3f000000, 0x3f000000, 0x3f800000, 0x3f000000, 0x3f000000, 0x3f800000},
    {0x3f800000, 0x3f800000, 0x3f800000, 0x3f000000, 0x3f000000, 0x3f800000},
};
static void __cdecl build_edge_model_n(EdInfo* info) {
    EdTri t;                                             // E-0xf4
    float p[3][3];                                       // E-0xe4, E-0xd8, E-0xcc
    int32_t n = 0, tb = 0, vi = 0;                       // E-0x104 triangles, E-0x100 their bytes, E-0x108 the first vertex
    info->nverts = 0;
    info->ntris = 0;
    info->n18 = 0;
    info->n20 = 0;
    info->nsurfs = 1;
    if (ccall<uint8_t>(F_ModBuilderGetTriangle, bld(), (int32_t)0, (void*)&t)) {
        do {
            const int32_t v1 = ed_add(vi, 1), v2 = ed_add(vi, 2);
            uint32_t k = 0;
            if (t.edge[2]) k = 1;
            if (t.edge[1]) k += 2;
            if (t.edge[0]) k += 4;
            const uint32_t* uv = k_wedge_uv[k];
            // FIX CANDIDATE: a triangle on a vertex the builder doesn't have keeps the last good one (or the stack's garbage)
            ccall<uint8_t>(F_ModBuilderGetVertex, bld(), (int32_t)t.v[0], (void*)p[0]);
            ccall<uint8_t>(F_ModBuilderGetVertex, bld(), (int32_t)t.v[1], (void*)p[1]);
            ccall<uint8_t>(F_ModBuilderGetVertex, bld(), (int32_t)t.v[2], (void*)p[2]);
            // FIX CANDIDATE: no bound on the vertices (0x1800) or the triangles (0x800) of the info's arrays
            const int32_t idx[3] = {vi, v1, v2};
            for (int j = 0; j < 3; j++) {
                const uint32_t off = (uint32_t)idx[j] << 5;
                ed_mv32(info->verts + off, &p[j][0]);
                ed_mv32(info->verts + off + 4, &p[j][1]);
                ed_mv32(info->verts + off + 8, &p[j][2]);
                *(volatile uint32_t*)(info->verts + off + 0x18) = uv[2 * j];
                *(volatile uint32_t*)(info->verts + off + 0x1c) = uv[2 * j + 1];
                *(volatile uint32_t*)(info->verts + off + 0xc) = 0x3f800000u;
                *(volatile uint32_t*)(info->verts + off + 0x10) = 0;
                *(volatile uint32_t*)(info->verts + off + 0x14) = 0;
                if (j == 1) n++;
            }
            const uint32_t tbo = (uint32_t)tb;
            const uint16_t a0 = (uint16_t)vi;
            vi = ed_add(vi, 3);
            tb = ed_add(tb, 8);
            *(volatile uint16_t*)(info->tris + tbo) = a0;
            *(volatile uint16_t*)(info->tris + tbo + 2) = (uint16_t)v1;
            *(volatile uint16_t*)(info->tris + tbo + 4) = (uint16_t)v2;
        } while (ccall<uint8_t>(F_ModBuilderGetTriangle, bld(), n, (void*)&t));
    }
    {
        const uint16_t c = (uint16_t)n;
        *(volatile uint16_t*)(info->surfs + 0x1c) = 0;
        *(volatile uint16_t*)(info->surfs + 0x1e) = c;
        info->ntris = n;
        *(volatile uint16_t*)(info->surfs + 0x18) = 0;
        *(volatile uint16_t*)(info->surfs + 0x1a) = (uint16_t)(c * 3u);
        info->nverts = ed_add(ed_add(n, n), n);
        crt_copy(info->surfs, ED_P(0x004feb70), 10);                                    // "wedge.tex"
    }
}
static void fp_build_edge_model(Footprint& f, EdInfo* info) {
    f.add(info, sizeof *info, "the info");
    // its arrays, as far as the builder's triangles reach (S_BUILDER's count at +0x10)
    const int32_t nt = bld() ? *(const int32_t*)((const uint8_t*)bld() + 0x10) : 0;
    const uint32_t n = nt > 0 && nt < 0x100000 ? (uint32_t)nt : 0;
    if (info->verts && n) f.add(info->verts, n * 3 * 32, "its vertices");
    if (info->tris && n) f.add(info->tris, n * 8, "its triangles");
    if (info->surfs) f.add(info->surfs, 32, "its surface");
}
PORT_FN(0x004b2cd0, "build_edge_model", build_edge_model_n, fp_build_edge_model)

// =================================================================================================================================
// the buttons: files
// =================================================================================================================================
static uint8_t __cdecl exit_cb_n(int32_t) { return ccall<uint8_t>(F_prompt_save) != 0 ? 1 : 0; }
PORT_FN(0x004b30d0, "exit_cb(modtool.obj)", exit_cb_n, fp_modal_i)

static uint8_t __cdecl save_cb_n(int32_t) {
    ccall<uint8_t>(F_save_file);
    return 0;
}
PORT_FN(0x004b30e0, "save_cb(modtool.obj)", save_cb_n, fp_modal_i)

// export_cb: the model as a .3ds (its file name with ".3ds")
static uint8_t __cdecl export_cb_n(int32_t) {
    char buf[0x104];                                     // "out.3ds" and zeros, then the file's name
    crt_copy(buf, ED_P(0x004feb7c), 8);
    {
        volatile uint32_t* z = (volatile uint32_t*)(buf + 8);
        for (int i = 0; i < 0x3f; i++) z[i] = 0;
    }
    // FIX CANDIDATE: the name (0x104) and ".3ds" into 0x104 bytes: a name of 0x100 characters or more without a dot overruns
    crt_strcpy(buf, ED_CP(S_FILE));
    if (ccall<char*>(F_strrchr, (const char*)buf, (int32_t)'.')) *ccall<char*>(F_strrchr, (const char*)buf, (int32_t)'.') = 0;
    cat5(buf, 0x004feb84);                                                                // ".3ds"
    if (file_box(F_UIDoSaveFileBox, 0x004feba4, 0x004feb98, 0x004feb90, buf, 0x104, 0x004feb8c)) {   // "Export File", "3DS File", "*.3ds"
        if (ccall<uint8_t>(F_Write3DS, ED_P(S_INFO), (const char*)buf)) {
            ED_LogReport(ED_CP(0x004febb0), (const char*)buf);                          // "Exported %s"
            return 0;
        }
        ccall<void>(F_UIDoOkBox, ED_CP(0x004febbc), (const char*)buf);                  // "Export Error"
        ED_LogReport(ED_CP(0x004febcc), (const char*)buf);                              // "Couldn't export %s"
    }
    return 0;
}
PORT_FN(0x004b30f0, "export_cb(modtool.obj)", export_cb_n, fp_modal_i)

// open_cb: a .mod (after "save changes?")
static uint8_t __cdecl open_cb_n(int32_t) {
    if (!ccall<uint8_t>(F_prompt_save)) return 0;
    if (file_box(F_UIDoOpenFileBox, 0x004febf8, 0x004febec, 0x004febe4, (char*)ED_P(S_FILE), 0x104, 0x004febe0)) {   // "Open File"
        ccall<uint8_t>(F_load_mod, ED_CP(S_FILE));
        UI_G32(S_SURFACE) = -1;
        ccall<void>(F_change_surface, (int32_t)0);
        UI_G32(S_TRIANGLE) = -1;
        UI_G32(S_VERTEX) = -1;
        UI_G32(S_TPOINT) = -1;
    }
    return 0;
}
PORT_FN(0x004b3210, "open_cb", open_cb_n, fp_modal_i)

static uint8_t __cdecl load_mod_n(const char* name) {
    if (!ccall<uint8_t>(F_ModBuilderImportMOD, bld(), name)) return 0;
    UI_G8(S_MODIFIED) = 0;
    rebuild();
    ccall<void>(F_reset);
    return 1;
}
PORT_FN(0x004b3280, "load_mod", load_mod_n, fp_modal_s)

// add_surface_cb: a new surface, with the shown one's texture and material
static uint8_t __cdecl add_surface_cb_n(int32_t) {
    struct { uint8_t m2, m1; char name[0x100]; } L;      // E-0x102, E-0x101, E-0x100
    static_assert(offsetof(decltype(L), name) == 2, "add_surface_cb's frame");
    L.name[0] = (char)UI_G8(0x004fec04);
    {
        volatile uint8_t* z = (volatile uint8_t*)L.name + 1;
        for (int i = 0; i < 0xff; i++) z[i] = 0;
    }
    L.m1 = 0;
    L.m2 = 0;
    if (UI_G32(S_SURFACE) >= 0 && is_surface(UI_G32(S_SURFACE))) {
        ccall<void>(F_ModBuilderGetSurfaceTexture, bld(), UI_G32(S_SURFACE), (char*)L.name);
        ccall<void>(F_ModBuilderGetSurfaceMaterial, bld(), UI_G32(S_SURFACE), (void*)&L.m1, (void*)&L.m2);
    }
    UI_G32(S_SURFACE) = ccall<int32_t>(F_ModBuilderAddSurface, bld());
    ccall<void>(F_ModBuilderSetSurfaceMaterial, bld(), UI_G32(S_SURFACE), (uint32_t)L.m1, (uint32_t)L.m2);
    ccall<void>(F_ModBuilderSetSurfaceTexture, bld(), UI_G32(S_SURFACE), (const char*)L.name);
    ccall<void>(F_change_surface, UI_G32(S_SURFACE));
    UI_G8(S_MODIFIED) = 1;
    rebuild();
    return 0;
}
PORT_FN(0x004b32b0, "add_surface_cb", add_surface_cb_n, fp_modal_i)

// =================================================================================================================================
// translate, scale, surface properties
// =================================================================================================================================
struct TranslateFrame {                           // translate_cb's frame from E-0x330
    float scale[3];                               // E-0x330 1, 0.01, 0.0254 (metres, centimetres, inches)
    float y, z;                                   // E-0x324, E-0x320
    char zbuf[0xc], ybuf[0xc], xbuf[0xc];         // E-0x31c, E-0x310, E-0x304: "0.0" and zeros (10 bytes)
    float p[3];                                   // E-0x2f8
    UIDialog dlg;                                 // E-0x2ec
    UIDialogItem items[13];                       // E-0x2d8
};
static_assert(offsetof(TranslateFrame, zbuf) == 0x330 - 0x31c && offsetof(TranslateFrame, p) == 0x330 - 0x2f8 &&
              offsetof(TranslateFrame, items) == 0x330 - 0x2d8 && sizeof(TranslateFrame) == 0x330, "translate_cb's frame");
static __forceinline void number_buf(char* b, uint32_t init) {     // "0.0" / "1.0", then six zeros
    *(volatile uint32_t*)b = UI_GU32(init);
    *(volatile uint32_t*)(b + 4) = 0;
    *(volatile uint16_t*)(b + 8) = 0;
}

// translate_cb: the model moved by three numbers (in metres, centimetres or inches)
static uint8_t __cdecl translate_cb_n(int32_t) {
    TranslateFrame fr;
    UIDialogItem* const it = fr.items;
    number_buf(fr.xbuf, 0x004fec08);
    number_buf(fr.ybuf, 0x004fec0c);
    *(volatile uint32_t*)&fr.scale[0] = 0x3f800000u;
    number_buf(fr.zbuf, 0x004fec10);
    *(volatile uint32_t*)&fr.scale[1] = 0x3c23d70au;
    *(volatile uint32_t*)&fr.scale[2] = 0x3cd013a9u;
    ed_item(&it[0], 5, 0, 0x14, 0x28, 0, 0, ED_P(0x004fec14), 0, 0, ed_text_style());      // "X"
    ed_item(&it[1], 9, 0, 0x64, 0x28, 0x50, 0x10, ED_P(1), 0xa, fr.xbuf, 9);
    ed_item(&it[2], 5, 0, 0x14, 0x3c, 0, 0, ED_P(0x004fec18), 0, 0, ed_text_style());      // "Y"
    ed_item(&it[3], 9, 0, 0x64, 0x3c, 0x50, 0x10, ED_P(1), 0xa, fr.ybuf, 9);
    ed_item(&it[4], 5, 0, 0x14, 0x50, 0, 0, ED_P(0x004fec1c), 0, 0, ed_text_style());      // "Z"
    ed_item(&it[5], 9, 0, 0x64, 0x50, 0x50, 0x10, ED_P(1), 0xa, fr.zbuf, 9);
    item_inline(&it[6], 5, 0, 0x14, 0x64, 0, 0, 0x004fec20, 0, 0, 0xb, 0, 0);               // "Units"
    ed_item(&it[7], 0xc, 0, 0x1e, 0x78, 0, 0, ED_P(0x004fec28), 0, ED_P(S_UNITS), 8);     // "Meters"
    ed_item(&it[8], 0xc, 0, 0x1e, 0x87, 0, 0, ED_P(0x004fec30), 1, ED_P(S_UNITS), 8);     // "Centimeters"
    ed_item(&it[9], 0xc, 0, 0x1e, 0x96, 0, 0, ED_P(0x004fec3c), 2, ED_P(S_UNITS), 8);     // "Inches"
    ed_item(&it[10], 3, -2, 8, 0xa3, 0, 0, ED_P(0x004fec44), 0, 0, 0);                     // ok.stp
    ed_item(&it[11], 3, -1, 0xf8, 0xa3, 0, 0, ED_P(0x004fec4c), 0, 0, 0);                  // cancel.stp
    ed_item_end(&it[12]);
    dialog(&fr.dlg, 0x004fec58, 0x004fec68, -2, fr.items, 0);                              // "Translate Model", dialog1.stp
    if (do_dialog(&fr.dlg, 0x140, 0xc8) != -2) return 0;
    // FIX CANDIDATE: the units index the scale table unchecked (only the radio buttons set them: 0..2)
    fr.z = (float)(ccall<double>(F_atof, (const char*)fr.zbuf) * D(fr.scale[UI_G32(S_UNITS)]));
    fr.y = (float)(ccall<double>(F_atof, (const char*)fr.ybuf) * D(fr.scale[UI_G32(S_UNITS)]));
    {
        const double x = ccall<double>(F_atof, (const char*)fr.xbuf);
        ed_mv32(&fr.p[1], &fr.y);
        ed_mv32(&fr.p[2], &fr.z);
        fr.p[0] = (float)(x * D(fr.scale[UI_G32(S_UNITS)]));
    }
    ccall<void>(F_ModBuilderTranslateModel, bld(), (const void*)fr.p);
    UI_G8(S_MODIFIED) = 1;
    rebuild();
    return 0;
}
PORT_FN(0x004b33a0, "translate_cb", translate_cb_n, fp_modal_i)

struct ScaleFrame {                               // scale_cb's frame from E-0x244
    float y, z;                                   // E-0x244, E-0x240
    char zbuf[0xc], ybuf[0xc], xbuf[0xc];         // E-0x23c, E-0x230, E-0x224: "1.0" and zeros
    float p[3];                                   // E-0x218
    UIDialog dlg;                                 // E-0x20c
    UIDialogItem items[9];                        // E-0x1f8
};
static_assert(offsetof(ScaleFrame, xbuf) == 0x244 - 0x224 && offsetof(ScaleFrame, dlg) == 0x244 - 0x20c &&
              offsetof(ScaleFrame, items) == 0x244 - 0x1f8 && sizeof(ScaleFrame) == 0x244, "scale_cb's frame");

// scale_cb: the model scaled by three numbers
static uint8_t __cdecl scale_cb_n(int32_t) {
    ScaleFrame fr;
    UIDialogItem* const it = fr.items;
    number_buf(fr.xbuf, 0x004fec74);
    number_buf(fr.ybuf, 0x004fec78);
    number_buf(fr.zbuf, 0x004fec7c);
    ed_item(&it[0], 5, 0, 0x14, 0x28, 0, 0, ED_P(0x004fec80), 0, 0, ed_text_style());      // "X"
    ed_item(&it[1], 9, 0, 0x64, 0x28, 0x50, 0x10, ED_P(1), 0xa, fr.xbuf, 9);
    item_inline(&it[2], 5, 0, 0x14, 0x3c, 0, 0, 0x004fec84, 0, 0, ed_text_style(), 0, 0);  // "Y"
    ed_item(&it[3], 9, 0, 0x64, 0x3c, 0x50, 0x10, ED_P(1), 0xa, fr.ybuf, 9);
    ed_item(&it[4], 5, 0, 0x14, 0x50, 0, 0, ED_P(0x004fec88), 0, 0, ed_text_style());      // "Z"
    ed_item(&it[5], 9, 0, 0x64, 0x50, 0x50, 0x10, ED_P(1), 0xa, fr.zbuf, 9);
    ed_item(&it[6], 3, -2, 8, 0xa3, 0, 0, ED_P(0x004fec8c), 0, 0, 0);                      // ok.stp
    item_inline(&it[7], 3, -1, 0xf8, 0xa3, 0, 0, 0x004fec94, 0, 0, 0, 0, 0);               // cancel.stp
    ed_item_end(&it[8]);
    dialog(&fr.dlg, 0x004feca0, 0x004fecac, -2, fr.items, 0);                              // "Scale Model", dialog1.stp
    if (do_dialog(&fr.dlg, 0x140, 0xc8) != -2) return 0;
    fr.z = (float)ccall<double>(F_atof, (const char*)fr.zbuf);
    fr.y = (float)ccall<double>(F_atof, (const char*)fr.ybuf);
    fr.p[0] = (float)ccall<double>(F_atof, (const char*)fr.xbuf);
    ed_mv32(&fr.p[1], &fr.y);
    ed_mv32(&fr.p[2], &fr.z);
    ccall<void>(F_ModBuilderScaleModel, bld(), (const void*)fr.p);
    UI_G8(S_MODIFIED) = 1;
    rebuild();
    return 0;
}
PORT_FN(0x004b3740, "scale_cb", scale_cb_n, fp_modal_i)

struct SurfaceFrame {                             // set_texture_cb's frame from E-0x3c8
    uint8_t m2, m1;                               // E-0x3c8 (alpha), E-0x3c7 (material)
    int16_t group;                                // E-0x3c6
    int32_t material;                             // E-0x3c4 (the radio buttons' 0..2)
    int32_t alpha;                                // E-0x3c0 (0..1)
    float groupf;                                 // E-0x3bc (the slider's)
    int32_t tmp;                                  // E-0x3b8
    UIDialog dlg;                                 // E-0x3b4
    char name[0x20];                              // E-0x3a0 the texture's name
    UIDialogItem items[16];                       // E-0x380
};
static_assert(offsetof(SurfaceFrame, material) == 0x3c8 - 0x3c4 && offsetof(SurfaceFrame, dlg) == 0x3c8 - 0x3b4 &&
              offsetof(SurfaceFrame, name) == 0x3c8 - 0x3a0 && offsetof(SurfaceFrame, items) == 0x3c8 - 0x380 &&
              sizeof(SurfaceFrame) == 0x3c8, "set_texture_cb's frame");

// set_texture_cb (surface properties): the surface's texture, material, alpha and smoothing group
static uint8_t __cdecl set_texture_cb_n(int32_t) {
    SurfaceFrame fr;
    UIDialogItem* const it = fr.items;
    if (!is_surface(UI_G32(S_SURFACE))) return 0;
    // FIX CANDIDATE: S_TEXNAME is left aimed at this frame (browse_cb, inside the dialog, is its only user)
    UI_GP(char, S_TEXNAME) = fr.name;
    ccall<void>(F_ModBuilderGetSurfaceTexture, bld(), UI_G32(S_SURFACE), (char*)fr.name);
    fr.m1 = 0;
    fr.m2 = 0;
    ccall<void>(F_ModBuilderGetSurfaceGroup, bld(), UI_G32(S_SURFACE), (void*)&fr.group);
    ccall<void>(F_ModBuilderGetSurfaceMaterial, bld(), UI_G32(S_SURFACE), (void*)&fr.m1, (void*)&fr.m2);
    fr.tmp = fr.group;
    fr.material = fr.m1;
    fr.groupf = (float)D(fr.tmp);
    fr.alpha = fr.m2;
    ed_item(&it[0], 5, 0, 0x14, 0x14, 0, 0, ED_P(0x004fecb8), 0, 0, ed_text_style());      // "Texture"
    ed_item(&it[1], 9, 0, 0x64, 0x14, 0x70, 0x10, ED_P(1), 0xe, fr.name, 9);
    ed_item(&it[2], 3, 1, 0xdc, 0x13, 0, 0, ED_P(0x004fecc0), 0, ED_P(F_browse_cb), 0);   // browse.stp
    ed_item(&it[3], 5, 0, 0x14, 0x28, 0, 0, ED_P(0x004feccc), 0, 0, ed_text_style());      // "Material"
    ed_item(&it[4], 0xc, 0, 0x64, 0x28, 0, 0, ED_P(0x004fecd8), 0, &fr.material, 8);      // "Normal"
    ed_item(&it[5], 0xc, 0, 0x64, 0x37, 0, 0, ED_P(0x004fece0), 1, &fr.material, 8);      // "Shiny"
    ed_item(&it[6], 0xc, 0, 0x64, 0x46, 0, 0, ED_P(0x004fece8), 2, &fr.material, 8);      // "Luminous"
    ed_item(&it[7], 0xc, 0, 0xdc, 0x28, 0, 0, ED_P(0x004fecf4), 0, &fr.alpha, 8);         // "Plain"
    ed_item(&it[8], 0xc, 0, 0xdc, 0x37, 0, 0, ED_P(0x004fecfc), 1, &fr.alpha, 8);         // "Alpha"
    ed_item(&it[9], 5, 0, 0x14, 0x5a, 0, 0, ED_P(0x004fed04), 0, 0, ed_text_style());      // "Group"
    item_inline(&it[10], 6, 0, 0x5a, 0x5a, 0, 0, 0x004fed0c, 0, &fr.groupf, 0x12, 0x3f800000u, 0);   // "%3.0f"
    ed_item(&it[11], 0xf, 0, 0xa0, 0x5a, 0x6e, 8, ED_P(S_EMPTY), 0x10, &fr.groupf, 0, 0u, 0x41700000u);   // 0..15
    ed_item(&it[12], 3, 2, 0x5a, 0xa3, 0, 0, ED_P(0x004fed14), 0, ED_P(F_template_cb), 0);  // mktmplt.stp
    ed_item(&it[13], 3, -2, 8, 0xa3, 0, 0, ED_P(0x004fed20), 0, 0, 0);                      // ok.stp
    ed_item(&it[14], 3, -1, 0xf8, 0xa3, 0, 0, ED_P(0x004fed28), 0, 0, 0);                   // cancel.stp
    ed_item_end(&it[15]);
    dialog(&fr.dlg, 0x004fed34, 0x004fed48, -2, fr.items, 0);                               // "Surface Properties"
    if (do_dialog(&fr.dlg, 0x140, 0xc8) != -2) return 0;
    ccall<void>(F_ModBuilderSetSurfaceTexture, bld(), UI_G32(S_SURFACE), (const char*)fr.name);
    fr.m1 = (uint8_t)fr.material;
    fr.m2 = fr.alpha != 0 ? 1 : 0;
    ccall<void>(F_ModBuilderSetSurfaceMaterial, bld(), UI_G32(S_SURFACE), (uint32_t)fr.m1, (uint32_t)fr.m2);
    ccall<void>(F_ModBuilderSetSurfaceGroup, bld(), UI_G32(S_SURFACE), x87_ftol(D(fr.groupf)));
    UI_G8(S_MODIFIED) = 1;
    rebuild();
    ccall<void>(F_change_surface, UI_G32(S_SURFACE));
    return 0;
}
PORT_FN(0x004b3a50, "set_texture_cb", set_texture_cb_n, fp_modal_i)

// template_cb (make template): the surface's texture triangles drawn (each its own colour, from 1) into a 256 x 256 canvas
// written as a .bmp
static uint8_t __cdecl template_cb_n(int32_t) {
    char name[0x104];                                    // E-0x104
    gxCanvas cv;                                         // E-0x128
    volatile int32_t t, ia, ib, ic, vtx;                 // E-0x12c, E-0x14c, E-0x150, E-0x154, E-0x15c
    float u, v;
    crt_strcpy(name, ED_CP(0x004fed54));                                                   // "template.bmp"
    if (!file_box(F_UIDoSaveFileBox, 0x004fed7c, 0x004fed70, 0x004fed68, name, 0x104, 0x004fed64)) return 0;   // "Save as template"
    if (!is_surface(UI_G32(S_SURFACE))) return 0;
    ccall<void>(F_gxAllocCanvas, &cv, (int32_t)0x100, (int32_t)0x100);
    set_canvas(&cv);
    int32_t col = 1;
    ccall<void>(F_gxClear, (uint32_t)0);
    int32_t k = 0;
    if (get_ttri(UI_G32(S_SURFACE), 0, (void*)&t, (void*)&ia, (void*)&ib, (void*)&ic)) {
        do {
            int32_t x0 = 0, y0 = 0, y1 = 0, y2 = 0, x1 = 0, x2 = 0;
            const volatile int32_t* tvx = (const volatile int32_t*)ED_P(S_TV + 4);
            const volatile int32_t* tvy = (const volatile int32_t*)ED_P(S_TV + 8);
            if (get_tp(UI_G32(S_SURFACE), ia, (void*)&vtx, &u, &v)) {
                x0 = ed_add(*tvx, tv_px(&u));
                y0 = ed_add(*tvy, tv_py(&v));
            }
            if (get_tp(UI_G32(S_SURFACE), ib, (void*)&vtx, &u, &v)) {
                x1 = ed_add(tv_px(&u), *tvx);
                y1 = ed_add(*tvy, tv_py(&v));
            }
            if (get_tp(UI_G32(S_SURFACE), ic, (void*)&vtx, &u, &v)) {
                x2 = ed_add(tv_px(&u), *tvx);
                y2 = ed_add(*tvy, tv_py(&v));
            }
            x0 = ed_sub(x0, *tvx);
            y0 = ed_sub(y0, *tvy);
            const int32_t tx = *tvx;
            y1 = ed_sub(y1, *tvy);
            y2 = ed_sub(y2, *tvy);
            x1 = ed_sub(x1, tx);
            x2 = ed_sub(x2, tx);
            ccall<void>(F_gxTriangle, x0, y0, x1, y1, x2, y2, (uint32_t)col);
            col++;
            k++;
        } while (get_ttri(UI_G32(S_SURFACE), k, (void*)&t, (void*)&ia, (void*)&ib, (void*)&ic));
    }
    ccall<void>(F_gxWriteBMP, (const char*)name, (const void*)&cv);
    ccall<void>(F_gxFreeCanvas, &cv);
    return 0;
}
PORT_FN(0x004b3ed0, "template_cb(modtool.obj)", template_cb_n, fp_modal_i)

// browse_cb: a .tex file box into the surface properties' texture name (32 bytes)
static uint8_t __cdecl browse_cb_n(int32_t) {
    file_box(F_UIDoOpenFileBox, 0x004feda8, 0x004fed9c, 0x004fed94, UI_GP(char, S_TEXNAME), 0x20, 0x004fed90);   // "Texture", "TEX File"
    return 0;
}
PORT_FN(0x004b41c0, "browse_cb", browse_cb_n, fp_modal_i)

// import_cb: a .dxf (yes) or a .3ds (no), after "save changes?"; the file's name becomes "<name>.mod"
static uint8_t __cdecl import_cb_n(int32_t) {
    char name[0x104];
    if (!ccall<uint8_t>(F_prompt_save)) return 0;
    name[0] = (char)UI_G8(0x004fedb0);
    {
        volatile uint8_t* z = (volatile uint8_t*)name + 1;
        for (int i = 0; i < 0x103; i++) z[i] = 0;
    }
    const bool dxf = ccall<uint8_t>(F_UIDoYesNoBox, ED_CP(0x004fedc8), ED_CP(0x004fedb4), (const void*)0) != 0;   // "Import", "Import a DXF file?"
    if (dxf) {
        if (!file_box(F_UIDoOpenFileBox, 0x004fede8, 0x004feddc, 0x004fedd4, name, 0x104, 0x004fedd0)) return 0;   // "Import DXF file"
        ccall<uint8_t>(F_dxf_import, (const char*)name);
    } else {
        if (!file_box(F_UIDoOpenFileBox, 0x004fee18, 0x004fee0c, 0x004fee04, name, 0x104, 0x004fee00)) return 0;   // "Import 3DS file"
        ccall<uint8_t>(F_ad3ds_import, (const char*)name);
    }
    // FIX CANDIDATE: the name (0x104) and ".mod" into S_FILE's 0x104 bytes (a name of 0x100 characters or more without a dot)
    crt_strcpy((char*)ED_P(S_FILE), name);
    if (char* d = ccall<char*>(F_strrchr, (const char*)ED_P(S_FILE), (int32_t)'.')) *d = 0;
    {
        char* const e = (char*)ED_P(S_FILE) + crt_strlen((const char*)ED_P(S_FILE));
        const uint32_t sfx = dxf ? 0x004fedf8u : 0x004fee28u;                            // ".mod"
        const uint32_t w = UI_GU32(sfx);
        const uint8_t c = UI_G8(sfx + 4);
        UI_G32(S_TRIANGLE) = -1;
        *(volatile uint32_t*)e = w;
        UI_G32(S_VERTEX) = -1;
        *(volatile uint8_t*)(e + 4) = c;
        UI_G32(S_TPOINT) = -1;
        UI_G32(S_SURFACE) = 0;
    }
    rebuild();
    UI_G8(S_MODIFIED) = 1;
    return 0;
}
PORT_FN(0x004b41f0, "import_cb(modtool.obj)", import_cb_n, fp_modal_i)

static uint8_t __cdecl dxf_import_n(const char* name) {
    if (!ccall<uint8_t>(F_ModBuilderImportDXF, bld(), name, (uint32_t)0)) return 0;
    UI_G8(S_MODIFIED) = 1;
    ccall<void>(F_reset);
    return 1;
}
PORT_FN(0x004b43e0, "dxf_import", dxf_import_n, fp_modal_s)

static uint8_t __cdecl ad3ds_import_n(const char* name) {
    if (!ccall<uint8_t>(F_ModBuilderImport3DS, bld(), name)) return 0;
    UI_G8(S_MODIFIED) = 1;
    ccall<void>(F_reset);
    return 1;
}
PORT_FN(0x004b4410, "ad3ds_import", ad3ds_import_n, fp_modal_s)

// =================================================================================================================================
// the model infos
// =================================================================================================================================
static void __cdecl create_model_info_n(EdInfo* info) {
    info->nverts = 0;
    info->verts = ccall<uint8_t*>(F_MemAlloc, (int32_t)0x30000);
    info->nsurfs = 0;
    info->surfs = ccall<uint8_t*>(F_MemAlloc, (int32_t)0x4000);
    info->ntris = 0;
    info->tris = ccall<uint8_t*>(F_MemAlloc, (int32_t)0x4000);
    info->n18 = 0;
    info->p1c = 0;
    info->n20 = 0;
    info->p24 = 0;
}
static void fp_create_model_info(Footprint& f, EdInfo*) { f.replay_only = "allocates the arrays"; }
PORT_FN(0x004b4440, "create_model_info", create_model_info_n, fp_create_model_info)

static void __cdecl destroy_model_info_n(EdInfo* info) {
    ccall<void>(F_Delete, (void*)info->tris);
    ccall<void>(F_Delete, (void*)info->surfs);
    ccall<void>(F_Delete, (void*)info->verts);
}
static void fp_destroy_model_info(Footprint& f, EdInfo*) { f.replay_only = "frees the arrays"; }
PORT_FN(0x004b4490, "destroy_model_info", destroy_model_info_n, fp_destroy_model_info)

// copy_model_info: the counts and the vertices, surfaces and triangles (into the destination's arrays)
// FIX CANDIDATE: no bound -- the destination's arrays hold 0x1800 vertices, 0x200 surfaces, 0x800 triangles (create_model_info)
static void __cdecl copy_model_info_n(EdInfo* d, const EdInfo* s) {
    const int32_t nv = s->nverts;
    d->nverts = nv;
    d->nsurfs = s->nsurfs;
    d->ntris = s->ntris;
    if (nv > 0) {
        int32_t i = 0;
        uint32_t o = 0;
        do {
            crt_copy(d->verts + o, s->verts + o, 32);
            o += 0x20;
            i++;
        } while (d->nverts > i);
    }
    for (int32_t i = 0; d->nsurfs > i; i++) crt_copy(d->surfs + (uint32_t)i * 32u, s->surfs + (uint32_t)i * 32u, 32);
    for (int32_t i = 0; d->ntris > i; i++) {
        const uint8_t* a = s->tris + (uint32_t)i * 8u;
        uint8_t* b = d->tris + (uint32_t)i * 8u;
        const uint32_t w0 = ed_bits(a), w1 = ed_bits(a + 4);
        *(volatile uint32_t*)b = w0;
        *(volatile uint32_t*)(b + 4) = w1;
    }
}
static void fp_copy_model_info(Footprint& f, EdInfo* d, const EdInfo* s) {
    f.add(d, sizeof *d, "the info");
    auto n = [](int32_t c) { return c > 0 && c < 0x100000 ? (uint32_t)c : 0u; };
    if (d->verts && n(s->nverts)) f.add(d->verts, n(s->nverts) * 32, "its vertices");
    if (d->surfs && n(s->nsurfs)) f.add(d->surfs, n(s->nsurfs) * 32, "its surfaces");
    if (d->tris && n(s->ntris)) f.add(d->tris, n(s->ntris) * 8, "its triangles");
}
PORT_FN(0x004b44c0, "copy_model_info", copy_model_info_n, fp_copy_model_info)

// =================================================================================================================================
// saving, reset, undo
// =================================================================================================================================
// prompt_save: "save before closing?" if there are unsaved changes -- yes saves (and goes on if it did), no goes on,
// cancel doesn't
static uint8_t __cdecl prompt_save_n() {
    if (UI_G8(S_MODIFIED) == 0) return 1;
    const int32_t r = ccall<int32_t>(F_UIDoYesNoCancelBox, ED_CP(0x004fee60), ED_CP(0x004fee48));   // "Unsaved changes", "Save before closing?"
    if (r == -3) return 1;
    if (r == -2) return ccall<uint8_t>(F_save_file) != 0 ? 1 : 0;
    if (r == -1) return 0;
    ED_LogPanic(ED_CP(0x004fee70));                                                        // "Unknown return value"
    return 0;
}
PORT_FN(0x004b4550, "prompt_save", prompt_save_n, fp_modal_0)

static uint8_t __cdecl save_file_n() {
    if (!file_box(F_UIDoSaveFileBox, 0x004feea0, 0x004fee94, 0x004fee8c, (char*)ED_P(S_FILE), 0x104, 0x004fee88)) return 0;   // "Save File"
    ccall<uint8_t>(F_save_mod, ED_CP(S_FILE));
    return 1;
}
PORT_FN(0x004b45a0, "save_file", save_file_n, fp_modal_0)

static uint8_t __cdecl save_mod_n(const char* name) {
    UI_G8(S_MODIFIED) = 0;
    if (ccall<uint8_t>(F_mrModelInfoSave, ED_P(S_INFO), name)) return 1;
    ED_LogReport(ED_CP(0x004feeac), name);                                                 // "Error saving %s"
    ccall<void>(F_UIDoOkBox, ED_CP(0x004feee0), ED_CP(0x004feebc));                        // "Error", "Can't save file! (may be read-only)"
    return 0;
}
PORT_FN(0x004b45e0, "save_mod", save_mod_n, fp_modal_s)

// reset: the picks and the selection cleared, the first surface, the texture transform the identity (and not set)
static void __cdecl reset_n() {
    UI_G32(S_TRIANGLE) = -1;
    UI_G32(S_VERTEX) = -1;
    UI_G32(S_SEL) = -1;
    UI_G32(S_TPOINT) = -1;
    UI_G32(S_SEL + 4) = -1;
    UI_G32(S_SEL + 8) = -1;
    UI_G32(S_SURFACE) = 0;
    volatile uint32_t* m = (volatile uint32_t*)ED_P(S_XFORM);
    m[0] = 0x3f800000u; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 0x3f800000u; m[5] = 0;
    m[6] = 0; m[7] = 0; m[8] = 0x3f800000u; m[9] = 0; m[10] = 0; m[11] = 0;
    UI_G8(S_XFORM_SET) = 0;
}
static void fp_reset(Footprint& f) {
    UI_FP(f, S_TRIANGLE, 4, "the triangle picked");
    UI_FP(f, S_VERTEX, 4, "the vertex picked");
    UI_FP(f, S_TPOINT, 4, "the texture point picked");
    UI_FP(f, S_SURFACE, 4, "the surface");
    UI_FP(f, S_SEL, 12, "the selection");
    UI_FP(f, S_XFORM, 0x30, "the texture transform");
    UI_FP(f, S_XFORM_SET, 1, "the transform's flag");
}
PORT_FN(0x004b4630, "reset", reset_n, fp_reset)

static uint8_t __cdecl three_point_put_cb_n(int32_t) {
    ccall<void>(F_set_transform, (uint32_t)0);
    return 0;
}
PORT_FN(0x004b46b0, "three_point_put_cb", three_point_put_cb_n, fp_rebuild_i)

static uint8_t __cdecl plane_proj_cb_n(int32_t) {
    ccall<void>(F_set_transform, (uint32_t)1);
    return 0;
}
PORT_FN(0x004b46c0, "plane_proj_cb", plane_proj_cb_n, fp_rebuild_i)

static void __cdecl save_undo_n() { ccall<void>(F_ModBuilderCopy, UI_GP(void, S_UNDO), bld()); }
static void fp_save_undo(Footprint& f) { f.replay_only = "ModBuilderCopy frees and allocates the surfaces' arrays"; }
PORT_FN(0x004b46d0, "save_undo", save_undo_n, fp_save_undo)

static uint8_t __cdecl undo_cb_n(int32_t) {
    ccall<void>(F_ModBuilderCopy, bld(), UI_GP(void, S_UNDO));
    rebuild();
    return 0;
}
PORT_FN(0x004b46f0, "undo_cb(modtool.obj)", undo_cb_n, fp_rebuild_i)

// capture_cb: the model "*<file>" loaded if one is found (drawn in capture mode); every surface's texture points (the
// distinct ones of its texture triangles) kept as an overlay for the texture view (10 surfaces at most)
static uint8_t __cdecl capture_cb_n(int32_t) {
    char buf[0x104];                                     // E-0x104 the model's name (its first dword: GetTexturePoint's vertex)
    char found[0x104];                                   // E-0x208 FileFindFirst's
    volatile int32_t t, tri[3];                          // E-0x20c, E-0x208.. (in the original, over `found`)
    float u, v;                                          // E-0x21c, E-0x218
    ModelViewer* const mv = ed_mv();
    TextureViewer* const tv = ed_tv();
    if (mv->capture != 0) ccall<void>(F_mrModelUnload, mv->capture);
    mv->capture = 0;
    ED_sprintf(buf, ED_CP(0x004fefe0), ED_CP(S_FILE));                                     // "*%s"
    {
        const intptr_t h = ccall<intptr_t>(F_FileFindFirst, ED_CP(S_FILE), (char*)found, (int32_t)0);
        if (h != -1) {
            ccall<void>(F_FileFindClose, (void*)h);
            mv->capture = ccall<int32_t>(F_mrModelLoad, (const char*)buf);
        }
    }
    tv->noverlays = 0;
    tv->overlay = 0;
    for (int32_t s = 0; s < 10; s++) {
        if (!is_surface(s)) break;
        tv->noverlays = ed_add(tv->noverlays, 1);
        tv->counts[s] = 0;
        int32_t k = 0;
        while (get_ttri(s, k, (void*)&t, (void*)&tri[0], (void*)&tri[1], (void*)&tri[2])) {
            for (int j = 0; j < 3; j++) {
                volatile uint32_t* ub = (volatile uint32_t*)&u;
                volatile uint32_t* vb = (volatile uint32_t*)&v;
                *ub = 0;
                *vb = 0;
                get_tp(s, tri[j], (void*)buf, &u, &v);
                bool have = false;
                const EdTexPt* pp = &tv->pts[s][0];
                for (int32_t i = 0; tv->counts[s] > i; i++, pp++) {
                    if (!(D(pp->u) < D(u) || D(pp->u) > D(u)) && !(D(pp->v) < D(v) || D(pp->v) > D(v))) { have = true; break; }
                }
                if (!have) {
                    // FIX CANDIDATE: no bound on a surface's points (0x1000 an overlay): more run into the next overlay, and from
                    // the tenth surface past the texture view (S_SEL_COLOR, S_MODE, the title...)
                    const uint32_t at = (uint32_t)ed_add(tv->counts[s], s * 0x1000);
                    *(volatile uint32_t*)ED_P(S_TV + 0x84 + at * 8u) = *ub;
                    *(volatile uint32_t*)ED_P(S_TV + 0x88 + (uint32_t)ed_add(tv->counts[s], s * 0x1000) * 8u) = *vb;
                    tv->counts[s] = ed_add(tv->counts[s], 1);
                }
            }
            k++;
        }
    }
    return 0;
}
PORT_FN(0x004b4710, "capture_cb", capture_cb_n, fp_modal_i)

// measure_cb: the model's width, height and depth in inches, in a box
static uint8_t __cdecl measure_cb_n(int32_t) {
    char buf[0x50];                                      // E-0x50
    float a, b, c, d, e, f;                              // E-0x64, E-0x68, E-0x5c, E-0x60, E-0x54, E-0x58
    *(volatile uint32_t*)&a = 0; *(volatile uint32_t*)&c = 0; *(volatile uint32_t*)&e = 0;
    *(volatile uint32_t*)&b = 0; *(volatile uint32_t*)&d = 0; *(volatile uint32_t*)&f = 0;
    ccall<void>(F_mrModelGetExtents, UI_G32(S_MODEL), &a, &b, &c, &d, &e, &f);
    const double w = D(b) - a, h = D(d) - c, dp = D(f) - e;
    ((Sprintf3d_t)(uintptr_t)F_sprintf)(buf, ED_CP(0x004feee8), w * D(k_inch), h * D(k_inch), dp * D(k_inch));   // "Width x Height x Depth..."
    ccall<void>(F_UIDoOkBox, ED_CP(0x004fef18), (const char*)buf);                         // "Measure"
    return 0;
}
PORT_FN(0x004b4920, "measure_cb", measure_cb_n, fp_modal_i)

// toggle_cb (Tab): view mode <-> capture mode
static uint8_t __cdecl toggle_cb_n(int32_t) {
    if (UI_G32(S_MODE) == 0) {
        UI_G32(S_MODE) = 5;
        return 0;
    }
    if (UI_G32(S_MODE) == 5) UI_G32(S_MODE) = 0;
    return 0;
}
static void fp_toggle_cb(Footprint& f, int32_t) { UI_FP(f, S_MODE, 4, "the mode"); }
PORT_FN(0x004b49e0, "toggle_cb", toggle_cb_n, fp_toggle_cb)

// switch_overlay_cb: the previous (id 1) or next overlay, wrapping
static uint8_t __cdecl switch_overlay_cb_n(int32_t id) {
    TextureViewer* const tv = ed_tv();
    if (id == 1) {
        if (tv->overlay < 1) tv->overlay = ed_sub(tv->noverlays, 1);
        else tv->overlay = ed_sub(tv->overlay, 1);
    } else {
        tv->overlay = ed_add(tv->overlay, 1);
        if (!(tv->noverlays > tv->overlay)) tv->overlay = 0;
    }
    cc_dirty(ED_P(S_TV));
    return 0;
}
static void fp_switch_overlay_cb(Footprint& f, int32_t) {
    UI_FP(f, S_TV + offsetof(TextureViewer, overlay), 4, "the overlay shown");
    fp_widget(f, ED_P(S_TV));
}
PORT_FN(0x004b4a10, "switch_overlay_cb", switch_overlay_cb_n, fp_switch_overlay_cb)

}  // namespace edit_tool
}  // namespace
