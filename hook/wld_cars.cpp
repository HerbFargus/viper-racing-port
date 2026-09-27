// wld_cars.cpp -- M3 world stage, group W3: car files, world objects and the race's car list, rewritten.
//
//   carfile.obj (library world)  the .car's car file (CARF, 0x228 bytes) and its upgrade set (<car>.ugs, UPGS),
//                combined with a CarSetupData into the CarData a Car is built from (CarFileCombine); the setups:
//                the player's per-track <user>\setups\<track>.csu file (a CarSetup, 0xd4 bytes), the .ccs
//                resources (CCS0, a CarSetupData, 0x8c bytes: <track>.ccs, <car>.ccs, default.ccs, aidef.ccs) and
//                the built-in generic setup.
//   wob.obj (library world)      the graphics side's objects: GraphObject (the base: a vtable), FrameObject (+ a
//                Frame), WorldObject (a FrameObject that follows a physics object: +0x34 its packet offset, +0x38
//                the last tick seen), ModelObject / WobbleObject (+0x3c a model).
//   game.obj (library root)      the top of a race: the single- and multi-player loops (GameDoSingle /
//                GameDoMulti), their begin / end, the car list (GenerateCarList), loading the track, the cars
//                and their setups (LoadRace / SetupCars / LoadCars / LoadTrack and the unloads), the game state.
//
// Skipped: the $E initialisers (static construction, run by the CRT before anything could be hooked); carfile.obj's
// ASSERT_MSG (0x464950, a bare `ret`: still called, as the original calls it); GraphObject::Update (0x469880, a bare
// `ret`); GraphObject::Draw2D (0x469860, a bare `ret 4` taking a gxCanvas), ModelObject::Draw (0x469ac0) and
// WobbleObject::Draw (0x469b20) -- they draw (mrModelDraw): the graphics stage; the four deleting destructors
// (0x469b40, 0x469b60, 0x469b80, 0x469ba0).
//
// Threads (docs/PORTING.md): everything here runs on the MAIN thread -- the menus and race set-up (game.obj),
// loading cars and setups (parse_car, SetupCars, the car menus), building the world's objects (parse_obstacle,
// parse_wobble, create_ball...) and, every frame, WorldUpdate -> WorldObject::ProcessPacket and the drawing code's
// IsVisible / GetZ. So every footprint lists the statics its function writes (the game state 0x4e4e3c, the
// live multiplayer info 0x505828, the AI strength 0x509990). Loading or saving files and resources, allocating,
// the menus and the race loops are replay_only.
//
// Layouts (from the code below; offsets checked by static_assert):
//   World (0xcd4; World::World zeroes it, +0 = 3, +4 = 0xcd4): +8 track name (0x20), +0x28 CarList, +0xcac
//     GameOptions (0x28: +0x0c race mode -- GameDoSingle's solo copy sets 4 --, +0x14 laps -- 20 there --,
//     +0x1c gc_type (multi), +0x20 AI strength, +0x25 reversed (byte)).
//   CarList (0xc84): 16 CarListEntry, +0xc80 count.
//   CarListEntry (0xc8): +0 type (0 this machine's player, 1 AI, 2 a network car, 3 a ghost), +4 driver name
//     (OptionsGet caps it at 13), +0x11 car (the resource's base name, "viper"), +0x34 CarSetupData (0x8c),
//     +0xc0 AI driver index (AIGetDriverForCar) or, for the player, ~player_paintjob, +0xc4 hornball (byte).
//   CarSetupData (0x8c): what make_default_setup fills; CarSetup (0xd4) = CarSetupData, +0x8c version (1),
//     +0x90 size (0xd4), +0x94 name ("Default Setup").
//   CarFile (0x228): the CARF resource, version 6 (the offsets CarFileCombine reads are named where it reads them).
//   CarUpgradeSet (UPGS, version 1): +0 count, +4 offsets (fixed up in place to base + offset + 0x404: a 0x404-byte
//     header); CarUpgrade: +0x6c count, +0x70 (offset, value) pairs written into the CarFile.
//   GraphObject (4): vtable 0x4dd358; FrameObject (0x34): + Frame at +4, vtable 0x4dd2d0; WorldObject (0x3c): +0x34
//     packet offset (PhysicsCreate's), +0x38 last tick (0xbad), vtable 0x4dd388; ModelObject (0x40, vtable 0x4dd3b0)
//     and WobbleObject (0x40, vtable 0x4dd3d8): +0x3c the model.
//
// Written from the v1.0 disassembly: every call in the original's order, by address (this file's own functions
// too, so the hooked rewrite or the original is what runs), float moves done with integer instructions kept as bit
// copies, register values as double and stored values as float.
//
// The one fix (FIX in LoadRace): a single race on a track with no AI racing line gets no AI cars.
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

namespace {

template <typename T> static inline T* P(uint32_t addr) { return (T*)(uintptr_t)addr; }
static __forceinline uint32_t U(const void* p, uint32_t off) { uint32_t u; memcpy(&u, (const uint8_t*)p + off, 4); return u; }
static __forceinline int32_t I(const void* p, uint32_t off) { int32_t u; memcpy(&u, (const uint8_t*)p + off, 4); return u; }
static __forceinline float F(const void* p, uint32_t off) { float f; memcpy(&f, (const uint8_t*)p + off, 4); return f; }
static __forceinline void putU(void* p, uint32_t off, uint32_t v) { memcpy((uint8_t*)p + off, &v, 4); }
static __forceinline void putF(void* p, uint32_t off, float v) { memcpy((uint8_t*)p + off, &v, 4); }
// a dword moved with integer instructions (a float's bits, as they are)
static __forceinline void mv(void* d, uint32_t doff, const void* s, uint32_t soff) {
    memcpy((uint8_t*)d + doff, (const uint8_t*)s + soff, 4);
}
// rep movsd: n dwords, forward
static __forceinline void movsd(void* dst, const void* src, uint32_t n) {
    __asm { mov edi, dst
            mov esi, src
            mov ecx, n
            rep movsd }
}
// the inlined strcpy / strcat (repne scasb; rep movsd; rep movsb): the string and its terminator
static __forceinline void s_copy(char* d, const char* s) { memcpy(d, s, strlen(s) + 1); }
static __forceinline void s_cat(char* d, const char* s) { s_copy(d + strlen(d), s); }

// ---- layouts ------------------------------------------------------------------------------------------------------
struct CarListEntry {
    int32_t type;                      // +0x00 0 player, 1 AI, 2 network, 3 ghost
    char driver[13];                   // +0x04
    char car[0x23];                    // +0x11
    uint8_t setup[0x8c];               // +0x34 CarSetupData
    int32_t driver_index;              // +0xc0 AI driver, or ~paintjob for the player
    uint8_t hornball, _c5[3];          // +0xc4
};
static_assert(offsetof(CarListEntry, car) == 0x11 && offsetof(CarListEntry, setup) == 0x34 &&
              offsetof(CarListEntry, driver_index) == 0xc0 && sizeof(CarListEntry) == 0xc8, "CarListEntry");
struct CarList { CarListEntry cars[16]; int32_t count; };
static_assert(sizeof(CarList) == 0xc84, "CarList");
struct GameOptions { uint32_t _00[3]; int32_t mode; uint32_t _10; int32_t laps; uint32_t _18; int32_t gc_type;
                     int32_t strength; uint8_t _24, reversed, _26[2]; };
static_assert(offsetof(GameOptions, strength) == 0x20 && offsetof(GameOptions, reversed) == 0x25 && sizeof(GameOptions) == 0x28, "GameOptions");
struct World { int32_t version, size; char track[0x20]; CarList cars; GameOptions options; };
static_assert(offsetof(World, cars) == 0x28 && offsetof(World, options) == 0xcac && sizeof(World) == 0xcd4, "World");
struct CarSetup { uint8_t data[0x8c]; int32_t version, size; char name[0x40]; };
static_assert(sizeof(CarSetup) == 0xd4, "CarSetup");

// ---- statics and constants ----------------------------------------------------------------------------------------
enum : uint32_t {
    S_GAME_STATE = 0x004e4e3c,       // enum GameState
    S_LIVE_MULTI = 0x00505828,       // LiveMultiInfo* (GameBeginMulti)
    S_CAR_NAME = 0x00505848,         // char[] the single race's car (MenuDoRaceSetup fills it)
    S_AI_STRENGTH = 0x00509990,      // AISetStrength's
    S_SECT_GLOBAL = 0x004db234, S_SECT_MULTI = 0x004db238, S_SECT_GAME = 0x004db23c,   // const char* "GLOBAL" / "MULTI" / "GAME"
    S_CAM_ROT6 = 0x00553340,         // current_camera.rot[6..8] (the view's forward axis)
    S_CAM_POS = 0x0055334c,          // current_camera.pos
    VT_GRAPH = 0x004dd358, VT_FRAME = 0x004dd2d0, VT_WORLDOBJ = 0x004dd388, VT_MODEL = 0x004dd3b0, VT_WOBBLE = 0x004dd3d8,
    VT_CONST_LINE = 0x004db6e8,
};
static const char* const k_car_count = (const char*)0x004e4e78;       // "car_count"
static const char* const k_gc_type = (const char*)0x004e4e84;         // "gc_type"
static const char* const k_bad_prerace = (const char*)0x004e4e8c;     // "Unknown PRERACE option"
static const char* const k_ghost_in_list = (const char*)0x004e4ea4;   // "Ghost cars should never be in carlist at this point."
static const char* const k_bad_car_type = (const char*)0x004e4edc;    // "Unknown carlist:car type : %d"
static const char* const k_ui_res = (const char*)0x004e4efc;          // "ui.res"
static const char* const k_race_res = (const char*)0x004e4f04;        // "race.res"
static const char* const k_fmt_trk = (const char*)0x004e4f10;         // "%s.trk"
static const char* const k_fmt_trk2 = (const char*)0x004e4f18;        // "%s.trk"
static const char* const k_race_res2 = (const char*)0x004e4f20;       // "race.res"
static const char* const k_ui_res2 = (const char*)0x004e4f2c;         // "ui.res"
static const char* const k_player_name = (const char*)0x004e4f34;     // "player_name"
static const char* const k_player_paintjob = (const char*)0x004e4f40; // "player_paintjob"
static const char* const k_viper = (const char*)0x004e4f50;           // "viper"
static const char* const k_fmt_car = (const char*)0x004e4f58;         // "%s.car"
static const uint32_t k_paint_res[8] = {                               // "easy.res" .. "career5.res"
    0x004e4f60, 0x004e4f6c, 0x004e4f78, 0x004e4f84, 0x004e4f90, 0x004e4f9c, 0x004e4fa8, 0x004e4fb4};
static const char* const k_carfile_size = (const char*)0x004f4a04;    // "Wrong carfile size: %d (expecting %d)"
static const uint8_t* const k_ugs = (const uint8_t*)0x004f4a2c;       // ".ugs" (5 bytes moved)
static const char* const k_no_ugs = (const char*)0x004f4a34;          // "Can't load upgrade set %s"
static const char* const k_carfile_version = (const char*)0x004f4a50; // "Wrong carfile version: %x (expecting %x)"
static const char* const k_not_found = (const char*)0x004f4a7c;       // "Couldn't find %s"
static const char* const k_cant_parse = (const char*)0x004f4a90;      // "Couldn't parse carfile \"%s\""
static const char* const k_ugs_version = (const char*)0x004f4aac;     // "Wrong upgrade set version: %x"
static const char* const k_ccs_size = (const char*)0x004f4acc;        // "%s: Mismatched carsetup size! (%d != %d)"
static const char* const k_ccs_version = (const char*)0x004f4af8;     // "%s: Mismatched carsetup version! (%d != %d)"
static const uint8_t* const k_setups_slash = (const uint8_t*)0x004f4b24;   // "setups\\" (8 bytes moved)
static const uint8_t* const k_csu = (const uint8_t*)0x004f4b2c;       // ".csu" (5 bytes moved)
static const char* const k_csu_version = (const char*)0x004f4b34;     // "CarFileLoadSetup: bad version on carsetup, continuing: (%d != %d)"
static const char* const k_csu_size = (const char*)0x004f4b78;        // "Wrong CarSetupSize: %d (expecting %d)"
static const char* const k_csu_open = (const char*)0x004f4ba0;        // "Can't open %s--using default"
static const uint8_t* const k_setups = (const uint8_t*)0x004f4bc0;    // "setups" (7 bytes moved)
static const uint8_t* const k_backslash = (const uint8_t*)0x004f4bc8; // "\\" (2 bytes moved)
static const uint8_t* const k_csu2 = (const uint8_t*)0x004f4bcc;      // ".csu" (5 bytes moved)
static const char* const k_csu_create = (const char*)0x004f4bd4;      // "Can't create setup file \"%s\""
static const uint8_t* const k_ccs1 = (const uint8_t*)0x004f4bf4;      // ".ccs" (5 bytes moved)
static const uint8_t* const k_ccs2 = (const uint8_t*)0x004f4bfc;      // ".ccs" (5 bytes moved)
static const char* const k_default_ccs = (const char*)0x004f4c04;     // "default.ccs"
static const char* const k_using_default = (const char*)0x004f4c10;   // "Can't find %s... using default.ccs"
static const char* const k_using_generic = (const char*)0x004f4c34;   // "Can't find any defaults--using generic"
static const char* const k_using = (const char*)0x004f4c5c;           // "Using %s"
static const char* const k_default_setup = (const char*)0x004f4c68;   // "Default Setup"
static const uint32_t TYPE_CARF = 0x43415246, TYPE_UPGS = 0x55504753, TYPE_CCS0 = 0x43435330;

}  // namespace

// ---- the game's functions these call (by v1.0 address) ------------------------------------------------------------
typedef void(__cdecl* Log_t)(const char*, ...);
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
typedef uint8_t(__cdecl* ResExists_t)(const char*);
typedef void*(__cdecl* ResGet_t)(const char*, uint32_t, uint32_t*, int32_t*, uint8_t*, uint8_t*);
typedef uint8_t(__cdecl* ResForget_t)(void*);
typedef void(__cdecl* ResName_t)(const char*);
typedef char*(__cdecl* strrchr_t)(const char*, int);
typedef int(__cdecl* sprintf_t)(char*, const char*, ...);
typedef const char*(__cdecl* Str_t)(void);
typedef int(__cdecl* FileOpen_t)(const char*);
typedef int(__cdecl* FileSize_t)(int);
typedef uint8_t(__cdecl* FileRead_t)(int, void*, int);
typedef void(__cdecl* FileClose_t)(int*);
typedef uint8_t(__cdecl* FileDir_t)(const char*);
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
typedef void(__cdecl* OptionsGetI_t)(const char*, const char*, int32_t*);
typedef void(__cdecl* OptionsGetS_t)(const char*, const char*, char*, int);
typedef int32_t(__cdecl* IntInt_t)(int32_t);
typedef void(__cdecl* VoidInt_t)(int32_t);
typedef uint8_t(__cdecl* Bool_t)(void);
typedef void(__cdecl* Void_t)(void);
typedef void(__cdecl* NameByCar_t)(char*, int32_t);
typedef uint8_t(__cdecl* MenuRace_t)(char*, char*, void*);
typedef int32_t(__cdecl* PreRace_t)(World*, uint32_t, uint8_t*, uint32_t, const float*, void*);
typedef void(__cdecl* DoRace_t)(World*, void*, void*);
typedef void*(__fastcall* Ctor_t)(void*, Edx);
typedef void(__fastcall* Method_t)(void*, Edx);
typedef uint8_t(__cdecl* MenuMulti_t)(void*);
typedef void*(__cdecl* LiveCreate_t)(void);
typedef uint8_t(__cdecl* Scheduler_t)(void*, void*);
typedef uint8_t(__cdecl* Sync_t)(void*, CarList*, char*);
typedef void(__cdecl* LiveBegin_t)(void*);
typedef void(__cdecl* Delete_t)(void*);
typedef void(__cdecl* CarSetupData_t)(void*, const char*, const char*);
typedef uint8_t(__cdecl* LoadSetupData_t)(void*, const char*, const char*);
typedef void(__cdecl* AICarSetup_t)(void*, const char*, int32_t, World*);
typedef void(__cdecl* WorldFn_t)(World*);
typedef uint8_t(__cdecl* WorldB_t)(World*);
typedef void(__cdecl* GenList_t)(World*, const char*, int32_t);
typedef void(__cdecl* ApplyRes_t)(ResName_t, const char*);
typedef const char*(__cdecl* Paint_t)(int32_t);
typedef uint8_t(__cdecl* LoadCarFile_t)(void*, const char*, uint8_t*);
typedef void(__cdecl* ApplyUpgrade_t)(void*, const void*);
typedef void*(__cdecl* GetUpgradeSet_t)(const char*);
typedef void(__cdecl* ForgetUpgradeSet_t)(void*);
typedef void(__cdecl* Combine_t)(void*, const void*, const void*);
typedef uint8_t(__cdecl* LoadSetupRes_t)(void*, const char*);
typedef uint8_t(__cdecl* LoadSetup_t)(CarSetup*, const char*, const char*);
typedef void(__cdecl* LoadDefaultSetup_t)(CarSetup*, const char*, const char*);
typedef void(__cdecl* MakeDefault_t)(void*);
typedef void(__cdecl* PhysCreate_t)(void*, int32_t*);
typedef int32_t(__cdecl* ModelLoad_t)(const char*);
typedef void(__cdecl* ModelUnload_t)(int32_t);
typedef int32_t(__cdecl* DynoModel_t)(int32_t);
typedef void(__fastcall* ThisPtr_t)(void*, Edx, const void*);
typedef int(__cdecl* Iterate_t)(char*, uint8_t*, int, int, int, int, const char*);
typedef int32_t(__cdecl* TrackNumber_t)(const char*, uint32_t);

static const Log_t LogReport = (Log_t)0x00411150;
static const Log_t LogPanic = (Log_t)0x004112b0;
static const AssertMsg_t ASSERT_MSG = (AssertMsg_t)0x00464950;          // a bare ret: kept, as the original calls it
static const ResExists_t ResourceExists = (ResExists_t)0x00419d10;
static const ResGet_t ResourceGet = (ResGet_t)0x00419fa0;
static const ResGet_t ResourceTry = (ResGet_t)0x00419ce0;
static const ResForget_t ResourceForget = (ResForget_t)0x0041a450;
static const ResName_t ResourceSetMustLoad = (ResName_t)0x00419a30;
static const ResName_t ResourceSetUnload = (ResName_t)0x00419bb0;
static const strrchr_t game_strrchr = (strrchr_t)0x004cf130;           // the game's CRT
static const sprintf_t game_sprintf = (sprintf_t)0x004cf0a0;
static const Str_t Win32GetUserDirectory = (Str_t)0x00412cc0;
static const FileOpen_t FileOpen = (FileOpen_t)0x00411780;
static const FileSize_t FileSize = (FileSize_t)0x00411890;
static const FileRead_t FileReadExact = (FileRead_t)0x004118b0;
static const FileClose_t FileClose = (FileClose_t)0x00411850;
static const FileDir_t FileCreateDirectory = (FileDir_t)0x00411d00;
static const FileOpen_t FileCreate = (FileOpen_t)0x004115f0;
static const FileWrite_t FileWrite = (FileWrite_t)0x00411a30;
static const OptionsGetI_t OptionsGetI = (OptionsGetI_t)0x004713e0;
static const OptionsGetS_t OptionsGetS = (OptionsGetS_t)0x00471500;
static const IntInt_t AIGetCarForDriver = (IntInt_t)0x00420d00;
static const IntInt_t AIGetDriverForCar = (IntInt_t)0x00420ce0;
static const VoidInt_t AISetStrength = (VoidInt_t)0x0041d320;
static const NameByCar_t AIGetDriverNameByCar = (NameByCar_t)0x00420d40;
static const Bool_t HackHornBall = (Bool_t)0x0040d540;
static const Bool_t MultiEnabled = (Bool_t)0x004a23c0;
static const MenuRace_t MenuDoRaceSetup = (MenuRace_t)0x004871e0;
static const PreRace_t PreRaceDo = (PreRace_t)0x0040f140;
static const DoRace_t DoRace = (DoRace_t)0x00401e10;
static const Ctor_t World_ctor = (Ctor_t)0x00462a90;
static const Bool_t LineBegin = (Bool_t)0x004a1420;
static const Void_t LineEnd = (Void_t)0x004a1600;
static const Ctor_t MultiGenesisInfo_ctor = (Ctor_t)0x0048f9f0;
static const MenuMulti_t MenuMultiChooseTransport = (MenuMulti_t)0x0048f060;
static const LiveCreate_t LiveMultiInfo_Create = (LiveCreate_t)0x004a2b90;
static const Method_t LiveMultiInfo_dtor = (Method_t)0x004a2c40;
static const Scheduler_t MenuMultiScheduler = (Scheduler_t)0x0048be90;
static const Void_t MultiResumeChat = (Void_t)0x004a2840;
static const Void_t MultiLiveEnd = (Void_t)0x004a2360;
static const Sync_t MultiSynchronize = (Sync_t)0x004a2850;
static const LiveBegin_t MultiLiveBegin = (LiveBegin_t)0x004a2370;
static const Delete_t op_delete = (Delete_t)0x00414390;
static const Void_t SplashLoading = (Void_t)0x0040cbc0;
static const AICarSetup_t AICarSetup = (AICarSetup_t)0x0041c2f0;
static const PhysCreate_t PhysicsCreate = (PhysCreate_t)0x0042bc10;
static const Bool_t PhysicsIsPaused = (Bool_t)0x0042bd20;
static const ModelLoad_t mrModelLoad = (ModelLoad_t)0x004558c0;
static const ModelUnload_t mrModelUnload = (ModelUnload_t)0x00455950;
static const DynoModel_t GrafLookupDynoModel = (DynoModel_t)0x0046dc50;
// the fix's: how an AI car finds its line (hook/phys_ai.cpp AICarInfo::init)
static const TrackNumber_t AIGetTrackNumber_name = (TrackNumber_t)0x00420dc0;
static const Iterate_t IterateILIname = (Iterate_t)0x0041c1b0;
static const Ctor_t ConstIdealLine_ctor = (Ctor_t)0x00422370;
static const Method_t ConstIdealLine_dtor = (Method_t)0x00422390;
// this file's own functions, called by address so the hooked rewrite (or the original) is what runs
static const WorldB_t GameBeginSingleA = (WorldB_t)0x0040a3f0;
static const WorldFn_t GameEndSingleA = (WorldFn_t)0x0040a490;
static const WorldB_t GameBeginMultiA = (WorldB_t)0x0040a4a0;
static const WorldFn_t GameEndMultiA = (WorldFn_t)0x0040a670;
static const WorldFn_t LoadRaceA = (WorldFn_t)0x0040a840;
static const WorldFn_t UnloadRaceA = (WorldFn_t)0x0040a870;
static const WorldFn_t SetupCarsA = (WorldFn_t)0x0040a890;
static const ResName_t LoadCarA = (ResName_t)0x0040a940;
static const ResName_t UnloadCarA = (ResName_t)0x0040a960;
static const WorldFn_t LoadCarsA = (WorldFn_t)0x0040a980;
static const WorldFn_t UnloadCarsA = (WorldFn_t)0x0040a9d0;
static const WorldFn_t LoadTrackA = (WorldFn_t)0x0040aa20;
static const WorldFn_t UnloadTrackA = (WorldFn_t)0x0040aa70;
static const GenList_t GenerateCarListA = (GenList_t)0x0040aac0;
static const ApplyRes_t apply_to_car_resourceA = (ApplyRes_t)0x0040ac10;
static const Paint_t get_paint_resourceA = (Paint_t)0x0040ac40;
static const LoadCarFile_t CarFileLoad_fileA = (LoadCarFile_t)0x004647d0;
static const ApplyUpgrade_t apply_upgradeA = (ApplyUpgrade_t)0x00464960;
static const Combine_t CarFileCombineA = (Combine_t)0x00464990;
static const GetUpgradeSet_t CarFileGetUpgradeSetA = (GetUpgradeSet_t)0x004651f0;
static const ForgetUpgradeSet_t CarFileForgetUpgradeSetA = (ForgetUpgradeSet_t)0x00465270;
static const LoadSetupRes_t CarFileLoadSetupResA = (LoadSetupRes_t)0x00465280;
static const LoadSetup_t CarFileLoadSetupA = (LoadSetup_t)0x00465320;
static const LoadSetupData_t CarFileLoadSetupDataA = (LoadSetupData_t)0x004654b0;
static const LoadDefaultSetup_t CarFileLoadDefaultSetupA = (LoadDefaultSetup_t)0x00465630;
static const MakeDefault_t CarFileMakeDefaultSetupA = (MakeDefault_t)0x00465780;
static const MakeDefault_t make_default_setupA = (MakeDefault_t)0x004657d0;
static const Method_t GraphObject_ctorA = (Method_t)0x00469840;
static const Method_t GraphObject_dtorA = (Method_t)0x00469850;
static const Method_t WorldObject_ctorA = (Method_t)0x00469920;
static const ThisPtr_t WorldObject_CreateA = (ThisPtr_t)0x004699f0;

static void fp_none(Footprint&) {}

// =================================================================================================================
// carfile.obj
// =================================================================================================================

// CarFileLoad(CarFile*, name, upgrades): the CARF resource (version 6) copied whole (0x228 bytes, whatever size the
// resource says: a mismatch is only ASSERT_MSG'd), then, if `upgrades` is given, the upgrade set <name less its
// extension>.ugs and every upgrade j with upgrades[j] != 0 applied.
static uint8_t __cdecl CarFileLoad_file(uint8_t* cf, const char* name, uint8_t* upgrades) {
    int32_t size;                                            // [L+0]
    uint32_t version;                                        // [L+4]
    // [L+0xc] the original's 32-byte name buffer (a name past 27 characters overruns its frame: not reproduced)
    char buf[0x120];
    uint8_t ok = 0;
    if (!ResourceExists(name)) {
        LogReport(k_not_found, name);
        return ok;
    }
    void* res = ResourceGet(name, TYPE_CARF, &version, &size, 0, 0);
    if (version != 6) {
        LogReport(k_carfile_version, version, 6);
    } else {
        ASSERT_MSG(size == 0x228, k_carfile_size, size, 0x228);
        movsd(cf, res, 0x8a);
        if (upgrades) {
            s_copy(buf, name);
            char* dot = game_strrchr(buf, '.');
            *(volatile char*)dot = 0;                        // no '.': a write to address 0, as the original
            memcpy(buf + strlen(buf), k_ugs, 5);             // ".ugs"
            uint8_t* set = (uint8_t*)CarFileGetUpgradeSetA(buf);
            if (set) {
                for (int32_t j = 0; I(set, 0) > j; j++)
                    if (upgrades[j]) apply_upgradeA(cf, (const void*)(uintptr_t)U(set, 4 + 4 * (uint32_t)j));
                CarFileForgetUpgradeSetA(set);
            } else {
                LogPanic(k_no_ugs, buf);
            }
        }
        ok = 1;
    }
    ResourceForget(res);
    return ok;
}
static void fp_carfile_load_file(Footprint& f, uint8_t*, const char*, uint8_t*) { f.replay_only = "loads the car file and its upgrade set"; }
PORT_FN(0x004647d0, "CarFileLoad(CarFile)", CarFileLoad_file, fp_carfile_load_file)

// apply_upgrade: each of the upgrade's (offset, value) pairs is a dword written into the car file
static void __cdecl apply_upgrade(uint8_t* cf, const uint8_t* upg) {
    for (int32_t j = 0; I(upg, 0x6c) > j; j++) {
        uint32_t off = U(upg, 0x70 + 8 * (uint32_t)j), v = U(upg, 0x74 + 8 * (uint32_t)j);
        memcpy(cf + off, &v, 4);
    }
}
static void fp_apply_upgrade(Footprint& f, uint8_t* cf, const uint8_t* upg) {
    int32_t n = I(upg, 0x6c);
    if (n > 256) { f.replay_only = "a huge upgrade"; return; }
    for (int32_t j = 0; j < n; j++) f.add(cf + U(upg, 0x70 + 8 * (uint32_t)j), 4, "car file (upgraded field)");
}
PORT_FN(0x00464960, "apply_upgrade", apply_upgrade, fp_apply_upgrade)

// CarFileCombine: the car file's ranges interpolated by the setup's sliders into the CarData. FLT_EPSILON stays
// in the x87 stack throughout: "set" means |x| > FLT_EPSILON (a NaN isn't).
static void __cdecl CarFileCombine(uint8_t* out, const uint8_t* s, const uint8_t* cf) {
    const double eps = D(F(P<uint8_t>(0x004dd100), 0));   // 0x34000000
    // lerp(lo, hi, t) as the original: (hi - lo) * t + lo, stored
    auto lerp = [](const uint8_t* a, uint32_t hi, uint32_t lo, const uint8_t* b, uint32_t t) -> float {
        return (float)((D(F(a, hi)) - F(a, lo)) * F(b, t) + F(a, lo));
    };
    putU(out, 4, 0x1e0);
    mv(out, 0x08, cf, 0x00); mv(out, 0x0c, cf, 0x04); mv(out, 0x10, cf, 0x08); mv(out, 0x14, cf, 0x0c);
    mv(out, 0x48, cf, 0x10); mv(out, 0x4c, cf, 0x14); mv(out, 0x50, cf, 0x18); mv(out, 0x54, cf, 0x1c);
    mv(out, 0x58, cf, 0x30); mv(out, 0x5c, cf, 0x34);
    putF(out, 0x60, lerp(cf, 0x24, 0x20, s, 0x40));
    putF(out, 0x64, lerp(cf, 0x2c, 0x28, s, 0x44));
    mv(out, 0x68, cf, 0x40); mv(out, 0x6c, cf, 0x38); mv(out, 0x70, cf, 0x44); mv(out, 0x74, cf, 0x3c);
    // six factors: each the car file's if set, else 1
    float sel[6];
    for (uint32_t k = 0; k < 6; k++) {
        if (fabs(D(F(cf, 0x60 + 4 * k))) > eps) memcpy(&sel[k], cf + 0x60 + 4 * k, 4);
        else putU(&sel[k], 0, 0x3f800000);
    }
    double sum = D(F(cf, 0x9c));
    sum += F(cf, 0x8c); sum += F(cf, 0x7c); sum += F(cf, 0xa0); sum += F(cf, 0x90); sum += F(cf, 0x80);
    sum += F(cf, 0xa4); sum += F(cf, 0x94); sum += F(cf, 0x84); sum += F(cf, 0x98); sum += F(cf, 0x88);
    sum += F(cf, 0x78);
    const double q = sum / F(out, 0x6c) + D(F(P<uint8_t>(0x004dd104), 0));   // + 1.0f
    double p = D(F(s, 4));
    p *= sel[0]; p *= sel[1]; p *= sel[2]; p *= sel[3]; p *= sel[4]; p *= sel[5];
    const double m = q * p;                                  // fmulp st(1)
    putF(out, 0x68, (float)(D(F(out, 0x68)) * m));
    putF(out, 0x6c, (float)(m * F(out, 0x6c)));
    {
        double w = D(F(cf, 0xac));
        w += F(cf, 0xc0); w += F(cf, 0xc4); w += F(cf, 0xb4); w += F(cf, 0xb8); w += F(cf, 0xa8); w += F(cf, 0xbc);
        w += F(cf, 0xb0); w += F(out, 0x08);
        putF(out, 0x08, (float)w);
    }
    mv(out, 0x78, cf, 0x48); mv(out, 0x7c, cf, 0x4c); mv(out, 0x88, cf, 0x58);
    putF(out, 0x8c, (float)(D(F(cf, 0x5c)) * F(s, 0x74)));
    putF(out, 0x90, lerp(cf, 0xcc, 0xc8, s, 0x00));
    // the gear ratios (+ final drive..): the car file's if its first is set, else the setup's
    if (!(fabs(D(F(cf, 0xe8))) > eps)) {
        mv(out, 0x94, s, 0x50); mv(out, 0x98, s, 0x54); mv(out, 0x9c, s, 0x58); mv(out, 0xa0, s, 0x5c);
        mv(out, 0xa4, s, 0x60); mv(out, 0xa8, s, 0x64); mv(out, 0xac, s, 0x68); mv(out, 0xb0, s, 0x6c);
    } else {
        mv(out, 0x94, cf, 0xe8); mv(out, 0x98, cf, 0xec); mv(out, 0x9c, cf, 0xf0); mv(out, 0xa0, cf, 0xf4);
        mv(out, 0xa4, cf, 0xf8); mv(out, 0xa8, cf, 0xfc); mv(out, 0xac, cf, 0x100); mv(out, 0xb0, cf, 0x104);
    }
    if (I(cf, 0x108) < 7) putU(out, 0xac, 0);                // the gear count: the gears past it cleared
    if (I(cf, 0x108) < 6) putU(out, 0xa8, 0);
    if (I(cf, 0x108) < 5) putU(out, 0xa4, 0);
    mv(out, 0xb4, cf, 0xd0); mv(out, 0xb8, cf, 0xd4); mv(out, 0xbc, cf, 0xd8); mv(out, 0xc0, cf, 0xdc);
    putF(out, 0xcc, lerp(cf, 0x13c, 0x10c, s, 0x38));
    putF(out, 0xd0, lerp(cf, 0x140, 0x110, s, 0x3c));
    putF(out, 0xd4, lerp(cf, 0x144, 0x114, s, 0x10));
    putF(out, 0xd8, lerp(cf, 0x148, 0x118, s, 0x18));
    putF(out, 0xdc, lerp(cf, 0x14c, 0x11c, s, 0x20));
    putF(out, 0xe0, lerp(cf, 0x150, 0x120, s, 0x28));
    putF(out, 0xe4, lerp(cf, 0x154, 0x124, s, 0x30));
    putF(out, 0xe8, lerp(cf, 0x158, 0x128, s, 0x34));
    putF(out, 0xec, lerp(cf, 0x15c, 0x12c, s, 0x78));
    putF(out, 0xf0, lerp(cf, 0x160, 0x130, s, 0x7c));
    putF(out, 0xf4, lerp(cf, 0x164, 0x134, s, 0x80));
    putF(out, 0xf8, lerp(cf, 0x168, 0x138, s, 0x84));
    mv(out, 0xfc, cf, 0x16c); mv(out, 0x84, cf, 0x54); mv(out, 0x80, cf, 0x50); mv(out, 0xc8, cf, 0xe4);
    mv(out, 0xc4, cf, 0xe0);
    for (uint32_t k = 0; k < 11; k++) mv(out, 0x100 + 4 * k, cf, 0x170 + 4 * k);
    putF(out, 0x12c, (float)(D(F(cf, 0x19c)) * F(s, 0x88)));
    mv(out, 0x130, cf, 0x1a0); mv(out, 0x134, cf, 0x1a4); mv(out, 0x138, cf, 0x1a8); mv(out, 0x13c, cf, 0x1ac);
    mv(out, 0x140, cf, 0x1b0); mv(out, 0x144, cf, 0x1b4); mv(out, 0x150, cf, 0x1b8); mv(out, 0x154, cf, 0x1bc);
    mv(out, 0x158, cf, 0x1c0);
    {
        uint32_t v1c4 = U(cf, 0x1c4);
        putU(out, 0x160, 0);
        putU(out, 0x15c, v1c4);
    }
    putF(out, 0x148, lerp(cf, 0x1c8, 0x1d0, s, 0x08));
    putF(out, 0x14c, lerp(cf, 0x1d0, 0x1cc, s, 0x08));
    for (uint32_t k = 0; k < 8; k++) mv(out, 0x164 + 4 * k, cf, 0x1d8 + 4 * k);
    // the lift pair: interpolated, or one of the car file's two presets when the setup picks it (+0x70: 1 or 2)
    float a = lerp(cf, 0x208, 0x200, s, 0x48);
    float b = lerp(cf, 0x20c, 0x204, s, 0x4c);
    if (fabs(D(F(cf, 0x218))) > eps && I(s, 0x70) == 1) {
        memcpy(&a, cf + 0x218, 4);
        memcpy(&b, cf + 0x21c, 4);
    } else if (fabs(D(F(cf, 0x220))) > eps && I(s, 0x70) == 2) {
        memcpy(&a, cf + 0x220, 4);
        memcpy(&b, cf + 0x224, 4);
    }
    putF(out, 0x184, (float)(D(F(cf, 0x1f8)) + a));
    putF(out, 0x188, (float)(D(F(cf, 0x1fc)) + b));
    putF(out, 0x18c, (float)(fabs(D(a)) * F(cf, 0x210)));
    putF(out, 0x190, (float)(fabs(D(b)) * F(cf, 0x214)));
}
static void fp_combine(Footprint& f, uint8_t* out, const uint8_t*, const uint8_t*) { f.add(out + 4, 0x190, "CarData"); }
PORT_FN(0x00464990, "CarFileCombine", CarFileCombine, fp_combine)

// CarFileLoad(CarData*, setup, name, upgrades): the car file, then its name (less the extension, from the last
// backslash on -- the backslash included) into the CarData, and the two combined
static uint8_t __cdecl CarFileLoad_data(uint8_t* out, const uint8_t* setup, const char* name, uint8_t* upgrades) {
    char buf[0x104];                                          // [L+0]
    uint8_t cf[0x228];                                        // [L+0x104] the CarFile
    if (!CarFileLoad_fileA(cf, name, upgrades)) {
        LogReport(k_cant_parse, name);
        return 0;
    }
    s_copy(buf, name);
    char* dot = game_strrchr(buf, '.');
    if (dot) *dot = 0;
    const char* base = game_strrchr(buf, '\\');
    if (!base) base = buf;
    s_copy((char*)out + 0x198, base);
    CarFileCombineA(out, setup, cf);
    return 1;
}
static void fp_carfile_load_data(Footprint& f, uint8_t*, const uint8_t*, const char*, uint8_t*) { f.replay_only = "loads the car file and its upgrade set"; }
PORT_FN(0x00465100, "CarFileLoad(CarData)", CarFileLoad_data, fp_carfile_load_data)

// CarFileGetUpgradeSet: the UPGS resource (version 1, else a panic); a fresh copy's offsets fixed up to pointers
static uint8_t* __cdecl CarFileGetUpgradeSet(const char* name) {
    uint8_t fix;                                              // [L+3]
    uint32_t version;                                         // [L+4]
    int32_t size;                                             // [L+8]
    uint8_t* set = (uint8_t*)ResourceTry(name, TYPE_UPGS, &version, &size, 0, &fix);
    if (!set) return 0;
    if (version != 1) LogPanic(k_ugs_version, version);
    if (fix)
        for (int32_t i = 0; I(set, 0) > i; i++) putU(set, 4 + 4 * (uint32_t)i, U(set, 4 + 4 * (uint32_t)i) + (uint32_t)(uintptr_t)set + 0x404);
    return set;
}
static void fp_get_upgrade_set(Footprint& f, const char*) { f.replay_only = "loads the upgrade set"; }
PORT_FN(0x004651f0, "CarFileGetUpgradeSet", CarFileGetUpgradeSet, fp_get_upgrade_set)

static void __cdecl CarFileForgetUpgradeSet(void* set) { ResourceForget(set); }
static void fp_forget_upgrade_set(Footprint& f, void*) { f.replay_only = "frees the upgrade set"; }
PORT_FN(0x00465270, "CarFileForgetUpgradeSet", CarFileForgetUpgradeSet, fp_forget_upgrade_set)

// CarFileLoadSetupRes: a CCS0 resource (version 2, 0x8c bytes) into the CarSetupData; anything else is logged and
// the generic setup filled in
static uint8_t __cdecl CarFileLoadSetupRes(uint8_t* out, const char* name) {
    int32_t size;                                             // [L+0]
    uint32_t version;                                         // [L+4]
    uint8_t ok = 0;
    void* res = ResourceTry(name, TYPE_CCS0, &version, &size, 0, 0);
    if (res) {
        if (version == 2) {
            if (size == 0x8c) {
                ok = 1;
                movsd(out, res, 0x23);
            } else {
                LogReport(k_ccs_size, name, size, 0x8c);
            }
        } else {
            LogReport(k_ccs_version, name, version, 2);
        }
        ResourceForget(res);
    }
    if (!ok) make_default_setupA(out);
    return ok;
}
static void fp_load_setup_res(Footprint& f, uint8_t*, const char*) { f.replay_only = "loads a setup resource"; }
PORT_FN(0x00465280, "CarFileLoadSetupRes", CarFileLoadSetupRes, fp_load_setup_res)

// CarFileLoadSetup: <user>\setups\<track>.csu (a whole CarSetup, 0xd4 bytes), else the defaults. The car name is
// only passed on to CarFileLoadDefaultSetup.
static uint8_t __cdecl CarFileLoadSetup(CarSetup* setup, const char* car, const char* track) {
    int fd;                                                   // [L+0]
    char path[0x300];                                         // [L+4] (the original's 256: longer overruns its frame)
    uint8_t ok = 0;
    s_copy(path, Win32GetUserDirectory());
    memcpy(path + strlen(path), k_setups_slash, 8);          // "setups\\"
    s_cat(path, track);
    memcpy(path + strlen(path), k_csu, 5);                   // ".csu"
    fd = FileOpen(path);
    if (fd) {
        if (FileSize(fd) == 0xd4) {
            FileReadExact(fd, setup, 0xd4);
            if (setup->version != 1) LogReport(k_csu_version, setup->version, 1);
            ok = 1;
        } else {
            int n = FileSize(fd);
            LogReport(k_csu_size, n, 0xd4);
            CarFileLoadDefaultSetupA(setup, car, track);
        }
        FileClose(&fd);
    } else {
        LogReport(k_csu_open, path);
        CarFileLoadDefaultSetupA(setup, car, track);
    }
    return ok;
}
static void fp_load_setup(Footprint& f, CarSetup*, const char*, const char*) { f.replay_only = "reads the setup file"; }
PORT_FN(0x00465320, "CarFileLoadSetup", CarFileLoadSetup, fp_load_setup)

// CarFileLoadSetupData: CarFileLoadSetup into a CarSetup on the stack, and its CarSetupData copied out (whatever
// the load did: a short read leaves the stack's own bytes)
static uint8_t __cdecl CarFileLoadSetupData(uint8_t* out, const char* car, const char* track) {
    CarSetup setup;
    uint8_t r = CarFileLoadSetupA(&setup, car, track);
    movsd(out, &setup, 0x23);
    return r;
}
static void fp_load_setup_data(Footprint& f, uint8_t*, const char*, const char*) { f.replay_only = "reads the setup file"; }
PORT_FN(0x004654b0, "CarFileLoadSetupData", CarFileLoadSetupData, fp_load_setup_data)

// CarFileSaveSetup: <user>\setups (created), then <user>\setups\<track>.csu written whole; the setup's version and
// size stamped first (in the caller's const setup)
static uint8_t __cdecl CarFileSaveSetup(CarSetup* setup, const char* car, const char* track) {
    int fd;                                                   // [L+0]
    char path[0x300];                                         // [L+4] (the original's 256)
    (void)car;
    s_copy(path, Win32GetUserDirectory());
    memcpy(path + strlen(path), k_setups, 7);                // "setups"
    FileCreateDirectory(path);
    memcpy(path + strlen(path), k_backslash, 2);             // "\\"
    s_cat(path, track);
    memcpy(path + strlen(path), k_csu2, 5);                  // ".csu"
    setup->version = 1;
    setup->size = 0xd4;
    uint8_t ok = 0;
    fd = FileCreate(path);
    if (fd) {
        FileWrite(fd, setup, 0xd4);
        ok = 1;
        FileClose(&fd);
    } else {
        LogReport(k_csu_create, path);
    }
    return ok;
}
static void fp_save_setup(Footprint& f, CarSetup*, const char*, const char*) { f.replay_only = "writes the setup file"; }
PORT_FN(0x004654f0, "CarFileSaveSetup", CarFileSaveSetup, fp_save_setup)

// CarFileLoadDefaultSetup: <track>.ccs, else <car>.ccs, else default.ccs, else the generic setup
static void __cdecl CarFileLoadDefaultSetup(CarSetup* setup, const char* car, const char* track) {
    char by_track[0x100], by_car[0x100];                      // [L+0], [L+0x100] (longer names overrun the frame)
    s_copy(by_track, track);
    memcpy(by_track + strlen(by_track), k_ccs1, 5);          // ".ccs"
    s_copy(by_car, car);
    memcpy(by_car + strlen(by_car), k_ccs2, 5);
    setup->version = 1;
    setup->size = 0xd4;
    setup->name[0] = 0;
    if (CarFileLoadSetupResA(setup, by_track)) return;
    if (CarFileLoadSetupResA(setup, by_car)) {
        LogReport(k_using, by_car);
        return;
    }
    if (CarFileLoadSetupResA(setup, k_default_ccs)) {
        LogReport(k_using_default, by_track);
        return;
    }
    LogReport(k_using_generic);
    CarFileMakeDefaultSetupA(setup);
}
static void fp_load_default_setup(Footprint& f, CarSetup*, const char*, const char*) { f.replay_only = "loads setup resources"; }
PORT_FN(0x00465630, "CarFileLoadDefaultSetup", CarFileLoadDefaultSetup, fp_load_default_setup)

static void __cdecl CarFileMakeDefaultSetup(CarSetup* setup) {
    setup->version = 1;
    setup->size = 0xd4;
    s_copy(setup->name, k_default_setup);
    make_default_setupA(setup);
}
static void fp_make_default_setup(Footprint& f, CarSetup* setup) { f.add(setup, sizeof(CarSetup), "CarSetup"); }
PORT_FN(0x00465780, "CarFileMakeDefaultSetup", CarFileMakeDefaultSetup, fp_make_default_setup)

// make_default_setup: the generic setup (+0x14, +0x1c, +0x24, +0x2c are left as they are)
static void __cdecl make_default_setup(uint8_t* d) {
    static const struct { uint16_t off; uint32_t v; } k[] = {
        {0x00, 0x3f333333}, {0x50, 0x402a3d71}, {0x5c, 0x3f800000}, {0x64, 0x3f000000}, {0x68, 0}, {0x6c, 0x402a3d71},
        {0x0c, 0x3f000000}, {0x54, 0x3fe3d70a}, {0x58, 0x3fa66666}, {0x4c, 0x3f000000}, {0x04, 0x3f800000},
        {0x60, 0x3f3d70a4}, {0x74, 0x3f800000}, {0x18, 0x3f000000}, {0x48, 0x3e4ccccd}, {0x08, 0x3ecccccd},
        {0x10, 0x3f333333}, {0x20, 0x3f666666}, {0x28, 0x3f333333}, {0x3c, 0x3f000000}, {0x78, 0x3f000000},
        {0x7c, 0x3f000000}, {0x80, 0x3f000000}, {0x84, 0x3f000000}, {0x70, 0}, {0x40, 0}, {0x44, 0},
        {0x88, 0x3f800000}, {0x38, 0x3f333333}, {0x30, 0x3f4ccccd}, {0x34, 0x3dcccccd}};
    for (const auto& e : k) putU(d, e.off, e.v);
}
static void fp_make_default(Footprint& f, uint8_t* d) { f.add(d, 0x8c, "CarSetupData"); }
PORT_FN(0x004657d0, "make_default_setup", make_default_setup, fp_make_default)

// =================================================================================================================
// wob.obj
// =================================================================================================================

static void* __fastcall GraphObject_ctor(void* self, Edx) {
    putU(self, 0, VT_GRAPH);
    return self;
}
static void fp_graph_ctor(Footprint& f, void* self, Edx) { f.add(self, 4, "GraphObject (constructing)"); }
PORT_FN(0x00469840, "GraphObject::GraphObject", GraphObject_ctor, fp_graph_ctor)

static void __fastcall GraphObject_dtor(void* self, Edx) { putU(self, 0, VT_GRAPH); }
PORT_FN(0x00469850, "GraphObject::~GraphObject", GraphObject_dtor, fp_graph_ctor)

static uint8_t __fastcall GraphObject_IsAlpha(void*, Edx) { return 0; }
static void fp_const(Footprint&, void*, Edx) {}
PORT_FN(0x00469870, "GraphObject::IsAlpha", GraphObject_IsAlpha, fp_const)

static int32_t __fastcall GraphObject_Order(void*, Edx) { return 0; }
PORT_FN(0x00469890, "GraphObject::Order", GraphObject_Order, fp_const)

// FrameObject::IsVisible: in front of a plane 8 behind the camera, along its forward axis (a NaN isn't)
static uint8_t __fastcall FrameObject_IsVisible(const uint8_t* self, Edx) {
    const float* fw = P<float>(S_CAM_ROT6);
    const float* cp = P<float>(S_CAM_POS);
    double z = (D(F(self, 0x28)) - cp[0]) * fw[0];
    z += (D(F(self, 0x2c)) - cp[1]) * fw[1];
    z += (D(F(self, 0x30)) - cp[2]) * fw[2];
    return z > D(F(P<uint8_t>(0x004dd380), 0));             // -8.0f (fcomp; test ah,0x41; sete)
}
static void fp_frame_const(Footprint&, const uint8_t*, Edx) {}
PORT_FN(0x004698a0, "FrameObject::IsVisible", FrameObject_IsVisible, fp_frame_const)

// FrameObject::GetZ: the distance along the camera's forward axis, unrounded (ST0): y and z first, then x
static double __fastcall FrameObject_GetZ(const uint8_t* self, Edx) {
    const float* fw = P<float>(S_CAM_ROT6);
    const float* cp = P<float>(S_CAM_POS);
    double z = (D(F(self, 0x2c)) - cp[1]) * fw[1];
    z += (D(F(self, 0x30)) - cp[2]) * fw[2];
    z += (D(F(self, 0x28)) - cp[0]) * fw[0];
    return z;
}
PORT_FN(0x004698e0, "FrameObject::GetZ", FrameObject_GetZ, fp_frame_const)

// WorldObject::WorldObject: GraphObject's, then FrameObject's inlined identity frame (written twice, as the
// original), the tick 0xbad, the two vtables in turn. +0x34 is left as it is.
static void* __fastcall WorldObject_ctor(uint8_t* self, Edx) {
    GraphObject_ctorA(self, 0);
    for (int pass = 0; pass < 2; pass++) {
        putU(self, 0x04, 0x3f800000); putU(self, 0x08, 0); putU(self, 0x0c, 0); putU(self, 0x10, 0);
        putU(self, 0x14, 0x3f800000); putU(self, 0x18, 0); putU(self, 0x1c, 0); putU(self, 0x20, 0);
        putU(self, 0x24, 0x3f800000);
        if (pass == 0) { putU(self, 0x28, 0); putU(self, 0x2c, 0); putU(self, 0x30, 0); putU(self, 0x38, 0xbad); }
    }
    putU(self, 0, VT_FRAME);
    putU(self, 0x28, 0); putU(self, 0x2c, 0); putU(self, 0x30, 0);
    putU(self, 0, VT_WORLDOBJ);
    return self;
}
static void fp_worldobj_ctor(Footprint& f, uint8_t* self, Edx) {
    f.add(self, 0x34, "WorldObject (constructing)");
    f.add(self + 0x38, 4, "WorldObject tick");
}
PORT_FN(0x00469920, "WorldObject::WorldObject", WorldObject_ctor, fp_worldobj_ctor)

// WorldObject::Create: the physics object, its packet offset into +0x34
static void __fastcall WorldObject_Create(uint8_t* self, Edx, void* data) { PhysicsCreate(data, (int32_t*)(self + 0x34)); }
static void fp_worldobj_create(Footprint& f, uint8_t*, Edx, void*) { f.replay_only = "creates a physics object"; }
PORT_FN(0x004699f0, "WorldObject::Create", WorldObject_Create, fp_worldobj_create)

// WorldObject::ProcessPacket: this object's message in the packet; a new tick (or any tick while paused) is taken
// (UpdateMessage, through the vtable)
static void __fastcall WorldObject_ProcessPacket(uint8_t* self, Edx, const uint8_t* packet) {
    const uint8_t* msg = packet + U(self, 0x34);
    if ((uint32_t)*(const uint16_t*)msg == U(self, 0x38) && !PhysicsIsPaused()) return;
    uint32_t tick = *(const uint16_t*)msg;
    putU(self, 0x38, tick);
    VFN(self, 0x24, void, const uint8_t*)(self, 0, msg);
}
static void fp_worldobj_packet(Footprint& f, uint8_t* self, Edx, const uint8_t*) {
    uint32_t vt = U(self, 0);
    if (vt == VT_WORLDOBJ || vt == VT_MODEL || vt == VT_WOBBLE) f.add(self + 4, 0x38, "WorldObject frame, tick");
    else f.replay_only = "a subclass's UpdateMessage (a car's: carwob.obj) writes beyond the WorldObject";
}
PORT_FN(0x00469a10, "WorldObject::ProcessPacket", WorldObject_ProcessPacket, fp_worldobj_packet)

// WorldObject::UpdateMessage: the message's frame (12 dwords after its tick) into the object's
static void __fastcall WorldObject_UpdateMessage(uint8_t* self, Edx, const uint8_t* msg) { movsd(self + 4, msg + 4, 12); }
static void fp_worldobj_msg(Footprint& f, uint8_t* self, Edx, const uint8_t*) { f.add(self + 4, 0x30, "WorldObject frame"); }
PORT_FN(0x00469a50, "WorldObject::UpdateMessage", WorldObject_UpdateMessage, fp_worldobj_msg)

static void* __fastcall ModelObject_ctor(uint8_t* self, Edx, void* data, const char* model) {
    WorldObject_ctorA(self, 0);
    putU(self, 0, VT_MODEL);
    putU(self, 0x3c, (uint32_t)mrModelLoad(model));
    WorldObject_CreateA(self, 0, data);
    return self;
}
static void fp_model_ctor(Footprint& f, uint8_t*, Edx, void*, const char*) { f.replay_only = "loads a model, creates a physics object"; }
PORT_FN(0x00469a70, "ModelObject::ModelObject", ModelObject_ctor, fp_model_ctor)

static void __fastcall ModelObject_dtor(uint8_t* self, Edx) {
    int32_t model = I(self, 0x3c);
    putU(self, 0, VT_MODEL);
    mrModelUnload(model);
    GraphObject_dtorA(self, 0);
}
static void fp_model_dtor(Footprint& f, uint8_t*, Edx) { f.replay_only = "unloads the model"; }
PORT_FN(0x00469aa0, "ModelObject::~ModelObject", ModelObject_dtor, fp_model_dtor)

static void* __fastcall WobbleObject_ctor(uint8_t* self, Edx, const uint8_t* data) {
    WorldObject_ctorA(self, 0);
    putU(self, 0, VT_WOBBLE);
    putU(self, 0x3c, (uint32_t)GrafLookupDynoModel(I(data, 0x18)));
    WorldObject_CreateA(self, 0, (void*)data);
    return self;
}
static void fp_wobble_ctor(Footprint& f, uint8_t*, Edx, const uint8_t*) { f.replay_only = "creates a physics object"; }
PORT_FN(0x00469ae0, "WobbleObject::WobbleObject", WobbleObject_ctor, fp_wobble_ctor)

// =================================================================================================================
// game.obj
// =================================================================================================================

static int32_t __cdecl GetGameState(void) { return *P<int32_t>(S_GAME_STATE); }
PORT_FN(0x0040a3d0, "GetGameState", GetGameState, fp_none)

static void __cdecl SetGameState(int32_t s) { *P<int32_t>(S_GAME_STATE) = s; }
static void fp_set_game_state(Footprint& f, int32_t) { f.add(P<void>(S_GAME_STATE), 4, "game state"); }
PORT_FN(0x0040a3e0, "SetGameState", SetGameState, fp_set_game_state)

// the fix's: which world, if any, is being loaded for a single race (GameBeginSingle -> LoadRace)
static World* s_single_race_world;

// GameBeginSingle: the race set-up menu (the car into 0x505848, the track, the options), the number of cars
// (GAME car_count), the car list (the player and car_count - 1 AI cars) and the race loaded
static uint8_t __cdecl GameBeginSingle(World* w) {
    int32_t car_count;                                        // [L+0]
    char track[0x100];                                        // [L+4] (the original's 0x40)
    if (!MenuDoRaceSetup(P<char>(S_CAR_NAME), track, &w->options)) return 0;
    const char* section = *P<const char*>(S_SECT_GAME);
    car_count = 0;
    OptionsGetI(section, k_car_count, &car_count);
    car_count--;
    s_copy(w->track, track);
    GenerateCarListA(w, P<char>(S_CAR_NAME), car_count);
    if (VP_FIX) s_single_race_world = w;                     // (the FIX in LoadRace: a single race's world)
    LoadRaceA(w);
    if (VP_FIX) s_single_race_world = 0;
    return 1;
}
static void fp_begin_single(Footprint& f, World*) { f.replay_only = "the race set-up menu; loads the race"; }
PORT_FN(0x0040a3f0, "GameBeginSingle", GameBeginSingle, fp_begin_single)

static void __cdecl GameEndSingle(World* w) { UnloadRaceA(w); }
static void fp_end_race(Footprint& f, World*) { f.replay_only = "unloads the race"; }
PORT_FN(0x0040a490, "GameEndSingle", GameEndSingle, fp_end_race)

// GameBeginMulti: the network line, the transport and the scheduler menus, the car lists synchronised; then the
// options from the session, the gc_type, the AI strength, the AI cars' names, the live session begun, the race
// loaded. Round again while a step asks to be retried.
static uint8_t __cdecl GameBeginMulti(World* w) {
    uint8_t ok, retry;                                        // [L+3], bl
    uint8_t mgi[0x6c];                                        // [L+4] MultiGenesisInfo
    do {
        ok = 0;
        retry = 0;
        if (*P<void*>(S_LIVE_MULTI) == 0) {
            if (!LineBegin()) return 0;
            MultiGenesisInfo_ctor(mgi, 0);
            if (MenuMultiChooseTransport(mgi)) {
                void* live = LiveMultiInfo_Create();
                *P<void*>(S_LIVE_MULTI) = live;
                if (live) {
                    ok = MenuMultiScheduler(*P<void*>(S_LIVE_MULTI), mgi);
                    retry = ok == 0;
                }
            }
        } else {
            uint8_t* live = *P<uint8_t*>(S_LIVE_MULTI);
            uint8_t* session = *(uint8_t**)(live + 8);
            if (I(session, 4) > 4) {
                ok = MenuMultiScheduler(live, 0);
            } else {
                retry = 1;
                MultiResumeChat();
                MultiLiveEnd();
                ok = 0;
            }
        }
        if (ok) {
            ok = MultiSynchronize(*P<void*>(S_LIVE_MULTI), &w->cars, w->track);
            retry = ok == 0;
        }
        if (!ok) {
            if (*P<void*>(S_LIVE_MULTI)) {
                void* live = *P<void*>(S_LIVE_MULTI);
                LiveMultiInfo_dtor(live, 0);
                op_delete(live);
                *P<void*>(S_LIVE_MULTI) = 0;
            }
            LineEnd();
        } else {
            uint8_t* session = *(uint8_t**)(*P<uint8_t*>(S_LIVE_MULTI) + 8);
            const void* opts = VFN(session, 0x64, const void*)(session, 0);
            movsd(&w->options, opts, 10);
            const char* section = *P<const char*>(S_SECT_MULTI);
            w->options.gc_type = 0;
            OptionsGetI(section, k_gc_type, &w->options.gc_type);
            AISetStrength(w->options.strength);
            for (int32_t i = 0; w->cars.count > i; i++) {
                uint8_t* s = *(uint8_t**)(*P<uint8_t*>(S_LIVE_MULTI) + 8);
                if (!VFN(s, 0x84, uint8_t, int32_t)(s, 0, i)) AIGetDriverNameByCar(w->cars.cars[i].driver, i);
            }
            MultiLiveBegin(*P<void*>(S_LIVE_MULTI));
            LoadRaceA(w);
        }
    } while (retry);
    return ok;
}
static void fp_begin_multi(Footprint& f, World*) { f.replay_only = "the network menus and session; loads the race"; }
PORT_FN(0x0040a4a0, "GameBeginMulti", GameBeginMulti, fp_begin_multi)

static void __cdecl GameEndMulti(World* w) { UnloadRaceA(w); }
PORT_FN(0x0040a670, "GameEndMulti", GameEndMulti, fp_end_race)

// GameDoSingle: single races until the set-up menu is left. Each race: the pre-race menu until it says go on
// (0), the race (1), or a solo run (2: a copy of the world with this machine's first player, after the first
// car, moved to the front, alone, 20 laps, mode 4).
static void __cdecl GameDoSingle(void) {
    static_assert(sizeof(World) == 0xcd4, "World");
    World solo;                                               // [S+0x14]
    World w;                                                  // [S+0xce8]
    World_ctor(&w, 0);
    if (!GameBeginSingleA(&w)) return;
    do {
        uint32_t flag = 0;                                    // ebx (only its low byte changes)
        uint8_t done = 0;
        do {
            int32_t r = PreRaceDo(&w, flag, 0, 0, 0, 0);
            if (r == 0) {
                done = 1;
            } else if (r == 1) {
                flag = 0;
                DoRace(&w, 0, 0);
            } else if (r == 2) {
                movsd(&solo, &w, 0x335);
                for (int32_t i = 1; solo.cars.count > i; i++)
                    if (solo.cars.cars[i].type == 0) {
                        movsd(&solo.cars.cars[0], &solo.cars.cars[i], 0x32);
                        break;
                    }
                solo.options.laps = 0x14;
                flag = 1;
                solo.cars.count = 1;
                solo.options.mode = 4;
                DoRace(&solo, 0, 0);
            } else {
                LogPanic(k_bad_prerace);
            }
        } while (!done);
        GameEndSingleA(&w);
    } while (GameBeginSingleA(&w));
}
static void fp_do_single(Footprint& f) { f.replay_only = "the single-player race loop"; }
PORT_FN(0x0040a680, "GameDoSingle", GameDoSingle, fp_do_single)

static void __cdecl GameDoMulti(void) {
    World w;
    World_ctor(&w, 0);
    if (!GameBeginMultiA(&w)) return;
    do {
        DoRace(&w, 0, 0);
        GameEndMultiA(&w);
    } while (GameBeginMultiA(&w));
}
PORT_FN(0x0040a7e0, "GameDoMulti", GameDoMulti, fp_do_single)

// ---- the fix: a single race on a track with no AI racing line ------------------------------------------------------
// An AI car's line is the first of IterateILIname's candidates that ConstIdealLine::load accepts (AICarInfo::init,
// hook/phys_ai.cpp): the driver's learned notes\<dstc>.ilq, its <dstc>.ilg (drivers.res), the track's default.ili
// (rdefault.ili reversed), default.ili driven reversed. With none, AICar's constructor keeps no line and
// AICar::reset reads seginfo[-1] of a NULL table: the game crashes as the race starts. The same happens with a line
// of no segments (AICar allocates 0 SegmentInfos and reads the one before). So each AI car is tried here exactly as
// its AICarInfo will be -- the same candidates, from the same inputs (the car's index, AIGetDriverForCar of it, the
// world's track number and reversed flag through AIGetTrackNumber, the world's AI strength that SetupCars will
// install, the car's name), loaded through a ConstIdealLine of the game's own -- once the track's resources are
// loaded (they hold default.ili) and before anything registers a car.
static bool ai_car_has_line(const World* w, int32_t i) {
    const int32_t driver = AIGetDriverForCar(i);
    const int32_t track = AIGetTrackNumber_name(w->track, w->options.reversed);
    const int32_t strength = w->options.strength;
    const char* car = w->cars.cars[i].car;
    alignas(4) uint8_t line[0x3c];                            // a ConstIdealLine
    ConstIdealLine_ctor(line, 0);
    char name[0x100];
    uint8_t rev;
    bool usable = false;
    for (int k = 0;;) {
        k = IterateILIname(name, &rev, k, driver, track, strength, car);
        if (k == 5) break;
        if (VFN(line, 4, uint8_t, const char*, uint32_t)(line, 0, name, rev)) {   // load: the first that loads is the car's
            usable = I(line, 0x2c) != 0 && I(line, 0x34) > 0;                      // head, num_segs
            break;
        }
    }
    ConstIdealLine_dtor(line, 0);
    return usable;
}
// A single race's list is this machine's player and the AI cars (GenerateCarList). If an AI car would have no line,
// the race gets none: the player alone at the front, count 1 -- the list GenerateCarList makes for 0 AI cars (its
// driver map is the identity in a single race, so the player's car is car 0). Anything else is left alone.
static void drop_ai_without_line(World* w) {
    CarList& cl = w->cars;
    if (cl.count < 2 || cl.count > 16) return;
    int32_t player = -1, ai = 0;
    for (int32_t i = 0; i < cl.count; i++) {
        if (cl.cars[i].type == 0 && player < 0) player = i;
        else if (cl.cars[i].type == 1) ai++;
        else return;                                          // a second player, a network car, a ghost: not ours
    }
    if (player < 0 || ai == 0) return;
    int32_t missing = -1;
    for (int32_t i = 0; i < cl.count && missing < 0; i++)
        if (cl.cars[i].type == 1 && !ai_car_has_line(w, i)) missing = i;
    if (missing < 0) return;
    logf("race: AI car %d has no racing line on track \"%.32s\"%s (none of its .ilq / .ilg / default.ili loads with "
         "segments): racing with no AI cars instead of the %d chosen",
         missing, w->track, w->options.reversed ? " (reversed)" : "", ai);
    if (player != 0) memcpy(&cl.cars[0], &cl.cars[player], sizeof(CarListEntry));
    cl.count = 1;
}

// LoadRace: the track, the cars' resources, their setups
static void __cdecl LoadRace(World* w) {
    LoadTrackA(w);
    // FIX: a single race on a track with no AI racing line crashed as it started (AICar::reset read seginfo[-1] of
    // a car without a line); such a race gets no AI cars, as if none had been chosen, and says why in the log.
    // Only the single race (GameBeginSingle's world): the career's events count on their 8 cars, and a network
    // race's list is the session's.
    if (VP_FIX && w == s_single_race_world) drop_ai_without_line(w);
    LoadCarsA(w);
    SetupCarsA(w);
}
static void fp_load_race(Footprint& f, World*) { f.replay_only = "loads the track and the cars"; }
PORT_FN(0x0040a840, "LoadRace", LoadRace, fp_load_race)

static void __cdecl UnloadRace(World* w) {
    UnloadCarsA(w);
    UnloadTrackA(w);
}
PORT_FN(0x0040a870, "UnloadRace", UnloadRace, fp_end_race)

// SetupCars: each car's setup -- the player's from its setup file (not in a network game: the session's), an AI
// car's from AICarSetup, a network car's as it came; a ghost or an unknown type panics
static void __cdecl SetupCars(World* w) {
    AISetStrength(w->options.strength);
    for (int32_t i = 0; w->cars.count > i; i++) {
        CarListEntry& e = w->cars.cars[i];
        uint32_t t = (uint32_t)e.type;
        switch (t) {
        case 0:
            if (!MultiEnabled()) CarFileLoadSetupDataA(e.setup, e.car, w->track);
            break;
        case 1: AICarSetup(e.setup, e.car, i, w); break;
        case 2: break;
        case 3: LogPanic(k_ghost_in_list); break;
        default: LogPanic(k_bad_car_type, t); break;
        }
    }
}
static void fp_setup_cars(Footprint& f, World*) { f.replay_only = "loads the cars' setups"; }
PORT_FN(0x0040a890, "SetupCars", SetupCars, fp_setup_cars)

static void __cdecl LoadCar(const char* car) { apply_to_car_resourceA(ResourceSetMustLoad, car); }
static void fp_car_res(Footprint& f, const char*) { f.replay_only = "loads or unloads a car's resources"; }
PORT_FN(0x0040a940, "LoadCar", LoadCar, fp_car_res)

static void __cdecl UnloadCar(const char* car) { apply_to_car_resourceA(ResourceSetUnload, car); }
PORT_FN(0x0040a960, "UnloadCar", UnloadCar, fp_car_res)

static void __cdecl LoadCars(World* w) {
    ResourceSetMustLoad(get_paint_resourceA(w->options.strength));
    for (int32_t i = 0; w->cars.count > i; i++) LoadCarA(w->cars.cars[i].car);
}
static void fp_cars_res(Footprint& f, World*) { f.replay_only = "loads or unloads the cars' resources"; }
PORT_FN(0x0040a980, "LoadCars", LoadCars, fp_cars_res)

static void __cdecl UnloadCars(World* w) {
    for (int32_t i = 0; w->cars.count > i; i++) UnloadCarA(w->cars.cars[i].car);
    ResourceSetUnload(get_paint_resourceA(w->options.strength));
}
PORT_FN(0x0040a9d0, "UnloadCars", UnloadCars, fp_cars_res)

static void __cdecl LoadTrack(World* w) {
    char buf[0x100];                                          // (the original's 0x40)
    SplashLoading();
    ResourceSetUnload(k_ui_res);
    ResourceSetMustLoad(k_race_res);
    game_sprintf(buf, k_fmt_trk, w->track);
    ResourceSetMustLoad(buf);
}
static void fp_track_res(Footprint& f, World*) { f.replay_only = "loads or unloads the track's resources"; }
PORT_FN(0x0040aa20, "LoadTrack", LoadTrack, fp_track_res)

static void __cdecl UnloadTrack(World* w) {
    char buf[0x100];                                          // (the original's 0x40)
    game_sprintf(buf, k_fmt_trk2, w->track);
    ResourceSetUnload(buf);
    ResourceSetUnload(k_race_res2);
    ResourceSetMustLoad(k_ui_res2);
}
PORT_FN(0x0040aa70, "UnloadTrack", UnloadTrack, fp_track_res)

// GenerateCarList: `ai` AI cars and the player, in the car the driver map gives driver `ai` (the last, with the
// identity map); the player's name and paint job from the GLOBAL options, the AI cars' names and drivers from the
// driver map. More than 15 AI cars run on past the list (the World's options, and on).
static void __cdecl GenerateCarList(World* w, const char* car, int32_t ai) {
    int32_t paint;                                            // [L+4]
    const int32_t player = AIGetCarForDriver(ai);             // [L+8]
    CarList* cl = &w->cars;                                   // [L+0]
    AISetStrength(w->options.strength);
    cl->count = ai + 1;
    for (int32_t i = 0; cl->count > i; i++) {
        CarListEntry& e = cl->cars[i];
        if (i == player) {
            e.type = 0;
            s_copy(e.car, car);
            const char* section = *P<const char*>(S_SECT_GLOBAL);
            e.driver[0] = 0;
            OptionsGetS(section, k_player_name, e.driver, 0xd);
            const char* section2 = *P<const char*>(S_SECT_GLOBAL);
            paint = 0;
            OptionsGetI(section2, k_player_paintjob, &paint);
            e.driver_index = -1 - paint;
            e.hornball = HackHornBall();
        } else {
            e.type = 1;
            s_copy(e.car, k_viper);
            AIGetDriverNameByCar(e.driver, i);
            e.driver_index = AIGetDriverForCar(i);
            e.hornball = 0;
        }
    }
}
static void fp_generate_car_list(Footprint& f, World* w, const char*, int32_t ai) {
    if (ai + 1 > 16) { f.replay_only = "more than 16 cars run on past the car list"; return; }
    f.add(&w->cars, sizeof(CarList), "car list");
    f.add(P<void>(S_AI_STRENGTH), 4, "AI strength");
}
PORT_FN(0x0040aac0, "GenerateCarList", GenerateCarList, fp_generate_car_list)

// apply_to_car_resource: fn("<car>.car")
static void __cdecl apply_to_car_resource(ResName_t fn, const char* car) {
    char buf[0x100];                                          // (the original's 0x20: a car name past 27 characters overruns it)
    game_sprintf(buf, k_fmt_car, car);
    fn(buf);
}
static void fp_apply_car_res(Footprint& f, ResName_t, const char*) { f.replay_only = "loads or unloads a car's resources"; }
PORT_FN(0x0040ac10, "apply_to_car_resource", apply_to_car_resource, fp_apply_car_res)

// get_paint_resource: the AI strength's paint jobs. (Out of 0..7 the original reads its own stack -- the return
// address, its argument...: not reproduced.)
static const char* __cdecl get_paint_resource(int32_t strength) {
    return (uint32_t)strength < 8 ? (const char*)(uintptr_t)k_paint_res[strength] : 0;
}
static void fp_paint(Footprint&, int32_t) {}
PORT_FN(0x0040ac40, "get_paint_resource", get_paint_resource, fp_paint)
