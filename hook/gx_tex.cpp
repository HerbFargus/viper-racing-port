// gx_tex.cpp -- M3 graphics stage, step G1 (faithful), group G1c: the textures. gx.obj (the gx* API: modes, the
// screen page, and the texture wrappers that take the gx lock), texture.obj (the texture cache: ids, names and keys,
// reference counts, lazy video loading, grabs, de-rezing), tmap.obj (every DirectDraw / Direct3D texture operation,
// through dd.obj's wrappers) and tex.obj (the .tex resources: mip levels, pasting into a gxCanvas).
//
// Layering (out/agents/rtex/report.md):
//   game code -> gx*Texture (gx.obj: _SingleEnter(gx_sync) .. _SingleLeave around each call)
//             -> Texture* / tc_* (texture.obj: an int id = an index into the texture table)
//             -> tx* (tmap.obj: texture_info*, the pools, dtexture / dsurface calls)
//             -> tex* (tex.obj: the tt_texture payload of a TEX resource)
// The texture table: 250 entries of 0x28 bytes at 0x522fa0 { u8 in_use; int refs; char name[16]; int key; u8 loaded;
// texture_info* sys; texture_info* vid }. M1 (viperport.cpp, relocate_texture_table) moves it into the DLL with 1000
// entries by repointing the 66 instructions that address it (texture_table_fields.inc) and tc_add's / tc_lookup's
// count immediates; it also raises the sys / vid texture_info pools txBegin makes (200 / 128 -> 1024). So every
// rewrite here reads those operands from the original's own instructions (m1_operand; m1_operand_hooked for the two
// that lie in a hooked function's first 5 bytes: TextureEnd's and tc_clear's table base), and a harness without M1
// gets the stock values.
//
// texture_info (0x28, from the pools): +0 u8 (never read), +4 dx_texture_format (0 solid, 1 transparent, 2 alpha: an
// index into the format table), +8 size as created, +0xc mip levels created, +0x10 tt_texture* (while loading),
// +0x14..+0x17 the .tex bytes 0..3 (format, use mips, wrap, de-res priority), +0x18 u16 colour key, +0x1c the file's
// level count, +0x20 dtexture*, +0x24 next (the sys list 0x4f3e74 / vid list 0x4f3e70). The format table (0x526688,
// 3 x 0x28): +0 found, +4 DDPIXELFORMAT, +0x24 gx canvas type.
//
// Written from the v1.0 disassembly: every call in the original's order, by address (this group's own functions too,
// so their rewrites run where hooked); the game's CRT by address (stricmp, strstr); ExitProcess through the game's
// import slot. The inlined string copies (repne scasb; rep movsd; rep movsb) are the same instructions, so an overlong
// name smears exactly as the original's does.
//
// Footprints (graphics runs on the main thread: every static written is listed). DirectDraw / Direct3D calls are the
// emulation's business (docs/PORTING.md, "DirectDraw and Direct3D"): recorded, compared and fed to the rewrite's pass.
// replay_only is kept for what allocates or frees heap memory (a texture surface is a MemAlloc'd dtexture; the pools),
// loads resources (texGet: ResourceGetDiscardable may open a file), or pumps the window messages (Win32Idle, reached
// through VidReady / VidFlip / VidGetPage), and for texture_find_formats, whose writes happen in the callbacks
// EnumTextureFormats makes (a rewrite's pass handed the call's results sees no callback). The cache functions repeat
// the lookup read-only in their footprints to tell a bounded call (an entry found, a reference counted) from one that
// loads or frees.
// Two things a byte-for-byte check can't match (test/world_gx_tex.cpp compares them as noted):
//   * gx's current canvas (0x4efbf8): load_texmap, load_texmap_mip and alpha_blit_texmap (and gxChangeMode, through
//     gxGrabScreen) gxSetCanvas a canvas on their own stack frame and return with it still current -- a dangling
//     pointer into a dead frame, whose value is the frame's layout: the rewrite's is elsewhere on the stack. Their
//     footprints list it with f.stack_ptr, which matches any two addresses on the stack.
//   * the pixels of a surface these functions lock themselves (load_texmap, load_texmap_mip, alpha_blit_texmap): no
//     footprint can name them before the Lock; both passes' Locks hand out the same pixels, and each Unlock's
//     recorded call carries the pixels as that pass left them, so the check compares them.
//
// Fixes (docs/PORTING.md, "Fixes"; each marked FIX:, VP_FIX; test/world_gx_tex.cpp built with /DFIX_TESTS tests them):
//   * texture names of any length (tc_add's unbounded strcpy into the entry's 16-byte name): a name of 16+ characters
//     is kept whole in the DLL and the entry holds a mark (set_long_tex_name / entry_name); every reader of the name
//     uses the full one. Names up to 15 characters are stored and read exactly as the original does. Long names are
//     reported to the log cut to 128 characters (the log formats into 256 bytes).
//   * an entry without a system copy (sys NULL: its surface couldn't be made, or a free entry) is treated as no
//     texture by TextureSelect / HasAlpha / GetSize / Blit / AlphaBlit / Grab, where the original crashed.
// Not fixed (not crashes in reachable play):
//   * dump_texture_format builds its flag list in a 0x50-byte stack buffer; the ten names total 98 characters, so a
//     pixel format with most flags set overruns the original's frame (its return address). Unreachable: nothing in
//     v1.0 calls it; the rewrite's overrun lands in a spill area of its own frame.
//   * txRelease without a grab reads NULL->dtex: TextureRelease only calls it with a grab id set, which a failed
//     txGrab (a lost surface) leaves behind -- but every caller releases only after a successful grab, and the next
//     grab replaces the id.
//   * TextureDestroy(-1) indexes entry -1 (no caller passes -1); TextureRestoreAll(1) / TextureBeginFrame leave `sys`
//     dangling when a reload fails (a pool element still valid memory, reused by the next texture_info).
//
// Skipped: the $E static initialisers of gx.obj, texture.obj, tmap.obj and tex.obj (colour constants; the CRT runs
// them before any hook exists).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

// ---- the inlined string operations, as the original's instructions -------------------------------------------------
// strcpy: repne scasb (the length, NUL included), then rep movsd, rep movsb: forward
static __forceinline void gxt_strcpy(char* dst, const char* src) {
    __asm { mov edi, src
            mov ecx, 0xffffffff
            sub eax, eax
            repne scasb
            not ecx
            sub edi, ecx
            mov eax, ecx
            shr ecx, 2
            mov esi, edi
            mov edi, dst
            rep movsd
            mov ecx, eax
            and ecx, 3
            rep movsb }
}
// strcat of a literal the compiler knew: the destination's end (repne scasb; dec edi), then the literal's bytes, NUL
// included
static __forceinline void gxt_strcat(char* dst, const char* src) {
    char* e = dst + strlen(dst);
    memcpy(e, src, strlen(src) + 1);
}

// __ftol of n * u, the way txBlit has it: fild n; fld dword u; fmul st(1) (the product in the register, at the
// thread's precision); fistp qword with the rounding chopped; the low dword. The fild'ed n is popped after.
static __forceinline int32_t gxt_ftol_mul(int32_t n, const float* u) {
    int32_t lo;
    uint16_t cw, chop;
    int64_t q;
    __asm { fild n
            mov eax, u
            fld dword ptr [eax]
            fmul st, st(1)
            fnstcw cw
            mov ax, cw
            or ah, 0x0c
            mov chop, ax
            fldcw chop
            fistp qword ptr q
            fldcw cw
            fstp st(0) }
    lo = (int32_t)q;
    return lo;
}

// ---- layouts --------------------------------------------------------------------------------------------------------
namespace {

struct TexInfo {                           // texture_info (0x28), a Pool<texture_info> element
    uint8_t b0, _1[3];                     // +0x00 (never read)
    int32_t fmt;                           // +0x04 dx_texture_format: the format table's index
    int32_t size;                          // +0x08 as created
    int32_t levels;                        // +0x0c mip levels created
    uint8_t* tt;                           // +0x10 the tt_texture, while loading
    uint8_t tfmt, mips, wrap, deres;       // +0x14 the .tex header's bytes 0..3
    uint16_t key, _1a;                     // +0x18 the colour key
    int32_t file_levels;                   // +0x1c the file's level count (size = 1 << (n - 1))
    void* dtex;                            // +0x20 dtexture*
    TexInfo* next;                         // +0x24 the sys / vid list
};
static_assert(sizeof(TexInfo) == 0x28 && offsetof(TexInfo, dtex) == 0x20, "TexInfo");

struct TexEntry {                          // the texture table's entry (0x28)
    uint8_t in_use, _1[3];                 // +0x00
    int32_t refs;                          // +0x04
    char name[16];                         // +0x08 strcpy'd, unbounded (tc_add)
    int32_t key;                           // +0x18
    uint8_t loaded, _1d[3];                // +0x1c the video copy is loaded
    TexInfo* sys;                          // +0x20
    TexInfo* vid;                          // +0x24
};
static_assert(sizeof(TexEntry) == 0x28 && offsetof(TexEntry, sys) == 0x20, "TexEntry");

struct Canvas {                            // gxCanvas (0x24)
    int8_t type;                           // +0x00 1 ARGB4444, 2 1555, 3 555, 4 565, 5 32-bit
    uint8_t flag, _2[2];                   // +0x01 (0x80)
    uint8_t* bits;                         // +0x04
    int32_t w, h, pitch;                   // +0x08 (pitch in bytes)
    int32_t clip[4];                       // +0x14
};
static_assert(sizeof(Canvas) == 0x24, "Canvas");

struct DTexture {                          // dd.obj's dtexture (0x10): its dsurface first
    void* surf;                            // +0x00 IDirectDrawSurface3*: the root, or the mip being walked
    void* tex;                             // +0x04 IDirect3DTexture2*
    uint32_t handle;                       // +0x08 D3DTEXTUREHANDLE
    void* root;                            // +0x0c the root, saved during a mip walk
};
static_assert(sizeof(DTexture) == 0x10, "DTexture");

}  // namespace

// ---- statics -----------------------------------------------------------------------------------------------------
#define G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define G16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define G32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define GI(a) (*(volatile int32_t*)(uintptr_t)(a))
#define GP(T, a) (*(T* volatile*)(uintptr_t)(a))
enum : uint32_t {
    // gx.obj
    X_GX_SYNC = 0x005228d0,        // int gx_sync: the gx lock (SingleBegin("gx"))
    X_MODE = 0x004eefa8,           // the current VIDEO_MODE
    X_SCREEN_W = 0x005228f4,       // gxScreenWid
    X_SCREEN_H = 0x005228d4,       // gxScreenHit
    X_BLACK = 0x005228ac,          // the colour gxChangeMode clears the screen to
    // texture.obj
    X_GRAB_ID = 0x004f3270,        // the grabbed texture's id (-1: none)
    X_DERES = 0x004f3274,          // u8: de-rez at the next TextureBeginFrame
    // tmap.obj
    X_VID_LIST = 0x004f3e70,
    X_SYS_LIST = 0x004f3e74,
    X_SYS_POOL = 0x004f3e78,       // Pool<texture_info>*
    X_VID_POOL = 0x004f3e7c,
    X_GRABBED = 0x004f3e80,        // the grabbed texture_info
    X_CONSERVE = 0x004f3e84,       // levels to drop (txConserveMode)
    X_CONSERVE_BASE = 0x004f3e88,
    X_FORMATS = 0x00526688,        // the format table, 3 x 0x28
    X_BLUE = 0x00526720,           // colours: load_texmap's clear
    X_YELLOW = 0x00526728,         // load_texmap_mip's clear
    X_RED = 0x00526750,            // the missing .tex's colour
    X_DUMMY = 0x00526770,          // the missing .tex: a tt_texture {format 0, key 0, 3 levels}
    // elsewhere
    X_MRCAPS_ALPHA = 0x00522a1c,   // mrCaps.alpha: 0, 1 or 2
    X_HW_MIPS = 0x00522a21,        // mrCaps: hardware mipmaps
    X_FEAT_MIP = 0x005229ed,       // the mipmapping feature
    X_DD = 0x004f1e88,             // class ddraw* dd
    X_D3D = 0x00522e64,            // class d3d*
    X_DX_RESULT = 0x00522e6c,      // long dx_result: every dd.obj wrapper stores its HRESULT
    X_DX_ERRBUF = 0x00522e78,      // char[0x20]: dx_name_error's "unknown error: %x" (dx_error on a failed call)
    X_CANVAS = 0x004efbf8,         // gx's current canvas (gxSetCanvas)
    X_TEX_TYPE = 0x004f4208,       // TEX_TYPE 'TEX '
    X_TEX_VER = 0x004f420c,        // TEX_VER 3
    X_SECONDARY = 0x004f1ea0,      // VidDisableSecondary's flag
    X_VID_STATE = 0x00522ac8,      // VidReady: > 2 answers without Win32Idle
    X_IMP_EXITPROCESS = 0x005d7478,// the import slot of ExitProcess
};
#define FMT_GXTYPE(i) G8(X_FORMATS + 0x24 + (uint32_t)(i) * 0x28u)   // the format table's gx type (movsx index)
static const char* S(uint32_t a) { return (const char*)(uintptr_t)a; }

// the texture table: entry i's field, from `at` -- the field's address in entry 0, which is the operand of the
// original's instruction (m1_operand(operand): moved by M1)
#define TE_AT(at, i) ((uint32_t)(at) + (uint32_t)(i) * 0x28u)
#define TE8(at, i) (*(volatile uint8_t*)(uintptr_t)TE_AT(at, i))
#define TE32(at, i) (*(volatile int32_t*)(uintptr_t)TE_AT(at, i))
#define TEP(at, i) (*(TexInfo* volatile*)(uintptr_t)TE_AT(at, i))

// ---- the functions they call, by address -----------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef int(__cdecl* SingleBegin_t)(const char*);
static const SingleBegin_t SingleBegin = (SingleBegin_t)0x00414f30;
typedef void(__cdecl* Single3_t)(int, const char*, int);
static const Single3_t SingleEnter = (Single3_t)0x00415000, SingleLeave = (Single3_t)0x00415070,
                       SingleEnd = (Single3_t)0x00415090;
typedef uint8_t(__cdecl* Byte_t)();
typedef void(__cdecl* ByteArg_t)(uint8_t);
typedef void(__cdecl* IntArg_t)(int);
typedef int(__cdecl* Int_t)();
static const Byte_t vid_begin = (Byte_t)0x004545b0;
static const Void_t gfx_begin = (Void_t)0x0044f850, gfx_end = (Void_t)0x0044f860, vid_end = (Void_t)0x00454630;
static const ByteArg_t ExceptSinglePrecision = (ByteArg_t)0x00415ca0;
typedef uint8_t(__cdecl* VidSetMode_t)(int, uint8_t);
static const VidSetMode_t VidSetMode = (VidSetMode_t)0x00454670;
static const Void_t pal_build_system = (Void_t)0x0045a950, mr_set_mode = (Void_t)0x0044e850,
                    mr_restore_mode = (Void_t)0x0044e8c0, VidRestoreMode = (Void_t)0x00454720,
                    mr_release = (Void_t)0x0044e8f0, mr_restore = (Void_t)0x0044e910, VidFlip = (Void_t)0x00454880,
                    VidDisableSecondary = (Void_t)0x00454650, VidReleasePage = (Void_t)0x00454870;
static const Byte_t VidReady = (Byte_t)0x00454850, VidHas3DHW = (Byte_t)0x00454930, VidIs3DFX = (Byte_t)0x00454660;
static const Int_t VidGetMegsRam = (Int_t)0x00454940;
typedef uint8_t(__cdecl* CanvasByte_t)(Canvas*);
static const CanvasByte_t VidGetPage = (CanvasByte_t)0x00454740;
typedef Canvas*(__cdecl* SetCanvas_t)(Canvas*);
static const SetCanvas_t gxSetCanvas = (SetCanvas_t)0x0044fe30;
typedef void(__cdecl* Clear_t)(uint32_t);
static const Clear_t gxClear = (Clear_t)0x0044ffd0;
typedef void(__cdecl* BuildCanvas_t)(Canvas*, int8_t, void*, int, int, int);
static const BuildCanvas_t gxBuildCanvas = (BuildCanvas_t)0x0044f880;
typedef void(__cdecl* Paste_t)(Canvas*, int, int);
static const Paste_t gxPaste = (Paste_t)0x00451500, gxPasteAlpha = (Paste_t)0x00451550;
typedef void(__cdecl* Reduce_t)(Canvas*);
static const Reduce_t gfxReduceTo555 = (Reduce_t)0x004516c0;
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* Delete_t)(void*);
static const Delete_t op_delete = (Delete_t)0x00414390;
typedef void*(__fastcall* PoolCtor_t)(void*, Edx, const char*, int, uint32_t);
static const PoolCtor_t PoolBase_ctor = (PoolCtor_t)0x0041ba30;
typedef void*(__fastcall* PoolAlloc_t)(void*, Edx);
static const PoolAlloc_t PoolBase_alloc = (PoolAlloc_t)0x0041bb20;
typedef void(__fastcall* PoolFree_t)(void*, Edx, void*);
static const PoolFree_t PoolBase_free = (PoolFree_t)0x0041bb60;
typedef void(__fastcall* ThisVoid_t)(void*, Edx);
static const ThisVoid_t PoolBase_dtor = (ThisVoid_t)0x0041bac0;
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
static const Stricmp_t crt_stricmp = (Stricmp_t)0x004da350;
typedef char*(__cdecl* Strstr_t)(const char*, const char*);
static const Strstr_t crt_strstr = (Strstr_t)0x004cf9a0;
typedef void(__stdcall* ExitProcess_t)(uint32_t);
typedef const void*(__cdecl* ResGetDisc_t)(const char*, uint32_t, uint32_t*, int32_t*);
static const ResGetDisc_t ResourceGetDiscardable = (ResGetDisc_t)0x00419f70;
typedef uint8_t(__cdecl* ResForget_t)(void*);
static const ResForget_t ResourceForget = (ResForget_t)0x0041a450;
// dd.obj's wrappers (__thiscall)
typedef DTexture*(__fastcall* DtexMip_t)(DTexture*, Edx);
static const DtexMip_t dtex_first_mip = (DtexMip_t)0x0045f930, dtex_next_mip = (DtexMip_t)0x0045f940;
typedef void(__fastcall* DtexVoid_t)(DTexture*, Edx);
static const DtexVoid_t dtex_cleanup_mip = (DtexVoid_t)0x0045f9a0, dsurf_unlock = (DtexVoid_t)0x0045f510;
typedef uint8_t(__fastcall* DsurfLock_t)(DTexture*, Edx, uint32_t*);
static const DsurfLock_t dsurf_lock = (DsurfLock_t)0x0045f490;
typedef void(__fastcall* DsurfKey_t)(DTexture*, Edx, uint32_t);   // (unsigned short: pushed as a dword, masked)
static const DsurfKey_t dsurf_set_colorkey = (DsurfKey_t)0x0045f710;
typedef void(__fastcall* DsurfBlit_t)(DTexture*, Edx, DTexture*);
static const DsurfBlit_t dsurf_blit = (DsurfBlit_t)0x0045f690, dtex_load = (DsurfBlit_t)0x0045f7e0;
typedef void(__fastcall* DsurfBltFast_t)(DTexture*, Edx, DTexture*, uint32_t, uint32_t, int32_t*);
static const DsurfBltFast_t dsurf_blit_fast = (DsurfBltFast_t)0x0045f6d0;
typedef void(__fastcall* DtexSelect_t)(DTexture*, Edx, uint32_t, uint32_t);   // (two unsigned chars)
static const DtexSelect_t dtex_select = (DtexSelect_t)0x0045f8a0;
typedef DTexture*(__fastcall* DdCreate_t)(void*, Edx, uint32_t*);
static const DdCreate_t ddraw_create_texture_surface = (DdCreate_t)0x0045f400;
typedef void(__fastcall* DdDestroy_t)(void*, Edx, DTexture*);
static const DdDestroy_t ddraw_destroy_texture_surface = (DdDestroy_t)0x0045f440;
typedef void(__fastcall* EnumFormats_t)(void*, Edx, uint32_t, void*);
static const EnumFormats_t d3d_EnumTextureFormats = (EnumFormats_t)0x0045f130;

// this group's own functions, by address (their rewrites run where they're hooked)
static const IntArg_t TextureUnload = (IntArg_t)0x0045a550;
static const ByteArg_t TextureReleaseAll = (ByteArg_t)0x0045a050, TextureRestoreAll = (ByteArg_t)0x0045a090,
                       txConserveMode = (ByteArg_t)0x0045fcb0;
typedef int(__cdecl* TexGet_t)(const char*, int);
static const TexGet_t TextureGet = (TexGet_t)0x0045a0c0;
static const IntArg_t TextureForget = (IntArg_t)0x0045a110, TextureDestroy = (IntArg_t)0x0045a1d0,
                      TextureReloadVideo = (IntArg_t)0x0045a2c0, TextureUnloadVideo = (IntArg_t)0x0045a2f0,
                      TextureRevert = (IntArg_t)0x0045a390;
typedef int(__cdecl* TexCreate_t)(const char*, int, int);
static const TexCreate_t TextureCreate = (TexCreate_t)0x0045a170;
static const Void_t TextureFreeAll = (Void_t)0x0045a000, TextureRelease = (Void_t)0x0045a330, tc_clear = (Void_t)0x0045a5b0,
                    txBegin = (Void_t)0x0045fbd0, txEnd = (Void_t)0x0045fc70, txRelease = (Void_t)0x004605e0;
typedef void(__cdecl* TexBlit_t)(int, int, uint32_t, uint32_t, uint32_t, uint32_t);   // floats, passed as bits
static const TexBlit_t TextureBlit = (TexBlit_t)0x0045a240;
typedef void(__cdecl* TexAlphaBlit_t)(char*, int);
static const TexAlphaBlit_t TextureAlphaBlit = (TexAlphaBlit_t)0x0045a290;
typedef uint8_t(__cdecl* TexGrab_t)(int, Canvas*);
static const TexGrab_t TextureGrab = (TexGrab_t)0x0045a300;
typedef int(__cdecl* IntInt_t)(int);
static const IntInt_t TextureGetSize = (IntInt_t)0x0045a3e0;
typedef uint8_t(__cdecl* TcAdd_t)(const char*, int, int*);
static const TcAdd_t tc_add = (TcAdd_t)0x0045a5d0;
typedef int(__cdecl* TcLookup_t)(const char*, int);
static const TcLookup_t tc_lookup = (TcLookup_t)0x0045a6a0;
typedef uint8_t(__cdecl* TcRemove_t)(int);
static const TcRemove_t tc_remove = (TcRemove_t)0x0045a6f0;
static const CanvasByte_t gxGrabScreen = (CanvasByte_t)0x0044e1c0;
static const Void_t gxReleaseScreen = (Void_t)0x0044e210;
typedef TexInfo*(__cdecl* NewInfo_t)();
static const NewInfo_t new_sys_info = (NewInfo_t)0x0045fcd0;
typedef uint8_t(__cdecl* TxLoadSystem_t)(char*, TexInfo**);
static const TxLoadSystem_t txLoadSystem = (TxLoadSystem_t)0x0045fd10;
typedef uint8_t(__cdecl* TxCreateSystem_t)(TexInfo**, int);
static const TxCreateSystem_t txCreateSystem = (TxCreateSystem_t)0x0045ff30;
typedef uint8_t(__cdecl* TxLoadVideo_t)(TexInfo*, TexInfo**);
static const TxLoadVideo_t txLoadVideo = (TxLoadVideo_t)0x0045ffe0;
typedef void(__cdecl* TxInfo_t)(TexInfo*);
static const TxInfo_t txUnloadSystem = (TxInfo_t)0x004600a0, txUnloadVideo = (TxInfo_t)0x00460110,
                      txSelect = (TxInfo_t)0x004601d0, destroy_texture_surface = (TxInfo_t)0x00460520,
                      unload_from_video = (TxInfo_t)0x00460690, set_color_key = (TxInfo_t)0x004606a0,
                      load_texmap_mip = (TxInfo_t)0x00460820;
typedef void(__cdecl* TxReload_t)(TexInfo*, TexInfo*);
static const TxReload_t txReloadVideo = (TxReload_t)0x00460180;
typedef void(__cdecl* TxRevert_t)(char*, TexInfo*);
static const TxRevert_t txRevert = (TxRevert_t)0x004601c0, load_texmap = (TxRevert_t)0x00460710,
                        txAlphaBlit = (TxRevert_t)0x00460980;
typedef void(__cdecl* TxBlit_t)(TexInfo*, TexInfo*, uint32_t, uint32_t, uint32_t, uint32_t);   // floats as bits
static const TxBlit_t txBlit = (TxBlit_t)0x00460240;
typedef uint8_t(__cdecl* CreateSurf_t)(int, int, int, uint8_t, uint8_t, TexInfo*);
static const CreateSurf_t create_texture_surface = (CreateSurf_t)0x004603b0;
typedef uint8_t(__cdecl* TxGrab_t)(TexInfo*, Canvas*);
static const TxGrab_t txGrab = (TxGrab_t)0x00460540;
typedef uint8_t(__cdecl* TxHasAlpha_t)(TexInfo*);
static const TxHasAlpha_t txHasAlpha = (TxHasAlpha_t)0x00460600;
typedef int(__cdecl* TxGetSize_t)(TexInfo*);
static const TxGetSize_t txGetSize = (TxGetSize_t)0x00460630;
typedef uint8_t(__cdecl* LoadIntoVideo_t)(TexInfo*, TexInfo*);
static const LoadIntoVideo_t load_into_video = (LoadIntoVideo_t)0x00460660;
typedef void(__cdecl* AlphaBlitTexmap_t)(TexInfo*, uint8_t*);
static const AlphaBlitTexmap_t alpha_blit_texmap = (AlphaBlitTexmap_t)0x004609b0;
static const Byte_t texture_find_formats = (Byte_t)0x00460aa0;
static const uint32_t k_texture_format_callback = 0x00460d50;         // passed to EnumTextureFormats as the original's
typedef int(__cdecl* CountBits_t)(int);
static const CountBits_t count_bits = (CountBits_t)0x00460ee0;
typedef TexInfo*(__cdecl* MapToInfo_t)(TexInfo*, void*);
static const MapToInfo_t map_to_info = (MapToInfo_t)0x00460f00;
typedef TexInfo*(__cdecl* MapToId_t)(TexInfo*);
static const MapToId_t map_to_id = (MapToId_t)0x00460f10;
typedef uint8_t*(__cdecl* TexGetFile_t)(const char*);
static const TexGetFile_t texGet = (TexGetFile_t)0x00461160, load_file = (TexGetFile_t)0x00461790;
typedef void(__cdecl* TexForget_t)(uint8_t*);
static const TexForget_t texForget = (TexForget_t)0x00461170, unload_file = (TexForget_t)0x00461830;
typedef uint8_t*(__cdecl* MipLevel_t)(const uint8_t*, int, int*);
static const MipLevel_t get_mip_level = (MipLevel_t)0x00461180;
typedef void(__cdecl* TexTransfer_t)(uint8_t*, Canvas*);
static const TexTransfer_t texTransfer = (TexTransfer_t)0x004613b0, texAlphaBlit = (TexTransfer_t)0x00461470;

// ---- fixes (docs/PORTING.md, "Fixes") ---------------------------------------------------------------------------------
// FIX: texture names of 16 characters and more. tc_add strcpy's the name into the entry's 16-byte field (+0x08),
// after the key, flags and copies are set, so a long name overwrites them (see tc_add): a 16-character keyed name is
// never found again, 21+ crash the first time they're drawn, 24+ reload a garbage name. The full name is kept here,
// keyed by the entry, and the entry's 16 bytes hold a mark instead: 0x01, a serial in 4 hex digits and the name's first
// 10 characters, NUL-terminated. No texture name has a 0x01, so the mark matches nothing the game asks for; every
// reader of the name (tc_lookup, TextureRestoreAll / BeginFrame's reloads, TextureEnd / Destroy's reports,
// TextureRevert) asks entry_name() for the full one. A record is trusted only while the entry still holds its mark:
// an entry written by an original function (viperport.ini, or a shadow check keeping the original's result) is read
// as its own bytes, as the original reads them. Names up to 15 characters are stored exactly as the original stores
// them. The records are DLL memory, not the game's: no footprint lists them, and a shadow check neither saves nor
// compares them (the mark check makes a stale record harmless). Textures live on the main thread: no lock.
namespace {
struct LongTexName { const uint8_t* entry; char mark[16]; char* name; };
}  // namespace
static LongTexName* g_long_tex;
static int g_nlong_tex, g_cap_long_tex;
static uint32_t g_long_tex_serial;
// the entry's name as TextureGet / TextureCreate were given it
static const char* entry_name(const uint8_t* e) {
    const char* n = (const char*)(e + 8);
    if (!VP_FIX || n[0] != 1) return n;
    for (int i = 0; i < g_nlong_tex; i++)
        if (g_long_tex[i].entry == e && !memcmp(g_long_tex[i].mark, n, 16)) return g_long_tex[i].name;
    return n;
}
// tc_add: a new entry's long name (the mark in the entry, the name kept here)
static void set_long_tex_name(uint8_t* e, const char* name) {
    char mark[16];
    static const char hex[] = "0123456789abcdef";
    const uint32_t serial = ++g_long_tex_serial;
    memset(mark, 0, 16);
    mark[0] = 1;
    for (int k = 0; k < 4; k++) mark[1 + k] = hex[(serial >> (12 - 4 * k)) & 15];
    for (int k = 0; k < 10 && name[k]; k++) mark[5 + k] = name[k];
    memcpy(e + 8, mark, 16);
    const size_t len = strlen(name);
    char* full = (char*)malloc(len + 1);
    if (!full) return;                              // (no memory: the texture just isn't found again, like the original)
    memcpy(full, name, len + 1);
    int i = 0;
    while (i < g_nlong_tex && g_long_tex[i].entry != e) i++;       // the entry's old record, reused
    if (i == g_nlong_tex) {
        if (g_nlong_tex == g_cap_long_tex) {
            const int cap = g_cap_long_tex ? g_cap_long_tex * 2 : 16;
            LongTexName* t = (LongTexName*)realloc(g_long_tex, cap * sizeof *t);
            if (!t) { free(full); return; }
            g_long_tex = t;
            g_cap_long_tex = cap;
        }
        g_nlong_tex++;
    } else {
        free(g_long_tex[i].name);
    }
    g_long_tex[i].entry = e;
    memcpy(g_long_tex[i].mark, mark, 16);
    g_long_tex[i].name = full;
}
// tc_add: a short name in an entry that had a long one: its record goes
static void forget_long_tex_name(const uint8_t* e) {
    for (int i = 0; i < g_nlong_tex; i++)
        if (g_long_tex[i].entry == e) {
            free(g_long_tex[i].name);
            g_long_tex[i] = g_long_tex[--g_nlong_tex];
            return;
        }
}
// FIX: the log formats into 256 bytes (and overruns them): a name the fix lets through at any length is reported cut
// to 128 characters
static const char* log_name(const char* s, char (&tmp)[0x88]) {
    if (!VP_FIX || strlen(s) <= 0x80) return s;
    memcpy(tmp, s, 0x80);
    memcpy(tmp + 0x80, "...", 4);
    return tmp;
}

// ---- footprint helpers ------------------------------------------------------------------------------------------------
// the table's capacity (tc_add's count: 250, or M1's 1000) and entry i, as the function's operand at `op` for the field
// at offset `fo` addresses it
static int32_t tex_capacity() { return (int32_t)m1_operand(0x0045a60c); }
static bool id_ok(int32_t i) { return i >= 0 && i < tex_capacity(); }
// tc_lookup's walk, read only: the entry a (name, key) finds, or -1
static int32_t fp_lookup(const char* name, int32_t key) {
    const int32_t n = tex_capacity();
    TexEntry* t = (TexEntry*)(uintptr_t)m1_operand(0x0045a6ab);
    for (int32_t i = 0; i < n; i++)
        if (t[i].in_use && crt_stricmp(entry_name((const uint8_t*)&t[i]), name) == 0 && t[i].key == key) return i;
    return -1;
}
static int32_t fp_first_free() {
    const int32_t n = tex_capacity();
    TexEntry* t = (TexEntry*)(uintptr_t)m1_operand(0x0045a5f6);
    for (int32_t i = 0; i < n; i++)
        if (!t[i].in_use) return i;
    return -1;
}
static TexEntry* fp_table() { return (TexEntry*)(uintptr_t)m1_operand(0x0045a6ab); }
static void fp_dtex(Footprint& f, const TexInfo* t, const char* what) {
    if (t && t->dtex) f.add(t->dtex, sizeof(DTexture), what);
}
static void fp_dx_result(Footprint& f) {                 // dx_result, and dx_error's name for an unknown code
    f.add((void*)X_DX_RESULT, 4, "dx_result");
    f.add((void*)X_DX_ERRBUF, 0x20, "dx_name_error's buffer");
}
static void fp_canvas_pixels(Footprint& f, const Canvas* c, const char* what) {
    if (!c || !c->bits || c->h <= 0 || c->pitch == 0) return;
    int64_t n = (int64_t)c->pitch * c->h;
    if (n < 0) f.add(c->bits + n + (-c->pitch), (uint32_t)(-n), what);
    else f.add(c->bits, (uint32_t)n, what);
}
// the pool element PoolBase::alloc will hand out (its head), and the pool itself
static void fp_pool_alloc(Footprint& f, void* pool) {
    if (!pool) return;
    f.add(pool, 0x20, "texture_info pool");
    void* head = *(void**)((uint8_t*)pool + 0x10);
    if (head) f.add(head, sizeof(TexInfo), "the texture_info it hands out");
}

// =====================================================================================================================
// gx.obj: the gx* API. Each takes the gx lock (_SingleEnter(gx_sync, 0, 0) .. _SingleLeave) around the call.
// =====================================================================================================================

// gxBegin (0x44deb0)
static void __cdecl gxBegin_rw() {
    GI(X_GX_SYNC) = SingleBegin(S(0x004eefac));                  // "gx"
    vid_begin();
    gfx_begin();
    ExceptSinglePrecision(1);
}
static void fp_gx_begin(Footprint& f) { f.replay_only = "start-up: makes the gx lock, and vid_begin / gfx_begin make the display"; }
PORT_FN(0x0044deb0, "gxBegin", gxBegin_rw, fp_gx_begin)

// gxEnd (0x44dee0)
static void __cdecl gxEnd_rw() {
    gfx_end();
    vid_end();
    SingleEnd(GI(X_GX_SYNC), 0, 0);
}
static void fp_gx_end(Footprint& f) { f.replay_only = "shut-down: frees the display and the gx lock"; }
PORT_FN(0x0044dee0, "gxEnd", gxEnd_rw, fp_gx_end)

// gxRestore (0x44df00): after Alt-Tab (Win32Idle) -- every video copy dropped, the system copies kept
static void __cdecl gxRestore_rw() {
    TextureReleaseAll(0);
    TextureRestoreAll(0);
}
static void fp_release_all(Footprint& f, uint8_t sys);
static void fp_gx_restore(Footprint& f) { fp_release_all(f, 0); }
PORT_FN(0x0044df00, "gxRestore", gxRestore_rw, fp_gx_restore)

// the screen size of a mode (1 512x384 -- which also asks VidSetMode for its flag --, 2 640x480, 3 800x600,
// 4 1024x768); anything else panics "unknown video mode" and leaves the size alone
static uint8_t set_screen_size(int mode, uint32_t panic_fmt) {
    uint8_t bl = 0;
    switch ((uint32_t)(mode - 1)) {
    case 0: GI(X_SCREEN_W) = 0x200; GI(X_SCREEN_H) = 0x180; bl = 1; break;
    case 1: GI(X_SCREEN_W) = 0x280; GI(X_SCREEN_H) = 0x1e0; break;
    case 2: GI(X_SCREEN_W) = 0x320; GI(X_SCREEN_H) = 0x258; break;
    case 3: GI(X_SCREEN_W) = 0x400; GI(X_SCREEN_H) = 0x300; break;
    default: LogPanic(S(panic_fmt)); break;
    }
    return bl;
}

// gxSetMode (0x44df20)
static void __cdecl gxSetMode_rw(int mode) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    if (mode != GI(X_MODE)) {
        const uint8_t bl = set_screen_size(mode, 0x004eefb0);
        VidSetMode(mode, bl);
        pal_build_system();
        mr_set_mode();
        GI(X_MODE) = mode;
    }
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_gx_set_mode(Footprint& f, int mode) {
    if (mode != GI(X_MODE)) f.replay_only = "changes the display mode (VidSetMode makes the surfaces)";
}
PORT_FN(0x0044df20, "gxSetMode", gxSetMode_rw, fp_gx_set_mode)

// gxRestoreMode (0x44e000)
static void __cdecl gxRestoreMode_rw() {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    mr_restore_mode();
    VidRestoreMode();
    GI(X_MODE) = 0;
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_gx_restore_mode(Footprint& f) { f.replay_only = "restores the desktop's display mode (frees the surfaces)"; }
PORT_FN(0x0044e000, "gxRestoreMode", gxRestoreMode_rw, fp_gx_restore_mode)

// gxChangeMode (0x44e040): as gxSetMode, releasing and restoring the 3D around it; then clears the new screen
static void __cdecl gxChangeMode_rw(int mode) {
    Canvas c;
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    if (mode != GI(X_MODE)) {
        const uint8_t bl = set_screen_size(mode, 0x004eefc4);
        mr_release();
        VidSetMode(mode, bl);
        mr_restore();
        GI(X_MODE) = mode;
    }
    SingleLeave(GI(X_GX_SYNC), 0, 0);
    if (gxGrabScreen(&c)) {
        gxClear(G32(X_BLACK));
        gxReleaseScreen();
    }
}
static void fp_gx_change_mode(Footprint& f, int) { f.replay_only = "changes the display mode, and grabs the screen (VidGetPage runs Win32Idle)"; }
PORT_FN(0x0044e040, "gxChangeMode", gxChangeMode_rw, fp_gx_change_mode)

// gxReady (0x44e150)
static uint8_t __cdecl gxReady_rw() {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    const uint8_t r = VidReady();
    SingleLeave(GI(X_GX_SYNC), 0, 0);
    return r;
}
static void fp_gx_ready(Footprint& f) {
    if (GI(X_VID_STATE) <= 2) f.replay_only = "VidReady pumps the window messages (Win32Idle)";
}
PORT_FN(0x0044e150, "gxReady", gxReady_rw, fp_gx_ready)

// gxFlip (0x44e180)
static void __cdecl gxFlip_rw() {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    VidFlip();
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_gx_flip(Footprint& f) { f.replay_only = "VidFlip pumps the window messages (Win32Idle)"; }
PORT_FN(0x0044e180, "gxFlip", gxFlip_rw, fp_gx_flip)

// gxDisableSecondary (0x44e1b0): a jump to VidDisableSecondary
static void __cdecl gxDisableSecondary_rw() { VidDisableSecondary(); }
static void fp_gx_disable_secondary(Footprint& f) { f.add((void*)X_SECONDARY, 1, "VidDisableSecondary's flag"); }
PORT_FN(0x0044e1b0, "gxDisableSecondary", gxDisableSecondary_rw, fp_gx_disable_secondary)

// gxGrabScreen (0x44e1c0)
static uint8_t __cdecl gxGrabScreen_rw(Canvas* c) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    const uint8_t r = VidGetPage(c);
    if (r) gxSetCanvas(c);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
    return r;
}
static void fp_gx_grab_screen(Footprint& f, Canvas*) { f.replay_only = "VidGetPage pumps the window messages (Win32Idle)"; }
PORT_FN(0x0044e1c0, "gxGrabScreen", gxGrabScreen_rw, fp_gx_grab_screen)

// gxReleaseScreen (0x44e210)
static void __cdecl gxReleaseScreen_rw() {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    VidReleasePage();
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_nothing(Footprint&) {}
PORT_FN(0x0044e210, "gxReleaseScreen", gxReleaseScreen_rw, fp_nothing)

// gxGetTexture (0x44e240) .. gxGetTextureSize (0x44e540): the Texture* functions under the gx lock
static int __cdecl gxGetTexture_rw(char* name, int key) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    const int r = TextureGet(name, key);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
    return r;
}
static void fp_texture_get(Footprint& f, const char* name, int key);
static void fp_gx_get_texture(Footprint& f, char* name, int key) { fp_texture_get(f, name, key); }
PORT_FN(0x0044e240, "gxGetTexture", gxGetTexture_rw, fp_gx_get_texture)

static void __cdecl gxForgetTexture_rw(int id) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureForget(id);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_texture_forget(Footprint& f, int id);
PORT_FN(0x0044e280, "gxForgetTexture", gxForgetTexture_rw, fp_texture_forget)

static int __cdecl gxCreateTexture_rw(char* name, int key, int size) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    const int r = TextureCreate(name, key, size);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
    return r;
}
static void fp_texture_create(Footprint& f, const char* name, int key, int);
static void fp_gx_create_texture(Footprint& f, char* name, int key, int size) { fp_texture_create(f, name, key, size); }
PORT_FN(0x0044e2c0, "gxCreateTexture", gxCreateTexture_rw, fp_gx_create_texture)

static void __cdecl gxDestroyTexture_rw(int id) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureDestroy(id);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_texture_destroy(Footprint& f, int id);
PORT_FN(0x0044e310, "gxDestroyTexture", gxDestroyTexture_rw, fp_texture_destroy)

static void __cdecl gxFreeAllTextures_rw() {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureFreeAll();
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_texture_free_all(Footprint& f);
PORT_FN(0x0044e350, "gxFreeAllTextures", gxFreeAllTextures_rw, fp_texture_free_all)

// the four floats go straight on to TextureBlit (mov / push): bits
static void __cdecl gxBlitTexture_rw(int src, int dst, uint32_t u0, uint32_t v0, uint32_t u1, uint32_t v1) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureBlit(src, dst, u0, v0, u1, v1);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_texture_blit(Footprint& f, int src, int dst, uint32_t, uint32_t, uint32_t, uint32_t);
PORT_FN(0x0044e380, "gxBlitTexture", gxBlitTexture_rw, fp_texture_blit)

static void __cdecl gxAlphaBlitTexture_rw(char* name, int id) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureAlphaBlit(name, id);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_texture_alpha_blit(Footprint& f, char*, int id);
PORT_FN(0x0044e3d0, "gxAlphaBlitTexture", gxAlphaBlitTexture_rw, fp_texture_alpha_blit)

static void __cdecl gxReloadTexture_rw(int id) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureReloadVideo(id);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_dx_only(Footprint& f, int) { fp_dx_result(f); }
PORT_FN(0x0044e410, "gxReloadTexture", gxReloadTexture_rw, fp_dx_only)

static void __cdecl gxUnloadTexture_rw(int id) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureUnloadVideo(id);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_texture_unload(Footprint& f, int id);
PORT_FN(0x0044e450, "gxUnloadTexture", gxUnloadTexture_rw, fp_texture_unload)

static uint8_t __cdecl gxGrabTexture_rw(int id, Canvas* c) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    const uint8_t r = TextureGrab(id, c);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
    return r;
}
static void fp_texture_grab(Footprint& f, int id, Canvas* c);
PORT_FN(0x0044e490, "gxGrabTexture", gxGrabTexture_rw, fp_texture_grab)

static void __cdecl gxReleaseTexture_rw() {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureRelease();
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
static void fp_texture_release(Footprint& f);
PORT_FN(0x0044e4d0, "gxReleaseTexture", gxReleaseTexture_rw, fp_texture_release)

static void __cdecl gxRevertTexture_rw(int id) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    TextureRevert(id);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
}
PORT_FN(0x0044e500, "gxRevertTexture", gxRevertTexture_rw, fp_dx_only)

static int __cdecl gxGetTextureSize_rw(int id) {
    SingleEnter(GI(X_GX_SYNC), 0, 0);
    const int r = TextureGetSize(id);
    SingleLeave(GI(X_GX_SYNC), 0, 0);
    return r;
}
static void fp_nothing_i(Footprint&, int) {}
PORT_FN(0x0044e540, "gxGetTextureSize", gxGetTextureSize_rw, fp_nothing_i)

// =====================================================================================================================
// texture.obj: the texture cache. An id is an index into the texture table (-1: none). Every table address below is
// the operand of the original's own instruction (m1_operand): the comment names the field it addresses in entry 0.
// The loops are the original's do-whiles (the end test at the bottom).
// =====================================================================================================================

// TextureBegin (0x459f80)
static uint8_t __cdecl TextureBegin_rw(int, int, int) {
    G8(X_DERES) = 0;
    txBegin();
    txConserveMode(0);
    tc_clear();
    return 1;
}
static void fp_texture_begin(Footprint& f, int, int, int) { f.replay_only = "txBegin allocates the texture_info pools"; }
PORT_FN(0x00459f80, "TextureBegin", TextureBegin_rw, fp_texture_begin)

// TextureEnd (0x459fa0): reports and frees every entry still in use
static void __cdecl TextureEnd_rw() {
    // mov esi, table -- in the hook's 5 bytes: read from TextureRestoreAll's (0x45a099), the same table base
    uint8_t* e = (uint8_t*)(uintptr_t)m1_operand_hooked(0x00459fa2, 0x00459fa0, 0x56, 0x0045a099, 0);
    do {
        TexEntry* t = (TexEntry*)e;
        if (t->in_use == 1) {
            char tmp[0x88];
            LogReport(S(0x004f3278), log_name(entry_name(e), tmp));   // "freeing unreleased texture elements: %s"
            if (TexInfo* v = t->vid) txUnloadVideo(v);
            if (TexInfo* s = t->sys) txUnloadSystem(s);
            t->in_use = 0;
        }
        e += 0x28;
    } while ((uintptr_t)e < m1_operand(0x00459fe4));             // the table's end
    tc_clear();
    txEnd();
}
static void fp_texture_end(Footprint& f) { f.replay_only = "frees every texture and the pools"; }
PORT_FN(0x00459fa0, "TextureEnd", TextureEnd_rw, fp_texture_end)

// TextureFreeAll (0x45a000)
static void __cdecl TextureFreeAll_rw() {
    int32_t i = 0;
    uint8_t* p = (uint8_t*)(uintptr_t)m1_operand(0x0045a006);    // &entry.sys
    do {
        if (p[-0x20] == 1) {
            TextureUnload(i);
            if (TexInfo* s = *(TexInfo* volatile*)p) txUnloadSystem(s);
            *(TexInfo* volatile*)(p + 4) = 0;                   // vid
            *(TexInfo* volatile*)p = 0;                         // sys
            *(volatile uint8_t*)(p - 0x20) = 0;                 // in_use
        }
        p += 0x28;
        i++;
    } while ((uintptr_t)p < m1_operand(0x0045a038));             // the end + 0x20
    tc_clear();
}
static void fp_texture_free_all(Footprint& f) {
    TexEntry* t = fp_table();
    const int32_t n = tex_capacity();
    for (int32_t i = 0; i < n; i++)
        if (t[i].in_use == 1 && (t[i].loaded || t[i].sys)) { f.replay_only = "frees the textures in use"; return; }
    f.add(t, (uint32_t)n * 0x28, "the texture table");
}
PORT_FN(0x0045a000, "TextureFreeAll", TextureFreeAll_rw, fp_texture_free_all)

// TextureReleaseAll (0x45a050): every video copy dropped; with `sys`, the system copies freed too (sys left dangling)
static void __cdecl TextureReleaseAll_rw(uint8_t sys) {
    int32_t i = 0;
    uint8_t* p = (uint8_t*)(uintptr_t)m1_operand(0x0045a056);    // &entry.sys
    do {
        if (p[-0x20] == 1) {
            TextureUnload(i);
            if (sys)
                if (TexInfo* s = *(TexInfo* volatile*)p) txUnloadSystem(s);
        }
        p += 0x28;
        i++;
    } while ((uintptr_t)p < m1_operand(0x0045a086));             // the end + 0x20
}
static void fp_release_all(Footprint& f, uint8_t sys) {
    TexEntry* t = fp_table();
    const int32_t n = tex_capacity();
    for (int32_t i = 0; i < n; i++)
        if (t[i].in_use == 1 && (t[i].loaded || (sys && t[i].sys))) { f.replay_only = "frees the textures' copies"; return; }
}
PORT_FN(0x0045a050, "TextureReleaseAll", TextureReleaseAll_rw, fp_release_all)

// TextureRestoreAll (0x45a090): with `sys`, every entry's system copy loaded again from its file
static void __cdecl TextureRestoreAll_rw(uint8_t sys) {
    if (!sys) return;
    uint8_t* e = (uint8_t*)(uintptr_t)m1_operand(0x0045a099);    // the table
    do {
        if (e[0] == 1) txLoadSystem((char*)entry_name(e), (TexInfo**)(e + 0x20));   // FIX: (entry_name) the full name
        e += 0x28;
    } while ((uintptr_t)e < m1_operand(0x0045a0b7));             // the end
}
static void fp_restore_all(Footprint& f, uint8_t sys) {
    if (!sys) return;
    TexEntry* t = fp_table();
    const int32_t n = tex_capacity();
    for (int32_t i = 0; i < n; i++)
        if (t[i].in_use == 1) { f.replay_only = "loads the textures' files and makes their surfaces"; return; }
}
PORT_FN(0x0045a090, "TextureRestoreAll", TextureRestoreAll_rw, fp_restore_all)

// TextureGet (0x45a0c0): the id of (name, key), loaded the first time
static int __cdecl TextureGet_rw(const char* name, int key) {
    if (*name == 0) return -1;
    int id;                                                     // (tc_add always sets it)
    if (tc_add(name, key, &id)) txLoadSystem((char*)name, (TexInfo**)(uintptr_t)TE_AT(m1_operand(0x0045a0f8), id));   // &entry.sys
    return id;
}
static void fp_texture_get(Footprint& f, const char* name, int key) {
    if (*name == 0) return;
    const int32_t i = fp_lookup(name, key);
    if (i < 0) { f.replay_only = "a texture not yet in the cache: loads its file and makes its surface"; return; }
    f.add(&fp_table()[i], 0x28, "the texture's entry");
}
PORT_FN(0x0045a0c0, "TextureGet", TextureGet_rw, fp_texture_get)

// TextureForget (0x45a110): one reference less; the last frees both copies
static void __cdecl TextureForget_rw(int id) {
    if (id == -1) return;
    if (!tc_remove(id)) return;
    if (TexInfo* v = TEP(m1_operand(0x0045a134), id)) {                      // entry.vid
        txUnloadVideo(v);
        TEP(m1_operand(0x0045a144), id) = 0;
    }
    if (TexInfo* s = TEP(m1_operand(0x0045a151), id)) {                      // entry.sys
        txUnloadSystem(s);
        TEP(m1_operand(0x0045a161), id) = 0;
    }
}
static void fp_texture_forget(Footprint& f, int id) {
    if (id == -1) return;
    if (!id_ok(id)) { f.replay_only = "an id outside the texture table"; return; }
    TexEntry* t = &fp_table()[id];
    if (t->refs == 1 && (t->vid || t->sys)) { f.replay_only = "the last reference: frees the texture's surfaces"; return; }
    f.add(t, 0x28, "the texture's entry");
}
PORT_FN(0x0045a110, "TextureForget", TextureForget_rw, fp_texture_forget)

// TextureCreate (0x45a170): a new, empty system texture of `size` (64 / 128 / 256)
static int __cdecl TextureCreate_rw(const char* name, int key, int size) {
    int id;
    if (tc_add(name, key, &id)) {
        txCreateSystem((TexInfo**)(uintptr_t)TE_AT(m1_operand(0x0045a19e), id), size);   // &entry.sys
        return id;
    }
    char tmp[0x88];
    LogPanic(S(0x004f32a0), log_name(name, tmp));                // "already there: can't create texture: %s"
    return id;
}
static void fp_texture_create(Footprint& f, const char* name, int key, int) {
    const int32_t i = fp_lookup(name, key);
    if (i < 0) { f.replay_only = "makes the texture's surface"; return; }
    f.add(&fp_table()[i], 0x28, "the texture's entry");
}
PORT_FN(0x0045a170, "TextureCreate", TextureCreate_rw, fp_texture_create)

// TextureDestroy (0x45a1d0): TextureCreate's counterpart; panics unless it was the last reference
static void __cdecl TextureDestroy_rw(int id) {
    if (tc_remove(id)) {
        if (TexInfo* v = TEP(m1_operand(0x0045a1ef), id)) {                  // entry.vid
            txUnloadVideo(v);
            TEP(m1_operand(0x0045a1ff), id) = 0;
        }
        if (TexInfo* s = TEP(m1_operand(0x0045a20c), id)) {                  // entry.sys
            txUnloadSystem(s);
            TEP(m1_operand(0x0045a21f), id) = 0;
        }
        return;
    }
    char tmp[0x88];
    const uint8_t* e = (const uint8_t*)(uintptr_t)TE_AT(m1_operand(0x0045a22c), id) - 8;   // entry.name's entry
    LogPanic(S(0x004f32c8), log_name(entry_name(e), tmp));       // "not last texture: can't destroy texture: %s"
}
static void fp_texture_destroy(Footprint& f, int id) {
    if (!id_ok(id)) { f.replay_only = "an id outside the texture table"; return; }
    TexEntry* t = &fp_table()[id];
    if (t->refs == 1 && (t->vid || t->sys)) { f.replay_only = "frees the texture's surfaces"; return; }
    f.add(t, 0x28, "the texture's entry");
}
PORT_FN(0x0045a1d0, "TextureDestroy", TextureDestroy_rw, fp_texture_destroy)

// FIX: an entry without a system copy (sys NULL: its txLoadSystem / txCreateSystem failed -- the surface couldn't be
// made -- or a free entry) crashed whatever used it: TextureSelect (txLoadVideo(NULL)), TextureHasAlpha (every frame
// the texture is drawn late), TextureGetSize, TextureBlit, TextureAlphaBlit, TextureGrab. Such an entry now behaves as
// no texture (id -1): not selected (0), no alpha, size 4, nothing blitted, not grabbed.
// TextureBlit (0x45a240): src's system copy BltFast'ed into dst's, level by level (txBlit); the floats go on as bits
static void __cdecl TextureBlit_rw(int src, int dst, uint32_t u0, uint32_t v0, uint32_t u1, uint32_t v1) {
    if (src == -1 || dst == -1) return;
    TexInfo* d = TEP(m1_operand(0x0045a26f), dst);                           // entry.sys (dst's read first)
    TexInfo* s = TEP(m1_operand(0x0045a277), src);
    if (VP_FIX && (!s || !d)) return;                                         // FIX: no system copy: nothing to blit
    txBlit(s, d, u0, v0, u1, v1);
}
static void fp_texture_blit(Footprint& f, int src, int dst, uint32_t, uint32_t, uint32_t, uint32_t) {
    if (src == -1 || dst == -1) return;
    if (!id_ok(src) || !id_ok(dst)) { f.replay_only = "an id outside the texture table"; return; }
    fp_dtex(f, fp_table()[src].sys, "the source's dtexture (mip walk)");
    fp_dtex(f, fp_table()[dst].sys, "the destination's dtexture (mip walk)");
    fp_dx_result(f);
}
PORT_FN(0x0045a240, "TextureBlit", TextureBlit_rw, fp_texture_blit)

// TextureAlphaBlit (0x45a290): the .tex `name` alpha-pasted over every level of the texture's system copy
static void __cdecl TextureAlphaBlit_rw(char* name, int id) {
    if (id == -1) return;
    TexInfo* s = TEP(m1_operand(0x0045a2a3), id);                            // entry.sys
    if (VP_FIX && !s) return;                                                 // FIX: no system copy: nothing to blit
    txAlphaBlit(name, s);
}
static void fp_texture_alpha_blit(Footprint& f, char*, int id) {
    if (id != -1) f.replay_only = "fetches the .tex resource (texGet: ResourceGetDiscardable may load its file)";
}
PORT_FN(0x0045a290, "TextureAlphaBlit", TextureAlphaBlit_rw, fp_texture_alpha_blit)

// TextureReloadVideo (0x45a2c0): the system copy's top level Blt'ed to the video copy, if loaded
static void __cdecl TextureReloadVideo_rw(int id) {
    if (id == -1) return;
    if (TE8(m1_operand(0x0045a2d1), id) == 0) return;                        // entry.loaded
    TexInfo* v = TEP(m1_operand(0x0045a2da), id);                            // entry.vid
    txReloadVideo(TEP(m1_operand(0x0045a2e0), id), v);                       // entry.sys
}
PORT_FN(0x0045a2c0, "TextureReloadVideo", TextureReloadVideo_rw, fp_dx_only)

// TextureUnloadVideo (0x45a2f0)
static void __cdecl TextureUnloadVideo_rw(int id) { TextureUnload(id); }
PORT_FN(0x0045a2f0, "TextureUnloadVideo", TextureUnloadVideo_rw, fp_texture_unload)

// TextureGrab (0x45a300): a canvas over the system copy's top level (locked until TextureRelease)
static uint8_t __cdecl TextureGrab_rw(int id, Canvas* c) {
    if (id == -1) return 0;
    TexInfo* s = TEP(m1_operand(0x0045a31c), id);                            // entry.sys
    if (VP_FIX && !s) return 0;                         // FIX: no system copy: not grabbed (and no grab to release)
    GI(X_GRAB_ID) = id;
    return txGrab(s, c);
}
static void fp_tx_grab(Footprint& f, TexInfo*, Canvas* c);
static void fp_texture_grab(Footprint& f, int id, Canvas* c) {
    if (id == -1) return;
    f.add((void*)X_GRAB_ID, 4, "the grabbed id");
    fp_tx_grab(f, 0, c);
}
PORT_FN(0x0045a300, "TextureGrab", TextureGrab_rw, fp_texture_grab)

// TextureRelease (0x45a330)
static void __cdecl TextureRelease_rw() {
    if (GI(X_GRAB_ID) != -1) {
        txRelease();
        const int32_t id = GI(X_GRAB_ID);
        if (TE8(m1_operand(0x0045a34b), id) != 0) {                          // entry.loaded
            TexInfo* v = TEP(m1_operand(0x0045a354), id);                     // entry.vid
            txReloadVideo(TEP(m1_operand(0x0045a35a), id), v);                // entry.sys
        }
        GI(X_GRAB_ID) = -1;
        return;
    }
    LogReport(S(0x004f32f4));                                    // "releasing no texture?"
}
static void fp_texture_release(Footprint& f) {
    f.add((void*)X_GRAB_ID, 4, "the grabbed id");
    f.add((void*)X_GRABBED, 4, "the grabbed texture_info");
    fp_dx_result(f);
}
PORT_FN(0x0045a330, "TextureRelease", TextureRelease_rw, fp_texture_release)

// TextureRevert (0x45a390): txRevert does nothing; the video copy reloaded
static void __cdecl TextureRevert_rw(int id) {
    if (id == -1) return;
    TexInfo* s = TEP(m1_operand(0x0045a3a0), id);                            // entry.sys
    txRevert((char*)entry_name((const uint8_t*)(uintptr_t)TE_AT(m1_operand(0x0045a3ae), id) - 8), s);   // entry.name (FIX: full)
    if (TE8(m1_operand(0x0045a3bd), id) != 0) {                              // entry.loaded
        TexInfo* v = TEP(m1_operand(0x0045a3c6), id);                         // entry.vid
        txReloadVideo(TEP(m1_operand(0x0045a3cc), id), v);                    // entry.sys
    }
}
PORT_FN(0x0045a390, "TextureRevert", TextureRevert_rw, fp_dx_only)

// TextureGetSize (0x45a3e0)
static int __cdecl TextureGetSize_rw(int id) {
    if (id == -1) return 4;
    TexInfo* s = TEP(m1_operand(0x0045a3f5), id);                            // entry.sys
    if (VP_FIX && !s) return 4;                                               // FIX: no system copy: as id -1
    return txGetSize(s);
}
PORT_FN(0x0045a3e0, "TextureGetSize", TextureGetSize_rw, fp_nothing_i)

// TextureWillDeresNextFrame (0x45a410)
static uint8_t __cdecl TextureWillDeresNextFrame_rw() { return G8(X_DERES); }
PORT_FN(0x0045a410, "TextureWillDeresNextFrame", TextureWillDeresNextFrame_rw, fp_nothing)

// TextureBeginFrame (0x45a420): out of video memory last frame -- every copy dropped, the system copies reloaded one
// conserve level smaller (the sys pointers are left dangling by the first loop and reused by the second)
static void __cdecl TextureBeginFrame_rw() {
    if (G8(X_DERES) == 0) return;
    LogReport(S(0x004f330c));                                    // "out of texture space, de-rezing!"
    int32_t i = 0;
    uint8_t* p = (uint8_t*)(uintptr_t)m1_operand(0x0045a434);    // &entry.sys
    do {
        if (p[-0x20] != 0) {
            TextureUnload(i);
            if (TexInfo* s = *(TexInfo* volatile*)p) txUnloadSystem(s);
        }
        p += 0x28;
        i++;
    } while ((uintptr_t)p < m1_operand(0x0045a464));
    p = (uint8_t*)(uintptr_t)m1_operand(0x0045a46d);             // &entry.sys
    txConserveMode(1);
    do {
        if (p[-0x20] != 0 && *(TexInfo* volatile*)p != 0)
            txLoadSystem((char*)entry_name(p - 0x20), (TexInfo**)p);   // FIX: (entry_name) the full name
        p += 0x28;
    } while ((uintptr_t)p < m1_operand(0x0045a496));
    G8(X_DERES) = 0;
}
static void fp_texture_begin_frame(Footprint& f) {
    if (G8(X_DERES)) f.replay_only = "de-rezes: frees and reloads every texture";
}
PORT_FN(0x0045a420, "TextureBeginFrame", TextureBeginFrame_rw, fp_texture_begin_frame)

// TextureEndFrame (0x45a4b0): a bare ret
static void __cdecl TextureEndFrame_rw() {}
PORT_FN(0x0045a4b0, "TextureEndFrame", TextureEndFrame_rw, fp_nothing)

// TextureFlush (0x45a4c0)
static void __cdecl TextureFlush_rw() {
    G8(X_DERES) = 0;
    txConserveMode(0);
}
static void fp_texture_flush(Footprint& f) {
    f.add((void*)X_DERES, 1, "the de-rez flag");
    f.add((void*)X_CONSERVE, 4, "the conserve level");
}
PORT_FN(0x0045a4c0, "TextureFlush", TextureFlush_rw, fp_texture_flush)

// TextureSelect (0x45a4e0): the texture made current, its video copy loaded first if need be; a failed load (or a
// de-rez already pending) sets the de-rez flag and answers 0
static uint8_t __cdecl TextureSelect_rw(int id) {
    if (id == -1) return 0;
    if (TE8(m1_operand(0x0045a4fa), id) == 0) {                              // entry.loaded
        if (G8(X_DERES) != 0) goto fail;
        {
            TexInfo** vid = (TexInfo**)(uintptr_t)TE_AT(m1_operand(0x0045a50c), id);   // &entry.vid
            TexInfo* s = TEP(m1_operand(0x0045a512), id);                            // entry.sys
            if (VP_FIX && !s) return 0;                     // FIX: no system copy: not selected (no de-rez either)
            if (!txLoadVideo(s, vid)) goto fail;
        }
        TE8(m1_operand(0x0045a526), id) = 1;
    }
    txSelect(TEP(m1_operand(0x0045a52d), id));                                // entry.vid
    return 1;
fail:
    G8(X_DERES) = 1;
    return 0;
}
static void fp_texture_select(Footprint& f, int id) {
    if (id == -1) return;
    if (!id_ok(id)) { f.replay_only = "an id outside the texture table"; return; }
    if (fp_table()[id].loaded) { fp_dx_result(f); return; }
    if (G8(X_DERES)) { f.add((void*)X_DERES, 1, "the de-rez flag"); return; }
    if (VP_FIX && !fp_table()[id].sys) return;                  // (FIX: no system copy: nothing happens)
    f.replay_only = "makes the texture's video copy";
}
PORT_FN(0x0045a4e0, "TextureSelect", TextureSelect_rw, fp_texture_select)

// TextureUnload (0x45a550): the video copy dropped
static void __cdecl TextureUnload_rw(int id) {
    if (id == -1) return;
    if (TE8(m1_operand(0x0045a566), id) == 0) return;                        // entry.loaded
    txUnloadVideo(TEP(m1_operand(0x0045a56f), id));                           // entry.vid
    TEP(m1_operand(0x0045a57b), id) = 0;
    TE8(m1_operand(0x0045a585), id) = 0;
}
static void fp_texture_unload(Footprint& f, int id) {
    if (id == -1) return;
    if (!id_ok(id)) { f.replay_only = "an id outside the texture table"; return; }
    if (fp_table()[id].loaded) f.replay_only = "frees the texture's video copy";
}
PORT_FN(0x0045a550, "TextureUnload", TextureUnload_rw, fp_texture_unload)

// TextureHasAlpha (0x45a590)
static uint8_t __cdecl TextureHasAlpha_rw(int id) {
    if (id == -1) return 0;
    TexInfo* s = TEP(m1_operand(0x0045a5a2), id);                            // entry.sys
    if (VP_FIX && !s) return 0;                                               // FIX: no system copy: as id -1
    return txHasAlpha(s);
}
PORT_FN(0x0045a590, "TextureHasAlpha", TextureHasAlpha_rw, fp_nothing_i)

// tc_clear (0x45a5b0)
static void __cdecl tc_clear_rw() {
    // mov eax, table -- in the hook's 5 bytes: read from TextureRestoreAll's (0x45a099), the same table base
    uint8_t* e = (uint8_t*)(uintptr_t)m1_operand_hooked(0x0045a5b1, 0x0045a5b0, 0xb8, 0x0045a099, 0);
    do {
        *(volatile uint8_t*)e = 0;
        e += 0x28;
    } while ((uintptr_t)e < m1_operand(0x0045a5bc));
}
static void fp_tc_clear(Footprint& f) { f.add(fp_table(), (uint32_t)tex_capacity() * 0x28, "the texture table"); }
PORT_FN(0x0045a5b0, "tc_clear", tc_clear_rw, fp_tc_clear)

// tc_add (0x45a5d0): the entry of (name, key), counted once more; a new one in the first free entry (answering 1)
static uint8_t __cdecl tc_add_rw(const char* name, int key, int* out) {
    uint8_t bl = 0;
    int32_t esi = tc_lookup(name, key);
    if (esi == -1) {
        int32_t edx = 0;
        uint8_t* e = (uint8_t*)(uintptr_t)m1_operand(0x0045a5f6);  // the table
        do {
            if (*e == 0) break;
            e += 0x28;
            edx++;
        } while ((uintptr_t)e < m1_operand(0x0045a604));
        if (edx < (int32_t)m1_operand(0x0045a60c)) {             // cmp edx, 250 (M1: 1000)
            bl = 1;
            TE8(m1_operand(0x0045a621), edx) = bl;                            // entry.in_use
            TE32(m1_operand(0x0045a630), edx) = 0;                            // entry.refs
            TE32(m1_operand(0x0045a636), edx) = key;                          // entry.key
            TEP(m1_operand(0x0045a641), edx) = 0;                             // entry.sys
            TEP(m1_operand(0x0045a647), edx) = 0;                             // entry.vid
            TE8(m1_operand(0x0045a64d), edx) = 0;                             // entry.loaded
            // The original strcpy's the name, unbounded, into the 16-byte field at entry+0x08, AFTER the key, flags and
            // copies above were set. Its terminator lands at +0x08 + len:
            //  * 16 characters: the NUL is the key's low byte (+0x18). tc_lookup (stricmp on +0x08, then the key)
            //    still matches the name, but the key only if its low byte was 0 -- so a keyed texture (a car's damage
            //    copy: key car + 1 / car + 0x20) is never found again: every TextureGet adds and loads another entry.
            //  * 17..19: the key's low bytes are name characters: never found again, for any key.
            //  * 20: the NUL lands on `loaded` (+0x1c, already 0). 21..23: `loaded` becomes a character: TextureSelect
            //    skips txLoadVideo and calls txSelect(vid = NULL) -- a crash (0x4601f3) the first time it's drawn.
            //  * 24+: sys (+0x20) is name bytes until txLoadSystem stores the real pointer over them, which also cuts
            //    the stored name to 24 characters and the pointer's bytes: TextureRestoreAll(1) / a de-rez reload a
            //    garbage name. 28+: vid (+0x24) is name bytes. 32+: into the next entry.
            char* field = (char*)(uintptr_t)TE_AT(m1_operand(0x0045a662), edx);   // entry.name
            // FIX: a name of 16+ characters: kept whole in the DLL, a mark in the field (set_long_tex_name)
            if (VP_FIX && strlen(name) >= 16) {
                set_long_tex_name((uint8_t*)field - 8, name);
            } else {
                gxt_strcpy(field, name);
                if (VP_FIX && g_nlong_tex) forget_long_tex_name((const uint8_t*)field - 8);
            }
            esi = edx;
        } else {
            LogPanic(S(0x004f3330));                             // "out of texture cache entries" (never returns)
        }
    }
    TE32(m1_operand(0x0045a68c), esi)++;                                      // entry.refs (the panic's -1: entry -1, if it returned)
    *out = esi;
    return bl;
}
static void fp_tc_add(Footprint& f, const char* name, int key, int* out) {
    f.add(out, 4, "the id");
    int32_t i = fp_lookup(name, key);
    if (i >= 0) { f.add(&fp_table()[i], 0x28, "the entry"); return; }
    i = fp_first_free();
    if (i < 0) return;                                          // full: the panic
    const uint32_t len = 8 + (uint32_t)strlen(name) + 1;         // the name's copy runs on past a long name's entry
    f.add(&fp_table()[i], len > 0x28 && !VP_FIX ? len : 0x28, "the new entry (and what a long name overruns)");
}
PORT_FN(0x0045a5d0, "tc_add", tc_add_rw, fp_tc_add)

// tc_lookup (0x45a6a0): the first in-use entry whose name matches (stricmp) and key equals; -1 when the walk reaches
// the count (250; M1: 1000)
static int __cdecl tc_lookup_rw(const char* name, int key) {
    int32_t ebx = 0;
    uint8_t* e = (uint8_t*)(uintptr_t)m1_operand(0x0045a6ab);    // the table
    do {
        if (*e != 0 && crt_stricmp(entry_name(e), name) == 0 && *(volatile int32_t*)(e + 0x18) == key) break;   // FIX: full
        e += 0x28;
        ebx++;
    } while ((uintptr_t)e < m1_operand(0x0045a6d5));
    if (ebx == (int32_t)m1_operand(0x0045a6dd)) ebx = -1;        // cmp ebx, 250 (M1: 1000)
    return ebx;
}
static void fp_nothing_si(Footprint&, const char*, int) {}
PORT_FN(0x0045a6a0, "tc_lookup", tc_lookup_rw, fp_nothing_si)

// tc_remove (0x45a6f0): one reference less; 1 when it was the last (the entry freed); a panic below 0
static uint8_t __cdecl tc_remove_rw(int id) {
    const int32_t r = TE32(m1_operand(0x0045a6fa), id) - 1;                  // entry.refs
    TE32(m1_operand(0x0045a708), id) = r;
    if (r == 0) {
        TE8(m1_operand(0x0045a710), id) = 0;                                  // entry.in_use
        return 1;
    }
    if (r > 0) return 0;
    LogPanic(S(0x004f3350));                                     // "too many removes from cache entry"
    return 0;
}
static void fp_tc_remove(Footprint& f, int id) {
    if (!id_ok(id)) { f.replay_only = "an id outside the texture table"; return; }
    f.add(&fp_table()[id], 0x28, "the entry");
}
PORT_FN(0x0045a6f0, "tc_remove", tc_remove_rw, fp_tc_remove)

// =====================================================================================================================
// tmap.obj: the texture_info pools and every DirectDraw / Direct3D texture operation (through dd.obj's wrappers).
// map_to_info / map_to_id are identities, called as the original calls them.
// =====================================================================================================================

// txBegin (0x45fbd0): the format table, then the two pools (200 / 128 x 0x28; M1: 1024 each)
static void __cdecl txBegin_rw() {
    texture_find_formats();
    void* p = MemAlloc(0x20);
    if (p) {
        PoolBase_ctor(p, 0, S(0x004f3ea0), (int)m1_operand(0x0045fbeb), 0x28);   // 'sys texture pool'
        *(volatile uint32_t*)p = 0x004dd07c;                     // Pool<texture_info>'s vtable
        GP(void, X_SYS_POOL) = p;
    } else {
        GP(void, X_SYS_POOL) = 0;
    }
    p = MemAlloc(0x20);
    if (p) {
        PoolBase_ctor(p, 0, S(0x004f3ec8), (int)m1_operand(0x0045fc26), 0x28);   // 'vid texture pool'
        *(volatile uint32_t*)p = 0x004dd07c;
        GP(void, X_VID_POOL) = p;
    } else {
        GP(void, X_VID_POOL) = 0;
    }
    G32(X_VID_LIST) = 0;
    G32(X_SYS_LIST) = 0;
    G32(X_GRABBED) = 0;
    G32(X_CONSERVE_BASE) = 0;
    G32(X_CONSERVE) = 0;
}
static void fp_tx_begin(Footprint& f) { f.replay_only = "allocates the texture_info pools"; }
PORT_FN(0x0045fbd0, "txBegin", txBegin_rw, fp_tx_begin)

// txEnd (0x45fc70): the pools deleted through their vtables
static void __cdecl txEnd_rw() {
    typedef void*(__fastcall* Sdd_t)(void*, Edx, uint32_t);
    if (void* p = GP(void, X_SYS_POOL)) (*(Sdd_t*)*(void**)p)(p, 0, 1);
    GP(void, X_SYS_POOL) = 0;
    if (void* p = GP(void, X_VID_POOL)) (*(Sdd_t*)*(void**)p)(p, 0, 1);
    GP(void, X_VID_POOL) = 0;
}
static void fp_tx_end(Footprint& f) { f.replay_only = "frees the texture_info pools"; }
PORT_FN(0x0045fc70, "txEnd", txEnd_rw, fp_tx_end)

// txConserveMode (0x45fcb0): on, one level more to drop; off, back to the base
static void __cdecl txConserveMode_rw(uint8_t on) {
    if (on) GI(X_CONSERVE)++;
    else GI(X_CONSERVE) = GI(X_CONSERVE_BASE);
}
static void fp_tx_conserve(Footprint& f, uint8_t) { f.add((void*)X_CONSERVE, 4, "the conserve level"); }
PORT_FN(0x0045fcb0, "txConserveMode", txConserveMode_rw, fp_tx_conserve)

// new_sys_info (0x45fcd0): a texture_info from the sys pool, defaulted (a 128 x 128, 7 levels; next left as it was)
static TexInfo* __cdecl new_sys_info_rw() {
    TexInfo* t = (TexInfo*)PoolBase_alloc(GP(void, X_SYS_POOL), 0);
    volatile TexInfo* v = t;
    v->b0 = 0;
    v->fmt = 0;
    v->size = 0x80;
    v->levels = 7;
    v->tt = 0;
    v->tfmt = 0;
    v->mips = 0;
    v->wrap = 0;
    v->deres = 0;
    v->key = 0;
    v->file_levels = 8;
    v->dtex = 0;
    return t;
}
static void fp_new_sys_info(Footprint& f) { fp_pool_alloc(f, GP(void, X_SYS_POOL)); }
PORT_FN(0x0045fcd0, "new_sys_info", new_sys_info_rw, fp_new_sys_info)

// txLoadSystem (0x45fd10): the .tex file loaded into a new system-memory surface (its levels per the caps, the VRAM
// and the conserve level)
static uint8_t __cdecl txLoadSystem_rw(char* name, TexInfo** out) {
    char* n = name;
    if (*n == '=') n++;
    if (GI(X_MRCAPS_ALPHA) == 1 && crt_stricmp(n, S(0x004f3edc)) == 0) {   // "effects.tex"
        n = (char*)S(0x004f3ee8);                                          // "effectsx.tex"
        LogReport(S(0x004f3ef8));                                          // "tx: using effectsx.tex"
    }
    int32_t ebp = 1;
    TexInfo* esi = new_sys_info();
    volatile TexInfo* vi = esi;
    int32_t edi = 0;
    int32_t size = 0x80;
    if (crt_strstr(n, S(0x004f3f10)) == 0 && crt_strstr(n, S(0x004f3f18)) == 0) {   // ".tex" / ".TEX"
        char tmp[0x88];
        LogReport(S(0x004f3f40), log_name(n, tmp));                             // "bad texture name: %s"
    } else {
        vi->tt = texGet(n);
        vi->tfmt = vi->tt[0];
        vi->mips = vi->tt[1];
        vi->wrap = vi->tt[2];
        vi->deres = vi->tt[3];
        vi->key = *(volatile uint16_t*)(vi->tt + 4);
        vi->file_levels = *(volatile int32_t*)(vi->tt + 8);
        uint8_t* tt = vi->tt;
        ebp = *(volatile int32_t*)(tt + 8);
        if (tt[3] == 2 && VidGetMegsRam() < 8) ebp--;
        if (GI(X_CONSERVE) != 0) {
            int32_t c = GI(X_CONSERVE);
            if (vi->tt[3] != 0) c--;
            if (c > 0) {
                ebp -= c;
                if (ebp < 4) ebp = 4;
            }
        }
        switch ((uint32_t)(int32_t)(int8_t)vi->tt[0]) {
        case 0: case 1: case 3: edi = 0; break;
        case 2: case 4: edi = 2; break;
        default: LogPanic(S(0x004f3f20)); break;                 // "tmap: unknown texture format"
        }
        size = (int32_t)(1u << ((uint32_t)(ebp - 1) & 31));
        if (vi->tt[1] == 0) ebp = 1;
    }
    if (G8(X_HW_MIPS) == 0) ebp = 1;
    if (G8(X_FEAT_MIP) == 0) ebp = 1;
    if (edi == 2 && (GI(X_MRCAPS_ALPHA) == 0 || G8(X_FORMATS + 0x50) == 0)) edi = 0;   // no alpha format: solid
    if (create_texture_surface(size, ebp, edi, 1, 0, esi)) {
        TexInfo* id = map_to_id(esi);
        load_texmap(n, id);
        if (uint8_t* tt = vi->tt) {
            texForget(tt);
            vi->tt = 0;
        }
        vi->next = GP(TexInfo, X_SYS_LIST);
        GP(TexInfo, X_SYS_LIST) = esi;
        *out = id;
        return 1;
    }
    char tmp[0x88];
    LogReport(S(0x004f3f58), log_name(n, tmp));                  // "can't create system texture surface: %s"
    return 0;
}
static void fp_tx_load_system(Footprint& f, char*, TexInfo**) { f.replay_only = "loads the .tex resource and makes its surface"; }
PORT_FN(0x0045fd10, "txLoadSystem", txLoadSystem_rw, fp_tx_load_system)

// txCreateSystem (0x45ff30): an empty system texture, one level, solid (64 / 128 / 256)
static uint8_t __cdecl txCreateSystem_rw(TexInfo** out, int size) {
    TexInfo* t = new_sys_info();
    volatile TexInfo* v = t;
    v->tt = 0;
    v->key = 0;
    v->tfmt = 0;
    if (size == 0x100) v->file_levels = 9;
    else if (size == 0x80) v->file_levels = 8;
    else if (size == 0x40) v->file_levels = 7;
    else LogPanic(S(0x004f3f80));                                // "hmm, not used as intended?"
    if (create_texture_surface(size, 1, 0, 1, 0, t)) {
        TexInfo* id = map_to_id(t);
        v->next = GP(TexInfo, X_SYS_LIST);
        GP(TexInfo, X_SYS_LIST) = t;
        *out = id;
        return 1;
    }
    LogReport(S(0x004f3f9c));                                    // "can't create system texture surface"
    return 0;
}
static void fp_tx_create_system(Footprint& f, TexInfo**, int) { f.replay_only = "makes a system texture surface"; }
PORT_FN(0x0045ff30, "txCreateSystem", txCreateSystem_rw, fp_tx_create_system)

// txLoadVideo (0x45ffe0): a video copy of the system texture (a new surface, IDirect3DTexture2::Load)
static uint8_t __cdecl txLoadVideo_rw(TexInfo* sys, TexInfo** out) {
    TexInfo* s = map_to_info(sys, GP(void, X_SYS_POOL));
    TexInfo* v = (TexInfo*)PoolBase_alloc(GP(void, X_VID_POOL), 0);
    volatile TexInfo* vs = s;
    volatile TexInfo* vv = v;
    vv->tt = vs->tt;
    volatile uint32_t* d = (volatile uint32_t*)&vv->tfmt;         // +0x14..+0x1f, as three dwords
    volatile uint32_t* c = (volatile uint32_t*)&vs->tfmt;
    d[0] = c[0];
    d[1] = c[1];
    d[2] = c[2];
    if (create_texture_surface(vs->size, vs->levels, vs->fmt, 0, 1, v)) {
        if (load_into_video(s, v)) {
            vv->next = GP(TexInfo, X_VID_LIST);
            GP(TexInfo, X_VID_LIST) = v;
            *out = map_to_id(v);
            return 1;
        }
        LogReport(S(0x004f3fc0));                                // "failed loading texture into vram"
        destroy_texture_surface(v);
    }
    PoolBase_free(GP(void, X_VID_POOL), 0, v);
    return 0;
}
static void fp_tx_load_video(Footprint& f, TexInfo*, TexInfo**) { f.replay_only = "makes the video texture surface"; }
PORT_FN(0x0045ffe0, "txLoadVideo", txLoadVideo_rw, fp_tx_load_video)

// txUnloadSystem (0x4600a0): off the sys list (a texture not on it links the list's last element to its next), the
// surface destroyed, the tt_texture forgotten, back to the pool
static void __cdecl txUnloadSystem_rw(TexInfo* t) {
    TexInfo* s = map_to_info(t, GP(void, X_SYS_POOL));
    TexInfo* prev = 0;
    TexInfo* cur = GP(TexInfo, X_SYS_LIST);
    if (cur != 0) {
        while (cur != s) {
            prev = cur;
            cur = ((volatile TexInfo*)cur)->next;
            if (cur == 0) break;
        }
    }
    TexInfo* next = ((volatile TexInfo*)s)->next;
    if (prev) ((volatile TexInfo*)prev)->next = next;
    else GP(TexInfo, X_SYS_LIST) = next;
    destroy_texture_surface(s);
    if (uint8_t* tt = ((volatile TexInfo*)s)->tt) texForget(tt);
    PoolBase_free(GP(void, X_SYS_POOL), 0, s);
}
static void fp_tx_unload_system(Footprint& f, TexInfo*) { f.replay_only = "frees the texture's surface"; }
PORT_FN(0x004600a0, "txUnloadSystem", txUnloadSystem_rw, fp_tx_unload_system)

// txUnloadVideo (0x460110): only a texture on the vid list is unloaded
static void __cdecl txUnloadVideo_rw(TexInfo* t) {
    TexInfo* v = map_to_info(t, GP(void, X_VID_POOL));
    TexInfo* prev = 0;
    TexInfo* cur = GP(TexInfo, X_VID_LIST);
    if (cur != 0) {
        while (cur != v) {
            prev = cur;
            cur = ((volatile TexInfo*)cur)->next;
            if (cur == 0) break;
        }
    }
    if (cur == 0) return;
    TexInfo* next = ((volatile TexInfo*)v)->next;
    if (prev) ((volatile TexInfo*)prev)->next = next;
    else GP(TexInfo, X_VID_LIST) = next;
    unload_from_video(v);
    destroy_texture_surface(v);
    PoolBase_free(GP(void, X_VID_POOL), 0, v);
}
static void fp_tx_unload_video(Footprint& f, TexInfo* t) {
    for (TexInfo* c = GP(TexInfo, X_VID_LIST); c; c = c->next)
        if (c == t) { f.replay_only = "frees the video texture surface"; return; }
}
PORT_FN(0x00460110, "txUnloadVideo", txUnloadVideo_rw, fp_tx_unload_video)

// txReloadVideo (0x460180): the system copy's top level Blt'ed to the video copy
static void __cdecl txReloadVideo_rw(TexInfo* sys, TexInfo* vid) {
    TexInfo* s = map_to_info(sys, GP(void, X_SYS_POOL));
    TexInfo* v = map_to_info(vid, GP(void, X_VID_POOL));
    DTexture* src = (DTexture*)((volatile TexInfo*)s)->dtex;
    dsurf_blit((DTexture*)((volatile TexInfo*)v)->dtex, 0, src);
}
static void fp_tx_reload_video(Footprint& f, TexInfo*, TexInfo*) { fp_dx_result(f); }
PORT_FN(0x00460180, "txReloadVideo", txReloadVideo_rw, fp_tx_reload_video)

// txRevert (0x4601c0): a bare ret
static void __cdecl txRevert_rw(char*, TexInfo*) {}
static void fp_nothing_ct(Footprint&, char*, TexInfo*) {}
PORT_FN(0x004601c0, "txRevert", txRevert_rw, fp_nothing_ct)

// txSelect (0x4601d0): the video texture's handle and wrap mode (.tex byte 2: 0 wrap / wrap, 1 wrap / clamp,
// 2 clamp / wrap, 3 clamp / clamp; anything else panics and wraps)
static void __cdecl txSelect_rw(TexInfo* vid) {
    uint8_t u = 1, v = 1;
    TexInfo* t = map_to_info(vid, GP(void, X_VID_POOL));
    const uint8_t w = ((volatile TexInfo*)t)->wrap;
    if (w != 0) {
        if (w == 3) { u = 0; v = 0; }
        else if (w == 1) v = 0;
        else if (w == 2) u = 0;
        else LogPanic(S(0x004f3fe4));                            // "unknown wrap mode"
    }
    dtex_select((DTexture*)((volatile TexInfo*)t)->dtex, 0, u, v);
}
static void fp_tx_select(Footprint& f, TexInfo*) { fp_dx_result(f); }
PORT_FN(0x004601d0, "txSelect", txSelect_rw, fp_tx_select)

// txBlit (0x460240): src's levels BltFast'ed into dst's, the rect (u0, v0, u1, v1) x the level's size (chopped),
// starting where the source's level is no bigger than the destination's, halving until a level or a rect is empty
static void __cdecl txBlit_rw(TexInfo* src, TexInfo* dst, float u0, float v0, float u1, float v1) {
    TexInfo* S_ = map_to_info(src, GP(void, X_SYS_POOL));
    TexInfo* D_ = map_to_info(dst, GP(void, X_SYS_POOL));
    volatile TexInfo* s = S_;
    volatile TexInfo* d = D_;
    int32_t edi = d->size;
    int32_t count = d->levels;
    DTexture* esi = dtex_first_mip((DTexture*)s->dtex, 0);
    DTexture* dmip = dtex_first_mip((DTexture*)d->dtex, 0);
    int32_t ebx = s->size;
    if (d->size < ebx) {
        for (;;) {
            if (esi == 0) goto done;
            if (ebx <= edi) break;
            esi = dtex_next_mip((DTexture*)s->dtex, 0);
            ebx = ebx / 2;
            if (esi == 0) break;
        }
    }
    if (esi == 0) goto done;
    for (;;) {
        if (dmip == 0) goto done;
        {
            int32_t rect[4];
            const int32_t l = gxt_ftol_mul(edi, &u0);
            const int32_t t = gxt_ftol_mul(edi, &v0);
            const int32_t r = gxt_ftol_mul(edi, &u1);
            const int32_t b = gxt_ftol_mul(edi, &v1);
            rect[0] = l;
            rect[1] = t;
            rect[2] = r;
            rect[3] = b;
            if (r == l || b == t) goto done;
            dsurf_blit_fast(dmip, 0, esi, (uint32_t)l, (uint32_t)t, rect);
        }
        count--;
        edi = edi / 2;
        if (edi <= 0 || count <= 0) goto done;
        esi = dtex_next_mip((DTexture*)s->dtex, 0);
        dmip = dtex_next_mip((DTexture*)d->dtex, 0);
        if (esi == 0) goto done;
    }
done:
    dtex_cleanup_mip((DTexture*)s->dtex, 0);
    dtex_cleanup_mip((DTexture*)d->dtex, 0);
}
static void fp_tx_blit(Footprint& f, TexInfo* src, TexInfo* dst, float, float, float, float) {
    fp_dtex(f, src, "the source's dtexture (mip walk)");
    fp_dtex(f, dst, "the destination's dtexture (mip walk)");
    fp_dx_result(f);
}
PORT_FN(0x00460240, "txBlit", txBlit_rw, fp_tx_blit)

// create_texture_surface (0x4603b0): a DDSURFACEDESC for the texture (system memory, or ALLOCONLOAD video memory; a
// colour key for .tex formats 1 / 3 unless it's a 3Dfx system copy; a mip chain with hardware mips), then the dtexture
// NB: the key's 555 test indexes the format table by the .tex format byte (1 or 3), not by `format` -- 3 reads past
// the table's 3 entries (0x526724, the blue clear colour's high word); set_color_key sets the right key later.
static uint8_t __cdecl create_texture_surface_rw(int size, int levels, int format, uint8_t sys, uint8_t vid, TexInfo* t) {
    uint32_t desc[27];
    const uint32_t pf = X_FORMATS + 4 + (uint32_t)format * 0x28u;   // the format's DDPIXELFORMAT
    for (int i = 0; i < 27; i++) ((volatile uint32_t*)desc)[i] = 0;
    desc[1] = 0x1007;                                            // dwFlags: CAPS | HEIGHT | WIDTH | PIXELFORMAT
    desc[0] = 0x6c;
    desc[26] = 0x1000;                                           // ddsCaps: TEXTURE
    if (vid) desc[26] = 0x4001000;                               // | ALLOCONLOAD
    if (sys) desc[26] |= 0x800;                                  // SYSTEMMEMORY
    else if (VidHas3DHW()) desc[26] |= 0x4000;                   // VIDEOMEMORY
    uint8_t is3dfx = 0;
    if (sys) is3dfx = VidIs3DFX() ? 1 : 0;
    volatile TexInfo* vt = t;
    const uint8_t tf = vt->tfmt;
    if ((tf == 1 || tf == 3) && !is3dfx) {
        uint16_t key = vt->key;
        desc[1] |= 0x10000;                                      // CKSRCBLT
        if (FMT_GXTYPE((int32_t)(int8_t)tf) == 3) key = (uint16_t)((key & 0x1f) | ((uint16_t)(key & 0xffc0) >> 1));
        desc[16] = key;                                          // ddckCKSrcBlt
        desc[17] = key;
    }
    if (G8(X_HW_MIPS) != 0) {
        desc[1] |= 0x20000;                                      // MIPMAPCOUNT
        desc[26] |= 0x400008;                                    // MIPMAP | COMPLEX
        desc[6] = (uint32_t)levels;
    }
    desc[3] = (uint32_t)size;                                    // dwWidth
    desc[2] = (uint32_t)size;                                    // dwHeight
    for (int i = 0; i < 8; i++) desc[18 + i] = ((volatile uint32_t*)(uintptr_t)pf)[i];   // ddpfPixelFormat
    DTexture* dt = ddraw_create_texture_surface(GP(void, X_DD), 0, desc);
    if (dt) {
        vt->size = size;
        vt->levels = levels;
        vt->fmt = format;
        vt->dtex = dt;
        return 1;
    }
    return 0;
}
static void fp_create_texture_surface(Footprint& f, int, int, int, uint8_t, uint8_t, TexInfo*) { f.replay_only = "allocates the dtexture"; }
PORT_FN(0x004603b0, "create_texture_surface", create_texture_surface_rw, fp_create_texture_surface)

// destroy_texture_surface (0x460520)
static void __cdecl destroy_texture_surface_rw(TexInfo* t) {
    void* dd = GP(void, X_DD);
    ddraw_destroy_texture_surface(dd, 0, (DTexture*)((volatile TexInfo*)t)->dtex);
    ((volatile TexInfo*)t)->dtex = 0;
}
static void fp_destroy_texture_surface(Footprint& f, TexInfo* t) {
    if (t->dtex) f.replay_only = "frees the dtexture";
    else f.add(&t->dtex, 4, "texture_info.dtex");
}
PORT_FN(0x00460520, "destroy_texture_surface", destroy_texture_surface_rw, fp_destroy_texture_surface)

// txGrab (0x460540): the top level locked, a canvas over it; a failed lock gives an empty canvas
static uint8_t __cdecl txGrab_rw(TexInfo* sys, Canvas* c) {
    TexInfo* t = map_to_info(sys, GP(void, X_SYS_POOL));
    uint32_t desc[27];
    for (int i = 0; i < 27; i++) ((volatile uint32_t*)desc)[i] = 0;
    desc[0] = 0x6c;
    volatile TexInfo* v = t;
    if (dsurf_lock((DTexture*)v->dtex, 0, desc)) {
        gxBuildCanvas(c, (int8_t)FMT_GXTYPE(v->fmt), (void*)(uintptr_t)desc[9], v->size, v->size, (int)desc[4]);
        GP(TexInfo, X_GRABBED) = t;
        return 1;
    }
    gxBuildCanvas(c, 0, 0, 0, 0, 0);
    return 0;
}
static void fp_tx_grab(Footprint& f, TexInfo*, Canvas* c) {
    f.add(c, sizeof(Canvas), "the canvas");
    f.add((void*)X_GRABBED, 4, "the grabbed texture_info");
    fp_dx_result(f);
}
PORT_FN(0x00460540, "txGrab", txGrab_rw, fp_tx_grab)

// txRelease (0x4605e0)
static void __cdecl txRelease_rw() {
    TexInfo* t = GP(TexInfo, X_GRABBED);
    GP(TexInfo, X_GRABBED) = 0;
    dsurf_unlock((DTexture*)((volatile TexInfo*)t)->dtex, 0);
}
static void fp_tx_release(Footprint& f) { f.add((void*)X_GRABBED, 4, "the grabbed texture_info"); }
PORT_FN(0x004605e0, "txRelease", txRelease_rw, fp_tx_release)

// txHasAlpha (0x460600): an ARGB4444 texture (gx type 1)
static uint8_t __cdecl txHasAlpha_rw(TexInfo* sys) {
    TexInfo* t = map_to_info(sys, GP(void, X_SYS_POOL));
    return (uint8_t)(FMT_GXTYPE(((volatile TexInfo*)t)->fmt) - 1) < 1 ? 1 : 0;
}
static void fp_nothing_t(Footprint&, TexInfo*) {}
PORT_FN(0x00460600, "txHasAlpha", txHasAlpha_rw, fp_nothing_t)

// txGetSize (0x460630): 1 << (file levels - 1), the count's low byte (shl edx, cl)
static int __cdecl txGetSize_rw(TexInfo* sys) {
    TexInfo* t = map_to_info(sys, GP(void, X_SYS_POOL));
    const uint8_t cl = (uint8_t)(((volatile TexInfo*)t)->file_levels - 1);
    return (int)(1u << (cl & 31));
}
PORT_FN(0x00460630, "txGetSize", txGetSize_rw, fp_nothing_t)

// load_into_video (0x460660): IDirect3DTexture2::Load from the system copy, the keys set again; always 1
static uint8_t __cdecl load_into_video_rw(TexInfo* sys, TexInfo* vid) {
    DTexture* src = (DTexture*)((volatile TexInfo*)sys)->dtex;
    dtex_load((DTexture*)((volatile TexInfo*)vid)->dtex, 0, src);
    set_color_key(vid);
    return 1;
}
static void fp_load_into_video(Footprint& f, TexInfo*, TexInfo* vid) {
    fp_dtex(f, vid, "the video dtexture (handle, mip walk)");
    fp_dx_result(f);
}
PORT_FN(0x00460660, "load_into_video", load_into_video_rw, fp_load_into_video)

// unload_from_video (0x460690): a bare ret
static void __cdecl unload_from_video_rw(TexInfo*) {}
PORT_FN(0x00460690, "unload_from_video", unload_from_video_rw, fp_nothing_t)

// set_color_key (0x4606a0): .tex formats 1 / 3: the key on every level (555: reduced)
static void __cdecl set_color_key_rw(TexInfo* t) {
    volatile TexInfo* v = t;
    const uint8_t tf = v->tfmt;
    if (tf != 1 && tf != 3) return;
    uint16_t key = v->key;
    if (FMT_GXTYPE(v->fmt) == 3) key = (uint16_t)((key & 0x1f) | ((uint16_t)(key & 0xffc0) >> 1));
    for (DTexture* m = dtex_first_mip((DTexture*)v->dtex, 0); m; m = dtex_next_mip((DTexture*)v->dtex, 0))
        dsurf_set_colorkey(m, 0, key);
    dtex_cleanup_mip((DTexture*)v->dtex, 0);
}
static void fp_set_color_key(Footprint& f, TexInfo* t) {
    fp_dtex(f, t, "the dtexture (mip walk)");
    fp_dx_result(f);
}
PORT_FN(0x004606a0, "set_color_key", set_color_key_rw, fp_set_color_key)

// load_texmap (0x460710): a .tex name goes to load_texmap_mip; anything else is obsolete (.bmp, .stp, none): the top
// level cleared blue and a report
static void __cdecl load_texmap_rw(char* name, TexInfo* t) {
    if (crt_strstr(name, S(0x004f3ff8)) || crt_strstr(name, S(0x004f4000))) {   // ".tex" / ".TEX"
        load_texmap_mip(t);
        return;
    }
    Canvas c;
    if (!txGrab(t, &c)) return;
    gxSetCanvas(&c);
    gxClear(G32(X_BLUE));
    if (crt_strstr(name, S(0x004f4008)) || crt_strstr(name, S(0x004f4010))) {   // ".bmp" / ".BMP"
        char tmp[0x88];
        LogReport(S(0x004f4018), log_name(name, tmp));
        txRelease();
        return;
    }
    if (crt_strstr(name, S(0x004f403c)) || crt_strstr(name, S(0x004f4044))) {   // ".stp" / ".STP"
        char tmp[0x88];
        LogReport(S(0x004f404c), log_name(name, tmp));
        txRelease();
        return;
    }
    char tmp[0x88];
    LogReport(S(0x004f4070), log_name(name, tmp));               // "obsolete empty texture support: %s"
    txRelease();
}
static void fp_load_texmap(Footprint& f, char*, TexInfo* t) {
    fp_dtex(f, t, "the dtexture (mip walk)");
    f.stack_ptr((void*)X_CANVAS, "gx's current canvas");
    f.add((void*)X_GRABBED, 4, "the grabbed texture_info");
    fp_dx_result(f);
}
PORT_FN(0x00460710, "load_texmap", load_texmap_rw, fp_load_texmap)

// load_texmap_mip (0x460820): per level of the surface: lock, clear yellow, texTransfer (the file level of the canvas's
// size), unlock; keyed formats get the key on each level
static void __cdecl load_texmap_mip_rw(TexInfo* sys) {
    TexInfo* x = map_to_info(sys, GP(void, X_SYS_POOL));
    volatile TexInfo* v = x;
    int32_t size = v->size;
    int32_t ebx = v->levels;
    uint16_t key = 0;
    if (uint8_t* tt = v->tt) {
        key = *(volatile uint16_t*)(tt + 4);
        if (FMT_GXTYPE(v->fmt) == 3) key = (uint16_t)((key & 0x1f) | ((uint16_t)(key & 0xffc0) >> 1));
    }
    DTexture* m = dtex_first_mip((DTexture*)v->dtex, 0);
    if (m != 0) {
        while (ebx != 0) {
            uint32_t desc[27];
            for (int i = 0; i < 27; i++) ((volatile uint32_t*)desc)[i] = 0;
            desc[0] = 0x6c;
            if (dsurf_lock(m, 0, desc)) {
                Canvas c;
                gxBuildCanvas(&c, (int8_t)FMT_GXTYPE(v->fmt), (void*)(uintptr_t)desc[9], size, size, (int)desc[4]);
                gxSetCanvas(&c);
                gxClear(G32(X_YELLOW));
                texTransfer(v->tt, &c);
                dsurf_unlock(m, 0);
            }
            if (uint8_t* tt = v->tt)
                if (tt[0] == 1 || tt[0] == 3) dsurf_set_colorkey(m, 0, key);
            ebx--;
            size = size / 2;
            m = dtex_next_mip((DTexture*)v->dtex, 0);
            if (m == 0) break;
        }
    }
    dtex_cleanup_mip((DTexture*)v->dtex, 0);
}
static void fp_load_texmap_mip(Footprint& f, TexInfo* t) {
    fp_dtex(f, t, "the dtexture (mip walk)");
    f.stack_ptr((void*)X_CANVAS, "gx's current canvas");
    fp_dx_result(f);
}
PORT_FN(0x00460820, "load_texmap_mip", load_texmap_mip_rw, fp_load_texmap_mip)

// txAlphaBlit (0x460980): the .tex `name` alpha-pasted over every level (the car damage)
static void __cdecl txAlphaBlit_rw(char* name, TexInfo* t) {
    uint8_t* tt = texGet(name);
    alpha_blit_texmap(t, tt);
    texForget(tt);
}
static void fp_tx_alpha_blit(Footprint& f, char*, TexInfo*) { f.replay_only = "fetches the .tex resource (texGet: ResourceGetDiscardable may load its file)"; }
PORT_FN(0x00460980, "txAlphaBlit", txAlphaBlit_rw, fp_tx_alpha_blit)

// alpha_blit_texmap (0x4609b0)
static void __cdecl alpha_blit_texmap_rw(TexInfo* sys, uint8_t* tt) {
    TexInfo* x = map_to_info(sys, GP(void, X_SYS_POOL));
    volatile TexInfo* v = x;
    int32_t size = v->size;
    for (DTexture* m = dtex_first_mip((DTexture*)v->dtex, 0); m != 0;) {
        uint32_t desc[27];
        for (int i = 0; i < 27; i++) ((volatile uint32_t*)desc)[i] = 0;
        desc[0] = 0x6c;
        if (dsurf_lock(m, 0, desc)) {
            Canvas c;
            gxBuildCanvas(&c, (int8_t)FMT_GXTYPE(v->fmt), (void*)(uintptr_t)desc[9], size, size, (int)desc[4]);
            gxSetCanvas(&c);
            texAlphaBlit(tt, &c);
            dsurf_unlock(m, 0);
        }
        size = size / 2;
        m = dtex_next_mip((DTexture*)v->dtex, 0);
    }
    dtex_cleanup_mip((DTexture*)v->dtex, 0);
}
static void fp_alpha_blit_texmap(Footprint& f, TexInfo* t, uint8_t*) {
    fp_dtex(f, t, "the dtexture (mip walk)");
    f.stack_ptr((void*)X_CANVAS, "gx's current canvas");
    fp_dx_result(f);
}
PORT_FN(0x004609b0, "alpha_blit_texmap", alpha_blit_texmap_rw, fp_alpha_blit_texmap)

// texture_find_formats (0x460aa0): EnumTextureFormats fills the format table; the solid format is required
static uint8_t __cdecl texture_find_formats_rw() {
    uint8_t bl = 1;
    d3d_EnumTextureFormats(GP(void, X_D3D), 0, k_texture_format_callback, 0);
    if (G8(X_FORMATS + 0x50) == 0) {                             // alpha
        LogReport(S(0x004f4094));
        bl = 0;
    }
    if (G8(X_FORMATS + 0x28) == 0) {                             // transparent
        LogReport(S(0x004f40bc));
        bl = 0;
    }
    if (G8(X_FORMATS) == 0) {                                    // solid
        LogReport(S(0x004f40ec));
        bl = 0;
        if (G8(X_FORMATS) == 0) {
            LogReport(S(0x004f4114));                            // "Your video card does not support required texture mapping."
            (*(ExitProcess_t volatile*)(uintptr_t)X_IMP_EXITPROCESS)(3);
        }
    }
    return bl;
}
static void fp_texture_find_formats(Footprint& f) {
    // the format table is written by texture_format_callback, which Direct3D calls back from EnumTextureFormats: a
    // rewrite's pass that is handed the call's results instead of making it never sees the callbacks
    f.replay_only = "EnumTextureFormats calls back into the game (texture_format_callback fills the format table)";
}
PORT_FN(0x00460aa0, "texture_find_formats", texture_find_formats_rw, fp_texture_find_formats)

// dump_texture_format (0x460b20): the pixel format's flags and masks to the log (nothing in v1.0 calls it)
static void __cdecl dump_texture_format_rw(uint32_t* pf) {
    // FIX CANDIDATE: the original's buffer is 0x50 bytes on its frame, and the ten flag names total 98 characters: a
    // format with enough flags set overruns into its return address. Here the overrun lands in `spill`, in this
    // frame (unreachable in v1.0: no caller).
    struct { char buf[0x50]; char spill[0x20]; } b;
    const uint32_t fl = pf[1];
    gxt_strcpy(b.buf, S(0x004f4150));                            // ""
    if (fl & 0x80) gxt_strcat(b.buf, S(0x004f4154));             // "(compressed)"
    if (fl & 0x400) gxt_strcat(b.buf, S(0x004f4164));            // "(zbuffer only)"
    if (fl & 0x2) gxt_strcat(b.buf, S(0x004f4174));              // "(alpha only)"
    if (fl & 0x40) gxt_strcat(b.buf, S(0x004f4184));             // "(rgb)"
    if (fl & 0x1) gxt_strcat(b.buf, S(0x004f418c));              // "(alpha)"
    if (fl & 0x800) gxt_strcat(b.buf, S(0x004f4194));            // "(index 1)"
    if (fl & 0x1000) gxt_strcat(b.buf, S(0x004f41a0));           // "(index 2)"
    if (fl & 0x8) gxt_strcat(b.buf, S(0x004f41ac));              // "(index 4)"
    if (fl & 0x10) gxt_strcat(b.buf, S(0x004f41b8));             // "(index to 8)"
    if (fl & 0x20) gxt_strcat(b.buf, S(0x004f41c8));             // "(index 8)"
    LogReport(S(0x004f41d4), b.buf);                             // "texture %s"
    if (((volatile uint8_t*)pf)[4] & 0x40)
        LogReport(S(0x004f41e0), pf[3], pf[4], pf[5], pf[6], pf[7]);   // "bits: %d, r: %x, g: %x, b: %x, a: %x"
}
static void fp_dump_texture_format(Footprint&, uint32_t*) {}
PORT_FN(0x00460b20, "dump_texture_format", dump_texture_format_rw, fp_dump_texture_format)

// texture_format_callback (0x460d50, __stdcall): each RGB format Direct3D offers is matched against 4444 with alpha
// (the alpha format), 1555 (transparent), 565 (solid, gx 4) and x555 (solid gx 3, unless a 565 was found already);
// a match is copied into the table. Always 1: go on.
static long __stdcall texture_format_callback_rw(uint8_t* desc, void*) {
    uint8_t* edi = 0;
    if (!(desc[5] & 0x10)) return 1;                             // DDSD_PIXELFORMAT
    const uint32_t* pf = (const uint32_t*)(desc + 0x48);
    const uint32_t ebx = pf[1];
    if (ebx & 0x1cb8) return 1;                                  // palettised, compressed, z-buffer, ...
    if (!(ebx & 0x40)) return 1;                                 // not RGB
    const uint32_t ebp = pf[3];
    const int32_t a = count_bits((int)pf[7]);
    const int32_t r = count_bits((int)pf[4]);
    const int32_t g = count_bits((int)pf[5]);
    const int32_t b = count_bits((int)pf[6]);
    if (ebx & 1) {                                               // ALPHAPIXELS
        if (ebp != 0x10) goto c565;
        if (r == 4 && g == 4 && b == 4 && a == 4) {
            G8(X_FORMATS + 0x74) = 1;                            // gx type 1: ARGB4444
            edi = (uint8_t*)(uintptr_t)(X_FORMATS + 0x50);
            goto fin;
        }
    }
    if (ebp != 0x10) goto c555;
    if (r == 5 && g == 5 && b == 5 && a == 1) {
        G8(X_FORMATS + 0x4c) = 2;                                // gx type 2: 1555
        edi = (uint8_t*)(uintptr_t)(X_FORMATS + 0x28);
        goto fin;
    }
c565:
    if (ebp != 0x10) goto fin;
    if (r == 5 && g == 6 && b == 5 && pf[4] == 0xf800 && pf[5] == 0x7e0 && pf[6] == 0x1f) {
        G8(X_FORMATS + 0x24) = 4;                                // gx type 4: 565
        edi = (uint8_t*)(uintptr_t)X_FORMATS;
        goto fin;
    }
c555:
    if (ebp != 0x10) goto fin;
    if (r == 5 && g == 5 && b == 5 && pf[4] == 0x7c00 && pf[5] == 0x3e0 && pf[6] == 0x1f) {
        edi = (uint8_t*)(uintptr_t)X_FORMATS;
        if (G8(X_FORMATS) != 0 && G8(X_FORMATS + 0x24) == 4) edi = 0;   // a 565 already: keep it
        else G8(X_FORMATS + 0x24) = 3;                           // gx type 3: 555
    }
fin:
    if (edi) {
        *(volatile uint8_t*)edi = 1;
        for (int i = 0; i < 8; i++) ((volatile uint32_t*)(edi + 4))[i] = pf[i];
    }
    return 1;
}
static void fp_texture_format_callback(Footprint& f, uint8_t*, void*) { f.add((void*)X_FORMATS, 3 * 0x28, "the format table"); }
PORT_FN(0x00460d50, "texture_format_callback", texture_format_callback_rw, fp_texture_format_callback)

// count_bits (0x460ee0): the set bits of x (32 steps of an arithmetic shift)
static int __cdecl count_bits_rw(int x) {
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (x & 1) n++;
        x >>= 1;
    }
    return n;
}
static void fp_pure_i(Footprint& f, int) { f.pure = true; }
PORT_FN(0x00460ee0, "count_bits", count_bits_rw, fp_pure_i)

// map_to_info (0x460f00), map_to_id (0x460f10): identities (texture_info* and Pool* taken as their bits: nothing is
// dereferenced, and the fuzzer can compare what comes back)
static uint32_t __cdecl map_to_info_rw(uint32_t t, uint32_t) { return t; }
static void fp_pure_uu(Footprint& f, uint32_t, uint32_t) { f.pure = true; }
PORT_FN(0x00460f00, "map_to_info", map_to_info_rw, fp_pure_uu)
static uint32_t __cdecl map_to_id_rw(uint32_t t) { return t; }
static void fp_pure_u(Footprint& f, uint32_t) { f.pure = true; }
PORT_FN(0x00460f10, "map_to_id", map_to_id_rw, fp_pure_u)

// Pool<texture_info>::scalar deleting destructor (0x460f20)
static void* __fastcall PoolTexInfo_sdd_rw(void* self, Edx, uint32_t flags) {
    PoolBase_dtor(self, 0);
    if (flags & 1) op_delete(self);
    return self;
}
static void fp_pool_sdd(Footprint& f, void*, Edx, uint32_t) { f.replay_only = "it frees the pool"; }
PORT_FN(0x00460f20, "Pool<texture_info>::scalar deleting destructor", PoolTexInfo_sdd_rw, fp_pool_sdd)

// =====================================================================================================================
// tex.obj: the .tex resource. A tt_texture: +0 format (0 opaque 565, 1 keyed 565, 2 ARGB4444, 3 dual -- keyed 565 then
// an ARGB4444 half whose header says 4 --, 4 the alpha half), +1 use mips, +2 wrap, +3 de-res priority, +4 u16 key,
// +8 level count; the levels from 1x1 up at +0xc, each in a slot of pitch max(size, 4) pixels.
// =====================================================================================================================

// texGet (0x461160), texForget (0x461170)
static uint8_t* __cdecl texGet_rw(const char* name) { return load_file(name); }
static void fp_tex_get(Footprint& f, const char*) { f.replay_only = "fetches the .tex resource (ResourceGetDiscardable may load its file)"; }
PORT_FN(0x00461160, "texGet", texGet_rw, fp_tex_get)
static void __cdecl texForget_rw(uint8_t* tt) { unload_file(tt); }
static void fp_tex_forget(Footprint& f, uint8_t*) { f.replay_only = "forgets the .tex resource (ResourceForget may free it)"; }
PORT_FN(0x00461170, "texForget", texForget_rw, fp_tex_forget)

// get_mip_level (0x461180): the level `size` pixels across, and its pitch in pixels; any other size panics (0)
static uint8_t* __cdecl get_mip_level_rw(const uint8_t* tt, int size, int* pitch) {
    uint32_t off, p;
    switch (size) {
    case 1: off = 0xc; p = 4; break;
    case 2: off = 0x2c; p = 4; break;
    case 4: off = 0x4c; p = 4; break;
    case 8: off = 0x6c; p = 8; break;
    case 0x10: off = 0xec; p = 0x10; break;
    case 0x20: off = 0x2ec; p = 0x20; break;
    case 0x40: off = 0xaec; p = 0x40; break;
    case 0x80: off = 0x2aec; p = 0x80; break;
    case 0x100: off = 0xaaec; p = 0x100; break;
    default:
        LogPanic(S(0x004f4210), size);                           // "get_mip_level: can't handle this size dest: %d"
        return 0;
    }
    *(volatile int*)pitch = (int)p;
    return (uint8_t*)tt + off;
}
static void fp_get_mip_level(Footprint& f, const uint8_t*, int, int* pitch) { f.add(pitch, 4, "the pitch"); }
PORT_FN(0x00461180, "get_mip_level", get_mip_level_rw, fp_get_mip_level)

// texTransfer (0x4613b0): the level of the canvas's width pasted into it (555: reduced); the missing .tex: red
static void __cdecl texTransfer_rw(uint8_t* tt, Canvas* c) {
    if ((uint32_t)(uintptr_t)tt == X_DUMMY) {
        gxSetCanvas(c);
        gxClear(G32(X_RED));
        return;
    }
    const int8_t type = c->type;
    if (type != 4 && type != 1 && type != 3) {
        LogReport(S(0x004f4240), (int)type);                     // "texTransfer: can't handle this format: %d"
        return;
    }
    int pitch = 0;
    uint8_t* bits = get_mip_level(tt, c->w, &pitch);
    Canvas src;
    gxBuildCanvas(&src, c->type, bits, pitch, pitch, pitch + pitch);
    gxSetCanvas(c);
    gxPaste(&src, 0, 0);
    if (c->type == 3) gfxReduceTo555(c);
}
static void fp_tex_transfer(Footprint& f, uint8_t*, Canvas* c) {
    f.stack_ptr((void*)X_CANVAS, "gx's current canvas");
    fp_canvas_pixels(f, c, "the canvas's pixels");
}
PORT_FN(0x004613b0, "texTransfer", texTransfer_rw, fp_tex_transfer)

// texAlphaBlit (0x461470): an ARGB4444 .tex alpha-blended into the canvas at its width's level
static void __cdecl texAlphaBlit_rw(uint8_t* tt, Canvas* c) {
    if ((uint32_t)(uintptr_t)tt == X_DUMMY) {
        gxSetCanvas(c);
        gxClear(G32(X_RED));
        return;
    }
    if (tt[0] != 2) {
        LogReport(S(0x004f426c));                                // "Can't alpha blit non-alpha textures"
        return;
    }
    const int8_t type = c->type;
    if (type != 4 && type != 1 && type != 3) {
        LogReport(S(0x004f4290), (int)type);                     // "texAlphaBlit: can't handle this format: %d"
        return;
    }
    int pitch = 0;
    uint8_t* bits = get_mip_level(tt, c->w, &pitch);
    Canvas src;
    gxBuildCanvas(&src, c->type, bits, pitch, pitch, pitch + pitch);
    gxSetCanvas(c);
    gxPasteAlpha(&src, 0, 0);
}
PORT_FN(0x00461470, "texAlphaBlit", texAlphaBlit_rw, fp_tex_transfer)

// texMapBits (0x461540, nothing in v1.0 calls it): a canvas over the level `size` across of type `type` (555: reduced
// in place, in the resource), and its bits; the missing .tex: a static red texel
static void* __cdecl texMapBits_rw(uint8_t* tt, int size, int8_t type) {
    if ((uint32_t)(uintptr_t)tt == X_DUMMY) return (void*)(uintptr_t)0x00551260;
    if (type != 4 && type != 1 && type != 3) {
        LogPanic(S(0x004f42e8), (int)type);                      // "texRead: can't handle this format: %d"
        return 0;
    }
    uint8_t* bits = 0;
    int32_t p = 0;
    switch (size) {
    case 1: bits = tt + 0xc; p = 4; break;
    case 2: bits = tt + 0x2c; p = 4; break;
    case 4: bits = tt + 0x4c; p = 4; break;
    case 8: bits = tt + 0x6c; p = 8; break;
    case 0x10: bits = tt + 0xec; p = 0x10; break;
    case 0x20: bits = tt + 0x2ec; p = 0x20; break;
    case 0x40: bits = tt + 0xaec; p = 0x40; break;
    case 0x80: bits = tt + 0x2aec; p = 0x80; break;
    case 0x100: bits = tt + 0xaaec; p = 0x100; break;
    default: LogReport(S(0x004f42bc), size); break;              // "texRead: can't handle this size dest: %d"
    }
    Canvas c;
    gxBuildCanvas(&c, type, bits, p, p, p * 2);
    if (type == 3) gfxReduceTo555(&c);
    return ((volatile Canvas*)&c)->bits;
}
static void fp_tex_map_bits(Footprint& f, uint8_t* tt, int size, int8_t type) {
    if ((uint32_t)(uintptr_t)tt == X_DUMMY || type != 3) return;
    uint32_t off = 0, p = 0;
    switch (size) {
    case 1: off = 0xc; p = 4; break;
    case 2: off = 0x2c; p = 4; break;
    case 4: off = 0x4c; p = 4; break;
    case 8: off = 0x6c; p = 8; break;
    case 0x10: off = 0xec; p = 0x10; break;
    case 0x20: off = 0x2ec; p = 0x20; break;
    case 0x40: off = 0xaec; p = 0x40; break;
    case 0x80: off = 0x2aec; p = 0x80; break;
    case 0x100: off = 0xaaec; p = 0x100; break;
    default: return;
    }
    f.add(tt + off, p * p * 2, "the level's texels (reduced to 555 in place)");
}
PORT_FN(0x00461540, "texMapBits", texMapBits_rw, fp_tex_map_bits)

// load_file (0x461790): the TEX resource (version 3; a dual file's alpha half when mrCaps.alpha); missing: the red
// dummy (reset: format 0, key 0, 3 levels)
static uint8_t* __cdecl load_file_rw(const char* name) {
    uint32_t version;
    int32_t size;
    uint8_t* d = (uint8_t*)ResourceGetDiscardable(name, G32(X_TEX_TYPE), &version, &size);
    if (d) {
        char tmp[0x88];
        if (G32(X_TEX_VER) != version) LogPanic(S(0x004f4310), log_name(name, tmp), version);   // "%s bad tex version: %d"
        if (d[0] == 3 && GI(X_MRCAPS_ALPHA) != 0) d += size / 2;
        return d;
    }
    G16(X_DUMMY + 4) = 0;
    GI(X_DUMMY + 8) = 3;
    G8(X_DUMMY) = 0;
    char tmp[0x88];
    LogReport(S(0x004f4328), log_name(name, tmp));               // "tex not found: %s"
    return (uint8_t*)(uintptr_t)X_DUMMY;
}
static void fp_load_file(Footprint& f, const char*) { f.replay_only = "fetches the .tex resource (ResourceGetDiscardable may load its file)"; }
PORT_FN(0x00461790, "load_file", load_file_rw, fp_load_file)

// unload_file (0x461830): the resource forgotten -- an alpha half (format 4) by its file's start, found from its level
// count (an unknown count panics and is taken as -1)
static void __cdecl unload_file_rw(uint8_t* tt) {
    if ((uint32_t)(uintptr_t)tt == X_DUMMY) return;
    if (tt[0] == 4) {
        uint32_t edi = 0xffffffffu;
        const int32_t n = *(volatile int32_t*)(tt + 8);
        switch ((uint32_t)(n - 1)) {
        case 0: edi = 0x2c; break;
        case 1: edi = 0x4c; break;
        case 2: edi = 0x6c; break;
        case 3: edi = 0xec; break;
        case 4: edi = 0x2ec; break;
        case 5: edi = 0xaec; break;
        case 6: edi = 0x2aec; break;
        case 7: edi = 0xaaec; break;
        case 8: edi = 0x2aaec; break;
        default: LogPanic(S(0x004f433c), n); break;              // "unhandled max_mip: %d"
        }
        tt = (uint8_t*)((uintptr_t)tt - edi);                    // (-1: one byte on)
    }
    ResourceForget(tt);
}
static void fp_unload_file(Footprint& f, uint8_t*) { f.replay_only = "forgets the .tex resource (ResourceForget may free it)"; }
PORT_FN(0x00461830, "unload_file", unload_file_rw, fp_unload_file)
