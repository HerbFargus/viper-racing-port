// phys_race.cpp -- M3 3.4 group R: the race logic, rewritten. deity.obj (CarDeityBegin / CarDeityEnd and
// the Deity base class), racedty.obj (RaceDeity: laps, checkpoints, places, the start countdown, finishing,
// teleport-to-line) and dragdty.obj (DragDeity and its ZeroToSixty / QuarterMile flavours, SixtyToZeroDeity).
//
// The deity is one global object (`deity`, 0x5218ac) with a vtable the physics calls every tick:
// PhysTaskUpdate -> Update (+0x10) and GetMessage (+0x28); Car::Update / PlayCar::Update -> UpdateCar (+0x38),
// TeleportToLine (+0x3c), GetDLong (+0x50), GetCurrentLap (+0x48); the constructors (LocalCar, NetCar,
// CheckPoint) -> RegisterCar (+0x2c/+0x30), RegisterCheckPoint (+0x34). Every virtual call here goes through
// the object's vtable (VFN), never to a fixed address, so a hooked rewrite is what runs.
//
// Vtable (Deity, 0x4db9a0; RaceDeity 0x4dc588; DragDeity 0x4dc460, ZeroToSixty 0x4db8c0, QuarterMile 0x4db930;
// SixtyToZero 0x4dc4d0), in bytes: 0 deleting dtor, 4 StartRaceAt(float), 8 StopRace, 0xc Init, 0x10 Update,
// 0x14 Reset, 0x18 PostRace, 0x1c LocalCarsReady, 0x20 GetNumLaps, 0x24 GetNumCheckpoints, 0x28 GetMessage,
// 0x2c RegisterCar(LocalCar), 0x30 RegisterCar(NetCar), 0x34 RegisterCheckPoint, 0x38 UpdateCar,
// 0x3c TeleportToLine, 0x40 CarIsFinished, 0x44 GetCarByPlace, 0x48 GetCurrentLap, 0x4c GetCurrentLapStart,
// 0x50 GetDLong, 0x54 GetMaxDLong, 0x58 ConvertDLong, 0x5c LatLongPoint, 0x60 NetPacket;
// DragDeity adds 0x64 GetMaxTime, 0x68 IsDone(Car*).
//
// Threads: everything that writes runs on the physics thread -- physics_thread calls PhysTaskBegin (the
// constructors, via CarDeityBegin), PhysTaskRestart (Reset), PhysTaskUpdate, PhysTaskPostRace and PhysTaskEnd
// (0x42c091 / 0x42c0ac / 0x42c0bd / 0x42c0da); the phobs' constructors (RegisterCar / RegisterCheckPoint) run
// inside PhysTaskBegin's create_phob loop; MultiBGRecv (NetPacket, Restart) runs inside PhysTaskUpdate. The
// main thread only reads (dashboards: CarIsFinished, GetNumLaps, GetCarByPlace...). So the physics statics
// these functions write (racedty.obj's lapped[] at 0x522368, dragdty.obj's line at 0x5222e0, RaceDeity::globa,
// the sort function pointer, record.obj's lists) are saved by the shadow check automatically. What isn't a
// physics static and is written is listed: the game state (SetGameState, 0x4e4e3c).
//
// What can't be bounded is replay_only: allocation (CarDeityBegin, register_car), freeing (CarDeityEnd,
// ~RaceDeity), LogPanic, MultiDeityCast (a network send), Sound::Toss (a sound object), GhostNewBestLap
// (the ghost car's replay buffers), and PostRace's run of records. The records themselves (RecordNewLap,
// RecordStage, RecordRaceOver) write into three RecordLists whose buffers are bounded by their capacity, so a
// footprint can name them; where a call would also feed the ghost (a ghostable car completing a lap) or play a
// sound, the footprint decides at run time and makes that one call replay_only.
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

static __forceinline int32_t I(const float& x) { int32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline uint32_t Ub(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }      // an integer move of a float
static __forceinline void cp12(void* d, const void* s) { memcpy(d, s, 12); }    // three of them (a P3)

// ---- layouts -----------------------------------------------------------------------------------------------------
namespace {

struct Point2D { float x, z; };

// GameOptions (40 bytes, copied whole into every deity): only two fields are read here
struct GameOptions {
    uint32_t _00[5];
    int32_t num_laps;                  // +0x14 (WorldGameOptions()->num_laps is the same field of the live copy)
    uint32_t race_type;                // +0x18 0 race, 1 0-60, 3 quarter mile, 5 60-0 (CarDeityBegin's switch)
    uint32_t _1c[3];
};
static_assert(sizeof(GameOptions) == 40, "GameOptions");

struct Deity {                         // the base: a vtable and the options copy
    void** vtable;
    GameOptions options;               // +0x04
};
static_assert(sizeof(Deity) == 0x2c, "Deity");

// CenterLine (ai:ideal.obj, 112 bytes, heap, one per registered car): the car's cursor along the track's
// centre line. Opaque here except what the deity reads directly.
struct ILine { uint8_t _00[0x14]; float width; };
struct CenterLine {
    void** vtable;
    ILine* line;                       // +0x04 (CanTeleport: its +0x14 is the track width; 20 if null)
    uint8_t _08[0x30 - 0x08];
    uint8_t on_track;                  // +0x30 CanTeleport: 0 -> may teleport
    uint8_t _31[0x54 - 0x31];
    float lateral;                     // +0x54 CanTeleport: |lateral| > width/2 -> may teleport
    uint32_t _58;
    float max_dlong;                   // +0x5c GetMaxDLong
    uint8_t _60[0x70 - 0x60];
};
static_assert(sizeof(CenterLine) == 0x70, "CenterLine");

// ILSeg (ai:ideal.obj, 68 bytes): a node of an ideal line; ILinePos: a position on it
struct ILSeg {
    ILSeg* next;                       // +0x00
    Point2D p;                         // +0x04 x, z
    Point2D dir;                       // +0x0c
    float corridor_half_width;         // +0x14
    uint8_t _18[0x2c - 0x18];
    uint32_t field_2c;                 // +0x2c TeleportToLine: 0 -> offset to the other side
    uint8_t _30[0x44 - 0x30];
};
static_assert(sizeof(ILSeg) == 68, "ILSeg");
struct ILinePos { ILSeg* seg; float t; };

struct RaceInfo {                      // RaceDeity::RaceInfo, 0x74 bytes, one per car
    P3 prev_pos;                       // +0x00 last tick's lap-line point
    P3 pos;                            // +0x0c this tick's (front axle in the world)
    float line_time;                   // +0x18 when the start/finish line was last crossed
    float lap_start_time;              // +0x1c
    float lap_top_speed;               // +0x20 the car's, copied each tick and at the lap
    float last_lap_time;               // +0x24
    float stage_start_time;            // +0x28
    uint32_t dlong_cookie;             // +0x2c CenterLine::get_car_dlong_cookie
    uint8_t finished;                  // +0x30
    uint8_t remote;                    // +0x31 a NetCar: unofficial records, no deity cast
    uint8_t ghost;                     // +0x32 CarMgr type 3: laps follow car 0's, never places
    uint8_t _33;
    int32_t lap;                       // +0x34
    int32_t next_checkpoint;           // +0x38
    int32_t place;                     // +0x3c 1-based
    float current_lap_time;            // +0x40
    float best_lap;                    // +0x44
    float last_lap_display;            // +0x48 -0.1 until a lap is done
    float best_speed;                  // +0x4c
    float time_ahead;                  // +0x50 of the car behind (place_cars)
    int32_t behind_index;              // +0x54
    float time_behind;                 // +0x58 the car ahead
    int32_t ahead_index;               // +0x5c
    uint32_t f60, f64;                 // +0x60 UpdateCar writes 0 and 1.0f every tick
    uint16_t w68, w6a;                 // +0x68 zeroed by reset_car, sent in the message
    CenterLine* centerline;            // +0x6c
    struct Car* car;                   // +0x70
};
static_assert(offsetof(RaceInfo, lap) == 0x34 && offsetof(RaceInfo, centerline) == 0x6c && sizeof(RaceInfo) == 0x74, "RaceInfo");

struct RaceDeity : Deity {             // 2012 bytes, vtable 0x4dc588
    int32_t num_laps;                  // +0x2c
    int32_t num_cars;                  // +0x30
    int32_t num_checkpoints;           // +0x34
    RaceInfo info[16];                 // +0x38
    float teleport_ring[4];            // +0x778 the last four teleport points (metres along the line)
    int32_t teleport_ring_index;       // +0x788
    int32_t place_order[16];           // +0x78c car indices, sorted by place
    float start_time;                  // +0x7cc StartRaceAt
    float countdown;                   // +0x7d0 start_time - now, last tick
    float finish_deadline;             // +0x7d4 when the race ends after the player finishes
    uint8_t player_finished;           // +0x7d8
    uint8_t start_scheduled;           // +0x7d9
    uint8_t _7da[2];
};
static_assert(offsetof(RaceDeity, teleport_ring) == 0x778 && offsetof(RaceDeity, place_order) == 0x78c &&
              offsetof(RaceDeity, start_time) == 0x7cc && sizeof(RaceDeity) == 0x7dc, "RaceDeity");

struct CheckPoint : PhobRoot { uint8_t _6c[132 - 0x6c]; };   // vtable 0x4dc350; frame at +0x38
static_assert(sizeof(CheckPoint) == 132, "CheckPoint");

struct DragDeity : Deity {             // 104 bytes, vtable 0x4dc460 (0x4db8c0 ZeroToSixty, 0x4db930 QuarterMile)
    P3 prev_pos;                       // +0x2c
    P3 pos;                            // +0x38
    CheckPoint* cp[3];                 // +0x44 start, stage, finish
    int32_t num_checkpoints;           // +0x50
    int32_t num_laps;                  // +0x54 runs completed
    float stage_time;                  // +0x58 when the state last changed
    float run_time;                    // +0x5c
    float finish_speed;                // +0x60
    int32_t state;                     // +0x64 0 waiting, 1 staged, 2 launched, 3 running
};
static_assert(offsetof(DragDeity, cp) == 0x44 && sizeof(DragDeity) == 0x68, "DragDeity");

struct SixtyToZeroDeity : Deity {      // 72 bytes, vtable 0x4dc4d0
    float last_stop_dist;              // +0x2c
    float stop_dist;                   // +0x30
    int32_t num_laps;                  // +0x34
    P3 brake_start;                    // +0x38
    int32_t state;                     // +0x44 0 accelerating, 1 above 60, 2 braking
};
static_assert(offsetof(SixtyToZeroDeity, state) == 0x44 && sizeof(SixtyToZeroDeity) == 0x48, "SixtyToZeroDeity");

// Car (car.obj, 3768 bytes, vtable 0x4dbfd0): what the deities read and write
struct Car : PhobDyno {
    uint8_t _478[0x4fc - 0x478];
    int32_t race_state;                // +0x4fc 0 held on the grid, 2 racing, 3 finished
    uint8_t _500[0x510 - 0x500];
    int32_t car_index;                 // +0x510
    uint8_t _514[0xd7c - 0x514];
    P3 front_axle;                     // +0xd7c body-frame point: the lap-line point
    uint8_t _d88[0xe30 - 0xd88];
    float lap_top_speed;               // +0xe30 zeroed at each lap
    uint8_t _e34[0xe3d - 0xe34];
    uint8_t lowering;                  // +0xe3d blocks CanTeleport
    uint8_t _e3e[3768 - 0xe3e];
};
static_assert(offsetof(Car, race_state) == 0x4fc && offsetof(Car, front_axle) == 0xd7c &&
              offsetof(Car, lap_top_speed) == 0xe30 && offsetof(Car, lowering) == 0xe3d && sizeof(Car) == 3768, "Car");

// the physics shared-memory packet (PhysTaskUpdate hands GetMessage the writable buffer): what's written
struct RaceCarMessage { uint32_t d[14]; };          // RaceInfo +0x34 .. +0x6c
struct PhysicsPacket {
    uint8_t _00[0x3c];
    int32_t countdown;                 // +0x3c seconds to the start (+1), -1 not scheduled, 0 racing
    uint32_t _40;
    union {
        RaceCarMessage cars[16];       // +0x44
        struct {
            int32_t laps;              // +0x44
            int32_t state;             // +0x48
            int32_t valid;             // +0x4c
            float time;                // +0x50
            float best_time;           // +0x54
            uint32_t _58;
            float best_speed;          // +0x5c
        } drag;
    };
};
static_assert(offsetof(PhysicsPacket, cars) == 0x44 && sizeof(PhysicsPacket) == 0x3c4, "PhysicsPacket");

struct DeityPacket { float a, b; uint8_t car, lap, kind, _b; };   // kind 0xfa: a lap (a time, b speed); else a stage
static_assert(sizeof(DeityPacket) == 12, "DeityPacket");

struct CarMgrInfo {                    // CarMgrGetInfo(i), 0x168 bytes each
    int32_t type;                      // +0x000 1 the player, 3 a ghost
    uint8_t ai;                        // +0x004 0 -> is_ghostable_car
    uint8_t _005[0x138 - 5];
    int32_t rank;                      // +0x138 Update: cars ranked >= the player's - 1 must finish
    uint8_t _13c[0x168 - 0x13c];
};
struct AITrackInfo { uint8_t _00[0x10]; float first_lap_allowance; };
struct RecordList { int32_t name; uint8_t* recs; int32_t count; int32_t cap; };   // record.obj; 0x48-byte records

}  // namespace

// ---- globals ---------------------------------------------------------------------------------------------------
static Deity** const g_deity = (Deity**)0x005218ac;                     // class Deity* deity
static RaceDeity** const g_globa = (RaceDeity**)0x004edd70;             // RaceDeity::globa
typedef int(__fastcall* SortFn)(RaceDeity*, Edx, RaceInfo*, RaceInfo*);
static SortFn* const g_sort_fn = (SortFn*)0x004edd74;                   // RaceDeity's sort member function
static uint8_t* const g_lapped = (uint8_t*)0x00522368;                  // racedty.obj: which cars must finish
static P3* const g_drag_start = (P3*)0x005222e0;                        // dragdty.obj: the line-up point
static Point2D* const g_drag_heading = (Point2D*)0x00522318;            // and heading (rot m6, m8)
static int32_t* const g_game_state = (int32_t*)0x004e4e3c;              // SetGameState's (not a physics static)
static RecordList* const g_rec_race = (RecordList*)0x00521540;
static RecordList* const g_rec_laps = (RecordList*)0x00521550;
static RecordList* const g_rec_stages = (RecordList*)0x00521568;

// ---- constants -------------------------------------------------------------------------------------------------
static const float k_one_over_dt = 62.499996185302734f;                 // 0x4279ffff (not 62.5)
static const float k_grace = 100000.0f;                                 // 0x47c35000
static const float k_start_delay = 4.800000190734863f;                  // 0x4099999a
static const float k_teleport_gap = 7.0f, k_lane = 2.5f;
static const float k_teleport_height = 1.5250000953674316f;             // 0x3fc33334
static const float k_variance_scale = 9.999999747378752e-06f;           // 0x3727c5ac
static const float k_drag_max_time = 60.0f, k_drag_wrap_back = 50.0f, k_drag_wrap = 1000.0f, k_drag_stage = 3.0f;
static const float k_sixty_sq = 711.111083984375f;                      // 0x4431c71c (26.67 m/s)^2
static const uint32_t k_sixty_bits = 0x41d55555u;                       // 26.666666 m/s (60 mph), compared as bits
static const uint32_t k_one_mph_bits = 0x3ee38e39u;                     // 0.44444 m/s
static const uint32_t k_tenth_bits = 0x3dcccccdu;                       // 0.1f
static const uint32_t k_minus_one_bits = 0xbf800000u, k_minus_tenth_bits = 0xbdcccccdu, k_minus_1000_bits = 0xc47a0000u;
static const uint32_t k_one_bits = 0x3f800000u;
static const uint32_t k_neg_zero_bits = 0x80000000u;

// ---- the functions they call, by address -----------------------------------------------------------------------
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef void(__cdecl* SetGameState_t)(int);
static const SetGameState_t SetGameState = (SetGameState_t)0x0040a3e0;
typedef int(__cdecl* GetInt_t)();
static const GetInt_t GetGameState = (GetInt_t)0x0040a3d0;
static const GetInt_t WorldGetPlayerCar = (GetInt_t)0x00462710;
static const GetInt_t CarMgrCount = (GetInt_t)0x00464480;
static const GetInt_t AIGetTrackNumber = (GetInt_t)0x00420df0;
typedef uint8_t(__cdecl* Flag_t)();
static const Flag_t MultiEnabled = (Flag_t)0x004a23c0;
static const Flag_t PhysReplayPlayMode = (Flag_t)0x0042d340;
static const Flag_t PhysicsIsPaused = (Flag_t)0x0042bd20;
static const Flag_t GhostCarIncognito = (Flag_t)0x0040e940;
typedef double(__cdecl* PhysicsGetTime_t)();                            // physics_tick * 0.016f, in ST0
static const PhysicsGetTime_t PhysicsGetTime = (PhysicsGetTime_t)0x0042bc80;
typedef const GameOptions*(__cdecl* WorldGameOptions_t)();
static const WorldGameOptions_t WorldGameOptions = (WorldGameOptions_t)0x004627a0;
typedef const CarMgrInfo*(__cdecl* CarMgrGetInfo_t)(int);
static const CarMgrGetInfo_t CarMgrGetInfo = (CarMgrGetInfo_t)0x00464490;
typedef const AITrackInfo*(__cdecl* AIGetTrackInfo_t)(int);
static const AIGetTrackInfo_t AIGetTrackInfo = (AIGetTrackInfo_t)0x00424e40;
typedef const uint8_t*(__cdecl* AIGetSkill_t)(int);
static const AIGetSkill_t AIGetSkill = (AIGetSkill_t)0x00420d90;
typedef void(__cdecl* GhostNewBestLap_t)(int, int);
static const GhostNewBestLap_t GhostNewBestLap = (GhostNewBestLap_t)0x0040e950;
typedef int(__cdecl* Random_t)(int);
static const Random_t Random = (Random_t)0x0041b6e0;
typedef void(__cdecl* SoundToss_t)(const char*, int);
static const SoundToss_t Sound_Toss = (SoundToss_t)0x004724c0;
typedef void(__cdecl* MultiDeityCast_t)(const DeityPacket*, int);
static const MultiDeityCast_t MultiDeityCast = (MultiDeityCast_t)0x004a2b70;
// records (record.obj, a later stage): the floats are pushed as bits
typedef void(__cdecl* RecordLap_t)(int, int, uint32_t, uint32_t);
static const RecordLap_t RecordNewLap = (RecordLap_t)0x0042a700;
static const RecordLap_t RecordUnofficialNewLap = (RecordLap_t)0x0042a5d0;
typedef void(__cdecl* RecordStage_t)(int, int, int, uint32_t);      // (car, stage, lap, time)
static const RecordStage_t RecordStage = (RecordStage_t)0x0042a7f0;
static const RecordStage_t RecordUnofficialStage = (RecordStage_t)0x0042a840;
typedef void(__cdecl* RecordOver_t)(int);
static const RecordOver_t RecordRaceOver = (RecordOver_t)0x0042a720;
static const RecordOver_t RecordUnofficialRaceOver = (RecordOver_t)0x0042a7e0;
typedef double(__cdecl* RecordGetRaceTime_t)(int);                      // ST0; negative = no time
static const RecordGetRaceTime_t RecordGetRaceTime = (RecordGetRaceTime_t)0x0042a860;
// CenterLine (ai:ideal.obj)
typedef CenterLine*(__fastcall* CenterLineCtor_t)(CenterLine*, Edx, const uint8_t*);
static const CenterLineCtor_t CenterLine_ctor = (CenterLineCtor_t)0x00422400;
typedef void(__fastcall* CLVoid_t)(CenterLine*, Edx);
static const CLVoid_t CenterLine_reset = (CLVoid_t)0x004224c0;
typedef int(__fastcall* CLInt_t)(CenterLine*, Edx);
static const CLInt_t CenterLine_get_next_checkpoint = (CLInt_t)0x00422950;
typedef uint32_t(__fastcall* CLUint_t)(CenterLine*, Edx);
static const CLUint_t CenterLine_get_car_dlong_cookie = (CLUint_t)0x00422600;
typedef uint8_t(__fastcall* CLUpdate_t)(CenterLine*, Edx, const Point2D*, int);
static const CLUpdate_t CenterLine_update = (CLUpdate_t)0x00422510;
typedef double(__fastcall* CLCookieToMeters_t)(CenterLine*, Edx, uint32_t);
static const CLCookieToMeters_t CenterLine_convert_cookie_to_meters = (CLCookieToMeters_t)0x004227a0;
typedef ILinePos*(__fastcall* CLMetersToIlpos_t)(CenterLine*, Edx, ILinePos*, uint32_t);   // struct return: hidden pointer first
static const CLMetersToIlpos_t CenterLine_convert_meters_to_ilpos = (CLMetersToIlpos_t)0x00422710;
typedef double(__fastcall* CLTimeBetween_t)(CenterLine*, Edx, const CenterLine*);
static const CLTimeBetween_t CenterLine_time_between = (CLTimeBetween_t)0x00422840;
typedef void(__fastcall* QuickTan_t)(const ILSeg*, Edx, Point2D*, uint32_t);
static const QuickTan_t ILSeg_QuickTan = (QuickTan_t)0x00426150;
typedef uint8_t(__fastcall* HasCrossed_t)(CheckPoint*, Edx, const P3*, const P3*);
static const HasCrossed_t CheckPoint_HasCrossed = (HasCrossed_t)0x00440db0;
typedef uint8_t(__fastcall* CarFlag_t)(Car*, Edx);
static const CarFlag_t Car_AnyWheelsAreDamaged = (CarFlag_t)0x0043b060;
typedef double(__cdecl* TerrainGetHeight2_t)(uint32_t, uint32_t);      // (x, z) as bits; the height in ST0
static const TerrainGetHeight2_t TerrainGetHeight2 = (TerrainGetHeight2_t)0x00465ae0;
typedef void(__cdecl* Qsort_t)(void*, unsigned, unsigned, int(__cdecl*)(const void*, const void*));
static const Qsort_t qsort_o = (Qsort_t)0x004cf160;                     // the CRT's, in race.exe: its order of calls
typedef void(__cdecl* HermiteEval_t)(Point2D*, const Point2D*, const Point2D*, const Point2D*, const Point2D*, uint32_t);
static const HermiteEval_t HermiteEval_o = (HermiteEval_t)0x00443c10;
// the deities' own functions, by address (a hooked rewrite is what runs)
typedef RaceDeity*(__fastcall* RaceDeityCtor_t)(RaceDeity*, Edx, const GameOptions*);
static const RaceDeityCtor_t RaceDeity_ctor_o = (RaceDeityCtor_t)0x00442da0;
typedef DragDeity*(__fastcall* DragDeityCtor_t)(DragDeity*, Edx, const GameOptions*);
static const DragDeityCtor_t DragDeity_ctor_o = (DragDeityCtor_t)0x00442240;
typedef SixtyToZeroDeity*(__fastcall* SixtyCtor_t)(SixtyToZeroDeity*, Edx, const GameOptions*);
static const SixtyCtor_t SixtyToZeroDeity_ctor_o = (SixtyCtor_t)0x00442890;
typedef RaceInfo*(__fastcall* RaceInfoCtor_t)(RaceInfo*, Edx);
static const RaceInfoCtor_t RaceInfo_ctor = (RaceInfoCtor_t)0x00444290;   // `mov eax, ecx; ret`
typedef void(__fastcall* RDInt_t)(RaceDeity*, Edx, int);
static const RDInt_t reset_car_o = (RDInt_t)0x00442e80;
typedef void(__fastcall* RDIntCar_t)(RaceDeity*, Edx, int, Car*);
static const RDIntCar_t register_car_o = (RDIntCar_t)0x00442f30;
static const RDIntCar_t do_cheater_o = (RDIntCar_t)0x00443900;          // bare rets in v1.0: kept, as the original calls them
static const RDIntCar_t do_wrongway_o = (RDIntCar_t)0x00443910;
typedef void(__fastcall* RecordNewLapM_t)(RaceDeity*, Edx, int, int, uint32_t, uint32_t);
static const RecordNewLapM_t record_newlap_o = (RecordNewLapM_t)0x00443520;
typedef void(__fastcall* RecordNewStageM_t)(RaceDeity*, Edx, int, int, int, uint32_t);
static const RecordNewStageM_t record_newstage_o = (RecordNewStageM_t)0x004435b0;
typedef uint8_t(__cdecl* CanTeleport_t)(Car*);
static const CanTeleport_t CanTeleport_o = (CanTeleport_t)0x00443920;
typedef void(__fastcall* SetBests_t)(RaceDeity*, Edx, RaceInfo*);
static const SetBests_t set_bests_o = (SetBests_t)0x00444090;
typedef void(__fastcall* SortCars_t)(RaceDeity*, Edx, uint32_t);
static const SortCars_t sort_cars_o = (SortCars_t)0x00443ea0;
typedef void(__fastcall* RDVoid_t)(RaceDeity*, Edx);
static const RDVoid_t place_cars_o = (RDVoid_t)0x00443ee0;
static const RDVoid_t RaceDeity_dtor_o = (RDVoid_t)0x00442e30;
typedef double(__cdecl* Variance_t)(uint32_t, int);
static const Variance_t variance_o = (Variance_t)0x00443260;
static const uint32_t final_sort_o = 0x00443cc0;                       // RaceDeity::final_sort, as sort_cars's argument
static const uint32_t race_sort_o = 0x00443dd0;                        // RaceDeity::race_sort
typedef int(__cdecl* StaticSort_t)(const void*, const void*);
static const StaticSort_t static_sort_f_o = (StaticSort_t)0x00443e50;
// strings and formats, at their v1.0 addresses
static const char* const k_fmt_begin_panic = (const char*)0x004ed150;   // CarDeityBegin: unknown race type
static const char* const k_fmt_netcar_panic = (const char*)0x004edcfc;  // DragDeity::RegisterCar(NetCar)
static const char* const k_fmt_checkpoint_report = (const char*)0x004edd28;
static const char* const k_fmt_drag_state_panic = (const char*)0x004edd5c;
static const char* const k_snd_staged = (const char*)0x004edd3c, *const k_snd_foul = (const char*)0x004edd48,
                        *const k_snd_go = (const char*)0x004edd54;
static const char* const k_snd_start = (const char*)0x004eddec, *const k_snd_tick = (const char*)0x004eddf4;

// fmod(x, 1.0) through the CRT's __CIfmod in race.exe (0x4cf36a): x in ST1, 1.0 in ST0, the result in ST0. fprem
// is exact, so the result of a float-valued x is a float value; kept as a double, as the original compares it
// in the register.
static double cifmod_one(double x) {
    static const double one = 1.0;
    double r;
    __asm {
        push ecx
        push edx
        fld x
        fld one
        mov eax, 0x4cf36a
        call eax
        pop edx
        pop ecx
        fstp r
    }
    return r;
}

// ---- footprint helpers -------------------------------------------------------------------------------------------
static void fp_game_state(Footprint& f) { f.add(g_game_state, 4, "game state"); }
static void fp_race_deity(Footprint& f, RaceDeity* self) { f.add(self, sizeof(RaceDeity), "RaceDeity"); }
static int fp_ncars(const RaceDeity* self) { int n = self->num_cars; return n < 0 ? 0 : n > 16 ? 16 : n; }
static void fp_cars(Footprint& f, RaceDeity* self) {
    for (int i = 0, n = fp_ncars(self); i < n; i++)
        if (self->info[i].car) f.object(self->info[i].car, "car");
}
static void fp_lines(Footprint& f, RaceDeity* self) {
    for (int i = 0, n = fp_ncars(self); i < n; i++)
        if (self->info[i].centerline) f.add(self->info[i].centerline, sizeof(CenterLine), "centerline");
}
// the three record lists' buffers (record() writes one record, new or found, in each; capacity-bounded)
static void fp_records(Footprint& f) {
    RecordList* const lists[3] = {g_rec_race, g_rec_laps, g_rec_stages};
    for (RecordList* l : lists)
        if (l->recs && l->cap > 0) f.add(l->recs, (uint32_t)l->cap * 0x48, "records");
}
static uint8_t is_ghostable_car(int i) { return CarMgrGetInfo(i)->ai == 0; }   // ghost.obj's predicate (0x40eb40)

// =================================================================================================================
// deity.obj
// =================================================================================================================

// ==== CarDeityBegin (0x42c4f0) ===================================================================================
// Makes the deity for the race type in the options and Resets it. Called by PhysTaskBegin (physics thread).
static void __cdecl CarDeityBegin(const GameOptions* o) {
    *g_deity = 0;
    switch (o->race_type) {                          // cmp eax, 5; ja -> unsigned
    case 0: {
        void* p = MemAlloc(0x7dc);
        if (p) *g_deity = RaceDeity_ctor_o((RaceDeity*)p, 0, o);
        else *g_deity = 0;
        break;
    }
    case 1: case 3: {
        DragDeity* p = (DragDeity*)MemAlloc(0x68);
        if (p) {
            DragDeity_ctor_o(p, 0, o);
            p->vtable = (void**)(o->race_type == 1 ? 0x004db8c0 : 0x004db930);   // ZeroToSixtyDeity / QuarterMileDeity
            *g_deity = p;
        } else *g_deity = 0;
        break;
    }
    case 5: {
        void* p = MemAlloc(0x48);
        if (p) *g_deity = SixtyToZeroDeity_ctor_o((SixtyToZeroDeity*)p, 0, o);
        else *g_deity = 0;
        break;
    }
    default:
        LogPanic(k_fmt_begin_panic, o->race_type);   // and then Reset through a null deity, as the original does
        break;
    }
    Deity* d = *g_deity;
    VFN(d, 0x14, void)(d, 0);                        // Reset
}
static void fp_car_deity_begin(Footprint& f, const GameOptions*) { f.replay_only = "allocates the deity"; }
PORT_FN(0x0042c4f0, "CarDeityBegin", CarDeityBegin, fp_car_deity_begin)

// ==== Deity::StopRace (0x42c5e0) ==================================================================================
static void __fastcall Deity_StopRace(Deity*, Edx) { SetGameState(3); }
static void fp_deity_stop_race(Footprint& f, Deity*, Edx) { fp_game_state(f); }
PORT_FN(0x0042c5e0, "Deity::StopRace", Deity_StopRace, fp_deity_stop_race)

// ==== CarDeityEnd (0x42c5f0) =====================================================================================
static void __cdecl CarDeityEnd() {
    Deity* d = *g_deity;
    if (d) VFN(d, 0, void*, int)(d, 0, 1);           // the deleting destructor
    *g_deity = 0;
}
static void fp_car_deity_end(Footprint& f) { f.replay_only = "frees the deity"; }
PORT_FN(0x0042c5f0, "CarDeityEnd", CarDeityEnd, fp_car_deity_end)

// ==== the Deity base's constant methods (the bare `ret` ones -- StartRaceAt, Update, PostRace, Reset,
// GetMessage, NetPacket -- are not ported) ========================================================================
static uint8_t __fastcall Deity_LocalCarsReady(Deity*, Edx) { return 1; }
static void fp_deity_const(Footprint& f, Deity*, Edx) { f.pure = true; }
PORT_FN(0x0042c640, "Deity::LocalCarsReady", Deity_LocalCarsReady, fp_deity_const)
static int __fastcall Deity_GetNumCheckpoints(Deity*, Edx) { return 0; }
PORT_FN(0x0042c650, "Deity::GetNumCheckpoints", Deity_GetNumCheckpoints, fp_deity_const)
static uint8_t __fastcall Deity_CarIsFinished(Deity*, Edx, int) { return 0; }
static void fp_deity_const_i(Footprint& f, Deity*, Edx, int) { f.pure = true; }
PORT_FN(0x0042c660, "Deity::CarIsFinished", Deity_CarIsFinished, fp_deity_const_i)
static int __fastcall Deity_GetCarByPlace(Deity*, Edx, int place) { return place; }
PORT_FN(0x0042c670, "Deity::GetCarByPlace", Deity_GetCarByPlace, fp_deity_const_i)
static int __fastcall Deity_GetCurrentLap(Deity*, Edx, int) { return 0; }
PORT_FN(0x0042c680, "Deity::GetCurrentLap", Deity_GetCurrentLap, fp_deity_const_i)
static double __fastcall Deity_GetCurrentLapStart(Deity*, Edx, int) { return 0.0f; }   // fld 0.0 -> ST0
PORT_FN(0x0042c690, "Deity::GetCurrentLapStart", Deity_GetCurrentLapStart, fp_deity_const_i)
static uint32_t __fastcall Deity_GetDLong(Deity*, Edx, int) { return 0; }
PORT_FN(0x0042c6a0, "Deity::GetDLong", Deity_GetDLong, fp_deity_const_i)
static double __fastcall Deity_GetMaxDLong(Deity*, Edx) { return 1.0f; }
PORT_FN(0x0042c6b0, "Deity::GetMaxDLong", Deity_GetMaxDLong, fp_deity_const)
static double __fastcall Deity_ConvertDLong(Deity*, Edx, uint32_t) { return 0.0f; }
static void fp_deity_const_u(Footprint& f, Deity*, Edx, uint32_t) { f.pure = true; }
PORT_FN(0x0042c6c0, "Deity::ConvertDLong", Deity_ConvertDLong, fp_deity_const_u)
static uint8_t __fastcall Deity_LatLongPoint(Deity*, Edx, Point2D*, Point2D*, const Point2D*) { return 0; }
static void fp_deity_latlong(Footprint& f, Deity*, Edx, Point2D*, Point2D*, const Point2D*) { f.pure = true; }
PORT_FN(0x0042c6d0, "Deity::LatLongPoint", Deity_LatLongPoint, fp_deity_latlong)
static int __fastcall DragDeity_GetNumLaps(DragDeity* self, Edx) { return self->num_laps; }
static void fp_drag_const(Footprint& f, DragDeity*, Edx) { f.pure = true; }
PORT_FN(0x0042c6f0, "DragDeity::GetNumLaps", DragDeity_GetNumLaps, fp_drag_const)
static uint8_t __fastcall Deity_Init(Deity*, Edx) { return 1; }
PORT_FN(0x0042c720, "Deity::Init", Deity_Init, fp_deity_const)
static int __fastcall Deity_GetNumLaps(Deity*, Edx) { return 0; }
PORT_FN(0x0042c740, "Deity::GetNumLaps", Deity_GetNumLaps, fp_deity_const)
static uint8_t __fastcall Deity_TeleportToLine(Deity*, Edx, int, Car*) { return 0; }
static void fp_deity_teleport(Footprint& f, Deity*, Edx, int, Car*) { f.pure = true; }
PORT_FN(0x0042c760, "Deity::TeleportToLine", Deity_TeleportToLine, fp_deity_teleport)

// =================================================================================================================
// dragdty.obj
// =================================================================================================================

// ==== DragDeity::DragDeity (0x442240) =============================================================================
static DragDeity* __fastcall DragDeity_ctor(DragDeity* self, Edx, const GameOptions* o) {
    self->vtable = (void**)0x004db9a0;
    memcpy(&self->options, o, sizeof(GameOptions));   // rep movsd
    self->vtable = (void**)0x004dc460;
    self->cp[2] = 0; self->cp[1] = 0; self->cp[0] = 0;
    self->stage_time = 0.0f; self->run_time = 0.0f; self->finish_speed = 0.0f; self->state = 0;
    g_drag_start->x = 0.0f; g_drag_start->y = 0.0f; g_drag_start->z = 0.0f;
    g_drag_heading->x = 0.0f; g_drag_heading->z = 0.0f;
    self->num_checkpoints = 0;
    return self;
}
// runs on the physics thread (PhysTaskBegin): the dragdty.obj statics it clears are saved automatically
static void fp_drag_ctor(Footprint& f, DragDeity* self, Edx, const GameOptions*) { f.add(self, sizeof(DragDeity), "DragDeity"); }
PORT_FN(0x00442240, "DragDeity::DragDeity", DragDeity_ctor, fp_drag_ctor)

// ==== DragDeity::RegisterCar(int, NetCar*) (0x4422a0): a panic ====================================================
static void __fastcall DragDeity_RegisterCar_Net(DragDeity*, Edx, int, Car*) { LogPanic(k_fmt_netcar_panic); }
static void fp_drag_register_net(Footprint& f, DragDeity*, Edx, int, Car*) { f.replay_only = "LogPanic"; }
PORT_FN(0x004422a0, "DragDeity::RegisterCar(NetCar)", DragDeity_RegisterCar_Net, fp_drag_register_net)

// ==== DragDeity::RegisterCar(int, LocalCar*) (0x4422b0) ==========================================================
// The car's lap-line point (the front axle in the world) becomes prev_pos; the car's position and heading
// (rotation rows 2's x and z) become the line-up point TeleportToLine returns to.
static void __fastcall DragDeity_RegisterCar_Local(DragDeity* self, Edx, int, Car* car) {
    const float* M = car->frame.rot.m;
    const P3& o = car->front_axle;
    float x = (float)((D(M[0]) * o.x + D(M[3]) * o.y) + D(M[6]) * o.z);
    float y = (float)((D(M[4]) * o.y + D(M[1]) * o.x) + D(M[7]) * o.z);
    float z = (float)((D(M[5]) * o.y + D(M[2]) * o.x) + D(M[8]) * o.z);
    x = (float)(D(car->frame.pos.x) + x);
    y = (float)(D(car->frame.pos.y) + y);
    z = (float)(D(car->frame.pos.z) + z);
    cp4(&self->prev_pos.x, &x); cp4(&self->prev_pos.y, &y); cp4(&self->prev_pos.z, &z);
    cp12(g_drag_start, &car->frame.pos);
    cp4(&g_drag_heading->x, &M[6]);
    cp4(&g_drag_heading->z, &M[8]);
}
static void fp_drag_register_local(Footprint& f, DragDeity* self, Edx, int, Car*) { f.add(self, sizeof(DragDeity), "DragDeity"); }
PORT_FN(0x004422b0, "DragDeity::RegisterCar(LocalCar)", DragDeity_RegisterCar_Local, fp_drag_register_local)

// ==== DragDeity::GetMaxTime (0x4423a0) ===========================================================================
static double __fastcall DragDeity_GetMaxTime(DragDeity*, Edx) { return k_drag_max_time; }
PORT_FN(0x004423a0, "DragDeity::GetMaxTime", DragDeity_GetMaxTime, fp_drag_const)

// ==== DragDeity::RegisterCheckPoint (0x4423b0) ====================================================================
static void __fastcall DragDeity_RegisterCheckPoint(DragDeity* self, Edx, CheckPoint* cp) {
    switch (self->num_checkpoints) {
    case 0: self->num_checkpoints++; self->cp[0] = cp; break;
    case 1: self->num_checkpoints++; self->cp[1] = cp; break;
    case 2: self->num_checkpoints++; self->cp[2] = cp; break;
    default: LogReport(k_fmt_checkpoint_report, self->num_checkpoints); self->num_checkpoints++; break;
    }
}
static void fp_drag_register_cp(Footprint& f, DragDeity* self, Edx, CheckPoint*) {
    if ((uint32_t)self->num_checkpoints > 2) f.replay_only = "LogReport";
    else f.add(self, sizeof(DragDeity), "DragDeity");
}
PORT_FN(0x004423b0, "DragDeity::RegisterCheckPoint", DragDeity_RegisterCheckPoint, fp_drag_register_cp)

// ==== DragDeity::Init (0x442410) ==================================================================================
static uint8_t __fastcall DragDeity_Init(DragDeity* self, Edx) { return self->cp[0] && self->cp[1] && self->cp[2]; }
PORT_FN(0x00442410, "DragDeity::Init", DragDeity_Init, fp_drag_const)

// ==== DragDeity::UpdateCar (0x442430) ============================================================================
// The lap-line point; the strip wraps the car's z into [line - 50, line + 950); then the state machine:
// 0 waiting for the start checkpoint, 1 staged (foul if the stage line is crossed within 3 s, else the go),
// 2 launched (3 s to cross the stage line), 3 running until IsDone (records the run) or GetMaxTime.
static void __fastcall DragDeity_UpdateCar(DragDeity* self, Edx, int idx, Car* car) {
    const float* M = car->frame.rot.m;
    const P3& o = car->front_axle;
    float x = (float)((D(M[6]) * o.z + D(M[3]) * o.y) + D(M[0]) * o.x);
    float y = (float)((D(M[4]) * o.y + D(M[1]) * o.x) + D(M[7]) * o.z);
    float z = (float)((D(M[2]) * o.x + D(M[8]) * o.z) + D(M[5]) * o.y);
    x = (float)(D(car->frame.pos.x) + x);
    y = (float)(D(car->frame.pos.y) + y);
    z = (float)(D(car->frame.pos.z) + z);
    cp4(&self->pos.x, &x); cp4(&self->pos.y, &y); cp4(&self->pos.z, &z);
    // wrap the car along the strip, about the stage checkpoint's z
    const float back = (float)(D(self->cp[1]->frame.pos.z) - k_drag_wrap_back);
    if (!(D(back) + k_drag_wrap >= D(car->frame.pos.z)))                    // test ah,1: less or unordered
        car->frame.pos.z = (float)(D(car->frame.pos.z) - k_drag_wrap);
    if (!(D(car->frame.pos.z) >= D(back)))
        car->frame.pos.z = (float)(D(car->frame.pos.z) + k_drag_wrap);
    switch ((uint32_t)self->state) {
    case 0:
        if (CheckPoint_HasCrossed(self->cp[0], 0, &self->prev_pos, &self->pos)) {
            Sound_Toss(k_snd_staged, 1);
            self->state = 1;
            self->stage_time = (float)PhysicsGetTime();
        }
        break;
    case 1:
        if (CheckPoint_HasCrossed(self->cp[1], 0, &self->prev_pos, &self->pos)) {
            Sound_Toss(k_snd_foul, 1);
            self->state = 0;
        } else if (PhysicsGetTime() - D(self->stage_time) > D(k_drag_stage)) {    // test ah,0x41: ordered greater
            self->stage_time = (float)PhysicsGetTime();
            Sound_Toss(k_snd_go, 1);
            self->state = 2;
        }
        break;
    case 2:
        if (PhysicsGetTime() - D(self->stage_time) > D(k_drag_stage)) self->state = 0;
        if (CheckPoint_HasCrossed(self->cp[1], 0, &self->prev_pos, &self->pos)) {
            self->stage_time = (float)PhysicsGetTime();
            self->state = 3;
        }
        break;
    case 3: {
        const float max_time = (float)VFN(self, 0x64, double)(self, 0);          // GetMaxTime, stored
        if (PhysicsGetTime() - D(self->stage_time) > D(max_time)) self->state = 0;
        if (VFN(self, 0x68, uint8_t, Car*)(self, 0, car)) {                      // IsDone
            self->run_time = (float)(PhysicsGetTime() - D(self->stage_time));
            const P3& v = car->velocity;
            const double s2 = (D(v.z) * v.z + D(v.y) * v.y) + D(v.x) * v.x;
            const int laps = self->num_laps + 1;
            self->num_laps = laps;
            self->finish_speed = (float)x87_sqrt(s2);
            RecordStage(idx, 1, laps, Ub(self->run_time));
            RecordNewLap(idx, laps, Ub(self->run_time), Ub(self->finish_speed));
            self->state = 0;
        }
        break;
    }
    default:
        LogPanic(k_fmt_drag_state_panic, self->state);
        break;
    }
    cp12(&self->prev_pos, &self->pos);
}
// The sounds and the records depend on CheckPoint::HasCrossed and IsDone this tick, which can't be known
// before the call without redoing it; the run is checked by replays.
static void fp_drag_update_car(Footprint& f, DragDeity*, Edx, int, Car*) { f.replay_only = "sounds / records on a line crossing"; }
PORT_FN(0x00442430, "DragDeity::UpdateCar", DragDeity_UpdateCar, fp_drag_update_car)

// ==== DragDeity::TeleportToLine (0x442700) =======================================================================
static uint8_t __fastcall DragDeity_TeleportToLine(DragDeity* self, Edx, int, Car* car) {
    self->state = 0;
    VFN(car, 0x40, void, const P3*, const Point2D*)(car, 0, g_drag_start, g_drag_heading);   // Car::Teleport
    return 1;
}
static void fp_drag_teleport(Footprint& f, DragDeity* self, Edx, int, Car* car) {
    f.add(self, sizeof(DragDeity), "DragDeity");
    f.object(car, "car");
}
PORT_FN(0x00442700, "DragDeity::TeleportToLine", DragDeity_TeleportToLine, fp_drag_teleport)

// ==== DragDeity::GetMessage (0x442720) ===========================================================================
static void __fastcall DragDeity_GetMessage(DragDeity* self, Edx, PhysicsPacket* p) {
    switch ((uint32_t)self->state) {
    case 0: p->countdown = -1; break;
    case 1: p->countdown = 1; break;
    case 2: case 3: p->countdown = 0; break;
    default: break;
    }
    p->drag.valid = 1;
    p->drag.state = self->state;
    p->drag.laps = self->num_laps + (self->state == 3 ? 1 : 0);
    if (self->state == 3) {
        p->drag.time = (float)(PhysicsGetTime() - D(self->stage_time));
        return;
    }
    cp4(&p->drag.time, &self->run_time);
    if (I(p->drag.best_time) > (int32_t)k_tenth_bits) {
        const double best = D(p->drag.best_time), run = D(self->run_time);
        p->drag.best_time = (float)(run > best ? best : run);                    // test ah,0x41
        const double spd = D(self->finish_speed), bs = D(p->drag.best_speed);
        p->drag.best_speed = (float)(!(bs >= spd) ? spd : bs);                   // test ah,1
    } else {
        cp4(&p->drag.best_time, &self->run_time);
        cp4(&p->drag.best_speed, &self->finish_speed);
    }
}
static void fp_drag_get_message(Footprint& f, DragDeity*, Edx, PhysicsPacket* p) { f.add(p, sizeof(PhysicsPacket), "packet"); }
PORT_FN(0x00442720, "DragDeity::GetMessage", DragDeity_GetMessage, fp_drag_get_message)

// ==== DragDeity::Reset (0x442810) ================================================================================
static void __fastcall DragDeity_Reset(DragDeity* self, Edx) {
    self->num_laps = 0; self->state = 0; self->run_time = 0.0f; self->finish_speed = 0.0f;
    SetGameState(1);
}
static void fp_drag_reset(Footprint& f, DragDeity* self, Edx) { f.add(self, sizeof(DragDeity), "DragDeity"); fp_game_state(f); }
PORT_FN(0x00442810, "DragDeity::Reset", DragDeity_Reset, fp_drag_reset)

// ==== ZeroToSixtyDeity::IsDone (0x442830): |v|^2 > (60 mph)^2 ====================================================
static uint8_t __fastcall ZeroToSixtyDeity_IsDone(DragDeity*, Edx, Car* car) {
    const P3& v = car->velocity;
    const double s2 = (D(v.y) * v.y + D(v.z) * v.z) + D(v.x) * v.x;
    return s2 > D(k_sixty_sq);                                                    // sete after test ah,0x41
}
static void fp_zts_is_done(Footprint& f, DragDeity*, Edx, Car*) { f.pure = true; }
PORT_FN(0x00442830, "ZeroToSixtyDeity::IsDone", ZeroToSixtyDeity_IsDone, fp_zts_is_done)

// ==== QuarterMileDeity::IsDone (0x442870): the finish checkpoint crossed ==========================================
static uint8_t __fastcall QuarterMileDeity_IsDone(DragDeity* self, Edx, Car*) {
    return CheckPoint_HasCrossed(self->cp[2], 0, &self->prev_pos, &self->pos);
}
static void fp_qm_is_done(Footprint&, DragDeity*, Edx, Car*) {}
PORT_FN(0x00442870, "QuarterMileDeity::IsDone", QuarterMileDeity_IsDone, fp_qm_is_done)

// ==== SixtyToZeroDeity::SixtyToZeroDeity (0x442890) ===============================================================
static SixtyToZeroDeity* __fastcall SixtyToZeroDeity_ctor(SixtyToZeroDeity* self, Edx, const GameOptions* o) {
    self->vtable = (void**)0x004db9a0;
    memcpy(&self->options, o, sizeof(GameOptions));
    self->vtable = (void**)0x004dc4d0;
    self->num_laps = 0;
    self->state = 0;
    return self;
}
// (not marked pure: it returns `this`, which the offline fuzzer can't compare between its two arenas)
static void fp_stz_ctor(Footprint& f, SixtyToZeroDeity* self, Edx, const GameOptions*) { f.add(self, sizeof(SixtyToZeroDeity), "SixtyToZeroDeity"); }
PORT_FN(0x00442890, "SixtyToZeroDeity::SixtyToZeroDeity", SixtyToZeroDeity_ctor, fp_stz_ctor)

// ==== SixtyToZeroDeity::Init (0x4428f0) ==========================================================================
static uint8_t __fastcall SixtyToZeroDeity_Init(SixtyToZeroDeity*, Edx) { return 1; }
static void fp_stz_const(Footprint& f, SixtyToZeroDeity*, Edx) { f.pure = true; }
PORT_FN(0x004428f0, "SixtyToZeroDeity::Init", SixtyToZeroDeity_Init, fp_stz_const)

// ==== SixtyToZeroDeity::Reset (0x442900) =========================================================================
static void __fastcall SixtyToZeroDeity_Reset(SixtyToZeroDeity* self, Edx) {
    self->state = 0; self->num_laps = 0; self->stop_dist = 0.0f; self->last_stop_dist = 0.0f;
    SetGameState(1);
}
static void fp_stz_reset(Footprint& f, SixtyToZeroDeity* self, Edx) { f.add(self, sizeof(SixtyToZeroDeity), "SixtyToZeroDeity"); fp_game_state(f); }
PORT_FN(0x00442900, "SixtyToZeroDeity::Reset", SixtyToZeroDeity_Reset, fp_stz_reset)

// ==== SixtyToZeroDeity::UpdateCar (0x442920) =====================================================================
// 0: wait for 60 mph; 1: wait to drop below it (the braking point); 2: measure the distance from it until 1 mph.
static void __fastcall SixtyToZeroDeity_UpdateCar(SixtyToZeroDeity* self, Edx, int idx, Car* car) {
    const P3& v = car->velocity;
    const float speed = (float)x87_sqrt((D(v.z) * v.z + D(v.y) * v.y) + D(v.x) * v.x);
    switch (self->state) {
    case 0:
        if (I(speed) > (int32_t)k_sixty_bits) self->state = 1;
        break;
    case 1:
        if (I(speed) < (int32_t)k_sixty_bits) {
            self->state = 2;
            cp12(&self->brake_start, &car->frame.pos);
            self->stop_dist = 0.0f;
        }
        break;
    case 2: {
        const float dx = (float)(D(self->brake_start.x) - car->frame.pos.x);
        const float dy = (float)(D(self->brake_start.y) - car->frame.pos.y);
        const float dz = (float)(D(self->brake_start.z) - car->frame.pos.z);
        self->stop_dist = (float)x87_sqrt((D(dz) * dz + D(dy) * dy) + D(dx) * dx);
        if (I(speed) < (int32_t)k_one_mph_bits) {
            self->state = 0;
            cp4(&self->last_stop_dist, &self->stop_dist);
            const int laps = self->num_laps + 1;
            self->num_laps = laps;
            RecordStage(idx, 1, laps, Ub(self->last_stop_dist));
            RecordNewLap(idx, self->num_laps, Ub(self->last_stop_dist), k_sixty_bits);
        }
        break;
    }
    default: break;
    }
}
// the records come only while braking (state 2); the other states write the deity alone
static void fp_stz_update_car(Footprint& f, SixtyToZeroDeity* self, Edx, int, Car*) {
    if (self->state == 2) f.replay_only = "records the stop";
    else f.add(self, sizeof(SixtyToZeroDeity), "SixtyToZeroDeity");
}
PORT_FN(0x00442920, "SixtyToZeroDeity::UpdateCar", SixtyToZeroDeity_UpdateCar, fp_stz_update_car)

// ==== SixtyToZeroDeity::TeleportToLine (0x442a60) ================================================================
static uint8_t __fastcall SixtyToZeroDeity_TeleportToLine(SixtyToZeroDeity*, Edx, int, Car*) { return 0; }
static void fp_stz_teleport(Footprint& f, SixtyToZeroDeity*, Edx, int, Car*) { f.pure = true; }
PORT_FN(0x00442a60, "SixtyToZeroDeity::TeleportToLine", SixtyToZeroDeity_TeleportToLine, fp_stz_teleport)

// ==== SixtyToZeroDeity::GetMessage (0x442a70) ====================================================================
static void __fastcall SixtyToZeroDeity_GetMessage(SixtyToZeroDeity* self, Edx, PhysicsPacket* p) {
    switch (self->state) {
    case 0: p->countdown = -1; break;
    case 1: p->countdown = 1; break;
    case 2: p->countdown = 0; break;
    default: break;
    }
    p->drag.valid = 1;
    p->drag.state = self->state;
    p->drag.laps = self->num_laps + (self->state == 2 ? 1 : 0);
    if (self->state == 2) { cp4(&p->drag.time, &self->stop_dist); return; }
    cp4(&p->drag.time, &self->last_stop_dist);
    if (I(p->drag.best_time) > (int32_t)k_tenth_bits) {
        const double best = D(p->drag.best_time), last = D(self->last_stop_dist);
        p->drag.best_time = (float)(last > best ? best : last);
    } else cp4(&p->drag.best_time, &self->last_stop_dist);
}
static void fp_stz_get_message(Footprint& f, SixtyToZeroDeity*, Edx, PhysicsPacket* p) { f.add(p, sizeof(PhysicsPacket), "packet"); f.pure = true; }
PORT_FN(0x00442a70, "SixtyToZeroDeity::GetMessage", SixtyToZeroDeity_GetMessage, fp_stz_get_message)

// ==== SixtyToZeroDeity::GetNumLaps (0x442b30) ====================================================================
static int __fastcall SixtyToZeroDeity_GetNumLaps(SixtyToZeroDeity* self, Edx) { return self->num_laps; }
PORT_FN(0x00442b30, "SixtyToZeroDeity::GetNumLaps", SixtyToZeroDeity_GetNumLaps, fp_stz_const)

// =================================================================================================================
// racedty.obj
// =================================================================================================================

// ==== RaceDeity::RaceDeity (0x442da0) ============================================================================
static RaceDeity* __fastcall RaceDeity_ctor(RaceDeity* self, Edx, const GameOptions* o) {
    self->vtable = (void**)0x004db9a0;
    memcpy(&self->options, o, sizeof(GameOptions));
    for (int i = 0; i < 16; i++) RaceInfo_ctor(&self->info[i], 0);              // a no-op, but called
    self->vtable = (void**)0x004dc588;
    *g_globa = self;
    self->player_finished = 0;
    self->num_checkpoints = 0;
    self->num_cars = 0;
    self->num_laps = WorldGameOptions()->num_laps;
    self->start_time = 0.0f;
    self->countdown = 0.0f;
    self->start_scheduled = 0;
    for (int i = 0; i < 16; i++) self->info[i].centerline = 0;
    return self;
}
// physics thread (PhysTaskBegin); RaceDeity::globa is a physics static
static void fp_race_ctor(Footprint& f, RaceDeity* self, Edx, const GameOptions*) { fp_race_deity(f, self); }
PORT_FN(0x00442da0, "RaceDeity::RaceDeity", RaceDeity_ctor, fp_race_ctor)

// ==== RaceDeity::~RaceDeity (0x442e30) ===========================================================================
static void __fastcall RaceDeity_dtor(RaceDeity* self, Edx) {
    self->vtable = (void**)0x004dc588;
    for (int i = 0; i < 16; i++) {
        CenterLine* cl = self->info[i].centerline;
        if (cl) {
            VFN(cl, 0, void*, int)(cl, 0, 1);
            self->info[i].centerline = 0;
        }
    }
    *g_globa = 0;
    self->vtable = (void**)0x004db9a0;
}
static void fp_race_dtor(Footprint& f, RaceDeity*, Edx) { f.replay_only = "frees the centre lines"; }
PORT_FN(0x00442e30, "RaceDeity::~RaceDeity", RaceDeity_dtor, fp_race_dtor)

// ==== RaceDeity::reset_car (0x442e80) ============================================================================
static void __fastcall RaceDeity_reset_car(RaceDeity* self, Edx, int idx) {
    RaceInfo& in = self->info[idx];
    in.prev_pos.x = 0.0f; in.prev_pos.y = 0.0f; in.prev_pos.z = 0.0f;
    in.pos.x = 0.0f; in.pos.y = 0.0f; in.pos.z = 0.0f;
    in.best_lap = 0.0f;
    in.last_lap_display = Fb(k_minus_tenth_bits);
    in.current_lap_time = 0.0f;
    in.best_speed = 0.0f;
    in.lap = 0; in.next_checkpoint = 0; in.place = 0;
    in.w68 = 0; in.w6a = 0;
    in.dlong_cookie = 0;
    in.lap_start_time = 0.0f; in.line_time = 0.0f; in.stage_start_time = 0.0f;
    in.lap_top_speed = 0.0f; in.last_lap_time = 0.0f;
    in.finished = 0;
    in.time_ahead = 0.0f;
    in.time_behind = 0.0f;
    CenterLine_reset(in.centerline, 0);
}
static void fp_reset_car(Footprint& f, RaceDeity* self, Edx, int idx) {
    fp_race_deity(f, self);
    if ((uint32_t)idx < 16 && self->info[idx].centerline) f.add(self->info[idx].centerline, sizeof(CenterLine), "centerline");
}
PORT_FN(0x00442e80, "RaceDeity::reset_car", RaceDeity_reset_car, fp_reset_car)

// ==== RaceDeity::register_car (0x442f30) =========================================================================
static void __fastcall RaceDeity_register_car(RaceDeity* self, Edx, int idx, Car* car) {
    if (!(self->num_cars > idx)) self->num_cars = idx + 1;
    RaceInfo& in = self->info[idx];
    in.car = car;
    CenterLine* cl = (CenterLine*)MemAlloc(0x70);
    if (cl) in.centerline = CenterLine_ctor(cl, 0, 0);
    else in.centerline = 0;
    car->race_state = 0;
    reset_car_o(self, 0, idx);
    if (CarMgrGetInfo(idx)->type == 3 && GhostCarIncognito()) self->num_cars--;   // an incognito ghost isn't counted
}
static void fp_register_car(Footprint& f, RaceDeity*, Edx, int, Car*) { f.replay_only = "allocates the centre line"; }
PORT_FN(0x00442f30, "RaceDeity::register_car", RaceDeity_register_car, fp_register_car)

// ==== RaceDeity::RegisterCar(int, LocalCar*) (0x442fc0) / (int, NetCar*) (0x443010) ==============================
static void __fastcall RaceDeity_RegisterCar_Local(RaceDeity* self, Edx, int idx, Car* car) {
    register_car_o(self, 0, idx, car);
    RaceInfo& in = self->info[idx];
    in.remote = 0;
    in.ghost = CarMgrGetInfo(idx)->type == 3;
}
PORT_FN(0x00442fc0, "RaceDeity::RegisterCar(LocalCar)", RaceDeity_RegisterCar_Local, fp_register_car)
static void __fastcall RaceDeity_RegisterCar_Net(RaceDeity* self, Edx, int idx, Car* car) {
    register_car_o(self, 0, idx, car);
    RaceInfo& in = self->info[idx];
    in.remote = 1;
    in.ghost = 0;
}
PORT_FN(0x00443010, "RaceDeity::RegisterCar(NetCar)", RaceDeity_RegisterCar_Net, fp_register_car)

// ==== RaceDeity::RegisterCheckPoint (0x443040) ====================================================================
static void __fastcall RaceDeity_RegisterCheckPoint(RaceDeity* self, Edx, CheckPoint*) { self->num_checkpoints++; }
static void fp_register_cp(Footprint& f, RaceDeity* self, Edx, CheckPoint*) { fp_race_deity(f, self); f.pure = true; }
PORT_FN(0x00443040, "RaceDeity::RegisterCheckPoint", RaceDeity_RegisterCheckPoint, fp_register_cp)

// ==== RaceDeity::Init (0x443050) ==================================================================================
static uint8_t __fastcall RaceDeity_Init(RaceDeity* self, Edx) {
    for (int i = 0; i < 16; i++) {
        self->place_order[i] = i;
        self->info[i].place = i + 1;
    }
    return 1;
}
static void fp_race_init(Footprint& f, RaceDeity* self, Edx) { fp_race_deity(f, self); f.pure = true; }
PORT_FN(0x00443050, "RaceDeity::Init", RaceDeity_Init, fp_race_init)

// ==== RaceDeity::Reset (0x443070) ================================================================================
static void __fastcall RaceDeity_Reset(RaceDeity* self, Edx) {
    self->player_finished = 0;
    self->start_scheduled = 0;
    SetGameState(0);
    if (!MultiEnabled()) {
        const float t = (float)(PhysicsGetTime() + D(k_start_delay));
        VFN(self, 4, void, float)(self, 0, t);                                   // StartRaceAt
    }
    for (int i = 0; i < self->num_cars; i++) {
        reset_car_o(self, 0, i);
        self->info[i].car->race_state = 0;
    }
    self->teleport_ring_index = 0;
    for (int i = 0; i < 4; i++) self->teleport_ring[i] = Fb(k_minus_1000_bits);
}
static void fp_race_reset(Footprint& f, RaceDeity* self, Edx) {
    fp_race_deity(f, self); fp_cars(f, self); fp_lines(f, self); fp_game_state(f);
}
PORT_FN(0x00443070, "RaceDeity::Reset", RaceDeity_Reset, fp_race_reset)

// ==== RaceDeity::StartRaceAt (0x443100) ==========================================================================
static void __fastcall RaceDeity_StartRaceAt(RaceDeity* self, Edx, float t) {
    cp4(&self->start_time, &t);
    const double now = PhysicsGetTime();
    self->start_scheduled = 1;
    self->countdown = (float)(D(t) - now);                                       // fsubr
}
static void fp_start_race_at(Footprint& f, RaceDeity* self, Edx, float) { fp_race_deity(f, self); }
PORT_FN(0x00443100, "RaceDeity::StartRaceAt", RaceDeity_StartRaceAt, fp_start_race_at)

// ==== variance (0x443260): x scaled by 1 +- up to n/1000, from Random(10000) ======================================
static double __cdecl variance(uint32_t x, int n) {
    const uint32_t an = (uint32_t)(n < 0 ? -n : n);                              // cdq; xor; sub
    const uint32_t r = (uint32_t)Random(10000);
    const uint32_t rem = r % (an * 1000u);                                       // div: unsigned (faults on n == 0, as the original)
    const float remf = (float)(double)rem;                                       // fild qword; fstp dword
    const float sign = n < 0 ? -1.0f : 1.0f;
    return ((D(sign) * remf + D(k_grace)) * Fb(x)) * D(k_variance_scale);
}
static void fp_variance(Footprint&, uint32_t, int) {}                             // Random is an input (fed)
PORT_FN(0x00443260, "variance", variance, fp_variance)

// ==== RaceDeity::PostRace (0x443130) =============================================================================
// Single player: every AI car still running is given the laps it has left (its skill's lap time for this track,
// varied +-0.5%, plus the first-lap allowance for lap 1) and a race-over record; then the final sort.
static void __fastcall RaceDeity_PostRace(RaceDeity* self, Edx) {
    const uint8_t no_player = WorldGetPlayerCar() + 1 == 0;
    const uint8_t multi = MultiEnabled();
    if (multi || no_player) { sort_cars_o(self, 0, final_sort_o); return; }
    PhysicsGetTime();                                                            // called, discarded
    for (int i = 0; i < self->num_cars; i++) {
        if (CarMgrGetInfo(i)->type != 1) continue;
        const RaceInfo& in = self->info[i];
        if (in.finished) continue;
        int lap = in.lap;
        if (lap == 0) lap = 1;
        uint32_t allowance;
        cp4(&allowance, &AIGetTrackInfo(AIGetTrackNumber())->first_lap_allowance);
        const uint8_t* skill = AIGetSkill(i);
        const int track = AIGetTrackNumber();
        uint32_t base;
        cp4(&base, skill + 0x98 + track * 24);
        while (self->options.num_laps >= lap) {
            const uint32_t extra = lap == 1 ? allowance : 0;
            const float t = (float)(variance_o(base, 5) + D(Fb(extra)));
            RecordNewLap(i, lap, Ub(t), k_minus_one_bits);
            lap++;
        }
        RecordRaceOver(i);
    }
    sort_cars_o(self, 0, final_sort_o);
}
static void fp_post_race(Footprint& f, RaceDeity*, Edx) { f.replay_only = "records every AI car's remaining laps"; }
PORT_FN(0x00443130, "RaceDeity::PostRace", RaceDeity_PostRace, fp_post_race)

// ==== RaceDeity::Update (0x4432e0) ===============================================================================
// Per tick: single player, once the player has finished, wait for the cars ranked near the player (lapped[])
// to finish, or 100000 s; the scheduled start (sound, state 1, the cars released); the countdown (state 0, a
// tick sound each second); the places; the countdown value.
static void __fastcall RaceDeity_Update(RaceDeity* self, Edx) {
    if (PhysReplayPlayMode()) return;
    const int state = GetGameState();
    const float now = (float)PhysicsGetTime();
    if (state == 1 && !MultiEnabled() && WorldGetPlayerCar() >= 0) {
        if (self->player_finished) {
            if (!(D(self->finish_deadline) >= D(now))) {
                uint8_t all = 1;
                for (int i = 0; CarMgrCount() > i; i++) {
                    if (!self->info[i].ghost && g_lapped[i]) {
                        if (!VFN(self, 0x40, uint8_t, int)(self, 0, i)) { all = 0; break; }   // CarIsFinished
                    }
                }
                if (all) SetGameState(3);
                else self->finish_deadline = (float)(D(now) + D(k_grace));
            }
        } else if (VFN(self, 0x40, uint8_t, int)(self, 0, WorldGetPlayerCar())) {
            self->player_finished = 1;
            self->finish_deadline = (float)(D(now) + D(k_grace));
            int rank = CarMgrGetInfo(WorldGetPlayerCar())->rank;
            if (CarMgrCount() > 0) {
                rank--;
                int i = 0;
                do {
                    const CarMgrInfo* ci = CarMgrGetInfo(i);
                    g_lapped[i] = 1;
                    if (ci->rank < rank) g_lapped[i] = 0;
                    i++;
                } while (CarMgrCount() > i);
            }
        }
    }
    const uint8_t scheduled = self->start_scheduled;
    if (scheduled && !(D(self->start_time) > D(now))) {
        if (state == 0) {
            Sound_Toss(k_snd_start, 1);
            SetGameState(1);
            for (int i = 0; i < self->num_cars; i++) {
                RaceInfo& in = self->info[i];
                cp4(&in.lap_start_time, &self->start_time);
                cp4(&in.stage_start_time, &self->start_time);
                in.car->race_state = 2;
            }
        }
    } else if (state != 3) {
        if (scheduled && !PhysicsIsPaused()) {
            const double to_go = cifmod_one(D(self->start_time) - D(now));
            const double last = cifmod_one(D(self->countdown));
            if (!(last >= to_go)) Sound_Toss(k_snd_tick, 1);                      // a new whole second
        }
        SetGameState(0);
    }
    if (!PhysicsIsPaused() && state != 0) place_cars_o(self, 0);
    self->countdown = (float)(D(self->start_time) - D(now));
}
// The two sounds are decided by the same reads the function makes, so the footprint knows the ticks that play
// one and leaves them to the replay; every other tick is bounded: the deity, the cars (race_state at the
// start), the game state; lapped[] is a physics static.
static void fp_race_update(Footprint& f, RaceDeity* self, Edx) {
    if (!PhysReplayPlayMode() && self->start_scheduled) {
        const int state = GetGameState();
        const float now = (float)PhysicsGetTime();
        if (!(D(self->start_time) > D(now))) {
            if (state == 0) { f.replay_only = "the start sound"; return; }
        } else if (state != 3 && !PhysicsIsPaused()) {
            const double to_go = cifmod_one(D(self->start_time) - D(now));
            const double last = cifmod_one(D(self->countdown));
            if (!(last >= to_go)) { f.replay_only = "the countdown tick sound"; return; }
        }
    }
    fp_race_deity(f, self); fp_cars(f, self); fp_game_state(f);
}
PORT_FN(0x004432e0, "RaceDeity::Update", RaceDeity_Update, fp_race_update)

// ==== RaceDeity::record_newlap (0x443520) / record_newstage (0x4435b0) ===========================================
static void __fastcall RaceDeity_record_newlap(RaceDeity* self, Edx, int idx, int lap, uint32_t time, uint32_t speed) {
    const uint8_t remote = self->info[idx].remote;
    if (remote) RecordUnofficialNewLap(idx, lap, time, speed);
    else RecordNewLap(idx, lap, time, speed);
    if (!remote && MultiEnabled()) {
        DeityPacket p;
        memset(&p, 0, sizeof p);                     // (the original leaves its pad byte as the stack had it)
        cp4(&p.a, &time); cp4(&p.b, &speed);
        p.car = (uint8_t)idx; p.lap = (uint8_t)lap; p.kind = 0xfa;
        MultiDeityCast(&p, 12);
    }
}
static void fp_record_newlap(Footprint& f, RaceDeity* self, Edx, int idx, int, uint32_t, uint32_t) {
    if ((uint32_t)idx < 16 && !self->info[idx].remote && MultiEnabled()) { f.replay_only = "MultiDeityCast"; return; }
    fp_records(f);
}
PORT_FN(0x00443520, "RaceDeity::record_newlap", RaceDeity_record_newlap, fp_record_newlap)
static void __fastcall RaceDeity_record_newstage(RaceDeity* self, Edx, int idx, int lap, int stage, uint32_t time) {
    const uint8_t remote = self->info[idx].remote;
    if (remote) RecordUnofficialStage(idx, stage, lap, time);                     // (car, stage, lap, time)
    else RecordStage(idx, stage, lap, time);
    if (!remote && MultiEnabled()) {
        DeityPacket p;
        memset(&p, 0, sizeof p);
        cp4(&p.a, &time); p.b = Fb(k_minus_one_bits);
        p.car = (uint8_t)idx; p.lap = (uint8_t)lap; p.kind = (uint8_t)stage;
        MultiDeityCast(&p, 12);
    }
}
static void fp_record_newstage(Footprint& f, RaceDeity* self, Edx, int idx, int, int, uint32_t) {
    if ((uint32_t)idx < 16 && !self->info[idx].remote && MultiEnabled()) { f.replay_only = "MultiDeityCast"; return; }
    fp_records(f);
}
PORT_FN(0x004435b0, "RaceDeity::record_newstage", RaceDeity_record_newstage, fp_record_newstage)

// ==== RaceDeity::UpdateCar (0x443640) ============================================================================
// The car's lap-line point (front axle) into the world; the centre line's cursor advances (a crossing when a
// checkpoint is passed); the lap timer; on a crossing: a stage record, and at the start/finish line a lap
// (lap time, top speed, the ghost, best times, the records, the finish after the last lap).
static void __fastcall RaceDeity_UpdateCar(RaceDeity* self, Edx, int idx, Car* car) {
    RaceInfo& in = self->info[idx];
    const float* M = car->frame.rot.m;
    const P3& o = car->front_axle;
    float x = (float)((D(M[3]) * o.y + D(M[0]) * o.x) + D(M[6]) * o.z);
    float y = (float)((D(M[7]) * o.z + D(M[4]) * o.y) + D(M[1]) * o.x);
    float z = (float)((D(M[8]) * o.z + D(M[5]) * o.y) + D(M[2]) * o.x);
    x = (float)(D(car->frame.pos.x) + x);
    y = (float)(D(car->frame.pos.y) + y);
    z = (float)(D(car->frame.pos.z) + z);
    cp4(&in.pos.x, &x); cp4(&in.pos.y, &y); cp4(&in.pos.z, &z);
    Point2D pt;
    cp4(&pt.x, &in.pos.x); cp4(&pt.z, &in.pos.z);
    cp4(&in.lap_top_speed, &car->lap_top_speed);
    const int stage = CenterLine_get_next_checkpoint(in.centerline, 0);        // the checkpoint being approached
    const uint8_t crossed = CenterLine_update(in.centerline, 0, &pt, idx);
    in.next_checkpoint = CenterLine_get_next_checkpoint(in.centerline, 0);
    in.dlong_cookie = CenterLine_get_car_dlong_cookie(in.centerline, 0);
    if (in.finished) return;                                                     // (prev_pos is not advanced)
    const float now = (float)PhysicsGetTime();
    in.f60 = 0;
    in.f64 = k_one_bits;
    const uint8_t racing = GetGameState() == 1;
    if (racing) {
        if (in.ghost) {
            in.current_lap_time = (float)(D(now) - D(in.line_time));
            in.lap = self->info[0].lap;                                          // a ghost's lap is car 0's
        } else in.current_lap_time = (float)(D(now) - D(in.lap_start_time));
    } else in.current_lap_time = 0.0f;
    if (!in.ghost && crossed) {
        if (in.lap != 0) {
            const float dt = (float)(D(now) - D(in.stage_start_time));
            record_newstage_o(self, 0, idx, in.lap, stage, Ub(dt));
            cp4(&in.stage_start_time, &now);
        }
        if (in.next_checkpoint == 1) {                                           // the start/finish line
            const int lap = in.lap + 1;
            in.lap = lap;
            if (lap > 1) {
                in.last_lap_time = (float)(D(now) - D(in.lap_start_time));
                cp4(&in.last_lap_display, &in.last_lap_time);
                cp4(&in.lap_top_speed, &car->lap_top_speed);
                const int ticks = x87_ftol((D(now) - D(in.line_time)) * D(k_one_over_dt) + 0.5f);
                GhostNewBestLap(idx, ticks);
                if (!in.remote) set_bests_o(self, 0, &in);
                car->lap_top_speed = 0.0f;
                cp4(&in.lap_start_time, &now);
                record_newlap_o(self, 0, idx, in.lap - 1, Ub(in.last_lap_time), Ub(in.lap_top_speed));
                if (self->num_laps < in.lap) {
                    if (in.remote) RecordUnofficialRaceOver(idx);
                    else RecordRaceOver(idx);
                    car->race_state = 3;
                    in.finished = 1;
                }
            }
            cp4(&in.line_time, &now);
        }
    }
    do_wrongway_o(self, 0, idx, car);
    if (racing) do_cheater_o(self, 0, idx, car);
    cp12(&in.prev_pos, &in.pos);
}
// A completed lap of a ghostable car feeds the ghost's replay buffers, and a lap or stage of a local car in
// multiplayer is broadcast: those cars are left to the replay. Otherwise: the deity, the car, its centre line
// and the record lists.
static void fp_race_update_car(Footprint& f, RaceDeity* self, Edx, int idx, Car* car) {
    if ((uint32_t)idx >= 16) { f.replay_only = "car index out of range"; return; }
    const RaceInfo& in = self->info[idx];
    if (!in.ghost && !in.finished) {
        if (is_ghostable_car(idx)) { f.replay_only = "a lap would feed the ghost car"; return; }
        if (!in.remote && MultiEnabled()) { f.replay_only = "MultiDeityCast"; return; }
    }
    fp_race_deity(f, self);
    f.object(car, "car");
    if (in.centerline) f.add(in.centerline, sizeof(CenterLine), "centerline");
    fp_records(f);
}
PORT_FN(0x00443640, "RaceDeity::UpdateCar", RaceDeity_UpdateCar, fp_race_update_car)

// ==== CanTeleport (0x443920) =====================================================================================
// A racing car that isn't being lowered, if it's the player's, upside down, off the line, wide of it, or damaged.
static uint8_t __cdecl CanTeleport(Car* car) {
    const uint8_t upside_down = Ub(car->frame.rot.m[4]) > k_neg_zero_bits;      // up.y negative (not -0)
    const uint8_t lowering = car->lowering;
    const CenterLine* cl = (*g_globa)->info[car->car_index].centerline;
    const uint8_t off_line = cl->on_track == 0;
    const double half_width = (cl->line ? D(cl->line->width) : D(20.0f)) * 0.5f;
    const uint8_t wide = fabs(D(cl->lateral)) > half_width;                     // test ah,0x41; sete
    const uint8_t damaged = Car_AnyWheelsAreDamaged(car, 0);
    const uint8_t player = CarMgrGetInfo(car->car_index)->type == 1;
    const uint8_t racing = car->race_state >= 2;
    if (!lowering && racing && (player || upside_down || off_line || wide || damaged)) return 1;
    return 0;
}
static void fp_can_teleport(Footprint&, Car*) {}
PORT_FN(0x00443920, "CanTeleport", CanTeleport, fp_can_teleport)

// ==== HermiteEval (0x443c10): out = h00 P0 + h10 T0 + h01 P1 + h11 T1 ============================================
static void __cdecl HermiteEval(Point2D* out, const Point2D* p0, const Point2D* p1, const Point2D* t0, const Point2D* t1, float t) {
    const double h00 = ((D(t) * 2.0f - 3.0f) * t) * t + 1.0f;
    const double h01 = ((D(t) * -2.0f + 3.0f) * t) * t;
    const double h10 = ((D(t) - 2.0f) * t + 1.0f) * t;
    const double h11 = ((D(t) - 1.0f) * t) * t;
    out->x = (float)(((D(t0->x) * h10 + D(p1->x) * h01) + D(t1->x) * h11) + D(p0->x) * h00);
    out->z = (float)(((D(t0->z) * h10 + D(t1->z) * h11) + D(p1->z) * h01) + D(p0->z) * h00);
}
static void fp_hermite(Footprint& f, Point2D* out, const Point2D*, const Point2D*, const Point2D*, const Point2D*, float) {
    f.add(out, 8, "out"); f.pure = true;
}
PORT_FN(0x00443c10, "HermiteEval", HermiteEval, fp_hermite)

// ==== RaceDeity::TeleportToLine (0x443a00) =======================================================================
// Puts the car back on the centre line at its progress, at least 7 m from the last four teleport points (else
// 7 m back, wrapping to the end of the lap), offset to one side of the line by half the corridor less 2.5 m,
// at the terrain height + 1.525, heading along the line.
static uint8_t __fastcall RaceDeity_TeleportToLine(RaceDeity* self, Edx, int idx, Car* car) {
    if (!CanTeleport_o(car)) return 0;
    RaceInfo& in = self->info[idx];
    float m = (float)CenterLine_convert_cookie_to_meters(in.centerline, 0, in.dlong_cookie);
    for (;;) {
        uint8_t clear = 1;
        for (uint32_t k = 0; k < 4; k++) {
            if (!(fabs(D(m) - D(self->teleport_ring[k])) >= D(k_teleport_gap))) {
                m = (float)(D(m) - D(k_teleport_gap));
                if (!(D(m) >= 0.0f)) m = (float)(VFN(self, 0x54, double)(self, 0) - D(k_teleport_gap));   // GetMaxDLong
                clear = 0;
                break;
            }
        }
        if (clear) break;
    }
    const uint32_t mb = Ub(m);
    self->teleport_ring[self->teleport_ring_index] = m;
    self->teleport_ring_index = (self->teleport_ring_index + 1) & 3;
    ILinePos lp;
    CenterLine_convert_meters_to_ilpos(in.centerline, 0, &lp, mb);
    const ILSeg* seg = lp.seg;
    const ILSeg* nxt = seg->next;
    const uint32_t tb = Ub(lp.t);
    Point2D pt;
    HermiteEval_o(&pt, &seg->p, &nxt->p, &seg->dir, &nxt->dir, tb);
    Point2D tan;
    ILSeg_QuickTan(seg, 0, &tan, tb);
    const float ntx = -tan.x;                                                     // fchs
    const double len = x87_sqrt(D(ntx) * ntx + D(tan.z) * tan.z);
    const float nx = (float)(D(tan.z) / len);
    const float nz = (float)(D(ntx) / len);
    float d = (float)(D(seg->corridor_half_width) * 0.5f - D(k_lane));
    if (seg->field_2c == 0) d = -d;
    pt.x = (float)(D(d) * nx + pt.x);
    pt.z = (float)(D(d) * nz + pt.z);
    P3 pos;
    pos.y = 0.0f;
    cp4(&pos.x, &pt.x);
    cp4(&pos.z, &pt.z);
    pos.y = (float)(TerrainGetHeight2(Ub(pt.x), Ub(pt.z)) + D(k_teleport_height));
    VFN(car, 0x40, void, const P3*, const Point2D*)(car, 0, &pos, &tan);         // Car::Teleport
    return 1;
}
static void fp_race_teleport(Footprint& f, RaceDeity* self, Edx, int, Car* car) {
    fp_race_deity(f, self);
    f.object(car, "car");
}
PORT_FN(0x00443a00, "RaceDeity::TeleportToLine", RaceDeity_TeleportToLine, fp_race_teleport)

// ==== RaceDeity::final_sort (0x443cc0): by race time; no time -> by best lap; ghosts last ========================
static int __fastcall RaceDeity_final_sort(RaceDeity* self, Edx, RaceInfo* a, RaceInfo* b) {
    if (a->ghost) return 1;
    if (b->ghost) return -1;
    const int ia = (int)((uint8_t*)a - (uint8_t*)self - 0x38) / 0x74;
    const int ib = (int)((uint8_t*)b - (uint8_t*)self - 0x38) / 0x74;
    const float ta = (float)RecordGetRaceTime(ia);
    const float tb = (float)RecordGetRaceTime(ib);
    const uint8_t no_a = Ub(ta) > k_neg_zero_bits, no_b = Ub(tb) > k_neg_zero_bits;
    if (!no_a) {
        if (!no_b) {
            if (!(D(tb) < D(ta) || D(tb) > D(ta))) return 0;                       // test ah,0x40
            return !(D(tb) >= D(ta)) ? 1 : -1;                                     // test ah,1
        }
        return -1;
    }
    if (!no_b) return 1;
    return !(D(b->best_lap) > D(a->best_lap)) ? 1 : -1;                          // test ah,0x41
}
static void fp_final_sort(Footprint&, RaceDeity*, Edx, RaceInfo*, RaceInfo*) {}    // RecordGetRaceTime only reads
PORT_FN(0x00443cc0, "RaceDeity::final_sort", RaceDeity_final_sort, fp_final_sort)

// ==== RaceDeity::race_sort (0x443dd0): finished cars by place, then by lap, then by progress; ghosts last ========
static int __fastcall RaceDeity_race_sort(RaceDeity*, Edx, RaceInfo* a, RaceInfo* b) {
    const uint8_t ga = a->ghost;
    if (!ga && !b->ghost) {
        if (a->finished && b->finished) return a->place - b->place;
        if (b->lap != a->lap) return b->lap > a->lap ? 1 : -1;
        return (int)(b->dlong_cookie - a->dlong_cookie);
    }
    if (a->dlong_cookie != b->dlong_cookie) return (int)(b->dlong_cookie - a->dlong_cookie);
    return ga ? 1 : -1;
}
static void fp_race_sort(Footprint& f, RaceDeity*, Edx, RaceInfo*, RaceInfo*) { f.pure = true; }
PORT_FN(0x00443dd0, "RaceDeity::race_sort", RaceDeity_race_sort, fp_race_sort)

// ==== RaceDeity::static_sort_f (0x443e50): qsort's comparator, through the member function pointer =============
static int __cdecl RaceDeity_static_sort_f(const void* pa, const void* pb) {
    RaceInfo* rb = &(*g_globa)->info[*(const int*)pb];
    RaceInfo* ra = &(*g_globa)->info[*(const int*)pa];
    return (*g_sort_fn)(*g_globa, 0, ra, rb);
}
static void fp_static_sort_f(Footprint&, const void*, const void*) {}
PORT_FN(0x00443e50, "RaceDeity::static_sort_f", RaceDeity_static_sort_f, fp_static_sort_f)

// ==== RaceDeity::sort_cars (0x443ea0) ============================================================================
// (the member function pointer is a plain code address; taken as one so the fuzz harness's templates accept it)
static void __fastcall RaceDeity_sort_cars(RaceDeity* self, Edx, uint32_t fn) {
    *g_sort_fn = (SortFn)fn;
    qsort_o(self->place_order, (unsigned)self->num_cars, 4, static_sort_f_o);
    *g_sort_fn = 0;
}
static void fp_sort_cars(Footprint& f, RaceDeity* self, Edx, uint32_t) { fp_race_deity(f, self); }
PORT_FN(0x00443ea0, "RaceDeity::sort_cars", RaceDeity_sort_cars, fp_sort_cars)

// ==== RaceDeity::place_cars (0x443ee0) ===========================================================================
// Sorts, then for each adjacent pair the time between them (the centre lines' time_between plus the leader's
// best lap per lap of difference), and each car's place.
static void __fastcall RaceDeity_place_cars(RaceDeity* self, Edx) {
    sort_cars_o(self, 0, race_sort_o);
    if (self->num_cars > 1) {
        int p = 1;
        do {
            const int b = self->place_order[p], a = self->place_order[p - 1];
            RaceInfo& A = self->info[a];
            RaceInfo& B = self->info[b];
            const uint32_t ca = CenterLine_get_car_dlong_cookie(A.centerline, 0);
            const uint32_t cb = CenterLine_get_car_dlong_cookie(B.centerline, 0);
            const int laps = A.lap - (ca < cb ? 1 : 0) - B.lap;
            const double gap = CenterLine_time_between(B.centerline, 0, A.centerline);
            const float t = (float)(D(A.best_lap) * (double)laps + gap);         // fimul; faddp
            p++;
            B.time_behind = t;
            B.ahead_index = a;
            A.time_ahead = t;
            A.behind_index = b;
        } while (self->num_cars > p);
    }
    for (int p = 0; p < self->num_cars; p++) self->info[self->place_order[p]].place = p + 1;
}
static void fp_place_cars(Footprint& f, RaceDeity* self, Edx) { fp_race_deity(f, self); fp_lines(f, self); }
PORT_FN(0x00443ee0, "RaceDeity::place_cars", RaceDeity_place_cars, fp_place_cars)

// ==== RaceDeity::GetMessage (0x444000) ===========================================================================
static void __fastcall RaceDeity_GetMessage(RaceDeity* self, Edx, PhysicsPacket* p) {
    for (int i = 0; i < self->num_cars; i++) memcpy(&p->cars[i], &self->info[i].lap, sizeof(RaceCarMessage));
    if (GetGameState() != 0) { p->countdown = 0; return; }
    if (self->start_scheduled) {
        const double now = PhysicsGetTime();
        p->countdown = x87_ftol(D(self->start_time) - now) + 1;
    } else p->countdown = -1;
}
static void fp_race_get_message(Footprint& f, RaceDeity*, Edx, PhysicsPacket* p) { f.add(p, sizeof(PhysicsPacket), "packet"); }
PORT_FN(0x00444000, "RaceDeity::GetMessage", RaceDeity_GetMessage, fp_race_get_message)

// ==== RaceDeity::set_bests (0x444090) ============================================================================
static void __fastcall RaceDeity_set_bests(RaceDeity*, Edx, RaceInfo* in) {
    if (I(in->best_lap) > (int32_t)k_tenth_bits) {
        const double lap = D(in->last_lap_time), best = D(in->best_lap);
        in->best_lap = (float)(best > lap ? lap : best);                         // test ah,0x41
    } else cp4(&in->best_lap, &in->last_lap_time);
    if (I(in->best_speed) > (int32_t)k_tenth_bits) {
        const double spd = D(in->lap_top_speed), best = D(in->best_speed);
        in->best_speed = (float)(!(best >= spd) ? spd : best);                   // test ah,1
    } else cp4(&in->best_speed, &in->lap_top_speed);
}
static void fp_set_bests(Footprint& f, RaceDeity*, Edx, RaceInfo* in) { f.add(in, sizeof(RaceInfo), "RaceInfo"); f.pure = true; }
PORT_FN(0x00444090, "RaceDeity::set_bests", RaceDeity_set_bests, fp_set_bests)

// ==== RaceDeity::NetPacket (0x4440f0): a remote car's lap or stage ================================================
static void __fastcall RaceDeity_NetPacket(RaceDeity* self, Edx, const DeityPacket* pk) {
    const uint8_t kind = pk->kind;
    if (kind == 0xfa) {
        RecordNewLap(pk->car, pk->lap, Ub(pk->a), Ub(pk->b));
        RaceInfo& in = self->info[pk->car];
        cp4(&in.last_lap_display, &pk->a);
        cp4(&in.last_lap_time, &pk->a);
        cp4(&in.lap_top_speed, &pk->b);
        set_bests_o(self, 0, &in);
        if ((int)pk->lap == self->num_laps) RecordRaceOver(pk->car);
    } else RecordStage(pk->car, kind, pk->lap, Ub(pk->a));                        // the kind is the stage
}
static void fp_net_packet(Footprint& f, RaceDeity* self, Edx, const DeityPacket*) { fp_race_deity(f, self); fp_records(f); }
PORT_FN(0x004440f0, "RaceDeity::NetPacket", RaceDeity_NetPacket, fp_net_packet)

// ==== the getters ================================================================================================
static uint8_t __fastcall RaceDeity_CarIsFinished(RaceDeity* self, Edx, int i) { return self->info[i].finished; }
static void fp_race_getter_i(Footprint&, RaceDeity*, Edx, int) {}
PORT_FN(0x00444190, "RaceDeity::CarIsFinished", RaceDeity_CarIsFinished, fp_race_getter_i)
static int __fastcall RaceDeity_GetCarByPlace(RaceDeity* self, Edx, int place) { return self->place_order[place]; }
static void fp_race_getter_i_pure(Footprint& f, RaceDeity*, Edx, int) { f.pure = true; }
PORT_FN(0x004441b0, "RaceDeity::GetCarByPlace", RaceDeity_GetCarByPlace, fp_race_getter_i_pure)
static int __fastcall RaceDeity_GetCurrentLap(RaceDeity* self, Edx, int i) { return self->info[i].lap; }
PORT_FN(0x004441c0, "RaceDeity::GetCurrentLap", RaceDeity_GetCurrentLap, fp_race_getter_i)
static int __fastcall RaceDeity_GetNumLaps(RaceDeity* self, Edx) { return self->num_laps; }
static void fp_race_getter_pure(Footprint& f, RaceDeity*, Edx) { f.pure = true; }
PORT_FN(0x004441e0, "RaceDeity::GetNumLaps", RaceDeity_GetNumLaps, fp_race_getter_pure)
static int __fastcall RaceDeity_GetNumCheckpoints(RaceDeity* self, Edx) { return self->num_checkpoints; }
PORT_FN(0x004441f0, "RaceDeity::GetNumCheckpoints", RaceDeity_GetNumCheckpoints, fp_race_getter_pure)
static double __fastcall RaceDeity_GetCurrentLapStart(RaceDeity* self, Edx, int i) { return self->info[i].lap_start_time; }
PORT_FN(0x00444200, "RaceDeity::GetCurrentLapStart", RaceDeity_GetCurrentLapStart, fp_race_getter_i)
static uint32_t __fastcall RaceDeity_GetDLong(RaceDeity* self, Edx, int i) { return self->info[i].dlong_cookie; }
PORT_FN(0x00444220, "RaceDeity::GetDLong", RaceDeity_GetDLong, fp_race_getter_i)
static double __fastcall RaceDeity_GetMaxDLong(RaceDeity* self, Edx) { return self->info[0].centerline->max_dlong; }
static void fp_race_getter(Footprint&, RaceDeity*, Edx) {}
PORT_FN(0x00444240, "RaceDeity::GetMaxDLong", RaceDeity_GetMaxDLong, fp_race_getter)
static double __fastcall RaceDeity_ConvertDLong(RaceDeity* self, Edx, uint32_t cookie) {
    return CenterLine_convert_cookie_to_meters(self->info[0].centerline, 0, cookie);
}
static void fp_race_convert_dlong(Footprint&, RaceDeity*, Edx, uint32_t) {}
PORT_FN(0x00444250, "RaceDeity::ConvertDLong", RaceDeity_ConvertDLong, fp_race_convert_dlong)
