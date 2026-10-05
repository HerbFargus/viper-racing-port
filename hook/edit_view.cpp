// edit_view.cpp -- M3 UI stage, step U5 (group A): the model tool's two views and their maths, rewritten faithfully (library
// `edit`, modtool.obj; the dialog, the callbacks and the files are edit_tool.cpp).
//
//   ModelViewer (the 3D view, the static at 0x5cdbf8): the model turned by dragging (the zoom slider sets the distance), and
//   by mode: 1 vertices (a click picks the builder's vertex under the mouse, its marker drawn), 2 triangles (a click puts the
//   triangle into the surface's texture triangles, made from three texture points on its vertices -- Shift makes the points
//   -- or a second click updates; Delete / Backspace takes the picked triangle out), 3 edges (a click flips the edge's
//   smoothing; Ctrl smooths all, Ctrl+Shift facets all), 4 geometry (a click puts the triangle with texture points from the
//   texture transform set_transform made). TextureViewer (the texture view, the static at 0x57d7c8): the surface's texture,
//   its texture triangles and points, the capture overlay; the mouse adds a point (tool 0, on the picked vertex), moves one
//   (tool 1, locked to u or v, or snapped to the overlay), translates or rotates them all (tool 2, Ctrl rotates) or scales
//   them (tool 3); Backspace deletes the point, S / s sets the texture transform from the last three points (s: a planar
//   projection, S: the three points' own plane).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own and group B's
// ModBuilder* too, so a hooked rewrite is what runs), the compiler's inline code as it runs it. x87: a register value is a
// double, a stored one a float, the original's grouping and constants' widths, __ftol as x87_ftol, fsin / fcos straight to a
// float; floats the original moves, pushes or compares as integers are moved, passed and compared as their bits.
//
// Footprints (main thread). Shadow-checkable: the maths (MatrixSet x2, MatrixInverse, snap: pure), select_tp / is_selected,
// get_textured_point (its outputs), GetTextureU / V, the views' MouseUp, ModelViewer::MouseMove / set_position (the view and
// its frame), the views' Draw (the current canvas, the target canvas), TextureViewer::Callback / update_coords (the widget,
// the coordinates' text). replay_only: whatever reaches rebuild_models (it loads the surface's texture: gxGetTexture /
// gxForgetTexture), save_undo (ModBuilderCopy allocates), a box (modal), notifications or canvases (Create / Destroy) or
// the 3D renderer (Draw3D, the picks' mrSetView ...).
//
// FIX CANDIDATEs (left faithful, marked in place; none reached by ordinary editing): the coordinates' text ("%5.1f %5.1f"
// into 0x20 bytes) overruns for a texture coordinate of about 1e9 or more (a .mod / .3ds / .dxf with one, or a scaled one);
// capture_cb's overlays have no bound on a surface's points (0x1000) -- TextureViewer::MouseMove / Draw read them back.
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "edit_types.h"

namespace {
namespace edit_view {
using namespace edt;
using uit::tcall; using uit::ccall; using uit::crt_copy; using uit::CustomWidget; using uit::S_GX_CANVAS;

#define D(x) ((double)(x))
static const float k_inv256 = 0.00390625f;       // 0x4df8b0
static const float k_128 = 128.0f;               // 0x4df8b4
static const float k_one = 1.0f;                 // 0x4df8bc
static const float k_001 = 0.00999999977648258209228515625f;   // 0x4df8cc (0x3c23d70a)
static const float k_five = 5.0f;                // 0x4df8d0
static const float k_1_1 = 1.10000002384185791015625f;         // 0x4df8d8 (0x3f8ccccd)
static const float k_snap = 0.00250000017695128917694091796875f;  // 0x4df8e0 (0x3b23d70b)
static const float k_half = 0.5f;                // 0x4df8e4
static const float k_pi2 = 1.57079637050628662109375f;         // 0x4df8e8 (0x3fc90fdb)
static const float k_256 = 256.0f;               // 0x4df8ec
static const float k_tiny = 0.001000000047497451305389404296875f;  // 0x4df8f0 (0x3a83126f)

// ---- calls --------------------------------------------------------------------------------------------------------------------
typedef int(__cdecl* Sprintf2d_t)(char*, const char*, double, double);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef void(__cdecl* LogD_t)(const char*, double);
static __forceinline void* bld() { return UI_GP(void, S_BUILDER); }
static __forceinline uint8_t is_surface(int32_t s) { return ccall<uint8_t>(F_ModBuilderIsSurface, bld(), s); }
static __forceinline uint8_t get_tp(int32_t s, int32_t i, void* vtx, void* u, void* v) {
    return ccall<uint8_t>(F_ModBuilderGetTexturePoint, bld(), s, i, vtx, u, v);
}
static __forceinline uint8_t get_ttri(int32_t s, int32_t i, void* t, void* a, void* b, void* c) {
    return ccall<uint8_t>(F_ModBuilderGetTextureTriangle, bld(), s, i, t, a, b, c);
}
static __forceinline uint8_t get_vertex(int32_t i, void* p) { return ccall<uint8_t>(F_ModBuilderGetVertex, bld(), i, p); }
static __forceinline void move_tp(int32_t s, int32_t i, uint32_t u, uint32_t v) { ccall<void>(F_ModBuilderMoveTexturePoint, bld(), s, i, u, v); }
static __forceinline int32_t add_tp(int32_t s, int32_t vtx, uint32_t u, uint32_t v) { return ccall<int32_t>(F_ModBuilderAddTexturePoint, bld(), s, vtx, u, v); }
static __forceinline void cc_dirty(const volatile void* ctl) { tcall<void>(F_UICC_Dirty, (const void*)ctl); }
static __forceinline void save_undo() { ccall<void>(F_save_undo); }
static __forceinline void rebuild() { ccall<void>(F_rebuild_models); }
static __forceinline uint8_t scan_down(uint32_t k) { return ccall<uint8_t>(F_ScanDown, k); }
static __forceinline gxCanvas* set_canvas(const volatile void* c) { return ccall<gxCanvas*>(F_gxSetCanvas, (void*)c); }
static __forceinline void mat_concat(void* d, const void* a, const void* b) { ccall<void>(F_MatrixConcat, d, a, b); }
static __forceinline void mul_point(void* d, const void* p, const void* m) { ccall<void>(F_MatrixMulPoint, d, p, m); }
// x87 equality as the 1998 code tests it (fcomp; test ah, 0x40): equal, or unordered
static __forceinline bool eq_or_nan(double a, double b) { return !(a < b || a > b); }
// "%5.1f %5.1f" of a texture point into the coordinates' text
// FIX CANDIDATE: 0x20 bytes -- a coordinate of about 1e9 or more (a model file with one) runs over the zoom, the units and the
// strings after them (0x4fe938..). Reachable by an editor user only with such a file (or scaling points far out).
static __forceinline void coord_text(const volatile float* u, const volatile float* v) {
    const double a = D(*v) * D(k_128);                // fld v; fmul 128 -> the second %f
    const double b = D(*u) * D(k_128);                // fld u; fmul 128 -> the first
    ((Sprintf2d_t)(uintptr_t)F_sprintf)((char*)ED_P(S_COORD_TEXT), ED_CP(0x004fef20), b, a);    // "%5.1f %5.1f"
}
static __forceinline void coord_clear() { ccall<int>(F_sprintf, (char*)ED_P(S_COORD_TEXT), ED_CP(S_EMPTY)); }
// a texture point's pixel in the texture view: the scroll axis's total times the coordinate, less its position
static __forceinline int32_t tv_px(const volatile float* u) { return x87_ftol(D(UI_G32(S_HAXIS)) * D(*u) - D(UI_G32(S_HAXIS + 8))); }
static __forceinline int32_t tv_py(const volatile float* v) { return x87_ftol(D(UI_G32(S_VAXIS)) * D(*v) - D(UI_G32(S_VAXIS + 8))); }
// a frame set to the identity (the compiler's inline stores, in order)
static __forceinline void frame_identity(EdFrame* f) {
    volatile uint32_t* m = (volatile uint32_t*)f;
    m[0] = 0x3f800000u; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 0x3f800000u; m[5] = 0;
    m[6] = 0; m[7] = 0; m[8] = 0x3f800000u; m[9] = 0; m[10] = 0; m[11] = 0;
}

// ---- footprints ---------------------------------------------------------------------------------------------------------------
static __forceinline void fp_cur(Footprint& f) { UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas"); }
static __forceinline void fp_widget(Footprint& f, const volatile void* ctl) {
    if (void* w = ((const EdCtl*)ctl)->widget) f.add(w, sizeof(CustomWidget), "its widget");
}
static void fp_rebuild(Footprint& f) { f.replay_only = "edits the model: save_undo copies it (allocates), rebuild_models loads its texture"; }

// =================================================================================================================================
// TextureViewer: points, keys
// =================================================================================================================================
// TextureViewer::AddPoint: a texture point on the vertex at the mouse's place in the texture (its pixel / 256)
static int32_t __fastcall TV_AddPoint_n(TextureViewer* self, Edx, int32_t x, int32_t y, int32_t vtx) {
    const double a = D(ed_sub(x, self->x)) * D(k_inv256);
    const double b = D(ed_sub(y, self->y)) * D(k_inv256);
    volatile float v = (float)b, u = (float)a;          // fstp [esp+0x10]; fstp [esp+0xc] (the arguments)
    const int32_t surf = UI_G32(S_SURFACE);
    const int32_t r = add_tp(surf, vtx, ed_bits(&u), ed_bits(&v));
    UI_G8(S_MODIFIED) = 1;
    rebuild();
    return r;
}
static void fp_TV_AddPoint(Footprint& f, TextureViewer*, Edx, int32_t, int32_t, int32_t) { fp_rebuild(f); }
PORT_FN(0x004b0cc0, "TextureViewer::AddPoint", TV_AddPoint_n, fp_TV_AddPoint)

// TextureViewer::MouseDown: the mouse in texture coordinates; tool 0 in vertex mode adds a point on the picked vertex, the
// others pick the nearest point (within 10 pixels, by |dx| + |dy|) and show its coordinates
static void __fastcall TV_MouseDown_n(TextureViewer* self, Edx, int32_t x, int32_t y) {
    volatile int32_t vtx, vtx2, vtx3;                    // [esp+0x20] / [esp+0x10] / [esp+0x14]
    volatile float pu, pv, cu, cv;                       // [esp+0x1c] / [esp+0x18], [esp+0x28] / [esp+0x24]
    self->down = 1;
    if (!is_surface(UI_G32(S_SURFACE))) return;
    self->u = (float)(D(ed_add(ed_sub(UI_G32(S_HAXIS + 8), self->x), x)) / D(UI_G32(S_HAXIS)));
    self->v = (float)(D(ed_add(ed_sub(UI_G32(S_VAXIS + 8), self->y), y)) / D(UI_G32(S_VAXIS)));
    if (UI_G32(S_TV_TOOL) == 0) {
        if (UI_G32(S_MODE) == 1) {
            save_undo();
            UI_G32(S_TPOINT) = tcall<int32_t>(F_TV_AddPoint, self, x, y, UI_G32(S_VERTEX));
        }
    } else {
        int32_t best = 1000, i = 0;
        UI_G32(S_TPOINT) = -1;
        if (get_tp(UI_G32(S_SURFACE), i, (void*)&vtx, (void*)&pu, (void*)&pv)) {
            do {
                const int32_t py = x87_ftol(D(UI_G32(S_VAXIS)) * D(pv) - D(UI_G32(S_VAXIS + 8)));
                const int32_t dy = uit::iabs(ed_add(ed_sub(py, y), self->y));
                const int32_t px = x87_ftol(D(UI_G32(S_HAXIS)) * D(pu) - D(UI_G32(S_HAXIS + 8)));
                const int32_t d = ed_add(uit::iabs(ed_add(ed_sub(px, x), self->x)), dy);
                if (d < 10 && (UI_G32(S_TPOINT) < 0 || d < best)) {
                    best = d;
                    UI_G32(S_TPOINT) = i;
                    UI_G32(S_VERTEX) = vtx;
                    save_undo();
                    if (is_surface(UI_G32(S_SURFACE)) && get_tp(UI_G32(S_SURFACE), UI_G32(S_TPOINT), (void*)&vtx2, (void*)&cu, (void*)&cv))
                        coord_text(&cu, &cv);
                    else
                        coord_clear();
                }
                i++;
            } while (get_tp(UI_G32(S_SURFACE), i, (void*)&vtx, (void*)&pu, (void*)&pv));
        }
    }
    if (UI_G32(S_TPOINT) >= 0) {
        ccall<void>(F_select_tp, UI_G32(S_TPOINT));
        get_tp(UI_G32(S_SURFACE), UI_G32(S_TPOINT), (void*)&vtx3, (void*)&self->u, (void*)&self->v);
    }
    cc_dirty(self);
}
static void fp_TV_MouseDown(Footprint& f, TextureViewer*, Edx, int32_t, int32_t) { fp_rebuild(f); }
PORT_FN(0x004b0d30, "TextureViewer::MouseDown", TV_MouseDown_n, fp_TV_MouseDown)

// select_tp: the last three texture points selected (a new one shifts the others down)
static void __cdecl select_tp_n(int32_t tp) {
    if (UI_G32(S_SEL + 8) == tp) return;
    const int32_t a = UI_G32(S_SEL + 4), b = UI_G32(S_SEL + 8);
    UI_G32(S_SEL) = a;
    UI_G32(S_SEL + 4) = b;
    UI_G32(S_SEL + 8) = tp;
}
static void fp_select_tp(Footprint& f, int32_t) { UI_FP(f, S_SEL, 12, "the three texture points selected"); }
PORT_FN(0x004b0fd0, "select_tp", select_tp_n, fp_select_tp)

// TextureViewer::CharHit: Backspace deletes the picked point (unless a triangle uses it: a box says so), S / s set the texture
// transform from the last three points
static uint8_t __fastcall TV_CharHit_n(TextureViewer*, Edx, uint16_t key) {
    if (key == 8) {
        if (UI_G32(S_TPOINT) < 0) return 1;
        save_undo();
        if (ccall<uint8_t>(F_ModBuilderDeleteTexturePoint, bld(), UI_G32(S_SURFACE), UI_G32(S_TPOINT))) {
            UI_G32(S_TPOINT) = -1;
            UI_G8(S_MODIFIED) = 1;
            rebuild();
            return 1;
        }
        ccall<void>(F_UIDoOkBox, ED_CP(0x004fe964), ED_CP(0x004fe940));    // "Model Tool", "Another triangle uses this point"
        return 1;
    }
    if (key == 'S' || key == 's') {
        save_undo();
        ccall<void>(F_set_transform, (uint32_t)(key == 's' ? 1 : 0));
    }
    return 0;
}
static void fp_TV_CharHit(Footprint& f, TextureViewer*, Edx, uint16_t) { f.replay_only = "edits the model (save_undo, rebuild_models), or a box (modal)"; }
PORT_FN(0x004b1000, "TextureViewer::CharHit", TV_CharHit_n, fp_TV_CharHit)

// =================================================================================================================================
// set_transform and its maths
// =================================================================================================================================
// set_transform (vertex... geometry mode only): the texture transform that takes the last three texture points' vertices to
// their texture coordinates -- the three vertices' plane (and the texture's) inverted -- planar (s) snapped to the nearest
// axis and flattened; the three points moved to it
static void __cdecl set_transform_n(uint8_t planar) {
    float a[3], b[3], n[3], m[9], inv[9], uvm[9], t[3];  // E-0x128, E-0x134, E-0xcc, E-0x24, E-0x90, E-0x6c, E-0xec
    float r[12];                                         // E-0x110: the transform (a matrix, then the position at E-0xec)
    float out[3];                                        // E-0x11c
    float p[3][3];                                       // E-0x48: the vertices
    float v0[3], v1[3], v2[3];                           // E-0xd8, E-0xa8, E-0x9c
    float u0, w0, u1, w1, u2, w2;                        // E-0xe0 / E-0xdc, E-0xc0 / E-0xbc, E-0xb8 / E-0xb4
    if (UI_G32(S_MODE) != 4) return;
    for (int i = 0; i < 3; i++) tcall<void*>(P3DBase_ctor, p[i]);
    uint8_t ok = ccall<uint8_t>(F_get_textured_point, UI_G32(S_SEL), v0, &u0, &w0) & 1;
    ok &= ccall<uint8_t>(F_get_textured_point, UI_G32(S_SEL + 4), v1, &u1, &w1);
    if (!(ccall<uint8_t>(F_get_textured_point, UI_G32(S_SEL + 8), v2, &u2, &w2) & ok)) return;
    for (int k = 0; k < 3; k++) {
        ed_mv32(&p[0][k], &v0[k]);
        ed_mv32(&p[1][k], &v1[k]);
        ed_mv32(&p[2][k], &v2[k]);
    }
    for (int k = 0; k < 3; k++) a[k] = (float)ed_subm(D(v1[k]), &v0[k]);
    for (int k = 0; k < 3; k++) b[k] = (float)ed_subm(D(v2[k]), &v0[k]);
    n[0] = (float)(D(b[2]) * a[1] - D(b[1]) * a[2]);
    n[1] = (float)(D(b[0]) * a[2] - D(b[2]) * a[0]);
    n[2] = (float)(D(b[1]) * a[0] - D(b[0]) * a[1]);
    ccall<void>(F_MatrixSet3, m, a, b, n);
    ccall<void>(F_MatrixInverse, inv, m);
    {
        t[0] = (float)-D(v0[0]);
        t[1] = (float)-D(v0[1]);
        const double tz = -D(v0[2]);
        t[2] = (float)tz;
        const double y = D(tz) * inv[7] + D(t[1]) * inv[4] + D(t[0]) * inv[1];
        const double z = (D(t[1]) * inv[5] + D(tz) * inv[8]) + D(t[0]) * inv[2];
        const double x = (D(tz) * inv[6] + D(t[1]) * inv[3]) + D(t[0]) * inv[0];
        t[0] = (float)x;
        t[1] = (float)y;
        t[2] = (float)z;
    }
    {   // the texture coordinates' plane: (u1 - u0, v1 - v0, 0), (u2 - u0, v2 - v0, 0), (0, 0, 1)
        volatile uint32_t* z = (volatile uint32_t*)uvm;
        uvm[0] = (float)ed_subm(D(u1), &u0);
        uvm[1] = (float)ed_subm(D(w1), &w0);
        z[2] = 0; z[5] = 0; z[6] = 0; z[7] = 0; z[8] = 0x3f800000u;
        uvm[3] = (float)ed_subm(D(u2), &u0);
        uvm[4] = (float)ed_subm(D(w2), &w0);
    }
    mat_concat(r, inv, uvm);
    ed_mv32(&out[0], &u0);
    ed_mv32(&out[1], &w0);
    mul_point(t, t, uvm);
    t[0] = (float)(D(out[0]) + t[0]);
    t[1] = (float)(D(out[1]) + t[1]);
    memcpy(&r[9], t, 12);                                // (t is the frame's position: the same memory in the original)
    if (planar) {
        volatile uint32_t* q = (volatile uint32_t*)r;
        const uint32_t m1 = q[1], m3 = q[3], m2 = q[2], m5 = q[5];
        q[1] = m3; q[3] = m1;
        const uint32_t m6 = q[6], m7 = q[7];
        q[2] = m6; q[6] = m2;
        q[5] = m7; q[7] = m5;
        ccall<void>(F_snap, &r[0]);
        ccall<void>(F_snap, &r[3]);
        const uint32_t n1 = q[1], n3 = q[3], n2 = q[2];
        q[8] = 0;
        q[1] = n3; q[3] = n1;
        const uint32_t n5 = q[5];
        q[2] = 0; q[6] = n2; q[5] = 0; q[7] = n5;
    }
    crt_copy(ED_P(S_XFORM), r, 0x30);
    UI_G8(S_XFORM_SET) = 1;
    for (int k = 0; k < 3; k++) {
        mul_point(out, p[k], r);
        out[0] = (float)(D(out[0]) + r[9]);
        out[1] = (float)(D(out[1]) + r[10]);
        out[2] = (float)(D(out[2]) + r[11]);
        move_tp(UI_G32(S_SURFACE), UI_G32(S_SEL + 4 * (uint32_t)k), ed_bits(&out[0]), ed_bits(&out[1]));
    }
    UI_G8(S_MODIFIED) = 1;
    rebuild();
}
static void fp_set_transform(Footprint& f, uint8_t) { fp_rebuild(f); }
PORT_FN(0x004b1190, "set_transform", set_transform_n, fp_set_transform)

// get_textured_point: a texture point's coordinates and its vertex's position
static uint8_t __cdecl get_textured_point_n(int32_t tp, float* p, float* u, float* v) {
    int32_t vtx;
    vtx = -1;
    if (get_tp(UI_G32(S_SURFACE), tp, &vtx, u, v) && get_vertex(vtx, p)) return 1;
    return 0;
}
static void fp_get_textured_point(Footprint& f, int32_t, float* p, float* u, float* v) {
    f.add(p, 12, "the vertex");
    f.add(u, 4, "u");
    f.add(v, 4, "v");
}
PORT_FN(0x004b1610, "get_textured_point", get_textured_point_n, fp_get_textured_point)

// MatrixSet(rows): three vectors as the matrix's rows (bit copies)
static void __cdecl MatrixSet3_n(float* m, const float* a, const float* b, const float* c) {
    for (int k = 0; k < 3; k++) ed_mv32(&m[k], &a[k]);
    for (int k = 0; k < 3; k++) ed_mv32(&m[3 + k], &b[k]);
    for (int k = 0; k < 3; k++) ed_mv32(&m[6 + k], &c[k]);
}
static void fp_MatrixSet3(Footprint& f, float* m, const float*, const float*, const float*) { f.add(m, 36, "the matrix"); }
PORT_FN(0x004b1680, "MatrixSet(rows)(modtool.obj)", MatrixSet3_n, fp_MatrixSet3)

// MatrixInverse: by cofactors over the determinant (the determinant and three cofactors kept in registers, as the original)
static void __cdecl MatrixInverse_n(float* dst, const float* s) {
    // a0..a4 loaded (fld: a signalling NaN comes out quiet), held as doubles read once; a5..a8 copied (mov) and read either
    // way: loaded the same, or as the memory operands of five products (ed_mulm), as the original has them
    const volatile float* const sv = s;
    const double a0 = sv[0], a1 = sv[1], a2 = sv[2], a3 = sv[3], a4 = sv[4];
    float m[4];                                          // [esp+0xc] .. [esp]: a5..a8
    ed_mv32(&m[0], &s[5]);
    ed_mv32(&m[1], &s[6]);
    ed_mv32(&m[2], &s[7]);
    ed_mv32(&m[3], &s[8]);
    const volatile float* const mv = m;
    const float* const a5 = &m[0], * const a6 = &m[1], * const a7 = &m[2], * const a8 = &m[3];
    const double l5 = mv[0], l6 = mv[1], l7 = mv[2], l8 = mv[3];   // their loaded (quiet) values
    float r[9];
    const double c0 = l5 * a1 - a4 * a2;
    const float c0f = (float)c0;
    const double c1 = a3 * a2 - l5 * a0;
    const float c1f = (float)c1;
    const double c2 = a4 * a0 - a3 * a1;
    const float c2f = (float)c2;
    const double det = (ed_mulm(c2, a8) + l7 * c1f) + l6 * c0f;
    r[0] = (float)((l8 * a4 - ed_mulm(l7, a5)) / det);
    r[1] = (float)((l7 * a2 - l8 * a1) / det);
    r[2] = (float)(D(c0f) / det);
    r[3] = (float)((ed_mulm(l6, a5) - l8 * a3) / det);
    r[4] = (float)((l8 * a0 - l6 * a2) / det);
    r[5] = (float)(D(c1f) / det);
    r[6] = (float)((ed_mulm(a3, a7) - a4 * l6) / det);
    r[7] = (float)((ed_mulm(a1, a6) - l7 * a0) / det);
    r[8] = (float)(D(c2f) / det);
    volatile float dead = (float)det;                    // fstp [esp+0x18]: the determinant stored, never read
    (void)dead;
    ccall<void>(F_MatrixSetF, dst, r);
}
static void fp_MatrixInverse(Footprint& f, float* d, const float*) { f.add(d, 36, "the matrix"); }
PORT_FN(0x004b16d0, "MatrixInverse(modtool.obj)", MatrixInverse_n, fp_MatrixInverse)

// MatrixSet(floats): nine floats (rep movsd)
static void __cdecl MatrixSetF_n(float* m, const float* f9) { crt_copy(m, f9, 36); }
static void fp_MatrixSetF(Footprint& f, float* m, const float*) { f.add(m, 36, "the matrix"); }
PORT_FN(0x004b18c0, "MatrixSet(floats)(modtool.obj)", MatrixSetF_n, fp_MatrixSetF)

// snap: a vector reduced to its largest component (the others zeroed)
static void __cdecl snap_n(float* p) {
    volatile float ay = (float)fabs(D(p[1]));            // fstp [esp]
    const double ax = fabs(D(p[0]));
    volatile float axf = (float)ax;                      // fstp [esp+4]
    const double az = fabs(D(p[2]));
    volatile uint32_t* q = (volatile uint32_t*)p;
    if (ax > D(ay)) {                                    // (fcom; test ah, 0x41)
        if (!(az >= D(axf))) { q[1] = 0; q[2] = 0; }
        else { q[1] = 0; q[0] = 0; }
    } else {
        const bool less = !(az >= D(ay));
        q[0] = 0;
        if (less) q[2] = 0;
        else q[1] = 0;
    }
}
static void fp_snap(Footprint& f, float* p) { f.add(p, 12, "the vector"); }
PORT_FN(0x004b18e0, "snap", snap_n, fp_snap)

// is_selected: one of the last three texture points selected
static uint8_t __cdecl is_selected_n(int32_t tp) {
    return (UI_G32(S_SEL) == tp || UI_G32(S_SEL + 4) == tp || UI_G32(S_SEL + 8) == tp) ? 1 : 0;
}
static void fp_is_selected(Footprint&, int32_t) {}
PORT_FN(0x004b19b0, "is_selected", is_selected_n, fp_is_selected)

// =================================================================================================================================
// ModelViewer
// =================================================================================================================================
static void __fastcall MV_Create_n(ModelViewer* self, Edx) { tcall<void>(F_UICC_AddNotification, self, (int32_t)0, ED_P(S_MODE), (uint32_t)4); }
static void fp_MV_Create(Footprint& f, ModelViewer*, Edx) { f.replay_only = "adds a notification (allocates)"; }
PORT_FN(0x004b4a70, "ModelViewer::Create", MV_Create_n, fp_MV_Create)

static void __fastcall MV_Callback_n(ModelViewer*, Edx, int32_t, const void*) { rebuild(); }
static void fp_MV_Callback(Footprint& f, ModelViewer*, Edx, int32_t, const void*) { f.replay_only = "rebuild_models loads the surface's texture"; }
PORT_FN(0x004b4a80, "ModelViewer::Callback", MV_Callback_n, fp_MV_Callback)

// the view's 3D set-up (MouseDown, Draw3D): its rectangle, the clip distances, the projection, the camera
static __forceinline void mv_setup(ModelViewer* self) {
    const int32_t h = self->h, w = self->w, y = self->y, x = self->x;
    ccall<void>(F_mrSetView, x, y, w, h, (uint32_t)0);
    ccall<void>(F_mrModelSetClipDist, (uint32_t)0, (uint32_t)0x42480000u);                         // 0, 50
    ccall<void>(F_mrSetProjection, (uint32_t)0x3dcccccdu, (uint32_t)0x42480000u, (uint32_t)0x3f9c61aau);   // 0.1, 50, 1.2217
    ccall<void>(F_mrSetCamera, (void*)self->camera);
}

// ModelViewer::MouseDown: a drag starts; in modes 1..4 (without Ctrl) the vertex / triangle / edge under the mouse is picked
// and the mode's edit made
static void __fastcall MV_MouseDown_n(ModelViewer* self, Edx, int32_t x, int32_t y) {
    struct {                                             // the frame's locals the edits use
        float v[3];                                      // E-0x9c the picked vertex's position / the 3-point vertices (p[3])
        float p1[3], p2[3];                              // E-0x90, E-0x84
        float inv[9];                                    // E-0x78 (mode 2: u at E-0x78)
        float m[9];                                      // E-0x54 (mode 1, 2, 3: a vertex, a triangle)
        float tri[3];                                    // E-0x30 (mode 2: the texture triangle's triangle; mode 4: the triangle)
    } L;
    volatile int32_t ta, tb, tc, pvtx;                   // E-0xf0, E-0xcc, E-0xd8; E-0xb4
    int32_t* const tri4 = (int32_t*)L.tri;
    self->mx = x;
    self->drag = 1;
    self->my = y;
    if (UI_G32(S_MODE) == 0 || UI_G32(S_MODE) == 5) return;
    if (scan_down(0xfe)) return;
    mv_setup(self);
    UI_G32(S_VERTEX) = -1;
    if (!ccall<uint8_t>(F_mrModelPickVertex, ED_P(S_INFO2), (const void*)self->frame, x, y, ED_P(S_TRIANGLE), ED_P(S_VERTEX))) {
        UI_G32(S_TRIANGLE) = ccall<int32_t>(F_mrModelPick, ED_P(S_INFO2), (const void*)self->frame, x, y);
    } else {
        const uint8_t* vt = ((EdInfo*)ED_P(S_INFO2))->verts + (uint32_t)UI_G32(S_VERTEX) * 32u;
        const uint32_t vx = ed_bits(vt), vy = ed_bits(vt + 4), vz = ed_bits(vt + 8);
        UI_G32(S_VERTEX) = -1;
        ((volatile uint32_t*)L.v)[0] = vx;
        ((volatile uint32_t*)L.v)[1] = vy;
        ((volatile uint32_t*)L.v)[2] = vz;
        for (int32_t i = 0; get_vertex(i, L.m); i++) {
            if (eq_or_nan(L.v[0], L.m[0]) && eq_or_nan(L.v[1], L.m[1]) && eq_or_nan(L.v[2], L.m[2])) {
                UI_G32(S_VERTEX) = i;
                break;
            }
        }
    }
    const int32_t mode = UI_G32(S_MODE);
    if (mode == 2) {
        // the triangle into the surface's texture triangles (again: an update), from three texture points on its vertices
        int32_t* const t = (int32_t*)L.m;
        if (!ccall<uint8_t>(F_ModBuilderGetTriangle, bld(), UI_G32(S_TRIANGLE), (void*)L.m)) {
            ((Log_t)(uintptr_t)F_LogReport)(ED_CP(0x004fef58));                    // "Can't find triangle"
            return;
        }
        if (!is_surface(UI_G32(S_SURFACE))) return;
        for (int32_t i = 0; get_ttri(UI_G32(S_SURFACE), i, tri4, (void*)&ta, (void*)&tb, (void*)&tc); i++) {
            if (tri4[0] == UI_G32(S_TRIANGLE)) {
                rebuild();
                return;
            }
        }
        save_undo();
        ta = -1;
        tb = -1;
        tc = -1;
        for (int32_t i = 0; get_tp(UI_G32(S_SURFACE), i, (void*)&pvtx, L.inv, L.v); i++) {
            if (t[0] == pvtx && (ta == -1 || !ccall<uint8_t>(F_is_selected, (int32_t)ta))) ta = i;
            if (t[1] == pvtx && (tb == -1 || !ccall<uint8_t>(F_is_selected, (int32_t)tb))) tb = i;
            if (t[2] == pvtx && (tc == -1 || !ccall<uint8_t>(F_is_selected, (int32_t)tc))) tc = i;
        }
        if (!scan_down(0xfd)) {
            if (ta < 0 || tb < 0 || tc < 0) {
                ccall<void>(F_UIDoOkBox, ED_CP(0x004fef2c), ED_CP(0x004fef38));   // "Model Tool", "You need three texture points."
                goto update;
            }
        } else {
            ta = add_tp(UI_G32(S_SURFACE), t[0], 0u, 0u);
            tb = add_tp(UI_G32(S_SURFACE), t[1], 0x3f800000u, 0u);
            tc = add_tp(UI_G32(S_SURFACE), t[2], 0u, 0x3f800000u);
        }
        {
            const int32_t c = tc, b = tb, a = ta;
            ccall<int32_t>(F_ModBuilderAddTextureTriangle, bld(), UI_G32(S_SURFACE), UI_G32(S_TRIANGLE), a, b, c);
        }
        UI_G8(S_MODIFIED) = 1;
        goto rebuild_update;
    }
    if (mode == 3) {
        // an edge's smoothing flipped (Ctrl: all smoothed, Ctrl+Shift: all faceted)
        volatile int32_t edge;
        uint8_t* const fl = (uint8_t*)&L.m[3];          // the triangle's edge flags (E-0x48)
        edge = -1;
        if (scan_down(0xff)) {
            save_undo();
            const uint8_t facet = scan_down(0xfd);
            void* const b = bld();
            if (facet) {
                ccall<void>(F_ModBuilderFacetAll, b);
                ((Log_t)(uintptr_t)F_LogReport)(ED_CP(0x004fefa4));                // "FacetAll"
            } else {
                ccall<void>(F_ModBuilderSmoothAll, b);
                ((Log_t)(uintptr_t)F_LogReport)(ED_CP(0x004fef98));                // "SmoothAll"
            }
            goto rebuild_update;
        }
        if (!ccall<uint8_t>(F_mrModelPickEdge, ED_P(S_INFO2), (const void*)self->frame, x, y, ED_P(S_TRIANGLE), (void*)&edge)) goto update;
        if (!ccall<uint8_t>(F_ModBuilderGetTriangle, bld(), UI_G32(S_TRIANGLE), (void*)L.m)) goto update;
        save_undo();
        switch (edge) {
        case 0: fl[0] ^= 1; break;
        case 1: fl[1] ^= 1; break;
        case 2: fl[2] ^= 1; break;
        default: ((Log_t)(uintptr_t)F_LogPanic)(ED_CP(0x004fef84), (int32_t)edge); break;     // "Unknown edge: %d"
        }
        ((Log_t)(uintptr_t)F_LogReport)(ED_CP(0x004fef6c), UI_G32(S_TRIANGLE), (int32_t)edge);   // "Flipping edge: %d/%d"
        ccall<void>(F_ModBuilderSetTriangle, bld(), UI_G32(S_TRIANGLE), (const void*)L.m);
        goto rebuild_update;
    }
    if (mode == 4) {
        // the triangle put with texture points where the texture transform takes its vertices
        float q0[3], q1[3], q2[3];                       // E-0xc0, E-0xb4, E-0xd8
        float u0, v0, u1, v1, u2, v2;                    // E-0x8 / E-0x4, E-0x10 / E-0xc, E-0x18 / E-0x14
        float a[3], b[3], n[3], d[3], r[3], tq[3];       // E-0xfc, E-0x108, E-0xa8, E-0x114, E-0xf0, E-0xe4
        int32_t pt[3];                                   // E-0xcc
        float* const p[3] = {L.v, L.p1, L.p2};
        if (!ccall<uint8_t>(F_ModBuilderGetTriangle, bld(), UI_G32(S_TRIANGLE), (void*)L.tri)) goto update;
        for (int k = 0; k < 3; k++) tcall<void*>(P3DBase_ctor, p[k]);
        uint8_t ok = get_vertex(tri4[0], L.v) & 1;
        ok &= get_vertex(tri4[1], L.p1);
        ok &= get_vertex(tri4[2], L.p2);
        ok &= ccall<uint8_t>(F_get_textured_point, UI_G32(S_SEL), q0, &u0, &v0);
        ok &= ccall<uint8_t>(F_get_textured_point, UI_G32(S_SEL + 4), q1, &u1, &v1);
        ok &= ccall<uint8_t>(F_get_textured_point, UI_G32(S_SEL + 8), q2, &u2, &v2);
        if (UI_G8(S_XFORM_SET) == 0) ok = 0;
        if (!ok) goto update;
        save_undo();
        for (int k = 0; k < 3; k++) a[k] = (float)ed_subm(D(q1[k]), &q0[k]);
        for (int k = 0; k < 3; k++) b[k] = (float)ed_subm(D(q2[k]), &q0[k]);
        n[0] = (float)(D(b[2]) * a[1] - D(b[1]) * a[2]);
        n[1] = (float)(D(b[0]) * a[2] - D(b[2]) * a[0]);
        n[2] = (float)(D(b[1]) * a[0] - D(b[0]) * a[1]);
        ccall<void>(F_MatrixSet3, L.m, a, b, n);
        ccall<void>(F_MatrixInverse, L.inv, L.m);
        const volatile float* X = (const volatile float*)ED_P(S_XFORM);
        for (int k = 0; k < 3; k++) {
            const float* pk = p[k];
            for (int j = 0; j < 3; j++) d[j] = (float)ed_subm(D(pk[j]), &q0[j]);
            volatile float* rv = r;                      // (the plane coordinates: stored, never read)
            rv[0] = (float)((D(d[2]) * L.inv[6] + D(d[1]) * L.inv[3]) + D(d[0]) * L.inv[0]);
            rv[1] = (float)((D(d[2]) * L.inv[7] + D(d[1]) * L.inv[4]) + D(d[0]) * L.inv[1]);
            rv[2] = (float)((D(d[1]) * L.inv[5] + D(d[2]) * L.inv[8]) + D(d[0]) * L.inv[2]);
            tq[0] = (float)((ed_mulm(D(pk[2]), &X[6]) + ed_mulm(D(pk[1]), &X[3])) + ed_mulm(D(pk[0]), &X[0]));
            tq[1] = (float)((ed_mulm(D(pk[2]), &X[7]) + ed_mulm(D(pk[1]), &X[4])) + ed_mulm(D(pk[0]), &X[1]));
            tq[2] = (float)((ed_mulm(D(pk[1]), &X[5]) + ed_mulm(D(pk[2]), &X[8])) + ed_mulm(D(pk[0]), &X[2]));
            tq[0] = (float)(D(X[9]) + tq[0]);
            tq[1] = (float)(D(X[10]) + tq[1]);
            tq[2] = (float)(D(X[11]) + tq[2]);
            pt[k] = add_tp(UI_G32(S_SURFACE), tri4[k], ed_bits(&tq[0]), ed_bits(&tq[1]));
        }
        ccall<int32_t>(F_ModBuilderAddTextureTriangle, bld(), UI_G32(S_SURFACE), UI_G32(S_TRIANGLE), pt[0], pt[1], pt[2]);
        UI_G8(S_MODIFIED) = 1;
        goto rebuild_update;
    }
    goto update;
rebuild_update:
    rebuild();
update:
    ccall<void>(F_update_model);
    ccall<void>(F_update_texture_viewer);
}
static void fp_MV_MouseDown(Footprint& f, ModelViewer*, Edx, int32_t, int32_t) { f.replay_only = "picks through the 3D renderer, edits the model (save_undo, rebuild_models), a box (modal)"; }
PORT_FN(0x004b4a90, "ModelViewer::MouseDown", MV_MouseDown_n, fp_MV_MouseDown)

static void __fastcall MV_MouseUp_n(ModelViewer* self, Edx, int32_t, int32_t) { self->drag = 0; }
static void fp_MV_MouseUp(Footprint& f, ModelViewer* self, Edx, int32_t, int32_t) { f.add(self, sizeof *self, "this"); }
PORT_FN(0x004b5410, "ModelViewer::MouseUp", MV_MouseUp_n, fp_MV_MouseUp)

// ModelViewer::MouseMove: a drag turns the model (in modes 1..4 only with Ctrl held)
static void __fastcall MV_MouseMove_n(ModelViewer* self, Edx, int32_t x, int32_t y) {
    if (self->drag == 0) return;
    if (UI_G32(S_MODE) != 0 && UI_G32(S_MODE) != 5 && !scan_down(0xfe)) {
        self->mx = x;
        self->my = y;
        return;
    }
    const int32_t dx = ed_sub(self->mx, x);
    self->yaw = (float)(D(dx) * D(k_001) + D(self->yaw));
    const int32_t dy = ed_sub(self->my, y);
    self->pitch = (float)(D(dy) * D(k_001) + D(self->pitch));
    EdFrame* F = self->frame;
    frame_identity(F);
    if ((int32_t)self->dist_b < 0x3c23d70a) self->dist_b = 0x3c23d70au;
    F->p[2] = (float)(D(k_five) / D(self->dist) + D(self->frame->p[2]));
    {
        float ma[9], mb[9];
        EdFrame* G = self->frame;
        const uint32_t pitch = self->pitch_b;
        ccall<void>(F_MatrixMakeYaw, ma, 0u);
        ccall<void>(F_MatrixMakePitch, mb, pitch);
        mat_concat(ma, mb, ma);
        ccall<void>(F_MatrixMakeRoll, mb, 0u);
        mat_concat(ma, mb, ma);
        mat_concat(G, ma, G);
        const uint32_t yaw = self->yaw_b;
        EdFrame* H = self->frame;
        ccall<void>(F_MatrixMakeModelRotation, mb, yaw, 0u, 0u);
        mat_concat(H, mb, H);
    }
    self->mx = x;
    self->my = y;
}
static void fp_MV_MouseMove(Footprint& f, ModelViewer* self, Edx, int32_t, int32_t) {
    f.add(self, sizeof *self, "this");
    if (EdFrame* F = self->frame) f.add(F, sizeof *F, "the model's frame");
}
PORT_FN(0x004b5420, "ModelViewer::MouseMove", MV_MouseMove_n, fp_MV_MouseMove)

// ModelViewer::CharHit: Delete / Backspace takes the picked triangle out of the surface's texture triangles (triangle mode)
static uint8_t __fastcall MV_CharHit_n(ModelViewer*, Edx, uint16_t key) {
    volatile int32_t t, a, b, c;                         // E-0x10, E-0x4, E-0x8, E-0xc
    if (key != 8 && key != 0x12e) return 0;
    if (UI_G32(S_MODE) == 2 && is_surface(UI_G32(S_SURFACE)) && UI_G32(S_TRIANGLE) >= 0) {
        int32_t i = 0;
        save_undo();
        if (get_ttri(UI_G32(S_SURFACE), i, (void*)&t, (void*)&a, (void*)&b, (void*)&c)) {
            do {
                if (t == UI_G32(S_TRIANGLE)) {
                    ccall<void>(F_ModBuilderDeleteTextureTriangle, bld(), UI_G32(S_SURFACE), i);
                    UI_G8(S_MODIFIED) = 1;
                }
                i++;
            } while (get_ttri(UI_G32(S_SURFACE), i, (void*)&t, (void*)&a, (void*)&b, (void*)&c));
        }
        UI_G32(S_TRIANGLE) = -1;
        rebuild();
    }
    return 1;
}
static void fp_MV_CharHit(Footprint& f, ModelViewer*, Edx, uint16_t) { fp_rebuild(f); }
PORT_FN(0x004b55c0, "ModelViewer::CharHit", MV_CharHit_n, fp_MV_CharHit)

// ModelViewer::Draw: the background stamp
static void __fastcall MV_Draw_n(ModelViewer* self, Edx, gxCanvas* c) {
    set_canvas(c);
    const int32_t y = self->y, x = self->x;
    ccall<void>(F_gxDrawStamp, (const void*)self->back, x, y, (int32_t)0, (const void*)0);
}
static void fp_MV_Draw(Footprint& f, ModelViewer*, Edx, gxCanvas* c) {
    fp_cur(f);
    UI_FP_CANVAS(f, c);
}
PORT_FN(0x004b56d0, "ModelViewer::Draw", MV_Draw_n, fp_MV_Draw)

// the marker's place: q, in the model's frame, pulled to 1.1 in front of the camera (if it isn't within 0.01 of its plane)
static __forceinline void marker_at(ModelViewer* self, float* q) {
    float rot[9];
    ccall<void>(F_MatrixMakeModelRotation, rot, (uint32_t)0x3e32b8c2u, 0u, 0u);             // 10 degrees
    mat_concat((void*)&self->marker, rot, (const void*)&self->marker);
    const EdFrame* F = self->frame;
    mul_point(q, q, (const void*)F);
    q[0] = (float)(D(F->p[0]) + q[0]);
    q[1] = (float)(D(F->p[1]) + q[1]);
    q[2] = (float)(D(F->p[2]) + q[2]);
    const EdFrame* C = self->camera;
    q[0] = (float)ed_subm(D(q[0]), &C->p[0]);
    q[1] = (float)ed_subm(D(q[1]), &C->p[1]);
    q[2] = (float)ed_subm(D(q[2]), &C->p[2]);
    ccall<void>(F_MatrixMulPointInv, q, q, (const void*)C);
    if (!!(fabs(D(q[2])) >= D(k_001))) {
        const double s = D(k_1_1) / D(q[2]);
        q[0] = (float)(D(q[0]) * s);
        q[1] = (float)(D(q[1]) * s);
        q[2] = (float)(s * D(q[2]));
    }
    const EdFrame* E = self->camera;
    mul_point(q, q, (const void*)E);
    q[0] = (float)(D(E->p[0]) + q[0]);
    q[1] = (float)(D(E->p[1]) + q[1]);
    q[2] = (float)(D(E->p[2]) + q[2]);
    const uint32_t qx = ed_bits(&q[0]), qy = ed_bits(&q[1]), qz = ed_bits(&q[2]);
    ((volatile uint32_t*)&self->marker.p)[0] = qx;
    ((volatile uint32_t*)&self->marker.p)[1] = qy;
    ((volatile uint32_t*)&self->marker.p)[2] = qz;
    ccall<void>(F_mrModelDraw, self->point, (const void*)&self->marker);
}

// ModelViewer::Draw3D: the model (or, in capture mode, the captured model alone) and the vertex marker
static void __fastcall MV_Draw3D_n(ModelViewer* self, Edx) {
    float p[3], q[3];                                    // E-0x3c, E-0x30
    mv_setup(self);
    if (UI_G32(S_MODE) == 5) {
        if (const int32_t m = self->capture) ccall<void>(F_mrModelDraw, m, (const void*)self->frame);
        return;
    }
    ccall<void>(F_mrModelDraw, self->model, (const void*)self->frame);
    if (UI_G32(S_MODE) == 1 && UI_G32(S_VERTEX) >= 0) {
        if (!get_vertex(UI_G32(S_VERTEX), p)) return;
        for (int k = 0; k < 3; k++) ed_mv32(&q[k], &p[k]);
        marker_at(self, q);
        return;
    }
    {
        volatile uint32_t z[3] = {0, 0, 0};              // E-0x24, copied
        for (int k = 0; k < 3; k++) ed_mv32(&p[k], &z[k]);
    }
    marker_at(self, p);
}
static void fp_MV_Draw3D(Footprint& f, ModelViewer*, Edx) { f.replay_only = "draws through the 3D renderer (its state, lists, DirectX)"; }
PORT_FN(0x004b5700, "ModelViewer::Draw3D", MV_Draw3D_n, fp_MV_Draw3D)

// ModelViewer::set_position: the view's angles and distance, the model's frame made from them
static void __fastcall MV_set_position_n(ModelViewer* self, Edx, uint32_t yaw, uint32_t pitch, uint32_t dist) {
    self->yaw_b = yaw;
    self->pitch_b = pitch;
    EdFrame* F = self->frame;
    self->dist_b = dist;
    frame_identity(F);
    if ((int32_t)self->dist_b < 0x3c23d70a) self->dist_b = 0x3c23d70au;
    self->frame->p[2] = (float)(D(k_five) / D(self->dist) + D(self->frame->p[2]));
    float ma[9], mb[9];
    EdFrame* G = self->frame;
    const uint32_t pb = self->pitch_b;
    ccall<void>(F_MatrixMakeYaw, ma, 0u);
    ccall<void>(F_MatrixMakePitch, mb, pb);
    mat_concat(ma, mb, ma);
    ccall<void>(F_MatrixMakeRoll, mb, 0u);
    mat_concat(ma, mb, ma);
    mat_concat(G, ma, G);
    EdFrame* H = self->frame;
    ccall<void>(F_MatrixMakeYaw, ma, (uint32_t)self->yaw_b);
    ccall<void>(F_MatrixMakePitch, mb, 0u);
    mat_concat(ma, mb, ma);
    ccall<void>(F_MatrixMakeRoll, mb, 0u);
    mat_concat(ma, mb, ma);
    mat_concat(H, ma, H);
}
static void fp_MV_set_position(Footprint& f, ModelViewer* self, Edx, uint32_t, uint32_t, uint32_t) {
    f.add(self, sizeof *self, "this");
    if (EdFrame* F = self->frame) f.add(F, sizeof *F, "the model's frame");
}
PORT_FN(0x004b6cc0, "ModelViewer::set_position", MV_set_position_n, fp_MV_set_position)

// =================================================================================================================================
// TextureViewer
// =================================================================================================================================
// TextureViewer::Create: its canvas (the item's size, cleared), the cursor; the scroll axes for zoom 1 around their middle
static void __fastcall TV_Create_n(TextureViewer* self, Edx) {
    volatile float bf;                                   // E-0x4
    self->u_b = 0;
    self->v_b = 0;
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, ED_P(S_VAXIS), (uint32_t)0xc);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)1, ED_P(S_HAXIS), (uint32_t)0xc);
    {
        const int32_t h = self->h, w = self->w;
        ccall<void>(F_gxAllocCanvas, (void*)&self->canvas, w, h);
    }
    set_canvas(&self->canvas);
    ccall<void>(F_gxClear, UI_GU32(S_BLACK));
    self->down = 0;
    self->noverlays = 0;
    self->overlay = 0;
    self->cursor = ccall<void*>(F_gxGetStamp, ED_CP(0x004fefb0));                          // "cross.stp"
    UI_G32(S_HAXIS) = 0x100;
    UI_G32(S_VAXIS) = 0x100;
    UI_G32(S_HAXIS + 4) = 0x100;
    UI_G32(S_VAXIS + 4) = 0x100;
    const double a = D(ed_add(UI_G32(S_HAXIS + 8), 0x80)) * D(k_inv256);
    self->zoom_b = 0x3f800000u;
    bf = (float)(D(ed_add(UI_G32(S_VAXIS + 8), 0x80)) * D(k_inv256));
    UI_G32(S_HAXIS) = x87_ftol(D(self->w));
    const int32_t vt = x87_ftol(D(self->h));
    const double ah = a * D(UI_G32(S_HAXIS));
    UI_G32(S_VAXIS) = vt;
    UI_G32(S_HAXIS + 4) = self->w;
    UI_G32(S_VAXIS + 4) = self->h;
    {
        const int32_t p = ed_sub(x87_ftol(ah), UI_G32(S_HAXIS + 4) / 2);
        UI_G32(S_HAXIS + 8) = p;
        const int32_t m = ed_sub(UI_G32(S_HAXIS), UI_G32(S_HAXIS + 4));
        if (p > m) UI_G32(S_HAXIS + 8) = m;
        if (UI_G32(S_HAXIS + 8) < 0) UI_G32(S_HAXIS + 8) = 0;
    }
    {
        const int32_t p = ed_sub(x87_ftol(D(UI_G32(S_VAXIS)) * D(bf)), UI_G32(S_VAXIS + 4) / 2);
        UI_G32(S_VAXIS + 8) = p;
        const int32_t m = ed_sub(UI_G32(S_VAXIS), UI_G32(S_VAXIS + 4));
        if (p > m) UI_G32(S_VAXIS + 8) = m;
        if (UI_G32(S_VAXIS + 8) < 0) UI_G32(S_VAXIS + 8) = 0;
    }
}
static void fp_TV_Create(Footprint& f, TextureViewer*, Edx) { f.replay_only = "adds notifications, allocates a canvas, loads a stamp"; }
PORT_FN(0x004b5ab0, "TextureViewer::Create", TV_Create_n, fp_TV_Create)

static void __fastcall TV_Destroy_n(TextureViewer* self, Edx) {
    ccall<void>(F_gxFreeCanvas, (void*)&self->canvas);
    ccall<void>(F_gxForgetStamp, (void*)self->cursor);
}
static void fp_TV_Destroy(Footprint& f, TextureViewer*, Edx) { f.replay_only = "frees its canvas, forgets its stamp"; }
PORT_FN(0x004b5c40, "TextureViewer::Destroy", TV_Destroy_n, fp_TV_Destroy)

static void __fastcall TV_Callback_n(TextureViewer* self, Edx, int32_t, const void*) { cc_dirty(self); }
static void fp_TV_Callback(Footprint& f, TextureViewer* self, Edx, int32_t, const void*) { fp_widget(f, self); }
PORT_FN(0x004b5c60, "TextureViewer::Callback", TV_Callback_n, fp_TV_Callback)

static void* __fastcall TV_GetCursor_n(TextureViewer* self, Edx) { return self->cursor; }
static void fp_TV_GetCursor(Footprint& f, TextureViewer*, Edx) { f.pure = true; }
PORT_FN(0x004b5c70, "TextureViewer::GetCursor", TV_GetCursor_n, fp_TV_GetCursor)

// TextureViewer::MouseMove: a drag moves the picked point (tools 0, 1: locked to u or v, or snapped to the nearest overlay
// point within 0.05), translates or rotates the points (tool 2), or scales them about the texture's middle (tool 3)
static void __fastcall TV_MouseMove_n(TextureViewer* self, Edx, int32_t x, int32_t y) {
    if (self->down == 0) return;
    if (!is_surface(UI_G32(S_SURFACE))) return;
    const uint32_t tool = (uint32_t)UI_G32(S_TV_TOOL);
    if (tool > 3) return;
    if (tool <= 1) {
        float u, v;                                      // E-0x4c, E-0x50
        volatile int32_t vtx;                            // E-0x24
        float tu, tv;                                    // E-0x30, E-0x54
        if (UI_G32(S_TPOINT) < 0) return;
        const int32_t tp = UI_G32(S_TPOINT);
        u = (float)tcall<double>(F_TV_GetTextureU, self, x);
        v = (float)tcall<double>(F_TV_GetTextureV, self, y);
        if (UI_G32(S_LOCK) == 1) {
            ed_mv32(&v, &self->v);
        } else if (UI_G32(S_LOCK) == 2) {
            ed_mv32(&u, &self->u);
        } else if (UI_G32(S_LOCK) == 3) {
            float bu, bv;                                // E-0x48, E-0x44
            volatile float bd;                           // E-0x38
            ed_mv32(&bu, &u);
            ed_mv32(&bv, &v);
            *(volatile uint32_t*)&bd = 0x4b189680u;      // 1e7
            const int32_t ov = self->overlay;
            if (ov >= 0 && self->noverlays > ov) {
                int32_t n = self->counts[ov];
                if (n > 0) {
                    const EdTexPt* pp = &self->pts[ov][0];
                    do {
                        float pu, pv;                    // E-0x34, E-0x40
                        ed_mv32(&pu, &pp->u);
                        ed_mv32(&pv, &pp->v);
                        const double dv = ed_subm(D(pv), &v);
                        const double du = ed_subm(D(pu), &u);
                        const double d = dv * dv + du * du;
                        const bool near_ = !(d >= D(k_snap));
                        volatile float df = (float)d;    // E-0x3c
                        if (near_ && D(bd) > D(df)) {
                            ed_mv32(&bu, &pu);
                            ed_mv32(&bv, &pv);
                            ed_mv32(&bd, &df);
                        }
                        pp++;
                    } while (--n);
                }
            }
            ed_mv32(&u, &bu);
            ed_mv32(&v, &bv);
        }
        if (get_tp(UI_G32(S_SURFACE), tp, (void*)&vtx, &tu, &tv)) {
            uint32_t* ub = (uint32_t*)&u;
            uint32_t* vb = (uint32_t*)&v;
            if (*(volatile uint32_t*)ub > 0x80000000u) *(volatile uint32_t*)ub = 0;
            if (*(volatile uint32_t*)vb > 0x80000000u) *(volatile uint32_t*)vb = 0;
            if (*(volatile int32_t*)ub > 0x3f800000) *(volatile uint32_t*)ub = 0x3f800000u;
            if (*(volatile int32_t*)vb > 0x3f800000) *(volatile uint32_t*)vb = 0x3f800000u;
            move_tp(UI_G32(S_SURFACE), tp, ed_bits(ub), ed_bits(vb));
            UI_G8(S_MODIFIED) = 1;
            tcall<void>(F_TV_update_coords, self);
            rebuild();
        }
        cc_dirty(self);
        return;
    }
    if (tool == 2) {
        tcall<void>(F_TV_TranslatePoints, self, x, y);
        cc_dirty(self);
        return;
    }
    // tool 3: scale about (0.5, 0.5)
    float nu, nv, sx, sy;                                // E-0x48, E-0x44, E-0x40, E-0x54
    float M[9], C[3];                                    // E-0x24, E-0x30
    volatile int32_t vtx;                                // E-0x34
    float tu, tv;                                        // E-0x38, E-0x3c
    save_undo();
    {
        const double a = D(ed_add(ed_sub(UI_G32(S_HAXIS + 8), self->x), x)) / D(UI_G32(S_HAXIS));
        const int32_t iy = ed_add(ed_sub(UI_G32(S_VAXIS + 8), self->y), y);
        const bool lock_u = UI_G32(S_LOCK) == 1;
        nu = (float)a;
        const double b = D(iy) / D(UI_G32(S_VAXIS));
        nv = (float)b;
        if (lock_u) ed_mv32(&nv, &self->v);
        else if (UI_G32(S_LOCK) == 2) ed_mv32(&nu, &self->u);
    }
    sx = (float)(ed_subm(D(nu), &self->u) + D(k_one));
    {
        volatile uint32_t* m = (volatile uint32_t*)M;
        m[0] = ed_bits(&sx);
        m[1] = 0; m[2] = 0; m[3] = 0;
        sy = (float)(ed_subm(D(nv), &self->v) + D(k_one));
        m[4] = ed_bits(&sy);
        volatile uint32_t* c = (volatile uint32_t*)C;
        c[0] = 0x3f000000u;
        m[5] = 0; m[6] = 0;
        c[1] = 0x3f000000u;
        m[7] = 0; m[8] = 0x3f800000u;
        c[2] = 0;
    }
    mat_concat(ED_P(S_XFORM), ED_P(S_XFORM), M);
    ccall<void>(F_VectorSub, ED_P(S_XFORM + 0x24), ED_P(S_XFORM + 0x24), (const void*)C);
    mul_point(ED_P(S_XFORM + 0x24), ED_P(S_XFORM + 0x24), M);
    ccall<void>(F_VectorAdd, ED_P(S_XFORM + 0x24), ED_P(S_XFORM + 0x24), (const void*)C);
    {
        const uint32_t a = ed_bits(&nu), b = ed_bits(&nv);
        self->u_b = a;
        self->v_b = b;
    }
    for (int32_t i = 0; get_tp(UI_G32(S_SURFACE), i, (void*)&vtx, &tu, &tv); i++) {
        volatile float nvv = (float)((D(tv) - D(k_half)) * D(sy) + D(k_half));
        volatile float nuu = (float)((D(tu) - D(k_half)) * D(sx) + D(k_half));
        move_tp(UI_G32(S_SURFACE), i, ed_bits(&nuu), ed_bits(&nvv));
    }
    UI_G8(S_MODIFIED) = 1;
    tcall<void>(F_TV_update_coords, self);
    rebuild();
    cc_dirty(self);
}
static void fp_TV_MouseMove(Footprint& f, TextureViewer*, Edx, int32_t, int32_t) { fp_rebuild(f); }
PORT_FN(0x004b5c80, "TextureViewer::MouseMove", TV_MouseMove_n, fp_TV_MouseMove)

// TextureViewer::GetTextureU / V: a window position in texture coordinates (the scroll axis: (pos - x + px) / total).
// Returned unrounded in st0, as the original.
static double __fastcall TV_GetTextureU_n(TextureViewer* self, Edx, int32_t px) {
    return D(ed_add(ed_sub(UI_G32(S_HAXIS + 8), self->x), px)) / D(UI_G32(S_HAXIS));
}
static double __fastcall TV_GetTextureV_n(TextureViewer* self, Edx, int32_t py) {
    return D(ed_add(ed_sub(UI_G32(S_VAXIS + 8), self->y), py)) / D(UI_G32(S_VAXIS));
}
static void fp_TV_GetTexture(Footprint&, TextureViewer*, Edx, int32_t) {}
PORT_FN(0x004b60e0, "TextureViewer::GetTextureU", TV_GetTextureU_n, fp_TV_GetTexture)
PORT_FN(0x004b6120, "TextureViewer::GetTextureV", TV_GetTextureV_n, fp_TV_GetTexture)

// TextureViewer::TranslatePoints: every point moved by the drag (locked to u or v), or with Ctrl rotated about the picked
// point by the horizontal drag (a quarter turn a texture's width)
static void __fastcall TV_TranslatePoints_n(TextureViewer* self, Edx, int32_t x, int32_t y) {
    float nu, nv;                                        // E-0x14, E-0x8
    volatile int32_t vtx;                                // E-0x28
    save_undo();
    {
        const double a = D(ed_add(ed_sub(UI_G32(S_HAXIS + 8), self->x), x)) / D(UI_G32(S_HAXIS));
        const int32_t iy = ed_add(ed_sub(UI_G32(S_VAXIS + 8), self->y), y);
        nu = (float)a;
        nv = (float)(D(iy) / D(UI_G32(S_VAXIS)));
    }
    if (!scan_down(0xfe)) {
        float du, dv, tu, tv;                            // E-0x24, E-0x20, E-0x18, E-0x1c
        if (UI_G32(S_LOCK) == 1) ed_mv32(&nv, &self->v);
        else if (UI_G32(S_LOCK) == 2) ed_mv32(&nu, &self->u);
        du = (float)ed_subm(D(nu), &self->u);
        dv = (float)ed_subm(D(nv), &self->v);
        UI_GF(S_XFORM + 0x24) = (float)ed_addm(D(du), (const volatile float*)ED_P(S_XFORM + 0x24));
        UI_GF(S_XFORM + 0x28) = (float)ed_addm(D(dv), (const volatile float*)ED_P(S_XFORM + 0x28));
        for (int32_t i = 0; get_tp(UI_G32(S_SURFACE), i, (void*)&vtx, &tu, &tv); i++) {
            volatile float nvv = (float)(D(tv) + dv);
            volatile float nuu = (float)(D(tu) + du);
            move_tp(UI_G32(S_SURFACE), i, ed_bits(&nuu), ed_bits(&nvv));
        }
    } else {
        volatile float th, s, cu, cv;                    // E-0x24 (theta, then its cosine), E-0x1c, E-0x10, E-0xc
        float tu, tv;                                    // E-0x4, E-0x18
        volatile int32_t vtx2;                           // E-0x20
        vtx = -1;
        th = (float)(ed_subm(D(nu), &self->u) * D(k_pi2));
        ((LogD_t)(uintptr_t)F_LogReport)(ED_CP(0x004fefbc), D(th));       // "theta = %f"
        const double thd = th;
        *(volatile uint32_t*)&cv = 0;
        *(volatile uint32_t*)&cu = 0;
        s = x87_sin_f(thd);
        const int32_t tp = UI_G32(S_TPOINT);
        th = x87_cos_f(D(th));
        if (!get_tp(UI_G32(S_SURFACE), tp, (void*)&vtx, (void*)&cu, (void*)&cv)) goto done;
        for (int32_t i = 0; get_tp(UI_G32(S_SURFACE), i, (void*)&vtx2, &tu, &tv); i++) {
            const double a = ed_subm(D(tu), &cu);
            const double b = ed_subm(D(tv), &cv);
            const double U = ed_addm(D(th) * a + D(s) * b, &cu);
            const double V = ed_addm(b * D(th) - D(s) * a, &cv);
            volatile float vv = (float)V, uu = (float)U;
            move_tp(UI_G32(S_SURFACE), i, ed_bits(&uu), ed_bits(&vv));
        }
    }
done:
    {
        const uint32_t a = ed_bits(&nu), b = ed_bits(&nv);
        self->u_b = a;
        self->v_b = b;
    }
    UI_G8(S_MODIFIED) = 1;
    {
        volatile int32_t v3;
        float cu, cv;                                    // E-0x24, E-0x20
        if (is_surface(UI_G32(S_SURFACE)) && get_tp(UI_G32(S_SURFACE), UI_G32(S_TPOINT), (void*)&v3, &cu, &cv)) coord_text(&cu, &cv);
        else coord_clear();
    }
    rebuild();
}
static void fp_TV_TranslatePoints(Footprint& f, TextureViewer*, Edx, int32_t, int32_t) { fp_rebuild(f); }
PORT_FN(0x004b6160, "TextureViewer::TranslatePoints", TV_TranslatePoints_n, fp_TV_TranslatePoints)

static void __fastcall TV_MouseUp_n(TextureViewer* self, Edx, int32_t, int32_t) {
    self->down = 0;
    is_surface(UI_G32(S_SURFACE));
}
static void fp_TV_MouseUp(Footprint& f, TextureViewer* self, Edx, int32_t, int32_t) { f.add(self, 0x84, "this"); }
PORT_FN(0x004b64d0, "TextureViewer::MouseUp", TV_MouseUp_n, fp_TV_MouseUp)

// TextureViewer::Draw: the texture zoomed, the texture triangles' outlines, the points (XOR, a circle: the picked one, the
// selected ones, the vertex's), the capture overlay's points, in geometry mode the last three points' lines
static void __fastcall TV_Draw_n(TextureViewer* self, Edx, gxCanvas* c) {
    // E-0x154: every GetTexturePoint's vertex, the triangles', the points' and the three points' (left as the last one set it)
    volatile int32_t vtx;
    if (!is_surface(UI_G32(S_SURFACE))) {
        set_canvas(c);
        const int32_t y = self->y, x = self->x;
        const uint32_t col = UI_GU32(S_BLACK);
        ccall<void>(F_gxRect, x, y, ed_add(self->w, x), ed_add(self->h, y), col);
        return;
    }
    {
        const int32_t y = self->y, x = self->x;
        ccall<void>(F_gxSetClip, c, x, y, ed_add(self->w, x), ed_add(self->h, y));
    }
    {
        char name[0x100];                                // "texture.stp", then zeros (the surface's texture: never used)
        crt_copy(name, ED_P(0x004fefc8), 12);
        volatile uint32_t* z = (volatile uint32_t*)(name + 12);
        for (int i = 0; i < 0x3d; i++) z[i] = 0;
        ccall<void>(F_ModBuilderGetSurfaceTexture, bld(), UI_G32(S_SURFACE), (char*)name);
    }
    set_canvas(c);
    {
        const double zf = D(self->zoom) / D(self->tex_size) * D(k_256);
        volatile float a = (float)zf, b = (float)zf;
        const int32_t py = ed_sub(self->y, UI_G32(S_VAXIS + 8));
        const int32_t px = ed_sub(self->x, UI_G32(S_HAXIS + 8));
        ccall<void>(F_gxPasteZoom, (void*)&self->canvas, px, py, ed_bits(&b), ed_bits(&a));
    }
    {
        volatile int32_t t, ia, ib, ic;                  // E-0x104, E-0x108, E-0x10c, E-0x110
        float u, v;
        int32_t k = 0;
        if (get_ttri(UI_G32(S_SURFACE), 0, (void*)&t, (void*)&ia, (void*)&ib, (void*)&ic)) {
            do {
                int32_t x0 = 0, y0 = 0, y1 = 0, x1 = 0, x2 = 0, y2 = 0;
                if (get_tp(UI_G32(S_SURFACE), ia, (void*)&vtx, &u, &v)) {
                    x0 = ed_add(self->x, tv_px(&u));
                    y0 = ed_add(self->y, tv_py(&v));
                }
                if (get_tp(UI_G32(S_SURFACE), ib, (void*)&vtx, &u, &v)) {
                    x1 = ed_add(tv_px(&u), self->x);
                    y1 = ed_add(self->y, tv_py(&v));
                }
                if (get_tp(UI_G32(S_SURFACE), ic, (void*)&vtx, &u, &v)) {
                    x2 = ed_add(tv_px(&u), self->x);
                    y2 = ed_add(tv_py(&v), self->y);
                }
                ccall<void>(F_gxLine, x0, y0, x1, y1, UI_GU32(S_BLACK));
                ccall<void>(F_gxLine, x1, y1, x2, y2, UI_GU32(S_BLACK));
                ccall<void>(F_gxLine, x2, y2, x0, y0, UI_GU32(S_BLACK));
                k++;
            } while (get_ttri(UI_G32(S_SURFACE), k, (void*)&t, (void*)&ia, (void*)&ib, (void*)&ic));
        }
        for (int32_t i = 0; get_tp(UI_G32(S_SURFACE), i, (void*)&vtx, &u, &v); i++) {
            const int32_t px = ed_add(self->x, tv_px(&u));
            const int32_t py = ed_add(self->y, tv_py(&v));
            ccall<void>(F_gxXORPoint, px, py);
            uint32_t col;
            if (UI_G32(S_MODE) == 4) {
                if (i == UI_G32(S_TPOINT)) col = UI_GU32(S_PT_COLOR);
                else if (ccall<uint8_t>(F_is_selected, i)) col = UI_GU32(S_SEL_COLOR);
                else col = UI_GU32(S_BLACK);
            } else {
                if (i == UI_G32(S_TPOINT)) col = UI_GU32(S_PT_COLOR);
                else if (vtx == UI_G32(S_VERTEX)) col = UI_GU32(S_SEL_COLOR);
                else col = UI_GU32(S_BLACK);
            }
            ccall<void>(F_gxCircle, px, py, (int32_t)3, col);
        }
    }
    {
        // FIX CANDIDATE: the overlay's points as capture_cb stored them (no bound on a surface's count: see capture_cb)
        const int32_t ov = self->overlay;
        if (ov >= 0 && self->noverlays > ov) {
            int32_t i = 0;
            const volatile int32_t* cnt = &self->counts[ov];
            if (*cnt > 0) {
                const EdTexPt* pp = &self->pts[ov][0];
                do {
                    const uint32_t col = UI_GU32(S_PT_COLOR);
                    i++;
                    const int32_t py = ed_add(self->y, x87_ftol(D(UI_G32(S_VAXIS)) * D(pp->v) - D(UI_G32(S_VAXIS + 8))));
                    const int32_t px = ed_add(self->x, x87_ftol(D(pp->u) * D(UI_G32(S_HAXIS)) - D(UI_G32(S_HAXIS + 8))));
                    ccall<void>(F_gxPoint, px, py, col);
                    pp++;
                } while (*cnt > i);
            }
        }
    }
    if (UI_G32(S_MODE) == 4) {
        // the three points' lines (where their vertices aren't all at one place)
        // FIX CANDIDATE: a point that isn't found leaves the vertex as the last GetTexturePoint set it -- uninitialised (the
        // stack's garbage) if none has: a surface without texture points and a selection not in it. No harm seen: the vertex
        // is only looked up (an out-of-range one isn't found).
        float u, v;                                      // E-0x144, E-0x140
        float p0[3] = {0, 0, 0}, p1[3] = {0, 0, 0}, p2[3] = {0, 0, 0};   // E-0x124, E-0x160, E-0x130
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (get_tp(UI_G32(S_SURFACE), UI_G32(S_SEL), (void*)&vtx, &u, &v)) {
            x0 = ed_add(tv_px(&u), self->x);
            y0 = ed_add(self->y, tv_py(&v));
        }
        if (!get_vertex(vtx, p0)) goto restore;
        if (get_tp(UI_G32(S_SURFACE), UI_G32(S_SEL + 4), (void*)&vtx, &u, &v)) {
            x1 = ed_add(tv_px(&u), self->x);
            y1 = ed_add(tv_py(&v), self->y);
        }
        if (!get_vertex(vtx, p1)) goto restore;
        if (get_tp(UI_G32(S_SURFACE), UI_G32(S_SEL + 8), (void*)&vtx, &u, &v)) {
            x2 = ed_add(self->x, tv_px(&u));
            y2 = ed_add(self->y, tv_py(&v));
        }
        if (!get_vertex(vtx, p2)) goto restore;
        {
            float dx, dy;                                // E-0x150, E-0x14c
            dx = (float)ed_subm(D(p2[0]), &p0[0]);
            dy = (float)ed_subm(D(p2[1]), &p0[1]);
            const double dz = ed_subm(D(p2[2]), &p0[2]);
            const float dzf = (float)dz;                 // fst [E-0x148]; fmul [E-0x148]
            const double len = x87_sqrt((dz * dzf + D(dy) * dy) + D(dx) * dx);
            if (len > D(k_tiny)) {
                ccall<void>(F_gxLine, x2, y2, x0, y0, UI_GU32(S_EDGE_COLOR));
                ccall<void>(F_gxLine, x2, y2, x1, y1, UI_GU32(S_EDGE_COLOR));
            }
        }
    }
restore:
    ccall<void>(F_gxRestoreClip, c);
}
static void fp_TV_Draw(Footprint& f, TextureViewer*, Edx, gxCanvas* c) {
    fp_cur(f);
    UI_FP_CANVAS(f, c);
}
PORT_FN(0x004b64f0, "TextureViewer::Draw", TV_Draw_n, fp_TV_Draw)

// TextureViewer::update_coords: the picked point's coordinates, or ""
static void __fastcall TV_update_coords_n(TextureViewer*, Edx) {
    volatile int32_t vtx;                                // E-0x4
    float u, v;                                          // E-0x8, E-0xc
    if (is_surface(UI_G32(S_SURFACE)) && get_tp(UI_G32(S_SURFACE), UI_G32(S_TPOINT), (void*)&vtx, &u, &v)) coord_text(&u, &v);
    else coord_clear();
}
// the text's 0x20 bytes, and what a huge coordinate's text runs over (two "%5.1f" of a float: 0x58 bytes at most)
static void fp_TV_update_coords(Footprint& f, TextureViewer*, Edx) { UI_FP(f, S_COORD_TEXT, 0x58, "the coordinates' text (and its overrun)"); }
PORT_FN(0x004b6c00, "TextureViewer::update_coords", TV_update_coords_n, fp_TV_update_coords)

}  // namespace edit_view
}  // namespace
