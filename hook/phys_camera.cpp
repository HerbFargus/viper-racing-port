// phys_camera.cpp -- M3 stage 3.6 group T2: the camera, the dashboards' logic, the crash sounds and the
// quarter-car test tool, rewritten.
//
//   phystask.obj  update_camera (the chase / cockpit / bumper / rear / blimp / TV cameras, once a physics tick),
//                 is_chase, camera_look, blimp_to_tv, load_tv_cameras, TVCamera::TVCamera, CameraDashboardKey
//   physdash.obj  PhysicsDashboardKey
//   collide.obj   CollisionSound::Play (both overloads)
//   carpart.obj   QuarterCarTest (the suspension rig's dialog), QuarterCarControl's constructor, Create,
//                 Callback and graph_fn
//
// Not rewritten here: the drawing (CameraDashboardDraw, PhysicsDashboardDraw, draw_tire_dash,
// QuarterCarControl::Draw: the UI stage); the bare `ret` stubs CameraDashboardBegin / CameraDashboardEnd /
// PhysicsDashboardBegin / PhysicsDashboardEnd; the $E initialisers and the deleting destructor. Everything else
// they call -- the matrix helpers, PhysTaskFindCar, TerrainGetHeight, the UI -- is called by its v1.0 address, and
// so are this file's own functions where one calls another (a hooked rewrite is what runs).
//
// Threads. update_camera runs on the physics thread: PhysTaskUpdate calls it after the ticks, with the frame it
// hands the renderer (shared memory). Its state is phystask.obj statics (listed below), all saved with the physics
// globals except blimp_frame's last 44 bytes, which the main thread's move_blimp shares (so the generator left
// them out); the footprint lists those, the output frame and the sound manager's listener (SoundSetListener copies
// the camera into it). camera_look runs only inside it; load_tv_cameras in PhysTaskBegin (physics_thread).
// The main thread runs blimp_to_tv (move_blimp, the free-flying blimp: its "copy to the TV camera" key),
// PhysicsDashboardKey, and the quarter-car rig (a developer dialog nothing calls). CollisionSound::Play runs on
// the physics thread (Collide, and the replay's crash handler).
//
// The camera's statics (v1.0; from the disassembly of update_camera and its helpers):
//   0x520ba8 P3     the cockpit's smoothed "up" (ground normal blended with the car's up; init (0,1,0), guard 2)
//   0x520bb8 Frame  the chase camera's frame, kept between ticks (reset 40 m behind, 10 m up on a switch/teleport)
//   0x520bec float  cos(30 deg) (guard 4): a slope steeper than 30 degrees doesn't tilt the cockpit
//   0x520bf8 P3     the focus point last tick (init 0, guard 1): a 2 m jump at under 20 m/s is a teleport
//   0x520c18 Car*[] PhysTaskFindCar's table
//   0x520c70 Frame  THE camera (what the renderer and the sound listener get)
//   0x520cac byte   the function-static guards (1, 2, 4)
//   0x520cb8 TVCamera[32]  camera.tab (type: 0 fixed, 1 pan, 2 pan_zoom, 3 chase; position; three parameters)
//   0x521050 Frame  blimp_frame (the TV camera's frame, or the blimp's while it flies)
//   0x521084 int    the number of TV cameras;  0x52108c the camera type last tick;  0x5214e8 P3 the listener's
//                   velocity (the focus car's, or 0);  0x4ec9dc int the TV camera in use
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

static __forceinline uint32_t Ub(const void* p) { uint32_t u; memcpy(&u, p, 4); return u; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }      // an integer move of a float
static __forceinline void put4(void* d, uint32_t v) { memcpy(d, &v, 4); }
template <typename T> static __forceinline T* P(uint32_t addr) { return (T*)(uintptr_t)addr; }
static __forceinline float& Fat(void* p, uint32_t off) { return *(float*)((uint8_t*)p + off); }
static __forceinline int32_t& Iat(void* p, uint32_t off) { return *(int32_t*)((uint8_t*)p + off); }

// fld dword; fchs; fstp dword -- through the FPU (a signalling NaN comes out quiet), as the original does
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

// ---- layouts and statics ----------------------------------------------------------------------------------
namespace {
// TVCamera (28 bytes; the constructor does nothing): camera.tab's rows
struct TVCamera {
    int32_t type;                      // +0x00  0 fixed, 1 pan, 2 pan_zoom, 3 chase
    P3 pos;                            // +0x04
    float a, b, c;                     // +0x10  fixed: the rotation vector in degrees; pan: the dolly along the
                                       //        view (a); pan_zoom: a x (distance - b)
};
static_assert(sizeof(TVCamera) == 28, "TVCamera");
// SoundSetListener's argument
struct Listener { int32_t focus; const Frame* frame; const P3* velocity; };
// UIDoDialog's dialog, and UIDialogItem (the constructor 0x4091c0 stores its 14 arguments)
struct UIDialogItem { uint32_t a[14]; };
struct UIDialog { uint32_t title, subtitle; int32_t _08; UIDialogItem* items; uint32_t idle; };
static_assert(sizeof(UIDialogItem) == 56 && sizeof(UIDialog) == 20, "UIDialog");
}  // namespace

enum : uint32_t {
    S_SMOOTH_UP = 0x00520ba8,            // P3
    S_CHASE = 0x00520bb8,             // Frame
    S_COS30 = 0x00520bec,             // float
    S_LAST_POS = 0x00520bf8,          // P3
    S_CAM = 0x00520c70,               // Frame
    S_GUARD = 0x00520cac,             // uint8_t
    S_TV = 0x00520cb8,                // TVCamera[32]
    S_BLIMP = 0x00521050,             // Frame blimp_frame
    S_NUM_TV = 0x00521084,            // int32_t
    S_LAST_TYPE = 0x0052108c,         // int32_t
    S_CAM_VEL = 0x005214e8,           // P3
    S_CUR_TV = 0x004ec9dc,            // int32_t
    S_CAMERA_TYPE = 0x00521600,       // volatile CameraType camera_type (physics.obj; the main thread sets it)
    S_PAUSED = 0x00521608,            // volatile bool physics_paused
    S_FOCUS = 0x00521614,             // volatile int camera_focus
    S_SOUND_MANAGER = 0x004f5708,     // SoundManager* (SoundSetListener's)
    S_COLLISIONS = 0x004ecce4,        // bool PhysicsCollisions
    S_WALLS = 0x004ecce8,             // bool PhysicsWalls
    S_ITEM_END = 0x00578d08,          // the UI's end-of-items item (14 dwords)
    S_SCREEN_WID = 0x005228f4, S_SCREEN_HIT = 0x005228d4,
};

// the Car's fields the camera reads (car.obj; Car is 3768 bytes)
static __forceinline Frame* car_frame(uint8_t* c) { return (Frame*)(c + 0x38); }
static __forceinline P3* car_velocity(uint8_t* c) { return (P3*)(c + 0x234); }
static __forceinline P3* car_focus_offset(uint8_t* c) { return (P3*)(c + 0x28); }    // PhobRoot +0x28
static __forceinline P3* car_cockpit_eye(uint8_t* c) { return (P3*)(c + 0x47c); }
static __forceinline uint8_t car_look_back(uint8_t* c) { return c[0x4ec]; }
static __forceinline float car_long_g(uint8_t* c) { return Fat(c, 0xea8); }

// ---- the functions they call, by address -------------------------------------------------------------------
typedef void*(__cdecl* FindCar_t)(int);
static const FindCar_t PhysTaskFindCar = (FindCar_t)0x00426d40;
typedef int(__cdecl* Atexit_t)(uint32_t);
static const Atexit_t game_atexit = (Atexit_t)0x004ceff0;
typedef void(__cdecl* MulPoint_t)(P3*, const P3*, const void*);
static const MulPoint_t MatrixMulPoint = (MulPoint_t)0x00429420;
typedef void(__cdecl* VecSub_t)(P3*, const P3*, const P3*);
static const VecSub_t VectorSub = (VecSub_t)0x00429120;
typedef double(__cdecl* VecLen_t)(const P3*);          // returns ST0 (the fsqrt, unstored)
static const VecLen_t VectorLength = (VecLen_t)0x00429100;
typedef void(__cdecl* MatVoid_t)(void*);
static const MatVoid_t MatrixNormalize = (MatVoid_t)0x00429180;
static const MatVoid_t MatrixMakeIdentity = (MatVoid_t)0x00429150;
typedef void(__cdecl* ModelRot_t)(M3*, uint32_t, uint32_t, uint32_t);   // the angles pushed as bits
static const ModelRot_t MatrixMakeModelRotation = (ModelRot_t)0x00429350;
typedef void(__cdecl* Concat_t)(void*, const void*, const void*);
static const Concat_t MatrixConcat = (Concat_t)0x00403040;
typedef uint8_t(__cdecl* TerrainHeight_t)(uint32_t, uint32_t, P3*, P3*, int32_t*);   // x, z as bits
static const TerrainHeight_t TerrainGetHeight = (TerrainHeight_t)0x00465c40;
typedef void(__cdecl* SetListener_t)(const Listener*);
static const SetListener_t SoundSetListener = (SetListener_t)0x00471e00;
typedef void(__cdecl* Panic_t)(const char*, ...);
static const Panic_t LogPanic = (Panic_t)0x004112b0;
typedef void(__cdecl* Assert_t)(int, const char*, ...);
static const Assert_t ASSERT_MSG = (Assert_t)0x00426d70;             // a bare ret: kept, as the original calls it
typedef uint8_t(__cdecl* GetByte_t)();
static const GetByte_t PhysReplayPlayMode = (GetByte_t)0x0042d340;
typedef void(__cdecl* AddEvent_t)(int, const void*, int);
static const AddEvent_t PhysReplayAddEvent = (AddEvent_t)0x0042d8b0;   // (an output: captured by the shadow check)
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
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
static const Stricmp_t game_stricmp = (Stricmp_t)0x004da350;
typedef double(__cdecl* Atof_t)(const char*);                       // fld qword: a double in ST0
static const Atof_t game_atof = (Atof_t)0x004cfe40;
typedef UIDialogItem*(__fastcall* ItemCtor_t)(UIDialogItem*, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                              uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                              uint32_t, uint32_t);
static const ItemCtor_t UIDialogItem_ctor = (ItemCtor_t)0x004091c0;
typedef int(__cdecl* UIDoDialog_t)(const UIDialog*, int, int, int, int, int);
static const UIDoDialog_t UIDoDialog = (UIDoDialog_t)0x00478ff0;
typedef void(__fastcall* AddNotif_t)(void*, Edx, int, const void*, unsigned);
static const AddNotif_t UICustomControl_AddNotification = (AddNotif_t)0x0047e840;
typedef void(__fastcall* ThisVoid_t)(void*, Edx);
static const ThisVoid_t UICustomControl_Dirty = (ThisVoid_t)0x0047e820;
typedef void(__fastcall* DamperSetup_t)(void*, Edx, uint32_t, uint32_t, uint32_t, uint32_t);   // floats as bits
static const DamperSetup_t Damper_Setup = (DamperSetup_t)0x00446a90;
typedef void(__fastcall* QcStep_t)(void*, Edx, uint32_t, uint32_t);                            // (dt, height) as bits
static const QcStep_t QuarterCar_Step = (QcStep_t)0x00447aa0;
typedef float(__fastcall* TerrainGet_t)(void*, Edx);
typedef void(__fastcall* TerrainStep_t)(void*, Edx, uint32_t, uint32_t);                       // (dt, speed) as bits

// this file's own functions, by address (so a hooked rewrite is what runs)
typedef uint8_t(__cdecl* IsChase_t)(int);
static const IsChase_t is_chase_o = (IsChase_t)0x00428750;
typedef void(__cdecl* Look_t)(const P3*, const P3*);
static const Look_t camera_look_o = (Look_t)0x00428790;
typedef void(__fastcall* PlayF_t)(void*, Edx, const P3*, uint32_t);   // the float argument moved as bits
static const PlayF_t CollisionSound_PlayF_o = (PlayF_t)0x0043c510;
typedef uint8_t*(__fastcall* QccCtor_t)(uint8_t*, Edx);
static const QccCtor_t QuarterCarControl_ctor_o = (QccCtor_t)0x00447320;

// ==== is_chase (0x428750) ===================================================================================
// the chase cameras: 4 near, 5 normal, 6 far (7, the chase camera looking back, isn't one: its switch doesn't
// reset the chase frame)
static uint8_t __cdecl is_chase(int type) { return type == 4 || type == 6 || type == 5 ? 1 : 0; }
static void fp_is_chase(Footprint& f, int) { f.pure = true; }
PORT_FN(0x00428750, "is_chase", is_chase, fp_is_chase)

// ==== camera_look (0x428790) ================================================================================
// Puts the camera at `eye`, looking at `target` with y up: forward = target - eye, right = (fz, 0, -fx),
// up = forward x right, then MatrixNormalize. Leaves the rotation alone closer than 1 m, or looking straight up or
// down (|fx| and |fz| under 0.01). Two (release-build no-op) asserts that neither point is 10 km out.
static void __cdecl camera_look(const P3* eye, const P3* target) {
    {
        const char* mode = PhysReplayPlayMode() ? (const char*)0x004ecb70 : (const char*)0x004ecb78;   // "replay" / "normal"
        const double m = (D(eye->x) * eye->x + D(eye->y) * eye->y) + D(eye->z) * eye->z;
        ASSERT_MSG(!(m >= 1e8f) ? 1 : 0, (const char*)0x004ecb80, mode, D(eye->x), D(eye->y), D(eye->z));
    }
    {
        const char* mode = PhysReplayPlayMode() ? (const char*)0x004ecba8 : (const char*)0x004ecbb0;
        const double m = (D(target->x) * target->x + D(target->z) * target->z) + D(target->y) * target->y;
        ASSERT_MSG(!(m >= 1e8f) ? 1 : 0, (const char*)0x004ecbb8, mode, D(target->x), D(target->y), D(target->z));
    }
    Frame& cam = *P<Frame>(S_CAM);
    memcpy(&cam.pos, eye, 12);                          // (eye is the camera's own position, in update_camera)
    P3 d;
    d.x = (float)(D(target->x) - cam.pos.x);
    d.y = (float)(D(target->y) - cam.pos.y);
    d.z = (float)(D(target->z) - cam.pos.z);
    const float zz = (float)(D(d.z) * d.z);
    const double m = (D(d.y) * d.y + D(d.x) * d.x) + zz;
    if (!(m >= 1.0)) return;                            // test ah,1: closer than 1 m, or a NaN
    if (!(fabs(D(d.x)) >= (float)0.01f) && !(fabs(D(d.z)) >= (float)0.01f)) return;
    float* r = cam.rot.m;
    memcpy(&r[6], &d, 12);                              // forward
    cp4(&r[0], &d.z);                                   // right = (fz, 0, -fx)
    put4(&r[1], 0);                                     // (the up vector (0,1,0) is folded in; a dead alias check)
    r[2] = fpu_neg(&d.x);
    r[3] = (float)(D(d.y) * r[2] - D(d.z) * r[1]);      // up = forward x right
    r[4] = (float)(D(zz) - D(d.x) * r[2]);
    r[5] = (float)(D(d.x) * r[1] - D(d.y) * d.z);
    MatrixNormalize(&cam);
}
static void fp_camera_look(Footprint&, const P3*, const P3*) {}   // the camera: a physics static
PORT_FN(0x00428790, "camera_look", camera_look, fp_camera_look)

// ==== update_camera (0x427610) ===============================================================================
// The fixed TV camera's orientation: the rotation vector A (radians) of length `len`, as a rotation matrix
// (Rodrigues: c I + (1-c) a a^T + s [a]x). fsin and fcos leave full-mantissa results in the registers, so the
// whole sequence is the original's instructions (0x4283a9..0x4284a7); w: A.x, A.y, A.z, len, then three scratch
// floats ([esp+0x20], [esp+0x84], [esp+0x88] in the original), which it overwrites as the original does.
static void tv_axis_angle(float* w, float* r) {
    float one = 1.0f;
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[w]\n\t"
                     "mov edx, %[r]\n\t"
                     "fld dword ptr [eax+12]\n\t"
                     "fdivr %[one]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "fmul st, st(1)\n\t"
                     "fld dword ptr [eax+4]\n\t"
                     "fmul st, st(2)\n\t"
                     "fxch st(2)\n\t"
                     "fmul dword ptr [eax+8]\n\t"
                     "fld dword ptr [eax+12]\n\t"
                     "fsin\n\t"
                     "fld dword ptr [eax+12]\n\t"
                     "fcos\n\t"
                     "fld %[one]\n\t"
                     "fsub st, st(1)\n\t"
                     "fld st(4)\n\t"
                     "fmul st, st(5)\n\t"
                     "fmul st, st(1)\n\t"
                     "fadd st, st(2)\n\t"
                     "fstp dword ptr [edx]\n\t"
                     "fld st(0)\n\t"
                     "fmul st, st(6)\n\t"
                     "fmul st, st(5)\n\t"
                     "fstp dword ptr [eax+20]\n\t"
                     "fld st(2)\n\t"
                     "fmul st, st(4)\n\t"
                     "fst dword ptr [eax]\n\t"
                     "fadd dword ptr [eax+20]\n\t"
                     "fstp dword ptr [edx+4]\n\t"
                     "fld st(0)\n\t"
                     "fmul st, st(4)\n\t"
                     "fst dword ptr [eax+12]\n\t"
                     "fmul st, st(5)\n\t"
                     "fstp dword ptr [eax+24]\n\t"
                     "fld st(2)\n\t"
                     "fmul st, st(6)\n\t"
                     "fst dword ptr [eax+16]\n\t"
                     "fsubr dword ptr [eax+24]\n\t"
                     "fstp dword ptr [edx+8]\n\t"
                     "fld dword ptr [eax+20]\n\t"
                     "fsub dword ptr [eax]\n\t"
                     "fstp dword ptr [edx+12]\n\t"
                     "fld st(5)\n\t"
                     "fmul st, st(6)\n\t"
                     "fmul st, st(1)\n\t"
                     "fadd st, st(2)\n\t"
                     "fstp dword ptr [edx+16]\n\t"
                     "fxch st(5)\n\t"
                     "fmul dword ptr [eax+12]\n\t"
                     "fxch st(2)\n\t"
                     "fmul st, st(4)\n\t"
                     "fld st(2)\n\t"
                     "fadd st, st(1)\n\t"
                     "fstp dword ptr [edx+20]\n\t"
                     "fld dword ptr [eax+24]\n\t"
                     "fadd dword ptr [eax+16]\n\t"
                     "fstp dword ptr [edx+24]\n\t"
                     ".byte 0xdc, 0xea\n\t"
                     "fxch st(2)\n\t"
                     "fstp dword ptr [edx+28]\n\t"
                     "fxch st(2)\n\t"
                     "fmul st, st(0)\n\t"
                     "fmul st, st(4)\n\t"
                     "fadd st, st(2)\n\t"
                     "fstp dword ptr [edx+32]\n\t"
                     "fstp st(0)\n\t"
                     "fstp st(0)\n\t"
                     "fstp st(0)\n\t"
                     "fstp st(0)"
                     :
                     : [w] "m"(w), [r] "m"(r), [one] "m"(one)
                     : VP_X87_CLOBBERS, "eax", "edx", "cc", "memory");
#else
    __asm {
        mov   eax, w
        mov   edx, r
        fld   dword ptr [eax+12]
        fdivr one                         ; 1/len
        fld   dword ptr [eax]
        fmul  st, st(1)                   ; ax
        fld   dword ptr [eax+4]
        fmul  st, st(2)                   ; ay
        fxch  st(2)
        fmul  dword ptr [eax+8]           ; az
        fld   dword ptr [eax+12]
        fsin                              ; s
        fld   dword ptr [eax+12]
        fcos                              ; c
        fld   one
        fsub  st, st(1)                   ; t = 1 - c        st: t c s az ax ay
        fld   st(4)
        fmul  st, st(5)
        fmul  st, st(1)
        fadd  st, st(2)
        fstp  dword ptr [edx]             ; r0 = ax ax t + c
        fld   st(0)
        fmul  st, st(6)
        fmul  st, st(5)
        fstp  dword ptr [eax+20]          ; m1 = t ay ax
        fld   st(2)
        fmul  st, st(4)
        fst   dword ptr [eax]             ; s az
        fadd  dword ptr [eax+20]
        fstp  dword ptr [edx+4]           ; r1
        fld   st(0)
        fmul  st, st(4)
        fst   dword ptr [eax+12]          ; m2 = t az
        fmul  st, st(5)
        fstp  dword ptr [eax+24]          ; m3 = t az ax
        fld   st(2)
        fmul  st, st(6)
        fst   dword ptr [eax+16]          ; m4 = s ay
        fsubr dword ptr [eax+24]
        fstp  dword ptr [edx+8]           ; r2 = m3 - s ay
        fld   dword ptr [eax+20]
        fsub  dword ptr [eax]
        fstp  dword ptr [edx+12]          ; r3 = m1 - s az
        fld   st(5)
        fmul  st, st(6)
        fmul  st, st(1)
        fadd  st, st(2)
        fstp  dword ptr [edx+16]          ; r4 = ay ay t + c
        fxch  st(5)                       ; st: ay c s az ax t
        fmul  dword ptr [eax+12]          ; ay m2
        fxch  st(2)
        fmul  st, st(4)                   ; s ax
        fld   st(2)
        fadd  st, st(1)
        fstp  dword ptr [edx+20]          ; r5 = ay m2 + s ax
        fld   dword ptr [eax+24]
        fadd  dword ptr [eax+16]
        fstp  dword ptr [edx+24]          ; r6 = m3 + m4
        _emit 0xdc                        ; fsub st(2), st  (dc ea: st(2) = st(2) - st, as the original)
        _emit 0xea
        fxch  st(2)
        fstp  dword ptr [edx+28]          ; r7 = ay m2 - s ax
        fxch  st(2)
        fmul  st, st(0)
        fmul  st, st(4)
        fadd  st, st(2)
        fstp  dword ptr [edx+32]          ; r8 = az az t + c
        fstp  st(0)
        fstp  st(0)
        fstp  st(0)
        fstp  st(0)
    }
#endif
}

// the rotation's other rows from its forward row (r6..r8) with y up; the TV cameras' look-at (inlined twice in
// the original, from the memory copy of the camera)
static void look_rows(Frame& cam) {
    float* r = cam.rot.m;
    cp4(&r[0], &r[8]);
    put4(&r[1], 0);                                     // (a dead alias check of the up vector)
    r[2] = fpu_neg(&r[6]);
    r[3] = (float)(D(r[2]) * r[7] - D(r[1]) * r[8]);
    r[4] = (float)(D(r[8]) * r[0] - D(r[6]) * r[2]);
    r[5] = (float)(D(r[1]) * r[6] - D(r[7]) * r[0]);
    MatrixNormalize(&cam);
}
// the forward row toward `to`; straight up or down (|x| and |z| within 2^-23) it falls back to the car's own
static void forward_to(Frame& cam, const P3& to, const Frame& cf) {
    float* r = cam.rot.m;
    r[6] = (float)(D(to.x) - cam.pos.x);
    r[7] = (float)(D(to.y) - cam.pos.y);
    r[8] = (float)(D(to.z) - cam.pos.z);
    if (!(fabs(D(r[6])) > FB(0x34000000)) && !(fabs(D(r[8])) > FB(0x34000000))) {   // test ah,0x41 each
        cp4(&r[6], &cf.rot.m[6]);
        cp4(&r[8], &cf.rot.m[8]);
    }
}

// the camera for this tick, the focus car found (everything between the lookup and the listener)
static void camera_update(uint8_t* car) {
    volatile int32_t& type = *P<volatile int32_t>(S_CAMERA_TYPE);
    Frame& cam = *P<Frame>(S_CAM);
    Frame& chase = *P<Frame>(S_CHASE);
    P3& vel = *P<P3>(S_CAM_VEL);
    P3& last_pos = *P<P3>(S_LAST_POS);
    uint8_t& guard = *P<uint8_t>(S_GUARD);

    Frame cf;                                           // [esp+0x54] the car's frame; pos becomes the focus point
    memcpy(&cf, car_frame(car), sizeof cf);
    memcpy(&vel, car_velocity(car), 12);
    P3 t;
    MatrixMulPoint(&t, car_focus_offset(car), &cf);
    cf.pos.x = (float)(D(t.x) + cf.pos.x);
    cf.pos.y = (float)(D(t.y) + cf.pos.y);
    cf.pos.z = (float)(D(t.z) + cf.pos.z);
    // a teleport: the focus point jumped 2 m while the car moves under 20 m/s
    uint8_t reset = 0;
    P3 d;
    VectorSub(&d, &cf.pos, &last_pos);
    if ((D(d.z) * d.z + D(d.y) * d.y) + D(d.x) * d.x > 4.0f &&                  // test ah,0x41
        !((D(vel.y) * vel.y + D(vel.z) * vel.z) + D(vel.x) * vel.x >= 400.0f))   // test ah,1
        reset = 1;
    int32_t& last_type = *P<int32_t>(S_LAST_TYPE);
    if (type != last_type || reset) {
        last_type = type;
        if (is_chase_o(type)) {                         // start the chase camera 40 m behind, 10 m up
            memcpy(&chase, &cf, sizeof chase);
            chase.pos.x = (float)(D(cf.rot.m[6]) * -40.0f + cf.pos.x);
            chase.pos.y = (float)(D(cf.rot.m[7]) * -40.0f + chase.pos.y);
            chase.pos.z = (float)(D(cf.rot.m[8]) * -40.0f + chase.pos.z);
            chase.pos.y = (float)(D(chase.pos.y) + 10.0f);
        }
    }
    memcpy(&last_pos, &cf.pos, 12);
    const int32_t kind = type;
    if ((uint32_t)kind > 12) {
        LogPanic((const char*)0x004ecb50, (int)type);   // "Unrecognized camera type (%d)"
        return;
    }
    float* r = cam.rot.m;
    switch (kind) {
    case 0: {                                           // cockpit: the eye, tilted halfway toward the ground's normal
        memcpy(&cam, &cf, sizeof cam);
        P3 e;
        MatrixMulPoint(&e, car_cockpit_eye(car), &cam);
        cam.pos.x = (float)(D(e.x) + cam.pos.x);
        cam.pos.y = (float)(D(e.y) + cam.pos.y);
        cam.pos.z = (float)(D(e.z) + cam.pos.z);
        P3& S = *P<P3>(S_SMOOTH_UP);
        if (!(guard & 2)) {
            put4(&S.x, 0);
            put4(&S.z, 0);
            guard |= 2;
            put4(&S.y, 0x3f800000);
            game_atexit(0x00428770);                    // $E85
        }
        float& cos30 = *P<float>(S_COS30);
        if (!(guard & 4)) {
            const double a = __builtin_bit_cast(double, 0x3fe0c15236000000ull);   // 0.5235987715423107 (qword)
            guard |= 4;
            cos30 = x87_cos_f(a);
        }
        P3 hit, N;
        int32_t surface;
        if (!TerrainGetHeight(Ub(&cf.pos.x), Ub(&cf.pos.z), &hit, &N, &surface)) {
            memcpy(&N, &cf.rot.m[3], 12);               // off the track: the car's up
        } else {
            // the ground's normal counts from 0 (30 degrees from the car's up) to 1 (the same)
            const double dot = (D(N.z) * cf.rot.m[5] + D(N.y) * cf.rot.m[4]) + D(N.x) * cf.rot.m[3];
            double w = (dot - cos30) / (1.0f - D(cos30));
            if (0.0f >= w) w = 0.0f;                    // fcom st(1); test ah,1: a NaN is kept
            const float wf = (float)w;
            N.x = (float)(w * N.x);                     // (the register)
            N.y = (float)(D(wf) * N.y);
            N.z = (float)(D(wf) * N.z);
            const double u = 1.0f - D(wf);
            const float uf = (float)u;
            N.x = (float)(u * cf.rot.m[3] + N.x);
            N.y = (float)(D(uf) * cf.rot.m[4] + N.y);
            N.z = (float)(D(uf) * cf.rot.m[5] + N.z);
        }
        S.x = (float)(D(S.x) * FB(0x3f733333));         // 0.95
        S.y = (float)(D(S.y) * FB(0x3f733333));
        S.z = (float)(D(S.z) * FB(0x3f733333));
        S.x = (float)(D(N.x) * FB(0x3d4cccd0) + S.x);    // 0.050000012
        S.y = (float)(D(N.y) * FB(0x3d4cccd0) + S.y);
        S.z = (float)(D(N.z) * FB(0x3d4cccd0) + S.z);
        r[3] = (float)(D(S.x) * 0.5f + D(cf.rot.m[3]) * 0.5f);
        r[4] = (float)(D(S.y) * 0.5f + D(cf.rot.m[4]) * 0.5f);
        r[5] = (float)(D(S.z) * 0.5f + D(cf.rot.m[5]) * 0.5f);
        r[0] = (float)(D(r[8]) * r[4] - D(r[5]) * r[7]);   // right = up x forward
        r[1] = (float)(D(r[5]) * r[6] - D(r[3]) * r[8]);
        r[2] = (float)(D(r[7]) * r[3] - D(r[6]) * r[4]);
        r[6] = (float)(D(r[5]) * r[1] - D(r[2]) * r[4]);   // forward = up x right
        r[7] = (float)(D(r[2]) * r[3] - D(r[5]) * r[0]);
        r[8] = (float)(D(r[4]) * r[0] - D(r[1]) * r[3]);
        MatrixNormalize(&cam);
        const double k = D(car_long_g(car)) * FB(0xbba3d70a);   // -0.005: the head moves with the g
        cam.pos.x = (float)(D(cf.rot.m[6]) * k + cam.pos.x);
        cam.pos.y = (float)(D(cf.rot.m[7]) * k + cam.pos.y);
        cam.pos.z = (float)(k * cf.rot.m[8] + cam.pos.z);
        return;
    }
    case 1:                                             // bumper: (0, 0.5, -0.1) in the car
    case 2: {                                           // hood: (0, 0.8, -0.5)
        memcpy(&cam, &cf, sizeof cam);
        P3 off, o;
        put4(&off.x, 0);
        put4(&off.y, kind == 1 ? 0x3f000000u : 0x3f4ccccdu);
        put4(&off.z, kind == 1 ? 0xbdcccccdu : 0xbf000000u);
        MatrixMulPoint(&o, &off, &cam);
        cam.pos.x = (float)(D(o.x) + cam.pos.x);
        cam.pos.y = (float)(D(o.y) + cam.pos.y);
        cam.pos.z = (float)(D(o.z) + cam.pos.z);
        return;
    }
    case 3: {                                           // rear view: turned by pi, (0, 0.8, 0) in the car
        memcpy(&cam, &cf, sizeof cam);
        M3 m;
        MatrixMakeModelRotation(&m, 0x40490fdb, 0, 0);
        MatrixConcat(&cam, &m, &cam);
        P3 off, o;
        put4(&off.x, 0);
        put4(&off.y, 0x3f4ccccd);
        put4(&off.z, 0);
        MatrixMulPoint(&o, &off, &cam);
        cam.pos.x = (float)(D(o.x) + cam.pos.x);
        cam.pos.y = (float)(D(o.y) + cam.pos.y);
        cam.pos.z = (float)(D(o.z) + cam.pos.z);
        return;
    }
    case 4: case 5: case 6: case 7: {                   // chase: near, normal, far; 7 looks back
        P3 T;                                           // [esp+0x48] what the camera looks at
        memcpy(&T, &cf.pos, 12);
        {
            const int32_t ty = type;
            if (ty == 4) T.y = (float)(D(cf.pos.y) + 1.5f);
            else if (ty == 6) T.y = (float)(D(cf.pos.y) + FB(0x406ccccd));   // 3.7
            else T.y = (float)(D(cf.pos.y) + 1.5f);
        }
        memcpy(&cam, &chase, sizeof cam);
        camera_look_o(&cam.pos, &T);
        const double v2 = (D(vel.y) * vel.y + D(vel.z) * vel.z) + D(vel.x) * vel.x;
        const float v2f = (float)v2;
        P3 V;                                           // the direction of travel (the velocity itself when slow)
        bool slow;
        if (!(v2 >= 1.0f)) {                            // fcom 1.0; test ah,1
            memcpy(&V, &vel, 12);
            slow = true;
        } else {
            const double inv = 1.0f / x87_sqrt(D(v2f));
            V.x = (float)(D(vel.x) * inv);
            V.y = (float)(D(vel.y) * inv);
            V.z = (float)(inv * vel.z);
            slow = false;
        }
        if (!*P<volatile uint8_t>(S_PAUSED)) {
            // swing the camera sideways (0.144 x the sideways velocity); driving toward the camera, a full step
            bool done = false;
            if (!slow && !((D(V.z) * r[8] + D(V.y) * r[7]) + D(V.x) * r[6] >= 0.0f)) {
                const double sx = (D(V.y) * r[1] + D(V.z) * r[2]) + D(V.x) * r[0];
                float s = 1.0f;
                if (!(sx > 0.0f)) s = -1.0f;            // test ah,0x41
                VP_OPAQUE(s);                           // (fmul s, as the original: a NaN keeps its sign)
                cam.pos.x = (float)(D(s) * r[0] * FB(0xbe1374bc) + cam.pos.x);   // -0.144
                cam.pos.y = (float)(D(s) * r[1] * FB(0xbe1374bc) + cam.pos.y);
                cam.pos.z = (float)(D(s) * r[2] * FB(0xbe1374bc) + cam.pos.z);
                done = true;
            }
            if (!done) {
                const double sx = ((D(V.y) * r[1] + D(V.z) * r[2]) + D(V.x) * r[0]) * FB(0xbe1374bc);
                cam.pos.x = (float)(D(r[0]) * sx + cam.pos.x);
                cam.pos.y = (float)(D(r[1]) * sx + cam.pos.y);
                cam.pos.z = (float)(sx * r[2] + cam.pos.z);
            }
        }
        camera_look_o(&cam.pos, &T);
        float dist = -10.0f;                            // [esp+0x24]
        {
            const int32_t ty = type;
            if (ty == 4) dist = -6.0f;
            else if (ty == 6) dist = -18.0f;
        }
        cam.pos.x = (float)(D(dist) * r[6] + T.x);
        cam.pos.y = (float)(D(dist) * r[7] + T.y);
        cam.pos.z = (float)(D(dist) * r[8] + T.z);
        memcpy(&chase, &cam, sizeof chase);
        if (type == 7 || car_look_back(car)) {          // mirrored through the car, raised, looking at it
            cam.pos.x = (float)(D(cf.pos.x) * 2.0f - cam.pos.x);
            const bool far_cam = type == 6;
            cam.pos.y = (float)(D(cf.pos.y) * 2.0f - chase.pos.y);
            cam.pos.z = (float)(D(cf.pos.z) * 2.0f - chase.pos.z);
            cam.pos.y = (float)(D(cam.pos.y) + (far_cam ? 8.0f : 3.0f));
            camera_look_o(&cam.pos, &cf.pos);
        }
        return;
    }
    case 8: case 9: {                                   // blimp: straight down, 10 m (8) or 50 m (9) up
        MatrixMakeIdentity(&cam);
        put4(&cam.pos.x, 0);
        put4(&cam.pos.y, 0);
        put4(&cam.pos.z, 0);
        M3 m;
        MatrixMakeModelRotation(&m, 0, 0x3fc90fdb, 0);
        MatrixConcat(&cam, &m, &cam);
        memcpy(&cam.pos, &cf.pos, 12);
        cam.pos.y = (float)(D(cam.pos.y) + (type == 8 ? 10.0f : 50.0f));
        put4(&vel.y, 0);
        return;
    }
    case 11: {                                          // the blimp's own frame (the flying blimp, move_blimp)
        memcpy(&cam, P<Frame>(S_BLIMP), sizeof cam);
        memset(&vel, 0, 12);
        return;
    }
    default: break;                                     // 10, 12: TV
    }
    // TV: 10 the camera nearest the focus point (of 1..n-1), 12 camera 0
    TVCamera* tv = P<TVCamera>(S_TV);
    const int32_t* num = P<int32_t>(S_NUM_TV);
    int best = 1;
    if (*num <= best) {                                 // none: stay put, look at the car
        forward_to(cam, cf.pos, cf);
        look_rows(cam);
        memset(&vel, 0, 12);
        return;
    }
    float dmin = FB(0x4dbebc20);                        // 4e8
    if (type == 10) {
        for (int i = 1; i < *num; i++) {
            P3 dv;
            VectorSub(&dv, &tv[i].pos, &cf.pos);
            const double d2 = (D(dv.z) * dv.z + D(dv.y) * dv.y) + D(dv.x) * dv.x;
            const float d2f = (float)d2;
            if (!(d2 >= dmin)) {                        // fcom; test ah,1
                dmin = d2f;
                best = i;
            }
        }
    } else {
        best = 0;
    }
    if (best < 0) return;                               // (never)
    *P<int32_t>(S_CUR_TV) = best;
    TVCamera& c = tv[best];
    memcpy(&cam.pos, &c.pos, 12);
    P3 Q;                                               // [esp+0x10] a metre above the focus point
    memcpy(&Q, &cf.pos, 12);
    Q.y = (float)(D(cf.pos.y) + 1.0f);
    if (c.type != 0) {
        forward_to(cam, Q, cf);
        look_rows(cam);
    } else {                                            // fixed: its own rotation vector, in degrees
        float w[7];
        w[0] = (float)(D(c.a) * FB(0x3c8efa35));
        w[1] = (float)(D(c.b) * FB(0x3c8efa35));
        w[2] = (float)(D(c.c) * FB(0x3c8efa35));
        const double len = VectorLength((const P3*)w);
        if (!(len > FB(0x38d1b717))) {                  // 1e-4: test ah,0x41
            MatrixMakeIdentity(&cam);
        } else {
            w[3] = (float)len;
            tv_axis_angle(w, cam.rot.m);
        }
    }
    memset(&vel, 0, 12);
    const int32_t ct = c.type;
    double k = 0;                                       // (set for 1 and 2, the only users)
    if (ct == 2) {                                      // pan_zoom: a x (the distance along the view - b)
        const double dd = ((D(Q.y) - cam.pos.y) * r[7] + (D(Q.z) - cam.pos.z) * r[8]) + (D(Q.x) - cam.pos.x) * r[6];
        k = (dd - c.b) * c.a;
    } else if (ct == 1) {                               // pan: a along the view
        k = c.a;
    }
    if (ct == 2 || ct == 1) {
        cam.pos.x = (float)(D(r[6]) * k + cam.pos.x);
        cam.pos.y = (float)(D(r[7]) * k + cam.pos.y);
        cam.pos.z = (float)(k * r[8] + cam.pos.z);
    }
    Frame& blimp = *P<Frame>(S_BLIMP);                  // the blimp starts from the TV camera
    memcpy(&blimp, &cam, sizeof blimp);
    memcpy(&blimp.pos, &c.pos, 12);
}

// the camera for this tick into *out (the renderer's frame); the focus car's index back
static int __cdecl update_camera(Frame* out) {
    const int32_t focus = *P<volatile int32_t>(S_FOCUS);
    uint8_t* car = (uint8_t*)PhysTaskFindCar(focus);
    uint8_t& guard = *P<uint8_t>(S_GUARD);
    if (!(guard & 1)) {
        P3& last_pos = *P<P3>(S_LAST_POS);
        put4(&last_pos.x, 0);
        put4(&last_pos.y, 0);
        guard |= 1;
        put4(&last_pos.z, 0);
        game_atexit(0x00428780);                        // $E84
    }
    if (car) camera_update(car);
    Listener l = {focus, P<Frame>(S_CAM), P<P3>(S_CAM_VEL)};
    SoundSetListener(&l);
    memcpy(out, P<Frame>(S_CAM), sizeof *out);
    return focus;
}
static void fp_update_camera(Footprint& f, Frame* out) {
    f.add(out, sizeof *out, "the camera frame out");
    f.add(P<uint8_t>(S_BLIMP + 4), 44, "blimp_frame (shared with move_blimp)");
    uint8_t* mgr = *P<uint8_t*>(S_SOUND_MANAGER);
    if (mgr) f.add(mgr + 0x50, 0x44, "the sound manager's listener");
    const uint8_t guard = *P<uint8_t>(S_GUARD);
    if (!(guard & 1) || (!(guard & 2) && *P<volatile int32_t>(S_CAMERA_TYPE) == 0))
        f.replay_only = "the first call registers a static's destructor (atexit)";
}
PORT_FN(0x00427610, "update_camera", update_camera, fp_update_camera)

// ==== blimp_to_tv (0x4267b0) =================================================================================
// the blimp's position into the TV camera in use; a fixed one also takes its heading and pitch (degrees; atan2 of
// the forward row). fpatan's result isn't rounded: the multiply by 57.29578 happens in the same asm block.
static void atan2_deg(const float* y, const float* x, float* dst) {
    float k = FB(0x42652ee1);                           // 57.29578
#ifdef VP_GCC
    __asm__ volatile("mov eax, %[y]\n\t"
                     "mov ecx, %[x]\n\t"
                     "mov edx, %[dst]\n\t"
                     "fld dword ptr [eax]\n\t"
                     "fld dword ptr [ecx]\n\t"
                     "fpatan\n\t"
                     "fmul %[k]\n\t"
                     "fstp dword ptr [edx]"
                     :
                     : [y] "m"(y), [x] "m"(x), [dst] "m"(dst), [k] "m"(k)
                     : VP_X87_CLOBBERS, "eax", "ecx", "edx", "cc", "memory");
#else
    __asm {
        mov  eax, y
        mov  ecx, x
        mov  edx, dst
        fld  dword ptr [eax]
        fld  dword ptr [ecx]
        fpatan
        fmul k
        fstp dword ptr [edx]
    }
#endif
}
static void __cdecl blimp_to_tv() {
    const int32_t cur = *P<int32_t>(S_CUR_TV);
    if (cur >= *P<int32_t>(S_NUM_TV)) return;
    TVCamera& c = P<TVCamera>(S_TV)[cur];
    const Frame& b = *P<Frame>(S_BLIMP);
    memcpy(&c.pos, &b.pos, 12);
    if (c.type != 0) return;
    atan2_deg(&b.rot.m[8], &b.rot.m[6], &c.a);
    atan2_deg(&b.rot.m[7], &b.rot.m[4], &c.b);
}
static void fp_blimp_to_tv(Footprint& f) {             // (the main thread)
    const int32_t cur = *P<int32_t>(S_CUR_TV);
    if (cur < *P<int32_t>(S_NUM_TV)) f.add(&P<TVCamera>(S_TV)[cur], sizeof(TVCamera), "the TV camera in use");
}
PORT_FN(0x004267b0, "blimp_to_tv", blimp_to_tv, fp_blimp_to_tv)

// ==== load_tv_cameras (0x428db0) =============================================================================
// camera.tab: one row a camera -- type, x, y, z, a, b, c (no bound on the rows in the original: the table holds 32)
static void __cdecl load_tv_cameras() {
    int32_t* num = P<int32_t>(S_NUM_TV);
    if (!ResourceExists((const char*)0x004ecc50)) {    // "camera.tab"
        *num = 0;
        return;
    }
    void* st = StringTableGet((const char*)0x004ecc5c);
    int row = 0;
    *num = StringTableNumRows(st);
    // FIX: the table holds 32 cameras and the original loads every row: the 33rd overwrote the TimerConditioner
    // (0x521038), the next ones blimp_frame and then the count itself (0x521084, in the 35th), which the loop
    // re-reads; and every reader of the count indexed past the table. The rows past 32 are ignored, the count 32.
    if (VP_FIX && *num > 32) {
        logf("camera.tab has %d rows; the TV camera table holds 32: the rest are ignored", *num);
        *num = 32;
    }
    if (*num > row) {
        TVCamera* c = P<TVCamera>(S_TV);
        do {
            const char* s = StringTableGetEntry(st, row, 0);
            if (!game_stricmp(s, (const char*)0x004ecc68)) c->type = 0;          // "fixed"
            else if (!game_stricmp(s, (const char*)0x004ecc70)) c->type = 1;     // "pan"
            else if (!game_stricmp(s, (const char*)0x004ecc74)) c->type = 2;     // "pan_zoom"
            else if (!game_stricmp(s, (const char*)0x004ecc80)) c->type = 3;     // "chase"
            else LogPanic((const char*)0x004ecc88, s);                          // "Unknown camera type %s"
            c->pos.x = (float)game_atof(StringTableGetEntry(st, row, 1));
            c->pos.y = (float)game_atof(StringTableGetEntry(st, row, 2));
            c->pos.z = (float)game_atof(StringTableGetEntry(st, row, 3));
            c->a = (float)game_atof(StringTableGetEntry(st, row, 4));
            c->b = (float)game_atof(StringTableGetEntry(st, row, 5));
            c->c = (float)game_atof(StringTableGetEntry(st, row, 6));
            row++;
            c++;
        } while (row < *num);
    }
    StringTableForget(st);
}
static void fp_load_tv_cameras(Footprint& f) { f.replay_only = "loads camera.tab"; }
PORT_FN(0x00428db0, "load_tv_cameras", load_tv_cameras, fp_load_tv_cameras)

// ==== TVCamera::TVCamera (0x4290f0) and CameraDashboardKey (0x4290e0): nothing ================================
static TVCamera* __fastcall TVCamera_ctor(TVCamera* self, Edx) { return self; }
// (writes nothing; not marked pure only because the fuzzer compares the returned pointer across its two arenas)
static void fp_tv_ctor(Footprint&, TVCamera*, Edx) {}
PORT_FN(0x004290f0, "TVCamera::TVCamera", TVCamera_ctor, fp_tv_ctor)

static uint8_t __cdecl CameraDashboardKey(uint16_t) { return 0; }
static void fp_camera_key(Footprint& f, uint16_t) { f.pure = true; }
PORT_FN(0x004290e0, "CameraDashboardKey", CameraDashboardKey, fp_camera_key)

// ==== PhysicsDashboardKey (0x429ba0) =========================================================================
// 'c' toggles PhysicsCollisions, 'w' PhysicsWalls (the dashboard's display switches); 1 when the key was theirs
static uint8_t __cdecl PhysicsDashboardKey(uint16_t key) {
    const uint32_t k = key;
    if (k == 0x63) {
        uint8_t* b = P<uint8_t>(S_COLLISIONS);
        *b = *b < 1 ? 1 : 0;                            // cmp 1; sbb; neg
        return 1;
    }
    if (k == 0x77) {
        uint8_t* b = P<uint8_t>(S_WALLS);
        *b = *b < 1 ? 1 : 0;
        return 1;
    }
    return 0;
}
static void fp_physics_key(Footprint& f, uint16_t) {   // (the main thread)
    f.add(P<uint8_t>(S_COLLISIONS), 1, "PhysicsCollisions");
    f.add(P<uint8_t>(S_WALLS), 1, "PhysicsWalls");
}
PORT_FN(0x00429ba0, "PhysicsDashboardKey", PhysicsDashboardKey, fp_physics_key)

// ==== CollisionSound::Play (0x43c510, 0x43c560) ================================================================
// A CollisionSound (40 bytes; Collide's table of 8 at 0x521e30) owns a Sound3D (+0xc) that plays from its
// position (+0x1c). Sound3D: +8 the volume; +0x29 play, +0x2a / +0x2b cleared, +0x2c the volume changed.
// Play(position, loudness): a change of loudness marks the sound's volume dirty; then it plays
static void __fastcall CollisionSound_PlayF(uint8_t* self, Edx, const P3* pos, uint32_t loud) {
    uint8_t* snd = *(uint8_t**)(self + 0xc);
    if (!snd) return;
    memcpy(self + 0x1c, pos, 12);
    const float l = __builtin_bit_cast(float, loud);
    if (D(Fat(snd, 8)) < l || D(Fat(snd, 8)) > l) {   // fcomp; test ah,0x40: equal or a NaN keeps it
        snd[0x2c] = 1;
        put4(snd + 8, loud);
    }
    snd = *(uint8_t**)(self + 0xc);
    snd[0x29] = 1;
    snd[0x2b] = 0;
    snd[0x2a] = 0;
}
static void fp_play_f(Footprint& f, uint8_t* self, Edx, const P3*, uint32_t) {
    uint8_t* snd = *(uint8_t**)(self + 0xc);
    if (!snd) return;
    f.add(self + 0x1c, 12, "the crash sound's position");
    f.add(snd + 8, 4, "the Sound3D's volume");
    f.add(snd + 0x29, 4, "the Sound3D's flags");
}
PORT_FN(0x0043c510, "CollisionSound::Play(float)", CollisionSound_PlayF, fp_play_f)

// Play(position, velocity): loudness |v| / 10000 up to 1; the replay's crash event (0x4d2, position, loudness)
static void __fastcall CollisionSound_Play(uint8_t* self, Edx, const P3* pos, const P3* vel) {
    const double x = x87_sqrt((D(vel->x) * vel->x + D(vel->y) * vel->y) + D(vel->z) * vel->z) * FB(0x38d1b717);
    const float loud = (float)(1.0f > x ? x : 1.0f);    // fcom st(1); test ah,0x41: a NaN gives 1
    struct { int32_t tag; P3 pos; float loud; } ev;     // 20 bytes
    ev.tag = 0x4d2;
    memcpy(&ev.pos, pos, 12);
    cp4(&ev.loud, &loud);
    PhysReplayAddEvent(1, &ev, sizeof ev);
    if (*(void**)(self + 0xc)) CollisionSound_PlayF_o(self, 0, pos, Ub(&loud));
}
static void fp_play(Footprint& f, uint8_t* self, Edx, const P3* pos, const P3*) { fp_play_f(f, self, 0, pos, 0); }
PORT_FN(0x0043c560, "CollisionSound::Play", CollisionSound_Play, fp_play)

// ==== the quarter-car rig (carpart.obj) ==========================================================================
// QuarterCarControl (0x408c bytes, vtable 0x4dc860; a UICustomControl, 0x4db190, whose +0x14 is its widget):
//   +0x18 QuarterCar (phys_engine.cpp: body / wheel mass, spring / tyre rate, damper*, positions, velocities,
//         load error, grip, contact steps, steps), +0x4c its road (a TableTerrain as DefaultTerrain, 0x4dc850),
//   +0x4060 the speed (mph), +0x4064 / +0x4068 the damper's bump / rebound (lb s/in), +0x406c the best load
//   fluctuation so far, +0x4070 its damping, +0x4074 its contact fraction, +0x4078 the Damper, +0x4088 "update"
//   (a checkbox: redraw when a setting changes).

// QuarterCarControl::QuarterCarControl (0x447320): the rig at rest and a road of 30 summed sines. The inlined
// QuarterCar and TableTerrain constructors read the masses and rates before they're set (whatever is in the
// memory): the rest positions come from that, and graph_fn recomputes them.
static uint8_t* __fastcall QuarterCarControl_ctor(uint8_t* self, Edx) {
    put4(self, 0x004db190);                             // UICustomControl
    Iat(self, 0x14) = 0;
    Iat(self, 0x34) = 0;
    const double m = D(Fat(self, 0x18)) + Fat(self, 0x1c);
    Iat(self, 0x2c) = 0;
    Iat(self, 0x38) = 0;
    Iat(self, 0x30) = 0;
    const double wheel = (m / Fat(self, 0x24)) * FB(0xc11cf5c3);   // -9.81
    Iat(self, 0x48) = 0;
    Iat(self, 0x3c) = 0;
    Iat(self, 0x44) = 0;
    Iat(self, 0x40) = 0;
    put4(self + 0x4c, 0x004dc840);                      // Terrain
    Fat(self, 0x30) = (float)wheel;
    const double sag = (D(Fat(self, 0x18)) * FB(0x411cf5c3)) / Fat(self, 0x20);   // 9.81
    Iat(self, 0x58) = 0;
    Iat(self, 0x5c) = 0;
    put4(self + 0x54, 0x44800000);                      // 1024
    put4(self + 0x50, 0x3e800000);                      // 0.25
    Fat(self, 0x2c) = (float)(D(Fat(self, 0x30)) - sag);
    memset(self + 0x60, 0, 0x4000);
    put4(self + 0x4c, 0x004dc850);                      // DefaultTerrain
    float period = FB(0x409c0ebf), amp = FB(0x3a052b4d);
    float* table = (float*)(self + 0x60);
    for (int k = 30; k; k--) {
        for (int i = 0; i < 0x1000; i++) {
            const double x = ((D(i) / period) * Fat(self, 0x50)) * FB(0x40c90fdb);   // 2 pi
            table[i] = (float)(x87_sin_mul(x, amp) + table[i]);
        }
        period = (float)(D(period) * FB(0x3f8ccccd));   // 1.1
        amp = (float)(D(amp) * FB(0x3f8ccccd));
    }
    put4(self, 0x004dc860);                             // QuarterCarControl
    put4(self + 0x4064, 0x42480000);                    // 50
    put4(self + 0x4068, 0x42480000);
    put4(self + 0x4060, 0x43480000);                    // 200
    put4(self + 0x1c, 0x41b5cccd);                      // 22.725
    *(uint8_t**)(self + 0x28) = self + 0x4078;
    put4(self + 0x18, 0x4382ab33);                      // 261.34
    put4(self + 0x24, 0x48ab1736);                      // 350392
    put4(self + 0x20, 0x4708df5e);                      // 35039.4
    return self;
}
static void fp_qcc_ctor(Footprint& f, uint8_t* self, Edx) { f.add(self, 0x408c, "QuarterCarControl"); }
PORT_FN(0x00447320, "QuarterCarControl::QuarterCarControl", QuarterCarControl_ctor, fp_qcc_ctor)

// QuarterCarControl::Create (0x4475b0, virtual): the widget reports changes to the three settings and "update"
static void __fastcall QuarterCarControl_Create(uint8_t* self, Edx) {
    UICustomControl_AddNotification(self, 0, 0, self + 0x4064, 4);
    UICustomControl_AddNotification(self, 0, 0, self + 0x4068, 4);
    UICustomControl_AddNotification(self, 0, 0, self + 0x4060, 4);
    UICustomControl_AddNotification(self, 0, 0, self + 0x4088, 1);
    self[0x4088] = 1;
}
static void fp_qcc_create(Footprint& f, uint8_t*, Edx) { f.replay_only = "registers notifications with its UI widget"; }
PORT_FN(0x004475b0, "QuarterCarControl::Create", QuarterCarControl_Create, fp_qcc_create)

// QuarterCarControl::Callback (0x447600, virtual): a setting changed -- redraw, if "update" is on
static void __fastcall QuarterCarControl_Callback(uint8_t* self, Edx, int, const void*) {
    if (self[0x4088]) UICustomControl_Dirty(self, 0);
}
static void fp_qcc_callback(Footprint& f, uint8_t* self, Edx, int, const void*) {   // (the main thread)
    uint8_t* widget = *(uint8_t**)(self + 0x14);
    if (self[0x4088] && widget) f.add(widget + 0x220, 1, "the widget's dirty flag");
}
PORT_FN(0x00447600, "QuarterCarControl::Callback", QuarterCarControl_Callback, fp_qcc_callback)

// QuarterCarControl::graph_fn (0x4478b0, static): the load fluctuation at speed x mph (the graph's x): the damper
// from the settings (bump / rebound lb s/in -> N s/m; Damper::Setup(bump, bump, rebound, rebound)), the car at
// rest, 9999 steps of 0.25 ms over the road; the RMS tyre-load error over the weight, times the tyre rate. The
// lowest so far is kept with its damping (back in lb s/in) and the fraction of steps the tyre touched the road.
static float __cdecl QuarterCarControl_graph_fn(float x, uint8_t* q) {
    volatile float k_in = FB(0x3cd013a9), k_one = 1.0f; // 0.0254 / 1.0, divided at run time as the original does
    const double in = D(k_in) / k_one;
    const float bump = (float)((D(Fat(q, 0x4064)) / in) * FB(0x408e6666));   // 4.45
    const float rebound = (float)((D(Fat(q, 0x4068)) / in) * FB(0x408e6666));
    Damper_Setup(q + 0x4078, 0, Ub(&bump), Ub(&bump), Ub(&rebound), Ub(&rebound));
    Iat(q, 0x34) = 0;
    Iat(q, 0x2c) = 0;
    Iat(q, 0x38) = 0;
    Iat(q, 0x30) = 0;
    Fat(q, 0x30) = (float)(((D(Fat(q, 0x18)) + Fat(q, 0x1c)) / Fat(q, 0x24)) * FB(0xc11cf5c3));
    const double sag = (D(Fat(q, 0x18)) * FB(0x411cf5c3)) / Fat(q, 0x20);
    Fat(q, 0x2c) = (float)(D(Fat(q, 0x30)) - sag);
    Iat(q, 0x48) = 0;
    Iat(q, 0x3c) = 0;
    Iat(q, 0x44) = 0;
    Iat(q, 0x40) = 0;
    uint8_t* road = q + 0x4c;
    void** vt = *(void***)road;
    ((ThisVoid_t)vt[1])(road, 0);                       // Reset
    const float speed = (float)(D(x) * FB(0x3ee38e39)); // 0.44444445 (mph -> m/s, near enough)
    const TerrainGet_t get_height = (TerrainGet_t)vt[2];
    const TerrainStep_t step = (TerrainStep_t)vt[3];
    for (int n = 9999; n; n--) {
        const float h = (float)get_height(road, 0);
        QuarterCar_Step(q + 0x18, 0, 0x3983126f, Ub(&h));   // 0.00025 s
        step(road, 0, 0x3983126f, Ub(&speed));
    }
    const double steps = D(Iat(q, 0x48));
    const double rms = x87_sqrt(D(Fat(q, 0x3c)) / steps);
    const double weight = (D(Fat(q, 0x18)) + Fat(q, 0x1c)) * FB(0x411cf5c3);
    const float fluct = (float)((rms / weight) * Fat(q, 0x24));
    const float contact = (float)(D(Iat(q, 0x44)) / steps);
    if (Fat(q, 0x406c) > fluct) {                       // test ah,0x41
        cp4(q + 0x406c, &fluct);
        Fat(q, 0x4070) = (float)(D(bump) * FB(0x3bbb090c));   // 0.0057078656 (N s/m -> lb s/in)
        cp4(q + 0x4074, &contact);
    }
    return fluct;
}
static void fp_qcc_graph(Footprint& f, float, uint8_t* q) { f.add(q, 0x408c, "QuarterCarControl"); }
PORT_FN(0x004478b0, "QuarterCarControl::graph_fn", QuarterCarControl_graph_fn, fp_qcc_graph)

// QuarterCarTest (0x446b40): the rig's modal dialog (nothing calls it). The control lives in the dialog's own
// frame, laid out as the original's: the dialog, its 12 items and the end item, the control; it isn't destroyed.
static void __cdecl QuarterCarTest() {
    struct {
        UIDialog dlg;                                   // [esp+0]
        UIDialogItem items[13];                         // [esp+0x14]
        uint8_t qcc[0x408c];                            // [esp+0x2ec]
    } f;
    static_assert(sizeof f == 0x4378, "QuarterCarTest's frame");
    QuarterCarControl_ctor_o(f.qcc, 0);
    const uint32_t qcc = (uint32_t)(uintptr_t)f.qcc;
    const uint32_t speed = qcc + 0x4060, bump = qcc + 0x4064, rebound = qcc + 0x4068, update = qcc + 0x4088;
    const uint32_t k_empty = 0x004e4db8;                // ""
    UIDialogItem_ctor(&f.items[0], 0, 0x17, 0, 10, 10, 600, 350, k_empty, 0, qcc, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&f.items[1], 0, 6, 0, 0x3c, 0x190, 0, 0, 0x004ede70, 0, speed, 0x15, 0x3f800000, 0, 0, 0);
    UIDialogItem_ctor(&f.items[2], 0, 0x10, 0, 0x29, 0x191, 0, 0, k_empty, 0x51, speed, 0, 0, 0x44480000, 0, 0);
    UIDialogItem_ctor(&f.items[3], 0, 0x10, 0, 0x6b, 0x191, 0, 0, k_empty, 0x51, speed, 1, 0, 0x44480000, 0, 0);
    UIDialogItem_ctor(&f.items[4], 0, 6, 0, 0xa0, 0x190, 0, 0, 0x004ede78, 0, bump, 0x15, 0x3f800000, 0, 0, 0);
    UIDialogItem_ctor(&f.items[5], 0, 0x10, 0, 0x8d, 0x191, 0, 0, k_empty, 0x65, bump, 0, 0, 0x42c80000, 0, 0);
    UIDialogItem_ctor(&f.items[6], 0, 0x10, 0, 0xcf, 0x191, 0, 0, k_empty, 0x65, bump, 1, 0, 0x42c80000, 0, 0);
    UIDialogItem_ctor(&f.items[7], 0, 6, 0, 0x104, 0x190, 0, 0, 0x004ede80, 0, rebound, 0x15, 0x3f800000, 0, 0, 0);
    UIDialogItem_ctor(&f.items[8], 0, 0x10, 0, 0xf1, 0x191, 0, 0, k_empty, 0x65, rebound, 0, 0, 0x42c80000, 0, 0);
    UIDialogItem_ctor(&f.items[9], 0, 0x10, 0, 0x133, 0x191, 0, 0, k_empty, 0x65, rebound, 1, 0, 0x42c80000, 0, 0);
    UIDialogItem_ctor(&f.items[10], 0, 0xb, 0, 0x64, 0x1a4, 0, 0, 0x004ede88, 0, update, 7, 0, 0, 0, 0);   // "Update"
    UIDialogItem_ctor(&f.items[11], 0, 2, 0xffffffffu, 0x1ef, 0x183, 0, 0, 0x004ede90, 0, 0, 2, 0, 0, 0, 0);  // "RETURN"
    memcpy(&f.items[12], P<void>(S_ITEM_END), sizeof f.items[12]);
    f.dlg.title = 0x004ede98;                           // ""
    f.dlg.items = f.items;
    f.dlg.subtitle = 0;
    f.dlg._08 = -2;
    f.dlg.idle = 0;
    UIDoDialog(&f.dlg, *P<int32_t>(S_SCREEN_WID), *P<int32_t>(S_SCREEN_HIT), -999, -999, 1);
}
static void fp_quarter_car_test(Footprint& f) { f.replay_only = "a modal UI dialog"; }
PORT_FN(0x00446b40, "QuarterCarTest", QuarterCarTest, fp_quarter_car_test)
