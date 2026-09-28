// paint_kit.h -- M3 UI stage, step U4 (group C): the paint kit's layouts, statics and callees (library `paintkit`:
// paintkit.obj, tga.obj), shared by hook/paint_kit.cpp, hook/paint_main.cpp, hook/paint_tga.cpp and test/world_paintkit.cpp.
//
// Recovered from the v1.0 disassembly (the constructors, paint_main's, template_cb's and decal_cb's frames, the methods).
// The paint kit is one dialog (paint_main): a PaintKitCanvas (the 256 x 256 painting, zoomed and scrolled, with its eight
// tools -- "modes" -- embedded), a PaintCarViewer3D (the car turning with the paint on it), a ColorChooser (a colour bar and
// a hue strip), a ColorIndicator (the left and right colours), radio buttons for the tool and the brush, buttons for the
// rest. Every control is a UICustomControl (ui_types.h). Fields are volatile, as in ui_types.h: the rewrites read and
// write each one where the original does, in its order.
//
// Classes (vtable, size):
//   PaintKitCanvas 0x4dfe48 0x15c      the painting: four canvases, the tools, the dirty rectangle, the zoom
//   Mode 0x4e0028 4                    a tool: Mouse* / Draw / GetCursor (all do nothing)
//     PencilMode 0x4dff48 0x14         a one-pixel line in the colour of the button pressed
//     BrushMode 0x4dffd0 0x10          the brush (brush.stp's four canvases, or a cut selection) stamped along the drag
//     ShapeMode 0x4dfef0 0x14          a shape dragged out (XOR outline while dragging), drawn on release:
//       LineMode 0x4dfec0, RectMode 0x4dfff8, TriangleMode 0x4dffa0, SelRectMode 0x4dff70 (cuts the brush)
//     EyeDropMode 0x4dff20 8           picks a colour
//     ZoomMode 0x4dfe98 8              zooms in (left) or out (right) around the click
//   PaintCarViewer3D 0x4dfdf8 0x60     the car model turned by dragging
//   ColorChooser 0x4dfda8 0x44         the palette (colorbar.cvs over the hue) and the hue strip
//   ColorIndicator 0x4dfd58 0x18       the two colours
//   ColorBucket 0x4e0050 0x1c          a colour swatch; clicking it takes the left colour
//   TemplateLayer 0x4e00f0 0x68        one of a template's three colour layers (with its bucket)
//   TemplatePreview 0x4e00a0 0x44      the three layers together (with the background's bucket)
//   DecalViewer 0x4e0140 0x830         the decal sets (decals.tab): a grid of decals, one set at a time
//   UICustomControl 0x4db190 0x18      the base (root library)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace pkit {
using uit::Edx;
using uit::gxCanvas;

struct PkCtl {                                    // UICustomControl's fields (ui_types.h), as the controls derive them
    const void* volatile vtbl;                    // +0
    volatile int32_t x, y, w, h;                  // +4 the item's rectangle (_UIAddItems)
    void* volatile widget;                        // +0x14 its CustomWidget
};
static_assert(sizeof(PkCtl) == 0x18, "PkCtl");

// ---- the tools (embedded in the PaintKitCanvas) -------------------------------------------------------------------------
struct PkMode {                                   // Mode: 4
    const void* volatile vtbl;
};
struct PencilMode : PkMode {                      // 0x14
    void* volatile cursor;                        // +4 c_pencil.stp
    volatile uint32_t color;                      // +8
    volatile int32_t x, y;                        // +0xc the last point
};
static_assert(sizeof(PencilMode) == 0x14, "PencilMode");
struct BrushMode : PkMode {                       // 0x10
    volatile uint32_t color;                      // +4
    volatile int32_t x, y;                        // +8 the last point
};
static_assert(sizeof(BrushMode) == 0x10, "BrushMode");
struct ShapeMode : PkMode {                       // 0x14 (Line, Rect, Triangle, SelRect)
    volatile int32_t x0, y0;                      // +4 where the drag started
    volatile int32_t x1, y1;                      // +0xc where it is
};
static_assert(sizeof(ShapeMode) == 0x14, "ShapeMode");
struct CursorMode : PkMode {                      // 8 (EyeDrop, Zoom)
    void* volatile cursor;                        // +4
};
static_assert(sizeof(CursorMode) == 8, "CursorMode");

// ---- paintkit.obj's controls -------------------------------------------------------------------------------------------------
struct PaintKitCanvas : PkCtl {                   // 0x15c (paint_main's frame)
    volatile int32_t paint;                       // +0x18 the paint job (constructor argument): -1.. (the file's number)
    gxCanvas canvas;                              // +0x1c the painting (256 x 256, format 5)
    gxCanvas* volatile overlay;                   // +0x40 gxCanvasGet("shade.cvs"): a viper's shading, pasted over it
    gxCanvas view;                                // +0x44 what is drawn: the painting, the overlay, the tool's preview
    gxCanvas undo;                                // +0x68 the painting before the last stroke
    gxCanvas redo;                                // +0x8c undo_cb's scratch
    void* volatile cursor;                        // +0xb0 cross.stp (a tool without its own)
    volatile uint8_t down;                        // +0xb4 a button is held (drags go to the tool)
    volatile uint8_t dirty;                       // +0xb5 the dirty rectangle is set (Update copies it to the view)
    volatile uint8_t over;                        // +0xb6 the mouse is over it (the tool draws its preview)
    uint8_t _b7;
    PencilMode pencil;                            // +0xb8 tool 0
    BrushMode brush;                              // +0xcc tool 1
    ShapeMode line;                               // +0xdc tool 2 (LineMode)
    CursorMode eyedrop;                           // +0xf0 tools 5, 6
    ShapeMode rect;                               // +0xf8 tool 3 (RectMode)
    ShapeMode triangle;                           // +0x10c tool 4 (TriangleMode)
    ShapeMode selrect;                            // +0x120 tool 7 (SelRectMode)
    CursorMode zoom_mode;                         // +0x134 tool 8
    volatile int32_t dx0, dy0, dx1, dy1;          // +0x13c the dirty rectangle (x1, y1 exclusive)
    volatile int32_t mx, my;                      // +0x14c the mouse, in the painting's pixels
    volatile int32_t zoom;                        // +0x154 1..5
    volatile uint8_t modified;                    // +0x158 unsaved changes
    uint8_t _159[3];
};
static_assert(sizeof(PaintKitCanvas) == 0x15c && offsetof(PaintKitCanvas, pencil) == 0xb8 && offsetof(PaintKitCanvas, zoom_mode) == 0x134 &&
              offsetof(PaintKitCanvas, dx0) == 0x13c && offsetof(PaintKitCanvas, zoom) == 0x154, "PaintKitCanvas");

struct PkFrame {                                  // Frame: a 3x3 matrix, then the position
    volatile float m[9];
    volatile float p[3];
};
static_assert(sizeof(PkFrame) == 0x30, "PkFrame");
struct PaintCarViewer3D : PkCtl {                 // 0x60
    PkFrame body;                                 // +0x18 the car's frame (turned by the drag)
    void* volatile cursor;                        // +0x48 c_hand.stp
    volatile int32_t mx, my;                      // +0x4c where the drag last was
    union { volatile float yaw; volatile uint32_t yaw_b; };      // +0x54 (kept to -pi..pi, compared by its bits)
    union { volatile float pitch; volatile uint32_t pitch_b; };  // +0x58 (held to -pi/2..pi/2, by its bits)
    volatile uint8_t drag;                        // +0x5c
    uint8_t _5d[3];
};
static_assert(sizeof(PaintCarViewer3D) == 0x60 && offsetof(PaintCarViewer3D, yaw) == 0x54, "PaintCarViewer3D");

struct ColorChooser : PkCtl {                     // 0x44
    gxCanvas canvas;                              // +0x18 the hue under the colour bar
    gxCanvas* volatile bar;                       // +0x3c colorbar.cvs
    void* volatile cursor;                        // +0x40 the eye dropper
};
static_assert(sizeof(ColorChooser) == 0x44, "ColorChooser");

struct ColorIndicator : PkCtl {};                 // 0x18

struct ColorBucket : PkCtl {                      // 0x1c
    volatile uint32_t* volatile var;              // +0x18 the colour it shows and sets
};
static_assert(sizeof(ColorBucket) == 0x1c, "ColorBucket");

struct TemplateLayer : PkCtl {                    // 0x68 (template_cb's frame: three)
    volatile int32_t frame;                       // +0x18 the template stamp's frame (the layer)
    gxCanvas canvas;                              // +0x1c the layer in its colour (gxMakeBrush)
    gxCanvas* volatile overlay;                   // +0x40 gxCanvasGet("shade.cvs")
    volatile uint32_t* volatile bg;               // +0x44 the background colour
    ColorBucket bucket;                           // +0x48 its colour's swatch
    volatile uint32_t* volatile color;            // +0x64 its colour
};
static_assert(sizeof(TemplateLayer) == 0x68 && offsetof(TemplateLayer, bucket) == 0x48, "TemplateLayer");

struct TemplatePreview : PkCtl {                  // 0x44
    gxCanvas* volatile overlay;                   // +0x18 gxCanvasGet("shade.cvs")
    TemplateLayer* volatile layers;               // +0x1c
    volatile int32_t count;                       // +0x20 (3)
    volatile uint32_t* volatile bg;               // +0x24 the background colour
    ColorBucket bucket;                           // +0x28 the background's swatch
};
static_assert(sizeof(TemplatePreview) == 0x44 && offsetof(TemplatePreview, bucket) == 0x28, "TemplatePreview");

struct DecalSet {                                 // 0x1c: a row of decals.tab
    gxCanvas* volatile canvas;                    // +0 decals<n>.cvs: the set's decals one under another
    const char* volatile name;                    // +4 Xlate("Paintkit:DecalSet:<column 1>")
    volatile int32_t count;                       // +8 column 0
    volatile int32_t w;                           // +0xc a decal's width (the canvas's)
    volatile int32_t h;                           // +0x10 ... height (the canvas's / count)
    volatile int32_t cols;                        // +0x14 (Create: the viewer's width / w)
    volatile int32_t rows;                        // +0x18 (Create)
};
static_assert(sizeof(DecalSet) == 0x1c, "DecalSet");
struct DecalViewer : PkCtl {                      // 0x830 (decal_cb's frame)
    DecalSet sets[64];                            // +0x18 (64 at most: decal_cb has no bound)
    volatile int32_t count;                       // +0x718
    volatile int32_t sel;                         // +0x71c the decal under the mouse, or -1 (watched)
    char name[0x100];                             // +0x720 the set's name (the title's text)
    volatile uint8_t drag;                        // +0x820
    uint8_t _821[3];
    uit::UIScrollAxis axis;                       // +0x824 rows, visible rows, the first shown (watched)
};
static_assert(sizeof(DecalViewer) == 0x830 && offsetof(DecalViewer, count) == 0x718 && offsetof(DecalViewer, axis) == 0x824, "DecalViewer");

// the TGA header (tga.obj), 0x12 bytes
#pragma pack(push, 1)
struct TGAHeader {
    volatile uint8_t id_length;                   // +0
    volatile uint8_t color_map_type;              // +1
    volatile uint8_t image_type;                  // +2 (2: true colour)
    volatile uint8_t cmap[5];                     // +3
    volatile uint16_t x_origin, y_origin;         // +8
    volatile uint16_t width, height;              // +0xc
    volatile uint8_t pixel_depth;                 // +0x10 (0x18)
    volatile uint8_t descriptor;                  // +0x11 (8)
};
#pragma pack(pop)
static_assert(sizeof(TGAHeader) == 0x12, "TGAHeader");

enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_PaintKitCanvas = 0x004dfe48, VT_Mode = 0x004e0028, VT_PencilMode = 0x004dff48,
    VT_BrushMode = 0x004dffd0, VT_ShapeMode = 0x004dfef0, VT_LineMode = 0x004dfec0, VT_RectMode = 0x004dfff8,
    VT_TriangleMode = 0x004dffa0, VT_SelRectMode = 0x004dff70, VT_EyeDropMode = 0x004dff20, VT_ZoomMode = 0x004dfe98,
    VT_PaintCarViewer3D = 0x004dfdf8, VT_ColorChooser = 0x004dfda8, VT_ColorIndicator = 0x004dfd58,
    VT_ColorBucket = 0x004e0050, VT_TemplateLayer = 0x004e00f0, VT_TemplatePreview = 0x004e00a0, VT_DecalViewer = 0x004e0140,
};
// a tool's virtual methods (Mode's vtable)
enum : uint32_t {
    MO_MouseDown = 4, MO_MouseUp = 8, MO_MouseRDown = 0xc, MO_MouseRUp = 0x10, MO_MouseDrag = 0x14, MO_MouseMove = 0x18,
    MO_Draw = 0x1c, MO_GetCursor = 0x20, MO_DrawShape = 0x24, MO_dirty_shape = 0x28,
};
// a UICustomControl's (ui_types.h)
enum : uint32_t { CC_Callback = 0x10, CC_MouseMove = 0x28 };

// ---- statics (v1.0) ---------------------------------------------------------------------------------------------------------
enum : uint32_t {
    // paintkit.obj .data
    S_CAR = 0x00500960,              // char[0x20] the car's name (paint_begin: strcpy, no bound -- FIX: 31 characters)
    S_BRUSH = 0x00500980,            // int: the brush (1..4: brush.stp's frames; 0: the cut selection) (1)
    S_HUE_Y = 0x00500984,            // int ColorChooser::hue_y: the hue strip's mark, or -1
    S_TEMPLATE = 0x00500988,         // int: the template shown
    S_LAYER_COLORS = 0x00500990,     // uint32[3]: the template layers' colours (template_cb, the first time)
    S_DECAL_SET = 0x0050099c,        // int: the decal set shown
    S_ZOOM_CB = 0x005009a0,          // int: zoom_cb's zoom (1..4)
    // paintkit.obj .bss
    S_BRUSH_CANVAS = 0x005d42d8,     // gxCanvas: the cut selection (the brush 0)
    S_VAXIS = 0x005d4300,            // UIScrollAxis: the painting's vertical scroll (zoom * 256, the height, the top)
    S_HAXIS = 0x005d4400,            // UIScrollAxis: ... horizontal
    S_HSCROLL = 0x005d4408, S_VSCROLL = 0x005d4308,   // their positions
    S_TOOL = 0x005d42a0,             // int: the tool (0..8, the radio buttons')
    S_SAVED_TOOL = 0x005d43fc,       // int: the tool before the eye dropper (5, 6) took over
    S_TEXTURE = 0x005d42b4,          // gxCreateTexture's handle: the car's paint
    S_CANVAS = 0x005d4334,           // PaintKitCanvas*: the one (Mode's and the callbacks')
    S_BRUSHES = 0x005d4428,          // gxCanvas*[5]: [0] the cut selection, [1..4] brush%d.cvs
    S_BRUSHES_END = 0x005d443c,
    S_MODEL = 0x005d43c4,            // the car's model (mrModelLoad)
    S_CAMERA = 0x005d4388,           // Frame: the 3D view's camera (paint_begin)
    S_IS_VIPER = 0x005d448c,         // u8: the car is the viper (its paint jobs are paint<n>, its template shown over)
    S_LEFT = 0x005d44a4,             // uint32: the left button's colour
    S_RIGHT = 0x005d43c0,            // uint32: the right button's colour
    S_HUE = 0x005d43d4,              // uint32 ColorChooser::hue
    S_BG = 0x005d43b8,               // uint32: the template's background colour
    S_CLEAR = 0x005d432c,            // uint32: black (the empty painting)
    S_WHITE = 0x005d43bc,            // uint32
    S_GREY = 0x005d43c8,             // uint32 (a bucket's frame)
    S_YELLOW = 0x005d4458,           // uint32 (the decal under the mouse)
    S_C_29C = 0x005d429c, S_C_49C = 0x005d449c,       // the layer colours' first values
    S_LAYERS = 0x005d444c,           // TemplateLayer*: template_cb's three (left pointing at its frame)
    S_NLAYERS = 0x005d43e8,          // int: 3
    S_NTEMPLATES = 0x005d4450,       // int: templates found (tmplt%d.stp, 32 at most)
    S_TEMPLATES = 0x005d44e0,        // gxStamp*[32]
    S_TEMPLATE_NAME = 0x005d44b0,    // char[0x20]: "%d" (the template's number)
    S_DECAL_VIEWER = 0x005d4560,     // DecalViewer::g (left pointing at decal_cb's frame)
    // function-local statics' guards and Xlators
    S_MAIN_ONCE = 0x005d43e4,        // paint_main's: 1 0x5d4460, 2 0x5d44d0, 4 0x5d4310, 8 0x5d4280, 0x10 0x5d4338, 0x20 0x5d4368
    S_EXIT_ONCE = 0x005d44dc,        // exit_cb's: 1 0x5d4440, 2 0x5d4290
    S_DEFAULT_ONCE = 0x005d4330,     // default_cb's: 1 0x5d4358, 2 0x5d42a8
    S_TEMPLATE_ONCE = 0x005d4420,    // template_cb's: 1 0x5d43d8, 2 0x5d4270, 4 0x5d4480, 8 0x5d42b8, 0x10 / 0x20 its statics
    S_DECAL_ONCE = 0x005d4454,       // decal_cb's: 1 0x5d4320, 2 0x5d42c8, 4 0x5d4410
    S_EXPORT_ONCE = 0x005d5ddc,      // export_cb's: 1 0x5d5df0, 2 0x5d5de0
    S_IMPORT_ONCE = 0x005d5dbc,      // import_cb's: 1 0x5d5dd0, 2 0x5d5dc0
    // elsewhere
    S_TEX_TYPE = 0x004f4208, S_TEX_VER = 0x004f420c,  // TEX_TYPE, TEX_VER (the .tex header)
    S_EMPTY = 0x004e4db8,            // ""
};

// ---- the game's functions (v1.0) --------------------------------------------------------------------------------------------
enum : uint32_t {
    // gfx
    F_gxSetCanvas = 0x0044fe30, F_gxPaste = 0x00451500, F_gxPasteAlpha = 0x00451550, F_gxPasteZoom = 0x004517c0,
    F_gxSetClip = 0x0044fdd0, F_gxRestoreClip = 0x0044fe10, F_gxClear = 0x0044ffd0, F_gxClearNoAlpha = 0x00450020,
    F_gxRect = 0x004506e0, F_gxLine = 0x00452790, F_gxPoint = 0x004503c0, F_gxXORLine = 0x00452c50,
    F_gxBrushLine = 0x004529f0, F_gxTriangle = 0x00450760, F_gxGetPixel = 0x00450520, F_gxCut = 0x004514a0,
    F_gxFlip = 0x004530e0, F_gxMirror = 0x00452fa0, F_gxRotate90 = 0x00453230, F_gxMakeBrush = 0x004537a0,
    F_gxStampCount = 0x004539a0, F_gxGetStamp = 0x00453500, F_gxForgetStamp = 0x004535d0,
    F_gxAllocCanvas = 0x0044f8e0,    // (canvas, w, h, format)
    F_gxFreeCanvas = 0x0044f960, F_gxCanvasGet = 0x0044fd40, F_gxCanvasForget = 0x0044fdc0, F_gxCanvasRead = 0x0044fa80,
    F_gxCanvasWrite = 0x0044fc70, F_gxCreateTexture = 0x0044e2c0, F_gxDestroyTexture = 0x0044e310,
    F_gxGrabTexture = 0x0044e490, F_gxReleaseTexture = 0x0044e4d0,
    // the 3D renderer
    F_mrSetView = 0x0044ecd0, F_mrSetProjection = 0x0044ede0, F_mrSetCamera = 0x0044ee70, F_mrModelLoad = 0x004558c0,
    F_mrModelUnload = 0x00455950, F_mrModelEnvMap = 0x00455cf0, F_mrModelDraw = 0x00455d00,
    F_mrModelSetClipDist = 0x004570d0, F_mrEnable = 0x00457890, F_mrPushState = 0x00457ea0, F_mrPopState = 0x00457ed0,
    F_MatrixMakeYaw = 0x00402f80, F_MatrixMakePitch = 0x00402fc0, F_MatrixMakeRoll = 0x00403000, F_MatrixConcat = 0x00403040,
    // kernel
    F_LogReport = 0x00411150, F_LogPanic = 0x004112b0, F_FileCreate = 0x004115f0, F_FileOpen = 0x00411780,
    F_FileClose = 0x00411850, F_FileReadExact = 0x004118b0, F_FileWrite = 0x00411a30, F_FileCreateDirectory = 0x00411d00,
    F_Win32GetUserDirectory = 0x00412cc0, F_ScanDown = 0x004130e0, F_MemAlloc = 0x004140e0, F_Delete = 0x00414390,
    F_ResourceSetMustLoad = 0x00419a30, F_ResourceSetUnload = 0x00419bb0, F_ResourceExists = 0x00419d10,
    F_Xlate = 0x0041aec0, F_StringTableGet = 0x0041b210, F_StringTableForget = 0x0041b270,
    F_StringTableNumRows = 0x0041b290, F_StringTableGetEntry = 0x0041b2a0,
    // root, ui
    F_GetCarFileName = 0x00406910, F_GetMaxCarFileNames = 0x00406920, F_UIDialogItem_ctor = 0x004091c0,
    F_UIDoOkBox = 0x004793c0, F_UIDoYesNoBox = 0x00479b30, F_UIDoYesNoCancelBox = 0x0047a0d0, F_UIDoDialog = 0x00478ff0,
    F_UICC_Dirty = 0x0047e820, F_UICC_AddNotification = 0x0047e840, F_UICC_AddItems = 0x0047e870, F_WidgetExit = 0x0047ff60,
    // CRT
    F_atexit = 0x004ceff0, F_sprintf = 0x004cf0a0, F_atoi = 0x004cf990, F_stricmp = 0x004da350,
    // this group's own (called by address, as the original does, so a hooked rewrite is what runs)
    F_Mode_Dirty = 0x004c5ed0, F_Mode_SetCanvas = 0x004c5f80, F_Mode_SaveForUndo = 0x004c5fa0, F_paint_begin = 0x004c6300,
    F_paint_end = 0x004c6430, F_paint_main = 0x004c6490, F_color_brush = 0x004c6d90, F_update_template_name = 0x004c7e70,
    F_write_tex = 0x004c88d0, F_make_mip = 0x004c8a70, F_PaintKitCanvas_ctor = 0x004c8e10, F_ShapeMode_ctor = 0x004c94c0,
    F_PaintKitCanvas_Default = 0x004c9b80, F_PaintKitCanvas_dtor = 0x004c9ce0, F_PaintKitCanvas_DirtyRect = 0x004ca7a0,
    F_PaintCarViewer3D_ctor = 0x004cab70, F_TemplateLayer_ctor = 0x004cb380, F_TemplateLayer_dtor = 0x004cb3e0,
    F_WriteTGA = 0x004cbf60, F_ReadTGA = 0x004cc100, F_compare_headers = 0x004cc2a0,
    // the callbacks and atexit destructors the originals hand out (their addresses, as data)
    F_save_cb = 0x004c6db0, F_exit_cb = 0x004c7060, F_default_cb = 0x004c7400, F_template_cb = 0x004c7630,
    F_undo_cb = 0x004c7ed0, F_flip_cb = 0x004c7fd0, F_mirror_cb = 0x004c7ff0, F_rotate_cb = 0x004c8010,
    F_decal_cb = 0x004c8050, F_zoom_cb = 0x004c84b0, F_export_cb = 0x004c8af0, F_import_cb = 0x004c8c10,
    F_next_template = 0x004c8dc0, F_prev_template = 0x004c8df0, F_DecalViewer_NextSet = 0x004cb840,
    F_DecalViewer_PrevSet = 0x004cb910,
};

// ---- helpers ------------------------------------------------------------------------------------------------------------------
#define PK_P(a) ((void*)(uintptr_t)(a))
#define PK_CP(a) ((const char*)(uintptr_t)(a))
static __forceinline PaintKitCanvas* pk_g() { return UI_GP(PaintKitCanvas, S_CANVAS); }
static __forceinline uint32_t pk_u(const volatile void* p) { return (uint32_t)(uintptr_t)p; }

// a function-local static Xlator, built on first use: the guard's bit set, the constructor, its destructor at exit
static __forceinline void pk_xl_once(uint32_t guard, uint8_t bit, uint32_t xl, uint32_t key, uint32_t dtor) {
    if (!(UI_G8(guard) & bit)) {
        UI_G8(guard) = (uint8_t)(UI_G8(guard) | bit);
        uit::tcall<void*>(uit::F_Xlator_ctor, PK_P(xl), PK_CP(key));
        uit::ccall<int>(F_atexit, dtor);
    }
}
static __forceinline bool pk_xl_built(uint32_t guard, uint8_t bits) { return (UI_G8(guard) & bits) == bits; }

// UIDialogItem::UIDialogItem (replay.obj) by address: every argument as the original pushes it (the floats' bits)
static __forceinline void pk_item(void* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, const void* text,
                                  int32_t i1c, const void* data, int32_t style) {
    uit::tcall<void*>(F_UIDialogItem_ctor, it, type, id, x, y, w, h, (const char*)text, i1c, data, style, (uint32_t)0, (uint32_t)0,
                      (const void*)0, (const char*)0);
}
// the end of an item list: the static item at 0x578d08, copied (rep movsd)
static __forceinline void pk_item_end(void* it) { uit::crt_copy(it, PK_P(uit::S_END_ITEM), 0x38); }

// int a - b, a + b as the original's 32-bit arithmetic (wrapping)
static __forceinline int32_t isub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static __forceinline int32_t iadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static __forceinline int32_t imul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

// ---- FIX helpers (docs/FIXES.md, "Paint kit") -----------------------------------------------------------------------------------
enum : uint32_t {
    PK_CAR_MAX = 0x1f,               // the characters a car's name may have: S_CAR's, and the root car list's (RaceBegin's FIX)
    PK_PATH_MAX = 0x103,             // the characters a path may have: the game's 0x104-byte path buffers
    S_CANT_CREATE_SAVE = 0x00500de4, // "Can't create %s" (save's)
    S_CANT_CREATE_TEX = 0x00500d18,  // "Can't create %s" (write_tex's)
};
// the characters the game's %d prints for v (a '-' and the digits)
static __forceinline uint32_t pk_dec_len(int32_t v) {
    uint32_t n = v < 0 ? 2u : 1u;
    uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    while (u >= 10u) { u /= 10u; n++; }
    return n;
}
// strlen(s), counting at most `max` + 1 characters (a longer string is known to be longer than max)
static __forceinline uint32_t pk_len(const char* s, uint32_t max) { return uit::ui_strnlen(s, max); }
// would "<user directory>" and `tail` characters after it overrun a 0x104-byte path buffer?
static __forceinline bool pk_path_too_long(const char* ud, uint32_t tail) { return pk_len(ud, PK_PATH_MAX) + tail > PK_PATH_MAX; }
// the log's "Can't create %s" (the game's own text, `cant`) for a path too long to be built: the user directory, then
// `rest` (the format's part after it, formatted by the caller with the directory ""), cut to 200 characters -- what
// LogReport's 0x100 bytes hold with the text
static __forceinline void pk_log_path(uint32_t cant, const char* ud, const char* rest) {
    char m[0xc9];
    uint32_t n = pk_len(ud, 0xc8);
    if (n > 0xc8) n = 0xc8;
    uit::crt_copy(m, ud, n);
    uit::ui_copy_bounded(m + n, rest, 0xc9 - n);
    ((uit::Log_t)(uintptr_t)F_LogReport)(PK_CP(cant), (const char*)m);
}
// a car's name for such a log line: as it is, or its first 0x40 characters in `buf`
static __forceinline const char* pk_cut(const char* s, char* buf /* 0x41 */) {
    if (pk_len(s, 0x40) <= 0x40) return s;
    uit::ui_copy_bounded(buf, s, 0x41);
    return buf;
}

}  // namespace pkit
