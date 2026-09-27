// gx_dd.cpp -- M3 graphics, step G2: dd.obj's wrappers on the OpenGL renderer (gl_core.h).
//
//   dd.obj   the thin C++ wrappers the game keeps its DirectX objects in: ddraw (IDirectDraw2), dsurface
//            (IDirectDrawSurface3), dtexture (+ its IDirect3DTexture2 and handle), d3d (IDirect3D2 + IDirect3DDevice2),
//            dviewport, dmaterial
//
// Step G1 rewrote these faithfully as DirectX calls. Here they call the renderer instead: a dsurface is the
// renderer's surface (a texture, one of its mip levels, the page the 2D draws on, the primary and z-buffer), a
// dtexture's IDirect3DTexture2 is that surface's texture, the d3d device is the renderer's render state, a dviewport
// and a dmaterial its viewport and background colour. The handles in the wrappers are the renderer's objects, the
// same ones the COM facade (ddraw_gl.cpp) serves to the original code, so the two can be mixed freely.
//
// What the game sees doesn't change. Each wrapper does what its original's DirectX calls came to: the same objects
// made, held and released (a QueryInterface that holds a reference holds one here too; a CreateSurface, a
// QueryInterface and a Release come to one new surface), the same results in dx_result in the same places, the same
// error paths (dx_error, LogReport, ExitProcess) behind the renderer's results -- which, as the game's DirectX calls
// did under the emulation, never fail, so nothing is ever lost and Alt-Tab needs no restore. Every other game
// function is called by its v1.0 address, as in G1.
//
// Checked in the game like any rewrite: a shadow check runs the original (through the facade) and then this, both on
// the renderer, and compares the OpenGL calls each made (gl_table.h) and the renderer's state each left (gl_core.h),
// with the footprints below for the game's own memory. These need the OpenGL renderer: with the game's own
// DirectDraw ([platform] renderer=ddraw) they stay original (PORT_FN_GL).
#define _CRT_SECURE_NO_WARNINGS
#include "gl_core.h"
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "gx_gl.h"

namespace {
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

enum : uint32_t {
    X_RESULT = 0x522e6c,        // HRESULT dx_result
    X_ERRBUF = 0x522e78,        // char[]: dx_name_error's "unknown error: %x"
    D_DEVICE = 0x4f3a60,        // IDirect3DDevice2* (also d3d +4)
};
enum : uint32_t { ERRBUF_BYTES = 0x20, DDSD_BYTES = 0x6c, DDCAPS_BYTES = 0x16c, D3DDD_BYTES = 0xcc };

static __forceinline volatile int32_t& I32(uint32_t a) { return *(volatile int32_t*)(uintptr_t)a; }
static __forceinline void* volatile& PV(uint32_t a) { return *(void* volatile*)(uintptr_t)a; }
static __forceinline const char* S(uint32_t a) { return (const char*)(uintptr_t)a; }
static __forceinline void* A(uint32_t a) { return (void*)(uintptr_t)a; }

// ---- the wrappers' layouts ------------------------------------------------------------------------------------------
struct DSurface { void* s; };                       // the surface (IDirectDrawSurface3)
struct DTexture { void* s; void* tex; uint32_t handle; void* first; };   // +4 its IDirect3DTexture2, +8 handle, +0xc
struct DDraw { void* dd; };                         // IDirectDraw2
struct D3D { void* d3d; void* dev; };               // IDirect3D2, the device (IDirect3DDevice2)
struct DMaterial { void* mat; uint32_t handle; };   // IDirect3DMaterial2, its handle
struct DViewport { void* vp; };                     // IDirect3DViewport2

// ---- calls ----------------------------------------------------------------------------------------------------------
template <typename R, typename... Args> static __forceinline R game(uint32_t at, Args... a) {
    typedef R(__cdecl* F)(Args...);
    return ((F)(uintptr_t)at)(a...);
}
template <typename R, typename... Args> static __forceinline R method(uint32_t at, void* self, Args... a) {
    typedef R(__fastcall* F)(void*, Edx, Args...);
    return ((F)(uintptr_t)at)(self, 0, a...);
}
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogReport = (Log_t)0x00411150;
static __forceinline void exit_process(uint32_t code) {        // call dword ptr [imp__ExitProcess@4]
    (*(void(__stdcall* volatile*)(uint32_t))0x005d7478)(code);
}
static __forceinline void* MemAlloc(int n) { return game<void*>(0x004140e0, n); }
static __forceinline void op_delete(void* p) { game<void>(0x00414390, p); }
static __forceinline void dx_error(uint32_t msg, int32_t hr) { game<void>(0x00458c20, S(msg), hr); }
static __forceinline uint8_t VidHas3DHW() { return game<uint8_t>(0x00454930); }
static __forceinline void* ddraw_ctor(void* self, void* dd2) { return method<void*>(0x0045f160, self, dd2); }
static __forceinline void ddraw_dtor(void* self) { method<void>(0x0045f170, self); }
static __forceinline void* ddraw_make_dd_surface(void* self, void* desc) { return method<void*>(0x0045f2d0, self, desc); }
static __forceinline void* dsurface_ctor(void* self, void* s3) { return method<void*>(0x0045f460, self, s3); }
static __forceinline void dsurface_dtor(void* self) { method<void>(0x0045f470, self); }
static __forceinline void* dtexture_ctor(void* self, void* s3) { return method<void*>(0x0045f750, self, s3); }
static __forceinline void dtexture_dtor(void* self) { method<void>(0x0045f7c0, self); }
static __forceinline void* d3d_create_driver(void* self, void* dd) { return method<void*>(0x0045ee20, self, dd); }
static __forceinline void* d3d_create_device(void* self, void* s) { return method<void*>(0x0045ee70, self, s); }
static __forceinline void* dmaterial_ctor(void* self, void* d3d, void* m) { return method<void*>(0x0045ea40, self, d3d, m); }
static __forceinline void dmaterial_dtor(void* self) { method<void>(0x0045eaf0, self); }
static __forceinline void* dviewport_ctor(void* self, void* d3d, void* m) { return method<void*>(0x0045eb40, self, d3d, m); }
static __forceinline void dviewport_dtor(void* self) { method<void>(0x0045ebf0, self); }

// ---- footprint helpers ------------------------------------------------------------------------------------------------
static void fp_at(Footprint& f, uint32_t a, uint32_t n, const char* what) { f.add(A(a), n, what); }
static void fp_result(Footprint& f) {                   // dx_result, and dx_error's name for an unknown code
    fp_at(f, X_RESULT, 4, "dx_result");
    fp_at(f, X_ERRBUF, ERRBUF_BYTES, "dx_name_error's buffer");
}
static void fp_transform_data(Footprint& f, const uint32_t* xf, uint32_t n) {
    if (!xf) return;
    f.add((void*)xf, 0x34, "D3DTRANSFORMDATA");
    if (xf[3]) f.add((void*)(uintptr_t)xf[3], n * xf[4], "transformed vertices");
    if (xf[5]) f.add((void*)(uintptr_t)xf[5], n * 0x10, "homogeneous vertices");
}

using namespace gfx;

// dd_enum (0x45e940): DirectDrawEnumerateA(cb, 0)
static void __cdecl dd_enum_rw(void* cb) { enum_drivers((LPDDENUMCALLBACKA)cb, 0); }
static void fp_dd_enum(Footprint& f, void*) { f.replay_only = "DirectDrawEnumerate calls back into the game"; }
PORT_FN_GL(0x0045e940, "dd_enum", dd_enum_rw, fp_dd_enum)

// dd_create (0x45e950): DirectDrawCreate and its IDirectDraw2 come to a new IDirectDraw2; a ddraw around it
static void* __cdecl dd_create_rw(void* guid) {
    PV(D_DEVICE) = 0;
    void* dd2 = handle(make<DirectDraw2>());
    int32_t hr = DD_OK;
    I32(X_RESULT) = hr;
    if (hr != 0) {
        LogReport(S(0x4f3a64));                          // "Unable to create DirectDraw Object"
        exit_process(2);
        return 0;
    }
    if (I32(X_RESULT) != 0) {
        dx_error(0x4f3a88, I32(X_RESULT));               // "Unable to get DirectDraw2 interface"
        exit_process(2);
        return 0;
    }
    void* p = MemAlloc(4);
    if (!p) return 0;
    return ddraw_ctor(p, dd2);
}
static void fp_dd_create(Footprint& f, void*) { f.replay_only = "allocates the ddraw wrapper"; }
PORT_FN_GL(0x0045e950, "dd_create", dd_create_rw, fp_dd_create)

// dd_destroy (0x45ea20)
static void __cdecl dd_destroy_rw(void* dd) {
    if (!dd) return;
    ddraw_dtor(dd);
    op_delete(dd);
}
static void fp_dd_destroy(Footprint& f, void* dd) {
    if (dd) f.replay_only = "frees the ddraw wrapper";
}
PORT_FN_GL(0x0045ea20, "dd_destroy", dd_destroy_rw, fp_dd_destroy)

// dmaterial::dmaterial (0x45ea40): a new material, the colour set, its handle
static DMaterial* __fastcall dmaterial_ctor_rw(DMaterial* self, Edx, void*, void* m) {
    volatile DMaterial* v = self;
    v->mat = 0;
    v->handle = 0;
    v->mat = handle(make<Material>());
    int32_t hr = DD_OK;
    I32(X_RESULT) = hr;
    if (hr != 0) {
        dx_error(0x4f3ad0, I32(X_RESULT));               // "create material"
        return self;
    }
    set_material(material(v->mat), (const D3DMATERIAL*)m);
    I32(X_RESULT) = hr;
    if (hr == 0) {
        v->handle = material_handle(material(v->mat));
        I32(X_RESULT) = hr;
        if (hr == 0) return self;
        dx_error(0x4f3ab4, hr);                          // "get handle"
    } else {
        dx_error(0x4f3ac0, I32(X_RESULT));               // "set material"
    }
    material(v->mat)->drop();
    v->mat = 0;
    return self;
}
static void fp_dmaterial_ctor(Footprint& f, DMaterial* self, Edx, void*, void*) {
    f.add(self, sizeof(DMaterial), "dmaterial");
    fp_result(f);
}
PORT_FN_GL(0x0045ea40, "dmaterial::dmaterial", dmaterial_ctor_rw, fp_dmaterial_ctor)

// dmaterial::~dmaterial (0x45eaf0)
static void __fastcall dmaterial_dtor_rw(DMaterial* self, Edx) {
    volatile DMaterial* v = self;
    material(v->mat)->drop();
    v->handle = 0;
    v->mat = 0;
}
static void fp_dmaterial_dtor(Footprint& f, DMaterial* self, Edx) { f.add(self, sizeof(DMaterial), "dmaterial"); }
PORT_FN_GL(0x0045eaf0, "dmaterial::~dmaterial", dmaterial_dtor_rw, fp_dmaterial_dtor)

// dmaterial::set_material (0x45eb10)
static void __fastcall dmaterial_set_material_rw(DMaterial* self, Edx, void* m) {
    set_material(material(((volatile DMaterial*)self)->mat), (const D3DMATERIAL*)m);
    I32(X_RESULT) = DD_OK;
}
static void fp_dmaterial_set(Footprint& f, DMaterial*, Edx, void*) { fp_result(f); }
PORT_FN_GL(0x0045eb10, "dmaterial::set_material", dmaterial_set_material_rw, fp_dmaterial_set)

// dviewport::dviewport (0x45eb40): a new viewport, the device's current one, the material its background
static DViewport* __fastcall dviewport_ctor_rw(DViewport* self, Edx, void*, DMaterial* m) {
    volatile DViewport* v = self;
    v->vp = handle(make<Viewport>());
    int32_t hr = DD_OK;
    I32(X_RESULT) = hr;                                  // (AddViewport: nothing to do)
    I32(X_RESULT) = hr;
    use_viewport(device(PV(D_DEVICE)), viewport(v->vp));
    I32(X_RESULT) = hr;
    set_background(viewport(v->vp), ((volatile DMaterial*)m)->handle);   // (result not kept)
    return self;
}
static void fp_dviewport_ctor(Footprint& f, DViewport* self, Edx, void*, DMaterial*) {
    f.add(self, sizeof(DViewport), "dviewport");
    fp_result(f);
}
PORT_FN_GL(0x0045eb40, "dviewport::dviewport", dviewport_ctor_rw, fp_dviewport_ctor)

// dviewport::~dviewport (0x45ebf0)
static void __fastcall dviewport_dtor_rw(DViewport* self, Edx) {
    volatile DViewport* v = self;
    remove_viewport(device(PV(D_DEVICE)), viewport(v->vp));
    viewport(v->vp)->drop();
    v->vp = 0;
}
static void fp_dviewport_dtor(Footprint& f, DViewport* self, Edx) { f.add(self, sizeof(DViewport), "dviewport"); }
PORT_FN_GL(0x0045ebf0, "dviewport::~dviewport", dviewport_dtor_rw, fp_dviewport_dtor)

// dviewport::set_viewport (0x45ec20): D3DVIEWPORT2 {x, y, w, h}, clip window (-1, h/w, 2, 2h/w), z 0..1
static uint8_t __fastcall dviewport_set_viewport_rw(DViewport* self, Edx, int x, int y, int w, int h) {
    uint32_t d[0x2c / 4];
    memset(d, 0, sizeof d);
    d[1] = (uint32_t)x;
    d[2] = (uint32_t)y;
    d[3] = (uint32_t)w;
    d[4] = (uint32_t)h;
    double q = (double)h / (double)w;                    // fild h; fidiv w (FIX CANDIDATE: w = 0)
    d[0] = 0x2c;
    d[5] = 0xbf800000;                                   // dvClipX -1
    d[7] = 0x40000000;                                   // dvClipWidth 2
    d[9] = 0;                                            // dvMinZ
    d[10] = 0x3f800000;                                  // dvMaxZ 1
    float cy = (float)q;                                 // fst dword
    float ch = (float)(q * 2.0);                         // fmul dword 2.0; fstp dword
    memcpy(&d[6], &cy, 4);
    memcpy(&d[8], &ch, 4);
    set_viewport(viewport(((volatile DViewport*)self)->vp), (const D3DVIEWPORT2*)d);
    I32(X_RESULT) = DD_OK;
    return 1;
}
static void fp_dviewport_set(Footprint& f, DViewport*, Edx, int, int, int, int) { fp_result(f); }
PORT_FN_GL(0x0045ec20, "dviewport::set_viewport", dviewport_set_viewport_rw, fp_dviewport_set)

// dviewport::clear (0x45ece0): the rectangle's z (and the target's colour, the background material)
static void __fastcall dviewport_clear_rw(DViewport* self, Edx, int x1, int y1, int x2, int y2, uint8_t target) {
    D3DRECT r = {x1, y1, x2, y2};
    clear(viewport(((volatile DViewport*)self)->vp), 1, &r, target ? 3u : 2u);   // TARGET|ZBUFFER / ZBUFFER
    I32(X_RESULT) = DD_OK;
}
static void fp_dviewport_clear(Footprint& f, DViewport*, Edx, int, int, int, int, uint8_t) { fp_result(f); }
PORT_FN_GL(0x0045ece0, "dviewport::clear", dviewport_clear_rw, fp_dviewport_clear)

// dviewport::transform_verts (0x45ed50)
static void __fastcall dviewport_transform_verts_rw(DViewport* self, Edx, uint32_t n, void* data, uint32_t flags, uint32_t* off) {
    transform(viewport(((volatile DViewport*)self)->vp), n, (LPD3DTRANSFORMDATA)data, flags, (LPDWORD)off);
    I32(X_RESULT) = DD_OK;
}
static void fp_dviewport_transform(Footprint& f, DViewport*, Edx, uint32_t n, void* data, uint32_t, uint32_t* off) {
    fp_transform_data(f, (const uint32_t*)data, n);
    if (off) f.add(off, 4, "offscreen flag");
    fp_result(f);
}
PORT_FN_GL(0x0045ed50, "dviewport::transform_verts", dviewport_transform_verts_rw, fp_dviewport_transform)

// d3d::d3d (0x45ed90)
static D3D* __fastcall d3d_ctor_rw(D3D* self, Edx, void* dd) {
    void* d = d3d_create_driver(self, dd);
    volatile D3D* v = self;
    v->dev = 0;
    v->d3d = d;
    return self;
}
static void fp_d3d_ctor(Footprint& f, D3D* self, Edx, void*) {
    f.add(self, sizeof(D3D), "d3d");
    fp_result(f);
}
PORT_FN_GL(0x0045ed90, "d3d::d3d", d3d_ctor_rw, fp_d3d_ctor)

// d3d::~d3d (0x45edb0)
static void __fastcall d3d_dtor_rw(D3D* self, Edx) {
    volatile D3D* v = self;
    direct3d(v->d3d)->drop();
    v->d3d = 0;
}
static void fp_d3d_dtor(Footprint& f, D3D* self, Edx) { f.add(&self->d3d, 4, "d3d IDirect3D2"); }
PORT_FN_GL(0x0045edb0, "d3d::~d3d", d3d_dtor_rw, fp_d3d_dtor)

// d3d::connect (0x45edd0)
static uint8_t __fastcall d3d_connect_rw(D3D* self, Edx, void* s) {
    void* dev = d3d_create_device(self, s);
    volatile D3D* v = self;
    v->dev = dev;
    PV(D_DEVICE) = dev;
    return v->dev != 0;
}
static void fp_d3d_connect(Footprint& f, D3D* self, Edx, void*) {
    f.add(&self->dev, 4, "d3d device");
    fp_at(f, D_DEVICE, 4, "the device");
    fp_result(f);
}
PORT_FN_GL(0x0045edd0, "d3d::connect", d3d_connect_rw, fp_d3d_connect)

// d3d::disconnect (0x45ee00)
static void __fastcall d3d_disconnect_rw(D3D* self, Edx) {
    volatile D3D* v = self;
    device(v->dev)->drop();
    v->dev = 0;
    PV(D_DEVICE) = 0;
}
static void fp_d3d_disconnect(Footprint& f, D3D* self, Edx) {
    f.add(&self->dev, 4, "d3d device");
    fp_at(f, D_DEVICE, 4, "the device");
}
PORT_FN_GL(0x0045ee00, "d3d::disconnect", d3d_disconnect_rw, fp_d3d_disconnect)

// d3d::create_driver (0x45ee20): the IDirectDraw2's IDirect3D2, a new one
static void* __fastcall d3d_create_driver_rw(D3D*, Edx, DDraw*) {
    void* out = handle(make<Direct3D>());
    int32_t hr = DD_OK;
    I32(X_RESULT) = hr;
    if (hr != 0) {
        dx_error(0x4f3b5c, hr);                          // "Can't find IDirect3D2"
        exit_process(2);
    }
    return out;
}
static void fp_d3d_create_driver(Footprint& f, D3D*, Edx, DDraw*) { fp_result(f); }
PORT_FN_GL(0x0045ee20, "d3d::create_driver", d3d_create_driver_rw, fp_d3d_create_driver)

// d3d::create_device (0x45ee70): the surface's IDirectDrawSurface (a reference held, never released), then the
// device (HAL, or RGB without hardware: one renderer either way)
static void* __fastcall d3d_create_device_rw(D3D*, Edx, DSurface* s) {
    surface(((volatile DSurface*)s)->s)->hold();
    int32_t hr = DD_OK;
    I32(X_RESULT) = hr;
    if (hr != 0) {
        dx_error(0x4f3b74, hr);                          // "can't get old interface"
        return 0;
    }
    VidHas3DHW();
    Device* dev = 0;
    hr = create_device(&dev);
    I32(X_RESULT) = hr;
    if (I32(X_RESULT) != 0) {
        dx_error(0x4f3b8c, I32(X_RESULT));               // "create device"
        exit_process(2);
    }
    return dev ? handle(dev) : 0;
}
static void fp_d3d_create_device(Footprint& f, D3D*, Edx, DSurface*) { fp_result(f); }
PORT_FN_GL(0x0045ee70, "d3d::create_device", d3d_create_device_rw, fp_d3d_create_device)

// d3d::CreateMaterial (0x45ef20), DestroyMaterial (0x45ef50), CreateViewport (0x45ef70), DestroyViewport (0x45efa0)
static void* __fastcall d3d_CreateMaterial_rw(D3D* self, Edx, void* m) {
    void* p = MemAlloc(8);
    if (!p) return 0;
    return dmaterial_ctor(p, ((volatile D3D*)self)->d3d, m);
}
static void fp_d3d_create_material(Footprint& f, D3D*, Edx, void*) { f.replay_only = "allocates the material wrapper"; }
PORT_FN_GL(0x0045ef20, "d3d::CreateMaterial", d3d_CreateMaterial_rw, fp_d3d_create_material)
static void __fastcall d3d_DestroyMaterial_rw(D3D*, Edx, void* m) {
    if (!m) return;
    dmaterial_dtor(m);
    op_delete(m);
}
static void fp_d3d_destroy_material(Footprint& f, D3D*, Edx, void* m) {
    if (m) f.replay_only = "frees the material wrapper";
}
PORT_FN_GL(0x0045ef50, "d3d::DestroyMaterial", d3d_DestroyMaterial_rw, fp_d3d_destroy_material)
static void* __fastcall d3d_CreateViewport_rw(D3D* self, Edx, void* m) {
    void* p = MemAlloc(4);
    if (!p) return 0;
    return dviewport_ctor(p, ((volatile D3D*)self)->d3d, m);
}
static void fp_d3d_create_viewport(Footprint& f, D3D*, Edx, void*) { f.replay_only = "allocates the viewport wrapper"; }
PORT_FN_GL(0x0045ef70, "d3d::CreateViewport", d3d_CreateViewport_rw, fp_d3d_create_viewport)
static void __fastcall d3d_DestroyViewport_rw(D3D*, Edx, void* v) {
    if (!v) return;
    dviewport_dtor(v);
    op_delete(v);
}
static void fp_d3d_destroy_viewport(Footprint& f, D3D*, Edx, void* v) {
    if (v) f.replay_only = "frees the viewport wrapper";
}
PORT_FN_GL(0x0045efa0, "d3d::DestroyViewport", d3d_DestroyViewport_rw, fp_d3d_destroy_viewport)

// d3d::GetCaps (0x45efc0), GetStats (0x45f000)
static uint8_t __fastcall d3d_GetCaps_rw(D3D*, Edx, void* hal, void* hel) {
    device_caps((LPD3DDEVICEDESC)hal, (LPD3DDEVICEDESC)hel);
    I32(X_RESULT) = DD_OK;
    return 1;
}
static void fp_d3d_get_caps(Footprint& f, D3D*, Edx, void* hal, void* hel) {
    if (hal) f.add(hal, D3DDD_BYTES, "D3DDEVICEDESC hal");
    if (hel) f.add(hel, D3DDD_BYTES, "D3DDEVICEDESC hel");
    fp_result(f);
}
PORT_FN_GL(0x0045efc0, "d3d::GetCaps", d3d_GetCaps_rw, fp_d3d_get_caps)
static uint8_t __fastcall d3d_GetStats_rw(D3D*, Edx, void* st) {
    stats((LPD3DSTATS)st);
    I32(X_RESULT) = DD_OK;
    return 1;
}
static void fp_d3d_get_stats(Footprint& f, D3D*, Edx, void* st) {
    if (st) f.add(st, 0x18, "D3DSTATS");
    fp_result(f);
}
PORT_FN_GL(0x0045f000, "d3d::GetStats", d3d_GetStats_rw, fp_d3d_get_stats)

// d3d::BeginFrame (0x45f040), EndFrame (0x45f070): BeginScene / EndScene, nothing for the renderer to do
static void __fastcall d3d_BeginFrame_rw(D3D*, Edx) { I32(X_RESULT) = DD_OK; }
static void fp_d3d_self(Footprint& f, D3D*, Edx) { fp_result(f); }
PORT_FN_GL(0x0045f040, "d3d::BeginFrame", d3d_BeginFrame_rw, fp_d3d_self)
static void __fastcall d3d_EndFrame_rw(D3D*, Edx) { I32(X_RESULT) = DD_OK; }
PORT_FN_GL(0x0045f070, "d3d::EndFrame", d3d_EndFrame_rw, fp_d3d_self)

// d3d::SetRenderState (0x45f0a0), SetLightState (0x45f0d0), SetTransform (0x45f100), EnumTextureFormats (0x45f130)
static void __fastcall d3d_SetRenderState_rw(D3D*, Edx, uint32_t s, uint32_t v) {
    set_state(s, v);
    I32(X_RESULT) = DD_OK;
}
static void fp_d3d_uu(Footprint& f, D3D*, Edx, uint32_t, uint32_t) { fp_result(f); }
PORT_FN_GL(0x0045f0a0, "d3d::SetRenderState", d3d_SetRenderState_rw, fp_d3d_uu)
static void __fastcall d3d_SetLightState_rw(D3D*, Edx, uint32_t, uint32_t) { I32(X_RESULT) = DD_OK; }   // (no lighting)
PORT_FN_GL(0x0045f0d0, "d3d::SetLightState", d3d_SetLightState_rw, fp_d3d_uu)
static void __fastcall d3d_SetTransform_rw(D3D*, Edx, uint32_t t, void* m) {
    set_transform(t, (const D3DMATRIX*)m);
    I32(X_RESULT) = DD_OK;
}
static void fp_d3d_up(Footprint& f, D3D*, Edx, uint32_t, void*) { fp_result(f); }
PORT_FN_GL(0x0045f100, "d3d::SetTransform", d3d_SetTransform_rw, fp_d3d_up)
static void __fastcall d3d_EnumTextureFormats_rw(D3D*, Edx, void* cb, void* ctx) {
    enum_formats((LPD3DENUMTEXTUREFORMATSCALLBACK)cb, ctx);
    I32(X_RESULT) = DD_OK;
}
static void fp_d3d_enum_formats(Footprint& f, D3D*, Edx, void*, void*) { f.replay_only = "EnumTextureFormats calls back into the game"; }
PORT_FN_GL(0x0045f130, "d3d::EnumTextureFormats", d3d_EnumTextureFormats_rw, fp_d3d_enum_formats)

// ddraw::ddraw (0x45f160), ~ddraw (0x45f170)
static DDraw* __fastcall ddraw_ctor_rw(DDraw* self, Edx, void* dd2) {
    ((volatile DDraw*)self)->dd = dd2;
    return self;
}
static void fp_ddraw_ctor(Footprint& f, DDraw* self, Edx, void*) { f.add(self, sizeof(DDraw), "ddraw"); }
PORT_FN_GL(0x0045f160, "ddraw::ddraw", ddraw_ctor_rw, fp_ddraw_ctor)
static void __fastcall ddraw_dtor_rw(DDraw* self, Edx) {
    volatile DDraw* v = self;
    directdraw(v->dd)->drop();
    v->dd = 0;
}
static void fp_ddraw_dtor(Footprint& f, DDraw* self, Edx) { f.add(self, sizeof(DDraw), "ddraw"); }
PORT_FN_GL(0x0045f170, "ddraw::~ddraw", ddraw_dtor_rw, fp_ddraw_dtor)

// ddraw::get_caps (0x45f190), get_mem (0x45f1c0)
static void __fastcall ddraw_get_caps_rw(DDraw*, Edx, void* hal, void* hel) {
    dd_caps((LPDDCAPS)hal, (LPDDCAPS)hel);
    I32(X_RESULT) = DD_OK;
}
static void fp_ddraw_get_caps(Footprint& f, DDraw*, Edx, void* hal, void* hel) {
    if (hal) f.add(hal, DDCAPS_BYTES, "DDCAPS hal");
    if (hel) f.add(hel, DDCAPS_BYTES, "DDCAPS hel");
    fp_result(f);
}
PORT_FN_GL(0x0045f190, "ddraw::get_caps", ddraw_get_caps_rw, fp_ddraw_get_caps)
static void __fastcall ddraw_get_mem_rw(DDraw*, Edx, void* caps, uint32_t* total, uint32_t* free_) {
    vidmem((LPDDSCAPS)caps, (LPDWORD)total, (LPDWORD)free_);
    I32(X_RESULT) = DD_OK;
}
static void fp_ddraw_get_mem(Footprint& f, DDraw*, Edx, void*, uint32_t* total, uint32_t* free_) {
    if (total) f.add(total, 4, "total video memory");
    if (free_) f.add(free_, 4, "free video memory");
    fp_result(f);
}
PORT_FN_GL(0x0045f1c0, "ddraw::get_mem", ddraw_get_mem_rw, fp_ddraw_get_mem)

// ddraw::set_exclusive (0x45f200): SetCooperativeLevel(FULLSCREEN | EXCLUSIVE) -- the renderer starts
static void __fastcall ddraw_set_exclusive_rw(DDraw*, Edx) {
    game<void*>(0x00412be0);                             // Win32GetWindow
    int32_t hr = start() ? DD_OK : DDERR_GENERIC;
    I32(X_RESULT) = hr;
    if (hr != 0) {
        LogReport(S(0x4f3c50));                          // "Unable to get exclusive mode"
        exit_process(2);
    }
}
static void fp_ddraw_set_exclusive(Footprint& f, DDraw*, Edx) { fp_at(f, X_RESULT, 4, "dx_result"); }
PORT_FN_GL(0x0045f200, "ddraw::set_exclusive", ddraw_set_exclusive_rw, fp_ddraw_set_exclusive)

// ddraw::enum_modes (0x45f240)
static void __fastcall ddraw_enum_modes_rw(DDraw*, Edx, void*, void* cb) {
    enum_modes(0, (LPDDENUMMODESCALLBACK)cb);
    I32(X_RESULT) = DD_OK;
}
static void fp_ddraw_enum_modes(Footprint& f, DDraw*, Edx, void*, void*) { f.replay_only = "EnumDisplayModes calls back into the game"; }
PORT_FN_GL(0x0045f240, "ddraw::enum_modes", ddraw_enum_modes_rw, fp_ddraw_enum_modes)

// ddraw::set_mode (0x45f270)
static uint8_t __fastcall ddraw_set_mode_rw(DDraw*, Edx, int w, int h, int) {
    set_mode(w, h);
    I32(X_RESULT) = DD_OK;
    return 1;
}
static void fp_ddraw_set_mode(Footprint& f, DDraw*, Edx, int, int, int) { fp_result(f); }
PORT_FN_GL(0x0045f270, "ddraw::set_mode", ddraw_set_mode_rw, fp_ddraw_set_mode)

// ddraw::restore_mode (0x45f2c0): nothing to restore
static void __fastcall ddraw_restore_mode_rw(DDraw*, Edx) {}
static void fp_ddraw_self(Footprint&, DDraw*, Edx) {}
PORT_FN_GL(0x0045f2c0, "ddraw::restore_mode", ddraw_restore_mode_rw, fp_ddraw_self)

// ddraw::make_dd_surface (0x45f2d0): a new surface (CreateSurface, its IDirectDrawSurface3, the first interface
// released: one reference). Out of video memory is silent for a texture.
static void* __fastcall ddraw_make_dd_surface_rw(DDraw*, Edx, const uint32_t* desc) {
    Surface* s = 0;
    int32_t hr = create_surface((LPDDSURFACEDESC)desc, &s);
    I32(X_RESULT) = hr;
    if (hr == 0) {
        I32(X_RESULT) = DD_OK;
        return handle(s);
    }
    if (I32(X_RESULT) != (int32_t)DDERR_OUTOFVIDEOMEMORY) {
        dx_error(0x4f3cc0, I32(X_RESULT));               // "unable to create surface"
        return 0;
    }
    if (!(((const volatile uint8_t*)desc)[0x69] & 0x10)) dx_error(0x4f3cdc, I32(X_RESULT));   // not DDSCAPS_TEXTURE
    return 0;
}
static void fp_ddraw_make_dd_surface(Footprint& f, DDraw*, Edx, const uint32_t*) { fp_result(f); }
PORT_FN_GL(0x0045f2d0, "ddraw::make_dd_surface", ddraw_make_dd_surface_rw, fp_ddraw_make_dd_surface)

// ddraw::create_surface (0x45f3a0), destroy_surface (0x45f3e0), create_texture_surface (0x45f400),
// destroy_texture_surface (0x45f440)
static void* __fastcall ddraw_create_surface_rw(DDraw* self, Edx, void* desc) {
    void* s3 = ddraw_make_dd_surface(self, desc);
    if (!s3) return 0;
    void* p = MemAlloc(4);
    if (!p) return 0;                                    // (the surface leaks: FIX CANDIDATE)
    return dsurface_ctor(p, s3);
}
static void fp_ddraw_create_surface(Footprint& f, DDraw*, Edx, void*) { f.replay_only = "allocates the surface wrapper"; }
PORT_FN_GL(0x0045f3a0, "ddraw::create_surface", ddraw_create_surface_rw, fp_ddraw_create_surface)
static void __fastcall ddraw_destroy_surface_rw(DDraw*, Edx, void* s) {
    if (!s) return;
    dsurface_dtor(s);
    op_delete(s);
}
static void fp_ddraw_destroy_surface(Footprint& f, DDraw*, Edx, void* s) {
    if (s) f.replay_only = "frees the surface wrapper";
}
PORT_FN_GL(0x0045f3e0, "ddraw::destroy_surface", ddraw_destroy_surface_rw, fp_ddraw_destroy_surface)
static void* __fastcall ddraw_create_texture_surface_rw(DDraw* self, Edx, void* desc) {
    void* s3 = ddraw_make_dd_surface(self, desc);
    if (!s3) return 0;
    void* p = MemAlloc(0x10);
    if (!p) return 0;                                    // (the surface leaks: FIX CANDIDATE)
    return dtexture_ctor(p, s3);
}
static void fp_ddraw_create_texture(Footprint& f, DDraw*, Edx, void*) { f.replay_only = "allocates the texture wrapper"; }
PORT_FN_GL(0x0045f400, "ddraw::create_texture_surface", ddraw_create_texture_surface_rw, fp_ddraw_create_texture)
static void __fastcall ddraw_destroy_texture_surface_rw(DDraw*, Edx, void* t) {
    if (!t) return;
    dtexture_dtor(t);
    op_delete(t);
}
static void fp_ddraw_destroy_texture(Footprint& f, DDraw*, Edx, void* t) {
    if (t) f.replay_only = "frees the texture wrapper";
}
PORT_FN_GL(0x0045f440, "ddraw::destroy_texture_surface", ddraw_destroy_texture_surface_rw, fp_ddraw_destroy_texture)

// dsurface::dsurface (0x45f460), ~dsurface (0x45f470)
static DSurface* __fastcall dsurface_ctor_rw(DSurface* self, Edx, void* s3) {
    ((volatile DSurface*)self)->s = s3;
    return self;
}
static void fp_dsurface_ctor(Footprint& f, DSurface* self, Edx, void*) { f.add(self, sizeof(DSurface), "dsurface"); }
PORT_FN_GL(0x0045f460, "dsurface::dsurface", dsurface_ctor_rw, fp_dsurface_ctor)
static void __fastcall dsurface_dtor_rw(DSurface* self, Edx) {
    volatile DSurface* v = self;
    surface(v->s)->drop();
    v->s = 0;
}
static void fp_dsurface_self(Footprint& f, DSurface* self, Edx) { f.add(self, sizeof(DSurface), "dsurface"); }
PORT_FN_GL(0x0045f470, "dsurface::~dsurface", dsurface_dtor_rw, fp_dsurface_self)

// dsurface::lock (0x45f490): the pixels (the page, for the back buffer), again while the blitter is busy
static uint8_t __fastcall dsurface_lock_rw(DSurface* self, Edx, void* desc) {
    volatile DSurface* v = self;
    for (;;) {
        int32_t hr = lock(surface(v->s), (LPDDSURFACEDESC)desc);
        I32(X_RESULT) = hr;
        if (hr != 0 && (uint32_t)hr != (uint32_t)DDERR_WASSTILLDRAWING) {
            if ((uint32_t)hr == (uint32_t)DDERR_SURFACELOST) {
                LogReport(S(0x4f3d10));                  // "lost surface on lock!"
                return 0;
            }
            dx_error(0x4f3d28, I32(X_RESULT));           // "surface lock"
            return 0;
        }
        if (I32(X_RESULT) == 0) return 1;
    }
}
static void fp_dsurface_lock(Footprint& f, DSurface*, Edx, void* desc) {
    if (desc) f.add(desc, DDSD_BYTES, "DDSURFACEDESC");
    fp_result(f);
}
PORT_FN_GL(0x0045f490, "dsurface::lock", dsurface_lock_rw, fp_dsurface_lock)

// dsurface::unlock (0x45f510) (the result isn't kept)
static void __fastcall dsurface_unlock_rw(DSurface* self, Edx) { unlock(surface(((volatile DSurface*)self)->s)); }
static void fp_dsurface_none(Footprint&, DSurface*, Edx) {}
PORT_FN_GL(0x0045f510, "dsurface::unlock", dsurface_unlock_rw, fp_dsurface_none)

// dsurface::flip (0x45f520): the frame to the window; only a lost surface fails
static uint8_t __fastcall dsurface_flip_rw(DSurface*, Edx) {
    present();
    int32_t hr = DD_OK;
    I32(X_RESULT) = hr;
    if (hr != 0) {
        if ((uint32_t)hr == (uint32_t)DDERR_SURFACELOST) {
            LogReport(S(0x4f3d38));                      // "lost surface on flip!"
            return 0;
        }
    }
    return 1;
}
static void fp_dsurface_flip(Footprint& f, DSurface*, Edx) { fp_at(f, X_RESULT, 4, "dx_result"); }
PORT_FN_GL(0x0045f520, "dsurface::flip", dsurface_flip_rw, fp_dsurface_flip)

// dsurface::is_flip_done (0x45f580): always (a flip is done when present returns)
static uint8_t __fastcall dsurface_is_flip_done_rw(DSurface*, Edx) { return 1; }
PORT_FN_GL(0x0045f580, "dsurface::is_flip_done", dsurface_is_flip_done_rw, fp_dsurface_none)

// dsurface::get_desc (0x45f5a0)
static void __fastcall dsurface_get_desc_rw(DSurface* self, Edx, void* desc) {
    describe(surface(((volatile DSurface*)self)->s), (LPDDSURFACEDESC)desc);
    I32(X_RESULT) = DD_OK;
}
PORT_FN_GL(0x0045f5a0, "dsurface::get_desc", dsurface_get_desc_rw, fp_dsurface_lock)

// dsurface::get_attached (0x45f5d0): the back buffer (a reference held), in a new dsurface
static void* __fastcall dsurface_get_attached_rw(DSurface* self, Edx, void* caps) {
    Surface* out = 0;
    int32_t hr = attached(surface(((volatile DSurface*)self)->s), (const DDSCAPS*)caps, &out);
    I32(X_RESULT) = hr;
    if (hr != 0) {
        dx_error(0x4f3d70, hr);                          // "unable to get back buffer"
        return 0;
    }
    void* p = MemAlloc(4);
    if (!p) return 0;
    return dsurface_ctor(p, handle(out));
}
static void fp_dsurface_get_attached(Footprint& f, DSurface*, Edx, void*) { f.replay_only = "allocates the surface wrapper"; }
PORT_FN_GL(0x0045f5d0, "dsurface::get_attached", dsurface_get_attached_rw, fp_dsurface_get_attached)

// dsurface::add_attached (0x45f640): the z-buffer; the renderer's target has its own
static void __fastcall dsurface_add_attached_rw(DSurface*, Edx, DSurface*) { I32(X_RESULT) = DD_OK; }
static void fp_dsurface_add_attached(Footprint& f, DSurface*, Edx, DSurface*) { fp_result(f); }
PORT_FN_GL(0x0045f640, "dsurface::add_attached", dsurface_add_attached_rw, fp_dsurface_add_attached)

// dsurface::restore (0x45f670): nothing is ever lost
static void __fastcall dsurface_restore_rw(DSurface*, Edx) { I32(X_RESULT) = DD_OK; }
static void fp_dsurface_result(Footprint& f, DSurface*, Edx) { fp_result(f); }
PORT_FN_GL(0x0045f670, "dsurface::restore", dsurface_restore_rw, fp_dsurface_result)

// dsurface::blit (0x45f690): the whole of src
static void __fastcall dsurface_blit_rw(DSurface* self, Edx, DSurface* src) {
    blt(surface(((volatile DSurface*)self)->s), 0, surface(((volatile DSurface*)src)->s), 0);
    I32(X_RESULT) = DD_OK;
}
static void fp_dsurface_blit(Footprint& f, DSurface*, Edx, DSurface*) { fp_result(f); }
PORT_FN_GL(0x0045f690, "dsurface::blit", dsurface_blit_rw, fp_dsurface_blit)

// dsurface::blit_fast (0x45f6d0): src's rect at (x, y)
static void __fastcall dsurface_blit_fast_rw(DSurface* self, Edx, DSurface* src, uint32_t x, uint32_t y, void* rect) {
    blt_fast(surface(((volatile DSurface*)self)->s), x, y, surface(((volatile DSurface*)src)->s), (LPRECT)rect);
    I32(X_RESULT) = DD_OK;
}
static void fp_dsurface_blit_fast(Footprint& f, DSurface*, Edx, DSurface*, uint32_t, uint32_t, void*) { fp_result(f); }
PORT_FN_GL(0x0045f6d0, "dsurface::blit_fast", dsurface_blit_fast_rw, fp_dsurface_blit_fast)

// dsurface::set_colorkey (0x45f710): the source colour key {c, c}
static void __fastcall dsurface_set_colorkey_rw(DSurface* self, Edx, uint16_t c) {
    DDCOLORKEY key;
    key.dwColorSpaceLowValue = c;
    key.dwColorSpaceHighValue = c;
    set_key(surface(((volatile DSurface*)self)->s), &key);
    I32(X_RESULT) = DD_OK;
}
static void fp_dsurface_colorkey(Footprint& f, DSurface*, Edx, uint16_t) { fp_result(f); }
PORT_FN_GL(0x0045f710, "dsurface::set_colorkey", dsurface_set_colorkey_rw, fp_dsurface_colorkey)

// dtexture::dtexture (0x45f750): the surface, and its texture (the surface held once more)
static DTexture* __fastcall dtexture_ctor_rw(DTexture* self, Edx, void* s3) {
    dsurface_ctor(self, s3);
    volatile DTexture* v = self;
    v->first = 0;
    Surface* s = surface(s3);
    s->hold();
    I32(X_RESULT) = DD_OK;
    v->tex = handle(&s->t2);
    return self;
}
static void fp_dtexture_ctor(Footprint& f, DTexture* self, Edx, void*) {
    f.add(self, sizeof(DTexture), "dtexture");
    fp_result(f);
}
PORT_FN_GL(0x0045f750, "dtexture::dtexture", dtexture_ctor_rw, fp_dtexture_ctor)

// dtexture::~dtexture (0x45f7c0)
static void __fastcall dtexture_dtor_rw(DTexture* self, Edx) {
    volatile DTexture* v = self;
    texture_surface(v->tex)->drop();
    v->tex = 0;
    dsurface_dtor(self);
}
static void fp_dtexture_dtor(Footprint& f, DTexture* self, Edx) {
    f.add(&self->s, 4, "dtexture surface");
    f.add(&self->tex, 4, "dtexture IDirect3DTexture2");
}
PORT_FN_GL(0x0045f7c0, "dtexture::~dtexture", dtexture_dtor_rw, fp_dtexture_dtor)

// dtexture::load (0x45f7e0): every level from src, then this texture's handle
static void __fastcall dtexture_load_rw(DTexture* self, Edx, DTexture* src) {
    volatile DTexture* v = self;
    int32_t hr = tex_load(texture_surface(v->tex), texture_surface(((volatile DTexture*)src)->tex));
    I32(X_RESULT) = hr;
    if (hr != 0) {
        dx_error(0x4f3df8, I32(X_RESULT));               // "texture load"
        return;
    }
    uint32_t h = tex_handle(texture_surface(v->tex));
    I32(X_RESULT) = DD_OK;
    v->handle = h;
}
static void fp_dtexture_load(Footprint& f, DTexture* self, Edx, DTexture*) {
    f.add(&self->handle, 4, "dtexture handle");
    fp_result(f);
}
PORT_FN_GL(0x0045f7e0, "dtexture::load", dtexture_load_rw, fp_dtexture_load)

// dtexture::select() (0x45f870), select(u, v) (0x45f8a0): the texture (and its addressing) in the render state
static void __fastcall dtexture_select_rw(DTexture* self, Edx) {
    set_state(1, ((volatile DTexture*)self)->handle);    // TEXTUREHANDLE
    I32(X_RESULT) = DD_OK;
}
static void fp_dtexture_result(Footprint& f, DTexture*, Edx) { fp_result(f); }
PORT_FN_GL(0x0045f870, "dtexture::select()", dtexture_select_rw, fp_dtexture_result)
static void __fastcall dtexture_select_uv_rw(DTexture* self, Edx, uint8_t wrap_u, uint8_t wrap_v) {
    set_state(1, ((volatile DTexture*)self)->handle);
    I32(X_RESULT) = DD_OK;
    set_state(0x2c, wrap_u ? 1u : 3u);                   // ADDRESSU WRAP / CLAMP
    I32(X_RESULT) = DD_OK;
    set_state(0x2d, wrap_v ? 1u : 3u);                   // ADDRESSV
    I32(X_RESULT) = DD_OK;
}
static void fp_dtexture_select_uv(Footprint& f, DTexture*, Edx, uint8_t, uint8_t) { fp_result(f); }
PORT_FN_GL(0x0045f8a0, "dtexture::select(u,v)", dtexture_select_uv_rw, fp_dtexture_select_uv)

// dtexture::first_mip (0x45f930), next_mip (0x45f940), cleanup_mip (0x45f9a0): walking the mip chain by
// overwriting the surface pointer (the first level kept at +0xc)
static DTexture* __fastcall dtexture_first_mip_rw(DTexture* self, Edx) {
    volatile DTexture* v = self;
    v->first = v->s;
    return self;
}
static void fp_dtexture_first_mip(Footprint& f, DTexture* self, Edx) { f.add(&self->first, 4, "dtexture first level"); }
PORT_FN_GL(0x0045f930, "dtexture::first_mip", dtexture_first_mip_rw, fp_dtexture_first_mip)
static DTexture* __fastcall dtexture_next_mip_rw(DTexture* self, Edx) {
    DDSCAPS caps;
    caps.dwCaps = 0x401000;                              // DDSCAPS_TEXTURE | DDSCAPS_MIPMAP
    Surface* next = 0;
    volatile DTexture* v = self;
    int32_t hr = attached(surface(v->s), &caps, &next);
    I32(X_RESULT) = hr;
    if (hr != 0) {
        if ((uint32_t)hr != (uint32_t)DDERR_NOTFOUND) dx_error(0x4f3e58, hr);   // "get attached surface"
        return 0;
    }
    v->s = handle(next);
    return self;
}
static void fp_dtexture_next_mip(Footprint& f, DTexture* self, Edx) {
    f.add(&self->s, 4, "dtexture surface");
    fp_result(f);
}
PORT_FN_GL(0x0045f940, "dtexture::next_mip", dtexture_next_mip_rw, fp_dtexture_next_mip)
static void __fastcall dtexture_cleanup_mip_rw(DTexture* self, Edx) {   // (returns nothing: eax is left as it was)
    volatile DTexture* v = self;
    void* first = v->first;
    v->first = 0;
    v->s = first;
}
static void fp_dtexture_cleanup_mip(Footprint& f, DTexture* self, Edx) {
    f.add(&self->s, 4, "dtexture surface");
    f.add(&self->first, 4, "dtexture first level");
}
PORT_FN_GL(0x0045f9a0, "dtexture::cleanup_mip", dtexture_cleanup_mip_rw, fp_dtexture_cleanup_mip)

}  // namespace

// the draw wrappers' call (gx_dx.cpp): DrawPrimitive / DrawIndexedPrimitive, triangle lists
int32_t gfx_draw_triangles(uint32_t vtype, const void* v, uint32_t nv, const uint16_t* idx, uint32_t ni) {
    return gfx::draw(D3DPT_TRIANGLELIST, (D3DVERTEXTYPE)vtype, v, nv, idx, ni);
}
