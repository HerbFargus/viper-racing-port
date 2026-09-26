// phys_aidriver.cpp -- M3 3.5 group A3: the AI drivers (library "ai", driver.obj), rewritten.
//
// NOT the physics library's driver.obj (the player's input driver, hook/phys_player.cpp). This one holds the AI
// driver personalities and the tables that hand them out:
//
//   RandomAIDriver   41 static objects (c1_driver.., built by the $E initialisers) that register themselves in
//                    RandomAIDriver::str, one AIDriverTable per AI strength (8). At start-up
//                    (app_begin -> AIDriverBegin -> AIDriverLounge -> RandomAIDriver::Initialize) each one's
//                    init() rolls its skills with random_real / Random: RandomAIDriver, Easy, Med, Hard.
//   AIDriverLounge   the object behind the global `Lounge` (0x4eb8b4; 0x4eb8b0 is the same pointer typed as the
//                    concrete class). It loads aidriver.adr (AIDriverResource2: 8 strengths x 16 drivers),
//                    converting older resource versions, and falls back to the random drivers. Get(strength,
//                    driver) hands a driver out, rotated by `offset`.
//   the driver map   0x51cba0: which driver (index) each car gets; AIGetDriverForCar = map[car] % 7,
//                    AIGetSkill / AIGetDriverNameByCar go through Lounge->Get(AIGetStrength(), that).
//   AIDriverConfig   the development tuning dialog (race() only calls it on the dev path): a UIDialog of 93 items
//                    over a working copy of the drivers (the "tab", 0x50a060) and one "live" driver being edited
//                    (0x51b080..), with callbacks prev/next car/strength/track, copy/paste, delete, undo, save.
//
// Threads (docs/PORTING.md): almost everything here runs on the MAIN thread -- start-up (AIDriverBegin and what
// it calls, the RandomAIDriver inits), menus (the driver map: career_main, do_event, do_race, the network
// proposal) and the config dialog. The getters (AIGetSkill, AIGetDriverForCar, AIGetTrackNumber,
// AIGetDriverNameByCar, Lounge->Get/Count) are also called from the AI cars on the physics thread; they write
// nothing global. So every footprint below lists the statics its function writes, whichever thread runs it.
// Loading / converting / saving resources, allocating and freeing, and the modal dialogs are replay_only.
//
// Skipped: the $E initialisers ($E1..$E192: static-object construction and registration, run by the CRT
// before anything could be hooked), ASSERT_MSG (0x41dc90, a bare `ret`).
//
// Written from the v1.0 disassembly: every call in the original's order, by address (this file's own
// functions too, so the hooked rewrite or the original is what runs), memory moved with integer instructions
// kept as bit copies (rep movsd), register values as double and stored values as float.
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

namespace {

template <typename T> static inline T& at(void* o, uint32_t off) { return *(T*)((uint8_t*)o + off); }
template <typename T> static inline T* P(uint32_t addr) { return (T*)(uintptr_t)addr; }
static inline float as_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
// rep movsd: n dwords, forward, as the original moves them (a bit copy, and the same result if they overlap)
static __forceinline void movsd(void* dst, const void* src, uint32_t n) {
    __asm { mov edi, dst
            mov esi, src
            mov ecx, n
            rep movsd }
}
// the inlined strcpy (repne scasb; rep movsd; rep movsb): strlen + 1 bytes
static __forceinline void str_copy(void* dst, const char* src) { memcpy(dst, src, strlen(src) + 1); }

// ---- layouts (from the constructors and the copies below) -------------------------------------------------------
// TrackSkillInfo1 (20 bytes): per-track tuning. ctor: f0 = fc = f10 = 1, f4 = 144 (0x43100000), f8 = -1.
struct TrackSkillInfo1 { float f0, f4, f8, fc, f10; };
static_assert(sizeof(TrackSkillInfo1) == 0x14, "TrackSkillInfo1");
// TrackSkillInfo2 (24): + one dword, 0 (the config dialog's lap time at +8 is PhysicsTimeString's input)
struct TrackSkillInfo2 : TrackSkillInfo1 { uint32_t _14; };
static_assert(sizeof(TrackSkillInfo2) == 0x18, "TrackSkillInfo2");
// SkillInfo0 (0x6c): the driver's skills (names from out/types.tsv where known). ctor writes all but
// +0x0c, +0x18 and +0x3b. think_ticks is an int (the ctor stores 1, the inits Random(2,4)); +0x0c / +0x0e are
// read as two shorts by the config dialog (copy_to_live), +0x04 as an int.
struct SkillInfo0 {
    float recovery_time;               // +0x00  0 (RandomAIDriver::init: random_real(0, 1))
    int32_t think_ticks;               // +0x04  1
    float brake_release_lag;           // +0x08  0.6 (0x3f19999a)
    uint32_t _0c;                      // +0x0c  (two shorts)
    float bump_tc_off_time;            // +0x10  0
    float aftershock_time;             // +0x14  0
    float _18;                         // +0x18
    float throttle_scale;              // +0x1c  2.4 (0x4019999a)
    float _20;                         // +0x20  1
    float target_speed_scale;          // +0x24  1
    float steer_lock_time;             // +0x28  1
    float _2c;                         // +0x2c  0
    float _30;                         // +0x30  1
    float overspeed_throttle;          // +0x34  1
    uint8_t hot_entry_threshold, cold_apex_threshold, dice3_threshold, _3b;   // +0x38  0xff x3
    float bump_impulse;                // +0x3c  1
    float crash_impulse;               // +0x40  1
    float _44[5];                      // +0x44  1
    float _58;                         // +0x58  100
    float _5c;                         // +0x5c  1/6 (0x3e2aaaab)
    float proxer_decay;                // +0x60  0.999 (0x3f7fbe77)
    float _64;                         // +0x64  150
    float _68;                         // +0x68  10
};
static_assert(sizeof(SkillInfo0) == 0x6c && offsetof(SkillInfo0, hot_entry_threshold) == 0x38 &&
              offsetof(SkillInfo0, proxer_decay) == 0x60, "SkillInfo0");
struct SkillInfo1 : SkillInfo0 { float _6c; TrackSkillInfo1 tracks[16]; };                    // 0x1b0
struct SkillInfo2 : SkillInfo0 { float _6c; char car_name[32]; TrackSkillInfo2 tracks[16]; };  // 0x210, "viper"
static_assert(sizeof(SkillInfo1) == 0x1b0 && sizeof(SkillInfo2) == 0x210 && offsetof(SkillInfo2, tracks) == 0x90, "SkillInfo");
// the drivers: a skill set, the driver's name (fixup_res points it at "E-1".. / a RandomAIDriver's name), and
// padding to the resource's stride. AIDriver0 is only ever read (its first 0x6c bytes).
struct AIDriver0 { SkillInfo0 skill; uint8_t _6c[0x10]; };                                    // 0x7c
struct AIDriver1 { SkillInfo1 skill; const char* name; uint8_t _1b4[12]; };                   // 0x1c0
struct AIDriver2 { SkillInfo2 skill; const char* name; uint8_t _214[12]; };                   // 0x220
static_assert(sizeof(AIDriver0) == 0x7c && sizeof(AIDriver1) == 0x1c0 && sizeof(AIDriver2) == 0x220, "AIDriver");
// a strength's drivers in a resource: 16, then the count
template <typename T> struct Bucket { T d[16]; int32_t count; };
static_assert(sizeof(Bucket<AIDriver0>) == 0x7c4 && sizeof(Bucket<AIDriver1>) == 0x1c04 && sizeof(Bucket<AIDriver2>) == 0x2204, "Bucket");
// AIDriverResource0/1/2 = 8 buckets (0x3e20 / 0xe020 / 0x11020 bytes); AIDriverResource2_OLD = 3 (0x660c)
// AIDriverTable (0x44): 16 AIDriver2 pointers and a count (ctor: count = 0)
struct AIDriverTable { AIDriver2* d[16]; int32_t count; };
static_assert(sizeof(AIDriverTable) == 0x44, "AIDriverTable");
// AIDriverLounge (0x22c, vtable 0x4db648: +0 Count, +4 Get, +8 Reload; IDriverLounge 0x4db658 is pure)
struct AIDriverLounge { void** vtable; uint8_t* res; AIDriverTable tables[8]; int32_t offset; };
static_assert(offsetof(AIDriverLounge, offset) == 0x228 && sizeof(AIDriverLounge) == 0x22c, "AIDriverLounge");
// RandomAIDriver (0x218, vtable 0x4db664 / Easy 0x4db668 / Med 0x4db66c / Hard 0x4db670: +0 init). str[]
// holds &skill, so the table's AIDriver2's name (+0x210) is this object's name at +0x214 -- and a copy of the
// whole AIDriver2 (0x220 bytes, convert_resource_2OLD) reads 12 bytes past the object.
struct RandomAIDriver { void** vtable; SkillInfo2 skill; const char* name; };
static_assert(sizeof(RandomAIDriver) == 0x218, "RandomAIDriver");
// the config dialog's items: 14 dwords, as UIDialogItem::UIDialogItem (0x4091c0) stores its 14 arguments
struct UIDialogItem { uint32_t a[14]; };
struct UIDialog { uint32_t title, subtitle; int32_t _08; UIDialogItem* items; uint32_t idle; };

// ---- the statics ----------------------------------------------------------------------------------------------
enum : uint32_t {
    S_LOUNGE_IMPL = 0x004eb8b0,      // AIDriverLounge* (AIDriverBegin; the config dialog's calls use it)
    S_LOUNGE = 0x004eb8b4,           // IDriverLounge* Lounge (the same object)
    S_STR = 0x0051d8d8,              // AIDriverTable RandomAIDriver::str[8] (to 0x51daf8)
    S_STR_COUNT0 = 0x0051d918,       // str[0].count
    S_MAP = 0x0051cba0,              // int driver_map[16]
    S_MAP_N = 0x0051cbe0,            // int driver_map_count
    S_AI_STRENGTH = 0x00509990,      // AIGetStrength's
    // AIDriverConfig's
    S_TAB = 0x0050a060, TAB_SIZE = 0x11020,   // AIDriverResource2: the drivers being edited
    S_TAB_COUNT0 = 0x0050c260,       // tab bucket[0].count
    S_LIVE_BASE = 0x0051b080, LIVE_SIZE = 0x35c,
    S_LIVE_F0C = 0x0051b080,         // float  live driver's short at +0x0c
    S_LIVE_F0E = 0x0051b084,         // float  short at +0x0e
    S_LIVE_F04 = 0x0051b088,         // float  int at +0x04 (think_ticks)
    S_LIVE_B38 = 0x0051b08c,         // float  1 - byte at +0x38 / 255 (and +0x39, +0x3a at 0x51b090, 0x51b094)
    S_LIVE_TRACK = 0x0051b098,       // TrackSkillInfo2: the current track's, being edited
    S_LIVE_LAP = 0x0051b0a0,         // its +8: the lap time PhysicsTimeString shows
    S_LIVE = 0x0051b0b0,             // AIDriver2: the driver being edited
    S_LIVE_TRACKS = 0x0051b140,      // its tracks[16]
    S_CUR_STR = 0x0051b2d0, S_CUR_CAR = 0x0051b2d4, S_CUR_TRK = 0x0051b2d8,
    S_STRENGTH_TEXT = 0x0051b2dc,    // char[64]  GetAIStrengthString
    S_CAR_TEXT = 0x0051b31c,         // char[64]  "%2.2d of %2.2d"
    S_TRACK_TEXT = 0x0051b35c,       // char[64]  "%s%s" name, " reversed"
    S_LAP_TEXT = 0x0051b39c,         // char[64]  PhysicsTimeString
    S_CLIP = 0x0051fb58,             // AIDriver2 copypaste_cb's clipboard (a function-static)
    S_CLIP_TRACKS = 0x0051fbe8,
    S_CLIP_GUARD = 0x00520848,       // its initialisation guard, bit 0
    S_HAVE_CLIP = 0x0051c748,        // u8
    S_ITEM_END = 0x00578d08,         // the UIDialogItem that ends a dialog (14 dwords)
    S_OPTIONS_SECTION = 0x004db580,  // const char* "AI"
    C_4000 = 0x004db588, C_0 = 0x004db58c, C_0B = 0x004db590, C_2 = 0x004db594,   // .rdata float constants
    VT_LOUNGE = 0x004db648, VT_ILOUNGE = 0x004db658, VT_RANDOM = 0x004db664,
    TYPE_AIDR = 0x41494472,          // 'AIDr'
};
static inline int32_t& tab_count(int32_t str) { return *(int32_t*)(S_TAB_COUNT0 + (uint32_t)str * 0x2204); }
static inline uint8_t* tab_driver(int32_t str, int32_t car) { return P<uint8_t>(S_TAB + (uint32_t)str * 0x2204 + (uint32_t)car * 0x220); }
static inline int32_t& cur_str() { return *P<int32_t>(S_CUR_STR); }
static inline int32_t& cur_car() { return *P<int32_t>(S_CUR_CAR); }
static inline int32_t& cur_trk() { return *P<int32_t>(S_CUR_TRK); }

}  // namespace

// ---- the game's functions these call (by v1.0 address) -----------------------------------------------------------
typedef void*(__fastcall* Ctor_t)(void*, Edx);
typedef void(__fastcall* Method_t)(void*, Edx);
typedef uint8_t(__fastcall* MethodB_t)(void*, Edx);
typedef uint8_t(__fastcall* LoadResName_t)(void*, Edx, const char*);
typedef const char*(__cdecl* IntToStr_t)(int);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
typedef uint8_t*(__cdecl* RandomGet_t)(int, int);
typedef uint8_t(__cdecl* ResourceForget_t)(void*);
typedef void*(__cdecl* ResourceTry_t)(const char*, uint32_t, uint32_t*, int32_t*, uint8_t*, uint8_t*);
typedef uint8_t(__cdecl* ConvertObsolete_t)(void*, uint32_t);
typedef uint8_t(__cdecl* Convert2Old_t)(void*);
typedef uint8_t(__cdecl* CopyDriver_t)(void*, const void*);
typedef uint8_t(__cdecl* SaveResource_t)(const char*, const void*);
typedef int(__cdecl* FileCreate_t)(const char*);
typedef void(__cdecl* ResourceWrite_t)(int, uint32_t, uint32_t);
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
typedef void(__cdecl* FileClose_t)(int*);
typedef int(__cdecl* sprintf_t)(char*, const char*, ...);
typedef void(__cdecl* UIDoDratBox_t)(const char*, const char*, void*);
typedef void(__cdecl* Void_t)(void);
typedef int(__cdecl* Random_t)(int);
typedef int(__cdecl* Random2_t)(int, int);
typedef int(__cdecl* StrToInt_t)(const char*);
typedef int(__cdecl* TrackNumber_t)(int, uint32_t);
typedef const uint8_t*(__cdecl* WorldGameOptions_t)(void);
typedef int(__cdecl* IntGet_t)(void);
typedef int(__cdecl* IntInt_t)(int);
typedef void(__cdecl* VerifyMap_t)(int32_t*, int);
typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* Free_t)(void*);
typedef const char*(__cdecl* PhysicsTimeString_t)(uint32_t, uint32_t);        // (float bits, flag)
typedef void(__cdecl* CopyLive_t)(void*);
typedef void*(__cdecl* memmove_t)(void*, const void*, size_t);
typedef int(__cdecl* atexit_t)(void*);
typedef void*(__cdecl* CreateViewer_t)(int32_t*, int32_t*, int32_t*);
typedef void*(__fastcall* ItemCtor_t)(void*, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                      uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef int(__cdecl* UIDoDialog_t)(const UIDialog*, int, int, int, int, int);
typedef void(__cdecl* OptionsGetI_t)(const char*, const char*, int32_t*);
typedef void(__cdecl* OptionsSetI_t)(const char*, const char*, int);
typedef uint8_t(__cdecl* Callback_t)(int);

static const IntToStr_t GetAIStrengthString = (IntToStr_t)0x004061a0;
static const IntToStr_t GetTrackName = (IntToStr_t)0x00406810;
static const StrToInt_t GetTrackNumber = (StrToInt_t)0x004068b0;
static const AssertMsg_t ASSERT_MSG = (AssertMsg_t)0x0041dc90;          // a bare ret: kept, as the original calls it
static const Log_t LogReport = (Log_t)0x00411150;
static const Log_t LogPanic = (Log_t)0x004112b0;
static const ResourceTry_t ResourceTry = (ResourceTry_t)0x00419ce0;
static const ResourceForget_t ResourceForget = (ResourceForget_t)0x0041a450;
static const ResourceWrite_t ResourceWrite = (ResourceWrite_t)0x0041a5c0;
static const FileCreate_t FileCreate = (FileCreate_t)0x004115f0;
static const FileWrite_t FileWrite = (FileWrite_t)0x00411a30;
static const FileClose_t FileClose = (FileClose_t)0x00411850;
static const sprintf_t g_sprintf = (sprintf_t)0x004cf0a0;                // the game's CRT
static const memmove_t g_memmove = (memmove_t)0x004cf400;
static const atexit_t g_atexit = (atexit_t)0x004ceff0;
static const UIDoDratBox_t UIDoDratBox = (UIDoDratBox_t)0x00479280;
static const Random_t Random = (Random_t)0x0041b6e0;                     // the recorder's hook: a shadow input
static const Random2_t Random2 = (Random2_t)0x0041b700;
static const WorldGameOptions_t WorldGameOptions = (WorldGameOptions_t)0x004627a0;
static const IntGet_t WorldGetTrackNumber = (IntGet_t)0x00462760;
static const IntGet_t AIGetStrength = (IntGet_t)0x0041d310;
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
static const Free_t op_delete = (Free_t)0x00414390;
static const PhysicsTimeString_t PhysicsTimeString = (PhysicsTimeString_t)0x0042bf30;
static const CreateViewer_t CreateIGhostCarViewer = (CreateViewer_t)0x00426050;
static const ItemCtor_t UIDialogItem_ctor = (ItemCtor_t)0x004091c0;
static const UIDoDialog_t UIDoDialog = (UIDoDialog_t)0x00478ff0;
static const OptionsGetI_t OptionsGetI = (OptionsGetI_t)0x004713e0;
static const OptionsSetI_t OptionsSetI = (OptionsSetI_t)0x00471430;
static const Void_t learn_dialog = (Void_t)0x004238a0;
static int* const gxScreenWid = (int*)0x005228f4;
static int* const gxScreenHit = (int*)0x005228d4;
// this file's own functions, called by address so the hooked rewrite (or the original) is what runs
static const Method_t Lounge_copy_resA = (Method_t)0x0041d600;
static const Method_t Lounge_fixup_resA = (Method_t)0x0041d660;
static const Method_t Lounge_copy_random_driversA = (Method_t)0x0041dc00;
static const MethodB_t Lounge_load_resA = (MethodB_t)0x0041dcd0;
static const LoadResName_t Lounge_load_res_nameA = (LoadResName_t)0x0041dd30;
static const ConvertObsolete_t convert_obsolete_resourceA = (ConvertObsolete_t)0x0041de10;
static const CopyDriver_t copy_driver_01A = (CopyDriver_t)0x0041dfe0;
static const CopyDriver_t copy_driver_12A = (CopyDriver_t)0x0041e000;
static const SaveResource_t save_resource1A = (SaveResource_t)0x0041e050;
static const SaveResource_t save_resource2A = (SaveResource_t)0x0041e0f0;
static const Convert2Old_t convert_resource_2OLDA = (Convert2Old_t)0x0041e190;
static const Ctor_t Lounge_ctorA = (Ctor_t)0x0041e250;
static const Method_t Lounge_dtorA = (Method_t)0x0041e2b0;
static const RandomGet_t RandomAIDriver_GetA = (RandomGet_t)0x0041e310;
static const Method_t RandomAIDriver_initA = (Method_t)0x0041e3c0;
static const Void_t RandomAIDriver_InitializeA = (Void_t)0x0041e410;
static const Ctor_t TrackSkillInfo2_ctorA = (Ctor_t)0x0041ec10;
static const Ctor_t TrackSkillInfo1_ctorA = (Ctor_t)0x0041ec30;
static const Ctor_t SkillInfo1_ctorA = (Ctor_t)0x0041ec50;
static const Ctor_t SkillInfo2_ctorA = (Ctor_t)0x0041ec80;
static const Ctor_t SkillInfo0_ctorA = (Ctor_t)0x0041ece0;
static const Void_t update_lap_stringA = (Void_t)0x004202d0;
static const Void_t copy_lounge_to_tabA = (Void_t)0x00420310;
static const Void_t cfg_update_stringsA = (Void_t)0x004203d0;
static const Void_t copy_tab_to_liveA = (Void_t)0x00420490;
static const CopyLive_t copy_to_liveA = (CopyLive_t)0x004204d0;
static const CopyLive_t copy_from_liveA = (CopyLive_t)0x004207d0;
static const Callback_t save_cbA = (Callback_t)0x004208b0;
static const Void_t copy_live_to_tabA = (Void_t)0x00420930;
static const Void_t AIResetDriverMapA = (Void_t)0x00420c30;
static const VerifyMap_t verify_driver_mapA = (VerifyMap_t)0x00420c80;
static const IntInt_t AIGetDriverForCarA = (IntInt_t)0x00420ce0;
static const TrackNumber_t AIGetTrackNumber_intA = (TrackNumber_t)0x00420de0;
static const Ctor_t AIDriverTable_ctorA = (Ctor_t)0x00420e20;
static const Ctor_t AIDriver1_ctorA = (Ctor_t)0x00420e60;
static const Ctor_t AIDriver2_ctorA = (Ctor_t)0x00420eb0;

// random_real(lo, hi) with its result stored straight from ST0 to a float (the callers' fstp dword): no C
// variable in between, so it's exact whatever the precision control
static __declspec(noinline) void random_real_store(float* dst, uint32_t lo, uint32_t hi) {
    __asm { push hi
            push lo
            mov eax, 0x0041e3e0
            call eax
            add esp, 8
            mov ecx, dst
            fstp dword ptr [ecx] }
}

// ---- footprint helpers ----------------------------------------------------------------------------------------------
static void fp_cfg(Footprint& f) {                                     // the config dialog's working state
    f.add(P<void>(S_TAB), TAB_SIZE, "config tab (AIDriverResource2)");
    f.add(P<void>(S_LIVE_BASE), LIVE_SIZE, "config live driver, indices, strings");
}
static void fp_none(Footprint&) {}
static bool counts_ok(const int32_t* first, uint32_t stride, int n) {  // every count within 0..16
    for (int i = 0; i < n; i++) {
        int32_t c = *(const int32_t*)((const uint8_t*)first + i * stride);
        if (c < 0 || c > 16) return false;
    }
    return true;
}

// =================================================================================================================
// AIDriverLounge
// =================================================================================================================

// copy_res: point each strength's table at the loaded resource's drivers, and copy the counts. No bounds check:
// a count past 16 runs on into the next table (and past the last one, into `offset` and beyond).
static void __fastcall Lounge_copy_res(AIDriverLounge* self, Edx) {
    for (uint32_t off = 0, t = 8; off < 0x11020; off += 0x2204, t += 0x44) {
        for (int32_t j = 0; at<int32_t>(self->res, off + 0x2200) > j; j++)     // the resource pointer re-read each time
            at<uint32_t>(self, t + 4 * j) = (uint32_t)(uintptr_t)(self->res + off + 0x220 * j);
        at<int32_t>(self, t + 0x40) = at<int32_t>(self->res, off + 0x2200);
    }
}
static void fp_lounge_tables(Footprint& f, AIDriverLounge* self, Edx) {
    if (!self->res || !counts_ok((const int32_t*)(self->res + 0x2200), 0x2204, 8)) {
        f.replay_only = "a resource count outside 0..16 writes past the lounge's tables";
        return;
    }
    f.add(&self->tables, sizeof self->tables, "lounge tables");
}
PORT_FN(0x0041d600, "AIDriverLounge::copy_res", Lounge_copy_res, fp_lounge_tables)

// fixup_res: a "development" resource's drivers get their names from a table of 128 strings (8 strengths x 16:
// "E-1".. at 0x4eba44..0x4ebdd0), which the original builds on its stack. A count past 16 takes the next
// strength's names -- and past the last one (index 128 and up) the original reads its own stack (saved
// registers, the return address): not reproduced (0 here).
static const uint32_t k_driver_names[128] = {
    0x4eba44, 0x4eba48, 0x4eba4c, 0x4eba50, 0x4eba54, 0x4eba58, 0x4eba5c, 0x4eba60,
    0x4eba64, 0x4eba68, 0x4eba70, 0x4eba78, 0x4eba80, 0x4eba88, 0x4eba90, 0x4eba98,
    0x4ebaa0, 0x4ebaa4, 0x4ebaa8, 0x4ebaac, 0x4ebab0, 0x4ebab4, 0x4ebab8, 0x4ebabc,
    0x4ebac0, 0x4ebac4, 0x4ebacc, 0x4ebad4, 0x4ebadc, 0x4ebae4, 0x4ebaec, 0x4ebaf4,
    0x4ebafc, 0x4ebb00, 0x4ebb04, 0x4ebb08, 0x4ebb0c, 0x4ebb10, 0x4ebb14, 0x4ebb18,
    0x4ebb1c, 0x4ebb20, 0x4ebb28, 0x4ebb30, 0x4ebb38, 0x4ebb40, 0x4ebb48, 0x4ebb50,
    0x4ebb58, 0x4ebb60, 0x4ebb68, 0x4ebb70, 0x4ebb78, 0x4ebb80, 0x4ebb88, 0x4ebb90,
    0x4ebb98, 0x4ebba0, 0x4ebba8, 0x4ebbb0, 0x4ebbb8, 0x4ebbc0, 0x4ebbc8, 0x4ebbd0,
    0x4ebbd8, 0x4ebbe0, 0x4ebbe8, 0x4ebbf0, 0x4ebbf8, 0x4ebc00, 0x4ebc08, 0x4ebc10,
    0x4ebc18, 0x4ebc20, 0x4ebc28, 0x4ebc30, 0x4ebc38, 0x4ebc40, 0x4ebc48, 0x4ebc50,
    0x4ebc58, 0x4ebc60, 0x4ebc68, 0x4ebc70, 0x4ebc78, 0x4ebc80, 0x4ebc88, 0x4ebc90,
    0x4ebc98, 0x4ebca0, 0x4ebca8, 0x4ebcb0, 0x4ebcb8, 0x4ebcc0, 0x4ebcc8, 0x4ebcd0,
    0x4ebcd8, 0x4ebce0, 0x4ebce8, 0x4ebcf0, 0x4ebcf8, 0x4ebd00, 0x4ebd08, 0x4ebd10,
    0x4ebd18, 0x4ebd20, 0x4ebd28, 0x4ebd30, 0x4ebd38, 0x4ebd40, 0x4ebd48, 0x4ebd50,
    0x4ebd58, 0x4ebd60, 0x4ebd68, 0x4ebd70, 0x4ebd78, 0x4ebd80, 0x4ebd88, 0x4ebd90,
    0x4ebd98, 0x4ebda0, 0x4ebda8, 0x4ebdb0, 0x4ebdb8, 0x4ebdc0, 0x4ebdc8, 0x4ebdd0,
};
static void __fastcall Lounge_fixup_res(AIDriverLounge* self, Edx) {
    for (uint32_t off = 0, k = 0; off < 0x11020; off += 0x2204, k += 16)
        for (int32_t j = 0; at<int32_t>(self->res, off + 0x2200) > j; j++) {
            uint32_t idx = k + (uint32_t)j;
            at<uint32_t>(self->res, off + 0x220 * j + 0x210) = idx < 128 ? k_driver_names[idx] : 0;
        }
}
static void fp_lounge_fixup_res(Footprint& f, AIDriverLounge* self, Edx) {
    if (!self->res || !counts_ok((const int32_t*)(self->res + 0x2200), 0x2204, 8)) {
        f.replay_only = "a resource count outside 0..16 writes past its bucket";
        return;
    }
    f.add(self->res, 0x11020, "the lounge's resource");
}
PORT_FN(0x0041d660, "AIDriverLounge::fixup_res", Lounge_fixup_res, fp_lounge_fixup_res)

// copy_random_drivers: no resource -- each strength gets the random drivers (RandomAIDriver::str)
static void __fastcall Lounge_copy_random_drivers(AIDriverLounge* self, Edx) {
    for (int32_t i = 0; i < 8; i++) {
        int32_t n = *P<int32_t>(S_STR_COUNT0 + 0x44 * i);
        const char* s = GetAIStrengthString(i);
        ASSERT_MSG(n, (const char*)0x004ebdd8, s);                       // "ai:driver:  No %s drivers defined."
        for (int32_t j = 0; n > j; j++) {
            uint8_t* d = RandomAIDriver_GetA(i, j);
            at<uint32_t>(self, 8 + 4 * (17 * i + j)) = (uint32_t)(uintptr_t)d;
        }
        at<int32_t>(self, 8 + 0x44 * i + 0x40) = n;
    }
}
static void fp_lounge_copy_random_drivers(Footprint& f, AIDriverLounge* self, Edx) {
    if (!counts_ok(P<int32_t>(S_STR_COUNT0), 0x44, 8)) { f.replay_only = "a random-driver count outside 0..16"; return; }
    f.add(&self->tables, sizeof self->tables, "lounge tables");
}
PORT_FN(0x0041dc00, "AIDriverLounge::copy_random_drivers", Lounge_copy_random_drivers, fp_lounge_copy_random_drivers)

// Reload (vtable +8): forget the resource and load it again
static uint8_t __fastcall Lounge_Reload(AIDriverLounge* self, Edx) {
    if (self->res) {
        ResourceForget(self->res);
        self->res = 0;
    }
    return Lounge_load_resA(self, 0);
}
static void fp_lounge_reload(Footprint& f, AIDriverLounge*, Edx) { f.replay_only = "frees and loads a resource"; }
PORT_FN(0x0041dca0, "AIDriverLounge::Reload", Lounge_Reload, fp_lounge_reload)

// load_res(): "*aidriver.adr" (the development drivers: logged), then "aidriver.adr"
static uint8_t __fastcall Lounge_load_res(AIDriverLounge* self, Edx) {
    const char* names[2] = {(const char*)0x004ebdfc, (const char*)0x004ebe0c};
    for (int i = 0; i < 2; i++)
        if (Lounge_load_res_nameA(self, 0, names[i])) {
            if (i == 0) LogReport((const char*)0x004ebe1c);                // warning: using "development" aidrivers
            return 1;
        }
    return 0;
}
static void fp_lounge_load_res(Footprint& f, AIDriverLounge*, Edx) { f.replay_only = "loads a resource"; }
PORT_FN(0x0041dcd0, "AIDriverLounge::load_res()", Lounge_load_res, fp_lounge_load_res)

// load_res(name): ResourceTry('AIDr'); version 2 of the right size is used as it is, version 2 of the old size
// (3 strengths) and versions 0 and 1 are converted -- which saves conv*.adr and always reports failure. On
// success: fix up the names if ResourceTry says so (its last output), and point the tables at the drivers.
static uint8_t __fastcall Lounge_load_res_name(AIDriverLounge* self, Edx, const char* name) {
    uint32_t version = 0;
    int32_t size = 0;                                  // (the original's is uninitialised; ResourceTry fills it)
    uint8_t fixup = 0;                                 // (likewise)
    void* r = ResourceTry(name, TYPE_AIDR, &version, &size, 0, &fixup);
    self->res = (uint8_t*)r;
    if (!r) return 0;
    uint8_t ok = 1;
    bool obsolete = version != 2;
    if (obsolete) ok = convert_obsolete_resourceA(r, version);
    if (!obsolete && size != 0x11020) {
        if (size == 0x660c) ok = convert_resource_2OLDA(self->res);
        else {
            LogReport((const char*)0x004ebe44, size, 0x11020);            // AIDriverResource size mismatch: %d != %d
            ok = 0;
        }
    }
    if (ok) {
        if (fixup) Lounge_fixup_resA(self, 0);
        Lounge_copy_resA(self, 0);
        return 1;
    }
    ResourceForget(self->res);
    self->res = 0;
    return 0;
}
static void fp_lounge_load_res_name(Footprint& f, AIDriverLounge*, Edx, const char*) { f.replay_only = "loads a resource"; }
PORT_FN(0x0041dd30, "AIDriverLounge::load_res(name)", Lounge_load_res_name, fp_lounge_load_res_name)

// convert_obsolete_resource: version 0 -> AIDriverResource1 (conv01.adr), version 1 -> AIDriverResource2
// (conv12.adr), copying bucket by bucket (the copy's result stops it, but the copies never fail). Returns 0
// whatever happens: the caller never uses a converted resource.
static uint8_t __cdecl convert_obsolete_resource(void* res, uint32_t version) {
    static_assert(sizeof(Bucket<AIDriver2>) * 8 == 0x11020, "tmp");
    uint8_t result;
    alignas(4) uint8_t tmp[0x11020];
    if (version == 0) {
        result = 0;
        for (uint32_t k = 0; k < 8; k++) ((Ctor_t)0x00420e30)(tmp + 0x1c04 * k, 0);   // AIDriverBucket1
        uint8_t failed = 0;
        uint8_t* src = (uint8_t*)res + 0x7c0;          // bucket 0's count
        for (uint8_t* dst = tmp; dst < tmp + 0xe020;) {
            uint8_t* d = dst;
            uint8_t* s = src - 0x7c0;
            for (int32_t j = 0; *(int32_t*)src > j; j++, d += 0x1c0, s += 0x7c)
                failed = copy_driver_01A(d, s) < 1;
            *(int32_t*)(dst + 0x1c00) = *(int32_t*)src;
            dst += 0x1c04;
            src += 0x7c4;
            if (failed) break;
        }
        if (!failed && save_resource1A((const char*)0x004ebe70, tmp))
            LogReport((const char*)0x004ebe7c, (const char*)0x004ebe70);   // Conversion saved to conv01.adr
    } else if (version == 1) {
        result = 0;
        for (uint32_t k = 0; k < 8; k++) ((Ctor_t)0x00420e80)(tmp + 0x2204 * k, 0);   // AIDriverBucket2
        uint8_t failed = 0;
        uint8_t* src = (uint8_t*)res + 0x1c00;
        for (uint8_t* dst = tmp; dst < tmp + 0x11020;) {
            uint8_t* d = dst;
            uint8_t* s = src - 0x1c00;
            for (int32_t j = 0; *(int32_t*)src > j; j++, d += 0x220, s += 0x1c0)
                failed = copy_driver_12A(d, s) < 1;
            *(int32_t*)(dst + 0x2200) = *(int32_t*)src;
            dst += 0x2204;
            src += 0x1c04;
            if (failed) break;
        }
        if (!failed && save_resource2A((const char*)0x004ebe94, tmp))
            LogReport((const char*)0x004ebea0, (const char*)0x004ebe94);   // Conversion saved to conv12.adr
    } else {
        LogReport((const char*)0x004ebeb8, version);                       // Can't convert ... version %d
        result = 0;
    }
    return result;
}
static void fp_convert_obsolete_resource(Footprint& f, void*, uint32_t) { f.replay_only = "writes a converted resource file"; }
PORT_FN(0x0041de10, "convert_obsolete_resource", convert_obsolete_resource, fp_convert_obsolete_resource)

// copy_driver_01: the skills only (0x6c bytes); the rest of the AIDriver1 stays as constructed
static uint8_t __cdecl copy_driver_01(AIDriver1* dst, const AIDriver0* src) {
    movsd(dst, src, 0x1b);
    return 1;
}
static void fp_copy_driver_01(Footprint& f, AIDriver1* dst, const AIDriver0*) { f.add(dst, 0x6c, "AIDriver1"); f.pure = true; }
PORT_FN(0x0041dfe0, "copy_driver_01", copy_driver_01, fp_copy_driver_01)

// copy_driver_12: the skills, +0x6c, and each track's 20 bytes into the 24-byte TrackSkillInfo2 (car_name, the
// tracks' +0x14 and the name stay as constructed)
static uint8_t __cdecl copy_driver_12(AIDriver2* dst, const AIDriver1* src) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    movsd(d, s, 0x1b);
    at<uint32_t>(d, 0x6c) = *(const uint32_t*)(s + 0x6c);
    for (int k = 0; k < 16; k++) movsd(d + 0x90 + 0x18 * k, s + 0x70 + 0x14 * k, 5);
    return 1;
}
static void fp_copy_driver_12(Footprint& f, AIDriver2* dst, const AIDriver1*) { f.add(dst, 0x210, "AIDriver2"); f.pure = true; }
PORT_FN(0x0041e000, "copy_driver_12", copy_driver_12, fp_copy_driver_12)

// save_resource: FileCreate, the 'AIDr' header (version 1 / 2), the body; or a "Shucks!" box
static uint8_t __cdecl save_resource1(const char* name, const void* res) {
    uint8_t ok = 1;
    int fd = FileCreate(name);
    if (fd) {
        ResourceWrite(fd, TYPE_AIDR, 1);
        FileWrite(fd, res, 0xe020);
        FileClose(&fd);
        return ok;
    }
    char buf[0x80];
    g_sprintf(buf, (const char*)0x004ebee4, name);                         // Can't create %s
    UIDoDratBox((const char*)0x004ebef4, buf, 0);                          // Shucks!
    return 0;
}
static void fp_save_resource(Footprint& f, const char*, const void*) { f.replay_only = "writes a file (or opens a message box)"; }
PORT_FN(0x0041e050, "save_resource(AIDriverResource1)", save_resource1, fp_save_resource)

static uint8_t __cdecl save_resource2(const char* name, const void* res) {
    uint8_t ok = 1;
    int fd = FileCreate(name);
    if (fd) {
        ResourceWrite(fd, TYPE_AIDR, 2);
        FileWrite(fd, res, 0x11020);
        FileClose(&fd);
        return ok;
    }
    char buf[0x80];
    g_sprintf(buf, (const char*)0x004ebefc, name);
    UIDoDratBox((const char*)0x004ebf0c, buf, 0);
    return 0;
}
PORT_FN(0x0041e0f0, "save_resource(AIDriverResource2)", save_resource2, fp_save_resource)

// convert_resource_2OLD: a 3-strength resource: its 3 buckets, then strengths 3..7 get one random driver each
// (a 0x220-byte copy of RandomAIDriver::Get(b, 0), which reads 12 bytes past that RandomAIDriver); saved as
// conv2old.adr. Returns 0.
static uint8_t __cdecl convert_resource_2OLD(void* old) {
    alignas(4) uint8_t tmp[0x11020];
    for (uint32_t k = 0; k < 8; k++) ((Ctor_t)0x00420e80)(tmp + 0x2204 * k, 0);       // AIDriverBucket2
    for (uint32_t k = 0; k < 3; k++) movsd(tmp + 0x2204 * k, (uint8_t*)old + 0x2204 * k, 0x881);
    for (int32_t b = 3; b < 8; b++) {
        *(int32_t*)(tmp + 0x2204 * b + 0x2200) = 1;
        movsd(tmp + 0x2204 * b, RandomAIDriver_GetA(b, 0), 0x88);
    }
    LogReport((const char*)0x004ebf14);                                    // converted 2old to 2
    save_resource2A((const char*)0x004ebf28, tmp);                         // conv2old.adr
    return 0;
}
static void fp_convert_resource_2old(Footprint& f, void*) { f.replay_only = "writes a converted resource file"; }
PORT_FN(0x0041e190, "convert_resource_2OLD", convert_resource_2OLD, fp_convert_resource_2old)

// the constructor: 8 empty tables, roll the random drivers, load the resource (else use the random drivers)
static AIDriverLounge* __fastcall Lounge_ctor(AIDriverLounge* self, Edx) {
    self->vtable = P<void*>(VT_ILOUNGE);
    self->res = 0;
    for (int k = 0; k < 8; k++) AIDriverTable_ctorA(&self->tables[k], 0);
    self->vtable = P<void*>(VT_LOUNGE);
    RandomAIDriver_InitializeA();
    if (!Lounge_load_resA(self, 0)) Lounge_copy_random_driversA(self, 0);
    self->offset = 0;
    return self;
}
static void fp_lounge_ctor(Footprint& f, AIDriverLounge* self, Edx) {
    f.add(self, sizeof *self, "AIDriverLounge");
    f.replay_only = "loads a resource; rolls every random driver";
}
PORT_FN(0x0041e250, "AIDriverLounge::AIDriverLounge", Lounge_ctor, fp_lounge_ctor)

static void __fastcall Lounge_dtor(AIDriverLounge* self, Edx) {
    self->vtable = P<void*>(VT_LOUNGE);
    if (self->res) {
        ResourceForget(self->res);
        self->res = 0;
    }
}
static void fp_lounge_dtor(Footprint& f, AIDriverLounge* self, Edx) {
    f.add(self, 8, "AIDriverLounge vtable, res");
    f.replay_only = "frees the resource";
}
PORT_FN(0x0041e2b0, "AIDriverLounge::~AIDriverLounge", Lounge_dtor, fp_lounge_dtor)

// Get (vtable +4): strength i's driver (offset + j) mod its count (a count of 0 divides by zero, as the original)
static AIDriver2* __fastcall Lounge_Get(AIDriverLounge* self, Edx, int32_t i, int32_t j) {
    int32_t k = (int32_t)((uint32_t)self->offset + (uint32_t)j);
    int32_t n = at<int32_t>(self, 0x48 + 0x44 * (uint32_t)i);
    return at<AIDriver2*>(self, 8 + 4 * (17 * (uint32_t)i + (uint32_t)(k % n)));
}
static void fp_lounge_get(Footprint&, AIDriverLounge*, Edx, int32_t, int32_t) {}
PORT_FN(0x0041e2e0, "AIDriverLounge::Get", Lounge_Get, fp_lounge_get)

// Count (vtable +0)
static int32_t __fastcall Lounge_Count(AIDriverLounge* self, Edx, int32_t i) { return at<int32_t>(self, 0x48 + 0x44 * (uint32_t)i); }
static void fp_lounge_count(Footprint&, AIDriverLounge*, Edx, int32_t) {}
PORT_FN(0x00420ed0, "AIDriverLounge::Count", Lounge_Count, fp_lounge_count)

// =================================================================================================================
// RandomAIDriver
// =================================================================================================================

static uint8_t* __cdecl RandomAIDriver_Get(int32_t i, int32_t j) { return *P<uint8_t*>(S_STR + 4 * (17 * (uint32_t)i + (uint32_t)j)); }
static void fp_random_get(Footprint&, int32_t, int32_t) {}
PORT_FN(0x0041e310, "RandomAIDriver::Get", RandomAIDriver_Get, fp_random_get)

// the constructor: default skills, then register &skill in str[strength] (no bounds check) and keep the name
static RandomAIDriver* __fastcall RandomAIDriver_ctor(RandomAIDriver* self, Edx, int32_t strength, const char* name) {
    uint8_t* skill = (uint8_t*)self + 4;
    SkillInfo2_ctorA(skill, 0);
    self->vtable = P<void*>(VT_RANDOM);
    self->name = 0;
    uint32_t k = 17 * (uint32_t)strength;
    uint32_t base = *P<uint32_t>(S_STR_COUNT0 + 4 * k);
    *P<uint32_t>(S_STR + 4 * (base + k)) = self ? (uint32_t)(uintptr_t)skill : 0;   // the compiler's null check
    ++*P<int32_t>(S_STR_COUNT0 + 4 * k);
    self->name = name;
    return self;
}
static void fp_random_ctor(Footprint& f, RandomAIDriver* self, Edx, int32_t strength, const char*) {
    uint32_t k = 17 * (uint32_t)strength;
    f.add(self, sizeof *self, "RandomAIDriver");
    f.add(P<void>(S_STR + 4 * (*P<uint32_t>(S_STR_COUNT0 + 4 * k) + k)), 4, "RandomAIDriver::str slot");
    f.add(P<void>(S_STR_COUNT0 + 4 * k), 4, "RandomAIDriver::str count");
}
PORT_FN(0x0041e330, "RandomAIDriver::RandomAIDriver", RandomAIDriver_ctor, fp_random_ctor)

// init (vtable +0): recovery_time = random_real(0, 1)
static void __fastcall RandomAIDriver_init(RandomAIDriver* self, Edx) {
    random_real_store(&self->skill.recovery_time, 0x00000000, 0x3f800000);
}
static void fp_random_init(Footprint& f, RandomAIDriver* self, Edx) { f.add(self, sizeof *self, "RandomAIDriver"); }
PORT_FN(0x0041e3c0, "RandomAIDriver::init", RandomAIDriver_init, fp_random_init)

// random_real: Random(1000) * 0.001 * (hi - lo) + lo, returned in ST0 unrounded
static double __cdecl random_real(float lo, float hi) {
    int32_t r = Random(1000);
    double t = D(r) * D(as_float(0x3a83126f));                        // fild; fmul 0.001f
    double w = D(hi) - D(lo);                                          // fld hi; fsub lo
    return t * w + D(lo);                                              // fmulp; fadd lo
}
static void fp_random_real(Footprint&, float, float) {}
PORT_FN(0x0041e3e0, "random_real(driver.obj)", random_real, fp_random_real)   // aicar.obj has its own random_real (0x430350)

// Initialize: every registered driver's init (through its vtable: Easy / Med / Hard / plain)
static void __cdecl RandomAIDriver_Initialize(void) {
    for (uint32_t t = S_STR; t < S_STR + 8 * 0x44; t += 0x44)
        for (int32_t j = 0; at<int32_t>(P<void>(t), 0x40) > j; j++) {
            uint8_t* p = at<uint8_t*>(P<void>(t), 4 * j);
            void* obj = p ? p - 4 : 0;
            VFN(obj, 0, void)(obj, 0);
        }
}
static void fp_random_initialize(Footprint& f) {
    if (!counts_ok(P<int32_t>(S_STR_COUNT0), 0x44, 8)) { f.replay_only = "a random-driver count outside 0..16"; return; }
    for (uint32_t t = S_STR; t < S_STR + 8 * 0x44; t += 0x44)
        for (int32_t j = 0; j < at<int32_t>(P<void>(t), 0x40); j++)
            if (uint8_t* p = at<uint8_t*>(P<void>(t), 4 * j)) f.add(p - 4, sizeof(RandomAIDriver), "RandomAIDriver");
}
PORT_FN(0x0041e410, "RandomAIDriver::Initialize", RandomAIDriver_Initialize, fp_random_initialize)

// Easy / Med / Hard init: the base init, then the skills rolled in their ranges (floats as random_real's
// bounds, bytes and an int from Random(lo, hi))
struct InitRange { uint32_t off, lo, hi; };
static void rolled_init(RandomAIDriver* self, const InitRange* r, int32_t b_lo, int32_t b_hi, int32_t t_lo, int32_t t_hi) {
    RandomAIDriver_initA(self, 0);
    for (int k = 0; k < 7; k++) random_real_store(&at<float>(self, r[k].off), r[k].lo, r[k].hi);
    at<uint8_t>(self, 0x3d) = (uint8_t)Random2(b_lo, b_hi);          // cold_apex_threshold
    at<uint8_t>(self, 0x3c) = (uint8_t)Random2(b_lo, b_hi);          // hot_entry_threshold
    at<int32_t>(self, 0x08) = t_lo ? Random2(t_lo, t_hi) : 1;          // think_ticks (Hard: 1)
    at<uint32_t>(self, 0x38) = 0x3f4ccccd;                             // overspeed_throttle 0.8
}
// (offsets are the RandomAIDriver's: the skill starts at +4)
static const InitRange k_easy[7] = {{0x24, 0x3f400000, 0x3f666666}, {0x20, 0x3f800000, 0x3f99999a}, {0x2c, 0x3fb33333, 0x400ccccd},
                                    {0x28, 0x3f4ccccd, 0x3f99999a}, {0x18, 0x40000000, 0x40800000}, {0x14, 0x3f000000, 0x3fc00000},
                                    {0x0c, 0x3f19999a, 0x3f59999a}};
static const InitRange k_med[7] = {{0x24, 0x3f733333, 0x3f866666}, {0x20, 0x3f800000, 0x40000000}, {0x2c, 0x3f8ccccd, 0x3fc00000},
                                   {0x28, 0x3f666666, 0x3f800000}, {0x18, 0x40000000, 0x40800000}, {0x14, 0x3e800000, 0x3f800000},
                                   {0x0c, 0x3f19999a, 0x3f4ccccd}};
static const InitRange k_hard[7] = {{0x24, 0x3f733333, 0x3faccccd}, {0x20, 0x40133333, 0x402ccccd}, {0x2c, 0x3f4ccccd, 0x3f800000},
                                    {0x28, 0x3f828f5c, 0x3f8ccccd}, {0x18, 0x3fc00000, 0x40200000}, {0x14, 0x3dcccccd, 0x3f333333},
                                    {0x0c, 0x3dcccccd, 0x3ecccccd}};
static void __fastcall EasyAIDriver_init(RandomAIDriver* self, Edx) { rolled_init(self, k_easy, 0x96, 0xc8, 2, 4); }
static void __fastcall MedAIDriver_init(RandomAIDriver* self, Edx) { rolled_init(self, k_med, 0xc8, 0xf0, 1, 3); }
static void __fastcall HardAIDriver_init(RandomAIDriver* self, Edx) { rolled_init(self, k_hard, 0xe6, 0xfa, 0, 0); }
PORT_FN(0x0041ed60, "EasyAIDriver::init", EasyAIDriver_init, fp_random_init)
PORT_FN(0x0041ee40, "MedAIDriver::init", MedAIDriver_init, fp_random_init)
PORT_FN(0x0041ef20, "HardAIDriver::init", HardAIDriver_init, fp_random_init)

// =================================================================================================================
// the skill constructors
// =================================================================================================================
// (not `pure` for the fuzzer, which compares return values bit for bit: a constructor returns its `this`, and
// the original's and the rewrite's are different copies. The world harness checks them.)

static TrackSkillInfo1* __fastcall TrackSkillInfo1_ctor(TrackSkillInfo1* self, Edx) {
    at<uint32_t>(self, 8) = 0xbf800000;                                // -1
    at<uint32_t>(self, 4) = 0x43100000;                                // 144
    at<uint32_t>(self, 0) = 0x3f800000;
    at<uint32_t>(self, 0xc) = 0x3f800000;
    at<uint32_t>(self, 0x10) = 0x3f800000;
    return self;
}
static void fp_track1_ctor(Footprint& f, TrackSkillInfo1* self, Edx) { f.add(self, sizeof *self, "TrackSkillInfo1"); }
PORT_FN(0x0041ec30, "TrackSkillInfo1::TrackSkillInfo1", TrackSkillInfo1_ctor, fp_track1_ctor)

static TrackSkillInfo2* __fastcall TrackSkillInfo2_ctor(TrackSkillInfo2* self, Edx) {
    TrackSkillInfo1_ctorA(self, 0);
    self->_14 = 0;
    return self;
}
static void fp_track2_ctor(Footprint& f, TrackSkillInfo2* self, Edx) { f.add(self, sizeof *self, "TrackSkillInfo2"); }
PORT_FN(0x0041ec10, "TrackSkillInfo2::TrackSkillInfo2", TrackSkillInfo2_ctor, fp_track2_ctor)

static SkillInfo0* __fastcall SkillInfo0_ctor(SkillInfo0* self, Edx) {
    uint8_t* s = (uint8_t*)self;
    at<uint8_t>(s, 0x38) = 0xff;
    at<uint8_t>(s, 0x39) = 0xff;
    at<uint8_t>(s, 0x3a) = 0xff;
    static const struct { uint8_t off; uint32_t v; } k[] = {
        {0x00, 0}, {0x14, 0}, {0x20, 0x3f800000}, {0x10, 0}, {0x30, 0x3f800000}, {0x2c, 0}, {0x34, 0x3f800000},
        {0x28, 0x3f800000}, {0x08, 0x3f19999a}, {0x1c, 0x4019999a}, {0x04, 1}, {0x58, 0x42c80000}, {0x5c, 0x3e2aaaab},
        {0x24, 0x3f800000}, {0x3c, 0x3f800000}, {0x40, 0x3f800000}, {0x54, 0x3f800000}, {0x50, 0x3f800000},
        {0x4c, 0x3f800000}, {0x48, 0x3f800000}, {0x44, 0x3f800000}, {0x60, 0x3f7fbe77}, {0x64, 0x43160000},
        {0x68, 0x41200000}};
    for (auto& e : k) at<uint32_t>(s, e.off) = e.v;
    return self;
}
static void fp_skill0_ctor(Footprint& f, SkillInfo0* self, Edx) { f.add(self, sizeof *self, "SkillInfo0"); }
PORT_FN(0x0041ece0, "SkillInfo0::SkillInfo0", SkillInfo0_ctor, fp_skill0_ctor)

static SkillInfo1* __fastcall SkillInfo1_ctor(SkillInfo1* self, Edx) {
    SkillInfo0_ctorA(self, 0);
    for (int k = 0; k < 16; k++) TrackSkillInfo1_ctorA(&self->tracks[k], 0);
    at<uint32_t>(self, 0x6c) = 0x3f800000;
    return self;
}
static void fp_skill1_ctor(Footprint& f, SkillInfo1* self, Edx) { f.add(self, sizeof *self, "SkillInfo1"); }
PORT_FN(0x0041ec50, "SkillInfo1::SkillInfo1", SkillInfo1_ctor, fp_skill1_ctor)

// SkillInfo2: + car_name = "viper" (0x4ebf38, copied as the original's inlined strcpy)
static SkillInfo2* __fastcall SkillInfo2_ctor(SkillInfo2* self, Edx) {
    SkillInfo0_ctorA(self, 0);
    for (int k = 0; k < 16; k++) TrackSkillInfo2_ctorA(&self->tracks[k], 0);
    at<uint32_t>(self, 0x6c) = 0x3f800000;
    str_copy(self->car_name, (const char*)0x004ebf38);
    return self;
}
static void fp_skill2_ctor(Footprint& f, SkillInfo2* self, Edx) { f.add(self, sizeof *self, "SkillInfo2"); }
PORT_FN(0x0041ec80, "SkillInfo2::SkillInfo2", SkillInfo2_ctor, fp_skill2_ctor)

static AIDriverTable* __fastcall AIDriverTable_ctor(AIDriverTable* self, Edx) {
    self->count = 0;
    return self;
}
static void fp_table_ctor(Footprint& f, AIDriverTable* self, Edx) { f.add(self, sizeof *self, "AIDriverTable"); }
PORT_FN(0x00420e20, "AIDriverTable::AIDriverTable", AIDriverTable_ctor, fp_table_ctor)

static AIDriver1* __fastcall AIDriver1_ctor(AIDriver1* self, Edx) {
    SkillInfo1_ctorA(self, 0);
    self->name = 0;
    return self;
}
static void fp_driver1_ctor(Footprint& f, AIDriver1* self, Edx) { f.add(self, 0x1b4, "AIDriver1"); }
PORT_FN(0x00420e60, "AIDriver1::AIDriver1", AIDriver1_ctor, fp_driver1_ctor)

static AIDriver2* __fastcall AIDriver2_ctor(AIDriver2* self, Edx) {
    SkillInfo2_ctorA(self, 0);
    self->name = 0;
    return self;
}
static void fp_driver2_ctor(Footprint& f, AIDriver2* self, Edx) { f.add(self, 0x214, "AIDriver2"); }
PORT_FN(0x00420eb0, "AIDriver2::AIDriver2", AIDriver2_ctor, fp_driver2_ctor)

// the buckets: 16 drivers each; the count is left alone
static void* __fastcall AIDriverBucket1_ctor(void* self, Edx) {
    for (int k = 0; k < 16; k++) AIDriver1_ctorA((uint8_t*)self + 0x1c0 * k, 0);
    return self;
}
static void fp_bucket1_ctor(Footprint& f, void* self, Edx) { f.add(self, 0x1c00, "AIDriverBucket1"); }
PORT_FN(0x00420e30, "AIDriverBucket1::AIDriverBucket1", AIDriverBucket1_ctor, fp_bucket1_ctor)

static void* __fastcall AIDriverBucket2_ctor(void* self, Edx) {
    for (int k = 0; k < 16; k++) AIDriver2_ctorA((uint8_t*)self + 0x220 * k, 0);
    return self;
}
static void fp_bucket2_ctor(Footprint& f, void* self, Edx) { f.add(self, 0x2200, "AIDriverBucket2"); }
PORT_FN(0x00420e80, "AIDriverBucket2::AIDriverBucket2", AIDriverBucket2_ctor, fp_bucket2_ctor)

// =================================================================================================================
// AIDriverConfig: the tuning dialog and its callbacks (MAIN thread)
// =================================================================================================================

// the dialog's 93 items, in the order the original builds them: {built by the constructor (else stored inline,
// as the compiler did for three of them), the 14 arguments}. Items 0 (+8: the ghost-car viewer), 68/69 (+7:
// 801, the slider's steps; +10/+11: 0 and 4000 from .rdata) and 71 (+10/+11: 0 and 2 from .rdata) are completed
// at run time.
static const struct { uint8_t ctor; uint32_t a[14]; } k_cfg_items[93] = {
    {1, {0x17, 0x0, 0x1a4, 0x96, 0x0, 0x0, 0x4e4db8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 0
    {1, {0x3, 0x0, 0x1a4, 0x1e, 0x0, 0x0, 0x4ebf48, 0x0, 0x420a70, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 1
    {1, {0x3, 0x0, 0x190, 0x1e, 0x0, 0x0, 0x4ebf54, 0x0, 0x420ad0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 2
    {1, {0x5, 0x0, 0x1c2, 0x1e, 0x0, 0x0, 0x51b35c, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 3
    {1, {0x6, 0x0, 0x1a4, 0x3c, 0x0, 0x0, 0x4ebf60, 0x0, 0x51b098, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 4
    {1, {0x5, 0x0, 0x1ef, 0x3c, 0x0, 0x0, 0x4ebf68, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 5
    {1, {0x10, 0x0, 0x190, 0x3c, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b098, 0x0, 0x3f000000, 0x3fc00000, 0x0, 0x0}},   // 6
    {1, {0x10, 0x0, 0x1d6, 0x3c, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b098, 0x1, 0x3f000000, 0x3fc00000, 0x0, 0x0}},   // 7
    {1, {0x6, 0x0, 0x1a4, 0x4b, 0x0, 0x0, 0x4ebf70, 0x0, 0x51b09c, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 8
    {1, {0x5, 0x0, 0x1ef, 0x4b, 0x0, 0x0, 0x4ebf78, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 9
    {1, {0x10, 0x0, 0x190, 0x4b, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b09c, 0x0, 0x42c80000, 0x437a0000, 0x0, 0x0}},   // 10
    {1, {0x10, 0x0, 0x1d6, 0x4b, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b09c, 0x1, 0x42c80000, 0x437a0000, 0x0, 0x0}},   // 11
    {1, {0x6, 0x0, 0x1a4, 0x5a, 0x0, 0x0, 0x4ebf84, 0x0, 0x51b0a4, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 12
    {1, {0x5, 0x0, 0x1ef, 0x5a, 0x0, 0x0, 0x4ebf8c, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 13
    {1, {0x10, 0x0, 0x190, 0x5a, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b0a4, 0x0, 0x0, 0x3fc00000, 0x0, 0x0}},   // 14
    {1, {0x10, 0x0, 0x1d6, 0x5a, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b0a4, 0x1, 0x0, 0x3fc00000, 0x0, 0x0}},   // 15
    {1, {0x6, 0x0, 0x1a4, 0x69, 0x0, 0x0, 0x4ebf94, 0x0, 0x51b0a8, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 16
    {1, {0x5, 0x0, 0x1ef, 0x69, 0x0, 0x0, 0x4ebf9c, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 17
    {1, {0x10, 0x0, 0x190, 0x69, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b0a8, 0x0, 0x3dcccccd, 0x3fc00000, 0x0, 0x0}},   // 18
    {1, {0x10, 0x0, 0x1d6, 0x69, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b0a8, 0x1, 0x3dcccccd, 0x3fc00000, 0x0, 0x0}},   // 19
    {1, {0x5, 0x0, 0x1d6, 0x78, 0x0, 0x0, 0x51b39c, 0x0, 0x0, 0x13, 0x0, 0x0, 0x0, 0x0}},   // 20
    {1, {0x5, 0x0, 0x1ef, 0x78, 0x0, 0x0, 0x4ebfa4, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 21
    {1, {0x10, 0x0, 0x190, 0x78, 0x0, 0x0, 0x4e4db8, 0x6e0, 0x51b0a0, 0x0, 0x41c80000, 0x43480000, 0x0, 0x0}},   // 22
    {1, {0x10, 0x0, 0x1d6, 0x78, 0x0, 0x0, 0x4e4db8, 0x6e0, 0x51b0a0, 0x1, 0x41c80000, 0x43480000, 0x0, 0x0}},   // 23
    {1, {0x6, 0x0, 0x1a4, 0x87, 0x0, 0x0, 0x4ebfac, 0x0, 0x51b0ac, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 24
    {1, {0x5, 0x0, 0x1ef, 0x87, 0x0, 0x0, 0x4ebfb4, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 25
    {1, {0x10, 0x0, 0x190, 0x87, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b0ac, 0x0, 0x0, 0x40000000, 0x0, 0x0}},   // 26
    {1, {0x10, 0x0, 0x1d6, 0x87, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b0ac, 0x1, 0x0, 0x40000000, 0x0, 0x0}},   // 27
    {1, {0xf, 0x0, 0x15, 0x32, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0d4, 0x0, 0x3f666666, 0x3fc00000, 0x0, 0x0}},   // 28
    {1, {0x6, 0x0, 0xa1, 0x32, 0x0, 0x0, 0x4ebfc0, 0x0, 0x51b0d4, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 29
    {1, {0x5, 0x0, 0xdd, 0x32, 0x0, 0x0, 0x4ebfc8, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 30
    {1, {0xf, 0x0, 0x15, 0x41, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0d0, 0x0, 0x3f400000, 0x3fc00000, 0x0, 0x0}},   // 31
    {1, {0x6, 0x0, 0xa1, 0x41, 0x0, 0x0, 0x4ebfd4, 0x0, 0x51b0d0, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 32
    {1, {0x5, 0x0, 0xdd, 0x41, 0x0, 0x0, 0x4ebfdc, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 33
    {1, {0xf, 0x0, 0x15, 0x50, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b11c, 0x0, 0x0, 0x40000000, 0x0, 0x0}},   // 34
    {1, {0x6, 0x0, 0xa1, 0x50, 0x0, 0x0, 0x4ebfec, 0x0, 0x51b11c, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 35
    {1, {0x5, 0x0, 0xdd, 0x50, 0x0, 0x0, 0x4ebff4, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 36
    {1, {0xf, 0x0, 0x15, 0x5f, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0e4, 0x0, 0x0, 0x3f800000, 0x0, 0x0}},   // 37
    {1, {0x6, 0x0, 0xa1, 0x5f, 0x0, 0x0, 0x4ec000, 0x0, 0x51b0e4, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 38
    {1, {0x5, 0x0, 0xdd, 0x5f, 0x0, 0x0, 0x4ec008, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 39
    {1, {0xf, 0x0, 0x15, 0x6e, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0c0, 0x0, 0x0, 0x40200000, 0x0, 0x0}},   // 40
    {1, {0x6, 0x0, 0xa1, 0x6e, 0x0, 0x0, 0x4ebf40, 0x0, 0x51b0c0, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 41
    {1, {0x5, 0x0, 0xdd, 0x6e, 0x0, 0x0, 0x4ec014, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 42
    {1, {0xf, 0x0, 0x15, 0x7d, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0c4, 0x0, 0x3f800000, 0x40800000, 0x0, 0x0}},   // 43
    {1, {0x6, 0x0, 0xa1, 0x7d, 0x0, 0x0, 0x4ebf40, 0x0, 0x51b0c4, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 44
    {1, {0x5, 0x0, 0xdd, 0x7d, 0x0, 0x0, 0x4ec024, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 45
    {1, {0xf, 0x0, 0x15, 0x8c, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0ec, 0x0, 0x0, 0x40a00000, 0x0, 0x0}},   // 46
    {1, {0x6, 0x0, 0xa1, 0x8c, 0x0, 0x0, 0x4ec038, 0x0, 0x51b0ec, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 47
    {1, {0x5, 0x0, 0xdd, 0x8c, 0x0, 0x0, 0x4ec040, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 48
    {1, {0xf, 0x0, 0x15, 0x9b, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0f0, 0x0, 0x3f000000, 0x40000000, 0x0, 0x0}},   // 49
    {1, {0x6, 0x0, 0xa1, 0x9b, 0x0, 0x0, 0x4ec050, 0x0, 0x51b0f0, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 50
    {1, {0x5, 0x0, 0xdd, 0x9b, 0x0, 0x0, 0x4ec058, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 51
    {0, {0xf, 0x0, 0x15, 0xaa, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0cc, 0x0, 0x3f800000, 0x40400000, 0x0, 0x0}},   // 52
    {1, {0x6, 0x0, 0xa1, 0xaa, 0x0, 0x0, 0x4ec06c, 0x0, 0x51b0cc, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 53
    {1, {0x5, 0x0, 0xdd, 0xaa, 0x0, 0x0, 0x4ec074, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 54
    {1, {0xf, 0x0, 0x15, 0xb9, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0b8, 0x0, 0x0, 0x3f733333, 0x0, 0x0}},   // 55
    {1, {0x6, 0x0, 0xa1, 0xb9, 0x0, 0x0, 0x4ebf40, 0x0, 0x51b0b8, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 56
    {1, {0x5, 0x0, 0xdd, 0xb9, 0x0, 0x0, 0x4ec080, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 57
    {0, {0xf, 0x0, 0x15, 0xc8, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b08c, 0x0, 0x0, 0x3f800000, 0x0, 0x0}},   // 58
    {1, {0x6, 0x0, 0xa1, 0xc8, 0x0, 0x0, 0x4ec090, 0x0, 0x51b08c, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 59
    {1, {0x5, 0x0, 0xdd, 0xc8, 0x0, 0x0, 0x4ec098, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 60
    {1, {0xf, 0x0, 0x15, 0xd7, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b090, 0x0, 0x0, 0x3f800000, 0x0, 0x0}},   // 61
    {1, {0x6, 0x0, 0xa1, 0xd7, 0x0, 0x0, 0x4ec0a4, 0x0, 0x51b090, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 62
    {1, {0x5, 0x0, 0xdd, 0xd7, 0x0, 0x0, 0x4ec0ac, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 63
    {0, {0xf, 0x0, 0x15, 0xe6, 0x64, 0x8, 0x4e4db8, 0xfffffffa, 0x51b088, 0x0, 0x3f800000, 0x40c00000, 0x0, 0x0}},   // 64
    {1, {0x6, 0x0, 0xa1, 0xe6, 0x0, 0x0, 0x4ec0b8, 0x0, 0x51b088, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 65
    {1, {0x5, 0x0, 0xdd, 0xe6, 0x0, 0x0, 0x4ec0c0, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 66
    {1, {0x6, 0x0, 0x15, 0xf5, 0x0, 0x0, 0x4ec0cc, 0x0, 0x51b0dc, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 67
    {1, {0x10, 0x0, 0x1, 0xf5, 0x0, 0x0, 0x4e4db8, 0x0, 0x51b0dc, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 68
    {1, {0x10, 0x0, 0x44, 0xf5, 0x0, 0x0, 0x4e4db8, 0x0, 0x51b0dc, 0x1, 0x0, 0x0, 0x0, 0x0}},   // 69
    {1, {0x5, 0x0, 0xdd, 0xf5, 0x0, 0x0, 0x4ec0d4, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 70
    {1, {0xf, 0x0, 0x15, 0x104, 0x64, 0x8, 0x4e4db8, 0xffffffc4, 0x51b0e0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 71
    {1, {0x6, 0x0, 0xa1, 0x104, 0x0, 0x0, 0x4ec0e4, 0x0, 0x51b0e0, 0x15, 0x42c80000, 0x0, 0x0, 0x0}},   // 72
    {1, {0x5, 0x0, 0xdd, 0x104, 0x0, 0x0, 0x4ec0ec, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 73
    {1, {0x6, 0x0, 0x15, 0x113, 0x0, 0x0, 0x4ec0f4, 0x0, 0x51b084, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 74
    {1, {0x10, 0x0, 0x1, 0x113, 0x0, 0x0, 0x4e4db8, 0x64, 0x51b084, 0x0, 0x0, 0x42c60000, 0x0, 0x0}},   // 75
    {1, {0x10, 0x0, 0x44, 0x113, 0x0, 0x0, 0x4e4db8, 0x64, 0x51b084, 0x1, 0x0, 0x42c60000, 0x0, 0x0}},   // 76
    {1, {0x5, 0x0, 0xdd, 0x113, 0x0, 0x0, 0x4ec0fc, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 77
    {1, {0x6, 0x0, 0x15, 0x122, 0x0, 0x0, 0x4ec108, 0x0, 0x51b080, 0x15, 0x3f800000, 0x0, 0x0, 0x0}},   // 78
    {1, {0x10, 0x0, 0x1, 0x122, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b080, 0x0, 0x0, 0x42c80000, 0x0, 0x0}},   // 79
    {1, {0x10, 0x0, 0x44, 0x122, 0x0, 0x0, 0x4e4db8, 0x65, 0x51b080, 0x1, 0x0, 0x42c80000, 0x0, 0x0}},   // 80
    {1, {0x5, 0x0, 0xdd, 0x122, 0x0, 0x0, 0x4ec110, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 81
    {1, {0x5, 0x0, 0x208, 0x181, 0x0, 0x0, 0x51b2dc, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 82
    {1, {0x3, 0x0, 0x1e0, 0x17c, 0x0, 0x0, 0x4ec11c, 0x0, 0x420b80, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 83
    {1, {0x3, 0x0, 0x1c7, 0x17c, 0x0, 0x0, 0x4ec128, 0x0, 0x420b30, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 84
    {1, {0x5, 0x0, 0x208, 0x168, 0x0, 0x0, 0x51b31c, 0x0, 0x0, 0xc, 0x0, 0x0, 0x0, 0x0}},   // 85
    {1, {0x3, 0x0, 0x1e0, 0x163, 0x0, 0x0, 0x4ec134, 0x0, 0x420a20, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 86
    {1, {0x3, 0x0, 0x1c7, 0x163, 0x0, 0x0, 0x4ec140, 0x0, 0x4209d0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 87
    {1, {0x2, 0xffffffff, 0x1fe, 0x1b2, 0x0, 0x0, 0x4ec14c, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 88
    {1, {0x2, 0x0, 0x190, 0x1b2, 0x0, 0x0, 0x4ec154, 0x0, 0x4208a0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 89
    {1, {0x2, 0x0, 0x3, 0x1b2, 0x0, 0x0, 0x4ec160, 0x0, 0x4206d0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 90
    {1, {0x2, 0x1, 0x6e, 0x1b2, 0x0, 0x0, 0x4ec168, 0x0, 0x4206d0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 91
    {1, {0x2, 0x0, 0xf0, 0x1b2, 0x0, 0x0, 0x4ec170, 0x0, 0x4205a0, 0x0, 0x0, 0x0, 0x0, 0x0}},   // 92
};

static uint8_t __cdecl AIDriverConfig(int) {
    void* viewer = CreateIGhostCarViewer(P<int32_t>(S_CUR_CAR), P<int32_t>(S_CUR_STR), P<int32_t>(S_CUR_TRK));
    UIDialog dlg;
    UIDialogItem items[94];
    int32_t steps = 0;
    for (int k = 0; k < 93; k++) {
        uint32_t a[14];
        memcpy(a, k_cfg_items[k].a, sizeof a);
        if (k == 0) a[8] = (uint32_t)(uintptr_t)viewer;
        if (k == 68) {                                                  // (4000 - 0) / 5 + 1, on the x87 at run time
            steps = x87_ftol(D(*P<float>(C_4000)) - D(*P<float>(C_0))) / 5 + 1;
        }
        if (k == 68 || k == 69) {
            a[7] = (uint32_t)steps;
            a[10] = *P<uint32_t>(C_0);
            a[11] = *P<uint32_t>(C_4000);
        }
        if (k == 71) {
            a[10] = *P<uint32_t>(C_0B);
            a[11] = *P<uint32_t>(C_2);
        }
        if (k_cfg_items[k].ctor)
            UIDialogItem_ctor(&items[k], 0, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13]);
        else
            memcpy(&items[k], a, sizeof a);
    }
    movsd(&items[93], P<void>(S_ITEM_END), 14);
    dlg.title = 0x004ec178;
    cur_str() = 0;
    dlg.items = items;
    cur_car() = 0;
    cur_trk() = 0;
    const char* section = *P<const char*>(S_OPTIONS_SECTION);            // "AI"
    dlg.subtitle = 0x004ec17c;
    dlg._08 = -2;
    dlg.idle = 0x004202c0;                                              // idle
    OptionsGetI(section, (const char*)0x004ec180, P<int32_t>(S_CUR_TRK));   // cur_track
    OptionsGetI(section, (const char*)0x004ec18c, P<int32_t>(S_CUR_CAR));   // cur_car
    OptionsGetI(section, (const char*)0x004ec194, P<int32_t>(S_CUR_STR));   // cur_str
    copy_lounge_to_tabA();
    UIDoDialog(&dlg, *gxScreenWid, *gxScreenHit, -999, -999, 1);
    save_cbA(0);
    if (viewer) VFN(viewer, 0, void, int)(viewer, 0, 1);               // the deleting destructor
    void* lounge = *P<void*>(S_LOUNGE_IMPL);
    VFN(lounge, 8, uint8_t)(lounge, 0);                                 // Reload
    OptionsSetI(section, (const char*)0x004ec19c, cur_trk());
    OptionsSetI(section, (const char*)0x004ec1a8, cur_car());
    OptionsSetI(section, (const char*)0x004ec1b0, cur_str());
    learn_dialog();
    return 0;
}
static void fp_ai_driver_config(Footprint& f, int) { f.replay_only = "runs a modal dialog; allocates the ghost-car viewer; saves and reloads the drivers"; }
PORT_FN(0x0041f050, "AIDriverConfig", AIDriverConfig, fp_ai_driver_config)

// idle: the dialog's idle callback
static uint8_t __cdecl idle(int32_t*) {
    update_lap_stringA();
    return 0;
}
static void fp_idle(Footprint& f, int32_t*) { fp_cfg(f); }
PORT_FN(0x004202c0, "idle", idle, fp_idle)

// update_lap_string: the current track's lap time (+8, passed as its bits) as text
static void __cdecl update_lap_string(void) {
    const char* s = PhysicsTimeString(*P<uint32_t>(S_LIVE_LAP), 0);
    str_copy(P<char>(S_LAP_TEXT), s);
}
PORT_FN(0x004202d0, "update_lap_string", update_lap_string, fp_cfg)

// copy_lounge_to_tab: every strength's drivers from the lounge (Count / Get through its vtable) into the tab
static void __cdecl copy_lounge_to_tab(void) {
    int32_t i = 0;
    for (uint8_t* bucket = P<uint8_t>(S_TAB); bucket < P<uint8_t>(S_TAB + TAB_SIZE); bucket += 0x2204, i++) {
        void* l = *P<void*>(S_LOUNGE_IMPL);
        int32_t n = VFN(l, 0, int32_t, int32_t)(l, 0, i);
        for (int32_t j = 0; n > j; j++) {
            void* l2 = *P<void*>(S_LOUNGE_IMPL);
            const void* d = VFN(l2, 4, const void*, int32_t, int32_t)(l2, 0, i, j);
            movsd(bucket + 0x220 * j, d, 0x88);
        }
        at<int32_t>(bucket, 0x2200) = n;
    }
    if (!(tab_count(cur_str()) > cur_car())) cur_car() = 0;
    copy_tab_to_liveA();
    cfg_update_stringsA();
}
static void fp_copy_lounge_to_tab(Footprint& f) {
    void* l = *P<void*>(S_LOUNGE_IMPL);
    if (!l || *(uint32_t*)l != VT_LOUNGE || !counts_ok(&((AIDriverLounge*)l)->tables[0].count, 0x44, 8)) {
        f.replay_only = "a lounge count outside 0..16 writes past the tab";
        return;
    }
    fp_cfg(f);
}
PORT_FN(0x00420310, "copy_lounge_to_tab", copy_lounge_to_tab, fp_copy_lounge_to_tab)

// cfg_update_strings: "NN of NN", the strength's name, the track's name (+ " reversed" for 8..15), the lap time
static void __cdecl cfg_update_strings(void) {
    int32_t n = tab_count(cur_str());
    g_sprintf(P<char>(S_CAR_TEXT), (const char*)0x004ec1b8, cur_car() + 1, n);
    str_copy(P<char>(S_STRENGTH_TEXT), GetAIStrengthString(cur_str()));
    const char* rev = cur_trk() / 8 ? (const char*)0x004ec1c8 : (const char*)0x004ec1d4;
    const char* name = GetTrackName(cur_trk() % 8);
    g_sprintf(P<char>(S_TRACK_TEXT), (const char*)0x004ec1d8, name, rev);
    update_lap_stringA();
}
PORT_FN(0x004203d0, "cfg_update_strings", cfg_update_strings, fp_cfg)

static void __cdecl copy_tab_to_live(void) {
    copy_to_liveA(tab_driver(cur_str(), cur_car()));
}
PORT_FN(0x00420490, "copy_tab_to_live", copy_tab_to_live, fp_cfg)

// copy_to_live: the driver into the live copy, and the dialog's float views of its integer fields; the current
// track's settings into the live track
static void __cdecl copy_to_live(const AIDriver2* d) {
    const uint8_t* s = (const uint8_t*)d;
    movsd(P<void>(S_LIVE), s, 0x88);
    *P<float>(S_LIVE_F04) = (float)*(const int32_t*)(s + 4);
    *P<float>(S_LIVE_F0E) = (float)*(const int16_t*)(s + 0xe);
    *P<float>(S_LIVE_F0C) = (float)*(const int16_t*)(s + 0xc);
    const double k = D(as_float(0x3b808081));                          // 1/255
    for (int b = 0; b < 3; b++)                                         // fild qword; fmul; fsubr 1.0
        *P<float>(S_LIVE_B38 + 4 * b) = (float)(D(1.0f) - D(s[0x38 + b]) * k);
    movsd(P<void>(S_LIVE_TRACK), P<void>(S_LIVE_TRACKS + 0x18 * (uint32_t)cur_trk()), 6);
}
static void fp_copy_to_live(Footprint& f, const AIDriver2*) { fp_cfg(f); }
PORT_FN(0x004204d0, "copy_to_live", copy_to_live, fp_copy_to_live)

// copy_from_live: the float views back into the live driver (__ftol), the live track into its slot, the live
// driver out
static void __cdecl copy_from_live(AIDriver2* out) {
    *P<int16_t>(S_LIVE + 0xe) = (int16_t)x87_ftol(*P<float>(S_LIVE_F0E));
    *P<int16_t>(S_LIVE + 0xc) = (int16_t)x87_ftol(*P<float>(S_LIVE_F0C));
    *P<int32_t>(S_LIVE + 4) = x87_ftol(*P<float>(S_LIVE_F04));
    for (int b = 0; b < 3; b++)                                         // fld 1; fsub; fmul 255
        *P<uint8_t>(S_LIVE + 0x38 + b) = (uint8_t)x87_ftol((D(1.0f) - D(*P<float>(S_LIVE_B38 + 4 * b))) * D(255.0f));
    movsd(P<void>(S_LIVE_TRACKS + 0x18 * (uint32_t)cur_trk()), P<void>(S_LIVE_TRACK), 6);
    movsd(out, P<void>(S_LIVE), 0x88);
}
static void fp_copy_from_live(Footprint& f, AIDriver2* out) {
    fp_cfg(f);
    f.add(out, sizeof *out, "AIDriver2");
}
PORT_FN(0x004207d0, "copy_from_live", copy_from_live, fp_copy_from_live)

// delete_cb: remove the current driver (unless it's the empty slot past the last one): shift the rest down, a
// default driver into the freed last slot. That default is built on the stack, and the bytes its constructor
// leaves alone (SkillInfo0 +0x0c, +0x18, +0x3b, car_name past "viper", +0x214..0x21f) are the original's stack
// junk (0 here): a freeing call is replay_only.
static uint8_t __cdecl delete_cb(int) {
    int32_t s = cur_str();
    uint32_t so = (uint32_t)s * 0x2204;
    int32_t n = tab_count(s);
    if (n > 0 && n != cur_car()) {
        if (n - 1 > cur_car()) {
            int32_t car = cur_car();
            uint8_t* dst = P<uint8_t>(S_TAB + (uint32_t)car * 0x220 + so);
            g_memmove(dst, dst + 0x220, (size_t)((uint32_t)(n - car) * 0x220 - 0x220));
        }
        alignas(4) uint8_t tmp[0x220] = {};
        SkillInfo2_ctorA(tmp, 0);
        at<uint32_t>(tmp, 0x210) = 0;
        --tab_count(cur_str());
        int32_t c = tab_count(cur_str());
        movsd(tab_driver(cur_str(), c), tmp, 0x88);
        if (cur_car() != 0) {
            int32_t c2 = tab_count(cur_str());
            if (!(c2 > cur_car())) cur_car() = c2 - 1;
        }
        copy_tab_to_liveA();
    }
    cfg_update_stringsA();
    return 0;
}
static void fp_delete_cb(Footprint& f, int) {
    int32_t s = cur_str(), c = cur_car();
    if ((uint32_t)s > 7 || c < 0 || c > 16 || !counts_ok(P<int32_t>(S_TAB_COUNT0), 0x2204, 8)) {
        f.replay_only = "a strength / car / count out of range writes outside the tab";
        return;
    }
    int32_t n = tab_count(s);
    if (n > 0 && n != c) {                            // (test/world_aidriver.cpp masks those bytes)
        f.replay_only = "the freed slot gets the original's uninitialised stack bytes (SkillInfo0 +0x0c/+0x18/+0x3b, "
                        "car_name past \"viper\", +0x214..): nothing to compare them with";
        return;
    }
    fp_cfg(f);
}
PORT_FN(0x004205a0, "delete_cb", delete_cb, fp_delete_cb)

// copypaste_cb: 0 copies the live driver to the clipboard (a function-static, built on first use and destroyed
// at exit); anything else pastes it: the clipboard's driver with the live driver's tracks, except the current
// track's, which is the clipboard's. An empty clipboard says so in a box.
static uint8_t __cdecl copypaste_cb(int paste) {
    uint8_t g = *P<uint8_t>(S_CLIP_GUARD);
    if (!(g & 1)) {
        *P<uint8_t>(S_CLIP_GUARD) = g | 1;
        SkillInfo2_ctorA(P<void>(S_CLIP), 0);
        *P<uint32_t>(S_CLIP + 0x210) = 0;
        g_atexit((void*)0x00420890);                                   // $E192 (a bare ret)
    }
    if (paste) {
        if (*P<uint8_t>(S_HAVE_CLIP)) {
            alignas(4) uint8_t tmp[0x220];
            SkillInfo2_ctorA(tmp, 0);
            at<uint32_t>(tmp, 0x210) = 0;
            copy_from_liveA(tmp);
            copy_to_liveA(P<void>(S_CLIP));
            movsd(P<void>(S_LIVE_TRACKS), tmp + 0x90, 0x60);
            uint32_t t = 0x18 * (uint32_t)cur_trk();
            movsd(P<void>(S_LIVE_TRACKS + t), P<void>(S_CLIP_TRACKS + t), 6);
            return 0;
        }
        UIDoDratBox((const char*)0x004ec1f4, (const char*)0x004ec1e0, 0);   // Whatever / Clipboard is empty.
        return 0;
    }
    *P<uint8_t>(S_HAVE_CLIP) = 1;
    copy_from_liveA(P<void>(S_CLIP));
    return 0;
}
static void fp_copypaste_cb(Footprint& f, int paste) {
    if (!(*P<uint8_t>(S_CLIP_GUARD) & 1)) { f.replay_only = "builds the clipboard (registers an atexit)"; return; }
    if (paste && !*P<uint8_t>(S_HAVE_CLIP)) { f.replay_only = "opens a message box"; return; }
    fp_cfg(f);
    f.add(P<void>(S_CLIP), 0x220, "clipboard");
    f.add(P<void>(S_HAVE_CLIP), 1, "clipboard full");
}
PORT_FN(0x004206d0, "copypaste_cb", copypaste_cb, fp_copypaste_cb)

static uint8_t __cdecl undo_cb(int) {
    copy_tab_to_liveA();
    return 0;
}
static void fp_cb(Footprint& f, int) { fp_cfg(f); }
PORT_FN(0x004208a0, "undo_cb", undo_cb, fp_cb)

// save_cb: the live driver into the tab; every strength must have a car, else a box each and no save
static uint8_t __cdecl save_cb(int) {
    char buf[0x80];
    copy_live_to_tabA();
    uint8_t ok = 1;
    for (int32_t i = 0; i < 8; i++)
        if (tab_count(i) == 0) {
            g_sprintf(buf, (const char*)0x004ec200, GetAIStrengthString(i));   // %s has no cars
            UIDoDratBox((const char*)0x004ec210, buf, 0);                    // Doh!
            ok = 0;
        }
    if (ok) save_resource2A((const char*)0x004ec218, P<void>(S_TAB));        // aidriver.adr
    return 0;
}
static void fp_save_cb(Footprint& f, int) { f.replay_only = "saves aidriver.adr (or opens message boxes)"; }
PORT_FN(0x004208b0, "save_cb", save_cb, fp_save_cb)

// copy_live_to_tab: the live driver back into its tab slot; a changed driver past the end extends the count
static void __cdecl copy_live_to_tab(void) {
    alignas(4) uint8_t tmp[0x220];
    SkillInfo2_ctorA(tmp, 0);
    at<uint32_t>(tmp, 0x210) = 0;
    copy_from_liveA(tmp);
    uint8_t* e = tab_driver(cur_str(), cur_car());
    if (memcmp(e, P<void>(S_LIVE), 0x220) != 0) {
        int32_t& c = tab_count(cur_str());
        int32_t car = cur_car();
        if (!(c > car)) c = car + 1;
    }
    movsd(e, P<void>(S_LIVE), 0x88);
}
static void fp_copy_live_to_tab(Footprint& f) {
    int32_t s = cur_str(), c = cur_car();
    if ((uint32_t)s > 7 || c < 0 || c > 16) { f.replay_only = "a strength / car out of range writes outside the tab"; return; }
    fp_cfg(f);
}
PORT_FN(0x00420930, "copy_live_to_tab", copy_live_to_tab, fp_copy_live_to_tab)

// the arrows: car (0..count, wrapping; 16 at most), track (0..15), strength (0..7, the car clamped to the count)
static uint8_t __cdecl prev_car(int) {
    copy_live_to_tabA();
    int32_t c = (int32_t)((uint32_t)cur_car() - 1);
    cur_car() = c;
    if (c < 0) cur_car() = tab_count(cur_str());
    if (!(cur_car() < 16)) cur_car() = 0;
    copy_tab_to_liveA();
    cfg_update_stringsA();
    return 0;
}
static void fp_car_cb(Footprint& f, int) {
    int32_t s = cur_str(), c = cur_car();
    if ((uint32_t)s > 7 || c < 0 || c > 16) { f.replay_only = "a strength / car out of range writes outside the tab"; return; }
    fp_cfg(f);
}
PORT_FN(0x004209d0, "prev_car", prev_car, fp_car_cb)

static uint8_t __cdecl next_car(int) {
    copy_live_to_tabA();
    cur_car() = (int32_t)((uint32_t)cur_car() + 1);
    int32_t c = cur_car();
    if (tab_count(cur_str()) < c || !(c < 16)) cur_car() = 0;
    copy_tab_to_liveA();
    cfg_update_stringsA();
    return 0;
}
PORT_FN(0x00420a20, "next_car", next_car, fp_car_cb)

static uint8_t __cdecl next_trk(int) {
    movsd(P<void>(S_LIVE_TRACKS + 0x18 * (uint32_t)cur_trk()), P<void>(S_LIVE_TRACK), 6);
    cur_trk() = (int32_t)((uint32_t)cur_trk() + 1);
    if (!(cur_trk() < 16)) cur_trk() = 0;
    movsd(P<void>(S_LIVE_TRACK), P<void>(S_LIVE_TRACKS + 0x18 * (uint32_t)cur_trk()), 6);
    cfg_update_stringsA();
    return 0;
}
static void fp_trk_cb(Footprint& f, int) {
    int32_t t = cur_trk();
    if (t < 0 || t > 16) { f.replay_only = "a track out of range writes outside the live driver"; return; }
    fp_cfg(f);
}
PORT_FN(0x00420a70, "next_trk", next_trk, fp_trk_cb)

static uint8_t __cdecl prev_trk(int) {
    movsd(P<void>(S_LIVE_TRACKS + 0x18 * (uint32_t)cur_trk()), P<void>(S_LIVE_TRACK), 6);
    int32_t t = (int32_t)((uint32_t)cur_trk() - 1);
    cur_trk() = t;
    if (t < 0) cur_trk() = 15;
    movsd(P<void>(S_LIVE_TRACK), P<void>(S_LIVE_TRACKS + 0x18 * (uint32_t)cur_trk()), 6);
    cfg_update_stringsA();
    return 0;
}
PORT_FN(0x00420ad0, "prev_trk", prev_trk, fp_trk_cb)

static uint8_t __cdecl prev_str(int) {
    copy_live_to_tabA();
    int32_t s = (int32_t)((uint32_t)cur_str() - 1);
    cur_str() = s;
    if (s < 0) cur_str() = 7;
    int32_t n = tab_count(cur_str());
    if (!(n >= cur_car())) cur_car() = n;
    copy_tab_to_liveA();
    cfg_update_stringsA();
    return 0;
}
PORT_FN(0x00420b30, "prev_str", prev_str, fp_car_cb)

static uint8_t __cdecl next_str(int) {
    copy_live_to_tabA();
    cur_str() = (int32_t)((uint32_t)cur_str() + 1);
    if (cur_str() == 8) cur_str() = 0;
    int32_t n = tab_count(cur_str());
    if (!(n >= cur_car())) cur_car() = n;
    copy_tab_to_liveA();
    cfg_update_stringsA();
    return 0;
}
PORT_FN(0x00420b80, "next_str", next_str, fp_car_cb)

// =================================================================================================================
// start-up, the driver map, the getters
// =================================================================================================================

static void __cdecl AIDriverBegin(void) {
    void* p = MemAlloc(0x22c);
    void* l = p ? Lounge_ctorA(p, 0) : 0;
    *P<void*>(S_LOUNGE_IMPL) = l;
    *P<void*>(S_LOUNGE) = l;
    AIResetDriverMapA();
}
static void fp_ai_driver_begin(Footprint& f) { f.replay_only = "allocates the lounge, loads the drivers, rolls the random drivers"; }
PORT_FN(0x00420bd0, "AIDriverBegin", AIDriverBegin, fp_ai_driver_begin)

static void __cdecl AIDriverEnd(void) {
    void* p = *P<void*>(S_LOUNGE_IMPL);
    if (p) {
        Lounge_dtorA(p, 0);
        op_delete(p);
    }
    *P<void*>(S_LOUNGE_IMPL) = 0;
    *P<void*>(S_LOUNGE) = 0;
}
static void fp_ai_driver_end(Footprint& f) { f.replay_only = "frees the lounge and its resource"; }
PORT_FN(0x00420c00, "AIDriverEnd", AIDriverEnd, fp_ai_driver_end)

static void __cdecl AIResetDriverMap(void) {
    for (uint32_t i = 0; i < 16; i++) P<uint32_t>(S_MAP)[i] = i;
    *P<int32_t>(S_MAP_N) = 16;
}
static void fp_map(Footprint& f) { f.add(P<void>(S_MAP), 0x44, "driver map"); }
PORT_FN(0x00420c30, "AIResetDriverMap", AIResetDriverMap, fp_map)

// AISetDriverMap: the count first, then the map (rep movsd of (n * 4) >> 2 dwords: a count past 16 overwrites
// the count it just stored, and beyond)
static void __cdecl AISetDriverMap(int32_t* map, int32_t n) {
    verify_driver_mapA(map, n);
    *P<int32_t>(S_MAP_N) = n;
    movsd(P<void>(S_MAP), map, ((uint32_t)n * 4) >> 2);
}
static void fp_set_map(Footprint& f, int32_t*, int32_t n) {
    uint32_t words = ((uint32_t)n * 4) >> 2;
    if (words > 256) { f.replay_only = "a huge map count"; return; }
    f.add(P<void>(S_MAP), words * 4 > 0x44 ? words * 4 : 0x44, "driver map");
}
PORT_FN(0x00420c50, "AISetDriverMap", AISetDriverMap, fp_set_map)

// verify_driver_map: a driver 0..15 used twice panics (out-of-range entries are ignored)
static void __cdecl verify_driver_map(int32_t* map, int32_t n) {
    int32_t seen[16] = {};
    for (; n > 0; n--, map++) {
        int32_t d = *map;
        if (d < 0 || d >= 16) continue;
        if (!seen[d]) seen[d] = 1;
        else LogPanic((const char*)0x004ec23c, d);
    }
}
static void fp_verify_map(Footprint&, int32_t*, int32_t) {}
PORT_FN(0x00420c80, "verify_driver_map", verify_driver_map, fp_verify_map)

static int32_t __cdecl AIGetDriverForCar(int32_t car) { return P<int32_t>(S_MAP)[car] % 7; }
static void fp_get_driver_for_car(Footprint&, int32_t) {}
PORT_FN(0x00420ce0, "AIGetDriverForCar", AIGetDriverForCar, fp_get_driver_for_car)

static int32_t __cdecl AIGetCarForDriver(int32_t d) {
    int32_t i = 0;
    if (*P<int32_t>(S_MAP_N) > 0)
        do {
            if (P<int32_t>(S_MAP)[i] == d) return i;
            i++;
        } while (i < *P<int32_t>(S_MAP_N));
    LogPanic((const char*)0x004ec278, d);                               // AIGetCarForDriver: No car for driver %d
    return 0;
}
PORT_FN(0x00420d00, "AIGetCarForDriver", AIGetCarForDriver, fp_get_driver_for_car)

// AIGetDriverNameByCar: the name of the driver the lounge gives this car at the current strength
static void __cdecl AIGetDriverNameByCar(char* buf, int32_t car) {
    int32_t d = AIGetDriverForCarA(car);
    int32_t s = AIGetStrength();
    void* l = *P<void*>(S_LOUNGE_IMPL);
    const uint8_t* p = VFN(l, 4, const uint8_t*, int32_t, int32_t)(l, 0, s, d);
    str_copy(buf, *(const char* const*)(p + 0x210));
}
static void fp_get_driver_name(Footprint& f, char* buf, int32_t car) {
    // the name is found the way the function finds it (the lounge's own Get, without the call)
    uint32_t len = 64;
    AIDriverLounge* l = *P<AIDriverLounge*>(S_LOUNGE_IMPL);
    int32_t d = P<int32_t>(S_MAP)[car] % 7, s = *P<int32_t>(S_AI_STRENGTH);
    if (l && *(uint32_t*)l == VT_LOUNGE && (uint32_t)s < 8 && l->tables[s].count > 0) {
        int32_t k = (int32_t)((uint32_t)l->offset + (uint32_t)d) % l->tables[s].count;
        const uint8_t* p = at<const uint8_t*>(l, 8 + 4 * (17 * (uint32_t)s + (uint32_t)k));
        const char* name = p ? *(const char* const*)(p + 0x210) : 0;
        if (name) len = (uint32_t)strlen(name) + 1;
    }
    f.add(buf, len, "driver name");
}
PORT_FN(0x00420d40, "AIGetDriverNameByCar", AIGetDriverNameByCar, fp_get_driver_name)

static const AIDriver2* __cdecl AIGetSkill(int32_t car) {
    int32_t d = AIGetDriverForCarA(car);
    int32_t s = AIGetStrength();
    void* l = *P<void*>(S_LOUNGE_IMPL);
    return VFN(l, 4, const AIDriver2*, int32_t, int32_t)(l, 0, s, d);
}
PORT_FN(0x00420d90, "AIGetSkill", AIGetSkill, fp_get_driver_for_car)

// AIGetTrackNumber: a track number 0..7 plus 8 when reversed. The flag is a byte; the callers push (and the
// named-track overload passes on) a whole dword.
static int32_t __cdecl AIGetTrackNumber_name(const char* name, uint32_t reversed) {
    int32_t t = GetTrackNumber(name);
    return AIGetTrackNumber_intA(t, reversed);
}
static void fp_track_number_name(Footprint&, const char*, uint32_t) {}
PORT_FN(0x00420dc0, "AIGetTrackNumber(name)", AIGetTrackNumber_name, fp_track_number_name)

static int32_t __cdecl AIGetTrackNumber_int(int32_t t, uint32_t reversed) { return (int32_t)((uint32_t)t + 8 * (uint32_t)(uint8_t)reversed); }
static void fp_track_number_int(Footprint& f, int32_t, uint32_t) { f.pure = true; }
PORT_FN(0x00420de0, "AIGetTrackNumber(int)", AIGetTrackNumber_int, fp_track_number_int)

static int32_t __cdecl AIGetTrackNumber_world(void) {
    const uint8_t* o = WorldGameOptions();
    uint32_t reversed = (uint32_t)(uintptr_t)o & 0xffffff00u | o[0x25];  // mov al, [eax+0x25]; push eax
    int32_t t = WorldGetTrackNumber();
    return AIGetTrackNumber_intA(t, reversed);
}
PORT_FN(0x00420df0, "AIGetTrackNumber()", AIGetTrackNumber_world, fp_none)

static uint8_t __cdecl AITrackIsReversed(int32_t t) { return t >= 8; }
static void fp_track_is_reversed(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x00420e10, "AITrackIsReversed", AITrackIsReversed, fp_track_is_reversed)
