// phys_replay.cpp -- M3 3.6 group T4: the game's own replay (physics:physrepl.obj) and the ghost lap car
// (physics:ghostcar.obj), rewritten.
//
// This is the replay the player watches after a race, not our recorder (replay.cpp). While a race runs,
// PhysReplayRecordUpdate stores one ReplayFrame every 6 physics ticks (about 10.4 a second) into a 2 MB ring:
//
//   ReplayFrame (at g_data + g_frames[i]):
//     +0x00 u32  telemetry dlong (PhysicsTelemetry +0, the deity's GetDLong)
//     +0x04 u8   telemetry lap   (PhysicsTelemetry +4, GetCurrentLap)
//     +0x05 u16  speed  over -300..300 (x 32768 / 600)
//     +0x07 u16  lat g  over -4..4     (x 32768 / 8)
//     +0x09 u16  long g over -4..4
//     +0x0b      each registered phob's packet at +0x0b + phob+0x34 (RegisterPhob hands out the offsets; the
//                sizes are GetReplayPacketSize: PhobDyno 0x1e, Car 0x3c, GhostCar 0x3d, ...)
//     +frame_size  the event stream: {u8 type, payload}..., closed by a 0 byte (add_separator_event); at most
//                0x600 bytes of events a frame (PhysReplayAddEvent drops, and logs every 3 s, past that)
//
// g_frames (int[nframes], -1 = empty) holds each frame slot's byte offset; nframes = min(2 MB / frame_size,
// 18750). Frame slot = (tick / 6) % nframes. The data runs on from frame to frame and wraps to 0 with the frame
// index, or when fewer than 0x600 bytes are left (advance_replay_frame). SealRecording (after the race) closes
// the last frame and, if the ring wrapped, walks back to the oldest frame still intact: g_num_frames and the
// start time. Playback (PhysReplayPlayUpdate, physics thread) moves each dynamic phob between the frame at the
// tick and the next (UpdateReplay at t = (tick % 6) / 6), rebuilds the telemetry, and dispatches the frame's
// events once through the handler table (8 slots, by type; a handler returns its payload size) in the current
// mode (0 play, 1 fast forward, 2 rewind). The UI (main thread) moves physics_tick: Rewind, FastForward,
// Shuttle, Pause/Resume. A replay file is a 7-dword header {'LPER', 6, start_time, num_frames, frame_size,
// nframes, data_used}, the frame table, then data_used bytes.
//
// The ghost: GhostNewBestLap (race.obj) copies a new best lap's packets out of the ring (feed_ghost_car, up to
// 2400 of them) into the ghost record and tells the GhostCar (NewLap); at the next lap the ghost starts over
// with them (init_from), drawn at 0.3 opacity, not solid, following the replay packets through UpdateGhost.
//
//   GhostData (the ghost record +0x2344c; GhostGetData, GhostNewBestLap):
//     +0 lap_ticks  +4 time_offset (ticks into the lap the packets start)  +8 count (packets)
//   GhostCar (LocalCar 0xec8 + 0x38 = 3840 bytes, vtable 0x4dba48):
//     +0xec8 follow_car   the car whose lap is ghosted (PhobData +0x1e0)
//     +0xecc car_type     the ghost's car (GhostGetData's int; packet +0x3c; message +0x18c)
//     +0xed0 speed        |velocity| between the two packets (message +0x4c)
//     +0xed4 last_lap     -1 after reset
//     +0xed8 lap_ticks    +0xedc lap_start (physics_tick at the lap change)  +0xee0 time_offset
//     +0xee4 best_ticks   the best lap time offered so far (new_lap takes only a better one)
//     +0xee8 pending_type +0xeec pending_data +0xef0 pending_packets   (new_lap: taken at the next lap)
//     +0xef4 packets      2400 x 60-byte Car packets (MemAlloc 0x23280)  +0xef8 count
//     +0xefc prev_flags   PhobRoot +4 as of the last UpdateCommon (bit 1 visible, bit 2 drawn)
//
// Skipped: the $E static initialisers (38 in physrepl.obj, 34 in ghostcar.obj; $E59 sets the 18750-frame
// cap), ASSERT_MSG (0x42d0a0, a bare `ret`: kept, called as the original calls it) and GhostCar's scalar
// deleting destructor (0x42e380).
//
// Threads: the physics thread records (RecordUpdate, AddEvent, advance/record_telemetry/add_separator), plays
// (PlayUpdate, update_replay) and runs the ghost; PhysTaskBegin/End/PostRace (physics_thread too, but before
// the shadow framework knows the thread) set up and seal (BGBegin, RegisterPhob, DoneCreating, BGEnd,
// SealRecording), so every footprint here lists the statics it writes. The replay screen (main thread) calls
// PlayBegin/End, Rewind, FastForward, Shuttle, Pause, Resume, IsPaused, IsAtEnd, Load, Save, GetTelemetry.
//
// Written from the v1.0 disassembly: register values as double, stored values as float, the same grouping,
// integer compares of float bits done on the bits, float copies made with integer moves kept as bit copies,
// and every call -- this file's own functions included -- made by address or through the vtable exactly as
// the original does. Nothing is inlined that the original calls.
#include <intrin.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                                  // a register value (x87.h)
#define FB(b) __builtin_bit_cast(float, (uint32_t)(b))       // a float constant from its bits
typedef int Edx;                                            // the unused edx of a __thiscall received as __fastcall

namespace {
static inline uint32_t Ubits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static inline float Fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint16_t rd16(const uint8_t* p) { uint16_t w; memcpy(&w, p, 2); return w; }
static inline float rdf(const uint8_t* p) { float f; memcpy(&f, p, 4); return f; }

// ---- layouts ----------------------------------------------------------------------------------------------------
struct PhysicsTelemetry {             // PhysTaskGetTelemetry (0x520c58); PlayCar::Update fills it
    uint32_t dlong;                   // +0x00 the deity's GetDLong
    int32_t lap;                      // +0x04 GetCurrentLap
    float speed;                      // +0x08
    float lat_g;                      // +0x0c
    float long_g;                     // +0x10
};
static_assert(sizeof(PhysicsTelemetry) == 20, "PhysicsTelemetry");
struct ReplayFrame { uint8_t b[11]; };  // the telemetry header; the packets and events follow (see the top)
struct GhostData { int32_t lap_ticks, time_offset, count; };

struct GhostCarL : PhobRoot {         // a GhostCar; the Car / LocalCar part by offset (phys_car.cpp)
    uint8_t _06c[0xec8 - 0x6c];
    int32_t follow_car;               // +0xec8
    int32_t car_type;                 // +0xecc
    float speed;                      // +0xed0
    int32_t last_lap;                 // +0xed4
    int32_t lap_ticks;                // +0xed8
    int32_t lap_start;                // +0xedc
    int32_t time_offset;              // +0xee0
    int32_t best_ticks;               // +0xee4
    int32_t pending_type;             // +0xee8
    const GhostData* pending_data;    // +0xeec
    const uint8_t* pending_packets;   // +0xef0
    uint8_t* packets;                 // +0xef4
    int32_t count;                    // +0xef8
    uint32_t prev_flags;              // +0xefc
};
static_assert(offsetof(GhostCarL, follow_car) == 0xec8 && offsetof(GhostCarL, speed) == 0xed0 &&
              offsetof(GhostCarL, best_ticks) == 0xee4 && offsetof(GhostCarL, packets) == 0xef4 &&
              offsetof(GhostCarL, prev_flags) == 0xefc && sizeof(GhostCarL) == 3840, "GhostCarL");
// a Car, for its inline stubs (compiled into ghostcar.obj) and footprints: 3768 bytes
struct CarL : PhobRoot { uint8_t _06c[3768 - 0x6c]; };
static_assert(sizeof(CarL) == 3768, "CarL");

// the Car fields used here (phys_car.cpp's Car)
template <typename T> static inline T& at(void* o, uint32_t off) { return *(T*)((uint8_t*)o + off); }
enum : uint32_t {
    C_BODY_VOLUME = 0x478, C_LIVE_MODELS = 0x4c4, C_OPACITY = 0x50c, C_CAR_INDEX = 0x510, C_SMOOTH_THROTTLE = 0xc34,
    C_PERCEIVED_RPM = 0xd70, C_THROTTLE = 0xde0, C_LOWERING = 0xe3d, C_SOUNDS = 0xe4c, C_WHEELS = 0x554,
};
static inline uint32_t& flags(void* o) { return ((PhobRoot*)o)->_004; }          // PhobRoot +4: the replay flags
static inline uint32_t msg_offset(const void* o) { return ((const PhobRoot*)o)->_008; }
static inline int32_t packet_offset(const void* o) { return (int32_t)((const PhobRoot*)o)->_028[3]; }   // +0x34

// ---- the statics (physrepl.obj: 0x5218d0..0x52197b in .bss, 0x4ed168..0x4ed177 in .data) --------------------------
#define g_cur_frame       (*(int32_t*)0x005218d0)          // the frame slot recorded or played last; -1 none
#define g_event_mode      (*(int32_t*)0x005218d8)          // ReplayEventMode: 0 play, 1 fast forward, 2 rewind
#define g_have_recording  (*(uint8_t*)0x005218dc)
#define g_data_used       (*(int32_t*)0x005218e4)          // bytes of the data buffer in use (the file's size)
#define g_play_mode       (*(uint8_t*)0x005218e8)
#define g_wrapped         (*(uint8_t*)0x005218fc)          // the ring has wrapped
#define g_nframes         (*(int32_t*)0x00521900)          // frame slots
#define g_wraps           (*(int32_t*)0x00521904)          // times round the ring
#define g_max_frames      (*(int32_t*)0x00521908)          // 18750 ($E59)
#define g_frames          (*(int32_t**)0x00521918)         // per slot, the frame's byte offset in g_data; -1 empty
#define g_ev_cursor       (*(int32_t*)0x00521920)          // where the next event goes, from the frame's start
#define g_data            (*(uint8_t**)0x00521924)         // 2 MB
#define g_handlers        ((void**)0x00521930)             // 8 event handlers, by type
#define g_start_time      (*(int32_t*)0x00521950)          // the physics tick of the first frame
#define g_wrap_mark       (*(int32_t*)0x00521954)          // g_wraps when the data last ran out of room; -1
#define g_num_frames      (*(int32_t*)0x0052195c)
#define g_frame_size      (*(int32_t*)0x00521978)          // 0xb + the packets
#define g_single          (*(int32_t*)0x004ed168)          // SingleBegin("PhysReplay")
#define g_last_frame      (*(int32_t*)0x004ed16c)          // the frame whose events were dispatched last; -1
#define g_last_drop_log   (*(int32_t*)0x004ed174)          // PTimeNow of the last "Too many events"
#define g_physics_tick    (*(volatile int32_t*)0x0052161c)
#define g_physics_paused  (*(uint8_t*)0x00521608)
#define g_ghost_global    (*(GhostCarL**)0x0052197c)       // GhostCar::global
#define g_deity           (*(void**)0x005218ac)
#define g_race_deity      (*(uint8_t**)0x004edd70)         // RaceDeity::global
#define g_ghost_record    (*(uint8_t**)0x00505bc8)         // GhostGetData's record
#define g_sound_manager   (*(uint8_t**)0x004f5708)
static const uint32_t REPL_COOKIE = 0x5245504c;              // 'LPER'
static const int32_t DATA_BYTES = 0x200000;

// the game's strings (the original passes these addresses)
static const char* const S_PHYSREPLAY = (const char*)0x004ed1d0;     // "PhysReplay"
static const char* const S_FRAME_IDX = (const char*)0x004ed1b0;      // "frame_idx[%d] == -1 (rw == %d)"
static const char* const S_NO_HEADER = (const char*)0x004ed1dc;      // "Can't read header"
static const char* const S_COOKIE = (const char*)0x004ed1f0;         // "Bad replay cookie"
static const char* const S_VERSION = (const char*)0x004ed204;        // "Bad file version %x, expecting %x"
static const char* const S_IDX_METHOD = (const char*)0x004ed228;     // "Bad replay version (idx calculation...)"
static const char* const S_DATASIZE = (const char*)0x004ed25c;       // "Bad replay version (datasize changed)"
static const char* const S_FRAMES = (const char*)0x004ed284;         // "Bad replay frames: %d"
static const char* const S_PACKET_SIZE = (const char*)0x004ed29c;    // "replay: base packet size mismatch"
static const char* const S_NO_BUFFER = (const char*)0x004ed2c0;      // "Can't read replay buffer"
static const char* const S_TOO_MANY = (const char*)0x004ed308;       // "Too many events, dropping."
static const char* const S_ONE_HOOK = (const char*)0x004ed324;       // the original assert's: multiple hooks not implemented
static const char* const S_NO_HANDLER = (const char*)0x004ed354;     // "...couldn't find handler 0x%x!"

// ---- constants (the original's .rdata, by their bits) --------------------------------------------------------------
static constexpr float K_62_5 = FB(0x4279ffff);             // 62.499996: ticks a second
static constexpr float K_SIXTH = FB(0x3e2aaaab);            // 1/6
static constexpr float K_300 = FB(0x43960000);
static constexpr float K_M300 = FB(0xc3960000);
static constexpr float K_4 = FB(0x40800000);
static constexpr float K_M4 = FB(0xc0800000);
static constexpr float K_Q15 = FB(0x38000000);              // 2^-15
static constexpr float K_32768 = FB(0x47000000);
static constexpr float K_ONE = FB(0x3f800000);
static constexpr uint32_t K_GHOST_OPACITY = 0x3e99999a;     // 0.3
static const double K_HALF = 0.5;                           // a double constant (fadd qword)

// ---- the game's functions these call --------------------------------------------------------------------------------
typedef int32_t(__cdecl* SingleBegin_t)(const char*);
typedef void(__cdecl* Single_t)(int32_t, const char*, int32_t);
typedef void(__cdecl* V_t)();
typedef uint8_t(__cdecl* B_t)();
typedef int32_t(__cdecl* I_t)();
typedef void(__cdecl* VI_t)(int32_t);
typedef uint8_t(__cdecl* FileIO_t)(int32_t, void*, int32_t);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef void(__cdecl* Assert_t)(int32_t, const char*, ...);
typedef void*(__cdecl* MemAlloc_t)(int32_t);
typedef void(__cdecl* Delete_t)(void*);
typedef PhysicsTelemetry*(__cdecl* GetTel_t)();
typedef void*(__cdecl* FindCar_t)(int32_t);
typedef uint8_t(__cdecl* GhostGetData_t)(GhostData*, uint8_t**, int32_t*);
typedef void*(__fastcall* LocalCarCtor_t)(void*, Edx, void*, void*);
typedef void(__fastcall* Obj_t)(void*, Edx);
typedef void(__fastcall* CarPkt_t)(void*, Edx, uint8_t*);
typedef void(__fastcall* CarUpdateReplay_t)(void*, Edx, const uint8_t*, const uint8_t*, uint32_t);
typedef void(__fastcall* CarDamage_t)(void*, Edx, const P3*, const P3*, uint32_t, uint32_t);
// this file's own functions, by address (a rewrite hooked there is what runs)
typedef int32_t(__cdecl* I1_t)(int32_t);
typedef int32_t(__cdecl* I2_t)(int32_t, int32_t);
typedef double(__cdecl* Deltat_t)(int32_t);                                      // ST0
typedef void(__cdecl* UpdateReplay_t)(const ReplayFrame*, const ReplayFrame*, uint32_t);
typedef void(__cdecl* GetRT_t)(PhysicsTelemetry*, const ReplayFrame*, const ReplayFrame*, uint32_t);
typedef void(__cdecl* RecTel_t)(ReplayFrame*);
typedef int32_t(__cdecl* Handler_t)(const void*, int32_t);
typedef void(__fastcall* UpdateGhost_t)(GhostCarL*, Edx, const uint8_t*, const uint8_t*, uint32_t);
typedef void(__fastcall* GhostData_t)(GhostCarL*, Edx, const GhostData*, const uint8_t*, int32_t);
typedef void(__cdecl* UpdateGhostCar_t)(GhostCarL*, uint8_t*, int32_t, int32_t);

static const SingleBegin_t SingleBegin = (SingleBegin_t)0x00414f30;
static const Single_t SingleEnd = (Single_t)0x00415090;                 // _SingleEnd
static const Single_t SingleEnter = (Single_t)0x00415000;               // _SingleEnter
static const Single_t SingleLeave = (Single_t)0x00415070;               // _SingleLeave
static const V_t PhysicsPause = (V_t)0x0042bcc0;
static const V_t PhysicsUnpause = (V_t)0x0042bcf0;
static const B_t PhysicsIsPaused = (B_t)0x0042bd20;
static const V_t SoundMuteCars = (V_t)0x00471dd0;
static const V_t SoundUnMuteCars = (V_t)0x00471de0;
static const VI_t SoundStopCar = (VI_t)0x00471e20;
static const FileIO_t FileReadExact = (FileIO_t)0x004118b0;
static const FileIO_t FileWrite = (FileIO_t)0x00411a30;
static const Log_t LogReport = (Log_t)0x00411150;
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Assert_t ASSERT_MSG = (Assert_t)0x0042d0a0;                // a bare `ret`: kept, as the original calls it
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
static const Delete_t op_delete = (Delete_t)0x00414390;
static const GetTel_t PhysTaskGetTelemetry = (GetTel_t)0x00428d80;
static const I_t PTimeNow = (I_t)0x00413b40;
static const FindCar_t PhysTaskFindCar = (FindCar_t)0x00426d40;
static const GhostGetData_t GhostGetData = (GhostGetData_t)0x0040eb80;
static const LocalCarCtor_t LocalCar_ctor = (LocalCarCtor_t)0x004444e0;
static const Obj_t LocalCar_dtor = (Obj_t)0x00444540;
static const Obj_t ReplayPacket_ctor = (Obj_t)0x0040eef0;              // Car::ReplayPacket::ReplayPacket (mov eax, ecx)
static const Obj_t Car_Reset = (Obj_t)0x00439f90;
static const Obj_t Car_UpdateCommon = (Obj_t)0x004391b0;
static const CarPkt_t Car_GetMessage = (CarPkt_t)0x00438ef0;
static const CarPkt_t Car_MakeReplayPacket = (CarPkt_t)0x00438cd0;
static const CarUpdateReplay_t Car_UpdateReplay = (CarUpdateReplay_t)0x00438950;
static const CarDamage_t Car_ActuallyApplyDamage = (CarDamage_t)0x00436b40;
static const Obj_t IdealLine_reset_to_head = (Obj_t)0x00421510;

static const V_t PhysReplayRewind = (V_t)0x0042ca70;
static const I1_t convert_phystime2frame = (I1_t)0x0042cb60;
static const I1_t convert_frame2phystime = (I1_t)0x0042cbd0;
static const I2_t next_replay_frame = (I2_t)0x0042ce30;
static const Deltat_t convert_phystime2deltat = (Deltat_t)0x0042d0b0;
static const UpdateReplay_t update_replay = (UpdateReplay_t)0x0042d0e0;
static const GetRT_t get_replay_telemetry = (GetRT_t)0x0042d100;
static const B_t PhysReplayPlayMode = (B_t)0x0042d340;
static const I2_t prev_replay_frame = (I2_t)0x0042d520;
static const V_t add_separator_event = (V_t)0x0042d540;
static const RecTel_t record_telemetry = (RecTel_t)0x0042d640;
static const VI_t advance_replay_frame = (VI_t)0x0042d7b0;
static const UpdateGhostCar_t update_ghost_car = (UpdateGhostCar_t)0x0042db20;
static const Obj_t GhostCar_reset_o = (Obj_t)0x0042df30;
static const UpdateGhost_t GhostCar_UpdateGhost_o = (UpdateGhost_t)0x0042e000;
static const GhostData_t GhostCar_init_from_o = (GhostData_t)0x0042e0e0;
static const GhostData_t GhostCar_new_lap_o = (GhostData_t)0x0042e140;

// the handlers the game installs (the addresses, as registered), for PlayUpdate's footprint, with their sizes
static const uint32_t H_CRASH = 0x0043c6c0, H_SPLASH = 0x0043c6a0, H_AI = 0x0042e5c0, H_DAMAGE = 0x004374e0,
                      H_RESET = 0x00437640;
static const uint32_t VT_GHOSTCAR = 0x004dba48, VT_RACEDEITY = 0x004dc588;

// ---- footprint helpers ------------------------------------------------------------------------------------------------
// the statics this file writes, listed rather than relying on the thread's (see Threads, above)
static void fp_statics(Footprint& f) {
    f.add((void*)0x005218d0, 172, "physrepl.obj statics");
    f.add((void*)0x004ed168, 16, "physrepl.obj statics (.data)");
}
// a Car's parts outside the object that Car::UpdateReplay / reset_damage / ActuallyApplyDamage write (phys_car.cpp's
// fp_models and fp_body_volume): the five live models' vertices and model_info bytes, the body SphereGroupVolume and
// its spheres
// PhobRoot::MakeReplayPacket's atan2 (the CRT's ctrandisp2) leaves its double in a CRT static, and may set errno
static void fp_crt_scratch(Footprint& f) {
    f.add((void*)0x005d5600, 9, "the CRT's transcendental scratch (atan2 in PhobRoot::MakeReplayPacket)");
    f.add((void*)0x00502730, 4, "errno");
}
static bool is_car_vtable(uint32_t vt) {
    // Car, LocalCar, PlayCar, AICar, NetCar, GhostCar (state_layout.inc)
    return vt == 0x004dbfd0 || vt == 0x004dc5f0 || vt == 0x004dc238 || vt == 0x004dbc28 || vt == 0x004dc2e8 ||
           vt == VT_GHOSTCAR;
}
static void fp_car_parts(Footprint& f, void* car) {
    for (int i = 0; i < 5; i++) {
        uint8_t* mi = (uint8_t*)(uintptr_t)at<int32_t>(car, C_LIVE_MODELS + 4 * i);
        if (!mi) continue;
        f.add(mi + 0x10, 8, "model_info");
        const uint8_t* info = *(uint8_t**)(mi + 0x10);        // mrModelInfo {num_verts, verts, ...}
        if (!info) continue;
        const int32_t nv = *(const int32_t*)info;
        void* verts = *(void**)(info + 4);
        if (verts && nv > 0) f.add(verts, (uint32_t)nv * 32, "model verts");
    }
    uint8_t* g = at<uint8_t*>(car, C_BODY_VOLUME);             // SphereGroupVolume: spheres[12] at +0x1c, count +0x4c
    if (!g) return;
    f.object(g, "body volume");
    const int32_t ns = *(int32_t*)(g + 0x4c);
    for (int i = 0; i < ns && i < 12; i++)
        if (void* s = *(void**)(g + 0x1c + 4 * i)) f.object(s, "body sphere");
}
static void fp_car_all(Footprint& f, void* car) {
    f.object(car, "car");
    fp_car_parts(f, car);
}
// the ghost's packet buffer, as init_from fills it from a GhostData
static void fp_ghost_packets(Footprint& f, GhostCarL* g, const GhostData* gd) {
    if (!gd || !g->packets) return;
    const uint32_t bytes = (((uint32_t)gd->count * 60u) >> 2) << 2;
    if (bytes) f.add(g->packets, bytes, "ghost packets");
}
// RaceDeity::UpdateCar's writes (as phys_car_update.cpp lists them): the deity and the car's centre line
static bool fp_update_car(Footprint& f, int32_t car_index) {
    uint8_t* deity = (uint8_t*)g_deity;
    if (!deity || *(uint32_t*)deity != VT_RACEDEITY) { f.replay_only = "the deity isn't a RaceDeity (size unknown)"; return false; }
    f.add(deity, 2012, "deity");
    if ((uint32_t)car_index < 16u) {
        uint8_t* info = deity + 0x38 + 0x74 * car_index;
        if (uint8_t* line = *(uint8_t**)(info + 0x6c)) f.add(line, 112, "centre line");
    }
    return true;
}
}  // namespace

// =====================================================================================================================
// The replay UI (main thread)
// =====================================================================================================================

// ---- PhysReplayPlayBegin (0x42ca10): register the single-thread check; play mode; rewind if there's a recording ----
static void __cdecl PhysReplayPlayBegin_rw() {
    g_single = SingleBegin(S_PHYSREPLAY);
    g_play_mode = 1;
    g_event_mode = 0;
    if (g_have_recording) PhysReplayRewind();              // (a tail jump)
}
static void fp_play_begin(Footprint& f) { f.replay_only = "SingleBegin registers \"PhysReplay\" (twice would panic)"; }
PORT_FN(0x0042ca10, "PhysReplayPlayBegin", PhysReplayPlayBegin_rw, fp_play_begin)

// ---- PhysReplayPlayEnd (0x42ca50) ----------------------------------------------------------------------------------
static void __cdecl PhysReplayPlayEnd_rw() {
    PhysicsPause();
    SingleEnd(g_single, 0, 0);
}
static void fp_play_end(Footprint& f) {
    f.add(&g_physics_paused, 1, "physics_paused");
    f.add((void*)(uintptr_t)(0x0050874c + 8 * (uint32_t)g_single), 4, "the single-thread table");   // _SingleEnd
}
PORT_FN(0x0042ca50, "PhysReplayPlayEnd", PhysReplayPlayEnd_rw, fp_play_end)

// ---- PhysReplayRewind (0x42ca70): rewind mode, back to the first frame's tick ----------------------------------------
static void __cdecl PhysReplayRewind_rw() {
    g_event_mode = 2;
    SingleEnter(g_single, 0, 0);
    g_physics_tick = g_start_time;
    SingleLeave(g_single, 0, 0);
}
static void fp_tick_and_mode(Footprint& f) {
    fp_statics(f);
    f.add((void*)&g_physics_tick, 4, "physics_tick");
}
PORT_FN(0x0042ca70, "PhysReplayRewind", PhysReplayRewind_rw, fp_tick_and_mode)

// ---- PhysReplayPause / Resume (0x42cab0 / 0x42cae0) -------------------------------------------------------------------
static void __cdecl PhysReplayPause_rw() {
    SingleEnter(g_single, 0, 0);
    PhysicsPause();
    SoundMuteCars();
    SingleLeave(g_single, 0, 0);
}
static void fp_pause(Footprint& f) {
    fp_statics(f);
    f.add(&g_physics_paused, 1, "physics_paused");
    if (uint8_t* sm = g_sound_manager) f.add(sm + 1, 1, "SoundManager: cars muted");
}
PORT_FN(0x0042cab0, "PhysReplayPause", PhysReplayPause_rw, fp_pause)

static void __cdecl PhysReplayResume_rw() {
    g_event_mode = 0;
    SingleEnter(g_single, 0, 0);
    PhysicsUnpause();
    SoundUnMuteCars();
    SingleLeave(g_single, 0, 0);
}
PORT_FN(0x0042cae0, "PhysReplayResume", PhysReplayResume_rw, fp_pause)

// ---- PhysReplayIsPaused (0x42cb20): a tail jump to PhysicsIsPaused ------------------------------------------------------
static uint8_t __cdecl PhysReplayIsPaused_rw() { return PhysicsIsPaused(); }
static void fp_nothing(Footprint&) {}
PORT_FN(0x0042cb20, "PhysReplayIsPaused", PhysReplayIsPaused_rw, fp_nothing)

// ---- PhysReplayIsAtEnd (0x42cb30): is the tick in the frame recorded last? ------------------------------------------
static uint8_t __cdecl PhysReplayIsAtEnd_rw() {
    const int32_t f = convert_phystime2frame(g_physics_tick);
    return (uint8_t)((uint32_t)f - (uint32_t)g_cur_frame == 0);
}
PORT_FN(0x0042cb30, "PhysReplayIsAtEnd", PhysReplayIsAtEnd_rw, fp_nothing)

// ---- convert_phystime2frame (0x42cb60): (t / 6) % nframes ------------------------------------------------------------
static int32_t __cdecl convert_phystime2frame_rw(int32_t t) { return (t / 6) % g_nframes; }
static void fp_i1(Footprint&, int32_t) {}
PORT_FN(0x0042cb60, "convert_phystime2frame", convert_phystime2frame_rw, fp_i1)

// ---- PhysReplayFastForward (0x42cb80): fast-forward mode, to the last frame's tick -------------------------------------
static void __cdecl PhysReplayFastForward_rw() {
    g_event_mode = 1;
    SingleEnter(g_single, 0, 0);
    const int32_t t = convert_frame2phystime(g_num_frames - 1);
    g_physics_tick = (int32_t)((uint32_t)t + (uint32_t)g_start_time);
    SingleLeave(g_single, 0, 0);
}
PORT_FN(0x0042cb80, "PhysReplayFastForward", PhysReplayFastForward_rw, fp_tick_and_mode)

// ---- convert_frame2phystime (0x42cbd0): frame x 6 --------------------------------------------------------------------
static int32_t __cdecl convert_frame2phystime_rw(int32_t f) { return (int32_t)((uint32_t)f * 6u); }
static void fp_pure_i1(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x0042cbd0, "convert_frame2phystime", convert_frame2phystime_rw, fp_pure_i1)

// ---- PhysReplayShuttle (0x42cbe0): the jog wheel: move the tick by s seconds, held within the recording ----------------
// (the direction from the float's bits: an integer compare, so -0 and negative NaNs rewind)
static void __cdecl PhysReplayShuttle_rw(uint32_t s_bits) {
    g_event_mode = (int32_t)s_bits > 0 ? 1 : 2;
    SingleEnter(g_single, 0, 0);
    const float s = Fbits(s_bits);
    const int32_t d = x87_ftol(D(s) * D(K_62_5));
    g_physics_tick = (int32_t)((uint32_t)g_physics_tick + (uint32_t)d);
    if (g_start_time > g_physics_tick) g_physics_tick = g_start_time;
    const int32_t e = convert_frame2phystime(g_num_frames - 1);
    if ((int32_t)((uint32_t)g_start_time + (uint32_t)e) < g_physics_tick) {
        const int32_t e2 = convert_frame2phystime(g_num_frames - 1);
        g_physics_tick = (int32_t)((uint32_t)e2 + (uint32_t)g_start_time);
    }
    SingleLeave(g_single, 0, 0);
}
static void fp_shuttle(Footprint& f, uint32_t) { fp_tick_and_mode(f); }
PORT_FN(0x0042cbe0, "PhysReplayShuttle", PhysReplayShuttle_rw, fp_shuttle)

// ---- PhysReplayLoad (0x42cc80): read a replay file into the buffer (made for the same track and cars) --------------
static uint8_t __cdecl PhysReplayLoad_rw(int32_t fd) {
    int32_t h[7];                                          // {cookie, version, start, frames, frame size, nframes, bytes}
    SingleEnter(g_single, 0, 0);
    SingleLeave(g_single, 0, 0);
    PhysicsPause();
    g_event_mode = 0;
    if (!FileReadExact(fd, h, 0x1c)) { LogReport(S_NO_HEADER); return 0; }
    if ((uint32_t)h[0] != REPL_COOKIE) { LogReport(S_COOKIE); return 0; }
    if (h[1] != 6) { LogReport(S_VERSION, h[1], 6); return 0; }
    if (h[5] != g_nframes) { LogReport(S_IDX_METHOD); return 0; }
    if (h[6] > DATA_BYTES) { LogReport(S_DATASIZE); return 0; }
    g_start_time = h[2];
    g_num_frames = h[3];
    const int32_t last = h[3] - 1;
    g_cur_frame = next_replay_frame(convert_phystime2frame(h[2]), last);
    const int32_t n = g_nframes;
    if (h[3] < 0 || h[3] > n) { LogReport(S_FRAMES, h[3]); return 0; }
    if (g_frame_size != h[4]) {
        LogReport(S_PACKET_SIZE);
    } else if (FileReadExact(fd, g_frames, n << 2) && FileReadExact(fd, g_data, h[6])) {
        g_have_recording = 1;
        PhysReplayRewind();
        return 1;
    }
    LogReport(S_NO_BUFFER);
    return 0;
}
static void fp_load(Footprint& f, int32_t) { f.replay_only = "reads the replay file"; }
PORT_FN(0x0042cc80, "PhysReplayLoad", PhysReplayLoad_rw, fp_load)

// ---- next_replay_frame (0x42ce30): (f + n) % nframes -----------------------------------------------------------------
static int32_t __cdecl next_replay_frame_rw(int32_t f, int32_t n) { return (int32_t)((uint32_t)f + (uint32_t)n) % g_nframes; }
static void fp_i2(Footprint&, int32_t, int32_t) {}
PORT_FN(0x0042ce30, "next_replay_frame", next_replay_frame_rw, fp_i2)

// ---- PhysReplaySave (0x42ce50): the header, the frame table, the data ------------------------------------------------
static void __cdecl PhysReplaySave_rw(int32_t fd) {
    SingleEnter(g_single, 0, 0);
    int32_t h[7];
    h[2] = g_start_time;
    h[3] = g_num_frames;
    h[4] = g_frame_size;
    h[5] = g_nframes;
    h[6] = g_data_used;
    h[0] = (int32_t)REPL_COOKIE;
    h[1] = 6;
    FileWrite(fd, h, 0x1c);
    FileWrite(fd, g_frames, g_nframes << 2);
    FileWrite(fd, g_data, h[6]);
    SingleLeave(g_single, 0, 0);
}
static void fp_save(Footprint& f, int32_t) { f.replay_only = "writes the replay file"; }
PORT_FN(0x0042ce50, "PhysReplaySave", PhysReplaySave_rw, fp_save)

// =====================================================================================================================
// Playback (physics thread)
// =====================================================================================================================

// ---- PhysReplayPlayUpdate (0x42cf00): every dynamic phob between this frame and the next; the telemetry; the frame's
// events, once; at the last frame, pause ----------------------------------------------------------------------------
static void __cdecl PhysReplayPlayUpdate_rw(PhobRoot** phobs, int32_t n) {
    if (!g_have_recording) return;
    const int32_t f = convert_phystime2frame(g_physics_tick);
    const float dt = (float)convert_phystime2deltat(g_physics_tick);   // fstp dword
    const uint8_t at_end = (uint32_t)g_cur_frame - (uint32_t)f == 0;
    int32_t g = f;
    if (!at_end) g = next_replay_frame(f, 1);
    ASSERT_MSG(g_frames[f] != -1, S_FRAME_IDX, f, g_cur_frame);
    const ReplayFrame* a = (const ReplayFrame*)(g_data + g_frames[f]);
    ASSERT_MSG(g_frames[g] != -1, S_FRAME_IDX, g, g_cur_frame);
    const ReplayFrame* b = (const ReplayFrame*)(g_data + g_frames[g]);
    const uint8_t* pa = a->b + 0xb;
    const uint8_t* pb = b->b + 0xb;
    const uint32_t dtb = Ubits(dt);                                    // pushed as a dword
    for (int32_t i = 0; i < n; i++) {
        PhobRoot* p = phobs[i];
        if (p && VFN(p, 0x2c, uint8_t)(p, 0)) {                         // IsDynamic
            const int32_t off = packet_offset(p);
            VFN(p, 0x1c, void, const uint8_t*, const uint8_t*, uint32_t)(p, 0, pa + off, pb + off, dtb);   // UpdateReplay
        }
    }
    update_replay(a, b, dtb);
    if (g_last_frame != f) {
        const uint8_t* ev = a->b + g_frame_size;
        while (*ev < 8) {
            void* h = g_handlers[*ev];
            if (!h) break;
            const int32_t size = ((Handler_t)h)(ev + 1, g_event_mode);
            ev = ev + size + 1;
        }
    }
    g_last_frame = f;
    if (at_end) g_physics_paused = 1;
}
// Every phob (UpdateReplay writes it; a car's models and body volume too), physics_paused, the telemetry, and what
// the frame's events will do: the stream is walked the way the original will walk it, by the handlers' known sizes.
// A Car's damage / reset handler dents or restores that car; the crash sound is an output; the splash goes to the
// physics task's event queue (a static); an AI event is fine unless it would LogPanic. An unknown handler: replay.
static void fp_play_update(Footprint& f, PhobRoot** phobs, int32_t n) {
    fp_statics(f);
    f.add(&g_physics_paused, 1, "physics_paused");
    f.add((void*)0x00520c58, 20, "telemetry");
    for (int32_t i = 0; i < n; i++) {
        PhobRoot* p = phobs[i];
        if (!p) continue;
        f.object(p, "phob");
        if (is_car_vtable((uint32_t)(uintptr_t)p->vtable)) fp_car_parts(f, p);
    }
    if (!g_have_recording) return;
    if (g_nframes <= 0 || !g_frames || !g_data) { f.replay_only = "no replay buffer"; return; }
    const int32_t fr = (g_physics_tick / 6) % g_nframes;
    if (g_last_frame == fr) return;
    const int32_t off = g_frames[fr];
    if (off < 0 || off >= DATA_BYTES) { f.replay_only = "a frame outside the buffer"; return; }
    const uint8_t* ev = g_data + off + g_frame_size;
    for (int k = 0; k < 4096; k++) {
        if (ev < g_data || ev >= g_data + DATA_BYTES) { f.replay_only = "events outside the buffer"; return; }
        if (*ev >= 8) return;
        const uint32_t h = (uint32_t)(uintptr_t)g_handlers[*ev];
        if (!h) return;
        int32_t size;
        void* car = 0;
        if (h == H_CRASH) size = 0x14;
        else if (h == H_SPLASH) size = 0x1c;
        else if (h == H_AI) {
            const int32_t kind = *(const int32_t*)(ev + 1);
            if (kind != 0 && kind != 1) { f.replay_only = "an AI replay event would LogPanic"; return; }
            size = 0x2c;
        } else if (h == H_DAMAGE) {
            car = PhysTaskFindCar((int32_t)(int8_t)ev[1 + 13]);
            size = 14;
        } else if (h == H_RESET) {
            car = PhysTaskFindCar((int32_t)(int8_t)ev[1]);
            size = 2;
        } else {
            f.replay_only = "an unknown replay event handler";
            return;
        }
        if (car) fp_car_all(f, car);
        ev += size + 1;
    }
    f.replay_only = "too many replay events";
}
PORT_FN(0x0042cf00, "PhysReplayPlayUpdate", PhysReplayPlayUpdate_rw, fp_play_update)

// ---- convert_phystime2deltat (0x42d0b0): (t % 6) / 6, left in ST0 ------------------------------------------------------
static double __cdecl convert_phystime2deltat_rw(int32_t t) { return D(t % 6) * D(K_SIXTH); }
PORT_FN(0x0042d0b0, "convert_phystime2deltat", convert_phystime2deltat_rw, fp_pure_i1)

// ---- update_replay (0x42d0e0): the physics task's telemetry between two frames ---------------------------------------
static void __cdecl update_replay_rw(const ReplayFrame* a, const ReplayFrame* b, uint32_t t_bits) {
    PhysicsTelemetry* tel = PhysTaskGetTelemetry();
    get_replay_telemetry(tel, a, b, t_bits);
}
static void fp_update_replay(Footprint& f, const ReplayFrame*, const ReplayFrame*, uint32_t) {
    f.add((void*)0x00520c58, 20, "telemetry");
}
PORT_FN(0x0042d0e0, "update_replay", update_replay_rw, fp_update_replay)

// ---- get_replay_telemetry (0x42d100): unpack a frame's telemetry, the three floats between the two frames -------------
// Each 16-bit value is x 2^-15 over the range [lo, lo + R]; the x87 sequence per value is, with A and B the two
// frames' raw values: ((B k - k A) R) t + (A R) k + lo. R (600, then 8) is computed at run time (exact either way).
static __forceinline float unpack_between(uint16_t ua, uint16_t ub, double r, double lo, float t) {
    const double A = (double)ua, B = (double)ub;             // fild of a zero-extended qword: exact
    const double diff = B * D(K_Q15) - D(K_Q15) * A;
    const double x = (diff * r) * D(t);
    return (float)((x + (A * r) * D(K_Q15)) + lo);
}
static void __cdecl get_replay_telemetry_rw(PhysicsTelemetry* out, const ReplayFrame* a, const ReplayFrame* b, float t) {
    memcpy(&out->dlong, a->b, 4);
    out->lap = a->b[4];
    const double r1 = D(K_300) - D(K_M300);
    out->speed = unpack_between(rd16(a->b + 5), rd16(b->b + 5), r1, D(K_M300), t);
    const double r2 = D(K_4) - D(K_M4);
    out->lat_g = unpack_between(rd16(a->b + 7), rd16(b->b + 7), r2, D(K_M4), t);
    out->long_g = unpack_between(rd16(a->b + 9), rd16(b->b + 9), r2, D(K_M4), t);
}
static void fp_get_replay_telemetry(Footprint& f, PhysicsTelemetry*, const ReplayFrame*, const ReplayFrame*, float) {
    f.pure = true;
}
PORT_FN(0x0042d100, "get_replay_telemetry", get_replay_telemetry_rw, fp_get_replay_telemetry)

// ---- PhysReplayGetTelemetry (0x42d240): the telemetry at t seconds, for the replay screen's graphs ------------------
static uint8_t __cdecl PhysReplayGetTelemetry_rw(PhysicsTelemetry* out, uint32_t t_bits) {
    const float t = Fbits(t_bits);
    const int32_t tick = x87_ftol(D(t) * D(K_62_5) + K_HALF);
    if (g_start_time > tick) return 0;
    const int32_t e = convert_frame2phystime(g_num_frames - 2);
    if ((int32_t)((uint32_t)g_start_time + (uint32_t)e) <= tick) return 0;
    const int32_t f = convert_phystime2frame(tick);
    const int32_t g = next_replay_frame(f, 1);
    const float dt = (float)convert_phystime2deltat(tick);
    ASSERT_MSG(g_frames[g] != -1, S_FRAME_IDX, g, g_cur_frame);
    ASSERT_MSG(g_frames[f] != -1, S_FRAME_IDX, f, g_cur_frame);
    const int32_t* fr = g_frames;
    const ReplayFrame* b = (const ReplayFrame*)(fr[g] + g_data);
    const ReplayFrame* a = (const ReplayFrame*)(fr[f] + g_data);
    get_replay_telemetry(out, a, b, Ubits(dt));
    return 1;
}
static void fp_get_telemetry(Footprint& f, PhysicsTelemetry* out, uint32_t) { f.add(out, sizeof *out, "telemetry out"); }
PORT_FN(0x0042d240, "PhysReplayGetTelemetry", PhysReplayGetTelemetry_rw, fp_get_telemetry)

// ---- PhysReplayPlayMode (0x42d340) -------------------------------------------------------------------------------------
static uint8_t __cdecl PhysReplayPlayMode_rw() { return g_play_mode; }
PORT_FN(0x0042d340, "PhysReplayPlayMode", PhysReplayPlayMode_rw, fp_nothing)

// =====================================================================================================================
// Setting up, recording and sealing (physics thread)
// =====================================================================================================================

// ---- PhysReplayBGBegin (0x42d350): no handlers, an empty buffer --------------------------------------------------------
static void __cdecl PhysReplayBGBegin_rw() {
    __stosd((unsigned long*)g_handlers, 0, 8);
    g_wrap_mark = -1;
    g_wraps = 0;
    g_data_used = 0;
    g_play_mode = 0;
    g_frame_size = 0;
    g_start_time = 0;
    g_cur_frame = -1;
    g_wrapped = 0;
    g_data = 0;
}
static void fp_statics_only(Footprint& f) { fp_statics(f); }
PORT_FN(0x0042d350, "PhysReplayBGBegin", PhysReplayBGBegin_rw, fp_statics_only)

// ---- PhysReplayDoneCreating (0x42d3a0): the 2 MB buffer; the frame size (+0xb for the telemetry); the frame table ----
static void __cdecl PhysReplayDoneCreating_rw() {
    uint8_t* data = (uint8_t*)MemAlloc(DATA_BYTES);
    const int32_t fs = g_frame_size + 0xb;
    g_data = data;
    g_frame_size = fs;
    g_ev_cursor = fs;
    int32_t q = DATA_BYTES / fs;
    const int32_t cap = g_max_frames;
    if (!(q < cap)) q = cap;
    const uint32_t bytes = (uint32_t)q * 4u;
    g_nframes = q;
    uint8_t* t = (uint8_t*)MemAlloc((int32_t)bytes);
    g_frames = (int32_t*)t;
    __stosd((unsigned long*)t, 0xffffffffu, bytes >> 2);
    __stosb(t + (bytes & ~3u), 0xff, bytes & 3);
}
static void fp_done_creating(Footprint& f) { f.replay_only = "allocates the replay buffer and the frame table"; }
PORT_FN(0x0042d3a0, "PhysReplayDoneCreating", PhysReplayDoneCreating_rw, fp_done_creating)

// ---- PhysReplayRegisterPhob (0x42d410): the phob's packet offset (+0x34); the frame grows by its packet ----------------
static void __cdecl PhysReplayRegisterPhob_rw(PhobRoot* p) {
    p->_028[3] = (uint32_t)g_frame_size;
    const int32_t size = VFN(p, 0x18, int32_t)(p, 0);         // GetReplayPacketSize
    g_frame_size += size;
}
static void fp_register_phob(Footprint& f, PhobRoot* p) {
    fp_statics(f);
    f.add(&p->_028[3], 4, "phob packet offset");
}
PORT_FN(0x0042d410, "PhysReplayRegisterPhob", PhysReplayRegisterPhob_rw, fp_register_phob)

// ---- PhysReplayBGEnd (0x42d430) ----------------------------------------------------------------------------------------
static void __cdecl PhysReplayBGEnd_rw() {
    op_delete(g_data);
    g_data = 0;
    op_delete(g_frames);
    g_frames = 0;
    g_nframes = 0;
    g_have_recording = 0;
}
static void fp_bg_end(Footprint& f) { f.replay_only = "frees the replay buffer and the frame table"; }
PORT_FN(0x0042d430, "PhysReplayBGEnd", PhysReplayBGEnd_rw, fp_bg_end)

// ---- PhysReplaySealRecording (0x42d470): close the last frame; if the ring wrapped, walk back from it to the oldest
// frame still intact (its data at or past where the last frame ends, the frame before it not), counting frames and
// unwinding the wrap count at each wrap of the index; the start time is that frame's tick ------------------------------
static void __cdecl PhysReplaySealRecording_rw() {
    add_separator_event();
    const int32_t cur = g_cur_frame;
    if (!g_wrapped) {
        g_num_frames = cur + 1;
        g_data_used = g_frames[g_cur_frame] + g_ev_cursor;
        return;
    }
    const int32_t end = g_frames[cur] + g_ev_cursor;
    int32_t k = prev_replay_frame(cur, 1);
    g_num_frames = 0;
    for (;;) {
        g_num_frames++;
        const int32_t e = prev_replay_frame(k, 1);
        const int32_t* fr = g_frames;
        if (fr[e] < end && fr[k] >= end) break;
        if (e > k) g_wraps--;
        k = e;
    }
    g_start_time = convert_frame2phystime((int32_t)((uint32_t)g_wraps * (uint32_t)g_nframes + (uint32_t)k));
}
// add_separator_event's byte, and the statics
static void fp_separator(Footprint& f) {
    fp_statics(f);
    if (!g_frames || !g_data) { f.replay_only = "no replay buffer"; return; }
    f.add(g_data + g_frames[g_cur_frame] + g_ev_cursor, 1, "event separator");
}
PORT_FN(0x0042d470, "PhysReplaySealRecording", PhysReplaySealRecording_rw, fp_separator)

// ---- prev_replay_frame (0x42d520): (f - n + nframes) % nframes -----------------------------------------------------------
static int32_t __cdecl prev_replay_frame_rw(int32_t f, int32_t n) {
    const int32_t m = g_nframes;
    return (int32_t)((uint32_t)f - (uint32_t)n + (uint32_t)m) % m;
}
PORT_FN(0x0042d520, "prev_replay_frame", prev_replay_frame_rw, fp_i2)

// ---- add_separator_event (0x42d540): a 0 byte closes the frame's events ----------------------------------------------------
static void __cdecl add_separator_event_rw() {
    const int32_t o = g_frames[g_cur_frame] + g_ev_cursor;
    g_data[o] = 0;
    g_ev_cursor++;
}
PORT_FN(0x0042d540, "add_separator_event", add_separator_event_rw, fp_separator)

// ---- PhysReplayRecordUpdate (0x42d570): a new frame every 6 ticks (each dynamic phob's packet); the telemetry, every
// tick, into the current frame -----------------------------------------------------------------------------------------
static void __cdecl PhysReplayRecordUpdate_rw(PhobRoot** phobs, int32_t n) {
    const int32_t f = convert_phystime2frame(g_physics_tick);
    if (f != g_cur_frame) {
        advance_replay_frame(f);
        ASSERT_MSG(g_frames[f] != -1, S_FRAME_IDX, f, g_cur_frame);
        uint8_t* base = g_data + g_frames[f] + 0xb;
        for (int32_t i = 0; i < n; i++) {
            PhobRoot* p = phobs[i];
            if (VFN(p, 0x2c, uint8_t)(p, 0)) {                  // IsDynamic (no null check)
                PhobRoot* q = phobs[i];
                VFN(q, 0x20, void, uint8_t*)(q, 0, base + packet_offset(q));   // MakeReplayPacket
            }
        }
    }
    ASSERT_MSG(g_frames[f] != -1, S_FRAME_IDX, f, g_cur_frame);
    record_telemetry((ReplayFrame*)(g_data + g_frames[f]));
    g_cur_frame = f;
}
// the data a new frame may take: the separator at the end of the last frame, the frame table entry, and the frame
// itself at either place advance_replay_frame can put it (straight after the last frame, or the buffer's start); or,
// in the same frame, just the telemetry
static void fp_new_frame(Footprint& f, int32_t fr) {
    fp_statics(f);
    if (!g_frames || !g_data || g_nframes <= 0) { f.replay_only = "no replay buffer"; return; }
    const uint32_t fs = (uint32_t)g_frame_size;
    if (g_cur_frame != -1) {
        const int32_t sep = g_frames[g_cur_frame] + g_ev_cursor;
        f.add(g_data + sep, 1, "event separator");
        f.add(g_data + sep + 1, fs, "the new frame");
    }
    f.add(g_data, fs, "the new frame (wrapped)");
    f.add(&g_frames[fr], 4, "frame table");
}
static void fp_record_update(Footprint& f, PhobRoot** phobs, int32_t n) {
    fp_crt_scratch(f);
    for (int32_t i = 0; i < n; i++)
        if (phobs[i]) f.object(phobs[i], "phob");               // MakeReplayPacket (Car's clears reset_event)
    if (g_nframes <= 0 || !g_frames || !g_data) { fp_statics(f); f.replay_only = "no replay buffer"; return; }
    const int32_t fr = (g_physics_tick / 6) % g_nframes;
    if (fr != g_cur_frame) fp_new_frame(f, fr);
    else {
        fp_statics(f);
        f.add(g_data + g_frames[fr], 11, "frame telemetry");
    }
}
PORT_FN(0x0042d570, "PhysReplayRecordUpdate", PhysReplayRecordUpdate_rw, fp_record_update)

// ---- record_telemetry (0x42d640): the physics task's telemetry into a frame's header -------------------------------------
// each float clamped (a NaN to the bottom), scaled to 16 bits, truncated (__ftol)
static __forceinline uint16_t pack16(const float* x, float lo, float hi, double range) {
    float c;
    if (!(D(*x) >= D(lo))) c = lo;                                     // fcomp; test ah,1 (the constant's bits)
    else c = D(hi) > D(*x) ? *x : hi;                                   // fcom; test ah,0x41
    return (uint16_t)x87_ftol(((D(c) - D(lo)) / range) * D(K_32768));
}
static void __cdecl record_telemetry_rw(ReplayFrame* fr) {
    const PhysicsTelemetry* tel = PhysTaskGetTelemetry();
    uint8_t* p = fr->b;
    memcpy(p, &tel->dlong, 4);
    p[4] = *((const uint8_t*)tel + 4);
    const uint16_t s = pack16(&tel->speed, K_M300, K_300, D(K_300) - D(K_M300));
    memcpy(p + 5, &s, 2);
    volatile float r8 = (float)(D(K_4) - D(K_M4));                    // fstp dword [esp+0xc]: 8
    const uint16_t la = pack16(&tel->lat_g, K_M4, K_4, D(r8));
    memcpy(p + 7, &la, 2);
    const uint16_t lo = pack16(&tel->long_g, K_M4, K_4, D(r8));
    memcpy(p + 9, &lo, 2);
}
static void fp_record_telemetry(Footprint& f, ReplayFrame* fr) { f.add(fr, 11, "frame telemetry"); }
PORT_FN(0x0042d640, "record_telemetry", record_telemetry_rw, fp_record_telemetry)

// ---- advance_replay_frame (0x42d7b0): start frame f: close the last one; place f straight after it -- unless the index
// wrapped (f == 0) or fewer than 0x600 bytes are left, when the ring has wrapped: the data goes back to 0 (it carries
// on instead at an index wrap after the data wrapped first in this lap: g_wrap_mark) --------------------------------------
static void __cdecl advance_replay_frame_rw(int32_t f) {
    uint8_t bl = 1;
    g_have_recording = 1;
    if (g_cur_frame == -1) {
        g_frames[f] = 0;
        g_ev_cursor = g_frame_size;
        return;
    }
    add_separator_event();
    int32_t* slot = &g_frames[g_cur_frame];
    const int32_t next = *slot + g_ev_cursor;
    const bool room = (int32_t)((uint32_t)next + 0x600u) < DATA_BYTES;
    if (f != 0 || g_cur_frame == -1) bl = 0;
    if (!bl && room) {
        g_frames[f] = next;
    } else {
        g_wrapped = 1;
        const int32_t e = *slot + g_ev_cursor;
        const int32_t du = g_data_used;
        g_data_used = e > du ? e : du;
        bool zero;
        if (!room) {
            g_wrap_mark = g_wraps;
            zero = true;
        } else zero = bl && g_wrap_mark != g_wraps;
        if (zero) g_frames[f] = 0;
        else g_frames[f] = *slot + g_ev_cursor;
        g_wraps += bl;
    }
    g_ev_cursor = g_frame_size;
}
static void fp_advance(Footprint& f, int32_t fr) {
    if (!g_frames || !g_data) { fp_statics(f); f.replay_only = "no replay buffer"; return; }
    fp_statics(f);
    if (g_cur_frame != -1) f.add(g_data + g_frames[g_cur_frame] + g_ev_cursor, 1, "event separator");
    f.add(&g_frames[fr], 4, "frame table");
}
PORT_FN(0x0042d7b0, "advance_replay_frame", advance_replay_frame_rw, fp_advance)

// ---- PhysReplayAddEvent (0x42d8b0): {type, payload} into the current frame's events ---------------------------------------
// (already hooked by port.cpp as an output: merged at integration)
static void __cdecl PhysReplayAddEvent_rw(int32_t type, const void* src, int32_t size) {
    if ((int32_t)((uint32_t)g_ev_cursor + (uint32_t)size + 1u) > 0x600) {
        const int32_t now = PTimeNow();
        if (g_last_drop_log == 0 || (int32_t)((uint32_t)now - 3000u) > g_last_drop_log) {
            LogReport(S_TOO_MANY);
            g_last_drop_log = now;
        }
        return;
    }
    const int32_t cur = g_cur_frame;
    ASSERT_MSG(g_frames[cur] != -1, S_FRAME_IDX, cur, cur);
    uint8_t* dst = g_data + (g_frames[cur] + g_ev_cursor);
    *dst++ = (uint8_t)type;
    __movsd((unsigned long*)dst, (const unsigned long*)src, (uint32_t)size >> 2);
    __movsb(dst + ((uint32_t)size & ~3u), (const unsigned char*)src + ((uint32_t)size & ~3u), (uint32_t)size & 3);
    g_ev_cursor += size + 1;
}
static void fp_add_event(Footprint& f, int32_t, const void*, int32_t size) {
    fp_statics(f);
    if ((int32_t)((uint32_t)g_ev_cursor + (uint32_t)size + 1u) > 0x600) return;   // dropped (the log time)
    if (!g_frames || !g_data) { f.replay_only = "no replay buffer"; return; }
    f.add(g_data + (g_frames[g_cur_frame] + g_ev_cursor), (uint32_t)size + 1, "replay event");
}
PORT_FN(0x0042d8b0, "PhysReplayAddEvent", PhysReplayAddEvent_rw, fp_add_event)

// ---- PhysReplayInstallEventHandler (0x42d960): one handler a type (no bounds check) -----------------------------------------
static void __cdecl PhysReplayInstallEventHandler_rw(int32_t type, void* fn) {
    void** slot = &g_handlers[type];
    ASSERT_MSG(*slot == 0, S_ONE_HOOK);
    *slot = fn;
}
static void fp_install(Footprint& f, int32_t type, void*) { f.add(&g_handlers[type], 4, "replay event handler"); }
PORT_FN(0x0042d960, "PhysReplayInstallEventHandler", PhysReplayInstallEventHandler_rw, fp_install)

// ---- PhysReplayUninstallEventHandler (0x42d990): the first slot holding fn is cleared; none: LogPanic -----------------------
static void __cdecl PhysReplayUninstallEventHandler_rw(void* fn) {
    for (int32_t i = 0; i < 8; i++)
        if (g_handlers[i] == fn) {
            g_handlers[i] = 0;
            return;
        }
    LogPanic(S_NO_HANDLER, fn);
}
static void fp_uninstall(Footprint& f, void* fn) {
    f.add(g_handlers, 32, "replay event handlers");
    for (int32_t i = 0; i < 8; i++)
        if (g_handlers[i] == fn) return;
    f.replay_only = "LogPanic: no such handler";
}
PORT_FN(0x0042d990, "PhysReplayUninstallEventHandler", PhysReplayUninstallEventHandler_rw, fp_uninstall)

// =====================================================================================================================
// The ghost car's lap, from the replay (physics thread)
// =====================================================================================================================

// ---- feed_ghost_car (0x42d9d0): copy a car's packets for the last `duration` ticks out of the ring into out (at most
// *count; *count becomes the number copied). A lap longer than the ring is cut to the ring (the oldest frame on) and
// the ticks cut are returned (the caller adds them to the record's time_offset); else 0. The frame being recorded is
// left out. -------------------------------------------------------------------------------------------------------------
static int32_t __cdecl feed_ghost_car_rw(int32_t car, int32_t start_time, int32_t duration, uint8_t* out, int32_t* count) {
    const int32_t span = (int32_t)((uint32_t)g_nframes * 6u);
    uint8_t* c = (uint8_t*)PhysTaskFindCar(car);
    int32_t result = 0;
    if (!c) return result;
    const int32_t off = packet_offset(c);
    int32_t first, n;
    if (span > duration) {
        first = convert_phystime2frame(start_time);
        n = next_replay_frame(convert_phystime2frame(duration), 1);
    } else {
        if (*count > g_nframes) *count = g_nframes;
        n = *count;
        first = next_replay_frame(g_cur_frame, 1);
        result = (int32_t)((uint32_t)duration - (uint32_t)span);
    }
    if (prev_replay_frame((int32_t)((uint32_t)first + (uint32_t)n - 1u), 1) == g_cur_frame) n--;
    int32_t i = 0;
    if (n > 0) {
        uint8_t* dst = out;
        do {
            if (!(*count > i)) break;
            const int32_t fr = (int32_t)((uint32_t)first + (uint32_t)i) % g_nframes;
            i++;
            ASSERT_MSG(g_frames[fr] != -1, S_FRAME_IDX, fr, g_cur_frame);
            const uint8_t* src = g_data + (g_frames[fr] + off) + 0xb;
            __movsd((unsigned long*)dst, (const unsigned long*)src, 15);
            dst += 0x3c;
        } while (i < n);
    }
    const int32_t k = *count;
    *count = k < n ? k : n;
    return result;
}
static void fp_feed_ghost(Footprint& f, int32_t, int32_t, int32_t, uint8_t* out, int32_t* count) {
    f.add(count, 4, "count");
    int32_t k = *count;
    if (k > g_nframes) k = g_nframes;
    if (k > 0) f.add(out, (uint32_t)k * 60u, "ghost packets");
}
PORT_FN(0x0042d9d0, "feed_ghost_car", feed_ghost_car_rw, fp_feed_ghost)

// ---- update_ghost_car (0x42db20): the ghost between packets (time / 6) and the next, at (time % 6) / 6 ------------------
// (the packet index is convert_phystime2frame's: modulo the replay's frame count, as the original has it)
static void __cdecl update_ghost_car_rw(GhostCarL* g, uint8_t* packets, int32_t time, int32_t count) {
    const int32_t fr = convert_phystime2frame(time);
    const int32_t nx = next_replay_frame(fr, 1);
    if (nx < count) {
        const float t = (float)(D(time % 6) * D(K_SIXTH));           // convert_phystime2deltat, inline
        GhostCar_UpdateGhost_o(g, 0, packets + (uint32_t)fr * 60u, packets + (uint32_t)nx * 60u, Ubits(t));
    }
}
static void fp_update_ghost_car(Footprint& f, GhostCarL* g, uint8_t*, int32_t, int32_t) { fp_car_all(f, g); }
PORT_FN(0x0042db20, "update_ghost_car", update_ghost_car_rw, fp_update_ghost_car)

// =====================================================================================================================
// GhostCar (ghostcar.obj)
// =====================================================================================================================

// ---- GhostCar::NewLap (0x42ddc0, static): offer the ghost a new best lap -----------------------------------------------
static void __cdecl GhostCar_NewLap(const GhostData* gd, const uint8_t* packets, int32_t car_type) {
    if (GhostCarL* g = g_ghost_global) GhostCar_new_lap_o(g, 0, gd, packets, car_type);
}
static void fp_new_lap_static(Footprint& f, const GhostData*, const uint8_t*, int32_t) {
    if (GhostCarL* g = g_ghost_global) f.add(&g->best_ticks, 16, "ghost pending lap");
}
PORT_FN(0x0042ddc0, "GhostCar::NewLap", GhostCar_NewLap, fp_new_lap_static)

// ---- GhostCar::GhostCar (0x42dde0): a LocalCar; the global ghost; the car it follows (PhobData +0x1e0); 2400 packets ----
static GhostCarL* __fastcall GhostCar_ctor(GhostCarL* self, Edx, void* pd, void* p2) {
    LocalCar_ctor(self, 0, pd, p2);
    self->vtable = (void**)(uintptr_t)VT_GHOSTCAR;
    g_ghost_global = self;
    self->follow_car = *(int32_t*)((uint8_t*)pd + 0x1e0);
    self->car_type = at<int32_t>(self, C_CAR_INDEX);
    uint8_t* p = (uint8_t*)MemAlloc(0x23280);
    if (p) {
        uint8_t* q = p;
        for (int32_t i = 0x95f; i >= 0; i--) {
            ReplayPacket_ctor(q, 0);
            q += 0x3c;
        }
        self->packets = p;
    } else self->packets = 0;
    self->prev_flags = 0;
    GhostCar_reset_o(self, 0);
    return self;
}
static void fp_ghost_ctor(Footprint& f, GhostCarL*, Edx, void*, void*) {
    f.replay_only = "constructs a LocalCar (registers it), allocates the packets";
}
PORT_FN(0x0042dde0, "GhostCar::GhostCar", GhostCar_ctor, fp_ghost_ctor)

// ---- GhostCar::GetMessage (0x42de70, vtable +0x14): Car's, then the speed (+0x4c) and the ghost's car (+0x18c) ----------
// The original tests (flags | 1) for zero -- never -- before a branch that would have sent the followed car's message
// instead (PhysTaskFindCar(follow_car)->GetMessage); that dead branch is left out.
static void __fastcall GhostCar_GetMessage(GhostCarL* self, Edx, uint8_t* buf) {
    uint8_t* m = buf + msg_offset(self);
    Car_GetMessage(self, 0, buf);
    memcpy(m + 0x4c, &self->speed, 4);
    memcpy(m + 0x18c, &self->car_type, 4);
}
static void fp_ghost_message(Footprint& f, GhostCarL* self, Edx, uint8_t* buf) {
    f.add(buf + msg_offset(self), 0x190, "message");
}
PORT_FN(0x0042de70, "GhostCar::GetMessage", GhostCar_GetMessage, fp_ghost_message)

// ---- GhostCar::Reset (0x42dec0, vtable +4): Car's, then reset --------------------------------------------------------------
static void __fastcall GhostCar_Reset(GhostCarL* self, Edx) {
    Car_Reset(self, 0);
    GhostCar_reset_o(self, 0);
}
// reset's init_from: the record GhostGetData will return
static void fp_ghost_reset_packets(Footprint& f, GhostCarL* self) {
    uint8_t* rec = g_ghost_record;
    if (rec && rec[1]) fp_ghost_packets(f, self, (const GhostData*)(rec + 0x2344c));
}
static void fp_ghost_Reset(Footprint& f, GhostCarL* self, Edx) {
    fp_car_all(f, self);
    fp_ghost_reset_packets(f, self);
}
PORT_FN(0x0042dec0, "GhostCar::Reset", GhostCar_Reset, fp_ghost_Reset)

// ---- GhostCar::UpdateReplay (0x42dee0, vtable +0x1c): Car's; the ghost's car from packet +0x3c --------------------------
static void __fastcall GhostCar_UpdateReplay(GhostCarL* self, Edx, const uint8_t* p0, const uint8_t* p1, uint32_t t_bits) {
    Car_UpdateReplay(self, 0, p0, p1, t_bits);
    self->car_type = (int32_t)(int8_t)p0[0x3c];
}
static void fp_ghost_update_replay(Footprint& f, GhostCarL* self, Edx, const uint8_t*, const uint8_t*, uint32_t) {
    fp_car_all(f, self);
}
PORT_FN(0x0042dee0, "GhostCar::UpdateReplay", GhostCar_UpdateReplay, fp_ghost_update_replay)

// ---- GhostCar::MakeReplayPacket (0x42df10, vtable +0x20): Car's 0x3c bytes and the ghost's car -----------------------------
static void __fastcall GhostCar_MakeReplayPacket(GhostCarL* self, Edx, uint8_t* p) {
    Car_MakeReplayPacket(self, 0, p);
    p[0x3c] = (uint8_t)self->car_type;
}
static void fp_ghost_make_packet(Footprint& f, GhostCarL* self, Edx, uint8_t* p) {
    fp_crt_scratch(f);
    f.object(self, "ghost");
    f.add(p, 0x3d, "packet");
}
PORT_FN(0x0042df10, "GhostCar::MakeReplayPacket", GhostCar_MakeReplayPacket, fp_ghost_make_packet)

// ---- GhostCar::reset (0x42df30, private): invisible; the stored ghost lap (GhostGetData) or none (lap 1234567 ticks) ------
static void __fastcall GhostCar_reset(GhostCarL* self, Edx) {
    GhostData gd;
    uint8_t* packets;
    int32_t car_type;
    at<uint32_t>(self, C_OPACITY) = 0;
    self->speed = 0.0f;
    flags(self) = 0;
    if (GhostGetData(&gd, &packets, &car_type)) GhostCar_init_from_o(self, 0, &gd, packets, car_type);
    else {
        self->lap_ticks = 0x12d687;
        self->count = 0;
        self->time_offset = 0;
    }
    self->pending_data = 0;
    self->pending_packets = 0;
    self->last_lap = -1;
    self->best_ticks = self->lap_ticks;
}
static void fp_ghost_reset(Footprint& f, GhostCarL* self, Edx) {
    f.object(self, "ghost");
    fp_ghost_reset_packets(f, self);
}
PORT_FN(0x0042df30, "GhostCar::reset(private)", GhostCar_reset, fp_ghost_reset)   // (the ini's keys ignore case)

// ---- GhostCar::~GhostCar (0x42dfc0) ---------------------------------------------------------------------------------------
static void __fastcall GhostCar_dtor(GhostCarL* self, Edx) {
    self->vtable = (void**)(uintptr_t)VT_GHOSTCAR;
    g_ghost_global = 0;
    if (self->packets) {
        op_delete(self->packets);
        self->packets = 0;
    }
    LocalCar_dtor(self, 0);
}
static void fp_ghost_dtor(Footprint& f, GhostCarL*, Edx) { f.replay_only = "frees the packets and the LocalCar"; }
PORT_FN(0x0042dfc0, "GhostCar::~GhostCar", GhostCar_dtor, fp_ghost_dtor)

// ---- GhostCar::UpdateGhost (0x42e000): Car::UpdateReplay between two packets, keeping the flags; the speed between them --
static void __fastcall GhostCar_UpdateGhost(GhostCarL* self, Edx, const uint8_t* p0, const uint8_t* p1, uint32_t t_bits) {
    const uint32_t fl = flags(self);
    Car_UpdateReplay(self, 0, p0, p1, t_bits);
    flags(self) = fl;
    const float t = Fbits(t_bits);
    const double x0 = rdf(p0 + 0x12), y0 = rdf(p0 + 0x16), z0 = rdf(p0 + 0x1a);   // PhobDyno packet: velocity
    const double s0 = x87_sqrt((y0 * y0 + z0 * z0) + x0 * x0);
    const float part = (float)(s0 * (D(K_ONE) - D(t)));               // fstp dword [esp+0x10]
    const double x1 = rdf(p1 + 0x12), y1 = rdf(p1 + 0x16), z1 = rdf(p1 + 0x1a);
    const double s1 = x87_sqrt((y1 * y1 + z1 * z1) + x1 * x1);
    self->speed = (float)(s1 * D(t) + D(part));
}
static void fp_update_ghost(Footprint& f, GhostCarL* self, Edx, const uint8_t*, const uint8_t*, uint32_t) { fp_car_all(f, self); }
PORT_FN(0x0042e000, "GhostCar::UpdateGhost", GhostCar_UpdateGhost, fp_update_ghost)

// ---- GhostCar::UpdateCommon (0x42e080, vtable +0xc): Car's while drawn (else its sounds stopped, once); the opacity -------
static void __fastcall GhostCar_UpdateCommon(GhostCarL* self, Edx) {
    if (flags(self) & 2) Car_UpdateCommon(self, 0);
    else if (self->prev_flags & 2) SoundStopCar(at<int32_t>(self, C_CAR_INDEX));
    const uint32_t fl = flags(self);
    at<uint32_t>(self, C_OPACITY) = (fl & 1) ? K_GHOST_OPACITY : 0u;
    self->prev_flags = fl;
}
// Car::UpdateCommon's footprint (phys_car_update.cpp: the car, its sounds' +4..+0x2d, the EngineSound and its samples,
// the tyre sounds, the status string), or SoundStopCar's: the stop byte (+0x2a) of the manager's sounds of this car
static void fp_ghost_update_common(Footprint& f, GhostCarL* self, Edx) {
    f.object(self, "ghost");
    const int32_t idx = at<int32_t>(self, C_CAR_INDEX);
    if (flags(self) & 2) {
        void** sounds = &at<void*>(self, C_SOUNDS);             // start, engine, shift, road, road2, scrape, horn
        if (uint8_t* es = (uint8_t*)sounds[1]) {
            f.add(es, 248, "engine sound");
            const int32_t ns = *(int32_t*)(es + 0xf4);
            for (int i = 0; i < ns && i < 7; i++)
                if (uint8_t* s = *(uint8_t**)(es + 0xd4 + 4 * i)) f.add(s + 4, 0x2a, "engine sample Sound3D");
            if (uint8_t* s = *(uint8_t**)(es + 0xf0)) f.add(s + 4, 0x2a, "engine idle Sound3D");
        }
        const int snd[6] = {0, 2, 3, 4, 5, 6};
        for (int i : snd)
            if (uint8_t* s = (uint8_t*)sounds[i]) f.add(s + 4, 0x2a, "Sound3D");
        for (int i = 0; i < 4; i++) {
            uint8_t* ts = at<uint8_t*>(self, C_WHEELS + 0x1a4 * i + 0x184);
            if (!ts) continue;
            const uint32_t vt = *(const uint32_t*)ts;
            if (vt == 0x004dd7b0) {
                f.add(ts, 0x3c, "tire sound");
                if (uint8_t* s3d = *(uint8_t**)(ts + 0x34)) f.add(s3d + 4, 0x2a, "tire Sound3D");
            } else if (vt == 0x004dd7a0) f.add(ts, 0x10, "tire sound");
        }
        if (idx >= 0) f.add((uint8_t*)0x005540c2 + 368 * idx, 256, "status string");   // not the CarInfo after it (main thread)
    } else if (self->prev_flags & 2) {
        uint8_t* sm = g_sound_manager;
        if (!sm) return;
        void** list = *(void***)(sm + 4);
        const int32_t n = *(int32_t*)(sm + 8);
        for (int32_t i = 0; i < n; i++) {
            uint8_t* s = (uint8_t*)list[i];
            if (*(int32_t*)(s + 0x10) == idx) f.add(s + 0x2a, 1, "sound stop");
        }
    }
}
PORT_FN(0x0042e080, "GhostCar::UpdateCommon", GhostCar_UpdateCommon, fp_ghost_update_common)

// ---- GhostCar::init_from (0x42e0e0, private): take a lap: its time, offset and packets ------------------------------------
static void __fastcall GhostCar_init_from(GhostCarL* self, Edx, const GhostData* gd, const uint8_t* packets, int32_t car_type) {
    self->car_type = car_type;
    self->time_offset = gd->time_offset;
    self->lap_ticks = gd->lap_ticks;
    uint8_t* dst = self->packets;
    const int32_t n = gd->count;
    self->count = n;
    __movsd((unsigned long*)dst, (const unsigned long*)packets, ((uint32_t)n * 60u) >> 2);
    self->best_ticks = self->lap_ticks;
}
static void fp_init_from(Footprint& f, GhostCarL* self, Edx, const GhostData* gd, const uint8_t*, int32_t) {
    f.object(self, "ghost");
    fp_ghost_packets(f, self, gd);
}
PORT_FN(0x0042e0e0, "GhostCar::init_from", GhostCar_init_from, fp_init_from)

// ---- GhostCar::new_lap (0x42e140, private): a better lap than the best so far waits for the next lap ------------------------
static void __fastcall GhostCar_new_lap(GhostCarL* self, Edx, const GhostData* gd, const uint8_t* packets, int32_t car_type) {
    const int32_t t = gd->lap_ticks;
    if (self->best_ticks > t) {
        self->best_ticks = t;
        self->pending_data = gd;
        self->pending_packets = packets;
        self->pending_type = car_type;
    }
}
static void fp_new_lap(Footprint& f, GhostCarL* self, Edx, const GhostData*, const uint8_t*, int32_t) {
    f.add(&self->best_ticks, 16, "ghost pending lap");
}
PORT_FN(0x0042e140, "GhostCar::new_lap", GhostCar_new_lap, fp_new_lap)

// ---- GhostCar::Update (0x42e180, vtable +8): at each lap of the followed car, take a waiting better lap and start over
// (visible from lap 1 with packets, the racing line back to its head); otherwise sit on the followed car; hidden once it
// finishes; while visible, follow the packets by the lap clock; the deity's UpdateCar ------------------------------------
static void __fastcall GhostCar_Update(GhostCarL* self, Edx) {
    void* deity = g_deity;
    const int32_t lap = VFN(deity, 0x48, int32_t, int32_t)(deity, 0, self->follow_car);   // GetCurrentLap
    if (self->last_lap != lap) {
        self->last_lap = lap;
        self->lap_start = g_physics_tick;
        if (const uint8_t* pk = self->pending_packets) {
            GhostCar_init_from_o(self, 0, self->pending_data, pk, self->pending_type);
            self->pending_packets = 0;
        }
        if (self->last_lap > 0 && self->count != 0) flags(self) |= 1;
        const int32_t idx = at<int32_t>(self, C_CAR_INDEX);
        IdealLine_reset_to_head(*(void**)(g_race_deity + 0xa4 + 0x74 * idx), 0);
    }
    if (!(flags(self) & 1)) {
        const uint8_t* c = (const uint8_t*)PhysTaskFindCar(self->follow_car);
        __movsd((unsigned long*)&self->frame, (const unsigned long*)(c + 0x38), 12);
    }
    deity = g_deity;
    if (VFN(deity, 0x40, uint8_t, int32_t)(deity, 0, self->follow_car)) flags(self) &= ~3u;   // CarIsFinished
    const uint32_t fl = flags(self);
    if (fl & 1) {
        const int32_t tick = g_physics_tick;
        flags(self) = fl | 2;
        const int32_t t = (int32_t)((uint32_t)tick - (uint32_t)self->lap_start - (uint32_t)self->time_offset);
        if (t < 0) update_ghost_car(self, self->packets, 0, self->count);
        else if (self->lap_ticks > t) update_ghost_car(self, self->packets, t, self->count);
    }
    deity = g_deity;
    VFN(deity, 0x38, void, int32_t, void*)(deity, 0, at<int32_t>(self, C_CAR_INDEX), self);   // UpdateCar
    self->tick_count++;
}
// the ghost and its car parts (UpdateGhost); a waiting lap's packets; the racing line reset_to_head writes; and
// RaceDeity::UpdateCar's (as for Car::Update: the lap events it may raise are left out, see phys_car_update.cpp)
static void fp_ghost_update(Footprint& f, GhostCarL* self, Edx) {
    fp_car_all(f, self);
    if (self->pending_packets) fp_ghost_packets(f, self, self->pending_data);
    const int32_t idx = at<int32_t>(self, C_CAR_INDEX);
    if (uint8_t* rd = g_race_deity)
        if (void* line = *(void**)(rd + 0xa4 + 0x74 * idx)) f.add(line, 56, "ideal line");
    fp_update_car(f, idx);
}
PORT_FN(0x0042e180, "GhostCar::Update", GhostCar_Update, fp_ghost_update)

// =====================================================================================================================
// The small virtual functions compiled into ghostcar.obj (the only copies: every Car-family vtable uses them)
// =====================================================================================================================
static void __fastcall PhobRoot_MakeStatusString(PhobRoot*, Edx, char* s) { s[0] = 0; }
static void fp_status_string(Footprint& f, PhobRoot*, Edx, char* s) {
    f.pure = true;
    f.add(s, 1, "status string");
}
PORT_FN(0x0042e2c0, "PhobRoot::MakeStatusString", PhobRoot_MakeStatusString, fp_status_string)

static uint8_t __fastcall PhobDyno_IsDynamic(PhobRoot*, Edx) { return 1; }
static void fp_pure_root(Footprint& f, PhobRoot*, Edx) { f.pure = true; }
PORT_FN(0x0042e2d0, "PhobDyno::IsDynamic", PhobDyno_IsDynamic, fp_pure_root)

static PhobRoot* __fastcall PhobDyno_GetDyno(PhobRoot* self, Edx) { return self; }
static void fp_root_getter(Footprint&, PhobRoot*, Edx) {}            // (returns its argument: not fuzzable as pure)
PORT_FN(0x0042e2e0, "PhobDyno::GetDyno", PhobDyno_GetDyno, fp_root_getter)

// Car::ApplyDamage (vtable +0x3c): ActuallyApplyDamage(force, point, flag, 0) -- the flag's whole dword pushed on
static void __fastcall Car_ApplyDamage(CarL* self, Edx, const P3* a, const P3* b, uint32_t flag) {
    Car_ActuallyApplyDamage(self, 0, a, b, flag, 0);
}
static void fp_apply_damage(Footprint& f, CarL* self, Edx, const P3*, const P3*, uint32_t) { fp_car_all(f, self); }
PORT_FN(0x0042e2f0, "Car::ApplyDamage", Car_ApplyDamage, fp_apply_damage)

// Car::GetPerceivedThrottle (vtable +0x44): the recorded throttle in a replay, else the engine's smoothed throttle
static float __fastcall Car_GetPerceivedThrottle(CarL* self, Edx) {
    if (PhysReplayPlayMode()) return at<float>(self, C_THROTTLE);
    return at<float>(self, C_SMOOTH_THROTTLE);
}
static void fp_car_getter(Footprint&, CarL*, Edx) {}
PORT_FN(0x0042e310, "Car::GetPerceivedThrottle", Car_GetPerceivedThrottle, fp_car_getter)

static float __fastcall Car_GetPerceivedRPM(CarL* self, Edx) { return at<float>(self, C_PERCEIVED_RPM); }
static void fp_pure_car(Footprint& f, CarL*, Edx) { f.pure = true; }
PORT_FN(0x0042e330, "Car::GetPerceivedRPM", Car_GetPerceivedRPM, fp_pure_car)

static void __fastcall Car_DoneLowering(CarL* self, Edx) { at<uint8_t>(self, C_LOWERING) = 0; }
static void fp_done_lowering(Footprint& f, CarL* self, Edx) {
    f.pure = true;
    f.add(&at<uint8_t>(self, C_LOWERING), 1, "lowering");
}
PORT_FN(0x0042e340, "Car::DoneLowering", Car_DoneLowering, fp_done_lowering)

static int32_t __fastcall GhostCar_GetReplayPacketSize(PhobRoot*, Edx) { return 0x3d; }
PORT_FN(0x0042e350, "GhostCar::GetReplayPacketSize", GhostCar_GetReplayPacketSize, fp_pure_root)

static int32_t __fastcall GhostCar_GetMessageSize(PhobRoot*, Edx) { return 0x190; }
PORT_FN(0x0042e360, "GhostCar::GetMessageSize", GhostCar_GetMessageSize, fp_pure_root)

static uint8_t __fastcall GhostCar_IsSolid(PhobRoot*, Edx) { return 0; }
PORT_FN(0x0042e370, "GhostCar::IsSolid", GhostCar_IsSolid, fp_pure_root)
