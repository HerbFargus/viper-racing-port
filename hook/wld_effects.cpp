// wld_effects.cpp -- M3 world stage, group W4: the effects, rewritten (library `world`).
//
//   effects.obj  skid marks (SkidObject, SkidMark, SkidClient::SkidOn / SkidOff / Clear), ShadowObject and its
//                model (create_shadow_model, find_idx_of, faces_up), SmokeObject (the smoke puffs: the model,
//                its animation, Puff, Update), ReflectionObject (the car's reflection: set-up, distance,
//                visibility, footensor), interpolate, EffectsBegin / EffectsEnd, and the shared GraphObject /
//                BuddyObject COMDATs the linker put here (GraphObject::GetZ, BuddyObject::IsVisible / GetZ)
//   debris.obj   DebrisObject (the smoke of a crash: 16 SmokeObjects and 16 debris velocities) and SparksObject
//                (32 sparks), their replay-stream handlers debris_event / sparks_event, DebrisBegin / End,
//                GetParticlePosition, RandomReal
//   splash.obj   SplashObject (32 water splashes, a 3 x 5 grid of vertices each): Splash, Update, the event
//                handler, Begin / End, get_random_v, random_real (library `world`, not the root's splash.obj)
//
// Not rewritten here: every function that draws (ShadowObject::Draw, SmokeObject::Draw, ReflectionObject::Draw,
// SkidObject::Draw, SparksObject::Draw, SplashObject::Draw, and DebrisObject::Draw, which only tail-calls
// WorldFXEnabled: the graphics stage); the bare `ret` stubs GraphObject::InitTextures (0x468e60),
// SkidObject::Update (0x468f60), DebrisObject::Update (0x46eb00), SparksObject::Update (0x46ed50); the $E
// initialisers; the deleting destructors (BuddyObject 0x468e90, ShadowObject 0x468eb0, FrameObject 0x468ed0,
// SmokeObject 0x468ef0, ReflectionObject 0x468f10, SkidObject 0x468f70, DebrisObject 0x46eb10, SparksObject
// 0x46ed60, SplashObject 0x46f890). The model and texture functions (mrModel*, gxGetTexture, TextureGet...), the
// GraphObject constructor / destructor, P3DBase's constructor, WorldAddGob, the event table, Random and the
// world's accessors are called by their v1.0 addresses, and so are this file's own functions where one calls
// another (a hooked rewrite is what runs). The event handlers are installed by their ORIGINAL addresses, as the
// original does.
//
// Threads. All of it runs on the MAIN (render) thread: the world's update (WorldUpdate -> EventDispatch runs
// debris_event / sparks_event / SplashObject::Handler on the physics thread's event stream), the car objects
// (CarObject::UpdateMessage: SkidOn / SkidOff / Clear; CarObject::DrawWheel: Puff), the graphics objects' Update /
// IsVisible, and the world's begin / end. So no footprint here gets the physics statics saved for it: each lists
// every static it writes. Random numbers (real_random, RandomReal, random_real, SplashObject's constructor) are
// inputs, fed automatically.
//
// Layouts (v1.0; from the disassembly, sizes from the allocations and the recovered types):
//   mrModelInfo (0x28 header): +0 nverts, +4 mrVertex* (32 bytes: position, normal, u, v), +8 nmaterials,
//     +0xc materials (32 bytes: texture name, ..., +0x1a vertex count (u16), +0x1e triangle count (u16)),
//     +0x10 ntriangles, +0x14 mrTriangle* (8 bytes: 3 s16 vertex indices, 1 more), +0x18 n / +0x1c 16-byte
//     records, +0x20 n / +0x24 4-byte records
//   SkidObject (0x14, vtable 0x4dd320): +4 SkidMark* marks, +8 count, +0xc next mark to try, +0x10 texture
//   SkidMark (0x38): +0 / +0xc the quad's first edge (left, right), +0x18 / +0x24 its last edge, +0x30 state
//     (0 free, 1 in use, 2 released), +0x34 its client's id
//   SkidClient (0x3c; a wheel's): +0 / +0xc the last edge (left, right), +0x18 SkidMark* current, +0x1c
//     SkidObject* owner, +0x20 float time since the mark began (steps of 1/30), +0x24 id, +0x28 u8 "new mark"
//   ShadowObject (0x1c, vtable 0x4dd258): +4 WorldObject* (its frame at +4, position +0x28), +8 the car's index
//     (the object's +0x1c8), +0xc squared distance to the camera, +0x10 mrModelInfo* (from the file), +0x14 its
//     copy (one MemAlloc), +0x18 model
//   ReflectionObject (0x1c, vtable 0x4dd2f8): +4 WorldObject*, +8 squared distance, +0xc model, +0x10 0,
//     +0x14 1.0, +0x18 0
//   SmokeObject (0x58, vtable 0x4dd2a8; a FrameObject): +4 Frame (position +0x28), +0x34 mrModelInfo* (its own:
//     create_smoke_model), +0x38 model (-1 once destroyed), +0x3c visible, +0x3d the animation's second row,
//     +0x40 velocity, +0x4c end time, +0x50 duration, +0x54 alpha
//   DebrisObject (0x11c, vtable 0x4dd5d8): +4 P3[16] debris velocities, +0xc4 the last crash point, +0xd0
//     texture, +0xd4 SmokeObject*[16], +0x114 next smoke, +0x118 float time of the last burst (init 1e6)
//   SparksObject (0x38c, vtable 0x4dd5b0): +4 Particle[32] (0x1c: +0 velocity, +0xc position, +0x18 float start
//     time, init 1e6), +0x384 next particle, +0x388 texture
//   SplashObject (0x380, vtable 0x4dd660): +4 the volume that splashed (void*), +8 texture, +0xc position,
//     +0x18 baz[15] (P3 velocities; particle k = i + 3 j: column i 0..2, row j 0..4), +0xcc u16 indices[96]
//     (both windings of the grid's 16 triangles), +0x18c mrVertex[15] (x y z, +0x10 colour ARGB, +0x14 specular,
//     +0x18 u, +0x1c v), +0x36c start time (init -1000), +0x370 duration, +0x374 last update time, +0x378 alpha
//     (init 0.7), +0x37c visible
//   the replay stream's events (EventDispatch; data unaligned): splash (type 0, 0x1c bytes: velocity, position,
//     the volume), sparks (type 1, 0x18: velocity, point), debris (type 2, 0x24: tangential velocity, point,
//     relative velocity)
//
// Statics (debris.obj and splash.obj; no symbols for the first two):
//   0x5599c0 SparksObject*   0x5599c4 DebrisObject*
//   0x559a18 SplashObject::g_tab: SplashObject* p[32], then 0x559a98 int count (the constructors' fill count, then
//            the handler's round-robin index)
//   (event.obj) 0x557ee0 the handler table {fn, type}[16], 0x557f60 its count
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
static __forceinline uint32_t Ub(const void* p) { uint32_t u; memcpy(&u, p, 4); return u; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }      // an integer move of a float
static __forceinline void put4(void* d, uint32_t v) { memcpy(d, &v, 4); }
template <typename T> static __forceinline T* P(uint32_t addr) { return (T*)(uintptr_t)addr; }
static __forceinline float& Fat(void* p, uint32_t off) { return *(float*)((uint8_t*)p + off); }
static __forceinline int32_t& Iat(void* p, uint32_t off) { return *(int32_t*)((uint8_t*)p + off); }
static __forceinline uint8_t*& Pat(void* p, uint32_t off) { return *(uint8_t**)((uint8_t*)p + off); }
// An fst / fstp dword the original always makes, kept even where the float then goes unused (docs/PORTING.md 10a):
// rounding a register value past FLT_MAX into a float is an overflow, a fault where it's unmasked, so the store has
// to happen where the original's does. (The compiler drops a dead conversion, or sinks it into a branch.)
static __forceinline float st(double d) {
    volatile float f = (float)d;
    return f;
}
static __forceinline void cp12(void* d, const void* s) {       // three integer moves, x then y then z
    cp4(d, s);
    cp4((uint8_t*)d + 4, (const uint8_t*)s + 4);
    cp4((uint8_t*)d + 8, (const uint8_t*)s + 8);
}
// fld dword; fchs; fstp dword -- through the FPU, as the original does
static __forceinline float fpu_neg(const float* p) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[p]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "fchs\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [p] "m"(p)
                     : VP_X87_CLOBBERS, "eax", "cc", "memory");
#else
    __asm { mov eax, p
            fld dword ptr [eax]
            fchs
            fstp r }
#endif
    return r;
}

struct mrModelInfo {
    int32_t nverts;  uint8_t* verts;               // +0x00  mrVertex, 32 bytes
    int32_t nmats;   uint8_t* mats;                // +0x08  32 bytes
    int32_t ntris;   uint8_t* tris;                // +0x10  mrTriangle, 8 bytes
    int32_t n18;     uint8_t* p1c;                 // +0x18  16 bytes each
    int32_t n20;     uint8_t* p24;                 // +0x20  4 bytes each
};
static_assert(sizeof(mrModelInfo) == 0x28, "mrModelInfo");
}  // namespace

enum : uint32_t {
    S_CAMERA_POS = 0x0055334c,          // current_camera (0x553328, a Frame) .pos
    S_SPARKS = 0x005599c0,              // SparksObject*
    S_DEBRIS = 0x005599c4,              // DebrisObject*
    S_SPLASH_TAB = 0x00559a18,          // SplashObject::g_tab.p[32]
    S_SPLASH_COUNT = 0x00559a98,        // SplashObject::g_tab.count
    S_EVENTS = 0x00557ee0,              // event.obj: {handler, type}[16], then the count (0x557f60)
    S_GOBS = 0x005522d4,                // WorldAddGob's table (1024) ...
    S_NUM_GOBS = 0x00553368,            // ... and count
};

// ---- the functions they call, by address -------------------------------------------------------------------
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* MemFree_t)(void*);
static const MemFree_t MemFree = (MemFree_t)0x00414300;
static const MemFree_t OpDelete = (MemFree_t)0x00414390;
typedef void(__cdecl* Panic_t)(const char*, ...);
static const Panic_t LogPanic = (Panic_t)0x004112b0;
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t game_sprintf = (Sprintf_t)0x004cf0a0;
typedef void*(__fastcall* ThisRet_t)(void*, Edx);
typedef void(__fastcall* ThisVoid_t)(void*, Edx);
static const ThisRet_t GraphObject_ctor = (ThisRet_t)0x00469840;
static const ThisVoid_t GraphObject_dtor = (ThisVoid_t)0x00469850;
static const ThisRet_t P3DBase_ctor = (ThisRet_t)0x0045d8d0;
typedef mrModelInfo*(__cdecl* InfoGet_t)(const char*);
static const InfoGet_t mrModelInfoGet = (InfoGet_t)0x004562f0;
typedef void(__cdecl* InfoForget_t)(mrModelInfo*);
static const InfoForget_t mrModelInfoForget = (InfoForget_t)0x00456460;
typedef int(__cdecl* ModelCreate_t)(const char*, int);
static const ModelCreate_t mrModelCreate = (ModelCreate_t)0x00455b40;
typedef void(__cdecl* ModelBuild_t)(int, mrModelInfo*, int);
static const ModelBuild_t mrModelBuild = (ModelBuild_t)0x00455c80;
typedef void(__cdecl* IntVoid_t)(int);
static const IntVoid_t mrModelDestroy = (IntVoid_t)0x00455bc0;
static const IntVoid_t mrModelUnload = (IntVoid_t)0x00455950;
static const IntVoid_t gxForgetTexture = (IntVoid_t)0x0044e280;
static const IntVoid_t TextureForget = (IntVoid_t)0x0045a110;
typedef int(__cdecl* ModelLoad_t)(const char*);
static const ModelLoad_t mrModelLoad = (ModelLoad_t)0x004558c0;
typedef int(__cdecl* GetTex_t)(const char*, int);
static const GetTex_t gxGetTexture = (GetTex_t)0x0044e240;
static const GetTex_t TextureGet = (GetTex_t)0x0045a0c0;
typedef int(__cdecl* IntFn_t)();
static const IntFn_t WorldShowShadow = (IntFn_t)0x00461f80;
static const IntFn_t WorldShowSmoke = (IntFn_t)0x00461fc0;
static const IntFn_t WorldGetFocusCar = (IntFn_t)0x004626d0;
static const IntFn_t WorldGetCameraType = (IntFn_t)0x004626e0;
typedef uint8_t(__cdecl* ByteFn_t)();
static const ByteFn_t ViewIsMirrorMode = (ByteFn_t)0x004667f0;
static const ByteFn_t WorldFXEnabled = (ByteFn_t)0x00462860;
static const ByteFn_t PhysicsIsPaused = (ByteFn_t)0x0042bd20;
typedef double(__cdecl* TimeFn_t)();                 // tick x 0.016f, left in ST0 (fild; fmul): a register value
static const TimeFn_t PhysicsGetTime = (TimeFn_t)0x0042bc80;
static const TimeFn_t WorldGetElapsedTime = (TimeFn_t)0x00462050;
typedef int(__cdecl* Random_t)(int);
static const Random_t Random = (Random_t)0x0041b6e0;
typedef void(__cdecl* AddGob_t)(void*);
static const AddGob_t WorldAddGob = (AddGob_t)0x004627c0;
typedef void(__cdecl* InstallHandler_t)(uint32_t, uint8_t);
static const InstallHandler_t EventInstallHandler = (InstallHandler_t)0x004691e0;
typedef void(__cdecl* UninstallHandler_t)(uint32_t);
static const UninstallHandler_t EventUnInstallHandler = (UninstallHandler_t)0x00469210;

// this file's own functions, by address (so a hooked rewrite is what runs)
typedef int16_t(__cdecl* FindIdx_t)(int, int16_t*, int);
static const FindIdx_t find_idx_of_o = (FindIdx_t)0x00467310;
typedef uint8_t(__cdecl* FacesUp_t)(const uint8_t*, const int16_t*);
static const FacesUp_t faces_up_o = (FacesUp_t)0x00467350;
static const ByteFn_t camera_is_in_car_o = (ByteFn_t)0x00467760;
typedef void(__cdecl* SmokeAnim_t)(int, mrModelInfo*, float, uint8_t);
static const SmokeAnim_t set_smoke_animation_o = (SmokeAnim_t)0x00467b30;
typedef mrModelInfo*(__cdecl* CreateSmoke_t)();
static const CreateSmoke_t create_smoke_model_o = (CreateSmoke_t)0x00467d70;
typedef void(__cdecl* DestroySmoke_t)(mrModelInfo*);
static const DestroySmoke_t destroy_smoke_model_o = (DestroySmoke_t)0x00467b20;
static const ThisRet_t SmokeObject_ctor_o = (ThisRet_t)0x00467c80;
typedef void(__fastcall* Puff_t)(void*, Edx, const P3*, const P3*, uint8_t);
static const Puff_t SmokeObject_Puff_o = (Puff_t)0x00467fa0;
typedef double(__cdecl* RealRandom_t)(uint32_t, uint32_t);   // (float, float), pushed as bits; ST0 back
static const RealRandom_t real_random_o = (RealRandom_t)0x004680b0;
static const RealRandom_t RandomReal_o = (RealRandom_t)0x0046e900;
static const ThisRet_t SkidMark_ctor_o = (ThisRet_t)0x00468f90;
typedef uint8_t*(__fastcall* GetSkid_t)(void*, Edx, int);
static const GetSkid_t SkidObject_GetSkid_o = (GetSkid_t)0x00468a70;
typedef void(__fastcall* ReleaseSkid_t)(void*, Edx, uint8_t*);
static const ReleaseSkid_t SkidObject_ReleaseSkid_o = (ReleaseSkid_t)0x00468ad0;
typedef void(__cdecl* Void_t)();
static const Void_t SplashBegin_o = (Void_t)0x0046f840;
static const Void_t SplashEnd_o = (Void_t)0x0046f850;
static const Void_t DebrisBegin_o = (Void_t)0x0046e570;
static const Void_t DebrisEnd_o = (Void_t)0x0046e750;
static const Void_t SplashObject_Begin_o = (Void_t)0x0046f7e0;
static const Void_t SplashObject_End_o = (Void_t)0x0046f830;
static const ThisRet_t Particle_ctor_o = (ThisRet_t)0x0046eda0;
static const ThisRet_t baz_ctor_o = (ThisRet_t)0x0046f8b0;
static const ThisRet_t SplashObject_ctor_o = (ThisRet_t)0x0046f590;
typedef void(__cdecl* GetRandomV_t)(P3*);
static const GetRandomV_t get_random_v_o = (GetRandomV_t)0x0046f320;
typedef double(__cdecl* RandomRealI_t)(int, int);
static const RandomRealI_t random_real_o = (RandomRealI_t)0x0046f360;
typedef void(__fastcall* Splash_t)(void*, Edx, const void*, const void*, void*);
static const Splash_t SplashObject_Splash_o = (Splash_t)0x0046efd0;
// the handlers, installed by their original addresses
enum : uint32_t { H_DEBRIS = 0x0046e770, H_SPARKS = 0x0046e930, H_SPLASH = 0x0046f780 };

// footprint helpers
static void fp_event_table(Footprint& f) { f.add(P<uint8_t>(S_EVENTS), 0x84, "the world's event handlers and count"); }
static void fp_smoke_verts(Footprint& f, uint8_t* smoke) {        // set_smoke_animation's writes
    mrModelInfo* info = *(mrModelInfo**)(smoke + 0x34);
    if (info && info->verts) f.add(info->verts, 4 * 32, "the smoke model's vertices");
}

// ==============================================================================================================
// effects.obj
// ==============================================================================================================

// ==== find_idx_of (0x467310) ==================================================================================
// the index of v in a[0..n) (as a short), or a panic and -10000
static int16_t __cdecl find_idx_of(int v, int16_t* a, int n) {
    for (int i = 0; i < n; i++)
        if ((int32_t)a[i] == v) return (int16_t)i;
    LogPanic((const char*)0x004f4e24, v, n);           // "can't find idx of %d in array[%d]."
    return (int16_t)0xd8f0;                             // (mov ax: eax's upper half is LogPanic's)
}
static void fp_find_idx_of(Footprint&, int, int16_t*, int) {}
PORT_FN(0x00467310, "find_idx_of", find_idx_of, fp_find_idx_of)

// ==== faces_up (0x467350) =====================================================================================
// the triangle's unit normal (v0 - v2) x (v0 - v1) has y above 0.2
static uint8_t __cdecl faces_up(const uint8_t* verts, const int16_t* tri) {
    const float* v0 = (const float*)(verts + ((int32_t)tri[0] << 5));
    const float* v1 = (const float*)(verts + ((int32_t)tri[1] << 5));
    float a[3], b[3], n[3];
    a[0] = (float)(D(v0[0]) - v1[0]);
    a[1] = (float)(D(v0[1]) - v1[1]);
    a[2] = (float)(D(v0[2]) - v1[2]);
    const float* v2 = (const float*)(verts + ((int32_t)tri[2] << 5));
    b[0] = (float)(D(v0[0]) - v2[0]);
    b[1] = (float)(D(v0[1]) - v2[1]);
    b[2] = (float)(D(v0[2]) - v2[2]);
    n[0] = (float)(D(b[2]) * a[1] - D(b[1]) * a[2]);   // (a dead alias check of the cross product's output)
    n[1] = (float)(D(b[0]) * a[2] - D(b[2]) * a[0]);
    n[2] = (float)(D(b[1]) * a[0] - D(b[0]) * a[1]);
    const double inv = 1.0f / x87_sqrt((D(n[1]) * n[1] + D(n[2]) * n[2]) + D(n[0]) * n[0]);
    const double y = inv * n[1];
    const uint8_t up = y > FB(0x3e4ccccd) ? 1 : 0;      // fcom 0.2f; test ah,0x41; sete
    st(y);                                              // (then fstp dword, unused)
    return up;
}
// (reads vertices anywhere the indices point: not fuzzed, the world harness checks it)
static void fp_faces_up(Footprint&, const uint8_t*, const int16_t*) {}
PORT_FN(0x00467350, "faces_up", faces_up, fp_faces_up)

// ==== create_shadow_model (0x4670a0) ==========================================================================
// The shadow's model: the triangles of `name` that face up (at most 256), their vertices (normal (0,1,0), uv
// (0.984, 0.984)), one material "effects.tex"; header, vertices, material and triangles in one MemAlloc, zeroed.
// (Nothing calls it.) The unique-vertex list is 512 shorts on the original's stack with no bound: 256 triangles
// can name 768 vertices, and past 512 the original overwrites its saved registers and return address. The
// rewrite's list holds 768, so it matches the original wherever the original survives.
static mrModelInfo* __cdecl create_shadow_model(const char* name) {
    mrModelInfo* src = mrModelInfoGet(name);
    if (!src) return 0;
    int16_t faces[256];
    int16_t uniq[768];
    int nfaces = 0, nuniq = 0;
    for (int t = 0; t < src->ntris; t++) {
        if (faces_up_o(src->verts, (const int16_t*)(src->tris + t * 8))) {
            faces[nfaces++] = (int16_t)t;
            if (nfaces == 256) break;
        }
    }
    for (int f = 0; f < nfaces; f++) {
        const int16_t* tri = (const int16_t*)(src->tris + ((int32_t)faces[f] << 3));
        for (int c = 0; c < 3; c++) {
            int k = 0;
            for (; k < nuniq; k++)
                if (uniq[k] == tri[c]) break;
            if (k == nuniq) uniq[nuniq++] = tri[c];
        }
    }
    const uint32_t size = (uint32_t)(nfaces + nuniq * 4) * 8 + 0x48;
    uint8_t* p = (uint8_t*)MemAlloc((int)size);
    memset(p, 0, size);
    uint8_t* mat = p + nuniq * 32 + 0x28;
    uint8_t* tris = p + nuniq * 32 + 0x48;
    for (int f = 0; f < nfaces; f++) {
        const uint8_t* s = src->tris + ((int32_t)faces[f] << 3);
        int16_t* d = (int16_t*)(tris + f * 8);
        uint32_t w0 = Ub(s), w1 = Ub(s + 4);
        memcpy(d, &w0, 4);
        memcpy(d + 2, &w1, 4);
        d[0] = find_idx_of_o(d[0], uniq, nuniq);
        d[1] = find_idx_of_o(d[1], uniq, nuniq);
        d[2] = find_idx_of_o(d[2], uniq, nuniq);
    }
    uint8_t* verts = p + 0x28;
    for (int k = 0; k < nuniq; k++) {
        uint8_t* v = verts + k * 32;
        memcpy(v, src->verts + ((int32_t)uniq[k] << 5), 32);
        put4(v + 0x18, 0x3f7c0000);                     // uv 0.984375
        put4(v + 0x1c, 0x3f7c0000);
        put4(v + 0xc, 0);                               // normal (0, 1, 0)
        put4(v + 0x10, 0x3f800000);
        put4(v + 0x14, 0);
    }
    const char* tex = (const char*)0x004f4e18;          // "effects.tex"
    memcpy(mat, tex, strlen(tex) + 1);
    const int16_t nu16 = (int16_t)nuniq, nf16 = (int16_t)nfaces;
    memcpy(mat + 0x1a, &nu16, 2);
    memcpy(mat + 0x1e, &nf16, 2);
    mrModelInfo* m = (mrModelInfo*)p;
    m->nverts = nuniq;
    m->verts = verts;
    m->ntris = nfaces;
    m->tris = tris;
    m->nmats = 1;
    m->mats = mat;
    return m;
}
static void fp_create_shadow_model(Footprint& f, const char*) { f.replay_only = "allocates the model"; }
PORT_FN(0x004670a0, "create_shadow_model", create_shadow_model, fp_create_shadow_model)

// ==== EffectsBegin (0x467460) / EffectsEnd (0x467470) ========================================================
static void __cdecl EffectsBegin() {
    SplashBegin_o();
    DebrisBegin_o();                                    // (a tail jump)
}
static void fp_effects_begin(Footprint& f) { f.replay_only = "allocates the splash, debris and sparks objects"; }
PORT_FN(0x00467460, "EffectsBegin", EffectsBegin, fp_effects_begin)

static void __cdecl EffectsEnd() {
    DebrisEnd_o();
    SplashEnd_o();
}
static void fp_effects_end(Footprint& f) { fp_event_table(f); }
PORT_FN(0x00467470, "EffectsEnd", EffectsEnd, fp_effects_end)

// ==== ShadowObject ==============================================================================================
// ShadowObject::ShadowObject (0x467480): the car's shadow model "<name><detail>.mod" (detail = the car's level of
// detail less WorldShowShadow(), at least 0), copied whole into one MemAlloc -- its materials' names become
// "wheels.tex" -- and built as the model "shadow".
static uint8_t* __fastcall ShadowObject_ctor(uint8_t* self, Edx, const char* name, uint8_t* wobj) {
    GraphObject_ctor(self, 0);
    put4(self, 0x004dd280);                             // BuddyObject
    Pat(self, 4) = wobj;
    put4(self, 0x004dd258);                             // ShadowObject
    Iat(self, 0x14) = 0;
    Iat(self, 8) = Iat(wobj, 0x1c8);
    int32_t detail = **(int32_t**)(wobj + 0x28c);
    detail = (int32_t)((uint32_t)detail - (uint32_t)WorldShowShadow());
    if (detail < 0) detail = 0;                         // sub; jns
    char buf[64];                                       // (the original's 64 bytes)
    game_sprintf(buf, (const char*)0x004f4e48, name, detail);   // "%s%d.mod"
    mrModelInfo* info = mrModelInfoGet(buf);
    *(mrModelInfo**)(self + 0x10) = info;
    if (!info) return self;
    const uint32_t nv = (uint32_t)info->nverts << 5, nt = (uint32_t)info->ntris << 3, nm = (uint32_t)info->nmats << 5;
    const uint32_t n18 = (uint32_t)info->n18 << 4, n20 = (uint32_t)info->n20 << 2;
    uint8_t* p = (uint8_t*)MemAlloc((int)(nv + (nt + (nm + (n18 + n20))) + 0x28));
    const uint32_t off_mat = nv + 0x28, off_tri = nm + off_mat, off_18 = off_tri + nt, off_20 = off_18 + n18;
    memcpy(p, *(mrModelInfo**)(self + 0x10), 0x28);
    memcpy(p + 0x28, (*(mrModelInfo**)(self + 0x10))->verts, nv);
    memcpy(p + off_mat, (*(mrModelInfo**)(self + 0x10))->mats, nm);
    const char* tex = (const char*)0x004f4e54;          // "wheels.tex"
    for (int i = 0; i < (*(mrModelInfo**)(self + 0x10))->nmats; i++) memcpy(p + off_mat + i * 32, tex, strlen(tex) + 1);
    memcpy(p + off_tri, (*(mrModelInfo**)(self + 0x10))->tris, nt);
    memcpy(p + off_18, (*(mrModelInfo**)(self + 0x10))->p1c, n18);
    memcpy(p + off_20, (*(mrModelInfo**)(self + 0x10))->p24, n20);
    Pat(self, 0x14) = p;
    (*(mrModelInfo**)(self + 0x14))->verts = p + 0x28;
    (*(mrModelInfo**)(self + 0x14))->mats = p + off_mat;
    (*(mrModelInfo**)(self + 0x14))->tris = p + off_tri;
    (*(mrModelInfo**)(self + 0x14))->p1c = p + off_18;
    (*(mrModelInfo**)(self + 0x14))->p24 = p + off_20;
    const int m = mrModelCreate((const char*)0x004f4e60, (*(mrModelInfo**)(self + 0x14))->ntris);   // "shadow"
    Iat(self, 0x18) = m;
    mrModelBuild(m, *(mrModelInfo**)(self + 0x14), 7);
    return self;
}
static void fp_shadow_ctor(Footprint& f, uint8_t*, Edx, const char*, uint8_t*) { f.replay_only = "loads and allocates the shadow model"; }
PORT_FN(0x00467480, "ShadowObject::ShadowObject", ShadowObject_ctor, fp_shadow_ctor)

// ShadowObject::~ShadowObject (0x4676c0)
static void __fastcall ShadowObject_dtor(uint8_t* self, Edx) {
    put4(self, 0x004dd258);
    if (Iat(self, 0x10) != 0) {
        MemFree(Pat(self, 0x14));
        mrModelInfo* info = *(mrModelInfo**)(self + 0x10);
        Iat(self, 0x14) = 0;
        mrModelInfoForget(info);
        mrModelDestroy(Iat(self, 0x18));
    }
    GraphObject_dtor(self, 0);
}
static void fp_shadow_dtor(Footprint& f, uint8_t*, Edx) { f.replay_only = "frees the model"; }
PORT_FN(0x004676c0, "ShadowObject::~ShadowObject", ShadowObject_dtor, fp_shadow_dtor)

// ShadowObject::IsVisible (0x467710): the car is, the camera isn't in it (when it's the focus car), not the mirror,
// and the camera is within 100 m
static uint8_t __fastcall ShadowObject_IsVisible(uint8_t* self, Edx) {
    uint8_t* w = Pat(self, 4);
    if (!VFN(w, 0xc, uint8_t)(w, 0)) return 0;
    if (WorldGetFocusCar() == Iat(self, 8) && camera_is_in_car_o()) return 0;
    if (ViewIsMirrorMode()) return 0;
    volatile float k100 = 100.0f;                       // fld 100; fmul 100 at run time
    if (!(D(k100) * k100 > Fat(self, 0xc))) return 0;   // fcomp; test ah,0x41
    return 1;
}
static void fp_shadow_visible(Footprint&, uint8_t*, Edx) {}
PORT_FN(0x00467710, "ShadowObject::IsVisible", ShadowObject_IsVisible, fp_shadow_visible)

// camera_is_in_car (0x467760): the cockpit, bumper, hood and rear-view cameras (0..3)
static uint8_t __cdecl camera_is_in_car() {
    const int t = WorldGetCameraType();
    return t >= 0 && t <= 3 ? 1 : 0;
}
static void fp_camera_in_car(Footprint&) {}
PORT_FN(0x00467760, "camera_is_in_car", camera_is_in_car, fp_camera_in_car)

// ShadowObject::Update (0x467780): the squared distance from the camera
static void __fastcall ShadowObject_Update(uint8_t* self, Edx) {
    const float* c = P<float>(S_CAMERA_POS);
    const float* p = (const float*)(Pat(self, 4) + 0x28);
    const float dx = (float)(D(c[0]) - p[0]);
    const float dy = (float)(D(c[1]) - p[1]);
    const float dz = (float)(D(c[2]) - p[2]);
    Fat(self, 0xc) = (float)((D(dy) * dy + D(dz) * dz) + D(dx) * dx);
}
static void fp_shadow_update(Footprint& f, uint8_t* self, Edx) { f.add(self + 0xc, 4, "the shadow's camera distance"); }
PORT_FN(0x00467780, "ShadowObject::Update", ShadowObject_Update, fp_shadow_update)

// ShadowObject::Order (0x467b10)
static int __fastcall ShadowObject_Order(void*, Edx) { return -1000; }
static void fp_order(Footprint& f, void*, Edx) { f.pure = true; }
PORT_FN(0x00467b10, "ShadowObject::Order", ShadowObject_Order, fp_order)

// ==== SmokeObject ===============================================================================================
// destroy_smoke_model (0x467b20)
static void __cdecl destroy_smoke_model(mrModelInfo* info) { MemFree(info); }
static void fp_destroy_smoke(Footprint& f, mrModelInfo*) { f.replay_only = "frees the model"; }
PORT_FN(0x00467b20, "destroy_smoke_model", destroy_smoke_model, fp_destroy_smoke)

// set_smoke_animation (0x467b30): the puff's frame of effects.tex (8 frames across, t from 1 down to 0; the second
// row of frames when `flag`) and its size (0.2 m growing to 0.85 m), then the model rebuilt
static void __cdecl set_smoke_animation(int model, mrModelInfo* info, float t, uint8_t flag) {
    const double x = D(FB(0x40fccccd)) - D(t) * FB(0x40fccccd);   // 7.9 - 7.9 t
    const int32_t i = x87_ftol(x);
    if (i < 0 || i >= 8) return;
    float u[4], v[4];
    const double a = D(i) * 0.125f + FB(0x3c000000);    // (register, stored twice)
    const float af = (float)a;
    const float bf = (float)(D(i + 1) * 0.125f - FB(0x3c000000));
    u[0] = af; u[1] = bf; u[2] = bf; u[3] = af;
    put4(&v[0], 0x3df00000);                            // 0.1171875
    put4(&v[1], 0x3df00000);
    put4(&v[2], 0x3c000000);                            // 0.0078125
    put4(&v[3], 0x3c000000);
    const float off = flag ? 0.125f : 0.0f;
    volatile float k17 = FB(0x3fd9999a), k04 = FB(0x3ecccccd);   // 1.7 - 0.4, at run time
    const float s = (float)(((D(k17) - k04) * t + FB(0x3ecccccd)) * 0.5f);
    for (int k = 0; k < 4; k++) {
        cp4(info->verts + k * 32 + 0x18, &u[k]);
        Fat(info->verts + k * 32, 0x1c) = (float)(D(v[k]) + off);
    }
    const float ns = fpu_neg(&s);
    cp4(info->verts + 0x00, &ns);                       // x: -s s s -s
    cp4(info->verts + 0x20, &s);
    cp4(info->verts + 0x40, &s);
    cp4(info->verts + 0x60, &ns);
    cp4(info->verts + 0x04, &ns);                       // y: -s -s s s
    cp4(info->verts + 0x24, &ns);
    cp4(info->verts + 0x44, &s);
    cp4(info->verts + 0x64, &s);
    mrModelBuild(model, info, 7);
}
static void fp_smoke_anim(Footprint& f, int, mrModelInfo* info, float, uint8_t) {
    if (info && info->verts) f.add(info->verts, 4 * 32, "the smoke model's vertices");
}
PORT_FN(0x00467b30, "set_smoke_animation", set_smoke_animation, fp_smoke_anim)

// SmokeObject::SmokeObject (0x467c80)
static void smoke_identity(uint8_t* self) {
    put4(self + 0x04, 0x3f800000); put4(self + 0x08, 0); put4(self + 0x0c, 0);
    put4(self + 0x10, 0); put4(self + 0x14, 0x3f800000); put4(self + 0x18, 0);
    put4(self + 0x1c, 0); put4(self + 0x20, 0); put4(self + 0x24, 0x3f800000);
    put4(self + 0x28, 0); put4(self + 0x2c, 0); put4(self + 0x30, 0);
}
static uint8_t* __fastcall SmokeObject_ctor(uint8_t* self, Edx) {
    GraphObject_ctor(self, 0);
    smoke_identity(self);
    self[0x3c] = 0;
    put4(self, 0x004dd2d0);                             // FrameObject
    put4(self, 0x004dd2a8);                             // SmokeObject
    mrModelInfo* info = create_smoke_model_o();
    *(mrModelInfo**)(self + 0x34) = info;
    const int m = mrModelCreate((const char*)0x004f4e68, info->ntris);   // "SmokeModel"
    Iat(self, 0x38) = m;
    mrModelBuild(m, *(mrModelInfo**)(self + 0x34), 7);
    smoke_identity(self);
    return self;
}
static void fp_smoke_ctor(Footprint& f, uint8_t*, Edx) { f.replay_only = "allocates the smoke model"; }
PORT_FN(0x00467c80, "SmokeObject::SmokeObject", SmokeObject_ctor, fp_smoke_ctor)

// create_smoke_model (0x467d70): a 0.4 m square (two triangles, normal +y, uv 0) with the material "effects.tex",
// built on the stack and copied into one MemAlloc (0xd8 bytes; +0x1c and +0x24 left as they come)
static mrModelInfo* __cdecl create_smoke_model() {
    volatile float k04 = FB(0x3ecccccd), k05 = 0.5f;   // 0.4 x 0.5 at run time
    const double hr = D(k04) * k05;
    const float h = (float)hr, nh = (float)-hr;         // fld st0; fchs; fst
    uint32_t verts[32];
    const uint32_t H = Ub(&h), N = Ub(&nh), ONE = 0x3f800000;
    const uint32_t vv[32] = {N, N, 0, 0, ONE, 0, 0, 0,  H, N, 0, 0, ONE, 0, 0, 0,
                             H, H, 0, 0, ONE, 0, 0, 0,  N, H, 0, 0, ONE, 0, 0, 0};
    memcpy(verts, vv, sizeof verts);
    uint8_t mat[32];
    memcpy(mat, (const void*)0x004f4e74, 12);            // "effects.tex" (three dwords)
    put4(mat + 0x0c, 0);
    mat[0x10] = 0;
    mat[0x11] = 0;
    const uint16_t w[7] = {0, 0, 0, 0, 4, 0, 2};        // +0x12 .. +0x1e
    memcpy(mat + 0x12, w, 14);
    const uint16_t tris[8] = {1, 0, 2, 0, 3, 2, 0, 0};
    uint8_t* p = (uint8_t*)MemAlloc(0xd8);
    mrModelInfo* m = (mrModelInfo*)p;
    m->nverts = 4;
    m->verts = p + 0x28;
    m->nmats = 1;
    m->mats = p + 0xa8;
    m->ntris = 2;
    m->tris = p + 0xc8;
    m->n20 = 0;
    m->n18 = 0;
    memcpy(m->verts, verts, 128);
    memcpy(m->mats, mat, 32);
    memcpy(m->tris, tris, 16);
    return m;
}
static void fp_create_smoke(Footprint& f) { f.replay_only = "allocates the model"; }
PORT_FN(0x00467d70, "create_smoke_model", create_smoke_model, fp_create_smoke)

// SmokeObject::~SmokeObject (0x467f40)
static void __fastcall SmokeObject_dtor(uint8_t* self, Edx) {
    put4(self, 0x004dd2a8);
    if (Iat(self, 0x38) != -1) {
        destroy_smoke_model_o(*(mrModelInfo**)(self + 0x34));
        mrModelDestroy(Iat(self, 0x38));
        Iat(self, 0x38) = -1;
    }
    GraphObject_dtor(self, 0);
}
static void fp_smoke_dtor(Footprint& f, uint8_t*, Edx) { f.replay_only = "frees the model"; }
PORT_FN(0x00467f40, "SmokeObject::~SmokeObject", SmokeObject_dtor, fp_smoke_dtor)

// SmokeObject::IsVisible (0x467f80) / IsAlpha (0x467f90)
static uint8_t __fastcall SmokeObject_IsVisible(uint8_t* self, Edx) { return self[0x3c]; }
static void fp_smoke_visible(Footprint& f, uint8_t*, Edx) { f.pure = true; }
PORT_FN(0x00467f80, "SmokeObject::IsVisible", SmokeObject_IsVisible, fp_smoke_visible)

static uint8_t __fastcall SmokeObject_IsAlpha(void*, Edx) { return 1; }
static void fp_const(Footprint& f, void*, Edx) { f.pure = true; }
PORT_FN(0x00467f90, "SmokeObject::IsAlpha", SmokeObject_IsAlpha, fp_const)

// real_random (0x4680b0): a + (b - a) x Random(65535) / 65535 (the ST0 of the last fadd)
static double __cdecl real_random(float a, float b) {
    const int32_t r = Random(0xffff);
    return (D(r) * FB(0x37800080)) * (D(b) - a) + a;
}
static void fp_real_random(Footprint&, float, float) {}
PORT_FN(0x004680b0, "real_random", real_random, fp_real_random)

// SmokeObject::Puff (0x467fa0): a puff at pos, 0.2 m up, for 0.75..1.75 s, drifting at 0.3 vel plus a little
// random, mostly up
static void __fastcall SmokeObject_Puff(uint8_t* self, Edx, const P3* pos, const P3* vel, uint8_t flag) {
    self[0x3d] = flag;
    self[0x3c] = 1;
    volatile float k04 = FB(0x3ecccccd), k05 = 0.5f;
    const double h = D(k04) * k05;
    cp12(self + 0x28, pos);
    Fat(self, 0x2c) = (float)(h + Fat(self, 0x2c));
    const float dur = (float)real_random_o(0x3f400000, 0x3fe00000);   // 0.75, 1.75
    const double end = PhysicsGetTime() + dur;
    cp4(self + 0x50, &dur);
    put4(self + 0x54, 0);
    Fat(self, 0x4c) = (float)end;
    const float r1 = (float)real_random_o(0xbc23d70a, 0x3c23d70a);   // -0.01, 0.01
    const float r2 = (float)real_random_o(0x3c23d70a, 0x3cf5c28f);   // 0.01, 0.03
    const double r3 = real_random_o(0xbc23d70a, 0x3c23d70a);
    *(volatile float*)(self + 0x40) = (float)r3;                     // fst: r3 stays in the register
    cp4(self + 0x44, &r2);
    cp4(self + 0x48, &r1);
    Fat(self, 0x40) = (float)(r3 + D(vel->x) * FB(0x3e99999a));      // 0.3
    Fat(self, 0x44) = (float)(D(vel->y) * FB(0x3e99999a) + r2);
    Fat(self, 0x48) = (float)(D(vel->z) * FB(0x3e99999a) + r1);
    set_smoke_animation_o(Iat(self, 0x38), *(mrModelInfo**)(self + 0x34), 1.0f, flag);
}
static void fp_puff(Footprint& f, uint8_t* self, Edx, const P3*, const P3*, uint8_t) {
    f.add(self, 0x58, "the SmokeObject");
    fp_smoke_verts(f, self);
}
PORT_FN(0x00467fa0, "SmokeObject::Puff", SmokeObject_Puff, fp_puff)

// SmokeObject::Update (0x4680e0): drift (the rise speeding up by up to 2% a frame) unless paused; the animation
// by the time left; alpha the time left up to 0.99 (over a second: 1.75 - it)
static void __fastcall SmokeObject_Update(uint8_t* self, Edx) {
    if (!WorldFXEnabled()) self[0x3c] = 0;
    if (!self[0x3c]) return;
    if (!PhysicsIsPaused()) {
        Fat(self, 0x28) = (float)(D(Fat(self, 0x40)) + Fat(self, 0x28));
        Fat(self, 0x2c) = (float)(D(Fat(self, 0x2c)) + Fat(self, 0x44));
        Fat(self, 0x30) = (float)(D(Fat(self, 0x48)) + Fat(self, 0x30));
        const double r = real_random_o(0, 0x3ca3d70a);                // 0, 0.02
        Fat(self, 0x44) = (float)((r + 1.0f) * Fat(self, 0x44));
    }
    const double t = PhysicsGetTime();
    const double left = D(Fat(self, 0x4c)) - t;
    const float lf = st(left);                          // (fstp dword, before the test)
    if (!(left > 0.0f)) {                               // fcom 0; test ah,0x41
        self[0x3c] = 0;
        return;
    }
    const double q = D(lf) / Fat(self, 0x50);
    set_smoke_animation_o(Iat(self, 0x38), *(mrModelInfo**)(self + 0x34), (float)(1.0f - q), self[0x3d]);
    double a;
    if ((int32_t)Ub(&lf) >= 0x3f800000) a = FB(0x3fe00000) - D(lf);   // cmp dword, 1.0; jge: 1.75 - left
    else a = lf;
    Fat(self, 0x54) = (float)(FB(0x3f7d70a4) > a ? a : FB(0x3f7d70a4));   // 0.99: fcom; test ah,0x41
}
static void fp_smoke_update(Footprint& f, uint8_t* self, Edx) {
    f.add(self, 0x58, "the SmokeObject");
    fp_smoke_verts(f, self);
}
PORT_FN(0x004680e0, "SmokeObject::Update", SmokeObject_Update, fp_smoke_update)

// ==== ReflectionObject ==========================================================================================
// ReflectionObject::ReflectionObject (0x468290): the model "<name>2.mod"
static uint8_t* __fastcall ReflectionObject_ctor(uint8_t* self, Edx, const char* name, uint8_t* wobj) {
    GraphObject_ctor(self, 0);
    put4(self, 0x004dd280);                             // BuddyObject
    Pat(self, 4) = wobj;
    put4(self, 0x004dd2f8);                             // ReflectionObject
    char buf[64];                                       // (the original's 64 bytes)
    game_sprintf(buf, (const char*)0x004f4e80, name, 2);   // "%s%d.mod"
    Iat(self, 0xc) = mrModelLoad(buf);
    put4(self + 0x14, 0x3f800000);
    Iat(self, 0x10) = 0;
    Iat(self, 0x18) = 0;
    return self;
}
static void fp_refl_ctor(Footprint& f, uint8_t*, Edx, const char*, uint8_t*) { f.replay_only = "loads the model"; }
PORT_FN(0x00468290, "ReflectionObject::ReflectionObject", ReflectionObject_ctor, fp_refl_ctor)

// ReflectionObject::~ReflectionObject (0x4682f0)
static void __fastcall ReflectionObject_dtor(uint8_t* self, Edx) {
    const int m = Iat(self, 0xc);
    put4(self, 0x004dd2f8);
    mrModelUnload(m);
    GraphObject_dtor(self, 0);
}
static void fp_refl_dtor(Footprint& f, uint8_t*, Edx) { f.replay_only = "unloads the model"; }
PORT_FN(0x004682f0, "ReflectionObject::~ReflectionObject", ReflectionObject_dtor, fp_refl_dtor)

// ReflectionObject::Order (0x468310)
static int __fastcall ReflectionObject_Order(void*, Edx) { return -1003; }
PORT_FN(0x00468310, "ReflectionObject::Order", ReflectionObject_Order, fp_order)

// ReflectionObject::Update (0x468320): the squared distance from the camera (z's square from the register and the
// stored float: fst; fmul dword)
static void __fastcall ReflectionObject_Update(uint8_t* self, Edx) {
    const float* c = P<float>(S_CAMERA_POS);
    const float* p = (const float*)(Pat(self, 4) + 0x28);
    const float dx = (float)(D(c[0]) - p[0]);
    const float dy = (float)(D(c[1]) - p[1]);
    const double dzr = D(c[2]) - p[2];
    const float dz = (float)dzr;
    Fat(self, 8) = (float)((dzr * dz + D(dy) * dy) + D(dx) * dx);
}
static void fp_refl_update(Footprint& f, uint8_t* self, Edx) { f.add(self + 8, 4, "the reflection's camera distance"); }
PORT_FN(0x00468320, "ReflectionObject::Update", ReflectionObject_Update, fp_refl_update)

// ReflectionObject::IsVisible (0x468370): the car is, the camera within 30 m, not the mirror
static uint8_t __fastcall ReflectionObject_IsVisible(uint8_t* self, Edx) {
    uint8_t* w = Pat(self, 4);
    if (!VFN(w, 0xc, uint8_t)(w, 0)) return 0;
    volatile float k30 = 30.0f;
    if (!(D(k30) * k30 > Fat(self, 8))) return 0;       // fcomp; test ah,0x41
    if (ViewIsMirrorMode()) return 0;
    return 1;
}
static void fp_refl_visible(Footprint&, uint8_t*, Edx) {}
PORT_FN(0x00468370, "ReflectionObject::IsVisible", ReflectionObject_IsVisible, fp_refl_visible)

// footensor (0x4683b0): the reflection I - 2 b a^T (column c of row r: -2 b_r a_c), written in the original's order
static void __cdecl footensor(M3* m, const P3* a, const P3* b) {
    float* r = m->m;
    r[0] = (float)((D(b->x) * a->x) * -2.0f + 1.0f);
    r[3] = (float)((D(b->y) * a->x) * -2.0f);
    r[6] = (float)((D(b->z) * a->x) * -2.0f);
    r[1] = (float)((D(b->x) * a->y) * -2.0f);
    r[4] = (float)((D(b->y) * a->y) * -2.0f + 1.0f);
    r[7] = (float)((D(a->y) * b->z) * -2.0f);
    r[2] = (float)((D(b->x) * a->z) * -2.0f);
    r[5] = (float)((D(b->y) * a->z) * -2.0f);
    r[8] = (float)((D(b->z) * a->z) * -2.0f + 1.0f);
}
static void fp_footensor(Footprint& f, M3*, const P3*, const P3*) { f.pure = true; }
PORT_FN(0x004683b0, "footensor", footensor, fp_footensor)

// interpolate (0x468780): p = p (1 - t) + q t
static void __cdecl interpolate(P3* p, const P3* q, float t) {
    const double k = 1.0f - D(t);
    p->x = (float)(D(q->x) * t + D(p->x) * k);
    p->y = (float)(D(q->y) * t + D(p->y) * k);
    p->z = (float)(D(q->z) * t + D(p->z) * k);
}
static void fp_interpolate(Footprint& f, P3*, const P3*, float) { f.pure = true; }
PORT_FN(0x00468780, "interpolate", interpolate, fp_interpolate)

// ==== SkidObject / SkidMark / SkidClient ========================================================================
// SkidObject::SkidObject (0x4687d0): n marks, all free, and skid.tex
static uint8_t* __fastcall SkidObject_ctor(uint8_t* self, Edx, int n) {
    GraphObject_ctor(self, 0);
    put4(self, 0x004dd320);
    Iat(self, 8) = n;
    uint8_t* marks = (uint8_t*)MemAlloc((int)((uint32_t)n * 56));
    if (marks) {
        uint8_t* m = marks;
        for (int k = n - 1; k >= 0; k--, m += 0x38) SkidMark_ctor_o(m, 0);
        Pat(self, 4) = marks;
    } else {
        Pat(self, 4) = 0;
    }
    Iat(self, 0x10) = gxGetTexture((const char*)0x004f4e98, 0);   // "skid.tex"
    Iat(self, 0xc) = 0;
    for (int k = 0; k < Iat(self, 8); k++) Iat(Pat(self, 4) + k * 0x38, 0x30) = 0;
    return self;
}
static void fp_skid_ctor(Footprint& f, uint8_t*, Edx, int) { f.replay_only = "allocates the skid marks"; }
PORT_FN(0x004687d0, "SkidObject::SkidObject", SkidObject_ctor, fp_skid_ctor)

// the skid marks array, for footprints (guarded: a footprint may meet an object in any state)
static void fp_skid_marks(Footprint& f, uint8_t* skid) {
    const int32_t n = Iat(skid, 8);
    uint8_t* marks = Pat(skid, 4);
    if (marks && n > 0 && n < 0x10000) f.add(marks, (uint32_t)n * 0x38, "the skid marks");
}

// SkidObject::Clear (0x468860): every mark not in use is freed
static void __fastcall SkidObject_Clear(uint8_t* self, Edx) {
    Iat(self, 0xc) = 0;
    for (int k = 0; k < Iat(self, 8); k++) {
        int32_t& state = Iat(Pat(self, 4) + k * 0x38, 0x30);
        if (state != 1) state = 0;
    }
}
static void fp_skid_clear(Footprint& f, uint8_t* self, Edx) {
    f.add(self + 0xc, 4, "the SkidObject's next mark");
    fp_skid_marks(f, self);
}
PORT_FN(0x00468860, "SkidObject::Clear", SkidObject_Clear, fp_skid_clear)

// SkidObject::~SkidObject (0x468890)
static void __fastcall SkidObject_dtor(uint8_t* self, Edx) {
    const int tex = Iat(self, 0x10);
    put4(self, 0x004dd320);
    gxForgetTexture(tex);
    OpDelete(Pat(self, 4));
    GraphObject_dtor(self, 0);
}
static void fp_skid_dtor(Footprint& f, uint8_t*, Edx) { f.replay_only = "frees the skid marks"; }
PORT_FN(0x00468890, "SkidObject::~SkidObject", SkidObject_dtor, fp_skid_dtor)

// SkidObject::Order (0x4688c0), IsVisible (0x468a60: not in the mirror), IsAlpha (0x468f50)
static int __fastcall SkidObject_Order(void*, Edx) { return -1002; }
PORT_FN(0x004688c0, "SkidObject::Order", SkidObject_Order, fp_order)

static uint8_t __fastcall SkidObject_IsVisible(void*, Edx) { return ViewIsMirrorMode() < 1 ? 1 : 0; }   // cmp 1; sbb; neg
static void fp_skid_visible(Footprint&, void*, Edx) {}
PORT_FN(0x00468a60, "SkidObject::IsVisible", SkidObject_IsVisible, fp_skid_visible)

static uint8_t __fastcall SkidObject_IsAlpha(void*, Edx) { return 0; }
PORT_FN(0x00468f50, "SkidObject::IsAlpha", SkidObject_IsAlpha, fp_const)

// SkidObject::GetSkid (0x468a70): the first mark not in use from the next one round (the index wraps by one
// subtraction of the count, as the original's); in use, with the client's id
static uint8_t* __fastcall SkidObject_GetSkid(uint8_t* self, Edx, int id) {
    const int32_t n = Iat(self, 8);
    int32_t k = Iat(self, 0xc);
    for (int i = 0; i < n; i++, k++) {
        if (k >= n) k -= n;
        uint8_t* m = Pat(self, 4) + k * 0x38;
        if (Iat(m, 0x30) != 1) {
            Iat(self, 0xc) = k + 1;
            Iat(m, 0x30) = 1;
            Iat(m, 0x34) = id;
            return m;
        }
    }
    return 0;
}
static void fp_get_skid(Footprint& f, uint8_t* self, Edx, int) {
    f.add(self + 0xc, 4, "the SkidObject's next mark");
    fp_skid_marks(f, self);
}
PORT_FN(0x00468a70, "SkidObject::GetSkid", SkidObject_GetSkid, fp_get_skid)

// SkidObject::ReleaseSkid (0x468ad0)
static void __fastcall SkidObject_ReleaseSkid(void*, Edx, uint8_t* mark) { Iat(mark, 0x30) = 2; }
static void fp_release_skid(Footprint& f, void*, Edx, uint8_t* mark) { f.add(mark + 0x30, 4, "the mark's state"); }
PORT_FN(0x00468ad0, "SkidObject::ReleaseSkid", SkidObject_ReleaseSkid, fp_release_skid)

// SkidClient::SkidOn (0x468ae0): the wheel at `at`, its axle along `axis`. A new mark starts where the last one
// ended; one older than 1/7 s (in steps of 1/30) whose start is 0.7 m behind is released and a new one begun. The
// mark's last edge is `at` -/+ 0.1 m across the path (the path's direction x the axle, normalised; a first
// update also sets the first edge from the mark's start). No mark free: nothing.
static void __fastcall SkidClient_SkidOn(uint8_t* self, Edx, const P3* at, const P3* axis) {
    float* a = (float*)self;                            // the last edge: a (+0), b (+0xc)
    if (!Pat(self, 0x18)) {
        self[0x28] = 1;
        put4(self + 0x20, 0);
        cp12(self, at);
        cp12(self + 0xc, at);
    } else {
        Fat(self, 0x20) = (float)(D(Fat(self, 0x20)) + FB(0x3d088889));   // + 1/30
        const float dx = (float)(D(a[0]) - at->x);
        const float dy = (float)(D(a[1]) - at->y);
        const float dz = (float)(D(a[2]) - at->z);
        if ((int32_t)Ub(self + 0x20) > 0x3e124925 &&   // an integer compare of the float's bits: over 1/7 s
            (D(dz) * dz + D(dy) * dy) + D(dx) * dx > FB(0x3efae147)) {   // 0.49: test ah,0x41
            uint8_t* mark = Pat(self, 0x18);
            self[0x28] = 0;
            put4(self + 0x20, 0);
            cp12(self, mark + 0x18);
            cp12(self + 0xc, mark + 0x24);
            SkidObject_ReleaseSkid_o(Pat(self, 0x1c), 0, mark);
            Pat(self, 0x18) = 0;
        }
    }
    uint8_t* mark = Pat(self, 0x18);
    if (!mark) {
        mark = SkidObject_GetSkid_o(Pat(self, 0x1c), 0, Iat(self, 0x24));
        Pat(self, 0x18) = mark;
        if (!mark) return;
        cp12(mark, self);
        cp12(Pat(self, 0x18) + 0xc, self + 0xc);
        cp12(Pat(self, 0x18) + 0x18, self);
        cp12(Pat(self, 0x18) + 0x24, self + 0xc);
        return;
    }
    const float* m = (const float*)mark;
    float d[3], c[3];
    d[0] = (float)(D(at->x) - m[0]);
    d[1] = (float)(D(at->y) - m[1]);
    d[2] = (float)(D(at->z) - m[2]);
    c[0] = (float)(D(axis->z) * d[1] - D(axis->y) * d[2]);   // (a dead alias check)
    c[1] = (float)(D(axis->x) * d[2] - D(axis->z) * d[0]);
    c[2] = (float)(D(axis->y) * d[0] - D(axis->x) * d[1]);
    const double len = x87_sqrt((D(c[1]) * c[1] + D(c[2]) * c[2]) + D(c[0]) * c[0]);
    const float lf = st(len);                           // fst dword, before the test
    if (!(fabs(len) > FB(0x34000000))) return;          // fabs; fcomp 2^-23; test ah,0x41
    c[0] = (float)((D(c[0]) / lf) * FB(0x3dcccccd));    // 0.1 m
    c[1] = (float)((D(c[1]) / lf) * FB(0x3dcccccd));
    const uint8_t first = self[0x28];
    c[2] = (float)((D(c[2]) / lf) * FB(0x3dcccccd));
    if (first) {
        self[0x28] = 0;
        a[0] = (float)(D(m[0]) - c[0]);
        a[1] = (float)(D(m[1]) - c[1]);
        a[2] = (float)(D(m[2]) - c[2]);
        a[3] = (float)(D(m[3]) + c[0]);
        a[4] = (float)(D(m[4]) + c[1]);
        a[5] = (float)(D(m[5]) + c[2]);
        cp12(mark, self);
        cp12(Pat(self, 0x18) + 0xc, self + 0xc);
    }
    float* e = (float*)(Pat(self, 0x18) + 0x18);
    e[0] = (float)(D(at->x) - c[0]);
    e[1] = (float)(D(at->y) - c[1]);
    e[2] = (float)(D(at->z) - c[2]);
    e = (float*)(Pat(self, 0x18) + 0x24);
    e[0] = (float)(D(at->x) + c[0]);
    e[1] = (float)(D(at->y) + c[1]);
    e[2] = (float)(D(at->z) + c[2]);
}
static void fp_skid_on(Footprint& f, uint8_t* self, Edx, const P3*, const P3*) {
    f.add(self, 0x2c, "the SkidClient");
    uint8_t* mark = Pat(self, 0x18);
    if (mark) f.add(mark, 0x38, "its skid mark");
    uint8_t* owner = Pat(self, 0x1c);
    if (owner) {
        f.add(owner + 0xc, 4, "the SkidObject's next mark");
        fp_skid_marks(f, owner);
    }
}
PORT_FN(0x00468ae0, "SkidClient::SkidOn", SkidClient_SkidOn, fp_skid_on)

// SkidClient::SkidOff (0x468e20) and SkidClient::Clear (0x468e40), the same code: the mark is released
static void __fastcall SkidClient_SkidOff(uint8_t* self, Edx) {
    uint8_t* mark = Pat(self, 0x18);
    if (!mark) return;
    SkidObject_ReleaseSkid_o(Pat(self, 0x1c), 0, mark);
    Pat(self, 0x18) = 0;
}
static void fp_skid_off(Footprint& f, uint8_t* self, Edx) {
    f.add(self + 0x18, 4, "the SkidClient's mark");
    uint8_t* mark = Pat(self, 0x18);
    if (mark) f.add(mark + 0x30, 4, "the mark's state");
}
PORT_FN(0x00468e20, "SkidClient::SkidOff", SkidClient_SkidOff, fp_skid_off)

static void __fastcall SkidClient_Clear(uint8_t* self, Edx) {
    uint8_t* mark = Pat(self, 0x18);
    if (!mark) return;
    SkidObject_ReleaseSkid_o(Pat(self, 0x1c), 0, mark);
    Pat(self, 0x18) = 0;
}
PORT_FN(0x00468e40, "SkidClient::Clear", SkidClient_Clear, fp_skid_off)

// SkidMark::SkidMark (0x468f90): its four points' (empty) constructors
static uint8_t* __fastcall SkidMark_ctor(uint8_t* self, Edx) {
    uint8_t* p = self;
    for (int k = 3; k >= 0; k--, p += 0xc) P3DBase_ctor(p, 0);
    return self;
}
// (writes nothing; not marked pure only because the fuzzer compares the returned pointer across its two arenas)
static void fp_nothing(Footprint&, uint8_t*, Edx) {}
PORT_FN(0x00468f90, "SkidMark::SkidMark", SkidMark_ctor, fp_nothing)

// ==== the shared GraphObject / BuddyObject functions ============================================================
// BuddyObject::IsVisible (0x468e70) / GetZ (0x468e80): tail calls to the buddy's own (whatever it leaves in al /
// ST0 is the result: jumped to, exactly as the original)
static __declspec(naked) uint8_t __fastcall BuddyObject_IsVisible(void*, Edx) {
#ifdef VP_GCC
    __asm__ volatile("mov ecx, dword ptr [ecx + 4]\n\t"
                     "mov eax, dword ptr [ecx]\n\t"
                     "jmp dword ptr [eax + 0xc]" : :);
#else
    __asm { mov ecx, dword ptr [ecx + 4]
            mov eax, dword ptr [ecx]
            jmp dword ptr [eax + 0xc] }
#endif
}
static void fp_buddy(Footprint&, void*, Edx) {}
PORT_FN(0x00468e70, "BuddyObject::IsVisible", BuddyObject_IsVisible, fp_buddy)

static __declspec(naked) double __fastcall BuddyObject_GetZ(void*, Edx) {
#ifdef VP_GCC
    __asm__ volatile("mov ecx, dword ptr [ecx + 4]\n\t"
                     "mov eax, dword ptr [ecx]\n\t"
                     "jmp dword ptr [eax + 0x20]" : :);
#else
    __asm { mov ecx, dword ptr [ecx + 4]
            mov eax, dword ptr [ecx]
            jmp dword ptr [eax + 0x20] }
#endif
}
PORT_FN(0x00468e80, "BuddyObject::GetZ", BuddyObject_GetZ, fp_buddy)

// GraphObject::GetZ (0x468f30): "Not a Z-able object!", 0
static float __fastcall GraphObject_GetZ(void*, Edx) {
    LogPanic((const char*)0x004f4ea4);
    return 0.0f;
}
PORT_FN(0x00468f30, "GraphObject::GetZ", GraphObject_GetZ, fp_buddy)

// ==============================================================================================================
// debris.obj
// ==============================================================================================================

// ==== DebrisBegin (0x46e570) ===================================================================================
// The DebrisObject with its 16 SmokeObjects and the SparksObject, into the world; the two event handlers
static void __cdecl DebrisBegin() {
    uint8_t* d = (uint8_t*)MemAlloc(0x11c);
    if (d) {
        GraphObject_ctor(d, 0);
        for (int k = 0; k < 16; k++) P3DBase_ctor(d + 4 + k * 12, 0);
        put4(d, 0x004dd5d8);
        Iat(d, 0xc4) = 0;
        Iat(d, 0xc8) = 0;
        Iat(d, 0xcc) = 0;
        put4(d + 0x118, 0x49742400);                    // 1e6
        Iat(d, 0xd0) = gxGetTexture((const char*)0x004f531c, 0);   // "effects.tex"
        Iat(d, 0x114) = 0;
        for (int k = 0; k < 16; k++) {
            void* m = MemAlloc(0x58);
            *(void**)(d + 0xd4 + k * 4) = m ? SmokeObject_ctor_o(m, 0) : 0;
            WorldAddGob(*(void**)(d + 0xd4 + k * 4));
        }
        *P<uint8_t*>(S_DEBRIS) = d;
    } else {
        *P<uint8_t*>(S_DEBRIS) = 0;
    }
    uint8_t* s = (uint8_t*)MemAlloc(0x38c);
    if (s) {
        GraphObject_ctor(s, 0);
        for (int k = 0; k < 32; k++) Particle_ctor_o(s + 4 + k * 0x1c, 0);
        put4(s, 0x004dd5b0);
        Iat(s, 0x384) = 0;
        for (int k = 0; k < 32; k++) {
            uint8_t* p = s + 4 + k * 0x1c;
            put4(p + 0xc, 0);
            put4(p + 0x10, 0);
            put4(p + 0x14, 0);
            put4(p + 0x18, 0x49742400);                 // 1e6
        }
        Iat(s, 0x388) = gxGetTexture((const char*)0x004f531c, 0);
        *P<uint8_t*>(S_SPARKS) = s;
    } else {
        *P<uint8_t*>(S_SPARKS) = 0;
    }
    WorldAddGob(*P<void*>(S_DEBRIS));
    WorldAddGob(*P<void*>(S_SPARKS));
    EventInstallHandler(H_DEBRIS, 2);
    EventInstallHandler(H_SPARKS, 1);
}
static void fp_debris_begin(Footprint& f) { f.replay_only = "allocates the debris, smoke and sparks objects"; }
PORT_FN(0x0046e570, "DebrisBegin", DebrisBegin, fp_debris_begin)

// ==== GetParticlePosition (0x46e700): p0 + v t, and g t^2 (not halved) down ===================================
static void __cdecl GetParticlePosition(P3* out, const P3* p0, const P3* v, float t) {
    out->x = (float)(D(v->x) * t + p0->x);
    out->y = (float)(D(v->y) * t + p0->y);
    out->z = (float)(D(v->z) * t + p0->z);
    out->y = (float)((D(t) * t) * FB(0xc11cf5c3) + out->y);   // -9.81
}
static void fp_particle_pos(Footprint& f, P3*, const P3*, const P3*, float) { f.pure = true; }
PORT_FN(0x0046e700, "GetParticlePosition(debris.obj)", GetParticlePosition, fp_particle_pos)

// ==== DebrisEnd (0x46e750) ====================================================================================
static void __cdecl DebrisEnd() {
    EventUnInstallHandler(H_SPARKS);
    EventUnInstallHandler(H_DEBRIS);
}
static void fp_debris_end(Footprint& f) { fp_event_table(f); }
PORT_FN(0x0046e750, "DebrisEnd", DebrisEnd, fp_debris_end)

// ==== RandomReal (0x46e900): a + (b - a) x Random(1000) x 0.001001 (the ST0 of the last fadd) ================
static double __cdecl RandomReal(float a, float b) {
    const int32_t r = Random(1000);
    return (D(r) * FB(0x3a833405)) * (D(b) - a) + a;
}
static void fp_random_real(Footprint&, float, float) {}
PORT_FN(0x0046e900, "RandomReal(debris.obj)", RandomReal, fp_random_real)

// ==== debris_event (0x46e770) ==================================================================================
// A crash (tangential velocity v, point, relative velocity w): a smoke puff at the LAST crash point (the next of
// the 16, round); and, if the last burst was over half a second ago (or the clock went back), a new burst of 16
// debris at this point: 0.6 v, raised by 0.01 w.y clamped to 5..7, each spread by up to 0.6 x that across and
// 0.8..1.2 x it up.
static int __cdecl debris_event(const uint8_t* data) {
    uint8_t* d = *P<uint8_t*>(S_DEBRIS);
    if (!WorldShowSmoke()) return 0x24;
    const float* in = (const float*)data;
    P3 s;
    s.x = st(D(in[0]) * FB(0x3f19999a));                // 0.6 (stored whether or not there's a burst)
    s.z = st(D(in[2]) * FB(0x3f19999a));
    double up = D(in[7]) * FB(0x3c23d70a);              // 0.01
    up = 7.0f > up ? up : 7.0f;                         // fcom; test ah,0x41: a NaN gives 7
    up = !(5.0f >= up) ? up : 5.0f;                     // fcom; test ah,1
    s.y = st(D(in[1]) * FB(0x3f19999a) + up);
    uint32_t zero[3] = {0, 0, 0};
    SmokeObject_Puff_o(Pat(d, 0xd4 + 4 * Iat(d, 0x114)), 0, (const P3*)(d + 0xc4), (const P3*)zero, 0);
    const int32_t next = Iat(d, 0x114) + 1;
    Iat(d, 0x114) = next;
    if (next >= 16) Iat(d, 0x114) = 0;
    if (!(WorldGetElapsedTime() >= Fat(d, 0x118)) ||             // fcomp; test ah,1
        !(D(Fat(d, 0x118)) + 0.5f >= WorldGetElapsedTime())) {   // fcompp; test ah,1
        cp12(d + 0xc4, in + 3);
        Fat(d, 0x118) = (float)WorldGetElapsedTime();
        for (int k = 0; k < 16; k++) {
            float* v = (float*)(d + 4 + k * 12);
            cp12(v, &s);
            v[0] = (float)(RandomReal_o(0xbf19999a, 0x3f19999a) * s.y + v[0]);   // -0.6 .. 0.6
            v[2] = (float)(RandomReal_o(0xbf19999a, 0x3f19999a) * s.y + v[2]);
            v[1] = (float)(RandomReal_o(0x3f4ccccd, 0x3f99999a) * v[1]);         // 0.8 .. 1.2
        }
    }
    return 0x24;
}
static void fp_debris_event(Footprint& f, const uint8_t*) {
    uint8_t* d = *P<uint8_t*>(S_DEBRIS);
    if (!d) return;
    f.add(d, 0x11c, "the DebrisObject");
    const int32_t k = Iat(d, 0x114);
    if (k >= 0 && k < 16) {
        uint8_t* smoke = Pat(d, 0xd4 + 4 * k);
        if (smoke) {
            f.add(smoke, 0x58, "the next SmokeObject");
            fp_smoke_verts(f, smoke);
        }
    }
}
PORT_FN(0x0046e770, "debris_event", debris_event, fp_debris_event)

// ==== sparks_event (0x46e930) ==================================================================================
// A scrape (velocity v, point): unless the next of the 32 sparks is younger than half a second, it's thrown from
// the point at v/2, 0.1 |v| more upward, spread by up to 0.1 |v| each way. (The index moves on first: the spark
// written is the one checked.)
static int __cdecl sparks_event(const uint8_t* data) {
    uint8_t* s = *P<uint8_t*>(S_SPARKS);
    if (!WorldShowSmoke()) return 0x18;
    float* p = (float*)(s + Iat(s, 0x384) * 28 + 4);
    const double t = WorldGetElapsedTime();
    const float tf = st(t);
    if (t > p[6] && D(p[6]) + 0.5f > tf) return 0x18;  // fcom; test ah,0x41 / fcomp; test ah,0x41
    const int32_t next = Iat(s, 0x384) + 1;
    Iat(s, 0x384) = next;
    if (next >= 32) Iat(s, 0x384) = 0;
    const float* in = (const float*)data;
    const float L = (float)x87_sqrt((D(in[2]) * in[2] + D(in[1]) * in[1]) + D(in[0]) * in[0]);
    const float hx = (float)(D(in[0]) * 0.5f);
    const float hz = (float)(D(in[2]) * 0.5f);
    const double hyr = D(in[1]) * 0.5f + D(L) * FB(0x3dcccccd);
    cp4(&p[3], &in[3]);
    cp4(&p[4], &in[4]);
    const float hy = (float)hyr;
    cp4(&p[5], &in[5]);
    cp4(&p[6], &tf);
    cp4(&p[0], &hx);
    cp4(&p[1], &hy);
    cp4(&p[2], &hz);
    p[0] = (float)(RandomReal_o(0xbdcccccd, 0x3dcccccd) * L + p[0]);   // -0.1 .. 0.1
    p[2] = (float)(RandomReal_o(0xbdcccccd, 0x3dcccccd) * L + p[2]);
    p[1] = (float)(RandomReal_o(0xbdcccccd, 0x3dcccccd) * L + p[1]);
    return 0x18;
}
static void fp_sparks_event(Footprint& f, const uint8_t*) {
    uint8_t* s = *P<uint8_t*>(S_SPARKS);
    if (s) f.add(s, 0x38c, "the SparksObject");
}
PORT_FN(0x0046e930, "sparks_event", sparks_event, fp_sparks_event)

// ==== DebrisObject::IsVisible (0x46ea80): within 5 s of the last burst (and not before it) ===================
static uint8_t __fastcall DebrisObject_IsVisible(uint8_t* self, Edx) {
    const double t = WorldGetElapsedTime();
    const float tf = st(t);
    if (!(t >= Fat(self, 0x118))) return 0;             // fcom; test ah,1
    if (!(D(Fat(self, 0x118)) + 5.0f > tf)) return 0;  // fcomp; test ah,0x41
    return 1;
}
static void fp_debris_visible(Footprint&, uint8_t*, Edx) {}
PORT_FN(0x0046ea80, "DebrisObject::IsVisible", DebrisObject_IsVisible, fp_debris_visible)

// DebrisObject / SparksObject / SplashObject::GetZ (-1), IsAlpha, SparksObject::IsVisible
static float __fastcall Effect_GetZ(void*, Edx) { return -1.0f; }
PORT_FN(0x0046ead0, "DebrisObject::GetZ", Effect_GetZ, fp_const)
static uint8_t __fastcall Effect_True(void*, Edx) { return 1; }
PORT_FN(0x0046eae0, "DebrisObject::IsAlpha", Effect_True, fp_const)

static float __fastcall SparksObject_GetZ(void*, Edx) { return -1.0f; }
PORT_FN(0x0046eb60, "SparksObject::GetZ", SparksObject_GetZ, fp_const)
static uint8_t __fastcall SparksObject_IsVisible(void*, Edx) { return 1; }
PORT_FN(0x0046eb50, "SparksObject::IsVisible", SparksObject_IsVisible, fp_const)
static uint8_t __fastcall SparksObject_IsAlpha(void*, Edx) { return 1; }
PORT_FN(0x0046eb70, "SparksObject::IsAlpha", SparksObject_IsAlpha, fp_const)

// SparksObject::Particle::Particle (0x46eda0): nothing
static uint8_t* __fastcall Particle_ctor(uint8_t* self, Edx) { return self; }
PORT_FN(0x0046eda0, "SparksObject::Particle::Particle", Particle_ctor, fp_nothing)

// ==============================================================================================================
// splash.obj
// ==============================================================================================================

// ==== random_real (0x46f360) / get_random_v (0x46f320) ========================================================
// a + (b - a) x Random(100) / 100 (the ST0 of the last fadd; a stored as a float first)
static double __cdecl random_real(int a, int b) {
    const float af = (float)a;                          // fild; fstp dword
    const int32_t r = Random(100);
    return (D(r) * FB(0x3c23d70a)) * (D(b) - af) + af;
}
static void fp_random_real_i(Footprint&, int, int) {}
PORT_FN(0x0046f360, "random_real(splash.obj)", random_real, fp_random_real_i)

static void __cdecl get_random_v(P3* v) {
    v->x = (float)random_real_o(-5, 5);
    v->y = (float)random_real_o(-10, 10);
    v->z = (float)random_real_o(-5, 5);
}
static void fp_get_random_v(Footprint& f, P3* v) { f.add(v, 12, "the random vector"); }
PORT_FN(0x0046f320, "get_random_v", get_random_v, fp_get_random_v)

// ==== SplashObject::Splash (0x46efd0) =========================================================================
// Something (volume) hit the water at pos with velocity vel: a 3 x 5 fan of droplets. The speed is capped at
// 5 m/s, then the horizontal part takes 0.4 and the vertical is at most -4.905; a splash of less than 0.316 m/s
// across is nothing. The columns sit 1.5 m apart across the path; each droplet gets 1.4 x the horizontal,
// -1.2 x the vertical, a random jitter, and a sideways push; its life is 2 x the rise time.
static void __fastcall SplashObject_Splash(uint8_t* self, Edx, const P3* pos, const P3* vel, void* volume) {
    *(void**)(self + 4) = volume;
    float v[3];
    cp12(v, vel);
    cp12(self + 0xc, pos);
    const double L = x87_sqrt((D(v[1]) * v[1] + D(v[2]) * v[2]) + D(v[0]) * v[0]);
    const float lf = st(L);                             // (fstp dword, before the test)
    if (L > 5.0f) {                                     // fcom; test ah,0x41
        v[0] = (float)((D(v[0]) / lf) * 5.0f);
        v[1] = (float)((D(v[1]) / lf) * 5.0f);
        v[2] = (float)((D(v[2]) / lf) * 5.0f);
    }
    v[0] = (float)(D(v[0]) * FB(0x3ecccccd));           // 0.4
    v[2] = (float)(D(v[2]) * FB(0x3ecccccd));
    uint32_t vyb = Ub(&v[1]);
    if (vyb < 0xc09cf5c3u) vyb = 0xc09cf5c3u;           // an unsigned compare of the bits: at most -4.905
    float vy;
    memcpy(&vy, &vyb, 4);
    float px = fpu_neg(&v[0]);                          // [esp+0x34]
    const double m2 = D(v[2]) * v[2] + D(px) * px;
    const float m2f = st(m2);                           // (fstp dword, before the test)
    if (!(m2 >= FB(0x3dcccccd))) return;                // 0.1: fcom; test ah,1
    const double inv = 1.0f / x87_sqrt(D(m2f));
    const float pz = (float)(D(v[2]) * inv);            // [esp+0x2c]: across the path
    px = (float)(inv * px);
    for (int i = 0; i < 3; i++) {
        const double w = (D(i) * FB(0x3eaaaaab)) * 3.0f - 1.5f;
        const float ox = (float)(D(pz) * w);
        const float oz = (float)(w * px);
        for (int j = 0, e = i; j < 5; j++, e += 3) {
            uint8_t* vt = self + 0x18c + e * 32;
            Fat(vt, 0) = (float)(D(Fat(self, 0xc)) + ox);
            cp4(vt + 4, self + 0x10);
            Fat(vt, 8) = (float)(D(Fat(self, 0x14)) + oz);
            put4(vt + 0x10, 0xffffffffu);
        }
    }
    const float a = (float)(D(v[0]) * FB(0x3fb33333));  // 1.4
    const float b = (float)(D(vy) * FB(0xbf99999a));    // -1.2
    const float c = (float)(D(v[2]) * FB(0x3fb33333));
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 5; j++) {
            P3 r;                                       // (the original's v, reused)
            get_random_v_o(&r);
            float* bz = (float*)(self + 0x18 + (i + 3 * j) * 12);
            const double f = 1.0f - D(j) * FB(0x3e4ccccd);
            const double g = FB(0x3d4ccccd) + f;        // 0.05
            bz[0] = (float)(D(a) * g + r.x);
            *(volatile float*)&bz[1] = (float)(D(b) * g + r.y);   // (overwritten below)
            bz[2] = (float)(D(c) * g + r.z);
            const double h = f * 0.5f + 0.5f;
            const double e = (D(i) * FB(0x3eaaaaab)) * 2.0f - 1.0f;
            const double sh = e * h;
            bz[0] = (float)((sh * pz) * 10.0f + bz[0]);
            bz[2] = (float)((sh * px) * 10.0f + bz[2]);
            const double q = 1.0f - g;
            bz[1] = (float)((1.0f - q * q) * b);
        }
    }
    const double dur = (D(b) / FB(0x411cf5c3)) * 2.0f;  // 9.81
    put4(self + 0x378, 0x3f333333);                     // alpha 0.7
    Fat(self, 0x370) = (float)dur;
    Fat(self, 0x36c) = (float)WorldGetElapsedTime();
}
static void fp_splash(Footprint& f, uint8_t* self, Edx, const P3*, const P3*, void*) { f.add(self, 0x380, "the SplashObject"); }
PORT_FN(0x0046efd0, "SplashObject::Splash", SplashObject_Splash, fp_splash)

// ==== SplashObject::Update (0x46f3a0) =========================================================================
// Visible while the splash lasts: the droplets fall (9.81 dt off their rise), slow by a drag that grows down the
// rows, and move; the alpha (0.7 fading to 0) goes into each vertex's colour. Otherwise the volume is forgotten.
static void __fastcall SplashObject_Update(uint8_t* self, Edx) {
    const double t = WorldGetElapsedTime();
    const float tf = st(t);
    const float dt = st(t - Fat(self, 0x374));          // (stored whether or not the splash is on)
    uint8_t& vis = self[0x37c];
    vis = !(D(Fat(self, 0x36c)) >= tf) && D(Fat(self, 0x370)) + Fat(self, 0x36c) > tf ? 1 : 0;   // test ah,1 / 0x41
    if (vis) {
        const double al = (1.0f - (D(tf) - Fat(self, 0x36c)) / Fat(self, 0x370)) * FB(0x3f333333);
        Fat(self, 0x378) = (float)al;
        const int32_t a8 = x87_ftol(al * 255.0f);
        const float g = (float)(D(FB(0x411cf5c3)) * dt);
        const uint32_t hi = (uint32_t)(uint8_t)a8 << 24;
        for (int k = 0; k < 15; k++) {
            float* b = (float*)(self + 0x18 + k * 12);
            uint8_t* vt = self + 0x18c + k * 32;
            b[1] = (float)(D(b[1]) - g);
            const int32_t row = k / 3;
            const double damp = 1.0f - (1.0f - ((D(row) * FB(0x3e4ccccd)) * FB(0x3ee66666) + 0.5f)) * dt;
            b[0] = (float)(D(b[0]) * damp);
            b[1] = (float)(D(b[1]) * damp);
            b[2] = (float)(damp * b[2]);
            Fat(vt, 0) = (float)(D(b[0]) * dt + Fat(vt, 0));
            Fat(vt, 4) = (float)(D(b[1]) * dt + Fat(vt, 4));
            Fat(vt, 8) = (float)(D(b[2]) * dt + Fat(vt, 8));
            const uint32_t col = Ub(vt + 0x10) & 0xffffffu;
            put4(vt + 0x10, col);
            put4(vt + 0x10, col | hi);
        }
    } else {
        Iat(self, 4) = 0;
    }
    cp4(self + 0x374, &tf);
}
static void fp_splash_update(Footprint& f, uint8_t* self, Edx) { f.add(self, 0x380, "the SplashObject"); }
PORT_FN(0x0046f3a0, "SplashObject::Update", SplashObject_Update, fp_splash_update)

// ==== SplashObject::SplashObject (0x46f590) ===================================================================
// The grid's triangles (both windings), the vertices' colours and uv, and a place in g_tab (no bound: a 33rd
// splash would land on the count)
static uint8_t* __fastcall SplashObject_ctor(uint8_t* self, Edx) {
    GraphObject_ctor(self, 0);
    for (int k = 0; k < 15; k++) baz_ctor_o(self + 0x18 + k * 12, 0);
    put4(self, 0x004dd660);
    Iat(self, 4) = 0;
    self[0x37c] = 0;
    put4(self + 0x378, 0x3f333333);                     // 0.7 (from 0x4dd600)
    Iat(self, 0x370) = 0;
    put4(self + 0x36c, 0xc47a0000);                     // -1000
    Iat(self, 8) = TextureGet(*(const char* const*)0x004dd608, 0);   // "splash.tex"
    uint16_t* ix = (uint16_t*)(self + 0xcc);
    int n = 0;
    for (int r = 0; r < 12; r += 3) {
        for (int e = 0; e < 2; e++) {
            const uint16_t a = (uint16_t)(r + e), b = (uint16_t)(r + 3 + e), c = (uint16_t)(a + 1), d = (uint16_t)(a + 4);
            const uint16_t tri[12] = {a, b, c, b, a, c, b, c, d, c, b, d};
            memcpy(ix + n, tri, sizeof tri);
            n += 12;
        }
    }
    Random(100);                                        // (drawn and unused)
    for (int i = 0; i < 3; i++) {
        const float u = (float)(D(i) * FB(0x3eaaaaab));
        for (int j = 0, e = i; j < 5; j++, e += 3) {
            uint8_t* vt = self + 0x18c + e * 32;
            put4(vt + 0x10, 0xffff7f7fu);
            put4(vt + 0x14, 0xff000000u);
            cp4(vt + 0x18, &u);
            Fat(vt, 0x1c) = (float)(D(j) * FB(0x3e4ccccd));
        }
    }
    const int32_t cnt = *P<int32_t>(S_SPLASH_COUNT);
    *(uint8_t**)(uintptr_t)(S_SPLASH_TAB + (uint32_t)cnt * 4) = self;
    *P<int32_t>(S_SPLASH_COUNT) += 1;
    return self;
}
static void fp_splash_ctor(Footprint& f, uint8_t*, Edx) { f.replay_only = "loads the splash texture"; }
PORT_FN(0x0046f590, "SplashObject::SplashObject", SplashObject_ctor, fp_splash_ctor)

// ==== SplashObject::~SplashObject (0x46f730) ==================================================================
static void __fastcall SplashObject_dtor(uint8_t* self, Edx) {
    const int tex = Iat(self, 8);
    put4(self, 0x004dd660);
    TextureForget(tex);
    for (int i = 0; i < 32; i++) {
        uint8_t** p = (uint8_t**)(uintptr_t)(S_SPLASH_TAB + i * 4);
        if (*p == self) {
            *p = 0;
            break;
        }
    }
    GraphObject_dtor(self, 0);
}
static void fp_splash_dtor(Footprint& f, uint8_t*, Edx) { f.replay_only = "releases the splash texture"; }
PORT_FN(0x0046f730, "SplashObject::~SplashObject", SplashObject_dtor, fp_splash_dtor)

// ==== SplashObject::Handler (0x46f780) ========================================================================
// The splash event: unless a splash of this volume is still on (any of the 32 -- a null entry is dereferenced),
// the next splash round.
static int __cdecl SplashObject_Handler(const uint8_t* data) {
    uint8_t fresh = 1;
    for (uint32_t a = S_SPLASH_TAB; a < S_SPLASH_COUNT; a += 4) {
        uint8_t* p = *(uint8_t**)(uintptr_t)a;
        if (Ub(p + 4) == Ub(data + 0x18)) fresh = 0;
    }
    if (fresh) {
        int32_t* cnt = P<int32_t>(S_SPLASH_COUNT);
        *cnt = *cnt + 1;
        *cnt = *cnt & 0x1f;
        SplashObject_Splash_o(*(uint8_t**)(uintptr_t)(S_SPLASH_TAB + (uint32_t)*cnt * 4), 0, data + 0xc, data,
                              *(void* const*)(data + 0x18));
    }
    return 0x1c;
}
static void fp_splash_handler(Footprint& f, const uint8_t*) {
    f.add(P<uint8_t>(S_SPLASH_COUNT), 4, "SplashObject::g_tab.count");
    const int32_t k = (*P<int32_t>(S_SPLASH_COUNT) + 1) & 0x1f;
    uint8_t* p = *(uint8_t**)(uintptr_t)(S_SPLASH_TAB + (uint32_t)k * 4);
    if (p) f.add(p, 0x380, "the next SplashObject");
}
PORT_FN(0x0046f780, "SplashObject::Handler", SplashObject_Handler, fp_splash_handler)

// ==== SplashObject::Begin (0x46f7e0) / End (0x46f830), SplashBegin (0x46f840) / SplashEnd (0x46f850) =========
static void __cdecl SplashObject_Begin() {
    *P<int32_t>(S_SPLASH_COUNT) = 0;
    EventInstallHandler(H_SPLASH, 0);
    for (int k = 32; k; k--) {
        void* m = MemAlloc(0x380);
        WorldAddGob(m ? SplashObject_ctor_o(m, 0) : 0);
    }
}
static void fp_splash_begin(Footprint& f) { f.replay_only = "allocates the 32 splashes"; }
PORT_FN(0x0046f7e0, "SplashObject::Begin", SplashObject_Begin, fp_splash_begin)

static void __cdecl SplashObject_End() { EventUnInstallHandler(H_SPLASH); }
static void fp_splash_end(Footprint& f) { fp_event_table(f); }
PORT_FN(0x0046f830, "SplashObject::End", SplashObject_End, fp_splash_end)

static void __cdecl SplashBegin() { SplashObject_Begin_o(); }   // (a tail jump)
PORT_FN(0x0046f840, "SplashBegin", SplashBegin, fp_splash_begin)
static void __cdecl SplashEnd() { SplashObject_End_o(); }       // (a tail jump)
PORT_FN(0x0046f850, "SplashEnd", SplashEnd, fp_splash_end)

// ==== SplashObject::IsVisible (0x46f860), IsAlpha (0x46f870: alpha under 1 by its bits), GetZ (0x46f880) =====
static uint8_t __fastcall SplashObject_IsVisible(uint8_t* self, Edx) { return self[0x37c]; }
PORT_FN(0x0046f860, "SplashObject::IsVisible", SplashObject_IsVisible, fp_smoke_visible)
static uint8_t __fastcall SplashObject_IsAlpha(uint8_t* self, Edx) { return (int32_t)Ub(self + 0x378) < 0x3f800000 ? 1 : 0; }
PORT_FN(0x0046f870, "SplashObject::IsAlpha", SplashObject_IsAlpha, fp_smoke_visible)
static float __fastcall SplashObject_GetZ(void*, Edx) { return -1.0f; }
PORT_FN(0x0046f880, "SplashObject::GetZ", SplashObject_GetZ, fp_const)

// SplashObject::baz::baz (0x46f8b0): nothing
static uint8_t* __fastcall baz_ctor(uint8_t* self, Edx) { return self; }
PORT_FN(0x0046f8b0, "SplashObject::baz::baz", baz_ctor, fp_nothing)
