// snd_mix.cpp -- M3 sound stage, step S1, group C: the mixers, rewritten faithfully. sound:mixer.obj (the mixer list
// and the [SOUND] options: MixerBegin / MixerEnd / MixerGet..., the volume and quality, SoundLocString, the NullMixer),
// softmxr.obj (the SoftMixer and its SoftSounds: the game's own software mixer, and music_test), fastmix.obj (the
// inner loops: fastmix / nullmix into an int32 stereo accumulator, fastout / fastout_8 out of it) and wave.obj (the ONE
// looping DirectSound buffer the SoftMixer streams into: WaveBegin, the primary and fallback secondary buffers,
// WaveGrab / WaveRelease, WaveEnd -- the only code in the game that calls DirectSound).
//
// How it runs. SoundBegin -> MixerBegin(1): the list is [NullMixer, SoftMixer if its probe
// works] (MixerAddMixer runs a mixer's Begin and End once, on the main thread). The SoundManager then runs the chosen
// mixer's Begin again and calls its Update from its BGHook, so SoftMixer::Update -- WaveGrab, mix_bytes into the
// locked memory, WaveRelease -- runs on the BGTask timer thread every 16 ms, the physics thread. SoftMixer::Begin:
// SOUND/fidelity (default 1) >= 1 tries WaveBegin(3,1) (22050 Hz 16-bit stereo) first, else WaveBegin(1,1) (22050 Hz
// 8-bit stereo, fastout_8). WaveBegin: DirectSoundCreate through the game's import thunk (0x4cccec), then a
// DSSCL_WRITEPRIMARY primary buffer in that format, else a 16 KB DSBCAPS_LOCSOFTWARE secondary.
//
// The mixing (mix_bytes): every sound's gains and step are set up once per call (setup_channel: gains from
// effects_volume * volume and the pan, ftol'd to 16.16 fixed point; the step from the frequency ratio, which
// SetFrequency clamps to 0.02..16), then 128-frame blocks: the int32 accumulator cleared, each playing sound checked for
// its end (setup_run: a one-shot stops, a loop steps back by its length) and mixed in (fastmix: 16-bit mono source,
// 16.16 resampling with no interpolation, sample * gain >> 8 per channel; nullmix when muted: the position moves on,
// nothing is added), then clamped out (fastout: >> 8 to S16; fastout_8: + 0x800000 >> 16 to U8).
//
// Faithful (docs/PORTING.md): every call in the original's order with its arguments -- game functions by their v1.0
// address (this group's own too, so a hooked rewrite runs), virtual calls through the vtable, DirectSound as the audio
// core's functions (hook/audio_core.h, step S2: each the call the original makes through the object's vtable slot, with
// its arguments, on the same object -- the core's handles are the COM facade's objects), the game's CRT (strncmp,
// atexit) at its own address, ftol as x87_ftol. Floats the original moves with integer instructions
// (MixerSetVolume, SetVolume / SetFrequency / SetPanning, the effects volume handed to MixerSetVolume) are taken and
// copied as bits; compares done on a float's bits stay on the bits. Globals are read and written through volatile at
// the points the original touches them.
//
// Fixes (docs/PORTING.md, "Fixes"; each marked FIX:, VP_FIX; test/world_snd_mix.cpp built with /DFIX_TESTS tests them):
//   * a sound's reads stay inside its resource: a 128-frame block past the end of the data read up to 4 KB on, which
//     the game's own .sfx resources cover with 4096 bytes of padding (zeros after a one-shot, the start again after a
//     loop) and a mod's resource may not. The SoftSound constructor notes how far its resource goes (its TOC entry's
//     size, found in the resource sets); do_run mixes a block that would read past it with fastmix's own arithmetic,
//     taking what the missing padding would hold: silence for a one-shot, the loop's start again for a loop. A block
//     inside the resource -- every block of a stock sound -- is mixed by fastmix exactly as before.
//   * setup_run: a loop shorter than a block's step (or of no length) stayed past its end, one length back per block,
//     and read further on each; it now steps back into the loop (the position modulo the length; a zero-length loop
//     to its start). A loop the original brings back in one step is unchanged.
//   * SoftSound: a missing resource (SoundResourceGet -> 0) makes an empty, silent sound instead of a crash, and its
//     destructor lets go of nothing.
//   * SoftMixer::CreateSound: a sound whose allocation failed isn't added to the bag (mix_bytes read through it).
//   * the sounds' bag is lifted M1-style: SoftMixer::Begin takes its capacity from the original's operand (0x474d79,
//     stock 256), which M1 raises.
// Not fixed: see the FIX CANDIDATEs left (MixerGet / SoundLocString / MixerGetQualityString: only the game's own menus
// call them in range; the WAVEFORMATEX a SoftSound ignores; fastmix's count and WaveGrab's Lock at the ring's end, each
// with its reason).
//
// Threads: SoftMixer::Update and everything under it (WaveGrab / WaveRelease, mix_bytes, setup_channel / setup_run /
// do_run, the fast* loops) and the SoftSound methods the SoundManager calls run on the BG thread (= the physics
// thread); the probe (MixerAddMixer -> SoftMixer::Begin / End -> WaveBegin / WaveEnd) and the options run on the main
// thread. None of the sound library's statics are among the automatically saved physics / AI statics, so every
// footprint lists the statics, the sounds and the buffers its function writes. The functions that create or release
// DirectSound objects, allocate or free, register an atexit handler or load resources are replay_only. Nothing here
// draws a random number.
//
// Shadow contract (dsound_sdl.cpp): GetStatus / GetCurrentPosition / GetCaps / Lock are inputs, Play / Stop / SetFormat
// / Unlock / Restore outputs, recorded by the audio core whichever way the call comes (the original's pass through the
// COM facade, the rewrite's directly), so the two passes' records compare as before. SoftMixer::Update locks
// (WaveGrab), mixes into the locked memory and unlocks (WaveRelease) inside one call: its footprint can't name the
// locked memory (Lock hands it out during the call), so that memory is left to the Unlock record's hash of the bytes
// written (both passes' Locks hand out the same pointers).
//
// Step S2 (PORT_FN_AUDIO): WaveBegin, create_primary_buffer, create_secondary_buffer, WaveEnd, WaveGrab and WaveRelease
// call the audio core, so they need the SDL audio ([platform] sdl=1, audio=sdl): with the game's own DirectSound they
// stay original (port_install). setup_wf and max_data_from_cursors don't touch DirectSound.
//
// Not rewritten (too short to hook: the jump is 5 bytes): NullMixer::GetCaps (lea eax,[ecx+4]; ret -- 4 bytes),
// NullMixer::IsNullMixer / Begin, IMixer::IsNullMixer, SoftSound::Ok / Can3D (mov al,1 or xor al,al; ret -- 3 bytes),
// NullMixer::Update (ret 4 -- 3 bytes), NullMixer::End and the $E11 / $E12 / $E13 (mixer.obj) and $E8 (softmxr.obj)
// atexit destructors (a bare ret -- 1 byte). ISound's scalar deleting destructor (0x4755b0) is unreachable: only
// ISound's own vtable (0x4dd918) holds it, and the objects that ever carry that vtable are a SoftSound in the last
// instruction of its destructor (its own deleting destructor frees it directly) and the dead hardware mixer's
// DSecondarySound (never constructed).
#include <intrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "audio_core.h"

namespace {

typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

// ---- memory ----------------------------------------------------------------------------------------------------------
#define SG8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define SG16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define SG32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define SGI32(a) (*(volatile int32_t*)(uintptr_t)(a))
#define SS(a) ((const char*)(uintptr_t)(a))                // one of the game's own strings
#define SFN(T, a) ((T)(uintptr_t)(a))
// a virtual call through the object's vtable (byte offset), __thiscall received as __fastcall
#define SVF(obj, off, R, ...) ((R(__fastcall*)(void*, Edx, ##__VA_ARGS__))((*(void* const* const*)(obj))[(off) / 4]))

static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

enum : uint32_t {
    // mixer.obj
    M_LIST = 0x578a60,             // IMixer* [6], then the count and the default: MixerBegin clears all 8 dwords
    M_COUNT = 0x578a78,
    M_DEFAULT = 0x578a7c,
    M_VOLUME = 0x4f5930,           // float: [SOUND] effects_volume, 0..1
    M_QUALITY = 0x4f5934,          // MixerQuality 0..2
    M_SECTION = 0x4dd7bc,          // const char*: "SOUND"
    M_NULLNAME = 0x578a40,         // Xlator "Sound:NoSound" (NullMixer::GetName)
    M_QX0 = 0x578a80, M_QX1 = 0x578a30, M_QX2 = 0x578a50,   // MixerGetQualityString's local static Xlators
    M_QGUARD = 0x578a90,           // their construction bits (1, 2, 4)
    X_COOKIE = 0x4eb108,           // Xlator::g_cookie
    // softmxr.obj
    SM_NAME = 0x578b20,            // SoftMixer::GetName's local static Xlator, guard bit 1 at 0x578b3c
    SM_NAMEGUARD = 0x578b3c,
    SM_CAPS = 0x578b30,            // MixerCaps: uint8 + 2 dwords
    SM_SECTION = 0x4dd868,         // const char*: "SOUND"
    // wave.obj
    W_DS = 0x578c90,               // IDirectSound*
    W_N1 = 0x578c94,               // Lock's results: first length, second pointer, second length, first pointer
    W_N2 = 0x578c98,
    W_BUF = 0x578c9c,              // IDirectSoundBuffer*: the primary, or the 16 KB secondary
    W_SIZE = 0x578ca0,             // the buffer's size S
    W_WFX = 0x578ca8,              // WAVEFORMATEX (18 bytes)
    W_RATE = 0x578cac,
    W_AVG = 0x578cb0,
    W_ALIGN = 0x578cb4,
    W_BITS = 0x578cb6,
    W_P1 = 0x578cc0,
    W_P2 = 0x578cc4,
    W_POS = 0x578cc8,              // the write offset (can equal S after a Lock that ends at the buffer's end)
    W_OK = 0x4f638c,               // uint8: the last WaveGrab locked (set and cleared, never read)
    W_RESYNC = 0x4f6390,           // uint8: take the write cursor as the write offset on the next WaveGrab
    // fastmix.obj
    F_FIRST = 0x4f65d4,            // uint8: cleared by the first fastmix (never read otherwise)
    DSERR_BUFFERLOST = 0x88780096,
};

// ---- the objects -------------------------------------------------------------------------------------------------------
struct MixChannel {                // mix_channel_data, SoftSound+0x2c
    uint32_t src;                  // +0x00 const int16_t*: the current sample
    int32_t lg, rg;                // +0x04 +0x08 gains into the accumulator's first / second channel (x 2^16)
    uint32_t step;                 // +0x0c 16.16
    uint32_t frac;                 // +0x10 the position's fraction (low 16 bits)
};
static_assert(sizeof(MixChannel) == 0x14, "MixChannel");
struct Bag {                       // BagBase
    void** items;                  // +0x00 MemAlloc(max * 4)
    const char* name;              // +0x04
    int32_t count;                 // +0x08
    int32_t max;                   // +0x0c
    uint8_t panic;                 // +0x10 LogPanic when full / not found
    uint8_t _11[3];
};
static_assert(sizeof(Bag) == 0x14, "Bag");
struct SoftMixerO {
    const void* vtbl;              // +0x00 0x4dd8c0
    uint8_t eight_bit;             // +0x04 WaveBegin(1,1): fastout_8
    uint8_t ok;                    // +0x05 the constructor's 1 (SoftMixerCreateMixers adds it only if set)
    uint8_t _6[2];
    Bag* sounds;                   // +0x08 BagBase("SoftSound:sounds", 256)
};
static_assert(sizeof(SoftMixerO) == 0xc, "SoftMixer");
struct SoftSoundO {
    const void* vtbl;              // +0x00 0x4dd8e0 (ISound's 0x4dd918 after the destructor)
    uint32_t len;                  // +0x04 samples: the resource's byte length >> 1
    uint32_t start;                // +0x08 the resource + 0x16: the first sample
    SoftMixerO* mixer;             // +0x0c
    uint8_t* res;                  // +0x10 SoundResource: dword byte length, 18-byte WAVEFORMATEX (ignored), data
    uint8_t looped;                // +0x14
    uint8_t playing;               // +0x15
    uint8_t muted;                 // +0x16
    uint8_t _17;
    uint32_t vol;                  // +0x18 float, 0..1 (SetVolume)
    uint32_t pan;                  // +0x1c float (SetPanning: unclamped)
    uint32_t ratio;                // +0x20 float, 0.02..16 (SetFrequency)
    uint32_t _24, _28;
    MixChannel ch;                 // +0x2c
};
static_assert(sizeof(SoftSoundO) == 0x40, "SoftSound");
static_assert(offsetof(SoftSoundO, ch) == 0x2c, "SoftSound.ch");

// a sample count / a location, as their own types so the offline fuzzer can give them sensible values (an int32 on the
// stack, like the originals' parameters)
enum SndCount : int32_t {};
enum SndLoc : int32_t {};

}  // namespace

#ifdef VP_FUZZ
// fastout / fastout_8 / nullmix loop (unsigned)count times: 0 is 2^32 (never in the game: mix_bytes passes 1..128)
template <> struct Arg<SndCount> {
    static void make(Arena&, SndCount& x, SndCount& y, uintptr_t*, int) { x = y = (SndCount)(1 + fuzz_rand() % 48); }
};
template <> struct Arg<SndLoc> {
    static void make(Arena&, SndLoc& x, SndLoc& y, uintptr_t*, int) { x = y = (SndLoc)(fuzz_rand() % 4); }
};
#endif

namespace {

// ---- the game's functions, by their v1.0 address --------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
#define rcfunc_is_internal SFN(Void_t, 0x00410cc0)
#define MemAlloc_o SFN(void*(__cdecl*)(int), 0x004140e0)
#define op_delete_o SFN(void(__cdecl*)(void*), 0x00414390)
#define LogReport_o SFN(void(__cdecl*)(const char*, ...), 0x00411150)
#define LogPanic_o SFN(void(__cdecl*)(const char*, ...), 0x004112b0)
#define TaskSleep_o SFN(void(__cdecl*)(int), 0x00414de0)
#define Win32GetWindow_o SFN(void*(__cdecl*)(), 0x00412be0)
#define Win32Idle_o SFN(Void_t, 0x00412bf0)
#define OptionsGetI_o SFN(void(__cdecl*)(const char*, const char*, int32_t*), 0x004713e0)
#define OptionsGetF_o SFN(void(__cdecl*)(const char*, const char*, void*), 0x00471350)
#define OptionsSetI_o SFN(void(__cdecl*)(const char*, const char*, int32_t), 0x00471430)
#define OptionsSetF_o SFN(void(__cdecl*)(const char*, const char*, uint32_t), 0x004713a0)   // the float as bits
#define Xlator_ctor_o SFN(void*(__fastcall*)(void*, Edx, const char*), 0x0041af80)
#define Xlator_xlate_o SFN(void(__fastcall*)(void*, Edx), 0x0041afb0)
#define crt_atexit SFN(int(__cdecl*)(uint32_t), 0x004ceff0)
#define crt_strncmp SFN(int(__cdecl*)(const char*, const char*, uint32_t), 0x004ce0e0)
#define Bag_ctor_o SFN(void*(__fastcall*)(void*, Edx, const char*, int32_t), 0x004d97c0)
#define Bag_dtor_o SFN(void(__fastcall*)(void*, Edx), 0x004d97f0)
#define Bag_add_o SFN(uint8_t(__fastcall*)(void*, Edx, void*), 0x004d9810)
#define Bag_remove_o SFN(void(__fastcall*)(void*, Edx, void*), 0x004d9860)
#define SoundResourceGet_o SFN(uint8_t*(__cdecl*)(const char*), 0x00477200)
#define SoundResourceForget_o SFN(void(__cdecl*)(void*), 0x00477270)
#define SoundBegin_o SFN(uint8_t(__cdecl*)(), 0x00471cb0)
#define SoundEnd_o SFN(Void_t, 0x00471d90)
#define KeyDisableAll_o SFN(Void_t, 0x00413d90)
#define ScanHit_o SFN(uint8_t(__cdecl*)(uint32_t), 0x00413100)        // unsigned char: the low byte counts
#define ScanDown_o SFN(uint8_t(__cdecl*)(uint32_t), 0x004130e0)
#define dsounderr2str_o SFN(const char*(__cdecl*)(int32_t), 0x004756b0)
#define directsound_create_mixers_o SFN(Void_t, 0x00476750)
#define MultiEnter_o SFN(void(__cdecl*)(int32_t, const char*, int32_t), 0x00415180)
#define MultiLeave_o SFN(void(__cdecl*)(int32_t, const char*, int32_t), 0x004151d0)
// this group, by address
#define MixerSetVolume_o SFN(void(__cdecl*)(uint32_t), 0x004739e0)
#define MixerSetQuality_o SFN(void(__cdecl*)(int32_t), 0x00473a20)
#define MixerGetVolume_o SFN(float(__cdecl*)(), 0x004739d0)
#define MixerAddMixer_o SFN(void(__cdecl*)(void*), 0x00473900)
#define SoftMixerCreateMixers_o SFN(Void_t, 0x00474c60)
#define SoftMixer_ctor_o SFN(void*(__fastcall*)(void*, Edx), 0x00474d20)
#define SoftMixer_Begin_o SFN(uint8_t(__fastcall*)(void*, Edx), 0x00474d40)
#define SoftMixer_End_o SFN(void(__fastcall*)(void*, Edx), 0x00474e00)
#define SoftMixer_CreateSound_o SFN(void*(__fastcall*)(void*, Edx, const char*, int32_t), 0x00474e30)
#define SoftMixer_RemoveSound_o SFN(void(__fastcall*)(void*, Edx, void*), 0x00474e70)
#define SoftMixer_Update_o SFN(void(__fastcall*)(void*, Edx, const void*), 0x00474e80)
#define setup_channel_o SFN(void(__fastcall*)(void*, Edx), 0x00474f20)
#define setup_run_o SFN(void(__fastcall*)(void*, Edx), 0x00474fd0)
#define do_run_o SFN(void(__fastcall*)(void*, Edx, void*, int32_t), 0x00475000)
#define mix_bytes_o SFN(void(__fastcall*)(void*, Edx, uint8_t*, uint32_t), 0x00475040)
#define SoftSound_ctor_o SFN(void*(__fastcall*)(void*, Edx, const char*, void*), 0x00475160)
#define SoftSound_dtor_o SFN(void(__fastcall*)(void*, Edx), 0x004751b0)
#define WaveBegin_o SFN(uint8_t(__cdecl*)(int32_t, int32_t), 0x00476840)
#define create_primary_buffer_o SFN(void*(__cdecl*)(int32_t, int32_t), 0x00476930)
#define setup_wf_o SFN(void(__cdecl*)(int32_t, int32_t), 0x00476b80)
#define create_secondary_buffer_o SFN(void*(__cdecl*)(int32_t, int32_t), 0x00476c20)
#define WaveEnd_o SFN(Void_t, 0x00476d30)
#define WaveGrab_o SFN(uint8_t(__cdecl*)(uint8_t**, uint32_t*, uint8_t**, uint32_t*), 0x00476d70)
#define max_data_from_cursors_o SFN(uint32_t(__cdecl*)(uint32_t, uint32_t), 0x00476ff0)
#define WaveRelease_o SFN(Void_t, 0x00477010)
#define fastmix_o SFN(void(__cdecl*)(void*, void*, int32_t), 0x00477090)
#define nullmix_o SFN(void(__cdecl*)(void*, void*, int32_t), 0x00477100)
#define fastout_o SFN(void(__cdecl*)(void*, void*, int32_t), 0x00477140)
#define fastout_8_o SFN(void(__cdecl*)(void*, void*, int32_t), 0x00477190)

// DirectSound: the audio core (audio_core.h; M3 step S2), called directly -- its handles are the COM objects the
// original code calls through their vtables (the facade in dsound_sdl.cpp), so W_DS / W_BUF hold the same objects
// whichever code made them. Each call is the one the original makes through the vtable slot, with its arguments.
#define DEV(a) ((audio::Device)(uintptr_t)(a))              // W_DS's value as the core's handle
#define BUF(a) ((audio::Buffer)(uintptr_t)(a))              // W_BUF's
static __forceinline int32_t buf_play(audio::Buffer b) { return audio::play(b, 0, 0, 1); }
static __forceinline int32_t buf_restore(audio::Buffer b) { return audio::restore(b); }
static __forceinline int32_t buf_lock(audio::Buffer b, uint32_t at, uint32_t n) {
    return audio::lock(b, at, n, (void**)(uintptr_t)W_P1, (unsigned long*)(uintptr_t)W_N1, (void**)(uintptr_t)W_P2,
                       (unsigned long*)(uintptr_t)W_N2, 0);
}
static __forceinline int32_t buf_unlock(audio::Buffer b) {
    return audio::unlock(b, (void*)(uintptr_t)SG32(W_P1), SG32(W_N1), (void*)(uintptr_t)SG32(W_P2), SG32(W_N2));
}
#define PTR(a) ((void*)(uintptr_t)(a))

static void fp_static_init(Footprint& f) { f.replay_only = "a static initialiser (runs once, from the CRT's _initterm)"; }
static void fp_none(Footprint&) {}                     // writes nothing (reads statics)

// ---- fixes (docs/PORTING.md, "Fixes"): how far a sound's resource goes ------------------------------------------------
// FIX: (do_run) a block of 128 frames reads up to 128 x the step (16 at most: 2032 samples, 4064 bytes) past its
// position, and the end of the data is only checked between blocks, so the last block of a sound reads past the data.
// The game's .sfx resources carry 4096 bytes after the data for it (every one of the 35 in v1.0: zeros after the 26
// one-shots, the data's start again after the 9 loops), and the original mixes what it finds there; a mod's resource
// without that padding was read past its end (a crash where it ended at the end of its memory, noise otherwise).
// So each sound notes where its resource ends: the resource's TOC entry holds its payload's size (the data AND its
// padding), found by the resource pointer in the resource sets when the sound is made. The records are DLL memory,
// keyed by the sound; one is trusted only for the resource it was made with. A sound made by the original's constructor
// has none and mixes as the original does. Sounds are made and mixed on both threads: a small lock.
enum : uint32_t { R_MULTI = 0x4eae3c, R_SETS = 0x4eae40 };   // res.obj: the "Resource" Multi, the set list
struct SndExtent { const void* sound; uint32_t res, lim; };
static SndExtent* g_ext;
static int g_next, g_cap_ext;
static volatile long g_ext_lock;
static volatile long g_ext_short;                  // a sound whose resource ends before its data does (a malformed one)
static void ext_lock() { while (_InterlockedCompareExchange(&g_ext_lock, 1, 0)) _mm_pause(); }
static void ext_unlock() { _InterlockedExchange(&g_ext_lock, 0); }
// the bytes from `res` that belong to its resource (its TOC entry's size), if a set holds it. The set list and
// the TOCs as res.obj keeps them: a node {name[16], next +0x10, toc +0x14, ..., count +0x1c}, 0x24-byte entries {...,
// size +0x18 (bit 31: fetched), ..., data +0x20: the payload's 8-byte head, so the resource is data + 8}.
static bool res_readable(const uint8_t* res, uint32_t* bytes) {
    const int32_t multi = SGI32(R_MULTI);
    if (!multi) return false;                      // (no resource system: no sets)
    bool found = false;
    MultiEnter_o(multi, 0, 0);
    for (const uint8_t* n = (const uint8_t*)(uintptr_t)SG32(R_SETS); n && !found;
         n = *(const uint8_t* const volatile*)(n + 0x10)) {
        const uint8_t* toc = *(const uint8_t* const volatile*)(n + 0x14);
        const uint32_t cnt = *(const volatile uint32_t*)(n + 0x1c);
        for (uint32_t i = 0; toc && i < cnt; i++) {
            const uint8_t* e = toc + i * 0x24;
            if (*(const volatile uint32_t*)(e + 0x20) + 8 == (uint32_t)(uintptr_t)res) {
                *bytes = *(const volatile uint32_t*)(e + 0x18) & 0x7fffffff;
                found = true;
                break;
            }
        }
    }
    MultiLeave_o(multi, 0, 0);
    return found;
}
// the SoftSound constructor: its resource's end (no resource: nothing to read; one in no set: the data's end)
static void note_extent(const void* sound, const uint8_t* res, uint32_t start, uint32_t len) {
    uint32_t lim = start;
    if (res) {
        uint32_t n;
        const uint32_t dend = start + len + len;
        lim = res_readable(res, &n) ? (uint32_t)(uintptr_t)res + n : dend >= start ? dend : start;
        if (lim < start || lim - start < len + len) _InterlockedExchange(&g_ext_short, 1);
    }
    ext_lock();
    int i = 0;
    while (i < g_next && g_ext[i].sound != sound) i++;           // the sound's old record (its memory reused), replaced
    if (i == g_next) {
        if (g_next == g_cap_ext) {
            const int cap = g_cap_ext ? g_cap_ext * 2 : 64;
            SndExtent* t = (SndExtent*)realloc(g_ext, cap * sizeof *t);
            if (!t) { ext_unlock(); return; }      // (no memory: the sound mixes as the original does)
            g_ext = t;
            g_cap_ext = cap;
        }
        g_next++;
    }
    g_ext[i].sound = sound;
    g_ext[i].res = (uint32_t)(uintptr_t)res;
    g_ext[i].lim = lim;
    ext_unlock();
}
static bool get_extent(const void* sound, uint32_t res, uint32_t* lim) {
    bool found = false;
    ext_lock();
    for (int i = 0; i < g_next; i++)
        if (g_ext[i].sound == sound) {
            found = g_ext[i].res == res;
            *lim = g_ext[i].lim;
            break;
        }
    ext_unlock();
    return found;
}
static void forget_extent(const void* sound) {
    ext_lock();
    for (int i = 0; i < g_next; i++)
        if (g_ext[i].sound == sound) {
            g_ext[i] = g_ext[--g_next];
            break;
        }
    ext_unlock();
}

}  // namespace

// ======================================================================================================================
// mixer.obj
// ======================================================================================================================
static void __cdecl mixer_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00473720, "$E1(mixer.obj)", mixer_E1_rw, fp_static_init)
static void __cdecl mixer_E2_rw() { SFN(Void_t, 0x00473720)(); }
PORT_FN(0x00473710, "$E2(mixer.obj)", mixer_E2_rw, fp_static_init)
static void __cdecl mixer_E7_rw() { Xlator_ctor_o(PTR(M_NULLNAME), 0, SS(0x004f5970)); }   // "Sound:NoSound"
PORT_FN(0x00473740, "$E7(mixer.obj)", mixer_E7_rw, fp_static_init)
static void __cdecl mixer_E8_rw() { SFN(Void_t, 0x00473740)(); }
PORT_FN(0x00473730, "$E8(mixer.obj)", mixer_E8_rw, fp_static_init)

// MixerBegin(create): the mixer list (create) and the [SOUND] options. The list's clear is 8 dwords: the 6 slots, the
// count and the default mixer.
static void __cdecl MixerBegin_rw(uint8_t create) {
    if (create) {
        for (uint32_t i = 0; i < 8; i++) SG32(M_LIST + 4 * i) = 0;
        uint32_t* nm = (uint32_t*)MemAlloc_o(0x10);
        if (nm) {                                      // NullMixer::NullMixer, inlined: vtable and zero MixerCaps
            ((volatile uint32_t*)nm)[0] = 0x004dd7c8;
            ((volatile uint32_t*)nm)[1] = 0;
            ((volatile uint32_t*)nm)[2] = 0;
            ((volatile uint32_t*)nm)[3] = 0;
            SG32(M_LIST + 4 * SG32(M_COUNT)) = (uint32_t)(uintptr_t)nm;
        } else {
            SG32(M_LIST + 4 * SG32(M_COUNT)) = 0;
        }
        SG32(M_COUNT) = SG32(M_COUNT) + 1;
        directsound_create_mixers_o();
        SoftMixerCreateMixers_o();
    }
    const char* sec = *(const char* const volatile*)(uintptr_t)M_SECTION;
    OptionsGetI_o(sec, SS(0x004f598c), (int32_t*)PTR(M_DEFAULT));     // "mixer"
    if (SGI32(M_DEFAULT) < 0 || !(SGI32(M_COUNT) > SGI32(M_DEFAULT))) SG32(M_DEFAULT) = 0;
    OptionsGetF_o(sec, SS(0x004f5994), PTR(M_VOLUME));                // "effects_volume"
    MixerSetVolume_o(SG32(M_VOLUME));
    OptionsGetI_o(sec, SS(0x004f59a4), (int32_t*)PTR(M_QUALITY));     // "quality"
    MixerSetQuality_o(SGI32(M_QUALITY));
}
static void fp_mixer_begin(Footprint& f, uint8_t) {
    f.replay_only = "it reads the options (OptionsGet adds a missing key); with create, it allocates the NullMixer and "
                    "probes the SoftMixer (DirectSoundCreate)";
}
PORT_FN(0x00473750, "MixerBegin", MixerBegin_rw, fp_mixer_begin)

static int32_t __cdecl MixerCount_rw() { return SGI32(M_COUNT); }
PORT_FN(0x00473840, "MixerCount", MixerCount_rw, fp_none)
// FIX CANDIDATE: no bounds check -- an index outside 0..count-1 reads past the list (the count, the default, the
// quality Xlators); the callers pass the clamped default or a chooser's index
static uint32_t __cdecl MixerGet_rw(int32_t i) { return SG32(M_LIST + (uint32_t)i * 4); }
static void fp_mixer_get(Footprint&, int32_t) {}
PORT_FN(0x00473850, "MixerGet", MixerGet_rw, fp_mixer_get)
static void __cdecl MixerSetDefault_rw(int32_t i) { SGI32(M_DEFAULT) = i; }
static void fp_mixer_set_default(Footprint& f, int32_t) { f.add(PTR(M_DEFAULT), 4, "default mixer (0x578a7c)"); }
PORT_FN(0x00473860, "MixerSetDefault", MixerSetDefault_rw, fp_mixer_set_default)
static int32_t __cdecl MixerGetDefault_rw() { return SGI32(M_DEFAULT); }
PORT_FN(0x00473870, "MixerGetDefault", MixerGetDefault_rw, fp_none)

// MixerEnd(free): frees the mixers with a plain operator delete (no destructor, and the count stays), then writes the
// options back
static void __cdecl MixerEnd_rw(uint8_t del) {
    if (del) {
        for (int32_t i = 0; SGI32(M_COUNT) > i; i++) op_delete_o(PTR(SG32(M_LIST + 4 * (uint32_t)i)));
    }
    const int32_t def = SGI32(M_DEFAULT);
    const char* sec = *(const char* const volatile*)(uintptr_t)M_SECTION;
    OptionsSetI_o(sec, SS(0x004f59ac), def);                          // "mixer"
    OptionsSetF_o(sec, SS(0x004f59b4), SG32(M_VOLUME));               // "effects_volume"
    OptionsSetI_o(sec, SS(0x004f59c4), SGI32(M_QUALITY));             // "quality"
}
static void fp_mixer_end(Footprint& f, uint8_t) { f.replay_only = "it writes the options (OptionsSet) and frees the mixers"; }
PORT_FN(0x00473880, "MixerEnd", MixerEnd_rw, fp_mixer_end)

// MixerAddMixer: a mixer whose name starts "Dream 9407," is tossed; past 6 mixers it's tossed; else it's probed (Begin,
// then End) and kept if Begin worked. GetName is called through the pointer read once from the vtable.
static void __cdecl MixerAddMixer_rw(void* m) {
    void* const* vt = *(void* const* const*)m;
    typedef const char*(__fastcall * Name_t)(void*, Edx);
    const Name_t name = (Name_t)vt[0];
    const char* n = name(m, 0);
    if (crt_strncmp(n, SS(0x004f59cc), 0xb) == 0) {                   // "Dream 9407,"
        LogReport_o(SS(0x004f59d8), name(m, 0));                       // "tossing bogus mixer: %s"
        op_delete_o(m);
        return;
    }
    if (SG32(M_COUNT) >= 6) {
        LogReport_o(SS(0x004f59f0), name(m, 0));                       // "too many sound devices: %s"
        op_delete_o(m);
        return;
    }
    if (((uint8_t(__fastcall*)(void*, Edx))((void* const volatile*)vt)[3])(m, 0)) {       // Begin
        ((void(__fastcall*)(void*, Edx))((void* const volatile*)vt)[6])(m, 0);            // End
        SG32(M_LIST + 4 * SG32(M_COUNT)) = (uint32_t)(uintptr_t)m;
        SG32(M_COUNT) = SG32(M_COUNT) + 1;
        return;
    }
    op_delete_o(m);
}
static void fp_mixer_add(Footprint& f, void*) { f.replay_only = "it probes the mixer (Begin / End: DirectSound) and may free it"; }
PORT_FN(0x00473900, "MixerAddMixer", MixerAddMixer_rw, fp_mixer_add)

// FIX CANDIDATE: no bounds check -- a location outside 0..3 reads the stack past the local table (the original's own
// frame: its return address, its argument ...; a rewrite reads its own frame there, so only 0..3 are comparable)
static const char* __cdecl SoundLocString_rw(SndLoc loc) {
    const char* t[4];
    t[0] = SS(0x004f5a0c);                                             // LOC_HARDWARE3D
    t[1] = SS(0x004f5a1c);                                             // LOC_HARDWARE
    t[2] = SS(0x004f5a2c);                                             // LOC_SOFTWARE
    t[3] = SS(0x004f5a3c);                                             // LOC_DONTCARE
    return ((const char* volatile*)t)[loc];
}
static void fp_pure_loc(Footprint& f, SndLoc) { f.pure = true; }
PORT_FN(0x004739a0, "SoundLocString", SoundLocString_rw, fp_pure_loc)

static float __cdecl MixerGetVolume_rw() { return Fb(SG32(M_VOLUME)); }
PORT_FN(0x004739d0, "MixerGetVolume", MixerGetVolume_rw, fp_none)
// the float as bits, compared as an integer: "negative" (above 0x80000000) is 0, anything above 1.0 as a signed integer
// (+NaN and +inf included) is 1.0; -0.0 is kept
static void __cdecl MixerSetVolume_rw(uint32_t v) {
    if (v > 0x80000000u) {
        v = 0;
    } else if ((int32_t)v > 0x3f800000) {
        v = 0x3f800000;
    }
    SG32(M_VOLUME) = v;
}
static void fp_mixer_set_volume(Footprint& f, uint32_t) { f.add(PTR(M_VOLUME), 4, "effects volume (0x4f5930)"); }
PORT_FN(0x004739e0, "MixerSetVolume", MixerSetVolume_rw, fp_mixer_set_volume)
static void __cdecl MixerSetQuality_rw(int32_t q) { SGI32(M_QUALITY) = (q < 0 || q > 2) ? 0 : q; }
static void fp_mixer_set_quality(Footprint& f, int32_t) { f.add(PTR(M_QUALITY), 4, "quality (0x4f5934)"); }
PORT_FN(0x00473a20, "MixerSetQuality", MixerSetQuality_rw, fp_mixer_set_quality)

// the three quality names, each a local static Xlator made on the first call (its destructor, a bare ret, registered
// with atexit), translated again when the language cookie moved on
// FIX CANDIDATE: no bounds check -- a quality outside 0..2 reads the stack past the local table (see SoundLocString)
static const char* __cdecl MixerGetQualityString_rw(int32_t q) {
    uint8_t g = SG8(M_QGUARD);
    if (!(g & 1)) {
        g |= 1;
        SG8(M_QGUARD) = g;
        Xlator_ctor_o(PTR(M_QX0), 0, SS(0x004f5a4c));                  // "Sound:Quality:Great"
        crt_atexit(0x00473b60);
        g = SG8(M_QGUARD);
    }
    if (!(g & 2)) {
        g |= 2;
        SG8(M_QGUARD) = g;
        Xlator_ctor_o(PTR(M_QX1), 0, SS(0x004f5a60));                  // "Sound:Quality:Awesome"
        crt_atexit(0x00473b50);
        g = SG8(M_QGUARD);
    }
    if (!(g & 4)) {
        g |= 4;
        SG8(M_QGUARD) = g;
        Xlator_ctor_o(PTR(M_QX2), 0, SS(0x004f5a78));                  // "Sound:Quality:Incredible"
        crt_atexit(0x00473b40);
    }
    const char* volatile t[3];
    uint32_t c = SG32(X_COOKIE);
    if (SG32(M_QX0 + 8) != c) {
        Xlator_xlate_o(PTR(M_QX0), 0);
        c = SG32(X_COOKIE);
    }
    t[0] = (const char*)(uintptr_t)SG32(M_QX0 + 4);
    if (SG32(M_QX1 + 8) != c) {
        Xlator_xlate_o(PTR(M_QX1), 0);
        c = SG32(X_COOKIE);
    }
    t[1] = (const char*)(uintptr_t)SG32(M_QX1 + 4);
    if (c != SG32(M_QX2 + 8)) Xlator_xlate_o(PTR(M_QX2), 0);
    t[2] = (const char*)(uintptr_t)SG32(M_QX2 + 4);
    return t[q];
}
static void fp_quality_string(Footprint& f, int32_t) {
    if ((SG8(M_QGUARD) & 7) != 7) {
        f.replay_only = "its first call constructs the local static Xlators and registers their destructors (atexit)";
        return;
    }
    f.add(PTR(M_QX0), 12, "Xlator Sound:Quality:Great (0x578a80)");
    f.add(PTR(M_QX1), 12, "Xlator Sound:Quality:Awesome (0x578a30)");
    f.add(PTR(M_QX2), 12, "Xlator Sound:Quality:Incredible (0x578a50)");
}
PORT_FN(0x00473a40, "MixerGetQualityString", MixerGetQualityString_rw, fp_quality_string)

static int32_t __cdecl MixerGetQuality_rw() { return SGI32(M_QUALITY); }
PORT_FN(0x00473b70, "MixerGetQuality", MixerGetQuality_rw, fp_none)

static const char* __fastcall NullMixer_GetName_rw(void*, Edx) {
    if (SG32(X_COOKIE) != SG32(M_NULLNAME + 8)) Xlator_xlate_o(PTR(M_NULLNAME), 0);
    return (const char*)(uintptr_t)SG32(M_NULLNAME + 4);
}
static void fp_nullmixer_name(Footprint& f, void*, Edx) { f.add(PTR(M_NULLNAME), 12, "Xlator Sound:NoSound (0x578a40)"); }
PORT_FN(0x00473b80, "NullMixer::GetName", NullMixer_GetName_rw, fp_nullmixer_name)
static void* __fastcall NullMixer_CreateSound_rw(void*, Edx, const char*, int32_t) { return 0; }
static void fp_nullmixer_create(Footprint& f, void*, Edx, const char*, int32_t) { f.pure = true; }
PORT_FN(0x00473bf0, "NullMixer::CreateSound", NullMixer_CreateSound_rw, fp_nullmixer_create)

// ======================================================================================================================
// softmxr.obj
// ======================================================================================================================
static void __cdecl softmxr_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00474c50, "$E1(softmxr.obj)", softmxr_E1_rw, fp_static_init)
static void __cdecl softmxr_E2_rw() { SFN(Void_t, 0x00474c50)(); }
PORT_FN(0x00474c40, "$E2(softmxr.obj)", softmxr_E2_rw, fp_static_init)

static void __cdecl SoftMixerCreateMixers_rw() {
    void* p = MemAlloc_o(0xc);
    void* m = p ? SoftMixer_ctor_o(p, 0) : 0;
    if (!m) return;
    if (((volatile SoftMixerO*)m)->ok) {
        MixerAddMixer_o(m);
        return;
    }
    op_delete_o(m);
}
static void fp_softmixer_create_mixers(Footprint& f) { f.replay_only = "it allocates the SoftMixer and probes it (MixerAddMixer)"; }
PORT_FN(0x00474c60, "SoftMixerCreateMixers", SoftMixerCreateMixers_rw, fp_softmixer_create_mixers)

static const char* __fastcall SoftMixer_GetName_rw(void*, Edx) {
    const uint8_t g = SG8(SM_NAMEGUARD);
    if (!(g & 1)) {
        SG8(SM_NAMEGUARD) = g | 1;
        Xlator_ctor_o(PTR(SM_NAME), 0, SS(0x004f5c5c));                // "Sound:SoftwareMixerName"
        crt_atexit(0x00474cf0);
    }
    if (SG32(X_COOKIE) != SG32(SM_NAME + 8)) Xlator_xlate_o(PTR(SM_NAME), 0);
    return (const char*)(uintptr_t)SG32(SM_NAME + 4);
}
static void fp_softmixer_name(Footprint& f, void*, Edx) {
    if (!(SG8(SM_NAMEGUARD) & 1)) {
        f.replay_only = "its first call constructs the local static Xlator and registers its destructor (atexit)";
        return;
    }
    f.add(PTR(SM_NAME), 12, "Xlator Sound:SoftwareMixerName (0x578b20)");
}
PORT_FN(0x00474ca0, "SoftMixer::GetName", SoftMixer_GetName_rw, fp_softmixer_name)

static const void* __fastcall SoftMixer_GetCaps_rw(void*, Edx) {
    SG8(SM_CAPS) = 1;
    SG32(SM_CAPS + 4) = 0;
    SG32(SM_CAPS + 8) = 0;
    return PTR(SM_CAPS);
}
static void fp_softmixer_caps(Footprint& f, void*, Edx) { f.add(PTR(SM_CAPS), 12, "SoftMixer caps (0x578b30)"); }
PORT_FN(0x00474d00, "SoftMixer::GetCaps", SoftMixer_GetCaps_rw, fp_softmixer_caps)

static void* __fastcall SoftMixer_ctor_rw(SoftMixerO* self, Edx) {
    volatile SoftMixerO* s = self;
    s->vtbl = PTR(0x004dd8c0);
    s->ok = 1;
    s->sounds = 0;
    return self;
}
static void fp_softmixer_ctor(Footprint& f, SoftMixerO* self, Edx) { f.add(self, sizeof(SoftMixerO), "SoftMixer (constructed)"); }
PORT_FN(0x00474d20, "SoftMixer::SoftMixer", SoftMixer_ctor_rw, fp_softmixer_ctor)

// Begin: the sounds' bag, then SOUND/fidelity >= 1: 16-bit (WaveBegin(3,1)); else, or if that fails, 8-bit
// (WaveBegin(1,1), eight_bit set). Neither: the bag is freed (the pointer stays).
// FIX: the bag held 256 sounds, and BagBase::add LogPanic'ed ("full") on the 257th. Its capacity is the operand of
// Begin's push (0x474d79), which M1 raises (to 1024: more than the SoundManager's 384-sound table can make); the
// rewrite reads it there (m1_operand), so both run with M1's value, and a harness with the stock 256.
static uint8_t __fastcall SoftMixer_Begin_rw(SoftMixerO* self, Edx) {
    volatile SoftMixerO* s = self;
    volatile int32_t fid = 1;
    OptionsGetI_o(*(const char* const volatile*)(uintptr_t)SM_SECTION, SS(0x004f5c74), (int32_t*)&fid);   // "fidelity"
    void* b = MemAlloc_o(0x14);
    if (b) {
        Bag_ctor_o(b, 0, SS(0x004f5c8c), (int32_t)m1_operand(0x00474d79));   // "SoftSound:sounds", 256 (M1: more)
        s->sounds = (Bag*)b;
    } else {
        s->sounds = 0;
    }
    s->eight_bit = 0;
    if (fid >= 1 && WaveBegin_o(3, 1)) return 1;
    if (WaveBegin_o(1, 1)) {
        s->eight_bit = 1;
        return 1;
    }
    Bag* bag = s->sounds;
    if (bag) {
        Bag_dtor_o(bag, 0);
        op_delete_o(bag);
    }
    return 0;
}
static void fp_softmixer_begin(Footprint& f, SoftMixerO*, Edx) {
    f.replay_only = "it allocates the sounds' bag and creates the DirectSound objects (WaveBegin)";
}
PORT_FN(0x00474d40, "SoftMixer::Begin", SoftMixer_Begin_rw, fp_softmixer_begin)

static void __fastcall SoftMixer_End_rw(SoftMixerO* self, Edx) {
    WaveEnd_o();
    Bag* bag = ((volatile SoftMixerO*)self)->sounds;
    if (bag) {
        Bag_dtor_o(bag, 0);
        op_delete_o(bag);
    }
}
static void fp_softmixer_end(Footprint& f, SoftMixerO*, Edx) { f.replay_only = "it releases the DirectSound objects (WaveEnd) and frees the bag"; }
PORT_FN(0x00474e00, "SoftMixer::End", SoftMixer_End_rw, fp_softmixer_end)

// FIX: a failed MemAlloc added a null sound to the bag, which mix_bytes then dereferenced: it isn't added (the null
// is returned as before). (The 257th sound's LogPanic -- the bag full -- is lifted in Begin.)
static void* __fastcall SoftMixer_CreateSound_rw(SoftMixerO* self, Edx, const char* name, int32_t) {
    void* r = 0;
    void* p = MemAlloc_o(0x40);
    if (p) r = SoftSound_ctor_o(p, 0, name, self);
    if (VP_FIX && !r) return 0;                                         // FIX: no sound, nothing in the bag
    Bag_add_o(((volatile SoftMixerO*)self)->sounds, 0, r);
    return r;
}
static void fp_softmixer_create_sound(Footprint& f, SoftMixerO*, Edx, const char*, int32_t) {
    f.replay_only = "it allocates the sound and gets its resource (SoundResourceGet)";
}
PORT_FN(0x00474e30, "SoftMixer::CreateSound", SoftMixer_CreateSound_rw, fp_softmixer_create_sound)

// the bag's count and its i-th item, read from memory as the original reads them (the mixer's bag pointer first)
static __forceinline int32_t bag_count(volatile SoftMixerO* s) { return ((volatile Bag*)s->sounds)->count; }
static __forceinline void* bag_item(volatile SoftMixerO* s, int32_t i) {
    return ((void* const volatile*)((volatile Bag*)s->sounds)->items)[i];
}

// the sounds' bag and its items, for footprints (guarded: a footprint may meet a mixer in any state)
static void fp_bag(Footprint& f, const Bag* b, bool sounds) {
    if (!b) return;
    f.add((void*)b, sizeof(Bag), "SoftMixer's bag");
    int32_t n = b->count;
    if (n <= 0 || !b->items) return;
    const int32_t cap = b->max > 0 && b->max <= 0x10000 ? b->max : 0x100;   // (256, or M1's lifted capacity)
    if (n > cap) n = cap;
    f.add(b->items, (uint32_t)n * 4, "the bag's items");
    if (!sounds) return;
    for (int32_t i = 0; i < n; i++)
        if (b->items[i]) f.add(b->items[i], sizeof(SoftSoundO), "SoftSound");
}

static void __fastcall SoftMixer_RemoveSound_rw(SoftMixerO* self, Edx, void* s) {
    Bag_remove_o(((volatile SoftMixerO*)self)->sounds, 0, s);
}
static void fp_softmixer_remove(Footprint& f, SoftMixerO* self, Edx, void*) { fp_bag(f, self->sounds, false); }
PORT_FN(0x00474e70, "SoftMixer::RemoveSound", SoftMixer_RemoveSound_rw, fp_softmixer_remove)

// wave.obj's statics a WaveGrab writes
static void fp_wave_grab_statics(Footprint& f) {
    f.add(PTR(W_N1), 8, "wave lock lengths / buffer (0x578c94)");   // 0x578c94, 0x578c98
    f.add(PTR(W_P1), 12, "wave lock pointers, write offset (0x578cc0)");   // 0x578cc0, 0x578cc4, 0x578cc8
    f.add(PTR(W_OK), 1, "wave locked flag (0x4f638c)");
    f.add(PTR(W_RESYNC), 1, "wave resync flag (0x4f6390)");
}

// Update, every BG tick: lock what the buffer can take, mix it, unlock. If WaveGrab fails, every sound that isn't
// looped is stopped.
static void __fastcall SoftMixer_Update_rw(SoftMixerO* self, Edx, const void*) {
    uint8_t* p1;
    uint32_t n1;
    uint8_t* p2;
    uint32_t n2;
    volatile SoftMixerO* s = self;
    if (WaveGrab_o(&p1, &n1, &p2, &n2)) {
        mix_bytes_o(self, 0, p1, n1);
        if (p2) mix_bytes_o(self, 0, p2, n2);
        WaveRelease_o();
        return;
    }
    for (int32_t i = 0; bag_count(s) > i; i++) {
        volatile SoftSoundO* ss = (volatile SoftSoundO*)bag_item(s, i);
        if (ss->looped == 0) {
            ss->playing = 0;
            ss->muted = 0;
        }
    }
}
static void fp_softmixer_update(Footprint& f, SoftMixerO* self, Edx, const void*) {
    // the locked DirectSound memory can't be named here (Lock hands it out during the call): the Unlock record hashes it
    fp_wave_grab_statics(f);
    f.add(PTR(F_FIRST), 1, "fastmix first flag (0x4f65d4)");
    fp_bag(f, self->sounds, true);
}
PORT_FN(0x00474e80, "SoftMixer::Update", SoftMixer_Update_rw, fp_softmixer_update)

// setup_channel: the gains and the step, from effects_volume * volume and the pan (+ right, - left), as 16.16:
//   v = effects * volume;  a = 1 + pan when pan < 0 (its bits above 0x80000000), else 1;  b = 1 - pan when pan > 0
//   (its bits a positive integer), else 1;  lg = ftol(b * v * 65535);  rg = ftol(a * v * 65536 - 1);  step =
//   ftol(ratio * 65536). Every value the original stores (v, a, a * v, b) goes through a float.
static void __fastcall setup_channel_rw(SoftSoundO* self, Edx) {
    volatile SoftSoundO* s = self;
    volatile float v, a, b;
    const double ev = (double)MixerGetVolume_o() * (double)Fb(s->vol);
    if (s->pan > 0x80000000u) {
        v = (float)ev;
        a = (float)((double)Fb(s->pan) + (double)1.0f);
    } else {
        v = (float)ev;
        a = Fb(0x3f800000);
    }
    const double av = (double)a * (double)v;
    if ((int32_t)s->pan > 0) {
        a = (float)av;
        b = (float)((double)1.0f - (double)Fb(s->pan));
    } else {
        a = (float)av;
        b = Fb(0x3f800000);
    }
    const int32_t lg = x87_ftol((double)b * (double)v * (double)65535.0f);
    const double rgd = (double)a * (double)65536.0f;
    s->ch.lg = lg;
    s->ch.rg = x87_ftol(rgd - (double)1.0f);
    s->ch.step = (uint32_t)x87_ftol((double)Fb(s->ratio) * (double)65536.0f);
}
static void fp_softsound(Footprint& f, SoftSoundO* self, Edx) { f.add(self, sizeof(SoftSoundO), "SoftSound"); }
PORT_FN(0x00474f20, "SoftSound::setup_channel", setup_channel_rw, fp_softsound)

// setup_run, before each 128-frame block: a playing sound at or past its end stops, or (looped) steps back by its
// length once.
// FIX: the end is only checked between blocks, so a block reads up to 128 * ratio (<= 2032) samples past the end of the
// data: the game's .sfx resources carry 4096 bytes of padding after the data (zeros for one-shots, the start again for
// loops), a mod's resource without it mixes whatever follows (do_run's fix). And a loop shorter than a block's step
// stayed past its end (one length back per block), reading further on each block; a zero-length loop never came back.
// A loop that one step back leaves outside its data now steps back into it: its position modulo the length (a
// zero-length loop: its start). One step that lands inside -- every stock loop -- is the original's.
static void __fastcall setup_run_rw(SoftSoundO* self, Edx) {
    volatile SoftSoundO* s = self;
    if (!s->playing) return;
    const uint32_t cur = s->ch.src;
    const uint32_t twice = s->len + s->len;
    const uint32_t end = s->start + twice;
    if (end > cur) {
        s->ch.src = cur;
        return;
    }
    if (s->looped) {
        uint32_t back = cur - twice;
        if (VP_FIX && back - s->start >= twice) {                        // FIX: still outside the loop's data
            const uint32_t start = s->start;
            back = twice ? start + (cur - start) % twice : start;
        }
        s->ch.src = back;
        return;
    }
    s->playing = 0;
    s->muted = 0;
}
PORT_FN(0x00474fd0, "SoftSound::setup_run", setup_run_rw, fp_softsound)

// FIX: (see setup_run and the extents above) a block whose reads would leave the sound's resource -- past its end, or
// before its data -- is mixed by mix_past_end: fastmix's arithmetic, frame for frame, with each sample outside the
// resource taken as the missing padding would give it (a one-shot: 0; a loop: its data from the start again). A block
// that stays inside is fastmix's, as before; so is every block of a sound without a record (made by the original).
static bool block_inside(const SoftSoundO* self, int32_t n, uint32_t* lim);
static void mix_past_end(SoftSoundO* self, void* dst, int32_t n, uint32_t lim);
static void __fastcall do_run_rw(SoftSoundO* self, Edx, void* acc, int32_t n) {
    volatile SoftSoundO* s = self;
    if (!s->playing) return;
    if (s->muted == 0) {
        uint32_t lim;
        if (VP_FIX && !block_inside(self, n, &lim)) {                     // FIX: it would read outside its resource
            mix_past_end(self, acc, n, lim);
            return;
        }
        fastmix_o(&self->ch, acc, n);
    } else {
        nullmix_o(&self->ch, acc, n);
    }
}
// the samples a block of n frames reads: src + 2 * (pos >> 16), pos = frac + k * step for k < n. Inside the data
// (and no resource ends before its data), or inside the resource's recorded extent, or no record: fastmix's block.
static bool block_inside(const SoftSoundO* self, int32_t n, uint32_t* lim) {
    const volatile SoftSoundO* s = self;
    if (n <= 0) return true;                        // (fastmix's own count: mix_bytes passes 1..128)
    const uint32_t src = s->ch.src, frac = s->ch.frac, step = s->ch.step, start = s->start;
    const uint64_t last = (uint64_t)frac + (uint64_t)step * (uint32_t)(n - 1);
    const uint64_t lo = (uint64_t)src + 2 * (uint64_t)(frac >> 16);
    const uint64_t hi = (uint64_t)src + 2 * (last >> 16) + 2;       // (last past 32 bits: pos wraps -- not inside)
    const bool wraps = (last >> 32) != 0;
    if (!wraps && lo >= start && hi <= (uint64_t)start + 2 * (uint64_t)s->len && !g_ext_short) return true;
    if (s->res) {
        if (!get_extent(self, (uint32_t)(uintptr_t)s->res, lim)) return true;
    } else {
        *lim = start;                               // (no resource: nothing to read)
    }
    return !wraps && lo >= start && hi <= *lim;
}
// a sample outside [start, lim): what the padding holds -- a loop's data from the start again (if that is inside the
// resource), else silence
static __forceinline int32_t past_end_sample(const volatile SoftSoundO* s, uint32_t a, uint32_t lim) {
    const uint32_t start = s->start, twice = s->len + s->len;
    if (s->looped && twice && a >= start) {
        const uint32_t b = start + (a - start) % twice;
        if (b >= start && (uint64_t)b + 2 <= lim) return *(const volatile int16_t*)(uintptr_t)b;
    }
    return 0;
}
// fastmix (below), a sample at a time through the resource's bounds
static void mix_past_end(SoftSoundO* self, void* dst, int32_t n, uint32_t lim) {
    const volatile SoftSoundO* s = self;
    if (SG8(F_FIRST)) SG8(F_FIRST) = 0;
    volatile MixChannel* c = &self->ch;
    volatile uint32_t* d = (volatile uint32_t*)dst;
    uint32_t cnt = (uint32_t)n;
    const uint32_t src = c->src, start = s->start;
    uint32_t pos = c->frac;
    uint32_t idx = pos >> 16;
    do {
        const uint32_t a = src + idx * 2;
        const int32_t smp = a >= start && (uint64_t)a + 2 <= lim ? *(const volatile int16_t*)(uintptr_t)a
                                                                : past_end_sample(s, a, lim);
        pos += c->step;
        const uint32_t l = (uint32_t)((int32_t)((uint32_t)c->lg * (uint32_t)smp) >> 8);
        d[0] = d[0] + l;
        const uint32_t r = (uint32_t)((int32_t)((uint32_t)c->rg * (uint32_t)smp) >> 8);
        d[1] = d[1] + r;
        idx = pos >> 16;
        d += 2;
    } while (--cnt);
    c->src = src + idx * 2;
    c->frac = pos & 0xffff;
}
static void fp_do_run(Footprint& f, SoftSoundO* self, Edx, void* acc, int32_t n) {
    f.add(self, sizeof(SoftSoundO), "SoftSound");
    f.add(PTR(F_FIRST), 1, "fastmix first flag (0x4f65d4)");
    if (n > 0) f.add(acc, (uint32_t)n * 8, "accumulator");
}
PORT_FN(0x00475000, "SoftSound::do_run", do_run_rw, fp_do_run)

// mix_bytes(dst, n): n bytes of the output (4 a frame at 16 bits, 2 at 8), in blocks of 128 frames through a 1 KB int32
// stereo accumulator
static void __fastcall mix_bytes_rw(SoftMixerO* self, Edx, uint8_t* dst, uint32_t n) {
    volatile SoftMixerO* s = self;
    int32_t frames = s->eight_bit == 0 ? (int32_t)(n >> 2) : (int32_t)(n >> 1);
    for (int32_t i = 0; bag_count(s) > i; i++) setup_channel_o(bag_item(s, i), 0);
    if (frames <= 0) return;
    do {
        int32_t blk = frames;
        if (blk >= 0x80) blk = 0x80;
        volatile int32_t acc[0x100];
        for (int i = 0; i < 0x100; i++) acc[i] = 0;
        for (int32_t i = 0; bag_count(s) > i; i++) {
            void* ss = bag_item(s, i);
            setup_run_o(ss, 0);
            do_run_o(ss, 0, (void*)acc, blk);
        }
        if (s->eight_bit == 0) {
            fastout_o((void*)acc, dst, blk + blk);
            dst += 0x200;
        } else {
            fastout_8_o((void*)acc, dst, blk + blk);
            dst += 0x100;
        }
        frames -= 0x80;
    } while (frames > 0);
}
static void fp_mix_bytes(Footprint& f, SoftMixerO* self, Edx, uint8_t* dst, uint32_t n) {
    f.add(dst, n, "the mixed output");
    f.add(PTR(F_FIRST), 1, "fastmix first flag (0x4f65d4)");
    fp_bag(f, self->sounds, true);
}
PORT_FN(0x00475040, "SoftMixer::mix_bytes", mix_bytes_rw, fp_mix_bytes)

// FIX: a missing resource (SoundResourceGet -> 0) was dereferenced: the sound is made empty (length 0; its start, as the
// original computes it, 0x16, is never read: do_run reads nothing of a sound without a resource) -- a one-shot stops at
// once, a loop plays silence. (SoundResourceGet LogPanics first in v1.0, so this is reached only if that returns.) And
// the sound's resource extent is noted (see do_run).
// FIX CANDIDATE: a resource that isn't 16-bit mono, or not 22050 Hz, is mixed as if it were (its WAVEFORMATEX is never
// read). Not fixed: honouring it would change how the stock game sounds (its engine loops are 44.1 kHz); what is read
// stays inside the data either way (the length is the data's bytes / 2).
static void* __fastcall SoftSound_ctor_rw(SoftSoundO* self, Edx, const char* name, SoftMixerO* mixer) {
    volatile SoftSoundO* s = self;
    s->vtbl = PTR(0x004dd8e0);
    s->mixer = mixer;
    uint8_t* res = SoundResourceGet_o(name);
    s->res = res;
    s->playing = 0;
    s->looped = 0;
    s->muted = 0;
    s->start = (uint32_t)(uintptr_t)(res + 0x16);
    const uint32_t bytes = VP_FIX && !res ? 0 : *(const volatile uint32_t*)res;   // FIX: (VP_FIX && !res) no resource
    s->pan = 0;
    s->len = bytes >> 1;
    s->vol = 0x3f800000;
    s->ratio = 0x3f800000;
    if (VP_FIX) note_extent(self, res, s->start, s->len);              // FIX: where its resource ends
    return self;
}
static void fp_softsound_ctor(Footprint& f, SoftSoundO*, Edx, const char*, SoftMixerO*) {
    f.replay_only = "it gets the sound's resource (SoundResourceGet)";
}
PORT_FN(0x00475160, "SoftSound::SoftSound", SoftSound_ctor_rw, fp_softsound_ctor)

static void __fastcall SoftSound_dtor_rw(SoftSoundO* self, Edx) {
    volatile SoftSoundO* s = self;
    s->vtbl = PTR(0x004dd8e0);
    SoftMixer_RemoveSound_o(s->mixer, 0, self);
    if (VP_FIX) forget_extent(self);                                   // FIX: (do_run) its record goes
    if (!VP_FIX || s->res) SoundResourceForget_o(s->res);              // FIX: no resource (SoftSound's fix): none to forget
    s->vtbl = PTR(0x004dd918);                                         // ISound's
}
static void fp_softsound_dtor(Footprint& f, SoftSoundO*, Edx) { f.replay_only = "it lets go of the sound's resource (SoundResourceForget)"; }
PORT_FN(0x004751b0, "SoftSound::~SoftSound", SoftSound_dtor_rw, fp_softsound_dtor)

static void __fastcall SoftSound_Play_rw(SoftSoundO* self, Edx) {
    volatile SoftSoundO* s = self;
    if (s->playing == 0) s->playing = 1;
    const uint32_t start = s->start;
    s->muted = 0;
    s->looped = 0;
    s->ch.src = start;
    s->ch.frac = 0;
}
static void fp_softsound_pure(Footprint& f, SoftSoundO* self, Edx) {
    f.pure = true;
    f.add(self, sizeof(SoftSoundO), "SoftSound");
}
PORT_FN(0x004751f0, "SoftSound::Play", SoftSound_Play_rw, fp_softsound_pure)
static void __fastcall SoftSound_PlayLooped_rw(SoftSoundO* self, Edx) {
    volatile SoftSoundO* s = self;
    if (s->playing == 0) s->playing = 1;
    const uint32_t start = s->start;
    s->muted = 0;
    s->ch.src = start;
    s->looped = 1;
    s->ch.frac = 0;
}
PORT_FN(0x00475210, "SoftSound::PlayLooped", SoftSound_PlayLooped_rw, fp_softsound_pure)
static void __fastcall SoftSound_Stop_rw(SoftSoundO* self, Edx) {
    volatile SoftSoundO* s = self;
    s->playing = 0;
    s->muted = 0;
}
PORT_FN(0x00475230, "SoftSound::Stop", SoftSound_Stop_rw, fp_softsound_pure)
// 1 playing, 2 playing muted, 3 stopped
static int32_t __fastcall SoftSound_GetStatus_rw(SoftSoundO* self, Edx) {
    volatile SoftSoundO* s = self;
    if (s->playing == 0) return 3;
    return s->muted == 0 ? 1 : 2;
}
static void fp_softsound_read(Footprint& f, SoftSoundO*, Edx) { f.pure = true; }
PORT_FN(0x00475240, "SoftSound::GetStatus", SoftSound_GetStatus_rw, fp_softsound_read)
static void __fastcall SoftSound_Mute_rw(SoftSoundO* self, Edx) { ((volatile SoftSoundO*)self)->muted = 1; }
PORT_FN(0x00475260, "SoftSound::Mute", SoftSound_Mute_rw, fp_softsound_pure)
static void __fastcall SoftSound_UnMute_rw(SoftSoundO* self, Edx) { ((volatile SoftSoundO*)self)->muted = 0; }
PORT_FN(0x00475270, "SoftSound::UnMute", SoftSound_UnMute_rw, fp_softsound_pure)

// the ratio's bits: below 0.02 as a signed integer (every negative value, -NaN included) is 0.02; above 16 (compared
// as floats: NaN stays) is 16
static void __fastcall SoftSound_SetFrequency_rw(SoftSoundO* self, Edx, uint32_t r) {
    if ((int32_t)r < 0x3ca3d70a) r = 0x3ca3d70a;
    if (Fb(r) > Fb(SG32(0x004dd86c))) r = SG32(0x004dd86c);           // 16.0f
    ((volatile SoftSoundO*)self)->ratio = r;
}
static void fp_softsound_set(Footprint& f, SoftSoundO* self, Edx, uint32_t) {
    f.pure = true;
    f.add(self, sizeof(SoftSoundO), "SoftSound");
}
PORT_FN(0x00475280, "SoftSound::SetFrequency", SoftSound_SetFrequency_rw, fp_softsound_set)
// the volume's bits: above 1.0 as a signed integer is 1.0, then "negative" (above 0x80000000) is 0
static void __fastcall SoftSound_SetVolume_rw(SoftSoundO* self, Edx, uint32_t v) {
    if ((int32_t)v > 0x3f800000) v = 0x3f800000;
    if (v > 0x80000000u) v = 0;
    ((volatile SoftSoundO*)self)->vol = v;
}
PORT_FN(0x004752c0, "SoftSound::SetVolume", SoftSound_SetVolume_rw, fp_softsound_set)
// the pan as it comes: no clamp (setup_channel's gains go negative or past 1 outside -1..1)
static void __fastcall SoftSound_SetPanning_rw(SoftSoundO* self, Edx, uint32_t p) { ((volatile SoftSoundO*)self)->pan = p; }
PORT_FN(0x004752f0, "SoftSound::SetPanning", SoftSound_SetPanning_rw, fp_softsound_set)

// music_test: a debug keyboard organ on its own SoftMixer (no caller in any build). SoundEnd; a SoftMixer on the stack;
// four "note.sfx" voices, looped and muted; then until ESC: each of 13 keys held takes a free voice at its pitch
// (frequency / 440), released keys mute theirs; the mixer's Update every 16 ms. Then the voices are deleted, the
// mixer ended and SoundBegin run again.
static void __cdecl music_test_rw() {
    struct Note { uint8_t key; uint8_t _1[3]; uint32_t freq; };
    SoundEnd_o();
    SoftMixerO m;
    SoftMixer_ctor_o(&m, 0);
    volatile Note notes[14] = {
        {0x2c, {}, 0x445c0000}, {0x1f, {}, 0x4468fae2}, {0x2d, {}, 0x4476d70a}, {0x20, {}, 0x4483c7ae},
        {0x2e, {}, 0x448a999a}, {0x2f, {}, 0x4492d99a}, {0x22, {}, 0x449b8a3e}, {0x30, {}, 0x44a4c7ae},
        {0x23, {}, 0x44ae91ec}, {0x31, {}, 0x44b9051f}, {0x24, {}, 0x44c4051e}, {0x32, {}, 0x44cfae15},
        {0x33, {}, 0x44dc0000}, {0, {}, 0},
    };
    if (!SoftMixer_Begin_o(&m, 0)) {
        LogReport_o(SS(0x004f5d08));                                    // "Couldn't begin mixer!"
        SoundBegin_o();
        return;
    }
    LogReport_o(SS(0x004f5cac), 4);                                    // "Mixer test (%d sounds)--press ESC to quit..."
    LogReport_o(SS(0x004f5cdc));
    LogReport_o(SS(0x004f5cec));
    volatile uint8_t held[4];
    *(volatile uint32_t*)held = 0;
    void* volatile voice[4];
    for (int i = 0; i < 4; i++) {
        void* v = SoftMixer_CreateSound_o(&m, 0, SS(0x004f5cfc), 3);    // "note.sfx", LOC_DONTCARE
        void* const* vt = *(void* const* const*)v;
        voice[i] = v;
        ((void(__fastcall*)(void*, Edx))((void* const volatile*)vt)[3])(v, 0);   // PlayLooped
        ((void(__fastcall*)(void*, Edx))((void* const volatile*)vt)[6])(v, 0);   // Mute
    }
    KeyDisableAll_o();
    if (!ScanHit_o(1)) {
        do {
            const volatile Note* n = notes;
            do {
                const uint8_t key = n->key;
                if (ScanDown_o(key)) {
                    int k = 0;
                    for (; k < 4; k++)
                        if (held[k] == key) break;
                    if (k == 4) {
                        for (k = 0; k < 4; k++)
                            if (held[k] == 0) break;
                        if (k < 4) {
                            const double f = (double)Fb(n->freq) * (double)Fb(0x3b14f209);   // x 1/440
                            held[k] = key;
                            void* v = voice[k];
                            void* const* vt = *(void* const* const*)v;
                            ((void(__fastcall*)(void*, Edx, float))((void* const volatile*)vt)[8])(v, 0, (float)f);
                            ((void(__fastcall*)(void*, Edx))((void* const volatile*)vt)[7])(v, 0);   // UnMute
                            ((void(__fastcall*)(void*, Edx))((void* const volatile*)vt)[3])(v, 0);   // PlayLooped
                        }
                    }
                } else {
                    for (int k = 0; k < 4; k++) {
                        if (held[k] != key) continue;
                        void* v = voice[k];
                        ((void(__fastcall*)(void*, Edx))(*(void* const volatile* const*)v)[6])(v, 0);   // Mute
                        held[k] = 0;
                    }
                }
                n++;
            } while (n->key != 0);
            SoftMixer_Update_o(&m, 0, 0);
            TaskSleep_o(0x10);
            Win32Idle_o();
        } while (!ScanHit_o(1));
    }
    for (int i = 0; i < 4; i++) {
        void* v = voice[i];
        if (v) ((void*(__fastcall*)(void*, Edx, uint32_t))(*(void* const volatile* const*)v)[0])(v, 0, 1);
    }
    SoftMixer_End_o(&m, 0);
    SoundBegin_o();
}
static void fp_music_test(Footprint& f) { f.replay_only = "a debug loop: it ends and restarts the sound system and runs until ESC"; }
PORT_FN(0x00475300, "music_test", music_test_rw, fp_music_test)

// Set3D: a SoftSound has no 3D (Can3D is false); LogPanic
static void __fastcall SoftSound_Set3D_rw(SoftSoundO*, Edx, const void*, const void*) { LogPanic_o(SS(0x004f5d20)); }
static void fp_softsound_set3d(Footprint&, SoftSoundO*, Edx, const void*, const void*) {}
PORT_FN(0x00475580, "SoftSound::Set3D", SoftSound_Set3D_rw, fp_softsound_set3d)

static void* __fastcall SoftSound_vdtor_rw(SoftSoundO* self, Edx, uint32_t flags) {
    SoftSound_dtor_o(self, 0);
    if (flags & 1) op_delete_o(self);
    return self;
}
static void fp_softsound_vdtor(Footprint& f, SoftSoundO*, Edx, uint32_t) { f.replay_only = "it frees the sound and lets go of its resource"; }
PORT_FN(0x00475590, "SoftSound::vector deleting destructor", SoftSound_vdtor_rw, fp_softsound_vdtor)

// ======================================================================================================================
// wave.obj
// ======================================================================================================================
static void __cdecl wave_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00476830, "$E1(wave.obj)", wave_E1_rw, fp_static_init)
static void __cdecl wave_E2_rw() { SFN(Void_t, 0x00476830)(); }
PORT_FN(0x00476820, "$E2(wave.obj)", wave_E2_rw, fp_static_init)

// WaveBegin(format, channels): the WAVEFORMATEX cleared, DirectSoundCreate, then the primary buffer, else the
// secondary, else the DirectSound object released. DirectSoundCreate is the audio core's (the original calls the
// DSOUND import, which audio_install points at the same core)
static uint8_t __cdecl WaveBegin_rw(int32_t fmt, int32_t ch) {
    TaskSleep_o(100);
    SG32(W_WFX) = 0;
    SG32(W_RATE) = 0;
    SG32(W_DS) = 0;
    SG32(W_AVG) = 0;
    SG32(W_ALIGN) = 0;
    SG16(W_WFX + 0x10) = 0;
    const int32_t hr = audio::create((audio::Device*)PTR(W_DS));
    if (hr) {
        LogReport_o(SS(0x004f640c), dsounderr2str_o(hr));             // "WaveBegin(): DirectSoundCreate fails! (%s)"
        return 0;
    }
    void* b = create_primary_buffer_o(fmt, ch);
    SG32(W_BUF) = (uint32_t)(uintptr_t)b;
    if (b) return 1;
    LogReport_o(SS(0x004f6394), fmt, ch);                              // "WaveBegin(): No primary in fmt %d:%d"
    b = create_secondary_buffer_o(fmt, ch);
    SG32(W_BUF) = (uint32_t)(uintptr_t)b;
    if (b) {
        LogReport_o(SS(0x004f63bc));                                    // "WaveBegin(): Using secondary buffer."
        return 1;
    }
    LogReport_o(SS(0x004f63e4), fmt, ch);                              // "WaveBegin(): No secondary in fmt %d:%d"
    audio::release(DEV(SG32(W_DS)));
    SG32(W_DS) = 0;
    return 0;
}
static void fp_wave_begin(Footprint& f, int32_t, int32_t) { f.replay_only = "it creates the DirectSound objects"; }
PORT_FN_AUDIO(0x00476840, "WaveBegin", WaveBegin_rw, fp_wave_begin)

struct DsCaps { uint32_t w[0x18]; };               // DSCAPS (0x60): dwSize, dwFlags ...
struct DsBufDesc { uint32_t size, flags, bytes, reserved, wfx; };   // DSBUFFERDESC (DirectX 5, 0x14)
struct DsBufCaps { uint32_t size, flags, bytes, unlock_rate, cpu; };   // DSBCAPS (0x14)

// the primary buffer in the format, written directly (DSSCL_WRITEPRIMARY): the card must have the sample depth
// (DSCAPS_PRIMARY16BIT / 8BIT), then SetFormat, its size, and Play (looping)
static void* __cdecl create_primary_buffer_rw(int32_t fmt, int32_t ch) {
    void* hwnd = Win32GetWindow_o();
    if (audio::set_cooperative_level(DEV(SG32(W_DS)), hwnd, 4)) {
        LogReport_o(SS(0x004f6540));                                    // "Wave: Couldn't get write primary access"
        return 0;
    }
    volatile DsCaps caps;
    for (int i = 0; i < 0x18; i++) caps.w[i] = 0;
    caps.w[0] = 0x60;
    if (audio::device_caps(DEV(SG32(W_DS)), (_DSCAPS*)&caps)) {
        LogReport_o(SS(0x004f651c));                                    // "Wave: Couldn't get DirectSound caps"
        return 0;
    }
    volatile DsBufDesc d;
    d.size = 0; d.flags = 0; d.bytes = 0; d.reserved = 0; d.wfx = 0;
    d.flags = 1;                                                       // DSBCAPS_PRIMARYBUFFER
    d.size = 0x14;
    void* volatile buf;
    d.bytes = 0;
    d.wfx = 0;
    buf = 0;
    int32_t hr = audio::create_buffer(DEV(SG32(W_DS)), (const _DSBUFFERDESC*)&d, (audio::Buffer*)&buf);
    if (hr) {
        LogReport_o(SS(0x004f6500), dsounderr2str_o(hr));             // "Wave: CreateSoundBuffer: %s"
        return 0;
    }
    setup_wf_o(fmt, ch);
    bool ok = true;
    if (SG16(W_BITS) == 0x10) {
        if (!(caps.w[1] & 8)) {
            LogReport_o(SS(0x004f6438));                                // "Wave: no 16-bit support on primary"
            ok = false;
        }
    } else if (SG16(W_BITS) == 8) {
        if (!(caps.w[1] & 4)) {
            LogReport_o(SS(0x004f645c));                                // "Wave: no 8-bit support on primary"
            ok = false;
        }
    } else {
        LogReport_o(SS(0x004f6480), (uint32_t)SG16(W_BITS));           // "Wave: unknown primary sample depth: %d"
        ok = false;
    }
    if (ok) {
        if (audio::set_format(BUF(buf), (const tWAVEFORMATEX*)PTR(W_WFX)) == 0) {
            volatile DsBufCaps bc;
            bc.size = 0; bc.flags = 0; bc.bytes = 0; bc.unlock_rate = 0; bc.cpu = 0;
            bc.size = 0x14;
            audio::buffer_caps(BUF(buf), (_DSBCAPS*)&bc);
            const uint32_t size = bc.bytes;
            const audio::Buffer b = BUF(buf);
            SG32(W_SIZE) = size;
            hr = buf_play(b);
            if (hr) LogReport_o(SS(0x004f64a8), dsounderr2str_o(hr));    // "Wave: Initial call to Play() failed (%s)"
            SG32(W_POS) = 0;
            return buf;
        }
    }
    LogReport_o(SS(0x004f64d4));                                        // "Wave: Couldn't set primary buffer format!"
    audio::release(BUF(buf));
    return 0;
}
static void fp_create_primary(Footprint& f, int32_t, int32_t) { f.replay_only = "it creates the primary buffer"; }
PORT_FN_AUDIO(0x00476930, "create_primary_buffer", create_primary_buffer_rw, fp_create_primary)

// setup_wf: PCM; stereo only for channels == 1; formats 0 / 2 11025 Hz, else 22050; formats 0 / 1 8-bit, else 16
static void __cdecl setup_wf_rw(int32_t fmt, int32_t ch) {
    SG16(W_WFX) = 1;
    SG16(W_WFX + 2) = (uint16_t)(ch == 1 ? 2 : 1);
    SG32(W_RATE) = (fmt == 0 || fmt == 2) ? 0x2b11 : 0x5622;
    SG16(W_BITS) = (uint16_t)((fmt == 0 || fmt == 1) ? 8 : 0x10);
    SG16(W_ALIGN) = (uint16_t)((int32_t)((uint32_t)SG16(W_WFX + 2) * (uint32_t)SG16(W_BITS)) / 8);
    SG32(W_AVG) = (uint32_t)SG16(W_ALIGN) * SG32(W_RATE);
}
static void fp_setup_wf(Footprint& f, int32_t, int32_t) { f.add(PTR(W_WFX), 18, "WAVEFORMATEX (0x578ca8)"); }
PORT_FN(0x00476b80, "setup_wf", setup_wf_rw, fp_setup_wf)

// the fallback: a 16 KB software secondary buffer in the format (DSSCL_EXCLUSIVE), its size, Play (looping)
static void* __cdecl create_secondary_buffer_rw(int32_t fmt, int32_t ch) {
    void* hwnd = Win32GetWindow_o();
    if (audio::set_cooperative_level(DEV(SG32(W_DS)), hwnd, 3)) {
        LogReport_o(SS(0x004f6584));                                    // "Wave: Couldn't get write primary access"
        return 0;
    }
    volatile DsBufDesc d;
    d.size = 0; d.flags = 0; d.bytes = 0; d.reserved = 0; d.wfx = 0;
    d.size = 0x14;
    d.flags = 8;                                                       // DSBCAPS_LOCSOFTWARE
    d.bytes = 0x4000;
    setup_wf_o(fmt, ch);
    void* volatile buf;
    buf = 0;
    const audio::Device ds = DEV(SG32(W_DS));
    d.wfx = W_WFX;
    const int32_t hr = audio::create_buffer(ds, (const _DSBUFFERDESC*)&d, (audio::Buffer*)&buf);
    if (hr) {
        LogReport_o(SS(0x004f6568), dsounderr2str_o(hr));             // "Wave: CreateSoundBuffer: %s"
        return 0;
    }
    volatile DsBufCaps bc;
    bc.size = 0; bc.flags = 0; bc.bytes = 0; bc.unlock_rate = 0; bc.cpu = 0;
    audio::Buffer b = BUF(buf);
    bc.size = 0x14;
    audio::buffer_caps(b, (_DSBCAPS*)&bc);
    const uint32_t size = bc.bytes;
    b = BUF(buf);
    SG32(W_SIZE) = size;
    buf_play(b);
    SG32(W_POS) = 0;
    return buf;
}
static void fp_create_secondary(Footprint& f, int32_t, int32_t) { f.replay_only = "it creates the secondary buffer"; }
PORT_FN_AUDIO(0x00476c20, "create_secondary_buffer", create_secondary_buffer_rw, fp_create_secondary)

// WaveEnd: Stop, release the buffer and DirectSound (the buffer's pointer stays), sleep 250 ms
static void __cdecl WaveEnd_rw() {
    audio::stop(BUF(SG32(W_BUF)));
    audio::release(BUF(SG32(W_BUF)));
    audio::release(DEV(SG32(W_DS)));
    SG32(W_DS) = 0;
    TaskSleep_o(0xfa);
}
static void fp_wave_end(Footprint& f) { f.replay_only = "it releases the DirectSound objects"; }
PORT_FN_AUDIO(0x00476d30, "WaveEnd", WaveEnd_rw, fp_wave_end)

// WaveGrab: lock the part of the buffer to write next. S = the buffer's size, pos = the write offset, T = 2 * (bytes a
// second / 50) = 40 ms. Play again if it stopped; the cursors (failing: Restore, Play, no lock); resync -> pos = write.
//   avail = play - pos (+ S when play < pos);  under S/2 (the writer got ahead of play: an underrun, or pos == play)
//   -> pos = write, avail = max_data_from_cursors(write, play) (play - write, + S when write >= play);
//   queued = pos - play when play < pos, else S - play + pos (S when they're equal);
//   n = queued > T ? 0x200 : min(avail, T - queued), rounded down to 4; 0 -> n = T and pos = write.
// Lock(pos, n): BUFFERLOST -> Restore (failing: no lock), Play, resync, Lock(pos, T) again; any error -> resync, 0.
// Locked: the four results handed out, pos = the second length when it wrapped, else pos + the first (so pos can be S).
// FIX CANDIDATE: pos == S asks Lock for an offset of exactly the buffer's size (real DirectSound: DSERR_INVALIDPARAM,
// which WaveGrab turns into a resync and a silent tick, no crash). Not fixed: this rewrite runs only on the audio core
// (PORT_FN_AUDIO: with the game's own DirectSound it stays original), whose lock takes the offset modulo the size --
// 0, the ring's start, where the next write belongs -- so it neither fails nor misplaces anything.
static uint8_t __cdecl WaveGrab_rw(uint8_t** p1, uint32_t* n1, uint8_t** p2, uint32_t* n2) {
    volatile uint32_t status;
    if (audio::status(BUF(SG32(W_BUF)), (unsigned long*)&status) == 0 && !(status & 1)) buf_play(BUF(SG32(W_BUF)));
    volatile uint32_t play, write;
    if (audio::position(BUF(SG32(W_BUF)), (unsigned long*)&play, (unsigned long*)&write)) {
        if (buf_restore(BUF(SG32(W_BUF))) == 0) buf_play(BUF(SG32(W_BUF)));
        return 0;
    }
    if (SG8(W_RESYNC)) {
        SG8(W_RESYNC) = 0;
        SG32(W_POS) = write;
    }
    uint32_t avail;
    {
        const uint32_t pos = SG32(W_POS);
        avail = play;
        if (play >= pos) avail -= pos;
        else avail = avail - SG32(W_POS) + SG32(W_SIZE);
    }
    if ((SG32(W_SIZE) >> 1) > avail) {
        const uint32_t w = write, p = play;
        SG32(W_POS) = w;
        avail = max_data_from_cursors_o(w, p);
    }
    uint32_t queued;
    {
        const uint32_t pos = SG32(W_POS);
        if (play < pos) queued = pos - play;
        else queued = SG32(W_SIZE) - play + SG32(W_POS);
    }
    const uint32_t T = (SG32(W_AVG) / 50) * 2;
    uint32_t n;
    if (T < queued) {
        n = 0x200;
    } else {
        n = avail;
        if (n > T - queued) n = T - queued;
    }
    n &= ~3u;
    if (n == 0) {
        n = T;
        SG32(W_POS) = write;
    }
    int32_t hr = buf_lock(BUF(SG32(W_BUF)), SG32(W_POS), n);
    if ((uint32_t)hr == DSERR_BUFFERLOST) {
        hr = buf_restore(BUF(SG32(W_BUF)));
        if (hr) goto fail;
        buf_play(BUF(SG32(W_BUF)));
        SG8(W_RESYNC) = 1;
        {
            const uint32_t pos = SG32(W_POS);
            hr = buf_lock(BUF(SG32(W_BUF)), pos, T);
        }
        if (hr == 0) LogReport_o(SS(0x004f65ac));                        // "wave: relocked!"
        else LogReport_o(SS(0x004f65bc), dsounderr2str_o(hr));         // "wave: ReLock fails (%s)"
    }
    if (hr) goto fail;
    if (SG8(W_OK) == 0) SG8(W_OK) = 1;
    *(uint32_t volatile*)p1 = SG32(W_P1);
    *(uint32_t volatile*)p2 = SG32(W_P2);
    *(uint32_t volatile*)n1 = SG32(W_N1);
    *(uint32_t volatile*)n2 = SG32(W_N2);
    if (SG32(W_P2)) {
        SG32(W_POS) = SG32(W_N2);
        return 1;
    }
    {
        const uint32_t add = SG32(W_N1);
        SG32(W_POS) = SG32(W_POS) + add;
    }
    return 1;
fail:
    if (SG8(W_OK)) SG8(W_OK) = 0;
    SG8(W_RESYNC) = 1;
    return 0;
}
static void fp_wave_grab(Footprint& f, uint8_t** p1, uint32_t* n1, uint8_t** p2, uint32_t* n2) {
    fp_wave_grab_statics(f);
    f.add(p1, 4, "first pointer");
    f.add(n1, 4, "first length");
    f.add(p2, 4, "second pointer");
    f.add(n2, 4, "second length");
}
PORT_FN_AUDIO(0x00476d70, "WaveGrab", WaveGrab_rw, fp_wave_grab)

// the bytes from the write cursor round to the play cursor: play - write, + S when write >= play (S when equal)
static uint32_t __cdecl max_data_from_cursors_rw(uint32_t write, uint32_t play) {
    if (write < play) return play - write;
    return play - write + SG32(W_SIZE);
}
static void fp_max_data(Footprint&, uint32_t, uint32_t) {}
PORT_FN(0x00476ff0, "max_data_from_cursors", max_data_from_cursors_rw, fp_max_data)

static void __cdecl WaveRelease_rw() {
    if ((uint32_t)buf_unlock(BUF(SG32(W_BUF))) == DSERR_BUFFERLOST) {
        buf_restore(BUF(SG32(W_BUF)));
        buf_unlock(BUF(SG32(W_BUF)));
    }
}
static void fp_wave_release(Footprint&) {}          // Unlock (and Restore): outputs, recorded by the audio core
PORT_FN_AUDIO(0x00477010, "WaveRelease", WaveRelease_rw, fp_wave_release)

// ======================================================================================================================
// fastmix.obj (hand-written assembler in the original: plain 32-bit integer loops)
// ======================================================================================================================
static void __cdecl fastmix_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00477080, "$E1(fastmix.obj)", fastmix_E1_rw, fp_static_init)
static void __cdecl fastmix_E2_rw() { SFN(Void_t, 0x00477080)(); }
PORT_FN(0x00477070, "$E2(fastmix.obj)", fastmix_E2_rw, fp_static_init)

// fastmix(ch, acc, n): n frames added into the int32 stereo accumulator: s = src[pos >> 16] (16-bit), pos += step,
// acc += (lg * s) >> 8, (rg * s) >> 8 (32-bit products, arithmetic shifts, wrapping sums); then src moves on by the
// whole samples and pos keeps its fraction. The channel's step and gains are read from memory on every frame, as the
// original does.
// FIX CANDIDATE: n is a do-while count: 0 (or negative) runs 2^32 (+n) frames over the accumulator. Not fixed: nothing
// reaches it -- fastmix / nullmix are called only by do_run, and do_run only by mix_bytes, with 1..128 frames (no
// sound data reaches the count).
static void __cdecl fastmix_rw(MixChannel* ch, void* dst, int32_t n) {
    if (SG8(F_FIRST)) SG8(F_FIRST) = 0;
    volatile MixChannel* c = ch;
    volatile uint32_t* d = (volatile uint32_t*)dst;
    uint32_t cnt = (uint32_t)n;
    const uint32_t src = c->src;
    uint32_t pos = c->frac;
    uint32_t idx = pos >> 16;
    do {
        const int32_t s = *(const volatile int16_t*)(uintptr_t)(src + idx * 2);
        pos += c->step;
        const uint32_t l = (uint32_t)((int32_t)((uint32_t)c->lg * (uint32_t)s) >> 8);
        d[0] = d[0] + l;
        const uint32_t r = (uint32_t)((int32_t)((uint32_t)c->rg * (uint32_t)s) >> 8);
        d[1] = d[1] + r;
        idx = pos >> 16;
        d += 2;
    } while (--cnt);
    c->src = src + idx * 2;
    c->frac = pos & 0xffff;
}
static void fp_fastmix(Footprint& f, MixChannel* ch, void* dst, int32_t n) {
    f.add(ch, sizeof(MixChannel), "mix channel");
    f.add(PTR(F_FIRST), 1, "fastmix first flag (0x4f65d4)");
    if (n > 0) f.add(dst, (uint32_t)n * 8, "accumulator");
}
PORT_FN(0x00477090, "fastmix", fastmix_rw, fp_fastmix)

// nullmix: fastmix's position update alone (a muted sound keeps its place)
static void __cdecl nullmix_rw(MixChannel* ch, void*, SndCount n) {
    volatile MixChannel* c = ch;
    uint32_t cnt = (uint32_t)n;
    const uint32_t src = c->src;
    uint32_t pos = c->frac;
    do {
        pos += c->step;
    } while (--cnt);
    c->src = src + (pos >> 16) * 2;
    c->frac = pos & 0xffff;
}
static void fp_nullmix(Footprint& f, MixChannel* ch, void*, SndCount) {
    f.pure = true;
    f.add(ch, sizeof(MixChannel), "mix channel");
}
PORT_FN(0x00477100, "nullmix", nullmix_rw, fp_nullmix)

// fastout: n samples (2 a frame) from the accumulator to S16: >= 0x7fffff -> 0x7fff, <= -0x800000 -> 0x8000, else >> 8
static void __cdecl fastout_rw(void* src, void* dst, SndCount n) {
    const volatile int32_t* s = (const volatile int32_t*)src;
    volatile uint16_t* d = (volatile uint16_t*)dst;
    uint32_t cnt = (uint32_t)n;
    do {
        const int32_t v = *s++;
        if (v >= 0x7fffff) *d = 0x7fff;
        else if (v <= -0x800000) *d = 0x8000;
        else *d = (uint16_t)(v >> 8);
        d++;
    } while (--cnt);
}
static void fp_fastout(Footprint& f, void*, void* dst, SndCount n) {
    f.pure = true;
    if (n > 0) f.add(dst, (uint32_t)n * 2, "S16 output");
}
PORT_FN(0x00477140, "fastout", fastout_rw, fp_fastout)

// fastout_8: to U8: >= 0x7fffff -> 0xff, <= -0x800000 -> 0, else (v + 0x800000) >> 16
static void __cdecl fastout_8_rw(void* src, void* dst, SndCount n) {
    const volatile int32_t* s = (const volatile int32_t*)src;
    volatile uint8_t* d = (volatile uint8_t*)dst;
    uint32_t cnt = (uint32_t)n;
    do {
        const int32_t v = *s++;
        if (v >= 0x7fffff) *d = 0xff;
        else if (v <= -0x800000) *d = 0;
        else *d = (uint8_t)(((uint32_t)v + 0x800000u) >> 16);
        d++;
    } while (--cnt);
}
static void fp_fastout_8(Footprint& f, void*, void* dst, SndCount n) {
    f.pure = true;
    if (n > 0) f.add(dst, (uint32_t)n, "U8 output");
}
PORT_FN(0x00477190, "fastout_8", fastout_8_rw, fp_fastout_8)

// ---- integration notes -------------------------------------------------------------------------------------------------
// * The $E1 / $E2 / $E8 (mixer.obj) and the other objects' $E2 start with a jmp rel32 (a tail call); $E1 of each object
//   is a jmp rel32 to rcfunc_is_internal (a bare ret). A trampoline must relocate them.
// * NullMixer::CreateSound (xor eax,eax; ret 8), SoftSound::Mute / UnMute (mov byte [ecx+0x16],n; ret) are exactly 5
//   bytes: the hook's jump covers the whole function.
// * mix_bytes, do_run, setup_run / setup_channel and the fast* loops run 128-frame blocks from SoftMixer::Update on the
//   BG thread, up to 32 sounds a block: calling them by address (so a hooked rewrite runs) costs a jump each.
#undef SG8
#undef SG16
#undef SG32
#undef SGI32
#undef SS
#undef SFN
#undef SVF
#undef PTR
#undef DEV
#undef BUF
