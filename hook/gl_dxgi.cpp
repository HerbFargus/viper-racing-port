// gl_dxgi.cpp -- the renderer's picture presented through a DXGI flip-model swap chain (gl_dxgi.h).
//
// The frame: wglDXLockObjectsNV on the shared texture, blit the render target into it (OpenGL's row 0 of a shared
// D3D texture is D3D's row 0, the top, and the target's row 0 is the top of the picture too, so unlike the blit into
// the window's own framebuffer there is no flip), unlock, CopyResource into the swap chain's back buffer, Present(1, 0)
// (vsync, as SDL_GL_SetSwapInterval(1)). The texture follows the render target's size (ResizeBuffers, re-registered).
#define _CRT_SECURE_NO_WARNINGS
#include "gl_dxgi.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include "SDL_syswm.h"
#include "viperport.h"

namespace dxgi {
namespace {

// WGL_NV_DX_interop2 (wglext.h isn't in the SDK)
typedef HANDLE(WINAPI* OpenDevice_t)(void* dx_device);
typedef BOOL(WINAPI* CloseDevice_t)(HANDLE device);
typedef HANDLE(WINAPI* RegisterObject_t)(HANDLE device, void* dx_object, GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI* UnregisterObject_t)(HANDLE device, HANDLE object);
typedef BOOL(WINAPI* LockObjects_t)(HANDLE device, GLint count, HANDLE* objects);
typedef const char*(WINAPI* GetExtensionsString_t)(HDC);
typedef HDC(WINAPI* GetCurrentDC_t)(void);
constexpr GLenum WGL_ACCESS_WRITE_DISCARD_NV = 0x0002;

struct {
    bool on = false;                             // presenting through the swap chain
    OpenDevice_t OpenDevice; CloseDevice_t CloseDevice; RegisterObject_t RegisterObject;
    UnregisterObject_t UnregisterObject; LockObjects_t LockObjects, UnlockObjects;
    HMODULE d3d11 = 0;
    ID3D11Device* dev = 0;
    ID3D11DeviceContext* ctx = 0;
    IDXGISwapChain1* sc = 0;
    ID3D11Texture2D* tex = 0;                    // the shared texture, at the render target's size
    HANDLE gl_dev = 0, gl_obj = 0;
    GLuint rb = 0, fbo = 0;                      // the shared texture as an OpenGL renderbuffer, on a framebuffer
    int w = 0, h = 0;                            // the swap chain's and the shared texture's size
    bool occluded = false;                       // the last Present said nothing of the window shows
} d;

template <class T> void release(T*& p) { if (p) p->Release(), p = 0; }

void drop_texture() {
    if (d.gl_obj) d.UnregisterObject(d.gl_dev, d.gl_obj), d.gl_obj = 0;
    release(d.tex);
}

// everything let go; from here SDL_GL_SwapWindow presents (the window can take a GL present again once the swap
// chain is really gone, hence ClearState + Flush: D3D11 destroys it lazily otherwise)
void stop() {
    drop_texture();
    if (d.fbo) gl_api.DeleteFramebuffers(1, &d.fbo), d.fbo = 0;
    if (d.rb) gl_api.DeleteRenderbuffers(1, &d.rb), d.rb = 0;
    if (d.gl_dev) d.CloseDevice(d.gl_dev), d.gl_dev = 0;
    release(d.sc);
    if (d.ctx) d.ctx->ClearState(), d.ctx->Flush();
    release(d.ctx);
    release(d.dev);
    d.on = false;
}

void fail(const char* what, long hr) {
    logf("renderer: DXGI present: %s (0x%08lx); presenting through SDL_GL_SwapWindow from here", what, hr);
    stop();
}

// the swap chain and the shared texture at w x h; leaves draw framebuffer 0 bound
bool make_texture(int w, int h) {
    drop_texture();
    if (w != d.w || h != d.h) {
        HRESULT hr = d.sc->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return fail("ResizeBuffers failed", hr), false;
        d.w = w, d.h = h;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w, td.Height = (UINT)h, td.MipLevels = 1, td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = d.dev->CreateTexture2D(&td, 0, &d.tex);
    if (FAILED(hr)) return fail("CreateTexture2D failed", hr), false;
    if (!d.rb) {
        gl_api.GenRenderbuffers(1, &d.rb);
        gl_api.BindRenderbuffer(GL_RENDERBUFFER, d.rb);   // (a renderbuffer exists once bound)
        gl_api.BindRenderbuffer(GL_RENDERBUFFER, 0);
        gl_api.GenFramebuffers(1, &d.fbo);
    }
    d.gl_obj = d.RegisterObject(d.gl_dev, d.tex, d.rb, GL_RENDERBUFFER, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!d.gl_obj) return fail("wglDXRegisterObjectNV failed", (long)GetLastError()), false;
    if (!d.LockObjects(d.gl_dev, 1, &d.gl_obj)) return fail("wglDXLockObjectsNV failed", (long)GetLastError()), false;
    gl_api.BindFramebuffer(GL_DRAW_FRAMEBUFFER, d.fbo);
    gl_api.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, d.rb);
    GLenum status = gl_api.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    gl_api.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    d.UnlockObjects(d.gl_dev, 1, &d.gl_obj);
    if (status != GL_FRAMEBUFFER_COMPLETE) return fail("the shared texture's framebuffer is incomplete", (long)status), false;
    return true;
}

}  // namespace

bool active() { return d.on; }

bool start(SDL_Window* win) {
    if (d.on) return true;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version")) {
        logf("renderer: presenting through SDL_GL_SwapWindow (Wine)");
        return false;
    }
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");
    GetCurrentDC_t current_dc = gl32 ? (GetCurrentDC_t)GetProcAddress(gl32, "wglGetCurrentDC") : 0;
    GetExtensionsString_t ext_string = (GetExtensionsString_t)SDL_GL_GetProcAddress("wglGetExtensionsStringARB");
    const char* ext = current_dc && ext_string ? ext_string(current_dc()) : 0;
    if (!ext || !strstr(ext, "WGL_NV_DX_interop2")) {
        logf("renderer: presenting through SDL_GL_SwapWindow (no WGL_NV_DX_interop2)");
        return false;
    }
    *(void**)&d.OpenDevice = SDL_GL_GetProcAddress("wglDXOpenDeviceNV");
    *(void**)&d.CloseDevice = SDL_GL_GetProcAddress("wglDXCloseDeviceNV");
    *(void**)&d.RegisterObject = SDL_GL_GetProcAddress("wglDXRegisterObjectNV");
    *(void**)&d.UnregisterObject = SDL_GL_GetProcAddress("wglDXUnregisterObjectNV");
    *(void**)&d.LockObjects = SDL_GL_GetProcAddress("wglDXLockObjectsNV");
    *(void**)&d.UnlockObjects = SDL_GL_GetProcAddress("wglDXUnlockObjectsNV");
    if (!d.OpenDevice || !d.CloseDevice || !d.RegisterObject || !d.UnregisterObject || !d.LockObjects || !d.UnlockObjects) {
        logf("renderer: presenting through SDL_GL_SwapWindow (WGL_NV_DX_interop2's functions missing)");
        return false;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        logf("renderer: presenting through SDL_GL_SwapWindow (no window handle)");
        return false;
    }
    HWND hwnd = info.info.win.window;
    if (!d.d3d11) d.d3d11 = LoadLibraryA("d3d11.dll");
    PFN_D3D11_CREATE_DEVICE create = d.d3d11 ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d.d3d11, "D3D11CreateDevice") : 0;
    if (!create) {
        logf("renderer: presenting through SDL_GL_SwapWindow (no d3d11.dll)");
        return false;
    }
    d.on = true;                                 // (from here a failure goes through fail, which lets it all go)
    HRESULT hr = create(0, D3D_DRIVER_TYPE_HARDWARE, 0, D3D11_CREATE_DEVICE_BGRA_SUPPORT, 0, 0, D3D11_SDK_VERSION,
                        &d.dev, 0, &d.ctx);
    if (FAILED(hr)) return fail("D3D11CreateDevice failed", hr), false;
    IDXGIDevice* xdev = 0;
    IDXGIAdapter* adapter = 0;
    IDXGIFactory2* factory = 0;
    hr = d.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xdev);
    if (SUCCEEDED(hr)) hr = xdev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};               // the window's client size (Width, Height 0)
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForHwnd(d.dev, hwnd, &sd, 0, 0, &d.sc);
        // the window is SDL's: DXGI leaves Alt+Enter and the window's size alone
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    release(factory);
    release(adapter);
    release(xdev);
    if (FAILED(hr)) return fail("no flip-model swap chain on the window", hr), false;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    d.sc->GetDesc1(&sd);
    d.w = (int)sd.Width, d.h = (int)sd.Height;
    d.gl_dev = d.OpenDevice(d.dev);
    if (!d.gl_dev) return fail("wglDXOpenDeviceNV failed", (long)GetLastError()), false;
    if (!make_texture(d.w, d.h)) return false;
    logf("renderer: presenting through a DXGI flip-model swap chain (WGL_NV_DX_interop2), %dx%d", d.w, d.h);
    return true;
}

void present(SDL_Window* win, GLuint src_fbo, int w, int h) {
    if (d.on && w > 0 && h > 0 && (w != d.w || h != d.h || !d.tex)) make_texture(w, h);
    if (!d.on || w <= 0 || h <= 0) {
        SDL_GL_SwapWindow(win);
        return;
    }
    if (!d.LockObjects(d.gl_dev, 1, &d.gl_obj)) {
        fail("wglDXLockObjectsNV failed", (long)GetLastError());
        SDL_GL_SwapWindow(win);
        return;
    }
    gl_api.BindFramebuffer(GL_DRAW_FRAMEBUFFER, d.fbo);
    gl_api.BindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    gl_api.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl_api.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    d.UnlockObjects(d.gl_dev, 1, &d.gl_obj);
    ID3D11Texture2D* back = 0;
    HRESULT hr = d.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (FAILED(hr)) {
        fail("GetBuffer failed", hr);
        SDL_GL_SwapWindow(win);
        return;
    }
    d.ctx->CopyResource(back, d.tex);
    back->Release();
    // minimised or covered: only a test present until the window shows again (DXGI_STATUS_OCCLUDED is a success
    // code -- the swap chain is kept, nothing is lost)
    if (d.occluded) {
        hr = d.sc->Present(0, DXGI_PRESENT_TEST);
        if (hr == DXGI_STATUS_OCCLUDED) return;
        d.occluded = false;
    }
    hr = d.sc->Present(1, 0);
    if (hr == DXGI_STATUS_OCCLUDED) d.occluded = true;
    else if (FAILED(hr)) {
        fail(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ? "the device was lost" : "Present failed",
             hr == DXGI_ERROR_DEVICE_REMOVED && d.dev ? (long)d.dev->GetDeviceRemovedReason() : hr);
        SDL_GL_SwapWindow(win);                  // (the window's own framebuffer has this frame: present() blitted it)
    }
}

}  // namespace dxgi
