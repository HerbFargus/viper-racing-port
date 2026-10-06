// phys_aicar2.cpp -- M3 stage 3.5 group A2: the AI driver, the second half of physics:aicar.obj (0x430250 to
// 0x432090), rewritten.
//
// AICar::Update is the AI car's tick: before the green flag it blips the throttle (rev_engine); racing, it reads
// the world (init_rtinfo, which calls init_rt_lat: the car's place against the centre line), decays its memory of
// the other cars, starts the race once, runs the periodic checks (four wheels off, too fast, lap change,
// stranded, damage, wall heat, the dice), picks traction and yaw control, updates its racing line and segment,
// looks at the three nearest cars (socialize -> fast_interact -> checker / passer -> dont_check, dont_push,
// add_contact, headon_panic), and on a think tick picks a target and drives (fast_drive -> obey_speed_limit,
// latslam, lonslam, the AISet* controls); then the aftershock of a crash, and analyze, which learns the
// segment notes; and last the car's own physics, Car::Update. With them: begin_race, begin_lap, random_real,
// off_road, decay, roll_dice, check_for_lapchange, DoneLowering, periodics, HermiteEval, Right,
// crash_aftershock, apply_yaw_control (the AI's own, with 0.93 of the steering), in_path, the trivial virtuals
// (Car::GetMessageSize, Car::IsSolid, AICar::GetReplayPacketSize, compiled into aicar.obj) and ProxerInfo's
// constructor. Left out: AICar::slow_drive (0x431960), a bare `ret`.
//
// Written from the v1.0 disassembly, block by block, as the other phys_*.cpp: the same sums in the same
// grouping, register values as doubles and stored ones as floats, floats the original moves with integer
// instructions moved as bits, every integer compare of a float's bits done on the bits, and every call in the
// original's order with its arguments. Every callee -- the first half of aicar.obj (another group), the ai
// library (IdealLine, CenterLine, ILSeg, ProxerDelta: driver.obj / ideal.obj / prox), the Car setters, the
// deity -- is called by address or through its vtable (or the AICar's own member-function pointers,
// drive_fn / interact_fn), never inlined, so whichever version is hooked there is what runs.
//
// Fixes (port.h: VP_FIX; VP_FAITHFUL builds the original's behaviour):
//  * the known AI crash: a stranded-car teleport (periodics -> check_for_stranded / check_for_damage / the wall
//    heat -> teleport_to_track -> IdealLine::reset_bead_position) or a NaN position could leave the line's bead
//    NULL, and init_rt_lat read it (the ILinePos from get_ilpos) unchecked. reset_bead_position (phys_ideal.cpp)
//    now puts a lost bead back at the nearest point of the line, and init_rt_lat asks it to before reading.
//    (passer / obey_speed_limit / fast_drive read the segment info's segments, never NULL, not the bead; in_path
//    hands get_nearest_bead one of those.)
//  * begin_race's 32-byte buffer for the driver's name (AIGetDriverNameByCar's unbounded strcpy): cut to 31.
//
// In the comments, [+0xNN] is a local's offset in the original's frame (after its register pushes).
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// the unused edx of a __thiscall received as __fastcall (an int, as in phys_sphere.cpp: test/fuzz.h sizes it)
typedef int Edx;

// a float's bits, for the original's integer compares and copies
static __forceinline int32_t I(const float& x) { int32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline uint32_t Ub(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
#define COPY4(dst, src) memcpy(&(dst), &(src), 4)  // an integer move of a float
#define SET4(dst, bits) do { const uint32_t b_ = (bits); memcpy(&(dst), &b_, 4); } while (0)   // mov dword, imm

// a float copied through the FPU (fld; fst): the same value, but a signalling NaN comes out quiet
static __declspec(noinline) float fpu_copy(float x) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("fld %[x]\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [x] "m"(x)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld x
            fstp r }
#endif
    return r;
}

// ---- layouts (anonymous: the first half of aicar.obj and the ai library define their own) ------------------
namespace {

struct Point2D { float x, z; };

// a node of an ideal line (ideal.obj, 68 bytes): a loop through `next`
struct ILSeg {
    ILSeg* next;                       // +0x00
    Point2D pos;                       // +0x04
    Point2D dir;                       // +0x0c
    float corridor_half_width;         // +0x14
    float target_speed;                // +0x18
    float curvature;                   // +0x1c
    float step;                        // +0x20
    float cum_distance;                // +0x24
    uint16_t index;                    // +0x28
    uint8_t sector, pad_ff;            // +0x2a
    float field_11;                    // +0x2c
    float dist_ahead;                  // +0x30 fast_drive: over the speed, < 1 s -> a cold apex may lift
    float dist_ahead_signed;           // +0x34 obey_speed_limit: over the speed, < 2 s -> near a braking point
    float cum_time;                    // +0x38
    float lateral;                     // +0x3c passer: the line's offset, blended along the segment
    uint32_t pool_guard;               // +0x40
};
static_assert(offsetof(ILSeg, dist_ahead) == 0x30 && offsetof(ILSeg, lateral) == 0x3c && sizeof(ILSeg) == 68, "ILSeg");

struct ILinePos { ILSeg* seg; float t; };

// AICar::SegmentInfo (24 bytes): the learned notes of one line segment
struct SegmentInfo {
    uint8_t flag;                      // +0x00 notes: analyze moves the speed notes of the 5 segments before it
    uint8_t offroad_left;              // +0x01 notes: wheels 0 / 2 off the road here (analyze)
    uint8_t offroad_right;             // +0x02 notes: wheels 1 / 3
    uint8_t _03;
    float speed_note;                  // +0x04 notes: -0.622 m/s per step back from a flagged segment
    float lateral_note;                // +0x08 m, +-1/3 .. 1 per learn toward the inside
    uint8_t locked, _0d[3];            // +0x0c set once |lateral_note| passes 4 m (or 0.1 m against the push)
    float lookahead;                   // +0x10
    ILSeg* seg;                        // +0x14
};
static_assert(offsetof(SegmentInfo, lateral_note) == 8 && offsetof(SegmentInfo, seg) == 0x14 && sizeof(SegmentInfo) == 24,
              "SegmentInfo");

struct ProxerInfo { float level; uint32_t _4; };

// the proximity table (prox): record k at records + k x record_size: position, velocity, then 2n dwords and the
// n nearest cars' ProxerDelta pointers, sorted
struct Proxer {
    int32_t num_objects;               // +0x00
    uint32_t _04, _08;
    int32_t record_size;               // +0x0c n x 12 + 16
    uint8_t* records;                  // +0x10
};
struct ProxerDelta {                   // 32 bytes: one pair of cars
    int16_t car_a, car_b;              // +0x00
    Point2D dl;                        // +0x04 as car_a sees it (GetMyDL negates it for car_b)
    Point2D dv;                        // +0x0c
    uint8_t _14[32 - 0x14];
};
static_assert(offsetof(ProxerDelta, dv) == 0x0c && sizeof(ProxerDelta) == 32, "ProxerDelta");

// IdealLine / ConstIdealLine (56 / 60 bytes): only what's read here
struct IdealLine {
    void** vtable;                     // +0x00
    ILSeg* bead_seg;                   // +0x04
    float bead_t;                      // +0x08
    uint8_t _0c[0x18 - 0x0c];
    Point2D car_pos;                   // +0x18 update_car_info's position
    uint8_t _20[60 - 0x20];
};
static_assert(offsetof(IdealLine, car_pos) == 0x18 && sizeof(IdealLine) == 60, "IdealLine");

// CenterLine (112 bytes, a ConstIdealLine): the deity's per-car centre line
struct CenterLine {
    void** vtable;                     // +0x00
    ILSeg* bead_seg;                   // +0x04
    uint8_t _08[0x54 - 0x08];
    float lat;                         // +0x54 m right of the centre
    float lat_delta;                   // +0x58 m per update
    float length;                      // +0x5c m round the loop (init_rt_lat takes fmod by it)
    uint8_t _60[112 - 0x60];
};
static_assert(offsetof(CenterLine, lat) == 0x54 && offsetof(CenterLine, length) == 0x5c && sizeof(CenterLine) == 112,
              "CenterLine");

// the driver's skill record (SkillInfo0, 108 bytes, as SkillInfo2 begins)
struct Skill {
    float recovery_time;               // +0x00
    int32_t think_ticks;               // +0x04 an int: Update divides the physics tick by it
    float brake_release_lag;           // +0x08
    uint32_t _0c;
    float bump_tc_off_time;            // +0x10
    float aftershock_time;             // +0x14
    uint32_t _18;
    float throttle_scale;              // +0x1c
    uint32_t _20;
    float target_speed_scale;          // +0x24
    float steer_lock_time;             // +0x28
    uint32_t _2c, _30;
    float overspeed_throttle;          // +0x34
    uint8_t hot_entry_threshold;       // +0x38
    uint8_t cold_apex_threshold;       // +0x39
    uint8_t dice3_threshold;           // +0x3a
    uint8_t _3b;
    float bump_impulse;                // +0x3c
    float crash_impulse;               // +0x40
    uint8_t _44[0x60 - 0x44];
    float proxer_decay;                // +0x60
    uint8_t _64[108 - 0x64];
};
static_assert(offsetof(Skill, overspeed_throttle) == 0x34 && offsetof(Skill, proxer_decay) == 0x60 && sizeof(Skill) == 108,
              "Skill");

struct AITrackInfo { float launch_time_base, launch_time_per_car; uint8_t tc_always, _09[3]; int32_t field_0c;
                     float first_lap_allowance; };
static_assert(sizeof(AITrackInfo) == 20, "AITrackInfo");

// a Wheel (420 bytes): only its surface
struct Wheel {
    uint8_t _000[0x19c];
    int32_t surface;                   // +0x19c 0 = the road
    uint8_t _1a0[4];
};
static_assert(sizeof(Wheel) == 0x1a4, "Wheel");

// AICar (aicar.obj, 4224 bytes, vtable 0x4dbc28): a LocalCar (3784) and the driver. Names from out/types.tsv.
struct AICar {
    void** vtable;                     // +0x000
    uint8_t _004[0x234 - 0x004];
    P3 velocity;                       // +0x234
    P3 angular_velocity;               // +0x240
    uint8_t _24c[0x4f8 - 0x24c];
    int32_t yaw_control;               // +0x4f8
    int32_t race_state;                // +0x4fc 0 grid, 2 racing, 3 finished
    float steering;                    // +0x500
    uint8_t _504[0x50c - 0x504];
    float opacity;                     // +0x50c
    int32_t car_index;                 // +0x510
    uint8_t _514[0x554 - 0x514];
    Wheel wheels[4];                   // +0x554 0,2 on the -x side
    uint8_t _be4[0xc34 - 0xbe4];
    float engine_throttle;             // +0xc34 Engine (+0xbfc) +0x38: the engine's smoothed throttle
    uint8_t _c38[0xd84 - 0xc38];
    float front_axle_z;                // +0xd84
    uint8_t _d88[0xd90 - 0xd88];
    float rear_axle_z;                 // +0xd90
    uint8_t _d94[0xdcc - 0xd94];
    float max_steer_angle;             // +0xdcc
    uint8_t _dd0[0xe3d - 0xdd0];
    uint8_t lowering;                  // +0xe3d
    uint8_t _e3e[0xeb0 - 0xe3e];
    float steer_feedback;              // +0xeb0
    uint8_t _eb4[0xec8 - 0xeb4];
    // ---- AICar
    void* drive_fn;                    // +0xec8 fast_drive / slow_drive
    void* interact_fn;                 // +0xecc fast_interact / slow_interact
    uint8_t ahead_valid, _ed1[3];      // +0xed0
    float ahead_dist;                  // +0xed4
    float ahead_rel_speed;             // +0xed8
    float side_lat;                    // +0xedc
    float side_lat_rel_speed;          // +0xee0
    float side_lon;                    // +0xee4
    float side_lon_rel_speed;          // +0xee8
    uint8_t side_i_lead, _eed[3];      // +0xeec
    float slam_steer;                  // +0xef0
    uint8_t side_valid;                // +0xef4
    uint8_t side_squeezed, _ef6[2];    // +0xef5
    float contact_time;                // +0xef8
    float contact_line_lat;            // +0xefc
    float pass_offset;                 // +0xf00
    float contact_center_lat;          // +0xf04
    uint8_t contact_valid;             // +0xf08
    uint8_t contact_multiple, _f0a[2]; // +0xf09
    float now;                         // +0xf0c
    float speed;                       // +0xf10
    float track_half_width;            // +0xf14
    float center_lat;                  // +0xf18
    float center_lat_norm;             // +0xf1c
    float center_lat_delta;            // +0xf20
    float line_lat;                    // +0xf24
    float lat_accel;                   // +0xf28 (init_rtinfo: Car lat_g x 9.81)
    float race_fraction;               // +0xf2c
    uint8_t finished, _f31[3];         // +0xf30
    int32_t lap;                       // +0xf34
    uint8_t race_begun, _f39[3];       // +0xf38
    float start_lat;                   // +0xf3c
    uint32_t tick;                     // +0xf40
    ILSeg* progress_seg;               // +0xf44
    float steer_damping;               // +0xf48
    float steer;                       // +0xf4c
    float steer_gain;                  // +0xf50
    uint8_t tc_always, _f55[3];        // +0xf54
    Skill* skill;                      // +0xf58
    Point2D target_world;              // +0xf5c
    Point2D target_pos;                // +0xf64
    Point2D target_vel;                // +0xf6c
    Point2D target_pos_dir;            // +0xf74
    Point2D target_vel_dir;            // +0xf7c
    float target_speed;                // +0xf84
    Point2D center_pos;                // +0xf88
    Point2D center_recover_vel;        // +0xf90
    Point2D center_tan;                // +0xf98
    Point2D edge_offset;               // +0xfa0
    Point2D side_edge_offset;          // +0xfa8
    Point2D center_right;              // +0xfb0
    float progress_time;               // +0xfb8
    float finish_time;                 // +0xfbc
    float crash_time;                  // +0xfc0
    float bump_time;                   // +0xfc4
    float fouroff_time;                // +0xfc8
    float race_start_time;             // +0xfcc
    float freeze_time;                 // +0xfd0
    float wall_heat;                   // +0xfd4
    float _fd8;                        // +0xfd8
    float recover_side;                // +0xfdc
    float offground_cut;               // +0xfe0
    float line_update_time;            // +0xfe4
    IdealLine* line;                   // +0xfe8
    const char* msgs[10];              // +0xfec
    int32_t num_msgs;                  // +0x1014
    float min_brake;                   // +0x1018
    uint8_t dice_cold_apex;            // +0x101c
    uint8_t dice_hot_entry;            // +0x101d
    uint8_t dice_unused, _101f;        // +0x101e
    float too_fast;                    // +0x1020
    float lap_target_time;             // +0x1024
    float base_lap_time;               // +0x1028
    float lap_time_spread;             // +0x102c
    float pace;                        // +0x1030
    float schedule_error;              // +0x1034
    float* lap_clock;                  // +0x1038
    float throttle_cap;                // +0x103c
    float lap_time_offset;             // +0x1040
    float line_lap_time;               // +0x1044
    ProxerInfo* proxer_info;           // +0x1048
    int32_t num_proxer_info;           // +0x104c
    int32_t learn_hiatus;              // +0x1050
    SegmentInfo* seginfo;              // +0x1054
    int32_t num_segs;                  // +0x1058
    int32_t seg_index;                 // +0x105c
    int32_t next_seg_index;            // +0x1060
    float seg_t;                       // +0x1064
    uint8_t seg_changed;               // +0x1068
    uint8_t seg_valid, _106a[2];       // +0x1069
    float wall_hit_lat;                // +0x106c
    int32_t last_learn_lap;            // +0x1070
    float latslam_rel_speed;           // +0x1074
    float launch_time;                 // +0x1078
    float teleport_armed_time;         // +0x107c
};
static_assert(offsetof(AICar, velocity) == 0x234 && offsetof(AICar, yaw_control) == 0x4f8 && offsetof(AICar, car_index) == 0x510 &&
              offsetof(AICar, wheels) == 0x554 && offsetof(AICar, engine_throttle) == 0xc34 && offsetof(AICar, max_steer_angle) == 0xdcc &&
              offsetof(AICar, lowering) == 0xe3d && offsetof(AICar, steer_feedback) == 0xeb0 && offsetof(AICar, drive_fn) == 0xec8 &&
              offsetof(AICar, side_i_lead) == 0xeec && offsetof(AICar, contact_valid) == 0xf08 && offsetof(AICar, now) == 0xf0c &&
              offsetof(AICar, finished) == 0xf30 && offsetof(AICar, tick) == 0xf40 && offsetof(AICar, skill) == 0xf58 &&
              offsetof(AICar, target_speed) == 0xf84 && offsetof(AICar, center_right) == 0xfb0 && offsetof(AICar, wall_heat) == 0xfd4 &&
              offsetof(AICar, line) == 0xfe8 && offsetof(AICar, num_msgs) == 0x1014 && offsetof(AICar, too_fast) == 0x1020 &&
              offsetof(AICar, lap_clock) == 0x1038 && offsetof(AICar, proxer_info) == 0x1048 && offsetof(AICar, seginfo) == 0x1054 &&
              offsetof(AICar, seg_changed) == 0x1068 && offsetof(AICar, teleport_armed_time) == 0x107c && sizeof(AICar) == 4224, "AICar");

// for the pure ones the fuzzer runs (its arena is 4 KB: an AICar doesn't fit)
struct CarOpacity { uint8_t _000[0x50c]; float opacity; };

static const uint32_t VT_RaceDeity = 0x004dc588;

}  // namespace

// ---- constants, exactly as the original's -------------------------------------------------------------------
static const float k_milli = 0x1.0624dep-10f;            // 0x3a83126f  0.001
static const float k_point2 = 0x1.99999ap-3f;            // 0x3e4ccccd  0.2
static const float k_note_step = -0x1.8e38e4p+1f;        // 0xc0471c72  -3.1111112 (x 0.2: -0.622 m/s)
static const float k_point1 = 0x1.99999ap-4f;            // 0x3dcccccd  0.1
static const float k_third = 0x1.555556p-2f;             // 0x3eaaaaab  0.33333334
static const float k_point99 = 0x1.fae148p-1f;           // 0x3f7d70a4  0.99
static const float k_point992 = 0x1.fbe76cp-1f;          // 0x3f7df3b6  0.992
static const float k_car_width = 0x1.e7ae14p+0f;         // 0x3ff3d70a  1.905 m
static const float k_point7 = 0x1.666666p-1f;            // 0x3f333333  0.7
static const float k_60mph = 0x1.aaaaaap+4f;             // 0x41d55555  26.666666 m/s
static const float k_m0225 = -0x1.ccccccp-3f;            // 0xbe666666  -0.225
static const float k_over_far = 0x1.1c71c8p+3f;          // 0x410e38e4  8.888889 m/s (20 mph)
static const float k_over_hard = 0x1.aaaaaap+2f;         // 0x40d55555  6.6666665 m/s (15 mph)
static const float k_015 = 0x1.333334p-3f;               // 0x3e19999a  0.15
static const float k_093 = 0x1.dc28f6p-1f;               // 0x3f6e147b  0.93
static const float k_yaw_min = 0x1.c71c72p-1f;           // 0x3f638e39  0.8888889 m/s
static const float k_yaw_steer = 0x1.eb851ep-7f;         // 0x3c75c28f  0.015
static const float k_eps = 0x1.0p-23f;                   // 0x34000000  FLT_EPSILON
static const float k_check_lon = 0x1.5753a4p+2f;         // 0x40aba9d2  5.36448 m (17.6 ft)
static const float k_check_lat = 0x1.249ba6p+2f;         // 0x40924dd3  4.572 m (15 ft)
static const float k_pass_ahead = 0x1.1e1b08p+3f;        // 0x410f0d84  8.9408 m (29.3 ft)
static const float k_rel_min = 0x1.a36e2ep-14f;          // 0x38d1b717  0.0001
static const float k_gap = 0x1.3ab756p+2f;               // 0x409d5bab  4.91744 m (16.1 ft)
static const float k_point9 = 0x1.ccccccp-1f;            // 0x3f666666  0.9
static const float k_point8 = 0x1.99999ap-1f;            // 0x3f4ccccd  0.8
static const float k_point96 = 0x1.eb851ep-1f;           // 0x3f75c28f  0.96
static const float k_edge_margin = 0x1.9e872cp+0f;       // 0x3fcf4396  1.61925 m
static const float k_point97 = 0x1.f0a3d8p-1f;           // 0x3f7851ec  0.97
static const float k_side_lat = 0x1.6dc290p+1f;          // 0x4036e148  2.8575 m
static const float k_side_lon = 0x1.2c692ep+2f;          // 0x40963497  4.69392 m
static const float k_slam_v0 = 0x1.e7ae14p-1f;           // 0x3f73d70a  0.9525 m/s
static const float k_slam_k = 0x1.ae06b8p-3f;            // 0x3e57035c  0.20997375
static const float k_slam_mk = -0x1.ae06b8p-3f;          // 0xbe57035c
static const float k_slam_k4 = 0x1.ae06b8p-1f;           // 0x3f57035c  0.839895
static const float k_slam_step_m = -0x1.7c1bdap-9f;      // 0xbb3e0ded  -0.0029
static const float k_slam_step = 0x1.7c1bdap-9f;         // 0x3b3e0ded  0.0029
static const float k_point6 = 0x1.333334p-1f;            // 0x3f19999a  0.6
static const float k_30th = 0x1.111112p-5f;              // 0x3d088889  1/30
static const float k_m30th = -0x1.111112p-5f;            // 0xbd088889
static const float k_150th = 0x1.b4e81cp-8f;             // 0x3bda740e  1/150

// ---- the functions these call, by address ----------------------------------------------------------------------
typedef void(__fastcall* Method0_t)(void*, Edx);
typedef void(__fastcall* MethodU32_t)(void*, Edx, uint32_t);              // a float passed as bits
typedef void(__fastcall* MethodU8_t)(void*, Edx, uint8_t);
typedef void(__fastcall* Msg_t)(AICar*, Edx, const char*);
typedef SegmentInfo*(__fastcall* RelSeg_t)(AICar*, Edx, int, int);
typedef SegmentInfo*(__fastcall* RelSeg2_t)(AICar*, Edx, const SegmentInfo*, int);
typedef ILinePos*(__fastcall* GetIlpos_t)(AICar*, Edx, ILinePos*);        // the hidden return pointer
typedef uint8_t(__fastcall* Teleport_t)(AICar*, Edx);
typedef uint8_t(__fastcall* Interact_t)(AICar*, Edx, const ProxerDelta*);
typedef uint8_t(__fastcall* InPath_t)(AICar*, Edx, const Point2D*, float, ILinePos*, float*);
typedef void(__fastcall* DontCheck_t)(AICar*, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t);
typedef void(__fastcall* DontPush_t)(AICar*, Edx, uint32_t, uint32_t);
typedef void(__fastcall* AddContact_t)(AICar*, Edx, uint32_t, uint32_t, float);
typedef void(__fastcall* Speed3_t)(AICar*, Edx, float*, float*, float*);
typedef void(__fastcall* Slam2_t)(AICar*, Edx, float*, float*);
typedef void(__fastcall* Slam1_t)(AICar*, Edx, float*);
typedef uint8_t(__cdecl* Flag_t)();
typedef int(__cdecl* Int_t)();
typedef int(__cdecl* RandomInt_t)(int);
typedef double(__cdecl* RandomReal_t)(float, uint32_t);                   // returns ST0
typedef const AITrackInfo*(__cdecl* TrackInfo_t)(int);
typedef void(__cdecl* DriverName_t)(char*, int);
typedef uint8_t(__cdecl* WheelFlag_t)(Wheel*);
typedef int(__cdecl* CountWheels_t)(uint32_t, Wheel*);                    // (the predicate's address, wheels)
typedef uint8_t(__cdecl* IsAI_t)(int);
typedef void(__cdecl* Hermite_t)(Point2D*, const Point2D*, const Point2D*, const Point2D*, const Point2D*, uint32_t);
typedef void(__cdecl* Right_t)(Point2D*);
typedef void(__fastcall* QuickTan_t)(const ILSeg*, Edx, Point2D*, uint32_t);
typedef double(__fastcall* DlongMeters_t)(CenterLine*, Edx);             // returns ST0
typedef ILinePos*(__fastcall* MetersToIlpos_t)(CenterLine*, Edx, ILinePos*, uint32_t);
typedef double(__fastcall* NearestBead_t)(IdealLine*, Edx, const Point2D*, ILinePos*, uint32_t);   // returns ST0
typedef void(__fastcall* GetMyD_t)(const ProxerDelta*, Edx, int, Point2D*);
typedef void(__fastcall* LineVoid_t)(IdealLine*, Edx);
typedef int(__cdecl* IntInt_t)(int);
typedef void(__cdecl* ProfVoid_t)();
typedef int(__cdecl* ProfStart_t)(const char*);
typedef void(__cdecl* ProfStop_t)(int);

// the first half of aicar.obj (another group's rewrites are hooked there)
static const Msg_t AICar__MSG = (Msg_t)0x0042f510;
static const Msg_t AICar_suspend_adjustment = (Msg_t)0x0042ef80;
static const Method0_t AICar_fire_fouroff = (Method0_t)0x0042efa0;
static const RelSeg_t AICar_GetRelSegment = (RelSeg_t)0x0042efd0;
static const RelSeg2_t AICar_GetRelSegment2 = (RelSeg2_t)0x0042f000;
static const GetIlpos_t AICar_get_ilpos = (GetIlpos_t)0x0042e860;
static const MethodU32_t AICar_AISetThrottle = (MethodU32_t)0x0042f030;
static const MethodU32_t AICar_AISetSteering = (MethodU32_t)0x0042f1c0;
static const MethodU32_t AICar_AISetBrake = (MethodU32_t)0x0042f310;
static const MethodU8_t AICar_set_yaw_control = (MethodU8_t)0x0042f460;
static const MethodU8_t AICar_set_traction_control = (MethodU8_t)0x0042f4a0;
static const Teleport_t AICar_teleport_to_track = (Teleport_t)0x0042f560;
static const Method0_t AICar_check_for_stranded = (Method0_t)0x0042f620;
static const Method0_t AICar_check_for_too_fast = (Method0_t)0x0042f730;
static const Method0_t AICar_check_for_damage = (Method0_t)0x0042f8b0;
static const Method0_t AICar_init_rtinfo = (Method0_t)0x0042fa60;
static const Method0_t AICar_update_line_info = (Method0_t)0x0042fb80;
static const Method0_t AICar_update_segment_info = (Method0_t)0x0042fbd0;
static const Method0_t AICar_get_real_target = (Method0_t)0x004300f0;
static const uint32_t count_wheels_a = 0x0042f190;
// this group's own, called by address (so each one's hooked version runs, as in the original)
static const Method0_t AICar_begin_race_o = (Method0_t)0x00430250;
static const Method0_t AICar_begin_lap_o = (Method0_t)0x004302d0;
static const RandomReal_t random_real_o = (RandomReal_t)0x00430350;
static const Method0_t AICar_analyze_o = (Method0_t)0x00430380;
static const uint32_t off_road_a = 0x004305d0;
static const WheelFlag_t off_road_o = (WheelFlag_t)off_road_a;
static const Method0_t AICar_decay_o = (Method0_t)0x004305e0;
static const Method0_t AICar_roll_dice_o = (Method0_t)0x00430630;
static const Method0_t AICar_check_for_lapchange_o = (Method0_t)0x004306a0;
static const Method0_t AICar_periodics_o = (Method0_t)0x004306f0;
static const Hermite_t HermiteEval_o = (Hermite_t)0x00430a70;
static const Right_t Right_o = (Right_t)0x00430b20;
static const Method0_t AICar_crash_aftershock_o = (Method0_t)0x00430b40;
static const Speed3_t AICar_obey_speed_limit_o = (Speed3_t)0x00430bc0;
static const Method0_t AICar_socialize_o = (Method0_t)0x00430d90;
static const Method0_t AICar_rev_engine_o = (Method0_t)0x00430e20;
static const Method0_t AICar_headon_panic_o = (Method0_t)0x004311d0;
static const InPath_t AICar_in_path_o = (InPath_t)0x00431240;
static const Interact_t AICar_checker_o = (Interact_t)0x004312d0;
static const Interact_t AICar_passer_o = (Interact_t)0x00431560;
static const DontPush_t AICar_dont_push_o = (DontPush_t)0x00431800;
static const DontCheck_t AICar_dont_check_o = (DontCheck_t)0x00431840;
static const AddContact_t AICar_add_contact_o = (AddContact_t)0x00431900;
static const Slam1_t AICar_lonslam_o = (Slam1_t)0x00431970;
static const Slam2_t AICar_latslam_o = (Slam2_t)0x00431a90;
// the Car's
static const MethodU32_t Car_SetSteering = (MethodU32_t)0x00439620;
static const MethodU32_t Car_SetThrottle = (MethodU32_t)0x004396d0;
static const MethodU32_t Car_SetBraking = (MethodU32_t)0x004396e0;
static const MethodU32_t Car_SetClutch = (MethodU32_t)0x004396f0;
static const MethodU32_t Car_SetEBrake = (MethodU32_t)0x00439700;
static const Method0_t Car_SetGearAutoAI = (Method0_t)0x00439760;
static const Method0_t Car_Update = (Method0_t)0x00437680;               // LocalCar's, called non-virtually
// the ai library and the rest
static const RandomInt_t Random = (RandomInt_t)0x0041b6e0;
static const Flag_t PhysReplayPlayMode = (Flag_t)0x0042d340;
static const DriverName_t AIGetDriverNameByCar = (DriverName_t)0x00420d40;
static const Int_t AIGetTrackNumber = (Int_t)0x00420df0;
static const TrackInfo_t AIGetTrackInfo = (TrackInfo_t)0x00424e40;
static const Flag_t AILearnMode = (Flag_t)0x0041d340;
static const IsAI_t IsAI = (IsAI_t)0x0041d370;
static const QuickTan_t ILSeg_QuickTan = (QuickTan_t)0x00426150;
static const DlongMeters_t CenterLine_get_car_dlong_meters = (DlongMeters_t)0x004225e0;
static const MetersToIlpos_t CenterLine_convert_meters_to_ilpos = (MetersToIlpos_t)0x00422710;
static const NearestBead_t IdealLine_get_nearest_bead = (NearestBead_t)0x004216f0;
static const LineVoid_t IdealLine_reset_bead_position = (LineVoid_t)0x00421520;
static const IntInt_t AIGetDriverForCar = (IntInt_t)0x00420ce0;
static const Int_t AIGetStrength = (Int_t)0x0041d310;
static const GetMyD_t ProxerDelta_GetMyDL = (GetMyD_t)0x00423430;
static const GetMyD_t ProxerDelta_GetMyDV = (GetMyD_t)0x00423470;
typedef int(__cdecl* GameState_t)();
static const GameState_t GetGameState = (GameState_t)0x0040a3d0;
typedef double(__cdecl* Time_t)();
static const Time_t PhysicsGetTime = (Time_t)0x0042bc80;
// globals
#define g_deity (*(void**)0x005218ac)                     // class Deity* deity
#define g_race_deity_global (*(uint8_t**)0x004edd70)      // RaceDeity::global: per-car records 0x74 apart
#define g_proxer (*(Proxer**)0x004ec480)                  // Proxer const* const g_proxer
#define g_physics_tick (*(volatile int32_t*)0x0052161c)
#define g_game_state ((void*)0x004e4e3c)
#define g_splash_sound (*(uint8_t**)0x00521f7c)
#define g_lounge (*(void**)0x004eb8b0)                   // the AI driver lounge (AIDriverLounge*)
// the profiler's hooks: pointers the original calls through
#define i_pr_overhead_begin (*(ProfVoid_t*)0x004e642c)
#define i_pr_overhead_end (*(ProfVoid_t*)0x004e6430)
#define i_prof_start (*(ProfStart_t*)0x004e6434)
#define i_prof_stop (*(ProfStop_t*)0x004e6438)

// the _MSG reason strings: pointers in msg_tab (0x4ed408), read at run time
static __forceinline const char* msg_at(uint32_t slot) { return *(const char* const*)slot; }
// RaceDeity::global's per-car record (0x74 bytes each, at +0x38): the lap count (+0x6c) and the CenterLine (+0xa4)
static __forceinline uint8_t* race_record(const AICar* self) {
    return g_race_deity_global + (uint32_t)self->car_index * 0x74u;
}

// fmod(a, b) through the CRT's __CIfmod in race.exe (0x4cf36a): a in ST1, b in ST0; the result stored as a float
VP_ASM_CALLS static float cifmod_f(double a, float b) {
    float r;
#ifdef VP_GCC
    __asm__ volatile("fld %[a]\n\t"
                     "fld %[b]\n\t"
                     "push ecx\n\t"
                     "push edx\n\t"
                     "mov eax, 0x4cf36a\n\t"
                     "call eax\n\t"
                     "pop edx\n\t"
                     "pop ecx\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [a] "m"(a), [b] "m"(b)
                     : VP_X87_CLOBBERS, "eax", "ecx", "edx", "cc", "memory");
#else
    __asm {
        push ecx
        push edx
        fld a
        fld b
        mov eax, 0x4cf36a
        call eax
        pop edx
        pop ecx
        fstp r
    }
#endif
    return r;
}

// fptan's full-mantissa tangent divided by the wheelbase in the same asm block (the quotient IS rounded to 24
// bits). For |steer| >= 2^63 fptan pushes nothing and the original underflows its x87 stack (phys_car_update.cpp):
// unreachable (the steering is clamped to +-1, max_steer_angle < 1 rad).
static __forceinline double yaw_tan_ratio(double steer, double wheelbase) {
    double r;
#ifdef VP_GCC
    __asm__ volatile("fld %[steer]\n\t"
                     "fptan\n\t"
                     "fstp st(0)\n\t"
                     "fld %[wheelbase]\n\t"
                     "fdivp st(1), st(0)\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [steer] "m"(steer), [wheelbase] "m"(wheelbase)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld steer
            fptan
            fstp st(0)
            fld wheelbase
            fdivp st(1), st(0)
            fstp r }
#endif
    return r;
}

// fpatan (atan2(y, x), a full 64-bit mantissa) divided by a float in the same asm block (the quotient is rounded)
static __forceinline double atan2_div(double y, double x, float m) {
    double r;
#ifdef VP_GCC
    __asm__ volatile("fld %[y]\n\t"
                     "fld %[x]\n\t"
                     "fpatan\n\t"
                     "fdiv %[m]\n\t"
                     "fstp %[r]"
                     : [r] "=m"(r)
                     : [y] "m"(y), [x] "m"(x), [m] "m"(m)
                     : VP_X87_CLOBBERS, "cc");
#else
    __asm { fld y
            fld x
            fpatan
            fdiv m
            fstp r }
#endif
    return r;
}

// the name AIGetDriverNameByCar copies for a car: the lounge's driver for it (AIGetDriverForCar) at the current
// strength, its name pointer (+0x210) -- found the same way, by the same calls (the fix below)
static const char* driver_name_of(int car) {
    const int d = AIGetDriverForCar(car);
    const int s = AIGetStrength();
    void* l = g_lounge;
    const uint8_t* p = VFN(l, 4, const uint8_t*, int, int)(l, 0, s, d);   // AIDriverLounge::Get(strength, driver)
    return *(const char* const*)(p + 0x210);
}

// ==== AICar::begin_race (0x430250): the clocks start; the grid lane is remembered ================================
static void __fastcall AICar_begin_race(AICar* self, Edx) {
    if (!PhysReplayPlayMode()) {
        char name[32];                                  // [+4] (the name isn't used)
        const char* n;
        // FIX: AIGetDriverNameByCar strcpy's the driver's name, however long, into this 32-byte buffer: a name of 32
        // characters or more ran over the stack frame (and a NULL name faulted). Such a name is cut to 31
        // characters here instead (the name is looked up the same way first; an ordinary one is fetched as before).
        if (VP_FIX && (!(n = driver_name_of(self->car_index)) || strlen(n) >= sizeof name)) {
            name[0] = 0;
            if (n) {
                memcpy(name, n, sizeof name - 1);
                name[sizeof name - 1] = 0;
            }
        } else
            AIGetDriverNameByCar(name, self->car_index);
    }
    const double c = D(self->now) - self->skill->aftershock_time;
    COPY4(self->progress_time, self->now);
    COPY4(self->race_start_time, self->now);
    COPY4(self->start_lat, self->center_lat);
    self->learn_hiatus = 13;
    self->crash_time = (float)c;
    self->fouroff_time = (float)(D(self->now) - 12.0f);
}
static void fp_begin_race(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00430250, "AICar::begin_race", AICar_begin_race, fp_begin_race)

// ==== AICar::begin_lap (0x4302d0): the lap's target time, with a random spread ===================================
static void __fastcall AICar_begin_lap(AICar* self, Edx) {
    float* off = &self->lap_time_offset;                // edi
    if (self->lap == 0) {
        const AITrackInfo* ti = AIGetTrackInfo(AIGetTrackNumber());
        COPY4(*off, ti->first_lap_allowance);
    } else SET4(*off, 0);
    const double r = random_real_o((float)-D(self->lap_time_spread), Ub(self->lap_time_spread));
    const double s = r + *off;
    *off = (float)s;                                    // fst: the sum stays in the register
    const double t = s + self->base_lap_time;
    self->lap_target_time = (float)t;
    self->pace = (float)(t / self->line_lap_time);
}
static void fp_begin_lap(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x004302d0, "AICar::begin_lap", AICar_begin_lap, fp_begin_lap)

// ==== random_real (0x430350): a + (b - a) x Random(1000) / 1000 ===================================================
static double __cdecl random_real(float a, float b) {
    const int r = Random(1000);
    return (D(r) * k_milli) * (D(b) - a) + a;
}
static void fp_random_real(Footprint&, float, float) {}   // writes nothing; Random is an input
PORT_FN(0x00430350, "random_real", random_real, fp_random_real)

// ==== AICar::analyze (0x430380): learn the notes ==================================================================
// Marks the segment's sides off-road; once the learn hiatus is over, on entering a segment, a flagged previous
// segment has the speed notes of it and the 4 before lowered (0.622 m/s per step back, unless locked), and a
// flagged or off-road one has the lateral notes of 5 segments around it pushed (1/3 to 1 m by distance) away from
// the wall it hit, or the side it's on; a lateral note too far out (4 m, or 0.1 m against the push) locks.
// In learn mode, 3 laps without a change stop the race.
static void __fastcall AICar_analyze(AICar* self, Edx) {
    if (self->seg_valid) {
        if (off_road_o(&self->wheels[0]) || off_road_o(&self->wheels[2])) self->seginfo[self->seg_index].offroad_left = 1;
        if (off_road_o(&self->wheels[1]) || off_road_o(&self->wheels[3])) self->seginfo[self->seg_index].offroad_right = 1;
    }
    if (self->learn_hiatus > 0) {
        AICar__MSG(self, 0, msg_at(0x004ed424));
    } else if (self->seg_changed) {
        SegmentInfo* prev = AICar_GetRelSegment(self, 0, self->seg_index, -1);   // [+0x14]
        bool flagged = false;                           // bl
        if (prev->flag != 0) {
            for (int i = 0; i < 5; i++) {
                SegmentInfo* s = AICar_GetRelSegment2(self, 0, prev, -i);
                if (s->locked == 0) {
                    self->last_learn_lap = self->lap;
                    const int k = i + 1;                // [+0x10]
                    s->speed_note = (float)((D(k) * k_point2) * k_note_step + s->speed_note);
                }
            }
            flagged = true;
        }
        if (flagged || prev->offroad_left != 0 || prev->offroad_right != 0) {
            float sign = Fb(0x3f800000u);               // [+0x18]
            const int32_t side = flagged ? I(self->wall_hit_lat) : I(self->center_lat);
            if (side > 0) sign = Fb(0xbf800000u);
            for (int j = 0; j < 3; j++) {
                int rel = -j;                           // edi
                for (int e = -1; e < 2; e += 2, rel += 2 * j) {   // ebp: behind, then ahead (j > 0)
                    if (e > 0 && j == 0) continue;
                    SegmentInfo* s = AICar_GetRelSegment2(self, 0, prev, rel);
                    const double a = fabs(D(s->lateral_note));
                    if ((((Ub(sign) ^ Ub(s->lateral_note)) & 0x80000000u) && a > k_point1) || a > 4.0f) s->locked = 1;
                    if (s->locked != 0) break;
                    self->last_learn_lap = self->lap;
                    s->lateral_note = (float)((D(3 - j) * k_third) * sign + s->lateral_note);
                }
            }
        }
    }
    if (AILearnMode() && (int32_t)((uint32_t)self->lap - (uint32_t)self->last_learn_lap) > 3) {
        void* deity = g_deity;
        VFN(deity, 0x08, void)(deity, 0);               // StopRace
        AICar_suspend_adjustment(self, 0, (const char*)0x004ed634);
    }
}
// the car, its segment notes, and in learn mode the game state (Deity::StopRace -> SetGameState(3))
static void fp_seginfo(Footprint& f, AICar* self) {
    const int n = self->num_segs;
    if (self->seginfo && n > 0 && n < 100000) f.add(self->seginfo, (uint32_t)n * sizeof(SegmentInfo), "segment notes");
}
static void fp_analyze(Footprint& f, AICar* self, Edx) {
    f.object(self, "car");
    fp_seginfo(f, self);
    if (AILearnMode()) f.add(g_game_state, 4, "game state");
}
PORT_FN(0x00430380, "AICar::analyze", AICar_analyze, fp_analyze)

// ==== off_road (0x4305d0): the wheel's surface isn't the road =====================================================
static uint8_t __cdecl off_road(Wheel* w) { return w->surface != 0 ? 1 : 0; }
static void fp_off_road(Footprint& f, Wheel*) { f.pure = true; }
PORT_FN(0x004305d0, "off_road", off_road, fp_off_road)

// ==== AICar::decay (0x4305e0): the proxer levels and the steering damping fade ===================================
static void __fastcall AICar_decay(AICar* self, Edx) {
    for (int i = 0; i < self->num_proxer_info; i++) {
        ProxerInfo* p = &self->proxer_info[i];
        p->level = (float)(D(self->skill->proxer_decay) * p->level);
    }
    self->steer_damping = (float)(D(self->steer_damping) * k_point99);
}
static void fp_proxer_info(Footprint& f, AICar* self) {
    const int n = self->num_proxer_info;
    if (self->proxer_info && n > 0 && n < 4096) f.add(self->proxer_info, (uint32_t)n * sizeof(ProxerInfo), "proxer info");
}
static void fp_decay(Footprint& f, AICar* self, Edx) {
    f.object(self, "car");
    fp_proxer_info(f, self);
}
PORT_FN(0x004305e0, "AICar::decay", AICar_decay, fp_decay)

// ==== AICar::roll_dice (0x430630): three rolls against the skill's thresholds ====================================
static void __fastcall AICar_roll_dice(AICar* self, Edx) {
    uint8_t t = self->skill->cold_apex_threshold;
    uint8_t r = (uint8_t)Random(255);
    const Skill* sk = self->skill;
    self->dice_cold_apex = t < r ? 1 : 0;
    t = sk->hot_entry_threshold;
    r = (uint8_t)Random(255);
    sk = self->skill;
    self->dice_hot_entry = t < r ? 1 : 0;
    t = sk->dice3_threshold;
    r = (uint8_t)Random(255);
    self->dice_unused = t < r ? 1 : 0;
}
static void fp_roll_dice(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00430630, "AICar::roll_dice", AICar_roll_dice, fp_roll_dice)

// ==== AICar::check_for_lapchange (0x4306a0) ========================================================================
static void __fastcall AICar_check_for_lapchange(AICar* self, Edx) {
    const int32_t lap = *(const int32_t*)(race_record(self) + 0x6c);
    if (self->lap != lap) {
        AICar_begin_lap_o(self, 0);
        self->lap = lap;
    }
}
static void fp_check_for_lapchange(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x004306a0, "AICar::check_for_lapchange", AICar_check_for_lapchange, fp_check_for_lapchange)

// ==== AICar::DoneLowering (0x4306e0) ===============================================================================
static void __fastcall AICar_DoneLowering(AICar* self, Edx) {
    self->lowering = 0;
    AICar_fire_fouroff(self, 0);
}
static void fp_done_lowering(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x004306e0, "AICar::DoneLowering", AICar_DoneLowering, fp_done_lowering)

// ==== AICar::periodics (0x4306f0) ==================================================================================
static void __fastcall AICar_periodics(AICar* self, Edx) {
    if (((CountWheels_t)count_wheels_a)(off_road_a, self->wheels) == 4 && I(self->speed) < 0x41f8e38e)   // < 31.1 m/s
        AICar_fire_fouroff(self, 0);
    if ((self->tick & 0xf) == 0) AICar_check_for_too_fast(self, 0);
    AICar_check_for_lapchange_o(self, 0);
    if ((self->tick & 0x3f) == 0) {
        AICar_check_for_stranded(self, 0);
        AICar_check_for_damage(self, 0);
    }
    if (I(self->speed) < 0x410e38e4 && I(self->wall_heat) > 0x42480000) {   // below 8.9 m/s, heat past 50
        SET4(self->wall_heat, 0xc2c80000u);             // -100
        AICar_teleport_to_track(self, 0);
    } else self->wall_heat = (float)(D(self->wall_heat) * k_point992);
    if ((uint8_t)self->tick == 0) AICar_roll_dice_o(self, 0);
    if (I(self->too_fast) >= 0x3d4ccccd)
        AICar__MSG(self, 0, I(self->too_fast) < 0x3f000000 ? msg_at(0x004ed408) : msg_at(0x004ed444));
    self->tick = (self->tick + 1) & 0xffff;
}
// Would a teleport_to_track called now fire? (armed 2..5 s ago, and the game running.) Then the deity teleports
// the car to the line and the line's bead is reset: outside any footprint.
static bool fp_teleport_possible(const AICar* self, double now) {
    const double d = now - self->teleport_armed_time;
    const float df = (float)d;
    return d > 2.0f && I(df) < 0x40a00000 && GetGameState() == 1;
}
static void fp_periodics(Footprint& f, AICar* self, Edx) {
    f.object(self, "car");
    if (self->line) f.add(self->line, sizeof(IdealLine), "ideal line");   // (check_for_too_fast: a lost bead found)
    if (fp_teleport_possible(self, self->now)) f.replay_only = "a teleport_to_track may fire (the deity's TeleportToLine)";
}
PORT_FN(0x004306f0, "AICar::periodics", AICar_periodics, fp_periodics)

// ==== AICar::init_rt_lat (0x4307e0): the car against the centre line and its racing line ==========================
static void __fastcall AICar_init_rt_lat(AICar* self, Edx) {
    CenterLine* cl = *(CenterLine**)(race_record(self) + 0xa4);   // edi
    float w;                                            // [+8] the corridor's width at the centre-line bead
    if (ILSeg* s = cl->bead_seg) COPY4(w, s->corridor_half_width);
    else SET4(w, 0x41a00000u);                          // 20 m
#ifdef VP_GCC
    __asm__("" : "+m"(w));      // (w opaque: GCC would fold (20 - 1.905) * 0.5 at compile time, where the original's
                                // fsub rounds it to the thread's precision -- 24 bits on the physics thread)
#endif
    const float half = (float)(D(w) * 0.5f);            // [+0x10]
    const double n = (D(w) - k_car_width) * 0.5f;
    COPY4(self->track_half_width, half);
    COPY4(self->center_lat, cl->lat);
    self->center_lat_norm = (float)(D(self->center_lat) / n);
    COPY4(self->center_lat_delta, cl->lat_delta);
    // the centre-line point 0.7 s ahead (at least 25 m)
    const double dl = CenterLine_get_car_dlong_meters(cl, 0);
    const double v = D(self->speed) * k_point7;
    const double ahead = !(25.0f >= v) ? v : 25.0;      // test ah,1: max, a NaN stays
    const float m = cifmod_f(dl + ahead, cl->length);   // [+8]
    ILinePos p;                                         // [+0x1c]
    CenterLine_convert_meters_to_ilpos(cl, 0, &p, Ub(m));
    ILSeg* seg = p.seg;
    ILSeg* nx = seg->next;
    Point2D pt;                                         // [+0x24]
    HermiteEval_o(&pt, &seg->pos, &nx->pos, &seg->dir, &nx->dir, Ub(p.t));
    Point2D tan;                                        // [+0x14]
    ILSeg_QuickTan(seg, 0, &tan, Ub(p.t));
    self->center_recover_vel.x = (float)(D(tan.x) * k_60mph);
    self->center_recover_vel.z = (float)(D(tan.z) * k_60mph);
    COPY4(self->center_tan.x, tan.x);
    COPY4(self->center_tan.z, tan.z);
    self->center_right.z = (float)-D(tan.x);
    const float half5 = (float)(D(half) - 5.0f);        // [+0x10]
    COPY4(self->center_pos.x, pt.x);
    COPY4(self->center_pos.z, pt.z);
    COPY4(self->center_right.x, tan.z);
    float e;                                            // [+8] toward the car's own side
    if (I(self->center_lat) > 0) COPY4(e, half5);
    else e = (float)-D(half5);
    self->side_edge_offset.x = (float)(D(e) * tan.z);
    self->side_edge_offset.z = (float)(D(self->center_right.z) * e);
    self->edge_offset.x = (float)(D(tan.z) * half5);
    self->edge_offset.z = (float)(D(self->center_right.z) * half5);
    // the car against its racing line, at its bead
    ILinePos b;                                         // [+8]
    const ILinePos* bp = AICar_get_ilpos(self, 0, &b);
    // FIX: a lost (NULL) bead -- a stranded car's teleport, a NaN position -- was read (seg->next) and crashed: the
    // line puts it back at the point nearest the car (reset_bead_position) and it's read again; with no bead even
    // then, the racing-line offset stays as it was. (A car with no line at all -- a track without one -- is left as
    // the original has it.)
    if (VP_FIX && !bp->seg && self->line) {
        IdealLine_reset_bead_position(self->line, 0);
        bp = AICar_get_ilpos(self, 0, &b);
        if (!bp->seg) return;
    }
    ILSeg* bs = bp->seg;                                // [+0x1c]
    const uint32_t bt = Ub(bp->t);                      // [+0x20]
    ILSeg* bn = bs->next;
    HermiteEval_o(&pt, &bs->pos, &bn->pos, &bs->dir, &bn->dir, bt);
    ILSeg_QuickTan(bs, 0, &tan, bt);
    Point2D r;                                          // [+0x2c]
    COPY4(r.x, tan.x);
    COPY4(r.z, tan.z);
    Right_o(&r);
    const IdealLine* line = self->line;
    const float dx = (float)(D(line->car_pos.x) - pt.x);   // [+0x24]
    const double dz = D(line->car_pos.z) - pt.z;        // fst [+0x28]; the register goes on
    self->line_lat = (float)(dz * r.z + D(r.x) * dx);
}
// the car; its line and its centre line (only read, unless a lost bead is found again: reset_bead_position,
// get_car_dlong_meters); __CIfmod sets a CRT byte of its own
static void fp_init_rt_lat(Footprint& f, AICar* self, Edx) {
    f.object(self, "car");
    if (self->line) f.add(self->line, sizeof(IdealLine), "ideal line");
    if (g_race_deity_global)
        if (CenterLine* cl = *(CenterLine**)(race_record(self) + 0xa4)) f.add(cl, sizeof(CenterLine), "centre line");
}
PORT_FN(0x004307e0, "AICar::init_rt_lat", AICar_init_rt_lat, fp_init_rt_lat)

// ==== HermiteEval (0x430a70): the cubic Hermite point between a and b with tangents c and d at t =================
static void __cdecl HermiteEval(Point2D* out, const Point2D* a, const Point2D* b, const Point2D* c, const Point2D* d, float t) {
    const double h00 = ((D(t) * 2.0f - 3.0f) * t) * t + 1.0f;
    const double h01 = ((D(t) * -2.0f + 3.0f) * t) * t;
    const double h10 = ((D(t) - 2.0f) * t + 1.0f) * t;
    const double h11 = ((D(t) - 1.0f) * t) * t;
    out->x = (float)(((D(c->x) * h10 + D(b->x) * h01) + D(d->x) * h11) + D(a->x) * h00);
    // (out.x is stored before the z inputs are read)
    out->z = (float)(((h11 * d->z + h10 * c->z) + h01 * b->z) + h00 * a->z);
}
static void fp_hermite(Footprint& f, Point2D*, const Point2D*, const Point2D*, const Point2D*, const Point2D*, float) { f.pure = true; }
PORT_FN(0x00430a70, "HermiteEval(aicar.obj)", HermiteEval, fp_hermite)   // race.obj has its own HermiteEval (0x443c10)

// ==== Right (0x430b20): (x, z) -> (z, -x) ==========================================================================
static void __cdecl Right(Point2D* p) {
    const double x = p->x;                              // fld
    uint32_t z;
    memcpy(&z, &p->z, 4);
    p->z = (float)-x;
    memcpy(&p->x, &z, 4);
}
static void fp_right(Footprint& f, Point2D*) { f.pure = true; }
PORT_FN(0x00430b20, "Right", Right, fp_right)

// ==== AICar::crash_aftershock (0x430b40): steer with the wheel's kick, coast on light brakes =====================
static void __fastcall AICar_crash_aftershock(AICar* self, Edx) {
    AICar__MSG(self, 0, msg_at(0x004ed420));
    const double s = D(self->steer_feedback) + self->steering;
    float sf = (float)s;                                // [+4]
    if (!(s >= -1.0f)) SET4(sf, 0xbf800000u);           // test ah,1: below -1 or a NaN
    else if (I(sf) > 0x3f800000) SET4(sf, 0x3f800000u);
    Car_SetSteering(self, 0, Ub(sf));
    Car_SetThrottle(self, 0, 0);
    Car_SetBraking(self, 0, 0x3e99999au);               // 0.3
    AICar_fire_fouroff(self, 0);
}
static void fp_crash_aftershock(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00430b40, "AICar::crash_aftershock", AICar_crash_aftershock, fp_crash_aftershock)

// ==== AICar::obey_speed_limit (0x430bc0): throttle and brake against the target speed =============================
// Acts when finished, 8.9 m/s over, or over at all within 2 s of a braking point (unless the dice said enter
// hot). Finished: throttle 1 - 0.225 x over (-1..1; a negative one brakes instead). Else ('MPH') more than
// 6.7 m/s over: full brake and the skill's throttle; less: the skill's throttle -+ 0.15 x over.
static void __fastcall AICar_obey_speed_limit(AICar* self, Edx, float* throttle, float* brake, float*) {
    const ILSeg* seg = *(ILSeg* volatile*)&self->seginfo[self->seg_index].seg;   // read whatever the speed
    bool near_brk = false;                                  // cl
    if (I(self->speed) > 0x3ee38e39) {                  // 0.444 m/s
        const double v = ((D(1.0f) - self->seg_t) * seg->dist_ahead_signed + D(seg->next->dist_ahead_signed) * self->seg_t) /
                         self->speed;
        near_brk = !(v >= 2.0f);                            // test ah,1
    }
    const bool early = near_brk && !self->dice_hot_entry;   // dl
    const double over = D(self->speed) - self->target_speed;
    const float overf = (float)over;                    // [+8]
    const bool way_over = over > k_over_far;            // al (test ah,0x41; sete)
    const bool act = self->finished || way_over || (early && I(overf) > 0);   // bl
    if (!act) {
        if (near_brk && !early) AICar__MSG(self, 0, msg_at(0x004ed428));
        return;
    }
    if (self->finished) {
        const double x = D(overf) * k_m0225 + 1.0f;
        float xf = (float)x;                            // [+8]
        if (!(x >= -1.0f)) SET4(xf, 0xbf800000u);
        else if (I(xf) > 0x3f800000) SET4(xf, 0x3f800000u);
        COPY4(*throttle, xf);
        if (Ub(xf) > 0x80000000u) {                     // negative: brake instead
            *brake = (float)-D(xf);
            SET4(*throttle, 0);
        }
        return;
    }
    AICar__MSG(self, 0, msg_at(0x004ed42c));
    const double over2 = D(self->speed) - self->target_speed;
    const float o2 = (float)over2;                      // [+8]
    if (over2 > k_over_hard) {                          // test ah,0x41
        SET4(*brake, 0x3f800000u);
        COPY4(*throttle, self->skill->overspeed_throttle);
        return;
    }
    const double k = D(o2) * k_015;
    *brake = (float)(D(self->skill->overspeed_throttle) + k);
    *throttle = (float)(D(self->skill->overspeed_throttle) - k);
}
static void fp_obey_speed_limit(Footprint& f, AICar* self, Edx, float* throttle, float* brake, float*) {
    f.object(self, "car");
    f.add(throttle, 4, "throttle");
    f.add(brake, 4, "brake");
}
PORT_FN(0x00430bc0, "AICar::obey_speed_limit", AICar_obey_speed_limit, fp_obey_speed_limit)

// ==== AICar::socialize (0x430d90): the (up to) three nearest cars ==================================================
static void __fastcall AICar_socialize(AICar* self, Edx) {
    self->contact_multiple = 0;
    self->contact_valid = 0;
    self->side_squeezed = 0;
    self->side_valid = 0;
    self->ahead_valid = 0;
    int n = g_proxer->num_objects - 1;
    if (n >= 3) n = 3;
    for (int i = 0; i < n; i++) {
        const Proxer* P = g_proxer;
        const uint32_t off = (uint32_t)P->record_size * (uint32_t)self->car_index + ((uint32_t)i + (uint32_t)P->num_objects * 2u) * 4u;
        const ProxerDelta* d = *(const ProxerDelta* const*)(P->records + off + 0x10);
        if (!d) break;
        if (((Interact_t)self->interact_fn)(self, 0, d)) AICar_suspend_adjustment(self, 0, (const char*)0x004ed644);
    }
}
// the car (fast_interact and what it calls write only the car)
static void fp_socialize(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00430d90, "AICar::socialize", AICar_socialize, fp_socialize)

// ==== AICar::rev_engine (0x430e20): blipping the throttle on the grid ============================================
static void __fastcall AICar_rev_engine(AICar* self, Edx) {
    if (Ub(self->engine_throttle) != 0x3f800000u) {
        const int r = Random(100);
        Car_SetThrottle(self, 0, r < 30 ? 0x3f800000u : 0u);
    } else {
        const int r = Random(100);
        Car_SetThrottle(self, 0, r < 50 ? 0x3f800000u : 0u);
    }
}
static void fp_rev_engine(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00430e20, "AICar::rev_engine", AICar_rev_engine, fp_rev_engine)

// ==== AICar::apply_yaw_control (0x430e80): Car's, on 0.93 of the steering ========================================
static void __fastcall AICar_apply_yaw_control(AICar* self, Edx, float* const brakes) {
    const int mode = self->yaw_control;
    if (mode == 0) return;
    const P3& v = self->velocity;
    const double speed = x87_sqrt((D(v.y) * v.y + D(v.z) * v.z) + D(v.x) * v.x);
    float wy;                                           // [+0xc] the yaw rate, an integer copy
    COPY4(wy, self->angular_velocity.y);
    const double steer = (D(self->steering) * k_093) * self->max_steer_angle;
    const double wheelbase = D(self->front_axle_z) - self->rear_axle_z;
    const float target = (float)(yaw_tan_ratio(steer, wheelbase) * speed);   // [+0]
    if (!(speed > k_yaw_min)) return;                   // test ah,0x41
    float g_in, g_out;                                  // [+4], [+8]
    if (mode == 1) { g_in = 0.5f; g_out = 5.0f; }
    else { g_in = 5.0f; g_out = 5.0f; }
    const float d = (float)(D(target) - wy);            // [+0]
    if (I(wy) > 0) {
        if (I(d) > 0) {
            const double x = D(d) * g_in;
            brakes[3] = (float)(!(D(brakes[3]) >= x) ? x : D(brakes[3]));
        } else {
            const double x = -(D(d) * g_out);
            brakes[0] = (float)(!(D(brakes[0]) >= x) ? x : D(brakes[0]));
        }
    } else {
        const double e_r = -D(d);
        const float e = (float)e_r;
        if (!(e_r > 0.0f)) {                            // test ah,0x41
            const double x = -(D(e) * g_out);
            brakes[1] = (float)(!(D(brakes[1]) >= x) ? x : D(brakes[1]));
        } else {
            const double x = D(e) * g_in;
            brakes[2] = (float)(!(D(brakes[2]) >= x) ? x : D(brakes[2]));
        }
    }
}
static void fp_ai_yaw_control(Footprint& f, AICar* self, Edx, float* const brakes) {
    f.object(self, "car");
    f.add(brakes, 16, "brakes");
}
PORT_FN(0x00430e80, "AICar::apply_yaw_control", AICar_apply_yaw_control, fp_ai_yaw_control)

// ==== AICar::Update (0x431010) =====================================================================================
static void __fastcall AICar_Update(AICar* self, Edx) {
    i_pr_overhead_begin();
    const int prof = i_prof_start((const char*)0x004ed64c);
    i_pr_overhead_end();
    if (self->race_state < 2) AICar_rev_engine_o(self, 0);
    else {
        AICar_init_rtinfo(self, 0);
        const Skill* sk = self->skill;                  // ebp
        bool think;                                     // [+0x13]: every think_ticks-th tick
        const int n = sk->think_ticks;
        if (n == 0) think = true;
        else think = (uint32_t)(g_physics_tick % n) - (uint32_t)n == 0xffffffffu;
        const bool aftershock = !((D(self->now) - sk->aftershock_time) >= self->crash_time);                // [+0x12]
        const bool freeze = !((D(self->now) - (D(sk->recovery_time) * 2.0f + 2.0f)) >= self->freeze_time); // bl
        const bool bump = !((D(self->now) - sk->bump_tc_off_time) >= self->bump_time);                      // [+0x11]
        if (think) self->num_msgs = 0;
        AICar_decay_o(self, 0);
        if (!self->race_begun) {
            AICar_begin_race_o(self, 0);
            self->race_begun = 1;
        }
        AICar_periodics_o(self, 0);
        if (bump) {
            AICar__MSG(self, 0, msg_at(0x004ed418));
            AICar_set_traction_control(self, 0, 0);
            AICar_set_yaw_control(self, 0, 0);
        } else {
            AICar_set_traction_control(self, 0, 1);
            const uint8_t y = fabs(D(self->steering)) > k_yaw_steer ? 1 : 0;   // test ah,0x41; sete
            AICar_set_yaw_control(self, 0, y);
        }
        AICar_update_line_info(self, 0);
        AICar_update_segment_info(self, 0);
        AICar_socialize_o(self, 0);
        if (think && !freeze) {
            AICar_get_real_target(self, 0);
            ((Method0_t)self->drive_fn)(self, 0);
        }
        if (aftershock) AICar_crash_aftershock_o(self, 0);
        if (freeze) AICar__MSG(self, 0, msg_at(0x004ed41c));
        AICar_analyze_o(self, 0);
    }
    i_pr_overhead_begin();
    i_prof_stop(prof);
    i_pr_overhead_end();
    Car_Update(self, 0);
}
// Car::Update's reach beyond the car, as phys_car_update.cpp's fp_car_volumes / fp_car_models list it: the body's
// collision volumes and group spheres (CollideGround), and the five live models' vertices and model_info
// +0x10/+0x15 (a queued impulse over 741.67 dents the car through ActuallyApplyDamage). The dents are physics
// state -- the wheels' hub vertices point into the LOD-0 mesh -- so a shadow check that left them out ran its
// rewrite pass on the mesh the original pass had already dented (damage_grid, damaged and frame mismatches).
static void fp_car_reach(Footprint& f, AICar* self) {
    const uint8_t* c = (const uint8_t*)self;
    const int32_t nvol = *(const int32_t*)(c + 0x24);                     // PhobRoot volumes[4] (+0x14), count
    for (int i = 0; i < nvol && i < 4; i++) {
        uint8_t* v = ((uint8_t* const*)(c + 0x14))[i];
        if (!v) continue;
        f.object(v, "volume");
        if (*(const uint32_t*)(v + 0x14) == 0x47525550u) {                // 'GRUP': a SphereGroupVolume's spheres
            const int n = *(const int32_t*)(v + 0x4c);
            for (int k = 0; k < n && k < 12; k++)
                if (void* s = ((void* const*)(v + 0x1c))[k]) f.object(s, "body sphere");
        }
    }
    const int32_t* live_models = (const int32_t*)(c + 0x4c4);             // Car +0x4c4 [5]
    for (int i = 0; i < 5; i++) {
        uint8_t* mi = (uint8_t*)(uintptr_t)live_models[i];
        if (!mi) continue;
        f.add(mi + 0x10, 8, "model_info");
        const uint8_t* info = *(const uint8_t* const*)(mi + 0x10);         // mrModelInfo: count, vertices (32 bytes)
        if (!info) continue;
        const int32_t n = *(const int32_t*)info;
        void* verts = *(void* const*)(info + 4);
        if (verts && n > 0) f.add(verts, (uint32_t)n * 32, "model verts");
    }
}
// The car; its segment notes, proxer levels and racing line (IdealLine::update_car_info and the bead); in learn
// mode the game state; and Car::Update's (phys_car_update.cpp): the collision volumes, the live models, the splash
// Sound3D and the deity's per-car record with its CenterLine. Left to the replay: a teleport (teleport_to_track
// firing: the deity's TeleportToLine), and Car::Update's own cases -- out of bounds, the damage reset, a deity that
// isn't a RaceDeity.
static void fp_ai_update(Footprint& f, AICar* self, Edx) {
    f.object(self, "car");
    fp_car_reach(f, self);
    fp_seginfo(f, self);
    fp_proxer_info(f, self);
    if (self->line) f.add(self->line, sizeof(IdealLine), "ideal line");
    if (AILearnMode()) f.add(g_game_state, 4, "game state");
    if (uint8_t* snd = g_splash_sound) f.add(snd + 8, 0x25, "splash Sound3D");
    uint8_t* deity = (uint8_t*)g_deity;
    if (!deity || *(uint32_t*)deity != VT_RaceDeity) { f.replay_only = "the deity isn't a RaceDeity (size unknown)"; return; }
    f.add(deity, 2012, "deity");
    if ((uint32_t)self->car_index < 16u) {
        uint8_t* info = deity + 0x38 + 0x74 * self->car_index;
        if (uint8_t* line = *(uint8_t**)(info + 0x6c)) f.add(line, 112, "centre line");
    }
    if (self->race_state >= 2 && fp_teleport_possible(self, PhysicsGetTime())) {
        f.replay_only = "a teleport_to_track may fire (the deity's TeleportToLine)";
        return;
    }
    const P3& p = *(const P3*)((const uint8_t*)self + 0x5c);   // Car::Update's out-of-bounds teleport (frame.pos)
    if (((uint8_t(__cdecl*)(const P3*))0x00438660)(&p)) {  // loc_is_out_of_bounds
        f.replay_only = "out of bounds: Car::Update teleports through the deity";
        return;
    }
    const uint8_t damaged = *((const uint8_t*)self + 0x48c);
    const float last_damage = *(const float*)((const uint8_t*)self + 0x488);
    if (!((Flag_t)0x0042bd60)() && damaged && !((D(last_damage) + 2.0f) >= PhysicsGetTime()))
        f.replay_only = "Car::Update's reset_damage rebuilds the models";
}
PORT_FN(0x00431010, "AICar::Update", AICar_Update, fp_ai_update)

// ==== AICar::headon_panic (0x4311d0): freeze, lock the brakes, steer hard to its own side =========================
// vrmod's headon.py (opt-in) makes the function's first byte a ret: no panic at all. On a race.exe carrying it the
// rewrite does the same (docs/FIXES.md, "vrmod's patches"); otherwise the stock panic.
static void __fastcall AICar_headon_panic(AICar* self, Edx) {
    if (port_vrmod_has(0x004311d0)) return;             // headon.py: `ret` in place of `sub esp, 4`
    COPY4(self->freeze_time, self->now);
    Car_SetBraking(self, 0, 0x3dcccccdu);               // 0.1
    Car_SetEBrake(self, 0, 0x3f800000u);
    Car_SetThrottle(self, 0, 0);
    Car_SetClutch(self, 0, 0);
    Car_SetSteering(self, 0, I(self->center_lat) > 0 ? 0x3f800000u : 0xbf800000u);
}
static void fp_headon_panic(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x004311d0, "AICar::headon_panic", AICar_headon_panic, fp_headon_panic)

// ==== AICar::in_path (0x431240): the nearest bead to p on the car's line, within max_dist of its own ==============
static uint8_t __fastcall AICar_in_path(AICar* self, Edx, const Point2D* p, uint32_t max_dist, ILinePos* pos, float* dist) {
    if (!self->seg_valid) return 0;
    pos->seg = self->seginfo[self->seg_index].seg;
    COPY4(pos->t, self->seg_t);
    const double r = IdealLine_get_nearest_bead(self->line, 0, p, pos, max_dist);
    *dist = (float)r;
    return pos->seg != 0 ? 1 : 0;
}
// its outputs (get_nearest_bead writes only the position it's given)
static void fp_in_path(Footprint& f, AICar* self, Edx, const Point2D*, uint32_t, ILinePos* pos, float* dist) {
    f.object(self, "car");
    f.add(pos, sizeof(ILinePos), "position");
    f.add(dist, 4, "distance");
}
PORT_FN(0x00431240, "AICar::in_path", AICar_in_path, fp_in_path)

// ==== AICar::fast_interact (0x4312a0) ==============================================================================
static uint8_t __fastcall AICar_fast_interact(AICar* self, Edx, const ProxerDelta* d) {
    if (AICar_checker_o(self, 0, d)) return 1;
    if (AICar_passer_o(self, 0, d)) return 1;
    return 0;
}
static void fp_fast_interact(Footprint& f, AICar* self, Edx, const ProxerDelta*) { f.object(self, "car"); }
PORT_FN(0x004312a0, "AICar::fast_interact", AICar_fast_interact, fp_fast_interact)

// ==== AICar::checker (0x4312d0): a car beside (dont_check) or right ahead (dont_push), above 8.9 m/s ==============
static uint8_t __fastcall AICar_checker(AICar* self, Edx, const ProxerDelta* d) {
    if (I(self->speed) < 0x410e38e4) return 0;
    Point2D dl, dv;                                     // [+0x40], [+0x2c]: the other car, from me
    ProxerDelta_GetMyDL(d, 0, self->car_index, &dl);
    ProxerDelta_GetMyDV(d, 0, self->car_index, &dv);
    // my heading: the velocity's direction
    float vx;                                           // [+0x18]
    COPY4(vx, self->velocity.x);
    const float vz = fpu_copy(self->velocity.z);        // [+0x1c] (fld; fst)
    const double len = x87_sqrt(D(vz) * vz + D(vx) * vx);
    const float lenf = (float)len;                      // [+0x20]
    float dirx, dirz;                                   // [+0x10], [+0x14]
    COPY4(dirx, vx);
    COPY4(dirz, vz);
    if (fabs(len) > k_eps) {                            // test ah,0x41
        dirx = (float)(D(vx) / lenf);
        dirz = (float)(D(vz) / lenf);
    }
    Point2D r;                                          // [+0x24]
    COPY4(r.x, dirx);
    COPY4(r.z, dirz);
    Right_o(&r);
    const float lon = (float)(D(dirz) * dl.z + D(dirx) * dl.x);   // [+0xc]
    const float lat = (float)(D(r.z) * dl.z + D(r.x) * dl.x);     // [+0x38]
    const double alon = fabs(D(lon));
    const float alonf = (float)alon;                    // [+0x20]
    const bool lon_near = !(alon >= k_check_lon);       // cl (test ah,1)
    const double alat = fabs(D(lat));
    const float alatf = (float)alat;                    // [+0x3c]
    const bool lat_near = !(alat >= k_check_lat) && I(alatf) > 0x3fe7a5e3;   // dl: 1.8176 .. 4.572 m
    uint8_t ret = 0;
    if (lat_near && lon_near) {
        const float lat_v = (float)(D(r.z) * dv.z + D(r.x) * dv.x);   // [+0x18]
        const float lon_v = (float)(D(dirz) * dv.z + D(dirx) * dv.x); // [+0x34]
        const bool at_edge = fabs(D(self->center_lat_norm)) > k_point7;   // al
        const bool behind = Ub(lon) > 0xbf6a3d71u;      // cl: behind me by more than 0.915 m (or a negative NaN)
        bool i_lead;                                    // bl
        if (Ub(lon) > 0xc00f0d84u) i_lead = true;       // behind by more than 2.235 m
        else if (!behind) i_lead = false;
        else i_lead = !at_edge;
        int other = d->car_a;
        if (self->car_index == other) other = d->car_b;
        const bool human = IsAI(other) == 0;            // cmp al,1; sbb; neg
        const bool well_back = Ub(lon) > 0xc08f0d84u ? I(lon_v) < 0x40471c72 : false;   // 4.47 m back, not gaining 3.1 m/s
        if (i_lead && human && !well_back) i_lead = false;
        AICar_dont_check_o(self, 0, Ub(lat), Ub(lat_v), Ub(lon), Ub(lon_v), i_lead ? 1 : 0);
        ret = 1;
    }
    const float lon_v2 = (float)(D(dirz) * dv.z + D(dirx) * dv.x);   // [+0x18]
    const bool lat_close = I(alatf) < 0x401e7ef9;       // 2.4765 m
    if (I(alonf) < 0x418b79fb && lat_close && I(lon) > 0) {   // within 17.4 m ahead
        AICar_dont_push_o(self, 0, Ub(lon), Ub(lon_v2));
        ret = 1;
    }
    return ret;
}
static void fp_checker(Footprint& f, AICar* self, Edx, const ProxerDelta*) { f.object(self, "car"); }
PORT_FN(0x004312d0, "AICar::checker", AICar_checker, fp_checker)

// ==== AICar::passer (0x431560): a slower car on my line ahead -> a contact to pass (or a head-on panic) ==========
static uint8_t __fastcall AICar_passer(AICar* self, Edx, const ProxerDelta* d) {
    uint8_t ret = 0;                                    // [+0x13]
    int other = d->car_a;
    if (self->car_index == other) other = d->car_b;
    const Proxer* P = g_proxer;
    const float* rec = (const float*)(P->records + (uint32_t)P->record_size * (uint32_t)other);   // ebx: position, then velocity
    ILinePos pos;                                       // [+0x24]
    pos.seg = 0;
    SET4(pos.t, 0);
    float dist;                                         // [+0x40]
    SET4(dist, 0);
    if (!AICar_in_path_o(self, 0, (const Point2D*)rec, (float)(D(self->speed) * 4.0f), &pos, &dist)) return ret;
    // the other car against the line at the bead nearest it
    ILSeg* seg = pos.seg;
    ILSeg* nx = seg->next;
    Point2D pt;                                         // [+0x5c]
    HermiteEval_o(&pt, &seg->pos, &nx->pos, &seg->dir, &nx->dir, Ub(pos.t));
    Point2D tan;                                        // [+0x50]
    ILSeg_QuickTan(pos.seg, 0, &tan, Ub(pos.t));
    Point2D r;                                          // [+0x34]
    COPY4(r.x, tan.x);
    COPY4(r.z, tan.z);
    Right_o(&r);
    const float lat_other = (float)((D(rec[1]) - pt.z) * r.z + (D(rec[0]) - pt.x) * r.x);   // [+0x48]
    const float lon_speed = (float)(D(rec[2]) * tan.x + D(rec[3]) * tan.z);               // [+0x4c]
    const float lat_speed = (float)(D(rec[2]) * r.x + D(rec[3]) * r.z);                   // [+0x58]
    const float closing = (float)(D(self->speed) - lon_speed);                            // [+0x44]
    Point2D dl;                                         // [+0x2c]
    ProxerDelta_GetMyDL(d, 0, self->car_index, &dl);
    float vx;                                           // [+0x14]
    COPY4(vx, self->velocity.x);
    const float vz = fpu_copy(self->velocity.z);        // [+0x18]
    const double len = x87_sqrt(D(vz) * vz + D(vx) * vx);
    const float lenf = (float)len;                      // [+0x3c]
    float dirx, dirz;                                   // [+0x1c], [+0x20]
    COPY4(dirx, vx);
    COPY4(dirz, vz);
    if (fabs(len) > k_eps) {
        dirx = (float)(D(vx) / lenf);
        dirz = (float)(D(vz) / lenf);
    }
    const double ahead = D(dirz) * dl.z + D(dirx) * dl.x;
    const bool in_front = ahead > k_pass_ahead;         // al (test ah,0x41; sete)
    if (I(closing) <= 0x3faaaaab || !in_front) return ret;   // closing at 1.33 m/s or more
    const double tt = D(dist) / closing;
    const float tf = (float)tt;                         // [+0x14] seconds to contact
    const float pred = (float)(tt * lat_speed + lat_other);   // [+0x1c] its offset from the line then
    const ILSeg* s = pos.seg;
    const ILSeg* sn = s->next;
    const double ll = (D(sn->lateral) - s->lateral) * pos.t + s->lateral;
    const float llf = (float)ll;                        // [+0x2c] the line's offset from the centre there
    if (fabs(ll + lat_other) >= self->track_half_width) return ret;   // test ah,1: off the road (a NaN goes on)
    if (Ub(lon_speed) > 0xc1b1c71cu) AICar_headon_panic_o(self, 0);      // coming at me faster than 22.2 m/s
    else {
        if (I(tf) <= 0) return ret;
        AICar_add_contact_o(self, 0, Ub(tf), Ub(pred), (float)(D(pred) + llf));
    }
    ret = 1;
    return ret;
}
static void fp_passer(Footprint& f, AICar* self, Edx, const ProxerDelta*) { f.object(self, "car"); }
PORT_FN(0x00431560, "AICar::passer", AICar_passer, fp_passer)

// ==== AICar::dont_push (0x431800): the nearest car ahead ===========================================================
static void __fastcall AICar_dont_push(AICar* self, Edx, uint32_t dist, uint32_t rel) {
    bool take = true;
    if (self->ahead_valid && !(D(self->ahead_dist) >= D(Fb(dist)))) take = false;   // test ah,1
    if (take) {
        memcpy(&self->ahead_dist, &dist, 4);
        memcpy(&self->ahead_rel_speed, &rel, 4);
        self->ahead_valid = 1;
    }
}
static void fp_dont_push(Footprint& f, AICar* self, Edx, uint32_t, uint32_t) { f.object(self, "car"); }
PORT_FN(0x00431800, "AICar::dont_push", AICar_dont_push, fp_dont_push)

// ==== AICar::dont_check (0x431840): the most urgent car beside; one on each side is a squeeze =====================
static void __fastcall AICar_dont_check(AICar* self, Edx, uint32_t lat, uint32_t lat_v, uint32_t lon, uint32_t lon_v, uint8_t i_lead) {
    bool take;
    if (!self->side_valid) take = true;
    else if ((Ub(self->side_lat) ^ lat) & 0x80000000u) {
        self->side_squeezed = 1;
        take = false;
    } else {
        const bool faster = fabs(D(Fb(lat_v))) > fabs(D(self->side_lat_rel_speed));   // fcompp; test ah,0x41
        if (!i_lead) take = self->side_i_lead != 0 || faster;
        else take = self->side_i_lead != 0 && faster;
    }
    if (take) {
        memcpy(&self->side_lat, &lat, 4);
        memcpy(&self->side_lat_rel_speed, &lat_v, 4);
        memcpy(&self->side_lon, &lon, 4);
        memcpy(&self->side_lon_rel_speed, &lon_v, 4);
        self->side_i_lead = i_lead;
        self->side_valid = 1;
    }
}
static void fp_dont_check(Footprint& f, AICar* self, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t) { f.object(self, "car"); }
PORT_FN(0x00431840, "AICar::dont_check", AICar_dont_check, fp_dont_check)

// ==== AICar::add_contact (0x431900): the soonest contact ===========================================================
static void __fastcall AICar_add_contact(AICar* self, Edx, uint32_t time, uint32_t line_lat, uint32_t center_lat) {
    bool take = true;
    if (self->contact_valid) {
        self->contact_multiple = 1;
        take = D(self->contact_time) > D(Fb(time));     // test ah,0x41; sete
    }
    if (take) {
        memcpy(&self->contact_time, &time, 4);
        memcpy(&self->contact_center_lat, &center_lat, 4);
        self->contact_valid = 1;
        memcpy(&self->contact_line_lat, &line_lat, 4);
    }
}
static void fp_add_contact(Footprint& f, AICar* self, Edx, uint32_t, uint32_t, uint32_t) { f.object(self, "car"); }
PORT_FN(0x00431900, "AICar::add_contact", AICar_add_contact, fp_add_contact)

// ==== AICar::slow_interact (0x431950) ==============================================================================
static uint8_t __fastcall AICar_slow_interact(void*, Edx, const void*) { return 0; }
static void fp_slow_interact(Footprint& f, void*, Edx, const void*) { f.pure = true; }
PORT_FN(0x00431950, "AICar::slow_interact", AICar_slow_interact, fp_slow_interact)

// ==== AICar::lonslam (0x431970): brake for the car ahead ===========================================================
// Closer than 5.14 m (or not closing measurably): brake at least 0.9. Else, with t = (dist - 4.92 m) / closing
// speed, brake at least 0.4 x (2.5 - t), 0..1.
static void __fastcall AICar_lonslam(AICar* self, Edx, float* brake) {
    if (!self->ahead_valid) return;
    if (I(self->ahead_dist) >= 0x40a482be && fabs(D(self->ahead_rel_speed)) > k_rel_min) {
        const double q = (D(self->ahead_dist) - k_gap) / self->ahead_rel_speed;
        const bool far_off = I(self->ahead_dist) > 0x408f0d84;   // (always, past the 5.14 m test)
        const float t = (float)-q;                      // [+0]
        if (!far_off) return;
        if (I(t) <= 0) return;
#ifdef VP_GCC
        const double k = (D(2.5f) - t) * (float)0.4f;   // (GCC reads a bare 0.4f as the long double 0.4L: the cast
                                                        //  gives the float the original multiplies by)
#else
        const double k = (D(2.5f) - t) * 0.4f;
#endif
        float kf = (float)k;
        if (!(k >= 0.0f)) SET4(kf, 0);                  // test ah,1
        else if (I(kf) > 0x3f800000) SET4(kf, 0x3f800000u);
        const double b = *brake;
        *brake = (float)(!(D(kf) >= b) ? b : D(kf));
        AICar__MSG(self, 0, msg_at(0x004ed45c));
        return;
    }
    const double b = *brake;
    *brake = (float)(!(D(k_point9) >= b) ? b : D(k_point9));
    AICar__MSG(self, 0, msg_at(0x004ed45c));
}
static void fp_lonslam(Footprint& f, AICar* self, Edx, float* brake) {
    f.object(self, "car");
    f.add(brake, 4, "brake");
}
PORT_FN(0x00431970, "AICar::lonslam", AICar_lonslam, fp_lonslam)

// ==== AICar::latslam (0x431a90): steer away from (or brake for) the car beside ====================================
static void __fastcall AICar_latslam(AICar* self, Edx, float* brake, float* steer) {
    if (self->side_squeezed) {                          // one on each side: brake at least 0.8
        const double b = *brake;
        *brake = (float)(!(D(k_point8) >= b) ? b : D(k_point8));
        self->slam_steer = (float)(D(self->slam_steer) * k_point96);
        AICar__MSG(self, 0, msg_at(0x004ed458));
    } else if (!self->side_valid || self->side_i_lead) {
        self->slam_steer = (float)(D(self->slam_steer) * k_point992);
    } else {
        const float away = (float)-D(self->side_lat);   // [+0xc]
        if ((Ub(away) ^ Ub(self->slam_steer)) & 0x80000000u) SET4(self->slam_steer, 0);
        // pinned against the edge by a car outside me, drifting outward: brake 0.6
        if (Ub(self->side_lon_rel_speed) <= 0xc0d55555u &&
            !((Ub(self->center_lat_delta) ^ Ub(self->center_lat)) & 0x80000000u) &&
            fabs(D(self->center_lat)) > D(self->track_half_width) - k_edge_margin &&
            ((Ub(self->side_lat) ^ Ub(self->center_lat_norm)) & 0x80000000u)) {
            SET4(*brake, 0x3f19999au);
            AICar__MSG(self, 0, msg_at(0x004ed460));
            self->slam_steer = (float)(D(self->slam_steer) * k_point97);
        }
        bool act;                                       // al
        if ((Ub(self->side_lat_rel_speed) ^ Ub(self->side_lat)) & 0x80000000u) act = true;   // closing
        else act = !(fabs(D(self->side_lat)) >= k_side_lat) && !(fabs(D(self->side_lon)) >= k_side_lon);
        SET4(self->latslam_rel_speed, 0);
        if (!act) AICar__MSG(self, 0, msg_at(0x004ed44c));
        else {
            const double a = fabs(D(self->side_lat_rel_speed)) - k_slam_v0;
            const float af = (float)a;                  // [+0x10]
            const double g = a * k_slam_k;
            float gf = (float)g;                        // [+0xc]
            if (!(g >= 0.0f)) SET4(gf, 0x3f000000u);    // test ah,1
            else if (I(gf) > 0x3f800000) SET4(gf, 0x40800000u);
            else gf = (float)((D(af) * k_slam_mk + 1.0f) * 0.5f + D(af) * k_slam_k4);
            COPY4(self->latslam_rel_speed, self->side_lat_rel_speed);
            const double inc = I(self->side_lat) > 0 ? D(gf) * k_slam_step_m : D(gf) * k_slam_step;
            self->slam_steer = (float)(inc + self->slam_steer);
            AICar__MSG(self, 0, I(self->slam_steer) > 0 ? msg_at(0x004ed450) : msg_at(0x004ed454));
        }
    }
    *steer = (float)(D(*steer) + self->slam_steer);
}
static void fp_latslam(Footprint& f, AICar* self, Edx, float* brake, float* steer) {
    f.object(self, "car");
    f.add(brake, 4, "brake");
    f.add(steer, 4, "steer");
}
PORT_FN(0x00431a90, "AICar::latslam", AICar_latslam, fp_latslam)

// ==== AICar::fast_drive (0x431ce0): steer at the target, obey the speed, mind the neighbours =====================
static void __fastcall AICar_fast_drive(AICar* self, Edx) {
    float steer;                                        // [+0x10]
    SET4(steer, 0);
    float throttle;                                     // [+0x28]
    SET4(throttle, 0x3f800000u);
    float brake;                                        // [+0x14]
    SET4(brake, 0);
    const double tf = self->too_fast;
    const float ebrake = (float)(D(k_point6) > tf ? tf : D(k_point6));   // [+0x2c] test ah,0x41: min, a NaN gives 0.6
    // a cold apex: lift within 1 s of the braking point, if the dice said so
    bool cold = false;
    if (I(self->speed) > 0x3ee38e39) {
        const ILSeg* a = self->seginfo[self->seg_index].seg;
        const ILSeg* b = self->seginfo[self->next_seg_index].seg;
        const double v = ((D(1.0f) - self->seg_t) * a->dist_ahead + D(b->dist_ahead) * self->seg_t) / self->speed;
        if (!(v >= 1.0f)) cold = self->dice_cold_apex != 0;   // test ah,1
    }
    if (cold) {
        throttle = (float)(D(self->engine_throttle) * k_point96);
        AICar__MSG(self, 0, msg_at(0x004ed430));
    }
    // steer: the target's bearing over the steering lock, or full lock toward it when it's behind
    if (I(self->target_vel_dir.z) > 0) {
        Point2D a;                                      // [+8]
        const Point2D* ap = &self->target_pos_dir;      // edi
        const double l0 = x87_sqrt(D(ap->z) * ap->z + D(ap->x) * ap->x);
        if (fabs(l0 - 1.0f) > k_eps) {                  // not unit length: normalise a copy
            memcpy(&a, &self->target_pos_dir, 8);
            const double l = x87_sqrt(D(a.z) * a.z + D(a.x) * a.x);
            a.x = (float)(D(a.x) / l);
            a.z = (float)(D(a.z) / l);
            ap = &a;
        }
        // (the forward vector (0, 1) [+0x18] is normalised the same way when |sqrt(1.0) - 1| > 2^-23: never. Left
        // out.)
        volatile float fwd_x = 0.0f, fwd_z = 1.0f;      // in memory, as the original's
        const double cross = D(fwd_z) * ap->x - D(ap->z) * fwd_x;
        const double dot = D(ap->z) * fwd_z + D(fwd_x) * ap->x;
        steer = (float)(atan2_div(cross, dot, self->max_steer_angle) * self->steer_gain);
    } else SET4(steer, I(self->target_pos.x) > 0 ? 0x3f800000u : 0xbf800000u);
    AICar_obey_speed_limit_o(self, 0, &throttle, &brake, &steer);
    // after the flag: the throttle cap ramps from 1 to 0.2 over 30 s
    if (self->finished) {
        const double el = D(self->now) - self->finish_time;
        const float elf = (float)el;                    // [+8]
        const double r = el * k_30th;
        float rf = (float)r;                            // [+0x18]
        if (!(r >= 0.0f)) SET4(rf, 0x3f800000u);        // test ah,1
        else if (I(rf) > 0x3f800000) SET4(rf, 0x3e4ccccdu);
        else rf = (float)((D(elf) * k_m30th + 1.0f) + D(elf) * k_150th);
        COPY4(self->throttle_cap, rf);
    }
    AICar_latslam_o(self, 0, &brake, &steer);
    AICar_lonslam_o(self, 0, &brake);
    const double mb = self->min_brake, b = brake;
    brake = (float)(!(b >= mb) ? mb : b);               // test ah,1: max, a NaN gives min_brake
    AICar_AISetSteering(self, 0, Ub(steer));
    AICar_AISetThrottle(self, 0, Ub(throttle));
    AICar_AISetBrake(self, 0, Ub(brake));
    Car_SetGearAutoAI(self, 0);
    Car_SetEBrake(self, 0, Ub(ebrake));
    Car_SetClutch(self, 0, 0x3f800000u);
}
static void fp_fast_drive(Footprint& f, AICar* self, Edx) { f.object(self, "car"); }
PORT_FN(0x00431ce0, "AICar::fast_drive", AICar_fast_drive, fp_fast_drive)

// ==== the small virtuals compiled into aicar.obj, and ProxerInfo's constructor ====================================
static int __fastcall Car_GetMessageSize(void*, Edx) { return 0x18c; }
static void fp_get_message_size(Footprint& f, void*, Edx) { f.pure = true; }
PORT_FN(0x00432060, "Car::GetMessageSize", Car_GetMessageSize, fp_get_message_size)

static uint8_t __fastcall Car_IsSolid(CarOpacity* self, Edx) { return I(self->opacity) >= 0x3f7d70a4 ? 1 : 0; }   // opacity >= 0.99
static void fp_is_solid(Footprint& f, CarOpacity*, Edx) { f.pure = true; }
PORT_FN(0x00432070, "Car::IsSolid", Car_IsSolid, fp_is_solid)

static int __fastcall AICar_GetReplayPacketSize(void*, Edx) { return 0x48; }
static void fp_replay_packet_size(Footprint& f, void*, Edx) { f.pure = true; }
PORT_FN(0x00432080, "AICar::GetReplayPacketSize", AICar_GetReplayPacketSize, fp_replay_packet_size)

static ProxerInfo* __fastcall ProxerInfo_ProxerInfo(ProxerInfo* self, Edx) {
    SET4(self->level, 0);
    return self;
}
static void fp_proxer_info_ctor(Footprint& f, ProxerInfo* self, Edx) { f.add(self, 4, "ProxerInfo"); }
PORT_FN(0x00432090, "AICar::ProxerInfo::ProxerInfo", ProxerInfo_ProxerInfo, fp_proxer_info_ctor)
