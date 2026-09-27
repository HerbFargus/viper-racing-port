// wld_carobj.cpp -- M3 world stage, group W5: the car as the renderer sees it, the view set-up and the sky, and
// the track's scenery graph, rewritten.
//
//   carwob.obj  CarObject (constructor, destructor, Update, IsAlpha, IsVisible, IsGhostCar, UpdateMessage -- the
//               physics packet applied: the frame, the view mode, the damage texture, the wheels' frames and skid
//               marks --, load_car_models, load_wheels, InitTextures, find_available_smoke), GhostCarObject
//               (constructor, destructor, UpdateMessage, IsGhostCar), GetParticlePosition, RandomReal, and the
//               trivial constructors Frame / SkidClient / DustParticle / WheelMessage
//   view.obj    ViewPreBegin, ViewPostEnd, ViewBegin, ViewEnd, the mirror mode, the getters, ViewSetSky and the
//               sky model (reset_sky_square, add_sky_square, add_sky_top, build_sky_model, destroy_sky_model)
//   graf.obj    GrafLoad, GrafUnload, GrafLookupDynoModel, GrafSetMinLOD / LODBias / ClipDistance, fixup_graf,
//               fixup_ptr, fixup_objs, cleanup_objs
//
// Not rewritten here: everything that draws (CarObject::Draw / Draw2D / DrawWheel / DrawXRay, ViewDraw,
// ViewDrawMirror -- a jump to TrackDraw --, GrafDraw, draw_tree: the graphics stage); the bare `ret` stubs
// ViewUpdate (0x466590), ViewDraw2D (0x466850), init_lights (0x466860, still called by address as ViewBegin
// does), SkidClient::~SkidClient (0x46ce00, called likewise) and GhostCarObject::Draw2D (0x46ce10, `ret 4`); the
// scalar deleting destructors (0x46cdc0, 0x46ce30) and the $E initialisers. Everything else they call -- gx*, mr*,
// the World* getters, the resource and string-table code, the game's CRT, the matrix helpers -- is called by its
// v1.0 address, and so are this file's own functions where one calls another (a hooked rewrite is what runs).
//
// Threads: all of it runs on the MAIN thread (WorldBegin / WorldUpdate / the renderer), none on the physics
// thread, so no physics or AI static is saved for it: each footprint lists the statics it writes. Model and
// texture loading, the renderer's state, allocation and the resource-relocating graf code are replay_only.
//
// ---- layouts (v1.0; evidence in the comments: the constructor's stores, UpdateMessage, the destructor) ----------
//   WorldObject (0x3c): vtable; Frame +4 (rotation +4, position +0x28); PhysicsCreate's handle +0x34; 0xbad +0x38
//   CarMessage (0x18c, Car::GetMessage): the frame at +4 (its position is the model origin, +0x28), the wheels'
//   WheelMessages at +0x78 (0x38 each), opacity +0x158, look side +0x15c, look back +0x160, the damage grid +0x162
//   (16 x 16 bits), coolant +0x184, block temperature +0x188; a ghost car's message adds its car's index at +0x18c.
//   CarObject (0x844) = WorldObject + the whole last message at +0x3c, so message +X is CarObject +0x3c+X (below).
//
// The statics (v1.0):
//   view.obj  0x555860 int video_mode; 0x555864 float draw distance (m); 0x555868 float detail level; 0x55586c
//             float LOD bias (2 - detail); 0x555870 int min car LOD; 0x555874..0x555879 bytes: sky on, 0x555875 (set
//             1), fog, lighting, filtering, mipmap; 0x555880 / 0x555888 floats: the sky's lower / upper height
//             (ViewSetSky); 0x555898 int the sky model; 0x5558ac float (the projection's third argument, not ours);
//             0x5558b8 Frame (identity); 0x5558f0 the sky's vertices (32 bytes, 200), 0x5571f0 int (zeroed),
//             0x5571f8 its materials (32 bytes, 50), 0x557838 its faces (8 bytes); 0x4f4c88 int frames drawn;
//             0x4f4c8c..0x4f4c98 mrGetStats; 0x4f4c9c byte mirror mode; 0x4f4ca0 / 0x4f4ca4 / 0x4f4ca8 ints the
//             sky's vertex / face / material counts; 0x4f4cb0 the sky's mrModelInfo {verts, ptr, mats, ptr,
//             faces, ptr, ...}
//   graf.obj  0x4f5284 float min LOD (squared); 0x4f5288 float clip distance; 0x4f528c float 90000 (set with
//             it); 0x4f5290 float LOD bias (squared); 0x558118 Frame (identity, GrafDraw's); 0x559168 int[512] the
//             dynamic models by facing id; 0x559970 {char* name; GrafNode* root}[] the loaded grafs (only [1])
//   carwob    0x558068 CarObject*[16] by car index
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
static __forceinline uint32_t& U(uint32_t addr) { return *(uint32_t*)(uintptr_t)addr; }
static __forceinline int32_t& I(uint32_t addr) { return *(int32_t*)(uintptr_t)addr; }
static __forceinline float& F(uint32_t addr) { return *(float*)(uintptr_t)addr; }
static __forceinline uint8_t& B(uint32_t addr) { return *(uint8_t*)(uintptr_t)addr; }
static __forceinline uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static __forceinline float asf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// fld dword; fchs; (fst) dword -- through the FPU, as the original does (a signalling NaN comes out quiet)
static __forceinline float fpu_neg(const uint32_t* p) {
    float r;
    __asm { mov eax, p
            fld dword ptr [eax]
            fchs
            fstp r }
    return r;
}

// ---- layouts ------------------------------------------------------------------------------------------------
struct WheelMsg {                      // WheelMessage (phys_wheel.cpp), as the renderer reads it
    P3 hub_pos;                        // +0x00
    float hub_height;                  // +0x0c
    float steer_angle, camber;         // +0x10
    float omega, spin_angle, slide;    // +0x18
    float lat_force_ratio, long_force_ratio, compression_fraction, brake_temp;   // +0x24
    uint8_t fx_flags, _35[3];          // +0x34  bit 0: skidding (a skid mark), 0x40: broken
};
static_assert(sizeof(WheelMsg) == 0x38, "WheelMsg");

struct LodRow {                        // one row of "<car>L.tab" (load_car_models)
    float dist;                        // +0  from this camera distance (x gxLODFactor) on
    int32_t faces;                     // +4  column 1, atoi; -1 when it has an 'x'
    uint8_t spec, alpha, _a[2];        // +8  column 2 has "spec" / "alpha" (alpha: IsAlpha)
};
static_assert(sizeof(LodRow) == 12, "LodRow");

struct DustParticle { P3 pos, vel; float life; };   // 0x1c; the constructor sets life 1e6 and the rest 0

struct WheelModels {                   // load_wheels: fwheel_%d / wheel_%d.mod scaled to the front / rear tyres
    int32_t fwheel_front, wheel_front, fwheel_rear, wheel_rear;
};

struct SkidClient { uint8_t raw[0x2c]; };   // its constructor zeroes +0x18, +0x1c, +0x24; +0x1c the SkidObject

struct CarObject {
    void** vtable;                     // +0x000  0x4dd4d0 (GhostCarObject 0x4dd500)
    Frame frame;                       // +0x004  WorldObject: rotation, then position (the model origin)
    int32_t phob;                      // +0x034  PhysicsCreate's handle
    uint32_t _038;                     // +0x038  0xbad (WorldObject's constructor)
    uint8_t msg[0x18c];                // +0x03c  the last CarMessage, whole (UpdateMessage's rep movsd)
    int32_t index;                     // +0x1c8  the car's index (PhobData +0x194)
    char skin[16];                     // +0x1cc  its texture's name (WorldGetCarTexture)
    int32_t lod_model[8];              // +0x1dc  "%s%d.mod" per LOD row (mrModelLoadRemap)
    int32_t lod_copy[8];               // +0x1fc  their mrModelCopyRemap copies
    int32_t shadow_model;              // +0x21c  "%ss.mod" (high detail only), else 0
    LodRow lod[8];                     // +0x220  "%sL.tab"
    int32_t num_lods;                  // +0x280
    int32_t* lod_models_p;             // +0x284  -> lod_copy (a ghost: its car's lod_model)
    LodRow* lod_p;                     // +0x288  -> lod (a ghost: its car's)
    int32_t* num_lods_p;               // +0x28c  -> num_lods (a ghost: its car's)
    int32_t needle[2];                 // +0x290  needle.mod twice
    uint8_t has_cockpit, _299[3];      // +0x298  "%s.car/cockpit.tab" exists
    int32_t cockpit_c, cockpit_w;      // +0x29c  "%sc.mod", +0x2a0 "%sw.mod"
    int32_t arm[6];                    // +0x2a4  arm_ul, arm_ur, arm_ll, arm_lr, arm_sl, arm_sr.mod
    int32_t arm_copy[2];               // +0x2bc  copies of arm_sl, arm_sr
    int32_t diskglow;                  // +0x2c4
    int32_t spin[2];                   // +0x2c8  spin_l, spin_r.mod
    int32_t brakelight;                // +0x2d0  "%sb.mod" or brakelt.mod
    float cockpit_row1[3];             // +0x2d4  cockpit.tab row 1, columns 1-3
    float cockpit_row3[3];             // +0x2e0  row 3
    float cockpit_row2[3];             // +0x2ec  row 2
    float needle2_min, needle2_scale;  // +0x2f8  row 5: -col1 rad; (-col2 rad - that) / col3
    float needle1_min, needle1_scale;  // +0x300  row 4
    int32_t skin_tex;                  // +0x308  -1, then the remap's (the texture the damage is blitted into)
    int32_t src_tex;                   // +0x30c  gxGetTexture(skin)
    int32_t damage_tex;                // +0x310  InitTextures ("%sd.tex" alpha-blitted), damage option only
    int32_t effects_tex;               // +0x314  effects.tex
    int32_t xray_tex;                  // +0x318  xray.tex
    char remap_name[16];               // +0x31c  mrModelRemapInfo: "%s.tex"
    char remap_skin[16];               // +0x32c  the skin's name
    int32_t remap_car;                 // +0x33c  index + 1
    int32_t* remap_tex;                // +0x340  -> skin_tex
    DustParticle dust[16];             // +0x344
    int32_t _504, _508;                // +0x504  zeroed
    float cam_dist;                    // +0x50c  camera distance x gxLODFactor (Update)
    int32_t lod_now, lod_now2;         // +0x510  the LOD row in use (+ the min car LOD, + half of it)
    WheelModels wheel_models[3];       // +0x518
    Frame wheel_frame[4];              // +0x548  the car's rotation x camber, at the hub (+ the radius, up)
    Frame wheel_frame2[4];             // +0x608  ... x steer (and pi on the right)
    P3 wheel_move[4];                  // +0x6c8  the hub's move since the last message
    SkidClient skid[4];                // +0x6f8
    uint32_t _7a8;                     // +0x7a8
    float radius_front, radius_rear;   // +0x7ac  tyre radius (m): rim x 0.0127 + width x aspect x 1e-5
    void* stamp;                       // +0x7b4  thought.stp
    void* smoke[32];                   // +0x7b8
    int32_t num_smoke;                 // +0x838  (not written when there's no smoke)
    int32_t view_mode, last_view_mode; // +0x83c  0 outside, 1 cockpit, 2 bumper (the focus car only)
};
static_assert(offsetof(CarObject, msg) == 0x3c && offsetof(CarObject, index) == 0x1c8 &&
              offsetof(CarObject, lod) == 0x220 && offsetof(CarObject, num_lods) == 0x280 &&
              offsetof(CarObject, needle) == 0x290 && offsetof(CarObject, cockpit_row1) == 0x2d4 &&
              offsetof(CarObject, needle2_min) == 0x2f8 && offsetof(CarObject, skin_tex) == 0x308 &&
              offsetof(CarObject, remap_name) == 0x31c && offsetof(CarObject, dust) == 0x344 &&
              offsetof(CarObject, cam_dist) == 0x50c && offsetof(CarObject, wheel_models) == 0x518 &&
              offsetof(CarObject, wheel_frame) == 0x548 && offsetof(CarObject, wheel_frame2) == 0x608 &&
              offsetof(CarObject, wheel_move) == 0x6c8 && offsetof(CarObject, skid) == 0x6f8 &&
              offsetof(CarObject, radius_front) == 0x7ac && offsetof(CarObject, smoke) == 0x7b8 &&
              offsetof(CarObject, num_smoke) == 0x838 && sizeof(CarObject) == 0x844, "CarObject");
static __forceinline WheelMsg* wheel_msg(CarObject* c, int i) { return (WheelMsg*)(c->msg + 0x78 + 0x38 * i); }
static __forceinline uint16_t* damage_grid(CarObject* c) { return (uint16_t*)(c->msg + 0x162); }   // +0x19e
static __forceinline float& opacity(CarObject* c) { return *(float*)(c->msg + 0x158); }           // +0x194

// GrafNode (graf.obj, the "track.grf" resource): +0 type (0 group, 1 sphere-culled group, 2 LOD, 3 model, 4 facing
// model), +4 and +8 the two links (offsets from the resource until fixup_ptr), +0xc the model (3, 4) or the cull
// flag (1); LOD: +0x18 / +0x1c distances (squared by fixup_objs; -1 = none); model: an mrModelInfo at +0x10
// ({verts, ptr, mats, ptr, faces, ptr, n, ptr, n, ptr}), its data from +0x38 (type 3) or +0x4c (type 4, whose
// +0x38 is the facing mode and +0x48 the facing id)
struct GrafNode { int32_t type; GrafNode* a; GrafNode* b; int32_t model; int32_t info[10]; };

// the sky's mesh records (mrModelInfo's): a vertex (32 bytes), a material (32), a face (8)
struct SkyVert { uint32_t x, y, z, color, spec, u, v, _1c; };
}  // namespace

enum : uint32_t {
    // view.obj
    S_VIDEO_MODE = 0x00555860, S_DRAW_DIST = 0x00555864, S_DETAIL = 0x00555868, S_LOD_BIAS = 0x0055586c,
    S_MIN_CAR_LOD = 0x00555870, S_SKY_ON = 0x00555874, S_555875 = 0x00555875, S_FOG = 0x00555876,
    S_LIGHTING = 0x00555877, S_FILTERING = 0x00555878, S_MIPMAP = 0x00555879, S_SKY_LOW = 0x00555880,
    S_SKY_HIGH = 0x00555888, S_SKY_MODEL = 0x00555898, S_PROJ3 = 0x005558ac, S_VIEW_FRAME = 0x005558b8,
    S_SKY_VERTS = 0x005558f0, S_5571F0 = 0x005571f0, S_SKY_MATS = 0x005571f8, S_SKY_FACES = 0x00557838,
    S_FRAMES = 0x004f4c88, S_STATS = 0x004f4c8c, S_MIRROR = 0x004f4c9c, S_SKY_NV = 0x004f4ca0,
    S_SKY_NF = 0x004f4ca4, S_SKY_NM = 0x004f4ca8, S_SKY_INFO = 0x004f4cb0, S_5558A8 = 0x005558a8,
    S_OPTIONS_SECTION = 0x004dd148,    // const char* "GX"
    // graf.obj
    S_GRAF_TYPE = 0x004f527c, S_GRAF_VER = 0x004f5280, S_MIN_LOD = 0x004f5284, S_CLIP = 0x004f5288,
    S_CLIP_90000 = 0x004f528c, S_GRAF_BIAS = 0x004f5290, S_GRAF_FRAME = 0x00558118, S_DYNO = 0x00559168,
    S_GRAFS = 0x00559970,
    // carwob.obj and around
    S_CAROBJS = 0x00558068, S_LOD_FACTOR = 0x004e53bc,
};

// ---- the functions they call, by address ----------------------------------------------------------------------
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t game_sprintf = (Sprintf_t)0x004cf0a0;
typedef double(__cdecl* Atof_t)(const char*);                     // fld qword: a double in ST0
static const Atof_t game_atof = (Atof_t)0x004cfe40;
typedef int(__cdecl* Atoi_t)(const char*);
static const Atoi_t game_atoi = (Atoi_t)0x004cf990;
typedef const char*(__cdecl* Strchr_t)(const char*, int);
static const Strchr_t game_strchr = (Strchr_t)0x004ce0c0;
typedef const char*(__cdecl* Strstr_t)(const char*, const char*);
static const Strstr_t game_strstr = (Strstr_t)0x004cf9a0;
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef void(__cdecl* Void_t)();
typedef void(__cdecl* VoidI_t)(int);
typedef void(__cdecl* VoidU_t)(uint32_t);
typedef uint8_t(__cdecl* Byte_t)();
typedef int(__cdecl* Int_t)();
typedef uint8_t(__cdecl* ByteI_t)(int);
// options
typedef void(__cdecl* OptionsGetI_t)(const char*, const char*, int*);
static const OptionsGetI_t OptionsGetInt = (OptionsGetI_t)0x004713e0;
typedef void(__cdecl* OptionsGetF_t)(const char*, const char*, float*);
static const OptionsGetF_t OptionsGetFloat = (OptionsGetF_t)0x00471350;
typedef void(__cdecl* OptionsGetB_t)(const char*, const char*, uint8_t*);
static const OptionsGetB_t OptionsGetByte = (OptionsGetB_t)0x00471470;
// gx
static const ByteI_t VidIsModeSupported = (ByteI_t)0x00454910;
static const VoidI_t gxChangeMode = (VoidI_t)0x0044e040;
typedef int(__cdecl* GetTexture_t)(const char*, int);
static const GetTexture_t gxGetTexture = (GetTexture_t)0x0044e240;
typedef uint8_t(__cdecl* Grab_t)(int, void*);
static const Grab_t gxGrabTexture = (Grab_t)0x0044e490;
static const Void_t gxReleaseTexture = (Void_t)0x0044e4d0;
static const VoidI_t gxForgetTexture = (VoidI_t)0x0044e280;
static const VoidI_t gxUnloadTexture = (VoidI_t)0x0044e450;
static const VoidI_t gxReloadTexture = (VoidI_t)0x0044e410;
typedef void(__cdecl* Blit_t)(int, int, uint32_t, uint32_t, uint32_t, uint32_t);      // the floats as bits
static const Blit_t gxBlitTexture = (Blit_t)0x0044e380;
typedef void(__cdecl* AlphaBlit_t)(const char*, int);
static const AlphaBlit_t gxAlphaBlitTexture = (AlphaBlit_t)0x0044e3d0;
typedef void*(__cdecl* GetStamp_t)(const char*);
static const GetStamp_t gxGetStamp = (GetStamp_t)0x00453500;
typedef void(__cdecl* ForgetStamp_t)(void*);
static const ForgetStamp_t gxForgetStamp = (ForgetStamp_t)0x004535d0;
// mr
typedef void(__cdecl* Float3_t)(uint32_t, uint32_t, uint32_t);                             // floats as bits
static const Float3_t mrSetProjection = (Float3_t)0x0044ede0;
static const Float3_t mrSetViewBackground = (Float3_t)0x0044ed60;
static const Void_t mrBeginFrame = (Void_t)0x0044e970;
static const Void_t mrEndFrame = (Void_t)0x0044e980;
static const VoidI_t mrEnable = (VoidI_t)0x00457890;
static const VoidI_t mrDisable = (VoidI_t)0x00457b00;
static const ByteI_t mrIsEnabled = (ByteI_t)0x00457d80;
static const VoidI_t mrLightMode = (VoidI_t)0x0045bf70;
typedef void(__cdecl* MirrorView_t)(uint8_t);
static const MirrorView_t mrMirrorView = (MirrorView_t)0x0044ee20;
typedef void(__cdecl* Stats_t)(int*, int*, int*, int*);
static const Stats_t mrGetStats = (Stats_t)0x0044e820;
typedef int(__cdecl* ModelCreate_t)(const char*, int);
static const ModelCreate_t mrModelCreate = (ModelCreate_t)0x00455b40;
typedef void(__cdecl* BuildLit_t)(int, void*, int);
static const BuildLit_t mrModelBuildLit = (BuildLit_t)0x00455ca0;
static const VoidI_t mrModelDestroy = (VoidI_t)0x00455bc0;
typedef int(__cdecl* ModelLoad_t)(const char*);
static const ModelLoad_t mrModelLoad = (ModelLoad_t)0x004558c0;
typedef int(__cdecl* LoadRemap_t)(const char*, void*, int);
static const LoadRemap_t mrModelLoadRemap = (LoadRemap_t)0x00455900;
typedef int(__cdecl* CopyRemap_t)(int, void*, int);
static const CopyRemap_t mrModelCopyRemap = (CopyRemap_t)0x00455a10;
typedef int(__cdecl* ModelCopy_t)(int);
static const ModelCopy_t mrModelCopy = (ModelCopy_t)0x00455b10;
static const VoidI_t mrModelUnload = (VoidI_t)0x00455950;
static const VoidI_t mrModelDestroyCopy = (VoidI_t)0x00455b30;
typedef void(__cdecl* Extents_t)(int, float*, float*, float*, float*, float*, float*);
static const Extents_t mrModelGetExtents = (Extents_t)0x00456ec0;
typedef int(__cdecl* CopyTransform_t)(int, const float*);
static const CopyTransform_t mrModelCopyTransform = (CopyTransform_t)0x00455aa0;
typedef double(__cdecl* CamDist_t)(const P3*);                  // fsqrt in ST0, unstored
static const CamDist_t mrCameraDistance = (CamDist_t)0x0044f140;
// resources, string tables
typedef void*(__cdecl* ResGet_t)(const char*, uint32_t, uint32_t*, int32_t*, uint8_t*, uint8_t*);
static const ResGet_t ResourceGet = (ResGet_t)0x00419fa0;
typedef uint8_t(__cdecl* ResForget_t)(void*);
static const ResForget_t ResourceForget = (ResForget_t)0x0041a450;
typedef uint8_t(__cdecl* ResExists_t)(const char*);
static const ResExists_t ResourceExists = (ResExists_t)0x00419d10;
typedef void*(__cdecl* StGet_t)(const char*);
static const StGet_t StringTableGet = (StGet_t)0x0041b210;
typedef int(__cdecl* StRows_t)(const void*);
static const StRows_t StringTableNumRows = (StRows_t)0x0041b290;
typedef const char*(__cdecl* StEntry_t)(const void*, int, int);
static const StEntry_t StringTableGetEntry = (StEntry_t)0x0041b2a0;
typedef void(__cdecl* StForget_t)(void*);
static const StForget_t StringTableForget = (StForget_t)0x0041b270;
// the world, the car manager, the physics
static const Void_t TerrainBegin = (Void_t)0x00465aa0;
static const Void_t TerrainEnd = (Void_t)0x00465ac0;
static const Void_t init_lights = (Void_t)0x00466860;             // a bare ret: kept, as ViewBegin calls it
static const Void_t TrackDraw = (Void_t)0x004695f0;
typedef uint8_t*(__cdecl* GameOpts_t)();
static const GameOpts_t WorldGameOptions = (GameOpts_t)0x004627a0;  // the GameOptions at 0x554024
static const Int_t WorldGetFocusCar = (Int_t)0x004626d0;
static const Int_t WorldGetCameraType = (Int_t)0x004626e0;
static const Int_t WorldGetPlayerCar = (Int_t)0x00462710;
static const Int_t WorldShowSkids = (Int_t)0x00461fa0;
static const Int_t WorldShowSmoke = (Int_t)0x00461fc0;
static const Int_t WorldShowShadow = (Int_t)0x00461f80;
static const Byte_t WorldShowReflection = (Byte_t)0x00461fe0;
static const Byte_t WorldFXEnabled = (Byte_t)0x00462860;
typedef void(__cdecl* AddGob_t)(void*);
static const AddGob_t WorldAddGob = (AddGob_t)0x004627c0;
typedef void(__cdecl* CarTexture_t)(char*, const char*, int);
static const CarTexture_t WorldGetCarTexture = (CarTexture_t)0x00462070;
typedef void(__cdecl* Register_t)(int, const void*, const char*, const char*, int);
static const Register_t CarMgrRegisterCar = (Register_t)0x00464390;
typedef const uint8_t*(__cdecl* CarInfo_t)(int);
static const CarInfo_t CarMgrGetInfo = (CarInfo_t)0x00464490;
static const Byte_t PhysicsIsPaused = (Byte_t)0x0042bd20;
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef int(__cdecl* Random_t)(int);
static const Random_t Random = (Random_t)0x0041b6e0;
// objects
typedef void*(__fastcall* Ctor_t)(void*, Edx);
static const Ctor_t WorldObject_ctor = (Ctor_t)0x00469920;
static const Ctor_t P3DBase_ctor = (Ctor_t)0x0045d8d0;
static const Ctor_t SmokeObject_ctor = (Ctor_t)0x00467c80;
typedef void*(__fastcall* SkidObjCtor_t)(void*, Edx, int);
static const SkidObjCtor_t SkidObject_ctor = (SkidObjCtor_t)0x004687d0;
typedef void*(__fastcall* NamedCtor_t)(void*, Edx, const char*, void*);
static const NamedCtor_t ShadowObject_ctor = (NamedCtor_t)0x00467480;
static const NamedCtor_t ReflectionObject_ctor = (NamedCtor_t)0x00468290;
typedef void(__fastcall* ThisVoid_t)(void*, Edx);
static const ThisVoid_t GraphObject_dtor = (ThisVoid_t)0x00469850;
typedef void(__fastcall* ThisPtr_t)(void*, Edx, const void*);
static const ThisPtr_t WorldObject_Create = (ThisPtr_t)0x004699f0;
static const ThisPtr_t WorldObject_UpdateMessage = (ThisPtr_t)0x00469a50;
typedef void(__fastcall* SkidOn_t)(void*, Edx, const P3*, const P3*);
static const SkidOn_t SkidClient_SkidOn = (SkidOn_t)0x00468ae0;
static const ThisVoid_t SkidClient_SkidOff = (ThisVoid_t)0x00468e20;
static const ThisVoid_t SkidClient_Clear = (ThisVoid_t)0x00468e40;
static const ThisVoid_t SkidClient_dtor = (ThisVoid_t)0x0046ce00;   // a bare ret: kept, as the original calls it
// matrices (union Matrix: 3x3, 36 bytes)
typedef void(__cdecl* Concat_t)(void*, const void*, const void*);
static const Concat_t MatrixConcat = (Concat_t)0x00403040;
typedef void(__cdecl* MakeAngle_t)(float*, uint32_t);          // the angle pushed as bits
static const MakeAngle_t MatrixMakeYaw = (MakeAngle_t)0x00402f80;
static const MakeAngle_t MatrixMakePitch = (MakeAngle_t)0x00402fc0;
static const MakeAngle_t MatrixMakeRoll = (MakeAngle_t)0x00403000;

// this file's own functions, by address (so a hooked rewrite is what runs)
static const Ctor_t WheelMessage_ctor_o = (Ctor_t)0x0046cdf0;
static const Ctor_t DustParticle_ctor_o = (Ctor_t)0x0046cde0;
static const Ctor_t Frame_ctor_o = (Ctor_t)0x0046cd90;
static const Ctor_t SkidClient_ctor_o = (Ctor_t)0x0046cda0;
typedef void(__fastcall* LoadModels_t)(void*, Edx, const char*, int, uint8_t);
static const LoadModels_t load_car_models_o = (LoadModels_t)0x0046c300;
static const ThisPtr_t load_wheels_o = (ThisPtr_t)0x0046ca60;
typedef void*(__fastcall* CarCtor_t)(void*, Edx, void*, const void*);
static const CarCtor_t CarObject_ctor_o = (CarCtor_t)0x00469e00;
static const ThisVoid_t CarObject_dtor_o = (ThisVoid_t)0x0046a170;
static const ThisPtr_t CarObject_UpdateMessage_o = (ThisPtr_t)0x0046bce0;
static const Int_t ViewGetMinCarLOD_o = (Int_t)0x00466810;
static const Byte_t ViewIsMirrorMode_o = (Byte_t)0x004667f0;
static const VoidU_t GrafSetClipDistance_o = (VoidU_t)0x0046dfd0;   // the float argument moved as bits
static const VoidU_t GrafSetLODBias_o = (VoidU_t)0x0046dfc0;
static const VoidI_t build_sky_model_o = (VoidI_t)0x00466ce0;
static const Void_t reset_sky_square_o = (Void_t)0x00466890;
static const Void_t destroy_sky_model_o = (Void_t)0x00466e70;
typedef void(__cdecl* SkySquare_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int);
static const SkySquare_t add_sky_square_o = (SkySquare_t)0x004668b0;
typedef void(__cdecl* SkyTop_t)(uint32_t, uint32_t);
static const SkyTop_t add_sky_top_o = (SkyTop_t)0x00466ae0;
typedef void(__cdecl* FixupGraf_t)(GrafNode*, void*, uint8_t);
static const FixupGraf_t fixup_graf_o = (FixupGraf_t)0x0046e130;
typedef void(__cdecl* FixupPtr_t)(GrafNode*, void*);
static const FixupPtr_t fixup_ptr_o = (FixupPtr_t)0x0046e160;
typedef void(__cdecl* FixupObjs_t)(GrafNode*, uint8_t);
static const FixupObjs_t fixup_objs_o = (FixupObjs_t)0x0046e1a0;
typedef void(__cdecl* Cleanup_t)(GrafNode*);
static const Cleanup_t cleanup_objs_o = (Cleanup_t)0x0046e300;

static void fp_none(Footprint&) {}
static void fp_pure(Footprint& f) { f.pure = true; }

// =============================================================================================================
// view.obj
// =============================================================================================================

// ==== ViewPreBegin (0x465ff0): the driving video mode ("GX" video_mode, 2 if unsupported), the projection, mipmaps
static void __cdecl ViewPreBegin() {
    const char* sect = *P<const char*>(S_OPTIONS_SECTION);
    OptionsGetInt(sect, (const char*)0x004f4d10, P<int>(S_VIDEO_MODE));              // "video_mode"
    if (!VidIsModeSupported(I(S_VIDEO_MODE))) I(S_VIDEO_MODE) = 2;
    if (I(S_VIDEO_MODE) != 2) {
        LogReport((const char*)0x004f4d1c, I(S_VIDEO_MODE));                          // "setting driving video mode = %d"
        gxChangeMode(I(S_VIDEO_MODE));
    }
    mrSetProjection(0x3f800000, 0x447a0000, U(S_PROJ3));                               // 1, 1000
    OptionsGetByte(sect, (const char*)0x004f4d3c, P<uint8_t>(S_MIPMAP));              // "mipmap"
    mrBeginFrame();
    if (B(S_MIPMAP)) mrEnable(0x20);
    else mrDisable(0x20);
    mrEndFrame();
}
static void fp_view_pre_begin(Footprint& f) { f.replay_only = "the video mode and the renderer's state"; }
PORT_FN(0x00465ff0, "ViewPreBegin", ViewPreBegin, fp_view_pre_begin)

// ==== ViewPostEnd (0x4660b0): back to mode 2
static void __cdecl ViewPostEnd() {
    if (I(S_VIDEO_MODE) != 2) gxChangeMode(2);
}
static void fp_view_post_end(Footprint& f) { f.replay_only = "the video mode"; }
PORT_FN(0x004660b0, "ViewPostEnd", ViewPostEnd, fp_view_post_end)

// ==== ViewBegin (0x4660d0): the options (draw distance x 1700 + 300 m, detail level, sky, fog, lighting,
// filtering), the min car LOD from the detail level, the sky's colour from sky1.tex's pixel 0x81 (565 or 555),
// the time of day (options +4: 0/1 lit, 2 night: no sky), the projection, the graf clip distance and LOD bias,
// the sky model (16 sides), the stats
static void __cdecl ViewBegin(const uint8_t* options) {
    U(S_5571F0) = 0;
    TerrainBegin();
    const char* sect = *P<const char*>(S_OPTIONS_SECTION);
    OptionsGetFloat(sect, (const char*)0x004f4d44, P<float>(S_DRAW_DIST));           // "draw_distance"
    OptionsGetFloat(sect, (const char*)0x004f4d54, P<float>(S_DETAIL));              // "detail_level"
    if (!(I(S_DETAIL) >= 0x3f000000)) U(S_DETAIL) = 0x3f000000;                      // an integer compare: 0.5 at least
    OptionsGetByte(sect, (const char*)0x004f4d64, P<uint8_t>(S_SKY_ON));             // "sky_texture"
    OptionsGetByte(sect, (const char*)0x004f4d70, P<uint8_t>(S_FOG));                // "fog"
    OptionsGetByte(sect, (const char*)0x004f4d74, P<uint8_t>(S_LIGHTING));           // "lighting"
    OptionsGetByte(sect, (const char*)0x004f4d80, P<uint8_t>(S_FILTERING));          // "filtering"
    const double bias = D(1.0f) - F(S_DETAIL);
    B(S_555875) = 1;
    F(S_LOD_BIAS) = (float)(bias + 1.0f);
    const int32_t detail = I(S_DETAIL);                                                // (the compare before the store)
    F(S_DRAW_DIST) = (float)(D(F(S_DRAW_DIST)) * 1700.0f + 300.0f);
    if (!(detail > 0x3e800000)) I(S_MIN_CAR_LOD) = 3;                                  // (can't happen after the clamp)
    else if (!(I(S_DETAIL) > 0x3f000000)) I(S_MIN_CAR_LOD) = 2;
    else {
        I(S_MIN_CAR_LOD) = 1;
        if (I(S_DETAIL) > 0x3f400000) I(S_MIN_CAR_LOD) = 0;
    }
    const int32_t time_of_day = *(const int32_t*)(options + 4);
    mrBeginFrame();
    float bgr[3] = {0.0f, 0.0f, 0.0f};                                                 // b, g, r (the original's order)
    const int tex = gxGetTexture((const char*)0x004f4d8c, 0);                          // "sky1.tex"
    struct { uint8_t format, _1[3]; const uint8_t* pixels; uint8_t _8[0x1c]; } canvas;
    if (gxGrabTexture(tex, &canvas)) {
        const uint32_t px = *(const volatile uint16_t*)(canvas.pixels + 0x102);       // read whatever the format
        if (canvas.format == 4) {                                                      // 565
            bgr[2] = (float)(D((int32_t)(px >> 11)) * 0.03125f + 0.015625f);
            bgr[1] = (float)(D((int32_t)((px & 0x7e0) >> 5)) * 0.015625f + 0.0078125f);
            bgr[0] = (float)(D((int32_t)(px & 0x1f)) * 0.03125f + 0.015625f);
            gxReleaseTexture();
        } else if (canvas.format == 3) {                                               // 555 (the top bit kept in red)
            bgr[2] = (float)(D((int32_t)(px >> 10)) * 0.03125f + 0.015625f);
            bgr[1] = (float)(D((int32_t)((px & 0x3e0) >> 5)) * 0.03125f + 0.0078125f);
            bgr[0] = (float)(D((int32_t)(px & 0x1f)) * 0.03125f + 0.015625f);
            gxReleaseTexture();
        } else {
            LogPanic((const char*)0x004f4d98);                                         // "sky: unusual texture format?"
            gxReleaseTexture();
        }
    } else LogReport((const char*)0x004f4db8);                                          // "sky grab failed!?"
    gxForgetTexture(tex);
    if (time_of_day < 0 || time_of_day > 2) LogPanic((const char*)0x004f4dcc);         // "unknown time of day for racing"
    else if (time_of_day <= 1) {
        mrLightMode(0);
        mrSetViewBackground(bits(bgr[2]), bits(bgr[1]), bits(bgr[0]));
    } else {
        mrLightMode(1);
        mrSetViewBackground(0, 0, 0);
        B(S_SKY_ON) = 0;
    }
    if (B(S_FOG)) mrEnable(0x40);
    else mrDisable(0x40);
    if (B(S_LIGHTING)) mrEnable(1);
    else mrDisable(1);
    if (B(S_FILTERING)) mrEnable(0x10);
    else mrDisable(0x10);
    mrEndFrame();
    mrSetProjection(0x3f800000, U(S_DRAW_DIST), U(S_PROJ3));
    GrafSetClipDistance_o(U(S_DRAW_DIST));
    GrafSetLODBias_o(U(S_LOD_BIAS));
    static const uint32_t ident[12] = {0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0};
    memcpy(P<void>(S_VIEW_FRAME), ident, 48);
    init_lights();
    build_sky_model_o(16);
    U(S_FRAMES) = 0;
    mrGetStats(P<int>(S_STATS), P<int>(S_STATS + 4), P<int>(S_STATS + 8), P<int>(S_STATS + 12));
    B(S_MIRROR) = 0;
}
static void fp_view_begin(Footprint& f, const uint8_t*) { f.replay_only = "options, textures, the renderer's state, the sky model"; }
PORT_FN(0x004660d0, "ViewBegin", ViewBegin, fp_view_begin)

// ==== ViewEnd (0x4664e0): after a minute of driving, "fps: %f" (frames x 1000 / the time); the sky model, the
// terrain; the background back to the menus' blue
static void __cdecl ViewEnd() {
    const int32_t ms = I(S_5558A8) - I(S_5571F0);
    if (ms > 60000) {
        int stats[4];
        mrGetStats(&stats[0], &stats[1], &stats[2], &stats[3]);
        const double fps = D(I(S_FRAMES)) * 1000.0 / D(ms);                           // fild; fmul qword; fidiv
        LogReport((const char*)0x004f4dec, fps);                                       // "fps: %f"
    }
    destroy_sky_model_o();
    TerrainEnd();
    mrBeginFrame();
    mrLightMode(0);
    mrSetViewBackground(0x3e20a0a1, 0x3edededf, 0x3f2babac);
    mrEndFrame();
}
static void fp_view_end(Footprint& f) { f.replay_only = "the sky model, the terrain, the renderer's state"; }
PORT_FN(0x004664e0, "ViewEnd", ViewEnd, fp_view_end)

// ==== the mirror (0x4666f0, 0x466780): a 150 m view at the renderer's mirror, LOD bias 4, no lighting or filtering;
// back again. (Entering stores mrIsEnabled over the lighting and filtering options.)
static void __cdecl ViewEnterMirrorMode() {
    mrSetProjection(0x3f800000, 0x43160000, 0x41a00000);                               // 1, 150, 20
    mrMirrorView(1);
    GrafSetClipDistance_o(0x43160000);
    GrafSetLODBias_o(0x40800000);
    B(S_LIGHTING) = mrIsEnabled(1);
    B(S_FILTERING) = mrIsEnabled(0x10);
    if (B(S_LIGHTING)) mrDisable(1);
    if (B(S_FILTERING)) mrDisable(0x10);
    B(S_MIRROR) = 1;
}
static void fp_view_mirror(Footprint& f) { f.replay_only = "the renderer's state"; }
PORT_FN(0x004666f0, "ViewEnterMirrorMode", ViewEnterMirrorMode, fp_view_mirror)

static void __cdecl ViewLeaveMirrorMode() {
    if (B(S_LIGHTING)) mrEnable(1);
    if (B(S_FILTERING)) mrEnable(0x10);
    mrMirrorView(0);
    mrSetProjection(0x3f800000, U(S_DRAW_DIST), U(S_PROJ3));
    GrafSetClipDistance_o(U(S_DRAW_DIST));
    GrafSetLODBias_o(U(S_LOD_BIAS));
    B(S_MIRROR) = 0;
}
PORT_FN(0x00466780, "ViewLeaveMirrorMode", ViewLeaveMirrorMode, fp_view_mirror)

// ==== the getters
static uint8_t __cdecl ViewIsMirrorMode() { return B(S_MIRROR); }
PORT_FN(0x004667f0, "ViewIsMirrorMode", ViewIsMirrorMode, fp_none)
static uint8_t __cdecl ViewIsSkyOn() { return B(S_SKY_ON); }
PORT_FN(0x00466800, "ViewIsSkyOn", ViewIsSkyOn, fp_none)
static int32_t __cdecl ViewGetMinCarLOD() { return I(S_MIN_CAR_LOD); }
PORT_FN(0x00466810, "ViewGetMinCarLOD", ViewGetMinCarLOD, fp_none)
static double __cdecl ViewGetDynoClipDist() { return D(F(S_DRAW_DIST)) * 0.25f; }        // ST0, unstored
PORT_FN(0x00466820, "ViewGetDynoClipDist", ViewGetDynoClipDist, fp_none)
static float __cdecl ViewGetStaticClipDist() { return F(S_DRAW_DIST); }                // fld dword
PORT_FN(0x00466830, "ViewGetStaticClipDist", ViewGetStaticClipDist, fp_none)

// ==== ViewSetSky (0x466870): the sky's lower and upper heights (moved as bits)
static void __cdecl ViewSetSky(uint32_t low, uint32_t high) {
    U(S_SKY_LOW) = low;
    U(S_SKY_HIGH) = high;
}
static void fp_view_set_sky(Footprint& f, uint32_t, uint32_t) {
    f.add(P<void>(S_SKY_LOW), 4, "sky low");
    f.add(P<void>(S_SKY_HIGH), 4, "sky high");
}
PORT_FN(0x00466870, "ViewSetSky", ViewSetSky, fp_view_set_sky)

// ==== reset_sky_square (0x466890)
static void __cdecl reset_sky_square() {
    U(S_SKY_NV) = 0;
    U(S_SKY_NF) = 0;
    U(S_SKY_NM) = 0;
}
static void fp_reset_sky_square(Footprint& f) { f.add(P<void>(S_SKY_NV), 12, "sky counts"); }
PORT_FN(0x00466890, "reset_sky_square", reset_sky_square, fp_reset_sky_square)

// the four vertices, two faces and one material a sky square or the top adds (the original's store order)
static __forceinline uint32_t* sky_vert(int v) { return P<uint32_t>(S_SKY_VERTS + 32 * v); }
static __forceinline uint16_t* sky_face(int f) { return P<uint16_t>(S_SKY_FACES + 8 * f); }
static void sky_material(int v, int f) {                        // after its name: from the counters, re-read
    const int32_t m = I(S_SKY_NM);
    uint8_t* mat = P<uint8_t>(S_SKY_MATS + 32 * m);
    const int32_t nv = I(S_SKY_NV);
    mat[0x10] = 2;
    mat[0x11] = 0;
    *(uint16_t*)(mat + 0x14) = 0;
    *(uint16_t*)(mat + 0x16) = 0;
    I(S_SKY_NM) = m + 1;
    *(uint16_t*)(mat + 0x18) = (uint16_t)v;
    const int32_t nf = I(S_SKY_NF);
    *(uint16_t*)(mat + 0x1a) = (uint16_t)nv;
    *(uint16_t*)(mat + 0x1c) = (uint16_t)f;
    *(uint16_t*)(mat + 0x1e) = (uint16_t)nf;
}
static void sky_footprint(Footprint& f) {
    f.add(P<void>(S_SKY_NV), 12, "sky counts");
    f.add(sky_vert(I(S_SKY_NV)), 4 * 32, "sky vertices");
    f.add(sky_face(I(S_SKY_NF)), 2 * 8, "sky faces");
    f.add(P<void>(S_SKY_MATS + 32 * I(S_SKY_NM)), 32, "sky material");
}

// ==== add_sky_square (0x4668b0): one side of the sky: (x0, lo, z0) (x1, lo, z1) (x0, hi, z0) (x1, hi, z1), UVs
// 1/128 .. 0.992, material "sky%d.tex" (4 - side % 4)
static void __cdecl add_sky_square(uint32_t x0, uint32_t y0, uint32_t z0, uint32_t x1, uint32_t y1, uint32_t z1, int side) {
    const int32_t v = I(S_SKY_NV), f = I(S_SKY_NF);
    const uint32_t lo = 0x3c000000, hi = 0x3f7e0000;
    uint32_t* p = sky_vert(v);
    p[0] = x0; p[1] = y0; p[2] = z0; p[6] = lo; p[7] = lo; p[4] = 0xffffffff; p[5] = 0xff000000;
    p = sky_vert(v + 1);
    p[0] = x1; p[1] = y0; p[2] = z1; p[6] = hi; p[7] = lo; p[4] = 0xffffffff; p[5] = 0xff000000;
    p = sky_vert(v + 2);
    p[0] = x0; p[1] = y1; p[2] = z0; p[6] = lo; p[7] = hi; p[4] = 0xffffffff; p[5] = 0xff000000;
    p = sky_vert(v + 3);
    p[0] = x1; p[1] = y1; p[2] = z1; p[6] = hi; p[7] = hi; p[4] = 0xffffffff; p[5] = 0xff000000;
    I(S_SKY_NV) = v + 4;
    uint16_t* q = sky_face(f);
    q[0] = (uint16_t)v; q[1] = (uint16_t)(v + 1); q[2] = (uint16_t)(v + 3); q[3] = 0;
    q = sky_face(f + 1);
    q[0] = (uint16_t)v; q[1] = (uint16_t)(v + 3);
    I(S_SKY_NF) = f + 2;
    q[2] = (uint16_t)(v + 2); q[3] = 0;
    game_sprintf(P<char>(S_SKY_MATS + 32 * I(S_SKY_NM)), (const char*)0x004f4df4, 4 - side % 4);   // "sky%d.tex"
    sky_material(v, f);
}
static void fp_add_sky_square(Footprint& f, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int) { sky_footprint(f); }
PORT_FN(0x004668b0, "add_sky_square", add_sky_square, fp_add_sky_square)

// ==== add_sky_top (0x466ae0): the lid, a square of half-side r at height h ("sky1.tex", UVs 1/128 .. 0.0178)
static void __cdecl add_sky_top(uint32_t r, uint32_t h) {
    const float nr = fpu_neg(&r);                                                      // fld; fchs, kept in ST0
    const int32_t v = I(S_SKY_NV), f = I(S_SKY_NF);
    const uint32_t lo = 0x3c000000, hi = 0x3c91eb85, n = bits(nr);
    uint32_t* p = sky_vert(v);
    p[0] = r; p[1] = h; p[2] = r; p[6] = lo; p[7] = lo; p[4] = 0xffffffff; p[5] = 0xff000000;
    p = sky_vert(v + 1);
    p[0] = n; p[1] = h; p[2] = r; p[6] = hi; p[7] = lo; p[4] = 0xffffffff; p[5] = 0xff000000;
    p = sky_vert(v + 2);
    p[0] = r; p[1] = h; p[2] = n; p[6] = lo; p[7] = hi; p[4] = 0xffffffff; p[5] = 0xff000000;
    p = sky_vert(v + 3);
    p[0] = n; p[1] = h; p[2] = n; p[6] = hi;
    I(S_SKY_NV) = v + 4;
    p[7] = hi; p[4] = 0xffffffff; p[5] = 0xff000000;
    uint16_t* q = sky_face(f);
    q[0] = (uint16_t)v; q[1] = (uint16_t)(v + 1); q[2] = (uint16_t)(v + 2); q[3] = 0;
    q = sky_face(f + 1);
    q[0] = (uint16_t)(v + 1); q[1] = (uint16_t)(v + 3);
    I(S_SKY_NF) = f + 2;
    q[2] = (uint16_t)(v + 2); q[3] = 0;
    game_sprintf(P<char>(S_SKY_MATS + 32 * I(S_SKY_NM)), (const char*)0x004f4e00);   // "sky1.tex"
    sky_material(v, f);
}
static void fp_add_sky_top(Footprint& f, uint32_t, uint32_t) { sky_footprint(f); }
PORT_FN(0x00466ae0, "add_sky_top", add_sky_top, fp_add_sky_top)

// ==== build_sky_model (0x466ce0): an n-sided ring of radius 500 between the two sky heights, and its lid
// (radius 500 sqrt 2). The ring's angles start at half a step. The original keeps sin and cos in two 32-entry
// stack arrays: n above 32 overruns its frame, and n <= 0 closes the ring on an uninitialised cos (its sin slot
// holds the step); ViewBegin passes 16.
static void __cdecl build_sky_model(int n) {
    reset_sky_square_o();
    const int model = mrModelCreate((const char*)0x004f4e0c, n * 4 + 2);               // "sky model"
    const double step = D(FB(0x40c90fdb)) / D(n);                                        // 2 pi (float) / n: fidiv
    U(S_SKY_MODEL) = (uint32_t)model;
    const uint32_t high = U(S_SKY_HIGH), low = U(S_SKY_LOW);
    float S[32], C[32];
    const float stepf = (float)step;                                                     // fst: its slot is S[0]
    S[0] = stepf;
    C[0] = 0.0f;
    float ang = (float)(step * 0.5f);
    const double st = stepf;                                                             // reloaded
    const double k500 = 500.0f;
    for (int i = 0; n > i; i++) {
        C[i] = (float)x87_cos_mul(ang, k500);
        S[i] = (float)x87_sin_mul(ang, k500);
        ang = (float)(D(ang) + st);
    }
    add_sky_top_o(bits((float)(x87_sqrt(2.0) * 500.0f)), high);
    const int last = n - 1;
    int i = 0;
    for (; last > i; i++) add_sky_square_o(bits(C[i + 1]), high, bits(S[i + 1]), bits(C[i]), low, bits(S[i]), i);
    add_sky_square_o(bits(C[0]), high, bits(S[0]), bits(C[i]), low, bits(S[i]), i);
    const int32_t nv = I(S_SKY_NV), nm = I(S_SKY_NM), nf = I(S_SKY_NF);
    I(S_SKY_INFO) = nv;
    I(S_SKY_INFO + 8) = nm;
    I(S_SKY_INFO + 16) = nf;
    mrModelBuildLit(I(S_SKY_MODEL), P<void>(S_SKY_INFO), 7);
}
static void fp_build_sky_model(Footprint& f, int) { f.replay_only = "creates the sky model"; }
PORT_FN(0x00466ce0, "build_sky_model", build_sky_model, fp_build_sky_model)

// ==== destroy_sky_model (0x466e70)
static void __cdecl destroy_sky_model() { mrModelDestroy(I(S_SKY_MODEL)); }
static void fp_destroy_sky_model(Footprint& f) { f.replay_only = "destroys the sky model"; }
PORT_FN(0x00466e70, "destroy_sky_model", destroy_sky_model, fp_destroy_sky_model)

// =============================================================================================================
// graf.obj
// =============================================================================================================

// ==== GrafLoad (0x46db30): the track's scene graph. The dynamic-model table is cleared first, whatever happens;
// a graf fetched for the first time is checked (version 3), relocated and its models built, and becomes graf 1;
// then GrafDraw's frame is the identity. Returns 1, or 0 without the resource.
static int __cdecl GrafLoad(char* name) {
    uint32_t version;
    int32_t size;
    uint8_t first, relocate;
    GrafNode* root = (GrafNode*)ResourceGet(name, U(S_GRAF_TYPE), &version, &size, &first, &relocate);
    for (int i = 0; i < 512; i++) I(S_DYNO + 4 * i) = -1;
    if (!root) return 0;
    if (first) {
        if (version != U(S_GRAF_VER)) LogPanic((const char*)0x004f5294);              // "bad graf version"
        fixup_graf_o(root, root, relocate);
        U(S_GRAFS + 8) = (uint32_t)(uintptr_t)name;
        U(S_GRAFS + 12) = (uint32_t)(uintptr_t)root;
    }
    static const uint32_t ident[12] = {0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0};
    memcpy(P<void>(S_GRAF_FRAME), ident, 48);
    return 1;
}
static void fp_graf_load(Footprint& f, char*) { f.replay_only = "loads and relocates a resource, builds models"; }
PORT_FN(0x0046db30, "GrafLoad", GrafLoad, fp_graf_load)

// ==== GrafUnload (0x46dc20): only graf 1
static void __cdecl GrafUnload(int h) {
    if (h == 1) {
        GrafNode** root = P<GrafNode*>(S_GRAFS + 8 * h + 4);
        cleanup_objs_o(*root);
        ResourceForget(*root);
    }
}
static void fp_graf_unload(Footprint& f, int) { f.replay_only = "destroys models, frees a resource"; }
PORT_FN(0x0046dc20, "GrafUnload", GrafUnload, fp_graf_unload)

// ==== GrafLookupDynoModel (0x46dc50)
static int32_t __cdecl GrafLookupDynoModel(int id) { return id >= 0 && id < 512 ? I(S_DYNO + 4 * id) : -1; }
static void fp_graf_lookup(Footprint&, int) {}
PORT_FN(0x0046dc50, "GrafLookupDynoModel", GrafLookupDynoModel, fp_graf_lookup)

// ==== the settings: min LOD and LOD bias squared; the clip distance as given (bits), and 90000 beside it
static void __cdecl GrafSetMinLOD(float x) { F(S_MIN_LOD) = (float)(D(x) * x); }
static void fp_graf_min_lod(Footprint& f, float) { f.add(P<void>(S_MIN_LOD), 4, "graf min LOD"); }
PORT_FN(0x0046dfb0, "GrafSetMinLOD", GrafSetMinLOD, fp_graf_min_lod)
static void __cdecl GrafSetLODBias(float x) { F(S_GRAF_BIAS) = (float)(D(x) * x); }
static void fp_graf_bias(Footprint& f, float) { f.add(P<void>(S_GRAF_BIAS), 4, "graf LOD bias"); }
PORT_FN(0x0046dfc0, "GrafSetLODBias", GrafSetLODBias, fp_graf_bias)
static void __cdecl GrafSetClipDistance(uint32_t d) {
    U(S_CLIP_90000) = 0x47afc800;
    U(S_CLIP) = d;
}
static void fp_graf_clip(Footprint& f, uint32_t) { f.add(P<void>(S_CLIP), 8, "graf clip distance"); }
PORT_FN(0x0046dfd0, "GrafSetClipDistance", GrafSetClipDistance, fp_graf_clip)

// ==== fixup_graf (0x46e130): relocate (when the resource asks), then build the models
static void __cdecl fixup_graf(GrafNode* root, void* base, uint8_t relocate) {
    if (relocate) fixup_ptr_o(root, base);
    fixup_objs_o(root, relocate);
}
static void fp_fixup_graf(Footprint& f, GrafNode*, void*, uint8_t) { f.replay_only = "relocates a resource, builds models"; }
PORT_FN(0x0046e130, "fixup_graf", fixup_graf, fp_fixup_graf)

// ==== fixup_ptr (0x46e160): both links from resource offsets to pointers; recursing on +4, looping on +8
static void __cdecl fixup_ptr(GrafNode* node, void* base) {
    do {
        if (node->a) node->a = (GrafNode*)((uintptr_t)node->a + (uintptr_t)base);
        if (node->b) node->b = (GrafNode*)((uintptr_t)node->b + (uintptr_t)base);
        if (node->a) fixup_ptr_o(node->a, base);
        node = node->b;
    } while (node);
}
static void fp_fixup_ptr(Footprint& f, GrafNode*, void*) { f.replay_only = "relocates a resource in place"; }
PORT_FN(0x0046e160, "fixup_ptr", fixup_ptr, fp_fixup_ptr)

// ==== fixup_objs (0x46e1a0): each model node's mrModelInfo pointed at its inline data (vertices 32 bytes,
// materials 32, faces 8, then 16-byte and a last array), the model built ("graf", lit) when it has materials, and
// a facing model (type 4, mode 4) entered in the dynamic-model table by id; with the flag, LOD distances squared
static void __cdecl fixup_objs(GrafNode* node, uint8_t squares) {
    do {
        const int32_t type = node->type;
        if (type == 3 || type == 4) {
            int32_t* mi = &node->info[0];                                       // +0x10
            uint8_t* data = (uint8_t*)node + (type == 3 ? 0x38 : 0x4c);
            if (mi[0] > 0) { mi[1] = (int32_t)(uintptr_t)data; data += mi[0] << 5; } else mi[1] = 0;
            if (mi[2] > 0) { mi[3] = (int32_t)(uintptr_t)data; data += mi[2] << 5; } else mi[3] = 0;
            if (mi[4] > 0) { mi[5] = (int32_t)(uintptr_t)data; data += mi[4] * 8; } else mi[5] = 0;
            if (mi[6] > 0) { mi[7] = (int32_t)(uintptr_t)data; data += mi[6] << 4; } else mi[7] = 0;
            mi[9] = (int32_t)(uintptr_t)data;
            if (!(mi[8] > 0)) mi[9] = 0;
            if (mi[2] > 0) {
                const int model = mrModelCreate((const char*)0x004f52cc, mi[4]);       // "graf"
                node->model = model;
                mrModelBuildLit(model, mi, 7);
                if (node->type == 4 && *(int32_t*)((uint8_t*)node + 0x38) == 4) {
                    const int32_t id = *(int32_t*)((uint8_t*)node + 0x48);
                    if (id >= 0 && id < 512) {
                        if (I(S_DYNO + 4 * id) == -1) I(S_DYNO + 4 * id) = node->model;
                        else LogPanic((const char*)0x004f52d4);                          // "two models with same id"
                    } else LogPanic((const char*)0x004f52ec);                            // "wrong facing id"
                }
            } else node->model = -1;
        } else if (type == 2 && squares) {
            float* lod = (float*)&node->info[2];                                         // +0x18, +0x1c
            lod[0] = (float)(D(lod[0]) * lod[0]);
            if (bits(lod[1]) != 0xbf800000) lod[1] = (float)(D(lod[1]) * lod[1]);
        }
        if (node->a) fixup_objs_o(node->a, squares);
        node = node->b;
    } while (node);
}
static void fp_fixup_objs(Footprint& f, GrafNode*, uint8_t) { f.replay_only = "builds models, rewrites a resource"; }
PORT_FN(0x0046e1a0, "fixup_objs", fixup_objs, fp_fixup_objs)

// ==== cleanup_objs (0x46e300): every model node's model destroyed
static void __cdecl cleanup_objs(GrafNode* node) {
    do {
        if (node->type == 3 && node->model != -1) mrModelDestroy(node->model);
        if (node->type == 4 && node->model != -1) mrModelDestroy(node->model);
        if (node->a) cleanup_objs_o(node->a);
        node = node->b;
    } while (node);
}
static void fp_cleanup_objs(Footprint& f, GrafNode*) { f.replay_only = "destroys models"; }
PORT_FN(0x0046e300, "cleanup_objs", cleanup_objs, fp_cleanup_objs)

// =============================================================================================================
// carwob.obj
// =============================================================================================================

// ==== the trivial constructors: `mov eax, ecx; ret` (SkidClient's zeroes three fields)
static void* __fastcall Frame_ctor(Frame* self, Edx) { return self; }
// (they write nothing; not marked pure only because the fuzzer compares the returned pointer across its two arenas:
// the world harness checks them)
static void fp_ctor_self(Footprint&, Frame*, Edx) {}
PORT_FN(0x0046cd90, "Frame::Frame", Frame_ctor, fp_ctor_self)
static void* __fastcall DustParticle_ctor(DustParticle* self, Edx) { return self; }
static void fp_dust_ctor(Footprint&, DustParticle*, Edx) {}
PORT_FN(0x0046cde0, "CarObject::DustParticle::DustParticle", DustParticle_ctor, fp_dust_ctor)
static void* __fastcall WheelMessage_ctor(WheelMsg* self, Edx) { return self; }
static void fp_wheel_msg_ctor(Footprint&, WheelMsg*, Edx) {}
PORT_FN(0x0046cdf0, "WheelMessage::WheelMessage", WheelMessage_ctor, fp_wheel_msg_ctor)
static void* __fastcall SkidClient_ctor(SkidClient* self, Edx) {
    uint32_t* w = (uint32_t*)self->raw;
    w[6] = 0;
    w[7] = 0;
    w[9] = 0;
    return self;
}
static void fp_skid_client_ctor(Footprint& f, SkidClient* self, Edx) { f.add(self, sizeof(SkidClient), "skid client"); }
PORT_FN(0x0046cda0, "SkidClient::SkidClient", SkidClient_ctor, fp_skid_client_ctor)

// ==== IsGhostCar, IsVisible
static uint8_t __fastcall CarObject_IsGhostCar(CarObject*, Edx) { return 0; }
static void fp_is_ghost(Footprint& f, CarObject*, Edx) { f.pure = true; }
PORT_FN(0x0046cdb0, "CarObject::IsGhostCar", CarObject_IsGhostCar, fp_is_ghost)
static uint8_t __fastcall GhostCarObject_IsGhostCar(CarObject*, Edx) { return 1; }
PORT_FN(0x0046ce20, "GhostCarObject::IsGhostCar", GhostCarObject_IsGhostCar, fp_is_ghost)
static uint8_t __fastcall CarObject_IsVisible(CarObject* self, Edx) {       // an integer compare: opacity > 0
    int32_t o;
    memcpy(&o, &opacity(self), 4);
    return o > 0;
}
static void fp_car_reads(Footprint&, CarObject*, Edx) {}
PORT_FN(0x0046c2f0, "CarObject::IsVisible", CarObject_IsVisible, fp_car_reads)

// ==== GetParticlePosition (0x46ad30): p + v t, and g t^2 (-9.81) on y
static void __cdecl GetParticlePosition(P3* out, const P3* p, const P3* v, float t) {
    out->x = (float)(D(v->x) * t + p->x);
    out->y = (float)(D(v->y) * t + p->y);
    out->z = (float)(D(v->z) * t + p->z);
    out->y = (float)((D(t) * t) * FB(0xc11cf5c3) + out->y);
}
static void fp_particle_position(Footprint& f, P3* out, const P3*, const P3*, float) {
    f.pure = true;
    f.add(out, sizeof(P3), "out");
}
PORT_FN(0x0046ad30, "GetParticlePosition(carwob.obj)", GetParticlePosition, fp_particle_position)

// ==== RandomReal (0x46b450): a + (b - a) x Random(1000) x 0.001001 (ST0, unstored)
static double __cdecl RandomReal(float a, float b) {
    const int r = Random(1000);
    const double span = D(b) - a;
    return span * (D(r) * FB(0x3a833405)) + a;
}
// (Random is a shadow check's input: the rewrite pass is fed the original's number, so ranmar's state, advanced
// once by the original, isn't part of the footprint)
static void fp_random_real(Footprint&, float, float) {}
PORT_FN(0x0046b450, "RandomReal(carwob.obj)", RandomReal, fp_random_real)

// ==== CarObject::find_available_smoke (0x46bca0): the first smoke puff that isn't visible (vtable +0xc), or -1
static int __fastcall CarObject_find_available_smoke(CarObject* self, Edx) {
    for (int i = 0; self->num_smoke > i; i++) {
        void* s = self->smoke[i];
        if (!VFN(s, 0xc, uint8_t)(s, 0)) return i;
    }
    return -1;
}
PORT_FN(0x0046bca0, "CarObject::find_available_smoke", CarObject_find_available_smoke, fp_car_reads)

// ==== CarObject::IsAlpha (0x46a470): never in the mirror; translucent while fading (opacity below 1, an integer
// compare), otherwise when not the focus car and its LOD row says "alpha"
static uint8_t __fastcall CarObject_IsAlpha(CarObject* self, Edx) {
    if (ViewIsMirrorMode_o()) return 0;
    const uint8_t focus = WorldGetFocusCar() - self->index == 0;
    int32_t o;
    memcpy(&o, &opacity(self), 4);
    if (o < 0x3f800000) return 1;
    if (focus) return 0;
    if (self->lod_p[self->lod_now].alpha != 0) return 1;
    return 0;
}
PORT_FN(0x0046a470, "CarObject::IsAlpha", CarObject_IsAlpha, fp_car_reads)

// ==== CarObject::Update (0x46a390): the LOD row from the camera distance (x gxLODFactor): the first whose
// distance is beyond it, else 4; plus the min car LOD (and half of it for the second); a ghost at least 2; at
// most the last row
static void __fastcall CarObject_Update(CarObject* self, Edx) {
    const double d = mrCameraDistance(&self->frame.pos);
    // (the original stores d first, then d x factor over it)
    self->cam_dist = (float)(d * F(S_LOD_FACTOR));
    self->lod_now = 4;
    self->lod_now2 = 4;
    const int32_t n = *self->num_lods_p;
    if (n > 0) {
        const LodRow* row = self->lod_p;
        for (int i = 0; i < n; i++, row++)
            if (!!(row->dist > self->cam_dist)) {                                        // test ah,0x41; je
                self->lod_now = i;
                self->lod_now2 = i;
                break;
            }
    }
    const int32_t m = ViewGetMinCarLOD_o();
    self->lod_now += m;
    self->lod_now2 += m / 2;
    if (VFN(self, 0x28, uint8_t)(self, 0)) {                                             // IsGhostCar
        if (self->lod_now < 2) self->lod_now = 2;
        if (self->lod_now2 < 2) self->lod_now2 = 2;
    }
    if (self->lod_now >= *self->num_lods_p) self->lod_now = *self->num_lods_p - 1;
    if (self->lod_now2 >= *self->num_lods_p) self->lod_now2 = *self->num_lods_p - 1;
}
static void fp_car_update(Footprint& f, CarObject* self, Edx) { f.add(&self->cam_dist, 12, "cam_dist, lod_now, lod_now2"); }
PORT_FN(0x0046a390, "CarObject::Update", CarObject_Update, fp_car_update)

// ==== CarObject::UpdateMessage (0x46bce0) ======================================================================
// The physics packet applied: WorldObject's (the frame); the view mode (the focus car in the cockpit 1, bumper 2,
// else 0); outside, the skin texture brought up to the damage grid (a cell that healed: the clean skin blitted back
// whole; each newly damaged cell: its 1/16 x 1/16 square of the damage texture; leaving the cockpit: the grid
// forgotten and the xray texture unloaded); the bumper view unloads the skin; then the whole message kept, and per
// wheel: its frame (the car's rotation x camber at the hub raised by the tyre radius, the front radius for wheels 0
// and 1), its move since the last message, the second frame (x steer, the right wheels turned by pi) and the skid
// mark (on while the wheel skids, off, or cleared without effects; nothing while paused or without skids).
static bool update_message_touches_outside(CarObject* self, const uint8_t* msg) {
    int32_t mode = 0;
    if (WorldGetFocusCar() == self->index) {
        const int t = WorldGetCameraType();
        mode = t == 0 ? 1 : t == 2 ? 2 : 0;
    }
    if (mode == 2 && self->last_view_mode != 2) return true;
    if (mode == 0) {
        if (self->last_view_mode != 0) return true;
        if (WorldGameOptions()[0x24]) {
            const uint16_t* old = damage_grid(self);
            const uint16_t* nw = (const uint16_t*)(msg + 0x162);
            for (int i = 0; i < 16; i++)
                if ((uint16_t)(~old[i] & nw[i]) || (old[i] & (uint16_t)~nw[i])) return true;
        }
    }
    if (WorldShowSkids() && self->index >= 0 && !(WorldFXEnabled() && PhysicsIsPaused())) return true;
    return false;
}
static void __fastcall CarObject_UpdateMessage(CarObject* self, Edx, const uint8_t* msg) {
    WorldObject_UpdateMessage(self, 0, msg);
    if (WorldGetFocusCar() == self->index) {
        const int t = WorldGetCameraType();
        if (t == 0) self->view_mode = 1;
        else if (t == 2) self->view_mode = 2;
        else self->view_mode = 0;
    } else self->view_mode = 0;
    const int32_t mode = self->view_mode;
    if (mode == 0) {
        uint8_t whole = 0, cells = 0;
        if (self->last_view_mode != mode) {
            gxUnloadTexture(self->xray_tex);
            whole = 1;
            cells = 1;
            memset(damage_grid(self), 0, 32);
        }
        uint16_t fresh[16];
        if (WorldGameOptions()[0x24]) {
            const uint16_t* old = damage_grid(self);
            const uint16_t* nw = (const uint16_t*)(msg + 0x162);
            for (int i = 0; i < 16; i++) {
                const uint32_t o = old[i], n = nw[i];
                const uint16_t d = (uint16_t)(~o & n);
                fresh[i] = d;
                if (o & ~n & 0xffff) whole = 1;
                if (d) cells = 1;
            }
        } else memset(fresh, 0, sizeof fresh);
        if (whole) {
            gxBlitTexture(self->src_tex, self->skin_tex, 0, 0, 0x3f800000, 0x3f800000);
            gxReloadTexture(self->skin_tex);
        }
        if (cells) {
            for (int row = 0; row < 16; row++)
                for (int col = 0; col < 16; col++) {
                    const uint32_t d = fresh[row];
                    if (d & (1u << col)) {
                        const float u0 = (float)(D(col) * 0.0625f), u1 = (float)(D(col + 1) * 0.0625f);
                        const float v0 = (float)(D(row) * 0.0625f), v1 = (float)(D(row + 1) * 0.0625f);
                        gxBlitTexture(self->damage_tex, self->skin_tex, bits(u0), bits(v0), bits(u1), bits(v1));
                    }
                }
            gxReloadTexture(self->skin_tex);
        }
    } else if (mode == 2 && self->last_view_mode != mode) gxUnloadTexture(self->skin_tex);
    memcpy(self->msg, msg, 0x18c);
    const float c0 = x87_cos_f(0.0), s0 = x87_sin_f(0.0);                               // fcos / fsin of 0.0
    const float ns0 = fpu_neg((const uint32_t*)&s0);
    self->last_view_mode = self->view_mode;
    for (int i = 0; i < 4; i++) {
        const uint32_t rb = (i == 0 || i == 1) ? bits(self->radius_front) : bits(self->radius_rear);
        WheelMsg* w = wheel_msg(self, i);
        Frame* wf = &self->wheel_frame[i];
        float hub[3];
        memcpy(hub, &w->hub_pos, 12);
        hub[1] = (float)(D(hub[1]) + asf(rb));
        const float* m = self->frame.rot.m;
        float q[3];
        q[0] = (float)((D(m[6]) * hub[2] + D(m[0]) * hub[0]) + D(m[3]) * hub[1]);
        q[1] = (float)((D(m[4]) * hub[1] + D(m[1]) * hub[0]) + D(m[7]) * hub[2]);
        q[2] = (float)((D(m[2]) * hub[0] + D(m[5]) * hub[1]) + D(m[8]) * hub[2]);
        q[0] = (float)(D(self->frame.pos.x) + q[0]);
        q[1] = (float)(D(self->frame.pos.y) + q[1]);
        q[2] = (float)(D(self->frame.pos.z) + q[2]);
        memcpy(&wf->rot, m, 36);
        P3* mv = &self->wheel_move[i];
        mv->x = (float)(D(q[0]) - wf->pos.x);
        mv->y = (float)(D(q[1]) - wf->pos.y);
        mv->z = (float)(D(q[2]) - wf->pos.z);
        memcpy(&wf->pos, q, 12);
        uint32_t camber;
        memcpy(&camber, &w->camber, 4);
        // MatrixMakePitch(0) and MatrixMakeYaw(0), inline
        float m1[9] = {1.0f, 0.0f, 0.0f, 0.0f, c0, s0, 0.0f, ns0, c0};
        float m2[9] = {c0, 0.0f, ns0, 0.0f, 1.0f, 0.0f, s0, 0.0f, c0};
        MatrixConcat(m2, m1, m2);
        MatrixMakeRoll(m1, camber);
        MatrixConcat(m2, m1, m2);
        MatrixConcat(wf, m2, wf);
        memcpy(&self->wheel_frame2[i], wf, 48);
        const float base = (i & 1) ? FB(0x40490fdb) : 0.0f;
        const float yaw = (float)(D(base) + w->steer_angle);
        float m3[9], m4[9];
        MatrixMakeYaw(m3, bits(yaw));
        MatrixMakePitch(m4, 0);
        MatrixConcat(m3, m4, m3);
        MatrixMakeRoll(m4, 0);
        MatrixConcat(m3, m4, m3);
        MatrixConcat(wf, m3, wf);
        if (WorldShowSkids() && self->index >= 0) {
            if (WorldFXEnabled()) {
                if (!PhysicsIsPaused()) {
                    if (w->fx_flags & 1) {
                        float at[3];
                        memcpy(at, &wf->pos, 12);
                        at[1] = (float)((D(at[1]) - self->radius_front) + FB(0x3dcccccd));   // always the front's
                        SkidClient_SkidOn(&self->skid[i], 0, (const P3*)at, (const P3*)&self->frame.rot.m[3]);
                    } else SkidClient_SkidOff(&self->skid[i], 0);
                }
            } else SkidClient_Clear(&self->skid[i], 0);
        }
    }
}
static void fp_car_update_message(Footprint& f, CarObject* self, Edx, const uint8_t* msg) {
    if (update_message_touches_outside(self, msg)) f.replay_only = "texture blits, skid marks";
    else f.add(self, sizeof(CarObject), "CarObject");
}
PORT_FN(0x0046bce0, "CarObject::UpdateMessage", CarObject_UpdateMessage, fp_car_update_message)

// ==== GhostCarObject::UpdateMessage (0x46cd50): the ghost uses its car's LOD table and models (its car's index at
// message +0x18c), then CarObject's
static void __fastcall GhostCarObject_UpdateMessage(CarObject* self, Edx, const uint8_t* msg) {
    const int32_t car = *(const int32_t*)(msg + 0x18c);
    uint8_t* other = (uint8_t*)P<CarObject*>(S_CAROBJS)[car] + 0x280;
    self->num_lods_p = (int32_t*)other;
    self->lod_models_p = (int32_t*)(other - 0xa4);
    self->lod_p = (LodRow*)(other - 0x60);
    CarObject_UpdateMessage_o(self, 0, msg);
}
static void fp_ghost_update_message(Footprint& f, CarObject* self, Edx, const uint8_t* msg) {
    if (update_message_touches_outside(self, msg)) f.replay_only = "texture blits, skid marks";
    else f.add(self, sizeof(CarObject), "GhostCarObject");
}
PORT_FN(0x0046cd50, "GhostCarObject::UpdateMessage", GhostCarObject_UpdateMessage, fp_ghost_update_message)

// ==== CarObject::load_car_models (0x46c300) ======================================================================
// "<name>L.tab" (the LOD rows: distance, faces or 'x', "spec" / "alpha"); "<name>.car/cockpit.tab", when there is
// one: the cockpit models, two needles, the cockpit's rows 1-3 and the needles' angles (rows 4, 5: -deg -> rad,
// and (max - min) / range); the brake lights; the skin ("<name>1.tex" without WorldGetCarTexture's) and its remap;
// the shadow ("%ss.mod", high detail only); each LOD's model and copy; the xray texture; the spinners and arms
static void __fastcall CarObject_load_car_models(CarObject* self, Edx, const char* name, int paint, uint8_t high) {
    char path[0x20], lod_name[0x40], shadow[0x20];
    game_sprintf(path, (const char*)0x004f4f64, name);                                   // "%sL.tab"
    void* st = StringTableGet(path);
    if (!st) LogPanic((const char*)0x004f4f6c, path);                                    // "Can't load LOD table %s"
    self->num_lods = StringTableNumRows(st);
    for (int i = 0; self->num_lods > i; i++) {
        const float dist = (float)game_atof(StringTableGetEntry(st, i, 0));
        self->lod_p[i].dist = dist;
        const int faces = game_atoi(StringTableGetEntry(st, i, 1));
        self->lod_p[i].faces = faces;
        if (game_strchr(StringTableGetEntry(st, i, 1), 'x')) self->lod_p[i].faces = -1;
        const uint8_t spec = game_strstr(StringTableGetEntry(st, i, 2), (const char*)0x004f4f84) != 0;   // "spec"
        self->lod_p[i].spec = spec;
        const uint8_t alpha = game_strstr(StringTableGetEntry(st, i, 2), (const char*)0x004f4f8c) != 0;  // "alpha"
        self->lod_p[i].alpha = alpha;
    }
    StringTableForget(st);
    game_sprintf(path, (const char*)0x004f4f94, name);                                   // "%s.car/cockpit.tab"
    if (ResourceExists(path)) {
        self->has_cockpit = 1;
        st = StringTableGet(path);
        game_sprintf(path, (const char*)0x004f4fa8, name);                               // "%sw.mod"
        self->cockpit_w = mrModelLoad(path);
        game_sprintf(path, (const char*)0x004f4fb0, name);                               // "%sc.mod"
        self->cockpit_c = mrModelLoad(path);
        self->needle[0] = mrModelLoad((const char*)0x004f4fb8);                         // "needle.mod"
        self->needle[1] = mrModelLoad((const char*)0x004f4fc4);                         // "needle.mod"
        self->cockpit_row1[0] = (float)game_atof(StringTableGetEntry(st, 1, 1));
        self->cockpit_row1[1] = (float)game_atof(StringTableGetEntry(st, 1, 2));
        self->cockpit_row1[2] = (float)game_atof(StringTableGetEntry(st, 1, 3));
        self->cockpit_row2[0] = (float)game_atof(StringTableGetEntry(st, 2, 1));
        self->cockpit_row2[1] = (float)game_atof(StringTableGetEntry(st, 2, 2));
        self->cockpit_row2[2] = (float)game_atof(StringTableGetEntry(st, 2, 3));
        self->cockpit_row3[0] = (float)game_atof(StringTableGetEntry(st, 3, 1));
        self->cockpit_row3[1] = (float)game_atof(StringTableGetEntry(st, 3, 2));
        self->cockpit_row3[2] = (float)game_atof(StringTableGetEntry(st, 3, 3));
        const double deg = FB(0x3c8efa35);                                               // pi / 180 (float)
        self->needle1_min = (float)(-game_atof(StringTableGetEntry(st, 4, 1)) * deg);
        float max = (float)(-game_atof(StringTableGetEntry(st, 4, 2)) * deg);
        double range = game_atof(StringTableGetEntry(st, 4, 3));
        self->needle1_scale = (float)((D(max) - self->needle1_min) / range);             // fdivrp: ST0 / ST1
        self->needle2_min = (float)(-game_atof(StringTableGetEntry(st, 5, 1)) * deg);
        max = (float)(-game_atof(StringTableGetEntry(st, 5, 2)) * deg);
        range = game_atof(StringTableGetEntry(st, 5, 3));
        self->needle2_scale = (float)((D(max) - self->needle2_min) / range);
        StringTableForget(st);
    } else self->has_cockpit = 0;
    game_sprintf(path, (const char*)0x004f4fd0, name);                                   // "%sb.mod"
    if (!ResourceExists(path)) game_sprintf(path, (const char*)0x004f4fd8);              // "brakelt.mod"
    self->brakelight = mrModelLoad(path);
    self->diskglow = mrModelLoad((const char*)0x004f4fe4);                              // "diskglow.mod"
    WorldGetCarTexture(self->skin, name, paint);
    if (!ResourceExists(self->skin)) game_sprintf(self->skin, (const char*)0x004f4ff4, name);   // "%s1.tex"
    game_sprintf(self->remap_name, (const char*)0x004f4ffc, name);                       // "%s.tex"
    {   // strcpy, inline in the original (repne scasb; rep movsd; rep movsb)
        const char* s = self->skin;
        char* d = self->remap_skin;
        size_t n = 0;
        while (s[n]) n++;
        memmove(d, s, n + 1);
    }
    self->remap_car = self->index + 1;
    self->remap_tex = &self->skin_tex;
    self->skin_tex = -1;
    self->src_tex = gxGetTexture(self->skin, 0);
    self->shadow_model = 0;
    game_sprintf(shadow, (const char*)0x004f5004, name);                                 // "%ss.mod"
    if (high && ResourceExists(shadow)) self->shadow_model = mrModelLoadRemap(shadow, self->remap_name, 1);
    for (int i = 0; self->num_lods > i; i++) {
        game_sprintf(lod_name, (const char*)0x004f500c, name, i);                        // "%s%d.mod"
        const int model = mrModelLoadRemap(lod_name, self->remap_name, 1);
        self->lod_model[i] = model;
        self->lod_copy[i] = mrModelCopyRemap(model, self->remap_name, 1);
    }
    self->xray_tex = gxGetTexture((const char*)0x004f5018, 0);                          // "xray.tex"
    self->spin[0] = mrModelLoad((const char*)0x004f5024);                               // "spin_l.mod"
    self->spin[1] = mrModelLoad((const char*)0x004f5030);                               // "spin_r.mod"
    self->arm[0] = mrModelLoad((const char*)0x004f503c);                                // "arm_ul.mod"
    self->arm[1] = mrModelLoad((const char*)0x004f5048);                                // "arm_ur.mod"
    self->arm[2] = mrModelLoad((const char*)0x004f5054);                                // "arm_ll.mod"
    self->arm[3] = mrModelLoad((const char*)0x004f5060);                                // "arm_lr.mod"
    self->arm[4] = mrModelLoad((const char*)0x004f506c);                                // "arm_sl.mod"
    self->arm[5] = mrModelLoad((const char*)0x004f5078);                                // "arm_sr.mod"
    self->arm_copy[0] = mrModelCopy(self->arm[4]);
    self->arm_copy[1] = mrModelCopy(self->arm[5]);
}
static void fp_load_car_models(Footprint& f, CarObject*, Edx, const char*, int, uint8_t) { f.replay_only = "loads models and textures"; }
PORT_FN(0x0046c300, "CarObject::load_car_models", CarObject_load_car_models, fp_load_car_models)

// ==== CarObject::InitTextures (0x46c9d0): the damage texture: "<car>d.tex" (or damage.tex) alpha-blitted over a
// copy of the skin (texture slot index + 0x20), with the damage option
static void __fastcall CarObject_InitTextures(CarObject* self, Edx) {
    char name[0x40];
    const uint8_t* info = CarMgrGetInfo(self->index);
    game_sprintf(name, (const char*)0x004f5084, info + 0x12);                            // "%sd.tex"
    if (!ResourceExists(name)) game_sprintf(name, (const char*)0x004f508c);              // "damage.tex"
    if (WorldGameOptions()[0x24]) {
        const int tex = gxGetTexture(self->skin, self->index + 0x20);
        self->damage_tex = tex;
        gxAlphaBlitTexture(name, tex);
    }
}
static void fp_init_textures(Footprint& f, CarObject*, Edx) { f.replay_only = "loads and blits textures"; }
PORT_FN(0x0046c9d0, "CarObject::InitTextures", CarObject_InitTextures, fp_init_textures)

// ==== CarObject::load_wheels (0x46ca60): the tyre radii (rim in x 0.0127 + width mm x aspect % x 1e-5), and each
// of the three wheel models (wheel_%d / fwheel_%d.mod, 1..3) copied scaled to the front and the rear tyre: width
// (m) / its x extent, diameter / its y and z extents
static void __fastcall CarObject_load_wheels(CarObject* self, Edx, const uint8_t* cd) {
    auto cf = [cd](uint32_t off) { return *(const float*)(cd + off); };
    self->radius_front = (float)(D(cf(0x138)) * FB(0x3c5013a9));
    self->radius_rear = (float)(D(cf(0x144)) * FB(0x3c5013a9));
    self->radius_front = (float)((D(cf(0x134)) * cf(0x130)) * FB(0x3727c5ad) + self->radius_front);
    self->radius_rear = (float)((D(cf(0x140)) * cf(0x13c)) * FB(0x3727c5ad) + self->radius_rear);
    const float width_f = (float)(D(cf(0x130)) * FB(0x3a83126f));
    const float diam_f = (float)(D(self->radius_front) * 2.0f);
    const float width_r = (float)(D(cf(0x13c)) * FB(0x3a83126f));
    const float diam_r = (float)(D(self->radius_rear) * 2.0f);
    char name[0x40];
    int k = 0;
    do {
        k++;
        game_sprintf(name, (const char*)0x004f5098, k);                                 // "wheel_%d.mod"
        const int wheel = mrModelLoad(name);
        game_sprintf(name, (const char*)0x004f50a8, k);                                 // "fwheel_%d.mod"
        const int fwheel = mrModelLoad(name);
        float e[6];
        mrModelGetExtents(wheel, &e[0], &e[1], &e[2], &e[3], &e[4], &e[5]);
        float scale[9];
        scale[1] = 0.0f; scale[2] = 0.0f; scale[3] = 0.0f; scale[5] = 0.0f; scale[6] = 0.0f; scale[7] = 0.0f;
        const float sx = (float)(D(e[1]) - e[0]);
        const float sy = (float)(D(e[3]) - e[2]);
        const float sz = (float)(D(e[5]) - e[4]);
        scale[0] = (float)(D(width_f) / sx);
        scale[4] = (float)(D(diam_f) / sy);
        scale[8] = (float)(D(diam_f) / sz);
        WheelModels& wm = self->wheel_models[k - 1];
        wm.wheel_front = mrModelCopyTransform(wheel, scale);
        wm.fwheel_front = mrModelCopyTransform(fwheel, scale);
        scale[0] = (float)(D(width_r) / sx);
        scale[7] = 0.0f; scale[6] = 0.0f; scale[5] = 0.0f; scale[3] = 0.0f; scale[2] = 0.0f; scale[1] = 0.0f;
        scale[4] = (float)(D(diam_r) / sy);
        scale[8] = (float)(D(diam_r) / sz);
        wm.wheel_rear = mrModelCopyTransform(wheel, scale);
        wm.fwheel_rear = mrModelCopyTransform(fwheel, scale);
        mrModelUnload(wheel);
        mrModelUnload(fwheel);
    } while (k < 3);
}
static void fp_load_wheels(Footprint& f, CarObject*, Edx, const uint8_t*) { f.replay_only = "loads and copies models"; }
PORT_FN(0x0046ca60, "CarObject::load_wheels", CarObject_load_wheels, fp_load_wheels)

// ==== CarObject::CarObject (0x469e00) ==============================================================================
// PhobData (the car's CarData: +0 'RACA' AI / 'RACP' player, +0x188, +0x194 the index, the tyre sizes at +0x130..
// +0x144, and +0x1b8 / +0x1cc where the first five LOD models and copies are handed back) and the car list's entry
// (+0 registered as, +4 driver, +0x11 the car's name, +0xc0 the paint)
static void* __fastcall CarObject_ctor(CarObject* self, Edx, uint8_t* pd, const uint8_t* cle) {
    WorldObject_ctor(self, 0);
    for (int i = 0; i < 4; i++) WheelMessage_ctor_o(self->msg + 0x78 + 0x38 * i, 0);
    for (int i = 0; i < 16; i++) DustParticle_ctor_o(&self->dust[i], 0);
    for (int i = 0; i < 4; i++) Frame_ctor_o(&self->wheel_frame[i], 0);
    for (int i = 0; i < 4; i++) Frame_ctor_o(&self->wheel_frame2[i], 0);
    for (int i = 0; i < 4; i++) P3DBase_ctor(&self->wheel_move[i], 0);
    for (int i = 0; i < 4; i++) SkidClient_ctor_o(&self->skid[i], 0);
    self->vtable = P<void*>(0x004dd4d0);
    self->index = *(const int32_t*)(pd + 0x194);
    CarMgrRegisterCar(*(const int32_t*)(pd + 0x194), self->msg, (const char*)cle + 4, (const char*)cle + 0x11,
                      *(const int32_t*)cle);
    for (int i = 0; i < 4; i++) self->wheel_move[i] = P3{0.0f, 0.0f, 0.0f};
    memcpy(&opacity(self), "\0\0\x80\x3f", 4);                                           // 1.0
    self->effects_tex = gxGetTexture((const char*)0x004f4f1c, 0);                       // "effects.tex"
    self->lod_p = self->lod;
    self->num_lods_p = &self->num_lods;
    self->lod_models_p = self->lod_copy;
    uint8_t high = 0;
    if (*(const uint32_t*)pd == 0x41434152) {                                           // 'RACA'
        if (*(const int32_t*)(WorldGameOptions() + 0x20) >= 4) high = 1;
    } else if (*(const uint32_t*)pd == 0x50434152) {                                    // 'RACP'
        if (*(const int32_t*)(WorldGameOptions() + 0x20) >= 3 && *(const uint32_t*)(pd + 0x188) > 0xbf19999au) high = 1;
    }
    load_car_models_o(self, 0, (const char*)cle + 0x11, *(const int32_t*)(cle + 0xc0), high);
    load_wheels_o(self, 0, pd);
    P<CarObject*>(S_CAROBJS)[self->index] = self;
    self->stamp = gxGetStamp((const char*)0x004f4f28);                                   // "thought.stp"
    if (WorldShowSmoke()) {
        const uint32_t n = ((uint32_t)WorldShowSmoke() & 0x7ffffff) << 4;
        self->num_smoke = (int32_t)n;
        if (n > 0x20) self->num_smoke = 0x20;
        for (int i = 0; self->num_smoke > i; i++) {
            void* p = MemAlloc(0x58);
            self->smoke[i] = p ? SmokeObject_ctor(p, 0) : 0;
            WorldAddGob(self->smoke[i]);
        }
    }
    if (WorldShowSkids()) {
        int k = 4;
        if (WorldShowSkids() <= 1) k = 1;
        int len = k << 4;
        if (WorldGetPlayerCar() == self->index) len = k * 40;
        void* p = MemAlloc(0x14);
        void* skids = 0;
        if (p) skids = SkidObject_ctor(p, 0, len);
        WorldAddGob(skids);
        for (int i = 0; i < 4; i++) *(void**)(self->skid[i].raw + 0x1c) = skids;
    }
    if (WorldShowShadow()) {
        void* p = MemAlloc(0x1c);
        void* o = 0;
        if (p) o = ShadowObject_ctor(p, 0, (const char*)cle + 0x11, self);
        WorldAddGob(o);
    }
    if (WorldShowReflection()) {
        void* p = MemAlloc(0x1c);
        void* o = 0;
        if (p) o = ReflectionObject_ctor(p, 0, (const char*)cle + 0x11, self);
        WorldAddGob(o);
    }
    for (int i = 0; i < 5; i++) {
        *(int32_t*)(pd + 0x1b8 + 4 * i) = self->lod_model[i];
        *(int32_t*)(pd + 0x1cc + 4 * i) = self->lod_copy[i];
    }
    self->_504 = 0;
    self->_508 = 0;
    for (int i = 0; i < 16; i++) {
        memcpy(&self->dust[i].life, "\x00\x24\x74\x49", 4);                              // 1e6
        self->dust[i].vel = P3{0.0f, 0.0f, 0.0f};
        self->dust[i].pos = P3{0.0f, 0.0f, 0.0f};
    }
    WorldObject_Create(self, 0, pd);
    return self;
}
static void fp_car_ctor(Footprint& f, CarObject*, Edx, uint8_t*, const uint8_t*) { f.replay_only = "allocates, loads models and textures"; }
PORT_FN(0x00469e00, "CarObject::CarObject", CarObject_ctor, fp_car_ctor)

// ==== GhostCarObject::GhostCarObject (0x46cd20)
static void* __fastcall GhostCarObject_ctor(CarObject* self, Edx, uint8_t* pd, const uint8_t* cle) {
    CarObject_ctor_o(self, 0, pd, cle);
    self->vtable = P<void*>(0x004dd500);
    return self;
}
PORT_FN(0x0046cd20, "GhostCarObject::GhostCarObject", GhostCarObject_ctor, fp_car_ctor)

// ==== CarObject::~CarObject (0x46a170): the stamp, the textures (the damage one with the option), the LOD models
// and copies, the shadow, the cockpit's four, the rest, the arm copies and the wheel copies; out of the table; the
// skid clients' (empty) destructors, GraphObject's
static void __fastcall CarObject_dtor(CarObject* self, Edx) {
    void* stamp = self->stamp;
    self->vtable = P<void*>(0x004dd4d0);
    gxForgetStamp(stamp);
    if (WorldGameOptions()[0x24]) gxForgetTexture(self->damage_tex);
    gxForgetTexture(self->xray_tex);
    gxForgetTexture(self->effects_tex);
    gxForgetTexture(self->src_tex);
    for (int i = 0; self->num_lods > i; i++) {
        mrModelDestroyCopy(self->lod_copy[i]);
        mrModelUnload(self->lod_model[i]);
    }
    if (self->shadow_model) mrModelUnload(self->shadow_model);
    if (self->has_cockpit) {
        mrModelUnload(self->cockpit_c);
        mrModelUnload(self->cockpit_w);
        mrModelUnload(self->needle[0]);
        mrModelUnload(self->needle[1]);
    }
    mrModelUnload(self->brakelight);
    mrModelUnload(self->diskglow);
    mrModelUnload(self->spin[0]);
    mrModelUnload(self->spin[1]);
    for (int i = 0; i < 6; i++) mrModelUnload(self->arm[i]);
    mrModelDestroyCopy(self->arm_copy[0]);
    mrModelDestroyCopy(self->arm_copy[1]);
    for (int k = 0; k < 3; k++) {
        mrModelDestroyCopy(self->wheel_models[k].wheel_front);
        mrModelDestroyCopy(self->wheel_models[k].fwheel_front);
        mrModelDestroyCopy(self->wheel_models[k].wheel_rear);
        mrModelDestroyCopy(self->wheel_models[k].fwheel_rear);
    }
    P<CarObject*>(S_CAROBJS)[self->index] = 0;
    for (int i = 3; i >= 0; i--) SkidClient_dtor(&self->skid[i], 0);
    GraphObject_dtor(self, 0);
}
static void fp_car_dtor(Footprint& f, CarObject*, Edx) { f.replay_only = "frees models and textures"; }
PORT_FN(0x0046a170, "CarObject::~CarObject", CarObject_dtor, fp_car_dtor)

// ==== GhostCarObject::~GhostCarObject (0x46cd40): its vtable, then CarObject's (a jump)
static void __fastcall GhostCarObject_dtor(CarObject* self, Edx) {
    self->vtable = P<void*>(0x004dd500);
    CarObject_dtor_o(self, 0);
}
PORT_FN(0x0046cd40, "GhostCarObject::~GhostCarObject", GhostCarObject_dtor, fp_car_dtor)
