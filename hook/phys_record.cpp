// phys_record.cpp -- M3 3.6 group T3: the race records (library "physics", record.obj), rewritten.
//
// Two halves:
//
//   this race's records   three RecordLists (statics 0x521540 race, 0x521550 lap, 0x521568 stage), each a
//                         heap array of 0x48-byte RaceRecords with a count and a capacity. RecordBegin
//                         (WorldBeginCommon) allocates them -- cars, cars x laps, cars x laps x 3 -- and the
//                         RecordMgr; the race logic (RaceDeity::UpdateCar / PostRace / NetPacket, and the drag
//                         deities) fills them through RecordNewLap / RecordStage / RecordRaceOver and their
//                         "unofficial" twins (a remote car's: flag 0x20, may be overwritten by an official one);
//                         RecordReset (PhysTaskRestart) empties them; RecordConsolidate (DoRace, after the race)
//                         adds a DNF record for every car that didn't finish, merges the laps and the finishers
//                         into the records file (CheckLaps / CheckRace), marks the stage bests and saves.
//                         RecordEnd (WorldEndCommon) frees everything.
//   the records file      RecordMgr: "<track>.sco" in the user directory, a RecordFile of 0x114c8 bytes (below),
//                         loaded (or recreated) by the constructor and written by save() when dirty. The high
//                         score board (BoardDo, HighScoreBoardTable) reads it through SetMode / GetNthBest* and
//                         empties one mode's table with Clear.
//
// Threads (docs/PORTING.md): the recorders (RecordNewLap / RecordUnofficialNewLap / record_new_lap,
// RecordStage / RecordUnofficialStage / record_stage, RecordRaceOver / RecordUnofficialRaceOver /
// record_race_over, record, init_baserecord, the best_* and sum_times queries, set_racetime_for_car) and
// RecordReset run on the PHYSICS thread (the deity's UpdateCar / PostRace / NetPacket, PhysTaskRestart);
// record.obj's statics (0x521538, 160 bytes) are physics statics, saved by the shadow check there. Everything
// else runs on the MAIN thread: RecordBegin / RecordEnd (WorldBegin/EndCommon), RecordConsolidate and what only
// it calls (create_dnfs, set_best_stages, CheckLaps, CheckRace, save), the result queries (dashboards, post-race
// tables: read only), and the RecordMgr's board functions (constructor, Clear, SetMode, GetNthBest*). The
// helpers called from both (init_baserecord, best_*, set_racetime_for_car) list the statics they write anyway.
// So every footprint below names the record statics it writes, whichever thread runs it. Allocation, freeing,
// file I/O and LogPanic (a record list out of room) are replay_only.
//
// Skipped: the $E initialisers ($E1..$E54 at 0x429d40..0x429f70: the CRT's static construction, run before
// anything could be hooked). There are no deleting destructors or bare-`ret` stubs in record.obj.
//
// Written from the v1.0 disassembly: every call in the original's order, by address (this file's own functions
// too, so the hooked rewrite or the original is what runs), floats moved with integer instructions kept as bit
// copies (uint32_t), rep movsd copies as rep movsd, flag comparisons with the original's NaN results.
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
static __forceinline uint32_t Ub(const float& x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
// rep movsd: n dwords, forward, as the original moves them (a bit copy)
static __forceinline void movsd(void* dst, const void* src, uint32_t n) {
    __asm { mov edi, dst
            mov esi, src
            mov ecx, n
            rep movsd }
}
// rep stosd of zero
static __forceinline void stosd0(void* dst, uint32_t n) {
    __asm { mov edi, dst
            xor eax, eax
            mov ecx, n
            rep stosd }
}
// the inlined strcpy / strcat (repne scasb; rep movsd; rep movsb): strlen + 1 bytes, forward
static __forceinline void str_copy(char* dst, const char* src) { memcpy(dst, src, strlen(src) + 1); }
static __forceinline void str_cat(char* dst, const char* src) { str_copy(dst + strlen(dst), src); }

// ---- layouts (from the code below; out/types.tsv has only the sizes' fragments) ---------------------------------
// RaceRecord (0x48; the constructor clears only the flags). One lap, stage or race of one car. init_baserecord
// fills it from CarMgrGetInfo(car) and the date RecordBegin took; record() sets lap / stage / time.
//   lap, stage   record()'s 2nd and 3rd arguments: laps  (lap, 0xfa);  races  (0xfa, 0xfa);  stages  (lap, stage)
//                -- RecordStage(car, stage, lap, time) swaps them into record(); set_best_stages indexes its table
//                by car*3 + stage - 1, so a stage is 1..3; RecordGetStageResult(car, lap, stage)
//   flags   1 valid, 0x10 CarMgrInfo+4 (set with 1: the ones the file takes), 0x20 unofficial (a remote car's;
//           record() overwrites it), 2 lap / stage best, 4 top-speed best, 8 in the race top ten (CheckLaps,
//           CheckRace, set_best_stages, RecordConsolidate); RecordFile::clear_record_bits clears 2|4|8 in the file
struct RaceRecord {
    uint8_t date[4];                   // +0x00  year (u16), day, month -- 0x521580, from TimeGetTimeOfDay
    float lap_time;                    // +0x04  lap / stage time; a race's best lap (-1: none)
    float top_speed;                   // +0x08  a lap's top speed; a race's best (-1: none)
    float race_time;                   // +0x0c  sum of the laps; -1 = DNF
    char car_name[13];                 // +0x10  CarMgrInfo+5
    char driver_name[0x21];            // +0x1d  CarMgrInfo+0x12, first letter upper-cased
    int16_t index;                     // +0x3e  CheckLaps / CheckRace: its index in the array being merged
    uint8_t flags;                     // +0x40
    uint8_t car;                       // +0x41
    uint8_t stage;                     // +0x42  0xfa: a lap's or a race's
    uint8_t lap;                       // +0x43  0xfa: a race's
    uint8_t race_type;                 // +0x44  the RecordMgr's (+0x18), low byte
    uint8_t _45[3];
};
static_assert(sizeof(RaceRecord) == 0x48 && offsetof(RaceRecord, index) == 0x3e && offsetof(RaceRecord, race_type) == 0x44, "RaceRecord");
// RecordList (16): the statics at 0x521540 / 0x521550 / 0x521568
struct RecordList { const char* name; RaceRecord* recs; int32_t count; int32_t cap; };
static_assert(sizeof(RecordList) == 16, "RecordList");
// the records file, "<track>.sco" (0x114c8 bytes, written and read whole):
//   +0   u32 size 0x114c8, u32 version 4 (anything else: recreated empty)
//   +8   bartag[6], 0x2e20 each: realism 0..2, and 3..5 = realism + 3 (SetMode's flag)
//          baztag[2], 0x1710 each: race type < 4, >= 4
//            +0x00  RaceRecord   best top speed           (GetNthBestSpeed(0))
//            +0x48  RaceRecord   best lap                 (GetNthBestLap(0))
//            +0x90  footag[8], 0x2d0 each, by race type (0..7; each baztag has all 8, half used):
//                     RaceRecord[10], the race top ten, worst first: slot 9 is the best (GetNthBestRace(n) = 9 - n)
//   A slot is used when its flags have bit 0. baztag index = realism*2 + (race type >= 4): 82 records each.
struct Footag { RaceRecord r[10]; };
struct Baztag { RaceRecord speed; RaceRecord lap; Footag races[8]; };
struct Bartag { Baztag baz[2]; };
struct RecordFile { uint32_t size, version; Bartag bars[6]; };
static_assert(sizeof(Footag) == 0x2d0 && sizeof(Baztag) == 0x1710 && sizeof(Bartag) == 0x2e20 && sizeof(RecordFile) == 0x114c8, "RecordFile");
// RecordMgr (0x1c)
struct RecordMgr {
    uint8_t dirty;                     // +0x00  save() writes the file only when set
    char filename[15];                 // +0x01  "<track>.sco" (the track's 3-letter extension stripped); unbounded strcpy
    RecordFile* file;                  // +0x10
    int32_t realism;                   // +0x14  SetMode: realism, + 3 with the flag
    int32_t race_type;                 // +0x18
};
static_assert(offsetof(RecordMgr, file) == 0x10 && sizeof(RecordMgr) == 0x1c, "RecordMgr");
// CarMgrInfo (0x170 each at 0x554090): what init_baserecord reads
//   +4 u8 (sets record flag 0x10), +5 char[13] car name, +0x12 char[] driver name

// ---- the statics ----------------------------------------------------------------------------------------------
enum : uint32_t {
    S_STATICS = 0x00521538, STATICS_N = 160,   // record.obj's .bss (the physics statics table's entry)
    S_RACES = 0x00521540,              // RecordList "race"
    S_LAPS = 0x00521550,               // RecordList "lap"
    S_STAGES = 0x00521568,             // RecordList "stage"
    S_DATE = 0x00521580,               // u16 year, u8 day, u8 month: RecordBegin's TimeGetTimeOfDay
    S_MGR = 0x0052159c,                // RecordMgr* (RecordBegin's)
    STR_LAP = 0x004ece30, STR_RACE = 0x004ece34, STR_STAGE = 0x004ece3c,
    FMT_OUT_OF_ROOM = 0x004ece58,      // "Can't record %s, out of room!"
    STR_SCO = 0x004ece90,              // ".sco"
    FMT_NO_SCO = 0x004ece98,           // "SCO file !exists or !legit; creating empty one"
    FMT_CANT_CREATE = 0x004ecec8,      // "Can't create SCO! Grak! This is impossible!"
    FMT_DUMP = 0x004ecef4, FMT_DUMP_SPEED = 0x004ecf10, FMT_DUMP_LAP = 0x004ecf34, FMT_DUMP_RACE = 0x004ecf58,
    C_225 = 0x004db88c,                // float 2.25 (dump's speed factor)
    FILE_SIZE = 0x114c8, FILE_VERSION = 4,
    BIG_TIME = 0x4ceb79a3,             // 123456789.0f: best_laptime_for_car's "none yet"
    MINUS_ONE = 0xbf800000,            // -1.0f
};
static inline RecordList* races() { return P<RecordList>(S_RACES); }
static inline RecordList* laps() { return P<RecordList>(S_LAPS); }
static inline RecordList* stages() { return P<RecordList>(S_STAGES); }
static inline RecordMgr* mgr() { return *P<RecordMgr*>(S_MGR); }
static inline uint8_t* rec_at(const RecordList* l, int32_t i) { return (uint8_t*)l->recs + (uint32_t)i * 0x48; }
// a mode's baztag index, as the original computes it (hi + realism*2; no bounds)
static inline uint32_t mode_idx(const RecordMgr* m) { return (uint32_t)(m->race_type >= 4) + (uint32_t)m->realism * 2; }

}  // namespace

// ---- the game's functions these call (by v1.0 address) -----------------------------------------------------------
typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* Free_t)(void*);
typedef const uint8_t*(__cdecl* CarMgrGetInfo_t)(int);
typedef int(__cdecl* IntGet_t)(void);
typedef void(__cdecl* TimeOfDay_t)(void*);
typedef const uint8_t*(__cdecl* WorldGameOptions_t)(void);
typedef uint8_t(__cdecl* ByteGet_t)(void);
typedef void(__cdecl* Void_t)(void);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef const char*(__cdecl* UserDir_t)(void);
typedef int(__cdecl* FileOpen_t)(const char*);
typedef uint8_t(__cdecl* FileRead_t)(int, void*, int*);
typedef void(__cdecl* FileClose_t)(int*);
typedef void(__cdecl* FileSetWritable_t)(const char*, uint8_t);
typedef uint8_t(__cdecl* FileRemove_t)(const char*);
typedef int(__cdecl* FileCreate_t)(const char*);
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
typedef void*(__cdecl* memmove_t)(void*, const void*, size_t);

static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
static const Free_t op_delete = (Free_t)0x00414390;
static const CarMgrGetInfo_t CarMgrGetInfo = (CarMgrGetInfo_t)0x00464490;
static const IntGet_t CarMgrCount = (IntGet_t)0x00464480;
static const TimeOfDay_t TimeGetTimeOfDay = (TimeOfDay_t)0x004d8af0;
static const WorldGameOptions_t WorldGameOptions = (WorldGameOptions_t)0x004627a0;
static const ByteGet_t MultiEnabled = (ByteGet_t)0x004a23c0;
static const Void_t MultiConsolidateRecords = (Void_t)0x004a2b60;
static const Log_t LogReport = (Log_t)0x00411150;
static const Log_t LogPanic = (Log_t)0x004112b0;
static const UserDir_t Win32GetUserDirectory = (UserDir_t)0x00412cc0;
static const FileOpen_t FileOpen = (FileOpen_t)0x00411780;
static const FileRead_t FileRead = (FileRead_t)0x00411940;
static const FileClose_t FileClose = (FileClose_t)0x00411850;
static const FileSetWritable_t FileSetWritable = (FileSetWritable_t)0x00411b80;
static const FileRemove_t FileRemove = (FileRemove_t)0x00411ba0;
static const FileCreate_t FileCreate = (FileCreate_t)0x004115f0;
static const FileWrite_t FileWrite = (FileWrite_t)0x00411a30;
static const memmove_t g_memmove = (memmove_t)0x004cf400;                 // the game's CRT
// this file's own functions, called by address so the hooked rewrite (or the original) is what runs
typedef void(__fastcall* InitBase_t)(RecordMgr*, Edx, RaceRecord*, int);
typedef void(__fastcall* Method_t)(void*, Edx);
typedef void*(__fastcall* Ctor_t)(void*, Edx);
typedef void*(__fastcall* MgrCtor_t)(void*, Edx, const char*);
typedef void(__fastcall* SetMode_t)(RecordMgr*, Edx, int, int, uint8_t);
typedef void(__fastcall* Check_t)(RecordMgr*, Edx, RaceRecord*, int);
typedef float(__cdecl* CarFloat_t)(int);
typedef void(__cdecl* SetRaceTime_t)(int, uint32_t);                      // (car, float bits)
typedef RaceRecord*(__cdecl* Record_t)(RecordList*, int, int, int, uint32_t, uint8_t);
typedef void(__cdecl* NewLap_t)(int, int, uint32_t, uint32_t, uint8_t);
typedef void(__cdecl* RaceOver_t)(int, uint8_t);
typedef void(__cdecl* Stage_t)(int, int, int, uint32_t, uint8_t);
static const InitBase_t init_baserecordA = (InitBase_t)0x00429f80;
static const Void_t RecordResetA = (Void_t)0x0042a030;
static const Void_t create_dnfsA = (Void_t)0x0042a360;
static const CarFloat_t best_laptime_for_carA = (CarFloat_t)0x0042a410;
static const CarFloat_t best_speed_for_carA = (CarFloat_t)0x0042a480;
static const SetRaceTime_t set_racetime_for_carA = (SetRaceTime_t)0x0042a4d0;
static const Void_t set_best_stagesA = (Void_t)0x0042a510;
static const NewLap_t record_new_lapA = (NewLap_t)0x0042a5f0;
static const Record_t recordA = (Record_t)0x0042a630;
static const RaceOver_t record_race_overA = (RaceOver_t)0x0042a730;
static const CarFloat_t sum_times_for_carA = (CarFloat_t)0x0042a790;
static const Stage_t record_stageA = (Stage_t)0x0042a810;
static const Method_t clear_record_bitsA = (Method_t)0x0042a9d0;
static const MgrCtor_t RecordMgr_ctorA = (MgrCtor_t)0x0042aa30;
static const Method_t RecordMgr_saveA = (Method_t)0x0042ac20;
static const Method_t RecordMgr_dtorA = (Method_t)0x0042acf0;
static const SetMode_t RecordMgr_SetModeA = (SetMode_t)0x0042ad50;
static const Check_t RecordMgr_CheckLapsA = (Check_t)0x0042ae80;
static const Check_t RecordMgr_CheckRaceA = (Check_t)0x0042b260;
static const Ctor_t RaceRecord_ctorA = (Ctor_t)0x0042b620;
static const Ctor_t bartag_ctorA = (Ctor_t)0x0042b630;
static const Ctor_t baztag_ctorA = (Ctor_t)0x0042b660;
static const Ctor_t footag_ctorA = (Ctor_t)0x0042b6b0;

// ---- footprint helpers ----------------------------------------------------------------------------------------------
static void fp_statics(Footprint& f) { f.add(P<void>(S_STATICS), STATICS_N, "record.obj statics"); }
static bool list_sane(const RecordList* l) { return l->cap >= 0 && l->count >= 0 && l->count <= l->cap && (l->recs || l->cap == 0); }
static void fp_list(Footprint& f, const RecordList* l) {                 // a list's whole buffer (capacity-bounded)
    if (l->recs && l->cap > 0) f.add(l->recs, (uint32_t)l->cap * 0x48, "records");
}
static void fp_all_lists(Footprint& f) { fp_list(f, races()); fp_list(f, laps()); fp_list(f, stages()); }
static bool lists_sane() { return list_sane(races()) && list_sane(laps()) && list_sane(stages()); }
static bool mode_in_file(const RecordMgr* m) {                            // the mode's tables lie inside the file
    uint32_t idx = mode_idx(m);
    return m->file && idx < 12 && (uint32_t)m->race_type < 8;
}
static void fp_car_query(Footprint&, int) {}                             // reads only

// =================================================================================================================
// the constructors
// =================================================================================================================

static RaceRecord* __fastcall RaceRecord_ctor(RaceRecord* self, Edx) {
    self->flags = 0;
    return self;
}
static void fp_race_record_ctor(Footprint& f, RaceRecord* self, Edx) { f.add(self, sizeof *self, "RaceRecord"); }
PORT_FN(0x0042b620, "RaceRecord::RaceRecord", RaceRecord_ctor, fp_race_record_ctor)

static Footag* __fastcall footag_ctor(Footag* self, Edx) {
    for (int i = 0; i < 10; i++) RaceRecord_ctorA(&self->r[i], 0);
    return self;
}
static void fp_footag_ctor(Footprint& f, Footag* self, Edx) { f.add(self, sizeof *self, "footag"); }
PORT_FN(0x0042b6b0, "RecordFile::bartag::baztag::footag::footag", footag_ctor, fp_footag_ctor)

static Baztag* __fastcall baztag_ctor(Baztag* self, Edx) {
    RaceRecord_ctorA(&self->speed, 0);
    RaceRecord_ctorA(&self->lap, 0);
    for (int i = 0; i < 8; i++) footag_ctorA(&self->races[i], 0);
    return self;
}
static void fp_baztag_ctor(Footprint& f, Baztag* self, Edx) { f.add(self, sizeof *self, "baztag"); }
PORT_FN(0x0042b660, "RecordFile::bartag::baztag::baztag", baztag_ctor, fp_baztag_ctor)

static Bartag* __fastcall bartag_ctor(Bartag* self, Edx) {
    for (int i = 0; i < 2; i++) baztag_ctorA(&self->baz[i], 0);
    return self;
}
static void fp_bartag_ctor(Footprint& f, Bartag* self, Edx) { f.add(self, sizeof *self, "bartag"); }
PORT_FN(0x0042b630, "RecordFile::bartag::bartag", bartag_ctor, fp_bartag_ctor)

// =================================================================================================================
// this race's records
// =================================================================================================================

// init_baserecord: a cleared record with the car's names (strcpy, unbounded), the date, the car, flags 1 (0x11
// when CarMgrInfo+4 is set) and the manager's race type
static void __fastcall RecordMgr_init_baserecord(RecordMgr* self, Edx, RaceRecord* rec, int car) {
    stosd0(rec, 0x12);
    const uint8_t* info = CarMgrGetInfo(car);
    str_copy(rec->car_name, (const char*)info + 5);
    str_copy(rec->driver_name, (const char*)info + 0x12);
    const int8_t c = (int8_t)rec->driver_name[0];                      // cmp al, 'a'; jl (signed)
    if (c >= 0x61 && c <= 0x7a) rec->driver_name[0] = (char)(c & 0xdf);
    memcpy(rec->date, P<void>(S_DATE), 4);
    rec->car = (uint8_t)car;
    rec->flags = 1;
    if (info[4] != 0) rec->flags = 0x11;
    rec->race_type = (uint8_t)self->race_type;
}
static void fp_init_baserecord(Footprint& f, RecordMgr*, Edx, RaceRecord* rec, int) { f.add(rec, sizeof *rec, "RaceRecord"); }
PORT_FN(0x00429f80, "RecordMgr::init_baserecord", RecordMgr_init_baserecord, fp_init_baserecord)

static void __cdecl RecordReset(void) {
    races()->count = 0;
    laps()->count = 0;
    stages()->count = 0;
}
PORT_FN(0x0042a030, "RecordReset", RecordReset, fp_statics)

// RecordBegin: the three lists (count 0, each record constructed) and the RecordMgr for the world's track, in
// the world's mode; then the date
static void make_list(RecordList* l, uint32_t name, int32_t cap) {
    l->name = P<const char>(name);
    l->cap = cap;
    l->count = 0;
    uint8_t* p = (uint8_t*)MemAlloc((int32_t)((uint32_t)cap * 0x48));
    if (p) {
        uint8_t* r = p;
        for (int32_t i = cap - 1; i >= 0; i--, r += 0x48) RaceRecord_ctorA(r, 0);
        l->recs = (RaceRecord*)p;
    } else l->recs = 0;
}
static void __cdecl RecordBegin(const uint8_t* world) {
    RecordResetA();
    const uint32_t* w = (const uint32_t*)world;
    make_list(laps(), STR_LAP, (int32_t)(w[0xca8 / 4] * w[0xcc0 / 4]));         // cars x laps (imul: wraps)
    make_list(races(), STR_RACE, (int32_t)w[0xca8 / 4]);                         // cars
    make_list(stages(), STR_STAGE, (int32_t)(w[0xca8 / 4] * w[0xcc0 / 4] * 3));  // cars x laps x 3
    void* p = MemAlloc(0x1c);
    *P<RecordMgr*>(S_MGR) = p ? (RecordMgr*)RecordMgr_ctorA(p, 0, (const char*)world + 8) : 0;
    RecordMgr_SetModeA(mgr(), 0, at<int32_t>((void*)world, 0xcac), at<int32_t>((void*)world, 0xcbc), at<uint8_t>((void*)world, 0xcd1));
    uint8_t tod[0x14];
    TimeGetTimeOfDay(tod);
    memcpy(P<void>(S_DATE), tod, 2);                                    // year
    *P<uint8_t>(S_DATE + 2) = tod[4];                                   // day
    *P<uint8_t>(S_DATE + 3) = tod[2];                                   // month
}
static void fp_record_begin(Footprint& f, const uint8_t*) { f.replay_only = "allocates the lists and the RecordMgr; loads the records file"; }
PORT_FN(0x0042a050, "RecordBegin", RecordBegin, fp_record_begin)

static void __cdecl RecordEnd(void) {
    op_delete(stages()->recs);
    stages()->recs = 0;
    op_delete(races()->recs);
    races()->recs = 0;
    op_delete(laps()->recs);
    laps()->recs = 0;
    RecordMgr* m = mgr();
    if (m) {
        RecordMgr_dtorA(m, 0);
        op_delete(m);
    }
    *P<RecordMgr*>(S_MGR) = 0;
}
static void fp_record_end(Footprint& f) { f.replay_only = "frees the lists and the RecordMgr; saves the records file"; }
PORT_FN(0x0042a210, "RecordEnd", RecordEnd, fp_record_end)

// RecordConsolidate: after the race. Game options +0xc of 4 or 5 skip the race table.
static void __cdecl RecordConsolidate(void) {
    const int32_t kind = at<int32_t>((void*)WorldGameOptions(), 0xc);
    uint8_t no_race = kind == 5 ? 1 : kind == 4 ? 1 : 0;
    if (MultiEnabled()) MultiConsolidateRecords();
    create_dnfsA();
    RecordMgr_CheckLapsA(mgr(), 0, laps()->recs, laps()->count);
    if (!no_race) RecordMgr_CheckRaceA(mgr(), 0, races()->recs, races()->count);
    set_best_stagesA();
    // a car's lap / speed bests mark its race record too (the lap's whole flags byte is or-ed in)
    for (int32_t i = 0; laps()->count > i; i++) {
        if ((rec_at(laps(), i)[0x40] & 6) == 0) continue;
        for (int32_t j = 0; races()->count > j; j++) {
            uint8_t* r = rec_at(races(), j);
            const uint8_t* l = rec_at(laps(), i);
            if (r[0x41] == l[0x41]) r[0x40] |= l[0x40];
        }
    }
    RecordMgr_saveA(mgr(), 0);
}
static void fp_record_consolidate(Footprint& f) { f.replay_only = "saves the records file (and MultiConsolidateRecords)"; }
PORT_FN(0x0042a280, "RecordConsolidate", RecordConsolidate, fp_record_consolidate)

// create_dnfs: every car without a race record gets one, race time -1 (and so do its lap records:
// set_racetime_for_car). The search runs over the count as it was on entry (read once), the capacity check over
// the current one.
static void __cdecl create_dnfs(void) {
    const int32_t n = CarMgrCount();
    const int32_t count0 = races()->count;
    for (int32_t car = 0; car < n; car++) {
        uint8_t found = 0;
        if (count0 > 0) {
            const uint8_t* p = rec_at(races(), 0) + 0x41;
            for (int32_t i = 0; count0 > i; i++, p += 0x48)
                if (*p == (uint8_t)car) { found = 1; break; }
        }
        if (found || races()->count >= races()->cap) continue;
        const int32_t k = races()->count;
        RecordMgr* m = mgr();
        races()->count = k + 1;
        RaceRecord* rec = (RaceRecord*)rec_at(races(), k);
        init_baserecordA(m, 0, rec, car);
        at<uint32_t>(rec, 0xc) = MINUS_ONE;
        rec->lap_time = best_laptime_for_carA(car);                       // fstp dword
        rec->top_speed = best_speed_for_carA(car);
        set_racetime_for_carA(car, at<uint32_t>(rec, 0xc));
    }
}
static void fp_create_dnfs(Footprint& f) {
    if (!list_sane(races()) || !list_sane(laps())) { f.replay_only = "a list count outside its capacity"; return; }
    fp_statics(f);
    fp_list(f, races());
    fp_list(f, laps());                                                  // (set_racetime_for_car)
}
PORT_FN(0x0042a360, "create_dnfs", create_dnfs, fp_create_dnfs)

// best_laptime_for_car: the car's fastest lap (a NaN one wins, and then the next lap wins over it), -1 if none
static float __cdecl best_laptime_for_car(int car) {
    uint32_t best = BIG_TIME;
    if (laps()->count > 0) {
        const uint8_t* p = (const uint8_t*)laps()->recs + 4;
        for (int32_t n = laps()->count; n != 0; n--, p += 0x48)
            if (p[0x3d] == (uint8_t)car && !(D(at<float>((void*)p, 0)) >= D(Fb(best)))) best = at<uint32_t>((void*)p, 0);
    }
    if (best == BIG_TIME) best = MINUS_ONE;
    return Fb(best);
}
PORT_FN(0x0042a410, "best_laptime_for_car", best_laptime_for_car, fp_car_query)

// best_speed_for_car: the car's highest lap top speed (NaNs never win), -1 if none
static float __cdecl best_speed_for_car(int car) {
    uint32_t best = MINUS_ONE;
    if (laps()->count > 0) {
        const uint8_t* p = (const uint8_t*)laps()->recs + 8;
        for (int32_t n = laps()->count; n != 0; n--, p += 0x48)
            if (p[0x39] == (uint8_t)car && D(at<float>((void*)p, 0)) > D(Fb(best))) best = at<uint32_t>((void*)p, 0);
    }
    return Fb(best);
}
PORT_FN(0x0042a480, "best_speed_for_car", best_speed_for_car, fp_car_query)

// set_racetime_for_car: every LAP record of the car gets the race time (bits) -- the lap list, not the race list
static void __cdecl set_racetime_for_car(int car, uint32_t time) {
    for (int32_t i = 0; laps()->count > i; i++) {
        uint8_t* r = rec_at(laps(), i);
        if (r[0x41] == (uint8_t)car) at<uint32_t>(r, 0xc) = time;
    }
}
static void fp_set_racetime(Footprint& f, int, uint32_t) {
    if (!list_sane(laps())) { f.replay_only = "a lap list count outside its capacity"; return; }
    fp_list(f, laps());
}
PORT_FN(0x0042a4d0, "set_racetime_for_car", set_racetime_for_car, fp_set_racetime)

// set_best_stages: per car and stage (a table of 16 x 3 on the stack, indexed car*3 + stage - 1 with the car
// signed), the fastest stage record (the first of equals; a NaN never replaces one) gets flag 2. An index
// outside the table writes the original's own stack (saved registers, the return address): not reproduced --
// those records are left alone here (the footprint makes such a call replay_only).
static void __cdecl set_best_stages(void) {
    struct Slot { int32_t idx; uint32_t time; };
    Slot tab[48];
    for (int i = 0; i < 48; i++) { tab[i].idx = -1; tab[i].time = 0; }
    for (int32_t i = 0; stages()->count > i; i++) {
        const uint8_t* r = rec_at(stages(), i);
        const int32_t k = (int32_t)(int8_t)r[0x41] * 3 + (int32_t)r[0x42] - 1;
        if ((uint32_t)k >= 48) continue;                                // (the original's stack)
        Slot& s = tab[k];
        if (s.idx == -1 || D(Fb(s.time)) > D(at<float>((void*)r, 4))) {
            s.idx = i;
            s.time = at<uint32_t>((void*)r, 4);
        }
    }
    for (int i = 0; i < 48; i++)
        if (tab[i].idx != -1) rec_at(stages(), tab[i].idx)[0x40] |= 2;
}
static void fp_set_best_stages(Footprint& f) {
    if (!list_sane(stages())) { f.replay_only = "a stage list count outside its capacity"; return; }
    for (int32_t i = 0; i < stages()->count; i++) {
        const uint8_t* r = rec_at(stages(), i);
        if ((uint32_t)((int32_t)(int8_t)r[0x41] * 3 + (int32_t)r[0x42] - 1) >= 48) {
            f.replay_only = "a stage record indexes past the original's stack table";
            return;
        }
    }
    fp_list(f, stages());
}
PORT_FN(0x0042a510, "set_best_stages", set_best_stages, fp_set_best_stages)

// record: the record for (car, lap, stage) in the list -- an unofficial one found is reused, an official one found
// means nothing is recorded (0); otherwise the next free one, or LogPanic and 0 when the list is full. Then
// init_baserecord, the unofficial flag, a, b, the time; top speed 0.
static RaceRecord* __cdecl record(RecordList* list, int car, int lap, int stage, uint32_t time, uint8_t unofficial) {
    RaceRecord* rec = 0;
    const int32_t n = list->count;
    if (n > 0) {
        const uint8_t* p = (const uint8_t*)list->recs + 0x41;
        for (int32_t i = 0; i < n; i++, p += 0x48)
            if (p[0] == (uint8_t)car && (int32_t)p[2] == lap && p[1] == (uint8_t)stage) {
                rec = (RaceRecord*)rec_at(list, i);
                if (!(rec->flags & 0x20)) return 0;
                break;
            }
    }
    if (!rec) {
        if (!(list->cap > n)) {
            LogPanic(P<const char>(FMT_OUT_OF_ROOM), list->name);
            return 0;
        }
        rec = (RaceRecord*)rec_at(list, n);
        list->count = n + 1;
    }
    init_baserecordA(mgr(), 0, rec, car);
    if (unofficial) rec->flags |= 0x20;
    rec->stage = (uint8_t)stage;
    rec->lap = (uint8_t)lap;
    at<uint32_t>(rec, 4) = time;
    at<uint32_t>(rec, 8) = 0;
    return rec;
}
// will this call find its record, or take a free one, or panic?
static bool record_panics(const RecordList* list, int car, int lap, int stage) {
    for (int32_t i = 0; i < list->count; i++) {
        const uint8_t* p = rec_at(list, i) + 0x41;
        if (p[0] == (uint8_t)car && (int32_t)p[2] == lap && p[1] == (uint8_t)stage) return false;
    }
    return !(list->cap > list->count);
}
static void fp_record_in(Footprint& f, const RecordList* list, int car, int lap, int stage) {
    if (!list_sane(list)) { f.replay_only = "a list count outside its capacity"; return; }
    if (record_panics(list, car, lap, stage)) { f.replay_only = "LogPanic: the list is out of room"; return; }
    fp_statics(f);
    f.add((void*)list, sizeof *list, "RecordList");
    fp_list(f, list);
}
static void fp_record(Footprint& f, RecordList* list, int car, int lap, int stage, uint32_t, uint8_t) { fp_record_in(f, list, car, lap, stage); }
PORT_FN(0x0042a630, "record", record, fp_record)

// record_new_lap: record(lap list, car, lap, 0xfa, time), then the lap (again, as a byte) and the top speed
static void __cdecl record_new_lap(int car, int lap, uint32_t time, uint32_t speed, uint8_t unofficial) {
    RaceRecord* rec = recordA(laps(), car, lap, 0xfa, time, unofficial);
    if (rec) {
        rec->lap = (uint8_t)lap;
        at<uint32_t>(rec, 8) = speed;
    }
}
static void fp_record_new_lap(Footprint& f, int car, int lap, uint32_t, uint32_t, uint8_t) { fp_record_in(f, laps(), car, lap, 0xfa); }
PORT_FN(0x0042a5f0, "record_new_lap", record_new_lap, fp_record_new_lap)

static void __cdecl RecordNewLap(int car, int lap, uint32_t time, uint32_t speed) { record_new_lapA(car, lap, time, speed, 0); }
static void fp_record_newlap(Footprint& f, int car, int lap, uint32_t, uint32_t) { fp_record_in(f, laps(), car, lap, 0xfa); }
PORT_FN(0x0042a700, "RecordNewLap", RecordNewLap, fp_record_newlap)
static void __cdecl RecordUnofficialNewLap(int car, int lap, uint32_t time, uint32_t speed) { record_new_lapA(car, lap, time, speed, 1); }
PORT_FN(0x0042a5d0, "RecordUnofficialNewLap", RecordUnofficialNewLap, fp_record_newlap)

// record_race_over: record(race list, car, 0xfa, 0xfa, the car's best lap), then its race time (the sum of its
// laps), best speed, and the race time into every LAP record of the car (set_racetime_for_car)
static void __cdecl record_race_over(int car, uint8_t unofficial) {
    const float best = best_laptime_for_carA(car);                     // fstp dword [esp]: the argument
    RaceRecord* rec = recordA(races(), car, 0xfa, 0xfa, Ub(best), unofficial);
    if (rec) {
        rec->race_time = sum_times_for_carA(car);
        rec->top_speed = best_speed_for_carA(car);
        set_racetime_for_carA(car, at<uint32_t>(rec, 0xc));
    }
}
static void fp_race_over(Footprint& f, int car) {                     // the race record, and the car's laps' race time
    fp_record_in(f, races(), car, 0xfa, 0xfa);
    if (f.replay_only) return;
    if (!list_sane(laps())) { f.replay_only = "a lap list count outside its capacity"; return; }
    fp_list(f, laps());
}
static void fp_record_race_over(Footprint& f, int car, uint8_t) { fp_race_over(f, car); }
PORT_FN(0x0042a730, "record_race_over", record_race_over, fp_record_race_over)

static void __cdecl RecordRaceOver(int car) { record_race_overA(car, 0); }
static void fp_record_raceover(Footprint& f, int car) { fp_race_over(f, car); }
PORT_FN(0x0042a720, "RecordRaceOver", RecordRaceOver, fp_record_raceover)
static void __cdecl RecordUnofficialRaceOver(int car) { record_race_overA(car, 1); }
PORT_FN(0x0042a7e0, "RecordUnofficialRaceOver", RecordUnofficialRaceOver, fp_record_raceover)

// sum_times_for_car: the car's lap times added in ST0 (each fadd rounded by the precision control), stored
static float __cdecl sum_times_for_car(int car) {
    float sum = 0.0f;
    if (laps()->count > 0) {
        double acc = D(sum);
        const uint8_t* p = (const uint8_t*)laps()->recs + 0x41;
        for (int32_t n = laps()->count; n != 0; n--, p += 0x48)
            if (p[0] == (uint8_t)car) acc = acc + D(*(const float*)(p - 0x3d));   // fadd dword [rec+4]
        sum = (float)acc;
    }
    return sum;
}
PORT_FN(0x0042a790, "sum_times_for_car", sum_times_for_car, fp_car_query)

// record_stage(car, stage, lap): record(stage list, car, lap, stage, time) -- the two swapped; its result dropped
static void __cdecl record_stage(int car, int stage, int lap, uint32_t time, uint8_t unofficial) { recordA(stages(), car, lap, stage, time, unofficial); }
static void fp_record_stage(Footprint& f, int car, int stage, int lap, uint32_t, uint8_t) { fp_record_in(f, stages(), car, lap, stage); }
PORT_FN(0x0042a810, "record_stage", record_stage, fp_record_stage)

static void __cdecl RecordStage(int car, int stage, int lap, uint32_t time) { record_stageA(car, stage, lap, time, 0); }
static void fp_record_stage4(Footprint& f, int car, int stage, int lap, uint32_t) { fp_record_in(f, stages(), car, lap, stage); }
PORT_FN(0x0042a7f0, "RecordStage", RecordStage, fp_record_stage4)
static void __cdecl RecordUnofficialStage(int car, int stage, int lap, uint32_t time) { record_stageA(car, stage, lap, time, 1); }
PORT_FN(0x0042a840, "RecordUnofficialStage", RecordUnofficialStage, fp_record_stage4)

// ---- the result queries (read only) ----------------------------------------------------------------------------------
// RecordGetRaceTime: the race time of the car's first official race record; else of its last unofficial one; -1
static float __cdecl RecordGetRaceTime(int car) {
    uint32_t t = MINUS_ONE;
    for (int32_t i = 0; races()->count > i; i++) {
        const uint8_t* r = rec_at(races(), 0) + (uint32_t)i * 0x48;      // (the base read once)
        if (r[0x41] != (uint8_t)car) continue;
        t = at<uint32_t>((void*)r, 0xc);
        if (!(r[0x40] & 0x20)) break;
    }
    return Fb(t);
}
PORT_FN(0x0042a860, "RecordGetRaceTime", RecordGetRaceTime, fp_car_query)

static const RaceRecord* __cdecl RecordGetRaceResult(int car) {
    for (int32_t i = 0; races()->count > i; i++)
        if (rec_at(races(), 0)[(uint32_t)i * 0x48 + 0x41] == (uint8_t)car) return (const RaceRecord*)rec_at(races(), i);
    return 0;
}
static void fp_get_result1(Footprint&, int) {}
PORT_FN(0x0042a8b0, "RecordGetRaceResult", RecordGetRaceResult, fp_get_result1)

static const RaceRecord* __cdecl RecordGetStageResult(int car, int lap, int stage) {
    const uint8_t* p = (const uint8_t*)stages()->recs;
    for (int32_t i = 0; stages()->count > i; i++, p += 0x48)
        if (p[0x41] == (uint8_t)car && (int32_t)p[0x43] == lap && (int32_t)p[0x42] == stage) return (const RaceRecord*)p;
    return 0;
}
static void fp_get_result3(Footprint&, int, int, int) {}
PORT_FN(0x0042a8f0, "RecordGetStageResult", RecordGetStageResult, fp_get_result3)

static const RaceRecord* __cdecl RecordGetLapResult(int car, int lap) {
    for (int32_t i = 0; laps()->count > i; i++) {
        const uint8_t* p = rec_at(laps(), 0) + (uint32_t)i * 0x48;
        if (p[0x41] == (uint8_t)car && p[0x43] == (uint8_t)lap) return (const RaceRecord*)rec_at(laps(), i);
    }
    return 0;
}
static void fp_get_result2(Footprint&, int, int) {}
PORT_FN(0x0042a940, "RecordGetLapResult", RecordGetLapResult, fp_get_result2)

// =================================================================================================================
// RecordMgr and the records file
// =================================================================================================================

static const RecordMgr* __cdecl RecordMgrCreate(const char* name) {
    void* p = MemAlloc(0x1c);
    if (!p) return 0;
    return (const RecordMgr*)RecordMgr_ctorA(p, 0, name);
}
static void fp_mgr_create(Footprint& f, const char*) { f.replay_only = "allocates; loads the records file"; }
PORT_FN(0x0042a990, "RecordMgrCreate", RecordMgrCreate, fp_mgr_create)

static void __cdecl RecordMgrDestroy(const RecordMgr* m) {
    if (!m) return;
    RecordMgr_dtorA((void*)m, 0);
    op_delete((void*)m);
}
static void fp_mgr_destroy(Footprint& f, const RecordMgr*) { f.replay_only = "frees; saves the records file"; }
PORT_FN(0x0042a9b0, "RecordMgrDestroy", RecordMgrDestroy, fp_mgr_destroy)

// clear_record_bits: the "new record" bits (2, 4, 8) off every record of the file
static void __fastcall RecordFile_clear_record_bits(RecordFile* self, Edx) {
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 2; j++) {
            Baztag& z = self->bars[i].baz[j];
            z.speed.flags &= 0xf1;
            z.lap.flags &= 0xf1;
            for (int k = 0; k < 8; k++)
                for (int m = 0; m < 10; m++) z.races[k].r[m].flags &= 0xf1;
        }
}
static void fp_clear_record_bits(Footprint& f, RecordFile* self, Edx) { f.add(self, sizeof *self, "RecordFile"); }
PORT_FN(0x0042a9d0, "RecordFile::clear_record_bits", RecordFile_clear_record_bits, fp_clear_record_bits)

// the constructor: an empty file (the six bartags constructed, then all of it zeroed and the header set);
// the name, its 3-letter extension stripped, + ".sco" (strcpy / strcat into 15 bytes, unbounded); load
// <user dir><name>; a missing, short or foreign file is removed and recreated empty (and saved)
static RecordMgr* __fastcall RecordMgr_ctor(RecordMgr* self, Edx, const char* name) {
    self->race_type = 0;
    self->realism = 0;
    self->dirty = 0;
    RecordFile* file = (RecordFile*)MemAlloc(FILE_SIZE);
    if (file) {
        for (int i = 0; i < 6; i++) bartag_ctorA(&file->bars[i], 0);
        stosd0(file, FILE_SIZE / 4);
        file->size = FILE_SIZE;
        file->version = FILE_VERSION;
        self->file = file;
    } else self->file = 0;
    char* fn = (char*)self + 1;
    str_copy(fn, name);
    if (fn[(int32_t)strlen(fn) - 4] == '.') fn[(int32_t)strlen(fn) - 4] = 0;   // (len < 4: reads before it, as the original)
    memcpy(fn + strlen(fn), P<void>(STR_SCO), 5);                       // ".sco" (a dword and a byte)
    char path[0x100];
    str_copy(path, Win32GetUserDirectory());
    str_cat(path, fn);
    int fd = FileOpen(path);
    int size = FILE_SIZE;
    if (fd && FileRead(fd, self->file, &size) && self->file->size == FILE_SIZE && self->file->version == FILE_VERSION) {
        FileClose(&fd);
        return self;
    }
    LogReport(P<const char>(FMT_NO_SCO));
    if (fd) FileClose(&fd);
    FileSetWritable(path, 1);
    FileRemove(path);
    RecordFile* f = self->file;
    self->dirty = 1;
    stosd0(f, FILE_SIZE / 4);
    f->size = FILE_SIZE;
    f->version = FILE_VERSION;
    RecordMgr_saveA(self, 0);
    return self;
}
static void fp_mgr_ctor(Footprint& f, RecordMgr* self, Edx, const char*) {
    f.add(self, sizeof *self, "RecordMgr");
    f.replay_only = "allocates; loads (or recreates and saves) the records file";
}
PORT_FN(0x0042aa30, "RecordMgr::RecordMgr", RecordMgr_ctor, fp_mgr_ctor)

// save: when dirty, the new-record bits cleared and the whole file written to <user dir><name>
static void __fastcall RecordMgr_save(RecordMgr* self, Edx) {
    if (!self->dirty) return;
    clear_record_bitsA(self->file, 0);
    self->dirty = 0;
    char path[0x100];
    str_copy(path, Win32GetUserDirectory());
    str_cat(path, (const char*)self + 1);                              // (an overlong name runs on into +0x10)
    int fd = FileCreate(path);
    if (fd) {
        FileWrite(fd, self->file, FILE_SIZE);
        FileClose(&fd);
    } else LogPanic(P<const char>(FMT_CANT_CREATE));
}
static void fp_mgr_save(Footprint& f, RecordMgr* self, Edx) {
    if (self->dirty) f.replay_only = "writes the records file";
}
PORT_FN(0x0042ac20, "RecordMgr::save", RecordMgr_save, fp_mgr_save)

static void __fastcall RecordMgr_dtor(RecordMgr* self, Edx) {
    RecordMgr_saveA(self, 0);
    op_delete(self->file);
    self->file = 0;
}
static void fp_mgr_dtor(Footprint& f, RecordMgr* self, Edx) {
    f.add(self, sizeof *self, "RecordMgr");
    f.replay_only = "saves the records file; frees it";
}
PORT_FN(0x0042acf0, "RecordMgr::~RecordMgr", RecordMgr_dtor, fp_mgr_dtor)

// Clear: the current mode's baztag emptied (the file marked dirty)
static void __fastcall RecordMgr_Clear(RecordMgr* self, Edx) {
    stosd0((uint8_t*)self->file + 8 + mode_idx(self) * 0x1710, 0x5c4);
    self->dirty = 1;
}
static void fp_mgr_clear(Footprint& f, RecordMgr* self, Edx) {
    if (!self->file || mode_idx(self) >= 12) { f.replay_only = "a mode outside the records file"; return; }
    f.add(self, sizeof *self, "RecordMgr");
    f.add((uint8_t*)self->file + 8 + mode_idx(self) * 0x1710, 0x1710, "RecordFile baztag");
}
PORT_FN(0x0042ad10, "RecordMgr::Clear", RecordMgr_Clear, fp_mgr_clear)

static void __fastcall RecordMgr_SetMode(RecordMgr* self, Edx, int realism, int race_type, uint8_t sim) {
    self->race_type = race_type;
    self->realism = sim ? realism + 3 : realism;
}
static void fp_mgr_setmode(Footprint& f, RecordMgr* self, Edx, int, int, uint8_t) { f.add(self, sizeof *self, "RecordMgr"); f.pure = true; }
PORT_FN(0x0042ad50, "RecordMgr::SetMode", RecordMgr_SetMode, fp_mgr_setmode)

// GetNthBest*: the mode's n-th best (0 = best), or 0 when out of range or the slot is empty
static const RaceRecord* __fastcall RecordMgr_GetNthBestRace(const RecordMgr* self, Edx, int n) {
    const int32_t i = 9 - n;
    const int32_t rt = self->race_type;
    if (i < 0 || i >= 10) return 0;
    const uint32_t k = ((uint32_t)(rt >= 4) + (uint32_t)self->realism * 2) * 82 + (uint32_t)rt * 10 + (uint32_t)i;
    const uint8_t* r = (const uint8_t*)self->file + k * 0x48;
    if (!(r[0xd8] & 1)) return 0;
    return (const RaceRecord*)(r + 0x98);
}
static void fp_get_nth(Footprint&, const RecordMgr*, Edx, int) {}
PORT_FN(0x0042ad80, "RecordMgr::GetNthBestRace", RecordMgr_GetNthBestRace, fp_get_nth)

static const RaceRecord* __fastcall RecordMgr_GetNthBestLap(const RecordMgr* self, Edx, int n) {
    const int32_t i = -n;
    if (i < 0 || i >= 1) return 0;
    const uint32_t k = ((uint32_t)(self->race_type >= 4) + (uint32_t)self->realism * 2) * 82 + (uint32_t)i;
    const uint8_t* r = (const uint8_t*)self->file + k * 0x48;
    if (!(r[0x90] & 1)) return 0;
    return (const RaceRecord*)(r + 0x50);
}
PORT_FN(0x0042ade0, "RecordMgr::GetNthBestLap", RecordMgr_GetNthBestLap, fp_get_nth)

static const RaceRecord* __fastcall RecordMgr_GetNthBestSpeed(const RecordMgr* self, Edx, int n) {
    const int32_t i = -n;
    if (i < 0 || i >= 1) return 0;
    const uint32_t k = ((uint32_t)(self->race_type >= 4) + (uint32_t)self->realism * 2) * 82 + (uint32_t)i;
    const uint8_t* r = (const uint8_t*)self->file + k * 0x48;
    if (!(r[0x48] & 1)) return 0;
    return (const RaceRecord*)(r + 8);
}
PORT_FN(0x0042ae30, "RecordMgr::GetNthBestSpeed", RecordMgr_GetNthBestSpeed, fp_get_nth)

// ---- merging a race into the file ------------------------------------------------------------------------------------
// A top-N table (N = 1 for the lap and speed bests, 10 for a race type's times), worst first. For a qualifying
// record the original walks j = 0..N-1: at the first used slot that beats it ("better"), it goes in just
// below (the worse ones below that shift down, the worst dropping out) -- nothing if that's slot 0; a slot that
// doesn't beat it is passed, and past the last one it goes in on top, everything shifting down. The shifts are
// the game's memmove (called even for 0 bytes, as the original does); the copy is rep movsd.
static inline uint8_t* lap_slot0(RecordMgr* m) { return (uint8_t*)m->file + 0x50 + mode_idx(m) * 82 * 0x48; }
static inline uint8_t* speed_slot0(RecordMgr* m) { return (uint8_t*)m->file + 8 + mode_idx(m) * 82 * 0x48; }
static inline uint8_t* race_slot0(RecordMgr* m) {
    return (uint8_t*)m->file + 0x98 + mode_idx(m) * 0x1710 + (uint32_t)m->race_type * 0x2d0;
}

// CheckLaps: every valid (flags & 0x11 == 0x11) lap with a positive time (the float's bits as an int) against
// the mode's best lap, then with a positive top speed against the best speed; the file's winners' `index`
// points back into recs, which get flag 2 (lap) and 4 (speed)
static void __fastcall RecordMgr_CheckLaps(RecordMgr* self, Edx, RaceRecord* recs, int n) {
    at<int16_t>(lap_slot0(self), 0x3e) = -1;
    at<int16_t>(speed_slot0(self), 0x3e) = -1;
    uint8_t* rec = (uint8_t*)recs;
    for (int32_t k = 0; k < n; k++, rec += 0x48) {
        if ((rec[0x40] & 0x11) == 0x11 && at<int32_t>(rec, 4) > 0) {
            at<int16_t>(rec, 0x3e) = (int16_t)k;
            for (uint32_t j = 0; j < 1; j++) {
                uint8_t* slot = lap_slot0(self) + j * 0x48;
                if (!(D(at<float>(slot, 4)) >= D(at<float>(rec, 4))) && (slot[0x40] & 1)) {
                    if (j != 0) {
                        g_memmove(lap_slot0(self), lap_slot0(self) + 0x48, j * 0x48 - 0x48);
                        movsd(lap_slot0(self) + (j - 1) * 0x48, rec, 0x12);
                        self->dirty = 1;
                    }
                    break;
                }
                if (j == 0) {                                          // the last slot (N - 1)
                    g_memmove(lap_slot0(self), lap_slot0(self) + 0x48, j * 0x48);
                    movsd(lap_slot0(self) + j * 0x48, rec, 0x12);
                    self->dirty = 1;
                }
            }
        }
        if ((rec[0x40] & 0x11) == 0x11 && at<int32_t>(rec, 8) > 0) {
            at<int16_t>(rec, 0x3e) = (int16_t)k;
            for (uint32_t j = 0; j < 1; j++) {
                uint8_t* slot = speed_slot0(self) + j * 0x48;
                if (D(at<float>(slot, 8)) > D(at<float>(rec, 8)) && (slot[0x40] & 1)) {
                    if (j != 0) {
                        g_memmove(speed_slot0(self), speed_slot0(self) + 0x48, j * 0x48 - 0x48);
                        movsd(speed_slot0(self) + (j - 1) * 0x48, rec, 0x12);
                        self->dirty = 1;
                    }
                    break;
                }
                if (j == 0) {
                    g_memmove(speed_slot0(self), speed_slot0(self) + 0x48, j * 0x48);
                    movsd(speed_slot0(self) + j * 0x48, rec, 0x12);
                    self->dirty = 1;
                }
            }
        }
    }
    for (uint32_t j = 0; j < 1; j++) {
        const int16_t v = at<int16_t>(lap_slot0(self) + j * 0x48, 0x3e);
        if (v != -1) ((uint8_t*)recs)[(int32_t)v * 0x48 + 0x40] |= 2;
    }
    for (uint32_t j = 0; j < 1; j++) {
        const int16_t v = at<int16_t>(speed_slot0(self) + j * 0x48, 0x3e);
        if (v != -1) ((uint8_t*)recs)[(int32_t)v * 0x48 + 0x40] |= 4;
    }
}
static void fp_check(Footprint& f, RecordMgr* self, Edx, RaceRecord* recs, int n) {
    if (!mode_in_file(self)) { f.replay_only = "a mode outside the records file"; return; }
    f.add(self, sizeof *self, "RecordMgr");
    f.add(self->file, sizeof(RecordFile), "RecordFile");
    if (n > 0) f.add(recs, (uint32_t)(n < 32768 ? n : 32768) * 0x48, "records");
}
PORT_FN(0x0042ae80, "RecordMgr::CheckLaps", RecordMgr_CheckLaps, fp_check)

// CheckRace: every valid race record with a positive race time into the mode's race type's top ten (flag 8)
static void __fastcall RecordMgr_CheckRace(RecordMgr* self, Edx, RaceRecord* recs, int n) {
    for (uint32_t j = 0; j < 10; j++) at<int16_t>(race_slot0(self) + j * 0x48, 0x3e) = -1;
    uint8_t* rec = (uint8_t*)recs;
    for (int32_t k = 0; n > k; k++, rec += 0x48) {
        if ((rec[0x40] & 0x11) != 0x11 || at<int32_t>(rec, 0xc) <= 0) continue;
        at<int16_t>(rec, 0x3e) = (int16_t)k;
        for (uint32_t j = 0; j < 10; j++) {
            uint8_t* slot = race_slot0(self) + j * 0x48;
            if (!(D(at<float>(slot, 0xc)) >= D(at<float>(rec, 0xc))) && (slot[0x40] & 1)) {
                if (j != 0) {
                    g_memmove(race_slot0(self), race_slot0(self) + 0x48, j * 0x48 - 0x48);
                    movsd(race_slot0(self) + (j - 1) * 0x48, rec, 0x12);
                    self->dirty = 1;
                }
                break;
            }
            if (j == 9) {
                g_memmove(race_slot0(self), race_slot0(self) + 0x48, j * 0x48);
                movsd(race_slot0(self) + j * 0x48, rec, 0x12);
                self->dirty = 1;
            }
        }
    }
    for (uint32_t j = 0; j < 10; j++) {
        const int16_t v = at<int16_t>(race_slot0(self) + j * 0x48, 0x3e);
        if (v != -1) ((uint8_t*)recs)[(int32_t)v * 0x48 + 0x40] |= 8;
    }
}
PORT_FN(0x0042b260, "RecordMgr::CheckRace", RecordMgr_CheckRace, fp_check)

// dump (no callers): the mode, then its used best speed, best lap and race-type slots, times as doubles
static void __fastcall RecordMgr_dump(RecordMgr* self, Edx) {
    LogReport(P<const char>(FMT_DUMP), self->realism, self->race_type);
    const float k = *P<float>(C_225);
    for (uint32_t j = 0; j < 1; j++) {
        const uint8_t* r = speed_slot0(self) + j * 0x48;
        if (r[0x40] & 1) {
            const double v = D(at<float>((void*)r, 8)) * D(k);        // fmul dword 2.25; fstp qword
            LogReport(P<const char>(FMT_DUMP_SPEED), j, r + 0x1d, r + 0x10, D(at<float>((void*)r, 4)), v);
        }
    }
    for (uint32_t j = 0; j < 1; j++) {
        const uint8_t* r = lap_slot0(self) + j * 0x48;
        if (r[0x40] & 1) {
            const double v = D(at<float>((void*)r, 8)) * D(k);
            LogReport(P<const char>(FMT_DUMP_LAP), j, r + 0x1d, r + 0x10, D(at<float>((void*)r, 4)), v);
        }
    }
    for (uint32_t j = 0; j < 10; j++) {
        const uint8_t* r = race_slot0(self) + j * 0x48;
        if (r[0x40] & 1) LogReport(P<const char>(FMT_DUMP_RACE), j, r + 0x1d, r + 0x10, D(at<float>((void*)r, 0xc)));
    }
}
static void fp_mgr_dump(Footprint&, RecordMgr*, Edx) {}
PORT_FN(0x0042b4e0, "RecordMgr::dump", RecordMgr_dump, fp_mgr_dump)
