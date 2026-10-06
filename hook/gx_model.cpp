// gx_model.cpp -- M3 graphics stage, step G1, group G1b: models, the CPU lighting and the model renderer, rewritten
// faithfully (library `gx`).
//
//   mr.obj        the renderer's front end: mrBegin / mrEnd, the mode set-up and teardown (mr_set_mode,
//                 mr_restore_mode, mr_release, mr_restore, mr_connect_3d ...), frames, the view rectangle and its
//                 Z clear, the background colour, the projection (calc_projection_matrix: fptan) and the mirror,
//                 the camera (mrSetCamera, mrCameraLook, distances, visibility tests), point projection, the
//                 immediate triangle calls and the dashboard's translucent rectangles
//   dxmatrix.obj  the WORLD / VIEW / PROJECTION transforms (d3d::SetTransform)
//   feature.obj   mrEnable / mrDisable / mrIsEnabled (the mrFeature bits onto the dxState block), the blend mode,
//                 and the feature stack (mrPushState / mrPopState)
//   light.obj     the CPU lighting into D3DLVERTEX: light_begin (the sun, the tables), placing the camera, lights
//                 and models, the day / night / fog vertex passes (calc_day, calc_night, calc_night_2,
//                 mrLightFogVerts), the per-surface lighting functions light_{off,pre,nopre}_{nofog,fog}_
//                 {noenv,someenv,env} picked by set_light_state, the sphere-map UVs (APPLY_ENVMAP) and the fog
//                 factor (APPLY_FOG)
//   model.obj     the model pool (mr_model_begin / end), loading, copying, building (direct_model_build: the
//                 index buffer and one model_surface_info per surface with its texture), drawing
//                 (direct_model_draw, mrModelDraw with its env-map second pass), picking, extents, the .mod file
//                 (mrModelInfoGet / Save), and the deferred surfaces (the late, texture-bucketed track draw)
//
// Everything runs on the MAIN thread, so every footprint lists the statics its function writes (no physics-globals
// save applies). Calls go to the game's functions by their v1.0 addresses -- this group's own too -- so a hooked
// rewrite is what runs; DirectX is reached only through the dx / dd / texture layers (other groups), never directly.
// What allocates or frees heap memory (pools, MemAlloc, the texture loads behind gxGetTexture and TextureSelect,
// resources, files) is replay_only: every draw path is, since dxDPBegin / dxBeginTriangles -> TextureSelect ->
// txLoadVideo can load a texture on the spot.
//
// x87 (hook/x87.h): a value the original keeps in a register is a double here, a stored one a float; every stored
// float the original makes is made (st(), docs/PORTING.md 10a), including the dead stores of the last fog distance
// that the lighting loops pop into a local on their way out. The lighting's float -> int conversions are the
// original's magic-number trick (fadd 2^52 + 2^31; fstp qword; the low dword less 2^31) -- done on the x87 at the
// current precision like the original's, so a 24-bit precision control gives the same (useless) result the
// original gets. fsin / fcos of the sun's angles go through x87.h; calc_projection_matrix's fptan is done in one asm
// block that is the original's instruction sequence (fptan's result is not rounded by precision control, and an
// argument out of fptan's range leaves the x87 stack a register short, which the rest of the sequence inherits).
//
// M1 (viperport.cpp) lifts three things here: the 'surf info' and 'models' pool sizes (mr_model_begin), the
// 'deferr pool' size and the deferred-surface buckets (begin_deferred: the bucket array's address and its
// 120-entry clear). The rewrites read those operands with m1_operand. The three deferred-surface functions
// (add_deferred_surf, end_deferred_surfs, draw_alpha_deferred_surfs) were M1's own C++ with 1,024 buckets; they are
// ported here as faithful rewrites that take the bucket array and its size from begin_deferred's operands: with the
// stock operands (a harness, or the lift not applied) they are the originals exactly -- 120 buckets, 119 of them
// drawn, texture 119 and up written past the array -- and with M1's they are M1's lifted buckets (below texture 118
// the same buckets, order and passes as the stock ones). They are build-agnostic (addresses through A()) and carry
// the race.bin builds' prologues, as M1's hooks did.
//
// Fixes (docs/PORTING.md, "Fixes"; each marked FIX:, off in a VP_FAITHFUL build): 16-character names in the 16-byte name
// fields read as terminated names (name16); a missing model logged and loaded as an empty model instead of a panic and a
// read through NULL; a model with no surfaces queues nothing when deferred; the lighting tables' indices clamped to the
// tables; mrLightSpecial's light number bounded to the eight lights.
//
// Skipped: the $E static initialisers of all five objects (the CRT runs them before any hook exists; light.obj's $E53
// too), and the bare `ret` stubs, too short to hook and with nothing to port: dxMatrixEnd (0x45db70), dxMatrixRelease
// (0x45db80), dxMatrixRestore (0x45db90), mr_feature_end (0x457880), mr_model_release (0x4558a0), mr_model_restore
// (0x4558b0), mrModelFinishDeferredMode (0x455ce0), model.obj's ASSERT_MSG (0x455eb0), light_end (0x45b140), and
// P3DBase::P3DBase (0x45d8d0: `mov eax, ecx; ret`, 3 bytes).
//
// Layouts (v1.0; from the disassembly):
//   mrModelInfo (0x28): +0 nverts, +4 mrVertex* (32: x y z, nx ny nz, u v), +8 nsurfaces, +0xc mrSurface* (32:
//     name[16], +0x10 u8 type (0 lit, 1 lit + env map, 2 full bright), +0x11 u8, +0x14 / +0x16 u16, +0x18 s16 first
//     vertex, +0x1a s16 end vertex, +0x1c s16 first triangle, +0x1e s16 end triangle), +0x10 ntriangles, +0x14
//     triangles (8: s16 v0 v1 v2, u16), +0x18 n / +0x1c 16-byte records, +0x20 n / +0x24 4-byte records
//   model_info ('models' pool, 0x30; the mrModel handle IS its address): +0 name[16], +0x10 mrModelInfo*, +0x14 u8
//     owns the info (a copy: MemFree'd), +0x15 u8 pre-lit, +0x18 remap table, +0x1c its count, +0x20 index count,
//     +0x24 u16 indices (MemAlloc), +0x28 model_surface_info*, +0x2c next model
//   model_surface_info ('surf info' pool, 0x2c): +0 texture name[16], +0x10 remap value, +0x14 texture, +0x18 u16*
//     its indices, +0x1c index count, +0x20 its vertices in the lit buffer, +0x24 vertex count, +0x28 next
//   mrModelRemapInfo (0x28): +0 name[16], +0x10 new name[16], +0x20 value, +0x24 int* (gets the texture)
//   deferred_entry ('deferr pool', 0x10): +0 model_surface_info*, +4 mrSurface*, +8 mrModelInfo*, +0xc next
//   PoolBase (0x20): +0 vtable, +4 u8 panic when empty, +8 name, +0xc storage, +0x10 free list, +0x14 count, +0x18
//     element size, +0x1c in use
//   D3DLVERTEX (32): x y z, +0xc reserved, +0x10 colour ARGB, +0x14 specular (alpha = the fog factor), +0x18 u, v
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
#define FB(b) __builtin_bit_cast(float, (uint32_t)(b))   // a float constant from its bits
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

namespace {
template <typename T> static __forceinline T* P(uint32_t addr) { return (T*)(uintptr_t)addr; }
static __forceinline float& Fa(uint32_t addr) { return *(float*)(uintptr_t)addr; }
static __forceinline int32_t& Ia(uint32_t addr) { return *(int32_t*)(uintptr_t)addr; }
static __forceinline uint32_t& Ua(uint32_t addr) { return *(uint32_t*)(uintptr_t)addr; }
static __forceinline uint8_t& Ba(uint32_t addr) { return *(uint8_t*)(uintptr_t)addr; }
static __forceinline float& Fp(const void* p, uint32_t off) { return *(float*)((uint8_t*)p + off); }
static __forceinline int32_t& Ip(const void* p, uint32_t off) { return *(int32_t*)((uint8_t*)p + off); }
static __forceinline uint32_t& Up(const void* p, uint32_t off) { return *(uint32_t*)((uint8_t*)p + off); }
static __forceinline int16_t& Sp(const void* p, uint32_t off) { return *(int16_t*)((uint8_t*)p + off); }
static __forceinline uint16_t& Wp(const void* p, uint32_t off) { return *(uint16_t*)((uint8_t*)p + off); }
static __forceinline uint8_t& Bp(const void* p, uint32_t off) { return *((uint8_t*)p + off); }
static __forceinline uint8_t*& Pp(const void* p, uint32_t off) { return *(uint8_t**)((uint8_t*)p + off); }
static __forceinline uint8_t* At(const void* p, int32_t off) { return (uint8_t*)p + off; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }          // an integer move of a float
static __forceinline void cp12(void* d, const void* s) {                            // x, then y, then z
    cp4(d, s);
    cp4((uint8_t*)d + 4, (const uint8_t*)s + 4);
    cp4((uint8_t*)d + 8, (const uint8_t*)s + 8);
}
// An fst / fstp dword the original always makes, kept even where the float then goes unused (docs/PORTING.md 10a).
static __forceinline float st(double d) {
    volatile float f = (float)d;
    return f;
}
static __forceinline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
// fadd qword [4503601774854144.0] (2^52 + 2^31); fstp qword; mov eax, low dword; sub eax, 0x80000000 -- the lighting's
// float -> int, on the x87 at the current precision control like the original's
static __forceinline int32_t magic_int(double v) {
    volatile double q = v + 4503601774854144.0;
    double t = q;
    uint64_t b;
    memcpy(&b, &t, 8);
    return (int32_t)((uint32_t)b - 0x80000000u);
}
}  // namespace

// A fix branch taken (docs/PORTING.md "Fixes"): nothing in the DLL; the harness counts them to tell a world a fix changed
// from an ordinary one, which must still give the original's bits.
#ifndef GX_FIX_FIRED
#define GX_FIX_FIRED() ((void)0)
#endif

// FIX: a name in a 16-byte field (a .mod surface's texture name, a remap entry's, a model_surface_info's) is 16
// characters with no terminator when it is exactly 16 long -- legal, since the resource and texture layers take 16-character
// names -- and the original then reads on into whatever follows the field (the surface type, the remap value) as more
// name, so the texture it asks for and every comparison went wrong. Such a name is read here as its 16 characters with a
// terminator; one with a terminator inside the field is handed on as it is (the original's bytes).
static __forceinline const char* name16(const char* s, char* buf17) {
    if (VP_FIX && s && strnlen(s, 16) == 16) {
        memcpy(buf17, s, 16);
        buf17[16] = 0;
        GX_FIX_FIRED();
        return buf17;
    }
    return s;
}

// ---- statics ---------------------------------------------------------------------------------------------------
enum : uint32_t {
    // mr.obj
    S_MR_OK = 0x004eefd8,           // u8: the mode is set (mr_ok)
    S_MR_ON = 0x004eefdc,           // u8: between mrBegin and mrEnd
    S_PROJ = 0x005228f8,            // dx_matrix projection (0x40)
    S_CAMMAT = 0x00522960,          // dx_matrix view (0x40); +0x30 the camera's translation
    S_VIEW = 0x005229a0,            // mrView: x, y, centre x, centre y, w, h, w/2, h/2 (8 ints)
    S_NEAR = 0x005229c0, S_FAR = 0x005229c4, S_FOV = 0x005229cc,
    S_FEAT2 = 0x005229d4,           // u8: mrFeature 2
    S_FEAT4 = 0x005229d5,           // u8: mrFeature 4 (specular)
    S_DXSTATE = 0x005229d8,         // dxState (0x18)
    S_CAMDIR = 0x005229f0,          // P3: the camera's forward axis
    S_CAMPOS = 0x00522a00,          // P3: the camera's position
    S_BGCOLOR = 0x00522a0c,         // 0xffRRGGBB
    S_MRCAPS = 0x00522a18,          // dx_driver_info; +4 the alpha level
    S_ALPHA_LEVEL = 0x00522a1c,
    S_SCREEN_HIT = 0x00522ae4, S_SCREEN_WID = 0x00522b08,
    S_GX_HIT = 0x005228d4, S_GX_WID = 0x005228f4,
    S_D3D = 0x00522e64,             // d3d* (dx.obj)
    S_IDENTITY_MAT = 0x004f3a00,    // dx_matrix identity (dxmatrix.obj)
    // feature.obj
    S_FSTACK_IDX = 0x004f22cc,      // the feature stack's top
    S_FSTACK = 0x00522df8,          // {mask, blend mode}[]
    // model.obj
    S_SURF_POOL = 0x00522b34,       // Pool<model_surface_info>*
    S_BUCKETS = 0x00522b40,         // deferred_entry* [120] (stock; M1 repoints begin_deferred at its own)
    S_IDENT_FRAME = 0x00522d28,     // an identity Frame (0x30)
    S_MODELS = 0x00522d5c,          // model_info list head
    S_MINF_VERSION = 0x00522d64,    // the .mod version mrModelInfoSave writes
    S_CLIP_FAR = 0x00522d70, S_CLIP_NEAR = 0x00522d78,
    S_DEFER_POOL = 0x00522d80,      // Pool<deferred_entry>*
    S_DEFER_ON = 0x00522d84,        // u8: surfaces are being deferred (begin_deferred_surfs)
    S_MODEL_POOL = 0x00522da8,      // Pool<model_info>*
    S_MINF_V0 = 0x004f217c, S_MINF_V1 = 0x004f2180,   // the .mod versions mrModelInfoGet accepts (0, 1)
    S_HAS_ALPHA = 0x004f2184,       // u8
    S_DEFERRED = 0x004f2188,        // u8: mrModelEnterDeferredMode
    S_ENVMAP = 0x004f218c,          // u8: mrModelEnvMap
    S_ENVPASS = 0x004f2190,         // u8: mrModelDraw's env-map pass is on
    S_ENV_TEX = 0x004f2194,         // the env-map texture ("envmap", mr_model_first)
    S_LIT_BUF = 0x004f2198,         // D3DLVERTEX[1500] stock (the rewrite: 32768, port.h), the lit vertices
    // light.obj
    L_ON = 0x00525f50,              // u8: lighting is calculated
    L_MODE = 0x00525f54,            // mrLMode: 0 day, 1 night
    L_CAMPOS = 0x00525f58, L_CAMDIR = 0x00525f64,  // the camera (mrLightPlaceCamera)
    L_SUN = 0x00525f70,             // P3: the sun's direction
    L_BASE = 0x00525f7c,            // int: the diffuse table's zero (0x80)
    L_SCALE = 0x00525f80,           // float: the tables' scale
    L_MSUN = 0x00525f84,            // P3: the sun in model space, x 255
    L_MCAM = 0x00525f90,            // P3: the camera in model space
    L_MCAMDIR = 0x00525f9c,         // P3: its forward axis in model space
    L_HALF = 0x00525fa8,            // P3: the specular half vector
    L_ALPHA = 0x00525fb4,           // float, and u8 at 0x525fb8, dword << 24 at 0x525fbc
    L_ALPHA_B = 0x00525fb8, L_ALPHA_OR = 0x00525fbc,
    L_SPEC = 0x00525fc0, L_FOG = 0x00525fc1,       // u8 switches
    L_FOG_START = 0x00525fc4, L_FOG_END = 0x00525fc8, L_FOG_K = 0x00525fcc, L_FOG_SCALE = 0x00525fd0,
    L_LIGHT_R2 = 0x00525fd4,        // a light is placed within this squared distance of the camera
    L_LIGHT_D = 0x00525fd8,         // and reaches this squared distance
    L_STATE = 0x00525fdc,           // int[8]: frames left
    L_ACTIVE = 0x00525ffc,          // u8[8]
    L_POS = 0x00526004, L_DIR = 0x00526064,        // P3[8] each
    L_MPOS = 0x005260c4, L_MDIR = 0x00526124,      // P3[8] each, in model space
    L_ENV = 0x00526184,             // int: 0, 1 (copy UVs), 2 (sphere map)
    L_MMAT = 0x00526188,            // the model's rotation (9 floats)
    L_COUNT = 0x005261ac,           // vertices lit
    L_DIFF = 0x00525738,            // u32[0x200] grey diffuse table
    L_SPECT = 0x005261c8,           // u32[0x100] grey specular table
    L_FN_LIT = 0x004f38f4, L_FN_PRELIT = 0x004f38f8,   // the set_light_state picks
    L_PROF = 0x004e642c,            // i_pr_overhead_begin, +4 i_pr_overhead_end, +8 i_prof_start, +0xc i_prof_stop
};

// ---- the functions they call, by address ------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
typedef uint8_t(__cdecl* Byte_t)();
typedef void(__cdecl* IntV_t)(int);
typedef void(__cdecl* PtrV_t)(void*);
typedef uint8_t(__cdecl* IntB_t)(int);
typedef void(__cdecl* ByteV_t)(uint8_t);
typedef void(__cdecl* FloatBitsV_t)(uint32_t);
typedef void(__cdecl* Float3BitsV_t)(uint32_t, uint32_t, uint32_t);
// kernel, CRT
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
static const PtrV_t MemFree = (PtrV_t)0x00414300;
static const PtrV_t OpDelete = (PtrV_t)0x00414390;
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef void*(__fastcall* PoolCtor_t)(void*, Edx, const char*, int, unsigned);
static const PoolCtor_t PoolBase_ctor = (PoolCtor_t)0x0041ba30;
typedef void(__fastcall* ThisV_t)(void*, Edx);
static const ThisV_t PoolBase_dtor = (ThisV_t)0x0041bac0;
typedef void*(__fastcall* PoolAlloc_t)(void*, Edx);
typedef void(__fastcall* PoolFree_t)(void*, Edx, void*);
static const PoolAlloc_t PoolBase_alloc_v10 = (PoolAlloc_t)0x0041bb20;
static const PoolFree_t PoolBase_free_v10 = (PoolFree_t)0x0041bb60;
typedef char*(__cdecl* Strncpy_t)(char*, const char*, unsigned);
static const Strncpy_t game_strncpy = (Strncpy_t)0x004cf3a0;
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
static const Stricmp_t game_stricmp = (Stricmp_t)0x004da350;
typedef void*(__cdecl* ResGet_t)(const char*, uint32_t, uint32_t*, int*, uint8_t*, uint8_t*);
static const ResGet_t ResourceGet = (ResGet_t)0x00419fa0;
typedef uint8_t(__cdecl* ResForget_t)(void*);
static const ResForget_t ResourceForget = (ResForget_t)0x0041a450;
typedef void(__cdecl* ResWrite_t)(int, uint32_t, uint32_t);
static const ResWrite_t ResourceWrite = (ResWrite_t)0x0041a5c0;
typedef int(__cdecl* FileCreate_t)(const char*);
static const FileCreate_t FileCreate = (FileCreate_t)0x004115f0;
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
static const FileWrite_t FileWrite = (FileWrite_t)0x00411a30;
typedef void(__cdecl* FileClose_t)(int*);
static const FileClose_t FileClose = (FileClose_t)0x00411850;
// maths (physics library)
typedef void(__cdecl* MatConcat_t)(float*, const float*, const float*);
static const MatConcat_t MatrixConcat = (MatConcat_t)0x00403040;
typedef void(__cdecl* MulPointInv_t)(void*, const void*, const void*);
static const MulPointInv_t MatrixMulPointInv = (MulPointInv_t)0x00436120;
typedef void(__cdecl* VecSub_t)(void*, const void*, const void*);
static const VecSub_t VectorSub = (VecSub_t)0x00429120;
typedef double(__cdecl* VecLen_t)(const void*);    // fsqrt left in ST0
static const VecLen_t VectorLength = (VecLen_t)0x00429100;
static const PtrV_t VectorNormalize = (PtrV_t)0x0043e4a0;
// dx.obj, dxstate.obj, dd.obj (d3d), texture.obj, gx
static const Byte_t dxBegin = (Byte_t)0x00458240;
static const Void_t dxEnd = (Void_t)0x00458260;
typedef void(__cdecl* Counters_t)(int*, int*, int*, int*);
static const Counters_t dxGetCounters = (Counters_t)0x00458890;
typedef uint8_t(__cdecl* Begin3D_t)(void*);
static const Begin3D_t dxBegin3D = (Begin3D_t)0x00458270;
static const Void_t dxEnd3D = (Void_t)0x004582a0;
static const Void_t dxRelease = (Void_t)0x00458450;
static const Void_t dxRestore = (Void_t)0x00458460;
typedef uint8_t(__cdecl* View4_t)(int, int, int, int);
static const View4_t dxSetViewport = (View4_t)0x004585b0;
static const Float3BitsV_t dxSetViewportColor = (Float3BitsV_t)0x00458610;
typedef void(__cdecl* ClearZ_t)(int, int, int, int, uint8_t);
static const ClearZ_t dxClearZ = (ClearZ_t)0x00458770;
typedef uint8_t(__cdecl* Project_t)(const void*, void*, uint8_t);
static const Project_t dxProjectPoint = (Project_t)0x00458800;
static const Void_t dxDPBeginFrame = (Void_t)0x00458920;
static const Void_t dxDPEndFrame = (Void_t)0x00458930;
static const ByteV_t dxEnableVertexAlpha = (ByteV_t)0x00458970;
static const IntV_t dxDPBegin_v10 = (IntV_t)0x00458990;
typedef void(__cdecl* Draw4_t)(const void*, int, const uint16_t*, int);
static const Draw4_t dxDPDraw_v10 = (Draw4_t)0x00458a60;
static const Void_t dxDPEnd_v10 = (Void_t)0x00458a90;
static const IntV_t dxBeginTriangles = (IntV_t)0x00458aa0;
static const PtrV_t dxDrawTriangle = (PtrV_t)0x00458b70;
typedef void(__cdecl* Draw2_t)(const void*, int);
static const Draw2_t dxDrawTriangles = (Draw2_t)0x00458b90;
static const Draw4_t dxDrawIndexTriangles = (Draw4_t)0x00458bb0;
static const Draw4_t dxDrawScreenIndexTriangles = (Draw4_t)0x00458be0;
static const Void_t dxEndTriangles = (Void_t)0x00458c10;
static const Void_t dxStateInit = (Void_t)0x0045de20;
static const Void_t dxStateClear = (Void_t)0x0045def0;
static const PtrV_t dxStateUpdate = (PtrV_t)0x0045df00;
static const Void_t dxStateFlush_v10 = (Void_t)0x0045dfd0;
typedef void(__fastcall* SetTransform_t)(void*, Edx, int, void*);
static const SetTransform_t d3d_SetTransform = (SetTransform_t)0x0045f100;
typedef uint8_t(__cdecl* TexBegin_t)(int, int, int);
static const TexBegin_t TextureBegin = (TexBegin_t)0x00459f80;
static const Void_t TextureEnd = (Void_t)0x00459fa0;
static const ByteV_t TextureReleaseAll = (ByteV_t)0x0045a050;
static const ByteV_t TextureRestoreAll = (ByteV_t)0x0045a090;
static const Byte_t TextureWillDeresNextFrame = (Byte_t)0x0045a410;
static const Void_t TextureBeginFrame = (Void_t)0x0045a420;
static const Void_t TextureEndFrame = (Void_t)0x0045a4b0;
static const Void_t TextureFlush = (Void_t)0x0045a4c0;
static const IntB_t TextureHasAlpha_v10 = (IntB_t)0x0045a590;
typedef int(__cdecl* GetTex_t)(const char*, int);
static const GetTex_t gxGetTexture = (GetTex_t)0x0044e240;
static const IntV_t gxForgetTexture = (IntV_t)0x0044e280;

// this group's own functions, by address (so a hooked rewrite is what runs)
static const Void_t mr_connect_3d_o = (Void_t)0x0044f430;
static const Void_t mr_disconnect_3d_o = (Void_t)0x0044f460;
static const Void_t mr_release_3d_o = (Void_t)0x0044f470;
static const Void_t mr_restore_3d_o = (Void_t)0x0044f480;
static const Void_t init_render_state_o = (Void_t)0x0044f490;
static const Void_t clear_render_state_o = (Void_t)0x0044f510;
static const Void_t init_camera_o = (Void_t)0x0044f520;
static const Float3BitsV_t calc_projection_matrix_o = (Float3BitsV_t)0x0044f590;
typedef void(__cdecl* View5_t)(int, int, int, int, uint8_t);
static const View5_t mrSetView5_o = (View5_t)0x0044ecd0;
typedef void(__cdecl* View4v_t)(int, int, int, int);
static const View4v_t set_viewport_o = (View4v_t)0x0044eb00;
static const View4v_t fill_z_rect_o = (View4v_t)0x0044eb70;
static const Float3BitsV_t mrSetProjection_o = (Float3BitsV_t)0x0044ede0;
static const PtrV_t mrSetCamera_o = (PtrV_t)0x0044ee70;
typedef double(__cdecl* Dist_t)(const void*);         // fsqrt / the sum left in ST0
static const Dist_t mrCameraDistance_o = (Dist_t)0x0044f140;
typedef uint8_t(__cdecl* ProjModel_t)(const void*, void*, const void*, uint8_t);
static const ProjModel_t mrProjectModelPoint_o = (ProjModel_t)0x0044f260;
typedef uint8_t(__cdecl* CanSee_t)(const void*);
static const CanSee_t mrPointCanSee_o = (CanSee_t)0x0044f290;
typedef void(__cdecl* CamLook_t)(void*, void*);
static const CamLook_t mrCameraLook_o = (CamLook_t)0x0044efc0;
static const PtrV_t dxMatrixSetModel_o = (PtrV_t)0x0045dba0;
static const PtrV_t dxMatrixSetCamera_o = (PtrV_t)0x0045dbc0;
static const PtrV_t dxMatrixSetProjection_o = (PtrV_t)0x0045dbe0;
static const Void_t dxMatrixBegin_o = (Void_t)0x0045db40;
static const Void_t dxMatrixEnd_o = (Void_t)0x0045db70;
static const Void_t dxMatrixRelease_o = (Void_t)0x0045db80;
static const Void_t dxMatrixRestore_o = (Void_t)0x0045db90;
static const Void_t mrBeginFrame_o = (Void_t)0x0044e970;
static const Void_t mrEndFrame_o = (Void_t)0x0044e980;
// feature.obj
static const Void_t mr_feature_begin_o = (Void_t)0x00457860;
static const Void_t mr_feature_end_o = (Void_t)0x00457880;
static const IntV_t mrEnable_o = (IntV_t)0x00457890;
static const IntV_t mrDisable_o = (IntV_t)0x00457b00;
static const IntB_t mrIsEnabled_o = (IntB_t)0x00457d80;
static const IntV_t mrSetBlendMode_o = (IntV_t)0x00457e30;
// light.obj
static const Void_t mr_light_begin_o = (Void_t)0x0045add0;
static const Void_t mr_light_end_o = (Void_t)0x0045ade0;
static const Void_t mr_light_release_o = (Void_t)0x0045adf0;
static const Void_t mr_light_restore_o = (Void_t)0x0045ae00;
static const Void_t light_begin_o = (Void_t)0x0045ae10;
static const Void_t light_end_o = (Void_t)0x0045b140;
typedef void(__cdecl* PlaceCam_t)(const void*, const void*);
static const PlaceCam_t mrLightPlaceCamera_o = (PlaceCam_t)0x0045b150;
static const PtrV_t mrLightPlaceModel_o = (PtrV_t)0x0045b1b0;
typedef void(__cdecl* Verts_t)(const void*, void*);
static const Verts_t calc_day_o = (Verts_t)0x0045b460;
static const Verts_t calc_night_o = (Verts_t)0x0045b810;
static const Verts_t nocalc_light_o = (Verts_t)0x0045bc00;
static const Verts_t calc_night_2_o = (Verts_t)0x0045bdc0;
static const FloatBitsV_t mrLightSetAlpha_o = (FloatBitsV_t)0x0045bf00;
static const IntV_t mrLightEnv_o = (IntV_t)0x0045bf30;
static const ByteV_t mrLightCalc_o = (ByteV_t)0x0045bf40;
static const ByteV_t mrLightCalcFog_o = (ByteV_t)0x0045bf50;
static const ByteV_t mrLightCalcSpecular_o = (ByteV_t)0x0045bf60;
static const FloatBitsV_t make_diffuse_table_o = (FloatBitsV_t)0x0045c0b0;
static const FloatBitsV_t make_specular_table_o = (FloatBitsV_t)0x0045c100;
static const Void_t set_light_state_o = (Void_t)0x0045c160;
typedef void(__cdecl* LightFn_t)(void*, void*, void*);
static const LightFn_t LightSurfaceLit_o = (LightFn_t)0x0045c230;
static const LightFn_t LightSurfacePreLit_v10 = (LightFn_t)0x0045c250;
typedef double(__cdecl* Dot_t)(const void*, const void*);
static const Dot_t DotProduct_o = (Dot_t)0x0045d8e0;
typedef void(__cdecl* AddScaled_t)(void*, const void*, const void*, uint32_t);
static const AddScaled_t VectorAddScaled_o = (AddScaled_t)0x0045d900;
static const Verts_t APPLY_ENVMAP_o = (Verts_t)0x0045d940;
static const Verts_t APPLY_FOG_o = (Verts_t)0x0045da80;
// the functions set_light_state installs, by their ORIGINAL addresses (as the original does: a hooked rewrite runs)
enum : uint32_t {
    F_LIGHT_OFF = 0x0045c270, F_PRE_NOFOG_NOENV = 0x0045c2e0, F_PRE_NOFOG_SOMEENV = 0x0045c400,
    F_PRE_NOFOG_ENV = 0x0045c410, F_PRE_FOG_NOENV = 0x0045c420, F_PRE_FOG_SOMEENV = 0x0045c6f0,
    F_PRE_FOG_ENV = 0x0045c700, F_NOPRE_NOFOG_NOENV = 0x0045c710, F_NOPRE_NOFOG_SOMEENV = 0x0045c8c0,
    F_NOPRE_NOFOG_ENV = 0x0045cb40, F_NOPRE_FOG_NOENV = 0x0045cea0, F_NOPRE_FOG_SOMEENV = 0x0045d1e0,
    F_NOPRE_FOG_ENV = 0x0045d570,
};
// model.obj
static const Void_t mr_model_begin_o = (Void_t)0x004556f0;
static const Void_t mr_model_end_o = (Void_t)0x00455800;
static const Void_t mr_model_first_o = (Void_t)0x00455860;
static const Void_t mr_model_last_o = (Void_t)0x00455880;
static const Void_t mr_model_release_o = (Void_t)0x004558a0;
static const Void_t mr_model_restore_o = (Void_t)0x004558b0;
typedef int(__cdecl* ModelCreate_t)(const char*, int);
static const ModelCreate_t mrModelCreate_o = (ModelCreate_t)0x00455b40;
static const IntV_t mrModelDestroy_o = (IntV_t)0x00455bc0;
typedef void(__cdecl* ModelBuild_t)(int, void*, int);
static const ModelBuild_t mrModelBuild_o = (ModelBuild_t)0x00455c80;
typedef void(__cdecl* Remap_t)(int, void*, int);
static const Remap_t mrModelRemapTextures_o = (Remap_t)0x00455980;
typedef int(__cdecl* CopyXf_t)(int, const void*);
static const CopyXf_t mrModelCopyTransform_o = (CopyXf_t)0x004559c0;
static const IntV_t mrModelDraw1_o = (IntV_t)0x00455ec0;
typedef void(__cdecl* DirectBuild_t)(int, void*, uint8_t, int);
static const DirectBuild_t direct_model_build_o = (DirectBuild_t)0x00455ed0;
typedef void(__cdecl* SetTexId_t)(int, const char*, void*);
static const SetTexId_t set_texture_id_o = (SetTexId_t)0x004560d0;
static const IntV_t direct_model_draw_o = (IntV_t)0x00456190;
static const PtrV_t mrModelSetFrame_o = (PtrV_t)0x004562d0;
typedef void*(__cdecl* InfoGet_t)(const char*);
static const InfoGet_t mrModelInfoGet_o = (InfoGet_t)0x004562f0;
static const PtrV_t mrModelInfoForget_o = (PtrV_t)0x00456460;
typedef int(__cdecl* Pick_t)(void*, const void*, int, int);
static const Pick_t mrModelPick_o = (Pick_t)0x00456560;
typedef double(__cdecl* LineDist_t)(int, int, const void*, const void*);
static const LineDist_t line_point_dist_o = (LineDist_t)0x00456d30;
static const PtrV_t mr_model_set_frame_o = (PtrV_t)0x00456e40;
static const FloatBitsV_t mrModelSetAlpha_o = (FloatBitsV_t)0x00457090;
static const Void_t mrModelClearAlpha_o = (Void_t)0x004570b0;
typedef void*(__cdecl* ToMrModel_t)(void*);
typedef uint8_t*(__cdecl* ToMip_t)(int);
static const ToMrModel_t convert_to_mrmodel_o = (ToMrModel_t)0x004571e0;
static const ToMip_t convert_to_mip_o = (ToMip_t)0x004571f0;
static const Void_t begin_deferred_o = (Void_t)0x00457200;
static const Void_t end_deferred_o = (Void_t)0x00457260;
static const Void_t begin_deferred_surfs_o = (Void_t)0x00457280;
static const Void_t end_deferred_surfs_o = (Void_t)0x00457290;
static const Void_t draw_alpha_deferred_surfs_o = (Void_t)0x00457340;
typedef void(__cdecl* AddDeferred_t)(void*, void*, void*);
static const AddDeferred_t add_deferred_surf_o = (AddDeferred_t)0x004573d0;
typedef int(__cdecl* CopyTransform_t)(int, const void*, void**);
static const CopyTransform_t model_copy_transform_o = (CopyTransform_t)0x00457410;
typedef void(__cdecl* Assert_t)(int, const char*, ...);
static const Assert_t model_ASSERT_MSG = (Assert_t)0x00455eb0;
// a model_info from a handle, and back: convert_to_mip / convert_to_mrmodel, by address like every call
static __forceinline uint8_t* mip(int m) { return convert_to_mip_o(m); }

// ---- footprint helpers ---------------------------------------------------------------------------------------------
// the dx layer's statics a state, draw, transform or viewport call may write: dx_result (every d3d method stores its
// HRESULT), dxstate's current-state pointer, cache and force flag, dx.obj's vertex-alpha byte, texture cache and
// dirty flag
static void fp_dx(Footprint& f) {
    f.add(P<uint8_t>(0x00522e6c), 4, "dx_result");
    f.add(P<uint8_t>(0x005265f4), 4, "dxstate: the current state");
    f.add(P<uint8_t>(0x00526608), 0x19, "dxstate: the flushed cache and the force flag");
    f.add(P<uint8_t>(0x004f2324), 0xc, "dx: vertex alpha, the texture cache and dirty flag");
}
static void fp_dxstate(Footprint& f) {
    f.add(P<uint8_t>(S_FEAT2), 0x1c, "mrFeature 2 / 4 bytes and the dxState block");
    fp_dx(f);
}
static void fp_project(Footprint& f, void* out) {           // dxProjectPoint: its D3DTRANSFORMDATA buffers, the output
    fp_dx(f);
    f.add(P<uint8_t>(0x00522f0c), 0x50, "dxProjectPoint's transform buffers");
    if (out) f.add(out, 12, "the projected point");
}
static void fp_light_state(Footprint& f) {                  // set_light_state and the switches that call it
    f.add(P<uint8_t>(L_FN_LIT), 8, "LightSurfaceLit / PreLit's functions");
}
static void fp_feature(Footprint& f, int32_t idx) {         // mrEnable / mrDisable / mrSetBlendMode at stack entry idx
    f.add(P<uint8_t>(S_FSTACK + 8u * (uint32_t)idx), 8, "the feature stack's entry");
    fp_dxstate(f);
    f.add(P<uint8_t>(L_ON), 1, "light: calculating");
    f.add(P<uint8_t>(L_SPEC), 2, "light: specular and fog switches");
    fp_light_state(f);
}
static void fp_light_all(Footprint& f) {                    // light.obj's statics, tables to the vertex count
    f.add(P<uint8_t>(L_DIFF), 0x800, "light: the diffuse table");
    f.add(P<uint8_t>(L_ON), 0x260, "light: its statics");
    f.add(P<uint8_t>(L_SPECT), 0x400, "light: the specular table");
    fp_light_state(f);
}
static void fp_pool(Footprint& f, uint32_t pool_ptr_addr) { // a PoolBase's header and storage (alloc / free)
    uint8_t* pool = *P<uint8_t*>(pool_ptr_addr);
    if (!pool) return;
    f.add(pool, 0x20, "the pool");
    uint8_t* store = Pp(pool, 0xc);
    const uint32_t n = (uint32_t)Ip(pool, 0x14) * (uint32_t)Ip(pool, 0x18);
    if (store && n && n < 0x4000000) f.add(store, n, "the pool's storage");
}

// ================================================================================================================
// mr.obj
// ================================================================================================================

// ==== mrBegin (0x44e7e0) / mrEnd (0x44e800) ======================================================================
static void __cdecl mrBegin() {
    dxBegin();
    Ba(S_MR_OK) = 0;
    Ba(S_MR_ON) = 1;
}
static void fp_mrBegin(Footprint& f) { f.replay_only = "dxBegin creates the d3d object (MemAlloc)"; }
PORT_FN(0x0044e7e0, "mrBegin", mrBegin, fp_mrBegin)

static void __cdecl mrEnd() {
    dxEnd();
    Ba(S_MR_OK) = 0;
    Ba(S_MR_ON) = 0;
}
static void fp_mrEnd(Footprint& f) { f.replay_only = "dxEnd frees the d3d object"; }
PORT_FN(0x0044e800, "mrEnd", mrEnd, fp_mrEnd)

// ==== mrGetStats (0x44e820) ======================================================================================
static void __cdecl mrGetStats(int* a, int* b, int* c, int* d) { dxGetCounters(a, b, c, d); }
static void fp_mrGetStats(Footprint& f, int* a, int* b, int* c, int* d) {
    fp_dx(f);
    f.add(a, 4, "out"); f.add(b, 4, "out"); f.add(c, 4, "out"); f.add(d, 4, "out");
}
PORT_FN(0x0044e820, "mrGetStats", mrGetStats, fp_mrGetStats)

// ==== mr_ok (0x44e840) ===========================================================================================
static uint8_t __cdecl mr_ok() { return (uint8_t)(Ba(S_MR_OK) - 1) < 1 ? 1 : 0; }   // dec al; cmp al, 1; sbb; neg
static void fp_mr_ok(Footprint&) {}
PORT_FN(0x0044e840, "mr_ok", mr_ok, fp_mr_ok)

// ==== mr_set_mode (0x44e850) =====================================================================================
static void __cdecl mr_set_mode() {
    if (!Ba(S_MR_ON)) return;
    mr_connect_3d_o();
    dxMatrixBegin_o();
    mr_light_begin_o();
    mr_model_begin_o();
    mr_feature_begin_o();
    init_camera_o();
    init_render_state_o();
    const int hit = Ia(S_SCREEN_HIT), wid = Ia(S_SCREEN_WID);
    Ba(S_MR_OK) = 1;
    mrSetView5_o(0, 0, wid, hit, 0);
    mrSetProjection_o(0x3f800000, 0x447a0000, 0x3f9c61aa);      // 1, 1000, 70 degrees
}
static void fp_mr_set_mode(Footprint& f) { f.replay_only = "sets the mode up: the device, textures, pools"; }
PORT_FN(0x0044e850, "mr_set_mode", mr_set_mode, fp_mr_set_mode)

// ==== mr_restore_mode (0x44e8c0) =================================================================================
static void __cdecl mr_restore_mode() {
    if (!Ba(S_MR_ON)) return;
    clear_render_state_o();
    mr_feature_end_o();
    mr_model_end_o();
    mr_light_end_o();
    dxMatrixEnd_o();
    mr_disconnect_3d_o();
}
static void fp_mr_restore_mode(Footprint& f) { f.replay_only = "tears the mode down: frees the pools, the device"; }
PORT_FN(0x0044e8c0, "mr_restore_mode", mr_restore_mode, fp_mr_restore_mode)

// ==== mr_release (0x44e8f0) / mr_restore (0x44e910) ==============================================================
static void __cdecl mr_release() {
    if (!Ba(S_MR_ON)) return;
    mr_model_release_o();
    mr_light_release_o();
    dxMatrixRelease_o();
    mr_release_3d_o();
}
static void fp_mr_release(Footprint& f) { f.replay_only = "releases the device and every texture"; }
PORT_FN(0x0044e8f0, "mr_release", mr_release, fp_mr_release)

static void __cdecl mr_restore() {
    if (!Ba(S_MR_ON)) return;
    mr_restore_3d_o();
    dxMatrixRestore_o();
    mr_light_restore_o();
    mr_model_restore_o();
    init_camera_o();
    init_render_state_o();
    mrSetView5_o(0, 0, Ia(S_SCREEN_WID), Ia(S_SCREEN_HIT), 0);
    mrSetProjection_o(0x3f800000, 0x447a0000, 0x3f9c61aa);
}
static void fp_mr_restore(Footprint& f) { f.replay_only = "restores the device and reloads every texture"; }
PORT_FN(0x0044e910, "mr_restore", mr_restore, fp_mr_restore)

// ==== mrBeginFrame (0x44e970) / mrEndFrame (0x44e980) ============================================================
static void __cdecl mrBeginFrame() {
    dxDPBeginFrame();
    TextureBeginFrame();
}
static void fp_mrBeginFrame(Footprint& f) { f.replay_only = "TextureBeginFrame can load textures"; }
PORT_FN(0x0044e970, "mrBeginFrame", mrBeginFrame, fp_mrBeginFrame)

static void __cdecl mrEndFrame() {
    TextureEndFrame();
    dxDPEndFrame();
}
static void fp_mrEndFrame(Footprint& f) { fp_dxstate(f); }
PORT_FN(0x0044e980, "mrEndFrame", mrEndFrame, fp_mrEndFrame)

// ==== mrBeginAlphaRects (0x44e990) / mrEndAlphaRects (0x44e9c0) ==================================================
static void __cdecl mrBeginAlphaRects() {
    mrDisable_o(0x80);
    mrDisable_o(0x100);
    dxEnableVertexAlpha(1);
    dxBeginTriangles(-1);
}
static void fp_mrBeginAlphaRects(Footprint& f) { f.replay_only = "dxBeginTriangles -> TextureSelect can load a texture"; }
PORT_FN(0x0044e990, "mrBeginAlphaRects", mrBeginAlphaRects, fp_mrBeginAlphaRects)

static void __cdecl mrEndAlphaRects() {
    dxEndTriangles();
    dxEnableVertexAlpha(0);
    mrEnable_o(0x100);
    mrEnable_o(0x80);
}
static void fp_mrEndAlphaRects(Footprint& f) { fp_feature(f, Ia(S_FSTACK_IDX)); }
PORT_FN(0x0044e9c0, "mrEndAlphaRects", mrEndAlphaRects, fp_mrEndAlphaRects)

// ==== mrDrawAlphaRect (0x44e9f0) =================================================================================
// four D3DTLVERTEX (x, y, 1.0, rhw 0, colour, 0, 0, 0) at the corners, drawn as {0,1,3, 0,3,2}
static void __cdecl mrDrawAlphaRect(int x1, int y1, int x2, int y2, uint32_t color) {
    uint32_t v[32];
    uint16_t idx[6] = {0, 1, 3, 0, 3, 2};
    const float fx1 = (float)x1, fy1 = (float)y1, fx2 = (float)x2, fy2 = (float)y2;   // fild; fst dword
    const float xs[4] = {fx1, fx2, fx1, fx2}, ys[4] = {fy1, fy1, fy2, fy2};
    for (int i = 0; i < 4; i++) {
        memcpy(&v[8 * i + 0], &xs[i], 4);
        memcpy(&v[8 * i + 1], &ys[i], 4);
        v[8 * i + 2] = 0x3f800000;
        v[8 * i + 3] = 0;
        v[8 * i + 4] = color;
        v[8 * i + 5] = 0;
        v[8 * i + 6] = 0;
        v[8 * i + 7] = 0;
    }
    dxDrawScreenIndexTriangles(v, 4, idx, 6);
}
static void fp_mrDrawAlphaRect(Footprint& f, int, int, int, int, uint32_t) { fp_dx(f); }
PORT_FN(0x0044e9f0, "mrDrawAlphaRect", mrDrawAlphaRect, fp_mrDrawAlphaRect)

// ==== set_viewport (0x44eb00) ====================================================================================
static void __cdecl set_viewport(int x, int y, int w, int h) {
    int32_t* v = P<int32_t>(S_VIEW);
    v[0] = x;
    v[1] = y;
    v[4] = w;
    v[5] = h;
    v[6] = w / 2;                                   // cdq; sub eax, edx; sar eax, 1
    v[7] = h / 2;
    v[2] = (int32_t)((uint32_t)(w / 2) + (uint32_t)x);
    v[3] = (int32_t)((uint32_t)(h / 2) + (uint32_t)y);
    dxSetViewport(x, y, w, h);
}
static void fp_view(Footprint& f) { f.add(P<uint8_t>(S_VIEW), 0x20, "the view rectangle"); }
static void fp_set_viewport(Footprint& f, int, int, int, int) { fp_view(f); fp_dx(f); }
PORT_FN(0x0044eb00, "set_viewport", set_viewport, fp_set_viewport)

// ==== fill_z_rect (0x44eb70) =====================================================================================
// (no callers but mrSetView(x, y, w, h)) the rectangle as four TL vertices at z 1.0 with rhw = the far distance's
// bits and the background colour, drawn with the Z test off
static void __cdecl fill_z_rect(int x, int y, int w, int h) {
    uint32_t v[32];
    uint16_t idx[6] = {0, 1, 3, 0, 3, 2};
    const float fx = (float)x, fy = (float)y;
    const float fx2 = (float)(int32_t)((uint32_t)x + (uint32_t)w), fy2 = (float)(int32_t)((uint32_t)h + (uint32_t)y);
    const float xs[4] = {fx, fx2, fx, fx2}, ys[4] = {fy, fy, fy2, fy2};
    const uint32_t far_bits = Ua(S_FAR), bg = Ua(S_BGCOLOR);
    for (int i = 0; i < 4; i++) {
        memcpy(&v[8 * i + 0], &xs[i], 4);
        memcpy(&v[8 * i + 1], &ys[i], 4);
        v[8 * i + 2] = 0x3f800000;
        v[8 * i + 3] = far_bits;
        v[8 * i + 4] = bg;
        v[8 * i + 5] = 0;
        v[8 * i + 6] = 0;
        v[8 * i + 7] = 0;
    }
    mrDisable_o(0x80);
    dxBeginTriangles(-1);
    dxDrawScreenIndexTriangles(v, 4, idx, 6);
    dxEndTriangles();
    mrEnable_o(0x80);
}
static void fp_fill_z_rect(Footprint& f, int, int, int, int) { f.replay_only = "dxBeginTriangles -> TextureSelect can load a texture"; }
PORT_FN(0x0044eb70, "fill_z_rect", fill_z_rect, fp_fill_z_rect)

// ==== mrSetView(x, y, w, h, clear) (0x44ecd0) ====================================================================
static void __cdecl mrSetView5(int x, int y, int w, int h, uint8_t clear) {
    set_viewport_o(x, y, w, h);
    const int32_t* v = P<int32_t>(S_VIEW);
    const int32_t y2 = (int32_t)((uint32_t)v[5] + (uint32_t)v[1]);
    const int32_t y1 = v[1], x1 = v[0];
    const int32_t x2 = (int32_t)((uint32_t)v[4] + (uint32_t)v[0]);
    dxClearZ(x1, y1, x2, y2, clear);
}
static void fp_mrSetView5(Footprint& f, int, int, int, int, uint8_t) { fp_view(f); fp_dx(f); }
PORT_FN(0x0044ecd0, "mrSetView(x,y,w,h,clear)", mrSetView5, fp_mrSetView5)

// ==== mrSetView(x, y, w, h) (0x44ed20) ===========================================================================
static void __cdecl mrSetView4(int x, int y, int w, int h) {
    fill_z_rect_o(x, y, w, h);
    set_viewport_o(x, y, w, h);
}
static void fp_mrSetView4(Footprint& f, int, int, int, int) { f.replay_only = "fill_z_rect: dxBeginTriangles -> TextureSelect"; }
PORT_FN(0x0044ed20, "mrSetView(x,y,w,h)", mrSetView4, fp_mrSetView4)

// ==== mrSetViewBackground (0x44ed60) =============================================================================
// the clear colour 0xffRRGGBB from r, g, b in 0..1 (x 255 + 0.5, __ftol), and the viewport material's
static void __cdecl mrSetViewBackground(float r, float g, float b) {
    uint32_t c = (uint8_t)x87_ftol(D(g) * 255.0 + 0.5);
    c = (c | 0xffff0000u) << 8;
    c |= (uint32_t)(uint8_t)x87_ftol(D(r) * 255.0 + 0.5) << 16;
    c |= (uint8_t)x87_ftol(D(b) * 255.0 + 0.5);
    Ua(S_BGCOLOR) = c;
    dxSetViewportColor(fbits(r), fbits(g), fbits(b));
}
static void fp_mrSetViewBackground(Footprint& f, float, float, float) {
    f.add(P<uint8_t>(S_BGCOLOR), 4, "the background colour");
    fp_dx(f);
}
PORT_FN(0x0044ed60, "mrSetViewBackground", mrSetViewBackground, fp_mrSetViewBackground)

// ==== mrSetProjection (0x44ede0) =================================================================================
static void __cdecl mrSetProjection(uint32_t n, uint32_t f, uint32_t fov) {      // floats, as bits
    Ua(S_FOV) = fov;
    Ua(S_NEAR) = n;
    Ua(S_FAR) = f;
    calc_projection_matrix_o(n, f, fov);
    dxMatrixSetProjection_o(P<void>(S_PROJ));
}
static void fp_proj(Footprint& f) { f.add(P<uint8_t>(S_PROJ), 0x40, "the projection matrix"); }
static void fp_mrSetProjection(Footprint& f, uint32_t, uint32_t, uint32_t) {
    f.add(P<uint8_t>(S_NEAR), 0x10, "near, far, fov");
    fp_proj(f);
    fp_dx(f);
}
PORT_FN(0x0044ede0, "mrSetProjection", mrSetProjection, fp_mrSetProjection)

// ==== mrMirrorView (0x44ee20) ====================================================================================
static __forceinline void fpu_abs_neg(float* p, uint8_t negate) {   // fld; fabs; [fchs]; fstp -- through the FPU
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[p]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "fabs\n\t"
                     "cmp %[negate], 0\n\t"
                     "je keep%=\n\t"
                     "fchs\n"
                     "keep%=:\n\t"
                     "fstp dword ptr [eax]"
                     :
                     : [p] "m"(p), [negate] "m"(negate)
                     : VP_X87_CLOBBERS, "eax", "cc", "memory");
#else
    __asm { mov eax, p
            fld dword ptr [eax]
            fabs
            cmp negate, 0
            je keep
            fchs
    keep:   fstp dword ptr [eax] }
#endif
}
static void __cdecl mrMirrorView(uint8_t on) {
    fpu_abs_neg(P<float>(S_PROJ), on);
    dxMatrixSetProjection_o(P<void>(S_PROJ));
    if (on) mrEnable_o(0x200);
    else mrDisable_o(0x200);
}
static void fp_mrMirrorView(Footprint& f, uint8_t) { fp_proj(f); fp_feature(f, Ia(S_FSTACK_IDX)); }
PORT_FN(0x0044ee20, "mrMirrorView", mrMirrorView, fp_mrMirrorView)

// ==== mrSetCamera (0x44ee70) =====================================================================================
// the camera's position and forward axis (to the lighting too), and the view matrix: the frame's rotation
// transposed, translation -(R p)
static void __cdecl mrSetCamera(const uint8_t* fr) {
    cp12(P<uint8_t>(S_CAMPOS), fr + 0x24);
    cp12(P<uint8_t>(S_CAMDIR), fr + 0x18);
    mrLightPlaceCamera_o(P<void>(S_CAMPOS), P<void>(S_CAMDIR));
    uint8_t* m = P<uint8_t>(S_CAMMAT);
    cp4(m + 0x00, fr + 0x00); cp4(m + 0x04, fr + 0x0c); cp4(m + 0x08, fr + 0x18); Up(m, 0x0c) = 0;
    cp4(m + 0x10, fr + 0x04); cp4(m + 0x14, fr + 0x10); cp4(m + 0x18, fr + 0x1c); Up(m, 0x1c) = 0;
    cp4(m + 0x20, fr + 0x08); cp4(m + 0x24, fr + 0x14); cp4(m + 0x28, fr + 0x20); Up(m, 0x2c) = 0;
    const float px = Fp(fr, 0x24), py = Fp(fr, 0x28), pz = Fp(fr, 0x2c);
    const float t0 = (float)((D(Fp(fr, 0x00)) * px + D(Fp(fr, 0x08)) * pz) + D(Fp(fr, 0x04)) * py);
    const float t1 = (float)((D(Fp(fr, 0x14)) * pz + D(px) * Fp(fr, 0x0c)) + D(py) * Fp(fr, 0x10));
    const float t2 = (float)((D(py) * Fp(fr, 0x1c) + D(px) * Fp(fr, 0x18)) + D(pz) * Fp(fr, 0x20));
    Fp(m, 0x30) = -t0;
    Up(m, 0x3c) = 0x3f800000;
    Fp(m, 0x34) = -t1;
    Fp(m, 0x38) = -t2;
    dxMatrixSetCamera_o(m);
}
static void fp_camera(Footprint& f) {
    f.add(P<uint8_t>(S_CAMMAT), 0x40, "the view matrix");
    f.add(P<uint8_t>(S_CAMDIR), 0x1c, "the camera's axis and position");
    f.add(P<uint8_t>(L_CAMPOS), 0x18, "light: the camera");
    f.add(P<uint8_t>(L_STATE), 0x28, "light: the lights' frames left and active flags");
    fp_dx(f);
}
static void fp_mrSetCamera(Footprint& f, const uint8_t*) { fp_camera(f); }
PORT_FN(0x0044ee70, "mrSetCamera", mrSetCamera, fp_mrSetCamera)

// ==== mrGetCameraPos (0x44efb0) ==================================================================================
static void* __cdecl mrGetCameraPos() { return P<void>(S_CAMPOS); }
static void fp_mrGetCameraPos(Footprint& f) { f.pure = true; }
PORT_FN(0x0044efb0, "mrGetCameraPos", mrGetCameraPos, fp_mrGetCameraPos)

// ==== mrCameraLook (0x44efc0) ====================================================================================
// a camera frame at eye looking at `at`, with no roll: forward = unit(at - eye), right = unit(-fz, 0, fx) (as m0..m2,
// m1 forced 0), up = forward x right
static void __cdecl mrCameraLook(const float* eye, const float* at) {
    float fr[12];
    cp12(&fr[9], eye);
    fr[6] = (float)(D(at[0]) - eye[0]);
    fr[7] = (float)(D(at[1]) - eye[1]);
    fr[8] = (float)(D(at[2]) - eye[2]);
    const double inv = 1.0 / x87_sqrt((D(fr[7]) * fr[7] + D(fr[6]) * fr[6]) + D(fr[8]) * fr[8]);
    fr[6] = (float)(D(fr[6]) * inv);
    fr[7] = (float)(D(fr[7]) * inv);
    fr[8] = (float)(inv * fr[8]);
    fr[1] = 0.0f;                                   // mov dword [m1], 0
    fr[2] = st(-D(fr[6]));                          // fld; fchs; fstp
    const double inv2 = 1.0 / x87_sqrt((D(fr[1]) * fr[1] + D(fr[2]) * fr[2]) + D(fr[8]) * fr[8]);
    fr[0] = (float)(D(fr[8]) * inv2);
    fr[1] = (float)(D(fr[1]) * inv2);
    const double r = inv2 * fr[2];                  // fmul [m2]; fst [m2] -- and on with the register
    fr[2] = st(r);
    fr[3] = (float)(r * fr[7] - D(fr[1]) * fr[8]);
    fr[4] = (float)(D(fr[8]) * fr[0] - D(fr[6]) * fr[2]);
    fr[5] = (float)(D(fr[1]) * fr[6] - D(fr[7]) * fr[0]);
    mrSetCamera_o(fr);
}
static void fp_mrCameraLook(Footprint& f, const float*, const float*) { fp_camera(f); }
PORT_FN(0x0044efc0, "mrCameraLook", mrCameraLook, fp_mrCameraLook)

// ==== mrCameraDistance (0x44f140) / mrCameraDistanceSqr (0x44f190) ===============================================
static __forceinline double camera_dist2(const float* p) {
    const float dx = (float)(D(p[0]) - Fa(S_CAMPOS));
    const float dy = (float)(D(p[1]) - Fa(S_CAMPOS + 4));
    const float dz = (float)(D(p[2]) - Fa(S_CAMPOS + 8));
    return (D(dy) * dy + D(dz) * dz) + D(dx) * dx;
}
static double __cdecl mrCameraDistance(const float* p) { return x87_sqrt(camera_dist2(p)); }
static void fp_mrCameraDistance(Footprint&, const float*) {}
PORT_FN(0x0044f140, "mrCameraDistance", mrCameraDistance, fp_mrCameraDistance)

static double __cdecl mrCameraDistanceSqr(const float* p) { return camera_dist2(p); }
static void fp_mrCameraDistanceSqr(Footprint&, const float*) {}
PORT_FN(0x0044f190, "mrCameraDistanceSqr", mrCameraDistanceSqr, fp_mrCameraDistanceSqr)

// ==== mrGetCamera (0x44f1e0) =====================================================================================
static void __cdecl mrGetCamera(float* out) { cp12(out, P<uint8_t>(S_CAMMAT + 0x30)); }
static void fp_mrGetCamera(Footprint& f, float* out) { f.add(out, 12, "out"); }
PORT_FN(0x0044f1e0, "mrGetCamera", mrGetCamera, fp_mrGetCamera)

// ==== mrProjectPoint (0x44f200) / mrProjectModelPoint (0x44f260) =================================================
static const uint32_t k_identity_frame[12] = {0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0};
static uint8_t __cdecl mrProjectPoint(const float* in, float* out) {
    uint32_t fr[12];
    memcpy(fr, k_identity_frame, sizeof fr);
    mr_model_set_frame_o(fr);
    return dxProjectPoint(in, out, 1);
}
static void fp_mrProjectPoint(Footprint& f, const float*, float* out) { fp_project(f, out); }
PORT_FN(0x0044f200, "mrProjectPoint", mrProjectPoint, fp_mrProjectPoint)

static uint8_t __cdecl mrProjectModelPoint(const float* in, float* out, const void* fr, uint8_t clip) {
    mr_model_set_frame_o((void*)fr);
    return dxProjectPoint(in, out, clip);
}
static void fp_mrProjectModelPoint(Footprint& f, const float*, float* out, const void*, uint8_t) { fp_project(f, out); }
PORT_FN(0x0044f260, "mrProjectModelPoint", mrProjectModelPoint, fp_mrProjectModelPoint)

// ==== mrPointCanSee (0x44f290) / mrSphereCanSee (0x44f2e0) =======================================================
static __forceinline double camera_ahead(const float* p) {       // (p - camera) . forward
    return ((D(p[2]) - Fa(S_CAMPOS + 8)) * Fa(S_CAMDIR + 8) + (D(p[1]) - Fa(S_CAMPOS + 4)) * Fa(S_CAMDIR + 4)) +
           (D(p[0]) - Fa(S_CAMPOS)) * Fa(S_CAMDIR);
}
static uint8_t __cdecl mrPointCanSee(const float* p) {
    return camera_ahead(p) >= D(FB(0xbdcccccd)) ? 1 : 0;       // -0.1f; test ah, 1: less or unordered -> 0
}
static void fp_mrPointCanSee(Footprint&, const float*) {}
PORT_FN(0x0044f290, "mrPointCanSee", mrPointCanSee, fp_mrPointCanSee)

static uint8_t __cdecl mrSphereCanSee(const float* p, float r) {
    const float d = (float)camera_ahead(p);
    if (-D(r) > D(d)) return 0;
    return D(Fa(S_FAR)) + r > D(d) ? 1 : 0;
}
static void fp_mrSphereCanSee(Footprint&, const float*, float) {}
PORT_FN(0x0044f2e0, "mrSphereCanSee", mrSphereCanSee, fp_mrSphereCanSee)

// ==== mrBeginTriangles (0x44f360) / mrBeginTrianglesModelSpace (0x44f3c0) ========================================
static void __cdecl mrBeginTriangles(int tex) {
    uint32_t fr[12];
    memcpy(fr, k_identity_frame, sizeof fr);
    mr_model_set_frame_o(fr);
    dxBeginTriangles(tex);
}
static void fp_mrBeginTriangles(Footprint& f, int) { f.replay_only = "dxBeginTriangles -> TextureSelect can load a texture"; }
PORT_FN(0x0044f360, "mrBeginTriangles", mrBeginTriangles, fp_mrBeginTriangles)

static void __cdecl mrBeginTrianglesModelSpace(int tex) { dxBeginTriangles(tex); }
static void fp_mrBeginTrianglesModelSpace(Footprint& f, int) { f.replay_only = "dxBeginTriangles -> TextureSelect can load a texture"; }
PORT_FN(0x0044f3c0, "mrBeginTrianglesModelSpace", mrBeginTrianglesModelSpace, fp_mrBeginTrianglesModelSpace)

// ==== mrDrawTriangle(s) (0x44f3d0, 0x44f3e0), mrDrawIndexTriangles (0x44f400), mrEndTriangles (0x44f420) ==========
static void __cdecl mrDrawTriangle(const void* v) { dxDrawTriangle((void*)v); }
static void fp_mrDrawTriangle(Footprint& f, const void*) { fp_dx(f); }
PORT_FN(0x0044f3d0, "mrDrawTriangle", mrDrawTriangle, fp_mrDrawTriangle)

static void __cdecl mrDrawTriangles(const void* v, int n) { dxDrawTriangles(v, n); }
static void fp_mrDrawTriangles(Footprint& f, const void*, int) { fp_dx(f); }
PORT_FN(0x0044f3e0, "mrDrawTriangles", mrDrawTriangles, fp_mrDrawTriangles)

static void __cdecl mrDrawIndexTriangles(const void* v, int n, const uint16_t* idx, int ni) { dxDrawIndexTriangles(v, n, idx, ni); }
static void fp_mrDrawIndexTriangles(Footprint& f, const void*, int, const uint16_t*, int) { fp_dx(f); }
PORT_FN(0x0044f400, "mrDrawIndexTriangles", mrDrawIndexTriangles, fp_mrDrawIndexTriangles)

static void __cdecl mrEndTriangles() { dxEndTriangles(); }
static void fp_mrEndTriangles(Footprint&) {}
PORT_FN(0x0044f420, "mrEndTriangles", mrEndTriangles, fp_mrEndTriangles)

// ==== mr_connect_3d (0x44f430) / mr_disconnect_3d (0x44f460) / mr_release_3d (0x44f470) / mr_restore_3d (0x44f480)
static void __cdecl mr_connect_3d() {
    if (!dxBegin3D(P<void>(S_MRCAPS))) return;
    if (!TextureBegin(0x80, 1, 2000000)) dxEnd3D();
}
static void fp_mr_connect_3d(Footprint& f) { f.replay_only = "creates the device and the texture pools"; }
PORT_FN(0x0044f430, "mr_connect_3d", mr_connect_3d, fp_mr_connect_3d)

static void __cdecl mr_disconnect_3d() {
    TextureEnd();
    dxEnd3D();
}
static void fp_mr_disconnect_3d(Footprint& f) { f.replay_only = "frees the texture pools and the device"; }
PORT_FN(0x0044f460, "mr_disconnect_3d", mr_disconnect_3d, fp_mr_disconnect_3d)

static void __cdecl mr_release_3d() {
    dxRelease();
    TextureReleaseAll(1);
}
static void fp_mr_release_3d(Footprint& f) { f.replay_only = "frees the viewport, material and textures"; }
PORT_FN(0x0044f470, "mr_release_3d", mr_release_3d, fp_mr_release_3d)

static void __cdecl mr_restore_3d() {
    dxRestore();
    TextureRestoreAll(1);
}
static void fp_mr_restore_3d(Footprint& f) { f.replay_only = "recreates the viewport, material and textures"; }
PORT_FN(0x0044f480, "mr_restore_3d", mr_restore_3d, fp_mr_restore_3d)

// ==== init_render_state (0x44f490) / clear_render_state (0x44f510) ===============================================
static void __cdecl init_render_state() {
    Ba(S_FEAT2) = 1;
    uint8_t* s = P<uint8_t>(S_DXSTATE);
    s[0x00] = 1;  s[0x01] = 0;                      // lighting, fog
    s[0x08] = 1;  s[0x09] = 1;  s[0x0a] = 1;  s[0x0b] = 1;   // Z, Z writes, perspective, filtering
    s[0x0c] = 0;  s[0x0d] = 0;  s[0x0e] = 0;        // specular, antialias, alpha
    Up(s, 0x10) = 0;                                // blend mode
    s[0x14] = 0;  s[0x15] = 0;  s[0x16] = 1;        // mirror cull, mipmap, wrap U
    Up(s, 0x04) = 0x8c8c8c;                         // fog colour
    s[0x17] = 1;                                    // wrap V
    dxStateInit();
    dxStateUpdate(s);
}
static void fp_init_render_state(Footprint& f) { fp_dxstate(f); }
PORT_FN(0x0044f490, "init_render_state", init_render_state, fp_init_render_state)

static void __cdecl clear_render_state() { dxStateClear(); }
static void fp_clear_render_state(Footprint&) {}
PORT_FN(0x0044f510, "clear_render_state", clear_render_state, fp_clear_render_state)

// ==== init_camera (0x44f520) =====================================================================================
static void __cdecl init_camera() {
    uint32_t* m = P<uint32_t>(S_CAMMAT);
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 0x3f800000u : 0u;   // identity
}
static void fp_init_camera(Footprint& f) { f.add(P<uint8_t>(S_CAMMAT), 0x40, "the view matrix"); }
PORT_FN(0x0044f520, "init_camera", init_camera, fp_init_camera)

// ==== calc_projection_matrix (0x44f590) ==========================================================================
// _11 = _22 = 2 tan(fov / 2), _33 = f / (f - n), _34 = 1, _43 = -(n f) / (f - n), _44 = 0: the original's own
// instruction sequence (fptan's result is not rounded by precision control; out of its range it leaves the stack a
// register short, and the rest of the sequence goes on with that)
static const float k_half = 0.5f, k_two = 2.0f;
static void __cdecl calc_projection_matrix(float n, float f, float fov) {
#ifdef VP_GCC
    __asm__ volatile("fld     %[fov]\n\t"
                     "fmul    %[half]\n\t"
                     "fptan\n\t"
                     "fstp    st(0)\n\t"
                     "fld     %[f]\n\t"
                     "fsub    %[n]\n\t"
                     "fxch    st(1)\n\t"
                     "fmul    %[two]\n\t"
                     "mov     eax, 0x005228f8\n\t"
                     "fst     dword ptr [eax]\n\t"
                     "fstp    dword ptr [eax + 0x14]\n\t"
                     "fld     %[f]\n\t"
                     "fdiv    st, st(1)\n\t"
                     "mov     dword ptr [eax + 0x2c], 0x3f800000\n\t"
                     "mov     dword ptr [eax + 0x3c], 0\n\t"
                     "fstp    dword ptr [eax + 0x28]\n\t"
                     "fld     %[f]\n\t"
                     "fmul    %[n]\n\t"
                     "fchs\n\t"
                     "fdivrp  st(1), st\n\t"
                     "fstp    dword ptr [eax + 0x38]"
                     :
                     : [fov] "m"(fov), [f] "m"(f), [n] "m"(n), [half] "m"(k_half), [two] "m"(k_two)
                     : VP_X87_CLOBBERS, "eax", "memory");
#else
    __asm {
        fld     fov
        fmul    dword ptr [k_half]
        fptan
        fstp    st(0)
        fld     f
        fsub    n
        fxch    st(1)
        fmul    dword ptr [k_two]
        mov     eax, 0x005228f8
        fst     dword ptr [eax]
        fstp    dword ptr [eax + 0x14]
        fld     f
        fdiv    st, st(1)
        mov     dword ptr [eax + 0x2c], 0x3f800000
        mov     dword ptr [eax + 0x3c], 0
        fstp    dword ptr [eax + 0x28]
        fld     f
        fmul    n
        fchs
        fdivrp  st(1), st
        fstp    dword ptr [eax + 0x38]
    }
#endif
}
static void fp_calc_projection_matrix(Footprint& f, float, float, float) { fp_proj(f); }
PORT_FN(0x0044f590, "calc_projection_matrix", calc_projection_matrix, fp_calc_projection_matrix)

// ================================================================================================================
// dxmatrix.obj
// ================================================================================================================
static void __cdecl dxMatrixBegin() {
    dxMatrixSetCamera_o(P<void>(S_IDENTITY_MAT));
    dxMatrixSetModel_o(P<void>(S_IDENTITY_MAT));
    dxMatrixSetProjection_o(P<void>(S_IDENTITY_MAT));
}
static void fp_dxMatrixBegin(Footprint& f) { fp_dx(f); }
PORT_FN(0x0045db40, "dxMatrixBegin", dxMatrixBegin, fp_dxMatrixBegin)

static void __cdecl dxMatrixSetModel(void* m) { d3d_SetTransform(*P<void*>(S_D3D), 0, 1, m); }
static void fp_dxMatrixSet(Footprint& f, void*) { fp_dx(f); }
PORT_FN(0x0045dba0, "dxMatrixSetModel", dxMatrixSetModel, fp_dxMatrixSet)
static void __cdecl dxMatrixSetCamera(void* m) { d3d_SetTransform(*P<void*>(S_D3D), 0, 2, m); }
PORT_FN(0x0045dbc0, "dxMatrixSetCamera", dxMatrixSetCamera, fp_dxMatrixSet)
static void __cdecl dxMatrixSetProjection(void* m) { d3d_SetTransform(*P<void*>(S_D3D), 0, 3, m); }
PORT_FN(0x0045dbe0, "dxMatrixSetProjection", dxMatrixSetProjection, fp_dxMatrixSet)

// ================================================================================================================
// feature.obj -- the mrFeature bits: 1 lighting, 2 (a byte of its own), 4 specular, 8 (nothing), 0x10 filtering,
// 0x20 mipmaps, 0x40 fog, 0x80 Z test, 0x100 Z writes, 0x200 mirror culling, 0x400 / 0x800 texture wrap U / V. The
// stack at 0x522df8 is {mask, blend mode} per level, 0x4f22cc its top; nothing bounds either end (FIX CANDIDATE: a
// push past the stack's end, or a pop below 0, writes outside it).
// ================================================================================================================

// ==== mr_feature_begin (0x457860) ================================================================================
static void __cdecl mr_feature_begin() {
    Ia(S_FSTACK_IDX) = 0;
    Ua(S_FSTACK) = 0xd93;
    Ua(S_FSTACK + 4) = 1;
}
static void fp_mr_feature_begin(Footprint& f) {
    f.add(P<uint8_t>(S_FSTACK_IDX), 4, "the feature stack's top");
    f.add(P<uint8_t>(S_FSTACK), 8, "the feature stack's first entry");
}
PORT_FN(0x00457860, "mr_feature_begin", mr_feature_begin, fp_mr_feature_begin)

// ==== mrEnable (0x457890) / mrDisable (0x457b00) =================================================================
static __forceinline uint32_t& feature_mask() { return Ua(S_FSTACK + 8u * (uint32_t)Ia(S_FSTACK_IDX)); }
static void __cdecl mrEnable(int f) {
    feature_mask() |= (uint32_t)f;
    uint32_t byte_at = 0;                           // the dxState byte a plain feature sets
    switch (f) {
    case 1:
        if (mrIsEnabled_o(1)) return;
        Ba(S_DXSTATE + 0x00) = 1;
        mrLightCalc_o(1);
        dxStateUpdate(P<void>(S_DXSTATE));
        return;
    case 2:
        if (mrIsEnabled_o(2)) return;
        Ba(S_FEAT2) = 1;
        return;
    case 4:
        if (mrIsEnabled_o(4)) return;
        Ba(S_FEAT4) = 1;
        Ba(S_DXSTATE + 0x0c) = 1;
        dxStateUpdate(P<void>(S_DXSTATE));
        mrLightCalcSpecular_o(1);
        return;
    case 8: return;
    case 0x40:
        if (mrIsEnabled_o(0x40)) return;
        Ba(S_DXSTATE + 0x01) = 1;
        dxStateUpdate(P<void>(S_DXSTATE));
        mrLightCalcFog_o(1);
        return;
    case 0x10: byte_at = S_DXSTATE + 0x0b; break;
    case 0x20: byte_at = S_DXSTATE + 0x15; break;
    case 0x80: byte_at = S_DXSTATE + 0x08; break;
    case 0x100: byte_at = S_DXSTATE + 0x09; break;
    case 0x200: byte_at = S_DXSTATE + 0x14; break;
    case 0x400: byte_at = S_DXSTATE + 0x16; break;
    case 0x800: byte_at = S_DXSTATE + 0x17; break;
    default:
        LogPanic(P<const char>(0x004f22d0));
        return;
    }
    if (mrIsEnabled_o(f)) return;
    Ba(byte_at) = 1;
    dxStateUpdate(P<void>(S_DXSTATE));
}
static void fp_mrEnable(Footprint& f, int) { fp_feature(f, Ia(S_FSTACK_IDX)); }
PORT_FN(0x00457890, "mrEnable", mrEnable, fp_mrEnable)

static void __cdecl mrDisable(int f) {
    feature_mask() &= ~(uint32_t)f;
    uint32_t byte_at = 0;
    switch (f) {
    case 1:
        if (!mrIsEnabled_o(1)) return;
        Ba(S_DXSTATE + 0x00) = 0;
        mrLightCalc_o(0);
        dxStateUpdate(P<void>(S_DXSTATE));
        return;
    case 2:
        if (!mrIsEnabled_o(2)) return;
        Ba(S_FEAT2) = 0;
        return;
    case 4:
        if (!mrIsEnabled_o(4)) return;
        Ba(S_FEAT4) = 0;
        Ba(S_DXSTATE + 0x0c) = 0;
        dxStateUpdate(P<void>(S_DXSTATE));
        mrLightCalcSpecular_o(0);
        return;
    case 8: return;
    case 0x40:
        if (!mrIsEnabled_o(0x40)) return;
        Ba(S_DXSTATE + 0x01) = 0;
        dxStateUpdate(P<void>(S_DXSTATE));
        mrLightCalcFog_o(0);
        return;
    case 0x10: byte_at = S_DXSTATE + 0x0b; break;
    case 0x20: byte_at = S_DXSTATE + 0x15; break;
    case 0x80: byte_at = S_DXSTATE + 0x08; break;
    case 0x100: byte_at = S_DXSTATE + 0x09; break;
    case 0x200: byte_at = S_DXSTATE + 0x14; break;
    case 0x400: byte_at = S_DXSTATE + 0x16; break;
    case 0x800: byte_at = S_DXSTATE + 0x17; break;
    default:
        LogPanic(P<const char>(0x004f22e0));
        return;
    }
    if (!mrIsEnabled_o(f)) return;
    Ba(byte_at) = 0;
    dxStateUpdate(P<void>(S_DXSTATE));
}
static void fp_mrDisable(Footprint& f, int) { fp_feature(f, Ia(S_FSTACK_IDX)); }
PORT_FN(0x00457b00, "mrDisable", mrDisable, fp_mrDisable)

// ==== mrIsEnabled (0x457d80) =====================================================================================
static uint8_t __cdecl mrIsEnabled(int f) {
    switch (f) {
    case 1: return Ba(S_DXSTATE + 0x00);
    case 2: return Ba(S_FEAT2);
    case 4: return Ba(S_FEAT4);
    case 8: return 0;
    case 0x10: return Ba(S_DXSTATE + 0x0b);
    case 0x20: return Ba(S_DXSTATE + 0x15);
    case 0x40: return Ba(S_DXSTATE + 0x01);
    case 0x80: return Ba(S_DXSTATE + 0x08);
    case 0x100: return Ba(S_DXSTATE + 0x09);
    case 0x200: return Ba(S_DXSTATE + 0x14);
    case 0x400: return Ba(S_DXSTATE + 0x16);
    case 0x800: return Ba(S_DXSTATE + 0x17);
    default:
        LogPanic(P<const char>(0x004f22f0));
        return 0;
    }
}
static void fp_mrIsEnabled(Footprint&, int) {}
PORT_FN(0x00457d80, "mrIsEnabled", mrIsEnabled, fp_mrIsEnabled)

// ==== mrSetBlendMode (0x457e30) ==================================================================================
// 1 normal, 2 additive, 4 modulate; below alpha level 2 always normal
static void __cdecl mrSetBlendMode(int m) {
    const int mode = Ia(S_ALPHA_LEVEL) < 2 ? 1 : m;
    Ia(S_FSTACK + 4 + 8u * (uint32_t)Ia(S_FSTACK_IDX)) = mode;
    if (mode == 1) Ia(S_DXSTATE + 0x10) = 0;
    else if (mode == 2) Ia(S_DXSTATE + 0x10) = 1;
    else if (mode == 4) Ia(S_DXSTATE + 0x10) = 2;
    else LogPanic(P<const char>(0x004f2300));
    dxStateUpdate(P<void>(S_DXSTATE));
}
static void fp_mrSetBlendMode(Footprint& f, int) { fp_feature(f, Ia(S_FSTACK_IDX)); }
PORT_FN(0x00457e30, "mrSetBlendMode", mrSetBlendMode, fp_mrSetBlendMode)

// ==== mrPushState (0x457ea0) / mrPopState (0x457ed0) =============================================================
static void __cdecl mrPushState() {
    const int32_t i = ++Ia(S_FSTACK_IDX);
    const uint32_t mask = Ua(S_FSTACK + 8u * (uint32_t)i - 8), blend = Ua(S_FSTACK + 8u * (uint32_t)i - 4);
    Ua(S_FSTACK + 8u * (uint32_t)i) = mask;
    Ua(S_FSTACK + 8u * (uint32_t)i + 4) = blend;
}
static void fp_mrPushState(Footprint& f) {
    f.add(P<uint8_t>(S_FSTACK_IDX), 4, "the feature stack's top");
    f.add(P<uint8_t>(S_FSTACK + 8u * (uint32_t)(Ia(S_FSTACK_IDX) + 1)), 8, "the new entry");
}
PORT_FN(0x00457ea0, "mrPushState", mrPushState, fp_mrPushState)

// every feature back to the level below's mask (0x200 last), then its blend mode
static void __cdecl mrPopState() {
    const int32_t i = --Ia(S_FSTACK_IDX);
    const uint32_t mask = Ua(S_FSTACK + 8u * (uint32_t)i);
    static const int k_order[11] = {1, 2, 4, 0x10, 0x20, 0x40, 0x80, 0x100, 0x400, 0x800, 0x200};
    for (int k = 0; k < 11; k++) {
        if (mask & (uint32_t)k_order[k]) mrEnable_o(k_order[k]);
        else mrDisable_o(k_order[k]);
    }
    mrSetBlendMode_o(Ia(S_FSTACK + 4 + 8u * (uint32_t)Ia(S_FSTACK_IDX)));
}
static void fp_mrPopState(Footprint& f) {
    f.add(P<uint8_t>(S_FSTACK_IDX), 4, "the feature stack's top");
    fp_feature(f, Ia(S_FSTACK_IDX) - 1);
}
PORT_FN(0x00457ed0, "mrPopState", mrPopState, fp_mrPopState)

// ================================================================================================================
// light.obj -- CPU lighting into D3DLVERTEX. The colour is a grey from the diffuse table, L_DIFF[0x80 + N.L*255]
// (N.L*255 rounded by the magic-number trick, negative -> 0; FIX: clamped to the 0x200-entry table, which a normal longer
// than 1 read past), OR'd with the model alpha; the specular is a grey from the 256-entry specular table
// (FIX: clamped to its ends; the original had no bound either way) with the fog factor in its alpha: 0xff when fog is off or the squared distance
// is within the fog start, else round((end - d^2) x scale), at least 0x80 and only its low byte kept.
// ================================================================================================================
typedef int(__cdecl* ProfStart_t)(const char*);
static __forceinline P3* V3(uint32_t addr) { return (P3*)(uintptr_t)addr; }

// the diffuse table entry for magic-rounded index i (negative -> 0), with the model alpha
static __forceinline uint32_t diffuse_of(int32_t i) {
    if (i < 0) i = 0;
    uint32_t k = (uint32_t)(Ia(L_BASE) + i);
    if (VP_FIX && k > 0x1ff) {                                        // FIX: a normal longer than 1 (a mod model's unnormalised
        k = 0x1ff;                                                    // normals) read past the 0x200-entry table: its last entry
        GX_FIX_FIRED();
    }
    return Ua(L_DIFF + 4u * k) | Ua(L_ALPHA_OR);
}
// the fog alpha byte (<< 24): d2 compared as the original compares it (a register, or the float it stored), d2sub
// what it subtracts from the fog end
static __forceinline uint32_t fog_alpha(double d2, double d2sub) {
    if (!(d2 > D(Fa(L_FOG_START)))) return 0xff000000u;              // fcom; test ah, 0x41: at or within the start
    int32_t a = magic_int((D(Fa(L_FOG_END)) - d2sub) * Fa(L_FOG_SCALE));
    if (a < 0x80) a = 0x80;                                           // cmp eax, 0x80; jge (signed)
    return ((uint32_t)a & 0xff) << 24;                                // mov cl, al
}
// the squared distance from the camera (model space) to vertex v: (the two of y and z) + x
static __forceinline double cam_d2(const uint8_t* v) {
    const double dz = D(Fa(L_MCAM + 8)) - Fp(v, 8), dy = D(Fa(L_MCAM + 4)) - Fp(v, 4), dx = D(Fa(L_MCAM)) - Fp(v, 0);
    return (dz * dz + dy * dy) + dx * dx;
}
static __forceinline void copy_pos_uv(uint8_t* o, const uint8_t* in) {
    cp12(o, in);
    cp4(o + 0x18, in + 0x18);
    cp4(o + 0x1c, in + 0x1c);
}
static __forceinline uint32_t spec_of(int32_t i) {
    if (VP_FIX && (uint32_t)i > 0xff) {                               // FIX: the specular index had no bound either way (an
        i = i < 0 ? 0 : 0xff;                                         // unnormalised normal or half vector): the table's ends
        GX_FIX_FIRED();
    }
    return Ua(L_SPECT + 4u * (uint32_t)i);
}

// ==== mr_light_begin (0x45add0), mr_light_end (0x45ade0), mr_light_release (0x45adf0), mr_light_restore (0x45ae00)
static void __cdecl mr_light_begin() { light_begin_o(); }
static void fp_mr_light_begin(Footprint& f) { fp_light_all(f); }
PORT_FN(0x0045add0, "mr_light_begin", mr_light_begin, fp_mr_light_begin)
static void __cdecl mr_light_end() { light_end_o(); }
static void fp_nothing(Footprint&) {}
PORT_FN(0x0045ade0, "mr_light_end", mr_light_end, fp_nothing)
static void __cdecl mr_light_release() { mr_light_end_o(); }
PORT_FN(0x0045adf0, "mr_light_release", mr_light_release, fp_nothing)
static void __cdecl mr_light_restore() { mr_light_begin_o(); }
PORT_FN(0x0045ae00, "mr_light_restore", mr_light_restore, fp_mr_light_begin)

// ==== light_begin (0x45ae10) =====================================================================================
// the tables at scale 1, day fog (300 m / 1500 m), the sun: (0, 0, 1) turned about y by 0, x by 85 degrees and z by 0
// (MatrixConcat, the original's aliasing kept), normalised and reversed; the eight lights cleared
static void __cdecl light_begin() {
    Ba(L_ON) = 1;
    Ia(L_MODE) = 0;
    Ia(L_BASE) = 0x80;
    Ua(L_SCALE) = 0x3f800000;
    make_diffuse_table_o(0x3f800000);
    make_specular_table_o(Ua(L_SCALE));
    Ba(L_SPEC) = 0;
    volatile float k255 = 255.0f, kfog = FB(0x34f88d25);              // a run-time product (docs/PORTING.md 7a)
    const double fs = D(k255) * kfog;
    Ua(L_FOG_START) = 0x47afc800;
    Ba(L_FOG) = 0;
    float v[3];
    memset(v, 0, 8);
    v[2] = FB(0x3f800000);
    Fa(L_FOG_SCALE) = st(fs);
    const float c0 = x87_cos_f(0.0);
    Ua(L_FOG_K) = 0x34f88d25;
    Ua(L_FOG_END) = 0x4a095440;
    const float s0 = x87_sin_f(0.0);
    const float ns0 = -s0;                                            // fld; fchs; fstp
    const float c85 = x87_cos_f(1.4835298527032137);
    const float s85 = x87_sin_f(1.4835298527032137);
    const float ns85 = -s85;
    float a[9] = {c0, 0.0f, ns0, 0.0f, FB(0x3f800000), 0.0f, s0, 0.0f, c0};
    float b[9] = {FB(0x3f800000), 0.0f, 0.0f, 0.0f, c85, s85, 0.0f, ns85, c85};
    MatrixConcat(a, b, a);
    const float b2[9] = {c0, s0, 0.0f, ns0, c0, 0.0f, 0.0f, 0.0f, FB(0x3f800000)};
    memcpy(b, b2, sizeof b);
    MatrixConcat(a, b, a);
    Fa(L_SUN) = (float)((D(a[6]) * v[2] + D(a[3]) * v[1]) + D(a[0]) * v[0]);
    Fa(L_SUN + 4) = (float)((D(a[7]) * v[2] + D(a[4]) * v[1]) + D(a[1]) * v[0]);
    Fa(L_SUN + 8) = (float)((D(a[5]) * v[1] + D(a[8]) * v[2]) + D(a[2]) * v[0]);
    const double inv = 1.0 / x87_sqrt((D(Fa(L_SUN)) * Fa(L_SUN) + D(Fa(L_SUN + 4)) * Fa(L_SUN + 4)) +
                                      D(Fa(L_SUN + 8)) * Fa(L_SUN + 8));
    Fa(L_SUN) = (float)(D(Fa(L_SUN)) * inv);
    Fa(L_SUN + 4) = (float)(D(Fa(L_SUN + 4)) * inv);
    Fa(L_SUN + 8) = (float)(inv * Fa(L_SUN + 8));
    Fa(L_SUN) = st(-D(Fa(L_SUN)));
    Fa(L_SUN + 4) = st(-D(Fa(L_SUN + 4)));
    Fa(L_SUN + 8) = st(-D(Fa(L_SUN + 8)));
    Ia(L_COUNT) = 0;
    for (int i = 0; i < 8; i++) {
        Ia(L_STATE + 4 * i) = 0;
        Ba(L_ACTIVE + i) = 0;
        uint32_t* p = P<uint32_t>(L_POS + 12 * i);
        p[0] = p[1] = p[2] = 0x49742400;                              // 1e6
        uint32_t* d = P<uint32_t>(L_DIR + 12 * i);
        d[0] = d[1] = d[2] = 0;
    }
    Ua(L_LIGHT_D) = 0x47742400;                                       // 250^2
    Ua(L_LIGHT_R2) = 0x49095440;                                      // 750^2
    Ia(L_ENV) = 0;
    set_light_state_o();
    mrLightSetAlpha_o(0x3f800000);
}
static void fp_light_begin(Footprint& f) { fp_light_all(f); }
PORT_FN(0x0045ae10, "light_begin", light_begin, fp_light_begin)

// ==== mrLightPlaceCamera (0x45b150) ==============================================================================
// the camera, and every light's frames-left count down by one (and all marked inactive until the next model)
static void __cdecl mrLightPlaceCamera(const float* pos, const float* dir) {
    cp12(P<uint8_t>(L_CAMPOS), pos);
    cp12(P<uint8_t>(L_CAMDIR), dir);
    for (int i = 0; i < 8; i++) {
        Ba(L_ACTIVE + i) = 0;
        if (Ia(L_STATE + 4 * i) > 0) Ia(L_STATE + 4 * i) -= 1;
    }
}
static void fp_mrLightPlaceCamera(Footprint& f, const float*, const float*) {
    f.add(P<uint8_t>(L_CAMPOS), 0x18, "light: the camera");
    f.add(P<uint8_t>(L_STATE), 0x28, "light: the lights' frames left and active flags");
}
PORT_FN(0x0045b150, "mrLightPlaceCamera", mrLightPlaceCamera, fp_mrLightPlaceCamera)

// ==== mrLightPlaceModel (0x45b1b0) ===============================================================================
// the camera, the sun and the camera's axis in the model's space (the sun x 255), the specular half vector, the
// model's rotation, and each light with frames left and within 750 m of the camera placed in model space
static void __cdecl mrLightPlaceModel(const uint8_t* fr) {
    Fa(L_MCAM) = (float)(D(Fa(L_CAMPOS)) - Fp(fr, 0x24));
    Fa(L_MCAM + 4) = (float)(D(Fa(L_CAMPOS + 4)) - Fp(fr, 0x28));
    Fa(L_MCAM + 8) = (float)(D(Fa(L_CAMPOS + 8)) - Fp(fr, 0x2c));
    MatrixMulPointInv(P<void>(L_MCAM), P<void>(L_MCAM), fr);
    memcpy(P<void>(L_MMAT), fr, 36);                                  // rep movsd
    if (Ba(L_ON)) {
        float l[3];
        MatrixMulPointInv(l, P<void>(L_SUN), fr);
        Fa(L_MSUN) = (float)(D(l[0]) * 255.0);
        Fa(L_MSUN + 4) = (float)(D(l[1]) * 255.0);
        Fa(L_MSUN + 8) = (float)(D(l[2]) * 255.0);
        MatrixMulPointInv(P<void>(L_MCAMDIR), P<void>(L_CAMDIR), fr);
        if (Ba(L_SPEC)) {
            Fa(L_HALF) = (float)((D(l[0]) - Fa(L_MCAMDIR)) * 0.5);
            Fa(L_HALF + 4) = (float)((D(l[1]) - Fa(L_MCAMDIR + 4)) * 0.5);
            Fa(L_HALF + 8) = (float)((D(l[2]) - Fa(L_MCAMDIR + 8)) * 0.5);
            const double inv = 1.0 / x87_sqrt((D(Fa(L_HALF)) * Fa(L_HALF) + D(Fa(L_HALF + 4)) * Fa(L_HALF + 4)) +
                                              D(Fa(L_HALF + 8)) * Fa(L_HALF + 8));
            Fa(L_HALF) = (float)(D(Fa(L_HALF)) * inv);
            Fa(L_HALF + 4) = (float)(D(Fa(L_HALF + 4)) * inv);
            Fa(L_HALF + 8) = (float)(inv * Fa(L_HALF + 8));
        }
    }
    for (int i = 0; i < 8; i++) {
        if (Ia(L_STATE + 4 * i) == 0) continue;
        const uint32_t pos = L_POS + 12 * i, mpos = L_MPOS + 12 * i;
        const float dx = (float)(D(Fa(pos)) - Fa(L_CAMPOS));
        const float dy = (float)(D(Fa(pos + 4)) - Fa(L_CAMPOS + 4));
        const float dz = (float)(D(Fa(pos + 8)) - Fa(L_CAMPOS + 8));
        if (!((D(dy) * dy + D(dz) * dz) + D(dx) * dx >= D(Fa(L_LIGHT_R2)))) {   // test ah, 1: less or unordered
            Ba(L_ACTIVE + i) = 1;
            Fa(mpos) = (float)(D(Fa(pos)) - Fp(fr, 0x24));
            Fa(mpos + 4) = (float)(D(Fa(pos + 4)) - Fp(fr, 0x28));
            Fa(mpos + 8) = (float)(D(Fa(pos + 8)) - Fp(fr, 0x2c));
            MatrixMulPointInv(P<void>(mpos), P<void>(mpos), fr);
            MatrixMulPointInv(P<void>(L_MDIR + 12 * i), P<void>(L_DIR + 12 * i), fr);
        } else {
            Ba(L_ACTIVE + i) = 0;
        }
    }
}
static void fp_place_model(Footprint& f) {
    f.add(P<uint8_t>(L_MSUN), 0x30, "light: the model-space sun, camera, axis and half vector");
    f.add(P<uint8_t>(L_ACTIVE), 8, "light: the active flags");
    f.add(P<uint8_t>(L_MPOS), 0xc0, "light: the lights in model space");
    f.add(P<uint8_t>(L_MMAT), 0x24, "light: the model's rotation");
}
static void fp_mrLightPlaceModel(Footprint& f, const uint8_t*) { fp_place_model(f); }
PORT_FN(0x0045b1b0, "mrLightPlaceModel", mrLightPlaceModel, fp_mrLightPlaceModel)

// footprint: every surface's vertex range in `out`, and the count
static void fp_lit_surfaces(Footprint& f, const uint8_t* info, uint8_t* out) {
    f.add(P<uint8_t>(L_COUNT), 4, "light: vertices lit");
    if (!info) return;
    const int32_t ns = Ip(info, 8);
    const uint8_t* s = Pp(info, 0xc);
    for (int32_t k = 0; k < ns && k < 4096; k++, s += 0x20) {
        const int32_t first = Sp(s, 0x18), n = Sp(s, 0x1a) - first;
        if (n > 0) f.add(out + 32 * first, 32u * (uint32_t)n, "the lit vertices");
    }
}
static void fp_lit_all(Footprint& f, const uint8_t* info, uint8_t* out) {
    f.add(P<uint8_t>(L_COUNT), 4, "light: vertices lit");
    if (info && Ip(info, 0) > 0) f.add(out, 32u * (uint32_t)Ip(info, 0), "the lit vertices");
}

// ==== mrLightVerts (0x45b410) ====================================================================================
static void __cdecl mrLightVerts(uint8_t* info, uint8_t* out) {
    if (!info || !Ip(info, 8) || !Ip(info, 0)) return;
    if (!Ba(L_ON)) nocalc_light_o(info, out);
    else if (!Ia(L_MODE)) calc_day_o(info, out);
    else calc_night_o(info, out);
}
static void fp_mrLightVerts(Footprint& f, uint8_t* info, uint8_t* out) {
    if (!info || !Ip(info, 8) || !Ip(info, 0)) return;
    if (!Ba(L_ON)) fp_lit_all(f, info, out);
    else fp_lit_surfaces(f, info, out);
}
PORT_FN(0x0045b410, "mrLightVerts", mrLightVerts, fp_mrLightVerts)

// ==== calc_day (0x45b460) ========================================================================================
// per surface, types 0 and 1: position and UVs copied, the sun's diffuse, the specular (the half vector), the env
// map for type 1 (copied UVs, or the sphere map), the fog; type 2 full bright. Profiled ("calc_day").
static void __cdecl calc_day(uint8_t* info, uint8_t* out) {
    ((Void_t)Ua(L_PROF))();                                          // i_pr_overhead_begin
    const int prof = ((ProfStart_t)Ua(L_PROF + 8))(P<const char>(0x004f38fc));
    ((Void_t)Ua(L_PROF + 4))();                                      // i_pr_overhead_end
    int32_t ns = Ip(info, 8);
    uint8_t* surf = Pp(info, 0xc);
    if (ns > 0) do {
        const int32_t first = Sp(surf, 0x18);
        int32_t n = Sp(surf, 0x1a) - first;
        uint8_t* o = out + 32 * first;
        const uint8_t* in = Pp(info, 4) + 32 * first;
        const uint32_t type = Bp(surf, 0x10);
        if (type <= 1) {
            if (n > 0) {
                double slot = 0.0;                                   // the fldz a fog distance is kept in
                do {
                    copy_pos_uv(o, in);
                    Up(o, 0x10) = diffuse_of(magic_int((D(Fp(in, 0x14)) * Fa(L_MSUN + 8) + D(Fp(in, 0xc)) * Fa(L_MSUN)) +
                                                       D(Fp(in, 0x10)) * Fa(L_MSUN + 4)));
                    uint32_t spec = 0;
                    if (Ba(L_SPEC)) {
                        const double h = (D(Fp(in, 0x14)) * Fa(L_HALF + 8) + D(Fp(in, 0xc)) * Fa(L_HALF)) +
                                         D(Fp(in, 0x10)) * Fa(L_HALF + 4);
                        const float hf = st(h);
                        if (h > 0.0) spec = spec_of(x87_ftol(D(hf) * 255.0));
                    }
                    if (Ia(L_ENV) != 0 && Bp(surf, 0x10) == 1) {
                        if (Ia(L_ENV) != 2) {
                            cp4(o + 0x18, in + 0x18);
                            cp4(o + 0x1c, in + 0x1c);
                        } else {
                            const double t = (((D(Fa(L_MCAM + 4)) - Fp(in, 4)) * Fp(in, 0x10) +
                                               (D(Fa(L_MCAM + 8)) - Fp(in, 8)) * Fp(in, 0x14)) +
                                              (D(Fa(L_MCAM)) - Fp(in, 0)) * Fp(in, 0xc)) * -2.0;
                            const float rx = (float)(D(Fp(in, 0)) - (D(Fp(in, 0xc)) * t + Fa(L_MCAM)));
                            const float ry = (float)(D(Fp(in, 4)) - (D(Fp(in, 0x10)) * t + Fa(L_MCAM + 4)));
                            const double rz = D(Fp(in, 8)) - (t * Fp(in, 0x14) + Fa(L_MCAM + 8));
                            const float rzf = st(rz);
                            const float* m = P<float>(L_MMAT);
                            const double Y = (rz * m[5] + D(ry) * m[4]) + D(rx) * m[3];
                            const double Z = (D(rzf) * m[8] + D(ry) * m[7]) + D(rx) * m[6];
                            const float xf = (float)((D(rzf) * m[2] + D(ry) * m[1]) + D(rx) * m[0]);
                            const double inv = 1.0 / x87_sqrt((Y * Y + D(xf) * xf) + Z * Z);
                            const double xn = D(xf) * inv;
                            st(xn);
                            Fp(o, 0x18) = (float)(xn * 0.5 + 0.5);
                            Fp(o, 0x1c) = (float)((Y * inv) * -0.5 + 0.5);
                        }
                    }
                    if (Ba(L_FOG)) {
                        const double dy = D(Fa(L_MCAM + 4)) - Fp(in, 4), dz = D(Fa(L_MCAM + 8)) - Fp(in, 8),
                                     dx = D(Fa(L_MCAM)) - Fp(in, 0);
                        slot = (dy * dy + dz * dz) + dx * dx;
                        Up(o, 0x14) = spec | fog_alpha(slot, slot);
                    } else {
                        Up(o, 0x14) = spec | 0xff000000u;
                    }
                    Ia(L_COUNT) += 1;
                    in += 0x20;
                    o += 0x20;
                } while (--n != 0);
                st(slot);                                            // fstp dword: the last distance, popped
            }
        } else if (type == 2) {
            if (n > 0) do {
                cp12(o, in);
                cp4(o + 0x18, in + 0x18);
                cp4(o + 0x1c, in + 0x1c);
                Up(o, 0x10) = 0xffffffffu;
                Up(o, 0x14) = 0xff000000u;
                Ia(L_COUNT) += 1;
                in += 0x20;
                o += 0x20;
            } while (--n != 0);
        }
        surf += 0x20;
    } while (--ns != 0);
    ((Void_t)Ua(L_PROF))();
    ((IntV_t)Ua(L_PROF + 0xc))(prof);                                 // i_prof_stop
    ((Void_t)Ua(L_PROF + 4))();
}
static void fp_calc(Footprint& f, uint8_t* info, uint8_t* out) { fp_lit_surfaces(f, info, out); }
PORT_FN(0x0045b460, "calc_day", calc_day, fp_calc)

// ==== calc_night (0x45b810) ======================================================================================
// types 0 and 1: past the fog end black (colour 0, fog 0xff); else the lights' sum (N.L x (1 - d^2/D)^2 x 6 each,
// capped at 128) as the diffuse, the sun's specular, the env map. The position is NOT copied (the original's own);
// type 2 full bright (colour and fog only).
static void __cdecl calc_night(uint8_t* info, uint8_t* out) {
    int32_t ns = Ip(info, 8);
    uint8_t* surf = Pp(info, 0xc);
    if (ns > 0) do {
        const int32_t first = Sp(surf, 0x18);
        int32_t n = Sp(surf, 0x1a) - first;
        const uint8_t* in = Pp(info, 4) + 32 * first;
        uint8_t* o = out + 32 * first;
        const uint32_t type = Bp(surf, 0x10);
        if (type <= 1) {
            if (n > 0) do {
                const float k = st((D(Fa(L_FOG_END)) - cam_d2(in)) * Fa(L_FOG_K));
                if (fbits(k) & 0x80000000u) {                        // test byte [k+3], 0x80
                    Up(o, 0x10) = 0;
                    Up(o, 0x14) = 0xff000000u;
                } else {
                    double acc = 0.0;
                    for (int i = 0; i < 8; i++) {
                        if (!Ba(L_ACTIVE + i)) continue;
                        const uint32_t mp = L_MPOS + 12 * i, md = L_MDIR + 12 * i;
                        const float dx = (float)(D(Fp(in, 0)) - Fa(mp));
                        const float dy = (float)(D(Fp(in, 4)) - Fa(mp + 4));
                        const double rdz = D(Fp(in, 8)) - Fa(mp + 8);
                        const float dz = st(rdz);
                        const double c = (rdz * Fa(md + 8) + D(Fa(md)) * dx) + D(Fa(md + 4)) * dy;
                        const float cf = st(c);
                        if (!(c > 0.0)) continue;
                        const double att = 1.0 - ((D(dy) * dy + D(dz) * dz) + D(dx) * dx) / Fa(L_LIGHT_D);
                        const float attf = st(att);
                        if (!(att > 0.0)) continue;
                        acc = acc + ((D(attf) * attf) * cf) * 6.0;
                    }
                    float accf = st(acc);
                    if (acc > 128.0) accf = FB(0x43000000);
                    int32_t ci = magic_int(D(accf));
                    if (ci < 0) ci = 0;
                    if (ci > 0xff) ci = 0xff;
                    Up(o, 0x10) = Ua(L_DIFF + 4u * (uint32_t)(Ia(L_BASE) + ci)) | Ua(L_ALPHA_OR);
                    uint32_t spec = 0;
                    if (Ba(L_SPEC)) {
                        const double h = (D(Fp(in, 0x14)) * Fa(L_HALF + 8) + D(Fp(in, 0xc)) * Fa(L_HALF)) +
                                         D(Fp(in, 0x10)) * Fa(L_HALF + 4);
                        const float hf = st(h);
                        if (h > 0.0) spec = spec_of(x87_ftol(D(hf) * 255.0));
                    }
                    Up(o, 0x14) = spec | 0xff000000u;
                    if (Ia(L_ENV) == 2) {
                        const double t = (((D(Fa(L_MCAM)) - Fp(in, 0)) * Fp(in, 0xc) +
                                           (D(Fa(L_MCAM + 8)) - Fp(in, 8)) * Fp(in, 0x14)) +
                                          (D(Fa(L_MCAM + 4)) - Fp(in, 4)) * Fp(in, 0x10)) * -2.0;
                        const float rx = (float)(D(Fp(in, 0)) - (D(Fp(in, 0xc)) * t + Fa(L_MCAM)));
                        const float ry = (float)(D(Fp(in, 4)) - (D(Fp(in, 0x10)) * t + Fa(L_MCAM + 4)));
                        const double rz = D(Fp(in, 8)) - (t * Fp(in, 0x14) + Fa(L_MCAM + 8));
                        const float rzf = st(rz);
                        const float* m = P<float>(L_MMAT);
                        const double Y = (rz * m[5] + D(m[4]) * ry) + D(m[3]) * rx;
                        const double Z = (D(m[7]) * ry + D(m[8]) * rzf) + D(m[6]) * rx;
                        const float xf = (float)((D(m[1]) * ry + D(m[2]) * rzf) + D(m[0]) * rx);
                        const double inv = 1.0 / x87_sqrt((Z * Z + D(xf) * xf) + Y * Y);
                        const double xn = D(xf) * inv;
                        st(xn);
                        Fp(o, 0x18) = (float)(xn * 0.5 + 0.5);
                        Fp(o, 0x1c) = (float)((Y * inv) * -0.5 + 0.5);
                    } else if (Ia(L_ENV) == 1) {
                        cp4(o + 0x18, in + 0x18);
                        cp4(o + 0x1c, in + 0x1c);
                    }
                }
                Ia(L_COUNT) += 1;
                in += 0x20;
                o += 0x20;
            } while (--n != 0);
        } else if (type == 2) {
            if (n > 0) do {
                Up(o, 0x10) = 0xffffffffu;
                Up(o, 0x14) = 0xff000000u;
                Ia(L_COUNT) += 1;
                o += 0x20;
            } while (--n != 0);
        }
        surf += 0x20;
    } while (--ns != 0);
}
PORT_FN(0x0045b810, "calc_night", calc_night, fp_calc)

// ==== nocalc_light (0x45bc00) ====================================================================================
// every vertex (all of the model's, not by surface): position and UVs, white at the model alpha, no fog
static void __cdecl nocalc_light(uint8_t* info, uint8_t* out) {
    const uint32_t color = ((uint32_t)Ba(L_ALPHA_B) << 24) | 0xffffffu;
    int32_t n = Ip(info, 0);
    const uint8_t* in = Pp(info, 4);
    if (n > 0) do {
        copy_pos_uv(out, in);
        Up(out, 0x10) = color;
        Up(out, 0x14) = 0xff000000u;
        Ia(L_COUNT) += 1;
        in += 0x20;
        out += 0x20;
    } while (--n != 0);
}
static void fp_nocalc_light(Footprint& f, uint8_t* info, uint8_t* out) { fp_lit_all(f, info, out); }
PORT_FN(0x0045bc00, "nocalc_light", nocalc_light, fp_nocalc_light)

// ==== mrLightFogVerts (0x45bc70) =================================================================================
// the fog only (the colour copied, every surface whatever its type); no fog: nocalc_light; night: calc_night_2
static void __cdecl mrLightFogVerts(uint8_t* info, uint8_t* out) {
    if (!Ba(L_FOG)) { nocalc_light_o(info, out); return; }
    if (Ia(L_MODE) == 1) { calc_night_2_o(info, out); return; }
    int32_t ns = Ip(info, 8);
    uint8_t* surf = Pp(info, 0xc);
    if (ns <= 0) return;
    double slot = 0.0;
    do {
        const int32_t first = Sp(surf, 0x18);
        int32_t n = Sp(surf, 0x1a) - first;
        const uint8_t* in = Pp(info, 4) + 32 * first;
        uint8_t* o = out + 32 * first;
        if (n > 0) do {
            cp12(o, in);
            cp4(o + 0x18, in + 0x18);
            cp4(o + 0x1c, in + 0x1c);
            cp4(o + 0x10, in + 0x10);
            slot = cam_d2(in);
            Up(o, 0x14) = fog_alpha(slot, slot);
            Ia(L_COUNT) += 1;
            in += 0x20;
            o += 0x20;
        } while (--n != 0);
        surf += 0x20;
    } while (--ns != 0);
    st(slot);
}
static void fp_mrLightFogVerts(Footprint& f, uint8_t* info, uint8_t* out) {
    if (!Ba(L_FOG)) fp_lit_all(f, info, out);
    else fp_lit_surfaces(f, info, out);
}
PORT_FN(0x0045bc70, "mrLightFogVerts", mrLightFogVerts, fp_mrLightFogVerts)

// ==== calc_night_2 (0x45bdc0) ====================================================================================
// types 0 and 1: past the fog end black; else the diffuse table's zero entry, the fog alpha OR'd into what the
// specular held; type 2 full bright
static void __cdecl calc_night_2(uint8_t* info, uint8_t* out) {
    int32_t ns = Ip(info, 8);
    uint8_t* surf = Pp(info, 0xc);
    if (ns > 0) do {
        const int32_t first = Sp(surf, 0x18);
        int32_t n = Sp(surf, 0x1a) - first;
        const uint8_t* in = Pp(info, 4) + 32 * first;
        uint8_t* o = out + 32 * first;
        const uint32_t type = Bp(surf, 0x10);
        if (type <= 1) {
            if (n > 0) do {
                const double dy = D(Fa(L_MCAM + 4)) - Fp(in, 4), dz = D(Fa(L_MCAM + 8)) - Fp(in, 8),
                             dx = D(Fa(L_MCAM)) - Fp(in, 0);
                const float k = st((D(Fa(L_FOG_END)) - ((dy * dy + dz * dz) + dx * dx)) * Fa(L_FOG_K));
                if (fbits(k) & 0x80000000u) {
                    Up(o, 0x10) = 0;
                    Up(o, 0x14) = 0xff000000u;
                } else {
                    Up(o, 0x10) = Ua(L_DIFF + 4u * (uint32_t)Ia(L_BASE)) | Ua(L_ALPHA_OR);   // 0x80000000 - 0x80000000
                    Up(o, 0x14) |= 0xff000000u;
                }
                Ia(L_COUNT) += 1;
                in += 0x20;
                o += 0x20;
            } while (--n != 0);
        } else if (type == 2) {
            if (n > 0) do {
                Up(o, 0x10) = 0xffffffffu;
                Up(o, 0x14) = 0xff000000u;
                Ia(L_COUNT) += 1;
                o += 0x20;
            } while (--n != 0);
        }
        surf += 0x20;
    } while (--ns != 0);
}
PORT_FN(0x0045bdc0, "calc_night_2", calc_night_2, fp_calc)

// ==== mrLightSetAlpha (0x45bf00) =================================================================================
static void __cdecl mrLightSetAlpha(float a) {
    Ua(L_ALPHA) = fbits(a);
    const int32_t v = x87_ftol(D(a) * 255.0);
    Ba(L_ALPHA_B) = (uint8_t)v;
    Ua(L_ALPHA_OR) = (uint32_t)v << 24;
}
static void fp_alpha(Footprint& f) { f.add(P<uint8_t>(L_ALPHA), 0xc, "light: the model alpha"); }
static void fp_mrLightSetAlpha(Footprint& f, float) { fp_alpha(f); }
PORT_FN(0x0045bf00, "mrLightSetAlpha", mrLightSetAlpha, fp_mrLightSetAlpha)

// ==== mrLightEnv (0x45bf30), mrLightCalc (0x45bf40), mrLightCalcFog (0x45bf50), mrLightCalcSpecular (0x45bf60) ====
static void __cdecl mrLightEnv(int e) {
    Ia(L_ENV) = e;
    set_light_state_o();
}
static void fp_mrLightEnv(Footprint& f, int) { f.add(P<uint8_t>(L_ENV), 4, "light: env mode"); fp_light_state(f); }
PORT_FN(0x0045bf30, "mrLightEnv", mrLightEnv, fp_mrLightEnv)

static void __cdecl mrLightCalc(uint8_t on) {
    Ba(L_ON) = on;
    set_light_state_o();
}
static void fp_mrLightCalc(Footprint& f, uint8_t) { f.add(P<uint8_t>(L_ON), 1, "light: calculating"); fp_light_state(f); }
PORT_FN(0x0045bf40, "mrLightCalc", mrLightCalc, fp_mrLightCalc)

static void __cdecl mrLightCalcFog(uint8_t on) {
    Ba(L_FOG) = on;
    set_light_state_o();
}
static void fp_mrLightCalcFog(Footprint& f, uint8_t) { f.add(P<uint8_t>(L_FOG), 1, "light: fog"); fp_light_state(f); }
PORT_FN(0x0045bf50, "mrLightCalcFog", mrLightCalcFog, fp_mrLightCalcFog)

static void __cdecl mrLightCalcSpecular(uint8_t on) {
    Ba(L_SPEC) = on;
    set_light_state_o();
}
static void fp_mrLightCalcSpecular(Footprint& f, uint8_t) { f.add(P<uint8_t>(L_SPEC), 1, "light: specular"); fp_light_state(f); }
PORT_FN(0x0045bf60, "mrLightCalcSpecular", mrLightCalcSpecular, fp_mrLightCalcSpecular)

// ==== mrLightMode (0x45bf70) =====================================================================================
// 0 day (fog 300 m / 1500 m, fog colour 0x8c8ca0), anything else night (100 m / 500 m, black)
static void __cdecl mrLightMode(int m) {
    Ia(L_BASE) = 0x80;
    Ia(L_MODE) = m;
    volatile float k255 = 255.0f;
    if (m == 0) {
        volatile float k = FB(0x34f88d25);
        const double fs = D(k255) * k;
        Ua(L_FOG_START) = 0x47afc800;
        Ua(L_FOG_END) = 0x4a095440;
        Ua(L_FOG_K) = 0x34f88d25;
        Fa(L_FOG_SCALE) = st(fs);
        Ua(L_SCALE) = 0x3f800000;
        make_diffuse_table_o(0x3f800000);
        make_specular_table_o(Ua(L_SCALE));
        Ua(S_DXSTATE + 4) = 0x8c8ca0;
    } else {
        volatile float k = FB(0x368bcf65);
        const double fs = D(k255) * k;
        Ua(L_FOG_START) = 0x461c4000;
        Ua(L_FOG_END) = 0x48742400;
        Ua(L_FOG_K) = 0x368bcf65;
        Fa(L_FOG_SCALE) = st(fs);
        Ua(L_SCALE) = 0x3f800000;
        make_diffuse_table_o(0x3f800000);
        make_specular_table_o(Ua(L_SCALE));
        Ua(S_DXSTATE + 4) = 0;
    }
}
static void fp_mrLightMode(Footprint& f, int) {
    f.add(P<uint8_t>(L_MODE), 4, "light: mode");
    f.add(P<uint8_t>(L_BASE), 8, "light: table zero and scale");
    f.add(P<uint8_t>(L_FOG_START), 0x10, "light: fog");
    f.add(P<uint8_t>(L_DIFF), 0x800, "light: the diffuse table");
    f.add(P<uint8_t>(L_SPECT), 0x400, "light: the specular table");
    f.add(P<uint8_t>(S_DXSTATE + 4), 4, "dxState: the fog colour");
}
PORT_FN(0x0045bf70, "mrLightMode", mrLightMode, fp_mrLightMode)

// ==== mrLightSpecial (0x45c050) ==================================================================================
// at night, light i (FIX: 0..7; others ignored) on for 2 frames at the frame's position, along its z axis
static void __cdecl mrLightSpecial(int i, const uint8_t* fr) {
    if (Ia(L_MODE) != 1) return;
    if (VP_FIX && (uint32_t)i >= 8) {                                // FIX: a light number past the eight wrote over the
        GX_FIX_FIRED();                                               // statics after the lights' tables: ignored
        return;
    }
    Ia(L_STATE + 4u * (uint32_t)i) = 2;
    cp12(P<uint8_t>(L_POS + 12u * (uint32_t)i), fr + 0x24);
    cp12(P<uint8_t>(L_DIR + 12u * (uint32_t)i), fr + 0x18);
}
static void fp_mrLightSpecial(Footprint& f, int i, const uint8_t*) {
    if (Ia(L_MODE) != 1) return;
    if (VP_FIX && (uint32_t)i >= 8) return;
    f.add(P<uint8_t>(L_STATE + 4u * (uint32_t)i), 4, "light: frames left");
    f.add(P<uint8_t>(L_POS + 12u * (uint32_t)i), 12, "light: position");
    f.add(P<uint8_t>(L_DIR + 12u * (uint32_t)i), 12, "light: direction");
}
PORT_FN(0x0045c050, "mrLightSpecial", mrLightSpecial, fp_mrLightSpecial)

// ==== make_diffuse_table (0x45c0b0) / make_specular_table (0x45c100) =============================================
static void __cdecl make_diffuse_table(float s) {
    for (int32_t i = 0; i < 0x200; i++) {
        const int32_t c = i < 0xff ? i : 0xff;
        const uint32_t b = (uint8_t)x87_ftol(D(c) * s);
        Ua(L_DIFF + 4u * (uint32_t)i) = (b << 16) | (b << 8) | b;
    }
}
static void fp_make_diffuse_table(Footprint& f, float) { f.add(P<uint8_t>(L_DIFF), 0x800, "light: the diffuse table"); }
PORT_FN(0x0045c0b0, "make_diffuse_table", make_diffuse_table, fp_make_diffuse_table)

// (i / 255 x s)^10 x 70
// The power stays on the x87 stack the whole way, as the original's does: x^10 of a large scale passes a double's
// range (a spill to a C double would overflow where the original's register doesn't), so it is one asm block, the
// original's instructions, ending in __ftol's fistp qword with chop (x87.h).
static const float k_inv255 = FB(0x3b808081), k_seventy = 70.0f;
static __forceinline int32_t spec_power(int32_t i, float s) {
    int32_t lo;
    int64_t q;
    uint16_t cw, chop;
#ifdef VP_GCC
    __asm__ volatile("fild    %[i]\n\t"
                     "fmul    %[inv255]\n\t"
                     "fmul    %[s]\n\t"
                     "fmul    st, st(0)\n\t"
                     "fld     st(0)\n\t"
                     "fmul    st, st(1)\n\t"
                     "fmul    st, st(0)\n\t"
                     "fmulp   st(1), st\n\t"
                     "fmul    %[seventy]\n\t"
                     "fnstcw  %[cw]\n\t"
                     "mov     ax, %[cw]\n\t"
                     "or      ah, 0x0c\n\t"
                     "mov     %[chop], ax\n\t"
                     "fldcw   %[chop]\n\t"
                     "fistp   %[q]\n\t"
                     "fldcw   %[cw]"
                     : [cw] "=m"(cw), [chop] "=m"(chop), [q] "=m"(q)
                     : [i] "m"(i), [s] "m"(s), [inv255] "m"(k_inv255), [seventy] "m"(k_seventy)
                     : VP_X87_CLOBBERS, "eax", "cc");
#else
    __asm {
        fild    i
        fmul    dword ptr [k_inv255]
        fmul    s
        fmul    st, st(0)
        fld     st(0)
        fmul    st, st(1)
        fmul    st, st(0)
        fmulp   st(1), st
        fmul    dword ptr [k_seventy]
        fnstcw  cw
        mov     ax, cw
        or      ah, 0x0c
        mov     chop, ax
        fldcw   chop
        fistp   qword ptr q
        fldcw   cw
    }
#endif
    lo = (int32_t)q;
    return lo;
}
static void __cdecl make_specular_table(float s) {
    for (int32_t i = 0; i < 0x100; i++) {
        const uint32_t b = (uint8_t)spec_power(i, s);
        Ua(L_SPECT + 4u * (uint32_t)i) = (b << 16) | (b << 8) | b;
    }
}
static void fp_make_specular_table(Footprint& f, float) { f.add(P<uint8_t>(L_SPECT), 0x400, "light: the specular table"); }
PORT_FN(0x0045c100, "make_specular_table", make_specular_table, fp_make_specular_table)

// ==== set_light_state (0x45c160) =================================================================================
static void __cdecl set_light_state() {
    uint32_t lit, pre;
    if (!Ba(L_ON)) lit = pre = F_LIGHT_OFF;
    else if (Ba(L_FOG)) {
        if (Ia(L_ENV)) { lit = F_NOPRE_FOG_ENV; pre = F_PRE_FOG_ENV; }
        else if (Ba(L_SPEC)) { lit = F_NOPRE_FOG_SOMEENV; pre = F_PRE_FOG_SOMEENV; }
        else { lit = F_NOPRE_FOG_NOENV; pre = F_PRE_FOG_NOENV; }
    } else {
        if (Ia(L_ENV)) { lit = F_NOPRE_NOFOG_ENV; pre = F_PRE_NOFOG_ENV; }
        else if (Ba(L_SPEC)) { lit = F_NOPRE_NOFOG_SOMEENV; pre = F_PRE_NOFOG_SOMEENV; }
        else { lit = F_NOPRE_NOFOG_NOENV; pre = F_PRE_NOFOG_NOENV; }
    }
    Ua(L_FN_LIT) = lit;
    Ua(L_FN_PRELIT) = pre;
}
static void fp_set_light_state(Footprint& f) { fp_light_state(f); }
PORT_FN(0x0045c160, "set_light_state", set_light_state, fp_set_light_state)

// ==== LightSurfaceLit (0x45c230) / LightSurfacePreLit (0x45c250) =================================================
static void fp_surface(Footprint& f, const uint8_t* s, uint8_t* out) {
    f.add(P<uint8_t>(L_COUNT), 4, "light: vertices lit");
    const int32_t first = Sp(s, 0x18), n = Sp(s, 0x1a) - first;
    if (n > 0) f.add(out + 32 * first, 32u * (uint32_t)n, "the lit vertices");
}
static void __cdecl LightSurfaceLit(uint8_t* s, uint8_t* in, uint8_t* out) { ((LightFn_t)Ua(L_FN_LIT))(s, in, out); }
static void fp_LightSurface(Footprint& f, uint8_t* s, uint8_t*, uint8_t* out) { fp_surface(f, s, out); }
PORT_FN(0x0045c230, "LightSurfaceLit", LightSurfaceLit, fp_LightSurface)
static void __cdecl LightSurfacePreLit(uint8_t* s, uint8_t* in, uint8_t* out) { ((LightFn_t)Ua(L_FN_PRELIT))(s, in, out); }
PORT_FN(0x0045c250, "LightSurfacePreLit", LightSurfacePreLit, fp_LightSurface)

// the surface's vertices: first, count, in, out
struct SurfRange { int32_t n; const uint8_t* in; uint8_t* o; };
static __forceinline SurfRange surf_range(const uint8_t* s, const uint8_t* in, uint8_t* out) {
    const int32_t first = Sp(s, 0x18);
    return {Sp(s, 0x1a) - first, in + 32 * first, out + 32 * first};
}

// ==== light_off (0x45c270) =======================================================================================
static void __cdecl light_off(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    if (r.n > 0) do {
        copy_pos_uv(r.o, r.in);
        Up(r.o, 0x10) = ((uint32_t)Ba(L_ALPHA_B) << 24) | 0xffffffu;
        Up(r.o, 0x14) = 0xff000000u;
        Ia(L_COUNT) += 1;
        r.in += 0x20;
        r.o += 0x20;
    } while (--r.n != 0);
}
PORT_FN(0x0045c270, "light_off", light_off, fp_LightSurface)

// ==== light_pre_nofog_noenv (0x45c2e0) ===========================================================================
// types 0, 1, 2 alike: the stored colour with the model alpha, no fog; other types nothing
static void __cdecl light_pre_nofog_noenv(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    const uint32_t type = Bp(s, 0x10);
    if (type > 2) return;
    if (r.n > 0) do {
        copy_pos_uv(r.o, r.in);
        Up(r.o, 0x10) = Up(r.in, 0x10) | Ua(L_ALPHA_OR);
        Up(r.o, 0x14) = 0xff000000u;
        Ia(L_COUNT) += 1;
        r.in += 0x20;
        r.o += 0x20;
    } while (--r.n != 0);
}
PORT_FN(0x0045c2e0, "light_pre_nofog_noenv", light_pre_nofog_noenv, fp_LightSurface)

// ==== the pre-lit env-map variants: a panic each ===================================================================
static void __cdecl light_pre_nofog_someenv(uint8_t*, uint8_t*, uint8_t*) { LogPanic(P<const char>(0x004f3908)); }
static void fp_panic_only(Footprint&, uint8_t*, uint8_t*, uint8_t*) {}
PORT_FN(0x0045c400, "light_pre_nofog_someenv", light_pre_nofog_someenv, fp_panic_only)
static void __cdecl light_pre_nofog_env(uint8_t*, uint8_t*, uint8_t*) { LogPanic(P<const char>(0x004f3948)); }
PORT_FN(0x0045c410, "light_pre_nofog_env", light_pre_nofog_env, fp_panic_only)
static void __cdecl light_pre_fog_someenv(uint8_t*, uint8_t*, uint8_t*) { LogPanic(P<const char>(0x004f3984)); }
PORT_FN(0x0045c6f0, "light_pre_fog_someenv", light_pre_fog_someenv, fp_panic_only)
static void __cdecl light_pre_fog_env(uint8_t*, uint8_t*, uint8_t*) { LogPanic(P<const char>(0x004f39c4)); }
PORT_FN(0x0045c700, "light_pre_fog_env", light_pre_fog_env, fp_panic_only)

// ==== light_pre_fog_noenv (0x45c420) =============================================================================
static void __cdecl light_pre_fog_noenv(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    const uint32_t type = Bp(s, 0x10);
    if (type > 2) return;
    if (r.n <= 0) return;
    double slot = 0.0;
    do {
        copy_pos_uv(r.o, r.in);
        Up(r.o, 0x10) = Up(r.in, 0x10) | Ua(L_ALPHA_OR);
        Up(r.o, 0x14) = 0;
        slot = cam_d2(r.in);
        Up(r.o, 0x14) = fog_alpha(slot, slot);
        Ia(L_COUNT) += 1;
        r.in += 0x20;
        r.o += 0x20;
    } while (--r.n != 0);
    st(slot);
}
PORT_FN(0x0045c420, "light_pre_fog_noenv", light_pre_fog_noenv, fp_LightSurface)

// ==== light_nopre_nofog_noenv (0x45c710) =========================================================================
// types 0 and 1: the sun's diffuse (the specular left as it was); type 2 full bright
static void __cdecl light_nopre_nofog_noenv(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    const uint32_t type = Bp(s, 0x10);
    if (type > 2) return;
    if (r.n <= 0) return;
    if (type <= 1) {
        do {
            copy_pos_uv(r.o, r.in);
            Up(r.o, 0x10) = diffuse_of(magic_int((D(Fp(r.in, 0x14)) * Fa(L_MSUN + 8) + D(Fp(r.in, 0xc)) * Fa(L_MSUN)) +
                                                 D(Fp(r.in, 0x10)) * Fa(L_MSUN + 4)));
            Ia(L_COUNT) += 1;
            r.in += 0x20;
            r.o += 0x20;
        } while (--r.n != 0);
    } else {
        do {
            copy_pos_uv(r.o, r.in);
            Up(r.o, 0x10) = 0xffffffffu;
            Up(r.o, 0x14) = 0xff000000u;
            Ia(L_COUNT) += 1;
            r.in += 0x20;
            r.o += 0x20;
        } while (--r.n != 0);
    }
}
PORT_FN(0x0045c710, "light_nopre_nofog_noenv", light_nopre_nofog_noenv, fp_LightSurface)

// ==== light_nopre_nofog_someenv (0x45c8c0) =======================================================================
// the sun's diffuse and the specular (its table entry as the whole specular: alpha 0); type 2 white + specular
static __forceinline uint32_t spec_whole(const uint8_t* in) {
    const int32_t i = magic_int(((D(Fp(in, 0x10)) * Fa(L_HALF + 4) + D(Fp(in, 0x14)) * Fa(L_HALF + 8)) +
                                 D(Fp(in, 0xc)) * Fa(L_HALF)) * 255.0);
    return spec_of(i < 0 ? 0 : i);
}
static __forceinline uint32_t diffuse_yzx(const uint8_t* in) {
    return diffuse_of(magic_int((D(Fp(in, 0x10)) * Fa(L_MSUN + 4) + D(Fp(in, 0x14)) * Fa(L_MSUN + 8)) +
                                D(Fp(in, 0xc)) * Fa(L_MSUN)));
}
static void __cdecl light_nopre_nofog_someenv(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    const uint32_t type = Bp(s, 0x10);
    if (type > 2) return;
    if (r.n <= 0) return;
    do {
        copy_pos_uv(r.o, r.in);
        if (type <= 1) {
            Up(r.o, 0x10) = diffuse_yzx(r.in);
        } else {
            Up(r.o, 0x10) = 0xffffffffu;
            Up(r.o, 0x14) = 0xff000000u;
        }
        Up(r.o, 0x14) = spec_whole(r.in);
        Ia(L_COUNT) += 1;
        r.in += 0x20;
        r.o += 0x20;
    } while (--r.n != 0);
}
PORT_FN(0x0045c8c0, "light_nopre_nofog_someenv", light_nopre_nofog_someenv, fp_LightSurface)

// ==== light_nopre_nofog_env (0x45cb40) ===========================================================================
// type 0: the diffuse, and in sphere-map mode the UVs from the reflected eye ray through the physics library's vector
// calls; type 1 the same by another route (DotProduct, _VectorNormalize); type 2 white, APPLY_ENVMAP
static void __cdecl light_nopre_nofog_env(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    const uint32_t type = Bp(s, 0x10);
    if (type > 2) return;
    if (r.n <= 0) return;
    if (type == 0) {
        do {
            copy_pos_uv(r.o, r.in);
            Up(r.o, 0x10) = diffuse_of(magic_int((D(Fp(r.in, 0x14)) * Fa(L_MSUN + 8) + D(Fp(r.in, 0xc)) * Fa(L_MSUN)) +
                                                 D(Fp(r.in, 0x10)) * Fa(L_MSUN + 4)));
            if (Ia(L_ENV) == 2) {
                float t1[3], t2[3], t3[3];
                VectorSub(t1, P<void>(L_MCAM), r.in);
                const float sc = st(((D(Fp(r.in, 0xc)) * t1[0] + D(Fp(r.in, 0x10)) * t1[1]) + D(Fp(r.in, 0x14)) * t1[2]) * -2.0);
                VectorAddScaled_o(t2, P<void>(L_MCAM), r.in + 0xc, fbits(sc));
                VectorSub(t3, r.in, t2);
                MatrixMulPointInv(t3, t3, P<void>(L_MMAT));
                const double inv = 1.0 / VectorLength(t3);
                t3[0] = (float)(D(t3[0]) * inv);
                t3[1] = (float)(D(t3[1]) * inv);
                t3[2] = st(inv * t3[2]);
                Fp(r.o, 0x18) = (float)(D(t3[0]) * 0.5 + 0.5);
                Fp(r.o, 0x1c) = (float)(D(t3[1]) * -0.5 + 0.5);
            }
            Ia(L_COUNT) += 1;
            r.in += 0x20;
            r.o += 0x20;
        } while (--r.n != 0);
    } else if (type == 1) {
        do {
            copy_pos_uv(r.o, r.in);
            Up(r.o, 0x10) = diffuse_of(magic_int((D(Fp(r.in, 0x14)) * Fa(L_MSUN + 8) + D(Fp(r.in, 0x10)) * Fa(L_MSUN + 4)) +
                                                 D(Fp(r.in, 0xc)) * Fa(L_MSUN)));
            if (Ia(L_ENV) == 2) {
                float t4[3], t1[3], t3[3];
                VectorSub(t4, P<void>(L_MCAM), r.in);
                const float sc = st(DotProduct_o(t4, r.in + 0xc) * -2.0);
                VectorAddScaled_o(t1, P<void>(L_MCAM), r.in + 0xc, fbits(sc));
                VectorSub(t3, r.in, t1);
                MatrixMulPointInv(t3, t3, P<void>(L_MMAT));
                VectorNormalize(t3);
                Fp(r.o, 0x18) = (float)(D(t3[0]) * 0.5 + 0.5);
                Fp(r.o, 0x1c) = (float)(D(t3[1]) * -0.5 + 0.5);
            }
            Ia(L_COUNT) += 1;
            r.in += 0x20;
            r.o += 0x20;
        } while (--r.n != 0);
    } else {
        do {
            copy_pos_uv(r.o, r.in);
            Up(r.o, 0x10) = 0xffffffffu;
            Up(r.o, 0x14) = 0xff000000u;
            APPLY_ENVMAP_o(r.in, r.o);
            Ia(L_COUNT) += 1;
            r.in += 0x20;
            r.o += 0x20;
        } while (--r.n != 0);
    }
}
PORT_FN(0x0045cb40, "light_nopre_nofog_env", light_nopre_nofog_env, fp_LightSurface)

// ==== light_nopre_fog_noenv (0x45cea0) ===========================================================================
static void __cdecl light_nopre_fog_noenv(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    const uint32_t type = Bp(s, 0x10);
    if (type > 2) return;
    if (r.n <= 0) return;
    double slot = 0.0;
    do {
        copy_pos_uv(r.o, r.in);
        if (type <= 1)
            Up(r.o, 0x10) = diffuse_of(magic_int((D(Fp(r.in, 0xc)) * Fa(L_MSUN) + D(Fp(r.in, 0x14)) * Fa(L_MSUN + 8)) +
                                                 D(Fp(r.in, 0x10)) * Fa(L_MSUN + 4)));
        else
            Up(r.o, 0x10) = 0xffffffffu;
        Up(r.o, 0x14) = 0;
        slot = cam_d2(r.in);
        Up(r.o, 0x14) = fog_alpha(slot, slot);
        Ia(L_COUNT) += 1;
        r.in += 0x20;
        r.o += 0x20;
    } while (--r.n != 0);
    st(slot);
}
PORT_FN(0x0045cea0, "light_nopre_fog_noenv", light_nopre_fog_noenv, fp_LightSurface)

// ==== light_nopre_fog_someenv (0x45d1e0) =========================================================================
static void __cdecl light_nopre_fog_someenv(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    const uint32_t type = Bp(s, 0x10);
    if (type > 2) return;
    if (r.n <= 0) return;
    if (type <= 1) {
        double slot = 0.0;
        do {
            copy_pos_uv(r.o, r.in);
            Up(r.o, 0x10) = diffuse_yzx(r.in);
            const uint32_t spec = spec_whole(r.in);
            Up(r.o, 0x14) = spec;
            const double dy = D(Fa(L_MCAM + 4)) - Fp(r.in, 4), dz = D(Fa(L_MCAM + 8)) - Fp(r.in, 8),
                         dx = D(Fa(L_MCAM)) - Fp(r.in, 0);
            slot = (dy * dy + dz * dz) + dx * dx;
            Up(r.o, 0x14) = spec | fog_alpha(slot, slot);
            Ia(L_COUNT) += 1;
            r.in += 0x20;
            r.o += 0x20;
        } while (--r.n != 0);
        st(slot);
    } else {
        do {
            copy_pos_uv(r.o, r.in);
            Up(r.o, 0x10) = 0xffffffffu;
            Up(r.o, 0x14) = 0xff000000u;
            Up(r.o, 0x14) = spec_whole(r.in);
            APPLY_FOG_o(r.in, r.o);
            Ia(L_COUNT) += 1;
            r.in += 0x20;
            r.o += 0x20;
        } while (--r.n != 0);
    }
}
PORT_FN(0x0045d1e0, "light_nopre_fog_someenv", light_nopre_fog_someenv, fp_LightSurface)

// ==== light_nopre_fog_env (0x45d570) =============================================================================
// the fog distance stored as a float here (and in APPLY_FOG) before it is subtracted from the end
static void __cdecl light_nopre_fog_env(uint8_t* s, uint8_t* in, uint8_t* out) {
    SurfRange r = surf_range(s, in, out);
    const uint32_t type = Bp(s, 0x10);
    if (type > 2) return;
    if (r.n <= 0) return;
    do {
        copy_pos_uv(r.o, r.in);
        if (type <= 1)
            Up(r.o, 0x10) = diffuse_of(magic_int((D(Fp(r.in, 0x14)) * Fa(L_MSUN + 8) + D(Fp(r.in, 0xc)) * Fa(L_MSUN)) +
                                                 D(Fp(r.in, 0x10)) * Fa(L_MSUN + 4)));
        else
            Up(r.o, 0x10) = 0xffffffffu;
        Up(r.o, 0x14) = 0;
        const double d2 = cam_d2(r.in);
        const float d2f = st(d2);
        Up(r.o, 0x14) = fog_alpha(d2, D(d2f));
        APPLY_ENVMAP_o(r.in, r.o);
        Ia(L_COUNT) += 1;
        r.in += 0x20;
        r.o += 0x20;
    } while (--r.n != 0);
}
PORT_FN(0x0045d570, "light_nopre_fog_env", light_nopre_fog_env, fp_LightSurface)

// ==== DotProduct (0x45d8e0) / VectorAddScaled (0x45d900) =========================================================
static double __cdecl DotProduct(const P3* a, const P3* b) { return (D(b->y) * a->y + D(b->z) * a->z) + D(b->x) * a->x; }
static void fp_DotProduct(Footprint& f, const P3*, const P3*) { f.pure = true; }
PORT_FN(0x0045d8e0, "DotProduct", DotProduct, fp_DotProduct)

// out = b x s + a, x then y then z (so an out that is also a or b reads what was just written)
static void __cdecl VectorAddScaled(P3* out, const P3* a, const P3* b, float s) {
    out->x = (float)(D(b->x) * s + a->x);
    out->y = (float)(D(b->y) * s + a->y);
    out->z = (float)(D(b->z) * s + a->z);
}
static void fp_VectorAddScaled(Footprint& f, P3* out, const P3*, const P3*, float) { f.pure = true; f.add(out, 12, "out"); }
PORT_FN(0x0045d900, "VectorAddScaled", VectorAddScaled, fp_VectorAddScaled)

// ==== APPLY_ENVMAP (0x45d940) / APPLY_FOG (0x45da80) =============================================================
static void __cdecl APPLY_ENVMAP(const uint8_t* in, uint8_t* o) {
    if (Ia(L_ENV) != 2) return;
    const double t = (((D(Fa(L_MCAM)) - Fp(in, 0)) * Fp(in, 0xc) + (D(Fa(L_MCAM + 8)) - Fp(in, 8)) * Fp(in, 0x14)) +
                      (D(Fa(L_MCAM + 4)) - Fp(in, 4)) * Fp(in, 0x10)) * -2.0;
    const float rx = (float)(D(Fp(in, 0)) - (D(Fp(in, 0xc)) * t + Fa(L_MCAM)));
    const float ry = (float)(D(Fp(in, 4)) - (D(Fp(in, 0x10)) * t + Fa(L_MCAM + 4)));
    const float rz = (float)(D(Fp(in, 8)) - (t * Fp(in, 0x14) + Fa(L_MCAM + 8)));
    const float* m = P<float>(L_MMAT);
    const double Y = (D(ry) * m[4] + D(rz) * m[5]) + D(rx) * m[3];
    const double Z = (D(ry) * m[7] + D(rz) * m[8]) + D(rx) * m[6];
    const float xf = (float)((D(ry) * m[1] + D(rz) * m[2]) + D(rx) * m[0]);
    const double inv = 1.0 / x87_sqrt((Z * Z + Y * Y) + D(xf) * xf);
    const double xn = D(xf) * inv;
    st(xn);
    Fp(o, 0x18) = (float)(xn * 0.5 + 0.5);
    Fp(o, 0x1c) = (float)((Y * inv) * -0.5 + 0.5);
}
static void fp_APPLY_ENVMAP(Footprint& f, const uint8_t*, uint8_t* o) { f.add(o + 0x18, 8, "the vertex's UVs"); }
PORT_FN(0x0045d940, "APPLY_ENVMAP", APPLY_ENVMAP, fp_APPLY_ENVMAP)

static void __cdecl APPLY_FOG(const uint8_t* in, uint8_t* o) {
    const double d2 = cam_d2(in);
    const float d2f = st(d2);
    Up(o, 0x14) |= fog_alpha(d2, D(d2f));
}
static void fp_APPLY_FOG(Footprint& f, const uint8_t*, uint8_t* o) { f.add(o + 0x14, 4, "the vertex's specular"); }
PORT_FN(0x0045da80, "APPLY_FOG", APPLY_FOG, fp_APPLY_FOG)

// ================================================================================================================
// model.obj
// ================================================================================================================
typedef void*(__fastcall* VDtor_t)(void*, Edx, unsigned);   // a pool's scalar deleting destructor (vtable slot 0)
static __forceinline void pool_delete(void* p) { ((VDtor_t)(*(void***)p)[0])(p, 0, 1); }

// ==== mr_model_begin (0x4556f0) ==================================================================================
static void __cdecl mr_model_begin() {
    uint8_t* p = (uint8_t*)MemAlloc(0x20);
    if (p) {
        PoolBase_ctor(p, 0, P<const char>(0x004f21a8), (int)m1_operand(0x00455707), 0x2c);   // 'surf info' (M1: 32768)
        Up(p, 0) = 0x004dcb34;
        Ua(S_SURF_POOL) = (uint32_t)(uintptr_t)p;
    } else {
        Ua(S_SURF_POOL) = 0;
    }
    p = (uint8_t*)MemAlloc(0x20);
    if (p) {
        PoolBase_ctor(p, 0, P<const char>(0x004f21bc), (int)m1_operand(0x00455742), 0x30);   // 'models' (M1: 16384)
        Up(p, 0) = 0x004dcb30;
        Ua(S_MODEL_POOL) = (uint32_t)(uintptr_t)p;
    } else {
        Ua(S_MODEL_POOL) = 0;
    }
    Ua(S_MODELS) = 0;
    // LIFT: room for 32,768 lit vertices, not 1,500 (port.h, VP_LIT_BUF_BYTES; docs/FIXES.md "Limits lifted"). A vrmod
    // race.exe's own `push` (vertexbuffer.py: up to the same 32,768) is accepted by the stock check and not read.
    if (VP_FIX) GX_FIX_FIRED();
    Ua(S_LIT_BUF) = (uint32_t)(uintptr_t)MemAlloc((int)VP_LIT_BUF_BYTES);
    vp_g_lit_buf_bytes = VP_LIT_BUF_BYTES;
    Ua(S_CLIP_NEAR) = 0;
    Ba(S_HAS_ALPHA) = 0;
    Ba(S_ENVMAP) = 0;
    Ba(S_ENVPASS) = 0;
    memcpy(P<void>(S_IDENT_FRAME), k_identity_frame, 0x30);
    Ua(S_CLIP_FAR) = 0x447a0000;                                     // 1000
    begin_deferred_o();
}
static void fp_mr_model_begin(Footprint& f) { f.replay_only = "allocates the pools and the lit-vertex buffer"; }
PORT_FN(0x004556f0, "mr_model_begin", mr_model_begin, fp_mr_model_begin)

// ==== mr_model_end (0x455800) ====================================================================================
static void __cdecl mr_model_end() {
    end_deferred_o();
    OpDelete(*P<void*>(S_LIT_BUF));
    Ua(S_LIT_BUF) = 0;
    if (void* p = *P<void*>(S_SURF_POOL)) pool_delete(p);
    Ua(S_SURF_POOL) = 0;
    if (void* p = *P<void*>(S_MODEL_POOL)) pool_delete(p);
    Ua(S_MODEL_POOL) = 0;
    Ua(S_MODELS) = 0;
}
static void fp_mr_model_end(Footprint& f) { f.replay_only = "frees the pools and the lit-vertex buffer"; }
PORT_FN(0x00455800, "mr_model_end", mr_model_end, fp_mr_model_end)

// ==== mr_model_first (0x455860) / mr_model_last (0x455880) =======================================================
static void __cdecl mr_model_first() { Ia(S_ENV_TEX) = gxGetTexture(P<const char>(0x004f21d0), 0); }
static void fp_mr_model_first(Footprint& f) { f.replay_only = "gxGetTexture loads the env-map texture"; }
PORT_FN(0x00455860, "mr_model_first", mr_model_first, fp_mr_model_first)

static void __cdecl mr_model_last() {
    gxForgetTexture(Ia(S_ENV_TEX));
    Ia(S_ENV_TEX) = -1;
}
static void fp_mr_model_last(Footprint& f) { f.replay_only = "gxForgetTexture can free the env-map texture"; }
PORT_FN(0x00455880, "mr_model_last", mr_model_last, fp_mr_model_last)

// ==== mrModelLoad (0x4558c0) / mrModelLoadRemap (0x455900) / mrModelUnload (0x455950) ============================
// FIX: a model that isn't found (mrModelInfoGet logs it and returns NULL) was read through NULL here: it becomes a model
// with an empty info (no vertices, surfaces or triangles), which draws nothing; mrModelUnload doesn't hand that info back
// to the resource layer.
static uint8_t g_empty_info[0x28];
static __forceinline uint8_t* info_or_empty(uint8_t* info) {
    if (VP_FIX && !info) {
        GX_FIX_FIRED();
        return g_empty_info;
    }
    return info;
}
static int __cdecl mrModelLoad(const char* name) {
    uint8_t* info = info_or_empty((uint8_t*)mrModelInfoGet_o(name));
    const int m = mrModelCreate_o(name, Ip(info, 0x10));
    mrModelBuild_o(m, info, 7);
    return m;
}
static void fp_load(Footprint& f) { f.replay_only = "loads a model: resources, pools, textures"; }
static void fp_mrModelLoad(Footprint& f, const char*) { fp_load(f); }
PORT_FN(0x004558c0, "mrModelLoad", mrModelLoad, fp_mrModelLoad)

static int __cdecl mrModelLoadRemap(const char* name, void* remap, int n) {
    uint8_t* info = info_or_empty((uint8_t*)mrModelInfoGet_o(name));
    const int m = mrModelCreate_o(name, Ip(info, 0x10));
    mrModelRemapTextures_o(m, remap, n);
    mrModelBuild_o(m, info, 7);
    return m;
}
static void fp_mrModelLoadRemap(Footprint& f, const char*, void*, int) { fp_load(f); }
PORT_FN(0x00455900, "mrModelLoadRemap", mrModelLoadRemap, fp_mrModelLoadRemap)

static void __cdecl mrModelUnload(int m) {
    uint8_t* info = Pp(mip(m), 0x10);
    if (info && !(VP_FIX && info == g_empty_info)) mrModelInfoForget_o(info);
    mrModelDestroy_o(m);
}
static void fp_mrModelUnload(Footprint& f, int) { f.replay_only = "frees a model"; }
PORT_FN(0x00455950, "mrModelUnload", mrModelUnload, fp_mrModelUnload)

// ==== mrModelRemapTextures (0x455980) ============================================================================
static void __cdecl mrModelRemapTextures(int m, void* remap, int n) {
    uint8_t* mi = mip(m);
    Pp(mi, 0x18) = (uint8_t*)remap;
    Ip(mi, 0x1c) = n;
    if (uint8_t* info = Pp(mi, 0x10)) direct_model_build_o(m, info, Bp(mi, 0x15), 2);
}
static void fp_mrModelRemapTextures(Footprint& f, int, void*, int) { f.replay_only = "rebuilds the surfaces: pool, textures"; }
PORT_FN(0x00455980, "mrModelRemapTextures", mrModelRemapTextures, fp_mrModelRemapTextures)

// ==== the copies: mrModelCopyTransform(frame) (0x4559c0), mrModelCopyRemap (0x455a10), mrModelCopyTransform(matrix)
// (0x455aa0), mrModelCopy (0x455b10), mrModelDestroyCopy (0x455b30) ============================================
static int __cdecl mrModelCopyTransformFrame(int m, const void* fr) {
    void* info;
    const int m2 = model_copy_transform_o(m, fr, &info);
    mrModelBuild_o(m2, info, 7);
    Bp(mip(m2), 0x14) = 1;
    return m2;
}
static void fp_copy(Footprint& f) { f.replay_only = "copies a model: MemAlloc, pools, textures"; }
static void fp_mrModelCopyTransformFrame(Footprint& f, int, const void*) { fp_copy(f); }
PORT_FN(0x004559c0, "mrModelCopyTransform(frame)", mrModelCopyTransformFrame, fp_mrModelCopyTransformFrame)

static int __cdecl mrModelCopyRemap(int m, void* remap, int n) {
    void* info;
    uint32_t fr[12];
    memcpy(fr, k_identity_frame, sizeof fr);
    const int m2 = model_copy_transform_o(m, fr, &info);
    mrModelRemapTextures_o(m2, remap, n);
    mrModelBuild_o(m2, info, 7);
    Bp(mip(m2), 0x14) = 1;
    return m2;
}
static void fp_mrModelCopyRemap(Footprint& f, int, void*, int) { fp_copy(f); }
PORT_FN(0x00455a10, "mrModelCopyRemap", mrModelCopyRemap, fp_mrModelCopyRemap)

static int __cdecl mrModelCopyTransformMatrix(int m, const void* mat) {
    uint32_t fr[12];
    memcpy(fr, k_identity_frame, sizeof fr);
    memcpy(fr, mat, 36);                                             // rep movsd: the rotation; the position stays 0
    return mrModelCopyTransform_o(m, fr);
}
static void fp_mrModelCopyTransformMatrix(Footprint& f, int, const void*) { fp_copy(f); }
PORT_FN(0x00455aa0, "mrModelCopyTransform(matrix)", mrModelCopyTransformMatrix, fp_mrModelCopyTransformMatrix)

static int __cdecl mrModelCopy(int m) { return mrModelCopyTransform_o(m, P<void>(S_IDENT_FRAME)); }
static void fp_mrModelCopy(Footprint& f, int) { fp_copy(f); }
PORT_FN(0x00455b10, "mrModelCopy", mrModelCopy, fp_mrModelCopy)

static void __cdecl mrModelDestroyCopy(int m) { mrModelDestroy_o(m); }
static void fp_mrModelDestroyCopy(Footprint& f, int) { f.replay_only = "frees a model"; }
PORT_FN(0x00455b30, "mrModelDestroyCopy", mrModelDestroyCopy, fp_mrModelDestroyCopy)

// ==== mrModelCreate (0x455b40) ===================================================================================
// A 16-character name is left unterminated in the model_info (strncpy 16, the original's bytes): nothing reads it but
// model_copy_transform, which hands it on terminated (name16)
static int __cdecl mrModelCreate(const char* name, int ntris) {
    uint8_t* mi = (uint8_t*)PoolBase_alloc_v10(*P<void*>(S_MODEL_POOL), 0);
    game_strncpy((char*)mi, name, 0x10);
    Ip(mi, 0x10) = 0;
    Bp(mi, 0x15) = 1;
    Bp(mi, 0x14) = 0;
    Ip(mi, 0x18) = 0;
    Ip(mi, 0x1c) = 0;
    const int32_t nidx = (int32_t)((uint32_t)ntris * 3u);
    Ip(mi, 0x20) = nidx;
    Pp(mi, 0x24) = (uint8_t*)MemAlloc((int)((uint32_t)nidx * 2u));
    Ip(mi, 0x28) = 0;
    if (Ua(S_MODELS) == 0) mr_model_first_o();
    Up(mi, 0x2c) = Ua(S_MODELS);
    Ua(S_MODELS) = (uint32_t)(uintptr_t)mi;
    return (int)(uintptr_t)convert_to_mrmodel_o(mi);
}
static void fp_mrModelCreate(Footprint& f, const char*, int) { f.replay_only = "allocates a model: pool, MemAlloc, texture"; }
PORT_FN(0x00455b40, "mrModelCreate", mrModelCreate, fp_mrModelCreate)

// ==== mrModelDestroy (0x455bc0) ==================================================================================
static void __cdecl mrModelDestroy(int m) {
    uint8_t* mi = mip(m);
    OpDelete(Pp(mi, 0x24));
    Ip(mi, 0x24) = 0;
    for (uint8_t* s = Pp(mi, 0x28); s;) {
        uint8_t* next = Pp(s, 0x28);
        gxForgetTexture(Ip(s, 0x14));
        Ip(s, 0x14) = -1;
        PoolBase_free_v10(*P<void*>(S_SURF_POOL), 0, s);
        s = next;
    }
    uint8_t* prev = 0;
    uint8_t* cur = *P<uint8_t*>(S_MODELS);
    if (cur) {
        while (mi != cur) {
            prev = cur;
            cur = Pp(cur, 0x2c);
            if (!cur) break;
        }
    }
    if (cur) {
        uint8_t* next = Pp(cur, 0x2c);
        if (prev) Pp(prev, 0x2c) = next;
        else Ua(S_MODELS) = (uint32_t)(uintptr_t)next;
    } else {
        LogPanic(P<const char>(0x004f21e8));
    }
    if (Bp(mi, 0x14)) MemFree(Pp(mi, 0x10));
    PoolBase_free_v10(*P<void*>(S_MODEL_POOL), 0, mi);
    if (Ua(S_MODELS) == 0) mr_model_last_o();
}
static void fp_mrModelDestroy(Footprint& f, int) { f.replay_only = "frees a model: pools, MemFree, textures"; }
PORT_FN(0x00455bc0, "mrModelDestroy", mrModelDestroy, fp_mrModelDestroy)

// ==== mrModelBuild (0x455c80) / mrModelBuildLit (0x455ca0) / direct_model_build (0x455ed0) =======================
// flags 1: the index buffer (each surface's triangles, relative to its first vertex); 2: a model_surface_info per
// surface (reused in order, new ones from the pool, extra ones freed) with its texture (set_texture_id), index range
// and vertex range in the lit buffer
static void fp_build(Footprint& f, int m, const uint8_t* info, int flags) {
    if (flags & 2) { f.replay_only = "builds the surfaces: pool, textures"; return; }
    uint8_t* mi = (uint8_t*)(uintptr_t)m;                            // convert_to_mip: the handle is the model_info
    f.add(mi + 0x10, 8, "the model's info and lit flag");
    if (!(flags & 1) || !info) return;
    const int32_t ns = Ip(info, 8);
    const uint8_t* s = Pp(info, 0xc);
    for (int32_t k = 0; k < ns && k < 4096; k++, s += 0x20) {
        const int32_t first = Sp(s, 0x1c), end = Sp(s, 0x1e);
        if (end > first) f.add(Pp(mi, 0x24) + 6 * first, 6u * (uint32_t)(end - first), "the model's indices");
    }
}
static void __cdecl mrModelBuild(int m, void* info, int flags) { direct_model_build_o(m, info, 0, flags); }
static void fp_mrModelBuild(Footprint& f, int m, void* info, int flags) { fp_build(f, m, (const uint8_t*)info, flags); }
PORT_FN(0x00455c80, "mrModelBuild", mrModelBuild, fp_mrModelBuild)
static void __cdecl mrModelBuildLit(int m, void* info, int flags) { direct_model_build_o(m, info, 1, flags); }
PORT_FN(0x00455ca0, "mrModelBuildLit", mrModelBuildLit, fp_mrModelBuild)

static void __cdecl direct_model_build(int m, uint8_t* info, uint8_t lit, int flags) {
    uint8_t* mi = mip(m);
    Pp(mi, 0x10) = info;
    Bp(mi, 0x15) = lit;
    if (flags & 1) {
        const uint8_t* surf = Pp(info, 0xc);
        int32_t ns = Ip(info, 8);
        if (ns > 0) do {
            int32_t i = Sp(surf, 0x1c);
            if (Sp(surf, 0x1e) > i) do {
                for (int k = 0; k < 3; k++) {
                    const uint16_t v = (uint16_t)(Wp(Pp(info, 0x14), 8u * (uint32_t)i + 2 * k) - Wp(surf, 0x18));
                    Wp(Pp(mi, 0x24), 6u * (uint32_t)i + 2 * k) = v;
                }
                i++;
            } while (Sp(surf, 0x1e) > i);
            surf += 0x20;
        } while (--ns != 0);
    }
    if (!(flags & 2)) return;
    const uint8_t* s = Pp(info, 0xc);
    uint8_t* prev = 0;
    int32_t n = Ip(info, 8);
    uint8_t* cur = Pp(mi, 0x28);
    if (n > 0) do {
        if (!cur) {
            uint8_t* ns2 = (uint8_t*)PoolBase_alloc_v10(*P<void*>(S_SURF_POOL), 0);
            Bp(ns2, 0) = 0;
            Ip(ns2, 0x10) = 0;
            Ip(ns2, 0x14) = -1;
            set_texture_id_o(m, (const char*)s, ns2);
            Pp(ns2, 0x28) = 0;
            if (prev) Pp(prev, 0x28) = ns2;
            else Pp(mi, 0x28) = ns2;
            prev = ns2;
            cur = 0;
        } else {
            char sb[17], cb[17];
            const int same = game_stricmp(name16((const char*)s, sb), name16((const char*)cur, cb));
            set_texture_id_o(m, same == 0 ? (const char*)cur : (const char*)s, cur);
            prev = cur;
            cur = Pp(cur, 0x28);
        }
        Pp(prev, 0x18) = Pp(mi, 0x24) + 6 * (int32_t)Sp(s, 0x1c);
        Ip(prev, 0x1c) = ((int32_t)Sp(s, 0x1e) - Sp(s, 0x1c)) * 3;
        Pp(prev, 0x20) = *P<uint8_t*>(S_LIT_BUF) + 32 * (int32_t)Sp(s, 0x18);
        Ip(prev, 0x24) = (int32_t)Sp(s, 0x1a) - Sp(s, 0x18);
        s += 0x20;
    } while (--n != 0);
    while (cur) {                                                    // the surfaces the model no longer has
        gxForgetTexture(Ip(cur, 0x14));
        Ip(cur, 0x14) = -1;
        uint8_t* next = Pp(cur, 0x28);
        if (prev) Pp(prev, 0x28) = next;
        else Pp(mi, 0x28) = next;
        PoolBase_free_v10(*P<void*>(S_SURF_POOL), 0, cur);
        cur = next;
    }
    for (uint8_t* volatile w = Pp(mi, 0x28); w;) w = Pp(w, 0x28);   // (a walk to the list's end, its result unused)
}
static void fp_direct_model_build(Footprint& f, int m, uint8_t* info, uint8_t, int flags) { fp_build(f, m, info, flags); }
PORT_FN(0x00455ed0, "direct_model_build", direct_model_build, fp_direct_model_build)

// ==== mrModelEnterDeferredMode (0x455cc0) / mrModelLeaveDeferredMode (0x455cd0) / mrModelEnvMap (0x455cf0) =======
static void __cdecl mrModelEnterDeferredMode() {
    Ba(S_DEFERRED) = 1;
    begin_deferred_surfs_o();
}
static void fp_mrModelEnterDeferredMode(Footprint& f) {
    f.add(P<uint8_t>(S_DEFERRED), 1, "deferred mode");
    f.add(P<uint8_t>(S_DEFER_ON), 1, "deferring");
}
PORT_FN(0x00455cc0, "mrModelEnterDeferredMode", mrModelEnterDeferredMode, fp_mrModelEnterDeferredMode)

static void __cdecl mrModelLeaveDeferredMode() {
    Ba(S_DEFERRED) = 0;
    end_deferred_surfs_o();
}
static void fp_draws(Footprint& f) { f.replay_only = "draws: dxDPBegin -> TextureSelect can load a texture"; }
PORT_FN(0x00455cd0, "mrModelLeaveDeferredMode", mrModelLeaveDeferredMode, fp_draws)

static void __cdecl mrModelEnvMap(uint8_t on) { Ba(S_ENVMAP) = on; }
static void fp_mrModelEnvMap(Footprint& f, uint8_t) { f.add(P<uint8_t>(S_ENVMAP), 1, "env map on"); }
PORT_FN(0x00455cf0, "mrModelEnvMap", mrModelEnvMap, fp_mrModelEnvMap)

// ==== mrModelDraw(m, frame) (0x455d00) ===========================================================================
// culled if its origin is behind the camera, nearer than the near clip or beyond the far one; with env mapping on,
// a second additive pass with the env texture on (at most) the first 10 surfaces, at alpha 1 - (d - near) / 20
static void __cdecl mrModelDrawFrame(int m, const uint8_t* fr) {
    const uint8_t* pos = fr + 0x24;
    if (!mrPointCanSee_o(pos)) return;
    const double d = mrCameraDistance_o(pos);
    const float df = st(d);
    if (!(d >= D(Fa(S_CLIP_NEAR)))) return;                          // fcom; test ah, 1
    if (D(df) >= D(Fa(S_CLIP_FAR))) return;                          // fcomp; test ah, 1; je
    mr_model_set_frame_o((void*)fr);
    mrLightPlaceModel_o((void*)fr);
    if (!Ba(S_ENVMAP)) {
        direct_model_draw_o(m);
        return;
    }
    mrLightEnv_o(1);
    direct_model_draw_o(m);
    uint8_t* mi = mip(m);
    uint32_t saved[10];
    {
        uint8_t* s = Pp(mi, 0x28);
        uint32_t* p = saved;
        while (s && p < saved + 10) {
            *p++ = Up(s, 0x14);
            Up(s, 0x14) = Ua(S_ENV_TEX);
            s = Pp(s, 0x28);
        }
    }
    int ok = 1;
    Ba(S_ENVPASS) = 1;
    mrLightEnv_o(2);
    if (!(D(df) >= D(Fa(S_CLIP_NEAR))) || (int32_t)fbits(df) > 0x41a00000) ok = 0;
    model_ASSERT_MSG(ok, P<const char>(0x004f2204), D(Fa(S_CLIP_NEAR)), D(df), 20.0);
    const float alpha = st((D(df) - Fa(S_CLIP_NEAR)) * D(FB(0xbd4ccccd)) + 1.0);
    mrModelSetAlpha_o(fbits(alpha));
    mrSetBlendMode_o(2);
    direct_model_draw_o(m);
    mrModelClearAlpha_o();
    mrSetBlendMode_o(1);
    mrLightEnv_o(0);
    Ba(S_ENVPASS) = 0;
    {
        uint8_t* s = Pp(mi, 0x28);
        const uint32_t* p = saved;
        while (s && p < saved + 10) {
            Up(s, 0x14) = *p++;
            s = Pp(s, 0x28);
        }
    }
}
static void fp_mrModelDrawFrame(Footprint& f, int, const uint8_t*) { fp_draws(f); }
PORT_FN(0x00455d00, "mrModelDraw(m,frame)", mrModelDrawFrame, fp_mrModelDrawFrame)

static void __cdecl mrModelDraw1(int m) { direct_model_draw_o(m); }
static void fp_mrModelDraw1(Footprint& f, int) { fp_draws(f); }
PORT_FN(0x00455ec0, "mrModelDraw(m)", mrModelDraw1, fp_mrModelDraw1)

// ==== set_texture_id (0x4560d0) ==================================================================================
// the surface's texture from its name, through the model's remap table (new name and value; the texture handed
// back through the entry's pointer); nothing if the name and value are what it has. The 16-byte names are read through
// name16 (FIX: 16-character names).
static void __cdecl set_texture_id(int m, const char* name, uint8_t* si) {
    int32_t k = 0, value = 0;
    uint8_t* entry = 0;
    uint8_t* mi = mip(m);
    if (uint8_t* remap = Pp(mi, 0x18)) {
        entry = remap;
        if (Ip(mi, 0x1c) > k) {
            do {
                char nb[17], eb[17];
                if (game_stricmp(name16(name, nb), name16((const char*)entry, eb)) == 0) break;
                entry += 0x28;
                k++;
            } while (Ip(mi, 0x1c) > k);
            if (Ip(mi, 0x1c) > k) {
                value = Ip(entry, 0x20);
                name = (const char*)entry + 0x10;
            } else {
                entry = 0;
            }
        } else {
            entry = 0;
        }
    }
    char sb[17], nb[17];
    if (game_stricmp(name16((const char*)si, sb), name16(name, nb)) == 0 && Ip(si, 0x10) == value) return;
    game_strncpy((char*)si, name, 0x10);
    Ip(si, 0x10) = value;
    const int t = gxGetTexture(name16(name, nb), value);
    if (Ip(si, 0x14) != -1) gxForgetTexture(Ip(si, 0x14));
    Ip(si, 0x14) = t;
    if (entry && Pp(entry, 0x24)) Ip(Pp(entry, 0x24), 0) = t;
}
static void fp_set_texture_id(Footprint& f, int, const char*, uint8_t*) { f.replay_only = "gxGetTexture / gxForgetTexture load and free textures"; }
PORT_FN(0x004560d0, "set_texture_id", set_texture_id, fp_set_texture_id)

// ==== direct_model_draw (0x456190) ===============================================================================
// deferred: every surface to the buckets (a do-while: a model with no surfaces still queues one, through its NULL
// surface info -- FIX: now it queues nothing); else each surface (in the env pass only type 1) lit and drawn
static void __cdecl direct_model_draw(int m) {
    uint8_t* mi = mip(m);
    uint8_t* info = Pp(mi, 0x10);
    uint8_t* si = Pp(mi, 0x28);
    uint8_t* surf = Pp(info, 0xc);
    int32_t n = Ip(info, 8);
    if (Ba(S_DEFERRED)) {
        if (VP_FIX && n <= 0) {                                       // FIX: a model with no surfaces queued one anyway, through
            GX_FIX_FIRED();                                           // its NULL surface info (a crash): it draws nothing
            return;
        }
        do {
            n--;
            add_deferred_surf_o(info, surf, si);
            surf += 0x20;
            si = Pp(si, 0x28);
        } while (n > 0);
        return;
    }
    dxStateFlush_v10();
    if (n == 0) return;
    const bool env_pass = Ba(S_ENVMAP) && Ba(S_ENVPASS);
    do {
        if (!env_pass || Bp(surf, 0x10) == 1) {
            dxDPBegin_v10(Ip(si, 0x14));
            uint8_t* verts = Pp(info, 4);
            const LightFn_t light = Bp(mi, 0x15) ? LightSurfacePreLit_v10 : LightSurfaceLit_o;
            light(surf, verts, *P<uint8_t*>(S_LIT_BUF));
            dxDPDraw_v10(Pp(si, 0x20), Ip(si, 0x24), (const uint16_t*)Pp(si, 0x18), Ip(si, 0x1c));
            dxDPEnd_v10();
        }
        surf += 0x20;
        n--;
        si = Pp(si, 0x28);
    } while (n > 0);
}
static void fp_direct_model_draw(Footprint& f, int) { fp_draws(f); }
PORT_FN(0x00456190, "direct_model_draw", direct_model_draw, fp_direct_model_draw)

// ==== mrModelSetFrame (0x4562d0) =================================================================================
static void __cdecl mrModelSetFrame(const void* fr) {
    mr_model_set_frame_o((void*)fr);
    mrLightPlaceModel_o((void*)fr);
}
static void fp_mrModelSetFrame(Footprint& f, const void*) { fp_dx(f); fp_place_model(f); }
PORT_FN(0x004562d0, "mrModelSetFrame", mrModelSetFrame, fp_mrModelSetFrame)

// ==== mrModelInfoGet (0x4562f0) / mrModelInfoForget (0x456460) / mrModelInfoSave (0x456470) ======================
// a 'MINF' resource; on its first load the pointers are made from the counts, and a version-0 file has its
// triangles' 4th word, its surfaces' type 2 (-> 1) and words 0x14 / 0x16 cleared
static void* __cdecl mrModelInfoGet(const char* name) {
    uint8_t fresh;
    uint32_t version;
    int32_t size;
    uint8_t* v = (uint8_t*)ResourceGet(name, 0x4d494e46, &version, &size, 0, &fresh);
    if (!v) {
        if (VP_FIX) {                                                 // FIX: a missing model was a panic (the game ends); every
            LogReport(P<const char>(0x004f227c), name);               // direct caller copes with NULL, and mrModelLoad /
            GX_FIX_FIRED();                                           // LoadRemap make an empty model instead: logged, carry on
            return 0;
        }
        LogPanic(P<const char>(0x004f227c), name);
        return 0;
    }
    if (!fresh) return v;
    if (Ua(S_MINF_V0) != version && Ua(S_MINF_V1) != version)
        LogPanic(P<const char>(0x004f2230), version, Ua(S_MINF_V1));
    uint8_t* p = v + 0x28;
    if (Ip(v, 0) > 0) { Pp(v, 4) = p; p += 32u * (uint32_t)Ip(v, 0); } else Ip(v, 4) = 0;
    if (Ip(v, 8) > 0) { Pp(v, 0xc) = p; p += 32u * (uint32_t)Ip(v, 8); } else Ip(v, 0xc) = 0;
    if (Ip(v, 0x10) > 0) { Pp(v, 0x14) = p; p += 8u * (uint32_t)Ip(v, 0x10); } else Ip(v, 0x14) = 0;
    if (Ip(v, 0x18) > 0) { Pp(v, 0x1c) = p; p += 16u * (uint32_t)Ip(v, 0x18); } else Ip(v, 0x1c) = 0;
    Pp(v, 0x24) = p;
    if (!(Ip(v, 0x20) > 0)) Ip(v, 0x24) = 0;
    if (Ua(S_MINF_V0) != version) return v;
    LogReport(P<const char>(0x004f2258), name);
    for (int32_t i = 0; Ip(v, 0x10) > i; i++) Wp(Pp(v, 0x14), 8u * (uint32_t)i + 6) = 0;
    for (int32_t i = 0; Ip(v, 8) > i; i++) {
        uint8_t* t = Pp(v, 0xc) + 32u * (uint32_t)i + 0x11;
        if (*t == 2) *t = 1;
        Wp(Pp(v, 0xc), 32u * (uint32_t)i + 0x14) = 0;
        Wp(Pp(v, 0xc), 32u * (uint32_t)i + 0x16) = 0;
    }
    return v;
}
static void fp_mrModelInfoGet(Footprint& f, const char*) { f.replay_only = "ResourceGet loads a resource"; }
PORT_FN(0x004562f0, "mrModelInfoGet", mrModelInfoGet, fp_mrModelInfoGet)

static void __cdecl mrModelInfoForget(void* info) { ResourceForget(info); }
static void fp_mrModelInfoForget(Footprint& f, void*) { f.replay_only = "ResourceForget can free a resource"; }
PORT_FN(0x00456460, "mrModelInfoForget", mrModelInfoForget, fp_mrModelInfoForget)

static uint8_t __cdecl mrModelInfoSave(const uint8_t* info, const char* name) {
    int h = FileCreate(name);
    if (!h) return 0;
    ResourceWrite(h, 0x4d494e46, Ua(S_MINF_VERSION));
    FileWrite(h, info, 0x28);
    if (Ip(info, 0)) FileWrite(h, Pp(info, 4), (int)((uint32_t)Ip(info, 0) << 5));
    if (Ip(info, 8)) FileWrite(h, Pp(info, 0xc), (int)((uint32_t)Ip(info, 8) << 5));
    if (Ip(info, 0x10)) FileWrite(h, Pp(info, 0x14), (int)((uint32_t)Ip(info, 0x10) << 3));
    if (Ip(info, 0x18)) FileWrite(h, Pp(info, 0x1c), (int)((uint32_t)Ip(info, 0x18) << 4));
    if (Ip(info, 0x20)) FileWrite(h, Pp(info, 0x24), (int)((uint32_t)Ip(info, 0x20) << 2));
    FileClose(&h);
    return 1;
}
static void fp_mrModelInfoSave(Footprint& f, const uint8_t*, const char*) { f.replay_only = "writes a file"; }
PORT_FN(0x00456470, "mrModelInfoSave", mrModelInfoSave, fp_mrModelInfoSave)

// ==== mrModelPick (0x456560) =====================================================================================
// the nearest front-facing triangle whose projection holds the screen point (x, y), or -1
static __forceinline int32_t sign_of(float v) {
    const uint32_t b = fbits(v);
    if ((int32_t)b > 0) return 1;
    if (b > 0x80000000u) return -1;
    return 0;
}
static __forceinline void tri_verts(const uint8_t* info, int32_t t, float* p0, float* p1, float* p2) {
    const uint8_t* tri = Pp(info, 0x14) + 8u * (uint32_t)t;
    cp12(p0, Pp(info, 4) + 32 * (int32_t)Sp(tri, 0));
    cp12(p1, Pp(info, 4) + 32 * (int32_t)Sp(tri, 2));
    cp12(p2, Pp(info, 4) + 32 * (int32_t)Sp(tri, 4));
}
static int __cdecl mrModelPick(uint8_t* info, const void* fr, int x, int y) {
    int32_t result = -1;
    float best = FB(0x47c35000);                                     // 100000
    int32_t i = 0;
    if (!(Ip(info, 0x10) > 0)) return result;
    do {
        float p0[3], p1[3], p2[3], s0[3], s1[3], s2[3];
        tri_verts(info, i, p0, p1, p2);
        if (mrProjectModelPoint_o(p0, s0, fr, 0) && mrProjectModelPoint_o(p1, s1, fr, 0) &&
            mrProjectModelPoint_o(p2, s2, fr, 0)) {
            const float e1x = (float)(D(s1[0]) - s0[0]), e1y = (float)(D(s1[1]) - s0[1]), e1z = (float)(D(s1[2]) - s0[2]);
            const float e2x = (float)(D(s2[0]) - s0[0]), e2y = (float)(D(s2[1]) - s0[1]), e2z = (float)(D(s2[2]) - s0[2]);
            const float nx = (float)(D(e2z) * e1y - D(e2y) * e1z);
            const float ny = (float)(D(e2x) * e1z - D(e2z) * e1x);
            const float nz = (float)(D(e2y) * e1x - D(e2x) * e1y);
            if (!(fbits(nz) > 0x80000000u)) {                        // facing away (negative z): skipped
                const float px = (float)x, py = (float)y;
                const float ax = (float)(D(s0[0]) - px), ay = (float)(D(s0[1]) - py);
                const float bx = (float)(D(s1[0]) - px), by = (float)(D(s1[1]) - py);
                const float cx = (float)(D(s2[0]) - px), cy = (float)(D(s2[1]) - py);
                const int32_t a = sign_of((float)(D(cx) * ay - D(cy) * ax));
                const int32_t b = sign_of((float)(D(cy) * bx - D(cx) * by));
                const int32_t c = sign_of((float)(D(by) * ax - D(ay) * bx));
                bool in = true;
                if (!a || !b || !c) in = false;
                else if (b != a && a && b) in = false;
                else if (c != a && a && c) in = false;
                else if (c != b && b && c) in = false;
                if (in) {
                    const double z = -(((-((D(nz) * s0[2] + D(ny) * s0[1]) + D(nx) * s0[0])) + D(ny) * py) + D(nx) * px) / nz;
                    const float zf = st(z);
                    if (!(z >= D(best))) {
                        best = zf;
                        result = i;
                    }
                }
            }
        }
        i++;
    } while (Ip(info, 0x10) > i);
    return result;
}
static void fp_mrModelPick(Footprint& f, uint8_t*, const void*, int, int) { fp_project(f, 0); }
PORT_FN(0x00456560, "mrModelPick", mrModelPick, fp_mrModelPick)

// ==== mrModelPickVertex (0x456960) ===============================================================================
// the picked triangle's vertex nearest (x, y) on screen, within 10 pixels
static uint8_t __cdecl mrModelPickVertex(uint8_t* info, const void* fr, int x, int y, int* tri, int* vtx) {
    const int t = mrModelPick_o(info, fr, x, y);
    if (t != -1) {
        float p0[3], p1[3], p2[3], s0[3], s1[3], s2[3];
        tri_verts(info, t, p0, p1, p2);
        if (mrProjectModelPoint_o(p0, s0, fr, 0) && mrProjectModelPoint_o(p1, s1, fr, 0) &&
            mrProjectModelPoint_o(p2, s2, fr, 0)) {
            const double X = D(x), Y = D(y);
            const float d0x = (float)(D(s0[0]) - X), d0y = (float)(D(s0[1]) - Y);
            const float d1x = (float)(D(s1[0]) - X), d1y = (float)(D(s1[1]) - Y);
            const float d2x = (float)(D(s2[0]) - X), d2y = (float)(D(s2[1]) - Y);
            float best = FB(0x41200000);                             // 10
            int32_t v = -1;
            const double r0 = x87_sqrt(D(d0y) * d0y + D(d0x) * d0x);
            const float r0f = st(r0);
            if (!(r0 >= D(FB(0x41200000)))) {
                best = r0f;
                v = Sp(Pp(info, 0x14), 8u * (uint32_t)t);
            }
            const double r1 = x87_sqrt(D(d1y) * d1y + D(d1x) * d1x);
            const float r1f = st(r1);
            if (!(r1 >= D(best))) {
                best = r1f;
                v = Sp(Pp(info, 0x14), 8u * (uint32_t)t + 2);
            }
            const double r2 = x87_sqrt(D(d2y) * d2y + D(d2x) * d2x);
            if (!(r2 >= D(best))) v = Sp(Pp(info, 0x14), 8u * (uint32_t)t + 4);
            if (v != -1) {
                *tri = t;
                *vtx = v;
                return 1;
            }
        }
    }
    *tri = -1;
    *vtx = -1;
    return 0;
}
static void fp_mrModelPickVertex(Footprint& f, uint8_t*, const void*, int, int, int* tri, int* vtx) {
    fp_project(f, 0);
    f.add(tri, 4, "out");
    f.add(vtx, 4, "out");
}
PORT_FN(0x00456960, "mrModelPickVertex", mrModelPickVertex, fp_mrModelPickVertex)

// ==== mrModelPickEdge (0x456b70) =================================================================================
// the picked triangle's edge nearest (x, y) on screen, within 5 pixels (the first test on the distance's bits)
static uint8_t __cdecl mrModelPickEdge(uint8_t* info, const void* fr, int x, int y, int* tri, int* edge) {
    const int t = mrModelPick_o(info, fr, x, y);
    if (t == -1) return 0;
    float p0[3], p1[3], p2[3], s0[3], s1[3], s2[3];
    tri_verts(info, t, p0, p1, p2);
    if (!mrProjectModelPoint_o(p0, s0, fr, 0) || !mrProjectModelPoint_o(p1, s1, fr, 0) ||
        !mrProjectModelPoint_o(p2, s2, fr, 0))
        return 0;
    const float d0 = st(line_point_dist_o(x, y, s0, s1));
    const float d1 = st(line_point_dist_o(x, y, s1, s2));
    const float d2 = st(line_point_dist_o(x, y, s2, s0));
    float best = FB(0x40a00000);                                     // 5
    int32_t e = -1;
    if ((int32_t)fbits(d0) < 0x40a00000) {
        best = d0;
        e = 0;
    }
    if (D(best) > D(d1)) {
        best = d1;
        e = 1;
    }
    if (D(best) > D(d2)) e = 2;
    if (e == -1) return 0;
    *tri = t;
    *edge = e;
    return 1;
}
static void fp_mrModelPickEdge(Footprint& f, uint8_t*, const void*, int, int, int* tri, int* edge) {
    fp_project(f, 0);
    f.add(tri, 4, "out");
    f.add(edge, 4, "out");
}
PORT_FN(0x00456b70, "mrModelPickEdge", mrModelPickEdge, fp_mrModelPickEdge)

// ==== line_point_dist (0x456d30) =================================================================================
// the distance of (x, y, a.z) from the line through a and b
static double __cdecl line_point_dist(int x, int y, const P3* a, const P3* b) {
    const float px = (float)x, py = (float)y;
    float pz;
    cp4(&pz, &a->z);
    float d[3];
    d[0] = (float)(D(b->x) - a->x);
    d[1] = (float)(D(b->y) - a->y);
    d[2] = (float)(D(b->z) - a->z);
    const double inv = 1.0 / x87_sqrt((D(d[1]) * d[1] + D(d[2]) * d[2]) + D(d[0]) * d[0]);
    d[0] = (float)(D(d[0]) * inv);
    d[1] = (float)(D(d[1]) * inv);
    d[2] = (float)(inv * d[2]);
    const double t = ((D(py) - a->y) * d[1] + (D(pz) - a->z) * d[2]) + (D(px) - a->x) * d[0];
    const float ex = (float)(D(px) - (t * d[0] + a->x));
    const float ey = (float)(D(py) - (D(d[1]) * t + a->y));
    const float ez = (float)(D(pz) - (t * d[2] + a->z));
    return x87_sqrt((D(ey) * ey + D(ez) * ez) + D(ex) * ex);
}
static void fp_line_point_dist(Footprint& f, int, int, const P3*, const P3*) { f.pure = true; }
PORT_FN(0x00456d30, "line_point_dist", line_point_dist, fp_line_point_dist)

// ==== mr_model_set_frame (0x456e40) ==============================================================================
// the frame as a D3D world matrix: the rotation's rows with a 0 column, the position with 1
static void __cdecl mr_model_set_frame(const uint8_t* fr) {
    uint32_t m[16];
    memcpy(&m[0], fr + 0x00, 12); m[3] = 0;
    memcpy(&m[4], fr + 0x0c, 12); m[7] = 0;
    memcpy(&m[8], fr + 0x18, 12); m[11] = 0;
    memcpy(&m[12], fr + 0x24, 12); m[15] = 0x3f800000;
    dxMatrixSetModel_o(m);
}
static void fp_mr_model_set_frame(Footprint& f, const uint8_t*) { fp_dx(f); }
PORT_FN(0x00456e40, "mr_model_set_frame", mr_model_set_frame, fp_mr_model_set_frame)

// ==== mrModelGetExtents (0x456ec0) ===============================================================================
static void __cdecl mrModelGetExtents(int m, float* mnx, float* mxx, float* mny, float* mxy, float* mnz, float* mxz) {
    const uint8_t* info = Pp(mip(m), 0x10);
    if (!info) {
        Up(mnx, 0) = 0; Up(mxx, 0) = 0; Up(mny, 0) = 0; Up(mxy, 0) = 0; Up(mnz, 0) = 0; Up(mxz, 0) = 0;
        LogReport(P<const char>(0x004f2298));
        return;
    }
    uint32_t e[6] = {0x461c4000, 0xc61c4000, 0x461c4000, 0xc61c4000, 0x461c4000, 0xc61c4000};   // +-10000
    int32_t n = Ip(info, 0);
    const uint8_t* v = Pp(info, 4);
    if (n > 0) do {
        for (int k = 0; k < 3; k++) {
            const float c = Fp(v, 4 * k);
            float lo, hi;
            memcpy(&lo, &e[2 * k], 4);
            if (!(D(c) >= D(lo))) cp4(&e[2 * k], v + 4 * k);         // test ah, 1: less or unordered
            memcpy(&hi, &e[2 * k + 1], 4);
            if (D(c) > D(hi)) cp4(&e[2 * k + 1], v + 4 * k);
        }
        v += 0x20;
    } while (--n != 0);
    Up(mnx, 0) = e[0]; Up(mxx, 0) = e[1]; Up(mny, 0) = e[2]; Up(mxy, 0) = e[3]; Up(mnz, 0) = e[4]; Up(mxz, 0) = e[5];
}
static void fp_mrModelGetExtents(Footprint& f, int, float* a, float* b, float* c, float* d, float* e, float* g) {
    f.add(a, 4, "out"); f.add(b, 4, "out"); f.add(c, 4, "out"); f.add(d, 4, "out"); f.add(e, 4, "out"); f.add(g, 4, "out");
}
PORT_FN(0x00456ec0, "mrModelGetExtents", mrModelGetExtents, fp_mrModelGetExtents)

// ==== mrModelGetInfo (0x457030) / mrModelGetVerts (0x457050) / mrModelHasAlpha (0x457080) ========================
static void* __cdecl mrModelGetInfo(int m) { return Pp(mip(m), 0x10); }
static void fp_mrModelGetInfo(Footprint&, int) {}
PORT_FN(0x00457030, "mrModelGetInfo", mrModelGetInfo, fp_mrModelGetInfo)

static void __cdecl mrModelGetVerts(int m, uint8_t** verts, int* n) {
    uint8_t* mi = mip(m);
    *verts = Pp(Pp(mi, 0x10), 4);
    *n = Ip(Pp(mi, 0x10), 0);
}
static void fp_mrModelGetVerts(Footprint& f, int, uint8_t** verts, int* n) { f.add(verts, 4, "out"); f.add(n, 4, "out"); }
PORT_FN(0x00457050, "mrModelGetVerts", mrModelGetVerts, fp_mrModelGetVerts)

static uint8_t __cdecl mrModelHasAlpha() { return Ba(S_HAS_ALPHA); }
static void fp_mrModelHasAlpha(Footprint&) {}
PORT_FN(0x00457080, "mrModelHasAlpha", mrModelHasAlpha, fp_mrModelHasAlpha)

// ==== mrModelSetAlpha (0x457090) / mrModelClearAlpha (0x4570b0) / mrModelSetClipDist (0x4570d0) ==================
static void __cdecl mrModelSetAlpha(float a) {
    mrLightSetAlpha_o(fbits(a));
    Ba(S_HAS_ALPHA) = 1;
    dxEnableVertexAlpha(1);
}
static void fp_model_alpha(Footprint& f) {
    fp_alpha(f);
    f.add(P<uint8_t>(S_HAS_ALPHA), 1, "the model alpha is on");
    fp_dx(f);
}
static void fp_mrModelSetAlpha(Footprint& f, float) { fp_model_alpha(f); }
PORT_FN(0x00457090, "mrModelSetAlpha", mrModelSetAlpha, fp_mrModelSetAlpha)

static void __cdecl mrModelClearAlpha() {
    mrLightSetAlpha_o(0x3f800000);
    Ba(S_HAS_ALPHA) = 0;
    dxEnableVertexAlpha(0);
}
PORT_FN(0x004570b0, "mrModelClearAlpha", mrModelClearAlpha, fp_model_alpha)

static void __cdecl mrModelSetClipDist(float n, float f) {
    Ua(S_CLIP_NEAR) = fbits(n);
    Ua(S_CLIP_FAR) = fbits(f);
}
static void fp_mrModelSetClipDist(Footprint& f, float, float) {
    f.add(P<uint8_t>(S_CLIP_FAR), 4, "the far clip");
    f.add(P<uint8_t>(S_CLIP_NEAR), 4, "the near clip");
}
PORT_FN(0x004570d0, "mrModelSetClipDist", mrModelSetClipDist, fp_mrModelSetClipDist)

// ==== mrModelFlush (0x4570f0) ====================================================================================
// every model drawn (lit) until the texture system stops de-ressing: the textures they use loaded up front
static void __cdecl mrModelFlush() {
    TextureFlush();
    do {
        uint32_t eye[3] = {0x42c80000, 0x42c80000, 0x42c80000};    // (100, 100, 100)
        uint32_t target[3] = {0, 0, 0};
        mrBeginFrame_o();
        mrSetProjection_o(0x3f800000, 0x447a0000, 0x3f9c61aa);
        const uint8_t lit = mrIsEnabled_o(1);
        if (!lit) mrEnable_o(1);
        mrSetView5_o(0, 0, Ia(S_GX_WID), Ia(S_GX_HIT), 1);
        mrCameraLook_o(eye, target);
        mrModelSetFrame_o(P<void>(S_IDENT_FRAME));
        for (uint8_t* mi = *P<uint8_t*>(S_MODELS); mi; mi = Pp(mi, 0x2c))
            if (Ip(mi, 0x10)) mrModelDraw1_o((int)(uintptr_t)convert_to_mrmodel_o(mi));
        if (!lit) mrDisable_o(1);
        mrEndFrame_o();
    } while (TextureWillDeresNextFrame());
}
static void fp_mrModelFlush(Footprint& f) { f.replay_only = "draws every model, frames, texture flush"; }
PORT_FN(0x004570f0, "mrModelFlush", mrModelFlush, fp_mrModelFlush)

// ==== convert_to_mrmodel (0x4571e0) / convert_to_mip (0x4571f0) ==================================================
static int __cdecl convert_to_mrmodel(uint32_t p) { return (int)p; }     // (model_info*): the handle is the pointer
static void fp_convert_to_mrmodel(Footprint& f, uint32_t) { f.pure = true; }
PORT_FN(0x004571e0, "convert_to_mrmodel", convert_to_mrmodel, fp_convert_to_mrmodel)
static void* __cdecl convert_to_mip(int m) { return (void*)(uintptr_t)(uint32_t)m; }
static void fp_convert_to_mip(Footprint& f, int) { f.pure = true; }
PORT_FN(0x004571f0, "convert_to_mip", convert_to_mip, fp_convert_to_mip)

// ==== begin_deferred (0x457200) / end_deferred (0x457260) / begin_deferred_surfs (0x457280) ======================
static void __cdecl begin_deferred() {
    uint8_t* p = (uint8_t*)MemAlloc(0x20);
    if (p) {
        PoolBase_ctor(p, 0, P<const char>(0x004f22c0), (int)m1_operand(0x00457217), 0x10);   // 'deferr pool' (M1: 8192)
        Up(p, 0) = 0x004dcb38;
        Ua(S_DEFER_POOL) = (uint32_t)(uintptr_t)p;
    } else {
        Ua(S_DEFER_POOL) = 0;
    }
    // the buckets: M1 repoints this clear at its own array and makes it 1,024 long
    memset(P<void>(m1_operand(0x0045723e)), 0, 4u * m1_operand(0x00457245));
    Ba(S_DEFER_ON) = 0;
}
static void fp_begin_deferred(Footprint& f) { f.replay_only = "allocates the deferred pool"; }
PORT_FN(0x00457200, "begin_deferred", begin_deferred, fp_begin_deferred)

static void __cdecl end_deferred() {
    if (void* p = *P<void*>(S_DEFER_POOL)) pool_delete(p);
    Ua(S_DEFER_POOL) = 0;
}
static void fp_end_deferred(Footprint& f) { f.replay_only = "frees the deferred pool"; }
PORT_FN(0x00457260, "end_deferred", end_deferred, fp_end_deferred)

static void __cdecl begin_deferred_surfs() { Ba(S_DEFER_ON) = 1; }
static void fp_begin_deferred_surfs(Footprint& f) { f.add(P<uint8_t>(S_DEFER_ON), 1, "deferring"); }
PORT_FN(0x00457280, "begin_deferred_surfs", begin_deferred_surfs, fp_begin_deferred_surfs)

// ==== the deferred surfaces: add_deferred_surf (0x4573d0), end_deferred_surfs (0x457290), draw_alpha_deferred_surfs
// (0x457340) =====================================================================================================
// Surfaces drawn late (the track, after the scene) are bucketed by texture number -- bucket id + 1, bucket 0 for
// untextured -- and drawn bucket by bucket: end_deferred_surfs draws the opaque textures' buckets, then
// draw_alpha_deferred_surfs everything still queued; each entry lit (LightSurfacePreLit) and drawn, then freed.
//   The array and its size are begin_deferred's operands (M1 repoints them: see the top of this file). Stock (120):
// the loops visit buckets 0..118, texture 118's bucket is filled and never drawn, and texture 119 and up are written
// past the array, into the model statics after it (FIX CANDIDATE: the "~119 textures" crash) -- all as the original.
// Lifted (M1: 1,024): the same, with the loops to bucket 1,022, and a texture number whose bucket would be outside
// the array goes to bucket 0 (drawn untextured) and is logged once -- M1's guard, which stock code never meets below
// texture 119. M1's exit line (the highest texture number bucketed, the overflows) is fed by vp_deferred_bucket_ok.
//   These three were M1's own hooks on every build (viperport.cpp), so they stay build-agnostic: every address
// through A(), and the race.bin builds' prologues are registered (PORT_FN_BUILDS), as M1's jmp_hooks checked them.
#if defined(VP_FUZZ) || defined(VP_GX_HARNESS)
static bool deferred_bucket_ok(int id, uint32_t buckets) { const int32_t b = id + 1; return b >= 0 && (uint32_t)b < buckets; }
#else
bool vp_deferred_bucket_ok(int id, int buckets);                     // viperport.cpp: the check, and the exit line's counts
static bool deferred_bucket_ok(int id, uint32_t buckets) { return vp_deferred_bucket_ok(id, (int)buckets); }
#endif
enum : uint32_t { DEFERRED_STOCK = 0x78 };                           // 120 buckets
static struct {
    bool ok;
    uint32_t pool, flag, lit_buf, buckets_op, count_op;
    PoolAlloc_t alloc;
    PoolFree_t free;
    Void_t flush, end, draw_alpha;
    IntV_t begin;
    Draw4_t draw;
    IntB_t has_alpha;
    LightFn_t prelit;
} g_def;
static void deferred_resolve() {
    if (g_def.ok) return;
    g_def.pool = A(S_DEFER_POOL);
    g_def.flag = A(S_DEFER_ON);
    g_def.lit_buf = A(S_LIT_BUF);
    const bool v10 = build_is_v10();                                 // an operand: at + 1 on v1.0, the table's own elsewhere
    g_def.buckets_op = v10 ? 0x0045723e : A(0x0045723d);
    g_def.count_op = v10 ? 0x00457245 : A(0x00457244);
    g_def.alloc = (PoolAlloc_t)A(0x0041bb20);
    g_def.free = (PoolFree_t)A(0x0041bb60);
    g_def.flush = (Void_t)A(0x0045dfd0);
    g_def.begin = (IntV_t)A(0x00458990);
    g_def.draw = (Draw4_t)A(0x00458a60);
    g_def.end = (Void_t)A(0x00458a90);
    g_def.has_alpha = (IntB_t)A(0x0045a590);
    g_def.prelit = (LightFn_t)A(0x0045c250);
    g_def.draw_alpha = (Void_t)A(0x00457340);
    g_def.ok = true;
}
static __forceinline uint8_t** deferred_buckets() { return (uint8_t**)(uintptr_t)m1_operand(g_def.buckets_op); }
static __forceinline uint32_t deferred_count() { return m1_operand(g_def.count_op); }

static void __cdecl add_deferred_surf(uint8_t* info, uint8_t* surf, uint8_t* si) {
    deferred_resolve();
    const int32_t id = Ip(si, 0x14);
    uint8_t* e = (uint8_t*)g_def.alloc(*P<void*>(g_def.pool), 0);
    Pp(e, 0) = si;
    Pp(e, 4) = surf;
    Pp(e, 8) = info;
    const uint32_t n = deferred_count();
    uint32_t b = (uint32_t)id + 1u;
    if (n != DEFERRED_STOCK && !deferred_bucket_ok(id, n)) b = 0;    // M1's guard (lifted buckets only)
    uint8_t** slot = deferred_buckets() + b;
    Pp(e, 0xc) = *slot;
    *slot = e;
}
static void fp_add_deferred_surf(Footprint& f, uint8_t*, uint8_t*, uint8_t* si) {
    deferred_resolve();
    if (deferred_count() != DEFERRED_STOCK) {
        f.replay_only = "M1's lifted buckets: the stock original writes the 120-entry array instead";
        return;
    }
    fp_pool(f, g_def.pool);
    f.add(deferred_buckets() + ((uint32_t)Ip(si, 0x14) + 1u), 4, "the texture's bucket");
}
static const uint8_t k_add_deferred_pro[] = {0x56, 0x8B, 0x0D, 0x80, 0x2D, 0x52, 0x00};   // push esi; mov ecx, [pool]
PORT_FN_BUILDS(0x004573d0, "add_deferred_surf", add_deferred_surf, fp_add_deferred_surf, k_add_deferred_pro,
               sizeof k_add_deferred_pro)

static void draw_deferred_bucket(uint8_t** slot, int tex) {
    g_def.begin(tex);
    for (uint8_t* e = *slot; e;) {
        g_def.prelit(Pp(e, 4), Pp(Pp(e, 8), 4), *P<uint8_t*>(g_def.lit_buf));
        uint8_t* si = Pp(e, 0);
        g_def.draw(Pp(si, 0x20), Ip(si, 0x24), (const uint16_t*)Pp(si, 0x18), Ip(si, 0x1c));
        uint8_t* next = Pp(e, 0xc);
        g_def.free(*P<void*>(g_def.pool), 0, e);
        e = next;
    }
    g_def.end();
    *slot = 0;
}

static void __cdecl end_deferred_surfs() {
    deferred_resolve();
    Ba(g_def.flag) = 0;                                               // a byte (mov byte ptr [522d84h], bl)
    g_def.flush();
    uint8_t** buckets = deferred_buckets();
    const int32_t last = (int32_t)deferred_count() - 1;               // stock: cmp ebx, 0x77; jl
    for (int32_t b = 0; b < last; b++)
        if (buckets[b] && !g_def.has_alpha(b - 1)) draw_deferred_bucket(&buckets[b], b - 1);
    g_def.draw_alpha();
}
static void fp_end_deferred_surfs(Footprint& f) { fp_draws(f); }
static const uint8_t k_end_deferred_pro[] = {0x53, 0x56, 0x57, 0x33, 0xDB, 0x55, 0x88, 0x1D};
PORT_FN_BUILDS(0x00457290, "end_deferred_surfs", end_deferred_surfs, fp_end_deferred_surfs, k_end_deferred_pro,
               sizeof k_end_deferred_pro)

static void __cdecl draw_alpha_deferred_surfs() {
    deferred_resolve();
    g_def.flush();
    uint8_t** buckets = deferred_buckets();
    const int32_t last = (int32_t)deferred_count() - 1;
    for (int32_t b = 0; b < last; b++)
        if (buckets[b]) draw_deferred_bucket(&buckets[b], b - 1);
}
static const uint8_t k_alpha_deferred_pro[] = {0x53, 0x56, 0x57, 0x55, 0x33, 0xFF};
PORT_FN_BUILDS(0x00457340, "draw_alpha_deferred_surfs", draw_alpha_deferred_surfs, fp_end_deferred_surfs,
               k_alpha_deferred_pro, sizeof k_alpha_deferred_pro)

// ==== model_copy_transform (0x457410) ============================================================================
// a new model with the same name and triangle count, and a copy of the info in one MemAlloc (header, vertices,
// surfaces, triangles, the two record arrays), its vertex positions moved by the frame (normals untouched)
static int __cdecl model_copy_transform(int m, const uint8_t* fr, uint8_t** out) {
    uint8_t* mi = mip(m);
    const uint8_t* src = Pp(mi, 0x10);
    char nb[17];
    const int m2 = mrModelCreate_o(name16((const char*)mi, nb), Ip(src, 0x10));
    const uint32_t vb = (uint32_t)Ip(src, 0) << 5, tb = (uint32_t)Ip(src, 0x10) << 3, sb = (uint32_t)Ip(src, 8) << 5,
                   rb = (uint32_t)Ip(src, 0x18) << 4, qb = (uint32_t)Ip(src, 0x20) << 2;
    uint8_t* p = (uint8_t*)MemAlloc((int)(qb + vb + rb + sb + tb + 0x28));
    const uint32_t tri_off = sb + vb + 0x28, rec_off = tri_off + tb, q_off = rec_off + rb;
    memcpy(p, src, 0x28);
    memcpy(p + 0x28, Pp(src, 4), vb);
    memcpy(p + vb + 0x28, Pp(src, 0xc), sb);
    memcpy(p + tri_off, Pp(src, 0x14), tb);
    memcpy(p + rec_off, Pp(src, 0x1c), rb);
    memcpy(p + q_off, Pp(src, 0x24), qb);
    Pp(p, 4) = p + 0x28;
    Pp(p, 0xc) = p + vb + 0x28;
    Pp(p, 0x14) = p + tri_off;
    Pp(p, 0x1c) = p + rec_off;
    Pp(p, 0x24) = p + q_off;
    *out = p;
    for (int32_t i = 0; Ip(p, 0) > i; i++) {
        float* v = (float*)(Pp(p, 4) + 32u * (uint32_t)i);
        const double y = (D(Fp(fr, 0x10)) * v[1] + D(Fp(fr, 0x04)) * v[0]) + D(Fp(fr, 0x1c)) * v[2];
        const double z = (D(Fp(fr, 0x20)) * v[2] + D(Fp(fr, 0x14)) * v[1]) + D(Fp(fr, 0x08)) * v[0];
        const double x = (D(Fp(fr, 0x18)) * v[2] + D(Fp(fr, 0x0c)) * v[1]) + D(Fp(fr, 0x00)) * v[0];
        v[0] = (float)x;
        v[1] = (float)y;
        v[2] = (float)z;
        v[0] = (float)(D(Fp(fr, 0x24)) + v[0]);
        v[1] = (float)(D(Fp(fr, 0x28)) + v[1]);
        v[2] = (float)(D(Fp(fr, 0x2c)) + v[2]);
    }
    return m2;
}
static void fp_model_copy_transform(Footprint& f, int, const uint8_t*, uint8_t**) { fp_copy(f); }
PORT_FN(0x00457410, "model_copy_transform", model_copy_transform, fp_model_copy_transform)

// ==== the pools' scalar deleting destructors (0x4575e0 surface info, 0x457600 model info, 0x457620 deferred) ======
static void* __fastcall Pool_sdtor(void* self, Edx, unsigned flags) {
    PoolBase_dtor(self, 0);
    if (flags & 1) OpDelete(self);
    return self;
}
static void fp_pool_sdtor(Footprint& f, void*, Edx, unsigned) { f.replay_only = "frees the pool's storage (and the pool)"; }
static void* __fastcall Pool_surf_sdtor(void* self, Edx e, unsigned flags) { return Pool_sdtor(self, e, flags); }
static void* __fastcall Pool_model_sdtor(void* self, Edx e, unsigned flags) { return Pool_sdtor(self, e, flags); }
static void* __fastcall Pool_deferred_sdtor(void* self, Edx e, unsigned flags) { return Pool_sdtor(self, e, flags); }
PORT_FN(0x004575e0, "Pool<model_surface_info>::scalar deleting destructor", Pool_surf_sdtor, fp_pool_sdtor)
PORT_FN(0x00457600, "Pool<model_info>::scalar deleting destructor", Pool_model_sdtor, fp_pool_sdtor)
PORT_FN(0x00457620, "Pool<deferred_entry>::scalar deleting destructor", Pool_deferred_sdtor, fp_pool_sdtor)
