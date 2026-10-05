// wld_world.cpp -- M3 stage "the world", group W2: loading the world, rewritten. world:world.obj (the race's
// begin / end / per-frame update, the world-file parser that turns track.obt into objects and physics creation
// records, the start line, the grid slots, the object lists, the small getters), world:carmgr.obj (the car
// manager: who each car is, its status line, its last CarInfo) and world:event.obj (the table of handlers the
// physics' event queue is dispatched to).
//
// What the parser does. WorldBeginCommon copies the World (the race description the menus built: track, car
// list, options) into its own static, loads the track, begins every part of the race, then parse_world reads the
// track's `track.obt` string table (a STAB resource: row 0 the compiler's own "out\track.txt", then one record
// per row, `;` starting a comment) line by line: "end" stops, "map"/"ai" are skipped, "obj <kind> ..." goes to
// parse_object, which hands each kind to its parser:
//   obj car X, Z                   parse_car: the next entry of the car list; its .cf file loaded (CarFileLoad);
//                                  placed on the centre line (set_car_frame_from_dlatlong: 28 m + 15 m per slot
//                                  back from the line's end, alternately 4 m right and left) or, when that fails
//                                  or byte 0x4f4368 is set, at (X, 0, Z) through the start-line frame, 2 m above
//                                  the ground; a CarObject / GhostCarObject (0x844 bytes) made from the CarData
//   obj obstacle S M X,Z:R MASS    parse_obstacle: S = ball | cube | prism, M a model whose extents give its size
//                                  and inertia (size^2 * 10.749798 * mass); a ModelObject (0x40) over OBST data
//   obj wobble S ID                parse_wobble: S = pole | flap; a WobbleObject over WOBL data
//   obj static S X,Y,Z A,B,C W,H,D parse_static: S = box; the axis-angle (A,B,C) rotation (angle = its length),
//                                  sent straight to the physics (PhysicsCreate, STAT data, no world object)
//   obj checkpoint S X,Z X,Z       parse_checkpoint: a CHKP gate; a visible ModelObject (checkpt1.mod) when the
//                                  options show checkpoints, else straight to the physics
// then create_remaining_cars makes any cars the file had no slot for ("obj car 0, 0"). The objects' constructors
// (other groups) register them with the physics (PhysicsCreate), so what this stage produces is the object lists
// (0x5522d8 graphics objects, 0x5512a0 world objects, sort_objects orders the first by each object's key) and,
// through them, the creation stream PhysTaskBegin turns into phobs (hook/phys_task.cpp, create_phob).
//
// Threads (docs/PORTING.md): everything here runs on the MAIN thread -- WorldBegin*/End* from the race's set-up,
// WorldUpdate / the getters every frame, the parser inside WorldBeginCommon, the car manager's writers from the
// world objects' constructors and WorldUpdate. CarMgrCount / CarMgrGetInfo (and the getters) are also read from
// the physics thread; they write nothing. So every footprint lists the world statics its function writes (none of
// them are among the automatically saved physics/AI globals). What loads files or resources (the track, the string
// table, car files, models, the centre line), allocates, begins or ends the race's parts, takes the shared packet,
// or runs code that can't be bounded (the event handlers, every object's virtuals) is replay_only.
//
// Skipped: WorldDraw (0x462200), draw_mirror (0x462390), WorldDraw2D (0x462570, takes a gxCanvas) and
// draw_physics_tris (0x462880): they draw (the graphics stage). ASSERT_MSG (0x462750, 0x464470) and EventEnd
// (0x4692d0) are bare `ret`s (still called, by address, where the originals call them). The $E initialisers.
//
// Written from the v1.0 disassembly: every call in the original's order, by address (this group's own functions
// too, so the hooked rewrite or the original is what runs), memory moved with integer instructions kept as bit
// copies, register values as double and stored values as float. The x87 runs at whatever precision the main
// thread has; the rewrites do the same operations at the same points, so they match at either.
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t Ub(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
// rep movsd: n dwords, forward (a bit copy)
static __forceinline void movsd(void* dst, const void* src, uint32_t n) {
    __asm { mov edi, dst
            mov esi, src
            mov ecx, n
            rep movsd }
}
// fld dword; fstp dword -- a float moved through the FPU (a signalling NaN comes out quiet), as the original does
static __forceinline void fcopy(float* dst, const float* src) {
    __asm { mov eax, src
            fld dword ptr [eax]
            mov eax, dst
            fstp dword ptr [eax] }
}
// the inlined strcpy (repne scasb; rep movsd; rep movsb): strlen + 1 bytes, forward
static __forceinline void str_copy(char* dst, const char* src) { memcpy(dst, src, strlen(src) + 1); }

// ---- layouts -------------------------------------------------------------------------------------------------------
namespace {

struct PhobData { uint32_t type; int32_t size; };                // a physics creation record's header (FourCC, bytes)
struct Point2D { float x, z; };

// GameOptions (0x28, World +0xcac; the static copy at 0x554024)
struct GameOptions {
    int32_t realism;                   // +0x00 (PhysicsBegin)
    uint32_t _04;
    int32_t reflection;                // +0x08 WorldShowReflection: 1 = on
    uint32_t _0c[3];
    int32_t show_checkpoints;          // +0x18 parse_checkpoint: a visible ModelObject instead of a bare CHKP phob
    uint32_t _1c[2];
    uint8_t damage;                    // +0x24 (PhysicsBegin)
    uint8_t reverse;                   // +0x25 get_start_line (and CenterLine's load): the track run backwards
    uint8_t _26[2];
};
static_assert(sizeof(GameOptions) == 0x28 && offsetof(GameOptions, reverse) == 0x25, "GameOptions");

// CarListEntry (0xc8): one car of the race
struct CarListEntry {
    int32_t subtype;                   // +0x00 0 player (PCAR), 1 AI (ACAR), 2 network (NCAR), 3 ghost (GCAR)
    uint8_t _04[0x0d];
    char car[0x23];                    // +0x11 the car's name: "<car>.cf" is its file
    uint8_t setup[0x88];               // +0x34 CarSetupData (CarFileLoad's second argument)
    float wheel_lock;                  // +0xbc setup_cars: must lie in [0.01, 1.01] (compared as bits)
    uint32_t _c0;
    uint8_t ball, _c5[3];              // +0xc4 create_ball for this car
};
static_assert(sizeof(CarListEntry) == 0xc8 && offsetof(CarListEntry, setup) == 0x34 &&
              offsetof(CarListEntry, wheel_lock) == 0xbc && offsetof(CarListEntry, ball) == 0xc4, "CarListEntry");
struct CarList { CarListEntry e[16]; int32_t count; };           // count at +0xc80
static_assert(offsetof(CarList, count) == 0xc80, "CarList");

// World (0xcd4, version 3): what WorldBegin is handed; copied whole to 0x553378
struct World {
    int32_t version;                   // +0x000 3 (World::is_valid_version)
    int32_t size;                      // +0x004 0xcd4
    char track[0x20];                  // +0x008 the track's name (TrackLoad, GetTrackNumber, AIBegin)
    CarList cars;                      // +0x028 (count at +0xca8)
    GameOptions options;               // +0xcac
};
static_assert(sizeof(World) == 0xcd4 && offsetof(World, options) == 0xcac, "World");

// CarData (0x1e4): a car's PhobData, filled by CarFileLoad from the .cf file and the setup, then completed here
struct CarData {
    PhobData h;                        // +0x000 PCAR / ACAR / NCAR / GCAR, 0x1e4
    uint8_t _008[0x10];
    Frame frame;                       // +0x018 where it starts
    uint8_t _048[0x194 - 0x48];
    int32_t car;                       // +0x194 its index in the car list (set before CarFileLoad, re-read after)
    char name[0x48];                   // +0x198 (CarFileLoad: the file's base name)
    int32_t ghost_target;              // +0x1e0 GCAR: GhostGetTarget()
};
static_assert(sizeof(CarData) == 0x1e4 && offsetof(CarData, car) == 0x194 && offsetof(CarData, ghost_target) == 0x1e0, "CarData");

// the creation records the other parsers build (PhobData first)
struct ObstacleData {                  // 'OBST', 0x38
    PhobData h;
    float mass;                        // +0x08
    float inertia[3];                  // +0x0c size.y^2, size.z^2, size.x^2, each * 10.749798 * mass
    P3 pos;                            // +0x18 (x, ground - model min y + 4, z)
    float rot;                         // +0x24 the ":R" field
    P3 size;                           // +0x28 the model's extents
    int32_t shape;                     // +0x34 0 ball, 1 cube, 2 prism
};
static_assert(sizeof(ObstacleData) == 0x38, "ObstacleData");
struct BallData {                      // 'BALL', 0x38 (create_ball)
    PhobData h;
    float mass;                        // +0x08 3000
    float inertia[3];                  // +0x0c 5000 x3
    P3 pos;                            // +0x18 (0, ground at 0,0 + 5000, 0)
    float radius;                      // +0x24 18
    float _28;                         // +0x28 5000
    float _2c;                         // +0x2c 0.6
    float _30;                         // +0x30 5
    int32_t car;                       // +0x34
};
static_assert(sizeof(BallData) == 0x38, "BallData");
struct WobbleData {                    // 'WOBL', 0x20
    PhobData h;
    float _08;                         // +0x08 10000
    float _0c[3];                      // +0x0c 13140 x3
    int32_t id;                        // +0x18
    int32_t kind;                      // +0x1c 0 pole, 1 flap
};
static_assert(sizeof(WobbleData) == 0x20, "WobbleData");
struct StaticData {                    // 'STAT', 0x48
    PhobData h;
    M3 rot;                            // +0x08 from the axis-angle
    P3 pos;                            // +0x2c
    int32_t kind;                      // +0x38 0 box
    P3 extent;                         // +0x3c
};
static_assert(sizeof(StaticData) == 0x48, "StaticData");
struct CheckPointData {                // 'CHKP', 0x20
    PhobData h;
    P3 a, b;                           // +0x08, +0x14 (x, ground, z)
};
static_assert(sizeof(CheckPointData) == 0x20, "CheckPointData");

// CarMgrInfo (0x170): 16 at 0x554090
struct CarMgrInfo {
    int32_t type;                      // +0x000 CarMgrRegisterCar's last argument (0 player, 2 network...)
    uint8_t human;                     // +0x004 type 0, or type 2 and MultiCarIsHuman
    char name[0x0d];                   // +0x005 (strcpy, unbounded)
    char driver[0x20];                 // +0x012 (strcpy, unbounded)
    char status[0x102];                // +0x032 _CarMgrGetStatusBuf
    const void* msg;                   // +0x134 the car's CarMessage
    uint8_t info[0x38];                // +0x138 the last CarInfo (CarMgrUpdateCar)
};
static_assert(sizeof(CarMgrInfo) == 0x170 && offsetof(CarMgrInfo, status) == 0x32 && offsetof(CarMgrInfo, msg) == 0x134 &&
              offsetof(CarMgrInfo, info) == 0x138, "CarMgrInfo");

// the event table (16 at 0x557ee0)
typedef int(__cdecl* EventHandler_t)(const void*);
struct EventEntry { EventHandler_t fn; uint8_t type, _5[3]; };
static_assert(sizeof(EventEntry) == 8, "EventEntry");
struct PhysicsEventStream { uint32_t len; uint8_t data[0x400]; };
static_assert(sizeof(PhysicsEventStream) == 0x404, "PhysicsEventStream");

// the centre line's position (CenterLine::convert_meters_to_ilpos): its segment and the fraction along it. An
// ILSeg: +0 the next segment, +4 its point (x, z), +0xc its tangent (x, z), +0x20 its length, +0x24 its start (m)
struct ILinePos { uint8_t* seg; uint32_t t; };

}  // namespace

// ---- statics -------------------------------------------------------------------------------------------------------
// world.obj
#define g_player        (*(volatile int32_t*)0x004f4354)   // the player's car (-1 until parse_car makes it)
#define g_focus         (*(volatile int32_t*)0x004f4358)   // the camera's car
#define g_world_single  (*(int32_t*)0x004f435c)            // SingleBegin("World")
#define g_mirror_x      (*(int32_t*)0x004f4364)            // MIRROR_X: (screen width - 128) / 2
#define g_use_grid_xz   (*(volatile uint8_t*)0x004f4368)   // parse_car: skip the centre line, use the file's X, Z
#define g_wobs          ((void**)0x005512a0)               // WorldObject*[0x400]
#define g_blimp         ((uint32_t*)0x005522a0)            // P3 (WorldMoveBlimp)
#define g_opt_mirror    (*(volatile int32_t*)0x005522b0)   // OptionsGet "mirror"
#define g_cars_made     (*(volatile int32_t*)0x005522b4)   // parse_car's next car
#define g_began         (*(volatile uint8_t*)0x005522bc)   // between WorldBeginCommon and WorldEndCommon
#define g_opt_shadow    (*(volatile int32_t*)0x005522c8)
#define g_nwobs         (*(volatile int32_t*)0x005522d0)
#define g_gobs          ((void**)0x005522d8)               // GraphObject*[0x400]
#define g_track_number  (*(int32_t*)0x005532d8)
#define g_start_line    ((Frame*)0x005532e0)               // parse_world / get_start_line
#define g_pkt_byte      (*(uint8_t*)0x00553314)            // WorldUpdate: the packet's byte at +0x40
#define g_frames        (*(volatile int32_t*)0x0055331c)   // frames drawn (WorldDraw), for the fps report
#define g_opt_specular  (*(volatile int32_t*)0x00553320)
#define g_camera        ((uint32_t*)0x00553328)            // Frame current_camera
#define g_opt_skids     (*(volatile int32_t*)0x00553358)
#define g_camera_type   (*(volatile int32_t*)0x0055335c)
#define g_race_state    (*(volatile int32_t*)0x00553360)
#define g_ngobs         (*(volatile int32_t*)0x00553368)
#define g_opt_smoke     (*(volatile int32_t*)0x0055336c)
#define g_world         ((World*)0x00553378)               // the race's World, copied
#define g_fx            (*(volatile uint8_t*)0x0055404c)
#define g_replay        (*(volatile uint8_t*)0x00554064)   // WorldBeginReplay: 1, WorldBegin: 0
#define g_frame_ms      (*(volatile int32_t*)0x0055406c)   // their total time (ms)
#define g_ghosts        (*(volatile int32_t*)0x00554070)   // ghost cars made so far
// read only
#define g_detail        (*(volatile int32_t*)0x00522a1c)   // the renderer's detail level
#define g_screen_wid    (*(volatile int32_t*)0x005228f4)   // gxScreenWid
#define g_career_upgrades (*(uint8_t* volatile*)0x004ff5a8)   // unsigned char* CareerUpgrades
static const char* const* const k_gx_section = (const char* const*)0x004dd080;   // -> "GX"
// carmgr.obj
#define g_carmgr_count  (*(volatile int32_t*)0x004f48d0)
#define g_carmgr_began  (*(volatile uint8_t*)0x004f48d4)
#define g_carmgr_single (*(int32_t*)0x004f48d8)            // SingleBegin("CarMgr")
#define g_car_registered ((volatile uint8_t*)0x00554078)   // [16]
#define g_carmgr        ((CarMgrInfo*)0x00554090)          // [16]
// event.obj
#define g_handlers      ((EventEntry*)0x00557ee0)          // [16]
#define g_nhandlers     (*(volatile int32_t*)0x00557f60)

// ---- constants (their bits) ------------------------------------------------------------------------------------------
static const uint32_t k_one = 0x3f800000;                   // 1.0f
static const uint32_t k_thousand = 0x447a0000;              // 1000.0f (0x4dd094)
static const uint32_t k_half = 0x3f000000;                  // 0.5f (0x4dd0ac)
static const uint32_t k_two = 0x40000000;                   // 2.0f (0x4dd0c4)
static const uint32_t k_three = 0x40400000;                 // 3.0f (0x4dd0d8)
static const uint32_t k_neg_two = 0xc0000000;               // -2.0f (0x4dd0dc)
static const uint32_t k_four = 0x40800000;                  // 4.0f (0x4dd0c8; set_car_frame's lateral offset)
static const uint32_t k_neg_four = 0xc0800000;              // -4.0f
static const uint32_t k_fifteen = 0x41700000;               // 15.0f (0x4dd0cc: metres per grid slot)
static const uint32_t k_twenty_eight = 0x41e00000;          // 28.0f (0x4dd0d0)
static const uint32_t k_wheel_height = 0x3f1c28f6;          // 0.61f (0x4dd0d4)
static const uint32_t k_ball_height = 0x459c4000;           // 5000.0f (0x4dd0e0)
static const uint32_t k_inertia = 0x412bff2c;               // 10.749798f (0x4dd0f0)
static const uint32_t k_min_axis = 0x38d1b717;              // 1e-4f (0x4dd0fc)
static const uint32_t k_pi = 0x40490fdb;                    // 3.1415927f
static const uint32_t k_wheel_lock_min = 0x3c23d70a;        // 0.01f, compared as an int
static const uint32_t k_wheel_lock_max = 0x3f8147ae;        // 1.01f

// ---- the functions they call, by address -----------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
typedef uint8_t(__cdecl* Flag_t)();
typedef int(__cdecl* Int_t)();
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
static const AssertMsg_t ASSERT_MSG_world = (AssertMsg_t)0x00462750;   // bare rets in this build
static const AssertMsg_t ASSERT_MSG_carmgr = (AssertMsg_t)0x00464470;
typedef int(__cdecl* SingleBegin_t)(const char*);
static const SingleBegin_t SingleBegin = (SingleBegin_t)0x00414f30;
typedef void(__cdecl* Single3_t)(int, const char*, int);
static const Single3_t SingleEnter = (Single3_t)0x00415000;         // _SingleEnter(handle, file, line)
static const Single3_t SingleLeave = (Single3_t)0x00415070;
static const Single3_t SingleEnd = (Single3_t)0x00415090;
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
// the game's CRT
typedef int(__cdecl* Sscanf_t)(const char*, const char*, ...);
static const Sscanf_t sscanf_o = (Sscanf_t)0x004ce190;
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t sprintf_o = (Sprintf_t)0x004cf0a0;
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
static const Stricmp_t stricmp_o = (Stricmp_t)0x004da350;
typedef char*(__cdecl* Strchr_t)(const char*, int);
static const Strchr_t strchr_o = (Strchr_t)0x004ce0c0;
typedef int(__cdecl* SortFn_t)(const void*, const void*);
typedef void(__cdecl* Qsort_t)(void*, uint32_t, uint32_t, SortFn_t);
static const Qsort_t qsort_o = (Qsort_t)0x004cf160;
// WorldBeginCommon / WorldEndCommon's callees
typedef int(__cdecl* GetTrackNumber_t)(const char*);
static const GetTrackNumber_t GetTrackNumber = (GetTrackNumber_t)0x004068b0;
typedef void(__cdecl* OptionsGet_t)(const char*, const char*, volatile int32_t*);
static const OptionsGet_t OptionsGet = (OptionsGet_t)0x004713e0;
static const Void_t ViewPreBegin = (Void_t)0x00465ff0, ViewPostEnd = (Void_t)0x004660b0, ViewEnd = (Void_t)0x004664e0;
typedef void(__cdecl* ViewBegin_t)(const GameOptions*);
static const ViewBegin_t ViewBegin = (ViewBegin_t)0x004660d0;
typedef uint8_t(__cdecl* TrackLoad_t)(const char*);
static const TrackLoad_t TrackLoad = (TrackLoad_t)0x00469500;
static const Void_t TrackUnload = (Void_t)0x00469580;
static const Void_t EffectsBegin = (Void_t)0x00467460, EffectsEnd = (Void_t)0x00467470;
typedef void(__cdecl* WorldArg_t)(const World*);
static const WorldArg_t RecordBegin = (WorldArg_t)0x0042a050;
static const Void_t RecordEnd = (Void_t)0x0042a210;
typedef void(__cdecl* PhysicsBegin_t)(const GameOptions*, int);
static const PhysicsBegin_t PhysicsBegin = (PhysicsBegin_t)0x0042b910;
static const Void_t PhysicsStart = (Void_t)0x0042ba30, PhysicsStop = (Void_t)0x0042bae0, PhysicsEnd = (Void_t)0x0042b9c0;
typedef uint8_t(__cdecl* AIBegin_t)(CarList*, const char*);
static const AIBegin_t AIBegin = (AIBegin_t)0x0041c600;
static const Void_t AIEnd = (Void_t)0x0041c650;
static const Void_t mrModelFlush = (Void_t)0x004570f0;
typedef void(__cdecl* IntArg_t)(int);
static const IntArg_t CameraSetFocusCar = (IntArg_t)0x0042beb0;
static const Int_t CameraGetMaxFocusCar = (Int_t)0x0042bee0;
static const WorldArg_t GhostBegin = (WorldArg_t)0x0040dce0, GhostEnd = (WorldArg_t)0x0040e810;
static const Void_t GhostSaveCars = (Void_t)0x0040e510;
static const Int_t GhostGetTarget = (Int_t)0x0040e8e0;
typedef double(__cdecl* PhysicsGetTime_t)();                        // ST0
static const PhysicsGetTime_t PhysicsGetTime = (PhysicsGetTime_t)0x0042bc80;
typedef uint8_t*(__cdecl* GetStatePacket_t)(void*, PhysicsEventStream*);
static const GetStatePacket_t PhysicsGetStatePacket = (GetStatePacket_t)0x0042bd70;
static const Void_t PhysicsReleaseStatePacket = (Void_t)0x0042bdd0;
typedef void(__fastcall* ProcessPacket_t)(void*, Edx, const uint8_t*);
static const ProcessPacket_t WorldObject_ProcessPacket = (ProcessPacket_t)0x00469a10;
typedef uint8_t(__cdecl* MrIsEnabled_t)(int);
static const MrIsEnabled_t mrIsEnabled = (MrIsEnabled_t)0x00457d80;
// the parser's callees
struct StringTable { uint32_t _opaque; };                 // (table.obj's: only passed along here)
typedef StringTable*(__cdecl* StringTableGet_t)(const char*);
static const StringTableGet_t StringTableGet = (StringTableGet_t)0x0041b210;
typedef void(__cdecl* StringTableForget_t)(StringTable*);
static const StringTableForget_t StringTableForget = (StringTableForget_t)0x0041b270;
typedef int(__cdecl* StringTableNumRows_t)(const StringTable*);
static const StringTableNumRows_t StringTableNumRows = (StringTableNumRows_t)0x0041b290;
typedef const char*(__cdecl* StringTableGetEntry_t)(const StringTable*, int, int);
static const StringTableGetEntry_t StringTableGetEntry = (StringTableGetEntry_t)0x0041b2a0;
typedef uint8_t(__cdecl* CarFileLoad_t)(CarData*, const void*, const char*, uint8_t*);
static const CarFileLoad_t CarFileLoad = (CarFileLoad_t)0x00465100;
typedef void(__cdecl* CarDataArg_t)(CarData*);
static const CarDataArg_t AITweakCarData = (CarDataArg_t)0x0041c590;
typedef double(__cdecl* TerrainGetHeight_t)(uint32_t, uint32_t);   // (float x, float z) as bits; ST0
static const TerrainGetHeight_t TerrainGetHeight = (TerrainGetHeight_t)0x00465ae0;
typedef void*(__fastcall* CarObjectCtor_t)(void*, Edx, void*, const CarListEntry*);
static const CarObjectCtor_t CarObject_ctor = (CarObjectCtor_t)0x00469e00;
static const CarObjectCtor_t GhostCarObject_ctor = (CarObjectCtor_t)0x0046cd20;
typedef void*(__fastcall* ModelObjectCtor_t)(void*, Edx, void*, const char*);
static const ModelObjectCtor_t ModelObject_ctor = (ModelObjectCtor_t)0x00469a70;
typedef void*(__fastcall* WobbleObjectCtor_t)(void*, Edx, void*);
static const WobbleObjectCtor_t WobbleObject_ctor = (WobbleObjectCtor_t)0x00469ae0;
typedef void*(__fastcall* CenterLineCtor_t)(void*, Edx, const uint8_t*);
static const CenterLineCtor_t CenterLine_ctor = (CenterLineCtor_t)0x00422400;
typedef void(__fastcall* CenterLineDtor_t)(void*, Edx);
static const CenterLineDtor_t CenterLine_dtor = (CenterLineDtor_t)0x00422490;
typedef ILinePos*(__fastcall* ConvertMeters_t)(void*, Edx, ILinePos*, float);
static const ConvertMeters_t CenterLine_convert_meters_to_ilpos = (ConvertMeters_t)0x00422710;
typedef void(__fastcall* QuickTan_t)(const void*, Edx, Point2D*, uint32_t);   // (Point2D&, float t as bits)
static const QuickTan_t ILSeg_QuickTan = (QuickTan_t)0x00426150;
typedef void(__cdecl* Cross_t)(P3*, const P3*, const P3*);
static const Cross_t CrossProduct = (Cross_t)0x0043b120;
typedef void(__cdecl* MatrixConcat_t)(void*, const void*, const void*);
static const MatrixConcat_t MatrixConcat = (MatrixConcat_t)0x00403040;
static const Flag_t HackNoWalls = (Flag_t)0x0040d560;
typedef int(__cdecl* MrModelLoad_t)(const char*);
static const MrModelLoad_t mrModelLoad = (MrModelLoad_t)0x004558c0;
typedef void(__cdecl* MrModelGetExtents_t)(int, float*, float*, float*, float*, float*, float*);
static const MrModelGetExtents_t mrModelGetExtents = (MrModelGetExtents_t)0x00456ec0;
static const IntArg_t mrModelUnload = (IntArg_t)0x00455950;
typedef void(__cdecl* PhysicsCreate_t)(void*, int*);
static const PhysicsCreate_t PhysicsCreate = (PhysicsCreate_t)0x0042bc10;
typedef uint8_t(__cdecl* MultiCarIsHuman_t)(int);
static const MultiCarIsHuman_t MultiCarIsHuman = (MultiCarIsHuman_t)0x004a2b30;
// this group's own functions, by address (the hooked rewrite -- or the original -- is what runs)
typedef void(__cdecl* WorldPtr_t)(World*);
static const WorldPtr_t WorldBeginCommon_o = (WorldPtr_t)0x00461b90;
static const Void_t WorldEndCommon_o = (Void_t)0x00461df0;
static const Void_t WorldInitTextures_o = (Void_t)0x00461f50;
typedef const GameOptions*(__cdecl* WorldGameOptions_t)();
static const WorldGameOptions_t WorldGameOptions_o = (WorldGameOptions_t)0x004627a0;
typedef void(__cdecl* AddObj_t)(void*);
static const AddObj_t WorldAddGob_o = (AddObj_t)0x004627c0;
static const AddObj_t WorldAddWob_o = (AddObj_t)0x00462800;
static const WorldPtr_t setup_cars_o = (WorldPtr_t)0x00462ab0;
typedef uint8_t(__cdecl* ParseWorld_t)(const World*);
static const ParseWorld_t parse_world_o = (ParseWorld_t)0x00462b10;
typedef void(__cdecl* GetStartLine_t)(Frame*, StringTable*, uint8_t);
static const GetStartLine_t get_start_line_o = (GetStartLine_t)0x00462d40;
typedef void(__cdecl* CarListArg_t)(const CarList*);
static const CarListArg_t create_remaining_cars_o = (CarListArg_t)0x00462f90;
typedef void(__cdecl* ParseCar_t)(const char*, const CarList*);
static const ParseCar_t parse_car_o = (ParseCar_t)0x00462fc0;
static const ParseCar_t parse_object_o = (ParseCar_t)0x00463750;
typedef uint8_t(__cdecl* SetCarFrame_t)(Frame*, int);
static const SetCarFrame_t set_car_frame_from_dlatlong_o = (SetCarFrame_t)0x004632f0;
typedef void(__cdecl* Hermite_t)(Point2D*, const void*, const void*, const void*, const void*, uint32_t);   // t as bits
static const Hermite_t HermiteEval_o = (Hermite_t)0x004635f0;
static const IntArg_t create_ball_o = (IntArg_t)0x004636a0;
typedef void(__cdecl* ParseLine_t)(const char*);
static const ParseLine_t parse_obstacle_o = (ParseLine_t)0x00463870;
static const ParseLine_t parse_wobble_o = (ParseLine_t)0x00463ab0;
static const ParseLine_t parse_static_o = (ParseLine_t)0x00463bc0;
static const ParseLine_t parse_checkpoint_o = (ParseLine_t)0x00463e30;
static const Void_t sort_objects_o = (Void_t)0x00463f40;
static const SortFn_t obj_sort_f_o = (SortFn_t)0x00463f60;           // qsort's comparator (by address)
static const Flag_t should_draw_mirror_o = (Flag_t)0x00463f90;
typedef void*(__fastcall* TrivialCtor_t)(void*, Edx);
static const TrivialCtor_t CarInfo_ctor_o = (TrivialCtor_t)0x00463fc0;
static const TrivialCtor_t Point2D_ctor_o = (TrivialCtor_t)0x00463fd0;
typedef void(__cdecl* MakeRotation_t)(M3*, uint32_t, uint32_t, uint32_t);   // (Matrix&, 3 floats as bits)
static const MakeRotation_t MatrixMakeWorldRotation_o = (MakeRotation_t)0x00463fe0;
static const Void_t CarMgrBegin_o = (Void_t)0x00464320, CarMgrEnd_o = (Void_t)0x00464370;
static const Int_t CarMgrCount_o = (Int_t)0x00464480;
typedef void(__cdecl* CarMgrUpdateCar_t)(int, const void*);
static const CarMgrUpdateCar_t CarMgrUpdateCar_o = (CarMgrUpdateCar_t)0x004644f0;
static const Void_t EventBegin_o = (Void_t)0x004691d0, EventEnd_o = (Void_t)0x004692d0;
typedef void(__cdecl* EventDispatch_t)(const PhysicsEventStream*);
static const EventDispatch_t EventDispatch_o = (EventDispatch_t)0x00469270;

// strings, at their v1.0 addresses
static const char* const k_str_smoke = (const char*)0x004f43a8;       // "smoke"
static const char* const k_str_specular = (const char*)0x004f43b0;    // "specular"
static const char* const k_str_mirror = (const char*)0x004f43bc;      // "mirror"
static const char* const k_str_shadow = (const char*)0x004f43c4;      // "shadow"
static const char* const k_str_skids = (const char*)0x004f43cc;       // "skids"
static const char* const k_str_world = (const char*)0x004f43d4;       // "World"
static const char* const k_str_no_parse = (const char*)0x004f43dc;    // "Couldn't parse world"
static const char* const k_str_ai_failed = (const char*)0x004f43f4;   // "AIBegin failed"
static const char* const k_str_no_track = (const char*)0x004f4404;    // "Couldn't load track"
static const char* const k_fmt_fps = (const char*)0x004f4418;         // "possible fps: %f (%d)"
static const char* const k_fmt_nobjects = (const char*)0x004f4430;    // "number of objects was %d"
static const char* const k_str_viper = (const char*)0x004f444c;       // "viper"
static const char* const k_fmt_paint = (const char*)0x004f4454;       // "~paint%d.tex"
static const char* const k_fmt_car_n = (const char*)0x004f4464;       // "%s%d.tex"
static const char* const k_fmt_tilde_car = (const char*)0x004f4470;   // "~%s.tex"
static const char* const k_fmt_car = (const char*)0x004f4478;         // "%s.tex"
static const char* const k_str_assert_entry = (const char*)0x004f4480;    // "WorldGetCarEntry(): cannot call before WorldBegin()"
static const char* const k_str_assert_trackno = (const char*)0x004f44b4;  // "WorldGetTrackNumber(): ..."
static const char* const k_str_assert_trackname = (const char*)0x004f44ec; // "WorldGetTrackname(): ..."
static const char* const k_str_assert_options = (const char*)0x004f4524;   // "WorldGameOptions(): ..."
static const char* const k_str_assert_gobs = (const char*)0x004f4558;     // "Too many objects allocated--increase MAX_OBJECTS"
static const char* const k_str_assert_wobs = (const char*)0x004f458c;     // "Too many wobjects allocated--increase MAX_OBJECTS"
static const char* const k_fmt_wheel_lock = (const char*)0x004f45c0;  // "setup_cars: car %d has bad wheel lock: %f"
static const char* const k_str_track_obt = (const char*)0x004f45ec;   // "track.obt"
static const char* const k_fmt_s = (const char*)0x004f45f8;           // "%s"
static const char* const k_str_end = (const char*)0x004f45fc;         // "end"
static const char* const k_str_map = (const char*)0x004f4600;         // "map"
static const char* const k_fmt_map = (const char*)0x004f4604;         // "map %s"
static const char* const k_str_ai = (const char*)0x004f460c;          // "ai"
static const char* const k_str_obj = (const char*)0x004f4610;         // "obj"
static const char* const k_fmt_bad_token = (const char*)0x004f4614;   // "Bad token: %s"
static const char* const k_fmt_start_cp = (const char*)0x004f4624;    // "obj checkpoint %s %f,%f %f,%f"
static const char* const k_str_no_checkpoints = (const char*)0x004f4644;  // "Couldn't find any checkpoints!"
static const char* const k_str_obj_car_00 = (const char*)0x004f4664;  // "obj car 0, 0"
static const char* const k_fmt_obj_car = (const char*)0x004f4674;     // "obj car %f, %f"
static const uint32_t* const k_dot_cf = (const uint32_t*)0x004f4684;  // ".cf" (read as a dword)
static const char* const k_str_bad_car = (const char*)0x004f4688;     // "Bad car line"
static const char* const k_fmt_subtype = (const char*)0x004f4698;     // "Unknown car subtype %d"
static const char* const k_str_ball_mod = (const char*)0x004f46c0;    // "ball.mod"
static const char* const k_fmt_obj = (const char*)0x004f46d0;         // "obj %s"
static const char* const k_str_car = (const char*)0x004f46d8;         // "car"
static const char* const k_str_obstacle = (const char*)0x004f46dc;    // "obstacle"
static const char* const k_str_wobble = (const char*)0x004f46e8;      // "wobble"
static const char* const k_str_static = (const char*)0x004f46f0;      // "static"
static const char* const k_str_checkpoint = (const char*)0x004f46f8;  // "checkpoint"
static const char* const k_fmt_unknown_obj = (const char*)0x004f4704; // "Unknown object type %s"
static const char* const k_fmt_obstacle = (const char*)0x004f4724;    // "obj obstacle %s %s %f,%f:%f %f"
static const char* const k_str_ball = (const char*)0x004f4744;        // "ball"
static const char* const k_str_cube = (const char*)0x004f474c;        // "cube"
static const char* const k_str_prism = (const char*)0x004f4754;       // "prism"
static const char* const k_fmt_bad_obstacle_type = (const char*)0x004f475c;   // "Bad obstacle type: %s"
static const char* const k_str_bad_obstacle = (const char*)0x004f4780;        // "Bad obstacle record"
static const char* const k_fmt_wobble = (const char*)0x004f4798;      // "obj wobble %s %d"
static const char* const k_str_pole = (const char*)0x004f47ac;        // "pole"
static const char* const k_str_flap = (const char*)0x004f47b4;        // "flap"
static const char* const k_fmt_bad_wobble_type = (const char*)0x004f47bc;    // "Unknown wobble type: %s"
static const char* const k_str_bad_wobble = (const char*)0x004f47e4;  // "Bad wobble record"
static const char* const k_fmt_static = (const char*)0x004f47fc;      // "obj static %s %f,%f,%f %f,%f,%f %f,%f,%f"
static const char* const k_str_box = (const char*)0x004f4828;         // "box"
static const char* const k_fmt_bad_static_type = (const char*)0x004f482c;    // "Bad static type: %s"
static const char* const k_str_bad_static = (const char*)0x004f4840;  // "Bad static record"
static const char* const k_fmt_checkpoint = (const char*)0x004f4854;  // "obj checkpoint %s %f,%f %f,%f"
static const char* const k_str_checkpt_mod = (const char*)0x004f48a8; // "checkpt1.mod"
static const char* const k_str_bad_checkpoint = (const char*)0x004f48b8;     // "Bad checkpoint record"
static const char* const k_str_carmgr = (const char*)0x004f4914;      // "CarMgr"
static const char* const k_str_fn_register = (const char*)0x004f491c; // "CarMgrRegisterCar"
static const char* const k_fmt_not_a_car = (const char*)0x004f4930;   // "%s: %d is not even a valid car number"
static const char* const k_str_lt = (const char*)0x004f4958;          // "<"
static const char* const k_str_ge = (const char*)0x004f495c;          // ">="
static const char* const k_str_fn_getinfo = (const char*)0x004f4960;  // "CarMgrGetInfo"
static const char* const k_fmt_out_of_range = (const char*)0x004f4970;    // "%s: Car out of range (%d %s %d)"
static const char* const k_str_lt2 = (const char*)0x004f4990;         // "<"
static const char* const k_str_ge2 = (const char*)0x004f4994;         // ">="
static const char* const k_str_fn_update = (const char*)0x004f4998;   // "CarMgrUpdateCar"
static const char* const k_fmt_out_of_range2 = (const char*)0x004f49a8;   // "%s: Car out of range (%d %s %d)"
static const char* const k_str_fn_status = (const char*)0x004f49c8;   // "_CarMgrGetStatusBuf"
static const char* const k_fmt_not_a_car2 = (const char*)0x004f49dc;  // "%s: %d is not even a valid car number"
static const char* const k_str_no_handler = (const char*)0x004f4ebc;  // "EventUnInstallHandler: Can't find handler!"

// ---- footprint helpers ---------------------------------------------------------------------------------------------
static void fp_none(Footprint&) {}
#define FP_ADD(f, addr, n, what) (f).add((void*)(uintptr_t)(addr), (n), (what))

// =====================================================================================================================
// world.obj: begin / end
// =====================================================================================================================

// ==== WorldBeginCommon (0x461b90) =====================================================================================
// The race's start (both WorldBegin and WorldBeginReplay): the World copied to its static, the track's number, the
// GX options, the world lock, the view's pre-begin, the camera and counters reset; the track loaded; then every
// part begun in order -- events, effects, the view, the car manager, the recorder, the physics, the AI -- the cars
// checked, the world file parsed, the objects sorted, the physics started, the models flushed, the textures loaded
// and the camera put on the player's car. Any failure is a panic.
static void __cdecl WorldBeginCommon_rw(World* w) {
    g_race_state = 0;
    movsd(g_world, w, 0x335);
    g_track_number = GetTrackNumber(g_world->track);
    const char* sec = *k_gx_section;
    g_opt_smoke = 0;
    OptionsGet(sec, k_str_smoke, &g_opt_smoke);
    g_opt_specular = 0;
    OptionsGet(sec, k_str_specular, &g_opt_specular);
    g_opt_mirror = 0;
    OptionsGet(sec, k_str_mirror, &g_opt_mirror);
    g_opt_shadow = 0;
    OptionsGet(sec, k_str_shadow, &g_opt_shadow);
    g_opt_skids = 0;
    OptionsGet(sec, k_str_skids, &g_opt_skids);
    g_world_single = SingleBegin(k_str_world);
    g_began = 1;
    ViewPreBegin();
    const int32_t wid = g_screen_wid;
    g_ngobs = 0;
    g_mirror_x = (int32_t)((uint32_t)wid - 0x80u) / 2;          // cdq; sub; sar: towards zero
    g_cars_made = 0;
    g_ghosts = 0;
    g_focus = 0;
    g_camera_type = 0;
    for (int k = 0; k < 12; k++) g_camera[k] = (k == 0 || k == 4 || k == 8) ? k_one : 0;
    g_player = -1;
    g_fx = 1;
    if (!TrackLoad(w->track)) {
        LogPanic(k_str_no_track);
    } else {
        EventBegin_o();
        EffectsBegin();
        ViewBegin(&w->options);
        CarMgrBegin_o();
        RecordBegin(w);
        PhysicsBegin(&w->options, w->cars.count);
        if (!AIBegin(&w->cars, w->track)) {
            LogPanic(k_str_ai_failed);
        } else {
            setup_cars_o(w);
            if (!parse_world_o(w)) {
                LogPanic(k_str_no_parse);
            } else {
                sort_objects_o();
                PhysicsStart();
                mrModelFlush();
                WorldInitTextures_o();
                CameraSetFocusCar(g_focus);
            }
        }
    }
    g_frame_ms = 0;
    g_frames = 0;
}
static void fp_world_begin(Footprint& f, World*) { f.replay_only = "it loads the track, begins every part of the race and parses the world"; }
PORT_FN(0x00461b90, "WorldBeginCommon", WorldBeginCommon_rw, fp_world_begin)

// ==== WorldBeginReplay (0x461db0), WorldBegin (0x461dd0) ================================================================
static void __cdecl WorldBeginReplay_rw(World* w) {
    g_replay = 1;
    WorldBeginCommon_o(w);
}
PORT_FN(0x00461db0, "WorldBeginReplay", WorldBeginReplay_rw, fp_world_begin)

static void __cdecl WorldBegin_rw(World* w) {
    g_replay = 0;
    GhostBegin(w);
    WorldBeginCommon_o(w);
}
PORT_FN(0x00461dd0, "WorldBegin", WorldBegin_rw, fp_world_begin)

// ==== WorldEndCommon (0x461df0) ========================================================================================
// The physics and the AI stopped and ended, every graphics object deleted (its scalar deleting destructor), both
// object lists cleared, then the ghosts saved and the other parts ended; "possible fps" reported after 300 frames.
static void __cdecl WorldEndCommon_rw() {
    PhysicsStop();
    AIEnd();
    PhysicsEnd();
    // the lists and their sizes as the original's instructions hold them (M1 repoints and resizes them)
    void** const gobs = (void**)m1_operand(0x00461e0f);          // mov esi, gobs
    for (int i = 0; i < g_ngobs; i++) {
        void* g = gobs[i];
        if (g) VFN(g, 0, void*, unsigned)(g, 0, 1);
    }
    memset((void*)m1_operand(0x00461e2c), 0, 4 * m1_operand(0x00461e33));   // mov edi, gobs; mov ecx, 0x400
    memset((void*)m1_operand(0x00461e3a), 0, 4 * m1_operand(0x00461e3f));   // mov edi, wobs; mov ecx, 0x400
    g_nwobs = 0;
    g_ngobs = 0;
    GhostSaveCars();
    RecordEnd();
    CarMgrEnd_o();
    ViewEnd();
    EffectsEnd();
    EventEnd_o();
    TrackUnload();
    ViewPostEnd();
    g_began = 0;
    if (g_frames > 300) {
        const int32_t n = g_frames;
        const double fps = D(n) / D(g_frame_ms) * D(Fb(k_thousand));   // fild; fidiv; fmul dword
        LogReport(k_fmt_fps, fps, n);
    }
    SingleEnd(g_world_single, 0, 0);
}
static void fp_world_end(Footprint& f) { f.replay_only = "it ends every part of the race and deletes every object"; }
PORT_FN(0x00461df0, "WorldEndCommon", WorldEndCommon_rw, fp_world_end)

// ==== WorldEnd (0x461ef0), WorldEndReplay (0x461f20) ====================================================================
static void __cdecl WorldEnd_rw(World* w) {
    LogReport(k_fmt_nobjects, g_ngobs);
    WorldEndCommon_o();
    GhostEnd(w);
}
static void fp_world_end_w(Footprint& f, World*) { f.replay_only = "it ends every part of the race and deletes every object"; }
PORT_FN(0x00461ef0, "WorldEnd", WorldEnd_rw, fp_world_end_w)

static void __cdecl WorldEndReplay_rw() { WorldEndCommon_o(); }         // jmp WorldEndCommon
PORT_FN(0x00461f20, "WorldEndReplay", WorldEndReplay_rw, fp_world_end)

// ==== WorldSwitchToDrive (0x461f30), WorldSwitchToUI (0x461f40): tail jumps ==============================================
static void __cdecl WorldSwitchToDrive_rw() { ViewPreBegin(); }
static void __cdecl WorldSwitchToUI_rw() { ViewPostEnd(); }
static void fp_view_switch(Footprint& f) { f.replay_only = "the view switches the display between the race and the menus"; }
PORT_FN(0x00461f30, "WorldSwitchToDrive", WorldSwitchToDrive_rw, fp_view_switch)
PORT_FN(0x00461f40, "WorldSwitchToUI", WorldSwitchToUI_rw, fp_view_switch)

// ==== WorldInitTextures (0x461f50): every graphics object's +4 virtual (no null check) ===================================
static void __cdecl WorldInitTextures_rw() {
    void** const gobs = (void**)m1_operand(0x00461f5d);          // the list as the original addresses it (M1)
    for (int i = 0; i < g_ngobs; i++) {
        void* g = gobs[i];
        VFN(g, 4, void)(g, 0);
    }
}
static void fp_init_textures(Footprint& f) { f.replay_only = "each object loads its textures"; }
PORT_FN(0x00461f50, "WorldInitTextures", WorldInitTextures_rw, fp_init_textures)

// =====================================================================================================================
// world.obj: the getters and small setters
// =====================================================================================================================
static int __cdecl WorldShowShadow_rw() { return g_detail != 0 ? g_opt_shadow : 0; }
PORT_FN(0x00461f80, "WorldShowShadow", WorldShowShadow_rw, fp_none)
static int __cdecl WorldShowSkids_rw() { return g_detail != 0 ? g_opt_skids : 0; }
PORT_FN(0x00461fa0, "WorldShowSkids", WorldShowSkids_rw, fp_none)
static int __cdecl WorldShowSmoke_rw() { return g_detail != 0 ? g_opt_smoke : 0; }
PORT_FN(0x00461fc0, "WorldShowSmoke", WorldShowSmoke_rw, fp_none)

static uint8_t __cdecl WorldShowReflection_rw() {
    const GameOptions* o = WorldGameOptions_o();
    return (o->reflection == 1 && g_detail != 0) ? 1 : 0;
}
PORT_FN(0x00461fe0, "WorldShowReflection", WorldShowReflection_rw, fp_none)

// detail 2: the option as it is; detail 1: 2 becomes 1; otherwise (or no specular in the renderer) 0
static int __cdecl WorldShowSpecular_rw() {
    if (!mrIsEnabled(1)) return 0;
    if (g_detail == 2) return g_opt_specular;
    if (g_detail == 1) return g_opt_specular == 2 ? 1 : g_opt_specular;
    return 0;
}
PORT_FN(0x00462000, "WorldShowSpecular", WorldShowSpecular_rw, fp_none)

static double __cdecl WorldGetElapsedTime_rw() { return PhysicsGetTime(); }   // jmp PhysicsGetTime: its ST0
PORT_FN(0x00462050, "WorldGetElapsedTime", WorldGetElapsedTime_rw, fp_none)

static int __cdecl WorldGetRaceState_rw() { return g_race_state; }
PORT_FN(0x00462060, "WorldGetRaceState", WorldGetRaceState_rw, fp_none)

// ==== WorldGetCarTexture (0x462070) ======================================================================================
// "viper" (any case): ~paint<-1-paint>.tex or viper<paint+1>.tex; any other car: ~<car>.tex or <car>.tex
static void __cdecl WorldGetCarTexture_rw(char* out, const char* car, int paint) {
    if (stricmp_o(car, k_str_viper) == 0) {
        if (paint < 0) sprintf_o(out, k_fmt_paint, (int)~(uint32_t)paint);
        else sprintf_o(out, k_fmt_car_n, car, (int)((uint32_t)paint + 1));
        return;
    }
    if (paint < 0) sprintf_o(out, k_fmt_tilde_car, car);
    else sprintf_o(out, k_fmt_car, car);
}
static void fp_car_texture(Footprint& f, char* out, const char* car, int) {
    f.add(out, (uint32_t)strlen(car) + 20, "texture name");    // the longest is "~paint-2147483648.tex" / car + "2147483648.tex"
}
PORT_FN(0x00462070, "WorldGetCarTexture", WorldGetCarTexture_rw, fp_car_texture)

// ==== WorldUpdate (0x462100) =============================================================================================
// Once a frame (main thread): the physics' state packet read (+0x04 the camera, +0x34 camera type, +0x38 focus
// car, +0x3c race state, +0x40 a byte, +0x44 the cars' CarInfos, then the event queue), the events dispatched,
// every world object handed its message, every car's CarInfo given to the car manager. The packet (0x3d8 bytes)
// and the event stream after it are on the stack.
static void __cdecl WorldUpdate_rw() {
    struct { uint8_t pkt[0x3d8]; PhysicsEventStream ev; } s;
    SingleEnter(g_world_single, 0, 0);
    for (int i = 0; i < 16; i++) CarInfo_ctor_o(s.pkt + 0x44 + i * 0x38, 0);
    const uint8_t* msgs = PhysicsGetStatePacket(s.pkt, &s.ev);
    movsd(g_camera, s.pkt + 4, 12);
    g_focus = *(const int32_t*)(s.pkt + 0x38);
    g_camera_type = *(const int32_t*)(s.pkt + 0x34);
    g_race_state = *(const int32_t*)(s.pkt + 0x3c);
    g_pkt_byte = s.pkt[0x40];
    EventDispatch_o(&s.ev);
    void** const wobs = (void**)m1_operand(0x00462197);          // mov edi, wobs (M1 repoints it)
    for (int i = 0; i < g_nwobs; i++) WorldObject_ProcessPacket(wobs[i], 0, msgs);
    for (int i = 0; i < g_cars_made; i++) CarMgrUpdateCar_o(i, s.pkt + 0x44 + i * 0x38);
    PhysicsReleaseStatePacket();
    SingleLeave(g_world_single, 0, 0);
}
static void fp_world_update(Footprint& f) { f.replay_only = "it takes the physics packet's read lock and runs the event handlers and every object's packet"; }
PORT_FN(0x00462100, "WorldUpdate", WorldUpdate_rw, fp_world_update)

// ==== the blimp, the focus car, the camera ================================================================================
static void __cdecl WorldMoveBlimp_rw(const uint32_t* p) {       // P3DBase const&, moved as three dwords
    uint32_t* b = g_blimp;
    b[0] = p[0];
    b[1] = p[1];
    b[2] = p[2];
}
static void fp_blimp(Footprint& f, const uint32_t*) { FP_ADD(f, 0x005522a0, 12, "blimp position"); }
PORT_FN(0x00462670, "WorldMoveBlimp", WorldMoveBlimp_rw, fp_blimp)

static void fp_focus(Footprint& f) { FP_ADD(f, 0x00521614, 4, "physics camera_focus (CameraSetFocusCar)"); }
static void __cdecl WorldNextFocusCar_rw() {
    const int32_t cur = g_focus;
    const int32_t n = CameraGetMaxFocusCar();
    CameraSetFocusCar((cur + 1) % n);                            // idiv: a divide error when there are none, as the original
}
PORT_FN(0x00462690, "WorldNextFocusCar", WorldNextFocusCar_rw, fp_focus)

static void __cdecl WorldPrevFocusCar_rw() {
    int32_t c = (int32_t)((uint32_t)g_focus - 1u);
    if (c < 0) c = (int32_t)((uint32_t)CameraGetMaxFocusCar() - 1u);
    CameraSetFocusCar(c);
}
PORT_FN(0x004626b0, "WorldPrevFocusCar", WorldPrevFocusCar_rw, fp_focus)

static int __cdecl WorldGetFocusCar_rw() { return g_focus; }
PORT_FN(0x004626d0, "WorldGetFocusCar", WorldGetFocusCar_rw, fp_none)
static int __cdecl WorldGetCameraType_rw() { return g_camera_type; }
PORT_FN(0x004626e0, "WorldGetCameraType", WorldGetCameraType_rw, fp_none)

static void __cdecl WorldSetFocusCar_rw(int car) {
    if (car >= 0 && CameraGetMaxFocusCar() > car) CameraSetFocusCar(car);
}
static void fp_set_focus(Footprint& f, int) { fp_focus(f); }
PORT_FN(0x004626f0, "WorldSetFocusCar", WorldSetFocusCar_rw, fp_set_focus)

static int __cdecl WorldGetPlayerCar_rw() { return g_player; }
PORT_FN(0x00462710, "WorldGetPlayerCar", WorldGetPlayerCar_rw, fp_none)

static const CarListEntry* __cdecl WorldGetCarEntry_rw(int car) {
    ASSERT_MSG_world((int)g_began, k_str_assert_entry);
    return (const CarListEntry*)(uintptr_t)(0x005533a0u + (uint32_t)car * 200u);   // no bounds check
}
static void fp_car_entry(Footprint&, int) {}
PORT_FN(0x00462720, "WorldGetCarEntry", WorldGetCarEntry_rw, fp_car_entry)

static int __cdecl WorldGetTrackNumber_rw() {
    ASSERT_MSG_world((int)g_began, k_str_assert_trackno);
    return g_track_number;
}
PORT_FN(0x00462760, "WorldGetTrackNumber", WorldGetTrackNumber_rw, fp_none)
static const char* __cdecl WorldGetTrackname_rw() {
    ASSERT_MSG_world((int)g_began, k_str_assert_trackname);
    return g_world->track;
}
PORT_FN(0x00462780, "WorldGetTrackname", WorldGetTrackname_rw, fp_none)
static const GameOptions* __cdecl WorldGameOptions_rw() {
    ASSERT_MSG_world((int)g_began, k_str_assert_options);
    return &g_world->options;
}
PORT_FN(0x004627a0, "WorldGameOptions", WorldGameOptions_rw, fp_none)

// ==== the object lists (0x400 each, the asserts are no-ops: no bounds check) =============================================
static void __cdecl WorldAddGob_rw(void* gob) {
    // the capacity and the list as the original's instructions hold them (M1 lifts both)
    ASSERT_MSG_world(g_ngobs < (int32_t)m1_operand(0x004627d0) ? 1 : 0, k_str_assert_gobs);
    const int32_t n = g_ngobs + 1;
    g_ngobs = n;
    ((void**)m1_operand(0x004627f3))[n] = gob;                   // gobs[n - 1]
}
static void fp_add_gob(Footprint& f, void*) {
    FP_ADD(f, 0x00553368, 4, "gob count");
    FP_ADD(f, m1_operand(0x004627f3) + 4u + 4u * (uint32_t)g_ngobs, 4, "gobs[count]");
}
PORT_FN(0x004627c0, "WorldAddGob", WorldAddGob_rw, fp_add_gob)

static void __cdecl WorldAddWob_rw(void* wob) {
    ASSERT_MSG_world(g_nwobs < (int32_t)m1_operand(0x0046280c) ? 1 : 0, k_str_assert_wobs);   // (M1)
    WorldAddGob_o(wob);
    const int32_t n = g_nwobs + 1;
    g_nwobs = n;
    ((void**)m1_operand(0x0046283d))[n] = wob;                   // wobs[n - 1] (M1 repoints the list)
}
static void fp_add_wob(Footprint& f, void* wob) {
    fp_add_gob(f, wob);
    FP_ADD(f, 0x005522d0, 4, "wob count");
    FP_ADD(f, m1_operand(0x0046283d) + 4u + 4u * (uint32_t)g_nwobs, 4, "wobs[count]");
}
PORT_FN(0x00462800, "WorldAddWob", WorldAddWob_rw, fp_add_wob)

static void __cdecl WorldFXEnable_rw() { g_fx = 1; }
static void __cdecl WorldFXDisable_rw() { g_fx = 0; }
static uint8_t __cdecl WorldFXEnabled_rw() { return g_fx; }
static void fp_fx(Footprint& f) { FP_ADD(f, 0x0055404c, 1, "fx enabled"); }
PORT_FN(0x00462850, "WorldFXEnable", WorldFXEnable_rw, fp_fx)
PORT_FN(0x00462860, "WorldFXEnabled", WorldFXEnabled_rw, fp_none)
PORT_FN(0x00462870, "WorldFXDisable", WorldFXDisable_rw, fp_fx)

// ==== World (0x462a70, 0x462a90) ===========================================================================================
static uint8_t __fastcall World_is_valid_version_rw(const World* self, Edx) {
    return (self->version == 3 && self->size == 0xcd4) ? 1 : 0;
}
static void fp_valid_version(Footprint& f, const World*, Edx) { f.pure = true; }
PORT_FN(0x00462a70, "World::is_valid_version", World_is_valid_version_rw, fp_valid_version)

static World* __fastcall World_ctor_rw(World* self, Edx) {
    uint32_t* p = (uint32_t*)self;
    for (int i = 0; i < 0x335; i++) p[i] = 0;                    // rep stosd
    self->size = 0xcd4;
    self->version = 3;
    return self;
}
static void fp_world_ctor(Footprint& f, World* self, Edx) { f.add(self, sizeof(World), "World"); }   // (returns this: not fuzzed)
PORT_FN(0x00462a90, "World::World", World_ctor_rw, fp_world_ctor)

// =====================================================================================================================
// world.obj: the world file
// =====================================================================================================================

// ==== setup_cars (0x462ab0): every car's wheel lock must be in [0.01, 1.01], compared as an int (a negative float,
// or a NaN, panics) =========================================================================================================
static void __cdecl setup_cars_rw(World* w) {
    CarList* cl = &w->cars;
    for (int i = 0; i < cl->count; i++) {
        const int32_t b = (int32_t)Ub(cl->e[i].wheel_lock);
        if (b < (int32_t)k_wheel_lock_min || b > (int32_t)k_wheel_lock_max)
            LogPanic(k_fmt_wheel_lock, i, D(cl->e[i].wheel_lock));
    }
}
static void fp_setup_cars(Footprint&, World*) {}
PORT_FN(0x00462ab0, "setup_cars", setup_cars_rw, fp_setup_cars)

// ==== parse_world (0x462b10) ============================================================================================
// The start line reset to identity and read (get_start_line); then track.obt's rows from 1: each copied (a 0x100
// buffer, unbounded), cut at ';', its first word read: "end" stops, "map" reads its name (unused), "ai" is skipped,
// "obj" goes to parse_object, anything else is reported. Then any cars left over are made. Always 1.
static uint8_t __cdecl parse_world_rw(const World* w) {
    char tok[0x100], line[0x100];
    uint32_t* sl = (uint32_t*)g_start_line;
    for (int k = 0; k < 12; k++) sl[k] = (k == 0 || k == 4 || k == 8) ? k_one : 0;
    StringTable* t = StringTableGet(k_str_track_obt);
    get_start_line_o(g_start_line, t, w->options.reverse);
    int row = 1;
    while (StringTableNumRows(t) > row) {
        const char* e = StringTableGetEntry(t, row, 0);
        row++;
        str_copy(line, e);
        char* c = strchr_o(line, ';');
        if (c) *c = 0;
        if (line[0] == 0) continue;
        if (sscanf_o(line, k_fmt_s, tok) <= 0) continue;
        if (stricmp_o(tok, k_str_end) == 0) break;
        if (tok[0] == ';' || tok[0] == 0) continue;
        if (stricmp_o(tok, k_str_map) == 0) {
            tok[0] = 0;
            sscanf_o(line, k_fmt_map, tok);
            continue;
        }
        if (stricmp_o(tok, k_str_ai) == 0) continue;
        if (stricmp_o(tok, k_str_obj) == 0) parse_object_o(line, &w->cars);
        else LogReport(k_fmt_bad_token, tok);
    }
    StringTableForget(t);
    create_remaining_cars_o(&w->cars);
    return 1;
}
static void fp_parse_world(Footprint& f, const World*) { f.replay_only = "it reads track.obt (a resource) and makes every object"; }
PORT_FN(0x00462b10, "parse_world", parse_world_rw, fp_parse_world)

// ==== get_start_line (0x462d40) ===========================================================================================
// The frame is identity unless the track runs backwards: then it is turned half round (MatrixMakeWorldRotation(pi,
// 0, 0)) about the midpoint M of the first "obj checkpoint" row with at least 4 fields: position -(-M + M R). No
// checkpoint row at all panics. (A row of exactly 4 fields leaves the second z as it was -- uninitialised on the
// first, as the original.)
static void __cdecl get_start_line_rw(Frame* f, StringTable* t, uint8_t reverse) {
    Point2D unused[2];
    char name[0x20];
    float x2, z2, x1, z1;
    float M[3];
    M3 L;
    for (int i = 0; i < 2; i++) Point2D_ctor_o(&unused[i], 0);
    uint32_t* fr = (uint32_t*)f;
    for (int k = 0; k < 12; k++) fr[k] = (k == 0 || k == 4 || k == 8) ? k_one : 0;
    int row = 1;
    for (;;) {
        if (!(StringTableNumRows(t) > row)) {
            LogPanic(k_str_no_checkpoints);
            return;
        }
        const char* e = StringTableGetEntry(t, row, 0);
        if (sscanf_o(e, k_fmt_start_cp, name, &x1, &z1, &x2, &z2) >= 4) break;
        row++;
    }
    for (int k = 0; k < 12; k++) fr[k] = (k == 0 || k == 4 || k == 8) ? k_one : 0;
    if (!reverse) return;
    M[1] = 0.0f;
    M[0] = (float)((D(x2) + D(x1)) * D(Fb(k_half)));
    M[2] = (float)((D(z2) + D(z1)) * D(Fb(k_half)));
    MatrixMakeWorldRotation_o(&L, k_pi, 0, 0);
    MatrixConcat(f, f, &L);
    const float* m = f->rot.m;
    f->pos.x = (float)-D(M[0]);
    f->pos.y = (float)-D(M[1]);
    f->pos.z = (float)-D(M[2]);
    float T[3];
    T[0] = (float)((D(m[6]) * D(M[2]) + D(m[3]) * D(M[1])) + D(m[0]) * D(M[0]));
    T[1] = (float)((D(m[4]) * D(M[1]) + D(m[1]) * D(M[0])) + D(m[7]) * D(M[2]));
    T[2] = (float)((D(m[5]) * D(M[1]) + D(m[2]) * D(M[0])) + D(m[8]) * D(M[2]));
    f->pos.x = (float)(D(f->pos.x) + D(T[0]));
    f->pos.y = (float)(D(f->pos.y) + D(T[1]));
    f->pos.z = (float)(D(f->pos.z) + D(T[2]));
    f->pos.x = (float)-D(f->pos.x);
    f->pos.y = (float)-D(f->pos.y);
    f->pos.z = (float)-D(f->pos.z);
}
static void fp_start_line(Footprint& f, Frame* fr, StringTable*, uint8_t) { f.add(fr, sizeof(Frame), "start line frame"); }
PORT_FN(0x00462d40, "get_start_line", get_start_line_rw, fp_start_line)

// ==== create_remaining_cars (0x462f90) ====================================================================================
static void __cdecl create_remaining_cars_rw(const CarList* cl) {
    while (cl->count > g_cars_made) parse_car_o(k_str_obj_car_00, cl);
}
static void fp_remaining_cars(Footprint& f, const CarList*) { f.replay_only = "it loads car files and makes the cars"; }
PORT_FN(0x00462f90, "create_remaining_cars", create_remaining_cars_rw, fp_remaining_cars)

// ==== parse_car (0x462fc0) ================================================================================================
// "obj car X, Z" makes the next car of the list (none once the list is used up): its <car>.cf loaded into a CarData
// (the career's upgrades for the player), the type by subtype (the player also becomes the focus and player car;
// an AI car's data tweaked; a ghost counted and given its target), placed on the centre line -- slot = its index
// less the ghosts so far -- or, when that fails or byte 0x4f4368 is set, at (X, 0, Z) through the start line, 2 m
// above the ground; a ball for it if the entry asks; then a CarObject or GhostCarObject (0x844) added.
static void __cdecl parse_car_rw(const char* line, const CarList* cl) {
    struct {
        float z, x;                    // B+0x000, +0x004
        CarData cd;                    // B+0x008
        char file[0x40];               // B+0x1ec  "<car>.cf"
    } L;
    L.x = 0.0f;
    L.z = 0.0f;
    sscanf_o(line, k_fmt_obj_car, &L.x, &L.z);
    const int32_t idx = g_cars_made;
    if (!(cl->count > idx)) return;
    g_cars_made = g_cars_made + 1;
    const CarListEntry* e = &cl->e[idx];
    str_copy(L.file, e->car);
    memcpy(L.file + strlen(L.file), k_dot_cf, 4);
    if (L.file[0] == 0) {                                        // (never: ".cf" was just appended)
        LogReport(k_str_bad_car);
        return;
    }
    L.cd.car = idx;
    uint8_t* up = 0;
    if (e->subtype == 0) up = g_career_upgrades;
    CarFileLoad(&L.cd, e->setup, L.file, up);
    const uint32_t st = (uint32_t)e->subtype;
    switch (st) {
    case 0:
        L.cd.h.type = 0x50434152;                                // 'PCAR'
        L.cd.h.size = 0x1e4;
        g_focus = L.cd.car;
        g_player = L.cd.car;
        break;
    case 1:
        L.cd.h.type = 0x41434152;                                // 'ACAR'
        L.cd.h.size = 0x1e4;
        AITweakCarData(&L.cd);
        break;
    case 2:
        L.cd.h.type = 0x4e434152;                                // 'NCAR'
        L.cd.h.size = 0x1e4;
        break;
    case 3:
        L.cd.h.type = 0x47434152;                                // 'GCAR'
        L.cd.h.size = 0x1e4;
        g_ghosts = g_ghosts + 1;
        L.cd.ghost_target = GhostGetTarget();
        break;
    default:
        LogReport(k_fmt_subtype, (int)st);
        return;
    }
    bool placed = false;
    if (g_use_grid_xz == 0 && set_car_frame_from_dlatlong_o(&L.cd.frame, (int32_t)((uint32_t)idx - (uint32_t)g_ghosts)))
        placed = true;
    if (!placed) {
        // (x, 0, z) through the start line: row vector times its rotation, then its position
        const Frame* s = g_start_line;
        const float* m = s->rot.m;
        P3& p = L.cd.frame.pos;
        memcpy(&p.x, &L.x, 4);                                   // mov: a bit copy
        memset(&p.y, 0, 4);
        fcopy(&p.z, &L.z);                                       // fld; fst: through the FPU
        const double z = D(p.z);
        const double y1 = (D(p.x) * D(m[1]) + D(p.y) * D(m[4])) + D(m[7]) * z;
        const double z1 = (D(p.x) * D(m[2]) + D(p.y) * D(m[5])) + D(m[8]) * z;
        const double x1 = (D(p.x) * D(m[0]) + D(p.y) * D(m[3])) + D(m[6]) * z;
        p.x = (float)x1;
        p.y = (float)y1;
        p.z = (float)z1;
        p.x = (float)(D(p.x) + D(s->pos.x));
        p.y = (float)(D(p.y) + D(s->pos.y));
        p.z = (float)(D(p.z) + D(s->pos.z));
        const double h = TerrainGetHeight(Ub(p.x), Ub(p.z));
        p.y = (float)(h + D(Fb(k_two)));
        uint32_t* r = (uint32_t*)L.cd.frame.rot.m;
        for (int k = 0; k < 9; k++) r[k] = (k == 0 || k == 4 || k == 8) ? k_one : 0;
    }
    if (e->ball) create_ball_o(L.cd.car);
    void* obj;
    if (e->subtype == 3) {
        void* p = MemAlloc(0x844);
        obj = p ? GhostCarObject_ctor(p, 0, &L.cd, e) : 0;
    } else {
        void* p = MemAlloc(0x844);
        obj = p ? CarObject_ctor(p, 0, &L.cd, e) : 0;
    }
    WorldAddWob_o(obj);
}
static void fp_parse_car(Footprint& f, const char*, const CarList*) { f.replay_only = "it loads the car's file and allocates the car object"; }
PORT_FN(0x00462fc0, "parse_car", parse_car_rw, fp_parse_car)

// ==== set_car_frame_from_dlatlong (0x4632f0) ==============================================================================
// Grid slot n on the centre line: 28 + 15 n metres back from its end, 4 m to the right for even n and left for odd,
// 0.61 m above the ground, facing along the line. The line (CenterLine(0): loaded for the race's direction) gives
// the segment; HermiteEval its point, ILSeg::QuickTan its tangent. The frame's rows: right (normalised), up (from
// right x forward, normalised), right x up. 0 if the line has no segment there.
static uint8_t __cdecl set_car_frame_rw(Frame* f, int n) {
    alignas(8) uint8_t line[0x70];                               // CenterLine
    Point2D tan;                                                 // B+0x00
    P3 up;                                                       // B+0x08
    float rx, rz;                                                // B+0x14, +0x18: (tz, -tx), the right in the plane
    P3 right;                                                    // B+0x1c
    P3 fwd;                                                      // B+0x28
    Point2D pos;                                                 // B+0x34
    ILinePos ip;                                                 // B+0x3c
    uint32_t off = k_four;
    if (n & 1) off = k_neg_four;
    CenterLine_ctor(line, 0, 0);
    const double back = D(n) * D(Fb(k_fifteen)) + D(Fb(k_twenty_eight));
    const float meters = (float)(D(*(const float*)(line + 0x5c)) - back);   // the line's length less that
    CenterLine_convert_meters_to_ilpos(line, 0, &ip, meters);
    if (ip.seg == 0) {
        CenterLine_dtor(line, 0);
        return 0;
    }
    {
        uint8_t* seg = ip.seg;
        uint8_t* next = *(uint8_t**)seg;
        HermiteEval_o(&pos, seg + 4, next + 4, seg + 0xc, next + 0xc, ip.t);
    }
    ILSeg_QuickTan(ip.seg, 0, &tan, ip.t);
    const double len = x87_sqrt(D(tan.z) * D(tan.z) + D(tan.x) * D(tan.x));
    tan.x = (float)(D(tan.x) / len);
    const double tzn = D(tan.z) / len;
    tan.z = (float)tzn;
    rx = (float)tzn;
    rz = (float)-D(tan.x);
    const float offf = Fb(off);
    pos.x = (float)(D(tan.z) * D(offf) + D(pos.x));
    pos.z = (float)(D(rz) * D(offf) + D(pos.z));
    uint32_t* fr = (uint32_t*)f;
    fr[10] = 0;                                                  // pos.y
    memcpy(&f->pos.x, &pos.x, 4);
    memcpy(&f->pos.z, &pos.z, 4);
    const double h = TerrainGetHeight(Ub(pos.x), Ub(pos.z)) + D(Fb(k_wheel_height));
    memcpy(&fr[0], &rx, 4);
    fr[1] = 0;
    f->pos.y = (float)h;
    memcpy(&fr[2], &rz, 4);
    memcpy(&fr[6], &tan.x, 4);
    fr[7] = 0;
    memcpy(&fr[8], &tan.z, 4);
    fr[3] = 0;
    fr[5] = 0;
    memcpy(&fwd.x, &tan.x, 4);
    memset(&fwd.y, 0, 4);
    memcpy(&fwd.z, &tan.z, 4);
    fr[4] = k_one;
    // up = right x forward, which is (0, y, 0)   (the original's alias check between two locals: both arms equal)
    const double uy = D(rx) * D(tan.z) - D(rz) * D(tan.x);
    memset(&up.x, 0, 4);
    memset(&up.z, 0, 4);
    up.y = (float)uy;
    const double inv = D(Fb(k_one)) / x87_sqrt(D(rz) * D(rz) + D(rx) * D(rx));
    right.x = (float)(D(rx) * inv);
    memset(&right.y, 0, 4);
    right.z = (float)(inv * D(rz));
    const double inv2 = D(Fb(k_one)) / x87_sqrt((D(up.y) * D(up.y) + D(up.z) * D(up.z)) + D(up.x) * D(up.x));
    up.x = (float)(D(up.x) * inv2);
    up.y = (float)(D(up.y) * inv2);
    up.z = (float)(inv2 * D(up.z));
    CrossProduct(&fwd, &right, &up);
    memcpy(&fr[0], &right, 12);
    memcpy(&fr[3], &up, 12);
    memcpy(&fr[6], &fwd, 12);
    CenterLine_dtor(line, 0);
    return 1;
}
static void fp_car_frame(Footprint& f, Frame*, int) { f.replay_only = "it builds a CenterLine (loads the race's line, allocates)"; }
PORT_FN(0x004632f0, "set_car_frame_from_dlatlong", set_car_frame_rw, fp_car_frame)

// ==== HermiteEval (0x4635f0) ================================================================================================
// The cubic Hermite point at t between a and b with tangents c and d (all in registers; each sum in the original's
// order).
static void __cdecl HermiteEval_rw(Point2D* out, const Point2D* a, const Point2D* b, const Point2D* c, const Point2D* d, float t) {
    const double T = D(t);
    const double h1 = ((T * D(Fb(k_two)) - D(Fb(k_three))) * T) * T + D(Fb(k_one));
    const double h2 = ((T * D(Fb(k_neg_two)) + D(Fb(k_three))) * T) * T;
    const double h3 = ((T - D(Fb(k_two))) * T + D(Fb(k_one))) * T;
    const double h4 = ((T - D(Fb(k_one))) * T) * T;
    out->x = (float)(((D(b->x) * h2 + D(a->x) * h1) + D(d->x) * h4) + D(c->x) * h3);
    out->z = (float)(((h4 * D(d->z) + h3 * D(c->z)) + h2 * D(b->z)) + h1 * D(a->z));
}
static void fp_hermite(Footprint& f, Point2D* out, const Point2D*, const Point2D*, const Point2D*, const Point2D*, float) {
    f.add(out, sizeof(Point2D), "out");
    f.pure = true;
}
PORT_FN(0x004635f0, "HermiteEval(world.obj)", HermiteEval_rw, fp_hermite)

// ==== create_ball (0x4636a0): a 'BALL' ModelObject (ball.mod) 5000 m above the ground at the origin ========================
// Its mass (3000) and collision radius (18 inches) are immediates vrmod's hornball.py sets to the player's values: they
// are read from the original's instructions (port.h, vrmod_operand: the stock values in a stock race.exe).
static void __cdecl create_ball_rw(int car) {
    BallData b;
    b.h.type = 0x42414c4c;                                       // 'BALL'
    memset(&b.pos.x, 0, 4);
    memset(&b.pos.z, 0, 4);
    const double h = TerrainGetHeight(0, 0) + D(Fb(k_ball_height));
    const uint32_t k5000 = k_ball_height;
    memcpy(&b.inertia[0], &k5000, 4);
    memcpy(&b.inertia[1], &k5000, 4);
    b.h.size = 0x38;
    b.pos.y = (float)h;
    memcpy(&b.inertia[2], &k5000, 4);
    memcpy(&b._28, &k5000, 4);
    const uint32_t k3000 = vrmod_operand(0x004636f2);            // mov [esp+8], 3000.0 (the mass)
    const uint32_t k18 = vrmod_operand(0x00463700);              // mov [esp+0x28], 18.0 (the radius, inches)
    const uint32_t k06 = 0x3f19999a, k5 = 0x40a00000;
    memcpy(&b.mass, &k3000, 4);
    memcpy(&b.radius, &k18, 4);
    memcpy(&b._2c, &k06, 4);
    memcpy(&b._30, &k5, 4);
    b.car = car;
    void* p = MemAlloc(0x40);
    void* obj = p ? ModelObject_ctor(p, 0, &b, k_str_ball_mod) : 0;
    WorldAddWob_o(obj);
}
static void fp_create_ball(Footprint& f, int) { f.replay_only = "it allocates a ModelObject (which loads ball.mod)"; }
PORT_FN(0x004636a0, "create_ball", create_ball_rw, fp_create_ball)

// ==== parse_object (0x463750): "obj <kind>" to its parser (statics only without HackNoWalls) ================================
static void __cdecl parse_object_rw(const char* line, const CarList* cl) {
    char kind[0x50];
    memset(kind, 0, sizeof kind);
    sscanf_o(line, k_fmt_obj, kind);
    if (stricmp_o(kind, k_str_car) == 0) {
        parse_car_o(line, cl);
        return;
    }
    if (stricmp_o(kind, k_str_obstacle) == 0) {
        parse_obstacle_o(line);
        return;
    }
    if (stricmp_o(kind, k_str_wobble) == 0) {
        parse_wobble_o(line);
        return;
    }
    if (stricmp_o(kind, k_str_static) == 0) {
        if (!HackNoWalls()) parse_static_o(line);
        return;
    }
    if (stricmp_o(kind, k_str_checkpoint) == 0) {
        parse_checkpoint_o(line);
        return;
    }
    LogReport(k_fmt_unknown_obj, kind);
}
static void fp_parse_object(Footprint& f, const char*, const CarList*) { f.replay_only = "it makes a world object (allocates, loads its model)"; }
PORT_FN(0x00463750, "parse_object", parse_object_rw, fp_parse_object)

// ==== parse_obstacle (0x463870) ==============================================================================================
// "obj obstacle <ball|cube|prism> <model> X,Z:R MASS": the model's extents give its size and inertia, its lowest
// point is put 4 m above the ground; a ModelObject (0x40) over the OBST data. A bad shape panics (the field is
// then whatever was on the stack).
static void __cdecl parse_obstacle_rw(const char* line) {
    float mass, z, x, miny, rot, maxx, minx, maxy, maxz, minz;   // B+0, +4, +8, +0xc, +0x10, +0x14, +0x18, +0x1c, +0x20, +0x24
    ObstacleData d;                                              // B+0x28
    char shape[0x50], model[0x50];                               // B+0x60, B+0xb0
    memset(shape, 0, sizeof shape);
    memset(model, 0, sizeof model);
    if (sscanf_o(line, k_fmt_obstacle, shape, model, &x, &z, &rot, &mass) != 6) {
        LogReport(k_str_bad_obstacle);
        return;
    }
    memcpy(&d.rot, &rot, 4);
    d.h.type = 0x4f425354;                                       // 'OBST'
    d.h.size = 0x38;
    const double g = TerrainGetHeight(Ub(x), Ub(z));
    d.pos.y = (float)g;
    memcpy(&d.pos.x, &x, 4);
    memcpy(&d.pos.z, &z, 4);
    if (stricmp_o(shape, k_str_ball) == 0) d.shape = 0;
    else if (stricmp_o(shape, k_str_cube) == 0) d.shape = 1;
    else if (stricmp_o(shape, k_str_prism) == 0) d.shape = 2;
    else LogPanic(k_fmt_bad_obstacle_type, shape);
    const int h = mrModelLoad(model);
    mrModelGetExtents(h, &minx, &maxx, &miny, &maxy, &minz, &maxz);
    d.pos.y = (float)((D(d.pos.y) - D(miny)) + D(Fb(k_four)));
    d.size.x = (float)(D(maxx) - D(minx));
    memcpy(&d.mass, &mass, 4);
    d.size.y = (float)(D(maxy) - D(miny));
    d.size.z = (float)(D(maxz) - D(minz));
    d.inertia[0] = (float)(((D(d.size.y) * D(d.size.y)) * D(Fb(k_inertia))) * D(mass));
    d.inertia[1] = (float)(((D(d.size.z) * D(d.size.z)) * D(Fb(k_inertia))) * D(mass));
    d.inertia[2] = (float)(((D(d.size.x) * D(d.size.x)) * D(Fb(k_inertia))) * D(mass));
    mrModelUnload(h);
    void* p = MemAlloc(0x40);
    void* obj = p ? ModelObject_ctor(p, 0, &d, model) : 0;
    WorldAddWob_o(obj);
}
static void fp_parse_line(Footprint& f, const char*) { f.replay_only = "it makes a world object (allocates, loads its model)"; }
PORT_FN(0x00463870, "parse_obstacle", parse_obstacle_rw, fp_parse_line)

// ==== parse_wobble (0x463ab0): "obj wobble <pole|flap> ID": a WobbleObject (0x40) over WOBL data ============================
static void __cdecl parse_wobble_rw(const char* line) {
    int32_t id;
    WobbleData d;
    char kind[0x50];
    memset(kind, 0, sizeof kind);
    if (sscanf_o(line, k_fmt_wobble, kind, &id) != 2) {
        LogReport(k_str_bad_wobble);
        return;
    }
    const uint32_t k13140 = 0x46435000, k10000 = 0x461c4000;
    d.h.type = 0x574f424c;                                       // 'WOBL'
    memcpy(&d._0c[0], &k13140, 4);
    memcpy(&d._0c[1], &k13140, 4);
    memcpy(&d._0c[2], &k13140, 4);
    d.h.size = 0x20;
    memcpy(&d._08, &k10000, 4);
    if (stricmp_o(kind, k_str_pole) == 0) d.kind = 0;
    else if (stricmp_o(kind, k_str_flap) == 0) d.kind = 1;
    else LogPanic(k_fmt_bad_wobble_type, kind);
    d.id = id;
    void* p = MemAlloc(0x40);
    void* obj = p ? WobbleObject_ctor(p, 0, &d) : 0;
    WorldAddWob_o(obj);
}
PORT_FN(0x00463ab0, "parse_wobble", parse_wobble_rw, fp_parse_line)

// ==== parse_static (0x463bc0) ================================================================================================
// "obj static box X,Y,Z A,B,C W,H,D": a box at (X,Y,Z), rotated about the axis (A,B,C) by its length in radians
// (identity when that is 1e-4 or less, or NaN), of extents (W,H,D), created straight in the physics (STAT). The
// rotation keeps the x87's fsin / fcos in registers across a dozen operations, so it is the original's own
// instruction sequence (static_rotation, below).
static void __declspec(noinline) static_rotation(const float* len_p, const float* a, float* R) {
    static const float one = 1.0f;
    float lenv = *len_p, a0 = a[0], a1 = a[1], a2 = a[2];
    float A, Bv, C, Dd, E;
    float r0, r1, r2, r3, r4, r5, r6, r7, r8;
    __asm {
        fld     lenv
        fdivr   one                    ; 1 / len
        fld     a0
        fmul    st, st(1)              ; n0
        fld     a1
        fmul    st, st(2)              ; n1
        fxch    st(2)
        fmul    a2                     ; n2
        fld     lenv
        fsin
        fld     lenv
        fcos
        fld     one
        fsub    st, st(1)              ; t = 1 - c        st: t c s n2 n0 n1
        fld     st(4)
        fmul    st, st(5)
        fmul    st, st(1)
        fadd    st, st(2)
        fstp    r0                     ; n0 n0 t + c
        fld     st(0)
        fmul    st, st(6)
        fmul    st, st(5)
        fstp    A                      ; t n1 n0
        fld     st(2)
        fmul    st, st(4)
        fst     Bv                     ; s n2
        fadd    A
        fstp    r1
        fld     st(0)
        fmul    st, st(4)
        fst     C                      ; t n2
        fmul    st, st(5)
        fstp    Dd                     ; t n2 n0
        fld     st(2)
        fmul    st, st(6)
        fst     E                      ; s n1
        fsubr   Dd
        fstp    r2
        fld     A
        fsub    Bv
        fstp    r3
        fld     st(5)
        fmul    st, st(6)
        fmul    st, st(1)
        fadd    st, st(2)
        fstp    r4                     ; n1 n1 t + c
        fxch    st(5)
        fmul    C
        fxch    st(2)
        fmul    st, st(4)
        fld     st(2)
        fadd    st, st(1)
        fstp    r5
        fld     Dd
        fadd    E
        fstp    r6
        _emit 0xdc                     ; fsub st(2), st(0): st(2) = st(2) - st(0) (DC E8+i)
        _emit 0xea
        fxch    st(2)
        fstp    r7
        fxch    st(2)
        _emit 0xdc                     ; fmul st(0), st(0) (DC C8)
        _emit 0xc8
        fmul    st, st(4)
        fadd    st, st(2)
        fstp    r8                     ; n2 n2 t + c
        fstp    st(0)
        fstp    st(0)
        fstp    st(0)
        fstp    st(0)
    }
    R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3; R[4] = r4; R[5] = r5; R[6] = r6; R[7] = r7; R[8] = r8;   // (float moves)
}
static void __cdecl parse_static_rw(const char* line) {
    float len;                                                   // B+0x00 (then PhysicsCreate's message offset)
    float axis[3];                                               // B+0x04
    float ext[3];                                                // B+0x20
    StaticData d;                                                // B+0x2c
    float pos[3];                                                // B+0x74
    char kind[0x50];                                             // B+0x80
    memset(kind, 0, sizeof kind);
    if (sscanf_o(line, k_fmt_static, kind, &pos[0], &pos[1], &pos[2], &axis[0], &axis[1], &axis[2], &ext[0], &ext[1],
                 &ext[2]) != 10) {
        LogReport(k_str_bad_static);
        return;
    }
    const double l = x87_sqrt((D(axis[1]) * D(axis[1]) + D(axis[2]) * D(axis[2])) + D(axis[0]) * D(axis[0]));
    d.h.type = 0x53544154;                                       // 'STAT'
    d.h.size = 0x48;
    len = (float)l;
    if (l > D(Fb(k_min_axis))) {
        uint32_t R[9];
        static_rotation(&len, axis, (float*)R);
        memcpy(d.rot.m, R, 36);
    } else {
        uint32_t* r = (uint32_t*)d.rot.m;
        for (int k = 0; k < 9; k++) r[k] = (k == 0 || k == 4 || k == 8) ? k_one : 0;
    }
    memcpy(&d.pos, pos, 12);
    if (stricmp_o(kind, k_str_box) == 0) {
        memcpy(&d.extent, ext, 12);
        d.kind = 0;
    } else {
        LogPanic(k_fmt_bad_static_type, kind);
    }
    PhysicsCreate(&d, (int*)&len);
}
PORT_FN(0x00463bc0, "parse_static", parse_static_rw, fp_parse_line)

// ==== parse_checkpoint (0x463e30) =============================================================================================
// "obj checkpoint <name> X,Z X,Z": a gate between the two points on the ground; a ModelObject (checkpt1.mod) when the
// options show checkpoints, else created straight in the physics.
static void __cdecl parse_checkpoint_rw(const char* line) {
    float z1, x1, z2, x2;                                        // B+0, +4, +8, +0xc
    int32_t msg;                                                 // B+0x10
    CheckPointData d;                                            // B+0x14
    char name[0x50];                                             // B+0x34 (not cleared)
    if (sscanf_o(line, k_fmt_checkpoint, name, &x1, &z1, &x2, &z2) != 5) {
        LogReport(k_str_bad_checkpoint);
        return;
    }
    d.h.type = 0x43484b50;                                       // 'CHKP'
    d.h.size = 0x20;
    d.a.y = (float)TerrainGetHeight(Ub(x1), Ub(z1));
    memcpy(&d.a.z, &z1, 4);
    memcpy(&d.a.x, &x1, 4);
    d.b.y = (float)TerrainGetHeight(Ub(x2), Ub(z2));
    memcpy(&d.b.x, &x2, 4);
    memcpy(&d.b.z, &z2, 4);
    if (g_world->options.show_checkpoints != 0) {
        void* p = MemAlloc(0x40);
        void* obj = p ? ModelObject_ctor(p, 0, &d, k_str_checkpt_mod) : 0;
        WorldAddWob_o(obj);
        return;
    }
    msg = 0;
    PhysicsCreate(&d, &msg);
}
PORT_FN(0x00463e30, "parse_checkpoint", parse_checkpoint_rw, fp_parse_line)

// ==== sort_objects (0x463f40), obj_sort_f (0x463f60) =============================================================================
// The graphics objects sorted (the game's qsort) by their +0x1c virtual's key, ascending (the difference, wrapping).
static void __cdecl sort_objects_rw() {                          // push gobs: the list as the original has it (M1)
    qsort_o((void*)m1_operand(0x00463f4e), (uint32_t)g_ngobs, 4, obj_sort_f_o);
}
static void fp_sort_objects(Footprint& f) { FP_ADD(f, m1_operand(0x00463f4e), 4u * (uint32_t)(g_ngobs > 0 ? g_ngobs : 0), "gobs"); }
PORT_FN(0x00463f40, "sort_objects", sort_objects_rw, fp_sort_objects)

static int __cdecl obj_sort_f_rw(const void* a, const void* b) {
    void* ga = *(void* const*)a;
    void* gb = *(void* const*)b;
    const int ka = VFN(ga, 0x1c, int)(ga, 0);
    const int kb = VFN(gb, 0x1c, int)(gb, 0);
    return (int)((uint32_t)ka - (uint32_t)kb);
}
static void fp_sort_f(Footprint&, const void*, const void*) {}
PORT_FN(0x00463f60, "obj_sort_f", obj_sort_f_rw, fp_sort_f)

// ==== should_draw_mirror (0x463f90): the mirror option on and the camera 0, 1 or 2 ===============================================
static uint8_t __cdecl should_draw_mirror_rw() {
    if (g_opt_mirror > 0) {
        const int32_t c = g_camera_type;
        if (c == 0 || c == 2 || c == 1) return 1;
    }
    return 0;
}
PORT_FN(0x00463f90, "should_draw_mirror", should_draw_mirror_rw, fp_none)

// ==== CarInfo::CarInfo (0x463fc0), Point2D::Point2D (0x463fd0): return this ======================================================
static void* __fastcall CarInfo_ctor_rw(void* self, Edx) { return self; }
static void fp_trivial_ctor(Footprint&, void*, Edx) {}
PORT_FN(0x00463fc0, "CarInfo::CarInfo", CarInfo_ctor_rw, fp_trivial_ctor)
static void* __fastcall Point2D_ctor_rw(void* self, Edx) { return self; }
PORT_FN(0x00463fd0, "Point2D::Point2D", Point2D_ctor_rw, fp_trivial_ctor)

// ==== MatrixMakeWorldRotation (0x463fe0) ==========================================================================================
// Ry(a) (cos, 0, -sin / 0, 1, 0 / sin, 0, cos), then times Rx(b) and Rz(c) (MatrixConcat(m, m, L)). Each fsin / fcos
// result goes straight to a float (negated: the rounding is symmetric, so -round(x) = round(-x)).
static void __cdecl MatrixMakeWorldRotation_rw(M3* m, float a, float b, float c) {
    float* M = m->m;
    const float ca = x87_cos_f(D(a));
    const float sa = x87_sin_f(D(a));
    uint32_t* u = (uint32_t*)M;
    u[1] = 0;
    u[3] = 0;
    u[4] = k_one;
    u[5] = 0;
    u[7] = 0;
    M[0] = ca;
    M[2] = -sa;
    M[6] = sa;
    M[8] = ca;
    M3 L;
    uint32_t* l = (uint32_t*)L.m;
    const float cb = x87_cos_f(D(b));
    l[3] = 0;
    l[0] = k_one;
    l[2] = 0;
    l[1] = 0;
    l[6] = 0;
    const float sb = x87_sin_f(D(b));
    L.m[4] = cb;
    L.m[5] = sb;
    L.m[7] = -sb;
    L.m[8] = cb;
    MatrixConcat(m, m, &L);
    const float cc = x87_cos_f(D(c));
    l[2] = 0;
    const float sc = x87_sin_f(D(c));
    l[5] = 0;
    l[6] = 0;
    l[7] = 0;
    l[8] = k_one;
    L.m[0] = cc;
    L.m[1] = sc;
    L.m[3] = -sc;
    L.m[4] = cc;
    MatrixConcat(m, m, &L);
}
static void fp_make_rotation(Footprint& f, M3* m, float, float, float) {
    f.add(m, sizeof(M3), "matrix");
    f.pure = true;
}
PORT_FN(0x00463fe0, "MatrixMakeWorldRotation", MatrixMakeWorldRotation_rw, fp_make_rotation)

// =====================================================================================================================
// carmgr.obj
// =====================================================================================================================
static void fp_carmgr_statics(Footprint& f) {
    FP_ADD(f, 0x004f48d0, 12, "carmgr count / began / single");
    FP_ADD(f, 0x00554078, 16, "carmgr registered");
    FP_ADD(f, 0x00554090, 16 * sizeof(CarMgrInfo), "carmgr infos");
}

// ==== CarMgrBegin (0x464320), CarMgrEnd (0x464370) ==================================================================================
static void __cdecl CarMgrBegin_rw() {
    g_carmgr_single = SingleBegin(k_str_carmgr);
    g_carmgr_count = 0;
    memset(g_carmgr, 0, 16 * sizeof(CarMgrInfo));
    uint32_t* r = (uint32_t*)g_car_registered;
    r[0] = 0;
    g_carmgr_began = 1;
    r[1] = 0;
    r[2] = 0;
    r[3] = 0;
}
static void fp_carmgr_begin(Footprint& f) { f.replay_only = "it creates the car manager's lock"; }
PORT_FN(0x00464320, "CarMgrBegin", CarMgrBegin_rw, fp_carmgr_begin)

static void __cdecl CarMgrEnd_rw() {
    SingleEnd(g_carmgr_single, 0, 0);
    g_carmgr_began = 0;
}
static void fp_carmgr_end(Footprint& f) { f.replay_only = "it destroys the car manager's lock"; }
PORT_FN(0x00464370, "CarMgrEnd", CarMgrEnd_rw, fp_carmgr_end)

// ==== CarMgrRegisterCar (0x464390) =====================================================================================================
// A car's type, humanity, message, names (strcpy, unbounded); the count covers it. It enters the car manager's lock
// and never leaves it (as the original).
static void __cdecl CarMgrRegisterCar_rw(int car, const void* msg, const char* name, const char* driver, int type) {
    SingleEnter(g_carmgr_single, 0, 0);
    ASSERT_MSG_carmgr((car >= 0 && car < 16) ? 1 : 0, k_fmt_not_a_car, k_str_fn_register, car);
    CarMgrInfo* i = (CarMgrInfo*)(uintptr_t)(0x00554090u + (uint32_t)car * 0x170u);
    if (type == 0 || (type == 2 && MultiCarIsHuman(car))) i->human = 1;
    else i->human = 0;
    i->msg = msg;
    i->type = type;
    g_car_registered[car] = 1;
    if (car >= g_carmgr_count) g_carmgr_count = car + 1;
    str_copy(i->name, name);
    str_copy(i->driver, driver);
}
static void fp_register_car(Footprint& f, int, const void*, const char*, const char*, int) {
    f.replay_only = "it enters the car manager's lock and never leaves it: a second pass would enter it twice";
}
PORT_FN(0x00464390, "CarMgrRegisterCar", CarMgrRegisterCar_rw, fp_register_car)

// ==== the readers ==========================================================================================================================
static int __cdecl CarMgrCount_rw() { return g_carmgr_count; }
PORT_FN(0x00464480, "CarMgrCount", CarMgrCount_rw, fp_none)

static const CarMgrInfo* __cdecl CarMgrGetInfo_rw(int car) {
    int n = 0;
    if (car >= 0) n = CarMgrCount_o();
    int ok = 0;
    if (car >= 0 && CarMgrCount_o() > car) ok = 1;
    ASSERT_MSG_carmgr(ok, k_fmt_out_of_range, k_str_fn_getinfo, car, car >= 0 ? k_str_ge : k_str_lt, n);
    return (const CarMgrInfo*)(uintptr_t)(0x00554090u + (uint32_t)car * 0x170u);
}
static void fp_get_info(Footprint&, int) {}
PORT_FN(0x00464490, "CarMgrGetInfo", CarMgrGetInfo_rw, fp_get_info)

static void __cdecl CarMgrUpdateCar_rw(int car, const void* info) {
    int n = 0;
    if (car >= 0) n = CarMgrCount_o();
    int ok = 0;
    if (car >= 0 && CarMgrCount_o() > car) ok = 1;
    ASSERT_MSG_carmgr(ok, k_fmt_out_of_range2, k_str_fn_update, car, car >= 0 ? k_str_ge2 : k_str_lt2, n);
    movsd((void*)(uintptr_t)(0x005541c8u + (uint32_t)car * 0x170u), info, 14);
}
static void fp_update_car(Footprint& f, int car, const void*) {
    FP_ADD(f, 0x005541c8u + (uint32_t)car * 0x170u, 0x38, "carmgr info[car].info");
}
PORT_FN(0x004644f0, "CarMgrUpdateCar", CarMgrUpdateCar_rw, fp_update_car)

static char* __cdecl CarMgrGetStatusBuf_rw(int car) {
    ASSERT_MSG_carmgr((car >= 0 && car < 16) ? 1 : 0, k_fmt_not_a_car2, k_str_fn_status, car);
    return (char*)(uintptr_t)(0x005540c2u + (uint32_t)car * 0x170u);
}
static void fp_status_buf(Footprint&, int) {}
PORT_FN(0x00464560, "_CarMgrGetStatusBuf", CarMgrGetStatusBuf_rw, fp_status_buf)

static void* __fastcall CarMgrInfo_ctor_rw(void* self, Edx) { return self; }
PORT_FN(0x004645a0, "CarMgrInfo::CarMgrInfo", CarMgrInfo_ctor_rw, fp_trivial_ctor)

// =====================================================================================================================
// event.obj: 16 handlers {function, event type}, no bounds check
// =====================================================================================================================
static void __cdecl EventBegin_rw() { g_nhandlers = 0; }
static void fp_event_begin(Footprint& f) { FP_ADD(f, 0x00557f60, 4, "handler count"); }
PORT_FN(0x004691d0, "EventBegin", EventBegin_rw, fp_event_begin)

static void __cdecl EventInstallHandler_rw(void* fn, uint8_t type) {   // int (__cdecl*)(void const*), unsigned char
    ((void**)0x00557ee0)[g_nhandlers * 2] = fn;                  // the count is read again after this store
    ((uint8_t*)0x00557ee4)[g_nhandlers * 8] = type;
    g_nhandlers = g_nhandlers + 1;
}
static void fp_event_install(Footprint& f, void*, uint8_t) {
    FP_ADD(f, 0x00557ee0u + 8u * (uint32_t)g_nhandlers, 8, "handlers[count]");
    FP_ADD(f, 0x00557f60, 4, "handler count");
}
PORT_FN(0x004691e0, "EventInstallHandler", EventInstallHandler_rw, fp_event_install)

// the first match is replaced by the last entry (both dwords), and the count drops; none panics
static void __cdecl EventUnInstallHandler_rw(void* fn) {
    uint8_t found = 0;
    for (int i = 0; i < g_nhandlers; i++) {
        if ((void*)g_handlers[i].fn == fn) {
            g_nhandlers = g_nhandlers - 1;
            const uint32_t* src = (const uint32_t*)(uintptr_t)(0x00557ee0u + 8u * (uint32_t)g_nhandlers);
            const uint32_t w0 = src[0], w1 = src[1];
            uint32_t* dst = (uint32_t*)&g_handlers[i];
            dst[0] = w0;
            found = 1;
            dst[1] = w1;
            break;
        }
    }
    if (!found) LogPanic(k_str_no_handler);
}
static void fp_event_uninstall(Footprint& f, void*) {
    FP_ADD(f, 0x00557ee0, 16 * 8, "handlers");
    FP_ADD(f, 0x00557f60, 4, "handler count");
}
PORT_FN(0x00469210, "EventUnInstallHandler", EventUnInstallHandler_rw, fp_event_uninstall)

// ==== EventDispatch (0x469270) =========================================================================================================
// The physics' event queue: each event is a type byte and its data; every handler of that type is called with the
// data and returns its length (the last one's counts); an event no handler takes (-1) ends the dispatch.
static void __cdecl EventDispatch_rw(const PhysicsEventStream* s) {
    const uint8_t* base = (const uint8_t*)s + 4;
    int32_t pos = 0;
    while ((int32_t)s->len > pos) {
        int32_t r = -1;
        for (int i = 0; i < g_nhandlers; i++) {
            const EventEntry* h = &g_handlers[i];
            if (base[pos] == h->type) r = h->fn(base + pos + 1);
        }
        if (r == -1) break;
        pos = (int32_t)((uint32_t)r + (uint32_t)pos + 1u);
    }
}
static void fp_event_dispatch(Footprint& f, const PhysicsEventStream*) { f.replay_only = "it runs the event handlers (effects, sounds), which write what can't be bounded"; }
PORT_FN(0x00469270, "EventDispatch", EventDispatch_rw, fp_event_dispatch)
