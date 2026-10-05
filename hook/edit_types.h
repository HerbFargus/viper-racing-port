// edit_types.h -- M3 UI stage, step U5 (group A): the model tool's layouts, statics and callees (library `edit`: modtool.obj,
// edit.obj), shared by hook/edit_tool.cpp, hook/edit_view.cpp and test/world_edit_tool.cpp. Group B (modbuild.obj, cvt3ds.obj,
// adtools.obj) has its own layouts in hook/edit_build.h; this file names its functions only by their v1.0 addresses.
//
// Recovered from the v1.0 disassembly (ModTool's frame and statics, the two controls' methods, $E62 / $E67 which build them).
// The model tool is one dialog (ModTool, reached by Ctrl+E from the main menu: race() -> EditMenu): a ModelViewer (the model in
// 3D, turned by dragging, picked with the mouse) and a TextureViewer (the selected surface's texture with its texture points),
// radio stamps for the 3D view's mode and the texture view's tool and lock, buttons for the rest. The model being edited is a
// ModBuilder (modbuild.obj) with an undo copy; rebuild_models makes the two mrModelInfos the 3D view draws from it (the model,
// and an overlay: the selected surface's geometry or the edges). Fields are volatile, as in ui_types.h: the rewrites read and
// write each one where the original does, in its order.
//
// Classes (vtable, size):
//   ModelViewer 0x4df908 0x78        the 3D view: the static at 0x5cdbf8 ($E62)
//   TextureViewer 0x4df958 0x50084   the texture view: the static at 0x57d7c8 ($E67), with capture_cb's overlays in it
//   UICustomControl 0x4db190 0x18    the base (root library)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "compiler.h"
#include "ui_types.h"

namespace edt {
using uit::Edx;
using uit::gxCanvas;

// ---- the controls ------------------------------------------------------------------------------------------------------------------
struct EdCtl {                                    // UICustomControl's fields (ui_types.h)
    const void* volatile vtbl;                    // +0
    volatile int32_t x, y, w, h;                  // +4 the item's rectangle (_UIAddItems)
    void* volatile widget;                        // +0x14 its CustomWidget
};
static_assert(sizeof(EdCtl) == 0x18, "EdCtl");

struct EdFrame {                                  // Frame: a 3x3 matrix, then the position
    volatile float m[9];
    volatile float p[3];
};
static_assert(sizeof(EdFrame) == 0x30, "EdFrame");

struct ModelViewer : EdCtl {                      // 0x78, the static at 0x5cdbf8
    volatile int32_t model;                       // +0x18 the model drawn (S_MODEL, the one rebuild_models builds)
    volatile int32_t point;                       // +0x1c point.mod (the selected vertex's marker)
    volatile int32_t capture;                     // +0x20 capture_cb's "*<file>" model, or 0 (drawn alone in mode 5)
    EdFrame* volatile frame;                      // +0x24 the model's frame (S_FRAME)
    EdFrame* volatile camera;                     // +0x28 the camera (S_CAMERA)
    EdFrame marker;                               // +0x2c the marker's frame (its position set by Draw3D)
    union { volatile float yaw; volatile uint32_t yaw_b; };      // +0x5c
    union { volatile float dist; volatile uint32_t dist_b; };    // +0x60 (held to 0.01 and more, by its bits)
    union { volatile float pitch; volatile uint32_t pitch_b; };  // +0x64
    volatile uint8_t drag;                        // +0x68
    uint8_t _69[3];
    volatile int32_t mx, my;                      // +0x6c where the drag last was
    void* volatile back;                          // +0x74 carback.stp (Draw's background)
};
static_assert(sizeof(ModelViewer) == 0x78 && offsetof(ModelViewer, marker) == 0x2c && offsetof(ModelViewer, yaw) == 0x5c &&
              offsetof(ModelViewer, back) == 0x74, "ModelViewer");

struct EdTexPt { volatile float u, v; };          // capture_cb's overlay points
struct TextureViewer : EdCtl {                    // 0x50084, the static at 0x57d7c8
    gxCanvas canvas;                              // +0x18 the texture (Create: the item's size)
    void* volatile cursor;                        // +0x3c cross.stp
    union { volatile float u; volatile uint32_t u_b; };  // +0x40 the mouse (MouseDown) / the drag's last point
    union { volatile float v; volatile uint32_t v_b; };  // +0x44
    volatile int32_t tex_size;                    // +0x48 the texture's size (rebuild_models: gxGetTextureSize)
    volatile uint8_t down;                        // +0x4c a button is held
    uint8_t _4d[3];
    union { volatile float zoom; volatile uint32_t zoom_b; };   // +0x50 (zoom_cb: 1..5)
    volatile int32_t overlay;                     // +0x54 the capture overlay shown (switch_overlay_cb), or -1..
    volatile int32_t noverlays;                   // +0x58 overlays captured (capture_cb: a surface each, 10 at most)
    volatile int32_t counts[10];                  // +0x5c each overlay's points
    EdTexPt pts[10][0x1000];                      // +0x84 the points (no bound on a surface's count: FIX CANDIDATE)
};
static_assert(sizeof(TextureViewer) == 0x50084 && offsetof(TextureViewer, cursor) == 0x3c && offsetof(TextureViewer, tex_size) == 0x48 &&
              offsetof(TextureViewer, counts) == 0x5c && offsetof(TextureViewer, pts) == 0x84, "TextureViewer");

// mrModelInfo (gx_model.cpp): counts and arrays
struct EdInfo {                                   // 0x28
    volatile int32_t nverts; uint8_t* volatile verts;      // +0 mrVertex, 0x20 each
    volatile int32_t nsurfs; uint8_t* volatile surfs;      // +8 mrSurface, 0x20 each
    volatile int32_t ntris;  uint8_t* volatile tris;       // +0x10 mrTriangle, 8 each
    volatile int32_t n18;    void* volatile p1c;           // +0x18
    volatile int32_t n20;    void* volatile p24;           // +0x20
};
static_assert(sizeof(EdInfo) == 0x28, "EdInfo");

// ModBuilderTriangle (modbuild.obj): what this group reads of one (ModBuilderGetTriangle copies it whole)
struct EdTri {                                    // 0x10
    volatile int32_t v[3];                        // +0 its vertices
    volatile uint8_t edge[3];                     // +0xc the edges' flags (a set one is smoothed; build_edge_model's wedges)
    uint8_t _f;
};
static_assert(sizeof(EdTri) == 0x10, "EdTri");

// the dialogs' items (UIDialogItem, ui_types.h): 0x38
using uit::UIDialogItem;
using uit::UIDialog;

enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_ModelViewer = 0x004df908, VT_TextureViewer = 0x004df958,
};

// ---- statics (v1.0) ------------------------------------------------------------------------------------------------------------
enum : uint32_t {
    // modtool.obj .data
    S_TV_TOOL = 0x004fe900,          // int: the texture view's tool (radio): 0 new point, 1 move point, 2 translate, 3 scale (1)
    S_LOCK = 0x004fe904,             // int: the lock (radio): 0 none, 1 u, 2 v, 3 snap to the overlay
    S_SURFACE = 0x004fe908,          // int: the surface selected
    S_VERTEX = 0x004fe90c,           // int: the vertex picked, or -1
    S_TPOINT = 0x004fe910,           // int: the texture point picked, or -1
    S_COORD_TEXT = 0x004fe918,       // char[0x20]: the texture point's coordinates ("%5.1f %5.1f", or "")
    S_ZOOM = 0x004fe938,             // int: zoom_cb's zoom (1..5)
    S_UNITS = 0x004fe93c,            // int: translate_cb's units (radio: 0 metres, 1 centimetres, 2 inches)
    // modtool.obj .bss
    S_SEL_COLOR = 0x005cd84c,        // uint32: a selected texture point's colour
    S_MODE = 0x005cd850,             // int: the 3D view's mode (radio): 0 view, 1 vertices, 2 triangles, 3 edges, 4 geometry, 5 capture
    S_TITLE = 0x005cd858,            // char[0x100]: "Surface %d (%s)" / "No surface"
    S_CAMERA = 0x005cd958,           // Frame: the camera
    S_XFORM_SET = 0x005cd988,        // u8: set_transform has set the texture transform
    S_TEXNAME = 0x005cd98c,          // char*: set_texture_cb's texture name (browse_cb's buffer)
    S_SEL = 0x005cd990,              // int[3]: the last three texture points selected (select_tp)
    S_VAXIS = 0x005cd9a0,            // UIScrollAxis: the texture view's vertical (total, visible, pos)
    S_BLACK = 0x005cd9ac,            // uint32: the background colour
    S_FILE = 0x005cd9c0,             // char[0x104]: the model's file name
    S_CANVASES = 0x005cdac8,         // gxCanvas[8]: ModTool's 128 x 128 canvases (allocated, never used)
    S_CANVASES_END = 0x005cdbe8,
    S_NEW_SURFACE = 0x005cdbe8,      // int: change_surface's argument, kept
    S_MODIFIED = 0x005cdbec,         // u8: unsaved changes
    S_EDGE_COLOR = 0x005cdbf0,       // uint32: the 3-point put's lines
    S_MV = 0x005cdbf8,               // ModelViewer
    S_MODEL = 0x005cdc74,            // mrModel: built by rebuild_models (mrModelCreate("texture-model"))
    S_INFO = 0x005cdc80,             // mrModelInfo: the model (ModBuilderBuildInfo's copy)
    S_BUILDER = 0x005cdca8,          // ModBuilder*: the model being edited
    S_INFO2 = 0x005cdcb0,            // mrModelInfo: the overlay (the surface's geometry, or the edges' wedges)
    S_HAXIS = 0x005cdcd8,            // UIScrollAxis: the texture view's horizontal
    S_UNDO = 0x005cdce4,             // ModBuilder*: the undo copy
    S_PT_COLOR = 0x005cdcec,         // uint32: the texture points' colour
    S_BG_CANVAS = 0x005cdcf0,        // gxCanvas: 128 x 128, cleared (never used)
    S_TRIANGLE = 0x005cdd18,         // int: the triangle picked, or -1
    S_SLIDER = 0x005cdd30,           // float: the zoom slider (0..20)
    S_FRAME = 0x005cdd38,            // Frame: the model's
    S_XFORM = 0x0057d798,            // Frame: the texture transform (set_transform; TextureViewer::MouseMove's scale tool)
    S_TV = 0x0057d7c8,               // TextureViewer
    // elsewhere
    S_EMPTY = 0x004e4db8,            // ""
};

// ---- the game's functions (v1.0) -----------------------------------------------------------------------------------------------
enum : uint32_t {
    // kernel, useful
    F_LogReport = 0x00411150, F_LogPanic = 0x004112b0, F_FileFindFirst = 0x00411c20, F_FileFindClose = 0x00411cd0,
    F_ScanDown = 0x004130e0, F_MemAlloc = 0x004140e0, F_Delete = 0x00414390, F_ResourceSetMustLoad = 0x00419a30,
    F_ResourceSetUnload = 0x00419bb0,
    // root, ui
    F_GetCarFileName = 0x00406910, F_GetMaxCarFileNames = 0x00406920, F_UIDialogItem_ctor = 0x004091c0,
    F_UIDoDialog = 0x00478ff0, F_UIDoOkBox = 0x004793c0, F_UIDoYesNoBox = 0x00479b30, F_UIDoYesNoCancelBox = 0x0047a0d0,
    F_UIDoOpenFileBox = 0x0047a6e0, F_UIDoSaveFileBox = 0x0047ac30, F_UICC_Dirty = 0x0047e820, F_UICC_AddNotification = 0x0047e840,
    P3DBase_ctor = 0x0045d8d0,
    // maths
    F_MatrixMakeYaw = 0x00402f80, F_MatrixMakePitch = 0x00402fc0, F_MatrixMakeRoll = 0x00403000, F_MatrixConcat = 0x00403040,
    F_VectorSub = 0x00429120, F_MatrixMakeIdentity = 0x00429150, F_MatrixMakeModelRotation = 0x00429350,
    F_MatrixMulPoint = 0x00429420, F_MatrixMulPointInv = 0x00436120, F_VectorAdd = 0x0043b0f0,
    // gfx
    F_gxGetTexture = 0x0044e240, F_gxForgetTexture = 0x0044e280, F_gxFreeAllTextures = 0x0044e350,
    F_gxGrabTexture = 0x0044e490, F_gxReleaseTexture = 0x0044e4d0, F_gxGetTextureSize = 0x0044e540,
    F_gxAllocCanvas = 0x0044f8c0, F_gxFreeCanvas = 0x0044f960, F_gxSetClip = 0x0044fdd0, F_gxRestoreClip = 0x0044fe10,
    F_gxSetCanvas = 0x0044fe30, F_gxClear = 0x0044ffd0, F_gxPoint = 0x004503c0, F_gxXORPoint = 0x00450410,
    F_gxRect = 0x004506e0, F_gxTriangle = 0x00450760, F_gxPasteZoom = 0x004517c0, F_gxLine = 0x00452790,
    F_gxCircle = 0x00452e80, F_gxGetStamp = 0x00453500, F_gxForgetStamp = 0x004535d0, F_gxDrawStamp = 0x004535f0,
    F_gxWriteBMP = 0x004d9d90,
    // the 3D renderer
    F_mrSetView = 0x0044ecd0, F_mrSetProjection = 0x0044ede0, F_mrSetCamera = 0x0044ee70, F_mrModelLoad = 0x004558c0,
    F_mrModelUnload = 0x00455950, F_mrModelCreate = 0x00455b40, F_mrModelDestroy = 0x00455bc0, F_mrModelBuild = 0x00455c80,
    F_mrModelDraw = 0x00455d00, F_mrModelInfoSave = 0x00456470, F_mrModelPick = 0x00456560, F_mrModelPickVertex = 0x00456960,
    F_mrModelPickEdge = 0x00456b70, F_mrModelGetExtents = 0x00456ec0, F_mrModelSetClipDist = 0x004570d0, F_mrLightCalc = 0x0045bf40,
    // the CRT
    F_sprintf = 0x004cf0a0, F_strrchr = 0x004cf130, F_atof = 0x004cfe40,
    // group B (modbuild.obj, cvt3ds.obj): by address
    F_ModBuilderCreate = 0x004b7050, F_ModBuilderDestroy = 0x004b7140, F_ModBuilderCopy = 0x004b7220,
    F_ModBuilderGetVertex = 0x004b7430, F_ModBuilderGetTriangle = 0x004b7520, F_ModBuilderSetTriangle = 0x004b7560,
    F_ModBuilderSmoothAll = 0x004b75b0, F_ModBuilderFacetAll = 0x004b7690, F_ModBuilderTranslateModel = 0x004b7770,
    F_ModBuilderScaleModel = 0x004b77b0, F_ModBuilderAddSurface = 0x004b7800, F_ModBuilderIsSurface = 0x004b78c0,
    F_ModBuilderSetSurfaceTexture = 0x004b78e0, F_ModBuilderGetSurfaceTexture = 0x004b7920,
    F_ModBuilderSetSurfaceMaterial = 0x004b7960, F_ModBuilderGetSurfaceMaterial = 0x004b7990,
    F_ModBuilderSetSurfaceGroup = 0x004b79c0, F_ModBuilderGetSurfaceGroup = 0x004b79e0,
    F_ModBuilderAddTextureTriangle = 0x004b7a00, F_ModBuilderGetTextureTriangle = 0x004b7ae0,
    F_ModBuilderDeleteTextureTriangle = 0x004b7b40, F_ModBuilderAddTexturePoint = 0x004b7ba0,
    F_ModBuilderGetTexturePoint = 0x004b7c70, F_ModBuilderDeleteTexturePoint = 0x004b7cc0,
    F_ModBuilderMoveTexturePoint = 0x004b7d90, F_ModBuilderBuildInfo = 0x004b7dd0, F_ModBuilderBuildGeometry = 0x004b8bd0,
    F_ModBuilderDeselectAll = 0x004b8e10, F_ModBuilderSelectTriangle = 0x004b8e90, F_ModBuilderImportDXF = 0x004b8fa0,
    F_ModBuilderImportMOD = 0x004b9800, F_ModBuilderImport3DS = 0x004b9890, F_Write3DS = 0x004ba440,
    // this group's own (called by address, as the original does, so a hooked rewrite is what runs)
    F_ModTool = 0x004b1a50, F_TV_AddPoint = 0x004b0cc0, F_select_tp = 0x004b0fd0, F_set_transform = 0x004b1190,
    F_get_textured_point = 0x004b1610, F_MatrixSet3 = 0x004b1680, F_MatrixInverse = 0x004b16d0, F_MatrixSetF = 0x004b18c0,
    F_snap = 0x004b18e0, F_is_selected = 0x004b19b0, F_change_surface = 0x004b27c0, F_update_texture_viewer = 0x004b2960,
    F_update_model = 0x004b2970, F_set_model = 0x004b2ab0, F_rebuild_models = 0x004b2ad0, F_build_edge_model = 0x004b2cd0,
    F_load_mod = 0x004b3280, F_dxf_import = 0x004b43e0, F_ad3ds_import = 0x004b4410, F_create_model_info = 0x004b4440,
    F_destroy_model_info = 0x004b4490, F_copy_model_info = 0x004b44c0, F_prompt_save = 0x004b4550, F_save_file = 0x004b45a0,
    F_save_mod = 0x004b45e0, F_reset = 0x004b4630, F_save_undo = 0x004b46d0, F_MV_set_position = 0x004b6cc0,
    F_TV_GetTextureU = 0x004b60e0, F_TV_GetTextureV = 0x004b6120, F_TV_TranslatePoints = 0x004b6160,
    F_TV_update_coords = 0x004b6c00,
    // the callbacks the originals hand out (their addresses, as data)
    F_zoom_cb = 0x004b2620, F_next_cb = 0x004b27a0, F_prev_cb = 0x004b2810, F_idle_func = 0x004b2830, F_exit_cb = 0x004b30d0,
    F_save_cb = 0x004b30e0, F_export_cb = 0x004b30f0, F_open_cb = 0x004b3210, F_add_surface_cb = 0x004b32b0,
    F_translate_cb = 0x004b33a0, F_scale_cb = 0x004b3740, F_set_texture_cb = 0x004b3a50, F_template_cb = 0x004b3ed0,
    F_browse_cb = 0x004b41c0, F_import_cb = 0x004b41f0, F_three_point_put_cb = 0x004b46b0, F_plane_proj_cb = 0x004b46c0,
    F_undo_cb = 0x004b46f0, F_capture_cb = 0x004b4710, F_measure_cb = 0x004b4920, F_toggle_cb = 0x004b49e0,
    F_switch_overlay_cb = 0x004b4a10,
};

// ---- helpers ---------------------------------------------------------------------------------------------------------------------
#define ED_P(a) ((void*)(uintptr_t)(a))
#define ED_CP(a) ((const char*)(uintptr_t)(a))
static __forceinline ModelViewer* ed_mv() { return (ModelViewer*)ED_P(S_MV); }
static __forceinline TextureViewer* ed_tv() { return (TextureViewer*)ED_P(S_TV); }
static __forceinline void* ed_builder() { return UI_GP(void, S_BUILDER); }
static __forceinline uint32_t ed_u(const volatile void* p) { return (uint32_t)(uintptr_t)p; }

// int a - b, a + b as the original's 32-bit arithmetic (wrapping)
static __forceinline int32_t ed_sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static __forceinline int32_t ed_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
// a float moved with integer instructions (mov): its bits, never through the FPU
static __forceinline void ed_mv32(volatile void* d, const volatile void* s) { *(volatile uint32_t*)d = *(const volatile uint32_t*)s; }
static __forceinline uint32_t ed_bits(const volatile void* s) { return *(const volatile uint32_t*)s; }

// st0 OP a float in memory, as `fadd / fsub / fmul dword ptr [m]` do it. Where both are NaNs and the one in memory is
// signalling, the x87 gives st0's NaN (its rule for an SNaN and a QNaN); a quietened copy loaded first would win by its
// larger significand instead. So wherever the original works on a float it moved (mov: a texture point's, a vertex's, a
// frame's -- possibly signalling) as a memory operand against a value that may itself be a NaN, the rewrite does too.
// The result is the register's, rounded by the precision control (so a double holds it).
static __forceinline double ed_addm(double x, const volatile float* m) {
    double r;
    __asm { fld x
            mov eax, m
            fadd dword ptr [eax]
            fstp r }
    return r;
}
static __forceinline double ed_subm(double x, const volatile float* m) {   // x - [m]
    double r;
    __asm { fld x
            mov eax, m
            fsub dword ptr [eax]
            fstp r }
    return r;
}
static __forceinline double ed_mulm(double x, const volatile float* m) {
    double r;
    __asm { fld x
            mov eax, m
            fmul dword ptr [eax]
            fstp r }
    return r;
}

// UIDialogItem's constructor (replay.obj) by address: every argument as the original pushes it (the floats' bits)
static __forceinline void ed_item(void* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, const void* text,
                                  int32_t i1c, const void* data, int32_t style, uint32_t lo = 0, uint32_t hi = 0) {
    uit::tcall<void*>(F_UIDialogItem_ctor, it, type, id, x, y, w, h, (const char*)text, i1c, data, style, lo, hi, (const void*)0,
                      (const char*)0);
}
// the end of an item list: the static item at 0x578d08, copied (rep movsd)
static __forceinline void ed_item_end(void* it) { uit::crt_copy(it, ED_P(uit::S_END_ITEM), 0x38); }
// the dialogs' text style: 0xb where the points' and the 3-point lines' colours are the same, else 0xc (`cmp; sbb; add`)
static __forceinline int32_t ed_text_style() { return UI_GU32(S_PT_COLOR) == UI_GU32(S_EDGE_COLOR) ? 0xb : 0xc; }

}  // namespace edt
