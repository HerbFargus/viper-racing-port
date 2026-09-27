// phys_ai.cpp -- M3 stage 3.5 group A5: the rest of the "ai" library, rewritten.
//
//   ai.obj       the AI's glue: the per-car AICarInfo (position, velocity and ideal line of every car), the
//                AI's begin/end and per-tick update (AIBGUpdate), car registration, AICarSetup / AITweakCarData
//                (the AI's gearing and skill tweaks), the dashboard's key handler, and the small getters.
//   notes.obj    the per-driver "notes" (what the AI learned about each segment of a track): finding them
//                (GetDriverNotes, get_ili_info, the crcs), generating them (generate_notes_for runs a whole race
//                in learn mode) and the learn run over every track (check_learning); the DSTC file names.
//   prox.obj     Proxer: which cars are near which. Every pair of cars has a ProxerDelta (their relative
//                position and velocity); every car has two lists of pointers to its deltas, sorted by distance
//                and by "interest" each tick.
//   gcviewer.obj GhostCarViewer: the ghost-car library screen (the non-drawing part: its state, its files).
//
// Not rewritten here (their callers are): the functions that draw -- AIDashboardDraw, draw_status,
// draw_focus_iline, draw_mouse_cursor, draw_target, AIDisplayLearnMode, learn_draw, clear_screen,
// GhostCarViewer::Draw -- and learn_dialog (a modal UI dialog: UIDoDialog after clear_screen); they belong to the
// UI stage. Bare `ret` stubs: handle_mouse_evt, AIFGUpdate, ASSERT_MSG, GhostCarViewer::Draw. The deleting
// destructor (`vector deleting destructor'), and the $E initialisers.
//
// Threads. The physics thread (physics_thread -> PhysTaskBegin / PhysTaskRestart / PhysTaskUpdate / PhysTaskEnd)
// runs AIBGUpdate (PhysTaskUpdate), AIResetCars (PhysTaskRestart), the cars' constructors and destructors
// (AIRegister* / AIUnregisterCar / GetDriverData / aicar_record_notes, via create_phob) and the AI cars' own
// code (Proxer's deltas and lists, _IsAI, AIGetLine...). The main thread runs AIBegin / AIEnd (WorldBeginCommon
// / WorldEndCommon), AICarSetup and AITweakCarData (SetupCars, parse_car), AISetStrength, the dashboard key
// handler, check_learning and the ghost viewer. Every function here that writes an AI static lists it in its
// footprint as well, so the footprint is right whichever thread a check runs on.
//
// replay_only: whatever allocates or frees (the AICarInfo, the Proxer and its tables, notes, ghost entries),
// loads resources (the ideal line, car files, notes), writes or finds files (notes, the ghost library), runs a
// race (generate_notes_for) or drives the UI (UIShowGroup / UIHideGroup, AddItems, message boxes). All calls are
// by v1.0 address or through the object's vtable, so a hooked rewrite of the callee is what runs.
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

static __forceinline int32_t Ib(const void* p) { int32_t u; memcpy(&u, p, 4); return u; }
static __forceinline uint32_t Ub(const void* p) { uint32_t u; memcpy(&u, p, 4); return u; }
static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline void cp4(void* d, const void* s) { memcpy(d, s, 4); }      // an integer move of a float
static __forceinline void put4(void* d, uint32_t v) { memcpy(d, &v, 4); }
static __forceinline float& Fat(void* p, uint32_t off) { return *(float*)((uint8_t*)p + off); }
static __forceinline int32_t& Iat(void* p, uint32_t off) { return *(int32_t*)((uint8_t*)p + off); }

// the inlined strcpy / strcat of the original (repne scasb; rep movsd / movsb): the string and its terminator
static __forceinline void s_copy(char* d, const char* s) {
    size_t n = strlen(s) + 1;
    memcpy(d, s, n);
}
static __forceinline void s_cat(char* d, const char* s) { s_copy(d + strlen(d), s); }
// FIX helper: the string cut to fit an n-byte field (n - 1 characters and the terminator)
static __forceinline void s_copy_max(char* d, const char* s, size_t n) {
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memcpy(d, s, l);
    d[l] = 0;
}

// ---- layouts -----------------------------------------------------------------------------------------------------
namespace {

struct Point2D { float x, z; };

// AICarInfo (ai.obj, 0x68 bytes, MemAlloc'd by AIRegister*): one per registered car, in ai_cars[16]. Evidence:
// UpdatePos writes +0..+0xc from the car's frame.pos x/z (+0x5c/+0x64) and velocity x/z (+0x234/+0x23c) and
// hands +0 and +8 to Proxer::update_object as the car's position and velocity; the constructors build a
// ConstIdealLine at +0x10 (60 bytes, AIGetLine returns it) and init stores the index at +0x50; the AICar* one
// stores the AICar at both +0x4c and +0x64, the Car* one the car at +0x4c and 0 at +0x64 (_IsAI tests +0x64).
struct AICarInfo {
    Point2D pos;                       // +0x00 x, z of the car's position
    Point2D vel;                       // +0x08 x, z of its velocity
    uint8_t line[60];                  // +0x10 ConstIdealLine (vtable 0x4db6e8; slot 1 = load(name, reversed))
    uint8_t* car;                      // +0x4c the Car (PhobRoot frame at +0x38, velocity at +0x234)
    int32_t index;                     // +0x50 the car's slot
    uint8_t _54[0x10];                 // +0x54 (never touched by ai.obj)
    void* aicar;                       // +0x64 the AICar, 0 for a player's or a net car
};
static_assert(offsetof(AICarInfo, car) == 0x4c && offsetof(AICarInfo, aicar) == 0x64 && sizeof(AICarInfo) == 0x68,
              "AICarInfo");

// ProxerDelta (prox.obj, 20 bytes: MemAlloc(m * 20), constructed in steps of 0x14): one per pair of cars a < b
struct ProxerDelta {
    int16_t a, b;                      // +0x00 the pair (init_tabs), a < b
    Point2D dl;                        // +0x04 pos[b] - pos[a]
    Point2D dv;                        // +0x0c vel[b] - vel[a]
};
static_assert(sizeof(ProxerDelta) == 20, "ProxerDelta");

// Proxer (prox.obj, 20 bytes, one: Proxer::global / g_proxer). Record k is at records + k * record_size:
// Point2D pos (+0), Point2D vel (+8), then three tables of n ProxerDelta pointers at +0x10: [0] by car index
// (unsorted), [1] sorted by distance, [2] sorted by interest (update's sort_edges calls).
struct Proxer {
    int32_t n;                         // +0x00 the number of cars
    int32_t focus;                     // +0x04 the car whose list is being sorted (sort_edges)
    ProxerDelta* deltas;               // +0x08 n (n - 1) / 2 of them (at least 1 allocated)
    int32_t record_size;               // +0x0c n * 12 + 16
    uint8_t* records;                  // +0x10 n records
};
static_assert(sizeof(Proxer) == 20, "Proxer");

// the notes' header (notes.obj: generate_notes_for builds it, GetDriverNotes checks it): a 'TNDA' resource of
// version 7, then datasize bytes of notes
struct NotesHeader {
    int32_t version;                   // +0x00 7
    int32_t size;                      // +0x04 0x40
    int32_t track;                     // +0x08
    int32_t driver;                    // +0x0c
    int32_t strength;                  // +0x10
    char car[0x20];                    // +0x14
    uint16_t driver_crc;               // +0x34 calc_driver_crc
    uint16_t ili_crc;                  // +0x36 get_ili_info: CRC16 of the line's 8 bytes at +0xdc
    int32_t nodes;                     // +0x38 get_ili_info: the line's +8
    int32_t datasize;                  // +0x3c (nodes * 8)
};
static_assert(sizeof(NotesHeader) == 0x40, "NotesHeader");

// UIDialogItem (replay.obj's constructor 0x4091c0 stores its 14 arguments): 56 bytes
struct UIDialogItem { uint32_t w[14]; };
static_assert(sizeof(UIDialogItem) == 56, "UIDialogItem");

// GhostCarViewer (gcviewer.obj, 0x2558 bytes, vtable 0x4db738 over UICustomControl's 0x4db190)
struct GhostInfo { uint8_t b[0xcc]; };        // +4 the name, +0xc8 the lap time (float)
struct SubEntry {                             // a submitted ghost: MemAlloc(0xec)
    char file[0x20];                          // +0x00 its file name
    GhostInfo info;                           // +0x20 (the time at +0xe8)
};
static_assert(sizeof(SubEntry) == 0xec, "SubEntry");
struct Submits {                              // one per track (0..15: + 8 reversed)
    int32_t count;                            // +0x00
    SubEntry* items[16];                      // +0x04 sorted by time
    int32_t current;                          // +0x44
};
static_assert(sizeof(Submits) == 0x48, "Submits");
struct UIData { char name[0x20]; char time[0x20]; };
struct GhostCarViewer {
    void** vtable;                            // +0x00
    int32_t x, y;                             // +0x04 UICustomControl's position
    uint32_t _0c, _10, _14;                   // +0x0c (+0x14 zeroed by the constructor)
    int32_t* p_driver;                        // +0x18 the constructor's arguments: the chosen driver,
    int32_t* p_strength;                      // +0x1c strength
    int32_t* p_track;                         // +0x20 and track
    UIData driver_text;                       // +0x24 the driver's own ghost ("<NONE>")
    UIData submit_text;                       // +0x64 the library's current entry
    char library_text[0x20];                  // +0xa4 "Library (%d/%d)"
    GhostInfo* ghosts[0x800];                 // +0xc4 [strength][driver][track]
    Submits subs[16];                         // +0x20c4
    uint8_t loaded, _2545[3];                 // +0x2544
    uint32_t g_library;                       // +0x2548 UI groups
    uint32_t g_scroll;                        // +0x254c
    uint32_t g_main;                          // +0x2550
    uint32_t g_load;                          // +0x2554
};
static_assert(offsetof(GhostCarViewer, ghosts) == 0xc4 && offsetof(GhostCarViewer, subs) == 0x20c4 &&
              offsetof(GhostCarViewer, loaded) == 0x2544 && sizeof(GhostCarViewer) == 0x2558, "GhostCarViewer");

}  // namespace

// ---- statics -----------------------------------------------------------------------------------------------------
static uint8_t* const g_learn_mode = (uint8_t*)0x004eb648;           // ai_learn_mode
static int32_t* const g_ai_count = (int32_t*)0x004eb66c;             // AICarCount
static int32_t* const g_dash_mode = (int32_t*)0x004eb670;            // the dashboard's page
static uint8_t* const g_dash_toggle = (uint8_t*)0x004eb674;          // '0' toggles it
static uint8_t* const g_ai_active = (uint8_t*)0x004eb678;            // between AIBegin and AIEnd
static float* const g_DU = (float*)0x004eb694;
static float* const g_DV = (float*)0x004eb698;
static void** const g_lounge = (void**)0x004eb8b4;                   // IDriverLounge* Lounge (driver.obj)
static Proxer** const g_proxer_c = (Proxer**)0x004ec480;             // g_proxer
static Proxer** const g_proxer = (Proxer**)0x004ec484;               // Proxer::global
typedef int(__fastcall* ProxSort_t)(Proxer*, Edx, const ProxerDelta*, const ProxerDelta*);
static ProxSort_t* const g_global_sort = (ProxSort_t*)0x004ec488;    // Proxer::global_sort
static GhostCarViewer** const g_gcv = (GhostCarViewer**)0x004ec838;  // GhostCarViewer::global
static AICarInfo** const g_ai_cars = (AICarInfo**)0x00509920;        // ai_cars[16]
static AICarInfo** const g_focus = (AICarInfo**)0x00509984;          // the dashboard's focus car
static int32_t* const g_strength = (int32_t*)0x00509990;             // AIGetStrength
static float* const g_status_f = (float*)0x005099a0;                 // status (footag2): a float,
static uint8_t* const g_status_b = (uint8_t*)0x005099a4;             //   and a flag the key handler clears
static float* const g_cursor = (float*)0x005099b8;                   // the dashboard's cursor, a P3
static uint32_t* const g_cam_x = (uint32_t*)0x00521074;              // phystask.obj: the point 'B'/'b' swap
static uint32_t* const g_cam_z = (uint32_t*)0x0052107c;              //   with the cursor (x, z)
static uint8_t* const g_notes_abort = (uint8_t*)0x00520ad0;          // hard_notes_abort
static int32_t* const g_notes_made = (int32_t*)0x00520ae8;           // check_learning's counts
static int32_t* const g_notes_missing = (int32_t*)0x00520aec;
static void** const g_notes_data = (void**)0x00520b10;               // aicar_record_notes
static int32_t* const g_notes_size = (int32_t*)0x00520b14;

// constants, at their v1.0 values
static const float k_dash_step = 0.0003000000142492354f;             // 0x399d4952
static const float k_five = 5.0f;
static const float k_gear_mass = 1.0000000656873453e-05f;            // 0x3727c5ad
static const float k_inch = 0.02539999969303608f;                    // 0x3cd013a9
static const float k_gear_scale = 0.12957480549812317f;              // 0x3e04af42
static const float k_prox_minus_one = -1.0f, k_prox_one = 1.0f;
static const float k_prox_tenth = 0.10000000149011612f;              // 0x3dcccccd
static const float k_prox_slow = 0.8888888955116272f;                // 0x3f638e39
static const float k_prox_never = 100.0f;
static const float k_zero = 0.0f;

// ---- the functions they call, by address ---------------------------------------------------------------------------
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* Free_t)(void*);
static const Free_t MemFree = (Free_t)0x00414300;
static const Free_t op_delete = (Free_t)0x00414390;
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t crt_sprintf = (Sprintf_t)0x004cf0a0;
typedef void(__cdecl* Qsort_t)(void*, unsigned, unsigned, uint32_t);
static const Qsort_t crt_qsort = (Qsort_t)0x004cf160;
typedef void*(__cdecl* Memmove_t)(void*, const void*, unsigned);
static const Memmove_t crt_memmove = (Memmove_t)0x004cf400;
typedef char*(__cdecl* Strncpy_t)(char*, const char*, unsigned);
static const Strncpy_t crt_strncpy = (Strncpy_t)0x004cf3a0;
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
static const Stricmp_t crt_stricmp = (Stricmp_t)0x004da350;
typedef int(__cdecl* Tolower_t)(int);
static const Tolower_t crt_tolower = (Tolower_t)0x004cfd50;

typedef int(__cdecl* GetInt_t)();
static const GetInt_t AIGetTrackNumber = (GetInt_t)0x00420df0;
static const GetInt_t AIGetStrength_o = (GetInt_t)0x0041d310;
static const GetInt_t WorldGetTrackNumber = (GetInt_t)0x00462760;
typedef int(__cdecl* IntInt_t)(int);
static const IntInt_t AIGetDriverForCar = (IntInt_t)0x00420ce0;
typedef const uint8_t*(__cdecl* PtrInt_t)(int);
static const PtrInt_t WorldGetCarEntry = (PtrInt_t)0x00462720;
static const PtrInt_t AIGetSkill = (PtrInt_t)0x00420d90;
static const PtrInt_t AIGetTrackInfo = (PtrInt_t)0x00424e40;
static const PtrInt_t CarMgrGetInfo = (PtrInt_t)0x00464490;
typedef const char*(__cdecl* StrInt_t)(int);
static const StrInt_t GetTrackName = (StrInt_t)0x00406810;
typedef const uint8_t*(__cdecl* PtrVoid_t)();
static const PtrVoid_t WorldGameOptions = (PtrVoid_t)0x004627a0;
typedef const char*(__cdecl* StrVoid_t)();
static const StrVoid_t WorldGetTrackname = (StrVoid_t)0x00462780;
typedef int(__cdecl* IntStr_t)(const char*);
static const IntStr_t GetTrackNumber = (IntStr_t)0x004068b0;
typedef uint8_t(__cdecl* TrackRev_t)(int);
static const TrackRev_t AITrackIsReversed = (TrackRev_t)0x00420e10;
typedef void(__cdecl* VoidStr_t)(const char*);
static const VoidStr_t ResourceSetMustLoad = (VoidStr_t)0x00419a30;
static const VoidStr_t ResourceSetUnload = (VoidStr_t)0x00419bb0;
typedef uint8_t(__cdecl* BoolStr_t)(const char*);
static const BoolStr_t FileCreateDirectory = (BoolStr_t)0x00411d00;
static const BoolStr_t FileRemove = (BoolStr_t)0x00411ba0;
typedef uint8_t(__cdecl* SetupRes_t)(void*, const char*);
static const SetupRes_t CarFileLoadSetupRes = (SetupRes_t)0x00465280;
typedef void(__cdecl* VoidPtr_t)(void*);
static const VoidPtr_t CarFileMakeDefaultSetup = (VoidPtr_t)0x00465780;
typedef uint8_t(__cdecl* CarFileLoad_t)(void*, const char*, uint8_t*);
static const CarFileLoad_t CarFileLoad = (CarFileLoad_t)0x004647d0;
typedef const void*(__cdecl* ResTry_t)(const char*, uint32_t, uint32_t*, int*);
static const ResTry_t ResourceTryDiscardable = (ResTry_t)0x00419f40;
typedef uint8_t(__cdecl* ResForget_t)(const void*);
static const ResForget_t ResourceForget = (ResForget_t)0x0041a450;
typedef void(__cdecl* ResWrite_t)(int, uint32_t, uint32_t);
static const ResWrite_t ResourceWrite = (ResWrite_t)0x0041a5c0;
typedef uint16_t(__cdecl* Crc16_t)(const void*, int);
static const Crc16_t CRC16 = (Crc16_t)0x004a3d70;
typedef void*(__cdecl* ILineTry_t)(const char*, uint8_t);
static const ILineTry_t ILineTry = (ILineTry_t)0x004211d0;
typedef void(__cdecl* Assert_t)(int, const char*, ...);
static const Assert_t ASSERT_MSG_o = (Assert_t)0x00424690;
typedef void(__cdecl* Munge_t)(char*, const char*);
static const Munge_t munge_carname_to_4char = (Munge_t)0x0040dc70;
typedef int(__cdecl* FileCreate_t)(const char*);
static const FileCreate_t FileCreate = (FileCreate_t)0x004115f0;
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
static const FileWrite_t FileWrite = (FileWrite_t)0x00411a30;
typedef void(__cdecl* FileClose_t)(int*);
static const FileClose_t FileClose = (FileClose_t)0x00411850;
typedef void(__fastcall* ThisVoid_t)(void*, Edx);
static const ThisVoid_t World_ctor = (ThisVoid_t)0x00462a90;
typedef void(__cdecl* WorldFn_t)(void*);
static const WorldFn_t LoadRace = (WorldFn_t)0x0040a840;
static const WorldFn_t UnloadRace = (WorldFn_t)0x0040a870;
typedef void(__cdecl* DoRace_t)(void*, void*, void*);
static const DoRace_t DoRace = (DoRace_t)0x00401e10;
typedef void(__cdecl* SetDriverMap_t)(int*, int);
static const SetDriverMap_t AISetDriverMap = (SetDriverMap_t)0x00420c50;
typedef void(__cdecl* Void_t)();
static const Void_t AIResetDriverMap = (Void_t)0x00420c30;
static const ThisVoid_t ConstIdealLine_ctor = (ThisVoid_t)0x00422370;
static const ThisVoid_t ConstIdealLine_dtor = (ThisVoid_t)0x00422390;
static const ThisVoid_t IdealLine_reset_bead_position = (ThisVoid_t)0x00421520;
// the profiler hooks AIBGUpdate calls through (pointers in .data)
typedef int(__cdecl* ProfStart_t)(const char*);
typedef void(__cdecl* ProfStop_t)(int);
static Void_t* const i_pr_overhead_begin = (Void_t*)0x004e642c;
static Void_t* const i_pr_overhead_end = (Void_t*)0x004e6430;
static ProfStart_t* const i_prof_start = (ProfStart_t*)0x004e6434;
static ProfStop_t* const i_prof_stop = (ProfStop_t*)0x004e6438;
// the UI and files (the ghost viewer)
typedef void(__fastcall* AddNotif_t)(void*, Edx, int, const void*, unsigned);
static const AddNotif_t UICustomControl_AddNotification = (AddNotif_t)0x0047e840;
typedef void(__fastcall* RemNotif_t)(void*, Edx, const void*);
static const RemNotif_t UICustomControl_RemoveNotification = (RemNotif_t)0x0047e860;
typedef void(__fastcall* AddItems_t)(void*, Edx, UIDialogItem*);
static const AddItems_t UICustomControl_AddItems = (AddItems_t)0x0047e870;
typedef void(__cdecl* Group_t)(unsigned);
static const Group_t UIHideGroup = (Group_t)0x004791b0;
static const Group_t UIShowGroup = (Group_t)0x004791e0;
typedef void(__cdecl* DratBox_t)(const char*, const char*, void*);
static const DratBox_t UIDoDratBox = (DratBox_t)0x00479280;
typedef UIDialogItem*(__fastcall* ItemCtor_t)(UIDialogItem*, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                              uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                              uint32_t, uint32_t);
static const ItemCtor_t UIDialogItem_ctor = (ItemCtor_t)0x004091c0;
typedef const char*(__cdecl* TimeString_t)(uint32_t, uint8_t);   // PhysicsTimeString(float, bool): the float as bits
static const TimeString_t PhysicsTimeString = (TimeString_t)0x0042bf30;
typedef uint8_t(__cdecl* FileCopy_t)(const char*, const char*);
static const FileCopy_t FileCopy = (FileCopy_t)0x00411e40;
typedef void(__cdecl* SetWritable_t)(const char*, uint8_t);
static const SetWritable_t FileSetWritable = (SetWritable_t)0x00411b80;
typedef uint8_t(__cdecl* System_t)(int*, int*, const char*, ...);
static const System_t Win32System = (System_t)0x00412d90;
typedef void*(__cdecl* FindFirst_t)(const char*, char*, int);
static const FindFirst_t FileFindFirst = (FindFirst_t)0x00411c20;
typedef uint8_t(__cdecl* FindNext_t)(void*, char*, int);
static const FindNext_t FileFindNext = (FindNext_t)0x00411c80;
typedef void(__cdecl* FindClose_t)(void*);
static const FindClose_t FileFindClose = (FindClose_t)0x00411cd0;
typedef void*(__cdecl* GhostLoad_t)(const char*, uint8_t);
static const GhostLoad_t GhostLoad = (GhostLoad_t)0x0040ebe0;

// this file's own functions, by address (so a hooked rewrite is what runs)
typedef void(__fastcall* InfoInit_t)(AICarInfo*, Edx, int);
static const InfoInit_t AICarInfo_init_o = (InfoInit_t)0x0041c070;
typedef AICarInfo*(__fastcall* InfoCtor_t)(AICarInfo*, Edx, void*, int);
static const InfoCtor_t AICarInfo_ctor_car_o = (InfoCtor_t)0x0041c110;
static const InfoCtor_t AICarInfo_ctor_aicar_o = (InfoCtor_t)0x0041c140;
typedef void(__fastcall* InfoVoid_t)(AICarInfo*, Edx);
static const InfoVoid_t AICarInfo_UpdatePos_o = (InfoVoid_t)0x0041c170;
typedef int(__cdecl* Iterate_t)(char*, uint8_t*, int, int, int, int, const char*);
static const Iterate_t IterateILIname_o = (Iterate_t)0x0041c1b0;
typedef void(__cdecl* GearRatios_t)(void*, const char*, uint32_t);   // the float argument is moved as bits
static const GearRatios_t setup_gear_ratios_o = (GearRatios_t)0x0041c3b0;
typedef void(__cdecl* ProxBegin_t)(int);
static const ProxBegin_t Proxer_Begin_o = (ProxBegin_t)0x004234b0;
static const Void_t Proxer_End_o = (Void_t)0x004234f0;
typedef Proxer*(__fastcall* ProxCtor_t)(Proxer*, Edx, int);
static const ProxCtor_t Proxer_ctor_o = (ProxCtor_t)0x00422d40;
typedef void(__fastcall* ProxVoid_t)(Proxer*, Edx);
static const ProxVoid_t Proxer_dtor_o = (ProxVoid_t)0x00422e00;
static const ProxVoid_t Proxer_init_tabs_o = (ProxVoid_t)0x00422e40;
static const ProxVoid_t Proxer_update_o = (ProxVoid_t)0x00422fd0;
typedef void(__fastcall* ProxUpdObj_t)(Proxer*, Edx, int, const Point2D*, const Point2D*);
static const ProxUpdObj_t Proxer_update_object_o = (ProxUpdObj_t)0x00422f90;
typedef void(__fastcall* SortEdges_t)(Proxer*, Edx, ProxerDelta**, uint32_t, int);
static const SortEdges_t Proxer_sort_edges_o = (SortEdges_t)0x004232d0;
static const uint32_t Proxer_compare_edges_by_interest_o = 0x004230b0;
static const uint32_t Proxer_compare_edges_by_distance_o = 0x00423220;
static const uint32_t Proxer_static_sort_f_o = 0x004232b0;
typedef ProxerDelta*(__fastcall* DeltaCtor_t)(ProxerDelta*, Edx);
static const DeltaCtor_t ProxerDelta_ctor_o = (DeltaCtor_t)0x00423520;
typedef void*(__cdecl* GetNotes_t)(int, int, int, const char*, uint8_t);
static const GetNotes_t GetDriverNotes_o = (GetNotes_t)0x004242b0;
typedef uint8_t(__cdecl* NotesRes_t)(int, char*, int, int, int, const char*);
static const NotesRes_t get_notes_resname_o = (NotesRes_t)0x004244b0;
typedef uint16_t(__cdecl* DriverCrc_t)(int, int);
static const DriverCrc_t calc_driver_crc_o = (DriverCrc_t)0x00424530;
typedef uint64_t(__cdecl* IliInfo_t)(int, int, int, const char*);   // struct ili_info {int nodes; u16 crc}: edx:eax
static const IliInfo_t get_ili_info_o = (IliInfo_t)0x00424560;
typedef void(__cdecl* GenNotes_t)(int, int, int, const char*);
static const GenNotes_t generate_notes_for_o = (GenNotes_t)0x004246a0;
typedef void(__cdecl* NotesFname_t)(char*, int, int, int, const char*);
static const NotesFname_t get_notes_fname_o = (NotesFname_t)0x004249e0;
static const NotesFname_t GetUniqueDSTCMunge_o = (NotesFname_t)0x00424b30;
typedef uint8_t(__cdecl* InvDSTC_t)(int*, int*, int*, char*, const char*);
static const InvDSTC_t InverseDSTC_o = (InvDSTC_t)0x00424a20;
typedef uint8_t(__cdecl* Hex2Int_t)(int*, uint32_t);                 // the char pushed as a dword
static const Hex2Int_t hex2int_o = (Hex2Int_t)0x00424a90;
typedef void(__cdecl* DSTCFname_t)(char*, const char*, const char*, int, int, int, const char*);
static const DSTCFname_t GetUniqueDSTCFname_o = (DSTCFname_t)0x00424ae0;
typedef void(__fastcall* GcvVoid_t)(GhostCarViewer*, Edx);
static const GcvVoid_t GCV_update_widgets_o = (GcvVoid_t)0x004259a0;
static const GcvVoid_t GCV_update_submit_o = (GcvVoid_t)0x004258b0;
static const GcvVoid_t GCV_update_driver_o = (GcvVoid_t)0x004257e0;
static const GcvVoid_t GCV_set_driver_ghost_o = (GcvVoid_t)0x004255e0;
static const GcvVoid_t GCV_next_submit_o = (GcvVoid_t)0x00425130;
static const GcvVoid_t GCV_prev_submit_o = (GcvVoid_t)0x00425170;
static const GcvVoid_t GCV_load_o = (GcvVoid_t)0x00425d00;
static const GcvVoid_t GCV_delete_ghost_o = (GcvVoid_t)0x00425a20;
typedef void(__fastcall* GcvText_t)(GhostCarViewer*, Edx, const GhostInfo*, UIData*);
static const GcvText_t GCV_update_text_o = (GcvText_t)0x00425840;
typedef void(__fastcall* GcvAddDriver_t)(GhostCarViewer*, Edx, const GhostInfo*, int, int, int);
static const GcvAddDriver_t GCV_add_driver_ghost_o = (GcvAddDriver_t)0x004259d0;
typedef void(__fastcall* GcvAddSub_t)(GhostCarViewer*, Edx, const uint8_t*, const char*);
static const GcvAddSub_t GCV_add_submitted_ghost_o = (GcvAddSub_t)0x00425b40;
typedef GhostCarViewer*(__fastcall* GcvCtor_t)(GhostCarViewer*, Edx, int32_t*, int32_t*, int32_t*);
static const GcvCtor_t GCV_ctor_o = (GcvCtor_t)0x00425f30;

// strings the original passes by address
static const char* const k_str_lameness = (const char*)0x004eb70c;   // "lameness prevails in AI."
static const char* const k_str_notes_star = (const char*)0x004eb700; // "*notes\\"
static const char* const k_str_empty_ai = (const char*)0x004eb708;   // ""
static const char** const k_ext_ilq = (const char**)0x004db508;      // -> ".ilq"
static const char** const k_ext_ilg = (const char**)0x004db50c;      // -> ".ilg"
static const char** const k_default_ili = (const char**)0x004db530;  // -> "default.ili", "rdefault.ili"
static const char* const k_str_aidef = (const char*)0x004eb728;      // "aidef.ccs"
static const uint8_t* const k_str_car = (const uint8_t*)0x004eb734;  // ".car" (5 bytes copied)
static const uint8_t* const k_str_cf = (const uint8_t*)0x004eb73c;   // ".cf" (4 bytes copied)
static const float* const k_gears5 = (const float*)0x004eb680;       // the gear spread of a five-speed box
static const float* const k_gears_other = (const float*)0x004db510;  // and of any other
static const char* const k_str_aibgupdate = (const char*)0x004eb874; // "AIBGUpdate"
static const char* const k_fmt_notes_mismatch = (const char*)0x004ec59c;
static const char* const k_fmt_notes_vers = (const char*)0x004ec5d4;
static const char* const k_str_notes_dir = (const char*)0x004ec600;  // "notes"
static const char* const k_str_dnt = (const char*)0x004ec608;        // ".dnt"
static const char* const k_str_notes_star2 = (const char*)0x004ec610;// "*notes\\"
static const char* const k_str_dnt2 = (const char*)0x004ec618;       // ".dnt"
static const char* const k_str_empty_notes = (const char*)0x004ec620;// ""
static const uint8_t* const k_str_trk = (const uint8_t*)0x004ec624;  // ".trk" (5 bytes copied)
static const char* const k_fmt_no_line = (const char*)0x004ec62c;
static const char* const k_fmt_cant_create = (const char*)0x004ec65c;
static const char* const k_str_no_data = (const char*)0x004ec66c;
static const char* const k_str_notes_dir2 = (const char*)0x004ec694; // "notes"
static const char* const k_str_dnt3 = (const char*)0x004ec69c;       // ".dnt"
static const char* const k_str_notes_slash = (const char*)0x004ec6a4;// "notes\\"
static const char* const k_fmt_sss = (const char*)0x004ec6ac;        // "%s%s%s"
static const char* const k_fmt_dstc = (const char*)0x004ec6b4;       // "%x%x%x%s"
static const char** const k_ghost_dir = (const char**)0x004db72c;    // -> "\\\\coffee\\vc\\ghosts\\"
static const char** const k_ext_ilq2 = (const char**)0x004db728;     // -> ".ilq"
static const char* const k_str_gcf = (const char*)0x004ec8d4;        // ".gcf"
static const char* const k_str_notes_slash2 = (const char*)0x004ec8dc;
static const char* const k_str_notes_slash3 = (const char*)0x004ec8e4;
static const char* const k_fmt_gcf2ili = (const char*)0x004ec8ec;
static const char* const k_str_grak1 = (const char*)0x004ec920, *const k_str_nopath = (const char*)0x004ec908;
static const char* const k_str_grak2 = (const char*)0x004ec938, *const k_str_choked = (const char*)0x004ec928;
static const char* const k_str_grak3 = (const char*)0x004ec954, *const k_str_nocopy = (const char*)0x004ec940;
static const char* const k_str_none = (const char*)0x004ec95c;       // "<NONE>"
static const char* const k_fmt_library = (const char*)0x004ec964;    // "Library (%d/%d)"
static const char* const k_str_library_empty = (const char*)0x004ec974;
static const uint8_t* const k_str_tmp = (const uint8_t*)0x004ec99c;  // "*.tmp" (6 bytes copied)
static const char* const k_str_gcf_glob = (const char*)0x004ec9a4;   // "notes\\*.gcf"
static const char* const k_fmt_notes_s = (const char*)0x004ec9b0;    // "notes\\%s"
static const UIDialogItem* const k_item_end = (const UIDialogItem*)0x00578d08;   // the dialog terminator

// ---- footprint helpers ---------------------------------------------------------------------------------------------
static void fp_info_state(Footprint& f, AICarInfo* ci) {       // UpdatePos: the info's pos/vel and its Proxer record
    f.add(ci, 16, "AICarInfo pos/vel");
    Proxer* p = *g_proxer;
    if (p && p->records) f.add(p->records + p->record_size * ci->index, 16, "Proxer record");
}
static void fp_proxer_all(Footprint& f, Proxer* p) {           // a Proxer's own fields and both its tables
    if (!p) return;
    f.add(p, sizeof(Proxer), "Proxer");
    if (p->records && p->n > 0) f.add(p->records, (uint32_t)(p->n * p->record_size), "Proxer records");
    int m = p->n * (p->n - 1) / 2;
    if (p->deltas && m > 0) f.add(p->deltas, (uint32_t)m * 20, "Proxer deltas");
    f.add(g_global_sort, 4, "Proxer::global_sort");
}

// =====================================================================================================================
// ai.obj
// =====================================================================================================================

// AICarInfo::init (0x41c070): the index, then the first ideal line that loads: IterateILIname walks the candidate
// names (the driver's own .ilq, then .ilg, then the track's default, then the reversed default) until one loads
// or it returns 5. The reversed flag reaches load() as the stack dword at &rev: rev, then the low three bytes of
// the strength (the original's locals), reproduced.
static void __fastcall AICarInfo_init(AICarInfo* self, Edx, int index) {
    char name[0x100];
    uint8_t rev;
    self->index = index;
    self->aicar = 0;
    self->car = 0;
    int track = AIGetTrackNumber();
    int driver = AIGetDriverForCar(self->index);
    int strength = AIGetStrength_o();
    const char* car = (const char*)WorldGetCarEntry(self->index) + 0x11;
    int k = 0;
    for (;;) {
        k = IterateILIname_o(name, &rev, k, driver, track, strength, car);
        if (k == 5) break;
        void* line = self->line;
        uint32_t arg = rev | ((uint32_t)strength << 8);
        if (VFN(line, 4, uint8_t, const char*, uint32_t)(line, 0, name, arg)) break;
    }
}
static void fp_info_init(Footprint& f, AICarInfo*, Edx, int) { f.replay_only = "loads the car's ideal line"; }
PORT_FN(0x0041c070, "AICarInfo::init", AICarInfo_init, fp_info_init)

// AICarInfo::AICarInfo(Car*, int) (0x41c110) and (AICar*, int) (0x41c140)
static AICarInfo* __fastcall AICarInfo_ctor_car(AICarInfo* self, Edx, void* car, int index) {
    ConstIdealLine_ctor(self->line, 0);
    AICarInfo_init_o(self, 0, index);
    self->aicar = 0;
    self->car = (uint8_t*)car;
    return self;
}
static void fp_info_ctor(Footprint& f, AICarInfo*, Edx, void*, int) { f.replay_only = "loads the car's ideal line"; }
PORT_FN(0x0041c110, "AICarInfo::AICarInfo(Car)", AICarInfo_ctor_car, fp_info_ctor)

static AICarInfo* __fastcall AICarInfo_ctor_aicar(AICarInfo* self, Edx, void* aicar, int index) {
    ConstIdealLine_ctor(self->line, 0);
    AICarInfo_init_o(self, 0, index);
    self->aicar = aicar;
    self->car = (uint8_t*)aicar;
    return self;
}
PORT_FN(0x0041c140, "AICarInfo::AICarInfo(AICar)", AICarInfo_ctor_aicar, fp_info_ctor)

// AICarInfo::UpdatePos (0x41c170): the car's x/z position and velocity, copied as bits, then handed to the Proxer
static void __fastcall AICarInfo_UpdatePos(AICarInfo* self, Edx) {
    const uint8_t* car = self->car;
    cp4(&self->vel.x, car + 0x234);
    cp4(&self->vel.z, car + 0x23c);
    cp4(&self->pos.x, car + 0x5c);
    cp4(&self->pos.z, car + 0x64);
    Proxer_update_object_o(*g_proxer, 0, self->index, &self->pos, &self->vel);
}
static void fp_info_update_pos(Footprint& f, AICarInfo* self, Edx) { fp_info_state(f, self); }
PORT_FN(0x0041c170, "AICarInfo::UpdatePos", AICarInfo_UpdatePos, fp_info_update_pos)

// IterateILIname (0x41c1b0): the k-th candidate ideal line's name, and whether it's to be driven reversed;
// returns the next k (5: none left)
static int __cdecl IterateILIname(char* name, uint8_t* reversed, int k, int driver, int track, int strength,
                                  const char* car) {
    uint8_t rev = AITrackIsReversed(track);
    *reversed = 0;
    switch ((uint32_t)k) {
    case 0:
        GetUniqueDSTCFname_o(name, k_str_notes_star, *k_ext_ilq, driver, strength, track, car);
        return k + 1;
    case 1:
        GetUniqueDSTCFname_o(name, k_str_empty_ai, *k_ext_ilg, driver, strength, track, car);
        return k + 1;
    case 2:
        s_copy(name, k_default_ili[rev]);
        return k + 1;
    case 3:
        if ((int)rev >= track) return k + 2;
        *reversed = 1;
        s_copy(name, k_default_ili[0]);
        return k + 1;
    case 4:
        return k + 1;
    default:
        LogPanic(k_str_lameness);
        return k + 1;
    }
}
static void fp_iterate(Footprint& f, char* name, uint8_t* reversed, int, int, int, int, const char*) {
    f.add(name, 0x100, "name");
    f.add(reversed, 1, "reversed");
}
PORT_FN(0x0041c1b0, "IterateILIname", IterateILIname, fp_iterate)

// AIGetLine (0x41c2e0): the car's ideal line (ai_cars[i] + 0x10, whether or not there's a car)
static void* __cdecl AIGetLine(int i) { return (uint8_t*)g_ai_cars[i] + 0x10; }
static void fp_nothing_int(Footprint&, int) {}
PORT_FN(0x0041c2e0, "AIGetLine", AIGetLine, fp_nothing_int)

// AICarSetup (0x41c2f0): the AI's car set-up: aidef.ccs (or the default set-up), its gear ratios from the car
// file spread by the driver's skill for this track, and the skill's scaling of +4, +0x48 and +0x4c
static void __cdecl AICarSetup(uint8_t* setup, const char* car, int driver, const uint8_t* world) {
    if (!CarFileLoadSetupRes(setup, k_str_aidef)) {
        alignas(4) uint8_t def[0xd4];
        CarFileMakeDefaultSetup(def);
        memcpy(setup, def, 0x8c);
    }
    const uint8_t* skill = AIGetSkill(driver);
    int t = GetTrackNumber((const char*)world + 8);
    int idx = t + (int)((uint32_t)world[0xcd1] * 8);
    const uint8_t* ts = skill + idx * 24;
    setup_gear_ratios_o(setup, car, Ub(ts + 0x94));
    Fat(setup, 4) = (float)(D(*(const float*)(ts + 0x90)) * *(const float*)(skill + 0x20));
    Fat(setup, 0x4c) = (float)((D(*(const float*)(ts + 0x9c)) * *(const float*)(skill + 0x6c)) * Fat(setup, 0x4c));
    Fat(setup, 0x48) = (float)((D(*(const float*)(ts + 0x9c)) * *(const float*)(skill + 0x6c)) * Fat(setup, 0x48));
}
static void fp_car_setup(Footprint& f, uint8_t*, const char*, int, const uint8_t*) { f.replay_only = "loads car files"; }
PORT_FN(0x0041c2f0, "AICarSetup", AICarSetup, fp_car_setup)

// setup_gear_ratios (0x41c3b0): the ratios from the car file's tyre size, engine and final drive: the top
// ratio's speed, spread over a five-speed table (or the other one), first gear fixed at 39. No car file: fixed
// ratios.
static void __cdecl setup_gear_ratios(uint8_t* setup, const char* car, float spread) {
    char res[64], fname[64];
    alignas(4) uint8_t cf[0x228];                      // the CarFile: the rest of the original's frame
    s_copy(res, car);
    memcpy(res + strlen(res), k_str_car, 5);
    ResourceSetMustLoad(res);
    s_copy(fname, car);
    memcpy(fname + strlen(fname), k_str_cf, 4);
    if (CarFileLoad(cf, fname, 0)) {
        double a = (D(Fat(cf, 0x1b0)) * Fat(cf, 0x1ac)) * k_gear_mass + D(Fat(cf, 0x1b4)) * k_inch;
        double b = (D(Fat(cf, 0xcc)) - Fat(cf, 0xc8)) * Fat(setup, 0) + Fat(cf, 0xc8);
        const float* table = Iat(cf, 0x108) == 5 ? k_gears5 : k_gears_other;
        double c = a / b;
        c = c * Fat(cf, 0x4c);
        c = c * k_gear_scale;
        float top = (float)c;
        for (int i = 0; i < Iat(cf, 0x108); i++) {
            float t = (float)(D(table[i]) * spread);
            if (i == 0) t = Fb(0x421c0000u);           // 39
            Fat(setup, 0x50 + 4 * i) = (float)(D(top) / t);
        }
    } else {
        put4(setup + 0x50, 0x402a3d71u);
        put4(setup + 0x54, 0x3fe3d70au);
        put4(setup + 0x58, 0x3fa66666u);
        put4(setup + 0x5c, 0x3f800000u);
        put4(setup + 0x60, 0x3f3d70a4u);
        put4(setup + 0x64, 0x3f000000u);
        put4(setup + 0x68, 0);
    }
    ResourceSetUnload(res);
}
static void fp_gear_ratios(Footprint& f, uint8_t*, const char*, float) { f.replay_only = "loads the car file"; }
PORT_FN(0x0041c3b0, "setup_gear_ratios", setup_gear_ratios, fp_gear_ratios)

// AITweakCarData (0x41c590): the driver's skill may raise the car's +8 (above 100) and scale +0x150/+0x154
// (above 1), both tests on the bits
static void __cdecl AITweakCarData(uint8_t* cd) {
    int driver = AIGetDriverForCar(Iat(cd, 0x194));
    int strength = AIGetStrength_o();
    void* lounge = *g_lounge;
    const uint8_t* skill = VFN(lounge, 4, const uint8_t*, int, int)(lounge, 0, strength, driver);
    WorldGameOptions();
    WorldGetTrackNumber();
    if (Ib(skill + 0x2c) > 0x42c80000) cp4(cd + 8, skill + 0x2c);
    if (Ib(skill + 0x30) > 0x3f800000) {
        Fat(cd, 0x150) = (float)(D(Fat(cd, 0x150)) * *(const float*)(skill + 0x30));
        Fat(cd, 0x154) = (float)(D(*(const float*)(skill + 0x30)) * Fat(cd, 0x154));
    }
}
static void fp_tweak(Footprint& f, uint8_t* cd) {
    f.add(cd + 8, 4, "CarData+8");
    f.add(cd + 0x150, 8, "CarData+0x150");
}
PORT_FN(0x0041c590, "AITweakCarData", AITweakCarData, fp_tweak)

// AIBegin (0x41c600): the AI's car count (the list's, less the ghost if the options have one), the Proxer
static uint8_t __cdecl AIBegin(const uint8_t* cars, const char*) {
    int n = Iat((void*)cars, 0xc80);
    if (Iat((void*)WorldGameOptions(), 0x1c) != 0) n--;
    *g_ai_count = n;
    put4(g_status_f, 0x42438e39u);
    *g_status_b = 0;
    Proxer_Begin_o(n);
    *g_ai_active = 1;
    return 1;
}
static void fp_ai_begin(Footprint& f, const uint8_t*, const char*) { f.replay_only = "allocates the Proxer"; }
PORT_FN(0x0041c600, "AIBegin", AIBegin, fp_ai_begin)

// AIEnd (0x41c650)
static void __cdecl AIEnd() {
    *g_ai_active = 0;
    Proxer_End_o();
}
static void fp_ai_end(Footprint& f) { f.replay_only = "frees the Proxer"; }
PORT_FN(0x0041c650, "AIEnd", AIEnd, fp_ai_end)

// AIDashboardKey (0x41c660): the AI dashboard's keys. '?'/'h' 'i' 'l' 's' pick the page; on pages 0-2 '0' toggles
// the display, 'Q'/'q' 'W'/'w' nudge DV/DU, 'v' puts the cursor on the focus car, 'V' teleports the focus car
// there, 'B'/'b' swap the cursor with phystask's point. Returns whether the key was taken.
static void dash_teleport() {
    g_cursor[1] = (float)(D(g_cursor[1]) + k_five);
    AICarInfo* ci = *g_focus;
    Point2D dir;
    put4(&dir.x, 0);
    put4(&dir.z, 0x3f800000u);
    uint8_t* car = ci->car;
    VFN(car, 0x40, void, const float*, const Point2D*)(car, 0, g_cursor, &dir);
    AICarInfo_UpdatePos_o(ci, 0);
    IdealLine_reset_bead_position(ci->line, 0);
    g_cursor[1] = (float)(D(g_cursor[1]) - k_five);
}
static uint8_t __cdecl AIDashboardKey(uint16_t key16) {
    int key = key16;
    uint8_t taken = 1;
    switch (key) {
    case 0x3f: case 0x68: *g_dash_mode = 0; break;
    case 0x69: *g_dash_mode = 1; break;
    case 0x6c: *g_dash_mode = 2; break;
    case 0x73:
        if (WorldGameOptions()[0x25] == 0) *g_dash_mode = 3;
        break;
    default: taken = 0; break;
    }
    if (taken) goto clear;
    if (*g_dash_mode < 3) {
        taken = 1;
        switch (key) {
        case 0x30: *g_dash_toggle ^= 1; break;
        case 0x42: put4(&g_cursor[0], *g_cam_x); put4(&g_cursor[2], *g_cam_z); break;
        case 0x51: *g_DV = (float)(D(*g_DV) + k_dash_step); break;
        case 0x56: dash_teleport(); break;
        case 0x57: *g_DU = (float)(D(*g_DU) + k_dash_step); break;
        case 0x62: *g_cam_x = Ub(&g_cursor[0]); *g_cam_z = Ub(&g_cursor[2]); break;
        case 0x71: *g_DV = (float)(D(*g_DV) - k_dash_step); break;
        case 0x76: {
            AICarInfo* ci = *g_focus;
            uint32_t x = Ub(&ci->pos.x);
            put4(&g_cursor[1], 0);
            put4(&g_cursor[0], x);
            cp4(&g_cursor[2], &ci->pos.z);
            break;
        }
        case 0x77: *g_DU = (float)(D(*g_DU) - k_dash_step); break;
        default: taken = 0; break;
        }
    }
    if (taken) goto clear;
    taken = 1;
    switch (*g_dash_mode) {
    case 0:
        if (key == 0x3f || key == 0x48) *g_dash_mode = 1;
        else taken = 0;
        goto g_key;
    case 1: taken = 0; goto tail;
    case 3: goto g_key;
    default: goto tail;
    }
g_key:
    if (crt_tolower(key) != 0x67) taken = 0;
    *g_dash_mode = 1;
tail:
    if (!taken && *g_dash_mode == 2) return taken;
clear:
    *g_status_b = 0;
    return taken;
}
static void fp_dash_key(Footprint& f, uint16_t key) {
    if (key == 0x56) { f.replay_only = "'V' teleports the focus car"; return; }
    f.add(g_dash_mode, 4, "dashboard page");
    f.add(g_dash_toggle, 1, "dashboard toggle");
    f.add(g_DU, 8, "DU, DV");
    f.add(g_cursor, 12, "dashboard cursor");
    f.add(g_status_b, 1, "status flag");
    f.add(g_cam_x, 12, "phystask's point");
}
PORT_FN(0x0041c660, "AIDashboardKey", AIDashboardKey, fp_dash_key)

// AIResetCars (0x41d1a0) and AIBGUpdate (0x41d1c0): every registered car's position to the Proxer; AIBGUpdate then
// updates the Proxer, inside the profiler's "AIBGUpdate" bracket
static void __cdecl AIResetCars() {
    for (AICarInfo** p = g_ai_cars; p < g_ai_cars + 16; p++)
        if (*p) AICarInfo_UpdatePos_o(*p, 0);
}
static void fp_all_cars(Footprint& f) {
    for (int i = 0; i < 16; i++)
        if (g_ai_cars[i]) fp_info_state(f, g_ai_cars[i]);
}
PORT_FN(0x0041d1a0, "AIResetCars", AIResetCars, fp_all_cars)

static void __cdecl AIBGUpdate() {
    (*i_pr_overhead_begin)();
    int prof = (*i_prof_start)(k_str_aibgupdate);
    (*i_pr_overhead_end)();
    for (AICarInfo** p = g_ai_cars; p < g_ai_cars + 16; p++)
        if (*p) AICarInfo_UpdatePos_o(*p, 0);
    Proxer_update_o(*g_proxer, 0);
    (*i_pr_overhead_begin)();
    (*i_prof_stop)(prof);
    (*i_pr_overhead_end)();
}
static void fp_bg_update(Footprint& f) {
    fp_all_cars(f);
    fp_proxer_all(f, *g_proxer);
}
PORT_FN(0x0041d1c0, "AIBGUpdate", AIBGUpdate, fp_bg_update)

// AIRegisterAICar / PlayCar / NetCar (0x41d220 / 0x41d260 / 0x41d2a0) and AIUnregisterCar (0x41d2e0)
static void __cdecl AIRegisterAICar(void* car, int i) {
    AICarInfo* p = (AICarInfo*)MemAlloc(0x68);
    g_ai_cars[i] = p ? AICarInfo_ctor_aicar_o(p, 0, car, i) : 0;
}
static void fp_register(Footprint& f, void*, int) { f.replay_only = "allocates the car's AICarInfo"; }
PORT_FN(0x0041d220, "AIRegisterAICar", AIRegisterAICar, fp_register)

static void __cdecl AIRegisterPlayCar(void* car, int i) {
    AICarInfo* p = (AICarInfo*)MemAlloc(0x68);
    g_ai_cars[i] = p ? AICarInfo_ctor_car_o(p, 0, car, i) : 0;
}
PORT_FN(0x0041d260, "AIRegisterPlayCar", AIRegisterPlayCar, fp_register)

static void __cdecl AIRegisterNetCar(void* car, int i) {
    AICarInfo* p = (AICarInfo*)MemAlloc(0x68);
    g_ai_cars[i] = p ? AICarInfo_ctor_car_o(p, 0, car, i) : 0;
}
PORT_FN(0x0041d2a0, "AIRegisterNetCar", AIRegisterNetCar, fp_register)

static void __cdecl AIUnregisterCar(void*, int i) {
    AICarInfo** slot = &g_ai_cars[i];
    AICarInfo* p = *slot;
    if (p) {
        ConstIdealLine_dtor(p->line, 0);
        op_delete(p);
    }
    *slot = 0;
}
static void fp_unregister(Footprint& f, void*, int) { f.replay_only = "frees the car's AICarInfo"; }
PORT_FN(0x0041d2e0, "AIUnregisterCar", AIUnregisterCar, fp_unregister)

// the getters (0x41d310...)
static int __cdecl AIGetStrength() { return *g_strength; }
static void fp_nothing(Footprint&) {}
PORT_FN(0x0041d310, "AIGetStrength", AIGetStrength, fp_nothing)

static void __cdecl AISetStrength(int s) { *g_strength = s; }
static void fp_set_strength(Footprint& f, int) { f.add(g_strength, 4, "AI strength"); }
PORT_FN(0x0041d320, "AISetStrength", AISetStrength, fp_set_strength)

static int __cdecl AICarCount() { return *g_ai_count; }
PORT_FN(0x0041d330, "AICarCount", AICarCount, fp_nothing)

static uint8_t __cdecl AILearnMode() { return *g_learn_mode; }
PORT_FN(0x0041d340, "AILearnMode", AILearnMode, fp_nothing)

static int __cdecl AIGetMaxGhostSubmitTicks() { return Iat((void*)AIGetTrackInfo(AIGetTrackNumber()), 0xc); }
PORT_FN(0x0041d350, "AIGetMaxGhostSubmitTicks", AIGetMaxGhostSubmitTicks, fp_nothing)

static uint8_t __cdecl IsAI(int i) { return g_ai_cars[i]->aicar != 0; }
PORT_FN(0x0041d370, "_IsAI", IsAI, fp_nothing_int)

// =====================================================================================================================
// prox.obj
// =====================================================================================================================

// Proxer::Proxer (0x422d40)
static Proxer* __fastcall Proxer_ctor(Proxer* self, Edx, int n) {
    self->n = n;
    int m = n * (n - 1) / 2;
    if (m <= 1) m = 1;
    ProxerDelta* d = (ProxerDelta*)MemAlloc(m * 20);
    if (d) {
        ProxerDelta* p = d;
        for (int k = m - 1; k >= 0; k--, p++) ProxerDelta_ctor_o(p, 0);
        self->deltas = d;
    } else {
        self->deltas = 0;
    }
    int size = n * 12 + 16;
    int total = n * size;
    self->record_size = size;
    uint8_t* r = (uint8_t*)MemAlloc(total);
    self->records = r;
    if (r && self->deltas) {
        memset(r, 0, (uint32_t)total);
        Proxer_init_tabs_o(self, 0);
    }
    return self;
}
static void fp_proxer_ctor(Footprint& f, Proxer*, Edx, int) { f.replay_only = "allocates the Proxer's tables"; }
PORT_FN(0x00422d40, "Proxer::Proxer", Proxer_ctor, fp_proxer_ctor)

// Proxer::~Proxer (0x422e00)
static void __fastcall Proxer_dtor(Proxer* self, Edx) {
    if (self->records) {
        op_delete(self->records);
        self->records = 0;
    }
    if (self->deltas) {
        op_delete(self->deltas);
        self->deltas = 0;
    }
    *g_proxer_c = 0;
}
static void fp_proxer_dtor(Footprint& f, Proxer*, Edx) { f.replay_only = "frees the Proxer's tables"; }
PORT_FN(0x00422e00, "Proxer::~Proxer", Proxer_dtor, fp_proxer_dtor)

// Proxer::init_tabs (0x422e40): each pair's delta, pointed to from both cars' three tables. Every field is read
// again at each use, as the original does.
static void __fastcall Proxer_init_tabs(Proxer* self, Edx) {
    int count = 0;
    for (int i = 0; i < self->n; i++) {
        ProxerDelta** ti = (ProxerDelta**)(self->records + self->record_size * i + 0x10);
        ti[i] = 0;
        ti = (ProxerDelta**)(self->records + self->record_size * i + 0x10);
        ti[self->n + i] = 0;
        ti = (ProxerDelta**)(self->records + self->record_size * i + 0x10);
        ti[self->n * 2 + i] = 0;
        int off = count * 20;
        for (int j = i + 1; j < self->n; j++) {
            count++;
            ProxerDelta** a = (ProxerDelta**)(self->records + self->record_size * i + 0x10);
            a[j] = (ProxerDelta*)((uint8_t*)self->deltas + off);
            a = (ProxerDelta**)(self->records + self->record_size * i + 0x10);
            a[self->n + j] = (ProxerDelta*)((uint8_t*)self->deltas + off);
            a = (ProxerDelta**)(self->records + self->record_size * i + 0x10);
            a[self->n * 2 + j] = (ProxerDelta*)((uint8_t*)self->deltas + off);
            ProxerDelta** b = (ProxerDelta**)(self->records + self->record_size * j + 0x10);
            b[i] = (ProxerDelta*)((uint8_t*)self->deltas + off);
            b = (ProxerDelta**)(self->records + self->record_size * j + 0x10);
            b[self->n + i] = (ProxerDelta*)((uint8_t*)self->deltas + off);
            b = (ProxerDelta**)(self->records + self->record_size * j + 0x10);
            b[self->n * 2 + i] = (ProxerDelta*)((uint8_t*)self->deltas + off);
            off += 20;
            ((ProxerDelta*)((uint8_t*)self->deltas + off - 20))->a = (int16_t)i;
            ((ProxerDelta*)((uint8_t*)self->deltas + off - 20))->b = (int16_t)j;
        }
    }
}
static void fp_init_tabs(Footprint& f, Proxer* self, Edx) { fp_proxer_all(f, self); }
PORT_FN(0x00422e40, "Proxer::init_tabs", Proxer_init_tabs, fp_init_tabs)

// Proxer::update_object (0x422f90): a car's position and velocity into its record, as bits
static void __fastcall Proxer_update_object(Proxer* self, Edx, int i, const Point2D* pos, const Point2D* vel) {
    uint32_t x = Ub(&pos->x), z = Ub(&pos->z);
    uint8_t* r = self->records + self->record_size * i;
    put4(r, x);
    put4(r + 4, z);
    x = Ub(&vel->x);
    z = Ub(&vel->z);
    r = self->records + self->record_size * i;
    put4(r + 8, x);
    put4(r + 12, z);
}
static void fp_update_object(Footprint& f, Proxer* self, Edx, int i, const Point2D*, const Point2D*) {
    f.add(self->records + self->record_size * i, 16, "Proxer record");
}
PORT_FN(0x00422f90, "Proxer::update_object", Proxer_update_object, fp_update_object)

// Proxer::update (0x422fd0): every pair's relative velocity then position; then each car's two lists sorted, by
// distance and by interest
static void __fastcall Proxer_update(Proxer* self, Edx) {
    int m = self->n * (self->n - 1) / 2;
    for (int off = 0; m > 0; m--, off += 20) {
        ProxerDelta* d = (ProxerDelta*)((uint8_t*)self->deltas + off);
        const float* va = (const float*)(self->records + d->a * self->record_size + 8);
        const float* vb = (const float*)(self->records + d->b * self->record_size + 8);
        d->dv.x = (float)(D(vb[0]) - va[0]);
        d->dv.z = (float)(D(vb[1]) - va[1]);
        d = (ProxerDelta*)((uint8_t*)self->deltas + off);
        const float* pa = (const float*)(self->records + d->a * self->record_size);
        const float* pb = (const float*)(self->records + d->b * self->record_size);
        d->dl.x = (float)(D(pb[0]) - pa[0]);
        d->dl.z = (float)(D(pb[1]) - pa[1]);
    }
    int n = self->n;
    for (int i = 0; i < n; i++) {
        Proxer_sort_edges_o(self, 0, (ProxerDelta**)(self->records + self->record_size * i + self->n * 4 + 0x10),
                            Proxer_compare_edges_by_distance_o, i);
        Proxer_sort_edges_o(self, 0, (ProxerDelta**)(self->records + self->record_size * i + self->n * 8 + 0x10),
                            Proxer_compare_edges_by_interest_o, i);
    }
}
static void fp_proxer_update(Footprint& f, Proxer* self, Edx) { fp_proxer_all(f, self); }
PORT_FN(0x00422fd0, "Proxer::update", Proxer_update, fp_proxer_update)

// Proxer::compare_edges_by_interest (0x4230b0): nulls last; then the near ones (|dl|^2 < 16, on the bits) first;
// then the ones closing faster than 0.1 (the focus car's velocity along the delta, pointing away from it) last;
// then the nearer first. Returns -1, 0 or 1.
static int __fastcall Proxer_compare_edges_by_interest(Proxer* self, Edx, const ProxerDelta* a, const ProxerDelta* b) {
    if (!a || !b) return a ? -1 : 1;
    int focus = self->focus;
    float sign = a->b == focus ? k_prox_minus_one : k_prox_one;
    const float* rec = (const float*)(self->records + self->record_size * focus);
    float closing_a = (float)((D(rec[2]) * a->dl.x + D(rec[3]) * a->dl.z) * sign);
    sign = b->b == focus ? k_prox_minus_one : k_prox_one;
    float closing_b = (float)((D(rec[2]) * b->dl.x + D(rec[3]) * b->dl.z) * sign);
    float da = (float)(D(a->dl.x) * a->dl.x + D(a->dl.z) * a->dl.z);
    float db = (float)(D(b->dl.x) * b->dl.x + D(b->dl.z) * b->dl.z);
    bool near_a = Ib(&da) < 0x41800000, near_b = Ib(&db) < 0x41800000;
    if (near_b != near_a) return near_a ? -1 : 1;
    bool fast_a = Ub(&closing_a) > 0xbdcccccdu, fast_b = Ub(&closing_b) > 0xbdcccccdu;
    if (fast_a != fast_b) return fast_a ? 1 : -1;
    if (!(db < da || db > da)) return 0;
    return !(db > da) ? 1 : -1;
}
static void fp_compare(Footprint&, Proxer*, Edx, const ProxerDelta*, const ProxerDelta*) {}
PORT_FN(0x004230b0, "Proxer::compare_edges_by_interest", Proxer_compare_edges_by_interest, fp_compare)

// Proxer::compare_edges_by_distance (0x423220): nulls last; |a|^2 - |b|^2 in the register, compared with 0.1,
// then its stored float's magnitude
static int __fastcall Proxer_compare_edges_by_distance(Proxer*, Edx, const ProxerDelta* a, const ProxerDelta* b) {
    if (!a || !b) return a ? -1 : 1;
    double v = ((D(a->dl.x) * a->dl.x - D(b->dl.x) * b->dl.x) - D(b->dl.z) * b->dl.z) + D(a->dl.z) * a->dl.z;
    float s = (float)v;
    if (v > k_prox_tenth) return 1;
    double m = s < 0 ? -D(s) : D(s);
    if (!(m >= k_prox_tenth)) return 0;
    return -1;
}
PORT_FN(0x00423220, "Proxer::compare_edges_by_distance", Proxer_compare_edges_by_distance, fp_compare)

// Proxer::static_sort_f (0x4232b0): qsort's comparator, through Proxer::global_sort on Proxer::global
static int __cdecl Proxer_static_sort_f(const void* a, const void* b) {
    const ProxerDelta* db = *(const ProxerDelta* const*)b;
    const ProxerDelta* da = *(const ProxerDelta* const*)a;
    return (*g_global_sort)(*g_proxer, 0, da, db);
}
static void fp_static_sort(Footprint&, const void*, const void*) {}
PORT_FN(0x004232b0, "Proxer::static_sort_f", Proxer_static_sort_f, fp_static_sort)

// Proxer::sort_edges (0x4232d0)
static void __fastcall Proxer_sort_edges(Proxer* self, Edx, ProxerDelta** list, uint32_t cmp, int focus) {
    self->focus = focus;
    *g_global_sort = (ProxSort_t)cmp;
    crt_qsort(list, (unsigned)self->n, 4, Proxer_static_sort_f_o);
    *g_global_sort = 0;
}
static void fp_sort_edges(Footprint& f, Proxer* self, Edx, ProxerDelta** list, uint32_t, int) {
    f.add(&self->focus, 4, "Proxer focus");
    if (self->n > 0) f.add(list, (uint32_t)self->n * 4, "Proxer list");
    f.add(g_global_sort, 4, "Proxer::global_sort");
}
PORT_FN(0x004232d0, "Proxer::sort_edges", Proxer_sort_edges, fp_sort_edges)

// ProxerDelta::naive_contact_time (0x423310): the time until the pair's straight-line paths come within r (100 if
// they're slower than 0.889 m/s relative, or never do); *side (if asked for) which side the other passes. The
// result is the unrounded ST0.
static double __fastcall ProxerDelta_naive_contact_time(const ProxerDelta* self, Edx, float r, float* side) {
    double speed = x87_sqrt(D(self->dv.x) * self->dv.x + D(self->dv.z) * self->dv.z);
    float speed_f = (float)speed;
    if (!(speed > k_prox_slow)) return k_prox_never;
    double dist = x87_sqrt(D(self->dl.z) * self->dl.z + D(self->dl.x) * self->dl.x);
    float dist_f = (float)dist;
    float px = (float)(-((dist * self->dv.x) / speed_f) - self->dl.x);
    float pz = (float)(-((D(self->dv.z) / speed_f) * dist_f) - self->dl.z);
    if (side) {
        float ndx = self->dl.x, dz = self->dl.z;
        put4(side, 0xbf800000u);
        double w = -D(ndx) * pz + D(dz) * px;
        if (!(w > k_zero)) put4(side, 0x3f800000u);
    }
    double miss = D(pz) * pz + D(px) * px;
    double rr = D(r) * r;
    if (!(rr > miss)) return k_prox_never;
    return D(dist_f) / speed_f;
}
static void fp_contact_time(Footprint& f, const ProxerDelta*, Edx, float, float* side) {
    if (side) f.add(side, 4, "side");
    f.pure = true;
}
PORT_FN(0x00423310, "ProxerDelta::naive_contact_time", ProxerDelta_naive_contact_time, fp_contact_time)

// ProxerDelta::GetMyDL / GetMyDV (0x423430 / 0x423470): the delta from car i's side: a bit copy if i is the
// pair's first car, else negated through the FPU
static void __fastcall ProxerDelta_GetMyDL(const ProxerDelta* self, Edx, int i, Point2D* out) {
    if (self->a == i) {
        uint32_t x = Ub(&self->dl.x), z = Ub(&self->dl.z);
        put4(&out->x, x);
        put4(&out->z, z);
        return;
    }
    out->x = -self->dl.x;
    out->z = -self->dl.z;
}
static void fp_get_my(Footprint& f, const ProxerDelta*, Edx, int, Point2D* out) {
    f.add(out, 8, "out");
    f.pure = true;
}
PORT_FN(0x00423430, "ProxerDelta::GetMyDL", ProxerDelta_GetMyDL, fp_get_my)

static void __fastcall ProxerDelta_GetMyDV(const ProxerDelta* self, Edx, int i, Point2D* out) {
    if (self->a == i) {
        uint32_t x = Ub(&self->dv.x), z = Ub(&self->dv.z);
        put4(&out->x, x);
        put4(&out->z, z);
        return;
    }
    out->x = -self->dv.x;
    out->z = -self->dv.z;
}
PORT_FN(0x00423470, "ProxerDelta::GetMyDV", ProxerDelta_GetMyDV, fp_get_my)

// Proxer::Begin / End (0x4234b0 / 0x4234f0)
static void __cdecl Proxer_Begin(int n) {
    Proxer* p = (Proxer*)MemAlloc(0x14);
    if (p) {
        p = Proxer_ctor_o(p, 0, n);
        *g_proxer = p;
        *g_proxer_c = p;
        return;
    }
    *g_proxer = 0;
    *g_proxer_c = 0;
}
static void fp_proxer_begin(Footprint& f, int) { f.replay_only = "allocates the Proxer"; }
PORT_FN(0x004234b0, "Proxer::Begin", Proxer_Begin, fp_proxer_begin)

static void __cdecl Proxer_End() {
    Proxer* p = *g_proxer;
    if (p) {
        Proxer_dtor_o(p, 0);
        op_delete(p);
    }
    *g_proxer = 0;
    *g_proxer_c = 0;
}
static void fp_proxer_end(Footprint& f) { f.replay_only = "frees the Proxer"; }
PORT_FN(0x004234f0, "Proxer::End", Proxer_End, fp_proxer_end)

// ProxerDelta::ProxerDelta (0x423520): zero the vectors (not the pair)
static ProxerDelta* __fastcall ProxerDelta_ctor(ProxerDelta* self, Edx) {
    put4(&self->dl.x, 0);
    put4(&self->dl.z, 0);
    put4(&self->dv.x, 0);
    put4(&self->dv.z, 0);
    return self;
}
static void fp_delta_ctor(Footprint& f, ProxerDelta* self, Edx) {
    f.add(self, sizeof(ProxerDelta), "ProxerDelta");      // (not pure: it returns `this`, which the fuzzer can't compare)
}
PORT_FN(0x00423520, "ProxerDelta::ProxerDelta", ProxerDelta_ctor, fp_delta_ctor)

// =====================================================================================================================
// notes.obj
// =====================================================================================================================

// aicar_record_notes (0x423780): an AI car hands over its notes (generate_notes_for writes them out)
static void __cdecl aicar_record_notes(void* data, int size) {
    *g_notes_data = data;
    *g_notes_size = size;
}
static void fp_record_notes(Footprint& f, void*, int) { f.add(g_notes_data, 8, "the recorded notes"); }
PORT_FN(0x00423780, "aicar_record_notes", aicar_record_notes, fp_record_notes)

// convert_to_string / convert_to_bool (0x4237f0 / 0x423820): flags <-> "0"/"1"
static void __cdecl convert_to_string(char* out, const uint8_t* in, int n) {
    for (int i = 0; i < n; i++) out[i] = in[i] ? '1' : '0';
}
static void fp_convert_string(Footprint& f, char* out, const uint8_t*, int n) {
    if (n > 0) f.add(out, (uint32_t)n, "out");
    f.pure = true;
}
PORT_FN(0x004237f0, "convert_to_string", convert_to_string, fp_convert_string)

static void __cdecl convert_to_bool(uint8_t* out, const char* in, int n) {
    for (int i = 0; i < n; i++) out[i] = in[i] == '1';
}
static void fp_convert_bool(Footprint& f, uint8_t* out, const char*, int n) {
    if (n > 0) f.add(out, (uint32_t)n, "out");
    f.pure = true;
}
PORT_FN(0x00423820, "convert_to_bool", convert_to_bool, fp_convert_bool)

// check_learning (0x4241c0): for every chosen track and strength, every driver: count the notes missing (pass 0),
// then generate them (pass 1, each a whole race); a hard abort stops it
static void __cdecl check_learning(const uint8_t* tracks, const uint8_t* strengths) {
    *g_notes_made = 0;
    *g_notes_missing = 0;
    *g_notes_abort = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int track = 0; track < 16; track++)
            for (int strength = 0; strength < 8; strength++) {
                if (!strengths[strength] || !tracks[track]) continue;
                void* lounge = *g_lounge;
                int n = VFN(lounge, 0, int, int)(lounge, 0, strength);
                for (int driver = 0; driver < n; driver++) {
                    lounge = *g_lounge;
                    const char* car = (const char*)VFN(lounge, 4, const uint8_t*, int, int)(lounge, 0, strength, driver) + 0x70;
                    void* notes = GetDriverNotes_o(driver, strength, track, car, 0);
                    if (!notes) {
                        if (pass) {
                            ++*g_notes_made;
                            generate_notes_for_o(driver, strength, track, car);
                        } else {
                            ++*g_notes_missing;
                        }
                    } else {
                        MemFree(notes);
                    }
                    if (*g_notes_abort) return;
                }
            }
}
static void fp_check_learning(Footprint& f, const uint8_t*, const uint8_t*) { f.replay_only = "runs learning races, writes notes"; }
PORT_FN(0x004241c0, "check_learning", check_learning, fp_check_learning)

// GetDriverNotes (0x4242b0): the notes resource for (driver, strength, track, car), first from notes\ (k 0) then
// from the game's own (k 1, if `all`), checked against the header, the driver's crc and the track's line; a
// MemAlloc'd copy, or 0
static void* __cdecl GetDriverNotes(int driver, int strength, int track, const char* car, uint8_t all) {
    void* result = 0;
    int k = 0;
    char name[0x100];
    uint32_t version;
    while (!result) {
        if (!get_notes_resname_o(k, name, driver, strength, track, car)) break;
        const int32_t* p = (const int32_t*)ResourceTryDiscardable(name, 0x41444e54, &version, 0);
        if (p && version == 7) {
            if (p[0] != 7 || p[1] != 0x40 || p[2] != track || p[4] != strength || p[3] != driver ||
                crt_stricmp((const char*)(p + 5), car)) {
                LogReport(k_fmt_notes_vers);
            } else {
                uint64_t ili = get_ili_info_o(track, driver, strength, car);
                int32_t nodes = (int32_t)(uint32_t)ili;
                uint16_t ili_crc = (uint16_t)(ili >> 32);
                const NotesHeader* h = (const NotesHeader*)p;
                if (calc_driver_crc_o(driver, strength) == h->driver_crc && h->ili_crc == ili_crc && h->nodes == nodes &&
                    h->datasize / h->nodes == 8) {
                    uint8_t* copy = (uint8_t*)MemAlloc(h->datasize + 0x40);
                    result = copy;
                    memcpy(copy, p, 0x40);
                    memcpy(copy + 0x40, p + 0x10, (uint32_t)h->datasize);
                } else {
                    int nodes_ok = h->nodes == nodes;
                    int ili_ok = h->ili_crc == ili_crc;
                    int crc_ok = (uint16_t)calc_driver_crc_o(driver, strength) == h->driver_crc;
                    LogReport(k_fmt_notes_mismatch, crc_ok, ili_ok, nodes_ok);
                }
            }
            ResourceForget(p);
        } else if (p) {
            // the resource found, but not version 7: skipped (and not forgotten), as the original does
        }
        k++;
        if (!all) break;
    }
    return result;
}
static void fp_get_notes(Footprint& f, int, int, int, const char*, uint8_t) { f.replay_only = "loads and allocates notes"; }
PORT_FN(0x004242b0, "GetDriverNotes", GetDriverNotes, fp_get_notes)

// get_notes_resname (0x4244b0): k 0 the player's notes\ (created), k 1 the game's own
static uint8_t __cdecl get_notes_resname(int k, char* name, int driver, int strength, int track, const char* car) {
    if (k == 0) {
        FileCreateDirectory(k_str_notes_dir);
        GetUniqueDSTCFname_o(name, k_str_notes_star2, k_str_dnt, driver, strength, track, car);
        return 1;
    }
    if (k == 1) {
        GetUniqueDSTCFname_o(name, k_str_empty_notes, k_str_dnt2, driver, strength, track, car);
        return 1;
    }
    return 0;
}
static void fp_notes_resname(Footprint& f, int, char*, int, int, int, const char*) { f.replay_only = "creates the notes directory"; }
PORT_FN(0x004244b0, "get_notes_resname", get_notes_resname, fp_notes_resname)

// calc_driver_crc (0x424530): CRC16 of the driver's 0x210-byte record
static uint16_t __cdecl calc_driver_crc(int driver, int strength) {
    void* lounge = *g_lounge;
    const uint8_t* d = VFN(lounge, 4, const uint8_t*, int, int)(lounge, 0, strength, driver);
    return CRC16(d, 0x210);
}
static void fp_driver_crc(Footprint&, int, int) {}
PORT_FN(0x00424530, "calc_driver_crc", calc_driver_crc, fp_driver_crc)

// get_ili_info (0x424560): the ideal line the car would drive here: its node count (+8) and the CRC16 of its 8
// bytes at +0xdc. Returned in edx:eax as the original's struct ili_info {int nodes; u16 crc} (its top half word is
// stack garbage in the original, and 0 here: its callers use the word). No line at all: the original reads
// through the null pointer, and so does this.
static uint64_t __cdecl get_ili_info(int track, int driver, int strength, const char* car) {
    char trk[0x40], name[0x100];
    uint8_t rev;
    s_copy(trk, GetTrackName(track % 8));
    memcpy(trk + strlen(trk), k_str_trk, 5);
    ResourceSetMustLoad(trk);
    int k = 0;
    uint8_t* line = 0;
    for (;;) {
        k = IterateILIname_o(name, &rev, k, driver, track, strength, car);
        if (k == 5) break;
        line = (uint8_t*)ILineTry(name, rev);
        if (line) break;
    }
    ASSERT_MSG_o(line != 0, k_fmt_no_line, trk);
    uint16_t crc = CRC16(line + 0xdc, 8);
    int32_t nodes = Iat(line, 8);
    MemFree(line);
    ResourceSetUnload(trk);
    return (uint32_t)nodes | ((uint64_t)crc << 32);
}
static void fp_ili_info(Footprint& f, int, int, int, const char*) { f.replay_only = "loads the ideal line"; }
PORT_FN(0x00424560, "get_ili_info", get_ili_info, fp_ili_info)

// generate_notes_for (0x4246a0): a whole race in learn mode, one car (the driver, in its own car) on the track, the
// notes it records then written to notes\ with their header. The header's unused bytes (after the car name) are
// uninitialised stack in the original; zero here. The World's first car-list entry (+0x28, 0xc8 bytes) holds the
// driver's name at +4 (13 bytes, as GenerateCarList's OptionsGet fills it) and the car's at +0x11 (35 bytes, up to
// the setup data at +0x34); the header's car name is 32 bytes.
static void __cdecl generate_notes_for(int driver, int strength, int track, const char* car) {
    alignas(4) uint8_t world[0xcd4];
    int args[1] = {driver};                            // AISetDriverMap's `int* const`: the original's argument slot
    *g_notes_data = 0;
    World_ctor(world, 0);
    s_copy((char*)world + 8, GetTrackName(track % 8));
    void* lounge = *g_lounge;
    Iat(world, 0xca8) = 1;
    Iat(world, 0x28) = 1;
    const uint8_t* d = VFN(lounge, 4, const uint8_t*, int, int)(lounge, 0, strength, driver);
    // FIX: the original strcpy's the driver's and the car's names into the World unbounded: a driver's name of 13
    // characters or more ran into the car's field, a car's of 35 or more into the car's setup data and on through
    // the World on the stack. Each is cut to its field.
    const char* dname = *(const char* const*)(d + 0x210);
    if (VP_FIX && strlen(dname) >= 13) s_copy_max((char*)world + 0x2c, dname, 13);
    else s_copy((char*)world + 0x2c, dname);
    if (VP_FIX && strlen(car) >= 35) s_copy_max((char*)world + 0x39, car, 35);
    else s_copy((char*)world + 0x39, car);
    Iat(world, 0xcac) = 2;
    Iat(world, 0xcb0) = 1;
    Iat(world, 0xcb4) = 0;
    Iat(world, 0xcb8) = 0;
    Iat(world, 0xcbc) = 3;
    Iat(world, 0xcc0) = 0x14;
    Iat(world, 0xcc4) = 0;
    Iat(world, 0xcc8) = 0;
    Iat(world, 0xccc) = strength;
    world[0xcd0] = 0;
    world[0xcd1] = (uint8_t)(track / 8);
    AISetDriverMap(args, 1);
    LoadRace(world);
    *g_learn_mode = 1;
    DoRace(world, 0, 0);
    *g_learn_mode = 0;
    UnloadRace(world);
    AIResetDriverMap();
    if (!*g_notes_data) {
        LogPanic(k_str_no_data);
        return;
    }
    NotesHeader h;
    memset(&h, 0, sizeof h);
    h.track = track;
    h.driver = args[0];
    h.version = 7;
    h.size = 0x40;
    h.strength = strength;
    // FIX: the header's car name (32 bytes) takes the car's name unbounded in the original: 32 characters or more ran
    // over the header's other fields and, from 44 on, past the header on the stack. Cut to the field.
    if (VP_FIX && strlen(car) >= sizeof h.car) s_copy_max(h.car, car, sizeof h.car);
    else s_copy(h.car, car);
    uint64_t ili = get_ili_info_o(track, args[0], strength, car);
    uint16_t crc = calc_driver_crc_o(args[0], strength);
    h.driver_crc = crc;
    h.nodes = (int32_t)(uint32_t)ili;
    h.ili_crc = (uint16_t)(ili >> 32);
    h.datasize = *g_notes_size;
    char fname[0x100];
    get_notes_fname_o(fname, args[0], strength, track, car);
    int fd = FileCreate(fname);
    if (fd) {
        ResourceWrite(fd, 0x41444e54, 7);
        FileWrite(fd, &h, 0x40);
        FileWrite(fd, *g_notes_data, *g_notes_size);
        FileClose(&fd);
    } else {
        LogPanic(k_fmt_cant_create, fname);
    }
    MemFree(*g_notes_data);
}
static void fp_generate_notes(Footprint& f, int, int, int, const char*) { f.replay_only = "runs a race, writes a notes file"; }
PORT_FN(0x004246a0, "generate_notes_for", generate_notes_for, fp_generate_notes)

// get_notes_fname (0x4249e0): notes\<dstc>.dnt (the directory created)
static void __cdecl get_notes_fname(char* name, int driver, int strength, int track, const char* car) {
    FileCreateDirectory(k_str_notes_dir2);
    GetUniqueDSTCFname_o(name, k_str_notes_slash, k_str_dnt3, driver, strength, track, car);
}
static void fp_notes_fname(Footprint& f, char*, int, int, int, const char*) { f.replay_only = "creates the notes directory"; }
PORT_FN(0x004249e0, "get_notes_fname", get_notes_fname, fp_notes_fname)

// InverseDSTC (0x424a20): a ghost's file name back into driver, strength, track (one hex digit each) and the
// car's four characters
static uint8_t __cdecl InverseDSTC(int* driver, int* strength, int* track, char* car4, const char* name) {
    uint8_t ok = 0;
    if (hex2int_o(driver, (uint8_t)name[0]) && hex2int_o(strength, (uint8_t)name[1])) {
        ok = 1;
        if (!hex2int_o(track, (uint8_t)name[2])) ok = 0;
    }
    if (ok) crt_strncpy(car4, name + 3, 4);
    return ok;
}
static void fp_inverse_dstc(Footprint& f, int* a, int* b, int* c, char* car4, const char*) {
    f.add(a, 4, "driver");
    f.add(b, 4, "strength");
    f.add(c, 4, "track");
    f.add(car4, 4, "car");
    f.pure = true;
}
PORT_FN(0x00424a20, "InverseDSTC", InverseDSTC, fp_inverse_dstc)

// hex2int (0x424a90): one hex digit (the char's signed value)
static uint8_t __cdecl hex2int(int* out, char c) {
    if (c >= 0x30 && c <= 0x39) { *out = (int)c - 0x30; return 1; }
    if (c >= 0x61 && c <= 0x66) { *out = (int)c - 0x57; return 1; }
    if (c >= 0x41 && c <= 0x46) { *out = (int)c - 0x37; return 1; }
    return 0;
}
static void fp_hex2int(Footprint& f, int* out, char) {
    f.add(out, 4, "out");
    f.pure = true;
}
PORT_FN(0x00424a90, "hex2int", hex2int, fp_hex2int)

// GetUniqueDSTCFname / GetUniqueDSTCMunge (0x424ae0 / 0x424b30): dir + "%x%x%x" (driver, strength, track) + the
// car's four characters + ext
static void __cdecl GetUniqueDSTCFname(char* out, const char* dir, const char* ext, int driver, int strength, int track,
                                       const char* car) {
    char munge[64];                                    // (the original's is 16 bytes: at most 3 x 8 + 4 + 1 used now)
    GetUniqueDSTCMunge_o(munge, driver, strength, track, car);
    crt_sprintf(out, k_fmt_sss, dir, munge, ext);
}
static void fp_dstc_fname(Footprint& f, char* out, const char*, const char*, int, int, int, const char*) {
    f.add(out, 0x100, "name");
}
PORT_FN(0x00424ae0, "GetUniqueDSTCFname", GetUniqueDSTCFname, fp_dstc_fname)

static void __cdecl GetUniqueDSTCMunge(char* out, int driver, int strength, int track, const char* car) {
    char car4[32];                                     // (uninitialised, as the original's)
    munge_carname_to_4char(car4, car);
    // FIX: munge_carname_to_4char strncpy's four characters of any car but viper / vipergt, unterminated (a mod car's
    // name of 4 or more characters): the name then ran on into the uninitialised stack, making the ghost and notes
    // file names from garbage and overrunning GetUniqueDSTCFname's 16-byte buffer. Terminated after the four.
    // (Every other car's code is already terminated by then: the same name.)
    if (VP_FIX) car4[4] = 0;
    crt_sprintf(out, k_fmt_dstc, driver, strength, track, car4);
}
static void fp_dstc_munge(Footprint& f, char* out, int, int, int, const char*) { f.add(out, 0x40, "name"); }
PORT_FN(0x00424b30, "GetUniqueDSTCMunge", GetUniqueDSTCMunge, fp_dstc_munge)

// GetDriverData / ReleaseDriverData (0x424b70 / 0x424be0): a car's notes for this race (the data after the header)
static const void* __cdecl GetDriverData(int car) {
    const char* name = (const char*)CarMgrGetInfo(car) + 0x12;
    int strength = AIGetStrength_o();
    int driver = AIGetDriverForCar(car);
    int track = GetTrackNumber(WorldGetTrackname());
    if (WorldGameOptions()[0x25]) track += 8;
    uint8_t* notes = (uint8_t*)GetDriverNotes_o(driver, strength, track, name, 1);
    return notes ? notes + 0x40 : 0;
}
static void fp_driver_data(Footprint& f, int) { f.replay_only = "loads and allocates notes"; }
PORT_FN(0x00424b70, "GetDriverData", GetDriverData, fp_driver_data)

static void __cdecl ReleaseDriverData(const void* data) { MemFree((uint8_t*)data - 0x40); }
static void fp_release_data(Footprint& f, const void*) { f.replay_only = "frees notes"; }
PORT_FN(0x00424be0, "ReleaseDriverData", ReleaseDriverData, fp_release_data)

// =====================================================================================================================
// gcviewer.obj (the non-drawing part)
// =====================================================================================================================

static __forceinline Submits* gcv_subs(GhostCarViewer* self) { return &self->subs[*self->p_track]; }
static void fp_gcv_ui(Footprint& f, GhostCarViewer*, Edx) { f.replay_only = "drives the UI"; }

// GhostCarViewer::Create / Destroy (0x4250a0 / 0x425100)
static void __fastcall GCV_Create(GhostCarViewer* self, Edx) {
    UICustomControl_AddNotification(self, 0, 0, self->p_driver, 4);
    UICustomControl_AddNotification(self, 0, 0, self->p_strength, 4);
    UICustomControl_AddNotification(self, 0, 0, self->p_track, 4);
    UIHideGroup(self->g_main);
    UIShowGroup(self->g_load);
    GCV_update_widgets_o(self, 0);
}
PORT_FN(0x004250a0, "GhostCarViewer::Create", GCV_Create, fp_gcv_ui)

static void __fastcall GCV_Destroy(GhostCarViewer* self, Edx) {
    UICustomControl_RemoveNotification(self, 0, self->p_track);
    UICustomControl_RemoveNotification(self, 0, self->p_strength);
    UICustomControl_RemoveNotification(self, 0, self->p_driver);
}
PORT_FN(0x00425100, "GhostCarViewer::Destroy", GCV_Destroy, fp_gcv_ui)

// next_submit / prev_submit (0x425130 / 0x425170): step through the track's library, wrapping
static void __fastcall GCV_next_submit(GhostCarViewer* self, Edx) {
    gcv_subs(self)->current++;
    Submits* s = gcv_subs(self);
    if (!(s->count > s->current)) s->current = 0;
    GCV_update_submit_o(self, 0);
}
PORT_FN(0x00425130, "GhostCarViewer::next_submit", GCV_next_submit, fp_gcv_ui)

static void __fastcall GCV_prev_submit(GhostCarViewer* self, Edx) {
    gcv_subs(self)->current--;
    Submits* s = gcv_subs(self);
    if (s->current < 0) s->current = s->count - 1;
    GCV_update_submit_o(self, 0);
}
PORT_FN(0x00425170, "GhostCarViewer::prev_submit", GCV_prev_submit, fp_gcv_ui)

// GhostCarViewer::Added (0x4251b0): the viewer's widgets, relative to its position (x, y); item 17 is the one the
// original builds in place rather than through the constructor
static void __fastcall GCV_Added(GhostCarViewer* self, Edx) {
    UIDialogItem items[20];
    uint32_t x = (uint32_t)self->x, y5 = (uint32_t)self->y + 5;
    uint32_t y14 = y5 + 0xf, y20 = y14 + 0xc, y48 = y20 + 0x28, y57 = y5 + 0x52, y63 = y57 + 0xc, y77 = y63 + 0x14;
    uint32_t y8d = y77 + 0x16;
    uint32_t main = (uint32_t)(uintptr_t)&self->g_main, lib = (uint32_t)(uintptr_t)&self->g_library;
    uint32_t scroll = (uint32_t)(uintptr_t)&self->g_scroll, load = (uint32_t)(uintptr_t)&self->g_load;
    uint32_t t = (uint32_t)(uintptr_t)self;
    UIDialogItem_ctor(&items[0], 0, 1, 0, 0, 0, 0, 0, 0, 0, main, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[1], 0, 5, 0, x, y5, 0, 0, 0x4ec898, 0, 0, 0xb, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[2], 0, 5, 0, x + 5, y14, 0, 0, t + 0x24, 0, 0, 0xc, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[3], 0, 5, 0, x + 5, y20, 0, 0, t + 0x44, 0, 0, 0xc, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[4], 0, 5, 0, x, y48, 0, 0, t + 0xa4, 0, 0, 0xb, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[5], 0, 1, 0, 0, 0, 0, 0, 0, 0, lib, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[6], 0, 1, 0, 0, 0, 0, 0, 0, 0, scroll, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[7], 0, 3, 0, x + 0x12, y57, 0, 0, 0x4ec8a4, 0, 0x426090, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[8], 0, 3, 0, x, y57, 0, 0, 0x4ec8b0, 0, 0x4260a0, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[9], 0, 1, 1, 0, 0, 0, 0, 0, 0, scroll, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[10], 0, 2, 0, x, y77, 0, 0, 0x4ec8bc, 0, 0x426080, 4, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[11], 0, 2, 0, x, y8d, 0, 0, 0x4ec8c4, 0, 0x4260c0, 4, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[12], 0, 5, 0, x + 0x28, y57, 0, 0, t + 0x64, 0, 0, 0xc, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[13], 0, 5, 0, x + 0x28, y63, 0, 0, t + 0x84, 0, 0, 0xc, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[14], 0, 1, 1, 0, 0, 0, 0, 0, 0, lib, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[15], 0, 1, 1, 0, 0, 0, 0, 0, 0, main, 0, 0, 0, 0, 0);
    UIDialogItem_ctor(&items[16], 0, 1, 0, 0, 0, 0, 0, 0, 0, load, 0, 0, 0, 0, 0);
    static const uint32_t item17[14] = {2, 0, 0, 0, 0, 0, 0x4ec8cc, 0, 0x4260b0, 4, 0, 0, 0, 0};
    memcpy(items[17].w, item17, sizeof item17);
    items[17].w[2] = x;
    items[17].w[3] = y77;
    UIDialogItem_ctor(&items[18], 0, 1, 1, 0, 0, 0, 0, 0, 0, load, 0, 0, 0, 0, 0);
    memcpy(&items[19], k_item_end, sizeof(UIDialogItem));
    UICustomControl_AddItems(self, 0, items);
}
PORT_FN(0x004251b0, "GhostCarViewer::Added", GCV_Added, fp_gcv_ui)

// set_driver_ghost (0x4255e0): the library's current ghost becomes the driver's: copied from the ghost library
// to notes\<dstc>.gcf, turned into notes\<dstc>.ilq by gcf2ili, and kept as the driver's ghost
static void __fastcall GCV_set_driver_ghost(GhostCarViewer* self, Edx) {
    Submits* s = gcv_subs(self);
    if (!s->count) return;
    SubEntry* e = self->subs[*self->p_track].items[s->current];
    char src[0x100], gcf[0x100], ilq[0x100];
    s_copy(src, *k_ghost_dir);
    s_cat(src, e->file);
    void* lounge = *g_lounge;
    const char* car = (const char*)VFN(lounge, 4, const uint8_t*, int, int)(lounge, 0, *self->p_strength, *self->p_driver) + 0x70;
    GetUniqueDSTCFname_o(gcf, k_str_notes_slash2, k_str_gcf, *self->p_driver, *self->p_strength, *self->p_track, car);
    if (!FileCopy(gcf, src)) {
        UIDoDratBox(k_str_grak3, k_str_nocopy, 0);
        return;
    }
    GetUniqueDSTCFname_o(ilq, k_str_notes_slash3, *k_ext_ilq2, *self->p_driver, *self->p_strength, *self->p_track, car);
    int exitcode = -1, other = 0;
    if (Win32System(&other, &exitcode, k_fmt_gcf2ili, gcf, ilq) && exitcode == 0) {
        exitcode = -1;
        GCV_add_driver_ghost_o(self, 0, &e->info, *self->p_driver, *self->p_strength, *self->p_track);
        GCV_update_driver_o(self, 0);
        return;
    }
    if (exitcode == -1) UIDoDratBox(k_str_grak1, k_str_nopath, 0);
    else UIDoDratBox(k_str_grak2, k_str_choked, 0);
}
static void fp_gcv_files(Footprint& f, GhostCarViewer*, Edx) { f.replay_only = "copies files, runs gcf2ili, drives the UI"; }
PORT_FN(0x004255e0, "GhostCarViewer::set_driver_ghost", GCV_set_driver_ghost, fp_gcv_files)

// update_driver (0x4257e0): the driver's own ghost's text, or "<NONE>"
static void __fastcall GCV_update_driver(GhostCarViewer* self, Edx) {
    GhostInfo* g = self->ghosts[(((*self->p_strength << 4) + *self->p_driver) << 4) + *self->p_track];
    if (g) {
        GCV_update_text_o(self, 0, g, &self->driver_text);
        return;
    }
    s_copy(self->driver_text.name, k_str_none);
    self->driver_text.time[0] = 0;
}
static void fp_gcv_text(Footprint& f, GhostCarViewer* self, Edx) {
    f.add(&self->driver_text, sizeof(UIData), "driver text");
    f.add((void*)0x005215d8, 0x30, "PhysicsTimeString's buffer");
}
PORT_FN(0x004257e0, "GhostCarViewer::update_driver", GCV_update_driver, fp_gcv_text)

// update_text (0x425840): a ghost's name and lap time
static void __fastcall GCV_update_text(GhostCarViewer*, Edx, const GhostInfo* g, UIData* d) {
    s_copy(d->name, (const char*)g->b + 4);
    s_copy(d->time, PhysicsTimeString(Ub(g->b + 0xc8), 1));
}
static void fp_gcv_update_text(Footprint& f, GhostCarViewer*, Edx, const GhostInfo*, UIData* d) {
    f.add(d, sizeof(UIData), "text");
    f.add((void*)0x005215d8, 0x30, "PhysicsTimeString's buffer");
}
PORT_FN(0x00425840, "GhostCarViewer::update_text", GCV_update_text, fp_gcv_update_text)

// update_submit (0x4258b0): the library's widgets for this track
static void __fastcall GCV_update_submit(GhostCarViewer* self, Edx) {
    uint32_t group = self->g_library;
    if (gcv_subs(self)->count) {
        UIShowGroup(group);
        group = self->g_scroll;
        if (gcv_subs(self)->count > 1) UIShowGroup(group);
        else UIHideGroup(group);
        Submits* s = gcv_subs(self);
        crt_sprintf(self->library_text, k_fmt_library, s->current + 1, s->count);
        s = gcv_subs(self);
        GCV_update_text_o(self, 0, &s->items[s->current]->info, &self->submit_text);
        return;
    }
    UIHideGroup(group);
    crt_sprintf(self->library_text, k_str_library_empty);
    memset(&self->submit_text, 0, sizeof(UIData));
}
PORT_FN(0x004258b0, "GhostCarViewer::update_submit", GCV_update_submit, fp_gcv_ui)

// update_widgets (0x4259a0) and Callback (0x4259c0)
static void __fastcall GCV_update_widgets(GhostCarViewer* self, Edx) {
    GCV_update_submit_o(self, 0);
    GCV_update_driver_o(self, 0);
}
PORT_FN(0x004259a0, "GhostCarViewer::update_widgets", GCV_update_widgets, fp_gcv_ui)

static void __fastcall GCV_Callback(GhostCarViewer* self, Edx, int, const void*) { GCV_update_widgets_o(self, 0); }
static void fp_gcv_callback(Footprint& f, GhostCarViewer*, Edx, int, const void*) { f.replay_only = "drives the UI"; }
PORT_FN(0x004259c0, "GhostCarViewer::Callback", GCV_Callback, fp_gcv_callback)

// add_driver_ghost (0x4259d0): a copy of the ghost's info as the driver's ghost (allocated once)
static void __fastcall GCV_add_driver_ghost(GhostCarViewer* self, Edx, const GhostInfo* g, int driver, int strength,
                                            int track) {
    GhostInfo** slot = &self->ghosts[(((strength << 4) + driver) << 4) + track];
    if (!*slot) *slot = (GhostInfo*)MemAlloc(0xcc);
    memcpy(*slot, g, 0xcc);
}
static void fp_gcv_add_driver(Footprint& f, GhostCarViewer*, Edx, const GhostInfo*, int, int, int) { f.replay_only = "allocates"; }
PORT_FN(0x004259d0, "GhostCarViewer::add_driver_ghost", GCV_add_driver_ghost, fp_gcv_add_driver)

// delete_ghost (0x425a20): the library's current ghost: its file removed, the entry freed and closed up
static void __fastcall GCV_delete_ghost(GhostCarViewer* self, Edx) {
    Submits* s = gcv_subs(self);
    int n = s->count;
    if (!n || s->current >= n) return;
    SubEntry* e = self->subs[*self->p_track].items[s->current];
    char path[0x100];
    s_copy(path, *k_ghost_dir);
    s_cat(path, e->file);
    FileRemove(path);
    op_delete(e);
    gcv_subs(self)->count--;
    s = gcv_subs(self);
    int cur = s->current;
    SubEntry** at = &self->subs[*self->p_track].items[cur];
    crt_memmove(at, at + 1, (unsigned)((s->count - cur) * 4));
    GCV_update_submit_o(self, 0);
}
PORT_FN(0x00425a20, "GhostCarViewer::delete_ghost", GCV_delete_ghost, fp_gcv_files)

// add_submitted_ghost (0x425b40): a ghost file into its track's library, sorted by lap time; a 16th pushes out
// (and deletes the file of) the slowest
static void __fastcall GCV_add_submitted_ghost(GhostCarViewer* self, Edx, const uint8_t* gf, const char* file) {
    int k = GetTrackNumber((const char*)gf + 0x10) + (int)((uint32_t)gf[0x1d] * 8);
    Submits* s = &self->subs[k];
    int n = s->count, i = 0;
    for (; i < n; i++)
        if (*(const float*)((const uint8_t*)s->items[i] + 0xe8) > *(const float*)(gf + 0xe8)) break;
    crt_memmove(&s->items[i + 1], &s->items[i], (unsigned)((n - i) * 4));
    SubEntry* e = (SubEntry*)MemAlloc(0xec);
    if (e) {
        memcpy(&e->info, gf + 0x20, 0xcc);
        s_copy(e->file, file);
        s->items[i] = e;
    } else {
        s->items[i] = 0;
    }
    if (++s->count == 16) {
        char path[0x100];
        s_copy(path, *k_ghost_dir);
        s_cat(path, s->items[15]->file);
        FileSetWritable(path, 1);
        FileRemove(path);
        op_delete(s->items[15]);
        int c = s->count - 1;
        s->count = c;
        crt_memmove(&s->items[15], &s->current, (unsigned)(c * 4 - 0x3c));
    }
}
static void fp_gcv_add_sub(Footprint& f, GhostCarViewer*, Edx, const uint8_t*, const char*) { f.replay_only = "allocates, deletes files"; }
PORT_FN(0x00425b40, "GhostCarViewer::add_submitted_ghost", GCV_add_submitted_ghost, fp_gcv_add_sub)

// load (0x425d00): once: the ghost library (*.tmp in the library directory) and the drivers' ghosts (notes\*.gcf)
static void __fastcall GCV_load(GhostCarViewer* self, Edx) {
    if (self->loaded) return;
    char pattern[0x100], found[0x18 + 8], path[0x100];
    int driver, strength, track;
    char car4[0x20];
    s_copy(pattern, *k_ghost_dir);
    memcpy(pattern + strlen(pattern), k_str_tmp, 6);
    void* h = FileFindFirst(pattern, found, 0x18);
    if (h != (void*)-1) {
        do {
            s_copy(path, *k_ghost_dir);
            s_cat(path, found);
            uint8_t* gf = (uint8_t*)GhostLoad(path, 1);
            if (gf) {
                GCV_add_submitted_ghost_o(self, 0, gf, found);
                MemFree(gf);
            }
        } while (FileFindNext(h, found, 0x18));
        FileFindClose(h);
    }
    h = FileFindFirst(k_str_gcf_glob, found, 0x18);
    if (h != (void*)-1) {
        do {
            if (InverseDSTC_o(&driver, &strength, &track, car4, found)) {
                crt_sprintf(path, k_fmt_notes_s, found);
                uint8_t* gf = (uint8_t*)GhostLoad(path, 1);
                if (gf) {
                    void* lounge = *g_lounge;
                    VFN(lounge, 4, const uint8_t*, int, int)(lounge, 0, strength, driver);
                    GCV_add_driver_ghost_o(self, 0, (const GhostInfo*)(gf + 0x20), driver, strength, track);
                    MemFree(gf);
                }
            }
        } while (FileFindNext(h, found, 0x18));
        FileFindClose(h);
    }
    self->loaded = 1;
    UIHideGroup(self->g_load);
    UIShowGroup(self->g_main);
}
static void fp_gcv_load(Footprint& f, GhostCarViewer*, Edx) { f.replay_only = "loads ghost files, allocates, drives the UI"; }
PORT_FN(0x00425d00, "GhostCarViewer::load", GCV_load, fp_gcv_load)

// GhostCarViewer::GhostCarViewer (0x425f30) and ~GhostCarViewer (0x425fa0)
static GhostCarViewer* __fastcall GCV_ctor(GhostCarViewer* self, Edx, int32_t* driver, int32_t* strength, int32_t* track) {
    self->vtable = (void**)0x004db190;
    self->vtable = (void**)0x004db738;
    self->_14 = 0;
    *g_gcv = self;
    self->p_driver = driver;
    self->loaded = 0;
    self->p_strength = strength;
    self->p_track = track;
    memset(self->subs, 0, sizeof self->subs);
    memset(&self->driver_text, 0, 0xa0);
    memset(self->ghosts, 0, sizeof self->ghosts);
    return self;
}
static void fp_gcv_ctor(Footprint& f, GhostCarViewer* self, Edx, int32_t*, int32_t*, int32_t*) {
    f.add(self, sizeof(GhostCarViewer), "GhostCarViewer");
    f.add(g_gcv, 4, "GhostCarViewer::global");
}
PORT_FN(0x00425f30, "GhostCarViewer::GhostCarViewer", GCV_ctor, fp_gcv_ctor)

static GhostCarViewer* __fastcall GCV_dtor(GhostCarViewer* self, Edx) {
    self->vtable = (void**)0x004db738;
    for (int c = 0; c < 16; c++)
        for (int m = 0; m < 128; m++) {
            GhostInfo** slot = &self->ghosts[m * 16 + c];
            if (*slot) {
                op_delete(*slot);
                *slot = 0;
            }
        }
    for (int k = 0; k < 16; k++) {
        Submits* s = &self->subs[k];
        for (int j = 0; j < s->count; j++) {
            op_delete(s->items[j]);
            s->items[j] = 0;
        }
    }
    *g_gcv = 0;
    self->vtable = (void**)0x004db190;
    return self;
}
static void fp_gcv_dtor(Footprint& f, GhostCarViewer*, Edx) { f.replay_only = "frees"; }
PORT_FN(0x00425fa0, "GhostCarViewer::~GhostCarViewer", GCV_dtor, fp_gcv_dtor)

// CreateIGhostCarViewer (0x426050)
static GhostCarViewer* __cdecl CreateIGhostCarViewer(int32_t* driver, int32_t* strength, int32_t* track) {
    GhostCarViewer* p = (GhostCarViewer*)MemAlloc(0x2558);
    if (!p) return 0;
    return GCV_ctor_o(p, 0, driver, strength, track);
}
static void fp_create_gcv(Footprint& f, int32_t*, int32_t*, int32_t*) { f.replay_only = "allocates the viewer"; }
PORT_FN(0x00426050, "CreateIGhostCarViewer", CreateIGhostCarViewer, fp_create_gcv)

// the buttons' callbacks (0x426080...): the global viewer's action; 0 (the dialog stays)
static uint8_t __cdecl GCV_SetDriverGhost(int) { GCV_set_driver_ghost_o(*g_gcv, 0); return 0; }
static void fp_gcv_button(Footprint& f, int) { f.replay_only = "drives the UI and files"; }
PORT_FN(0x00426080, "GhostCarViewer::SetDriverGhost", GCV_SetDriverGhost, fp_gcv_button)
static uint8_t __cdecl GCV_NextSubmit(int) { GCV_next_submit_o(*g_gcv, 0); return 0; }
PORT_FN(0x00426090, "GhostCarViewer::NextSubmit", GCV_NextSubmit, fp_gcv_button)
static uint8_t __cdecl GCV_PrevSubmit(int) { GCV_prev_submit_o(*g_gcv, 0); return 0; }
PORT_FN(0x004260a0, "GhostCarViewer::PrevSubmit", GCV_PrevSubmit, fp_gcv_button)
static uint8_t __cdecl GCV_LoadSet(int) { GCV_load_o(*g_gcv, 0); return 0; }
PORT_FN(0x004260b0, "GhostCarViewer::LoadSet", GCV_LoadSet, fp_gcv_button)
static uint8_t __cdecl GCV_Delete(int) { GCV_delete_ghost_o(*g_gcv, 0); return 0; }
PORT_FN(0x004260c0, "GhostCarViewer::Delete", GCV_Delete, fp_gcv_button)
