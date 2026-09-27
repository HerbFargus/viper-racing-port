// phys_task.cpp -- M3 3.6 group T1: the physics task and its API, rewritten. physics:phystask.obj (the task's
// begin / end / restart / tick, the phob list, the static-object quadtree walk, the frame renormaliser, the
// timer conditioner, the event queue -- all but the camera code and what earlier stages ported), all of
// physics:physics.obj (the main thread's API to the physics task, the timer thread's physics_thread and its
// TimerWatchdog) and the 13 DriverGet* readers of physics:driver.obj.
//
// Threads. The physics runs on the game's timer thread: PhysicsStart hooks physics_thread (0x42c050) into the
// background timer (BGHook), which calls it every tick; it sets the x87 to single precision
// (ExceptSinglePrecision(1)) and runs the task's state machine (physics.obj's `state`, 0x4ecf88):
//   1 begin (PhysTaskBegin, -> 2)   2 idle   3 run (PhysTaskUpdate)   4 restart (PhysTaskRestart, -> 3)
//   5 post-race (PhysTaskPostRace + PhysTaskUpdate, -> 6)   6 run   7 end (PhysTaskEnd, -> 8)   8 ended
// The main thread drives it through physics.obj: PhysicsStart / Restart / PostRace / Stop set the state and
// wait (TaskSleep(10)) for the timer thread to move it on. So everything phystask.obj does runs on the timer
// thread, and physics.obj's API on the main thread -- except PhysicsGetTime, PhysicsIsPaused, the getters and
// PhysicsEventStream::operator= (both). The main-thread functions that write list what they write in their
// footprints (the shadow check saves the physics statics only on the physics thread): the pause flag
// (0x521608), the time string (0x5215d8), speed, temperature, camera type and focus, the abort flag.
//
// What can't be shadow-checked is replay_only: whatever runs the tick or the task (PhysTaskUpdate,
// update_phobs, physics_thread, PhysTaskBegin/End/Restart/PostRace), allocates or frees (create_phob,
// make_phob, PhysicsBegin/End, PhysicsCreate's stream), waits on the other thread (PhysicsStart/Stop/Restart/
// PostRace), takes the shared-memory locks (the state packet), reads the input devices (PhysicsReadControls ->
// DriverUpdate), writes a file (TimerWatchdog::dump), or reads the real-time clock (PTimeNow: GetTicks,
// PhysicsDeviation, PhysicsGetDeviation, TimerWatchdog::tick_begin / tick_end -- two passes see two times).
//
// Skipped: ASSERT_MSG (0x426d70) and PhysicsMaybeRun (0x42ba20) are bare `ret`s (ASSERT_MSG is still called,
// by address, where the original calls it); the $E initialisers.
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "fix_paths.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t Ub(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }

// ---- layouts -----------------------------------------------------------------------------------------------------
namespace {

// TimerConditioner (the one at 0x521038, 16 bytes): turns real time into a number of 16 ms ticks per call
struct TimerConditioner {
    float time;                        // +0x0 the physics time at the last resync (PhysicsGetTime)
    int32_t ptime;                     // +0x4 the real time then (PTimeNow, ms)
    uint8_t half_toggle;               // +0x8 PhysicsSpeed 2 (half speed): 1, 0, 1, 0 ticks
    uint8_t resync;                    // +0x9 set: the next call re-bases time/ptime and runs one tick
    uint8_t _0a[6];
};
static_assert(sizeof(TimerConditioner) == 16, "TimerConditioner");

// TimerWatchdog (the one at 0x521668, 0x21c bytes -- the recovered type's 288 is only its last seen use): a
// histogram of the timer thread's tick-to-tick jitter, written to c:\timer.log by PhysicsStop (the FIX: to
// <race.exe's folder>\log\timer.log; see TimerWatchdog::dump)
struct TimerWatchdog {
    int32_t ticks;                     // +0x00 tick_begin calls; the first 100 aren't measured
    int32_t measured;                  // +0x04
    float last;                        // +0x08 the previous tick's start (ms, as a float); 0 = none yet
    int32_t tick_start;                // +0x0c PTimeNow at tick_begin
    int32_t slowest;                   // +0x10 creeps up by 1 (to 32) when a tick takes longer; init 8
    int32_t outliers;                  // +0x14 intervals off by 64 ms or more
    int32_t outlier_sum;               // +0x18 the sum of their |ftol| errors
    int32_t hist[128];                 // +0x1c interval - 16 ms, from -64 to 63
};
static_assert(offsetof(TimerWatchdog, hist) == 0x1c && sizeof(TimerWatchdog) == 0x21c, "TimerWatchdog");

// the event queue the physics posts to the main thread (sparks, damage): a length and the bytes, each event a
// type byte and its data; copied into the shared packet at +0x3d8 every tick
struct PhysicsEventStream { uint32_t len; uint8_t data[0x400]; };
static_assert(sizeof(PhysicsEventStream) == 0x404, "PhysicsEventStream");

// the track's static objects (track.sol, volume.obj's StaticObjectListGet) and their quadtree
struct StaticObject {
    Frame frame;                       // +0x00
    int32_t id;                        // +0x30 PhysTaskFindStaticObject's key
    uint32_t type_tag;                 // +0x34
    CollisionVolume* volume;           // +0x38 what collide_object collides with
    uint8_t storage[0xa4];             // +0x3c
};
static_assert(sizeof(StaticObject) == 0xe0, "StaticObject");
struct QuadNode {                      // 8 bytes
    uint16_t first, end;               // +0 +2 its objects: index[first .. end)
    uint16_t child;                    // +4 its four children are nodes child..child+3 (x-/z-, x+/z-, x-/z+, x+/z+); 0 = leaf
    uint16_t _6;
};
struct StaticObjectList {
    int32_t count;                     // +0x00
    int32_t count2;                    // +0x04
    StaticObject* objects;             // +0x08
    uint16_t* index;                   // +0x0c object indices, by node
    QuadNode* nodes;                   // +0x10
};

struct PhobData { uint32_t type; int32_t size; };                 // a phob's creation record (FourCC, bytes)
struct MsgSize { uint32_t type; int32_t size; };                  // PhysTaskGetMsgSize's table (0x4ec9e0)

}  // namespace

// ---- statics -----------------------------------------------------------------------------------------------------
// phystask.obj
#define g_tick_overflow (*(uint8_t*)0x004ec9d0)            // GetTicks: more real time went by than it ran
#define g_task_single   (*(int32_t*)0x004ec9d4)            // SingleBegin("PhysTask")
#define g_shm_w         (*(uint32_t*)0x004ec9d8)           // the shared state packet (the task writes it)
#define k_msg_sizes     ((const MsgSize*)0x004ec9e0)
#define g_renorm_index  (*(int32_t*)0x004eca38)            // renormalize_phobs: whose frame next
#define g_phobs         (*(PhobRoot***)0x00520bb4)
#define g_cars          ((void**)0x00520c18)                // Car*[16], PhysTaskRegisterCar
#define g_collide_phob  (*(PhobRoot**)0x00520cb0)          // collide_object: the object being collided
#define g_timer         ((TimerConditioner*)0x00521038)
#define g_collide_x     (*(int32_t*)0x00521044)            // its position x10, as ints
#define g_collide_z     (*(int32_t*)0x00521048)
#define g_nphobs        (*(int32_t*)0x00521090)
#define g_events        ((PhysicsEventStream*)0x005210e0)
#define g_static_list   (*(StaticObjectList**)0x005214e4)
#define g_deity         (*(void**)0x005218ac)
// update_camera's state, which PhysTaskBegin resets
#define g_cam_52108c    (*(int32_t*)0x0052108c)
static uint32_t* const k_cam_frames[3] = {(uint32_t*)0x00520c70, (uint32_t*)0x00520bb8, (uint32_t*)0x005210a0};
#define g_cam_p3        ((uint32_t*)0x005214e8)
// physics.obj
#define g_speed         (*(volatile int32_t*)0x004ecf74)   // enum PhysicsSpeed volatile physics_speed
#define g_phys_single   (*(int32_t*)0x004ecf78)            // SingleBegin("Physics")
#define g_shm_r         (*(uint32_t*)0x004ecf7c)           // the state packet, the main thread's side
#define g_metric        (*(int32_t*)0x004ecf80)            // TelemetryCreateMetric("elapsed_time")
#define g_temperature   (*(uint32_t*)0x004ecf84)           // a float (292.0), moved as bits
#define g_state         (*(volatile int32_t*)0x004ecf88)   // the task's state (above)
#define g_controls_time (*(float*)0x004ecf8c)              // PhysicsReadControls: when it last ran (main thread)
#define g_time_string   ((char*)0x005215d8)                // PhysicsTimeString's buffer (main thread)
#define g_stream        (*(void**)0x005215fc)              // MemStream: the creation records
#define g_camera_type   (*(volatile int32_t*)0x00521600)
#define g_paused        (*(volatile uint8_t*)0x00521608)   // physics_paused (the menu's; an input)
#define g_camera_focus  (*(volatile int32_t*)0x00521614)
#define g_tick          (*(volatile int32_t*)0x0052161c)   // physics_tick
#define g_msg_tail      (*(int32_t*)0x0052162c)            // message_list_tail
#define g_damage        (*(uint8_t*)0x00521634)
#define g_realism       (*(int32_t*)0x0052163c)
#define g_stream_ptr    (*(void**)0x00521644)              // MemStreamPtr over g_stream
#define g_please_stop   (*(volatile uint8_t*)0x0052165c)   // PhysicsAbortRace
#define g_watchdog      ((TimerWatchdog*)0x00521668)
#define g_locale        (*(const char**)0x00509354)        // LocaleInfo const&: its first byte is the decimal point

// ---- constants (their bits) --------------------------------------------------------------------------------------
static const uint32_t k_milli = 0x3a83126f;                 // 0.001f
static const uint32_t k_thousand = 0x447a0000;              // 1000.0f
static const uint32_t k_half = 0x3f000000;                  // 0.5f
static const uint32_t k_dt = 0x3c83126f;                    // 0.016f
static const uint32_t k_one_over_dt = 0x4279ffff;           // 62.499996f (not 62.5)
static const uint32_t k_ten = 0x41200000;                   // 10.0f
static const uint32_t k_world_half = 0x4a742400;            // 4000000.0f
static const uint32_t k_one = 0x3f800000;                   // 1.0f
static const uint32_t k_hundred = 0x42c80000;               // 100.0f
static const uint32_t k_sixteen = 0x41800000;               // 16.0f
static const uint32_t k_quarter = 0x3e800000;               // 0.25f
static const uint32_t k_hundredth = 0x3c23d70a;             // 0.01f
static const uint32_t k_steer_speed = 0x4231c71c;           // 44.444443f (100 mph)
static const uint32_t k_steer_slope = 0xbc8dc8dd;           // -0.017307693f
static const uint32_t k_steer_base = 0x3fa27627;            // 1.2692307f
static const uint32_t k_clutch_slope = 0xbf99999a;          // -1.2f
static const uint32_t k_clutch_base = 0x3f8ccccd;           // 1.1f
static const uint32_t k_zero = 0x00000000;                  // 0.0f

// ---- the functions they call, by address -------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
typedef uint8_t(__cdecl* Flag_t)();
typedef int(__cdecl* Int_t)();
typedef void(__cdecl* Single3_t)(int, const char*, int);
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
static const AssertMsg_t ASSERT_MSG = (AssertMsg_t)0x00426d70;     // a bare ret in this build
typedef int(__cdecl* SingleBegin_t)(const char*);
static const SingleBegin_t SingleBegin = (SingleBegin_t)0x00414f30;
static const Single3_t SingleEnter = (Single3_t)0x00415000;         // _SingleEnter(handle, file, line)
static const Single3_t SingleLeave = (Single3_t)0x00415070;
static const Single3_t SingleEnd = (Single3_t)0x00415090;
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* Delete_t)(void*);
static const Delete_t operator_delete = (Delete_t)0x00414390;
typedef void(__cdecl* TaskSleep_t)(int);
static const TaskSleep_t TaskSleep = (TaskSleep_t)0x00414de0;
static const Int_t PTimeNow = (Int_t)0x00413b40;
// begin / end of the physics' parts
static const Void_t PhysReplayBGBegin = (Void_t)0x0042d350, PhysReplayBGEnd = (Void_t)0x0042d430;
static const Void_t PhysReplayDoneCreating = (Void_t)0x0042d3a0, PhysReplaySealRecording = (Void_t)0x0042d470;
static const Void_t CollideBegin = (Void_t)0x0043c600, CollideEnd = (Void_t)0x0043caf0;
static const Void_t TireBegin = (Void_t)0x0043bd20, TireEnd = (Void_t)0x0043bd30;
static const Void_t AeroBegin = (Void_t)0x0043b1e0, AeroEnd = (Void_t)0x0043b1f0;
static const Void_t CarBegin = (Void_t)0x0043b090, CarEnd = (Void_t)0x0043b0b0;
static const Void_t CarDeityEnd = (Void_t)0x0042c5f0;
static const Void_t load_tv_cameras = (Void_t)0x00428db0;
static const Void_t RecordReset = (Void_t)0x0042a030;
static const Void_t AIResetCars = (Void_t)0x0041d1a0;
static const Void_t AIBGUpdate = (Void_t)0x0041d1c0;
static const Void_t MultiBGRecv = (Void_t)0x004a2440, MultiBGSend = (Void_t)0x004a2620;
static const Flag_t MultiEnabled = (Flag_t)0x004a23c0;
static const Flag_t PhysReplayPlayMode = (Flag_t)0x0042d340;
static const Flag_t GhostCarIncognito = (Flag_t)0x0040e940;
static const Int_t CarMgrCount = (Int_t)0x00464480;
typedef const void*(__cdecl* WorldGameOptions_t)();
static const WorldGameOptions_t WorldGameOptions = (WorldGameOptions_t)0x004627a0;
typedef void(__cdecl* CarDeityBegin_t)(const void*);
static const CarDeityBegin_t CarDeityBegin = (CarDeityBegin_t)0x0042c4f0;
typedef StaticObjectList*(__cdecl* StaticListGet_t)(const char*);
static const StaticListGet_t StaticObjectListGet = (StaticListGet_t)0x00435f00;
typedef void(__cdecl* StaticListForget_t)(StaticObjectList*);
static const StaticListForget_t StaticObjectListForget = (StaticListForget_t)0x00436040;
typedef void(__cdecl* PhobsN_t)(PhobRoot**, int);
static const PhobsN_t PhysReplayRecordUpdate = (PhobsN_t)0x0042d570;
static const PhobsN_t PhysReplayPlayUpdate = (PhobsN_t)0x0042cf00;
typedef void(__cdecl* RegisterPhob_t)(PhobRoot*);
static const RegisterPhob_t PhysReplayRegisterPhob = (RegisterPhob_t)0x0042d410;
typedef void(__fastcall* UpdateDraft_t)(void*, Edx, void*);
static const UpdateDraft_t Car_UpdateDraft = (UpdateDraft_t)0x00438830;
// memory streams and shared memory
typedef void*(__cdecl* MemStreamCreate_t)(int);
static const MemStreamCreate_t MemStreamCreate = (MemStreamCreate_t)0x004d8b80;
typedef void*(__cdecl* MemStreamCreatePtr_t)(void*);
static const MemStreamCreatePtr_t MemStreamCreatePtr = (MemStreamCreatePtr_t)0x004d8be0;
typedef void(__cdecl* MemStreamOp_t)(void*);
static const MemStreamOp_t MemStreamSeekStart = (MemStreamOp_t)0x004d8c60;
static const MemStreamOp_t MemStreamDestroyPtr = (MemStreamOp_t)0x004d8c00;
static const MemStreamOp_t MemStreamDestroy = (MemStreamOp_t)0x004d8bb0;
typedef void(__cdecl* MemStreamGetInt_t)(void*, int32_t*);
static const MemStreamGetInt_t MemStreamGetInt = (MemStreamGetInt_t)0x004d8dc0;
typedef int(__cdecl* MemStreamGetPos_t)(void*);
static const MemStreamGetPos_t MemStreamGetPos = (MemStreamGetPos_t)0x004d8c90;
typedef void(__cdecl* MemStreamSeekPos_t)(void*, int);
static const MemStreamSeekPos_t MemStreamSeekPos = (MemStreamSeekPos_t)0x004d8c80;
typedef void(__cdecl* MemStreamGetData_t)(void*, void*, int);
static const MemStreamGetData_t MemStreamGetData = (MemStreamGetData_t)0x004d8e50;
typedef void(__cdecl* MemStreamPutInt_t)(void*, int);
static const MemStreamPutInt_t MemStreamPutInt = (MemStreamPutInt_t)0x004d8ce0;
typedef void(__cdecl* MemStreamPutData_t)(void*, const void*, int);
static const MemStreamPutData_t MemStreamPutData = (MemStreamPutData_t)0x004d8d80;
typedef uint32_t(__cdecl* ShmemAlloc_t)(const char*, int);
static const ShmemAlloc_t ShmemAlloc = (ShmemAlloc_t)0x004d8960;
typedef uint32_t(__cdecl* ShmemGet_t)(const char*);
static const ShmemGet_t ShmemGet = (ShmemGet_t)0x004d89b0;
typedef void(__cdecl* ShmemOp_t)(uint32_t);
static const ShmemOp_t ShmemFree = (ShmemOp_t)0x004d8aa0;
static const ShmemOp_t ShmemReleaseWritable = (ShmemOp_t)0x004d8a90;
static const ShmemOp_t ShmemReleaseReadable = (ShmemOp_t)0x004d8a80;
typedef void*(__cdecl* ShmemGetMem_t)(uint32_t);
static const ShmemGetMem_t ShmemGetWritable = (ShmemGetMem_t)0x004d8a70;
static const ShmemGetMem_t ShmemGetReadable = (ShmemGetMem_t)0x004d8a30;
// the profiler, through the game's own function pointers (read at each call)
typedef int(__cdecl* ProfStart_t)(const char*);
typedef void(__cdecl* ProfStop_t)(int);
#define i_pr_overhead_begin (*(Void_t*)0x004e642c)
#define i_pr_overhead_end   (*(Void_t*)0x004e6430)
#define i_prof_start        (*(ProfStart_t*)0x004e6434)
#define i_prof_stop         (*(ProfStop_t*)0x004e6438)
// physics.obj's own callees
static const Void_t PhysicsDashboardBegin = (Void_t)0x00429850, PhysicsDashboardEnd = (Void_t)0x00429860;
static const Flag_t DriverBegin = (Flag_t)0x00441490;
static const Void_t DriverEnd = (Void_t)0x00441550;
typedef void(__cdecl* DriverUpdate_t)(float);
static const DriverUpdate_t DriverUpdate = (DriverUpdate_t)0x004417c0;
typedef int(__cdecl* TelemetryCreate_t)(const char*);
static const TelemetryCreate_t TelemetryCreateMetric = (TelemetryCreate_t)0x00471a00;
typedef void(__cdecl* TelemetryUpdate_t)(int, uint32_t);          // (metric, float as bits)
static const TelemetryUpdate_t TelemetryUpdateMetric = (TelemetryUpdate_t)0x00471b00;
typedef void(__cdecl* OptionsSet_t)(const char*, const char*, int);
static const OptionsSet_t OptionsSet = (OptionsSet_t)0x00471430;
typedef void(__cdecl* BGHook_t)(uint32_t, int);
static const BGHook_t BGHook = (BGHook_t)0x004189f0;
typedef void(__cdecl* BGUnhook_t)(uint32_t);
static const BGUnhook_t BGUnhook = (BGUnhook_t)0x00418ac0;
typedef void(__cdecl* ExceptSinglePrecision_t)(uint8_t);
static const ExceptSinglePrecision_t ExceptSinglePrecision = (ExceptSinglePrecision_t)0x00415ca0;
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t sprintf_o = (Sprintf_t)0x004cf0a0;           // the game's CRT sprintf
typedef int(__cdecl* FileCreate_t)(const char*);
static const FileCreate_t FileCreate = (FileCreate_t)0x004115f0;
typedef void(__cdecl* FilePrintf_t)(int, char*, const char*, ...);
static const FilePrintf_t FilePrintf = (FilePrintf_t)0x00411b40;
typedef void(__cdecl* FileClose_t)(int*);
static const FileClose_t FileClose = (FileClose_t)0x00411850;
// this group's own functions, by address (a hooked rewrite -- or the recorder -- is what runs)
typedef double(__cdecl* PhysicsGetTime_t)();                        // physics_tick * 0.016f, in ST0
static const PhysicsGetTime_t PhysicsGetTime_o = (PhysicsGetTime_t)0x0042bc80;
static const Int_t PhysicsGetSpeed_o = (Int_t)0x0042bd40;
static const Void_t PhysicsPause_o = (Void_t)0x0042bcc0;
typedef void(__cdecl* PhysTaskBegin_t)(void*);
static const PhysTaskBegin_t PhysTaskBegin_o = (PhysTaskBegin_t)0x00426850;
static const Void_t PhysTaskPostRace_o = (Void_t)0x00426a60, PhysTaskEnd_o = (Void_t)0x00426a80;
static const Void_t PhysTaskRestart_o = (Void_t)0x00426b20, PhysTaskUpdate_o = (Void_t)0x00426b90;
static const Void_t renormalize_phobs_o = (Void_t)0x00426f00, collide_phobs_o = (Void_t)0x00427120;
static const Void_t update_phobs_o = (Void_t)0x004274d0;
typedef void(__cdecl* CollideNode_t)(int, int, int, int, int);
static const CollideNode_t collide_object_node_o = (CollideNode_t)0x004273b0;
typedef int(__cdecl* UpdateCamera_t)(Frame*);
static const UpdateCamera_t update_camera = (UpdateCamera_t)0x00427610;
typedef uint8_t(__cdecl* CreatePhob_t)(void*, PhobRoot**);
static const CreatePhob_t create_phob_o = (CreatePhob_t)0x004289c0;
typedef PhobRoot*(__cdecl* MakePhob_t)(PhobData*, int);
static const MakePhob_t make_phob_o = (MakePhob_t)0x00428a70;
typedef int(__fastcall* GetTicks_t)(TimerConditioner*, Edx);
static const GetTicks_t TimerConditioner_GetTicks_o = (GetTicks_t)0x00428c30;
typedef double(__fastcall* Deviation_t)(TimerConditioner*, Edx);    // ST0
static const Deviation_t TimerConditioner_PhysicsDeviation_o = (Deviation_t)0x00428d50;
typedef int(__cdecl* GetMsgSize_t)(uint32_t);
static const GetMsgSize_t PhysTaskGetMsgSize_o = (GetMsgSize_t)0x00426df0;
typedef void*(__cdecl* FindCar_t)(int);
static const FindCar_t PhysTaskFindCar_o = (FindCar_t)0x00426d40;
typedef PhysicsEventStream*(__fastcall* EventsAssign_t)(PhysicsEventStream*, Edx, const PhysicsEventStream*);
static const EventsAssign_t PhysicsEventStream_assign_o = (EventsAssign_t)0x0042c020;
typedef void(__fastcall* Watchdog_t)(TimerWatchdog*, Edx);
static const Watchdog_t TimerWatchdog_dump_o = (Watchdog_t)0x0042c140;
static const Watchdog_t TimerWatchdog_init_o = (Watchdog_t)0x0042c1e0;
static const Watchdog_t TimerWatchdog_tick_end_o = (Watchdog_t)0x0042c200;
static const Watchdog_t TimerWatchdog_tick_begin_o = (Watchdog_t)0x0042c220;
static const uint32_t k_physics_thread = 0x0042c050;               // what BGHook / BGUnhook are given
// constructors, as make_phob calls them: (this, data, message offset)
typedef PhobRoot*(__fastcall* PhobCtor_t)(void*, Edx, PhobData*, void*);

// strings, at their v1.0 addresses
static const char* const k_str_phystask = (const char*)0x004eca78;     // "PhysTask"
static const char* const k_str_track_sol = (const char*)0x004eca90;    // "track.sol"
static const char* const k_str_shmem = (const char*)0x004eca9c;        // "shm_PhysicsShmem"
static const char* const k_str_prof_task = (const char*)0x004ecab0;    // "PhysTaskUpdate"
static const char* const k_str_findcar = (const char*)0x004ecac0;      // "Must call FindCar between PhysTaskBegin/End"
static const char* const k_fmt_msgsize = (const char*)0x004ecaec;      // "Can't find message size for type %08x"
static const char* const k_str_prof_update = (const char*)0x004ecb30;  // "physics_update"
static const char* const k_str_prof_common = (const char*)0x004ecb40;  // "physics_common"
static const char* const k_fmt_phob_type = (const char*)0x004ecc38;    // "Unknown phob type: %x"
static const char* const k_str_physics = (const char*)0x004ecfc8;      // "Physics"
static const char* const k_str_elapsed = (const char*)0x004ecfd0;      // "elapsed_time"
static const char* const k_str_camera = (const char*)0x004ecfe0;       // "camera"
static const char* const* const k_options_section = (const char* const*)0x004db890;   // -> "PHYSICS"
static const char* const k_str_reboot = (const char*)0x004ecfe8;       // "A necessary system resource is unavailable..."
static const char* const k_str_shmem2 = (const char*)0x004ed024;       // "shm_PhysicsShmem" (physics.obj's copy)
static const char* const k_fmt_overflow2 = (const char*)0x004ed038;    // "99:99%c99"
static const char* const k_fmt_overflow1 = (const char*)0x004ed044;    // "99:99%c9"
static const char* const k_fmt_time2 = (const char*)0x004ed050;        // "%01d:%02d%c%02d"
static const char* const k_fmt_time1 = (const char*)0x004ed060;        // "%01d:%02d%c%01d"
static const char* const k_fmt_state = (const char*)0x004ed070;        // "Unknown physics state: %d"
static const char* const k_str_timer_log = (const char*)0x004ed08c;    // "c:\timer.log"
static const char* const k_fmt_timer_bin = (const char*)0x004ed09c;    // "[%+2.2d]: %d\r\n"
static const char* const k_fmt_timer_avg = (const char*)0x004ed0ac;    // "[???]: %d [avg: %d]\r\n"
static const char* const k_str_timer_fail = (const char*)0x004ed0c4;   // "Unable to create timer log"
static const char* const k_str_dolt = (const char*)0x004ed0e0;         // "dolt."

// ---- footprint helpers -------------------------------------------------------------------------------------------
static void fp_none(Footprint&) {}
static void fp_replay_tick(Footprint& f) { f.replay_only = "it runs a whole physics tick (every object, the deity, the AI, the packet)"; }
static void fp_replay_clock(Footprint& f) { f.replay_only = "it reads the real-time clock (PTimeNow): two passes see two times"; }

// =================================================================================================================
// phystask.obj
// =================================================================================================================

// ==== PhysTaskBegin (0x426850) ===================================================================================
// The task's start (timer thread, state 1): the physics parts' Begins, the deity for the race type, the TV
// cameras; the phob list (0x800 bytes: 512 slots, unchecked), the track's static objects, the cars[] table,
// the camera's frames reset to identity; then every creation record in the stream becomes a phob
// (create_phob, until it meets the -1 PhysicsStart wrote), the shared packet is sized for their messages, the
// deity initialised, and the timer conditioner set to resync on its first tick.
static void __cdecl PhysTaskBegin_rw(void* stream) {
    g_task_single = SingleBegin(k_str_phystask);
    PhysReplayBGBegin();
    CollideBegin();
    TireBegin();
    AeroBegin();
    CarBegin();
    CarDeityBegin(WorldGameOptions());
    load_tv_cameras();
    g_cam_52108c = 0;
    g_phobs = (PhobRoot**)MemAlloc((int)m1_operand(0x00426894));  // push 0x800 (512 slots); M1 lifts it
    g_static_list = StaticObjectListGet(k_str_track_sol);
    for (int i = 0; i < 16; i++) g_cars[i] = 0;
    g_tick_overflow = 0;
    g_nphobs = 0;
    g_events->len = 0;
    for (int k = 0; k < 3; k++) {                        // rotation identity, position 0
        uint32_t* fr = k_cam_frames[k];
        for (int w = 0; w < 12; w++) fr[w] = (w == 0 || w == 4 || w == 8) ? k_one : 0;
    }
    g_cam_p3[0] = g_cam_p3[1] = g_cam_p3[2] = 0;
    void* ptr = MemStreamCreatePtr(stream);
    MemStreamSeekStart(ptr);
    g_nphobs = 0;
    if (create_phob_o(ptr, g_phobs)) {
        do {
            g_nphobs++;
        } while (create_phob_o(ptr, g_phobs + g_nphobs));
    }
    MemStreamDestroyPtr(ptr);
    PhysReplayDoneCreating();
    g_shm_w = ShmemAlloc(k_str_shmem, g_msg_tail + 0x7dc);
    void* d = g_deity;
    VFN(d, 0xc, void)(d, 0);                             // Deity::Init
    g_timer->resync = 1;
    g_timer->half_toggle = 0;
}
static void fp_task_begin(Footprint& f, void*) { f.replay_only = "it allocates the phob list and creates every phob"; }
PORT_FN(0x00426850, "PhysTaskBegin", PhysTaskBegin_rw, fp_task_begin)

// ==== PhysTaskPostRace (0x426a60) ================================================================================
static void __cdecl PhysTaskPostRace_rw() {
    void* d = g_deity;
    VFN(d, 0x18, void)(d, 0);                            // Deity::PostRace
    PhysReplaySealRecording();
}
static void fp_task_postrace(Footprint& f) { f.replay_only = "the deity's PostRace runs the records; the replay recording is sealed"; }
PORT_FN(0x00426a60, "PhysTaskPostRace", PhysTaskPostRace_rw, fp_task_postrace)

// ==== PhysTaskEnd (0x426a80) =====================================================================================
static void __cdecl PhysTaskEnd_rw() {
    ShmemFree(g_shm_w);
    for (int i = 0; i < g_nphobs; i++) {
        PhobRoot* p = g_phobs[i];
        if (p) VFN(p, 0, void*, unsigned)(p, 0, 1);      // scalar deleting destructor
    }
    operator_delete(g_phobs);
    g_phobs = 0;
    StaticObjectListForget(g_static_list);
    g_static_list = 0;
    CarDeityEnd();
    TireEnd();
    AeroEnd();
    CarEnd();
    CollideEnd();
    PhysReplayBGEnd();
    SingleEnd(g_task_single, 0, 0);
}
static void fp_task_end(Footprint& f) { f.replay_only = "it deletes every phob and frees the list"; }
PORT_FN(0x00426a80, "PhysTaskEnd", PhysTaskEnd_rw, fp_task_end)

// ==== PhysTaskRestart (0x426b20) =================================================================================
// Every phob's Reset (no null check, unlike End and update_phobs), the deity's, the records, the AI.
static void __cdecl PhysTaskRestart_rw() {
    SingleEnter(g_task_single, 0, 0);
    for (int i = 0; i < g_nphobs; i++) {
        PhobRoot* p = g_phobs[i];
        VFN(p, 4, void)(p, 0);
    }
    void* d = g_deity;
    VFN(d, 0x14, void)(d, 0);                            // Deity::Reset
    RecordReset();
    AIResetCars();
    SingleLeave(g_task_single, 0, 0);
}
static void fp_task_restart(Footprint& f) { f.replay_only = "it resets every object, the deity, the records and the AI"; }
PORT_FN(0x00426b20, "PhysTaskRestart", PhysTaskRestart_rw, fp_task_restart)

// ==== PhysTaskUpdate (0x426b90) ==================================================================================
// One timer tick: GetTicks says how many 16 ms physics ticks are due (0-4); the network's input, the deity;
// unless the menu has paused it (a replay playing back runs regardless), the AI, one frame renormalised, and
// per tick collide (not in playback) and update, counting physics_tick unless paused. Then the network's
// output and the shared packet for the main thread: the camera (+4 frame, +0x38 its result, +0x34 the camera
// type, +0x40 the overflow flag), every phob's message (from +0x7dc), the deity's, and the event queue (+0x3d8),
// which is emptied.
static void __cdecl PhysTaskUpdate_rw() {
    SingleEnter(g_task_single, 0, 0);
    int n = TimerConditioner_GetTicks_o(g_timer, 0);
    i_pr_overhead_begin();
    int prof = i_prof_start(k_str_prof_task);
    i_pr_overhead_end();
    MultiBGRecv();
    void* d = g_deity;
    VFN(d, 0x10, void)(d, 0);                            // Deity::Update
    uint8_t play = PhysReplayPlayMode();
    if (play || !g_paused) {
        if (!play) AIBGUpdate();
        renormalize_phobs_o();
        for (; n > 0; n--) {
            if (!play) collide_phobs_o();
            update_phobs_o();
            if (!g_paused) g_tick++;
        }
    }
    MultiBGSend();
    uint8_t* p = (uint8_t*)ShmemGetWritable(g_shm_w);
    uint8_t* msgs = p + 0x7dc;
    int cam = update_camera((Frame*)(p + 4));
    *(int32_t*)(p + 0x38) = cam;
    *(int32_t*)(p + 0x34) = g_camera_type;
    p[0x40] = g_tick_overflow;
    for (int i = 0; i < g_nphobs; i++) {
        PhobRoot* o = g_phobs[i];
        VFN(o, 0x14, void, uint8_t*)(o, 0, msgs);        // GetMessage
    }
    d = g_deity;
    VFN(d, 0x28, void, void*)(d, 0, p);                  // Deity::GetMessage
    PhysicsEventStream_assign_o((PhysicsEventStream*)(p + 0x3d8), 0, g_events);
    g_events->len = 0;
    ShmemReleaseWritable(g_shm_w);
    i_pr_overhead_begin();
    i_prof_stop(prof);
    i_pr_overhead_end();
    SingleLeave(g_task_single, 0, 0);
}
PORT_FN(0x00426b90, "PhysTaskUpdate", PhysTaskUpdate_rw, fp_replay_tick)

// ==== PhysTaskRegisterCar (0x426d00) =============================================================================
static void __cdecl PhysTaskRegisterCar_rw(void* car, int i) {
    SingleEnter(g_task_single, 0, 0);
    g_cars[i] = car;
    SingleLeave(g_task_single, 0, 0);
}
static void fp_register_car(Footprint& f, void*, int i) { f.add(&g_cars[i], 4, "cars[i]"); }
PORT_FN(0x00426d00, "PhysTaskRegisterCar", PhysTaskRegisterCar_rw, fp_register_car)

// ==== PhysTaskFindCar (0x426d40) =================================================================================
static void* __cdecl PhysTaskFindCar_rw(int i) {
    ASSERT_MSG(g_phobs != 0, k_str_findcar);
    return g_cars[i];
}
static void fp_find_car(Footprint&, int) {}
PORT_FN(0x00426d40, "PhysTaskFindCar", PhysTaskFindCar_rw, fp_find_car)

// ==== PhysTaskFindStaticObject (0x426d80) ========================================================================
static StaticObject* __cdecl PhysTaskFindStaticObject_rw(int id) {
    SingleEnter(g_task_single, 0, 0);
    StaticObjectList* l = g_static_list;
    StaticObject* found = 0;
    int n = l->count;
    StaticObject* o = l->objects;
    for (int i = 0; i < n; i++)
        if (o[i].id == id) { found = &o[i]; break; }
    SingleLeave(g_task_single, 0, 0);
    return found;
}
static void fp_find_static(Footprint&, int) {}
PORT_FN(0x00426d80, "PhysTaskFindStaticObject", PhysTaskFindStaticObject_rw, fp_find_static)

// ==== PhysTaskGetMsgSize (0x426df0) ==============================================================================
// The message bytes each phob type needs in the shared packet, from a {FourCC, size} table ending at 0.
static int __cdecl PhysTaskGetMsgSize_rw(uint32_t type) {
    if (k_msg_sizes[0].type != 0) {
        int i = 0;
        do {
            if (k_msg_sizes[i].type == type) return k_msg_sizes[i].size;
            i++;
        } while (k_msg_sizes[i].type != 0);
    }
    LogPanic(k_fmt_msgsize, type);
    return -1;
}
static void fp_msg_size(Footprint& f, uint32_t type) {
    for (int i = 0; k_msg_sizes[i].type != 0; i++)
        if (k_msg_sizes[i].type == type) return;
    f.replay_only = "an unknown type panics";
}
PORT_FN(0x00426df0, "PhysTaskGetMsgSize", PhysTaskGetMsgSize_rw, fp_msg_size)

// ==== PhysTaskEvent (0x426e40) ===================================================================================
// Queue an event for the main thread: a type byte and the data, if it fits (length + n + 1 < 0x400, as an
// unsigned compare); else it's dropped.
static void __cdecl PhysTaskEvent_rw(uint8_t type, const void* data, int n) {
    if ((uint32_t)(g_events->len + (uint32_t)n + 1) >= 0x400) return;
    uint32_t at = g_events->len;
    g_events->data[at] = type;
    uint8_t* dst = &g_events->data[g_events->len + 1];
    const uint8_t* src = (const uint8_t*)data;
    uint32_t words = (uint32_t)n >> 2, bytes = (uint32_t)n & 3;   // rep movsd, rep movsb: forwards
    for (uint32_t k = 0; k < words; k++, dst += 4, src += 4) memcpy(dst, src, 4);
    for (uint32_t k = 0; k < bytes; k++) *dst++ = *src++;
    g_events->len += (uint32_t)n + 1;
}
static void fp_task_event(Footprint& f, uint8_t, const void*, int) { f.add(g_events, sizeof(PhysicsEventStream), "the event queue"); }
PORT_FN(0x00426e40, "PhysTaskEvent", PhysTaskEvent_rw, fp_task_event)

// ==== PhysicsGetDeviation (0x426e90), ConvertPTimeToPhysicsTime (0x426ea0), ConvertPhysicsTimeToPTime (0x426ed0) ==
static double __cdecl PhysicsGetDeviation_rw() { return TimerConditioner_PhysicsDeviation_o(g_timer, 0); }
PORT_FN(0x00426e90, "PhysicsGetDeviation", PhysicsGetDeviation_rw, fp_replay_clock)

static double __cdecl ConvertPTimeToPhysicsTime_rw(int ptime) {
    int32_t d = (int32_t)((uint32_t)ptime - (uint32_t)g_timer->ptime);
    return D(d) * D(Fb(k_milli)) + D(g_timer->time);     // fild; fmul; fadd -- ST0
}
static void fp_convert_p2t(Footprint&, int) {}
PORT_FN(0x00426ea0, "ConvertPTimeToPhysicsTime", ConvertPTimeToPhysicsTime_rw, fp_convert_p2t)

static int __cdecl ConvertPhysicsTimeToPTime_rw(float t) {
    int32_t n = x87_ftol((D(t) - D(g_timer->time)) * D(Fb(k_thousand)) + D(Fb(k_half)));
    return (int32_t)((uint32_t)n + (uint32_t)g_timer->ptime);
}
static void fp_convert_t2p(Footprint&, float) {}
PORT_FN(0x00426ed0, "ConvertPhysicsTimeToPTime", ConvertPhysicsTimeToPTime_rw, fp_convert_t2p)

// ==== renormalize_phobs (0x426f00) ===============================================================================
// Each timer tick, one dynamic object's rotation (round robin) is made orthonormal again: row 1 = row 2 x row 0,
// rows 0 and 1 normalised, row 2 = row 0 x row 1. (The two compiler alias checks on its own locals are dead.)
static void __cdecl renormalize_phobs_rw() {
    if (!(g_nphobs > g_renorm_index)) g_renorm_index = 0;
    PhobRoot* p = g_phobs[g_renorm_index];
    if (p && VFN(p, 0x2c, uint8_t)(p, 0)) {              // IsDynamic
        float* m = p->frame.rot.m;
        const float a0 = m[0], a1 = m[1], a2 = m[2];     // row 0 (the original's copies: integer moves)
        const float c0 = m[6], c1 = m[7], c2 = m[8];     // row 2
        float b0 = (float)(D(a2) * D(c1) - D(c2) * D(a1));
        float b1 = (float)(D(a0) * D(c2) - D(a2) * D(c0));
        float b2 = (float)(D(c0) * D(a1) - D(a0) * D(c1));
        double inv = D(Fb(k_one)) / x87_sqrt((D(a2) * D(a2) + D(a1) * D(a1)) + D(a0) * D(a0));
        const float n0 = (float)(D(a0) * inv), n1 = (float)(D(a1) * inv), n2 = (float)(inv * D(a2));
        double invb = D(Fb(k_one)) / x87_sqrt((D(b1) * D(b1) + D(b2) * D(b2)) + D(b0) * D(b0));
        b0 = (float)(D(b0) * invb);
        b1 = (float)(D(b1) * invb);
        b2 = (float)(invb * D(b2));
        const float r0 = (float)(D(b2) * D(n1) - D(b1) * D(n2));
        const float r1 = (float)(D(b0) * D(n2) - D(b2) * D(n0));
        const float r2 = (float)(D(b1) * D(n0) - D(b0) * D(n1));
        m[0] = n0; m[1] = n1; m[2] = n2;
        m[3] = b0; m[4] = b1; m[5] = b2;
        m[6] = r0; m[7] = r1; m[8] = r2;
    }
    g_renorm_index++;
}
static void fp_renormalize(Footprint& f) {
    int i = g_renorm_index;
    if (!(g_nphobs > i)) i = 0;
    if (g_phobs && g_phobs[i]) f.add(&g_phobs[i]->frame.rot, sizeof(M3), "the object's rotation");
}
PORT_FN(0x00426f00, "renormalize_phobs", renormalize_phobs_rw, fp_renormalize)

// ==== collide_object (0x427350, 0x4273b0) ========================================================================
// An object against the track's static objects: down the static quadtree from the root (the world is
// +-4,000,000 x 10 in integer units of 0.1 m), at each node colliding each of the object's volumes (Collide,
// +8) with every static object listed there, then into the child quadrant that holds the object's x, z.
static void __cdecl collide_object_node_rw(int node, int x0, int z0, int x1, int z1) {
    for (;;) {
        const QuadNode* nd = &g_static_list->nodes[node];
        for (uint16_t k = nd->first; nd->end > k; k++) {
            uint16_t idx = g_static_list->index[k];
            CollisionVolume* sv = g_static_list->objects[idx].volume;
            PhobRoot* ph = g_collide_phob;
            for (int v = 0; v < ph->num_volumes; v++) {
                CollisionVolume* vol = ph->volumes[v];
                VFN(vol, 8, void, CollisionVolume*)(vol, 0, sv);
            }
        }
        int child = nd->child;
        if (child == 0) return;
        int mx = (int32_t)((uint32_t)x0 + (uint32_t)x1) / 2;
        int mz = (int32_t)((uint32_t)z0 + (uint32_t)z1) / 2;
        if (g_collide_z > mz) {
            if (g_collide_x > mx) { node = child + 3; z0 = mz; x0 = mx; }
            else { node = child + 2; x1 = mx; z0 = mz; }
        } else {
            z1 = mz;
            if (g_collide_x > mx) { node = child + 1; x0 = mx; }
            else { node = child; x1 = mx; }
        }
    }
}
// Footprint: the object, its volumes and every static volume the walk visits (with their owners); a sphere
// group collides member spheres that aren't enumerated here, so it's left to the replay.
static void fp_collide_walk(Footprint& f, PhobRoot* ph, int node, int x0, int z0, int x1, int z1, int px, int pz) {
    if (!ph || !g_static_list) return;
    f.object(ph, "object");
    for (int v = 0; v < ph->num_volumes && v < 4; v++) {
        CollisionVolume* vol = ph->volumes[v];
        if (!vol) continue;
        if (vol->type_tag == 0x47525550u) { f.replay_only = "a sphere group collides member spheres not enumerated here"; return; }
        f.object(vol, "volume");
    }
    for (int depth = 0; depth < 64; depth++) {
        const QuadNode* nd = &g_static_list->nodes[node];
        for (uint16_t k = nd->first; nd->end > k; k++) {
            CollisionVolume* sv = g_static_list->objects[g_static_list->index[k]].volume;
            if (!sv) continue;
            f.object(sv, "static volume");
            if (sv->owner) f.object(sv->owner, "static volume's owner");
        }
        int child = nd->child;
        if (child == 0) return;
        int mx = (int32_t)((uint32_t)x0 + (uint32_t)x1) / 2;
        int mz = (int32_t)((uint32_t)z0 + (uint32_t)z1) / 2;
        if (pz > mz) {
            if (px > mx) { node = child + 3; z0 = mz; x0 = mx; }
            else { node = child + 2; x1 = mx; z0 = mz; }
        } else {
            z1 = mz;
            if (px > mx) { node = child + 1; x0 = mx; }
            else { node = child; x1 = mx; }
        }
    }
}
static void fp_collide_node(Footprint& f, int node, int x0, int z0, int x1, int z1) {
    fp_collide_walk(f, g_collide_phob, node, x0, z0, x1, z1, g_collide_x, g_collide_z);
}
PORT_FN(0x004273b0, "collide_object(node)", collide_object_node_rw, fp_collide_node)

static int32_t world_half_x10() {
    volatile uint32_t a = k_world_half, b = k_ten;       // computed at run time, as the original does
    return x87_ftol(D(Fb(a)) * D(Fb(b)));
}
static void __cdecl collide_object_rw(PhobRoot* ph) {
    g_collide_phob = ph;
    g_collide_x = x87_ftol(D(ph->frame.pos.x) * D(Fb(k_ten)));
    g_collide_z = x87_ftol(D(ph->frame.pos.z) * D(Fb(k_ten)));
    int32_t v = world_half_x10();
    collide_object_node_o(0, -v, -v, v, v);
}
static void fp_collide_object(Footprint& f, PhobRoot* ph) {
    if (!ph) return;
    int32_t v = world_half_x10();
    fp_collide_walk(f, ph, 0, -v, -v, v, v, x87_ftol(D(ph->frame.pos.x) * D(Fb(k_ten))),
                    x87_ftol(D(ph->frame.pos.z) * D(Fb(k_ten))));
}
PORT_FN(0x00427350, "collide_object(phob)", collide_object_rw, fp_collide_object)

// ==== update_phobs (0x4274d0) ====================================================================================
// One physics tick of every object: live, each dynamic object's Update (+8), the recorder's frame, every pair
// of registered cars' drafting; in playback, the recording's frame instead; then everyone's UpdateCommon (+0xc).
static void __cdecl update_phobs_rw() {
    i_pr_overhead_begin();
    int prof = i_prof_start(k_str_prof_update);
    i_pr_overhead_end();
    if (!PhysReplayPlayMode()) {
        for (int i = 0; i < g_nphobs; i++) {
            PhobRoot* p = g_phobs[i];
            if (!p) continue;
            void** vt = p->vtable;                       // read once, for both calls
            if (((uint8_t(__fastcall*)(void*, Edx))vt[0x2c / 4])(p, 0))
                ((void(__fastcall*)(void*, Edx))vt[8 / 4])(p, 0);
        }
        PhysReplayRecordUpdate(g_phobs, g_nphobs);
        for (int i = 0; i < 16; i++) {
            if (!g_cars[i]) continue;
            for (int j = 0; j < 16; j++)
                if (i != j && g_cars[j]) Car_UpdateDraft(g_cars[i], 0, g_cars[j]);
        }
    } else {
        PhysReplayPlayUpdate(g_phobs, g_nphobs);
    }
    i_pr_overhead_begin();
    int prof2 = i_prof_start(k_str_prof_common);
    i_pr_overhead_end();
    for (int i = 0; i < g_nphobs; i++) {
        PhobRoot* p = g_phobs[i];
        if (p) VFN(p, 0xc, void)(p, 0);                  // UpdateCommon
    }
    i_pr_overhead_begin();
    i_prof_stop(prof2);
    i_pr_overhead_end();
    i_pr_overhead_begin();
    i_prof_stop(prof);
    i_pr_overhead_end();
}
PORT_FN(0x004274d0, "update_phobs", update_phobs_rw, fp_replay_tick)

// ==== create_phob (0x4289c0), make_phob (0x428a70) ===============================================================
// A creation record in the stream: the phob's message offset (int; negative ends the list), then its PhobData
// (FourCC, size, ...), read whole into a 0x1e4-byte buffer on the stack (unchecked in the original: see FIX).
static uint8_t __cdecl create_phob_rw(void* s, PhobRoot** out) {
    struct { int32_t msg; uint8_t data[0x1e4]; } rec;
    MemStreamGetInt(s, &rec.msg);
    if (rec.msg < 0) return 0;
    int pos = MemStreamGetPos(s);
    MemStreamGetData(s, rec.data, 8);
    MemStreamSeekPos(s, pos);
    const int32_t size = ((PhobData*)rec.data)->size;
    // FIX: a record bigger than the buffer overran the original's stack (and a negative size copied ~4 GB). An
    // oversized record is read truncated to the buffer -- its first 0x1e4 bytes, which hold every field a phob
    // constructor reads -- and the stream is moved on past the rest of it, so the records after it still line up
    // (past the stream's end if it is cut short: the next read fails, as the original's own read would have). A
    // negative size is read as the 8-byte header alone.
    if (VP_FIX && (size < 0 || size > (int32_t)sizeof rec.data)) {
        logf("create_phob: a %08x record of %d bytes; the buffer holds %u", ((PhobData*)rec.data)->type, size,
             (unsigned)sizeof rec.data);
        if (size < 0) {
            MemStreamGetData(s, rec.data, 8);
        } else {
            MemStreamGetData(s, rec.data, (int)sizeof rec.data);
            MemStreamSeekPos(s, (int)((uint32_t)pos + (uint32_t)size));
        }
    } else
        MemStreamGetData(s, rec.data, size);
    PhobRoot* p = make_phob_o((PhobData*)rec.data, rec.msg);
    void** vt = p->vtable;
    if (((uint8_t(__fastcall*)(void*, Edx))vt[0x2c / 4])(p, 0)) PhysReplayRegisterPhob(p);   // IsDynamic
    *out = p;
    return 1;
}
static void fp_create_phob(Footprint& f, void*, PhobRoot**) { f.replay_only = "it reads the stream and allocates a phob"; }
PORT_FN(0x004289c0, "create_phob", create_phob_rw, fp_create_phob)

static PhobRoot* make(int bytes, uint32_t ctor, PhobData* d, int msg) {
    void* p = MemAlloc(bytes);
    if (!p) return 0;
    return ((PhobCtor_t)ctor)(p, 0, d, (void*)(intptr_t)msg);
}
static PhobRoot* __cdecl make_phob_rw(PhobData* d, int msg) {
    switch (d->type) {
    case 0x42414c4c: return make(0x480, 0x004410f0, d, msg);   // 'BALL' Ball
    case 0x41434152: return make(0x1080, 0x0042eac0, d, msg);  // 'ACAR' AICar
    case 0x47434152: return make(0xf00, 0x0042dde0, d, msg);   // 'GCAR' GhostCar
    case 0x43484b50: return make(0x84, 0x00440b50, d, msg);    // 'CHKP' CheckPoint
    case 0x4e434152: return make(0xf38, 0x0043f370, d, msg);   // 'NCAR' NetCar
    case 0x4f425354: return make(0x4b0, 0x0043cd90, d, msg);   // 'OBST' Obstacle
    case 0x50434152: return make(0xef0, 0x0043e710, d, msg);   // 'PCAR' PlayCar
    case 0x53544154: return make(0x6c, 0x0043e190, d, msg);    // 'STAT' PhobStatic
    case 0x574f424c: return make(0x4b4, 0x0043d400, d, msg);   // 'WOBL' Wobble
    }
    LogPanic(k_fmt_phob_type, d->type);
    return 0;
}
static void fp_make_phob(Footprint& f, PhobData*, int) { f.replay_only = "it allocates and constructs a phob"; }
PORT_FN(0x00428a70, "make_phob", make_phob_rw, fp_make_phob)

// ==== TimerConditioner::GetTicks (0x428c30) ======================================================================
// How many physics ticks this timer tick runs. Speed 0 (as fast as it can): 24 / the number of cars, yielding
// 20 ms when physics_tick's low byte is about to wrap; speed 2 (half): 1, 0, 1, 0...; playback, the pause
// menu or any other speed: 1. At normal speed live: the real time's deviation from the physics clock, in
// ticks rounded towards zero, plus one -- clamped to 4 in a network game, else resynchronised (one tick at 2
// and a fresh base next time) when it's below 0 or above 2; a resync call runs exactly 1 and takes the base.
static int __fastcall TimerConditioner_GetTicks_rw(TimerConditioner* self, Edx) {
    int n = 1;
    int speed = PhysicsGetSpeed_o();
    uint8_t fixed = !(!PhysReplayPlayMode() && !g_paused && speed == 1);
    if (speed == 0) {
        n = 24 / CarMgrCount();
        if ((uint8_t)g_tick < 0x80 && (uint8_t)((uint8_t)g_tick - (uint8_t)n) > 0x80) TaskSleep(20);
    } else if (speed == 2) {
        uint8_t t = self->half_toggle;
        n = t;
        self->half_toggle = t == 0;
    }
    g_tick_overflow = 0;
    if (fixed) return n;
    if (self->resync) {
        self->time = (float)PhysicsGetTime_o();
        self->ptime = PTimeNow();
        self->resync = 0;
        return n;
    }
    n = x87_ftol((TimerConditioner_PhysicsDeviation_o(self, 0) + D(Fb(k_dt))) * D(Fb(k_one_over_dt)));
    if (MultiEnabled()) {
        if (n > 4) {
            n = 4;
            g_tick_overflow = 1;
        }
        return n;
    }
    if (n < 0 || n > 2) {
        self->resync = 1;
        g_tick_overflow = 1;
        n = 2;
    }
    return n;
}
static void fp_get_ticks(Footprint& f, TimerConditioner*, Edx) { f.replay_only = "it reads the real-time clock and may sleep"; }
PORT_FN(0x00428c30, "TimerConditioner::GetTicks", TimerConditioner_GetTicks_rw, fp_get_ticks)

// ==== TimerConditioner::PhysicsDeviation (0x428d50) ==============================================================
// How far real time has run ahead of the physics clock, in seconds: the base time plus the real milliseconds
// since the base (stored as a float), less PhysicsGetTime (in the register).
static double __fastcall TimerConditioner_PhysicsDeviation_rw(TimerConditioner* self, Edx) {
    int32_t now_ms = PTimeNow();                          // (the base is read after the call, as the original)
    int32_t d = (int32_t)((uint32_t)now_ms - (uint32_t)self->ptime);
    float t = (float)(D(d) * D(Fb(k_milli)) + D(self->time));
    double now = PhysicsGetTime_o();
    return D(t) - now;
}
static void fp_deviation(Footprint& f, TimerConditioner*, Edx) { f.replay_only = "it reads the real-time clock"; }
PORT_FN(0x00428d50, "TimerConditioner::PhysicsDeviation", TimerConditioner_PhysicsDeviation_rw, fp_deviation)

// ==== PhysTaskGetTelemetry (0x428d80) ============================================================================
static void* __cdecl PhysTaskGetTelemetry_rw() {
    SingleEnter(g_task_single, 0, 0);
    SingleLeave(g_task_single, 0, 0);
    return (void*)0x00520c58;                            // struct PhysicsTelemetry
}
PORT_FN(0x00428d80, "PhysTaskGetTelemetry", PhysTaskGetTelemetry_rw, fp_none)

// =================================================================================================================
// physics.obj
// =================================================================================================================

// ==== PhysicsBegin (0x42b910) ====================================================================================
static void __cdecl PhysicsBegin_rw(const void* options, int) {
    g_phys_single = SingleBegin(k_str_physics);
    g_state = 0;
    g_paused = 1;
    g_please_stop = 0;
    g_speed = 1;
    g_tick = 0;
    g_camera_type = 0;
    g_camera_focus = 0;
    g_realism = *(const int32_t*)options;                // GameOptions +0 realism
    g_damage = ((const uint8_t*)options)[0x24];          // +0x24 damage on
    g_msg_tail = 0;
    g_stream = MemStreamCreate(0x3c800);
    g_stream_ptr = MemStreamCreatePtr(g_stream);
    MemStreamSeekStart(g_stream_ptr);
    DriverBegin();
    PhysicsDashboardBegin();
    g_metric = TelemetryCreateMetric(k_str_elapsed);
}
static void fp_physics_begin(Footprint& f, const void*, int) { f.replay_only = "it creates the creation stream, the driver and the dashboard"; }
PORT_FN(0x0042b910, "PhysicsBegin", PhysicsBegin_rw, fp_physics_begin)

// ==== PhysicsEnd (0x42b9c0) ======================================================================================
static void __cdecl PhysicsEnd_rw() {
    g_state = 0;
    PhysicsDashboardEnd();
    DriverEnd();
    MemStreamDestroyPtr(g_stream_ptr);
    MemStreamDestroy(g_stream);
    OptionsSet(*k_options_section, k_str_camera, g_camera_type);
    SingleEnd(g_phys_single, 0, 0);
}
static void fp_physics_end(Footprint& f) { f.replay_only = "it destroys the stream, the driver and the dashboard"; }
PORT_FN(0x0042b9c0, "PhysicsEnd", PhysicsEnd_rw, fp_physics_end)

// ==== PhysicsStart (0x42ba30) ====================================================================================
// Ends the creation records (-1), hooks physics_thread into the timer and waits for it to take state 1 (the
// task's begin) -- a minute and it panics, each further 10 ms -- then opens the packet's reading side and runs.
static void __cdecl PhysicsStart_rw() {
    SingleEnter(g_phys_single, 0, 0);
    g_state = 1;
    TimerWatchdog_init_o(g_watchdog, 0);
    MemStreamPutInt(g_stream_ptr, -1);
    int waited = 0;
    BGHook(k_physics_thread, 0);
    if (g_state == 1) {
        do {
            waited += 10;
            TaskSleep(10);
            if (waited >= 60000) LogPanic(k_str_reboot);
        } while (g_state == 1);
    }
    g_shm_r = ShmemGet(k_str_shmem2);
    g_state = 3;
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_physics_start(Footprint& f) { f.replay_only = "it starts the timer thread and waits for it"; }
PORT_FN(0x0042ba30, "PhysicsStart", PhysicsStart_rw, fp_physics_start)

// ==== PhysicsStop (0x42bae0), PhysicsRestart (0x42bb50), PhysicsPostRace (0x42bba0) ==============================
static void __cdecl PhysicsStop_rw() {
    SingleEnter(g_phys_single, 0, 0);
    g_state = 7;
    while (g_state != 8) TaskSleep(10);
    BGUnhook(k_physics_thread);
    TimerWatchdog_dump_o(g_watchdog, 0);
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_physics_stop(Footprint& f) { f.replay_only = "it waits for the timer thread to end the race, unhooks it and writes timer.log"; }
PORT_FN(0x0042bae0, "PhysicsStop", PhysicsStop_rw, fp_physics_stop)

static void __cdecl PhysicsRestart_rw() {
    SingleEnter(g_phys_single, 0, 0);
    g_state = 4;
    while (g_state == 4) TaskSleep(10);
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_physics_restart(Footprint& f) { f.replay_only = "it hands the restart to the timer thread and waits"; }
PORT_FN(0x0042bb50, "PhysicsRestart", PhysicsRestart_rw, fp_physics_restart)

static void __cdecl PhysicsPostRace_rw() {
    SingleEnter(g_phys_single, 0, 0);
    PhysicsPause_o();
    g_state = 5;
    while (g_state == 5) TaskSleep(10);
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_physics_postrace(Footprint& f) { f.replay_only = "it hands the post-race to the timer thread and waits"; }
PORT_FN(0x0042bba0, "PhysicsPostRace", PhysicsPostRace_rw, fp_physics_postrace)

// ==== PhysicsAbortRace (0x42bc00) ================================================================================
static void __cdecl PhysicsAbortRace_rw() { g_please_stop = 1; }
static void fp_abort(Footprint& f) { f.add((void*)&g_please_stop, 1, "please_stop"); }
PORT_FN(0x0042bc00, "PhysicsAbortRace", PhysicsAbortRace_rw, fp_abort)

// ==== PhysicsCreate (0x42bc10) ===================================================================================
// A creation record: the message offset (returned), then the PhobData; the message area grows by its type's size.
static void __cdecl PhysicsCreate_rw(const PhobData* d, int* msg) {
    SingleEnter(g_phys_single, 0, 0);
    int tail = g_msg_tail;
    *msg = tail;
    MemStreamPutInt(g_stream_ptr, tail);
    MemStreamPutData(g_stream_ptr, d, d->size);
    int n = PhysTaskGetMsgSize_o(d->type);
    g_msg_tail += n;
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_physics_create(Footprint& f, const PhobData*, int*) { f.replay_only = "it writes the creation stream (a growing buffer)"; }
PORT_FN(0x0042bc10, "PhysicsCreate", PhysicsCreate_rw, fp_physics_create)

// ==== the getters and setters ====================================================================================
static double __cdecl PhysicsGetTime_rw() { return D(g_tick) * D(Fb(k_dt)); }   // fild; fmul -- ST0
PORT_FN(0x0042bc80, "PhysicsGetTime", PhysicsGetTime_rw, fp_none)

static float __cdecl PhysicsGetTemperature_rw() { return *(const float*)&g_temperature; }   // fld dword
PORT_FN(0x0042bca0, "PhysicsGetTemperature", PhysicsGetTemperature_rw, fp_none)

static void __cdecl PhysicsSetTemperature_rw(uint32_t kelvin) { g_temperature = kelvin; }   // a float, as bits
static void fp_set_temperature(Footprint& f, uint32_t) { f.add(&g_temperature, 4, "temperature"); }
PORT_FN(0x0042bcb0, "PhysicsSetTemperature", PhysicsSetTemperature_rw, fp_set_temperature)

static void __cdecl PhysicsPause_rw() {
    SingleEnter(g_phys_single, 0, 0);
    g_paused = 1;
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_pause(Footprint& f) { f.add((void*)&g_paused, 1, "physics_paused"); }
PORT_FN(0x0042bcc0, "PhysicsPause", PhysicsPause_rw, fp_pause)

static void __cdecl PhysicsUnpause_rw() {
    SingleEnter(g_phys_single, 0, 0);
    g_paused = 0;
    SingleLeave(g_phys_single, 0, 0);
}
PORT_FN(0x0042bcf0, "PhysicsUnpause", PhysicsUnpause_rw, fp_pause)

static uint8_t __cdecl PhysicsIsPaused_rw() { return g_paused; }
PORT_FN(0x0042bd20, "PhysicsIsPaused", PhysicsIsPaused_rw, fp_none)

static void __cdecl PhysicsSetSpeed_rw(int speed) { g_speed = speed; }
static void fp_set_speed(Footprint& f, int) { f.add((void*)&g_speed, 4, "physics_speed"); }
PORT_FN(0x0042bd30, "PhysicsSetSpeed", PhysicsSetSpeed_rw, fp_set_speed)

static int __cdecl PhysicsGetSpeed_rw() { return g_speed; }
PORT_FN(0x0042bd40, "PhysicsGetSpeed", PhysicsGetSpeed_rw, fp_none)

static int __cdecl PhysicsGetRealism_rw() { return g_realism; }
PORT_FN(0x0042bd50, "PhysicsGetRealism", PhysicsGetRealism_rw, fp_none)

static uint8_t __cdecl PhysicsIsDamageOn_rw() { return g_damage; }
PORT_FN(0x0042bd60, "PhysicsIsDamageOn", PhysicsIsDamageOn_rw, fp_none)

// ==== PhysicsGetStatePacket (0x42bd70), PhysicsReleaseStatePacket (0x42bdd0) =====================================
// The main thread's copy of the shared packet: the state (0x3d8 bytes) and the event queue; the pointer
// returned is the phobs' message area (+0x7dc), held until the release.
static uint8_t* __cdecl PhysicsGetStatePacket_rw(void* packet, PhysicsEventStream* events) {
    SingleEnter(g_phys_single, 0, 0);
    uint8_t* p = (uint8_t*)ShmemGetReadable(g_shm_r);
    uint32_t* dst = (uint32_t*)packet;
    const uint32_t* src = (const uint32_t*)p;
    for (int k = 0; k < 0xf6; k++) dst[k] = src[k];     // rep movsd
    PhysicsEventStream_assign_o(events, 0, (const PhysicsEventStream*)(p + 0x3d8));
    SingleLeave(g_phys_single, 0, 0);
    return p + 0x7dc;
}
static void fp_get_packet(Footprint& f, void*, PhysicsEventStream*) { f.replay_only = "it takes the shared packet's read lock (which may wait for a frame)"; }
PORT_FN(0x0042bd70, "PhysicsGetStatePacket", PhysicsGetStatePacket_rw, fp_get_packet)

static void __cdecl PhysicsReleaseStatePacket_rw() {
    SingleEnter(g_phys_single, 0, 0);
    ShmemReleaseReadable(g_shm_r);
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_release_packet(Footprint& f) { f.replay_only = "it releases the shared packet's read lock"; }
PORT_FN(0x0042bdd0, "PhysicsReleaseStatePacket", PhysicsReleaseStatePacket_rw, fp_release_packet)

// ==== PhysicsReadControls (0x42be10) =============================================================================
// The main thread reads the controls (DriverUpdate) with the physics time since it last did, clamped to
// [0.01, 0.25] s (a NaN gives 0.25), and reports the time to the telemetry.
static void __cdecl PhysicsReadControls_rw() {
    SingleEnter(g_phys_single, 0, 0);
    double t = PhysicsGetTime_o();
    float tf = (float)t;                                  // fst dword
    double dt = t - D(g_controls_time);
    double hi = D(Fb(k_quarter));
    dt = !(hi > dt) ? hi : dt;
    double lo = D(Fb(k_hundredth));
    dt = !(lo >= dt) ? dt : lo;
    DriverUpdate((float)dt);
    g_controls_time = tf;
    TelemetryUpdateMetric(g_metric, Ub(tf));
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_read_controls(Footprint& f) { f.replay_only = "DriverUpdate reads the keyboard and the game controllers"; }
PORT_FN(0x0042be10, "PhysicsReadControls", PhysicsReadControls_rw, fp_read_controls)

// ==== the camera setters (0x42bea0, 0x42beb0, 0x42bee0) ==========================================================
static void __cdecl CameraSetType_rw(int type) { g_camera_type = type; }
static void fp_camera_type(Footprint& f, int) { f.add((void*)&g_camera_type, 4, "camera_type"); }
PORT_FN(0x0042bea0, "CameraSetType", CameraSetType_rw, fp_camera_type)

static void __cdecl CameraSetFocusCar_rw(int car) {
    SingleEnter(g_phys_single, 0, 0);
    g_camera_focus = car;
    SingleLeave(g_phys_single, 0, 0);
}
static void fp_camera_focus(Footprint& f, int) { f.add((void*)&g_camera_focus, 4, "camera_focus"); }
PORT_FN(0x0042beb0, "CameraSetFocusCar", CameraSetFocusCar_rw, fp_camera_focus)

// the number of cars (registered from 0 up), less the incognito ghost
static int __cdecl CameraGetMaxFocusCar_rw() {
    SingleEnter(g_phys_single, 0, 0);
    int n = 0;
    while (PhysTaskFindCar_o(n)) {
        n++;
        if (n >= 16) break;
    }
    SingleLeave(g_phys_single, 0, 0);
    if (GhostCarIncognito()) n--;
    return n;
}
PORT_FN(0x0042bee0, "CameraGetMaxFocusCar", CameraGetMaxFocusCar_rw, fp_none)

// ==== PhysicsTimeString (0x42bf30) ===============================================================================
// m:ss.hh (or m:ss.h), with the locale's decimal point; an hour or more shows 99:99.
static const char* __cdecl PhysicsTimeString_rw(float t, uint8_t hundredths) {
    int32_t s = x87_ftol(D(t));
    int32_t frac = x87_ftol((D(t) - D(s)) * D(Fb(k_hundred)));   // fisub; fmul
    int32_t min = s / 60;
    int32_t sec = (int32_t)((uint32_t)s - (uint32_t)(min * 60));
    if (min >= 60) {
        int dp = (signed char)*g_locale;
        if (hundredths) sprintf_o(g_time_string, k_fmt_overflow2, dp);
        else sprintf_o(g_time_string, k_fmt_overflow1, dp);
        return g_time_string;
    }
    if (hundredths) sprintf_o(g_time_string, k_fmt_time2, min, sec, (int)(signed char)*g_locale, frac);
    else sprintf_o(g_time_string, k_fmt_time1, min, sec, (int)(signed char)*g_locale, frac / 10);
    return g_time_string;
}
static void fp_time_string(Footprint& f, float, uint8_t) { f.add(g_time_string, 0x24, "the time string"); }
PORT_FN(0x0042bf30, "PhysicsTimeString", PhysicsTimeString_rw, fp_time_string)

// ==== PhysicsEventStream::operator= (0x42c020) ===================================================================
static PhysicsEventStream* __fastcall PhysicsEventStream_assign_rw(PhysicsEventStream* self, Edx, const PhysicsEventStream* o) {
    uint32_t n = o->len;
    self->len = n;
    uint8_t* dst = self->data;
    const uint8_t* src = o->data;
    for (uint32_t k = 0; k < (n >> 2); k++, dst += 4, src += 4) memcpy(dst, src, 4);   // rep movsd, forwards
    for (uint32_t k = 0; k < (n & 3); k++) *dst++ = *src++;
    return self;
}
static void fp_events_assign(Footprint& f, PhysicsEventStream* self, Edx, const PhysicsEventStream* o) {
    uint32_t n = o->len;
    if (n > 0x400) { f.replay_only = "an event queue longer than its buffer"; return; }
    f.add(self, 4 + n, "the event stream");
}
PORT_FN(0x0042c020, "PhysicsEventStream::operator=", PhysicsEventStream_assign_rw, fp_events_assign)

// ==== physics_thread (0x42c050) ==================================================================================
// Not a loop: the background timer calls it every tick once PhysicsStart has hooked it (BGHook(0x42c050)), so
// the rewrite, hooked at that address, is what the timer calls. The state machine is described at the top.
static void __cdecl physics_thread_rw() {
    ExceptSinglePrecision(1);
    TimerWatchdog_tick_begin_o(g_watchdog, 0);
    switch (g_state) {
    case 1:
        PhysTaskBegin_o(g_stream);
        g_state = 2;
        break;
    case 2: case 8: break;
    case 3: PhysTaskUpdate_o(); break;
    case 4:
        PhysTaskRestart_o();
        g_state = 3;
        break;
    case 5:
        PhysTaskPostRace_o();
        PhysTaskUpdate_o();
        g_state = 6;
        break;
    case 6: PhysTaskUpdate_o(); break;
    case 7:
        PhysTaskEnd_o();
        g_state = 8;
        break;
    default: LogPanic(k_fmt_state, g_state); break;
    }
    if (g_please_stop) {
        void* d = g_deity;
        g_please_stop = 0;
        VFN(d, 8, void)(d, 0);                           // Deity::StopRace
    }
    TimerWatchdog_tick_end_o(g_watchdog, 0);
}
static void fp_physics_thread(Footprint& f) { f.replay_only = "the timer thread's whole tick (the task's state machine)"; }
PORT_FN(0x0042c050, "physics_thread", physics_thread_rw, fp_physics_thread)

// ==== TimerWatchdog (0x42c140 dump, 0x42c1e0 init, 0x42c200 tick_end, 0x42c220 tick_begin) =======================
// FIX: c:\timer.log -- the root of C:, not writable without elevation, so it was never written -- is
// <race.exe's folder>\log\timer.log instead, the folder made (fix_paths.h; vrmod's patched "log\timer.log" names the
// same file from the game's folder). A folder too long for the file table's 0x100-byte names keeps the literal.
static void __fastcall TimerWatchdog_dump_rw(TimerWatchdog* self, Edx) {
    char line[0x80];
    const char* name = k_str_timer_log;
    char path[0x100];
    if (VP_FIX) {
        if (vp_log_path(path, sizeof path, "timer.log")) name = path;
        else logf("fix: race.exe's folder isn't known, or is too long for the timer log's path; it stays at %s", name);
    }
    int file = FileCreate(name);
    if (!file) {
        LogReport(k_str_timer_fail);
        return;
    }
    for (int i = 0; i < 128; i++) FilePrintf(file, line, k_fmt_timer_bin, i - 64, self->hist[i]);
    int n = self->outliers;
    if (n) FilePrintf(file, line, k_fmt_timer_avg, n, self->outlier_sum / n);
    FileClose(&file);
}
static void fp_watchdog_dump(Footprint& f, TimerWatchdog*, Edx) { f.replay_only = "it writes timer.log"; }
PORT_FN(0x0042c140, "TimerWatchdog::dump", TimerWatchdog_dump_rw, fp_watchdog_dump)

static void __fastcall TimerWatchdog_init_rw(TimerWatchdog* self, Edx) {
    self->ticks = 0;
    self->measured = 0;
    memset(&self->last, 0, 4);
    self->slowest = 8;
    self->outliers = 0;
    self->outlier_sum = 0;
    for (int i = 0; i < 128; i++) self->hist[i] = 0;
}
static void fp_watchdog_init(Footprint& f, TimerWatchdog* self, Edx) { f.add(self, sizeof(TimerWatchdog), "the watchdog"); }
PORT_FN(0x0042c1e0, "TimerWatchdog::init", TimerWatchdog_init_rw, fp_watchdog_init)

static void __fastcall TimerWatchdog_tick_end_rw(TimerWatchdog* self, Edx) {
    int32_t now = PTimeNow();
    int32_t took = (int32_t)((uint32_t)now - (uint32_t)self->tick_start);
    int32_t m = self->slowest;
    if (took > m && m < 32) self->slowest = m + 1;
}
static void fp_watchdog_clock(Footprint& f, TimerWatchdog*, Edx) { f.replay_only = "it reads the real-time clock"; }
PORT_FN(0x0042c200, "TimerWatchdog::tick_end", TimerWatchdog_tick_end_rw, fp_watchdog_clock)

// After 100 ticks, each interval less 16 ms goes in the histogram; one of +-64 ms or more (as the stored
// float's bits: any negative from -64, a negative NaN, anything from +64) counts as an outlier instead.
static void __fastcall TimerWatchdog_tick_begin_rw(TimerWatchdog* self, Edx) {
    int32_t now = PTimeNow();
    self->tick_start = now;
    int32_t c = self->ticks + 1;
    self->ticks = c;
    if (c <= 100) return;
    if ((Ub(self->last) & 0x7fffffffu) == 0) {
        self->last = (float)D(now);                      // fild; fstp dword
        return;
    }
    double nowd = D(now);
    float last = self->last;
    self->measured++;
    float dev = (float)((nowd - D(last)) - D(Fb(k_sixteen)));
    self->last = (float)nowd;
    uint32_t db = Ub(dev);
    if (db >= 0xc2800000u || (int32_t)db >= 0x42800000) {
        self->outliers++;
        int32_t e = x87_ftol(D(dev));
        uint32_t a = e < 0 ? 0u - (uint32_t)e : (uint32_t)e;   // cdq; xor; sub (INT_MIN stays)
        self->outlier_sum = (int32_t)((uint32_t)self->outlier_sum + a);
        return;
    }
    int32_t e = x87_ftol(D(dev));
    int32_t k = (int32_t)((uint32_t)e + 64);
    if (k >= 0 && k < 128) {
        self->hist[k]++;
        return;
    }
    LogPanic(k_str_dolt);
}
PORT_FN(0x0042c220, "TimerWatchdog::tick_begin", TimerWatchdog_tick_begin_rw, fp_watchdog_clock)

// =================================================================================================================
// driver.obj: the 13 control readers (the physics thread's PlayCar::Update reads them; DriverUpdate fills them on
// the main thread). The recorder hooks these same addresses (replay.cpp).
// =================================================================================================================
#define g_steer        ((const float*)0x00522200)
#define g_steer_square (*(const uint8_t*)0x005222a4)

// fld [steer]; fabs; fmul [steer]; fstp -- by construction (a NaN's sign and payload follow the x87's rules)
static __forceinline float steer_squared_signed() {
    float r;
    __asm {
        mov eax, 0x00522200
        fld dword ptr [eax]
        fabs
        fmul dword ptr [eax]
        fstp r
    }
    return r;
}

// The steering: squared (keeping its sign) for the "square" option; above 15.6 m/s (35 mph, as the argument's
// bits) scaled down linearly towards 100 mph: x (1.269 - 0.0173 v), v capped at 44.44 (a NaN gives 44.44).
static float __cdecl DriverGetSteering_rw(float speed) {
    float s;
    memcpy(&s, g_steer, 4);
    if (g_steer_square) s = steer_squared_signed();
    int32_t sb;
    memcpy(&sb, &speed, 4);
    if (sb > 0x4178e38e) {
        double v = D(speed), cap = D(Fb(k_steer_speed));
        v = !(cap > v) ? cap : v;
        s = (float)((v * D(Fb(k_steer_slope)) + D(Fb(k_steer_base))) * D(s));
    }
    return s;
}
static void fp_driver_steering(Footprint&, float) {}
PORT_FN(0x00441df0, "DriverGetSteering", DriverGetSteering_rw, fp_driver_steering)

static float __cdecl DriverGetThrottle_rw() { return *(const float*)0x0052229c; }
PORT_FN(0x00441e60, "DriverGetThrottle", DriverGetThrottle_rw, fp_none)

static float __cdecl DriverGetBraking_rw() { return *(const float*)0x005221c8; }
PORT_FN(0x00441e70, "DriverGetBraking", DriverGetBraking_rw, fp_none)

// the pedal's travel mapped to engagement: 1.1 - 1.2 x, clamped to [0, 1] in the register (a NaN gives 1)
static double __cdecl DriverGetClutch_rw() {
    double v = D(*(const float*)0x00522270) * D(Fb(k_clutch_slope)) + D(Fb(k_clutch_base));
    double zero = D(Fb(k_zero));
    v = !(zero >= v) ? v : zero;
    double one = D(Fb(k_one));
    v = !(one > v) ? one : v;
    return v;
}
PORT_FN(0x00441e80, "DriverGetClutch", DriverGetClutch_rw, fp_none)

static float __cdecl DriverGetEBrake_rw() { return *(const float*)0x00522260; }
PORT_FN(0x00441ec0, "DriverGetEBrake", DriverGetEBrake_rw, fp_none)

static int __cdecl DriverGetGear_rw() { return *(const int32_t*)0x00522264; }
PORT_FN(0x00441ed0, "DriverGetGear", DriverGetGear_rw, fp_none)

static uint8_t __cdecl DriverGetHorn_rw() { return *(const uint8_t*)0x005222a0; }
PORT_FN(0x00441ef0, "DriverGetHorn", DriverGetHorn_rw, fp_none)

static uint8_t __cdecl DriverGetAirlift_rw() { return *(const uint8_t*)0x00522208; }
PORT_FN(0x00441f00, "DriverGetAirlift", DriverGetAirlift_rw, fp_none)

static uint8_t __cdecl DriverGetReverse_rw() { return *(const uint8_t*)0x00522298; }
PORT_FN(0x00441f10, "DriverGetReverse", DriverGetReverse_rw, fp_none)

static uint8_t __cdecl DriverGetHelp_rw() { return *(const uint8_t*)0x00522274 != 0; }
PORT_FN(0x00441f20, "DriverGetHelp", DriverGetHelp_rw, fp_none)

static float __cdecl DriverGetLookSide_rw() { return *(const float*)0x005221ec; }
PORT_FN(0x00441f30, "DriverGetLookSide", DriverGetLookSide_rw, fp_none)

static float __cdecl DriverGetPitch_rw() { return *(const float*)0x005221e8; }
PORT_FN(0x00441f40, "DriverGetPitch", DriverGetPitch_rw, fp_none)

static uint8_t __cdecl DriverGetLookBack_rw() { return *(const uint8_t*)0x005222c8; }
PORT_FN(0x00441f50, "DriverGetLookBack", DriverGetLookBack_rw, fp_none)
