// snd_mgr.cpp -- M3 sound stage S1, group A: the game's sound objects and their manager, rewritten (library `sound`).
//
//   sound.obj     SoundBegin / SoundRestart / SoundEnd, the C entry points (SoundMuteCars, SoundUnMuteCars,
//                 SoundFlushAsync, SoundSetListener, SoundStopCar, SoundClassString, SoundClick), the sound objects:
//                 SoundBase (the flags a sound's owner sets; UpdateStatus / UpdateSound, what the BG thread does with
//                 them), Sound (2D: a pan), Sound3D (a position and a velocity: distance attenuation ((200-d)/200)^2,
//                 pan from the listener's right vector, Doppler 339.97/(339.97 + closing speed)), SoundDash (a
//                 Sound heard only from its own car), their static factories (Create, Toss) and EngineSound::Create
//   soundmgr.obj  the SoundManager: the sound table (every sound, sorted by class into 8 per-class runs), the voice
//                 budget per mixer quality (6 / 16 / 32, and per class), the 3D priority sort, the listener (two
//                 buffers), Update on the BG thread, the create-on-the-BG-thread handshake, the destroy lists; the
//                 colour constants its $E initialisers store
//   soundres.obj  SoundResourceGet / SoundResourceForget ('SFX0' resources)
//
// Faithful (docs/PORTING.md): every call in the original's order with the original's arguments, game functions by their
// v1.0 address (this file's own too, so a hooked rewrite is what runs), virtual calls through the object's vtable (the
// mixer and its sounds are group C's: IMixer / ISound, SoftMixer / SoftSound in the game). Function pointers the
// original stores (update_f, the BGHook) are the ORIGINAL's addresses. Globals and object fields are read and written
// through volatile where the original touches them: the main thread and the BG thread share the manager, and some of
// its handshakes are ordered stores with no lock (SetListener, update's async create). Float arguments are passed as
// their bits (the original pushes them with integer moves). No fixes: every FIX CANDIDATE below is reproduced.
//
// Threads. The SoundManager registers SoundManager::Update as a BGHook (priority 1, after physics_thread at 0), so
// update() -- the sounds' UpdateStatus / UpdateSound, the sort, the voice budget, the destroy lists, the mixer's own
// Update (group C: WaveGrab / mix / WaveRelease into the DirectSound ring) -- runs on the BGTask timer thread, which IS
// the physics thread (x87 at single precision, overflow / divide-by-zero unmasked). The sound objects are made and
// destroyed by game code on either thread: SoundManager::Create / Destroy compare TaskGetID with the BG task's id
// (recorded by the first update) and, on another thread, hand the work to the BG thread and sleep until it's done.
// The physics-thread auto-save covers none of this library's memory: every footprint lists what it writes -- the
// manager (0x180), its sound table (0x600), the sounds, their mixer sounds (SoftSound, 0x40), the mixer, the statics.
//
// Replay-only (f.replay_only): what allocates or frees (the factories, the constructors that create a mixer sound,
// the destructors that destroy one), what waits for the BG thread (fg_mixer_begin / fg_mixer_end, fg_create_sound,
// Destroy from another thread, destroy_sounds' 50 ms sleep), what starts or stops the mixer (DirectSoundCreate /
// Release inside SoftMixer::Begin / End, the BGHook, MultiBegin / MultiEnd), resources, and the $E initialisers.
//
// Layouts (v1.0, from the disassembly):
//   Listener (the argument): +0 int car, +4 const float* frame (3x3 rotation, row-major, then the position at +0x24),
//     +8 const float* velocity
//   SoundBase (vtable 0x4dd6c8: 0 deleting dtor, 4 CanBeHeard, 8 update_sound, 0xc update_status):
//     +4 float frequency ratio (1.0), +8 float volume (1.0), +0xc ISound*, +0x10 int car (-100), +0x14 char name[12],
//     +0x20 u8 0 (the name's end), +0x24 int sound class, +0x28 u8 muted (the voice budget's), +0x29 u8 play request,
//     +0x2a u8 stop request (1), +0x2b u8 looped, +0x2c u8 volume changed (1), +0x2d u8 frequency changed (1)
//   Sound (0x38, vtable 0x4dd6e8): +0x30 float pan, +0x34 u8 pan changed (1)
//   SoundDash (0x3c, vtable 0x4dd6f8; a Sound): +0x38 u8 the listener is its car (never initialised: FIX CANDIDATE)
//   Sound3D (0x40, vtable 0x4dd6d8): +0x30 u8 in range (1), +0x34 float attenuated volume, +0x38 const float* velocity,
//     +0x3c const float* position
//   ISound (group C; SoftSound 0x40): 0 deleting dtor, 4 Ok, 8 Play, 0xc PlayLooped, 0x10 Stop, 0x14 GetStatus
//     (1 playing, 2 muted, 3 stopped), 0x18 Mute, 0x1c UnMute, 0x20 SetFrequency, 0x24 SetVolume, 0x28 Can3D,
//     0x2c SetPanning, 0x30 Set3D(velocity, position)
//   IMixer (group C): 4 GetCaps (-> {u8 software, int hw 3D, int hw}), 0xc Begin, 0x10 CreateSound(name, SoundLoc),
//     0x14 Update(Listener*), 0x18 End
//   SoundManager (0x180): +0 u8 ok (the mixer began), +1 u8 cars muted, +4 SoundTable all {SoundBase** list (0x600
//     bytes, one MemAlloc: 384 sounds), int count}, +0xc SoundTable per class [8] (each a run of `all`), +0x4c u8 flip
//     the listener buffers, +0x50 int current listener buffer (-1: none), +0x54 listener_info[2] (0x40: int car,
//     float frame[12], float velocity[3]), +0xd4 IMixer*, +0xd8 SoundBase* to destroy when stopped [32], +0x158 their
//     count, +0x15c ISound* to destroy [1], +0x160 its count, +0x164 const char* async create's name, +0x168 its class,
//     +0x16c its ISound*, +0x170 u8 done, +0x174 u8 destroying, +0x178 the Multi lock, +0x17c the BG task's id
//   the quality table 0x4dd7e8 (MixerGetQuality 0..2; 40 bytes each): int voices (6 / 16 / 32), int per-class caps[8]
//     (256, 256, 3/6/10, 2/6/6, 2/3/6, 1/3/6, 1, 1), int 0x22b
// Statics: 0x4f5708 SoundManager* (sound.obj), 0x4f570c u8 restarting, 0x4f5a94 SoundManager::global, 0x4f5a98
//   SoundManager::update_f (a __thiscall member: update 0x474230, mixer_begin 0x474060, mixer_end 0x474090,
//   destroy_isounds 0x4743d0, or 0), 0x578a98 the default listener's velocity (3 floats), 0x578ad0 its frame (12),
//   0x578aa4..0x578b18 soundmgr.obj's 16 colour constants ($E initialisers).
//
// Shadow contract (DirectSound): nothing here calls DirectSound; update() reaches it through IMixer::Update (group C:
// WaveGrab's GetStatus / GetCurrentPosition / Lock, the mix into the locked ring, WaveRelease's Unlock -- inputs, then
// one output, the Unlock's record carrying the bytes), so update()'s footprint lists the mixer's memory but not the
// ring. SoundManager's constructor / destructor (through mixer_begin / mixer_end on the BG thread) create and release
// the DirectSound objects: replay-only.
//
// FIX CANDIDATES (reproduced, marked where they happen):
//   * every entry point uses the manager pointer (0x4f5708) unchecked: sound that failed to start (no mixer began, or
//     the manager's allocation failed) leaves it NULL, and SoundMuteCars / SetListener / StopCar / FlushAsync / any
//     sound's constructor then fault
//   * add_sound: no bound on the 0x600-byte table (384 sounds) and none on the sound's class (0..7): an out-of-range
//     class indexes past the 8 per-class tables and shifts the wrong ones
//   * DestroyWhenStopped panics past 32 (LogPanic); Destroy's ISound hand-off has room for one
//   * check_destroy_lists skips the entry it just moved down (checked a frame later)
//   * update(): with update_f cleared between Update's read and the lock, it returns with the lock held; and it stores
//     the async create's "done" (+0x170) BEFORE the result (+0x16c): the waiting thread can read the result as NULL
//     (the sound is then deleted by its factory as if the mixer had failed)
//   * Sound3D::update_sound: Doppler divides by (339.97 + closing speed): 0 when the source and listener close at the
//     speed of sound -> divide-by-zero on the BG (physics) thread, where it is unmasked
//   * SoundDash never initialises +0x38; CanBeHeard reads it on the first update (mute_sounds runs before UpdateSound)
//   * SoundClassString indexes its table with a signed class: a negative one reads the stack
//   * SoundResourceGet: an old-format 'SFX0' shorter than 0x800 bytes has its length wrapped negative
//   * mute_sounds: a class that gets part of its cap leaves want = want - (given + cap), not want - cap (the budget
//     sometimes grows back); harmless
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"

typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall
#define D(x) ((double)(x))                          // a register value (x87.h)

namespace {
// ---- memory -----------------------------------------------------------------------------------------------------------
static __forceinline volatile uint8_t& B8(const void* p, uint32_t off) { return *(volatile uint8_t*)((uintptr_t)p + off); }
static __forceinline volatile int32_t& I32(const void* p, uint32_t off) { return *(volatile int32_t*)((uintptr_t)p + off); }
static __forceinline volatile uint32_t& U32(const void* p, uint32_t off) { return *(volatile uint32_t*)((uintptr_t)p + off); }
static __forceinline uint8_t* PTR(const void* p, uint32_t off) { return (uint8_t*)(uintptr_t)U32(p, off); }
static __forceinline float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static __forceinline float F32(const void* p, uint32_t off) { return bf(U32(p, off)); }
// An fst / fstp dword the original makes, kept even where the value goes unused (docs/PORTING.md 10a)
static __forceinline float st(double d) {
    volatile float f = (float)d;
    return f;
}
// fld dword; fchs; fstp dword -- through the FPU, as the original does (a signalling NaN comes out quiet)
static __forceinline float fpu_neg(const float* p) {
    float r;
    __asm { mov eax, p
            fld dword ptr [eax]
            fchs
            fstp r }
    return r;
}
// a virtual call: obj's vtable slot at byte `off`, received as __thiscall
template <typename R, typename... A> static __forceinline R vcall(const void* obj, uint32_t off, A... a) {
    typedef R(__fastcall * Fn)(const void*, void*, A...);
    return ((Fn)(*(void* const* const*)obj)[off / 4])(obj, 0, a...);
}

struct Listener { int32_t car; const float* frame; const float* vel; };
static_assert(sizeof(Listener) == 12, "Listener");
}  // namespace

enum : uint32_t {
    S_MGR = 0x004f5708,                 // SoundManager* (sound.obj)
    S_RESTART = 0x004f570c,             // u8: SoundRestart in progress (MixerBegin / MixerEnd keep the mixers)
    S_GLOBAL = 0x004f5a94,              // SoundManager::global
    S_UPDATE_F = 0x004f5a98,            // SoundManager::update_f
    S_DEF_VEL = 0x00578a98,             // the default listener's velocity (3 floats)
    S_DEF_FRAME = 0x00578ad0,           // its frame (12 floats)
    S_QUALITY = 0x004dd7e8,             // the voice budgets: 10 ints per quality
    // what a mixer frame writes (group C's; listed by update()'s footprint)
    S_SOFT_CAPS = 0x00578b30,           // SoftMixer::GetCaps's MixerCaps (12 bytes)
    S_WAVE = 0x00578c90,                // wave.obj: ds, lock results, buffer, size, WAVEFORMATEX, write offset (0x3c)
    S_WAVE_RESYNC = 0x004f6390,         // wave.obj: the resync flag
    VT_SOUNDBASE = 0x004dd6c8, VT_SOUND3D = 0x004dd6d8, VT_SOUND = 0x004dd6e8, VT_SOUNDDASH = 0x004dd6f8,
    VT_SOFTMIXER = 0x004dd8c0,
    // the original's own functions, stored as pointers
    F_UPDATE = 0x00473e60, F_MGR_UPDATE = 0x00474230, F_MIXER_BEGIN = 0x00474060, F_MIXER_END = 0x00474090,
    F_DESTROY_ISOUNDS = 0x004743d0,
};
#define G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define G32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define S(a) ((const char*)(uintptr_t)(a))                 // one of the game's own strings

// ---- the functions they call, by address -------------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
static void* (__cdecl* const MemAlloc)(int) = (void* (__cdecl*)(int))0x004140e0;
static void (__cdecl* const OpDelete)(void*) = (void (__cdecl*)(void*))0x00414390;
static void (__cdecl* const LogPanic)(const char*, ...) = (void (__cdecl*)(const char*, ...))0x004112b0;
static void (__cdecl* const LogReport)(const char*, ...) = (void (__cdecl*)(const char*, ...))0x00411150;
static char* (__cdecl* const crt_strncpy)(char*, const char*, uint32_t) = (char* (__cdecl*)(char*, const char*, uint32_t))0x004cf3a0;
static void* (__cdecl* const crt_memmove)(void*, const void*, uint32_t) = (void* (__cdecl*)(void*, const void*, uint32_t))0x004cf400;
static void (__cdecl* const TaskSleep)(int) = (void (__cdecl*)(int))0x00414de0;
static int (__cdecl* const TaskGetID)() = (int (__cdecl*)())0x00414dd0;
static int (__cdecl* const MultiBegin)(const char*) = (int (__cdecl*)(const char*))0x004150a0;
static void (__cdecl* const MultiEnter)(int, const char*, int) = (void (__cdecl*)(int, const char*, int))0x00415180;
static void (__cdecl* const MultiLeave)(int, const char*, int) = (void (__cdecl*)(int, const char*, int))0x004151d0;
static void (__cdecl* const MultiEnd)(int, const char*, int) = (void (__cdecl*)(int, const char*, int))0x00415220;
static void (__cdecl* const BGHook)(uint32_t, int) = (void (__cdecl*)(uint32_t, int))0x004189f0;
static void (__cdecl* const BGUnhook)(uint32_t) = (void (__cdecl*)(uint32_t))0x00418ac0;
static void (__cdecl* const MixerBegin)(uint8_t) = (void (__cdecl*)(uint8_t))0x00473750;
static void (__cdecl* const MixerEnd)(uint8_t) = (void (__cdecl*)(uint8_t))0x00473880;
static void* (__cdecl* const MixerGet)(int) = (void* (__cdecl*)(int))0x00473850;
static int (__cdecl* const MixerGetDefault)() = (int (__cdecl*)())0x00473870;
static int32_t (__cdecl* const MixerGetQuality)() = (int32_t (__cdecl*)())0x00473b70;
static int (__cdecl* const WorldGetFocusCar)() = (int (__cdecl*)())0x004626d0;
static void (__cdecl* const MatrixMulPoint)(float*, const float*, const float*) = (void (__cdecl*)(float*, const float*, const float*))0x00429420;
static void* (__cdecl* const ResourceGet)(const char*, uint32_t, uint32_t*, int*, uint8_t*, uint8_t*) =
    (void* (__cdecl*)(const char*, uint32_t, uint32_t*, int*, uint8_t*, uint8_t*))0x00419fa0;
static uint8_t (__cdecl* const ResourceForget)(void*) = (uint8_t (__cdecl*)(void*))0x0041a450;
static void (__cdecl* const rcfunc_is_internal)() = (void (__cdecl*)())0x00410cc0;
static void* (__fastcall* const EngineSound_ctor)(void*, Edx, void*) = (void* (__fastcall*)(void*, Edx, void*))0x004728f0;
static void (__fastcall* const EngineSound_dtor)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x00472c10;
// this group's own, by address (a hooked rewrite is what runs)
static uint8_t (__cdecl* const g_SoundBegin)() = (uint8_t (__cdecl*)())0x00471cb0;
static void (__cdecl* const g_SoundEnd)() = (void (__cdecl*)())0x00471d90;
static void (__cdecl* const g_Sound_Toss)(const char*, int) = (void (__cdecl*)(const char*, int))0x004724c0;
static uint8_t* (__cdecl* const g_Sound_Create)(const char*, int) = (uint8_t* (__cdecl*)(const char*, int))0x004725b0;
static void* (__fastcall* const g_SoundBase_ctor)(void*, Edx, const char*, int) = (void* (__fastcall*)(void*, Edx, const char*, int))0x00471ec0;
static void (__fastcall* const g_SoundBase_dtor)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x00471f20;
static uint8_t (__fastcall* const g_SoundBase_CanBeHeard)(void*, Edx) = (uint8_t (__fastcall*)(void*, Edx))0x00471f60;
static void (__fastcall* const g_SoundBase_UpdateStatus)(void*, Edx, const Listener*) = (void (__fastcall*)(void*, Edx, const Listener*))0x00471f90;
static void (__fastcall* const g_SoundBase_UpdateSound)(void*, Edx, const Listener*) = (void (__fastcall*)(void*, Edx, const Listener*))0x00471fa0;
static void (__fastcall* const g_SoundBase_update_sound)(void*, Edx, const Listener*) = (void (__fastcall*)(void*, Edx, const Listener*))0x00472030;
static double (__cdecl* const g_attenuation)(float) = (double (__cdecl*)(float))0x00472120;
static void* (__fastcall* const g_Sound3D_ctor)(void*, Edx, const char*, int, const void*, const void*) =
    (void* (__fastcall*)(void*, Edx, const char*, int, const void*, const void*))0x00472340;
static void* (__fastcall* const g_Sound_ctor)(void*, Edx, const char*, int) = (void* (__fastcall*)(void*, Edx, const char*, int))0x004723c0;
static void (__fastcall* const g_Sound_update_sound)(void*, Edx, const Listener*) = (void (__fastcall*)(void*, Edx, const Listener*))0x00472410;
static void* (__fastcall* const g_SoundDash_ctor)(void*, Edx, const char*, int, int) = (void* (__fastcall*)(void*, Edx, const char*, int, int))0x00472440;
static void (__cdecl* const g_E1_sound)() = (void (__cdecl*)())0x00471ca0;
static void (__cdecl* const g_E1_soundmgr)() = (void (__cdecl*)())0x00473c10;
static void (__cdecl* const g_E1_soundres)() = (void (__cdecl*)())0x004771f0;
static void (__fastcall* const g_SoundTable_ctor)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x00473e80;
static void* (__fastcall* const g_SM_ctor)(void*, Edx, void*) = (void* (__fastcall*)(void*, Edx, void*))0x00473e90;
static void (__cdecl* const g_verify_entitlements)() = (void (__cdecl*)())0x00474050;
static void (__fastcall* const g_SM_fg_mixer_begin)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x004740b0;
static void (__fastcall* const g_SM_fg_mixer_end)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x004740d0;
static void (__fastcall* const g_SM_dtor)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x004740f0;
static void (__cdecl* const g_ASSERT_MSG)(int, const char*, ...) = (void (__cdecl*)(int, const char*, ...))0x00474190;
static void (__fastcall* const g_SM_StopCar)(void*, Edx, int) = (void (__fastcall*)(void*, Edx, int))0x004741a0;
static void (__fastcall* const g_SM_SetListener)(void*, Edx, const Listener*) = (void (__fastcall*)(void*, Edx, const Listener*))0x004741e0;
static void (__fastcall* const g_SM_destroy_sounds)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x00474370;
static void (__fastcall* const g_SM_destroy_isounds)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x004743d0;
static void (__fastcall* const g_SM_check_destroy_lists)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x00474420;
static void (__fastcall* const g_SM_mute_sounds)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x004744a0;
static void (__fastcall* const g_SM_verify_sanity)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x00474630;
static void (__fastcall* const g_SM_sort_3D)(void*, Edx, void*) = (void (__fastcall*)(void*, Edx, void*))0x004746a0;
static uint8_t (__cdecl* const g_sound3D_has_priority)(void*, void*) = (uint8_t (__cdecl*)(void*, void*))0x00474720;
static void (__fastcall* const g_SM_sort_sounds)(void*, Edx, const Listener*) = (void (__fastcall*)(void*, Edx, const Listener*))0x00474780;
static void (__fastcall* const g_SM_verify_soundtables)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x00474800;
static void (__fastcall* const g_SM_add_sound)(void*, Edx, void*) = (void (__fastcall*)(void*, Edx, void*))0x00474810;
static void (__fastcall* const g_SM_Add_Sound)(void*, Edx, void*) = (void (__fastcall*)(void*, Edx, void*))0x004748a0;
static void (__fastcall* const g_SM_Add_Sound3D)(void*, Edx, void*) = (void (__fastcall*)(void*, Edx, void*))0x004748b0;
static uint8_t (__fastcall* const g_SM_soundclass_is_3d)(void*, Edx, int) = (uint8_t (__fastcall*)(void*, Edx, int))0x004748c0;
static void (__fastcall* const g_SM_Remove)(void*, Edx, void*) = (void (__fastcall*)(void*, Edx, void*))0x004748e0;
static void (__fastcall* const g_SM_FlushAsync)(void*, Edx) = (void (__fastcall*)(void*, Edx))0x004749a0;
static void (__fastcall* const g_SM_DestroyWhenStopped)(void*, Edx, void*) = (void (__fastcall*)(void*, Edx, void*))0x004749b0;
static void* (__fastcall* const g_SM_create_sound)(void*, Edx, const char*, int) = (void* (__fastcall*)(void*, Edx, const char*, int))0x00474a10;
static void* (__fastcall* const g_SM_fg_create_sound)(void*, Edx, const char*, int) = (void* (__fastcall*)(void*, Edx, const char*, int))0x00474ab0;
static void* (__fastcall* const g_SM_Create)(void*, Edx, const char*, int) = (void* (__fastcall*)(void*, Edx, const char*, int))0x00474b30;
static void* (__fastcall* const g_SM_Create3D)(void*, Edx, const char*, int) = (void* (__fastcall*)(void*, Edx, const char*, int))0x00474b60;
static void (__fastcall* const g_SM_Destroy)(void*, Edx, void*) = (void (__fastcall*)(void*, Edx, void*))0x00474b90;
static void* (__fastcall* const g_listener_info_ctor)(void*, Edx) = (void* (__fastcall*)(void*, Edx))0x00474c30;

// ==== footprints ==========================================================================================================
namespace {
static uint32_t sound_size(const uint8_t* s) {
    switch (U32(s, 0)) {
    case VT_SOUND3D: return 0x40;
    case VT_SOUND: return 0x38;
    case VT_SOUNDDASH: return 0x3c;
    case VT_SOUNDBASE: return 0x30;
    default: return 0x40;
    }
}
// a sound and the mixer's sound it drives (SoftSound, 0x40)
static void fp_sound(Footprint& f, uint8_t* s) {
    if (!s) return;
    f.add(s, sound_size(s), "a sound");
    if (uint8_t* is = PTR(s, 0xc)) f.add(is, 0x40, "its mixer sound (SoftSound)");
}
// the manager, its table and every sound it holds or is about to destroy, and their mixer sounds
static void fp_manager(Footprint& f, uint8_t* mgr) {
    if (!mgr) return;
    f.add(mgr, 0x180, "the SoundManager");
    uint8_t* list = PTR(mgr, 4);
    if (!list) return;
    f.add(list, 0x600, "its sound table");
    const int32_t n = I32(mgr, 8);
    for (int32_t i = 0; i < n && i < 0x180; i++) fp_sound(f, PTR(list, 4u * (uint32_t)i));
    const int32_t nd = I32(mgr, 0x158);
    for (int32_t i = 0; i < nd && i < 32; i++) fp_sound(f, PTR(mgr, 0xd8 + 4u * (uint32_t)i));
    if (I32(mgr, 0x160) > 0 && PTR(mgr, 0x15c)) f.add(PTR(mgr, 0x15c), 0x40, "the mixer sound to destroy");
}
// a mixer frame (group C's SoftMixer::Update: its sounds, its caps, wave.obj's statics; the DirectSound ring is
// covered by the Unlock's record)
static void fp_mixer(Footprint& f, uint8_t* mgr) {
    uint8_t* m = PTR(mgr, 0xd4);
    if (!m) return;
    f.add(m, 0x10, "the mixer");
    if (U32(m, 0) == VT_SOFTMIXER) {
        if (uint8_t* bag = PTR(m, 8)) {
            f.add(bag, 0x14, "the SoftMixer's bag of sounds");
            uint8_t* l = PTR(bag, 0);
            const int32_t n = I32(bag, 8);
            for (int32_t i = 0; l && i < n && i < 0x400; i++)
                if (uint8_t* s = PTR(l, 4u * (uint32_t)i)) f.add(s, 0x40, "a SoftSound");
        }
    }
    f.add((void*)(uintptr_t)S_SOFT_CAPS, 12, "SoftMixer's caps (0x578b30)");
    f.add((void*)(uintptr_t)S_WAVE, 0x3c, "wave.obj's statics (0x578c90)");
    f.add((void*)(uintptr_t)S_WAVE_RESYNC, 4, "wave.obj's resync flag (0x4f6390)");
    f.add((void*)(uintptr_t)0x004f638c, 1, "wave.obj's locked flag (0x4f638c)");
    f.add((void*)(uintptr_t)0x004f65d4, 1, "fastmix's first flag (0x4f65d4)");
}
static void fp_static_init(Footprint& f) { f.replay_only = "a static initialiser (runs once, from the CRT's _initterm)"; }
static void fp_pure0(Footprint& f) { f.pure = true; }
}  // namespace

// ==== sound.obj ===========================================================================================================
static void __cdecl sound_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00471ca0, "$E1(sound.obj)", sound_E1_rw, fp_static_init)
static void __cdecl sound_E2_rw() { g_E1_sound(); }
PORT_FN(0x00471c90, "$E2(sound.obj)", sound_E2_rw, fp_static_init)

// Start sound: the mixers (unless restarting), the manager on the default mixer; a manager whose mixer didn't begin
// is destroyed again and the mixers ended.
static uint8_t __cdecl SoundBegin_rw() {
    MixerBegin(G8(S_RESTART) < 1);
    void* p = MemAlloc(0x180);
    if (p) {
        const int d = MixerGetDefault();
        void* m = MixerGet(d);
        G32(S_MGR) = (uint32_t)(uintptr_t)g_SM_ctor(p, 0, m);
    } else {
        G32(S_MGR) = 0;
    }
    if (G32(S_MGR) != 0) {
        if (*(volatile uint8_t*)(uintptr_t)G32(S_MGR) != 0) return 1;
        void* esi = (void*)(uintptr_t)G32(S_MGR);
        if (esi) {
            g_SM_dtor(esi, 0);
            OpDelete(esi);
        }
        G32(S_MGR) = 0;
    }
    MixerEnd(G8(S_RESTART) < 1);
    return 0;
}
static void fp_sound_begin(Footprint& f) { f.replay_only = "allocates the manager, which registers a BGHook and starts the mixer (DirectSoundCreate) on the BG thread"; }
PORT_FN(0x00471cb0, "SoundBegin", SoundBegin_rw, fp_sound_begin)

static void __cdecl SoundRestart_rw() {
    if (G32(S_MGR) != 0) {
        G8(S_RESTART) = 1;
        g_SoundEnd();
        g_SoundBegin();
        G8(S_RESTART) = 0;
    }
}
static void fp_sound_restart(Footprint& f) { f.replay_only = "ends and restarts the manager and the mixer"; }
PORT_FN(0x00471d60, "SoundRestart", SoundRestart_rw, fp_sound_restart)

static void __cdecl SoundEnd_rw() {
    void* esi = (void*)(uintptr_t)G32(S_MGR);
    if (esi) {
        g_SM_dtor(esi, 0);
        OpDelete(esi);
    }
    G32(S_MGR) = 0;
    MixerEnd(G8(S_RESTART) < 1);
}
static void fp_sound_end(Footprint& f) { f.replay_only = "destroys the manager, stops the mixer on the BG thread, frees"; }
PORT_FN(0x00471d90, "SoundEnd", SoundEnd_rw, fp_sound_end)

// FIX CANDIDATE: the manager unchecked (NULL when sound failed to start) -- here and in every entry point below
static void __cdecl SoundMuteCars_rw() { B8(PTR((void*)(uintptr_t)S_MGR, 0), 1) = 1; }
static void fp_sound_mute_cars(Footprint& f) { f.add((uint8_t*)(uintptr_t)G32(S_MGR) + 1, 1, "the manager's cars-muted flag"); }
PORT_FN(0x00471dd0, "SoundMuteCars", SoundMuteCars_rw, fp_sound_mute_cars)

static void __cdecl SoundUnMuteCars_rw() { B8(PTR((void*)(uintptr_t)S_MGR, 0), 1) = 0; }
PORT_FN(0x00471de0, "SoundUnMuteCars", SoundUnMuteCars_rw, fp_sound_mute_cars)

static void __cdecl SoundFlushAsync_rw() { g_SM_FlushAsync((void*)(uintptr_t)G32(S_MGR), 0); }
static void fp_flush(Footprint& f) { f.replay_only = "sleeps 50 ms (the BG thread runs), deletes the sounds left to stop"; }
PORT_FN(0x00471df0, "SoundFlushAsync", SoundFlushAsync_rw, fp_flush)

static void __cdecl SoundSetListener_rw(const Listener* l) { g_SM_SetListener((void*)(uintptr_t)G32(S_MGR), 0, l); }
static void fp_sound_set_listener(Footprint& f, const Listener*) {
    f.add((uint8_t*)(uintptr_t)G32(S_MGR) + 0x50, 0x44, "the manager's listener (+0x50: the index, listener_info[0])");
}
PORT_FN(0x00471e00, "SoundSetListener", SoundSetListener_rw, fp_sound_set_listener)

static void fp_stop_car_of(Footprint& f, uint8_t* mgr) {
    if (!mgr) return;                                       // (the call faults)
    uint8_t* list = PTR(mgr, 4);
    const int32_t n = I32(mgr, 8);
    for (int32_t i = 0; list && i < n && i < 0x180; i++)
        if (uint8_t* s = PTR(list, 4u * (uint32_t)i)) f.add(s + 0x2a, 1, "a sound's stop request");
}
static void __cdecl SoundStopCar_rw(int car) { g_SM_StopCar((void*)(uintptr_t)G32(S_MGR), 0, car); }
static void fp_sound_stop_car(Footprint& f, int) { fp_stop_car_of(f, (uint8_t*)(uintptr_t)G32(S_MGR)); }
PORT_FN(0x00471e20, "SoundStopCar", SoundStopCar_rw, fp_sound_stop_car)

// FIX CANDIDATE: a negative class reads below the table (the original's is on its stack)
static const char* __cdecl SoundClassString_rw(int cls) {
    const char* const tab[9] = {S(0x4f5758), S(0x4f5764), S(0x4f5778), S(0x4f5788), S(0x4f579c),
                                S(0x4f57ac), S(0x4f57c4), S(0x4f57d8), S(0x4f57e8)};
    if (cls < 9) return tab[cls];
    return S(0x4f57f8);                                    // "<invalid>"
}
static void fp_class_string(Footprint& f, int) { f.pure = true; }
PORT_FN(0x00471e40, "SoundClassString", SoundClassString_rw, fp_class_string)

static void __cdecl SoundClick_rw() { g_Sound_Toss(S(0x4f5804), 0); }   // "click.sfx", class 0
static void fp_sound_click(Footprint& f) { f.replay_only = "creates a sound (allocates, or waits for the BG thread)"; }
PORT_FN(0x00471eb0, "SoundClick", SoundClick_rw, fp_sound_click)

// ---- SoundBase ------------------------------------------------------------------------------------------------------------
static void* __fastcall SoundBase_ctor_rw(uint8_t* self, Edx, const char* name, int cls) {
    U32(self, 0) = VT_SOUNDBASE;
    crt_strncpy((char*)self + 0x14, name, 12);
    B8(self, 0x20) = 0;
    U32(self, 0xc) = 0;
    I32(self, 0x24) = cls;
    U32(self, 4) = 0x3f800000;
    U32(self, 8) = 0x3f800000;
    B8(self, 0x29) = 0;
    B8(self, 0x28) = 0;
    B8(self, 0x2a) = 1;
    B8(self, 0x2b) = 0;
    B8(self, 0x2d) = 1;
    B8(self, 0x2c) = 1;
    I32(self, 0x10) = -100;
    return self;
}
static void fp_soundbase_ctor(Footprint& f, uint8_t* self, Edx, const char*, int) { f.add(self, 0x2e, "the sound (constructing)"); }
PORT_FN(0x00471ec0, "SoundBase::SoundBase", SoundBase_ctor_rw, fp_soundbase_ctor)

static void __fastcall SoundBase_dtor_rw(uint8_t* self, Edx) {
    U32(self, 0) = VT_SOUNDBASE;
    if (U32(self, 0xc) != 0) {
        g_SM_Remove((void*)(uintptr_t)G32(S_MGR), 0, self);
        g_SM_Destroy((void*)(uintptr_t)G32(S_MGR), 0, PTR(self, 0xc));
        U32(self, 0xc) = 0;
    }
}
static void fp_soundbase_dtor(Footprint& f, uint8_t* self, Edx) {
    if (PTR(self, 0xc)) f.replay_only = "leaves the manager and destroys its mixer sound (frees, or waits for the BG thread to)";
    else f.add(self, 4, "the sound's vtable");
}
PORT_FN(0x00471f20, "SoundBase::~SoundBase", SoundBase_dtor_rw, fp_soundbase_dtor)

// audible: louder than 0.05 (the volume's bits, signed), and playing -- or stopped with a play request pending
static uint8_t __fastcall SoundBase_CanBeHeard_rw(uint8_t* self, Edx) {
    if (I32(self, 8) <= 0x3d4ccccd) return 0;
    if (vcall<int32_t>(PTR(self, 0xc), 0x14) != 3) return 1;
    if (B8(self, 0x29) == 0) return 0;
    return 1;
}
static void fp_reads_only(Footprint&, uint8_t*, Edx) {}
PORT_FN(0x00471f60, "SoundBase::CanBeHeard", SoundBase_CanBeHeard_rw, fp_reads_only)

static void __fastcall SoundBase_UpdateStatus_rw(uint8_t* self, Edx, const Listener* l) { vcall<void>(self, 0xc, l); }
static void fp_one_sound(Footprint& f, uint8_t* self, Edx, const Listener*) { fp_sound(f, self); }
PORT_FN(0x00471f90, "SoundBase::UpdateStatus", SoundBase_UpdateStatus_rw, fp_one_sound)

// the requests the owner left, carried out on the mixer's sound, then the class's own update (volume, pan, pitch)
static void __fastcall SoundBase_UpdateSound_rw(uint8_t* self, Edx, const Listener* l) {
    const int32_t status = vcall<int32_t>(PTR(self, 0xc), 0x14);
    if (status == 1) {
        if (B8(self, 0x2a) != 0) {
            vcall<void>(PTR(self, 0xc), 0x10);             // Stop
        } else {
            if (B8(self, 0x28) == 0) goto update;
            vcall<void>(PTR(self, 0xc), 0x18);             // Mute
        }
    } else if (status == 2) {
        if (B8(self, 0x28) != 0) return;
        if (B8(self, 0x2a) != 0) vcall<void>(PTR(self, 0xc), 0x10);   // Stop
        else vcall<void>(PTR(self, 0xc), 0x1c);                         // UnMute
    } else if (status == 3) {
        if (B8(self, 0x29) != 0) {
            B8(self, 0x29) = 0;
            if (B8(self, 0x2b) != 0) vcall<void>(PTR(self, 0xc), 0xc);  // PlayLooped
            else vcall<void>(PTR(self, 0xc), 8);                        // Play
        }
        if (B8(self, 0x28) == 0) goto update;
        vcall<void>(PTR(self, 0xc), 0x18);                              // Mute
    }
    if (B8(self, 0x28) != 0) return;
update:
    vcall<void>(self, 8, l);                                            // update_sound
}
PORT_FN(0x00471fa0, "SoundBase::UpdateSound", SoundBase_UpdateSound_rw, fp_one_sound)

static void __fastcall SoundBase_update_sound_rw(uint8_t* self, Edx, const Listener*) {
    if (B8(self, 0x2c) != 0) {
        uint8_t* is = PTR(self, 0xc);
        const uint32_t vol = U32(self, 8);
        B8(self, 0x2c) = 0;
        vcall<void>(is, 0x24, vol);                                     // SetVolume
    }
    if (B8(self, 0x2d) != 0) {
        uint8_t* is = PTR(self, 0xc);
        const uint32_t freq = U32(self, 4);
        B8(self, 0x2d) = 0;
        vcall<void>(is, 0x20, freq);                                    // SetFrequency
    }
}
PORT_FN(0x00472030, "SoundBase::update_sound", SoundBase_update_sound_rw, fp_one_sound)

// in range (within 200 m) and audible: the attenuated volume
static void __fastcall Sound3D_update_status_rw(uint8_t* self, Edx, const Listener* l) {
    if (!g_SoundBase_CanBeHeard(self, 0)) return;
    const uint8_t* pos = PTR(self, 0x3c);
    const uint8_t* lp = (const uint8_t*)l->frame + 0x24;
    const float dx = st(D(F32(pos, 0)) - D(F32(lp, 0)));
    const float dy = st(D(F32(pos, 4)) - D(F32(lp, 4)));
    const float dz = st(D(F32(pos, 8)) - D(F32(lp, 8)));
    const double yz = D(dy) * D(dy) + D(dz) * D(dz);
    U32(self, 0x34) = 0;
    B8(self, 0x30) = 0;
    const float d2 = st(yz + D(dx) * D(dx));
    const float k200 = 200.0f;
    if (!(D(k200) * D(k200) > D(d2))) return;              // fcomp; test ah,0x41
    B8(self, 0x30) = 1;
    const float d = (float)x87_sqrt(D(d2));                 // fstp dword: attenuation's argument
    const double a = g_attenuation(d);
    U32(self, 0x34) = fb(st(a * D(F32(self, 8))));
}
PORT_FN(0x00472070, "Sound3D::update_status", Sound3D_update_status_rw, fp_one_sound)

// ((200 - d) / 200)^2, left in ST0
static double __cdecl attenuation_rw(float d) {
    const float k200 = 200.0f;
    const double t = (D(k200) - D(d)) / D(k200);
    return t * t;
}
static void fp_attenuation(Footprint& f, float) { f.pure = true; }
PORT_FN(0x00472120, "attenuation", attenuation_rw, fp_attenuation)

// FIX CANDIDATE: Doppler's divisor 339.97 + closing speed reaches 0 at the speed of sound (divide-by-zero on the BG thread)
static void __fastcall Sound3D_update_sound_rw(uint8_t* self, Edx, const Listener* l) {
    const uint8_t* pos = PTR(self, 0x3c);
    const uint8_t* lp = (const uint8_t*)l->frame + 0x24;
    const float* lv = l->vel;
    float d0 = st(D(F32(pos, 0)) - D(F32(lp, 0)));
    float d1 = st(D(F32(pos, 4)) - D(F32(lp, 4)));
    float d2 = st(D(F32(pos, 8)) - D(F32(lp, 8)));
    const float dist = st(x87_sqrt((D(d1) * D(d1) + D(d2) * D(d2)) + D(d0) * D(d0)));
    if (B8(self, 0x30) == 0) return;
    if (vcall<uint8_t>(PTR(self, 0xc), 0x28)) {                        // Can3D: the mixer places it
        g_SoundBase_update_sound(self, 0, l);
        vcall<void>(PTR(self, 0xc), 0x30, PTR(self, 0x38), PTR(self, 0x3c));   // Set3D
        return;
    }
    vcall<void>(PTR(self, 0xc), 0x24, U32(self, 0x34));                // SetVolume(attenuated)
    float r[3];
    const uint32_t right[3] = {0x3f800000, 0, 0};                      // (1, 0, 0): the listener's right
    memcpy(r, right, 12);
    MatrixMulPoint(r, r, l->frame);
    uint32_t freq = U32(self, 4);
    if ((int32_t)fb(dist) > 0x3dcccccd) {                              // farther than 0.1 m (the bits, signed)
        const float one = 1.0f;
        const double inv = D(one) / D(dist);
        d0 = st(D(d0) * inv);
        d1 = st(D(d1) * inv);
        const double z = inv * D(d2);
        d2 = st(z);                                                     // fst: the register value goes on
        const double dot = (z * D(r[2]) + D(r[1]) * D(d1)) + D(r[0]) * D(d0);
        float pan;
        if ((int32_t)fb(dist) < 0x41200000) {                           // nearer than 10 m: scaled down
            pan = st(dot);
            const float k01 = bf(0x3dcccccd);
            pan = st((D(dist) * D(pan)) * D(k01));
        } else {
            pan = st(dot);
        }
        if (U32(l->frame, 0x10) > 0x80000000u) pan = fpu_neg(&pan);     // upside down
        vcall<void>(PTR(self, 0xc), 0x2c, fb(pan));                     // SetPanning
        const uint8_t* v = PTR(self, 0x38);
        const float c = bf(0x43a9fc29);                                 // 339.97: the speed of sound
        double s = ((D(F32(v, 4)) - D(lv[1])) * D(d1) + (D(F32(v, 8)) - D(lv[2])) * D(d2)) +
                   (D(F32(v, 0)) - D(lv[0])) * D(d0);
        s = s + D(c);
        const double q = D(c) / s;
        freq = fb(st(q * D(bf(freq))));
    } else {
        vcall<void>(PTR(self, 0xc), 0x2c, (uint32_t)0);                // SetPanning(0)
    }
    vcall<void>(PTR(self, 0xc), 0x20, freq);                            // SetFrequency
}
PORT_FN(0x00472150, "Sound3D::update_sound", Sound3D_update_sound_rw, fp_one_sound)

static void* __fastcall Sound3D_ctor_rw(uint8_t* self, Edx, const char* name, int cls, const void* vel, const void* pos) {
    g_SoundBase_ctor(self, 0, name, cls);
    U32(self, 0) = VT_SOUND3D;
    U32(self, 0x38) = (uint32_t)(uintptr_t)vel;
    U32(self, 0x3c) = (uint32_t)(uintptr_t)pos;
    U32(self, 0x34) = 0;
    B8(self, 0x30) = 1;
    void* is = g_SM_Create3D((void*)(uintptr_t)G32(S_MGR), 0, name, cls);
    U32(self, 0xc) = (uint32_t)(uintptr_t)is;
    if (is) g_SM_Add_Sound3D((void*)(uintptr_t)G32(S_MGR), 0, self);
    return self;
}
static void fp_sound_ctor(Footprint& f) { f.replay_only = "creates the mixer's sound (allocates, or waits for the BG thread)"; }
static void fp_sound3d_ctor(Footprint& f, uint8_t*, Edx, const char*, int, const void*, const void*) { fp_sound_ctor(f); }
PORT_FN(0x00472340, "Sound3D::Sound3D", Sound3D_ctor_rw, fp_sound3d_ctor)

static uint8_t __fastcall Sound3D_CanBeHeard_rw(uint8_t* self, Edx) {
    if (!g_SoundBase_CanBeHeard(self, 0)) return 0;
    if (B8(self, 0x30) == 0) return 0;
    return 1;
}
PORT_FN(0x004723a0, "Sound3D::CanBeHeard", Sound3D_CanBeHeard_rw, fp_reads_only)

static void* __fastcall Sound_ctor_rw(uint8_t* self, Edx, const char* name, int cls) {
    g_SoundBase_ctor(self, 0, name, cls);
    U32(self, 0) = VT_SOUND;
    U32(self, 0x30) = 0;
    B8(self, 0x34) = 1;
    void* is = g_SM_Create((void*)(uintptr_t)G32(S_MGR), 0, name, cls);
    U32(self, 0xc) = (uint32_t)(uintptr_t)is;
    if (is) g_SM_Add_Sound((void*)(uintptr_t)G32(S_MGR), 0, self);
    return self;
}
static void fp_sound_ctor2(Footprint& f, uint8_t*, Edx, const char*, int) { fp_sound_ctor(f); }
PORT_FN(0x004723c0, "Sound::Sound", Sound_ctor_rw, fp_sound_ctor2)

static void __fastcall Sound_update_sound_rw(uint8_t* self, Edx, const Listener* l) {
    g_SoundBase_update_sound(self, 0, l);
    if (B8(self, 0x34) != 0) {
        uint8_t* is = PTR(self, 0xc);
        const uint32_t pan = U32(self, 0x30);
        B8(self, 0x34) = 0;
        vcall<void>(is, 0x2c, pan);                                     // SetPanning
    }
}
PORT_FN(0x00472410, "Sound::update_sound", Sound_update_sound_rw, fp_one_sound)

// FIX CANDIDATE: +0x38 (heard from its own car) is never initialised here
static void* __fastcall SoundDash_ctor_rw(uint8_t* self, Edx, const char* name, int cls, int car) {
    g_Sound_ctor(self, 0, name, cls);
    U32(self, 0) = VT_SOUNDDASH;
    I32(self, 0x10) = car;
    return self;
}
static void fp_sounddash_ctor(Footprint& f, uint8_t*, Edx, const char*, int, int) { fp_sound_ctor(f); }
PORT_FN(0x00472440, "SoundDash::SoundDash", SoundDash_ctor_rw, fp_sounddash_ctor)

static void __fastcall SoundDash_update_sound_rw(uint8_t* self, Edx, const Listener* l) {
    g_Sound_update_sound(self, 0, l);
    B8(self, 0x38) = I32(self, 0x10) == l->car ? 1 : 0;
}
PORT_FN(0x00472470, "SoundDash::update_sound", SoundDash_update_sound_rw, fp_one_sound)

static uint8_t __fastcall SoundDash_CanBeHeard_rw(uint8_t* self, Edx) {
    if (!g_SoundBase_CanBeHeard(self, 0)) return 0;
    if (B8(self, 0x38) == 0) return 0;
    return 1;
}
PORT_FN(0x004724a0, "SoundDash::CanBeHeard", SoundDash_CanBeHeard_rw, fp_reads_only)

// fire and forget: played once (or looped, for a negative class: never stopped, so never destroyed), destroyed when
// it stops
static void __cdecl Sound_Toss_rw(const char* name, int cls) {
    const int a = (int)(((uint32_t)cls ^ (uint32_t)(cls >> 31)) - (uint32_t)(cls >> 31));   // cdq; xor; sub
    uint8_t* s = g_Sound_Create(name, a);
    if (!s) return;
    if (cls >= 0) {
        B8(s, 0x29) = 1;
        B8(s, 0x2b) = 0;
        B8(s, 0x2a) = 0;
    } else {
        B8(s, 0x2a) = 0;
        B8(s, 0x2b) = 1;
        B8(s, 0x29) = 1;
    }
    g_SM_DestroyWhenStopped((void*)(uintptr_t)G32(S_MGR), 0, s);
}
static void fp_toss(Footprint& f, const char*, int) { f.replay_only = "creates a sound (allocates, or waits for the BG thread)"; }
PORT_FN(0x004724c0, "Sound::Toss", Sound_Toss_rw, fp_toss)

static uint8_t* __cdecl SoundDash_Create_rw(const char* name, int cls, int car) {
    uint8_t* p = (uint8_t*)MemAlloc(0x3c);
    uint8_t* r = 0;
    if (p) r = (uint8_t*)g_SoundDash_ctor(p, 0, name, cls, car);
    if (r && U32(r, 0xc) == 0) {
        if (r) vcall<void*>(r, 0, 1u);
        r = 0;
    }
    return r;
}
static void fp_create3(Footprint& f, const char*, int, int) { f.replay_only = "allocates a sound"; }
PORT_FN(0x00472510, "SoundDash::Create", SoundDash_Create_rw, fp_create3)

static uint8_t* __cdecl Sound3D_Create_rw(const char* name, int cls, const void* vel, const void* pos) {
    uint8_t* p = (uint8_t*)MemAlloc(0x40);
    uint8_t* r = 0;
    if (p) r = (uint8_t*)g_Sound3D_ctor(p, 0, name, cls, vel, pos);
    if (r && U32(r, 0xc) == 0) {
        if (r) vcall<void*>(r, 0, 1u);
        r = 0;
    }
    return r;
}
static void fp_create4(Footprint& f, const char*, int, const void*, const void*) { f.replay_only = "allocates a sound"; }
PORT_FN(0x00472560, "Sound3D::Create", Sound3D_Create_rw, fp_create4)

static uint8_t* __cdecl Sound_Create_rw(const char* name, int cls) {
    uint8_t* p = (uint8_t*)MemAlloc(0x38);
    uint8_t* r = 0;
    if (p) r = (uint8_t*)g_Sound_ctor(p, 0, name, cls);
    if (r && U32(r, 0xc) == 0) {
        if (r) vcall<void*>(r, 0, 1u);
        r = 0;
    }
    return r;
}
static void fp_create2(Footprint& f, const char*, int) { f.replay_only = "allocates a sound"; }
PORT_FN(0x004725b0, "Sound::Create", Sound_Create_rw, fp_create2)

static uint8_t* __cdecl EngineSound_Create_rw(void* car) {
    uint8_t* esi = 0;
    uint8_t* p = (uint8_t*)MemAlloc(0xf8);
    if (p) esi = (uint8_t*)EngineSound_ctor(p, 0, car);
    if (esi && U32(esi, 0xf4) == 0) {
        if (esi) {
            EngineSound_dtor(esi, 0);
            OpDelete(esi);
        }
        esi = 0;
    }
    return esi;
}
static void fp_engine_create(Footprint& f, void*) { f.replay_only = "allocates an engine sound (and its samples' sounds)"; }
PORT_FN(0x004725f0, "EngineSound::Create", EngineSound_Create_rw, fp_engine_create)

static void __fastcall SoundBase_update_status_rw(uint8_t*, Edx, const Listener*) {}
static void fp_update_status_base(Footprint& f, uint8_t*, Edx, const Listener*) { f.pure = true; }
PORT_FN(0x00472640, "SoundBase::update_status", SoundBase_update_status_rw, fp_update_status_base)

// the deleting destructors: all four run SoundBase's (none of the classes has its own)
static void* __fastcall sound_sdtor(uint8_t* self, uint32_t flags) {
    g_SoundBase_dtor(self, 0);
    if (flags & 1) OpDelete(self);
    return self;
}
static void* __fastcall SoundBase_sdtor_rw(uint8_t* self, Edx, uint32_t flags) { return sound_sdtor(self, flags); }
static void* __fastcall Sound3D_sdtor_rw(uint8_t* self, Edx, uint32_t flags) { return sound_sdtor(self, flags); }
static void* __fastcall Sound_sdtor_rw(uint8_t* self, Edx, uint32_t flags) { return sound_sdtor(self, flags); }
static void* __fastcall SoundDash_sdtor_rw(uint8_t* self, Edx, uint32_t flags) { return sound_sdtor(self, flags); }
static void fp_sdtor(Footprint& f, uint8_t* self, Edx, uint32_t flags) {
    if (flags & 1) f.replay_only = "frees the sound";
    else fp_soundbase_dtor(f, self, 0);
}
PORT_FN(0x00472650, "SoundBase::scalar deleting destructor", SoundBase_sdtor_rw, fp_sdtor)
PORT_FN(0x00472670, "Sound3D::scalar deleting destructor", Sound3D_sdtor_rw, fp_sdtor)
PORT_FN(0x00472690, "Sound::scalar deleting destructor", Sound_sdtor_rw, fp_sdtor)
PORT_FN(0x004726b0, "SoundDash::scalar deleting destructor", SoundDash_sdtor_rw, fp_sdtor)

// ==== soundmgr.obj ========================================================================================================
static void __cdecl soundmgr_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00473c10, "$E1(soundmgr.obj)", soundmgr_E1_rw, fp_static_init)
static void __cdecl soundmgr_E2_rw() { g_E1_soundmgr(); }
PORT_FN(0x00473c00, "$E2(soundmgr.obj)", soundmgr_E2_rw, fp_static_init)
// 16 colour constants (a header's statics: 5-5-5 in the low half, 5-6-5 in the high); each $E(n+1) calls $E(n)
// (written out, not a macro: tools/gen_port_tables.py finds hooked addresses by PORT_FN(0x...) in the source)
static void __cdecl soundmgr_E5_rw() { G32(0x00578aa8) = 0x00000000u; }
PORT_FN(0x00473c30, "$E5(soundmgr.obj)", soundmgr_E5_rw, fp_static_init)
static void __cdecl soundmgr_E6_rw() { ((Void_t)(uintptr_t)(0x00473c30))(); }
PORT_FN(0x00473c20, "$E6(soundmgr.obj)", soundmgr_E6_rw, fp_static_init)
static void __cdecl soundmgr_E8_rw() { G32(0x00578ac4) = 0x001f001fu; }
PORT_FN(0x00473c50, "$E8(soundmgr.obj)", soundmgr_E8_rw, fp_static_init)
static void __cdecl soundmgr_E9_rw() { ((Void_t)(uintptr_t)(0x00473c50))(); }
PORT_FN(0x00473c40, "$E9(soundmgr.obj)", soundmgr_E9_rw, fp_static_init)
static void __cdecl soundmgr_E11_rw() { G32(0x00578ab4) = 0x03e001e0u; }
PORT_FN(0x00473c70, "$E11(soundmgr.obj)", soundmgr_E11_rw, fp_static_init)
static void __cdecl soundmgr_E12_rw() { ((Void_t)(uintptr_t)(0x00473c70))(); }
PORT_FN(0x00473c60, "$E12(soundmgr.obj)", soundmgr_E12_rw, fp_static_init)
static void __cdecl soundmgr_E14_rw() { G32(0x00578ac8) = 0x03ef01efu; }
PORT_FN(0x00473c90, "$E14(soundmgr.obj)", soundmgr_E14_rw, fp_static_init)
static void __cdecl soundmgr_E15_rw() { ((Void_t)(uintptr_t)(0x00473c90))(); }
PORT_FN(0x00473c80, "$E15(soundmgr.obj)", soundmgr_E15_rw, fp_static_init)
static void __cdecl soundmgr_E17_rw() { G32(0x00578b14) = 0x78003c00u; }
PORT_FN(0x00473cb0, "$E17(soundmgr.obj)", soundmgr_E17_rw, fp_static_init)
static void __cdecl soundmgr_E18_rw() { ((Void_t)(uintptr_t)(0x00473cb0))(); }
PORT_FN(0x00473ca0, "$E18(soundmgr.obj)", soundmgr_E18_rw, fp_static_init)
static void __cdecl soundmgr_E20_rw() { G32(0x00578ac0) = 0x780f3c0fu; }
PORT_FN(0x00473cd0, "$E20(soundmgr.obj)", soundmgr_E20_rw, fp_static_init)
static void __cdecl soundmgr_E21_rw() { ((Void_t)(uintptr_t)(0x00473cd0))(); }
PORT_FN(0x00473cc0, "$E21(soundmgr.obj)", soundmgr_E21_rw, fp_static_init)
static void __cdecl soundmgr_E23_rw() { G32(0x00578b08) = 0x7be03de0u; }
PORT_FN(0x00473cf0, "$E23(soundmgr.obj)", soundmgr_E23_rw, fp_static_init)
static void __cdecl soundmgr_E24_rw() { ((Void_t)(uintptr_t)(0x00473cf0))(); }
PORT_FN(0x00473ce0, "$E24(soundmgr.obj)", soundmgr_E24_rw, fp_static_init)
static void __cdecl soundmgr_E26_rw() { G32(0x00578abc) = 0x7bef3defu; }
PORT_FN(0x00473d10, "$E26(soundmgr.obj)", soundmgr_E26_rw, fp_static_init)
static void __cdecl soundmgr_E27_rw() { ((Void_t)(uintptr_t)(0x00473d10))(); }
PORT_FN(0x00473d00, "$E27(soundmgr.obj)", soundmgr_E27_rw, fp_static_init)
static void __cdecl soundmgr_E29_rw() { G32(0x00578b04) = 0x42082108u; }
PORT_FN(0x00473d30, "$E29(soundmgr.obj)", soundmgr_E29_rw, fp_static_init)
static void __cdecl soundmgr_E30_rw() { ((Void_t)(uintptr_t)(0x00473d30))(); }
PORT_FN(0x00473d20, "$E30(soundmgr.obj)", soundmgr_E30_rw, fp_static_init)
static void __cdecl soundmgr_E32_rw() { G32(0x00578aa4) = 0x001f001fu; }
PORT_FN(0x00473d50, "$E32(soundmgr.obj)", soundmgr_E32_rw, fp_static_init)
static void __cdecl soundmgr_E33_rw() { ((Void_t)(uintptr_t)(0x00473d50))(); }
PORT_FN(0x00473d40, "$E33(soundmgr.obj)", soundmgr_E33_rw, fp_static_init)
static void __cdecl soundmgr_E35_rw() { G32(0x00578b10) = 0x07e003e0u; }
PORT_FN(0x00473d70, "$E35(soundmgr.obj)", soundmgr_E35_rw, fp_static_init)
static void __cdecl soundmgr_E36_rw() { ((Void_t)(uintptr_t)(0x00473d70))(); }
PORT_FN(0x00473d60, "$E36(soundmgr.obj)", soundmgr_E36_rw, fp_static_init)
static void __cdecl soundmgr_E38_rw() { G32(0x00578ab0) = 0x07ff03ffu; }
PORT_FN(0x00473d90, "$E38(soundmgr.obj)", soundmgr_E38_rw, fp_static_init)
static void __cdecl soundmgr_E39_rw() { ((Void_t)(uintptr_t)(0x00473d90))(); }
PORT_FN(0x00473d80, "$E39(soundmgr.obj)", soundmgr_E39_rw, fp_static_init)
static void __cdecl soundmgr_E41_rw() { G32(0x00578aac) = 0xf8007c00u; }
PORT_FN(0x00473db0, "$E41(soundmgr.obj)", soundmgr_E41_rw, fp_static_init)
static void __cdecl soundmgr_E42_rw() { ((Void_t)(uintptr_t)(0x00473db0))(); }
PORT_FN(0x00473da0, "$E42(soundmgr.obj)", soundmgr_E42_rw, fp_static_init)
static void __cdecl soundmgr_E44_rw() { G32(0x00578b18) = 0xf81f7c1fu; }
PORT_FN(0x00473dd0, "$E44(soundmgr.obj)", soundmgr_E44_rw, fp_static_init)
static void __cdecl soundmgr_E45_rw() { ((Void_t)(uintptr_t)(0x00473dd0))(); }
PORT_FN(0x00473dc0, "$E45(soundmgr.obj)", soundmgr_E45_rw, fp_static_init)
static void __cdecl soundmgr_E47_rw() { G32(0x00578b00) = 0xffe07fe0u; }
PORT_FN(0x00473df0, "$E47(soundmgr.obj)", soundmgr_E47_rw, fp_static_init)
static void __cdecl soundmgr_E48_rw() { ((Void_t)(uintptr_t)(0x00473df0))(); }
PORT_FN(0x00473de0, "$E48(soundmgr.obj)", soundmgr_E48_rw, fp_static_init)
static void __cdecl soundmgr_E50_rw() { G32(0x00578ab8) = 0xffff7fffu; }
PORT_FN(0x00473e10, "$E50(soundmgr.obj)", soundmgr_E50_rw, fp_static_init)
static void __cdecl soundmgr_E51_rw() { ((Void_t)(uintptr_t)(0x00473e10))(); }
PORT_FN(0x00473e00, "$E51(soundmgr.obj)", soundmgr_E51_rw, fp_static_init)
static void __cdecl soundmgr_E55_rw() {}                  // a bare `ret` (an empty static's constructor)
PORT_FN(0x00473e30, "$E55(soundmgr.obj)", soundmgr_E55_rw, fp_static_init)
static void __cdecl soundmgr_E56_rw() { ((Void_t)(uintptr_t)0x00473e30)(); }
PORT_FN(0x00473e20, "$E56(soundmgr.obj)", soundmgr_E56_rw, fp_static_init)
static void __cdecl soundmgr_E58_rw() {}
PORT_FN(0x00473e50, "$E58(soundmgr.obj)", soundmgr_E58_rw, fp_static_init)
static void __cdecl soundmgr_E59_rw() { ((Void_t)(uintptr_t)0x00473e50)(); }
PORT_FN(0x00473e40, "$E59(soundmgr.obj)", soundmgr_E59_rw, fp_static_init)

namespace {
// what the BG thread's update_f is about to do
static void fp_update_f(Footprint& f, uint32_t fn, uint8_t* mgr);
}

// the BGHook: whatever update_f holds (update, or a job the main thread left: mixer_begin, mixer_end, destroy_isounds)
static void __cdecl SoundManager_Update_rw() {
    const uint32_t fn = G32(S_UPDATE_F);
    if (fn) ((void(__fastcall*)(void*, Edx))(uintptr_t)fn)((void*)(uintptr_t)G32(S_GLOBAL), 0);
}
static void fp_sm_Update(Footprint& f) { fp_update_f(f, G32(S_UPDATE_F), (uint8_t*)(uintptr_t)G32(S_GLOBAL)); }
PORT_FN(0x00473e60, "SoundManager::Update", SoundManager_Update_rw, fp_sm_Update)

static void* __fastcall SoundTable_ctor_rw(uint8_t* self, Edx) {
    U32(self, 0) = 0;
    U32(self, 4) = 0;
    return self;
}
static void fp_soundtable_ctor(Footprint& f, uint8_t* self, Edx) { f.add(self, 8, "the SoundTable"); }
PORT_FN(0x00473e80, "SoundManager::SoundTable::SoundTable", SoundTable_ctor_rw, fp_soundtable_ctor)

static void* __fastcall SoundManager_ctor_rw(uint8_t* self, Edx, void* mixer) {
    B8(self, 0) = 0;
    g_SoundTable_ctor(self + 4, 0);
    for (uint32_t k = 0; k < 8; k++) g_SoundTable_ctor(self + 0xc + 8 * k, 0);
    for (uint32_t k = 0; k < 2; k++) g_listener_info_ctor(self + 0x54 + 0x40 * k, 0);
    U32(self, 4) = (uint32_t)(uintptr_t)MemAlloc(0x600);
    g_verify_entitlements();
    if (U32(self, 4) == 0) return self;
    U32(self, 0xd4) = (uint32_t)(uintptr_t)mixer;
    G32(S_GLOBAL) = (uint32_t)(uintptr_t)self;
    U32(self, 8) = 0;
    B8(self, 0x4c) = 0;
    B8(self, 1) = 0;
    B8(self, 0x174) = 0;
    I32(self, 0x50) = -1;
    static const uint32_t frame[12] = {0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0};
    for (int i = 0; i < 12; i++) G32(S_DEF_FRAME + 4 * i) = frame[i];
    G32(S_DEF_VEL) = 0;
    G32(S_DEF_VEL + 4) = 0;
    G32(S_DEF_VEL + 8) = 0;
    Listener l;
    l.car = -100;
    l.frame = (const float*)(uintptr_t)S_DEF_FRAME;
    l.vel = (const float*)(uintptr_t)S_DEF_VEL;
    g_SM_SetListener(self, 0, &l);
    U32(self, 0x15c) = 0;
    U32(self, 0x160) = 0;
    for (uint32_t i = 0; i < 0x21; i++) U32(self, 0xd8 + 4 * i) = 0;   // the destroy list and its count
    U32(self, 0x164) = 0;
    U32(self, 0x168) = 0;
    U32(self, 0x16c) = 0;
    U32(self, 0x170) = 0;
    for (int i = 0; i < 8; i++) U32(self, 0xc + 8 * i) = U32(self, 4);
    U32(self, 0x17c) = 0;
    I32(self, 0x178) = MultiBegin(S(0x4f5aec));           // "SoundManager"
    BGHook(F_UPDATE, 1);
    g_SM_fg_mixer_begin(self, 0);
    if (B8(self, 0) == 0) {
        MultiEnd(I32(self, 0x178), 0, 0);
        void* list = PTR(self, 4);
        U32(self, 0x178) = 0;
        OpDelete(list);
        U32(self, 4) = 0;
    }
    return self;
}
static void fp_sm_ctor(Footprint& f, uint8_t*, Edx, void*) { f.replay_only = "allocates the table, registers a BGHook and a lock, starts the mixer on the BG thread"; }
PORT_FN(0x00473e90, "SoundManager::SoundManager", SoundManager_ctor_rw, fp_sm_ctor)

static void __cdecl verify_entitlements_rw() {}           // a bare `ret` in the release build
PORT_FN(0x00474050, "verify_entitlements", verify_entitlements_rw, fp_pure0)

// on the BG thread (fg_mixer_begin's job): the mixer's Begin, and update() from then on
static void __fastcall SM_mixer_begin_rw(uint8_t* self, Edx) {
    if (vcall<uint8_t>(PTR(self, 0xd4), 0xc)) {
        B8(self, 0) = 1;
        G32(S_UPDATE_F) = F_MGR_UPDATE;
        return;
    }
    G32(S_UPDATE_F) = 0;
}
static void fp_mixer_begin(Footprint& f, uint8_t*, Edx) { f.replay_only = "starts the mixer (DirectSoundCreate, the buffers)"; }
PORT_FN(0x00474060, "SoundManager::mixer_begin", SM_mixer_begin_rw, fp_mixer_begin)

static void __fastcall SM_mixer_end_rw(uint8_t* self, Edx) {
    vcall<void>(PTR(self, 0xd4), 0x18);
    G32(S_UPDATE_F) = 0;
}
static void fp_mixer_end(Footprint& f, uint8_t*, Edx) { f.replay_only = "stops the mixer (releases DirectSound)"; }
PORT_FN(0x00474090, "SoundManager::mixer_end", SM_mixer_end_rw, fp_mixer_end)

static void __fastcall SM_fg_mixer_begin_rw(uint8_t*, Edx) {
    G32(S_UPDATE_F) = F_MIXER_BEGIN;
    do TaskSleep(50);
    while (G32(S_UPDATE_F) == F_MIXER_BEGIN);
}
static void fp_fg_wait(Footprint& f, uint8_t*, Edx) { f.replay_only = "hands a job to the BG thread and sleeps until it's done"; }
PORT_FN(0x004740b0, "SoundManager::fg_mixer_begin", SM_fg_mixer_begin_rw, fp_fg_wait)

static void __fastcall SM_fg_mixer_end_rw(uint8_t*, Edx) {
    G32(S_UPDATE_F) = F_MIXER_END;
    do TaskSleep(50);
    while (G32(S_UPDATE_F) == F_MIXER_END);
}
PORT_FN(0x004740d0, "SoundManager::fg_mixer_end", SM_fg_mixer_end_rw, fp_fg_wait)

static void __fastcall SoundManager_dtor_rw(uint8_t* self, Edx) {
    if (B8(self, 0) == 0) return;
    MultiEnter(I32(self, 0x178), 0, 0);
    G32(S_UPDATE_F) = F_DESTROY_ISOUNDS;
    MultiLeave(I32(self, 0x178), 0, 0);
    g_SM_destroy_sounds(self, 0);
    g_ASSERT_MSG(U32(self, 8) < 1u ? 1 : 0, S(0x4f5afc));   // "Undestroyed sounds in ~SoundManager"
    OpDelete(PTR(self, 4));
    U32(self, 4) = 0;
    U32(self, 8) = 0;
    B8(self, 0) = 0;
    g_SM_fg_mixer_end(self, 0);
    BGUnhook(F_UPDATE);
    MultiEnd(I32(self, 0x178), 0, 0);
}
static void fp_sm_dtor(Footprint& f, uint8_t* self, Edx) {
    if (B8(self, 0)) f.replay_only = "frees the sounds and the table, stops the mixer on the BG thread, unhooks";
}
PORT_FN(0x004740f0, "SoundManager::~SoundManager", SoundManager_dtor_rw, fp_sm_dtor)

static void __cdecl ASSERT_MSG_rw(int, const char*) {}   // variadic in the original; a bare `ret` either way
static void fp_assert_msg(Footprint& f, int, const char*) { f.pure = true; }
PORT_FN(0x00474190, "ASSERT_MSG(soundmgr.obj)", ASSERT_MSG_rw, fp_assert_msg)

static void __fastcall SM_StopCar_rw(uint8_t* self, Edx, int car) {
    for (int32_t i = 0; I32(self, 8) > i; i++) {
        uint8_t* s = PTR(PTR(self, 4), 4u * (uint32_t)i);
        if (I32(s, 0x10) == car) B8(s, 0x2a) = 1;
    }
}
static void fp_stop_car(Footprint& f, uint8_t* self, Edx, int) { fp_stop_car_of(f, self); }
PORT_FN(0x004741a0, "SoundManager::StopCar", SM_StopCar_rw, fp_stop_car)

// into listener_info[0], the index held at -1 while it's copied (no lock: the BG thread reads it)
static void __fastcall SM_SetListener_rw(uint8_t* self, Edx, const Listener* l) {
    I32(self, 0x50) = -1;
    I32(self, 0x54) = l->car;
    const uint8_t* m = (const uint8_t*)l->frame;
    for (uint32_t i = 0; i < 12; i++) U32(self, 0x58 + 4 * i) = U32(m, 4 * i);    // rep movsd
    const uint8_t* v = (const uint8_t*)l->vel;
    U32(self, 0x88) = U32(v, 0);
    U32(self, 0x8c) = U32(v, 4);
    const uint32_t vz = U32(v, 8);
    I32(self, 0x50) = 0;
    U32(self, 0x90) = vz;
}
static void fp_set_listener(Footprint& f, uint8_t* self, Edx, const Listener*) {
    f.add(self + 0x50, 0x44, "the manager's listener (+0x50: the index, listener_info[0])");
}
PORT_FN(0x004741e0, "SoundManager::SetListener", SM_SetListener_rw, fp_set_listener)

// FIX CANDIDATE: with update_f cleared after Update read it, returns holding the lock; the async create's done flag
// is stored before its result
static void __fastcall SM_update_rw(uint8_t* self, Edx) {
    MultiEnter(I32(self, 0x178), 0, 0);
    if (G32(S_UPDATE_F) == 0) return;
    if (I32(self, 0x17c) == 0) I32(self, 0x17c) = TaskGetID();
    const int32_t idx = I32(self, 0x50);
    if (idx != -1) {
        if (B8(self, 0x4c) != 0) {
            B8(self, 0x4c) = 0;
            I32(self, 0x50) = (uint32_t)idx < 1u ? 1 : 0;
        }
        const uint32_t o = (uint32_t)I32(self, 0x50) << 6;
        Listener l;
        l.car = I32(self, o + 0x54);
        l.vel = (const float*)(self + o + 0x88);
        l.frame = (const float*)(self + o + 0x58);
        for (int32_t i = 0; I32(self, 8) > i; i++) g_SoundBase_UpdateStatus(PTR(PTR(self, 4), 4u * (uint32_t)i), 0, &l);
        g_SM_sort_sounds(self, 0, &l);
        g_SM_mute_sounds(self, 0);
        for (int32_t i = 0; I32(self, 8) > i; i++) g_SoundBase_UpdateSound(PTR(PTR(self, 4), 4u * (uint32_t)i), 0, &l);
        g_SM_verify_sanity(self, 0);
        g_SM_check_destroy_lists(self, 0);
        vcall<void>(PTR(self, 0xd4), 0x14, (const Listener*)&l);    // the mixer's frame
        const char* name = (const char*)PTR(self, 0x164);
        if (name && U32(self, 0x16c) == 0) {
            void* r = g_SM_create_sound(self, 0, name, I32(self, 0x168));
            B8(self, 0x170) = 1;
            U32(self, 0x16c) = (uint32_t)(uintptr_t)r;
        }
    }
    MultiLeave(I32(self, 0x178), 0, 0);
}
namespace {
static void fp_update_frame(Footprint& f, uint8_t* self) {
    if (G32(S_UPDATE_F) == 0 || !self) return;
    if (I32(self, 0x50) != -1 && PTR(self, 0x164) && U32(self, 0x16c) == 0) {
        f.replay_only = "creates the sound the main thread asked for (the mixer allocates it, loads its resource)";
        return;
    }
    fp_manager(f, self);
    fp_mixer(f, self);
}
static void fp_update_f(Footprint& f, uint32_t fn, uint8_t* mgr) {
    switch (fn) {
    case 0: break;
    case F_MGR_UPDATE: fp_update_frame(f, mgr); break;
    case F_DESTROY_ISOUNDS: if (mgr && I32(mgr, 0x160) > 0) f.replay_only = "deletes the mixer sounds handed over"; else if (mgr) f.add(mgr + 0x160, 4, "the count"); break;
    default: f.replay_only = "starts or stops the mixer on the BG thread"; break;
    }
}
}  // namespace
static void fp_sm_update(Footprint& f, uint8_t* self, Edx) { fp_update_frame(f, self); }
PORT_FN(0x00474230, "SoundManager::update", SM_update_rw, fp_sm_update)

static void __fastcall SM_destroy_sounds_rw(uint8_t* self, Edx) {
    B8(self, 0x174) = 1;
    TaskSleep(50);
    for (int32_t i = 0; I32(self, 0x158) > i; i++) {
        uint8_t* s = PTR(self, 0xd8 + 4u * (uint32_t)i);
        if (s) vcall<void*>(s, 0, 1u);
        U32(self, 0xd8 + 4u * (uint32_t)i) = 0;
    }
    B8(self, 0x174) = 0;
    U32(self, 0x158) = 0;
}
static void fp_destroy_sounds(Footprint& f, uint8_t*, Edx) { fp_flush(f); }
PORT_FN(0x00474370, "SoundManager::destroy_sounds", SM_destroy_sounds_rw, fp_destroy_sounds)

static void __fastcall SM_destroy_isounds_rw(uint8_t* self, Edx) {
    for (int32_t i = 0; I32(self, 0x160) > i; i++) {
        uint8_t* s = PTR(self, 0x15c + 4u * (uint32_t)i);
        if (s) vcall<void*>(s, 0, 1u);
        U32(self, 0x15c + 4u * (uint32_t)i) = 0;
    }
    U32(self, 0x160) = 0;
}
static void fp_destroy_isounds(Footprint& f, uint8_t* self, Edx) { fp_update_f(f, F_DESTROY_ISOUNDS, self); }
PORT_FN(0x004743d0, "SoundManager::destroy_isounds", SM_destroy_isounds_rw, fp_destroy_isounds)

// FIX CANDIDATE: the entry moved down into a deleted one's slot is skipped this frame
static void __fastcall SM_check_destroy_lists_rw(uint8_t* self, Edx) {
    if (B8(self, 0x174) == 0) {
        int32_t i = 0;
        if (I32(self, 0x158) > 0) {
            uint32_t p = 0xd8;
            do {
                if (vcall<int32_t>(PTR(PTR(self, p), 0xc), 0x14) == 3) {
                    uint8_t* s = PTR(self, p);
                    if (s) vcall<void*>(s, 0, 1u);
                    U32(self, p) = 0;
                    const int32_t n = I32(self, 0x158) - 1;
                    I32(self, 0x158) = n;
                    crt_memmove(self + p, self + p + 4, (uint32_t)(n - i) << 2);
                }
                p += 4;
                i++;
            } while (I32(self, 0x158) > i);
        }
    }
    g_SM_destroy_isounds(self, 0);
}
static void fp_check_destroy(Footprint& f, uint8_t* self, Edx) { fp_manager(f, self); }
PORT_FN(0x00474420, "SoundManager::check_destroy_lists", SM_check_destroy_lists_rw, fp_check_destroy)

// The voice budget (the quality's voices, each class capped): count the audible sounds per class (all but the UI class
// skipped while the cars are muted), hand out voices class by class until the budget or the demand runs out, then mute
// everything that didn't get one.
static void __fastcall SM_mute_sounds_rw(uint8_t* self, Edx) {
    const int32_t q = MixerGetQuality();
    uint8_t muted_with_cars[8];
    muted_with_cars[0] = 0;
    const uint32_t qoff = (uint32_t)q * 40u;
    int32_t budget = *(const volatile int32_t*)(uintptr_t)(S_QUALITY + qoff);
    for (int k = 1; k < 8; k++) muted_with_cars[k] = 1;
    int32_t want[8], given[8];
    for (int k = 0; k < 8; k++) want[k] = 0;
    if (budget > 0) {
        for (uint32_t k = 0; k < 8; k++) {
            want[k] = 0;
            if (B8(self, 1) == 0 || muted_with_cars[k] == 0) {
                int32_t n = I32(self, 0x10 + 8 * k);
                if (n > 0) {
                    uint32_t o = 0;
                    do {
                        if (vcall<uint8_t>(PTR(PTR(self, 0xc + 8 * k), o), 4)) want[k]++;
                        o += 4;
                    } while (--n != 0);
                }
            }
        }
    }
    for (int k = 0; k < 8; k++) given[k] = 0;
    for (;;) {
        uint8_t done = 1;
        if (budget > 0) {
            uint32_t i = 0;
            const volatile int32_t* cap_p = (const volatile int32_t*)(uintptr_t)(S_QUALITY + 4 + qoff);
            do {
                if (i >= 8) break;
                const int32_t w = want[i];
                if (w != 0) {
                    int32_t cap = *cap_p;
                    if (cap >= budget) cap = budget;
                    if (cap < w) {
                        budget = (int32_t)((uint32_t)budget - (uint32_t)cap);
                        const int32_t g = (int32_t)((uint32_t)given[i] + (uint32_t)cap);
                        given[i] = g;
                        done = 0;
                        want[i] = (int32_t)((uint32_t)w - (uint32_t)g);     // FIX CANDIDATE: w - cap meant
                    } else {
                        budget = (int32_t)((uint32_t)budget - (uint32_t)w);
                        given[i] = (int32_t)((uint32_t)given[i] + (uint32_t)w);
                        want[i] = 0;
                    }
                }
                i++;
                cap_p++;
            } while (budget > 0);
        }
        if (done != 0 || budget < 1) break;
    }
    for (uint32_t k = 0; k < 8; k++) {
        int32_t n = I32(self, 0x10 + 8 * k);
        if (n > 0) {
            uint32_t o = 0;
            do {
                const uint8_t heard = vcall<uint8_t>(PTR(PTR(self, 0xc + 8 * k), o), 4);
                if (heard && given[k] != 0) {
                    given[k]--;
                    B8(PTR(PTR(self, 0xc + 8 * k), o), 0x28) = 0;
                } else {
                    B8(PTR(PTR(self, 0xc + 8 * k), o), 0x28) = 1;
                }
                o += 4;
            } while (--n != 0);
        }
    }
}
static void fp_mute_sounds(Footprint& f, uint8_t* self, Edx) {
    for (uint32_t k = 0; k < 8; k++) {
        uint8_t* l = PTR(self, 0xc + 8 * k);
        const int32_t n = I32(self, 0x10 + 8 * k);
        for (int32_t i = 0; l && i < n && i < 0x180; i++)
            if (uint8_t* s = PTR(l, 4u * (uint32_t)i)) f.add(s + 0x28, 1, "a sound's muted flag");
    }
}
PORT_FN(0x004744a0, "SoundManager::mute_sounds", SM_mute_sounds_rw, fp_mute_sounds)

static void __fastcall SM_verify_sanity_rw(uint8_t* self, Edx) {
    int32_t playing = 0;
    const int32_t q = MixerGetQuality();
    const int32_t max = *(const volatile int32_t*)(uintptr_t)(S_QUALITY + (uint32_t)q * 40u);
    for (int32_t i = 0; I32(self, 8) > i; i++)
        if (vcall<int32_t>(PTR(PTR(PTR(self, 4), 4u * (uint32_t)i), 0xc), 0x14) == 1) playing++;
    if (playing > max) LogPanic(S(0x4f5b20), playing, max);   // "Hey I suck!  playing > max_sounds (%d > %d)"
}
static void fp_nothing(Footprint&, uint8_t*, Edx) {}
PORT_FN(0x00474630, "SoundManager::verify_sanity", SM_verify_sanity_rw, fp_nothing)

// insertion sort, the higher-priority sound first
static void __fastcall SM_sort_3D_rw(uint8_t*, Edx, uint8_t* table) {
    const int32_t n = I32(table, 4);
    uint32_t* const list = (uint32_t*)PTR(table, 0);
    int32_t i = 1;
    if (n > 1) {
        volatile uint32_t* pi = list + 1;
        do {
            const uint32_t key = *pi;
            int32_t j = i;
            if (j > 0) {
                volatile uint32_t* pj = pi - 1;
                do {
                    if (!g_sound3D_has_priority((void*)(uintptr_t)key, (void*)(uintptr_t)*pj)) break;
                    const uint32_t v = *pj;
                    pj--;
                    j--;
                    pj[2] = v;
                } while (j > 0);
            }
            ((volatile uint32_t*)list)[j] = key;
            pi++;
            i++;
        } while (i < n);
    }
}
static void fp_sort_3D(Footprint& f, uint8_t*, Edx, uint8_t* table) {
    const int32_t n = I32(table, 4);
    if (n > 1) f.add(PTR(table, 0), 4u * (uint32_t)n, "a class's run of the sound table");
}
PORT_FN(0x004746a0, "SoundManager::sort_3D_soundtable", SM_sort_3D_rw, fp_sort_3D)

// louder first; among two tire squeals the focus car's first
static uint8_t __cdecl sound3D_has_priority_rw(uint8_t* a, uint8_t* b) {
    const uint8_t louder = !(F32(b, 0x34) >= F32(a, 0x34)) ? 1 : 0;     // fld b; fcomp a; test ah,1
    if (I32(a, 0x24) != 3 || I32(b, 0x24) != 3) return louder;
    const int focus = WorldGetFocusCar();
    const uint8_t b_focus = I32(b, 0x10) == focus ? 1 : 0;
    if (I32(a, 0x10) == focus) return 1;
    if (b_focus) return 0;
    if (louder) return 1;
    return 0;
}
static void fp_has_priority(Footprint&, uint8_t*, uint8_t*) {}
PORT_FN(0x00474720, "sound3D_has_priority", sound3D_has_priority_rw, fp_has_priority)

static void __fastcall SM_sort_sounds_rw(uint8_t* self, Edx, const Listener*) {
    for (int32_t k = 0; k < 8; k++) {
        switch (k) {
        case 0: case 1: case 6: break;
        case 2: case 3: case 4: case 5: case 7: g_SM_sort_3D(self, 0, self + 0xc + 8 * k); break;
        default:                                            // (unreachable: k < 8)
            LogPanic(S(0x4f5b5c), S(0x4f5b4c), 0x29d, ((const char* (__cdecl*)(int))(uintptr_t)0x00471e40)(k));
        }
    }
}
static void fp_sort_sounds(Footprint& f, uint8_t* self, Edx, const Listener*) {
    if (uint8_t* list = PTR(self, 4)) f.add(list, 0x600, "the sound table");
}
PORT_FN(0x00474780, "SoundManager::sort_sounds", SM_sort_sounds_rw, fp_sort_sounds)

static void __fastcall SM_verify_soundtables_rw(uint8_t*, Edx) {}   // an empty loop over the tables (asserts compiled out)
static void fp_pure_this(Footprint& f, uint8_t*, Edx) { f.pure = true; }
PORT_FN(0x00474800, "SoundManager::verify_soundtables", SM_verify_soundtables_rw, fp_pure_this)

// FIX CANDIDATE: no bound on the table (384) nor on the class (0..7)
static void __fastcall SM_add_sound_rw(uint8_t* self, Edx, uint8_t* s) {
    MultiEnter(I32(self, 0x178), 0, 0);
    const int32_t count = I32(self, 8);
    const int32_t cls = I32(s, 0x24);
    uint8_t* at = PTR(self, 0xc + 8u * (uint32_t)cls);
    const int32_t idx = (int32_t)((uint32_t)at - U32(self, 4)) >> 2;
    crt_memmove(at + 4, at, (uint32_t)(count - idx) << 2);
    const int32_t c1 = cls + 1;
    if (c1 < 8) {
        uint32_t o = 0xc + 8u * (uint32_t)c1;
        int32_t n = 8 - c1;
        do {
            U32(self, o) += 4;
            o += 8;
        } while (--n != 0);
    }
    U32(PTR(self, 0xc + 8u * (uint32_t)cls), 0) = (uint32_t)(uintptr_t)s;
    I32(self, 0x10 + 8u * (uint32_t)cls) += 1;
    I32(self, 8) += 1;
    g_SM_verify_soundtables(self, 0);
    MultiLeave(I32(self, 0x178), 0, 0);
}
static void fp_add_sound(Footprint& f, uint8_t* self, Edx, uint8_t*) {
    f.add(self, 0x180, "the SoundManager");
    if (uint8_t* list = PTR(self, 4)) f.add(list, 0x604, "the sound table (and past it, when it's full)");
}
PORT_FN(0x00474810, "SoundManager::add_sound", SM_add_sound_rw, fp_add_sound)

static void __fastcall SM_Add_Sound_rw(uint8_t* self, Edx, uint8_t* s) { g_SM_add_sound(self, 0, s); }
PORT_FN(0x004748a0, "SoundManager::Add(Sound)", SM_Add_Sound_rw, fp_add_sound)
static void __fastcall SM_Add_Sound3D_rw(uint8_t* self, Edx, uint8_t* s) { g_SM_add_sound(self, 0, s); }
PORT_FN(0x004748b0, "SoundManager::Add(Sound3D)", SM_Add_Sound3D_rw, fp_add_sound)

static uint8_t __fastcall SM_soundclass_is_3d_rw(uint8_t*, Edx, int cls) {
    if (cls < 2) return 0;
    if (cls <= 5) return 1;
    if (cls == 7) return 1;
    return 0;
}
static void fp_is_3d(Footprint& f, uint8_t*, Edx, int) { f.pure = true; }
PORT_FN(0x004748c0, "SoundManager::soundclass_is_3d", SM_soundclass_is_3d_rw, fp_is_3d)

static void __fastcall SM_Remove_rw(uint8_t* self, Edx, uint8_t* s) {
    MultiEnter(I32(self, 0x178), 0, 0);
    const int32_t cls = I32(s, 0x24);
    const int32_t n = I32(self, 0x10 + 8u * (uint32_t)cls);
    uint8_t* found = 0;
    if (n > 0) {
        const uint8_t* p = PTR(self, 0xc + 8u * (uint32_t)cls);
        for (int32_t j = 0; j < n; j++, p += 4)
            if (U32(p, 0) == (uint32_t)(uintptr_t)s) {
                found = PTR(self, 0xc + 8u * (uint32_t)cls) + 4u * (uint32_t)j;
                break;
            }
    }
    if (found) {
        I32(self, 8) -= 1;
        I32(self, 0x10 + 8u * (uint32_t)cls) -= 1;
        const int32_t count = I32(self, 8);
        const int32_t idx = (int32_t)((uint32_t)found - U32(self, 4)) >> 2;
        const int32_t c1 = cls + 1;
        crt_memmove(found, found + 4, (uint32_t)(count - idx) << 2);
        if (c1 < 8) {
            uint32_t o = 0xc + 8u * (uint32_t)c1;
            int32_t k = 8 - c1;
            do {
                U32(self, o) -= 4;
                o += 8;
            } while (--k != 0);
        }
    } else {
        LogPanic(S(0x4f5b7c), s + 0x14);                    // "SoundManager::Remove(): Can't find sound (%s)."
    }
    MultiLeave(I32(self, 0x178), 0, 0);
}
static void fp_remove(Footprint& f, uint8_t* self, Edx, uint8_t*) { fp_add_sound(f, self, 0, 0); }
PORT_FN(0x004748e0, "SoundManager::Remove", SM_Remove_rw, fp_remove)

static void __fastcall SM_FlushAsync_rw(uint8_t* self, Edx) { g_SM_destroy_sounds(self, 0); }
PORT_FN(0x004749a0, "SoundManager::FlushAsync", SM_FlushAsync_rw, fp_destroy_sounds)

static void __fastcall SM_DestroyWhenStopped_rw(uint8_t* self, Edx, uint8_t* s) {
    MultiEnter(I32(self, 0x178), 0, 0);
    const uint32_t n = U32(self, 0x158);
    if (n < 32) {
        U32(self, 0xd8 + 4 * n) = (uint32_t)(uintptr_t)s;
        U32(self, 0x158) += 1;
    } else {
        LogPanic(S(0x4f5bac));                               // "...DestroyWhenStopped: increase N"
    }
    MultiLeave(I32(self, 0x178), 0, 0);
}
static void fp_destroy_when_stopped(Footprint& f, uint8_t* self, Edx, uint8_t*) { f.add(self + 0xd8, 0x84, "the destroy list and its count"); }
PORT_FN(0x004749b0, "SoundManager::DestroyWhenStopped", SM_DestroyWhenStopped_rw, fp_destroy_when_stopped)

// on the BG thread: the mixer's sound, placed by what the mixer can do (SoundLoc 0 hardware 3D, 1 hardware, 2 software)
static void* __fastcall SM_create_sound_rw(uint8_t* self, Edx, const char* name, int cls) {
    const uint8_t is3d = g_SM_soundclass_is_3d(self, 0, cls);
    const uint8_t* caps = vcall<const uint8_t*>(PTR(self, 0xd4), 4);
    if (B8(caps, 0) != 0 && is3d == 0) return vcall<void*>(PTR(self, 0xd4), 0x10, name, 2);
    int32_t loc;
    if (I32(caps, 4) != 0 && is3d != 0) loc = 0;
    else loc = I32(caps, 8) == 0 ? 2 : 1;
    void* r = vcall<void*>(PTR(self, 0xd4), 0x10, name, loc);
    if (loc == 0 && r == 0) {
        LogReport(S(0x4f5bdc));                              // "Creating 3d sound failed.  Supposed to be impossible."
        r = vcall<void*>(PTR(self, 0xd4), 0x10, name, 1);
    }
    return r;
}
static void fp_create_sound(Footprint& f, uint8_t*, Edx, const char*, int) { f.replay_only = "the mixer creates a sound (allocates, loads its resource)"; }
PORT_FN(0x00474a10, "SoundManager::create_sound", SM_create_sound_rw, fp_create_sound)

// on another thread: leave the request for update() and sleep until it's done
static void* __fastcall SM_fg_create_sound_rw(uint8_t* self, Edx, const char* name, int cls) {
    I32(self, 0x168) = cls;
    B8(self, 0x170) = 0;
    U32(self, 0x164) = (uint32_t)(uintptr_t)name;
    do TaskSleep(25);
    while (B8(self, 0x170) == 0);
    void* r = PTR(self, 0x16c);
    MultiEnter(I32(self, 0x178), 0, 0);
    U32(self, 0x164) = 0;
    U32(self, 0x16c) = 0;
    MultiLeave(I32(self, 0x178), 0, 0);
    return r;
}
static void fp_fg_create(Footprint& f, uint8_t*, Edx, const char*, int) { f.replay_only = "asks the BG thread to create a sound and sleeps until it has"; }
PORT_FN(0x00474ab0, "SoundManager::fg_create_sound", SM_fg_create_sound_rw, fp_fg_create)

static void* __fastcall SM_Create_rw(uint8_t* self, Edx, const char* name, int cls) {
    if (TaskGetID() == I32(self, 0x17c)) return g_SM_create_sound(self, 0, name, cls);
    return g_SM_fg_create_sound(self, 0, name, cls);
}
PORT_FN(0x00474b30, "SoundManager::Create", SM_Create_rw, fp_create_sound)
static void* __fastcall SM_Create3D_rw(uint8_t* self, Edx, const char* name, int cls) {
    if (TaskGetID() == I32(self, 0x17c)) return g_SM_create_sound(self, 0, name, cls);
    return g_SM_fg_create_sound(self, 0, name, cls);
}
PORT_FN(0x00474b60, "SoundManager::Create3D", SM_Create3D_rw, fp_create_sound)

// FIX CANDIDATE: the hand-off holds one (a second thread waits for the first's to be taken)
static void __fastcall SM_Destroy_rw(uint8_t* self, Edx, uint8_t* is) {
    if (TaskGetID() == I32(self, 0x17c)) {
        if (is) vcall<void*>(is, 0, 1u);
        return;
    }
    while (I32(self, 0x160) == 1) TaskSleep(50);
    MultiEnter(I32(self, 0x178), 0, 0);
    U32(self, 0x15c + 4u * U32(self, 0x160)) = (uint32_t)(uintptr_t)is;
    U32(self, 0x160) += 1;
    MultiLeave(I32(self, 0x178), 0, 0);
    do TaskSleep(50);
    while (I32(self, 0x160) != 0);
}
static void fp_destroy(Footprint& f, uint8_t*, Edx, uint8_t*) { f.replay_only = "deletes the mixer's sound, or hands it to the BG thread and sleeps until it's gone"; }
PORT_FN(0x00474b90, "SoundManager::Destroy", SM_Destroy_rw, fp_destroy)

static void* __fastcall listener_info_ctor_rw(uint8_t* self, Edx) { return self; }
PORT_FN(0x00474c30, "SoundManager::listener_info::listener_info", listener_info_ctor_rw, fp_nothing)

// ==== soundres.obj ========================================================================================================
static void __cdecl soundres_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x004771f0, "$E1(soundres.obj)", soundres_E1_rw, fp_static_init)
static void __cdecl soundres_E2_rw() { g_E1_soundres(); }
PORT_FN(0x004771e0, "$E2(soundres.obj)", soundres_E2_rw, fp_static_init)

// FIX CANDIDATE: an old-format resource shorter than 0x800 bytes gets a negative length
static void* __cdecl SoundResourceGet_rw(const char* name) {
    uint32_t version;
    uint8_t b;
    uint8_t* r = (uint8_t*)ResourceGet(name, 0x53465830, &version, 0, &b, 0);   // 'SFX0'
    if (r) {
        if (version == 0) {
            LogReport(S(0x4f65d8), name);                    // "%s is in old format--truncating"
            I32(r, 0) -= 0x800;
        }
        return r;
    }
    LogPanic(S(0x4f65f8), name);                             // "Couldn't find sound %s"
    return 0;
}
static void fp_res_get(Footprint& f, const char*) { f.replay_only = "loads a resource"; }
PORT_FN(0x00477200, "SoundResourceGet", SoundResourceGet_rw, fp_res_get)

static void __cdecl SoundResourceForget_rw(void* r) { ResourceForget(r); }
static void fp_res_forget(Footprint& f, void*) { f.replay_only = "releases a resource"; }
PORT_FN(0x00477270, "SoundResourceForget", SoundResourceForget_rw, fp_res_forget)
