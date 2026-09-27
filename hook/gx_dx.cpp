// gx_dx.cpp -- M3 graphics stage, step G1, group G1d: the DirectX layer, rewritten faithfully.
//
//   vid.obj      the screen: DirectDraw's creation (the 3D device hunt over DirectDrawEnumerate, the mode list, the
//                video memory tiers), the display mode, the primary flip chain / back buffer / z-buffer, the page
//                lock for the 2D (VidGetPage / VidReleasePage), VidFlip, the lost-surface restore, the getters
//   dx.obj       the Direct3D front end: the device and its viewport and background material, the caps test and
//                convert_caps, the texture / vertex-alpha cache in front of dxStateFlush (dxDPBegin,
//                dxBeginTriangles), the five draw wrappers (straight on the device's vtable), dxProjectPoint,
//                dxGetCounters, dx_error / dx_name_error
//   dxstate.obj  the render-state cache: dxStateInit / Update / Flush and the one-state setters
//
// dd.obj, the thin C++ wrappers around the DirectX objects, is gx_dd.cpp: step G2 put them on the OpenGL renderer,
// and the five draw wrappers here call it too (gx_gl.h). Everything else reaches DirectX through those wrappers,
// called by address, exactly as the original does. Every other game function -- this group's own included -- is called
// by its v1.0 address, so a hooked rewrite is what runs; the game's CRT (sprintf, strstr) too, and ExitProcess
// through the game's import slot. Floats that the original moves with integer instructions (the viewport colour,
// the LOD bias, dxProjectPoint's point) are moved as bits.
//
// Not rewritten: the $E static initialisers of the four objects (they run from the CRT's _initterm before any
// hook is installed). Everything else in the list is here, the bare `ret`s (dxDPEnd, dxEndTriangles,
// dxStateClear) included: the linker's padding after them leaves room for the hook's jmp.
//
// Footprints: all of this runs on the MAIN thread, so each lists every static it writes. The DirectX calls
// themselves need no footprint (the emulation records the rewrite's calls and hands it the original's results);
// what they write into the game's memory -- a Lock's or GetSurfaceDesc's descriptor, GetCaps' descriptions,
// TransformVertices' output vertices -- is listed. replay_only: whatever allocates or frees (MemAlloc / operator
// delete: the wrappers), whatever enumerates with a callback into the game (DirectDrawEnumerate,
// EnumDisplayModes, EnumTextureFormats: the callback's own writes aren't replayed to the rewrite's pass),
// whatever pumps the window's messages (Win32Idle) or reads the clock (PTimeNow: VidGetPage / VidFlip's timing
// statistics), and dxDPBegin / dxBeginTriangles when they call TextureSelect (which may load a texture).
//
// FIX CANDIDATES (faithful here; see the report):
//   * VidSetMode / VidIsModeSupported index mode_ok[6] with the caller's mode unchecked.
//   * find_mem, VidDumpMem, dxProjectPoint, d3d_test_caps read locals the DirectX call didn't write when it fails
//     (the video memory totals, the offscreen flag, the converted caps when convert_caps rejects the device): the
//     original reads stack garbage; here they start at 0 (find_mem, VidDumpMem, dxProjectPoint) or are left
//     uninitialised as in the original (d3d_test_caps' converted caps).
//   * dtexture::dtexture leaves +4 (the IDirect3DTexture2) and +8 (the handle) as MemAlloc left them when the
//     QueryInterface fails; ddraw::create_surface / create_texture_surface leak the surface when MemAlloc fails.
//   * dviewport::set_viewport divides by the width (w = 0: an infinite / NaN clip window).
//   * dviewport::dviewport doesn't clear its pointer when CreateViewport fails.
//   * VidGetPage / restore_surfaces / VidFlip use the primary / back / z-buffer pointers unchecked (null after a
//     failed mode set).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "gx_gl.h"

namespace {
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

// ---- statics (v1.0) -----------------------------------------------------------------------------------------------
enum : uint32_t {
    // vid.obj
    V_DD = 0x4f1e88,            // ddraw* dd
    V_PRIMARY = 0x4f1e8c,       // dsurface* primary
    V_BACK = 0x4f1e90,          // dsurface* back (the page the 2D locks)
    V_3D = 0x4f1e94,            // dsurface* vid_3d (the render target: = back)
    V_ZBUF = 0x4f1e98,          // dsurface* z-buffer
    V_HAS3DHW = 0x4f1e9c,       // byte
    V_NO_SECONDARY = 0x4f1ea0,  // byte (VidDisableSecondary)
    V_ACTIVE = 0x4f1ea4,        // byte (1 unless _VidActivate(0))
    V_CUR_MODE = 0x4f1ea8,      // int
    V_INITED = 0x4f1eac,        // byte
    V_WAS_LOST = 0x4f1eb0,      // byte
    V_LOCKS = 0x4f1eb4,         // int x4: locks, locks that took time, their total time, the longest
    V_LOCK_WAITS = 0x4f1eb8, V_LOCK_TIME = 0x4f1ebc, V_LOCK_MAX = 0x4f1ec0,
    V_FLIPS = 0x4f1ec4,         // int x4: the same for flips
    V_FLIP_WAITS = 0x4f1ec8, V_FLIP_TIME = 0x4f1ecc, V_FLIP_MAX = 0x4f1ed0,
    V_FIND_MEM = 0x4f1ed4,      // byte: find_mem still to run
    V_PAGES = 0x522ac8,         // int page count
    V_MEGS = 0x522ad4,          // int video memory tier
    V_SCREEN_HIT = 0x522ae4, V_IS3DFX = 0x522ae8,
    V_MODE_OK = 0x522af8,       // byte[6]
    V_AGP = 0x522b00, V_SCREEN_WID = 0x522b08, V_SCREEN_FMT = 0x522b20,
    // dx.obj
    X_MATERIAL = 0x4f2314,      // dmaterial* (the viewport background)
    X_VIEWPORT = 0x4f2318,      // dviewport*
    X_COUNT1 = 0x4f231c, X_COUNT2 = 0x4f2320,
    X_VERTEX_ALPHA = 0x4f2324,  // byte
    X_TEXTURE = 0x4f2328,       // int: the texture the last Begin selected (-1 none)
    X_ALPHA_DIRTY = 0x4f232c,   // byte
    X_CTX = 0x522e60,           // byte: the device is connected
    X_D3D = 0x522e64,           // d3d*
    X_RESULT = 0x522e6c,        // HRESULT dx_result
    X_ERRBUF = 0x522e78,        // char[]: dx_name_error's "unknown error: %x"
    X_IN = 0x522f0c,            // D3DVERTEX (0x20): dxProjectPoint's point
    X_OUT = 0x522f2c,           // D3DTLVERTEX (0x20)
    X_HOUT = 0x522f4c,          // D3DHVERTEX (0x10)
    X_XFORM = 0x522f5c,         // D3DTRANSFORMDATA (0x34)
    // dxstate.obj
    S_CUR = 0x5229d8,           // dxState (0x18): the current state
    S_ALPHA = 0x5229e6,         //   its alpha byte (+0xe)
    S_PTR = 0x5265f4,           // dxState*: the state dxStateFlush applies
    S_CACHE = 0x526608,         // dxState: what the device has
    S_FORCE = 0x526620,         // byte: the next dxStateUpdate invalidates the cache
    // mr.obj's mrCaps (dx_driver_info, 0x522a18)
    CAPS_DITHER = 0x522a23, CAPS_LODBIAS = 0x522a24, CAPS_MODALPHA = 0x522a25,
    // dd.obj
    D_DEVICE = 0x4f3a60,        // IDirect3DDevice2* (also d3d +4)
};
// sizes written (footprints)
enum : uint32_t { ERRBUF_BYTES = 0x20, DDSD = 0x6c, DDCAPS_BYTES = 0x16c, D3DDD = 0xcc };

// HRESULTs the code tests
enum : uint32_t { DDERR_SURFACELOST = 0x887601c2, DDERR_WASSTILLDRAWING = 0x8876021c,
                  DDERR_OUTOFVIDEOMEMORY = 0x8876017c, DDERR_NOTFOUND = 0x887600ff };

static __forceinline volatile uint8_t& B8(uint32_t a) { return *(volatile uint8_t*)(uintptr_t)a; }
static __forceinline volatile uint16_t& U16(uint32_t a) { return *(volatile uint16_t*)(uintptr_t)a; }
static __forceinline volatile int32_t& I32(uint32_t a) { return *(volatile int32_t*)(uintptr_t)a; }
static __forceinline volatile uint32_t& U32(uint32_t a) { return *(volatile uint32_t*)(uintptr_t)a; }
static __forceinline void* volatile& PV(uint32_t a) { return *(void* volatile*)(uintptr_t)a; }
static __forceinline const char* S(uint32_t a) { return (const char*)(uintptr_t)a; }
static __forceinline void* A(uint32_t a) { return (void*)(uintptr_t)a; }

// ---- the wrappers' layouts ------------------------------------------------------------------------------------------
struct DSurface { void* s; };                       // IDirectDrawSurface3*
struct DTexture { void* s; void* tex; uint32_t handle; void* first; };   // +4 IDirect3DTexture2*, +8 handle, +0xc
struct DDraw { void* dd; };                         // IDirectDraw2*
struct D3D { void* d3d; void* dev; };               // IDirect3D2*, IDirect3DDevice2*
struct DMaterial { void* mat; uint32_t handle; };   // IDirect3DMaterial2*, D3DMATERIALHANDLE
struct DViewport { void* vp; };                     // IDirect3DViewport2*

// ---- calls ----------------------------------------------------------------------------------------------------------
// a game function by its v1.0 address
template <typename R, typename... Args> static __forceinline R game(uint32_t at, Args... a) {
    typedef R(__cdecl* F)(Args...);
    return ((F)(uintptr_t)at)(a...);
}
// an imported API through the game's own thunk (jmp [import slot]): __stdcall
template <typename R, typename... Args> static __forceinline R winapi(uint32_t at, Args... a) {
    typedef R(__stdcall* F)(Args...);
    return ((F)(uintptr_t)at)(a...);
}
// a game method (__thiscall) by its v1.0 address
template <typename R, typename... Args> static __forceinline R method(uint32_t at, void* self, Args... a) {
    typedef R(__fastcall* F)(void*, Edx, Args...);
    return ((F)(uintptr_t)at)(self, 0, a...);
}
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogReport = (Log_t)0x00411150;
static const Log_t LogPanic = (Log_t)0x004112b0;
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t game_sprintf = (Sprintf_t)0x004cf0a0;
static __forceinline void exit_process(uint32_t code) {        // call dword ptr [imp__ExitProcess@4]
    (*(void(__stdcall* volatile*)(uint32_t))0x005d7478)(code);
}
static __forceinline void* MemAlloc(int n) { return game<void*>(0x004140e0, n); }
static __forceinline void op_delete(void* p) { game<void>(0x00414390, p); }
static __forceinline void Win32Idle() { game<void>(0x00412bf0); }
static __forceinline int PTimeNow() { return game<int>(0x00413b40); }
static __forceinline char* game_strstr(const char* s, const char* k) { return game<char*>(0x004cf9a0, s, k); }

// this group, by address
static __forceinline void dx_error(uint32_t msg, int32_t hr) { game<void>(0x00458c20, S(msg), hr); }
static __forceinline uint8_t VidHas3DHW() { return game<uint8_t>(0x00454930); }
static __forceinline void VidRestoreMode() { game<void>(0x00454720); }
static __forceinline uint8_t grab_ddraw() { return game<uint8_t>(0x00454990); }
static __forceinline void release_ddraw() { game<void>(0x004549e0); }
static __forceinline uint8_t create_3d_device(void* guid) { return game<uint8_t>(0x00454a00, guid); }
static __forceinline void find_hardware() { game<void>(0x00454a90); }
static __forceinline uint8_t set_mode(int m) { return game<uint8_t>(0x00454b50, m); }
static __forceinline void restore_mode() { game<void>(0x00454c20); }
static __forceinline uint8_t create_surfaces(uint8_t triple) { return game<uint8_t>(0x00454c30, triple); }
static __forceinline uint8_t make_primary(uint8_t triple, void* pf) { return game<uint8_t>(0x00454ca0, triple, pf); }
static __forceinline void destroy_primary() { game<void>(0x00454e80); }
static __forceinline uint8_t make_zbuffer(void* s, uint8_t hw) { return game<uint8_t>(0x00454f50, s, hw); }
static __forceinline void destroy_surface_all() { game<void>(0x00455070); }
static __forceinline void restore_surfaces() { game<void>(0x00455100); }
static __forceinline void find_mem() { game<void>(0x00455130); }
static __forceinline void find_modes() { game<void>(0x004553e0); }
static __forceinline void d3d_begin() { game<void>(0x00458470); }
static __forceinline uint8_t d3d_grab() { return game<uint8_t>(0x00458490); }
static __forceinline void d3d_release() { game<void>(0x004584d0); }
static __forceinline uint8_t d3d_grab_driver() { return game<uint8_t>(0x00458500); }
static __forceinline void d3d_release_driver() { game<void>(0x00458530); }
static __forceinline uint8_t d3d_make_viewport() { return game<uint8_t>(0x00458550); }
static __forceinline void d3d_destroy_viewport() { game<void>(0x004585d0); }
static __forceinline void d3d_test_caps(void* info) { game<void>(0x004582b0, info); }
static __forceinline void* viewport_make_material() { return game<void*>(0x004586b0); }
static __forceinline void viewport_destroy_material(void* m) { game<void>(0x00458750, m); }
static __forceinline void init_xform_params() { game<void>(0x004587a0); }
static __forceinline void d3d_end() { game<void>(0x00458480); }
static __forceinline uint8_t dxBegin3D(void* info) { return game<uint8_t>(0x00458270, info); }
static __forceinline void dxEnd3D() { game<void>(0x004582a0); }
static __forceinline void convert_caps(void* desc, void* out) { game<void>(0x00459630, desc, out); }
static __forceinline void dxStateFlush() { game<void>(0x0045dfd0); }
static __forceinline void dxSetColorKeyEnable(uint8_t v) { game<void>(0x0045e420, v); }
static __forceinline void dxSetCullMode(uint32_t v) { game<void>(0x0045e440, v); }
static __forceinline void dxSetTextureWrapMode(uint32_t a, uint8_t u, uint8_t v) { game<void>(0x0045e460, a, u, v); }
static __forceinline void dxSetTextureWrapModeU(uint32_t v) { game<void>(0x0045e4a0, v); }
static __forceinline void dxSetTextureWrapModeV(uint32_t v) { game<void>(0x0045e4c0, v); }
static __forceinline void dxEnablePerspectiveCorrect(uint8_t v) { game<void>(0x0045e4e0, v); }
static __forceinline void dxSetFillMode(uint32_t v) { game<void>(0x0045e500, v); }
static __forceinline void dxSetShadeMode(uint32_t v) { game<void>(0x0045e520, v); }
static __forceinline void dxEnableAntialias(uint8_t v) { game<void>(0x0045e540, v); }
static __forceinline void dxEnableZbuffer(uint8_t v) { game<void>(0x0045e560, v); }
static __forceinline void dxSetZMode(uint32_t v) { game<void>(0x0045e580, v); }
static __forceinline void dxEnableZWrites(uint8_t v) { game<void>(0x0045e5a0, v); }
static __forceinline void dxEnableFog(uint8_t v) { game<void>(0x0045e5c0, v); }
static __forceinline void dxSetFogColor(uint32_t v) { game<void>(0x0045e5e0, v); }
static __forceinline void dxSetFilterMode(uint32_t mn, uint32_t mg) { game<void>(0x0045e600, mn, mg); }
static __forceinline void dxSetBlendParams(uint32_t s, uint32_t d) { game<void>(0x0045e630, s, d); }
static __forceinline void dxSetBlendMode(uint32_t v) { game<void>(0x0045e660, v); }
static __forceinline void dxEnableDither(uint8_t v) { game<void>(0x0045e680, v); }
static __forceinline void dxEnableAlphaBlend(uint8_t v) { game<void>(0x0045e6a0, v); }
static __forceinline void dxEnableAlphaTest(uint8_t v) { game<void>(0x0045e6c0, v); }
static __forceinline void dxEnableSpecular(uint8_t v) { game<void>(0x0045e6e0, v); }
static __forceinline void dxSetLODBias(uint32_t bits) { game<void>(0x0045e700, bits); }   // a float, pushed as bits
static __forceinline void* dd_create(void* guid) { return game<void*>(0x0045e950, guid); }
static __forceinline void dd_destroy(void* dd) { game<void>(0x0045ea20, dd); }
// the wrappers' methods
static __forceinline void* ddraw_ctor(void* self, void* dd2) { return method<void*>(0x0045f160, self, dd2); }
static __forceinline void ddraw_dtor(void* self) { method<void>(0x0045f170, self); }
static __forceinline void ddraw_get_caps(void* self, void* hal, void* hel) { method<void>(0x0045f190, self, hal, hel); }
static __forceinline void ddraw_get_mem(void* self, void* caps, uint32_t* total, uint32_t* free_) {
    method<void>(0x0045f1c0, self, caps, total, free_);
}
static __forceinline void ddraw_set_exclusive(void* self) { method<void>(0x0045f200, self); }
static __forceinline void ddraw_enum_modes(void* self, void* desc, uint32_t cb) { method<void>(0x0045f240, self, desc, cb); }
static __forceinline uint8_t ddraw_set_mode(void* self, int w, int h, int bpp) { return method<uint8_t>(0x0045f270, self, w, h, bpp); }
static __forceinline void ddraw_restore_mode(void* self) { method<void>(0x0045f2c0, self); }
static __forceinline void* ddraw_make_dd_surface(void* self, void* desc) { return method<void*>(0x0045f2d0, self, desc); }
static __forceinline void* ddraw_create_surface(void* self, void* desc) { return method<void*>(0x0045f3a0, self, desc); }
static __forceinline void ddraw_destroy_surface(void* self, void* s) { method<void>(0x0045f3e0, self, s); }
static __forceinline void* dsurface_ctor(void* self, void* s3) { return method<void*>(0x0045f460, self, s3); }
static __forceinline void dsurface_dtor(void* self) { method<void>(0x0045f470, self); }
static __forceinline uint8_t dsurface_lock(void* self, void* desc) { return method<uint8_t>(0x0045f490, self, desc); }
static __forceinline void dsurface_unlock(void* self) { method<void>(0x0045f510, self); }
static __forceinline uint8_t dsurface_flip(void* self) { return method<uint8_t>(0x0045f520, self); }
static __forceinline uint8_t dsurface_is_flip_done(void* self) { return method<uint8_t>(0x0045f580, self); }
static __forceinline void dsurface_get_desc(void* self, void* desc) { method<void>(0x0045f5a0, self, desc); }
static __forceinline void* dsurface_get_attached(void* self, void* caps) { return method<void*>(0x0045f5d0, self, caps); }
static __forceinline void dsurface_add_attached(void* self, void* s) { method<void>(0x0045f640, self, s); }
static __forceinline void dsurface_restore(void* self) { method<void>(0x0045f670, self); }
static __forceinline void* dtexture_ctor(void* self, void* s3) { return method<void*>(0x0045f750, self, s3); }
static __forceinline void dtexture_dtor(void* self) { method<void>(0x0045f7c0, self); }
static __forceinline void* d3d_ctor(void* self, void* dd) { return method<void*>(0x0045ed90, self, dd); }
static __forceinline void d3d_dtor(void* self) { method<void>(0x0045edb0, self); }
static __forceinline uint8_t d3d_connect(void* self, void* s) { return method<uint8_t>(0x0045edd0, self, s); }
static __forceinline void d3d_disconnect(void* self) { method<void>(0x0045ee00, self); }
static __forceinline void* d3d_create_driver(void* self, void* dd) { return method<void*>(0x0045ee20, self, dd); }
static __forceinline void* d3d_create_device(void* self, void* s) { return method<void*>(0x0045ee70, self, s); }
static __forceinline void* d3d_CreateMaterial(void* self, void* m) { return method<void*>(0x0045ef20, self, m); }
static __forceinline void d3d_DestroyMaterial(void* self, void* m) { method<void>(0x0045ef50, self, m); }
static __forceinline void* d3d_CreateViewport(void* self, void* m) { return method<void*>(0x0045ef70, self, m); }
static __forceinline void d3d_DestroyViewport(void* self, void* v) { method<void>(0x0045efa0, self, v); }
static __forceinline uint8_t d3d_GetCaps(void* self, void* hal, void* hel) { return method<uint8_t>(0x0045efc0, self, hal, hel); }
static __forceinline uint8_t d3d_GetStats(void* self, void* st) { return method<uint8_t>(0x0045f000, self, st); }
static __forceinline void d3d_BeginFrame(void* self) { method<void>(0x0045f040, self); }
static __forceinline void d3d_EndFrame(void* self) { method<void>(0x0045f070, self); }
static __forceinline void d3d_SetRenderState(void* self, uint32_t s, uint32_t v) { method<void>(0x0045f0a0, self, s, v); }
static __forceinline void* dmaterial_ctor(void* self, void* d3d, void* m) { return method<void*>(0x0045ea40, self, d3d, m); }
static __forceinline void dmaterial_dtor(void* self) { method<void>(0x0045eaf0, self); }
static __forceinline void dmaterial_set_material(void* self, void* m) { method<void>(0x0045eb10, self, m); }
static __forceinline void* dviewport_ctor(void* self, void* d3d, void* m) { return method<void*>(0x0045eb40, self, d3d, m); }
static __forceinline void dviewport_dtor(void* self) { method<void>(0x0045ebf0, self); }
static __forceinline uint8_t dviewport_set_viewport(void* self, int x, int y, int w, int h) {
    return method<uint8_t>(0x0045ec20, self, x, y, w, h);
}
static __forceinline void dviewport_clear(void* self, int x1, int y1, int x2, int y2, uint8_t t) {
    method<void>(0x0045ece0, self, x1, y1, x2, y2, t);
}
static __forceinline void dviewport_transform_verts(void* self, uint32_t n, void* data, uint32_t flags, uint32_t* off) {
    method<void>(0x0045ed50, self, n, data, flags, off);
}

// ---- footprint helpers ------------------------------------------------------------------------------------------------
static void fp_at(Footprint& f, uint32_t a, uint32_t n, const char* what) { f.add(A(a), n, what); }
static void fp_result(Footprint& f) {                   // dx_result, and dx_error's name for an unknown code
    fp_at(f, X_RESULT, 4, "dx_result");
    fp_at(f, X_ERRBUF, ERRBUF_BYTES, "dx_name_error's buffer");
}
static void fp_none(Footprint&) {}

// =====================================================================================================================
// vid.obj
// =====================================================================================================================

// vid_begin (0x4545b0)
static uint8_t __cdecl vid_begin_rw() {
    U32(V_MODE_OK) = 0;
    B8(V_IS3DFX) = 0;
    U16(V_MODE_OK + 4) = 0;
    if (!grab_ddraw()) {
        LogReport(S(0x4f1ed8));                        // "Unable to initialize DirectDraw"
        return 0;
    }
    B8(V_WAS_LOST) = 0;
    I32(V_MEGS) = 0;
    B8(V_AGP) = 0;
    for (uint32_t a = V_LOCKS; a <= V_FLIP_MAX; a += 4) U32(a) = 0;
    B8(V_INITED) = 1;
    return 1;
}
static void fp_vid_begin(Footprint& f) { f.replay_only = "DirectDrawEnumerate calls back into the game; allocates the ddraw wrapper"; }
PORT_FN(0x004545b0, "vid_begin", vid_begin_rw, fp_vid_begin)

// vid_end (0x454630)
static void __cdecl vid_end_rw() {
    VidRestoreMode();
    release_ddraw();
    B8(V_INITED) = 0;
}
static void fp_vid_end(Footprint& f) { f.replay_only = "frees the surface and ddraw wrappers"; }
PORT_FN(0x00454630, "vid_end", vid_end_rw, fp_vid_end)

// VidDisableSecondary (0x454650)
static void __cdecl VidDisableSecondary_rw() { B8(V_NO_SECONDARY) = 1; }
static void fp_disable_secondary(Footprint& f) { fp_at(f, V_NO_SECONDARY, 1, "vid disable_secondary"); }
PORT_FN(0x00454650, "VidDisableSecondary", VidDisableSecondary_rw, fp_disable_secondary)

// VidIs3DFX (0x454660)
static uint8_t __cdecl VidIs3DFX_rw() { return B8(V_IS3DFX); }
PORT_FN(0x00454660, "VidIs3DFX", VidIs3DFX_rw, fp_none)

// VidSetMode (0x454670)
static uint8_t __cdecl VidSetMode_rw(int mode, uint8_t want_triple) {
    VidRestoreMode();
    game<void>(0x0045a4c0);                              // TextureFlush
    if (!B8(V_MODE_OK + (uint32_t)mode)) mode = 2;       // FIX CANDIDATE: mode indexes mode_ok[6] unchecked
    if (!set_mode(mode)) {
        LogReport(S(0x4f1f14));                          // "Unable to set mode"
        return 0;
    }
    if (B8(V_FIND_MEM)) {
        find_mem();
        B8(V_FIND_MEM) = 0;
    }
    int32_t megs = I32(V_MEGS);
    uint8_t triple = megs >= 8 ? 1 : megs >= 4 ? want_triple : 0;
    for (;;) {
        if (create_surfaces(triple)) {
            I32(V_CUR_MODE) = mode;
            return 1;
        }
        LogReport(S(0x4f1ef8));                          // "Unable to create surface(s)"
        if (!triple) break;
        triple = 0;                                      // once more, double buffered
    }
    restore_mode();
    return 0;
}
static void fp_vid_set_mode(Footprint& f, int, uint8_t) { f.replay_only = "TextureFlush; frees and allocates the surface wrappers"; }
PORT_FN(0x00454670, "VidSetMode", VidSetMode_rw, fp_vid_set_mode)

// VidRestoreMode (0x454720)
static void __cdecl VidRestoreMode_rw() {
    if (!I32(V_CUR_MODE)) return;
    destroy_surface_all();
    restore_mode();
    I32(V_CUR_MODE) = 0;
}
static void fp_vid_restore_mode(Footprint& f) {
    if (I32(V_CUR_MODE)) f.replay_only = "frees the surface wrappers";
}
PORT_FN(0x00454720, "VidRestoreMode", VidRestoreMode_rw, fp_vid_restore_mode)

// VidGetPage (0x454740): the back buffer locked for the 2D, as the canvas
static uint8_t __cdecl VidGetPage_rw(uint8_t* c) {
    if (B8(V_WAS_LOST)) B8(V_WAS_LOST) = 0;
    volatile uint8_t* vc = c;
    vc[0] = 0;
    vc[1] = 0;
    *(volatile uint32_t*)(c + 4) = 0;
    *(volatile uint32_t*)(c + 8) = 0;
    *(volatile uint32_t*)(c + 0xc) = 0;
    *(volatile uint32_t*)(c + 0x10) = 0;
    Win32Idle();
    if (!B8(V_ACTIVE)) return 0;
    uint32_t desc[DDSD / 4];
    memset(desc, 0, sizeof desc);
    desc[0] = DDSD;
    int t0 = PTimeNow();
    uint8_t ok = dsurface_lock(PV(V_BACK), desc);
    U32(V_LOCKS) = U32(V_LOCKS) + 1;
    int dt = PTimeNow() - t0;
    if (dt > 0) {
        U32(V_LOCK_WAITS) = U32(V_LOCK_WAITS) + 1;
        I32(V_LOCK_TIME) = I32(V_LOCK_TIME) + dt;
        if (I32(V_LOCK_MAX) < dt) I32(V_LOCK_MAX) = dt;
    }
    if (ok) {
        // gxBuildCanvas(c, screen_format, lpSurface, screen_wid, screen_hit, lPitch)
        game<void>(0x0044f880, c, (char)B8(V_SCREEN_FMT), (char*)(uintptr_t)desc[9], I32(V_SCREEN_WID), I32(V_SCREEN_HIT),
                   (int)desc[4]);
        if (!*(volatile uint32_t*)(c + 4)) {
            LogReport(S(0x4f1f28));                      // "NULL page pointer"
            return 0;
        }
        return 1;
    }
    B8(V_WAS_LOST) = 1;
    restore_surfaces();
    return 0;
}
static void fp_vid_get_page(Footprint& f, uint8_t*) { f.replay_only = "Win32Idle pumps the window's messages; PTimeNow (timing statistics)"; }
PORT_FN(0x00454740, "VidGetPage", VidGetPage_rw, fp_vid_get_page)

// VidReady (0x454850)
static uint8_t __cdecl VidReady_rw() {
    if (I32(V_PAGES) > 2) return 1;
    Win32Idle();
    return dsurface_is_flip_done(PV(V_PRIMARY));
}
static void fp_vid_ready(Footprint& f) {
    if (I32(V_PAGES) <= 2) f.replay_only = "Win32Idle pumps the window's messages";
}
PORT_FN(0x00454850, "VidReady", VidReady_rw, fp_vid_ready)

// VidReleasePage (0x454870)
static void __cdecl VidReleasePage_rw() { dsurface_unlock(PV(V_BACK)); }
PORT_FN(0x00454870, "VidReleasePage", VidReleasePage_rw, fp_none)

// VidFlip (0x454880)
static void __cdecl VidFlip_rw() {
    Win32Idle();
    if (!B8(V_ACTIVE)) return;
    int t0 = PTimeNow();
    uint8_t ok = dsurface_flip(PV(V_PRIMARY));
    U32(V_FLIPS) = U32(V_FLIPS) + 1;
    int dt = PTimeNow() - t0;
    if (dt > 0) {
        U32(V_FLIP_WAITS) = U32(V_FLIP_WAITS) + 1;
        I32(V_FLIP_TIME) = I32(V_FLIP_TIME) + dt;
        if (I32(V_FLIP_MAX) < dt) I32(V_FLIP_MAX) = dt;
    }
    if (!ok) {
        restore_surfaces();
        B8(V_WAS_LOST) = 1;
    }
}
static void fp_vid_flip(Footprint& f) { f.replay_only = "Win32Idle pumps the window's messages; PTimeNow (timing statistics)"; }
PORT_FN(0x00454880, "VidFlip", VidFlip_rw, fp_vid_flip)

// VidGetPageCount (0x4548f0), VidWasLost (0x454900)
static int __cdecl VidGetPageCount_rw() { return I32(V_PAGES); }
PORT_FN(0x004548f0, "VidGetPageCount", VidGetPageCount_rw, fp_none)
static uint8_t __cdecl VidWasLost_rw() { return B8(V_WAS_LOST); }
PORT_FN(0x00454900, "VidWasLost", VidWasLost_rw, fp_none)

// VidIsModeSupported (0x454910): 640x480 not on a card of 2 MB or less
static uint8_t __cdecl VidIsModeSupported_rw(int mode) {
    if (mode == 2 && I32(V_MEGS) <= 2) return 0;
    return B8(V_MODE_OK + (uint32_t)mode);               // FIX CANDIDATE: unchecked index
}
static void fp_mode_supported(Footprint&, int) {}
PORT_FN(0x00454910, "VidIsModeSupported", VidIsModeSupported_rw, fp_mode_supported)

// VidHas3DHW (0x454930), VidGetMegsRam (0x454940)
static uint8_t __cdecl VidHas3DHW_rw() { return B8(V_HAS3DHW); }
PORT_FN(0x00454930, "VidHas3DHW", VidHas3DHW_rw, fp_none)
static int __cdecl VidGetMegsRam_rw() { return I32(V_MEGS); }
PORT_FN(0x00454940, "VidGetMegsRam", VidGetMegsRam_rw, fp_none)

// VidDumpMem (0x454950)
static void __cdecl VidDumpMem_rw() {
    uint32_t caps = 0x10001000, total = 0, free_ = 0;    // (the original's totals are uninitialised: FIX CANDIDATE)
    ddraw_get_mem(PV(V_DD), &caps, &total, &free_);
    LogReport(S(0x4f1f3c), free_, total);                // "texture mem: %d of %d"
}
static void fp_vid_dump_mem(Footprint& f) { fp_result(f); }
PORT_FN(0x00454950, "VidDumpMem", VidDumpMem_rw, fp_vid_dump_mem)

// grab_ddraw (0x454990)
static uint8_t __cdecl grab_ddraw_rw() {
    PV(V_DD) = 0;
    B8(V_HAS3DHW) = 0;
    find_hardware();
    if (!B8(V_HAS3DHW)) {
        LogReport(S(0x4f1f54));                          // "vid: no 3D hardware"
        exit_process(4);
    }
    find_modes();
    ddraw_set_exclusive(PV(V_DD));
    return 1;
}
static void fp_grab_ddraw(Footprint& f) { f.replay_only = "DirectDrawEnumerate / EnumDisplayModes call back into the game; allocates"; }
PORT_FN(0x00454990, "grab_ddraw", grab_ddraw_rw, fp_grab_ddraw)

// release_ddraw (0x4549e0)
static void __cdecl release_ddraw_rw() {
    dd_destroy(PV(V_DD));
    PV(V_DD) = 0;
}
static void fp_release_ddraw(Footprint& f) {
    if (PV(V_DD)) f.replay_only = "frees the ddraw wrapper";
    else fp_at(f, V_DD, 4, "vid dd");
}
PORT_FN(0x004549e0, "release_ddraw", release_ddraw_rw, fp_release_ddraw)

// create_3d_device (0x454a00): a DirectDraw on this driver, kept if it has 3D hardware
static uint8_t __cdecl create_3d_device_rw(void* guid) {
    void* dd = dd_create(guid);
    if (!dd) return 0;
    uint32_t hal[DDCAPS_BYTES / 4], hel[DDCAPS_BYTES / 4];
    memset(hal, 0, sizeof hal);
    hal[0] = DDCAPS_BYTES;
    memset(hel, 0, sizeof hel);
    hel[0] = DDCAPS_BYTES;
    ddraw_get_caps(dd, hal, hel);
    if (*(volatile uint8_t*)&hal[1] & 1) {               // DDCAPS_3D
        B8(V_HAS3DHW) = 1;
        PV(V_DD) = dd;
        return 1;
    }
    dd_destroy(dd);
    return 0;
}
static void fp_create_3d_device(Footprint& f, void*) { f.replay_only = "allocates (and may free) the ddraw wrapper"; }
PORT_FN(0x00454a00, "create_3d_device", create_3d_device_rw, fp_create_3d_device)

// find_hardware (0x454a90)
static void __cdecl find_hardware_rw() {
    if (!B8(V_NO_SECONDARY)) winapi<int32_t>(0x004ccce0, 0x00454ac0u, 0u);   // DirectDrawEnumerateA(hardware_callback, 0)
    if (!B8(V_HAS3DHW)) create_3d_device(0);
}
static void fp_find_hardware(Footprint& f) { f.replay_only = "DirectDrawEnumerate calls back into the game; allocates"; }
PORT_FN(0x00454a90, "find_hardware", find_hardware_rw, fp_find_hardware)

// hardware_callback (0x454ac0): DirectDrawEnumerate's callback -- the first secondary driver with 3D hardware
static int __stdcall hardware_callback_rw(void* guid, char* desc, char* name, void*) {
    if (!guid) return 1;
    if (!create_3d_device(guid)) return 1;
    if (game_strstr(name, S(0x4f1f68)) || game_strstr(desc, S(0x4f1f70))) {   // "3dfx", "3Dfx"
        B8(V_IS3DFX) = 1;
        LogReport(S(0x4f1f78), name, desc);              // "vid found 3Dfx : %s (%s)"
        return 0;
    }
    LogReport(S(0x4f1f94), name, desc);                  // "vid: found 3D device: %s (%s)"
    return 0;
}
static void fp_hardware_callback(Footprint& f, void* guid, char*, char*, void*) {
    if (guid) f.replay_only = "create_3d_device allocates the ddraw wrapper";
}
PORT_FN(0x00454ac0, "hardware_callback", hardware_callback_rw, fp_hardware_callback)

// set_mode (0x454b50)
static uint8_t __cdecl set_mode_rw(int mode) {
    uint8_t ok = 1;
    int bpp = 0, w, h;
    switch (mode) {
    case 1: w = 0x200; h = 0x180; B8(V_SCREEN_FMT) = 4; bpp = 16; break;
    case 2: w = 0x280; h = 0x1e0; B8(V_SCREEN_FMT) = 4; bpp = 16; break;
    case 3: w = 0x320; h = 0x258; B8(V_SCREEN_FMT) = 4; bpp = 16; break;
    case 4: w = 0x400; h = 0x300; B8(V_SCREEN_FMT) = 4; bpp = 16; break;
    default: ok = 0; w = I32(V_SCREEN_WID); h = I32(V_SCREEN_HIT); break;
    }
    I32(V_SCREEN_WID) = w;
    I32(V_SCREEN_HIT) = h;
    if (!ok) {
        LogReport(S(0x4f1fd0));                          // "Passed invalid video mode"
        return 0;
    }
    if (ddraw_set_mode(PV(V_DD), w, h, bpp)) return 1;
    LogReport(S(0x4f1fb4));                              // "Unable to set video mode"
    return 0;
}
static void fp_set_mode(Footprint& f, int) {
    fp_at(f, V_SCREEN_WID, 4, "screen_wid");
    fp_at(f, V_SCREEN_HIT, 4, "screen_hit");
    fp_at(f, V_SCREEN_FMT, 1, "screen_format");
    fp_result(f);
}
PORT_FN(0x00454b50, "set_mode", set_mode_rw, fp_set_mode)

// restore_mode (0x454c20)
static void __cdecl restore_mode_rw() { ddraw_restore_mode(PV(V_DD)); }
PORT_FN(0x00454c20, "restore_mode", restore_mode_rw, fp_none)

// create_surfaces (0x454c30)
static uint8_t __cdecl create_surfaces_rw(uint8_t triple) {
    uint32_t pf[8];                                      // the primary's pixel format (make_primary may fill it; unused)
    if (!make_primary(triple, pf)) return 0;
    uint8_t hw = VidHas3DHW();
    void* back = PV(V_BACK);
    PV(V_3D) = back;
    if (make_zbuffer(back, hw ? 1 : 0)) return 1;
    destroy_primary();
    return 0;
}
static void fp_create_surfaces(Footprint& f, uint8_t) { f.replay_only = "allocates the surface wrappers"; }
PORT_FN(0x00454c30, "create_surfaces", create_surfaces_rw, fp_create_surfaces)

// make_primary (0x454ca0): the flip chain, its pixel format, the back buffer
static uint8_t __cdecl make_primary_rw(uint8_t triple, uint32_t* pf) {
    uint32_t d[DDSD / 4];
    memset(d, 0, sizeof d);
    d[0] = DDSD;
    d[1] = 0x21;                                         // DDSD_CAPS | DDSD_BACKBUFFERCOUNT
    d[0x68 / 4] = 0x6218;                                // PRIMARYSURFACE | FLIP | COMPLEX | 3DDEVICE | VIDEOMEMORY
    if (triple) {
        I32(V_PAGES) = 3;
        d[5] = 2;
        LogReport(S(0x4f1fec));                          // "try triple buffer"
    } else {
        I32(V_PAGES) = 2;
        d[5] = 1;
    }
    void* p = ddraw_create_surface(PV(V_DD), d);
    PV(V_PRIMARY) = p;
    if (!p) {
        LogReport(S(0x4f205c));                          // "can't create screens"
        return 0;
    }
    uint32_t g[DDSD / 4];
    memset(g, 0, sizeof g);
    g[0] = DDSD;
    dsurface_get_desc(PV(V_PRIMARY), g);
    if ((g[1] & 0x20) && g[5] == 2) LogReport(S(0x4f2000));   // "vid: triple buffer on"
    if (g[1] & 0x1000) {                                 // DDSD_PIXELFORMAT
        volatile uint32_t* vpf = pf;
        for (int i = 0; i < 8; i++) vpf[i] = g[0x48 / 4 + i];
        if (g[0x4c / 4] & 0x40) {                        // DDPF_RGB
            if (g[0x54 / 4] == 16 && g[0x58 / 4] == 0xf800 && g[0x5c / 4] == 0x7e0 && g[0x60 / 4] == 0x1f) {
                B8(V_SCREEN_FMT) = 4;
            } else if (g[0x54 / 4] == 16 && g[0x58 / 4] == 0x7c00 && g[0x5c / 4] == 0x3e0 && g[0x60 / 4] == 0x1f) {
                B8(V_SCREEN_FMT) = 3;
            } else {
                LogReport(S(0x4f2018));                  // "non-565, non-555 primary surface:"
                ddraw_destroy_surface(PV(V_DD), PV(V_PRIMARY));
                PV(V_PRIMARY) = 0;
                return 0;
            }
        } else {
            LogReport(S(0x4f203c));                      // "no rgb pixels"
        }
    } else {
        LogReport(S(0x4f204c));                          // "no pixel format"
    }
    uint32_t caps = 4;                                   // DDSCAPS_BACKBUFFER
    PV(V_BACK) = dsurface_get_attached(PV(V_PRIMARY), &caps);
    return 1;
}
static void fp_make_primary(Footprint& f, uint8_t, uint32_t*) { f.replay_only = "allocates (and may free) the surface wrappers"; }
PORT_FN(0x00454ca0, "make_primary", make_primary_rw, fp_make_primary)

// destroy_primary (0x454e80)
static void __cdecl destroy_primary_rw() {
    ddraw_destroy_surface(PV(V_DD), PV(V_PRIMARY));
    PV(V_PRIMARY) = 0;
}
static void fp_destroy_primary(Footprint& f) {
    if (PV(V_PRIMARY)) f.replay_only = "frees the primary's wrapper";
    else fp_at(f, V_PRIMARY, 4, "vid primary");
}
PORT_FN(0x00454e80, "destroy_primary", destroy_primary_rw, fp_destroy_primary)

// make_software_3d (0x454ea0): a system-memory 3D target (no callers in v1.0)
static void* __cdecl make_software_3d_rw(const uint32_t* pf) {
    uint32_t d[DDSD / 4];
    memset(d, 0, sizeof d);
    d[2] = (uint32_t)I32(V_SCREEN_HIT);
    d[3] = (uint32_t)I32(V_SCREEN_WID);
    d[0] = DDSD;
    d[1] = 0x1007;                                       // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    d[0x68 / 4] = 0x2800;                                // 3DDEVICE | SYSTEMMEMORY
    for (int i = 0; i < 8; i++) d[0x48 / 4 + i] = ((const volatile uint32_t*)pf)[i];
    void* s = ddraw_create_surface(PV(V_DD), d);
    if (!s) {
        LogReport(S(0x4f2074));                          // "can't create software surfaces"
        return 0;
    }
    return s;
}
static void fp_make_software_3d(Footprint& f, const uint32_t*) { f.replay_only = "allocates a surface wrapper"; }
PORT_FN(0x00454ea0, "make_software_3d", make_software_3d_rw, fp_make_software_3d)

// destroy_software_3d (0x454f20)
static void __cdecl destroy_software_3d_rw() {
    void* v = PV(V_3D);
    if (PV(V_BACK) == v) return;
    ddraw_destroy_surface(PV(V_DD), v);
    PV(V_3D) = 0;
}
static void fp_destroy_software_3d(Footprint& f) {
    if (PV(V_BACK) != PV(V_3D)) f.replay_only = "frees a surface wrapper";
}
PORT_FN(0x00454f20, "destroy_software_3d", destroy_software_3d_rw, fp_destroy_software_3d)

// make_zbuffer (0x454f50): a 16-bit z-buffer the screen's size, attached to `surf`
static uint8_t __cdecl make_zbuffer_rw(void* surf, uint8_t hw) {
    uint32_t d[DDSD / 4];
    memset(d, 0, sizeof d);
    d[0] = DDSD;
    d[1] = 0x47;                                         // CAPS | HEIGHT | WIDTH | ZBUFFERBITDEPTH
    d[0x68 / 4] = hw ? 0x24000 : 0x20800;                // ZBUFFER | VIDEOMEMORY, else ZBUFFER | SYSTEMMEMORY
    d[2] = (uint32_t)I32(V_SCREEN_HIT);
    d[3] = (uint32_t)I32(V_SCREEN_WID);
    d[6] = 16;
    void* z = ddraw_create_surface(PV(V_DD), d);
    PV(V_ZBUF) = z;
    if (!z) {
        dx_error(0x4f20b4, I32(X_RESULT));               // "unable to create 16-bit zbuffer"
        exit_process(4);
        return 0;
    }
    uint32_t g[DDSD / 4];
    memset(g, 0, sizeof g);
    g[0] = DDSD;
    dsurface_get_desc(z, g);
    if (hw && !(g[0x68 / 4] & 0x4000)) {
        LogReport(S(0x4f2094));                          // "zbuffer must be in video memory"
        ddraw_destroy_surface(PV(V_DD), PV(V_ZBUF));
        PV(V_ZBUF) = 0;
        return 0;
    }
    dsurface_add_attached(surf, PV(V_ZBUF));
    return 1;
}
static void fp_make_zbuffer(Footprint& f, void*, uint8_t) { f.replay_only = "allocates (and may free) the z-buffer's wrapper"; }
PORT_FN(0x00454f50, "make_zbuffer", make_zbuffer_rw, fp_make_zbuffer)

// destroy_surface (0x455070): the z-buffer, a separate 3D target, the back buffer, the primary
static void __cdecl destroy_surface_rw() {
    void* z = PV(V_ZBUF);
    if (z) {
        ddraw_destroy_surface(PV(V_DD), z);
        PV(V_ZBUF) = 0;
    }
    void* v = PV(V_3D);
    void* b = PV(V_BACK);
    if (v && b != v) {
        ddraw_destroy_surface(PV(V_DD), v);
        PV(V_3D) = 0;
        b = PV(V_BACK);
    }
    if (b) {
        ddraw_destroy_surface(PV(V_DD), b);
        PV(V_BACK) = 0;
    }
    void* p = PV(V_PRIMARY);
    if (p) {
        ddraw_destroy_surface(PV(V_DD), p);
        PV(V_PRIMARY) = 0;
    }
}
static void fp_destroy_surface(Footprint& f) {
    if (PV(V_ZBUF) || PV(V_3D) || PV(V_BACK) || PV(V_PRIMARY)) f.replay_only = "frees the surface wrappers";
}
PORT_FN(0x00455070, "destroy_surface", destroy_surface_rw, fp_destroy_surface)

// restore_surfaces (0x455100)
static void __cdecl restore_surfaces_rw() {
    B8(V_WAS_LOST) = 1;
    dsurface_restore(PV(V_PRIMARY));
    dsurface_restore(PV(V_ZBUF));
}
static void fp_restore_surfaces(Footprint& f) {
    fp_at(f, V_WAS_LOST, 1, "vid was_lost");
    fp_result(f);
}
PORT_FN(0x00455100, "restore_surfaces", restore_surfaces_rw, fp_restore_surfaces)

// _VidActivate (0x455120)
static void __cdecl VidActivate_rw(uint8_t on) { B8(V_ACTIVE) = on; }
static void fp_vid_activate(Footprint& f, uint8_t) { fp_at(f, V_ACTIVE, 1, "vid active"); }
PORT_FN(0x00455120, "_VidActivate", VidActivate_rw, fp_vid_activate)

// find_mem (0x455130): the video memory tier (0/2/4/8/16), AGP, the modes it can't hold
static void __cdecl find_mem_rw() {
    uint32_t caps = 0x4000, total = 0, free_ = 0;        // DDSCAPS_VIDEOMEMORY (the original's totals are
    ddraw_get_mem(PV(V_DD), &caps, &total, &free_);      // uninitialised if the call fails: FIX CANDIDATE)
    total += 0x96000;
    int32_t megs = total < 2000000 ? 0 : total < 4000000 ? 2 : total < 8000000 ? 4 : total < 16000000 ? 8 : 16;
    I32(V_MEGS) = megs;
    // fild qword (the total, zero-extended); fmul dword 2^-20; fstp qword
    LogReport(S(0x4f20d4), megs, (double)total * 9.5367431640625e-07);   // "vid: %d meg card (reported:%f)"
    caps = 0x10001000;                                   // TEXTURE | LOCALVIDMEM
    ddraw_get_mem(PV(V_DD), &caps, &total, &free_);
    int32_t m = I32(V_MEGS);
    if (m > 2 && free_ < 2000000) LogReport(S(0x4f20f4), m, free_ / 1000000);   // "!!!! %d meg card with only %d texture ram"
    caps = 0x20001000;                                   // TEXTURE | NONLOCALVIDMEM
    ddraw_get_mem(PV(V_DD), &caps, &total, &free_);
    if (free_ != 0) {
        B8(V_AGP) = 1;
        LogReport(S(0x4f2120));                          // "vid: using AGP textures"
        m = I32(V_MEGS);
        if (m == 2 && free_ > 2000000) m = 4;
        else if (m == 4 && free_ > 4000000) m = 8;
        else if (m == 8 && free_ > 8000000) m = 16;
    } else {
        m = I32(V_MEGS);
    }
    I32(V_MEGS) = m;
    switch (m) {
    case 2: B8(V_MODE_OK + 2) = 0; B8(V_MODE_OK + 3) = 0; B8(V_MODE_OK + 4) = 0; break;
    case 4: B8(V_MODE_OK + 3) = 0; B8(V_MODE_OK + 4) = 0; break;
    case 8: B8(V_MODE_OK + 4) = 0; break;
    case 16: break;
    default:
        I32(V_MEGS) = m;
        LogReport(S(0x4f2138), m);                       // "unsupported: %d megs of vram"
        exit_process(5);
        break;
    }
}
static void fp_find_mem(Footprint& f) {
    fp_at(f, V_MEGS, 4, "vid megs");
    fp_at(f, V_AGP, 1, "vid agp");
    fp_at(f, V_MODE_OK + 2, 3, "vid mode_ok[2..4]");
    fp_result(f);
}
PORT_FN(0x00455130, "find_mem", find_mem_rw, fp_find_mem)

// mode_callback (0x455330): EnumDisplayModes' callback
static int32_t __stdcall mode_callback_rw(const uint32_t* d, void*) {
    const volatile uint32_t* v = d;
    uint32_t w = v[3];
    if (w == 320 && v[2] == 200) { B8(V_MODE_OK + 5) = 1; return 1; }
    if (w == 512 && v[2] == 384) { B8(V_MODE_OK + 1) = 1; return 1; }
    if (w == 640 && v[2] == 480) { B8(V_MODE_OK + 2) = 1; return 1; }
    if (w == 800 && v[2] == 600) { B8(V_MODE_OK + 3) = 1; return 1; }
    if (w == 1024 && v[2] == 768) B8(V_MODE_OK + 4) = 1;
    return 1;
}
static void fp_mode_callback(Footprint& f, const uint32_t*, void*) { fp_at(f, V_MODE_OK + 1, 5, "vid mode_ok[1..5]"); }
PORT_FN(0x00455330, "mode_callback", mode_callback_rw, fp_mode_callback)

// find_modes (0x4553e0): the 16-bit 3D-capable modes
static void __cdecl find_modes_rw() {
    uint32_t d[DDSD / 4];
    memset(d, 0, sizeof d);
    d[0] = DDSD;
    d[1] = 0x1001;                                       // CAPS | PIXELFORMAT
    d[0x68 / 4] = 0x2000;                                // 3DDEVICE
    d[0x48 / 4] = 0x20;                                  // the pixel format: dwSize, DDPF_RGB, 16 bits
    d[0x4c / 4] = 0x40;
    d[0x54 / 4] = 16;
    ddraw_enum_modes(PV(V_DD), d, 0x00455330u);          // mode_callback
    if (!B8(V_MODE_OK + 2)) {
        LogReport(S(0x4f2158));                          // "video card cannot do 640x480x16!"
        exit_process(5);
    }
}
static void fp_find_modes(Footprint& f) { f.replay_only = "EnumDisplayModes calls back into the game (mode_callback)"; }
PORT_FN(0x004553e0, "find_modes", find_modes_rw, fp_find_modes)

// _introGetPrimarySurface (0x455470), _introGetDirectDraw (0x455480)
static void* __cdecl introGetPrimarySurface_rw() { return ((DSurface*)PV(V_PRIMARY))->s; }
PORT_FN(0x00455470, "_introGetPrimarySurface", introGetPrimarySurface_rw, fp_none)
static void* __cdecl introGetDirectDraw_rw() { return ((DDraw*)PV(V_DD))->dd; }
PORT_FN(0x00455480, "_introGetDirectDraw", introGetDirectDraw_rw, fp_none)

// =====================================================================================================================
// dx.obj
// =====================================================================================================================

// dxBegin (0x458240)
static uint8_t __cdecl dxBegin_rw() {
    d3d_begin();
    d3d_grab();
    init_xform_params();
    PV(X_MATERIAL) = 0;
    PV(X_VIEWPORT) = 0;
    return 1;
}
static void fp_dx_begin(Footprint& f) { f.replay_only = "allocates the d3d wrapper"; }
PORT_FN(0x00458240, "dxBegin", dxBegin_rw, fp_dx_begin)

// dxEnd (0x458260)
static void __cdecl dxEnd_rw() {
    d3d_release();
    d3d_end();
}
static void fp_dx_end(Footprint& f) { f.replay_only = "frees the d3d wrapper"; }
PORT_FN(0x00458260, "dxEnd", dxEnd_rw, fp_dx_end)

// dxBegin3D (0x458270)
static uint8_t __cdecl dxBegin3D_rw(void* info) {
    if (!d3d_grab_driver()) return 0;
    if (!d3d_make_viewport()) {
        d3d_release_driver();
        return 0;
    }
    if (info) d3d_test_caps(info);
    return 1;
}
static void fp_dx_begin3d(Footprint& f, void*) { f.replay_only = "allocates the viewport and material wrappers"; }
PORT_FN(0x00458270, "dxBegin3D", dxBegin3D_rw, fp_dx_begin3d)

// dxEnd3D (0x4582a0)
static void __cdecl dxEnd3D_rw() {
    d3d_destroy_viewport();
    d3d_release_driver();
}
static void fp_dx_end3d(Footprint& f) { f.replay_only = "frees the viewport and material wrappers"; }
PORT_FN(0x004582a0, "dxEnd3D", dxEnd3D_rw, fp_dx_end3d)

// d3d_test_caps (0x4582b0): the device's caps into the renderer's dx_driver_info
static void __cdecl d3d_test_caps_rw(uint8_t* info) {
    uint32_t hal[D3DDD / 4], hel[D3DDD / 4];
    memset(hal, 0, sizeof hal);
    hal[0] = D3DDD;
    memset(hel, 0, sizeof hel);
    hel[0] = D3DDD;
    uint8_t drv[0x80];                                   // uninitialised, as the original's (FIX CANDIDATE)
    if (!d3d_GetCaps(PV(X_D3D), hal, hel)) return;
    volatile uint8_t* o = info;
    const volatile uint8_t* c = drv;
    if (VidHas3DHW()) {
        convert_caps(hal, drv);
        o[0] = c[0x5f] ? 1 : 0;                          // bilinear
        o[1] = c[0x63] ? 1 : 0;                          // trilinear
        int32_t alpha;
        if (!c[0x5b]) alpha = 0;                         // no alpha textures
        else if (c[0x1a] && c[0x1b] && c[0x20] && c[0x1e] && c[0x1f] && c[0x27] && c[0x28] && c[0x29] && c[0x2b] && c[0x2c])
            alpha = 2;
        else alpha = 1;
        *(volatile int32_t*)(info + 4) = alpha;
        o[8] = 0;
        o[9] = c[0x61] ? 1 : 0;
        o[0xa] = 0;
        o[0xb] = c[0x12] ? 1 : 0;
        o[0xc] = c[0x19] ? 1 : 0;
        o[0xd] = c[0x67] ? 1 : 0;
    } else {
        convert_caps(hel, drv);
        o[0] = 0;
        o[1] = 0;
        *(volatile int32_t*)(info + 4) = 0;
        o[8] = 0;
        o[9] = 0;
        o[0xa] = 0;
        o[0xb] = 0;
        o[0xc] = 0;
        o[0xd] = 0;
    }
}
static void fp_d3d_test_caps(Footprint& f, uint8_t* info) {
    f.add(info, 0xe, "dx_driver_info");
    fp_result(f);
}
PORT_FN(0x004582b0, "d3d_test_caps", d3d_test_caps_rw, fp_d3d_test_caps)

// dxRelease (0x458450), dxRestore (0x458460)
static void __cdecl dxRelease_rw() { dxEnd3D(); }
static void fp_dx_release(Footprint& f) { f.replay_only = "frees the viewport and material wrappers"; }
PORT_FN(0x00458450, "dxRelease", dxRelease_rw, fp_dx_release)
static void __cdecl dxRestore_rw() { dxBegin3D(0); }
static void fp_dx_restore(Footprint& f) { f.replay_only = "allocates the viewport and material wrappers"; }
PORT_FN(0x00458460, "dxRestore", dxRestore_rw, fp_dx_restore)

// d3d_begin (0x458470), d3d_end (0x458480)
static void __cdecl d3d_begin_rw() {
    PV(X_D3D) = 0;
    B8(X_CTX) = 0;
}
static void fp_d3d_begin(Footprint& f) {
    fp_at(f, X_D3D, 4, "the d3d");
    fp_at(f, X_CTX, 1, "dx connected");
}
PORT_FN(0x00458470, "d3d_begin", d3d_begin_rw, fp_d3d_begin)
static void __cdecl d3d_end_rw() { d3d_begin(); }
PORT_FN(0x00458480, "d3d_end", d3d_end_rw, fp_d3d_begin)

// d3d_grab (0x458490)
static uint8_t __cdecl d3d_grab_rw() {
    void* p = MemAlloc(8);
    if (p) PV(X_D3D) = d3d_ctor(p, PV(V_DD));
    else PV(X_D3D) = 0;
    return PV(X_D3D) != 0;
}
static void fp_d3d_grab(Footprint& f) { f.replay_only = "allocates the d3d wrapper"; }
PORT_FN(0x00458490, "d3d_grab", d3d_grab_rw, fp_d3d_grab)

// d3d_release (0x4584d0)
static void __cdecl d3d_release_rw() {
    void* p = PV(X_D3D);
    if (p) {
        d3d_dtor(p);
        op_delete(p);
    }
    PV(X_D3D) = 0;
}
static void fp_d3d_release(Footprint& f) {
    if (PV(X_D3D)) f.replay_only = "frees the d3d wrapper";
    else fp_at(f, X_D3D, 4, "the d3d");
}
PORT_FN(0x004584d0, "d3d_release", d3d_release_rw, fp_d3d_release)

// d3d_grab_driver (0x458500)
static uint8_t __cdecl d3d_grab_driver_rw() {
    void* v = PV(V_3D);
    if (!d3d_connect(PV(X_D3D), v)) return 0;
    B8(X_CTX) = 1;
    return 1;
}
static void fp_d3d_grab_driver(Footprint& f) {
    fp_at(f, X_CTX, 1, "dx connected");
    if (PV(X_D3D)) f.add(&((D3D*)PV(X_D3D))->dev, 4, "d3d device");
    fp_at(f, D_DEVICE, 4, "the device");
    fp_result(f);
}
PORT_FN(0x00458500, "d3d_grab_driver", d3d_grab_driver_rw, fp_d3d_grab_driver)

// d3d_release_driver (0x458530)
static void __cdecl d3d_release_driver_rw() {
    B8(X_CTX) = 0;
    d3d_disconnect(PV(X_D3D));
}
static void fp_d3d_release_driver(Footprint& f) {
    fp_at(f, X_CTX, 1, "dx connected");
    if (PV(X_D3D)) f.add(&((D3D*)PV(X_D3D))->dev, 4, "d3d device");
    fp_at(f, D_DEVICE, 4, "the device");
}
PORT_FN(0x00458530, "d3d_release_driver", d3d_release_driver_rw, fp_d3d_release_driver)

// d3d_make_viewport (0x458550)
static uint8_t __cdecl d3d_make_viewport_rw() {
    void* m = viewport_make_material();
    PV(X_MATERIAL) = m;
    if (!m) return 0;
    void* v = d3d_CreateViewport(PV(X_D3D), m);
    PV(X_VIEWPORT) = v;
    if (v) return dviewport_set_viewport(v, 0, 0, I32(V_SCREEN_WID), I32(V_SCREEN_HIT));
    viewport_destroy_material(PV(X_MATERIAL));
    PV(X_MATERIAL) = 0;
    return 0;
}
static void fp_d3d_make_viewport(Footprint& f) { f.replay_only = "allocates the viewport and material wrappers"; }
PORT_FN(0x00458550, "d3d_make_viewport", d3d_make_viewport_rw, fp_d3d_make_viewport)

// dxSetViewport (0x4585b0)
static uint8_t __cdecl dxSetViewport_rw(int x, int y, int w, int h) { return dviewport_set_viewport(PV(X_VIEWPORT), x, y, w, h); }
static void fp_dx_set_viewport(Footprint& f, int, int, int, int) { fp_result(f); }
PORT_FN(0x004585b0, "dxSetViewport", dxSetViewport_rw, fp_dx_set_viewport)

// d3d_destroy_viewport (0x4585d0)
static void __cdecl d3d_destroy_viewport_rw() {
    viewport_destroy_material(PV(X_MATERIAL));
    void* v = PV(X_VIEWPORT);
    PV(X_MATERIAL) = 0;
    d3d_DestroyViewport(PV(X_D3D), v);
    PV(X_VIEWPORT) = 0;
}
static void fp_d3d_destroy_viewport(Footprint& f) {
    if (PV(X_MATERIAL) || PV(X_VIEWPORT)) f.replay_only = "frees the viewport and material wrappers";
    else {
        fp_at(f, X_MATERIAL, 4, "dx material");
        fp_at(f, X_VIEWPORT, 4, "dx viewport");
    }
}
PORT_FN(0x004585d0, "d3d_destroy_viewport", d3d_destroy_viewport_rw, fp_d3d_destroy_viewport)

// a D3DMATERIAL (0x50): diffuse (r, g, b, 1), power 1, ramp size 1 -- the viewport's background (float bits)
static __forceinline void background_material(uint32_t* m, uint32_t r, uint32_t g, uint32_t b) {
    memset(m, 0, 0x50);
    m[0] = 0x50;
    m[1] = r;
    m[2] = g;
    m[3] = b;
    m[4] = 0x3f800000;
    m[0x44 / 4] = 0x3f800000;
    m[0x4c / 4] = 1;
}

// dxSetViewportColor (0x458610)
static void __cdecl dxSetViewportColor_rw(uint32_t r, uint32_t g, uint32_t b) {
    uint32_t m[0x50 / 4];
    background_material(m, r, g, b);
    dmaterial_set_material(PV(X_MATERIAL), m);
}
static void fp_dx_set_viewport_color(Footprint& f, uint32_t, uint32_t, uint32_t) { fp_result(f); }
PORT_FN(0x00458610, "dxSetViewportColor", dxSetViewportColor_rw, fp_dx_set_viewport_color)

// viewport_make_material (0x4586b0): the default background (0.156863, 0.435294, 0.670588)
static void* __cdecl viewport_make_material_rw() {
    uint32_t m[0x50 / 4];
    background_material(m, 0x3e20a0a1, 0x3edededf, 0x3f2babac);
    return d3d_CreateMaterial(PV(X_D3D), m);
}
static void fp_viewport_make_material(Footprint& f) { f.replay_only = "allocates the material wrapper"; }
PORT_FN(0x004586b0, "viewport_make_material", viewport_make_material_rw, fp_viewport_make_material)

// viewport_destroy_material (0x458750)
static void __cdecl viewport_destroy_material_rw(void* m) { d3d_DestroyMaterial(PV(X_D3D), m); }
static void fp_viewport_destroy_material(Footprint& f, void* m) {
    if (m) f.replay_only = "frees the material wrapper";
}
PORT_FN(0x00458750, "viewport_destroy_material", viewport_destroy_material_rw, fp_viewport_destroy_material)

// dxClearZ (0x458770)
static void __cdecl dxClearZ_rw(int x1, int y1, int x2, int y2, uint8_t target) {
    dviewport_clear(PV(X_VIEWPORT), x1, y1, x2, y2, target);
}
static void fp_dx_clear_z(Footprint& f, int, int, int, int, uint8_t) { fp_result(f); }
PORT_FN(0x00458770, "dxClearZ", dxClearZ_rw, fp_dx_clear_z)

// init_xform_params (0x4587a0): dxProjectPoint's D3DTRANSFORMDATA
static void __cdecl init_xform_params_rw() {
    for (uint32_t i = 0; i < 0x34; i += 4) U32(X_XFORM + i) = 0;
    U32(X_XFORM + 0x00) = 0x34;                          // dwSize
    U32(X_XFORM + 0x04) = X_IN;                          // lpIn
    U32(X_XFORM + 0x08) = 0x20;                          // dwInSize
    U32(X_XFORM + 0x10) = 0x20;                          // dwOutSize
    U32(X_XFORM + 0x0c) = X_OUT;                         // lpOut
    U32(X_XFORM + 0x14) = X_HOUT;                        // lpHOut
    U32(X_XFORM + 0x18) = 0x3f;                          // dwClip
}
static void fp_init_xform_params(Footprint& f) { fp_at(f, X_XFORM, 0x34, "dx transform data"); }
PORT_FN(0x004587a0, "init_xform_params", init_xform_params_rw, fp_init_xform_params)

// dxProjectPoint (0x458800): one point through TransformVertices; on screen (sx, sy, sz) and 1, else (hx, hy, hz) and 0
static uint8_t __cdecl dxProjectPoint_rw(const uint32_t* in, uint32_t* out, uint8_t clip) {
    const volatile uint32_t* vi = in;
    U32(X_IN) = vi[0];
    U32(X_IN + 4) = vi[1];
    U32(X_IN + 8) = vi[2];
    uint32_t offscreen = 0;                              // (uninitialised in the original: FIX CANDIDATE)
    dviewport_transform_verts(PV(X_VIEWPORT), 1, A(X_XFORM), clip ? 1 : 2, &offscreen);   // D3DTRANSFORM_(UN)CLIPPED
    volatile uint32_t* vo = out;
    if (offscreen) {
        vo[0] = U32(X_HOUT + 4);
        vo[1] = U32(X_HOUT + 8);
        vo[2] = U32(X_HOUT + 0xc);
        return 0;
    }
    vo[0] = U32(X_OUT);
    vo[1] = U32(X_OUT + 4);
    vo[2] = U32(X_OUT + 8);
    return 1;
}
// what TransformVertices writes: the output vertex, the homogeneous one, the clip flags and extent in the
// transform data, the offscreen flag (a local)
static void fp_transform_data(Footprint& f, const uint32_t* xf, uint32_t n) {
    if (!xf) return;
    f.add((void*)xf, 0x34, "D3DTRANSFORMDATA");
    if (xf[3]) f.add((void*)(uintptr_t)xf[3], n * xf[4], "transformed vertices");
    if (xf[5]) f.add((void*)(uintptr_t)xf[5], n * 0x10, "homogeneous vertices");
}
static void fp_dx_project_point(Footprint& f, const uint32_t*, uint32_t* out, uint8_t) {
    fp_at(f, X_IN, 0xc, "dx projected point");
    fp_transform_data(f, (const uint32_t*)A(X_XFORM), 1);
    f.add(out, 0xc, "projected point");
    fp_result(f);
}
PORT_FN(0x00458800, "dxProjectPoint", dxProjectPoint_rw, fp_dx_project_point)

// dxGetCounters (0x458890)
static void __cdecl dxGetCounters_rw(int32_t* tris, int32_t* verts, int32_t* c1, int32_t* c2) {
    uint32_t st[6] = {0, 0, 0, 0, 0, 0};
    st[0] = 0x18;
    volatile int32_t *a = tris, *b = verts, *c = c1, *d = c2;
    if (d3d_GetStats(PV(X_D3D), st)) {
        *a = (int32_t)st[1];                             // dwTrianglesDrawn
        *b = (int32_t)st[5];                             // dwVerticesProcessed
        *c = I32(X_COUNT1);
        *d = I32(X_COUNT2);
        return;
    }
    *a = 0;
    *b = 0;
    *c = 0;
    *d = 0;
}
static void fp_dx_get_counters(Footprint& f, int32_t* a, int32_t* b, int32_t* c, int32_t* d) {
    f.add(a, 4, "triangles");
    f.add(b, 4, "vertices");
    f.add(c, 4, "counter 1");
    f.add(d, 4, "counter 2");
    fp_result(f);
}
PORT_FN(0x00458890, "dxGetCounters", dxGetCounters_rw, fp_dx_get_counters)

// dxDPBeginFrame (0x458920)
static void __cdecl dxDPBeginFrame_rw() { d3d_BeginFrame(PV(X_D3D)); }
static void fp_result0(Footprint& f) { fp_result(f); }
PORT_FN(0x00458920, "dxDPBeginFrame", dxDPBeginFrame_rw, fp_result0)

// dxDPEndFrame (0x458930): no texture, no blending, the texture cache forgotten, EndScene
static void __cdecl dxDPEndFrame_rw() {
    d3d_SetRenderState(PV(X_D3D), 1, 0);                 // TEXTUREHANDLE
    void* d = PV(X_D3D);
    I32(X_TEXTURE) = -1;
    d3d_SetRenderState(d, 0x1b, 0);                      // ALPHABLENDENABLE
    B8(S_ALPHA) = 0;
    d3d_EndFrame(PV(X_D3D));
}
static void fp_dx_dp_end_frame(Footprint& f) {
    fp_at(f, X_TEXTURE, 4, "dx texture");
    fp_at(f, S_ALPHA, 1, "dxState alpha");
    fp_result(f);
}
PORT_FN(0x00458930, "dxDPEndFrame", dxDPEndFrame_rw, fp_dx_dp_end_frame)

// dxEnableVertexAlpha (0x458970)
static void __cdecl dxEnableVertexAlpha_rw(uint8_t on) {
    B8(X_ALPHA_DIRTY) = 1;
    B8(X_VERTEX_ALPHA) = on;
}
static void fp_dx_vertex_alpha(Footprint& f, uint8_t) {
    fp_at(f, X_ALPHA_DIRTY, 1, "dx alpha dirty");
    fp_at(f, X_VERTEX_ALPHA, 1, "dx vertex alpha");
}
PORT_FN(0x00458970, "dxEnableVertexAlpha", dxEnableVertexAlpha_rw, fp_dx_vertex_alpha)

// dxDPBegin (0x458990) and dxBeginTriangles (0x458aa0), the same code: select the texture (-1: none), set the
// state's alpha from the vertex alpha or the texture's, flush the state. The same texture again flushes only when
// the vertex alpha changed since.
static __forceinline void begin_texture(int32_t tex) {
    int32_t old = I32(X_TEXTURE);
    if (old == tex) {
        if (!B8(X_ALPHA_DIRTY)) return;
        if (B8(X_VERTEX_ALPHA) || (old != -1 && game<uint8_t>(0x0045a590, old))) B8(S_ALPHA) = 1;   // TextureHasAlpha
        else B8(S_ALPHA) = 0;
        dxStateFlush();
        B8(X_ALPHA_DIRTY) = 0;
        return;
    }
    I32(X_TEXTURE) = tex;
    if (tex != -1) {
        if (!game<uint8_t>(0x0045a4e0, tex)) {           // TextureSelect failed: the alpha byte stays
            dxStateFlush();
            return;
        }
        if (B8(X_VERTEX_ALPHA) || game<uint8_t>(0x0045a590, tex)) B8(S_ALPHA) = 1;
        else B8(S_ALPHA) = 0;
        dxStateFlush();
        return;
    }
    d3d_SetRenderState(PV(X_D3D), 1, 0);                 // TEXTUREHANDLE 0
    if (B8(X_VERTEX_ALPHA)) B8(S_ALPHA) = 1;
    else B8(S_ALPHA) = 0;
    dxStateFlush();
}
static void fp_state_flush_writes(Footprint& f) {
    fp_at(f, S_CACHE, 0x18, "dxState cache");
    fp_at(f, S_FORCE, 1, "dxState force");
    fp_result(f);
}
static void fp_begin_texture(Footprint& f, int32_t tex) {
    if (tex != I32(X_TEXTURE) && tex != -1) {
        f.replay_only = "TextureSelect may load the texture into video memory";
        return;
    }
    fp_at(f, X_TEXTURE, 4, "dx texture");
    fp_at(f, X_ALPHA_DIRTY, 1, "dx alpha dirty");
    fp_at(f, S_ALPHA, 1, "dxState alpha");
    fp_state_flush_writes(f);
}
static void __cdecl dxDPBegin_rw(int32_t tex) { begin_texture(tex); }
PORT_FN(0x00458990, "dxDPBegin", dxDPBegin_rw, fp_begin_texture)
static void __cdecl dxBeginTriangles_rw(int32_t tex) { begin_texture(tex); }
PORT_FN(0x00458aa0, "dxBeginTriangles", dxBeginTriangles_rw, fp_begin_texture)

// the draw wrappers: straight on the device's vtable in the original (DrawPrimitive / DrawIndexedPrimitive, triangle
// lists, D3DDP_DONOTUPDATEEXTENTS, no result kept); the renderer's draw here (G2)
static void __cdecl dxDPDraw_rw(const void* v, int nv, const uint16_t* idx, int ni) {
    gfx_draw_triangles(2, v, nv, idx, ni);               // D3DVT_LVERTEX
}
static void fp_draw4(Footprint&, const void*, int, const uint16_t*, int) {}
PORT_FN_GL(0x00458a60, "dxDPDraw", dxDPDraw_rw, fp_draw4)

static void __cdecl dxDPEnd_rw() {}
PORT_FN(0x00458a90, "dxDPEnd", dxDPEnd_rw, fp_none)

static void __cdecl dxDrawTriangle_rw(const void* v) { gfx_draw_triangles(2, v, 3, 0, 0); }
static void fp_draw1(Footprint&, const void*) {}
PORT_FN_GL(0x00458b70, "dxDrawTriangle", dxDrawTriangle_rw, fp_draw1)

static void __cdecl dxDrawTriangles_rw(const void* v, int n) { gfx_draw_triangles(2, v, n, 0, 0); }
static void fp_draw2(Footprint&, const void*, int) {}
PORT_FN_GL(0x00458b90, "dxDrawTriangles", dxDrawTriangles_rw, fp_draw2)

static void __cdecl dxDrawIndexTriangles_rw(const void* v, int nv, const uint16_t* idx, int ni) {
    gfx_draw_triangles(2, v, nv, idx, ni);
}
PORT_FN_GL(0x00458bb0, "dxDrawIndexTriangles", dxDrawIndexTriangles_rw, fp_draw4)

static void __cdecl dxDrawScreenIndexTriangles_rw(const void* v, int nv, const uint16_t* idx, int ni) {
    gfx_draw_triangles(3, v, nv, idx, ni);               // D3DVT_TLVERTEX
}
PORT_FN_GL(0x00458be0, "dxDrawScreenIndexTriangles", dxDrawScreenIndexTriangles_rw, fp_draw4)

static void __cdecl dxEndTriangles_rw() {}
PORT_FN(0x00458c10, "dxEndTriangles", dxEndTriangles_rw, fp_none)

// dx_error (0x458c20): the message, then the code's name (as a format string, as the original passes it)
static void __cdecl dx_error_rw(const char* msg, int32_t hr) {
    LogReport(msg);
    const char* name = game<const char*>(0x00458c50, hr);   // dx_name_error
    LogReport(name);
}
static void fp_dx_error(Footprint& f, const char*, int32_t) { fp_at(f, X_ERRBUF, ERRBUF_BYTES, "dx_name_error's buffer"); }
PORT_FN(0x00458c20, "dx_error", dx_error_rw, fp_dx_error)

// dx_name_error (0x458c50): the original's switch, as (code, name's address) pairs
struct ErrName { uint32_t hr, name; };
static const ErrName k_err_names[] = {
    {0x80004001, 0x004f2dec}, {0x80004005, 0x004f2990}, {0x800401f0, 0x004f3230}, {0x8007000e, 0x004f2c68},
    {0x80070057, 0x004f2a24}, {0x88760005, 0x004f2914}, {0x8876000a, 0x004f2930}, {0x88760014, 0x004f294c},
    {0x88760028, 0x004f2968}, {0x88760037, 0x004f2980}, {0x8876005a, 0x004f29a0}, {0x8876005f, 0x004f29b4},
    {0x88760064, 0x004f29d0}, {0x8876006e, 0x004f29e4}, {0x88760078, 0x004f29fc}, {0x88760082, 0x004f2a10},
    {0x88760091, 0x004f2a38}, {0x88760096, 0x004f2a54}, {0x887600a0, 0x004f2a68}, {0x887600aa, 0x004f2a80},
    {0x887600b4, 0x004f2a8c}, {0x887600cd, 0x004f2a9c}, {0x887600d2, 0x004f2ab0}, {0x887600d4, 0x004f2ac4},
    {0x887600d7, 0x004f2ae0}, {0x887600dc, 0x004f2af4}, {0x887600de, 0x004f2b08}, {0x887600e1, 0x004f2b24},
    {0x887600e6, 0x004f2b3c}, {0x887600f0, 0x004f2b4c}, {0x887600fa, 0x004f2b58}, {0x887600ff, 0x004f2b6c},
    {0x88760104, 0x004f2b7c}, {0x88760118, 0x004f2b90}, {0x88760122, 0x004f2ba4}, {0x88760136, 0x004f2bb8},
    {0x8876013c, 0x004f2bcc}, {0x8876013d, 0x004f2be0}, {0x88760140, 0x004f2bf8}, {0x8876014a, 0x004f2c0c},
    {0x8876014f, 0x004f2c20}, {0x88760154, 0x004f2c30}, {0x8876015e, 0x004f2c44}, {0x88760168, 0x004f2c58},
    {0x8876017c, 0x004f2c7c}, {0x8876017e, 0x004f2c94}, {0x88760180, 0x004f2cac}, {0x88760183, 0x004f2cd0},
    {0x88760190, 0x004f2ce4}, {0x8876019a, 0x004f2cfc}, {0x887601a4, 0x004f2d1c}, {0x887601ae, 0x004f2d3c},
    {0x887601b3, 0x004f2d50}, {0x887601b8, 0x004f2d68}, {0x887601c2, 0x004f2d80}, {0x887601cc, 0x004f2d94},
    {0x887601d6, 0x004f2db0}, {0x887601e0, 0x004f2dc4}, {0x887601ea, 0x004f2dd8}, {0x887601fe, 0x004f2e00},
    {0x88760208, 0x004f2e18}, {0x88760219, 0x004f2e30}, {0x8876021c, 0x004f2e50}, {0x88760230, 0x004f2e68},
    {0x88760231, 0x004f2e78}, {0x88760232, 0x004f2e94}, {0x88760233, 0x004f2eb4}, {0x88760234, 0x004f2ecc},
    {0x88760235, 0x004f2ef0}, {0x88760236, 0x004f2f04}, {0x88760237, 0x004f2f1c}, {0x88760238, 0x004f2f38},
    {0x88760239, 0x004f2f50}, {0x8876023a, 0x004f2f60}, {0x8876023b, 0x004f2f78}, {0x8876023c, 0x004f2f90},
    {0x8876023d, 0x004f2fa8}, {0x8876023e, 0x004f2fbc}, {0x8876023f, 0x004f2fd4}, {0x88760240, 0x004f2fe4},
    {0x88760241, 0x004f2ff8}, {0x88760242, 0x004f3010}, {0x88760243, 0x004f3024}, {0x88760244, 0x004f303c},
    {0x88760245, 0x004f3058}, {0x88760246, 0x004f3078}, {0x88760247, 0x004f308c}, {0x88760248, 0x004f30a0},
    {0x88760249, 0x004f30b0}, {0x8876024a, 0x004f30c4}, {0x8876024b, 0x004f30d0}, {0x8876024c, 0x004f30e0},
    {0x8876024d, 0x004f30f8}, {0x8876024e, 0x004f310c}, {0x8876024f, 0x004f3124}, {0x88760250, 0x004f3138},
    {0x88760258, 0x004f3154}, {0x88760259, 0x004f3168}, {0x8876026c, 0x004f3178}, {0x88760276, 0x004f3190},
    {0x88760280, 0x004f31a8}, {0x88760294, 0x004f31bc}, {0x887602a8, 0x004f31d4}, {0x887602b2, 0x004f31e8},
    {0x887602b7, 0x004f31f8}, {0x887602bb, 0x004f3210}, {0x887602bc, 0x004f2334}, {0x887602bd, 0x004f234c},
    {0x887602c1, 0x004f2364}, {0x887602c2, 0x004f237c}, {0x887602c3, 0x004f2390}, {0x887602c6, 0x004f23a8},
    {0x887602c7, 0x004f23c8}, {0x887602c8, 0x004f23e8}, {0x887602c9, 0x004f2404}, {0x887602ca, 0x004f2424},
    {0x887602cb, 0x004f243c}, {0x887602cc, 0x004f2458}, {0x887602cd, 0x004f2470}, {0x887602d0, 0x004f2490},
    {0x887602d1, 0x004f24ac}, {0x887602d2, 0x004f24cc}, {0x887602d3, 0x004f24ec}, {0x887602d4, 0x004f2508},
    {0x887602d5, 0x004f2528}, {0x887602d6, 0x004f2544}, {0x887602d7, 0x004f2560}, {0x887602d8, 0x004f2578},
    {0x887602d9, 0x004f2594}, {0x887602da, 0x004f25b4}, {0x887602db, 0x004f25d0}, {0x887602dc, 0x004f25f0},
    {0x887602dd, 0x004f2610}, {0x887602de, 0x004f2630}, {0x887602df, 0x004f2650}, {0x887602e0, 0x004f2670},
    {0x887602e1, 0x004f268c}, {0x887602e2, 0x004f26a8}, {0x887602e3, 0x004f26c0}, {0x887602e4, 0x004f26dc},
    {0x887602e5, 0x004f26fc}, {0x887602e6, 0x004f271c}, {0x887602e7, 0x004f273c}, {0x887602e8, 0x004f275c},
    {0x887602e9, 0x004f2774}, {0x887602ea, 0x004f2794}, {0x887602eb, 0x004f27b4}, {0x887602ee, 0x004f27d0},
    {0x887602ef, 0x004f27e8}, {0x887602f0, 0x004f2800}, {0x887602f8, 0x004f2820}, {0x887602f9, 0x004f2838},
    {0x887602fa, 0x004f2854}, {0x887602fb, 0x004f2870}, {0x88760302, 0x004f2888}, {0x88760303, 0x004f2898},
    {0x88760304, 0x004f28ac}, {0x88760305, 0x004f28c0}, {0x88760306, 0x004f28dc}, {0x88760307, 0x004f28f8},
};
static char* __cdecl dx_name_error_rw(int32_t hr) {
    int lo = 0, hi = (int)(sizeof k_err_names / sizeof k_err_names[0]) - 1;
    while (lo <= hi) {                                   // sorted by code (unsigned)
        int mid = (lo + hi) / 2;
        uint32_t k = k_err_names[mid].hr;
        if (k == (uint32_t)hr) return (char*)(uintptr_t)k_err_names[mid].name;
        if (k < (uint32_t)hr) lo = mid + 1;
        else hi = mid - 1;
    }
    game_sprintf((char*)A(X_ERRBUF), S(0x4f3248), hr);   // "unknown error: %x"
    return (char*)A(X_ERRBUF);
}
static void fp_dx_name_error(Footprint& f, int32_t) { fp_at(f, X_ERRBUF, ERRBUF_BYTES, "dx_name_error's buffer"); }
PORT_FN(0x00458c50, "dx_name_error", dx_name_error_rw, fp_dx_name_error)

// convert_caps (0x459630): a D3DDEVICEDESC flattened into d3d_driver_desc, one byte per capability bit, in the
// original's order {source offset, mask, destination offset}
struct CapBit { uint16_t src; uint32_t mask; uint8_t dst; };
static const CapBit k_cap_bits[] = {
    // dwDevCaps
    {0x0c, 0x10, 0x02}, {0x0c, 0x20, 0x03}, {0x0c, 0x01, 0x04}, {0x0c, 0x02, 0x05}, {0x0c, 0x08, 0x06}, {0x0c, 0x04, 0x07},
    {0x0c, 0x100, 0x08}, {0x0c, 0x200, 0x09}, {0x0c, 0x40, 0x0a}, {0x0c, 0x80, 0x0b},
    // dpcTriCaps.dwMiscCaps
    {0x68, 0x08, 0x0c}, {0x68, 0x20, 0x0d}, {0x68, 0x40, 0x0e}, {0x68, 0x10, 0x0f}, {0x68, 0x01, 0x10}, {0x68, 0x02, 0x11},
    // dwRasterCaps
    {0x6c, 0x01, 0x12}, {0x6c, 0x100, 0x13}, {0x6c, 0x80, 0x14}, {0x6c, 0x200, 0x15}, {0x6c, 0x20, 0x16}, {0x6c, 0x40, 0x17},
    {0x6c, 0x10, 0x18}, {0x6c, 0x2000, 0x19},
    // dwSrcBlendCaps
    {0x74, 0x01, 0x1a}, {0x74, 0x02, 0x1b}, {0x74, 0x04, 0x1c}, {0x74, 0x08, 0x1d}, {0x74, 0x10, 0x1e}, {0x74, 0x20, 0x1f},
    {0x74, 0x100, 0x20}, {0x74, 0x200, 0x21}, {0x74, 0x40, 0x22}, {0x74, 0x80, 0x23}, {0x74, 0x800, 0x24}, {0x74, 0x1000, 0x25},
    {0x74, 0x400, 0x26},
    // dwDestBlendCaps
    {0x78, 0x01, 0x27}, {0x78, 0x02, 0x28}, {0x78, 0x04, 0x29}, {0x78, 0x08, 0x2a}, {0x78, 0x10, 0x2b}, {0x78, 0x20, 0x2c},
    {0x78, 0x100, 0x2d}, {0x78, 0x200, 0x2e}, {0x78, 0x40, 0x2f}, {0x78, 0x80, 0x30}, {0x78, 0x800, 0x31}, {0x78, 0x1000, 0x32},
    {0x78, 0x400, 0x33},
    // dwZCmpCaps
    {0x70, 0x80, 0x34}, {0x70, 0x04, 0x35}, {0x70, 0x10, 0x36}, {0x70, 0x40, 0x37}, {0x70, 0x02, 0x38}, {0x70, 0x08, 0x39},
    {0x70, 0x01, 0x3a}, {0x70, 0x20, 0x3b},
    // dwAlphaCmpCaps
    {0x7c, 0x80, 0x3c}, {0x7c, 0x04, 0x3d}, {0x7c, 0x10, 0x3e}, {0x7c, 0x40, 0x3f}, {0x7c, 0x02, 0x40}, {0x7c, 0x08, 0x41},
    {0x7c, 0x01, 0x42}, {0x7c, 0x20, 0x43},
    // dwShadeCaps
    {0x80, 0x01, 0x44}, {0x80, 0x02, 0x46}, {0x80, 0x40, 0x45}, {0x80, 0x80, 0x47}, {0x80, 0x1000, 0x48}, {0x80, 0x2000, 0x49},
    {0x80, 0x40000, 0x4a}, {0x80, 0x04, 0x4b}, {0x80, 0x08, 0x4d}, {0x80, 0x100, 0x4c}, {0x80, 0x200, 0x4e}, {0x80, 0x4000, 0x4f},
    {0x80, 0x8000, 0x50}, {0x80, 0x80000, 0x51}, {0x80, 0x10, 0x52}, {0x80, 0x20, 0x54}, {0x80, 0x400, 0x53}, {0x80, 0x800, 0x55},
    {0x80, 0x10000, 0x56}, {0x80, 0x20000, 0x57}, {0x80, 0x100000, 0x58},
    // dwTextureCaps
    {0x84, 0x01, 0x59}, {0x84, 0x02, 0x5a}, {0x84, 0x04, 0x5b}, {0x84, 0x08, 0x5c}, {0x84, 0x20, 0x5d},
    // dwTextureFilterCaps
    {0x88, 0x01, 0x5e}, {0x88, 0x02, 0x5f}, {0x88, 0x04, 0x60}, {0x88, 0x08, 0x61}, {0x88, 0x10, 0x62}, {0x88, 0x20, 0x63},
    // dwTextureBlendCaps
    {0x8c, 0x01, 0x64}, {0x8c, 0x02, 0x65}, {0x8c, 0x04, 0x66}, {0x8c, 0x08, 0x67}, {0x8c, 0x10, 0x68}, {0x8c, 0x20, 0x69},
    {0x8c, 0x40, 0x6a},
    // dwTextureAddressCaps
    {0x90, 0x01, 0x6b}, {0x90, 0x02, 0x6c}, {0x90, 0x04, 0x6d},
};
static void __cdecl convert_caps_rw(const uint8_t* desc, uint8_t* out) {
    const volatile uint8_t* d = desc;
    volatile uint8_t* o = out;
    if ((*(const volatile uint32_t*)(d + 4) & 0x1c3) != 0x1c3) {   // dwFlags: COLORMODEL|DEVCAPS|TRICAPS|... valid
        LogReport(S(0x4f325c));                          // "bad driver info!"
        return;
    }
    o[1] = (*(const volatile uint32_t*)(d + 8) & 1) ? 1 : 0;   // D3DCOLOR_MONO
    o[0] = (*(const volatile uint32_t*)(d + 8) & 2) ? 1 : 0;   // D3DCOLOR_RGB
    if (!(d[8] & 3)) {
        LogReport(S(0x4f325c));
        return;
    }
    for (const CapBit& b : k_cap_bits) o[b.dst] = (*(const volatile uint32_t*)(d + b.src) & b.mask) ? 1 : 0;
    // the render and z-buffer depths (DDBD_ bits of byte 1) into bit masks, OR'd into what's there
    static const uint8_t k_depth[7] = {0x40, 0x20, 0x10, 8, 4, 2, 1};
    for (int i = 0; i < 7; i++)
        if (d[0x9d] & k_depth[i]) *(volatile uint32_t*)(out + 0x70) = *(volatile uint32_t*)(out + 0x70) | (1u << i);
    for (int i = 0; i < 7; i++)
        if (d[0xa1] & k_depth[i]) *(volatile uint32_t*)(out + 0x74) = *(volatile uint32_t*)(out + 0x74) | (1u << i);
    *(volatile uint32_t*)(out + 0x78) = *(const volatile uint32_t*)(d + 0xa4);   // dwMaxBufferSize
    *(volatile uint32_t*)(out + 0x7c) = *(const volatile uint32_t*)(d + 0xa8);   // dwMaxVertexCount
}
static void fp_convert_caps(Footprint& f, const uint8_t*, uint8_t* out) {
    f.add(out, 0x80, "d3d_driver_desc");
}
PORT_FN(0x00459630, "convert_caps", convert_caps_rw, fp_convert_caps)

// =====================================================================================================================
// dxstate.obj
// =====================================================================================================================

// dxStateInit (0x45de20)
static void __cdecl dxStateInit_rw() {
    if (VidHas3DHW()) {
        dxSetColorKeyEnable(1);
        dxSetFillMode(3);                                // SOLID
        dxSetBlendParams(5, 6);                          // SRCALPHA, INVSRCALPHA
        dxEnableZbuffer(1);
        dxSetZMode(4);                                   // LESSEQUAL
        dxSetTextureWrapMode(1, 0, 0);                   // WRAP
        d3d_SetRenderState(PV(X_D3D), 0x19, 6);          // ALPHAFUNC NOTEQUAL
        d3d_SetRenderState(PV(X_D3D), 0x18, 0);          // ALPHAREF 0
    } else {
        dxSetFillMode(3);
        dxEnableZbuffer(1);
        dxSetZMode(4);
        dxSetShadeMode(1);                               // FLAT
        dxSetBlendMode(7);                               // COPY
        dxSetFilterMode(1, 1);                           // NEAREST
        dxEnablePerspectiveCorrect(1);
    }
    B8(S_FORCE) = 1;
    PV(S_PTR) = 0;
}
static void fp_dx_state_init(Footprint& f) {
    fp_at(f, S_FORCE, 1, "dxState force");
    fp_at(f, S_PTR, 4, "dxState pointer");
    fp_result(f);
}
PORT_FN(0x0045de20, "dxStateInit", dxStateInit_rw, fp_dx_state_init)

// dxStateClear (0x45def0)
static void __cdecl dxStateClear_rw() {}
PORT_FN(0x0045def0, "dxStateClear", dxStateClear_rw, fp_none)

// dxStateUpdate (0x45df00): the state to apply; forced, the cache set to the opposite of every flag
static void __cdecl dxStateUpdate_rw(const uint8_t* s) {
    PV(S_PTR) = (void*)s;
    if (!B8(S_FORCE)) return;
    const volatile uint8_t* v = s;
    B8(S_CACHE + 0x00) = v[0x00] == 0;
    B8(S_CACHE + 0x01) = v[0x01] == 0;
    B8(S_CACHE + 0x08) = v[0x08] == 0;
    B8(S_CACHE + 0x09) = v[0x09] == 0;
    B8(S_CACHE + 0x0a) = v[0x0a] == 0;
    B8(S_CACHE + 0x0b) = v[0x0b] == 0;
    B8(S_CACHE + 0x0c) = v[0x0c] == 0;
    B8(S_CACHE + 0x0d) = v[0x0d] == 0;
    uint8_t e = v[0x0e] == 0;
    I32(S_CACHE + 0x10) = -1;                            // the blend mode
    B8(S_CACHE + 0x0e) = e;
    B8(S_CACHE + 0x14) = v[0x14] == 0;
    B8(S_CACHE + 0x15) = v[0x15] == 0;
    B8(S_CACHE + 0x16) = v[0x16] == 0;
    B8(S_CACHE + 0x17) = v[0x17] == 0;
}
static void fp_dx_state_update(Footprint& f, const uint8_t*) {
    fp_at(f, S_PTR, 4, "dxState pointer");
    fp_at(f, S_CACHE, 0x18, "dxState cache");
}
PORT_FN(0x0045df00, "dxStateUpdate", dxStateUpdate_rw, fp_dx_state_update)

// dxStateFlush (0x45dfd0): each field that differs from the cache sent to the device; the cache updated if any did
static __forceinline const volatile uint8_t* cur_state() { return (const volatile uint8_t*)PV(S_PTR); }
static void __cdecl dxStateFlush_rw() {
    if (!VidHas3DHW()) return;
    uint8_t changed = 0;
    uint8_t a = cur_state()[0x00];                       // lighting: shading and texture blend
    if (B8(S_CACHE + 0x00) != a) {
        changed = 1;
        if (a) {
            dxSetShadeMode(2);                           // GOURAUD
            dxSetBlendMode(B8(CAPS_MODALPHA) ? 4 : 2);   // MODULATEALPHA / MODULATE
        } else {
            dxSetShadeMode(1);                           // FLAT
            dxSetBlendMode(1);                           // DECAL
        }
    }
    a = cur_state()[0x14];                               // mirrored: the cull mode
    if (B8(S_CACHE + 0x14) != a) {
        changed = 1;
        dxSetCullMode(a ? 2 : 3);                        // CW / CCW
    }
    a = cur_state()[0x08];
    if (B8(S_CACHE + 0x08) != a) {
        changed = 1;
        dxEnableZbuffer(a ? 1 : 0);
    }
    a = cur_state()[0x09];
    if (B8(S_CACHE + 0x09) != a) {
        changed = 1;
        dxEnableZWrites(a ? 1 : 0);
    }
    a = cur_state()[0x0a];
    if (B8(S_CACHE + 0x0a) != a) {
        changed = 1;
        dxEnablePerspectiveCorrect(a ? 1 : 0);
    }
    uint8_t filt = cur_state()[0x0b];                    // filtering and mipmapping
    if (B8(S_CACHE + 0x0b) != filt || cur_state()[0x15] != B8(S_CACHE + 0x15)) {
        changed = 1;
        if (filt) {
            if (cur_state()[0x15]) {
                dxSetFilterMode(4, 2);                   // MIPLINEAR, LINEAR
                if (B8(CAPS_LODBIAS)) dxSetLODBias(0x3d4ccccd);   // 0.05f
            } else {
                dxSetFilterMode(2, 2);                   // LINEAR, LINEAR
                if (B8(CAPS_LODBIAS)) dxSetLODBias(0);
            }
            dxEnableDither(1);
        } else {
            if (cur_state()[0x15]) {
                dxSetFilterMode(3, 1);                   // MIPNEAREST, NEAREST
                if (B8(CAPS_LODBIAS)) dxSetLODBias(0x3d4ccccd);
            } else {
                dxSetFilterMode(1, 1);                   // NEAREST, NEAREST
                if (B8(CAPS_LODBIAS)) dxSetLODBias(0);
            }
            dxEnableDither(0);
        }
    }
    a = cur_state()[0x0c];
    if (B8(S_CACHE + 0x0c) != a) {
        changed = 1;
        dxEnableSpecular(a ? 1 : 0);
    }
    a = cur_state()[0x0d];
    if (B8(S_CACHE + 0x0d) != a) {
        changed = 1;
        dxEnableAntialias(a ? 1 : 0);
    }
    uint8_t alpha_changed = 0;
    a = cur_state()[0x0e];                               // alpha: blending and the alpha test together
    if (B8(S_CACHE + 0x0e) != a) {
        changed = 1;
        alpha_changed = 1;
        dxEnableAlphaBlend(a ? 1 : 0);
        dxEnableAlphaTest(a ? 1 : 0);
    }
    int32_t blend = *(const volatile int32_t*)(cur_state() + 0x10);
    if (I32(S_CACHE + 0x10) != blend) {
        changed = 1;
        switch (blend) {
        case 0: dxSetBlendParams(5, 6); break;           // SRCALPHA, INVSRCALPHA
        case 1: dxSetBlendParams(5, 2); break;           // SRCALPHA, ONE
        case 2: dxSetBlendParams(9, 1); break;           // DESTCOLOR, ZERO
        case 3: dxSetBlendParams(5, 1); break;           // SRCALPHA, ZERO
        case 4: dxSetBlendParams(2, 2); break;           // ONE, ONE
        default: LogPanic(S(0x4f3a40)); break;           // "unknown alpha blend mode"
        }
    }
    a = cur_state()[0x01];                               // fog, never with alpha
    if (B8(S_CACHE + 0x01) != a || (a && alpha_changed)) {
        changed = 1;
        dxEnableFog(a && !cur_state()[0x0e] ? 1 : 0);
    }
    uint32_t fog = *(const volatile uint32_t*)(cur_state() + 4);
    if (U32(S_CACHE + 4) != fog) {
        changed = 1;
        dxSetFogColor(fog);
    }
    uint8_t wu = cur_state()[0x16];                      // wrap U / V
    if (B8(S_CACHE + 0x16) != wu || cur_state()[0x17] != B8(S_CACHE + 0x17)) {
        changed = 1;
        if (wu) {
            if (cur_state()[0x17]) {
                dxSetTextureWrapMode(1, 0, 0);
            } else {
                dxSetTextureWrapMode(1, 0, 0);
                dxSetTextureWrapModeU(1);
                dxSetTextureWrapModeV(3);
            }
        } else if (cur_state()[0x17]) {
            dxSetTextureWrapMode(1, 0, 0);
            dxSetTextureWrapModeU(3);
            dxSetTextureWrapModeV(1);
        } else {
            dxSetTextureWrapMode(3, 0, 0);
            dxSetTextureWrapModeU(3);
            dxSetTextureWrapModeV(3);
        }
    }
    B8(S_FORCE) = 0;
    if (changed) {
        const volatile uint32_t* src = (const volatile uint32_t*)PV(S_PTR);
        for (int i = 0; i < 6; i++) U32(S_CACHE + 4 * i) = src[i];   // rep movsd
    }
}
static void fp_dx_state_flush(Footprint& f) { fp_state_flush_writes(f); }
PORT_FN(0x0045dfd0, "dxStateFlush", dxStateFlush_rw, fp_dx_state_flush)

// dxStateDrawSolid (0x45e390), dxStateDrawTransp (0x45e3d0), dxStateDrawNormal (0x45e410): no callers in v1.0
static void __cdecl dxStateDrawSolid_rw() {
    d3d_SetRenderState(PV(X_D3D), 0xf, 1);               // ALPHATESTENABLE
    d3d_SetRenderState(PV(X_D3D), 0x19, 5);              // ALPHAFUNC GREATER
    d3d_SetRenderState(PV(X_D3D), 0x18, 0xa000);         // ALPHAREF
}
PORT_FN(0x0045e390, "dxStateDrawSolid", dxStateDrawSolid_rw, fp_result0)
static void __cdecl dxStateDrawTransp_rw() {
    d3d_SetRenderState(PV(X_D3D), 0xf, 1);
    d3d_SetRenderState(PV(X_D3D), 0x19, 4);              // ALPHAFUNC LESSEQUAL
    d3d_SetRenderState(PV(X_D3D), 0x18, 0xa000);
}
PORT_FN(0x0045e3d0, "dxStateDrawTransp", dxStateDrawTransp_rw, fp_result0)
static void __cdecl dxStateDrawNormal_rw() { d3d_SetRenderState(PV(X_D3D), 0xf, 0); }
PORT_FN(0x0045e410, "dxStateDrawNormal", dxStateDrawNormal_rw, fp_result0)

// the one-state setters: d3d::SetRenderState(state, value); the flags zero-extended from a byte
static void fp_set_b(Footprint& f, uint8_t) { fp_result(f); }
static void fp_set_u(Footprint& f, uint32_t) { fp_result(f); }
static void fp_set_uu(Footprint& f, uint32_t, uint32_t) { fp_result(f); }
static void __cdecl dxSetColorKeyEnable_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 0x29, on); }
PORT_FN(0x0045e420, "dxSetColorKeyEnable", dxSetColorKeyEnable_rw, fp_set_b)
static void __cdecl dxSetCullMode_rw(uint32_t v) { d3d_SetRenderState(PV(X_D3D), 0x16, v); }
PORT_FN(0x0045e440, "dxSetCullMode", dxSetCullMode_rw, fp_set_u)
static void __cdecl dxSetTextureWrapMode_rw(uint32_t addr, uint8_t wu, uint8_t wv) {
    d3d_SetRenderState(PV(X_D3D), 3, addr);              // TEXTUREADDRESS
    d3d_SetRenderState(PV(X_D3D), 5, wu);                // WRAPU
    d3d_SetRenderState(PV(X_D3D), 6, wv);                // WRAPV
}
static void fp_wrap_mode(Footprint& f, uint32_t, uint8_t, uint8_t) { fp_result(f); }
PORT_FN(0x0045e460, "dxSetTextureWrapMode", dxSetTextureWrapMode_rw, fp_wrap_mode)
static void __cdecl dxSetTextureWrapModeU_rw(uint32_t v) { d3d_SetRenderState(PV(X_D3D), 0x2c, v); }
PORT_FN(0x0045e4a0, "dxSetTextureWrapModeU", dxSetTextureWrapModeU_rw, fp_set_u)
static void __cdecl dxSetTextureWrapModeV_rw(uint32_t v) { d3d_SetRenderState(PV(X_D3D), 0x2d, v); }
PORT_FN(0x0045e4c0, "dxSetTextureWrapModeV", dxSetTextureWrapModeV_rw, fp_set_u)
static void __cdecl dxEnablePerspectiveCorrect_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 4, on); }
PORT_FN(0x0045e4e0, "dxEnablePerspectiveCorrect", dxEnablePerspectiveCorrect_rw, fp_set_b)
static void __cdecl dxSetFillMode_rw(uint32_t v) { d3d_SetRenderState(PV(X_D3D), 8, v); }
PORT_FN(0x0045e500, "dxSetFillMode", dxSetFillMode_rw, fp_set_u)
static void __cdecl dxSetShadeMode_rw(uint32_t v) { d3d_SetRenderState(PV(X_D3D), 9, v); }
PORT_FN(0x0045e520, "dxSetShadeMode", dxSetShadeMode_rw, fp_set_u)
static void __cdecl dxEnableAntialias_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 2, on); }
PORT_FN(0x0045e540, "dxEnableAntialias", dxEnableAntialias_rw, fp_set_b)
static void __cdecl dxEnableZbuffer_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 7, on); }
PORT_FN(0x0045e560, "dxEnableZbuffer", dxEnableZbuffer_rw, fp_set_b)
static void __cdecl dxSetZMode_rw(uint32_t v) { d3d_SetRenderState(PV(X_D3D), 0x17, v); }
PORT_FN(0x0045e580, "dxSetZMode", dxSetZMode_rw, fp_set_u)
static void __cdecl dxEnableZWrites_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 0xe, on); }
PORT_FN(0x0045e5a0, "dxEnableZWrites", dxEnableZWrites_rw, fp_set_b)
static void __cdecl dxEnableFog_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 0x1c, on); }
PORT_FN(0x0045e5c0, "dxEnableFog", dxEnableFog_rw, fp_set_b)
static void __cdecl dxSetFogColor_rw(uint32_t c) { d3d_SetRenderState(PV(X_D3D), 0x22, c); }
PORT_FN(0x0045e5e0, "dxSetFogColor", dxSetFogColor_rw, fp_set_u)
static void __cdecl dxSetFilterMode_rw(uint32_t mn, uint32_t mg) {
    d3d_SetRenderState(PV(X_D3D), 0x12, mn);             // TEXTUREMIN
    d3d_SetRenderState(PV(X_D3D), 0x11, mg);             // TEXTUREMAG
}
PORT_FN(0x0045e600, "dxSetFilterMode", dxSetFilterMode_rw, fp_set_uu)
static void __cdecl dxSetBlendParams_rw(uint32_t src, uint32_t dst) {
    d3d_SetRenderState(PV(X_D3D), 0x13, src);            // SRCBLEND
    d3d_SetRenderState(PV(X_D3D), 0x14, dst);            // DESTBLEND
}
PORT_FN(0x0045e630, "dxSetBlendParams", dxSetBlendParams_rw, fp_set_uu)
static void __cdecl dxSetBlendMode_rw(uint32_t v) { d3d_SetRenderState(PV(X_D3D), 0x15, v); }
PORT_FN(0x0045e660, "dxSetBlendMode", dxSetBlendMode_rw, fp_set_u)
static void __cdecl dxEnableDither_rw(uint8_t on) {
    if (B8(CAPS_DITHER)) d3d_SetRenderState(PV(X_D3D), 0x1a, on);
}
PORT_FN(0x0045e680, "dxEnableDither", dxEnableDither_rw, fp_set_b)
static void __cdecl dxEnableAlphaBlend_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 0x1b, on); }
PORT_FN(0x0045e6a0, "dxEnableAlphaBlend", dxEnableAlphaBlend_rw, fp_set_b)
static void __cdecl dxEnableAlphaTest_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 0xf, on); }
PORT_FN(0x0045e6c0, "dxEnableAlphaTest", dxEnableAlphaTest_rw, fp_set_b)
static void __cdecl dxEnableSpecular_rw(uint8_t on) { d3d_SetRenderState(PV(X_D3D), 0x1d, on); }
PORT_FN(0x0045e6e0, "dxEnableSpecular", dxEnableSpecular_rw, fp_set_b)
static void __cdecl dxSetLODBias_rw(uint32_t bits) { d3d_SetRenderState(PV(X_D3D), 0x2e, bits); }   // the float's bits
PORT_FN(0x0045e700, "dxSetLODBias", dxSetLODBias_rw, fp_set_u)

}  // namespace
