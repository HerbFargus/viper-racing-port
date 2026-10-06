// wld_draw.cpp -- M3 graphics stage, step G1, group G1e: the world's drawing, rewritten (library `world`): the
// functions the world stage left out because they draw.
//
//   world.obj    WorldDraw (a frame: the view, the objects -- opaque ones straight away, translucent ones sorted
//                far to near --, the mirror), draw_mirror, WorldDraw2D (the objects' 2D and the mirror's frame),
//                draw_physics_tris (a debug grid of terrain triangles under the camera)
//   view.obj     ViewDraw (the sky, then the track), ViewDrawMirror (a jump to TrackDraw)
//   graf.obj     GrafDraw (the track's scenery graph, then its camera-facing models), draw_tree
//   carwob.obj   CarObject::Draw (the cockpit or the body, the wheels, the x-ray view, the tyre dust),
//                CarObject::Draw2D (the name tag), DrawWheel (the wheel, the brake glow, tyre smoke and dust),
//                DrawXRay (the suspension arms and spindles), the deleting destructors of CarObject and
//                GhostCarObject
//   effects.obj  ShadowObject / SmokeObject / ReflectionObject / SkidObject ::Draw, and the deleting destructors of
//                ShadowObject, SmokeObject, ReflectionObject and SkidObject
//   wob.obj      ModelObject::Draw, WobbleObject::Draw, their deleting destructors
//   debris.obj   DebrisObject::Draw (a jump to WorldFXEnabled), SparksObject::Draw, their deleting destructors
//   splash.obj   SplashObject::Draw, its deleting destructor
//
// Not rewritten (can't be hooked, or can't run): the bare `ret` / `ret 4` stubs ASSERT_MSG (0x462750), ViewUpdate
// (0x466590), ViewDraw2D (0x466850), init_lights (0x466860), GraphObject::InitTextures (0x468e60), SkidObject::Update
// (0x468f60), GraphObject::Draw2D (0x469860), GraphObject::Update (0x469880), SkidClient::~SkidClient (0x46ce00),
// GhostCarObject::Draw2D (0x46ce10), DebrisObject::Update (0x46eb00), SparksObject::Update (0x46ed50) -- 1 or 3 bytes,
// still called by address where the originals call them; the $E initialisers (run by the CRT before any hook); the
// deleting destructors of the abstract bases BuddyObject (0x468e90), FrameObject (0x468ed0), GraphObject (0x469b40)
// and WorldObject (0x469b60): every constructor overwrites their vtables, so no live object has one.
//
// Everything they call -- the renderer (mr*, gx*), the terrain, the matrix helpers, the World* / View* getters, the
// game's CRT -- is called by its v1.0 address, and so are this group's own functions where one calls another (a
// hooked rewrite is what runs), virtual calls through the object's vtable.
//
// Threads: all of it runs on the MAIN thread (game_loop -> WorldDraw / WorldDraw2D), so no physics static is saved
// for it: each footprint lists what the function and its callees write. The callees here are the renderer, whose
// state is listed by fp_render (below); the DirectX calls under it are the emulation's outputs (docs/PORTING.md,
// "DirectDraw and Direct3D"). replay_only: the functions that call every graphics object's virtuals (WorldDraw,
// draw_mirror, WorldDraw2D), read the performance clock (WorldDraw, ViewDraw: PTimeNow is not a fed input), or
// draw the track in deferred mode (ViewDraw, ViewDrawMirror, GrafDraw, draw_tree: every surface goes into the
// deferred-draw pool and M1's texture buckets), and the deleting destructors (they free).
//
// The vertices. mrVertex (32 bytes, a D3DLVERTEX): x y z, +0xc reserved, +0x10 colour, +0x14 specular, +0x18 u,
// +0x1c v. The originals build their triangles on the stack and never write +0xc (nor u / v in draw_physics_tris):
// it is stack garbage in the original and uninitialised here too, and reaches DrawPrimitive's vertex data as such.
//
// Layouts (v1.0; the other groups' files have the evidence): CarObject as hook/wld_carobj.cpp; ShadowObject,
// ReflectionObject, SmokeObject, SkidObject, SparksObject, SplashObject as hook/wld_effects.cpp; ModelObject /
// WobbleObject (+4 Frame, +0x3c model) as hook/wld_cars.cpp; GrafNode as hook/wld_carobj.cpp; gxCanvas (+4 pixels,
// +0xc height, +0x10 pitch) from out/agents/r2d/report.md.
//
// Statics:
//   world.obj  0x5522d8 the graphics objects (M1 moves the list: m1_operand), 0x553368 their count, 0x553328
//              current_camera (a Frame), 0x55335c the camera type, 0x4f436c show physics tris, 0x55331c frames
//              drawn, 0x55406c the time they took, 0x5522b0 the mirror's detail, 0x4f4364 MIRROR_X, 0x554050 the
//              mirror frame's colour, 0x4f435c the profile id
//   view.obj   0x5571f0 the first frame's time, 0x5558a8 this frame's, 0x4f4c88 frames, 0x5558dc the camera
//              position, 0x555874 sky on, 0x555898 the sky model, 0x5558b8 its frame, 0x5558ac the projection's
//              field of view, 0x555864 the draw distance
//   graf.obj   0x558118 GrafDraw's frame, 0x55997c the root, 0x558108 / 0x558968 the camera-facing nodes (mode 1,
//              512 slots), 0x559998 / 0x558160 the upright facing nodes (mode 2, 514 slots), 0x4f5284 min LOD,
//              0x4f5290 LOD bias, 0x4f528c the clip distance (squared)
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
#define FB(b) __builtin_bit_cast(float, (uint32_t)(b))   // a float constant from its bits
#define DB(b) __builtin_bit_cast(double, (uint64_t)(b))  // a double constant from its bits
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

namespace {
template <typename T> static __forceinline T* P(uint32_t addr) { return (T*)(uintptr_t)addr; }
static __forceinline int32_t& I(uint32_t addr) { return *(int32_t*)(uintptr_t)addr; }
static __forceinline float& F(uint32_t addr) { return *(float*)(uintptr_t)addr; }
static __forceinline uint8_t& B(uint32_t addr) { return *(uint8_t*)(uintptr_t)addr; }
static __forceinline uint32_t U32(const void* p) { uint32_t u; memcpy(&u, p, 4); return u; }
static __forceinline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }      // an integer move of a float
static __forceinline void put4(void* d, uint32_t v) { memcpy(d, &v, 4); }
static __forceinline void cp12(void* d, const void* s) {       // three integer moves, x then y then z
    cp4(d, s);
    cp4((uint8_t*)d + 4, (const uint8_t*)s + 4);
    cp4((uint8_t*)d + 8, (const uint8_t*)s + 8);
}
static __forceinline float& Fat(void* p, uint32_t off) { return *(float*)((uint8_t*)p + off); }
static __forceinline int32_t& Iat(void* p, uint32_t off) { return *(int32_t*)((uint8_t*)p + off); }
static __forceinline uint8_t*& Pat(void* p, uint32_t off) { return *(uint8_t**)((uint8_t*)p + off); }

// fld dword; fchs; fstp dword -- through the FPU, as the original does (a signalling NaN comes out quiet)
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

// DrawXRay's arms: fld y; fld x; fpatan; [fchs]; fld st0; fcos; fxch; fsin; fld st1; fstp c; fst s; fchs; fstp ns --
// the angle and its cosine and sine never leave the x87 (docs/PORTING.md 9: fpatan / fsin / fcos aren't rounded)
static __forceinline void atan_roll(double y, double x, float* c, float* s, float* ns) {
    float vc, vs, vn;
#ifdef VP_GCC
    __asm__ volatile("fld %[y]\n\t"
                     "fld %[x]\n\t"
                     "fpatan\n\t"
                     "fld st(0)\n\t"
                     "fcos\n\t"
                     "fxch st(1)\n\t"
                     "fsin\n\t"
                     "fld st(1)\n\t"
                     "fstp %[vc]\n\t"
                     "fst %[vs]\n\t"
                     "fchs\n\t"
                     "fstp %[vn]\n\t"
                     "fstp st(0)"
                     : [vc] "=m"(vc), [vs] "=m"(vs), [vn] "=m"(vn)
                     : [y] "m"(y), [x] "m"(x)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld y
            fld x
            fpatan
            fld st(0)
            fcos
            fxch st(1)
            fsin
            fld st(1)
            fstp vc
            fst vs
            fchs
            fstp vn
            fstp st(0) }
#endif
    *c = vc; *s = vs; *ns = vn;
}
static __forceinline void atan_roll_neg(double y, double x, float* c, float* s, float* ns) {   // (with the fchs)
    float vc, vs, vn;
#ifdef VP_GCC
    __asm__ volatile("fld %[y]\n\t"
                     "fld %[x]\n\t"
                     "fpatan\n\t"
                     "fchs\n\t"
                     "fld st(0)\n\t"
                     "fcos\n\t"
                     "fxch st(1)\n\t"
                     "fsin\n\t"
                     "fld st(1)\n\t"
                     "fstp %[vc]\n\t"
                     "fst %[vs]\n\t"
                     "fchs\n\t"
                     "fstp %[vn]\n\t"
                     "fstp st(0)"
                     : [vc] "=m"(vc), [vs] "=m"(vs), [vn] "=m"(vn)
                     : [y] "m"(y), [x] "m"(x)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld y
            fld x
            fpatan
            fchs
            fld st(0)
            fcos
            fxch st(1)
            fsin
            fld st(1)
            fstp vc
            fst vs
            fchs
            fstp vn
            fstp st(0) }
#endif
    *c = vc; *s = vs; *ns = vn;
}
// the spindles: fld a; fld st0; fcos; fxch; fsin; fld st1; fstp c; fld st0; fchs; fstp ns; fstp s; fstp c
static __forceinline void yaw_cs(double a, float* c, float* s, float* ns) {
    float vc, vs, vn, c2;
#ifdef VP_GCC
    __asm__ volatile("fld %[a]\n\t"
                     "fld st(0)\n\t"
                     "fcos\n\t"
                     "fxch st(1)\n\t"
                     "fsin\n\t"
                     "fld st(1)\n\t"
                     "fstp %[vc]\n\t"
                     "fld st(0)\n\t"
                     "fchs\n\t"
                     "fstp %[vn]\n\t"
                     "fstp %[vs]\n\t"
                     "fstp %[c2]"
                     : [vc] "=m"(vc), [vn] "=m"(vn), [vs] "=m"(vs), [c2] "=m"(c2)
                     : [a] "m"(a)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld a
            fld st(0)
            fcos
            fxch st(1)
            fsin
            fld st(1)
            fstp vc
            fld st(0)
            fchs
            fstp vn
            fstp vs
            fstp c2 }
#endif
    *c = vc; *s = vs; *ns = vn;
    (void)c2;
}

struct Vtx {                                         // mrVertex (a D3DLVERTEX)
    float x, y, z;
    uint32_t rhw;                                    // +0x0c never written (see the head of the file)
    uint32_t color, spec;                            // +0x10, +0x14
    uint32_t u, v;                                   // +0x18, +0x1c (bits)
};
static_assert(sizeof(Vtx) == 32, "Vtx");

struct mrModelInfo {
    int32_t nverts;  uint8_t* verts;                 // +0x00  mrVertex, 32 bytes
    int32_t nmats;   uint8_t* mats;                  // +0x08
    int32_t ntris;   uint8_t* tris;                  // +0x10
    int32_t n18;     uint8_t* p1c;                   // +0x18
    int32_t n20;     uint8_t* p24;                   // +0x20
};
static_assert(sizeof(mrModelInfo) == 0x28, "mrModelInfo");

struct WheelMsg {                      // WheelMessage (phys_wheel.cpp), as the renderer reads it
    P3 hub_pos;                        // +0x00
    float hub_height;                  // +0x0c
    float steer_angle, camber;         // +0x10
    float omega, spin_angle, slide;    // +0x18
    float lat_force_ratio, long_force_ratio, compression_fraction, brake_temp;   // +0x24
    uint8_t fx_flags, _35[3];          // +0x34  0x2 the smoke's second row, 0x4 dust, 0x6 smoke
};
static_assert(sizeof(WheelMsg) == 0x38, "WheelMsg");
struct LodRow { float dist; int32_t faces; uint8_t spec, alpha, _a[2]; };   // faces: the wheels' LOD (-1: none)
static_assert(sizeof(LodRow) == 12, "LodRow");
struct DustParticle { P3 pos, vel; float life; };   // 0x1c
struct WheelModels { int32_t fwheel_front, wheel_front, fwheel_rear, wheel_rear; };
struct SkidClient { uint8_t raw[0x2c]; };

struct CarObject {                     // (hook/wld_carobj.cpp)
    void** vtable;                     // +0x000
    Frame frame;                       // +0x004
    int32_t phob;                      // +0x034
    uint32_t _038;                     // +0x038
    uint8_t msg[0x18c];                // +0x03c  the last CarMessage
    int32_t index;                     // +0x1c8
    char skin[16];                     // +0x1cc
    int32_t lod_model[8];              // +0x1dc
    int32_t lod_copy[8];               // +0x1fc
    int32_t shadow_model;              // +0x21c
    LodRow lod[8];                     // +0x220
    int32_t num_lods;                  // +0x280
    int32_t* lod_models_p;             // +0x284
    LodRow* lod_p;                     // +0x288
    int32_t* num_lods_p;               // +0x28c
    int32_t needle[2];                 // +0x290
    uint8_t has_cockpit, _299[3];      // +0x298
    int32_t cockpit_c, cockpit_w;      // +0x29c
    int32_t arm[6];                    // +0x2a4  ul, ur, ll, lr, sl, sr
    int32_t arm_copy[2];               // +0x2bc  copies of arm_sl, arm_sr
    int32_t diskglow;                  // +0x2c4
    int32_t spin[2];                   // +0x2c8
    int32_t brakelight;                // +0x2d0
    float cockpit_row1[3];             // +0x2d4
    float cockpit_row3[3];             // +0x2e0
    float cockpit_row2[3];             // +0x2ec
    float needle2_min, needle2_scale;  // +0x2f8
    float needle1_min, needle1_scale;  // +0x300
    int32_t skin_tex, src_tex, damage_tex, effects_tex, xray_tex;   // +0x308
    char remap_name[16];               // +0x31c
    char remap_skin[16];               // +0x32c
    int32_t remap_car;                 // +0x33c
    int32_t* remap_tex;                // +0x340
    DustParticle dust[16];             // +0x344
    int32_t dust_next;                 // +0x504
    float dust_time;                   // +0x508
    float cam_dist;                    // +0x50c
    int32_t lod_now, lod_now2;         // +0x510
    WheelModels wheel_models[3];       // +0x518
    Frame wheel_frame[4];              // +0x548
    Frame wheel_frame2[4];             // +0x608
    P3 wheel_move[4];                  // +0x6c8
    SkidClient skid[4];                // +0x6f8
    uint32_t _7a8;                     // +0x7a8
    float radius_front, radius_rear;   // +0x7ac
    void* stamp;                       // +0x7b4
    uint8_t* smoke[32];                // +0x7b8
    int32_t num_smoke;                 // +0x838
    int32_t view_mode, last_view_mode; // +0x83c
};
static_assert(offsetof(CarObject, index) == 0x1c8 && offsetof(CarObject, lod_models_p) == 0x284 &&
              offsetof(CarObject, has_cockpit) == 0x298 && offsetof(CarObject, arm) == 0x2a4 &&
              offsetof(CarObject, brakelight) == 0x2d0 && offsetof(CarObject, needle1_scale) == 0x304 &&
              offsetof(CarObject, effects_tex) == 0x314 && offsetof(CarObject, dust) == 0x344 &&
              offsetof(CarObject, dust_time) == 0x508 && offsetof(CarObject, lod_now) == 0x510 &&
              offsetof(CarObject, wheel_models) == 0x518 && offsetof(CarObject, wheel_frame) == 0x548 &&
              offsetof(CarObject, wheel_move) == 0x6c8 && offsetof(CarObject, radius_front) == 0x7ac &&
              offsetof(CarObject, stamp) == 0x7b4 && offsetof(CarObject, smoke) == 0x7b8 &&
              offsetof(CarObject, num_smoke) == 0x838 && sizeof(CarObject) == 0x844, "CarObject");
static __forceinline WheelMsg* wheel_msg(CarObject* c, int i) { return (WheelMsg*)(c->msg + 0x78 + 0x38 * i); }
static __forceinline float& opacity(CarObject* c) { return *(float*)(c->msg + 0x158); }           // +0x194

struct AlphaItem { uint8_t* obj; uint32_t z; };    // WorldDraw's translucent objects, far to near
}  // namespace

enum : uint32_t {
    S_CAMERA = 0x00553328,              // current_camera (a Frame): rotation, then position (+0x24)
    S_CAMERA_POS = 0x0055334c,
    S_GOBS_OP_DRAW = 0x004622a3,        // WorldDraw's `mov edi, 0x5522d8` (M1 moves the list)
    S_GOBS_OP_MIRROR = 0x004624c8,      // draw_mirror's
    S_GOBS_OP_2D = 0x00462596,          // WorldDraw2D's
    S_NUM_GOBS = 0x00553368,
    S_CAMTYPE = 0x0055335c, S_SHOW_PHYS_TRIS = 0x004f436c, S_FRAMES_DRAWN = 0x0055331c, S_DRAW_TIME = 0x0055406c,
    S_MIRROR_DETAIL = 0x005522b0, S_MIRROR_X = 0x004f4364, S_MIRROR_COLOR = 0x00554050, S_PROFILE = 0x004f435c,
    S_SCREEN_WID = 0x005228f4, S_SCREEN_HIT = 0x005228d4,
    // view.obj
    S_FIRST_TIME = 0x005571f0, S_FRAME_TIME = 0x005558a8, S_VIEW_FRAMES = 0x004f4c88, S_VIEW_POS = 0x005558dc,
    S_SKY_ON = 0x00555874, S_SKY_MODEL = 0x00555898, S_SKY_FRAME = 0x005558b8, S_FOV = 0x005558ac,
    S_DRAW_DIST = 0x00555864,
    // graf.obj
    S_GRAF_FRAME = 0x00558118, S_GRAF_ROOT = 0x0055997c, S_FACING_N = 0x00558108, S_UPRIGHT_N = 0x00559998,
    S_FACING_STOCK = 0x00558968, S_UPRIGHT_STOCK = 0x00558160,       // the lists' v1.0 homes (M1 moves them)
    S_FACING_OP_TREE = 0x0046e04a, S_UPRIGHT_OP_TREE = 0x0046e066,   // draw_tree's stores: mov [eax*4 + list], esi
    S_FACING_OP_DRAW = 0x0046dcb7, S_UPRIGHT_OP_DRAW = 0x0046ddcb,   // GrafDraw's loops: mov ebp / esi, list
    S_MIN_LOD = 0x004f5284, S_LOD_BIAS = 0x004f5290,
    S_GRAF_CLIP = 0x004f528c,
    // mr / light
    S_ALPHA_LEVEL = 0x00522a1c,         // mrCaps +4: 0 no alpha, 1 some blends, 2 all
    S_CANVAS = 0x004efbf8,              // gx's current canvas
    S_CAR_TAG_COLOR = 0x0055803c,
};

// ---- the functions they call, by address -------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
typedef void(__cdecl* VoidI_t)(int);
typedef void(__cdecl* VoidP_t)(const void*);
typedef uint8_t(__cdecl* Byte_t)();
typedef int(__cdecl* Int_t)();
typedef double(__cdecl* Real_t)();                  // a float left in ST0: a register value
typedef void(__cdecl* Prof_t)(int, const char*, int);
static const Prof_t SingleEnter = (Prof_t)0x00415000;
static const Prof_t SingleLeave = (Prof_t)0x00415070;
static const Int_t PTimeNow = (Int_t)0x00413b40;
typedef void(__cdecl* SetView_t)(int, int, int, int, uint8_t);
static const SetView_t mrSetView = (SetView_t)0x0044ecd0;
static const VoidP_t mrSetCamera = (VoidP_t)0x0044ee70;
static const VoidP_t ViewUpdate = (VoidP_t)0x00466590;
static const VoidP_t ViewDraw_o = (VoidP_t)0x004665a0;
static const Void_t draw_physics_tris_o = (Void_t)0x00462880;
static const Void_t draw_mirror_o = (Void_t)0x00462390;
static const Void_t mrModelFinishDeferredMode = (Void_t)0x00455ce0;
static const Void_t mrEndFrame = (Void_t)0x0044e980;
static const Void_t mrBeginFrame = (Void_t)0x0044e970;
static const Void_t ViewEnterMirrorMode = (Void_t)0x004666f0;
static const Void_t ViewLeaveMirrorMode = (Void_t)0x00466780;
static const Void_t ViewDrawMirror_o = (Void_t)0x00466840;
static const Void_t ViewDraw2D = (Void_t)0x00466850;
static const Void_t TrackDraw = (Void_t)0x004695f0;
static const Void_t mrModelEnterDeferredMode = (Void_t)0x00455cc0;
static const Void_t mrModelLeaveDeferredMode = (Void_t)0x00455cd0;
static const Void_t mrEndTriangles = (Void_t)0x0044f420;
static const Void_t mrPushState = (Void_t)0x00457ea0;
static const Void_t mrPopState = (Void_t)0x00457ed0;
static const Void_t mrModelClearAlpha = (Void_t)0x004570b0;
static const Byte_t should_draw_mirror = (Byte_t)0x00463f90;
static const Byte_t WorldFXEnabled = (Byte_t)0x00462860;
static const Byte_t ViewIsMirrorMode = (Byte_t)0x004667f0;
static const Byte_t PhysicsIsPaused = (Byte_t)0x0042bd20;
static const Byte_t MultiEnabled = (Byte_t)0x004a23c0;
static const Byte_t HackShowCarStatusInfo = (Byte_t)0x0040d4c0;
static const Int_t WorldGetFocusCar = (Int_t)0x004626d0;
static const Int_t WorldGetCameraType = (Int_t)0x004626e0;
static const Int_t WorldShowSmoke = (Int_t)0x00461fc0;
static const Int_t WorldShowSpecular = (Int_t)0x00462000;
static const Real_t WorldGetElapsedTime = (Real_t)0x00462050;
static const Real_t ViewGetDynoClipDist = (Real_t)0x00466820;
static const Real_t ViewGetStaticClipDist = (Real_t)0x00466830;
typedef void*(__cdecl* Memmove_t)(void*, const void*, size_t);
static const Memmove_t game_memmove = (Memmove_t)0x004cf400;
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t game_sprintf = (Sprintf_t)0x004cf0a0;
typedef void(__cdecl* Panic_t)(const char*, ...);
static const Panic_t LogPanic = (Panic_t)0x004112b0;
typedef void(__cdecl* OpDelete_t)(void*);
static const OpDelete_t OpDelete = (OpDelete_t)0x00414390;
// the renderer (floats passed as their bits, as the originals push them)
static const VoidI_t mrEnable = (VoidI_t)0x00457890;
static const VoidI_t mrDisable = (VoidI_t)0x00457b00;
typedef uint8_t(__cdecl* ByteI_t)(int);
static const ByteI_t mrIsEnabled = (ByteI_t)0x00457d80;
static const VoidI_t mrSetBlendMode = (VoidI_t)0x00457e30;
static const VoidI_t mrModelEnvMap = (VoidI_t)0x00455cf0;
static const VoidI_t mrBeginTriangles = (VoidI_t)0x0044f360;
static const VoidP_t mrDrawTriangle = (VoidP_t)0x0044f3d0;
typedef void(__cdecl* DrawIndex_t)(const void*, int, const uint16_t*, int);
static const DrawIndex_t mrDrawIndexTriangles = (DrawIndex_t)0x0044f400;
typedef void(__cdecl* U1_t)(uint32_t);
typedef void(__cdecl* U2_t)(uint32_t, uint32_t);
typedef void(__cdecl* U3_t)(uint32_t, uint32_t, uint32_t);
static const U1_t mrModelSetAlpha = (U1_t)0x00457090;
static const U2_t mrModelSetClipDist = (U2_t)0x004570d0;
static const U3_t mrSetProjection = (U3_t)0x0044ede0;
typedef void(__cdecl* ModelDraw_t)(int, const void*);
static const ModelDraw_t mrModelDraw = (ModelDraw_t)0x00455d00;
static const VoidI_t mrModelDrawDeferred = (VoidI_t)0x00455ec0;       // mrModelDraw(int): the model's own frame
typedef void(__cdecl* ModelBuild_t)(int, void*, int);
static const ModelBuild_t mrModelBuild = (ModelBuild_t)0x00455c80;
static const VoidP_t mrModelSetFrame = (VoidP_t)0x004562d0;
typedef P3*(__cdecl* CamPos_t)();
static const CamPos_t mrGetCameraPos = (CamPos_t)0x0044efb0;
typedef double(__cdecl* CamDist_t)(const void*);
static const CamDist_t mrCameraDistanceSqr = (CamDist_t)0x0044f190;
typedef uint8_t(__cdecl* SphereSee_t)(const void*, uint32_t);
static const SphereSee_t mrSphereCanSee = (SphereSee_t)0x0044f2e0;
typedef void(__cdecl* LightSpecial_t)(int, const void*);
static const LightSpecial_t mrLightSpecial = (LightSpecial_t)0x0045c050;
typedef void(__cdecl* GetVerts_t)(int, uint8_t**, int*);
static const GetVerts_t mrModelGetVerts = (GetVerts_t)0x00457050;
typedef mrModelInfo*(__cdecl* GetInfo_t)(int);
static const GetInfo_t mrModelGetInfo = (GetInfo_t)0x00457030;
typedef uint8_t(__cdecl* Project_t)(const P3*, P3*);
static const Project_t mrProjectPoint = (Project_t)0x0044f200;
// 2D
typedef void(__cdecl* Line_t)(int, int, int, int, uint32_t);
static const Line_t gxLine = (Line_t)0x00452790;
typedef void*(__cdecl* SetCanvas_t)(void*);
static const SetCanvas_t gxSetCanvas = (SetCanvas_t)0x0044fe30;
typedef void(__cdecl* TextC_t)(int, int, const char*, uint32_t);
static const TextC_t gxTextCentered = (TextC_t)0x00453c40;
typedef void(__cdecl* Stamp_t)(const void*, int, int, int, int);
static const Stamp_t gxDrawStamp = (Stamp_t)0x004535f0;
typedef uint8_t*(__cdecl* CarInfo_t)(int);
static const CarInfo_t CarMgrGetInfo = (CarInfo_t)0x00464490;
// the terrain
typedef uint8_t(__cdecl* Height_t)(P3*);
static const Height_t TerrainGetHeight = (Height_t)0x00465b30;
typedef uint8_t(__cdecl* Isect4_t)(const P3*, const P3*, P3*, P3*);
static const Isect4_t TerrainGetIntersection4 = (Isect4_t)0x00465b90;
typedef uint8_t(__cdecl* Isect5_t)(const P3*, const P3*, P3*, P3*, int32_t*);
static const Isect5_t TerrainGetIntersection5 = (Isect5_t)0x00465be0;
// maths
typedef void(__cdecl* MulPoint_t)(P3*, const P3*, const void*);
static const MulPoint_t MatrixMulPoint = (MulPoint_t)0x00429420;
static const MulPoint_t MatrixMulPointInv = (MulPoint_t)0x00436120;
typedef void(__cdecl* MakeRot_t)(void*, uint32_t);
static const MakeRot_t MatrixMakeYaw = (MakeRot_t)0x00402f80;
static const MakeRot_t MatrixMakePitch = (MakeRot_t)0x00402fc0;
static const MakeRot_t MatrixMakeRoll = (MakeRot_t)0x00403000;
typedef void(__cdecl* Concat_t)(void*, const void*, const void*);
static const Concat_t MatrixConcat = (Concat_t)0x00403040;
typedef void(__cdecl* Vec3_t)(P3*, const P3*, const P3*);
static const Vec3_t VectorAdd = (Vec3_t)0x0043b0f0;
static const Vec3_t CrossProduct = (Vec3_t)0x0043b120;
typedef void(__cdecl* Interp_t)(P3*, const P3*, uint32_t);
static const Interp_t interpolate = (Interp_t)0x00468780;
typedef void(__cdecl* Footensor_t)(void*, const P3*, const P3*);
static const Footensor_t footensor = (Footensor_t)0x004683b0;
typedef void(__cdecl* PartPos_t)(P3*, const P3*, const P3*, uint32_t);
static const PartPos_t GetParticlePosition_car = (PartPos_t)0x0046ad30;       // carwob.obj's
static const PartPos_t GetParticlePosition_debris = (PartPos_t)0x0046e700;    // debris.obj's
typedef double(__cdecl* RandomReal_t)(uint32_t, uint32_t);                     // (float, float) as bits; ST0 back
static const RandomReal_t RandomReal_car = (RandomReal_t)0x0046b450;
// the objects' own functions
typedef void(__fastcall* This_t)(void*, Edx);
static const This_t SkidObject_Clear = (This_t)0x00468860;
typedef int(__fastcall* FindSmoke_t)(void*, Edx);
static const FindSmoke_t CarObject_find_available_smoke = (FindSmoke_t)0x0046bca0;
typedef void(__fastcall* Puff_t)(void*, Edx, const P3*, const P3*, uint8_t);
static const Puff_t SmokeObject_Puff = (Puff_t)0x00467fa0;
typedef void(__fastcall* DrawWheel_t)(void*, Edx, int, uint32_t, uint8_t);
static const DrawWheel_t CarObject_DrawWheel_o = (DrawWheel_t)0x0046af10;
static const This_t CarObject_DrawXRay_o = (This_t)0x0046b480;
static const This_t GraphObject_dtor = (This_t)0x00469850;
static const VoidI_t gxForgetTexture = (VoidI_t)0x0044e280;
typedef void(__cdecl* DrawTree_t)(uint8_t*);
static const DrawTree_t draw_tree_o = (DrawTree_t)0x0046dff0;

// ---- footprints ----------------------------------------------------------------------------------------------
// The renderer's state a draw can change (from a static scan of every absolute store in the functions reachable from
// the mr* calls these functions make): the model layer's flags (0x4f2184..), the feature stack and its index,
// dx's texture cache (0x4f2324..), the lighting function pointers, the texture counters, the projection and camera
// matrices, the view (mrView 0x5229a0: viewport, projection, dxState), the model clip distances and deferred flag
// (0x522d28..), the feature stack (0x522df0..), dx_result, dxProjectPoint's transform data (0x522f0c..), the texture
// table (M1 moves it: m1_operand), light.obj's statics (0x525f50..0x5261c8), the dxState cache (0x5265f4..) and the
// lit-vertex buffer (*0x4f2198: 48000 bytes stock, 1 MB once mr_model_begin's rewrite lifts it). Not listed: a texture loaded on a cache miss (its surfaces, the
// resource and file code under it: an allocation) and the deferred-draw pool (only in deferred mode: GrafDraw).
static void fp_render(Footprint& f) {
    f.add(P<uint8_t>(0x004f2184), 0x10, "mr: model alpha / deferred / env-map / draw flags");
    f.add(P<uint8_t>(0x004f22cc), 4, "mr: the feature stack's index");
    f.add(P<uint8_t>(0x004f2324), 0xc, "dx: vertex alpha, the current texture, its dirty flag");
    f.add(P<uint8_t>(0x004f3274), 4, "texture: the frame's selection flag");
    f.add(P<uint8_t>(0x004f38f4), 8, "light: the surface lighting functions");
    f.add(P<uint8_t>(0x004f3e70), 0x18, "texture: the video / system memory counters");
    f.add(P<uint8_t>(0x005228f8), 0xa8, "mr: the projection and camera matrices");
    f.add(P<uint8_t>(0x005229a0), 0x50, "mr: the view (viewport, projection, dxState)");
    f.add(P<uint8_t>(0x00522d28), 0x60, "mr: the model frame, clip distances, deferred flag");
    f.add(P<uint8_t>(0x00522df0), 0x70, "mr: the feature stack");
    f.add(P<uint8_t>(0x00522e6c), 4, "dx_result");
    f.add(P<uint8_t>(0x00522f0c), 0x84, "dx: dxProjectPoint's transform data");
    const uint32_t tex = m1_operand(0x0045a0f8) - 0x20;          // TextureGet: [eax + table+0x20]
    const uint32_t ntex = m1_operand(0x0045a60c);                 // tc_add: cmp edx, capacity
    f.add(P<uint8_t>(tex), ntex * 0x28, "the texture table");
    f.add(P<uint8_t>(0x00525f50), 0x278, "light: the lighting state");
    f.add(P<uint8_t>(0x005265f4), 0x30, "dx: the dxState cache");
    uint8_t* lit = *P<uint8_t*>(0x004f2198);
    if (lit) f.add(lit, vp_g_lit_buf_bytes, "mr: the lit-vertex buffer");      // (48000 bytes stock; port.h)
}
static void fp_terrain(Footprint& f) { f.add(P<uint8_t>(0x004f50c8), 4, "terrain: BPPFinder's last node"); }

// ==============================================================================================================
// world.obj
// ==============================================================================================================

// ==== WorldDraw (0x462200) ======================================================================================
// The view (below the dashboard's 64 rows unless `full`), the camera, the sky and track (ViewDraw), the physics
// triangles (camera 11 with the option), then every graphics object: Update, and if visible either Draw (opaque)
// or into a list sorted far to near by GetZ, drawn after the rest; the deferred surfaces; the time it took; the
// mirror. The vtable is read once per object for the first four calls (the original keeps it in ebx).
// FIX CANDIDATE: the translucent list is 1024 entries on the original's stack (0x2000 bytes) with no bound, and M1
// lifts the object list to 8192: more than 1024 translucent objects overwrite its saved registers and return address.
// The rewrite's list holds 8192, so it matches the original wherever the original survives.
static AlphaItem g_alpha[8192];
static void __cdecl WorldDraw(uint8_t full) {
    SingleEnter(I(S_PROFILE), 0, 0);
    const int t0 = PTimeNow();
    const int top = full == 0 ? 0x40 : 0;                // cmp byte, 1; sbb; and 0x40
    mrSetView(0, top, I(S_SCREEN_WID), I(S_SCREEN_HIT) - top, 1);
    mrSetCamera(P<void>(S_CAMERA));
    ViewUpdate(P<void>(S_CAMERA));
    ViewDraw_o(P<void>(S_CAMERA));
    if (I(S_CAMTYPE) == 0xb && B(S_SHOW_PHYS_TRIS) != 0) draw_physics_tris_o();
    int n = 0;
    uint8_t** list = P<uint8_t*>(m1_operand(0x004622a3));
    for (int i = 0; I(S_NUM_GOBS) > i; i++) {
        uint8_t* obj = list[i];
        void** vt = *(void***)obj;
        ((void(__fastcall*)(void*, Edx))vt[2])(obj, 0);                          // Update
        if (!((uint8_t(__fastcall*)(void*, Edx))vt[3])(obj, 0)) continue;         // IsVisible
        if (((uint8_t(__fastcall*)(void*, Edx))vt[4])(obj, 0)) {                   // IsAlpha
            const float z = (float)((double(__fastcall*)(void*, Edx))vt[8])(obj, 0);   // GetZ (fstp dword)
            int k = 0;
            if (n > 0)
                for (; !(z > *(const float*)&g_alpha[k].z);)                     // fcom; test ah,0x41
                    if (!(n > ++k)) break;
            game_memmove(&g_alpha[k + 1], &g_alpha[k], (size_t)((n - k) << 3));
            n++;
            g_alpha[k].obj = obj;
            g_alpha[k].z = fbits(z);
        } else {
            ((void(__fastcall*)(void*, Edx))vt[5])(obj, 0);                          // Draw
        }
    }
    for (int k = 0; k < n; k++) VFN(g_alpha[k].obj, 0x14, void)(g_alpha[k].obj, 0);
    mrModelFinishDeferredMode();
    const int dt = PTimeNow() - t0;
    I(S_FRAMES_DRAWN)++;
    I(S_DRAW_TIME) += dt;
    draw_mirror_o();
    SingleLeave(I(S_PROFILE), 0, 0);
}
static void fp_world_draw(Footprint& f, uint8_t) {
    f.replay_only = "calls every graphics object's Update / IsVisible / IsAlpha / GetZ / Draw and reads the clock";
}
PORT_FN(0x00462200, "WorldDraw", WorldDraw, fp_world_draw)

// ==== draw_mirror (0x462390) ====================================================================================
// A second scene: the mirror's viewport (MIRROR_X, 64, 128 x 32), the camera turned round (its forward row negated,
// the side row their cross product) and moved 4 m back, the mirror mode, the track when the mirror's detail is
// above 1, the visible objects (translucent ones only above detail 2); then the camera, its matrices and the full
// view back (a Z-only clear).
static void __cdecl draw_mirror() {
    SingleEnter(I(S_PROFILE), 0, 0);
    if (should_draw_mirror()) {
        mrEndFrame();
        mrBeginFrame();
        mrSetView(I(S_MIRROR_X), 0x40, 0x80, 0x20, 1);
        Frame saved, m;
        memcpy(&saved, P<void>(S_CAMERA), 0x30);                        // rep movsd
        memcpy(&m, P<void>(S_CAMERA), 0x30);
        float* r = m.rot.m;
        r[6] = fpu_neg(P<float>(S_CAMERA + 0x18));                       // fld [camera.m6]; fchs
        r[7] = fpu_neg(&r[7]);
        r[8] = fpu_neg(&r[8]);
        r[0] = (float)(D(r[8]) * r[4] - D(r[5]) * r[7]);
        r[1] = (float)(D(r[5]) * r[6] - D(r[3]) * r[8]);
        r[2] = (float)(D(r[7]) * r[3] - D(r[6]) * r[4]);
        m.pos.x = (float)(D(r[6]) * -4.0f + m.pos.x);
        m.pos.y = (float)(D(r[7]) * -4.0f + m.pos.y);
        m.pos.z = (float)(D(r[8]) * -4.0f + m.pos.z);
        memcpy(P<void>(S_CAMERA), &m, 0x30);
        mrSetCamera(&m);
        ViewEnterMirrorMode();
        if (I(S_MIRROR_DETAIL) > 1) ViewDrawMirror_o();
        uint8_t** list = P<uint8_t*>(m1_operand(0x004624c8));
        for (int i = 0; I(S_NUM_GOBS) > i; i++) {
            uint8_t* obj = list[i];
            uint8_t ok = 1;
            if (VFN(obj, 0x10, uint8_t)(obj, 0)) ok = I(S_MIRROR_DETAIL) > 2 ? 1 : 0;   // IsAlpha
            if (VFN(obj, 0xc, uint8_t)(obj, 0) && ok) VFN(obj, 0x14, void)(obj, 0);      // IsVisible, Draw
        }
        ViewLeaveMirrorMode();
        memcpy(P<void>(S_CAMERA), &saved, 0x30);
        mrSetCamera(P<void>(S_CAMERA));
        mrSetView(0, 0, I(S_SCREEN_WID), I(S_SCREEN_HIT), 0);
    }
    SingleLeave(I(S_PROFILE), 0, 0);
}
static void fp_draw_mirror(Footprint& f) { f.replay_only = "calls every graphics object's IsAlpha / IsVisible / Draw"; }
PORT_FN(0x00462390, "draw_mirror", draw_mirror, fp_draw_mirror)

// ==== WorldDraw2D (0x462570) ====================================================================================
// every object's Draw2D, then the mirror's frame: eight lines round (MIRROR_X, 64, 128 x 32)
static void __cdecl WorldDraw2D(void* canvas) {
    SingleEnter(I(S_PROFILE), 0, 0);
    ViewDraw2D();
    uint8_t** list = P<uint8_t*>(m1_operand(0x00462596));
    for (int i = 0; I(S_NUM_GOBS) > i; i++) {
        uint8_t* obj = list[i];
        VFN(obj, 0x18, void, void*)(obj, 0, canvas);
    }
    if (should_draw_mirror()) {
        const int x0 = I(S_MIRROR_X) - 1;
        int x1 = I(S_MIRROR_X) + 0x80;
        const int xl = x0 - 1;
        const uint32_t c = (uint32_t)I(S_MIRROR_COLOR);
        gxLine(xl, 0x3e, x1 + 2, 0x3e, c);
        gxLine(x0, 0x3f, x1, 0x3f, c);
        gxLine(x0, 0x60, x1, 0x60, c);
        gxLine(x0, 0x61, x1, 0x61, c);
        gxLine(x0, 0x3f, x0, 0x60, c);
        gxLine(xl, 0x3f, xl, 0x62, c);
        gxLine(x1, 0x3f, x1, 0x62, c);
        x1++;
        gxLine(x1, 0x3f, x1, 0x62, c);
    }
    SingleLeave(I(S_PROFILE), 0, 0);
}
static void fp_world_draw2d(Footprint& f, void*) { f.replay_only = "calls every graphics object's Draw2D"; }
PORT_FN(0x00462570, "WorldDraw2D", WorldDraw2D, fp_world_draw2d)

// ==== draw_physics_tris (0x462880) ==============================================================================
// Where the camera's forward ray (250 m) meets the terrain, a 5 x 5 grid of small red triangles (1 m apart) on it.
// Only x y z and the colours are written: u, v and +0xc are the stack's (the original's too).
static void __cdecl draw_physics_tris() {
    const float* cam = P<float>(S_CAMERA);
    P3 t, hit, normal;
    t.x = (float)(D(cam[6]) * 250.0f + cam[9]);
    t.y = (float)(D(cam[7]) * 250.0f + cam[10]);
    t.z = (float)(D(cam[8]) * 250.0f + cam[11]);
    if (!TerrainGetIntersection4(P<P3>(S_CAMERA_POS), &t, &hit, &normal)) return;
    mrBeginTriangles(-1);
    Vtx v[3];
    v[0].spec = 0xff000000; v[2].color = 0xffff0000; v[1].color = 0xffff0000;
    v[1].spec = 0xff000000; v[0].color = 0xffff7f7f; v[2].spec = 0xff000000;
    for (int i = 0; i < 5; i++) {
        const float a = (float)((D(i) * 5.0f) * FB(0x3e4ccccd));         // fild; fmul 5; fmul 0.2; fstp dword
        for (int j = 0; j < 5; j++) {
            v[0].x = (float)((D(hit.x) + a) - 2.5f);
            v[0].z = (float)(((D(j) * 5.0f) * FB(0x3e4ccccd) + hit.z) - 2.5f);
            if (!TerrainGetHeight((P3*)&v[0])) continue;
            const double a3 = D(cam[3]) * 0.5f, a4 = D(cam[4]) * 0.5f, a5 = D(cam[5]) * 0.5f;
            v[1].x = (float)((D(cam[0]) * FB(0x3e99999a) + v[0].x) - a3);
            v[1].y = (float)((D(cam[1]) * FB(0x3e99999a) + v[0].y) - a4);
            v[1].z = (float)((D(cam[2]) * FB(0x3e99999a) + v[0].z) - a5);
            v[2].x = (float)((D(cam[0]) * FB(0xbe99999a) + v[0].x) - a3);
            v[2].y = (float)((D(cam[1]) * FB(0xbe99999a) + v[0].y) - a4);
            v[2].z = (float)((D(cam[2]) * FB(0xbe99999a) + v[0].z) - a5);
            mrDrawTriangle(v);
        }
    }
    mrEndTriangles();
}
static void fp_draw_physics_tris(Footprint& f) { fp_render(f); fp_terrain(f); }
PORT_FN(0x00462880, "draw_physics_tris", draw_physics_tris, fp_draw_physics_tris)

// ==============================================================================================================
// view.obj
// ==============================================================================================================

// ==== ViewDraw (0x4665a0) =======================================================================================
// The frame's times and count, the camera's position; the sky (near 1, far 1000, clip 500, fog / wrap / Z off);
// the dynamic models' clip distance; the track's projection (near 0.35, the draw distance); the track.
static void __cdecl ViewDraw(const Frame* camera) {
    if (I(S_FIRST_TIME) == 0) I(S_FIRST_TIME) = PTimeNow();
    I(S_FRAME_TIME) = PTimeNow();
    I(S_VIEW_FRAMES)++;
    cp12(P<void>(S_VIEW_POS), &camera->pos);
    if (B(S_SKY_ON) != 0) {
        mrSetProjection(0x3f800000, 0x447a0000, (uint32_t)I(S_FOV));
        mrModelSetClipDist(0, 0x43fa0000);
        const uint8_t fog = mrIsEnabled(0x40);
        if (fog) mrDisable(0x40);
        mrDisable(0x400);
        mrDisable(0x800);
        mrDisable(0x80);
        mrDisable(0x100);
        mrModelDraw(I(S_SKY_MODEL), P<void>(S_SKY_FRAME));
        mrEnable(0x100);
        mrEnable(0x80);
        mrEnable(0x400);
        mrEnable(0x800);
        if (fog) mrEnable(0x40);
    }
    mrModelSetClipDist(0, fbits((float)ViewGetDynoClipDist()));
    mrSetProjection(0x3eb33333, (uint32_t)I(S_DRAW_DIST), (uint32_t)I(S_FOV));
    TrackDraw();
}
static void fp_view_draw(Footprint& f, const Frame*) { f.replay_only = "reads the clock and draws the track (deferred)"; }
PORT_FN(0x004665a0, "ViewDraw", ViewDraw, fp_view_draw)

// ==== ViewDrawMirror (0x466840): a jump to TrackDraw ===========================================================
static void __cdecl ViewDrawMirror() { TrackDraw(); }
static void fp_view_draw_mirror(Footprint& f) { f.replay_only = "draws the track (deferred)"; }
PORT_FN(0x00466840, "ViewDrawMirror", ViewDrawMirror, fp_view_draw_mirror)

// ==============================================================================================================
// graf.obj
// ==============================================================================================================

// ==== draw_tree (0x46dff0) ======================================================================================
// A sibling list (+4) of GrafNodes: 3 a model (its own frame; -1 none), 4 a facing model queued by its mode (1 the
// camera-facing list, 2 the upright list, 4 none, else a panic), 2 an LOD group (drawn while the camera's distance
// squared, + the min LOD, x the bias, is below +0x18 and not below +0x1c; +0x1c = -1 never), 1 a sphere-culled
// group (+0xc the flag; the sphere +0x10 radius +0x1c), 0 a group; a group's children (+8) recursively.
// FIX: the facing lists had no bound: the 513th camera-facing node (mode 1) wrote 0x559168 (the dynamic models), the
// 515th upright one (mode 2) the camera-facing list. M1 moves both lists into the DLL with 8192 slots each
// (viperport.cpp, relocate_graf_lists; the lists are read from the original's operands), and a node past the capacity
// -- the lifted one, or the stock 512 / 514 where M1 didn't move them -- is dropped (not drawn; logged once).
static uint32_t facing_capacity() { return m1_operand(0x0046e04a) == S_FACING_STOCK ? 512 : VP_LIFT_GRAF_FACING; }
static uint32_t upright_capacity() { return m1_operand(0x0046e066) == S_UPRIGHT_STOCK ? 514 : VP_LIFT_GRAF_FACING; }
static bool facing_full(uint32_t k, uint32_t cap) {
    if (k < cap) return false;
    static bool said;
    if (!said) { said = true; logf("draw_tree: more than %u facing models of one kind in view: the rest aren't drawn", cap); }
    return true;
}
static void __cdecl draw_tree(uint8_t* n) {
    for (; n; n = Pat(n, 4)) {
        const int32_t type = Iat(n, 0);
        uint8_t* child;
        if (type == 3) {
            if (Iat(n, 0xc) != -1) mrModelDrawDeferred(Iat(n, 0xc));
            continue;
        } else if (type == 4) {
            if (Iat(n, 0xc) == -1) continue;
            const int32_t mode = Iat(n, 0x38);
            if (mode == 1) {
                const int32_t k = I(S_FACING_N);
                if (VP_FIX && facing_full((uint32_t)k, facing_capacity())) continue;
                I(S_FACING_N)++;
                P<uint8_t*>(m1_operand(0x0046e04a))[k] = n;
            } else if (mode == 2) {
                const int32_t k = I(S_UPRIGHT_N);
                if (VP_FIX && facing_full((uint32_t)k, upright_capacity())) continue;
                I(S_UPRIGHT_N)++;
                P<uint8_t*>(m1_operand(0x0046e066))[k] = n;
            } else if (mode != 4) {
                LogPanic((const char*)0x004f52a8);                   // "poorly formed graf: bad facing mode"
            }
            continue;
        } else if (type == 2) {
            if (U32(n + 0x1c) == 0xbf800000) continue;
            const double d = (mrCameraDistanceSqr(n + 0xc) + F(S_MIN_LOD)) * F(S_LOD_BIAS);
            const float df = (float)d;                                 // (fcom first, then fstp dword)
            if (d >= Fat(n, 0x18)) continue;                           // fcom [+0x18]; test ah,1
            if (Fat(n, 0x1c) > df) continue;                           // fcomp; test ah,0x41
            child = Pat(n, 8);
            if (!child) continue;
        } else if (type == 1) {
            if (n[0xc] != 0) {
                if (!Pat(n, 8)) continue;
                if (!mrSphereCanSee(n + 0x10, U32(n + 0x1c))) continue;
                child = Pat(n, 8);
            } else {
                child = Pat(n, 8);
                if (!child) continue;
            }
        } else {
            child = Pat(n, 8);
            if (!child) continue;
        }
        draw_tree_o(child);
    }
}
static void fp_draw_tree(Footprint& f, uint8_t*) { f.replay_only = "draws the track in deferred mode (the deferred pool)"; }
PORT_FN(0x0046dff0, "draw_tree", draw_tree, fp_draw_tree)

// ==== GrafDraw (0x46dc70) =======================================================================================
// The graph in deferred mode (GrafDraw's identity frame), then the facing models within the clip distance: the
// camera-facing ones in the camera's rotation turned round (forward row negated, side row their cross product), the
// upright ones turned about y to face it (x = y cross the direction to the camera, z = x cross y).
static void __cdecl GrafDraw(int) {
    I(S_FACING_N) = 0;
    I(S_UPRIGHT_N) = 0;
    mrModelSetFrame(P<void>(S_GRAF_FRAME));
    mrModelEnterDeferredMode();
    draw_tree_o(*P<uint8_t*>(S_GRAF_ROOT));
    mrModelLeaveDeferredMode();
    Frame f;
    float* r = f.rot.m;
    for (int i = 0; I(S_FACING_N) > i; i++) {
        uint8_t* node = P<uint8_t*>(m1_operand(0x0046dcb7))[i];
        cp12(&f.pos, node + 0x3c);
        const P3* c = mrGetCameraPos();
        const float dx = (float)(D(f.pos.x) - c->x), dy = (float)(D(f.pos.y) - c->y), dz = (float)(D(f.pos.z) - c->z);
        if ((D(dy) * dy + D(dz) * dz) + D(dx) * dx >= F(S_GRAF_CLIP)) continue;   // fcomp; test ah,1
        memcpy(r, P<void>(S_CAMERA), 0x24);
        r[6] = fpu_neg(P<float>(S_CAMERA + 0x18));
        r[7] = fpu_neg(&r[7]);
        r[8] = fpu_neg(&r[8]);
        r[0] = (float)(D(r[8]) * r[4] - D(r[7]) * r[5]);
        r[1] = (float)(D(r[6]) * r[5] - D(r[3]) * r[8]);
        r[2] = (float)(D(r[7]) * r[3] - D(r[6]) * r[4]);
        mrModelDraw(Iat(node, 0xc), &f);
    }
    for (int i = 0; I(S_UPRIGHT_N) > i; i++) {
        uint8_t* node = P<uint8_t*>(m1_operand(0x0046ddcb))[i];
        cp12(&f.pos, node + 0x3c);
        const P3* c = mrGetCameraPos();
        const float dx = (float)(D(f.pos.x) - c->x), dy = (float)(D(f.pos.y) - c->y);
        const double dzr = D(f.pos.z) - c->z;                         // fst dword; fmul dword: the register x the float
        const float dz = (float)dzr;
        if ((dzr * dz + D(dy) * dy) + D(dx) * dx >= F(S_GRAF_CLIP)) continue;
        const P3* c2 = mrGetCameraPos();
        P3 e;
        e.x = (float)(D(c2->x) - f.pos.x);
        e.y = (float)(D(c2->y) - f.pos.y);
        const double ezr = D(c2->z) - f.pos.z;                         // (the same)
        e.z = (float)ezr;
        const double q = (ezr * e.z + D(e.y) * e.y) + D(e.x) * e.x;
        const float qf = (float)q;
        if (!(q > FB(0x3c23d70a))) continue;                           // fcom 0.01; test ah,0x41
        const double inv = 1.0f / x87_sqrt(qf);                        // fsqrt; fdivr 1.0f
        put4(&r[3], 0);
        put4(&r[4], 0x3f800000);
        put4(&r[5], 0);
        e.x = (float)(D(e.x) * inv);
        e.y = (float)(D(e.y) * inv);
        e.z = (float)(inv * e.z);
        CrossProduct((P3*)&r[0], (const P3*)&r[3], &e);
        const double q2 = (D(r[1]) * r[1] + D(r[2]) * r[2]) + D(r[0]) * r[0];
        const float q2f = (float)q2;
        if (!(q2 > FB(0x3c23d70a))) {
            put4(&r[0], 0x3f800000);
            put4(&r[1], 0);
            put4(&r[2], 0);
        } else {
            const double inv2 = 1.0f / x87_sqrt(q2f);
            r[0] = (float)(D(r[0]) * inv2);
            r[1] = (float)(D(r[1]) * inv2);
            r[2] = (float)(inv2 * r[2]);
        }
        CrossProduct((P3*)&r[6], (const P3*)&r[0], (const P3*)&r[3]);
        mrModelDraw(Iat(node, 0xc), &f);
    }
}
static void fp_graf_draw(Footprint& f, int) { f.replay_only = "draws the track in deferred mode (the deferred pool)"; }
PORT_FN(0x0046dc70, "GrafDraw", GrafDraw, fp_graf_draw)

// ==============================================================================================================
// carwob.obj
// ==============================================================================================================

// the cockpit's needles and wheel: the frame's rotation x (yaw 0 x pitch 0 x roll `angle`)
static void needle_rotation(Frame* f, uint32_t angle) {
    M3 y, t;
    MatrixMakeYaw(&y, 0);
    MatrixMakePitch(&t, 0);
    MatrixConcat(&y, &t, &y);
    MatrixMakeRoll(&t, angle);
    MatrixConcat(&y, &t, &y);
    MatrixConcat(&f->rot, &y, &f->rot);
}

// ==== CarObject::Draw (0x46a4c0) ================================================================================
// In the mirror the focus car isn't drawn and the others at LOD 4 (put back at the end). The light, the fade. The
// cockpit view (camera 0, the focus car): the cockpit model, the two needles (rows 2 and 3 of cockpit.tab, turned by
// the gauges) and the steering wheel (row 1, turned by the steering), no wheels; camera 1 no car at all; camera 2
// the wheels and the x-ray; otherwise the body at its LOD (specular or env-map by the LOD row), the shadow model,
// the brake lights while braking; the wheels (the LOD row's wheel LOD >= 0); the x-ray; the tyre dust (16 particles
// as triangles facing the camera).
static void __fastcall CarObject_Draw(CarObject* self, Edx) {
    const int32_t lod_was = self->lod_now, lod2_was = self->lod_now2;
    if (ViewIsMirrorMode()) {
        if (WorldGetFocusCar() == self->index) return;
        self->lod_now = 4;
        self->lod_now2 = 4;
    }
    mrLightSpecial(self->index, &self->frame);
    if ((int32_t)U32(&opacity(self)) < 0x3f800000) mrModelSetAlpha(U32(&opacity(self)));
    uint8_t xray = 0;
    uint8_t wheels = 1;
    if (WorldGetCameraType() == 0 && WorldGetFocusCar() == self->index) {
        if (self->has_cockpit != 0) {
            mrModelDraw(self->cockpit_c, &self->frame);
            Frame f;
            P3 v;
            float* m = f.rot.m;
            memcpy(&f, &self->frame, 0x30);
            const float* a = self->cockpit_row2;
            v.x = (float)((D(a[0]) * m[0] + D(a[2]) * m[6]) + D(a[1]) * m[3]);
            v.y = (float)((D(a[0]) * m[1] + D(a[2]) * m[7]) + D(a[1]) * m[4]);
            v.z = (float)((D(a[0]) * m[2] + D(a[2]) * m[8]) + D(a[1]) * m[5]);
            f.pos.x = (float)(D(v.x) + f.pos.x);
            f.pos.y = (float)(D(v.y) + f.pos.y);
            f.pos.z = (float)(D(v.z) + f.pos.z);
            float ang = (float)(D(self->needle1_scale) * Fat(self, 0x70) + self->needle1_min);
            needle_rotation(&f, fbits(ang));
            mrModelDraw(self->needle[1], &f);
            memcpy(&f, &self->frame, 0x30);
            MatrixMulPoint(&v, (const P3*)self->cockpit_row3, &f.rot);
            f.pos.x = (float)(D(v.x) + f.pos.x);
            f.pos.y = (float)(D(v.y) + f.pos.y);
            f.pos.z = (float)(D(v.z) + f.pos.z);
            ang = (float)(D(self->needle2_scale) * Fat(self, 0x88) + self->needle2_min);
            needle_rotation(&f, fbits(ang));
            mrModelDraw(self->needle[0], &f);
            memcpy(&f, &self->frame, 0x30);
            const float* b = self->cockpit_row1;
            v.x = (float)((D(b[2]) * m[6] + D(b[0]) * m[0]) + D(b[1]) * m[3]);
            v.y = (float)((D(b[2]) * m[7] + D(b[0]) * m[1]) + D(b[1]) * m[4]);
            v.z = (float)((D(b[2]) * m[8] + D(b[0]) * m[2]) + D(b[1]) * m[5]);
            f.pos.x = (float)(D(f.pos.x) + v.x);
            f.pos.y = (float)(D(f.pos.y) + v.y);
            f.pos.z = (float)(D(f.pos.z) + v.z);
            ang = (float)(D(Fat(self, 0x78)) * FB(0xbf860a92));     // the steering x -60 degrees
            needle_rotation(&f, fbits(ang));
            wheels = 0;
            mrModelDraw(self->cockpit_w, &f);
        } else {
            wheels = 1;
        }
    } else if (WorldGetCameraType() == 1 && WorldGetFocusCar() == self->index) {
        wheels = 0;
    } else if (WorldGetCameraType() == 2 && WorldGetFocusCar() == self->index) {
        wheels = 1;
        xray = 1;
    } else {
        uint8_t body = 1;
        if (WorldGetFocusCar() == self->index) {
            const int t = WorldGetCameraType();
            if (t == 0 || t == 3) {
                wheels = 0;
                body = 0;
            } else if (t == 2) {
                body = 0;
                wheels = 1;
            }
        }
        if (body) {
            mrModelSetClipDist(0, fbits((float)ViewGetStaticClipDist()));
            if (self->lod_p[self->lod_now].spec != 0) {
                if (WorldShowSpecular() == 1) mrEnable(4);
                else if (WorldShowSpecular() == 2) mrModelEnvMap(1);
            }
            mrModelDraw(self->lod_models_p[self->lod_now], &self->frame);
            if (self->shadow_model != 0) mrModelDraw(self->shadow_model, &self->frame);
            if (self->lod_p[self->lod_now].spec != 0) {
                if (WorldShowSpecular() == 1) mrDisable(4);
                else if (WorldShowSpecular() == 2) mrModelEnvMap(0);
            }
            mrModelSetClipDist(0, fbits((float)ViewGetDynoClipDist()));
            if ((int32_t)U32(self->msg + 0x40) > 0x3c23d70a && I(S_ALPHA_LEVEL) >= 1) {   // braking (+0x7c)
                mrPushState();
                mrDisable(0x100);
                mrDisable(1);
                mrSetBlendMode(2);
                mrModelDraw(self->brakelight, &self->frame);
                mrPopState();
            }
        }
    }
    if (self->lod_p[self->lod_now2].faces >= 0 && wheels)
        for (int i = 0; i < 4; i++) CarObject_DrawWheel_o(self, 0, i, U32(&self->cam_dist), xray);
    if (xray) CarObject_DrawXRay_o(self, 0);
    if ((int32_t)U32(&opacity(self)) < 0x3f800000) mrModelClearAlpha();
    if (WorldShowSmoke() != 0) {
        const float t = (float)WorldGetElapsedTime();
        mrDisable(0x100);
        mrBeginTriangles(self->effects_tex);
        Vtx v[3];
        for (int k = 0; k < 3; k++) {
            v[k].color = 0xffffffff;
            v[k].spec = 0xffffffff;
            v[k].u = 0x3e000000;
            v[k].v = 0x3e800000;
        }
        const float* cam = P<float>(S_CAMERA);
        for (int k = 0; k < 16; k++) {
            DustParticle* p = &self->dust[k];
            if (p->life > t) continue;                                 // fcomp; test ah,0x41
            if (!(D(p->life) + 2.0f > t)) continue;
            P3 q, s, r;
            GetParticlePosition_car(&q, &p->vel, &p->pos, fbits((float)(D(t) - p->life)));
            const double k0 = D(cam[0]) * FB(0x3ecccccd);
            v[1].u = 0x3e800000;
            cp12(&v[0], &q);
            v[2].v = 0x3ec00000;
            s.x = (float)(k0 + q.x);
            s.y = (float)(D(cam[1]) * FB(0x3ecccccd) + q.y);
            s.z = (float)(D(cam[2]) * FB(0x3ecccccd) + q.z);
            const double k3 = D(cam[3]) * FB(0x3ecccccd);
            cp12(&v[2], &s);
            r.x = (float)(k3 + q.x);
            r.y = (float)(D(cam[4]) * FB(0x3ecccccd) + q.y);
            r.z = (float)(D(cam[5]) * FB(0x3ecccccd) + q.z);
            cp12(&v[1], &r);
            mrDrawTriangle(v);
        }
        mrEndTriangles();
        mrEnable(0x100);
    }
    self->lod_now = lod_was;
    self->lod_now2 = lod2_was;
}
static void fp_drawwheel_writes(Footprint& f, CarObject* self) {     // DrawWheel's own writes, any wheel
    f.add(&self->dust[0], 0x1c8, "the car's tyre dust, its next index and time");
    for (int k = 0; self->num_smoke > k && k < 32; k++) {
        uint8_t* s = self->smoke[k];
        if (!s) continue;
        f.add(s, 0x58, "a tyre smoke puff (SmokeObject::Puff)");
        mrModelInfo* info = *(mrModelInfo**)(s + 0x34);
        if (info && info->verts) f.add(info->verts, 4 * 32, "its model's vertices");
    }
}
static void fp_drawxray_writes(Footprint& f, CarObject* self) {     // DrawXRay's: the two strut copies' vertices,
    for (int k = 0; k < 2; k++) {                                    // as many as their sources have
        uint8_t* verts;
        uint8_t* sverts;
        int n, sn;
        mrModelGetVerts(self->arm_copy[k], &verts, &n);
        mrModelGetVerts(self->arm[4 + k], &sverts, &sn);
        if (verts && sn > 0) f.add(verts, (uint32_t)sn * 32, "an x-ray strut copy's vertices");
    }
}
// A concurrent input the shadow check can't feed: the body models drawn here (lod_models_p[], the shadow and
// brake-light models) are the physics Car's live models, which Car::ActuallyApplyDamage (0x436b40) dents on the
// PHYSICS thread (mrModelGetVerts / mrModelGetInfo, vertex x y z rewritten, mrModelBuild). A crash landing between the
// two passes of a check lights different vertices into the lit-vertex buffer (and hands DrawIndexedPrimitive
// different data): a mismatch of the check, not of the rewrite (the original is racing the physics in the same way).
static void fp_car_draw(Footprint& f, CarObject* self, Edx) {
    f.add(&self->lod_now, 8, "the car's LOD rows (put back)");
    fp_drawwheel_writes(f, self);
    fp_drawxray_writes(f, self);
    fp_render(f);
}
PORT_FN(0x0046a4c0, "CarObject::Draw", CarObject_Draw, fp_car_draw)

// ==== CarObject::Draw2D (0x46ad80) ==============================================================================
// Another car's name tag within 100 m: its position 1 m up projected, 32 pixels above: "name (n)" in multiplayer;
// with the status hack on, thought.stp and the status text. The canvas stays set (the original doesn't restore it).
// FIX: "%s (%d)" went into 44 bytes of the original's stack (up to its saved registers), so a long name (the car
// manager's +5, up to 45 bytes) overran it. The name is cut so the tag fits in 44 bytes (43 characters); a tag that
// fits is formatted exactly as before.
static int decimal_length(int32_t v) {                               // the characters %d writes
    uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    int n = v < 0 ? 2 : 1;
    while (u >= 10) { u /= 10; n++; }
    return n;
}
static void __fastcall CarObject_Draw2D(CarObject* self, Edx, void* canvas) {
    if (WorldGetFocusCar() == self->index) return;
    const float* cam = P<float>(S_CAMERA_POS);
    const float dx = (float)(D(cam[0]) - self->frame.pos.x);
    const float dy = (float)(D(cam[1]) - self->frame.pos.y);
    const float dz = (float)(D(cam[2]) - self->frame.pos.z);
    if ((D(dy) * dy + D(dz) * dz) + D(dx) * dx >= 10000.0f) return;   // fcomp; test ah,1
    P3 p, q;
    cp12(&p, &self->frame.pos);
    p.y = (float)(D(p.y) + 1.0f);
    if (!mrProjectPoint(&p, &q)) return;
    gxSetCanvas(canvas);
    q.y = (float)(D(q.y) - 32.0f);
    uint8_t* info = CarMgrGetInfo(self->index);
    const int32_t num = Iat(info, 0x140);
    if (MultiEnabled()) {
        char buf[256];
        const char* name = (const char*)info + 5;
        char cut[44];
        const int room = 43 - 3 - decimal_length(num);                 // 44 bytes less " (", ")" and the number
        if (VP_FIX && (int)strlen(name) > room) {
            memcpy(cut, name, (size_t)room);
            cut[room] = 0;
            name = cut;
        }
        game_sprintf(buf, (const char*)0x004f4f5c, name, num);         // "%s (%d)"
        const int y = x87_ftol(q.y);
        gxTextCentered(x87_ftol(q.x), y, buf, (uint32_t)I(S_CAR_TAG_COLOR));
    }
    if (HackShowCarStatusInfo()) {
        const char* status = (const char*)info + 0x32;
        if (status[0] != 0) {
            const int y = x87_ftol(q.y);
            gxDrawStamp(self->stamp, x87_ftol(q.x), y - 9, 0, 0);
            const int y2 = x87_ftol(q.y);
            gxTextCentered(x87_ftol(q.x), y2 - 9, status, (uint32_t)I(S_CAR_TAG_COLOR));
        }
    }
}
static void fp_car_draw2d(Footprint& f, CarObject*, Edx, void* canvas) {
    f.add(P<uint8_t>(S_CANVAS), 4, "gx: the current canvas");
    if (canvas && Pat(canvas, 4) && Iat(canvas, 0x10) > 0 && Iat(canvas, 0xc) > 0)
        f.add(Pat(canvas, 4), (uint32_t)Iat(canvas, 0x10) * (uint32_t)Iat(canvas, 0xc), "the canvas's pixels");
    fp_render(f);
}
PORT_FN(0x0046ad80, "CarObject::Draw2D", CarObject_Draw2D, fp_car_draw2d)

// ==== CarObject::DrawWheel (0x46af10) ===========================================================================
// Wheel i at the LOD row's wheel LOD (2 in x-ray, with the wheel moved in by 0.1778 m): spun by its angle (a
// negative spin on the right), the blurred model above 30 rad/s; in x-ray the front brake discs glow above 872;
// then at the tyre's contact point (the frame 1 tyre radius down -- the front radius on all four), tyre smoke (a
// free puff, not paused) and dust: one particle every 0.1 s (or at once when the clock hasn't moved or went back).
// (The distance argument is unused.)
// FIX: the wheel LOD is the LOD table's column ("<car>L.tab", atoi) with no bound: a value above 2 read the wheel
// model handle from past wheel_models[3] (the wheel frames) and drew a garbage handle. The model is now picked from
// wheel_models[2] for any value above 2 (negative ones draw no wheel, as before); the rest of the wheel -- the spin,
// the x-ray offset -- still goes by the value itself.
static void __fastcall CarObject_DrawWheel(CarObject* self, Edx, int i, uint32_t, uint8_t xray) {
    int32_t lod = self->lod_p[self->lod_now2].faces;
    if (xray) lod = 2;
    if (lod < 0) return;
    Frame f;
    memcpy(&f, &self->wheel_frame[i], 0x30);
    const uint32_t odd = (uint32_t)i & 1;
    const float side = (float)(D(odd ? -1.0f : 1.0f) * FB(0x3e361134));
    if (lod == 2) {
        volatile float kk = FB(0x3e361134);
        const double k = -D(kk);                                       // fld; fchs
        f.pos.x = (float)(D(f.rot.m[0]) * k + f.pos.x);
        f.pos.y = (float)(D(f.rot.m[1]) * k + f.pos.y);
        f.pos.z = (float)(D(f.rot.m[2]) * k + f.pos.z);
    }
    if (lod <= 0 || lod == 2) {
        const float a = (float)(D(odd ? -1.0f : 1.0f) * wheel_msg(self, i)->spin_angle);
        M3 y, t;
        const float c0 = x87_cos_f(0.0), s0 = x87_sin_f(0.0);
        y.m[0] = c0; put4(&y.m[1], 0); y.m[2] = fpu_neg(&s0);
        put4(&y.m[3], 0); put4(&y.m[4], 0x3f800000); put4(&y.m[5], 0);
        y.m[6] = s0; put4(&y.m[7], 0); y.m[8] = c0;
        MatrixMakePitch(&t, fbits(a));
        MatrixConcat(&y, &t, &y);
        MatrixMakeRoll(&t, 0);
        MatrixConcat(&y, &t, &y);
        MatrixConcat(&f.rot, &y, &f.rot);
        if (lod == 2) {
            f.pos.x = (float)(D(self->frame.rot.m[0]) * side + f.pos.x);
            f.pos.y = (float)(D(self->frame.rot.m[1]) * side + f.pos.y);
            f.pos.z = (float)(D(self->frame.rot.m[2]) * side + f.pos.z);
        }
    }
    WheelMsg* w = wheel_msg(self, i);
    const bool fast = fabs(D(w->omega)) > 30.0f;                      // fabs; fcomp 30; test ah,0x41
    const int32_t mlod = VP_FIX && lod > 2 ? 2 : lod;
    const uint8_t* wm = (const uint8_t*)self + 0x518 + ((uint32_t)mlod << 4);   // wheel_models[lod]
    int32_t model;
    if (i == 0 || i == 1) model = *(const int32_t*)(wm + (fast ? 0 : 4));
    else model = *(const int32_t*)(wm + (fast ? 8 : 12));
    mrModelDraw(model, &f);
    if (xray && (i == 0 || i == 1)) {
        const uint32_t bt = U32(&w->brake_temp);
        if ((int32_t)bt > 0x445a0000 && I(S_ALPHA_LEVEL) >= 2) {
            mrPushState();
            mrDisable(0x80);
            mrDisable(0x100);
            const double x = (D(FB(bt)) - 872.0f) * FB(0x3ba3d70a);
            const double k = FB(0x3f7d70a4);
            mrModelSetAlpha(fbits((float)(k > x ? x : k)));             // fcom st(1); test ah,0x41
            mrSetBlendMode(2);
            mrModelDraw(self->diskglow, &f);
            mrModelClearAlpha();
            mrPopState();
        }
    }
    f.pos.y = (float)(D(f.pos.y) - self->radius_front);
    if (!WorldShowSmoke()) return;
    const uint8_t smoky = (w->fx_flags & 6) != 0 ? 1 : 0;
    if (!PhysicsIsPaused() && smoky) {
        const int k = CarObject_find_available_smoke(self, 0);
        if (k != -1) SmokeObject_Puff(self->smoke[k], 0, &f.pos, &self->wheel_move[i], (w->fx_flags & 2) == 0 ? 1 : 0);
    }
    const float t = (float)WorldGetElapsedTime();
    if (PhysicsIsPaused()) return;
    if (!(w->fx_flags & 4)) return;
    if (self->dust_time > t) put4(&self->dust_time, 0);                // fcomp; test ah,0x41
    if (fabs(D(t) - self->dust_time) > FB(0x34000000)) {
        if (D(self->dust_time) + FB(0x3dcccccd) > t) return;
    }
    cp4(&self->dust_time, &t);
    const int n = self->dust_next;
    DustParticle* p = (DustParticle*)((uint8_t*)self + 0x344 + n * 0x1c);
    cp12(&p->vel, &self->wheel_frame[i].pos);
    p->pos.x = (float)(D(Fat(self, 0xa8)) * FB(0x3f19999a));
    p->pos.y = (float)(D(Fat(self, 0xac)) * FB(0x3f19999a));
    p->pos.z = (float)(D(Fat(self, 0xb0)) * FB(0x3f19999a));
    const double m = fabs(D(w->omega)) * FB(0x3dcccccd);
    p->pos.y = (float)((8.0f > m ? m : D(8.0f)) + p->pos.y);           // fcom st(1); test ah,0x41
    p->pos.x = (float)(RandomReal_car(0xbdcccccd, 0x3dcccccd) * p->pos.y + p->pos.x);
    p->pos.z = (float)(RandomReal_car(0xbdcccccd, 0x3dcccccd) * p->pos.y + p->pos.z);
    cp4(&p->life, &t);
    const int nn = self->dust_next + 1;
    self->dust_next = nn;
    if (nn >= 16) self->dust_next = 0;
}
static void fp_car_drawwheel(Footprint& f, CarObject* self, Edx, int, uint32_t, uint8_t) {
    fp_drawwheel_writes(f, self);
    fp_render(f);
}
PORT_FN(0x0046af10, "CarObject::DrawWheel", CarObject_DrawWheel, fp_car_drawwheel)

// ==== CarObject::DrawXRay (0x46b480) ============================================================================
// Each front wheel's suspension: the upper and lower arms (rolled towards the hub from the frame's pivots, 0.55 and
// 0.57 m in), the strut (rolled from (0.88 m, -/+0.3)), whose copy is stretched: every vertex above 0.1 of the source
// model moved up by (0.52 - its pivot's height) / cos - 0.5 -- the model rebuilt after each vertex --, and the spindle
// (turned by the steering; the right one by pi more). Each part in a frame at the hub's height in the car's frame.
// FIX CANDIDATE: the stretch loop runs over the SOURCE model's vertices and writes the copy's at the same indices:
// a copy with fewer vertices than its source (mrModelCopyRemap makes them equal, so only with odd model data) is
// written past its end.
static void xray_part(CarObject* self, Frame* g, const Frame* r) {
    MatrixMulPoint(&g->pos, &r->pos, &self->frame.rot);
    VectorAdd(&g->pos, &g->pos, &self->frame.pos);
    MatrixConcat(&g->rot, &r->rot, &self->frame.rot);
}
static void xray_roll(Frame* r, float c, float s, float ns) {
    float* m = r->rot.m;
    put4(&m[2], 0); put4(&m[5], 0); put4(&m[6], 0); put4(&m[7], 0); put4(&m[8], 0x3f800000);
    m[0] = c; m[1] = s; m[3] = ns; m[4] = c;
}
static void xray_yaw(Frame* r, float c, float s, float ns) {
    float* m = r->rot.m;
    put4(&m[1], 0); put4(&m[3], 0); put4(&m[4], 0x3f800000); put4(&m[5], 0); put4(&m[7], 0);
    m[0] = c; m[2] = ns; m[6] = s; m[8] = c;
}
static void xray_side(CarObject* self, int side) {
    const bool left = side == 0;
    float a[3];
    cp12(a, (uint8_t*)self + (left ? 0xb4 : 0xec));                    // the wheel's hub position
    a[0] = left ? (float)(D(FB(0x3e361134)) + a[0]) : (float)(D(a[0]) - FB(0x3e361134));
    const double hr = D(self->radius_front) + a[1];
    const float h = (float)hr;
    const double h2 = hr - FB(0x3e3851ec);
    const float h2f = (float)h2;
    Frame r, g;
    float c, s, ns;
    // the upper arm
    if (left) atan_roll_neg(h2, DB(0x3fe19999a0000000), &c, &s, &ns);
    else atan_roll(h2, DB(0x3fe19999a0000000), &c, &s, &ns);
    xray_roll(&r, c, s, ns);
    cp4(&r.pos.x, &a[0]);
    cp4(&r.pos.z, &a[2]);
    r.pos.y = (float)(D(h) + FB(0x3e0f5c29));
    xray_part(self, &g, &r);
    mrModelDraw(self->arm[left ? 0 : 1], &g);
    // the lower arm
    if (left) atan_roll_neg(D(h2f), DB(0x3fe23d70a0000000), &c, &s, &ns);
    else atan_roll(D(h2f), DB(0x3fe23d70a0000000), &c, &s, &ns);
    xray_roll(&r, c, s, ns);
    cp4(&r.pos.x, &a[0]);
    cp4(&r.pos.z, &a[2]);
    const float h3 = (float)(D(h) - FB(0x3e23d70a));
    cp4(&r.pos.y, &h3);
    xray_part(self, &g, &r);
    mrModelDraw(self->arm[left ? 2 : 3], &g);
    // the strut, stretched
    atan_roll(left ? DB(0xbfd3333340000000) : DB(0x3fd3333340000000), D(FB(0x3f6147ae)) - h, &c, &s, &ns);
    xray_roll(&r, c, s, ns);
    r.pos.x = left ? (float)(D(a[0]) + FB(0x3d8f5c29)) : (float)(D(a[0]) - FB(0x3d8f5c29));
    cp4(&r.pos.y, &h3);
    cp4(&r.pos.z, &a[2]);
    xray_part(self, &g, &r);
    const float dy = (float)((D(FB(0x3f051eb8)) - r.pos.y) / r.rot.m[4] - 0.5f);
    const int copy = left ? 0 : 1;
    uint8_t* verts;
    uint8_t* sverts;
    int n, sn;
    mrModelGetVerts(self->arm_copy[copy], &verts, &n);
    mrModelGetVerts(self->arm[left ? 4 : 5], &sverts, &sn);
    for (int k = 0; sn > k; k++) {
        const uint8_t* sy = sverts + k * 32 + 4;
        if ((int32_t)U32(sy) > 0x3dcccccd) Fat(verts, k * 32 + 4) = (float)(D(*(const float*)sy) + dy);
        mrModelInfo* info = mrModelGetInfo(self->arm_copy[copy]);
        mrModelBuild(self->arm_copy[copy], info, 4);
    }
    mrModelDraw(self->arm_copy[copy], &g);
    // the spindle
    if (left) yaw_cs(D(Fat(self, 0xc4)), &c, &s, &ns);
    else yaw_cs(D(Fat(self, 0xfc)) + FB(0x40490fdb), &c, &s, &ns);
    xray_yaw(&r, c, s, ns);
    cp4(&r.pos.x, &a[0]);
    cp4(&r.pos.y, &h);
    cp4(&r.pos.z, &a[2]);
    xray_part(self, &g, &r);
    mrModelDraw(self->spin[left ? 0 : 1], &g);
}
static void __fastcall CarObject_DrawXRay(CarObject* self, Edx) {
    xray_side(self, 0);
    xray_side(self, 1);
}
static void fp_car_drawxray(Footprint& f, CarObject* self, Edx) {
    fp_drawxray_writes(f, self);
    fp_render(f);
}
PORT_FN(0x0046b480, "CarObject::DrawXRay", CarObject_DrawXRay, fp_car_drawxray)

// ==== the deleting destructors: the destructor, then operator delete when flag 1 is set; `this` back ============
typedef void(__fastcall* Dtor_t)(void*, Edx);
template <uint32_t DTOR> static void* __fastcall deleting_dtor(void* self, Edx, uint32_t flag) {
    ((Dtor_t)DTOR)(self, 0);
    if (flag & 1) OpDelete(self);
    return self;
}
static void* __fastcall CarObject_sdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x0046a170>(self, e, flag); }
static void* __fastcall GhostCarObject_sdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x0046cd40>(self, e, flag); }
static void* __fastcall ShadowObject_sdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x004676c0>(self, e, flag); }
static void* __fastcall SmokeObject_sdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x00467f40>(self, e, flag); }
static void* __fastcall ReflectionObject_sdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x004682f0>(self, e, flag); }
static void* __fastcall SkidObject_sdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x00468890>(self, e, flag); }
static void* __fastcall ModelObject_vdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x00469aa0>(self, e, flag); }
static void* __fastcall WobbleObject_vdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x00469850>(self, e, flag); }
static void* __fastcall SplashObject_vdd(void* self, Edx e, uint32_t flag) { return deleting_dtor<0x0046f730>(self, e, flag); }
// DebrisObject's and SparksObject's inline their destructors: the vtable, the texture forgotten, ~GraphObject
static void* __fastcall DebrisObject_vdd(void* self, Edx, uint32_t flag) {
    const int32_t tex = Iat(self, 0xd0);
    put4(self, 0x004dd5d8);
    gxForgetTexture(tex);
    GraphObject_dtor(self, 0);
    if (flag & 1) OpDelete(self);
    return self;
}
static void* __fastcall SparksObject_vdd(void* self, Edx, uint32_t flag) {
    const int32_t tex = Iat(self, 0x388);
    put4(self, 0x004dd5b0);
    gxForgetTexture(tex);
    GraphObject_dtor(self, 0);
    if (flag & 1) OpDelete(self);
    return self;
}
static void fp_deleting(Footprint& f, void*, Edx, uint32_t) { f.replay_only = "destroys (and frees) the object"; }
PORT_FN(0x0046cdc0, "CarObject::`scalar deleting destructor'", CarObject_sdd, fp_deleting)
PORT_FN(0x0046ce30, "GhostCarObject::`scalar deleting destructor'", GhostCarObject_sdd, fp_deleting)
PORT_FN(0x00468eb0, "ShadowObject::`scalar deleting destructor'", ShadowObject_sdd, fp_deleting)
PORT_FN(0x00468ef0, "SmokeObject::`vector deleting destructor'", SmokeObject_sdd, fp_deleting)
PORT_FN(0x00468f10, "ReflectionObject::`vector deleting destructor'", ReflectionObject_sdd, fp_deleting)
PORT_FN(0x00468f70, "SkidObject::`vector deleting destructor'", SkidObject_sdd, fp_deleting)
PORT_FN(0x00469b80, "ModelObject::`scalar deleting destructor'", ModelObject_vdd, fp_deleting)
PORT_FN(0x00469ba0, "WobbleObject::`scalar deleting destructor'", WobbleObject_vdd, fp_deleting)
PORT_FN(0x0046f890, "SplashObject::`vector deleting destructor'", SplashObject_vdd, fp_deleting)
PORT_FN(0x0046eb10, "DebrisObject::`vector deleting destructor'", DebrisObject_vdd, fp_deleting)
PORT_FN(0x0046ed60, "SparksObject::`vector deleting destructor'", SparksObject_vdd, fp_deleting)

// ==============================================================================================================
// effects.obj
// ==============================================================================================================

// ==== ShadowObject::Draw (0x4677e0) =============================================================================
// Each vertex of the shadow model (the car's frame) dropped on to the terrain below it and lifted 0.01 m towards
// the camera (+0.1 up): where the terrain is below the vertex, the model's copy gets the point back in the car's
// frame; elsewhere the previous vertex's (the first stays). The alpha: (1 - distance / 100) x (12.2 - the first
// vertex's height above the terrain) / 12.2 x 0.5, clamped to 0..1 (an integer compare above).
static void __fastcall ShadowObject_Draw(uint8_t* self, Edx) {
    uint8_t* w = Pat(self, 4);
    const Frame* fr = (const Frame*)(w + 4);
    if (!Pat(self, 0x10)) return;
    const float* cam = P<float>(S_CAMERA_POS);
    P3 d;
    d.x = (float)(D(cam[0]) - fr->pos.x);
    d.y = (float)(D(cam[1]) - fr->pos.y);
    d.z = (float)(D(cam[2]) - fr->pos.z);
    const double k = FB(0x3c23d70a) / x87_sqrt((D(d.y) * d.y + D(d.z) * d.z) + D(d.x) * d.x);   // fdivr 0.01f
    d.x = (float)(D(d.x) * k);
    d.y = (float)(D(d.y) * k);
    d.z = (float)(k * d.z);
    uint32_t h2 = 0x4314d70b;                                          // 148.84 = 12.2^2
    uint8_t first = 1;
    d.y = (float)(D(d.y) + FB(0x3dcccccd));
    VP_OPAQUE(d);                       // (GCC: the divide done here, as the original does, though no vertex uses d)
    P3 delta;
    for (int i = 0, off = 0; (*(mrModelInfo**)(self + 0x10))->nverts > i;) {
        P3 v, q, t;
        cp12(&v, (*(mrModelInfo**)(self + 0x10))->verts + off);
        MatrixMulPoint(&q, &v, &fr->rot);
        q.x = (float)(D(fr->pos.x) + q.x);
        q.y = (float)(D(fr->pos.y) + q.y);
        q.z = (float)(D(fr->pos.z) + q.z);
        cp12(&t, &q);
        uint8_t* cverts = (*(mrModelInfo**)(self + 0x14))->verts;
        if (TerrainGetHeight(&t) && !(t.y >= q.y)) {                   // fcomp; test ah,1
            if (first) {
                first = 0;
                delta.x = (float)(D(q.x) - t.x);
                delta.y = (float)(D(q.y) - t.y);
                const double dzr = D(q.z) - t.z;                       // fst dword; fmul dword: the register x the float
                delta.z = (float)dzr;
                h2 = fbits((float)((dzr * delta.z + D(delta.y) * delta.y) + D(delta.x) * delta.x));
            }
            t.x = (float)(D(t.x) + d.x);
            t.y = (float)(D(t.y) + d.y);
            t.z = (float)(D(t.z) + d.z);
            v.x = (float)(D(t.x) - fr->pos.x);
            v.y = (float)(D(t.y) - fr->pos.y);
            v.z = (float)(D(t.z) - fr->pos.z);
            MatrixMulPointInv(&v, &v, &fr->rot);
            cverts = (*(mrModelInfo**)(self + 0x14))->verts;
            cp12(cverts + off, &v);
        } else if (off != 0) {
            cverts = (*(mrModelInfo**)(self + 0x14))->verts;
            memcpy(cverts + off, cverts + off - 0x20, 0x20);           // rep movsd, 8 dwords
        }
        const int o = off;
        i++;
        off += 0x20;
        put4((*(mrModelInfo**)(self + 0x14))->verts + o + 0x18, 0x3f4a0000);
        put4((*(mrModelInfo**)(self + 0x14))->verts + o + 0x1c, 0x3f240000);
    }
    mrModelBuild(Iat(self, 0x18), Pat(self, 0x14), 4);
    const float fade = (float)(1.0f - x87_sqrt(Fat(self, 0xc)) / 100.0f);
    const double al = ((D(FB(0x41433334)) - x87_sqrt(FB(h2))) * FB(0x3da7de6c)) * fade * 0.5f;
    float alpha = (float)al;                                           // (fcom first, then fstp dword)
    if (!(al >= 0.0f)) put4(&alpha, 0);                                // test ah,1
    else if ((int32_t)fbits(alpha) > 0x3f800000) put4(&alpha, 0x3f800000);
    mrModelSetAlpha(fbits(alpha));
    mrDisable(0x100);
    mrModelDraw(Iat(self, 0x18), fr);
    mrEnable(0x100);
    mrModelClearAlpha();
}
static void fp_shadow_draw(Footprint& f, uint8_t* self, Edx) {
    mrModelInfo* info = *(mrModelInfo**)(self + 0x10);
    mrModelInfo* copy = *(mrModelInfo**)(self + 0x14);
    if (info && copy && copy->verts && info->nverts > 0) f.add(copy->verts, (uint32_t)info->nverts * 32, "the shadow's vertices");
    fp_render(f);
    fp_terrain(f);
}
PORT_FN(0x004677e0, "ShadowObject::Draw", ShadowObject_Draw, fp_shadow_draw)

// ==== SmokeObject::Draw (0x4681f0) ==============================================================================
// the puff turned to the camera (its rotation), additive, lighting and Z writes off
static void __fastcall SmokeObject_Draw(uint8_t* self, Edx) {
    memcpy(self + 4, P<void>(S_CAMERA), 0x24);
    mrModelSetAlpha(U32(self + 0x54));
    const uint8_t lit = mrIsEnabled(1);
    if (lit) mrDisable(1);
    mrDisable(0x100);
    mrSetBlendMode(2);
    mrModelDraw(Iat(self, 0x38), self + 4);
    mrSetBlendMode(1);
    mrEnable(0x100);
    if (lit) mrEnable(1);
    mrModelClearAlpha();
}
static void fp_smoke_draw(Footprint& f, uint8_t* self, Edx) {
    f.add(self + 4, 0x24, "the puff's rotation");
    fp_render(f);
}
PORT_FN(0x004681f0, "SmokeObject::Draw", SmokeObject_Draw, fp_smoke_draw)

// ==== ReflectionObject::Draw (0x468450) =========================================================================
// The car mirrored in the ground under it: the terrain's normal there (smoothed: 70% the last one), the car's
// frame reflected (I - 2 n n^T) about the hit point; alpha (1 - distance / 30) x 0.15, culling reversed, lighting
// and Z off. (A dead alias check of footensor's output is left out.)
static void __fastcall ReflectionObject_Draw(uint8_t* self, Edx) {
    Frame fr;
    memcpy(&fr, Pat(self, 4) + 4, 0x30);
    P3 a, b, hit, n;
    int32_t surface;
    cp12(&a, &fr.pos);
    cp12(&b, &fr.pos);
    a.y = (float)(D(fr.pos.y) - 1000.0f);
    b.y = (float)(D(fr.pos.y) + 1000.0f);
    if (!TerrainGetIntersection5(&a, &b, &hit, &n, &surface)) return;
    P3* smooth = (P3*)(self + 0x10);
    interpolate(&n, smooth, 0x3f333333);
    cp12(smooth, &n);
    M3 m;
    footensor(&m, &n, &n);
    const float* f = fr.rot.m;
    const float* t = m.m;
    Frame r;
    float* o = r.rot.m;
    o[0] = (float)((D(f[2]) * t[6] + D(f[1]) * t[3]) + D(f[0]) * t[0]);
    o[3] = (float)((D(f[5]) * t[6] + D(f[4]) * t[3]) + D(f[3]) * t[0]);
    o[6] = (float)((D(f[8]) * t[6] + D(f[7]) * t[3]) + D(f[6]) * t[0]);
    o[1] = (float)((D(f[2]) * t[7] + D(f[1]) * t[4]) + D(f[0]) * t[1]);
    o[4] = (float)((D(f[5]) * t[7] + D(f[3]) * t[1]) + D(f[4]) * t[4]);
    o[7] = (float)((D(f[6]) * t[1] + D(f[8]) * t[7]) + D(f[7]) * t[4]);
    o[2] = (float)((D(f[1]) * t[5] + D(f[2]) * t[8]) + D(f[0]) * t[2]);
    o[5] = (float)((D(f[4]) * t[5] + D(f[3]) * t[2]) + D(f[5]) * t[8]);
    o[8] = (float)((D(f[7]) * t[5] + D(f[6]) * t[2]) + D(f[8]) * t[8]);
    r.pos.x = (float)((D(hit.x) - fr.pos.x) * 2.0f + fr.pos.x);
    r.pos.y = (float)((D(hit.y) - fr.pos.y) * 2.0f + fr.pos.y);
    r.pos.z = (float)((D(hit.z) - fr.pos.z) * 2.0f + fr.pos.z);
    mrModelSetAlpha(fbits((float)((1.0f - x87_sqrt(Fat(self, 8)) / 30.0f) * FB(0x3e19999a))));
    mrDisable(0x100);
    mrDisable(0x80);
    mrEnable(0x200);
    mrModelDraw(Iat(self, 0xc), &r);
    mrDisable(0x200);
    mrEnable(0x80);
    mrEnable(0x100);
    mrModelClearAlpha();
}
static void fp_reflection_draw(Footprint& f, uint8_t* self, Edx) {
    f.add(self + 0x10, 12, "the reflection's smoothed normal");
    fp_render(f);
    fp_terrain(f);
}
PORT_FN(0x00468450, "ReflectionObject::Draw", ReflectionObject_Draw, fp_reflection_draw)

// ==== SkidObject::Draw (0x4688d0) ===============================================================================
// Without effects the marks are cleared. Otherwise each mark in use is a quad (two triangles, 0 1 2 / 1 3 2) of
// its first and last edges, u 0 / 1 across, v 0 / 1 along, white; Z writes off, the texture's alpha.
static void __fastcall SkidObject_Draw(uint8_t* self, Edx) {
    if (!WorldFXEnabled()) {
        SkidObject_Clear(self, 0);
        return;
    }
    const uint8_t* mark = Pat(self, 4);
    Vtx v[4];
    for (int k = 0; k < 4; k++) {
        v[k].color = 0xffffffff;
        v[k].spec = 0xffffffff;
    }
    mrEnable(8);
    mrDisable(0x100);
    mrBeginTriangles(Iat(self, 0x10));
    const uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
    for (int i = 0; Iat(self, 8) > i; i++, mark += 0x38) {
        if (*(const int32_t*)(mark + 0x30) == 0) continue;
        cp12(&v[0], mark);
        put4(&v[0].u, 0);
        put4(&v[0].v, 0);
        cp12(&v[1], mark + 0xc);
        put4(&v[1].v, 0);
        put4(&v[1].u, 0x3f800000);
        cp12(&v[2], mark + 0x18);
        put4(&v[2].u, 0);
        put4(&v[2].v, 0x3f800000);
        cp12(&v[3], mark + 0x24);
        put4(&v[3].u, 0x3f800000);
        put4(&v[3].v, 0x3f800000);
        mrDrawIndexTriangles(v, 4, idx, 6);
    }
    mrEndTriangles();
    mrEnable(0x100);
    mrDisable(8);
}
static void fp_skid_draw(Footprint& f, uint8_t* self, Edx) {
    f.add(self, 0x14, "the SkidObject (Clear)");
    if (Pat(self, 4) && Iat(self, 8) > 0) f.add(Pat(self, 4), (uint32_t)Iat(self, 8) * 0x38, "the skid marks (Clear)");
    fp_render(f);
}
PORT_FN(0x004688d0, "SkidObject::Draw", SkidObject_Draw, fp_skid_draw)

// ==============================================================================================================
// wob.obj
// ==============================================================================================================

// ==== ModelObject::Draw (0x469ac0) / WobbleObject::Draw (0x469b20): the model (+0x3c) in the object's frame ========
static void __fastcall ModelObject_Draw(uint8_t* self, Edx) { mrModelDraw(Iat(self, 0x3c), self + 4); }
static void __fastcall WobbleObject_Draw(uint8_t* self, Edx) { mrModelDraw(Iat(self, 0x3c), self + 4); }
static void fp_model_draw(Footprint& f, uint8_t*, Edx) { fp_render(f); }
PORT_FN(0x00469ac0, "ModelObject::Draw", ModelObject_Draw, fp_model_draw)
PORT_FN(0x00469b20, "WobbleObject::Draw", WobbleObject_Draw, fp_model_draw)

// ==============================================================================================================
// debris.obj
// ==============================================================================================================

// ==== DebrisObject::Draw (0x46eaf0): a jump to WorldFXEnabled (the smoke draws itself) ==========================
static void __fastcall DebrisObject_Draw(uint8_t*, Edx) { WorldFXEnabled(); }
static void fp_debris_draw(Footprint&, uint8_t*, Edx) {}
PORT_FN(0x0046eaf0, "DebrisObject::Draw", DebrisObject_Draw, fp_debris_draw)

// ==== SparksObject::Draw (0x46eb80) =============================================================================
// Each spark younger than a second (effects and smoke on): a small triangle facing the camera (0.2 m along its
// right and up rows) at its position, additive, Z writes off.
static void __fastcall SparksObject_Draw(uint8_t* self, Edx) {
    if (!WorldFXEnabled()) return;
    if (!WorldShowSmoke()) return;
    const float t = (float)WorldGetElapsedTime();
    mrPushState();
    mrSetBlendMode(2);
    mrDisable(0x100);
    mrBeginTriangles(Iat(self, 0x388));
    Vtx v[3];
    for (int k = 0; k < 3; k++) {
        v[k].color = 0xffffffff;
        v[k].spec = 0xffffffff;
        v[k].u = 0;
        v[k].v = 0x3e800000;
    }
    const float* cam = P<float>(S_CAMERA);
    uint8_t* p = self + 4;
    for (int k = 0; k < 32; k++, p += 0x1c) {
        const double ad = D(t) - Fat(p, 0x18);
        const float age = (float)ad;
        if (!(ad >= 0.0f)) continue;                                   // fcom 0; test ah,1
        if ((int32_t)fbits(age) >= 0x3f800000) continue;
        P3 q, s, r;
        GetParticlePosition_debris(&q, (const P3*)(p + 0xc), (const P3*)p, fbits(age));
        const double k0 = D(cam[0]) * FB(0x3e4ccccd);
        v[1].u = 0x3e000000;
        cp12(&v[0], &q);
        v[2].v = 0x3ec00000;
        s.x = (float)(k0 + q.x);
        s.y = (float)(D(cam[1]) * FB(0x3e4ccccd) + q.y);
        s.z = (float)(D(cam[2]) * FB(0x3e4ccccd) + q.z);
        const double k3 = D(cam[3]) * FB(0x3e4ccccd);
        cp12(&v[2], &s);
        r.x = (float)(k3 + q.x);
        r.y = (float)(D(cam[4]) * FB(0x3e4ccccd) + q.y);
        r.z = (float)(D(cam[5]) * FB(0x3e4ccccd) + q.z);
        cp12(&v[1], &r);
        mrDrawTriangle(v);
    }
    mrEndTriangles();
    mrPopState();
}
static void fp_sparks_draw(Footprint& f, uint8_t*, Edx) { fp_render(f); }
PORT_FN(0x0046eb80, "SparksObject::Draw", SparksObject_Draw, fp_sparks_draw)

// ==============================================================================================================
// splash.obj
// ==============================================================================================================

// ==== SplashObject::Draw (0x46f530): its 15 vertices and 96 indices, the texture's alpha, Z writes off ===========
static void __fastcall SplashObject_Draw(uint8_t* self, Edx) {
    mrEnable(8);
    mrDisable(0x100);
    mrBeginTriangles(Iat(self, 8));
    mrDrawIndexTriangles(self + 0x18c, 0xf, (const uint16_t*)(self + 0xcc), 0x60);
    mrEndTriangles();
    mrEnable(0x100);
    mrDisable(8);
}
static void fp_splash_draw(Footprint& f, uint8_t*, Edx) { fp_render(f); }
PORT_FN(0x0046f530, "SplashObject::Draw", SplashObject_Draw, fp_splash_draw)
