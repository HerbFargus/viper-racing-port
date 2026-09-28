// snd_car.cpp -- M3 sound stage S1, group B: the car sounds, rewritten (library `sound`).
//
//   engsnd.obj   EngineSound (a car's engine: an idle sample and up to 7 RPM bands, each a Sound3D whose volume and
//                pitch follow the car's perceived RPM and throttle) and EngineSoundSample (one band: its RPM ramp
//                and volume blend), with the object file's $E static initialisers
//   tiresnd.obj  TireSound / RealTireSound / DummyTireSound (the tyre squeal: one "squeal.sfx" Sound3D per car,
//                per axle or per wheel, by the mixer quality; the other wheels get a silent dummy), and its $E
//                initialisers
//
// Faithful (docs/PORTING.md, the S1 brief): written from the v1.0 disassembly. Every call in the original's order with
// its arguments: the game's functions by their v1.0 address (this file's own too -- EngineSound::Update calls
// EngineSoundSample::Update, which calls GetVolume, each at its address, so a hooked rewrite or the original is what
// runs), the Car's GetPerceivedThrottle / GetPerceivedRPM and the sounds' deleting destructors through their vtables,
// the CRT's sprintf / sscanf at their own addresses, the game's own strings. Floats the original moves with integer
// instructions are copied as bits; values it keeps on the x87 are doubles, what it stores is a float, each such store
// forced (st(), rule 10a: these run on the physics / BG thread, overflow and divide-by-zero unmasked); comparisons
// keep the original's NaN result; integer compares of a float's bits are done on the bits.
//
// Fixes (docs/PORTING.md, "Fixes"; each marked FIX:, VP_FIX; test/world_snd_car.cpp built with /DVP_SND_CAR_FIXES
// tests them). All are mod-data cases; stock data never reaches them:
//   * EngineSoundSample::GetVolume / Update: a zero-width plateau or ramp (rpm_full == rpm_peak, rpm_on == rpm_full,
//     rpm_peak == rpm_off) or a zero ref_rpm divided by zero, which faults on the BG thread (divide-by-zero unmasked).
//     The division by 0 now gives what the FPU gives with that exception masked (div_masked): the same bits the
//     original computes wherever it doesn't fault.
//   * EngineSound::EngineSound: an .ens resource with more than 7 bands is read to its first 7 (the 8th overwrote
//     `idle`, the 9th on ran past the object; logged once); with neither resource nor engine.txt, `count` is 0 (it was
//     MemAlloc garbage, which Update and the destructor walked); engine.txt's uncounted last Sound3D is deleted.
//   * EngineSound::Update: a NULL band (a missing engine<n>.sfx, engine.txt's path) is skipped where it's switched off.
//   * EngineSound::EngineSound: "<car>e.ens" is built in a buffer long enough for any car name (27+ characters ran
//     past the 32 bytes; harmless in the original's frame).
//   * TireSound::Create: a failed MemAlloc of the RealTireSound no longer writes its siblings through NULL; a car
//     index outside the table's 16 is neither read nor written (the car's sound speaks for its own wheel only); a
//     mixer quality outside 0..2 (which read past the RealWheel table) gives no squeal (every wheel a dummy).
//   * RealTireSound::RealTireSound: a RealWheel outside 0..3 is logged, not a panic, and its gain is 0 (silent).
// Candidates not fixed are still marked `// FIX CANDIDATE:`.
//
// Threads. EngineSound::Update (from Car::UpdateCommon) and RealTireSound::Update (Wheel::UpdateCommon through the
// TireSound's vtable) run every physics tick from update_phobs, on the BGTask timer thread -- the same thread the
// SoundManager mixes on, so nothing here races the mixer. They write only heap objects (the Sound3Ds' pitch, volume
// and flags; the RealTireSound's averaged position), listed in their footprints. The constructors run at race set-up
// (EngineSound::Create from Car::Car, which PhysTaskBegin's create_phob calls; TireSound::Create from Wheel::Setup) and
// allocate: replay_only, as are the destructors. No function here draws a random number, and none calls DirectSound.
//
// Layouts (v1.0; from the disassembly and the allocations):
//   Sound3D (sound.obj, group A; 0x40 bytes): +0 vtable (slot 0 the scalar deleting destructor), +4 float pitch (the
//     frequency ratio), +8 float volume, +0xc the sample (Create deletes a sound without one), +0x10 int owner (the
//     tyre sound's car index), +0x29 u8 on, +0x2a u8 off, +0x2b u8 keep, +0x2c u8 volume changed, +0x2d u8 pitch changed
//   EngineSound (0xf8, no vtable): +8 Car*, +0xc the car's index (Car +0x510), +0x10 EngineSoundSample[7],
//     +0xd4 Sound3D* band[7], +0xf0 Sound3D* idle, +0xf4 int number of bands
//   EngineSoundSample (0x1c): +0 rpm_on (below: silent), +4 rpm_full (the ramp up ends), +8 rpm_peak (the ramp down
//     starts), +0xc rpm_off (at and above: silent), +0x10 vol_lo, +0x14 vol_hi (the blend across the plateau),
//     +0x18 ref_rpm (the recording's RPM: pitch = rpm / ref_rpm). Defaults 0 / 1000 / 6000 / 7000, 0.4 / 0.5, 3000.
//     The resource (<car>e.ens, else engine.ens, FourCC 'XFSE') and engine.txt give each band as
//     "ref_rpm vol_lo vol_hi rpm_on rpm_full rpm_peak rpm_off".
//   TireSound (0xc, vtable 0x4dd790: 0 deleting destructor, 1 Update (pure), 2 Ok): +4 wheel index, +8 Wheel*
//   DummyTireSound (0x10, vtable 0x4dd7a0): +0xc u8 ok (1)
//   RealTireSound (0x3c, vtable 0x4dd7b0): +0xc TireSound* wheel_sound[4] (the wheels it speaks for, itself included),
//     +0x1c P3 position (the average contact point, the Sound3D's position), +0x28 car index, +0x30 Car*,
//     +0x34 Sound3D*, +0x38 float gain (by RealWheel: 0, 0.7, 0.63, 0.55)
//   Wheel (wheel.obj, phys_wheel.cpp): +0xc8 P3 droop_point_world, +0x11c float slide, +0x121 s8 surface_class
//   Car: +0x5c and +0x234 the P3DBases a Sound3D follows, +0x510 car_index, +0x514 char name[32]; vtable +0x44
//     GetPerceivedThrottle, +0x48 GetPerceivedRPM (both return ST0)
//
// Statics: engsnd.obj 0x5788a4..0x5788e7 and tiresnd.obj 0x5788e8..0x57892b are per-object copies of a shared
// header's 16-bit colour pairs, set by the $E initialisers; 0x578930 TireSound* g_tire_sounds[16 cars][4 wheels]
// (TireSound::Create's table, which the axle and car sounds read their siblings from).
//
// Every function in the two object files is here. The $E pairs are rewritten as krn_core.cpp does them: $E1 is a
// tail jump to rcfunc_is_internal (a bare `ret`) and stays a call to it; each even/odd pair is a jump and a store.
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"

#ifndef SND_FIX_FIRED
#define SND_FIX_FIRED() ((void)0)                   // (the harness counts the worlds a fix changed)
#endif

#define D(x) ((double)(x))                          // a register value (x87.h)
#define FB(b) __builtin_bit_cast(float, (uint32_t)(b))   // a float constant from its bits
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

namespace {
#define SC_G32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define SC_S(a) ((const char*)(uintptr_t)(a))       // one of the game's own strings
#define SC_FN(T, a) ((T)(uintptr_t)(a))

static __forceinline uint32_t Ub(const void* p) { uint32_t u; memcpy(&u, p, 4); return u; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }     // an integer move of a float
static __forceinline void put4(void* d, uint32_t v) { memcpy(d, &v, 4); }
// an fst / fstp dword the original always makes, kept where the float then goes unused (docs/PORTING.md 10a)
static __forceinline float st(double d) {
    volatile float f = (float)d;
    return f;
}
// the same as ONE fstp dword, nothing after it: st() compiles to a rounding fstp, then fld / fstp into the volatile,
// and that fld raises a pending overflow at once, where the original's lone fstp leaves it pending until its next
// x87 instruction (EngineSoundSample::Update writes `off` in between)
static __forceinline void st_to(float* dst, double d) {
    __asm { fld d
            mov eax, dst
            fstp dword ptr [eax] }
}
// FIX: num / den, where a zero den gives what the FPU gives with the divide-by-zero exception masked -- num x the
// infinity of den's sign: a finite non-zero num gives the signed infinity, 0 the default NaN (0/0's), an infinity or a
// NaN itself, all exactly as the division, and with no exception (an operation on an infinity is exact). The engine
// code runs on the BG thread, where divide-by-zero is unmasked, so the original faults there instead. Every other den
// is the original's division.
static __forceinline double div_masked(double num, double den) {
    if (!VP_FIX || !(den == 0.0)) return num / den;
    SND_FIX_FIRED();
    uint64_t b;
    memcpy(&b, &den, 8);
    return num * __builtin_bit_cast(double, (b & 0x8000000000000000ull) | 0x7ff0000000000000ull);
}

struct Sound3D {                                    // what this file touches (sound.obj's own layout is group A's)
    void** vtable;                                  // +0x00  slot 0: scalar deleting destructor
    float pitch;                                    // +0x04  frequency ratio
    float volume;                                   // +0x08
    void* sample;                                   // +0x0c
    int32_t owner;                                  // +0x10
    uint8_t _14[0x29 - 0x14];
    uint8_t on;                                     // +0x29
    uint8_t off;                                    // +0x2a
    uint8_t keep;                                   // +0x2b
    uint8_t volume_changed;                         // +0x2c
    uint8_t pitch_changed;                          // +0x2d
    uint8_t _2e[0x40 - 0x2e];
};
static_assert(offsetof(Sound3D, on) == 0x29 && offsetof(Sound3D, pitch_changed) == 0x2d && sizeof(Sound3D) == 0x40,
              "Sound3D");

struct EngineSoundSample {                          // 0x1c
    float rpm_on;                                   // +0x00
    float rpm_full;                                 // +0x04
    float rpm_peak;                                 // +0x08
    float rpm_off;                                  // +0x0c
    float vol_lo;                                   // +0x10
    float vol_hi;                                   // +0x14
    float ref_rpm;                                  // +0x18
};
static_assert(sizeof(EngineSoundSample) == 0x1c, "EngineSoundSample");

struct EngineSound {                                // 0xf8
    uint8_t _00[8];
    uint8_t* car;                                   // +0x08  Car*
    int32_t car_index;                              // +0x0c
    EngineSoundSample samples[7];                   // +0x10
    Sound3D* band[7];                               // +0xd4
    Sound3D* idle;                                  // +0xf0
    int32_t count;                                  // +0xf4
};
static_assert(offsetof(EngineSound, samples) == 0x10 && offsetof(EngineSound, band) == 0xd4 &&
              offsetof(EngineSound, count) == 0xf4 && sizeof(EngineSound) == 0xf8, "EngineSound");

struct TireSound {                                  // 0xc
    void** vtable;
    int32_t wheel_index;                            // +0x04
    uint8_t* wheel;                                 // +0x08  Wheel*
};
struct DummyTireSound : TireSound {                 // 0x10
    uint8_t ok;                                     // +0x0c
    uint8_t _0d[3];
};
static_assert(sizeof(DummyTireSound) == 0x10, "DummyTireSound");
struct RealTireSound : TireSound {                  // 0x3c
    TireSound* wheel_sound[4];                      // +0x0c
    P3 pos;                                         // +0x1c
    int32_t car_index;                              // +0x28
    uint8_t _2c[4];
    uint8_t* car;                                   // +0x30  Car*
    Sound3D* sound;                                 // +0x34
    float gain;                                     // +0x38
};
static_assert(offsetof(RealTireSound, pos) == 0x1c && offsetof(RealTireSound, sound) == 0x34 &&
              sizeof(RealTireSound) == 0x3c, "RealTireSound");

// Wheel fields RealTireSound::Update reads
static __forceinline const float* wheel_droop_point(const uint8_t* w) { return (const float*)(w + 0xc8); }
static __forceinline const float* wheel_slide(const uint8_t* w) { return (const float*)(w + 0x11c); }
static __forceinline int8_t wheel_surface_class(const uint8_t* w) { return *(const int8_t*)(w + 0x121); }

// ---- the game's functions -----------------------------------------------------------------------------------------
typedef void(__cdecl* SndVoid_t)();
typedef int(__cdecl* SndSprintf_t)(char*, const char*, ...);
typedef int(__cdecl* SndSscanf_t)(const char*, const char*, ...);
typedef void(__cdecl* SndLog_t)(const char*, ...);
typedef uint8_t*(__cdecl* SndResource_t)(const char*, uint32_t, uint32_t*, int32_t*, uint8_t*, uint8_t*);
typedef uint8_t(__cdecl* SndResourceExists_t)(const char*);
typedef uint8_t(__cdecl* SndResourceForget_t)(void*);
typedef int32_t(__cdecl* SndFileOpen_t)(const char*);
typedef uint8_t(__cdecl* SndFileReadLine_t)(int32_t, char*, int);
typedef void(__cdecl* SndFileClose_t)(int32_t*);
typedef Sound3D*(__cdecl* Sound3DCreate_t)(const char*, int, const void*, const void*);
typedef int(__cdecl* SndInt_t)();
typedef void*(__cdecl* SndMemAlloc_t)(int);
typedef void(__cdecl* SndDelete_t)(void*);
typedef void*(__fastcall* SampleCtor_t)(void*, Edx);
typedef double(__fastcall* SampleGetVolume_t)(void*, Edx, uint32_t rpm);                  // ST0, unrounded
typedef void(__fastcall* SampleUpdate_t)(void*, Edx, uint32_t rpm, Sound3D*, uint32_t volume);
typedef void*(__fastcall* TireSoundCtor_t)(void*, Edx, int, void*);
typedef void*(__fastcall* RealTireSoundCtor_t)(void*, Edx, int, int, void*, void*, int);
typedef void(__fastcall* SndDtor_t)(void*, Edx);
typedef double(__fastcall* CarFloat_t)(void*, Edx);                                         // ST0
typedef void*(__fastcall* SndVDel_t)(void*, Edx, unsigned);

#define snd_rcfunc_is_internal SC_FN(SndVoid_t, 0x00410cc0)
#define snd_sprintf SC_FN(SndSprintf_t, 0x004cf0a0)
#define snd_sscanf SC_FN(SndSscanf_t, 0x004ce190)
#define snd_LogReport SC_FN(SndLog_t, 0x00411150)
#define snd_LogPanic SC_FN(SndLog_t, 0x004112b0)
#define snd_ResourceTry SC_FN(SndResource_t, 0x00419ce0)
#define snd_ResourceGet SC_FN(SndResource_t, 0x00419fa0)
#define snd_ResourceExists SC_FN(SndResourceExists_t, 0x00419d10)
#define snd_ResourceForget SC_FN(SndResourceForget_t, 0x0041a450)
#define snd_FileOpen SC_FN(SndFileOpen_t, 0x00411780)
#define snd_FileReadLine SC_FN(SndFileReadLine_t, 0x004119b0)
#define snd_FileClose SC_FN(SndFileClose_t, 0x00411850)
#define snd_Sound3D_Create SC_FN(Sound3DCreate_t, 0x00472560)
#define snd_WorldGetFocusCar SC_FN(SndInt_t, 0x004626d0)
#define snd_WorldGetPlayerCar SC_FN(SndInt_t, 0x00462710)
#define snd_MixerGetQuality SC_FN(SndInt_t, 0x00473b70)
#define snd_MemAlloc SC_FN(SndMemAlloc_t, 0x004140e0)
#define snd_operator_delete SC_FN(SndDelete_t, 0x00414390)
#define snd_EngineSoundSample_ctor SC_FN(SampleCtor_t, 0x00472db0)
#define snd_EngineSoundSample_GetVolume SC_FN(SampleGetVolume_t, 0x00472df0)
#define snd_EngineSoundSample_Update SC_FN(SampleUpdate_t, 0x00472ed0)
#define snd_TireSound_ctor SC_FN(TireSoundCtor_t, 0x004731a0)
#define snd_RealTireSound_ctor SC_FN(RealTireSoundCtor_t, 0x004732f0)
#define snd_RealTireSound_dtor SC_FN(SndDtor_t, 0x004733c0)

// a sound's scalar deleting destructor, through its vtable (slot 0)
static __forceinline void snd_vdelete(void* obj, unsigned flags) { ((SndVDel_t)(*(void***)obj)[0])(obj, 0, flags); }
// the Car's virtuals: +0x44 GetPerceivedThrottle, +0x48 GetPerceivedRPM
static __forceinline double car_vfloat(uint8_t* car, uint32_t off) { return ((CarFloat_t)(*(void***)car)[off / 4])(car, 0); }
}  // namespace

enum : uint32_t {
    VT_TireSound = 0x004dd790,
    VT_DummyTireSound = 0x004dd7a0,
    VT_RealTireSound = 0x004dd7b0,
    SND_TIRE_TABLE = 0x00578930,                    // TireSound* [16][4]
    SND_REAL_WHEEL_BY_QUALITY = 0x004dd748,         // int [3 qualities][2: other car, player's car]: 1 1 / 2 2 / 2 3
    SND_FOURCC_ENS = 0x45534658,                    // 'XFSE'
};

// ====================================================================================================================
// engsnd.obj
// ====================================================================================================================

// ---- EngineSoundSample::EngineSoundSample (0x472db0): the defaults ---------------------------------------------------
static EngineSoundSample* __fastcall EngineSoundSample_ctor(EngineSoundSample* self, Edx) {
    put4(&self->rpm_full, 0x447a0000);              // 1000
    put4(&self->rpm_peak, 0x45bb8000);              // 6000
    put4(&self->rpm_off, 0x45dac000);               // 7000
    put4(&self->rpm_on, 0);
    put4(&self->vol_lo, 0x3ecccccd);                // 0.4
    put4(&self->vol_hi, 0x3f000000);                // 0.5
    put4(&self->ref_rpm, 0x453b8000);               // 3000
    return self;
}
static void fp_sample_ctor(Footprint& f, EngineSoundSample* self, Edx) {
    f.add(self, sizeof(EngineSoundSample), "engine sample");   // (not `pure`: the fuzzer can't compare the `this` it returns)
}
PORT_FN(0x00472db0, "EngineSoundSample::EngineSoundSample", EngineSoundSample_ctor, fp_sample_ctor)

// ---- EngineSoundSample::GetVolume (0x472df0) -------------------------------------------------------------------------
// Silent outside [rpm_on, rpm_off). t = (rpm - rpm_full) / (rpm_peak - rpm_full), stored; by t's BITS: above 1.0
// (and a positive NaN) the plateau's top blend with the ramp down (rpm - rpm_off) / (rpm_peak - rpm_off); negative
// (not -0; and a negative NaN) no blend, with the ramp up (rpm - rpm_on) / (rpm_full - rpm_on); else the blend t at
// full volume. Returns ((vol_hi - vol_lo) * blend + vol_lo) * ramp, unrounded in ST0.
// FIX: engine data with rpm_full == rpm_peak (or rpm_on == rpm_full, rpm_peak == rpm_off, on the branches that use
// them) divided by zero -- a fault on the BG thread, where divide-by-zero is unmasked. Each division by 0 now gives
// the masked FPU's result (div_masked), so a zero-width plateau works as the data means it (t is +-infinity: the ramp
// up below rpm_full, the ramp down above it); a zero-width ramp is reached only with its corners out of order, and its
// +-infinite ramp is what the original computes with the exception masked (-infinity: silent; +infinity: the mixer's
// SetVolume takes it as 1.0).
static double __fastcall EngineSoundSample_GetVolume(EngineSoundSample* self, Edx, float rpm) {
    if (D(self->rpm_on) > rpm) return 0.0;          // fld [0]; fcomp rpm; test ah,0x41; je
    if (!(D(self->rpm_off) > rpm)) return 0.0;      // fld [0xc]; fcomp rpm; test ah,0x41; jne
    const float t = st(div_masked(D(rpm) - self->rpm_full, D(self->rpm_peak) - self->rpm_full));
    float blend, ramp;                               // [esp], [esp+4]
    put4(&ramp, 0x3f800000);
    const uint32_t tb = Ub(&t);
    if ((int32_t)tb > 0x3f800000) {
        put4(&blend, 0x3f800000);
        ramp = st(div_masked(D(rpm) - self->rpm_off, D(self->rpm_peak) - self->rpm_off));
    } else if (tb > 0x80000000u) {
        put4(&blend, 0);
        ramp = st(div_masked(D(rpm) - self->rpm_on, D(self->rpm_full) - self->rpm_on));
    } else {
        cp4(&blend, &t);
    }
    return ((D(self->vol_hi) - self->vol_lo) * blend + self->vol_lo) * ramp;
}
static void fp_sample_volume(Footprint& f, EngineSoundSample*, Edx, float) { f.pure = true; }
PORT_FN(0x00472df0, "EngineSoundSample::GetVolume", EngineSoundSample_GetVolume, fp_sample_volume)

// ---- EngineSoundSample::Update (0x472ed0): one band's Sound3D --------------------------------------------------------
// volume = GetVolume(rpm) x the throttle volume: not above 0 (or NaN) and the sound is switched off. Else pitch =
// rpm / ref_rpm, the Sound3D's volume 0.6 x volume; each written (and flagged) only where it changed.
// FIX: a ref_rpm of 0 (engine data) divided by zero on the BG thread; it now gives the masked FPU's result
// (div_masked): an infinite pitch, which the software mixer's SetFrequency clamps to its top (16x; -infinity, from a
// ref_rpm of -0, to its bottom).
static void __fastcall EngineSoundSample_Update(EngineSoundSample* self, Edx, float rpm, Sound3D* s, float volume) {
    const double v = snd_EngineSoundSample_GetVolume(self, 0, Ub(&rpm)) * D(volume);
    // fcom 0; fstp [esp+4]: an overflowing store faults at the NEXT x87 instruction, which on the switch-off path is
    // after `off` is written (in the caller): a lone fstp here too (st_to)
    const bool on = v > 0.0;
    float vf;
    st_to(&vf, v);
    if (!on) {
        s->off = 1;
        return;
    }
    const float pitch = st(div_masked(D(rpm), D(self->ref_rpm)));
    const double v6 = D(vf) * FB(0x3f19999a);       // 0.6
    const float v6f = st(v6);                       // fcom [s+8]; fstp
    if (v6 < s->volume || v6 > s->volume) {         // test ah,0x40: differs, ordered
        s->volume_changed = 1;
        cp4(&s->volume, &v6f);
    }
    if (D(s->pitch) < pitch || D(s->pitch) > pitch) {
        s->pitch_changed = 1;
        cp4(&s->pitch, &pitch);
    }
    s->keep = 1;
    s->on = 1;
    s->off = 0;
}
static void fp_sample_update(Footprint& f, EngineSoundSample*, Edx, float, Sound3D* s, float) {
    if (s) f.add((uint8_t*)s + 4, 0x2a, "engine band Sound3D");
    f.pure = true;
}
PORT_FN(0x00472ed0, "EngineSoundSample::Update", EngineSoundSample_Update, fp_sample_update)

// ---- EngineSound::EngineSound (0x4728f0) -----------------------------------------------------------------------------
// The bands from <car>e.ens, else engine.ens (a count, then 7 floats a band): the idle "<car>i.sfx" and a Sound3D per
// band, "<car><n>.sfx" or else "engine<n>.sfx", both following the car (+0x234, +0x5c), stopping at the first sound
// that doesn't load. With neither resource, engine.txt: a line per band, up to 7, each band's "engine<n>.sfx" (no
// idle sound).
// FIX: the resource's count was unchecked: an 8th band wrote its Sound3D over `idle` and its floats over
// band[0..6], and a 9th and more ran past the 0xf8-byte object (band n at +0xd4 + 4n, sample n at +0x10 + 0x1c n).
// The bands live inside the object (EngineSound::Create's MemAlloc(0xf8), and every reader's fixed offsets), so the
// limit isn't lifted: the first 7 bands are read, the rest ignored, and the log says so once.
// FIX: engine.txt missing too -- `count` was never written (the object is MemAlloc'd: garbage), and Update and the
// destructor walked that many bands. It is 0 now: no bands, as with an empty resource.
// FIX: engine.txt's bands keep a NULL Sound3D (a missing engine<n>.sfx); Update's switch-off branches wrote through
// it -- fixed there. And the loop creates band n's Sound3D before reading its line, so the band whose line is missing
// or fails to parse (always the last, short of 7) was left uncounted: never updated, never deleted. It is deleted.
// FIX: a car name of 27 characters or more (31 is the most Car::Setup keeps) overflowed the 32-byte "%se.ens" buffer
// into the next one -- harmless, that one was unused at that point. The buffer holds any such name now; the resource
// name is the same, so it's looked up (and not found) as before.
static bool g_said_ens_bands;                       // (FIX: the band limit is logged once; DLL memory, not the game's)
static EngineSound* __fastcall EngineSound_ctor(EngineSound* self, Edx, uint8_t* car) {
    uint8_t* sample = (uint8_t*)self + 0x10;
    for (int i = 6; i >= 0; i--) {                  // dec edi; jns: 7 samples
        snd_EngineSoundSample_ctor(sample, 0);
        sample += 0x1c;
    }
    self->car = car;
    const char* name = (const char*)(car + 0x514);
    struct {                                        // the original frame's buffers, in its order ([esp+0x24..0x164))
        char b24[32];                               // engine.txt's "engine<n>.sfx"
        char b44[VP_FIX ? 32 + 16 : 32];            // "<car>e.ens" (FIX: room for a 31-character name + "e.ens")
        char b64[256];                              // the .sfx names; engine.txt's line
    } fr;
    self->car_index = *(const int32_t*)(car + 0x510);
    snd_sprintf(fr.b44, SC_S(0x004f5838), name);    // "%se.ens"
    self->idle = 0;
    uint32_t size;                                  // [esp+0x20]
    int32_t flags;                                  // [esp+0x1c]
    uint8_t* res = snd_ResourceTry(fr.b44, SND_FOURCC_ENS, &size, &flags, 0, 0);
    if (!res) {
        snd_LogReport(SC_S(0x004f5840), fr.b44);    // "Can't find %s..."
        res = snd_ResourceGet(SC_S(0x004f5854), SND_FOURCC_ENS, &size, &flags, 0, 0);   // "engine.ens"
    }
    if (res) {
        snd_sprintf(fr.b64, SC_S(0x004f5860), name);                                    // "%si.sfx"
        int32_t n = 0;
        self->idle = snd_Sound3D_Create(fr.b64, 2, car + 0x234, car + 0x5c);
        if (*(volatile int32_t*)res > n) {
            const uint8_t* rec = res + 4;
            uint8_t* smp = (uint8_t*)self + 0x10;
            uint8_t* slot = (uint8_t*)self + 0xd4;
            do {
                snd_sprintf(fr.b64, SC_S(0x004f5868), name, n);                         // "%s%d.sfx"
                if (!snd_ResourceExists(fr.b64)) snd_sprintf(fr.b64, SC_S(0x004f5874), n);   // "engine%d.sfx"
                Sound3D* s = snd_Sound3D_Create(fr.b64, 2, car + 0x234, car + 0x5c);
                *(Sound3D* volatile*)slot = s;
                if (*(volatile uint32_t*)slot == 0) break;
                cp4(smp + 0x18, rec + 0x00);        // ref_rpm
                cp4(smp + 0x10, rec + 0x04);        // vol_lo
                cp4(smp + 0x14, rec + 0x08);        // vol_hi
                cp4(smp + 0x00, rec + 0x0c);        // rpm_on
                cp4(smp + 0x04, rec + 0x10);        // rpm_full
                cp4(smp + 0x08, rec + 0x14);        // rpm_peak
                cp4(smp + 0x0c, rec + 0x18);        // rpm_off
                rec += 0x1c;
                smp += 0x1c;
                slot += 4;
                n++;
                if (VP_FIX && n == 7 && *(volatile int32_t*)res > 7) {   // FIX: the 8th and on don't fit the object
                    SND_FIX_FIRED();
                    if (!g_said_ens_bands) {
                        g_said_ens_bands = true;
                        logf("fix: engine sound: car %.31s's engine resource has %d bands; an engine holds 7, the rest are "
                             "ignored (logged once)", name, *(volatile int32_t*)res);
                    }
                    break;
                }
            } while (*(volatile int32_t*)res > n);
        }
        *(volatile int32_t*)&self->count = n;
        snd_ResourceForget(res);
        return self;
    }
    snd_LogReport(SC_S(0x004f5884));                // "Using engine.txt..."
    int32_t fh = snd_FileOpen(SC_S(0x004f5898));    // "engine.txt"
    if (!fh) {
        snd_LogReport(SC_S(0x004f58d0), SC_S(0x004f5898));   // "Can't find %s"
        if (VP_FIX) {                                // FIX: no bands (count was left as MemAlloc garbage)
            SND_FIX_FIRED();
            *(volatile int32_t*)&self->count = 0;
        }
        return self;
    }
    int32_t n = 0;
    uint8_t* smp = (uint8_t*)self + 0x10;
    uint8_t* slot = (uint8_t*)self + 0xd4;
    for (;;) {
        snd_sprintf(fr.b24, SC_S(0x004f58a4), n);   // "engine%d.sfx"
        *(Sound3D* volatile*)slot = snd_Sound3D_Create(fr.b24, 2, car + 0x234, car + 0x5c);
        fr.b64[0] = *(const volatile char*)SC_S(0x004f58b4);   // char line[256] = "";
        memset(fr.b64 + 1, 0, 255);
        bool ok = snd_FileReadLine(fh, fr.b64, 0x100) != 0;
        if (ok)
            ok = snd_sscanf(fr.b64, SC_S(0x004f58b8), smp + 0x18, smp + 0x10, smp + 0x14, smp, smp + 4, smp + 8, smp + 0xc) == 7;
        if (!ok) {
            if (VP_FIX) {                            // FIX: the uncounted band's Sound3D is deleted (it leaked)
                if (Sound3D* s = *(Sound3D* volatile*)slot) {
                    SND_FIX_FIRED();
                    snd_vdelete(s, 1);
                    *(Sound3D* volatile*)slot = 0;
                }
            }
            break;
        }
        smp += 0x1c;
        slot += 4;
        if (++n >= 7) break;
    }
    *(volatile int32_t*)&self->count = n;
    snd_FileClose(&fh);
    return self;
}
static void fp_engine_sound_ctor(Footprint& f, EngineSound*, Edx, uint8_t*) {
    f.replay_only = "creates its Sound3Ds (allocates), reads the engine resource or engine.txt";
}
PORT_FN(0x004728f0, "EngineSound::EngineSound", EngineSound_ctor, fp_engine_sound_ctor)

// ---- EngineSound::~EngineSound (0x472c10): deletes the bands' and the idle Sound3Ds ----------------------------------
static void __fastcall EngineSound_dtor(EngineSound* self, Edx) {
    uint8_t* slot = (uint8_t*)self + 0xd4;
    for (int32_t i = 0; *(volatile int32_t*)&self->count > i; i++, slot += 4)
        if (Sound3D* s = *(Sound3D* volatile*)slot) snd_vdelete(s, 1);
    if (Sound3D* s = *(Sound3D* volatile*)&self->idle) snd_vdelete(s, 1);
}
static void fp_engine_sound_dtor(Footprint& f, EngineSound*, Edx) { f.replay_only = "deletes its Sound3Ds"; }
PORT_FN(0x00472c10, "EngineSound::~EngineSound", EngineSound_dtor, fp_engine_sound_dtor)

// ---- EngineSound::Update (0x472c60): every physics tick, from Car::UpdateCommon ---------------------------------------
// The idle sound plays (volume 0.25) below 1600 RPM (by the stored RPM's bits) with the throttle under 0.1. The band
// volume is throttle x 0.15625 + 0.09375, x 2.6666667 for the car in view, x 2.2. Band 0 is for the other cars, bands
// 1.. for the car in view: the rest are switched off (with no NULL check, below); `stalled` switches all of them off.
// FIX: the switch-off of a band that belongs to the other view wrote through its Sound3D unchecked -- a NULL band
// (engine.txt with a missing engine<n>.sfx) faulted, where the normal path checks. A NULL band is skipped there too.
static void __fastcall EngineSound_Update(EngineSound* self, Edx, uint8_t stalled) {
    if (*(volatile int32_t*)&self->count == 0) return;
    const float rpm = st(car_vfloat(self->car, 0x48));              // GetPerceivedRPM; fstp [esp+0x14]
    if (*(Sound3D* volatile*)&self->idle) {
        bool on = false;
        if ((int32_t)Ub(&rpm) < 0x44c80000) {                         // below 1600, as integers
            const double thr = car_vfloat(self->car, 0x44);           // GetPerceivedThrottle
            on = !(thr >= FB(0x3dcccccd));                            // fcomp 0.1; test ah,1; je: off
        }
        if (on) {
            Sound3D* s = *(Sound3D* volatile*)&self->idle;
            s->off = 0;
            s->keep = 1;
            s->on = 1;
            s = *(Sound3D* volatile*)&self->idle;
            if (Ub(&s->volume) != 0x3e800000) {
                s->volume_changed = 1;
                put4(&s->volume, 0x3e800000);                         // 0.25
            }
        } else {
            (*(Sound3D* volatile*)&self->idle)->off = 1;
        }
    }
    float vol = st(car_vfloat(self->car, 0x44) * FB(0x3e200000) + FB(0x3dc00000));   // x 0.15625 + 0.09375
    if (snd_WorldGetFocusCar() == self->car_index) vol = st(D(vol) * FB(0x402aaaab)); // x 2.6666667
    vol = st(D(vol) * FB(0x400ccccd));                                                 // x 2.2
    const bool in_view = snd_WorldGetFocusCar() == self->car_index;
    uint8_t* slot = (uint8_t*)self + 0xd4;
    uint8_t* sample = (uint8_t*)self + 0x10;
    for (int32_t i = 0; *(volatile int32_t*)&self->count > i; i++, slot += 4, sample += 0x1c) {
        if (i == 0 ? in_view : !in_view) {
            Sound3D* s = *(Sound3D* volatile*)slot;
            if (VP_FIX && !s) {                      // FIX: a NULL band has nothing to switch off
                SND_FIX_FIRED();
                continue;
            }
            s->off = 1;
            continue;
        }
        Sound3D* s = *(Sound3D* volatile*)slot;
        if (!s) continue;
        if (stalled) {
            s->off = 1;
            continue;
        }
        snd_EngineSoundSample_Update(sample, 0, Ub(&rpm), s, Ub(&vol));
    }
}
// the bands' and the idle Sound3Ds: pitch, volume and flags (+4..+0x2d). A count past 7 (the original constructor's
// resource overflow, fixed above) reads `idle` and the count as bands: capped here, as phys_car_update.cpp's
// UpdateCommon footprint does. (The fixed switch-off writes a subset: NULL bands are skipped.)
static void fp_engine_sound_update(Footprint& f, EngineSound* self, Edx, uint8_t) {
    const int32_t n = self->count;
    for (int32_t i = 0; i < n && i < 7; i++)
        if (Sound3D* s = self->band[i]) f.add((uint8_t*)s + 4, 0x2a, "engine band Sound3D");
    if (Sound3D* s = self->idle) f.add((uint8_t*)s + 4, 0x2a, "engine idle Sound3D");
}
PORT_FN(0x00472c60, "EngineSound::Update", EngineSound_Update, fp_engine_sound_update)

// ---- engsnd.obj's static initialisers --------------------------------------------------------------------------------
static void fp_snd_static_init(Footprint& f) { f.replay_only = "a static initialiser (runs once, from the CRT's _initterm)"; }
static void __cdecl engsnd_E1_rw() { snd_rcfunc_is_internal(); }
PORT_FN(0x004726e0, "$E1(engsnd.obj)", engsnd_E1_rw, fp_snd_static_init)
static void __cdecl engsnd_E2_rw() { SC_FN(SndVoid_t, 0x004726e0)(); }
PORT_FN(0x004726d0, "$E2(engsnd.obj)", engsnd_E2_rw, fp_snd_static_init)
// each pair: $E(3k) calls $E(3k-1), which stores one colour constant
static void __cdecl engsnd_E5_rw() { SC_G32(0x005788a8) = 0x00000000u; }
PORT_FN(0x00472700, "$E5(engsnd.obj)", engsnd_E5_rw, fp_snd_static_init)
static void __cdecl engsnd_E6_rw() { SC_FN(SndVoid_t, 0x00472700)(); }
PORT_FN(0x004726f0, "$E6(engsnd.obj)", engsnd_E6_rw, fp_snd_static_init)
static void __cdecl engsnd_E8_rw() { SC_G32(0x005788c4) = 0x001f001fu; }
PORT_FN(0x00472720, "$E8(engsnd.obj)", engsnd_E8_rw, fp_snd_static_init)
static void __cdecl engsnd_E9_rw() { SC_FN(SndVoid_t, 0x00472720)(); }
PORT_FN(0x00472710, "$E9(engsnd.obj)", engsnd_E9_rw, fp_snd_static_init)
static void __cdecl engsnd_E11_rw() { SC_G32(0x005788b4) = 0x03e001e0u; }
PORT_FN(0x00472740, "$E11(engsnd.obj)", engsnd_E11_rw, fp_snd_static_init)
static void __cdecl engsnd_E12_rw() { SC_FN(SndVoid_t, 0x00472740)(); }
PORT_FN(0x00472730, "$E12(engsnd.obj)", engsnd_E12_rw, fp_snd_static_init)
static void __cdecl engsnd_E14_rw() { SC_G32(0x005788c8) = 0x03ef01efu; }
PORT_FN(0x00472760, "$E14(engsnd.obj)", engsnd_E14_rw, fp_snd_static_init)
static void __cdecl engsnd_E15_rw() { SC_FN(SndVoid_t, 0x00472760)(); }
PORT_FN(0x00472750, "$E15(engsnd.obj)", engsnd_E15_rw, fp_snd_static_init)
static void __cdecl engsnd_E17_rw() { SC_G32(0x005788e0) = 0x78003c00u; }
PORT_FN(0x00472780, "$E17(engsnd.obj)", engsnd_E17_rw, fp_snd_static_init)
static void __cdecl engsnd_E18_rw() { SC_FN(SndVoid_t, 0x00472780)(); }
PORT_FN(0x00472770, "$E18(engsnd.obj)", engsnd_E18_rw, fp_snd_static_init)
static void __cdecl engsnd_E20_rw() { SC_G32(0x005788c0) = 0x780f3c0fu; }
PORT_FN(0x004727a0, "$E20(engsnd.obj)", engsnd_E20_rw, fp_snd_static_init)
static void __cdecl engsnd_E21_rw() { SC_FN(SndVoid_t, 0x004727a0)(); }
PORT_FN(0x00472790, "$E21(engsnd.obj)", engsnd_E21_rw, fp_snd_static_init)
static void __cdecl engsnd_E23_rw() { SC_G32(0x005788d4) = 0x7be03de0u; }
PORT_FN(0x004727c0, "$E23(engsnd.obj)", engsnd_E23_rw, fp_snd_static_init)
static void __cdecl engsnd_E24_rw() { SC_FN(SndVoid_t, 0x004727c0)(); }
PORT_FN(0x004727b0, "$E24(engsnd.obj)", engsnd_E24_rw, fp_snd_static_init)
static void __cdecl engsnd_E26_rw() { SC_G32(0x005788bc) = 0x7bef3defu; }
PORT_FN(0x004727e0, "$E26(engsnd.obj)", engsnd_E26_rw, fp_snd_static_init)
static void __cdecl engsnd_E27_rw() { SC_FN(SndVoid_t, 0x004727e0)(); }
PORT_FN(0x004727d0, "$E27(engsnd.obj)", engsnd_E27_rw, fp_snd_static_init)
static void __cdecl engsnd_E29_rw() { SC_G32(0x005788d0) = 0x42082108u; }
PORT_FN(0x00472800, "$E29(engsnd.obj)", engsnd_E29_rw, fp_snd_static_init)
static void __cdecl engsnd_E30_rw() { SC_FN(SndVoid_t, 0x00472800)(); }
PORT_FN(0x004727f0, "$E30(engsnd.obj)", engsnd_E30_rw, fp_snd_static_init)
static void __cdecl engsnd_E32_rw() { SC_G32(0x005788a4) = 0x001f001fu; }
PORT_FN(0x00472820, "$E32(engsnd.obj)", engsnd_E32_rw, fp_snd_static_init)
static void __cdecl engsnd_E33_rw() { SC_FN(SndVoid_t, 0x00472820)(); }
PORT_FN(0x00472810, "$E33(engsnd.obj)", engsnd_E33_rw, fp_snd_static_init)
static void __cdecl engsnd_E35_rw() { SC_G32(0x005788dc) = 0x07e003e0u; }
PORT_FN(0x00472840, "$E35(engsnd.obj)", engsnd_E35_rw, fp_snd_static_init)
static void __cdecl engsnd_E36_rw() { SC_FN(SndVoid_t, 0x00472840)(); }
PORT_FN(0x00472830, "$E36(engsnd.obj)", engsnd_E36_rw, fp_snd_static_init)
static void __cdecl engsnd_E38_rw() { SC_G32(0x005788b0) = 0x07ff03ffu; }
PORT_FN(0x00472860, "$E38(engsnd.obj)", engsnd_E38_rw, fp_snd_static_init)
static void __cdecl engsnd_E39_rw() { SC_FN(SndVoid_t, 0x00472860)(); }
PORT_FN(0x00472850, "$E39(engsnd.obj)", engsnd_E39_rw, fp_snd_static_init)
static void __cdecl engsnd_E41_rw() { SC_G32(0x005788ac) = 0xf8007c00u; }
PORT_FN(0x00472880, "$E41(engsnd.obj)", engsnd_E41_rw, fp_snd_static_init)
static void __cdecl engsnd_E42_rw() { SC_FN(SndVoid_t, 0x00472880)(); }
PORT_FN(0x00472870, "$E42(engsnd.obj)", engsnd_E42_rw, fp_snd_static_init)
static void __cdecl engsnd_E44_rw() { SC_G32(0x005788e4) = 0xf81f7c1fu; }
PORT_FN(0x004728a0, "$E44(engsnd.obj)", engsnd_E44_rw, fp_snd_static_init)
static void __cdecl engsnd_E45_rw() { SC_FN(SndVoid_t, 0x004728a0)(); }
PORT_FN(0x00472890, "$E45(engsnd.obj)", engsnd_E45_rw, fp_snd_static_init)
static void __cdecl engsnd_E47_rw() { SC_G32(0x005788cc) = 0xffe07fe0u; }
PORT_FN(0x004728c0, "$E47(engsnd.obj)", engsnd_E47_rw, fp_snd_static_init)
static void __cdecl engsnd_E48_rw() { SC_FN(SndVoid_t, 0x004728c0)(); }
PORT_FN(0x004728b0, "$E48(engsnd.obj)", engsnd_E48_rw, fp_snd_static_init)
static void __cdecl engsnd_E50_rw() { SC_G32(0x005788b8) = 0xffff7fffu; }
PORT_FN(0x004728e0, "$E50(engsnd.obj)", engsnd_E50_rw, fp_snd_static_init)
static void __cdecl engsnd_E51_rw() { SC_FN(SndVoid_t, 0x004728e0)(); }
PORT_FN(0x004728d0, "$E51(engsnd.obj)", engsnd_E51_rw, fp_snd_static_init)

// ====================================================================================================================
// tiresnd.obj
// ====================================================================================================================

// ---- TireSound::TireSound (0x4731a0) ---------------------------------------------------------------------------------
static TireSound* __fastcall TireSound_ctor(TireSound* self, Edx, int wheel_index, uint8_t* wheel) {
    self->vtable = (void**)VT_TireSound;
    self->wheel_index = wheel_index;
    self->wheel = wheel;
    return self;
}
static void fp_tire_sound_ctor(Footprint& f, TireSound* self, Edx, int, uint8_t*) {
    f.add(self, sizeof(TireSound), "tire sound");             // (not `pure`: it returns `this`)
}
PORT_FN(0x004731a0, "TireSound::TireSound", TireSound_ctor, fp_tire_sound_ctor)

// ---- TireSound::Create (0x4731c0): from Wheel::Setup, wheels 0..3 in order ---------------------------------------------
// RealWheel by the mixer quality and whether this is the player's car: low 1 (one sound for the car, on wheel 3),
// medium 2 (one an axle, on wheels 1 and 3), high: the player's car 3 (every wheel), the others 2. A wheel with a
// real sound gets it; the rest a DummyTireSound. The car's and the axle's sound take over the wheels created before it
// (their dummies, from the table), and every wheel's sound goes in the table.
// FIX: a failed MemAlloc of the RealTireSound (RealWheel 1 or 2) wrote the siblings through NULL + 0xc. With no
// sound there are no siblings to take over: the NULL goes in the table, as the original's does.
// FIX: the table has 16 cars; a car index outside 0..15 wrote past it (into the mixer's statics at 0x578a30) or
// before it (tiresnd.obj's colour constants), and read its siblings from there. Such a car's sounds are neither read
// from nor put in the table: an axle's or the car's sound speaks for its own wheel only. Not lifted: 16 is the game's
// car limit everywhere (PhysTaskRegisterCar's Car*[16] at 0x520c18, the car status strings, ...), so a car index past
// 15 has already overrun those before its wheels are set up; a bigger table here alone would buy nothing.
// FIX: a mixer quality outside 0..2 read past the RealWheel table (0x4dd760: 0, then 0x3f333333, the gains...) and
// with that as the RealWheel past the local flag array. MixerSetQuality keeps the quality to 0..2, so only a quality
// written some other way gets here; it now gives RealWheel 0, no squeal (every wheel a dummy) -- what the original
// reads for quality 3 on a car not the player's, the one case past the table that didn't crash.
static TireSound* __cdecl TireSound_Create(int car_index, int wheel, uint8_t* w, uint8_t* car) {
    const int quality = snd_MixerGetQuality();
    const uint8_t is_real[16] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1};   // [RealWheel][wheel]
    const uint32_t players = snd_WorldGetPlayerCar() == car_index ? 1u : 0u;
    int32_t mode;
    if (VP_FIX && (uint32_t)quality > 2u) {           // FIX: past the RealWheel table: no squeal
        SND_FIX_FIRED();
        mode = 0;
    } else {
        mode = *(const volatile int32_t*)(uintptr_t)(SND_REAL_WHEEL_BY_QUALITY + 4u * (players + (uint32_t)quality * 2u));
    }
    const bool in_table = !VP_FIX || (uint32_t)car_index < 16u;   // FIX: a car index the table has
    if (!in_table) SND_FIX_FIRED();
    uint8_t* r;
    if (is_real[wheel + mode * 4]) {
        void* m = snd_MemAlloc(0x3c);
        r = m ? (uint8_t*)snd_RealTireSound_ctor(m, 0, car_index, wheel, w, car, mode) : 0;
        if (VP_FIX && (!r || !in_table)) {             // FIX: no sound, or no table row: no siblings
            if (!r && (mode == 1 ? wheel > 0 : mode == 2)) SND_FIX_FIRED();
        } else if (mode == 1) {
            if (wheel > 0) {
                uint32_t src = ((uint32_t)car_index << 4) + SND_TIRE_TABLE;
                uint8_t* dst = r + 0xc;
                for (int k = wheel; k != 0; k--, src += 4, dst += 4) put4(dst, SC_G32(src));
            }
        } else if (mode == 2) {
            const uint32_t e = (uint32_t)wheel & 0xfffffffeu;
            put4(r + e * 4 + 0xc, SC_G32(SND_TIRE_TABLE + (e + (uint32_t)car_index * 4u) * 4u));
        }
    } else {
        uint8_t* m = (uint8_t*)snd_MemAlloc(0x10);
        r = 0;
        if (m) {
            snd_TireSound_ctor(m, 0, wheel, w);
            r = m;
            ((DummyTireSound*)m)->vtable = (void**)VT_DummyTireSound;
            ((DummyTireSound*)m)->ok = 1;
        }
    }
    if (in_table) SC_G32(SND_TIRE_TABLE + ((uint32_t)wheel + (uint32_t)car_index * 4u) * 4u) = (uint32_t)(uintptr_t)r;
    return (TireSound*)r;
}
static void fp_tire_sound_create(Footprint& f, int, int, uint8_t*, uint8_t*) {
    f.replay_only = "allocates the tyre sound (and its Sound3D)";
}
PORT_FN(0x004731c0, "TireSound::Create", TireSound_Create, fp_tire_sound_create)

// ---- RealTireSound::RealTireSound (0x4732f0) -------------------------------------------------------------------------
// FIX: a RealWheel outside 0..3 LogPanic'd (the game's ends the run with a deliberate crash; a harness's returns and
// leaves the gain unset: MemAlloc garbage, multiplied into the volume every tick). It is logged instead (the same
// message, LogReport) and the gain is 0: a silent sound. TireSound::Create, the only caller, now passes 0..3 always.
static RealTireSound* __fastcall RealTireSound_ctor(RealTireSound* self, Edx, int car_index, int wheel, uint8_t* w,
                                                    uint8_t* car, int mode) {
    snd_TireSound_ctor(self, 0, wheel, w);
    self->vtable = (void**)VT_RealTireSound;
    switch ((uint32_t)mode) {
    case 0: put4(&self->gain, 0); break;
    case 1: put4(&self->gain, 0x3f333333); break;   // 0.7
    case 2: put4(&self->gain, 0x3f2147ae); break;   // 0.63
    case 3: put4(&self->gain, 0x3f0ccccd); break;   // 0.55
    default:
        if (VP_FIX) {                               // FIX: logged, not a panic; silent
            SND_FIX_FIRED();
            snd_LogReport(SC_S(0x004f5900), mode);
            put4(&self->gain, 0);
            break;
        }
        snd_LogPanic(SC_S(0x004f5900), mode);       // "RealTireSound: Lame programmer %d"
    }
    self->wheel_sound[0] = 0;
    self->wheel_sound[1] = 0;
    self->wheel_sound[2] = 0;
    self->wheel_sound[3] = 0;
    *(RealTireSound**)((uint8_t*)self + 0xc + (uint32_t)wheel * 4u) = self;
    self->car = car;
    self->car_index = car_index;
    Sound3D* s = snd_Sound3D_Create(SC_S(0x004f5924), 3, car + 0x234, &self->pos);   // "squeal.sfx"
    self->sound = s;
    if (s) s->owner = car_index;
    return self;
}
static void fp_real_tire_sound_ctor(Footprint& f, RealTireSound*, Edx, int, int, uint8_t*, uint8_t*, int) {
    f.replay_only = "creates its Sound3D (allocates)";
}
PORT_FN(0x004732f0, "RealTireSound::RealTireSound", RealTireSound_ctor, fp_real_tire_sound_ctor)

// ---- RealTireSound::~RealTireSound (0x4733c0) ------------------------------------------------------------------------
static void __fastcall RealTireSound_dtor(RealTireSound* self, Edx) {
    *(void** volatile*)&self->vtable = (void**)VT_RealTireSound;
    if (Sound3D* s = self->sound) {
        snd_vdelete(s, 1);
        self->sound = 0;
    }
    *(void** volatile*)&self->vtable = (void**)VT_TireSound;
}
static void fp_real_tire_sound_dtor(Footprint& f, RealTireSound*, Edx) { f.replay_only = "deletes its Sound3D"; }
PORT_FN(0x004733c0, "RealTireSound::~RealTireSound", RealTireSound_dtor, fp_real_tire_sound_dtor)

// ---- RealTireSound::Update (0x4733f0): every physics tick, from Wheel::UpdateCommon ------------------------------------
// Each wheel it speaks for squeals while its slide x 2 is above 0 on surface class 0: pitch 1.4 / (min(0.15 x slide
// x 2, 1) + 1.1), volume min(0.6 x slide x 2, 0.8). The sound sits at the squealing wheels' average contact point, at
// their average pitch and the average volume x gain x 0.5; with none squealing its volume goes to 0.
static void __fastcall RealTireSound_Update(RealTireSound* self, Edx) {
    if (!self->sound) return;
    struct Squeal { uint8_t on; float pitch, volume; } q[4];       // [esp+0x28], 12 bytes each
    volatile float n;                                              // [esp+0x14]: how many squeal
    put4((void*)&n, 0);
    for (int i = 0; i < 4; i++) {
        q[i].on = 0;
        TireSound* ts = self->wheel_sound[i];
        if (!ts) continue;
        const uint8_t* w = ts->wheel;
        const double s2 = D(*wheel_slide(w)) * FB(0x40000000);    // x 2
        const float s2f = st(s2);                                  // fcom 0; fstp [esp+0x10]
        if (s2 > 0.0 && wheel_surface_class(w) == 0) {             // test ah,0x41: above 0, ordered
            const double p = D(s2f) * FB(0x3e19999a);              // x 0.15
            volatile float k_one = FB(0x3f800000);                 // fld 1.0: loaded, not folded into the + 1.1
            const double one = k_one;
            const double m = one > p ? p : one;                    // fcom st(1); test ah,0x41; je: keep p
            q[i].on = 1;
            q[i].pitch = st(D(FB(0x3fb33333)) / (m + FB(0x3f8ccccd)));   // 1.4 / (m + 1.1): fdivr
            const double v = D(s2f) * FB(0x3f19999a);              // x 0.6
            const double c = D(FB(0x3f4ccccd));                    // 0.8
            q[i].volume = st(c > v ? v : c);
        }
        if (q[i].on) n = st(D(n) + FB(0x3f800000));
    }
    volatile float sx, sy, sz;                                     // [esp+0x1c], [0x20], [0x24]
    put4((void*)&sx, 0);
    put4((void*)&sy, 0);
    put4((void*)&sz, 0);
    double pitch_sum = 0.0, volume_sum = 0.0;                      // fld [esp+0x10] / [esp+0x18], both 0
    for (int i = 0; i < 4; i++) {
        if (!q[i].on) continue;
        const float* c = wheel_droop_point(self->wheel_sound[i]->wheel);
        sx = st(D(c[0]) + sx);
        sy = st(D(c[1]) + sy);
        sz = st(D(c[2]) + sz);
        volume_sum = volume_sum + q[i].volume;
        pitch_sum = pitch_sum + q[i].pitch;                        // faddp st(2)
    }
    const float vs = st(volume_sum);                               // fstp [esp+0x18]
    const float ps = st(pitch_sum);                                // fstp [esp+0x10]
    if (Ub((const void*)&n) & 0x7fffffffu) {
        self->pos.x = st(D(sx) / n);
        self->pos.y = st(D(sy) / n);
        self->pos.z = st(D(sz) / n);
        const float vol = st(((D(vs) / n) * self->gain) * FB(0x3f000000));   // x 0.5
        const double pitch = D(ps) / n;
        const float pitchf = st(pitch);                            // fcom [s+4]; fstp [esp+0x10]
        Sound3D* s = self->sound;
        if (pitch < s->pitch || pitch > s->pitch) {
            s->pitch_changed = 1;
            cp4(&s->pitch, &pitchf);
        }
        s = self->sound;
        if (D(s->volume) < vol || D(s->volume) > vol) {
            s->volume_changed = 1;
            cp4(&s->volume, &vol);
        }
    } else {
        Sound3D* s = self->sound;
        if (Ub(&s->volume) & 0x7fffffffu) {
            s->volume_changed = 1;
            put4(&s->volume, 0);
        }
    }
    Sound3D* s = self->sound;
    s->off = 0;
    s->keep = 1;
    s->on = 1;
}
static void fp_real_tire_sound_update(Footprint& f, RealTireSound* self, Edx) {
    f.add(self, sizeof(RealTireSound), "tire sound");
    if (Sound3D* s = self->sound) f.add((uint8_t*)s + 4, 0x2a, "tire Sound3D");
}
PORT_FN(0x004733f0, "RealTireSound::Update", RealTireSound_Update, fp_real_tire_sound_update)

// ---- the little virtuals ---------------------------------------------------------------------------------------------
static uint8_t __fastcall TireSound_Ok(TireSound*, Edx) { return 1; }
static void fp_tire_sound_ok(Footprint& f, TireSound*, Edx) { f.pure = true; }
PORT_FN(0x00473670, "TireSound::Ok", TireSound_Ok, fp_tire_sound_ok)

static uint8_t __fastcall DummyTireSound_Ok(DummyTireSound* self, Edx) { return self->ok; }
static void fp_dummy_ok(Footprint& f, DummyTireSound*, Edx) { f.pure = true; }
PORT_FN(0x004736a0, "DummyTireSound::Ok", DummyTireSound_Ok, fp_dummy_ok)

static void __fastcall DummyTireSound_Update(DummyTireSound*, Edx) {}
static void fp_dummy_update(Footprint& f, DummyTireSound*, Edx) { f.pure = true; }
PORT_FN(0x004736b0, "DummyTireSound::Update", DummyTireSound_Update, fp_dummy_update)

// ---- the deleting destructors (vtable slot 0) ---------------------------------------------------------------------------
// TireSound's is unreachable (TireSound is abstract: every tyre sound has a derived vtable when it's deleted); it is
// rewritten with the rest.
static void* __fastcall TireSound_vdtor(TireSound* self, Edx, unsigned flags) {
    *(void** volatile*)&self->vtable = (void**)VT_TireSound;
    if (flags & 1) snd_operator_delete(self);
    return self;
}
static void fp_tire_vdtor(Footprint& f, TireSound*, Edx, unsigned) { f.replay_only = "frees the tyre sound"; }
PORT_FN(0x00473680, "TireSound::`vector deleting destructor'", TireSound_vdtor, fp_tire_vdtor)

static void* __fastcall DummyTireSound_sdtor(DummyTireSound* self, Edx, unsigned flags) {
    *(void** volatile*)&self->vtable = (void**)VT_DummyTireSound;   // ~DummyTireSound, then ~TireSound
    *(void** volatile*)&self->vtable = (void**)VT_TireSound;
    self->ok = 0;
    if (flags & 1) snd_operator_delete(self);
    return self;
}
static void fp_dummy_sdtor(Footprint& f, DummyTireSound*, Edx, unsigned) { f.replay_only = "frees the tyre sound"; }
PORT_FN(0x004736c0, "DummyTireSound::`scalar deleting destructor'", DummyTireSound_sdtor, fp_dummy_sdtor)

static void* __fastcall RealTireSound_vdtor(RealTireSound* self, Edx, unsigned flags) {
    snd_RealTireSound_dtor(self, 0);
    if (flags & 1) snd_operator_delete(self);
    return self;
}
static void fp_real_vdtor(Footprint& f, RealTireSound*, Edx, unsigned) {
    f.replay_only = "deletes its Sound3D, frees the tyre sound";
}
PORT_FN(0x004736f0, "RealTireSound::`vector deleting destructor'", RealTireSound_vdtor, fp_real_vdtor)

// ---- tiresnd.obj's static initialisers -------------------------------------------------------------------------------
static void __cdecl tiresnd_E1_rw() { snd_rcfunc_is_internal(); }
PORT_FN(0x00472f90, "$E1(tiresnd.obj)", tiresnd_E1_rw, fp_snd_static_init)
static void __cdecl tiresnd_E2_rw() { SC_FN(SndVoid_t, 0x00472f90)(); }
PORT_FN(0x00472f80, "$E2(tiresnd.obj)", tiresnd_E2_rw, fp_snd_static_init)
static void __cdecl tiresnd_E5_rw() { SC_G32(0x005788ec) = 0x00000000u; }
PORT_FN(0x00472fb0, "$E5(tiresnd.obj)", tiresnd_E5_rw, fp_snd_static_init)
static void __cdecl tiresnd_E6_rw() { SC_FN(SndVoid_t, 0x00472fb0)(); }
PORT_FN(0x00472fa0, "$E6(tiresnd.obj)", tiresnd_E6_rw, fp_snd_static_init)
static void __cdecl tiresnd_E8_rw() { SC_G32(0x00578908) = 0x001f001fu; }
PORT_FN(0x00472fd0, "$E8(tiresnd.obj)", tiresnd_E8_rw, fp_snd_static_init)
static void __cdecl tiresnd_E9_rw() { SC_FN(SndVoid_t, 0x00472fd0)(); }
PORT_FN(0x00472fc0, "$E9(tiresnd.obj)", tiresnd_E9_rw, fp_snd_static_init)
static void __cdecl tiresnd_E11_rw() { SC_G32(0x005788f8) = 0x03e001e0u; }
PORT_FN(0x00472ff0, "$E11(tiresnd.obj)", tiresnd_E11_rw, fp_snd_static_init)
static void __cdecl tiresnd_E12_rw() { SC_FN(SndVoid_t, 0x00472ff0)(); }
PORT_FN(0x00472fe0, "$E12(tiresnd.obj)", tiresnd_E12_rw, fp_snd_static_init)
static void __cdecl tiresnd_E14_rw() { SC_G32(0x0057890c) = 0x03ef01efu; }
PORT_FN(0x00473010, "$E14(tiresnd.obj)", tiresnd_E14_rw, fp_snd_static_init)
static void __cdecl tiresnd_E15_rw() { SC_FN(SndVoid_t, 0x00473010)(); }
PORT_FN(0x00473000, "$E15(tiresnd.obj)", tiresnd_E15_rw, fp_snd_static_init)
static void __cdecl tiresnd_E17_rw() { SC_G32(0x00578924) = 0x78003c00u; }
PORT_FN(0x00473030, "$E17(tiresnd.obj)", tiresnd_E17_rw, fp_snd_static_init)
static void __cdecl tiresnd_E18_rw() { SC_FN(SndVoid_t, 0x00473030)(); }
PORT_FN(0x00473020, "$E18(tiresnd.obj)", tiresnd_E18_rw, fp_snd_static_init)
static void __cdecl tiresnd_E20_rw() { SC_G32(0x00578904) = 0x780f3c0fu; }
PORT_FN(0x00473050, "$E20(tiresnd.obj)", tiresnd_E20_rw, fp_snd_static_init)
static void __cdecl tiresnd_E21_rw() { SC_FN(SndVoid_t, 0x00473050)(); }
PORT_FN(0x00473040, "$E21(tiresnd.obj)", tiresnd_E21_rw, fp_snd_static_init)
static void __cdecl tiresnd_E23_rw() { SC_G32(0x00578918) = 0x7be03de0u; }
PORT_FN(0x00473070, "$E23(tiresnd.obj)", tiresnd_E23_rw, fp_snd_static_init)
static void __cdecl tiresnd_E24_rw() { SC_FN(SndVoid_t, 0x00473070)(); }
PORT_FN(0x00473060, "$E24(tiresnd.obj)", tiresnd_E24_rw, fp_snd_static_init)
static void __cdecl tiresnd_E26_rw() { SC_G32(0x00578900) = 0x7bef3defu; }
PORT_FN(0x00473090, "$E26(tiresnd.obj)", tiresnd_E26_rw, fp_snd_static_init)
static void __cdecl tiresnd_E27_rw() { SC_FN(SndVoid_t, 0x00473090)(); }
PORT_FN(0x00473080, "$E27(tiresnd.obj)", tiresnd_E27_rw, fp_snd_static_init)
static void __cdecl tiresnd_E29_rw() { SC_G32(0x00578914) = 0x42082108u; }
PORT_FN(0x004730b0, "$E29(tiresnd.obj)", tiresnd_E29_rw, fp_snd_static_init)
static void __cdecl tiresnd_E30_rw() { SC_FN(SndVoid_t, 0x004730b0)(); }
PORT_FN(0x004730a0, "$E30(tiresnd.obj)", tiresnd_E30_rw, fp_snd_static_init)
static void __cdecl tiresnd_E32_rw() { SC_G32(0x005788e8) = 0x001f001fu; }
PORT_FN(0x004730d0, "$E32(tiresnd.obj)", tiresnd_E32_rw, fp_snd_static_init)
static void __cdecl tiresnd_E33_rw() { SC_FN(SndVoid_t, 0x004730d0)(); }
PORT_FN(0x004730c0, "$E33(tiresnd.obj)", tiresnd_E33_rw, fp_snd_static_init)
static void __cdecl tiresnd_E35_rw() { SC_G32(0x00578920) = 0x07e003e0u; }
PORT_FN(0x004730f0, "$E35(tiresnd.obj)", tiresnd_E35_rw, fp_snd_static_init)
static void __cdecl tiresnd_E36_rw() { SC_FN(SndVoid_t, 0x004730f0)(); }
PORT_FN(0x004730e0, "$E36(tiresnd.obj)", tiresnd_E36_rw, fp_snd_static_init)
static void __cdecl tiresnd_E38_rw() { SC_G32(0x005788f4) = 0x07ff03ffu; }
PORT_FN(0x00473110, "$E38(tiresnd.obj)", tiresnd_E38_rw, fp_snd_static_init)
static void __cdecl tiresnd_E39_rw() { SC_FN(SndVoid_t, 0x00473110)(); }
PORT_FN(0x00473100, "$E39(tiresnd.obj)", tiresnd_E39_rw, fp_snd_static_init)
static void __cdecl tiresnd_E41_rw() { SC_G32(0x005788f0) = 0xf8007c00u; }
PORT_FN(0x00473130, "$E41(tiresnd.obj)", tiresnd_E41_rw, fp_snd_static_init)
static void __cdecl tiresnd_E42_rw() { SC_FN(SndVoid_t, 0x00473130)(); }
PORT_FN(0x00473120, "$E42(tiresnd.obj)", tiresnd_E42_rw, fp_snd_static_init)
static void __cdecl tiresnd_E44_rw() { SC_G32(0x00578928) = 0xf81f7c1fu; }
PORT_FN(0x00473150, "$E44(tiresnd.obj)", tiresnd_E44_rw, fp_snd_static_init)
static void __cdecl tiresnd_E45_rw() { SC_FN(SndVoid_t, 0x00473150)(); }
PORT_FN(0x00473140, "$E45(tiresnd.obj)", tiresnd_E45_rw, fp_snd_static_init)
static void __cdecl tiresnd_E47_rw() { SC_G32(0x00578910) = 0xffe07fe0u; }
PORT_FN(0x00473170, "$E47(tiresnd.obj)", tiresnd_E47_rw, fp_snd_static_init)
static void __cdecl tiresnd_E48_rw() { SC_FN(SndVoid_t, 0x00473170)(); }
PORT_FN(0x00473160, "$E48(tiresnd.obj)", tiresnd_E48_rw, fp_snd_static_init)
static void __cdecl tiresnd_E50_rw() { SC_G32(0x005788fc) = 0xffff7fffu; }
PORT_FN(0x00473190, "$E50(tiresnd.obj)", tiresnd_E50_rw, fp_snd_static_init)
static void __cdecl tiresnd_E51_rw() { SC_FN(SndVoid_t, 0x00473190)(); }
PORT_FN(0x00473180, "$E51(tiresnd.obj)", tiresnd_E51_rw, fp_snd_static_init)
