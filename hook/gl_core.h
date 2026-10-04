// gl_core.h -- the renderer (M3 graphics, step G2): the game's DirectDraw / Direct3D world as OpenGL objects.
//
// Two ways in, one renderer:
//   * the game's own code (hook/gx_dd.cpp, the dd.obj wrappers rewritten) calls the operations below directly;
//   * the COM facade (ddraw_gl.cpp) serves the same objects as IDirectDraw2 / IDirectDrawSurface3 / ... to the
//     original code: the race.bin builds, `original` mode, and a shadow check's original pass.
// A handle the game keeps (the pointer in a ddraw / dsurface / dtexture / d3d / dviewport / dmaterial wrapper) is
// the object itself, so both ways see the same objects. What the renderer does -- textures, the page the 2D draws
// on, the render target, render state, draws, clears, present -- happens here, through gl_table.h.
//
//   screen    the render target is the window's full size. The game still thinks in its display mode (640x480...):
//             its viewports, clears and screen-space vertices are scaled into the 4:3 middle of the target, and a
//             viewport as wide as the game's screen spreads over the whole width with its clip window widened to
//             match (Hor+ widescreen: more to the sides, nothing cropped). Lock of the back buffer hands the game's
//             software 2D the middle, shrunk to its own resolution; Unlock lays back only the pixels the 2D changed,
//             so the 3D under the HUD stays at full resolution
//   3D        D3DLVERTEX / D3DTLVERTEX triangle lists through one shader that reproduces the D3D5 fixed-function
//             path the game uses: texture blend (decal / modulate / modulatealpha), specular add, vertex fog from the
//             specular alpha, colour key, alpha test, blending; the D3DVIEWPORT2 clip window mapped exactly; flat or
//             Gouraud shading
//   textures  every surface keeps its pixels in CPU memory (system and "video" copies alike); a video texture
//             becomes a GL texture on first use and re-uploads whenever a level changes. Nothing is ever lost, so
//             Alt-Tab no longer leaves garbage.
//
// Shadow checks (port.h): the renderer's state is part of what a check compares. Each object a pass changes is
// saved the first time (touch), put back between the passes, and its state after each pass compared; an object
// made in the original's pass is handed to the rewrite's pass when it makes the same kind (make); one released for
// good is deleted only when the check is over (drop). With gl_table.h's recorded calls, a check proves the rewrite
// leaves the renderer exactly as the original did and sends OpenGL exactly the same calls.
#pragma once
#include "dx5.h"
#include "com_base.h"
#include <stdint.h>
#include <vector>
#include <set>
#include "gl_table.h"

namespace gfx {

enum Format { F565, F555, F1555, F4444 };

// ---- objects ------------------------------------------------------------------------------------------------------
enum Kind : uint8_t { K_STATE, K_PAGE, K_HANDLES, K_DDRAW1, K_DDRAW, K_D3D, K_DEVICE, K_VIEWPORT, K_MATERIAL, K_SURFACE };

struct Obj {
    Kind kind;
    LONG refs = 1;
    explicit Obj(Kind k) : kind(k) {}
    virtual ~Obj();
    virtual void save(std::vector<uint8_t>& out) const;       // the state a check compares (refs and the fields)
    virtual void load(const uint8_t*& in);
    virtual ULONG hold();                                      // AddRef
    virtual ULONG drop();                                      // Release: at 0 the object goes (gfx::gone)
protected:
    virtual void save_fields(std::vector<uint8_t>&) const {}
    virtual void load_fields(const uint8_t*&) {}
};

struct Surface;
struct Texture2 : Base_IDirect3DTexture2 {                     // a texture surface's IDirect3DTexture2
    Surface* s;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetHandle(LPDIRECT3DDEVICE2, LPD3DTEXTUREHANDLE h) override;
    HRESULT STDMETHODCALLTYPE Load(LPDIRECT3DTEXTURE2 src) override;
    HRESULT STDMETHODCALLTYPE PaletteChanged(DWORD, DWORD) override;
};

// a surface: the primary, the back buffer (the page), the z-buffer, an offscreen image, a texture and its mip levels
struct Surface : Base_IDirectDrawSurface3, Obj {
    DDSCAPS caps = {};
    DWORD flags = 0;                     // DDSD_* it was created with
    int w = 0, h = 0, backbuffers = 0, mip_count = 1;
    Format fmt = F565;
    std::vector<uint16_t> pixels;
    Surface* root = 0;                   // the first level of a mip chain (itself for the root)
    Surface* next_mip = 0;
    Surface* back = 0;                   // a primary's back buffer
    bool is_back = false;
    bool keyed = false;
    uint16_t key = 0;
    uint32_t gen = 1;                    // bumped whenever the pixels change
    GLuint tex = 0;                      // the GL texture (a root that was ever drawn with)
    std::vector<uint32_t> uploaded;      // the gen of each level at its last upload
    Texture2 t2;

    static const Kind KIND = K_SURFACE;
    Surface() : Obj(KIND) { t2.s = this; }
    ~Surface();
    ULONG hold() override;
    ULONG drop() override;
    void save_fields(std::vector<uint8_t>&) const override;
    void load_fields(const uint8_t*&) override;

    // the COM facade (ddraw_gl.cpp)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC d) override;
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT pf) override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS c) override;
    HRESULT STDMETHODCALLTYPE IsLost() override;
    HRESULT STDMETHODCALLTYPE Restore() override;
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD f) override;
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD f) override;
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE3 a) override;
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD f, LPDIRECTDRAWSURFACE3 a) override;
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS c, LPDIRECTDRAWSURFACE3* out) override;
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD f, LPDDCOLORKEY k) override;
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD f, LPDDCOLORKEY k) override;
    HRESULT STDMETHODCALLTYPE Lock(LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e) override;
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID p) override;
    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE3 t, DWORD f) override;
    HRESULT STDMETHODCALLTYPE Blt(LPRECT dst, LPDIRECTDRAWSURFACE3 src, LPRECT srcr, DWORD f, LPDDBLTFX fx) override;
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 src, LPRECT srcr, DWORD f) override;
};

struct Material : Base_IDirect3DMaterial2, Obj {
    D3DMATERIAL m = {};
    static const Kind KIND = K_MATERIAL;
    Material() : Obj(KIND) {}
    void save_fields(std::vector<uint8_t>&) const override;
    void load_fields(const uint8_t*&) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SetMaterial(LPD3DMATERIAL p) override;
    HRESULT STDMETHODCALLTYPE GetMaterial(LPD3DMATERIAL p) override;
    HRESULT STDMETHODCALLTYPE GetHandle(LPDIRECT3DDEVICE2 dev, LPD3DMATERIALHANDLE h) override;
};

struct Viewport : Base_IDirect3DViewport2, Obj {
    D3DVIEWPORT2 vp = {};
    Material* background = 0;
    static const Kind KIND = K_VIEWPORT;
    Viewport() : Obj(KIND) {}
    void save_fields(std::vector<uint8_t>&) const override;
    void load_fields(const uint8_t*&) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SetViewport2(LPD3DVIEWPORT2 v) override;
    HRESULT STDMETHODCALLTYPE GetViewport2(LPD3DVIEWPORT2 v) override;
    HRESULT STDMETHODCALLTYPE SetBackground(D3DMATERIALHANDLE h) override;
    HRESULT STDMETHODCALLTYPE Clear(DWORD n, LPD3DRECT rects, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE TransformVertices(DWORD n, LPD3DTRANSFORMDATA d, DWORD flags, LPDWORD offscreen) override;
};

struct Device : Base_IDirect3DDevice2, Obj {
    Viewport* current = 0;
    static const Kind KIND = K_DEVICE;
    Device() : Obj(KIND) {}
    void save_fields(std::vector<uint8_t>&) const override;
    void load_fields(const uint8_t*&) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel) override;
    HRESULT STDMETHODCALLTYPE GetStats(LPD3DSTATS s) override;
    HRESULT STDMETHODCALLTYPE AddViewport(LPDIRECT3DVIEWPORT2 v) override;
    HRESULT STDMETHODCALLTYPE DeleteViewport(LPDIRECT3DVIEWPORT2 v) override;
    HRESULT STDMETHODCALLTYPE SetCurrentViewport(LPDIRECT3DVIEWPORT2 v) override;
    HRESULT STDMETHODCALLTYPE GetCurrentViewport(LPDIRECT3DVIEWPORT2* v) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override;
    HRESULT STDMETHODCALLTYPE EndScene() override;
    HRESULT STDMETHODCALLTYPE EnumTextureFormats(LPD3DENUMTEXTUREFORMATSCALLBACK cb, LPVOID ctx) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE s, LPDWORD v) override;
    HRESULT STDMETHODCALLTYPE SetLightState(D3DLIGHTSTATETYPE s, DWORD v) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE pt, D3DVERTEXTYPE vt, LPVOID v, DWORD n, DWORD f) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE pt, D3DVERTEXTYPE vt, LPVOID v, DWORD n, LPWORD idx,
                                                   DWORD ni, DWORD f) override;
};

struct Direct3D : Base_IDirect3D2, Obj {
    static const Kind KIND = K_D3D;
    Direct3D() : Obj(KIND) {}
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE CreateMaterial(LPDIRECT3DMATERIAL2* out, IUnknown*) override;
    HRESULT STDMETHODCALLTYPE CreateViewport(LPDIRECT3DVIEWPORT2* out, IUnknown*) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID c, LPDIRECTDRAWSURFACE sf, LPDIRECT3DDEVICE2* out) override;
};

struct DirectDraw2 : Base_IDirectDraw2, Obj {
    static const Kind KIND = K_DDRAW;
    DirectDraw2() : Obj(KIND) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS hal, LPDDCAPS hel) override;
    HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS caps, LPDWORD total, LPDWORD free) override;
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND w, DWORD f) override;
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, LPDDSURFACEDESC, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override;
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD f) override;
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC d) override;
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD f, HANDLE e) override;
    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out, IUnknown*) override;
};

struct DirectDraw1 : Base_IDirectDraw, Obj {                   // what DirectDrawCreate returns; the game QIs it at once
    static const Kind KIND = K_DDRAW1;
    DirectDraw1() : Obj(KIND) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
};

// a handle as the game keeps it -> the object
inline Surface* surface(void* p) { return static_cast<Surface*>((IDirectDrawSurface3*)p); }
inline Surface* texture_surface(void* p) { return p ? static_cast<Texture2*>((IDirect3DTexture2*)p)->s : 0; }
inline Material* material(void* p) { return static_cast<Material*>((IDirect3DMaterial2*)p); }
inline Viewport* viewport(void* p) { return static_cast<Viewport*>((IDirect3DViewport2*)p); }
inline Device* device(void* p) { return static_cast<Device*>((IDirect3DDevice2*)p); }
inline Direct3D* direct3d(void* p) { return static_cast<Direct3D*>((IDirect3D2*)p); }
inline DirectDraw2* directdraw(void* p) { return static_cast<DirectDraw2*>((IDirectDraw2*)p); }
// ... and back
inline void* handle(Surface* s) { return static_cast<IDirectDrawSurface3*>(s); }
inline void* handle(Texture2* t) { return static_cast<IDirect3DTexture2*>(t); }
inline void* handle(Material* m) { return static_cast<IDirect3DMaterial2*>(m); }
inline void* handle(Viewport* v) { return static_cast<IDirect3DViewport2*>(v); }
inline void* handle(Device* d) { return static_cast<IDirect3DDevice2*>(d); }
inline void* handle(Direct3D* d) { return static_cast<IDirect3D2*>(d); }
inline void* handle(DirectDraw2* d) { return static_cast<IDirectDraw2*>(d); }

// ---- the renderer's own state ---------------------------------------------------------------------------------------
struct State {                                   // what a check saves (K_STATE)
    int w = 640, h = 480;                        // the game's display mode
    int rt_w = 0, rt_h = 0;                      // the render target: the window's drawable size
    float scale = 1, ox = 0;                     // game pixels -> target pixels (x offset of the 4:3 middle)
    GLuint fbo = 0, fbo_color = 0, fbo_depth = 0;
    GLuint small_fbo = 0, small_color = 0;       // the middle of the target at the game's resolution
    bool page_locked = false;
    DWORD rs[64];                                // the device's render states
    D3DMATRIX world, view, proj;
    D3DVIEWPORT2 vp;                             // the current viewport's
    uint64_t frames = 0, draws = 0, triangles = 0;
};
extern State st;
struct Page {                                    // the back buffer as the game's 2D sees it (K_PAGE)
    std::vector<uint16_t> page;                  // (565)
    std::vector<uint16_t> under;                 // the page as it was handed out: what the 2D changed shows
    std::vector<uint32_t> overlay;               // the 2D's pixels, premultiplied RGBA
    std::vector<uint8_t> drawn3d;                // 1: 3D was drawn there this frame (display only: never saved or compared)
};
extern Page pg;
extern std::set<Surface*> handles;               // live texture surfaces, to validate texture handles (K_HANDLES)

// ---- shadow checks: the objects' state (above) ---------------------------------------------------------------------
void touch(Obj* o);                              // about to change o
void touch_state();
void touch_page();
void touch_handles();
Obj* made(Kind k);                               // (make's halves)
void born(Obj* o);
template <class T> T* make() {                   // a new object: in the rewrite's pass, the one the original's made
    if (Obj* o = made(T::KIND)) return static_cast<T*>(o);
    T* t = new T;
    born(t);
    return t;
}
void gone(Obj* o);                               // its last reference went: deleted, or at the check's end
void check_hooks_install();                      // tell port.cpp (shadow_set_state)

// ---- operations --------------------------------------------------------------------------------------------------
// the renderer's start (SetCooperativeLevel, CreateDevice); false if OpenGL 3.3 can't be had
bool start();
bool started();
void set_defaults();                             // the D3D5 device's initial render states
void set_mode(int w, int h);                     // SetDisplayMode: the game's own resolution
void present();                                  // Flip
void repaint();                                  // the picture shown again (switched away: the taskbar's preview)
void pixel_format(Format f, DDPIXELFORMAT& pf);
Format format_of(const DDPIXELFORMAT& pf);

// DirectDraw
void enum_drivers(LPDDENUMCALLBACKA cb, LPVOID ctx);   // DirectDrawEnumerateA: the one display
void dd_caps(LPDDCAPS hal, LPDDCAPS hel);
void vidmem(LPDDSCAPS caps, LPDWORD total, LPDWORD free);
void display_mode(LPDDSURFACEDESC d);
void enum_modes(LPVOID ctx, LPDDENUMMODESCALLBACK cb);
HRESULT create_surface(LPDDSURFACEDESC d, Surface** out);

// surfaces
void describe(Surface* s, LPDDSURFACEDESC d);
HRESULT lock(Surface* s, LPDDSURFACEDESC d);
HRESULT unlock(Surface* s);
HRESULT blt(Surface* dst, LPRECT dr, Surface* src, LPRECT sr);
HRESULT blt_fast(Surface* dst, DWORD x, DWORD y, Surface* src, LPRECT sr);
void set_key(Surface* s, const DDCOLORKEY* k);   // 0: no key
HRESULT attached(Surface* s, const DDSCAPS* caps, Surface** out);   // the back buffer (held) or the next mip level
HRESULT tex_load(Surface* dst, Surface* src);    // IDirect3DTexture2::Load: every level
D3DTEXTUREHANDLE tex_handle(Surface* s);

// Direct3D
HRESULT create_device(Device** out);
void device_caps(LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel);
void stats(LPD3DSTATS s);
void enum_formats(LPD3DENUMTEXTUREFORMATSCALLBACK cb, LPVOID ctx);
void set_state(DWORD s, DWORD v);
DWORD get_state(DWORD s);
void set_transform(DWORD t, const D3DMATRIX* m);
void get_transform(DWORD t, D3DMATRIX* m);
void remove_viewport(Device* d, Viewport* v);
void use_viewport(Device* d, Viewport* v);
void set_viewport(Viewport* v, const D3DVIEWPORT2* vp);
void set_background(Viewport* v, D3DMATERIALHANDLE h);
void clear(Viewport* v, DWORD n, const D3DRECT* rects, DWORD flags);
void transform(Viewport* v, DWORD n, LPD3DTRANSFORMDATA d, DWORD flags, LPDWORD offscreen);
void set_material(Material* m, const D3DMATERIAL* p);
D3DMATERIALHANDLE material_handle(Material* m);
HRESULT draw(D3DPRIMITIVETYPE pt, D3DVERTEXTYPE vt, const void* v, DWORD n, const WORD* idx, DWORD ni);

void report();                                   // the exit log's line

// ---- for the harnesses ---------------------------------------------------------------------------------------------
bool start_headless();                           // test/world_gx_dd.cpp: started on gl_api's fakes, no window (gl_core.cpp)

}  // namespace gfx
