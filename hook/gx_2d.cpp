// gx_2d.cpp -- M3 graphics stage, step G1, group G1a: the software 2D canvas and what draws with it, rewritten
// faithfully (library `gx`, plus the debug screens of `ai` and `phys`).
//
//   gfx.obj      the canvas (gxCanvas): building / allocating / loading / saving canvases, the current canvas and its
//                clip rectangle, clears, points, rectangles, triangles, lines (plain, brush, XOR), circles, copies
//                between 16- and 32-bit canvases, alpha blits, zoomed pastes, the pixel-format converters, mirror /
//                flip / rotate, stamps (gxStamp: the RLE sprites) and brushes, palettes, and the 8x8 text
//   font.obj     proportional fonts (gxFont: a stamp and per-character tables): widths, metrics, printing
//   pal.obj      the system palette and palette conversion to the screen's 555 / 565
//   bmp.obj      .bmp reading and writing (the paint kit's)
//   graph.obj    GraphDraw*: the debug graphs (line plots, axes, the bounding box)
//   ai.obj, notes.obj, physdash.obj, phystask.obj   the debug screens drawn with it: AIDashboardDraw, draw_status,
//                draw_focus_iline, draw_mouse_cursor, draw_target, AIDisplayLearnMode, learn_draw, clear_screen,
//                learn_dialog, PhysicsDashboardDraw, draw_tire_dash, CameraDashboardDraw
//
// Written from the v1.0 disassembly (tools/disasm.py), every call in the original's order by its v1.0 address (this
// group's own functions too, so a hooked rewrite is what runs), the game's C runtime by address, register values as
// double and stored values as float (docs/PORTING.md). All of it is integer pixel code except the zoomed pastes, the
// triangle's edges, the palette gradient, the graphs and the dashboards, whose x87 sequences are kept.
//
// Pixels. The canvas writes straight into memory: a locked DirectDraw surface (the back buffer, 565), or a canvas's
// own buffer. Pixel loads and stores go through volatile pointers of the original's width (a 16-bit canvas is copied
// two pixels at a time as dwords where the original does), so a copy between overlapping rectangles of one canvas
// reads and writes in the original's order, and the compiler can't merge or reorder them.
//
// Footprints: everything here runs on the main thread, so each lists every static it writes; a pixel routine lists
// the span of its canvas's rows its clip rectangle allows (fp_clip), widened where the original writes past the clip
// (see FIX CANDIDATES). Functions that allocate, free or read / write files are replay_only.
//
// Threads / locks: the gx_sync critical section (_SingleEnter / _SingleLeave, an ownership check that writes
// nothing) is entered and left exactly where the original does -- gxGetPixel enters and never leaves.
//
// FIX CANDIDATES (faithful here; see the report):
//   * (fixed: gxTriangle) gxTriangle's edge tables are 1024-entry stack arrays indexed by row: a current canvas taller
//     than 1024 rows writes past them (the stack). fill_line only advances x on rows inside the canvas, so a triangle
//     poking above row 0 is drawn skewed (not fixed: it is how the game draws).
//   * gxPasteDouble clips its bottom edge against 2 x the source's height, not y + 2h, and writes one row and one
//     column past the clip rectangle when the clipped size is odd.
//   * cvt_555_8888 / cvt_565_8888 return unsigned short: copy_16_to_32 stores (G << 8 | B), red and alpha lost.
//   * the 16-bit clears / rects / copies run their row loops at least once (do / while), so an empty or negative
//     canvas size in gfxReduceTo555 / gxMirror / gxFlip / rotate still touches the first pixel.
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
#define FB(b) __builtin_bit_cast(float, (uint32_t)(b))   // a float constant from its bits

#define G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define G16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define G32(a) (*(volatile int32_t*)(uintptr_t)(a))
#define GU32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define GF(a) (*(volatile float*)(uintptr_t)(a))
#define GP(T, a) (*(T* volatile*)(uintptr_t)(a))
#define FP_ADD(f, addr, n, what) (f).add((void*)(uintptr_t)(addr), (uint32_t)(n), (what))

// Everything below is file-local: one anonymous namespace to the end of the file.
namespace {

// ---- layouts -------------------------------------------------------------------------------------------------------
struct gxCanvas {                  // 0x24
    int8_t format;                 // +0: 1 4444, 3 555, 4 565, 5 8888
    uint8_t flags;                 // +1: 0x80 wraps outside memory, 0x82 owns it
    uint16_t _pad;
    uint8_t* pixels;               // +4
    int32_t w, h;                  // +8, +0xc
    int32_t pitch;                 // +0x10 bytes
    int32_t cx0, cy0, cx1, cy1;    // +0x14 clip rectangle (x1, y1 exclusive)
};
static_assert(sizeof(gxCanvas) == 0x24, "gxCanvas");

// ---- statics ---------------------------------------------------------------------------------------------------------
enum : uint32_t {
    S_CANVAS = 0x004efbf8,         // gfx.obj: the current canvas
    S_GX_SYNC = 0x005228d0,        // gx.obj: the critical section
    S_SCREEN_FORMAT = 0x00522b20,  // vid.obj: screen_format
    S_OFF_CANVAS = 0x00522a44,     // what gxGetPixel returns outside the clip rectangle
};
#define CUR GP(gxCanvas, S_CANVAS)

// ---- pixels, through the original's widths ----------------------------------------------------------------------------
static __forceinline uint16_t r16(const void* p) { return *(const volatile uint16_t*)p; }
static __forceinline uint32_t r32(const void* p) { return *(const volatile uint32_t*)p; }
static __forceinline void w16(void* p, uint32_t v) { *(volatile uint16_t*)p = (uint16_t)v; }
static __forceinline void w32(void* p, uint32_t v) { *(volatile uint32_t*)p = v; }
// an fst / fstp dword the original always makes, kept where the value then goes unused (docs/PORTING.md 10a)
static __forceinline float st(double d) {
    volatile float f = (float)d;
    return f;
}
static __forceinline int32_t imul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static __forceinline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static __forceinline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

// the 32-bit colour argument's 565 half as 8888: ff RR GG BB (clear_32, point_32, rect_32)
static __forceinline uint32_t c565_to_8888(uint32_t c) {
    return 0xff000000u | (((c >> 24) & 0xf8) << 16) | (((c >> 19) & 0xfc) << 8) | ((c >> 13) & 0xf8);
}

// ---- footprints -------------------------------------------------------------------------------------------------------
// the bytes of c's rows from (x0, y0) to (x1, y1), inclusive, as one span: what a routine clipped to it can write
static void fp_span(Footprint& f, const gxCanvas* c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t bpp, const char* what) {
    if (!c || !c->pixels || c->pitch <= 0 || x1 < x0 || y1 < y0) return;
    const int64_t a = (int64_t)y0 * c->pitch + (int64_t)x0 * bpp;
    const int64_t b = (int64_t)y1 * c->pitch + (int64_t)(x1 + 1) * bpp;
    if (b <= a || b - a > 0x4000000) return;
    f.add(c->pixels + a, (uint32_t)(b - a), what);
}
static __forceinline int32_t bpp_of(const gxCanvas* c) { return c->format == 5 ? 4 : 2; }
// the clip rectangle of c; for a 32-bit canvas from where a 16-bit writer (gxDrawStamp, which doesn't check the
// format) would start, to where a 32-bit one ends
static void fp_clip(Footprint& f, const gxCanvas* c, const char* what = "canvas pixels (clip)") {
    if (!c) return;
    if (bpp_of(c) == 2) {
        fp_span(f, c, c->cx0, c->cy0, c->cx1 - 1, c->cy1 - 1, 2, what);
        return;
    }
    if (!c->pixels || c->pitch <= 0 || c->cx1 <= c->cx0 || c->cy1 <= c->cy0) return;
    const int64_t a = (int64_t)c->cy0 * c->pitch + (int64_t)c->cx0 * 2;
    const int64_t b = (int64_t)(c->cy1 - 1) * c->pitch + (int64_t)c->cx1 * 4;
    if (b > a && b - a <= 0x4000000) f.add(c->pixels + a, (uint32_t)(b - a), what);
}
// the whole canvas, at least one row and one pixel (the do / while loops)
static void fp_whole(Footprint& f, const gxCanvas* c, int32_t bpp, const char* what = "canvas pixels") {
    if (c) fp_span(f, c, 0, 0, (c->w > 1 ? c->w : 1) - 1, (c->h > 1 ? c->h : 1) - 1, bpp, what);
}
static void fp_cur_clip(Footprint& f) { fp_clip(f, CUR, "the current canvas's pixels (clip)"); }

// ---- the game's functions (by v1.0 address) ----------------------------------------------------------------------------
typedef void(__cdecl* SingleOp_t)(int, const char*, int);
static const SingleOp_t SingleEnter = (SingleOp_t)0x00415000, SingleLeave = (SingleOp_t)0x00415070;
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogReport = (Log_t)0x00411150, LogPanic = (Log_t)0x004112b0;
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* MemFree_t)(void*);
static const MemFree_t MemFree = (MemFree_t)0x00414300;
typedef int(__cdecl* FileOpen_t)(const char*);
static const FileOpen_t FileOpen = (FileOpen_t)0x00411780, FileCreate = (FileOpen_t)0x004115f0;
typedef uint8_t(__cdecl* FileIO_t)(int, void*, int);
static const FileIO_t FileReadExact = (FileIO_t)0x004118b0;
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
static const FileWrite_t FileWrite = (FileWrite_t)0x00411a30;
typedef void(__cdecl* FileClose_t)(int&);
static const FileClose_t FileClose = (FileClose_t)0x00411850;
typedef void*(__cdecl* ResourceGet_t)(const char*, uint32_t, uint32_t&, int*, uint8_t*, uint8_t*);
static const ResourceGet_t ResourceGet = (ResourceGet_t)0x00419fa0;
typedef uint8_t(__cdecl* ResourceForget_t)(void*);
static const ResourceForget_t ResourceForget = (ResourceForget_t)0x0041a450;

static __forceinline void gx_enter() { SingleEnter(G32(S_GX_SYNC), 0, 0); }
static __forceinline void gx_leave() { SingleLeave(G32(S_GX_SYNC), 0, 0); }

// this file's own functions, by address
typedef void(__cdecl* Void_t)();
typedef void(__cdecl* U32_t)(uint32_t);
typedef void(__cdecl* CanvasFn_t)(gxCanvas*);
typedef gxCanvas*(__cdecl* SetCanvas_t)(gxCanvas*);
typedef uint8_t(__cdecl* Adjust_t)(gxCanvas*, int32_t*, int32_t*, int32_t*, int32_t*);
typedef uint8_t(__cdecl* InClip_t)(gxCanvas*, int32_t, int32_t);
typedef uint8_t(__cdecl* Shrink_t)(gxCanvas*, int32_t&, int32_t&, int32_t&, int32_t&, int32_t&, int32_t&);
typedef uint16_t*(__cdecl* Addr16_t)(gxCanvas*, int32_t, int32_t);
typedef uint32_t*(__cdecl* Addr32_t)(gxCanvas*, int32_t, int32_t);
typedef void(__cdecl* Point_t)(int32_t, int32_t, uint32_t);
typedef void(__cdecl* XY_t)(int32_t, int32_t);
typedef uint32_t(__cdecl* ReadPt_t)(int32_t, int32_t);
typedef void(__cdecl* Rect_t)(int32_t, int32_t, int32_t, int32_t, uint32_t);
typedef void(__cdecl* FillLine_t)(int32_t, int32_t, int32_t, int32_t, int32_t*, int32_t*);
typedef void(__cdecl* Blit_t)(gxCanvas*, int32_t, int32_t, int32_t, int32_t, gxCanvas*, int32_t, int32_t);
typedef void(__cdecl* Zoom_t)(gxCanvas*, int32_t, int32_t, float, float);
typedef uint16_t(__cdecl* Cvt16_t)(uint16_t);
typedef uint16_t(__cdecl* Cvt32_t)(uint32_t);
typedef Cvt16_t(__cdecl* GetCvt_t)(int8_t, int8_t);
typedef void(__cdecl* AllocCanvas4_t)(gxCanvas*, int32_t, int32_t, int8_t);
typedef void(__cdecl* AllocCanvas3_t)(gxCanvas*, int32_t, int32_t);
static const Void_t gx_reset_o = (Void_t)0x0044f870;
static const AllocCanvas3_t gxAllocCanvas3_o = (AllocCanvas3_t)0x0044f8c0;
static const AllocCanvas4_t gxAllocCanvas4_o = (AllocCanvas4_t)0x0044f8e0;
static const CanvasFn_t gxFreeCanvas_o = (CanvasFn_t)0x0044f960;
static const CanvasFn_t gxRestoreClip_o = (CanvasFn_t)0x0044fe10;
static const SetCanvas_t gxSetCanvas_o = (SetCanvas_t)0x0044fe30;
static const Adjust_t adjust_to_clip_o = (Adjust_t)0x0044fe70;
static const InClip_t in_clip_o = (InClip_t)0x0044fee0;
static const Shrink_t shrink_rect_o = (Shrink_t)0x0044ff10;
static const U32_t clear_noalpha_16_o = (U32_t)0x00450050, clear_noalpha_32_o = (U32_t)0x00450060;
static const U32_t clear_16_o = (U32_t)0x00450170, clear_32_o = (U32_t)0x004502c0, clear_32_32_o = (U32_t)0x00450300;
static const Addr32_t calc_address_32_o = (Addr32_t)0x00450150;
static const Addr16_t calc_address_16_o = (Addr16_t)0x004502a0;
static const Point_t point_16_o = (Point_t)0x00450460, point_32_o = (Point_t)0x004504b0;
static const ReadPt_t read_point_16_o = (ReadPt_t)0x00450560, read_point_32_o = (ReadPt_t)0x004505c0;
static const XY_t xor_point_16_o = (XY_t)0x00450660, xor_point_32_o = (XY_t)0x004506a0;
static const Rect_t gxRect_o = (Rect_t)0x004506e0;
static const Point_t gxPoint_o = (Point_t)0x004503c0;
static const U32_t gxClear_o = (U32_t)0x0044ffd0;
typedef void(__cdecl* Paste_t)(gxCanvas*, int32_t, int32_t);
static const Paste_t gxPaste_o = (Paste_t)0x00451500;
static const FillLine_t fill_line_o = (FillLine_t)0x00450900;
static const Rect_t rect_16_o = (Rect_t)0x004509c0, rect_32_o = (Rect_t)0x00450a80, rect_4444_o = (Rect_t)0x00450b70;
static const Blit_t gxCopy_o = (Blit_t)0x00450c60, gxAlphaBlit_o = (Blit_t)0x00450d80;
static const Blit_t copy_16_to_16_o = (Blit_t)0x00450e70, copy_32_to_32_o = (Blit_t)0x004510b0;
static const Blit_t copy_16_to_32_o = (Blit_t)0x00451190, copy_32_to_16_o = (Blit_t)0x004512f0;
static const Cvt32_t cvt_555_8888_o = (Cvt32_t)0x00451740, cvt_565_8888_o = (Cvt32_t)0x00451780;
static const Zoom_t paste_zoom_16_16_o = (Zoom_t)0x00451870, paste_zoom_32_16_o = (Zoom_t)0x00451c30,
                    paste_zoom_32_32_o = (Zoom_t)0x00451eb0;
static const GetCvt_t get_cvt_fn_o = (GetCvt_t)0x00451a70;
static const Cvt32_t cvt_8888_555_o = (Cvt32_t)0x00451e50, cvt_8888_565_o = (Cvt32_t)0x00451e80;
static const Blit_t alpha_16_to_16_o = (Blit_t)0x00452070, alpha_32_to_16_o = (Blit_t)0x00452370,
                    alpha_32_to_32_o = (Blit_t)0x00452620;
typedef uint16_t(__cdecl* Blend_t)(uint16_t, uint32_t);
static const Blend_t alpha_blend_8888_to_555_o = (Blend_t)0x004524c0, alpha_blend_8888_to_565_o = (Blend_t)0x00452570;

// =====================================================================================================================
// gfx.obj: canvases
// =====================================================================================================================

// gfx_begin / gfx_end: jmp gx_reset
static void __cdecl gfx_begin_n() { gx_reset_o(); }
static void fp_gfx_begin(Footprint& f) { FP_ADD(f, S_CANVAS, 4, "gfx.obj current canvas"); }
PORT_FN(0x0044f850, "gfx_begin", gfx_begin_n, fp_gfx_begin)
static void __cdecl gfx_end_n() { gx_reset_o(); }
PORT_FN(0x0044f860, "gfx_end", gfx_end_n, fp_gfx_begin)
static void __cdecl gx_reset_n() { G32(S_CANVAS) = 0; }
PORT_FN(0x0044f870, "gx_reset", gx_reset_n, fp_gfx_begin)

static void __cdecl gxBuildCanvas_n(gxCanvas* c, int8_t format, char* pixels, int32_t w, int32_t h, int32_t pitch) {
    c->format = format;
    c->pixels = (uint8_t*)pixels;
    c->w = w;
    c->h = h;
    c->flags = 0x80;
    c->pitch = pitch;
    gxRestoreClip_o(c);
}
static void fp_gxBuildCanvas(Footprint& f, gxCanvas* c, int8_t, char*, int32_t, int32_t, int32_t) { f.add(c, sizeof(gxCanvas), "canvas"); }
PORT_FN(0x0044f880, "gxBuildCanvas", gxBuildCanvas_n, fp_gxBuildCanvas)

static void __cdecl gxAllocCanvas3_n(gxCanvas* c, int32_t w, int32_t h) { gxAllocCanvas4_o(c, w, h, (int8_t)G8(S_SCREEN_FORMAT)); }
static void fp_gxAllocCanvas3(Footprint& f, gxCanvas*, int32_t, int32_t) { f.replay_only = "allocates the canvas's pixels"; }
PORT_FN(0x0044f8c0, "gxAllocCanvas(w,h)", gxAllocCanvas3_n, fp_gxAllocCanvas3)

static void __cdecl gxAllocCanvas4_n(gxCanvas* c, int32_t w, int32_t h, int8_t format) {
    gx_enter();
    c->format = format;
    c->flags = 0x82;
    const int32_t bpp = (uint8_t)(format - 5) < 1 ? 4 : 2;     // sub al,5; cmp al,1; sbb: format == 5
    const int32_t pitch = imul(bpp, w);
    c->pixels = (uint8_t*)MemAlloc(imul(h, pitch));
    c->w = w;
    c->h = h;
    c->pitch = pitch;
    gxRestoreClip_o(c);
    gx_leave();
}
static void fp_gxAllocCanvas4(Footprint& f, gxCanvas*, int32_t, int32_t, int8_t) { f.replay_only = "allocates the canvas's pixels"; }
PORT_FN(0x0044f8e0, "gxAllocCanvas(w,h,format)", gxAllocCanvas4_n, fp_gxAllocCanvas4)

static void __cdecl gxFreeCanvas_n(gxCanvas* c) {
    gx_enter();
    MemFree(c->pixels);
    memset(c, 0, sizeof(gxCanvas));                            // rep stosd, 9 dwords
    gx_leave();
}
static void fp_gxFreeCanvas(Footprint& f, gxCanvas*) { f.replay_only = "frees the canvas's pixels"; }
PORT_FN(0x0044f960, "gxFreeCanvas", gxFreeCanvas_n, fp_gxFreeCanvas)

typedef struct gxStamp gxStamp;
typedef gxStamp*(__cdecl* GetStamp_t)(const char*);
typedef int32_t(__cdecl* StampInt_t)(const gxStamp*);
typedef void(__cdecl* ForgetStamp_t)(gxStamp*);
typedef void(__cdecl* DrawStamp_t)(const gxStamp*, int32_t, int32_t, int32_t, const void*);
static const GetStamp_t gxGetStamp_o = (GetStamp_t)0x00453500;
static const StampInt_t gxStampWidth_o = (StampInt_t)0x00453980, gxStampHeight_o = (StampInt_t)0x00453990;
static const ForgetStamp_t gxForgetStamp_o = (ForgetStamp_t)0x004535d0;
static const DrawStamp_t gxDrawStamp_o = (DrawStamp_t)0x004535f0;

// gxCanvasLoad: a stamp drawn into a new canvas. The canvas is made current and never put back: the first
// gxSetCanvas's result is dropped and the second sets the same canvas again (faithful).
static void __cdecl gxCanvasLoad_n(gxCanvas* b, const char* name) {
    gx_enter();
    gxStamp* s = gxGetStamp_o(name);
    if (s) {
        const int32_t h = gxStampHeight_o(s);
        const int32_t w = gxStampWidth_o(s);
        gxAllocCanvas3_o(b, w, h);
        gxSetCanvas_o(b);
        gxDrawStamp_o(s, 0, 0, 0, 0);
        gxSetCanvas_o(b);
        gxForgetStamp_o(s);
    } else {
        LogPanic((const char*)0x004f1b1c);
    }
    gx_leave();
}
static void fp_gxCanvasLoad(Footprint& f, gxCanvas*, const char*) { f.replay_only = "loads a stamp, allocates a canvas"; }
PORT_FN(0x0044f9a0, "gxCanvasLoad", gxCanvasLoad_n, fp_gxCanvasLoad)

static void __cdecl gxCanvasUnload_n(gxCanvas* b) {
    gx_enter();
    gxFreeCanvas_o(b);
    gx_leave();
}
static void fp_gxCanvasUnload(Footprint& f, gxCanvas*) { f.replay_only = "frees a canvas"; }
PORT_FN(0x0044fa40, "gxCanvasUnload", gxCanvasUnload_n, fp_gxCanvasUnload)

// gxCanvasRead: a 'RES0' 'VNAC' 1 / 0 'MGI!' / gxCanvas header, then the pixels, into a canvas of the same shape
static uint8_t __cdecl gxCanvasRead_n(gxCanvas* c, const char* name) {
    struct { int32_t fh; uint32_t hdr[3]; gxCanvas file; uint32_t tag[2]; } L;
    uint8_t bad = 1;
    gx_enter();
    L.fh = FileOpen(name);
    if (L.fh) {
        if (!FileReadExact(L.fh, L.hdr, 0xc) || L.hdr[0] != 0x52455330 || L.hdr[1] != 0x43414e56 || L.hdr[2] != 1 ||
            !FileReadExact(L.fh, L.tag, 8) || L.tag[1] != 0x4d474921 || !FileReadExact(L.fh, &L.file, 0x24)) {
            LogReport((const char*)0x004f1c28);
        } else {
            bad = 0;
            if (L.file.format != c->format) {
                bad = 1;
                LogReport((const char*)0x004f1b54, (const char*)0x004f1b4c, (int32_t)L.file.format, (int32_t)c->format);
            }
            if (L.file.flags != c->flags) {
                bad = 1;
                LogReport((const char*)0x004f1b7c, (const char*)0x004f1b74, (uint32_t)L.file.flags, (uint32_t)c->flags);
            }
            if (L.file.w != c->w) { bad = 1; LogReport((const char*)0x004f1ba0, (const char*)0x004f1b9c, L.file.w, c->w); }
            if (L.file.h != c->h) { bad = 1; LogReport((const char*)0x004f1bc4, (const char*)0x004f1bc0, L.file.h, c->h); }
            if (L.file.pitch != c->pitch) { bad = 1; LogReport((const char*)0x004f1bec, (const char*)0x004f1be4, L.file.pitch, c->pitch); }
            if (!bad && !FileReadExact(L.fh, c->pixels, imul(c->h, c->pitch))) {
                bad = 1;
                LogReport((const char*)0x004f1c0c);
            }
        }
        FileClose(L.fh);
    }
    gx_leave();
    return bad < 1;
}
static void fp_gxCanvasRead(Footprint& f, gxCanvas*, const char*) { f.replay_only = "reads a file"; }
PORT_FN(0x0044fa80, "gxCanvasRead", gxCanvasRead_n, fp_gxCanvasRead)

static uint8_t __cdecl gxCanvasWrite_n(const gxCanvas* c, const char* name) {
    struct { int32_t fh; uint32_t tag[2]; uint32_t hdr[3]; } L;
    uint8_t bad = 1;
    L.fh = FileCreate(name);
    if (L.fh) {
        L.hdr[0] = 0x52455330;
        L.hdr[2] = 1;
        L.hdr[1] = 0x43414e56;
        L.tag[1] = 0x4d474921;
        L.tag[0] = 0;
        FileWrite(L.fh, L.hdr, 0xc);
        FileWrite(L.fh, L.tag, 8);
        FileWrite(L.fh, c, 0x24);
        bad = 0;
        FileWrite(L.fh, c->pixels, imul(c->h, c->pitch));
        FileClose(L.fh);
    } else {
        LogReport((const char*)0x004f1c48, name);
    }
    return bad < 1;
}
static void fp_gxCanvasWrite(Footprint& f, const gxCanvas*, const char*) { f.replay_only = "writes a file"; }
PORT_FN(0x0044fc70, "gxCanvasWrite", gxCanvasWrite_n, fp_gxCanvasWrite)

// gxCanvasGet: a canvas resource ('VNAC', version 1); a freshly loaded one gets its pixel pointer fixed up
static gxCanvas* __cdecl gxCanvasGet_n(const char* name) {
    struct { uint8_t fresh; uint32_t version; int32_t size; } L;
    gxCanvas* c = (gxCanvas*)ResourceGet(name, 0x43414e56, L.version, &L.size, 0, &L.fresh);
    if (!c) {
        LogPanic((const char*)0x004f1c94, name);
        return c;
    }
    if (L.version != 1) {
        LogPanic((const char*)0x004f1c6c, name, L.version, 1);
        return c;
    }
    if (L.fresh) c->pixels = (uint8_t*)c + 0x24;
    return c;
}
static void fp_gxCanvasGet(Footprint& f, const char*) { f.replay_only = "loads a resource"; }
PORT_FN(0x0044fd40, "gxCanvasGet", gxCanvasGet_n, fp_gxCanvasGet)

static void __cdecl gxCanvasForget_n(gxCanvas* c) { ResourceForget(c); }
static void fp_gxCanvasForget(Footprint& f, gxCanvas*) { f.replay_only = "releases a resource"; }
PORT_FN(0x0044fdc0, "gxCanvasForget", gxCanvasForget_n, fp_gxCanvasForget)

static void __cdecl gxSetClip_n(gxCanvas* c, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    c->cx0 = x0 > 0 ? x0 : 0;
    c->cy0 = y0 > 0 ? y0 : 0;
    { const int32_t w = c->w; c->cx1 = w < x1 ? w : x1; }
    { const int32_t h = c->h; c->cy1 = h < y1 ? h : y1; }
}
static void fp_gxSetClip(Footprint& f, gxCanvas* c, int32_t, int32_t, int32_t, int32_t) { f.add(c, sizeof(gxCanvas), "canvas"); }
PORT_FN(0x0044fdd0, "gxSetClip", gxSetClip_n, fp_gxSetClip)

static void __cdecl gxRestoreClip_n(gxCanvas* c) {
    const int32_t w = c->w;
    c->cx0 = 0;
    c->cy0 = 0;
    c->cx1 = w;
    c->cy1 = c->h;
}
static void fp_gxRestoreClip(Footprint& f, gxCanvas* c) { f.add(c, sizeof(gxCanvas), "canvas"); }
PORT_FN(0x0044fe10, "gxRestoreClip", gxRestoreClip_n, fp_gxRestoreClip)

static gxCanvas* __cdecl gxSetCanvas_n(gxCanvas* c) {
    gx_enter();
    gxCanvas* old = CUR;
    CUR = c;
    gx_leave();
    return old;
}
static void fp_gxSetCanvas(Footprint& f, gxCanvas*) { FP_ADD(f, S_CANVAS, 4, "gfx.obj current canvas"); }
PORT_FN(0x0044fe30, "gxSetCanvas", gxSetCanvas_n, fp_gxSetCanvas)

// adjust_to_clip: clamp (x0, y0)-(x1, y1) to c's clip rectangle; false if it's empty. The first x0 is read once.
static uint8_t __cdecl adjust_to_clip_n(gxCanvas* c, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    const int32_t ox0 = *x0;
    if (!(c->cx1 > ox0)) return 0;
    if (!(*y0 < c->cy1)) return 0;
    if (c->cx0 > ox0) *x0 = c->cx0;
    if (*y0 < c->cy0) *y0 = c->cy0;
    if (*x1 > c->cx1) *x1 = c->cx1;
    if (*y1 > c->cy1) *y1 = c->cy1;
    if (!(*x1 > *x0)) return 0;
    return *y1 > *y0;
}
static void fp_adjust_to_clip(Footprint& f, gxCanvas*, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    f.add(x0, 4, "x0"); f.add(y0, 4, "y0"); f.add(x1, 4, "x1"); f.add(y1, 4, "y1");
}
PORT_FN(0x0044fe70, "adjust_to_clip", adjust_to_clip_n, fp_adjust_to_clip)

static uint8_t __cdecl in_clip_n(gxCanvas* c, int32_t x, int32_t y) {
    if (c->cx0 > x || c->cy0 > y || c->cx1 <= x || c->cy1 <= y) return 0;
    return 1;
}
static void fp_in_clip(Footprint&, gxCanvas*, int32_t, int32_t) {}
PORT_FN(0x0044fee0, "in_clip", in_clip_n, fp_in_clip)

// shrink_rect(dst, x, y, sx, sy, w, h): clip a w x h blit at (x, y) against dst's clip, moving the source corner.
// The references may alias (callers pass their own argument slots); each is read where the original reads it.
static uint8_t __cdecl shrink_rect_n(gxCanvas* c, int32_t& x, int32_t& y, int32_t& sx, int32_t& sy, int32_t& w, int32_t& h) {
    int32_t cx0 = c->cx0;
    if (add32(w, x) < cx0) return 0;
    if (add32(h, y) < c->cy0) return 0;
    if (!(c->cx1 > x)) return 0;
    if (!(c->cy1 > y)) return 0;
    if (cx0 > x) {
        cx0 -= x;
        sx += cx0;
        w -= cx0;
        x = c->cx0;
    }
    {
        int32_t cy0 = c->cy0;
        const int32_t oy = y;
        if (cy0 > oy) {
            cy0 -= oy;
            sy += cy0;
            h -= cy0;
            y = c->cy0;
        }
    }
    {
        const int32_t ox = x, cx1 = c->cx1;
        if (add32(w, ox) > cx1) w = cx1 - ox;
    }
    {
        const int32_t oy = y, cy1 = c->cy1;
        if (add32(h, oy) > cy1) h = cy1 - oy;
    }
    if (!(w > 0)) return 0;
    return h > 0;
}
static void fp_shrink_rect(Footprint& f, gxCanvas*, int32_t& x, int32_t& y, int32_t& sx, int32_t& sy, int32_t& w, int32_t& h) {
    f.add(&x, 4, "x"); f.add(&y, 4, "y"); f.add(&sx, 4, "sx"); f.add(&sy, 4, "sy"); f.add(&w, 4, "w"); f.add(&h, 4, "h");
}
PORT_FN(0x0044ff10, "shrink_rect", shrink_rect_n, fp_shrink_rect)

// =====================================================================================================================
// gfx.obj: clears, points, rectangles, triangles
// =====================================================================================================================
static void fp_cur_u32(Footprint& f, uint32_t) { fp_cur_clip(f); }

static void __cdecl gxClear_n(uint32_t c) {
    gx_enter();
    if (CUR->format == 5) clear_32_o(c);
    else clear_16_o(c);
    gx_leave();
}
PORT_FN(0x0044ffd0, "gxClear", gxClear_n, fp_cur_u32)

static void __cdecl gxClearNoAlpha_n(uint32_t c) {
    if (CUR->format == 5) clear_noalpha_32_o(c);
    else clear_noalpha_16_o(c);
}
PORT_FN(0x00450020, "gxClearNoAlpha", gxClearNoAlpha_n, fp_cur_u32)

static void __cdecl clear_noalpha_16_n(uint32_t) { LogPanic((const char*)0x004f1cac); }
static void fp_nothing_u32(Footprint&, uint32_t) {}
PORT_FN(0x00450050, "clear_noalpha_16", clear_noalpha_16_n, fp_nothing_u32)

// the whole canvas clamped to its clip: x0 = y0 = 0, x1 = w, y1 = h
struct ClipBox { int32_t y0, x0, y1, x1; };
static __forceinline uint8_t clip_whole(ClipBox& b) {
    gxCanvas* c = CUR;
    b.x0 = 0;
    b.y0 = 0;
    b.x1 = c->w;
    b.y1 = c->h;
    return adjust_to_clip_o(CUR, &b.x0, &b.y0, &b.x1, &b.y1);
}

static void __cdecl clear_noalpha_32_n(uint32_t c) {
    const uint32_t rgb = (((c >> 24) & 0xf8) << 16) | (((c >> 19) & 0xfc) << 8) | ((c >> 13) & 0xf8);
    ClipBox b;
    if (!clip_whole(b)) return;
    int32_t rows = b.y1 - b.y0;
    const int32_t cols = b.x1 - b.x0;
    const int32_t skip = (CUR->pitch - cols * 4) / 4;
    uint8_t* p = (uint8_t*)calc_address_32_o(CUR, b.x0, b.y0);
    do {
        int32_t n = cols;
        do {
            w32(p, (r32(p) & 0xff000000u) | rgb);
            p += 4;
        } while (--n > 0);
        p += skip * 4;
    } while (--rows > 0);
}
PORT_FN(0x00450060, "clear_noalpha_32", clear_noalpha_32_n, fp_cur_u32)

static uint32_t* __cdecl calc_address_32_n(gxCanvas* c, int32_t x, int32_t y) { return (uint32_t*)(c->pixels + imul(c->pitch, y) + x * 4); }
static void fp_calc_address(Footprint&, gxCanvas*, int32_t, int32_t) {}
PORT_FN(0x00450150, "calc_address_32", calc_address_32_n, fp_calc_address)

static void __cdecl clear_16_n(uint32_t c) {
    uint32_t v = CUR->format == 3 ? (c & 0xffff) : (c >> 16);
    ClipBox b;
    if (!clip_whole(b)) return;
    int32_t rows = b.y1 - b.y0;
    int32_t cols = b.x1 - b.x0;
    const int32_t skip = (CUR->pitch - cols * 2) / 2;
    if (cols & 1) {
        uint8_t* p = (uint8_t*)calc_address_16_o(CUR, b.x0, b.y0);
        do {
            int32_t n = cols;
            do {
                w16(p, v);
                p += 2;
            } while (--n > 0);
            p += skip * 2;
        } while (--rows > 0);
        return;
    }
    uint8_t* p = (uint8_t*)calc_address_16_o(CUR, b.x0, b.y0);
    v = (v & 0xffff) << 16 | (v & 0xffff);
    cols = cols / 2;
    const int32_t skip2 = (CUR->pitch - cols * 4) / 4;
    do {
        int32_t n = cols;
        do {
            w32(p, v);
            p += 4;
        } while (--n > 0);
        p += skip2 * 4;
    } while (--rows > 0);
}
PORT_FN(0x00450170, "clear_16", clear_16_n, fp_cur_u32)

static uint16_t* __cdecl calc_address_16_n(gxCanvas* c, int32_t x, int32_t y) { return (uint16_t*)(c->pixels + imul(c->pitch, y) + x * 2); }
PORT_FN(0x004502a0, "calc_address_16", calc_address_16_n, fp_calc_address)

static void __cdecl clear_32_n(uint32_t c) { clear_32_32_o(c565_to_8888(c)); }
PORT_FN(0x004502c0, "clear_32", clear_32_n, fp_cur_u32)

static void __cdecl clear_32_32_n(uint32_t v) {
    ClipBox b;
    if (!clip_whole(b)) return;
    int32_t rows = b.y1 - b.y0;
    const int32_t cols = b.x1 - b.x0;
    const int32_t skip = (CUR->pitch - cols * 4) / 4;
    uint8_t* p = (uint8_t*)calc_address_32_o(CUR, b.x0, b.y0);
    do {
        int32_t n = cols;
        do {
            w32(p, v);
            p += 4;
        } while (--n > 0);
        p += skip * 4;
    } while (--rows > 0);
}
PORT_FN(0x00450300, "clear_32_32", clear_32_32_n, fp_cur_u32)

static void fp_point(Footprint& f, int32_t, int32_t, uint32_t) { fp_cur_clip(f); }
static void fp_xy(Footprint& f, int32_t, int32_t) { fp_cur_clip(f); }
static void __cdecl gxPoint_n(int32_t x, int32_t y, uint32_t c) {
    gx_enter();
    if (CUR->format == 5) point_32_o(x, y, c);
    else point_16_o(x, y, c);
    gx_leave();
}
PORT_FN(0x004503c0, "gxPoint", gxPoint_n, fp_point)

static void __cdecl gxXORPoint_n(int32_t x, int32_t y) {
    gx_enter();
    if (CUR->format == 5) xor_point_32_o(x, y);
    else xor_point_16_o(x, y);
    gx_leave();
}
PORT_FN(0x00450410, "gxXORPoint", gxXORPoint_n, fp_xy)

static void __cdecl point_16_n(int32_t x, int32_t y, uint32_t c) {
    if (!in_clip_o(CUR, x, y)) return;
    const uint32_t v = CUR->format == 3 ? (c & 0xffff) : (c >> 16);
    w16(calc_address_16_o(CUR, x, y), v);
}
PORT_FN(0x00450460, "point_16", point_16_n, fp_point)

static void __cdecl point_32_n(int32_t x, int32_t y, uint32_t c) {
    if (!in_clip_o(CUR, x, y)) return;
    uint32_t* p = calc_address_32_o(CUR, x, y);
    w32(p, c565_to_8888(c));
}
PORT_FN(0x004504b0, "point_32", point_32_n, fp_point)

// gxGetPixel enters gx_sync and never leaves it (faithful)
static uint32_t __cdecl gxGetPixel_n(int32_t x, int32_t y) {
    gx_enter();
    if (CUR->format == 5) return read_point_32_o(x, y);
    return read_point_16_o(x, y);
}
static void fp_nothing_xy(Footprint&, int32_t, int32_t) {}
PORT_FN(0x00450520, "gxGetPixel", gxGetPixel_n, fp_nothing_xy)

static uint32_t __cdecl read_point_16_n(int32_t x, int32_t y) {
    if (!in_clip_o(CUR, x, y)) return GU32(S_OFF_CANVAS);
    const uint32_t v = r16(calc_address_16_o(CUR, x, y));
    return v << 16 | v;                                        // both formats alike
}
PORT_FN(0x00450560, "read_point_16", read_point_16_n, fp_nothing_xy)

static uint32_t __cdecl read_point_32_n(int32_t x, int32_t y) {
    if (!in_clip_o(CUR, x, y)) return GU32(S_OFF_CANVAS);
    const uint32_t p = r32(calc_address_32_o(CUR, x, y));
    const uint32_t b = (p & 0xff) >> 3, g = (p >> 8) & 0xff, r = ((p >> 16) & 0xff) >> 3;
    return (((g >> 2) << 16 | r << 22 | r << 5 | (g >> 3)) << 5) | b << 16 | b;
}
PORT_FN(0x004505c0, "read_point_32", read_point_32_n, fp_nothing_xy)

static void __cdecl xor_point_16_n(int32_t x, int32_t y) {
    if (!in_clip_o(CUR, x, y)) return;
    uint16_t* p = calc_address_16_o(CUR, x, y);
    w16(p, ~(uint32_t)r16(p));
}
PORT_FN(0x00450660, "xor_point_16", xor_point_16_n, fp_xy)

static void __cdecl xor_point_32_n(int32_t x, int32_t y) {
    if (!in_clip_o(CUR, x, y)) return;
    uint32_t* p = calc_address_32_o(CUR, x, y);
    w32(p, ~r32(p));
}
PORT_FN(0x004506a0, "xor_point_32", xor_point_32_n, fp_xy)

static void fp_rect(Footprint& f, int32_t, int32_t, int32_t, int32_t, uint32_t) { fp_cur_clip(f); }
static void __cdecl gxRect_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
    gx_enter();
    const int8_t fmt = CUR->format;
    if (fmt == 5) rect_32_o(x0, y0, x1, y1, c);
    else if (fmt == 1) rect_4444_o(x0, y0, x1, y1, c);
    else rect_16_o(x0, y0, x1, y1, c);
    gx_leave();
}
PORT_FN(0x004506e0, "gxRect", gxRect_n, fp_rect)

// gxTriangle / fill_line: one edge's x per row into two edge tables on gxTriangle's stack (left, right), then one gxRect
// per row. Both test a row against the current canvas's height, not the tables': stock they hold 1024 rows, so a canvas
// taller than that (the dash's tachometer needle at 1920x1080, drawn at height - 80) made fill_line write past them
// (the stack: a crash at a garbage EIP) and the fill read past the first into the second (red streaks).
// FIX: the tables hold 2048 rows each (as vrmod's tablefix.py makes them), and both fill_line's walker and the fill
// skip a row past them as well as one off the canvas (vrmod's needlefix.py bounds); the rows a canvas taller than
// 2048 has below that aren't drawn (drawing is clipped to the canvas anyway). The two are one fix: gxTriangle's
// rewrite calls fill_line's directly, never an original with other tables (vrmod's race.exe has both patched, and the
// stock check accepts exactly those bytes for both: tools/gen_port_tables.py, VRMOD_PATCHES). Every row of a canvas
// up to 1024 high comes out as the original's.
enum { TRI_ROWS = VP_FIX ? 2048 : 1024 };
static void __cdecl fill_line_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t* left, int32_t* right);
static void __cdecl gxTriangle_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c) {
    int32_t left[TRI_ROWS], right[TRI_ROWS];
    const FillLine_t fill = VP_FIX ? &fill_line_n : fill_line_o;   // FIX: (above) the two halves together
    gx_enter();
    if (imul(sub32(x1, x0), sub32(y2, y0)) < imul(sub32(x2, x0), sub32(y1, y0))) {
        fill(x0, y0, x1, y1, left, right);
        fill(x1, y1, x2, y2, left, right);
        fill(x2, y2, x0, y0, left, right);
    } else {
        fill(x1, y1, x0, y0, left, right);
        fill(x2, y2, x1, y1, left, right);
        fill(x0, y0, x2, y2, left, right);
    }
    int32_t lo = y2, hi = y2;
    if (!(lo < y1)) lo = y1;
    if (!(lo < y0)) lo = y0;
    if (!(hi > y1)) hi = y1;
    if (!(hi > y0)) hi = y0;
    for (int32_t y = lo; y < hi; y++) {
        if ((int32_t)((uint32_t)y * 4) < 0 || !(CUR->h > y)) continue;
        if (VP_FIX && y >= TRI_ROWS) continue;             // FIX: (above) a row past the tables
        const int32_t l = left[y], r = right[y];
        if (l < r) gxRect_o(l, y, r, y + 1, c);
    }
    gx_leave();
}
static void fp_gxTriangle(Footprint& f, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t) { fp_cur_clip(f); }
PORT_FN(0x00450760, "gxTriangle", gxTriangle_n, fp_gxTriangle)

// fill_line: one edge's x per row into the left table (going down) or the right (going up). x only advances on
// rows inside the current canvas (faithful: an edge starting above row 0 is shifted). FIX: (gxTriangle, above) and
// inside the tables.
static void __cdecl fill_line_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t* left, int32_t* right) {
    if (y1 == y0) return;
    int32_t* t = y1 > y0 ? left : right;
    const float slope = (float)(D(sub32(x1, x0)) / D(sub32(y1, y0)));      // fild; fidiv; fstp dword
    const float fx = (float)(y1 > y0 ? x0 : x1);                             // fild; fstp dword
    const int32_t lo = y1 < y0 ? y1 : y0;
    const int32_t hi = y1 > y0 ? y1 : y0;
    if (hi <= lo) return;
    double x = fx;
    const double s = slope;
    for (int32_t y = lo; y < hi; y++) {
        if (y < 0 || !(CUR->h > y)) continue;
        if (VP_FIX && y >= TRI_ROWS) continue;             // FIX: (gxTriangle) a row past the tables
        t[y] = x87_ftol(x);
        x = x + s;
    }
    volatile float st_x = (float)x;                                           // fstp dword [esp+0xc]
    (void)st_x;
}
static void fp_fill_line(Footprint& f, int32_t, int32_t y0, int32_t, int32_t y1, int32_t* left, int32_t* right) {
    int32_t lo = y1 < y0 ? y1 : y0, hi = y1 > y0 ? y1 : y0;
    if (lo < 0) lo = 0;
    if (CUR && hi > CUR->h) hi = CUR->h;
    if (VP_FIX && hi > TRI_ROWS) hi = TRI_ROWS;
    if (hi > lo) {
        f.add(left + lo, (uint32_t)(hi - lo) * 4, "left edges");
        f.add(right + lo, (uint32_t)(hi - lo) * 4, "right edges");
    }
}
PORT_FN(0x00450900, "fill_line", fill_line_n, fp_fill_line)

// the rectangle's corners are clamped in place (adjust_to_clip on the argument slots), then filled row by row
static void __cdecl rect_16_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
    if (x1 == x0 || y1 == y0) return;
    if (!adjust_to_clip_o(CUR, &x0, &y0, &x1, &y1)) return;
    const int32_t cols = x1 - x0;
    int32_t rows = y1 - y0;
    const uint32_t v = CUR->format == 3 ? (c & 0xffff) : (c >> 16);
    uint8_t* p = (uint8_t*)calc_address_16_o(CUR, x0, y0);
    const int32_t skip = ((CUR->pitch - cols * 2) / 2) * 2;
    do {
        int32_t n = cols;
        do {
            w16(p, v);
            p += 2;
        } while (--n > 0);
        p += skip;
    } while (--rows > 0);
}
PORT_FN(0x004509c0, "rect_16", rect_16_n, fp_rect)

static void __cdecl rect_32_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
    if (x0 == x1 || y1 == y0) return;
    if (!adjust_to_clip_o(CUR, &x0, &y0, &x1, &y1)) return;
    const int32_t cols = x1 - x0;
    int32_t rows = y1 - y0;
    const uint32_t v = c565_to_8888(c);
    uint8_t* p = (uint8_t*)calc_address_32_o(CUR, x0, y0);
    const int32_t skip = ((CUR->pitch - cols * 4) / 4) * 4;
    do {
        int32_t n = cols;
        do {
            w32(p, v);
            p += 4;
        } while (--n > 0);
        p += skip;
    } while (--rows > 0);
}
PORT_FN(0x00450a80, "rect_32", rect_32_n, fp_rect)

// rect_4444: the 555 half as 4444, keeping each pixel's alpha nibble
static void __cdecl rect_4444_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
    if (x1 == x0 || y1 == y0) return;
    if (!adjust_to_clip_o(CUR, &x0, &y0, &x1, &y1)) return;
    const int32_t cols = x1 - x0;
    int32_t rows = y1 - y0;
    const uint32_t v = (((c >> 7) & 0xf0) << 4) | ((c >> 2) & 0xf0) | ((c & 0x1e) >> 1);
    uint8_t* p = (uint8_t*)calc_address_16_o(CUR, x0, y0);
    const int32_t skip = ((CUR->pitch - cols * 2) / 2) * 2;
    do {
        int32_t n = cols;
        do {
            w16(p, (r16(p) & 0xf000) | v);
            p += 2;
        } while (--n > 0);
        p += skip;
    } while (--rows > 0);
}
PORT_FN(0x00450b70, "rect_4444", rect_4444_n, fp_rect)

// =====================================================================================================================
// gfx.obj: copies and pastes
// =====================================================================================================================
static void fp_blit(Footprint& f, gxCanvas*, int32_t, int32_t, int32_t, int32_t, gxCanvas* dst, int32_t, int32_t) {
    fp_clip(f, dst, "the destination's pixels (clip)");
}

static void __cdecl gxCopy_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    gx_enter();
    const int8_t fs = src->format;
    if (fs != 5 && dst->format != 5) copy_16_to_16_o(src, sx0, sy0, sx1, sy1, dst, dx, dy);
    else if (fs == 5 && dst->format == 5) copy_32_to_32_o(src, sx0, sy0, sx1, sy1, dst, dx, dy);
    else if (fs != 5 && dst->format == 5) copy_16_to_32_o(src, sx0, sy0, sx1, sy1, dst, dx, dy);
    else if (fs == 5 && dst->format != 5) copy_32_to_16_o(src, sx0, sy0, sx1, sy1, dst, dx, dy);
    else LogPanic((const char*)0x004f1cd0, (int32_t)fs, (int32_t)dst->format);     // (unreachable)
    gx_leave();
}
PORT_FN(0x00450c60, "gxCopy", gxCopy_n, fp_blit)

static void __cdecl gxAlphaBlit_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    gx_enter();
    const int8_t fs = src->format;
    if (fs != 5 && dst->format != 5) alpha_16_to_16_o(src, sx0, sy0, sx1, sy1, dst, dx, dy);
    else if (fs == 5 && dst->format == 5) alpha_32_to_32_o(src, sx0, sy0, sx1, sy1, dst, dx, dy);
    else if (fs == 5 && dst->format != 5) alpha_32_to_16_o(src, sx0, sy0, sx1, sy1, dst, dx, dy);
    else LogPanic((const char*)0x004f1cec, (int32_t)fs, (int32_t)dst->format);     // 16-bit onto 32-bit
    gx_leave();
}
PORT_FN(0x00450d80, "gxAlphaBlit", gxAlphaBlit_n, fp_blit)

// copy_16_to_16: raw copy (dwords when the clipped width is even), then a 565 -> 555 pass over the same rectangle
// when the destination is 555 (reading the source again); any other format pair panics after the copy.
static void __cdecl copy_16_to_16_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    int32_t w = sx1 - sx0, h = sy1 - sy0;
    if (!shrink_rect_o(dst, dx, dy, sx0, sy0, w, h)) return;
    uint8_t* s = (uint8_t*)calc_address_16_o(src, sx0, sy0);
    uint8_t* d = (uint8_t*)calc_address_16_o(dst, dx, dy);
    const int32_t w_saved = w, h_saved = h, w2 = w * 2;
    const int32_t sskip = (src->pitch - w2) / 2;
    const int32_t dskip = (dst->pitch - w2) / 2;
    if (w & 1) {
        do {
            int32_t n = w;
            do {
                w16(d, r16(s));
                s += 2;
                d += 2;
            } while (--n > 0);
            s += sskip * 2;
            d += dskip * 2;
        } while (--h > 0);
    } else {
        w = w / 2;
        const int32_t sk = ((src->pitch - w * 4) / 4) * 4;
        const int32_t dk = ((dst->pitch - w * 4) / 4) * 4;
        do {
            int32_t n = w;
            do {
                w32(d, r32(s));
                s += 4;
                d += 4;
            } while (--n > 0);
            s += sk;
            d += dk;
        } while (--h > 0);
    }
    const int8_t fs = src->format, fd = dst->format;
    if (fs == fd) return;
    if (fs != 4 || fd != 3) {
        LogPanic((const char*)0x004f1d10, (int32_t)fs, (int32_t)fd);
        return;
    }
    s = (uint8_t*)calc_address_16_o(src, sx0, sy0);
    d = (uint8_t*)calc_address_16_o(dst, dx, dy);
    w = w_saved;
    h = h_saved;
    const int32_t sk = ((src->pitch - w2) / 2) * 2;
    const int32_t dk = ((dst->pitch - w2) / 2) * 2;
    do {
        int32_t n = w;
        do {
            const uint32_t v = r16(s);
            s += 2;
            d += 2;
            w16(d - 2, (v & 0x1f) | ((v & 0xffc0) >> 1));
        } while (--n > 0);
        s += sk;
        d += dk;
    } while (--h > 0);
}
PORT_FN(0x00450e70, "copy_16_to_16", copy_16_to_16_n, fp_blit)

static void __cdecl copy_32_to_32_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    int32_t w = sx1 - sx0, h = sy1 - sy0;
    if (!shrink_rect_o(dst, dx, dy, sx0, sy0, w, h)) return;
    uint8_t* s = (uint8_t*)calc_address_32_o(src, sx0, sy0);
    uint8_t* d = (uint8_t*)calc_address_32_o(dst, dx, dy);
    const int32_t sk = ((src->pitch - w * 4) / 4) * 4;
    const int32_t dk = ((dst->pitch - w * 4) / 4) * 4;
    do {
        int32_t n = w;
        do {
            w32(d, r32(s));
            s += 4;
            d += 4;
        } while (--n > 0);
        s += sk;
        d += dk;
    } while (--h > 0);
}
PORT_FN(0x004510b0, "copy_32_to_32", copy_32_to_32_n, fp_blit)

// copy_16_to_32: through cvt_555_8888 / cvt_565_8888, whose unsigned short result is what's stored (FIX CANDIDATE)
static void __cdecl copy_16_to_32_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    int32_t w = sx1 - sx0, h = sy1 - sy0;
    if (!shrink_rect_o(dst, dx, dy, sx0, sy0, w, h)) return;
    uint8_t* s = (uint8_t*)calc_address_16_o(src, sx0, sy0);
    uint8_t* d = (uint8_t*)calc_address_32_o(dst, dx, dy);
    const int32_t sskip = (src->pitch - w * 2) / 2;
    const int32_t dskip = (dst->pitch - w * 4) / 4;
    const int8_t fs = src->format;
    Cvt32_t cvt;
    if (fs == 4) cvt = cvt_565_8888_o;
    else if (fs == 3) cvt = cvt_555_8888_o;
    else {
        LogPanic((const char*)0x004f1d3c, (int32_t)fs, (int32_t)dst->format);
        return;
    }
    do {
        int32_t n = w;
        do {
            const uint32_t v = r16(s);
            s += 2;
            d += 4;
            --n;
            w32(d - 4, cvt(v));
        } while (n > 0);
        d += dskip * 4;
        s += sskip * 2;
    } while (--h > 0);
}
PORT_FN(0x00451190, "copy_16_to_32", copy_16_to_32_n, fp_blit)

static void __cdecl copy_32_to_16_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    int32_t w = sx1 - sx0, h = sy1 - sy0;
    if (!shrink_rect_o(dst, dx, dy, sx0, sy0, w, h)) return;
    uint8_t* s = (uint8_t*)calc_address_32_o(src, sx0, sy0);
    uint8_t* d = (uint8_t*)calc_address_16_o(dst, dx, dy);
    const int32_t sskip = (src->pitch - w * 4) / 4;
    const int32_t dskip = (dst->pitch - w * 2) / 2;
    const int8_t fd = dst->format;
    if (fd != 3 && fd != 4) {
        LogPanic((const char*)0x004f1d58, (int32_t)src->format, (int32_t)fd);
        return;
    }
    do {
        int32_t n = w;
        do {
            const uint32_t p = r32(s);
            s += 4;
            d += 2;
            --n;
            const uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            if (fd == 4) w16(d - 2, ((r & 0xf8) << 8) | ((g << 3) & 0x7e0) | (b >> 3));
            else w16(d - 2, ((r & 0xf8) << 7) | ((g & 0xf8) << 2) | (b >> 3));
        } while (n > 0);
        s += sskip * 4;
        d += dskip * 2;
    } while (--h > 0);
}
PORT_FN(0x004512f0, "copy_32_to_16", copy_32_to_16_n, fp_blit)

static void fp_canvas_xy_cur(Footprint& f, gxCanvas*, int32_t, int32_t) { fp_cur_clip(f); }
static void fp_canvas_xy_arg(Footprint& f, gxCanvas* c, int32_t, int32_t) { fp_clip(f, c, "the canvas's pixels (clip)"); }

static void __cdecl gxCut_n(gxCanvas* c, int32_t x, int32_t y) {
    gx_enter();
    const int32_t y1 = c->h + y;
    const int32_t x1 = c->w + x;
    gxCopy_o(CUR, x, y, x1, y1, c, 0, 0);
    gx_leave();
}
PORT_FN(0x004514a0, "gxCut", gxCut_n, fp_canvas_xy_arg)

static void __cdecl gxPaste_n(gxCanvas* c, int32_t x, int32_t y) {
    gx_enter();
    gxCopy_o(c, 0, 0, c->w, c->h, CUR, x, y);
    gx_leave();
}
PORT_FN(0x00451500, "gxPaste", gxPaste_n, fp_canvas_xy_cur)

static void __cdecl gxPasteAlpha_n(gxCanvas* c, int32_t x, int32_t y) {
    gx_enter();
    gxAlphaBlit_o(c, 0, 0, c->w, c->h, CUR, x, y);
    gx_leave();
}
PORT_FN(0x00451550, "gxPasteAlpha", gxPasteAlpha_n, fp_canvas_xy_cur)

// gxPasteDouble: c at twice its size, 16-bit only. The bottom is clipped against 2h, not y + 2h, and an odd clipped
// width or height writes one column / row past the clip rectangle (FIX CANDIDATE).
static void __cdecl gxPasteDouble_n(gxCanvas* c, int32_t x, int32_t y) {
    int32_t x0 = x, y0 = y;
    int32_t x1 = x + c->w * 2;
    int32_t y1 = c->h * 2;
    if (x < CUR->cx0) x0 = CUR->cx0;
    if (!(x1 < CUR->cx1)) x1 = CUR->cx1;
    if (y < CUR->cy0) y0 = CUR->cy0;
    if (!(y1 < CUR->cy1)) y1 = CUR->cy1;
    if (y1 <= y0) return;
    do {
        const int32_t sx = (x0 - x) / 2;
        uint8_t* s = (uint8_t*)calc_address_16_o(c, sx, (y0 - y) / 2);
        uint8_t* d0 = (uint8_t*)calc_address_16_o(CUR, x0, y0);
        uint8_t* d1 = (uint8_t*)calc_address_16_o(CUR, x0, y0 + 1);
        if (x1 > x0) {
            uint32_t n = (uint32_t)(x1 - x0 + 1) >> 1;
            do {
                const uint32_t v = r16(s);
                s += 2;
                w16(d0, v);
                d0 += 2;
                w16(d0, v);
                d0 += 2;
                w16(d1, v);
                d1 += 2;
                w16(d1, v);
                d1 += 2;
            } while (--n != 0);
        }
        y0 += 2;
    } while (y1 > y0);
}
static void fp_gxPasteDouble(Footprint& f, gxCanvas*, int32_t, int32_t) {
    gxCanvas* c = CUR;
    if (c) fp_span(f, c, c->cx0, c->cy0, c->cx1, c->cy1, 2, "the current canvas's pixels (clip, and one past)");
}
PORT_FN(0x004515a0, "gxPasteDouble", gxPasteDouble_n, fp_gxPasteDouble)

// gfxReduceTo555: the whole canvas from 565 to 555 in place (the format byte is left alone)
static void __cdecl gfxReduceTo555_n(gxCanvas& c) {
    const int32_t w = c.w;
    int32_t rows = c.h;
    uint8_t* s = (uint8_t*)calc_address_16_o(&c, 0, 0);
    uint8_t* d = (uint8_t*)calc_address_16_o(&c, 0, 0);
    const int32_t skip = ((c.pitch - w * 2) / 2) * 2;
    do {
        int32_t n = w;
        do {
            const uint32_t v = r16(s);
            s += 2;
            d += 2;
            w16(d - 2, (v & 0x1f) | ((v & 0xffc0) >> 1));
        } while (--n > 0);
        s += skip;
        d += skip;
    } while (--rows > 0);
}
static void fp_gfxReduceTo555(Footprint& f, gxCanvas& c) { fp_whole(f, &c, 2); }
PORT_FN(0x004516c0, "gfxReduceTo555", gfxReduceTo555_n, fp_gfxReduceTo555)

// ---- the pixel converters (16-bit register arithmetic, written in 32 bits and cut to 16) -----------------------------
static __forceinline uint32_t lo8(uint32_t v) { return v & 0xff; }
static __forceinline uint32_t lo16(uint32_t v) { return v & 0xffff; }

// cvt_555_8888 / cvt_565_8888: the red byte is shifted out of the 16-bit result (G << 8 | B)
static uint16_t __cdecl cvt_555_8888_n(uint32_t a) {
    const uint32_t u = a & 0xffff;
    uint32_t ax = lo16(lo8((u >> 10) << 3) << 8);
    ax |= lo8((u >> 5) << 3);
    ax = lo16(ax << 8);
    return (uint16_t)(ax | lo8(u << 3));
}
static void fp_pure_u32(Footprint& f, uint32_t) { f.pure = true; }
PORT_FN(0x00451740, "cvt_555_8888", cvt_555_8888_n, fp_pure_u32)

static uint16_t __cdecl cvt_565_8888_n(uint32_t a) {
    const uint32_t u = a & 0xffff;
    uint32_t ax = lo16(lo8((u >> 11) << 3) << 8);
    ax |= lo8((u >> 5) << 2);
    ax = lo16(ax << 8);
    return (uint16_t)(ax | lo8(u << 3));
}
PORT_FN(0x00451780, "cvt_565_8888", cvt_565_8888_n, fp_pure_u32)

static void fp_zoom(Footprint& f, gxCanvas*, int32_t, int32_t, float, float) { fp_cur_clip(f); }
static void __cdecl gxPasteZoom_n(gxCanvas* c, int32_t x, int32_t y, float sx, float sy) {
    const int8_t fs = c->format;
    if (fs != 5) {
        if (CUR->format != 5) {
            paste_zoom_16_16_o(c, x, y, sx, sy);
            return;
        }
    } else {
        if (CUR->format != 5) {
            paste_zoom_32_16_o(c, x, y, sx, sy);
            return;
        }
        if (CUR->format == 5) {
            paste_zoom_32_32_o(c, x, y, sx, sy);
            return;
        }
    }
    LogPanic((const char*)0x004f1d7c, (int32_t)fs, (int32_t)CUR->format);
}
PORT_FN(0x004517c0, "gxPasteZoom", gxPasteZoom_n, fp_zoom)

// the zoomed pastes: 16.16 fixed-point steps from 1 / scale, the clipped size from the source's size x scale
struct ZoomSetup { int32_t stepx, stepy, ox, oy, w, h; };
static __forceinline void zoom_steps(ZoomSetup& z, float sx, float sy) {
    z.stepx = x87_ftol(1.0 / D(sx) * 65536.0);                 // fld; fdivr 1.0f; fmul 65536.0f; ftol
    z.stepy = x87_ftol(1.0 / D(sy) * 65536.0);
}

static void __cdecl paste_zoom_16_16_n(gxCanvas* src, int32_t dx, int32_t dy, float sx, float sy) {
    ZoomSetup z;
    zoom_steps(z, sx, sy);
    z.ox = 0;
    z.oy = 0;
    if (CUR->cx0 > dx) z.ox = CUR->cx0 - dx;
    if (CUR->cy0 > dy) z.oy = CUR->cy0 - dy;
    z.w = x87_ftol(D(src->w) * D(sx)) - z.ox;
    z.h = x87_ftol(D(src->h) * D(sy)) - z.oy;
    const int32_t x = add32(z.ox, dx);
    if (add32(x, z.w) > CUR->cx1) z.w = sub32(sub32(CUR->cx1, dx), z.ox);
    if (add32(add32(dy, z.oy), z.h) > CUR->cy1) z.h = sub32(sub32(CUR->cy1, dy), z.oy);
    uint32_t ay = (uint32_t)imul(z.oy, z.stepy);
    const Cvt16_t cvt = get_cvt_fn_o(src->format, CUR->format);
    if (z.h <= 0) return;
    const uint32_t ax0 = (uint32_t)imul(z.ox, z.stepx);
    int32_t row = 0;
    do {
        uint32_t ax = ax0;
        const int32_t sr = (int32_t)(ay >> 16);
        uint8_t* d = (uint8_t*)calc_address_16_o(CUR, x, add32(dy, add32(z.oy, row)));
        const uint8_t* s = (const uint8_t*)calc_address_16_o(src, 0, sr);
        if (z.w > 0) {
            int32_t n = z.w;
            do {
                const uint16_t v = r16(s + (ax >> 16) * 2);
                d += 2;
                w16(d - 2, cvt(v));
                ax += (uint32_t)z.stepx;
            } while (--n != 0);
        }
        row++;
        ay += (uint32_t)z.stepy;
    } while (row < z.h);
}
PORT_FN(0x00451870, "paste_zoom_16_16", paste_zoom_16_16_n, fp_zoom)

// get_cvt_fn(from, to): the converter's original address (a hooked rewrite runs through it)
static Cvt16_t __cdecl get_cvt_fn_n(int8_t from, int8_t to) {
    const int32_t t = to;
    if (t == 3) {
        const int32_t s = from;
        if (s == 1) return (Cvt16_t)0x00451be0;               // cvt_4444_555
        if (s == 3) return (Cvt16_t)0x00451af0;               // cvt_identity
        if (s == 4) return (Cvt16_t)0x00451b10;               // cvt_565_555
        return (Cvt16_t)0x00451b00;                           // cvt_black
    }
    if (t == 4) {
        const int32_t s = from;
        if (s == 1) return (Cvt16_t)0x00451b90;               // cvt_4444_565
        if (s == 3) return (Cvt16_t)0x00451b50;               // cvt_555_565
        if (s == 4) return (Cvt16_t)0x00451af0;               // cvt_identity
        return (Cvt16_t)0x00451b00;
    }
    LogPanic((const char*)0x004f1d98, t);
    return (Cvt16_t)0x00451b00;
}
static void fp_get_cvt_fn(Footprint&, int8_t, int8_t) {}
PORT_FN(0x00451a70, "get_cvt_fn", get_cvt_fn_n, fp_get_cvt_fn)

static uint16_t __cdecl cvt_identity_n(uint16_t u) { return u; }
static void fp_pure_u16(Footprint& f, uint16_t) { f.pure = true; }
PORT_FN(0x00451af0, "cvt_identity", cvt_identity_n, fp_pure_u16)
// cvt_black (0x451b00, xor ax,ax; ret) is 4 bytes: too short to hook

static uint16_t __cdecl cvt_565_555_n(uint16_t u) {
    uint32_t ax = lo8(((uint32_t)u >> 11) << 3) & 0xfff8;
    ax = lo16(ax << 5);
    ax |= lo8(((uint32_t)u >> 5) << 2);
    ax &= 0xfff8;
    ax = lo16(ax << 2);
    return (uint16_t)(ax | (u & 0x1f));
}
PORT_FN(0x00451b10, "cvt_565_555", cvt_565_555_n, fp_pure_u16)

static uint16_t __cdecl cvt_555_565_n(uint16_t u) {
    uint32_t ax = lo8(((uint32_t)u >> 10) << 3) & 0xfff8;
    ax = lo16(ax << 8);
    ax |= lo16(lo8(((uint32_t)u >> 5) << 3) << 3);
    ax &= 0xffe0;
    return (uint16_t)(ax | (u & 0x1f));
}
PORT_FN(0x00451b50, "cvt_555_565", cvt_555_565_n, fp_pure_u16)

static uint16_t __cdecl cvt_4444_565_n(uint16_t u) {
    uint32_t ax = lo16(lo8(((uint32_t)u >> 4) << 4) << 3);
    const uint32_t cx = lo16((lo8(((uint32_t)u >> 8) << 4) & 0xfff8) << 8);
    const uint32_t dl = lo8((u & 0xf) * 2);
    ax |= cx;
    ax &= 0xffe0;
    return (uint16_t)(ax | dl);
}
PORT_FN(0x00451b90, "cvt_4444_565", cvt_4444_565_n, fp_pure_u16)

static uint16_t __cdecl cvt_4444_555_n(uint16_t u) {
    uint32_t ax = lo8(((uint32_t)u >> 8) << 4) & 0xfff8;
    ax = lo16(ax << 5);
    const uint32_t dl = lo8((u & 0xf) * 2);
    ax |= lo8(((uint32_t)u >> 4) << 4);
    ax &= 0xfff8;
    ax = lo16(ax << 2);
    return (uint16_t)(ax | dl);
}
PORT_FN(0x00451be0, "cvt_4444_555", cvt_4444_555_n, fp_pure_u16)

// paste_zoom_32_16: an 8888 source onto a 565 / 555 canvas; any other canvas format panics and leaves the converter
// NULL, which the loop then calls (FIX CANDIDATE)
static void __cdecl paste_zoom_32_16_n(gxCanvas* src, int32_t dx, int32_t dy, float sx, float sy) {
    ZoomSetup z;
    zoom_steps(z, sx, sy);
    z.ox = 0;
    z.oy = 0;
    if (dx < CUR->cx0) z.ox = CUR->cx0 - dx;
    if (dy < CUR->cy0) z.oy = CUR->cy0 - dy;
    z.w = x87_ftol(D(src->w) * D(sx)) - z.ox;
    z.h = x87_ftol(D(src->h) * D(sy)) - z.oy;
    if (add32(add32(dx, z.w), z.ox) > CUR->cx1) z.w = sub32(sub32(CUR->cx1, dx), z.ox);
    if (add32(add32(dy, z.h), z.oy) > CUR->cy1) z.h = sub32(sub32(CUR->cy1, dy), z.oy);
    Cvt32_t cvt = 0;
    uint32_t ay = (uint32_t)imul(z.stepy, z.oy);
    const int8_t fc = CUR->format;
    if (fc == 4) cvt = cvt_8888_565_o;
    else if (fc == 3) cvt = cvt_8888_555_o;
    else LogPanic((const char*)0x004f1dbc, (int32_t)src->format, (int32_t)fc);
    int32_t row = 0;
    if (z.h <= 0) return;
    const uint32_t ax0 = (uint32_t)imul(z.stepx, z.ox);
    do {
        uint32_t ax = ax0;
        const int32_t sr = (int32_t)(ay >> 16);
        uint8_t* d = (uint8_t*)calc_address_16_o(CUR, add32(z.ox, dx), add32(z.oy, add32(dy, row)));
        const uint8_t* s = (const uint8_t*)calc_address_32_o(src, 0, sr);
        if (z.w > 0) {
            int32_t n = z.w;
            do {
                d += 2;
                const uint32_t v = r32(s + (ax >> 16) * 4);
                w16(d - 2, cvt(v));
                ax += (uint32_t)z.stepx;
            } while (--n != 0);
        }
        row++;
        ay += (uint32_t)z.stepy;
    } while (row < z.h);
}
PORT_FN(0x00451c30, "paste_zoom_32_16", paste_zoom_32_16_n, fp_zoom)

static uint16_t __cdecl cvt_8888_555_n(uint32_t p) {
    uint32_t ax = ((p >> 16) & 0xff) & 0xfff8;
    ax = lo16(ax << 5);
    ax |= (p >> 8) & 0xff;
    ax &= 0xfff8;
    ax = lo16(ax << 2);
    return (uint16_t)(ax | ((p & 0xff) >> 3));
}
PORT_FN(0x00451e50, "cvt_8888_555", cvt_8888_555_n, fp_pure_u32)

static uint16_t __cdecl cvt_8888_565_n(uint32_t p) {
    uint32_t ax = ((p >> 16) & 0xff) & 0xfff8;
    ax = lo16(ax << 8);
    ax |= lo16(((p >> 8) & 0xff) << 3);
    ax &= 0xffe0;
    return (uint16_t)(ax | ((p & 0xff) >> 3));
}
PORT_FN(0x00451e80, "cvt_8888_565", cvt_8888_565_n, fp_pure_u32)

static void __cdecl paste_zoom_32_32_n(gxCanvas* src, int32_t dx, int32_t dy, float sx, float sy) {
    ZoomSetup z;
    zoom_steps(z, sx, sy);
    z.ox = 0;
    z.oy = 0;
    if (dx < CUR->cx0) z.ox = CUR->cx0 - dx;
    if (dy < CUR->cy0) z.oy = CUR->cy0 - dy;
    z.w = x87_ftol(D(src->w) * D(sx)) - z.ox;
    z.h = x87_ftol(D(src->h) * D(sy)) - z.oy;
    if (add32(add32(dx, z.w), z.ox) > CUR->cx1) z.w = sub32(sub32(CUR->cx1, z.ox), dx);
    if (add32(add32(dy, z.h), z.oy) > CUR->cy1) z.h = sub32(sub32(CUR->cy1, dy), z.oy);
    uint32_t ay = (uint32_t)imul(z.oy, z.stepy);
    int32_t row = 0;
    if (z.h <= row) return;
    const uint32_t ax0 = (uint32_t)imul(z.ox, z.stepx);
    do {
        uint32_t ax = ax0;
        const int32_t sr = (int32_t)(ay >> 16);
        uint8_t* d = (uint8_t*)calc_address_32_o(CUR, add32(dx, z.ox), add32(z.oy, add32(dy, row)));
        const uint8_t* s = (const uint8_t*)calc_address_32_o(src, 0, sr);
        if (z.w > 0) {
            int32_t n = z.w;
            do {
                d += 4;
                const uint32_t i = ax >> 16;
                ax += (uint32_t)z.stepx;
                w32(d - 4, r32(s + i * 4));
            } while (--n != 0);
        }
        row++;
        ay += (uint32_t)z.stepy;
    } while (row < z.h);
}
PORT_FN(0x00451eb0, "paste_zoom_32_32", paste_zoom_32_32_n, fp_zoom)

// =====================================================================================================================
// gfx.obj: alpha blits (a 4444 source's top nibble, or an 8888 source's top byte, over the destination)
// =====================================================================================================================
static void __cdecl alpha_16_to_16_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    int32_t w = sx1 - sx0, h = sy1 - sy0;
    if (!shrink_rect_o(dst, dx, dy, sx0, sy0, w, h)) return;
    uint8_t* s = (uint8_t*)calc_address_16_o(src, sx0, sy0);
    uint8_t* d = (uint8_t*)calc_address_16_o(dst, dx, dy);
    const int32_t sskip = (src->pitch - w * 2) / 2;
    const int32_t dskip = (dst->pitch - w * 2) / 2;
    const int8_t fd = dst->format;
    if (fd != 3 && fd != 4) {
        LogPanic((const char*)0x004f1ddc);
        return;
    }
    do {
        int32_t n = w;
        do {
            const uint32_t sv = r16(s);
            s += 2;
            const uint32_t a = (sv >> 12) & 0xf, na = 15 - a;
            const uint32_t dv = r16(d);
            uint32_t si;
            if (fd == 4) {
                const uint32_t r = (lo8((dv >> 11) << 3) * na + lo8((sv >> 8) << 4) * a) / 15;
                si = lo16((lo8(r) & 0xfff8) << 8);
                const uint32_t g = (lo8((sv >> 4) << 4) * a + lo8((dv >> 5) << 2) * na) / 15;
                si |= lo16(lo8(g) << 3);
                si &= 0xffe0;
                const uint32_t b = (lo8(dv << 3) * na + lo8(sv << 4) * a) / 15;
                si |= lo8(b) >> 3;
            } else {
                const uint32_t r = (lo8((sv >> 8) << 4) * a + lo8((dv >> 10) << 3) * na) / 15;
                si = lo16((lo8(r) & 0xfff8) << 5);
                const uint32_t g = (lo8((dv >> 5) << 3) * na + lo8((sv >> 4) << 4) * a) / 15;
                si |= lo8(g);
                si &= 0xfff8;
                si = lo16(si << 2);
                const uint32_t b = (lo8(sv << 4) * a + lo8(dv << 3) * na) / 15;
                si |= lo8(b) >> 3;
            }
            d += 2;
            w16(d - 2, si);
        } while (--n > 0);
        s += sskip * 2;
        d += dskip * 2;
    } while (--h > 0);
}
PORT_FN(0x00452070, "alpha_16_to_16", alpha_16_to_16_n, fp_blit)

static void __cdecl alpha_32_to_16_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    int32_t w = sx1 - sx0, h = sy1 - sy0;
    if (!shrink_rect_o(dst, dx, dy, sx0, sy0, w, h)) return;
    uint8_t* s = (uint8_t*)calc_address_32_o(src, sx0, sy0);
    uint8_t* d = (uint8_t*)calc_address_16_o(dst, dx, dy);
    const int32_t sskip = (src->pitch - w * 4) / 4;
    const int32_t dskip = (dst->pitch - w * 2) / 2;
    const int8_t fd = dst->format;
    Blend_t blend;
    if (fd == 3) blend = alpha_blend_8888_to_555_o;
    else if (fd == 4) blend = alpha_blend_8888_to_565_o;
    else {
        LogPanic((const char*)0x004f1dfc);
        return;
    }
    do {
        int32_t n = w;
        do {
            const uint32_t sv = r32(s);
            const uint16_t dv = r16(d);
            s += 4;
            d += 2;
            --n;
            w16(d - 2, blend(dv, sv));
        } while (n > 0);
        d += dskip * 2;
        s += sskip * 4;
    } while (--h > 0);
}
PORT_FN(0x00452370, "alpha_32_to_16", alpha_32_to_16_n, fp_blit)

static uint16_t __cdecl alpha_blend_8888_to_555_n(uint16_t dv, uint32_t sv) {
    const uint32_t a = sv >> 24, na = 255 - a;
    const uint32_t r = (((sv >> 16) & 0xff) * a + lo8(((uint32_t)dv >> 10) << 3) * na) / 255;
    uint32_t si = lo16((lo8(r) & 0xfff8) << 5);
    const uint32_t g = (((sv >> 8) & 0xff) * a + lo8(((uint32_t)dv >> 5) << 3) * na) / 255;
    si |= lo8(g);
    si &= 0xfff8;
    si = lo16(si << 2);
    const uint32_t b = ((sv & 0xff) * a + lo8((uint32_t)dv << 3) * na) / 255;
    return (uint16_t)(si | (lo8(b) >> 3));
}
static void fp_blend(Footprint& f, uint16_t, uint32_t) { f.pure = true; }
PORT_FN(0x004524c0, "alpha_blend_8888_to_555", alpha_blend_8888_to_555_n, fp_blend)

static uint16_t __cdecl alpha_blend_8888_to_565_n(uint16_t dv, uint32_t sv) {
    const uint32_t a = sv >> 24, na = 255 - a;
    const uint32_t r = (((sv >> 16) & 0xff) * a + lo8(((uint32_t)dv >> 11) << 3) * na) / 255;
    uint32_t si = lo16((lo8(r) & 0xfff8) << 8);
    const uint32_t g = (((sv >> 8) & 0xff) * a + lo8(((uint32_t)dv >> 5) << 2) * na) / 255;
    si |= lo16(lo8(g) << 3);
    si &= 0xffe0;
    const uint32_t b = ((sv & 0xff) * a + lo8((uint32_t)dv << 3) * na) / 255;
    return (uint16_t)(si | (lo8(b) >> 3));
}
PORT_FN(0x00452570, "alpha_blend_8888_to_565", alpha_blend_8888_to_565_n, fp_blend)

// alpha_32_to_32: each channel blended by the source's alpha; the destination keeps its own alpha byte
static void __cdecl alpha_32_to_32_n(gxCanvas* src, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, gxCanvas* dst, int32_t dx, int32_t dy) {
    int32_t w = sx1 - sx0, h = sy1 - sy0;
    if (!shrink_rect_o(dst, dx, dy, sx0, sy0, w, h)) return;
    uint8_t* s = (uint8_t*)calc_address_32_o(src, sx0, sy0);
    uint8_t* d = (uint8_t*)calc_address_32_o(dst, dx, dy);
    const int32_t sskip = (src->pitch - w * 4) / 4;
    const int32_t dskip = (dst->pitch - w * 4) / 4;
    do {
        int32_t n = w;
        do {
            const uint32_t dv = r32(d);
            const uint32_t sv = r32(s);
            d += 4;
            --n;
            const uint32_t a = sv >> 24, na = 255 - a;
            s += 4;
            const uint32_t r = (((sv >> 16) & 0xff) * a + ((dv >> 16) & 0xff) * na) / 255;
            const uint32_t g = (((sv >> 8) & 0xff) * a + ((dv >> 8) & 0xff) * na) / 255;
            const uint32_t b = ((dv & 0xff) * na + (sv & 0xff) * a) / 255;
            w32(d - 4, (dv & 0xff000000u) | lo8(r) << 16 | lo8(g) << 8 | lo8(b));
        } while (n > 0);
        s += sskip * 4;
        d += dskip * 4;
    } while (--h > 0);
}
PORT_FN(0x00452620, "alpha_32_to_32", alpha_32_to_32_n, fp_blit)

// =====================================================================================================================
// gfx.obj: lines, circles, mirror / flip / rotate
// =====================================================================================================================
// The lines: a horizontal or vertical run of points stepped by +-1 (the end point left out), else a DDA along the
// major axis, the minor coordinate a float advanced by (d_minor / d_major) * step and stored each step (fstp dword).
static __forceinline float line_slope(int32_t num, int32_t den, int32_t step) {
    return (float)(D(num) / D(den) * D(step));             // fild; fidiv; fimul; fstp dword
}

static void __cdecl gxLine_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
    gx_enter();
    const int32_t dx = sub32(x1, x0), dy = sub32(y1, y0);
    int32_t x = x0, y = y0;
    if (dx == 0) {
        if (dy != 0) {
            const int32_t step = y1 > y0 ? 1 : -1;
            const int32_t end = imul(step, y1);
            int32_t k = imul(step, y);
            if (end > k) {
                const int32_t inc = imul(step, step);
                do {
                    gxPoint_o(x, y, c);
                    k = add32(k, inc);
                    y = add32(y, step);
                } while (k < end);
            }
        }
    } else if (dy == 0) {
        const int32_t step = x1 > x0 ? 1 : -1;
        const int32_t end = imul(x1, step);
        int32_t k = imul(step, x);
        if (end > k) {
            const int32_t inc = imul(step, step);
            do {
                gxPoint_o(x, y, c);
                k = add32(k, inc);
                x = add32(x, step);
            } while (end > k);
        }
    } else if ((dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx)) {
        const int32_t step = y1 > y0 ? 1 : -1;
        const float slope = line_slope(dx, dy, step);
        float fx = (float)x;
        if (y1 != y0) {
            do {
                const int32_t yy = y;
                y = add32(y, step);
                gxPoint_o(x87_ftol(fx), yy, c);
                fx = (float)(D(fx) + D(slope));
            } while (y != y1);
        }
    } else {
        const int32_t step = x1 > x0 ? 1 : -1;
        const float slope = line_slope(dy, dx, step);
        float fy = (float)y;
        if (x1 != x0) {
            do {
                const int32_t yy = x87_ftol(fy);
                const int32_t xx = x;
                x = add32(x, step);
                gxPoint_o(xx, yy, c);
                fy = (float)(D(fy) + D(slope));
            } while (x != x1);
        }
    }
    gx_leave();
}
static void fp_line(Footprint& f, int32_t, int32_t, int32_t, int32_t, uint32_t) { fp_cur_clip(f); }
PORT_FN(0x00452790, "gxLine", gxLine_n, fp_line)

typedef void(__cdecl* PasteFn_t)(gxCanvas*, int32_t, int32_t);
// gxBrushLine: the brush pasted at each point (gxPasteAlpha for a 4444 / 8888 brush, else gxPaste)
static void __cdecl gxBrushLine_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1, gxCanvas* brush) {
    const int8_t bf = brush->format;
    const PasteFn_t fn = (bf == 1 || bf == 5) ? (PasteFn_t)0x00451550 : (PasteFn_t)0x00451500;
    gx_enter();
    const int32_t dx = sub32(x1, x0), dy = sub32(y1, y0);
    int32_t x = x0, y = y0;
    if (dx == 0) {
        if (dy != 0) {
            const int32_t step = y < y1 ? 1 : -1;
            int32_t k = imul(y, step);
            const int32_t end = imul(step, y1);
            if (k < end) {
                const int32_t inc = imul(step, step);
                do {
                    fn(brush, x, y);
                    k = add32(k, inc);
                    y = add32(y, step);
                } while (k < end);
            }
        }
    } else if (dy == 0) {
        const int32_t step = x1 > x ? 1 : -1;
        int32_t k = imul(x, step);
        const int32_t end = imul(x1, step);
        if (end > k) {
            const int32_t inc = imul(step, step);
            do {
                fn(brush, x, y);
                k = add32(k, inc);
                x = add32(x, step);
            } while (end > k);
        }
    } else if ((dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx)) {
        const int32_t step = y < y1 ? 1 : -1;
        const float slope = line_slope(dx, dy, step);
        float fx = (float)x;
        if (y != y1) {
            do {
                const int32_t yy = y;
                const int32_t xx = x87_ftol(fx);
                y = add32(y, step);
                fn(brush, xx, yy);
                fx = (float)(D(fx) + D(slope));
            } while (y != y1);
        }
    } else {
        const int32_t step = x1 > x ? 1 : -1;
        const float slope = line_slope(dy, dx, step);
        float fy = (float)y;
        if (x1 != x) {
            do {
                const int32_t yy = x87_ftol(fy);
                const int32_t xx = x;
                x = add32(x, step);
                fn(brush, xx, yy);
                fy = (float)(D(fy) + D(slope));
            } while (x1 != x);
        }
    }
    gx_leave();
}
static void fp_gxBrushLine(Footprint& f, int32_t, int32_t, int32_t, int32_t, gxCanvas*) { fp_cur_clip(f); }
PORT_FN(0x004529f0, "gxBrushLine", gxBrushLine_n, fp_gxBrushLine)

typedef void(__cdecl* XorPt_t)(int32_t, int32_t);
static const XorPt_t gxXORPoint_o = (XorPt_t)0x00450410;
static void __cdecl gxXORLine_n(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    gx_enter();
    const int32_t dx = sub32(x1, x0), dy = sub32(y1, y0);
    int32_t x = x0, y = y0;
    if (dx == 0) {
        if (dy != 0) {
            const int32_t step = y < y1 ? 1 : -1;
            int32_t k = imul(y, step);
            const int32_t end = imul(step, y1);
            if (k < end) {
                const int32_t inc = imul(step, step);
                do {
                    const int32_t yy = y;
                    y = add32(y, step);
                    gxXORPoint_o(x, yy);
                    k = add32(k, inc);
                } while (k < end);
            }
        }
    } else if (dy == 0) {
        const int32_t step = x < x1 ? 1 : -1;
        int32_t k = imul(x, step);
        const int32_t end = imul(step, x1);
        if (k < end) {
            const int32_t inc = imul(step, step);
            do {
                const int32_t xx = x;
                x = add32(x, step);
                gxXORPoint_o(xx, y);
                k = add32(k, inc);
            } while (k < end);
        }
    } else if ((dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx)) {
        const int32_t step = y < y1 ? 1 : -1;
        const float slope = line_slope(dx, dy, step);
        float fx = (float)x;
        if (y != y1) {
            do {
                const int32_t yy = y;
                const int32_t xx = x87_ftol(fx);
                y = add32(y, step);
                gxXORPoint_o(xx, yy);
                fx = (float)(D(fx) + D(slope));
            } while (y != y1);
        }
    } else {
        const int32_t step = x < x1 ? 1 : -1;
        const float slope = line_slope(dy, dx, step);
        float fy = (float)y;
        if (x != x1) {
            do {
                const int32_t yy = x87_ftol(fy);
                const int32_t xx = x;
                x = add32(x, step);
                gxXORPoint_o(xx, yy);
                fy = (float)(D(slope) + D(fy));
            } while (x != x1);
        }
    }
    gx_leave();
}
static void fp_gxXORLine(Footprint& f, int32_t, int32_t, int32_t, int32_t) { fp_cur_clip(f); }
PORT_FN(0x00452c50, "gxXORLine", gxXORLine_n, fp_gxXORLine)

// gxCircle: the midpoint circle, 8 points per step
static void __cdecl gxCircle_n(int32_t x, int32_t y, int32_t r, uint32_t c) {
    int32_t xx = 0;
    gx_enter();
    int32_t yy = r;
    int32_t d = sub32(1, r);
    do {
        const int32_t a = add32(x, xx);
        gxPoint_o(a, add32(y, yy), c);
        const int32_t b = sub32(x, xx);
        gxPoint_o(b, add32(y, yy), c);
        gxPoint_o(a, sub32(y, yy), c);
        gxPoint_o(b, sub32(y, yy), c);
        const int32_t a2 = add32(x, yy);
        gxPoint_o(a2, add32(y, xx), c);
        const int32_t b2 = sub32(x, yy);
        gxPoint_o(b2, add32(y, xx), c);
        gxPoint_o(a2, sub32(y, xx), c);
        gxPoint_o(b2, sub32(y, xx), c);
        xx++;
        int32_t e;
        if (d < 0) e = xx * 2;
        else {
            yy--;
            e = sub32(xx, yy) * 2;
        }
        d = add32(d, e + 1);
    } while (yy >= xx);
    gx_leave();
}
static void fp_gxCircle(Footprint& f, int32_t, int32_t, int32_t, uint32_t) { fp_cur_clip(f); }
PORT_FN(0x00452e80, "gxCircle", gxCircle_n, fp_gxCircle)

// gxMirror: each row reversed in place
static void __cdecl gxMirror_n(gxCanvas* c) {
    gx_enter();
    if (c->format != 5) {
        for (int32_t row = 0; c->h > row; row++) {
            uint8_t* p = (uint8_t*)calc_address_16_o(c, 0, row);
            uint8_t* q = (uint8_t*)calc_address_16_o(c, c->w - 1, row);
            int32_t n = c->w / 2;
            if (n > 0) {
                do {
                    const uint32_t a = r16(p);
                    const uint32_t b = r16(q);
                    w16(p, b);
                    w16(q, a);
                    q -= 2;
                    p += 2;
                } while (--n != 0);
            }
        }
    } else {
        for (int32_t row = 0; c->h > row; row++) {
            uint8_t* p = (uint8_t*)calc_address_32_o(c, 0, row);
            uint8_t* q = (uint8_t*)calc_address_32_o(c, c->w - 1, row);
            int32_t n = c->w / 2;
            if (n > 0) {
                do {
                    const uint32_t a = r32(p);
                    const uint32_t b = r32(q);
                    w32(p, b);
                    w32(q, a);
                    q -= 4;
                    p += 4;
                } while (--n != 0);
            }
        }
    }
    gx_leave();
}
static void fp_gxMirror(Footprint& f, gxCanvas* c) { fp_whole(f, c, bpp_of(c)); }
PORT_FN(0x00452fa0, "gxMirror", gxMirror_n, fp_gxMirror)

// gxFlip: rows swapped top to bottom; each swap runs w + 1 pixels, one past the row (FIX CANDIDATE)
static void __cdecl gxFlip_n(gxCanvas* c) {
    gx_enter();
    if (c->format != 5) {
        for (int32_t row = 0; c->h / 2 > row; row++) {
            uint8_t* p = (uint8_t*)calc_address_16_o(c, 0, row);
            uint8_t* q = (uint8_t*)calc_address_16_o(c, 0, c->h - row - 1);
            int32_t n = c->w;
            if (n >= 0) {
                do {
                    const uint32_t a = r16(p);
                    const uint32_t b = r16(q);
                    w16(p, b);
                    w16(q, a);
                    q += 2;
                    p += 2;
                } while (--n >= 0);
            }
        }
    } else {
        for (int32_t row = 0; c->h / 2 > row; row++) {
            uint8_t* p = (uint8_t*)calc_address_32_o(c, 0, row);
            uint8_t* q = (uint8_t*)calc_address_32_o(c, 0, c->h - row - 1);
            int32_t n = c->w;
            if (n >= 0) {
                do {
                    const uint32_t a = r32(p);
                    const uint32_t b = r32(q);
                    w32(p, b);
                    w32(q, a);
                    q += 4;
                    p += 4;
                } while (--n >= 0);
            }
        }
    }
    gx_leave();
}
static void fp_gxFlip(Footprint& f, gxCanvas* c) {
    if (c) fp_span(f, c, 0, 0, c->w, c->h - 1, bpp_of(c), "canvas pixels (and one past each row)");
}
PORT_FN(0x004530e0, "gxFlip", gxFlip_n, fp_gxFlip)

typedef void(__cdecl* RotFn_t)(gxCanvas*);
static const RotFn_t rotate_90_16_o = (RotFn_t)0x00453260, rotate_90_32_o = (RotFn_t)0x004533b0;
// gxRotate90 enters gx_sync; the rotate_90_* it jumps to leaves it
static void __cdecl gxRotate90_n(gxCanvas* c) {
    gx_enter();
    if (c->format != 5) rotate_90_16_o(c);
    else rotate_90_32_o(c);
}
static void fp_rotate(Footprint& f, gxCanvas*) { f.replay_only = "allocates and frees a scratch canvas"; }
PORT_FN(0x00453230, "gxRotate90", gxRotate90_n, fp_rotate)

typedef uint16_t(__cdecl* GetPt16_t)(gxCanvas*, int32_t, int32_t);
typedef uint32_t(__cdecl* GetPt32_t)(gxCanvas*, int32_t, int32_t);
typedef void(__cdecl* PutPt16_t)(int32_t, int32_t, uint16_t);
typedef void(__cdecl* PutPt32_t)(int32_t, int32_t, uint32_t);
static const GetPt16_t get_point_16_o = (GetPt16_t)0x00453370;
static const GetPt32_t get_point_32_o = (GetPt32_t)0x004534c0;
static const PutPt16_t point_16_16_o = (PutPt16_t)0x00453330;
static const PutPt32_t point_32_32_o = (PutPt32_t)0x00453480;

// rotate_90_*: into a scratch canvas (h x w), point by point, then the canvas takes its shape (not its clip) and the
// scratch is pasted back; the current canvas is put back by a plain store
static void __cdecl rotate_90_16_n(gxCanvas* c) {
    gxCanvas* const saved = CUR;
    gxCanvas tmp;
    gxAllocCanvas4_o(&tmp, c->h, c->w, c->format);
    gxSetCanvas_o(&tmp);
    for (int32_t y = 0; c->h > y; y++) {
        const int32_t tx = c->h - y - 1;
        for (int32_t x = 0; c->w > x; x++) {
            const uint16_t v = get_point_16_o(c, x, y);
            point_16_16_o(tx, x, v);
        }
    }
    c->w = tmp.w;
    c->h = tmp.h;
    c->pitch = tmp.pitch;
    gxSetCanvas_o(c);
    gxPaste_o(&tmp, 0, 0);
    CUR = saved;
    gxFreeCanvas_o(&tmp);
    gx_leave();
}
PORT_FN(0x00453260, "rotate_90_16", rotate_90_16_n, fp_rotate)

static void __cdecl point_16_16_n(int32_t x, int32_t y, uint16_t v) {
    if (!in_clip_o(CUR, x, y)) return;
    w16(calc_address_16_o(CUR, x, y), v);
}
static void fp_point_16_16(Footprint& f, int32_t, int32_t, uint16_t) { fp_cur_clip(f); }
PORT_FN(0x00453330, "point_16_16", point_16_16_n, fp_point_16_16)

static uint16_t __cdecl get_point_16_n(gxCanvas* c, int32_t x, int32_t y) {
    if (!in_clip_o(c, x, y)) return 0;
    return r16(calc_address_16_o(c, x, y));
}
static void fp_get_point(Footprint&, gxCanvas*, int32_t, int32_t) {}
PORT_FN(0x00453370, "get_point_16", get_point_16_n, fp_get_point)

static void __cdecl rotate_90_32_n(gxCanvas* c) {
    gxCanvas* const saved = CUR;
    gxCanvas tmp;
    gxAllocCanvas4_o(&tmp, c->h, c->w, c->format);
    gxSetCanvas_o(&tmp);
    for (int32_t y = 0; c->h > y; y++) {
        const int32_t tx = c->h - y - 1;
        for (int32_t x = 0; c->w > x; x++) {
            const uint32_t v = get_point_32_o(c, x, y);
            point_32_32_o(tx, x, v);
        }
    }
    c->w = tmp.w;
    c->h = tmp.h;
    c->pitch = tmp.pitch;
    gxSetCanvas_o(c);
    gxPaste_o(&tmp, 0, 0);
    CUR = saved;
    gxFreeCanvas_o(&tmp);
    gx_leave();
}
PORT_FN(0x004533b0, "rotate_90_32", rotate_90_32_n, fp_rotate)

static void __cdecl point_32_32_n(int32_t x, int32_t y, uint32_t v) {
    if (!in_clip_o(CUR, x, y)) return;
    w32(calc_address_32_o(CUR, x, y), v);
}
PORT_FN(0x00453480, "point_32_32", point_32_32_n, fp_point)

static uint32_t __cdecl get_point_32_n(gxCanvas* c, int32_t x, int32_t y) {
    if (!in_clip_o(c, x, y)) return 0;
    return r32(calc_address_32_o(c, x, y));
}
PORT_FN(0x004534c0, "get_point_32", get_point_32_n, fp_get_point)

// =====================================================================================================================
// gfx.obj: stamps (gxStamp, 'STMP' version 3), brushes, palettes, the 8x8 text
// =====================================================================================================================
// gxStamp: +0 w, +4 h, +8 / +0xc hot spot, +0x10 frame count, +0x14 256 x 555 palette, +0x214 256 x 565 palette,
// +0x414 per-frame user data, +0x814 row-pointer table (h x frames), +0x818 the RLE data. A row's RLE: a byte with
// the high bit clear skips that many transparent pixels, one with it set is followed by (b & 0x7f) palette indices.
struct StampHdr {
    int32_t w, h, hotx, hoty, count;
    uint16_t pal555[256], pal565[256];
    uint32_t user[256];
    uint8_t** rows;                                             // +0x814
    uint8_t* data;                                              // +0x818
};
static_assert(offsetof(StampHdr, rows) == 0x814 && offsetof(StampHdr, data) == 0x818, "gxStamp");

// gxGetStamp: a 'STMP' resource (version 3); a freshly loaded one has its row offsets turned into pointers
static gxStamp* __cdecl gxGetStamp_n(const char* name) {
    struct { uint8_t fresh; uint32_t version; int32_t size; } L;
    L.fresh = 0;
    L.size = 0;
    StampHdr* s = (StampHdr*)ResourceGet(name, 0x53544d50, L.version, &L.size, 0, &L.fresh);
    if (!s) {
        LogReport((const char*)0x004f1e4c, name);
        return 0;
    }
    if (L.version != 3) LogPanic((const char*)0x004f1e1c, name, L.version, 3);
    if (L.fresh) {
        s->rows = (uint8_t**)((uint8_t*)s + 0x81c);
        s->data = (uint8_t*)s->rows + imul(s->h, s->count) * 4;
        for (int32_t i = 0; imul(s->h, s->count) > i; i++) s->rows[i] += (uintptr_t)s->data;
    }
    return (gxStamp*)s;
}
static void fp_gxGetStamp(Footprint& f, const char*) { f.replay_only = "loads a resource"; }
PORT_FN(0x00453500, "gxGetStamp", gxGetStamp_n, fp_gxGetStamp)

static void __cdecl gxForgetStamp_n(gxStamp* s) {
    if (s) ResourceForget(s);
}
static void fp_gxForgetStamp(Footprint& f, gxStamp*) { f.replay_only = "releases a resource"; }
PORT_FN(0x004535d0, "gxForgetStamp", gxForgetStamp_n, fp_gxForgetStamp)

// gxDrawStamp: one frame at (x, y) less the hot spot, clipped to the current canvas; the palette is the caller's
// (its 565 half unless the canvas is 555) or the stamp's own. 16-bit canvases only.
static void __cdecl gxDrawStamp_n(const gxStamp* stamp, int32_t x, int32_t y, int32_t frame, const void* pal) {
    const StampHdr* s = (const StampHdr*)stamp;
    if (!s) return;
    if (!(s->count > frame)) return;
    const int32_t x0 = x - s->hotx;
    const int32_t y0 = y - s->hoty;
    int32_t row = CUR->cy0 - y0;
    if (!(row > 0)) row = 0;
    int32_t row_end = CUR->cy1 - y0;
    if (!(row_end < s->h)) row_end = s->h;
    int32_t col_start = CUR->cx0 - x0;
    if (!(col_start > 0)) col_start = 0;
    int32_t col_end = CUR->cx1 - x0;
    if (!(col_end < s->w)) col_end = s->w;
    const uint8_t* p16;
    if (pal) p16 = CUR->format == 3 ? (const uint8_t*)pal : (const uint8_t*)pal + 0x200;
    else p16 = CUR->format == 3 ? (const uint8_t*)s + 0x14 : (const uint8_t*)s + 0x214;
    if (!(row_end > row)) return;
    do {
        const uint8_t* rle = s->rows[imul(s->h, frame) + row];
        uint8_t* d = (uint8_t*)calc_address_16_o(CUR, x0, y0 + row);
        int32_t col = 0;
        if (col_end > 0) {
            do {
                int32_t run = *rle++;
                uint8_t opaque = 0;
                if (run & 0x80) {
                    opaque = 1;
                    run &= 0xffffff7f;
                }
                int32_t n = col_end - col;
                if (!(n < run)) n = run;
                if (opaque) {
                    if (col < col_start) {
                        int32_t k = col_start - col;
                        if (!(k < n)) k = n;
                        n -= k;
                        rle += k;
                        d += k * 2;
                        col += k;
                    }
                    uint8_t m = (uint8_t)n;
                    if (m) {
                        col += m;
                        do {
                            const uint32_t idx = *rle++;
                            d += 2;
                            w16(d - 2, r16(p16 + idx * 2));
                        } while (--m != 0);
                    }
                } else {
                    col += n;
                    d += n * 2;
                }
            } while (col_end > col);
        }
        row++;
    } while (row_end > row);
}
static void fp_gxDrawStamp(Footprint& f, const gxStamp*, int32_t, int32_t, int32_t, const void*) { fp_cur_clip(f); }
PORT_FN(0x004535f0, "gxDrawStamp", gxDrawStamp_n, fp_gxDrawStamp)

// gxMakeBrush: the canvas cleared to 0 (as the current canvas, then the old one put back by a plain store), then a
// stamp frame drawn into it as 8888 with the colour's 565 half as RGB and each pixel's palette index as alpha
static void __cdecl gxMakeBrush_n(gxCanvas* c, const gxStamp* stamp, uint32_t color, int32_t frame) {
    const StampHdr* s = (const StampHdr*)stamp;
    gxCanvas* const saved = CUR;
    CUR = c;
    const uint32_t rgb = (((color >> 24) & 0xf8) << 16) | (((color >> 19) & 0xfc) << 8) | ((color >> 13) & 0xf8);
    clear_32_32_o(0);
    CUR = saved;
    if (!s) return;
    if (!(s->count > frame)) return;
    const int32_t x0 = -s->hotx;
    const int32_t y0 = -s->hoty;
    int32_t row_end = c->cy1 - y0;
    if (!(row_end < s->h)) row_end = s->h;
    int32_t col_start = c->cx0 - x0;
    if (!(col_start > 0)) col_start = 0;
    int32_t col_end = c->cx1 - x0;
    if (!(col_end < s->w)) col_end = s->w;
    int32_t row = c->cy0 - y0;
    if (!(row > 0)) row = 0;
    if (row >= row_end) return;
    do {
        const uint8_t* rle = s->rows[imul(s->h, frame) + row];
        uint8_t* d = (uint8_t*)calc_address_32_o(c, x0, y0 + row);
        int32_t col = 0;
        if (col_end > 0) {
            do {
                int32_t run = *rle++;
                uint8_t opaque = 0;
                if (run & 0x80) {
                    run &= 0xffffff7f;
                    opaque = 1;
                }
                int32_t n = col_end - col;
                if (!(n < run)) n = run;
                if (opaque) {
                    if (col < col_start) {
                        int32_t k = col_start - col;
                        if (!(k < n)) k = n;
                        n -= k;
                        rle += k;
                        d += k * 4;
                        col += k;
                    }
                    uint8_t m = (uint8_t)n;
                    if (m) {
                        col += m;
                        do {
                            const uint32_t idx = *rle++;
                            d += 4;
                            w32(d - 4, idx << 24 | rgb);
                        } while (--m != 0);
                    }
                } else {
                    col += n;
                    d += n * 4;
                }
            } while (col < col_end);
        }
        row++;
    } while (row < row_end);
}
// the 32-bit clear of c (clear_32_32 as the current canvas): rows step by w x 4 plus the pitch's remainder rounded
// toward zero, which drifts past the pitch when c isn't really 32-bit
static void fp_clear32_span(Footprint& f, const gxCanvas* c) {
    int32_t x0 = 0, y0 = 0, x1 = c->w, y1 = c->h;
    if (!(c->cx1 > x0) || !(y0 < c->cy1)) return;
    if (c->cx0 > x0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    if (!(x1 > x0) || !(y1 > y0) || !c->pixels) return;
    const int64_t cols = x1 - x0, rows = y1 - y0;
    const int64_t adv = cols * 4 + ((c->pitch - (int32_t)cols * 4) / 4) * 4;
    const int64_t a = (int64_t)y0 * c->pitch + (int64_t)x0 * 4, last = a + (rows - 1) * adv;
    const int64_t lo = a < last ? a : last, hi = (a > last ? a : last) + cols * 4;
    if (hi - lo < 0x4000000) f.add(c->pixels + lo, (uint32_t)(hi - lo), "the brush's pixels (the clear)");
}
static void fp_gxMakeBrush(Footprint& f, gxCanvas* c, const gxStamp*, uint32_t, int32_t) {
    FP_ADD(f, S_CANVAS, 4, "gfx.obj current canvas (set and put back)");
    if (!c) return;
    fp_span(f, c, c->cx0, c->cy0, c->cx1 - 1, c->cy1 - 1, 4, "the brush's pixels (clip, 32-bit)");
    fp_clear32_span(f, c);
}
PORT_FN(0x004537a0, "gxMakeBrush", gxMakeBrush_n, fp_gxMakeBrush)

static uint8_t __cdecl gxStampHitTest_n(const gxStamp* stamp, int32_t x, int32_t y) {
    const StampHdr* s = (const StampHdr*)stamp;
    if (!s) return 0;
    x -= s->hotx;
    y -= s->hoty;
    if (x < 0 || y < 0 || !(s->w > x) || !(s->h > y)) return 0;
    return 1;
}
static void fp_stamp_xy(Footprint&, const gxStamp*, int32_t, int32_t) {}
PORT_FN(0x00453950, "gxStampHitTest", gxStampHitTest_n, fp_stamp_xy)

static int32_t __cdecl gxStampWidth_n(const gxStamp* s) { return s ? ((const StampHdr*)s)->w : 0; }
static void fp_stamp(Footprint&, const gxStamp*) {}
PORT_FN(0x00453980, "gxStampWidth", gxStampWidth_n, fp_stamp)
static int32_t __cdecl gxStampHeight_n(const gxStamp* s) { return s ? ((const StampHdr*)s)->h : 0; }
PORT_FN(0x00453990, "gxStampHeight", gxStampHeight_n, fp_stamp)
static int32_t __cdecl gxStampCount_n(const gxStamp* s) { return s ? ((const StampHdr*)s)->count : 0; }
PORT_FN(0x004539a0, "gxStampCount", gxStampCount_n, fp_stamp)

static void __cdecl gxStampHotSpot_n(const gxStamp* stamp, int32_t* px, int32_t* py) {
    const StampHdr* s = (const StampHdr*)stamp;
    if (!s) {
        *py = 0;
        *px = 0;
        return;
    }
    *px = s->hotx;
    *py = s->hoty;
}
static void fp_gxStampHotSpot(Footprint& f, const gxStamp*, int32_t* px, int32_t* py) { f.add(px, 4, "x"); f.add(py, 4, "y"); }
PORT_FN(0x004539b0, "gxStampHotSpot", gxStampHotSpot_n, fp_gxStampHotSpot)

static uint32_t __cdecl gxStampGetUserData_n(const gxStamp* stamp, int32_t i) {
    const StampHdr* s = (const StampHdr*)stamp;
    if (!s || !(s->count > i)) return 0;
    return *(const uint32_t*)((const uint8_t*)s + 0x414 + i * 4);
}
static void fp_stamp_i(Footprint&, const gxStamp*, int32_t) {}
PORT_FN(0x004539e0, "gxStampGetUserData", gxStampGetUserData_n, fp_stamp_i)

typedef void(__cdecl* OpDelete_t)(void*);
static const OpDelete_t OperatorDelete = (OpDelete_t)0x00414390;
static void* __cdecl gxPaletteCreate_n() { return MemAlloc(0x400); }
static void fp_gxPaletteCreate(Footprint& f) { f.replay_only = "allocates"; }
PORT_FN(0x00453a00, "gxPaletteCreate", gxPaletteCreate_n, fp_gxPaletteCreate)
static void __cdecl gxPaletteDestroy_n(void* p) { OperatorDelete(p); }
static void fp_gxPaletteDestroy(Footprint& f, void*) { f.replay_only = "frees"; }
PORT_FN(0x00453a10, "gxPaletteDestroy", gxPaletteDestroy_n, fp_gxPaletteDestroy)

// a palette: 256 x 555 then 256 x 565; a colour's low half is the 555, its high half the 565
static void __cdecl gxPaletteSetColor_n(void* pal, int32_t i, uint32_t c) {
    uint8_t* p = (uint8_t*)pal + i * 2;
    *(uint16_t*)p = (uint16_t)c;
    *(uint16_t*)(p + 0x200) = (uint16_t)(c >> 16);
}
static void fp_gxPaletteSetColor(Footprint& f, void* pal, int32_t i, uint32_t) {
    f.add((uint8_t*)pal + i * 2, 2, "palette 555");
    f.add((uint8_t*)pal + i * 2 + 0x200, 2, "palette 565");
}
PORT_FN(0x00453a20, "gxPaletteSetColor", gxPaletteSetColor_n, fp_gxPaletteSetColor)

static uint32_t __cdecl gxPaletteGetColor_n(const void* pal, int32_t i) {
    const uint8_t* p = (const uint8_t*)pal + i * 2;
    const uint32_t lo = *(const uint16_t*)p;
    return (uint32_t)*(const uint16_t*)(p + 0x200) << 16 | lo;
}
static void fp_gxPaletteGetColor(Footprint&, const void*, int32_t) {}
PORT_FN(0x00453a40, "gxPaletteGetColor", gxPaletteGetColor_n, fp_gxPaletteGetColor)

typedef void(__cdecl* PalSet_t)(void*, int32_t, uint32_t);
static const PalSet_t gxPaletteSetColor_o = (PalSet_t)0x00453a20;
// gxPaletteMakeGradient: 256 steps from c0 to c1 (their 555 halves, as 8-bit channels), t = i * (1/255.0f)
static void __cdecl gxPaletteMakeGradient_n(void* pal, uint32_t c0, uint32_t c1) {
    const float r0 = (float)(int64_t)((c0 >> 7) & 0xf8);          // fild qword; fstp dword
    const float g0 = (float)(int64_t)((c0 >> 2) & 0xf8);
    const float b0 = (float)(int64_t)lo8(c0 << 3);
    const float dr = (float)(D((int64_t)((c1 >> 7) & 0xf8)) - D(r0));
    const float dg = (float)(D((int64_t)((c1 >> 2) & 0xf8)) - D(g0));
    const float db = (float)(D((int64_t)lo8(c1 << 3)) - D(b0));
    for (int32_t i = 0; i < 0x100; i++) {
        const double t = D(i) * D(FB(0x3b808081));
        const uint32_t r = lo8((uint32_t)x87_ftol(D(dr) * t + D(r0) + 0.5)) >> 3;
        const uint32_t g = lo8((uint32_t)x87_ftol(D(dg) * t + D(g0) + 0.5));
        const uint32_t b = lo8((uint32_t)x87_ftol(t * D(db) + D(b0) + 0.5)) >> 3;
        const uint32_t c = ((((g >> 2) << 16) | r << 22 | r << 5 | (g >> 3)) << 5) | b << 16 | b;
        gxPaletteSetColor_o(pal, i, c);
    }
}
static void fp_gxPaletteMakeGradient(Footprint& f, void* pal, uint32_t, uint32_t) { f.add(pal, 0x400, "palette"); }
PORT_FN(0x00453a60, "gxPaletteMakeGradient", gxPaletteMakeGradient_n, fp_gxPaletteMakeGradient)

typedef void(__cdecl* DrawChar_t)(int32_t, int32_t, int32_t, uint32_t);
static const DrawChar_t draw_char_o = (DrawChar_t)0x00453cb0;
typedef void(__cdecl* Text_t)(int32_t, int32_t, const char*, uint32_t);
static const Text_t gxText_o = (Text_t)0x00453bd0, gxTextCentered_o = (Text_t)0x00453c40;
typedef int32_t(__cdecl* TextWidth_t)(const char*);
static const TextWidth_t gxTextWidth_o = (TextWidth_t)0x00453c80;
typedef int32_t(__cdecl* IntVoid_t)();
static const IntVoid_t gxTextHeight_o = (IntVoid_t)0x00453ca0;

// gxText: the 8x8 font, 8 pixels a character; bytes outside ' '..0x7f (signed) draw as '#'
static void __cdecl gxText_n(int32_t x, int32_t y, const char* s, uint32_t c) {
    gx_enter();
    int8_t ch = (int8_t)*s++;
    while (ch) {
        draw_char_o(x, y, (ch < 0x20 || ch > 0x7f) ? 0x23 : ch, c);
        x += 8;
        ch = (int8_t)*s++;
    }
    gx_leave();
}
static void fp_text(Footprint& f, int32_t, int32_t, const char*, uint32_t) { fp_cur_clip(f); }
PORT_FN(0x00453bd0, "gxText", gxText_n, fp_text)

static void __cdecl gxTextCentered_n(int32_t x, int32_t y, const char* s, uint32_t c) {
    gxText_o(x - (int32_t)strlen(s) * 4, y - 4, s, c);
}
PORT_FN(0x00453c40, "gxTextCentered", gxTextCentered_n, fp_text)

static int32_t __cdecl gxTextWidth_n(const char* s) { return (int32_t)strlen(s) * 8; }
static void fp_gxTextWidth(Footprint&, const char*) {}
PORT_FN(0x00453c80, "gxTextWidth", gxTextWidth_n, fp_gxTextWidth)

static int32_t __cdecl gxTextHeight_n() { return 8; }
static void fp_gxTextHeight(Footprint&) {}
PORT_FN(0x00453ca0, "gxTextHeight", gxTextHeight_n, fp_gxTextHeight)

// draw_char: the glyph table at 0x4ef9f8 (128 glyphs of 8 row strings, 'x' = set), indexed by the signed char
static void __cdecl draw_char_n(int32_t x, int32_t y, int32_t ch, uint32_t c) {
    const char* const* glyph = GP(const char* const, 0x004ef9f8 + (int32_t)(int8_t)ch * 4);
    for (int32_t row = 0; row < 8; row++) {
        const char* r = glyph[row];
        for (int32_t col = 0; col < 8; col++, r++)
            if (*r == 'x') gxPoint_o(col + x, y + row, c);
    }
}
static void fp_draw_char(Footprint& f, int32_t, int32_t, int32_t, uint32_t) { fp_cur_clip(f); }
PORT_FN(0x00453cb0, "draw_char", draw_char_n, fp_draw_char)

// =====================================================================================================================
// font.obj
// =====================================================================================================================
// gxFont (0x608): +0 the stamp, +4 short frame[256] (-1: none), +0x204 short advance[256], +0x404 short x offset[256],
// +0x604 the widest character (never set: 0)
struct FontHdr {
    gxStamp* stamp;
    int16_t frame[256], adv[256], xoff[256];
    int32_t maxw;
};
static_assert(sizeof(FontHdr) == 0x608, "gxFont");
typedef int32_t(__cdecl* StampCount_t)(const gxStamp*);
typedef uint32_t(__cdecl* StampUser_t)(const gxStamp*, int32_t);
typedef void(__cdecl* HotSpot_t)(const gxStamp*, int32_t*, int32_t*);
static const StampCount_t gxStampCount_o = (StampCount_t)0x004539a0;
static const StampUser_t gxStampGetUserData_o = (StampUser_t)0x004539e0;
static const HotSpot_t gxStampHotSpot_o = (HotSpot_t)0x004539b0;
typedef int32_t(__cdecl* FontInt_t)(const FontHdr*);
static const FontInt_t gxFontAscent_o = (FontInt_t)0x004540f0, gxFontDescent_o = (FontInt_t)0x00454130;
typedef int32_t(__cdecl* FontWidth_t)(const FontHdr*, const char*);
static const FontWidth_t gxFontStringWidth_o = (FontWidth_t)0x00454040;

// gxFontGet: a stamp whose frames' user data are (character, x offset, a, b); advance = a + b + x offset
static FontHdr* __cdecl gxFontGet_n(const char* name) {
    FontHdr* f = (FontHdr*)MemAlloc(0x608);
    f->stamp = gxGetStamp_o(name);
    if (!f->stamp) {
        LogReport((const char*)0x004f1e70, name);
        OperatorDelete(f);
        return 0;
    }
    f->maxw = 0;
    for (int i = 0; i < 0x100; i++) {
        f->frame[i] = -1;
        f->adv[i] = 0;
        f->xoff[i] = 0;
    }
    for (int32_t i = 0; gxStampCount_o(f->stamp) > i; i++) {
        const uint32_t ud = gxStampGetUserData_o(f->stamp, i);
        const uint32_t ab = (uint32_t)(int32_t)(int8_t)(ud >> 16) + (uint32_t)(int32_t)(int8_t)(ud >> 24);
        const uint32_t ch = ud & 0xff;
        f->frame[ch] = (int16_t)i;
        const uint32_t xo = (uint32_t)(int32_t)(int8_t)(ud >> 8);
        f->adv[ch] = (int16_t)(ab + xo);
        f->xoff[ch] = (int16_t)xo;
    }
    return f;
}
static void fp_gxFontGet(Footprint& f, const char*) { f.replay_only = "allocates, loads a stamp"; }
PORT_FN(0x00453f30, "gxFontGet", gxFontGet_n, fp_gxFontGet)

static void __cdecl gxFontForget_n(FontHdr* f) {
    if (!f) return;
    gxForgetStamp_o(f->stamp);
    OperatorDelete(f);
}
static void fp_gxFontForget(Footprint& f, FontHdr*) { f.replay_only = "frees"; }
PORT_FN(0x00454020, "gxFontForget", gxFontForget_n, fp_gxFontForget)

static int32_t __cdecl gxFontStringWidth_n(const FontHdr* f, const char* s) {
    if (!f) return gxTextWidth_o(s);
    int32_t w = 0;
    if (*s == 0) return 0;
    do {
        w += f->adv[(uint8_t)*s++];
    } while (*s != 0);
    return w;
}
static void fp_font_str(Footprint&, const FontHdr*, const char*) {}
PORT_FN(0x00454040, "gxFontStringWidth", gxFontStringWidth_n, fp_font_str)

static int32_t __cdecl gxFontStringWidthN_n(const FontHdr* f, const char* s, int32_t n) {
    if (!f) return gxTextWidth_o(s);
    int32_t w = 0;
    if (*s == 0) return 0;
    do {
        if (n <= 0) return w;
        n--;
        w += f->adv[(uint8_t)*s++];
    } while (*s != 0);
    return w;
}
static void fp_font_str_n(Footprint&, const FontHdr*, const char*, int32_t) {}
PORT_FN(0x00454080, "gxFontStringWidthN", gxFontStringWidthN_n, fp_font_str_n)

static int32_t __cdecl gxFontMaxCharWidth_n(const FontHdr* f) {
    if (f) return f->maxw;
    return gxTextWidth_o((const char*)0x004f1e80);
}
static void fp_font(Footprint&, const FontHdr*) {}
PORT_FN(0x004540d0, "gxFontMaxCharWidth", gxFontMaxCharWidth_n, fp_font)

// the ascent is the stamp's hot spot y, the descent its height less that
static int32_t __cdecl gxFontAscent_n(const FontHdr* f) {
    if (!f) return gxTextHeight_o();
    struct { int32_t y, x; } L;
    L.y = 0;
    gxStampHotSpot_o(f->stamp, &L.x, &L.y);
    return L.y;
}
PORT_FN(0x004540f0, "gxFontAscent", gxFontAscent_n, fp_font)

static int32_t __cdecl gxFontDescent_n(const FontHdr* f) {
    if (!f) return 0;
    struct { int32_t y, x; } L;
    L.y = 0;
    gxStampHotSpot_o(f->stamp, &L.x, &L.y);
    return gxStampHeight_o(f->stamp) - L.y;
}
PORT_FN(0x00454130, "gxFontDescent", gxFontDescent_n, fp_font)

static int32_t __cdecl gxFontHeight_n(const FontHdr* f) {
    const int32_t d = gxFontDescent_o(f);
    return gxFontAscent_o(f) + d;
}
PORT_FN(0x00454180, "gxFontHeight", gxFontHeight_n, fp_font)

// gxFontPrintf is variadic: registered with a fixed signature of 16 dwords after the format (as krn_util's
// HTMLWriteLn); on x86 __cdecl the caller's arguments lie in order on the stack, so the va_list is &a0.
typedef int(__cdecl* VSprintf_t)(char*, const char*, va_list);
static const VSprintf_t game_vsprintf = (VSprintf_t)0x004cf7c0;
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t game_sprintf = (Sprintf_t)0x004cf0a0;
#define FONT_VA uint32_t a0, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, \
                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t
#define FONT_VA_FP uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, \
                   uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t
// flags: 2 right-aligned, 4 centred; 8 y is the top, 0x10 centred vertically, 0x40 y is the bottom. No font: the
// 8x8 text twice, a shadow at +1 +1 in 0x522a88's colour and the text in 0x522a98's.
static void __cdecl gxFontPrintf_n(const FontHdr* f, const void* pal, uint32_t flags, int32_t x, int32_t y, const char* fmt, FONT_VA) {
    char buf[0x1000];
    game_vsprintf(buf, fmt, (va_list)&a0);
    const int32_t w = gxFontStringWidth_o(f, buf);
    int32_t px;
    if (flags & 2) px = x - w;
    else if (flags & 4) px = x - w / 2;
    else px = x;
    int32_t py;
    if (flags & 8) py = y + gxFontAscent_o(f);
    else if (flags & 0x10) {
        const int32_t a = gxFontAscent_o(f);
        const int32_t d = gxFontDescent_o(f);
        py = y + (a - d) / 2;
    } else if (flags & 0x40) py = y - gxFontDescent_o(f);
    else py = y;
    if (!f) {
        gxText_o(px + 1, py + 1, buf, GU32(0x00522a88));
        gxText_o(px, py, buf, GU32(0x00522a98));
        return;
    }
    if (buf[0] == 0) return;
    const char* p = buf;
    do {
        const uint32_t ch = (uint8_t)*p;
        const int32_t fr = f->frame[ch];
        if (fr >= 0) gxDrawStamp_o(f->stamp, px + f->xoff[ch], py, fr, pal);
        px += f->adv[(uint8_t)*p++];
    } while (*p != 0);
}
static void fp_gxFontPrintf(Footprint& f, const FontHdr*, const void*, uint32_t, int32_t, int32_t, const char*, FONT_VA_FP) { fp_cur_clip(f); }
PORT_FN(0x004541a0, "gxFontPrintf", gxFontPrintf_n, fp_gxFontPrintf)

typedef char*(__cdecl* Strncpy_t)(char*, const char*, size_t);
static const Strncpy_t game_strncpy = (Strncpy_t)0x004cf3a0;
typedef void(__cdecl* FontPrintf_t)(const FontHdr*, const void*, uint32_t, int32_t, int32_t, const char*, ...);
static const FontPrintf_t gxFontPrintf_o = (FontPrintf_t)0x004541a0;
// gxFontPrintN: the first n characters, through a 256-byte buffer (FIX CANDIDATE: n >= 256 writes past it)
static void __cdecl gxFontPrintN_n(const FontHdr* f, const void* pal, uint32_t flags, int32_t x, int32_t y, const char* s, int32_t n) {
    char buf[0x100];
    game_strncpy(buf, s, (size_t)n);
    buf[n] = 0;
    gxFontPrintf_o(f, pal, flags, x, y, (const char*)0x004f1e84, buf);
}
static void fp_gxFontPrintN(Footprint& f, const FontHdr*, const void*, uint32_t, int32_t, int32_t, const char*, int32_t) { fp_cur_clip(f); }
PORT_FN(0x00454320, "gxFontPrintN", gxFontPrintN_n, fp_gxFontPrintN)

// =====================================================================================================================
// pal.obj
// =====================================================================================================================
enum : uint32_t { S_SYS_PAL = 0x004f3378, S_SYSTEM_PALETTE = 0x004f3678 };
typedef void(__cdecl* ConvPal_t)(const uint8_t*, uint16_t*);
static const ConvPal_t convert_palette_o = (ConvPal_t)0x0045a9d0;

static void __cdecl pal_build_system_n() { convert_palette_o((const uint8_t*)S_SYS_PAL, (uint16_t*)S_SYSTEM_PALETTE); }
static void fp_pal_build_system(Footprint& f) { FP_ADD(f, S_SYSTEM_PALETTE, 0x200, "pal.obj system_palette"); }
PORT_FN(0x0045a950, "pal_build_system", pal_build_system_n, fp_pal_build_system)

static uint16_t* __cdecl gxPaletteGrab_n(int16_t id) {
    gx_enter();
    if (id == -1) {
        gx_leave();
        return (uint16_t*)S_SYSTEM_PALETTE;
    }
    LogPanic((const char*)0x004f3878);
    gx_leave();
    return 0;
}
static void fp_gxPaletteGrab(Footprint&, int16_t) {}
PORT_FN(0x0045a970, "gxPaletteGrab", gxPaletteGrab_n, fp_gxPaletteGrab)

// convert_palette: 256 6-bit RGB triples to the screen's format
static void __cdecl convert_palette_n(const uint8_t* src, uint16_t* dst) {
    const int8_t fmt = (int8_t)G8(S_SCREEN_FORMAT);
    if (fmt == 4) {
        for (int i = 0; i < 0x100; i++, src += 3, dst++) {
            const uint32_t r = src[0] >> 1, g = src[1], b = src[2] >> 1;
            *dst = (uint16_t)(lo16(r << 11) | b | lo16(g << 5));
        }
        return;
    }
    if (G8(S_SCREEN_FORMAT) == 3) {
        for (int i = 0; i < 0x100; i++, src += 3, dst++) {
            const uint32_t r = src[0] >> 1, g = src[1] >> 1, b = src[2] >> 1;
            *dst = (uint16_t)(lo16(g << 5) | lo16(r << 10) | b);
        }
        return;
    }
    LogPanic((const char*)0x004f389c);
}
static void fp_convert_palette(Footprint& f, const uint8_t*, uint16_t* dst) { f.add(dst, 0x200, "palette"); }
PORT_FN(0x0045a9d0, "convert_palette", convert_palette_n, fp_convert_palette)

static __forceinline void pal_565_to_555(uint16_t* p) {
    for (int i = 0; i < 0x100; i++, p++) {
        const uint32_t v = *p;
        *p = (uint16_t)((v & 0x1f) | ((v & 0xffc0) >> 1));
    }
}
static void __cdecl gxPaletteConvert565ToScreen_n(uint16_t* p) {
    const int32_t fmt = (int8_t)G8(S_SCREEN_FORMAT);
    if (fmt == 3) pal_565_to_555(p);
    else if (fmt != 4) LogPanic((const char*)0x004f38b4);
}
static void fp_pal565(Footprint& f, uint16_t* p) { f.add(p, 0x200, "palette"); }
PORT_FN(0x0045aa90, "gxPaletteConvert565ToScreen", gxPaletteConvert565ToScreen_n, fp_pal565)

static void __cdecl gxPaletteConvert565_n(uint16_t* p, int8_t fmt) {
    if (fmt == 3) pal_565_to_555(p);
    else if (fmt != 4) LogPanic((const char*)0x004f38d4);
}
static void fp_pal565f(Footprint& f, uint16_t* p, int8_t) { f.add(p, 0x200, "palette"); }
PORT_FN(0x0045aae0, "gxPaletteConvert565", gxPaletteConvert565_n, fp_pal565f)

// =====================================================================================================================
// graph.obj: the debug graphs
// =====================================================================================================================
// GraphInfo: +0 x0, +4 y0, +8 x1, +0xc y1 (the screen rectangle), +0x10 samples, +0x14 / +0x18 x range, +0x1c / +0x20
// y range
struct GraphInfo {
    int32_t x0, y0, x1, y1, n;
    float xmin, xmax, ymin, ymax;
};
static_assert(sizeof(GraphInfo) == 0x24, "GraphInfo");
typedef uint8_t(__cdecl* GraphCoords_t)(const GraphInfo*, uint32_t, uint32_t, int32_t*, int32_t*);   // floats as bits
static const GraphCoords_t get_graph_coords_o = (GraphCoords_t)0x004d96d0;
typedef float(__cdecl* PlotFn_t)(uint32_t, void*);                                                    // float as bits
static const Rect_t gxLine_o = (Rect_t)0x00452790;
typedef void(__cdecl* TrimZeros_t)(char*);
static const TrimZeros_t trim_zeros_o = (TrimZeros_t)0x004d9690;
typedef int32_t(__cdecl* UIStyleWidth_t)(int32_t, const char*);
static const UIStyleWidth_t UIStyleWidth = (UIStyleWidth_t)0x0047f910;
typedef void(__cdecl* UIStyleDraw_t)(int32_t, int32_t, int32_t, const char*, uint32_t);
static const UIStyleDraw_t UIStyleDraw = (UIStyleDraw_t)0x0047fa40;
static __forceinline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// GraphDrawLinePlot: fn sampled n times across the x range, each segment drawn when both ends' rows lie in the
// graph (the columns aren't checked; get_graph_coords's own verdicts are ignored)
static void __cdecl GraphDrawLinePlot_n(const GraphInfo* g, PlotFn_t fn, void* ctx, uint32_t c) {
    uint32_t x = fbits(g->xmin);                                                  // integer copies of the floats
    float step = (float)((D(g->xmax) - D(g->xmin)) / D(g->n - 1));
    VP_OPAQUE(step);                    // (GCC: divided here, as the original does, though n = 1 leaves it unused)
    const float y0f = fn(x, ctx);
    uint32_t yp = fbits(y0f);
    for (int32_t i = 1; g->n > i; i++) {
        float xf;
        memcpy(&xf, &x, 4);
        const float xn = (float)(D(step) + D(xf));
        const uint32_t xnb = fbits(xn);
        const float yn = fn(xnb, ctx);
        const uint32_t ynb = fbits(yn);
        int32_t ax, ay, bx, by;
        get_graph_coords_o(g, x, yp, &ax, &ay);
        get_graph_coords_o(g, xnb, ynb, &bx, &by);
        const int32_t top = g->y0;
        if (ay >= top && ay <= g->y1 && by >= top && by <= g->y1) gxLine_o(ax, ay, bx, by, c);
        yp = ynb;
        x = xnb;
    }
}
static void fp_GraphDrawLinePlot(Footprint& f, const GraphInfo*, PlotFn_t, void*, uint32_t) { fp_cur_clip(f); }
PORT_FN(0x004d9290, "GraphDrawLinePlot", GraphDrawLinePlot_n, fp_GraphDrawLinePlot)

static void __cdecl GraphDrawBoundingBox_n(const GraphInfo* g, uint32_t c) {
    gxLine_o(g->x0, g->y0, g->x1, g->y0, c);
    gxLine_o(g->x1, g->y0, g->x1, g->y1, c);
    gxLine_o(g->x1, g->y1, g->x0, g->y1, c);
    gxLine_o(g->x0, g->y1, g->x0, g->y0, c);
}
static void fp_GraphDrawBoundingBox(Footprint& f, const GraphInfo*, uint32_t) { fp_cur_clip(f); }
PORT_FN(0x004d93b0, "GraphDrawBoundingBox", GraphDrawBoundingBox_n, fp_GraphDrawBoundingBox)

// GraphDrawAxes: the axes through (0, 0), nx ticks along x (1..nx) and ny + 1 along y (0..ny), each labelled
// (UIStyle 0x14) unless its value is within FLT_EPSILON of 0
static void __cdecl GraphDrawAxes_n(const GraphInfo* g, int32_t nx, int32_t ny, uint32_t c) {
    char buf[0x50];
    int32_t ox, oy;
    get_graph_coords_o(g, 0, 0, &ox, &oy);
    gxLine_o(g->x0, oy, g->x1, oy, c);
    gxLine_o(ox, g->y0, ox, g->y1, c);
    const double eps = FB(0x34000000);
    if (nx >= 1) {
        const float nf = (float)nx;
        for (int32_t i = 1; i <= nx; i++) {
            const int32_t x0 = g->x0;
            const float fi = (float)i;
            const int32_t x = x0 + x87_ftol(D(g->x1 - x0) / D(nf) * D(fi));
            gxLine_o(x, oy - 1, x, oy + 2, c);
            const double v = (D(g->xmax) - D(g->xmin)) / D(nf) * D(fi) + D(g->xmin);
            if (fabs(v) > eps) {
                game_sprintf(buf, (const char*)0x00503d78, v);
                trim_zeros_o(buf);
                const int32_t w = UIStyleWidth(0x14, buf);
                UIStyleDraw(0x14, w / 2 + x, oy + 3, buf, 0);
            }
        }
    }
    if (ny >= 0) {
        const float nf = (float)ny;
        for (int32_t i = 0; ny >= i; i++) {
            const int32_t y0 = g->y0;
            const float fi = (float)i;
            int32_t y = y0 + x87_ftol(D(g->y1 - y0) / D(nf) * D(fi));
            gxLine_o(ox - 1, y, ox + 2, y, c);
            const double v = (D(g->ymin) - D(g->ymax)) / D(nf) * D(fi) + D(g->ymax);
            if (fabs(v) > eps) {
                y -= 4;
                game_sprintf(buf, (const char*)0x00503d80, v);
                trim_zeros_o(buf);
                UIStyleDraw(0x14, ox - 3, y, buf, 0);
            }
        }
    }
}
static void fp_GraphDrawAxes(Footprint& f, const GraphInfo*, int32_t, int32_t, uint32_t) { fp_cur_clip(f); }
PORT_FN(0x004d9420, "GraphDrawAxes", GraphDrawAxes_n, fp_GraphDrawAxes)

typedef char*(__cdecl* Strchr_t)(const char*, int);
static const Strchr_t game_strchr = (Strchr_t)0x004ce0c0;
// trim_zeros: a number's trailing zeros after its '.' cut, and the '.' too if nothing's left after it
static void __cdecl trim_zeros_n(char* s) {
    if (!game_strchr(s, '.')) return;
    char* e = s + strlen(s) - 1;
    if (*e != '.') {
        for (;;) {
            if (*e != '0') return;
            *e = 0;
            e--;
            if (*e == '.') break;
        }
    }
    *e = 0;
}
static void fp_trim_zeros(Footprint& f, char* s) { f.add(s, (uint32_t)strlen(s) + 1, "string"); }
PORT_FN(0x004d9690, "trim_zeros", trim_zeros_n, fp_trim_zeros)

// get_graph_coords: (x, y) to the screen; true if it's inside the ranges (a NaN passes the low bounds' tests)
static uint8_t __cdecl get_graph_coords_n(const GraphInfo* g, float x, float y, int32_t* px, int32_t* py) {
    const double kx = D(g->x1 - g->x0) / (D(g->xmax) - D(g->xmin));
    *px = g->x0 + x87_ftol((D(x) - D(g->xmin)) * kx);
    const int32_t y1 = g->y1;
    const double ky = D(g->y0 - y1) / (D(g->ymax) - D(g->ymin));
    *py = y1 + x87_ftol((D(y) - D(g->ymin)) * ky);
    if (!(D(g->ymin) > D(y)) && D(g->ymax) >= D(y) && !(D(g->xmin) > D(x)) && D(g->xmax) >= D(x)) return 1;
    return 0;
}
static void fp_get_graph_coords(Footprint& f, const GraphInfo*, float, float, int32_t* px, int32_t* py) {
    f.add(px, 4, "x");
    f.add(py, 4, "y");
}
PORT_FN(0x004d96d0, "get_graph_coords", get_graph_coords_n, fp_get_graph_coords)

// =====================================================================================================================
// bmp.obj
// =====================================================================================================================
#define IAT(T, slot) (*(T*)(uintptr_t)(slot))           // a Windows function through the game's own import slot
typedef void*(__stdcall* CreateFileA_t)(const char*, uint32_t, uint32_t, void*, uint32_t, uint32_t, void*);
typedef int(__stdcall* ReadFile_t)(void*, void*, uint32_t, uint32_t*, void*);
typedef int(__stdcall* CloseHandle_t)(void*);
enum : uint32_t {
    IAT_CREATEFILEA = 0x005d7554, IAT_READFILE = 0x005d753c, IAT_CLOSEHANDLE = 0x005d7548,
    S_BMP_INFO = 0x005d5800,       // bmp.obj: BITMAPINFOHEADER (0x28)
    S_BMP_FILE = 0x005d5828,       // bmp.obj: BITMAPFILEHEADER (14)
};
typedef void(__cdecl* BmpFn_t)(const char*, gxCanvas*);
static const BmpFn_t bmp_read_o = (BmpFn_t)0x004d9dd0, bmp_write_o = (BmpFn_t)0x004da200;

static void __cdecl gxReadBMP_n(const char* name, gxCanvas* c) {
    gx_enter();
    bmp_read_o(name, c);
    gx_leave();
}
static void fp_bmp(Footprint& f, const char*, gxCanvas*) { f.replay_only = "reads or writes a file"; }
PORT_FN(0x004d9d50, "gxReadBMP", gxReadBMP_n, fp_bmp)

static void __cdecl gxWriteBMP_n(const char* name, gxCanvas* c) {
    gx_enter();
    bmp_write_o(name, c);
    gx_leave();
}
PORT_FN(0x004d9d90, "gxWriteBMP", gxWriteBMP_n, fp_bmp)

// bmp_read: a 24-bit BMP of exactly the canvas's size, bottom row first, into a 565 / 555 / 1555 (black
// transparent) / 4444 canvas. It reads the pixels straight after the two headers, ignores row padding and bfOffBits,
// and steps rows by width x 2 rather than the pitch (faithful).
static void __cdecl bmp_read_n(const char* name, gxCanvas* c) {
    uint32_t nread;
    void* const h = IAT(CreateFileA_t, IAT_CREATEFILEA)(name, 0x80000000u, 1, 0, 3, 1, 0);
    if (h == (void*)(intptr_t)-1) {
        LogReport((const char*)0x00503e88, name);
        return;
    }
    const ReadFile_t read = IAT(ReadFile_t, IAT_READFILE);
    if (!read(h, (void*)S_BMP_FILE, 0xe, &nread, 0)) LogReport((const char*)0x00503e9c);
    if (G16(S_BMP_FILE) != 0x4d42) {
        LogReport((const char*)0x00503ea8, name);
        IAT(CloseHandle_t, IAT_CLOSEHANDLE)(h);
        return;
    }
    read(h, (void*)S_BMP_INFO, 0x28, &nread, 0);
    if (G16(S_BMP_INFO + 0xe) != 0x18) {
        LogReport((const char*)0x00503ec8, name);
        IAT(CloseHandle_t, IAT_CLOSEHANDLE)(h);
        return;
    }
    if (G32(S_BMP_INFO + 4) != c->w) {
        LogReport((const char*)0x00503ee4, name, c->w, G32(S_BMP_INFO + 4));
        IAT(CloseHandle_t, IAT_CLOSEHANDLE)(h);
        return;
    }
    if (G32(S_BMP_INFO + 8) != c->h) {
        LogReport((const char*)0x00503f0c, name, c->h, G32(S_BMP_INFO + 8));
        IAT(CloseHandle_t, IAT_CLOSEHANDLE)(h);
        return;
    }
    if (G32(S_BMP_INFO + 0x10) != 0) {
        LogReport((const char*)0x00503f34, name);
        IAT(CloseHandle_t, IAT_CLOSEHANDLE)(h);
        return;
    }
    {
        const int8_t fmt = c->format;
        if (fmt != 3 && fmt != 4 && fmt != 2 && fmt != 1) {
            LogReport((const char*)0x00503f4c, name);
            IAT(CloseHandle_t, IAT_CLOSEHANDLE)(h);
            return;
        }
    }
    const int32_t size = G32(S_BMP_FILE + 2) - G32(S_BMP_FILE + 0xa);
    uint8_t* const buf = (uint8_t*)MemAlloc(size);
    if (!read(h, buf, (uint32_t)size, &nread, 0)) LogReport((const char*)0x00503f6c);
    const uint8_t* s = buf;
    const int32_t rows = c->h;
    const int8_t fmt = c->format;
    uint8_t* d = c->pixels + imul(imul(rows - 1, c->w), 2);
    if (fmt == 4) {
        if (rows > 0) {
            int32_t row = 0;
            do {
                for (int32_t col = 0; c->w > col; col++) {
                    const uint32_t b = s[0] >> 3, g = s[1], r = s[2];
                    s += 3;
                    d += 2;
                    w16(d - 2, ((lo16((r & 0xfff8) << 8) | lo16(g << 3)) & 0xffe0) | b);
                }
                d -= c->w * 4;
                row++;
            } while (c->h > row);
        }
    } else if (fmt == 3) {
        if (rows > 0) {
            int32_t row = 0;
            do {
                for (int32_t col = 0; c->w > col; col++) {
                    const uint32_t b = s[0] >> 3, g = s[1], r = s[2];
                    s += 3;
                    d += 2;
                    w16(d - 2, lo16(((lo16((r & 0xfff8) << 5) | g) & 0xfff8) << 2) | b);
                }
                d -= c->w * 4;
                row++;
            } while (c->h > row);
        }
    } else if (fmt == 2) {
        LogReport((const char*)0x00503f78);
        if (c->h > 0) {
            int32_t row = 0;
            do {
                for (int32_t col = 0; c->w > col; col++) {
                    const uint32_t b = s[0], g = s[1], r = s[2];
                    s += 3;
                    uint32_t v = lo16(((lo16((r & 0xfff8) << 5) | g) & 0xfff8) << 2) | (b >> 3);
                    if (r == 0 && g == 0 && b == 0) v = 0;
                    else v |= 0x8000;
                    w16(d, v);
                    d += 2;
                }
                d -= c->w * 4;
                row++;
            } while (c->h > row);
        }
    } else if (fmt == 1) {
        LogReport((const char*)0x00503f84);
        if (c->h > 0) {
            int32_t row = 0;
            do {
                for (int32_t col = 0; c->w > col; col++) {
                    const uint32_t b = s[0] >> 4, g = s[1], r = s[2];
                    s += 3;
                    d += 2;
                    w16(d - 2, (((lo16((r & 0xfff0) << 4) | g) & 0xfff0) | b) | 0x7000);
                }
                d -= c->w * 4;
                row++;
            } while (c->h > row);
        }
    }
    MemFree(buf);
    IAT(CloseHandle_t, IAT_CLOSEHANDLE)(h);
}
PORT_FN(0x004d9dd0, "bmp_read", bmp_read_n, fp_bmp)

// bmp_write: a 565 canvas as a 24-bit BMP, one FileWrite per pixel. Its header gives bfOffBits = 40 (not 54) and
// leaves biYPelsPerMeter uninitialised (four bytes of stack go into the file; FIX CANDIDATE).
#pragma pack(push, 1)
struct BmpHeaders {
    uint16_t type; uint32_t size; uint16_t res1, res2; uint32_t offbits;     // 14
    uint16_t _pad;
    uint32_t bi_size; int32_t w, h; uint16_t planes, bits; uint32_t compression, size_image;
    int32_t xppm, yppm; uint32_t clr_used, clr_important;                     // 0x28
};
#pragma pack(pop)
static void __cdecl bmp_write_n(const char* name, gxCanvas* c) {
    BmpHeaders H;
    int32_t fh;
    uint8_t bgr[4];
    H.type = 0x4d42;
    const int32_t w = c->w, hh = c->h;
    H.res1 = 0;
    H.offbits = 0x28;
    H.res2 = 0;
    H.bi_size = 0x28;
    H.w = w;
    H.h = hh;
    H.compression = 0;
    H.size_image = 0;
    H.xppm = 0x3e8;
    H.clr_used = 0;
    H.clr_important = 0;
    H.planes = 1;
    H.bits = 0x18;
    H.size = (uint32_t)imul(w, hh) * 3 + 0x36;
    fh = FileCreate(name);
    if (!fh) {
        LogReport((const char*)0x00503f90, name);
        return;
    }
    FileWrite(fh, &H.type, 0xe);
    FileWrite(fh, &H.bi_size, 0x28);
    for (int32_t row = c->h - 1; row >= 0; row--) {
        const uint8_t* p = c->pixels + imul(c->pitch, row);
        for (int32_t col = 0; c->w > col; col++) {
            const uint32_t v = r16(p);
            p += 2;
            bgr[2] = (uint8_t)((v >> 8) & 0xf8);
            bgr[0] = (uint8_t)(v << 3);
            bgr[1] = (uint8_t)((v >> 5) << 2);
            FileWrite(fh, bgr, 3);
        }
    }
    FileClose(fh);
}
PORT_FN(0x004da200, "bmp_write", bmp_write_n, fp_bmp)

// =====================================================================================================================
// The debug screens: ai.obj (the AI dashboard, the learn-mode screen), notes.obj (the learn dialog), phystask.obj
// (the camera dashboard), physdash.obj (the physics dashboard)
// =====================================================================================================================
struct Point2Df { float x, y; };
struct P3Df { float x, y, z; };
typedef int32_t(__cdecl* IntV_t)();
typedef void(__cdecl* VoidV_t)();
typedef void(__cdecl* VoidI_t)(int32_t);
static const IntV_t WorldGetFocusCar = (IntV_t)0x004626d0;
typedef uint8_t(__cdecl* MouseGetEvent_t)(void*);
static const MouseGetEvent_t MouseGetEvent = (MouseGetEvent_t)0x004144f0;
typedef void(__cdecl* HandleMouse_t)(void*);
static const HandleMouse_t handle_mouse_evt_o = (HandleMouse_t)0x0041ca70;
static const VoidV_t MouseCenter = (VoidV_t)0x00414620;
static const VoidV_t draw_status_o = (VoidV_t)0x0041ca80, draw_focus_iline_o = (VoidV_t)0x0041cc60,
                     draw_mouse_cursor_o = (VoidV_t)0x0041cca0, draw_target_o = (VoidV_t)0x0041cd00;
typedef float(__cdecl* TerrainH_t)(uint32_t, uint32_t);                    // (x, z) as float bits
static const TerrainH_t TerrainGetHeight = (TerrainH_t)0x00465ae0;
typedef uint8_t(__cdecl* Project3_t)(const P3Df*, P3Df*);
static const Project3_t mrProjectPoint = (Project3_t)0x0044f200;
typedef uint8_t(__cdecl* Project2_t)(const Point2Df*, Point2Df*);
static const Project2_t project2d = (Project2_t)0x00421110;
typedef void(__fastcall* IdealDraw_t)(void*, int, const Point2Df*, uint32_t);
static const IdealDraw_t IdealLine_Draw = (IdealDraw_t)0x00421c30;
typedef void(__cdecl* Circle_t)(int32_t, int32_t, int32_t, uint32_t);
static const Circle_t gxCircle_o = (Circle_t)0x00452e80;

enum : uint32_t {
    S_AI_CARS = 0x00509920,        // AICar*[] by car index
    S_AI_FOCUS = 0x00509984,       // the focus car's AICar*
    S_AI_TICK = 0x005099c4,        // the dashboard's heartbeat pixel (0..31)
    S_AI_MOUSE = 0x005099b8,       // P3D: the mouse on the ground (x, y, z)
    S_AI_MOUSE_MODE = 0x004eb674,  // byte
    S_AI_STATUS = 0x004eb670,      // the status page (0..3)
    S_AI_TEXT = 0x005219e0,        // 8 x 0x50 status lines
    S_GX_SCREEN_WID = 0x005228f4,
    S_GX_SCREEN_HIT = 0x005228d4,
    S_DEITY = 0x005218ac,
};

// AIDashboardDraw: the focus car's AI dashboard. In mouse mode, a mouse event moves the ground cursor by half its
// offset from the screen's centre (x) and from row 240 (z).
static void __cdecl AIDashboardDraw_n(gxCanvas* c) {
    struct { int32_t t; int32_t ev[4]; } L;                      // the MouseEvent at +4, as the original's frame
    GU32(S_AI_FOCUS) = GU32(S_AI_CARS + WorldGetFocusCar() * 4);
    if (!GU32(S_AI_FOCUS)) return;
    gxSetCanvas_o(c);
    const uint32_t col = G8(S_AI_MOUSE_MODE) ? GU32(0x00509988) : GU32(0x00509968);
    const int32_t x = G32(S_AI_TICK);
    G32(S_AI_TICK) = x + 1;
    gxPoint_o(x, 0, col);
    G32(S_AI_TICK) = G32(S_AI_TICK) & 0x1f;
    draw_mouse_cursor_o();
    if (G8(S_AI_MOUSE_MODE) && MouseGetEvent(L.ev)) {
        L.t = L.ev[1] - 0x140;
        GF(S_AI_MOUSE) = (float)(D(L.t) * 0.5 + D(GF(S_AI_MOUSE)));
        L.t = 0xf0 - L.ev[2];
        GF(S_AI_MOUSE + 8) = (float)(D(L.t) * 0.5 + D(GF(S_AI_MOUSE + 8)));
        handle_mouse_evt_o(L.ev);
    }
    MouseCenter();
    draw_status_o();
    draw_target_o();
}
static void fp_AIDashboardDraw(Footprint& f, gxCanvas*) { f.replay_only = "MouseGetEvent pumps the window's messages; MouseCenter moves the cursor"; }
PORT_FN(0x0041c980, "AIDashboardDraw", AIDashboardDraw_n, fp_AIDashboardDraw)
// handle_mouse_evt (0x41ca70) and AIFGUpdate (0x41d190) are a bare ret: too short to hook

// draw_status: page 0 a caption, 1 the eight status lines (the focus car's white), 2 the ground cursor's line to the
// picked point and the "foot" status, 3 two lines of help
static void __cdecl draw_status_n() {
    char buf[0x94];
    draw_focus_iline_o();
    switch (GU32(S_AI_STATUS)) {
    case 0:
        gxText_o(0, 0xf0, (const char*)0x004eb7a4, GU32(0x00509988));
        return;
    case 1: {
        gxText_o(0, 0, (const char*)0x004eb79c, GU32(0x00509988));
        int32_t i = 0, y = 0x14;
        const int32_t focus = WorldGetFocusCar();
        for (uint32_t p = S_AI_TEXT; p < 0x00521c60; p += 0x50) {
            const uint32_t col = i == focus ? GU32(0x00509988) : GU32(0x00509968);
            i++;
            gxText_o(1, y, (const char*)p, col);
            y += 0xa;
        }
        return;
    }
    case 2: {
        if (G8(0x005099a4)) {
            struct { P3Df a; P3Df b; P3Df m; } L;
            const uint32_t x = GU32(0x005099a8), z = GU32(0x005099ac);
            memcpy(&L.a.x, &x, 4);
            memcpy(&L.a.z, &z, 4);
            L.a.y = 0;
            L.a.y = TerrainGetHeight(x, z);
            if (mrProjectPoint((const P3Df*)S_AI_MOUSE, &L.m) && mrProjectPoint(&L.a, &L.b)) {
                const uint32_t col = GU32(0x005099c8);
                const int32_t by = x87_ftol(L.b.y), bx = x87_ftol(L.b.x);
                const int32_t my = x87_ftol(L.m.y), mx = x87_ftol(L.m.x);
                gxLine_o(mx, my, bx, by, col);
            }
        }
        const double foot = D(GF(0x005099a0)) * 2.25;
        game_sprintf(buf, (const char*)0x004eb784, 6.5, foot);
        gxText_o(0xf0, 0x50, buf, GU32(0x00509988));
        return;
    }
    case 3:
        gxText_o(0x64, 0x64, (const char*)0x004eb740, GU32(0x00509988));
        gxText_o(0x64, 0x6e, (const char*)0x004eb764, GU32(0x00509988));
        return;
    default:
        return;
    }
}
static void fp_cur_void(Footprint& f) { fp_cur_clip(f); }
PORT_FN(0x0041ca80, "draw_status", draw_status_n, fp_cur_void)

// draw_focus_iline: the focus car's racing line, highlighted near the ground cursor (unless on page 2)
static void __cdecl draw_focus_iline_n() {
    Point2Df p;
    memcpy(&p.x, (const void*)S_AI_MOUSE, 4);
    memcpy(&p.y, (const void*)(S_AI_MOUSE + 8), 4);
    const uint32_t flag = (uint32_t)(G32(S_AI_STATUS) - 2) < 1 ? 0 : 1;
    IdealLine_Draw((void*)(GU32(S_AI_FOCUS) + 0x10), 0, &p, flag);
}
PORT_FN(0x0041cc60, "draw_focus_iline", draw_focus_iline_n, fp_cur_void)

// draw_mouse_cursor: the ground cursor dropped on to the terrain and circled
static void __cdecl draw_mouse_cursor_n() {
    P3Df out;
    GF(S_AI_MOUSE + 4) = TerrainGetHeight(GU32(S_AI_MOUSE), GU32(S_AI_MOUSE + 8));
    if (mrProjectPoint((const P3Df*)S_AI_MOUSE, &out)) {
        const int32_t y = x87_ftol(out.y);
        gxCircle_o(x87_ftol(out.x), y, 3, GU32(0x00509988));
    }
}
static void fp_draw_mouse_cursor(Footprint& f) {
    FP_ADD(f, S_AI_MOUSE + 4, 4, "ai.obj mouse cursor y");
    fp_cur_clip(f);
}
PORT_FN(0x0041cca0, "draw_mouse_cursor", draw_mouse_cursor_n, fp_draw_mouse_cursor)

// draw_target: the AI's target circled, a line to target + foo, and a second point circled
static void __cdecl draw_target_n() {
    struct { Point2Df s; Point2Df q; Point2Df t; } L;
    if (project2d((const Point2Df*)0x00509910, &L.s)) {
        const int32_t y = x87_ftol(L.s.y);
        gxCircle_o(x87_ftol(L.s.x), y, 3, GU32(0x005099c8));
        L.q.x = (float)(D(GF(0x00509910)) + D(GF(0x00509978)));
        L.q.y = (float)(D(GF(0x00509914)) + D(GF(0x0050997c)));
        if (project2d(&L.q, &L.t)) {
            const uint32_t col = GU32(0x00509988);
            const int32_t ty = x87_ftol(L.t.y), tx = x87_ftol(L.t.x);
            const int32_t sy = x87_ftol(L.s.y), sx = x87_ftol(L.s.x);
            gxLine_o(sx, sy, tx, ty, col);
        }
    }
    if (project2d((const Point2Df*)0x00509960, &L.s)) {
        const int32_t y = x87_ftol(L.s.y);
        gxCircle_o(x87_ftol(L.s.x), y, 3, GU32(0x00509970));
    }
}
PORT_FN(0x0041cd00, "draw_target", draw_target_n, fp_cur_void)

// ---- AIDisplayLearnMode ---------------------------------------------------------------------------------------------
typedef void*(__cdecl* CarMgrGetInfo_t)(int32_t);
static const CarMgrGetInfo_t CarMgrGetInfo = (CarMgrGetInfo_t)0x00464490;
static const VoidI_t PhysicsSetSpeed = (VoidI_t)0x0042bd30, TaskSleep = (VoidI_t)0x00414de0;
static const VoidV_t PhysicsUnpause = (VoidV_t)0x0042bcf0, SoundMuteCars = (VoidV_t)0x00471dd0,
                     PhysicsPause = (VoidV_t)0x0042bcc0, clear_screen_o = (VoidV_t)0x00423850,
                     learn_draw_o = (VoidV_t)0x004237a0, gxReleaseScreen = (VoidV_t)0x0044e210,
                     gxFlipScreen = (VoidV_t)0x0044e180, Win32Idle = (VoidV_t)0x00412bf0,
                     PhysicsAbortRace = (VoidV_t)0x0042bc00, KeyClear = (VoidV_t)0x00413c50;
static const IntV_t GetGameState = (IntV_t)0x0040a3d0;
typedef uint8_t(__cdecl* Grab_t)(gxCanvas*);
static const Grab_t gxGrabScreen = (Grab_t)0x0044e1c0;
typedef float(__cdecl* FloatV_t)();
static const FloatV_t WorldGetElapsedTime = (FloatV_t)0x00462050;
typedef const char*(__cdecl* TimeString_t)(float, uint8_t);
static const TimeString_t PhysicsTimeString = (TimeString_t)0x0042bf30;
typedef const char*(__cdecl* TimeStringB_t)(uint32_t, uint32_t);          // a float passed as its bits
static const TimeStringB_t PhysicsTimeStringB = (TimeStringB_t)0x0042bf30;
typedef const uint8_t*(__cdecl* LapResult_t)(int32_t, int32_t);
static const LapResult_t RecordGetLapResult = (LapResult_t)0x0042a940;
typedef uint8_t(__cdecl* KeyHit_t)();
static const KeyHit_t KeyHit = (KeyHit_t)0x00413cc0;
typedef uint16_t(__cdecl* KeyGet_t)();
static const KeyGet_t KeyGet = (KeyGet_t)0x00413c90;
typedef void(__cdecl* OptionsGetB_t)(const char*, const char*, uint8_t*);
static const OptionsGetB_t OptionsGetBool = (OptionsGetB_t)0x00471470;
typedef int32_t(__fastcall* DeityInt_t)(void*, int, int32_t);
static __forceinline int32_t deity_call(uint32_t slot, int32_t a) {
    void* d = GP(void, S_DEITY);
    return (*(DeityInt_t*)(*(uint8_t**)d + slot))(d, 0, a);
}

// AIDisplayLearnMode: the learn-mode screen, redrawn (half-second sleeps while not racing; 31 frames once racing)
// until Esc or '1' (which also sets 0x520ad0). It leaves gfx.obj's current canvas pointing into its own stack frame,
// and its closing message is drawn through that stale canvas, whose format byte the OptionsGet flag has overwritten
// (faithful: the flag shares the canvas's first byte here as in the original's frame).
static void __cdecl AIDisplayLearnMode_n(const uint8_t* world) {
    struct { gxCanvas screen; char buf[0x70]; } L;              // the flag byte is L.screen's first byte
    PhysicsSetSpeed(0);
    PhysicsUnpause();
    SoundMuteCars();
    const uint8_t* info = (const uint8_t*)CarMgrGetInfo(0);
    clear_screen_o();
    uint8_t done = 0;
    int32_t frames = 0;
    for (;;) {
        if (GetGameState() != 3) TaskSleep(500);
        else {
            PhysicsPause();
            if (frames++ > 0x1e) break;
        }
        if (gxGrabScreen(&L.screen)) {
            gxSetCanvas_o(&L.screen);
            gxClear_o(GU32(0x00509968));
            learn_draw_o();
            const uint32_t c1 = GU32(0x005099c8), c2 = GU32(0x005099d8);
            const int32_t dy = gxTextHeight_o() + 3;
            gxTextCentered_o(G32(S_GX_SCREEN_WID) / 2, 10, (const char*)(info + 5), c2);
            int32_t y = dy + 10;
            gxTextCentered_o(G32(S_GX_SCREEN_WID) / 2, y, (const char*)0x004eb7ac, c1);
            y += dy;
            gxTextCentered_o(G32(S_GX_SCREEN_WID) / 2, y, (const char*)(info + 0x12), c2);
            y += dy;
            gxTextCentered_o(G32(S_GX_SCREEN_WID) / 2, y, (const char*)0x004eb7c4, c1);
            y += dy;
            game_sprintf(L.buf, (const char*)0x004eb7d8, world + 8, world[0xcd1] ? (const char*)0x004eb7c8 : (const char*)0x004eb7d4);
            gxTextCentered_o(G32(S_GX_SCREEN_WID) / 2, y, L.buf, c2);
            y += dy;
            y += dy;
            const float t = WorldGetElapsedTime();
            const char* ts = PhysicsTimeString(t, 1);
            gxTextCentered_o(G32(S_GX_SCREEN_WID) / 2, y, ts, c1);
            y = 0x64;
            int32_t lap = 1;
            if (deity_call(0x48, 0) > 0x14) lap = deity_call(0x48, 0) - 0x14;
            if (deity_call(0x48, 0) > lap) {
                do {
                    const uint8_t* rec = RecordGetLapResult(0, lap);
                    if (!rec) break;
                    const char* s = PhysicsTimeStringB(*(const uint32_t*)(rec + 4), 1);
                    game_sprintf(L.buf, (const char*)0x004eb7e0, lap, s);
                    lap++;
                    gxText_o(0x190, y, L.buf, c1);
                    y += dy;
                } while (deity_call(0x48, 0) > lap);
            }
            gxReleaseScreen();
            gxFlipScreen();
        }
        Win32Idle();
        if (KeyHit()) {
            const uint32_t k = KeyGet();
            if (k == 0x1b) done = 1;
            else if (k == 0x31) {
                done = 1;
                G8(0x00520ad0) = 1;
            }
        }
        if ((uint8_t)deity_call(0x40, 0)) PhysicsAbortRace();
        if (done) break;
    }
    uint8_t* const flag = (uint8_t*)&L.screen;
    *flag = 0;
    OptionsGetBool(GP(const char, 0x004db500), (const char*)0x004eb7f0, flag);
    if (*flag) {
        gxCanvas second;
        KeyClear();
        if (!gxGrabScreen(&second)) {
            do Win32Idle();
            while (!gxGrabScreen(&second));
        }
        gxTextCentered_o(0x140, 0x1ae, (const char*)0x004eb7fc, GU32(0x00509974));
        gxTextCentered_o(0x140, 0x1b8, (const char*)0x004eb824, GU32(0x00509974));
        gxTextCentered_o(0x140, 0x1c2, (const char*)0x004eb84c, GU32(0x00509974));
        gxReleaseScreen();
        gxFlipScreen();
        if (!KeyHit()) {
            do Win32Idle();
            while (!KeyHit());
        }
        KeyGet();
    }
    clear_screen_o();
}
static void fp_AIDisplayLearnMode(Footprint& f, const uint8_t*) { f.replay_only = "an interactive loop (keys, sleeps, flips); leaves the current canvas on its stack"; }
PORT_FN(0x0041cdf0, "AIDisplayLearnMode", AIDisplayLearnMode_n, fp_AIDisplayLearnMode)

// ---- notes.obj ------------------------------------------------------------------------------------------------------
static void __cdecl learn_draw_n() {
    char buf[0x40];
    game_sprintf(buf, (const char*)0x004ec4f8, G32(0x00520ae8), G32(0x00520aec));
    gxTextCentered_o(G32(S_GX_SCREEN_WID) / 2, 0x190, buf, GU32(0x00520b04));
}
PORT_FN(0x004237a0, "learn_draw", learn_draw_n, fp_cur_void)

// clear_screen: three pages cleared and flipped (retrying until three locks succeed); the current canvas is left
// pointing at its stack frame (faithful; FIX CANDIDATE: a draw before the next gxSetCanvas goes through a dead frame)
static void __cdecl clear_screen_n() {
    int32_t n = 0;
    do {
        gxCanvas c;
        if (gxGrabScreen(&c)) {
            n++;
            gxSetCanvas_o(&c);
            gxClear_o(GU32(0x00520abc));
            gxReleaseScreen();
            gxFlipScreen();
        }
    } while (n < 3);
}
// the pages it clears are surfaces it locks itself (compared by the emulation at each Unlock); the current canvas is
// left aimed at its own frame
static void fp_clear_screen(Footprint& f) { f.stack_ptr((void*)(uintptr_t)S_CANVAS, "gfx.obj current canvas (left on clear_screen's stack)"); }
PORT_FN(0x00423850, "clear_screen", clear_screen_n, fp_clear_screen)

typedef void(__cdecl* OptionsGetS_t)(const char*, const char*, char*, int32_t);
static const OptionsGetS_t OptionsGetStr = (OptionsGetS_t)0x00471500;
typedef void(__cdecl* ConvBool_t)(uint8_t*, char*, int32_t);
static const ConvBool_t convert_to_bool_o = (ConvBool_t)0x00423820;
typedef void(__cdecl* ConvStr_t)(char*, uint8_t*, int32_t);
static const ConvStr_t convert_to_string_o = (ConvStr_t)0x004237f0;
typedef void(__fastcall* ItemCtor_t)(void*, int, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, int32_t,
                                     void*, int32_t, uint32_t, uint32_t, int32_t*, const char*);
static const ItemCtor_t UIDialogItem_ctor = (ItemCtor_t)0x004091c0;
typedef const char*(__cdecl* NameOf_t)(int32_t);
static const NameOf_t GetTrackName = (NameOf_t)0x00406810, GetAIStrengthString = (NameOf_t)0x004061a0;
typedef int32_t(__cdecl* UIDoDialog_t)(const void*, int32_t, int32_t, int32_t, int32_t, int32_t);
static const UIDoDialog_t UIDoDialog = (UIDoDialog_t)0x00478ff0;
typedef void(__cdecl* OptionsSetS_t)(const char*, const char*, const char*);
static const OptionsSetS_t OptionsSetStr = (OptionsSetS_t)0x00471560;
typedef void(__cdecl* OptionsSetB_t)(const char*, const char*, uint32_t);
static const OptionsSetB_t OptionsSetBool = (OptionsSetB_t)0x004714c0;
typedef void(__cdecl* CheckLearning_t)(uint8_t*, uint8_t*);
static const CheckLearning_t check_learning = (CheckLearning_t)0x004241c0;

struct DialogItem {                // UIDialogItem, 0x38
    int32_t type, a2, x, y, a5, a6;
    const char* text;
    int32_t a8;
    void* data;
    int32_t a10;
    uint32_t f1, f2;
    int32_t* a13;
    const char* a14;
};
static_assert(sizeof(DialogItem) == 0x38, "UIDialogItem");
// an item the original builds with the constructor
static __forceinline void item_ctor(DialogItem* it, int32_t type, int32_t a2, int32_t x, int32_t y, const char* text, void* data, int32_t a10) {
    UIDialogItem_ctor(it, 0, type, a2, x, y, 0, 0, text, 0, data, a10, 0, 0, 0, 0);
}
// one the original builds inline (the constructor's stores, written out)
static __forceinline void item_inline(DialogItem* it, int32_t type, int32_t x, int32_t y, const char* text, void* data, int32_t a10) {
    it->type = type;
    it->a2 = 0;
    it->a5 = 0;
    it->a6 = 0;
    it->text = text;
    it->a8 = 0;
    it->data = data;
    it->a10 = a10;
    it->f1 = 0;
    it->f2 = 0;
    it->a13 = 0;
    it->x = x;
    it->y = y;
    it->a14 = 0;
}

// learn_dialog: which tracks (8 x 2 columns) and AI strengths (7) the AI learns on, from and back to the options file
static void __cdecl learn_dialog_n() {
    struct {
        uint8_t _p[3];
        uint8_t flag;              // +0x13
        uint8_t strength[8];       // +0x14
        uint8_t tracks[16];        // +0x1c
        char s_strength[9];        // +0x2c
        uint8_t _q[3];
        struct { const char* title; int32_t a, b; DialogItem* items; int32_t c; } dlg;   // +0x38
        char s_tracks[17];         // +0x4c
        uint8_t _r[3];
        DialogItem items[28];      // +0x60
    } L;
    clear_screen_o();
    const char* const sec = GP(const char, 0x004db718);
    L.flag = 1;
    OptionsGetBool(sec, (const char*)0x004ec508, &L.flag);
    memcpy(L.s_strength, (const void*)0x004ec514, 4);
    memset(L.s_strength + 4, 0, 5);
    OptionsGetStr(sec, (const char*)0x004ec518, L.s_strength, 9);
    convert_to_bool_o(L.strength, L.s_strength, 8);
    memcpy(L.s_tracks, (const void*)0x004ec524, 0x11);
    OptionsGetStr(sec, (const char*)0x004ec538, L.s_tracks, 0x11);
    convert_to_bool_o(L.tracks, L.s_tracks, 0x10);
    DialogItem* it = L.items;
    item_ctor(&it[0], 0xb, 0, 0x64, 0x190, (const char*)0x004ec544, &L.flag, 7);
    item_ctor(&it[1], 5, 0, 0x1a4, 0xb8, (const char*)0x004ec55c, 0, 0xc);
    item_ctor(&it[2], 0xb, 0, 0x118, 0xc8, GetTrackName(0), &L.tracks[0], 7);
    item_ctor(&it[3], 0xb, 0, 0x118, 0xd6, GetTrackName(1), &L.tracks[1], 7);
    item_ctor(&it[4], 0xb, 0, 0x118, 0xe4, GetTrackName(2), &L.tracks[2], 7);
    item_ctor(&it[5], 0xb, 0, 0x118, 0xf2, GetTrackName(3), &L.tracks[3], 7);
    item_ctor(&it[6], 0xb, 0, 0x118, 0x100, GetTrackName(4), &L.tracks[4], 7);
    item_ctor(&it[7], 0xb, 0, 0x118, 0x10e, GetTrackName(5), &L.tracks[5], 7);
    item_ctor(&it[8], 0xb, 0, 0x118, 0x11c, GetTrackName(6), &L.tracks[6], 7);
    item_ctor(&it[9], 0xb, 0, 0x118, 0x12a, GetTrackName(7), &L.tracks[7], 7);
    item_ctor(&it[10], 0xb, 0, 0x1a4, 0xc8, GetTrackName(0), &L.tracks[8], 7);
    item_inline(&it[11], 0xb, 0x1a4, 0xd6, GetTrackName(1), &L.tracks[9], 7);
    item_inline(&it[12], 0xb, 0x1a4, 0xe4, GetTrackName(2), &L.tracks[10], 7);
    item_ctor(&it[13], 0xb, 0, 0x1a4, 0xf2, GetTrackName(3), &L.tracks[11], 7);
    item_inline(&it[14], 0xb, 0x1a4, 0x100, GetTrackName(4), &L.tracks[12], 7);
    item_ctor(&it[15], 0xb, 0, 0x1a4, 0x10e, GetTrackName(5), &L.tracks[13], 7);
    item_inline(&it[16], 0xb, 0x1a4, 0x11c, GetTrackName(6), &L.tracks[14], 7);
    item_ctor(&it[17], 0xb, 0, 0x1a4, 0x12a, GetTrackName(7), &L.tracks[15], 7);
    item_inline(&it[18], 0xb, 0x64, 0x64, GetAIStrengthString(0), &L.strength[0], 7);
    item_ctor(&it[19], 0xb, 0, 0x64, 0x72, GetAIStrengthString(1), &L.strength[1], 7);
    item_inline(&it[20], 0xb, 0x64, 0x80, GetAIStrengthString(2), &L.strength[2], 7);
    item_inline(&it[21], 0xb, 0x64, 0x8e, GetAIStrengthString(3), &L.strength[3], 7);
    item_ctor(&it[22], 0xb, 0, 0x64, 0x9c, GetAIStrengthString(4), &L.strength[4], 7);
    item_inline(&it[23], 0xb, 0x64, 0xaa, GetAIStrengthString(5), &L.strength[5], 7);
    item_ctor(&it[24], 0xb, 0, 0x64, 0xb8, GetAIStrengthString(6), &L.strength[6], 7);
    item_inline(&it[25], 2, 0x1fe, 0x1b2, (const char*)0x004ec568, 0, 0);
    it[25].a2 = -2;
    UIDialogItem_ctor(&it[26], 0, 4, -1, 0x1b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    memcpy(&it[27], (const void*)0x00578d08, 0x38);             // rep movsd: the list's terminator
    L.dlg.a = 0;
    L.dlg.items = it;
    L.dlg.c = 0;
    L.dlg.title = (const char*)0x004ec56c;
    L.dlg.b = -1;
    const uint8_t ok = UIDoDialog(&L.dlg, G32(S_GX_SCREEN_WID), G32(S_GX_SCREEN_HIT), -999, -999, 1) != -1;
    convert_to_string_o(L.s_tracks, L.tracks, 0x10);
    OptionsSetStr(sec, (const char*)0x004ec578, L.s_tracks);
    convert_to_string_o(L.s_strength, L.strength, 8);
    OptionsSetStr(sec, (const char*)0x004ec584, L.s_strength);
    OptionsSetBool(sec, (const char*)0x004ec590, L.flag);
    if (ok) check_learning(L.tracks, L.strength);
}
static void fp_learn_dialog(Footprint& f) { f.replay_only = "an interactive dialog; reads and writes the options file"; }
PORT_FN(0x004238a0, "learn_dialog", learn_dialog_n, fp_learn_dialog)
// ASSERT_MSG (0x424690 notes.obj, 0x426d70 phystask.obj), CameraDashboardBegin / End, PhysicsDashboardBegin / End:
// a bare ret, too short to hook

// ---- phystask.obj: the camera dashboard -----------------------------------------------------------------------------
typedef void(__cdecl* ToQuat_t)(Q4*, const void*);
static const ToQuat_t MatrixToQuat = (ToQuat_t)0x004294c0;
static const float k_cam_eps = 1.1920928955078125e-07f;          // 0x34000000
// acos(q.w) through the C runtime's _CIacos (0x4cf07c: argument and result in ST0), stored to a; its sine (of the
// unrounded acos) stored to s; true if |sine| > FLT_EPSILON (the unrounded sine is compared)
VP_ASM_CALLS static uint8_t cam_acos_sin(const float* qw, float* a, float* s) {
    uint8_t big;
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[qw]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "mov eax, 0x004cf07c\n\t"
                     "call eax\n\t"
                     "mov eax, %[a]\n\t"
                     "fst dword ptr [eax]\n\t"
                     "fsin\n\t"
                     "mov eax, %[s]\n\t"
                     "fst dword ptr [eax]\n\t"
                     "fabs\n\t"
                     "fcomp %[eps]\n\t"
                     "fnstsw ax\n\t"
                     "test ah, 0x41\n\t"
                     "setz %[big]"
                     : [big] "=m"(big)
                     : [qw] "m"(qw), [a] "m"(a), [s] "m"(s), [eps] "m"(k_cam_eps)
                     : VP_X87_CLOBBERS, "eax", "ecx", "edx", "cc", "memory");
#else
    __asm {
        mov eax, qw
        fld dword ptr [eax]
        mov eax, 0x004cf07c
        call eax
        mov eax, a
        fst dword ptr [eax]
        fsin
        mov eax, s
        fst dword ptr [eax]
        fabs
        fcomp k_cam_eps
        fnstsw ax
        test ah, 0x41
        setz big
    }
#endif
    return big;
}

// CameraDashboardDraw: the camera's frame as an axis-angle (degrees) and its position
static void __cdecl CameraDashboardDraw_n(gxCanvas* c) {
    struct { P3Df v; float s; Q4 q; char buf[0x50]; } L;
    gxSetCanvas_o(c);
    gxRect_o(10, 10, 0x17c, 0x3c, GU32(0x00520bf0));
    game_sprintf(L.buf, (const char*)0x004ecca0, G32(0x004ec9dc), G32(0x00521084));
    gxText_o(0xf, 0xf, L.buf, GU32(0x00521088));
    MatrixToQuat(&L.q, (const void*)0x00520c70);
    float a;
    if (!cam_acos_sin(&L.q.w, &a, &L.s)) {
        L.v.x = 0;
        L.v.y = 0;
        L.v.z = 0;
    } else {
        const double k = D(a) * 2.0 / D(L.s);
        L.v.x = (float)(D(L.q.x) * k);
        L.v.y = (float)(D(L.q.y) * k);
        L.v.z = (float)(k * D(L.q.z));
    }
    const double deg = FB(0x42652ee1);
    L.v.x = (float)(D(L.v.x) * deg);
    L.v.y = (float)(D(L.v.y) * deg);
    const double vz = D(L.v.z) * deg;
    L.v.z = st(vz);
    game_sprintf(L.buf, (const char*)0x004eccb4, D(GF(0x00520c94)), D(GF(0x00520c98)), D(GF(0x00520c9c)), D(L.v.x), D(L.v.y), vz);
    gxText_o(0xf, 0x1e, L.buf, GU32(0x00520ca0));
}
static void fp_dashboard(Footprint& f, gxCanvas* c) {
    FP_ADD(f, S_CANVAS, 4, "gfx.obj current canvas");
    fp_clip(f, c, "the dashboard canvas's pixels (clip)");
}
static void fp_camera_dashboard(Footprint& f, gxCanvas* c) {
    fp_dashboard(f, c);
    FP_ADD(f, 0x005d5600, 9, "the C runtime's transcendental dispatch scratch (_CIacos: ctrandisp2)");
}
PORT_FN(0x00428f60, "CameraDashboardDraw", CameraDashboardDraw_n, fp_camera_dashboard)

// ---- physdash.obj: the physics dashboard ----------------------------------------------------------------------------
typedef void(__cdecl* TireDash_t)(int32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t);   // Point2D and floats as bits
static const TireDash_t draw_tire_dash_o = (TireDash_t)0x00429be0;
static __forceinline double kelvin_to_f(float k) { return (D(k) - 272.0) * D(FB(0x3fe66666)) + 32.0; }

static void __cdecl PhysicsDashboardDraw_n(gxCanvas* c) {
    char buf[0x50];
    gxSetCanvas_o(c);
    gxRect_o(1, 1, 0xe6, 0x22, GU32(0x005214f8));
    gxText_o(10, 10, (const char*)0x004ecd24, GU32(0x00521508));
    gxText_o(0xa0, 10, (const char*)0x004ecd2c, G8(0x004ecce4) ? GU32(0x0052152c) : GU32(0x00521520));
    gxText_o(0xa0, 0x14, (const char*)0x004ecd34, G8(0x004ecce8) ? GU32(0x0052152c) : GU32(0x00521520));
    const uint8_t* info = (const uint8_t*)CarMgrGetInfo(WorldGetFocusCar());
    const uint8_t* e = *(const uint8_t* const*)(info + 0x134);
    const int32_t gear = *(const int32_t*)(e + 0x48);
    uint8_t g;
    if (gear > 0) g = (uint8_t)(gear + 0x30);
    else g = (uint8_t)(((uint32_t)gear < 1 ? 0xfc : 0) + 0x52);
    const float* ef = (const float*)e;
    const double t188 = kelvin_to_f(ef[0x188 / 4]);
    game_sprintf(buf, (const char*)0x004ecd3c, kelvin_to_f(ef[0x184 / 4]), t188);
    gxTextCentered_o(0x140, 0x64, buf, GU32(0x0052152c));
    game_sprintf(buf, (const char*)0x004ecd50, D(ef[0x3c / 4]), D(ef[0x38 / 4]), D(ef[0x40 / 4]), D(ef[0x44 / 4]));
    gxTextCentered_o(0x140, 0x78, buf, GU32(0x0052152c));
    const float* inf = (const float*)info;
    const double speed = x87_sqrt(D(inf[0x164 / 4]) * D(inf[0x164 / 4]) + D(inf[0x168 / 4]) * D(inf[0x168 / 4])) * D(FB(0x4051d608));
    game_sprintf(buf, (const char*)0x004ecd74, D(ef[0x4c / 4]) * 2.25, D(ef[0x60 / 4]) * 2.25, D(ef[0x34 / 4]), (int32_t)(int8_t)g, speed);
    gxTextCentered_o(0x140, 0x8c, buf, GU32(0x0052151c));
    const double slip = x87_sqrt(D(ef[0x58 / 4]) * D(ef[0x58 / 4]) + D(ef[0x5c / 4]) * D(ef[0x5c / 4]));
    game_sprintf(buf, (const char*)0x004ecda8, D(ef[0x58 / 4]), D(ef[0x5c / 4]), slip, D(ef[0x64 / 4]));
    gxTextCentered_o(0x140, 0x96, buf, GU32(0x0052151c));
    game_sprintf(buf, (const char*)0x004ecddc, D(GF(0x0055334c)), D(GF(0x00553350)), D(GF(0x00553354)));
    gxTextCentered_o(0x140, 0xa0, buf, GU32(0x0052151c));
    const uint8_t* p = e + 0xa8;
    for (int32_t i = 0; i < 4; i++, p += 0x38) {
        const uint32_t b = *(const uint32_t*)p, a = *(const uint32_t*)(p - 4);
        const uint32_t vy = *(const uint32_t*)(p - 8), vx = *(const uint32_t*)(p - 0xc);
        draw_tire_dash_o((i % 2) * 50 + 0x127, (i / 2) * 50 + 0xd7, vx, vy, a, b);
    }
}
PORT_FN(0x00429870, "PhysicsDashboardDraw", PhysicsDashboardDraw_n, fp_dashboard)

// draw_tire_dash: a tyre's load bar, a circle, its slip vector (clamped to 1.2, then drawn in the highlight colour)
// and its temperature
static void __cdecl draw_tire_dash_n(int32_t x, int32_t y, Point2Df v, float load, float temp) {
    char buf[0x50];
    const int32_t h = x87_ftol(D(load) * 40.0);
    if (h > 0) gxRect_o(x - 10, y - h + 0x14, x + 10, y + 0x14, GU32(0x005214fc));
    gxCircle_o(x, y, 0x14, GU32(0x005214f8));
    uint32_t col = GU32(0x005214f8);
    const double len = x87_sqrt(D(v.y) * D(v.y) + D(v.x) * D(v.x));
    const float lenf = st(len);
    if (len > D(FB(0x3f99999a))) {
        col = GU32(0x00521508);
        v.x = (float)(D(v.x) / D(lenf) * D(FB(0x3f99999a)));
        v.y = (float)(D(v.y) / D(lenf) * D(FB(0x3f99999a)));
    }
    const int32_t dy = x87_ftol(D(v.y) * -20.0);
    const int32_t dx = x87_ftol(D(v.x) * 20.0);
    gxLine_o(x, y, dx + x, dy + y, col);
    game_sprintf(buf, (const char*)0x004ecdf0, kelvin_to_f(temp));
    UIStyleDraw(0x11, x, y + 5, buf, 0);
}
static void fp_draw_tire_dash(Footprint& f, int32_t, int32_t, Point2Df, float, float) { fp_cur_clip(f); }
PORT_FN(0x00429be0, "draw_tire_dash", draw_tire_dash_n, fp_draw_tire_dash)

}  // namespace
