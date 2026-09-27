// krn_core.cpp -- M3 stage "kernel and utilities", group K1: the kernel's core, rewritten. kernel:kernel.obj
// (KernelBegin / KernelEnd), mem.obj (the heap: MemAlloc / MemFree, operator new / delete), task.obj (tasks, threads,
// TaskSetTimer and timer_vector: the loop every timer thread runs, the physics' included), sync.obj (the Single* /
// Multi* module locks, mutexes on a pool of critical sections, strobes), bg.obj (the BG hook list: how physics_thread
// is called every 16 ms), except.obj (the crash handler and the FPU mode: ExceptDiv0Crashes, ExceptSinglePrecision),
// prof.obj (PTimeNow, the profiler, the QPC / rdtsc calibration), _prof.obj (its hand-written assembler: rdtsc,
// isCpuidSupported, isRdtscSupported, pr_*), time.obj, vxd.obj (the Win95 VxD that locked the exe in memory; on NT
// "\\.\vexed.vxd" never opens and every vxd* call is a no-op) and abend.obj.
//
// How the pieces fit (v1.0). KernelBegin: ProfBegin (the clock: rdtsc when the CPU has it AND there is exactly one
// processor, else QueryPerformanceCounter -- so on any modern machine PTimeNow is QPC in ms and the pr_* profiler
// hooks are no-ops), SyncBegin (the global mutex), TaskBegin (task 1 = the main thread; a 100 ms "test timer" thread
// must run within 6 s), FileBegin, LogBegin, BGBegin (the "BGTask" timer thread, 16 ms: bg_vector runs every BGHook'd
// function in priority order under the "BG" lock -- physics_thread at priority 0, the SoundManager at 1, empty_hook at
// 2), ExceptBegin (the unhandled-exception filter, c:\except.log, the link map appended to race.exe), MemBegin (a
// 20 MB growable Win32 heap), then Win32/Key/Scan/Mouse/Joy. timer_vector is each timer thread's body: register with
// the VxD, ExceptDiv0Crashes(1) -- overflow, divide-by-zero and denormal exceptions unmasked for the thread's whole
// life: docs/PORTING.md rule 10a -- THREAD_PRIORITY_TIME_CRITICAL, then until TaskKillTimer asks it to die: call the
// function, sleep what's left of the period (or Sleep(0); every 60th overrun Sleep(20)). physics_thread itself sets
// single precision every tick (ExceptSinglePrecision(1)).
//
// Faithful (docs/PORTING.md): every call in the original's order with the original's arguments -- game functions by
// their v1.0 address (the group's own too, so a hooked rewrite or the original is what runs), Windows through the SAME
// game import slot the original uses (`call dword ptr [0x5d74xx]`: the DLL and Wine may patch imports, and a harness
// stubs them), the game's statically linked CRT at its own addresses (sprintf, vsprintf, sscanf, strstr, stricmp,
// strnicmp, strncpy, qsort, memmove, _controlfp, _clearfp, atexit). Thread entry points, callbacks and handlers are
// passed as the ORIGINAL's addresses (0x414a60, 0x414ba0, 0x414920, 0x414950, 0x415680, 0x418990, 0x4189e0,
// 0x413aa0), strings as the game's own (a Multi/Single table keeps the name pointer). Globals are read and written
// through volatile at the points the original does (the other thread may be looking). Bugs and races are kept; the
// fix candidates are listed where they happen (// FIX CANDIDATE:) -- none is fixed here.
//
// The x87: only ProfBegin (the TSC calibration) and prof_report (the per-profile percentage) use it; both are asm
// sequences copied from the original, so they give its bits at whatever precision the thread runs.
//
// Threads: everything here can run on both the main thread and a timer thread (physics / BG). The lock, heap, thread,
// clock and device functions are replay_only (running one twice in a shadow check would take a lock twice, allocate
// twice, create two threads or read two different times); the rest list every static they write.
//
// Not a PORT_FN of its own: nothing is left out. The $E static initialisers are rewritten too (they run from the CRT's
// _initterm, after the DLL's hooks are in); the two whose body is a tail jump to rcfunc_is_internal (a bare `ret`) are
// kept as calls. The _prof.obj labels are data labels in the map, not functions: see the integration notes at the end.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"

typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

// The time stamp counter, as the hand-written assembler reads it inline. A harness (test/world_krn_core.cpp) defines
// KRN_RDTSC before including this file to feed the rewrite the same fake clock it feeds the original's rdtsc
// instructions; in the DLL it is the instruction.
#ifndef KRN_RDTSC
#define KRN_RDTSC() __rdtsc()
#endif

// ---- memory: the game's statics, read and written where the original does ----------------------------------------------
#define G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define G32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define GI32(a) (*(volatile int32_t*)(uintptr_t)(a))
#define G64(a) (*(volatile uint64_t*)(uintptr_t)(a))
#define S(a) ((const char*)(uintptr_t)(a))                 // one of the game's own strings
#define FN(T, a) ((T)(uintptr_t)(a))
// a Windows function, through the game's own import slot
#define IAT(fn, slot) (*(decltype(&::fn)*)(uintptr_t)(slot))

// prof.obj / _prof.obj
enum : uint32_t {
    P_FILTER = 0x4e6420,           // uint8: nonzero -> only names starting with this character are profiled
    P_SYS_BEGIN = 0x4e6424,        // void (*)(): pr_sys_begin (0x41883e) or noop_void
    P_SYS_END = 0x4e6428,          // pr_sys_end (0x41888f) or noop_void
    P_OV_BEGIN = 0x4e642c,         // pr_overhead_begin (0x418780) or noop_void -- called around every profiled block
    P_OV_END = 0x4e6430,           // pr_overhead_end (0x418798) or noop_void
    P_START = 0x4e6434,            // prof_start (0x413480) or noop_prof_start
    P_STOP = 0x4e6438,             // prof_stop (0x4134d0) or noop_prof_stop
    P_TICKS_MS = 0x4e643c,         // uint32: TSC ticks per ms (rdtsc path)
    P_HI_MS = 0x4e6440,            // uint32: 0xffffffff / ticks per ms: ms per 2^32 ticks
    P_TSC0 = 0x4e6448,             // uint64: the TSC at ProfBegin
    P_FLIP = 0x4e6450,             // uint8: timestr's buffer toggle
    P_QPC_MS = 0x508198,           // int32: QPC counts per ms (0: PTimeNow falls back to Win32GetTime)
    P_QPC0 = 0x5081a0,             // LARGE_INTEGER: QPC at ProfBegin
    P_RDTSC = 0x5081a8,            // uint8: the rdtsc path is in use
    P_STR_A = 0x5081b0,            // char[16]: timestr's two buffers
    P_NAMES = 0x5081c0,            // name_int_map: 16 names of 16 bytes, count at +0x100 (0x5082c0)
    P_NNAMES = 0x5082c0,
    P_STR_B = 0x5082d8,
    P_SYS = 0x4ea91c,              // uint64: TSC at pr_sys_begin, then the elapsed ticks after pr_sys_end
    P_OV_ACC = 0x4ea924,           // uint64: overhead ticks accumulated
    P_OV_T = 0x4ea92c,             // uint64: TSC at the last pr_overhead_begin
    P_PROFS = 0x4ea934,            // ProfEntry[16]
};
struct ProfEntry { uint64_t start, acc, ov_snap; uint32_t count, _1c; };       // 32 bytes
static_assert(sizeof(ProfEntry) == 32, "ProfEntry");
struct name_int_map { char names[16][16]; int32_t count; };
static_assert(offsetof(name_int_map, count) == 0x100, "name_int_map");

// mem.obj
enum : uint32_t { M_HEAP = 0x4e6588, M_SYNC = 0x4e658c, M_MODE = 0x4e6590, M_CUR = 0x50853c, M_MAX = 0x508544 };

// task.obj: 8 tasks of 0x1c bytes at 0x508668; task ids are 1-based (get_task_info(id) = &tasks[id - 1])
enum : uint32_t { T_SYNC = 0x4e6694, T_MAIN = 0x4e6698, T_TABLE = 0x508668, T_TABLE_END = 0x508748, T_TEST = 0x508748 };
struct task_info {
    const char* name;              // +0x00 0: a free slot
    uint32_t thread_id;            // +0x04 CreateThread's lpThreadId
    int32_t period;                // +0x08 a timer's period in ms (task_exception_handler passes it to timeKillEvent)
    HANDLE handle;                 // +0x0c
    int32_t parent;                // +0x10 the creating task
    int32_t _14;
    uint8_t suspend_req;           // +0x18 TaskSuspend -> TaskShouldISuspend
    uint8_t kill;                  // +0x19 TaskKill -> TaskShouldIDie
    uint8_t flags;                 // +0x1a 1 suspended, 2 dead
    uint8_t _1b;
};
static_assert(sizeof(task_info) == 0x1c && offsetof(task_info, suspend_req) == 0x18, "task_info");
typedef volatile task_info VTask;

// sync.obj: the unsafe (Single) and safe (Multi) module tables, 16 entries each; a pool of 16 critical sections
enum : uint32_t {
    Y_NSINGLE = 0x4e685c, Y_NMULTI = 0x4e6860, Y_MUTEX = 0x4e6864,
    Y_SINGLES = 0x508750,          // {name, owner task} x 16
    Y_MULTIS = 0x5087d0,           // {name, mutex} x 16
    Y_NCS = 0x508850, Y_CS = 0x508858,   // cs_tag x 16: a CRITICAL_SECTION (0x18) + in use (+0x18)
};
struct SyncEntry { const char* name; uint32_t v; };
typedef volatile SyncEntry VSync;
static __forceinline VSync* single_entry(int h) { return (VSync*)(uintptr_t)(Y_SINGLES + (uint32_t)h * 8u - 8u); }
static __forceinline VSync* multi_entry(int h) { return (VSync*)(uintptr_t)(Y_MULTIS + (uint32_t)h * 8u - 8u); }

// except.obj
enum : uint32_t {
    X_LOG = 0x4e6a5c,              // the except.log file (FileCreate), 0 if it didn't open
    X_MAP = 0x4e6a60, X_MAP_SIZE = 0x4e6a64,   // the link map appended to race.exe, and its length
    X_FILE = 0x4e6a68, X_MAPPING = 0x4e6a6c, X_VIEW = 0x4e6a70,
    X_ABENDED = 0x4e6a74,          // uint8: abend() crashed on purpose (LogPanic): the handler stays quiet in the log
    X_COUNT = 0x4e6a78,            // exceptions handled so far (after 4 the filter gives up)
    X_PREV = 0x508a18,             // the filter SetUnhandledExceptionFilter replaced
    X_BEST = 0x508a20, X_LINE = 0x508b20, X_NAME = 0x508c20,   // char[256] each
    X_HANDLERS = 0x508d20,         // void (*)() x 4: the exit handlers
};

// vxd.obj, kernel.obj, bg.obj
enum : uint32_t { V_HANDLE = 0x4ea7b4, V_NAME = 0x4db454, K_UP = 0x4ea850 };
enum : uint32_t { B_SYNC = 0x4eab34, B_TASK = 0x4eab38, B_N = 0x4eab3c, B_HOOKS = 0x508e50 };
struct BGHookEntry { void(__cdecl* fn)(); int32_t prio; };
typedef volatile BGHookEntry VHook;
static __forceinline VHook* bg_hook(int i) { return (VHook*)(uintptr_t)(B_HOOKS + (uint32_t)i * 8u); }

struct TimeOfDay { uint16_t year, month, day, hour, minute, second; uint32_t ms, dow; };
static_assert(sizeof(TimeOfDay) == 0x14, "TimeOfDay");

// ---- the functions they call, by address ---------------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
typedef uint8_t(__cdecl* Flag_t)();
typedef int(__cdecl* Int_t)();
typedef void(__cdecl* IntArg_t)(int);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef void(__cdecl* Lock3_t)(int, const char*, int);
typedef void(__cdecl* PtrArg_t)(void*);
#define LogReport FN(Log_t, 0x00411150)
#define LogError FN(Log_t, 0x004111d0)
#define LogPanic FN(Log_t, 0x004112b0)
#define Win32GetErrorString FN(const char*(__cdecl*)(), 0x00416070)
#define Win32GetTime FN(Int_t, 0x00412d70)
#define Win32GetAppInstance FN(uint8_t*(__cdecl*)(), 0x00412bc0)
#define FileWrite FN(uint8_t(__cdecl*)(int, const void*, int), 0x00411a30)
#define FileCreate FN(int(__cdecl*)(const char*), 0x004115f0)
#define FileClose FN(void(__cdecl*)(volatile uint32_t*), 0x00411850)
#define rcfunc_is_internal FN(Void_t, 0x00410cc0)
// prof.obj / _prof.obj
#define map_name FN(int(__fastcall*)(void*, Edx, const char*), 0x004133e0)
#define prof_report FN(Void_t, 0x004137a0)
#define timestr FN(const char*(__cdecl*)(uint32_t), 0x00413960)
#define sort_profs FN(void(__cdecl*)(int32_t*), 0x00413a70)
#define li_add FN(void(__cdecl*)(LARGE_INTEGER*, LARGE_INTEGER*), 0x00413530)
#define li_sub FN(void(__cdecl*)(LARGE_INTEGER*, LARGE_INTEGER*), 0x00413510)
#define li_div FN(int(__cdecl*)(LARGE_INTEGER*, int), 0x004134f0)
#define only_one_processor FN(Flag_t, 0x00413750)
#define pticks2msec FN(int(__cdecl*)(uint64_t), 0x00413bb0)
#define PTimeNow FN(Int_t, 0x00413b40)
#define pr_start FN(IntArg_t, 0x004187b7)
#define pr_end FN(IntArg_t, 0x004187f3)
#define pr_rdtsc_lo FN(uint32_t(__cdecl*)(), 0x00418839)
#define rdtsc_fn FN(uint64_t(__cdecl*)(), 0x004188ad)
#define isCpuidSupported FN(Int_t, 0x004188b0)
#define isRdtscSupported FN(Int_t, 0x004188d5)
static const uint32_t k_compare_f = 0x00413aa0;             // sort_profs' qsort comparator, passed as the original's
// mem.obj
#define MemBegin FN(uint8_t(__cdecl*)(uint32_t), 0x00414080)
#define MemEnd FN(Void_t, 0x004143a0)
#define MemFree FN(PtrArg_t, 0x00414300)
#define touch_memory FN(void(__cdecl*)(void*, int), 0x004141f0)
#define mem_use_vxd_allocator FN(Flag_t, 0x00414200)
// task.obj
#define get_task_info FN(VTask*(__cdecl*)(int), 0x00414870)
#define alloc_task FN(int(__cdecl*)(const char*, int), 0x00414890)
#define free_task FN(IntArg_t, 0x004149c0)
#define current_taskid FN(Int_t, 0x00414a80)
#define current_taskname FN(const char*(__cdecl*)(), 0x00414ce0)
#define TaskBegin FN(Void_t, 0x004147b0)
#define TaskEnd FN(Void_t, 0x00414960)
#define TaskDestroy FN(IntArg_t, 0x00414ac0)
#define TaskSetTimer FN(int(__cdecl*)(const char*, uint32_t, int), 0x00414b10)
#define TaskKillTimer FN(IntArg_t, 0x00414c60)
#define TaskSetPriority FN(IntArg_t, 0x00414cb0)
#define TaskKill FN(IntArg_t, 0x00414d20)
#define TaskResume FN(IntArg_t, 0x00414d40)
#define TaskShouldIDie FN(Flag_t, 0x00414db0)
#define TaskGetID FN(Int_t, 0x00414dd0)
#define TaskSleep FN(IntArg_t, 0x00414de0)
#define TaskIsSuspended FN(uint8_t(__cdecl*)(int), 0x00414e40)
#define TaskGetName FN(const char*(__cdecl*)(int), 0x00414ec0)
static const uint32_t k_thread_vector = 0x00414a60, k_timer_vector = 0x00414ba0;
static const uint32_t k_task_exception_handler = 0x00414920, k_timer_test_f = 0x00414950;
// sync.obj
#define SyncBegin FN(Flag_t, 0x00414ef0)
#define SyncEnd FN(Void_t, 0x00414f10)
#define unsafe_check FN(Lock3_t, 0x00415020)
#define MultiBegin FN(int(__cdecl*)(const char*), 0x004150a0)
#define MultiEnter FN(Lock3_t, 0x00415180)
#define MultiLeave FN(Lock3_t, 0x004151d0)
#define MultiEnd FN(Lock3_t, 0x00415220)
#define SyncMutexAlloc FN(void*(__cdecl*)(), 0x004152a0)
#define alloc_cs FN(uint8_t*(__cdecl*)(), 0x004152c0)
#define SyncMutexLock FN(PtrArg_t, 0x00415330)
#define SyncMutexUnlock FN(PtrArg_t, 0x00415340)
#define SyncMutexFree FN(void(__cdecl*)(volatile uint32_t*), 0x00415350)
#define cs_tag_ctor FN(void*(__fastcall*)(void*, Edx), 0x00415420)
// except.obj
#define DumpExceptionString FN(void(__cdecl*)(int, char*, const char*, ...), 0x00415450)
#define ExceptBegin FN(Void_t, 0x004154a0)
#define get_mapfile_ptr FN(char*(__cdecl*)(), 0x004154e0)
#define except2name FN(const char*(__cdecl*)(uint32_t), 0x00415850)
#define is_floating_exception FN(uint8_t(__cdecl*)(uint32_t), 0x00415a40)
#define find_func_in_mapfile FN(IntArg_t, 0x00415a60)
#define get_string FN(char*(__cdecl*)(char*, char*), 0x00415b60)
#define ExceptInstallExitHandler FN(void(__cdecl*)(uint32_t), 0x00415b90)
#define ExceptUninstallExitHandler FN(void(__cdecl*)(uint32_t), 0x00415bd0)
#define ExceptTrigger FN(Void_t, 0x00415bf0)
#define ExceptEnd FN(Void_t, 0x00415c20)
#define ExceptDiv0Crashes FN(void(__cdecl*)(uint8_t), 0x00415c60)
static const uint32_t k_my_handler = 0x00415680;
// vxd.obj
#define lockdown_exe FN(Void_t, 0x00418380)
#define lockdown_memory FN(uint8_t(__cdecl*)(void*, uint32_t), 0x00418400)
#define should_lockdown_section FN(uint8_t(__cdecl*)(const uint8_t*), 0x00418410)
#define vxdIsLoaded FN(Flag_t, 0x00418480)
#define vxdMemAlloc FN(void*(__cdecl*)(int), 0x00418490)
#define call_vxd FN(uint8_t(__cdecl*)(int, void*, int, void*, int), 0x004184e0)
#define vxdMemFree FN(PtrArg_t, 0x00418530)
#define vxdRegisterPhysicsThread FN(Void_t, 0x00418560)
#define vxdUnregisterPhysicsThread FN(Void_t, 0x00418590)
#define vxdGetPhysicsThreadEIP FN(Int_t, 0x004185c0)
// kernel.obj's callees in other objects
#define ProfBegin FN(Flag_t, 0x00413580)
#define ProfEnd FN(Void_t, 0x00413b30)
#define FileBegin FN(Void_t, 0x00411540)
#define FileEnd FN(Void_t, 0x004115d0)
#define FileVerifyNoOpenFiles FN(Void_t, 0x00411580)
#define LogBegin FN(Flag_t, 0x00410d10)
#define LogEnd FN(Void_t, 0x00411080)
#define Win32Begin FN(Void_t, 0x00412650)
#define Win32End FN(Void_t, 0x00412b50)
#define KeyBegin FN(Void_t, 0x00413bf0)
#define KeyEnd FN(Void_t, 0x00413c30)
#define ScanBegin FN(Void_t, 0x00412f00)
#define ScanEnd FN(Void_t, 0x00412ff0)
#define MouseBegin FN(Void_t, 0x00414430)
#define MouseEnd FN(Void_t, 0x00414450)
#define JoyBegin FN(Void_t, 0x00418bb0)
#define JoyEnd FN(Void_t, 0x00418f30)
// bg.obj
#define BGBegin FN(Void_t, 0x00418940)
#define BGEnd FN(Void_t, 0x00418b50)
#define ASSERT_MSG_bg FN(void(__cdecl*)(int, const char*, ...), 0x00418ab0)   // a bare `ret` in this build
static const uint32_t k_bg_vector = 0x00418990, k_empty_hook = 0x004189e0;
// the game's CRT
#define crt_strnicmp FN(int(__cdecl*)(const char*, const char*, uint32_t), 0x004da3e0)
#define crt_stricmp FN(int(__cdecl*)(const char*, const char*), 0x004da350)
#define crt_strncpy FN(char*(__cdecl*)(char*, const char*, uint32_t), 0x004cf3a0)
#define crt_sprintf FN(int(__cdecl*)(char*, const char*, ...), 0x004cf0a0)
#define crt_vsprintf FN(int(__cdecl*)(char*, const char*, va_list), 0x004cf7c0)
#define crt_sscanf FN(int(__cdecl*)(const char*, const char*, ...), 0x004ce190)
#define crt_strstr FN(char*(__cdecl*)(const char*, const char*), 0x004cf9a0)
#define crt_qsort FN(void(__cdecl*)(void*, uint32_t, uint32_t, uint32_t), 0x004cf160)
#define crt_memmove FN(void*(__cdecl*)(volatile void*, volatile void*, uint32_t), 0x004cf400)
#define crt_controlfp FN(uint32_t(__cdecl*)(uint32_t, uint32_t), 0x004cfa40)
#define crt_clearfp FN(uint32_t(__cdecl*)(), 0x004cf9e0)
#define crt_atexit FN(int(__cdecl*)(uint32_t), 0x004ceff0)

// the inlined strcpy / strlen (repne scasb; rep movsd; rep movsb): a forward byte copy of strlen + 1
static __forceinline void str_copy(char* dst, const char* src) { memcpy(dst, src, strlen(src) + 1); }
// rep stosd n/4; rep stosb n&3 -- the memset the originals inline
static __forceinline void fill_a3(uint8_t* p, uint32_t n) {
    __stosd((unsigned long*)p, 0xa3a3a3a3u, n >> 2);
    __stosb(p + (n & ~3u), 0xa3, n & 3);
}
// cdq; xor eax, edx; sub eax, edx
static __forceinline uint32_t abs32(int32_t v) { uint32_t s = (uint32_t)(v >> 31); return ((uint32_t)v ^ s) - s; }

// ====================================================================================================================
// prof.obj
// ====================================================================================================================

// name_int_map::map_name (thiscall): the index of `name` (first 15 characters, case-insensitively), added if new
static int __fastcall map_name_rw(name_int_map* self, Edx, const char* name) {
    volatile int32_t& count = self->count;
    int i = 0;
    if (count > 0) {
        const char* e = self->names[0];
        do {
            if (crt_strnicmp(name, e, 15) == 0) return i;
            e += 16;
            i++;
        } while (count > i);
    }
    int32_t n = count;
    if (n < 16) {
        crt_strncpy(self->names[n], name, 16);
        ((volatile char*)self->names[count])[15] = 0;
        int32_t r = count;
        count = r + 1;
        return r;
    }
    LogError(S(0x4e6454), name);                         // "Too many profiles! (%s will not be profiled)"
    return -1;
}
static void fp_map_name(Footprint& f, name_int_map* self, Edx, const char*) {
    f.add(self, sizeof(name_int_map), "name_int_map");
    if (self->count < 0) f.add(self->names[self->count], 16, "the new name (a negative count writes before the map)");
}
PORT_FN(0x004133e0, "name_int_map::map_name", map_name_rw, fp_map_name)

static int __cdecl prof_start_rw(const char* name) {
    if (G8(P_FILTER) != 0) {
        uint8_t c = G8(P_FILTER);
        if (*(const volatile uint8_t*)name != c) return -1;
    }
    int id = map_name((void*)(uintptr_t)P_NAMES, 0, name);
    if (id != -1) pr_start(id);
    return id;
}
static void fp_prof_start(Footprint& f, const char* name) {
    fp_map_name(f, (name_int_map*)(uintptr_t)P_NAMES, 0, name);
    f.add((void*)(uintptr_t)P_PROFS, 16 * sizeof(ProfEntry), "profiles");
}
PORT_FN(0x00413480, "prof_start", prof_start_rw, fp_prof_start)

static void __cdecl prof_stop_rw(int id) {
    if (id != -1) pr_end(id);
}
static void fp_prof_stop(Footprint& f, int id) {
    if (id != -1) f.add((void*)(uintptr_t)(P_PROFS + (uint32_t)id * 32u), sizeof(ProfEntry), "profile");
}
PORT_FN(0x004134d0, "prof_stop", prof_stop_rw, fp_prof_stop)

// idiv: faults on 0 and on a quotient past 32 bits, as the original
static int __cdecl li_div_rw(LARGE_INTEGER* a, int d) {
    int r;
    __asm { mov ecx, a
            mov eax, [ecx]
            mov edx, [ecx + 4]
            idiv d
            mov r, eax }
    return r;
}
// not fuzzed (a random divisor faults): checked by the harness; writes nothing
static void fp_li_div(Footprint&, LARGE_INTEGER*, int) {}
PORT_FN(0x004134f0, "li_div", li_div_rw, fp_li_div)

static void __cdecl li_sub_rw(LARGE_INTEGER* a, LARGE_INTEGER* b) {
    uint32_t lo = b->LowPart, hi = (uint32_t)b->HighPart;          // both read before either is written
    uint64_t v = ((uint64_t)(uint32_t)a->HighPart << 32 | a->LowPart) - ((uint64_t)hi << 32 | lo);
    a->LowPart = (uint32_t)v;
    a->HighPart = (LONG)(uint32_t)(v >> 32);
}
static void fp_li(Footprint& f, LARGE_INTEGER* a, LARGE_INTEGER*) { f.add(a, 8, "a"); f.pure = true; }
PORT_FN(0x00413510, "li_sub", li_sub_rw, fp_li)

static void __cdecl li_add_rw(LARGE_INTEGER* a, LARGE_INTEGER* b) {
    uint32_t lo = b->LowPart, hi = (uint32_t)b->HighPart;
    uint64_t v = ((uint64_t)(uint32_t)a->HighPart << 32 | a->LowPart) + ((uint64_t)hi << 32 | lo);
    a->LowPart = (uint32_t)v;
    a->HighPart = (LONG)(uint32_t)(v >> 32);
}
PORT_FN(0x00413530, "li_add", li_add_rw, fp_li)

static void __cdecl noop_void_rw() {}
static void fp_pure0(Footprint& f) { f.pure = true; }
PORT_FN(0x00413550, "noop_void", noop_void_rw, fp_pure0)

static int __cdecl noop_prof_start_rw(const char*) { return 0; }
static void fp_pure_str(Footprint& f, const char*) { f.pure = true; }
PORT_FN(0x00413560, "noop_prof_start", noop_prof_start_rw, fp_pure_str)

static void __cdecl noop_prof_stop_rw(int) {}
static void fp_pure_int(Footprint& f, int) { f.pure = true; }
PORT_FN(0x00413570, "noop_prof_stop", noop_prof_stop_rw, fp_pure_int)

// ProfBegin's TSC rate: fild qword over; fild qword freq; fdivp; fadd 1.0 (double); fild qword delta; fdivrp; __ftol
// -- ticks per second = delta / (1 + over / freq), in the thread's precision; __ftol keeps the low dword of the
// fistp qword (so a rate past 2^31 Hz comes out negative).
static __declspec(noinline) int32_t tsc_rate(uint32_t over, uint32_t freq, uint32_t delta) {
    static const double one = 1.0;                   // 0x4db448
    uint32_t q[2] = {0, 0};
    uint16_t cw, chop;
    int64_t r;
    __asm { mov eax, over
            mov q, eax
            fild qword ptr q
            mov eax, freq
            mov q, eax
            fild qword ptr q
            fdivp st(1), st
            fadd qword ptr one
            mov eax, delta
            mov q, eax
            fild qword ptr q
            fdivrp st(1), st
            fnstcw cw
            mov ax, cw
            or ah, 0x0c
            mov chop, ax
            fldcw chop
            fistp qword ptr r
            fldcw cw }
    return (int32_t)r;
}

// ProfBegin: pick the clock. rdtsc when the CPU has it and there is exactly one processor: calibrate the TSC against
// QPC over one second (busy-waiting), point the profiler hooks at the pr_* routines. Otherwise the hooks are no-ops and
// PTimeNow reads QPC (counts per ms from the frequency's LOW dword).
// FIX CANDIDATE: the calibration overflows on any single-processor machine (a 1-vCPU VM) with a TSC past 2.147 GHz:
// __ftol's low dword goes negative, ticks-per-ms goes negative and 0xffffffff / it (unsigned) is tiny, so PTimeNow --
// the physics timer's clock -- is garbage; the 32-bit TSC delta also wraps past 4.29 GHz, and a delta under 1000
// ticks makes the `div` a divide-by-zero crash.
static uint8_t __cdecl ProfBegin_rw() {
    uint8_t use = 1;
    G8(P_RDTSC) = 1;
    if (!(uint8_t)isRdtscSupported() || !only_one_processor()) use = 0;
    G8(P_RDTSC) = use;
    if (use) {
        uint64_t t = rdtsc_fn();
        G32(P_TSC0) = (uint32_t)t;
        LARGE_INTEGER freq, t0, now;
        G32(P_TSC0 + 4) = (uint32_t)(t >> 32);
        if (!IAT(QueryPerformanceFrequency, 0x5d74c4)(&freq)) return 0;
        BOOL(WINAPI * qpc)(LARGE_INTEGER*) = IAT(QueryPerformanceCounter, 0x5d7488);   // mov edi, [slot]: read once
        qpc(&t0);
        uint32_t first = t0.LowPart;
        do qpc(&t0); while (first == *(volatile DWORD*)&t0.LowPart);
        uint32_t tsc0 = pr_rdtsc_lo();
        li_add(&t0, &freq);
        do {
            qpc(&now);
            li_sub(&now, &t0);
        } while (*(volatile LONG*)&now.HighPart < 0);
        uint32_t tsc1 = pr_rdtsc_lo();
        int32_t hz = tsc_rate(now.LowPart, freq.LowPart, tsc1 - tsc0);
        uint32_t per_ms = (uint32_t)(hz / 1000);         // cdq; idiv 1000
        G32(P_TICKS_MS) = per_ms;
        G32(P_HI_MS) = 0xffffffffu / per_ms;             // div: faults on 0
        FN(Void_t, G32(P_SYS_BEGIN))();
        return 1;
    }
    G32(P_START) = 0x00413560;                           // noop_prof_start
    G32(P_SYS_END) = 0x00413550;                         // noop_void
    G32(P_SYS_BEGIN) = 0x00413550;
    G32(P_OV_BEGIN) = 0x00413550;
    LARGE_INTEGER freq;
    G32(P_STOP) = 0x00413570;                            // noop_prof_stop
    G32(P_OV_END) = 0x00413550;
    if (IAT(QueryPerformanceFrequency, 0x5d74c4)(&freq)) {
        G32(P_QPC_MS) = freq.LowPart / 1000u;
        IAT(QueryPerformanceCounter, 0x5d7488)((LARGE_INTEGER*)(uintptr_t)P_QPC0);
        return 1;
    }
    G32(P_QPC_MS) = 0;
    return 1;
}
static void fp_prof_begin(Footprint& f) { f.replay_only = "reads the clocks (QPC, rdtsc) and busy-waits a second"; }
PORT_FN(0x00413580, "ProfBegin", ProfBegin_rw, fp_prof_begin)

static uint8_t __cdecl only_one_processor_rw() {
    SYSTEM_INFO si;
    IAT(GetSystemInfo, 0x5d745c)(&si);
    return si.dwNumberOfProcessors - 1u < 1u;
}
static void fp_none(Footprint&) {}
PORT_FN(0x00413750, "only_one_processor", only_one_processor_rw, fp_none)

static void __cdecl ProfReset_rw() {
    FN(Void_t, G32(P_SYS_END))();
    FN(Void_t, G32(P_SYS_BEGIN))();
}
static void fp_reads_tsc(Footprint& f) { f.replay_only = "reads the time stamp counter"; }
PORT_FN(0x00413770, "ProfReset", ProfReset_rw, fp_reads_tsc)

static void __cdecl ProfResetAndReport_rw() {
    FN(Void_t, G32(P_SYS_END))();
    prof_report();
    FN(Void_t, G32(P_SYS_BEGIN))();
}
PORT_FN(0x00413780, "ProfResetAndReport", ProfResetAndReport_rw, fp_reads_tsc)

// prof_report's percentage: fild qword ms; fmul dword 100.0f; fdiv dword total (a float); fstp qword
static __forceinline double prof_pct(uint32_t ms, const float* total) {
    static const uint32_t k100 = 0x42c80000;         // 100.0f (0x4db450)
    uint32_t q[2] = {ms, 0};
    double r;
    __asm { fild qword ptr q
            fmul dword ptr k100
            mov eax, total
            fdiv dword ptr [eax]
            fstp r }
    return r;
}
// fild qword (the total, zero-extended); fstp dword
static __forceinline float u32_to_float(uint32_t v) {
    uint32_t q[2] = {v, 0};
    float r;
    __asm { fild qword ptr q
            fstp r }
    return r;
}
// ticks -> ms, as the report inlines it: hi * (ms per 2^32 ticks) + lo / ticks per ms (div: faults when 0)
static __forceinline uint32_t ticks_ms(uint32_t lo, uint32_t hi) { return hi * G32(P_HI_MS) + lo / G32(P_TICKS_MS); }

// the profile report (DoRace, ReplayBenchmark: ProfResetAndReport). Nothing registered -> nothing (so the QPC path,
// whose ticks-per-ms is 0, never divides by it).
static void __cdecl prof_report_rw() {
    int32_t count = GI32(P_NNAMES);
    if (count == 0) return;
    int32_t order[16];
    uint32_t total = ticks_ms(G32(P_SYS), G32(P_SYS + 4));
    sort_profs(order);
    LogReport(S(0x4e6484));                              // "==== Profile Report ===="
    uint32_t per_ms = G32(P_TICKS_MS);
    LogReport(S(0x4e64d4), per_ms / 1000u, per_ms % 1000u);   // "Estimated machine speed: %d.%d MHz"
    if (count > 0) {
        float total_f = u32_to_float(total);
        const int32_t* o = order;
        int32_t left = count;
        do {
            int32_t id = *o;
            volatile ProfEntry* e = (volatile ProfEntry*)(uintptr_t)(P_PROFS + (uint32_t)id * 32u);
            uint32_t ms = (uint32_t)(e->acc >> 32) * G32(P_HI_MS) + (uint32_t)e->acc / G32(P_TICKS_MS);
            const char* avg = S(0x4e64f8);               // ""
            if (e->count != 0) avg = timestr(ms / e->count);
            const char* name = id >= 0 && GI32(P_NNAMES) > id ? S(P_NAMES + (uint32_t)id * 16u) : 0;
            o++;
            uint32_t n = e->count;
            double pct = prof_pct(ms, &total_f);
            const char* tot = timestr(ms);
            LogReport(S(0x4e64fc), name, tot, n, pct, avg);   // " %-16s : %9sN: %10d %4.1fp  a: %-14s"
        } while (--left != 0);
    }
    uint32_t ov = G32(P_HI_MS) * G32(P_OV_ACC + 4) + G32(P_OV_ACC) / G32(P_TICKS_MS);
    uint32_t pct = ov * 100u / total;                    // div: faults when total is 0
    LogReport(S(0x4e6524), timestr(ov), pct);            // " (prof overhead  : %s : %d%%)"
}
static void fp_timestr_bufs(Footprint& f) {
    f.add((void*)(uintptr_t)P_FLIP, 1, "timestr toggle");
    f.add((void*)(uintptr_t)P_STR_A, 16, "timestr buffer A");
    f.add((void*)(uintptr_t)P_STR_B, 16, "timestr buffer B");
}
PORT_FN(0x004137a0, "prof_report", prof_report_rw, fp_timestr_bufs)

// "1h 02m 03.456s" into one of two 16-byte buffers, alternately.
// Quirk kept: with hours, minutes and seconds all present the seconds end at byte 15 and the trailing ' ' overwrites
// the terminator -- the string runs on into whatever follows the buffer (the profile names, for buffer A).
static const char* __cdecl timestr_rw(uint32_t ms) {
    uint8_t flip = G8(P_FLIP) ^ 1;
    G8(P_FLIP) = flip;
    char* buf = (char*)(uintptr_t)(flip != 0 ? P_STR_B : P_STR_A);
    volatile uint32_t* w = (volatile uint32_t*)buf;
    w[0] = 0x20202020u;
    w[1] = 0x20202020u;
    w[2] = 0x20202020u;
    w[3] = 0x20202020u;
    ((volatile char*)buf)[15] = 0;
    if (ms == 0) {
        str_copy(buf, S(0x4e6544));                     // "<no time>"
        ((volatile char*)buf)[9] = ' ';
        return buf;
    }
    uint32_t h = ms / 3600000u;
    uint32_t m = ms % 3600000u / 60000u;
    uint32_t s = ms % 60000u / 1000u;
    uint32_t r = ms % 1000u;
    char* p = buf;
    if (h != 0) {
        p = buf + 4;
        crt_sprintf(buf, S(0x4e6550), h);               // "%2.2dh "
    }
    if (m != 0) {
        crt_sprintf(p, S(0x4e6558), m);                 // "%2.2dm "
        p += 4;
    }
    if (s != 0 || r != 0) {
        char* at = p;
        p += 7;
        crt_sprintf(at, S(0x4e6560), s, r);             // "%2.2d.%3.3ds"
    }
    *(volatile char*)p = ' ';
    return buf;
}
static void fp_timestr(Footprint& f, uint32_t) { fp_timestr_bufs(f); }
PORT_FN(0x00413960, "timestr", timestr_rw, fp_timestr)

static void __cdecl sort_profs_rw(int32_t* order) {
    int32_t n = GI32(P_NNAMES);
    for (int32_t i = 0; i < n; i++) order[i] = i;
    crt_qsort(order, (uint32_t)n, 4, k_compare_f);
}
static void fp_sort_profs(Footprint& f, int32_t* order) {
    int32_t n = GI32(P_NNAMES);
    if (n > 0) f.add(order, (uint32_t)n * 4u, "order");
}
PORT_FN(0x00413a70, "sort_profs", sort_profs_rw, fp_sort_profs)

// the inlined strcmp: -1 / 0 / 1 by the first differing byte (unsigned); an id out of range compares a null pointer
static __forceinline int inline_strcmp(const volatile uint8_t* a, const volatile uint8_t* b) {
    for (;;) {
        uint8_t c = a[0];
        if (c != b[0]) return c < b[0] ? -1 : 1;
        if (c == 0) return 0;
        c = a[1];
        if (c != b[1]) return c < b[1] ? -1 : 1;
        a += 2;
        b += 2;
        if (c == 0) return 0;
    }
}
static int __cdecl compare_f_rw(const void* pa, const void* pb) {
    int32_t ia = *(const int32_t*)pa;
    const volatile uint8_t* a = ia >= 0 && GI32(P_NNAMES) > ia ? (const volatile uint8_t*)(uintptr_t)(P_NAMES + (uint32_t)ia * 16u) : 0;
    int32_t ib = *(const int32_t*)pb;
    const volatile uint8_t* b = ib >= 0 && GI32(P_NNAMES) > ib ? (const volatile uint8_t*)(uintptr_t)(P_NAMES + (uint32_t)ib * 16u) : 0;
    return inline_strcmp(a, b);
}
static void fp_compare_f(Footprint&, const void*, const void*) {}
PORT_FN(0x00413aa0, "compare_f", compare_f_rw, fp_compare_f)

static void __cdecl ProfEnd_rw() { FN(Void_t, G32(P_SYS_END))(); }
PORT_FN(0x00413b30, "ProfEnd", ProfEnd_rw, fp_reads_tsc)

// the kernel's clock, in ms (every timer thread's pacing, the physics clock's source)
static int __cdecl PTimeNow_rw() {
    if (G8(P_RDTSC) == 0) {
        if (G32(P_QPC_MS) == 0) return Win32GetTime();
        LARGE_INTEGER now;
        IAT(QueryPerformanceCounter, 0x5d7488)(&now);
        li_sub(&now, (LARGE_INTEGER*)(uintptr_t)P_QPC0);
        return li_div(&now, GI32(P_QPC_MS));
    }
    uint64_t t = rdtsc_fn();
    t -= G64(P_TSC0);
    return pticks2msec(t);
}
static void fp_reads_clock(Footprint& f) { f.replay_only = "reads the clock"; }
PORT_FN(0x00413b40, "PTimeNow", PTimeNow_rw, fp_reads_clock)

static int __cdecl pticks2msec_rw(uint64_t t) {
    return (int)((uint32_t)(t >> 32) * G32(P_HI_MS) + (uint32_t)t / G32(P_TICKS_MS));
}
static void fp_pticks2msec(Footprint&, uint64_t) {}
PORT_FN(0x00413bb0, "pticks2msec", pticks2msec_rw, fp_pticks2msec)

// ---- prof.obj's static initialisers ------------------------------------------------------------------------------------
static void fp_static_init(Footprint& f) { f.replay_only = "a static initialiser (runs once, from the CRT's _initterm)"; }
static void __cdecl prof_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00413390, "$E1(prof.obj)", prof_E1_rw, fp_static_init)
static void __cdecl prof_E2_rw() { FN(Void_t, 0x00413390)(); }
PORT_FN(0x00413380, "$E2(prof.obj)", prof_E2_rw, fp_static_init)
static void __cdecl prof_E4_rw() { G32(P_NNAMES) = 0; }      // the name map's constructor
PORT_FN(0x004133b0, "$E4(prof.obj)", prof_E4_rw, fp_static_init)
static void __cdecl prof_E6_rw() { crt_atexit(0x004133d0); }  // its destructor, at exit
PORT_FN(0x004133c0, "$E6(prof.obj)", prof_E6_rw, fp_static_init)
static void __cdecl prof_E5_rw() { G32(P_NNAMES) = 0; }
PORT_FN(0x004133d0, "$E5(prof.obj)", prof_E5_rw, fp_static_init)
static void __cdecl prof_E7_rw() {
    FN(Void_t, 0x004133b0)();
    FN(Void_t, 0x004133c0)();
}
PORT_FN(0x004133a0, "$E7(prof.obj)", prof_E7_rw, fp_static_init)

// ====================================================================================================================
// _prof.obj: the hand-written assembler (data labels in the map). The originals keep every register but their result;
// their callers are compiled C, which assumes nothing of eax/ecx/edx, so plain C functions are equivalent.
// ====================================================================================================================

// the TSC at the start of a profiled block's bookkeeping (`sub eax, 0; sbb edx, 0`: an overhead correction left at 0)
static void __cdecl pr_overhead_begin_rw() { G64(P_OV_T) = KRN_RDTSC(); }
PORT_FN(0x00418780, "pr_overhead_begin", pr_overhead_begin_rw, fp_reads_tsc)

static void __cdecl pr_overhead_end_rw() {
    uint64_t t = KRN_RDTSC() - G64(P_OV_T);
    G64(P_OV_ACC) += t;
}
PORT_FN(0x00418798, "pr_overhead_end", pr_overhead_end_rw, fp_reads_tsc)

static void __cdecl pr_start_rw(int id) {
    volatile ProfEntry* e = (volatile ProfEntry*)(uintptr_t)(P_PROFS + ((uint32_t)id << 5));
    e->ov_snap = G64(P_OV_ACC);
    e->start = G64(P_OV_T);
    e->count = ~e->count;
}
static void fp_pr_entry(Footprint& f, int id) { f.add((void*)(uintptr_t)(P_PROFS + ((uint32_t)id << 5)), sizeof(ProfEntry), "profile"); }
PORT_FN(0x004187b7, "pr_start", pr_start_rw, fp_pr_entry)

// the block's ticks, less the profiler's own overhead since pr_start
static void __cdecl pr_end_rw(int id) {
    volatile ProfEntry* e = (volatile ProfEntry*)(uintptr_t)(P_PROFS + ((uint32_t)id << 5));
    uint64_t t = G64(P_OV_T) + e->ov_snap - G64(P_OV_ACC) - e->start;
    e->acc += t;
    e->count = ~e->count + 1;
}
PORT_FN(0x004187f3, "pr_end", pr_end_rw, fp_pr_entry)

static uint32_t __cdecl pr_rdtsc_lo_rw() { return (uint32_t)KRN_RDTSC(); }
PORT_FN(0x00418839, "pr_rdtsc_lo", pr_rdtsc_lo_rw, fp_reads_tsc)

static void __cdecl pr_sys_begin_rw() {
    __stosd((unsigned long*)(uintptr_t)P_PROFS, 0, 0x80);
    G32(P_OV_ACC) = 0;
    G32(P_OV_ACC + 4) = 0;
    G32(P_OV_T) = 0;
    G32(P_OV_T + 4) = 0;
    G64(P_SYS) = KRN_RDTSC();
}
PORT_FN(0x0041883e, "pr_sys_begin", pr_sys_begin_rw, fp_reads_tsc)

static void __cdecl pr_sys_end_rw() { G64(P_SYS) = KRN_RDTSC() - G64(P_SYS); }
PORT_FN(0x0041888f, "pr_sys_end", pr_sys_end_rw, fp_reads_tsc)

// INTEGRATION: the original is 3 bytes (rdtsc; ret) with isCpuidSupported at 0x4188b0 -- a 5-byte jump here would
// overwrite that function's first two bytes. It needs a 2-byte short jump (EB 63) into the int3 padding at 0x418912
// (14 bytes), and the 5-byte jump to the rewrite there; the harness hooks it that way.
// In the game it stays original (rdtsc; ret is the instruction itself): only the harness, which can hook it, registers it.
static uint64_t __cdecl rdtsc_rw() { return KRN_RDTSC(); }
#ifdef VP_FAITHFUL
#define HARNESS_ONLY_FN PORT_FN      // (a name tools/gen_port_tables.py doesn't pick up)
HARNESS_ONLY_FN(0x004188ad, "rdtsc", rdtsc_rw, fp_reads_tsc)
#endif

// can EFLAGS.ID (bit 21) be toggled? (restores the flags)
static int __cdecl isCpuidSupported_rw() {
    int r;
    __asm { pushfd
            pushfd
            pop eax
            mov ecx, eax
            xor eax, 0x200000
            push eax
            popfd
            pushfd
            pop eax
            popfd
            xor edx, edx
            cmp ecx, eax
            je same
            inc edx
          same:
            mov r, edx }
    return r;
}
PORT_FN(0x004188b0, "isCpuidSupported", isCpuidSupported_rw, fp_pure0)

static int __cdecl isRdtscSupported_rw() {
    if (isCpuidSupported() == 0) return 0;
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 1) return 0;
    __cpuid(regs, 1);
    if (!((uint32_t)regs[3] & 0x10u)) return 0;
    return 1;
}
PORT_FN(0x004188d5, "isRdtscSupported", isRdtscSupported_rw, fp_pure0)

// ====================================================================================================================
// mem.obj
// ====================================================================================================================
static void __cdecl mem_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00414050, "$E1(mem.obj)", mem_E1_rw, fp_static_init)
static void __cdecl mem_E2_rw() { FN(Void_t, 0x00414050)(); }
PORT_FN(0x00414040, "$E2(mem.obj)", mem_E2_rw, fp_static_init)

static void __cdecl do_nothing_with_rw(int) {}
PORT_FN(0x00414060, "do_nothing_with", do_nothing_with_rw, fp_pure_int)

static void __cdecl MemSetAllocMode_rw(uint8_t mode) { G8(M_MODE) = mode; }
static void fp_mem_set_alloc_mode(Footprint& f, uint8_t) { f.add((void*)(uintptr_t)M_MODE, 1, "alloc mode (0x4e6590)"); }
PORT_FN(0x00414070, "MemSetAllocMode", MemSetAllocMode_rw, fp_mem_set_alloc_mode)

static uint8_t __cdecl MemBegin_rw(uint32_t size) {
    HANDLE h = IAT(HeapCreate, 0x5d7494)(HEAP_NO_SERIALIZE, size, 0);
    G32(M_HEAP) = (uint32_t)(uintptr_t)h;
    if (!h) return 0;
    G32(M_SYNC) = (uint32_t)MultiBegin(S(0x4e6594));   // "Mem"
    G32(M_MAX) = 0;
    G32(M_CUR) = 0;
    if (vxdIsLoaded()) LogReport(S(0x4e6598));          // "MemBegin: using VXD allocator."
    return 1;
}
static void fp_mem_begin(Footprint& f, uint32_t) { f.replay_only = "creates the heap"; }
PORT_FN(0x00414080, "MemBegin", MemBegin_rw, fp_mem_begin)

// A block is a 4-byte size (negative: from the VxD) then the caller's bytes, filled with 0xa3.
// Quirks kept: the current / peak counters are updated outside the lock (both threads allocate); a failed HeapAlloc
// is logged and then the fill writes through the null pointer (a crash).
static void* __cdecl MemAlloc_rw(int n) {
    int32_t size = n + 4;
    int32_t peak = GI32(M_MAX);
    GI32(M_CUR) += size;
    if (GI32(M_CUR) > peak) G32(M_MAX) = G32(M_CUR);
    uint8_t from_vxd = 0;
    uint8_t* p = 0;
    MultiEnter((int)G32(M_SYNC), 0, 0);
    if (mem_use_vxd_allocator()) {
        p = (uint8_t*)vxdMemAlloc(size);
        from_vxd = p != 0;
        if (!p) LogReport(S(0x4e65b8), size / 1024);   // "MemAlloc: cannot alloc %dK from vxd, using heap."
    }
    if (!from_vxd) {
        p = (uint8_t*)IAT(HeapAlloc, 0x5d7498)((HANDLE)(uintptr_t)G32(M_HEAP), HEAP_NO_SERIALIZE, (SIZE_T)(uint32_t)size);
        if (!p) {
            const char* err = Win32GetErrorString();
            LogReport(S(0x4e65ec), size, err);          // "HeapAlloc fails!  Cannot allocate %d bytes! (%s)"
        }
    } else {
        size = -size;
    }
    MultiLeave((int)G32(M_SYNC), 0, 0);
    uint32_t bytes = abs32(size);
    fill_a3(p, bytes);
    *(volatile int32_t*)p = size;
    touch_memory(p + 4, (int)bytes);
    return p + 4;
}
static void fp_allocates(Footprint& f, int) { f.replay_only = "allocates"; }
PORT_FN(0x004140e0, "MemAlloc", MemAlloc_rw, fp_allocates)

static void __cdecl touch_memory_rw(void*, int) {}
static void fp_pure_pi(Footprint& f, void*, int) { f.pure = true; }
PORT_FN(0x004141f0, "touch_memory", touch_memory_rw, fp_pure_pi)

static uint8_t __cdecl mem_use_vxd_allocator_rw() {
    if (vxdIsLoaded() && G8(M_MODE) != 0) return 1;
    return 0;
}
PORT_FN(0x00414200, "mem_use_vxd_allocator", mem_use_vxd_allocator_rw, fp_none)

static void* __cdecl operator_new_rw(uint32_t size) {
    LogPanic(S(0x4e6620), size);                       // "new called with no name! (size == %d"
    return 0;
}
static void fp_operator_new(Footprint& f, uint32_t) { f.replay_only = "panics (LogPanic doesn't return in the game)"; }
PORT_FN(0x00414220, "operator new", operator_new_rw, fp_operator_new)

static void __cdecl MemAttrib_rw(void*, int) {}
PORT_FN(0x00414240, "MemAttrib", MemAttrib_rw, fp_pure_pi)

static void __cdecl MemVerifyAll_rw() {
    MultiEnter((int)G32(M_SYNC), 0, 0);
    IAT(HeapValidate, 0x5d749c)((HANDLE)(uintptr_t)G32(M_HEAP), HEAP_NO_SERIALIZE, 0);
    MultiLeave((int)G32(M_SYNC), 0, 0);
}
static void fp_locks(Footprint& f) { f.replay_only = "takes a lock"; }
PORT_FN(0x00414250, "MemVerifyAll", MemVerifyAll_rw, fp_locks)

// Quirk kept: validates the caller's pointer, not the block's start 4 bytes before it (HeapValidate says no)
static void __cdecl MemVerify_rw(void* p) {
    MultiEnter((int)G32(M_SYNC), 0, 0);
    IAT(HeapValidate, 0x5d749c)((HANDLE)(uintptr_t)G32(M_HEAP), HEAP_NO_SERIALIZE, p);
    MultiLeave((int)G32(M_SYNC), 0, 0);
}
static void fp_locks_p(Footprint& f, void*) { f.replay_only = "takes a lock"; }
PORT_FN(0x00414290, "MemVerify", MemVerify_rw, fp_locks_p)

static void __cdecl MemDump_rw(int set) {
    if (set & 1) LogReport(S(0x4e6648), GI32(M_MAX) / 1024);   // "Max alloc: %dK"
}
static void fp_mem_dump(Footprint&, int) {}
PORT_FN(0x004142d0, "MemDump", MemDump_rw, fp_mem_dump)

// Quirks kept: no null check (MemFree(0) reads address -4); the whole block, header included, is filled with 0xa3
// before it's freed; the counter is updated outside the lock.
static void __cdecl MemFree_rw(void* q) {
    uint8_t* p = (uint8_t*)q - 4;
    int32_t size = *(volatile int32_t*)p;
    uint32_t bytes = abs32(size);
    uint8_t from_vxd = size < 0;
    fill_a3(p, bytes);
    G32(M_CUR) -= bytes;
    if (from_vxd) {
        vxdMemFree(p);
        return;
    }
    MultiEnter((int)G32(M_SYNC), 0, 0);
    IAT(HeapFree, 0x5d74a0)((HANDLE)(uintptr_t)G32(M_HEAP), HEAP_NO_SERIALIZE, p);
    MultiLeave((int)G32(M_SYNC), 0, 0);
}
static void fp_frees(Footprint& f, void*) { f.replay_only = "frees"; }
PORT_FN(0x00414300, "MemFree", MemFree_rw, fp_frees)

static void __cdecl operator_delete_rw(void* p) { MemFree(p); }
PORT_FN(0x00414390, "operator delete", operator_delete_rw, fp_frees)

static void __cdecl MemEnd_rw() {
    IAT(HeapDestroy, 0x5d74a4)((HANDLE)(uintptr_t)G32(M_HEAP));
    int sync = (int)G32(M_SYNC);
    G32(M_HEAP) = 0xffffffffu;
    MultiEnd(sync, 0, 0);
    G32(M_SYNC) = 0;
    LogReport(S(0x4e6658), GI32(M_MAX) / 1024);          // "Max alloc: %dK"
    if (GI32(M_CUR) > 0) LogPanic(S(0x4e6668), GI32(M_CUR));   // "Memory still allocated: %d bytes"
}
static void fp_mem_end(Footprint& f) { f.replay_only = "destroys the heap"; }
PORT_FN(0x004143a0, "MemEnd", MemEnd_rw, fp_mem_end)

// ====================================================================================================================
// task.obj
// ====================================================================================================================
static void __cdecl task_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x004147a0, "$E1(task.obj)", task_E1_rw, fp_static_init)
static void __cdecl task_E2_rw() { FN(Void_t, 0x004147a0)(); }
PORT_FN(0x00414790, "$E2(task.obj)", task_E2_rw, fp_static_init)

// Task 1 is the main thread; then a 100 ms timer must run once within 30 x 200 ms.
static void __cdecl TaskBegin_rw() {
    G32(T_SYNC) = (uint32_t)MultiBegin(S(0x4e66bc));   // "task"
    int main_task = alloc_task(S(0x4e66c4), 0);         // "<main>"
    G32(T_MAIN) = (uint32_t)main_task;
    VTask* ti = get_task_info(main_task);
    DWORD tid = IAT(GetCurrentThreadId, 0x5d7474)();
    ti->thread_id = tid;
    ti->handle = 0;
    ExceptInstallExitHandler(k_task_exception_handler);
    int tries = 0;
    G8(T_TEST) = 0;
    int timer = TaskSetTimer(S(0x4e66cc), k_timer_test_f, 100);   // "test timer"
    while (G8(T_TEST) == 0) {
        tries++;
        TaskSleep(200);
        if (tries >= 30) {
            if (G8(T_TEST) == 0) LogPanic(S(0x4e66d8));   // "Tried to create MMTimer, did not run"
            break;
        }
    }
    TaskKillTimer(timer);
    G8(T_TEST) = 0;
}
static void fp_threads(Footprint& f) { f.replay_only = "creates or ends threads"; }
PORT_FN(0x004147b0, "TaskBegin", TaskBegin_rw, fp_threads)

static VTask* __cdecl get_task_info_rw(int id) { return (VTask*)(uintptr_t)((uint32_t)(id - 1) * 0x1cu + T_TABLE); }
PORT_FN(0x00414870, "get_task_info", get_task_info_rw, fp_pure_int)

static int __cdecl alloc_task_rw(const char* name, int parent) {
    int i = 0;
    VTask* t = (VTask*)(uintptr_t)T_TABLE;
    for (;;) {
        if (t->name == 0) break;
        t++;
        i++;
        if (!((uintptr_t)t < T_TABLE_END)) {
            LogPanic(S(0x4e670c), name, S(0x4e6700));   // "Couldn't alloc task %s--increase %s:TASK_N", "task.cpp"
            return 0;
        }
    }
    VTask* e = (VTask*)(uintptr_t)(T_TABLE + (uint32_t)i * 0x1cu);
    e->name = name;
    e->parent = parent;
    e->period = 0;
    e->thread_id = 0;
    e->handle = 0;
    e->_14 = 0;
    e->suspend_req = 0;
    e->kill = 0;
    e->flags = 0;
    return i + 1;
}
static void fp_task_table(Footprint& f) { f.add((void*)(uintptr_t)T_TABLE, 8 * sizeof(task_info), "task table"); }
static void fp_alloc_task(Footprint& f, const char*, int) { fp_task_table(f); }
PORT_FN(0x00414890, "alloc_task", alloc_task_rw, fp_alloc_task)

// the exit handler TaskBegin installs (runs from the crash handler)
// Quirk kept: it passes each task's +8 -- a timer's PERIOD in this version -- to timeKillEvent, a leftover from when
// timers were multimedia timers: it kills whatever multimedia timer happens to have that id (normally none).
static void __cdecl task_exception_handler_rw() {
    decltype(&::timeKillEvent) kill = IAT(timeKillEvent, 0x5d76dc);   // read once (mov esi, [slot])
    for (VTask* t = (VTask*)(uintptr_t)T_TABLE; (uintptr_t)&t->period < 0x508750u; t++) {
        int32_t p = t->period;
        if (p != 0) kill((UINT)p);
    }
}
static void fp_kills_timers(Footprint& f) { f.replay_only = "calls timeKillEvent"; }
PORT_FN(0x00414920, "task_exception_handler", task_exception_handler_rw, fp_kills_timers)

static void __cdecl timer_test_f_rw() { G8(T_TEST) = 1; }
static void fp_timer_test_f(Footprint& f) { f.add((void*)(uintptr_t)T_TEST, 1, "test timer ran (0x508748)"); }
PORT_FN(0x00414950, "timer_test_f", timer_test_f_rw, fp_timer_test_f)

static void __cdecl TaskEnd_rw() {
    for (VTask* t = (VTask*)(uintptr_t)(T_TABLE + 0x1c); (uintptr_t)t < T_TABLE_END; t++) {   // tasks 2..8
        const char* n = t->name;
        if (n) LogPanic(S(0x4e6738), n);               // "Thread %s was never destroyed"
    }
    free_task((int)G32(T_MAIN));
    G32(T_MAIN) = 0;
    ExceptUninstallExitHandler(k_task_exception_handler);
    MultiEnd((int)G32(T_SYNC), 0, 0);
    G32(T_SYNC) = 0;
}
PORT_FN(0x00414960, "TaskEnd", TaskEnd_rw, fp_threads)

static void __cdecl free_task_rw(int id) {
    VTask* t = get_task_info(id);
    t->name = 0;
    t->handle = 0;
    t->thread_id = 0;
    t->period = 0;
}
static void fp_free_task(Footprint& f, int id) { f.add((void*)(uintptr_t)((uint32_t)(id - 1) * 0x1cu + T_TABLE), 0x10, "task"); }
PORT_FN(0x004149c0, "free_task", free_task_rw, fp_free_task)

// FIX CANDIDATE (TaskCreate and TaskSetTimer): the new thread can run before this thread has stored its thread id
// (CreateThread writes lpThreadId after the thread exists) and, for a timer, its period (stored after CreateThread
// returns): its first current_taskid then panics "Current thread is unlisted", or timer_vector reads a period of 0 and
// runs its function back to back. timer_vector takes no lock. Rare: the new thread first runs every DLL's
// DLL_THREAD_ATTACH under the loader lock.
static int __cdecl TaskCreate_rw(const char* name, uint32_t fn) {
    MultiEnter((int)G32(T_SYNC), 0, 0);
    int parent = current_taskid();
    int id = alloc_task(name, parent);
    VTask* t = get_task_info(id);
    HANDLE h = IAT(CreateThread, 0x5d74a8)(0, 0, (LPTHREAD_START_ROUTINE)(uintptr_t)k_thread_vector, (void*)(uintptr_t)fn, 0,
                                           (DWORD*)&t->thread_id);
    t->handle = h;
    if (!h) LogReport(S(0x4e6758), Win32GetErrorString());   // "CreateThread fails! (%s)"
    MultiLeave((int)G32(T_SYNC), 0, 0);
    return id;
}
static void fp_task_create(Footprint& f, const char*, uint32_t) { f.replay_only = "creates a thread"; }
PORT_FN(0x004149e0, "TaskCreate", TaskCreate_rw, fp_task_create)

// every TaskCreate thread's body
static DWORD __stdcall thread_vector_rw(void* fn) {
    FN(Void_t, fn)();
    get_task_info(current_taskid())->flags |= 2;
    return 0;
}
static void fp_thread_body(Footprint& f, void*) { f.replay_only = "a thread's body"; }
PORT_FN(0x00414a60, "thread_vector", thread_vector_rw, fp_thread_body)

static int __cdecl current_taskid_rw() {
    DWORD tid = IAT(GetCurrentThreadId, 0x5d7474)();
    int i = 0;
    for (VTask* t = (VTask*)(uintptr_t)T_TABLE; (uintptr_t)&t->thread_id < 0x50874cu; t++, i++)
        if (t->thread_id == tid) return i + 1;
    LogPanic(S(0x4e6774));                             // "Current thread is unlisted (this is impossible)"
    return -1;
}
PORT_FN(0x00414a80, "current_taskid", current_taskid_rw, fp_none)

static void __cdecl TaskDestroy_rw(int id) {
    VTask* t = get_task_info(id);
    TaskKill(id);
    if (TaskIsSuspended(id)) TaskResume(id);
    IAT(WaitForSingleObject, 0x5d7480)(t->handle, INFINITE);
    free_task(id);
}
static void fp_threads_i(Footprint& f, int) { f.replay_only = "waits for or changes a thread"; }
PORT_FN(0x00414ac0, "TaskDestroy", TaskDestroy_rw, fp_threads_i)

static int __cdecl TaskSetTimer_rw(const char* name, uint32_t fn, int period) {
    MultiEnter((int)G32(T_SYNC), 0, 0);
    int parent = current_taskid();
    int id = alloc_task(name, parent);
    VTask* t = get_task_info(id);
    HANDLE h = IAT(CreateThread, 0x5d74a8)(0, 0, (LPTHREAD_START_ROUTINE)(uintptr_t)k_timer_vector, (void*)(uintptr_t)fn, 0,
                                           (DWORD*)&t->thread_id);
    t->handle = h;
    if (!h) LogReport(S(0x4e67a4), Win32GetErrorString());   // "CreateThread fails! (%s)"
    t->period = period;
    MultiLeave((int)G32(T_SYNC), 0, 0);
    return id;
}
static void fp_task_set_timer(Footprint& f, const char*, uint32_t, int) { f.replay_only = "creates a thread"; }
PORT_FN(0x00414b10, "TaskSetTimer", TaskSetTimer_rw, fp_task_set_timer)

// Every timer thread's body -- the physics' too (BGTask -> bg_vector -> physics_thread). The period is read once, at
// the start. Quirk kept: the overrun counter is never reset by an on-time tick, so "every 60th overrun" yields 20 ms.
static DWORD __stdcall timer_vector_rw(void* fn) {
    vxdRegisterPhysicsThread();
    ExceptDiv0Crashes(1);
    TaskSetPriority(15);                                 // THREAD_PRIORITY_TIME_CRITICAL
    int overruns = 0, yielded = 0;
    int32_t period = get_task_info(current_taskid())->period;
    if (!TaskShouldIDie()) {
        do {
            int t0 = PTimeNow();
            FN(Void_t, fn)();
            int dt = PTimeNow() - t0;
            if (dt < period) {
                TaskSleep(period - dt);
            } else if (++overruns >= 60) {
                overruns = 0;
                yielded++;
                TaskSleep(20);
            } else {
                TaskSleep(0);
            }
        } while (!TaskShouldIDie());
    }
    vxdUnregisterPhysicsThread();
    get_task_info(current_taskid())->flags |= 2;
    if (yielded > 0) LogReport(S(0x4e67c0), yielded);   // "Timer: bg lockout was egregious: (yielded %d)"
    return 0;
}
PORT_FN(0x00414ba0, "timer_vector", timer_vector_rw, fp_thread_body)

// Quirk kept: holds the task lock while it waits (INFINITE) for the timer thread to end
static void __cdecl TaskKillTimer_rw(int id) {
    MultiEnter((int)G32(T_SYNC), 0, 0);
    get_task_info(id)->period = 0;
    TaskDestroy(id);
    MultiLeave((int)G32(T_SYNC), 0, 0);
}
PORT_FN(0x00414c60, "TaskKillTimer", TaskKillTimer_rw, fp_threads_i)

static void __cdecl TaskSetPriority_rw(int prio) {
    if (!IAT(SetThreadPriority, 0x5d74ac)(IAT(GetCurrentThread, 0x5d74b0)(), prio))
        LogPanic(S(0x4e67f0), current_taskname(), prio);   // "Attempted to set priority of \"%s\" to %d"
}
PORT_FN(0x00414cb0, "TaskSetPriority", TaskSetPriority_rw, fp_threads_i)

static const char* __cdecl current_taskname_rw() { return get_task_info(current_taskid())->name; }
PORT_FN(0x00414ce0, "current_taskname", current_taskname_rw, fp_none)

static void __cdecl TaskSuspend_rw(int id) {
    VTask* t = get_task_info(id);
    if (!(t->flags & 1)) t->suspend_req = 1;
}
static void fp_task_suspend(Footprint& f, int id) { f.add((void*)(uintptr_t)((uint32_t)(id - 1) * 0x1cu + T_TABLE + 0x18), 1, "suspend request"); }
PORT_FN(0x00414d00, "TaskSuspend", TaskSuspend_rw, fp_task_suspend)

static void __cdecl TaskKill_rw(int id) { get_task_info(id)->kill = 1; }
static void fp_task_kill(Footprint& f, int id) { f.add((void*)(uintptr_t)((uint32_t)(id - 1) * 0x1cu + T_TABLE + 0x19), 1, "kill request"); }
PORT_FN(0x00414d20, "TaskKill", TaskKill_rw, fp_task_kill)

static void __cdecl TaskResume_rw(int id) {
    VTask* t = get_task_info(id);
    t->flags &= 0xfe;
    DWORD r = IAT(ResumeThread, 0x5d74b4)(t->handle);
    if (r != 1) LogPanic(S(0x4e683c), t->name, r == 0 ? S(0x4e6818) : S(0x4e6828));   // "TaskResume: Task \"%s\" was %s"
}
PORT_FN(0x00414d40, "TaskResume", TaskResume_rw, fp_threads_i)

static uint8_t __cdecl TaskShouldISuspend_rw() { return get_task_info(current_taskid())->suspend_req; }
PORT_FN(0x00414d90, "TaskShouldISuspend", TaskShouldISuspend_rw, fp_none)

static uint8_t __cdecl TaskShouldIDie_rw() { return get_task_info(current_taskid())->kill; }
PORT_FN(0x00414db0, "TaskShouldIDie", TaskShouldIDie_rw, fp_none)

static int __cdecl TaskGetID_rw() { return current_taskid(); }
PORT_FN(0x00414dd0, "TaskGetID", TaskGetID_rw, fp_none)

static void __cdecl TaskSleep_rw(int ms) { IAT(Sleep, 0x5d74b8)((DWORD)ms); }
static void fp_sleeps(Footprint& f, int) { f.replay_only = "sleeps"; }
PORT_FN(0x00414de0, "TaskSleep", TaskSleep_rw, fp_sleeps)

static void __cdecl TaskSuspendMe_rw() {
    VTask* t = get_task_info(current_taskid());
    t->flags |= 1;
    t->suspend_req = 0;
    IAT(SuspendThread, 0x5d74bc)(t->handle);
}
PORT_FN(0x00414df0, "TaskSuspendMe", TaskSuspendMe_rw, fp_threads)

static void __cdecl TaskKillMe_rw() {
    get_task_info(current_taskid())->flags |= 2;
    IAT(ExitThread, 0x5d74c0)(0);
}
PORT_FN(0x00414e20, "TaskKillMe", TaskKillMe_rw, fp_threads)

static uint8_t __cdecl TaskIsSuspended_rw(int id) { return get_task_info(id)->flags & 1; }
static void fp_none_i(Footprint&, int) {}
PORT_FN(0x00414e40, "TaskIsSuspended", TaskIsSuspended_rw, fp_none_i)

static uint8_t __cdecl TaskIsDead_rw(int id) {
    VTask* t = get_task_info(id);
    if (t->flags & 2) return 1;
    DWORD code;
    if (IAT(GetExitCodeThread, 0x5d7574)(t->handle, &code)) {
        if (code == STILL_ACTIVE) return 0;
        t->flags |= 2;
        return 1;
    }
    LogPanic(S(0x4e669c), t->name);                    // "GetExitCodeThread(%s) fails."
    return 1;
}
static void fp_task_is_dead(Footprint& f, int id) { f.add((void*)(uintptr_t)((uint32_t)(id - 1) * 0x1cu + T_TABLE + 0x1a), 1, "task flags"); }
PORT_FN(0x00414e60, "TaskIsDead", TaskIsDead_rw, fp_task_is_dead)

static const char* __cdecl TaskGetName_rw(int id) { return get_task_info(id)->name; }
PORT_FN(0x00414ec0, "TaskGetName", TaskGetName_rw, fp_none_i)

// ====================================================================================================================
// sync.obj
// ====================================================================================================================
static void __cdecl sync_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00414ee0, "$E1(sync.obj)", sync_E1_rw, fp_static_init)
static void __cdecl sync_E2_rw() { FN(Void_t, 0x00414ee0)(); }
PORT_FN(0x00414ed0, "$E2(sync.obj)", sync_E2_rw, fp_static_init)

static uint8_t __cdecl SyncBegin_rw() {
    void* m = SyncMutexAlloc();
    G32(Y_MUTEX) = (uint32_t)(uintptr_t)m;
    return m != 0;
}
static void fp_mutex(Footprint& f) { f.replay_only = "creates or deletes a critical section"; }
PORT_FN(0x00414ef0, "SyncBegin", SyncBegin_rw, fp_mutex)

static void __cdecl SyncEnd_rw() {
    SyncMutexFree((volatile uint32_t*)(uintptr_t)Y_MUTEX);
    G32(Y_MUTEX) = 0;
}
PORT_FN(0x00414f10, "SyncEnd", SyncEnd_rw, fp_mutex)

// An "unsafe" module: owned by the task that began it; _SingleEnter / _SingleLeave only check the caller is the owner.
// Quirk kept: with the table full and no free slot the index is -1 and the entry written is the 8 bytes before the
// table (0x508748: the test timer's flag and 0x50874c).
static int __cdecl SingleBegin_rw(const char* name) {
    SyncMutexLock((void*)(uintptr_t)G32(Y_MUTEX));
    int32_t n = GI32(Y_NSINGLE);
    int i = 0;
    if (n > 0) {
        do {
            if (single_entry(i + 1)->v != 0) {
                int c = crt_stricmp(name, single_entry(i + 1)->name);
                n = GI32(Y_NSINGLE);
                if (c == 0) {
                    LogPanic(S(0x4e689c), name);       // "Unsafe module \"%s\" attempted to begin twice."
                    n = GI32(Y_NSINGLE);
                }
            }
            i++;
        } while (n > i);
    }
    int slot;
    if (n < 16) {
        slot = n;
        GI32(Y_NSINGLE) = n + 1;
    } else {
        slot = 0;
        bool found = false;
        if (n > 0) {
            do {
                if (single_entry(slot + 1)->v == 0) { found = true; break; }
                slot++;
            } while (slot < n);
        }
        if (!found) {
            slot = -1;
            LogPanic(S(0x4e6880));                     // "Not enough UNSAFE slots!"
        }
    }
    VSync* e = single_entry(slot + 1);
    e->v = (uint32_t)TaskGetID();
    void* m = (void*)(uintptr_t)G32(Y_MUTEX);
    e->name = name;
    SyncMutexUnlock(m);
    return slot + 1;
}
static void fp_locks_s(Footprint& f, const char*) { f.replay_only = "takes the global lock"; }
PORT_FN(0x00414f30, "SingleBegin", SingleBegin_rw, fp_locks_s)

static void __cdecl SingleEnter_rw(int h, const char* file, int line) { unsafe_check(h, file, line); }
static void fp_lock3_none(Footprint&, int, const char*, int) {}
PORT_FN(0x00415000, "_SingleEnter", SingleEnter_rw, fp_lock3_none)

static void __cdecl unsafe_check_rw(int h, const char*, int) {
    VSync* e = single_entry(h);
    if ((uint32_t)TaskGetID() != e->v) {
        const char* owner = TaskGetName((int)e->v);
        const char* module = e->name;
        const char* me = TaskGetName(TaskGetID());
        LogPanic(S(0x4e68cc), me, module, owner);      // "\"%s\" called module %s owned by \"%s\""
    }
}
PORT_FN(0x00415020, "unsafe_check", unsafe_check_rw, fp_lock3_none)

static void __cdecl SingleLeave_rw(int h, const char* file, int line) { unsafe_check(h, file, line); }
PORT_FN(0x00415070, "_SingleLeave", SingleLeave_rw, fp_lock3_none)

static void __cdecl SingleEnd_rw(int h, const char*, int) { single_entry(h)->v = 0; }
static void fp_single_end(Footprint& f, int h, const char*, int) { f.add((void*)&single_entry(h)->v, 4, "unsafe module's owner"); }
PORT_FN(0x00415090, "_SingleEnd", SingleEnd_rw, fp_single_end)

// A "safe" module: a critical section of its own.
static int __cdecl MultiBegin_rw(const char* name) {
    SyncMutexLock((void*)(uintptr_t)G32(Y_MUTEX));
    int32_t n = GI32(Y_NMULTI);
    int i = 0;
    if (n > 0) {
        do {
            if (multi_entry(i + 1)->v != 0) {
                int c = crt_stricmp(name, multi_entry(i + 1)->name);
                n = GI32(Y_NMULTI);
                if (c == 0) {
                    LogPanic(S(0x4e68f0), name);       // "Safe module \"%s\" attempted to begin twice."
                    n = GI32(Y_NMULTI);
                }
            }
            i++;
        } while (i < n);
    }
    int slot;
    if (n < 16) {
        slot = n;
        GI32(Y_NMULTI) = n + 1;
    } else {
        slot = 0;
        bool found = false;
        if (n > 0) {
            do {
                if (multi_entry(slot + 1)->v == 0) { found = true; break; }
                slot++;
            } while (slot < n);
        }
        if (!found) {
            slot = -1;
            LogPanic(S(0x4e6868));                     // "Not enough SAFE slots!"
        }
    }
    VSync* e = multi_entry(slot + 1);
    void* m = SyncMutexAlloc();
    e->v = (uint32_t)(uintptr_t)m;
    if (!m) LogPanic(S(0x4e691c), name);               // "Can't alloc mutex for safe module %s"
    void* g = (void*)(uintptr_t)G32(Y_MUTEX);
    e->name = name;
    SyncMutexUnlock(g);
    return slot + 1;
}
PORT_FN(0x004150a0, "MultiBegin", MultiBegin_rw, fp_locks_s)

static void __cdecl MultiEnter_rw(int h, const char*, int) {
    VSync* e = multi_entry(h);
    if (e->v == 0) {
        const char* module = e->name;
        LogPanic(S(0x4e6944), TaskGetName(TaskGetID()), module);   // "Task \"%s\" called module \"%s\" after module ended."
    }
    SyncMutexLock((void*)(uintptr_t)e->v);
}
static void fp_lock3(Footprint& f, int, const char*, int) { f.replay_only = "takes or releases a lock"; }
PORT_FN(0x00415180, "_MultiEnter", MultiEnter_rw, fp_lock3)

static void __cdecl MultiLeave_rw(int h, const char*, int) {
    VSync* e = multi_entry(h);
    if (e->v == 0) {
        const char* module = e->name;
        LogPanic(S(0x4e6978), TaskGetName(TaskGetID()), module);   // "Task \"%s\" called module \"%s\" after module end."
    }
    SyncMutexUnlock((void*)(uintptr_t)e->v);
}
PORT_FN(0x004151d0, "_MultiLeave", MultiLeave_rw, fp_lock3)

static void __cdecl MultiEnd_rw(int h, const char*, int) {
    volatile uint32_t* m = (volatile uint32_t*)(uintptr_t)((uint32_t)h * 8u + 0x5087ccu);
    if (*m == 0) {
        const char* module = multi_entry(h)->name;
        LogPanic(S(0x4e69a8), TaskGetName(TaskGetID()), module);   // "... stopped module \"%s\", which had already stopped."
    }
    SyncMutexFree(m);
    *m = 0;
}
PORT_FN(0x00415220, "_MultiEnd", MultiEnd_rw, fp_lock3)

// the 16 cs_tags' constructors (a static initialiser)
static void __cdecl sync_E4_rw() {
    uint32_t at = Y_CS;
    int k = 15;
    do {
        cs_tag_ctor((void*)(uintptr_t)at, 0);
        at += 0x1c;
    } while (--k >= 0);
}
PORT_FN(0x00415280, "$E4(sync.obj)", sync_E4_rw, fp_static_init)
static void __cdecl sync_E5_rw() { FN(Void_t, 0x00415280)(); }
PORT_FN(0x00415270, "$E5(sync.obj)", sync_E5_rw, fp_static_init)

static void* __cdecl SyncMutexAlloc_rw() {
    uint8_t* cs = alloc_cs();
    if (cs) IAT(InitializeCriticalSection, 0x5d7578)((CRITICAL_SECTION*)cs);
    return cs;
}
static void fp_mutex_p(Footprint& f) { f.replay_only = "initialises a critical section"; }
PORT_FN(0x004152a0, "SyncMutexAlloc", SyncMutexAlloc_rw, fp_mutex_p)

// Not under any lock (the global mutex itself comes from here); both threads can call MultiBegin.
static uint8_t* __cdecl alloc_cs_rw() {
    int32_t n = GI32(Y_NCS);
    if (n < 16) {
        uint32_t off = (uint32_t)n * 0x1cu;
        GI32(Y_NCS) = n + 1;
        G8(Y_CS + off + 0x18) = 1;
        return (uint8_t*)(uintptr_t)(Y_CS + off);
    }
    int i = 0;
    if (n > 0) {
        do {
            if (G8(Y_CS + (uint32_t)i * 0x1cu + 0x18) == 0) {
                uint32_t off = (uint32_t)i * 0x1cu;
                G8(Y_CS + off + 0x18) = 1;
                return (uint8_t*)(uintptr_t)(Y_CS + off);
            }
            i++;
        } while (i < n);
    }
    LogPanic(S(0x4e69e4));                             // "Out of Pre-allocated CS's, increase sync.cpp:MAX_CS"
    return 0;
}
static void fp_alloc_cs(Footprint& f) { f.add((void*)(uintptr_t)Y_NCS, Y_CS + 16 * 0x1c - Y_NCS, "critical section pool"); }
PORT_FN(0x004152c0, "alloc_cs", alloc_cs_rw, fp_alloc_cs)

static void __cdecl SyncMutexLock_rw(void* m) { IAT(EnterCriticalSection, 0x5d74cc)((CRITICAL_SECTION*)m); }
static void fp_lock_p(Footprint& f, void*) { f.replay_only = "takes or releases a lock"; }
PORT_FN(0x00415330, "SyncMutexLock", SyncMutexLock_rw, fp_lock_p)

static void __cdecl SyncMutexUnlock_rw(void* m) { IAT(LeaveCriticalSection, 0x5d74d0)((CRITICAL_SECTION*)m); }
PORT_FN(0x00415340, "SyncMutexUnlock", SyncMutexUnlock_rw, fp_lock_p)

static void __cdecl SyncMutexFree_rw(volatile uint32_t* m) {
    IAT(DeleteCriticalSection, 0x5d74d4)((CRITICAL_SECTION*)(uintptr_t)*m);
    *(volatile uint8_t*)(uintptr_t)(*m + 0x18) = 0;
    *m = 0;
}
static void fp_mutex_free(Footprint& f, volatile uint32_t*) { f.replay_only = "deletes a critical section"; }
PORT_FN(0x00415350, "SyncMutexFree", SyncMutexFree_rw, fp_mutex_free)

static HANDLE __cdecl SyncStrobeAlloc_rw() { return IAT(CreateEventA, 0x5d7564)(0, FALSE, FALSE, 0); }
static void fp_event(Footprint& f) { f.replay_only = "creates an event"; }
PORT_FN(0x00415370, "SyncStrobeAlloc", SyncStrobeAlloc_rw, fp_event)

// the main thread waiting for the physics' packet (shmem_block)
static void __cdecl SyncStrobeWait_rw(HANDLE h) {
    if (IAT(WaitForSingleObject, 0x5d7480)(h, 15000) == WAIT_TIMEOUT)
        LogPanic(S(0x4e6a18), 15, vxdGetPhysicsThreadEIP());   // "Physics has not dropped off a packet in %d seconds..."
}
static void fp_event_h(Footprint& f, HANDLE) { f.replay_only = "waits for or signals an event"; }
PORT_FN(0x00415380, "SyncStrobeWait", SyncStrobeWait_rw, fp_event_h)

// FIX CANDIDATE (minor): PulseEvent is documented as unreliable -- a waiter taken off its wait by a kernel APC misses
// the pulse; the wait above then lasts until the next packet (or 15 s and a panic if none comes).
static void __cdecl SyncStrobeFlash_rw(HANDLE h) { IAT(PulseEvent, 0x5d7568)(h); }
PORT_FN(0x004153b0, "SyncStrobeFlash", SyncStrobeFlash_rw, fp_event_h)

static void __cdecl SyncStrobeFree_rw(volatile uint32_t* h) {
    IAT(CloseHandle, 0x5d7548)((HANDLE)(uintptr_t)*h);
    *h = 0xffffffffu;
}
static void fp_event_free(Footprint& f, volatile uint32_t*) { f.replay_only = "closes an event"; }
PORT_FN(0x004153c0, "SyncStrobeFree", SyncStrobeFree_rw, fp_event_free)

// a spin lock: xchg 1 in until it was 0, Sleep(10) between tries (Sleep's slot read once)
static void __cdecl SyncLameMutexLock_rw(volatile long* p) {
    decltype(&::Sleep) sleep = IAT(Sleep, 0x5d74b8);
    while (_InterlockedExchange(p, 1) == 1) sleep(10);
}
static void fp_lame_lock(Footprint& f, volatile long*) { f.replay_only = "takes a lock"; }
PORT_FN(0x004153e0, "SyncLameMutexLock", SyncLameMutexLock_rw, fp_lame_lock)

static void __cdecl SyncLameMutexUnlock_rw(volatile long* p) { *p = 0; }
static void fp_lame_unlock(Footprint& f, volatile long* p) { f.add((void*)p, 4, "lock"); }
PORT_FN(0x00415410, "SyncLameMutexUnlock", SyncLameMutexUnlock_rw, fp_lame_unlock)

static void* __fastcall cs_tag_rw(void* self, Edx) {
    ((volatile uint8_t*)self)[0x18] = 0;
    return self;
}
// not `pure`: it returns `this`, which the fuzzer's two arenas make differ (checked by the harness instead)
static void fp_cs_tag(Footprint& f, void* self, Edx) { f.add(self, 0x1c, "cs_tag"); }
PORT_FN(0x00415420, "cs_tag::cs_tag", cs_tag_rw, fp_cs_tag)

// ====================================================================================================================
// except.obj
// ====================================================================================================================
static void __cdecl except_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00415440, "$E1(except.obj)", except_E1_rw, fp_static_init)
static void __cdecl except_E2_rw() { FN(Void_t, 0x00415440)(); }
PORT_FN(0x00415430, "$E2(except.obj)", except_E2_rw, fp_static_init)

// DumpExceptionString(file, buf, fmt, ...): vsprintf, then the line and a "\n" to the file. Variadic: its varargs are
// the stack slots after fmt, declared here as four dwords so a shadow wrapper passes them on (its callers pass at most
// two); the rewrite hands vsprintf the address of the first, exactly the original's va_list.
static void __cdecl DumpExceptionString_rw(int file, char* buf, const char* fmt, uint32_t a0, uint32_t, uint32_t, uint32_t) {
    crt_vsprintf(buf, fmt, (va_list)&a0);
    FileWrite(file, buf, (int)strlen(buf));
    FileWrite(file, S(0x4e6a7c), 1);                   // "\n"
}
static void fp_dump_exception_string(Footprint& f, int, char*, const char*, uint32_t, uint32_t, uint32_t, uint32_t) {
    f.replay_only = "writes the crash log";
}
PORT_FN(0x00415450, "DumpExceptionString", DumpExceptionString_rw, fp_dump_exception_string)

// FIX CANDIDATE: the crash log is c:\except.log -- the root of C:, which a normal user can't write on NT 6+, so
// FileCreate fails and the handler writes no log at all (the map-file trace goes only to the game's log).
static void __cdecl ExceptBegin_rw() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev =
        IAT(SetUnhandledExceptionFilter, 0x5d7570)((LPTOP_LEVEL_EXCEPTION_FILTER)(uintptr_t)k_my_handler);
    G32(X_PREV) = (uint32_t)(uintptr_t)prev;
    ExceptDiv0Crashes(0);
    G32(X_LOG) = (uint32_t)FileCreate(S(0x4e6a80));    // "c:\\except.log"
    G32(X_MAP) = (uint32_t)(uintptr_t)get_mapfile_ptr();
}
static void fp_except_begin(Footprint& f) { f.replay_only = "installs the exception filter, opens files"; }
PORT_FN(0x004154a0, "ExceptBegin", ExceptBegin_rw, fp_except_begin)

// The link map the build appended to race.exe: map the exe and return what follows its last section.
// Quirk kept: CreateFileA's failure (INVALID_HANDLE_VALUE) isn't caught -- only 0 is -- so a failed open carries on
// with -1 (then CreateFileMapping fails on it and "Couldn't create file mapping" is logged).
static char* __cdecl get_mapfile_ptr_rw() {
    char path[260];
    path[0] = (char)G8(0x4e6a90);                        // ""
    memset(path + 1, 0, sizeof path - 1);
    if (!IAT(GetModuleFileNameA, 0x5d74e4)(0, path, 260)) {
        LogReport(S(0x4e6a94));                          // "Can't get module file name"
        return 0;
    }
    G32(X_VIEW) = 0;
    HANDLE file = IAT(CreateFileA, 0x5d7554)(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    G32(X_FILE) = (uint32_t)(uintptr_t)file;
    if (file == 0) {
        LogReport(S(0x4e6b04), path);                    // "Couldn't open exe: %s"
        return 0;
    }
    DWORD size = IAT(GetFileSize, 0x5d7544)(file, 0);
    HANDLE mapping = IAT(CreateFileMappingA, 0x5d7468)((HANDLE)(uintptr_t)G32(X_FILE), 0, PAGE_READONLY, 0, 0, S(0x4e6ab0));
    G32(X_MAPPING) = (uint32_t)(uintptr_t)mapping;
    decltype(&::CloseHandle) close;
    if (mapping == 0) {
        LogReport(S(0x4e6ae4));                          // "Couldn't create file mapping"
        close = IAT(CloseHandle, 0x5d7548);
    } else {
        uint8_t* view = (uint8_t*)IAT(MapViewOfFile, 0x5d7464)(mapping, FILE_MAP_READ, 0, 0, 0);
        G32(X_VIEW) = (uint32_t)(uintptr_t)view;
        if (view != 0) {
            int32_t lfanew = *(volatile int32_t*)(view + 0x3c);
            uint8_t* fh = (uint8_t*)(uintptr_t)(lfanew + G32(X_VIEW) + 4);   // IMAGE_FILE_HEADER
            uint16_t nsec = *(volatile uint16_t*)(fh + 2);
            uint8_t* sec = fh + 0xf4;                    // the section table (a 0xe0-byte optional header assumed)
            int32_t end = 0;
            for (uint32_t k = nsec; k != 0; k--, sec += 0x28) {
                int32_t e = *(volatile int32_t*)(sec + 0x14) + *(volatile int32_t*)(sec + 0x10);   // raw pointer + size
                if (e > end) end = e;
            }
            int32_t rest = (int32_t)size - end;
            G32(X_MAP_SIZE) = (uint32_t)rest;
            if (rest != 0) return (char*)(uintptr_t)(G32(X_VIEW) + (uint32_t)end);
            LogReport(S(0x4e6ab8));                      // "No mapfile present"
            IAT(UnmapViewOfFile, 0x5d746c)((void*)(uintptr_t)G32(X_VIEW));
            G32(X_VIEW) = 0;
        } else {
            LogReport(S(0x4e6acc));                      // "Can't map view of file"
        }
        close = IAT(CloseHandle, 0x5d7548);
        close((HANDLE)(uintptr_t)G32(X_MAPPING));
    }
    close((HANDLE)(uintptr_t)G32(X_FILE));
    return 0;
}
static void fp_opens_files(Footprint& f) { f.replay_only = "opens and maps files"; }
PORT_FN(0x004154e0, "get_mapfile_ptr", get_mapfile_ptr_rw, fp_opens_files)

// The unhandled-exception filter: log the exception, dump it and a guess at the call stack (return addresses in
// 0x400000..0x480000 just after an E8 call whose target is near the previous frame's) to except.log with the map's
// names, run the exit handlers, ExitProcess(1). After four exceptions it declines (returns 0).
static LONG __stdcall my_handler_rw(EXCEPTION_POINTERS* ep) {
    char buf[80];
    IAT(ClipCursor, 0x5d76b4)(0);
    EXCEPTION_RECORD* rec = *(EXCEPTION_RECORD* volatile*)&ep->ExceptionRecord;   // read before the count check
    uint32_t code = *(volatile DWORD*)&rec->ExceptionCode;
    uint32_t addr = (uint32_t)(uintptr_t)*(void* volatile*)&rec->ExceptionAddress;
    if (GI32(X_COUNT) >= 4) return 0;
    GI32(X_COUNT) += 1;
    if (G8(X_ABENDED) == 0) {
        LogReport(S(0x4e6b1c));                          // "===="
        const char* what = except2name(code);
        LogReport(S(0x4e6b5c), TaskGetName(TaskGetID()), addr, what);   // "EXCEPTION: Task \"%s\" @ %p : %s"
    }
    if (G32(X_LOG) != 0) {
        if (GI32(X_COUNT) == 1) {
            DumpExceptionString((int)G32(X_LOG), buf, S(0x4e6b7c));              // "NON-DEBUG"
            DumpExceptionString((int)G32(X_LOG), buf, S(0x4e6b88), k_my_handler); // "( %p , none , my_handler )"
        }
        const char* what = except2name(code);
        DumpExceptionString((int)G32(X_LOG), buf, S(0x4e6ba4), addr, what);       // "( %p , %s , ? )"
        find_func_in_mapfile((int)addr);
        CONTEXT* ctx = ep->ContextRecord;
        if (ctx) {
            volatile int32_t* sp = (volatile int32_t*)(uintptr_t)ctx->Esp;
            uint32_t last = addr;
            if ((uintptr_t)sp & 0xffff) {
                do {
                    if (!(last <= 0x480000u && last >= 0x400000u)) last = 0;
                    int32_t ret = *sp;
                    if (ret < 0x480000 && ret > 0x400000 && *(volatile uint8_t*)(uintptr_t)(ret - 5) == 0xe8) {
                        uint32_t target = (uint32_t)(*(volatile int32_t*)(uintptr_t)(ret - 4) + ret);
                        if (last == 0 || last - target < 0x190u) {
                            DumpExceptionString((int)G32(X_LOG), buf, S(0x4e6bb4), ret);  // "( %p, call-stack , ? )"
                            find_func_in_mapfile(*sp);
                            last = (uint32_t)*sp;
                        }
                    }
                    sp++;
                } while ((uintptr_t)sp & 0xffff);
            }
        }
    }
    if (is_floating_exception(code)) crt_clearfp();
    if (G8(X_ABENDED) == 0) LogReport(S(0x4e6bcc));     // "===="
    ExceptTrigger();
    IAT(ExitProcess, 0x5d7478)(1);
    return 0;
}
static void fp_my_handler(Footprint& f, EXCEPTION_POINTERS*) { f.replay_only = "the crash handler: ends the process"; }
PORT_FN(0x00415680, "my_handler", my_handler_rw, fp_my_handler)

static const char* __cdecl except2name_rw(uint32_t code) {
    switch (code) {
    case 0x80000001: return S(0x4e6e50);   // EXCEPTION_GUARD_PAGE
    case 0x80000002: return S(0x4e6c28);   // EXCEPTION_DATATYPE_MISALIGNMENT
    case 0x80000003: return S(0x4e6c48);   // EXCEPTION_BREAKPOINT
    case 0x80000004: return S(0x4e6c60);   // EXCEPTION_SINGLE_STEP
    case 0xc0000005: return S(0x4e6c0c);   // EXCEPTION_ACCESS_VIOLATION
    case 0xc0000006: return S(0x4e6db8);   // EXCEPTION_IN_PAGE_ERROR
    case 0xc000001d: return S(0x4e6dd0);   // EXCEPTION_ILLEGAL_INSTRUCTION
    case 0xc0000025: return S(0x4e6df0);   // EXCEPTION_NONCONTINUABLE_EXCEPTION
    case 0xc0000026: return S(0x4e6e30);   // EXCEPTION_INVALID_DISPOSITION
    case 0xc000008c: return S(0x4e6c78);   // EXCEPTION_ARRAY_BOUNDS_EXCEEDED
    case 0xc000008d: return S(0x4e6c98);   // EXCEPTION_FLT_DENORMAL_OPERAND
    case 0xc000008e: return S(0x4e6cb8);   // EXCEPTION_FLT_DIVIDE_BY_ZERO
    case 0xc000008f: return S(0x4e6cd8);   // EXCEPTION_FLT_INEXACT_RESULT
    case 0xc0000090: return S(0x4e6cf8);   // EXCEPTION_FLT_INVALID_OPERATION
    case 0xc0000091: return S(0x4e6d18);   // EXCEPTION_FLT_OVERFLOW
    case 0xc0000092: return S(0x4e6d30);   // EXCEPTION_FLT_STACK_CHECK
    case 0xc0000093: return S(0x4e6d4c);   // EXCEPTION_FLT_UNDERFLOW
    case 0xc0000094: return S(0x4e6d64);   // EXCEPTION_INT_DIVIDE_BY_ZERO
    case 0xc0000095: return S(0x4e6d84);   // EXCEPTION_INT_OVERFLOW
    case 0xc0000096: return S(0x4e6d9c);   // EXCEPTION_PRIV_INSTRUCTION
    case 0xc00000fd: return S(0x4e6e14);   // EXCEPTION_STACK_OVERFLOW
    case 0xc000013a: return S(0x4e6e68);   // CONTROL_C_EXIT
    default: return S(0x4e6e78);           // "<unknown>"
    }
}
static void fp_pure_u(Footprint& f, uint32_t) { f.pure = true; }
PORT_FN(0x00415850, "except2name", except2name_rw, fp_pure_u)

static uint8_t __cdecl is_floating_exception_rw(uint32_t code) { return code >= 0xc000008du && code <= 0xc0000093u; }
PORT_FN(0x00415a40, "is_floating_exception", is_floating_exception_rw, fp_pure_u)

// the map's nearest symbol at or below `addr` (" 0001:offset name address" lines, until "FIXUPS")
// Quirk kept: a map line over 255 characters would overrun the 256-byte line buffer into the name buffer and then
// the exit handler table (0x508d20); the longest line in v1.0's map is 158.
static void __cdecl find_func_in_mapfile_rw(int addr) {
    char* p = (char*)(uintptr_t)G32(X_MAP);
    if (!p) return;
    int32_t best = 123456789;
    G8(X_BEST) = 0;
    for (;;) {
        p = get_string((char*)(uintptr_t)X_LINE, p);
        if (G32(X_MAP) + G32(X_MAP_SIZE) <= (uint32_t)(uintptr_t)p) break;
        int32_t off, at;
        int n = crt_sscanf((char*)(uintptr_t)X_LINE, S(0x4e6e84), &off, (char*)(uintptr_t)X_NAME, &at);   // " 0001:%x %s %x"
        if (n == 3) {
            if (at < addr) {
                int32_t d = addr - at;
                if (best > d) {
                    str_copy((char*)(uintptr_t)X_BEST, (const char*)(uintptr_t)X_NAME);
                    best = d;
                }
            }
        } else if (crt_strstr((char*)(uintptr_t)X_LINE, S(0x4e6e94))) {   // "FIXUPS"
            break;
        }
    }
    if (best != 123456789) LogReport(S(0x4e6e9c), best, (const char*)(uintptr_t)X_BEST);   // "trace: byte 0x%x of \"%s\""
    else LogReport(S(0x4e6eb8), addr);                  // "trace: Can't find 0x%x in mapfile"
}
static void fp_find_func(Footprint& f, int) { f.add((void*)(uintptr_t)X_BEST, 0x300, "map search buffers"); }
PORT_FN(0x00415a60, "find_func_in_mapfile", find_func_in_mapfile_rw, fp_find_func)

// one line (to '\n', not included) into dst; returns the next line. No end check: it needs the '\n'.
static char* __cdecl get_string_rw(char* dst, char* src) {
    while (*(volatile char*)src != '\n') *dst++ = *src++;
    src++;
    *dst = 0;
    return src;
}
static void fp_get_string(Footprint& f, char* dst, char* src) {
    uint32_t n = 0;
    while (src[n] != '\n') n++;
    f.add(dst, n + 1, "line");
}
PORT_FN(0x00415b60, "get_string", get_string_rw, fp_get_string)

static void __cdecl ExceptInstallExitHandler_rw(uint32_t fn) {
    int i = 0;
    for (uint32_t at = X_HANDLERS; at < X_HANDLERS + 16; at += 4, i++)
        if (G32(at) == 0) {
            G32(X_HANDLERS + (uint32_t)i * 4u) = fn;
            return;
        }
    LogPanic(S(0x4e6edc));                             // "Not enough slots in exception handler table."
}
static void fp_exit_handlers(Footprint& f, uint32_t) { f.add((void*)(uintptr_t)X_HANDLERS, 16, "exit handlers"); }
PORT_FN(0x00415b90, "ExceptInstallExitHandler", ExceptInstallExitHandler_rw, fp_exit_handlers)

static void __cdecl ExceptUninstallExitHandler_rw(uint32_t fn) {
    for (uint32_t at = X_HANDLERS; at < X_HANDLERS + 16; at += 4)
        if (G32(at) == fn) G32(at) = 0;
}
PORT_FN(0x00415bd0, "ExceptUninstallExitHandler", ExceptUninstallExitHandler_rw, fp_exit_handlers)

// the exit handlers, last first; each cleared after it runs
static void __cdecl ExceptTrigger_rw() {
    for (uint32_t at = X_HANDLERS + 12; at >= X_HANDLERS; at -= 4) {
        uint32_t fn = G32(at);
        if (fn != 0) {
            FN(Void_t, fn)();
            G32(at) = 0;
        }
    }
}
static void fp_runs_handlers(Footprint& f) { f.replay_only = "runs the exit handlers"; }
PORT_FN(0x00415bf0, "ExceptTrigger", ExceptTrigger_rw, fp_runs_handlers)

static void __cdecl ExceptEnd_rw() {
    IAT(SetUnhandledExceptionFilter, 0x5d7570)((LPTOP_LEVEL_EXCEPTION_FILTER)(uintptr_t)G32(X_PREV));
    if (G32(X_LOG) != 0) {
        FileClose((volatile uint32_t*)(uintptr_t)X_LOG);
        G32(X_LOG) = 0;
    }
    G32(X_HANDLERS) = 0;
    G32(X_HANDLERS + 4) = 0;
    G32(X_HANDLERS + 8) = 0;
    G32(X_HANDLERS + 12) = 0;
}
static void fp_except_end(Footprint& f) { f.replay_only = "restores the exception filter, closes a file"; }
PORT_FN(0x00415c20, "ExceptEnd", ExceptEnd_rw, fp_except_end)

// The FPU exception mask, through the game's _controlfp (which leaves _EM_DENORMAL alone): on -> overflow and
// divide-by-zero unmasked (docs/PORTING.md rule 10a); off -> overflow unmasked, divide-by-zero masked.
static void __cdecl ExceptDiv0Crashes_rw(uint8_t on) {
    uint32_t cw = crt_controlfp(0, 0);
    if (on == 0) cw = (cw & 0xfff7fffbu) | 8u;
    else cw &= 0xfff7fff3u;
    crt_controlfp(cw, 0x8001fu);
}
static void fp_fpu_mode(Footprint& f, uint8_t) { f.replay_only = "sets the FPU control word (checked by the harness)"; }
PORT_FN(0x00415c60, "ExceptDiv0Crashes", ExceptDiv0Crashes_rw, fp_fpu_mode)

// precision control: on -> 24 bits (every tick of physics_thread; gxBegin), off -> 64
static void __cdecl ExceptSinglePrecision_rw(uint8_t on) { crt_controlfp(on != 0 ? 0x20000u : 0u, 0x30000u); }
PORT_FN(0x00415ca0, "ExceptSinglePrecision", ExceptSinglePrecision_rw, fp_fpu_mode)

// ====================================================================================================================
// abend.obj: LogPanic's end -- a write to address 0, so the crash handler reports it
// ====================================================================================================================
static void __cdecl abend_rw() {
    G8(X_ABENDED) = 1;
    *(volatile uint8_t*)(uintptr_t)0 = 0;
}
static void fp_abend(Footprint& f) { f.replay_only = "crashes on purpose"; }
PORT_FN(0x00416040, "abend", abend_rw, fp_abend)

// ====================================================================================================================
// vxd.obj: Windows 95's vexed.vxd (physics-thread registration, locked memory). On NT the open fails and every call
// below is a no-op; MemAlloc never uses it.
// ====================================================================================================================
static uint8_t __cdecl vxdLoad_rw() {
    HANDLE h = IAT(CreateFileA, 0x5d7554)(*(const char* volatile*)(uintptr_t)V_NAME, GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, 0);
    G32(V_HANDLE) = (uint32_t)(uintptr_t)h;
    uint32_t v = (uint32_t)(uintptr_t)h;
    if (v != 0xffffffffu) {
        lockdown_exe();
        v = G32(V_HANDLE);
    }
    G32(V_HANDLE) = v;
    return v != 0xffffffffu;
}
static void fp_vxd(Footprint& f) { f.replay_only = "opens or talks to the VxD"; }
PORT_FN(0x00418340, "vxdLoad", vxdLoad_rw, fp_vxd)

// each of the exe's .text / .rdata / .data sections, to lockdown_memory (a stub returning 1)
static void __cdecl lockdown_exe_rw() {
    uint8_t* base = Win32GetAppInstance();
    uint8_t* nt = base + *(volatile int32_t*)(base + 0x3c);
    uint8_t* opt = nt + 0x18;
    int i = 0;
    uint32_t off = 0;
    if (*(volatile uint16_t*)(nt + 6) > 0) {
        do {
            uint8_t* sec = nt + *(volatile uint16_t*)(nt + 0x14) + off + 0x18;
            if (should_lockdown_section(sec)) {
                uint32_t at = *(volatile uint32_t*)(sec + 0xc) + *(volatile uint32_t*)(opt + 0x1c);
                uint32_t vsize = *(volatile uint32_t*)(sec + 8), raw = *(volatile uint32_t*)(sec + 0x10);
                lockdown_memory((void*)(uintptr_t)at, vsize > raw ? vsize : raw);
            }
            off += 0x28;
            i++;
        } while ((int)*(volatile uint16_t*)(nt + 6) > i);
    }
}
PORT_FN(0x00418380, "lockdown_exe", lockdown_exe_rw, fp_none)

static uint8_t __cdecl lockdown_memory_rw(void*, uint32_t) { return 1; }
static void fp_lockdown_memory(Footprint& f, void*, uint32_t) { f.pure = true; }
PORT_FN(0x00418400, "lockdown_memory", lockdown_memory_rw, fp_lockdown_memory)

// Quirk kept: the section's 8-byte name isn't necessarily terminated (stricmp reads on into the header)
static uint8_t __cdecl should_lockdown_section_rw(const uint8_t* name) {
    const char* list[3] = {S(0x4ea7c8), S(0x4ea7d0), S(0x4ea7d8)};   // ".text", ".rdata", ".data"
    for (const char** p = list; p < list + 3; p++)
        if (crt_stricmp(*p, (const char*)name) == 0) return 1;
    return 0;
}
static void fp_should_lockdown(Footprint&, const uint8_t*) {}
PORT_FN(0x00418410, "should_lockdown_section", should_lockdown_section_rw, fp_should_lockdown)

static void __cdecl vxdUnload_rw() {
    uint32_t h = G32(V_HANDLE);
    if (h != 0xffffffffu) {
        IAT(CloseHandle, 0x5d7548)((HANDLE)(uintptr_t)h);
        G32(V_HANDLE) = 0xffffffffu;
    }
}
PORT_FN(0x00418460, "vxdUnload", vxdUnload_rw, fp_vxd)

static uint8_t __cdecl vxdIsLoaded_rw() { return G32(V_HANDLE) + 1u >= 1u; }
PORT_FN(0x00418480, "vxdIsLoaded", vxdIsLoaded_rw, fp_none)

static void* __cdecl vxdMemAlloc_rw(int size) {
    void* p = 0;
    if (G32(V_HANDLE) != 0xffffffffu) {
        if (!call_vxd(4, &size, 4, &p, 4)) LogPanic(S(0x4ea7e0));   // "vxdMemAlloc: table full?"
    }
    return p;
}
static void fp_vxd_i(Footprint& f, int) { f.replay_only = "talks to the VxD"; }
PORT_FN(0x00418490, "vxdMemAlloc", vxdMemAlloc_rw, fp_vxd_i)

static uint8_t __cdecl call_vxd_rw(int code, void* in, int in_size, void* out, int out_size) {
    DWORD got;
    uint32_t h = G32(V_HANDLE);
    if (h == 0xffffffffu) return 0;
    return IAT(DeviceIoControl, 0x5d74ec)((HANDLE)(uintptr_t)h, (DWORD)code, in, (DWORD)in_size, out, (DWORD)out_size, &got, 0) != 0;
}
static void fp_call_vxd(Footprint& f, int, void*, int, void*, int) { f.replay_only = "talks to the VxD"; }
PORT_FN(0x004184e0, "call_vxd", call_vxd_rw, fp_call_vxd)

static void __cdecl vxdMemFree_rw(void* p) {
    if (G32(V_HANDLE) != 0xffffffffu) {
        if (!call_vxd(5, &p, 4, 0, 0)) LogPanic(S(0x4ea7fc));   // "vxdMemFree: bogus ptr?"
    }
}
static void fp_vxd_p(Footprint& f, void*) { f.replay_only = "talks to the VxD"; }
PORT_FN(0x00418530, "vxdMemFree", vxdMemFree_rw, fp_vxd_p)

static void __cdecl vxdRegisterPhysicsThread_rw() {
    if (G32(V_HANDLE) != 0xffffffffu) {
        if (!call_vxd(1, 0, 0, 0, 0)) LogReport(S(0x4ea814));   // "!!!!: timer register fails."
    }
}
PORT_FN(0x00418560, "vxdRegisterPhysicsThread", vxdRegisterPhysicsThread_rw, fp_vxd)

static void __cdecl vxdUnregisterPhysicsThread_rw() {
    if (G32(V_HANDLE) != 0xffffffffu) {
        if (!call_vxd(3, 0, 0, 0, 0)) LogReport(S(0x4ea830));   // "!!!!: timer UNregister fails."
    }
}
PORT_FN(0x00418590, "vxdUnregisterPhysicsThread", vxdUnregisterPhysicsThread_rw, fp_vxd)

static int __cdecl vxdGetPhysicsThreadEIP_rw() {
    int eip = 0x12345678;
    int in;
    if (G32(V_HANDLE) != 0xffffffffu) {
        in = 0;
        call_vxd(2, &in, 4, &eip, 4);
    }
    return eip;
}
PORT_FN(0x004185c0, "vxdGetPhysicsThreadEIP", vxdGetPhysicsThreadEIP_rw, fp_vxd)

// ====================================================================================================================
// kernel.obj
// ====================================================================================================================
static void __cdecl kernel_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00418610, "$E1(kernel.obj)", kernel_E1_rw, fp_static_init)
static void __cdecl kernel_E2_rw() { FN(Void_t, 0x00418610)(); }
PORT_FN(0x00418600, "$E2(kernel.obj)", kernel_E2_rw, fp_static_init)

static uint8_t __cdecl KernelBegin_rw() {
    if (G8(K_UP) != 0) {
        LogPanic(S(0x4ea8dc));                           // "Kernel began twice."
        return 0;
    }
    if (!ProfBegin()) {
        LogPanic(S(0x4ea8b8));                           // "Can't start Pentium timer subsystem"
        return 0;
    }
    if (!SyncBegin()) {
        LogPanic(S(0x4ea890));                           // "Can't start Synchronization subsystem."
        ProfEnd();
        return 0;
    }
    TaskBegin();
    FileBegin();
    if (LogBegin()) {
        BGBegin();
        ExceptBegin();
        if (MemBegin(0x1400000)) {
            Win32Begin();
            KeyBegin();
            ScanBegin();
            MouseBegin();
            JoyBegin();
            G8(K_UP) = 1;
            return 1;
        }
        LogPanic(S(0x4ea854));                           // "Can't start Memory subsystem"
        LogEnd();
    } else {
        LogPanic(S(0x4ea874));                           // "Can't start Log subsystem."
    }
    FileEnd();
    BGEnd();
    TaskEnd();
    SyncEnd();
    ProfEnd();
    return 0;
}
static void fp_kernel(Footprint& f) { f.replay_only = "starts or stops the kernel"; }
PORT_FN(0x00418620, "KernelBegin", KernelBegin_rw, fp_kernel)

static void __cdecl KernelEnd_rw() {
    if (G8(K_UP) == 0) {
        LogPanic(S(0x4ea8f0));                           // "KernelEnd() called when kernel not active!"
        return;
    }
    JoyEnd();
    MouseEnd();
    ScanEnd();
    KeyEnd();
    Win32End();
    MemEnd();
    ExceptEnd();
    FileVerifyNoOpenFiles();
    BGEnd();
    LogEnd();
    FileEnd();
    TaskEnd();
    SyncEnd();
    ProfEnd();
    G8(K_UP) = 0;
}
PORT_FN(0x00418710, "KernelEnd", KernelEnd_rw, fp_kernel)

// ====================================================================================================================
// bg.obj: the BG task -- a 16 ms timer thread calling every hooked function in priority order under the "BG" lock
// ====================================================================================================================
static void __cdecl bg_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x00418930, "$E1(bg.obj)", bg_E1_rw, fp_static_init)
static void __cdecl bg_E2_rw() { FN(Void_t, 0x00418930)(); }
PORT_FN(0x00418920, "$E2(bg.obj)", bg_E2_rw, fp_static_init)

static void __cdecl BGBegin_rw() {
    G32(B_SYNC) = (uint32_t)MultiBegin(S(0x4eab44));   // "BG"
    G32(B_TASK) = (uint32_t)TaskSetTimer(S(0x4eab48), k_bg_vector, 16);   // "BGTask"
    bg_hook(0)->fn = FN(Void_t, k_empty_hook);
    bg_hook(0)->prio = 2;
    G32(B_N) = 1;
}
PORT_FN(0x00418940, "BGBegin", BGBegin_rw, fp_threads)

static void __cdecl bg_vector_rw() {
    MultiEnter((int)G32(B_SYNC), 0, 0);
    for (int i = 0; GI32(B_N) > i; i++) bg_hook(i)->fn();
    MultiLeave((int)G32(B_SYNC), 0, 0);
}
static void fp_bg_vector(Footprint& f) { f.replay_only = "runs the BG hooks (physics_thread)"; }
PORT_FN(0x00418990, "bg_vector", bg_vector_rw, fp_bg_vector)

static void __cdecl empty_hook_rw() {}
PORT_FN(0x004189e0, "empty_hook", empty_hook_rw, fp_pure0)

// Insert before the first hook of equal or higher priority number.
// Quirks kept: a priority above every hook's (empty_hook sits at 2) finds no place and is silently dropped (the
// ASSERT_MSGs are bare `ret`s); with 16 hooks the ASSERT does nothing and the 17th overruns the 16-entry table.
static void __cdecl BGHook_rw(uint32_t fn, int prio) {           // fn: the hook's address
    MultiEnter((int)G32(B_SYNC), 0, 0);
    ASSERT_MSG_bg(GI32(B_N) < 16 ? 1 : 0, S(0x4eab60));   // "BGHook(): Too many hooks, increase MAX_HOOKS!"
    uint8_t inserted = 0;
    int32_t n = GI32(B_N);
    int i = 0;
    if (n > 0) {
        do {
            if (bg_hook(i)->prio >= prio) {
                crt_memmove(bg_hook(i + 1), bg_hook(i), (uint32_t)(n - i) * 8u);
                inserted = 1;
                bg_hook(i)->fn = FN(Void_t, fn);
                bg_hook(i)->prio = prio;
                GI32(B_N) += 1;
                break;
            }
            i++;
        } while (n > i);
    }
    ASSERT_MSG_bg(inserted, S(0x4eab90));               // "BGHook(): (!inserted) ?!?!  ..."
    MultiLeave((int)G32(B_SYNC), 0, 0);
}
static void fp_bg_hook(Footprint& f, uint32_t, int) { f.replay_only = "takes the BG lock"; }
PORT_FN(0x004189f0, "BGHook", BGHook_rw, fp_bg_hook)

static void __cdecl ASSERT_MSG_rw(int, const char*) {}   // variadic in the original; a bare `ret` either way
static void fp_assert_msg(Footprint& f, int, const char*) { f.pure = true; }
PORT_FN(0x00418ab0, "ASSERT_MSG(bg.obj)", ASSERT_MSG_rw, fp_assert_msg)

static void __cdecl BGUnhook_rw(uint32_t fn) {
    MultiEnter((int)G32(B_SYNC), 0, 0);
    uint8_t found = 0;
    int32_t n = GI32(B_N);
    int i = 0;
    if (n > 0) {
        do {
            if ((uint32_t)(uintptr_t)bg_hook(i)->fn == fn) {
                found = 1;
                n--;
                GI32(B_N) = n;
                crt_memmove(bg_hook(i), bg_hook(i + 1), (uint32_t)(n - i) * 8u);
                break;
            }
            i++;
        } while (i < n);
    }
    ASSERT_MSG_bg(found, S(0x4eabc8), fn);              // "BGUnHook(): Can't find func matching 0x%x"
    MultiLeave((int)G32(B_SYNC), 0, 0);
}
static void fp_bg_unhook(Footprint& f, uint32_t) { f.replay_only = "takes the BG lock"; }
PORT_FN(0x00418ac0, "BGUnhook", BGUnhook_rw, fp_bg_unhook)

static void __cdecl BGEnd_rw() {
    TaskKillTimer((int)G32(B_TASK));
    MultiEnd((int)G32(B_SYNC), 0, 0);
    ASSERT_MSG_bg(GI32(B_N) == 1 ? 1 : 0, S(0x4eabf4));   // "BGEnd(): Someone didn't free their hook!"
}
PORT_FN(0x00418b50, "BGEnd", BGEnd_rw, fp_threads)

// ====================================================================================================================
// time.obj
// ====================================================================================================================
static void __cdecl time_E1_rw() { rcfunc_is_internal(); }
PORT_FN(0x004d8ae0, "$E1(time.obj)", time_E1_rw, fp_static_init)
static void __cdecl time_E2_rw() { FN(Void_t, 0x004d8ae0)(); }
PORT_FN(0x004d8ad0, "$E2(time.obj)", time_E2_rw, fp_static_init)

static void __cdecl TimeGetTimeOfDay_rw(TimeOfDay* t) {
    SYSTEMTIME st;
    IAT(GetLocalTime, 0x5d75f0)(&st);
    t->year = st.wYear;
    t->month = st.wMonth;
    t->day = st.wDay;
    t->hour = st.wHour;
    t->minute = st.wMinute;
    t->second = st.wSecond;
    t->ms = st.wMilliseconds;
    t->dow = st.wDayOfWeek;
}
static void fp_time_of_day(Footprint& f, TimeOfDay*) { f.replay_only = "reads the clock"; }
PORT_FN(0x004d8af0, "TimeGetTimeOfDay", TimeGetTimeOfDay_rw, fp_time_of_day)

// ---- integration notes -------------------------------------------------------------------------------------------------
// * The _prof.obj routines (0x418780 pr_overhead_begin, 0x418798 pr_overhead_end, 0x4187b7 pr_start, 0x4187f3 pr_end,
//   0x418839 pr_rdtsc_lo -- exactly 5 bytes, 0x41883e pr_sys_begin, 0x41888f pr_sys_end, 0x4188ad rdtsc, 0x4188b0
//   isCpuidSupported, 0x4188d5 isRdtscSupported) are data labels in the map, so the inventory has no sizes for them.
// * rdtsc (0x4188ad) is 3 bytes: see its note above (a short jump into the padding at 0x418912).
// * $E2 / TaskGetID start with a jmp rel32, $E7 and isRdtscSupported (pushal; call rel32) with a call.
