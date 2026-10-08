// viperport -- the game's DirectDraw and Direct3D, served from the OpenGL renderer (gl_core.h): the COM facade.
//
// race.exe's DirectDrawCreate / DirectDrawEnumerateA imports are pointed here, and the game gets objects that
// implement exactly the DirectX 5 methods it calls (traced from its 2D, 3D and texture code); everything else
// is a generated "unsupported" method that logs its first call (com_base.h). Each method is the renderer's
// operation of the same name. This is how every build reaches the renderer (v1.0, v1.1, 1.2.x) in `original` mode,
// and how a shadow check's original pass does; the rewritten dd.obj wrappers (gx_dd.cpp) call the renderer
// directly, on the same objects.
#define _CRT_SECURE_NO_WARNINGS
#include "gl_core.h"
#include "vp_os.h"
#include <string.h>
#include "viperport.h"
#include "port.h"

void com_unsupported(const char* method) {
    static std::set<const char*> seen;
    if (seen.insert(method).second) logf("ddraw_gl: the game called %s, which isn't emulated", method);
}

namespace gfx {

// ---- surfaces ---------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Surface::QueryInterface(REFIID riid, LPVOID* out) {
    if (riid == IID_IDirect3DTexture2) { *out = &t2; hold(); return DD_OK; }
    if (riid == IID_IDirectDrawSurface || riid == IID_IDirectDrawSurface2 || riid == IID_IDirectDrawSurface3 ||
        riid == IID_IUnknown) { *out = handle(this); hold(); return DD_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE Surface::AddRef() { return hold(); }
ULONG STDMETHODCALLTYPE Surface::Release() { return drop(); }
HRESULT STDMETHODCALLTYPE Surface::GetSurfaceDesc(LPDDSURFACEDESC d) { describe(this, d); return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::GetPixelFormat(LPDDPIXELFORMAT pf) { pixel_format(fmt, *pf); return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::GetCaps(LPDDSCAPS c) { *c = caps; return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::IsLost() { return DD_OK; }             // nothing is ever lost
HRESULT STDMETHODCALLTYPE Surface::Restore() { return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::GetFlipStatus(DWORD) { return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::GetBltStatus(DWORD) { return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::AddAttachedSurface(LPDIRECTDRAWSURFACE3) { return DD_OK; }   // the z-buffer
HRESULT STDMETHODCALLTYPE Surface::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE3) { return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::GetAttachedSurface(LPDDSCAPS c, LPDIRECTDRAWSURFACE3* out) {
    Surface* s = 0;
    HRESULT hr = attached(this, c, &s);
    *out = s ? (LPDIRECTDRAWSURFACE3)handle(s) : 0;
    return hr;
}
HRESULT STDMETHODCALLTYPE Surface::SetColorKey(DWORD, LPDDCOLORKEY k) { set_key(this, k); return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::GetColorKey(DWORD, LPDDCOLORKEY k) {
    if (!keyed) return DDERR_NOCOLORKEY;
    k->dwColorSpaceLowValue = k->dwColorSpaceHighValue = key;
    return DD_OK;
}
HRESULT STDMETHODCALLTYPE Surface::Lock(LPRECT, LPDDSURFACEDESC d, DWORD, HANDLE) { return lock(this, d); }
HRESULT STDMETHODCALLTYPE Surface::Unlock(LPVOID) { return unlock(this); }
HRESULT STDMETHODCALLTYPE Surface::Flip(LPDIRECTDRAWSURFACE3, DWORD) { present(); return DD_OK; }
HRESULT STDMETHODCALLTYPE Surface::Blt(LPRECT dst, LPDIRECTDRAWSURFACE3 src, LPRECT srcr, DWORD, LPDDBLTFX) {
    return blt(this, dst, src ? surface(src) : 0, srcr);
}
HRESULT STDMETHODCALLTYPE Surface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 src, LPRECT srcr, DWORD) {
    return blt_fast(this, x, y, surface(src), srcr);
}

HRESULT STDMETHODCALLTYPE Texture2::QueryInterface(REFIID riid, LPVOID* out) { return s->QueryInterface(riid, out); }
ULONG STDMETHODCALLTYPE Texture2::AddRef() { return s->AddRef(); }
ULONG STDMETHODCALLTYPE Texture2::Release() { return s->Release(); }
HRESULT STDMETHODCALLTYPE Texture2::GetHandle(LPDIRECT3DDEVICE2, LPD3DTEXTUREHANDLE h) { *h = tex_handle(s); return DD_OK; }
HRESULT STDMETHODCALLTYPE Texture2::Load(LPDIRECT3DTEXTURE2 src) { return tex_load(s, texture_surface(src)); }
HRESULT STDMETHODCALLTYPE Texture2::PaletteChanged(DWORD, DWORD) { return DD_OK; }

// ---- Direct3D -----------------------------------------------------------------------------------------------------------
ULONG STDMETHODCALLTYPE Material::AddRef() { return hold(); }
ULONG STDMETHODCALLTYPE Material::Release() { return drop(); }
HRESULT STDMETHODCALLTYPE Material::SetMaterial(LPD3DMATERIAL p) { set_material(this, p); return DD_OK; }
HRESULT STDMETHODCALLTYPE Material::GetMaterial(LPD3DMATERIAL p) { *p = m; return DD_OK; }
HRESULT STDMETHODCALLTYPE Material::GetHandle(LPDIRECT3DDEVICE2, LPD3DMATERIALHANDLE h) { *h = material_handle(this); return DD_OK; }

ULONG STDMETHODCALLTYPE Viewport::AddRef() { return hold(); }
ULONG STDMETHODCALLTYPE Viewport::Release() { return drop(); }
HRESULT STDMETHODCALLTYPE Viewport::SetViewport2(LPD3DVIEWPORT2 v) { set_viewport(this, v); return DD_OK; }
HRESULT STDMETHODCALLTYPE Viewport::GetViewport2(LPD3DVIEWPORT2 v) { *v = vp; return DD_OK; }
HRESULT STDMETHODCALLTYPE Viewport::SetBackground(D3DMATERIALHANDLE h) { set_background(this, h); return DD_OK; }
HRESULT STDMETHODCALLTYPE Viewport::Clear(DWORD n, LPD3DRECT rects, DWORD flags) { clear(this, n, rects, flags); return DD_OK; }
HRESULT STDMETHODCALLTYPE Viewport::TransformVertices(DWORD n, LPD3DTRANSFORMDATA d, DWORD flags, LPDWORD offscreen) {
    transform(this, n, d, flags, offscreen);
    return DD_OK;
}

ULONG STDMETHODCALLTYPE Device::AddRef() { return hold(); }
ULONG STDMETHODCALLTYPE Device::Release() { return drop(); }
HRESULT STDMETHODCALLTYPE Device::GetCaps(LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel) { device_caps(hal, hel); return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::GetStats(LPD3DSTATS s) { stats(s); return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::AddViewport(LPDIRECT3DVIEWPORT2) { return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::DeleteViewport(LPDIRECT3DVIEWPORT2 v) { remove_viewport(this, viewport(v)); return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::SetCurrentViewport(LPDIRECT3DVIEWPORT2 v) { use_viewport(this, v ? viewport(v) : 0); return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::GetCurrentViewport(LPDIRECT3DVIEWPORT2* v) {
    *v = current ? (LPDIRECT3DVIEWPORT2)handle(current) : 0;
    return DD_OK;
}
HRESULT STDMETHODCALLTYPE Device::BeginScene() { return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::EndScene() { return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::EnumTextureFormats(LPD3DENUMTEXTUREFORMATSCALLBACK cb, LPVOID ctx) {
    enum_formats(cb, ctx);
    return DD_OK;
}
HRESULT STDMETHODCALLTYPE Device::SetRenderState(D3DRENDERSTATETYPE s, DWORD v) { set_state((DWORD)s, v); return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::GetRenderState(D3DRENDERSTATETYPE s, LPDWORD v) { *v = get_state((DWORD)s); return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::SetLightState(D3DLIGHTSTATETYPE, DWORD) { return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::SetTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m) { set_transform((DWORD)t, m); return DD_OK; }
HRESULT STDMETHODCALLTYPE Device::GetTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m) { get_transform((DWORD)t, m); return DD_OK; }
// every vertex type the game draws is 32 bytes (D3DVERTEX, D3DLVERTEX, D3DTLVERTEX)
HRESULT STDMETHODCALLTYPE Device::DrawPrimitive(D3DPRIMITIVETYPE pt, D3DVERTEXTYPE vt, LPVOID v, DWORD n, DWORD) {
    return draw(pt, vt, v, n, 0, 0);
}
HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE pt, D3DVERTEXTYPE vt, LPVOID v, DWORD n, LPWORD idx,
                                                       DWORD ni, DWORD) {
    return draw(pt, vt, v, n, idx, ni);
}

ULONG STDMETHODCALLTYPE Direct3D::AddRef() { return hold(); }
ULONG STDMETHODCALLTYPE Direct3D::Release() { return drop(); }
HRESULT STDMETHODCALLTYPE Direct3D::CreateMaterial(LPDIRECT3DMATERIAL2* out, IUnknown*) { *out = make<Material>(); return DD_OK; }
HRESULT STDMETHODCALLTYPE Direct3D::CreateViewport(LPDIRECT3DVIEWPORT2* out, IUnknown*) { *out = make<Viewport>(); return DD_OK; }
HRESULT STDMETHODCALLTYPE Direct3D::CreateDevice(REFCLSID, LPDIRECTDRAWSURFACE, LPDIRECT3DDEVICE2* out) {
    Device* d = 0;
    HRESULT hr = create_device(&d);
    *out = d;
    return hr;
}

// ---- DirectDraw ---------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE DirectDraw2::QueryInterface(REFIID riid, LPVOID* out) {
    if (riid == IID_IDirect3D2) { *out = static_cast<IDirect3D2*>(make<Direct3D>()); return DD_OK; }
    if (riid == IID_IDirectDraw2 || riid == IID_IUnknown) { *out = handle(this); hold(); return DD_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE DirectDraw2::AddRef() { return hold(); }
ULONG STDMETHODCALLTYPE DirectDraw2::Release() { return drop(); }
HRESULT STDMETHODCALLTYPE DirectDraw2::GetCaps(LPDDCAPS hal, LPDDCAPS hel) { dd_caps(hal, hel); return DD_OK; }
HRESULT STDMETHODCALLTYPE DirectDraw2::GetAvailableVidMem(LPDDSCAPS caps, LPDWORD total, LPDWORD free) {
    vidmem(caps, total, free);
    return DD_OK;
}
HRESULT STDMETHODCALLTYPE DirectDraw2::SetCooperativeLevel(HWND, DWORD) { return start() ? DD_OK : DDERR_GENERIC; }
HRESULT STDMETHODCALLTYPE DirectDraw2::EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID ctx, LPDDENUMMODESCALLBACK cb) {
    enum_modes(ctx, cb);
    return DD_OK;
}
HRESULT STDMETHODCALLTYPE DirectDraw2::SetDisplayMode(DWORD w, DWORD h, DWORD, DWORD, DWORD) {
    set_mode((int)w, (int)h);
    return DD_OK;
}
HRESULT STDMETHODCALLTYPE DirectDraw2::RestoreDisplayMode() { return DD_OK; }
HRESULT STDMETHODCALLTYPE DirectDraw2::GetDisplayMode(LPDDSURFACEDESC d) { display_mode(d); return DD_OK; }
HRESULT STDMETHODCALLTYPE DirectDraw2::WaitForVerticalBlank(DWORD, HANDLE) { return DD_OK; }
HRESULT STDMETHODCALLTYPE DirectDraw2::CreateSurface(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out, IUnknown*) {
    Surface* s = 0;
    HRESULT hr = create_surface(d, &s);
    *out = s ? (LPDIRECTDRAWSURFACE)handle(s) : 0;
    return hr;
}

HRESULT STDMETHODCALLTYPE DirectDraw1::QueryInterface(REFIID riid, LPVOID* out) {
    if (riid == IID_IDirectDraw2) { *out = handle(make<DirectDraw2>()); return DD_OK; }
    if (riid == IID_IDirectDraw || riid == IID_IUnknown) { *out = static_cast<IDirectDraw*>(this); hold(); return DD_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE DirectDraw1::AddRef() { return hold(); }
ULONG STDMETHODCALLTYPE DirectDraw1::Release() { return drop(); }

}  // namespace gfx

namespace {

HRESULT WINAPI emu_DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
    *out = gfx::make<gfx::DirectDraw1>();
    return DD_OK;
}

HRESULT WINAPI emu_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    gfx::enum_drivers(cb, ctx);
    return DD_OK;
}

}  // namespace

// point the main module's imports of `dll!name` at `to`
bool patch_import(const char* dll, const char* name, void* to) {
    uint8_t* base = (uint8_t*)vpos_GetModuleHandleA(0);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, name) != 0) continue;
            DWORD old;
            vpos_VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD)(uintptr_t)to;
            vpos_VirtualProtect(&iat->u1.Function, 4, old, &old);
            return true;
        }
    }
    return false;
}

// ---- switching it on -------------------------------------------------------------------------------------------
bool renderer_install() {
    bool a = patch_import("DDRAW.dll", "DirectDrawCreate", (void*)emu_DirectDrawCreate);
    bool b = patch_import("DDRAW.dll", "DirectDrawEnumerateA", (void*)emu_DirectDrawEnumerateA);
    if (!a || !b) {
        logf("renderer: NOT switching to OpenGL -- the DDRAW imports weren't found (DirectDrawCreate %d, Enumerate %d)", a, b);
        return false;
    }
    gfx::check_hooks_install();
    logf("renderer: OpenGL 3.3 (DirectDraw and Direct3D served from it)");
    return true;
}

void renderer_report() { gfx::report(); }
