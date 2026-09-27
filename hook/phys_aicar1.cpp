// phys_aicar1.cpp -- M3 3.5 group A1: the AI driver, first half of physics:aicar.obj (0x42e5c0..0x4300f0).
//
// AICar is the LocalCar the computer drives. Each think it chases a rabbit point on its racing line (the
// ai library's IdealLine: get_rabbit_position, a lookahead x speed ahead of the car's bead), blended from the
// grid lane at the launch and toward the recovery point after four wheels went off the road, nudged by the
// per-segment .dnt notes (speed and lateral), and pushed sideways to pass a car it's about to hit; the
// target is then made car-local (localify_*). The pedals and the wheel go through AISetThrottle / Brake /
// Steering (skill scalings, slew limits, the too-fast throttle cap). check_for_too_fast rubber-bands against
// a lap-time schedule (the line's cumulative time x pace against the deity's lap clock); check_for_stranded
// teleports a car that has made no progress on the line for skill+6 s (teleport_to_track, through the
// deity); check_for_damage brakes a car with a broken wheel. Also here: the replay packet's AI part (the
// target point and the _MSG reason bitmask), the constructor / destructor / reset, and the external force
// and impulse hooks that feed the slam steering, the wall heat and the notes' hit_wall.
//
//   Skipped: the 34 $E static initialisers (0x42e3a0..0x42e5b0, the msg colours) and AICar::show_msg
//   (0x42fb70), a bare `ret` (called here by address all the same).
//
// Fixes (port.h: VP_FIX; VP_FAITHFUL builds the original's behaviour): the known AI crash. A stranded car's teleport
// (teleport_to_track -> IdealLine::update_car_info / reset_bead_position) or a NaN position could leave the line's
// bead segment NULL; get_ilpos then handed out a NULL segment, and check_for_too_fast read seg->next through it.
// reset_bead_position (phys_ideal.cpp) now puts a lost bead back at the nearest point of the line itself, and
// check_for_too_fast asks it to before reading. update_segment_info checks for NULL; the other reader
// (init_rt_lat) is the second half's. And stuff_event's text is bounded to its 32-byte buffer.
//
// Written from the v1.0 disassembly: register values as double, stored values as float, the same grouping,
// integer compares of float bits done on the bits, float copies made with integer moves kept as bit copies,
// and every call -- Car's methods, the ai library (IdealLine, AIGet*, the deity), this file's own functions
// -- made by address or through the vtable exactly as the original does, so whichever version is hooked is
// what runs. Nothing is inlined.
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
static __forceinline uint32_t U(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline float FB(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
#define COPY4(dst, src) memcpy(&(dst), &(src), 4)  // an integer move of a float
#define SETB(dst, u) do { const uint32_t u_ = (u); memcpy(&(dst), &u_, 4); } while (0)   // mov dword ptr, imm

namespace {

// ---- the ai library's types (ideal.obj; out/types.tsv) -----------------------------------------------------
struct Point2D { float x, z; };
struct ILSeg {                                      // 68 bytes: a node of the racing line (a closed loop)
    ILSeg* next;                                    // +0x00
    float x, z;                                     // +0x04
    float dir_x, dir_z;                             // +0x0c
    float corridor_half_width;                      // +0x14
    float target_speed;                             // +0x18 m/s
    float curvature;                                // +0x1c
    float step;                                     // +0x20 m to the next node
    float cum_distance;                             // +0x24
    int16_t index;                                  // +0x28
    uint8_t sector, pad_ff;                         // +0x2a
    float field_11, dist_ahead, dist_ahead_signed;  // +0x2c
    float cum_time;                                 // +0x38 s from the start of the lap
    float lateral;                                  // +0x3c
    uint32_t pool_guard;                            // +0x40
};
static_assert(offsetof(ILSeg, target_speed) == 0x18 && offsetof(ILSeg, index) == 0x28 &&
              offsetof(ILSeg, cum_time) == 0x38 && sizeof(ILSeg) == 68, "ILSeg");
struct ILinePos { ILSeg* seg; float t; };           // a point on the line: a segment and 0..1 along it
struct IdealLine {                                  // 56 bytes (a ConstIdealLine adds 4: 60)
    void** vtable;
    ILSeg* bead_seg;                                // +0x04 the car's segment (NULL: the crash)
    float bead_t;                                   // +0x08
    uint8_t _0c[0x2c - 0x0c];
    ILSeg* head;                                    // +0x2c
    uint8_t bead_on_line, _31[3];                   // +0x30
    int32_t num_segs;                               // +0x34
};
static_assert(offsetof(IdealLine, head) == 0x2c && offsetof(IdealLine, num_segs) == 0x34, "IdealLine");
enum { IDEAL_LINE_SIZE = 60 };                      // AIGetLine's: the AICarInfo's ConstIdealLine (+0x10)

// ---- AICar's own ---------------------------------------------------------------------------------------------
struct Notes {                                      // AICar::SegmentInfo::Notes, 8 bytes
    uint8_t hit_wall, off_road_a, off_road_b, _3;   // clear() zeroes these three (a word and a byte)
    float speed_note;                               // +4 m/s added to the target speed
};
struct SegmentInfo {                                // AICar::SegmentInfo, 24 bytes: one per line segment
    Notes notes;                                    // +0x00
    float lateral_note;                             // +0x08 m along center_right
    uint8_t locked, _0d[3];                         // +0x0c
    float lookahead;                                // +0x10 s: lookahead_from_curvature
    ILSeg* seg;                                     // +0x14
};
static_assert(sizeof(SegmentInfo) == 24, "SegmentInfo");
struct ProxerInfo { float level; uint32_t _4; };    // 8 bytes

// Wheel (wheel.obj, 420 bytes): only what this group reads is named (the full layout is in phys_wheel_update.cpp)
struct Wheel {
    void** vtable;
    uint8_t _004[0x20 - 0x04];
    uint8_t broken, _021[3];                        // +0x020 damaged()
    uint8_t _024[0xdc - 0x24];
    float steer_input;                              // +0x0dc rad
    uint8_t _0e0[0x168 - 0xe0];
    uint8_t on_ground, _169[3];                     // +0x168 off_ground()
    uint8_t _16c[0x184 - 0x16c];
    void* tire_sound;                               // +0x184 (UpdateCommon's footprint)
    uint8_t _188[0x19c - 0x188];
    int32_t surface;                                // +0x19c 14: water (on_water())
    uint8_t _1a0[4];
};
static_assert(offsetof(Wheel, steer_input) == 0xdc && offsetof(Wheel, on_ground) == 0x168 &&
              offsetof(Wheel, surface) == 0x19c && sizeof(Wheel) == 0x1a4, "Wheel");

// AICar (aicar.obj, 4224 bytes, vtable 0x4dbc28), over LocalCar (3784) over Car (3768) over PhobDyno. Names
// from out/types.tsv; Car's fields only as far as this group touches them.
struct AICar : PhobDyno {
    // ---- Car
    void* body_volume;                  // +0x478 (footprints)
    uint8_t _47c[0x4c4 - 0x47c];
    int32_t live_models[5];             // +0x4c4 (footprints: the model_infos damage rebuilds)
    uint8_t _4d8[0x4f4 - 0x4d8];
    int32_t realism;                    // +0x4f4
    int32_t yaw_control;                // +0x4f8
    int32_t race_state;                 // +0x4fc 3: finished
    float steering;                     // +0x500 -1..1
    uint8_t _504[0x510 - 0x504];
    int32_t car_index;                  // +0x510
    uint8_t _514[0x534 - 0x514];
    uint8_t auto_clutch, _535[3];       // +0x534
    uint8_t _538[0x554 - 0x538];
    Wheel wheels[4];                    // +0x554
    uint8_t _be4[0xdcc - 0xbe4];
    float max_steer_angle;              // +0xdcc rad
    uint8_t _dd0[0xde4 - 0xdd0];
    float braking;                      // +0xde4
    uint8_t _de8[0xe4c - 0xde8];
    void* sounds[7];                    // +0xe4c start, engine (EngineSound), shift, road, road2, scrape, horn
    uint8_t _e68[0xea4 - 0xe68];
    float lat_g;                        // +0xea4
    uint8_t _ea8[0xec0 - 0xea8];
    // ---- LocalCar
    uint8_t teleported, _ec1[3];        // +0xec0
    uint32_t _ec4;
    // ---- AICar
    void* drive_fn;                     // +0xec8 fast_drive / slow_drive
    void* interact_fn;                  // +0xecc fast_interact / slow_interact
    uint8_t ahead_valid, _ed1[3];       // +0xed0
    float ahead_dist, ahead_rel_speed;  // +0xed4
    float side_lat, side_lat_rel_speed, side_lon, side_lon_rel_speed;   // +0xedc
    uint8_t side_i_lead, _eed[3];       // +0xeec
    float slam_steer;                   // +0xef0
    uint8_t side_valid, side_squeezed, _ef6[2];   // +0xef4
    float contact_time;                 // +0xef8 s
    float contact_line_lat;             // +0xefc m
    float pass_offset;                  // +0xf00 m
    float contact_center_lat;           // +0xf04 m
    uint8_t contact_valid, contact_multiple, _f0a[2];   // +0xf08
    float now;                          // +0xf0c
    float speed;                        // +0xf10 m/s
    float track_half_width;             // +0xf14
    float center_lat;                   // +0xf18
    float center_lat_norm;              // +0xf1c
    float center_lat_delta;             // +0xf20
    float line_lat;                     // +0xf24
    float lat_accel;                    // +0xf28 m/s^2: Car lat_g x 9.81 (init_rtinfo)
    float race_fraction;                // +0xf2c
    uint8_t finished, _f31[3];          // +0xf30
    int32_t lap;                        // +0xf34
    uint8_t race_begun, _f39[3];        // +0xf38
    float start_lat;                    // +0xf3c
    uint32_t tick;                      // +0xf40
    ILSeg* progress_seg;                // +0xf44
    float steer_damping;                // +0xf48
    float steer;                        // +0xf4c
    float steer_gain;                   // +0xf50
    uint8_t tc_always, _f55[3];         // +0xf54
    uint8_t* skill;                     // +0xf58 SkillInfo2 (float fields by offset: skf)
    Point2D target_world;               // +0xf5c
    Point2D target_pos;                 // +0xf64
    Point2D target_vel;                 // +0xf6c
    Point2D target_pos_dir;             // +0xf74
    Point2D target_vel_dir;             // +0xf7c
    float target_speed;                 // +0xf84
    Point2D center_pos;                 // +0xf88
    Point2D center_recover_vel;         // +0xf90
    Point2D center_tan;                 // +0xf98
    Point2D edge_offset;                // +0xfa0
    Point2D side_edge_offset;           // +0xfa8
    Point2D center_right;               // +0xfb0
    float progress_time;                // +0xfb8
    float finish_time;                  // +0xfbc
    float crash_time;                   // +0xfc0
    float bump_time;                    // +0xfc4
    float fouroff_time;                 // +0xfc8
    float race_start_time;              // +0xfcc
    float freeze_time;                  // +0xfd0
    float wall_heat;                    // +0xfd4
    uint32_t _fd8;
    float recover_side;                 // +0xfdc +-1
    float offground_cut;                // +0xfe0
    float line_update_time;             // +0xfe4
    IdealLine* line;                    // +0xfe8
    const char* msgs[10];               // +0xfec
    int32_t num_msgs;                   // +0x1014
    float min_brake;                    // +0x1018
    uint8_t dice_cold_apex, dice_hot_entry, dice_unused, _101f;   // +0x101c
    float too_fast;                     // +0x1020 0..1
    float lap_target_time;              // +0x1024
    float base_lap_time;                // +0x1028
    float lap_time_spread;              // +0x102c
    float pace;                         // +0x1030
    float schedule_error;               // +0x1034
    float* lap_clock;                   // +0x1038
    float throttle_cap;                 // +0x103c
    float lap_time_offset;              // +0x1040
    float line_lap_time;                // +0x1044
    ProxerInfo* proxer_info;            // +0x1048
    int32_t num_proxer_info;            // +0x104c
    int32_t learn_hiatus;               // +0x1050
    SegmentInfo* seginfo;               // +0x1054
    int32_t num_segs;                   // +0x1058
    int32_t seg_index;                  // +0x105c
    int32_t next_seg_index;             // +0x1060
    float seg_t;                        // +0x1064
    uint8_t seg_changed, seg_valid, _106a[2];   // +0x1068
    float wall_hit_lat;                 // +0x106c
    int32_t last_learn_lap;             // +0x1070
    float latslam_rel_speed;            // +0x1074
    float launch_time;                  // +0x1078
    float teleport_armed_time;          // +0x107c
};
static_assert(offsetof(AICar, realism) == 0x4f4 && offsetof(AICar, car_index) == 0x510 && offsetof(AICar, wheels) == 0x554 &&
              offsetof(AICar, max_steer_angle) == 0xdcc && offsetof(AICar, braking) == 0xde4 && offsetof(AICar, lat_g) == 0xea4 &&
              offsetof(AICar, drive_fn) == 0xec8 && offsetof(AICar, slam_steer) == 0xef0 && offsetof(AICar, contact_time) == 0xef8 &&
              offsetof(AICar, contact_valid) == 0xf08 && offsetof(AICar, now) == 0xf0c && offsetof(AICar, lat_accel) == 0xf28 &&
              offsetof(AICar, finished) == 0xf30 && offsetof(AICar, progress_seg) == 0xf44 && offsetof(AICar, skill) == 0xf58 &&
              offsetof(AICar, target_world) == 0xf5c && offsetof(AICar, target_speed) == 0xf84 && offsetof(AICar, center_right) == 0xfb0 &&
              offsetof(AICar, progress_time) == 0xfb8 && offsetof(AICar, recover_side) == 0xfdc && offsetof(AICar, line) == 0xfe8 &&
              offsetof(AICar, num_msgs) == 0x1014 && offsetof(AICar, too_fast) == 0x1020 && offsetof(AICar, lap_clock) == 0x1038 &&
              offsetof(AICar, proxer_info) == 0x1048 && offsetof(AICar, seginfo) == 0x1054 && offsetof(AICar, seg_t) == 0x1064 &&
              offsetof(AICar, launch_time) == 0x1078 && sizeof(AICar) == 4224, "AICar");

// SkillInfo2 (the drivers.res record): floats by offset
static __forceinline const float& skf(const uint8_t* skill, uint32_t off) { return *(const float*)(skill + off); }

// the AI debug event (replay event 4): stuff_event's 0x2c bytes
struct AIEvent { int32_t kind; int32_t car; int32_t value; char text[32]; };
static_assert(sizeof(AIEvent) == 0x2c, "AIEvent");

}  // namespace

// ---- the game's functions, called by address --------------------------------------------------------------
typedef void(__cdecl* LogV_t)(const char*, ...);
typedef void(__cdecl* AddEvent_t)(int32_t, const void*, int32_t);
typedef void(__cdecl* InstallHandler_t)(int32_t, void*);
typedef void(__cdecl* UninstallHandler_t)(void*);
typedef int32_t(__cdecl* ReplHandler_t)(const void*, int32_t);
typedef void(__fastcall* CarVoid_t)(AICar*, Edx);
typedef void(__fastcall* CarBuf_t)(AICar*, Edx, uint8_t*);
typedef void(__fastcall* CarUpdateReplay_t)(AICar*, Edx, const uint8_t*, const uint8_t*, uint32_t);
typedef void(__fastcall* CarForce_t)(AICar*, Edx, const P3*, const P3*, int32_t);
typedef void(__fastcall* CarBits_t)(AICar*, Edx, uint32_t);    // a float argument passed as its bits
typedef void(__fastcall* CarInt_t)(AICar*, Edx, int32_t);
typedef void(__fastcall* CarByte_t)(AICar*, Edx, uint8_t);
typedef void(__fastcall* CarStr_t)(AICar*, Edx, const char*);
typedef uint8_t(__fastcall* CarRetByte_t)(AICar*, Edx);
typedef void(__fastcall* CarPoint_t)(AICar*, Edx, Point2D*);
typedef ILinePos*(__fastcall* CarIlpos_t)(AICar*, Edx, ILinePos*);
typedef void*(__fastcall* LocalCarCtor_t)(AICar*, Edx, void*, void*);
typedef void*(__fastcall* Ctor0_t)(void*, Edx);
typedef void(__fastcall* NotesClear_t)(Notes*, Edx);
typedef uint8_t(__cdecl* WheelPred_t)(Wheel*);
typedef int32_t(__cdecl* CountWheels_t)(WheelPred_t, Wheel*);
typedef double(__cdecl* Lookahead_t)(uint32_t);
typedef float(__cdecl* TerrainXZ_t)(uint32_t, uint32_t);
typedef float(__cdecl* GetFloat_t)();
typedef int32_t(__cdecl* GetInt_t)();
typedef uint8_t(__cdecl* GetByte_t)();
typedef uint8_t*(__cdecl* IntToPtr_t)(int32_t);
typedef int32_t(__cdecl* Stricmp_t)(const char*, const char*);
typedef void(__cdecl* RegisterAI_t)(AICar*, int32_t);
typedef IdealLine*(__cdecl* GetLine_t)(int32_t);
typedef void*(__cdecl* MemAlloc_t)(int32_t);
typedef void(__cdecl* Free_t)(void*);
typedef const uint8_t*(__cdecl* GetDriverData_t)(int32_t);
typedef void(__cdecl* ReleaseDriverData_t)(const uint8_t*);
typedef void(__cdecl* RecordNotes_t)(const void*, int32_t);
typedef ILinePos*(__fastcall* LineIlpos_t)(IdealLine*, Edx, ILinePos*);
typedef void(__fastcall* LineUpdateCarInfo_t)(IdealLine*, Edx, const Point2D*, const Point2D*);
typedef void(__fastcall* LineVoid_t)(IdealLine*, Edx);
typedef void(__fastcall* LineRabbit_t)(IdealLine*, Edx, Point2D*, Point2D*, uint32_t, ILinePos*);

#define LogPanic                    ((LogV_t)0x004112b0)
#define LogReport                   ((LogV_t)0x00411150)
#define PhysReplayAddEvent          ((AddEvent_t)0x0042d8b0)          // (an output: captured by the shadow check)
#define PhysReplayInstallEventHandler ((InstallHandler_t)0x0042d960)
#define PhysReplayUninstallEventHandler ((UninstallHandler_t)0x0042d990)
#define PhysReplayPlayMode          ((GetByte_t)0x0042d340)
#define PhysicsGetTime              ((GetFloat_t)0x0042bc80)
#define GetGameState                ((GetInt_t)0x0040a3d0)
#define TerrainGetHeightXZ          ((TerrainXZ_t)0x00465ae0)
#define WorldGetCarEntry            ((IntToPtr_t)0x00462720)
#define game_stricmp                ((Stricmp_t)0x004da350)
#define MemAlloc                    ((MemAlloc_t)0x004140e0)
#define operator_delete             ((Free_t)0x00414390)
#define AIGetSkill                  ((IntToPtr_t)0x00420d90)
#define AIGetTrackNumber            ((GetInt_t)0x00420df0)
#define AIGetTrackInfo              ((IntToPtr_t)0x00424e40)
#define AIRegisterAICar             ((RegisterAI_t)0x0041d220)
#define AIUnregisterCar             ((RegisterAI_t)0x0041d2e0)
#define AIGetLine                   ((GetLine_t)0x0041c2e0)
#define AICarCount                  ((GetInt_t)0x0041d330)
#define AILearnMode                 ((GetByte_t)0x0041d340)
#define GetDriverData               ((GetDriverData_t)0x00424b70)
#define ReleaseDriverData           ((ReleaseDriverData_t)0x00424be0)
#define aicar_record_notes          ((RecordNotes_t)0x00423780)
#define IdealLine_get_actual_bead_position ((LineIlpos_t)0x00421c10)
#define IdealLine_update_car_info   ((LineUpdateCarInfo_t)0x004214c0)
#define IdealLine_reset_bead_position ((LineVoid_t)0x00421520)
#define IdealLine_get_rabbit_position ((LineRabbit_t)0x00421a70)
#define LocalCar_ctor               ((LocalCarCtor_t)0x004444e0)
#define LocalCar_dtor               ((CarVoid_t)0x00444540)
#define Car_Reset                   ((CarVoid_t)0x00439f90)
#define Car_UpdateCommon            ((CarVoid_t)0x004391b0)
#define Car_GetMessage              ((CarBuf_t)0x00438ef0)
#define Car_MakeReplayPacket        ((CarBuf_t)0x00438cd0)
#define Car_UpdateReplay            ((CarUpdateReplay_t)0x00438950)
#define Car_ResolveExternalImpulse  ((CarForce_t)0x00437120)
#define Car_ApplyExternalForce      ((CarForce_t)0x00436a90)
#define Car_SetThrottle             ((CarBits_t)0x004396d0)
#define Car_SetBraking              ((CarBits_t)0x004396e0)
#define Car_SetSteering             ((CarBits_t)0x00439620)
#define Car_SetTractionControl      ((CarInt_t)0x004395e0)
#define Car_SetABSBraking           ((CarInt_t)0x00439600)
#define ProxerInfo_ctor             ((Ctor0_t)0x00432090)
// this object file's own, by address too (their rewrites, or the second half's, run when hooked)
#define call_repl_handler           ((ReplHandler_t)0x0042e5c0)
#define call_get_ilpos              ((CarIlpos_t)0x0042e860)
#define call_localify_vector        ((CarPoint_t)0x0042e790)
#define call_localify_loc           ((CarPoint_t)0x0042e7d0)
#define call_fast_mode              ((CarStr_t)0x0042ea80)
#define call_reset                  ((CarVoid_t)0x0042ec80)
#define call_suspend_adjustment     ((CarStr_t)0x0042ef80)
#define call_count_wheels           ((CountWheels_t)0x0042f190)
#define call_off_ground             ((WheelPred_t)0x0042f180)
#define call_set_yaw_control        ((CarByte_t)0x0042f460)
#define call_set_traction_control   ((CarByte_t)0x0042f4a0)
#define call_set_abs                ((CarByte_t)0x0042f4e0)
#define call_Notes_clear            ((NotesClear_t)0x0042f500)
#define call_MSG                    ((CarStr_t)0x0042f510)
#define call_teleport_to_track      ((CarRetByte_t)0x0042f560)
#define call_init_seginfo           ((CarVoid_t)0x0042f930)
#define call_lookahead_from_curvature ((Lookahead_t)0x0042fa00)
#define call_show_msg               ((CarVoid_t)0x0042fb70)
#define call_update_line_info       ((CarVoid_t)0x0042fb80)
#define call_get_mpath_target       ((CarBits_t)0x0042fc50)
#define call_init_rt_lat            ((CarVoid_t)0x004307e0)     // (the second half's)
// the predicates count_wheels is handed: their addresses, as the original pushes them
static const WheelPred_t PRED_ON_WATER = (WheelPred_t)0x0042f160;
static const WheelPred_t PRED_OFF_GROUND = (WheelPred_t)0x0042f180;
static const WheelPred_t PRED_DAMAGED = (WheelPred_t)0x0042f920;

static void** const VT_AICar = (void**)0x004dbc28;
static const uint32_t VT_RaceDeity = 0x004dc588;
#define g_deity (*(void**)0x005218ac)                       // class Deity* deity
#define g_race_deity_global (*(uint8_t**)0x004edd70)        // RaceDeity::global
#define g_instance_count (*(int32_t*)0x00521c74)            // AICar::instance_count
// msg_tab (0x4ed408, 24 entries): the _MSG reason strings, read from the table at the call, as the original does
static __forceinline const char* msg_tab(int i) { return ((const char* const*)0x004ed408)[i]; }
static const char* const* const MSG_TAB_BEGIN = (const char* const*)0x004ed408;
static const char* const* const MSG_TAB_END = (const char* const*)0x004ed468;
enum { MSG_MECCA_ZERO = 1, MSG_FOUROFF = 2, MSG_BEGIN = 3, MSG_WATER_CUT = 12, MSG_OFFGROUND_CUT = 13, MSG_PANIC_BRAKE = 14,
       MSG_CONTACT = 16, MSG_CONTACT_FLIP = 23 };
// the game's strings (the original passes these addresses)
static const char* const S_WHAT_THE = (const char*)0x004ed5c0;        // "What the!"
static const char* const S_CAR_IMPULSE = (const char*)0x004ed5cc;     // "car-impulse"
static const char* const S_INAPPROPRIATE = (const char*)0x004ed5d8;   // "AICar[%d]: driving inappropriate car (%s != %s)"
static const char* const S_RESET = (const char*)0x004ed62c;           // "reset"
static void* const REPL_HANDLER = (void*)0x0042e5c0;                  // AICar::repl_handler, as registered
static const uint32_t FAST_DRIVE = 0x00431ce0, FAST_INTERACT = 0x004312a0, SLOW_DRIVE = 0x00431960, SLOW_INTERACT = 0x00431950;

// ---- constants, by their bits ---------------------------------------------------------------------------------
static const float K_ONE = 1.0f;
static const float K_HALF = 0.5f;
static const float K_TWO = 2.0f;
static const float K_ZERO = 0.0f;
static const float K_2_5E7 = 25000000.0f;                 // 0x4bbebc20 (x skill+0x40 squared: a crash impulse)
static const float K_2_25E6 = 2250000.0f;                 // 0x4a095440 (x skill+0x3c squared: a bump)
static const float K_0_005 = FB(0x3ba3d70a);              // slam steering per car contact
static const float K_0_03 = FB(0x3cf5c28f);               // offground_cut per tick
static const float K_31_25 = 31.25f;
static const float K_0_9 = FB(0x3f666666);
static const float K_0_12 = FB(0x3df5c28f);
static const float K_0_8 = FB(0x3f4ccccd);
static const float K_10 = 10.0f;
static const float K_6 = 6.0f;
static const float K_5 = 5.0f;
static const float K_0_6 = FB(0x3f19999a);
static const float K_20 = 20.0f;
static const float K_0_4m = FB(0x3ecccccc);               // 0.39999998, not 0.4f
static const float K_0_3 = FB(0x3e99999a);
static const float K_9_81 = FB(0x411cf5c3);
static const float K_12 = 12.0f;
static const float K_1_12 = FB(0x3daaaaab);               // 1/12
static const float K_0_2 = FB(0x3e4ccccd);
static const float K_6_7056 = FB(0x40d69446);             // 22 ft: the passing offset
static const float K_5_715 = FB(0x40b6e148);              // 18.75 ft
static const float K_0_986 = FB(0x3f7c6a7f);
static const float K_EPS = FB(0x34000000);                // 2^-23
static const uint32_t B_ONE = 0x3f800000u, B_MINUS_ONE = 0xbf800000u, B_M100 = 0xc2c80000u, B_M1000 = 0xc47a0000u;

// ---- footprint helpers ----------------------------------------------------------------------------------------
struct mrModelInfo { int32_t num_verts; void* verts; };
// the car, its live models (damage rebuilds them) and its body volume with its spheres (phys_car.cpp's fp_car_all)
static void fp_car_all(Footprint& f, AICar* c) {
    f.object(c, "AICar");
    for (int i = 0; i < 5; i++) {
        uint8_t* mi = (uint8_t*)(uintptr_t)c->live_models[i];
        if (!mi) continue;
        f.add(mi + 0x10, 8, "model_info");
        mrModelInfo* info = *(mrModelInfo**)(mi + 0x10);
        if (info && info->verts && info->num_verts > 0) f.add(info->verts, (uint32_t)info->num_verts * 32, "model verts");
    }
    if (uint8_t* g = (uint8_t*)c->body_volume) {
        f.object(g, "body volume");
        const int32_t n = *(int32_t*)(g + 0x4c);
        for (int i = 0; i < n && i < 12; i++)
            if (void* s = ((void**)(g + 0x1c))[i]) f.object(s, "body sphere");
    }
}
static void fp_line(Footprint& f, AICar* c) {
    if (c->line) f.add(c->line, IDEAL_LINE_SIZE, "ideal line");
}
// would teleport_to_track fire now (and move the car through the deity's TeleportToLine)?
static bool teleport_would_fire(AICar* c) {
    const double d = D(c->now) - c->teleport_armed_time;
    const float df = (float)d;
    return d > D(K_TWO) && I(df) < 0x40a00000 && GetGameState() == 1;
}

// =============================================================================================================
// AICar::repl_handler (0x42e5c0, replay event 4): the AI debug event; its first dword must be 0 or 1. 0x2c bytes.
// =============================================================================================================
static int32_t __cdecl AICar_repl_handler(const void* ev, int32_t) {
    const int32_t kind = *(const int32_t*)ev;
    if (kind != 0 && kind != 1) LogPanic(S_WHAT_THE);
    return 0x2c;
}
static void fp_repl_handler(Footprint& f, const void* ev, int32_t) {
    const int32_t kind = *(const int32_t*)ev;
    if (kind != 0 && kind != 1) f.replay_only = "LogPanic";
}
PORT_FN(0x0042e5c0, "AICar::repl_handler", AICar_repl_handler, fp_repl_handler)

// =============================================================================================================
// AICar::stuff_event (0x42e5f0; nothing calls it): replay event 4 {kind, car, value, text}, and its handler. The
// text is strcpy'd into a 32-byte stack buffer, as the original's, and the bytes after its terminator are whatever
// the stack held (the event carries them). So a shorter text's event can't match the original's byte for byte --
// a 31-character one does.
// =============================================================================================================
static void __fastcall AICar_stuff_event(AICar* self, Edx, int32_t kind, int32_t value, const char* text) {
    AIEvent ev;
    ev.kind = kind;
    ev.value = value;
    ev.car = self->car_index;
    const size_t len = strlen(text);
    // FIX: the original's strcpy has no bound: a text of 32 characters or more ran off the event's 32 bytes and
    // over its stack frame. Cut to 31 characters (nothing calls it; bounded all the same).
    if (VP_FIX && len >= sizeof ev.text) {
        memcpy(ev.text, text, sizeof ev.text - 1);
        ev.text[sizeof ev.text - 1] = 0;
    } else
        memcpy(ev.text, text, len + 1);             // repne scasb; rep movsd; rep movsb
    PhysReplayAddEvent(4, &ev, 0x2c);
    call_repl_handler(&ev, 0);
}
static void fp_stuff_event(Footprint& f, AICar*, Edx, int32_t kind, int32_t, const char*) {
    if (kind != 0 && kind != 1) f.replay_only = "LogPanic";
}
PORT_FN(0x0042e5f0, "AICar::stuff_event", AICar_stuff_event, fp_stuff_event)

// ---- AICar::GetMessage (0x42e660, vtable +0x14): Car's ---------------------------------------------------------
static void __fastcall AICar_GetMessage(AICar* self, Edx, uint8_t* buf) { Car_GetMessage(self, 0, buf); }
static void fp_get_message(Footprint& f, AICar* self, Edx, uint8_t* buf) {
    f.add(buf + *(uint32_t*)((uint8_t*)self + 8), 0x18c, "message");    // at PhobRoot +8 (phys_car.cpp)
}
PORT_FN(0x0042e660, "AICar::GetMessage", AICar_GetMessage, fp_get_message)

// =============================================================================================================
// AICar::UpdateReplay (0x42e670, vtable +0x1c): Car's between the two packets; then the AI part of the first packet
// -- the target point (+0x3c) and the reasons (+0x44, a bitmask over msg_tab) -- back into the car
// =============================================================================================================
static void __fastcall AICar_UpdateReplay(AICar* self, Edx, const uint8_t* p0, const uint8_t* p1, uint32_t t_bits) {
    Car_UpdateReplay(self, 0, p0, p1, t_bits);
    memcpy(&self->target_world, p0 + 0x3c, 8);
    self->num_msgs = 0;
    uint32_t mask = *(const uint32_t*)(p0 + 0x44);
    for (const char* const* e = MSG_TAB_BEGIN; mask; e++) {
        if (e >= MSG_TAB_END) break;
        if (mask & 1) call_MSG(self, 0, *e);
        mask >>= 1;
    }
    self->latslam_rel_speed = 0.0f;
}
static void fp_update_replay(Footprint& f, AICar* self, Edx, const uint8_t*, const uint8_t*, uint32_t) { fp_car_all(f, self); }
PORT_FN(0x0042e670, "AICar::UpdateReplay", AICar_UpdateReplay, fp_update_replay)

// =============================================================================================================
// AICar::UpdateCommon (0x42e6f0, vtable +0xc): Car's, then show_msg (a bare ret)
// =============================================================================================================
static void __fastcall AICar_UpdateCommon(AICar* self, Edx) {
    Car_UpdateCommon(self, 0);
    call_show_msg(self, 0);
}
// Car::UpdateCommon's footprint (phys_car_update.cpp): the car, the sounds it drives (+4..+0x2d), the EngineSound
// and its samples' Sound3Ds, the wheels' tyre sounds, and the car's status string in the car manager's table
static void fp_update_common(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    if (uint8_t* es = (uint8_t*)self->sounds[1]) {
        f.add(es, 248, "engine sound");
        const int32_t n = *(int32_t*)(es + 0xf4);
        for (int i = 0; i < n && i < 7; i++)
            if (uint8_t* s = *(uint8_t**)(es + 0xd4 + 4 * i)) f.add(s + 4, 0x2a, "engine sample Sound3D");
        if (uint8_t* s = *(uint8_t**)(es + 0xf0)) f.add(s + 4, 0x2a, "engine idle Sound3D");
    }
    const int snd[6] = {0, 2, 3, 4, 5, 6};
    for (int i : snd)
        if (uint8_t* s = (uint8_t*)self->sounds[i]) f.add(s + 4, 0x2a, "Sound3D");
    for (int i = 0; i < 4; i++) {
        uint8_t* ts = (uint8_t*)self->wheels[i].tire_sound;
        if (!ts) continue;
        const uint32_t vt = *(const uint32_t*)ts;
        if (vt == 0x004dd7b0) {
            f.add(ts, 0x3c, "tire sound");
            if (uint8_t* s3d = *(uint8_t**)(ts + 0x34)) f.add(s3d + 4, 0x2a, "tire Sound3D");
        } else if (vt == 0x004dd7a0) f.add(ts, 0x10, "tire sound");
    }
    // the status string only: the char[256] at CarMgrInfo +0x32 (phys_car_update.cpp fp_update_common). The rest of
    // the 368-byte record is the main thread's (WorldUpdate -> CarMgrUpdateCar writes its CarInfo every frame)
    if (self->car_index >= 0) f.add((uint8_t*)0x005540c2 + 368 * self->car_index, 256, "status string");
}
PORT_FN(0x0042e6f0, "AICar::UpdateCommon", AICar_UpdateCommon, fp_update_common)

// =============================================================================================================
// AICar::MakeReplayPacket (0x42e710, vtable +0x20): Car's 0x3c bytes, the target point at +0x3c, and this think's
// _MSG reasons as a bitmask over msg_tab at +0x44 (a reason not in the table sets nothing)
// =============================================================================================================
static void __fastcall AICar_MakeReplayPacket(AICar* self, Edx, uint8_t* p) {
    Car_MakeReplayPacket(self, 0, p);
    memcpy(p + 0x3c, &self->target_world, 8);
    uint32_t* mask = (uint32_t*)(p + 0x44);
    *mask = 0;
    for (int32_t i = 0; self->num_msgs > i; i++) {
        const char* m = self->msgs[i];
        int32_t bit = 0;
        for (const char* const* e = MSG_TAB_BEGIN; e < MSG_TAB_END; e++, bit++)
            if (*e == m) { *mask |= 1u << (bit & 31); break; }
    }
}
static void fp_make_packet(Footprint& f, AICar* self, Edx, uint8_t* p) {
    f.object(self, "AICar");
    f.add(p, 0x48, "packet");
}
PORT_FN(0x0042e710, "AICar::MakeReplayPacket", AICar_MakeReplayPacket, fp_make_packet)

// =============================================================================================================
// AICar::localify_vector (0x42e790): a world x,z vector into the car's frame (rows 0 and 2 of the rotation)
// =============================================================================================================
static void __fastcall AICar_localify_vector(AICar* self, Edx, Point2D* v) {
    const float x = v->x;                           // (an integer copy)
    const float z = v->z;
    const float* m = self->frame.rot.m;
    const double nz = D(z) * m[8] + D(m[6]) * x;
    const double nx = D(m[0]) * x + D(m[2]) * z;
    v->x = (float)nx;
    v->z = (float)nz;
}
static void fp_localify_vector(Footprint& f, AICar*, Edx, Point2D* v) { f.add(v, 8, "vector"); }
PORT_FN(0x0042e790, "AICar::localify_vector", AICar_localify_vector, fp_localify_vector)

// =============================================================================================================
// AICar::localify_loc (0x42e7d0): a world x,z point, at the ground's height there, into the car's frame
// =============================================================================================================
static void __fastcall AICar_localify_loc(AICar* self, Edx, Point2D* p) {
    uint32_t xb, zb;
    memcpy(&xb, &p->x, 4);
    memcpy(&zb, &p->z, 4);
    const float y = TerrainGetHeightXZ(xb, zb);
    const float dx = (float)(D(FB(xb)) - self->frame.pos.x);
    const float dy = (float)(D(y) - self->frame.pos.y);
    const double dzr = D(FB(zb)) - self->frame.pos.z;
    const float dz = (float)dzr;                    // fst: the register goes on into the first product
    const float* m = self->frame.rot.m;
    const double nz = (dzr * m[8] + D(m[6]) * dx) + D(m[7]) * dy;
    const double nx = (D(m[1]) * dy + D(m[2]) * dz) + D(m[0]) * dx;
    p->x = (float)nx;
    p->z = (float)nz;
}
static void fp_localify_loc(Footprint& f, AICar*, Edx, Point2D* p) { f.add(p, 8, "point"); }
PORT_FN(0x0042e7d0, "AICar::localify_loc", AICar_localify_loc, fp_localify_loc)

// =============================================================================================================
// AICar::get_ilpos (0x42e860): the car's bead on its line (IdealLine::get_actual_bead_position), {0, 0} without a
// line. Returned through the caller's buffer (a member function's struct return).
// =============================================================================================================
static ILinePos* __fastcall AICar_get_ilpos(AICar* self, Edx, ILinePos* ret) {
    IdealLine* line = self->line;
    if (line) {
        IdealLine_get_actual_bead_position(line, 0, ret);
        return ret;
    }
    memset(ret, 0, 8);
    return ret;
}
static void fp_get_ilpos(Footprint& f, AICar*, Edx, ILinePos* ret) { f.add(ret, 8, "ILinePos"); }
PORT_FN(0x0042e860, "AICar::get_ilpos", AICar_get_ilpos, fp_get_ilpos)

// =============================================================================================================
// AICar::ResolveExternalImpulse (0x42e8a0, vtable +0x38): Car's; |J| over 5000 x skill+0x40 stamps crash_time, over
// 1500 x skill+0x3c bump_time. A car contact (0x6d) holds the notes' learning off; anything else but 0 is a wall:
// the bead segment's hit_wall note, and where across the road it happened.
// =============================================================================================================
static void __fastcall AICar_ResolveExternalImpulse(AICar* self, Edx, const P3* j, const P3* point, int32_t type) {
    Car_ResolveExternalImpulse(self, 0, j, point, type);
    const float jsq = (float)((D(j->x) * j->x + D(j->y) * j->y) + D(j->z) * j->z);
    const uint8_t* sk = self->skill;
    if (!((D(skf(sk, 0x40)) * skf(sk, 0x40)) * K_2_5E7 >= jsq)) COPY4(self->crash_time, self->now);   // fcomp; test ah,1
    if (!((D(skf(sk, 0x3c)) * skf(sk, 0x3c)) * K_2_25E6 >= jsq)) COPY4(self->bump_time, self->now);
    if (type == 0x6d) {
        call_suspend_adjustment(self, 0, S_CAR_IMPULSE);
        return;
    }
    if (type == 0) return;
    self->seginfo[self->seg_index].notes.hit_wall = 1;
    COPY4(self->wall_hit_lat, self->center_lat);
}
static void fp_resolve_impulse(Footprint& f, AICar* self, Edx, const P3*, const P3*, int32_t type) {
    fp_car_all(f, self);                             // Car's: ApplyDamage -> ActuallyApplyDamage (the models)
    if (type != 0x6d && type != 0) f.add(&self->seginfo[self->seg_index], 1, "seginfo hit_wall");
}
PORT_FN(0x0042e8a0, "AICar::ResolveExternalImpulse", AICar_ResolveExternalImpulse, fp_resolve_impulse)

// =============================================================================================================
// AICar::ApplyExternalForce (0x42e970, vtable +0x34): Car's; then the force in the car's frame (x across, z along,
// both negated). A car contact (0x6d) pushing more sideways than lengthways nudges the slam steering 0.005 away;
// a wall (0x6e) heats wall_heat by 1 and cancels slam steering that points the way the wall pushes.
// =============================================================================================================
static void __fastcall AICar_ApplyExternalForce(AICar* self, Edx, const P3* force, const P3* point, int32_t type) {
    Car_ApplyExternalForce(self, 0, force, point, type);
    const float* m = self->frame.rot.m;
    const float lat = (float)-((D(m[0]) * force->x + D(m[1]) * force->y) + D(m[2]) * force->z);
    const float lon = (float)-((D(m[8]) * force->z + D(m[6]) * force->x) + D(m[7]) * force->y);
    if (type == 0x6d) {
        if (!(fabs(D(lon)) >= fabs(D(lat)))) {       // fcompp; test ah,1
            if (I(lat) > 0) self->slam_steer = (float)(D(self->slam_steer) - K_0_005);
            else self->slam_steer = (float)(D(self->slam_steer) + K_0_005);
        }
    } else if (type == 0x6e) {
        self->wall_heat = (float)(D(self->wall_heat) + K_ONE);
        if (!((U(self->slam_steer) ^ U(lat)) & 0x80000000u)) self->slam_steer = 0.0f;
    }
}
static void fp_apply_force(Footprint& f, AICar* self, Edx, const P3*, const P3*, int32_t) { f.object(self, "AICar"); }
PORT_FN(0x0042e970, "AICar::ApplyExternalForce", AICar_ApplyExternalForce, fp_apply_force)

// ---- AICar::Reset (0x42ea60, vtable +4): Car's, then reset -------------------------------------------------------
static void __fastcall AICar_Reset(AICar* self, Edx) {
    Car_Reset(self, 0);
    call_reset(self, 0);
}
static void fp_reset_private(Footprint& f, AICar* self, Edx);
static void fp_Reset(Footprint& f, AICar* self, Edx) {
    fp_car_all(f, self);
    fp_reset_private(f, self, 0);
}
PORT_FN(0x0042ea60, "AICar::Reset", AICar_Reset, fp_Reset)

// ---- AICar::fast_mode / slow_mode (0x42ea80 / 0x42eaa0): the drive and interact routines (slow_mode is never used)
static void __fastcall AICar_fast_mode(AICar* self, Edx, const char*) {
    SETB(self->drive_fn, FAST_DRIVE);
    SETB(self->interact_fn, FAST_INTERACT);
}
static void __fastcall AICar_slow_mode(AICar* self, Edx, const char*) {
    SETB(self->drive_fn, SLOW_DRIVE);
    SETB(self->interact_fn, SLOW_INTERACT);
}
static void fp_mode(Footprint& f, AICar* self, Edx, const char*) { f.object(self, "AICar"); }
PORT_FN(0x0042ea80, "AICar::fast_mode", AICar_fast_mode, fp_mode)
PORT_FN(0x0042eaa0, "AICar::slow_mode", AICar_slow_mode, fp_mode)

// =============================================================================================================
// AICar::AICar (0x42eac0): LocalCar's constructor; the replay handler (first instance); the skill record (a
// driver meant for another car is logged); the per-track steering gain and traction-control flag; registered
// with the ai library; the racing line and its per-segment info (none without a line: line NULL); the proximity
// table; yaw control, traction control and ABS on, the automatic clutch; reset.
// =============================================================================================================
static AICar* __fastcall AICar_ctor(AICar* self, Edx, void* data, void* p) {
    LocalCar_ctor(self, 0, data, p);
    self->vtable = VT_AICar;
    const int32_t count = g_instance_count;
    g_instance_count = count + 1;
    if (count == 0) PhysReplayInstallEventHandler(4, REPL_HANDLER);
    memset(&self->seginfo, 0, 28);                  // seginfo .. wall_hit_lat
    self->realism = 2;
    self->skill = AIGetSkill(self->car_index);
    if (game_stricmp((const char*)WorldGetCarEntry(self->car_index) + 0x11, (const char*)self->skill + 0x70) != 0) {
        const int32_t ci = self->car_index;
        const char* want = (const char*)self->skill + 0x70;
        LogReport(S_INAPPROPRIATE, ci, (const char*)WorldGetCarEntry(ci) + 0x11, want);
    }
    {
        const int32_t track = AIGetTrackNumber();
        COPY4(self->steer_gain, skf(self->skill, 0xa0 + 0x18 * (uint32_t)track));
    }
    self->tc_always = AIGetTrackInfo(AIGetTrackNumber())[8];
    AIRegisterAICar(self, self->car_index);
    IdealLine* line = AIGetLine(self->car_index);
    self->line = line;
    if (line->head) {
        const int32_t n = line->num_segs;
        self->num_segs = n;
        self->seginfo = (SegmentInfo*)MemAlloc((int32_t)((uint32_t)n * 24u));
        call_init_seginfo(self, 0);
    } else self->line = 0;
    const int32_t np = AICarCount();
    self->num_proxer_info = np;
    uint8_t* pp = (uint8_t*)MemAlloc((int32_t)((uint32_t)np * 8u));
    if (pp) {
        uint8_t* q = pp;
        for (int32_t k = (int32_t)((uint32_t)np - 1u); k >= 0; k--) {
            ProxerInfo_ctor(q, 0);
            q += 8;
        }
        self->proxer_info = (ProxerInfo*)pp;
    } else self->proxer_info = 0;
    call_set_yaw_control(self, 0, 1);
    call_set_traction_control(self, 0, 1);
    call_set_abs(self, 0, 1);
    self->auto_clutch = 1;
    call_reset(self, 0);
    return self;
}
static void fp_ctor(Footprint& f, AICar*, Edx, void*, void*) {
    f.replay_only = "constructs a LocalCar, allocates the segment info and the proximity table, registers with the AI";
}
PORT_FN(0x0042eac0, "AICar::AICar", AICar_ctor, fp_ctor)

// =============================================================================================================
// AICar::reset (0x42ec80): the launch time (the track's base + per-car x the grid slot), the lap clock, the lap-time
// schedule (the per-track base lap time, or the line's own when that's negative, and the pace between them), every
// timer back (-1000: long ago), the line info and the bead re-found, the proximity levels cleared, fast mode.
// Reads the last segment's info unguarded: a car without a line has no seginfo (NULL), as in the original.
// =============================================================================================================
static void __fastcall AICar_reset(AICar* self, Edx) {
    SETB(self->teleport_armed_time, B_M100);
    float per_car;
    {
        const uint8_t* ti = AIGetTrackInfo(AIGetTrackNumber());
        per_car = (float)(D(skf(ti, 4)) * (double)self->car_index);        // fimul
    }
    const uint8_t* ti = AIGetTrackInfo(AIGetTrackNumber());
    const double launch = D(skf(ti, 0)) + per_car;
    self->offground_cut = 0.0f;
    self->last_learn_lap = 0;
    self->launch_time = (float)launch;
    const float now = PhysicsGetTime();
    const uint32_t clock = (uint32_t)self->car_index * 0x74u + (uint32_t)(uintptr_t)g_race_deity_global + 0x78u;
    self->too_fast = 0.0f;
    self->lap_clock = (float*)(uintptr_t)clock;
    self->schedule_error = 0.0f;
    self->lap_time_offset = 0.0f;
    self->lap_target_time = 0.0f;
    SETB(self->throttle_cap, B_ONE);
    {
        const int32_t track = AIGetTrackNumber();
        COPY4(self->base_lap_time, skf(self->skill, 0x98 + 0x18 * (uint32_t)track));
    }
    {
        const int32_t track = AIGetTrackNumber();
        COPY4(self->lap_time_spread, skf(self->skill, 0xa4 + 0x18 * (uint32_t)track));
    }
    const ILSeg* last = self->seginfo[self->num_segs - 1].seg;
    const ILSeg* first = last->next;
    const bool use_line = U(self->base_lap_time) > 0x80000000u;      // negative (cmp; ja)
    self->line_lap_time = (float)(D(last->step) / ((D(first->target_speed) + last->target_speed) * K_HALF) + last->cum_time);
    if (use_line) COPY4(self->base_lap_time, self->line_lap_time);
    const double pace = D(self->base_lap_time) / self->line_lap_time;
    self->wall_heat = 0.0f;
    self->pass_offset = 0.0f;
    SETB(self->freeze_time, B_M1000);
    SETB(self->crash_time, B_M1000);
    COPY4(self->progress_time, now);
    SETB(self->fouroff_time, B_M1000);
    self->finish_time = 0.0f;
    SETB(self->bump_time, B_M1000);
    self->race_begun = 0;
    self->learn_hiatus = 0;
    self->steer_damping = 0.0f;
    self->seg_valid = 0;
    SETB(self->line_update_time, B_M100);
    self->steer = 0.0f;
    self->slam_steer = 0.0f;
    self->lap = 0;
    self->pace = (float)pace;
    call_update_line_info(self, 0);
    IdealLine_reset_bead_position(self->line, 0);
    for (int32_t i = 0; self->num_proxer_info > i; i++) self->proxer_info[i].level = 0.0f;
    call_fast_mode(self, 0, S_RESET);
}
// the car, its line (update_car_info, reset_bead_position) and the proximity levels
static void fp_reset_private(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    fp_line(f, self);
    if (self->proxer_info && self->num_proxer_info > 0) f.add(self->proxer_info, (uint32_t)self->num_proxer_info * 8, "proxer info");
}
PORT_FN(0x0042ec80, "AICar::reset(private)", AICar_reset, fp_reset_private)   // (the ini's keys ignore case: AICar::Reset)

// =============================================================================================================
// AICar::~AICar (0x42eea0): the replay handler (last instance), unregistered, the proximity table freed; in learn
// mode the notes (speed, lateral per segment) are handed to aicar_record_notes in a new buffer; the segment info
// freed; LocalCar's destructor
// =============================================================================================================
static void __fastcall AICar_dtor(AICar* self, Edx) {
    self->vtable = VT_AICar;
    if (--g_instance_count == 0) PhysReplayUninstallEventHandler(REPL_HANDLER);
    AIUnregisterCar(self, self->car_index);
    operator_delete(self->proxer_info);
    if (self->num_segs != 0) {
        if (AILearnMode()) {
            const int32_t bytes = (int32_t)((uint32_t)self->num_segs << 3);
            uint32_t* buf = (uint32_t*)MemAlloc(bytes);
            for (int32_t i = 0; self->num_segs > i; i++) {
                const uint8_t* si = (const uint8_t*)self->seginfo + 24 * i;
                memcpy(&buf[2 * i], si + 4, 4);
                memcpy(&buf[2 * i + 1], si + 8, 4);
            }
            aicar_record_notes(buf, bytes);
        }
        operator_delete(self->seginfo);
        self->seginfo = 0;
    }
    LocalCar_dtor(self, 0);
}
static void fp_dtor(Footprint& f, AICar*, Edx) { f.replay_only = "frees the tables (and in learn mode allocates the notes)"; }
PORT_FN(0x0042eea0, "AICar::~AICar", AICar_dtor, fp_dtor)

// ---- AICar::suspend_adjustment (0x42ef80): the notes aren't adjusted for at least 7 more segments -----------------
static void __fastcall AICar_suspend_adjustment(AICar* self, Edx, const char*) {
    int32_t h = self->learn_hiatus;
    if (h <= 7) h = 7;
    self->learn_hiatus = h;
}
PORT_FN(0x0042ef80, "AICar::suspend_adjustment", AICar_suspend_adjustment, fp_mode)

// ---- AICar::fire_fouroff (0x42efa0): the recovery starts now, toward the side of the road the car is on ------------
static void __fastcall AICar_fire_fouroff(AICar* self, Edx) {
    const int32_t side = I(self->center_lat);
    COPY4(self->fouroff_time, self->now);
    SETB(self->recover_side, side > 0 ? B_ONE : B_MINUS_ONE);
}
static void fp_self(Footprint& f, AICar* self, Edx) { f.object(self, "AICar"); }
static void fp_self_f(Footprint& f, AICar* self, Edx, float) { f.object(self, "AICar"); }
PORT_FN(0x0042efa0, "AICar::fire_fouroff", AICar_fire_fouroff, fp_self)

// ---- AICar::GetRelSegment (0x42efd0 / 0x42f000): the segment info a + b (or s + off) around the loop ---------------
static SegmentInfo* __fastcall AICar_GetRelSegment_ii(AICar* self, Edx, int32_t a, int32_t b) {
    const int32_t n = self->num_segs;
    const int32_t i = (int32_t)((uint32_t)a + (uint32_t)b + (uint32_t)n) % n;          // cdq; idiv
    return (SegmentInfo*)(uintptr_t)((uint32_t)(uintptr_t)self->seginfo + (uint32_t)i * 24u);
}
static void fp_rel_ii(Footprint&, AICar*, Edx, int32_t, int32_t) {}
PORT_FN(0x0042efd0, "AICar::GetRelSegment(int,int)", AICar_GetRelSegment_ii, fp_rel_ii)

static SegmentInfo* __fastcall AICar_GetRelSegment_si(AICar* self, Edx, const SegmentInfo* s, int32_t off) {
    const uint32_t base = (uint32_t)(uintptr_t)self->seginfo;
    const int32_t n = self->num_segs;
    const int32_t at = (int32_t)((uint32_t)(uintptr_t)s - base) / 24;                  // cdq; idiv 0x18
    const int32_t i = (int32_t)((uint32_t)at + (uint32_t)off + (uint32_t)n) % n;
    return (SegmentInfo*)(uintptr_t)(base + (uint32_t)i * 24u);
}
static void fp_rel_si(Footprint&, AICar*, Edx, const SegmentInfo*, int32_t) {}
PORT_FN(0x0042f000, "AICar::GetRelSegment(seg,int)", AICar_GetRelSegment_si, fp_rel_si)

// =============================================================================================================
// AICar::AISetThrottle (0x42f030): x skill+0x1c, capped at throttle_cap (negative: 0); none in water; lifted by a
// growing offground_cut while a rear wheel is off the ground; 0..1 to Car::SetThrottle
// =============================================================================================================
static void __fastcall AICar_AISetThrottle(AICar* self, Edx, float throttle) {
    const uint32_t cap = U(self->throttle_cap);
    const float t = (float)(D(skf(self->skill, 0x1c)) * throttle);
    float v;
    if (U(t) > 0x80000000u) v = 0.0f;
    else if (!(D(FB(cap)) >= t)) SETB(v, cap);       // fcomp; test ah,1
    else COPY4(v, t);
    if (call_count_wheels(PRED_ON_WATER, self->wheels) != 0) {
        call_MSG(self, 0, msg_tab(MSG_WATER_CUT));
        v = 0.0f;
    }
    if (call_off_ground(&self->wheels[2]) || call_off_ground(&self->wheels[3])) {
        call_MSG(self, 0, msg_tab(MSG_OFFGROUND_CUT));
        v = (float)(D(v) - self->offground_cut);
        self->offground_cut = (float)(D(self->offground_cut) + K_0_03);
    } else self->offground_cut = 0.0f;
    uint32_t r;
    if (U(v) > 0x80000000u) r = 0;
    else if (I(v) > 0x3f800000) r = B_ONE;
    else r = U(v);
    Car_SetThrottle(self, 0, r);
}
PORT_FN(0x0042f030, "AICar::AISetThrottle", AICar_AISetThrottle, fp_self_f)

// ---- the wheel predicates count_wheels is handed, and count_wheels itself -----------------------------------------
static uint8_t __cdecl on_water(Wheel* w) { return w->surface == 14; }
static uint8_t __cdecl off_ground(Wheel* w) { return w->on_ground == 0; }
static uint8_t __cdecl damaged(Wheel* w) { return w->broken; }
static void fp_wheel_pred(Footprint& f, Wheel*) { f.pure = true; }
PORT_FN(0x0042f160, "on_water", on_water, fp_wheel_pred)
PORT_FN(0x0042f180, "off_ground", off_ground, fp_wheel_pred)
PORT_FN(0x0042f920, "damaged", damaged, fp_wheel_pred)

// (the predicate as a plain pointer: test/fuzz.h builds arguments for every rewrite's parameter types)
static int32_t __cdecl count_wheels(void* pred_fn, Wheel* wheels) {
    const WheelPred_t pred = (WheelPred_t)pred_fn;
    int32_t n = 0;
    Wheel* w = wheels;
    for (int i = 4; i; i--) {
        if (pred(w)) n++;
        w = (Wheel*)((uint8_t*)w + 0x1a4);
    }
    return n;
}
static void fp_count_wheels(Footprint&, void*, Wheel*) {}     // the three predicates write nothing
PORT_FN(0x0042f190, "count_wheels", count_wheels, fp_count_wheels)

// =============================================================================================================
// AICar::AISetSteering (0x42f1c0): the change slew-limited to 2 / (skill+0x28 x 31.25) a tick and softened by
// too_fast; the damping grows 0.12 (below 0.7) when the command crosses the wheels' side, and scales it; clamped to
// +-1 on to Car +0x500 and the front wheels' steer input (x max_steer_angle)
// =============================================================================================================
static void __fastcall AICar_AISetSteering(AICar* self, Edx, float s) {
    const float max_step = (float)(D(K_TWO) / (D(skf(self->skill, 0x28)) * K_31_25));
    float d = (float)(D(s) - self->steer);
    const double neg = -D(max_step);
    const float negf = (float)neg;
    if (neg > D(d)) d = negf;                        // fcom; test ah,0x41; jne
    else if (D(d) > D(max_step)) d = max_step;
    const bool damping_low = I(self->steer_damping) < 0x3f333333;     // cmp; jge (0.7)
    self->steer = (float)((D(K_ONE) - D(self->too_fast) * K_0_9) * d + self->steer);
    if (damping_low && ((U(self->steer) ^ U(self->steering)) & 0x80000000u))
        self->steer_damping = (float)(D(self->steer_damping) + K_0_12);
    self->steer = (float)((D(K_ONE) - self->steer_damping) * self->steer);
    if (fabs(D(self->steer)) > D(K_ONE)) {
        const bool pos = I(self->steer) > 0;
        SETB(self->steer, pos ? B_ONE : B_MINUS_ONE);
    }
    const double a = D(self->steer) * self->max_steer_angle;
    COPY4(self->steering, self->steer);
    self->wheels[0].steer_input = (float)a;
    self->wheels[1].steer_input = (float)a;
}
PORT_FN(0x0042f1c0, "AICar::AISetSteering", AICar_AISetSteering, fp_self_f)

// =============================================================================================================
// AICar::AISetBrake (0x42f310): 0..1; full brakes rolling backwards at 0.44..8.9 m/s ('panic_brake'), none unless
// all four wheels are down; easing off (the command below the current braking) blends toward the current by
// max(skill+8, 0.8, too_fast)
// =============================================================================================================
static void __fastcall AICar_AISetBrake(AICar* self, Edx, uint32_t b) {
    uint32_t v = b;
    if (b > 0x80000000u) v = 0;
    else if ((int32_t)b > 0x3f800000) v = B_ONE;
    float arg = FB(v);
    const float* m = self->frame.rot.m;
    const P3& vel = self->velocity;
    const double fwd = (D(vel.y) * m[7] + D(m[8]) * vel.z) + D(m[6]) * vel.x;
    bool reversing = false;
    if (!(fwd >= D(K_ZERO)) && I(self->speed) > 0x3ee38e39 && I(self->speed) < 0x410e38e4) reversing = true;
    if (reversing) {
        SETB(arg, B_ONE);
        call_MSG(self, 0, msg_tab(MSG_PANIC_BRAKE));
    } else {
        const int n = self->wheels[1].on_ground + self->wheels[3].on_ground + self->wheels[0].on_ground + self->wheels[2].on_ground;
        if (n != 4) arg = 0.0f;
    }
    const float current = self->braking;
    if (D(current) > D(arg)) {                        // fcomp; test ah,0x41; jne
        const double s8 = skf(self->skill, 8);
        const double a = !(D(K_0_8) >= s8) ? s8 : D(K_0_8);
        const double tf = self->too_fast;
        const double w = !(tf >= a) ? a : tf;
        const float wf = (float)w;
        arg = (float)((D(K_ONE) - w) * arg + D(wf) * current);
    }
    Car_SetBraking(self, 0, U(arg));
}
static void fp_brake(Footprint& f, AICar* self, Edx, uint32_t) { f.object(self, "AICar"); }
PORT_FN(0x0042f310, "AICar::AISetBrake", AICar_AISetBrake, fp_brake)

// ---- AICar::set_yaw_control / set_traction_control / set_abs (0x42f460 / 0x42f4a0 / 0x42f4e0) -----------------------
// yaw control only from 10 s after the start; traction control below 33.3 m/s (or anywhere on a tc_always track)
static void __fastcall AICar_set_yaw_control(AICar* self, Edx, uint8_t on) {
    if (!((D(self->now) - self->race_start_time) >= D(K_10))) self->yaw_control = 0;   // fcomp; test ah,1
    else self->yaw_control = on ? 1 : 0;
}
static void __fastcall AICar_set_traction_control(AICar* self, Edx, uint8_t on) {
    const bool slow = I(self->speed) < 0x42055555;
    const uint8_t always = self->tc_always;
    if (on && (slow || always)) Car_SetTractionControl(self, 0, 1);
    else Car_SetTractionControl(self, 0, 0);
}
static void __fastcall AICar_set_abs(AICar* self, Edx, uint8_t on) { Car_SetABSBraking(self, 0, on ? 2 : 0); }
static void fp_set_byte(Footprint& f, AICar* self, Edx, uint8_t) { f.object(self, "AICar"); }
PORT_FN(0x0042f460, "AICar::set_yaw_control", AICar_set_yaw_control, fp_set_byte)
PORT_FN(0x0042f4a0, "AICar::set_traction_control", AICar_set_traction_control, fp_set_byte)
PORT_FN(0x0042f4e0, "AICar::set_abs", AICar_set_abs, fp_set_byte)

// ---- AICar::SegmentInfo::Notes::clear (0x42f500): the three flags (a word and a byte; the speed note stays) ---------
static void __fastcall Notes_clear(Notes* self, Edx) {
    const uint16_t zero = 0;
    memcpy(self, &zero, 2);
    self->off_road_b = 0;
}
static void fp_notes_clear(Footprint& f, Notes* self, Edx) { f.add(self, 3, "notes"); f.pure = true; }
PORT_FN(0x0042f500, "AICar::SegmentInfo::Notes::clear", Notes_clear, fp_notes_clear)

// ---- AICar::_MSG (0x42f510): a reason for this think, each once, at most 10 ------------------------------------------
static void __fastcall AICar_MSG(AICar* self, Edx, const char* m) {
    const int32_t n = self->num_msgs;
    if ((uint32_t)n >= 10u) return;
    for (int32_t i = 0; n > i; i++)
        if (self->msgs[i] == m) return;
    self->msgs[n] = m;
    self->num_msgs++;
}
PORT_FN(0x0042f510, "AICar::_MSG", AICar_MSG, fp_mode)

// =============================================================================================================
// AICar::teleport_to_track (0x42f560): armed by a call (teleport_armed_time = now); a call 2..5 s later fires (and
// disarms: -100): in a race (game state 1) the deity's TeleportToLine puts the car on the line, whose bead is then
// re-found from the car's new position (update_car_info with a zero direction, reset_bead_position). 1 if moved.
// =============================================================================================================
static uint8_t __fastcall AICar_teleport_to_track(AICar* self, Edx) {
    const double d = D(self->now) - self->teleport_armed_time;
    const float df = (float)d;
    bool fire = false;
    if (d > D(K_TWO)) {                              // fcom; test ah,0x41; jne
        if (I(df) < 0x40a00000) {                    // < 5.0
            SETB(self->teleport_armed_time, B_M100);
            fire = true;
        } else COPY4(self->teleport_armed_time, self->now);
    }
    if (!fire || GetGameState() != 1) return 0;
    void* deity = g_deity;
    if (!VFN(deity, 0x3c, uint8_t, int32_t, AICar*)(deity, 0, self->car_index, self)) return 0;   // TeleportToLine
    Point2D dir, pos;
    COPY4(pos.x, self->frame.pos.x);
    COPY4(pos.z, self->frame.pos.z);
    SETB(dir.x, 0);
    SETB(dir.z, 0);
    IdealLine_update_car_info(self->line, 0, &pos, &dir);
    IdealLine_reset_bead_position(self->line, 0);
    return 1;
}
static void fp_teleport(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    fp_line(f, self);
    if (teleport_would_fire(self)) f.replay_only = "teleports the car through the deity (TeleportToLine)";
}
PORT_FN(0x0042f560, "AICar::teleport_to_track", AICar_teleport_to_track, fp_teleport)

// =============================================================================================================
// AICar::check_for_stranded (0x42f620, every 64 ticks): progress is the bead changing segment with no wheel in water
// or off the ground and the car upright; upside down or in water the clock runs a second faster; skill+0 + 6 s with
// no progress: teleport_to_track (progress stamped if it moved, else 2 s more grace)
// =============================================================================================================
static void __fastcall AICar_check_for_stranded(AICar* self, Edx) {
    if (!self->line) return;
    const uint32_t now = U(self->now);               // (an integer copy)
    ILinePos ilp;
    call_get_ilpos(self, 0, &ilp);
    if (self->progress_seg != ilp.seg && call_count_wheels(PRED_ON_WATER, self->wheels) == 0 &&
        call_count_wheels(PRED_OFF_GROUND, self->wheels) == 0 && U(self->frame.rot.m[4]) <= 0x80000000u) {
        self->progress_seg = ilp.seg;
        SETB(self->progress_time, now);
        return;
    }
    if (call_count_wheels(PRED_ON_WATER, self->wheels) != 0 || U(self->frame.rot.m[4]) > 0x80000000u)
        self->progress_time = (float)(D(self->progress_time) - K_ONE);
    const double limit = D(skf(self->skill, 0)) + K_6;
    if ((D(FB(now)) - self->progress_time) > limit) {   // fcompp; test ah,0x41; jne
        if (call_teleport_to_track(self, 0)) {
            SETB(self->progress_time, now);
            return;
        }
        self->progress_time = (float)(D(self->progress_time) + K_TWO);
    }
}
// the car, its line; replay-only when it would reach a teleport that fires (the same decisions, read-only)
static void fp_stranded(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    fp_line(f, self);
    if (!self->line) return;
    ILinePos ilp;
    call_get_ilpos(self, 0, &ilp);
    if (self->progress_seg != ilp.seg && call_count_wheels(PRED_ON_WATER, self->wheels) == 0 &&
        call_count_wheels(PRED_OFF_GROUND, self->wheels) == 0 && U(self->frame.rot.m[4]) <= 0x80000000u) return;
    float pt = self->progress_time;
    if (call_count_wheels(PRED_ON_WATER, self->wheels) != 0 || U(self->frame.rot.m[4]) > 0x80000000u) pt = (float)(D(pt) - K_ONE);
    if ((D(self->now) - pt) > D(skf(self->skill, 0)) + K_6 && teleport_would_fire(self))
        f.replay_only = "teleports the car through the deity (TeleportToLine)";
}
PORT_FN(0x0042f620, "AICar::check_for_stranded", AICar_check_for_stranded, fp_stranded)

// =============================================================================================================
// AICar::check_for_too_fast (0x42f730, every 16 ticks): above 22.2 m/s, racing, after lap 0: the schedule error (the
// lap clock against the line's cumulative time at the bead x pace); ahead of the schedule, away from the lap's first
// and last 3 segments, too_fast = 5 x (the share of the remaining time it's ahead, plus the error's growth), 0..1,
// and the throttle cap from it (1 .. 0.6). The original reads the bead segment unguarded: NULL there is the known AI
// crash (fixed below).
// =============================================================================================================
static void __fastcall AICar_check_for_too_fast(AICar* self, Edx) {
    if (I(self->speed) <= 0x41b1c71c || self->finished || self->lap == 0) {
        self->too_fast = 0.0f;
        return;
    }
    ILinePos ilp;
    call_get_ilpos(self, 0, &ilp);
    // FIX: a lost (NULL) bead -- a stranded car's teleport, a NaN position -- was read (seg->next) and crashed: the
    // line puts it back at the point nearest the car (reset_bead_position) and it's read again; with no bead even
    // then, as the early out: not too fast. (A car with no line at all -- a track without one -- is left as the
    // original has it.)
    if (VP_FIX && !ilp.seg && self->line) {
        IdealLine_reset_bead_position(self->line, 0);
        call_get_ilpos(self, 0, &ilp);
        if (!ilp.seg) {
            self->too_fast = 0.0f;
            return;
        }
    }
    const double old_error = self->schedule_error;
    const float* clock = self->lap_clock;
    const ILSeg* seg = ilp.seg;
    const ILSeg* next = seg->next;                   // the crash, with the bead segment NULL
    const double error = D(*clock) - ((D(next->cum_time) - seg->cum_time) * ilp.t + seg->cum_time) * self->pace;
    const double remaining = D(self->lap_target_time) - *clock;
    self->schedule_error = (float)error;
    const float x = (float)(-(error / remaining) - (error - old_error));
    float* tf = &self->too_fast;
    if (!(remaining >= D(K_ZERO))) *tf = 0.0f;      // fcomp; test ah,1
    else {
        const int32_t at = (int32_t)((uint32_t)(uintptr_t)seg - (uint32_t)(uintptr_t)self->seginfo[0].seg) / 0x44;
        if (at < 3 || (int32_t)((uint32_t)self->num_segs - 3u) < at) *tf = 0.0f;
        else {
            const double v = D(x) * K_5;
            float c = (float)v;
            if (!(v >= D(K_ZERO))) c = 0.0f;
            else if (I(c) > 0x3f800000) SETB(c, B_ONE);
            *tf = c;
        }
    }
    if (U(*tf) > 0x80000000u) SETB(self->throttle_cap, B_ONE);
    else if (I(*tf) > 0x3f800000) SETB(self->throttle_cap, 0x3f19999au);
    else self->throttle_cap = (float)((D(K_ONE) - *tf) + D(*tf) * K_0_6);
}
// the car; the line (a lost bead is found again: reset_bead_position)
static void fp_too_fast(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    fp_line(f, self);
}
PORT_FN(0x0042f730, "AICar::check_for_too_fast", AICar_check_for_too_fast, fp_too_fast)

// =============================================================================================================
// AICar::check_for_damage (0x42f8b0, every 64 ticks): a broken wheel freezes the driving (brake on, throttle and
// steering off); crawling below 2.2 m/s it asks for a teleport, the freeze backdated 2 s
// =============================================================================================================
static void __fastcall AICar_check_for_damage(AICar* self, Edx) {
    if (call_count_wheels(PRED_DAMAGED, self->wheels) == 0) return;
    COPY4(self->freeze_time, self->now);
    Car_SetBraking(self, 0, B_ONE);
    Car_SetThrottle(self, 0, 0);
    Car_SetSteering(self, 0, 0);
    if (I(self->speed) < 0x400e38e4) {
        call_teleport_to_track(self, 0);
        self->freeze_time = (float)(D(self->now) - K_TWO);
    }
}
static void fp_damage(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    fp_line(f, self);
    if (call_count_wheels(PRED_DAMAGED, self->wheels) && I(self->speed) < 0x400e38e4 && teleport_would_fire(self))
        f.replay_only = "teleports the car through the deity (TeleportToLine)";
}
PORT_FN(0x0042f8b0, "AICar::check_for_damage", AICar_check_for_damage, fp_damage)

// =============================================================================================================
// AICar::init_seginfo (0x42f930): one SegmentInfo per line segment, from the head: the flags cleared, the speed and
// lateral notes from the driver's .dnt data (none in learn mode or a replay), unlocked, the lookahead from the
// segment's curvature
// =============================================================================================================
static void __fastcall AICar_init_seginfo(AICar* self, Edx) {
    const ILSeg* s = self->line->head;
    const uint8_t* data = 0;
    SegmentInfo* si = self->seginfo;
    if (!AILearnMode() && !PhysReplayPlayMode()) data = GetDriverData(self->car_index);
    const uint8_t* d = data;
    for (int32_t i = 0; self->num_segs > i; i++) {
        call_Notes_clear(&si->notes, 0);
        if (data) {
            memcpy(&si->notes.speed_note, d, 4);
            memcpy(&si->lateral_note, d + 4, 4);
        } else {
            si->notes.speed_note = 0.0f;
            si->lateral_note = 0.0f;
        }
        d += 8;
        si->locked = 0;
        si->lookahead = (float)call_lookahead_from_curvature(U(s->curvature));
        si->seg = (ILSeg*)s;
        s = s->next;
        si++;
    }
    if (data) ReleaseDriverData(data);
}
static void fp_init_seginfo(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    if (self->seginfo && self->num_segs > 0) f.add(self->seginfo, (uint32_t)self->num_segs * 24, "seginfo");
    if (!AILearnMode() && !PhysReplayPlayMode()) f.replay_only = "GetDriverData loads the driver's notes";
}
PORT_FN(0x0042f930, "AICar::init_seginfo", AICar_init_seginfo, fp_init_seginfo)

// ---- lookahead_from_curvature (0x42fa00): 0.3 .. 0.7 s, shorter as the curvature grows to 0.05 --------------------------
static double __cdecl lookahead_from_curvature(float curvature) {
    const double v = D(K_ONE) - D(curvature) * K_20;
    float c = (float)v;
    if (!(v >= D(K_ZERO))) c = 0.0f;                  // fcom; test ah,1
    else if (I(c) > 0x3f800000) SETB(c, B_ONE);
    return D(c) * K_0_4m + K_0_3;
}
static void fp_lookahead(Footprint& f, float) { f.pure = true; }
PORT_FN(0x0042fa00, "lookahead_from_curvature", lookahead_from_curvature, fp_lookahead)

// =============================================================================================================
// AICar::init_rtinfo (0x42fa60): each think's inputs -- speed, time, finished, the lateral info (init_rt_lat), the
// race fraction (the deity's lap / laps), the lateral acceleration; at the finish the time and the side to pull to
// =============================================================================================================
static void __fastcall AICar_init_rtinfo(AICar* self, Edx) {
    const P3& v = self->velocity;
    self->speed = (float)x87_sqrt((D(v.y) * v.y + D(v.z) * v.z) + D(v.x) * v.x);
    self->now = PhysicsGetTime();
    self->finished = self->race_state == 3;
    call_init_rt_lat(self, 0);
    void* deity = g_deity;
    void** vt = *(void***)deity;
    const int32_t lap = ((int32_t(__fastcall*)(void*, Edx, int32_t))vt[0x48 / 4])(deity, 0, self->car_index);  // GetCurrentLap
    const float lapf = (float)lap;
    void* deity2 = g_deity;
    const int32_t laps = ((int32_t(__fastcall*)(void*, Edx))vt[0x20 / 4])(deity2, 0);   // (the vtable read once)
    self->race_fraction = (float)(D(lapf) / (double)laps);
    self->lat_accel = (float)(D(self->lat_g) * K_9_81);
    if (self->finished && !(U(self->finish_time) & 0x7fffffffu)) {
        COPY4(self->finish_time, self->now);
        SETB(self->recover_side, I(self->center_lat) > 0 ? B_ONE : B_MINUS_ONE);
    }
}
// the car; init_rt_lat's: the car, its line (a lost bead found again) and (at most) the deity's race record with its
// centre line, as Car::Update lists it
static void fp_init_rtinfo(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    fp_line(f, self);
    uint8_t* deity = (uint8_t*)g_deity;
    if (!deity || *(uint32_t*)deity != VT_RaceDeity) return;
    f.add(deity, 2012, "deity");
    if ((uint32_t)self->car_index < 16u) {
        uint8_t* info = deity + 0x38 + 0x74 * self->car_index;
        if (uint8_t* line = *(uint8_t**)(info + 0x6c)) f.add(line, 112, "centre line");
    }
}
PORT_FN(0x0042fa60, "AICar::init_rtinfo", AICar_init_rtinfo, fp_init_rtinfo)

// ---- AICar::update_line_info (0x42fb80): the line follows the car (position, velocity): update_car_info ----------------
static void __fastcall AICar_update_line_info(AICar* self, Edx) {
    COPY4(self->line_update_time, self->now);
    Point2D pos, dir;
    COPY4(pos.x, self->frame.pos.x);
    COPY4(pos.z, self->frame.pos.z);
    COPY4(dir.x, self->velocity.x);
    COPY4(dir.z, self->velocity.z);
    IdealLine_update_car_info(self->line, 0, &pos, &dir);
}
static void fp_update_line_info(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    fp_line(f, self);
}
PORT_FN(0x0042fb80, "AICar::update_line_info", AICar_update_line_info, fp_update_line_info)

// =============================================================================================================
// AICar::update_segment_info (0x42fbd0): the bead's segment index (none: nothing), the next one around the loop,
// the blend between them; entering a segment clears its flags and counts the learning hiatus down
// =============================================================================================================
static void __fastcall AICar_update_segment_info(AICar* self, Edx) {
    ILinePos ilp;
    call_get_ilpos(self, 0, &ilp);
    if (!ilp.seg) return;
    const int32_t idx = ilp.seg->index;             // movsx
    const int32_t old = self->seg_index;
    self->seg_index = idx;
    const uint8_t changed = (uint32_t)old - (uint32_t)idx != 0;
    const int32_t next = (idx + 1) % self->num_segs;   // cdq; idiv
    self->seg_changed = changed;
    self->next_seg_index = next;
    COPY4(self->seg_t, ilp.t);
    if (changed) {
        call_Notes_clear((Notes*)(uintptr_t)((uint32_t)(uintptr_t)self->seginfo + (uint32_t)idx * 24u), 0);
        self->learn_hiatus--;
    }
    self->seg_valid = 1;
}
static void fp_update_segment_info(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    ILinePos ilp;
    call_get_ilpos(self, 0, &ilp);
    if (ilp.seg && self->seginfo) f.add((uint8_t*)self->seginfo + 24 * ilp.seg->index, 3, "seginfo notes");
}
PORT_FN(0x0042fbd0, "AICar::update_segment_info", AICar_update_segment_info, fp_update_segment_info)

// =============================================================================================================
// AICar::get_mpath_target (0x42fc50): the rabbit on the racing line, lookahead (the bead's two segments' blend) x
// speed ahead, at least 20 m ('meccaZERO' without a line: straight ahead at 4.4 m/s); for 12 s after a four-off it
// blends from the recovery point and velocity; during the launch the grid lane blends into it (quadratically);
// after the launch the target velocity is scaled by skill+0x24 (less when too fast). The notes add speed (which
// get_real_target then overwrites) and a lateral offset; a contact coming within 0.3..4 s passes it by 6.7 m
// (flipped to the other side near the road's edge; 'CONTACT'), halved speed for a second one; the pass offset
// decays 1.4% a think.
// =============================================================================================================
static void __fastcall AICar_get_mpath_target(AICar* self, Edx, float now) {
    float lookahead = 0.0f;                          // [+0x0c] (stays 0 without a line)
    ILinePos rabbit;
    memset(&rabbit, 0, 8);
    Point2D* tp = &self->target_pos;
    Point2D* tv = &self->target_vel;
    if (IdealLine* line = self->line) {
        const SegmentInfo* si = self->seginfo;
        const double la = (D(K_ONE) - self->seg_t) * si[self->seg_index].lookahead + D(si[self->next_seg_index].lookahead) * self->seg_t;
        lookahead = (float)la;
        const double v = la * self->speed;
        const float dist = (float)(!(D(K_20) >= v) ? v : D(K_20));   // fcom st(1); test ah,1
        IdealLine_get_rabbit_position(line, 0, tp, tv, U(dist), &rabbit);
    } else {
        SETB(tp->z, 0);
        SETB(tp->x, 0);
        SETB(tv->z, 0);
        SETB(tv->x, 0x408e38e4u);                    // 4.44 m/s
        call_MSG(self, 0, msg_tab(MSG_MECCA_ZERO));
    }
    const double since_fouroff = D(now) - self->fouroff_time;
    const float sf = (float)since_fouroff;
    if (!(since_fouroff >= D(K_12))) {               // fcom; test ah,1
        const float side = self->recover_side;
        const double bd = D(K_ONE) - D(sf) * K_1_12;
        float b = (float)bd;
        if (!(bd >= D(K_ZERO))) b = 0.0f;
        else if (I(b) > 0x3f800000) SETB(b, B_ONE);
        const double inv = D(K_ONE) - b;
        const float vx = (float)(D(self->center_recover_vel.x) * b + D(tv->x) * inv);
        const float vz = (float)(D(self->center_recover_vel.z) * b + D(tv->z) * inv);
        COPY4(tv->x, vx);
        COPY4(tv->z, vz);
        const float px = (float)((D(self->edge_offset.x) * side + self->center_pos.x) * b + D(tp->x) * inv);
        const float pz = (float)((D(self->edge_offset.z) * side + self->center_pos.z) * b + D(tp->z) * inv);
        COPY4(tp->x, px);
        COPY4(tp->z, pz);
        call_MSG(self, 0, msg_tab(MSG_FOUROFF));
    } else {
        const double since_start = D(now) - self->race_start_time;
        const float ss = (float)since_start;
        if (!(since_start >= D(self->launch_time))) {
            const double q = D(ss) / self->launch_time;
            const double s = q * q;
            const double lane = self->start_lat;
            const double rest = D(K_ONE) - s;
            const float px = (float)((D(self->center_right.x) * lane + self->center_pos.x) * rest + D(tp->x) * s);
            const float pz = (float)((lane * self->center_right.z + self->center_pos.z) * rest + s * tp->z);
            COPY4(tp->x, px);
            COPY4(tp->z, pz);
            call_MSG(self, 0, msg_tab(MSG_BEGIN));
        } else {
            const double k = (D(K_ONE) - D(self->too_fast) * K_0_2) * skf(self->skill, 0x24);
            tv->x = (float)(D(tv->x) * k);
            tv->z = (float)(k * tv->z);
        }
    }
    if (!self->finished && self->seg_valid) {
        const SegmentInfo* cur = &self->seginfo[self->seg_index];
        const float rest = (float)(D(K_ONE) - self->seg_t);
        const SegmentInfo* nxt = &self->seginfo[self->next_seg_index];
        self->target_speed = (float)((D(nxt->notes.speed_note) * self->seg_t + D(cur->notes.speed_note) * rest) + self->target_speed);
        if (U(cur->lateral_note) & 0x7fffffffu) {
            const double lat = D(nxt->lateral_note) * self->seg_t + D(cur->lateral_note) * rest;
            tp->x = (float)(D(self->center_right.x) * lat + tp->x);
            tp->z = (float)(lat * self->center_right.z + tp->z);
        }
    }
    if (self->contact_valid && I(self->contact_time) > 0x3e99999a && I(self->contact_time) < 0x40800000) {   // 0.3 .. 4 s
        const double d = D(self->contact_line_lat) - self->line_lat;
        float p = (float)d;                          // [+0]
        if (!(fabs(d) >= D(K_6_7056))) {             // fcomp; test ah,1
            if (fabs(D(self->contact_center_lat)) > D(self->track_half_width) - K_5_715) {   // fcompp; test ah,0x41
                SETB(p, I(self->contact_center_lat) > 0 ? B_ONE : B_MINUS_ONE);
                call_MSG(self, 0, msg_tab(MSG_CONTACT_FLIP));
            }
            const float q = (float)(I(p) > 0 ? D(self->contact_line_lat) - K_6_7056 : D(self->contact_line_lat) + K_6_7056);
            const double ct = self->contact_time;
            const double m = !(D(lookahead) >= ct) ? ct : D(lookahead);   // fcom st(1); test ah,1
            self->pass_offset = (float)((D(q) / m) * lookahead);
            call_MSG(self, 0, msg_tab(MSG_CONTACT));
            if (self->contact_multiple) {
                tv->x = (float)(D(tv->x) * K_HALF);
                tv->z = (float)(D(tv->z) * K_HALF);
            }
        }
    }
    const double po = self->pass_offset;
    tp->x = (float)(D(self->center_right.x) * po + tp->x);
    tp->z = (float)(po * self->center_right.z + tp->z);
    self->pass_offset = (float)(D(self->pass_offset) * K_0_986);
}
static void fp_mpath(Footprint& f, AICar* self, Edx, float) {
    f.object(self, "AICar");
    fp_line(f, self);                                // get_rabbit_position
}
PORT_FN(0x0042fc50, "AICar::get_mpath_target", AICar_get_mpath_target, fp_mpath)

// =============================================================================================================
// AICar::get_real_target (0x4300f0): get_mpath_target; the target speed (|target_vel|) and the world point kept for
// the replay; both made car-local; their directions (normalised unless shorter than 2^-23)
// =============================================================================================================
static void __fastcall AICar_get_real_target(AICar* self, Edx) {
    call_get_mpath_target(self, 0, U(self->now));
    const double ss = D(self->target_vel.x) * self->target_vel.x + D(self->target_vel.z) * self->target_vel.z;
    memcpy(&self->target_world, &self->target_pos, 8);
    self->target_speed = (float)x87_sqrt(ss);
    call_localify_vector(self, 0, &self->target_vel);
    call_localify_loc(self, 0, &self->target_pos);
    {
        Point2D& d = self->target_vel_dir;
        memcpy(&d, &self->target_vel, 8);
        const double l = x87_sqrt(D(d.x) * d.x + D(d.z) * d.z);
        const float lf = (float)l;
        if (fabs(l) > D(K_EPS)) {                    // fcomp; test ah,0x41; jne
            d.x = (float)(D(d.x) / lf);
            d.z = (float)(D(d.z) / lf);
        }
    }
    {
        Point2D& d = self->target_pos_dir;
        memcpy(&d, &self->target_pos, 8);
        const double l = x87_sqrt(D(d.z) * d.z + D(d.x) * d.x);
        const float lf = (float)l;
        if (fabs(l) > D(K_EPS)) {
            d.x = (float)(D(d.x) / lf);
            d.z = (float)(D(d.z) / lf);
        }
    }
}
static void fp_real_target(Footprint& f, AICar* self, Edx) {
    f.object(self, "AICar");
    fp_line(f, self);                                // get_mpath_target's get_rabbit_position
}
PORT_FN(0x004300f0, "AICar::get_real_target", AICar_get_real_target, fp_real_target)
