// port.h -- M3: the game's own functions rewritten, one at a time, and checked against the originals.
//
// Every rewritten function is registered with PORT_FN and switched in viperport.ini:
//
//   [port]
//   default=new                 ; new | original | shadow -- for every function without its own line
//   Obstacle::Update=shadow     ; one function's own switch, by its name in the map
//   shadow_every=1              ; shadow: check every Nth call (1 = every call)
//   shadow_per_tick=4           ; and at most this many checks of each function per physics tick (0 = no limit),
//                               ; so the cost stays bounded however many functions are in shadow mode
//
//   new       the rewrite runs in place of the original
//   original  the original runs; the rewrite is never called
//   shadow    both run, from the same starting state, and are compared byte for byte: whatever the
//             function may change (its footprint, below), the physics and AI globals, and the return
//             value. The game always continues on the ORIGINAL's result, so a mismatch is only logged:
//             the function, the call, and the first byte that differs, named from the recovered types.
//
// A shadow check needs to call the original after its entry has been replaced, so it goes through a
// trampoline: the original's first instructions, copied, then a jump back. tools/gen_port_table.py
// finds every address hooked this way (PORT_FN and detour below) and writes their instructions from the
// v1.0 race.exe into prologues.inc; the trampoline is built only if the live bytes still match. So the
// harness works on the reference build, v1.0. On the race.bin builds a PORT_FN with a per-build
// prologue still switches between new and original; shadow and detours need v1.0.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// Fixes (docs/PORTING.md, "Fixes"): a rewrite departs from the original only where the original would
// crash, hang or overrun a buffer, each place marked `// FIX:` and written `if (VP_FIX && ...)`, so every
// other input still gives the original's bits. Always on in the game. The test harnesses define
// VP_FAITHFUL to build the original behaviour and check it against the original bit for bit, and test
// the fixes separately on the inputs that used to fail.
#ifdef VP_FAITHFUL
#define VP_FIX 0
#else
#define VP_FIX 1
#endif

// M1 (viperport.cpp) lifts the game's limits by patching operands of the original code in place: list
// addresses, capacities, pool sizes. A rewrite of a function M1 patches must take those values from the
// original's own instruction, m1_operand(address of the operand), so it gets M1's value in the game and the
// stock one in a harness (docs/PORTING.md rule 12).
static inline uint32_t m1_operand(uint32_t at) { return *(const volatile uint32_t*)at; }
// ... except an operand in the first 5 bytes of a function the port hooks: the hook's jmp overwrites it. It is read
// there only while the function's first byte is still the original's (`first`: a harness, or the function left
// original), else from `other`, another instruction M1 repoints at the same table, plus `delta` (the difference of
// their stock values). tools/gen_port_tables.py requires this form for such an operand.
static inline uint32_t m1_operand_hooked(uint32_t at, uint32_t fn, uint8_t first, uint32_t other, int32_t delta) {
    return *(const volatile uint8_t*)fn == first ? m1_operand(at) : m1_operand(other) + (uint32_t)delta;
}

enum PortMode { PORT_ORIGINAL, PORT_NEW, PORT_SHADOW };

// ---- footprints: the memory a function may change, besides the physics and AI globals ----------------
struct Footprint {
    enum { MAX = 8192 };
    struct Region { void* p; uint32_t n; const char* what; };
    Region r[MAX];
    int n = 0;
    const char* replay_only = 0;                            // set: no shadow check; the replay checks it
    bool pure = false;                                      // set: touches only its footprint -- no globals
    void add(void* p, uint32_t bytes, const char* what);   // a plain block
    void object(void* obj, const char* what = "this");     // a game object, sized by its class (vtable)
};

// ---- a rewritten function ------------------------------------------------------------------------------
struct PortFn {
    uint32_t v10;                     // the original's address in v1.0 race.exe
    const char* name;                 // its name in the map: the ini key
    void* repl;                       // the rewrite
    void* shadow;                     // the shadow wrapper (same signature as the rewrite)
    const uint8_t* other_prologue;    // first bytes on the race.bin builds (only for PORT_NEW there), or 0
    uint8_t other_len;
    void* orig = 0;                   // trampoline to the original, once installed (v1.0)
    PortMode mode = PORT_NEW;
    volatile long calls = 0, checks = 0, mismatches = 0;
    int budget_tick = -1, budget_used = 0;         // shadow_per_tick bookkeeping
    bool patched = false;                          // the installed function isn't stock v1.0: left original
    PortFn(uint32_t v, const char* nm, void* r, void* s, const uint8_t* op = 0, uint8_t ol = 0);
};

// shadow machinery, shared by every wrapper (port.cpp). A check runs the ORIGINAL first -- it reads the
// live inputs (random numbers, the controls), and they're logged -- then restores the starting state and
// runs the rewrite, feeding it the logged inputs in order; compares; and puts the original's result back.
bool shadow_begin(PortFn* f);                  // should this call be checked? (not nested, sampled)
Footprint& shadow_footprint();                 // the current check's footprint, to fill
void shadow_snapshot();                        // save the footprint and the globals; the original's pass
void shadow_after_original(const void* ret, size_t n);   // keep its results, restore; the rewrite's pass
void shadow_finish(PortFn* f, const void* ret, size_t n);   // compare, log, restore the original's result
void shadow_abandon(PortFn* f);                // end a check without comparing (replay_only)

// Inputs, for the hooks that deliver them (replay.cpp: Random, DriverGet*). In a check's rewrite pass,
// shadow_feed fills v with what the original's pass read at that point and returns true; otherwise it
// returns false, the hook gets the value as usual and passes it to shadow_saw (which logs it during an
// original's pass, on that thread).
bool shadow_feed(uint8_t kind, void* v, size_t n);
void shadow_saw(uint8_t kind, const void* v, size_t n);
bool shadow_on();                              // is any function in shadow mode? (the input hooks are needed)
// the physics thread (set by the recorder's PhysTaskUpdate hook, installed whenever shadow checks are on).
// A check made on another thread -- the main thread's input code -- doesn't save or restore the physics
// globals, which the physics thread is using at that moment: its footprint lists what it writes.
extern volatile unsigned long g_physics_thread_id;

// Shadow<F>::call<fn, NEW, FOOTPRINT> -- a wrapper with the rewrite's exact signature and calling
// convention (a __thiscall arrives as __fastcall with an unused edx, as everywhere in this DLL).
template <typename F> struct Shadow;
#define VP_SHADOW_CC(CC)                                                                                   \
    template <typename R, typename... Args> struct Shadow<R(CC*)(Args...)> {                             \
        typedef R(CC* Fn)(Args...);                                                                      \
        template <PortFn* P, Fn NEW, void (*FP)(Footprint&, Args...)>                                    \
        static R CC call(Args... a) {                                                                    \
            if (!shadow_begin(P)) return ((Fn)P->orig)(a...);                                            \
            FP(shadow_footprint(), a...);                                                                \
            if (shadow_footprint().replay_only) { shadow_abandon(P); return ((Fn)P->orig)(a...); }       \
            shadow_snapshot();                                                                         \
            if constexpr (std::is_void_v<R>) {                                                           \
                ((Fn)P->orig)(a...);                                                                     \
                shadow_after_original(0, 0);                                                             \
                NEW(a...);                                                                               \
                shadow_finish(P, 0, 0);                                                                  \
            } else {                                                                                     \
                R ro = ((Fn)P->orig)(a...);                                                              \
                shadow_after_original(&ro, sizeof ro);                                                   \
                R rn = NEW(a...);                                                                        \
                shadow_finish(P, &rn, sizeof rn);                                                        \
                return ro;                                                                               \
            }                                                                                            \
        }                                                                                                \
    };
VP_SHADOW_CC(__cdecl)
VP_SHADOW_CC(__fastcall)
VP_SHADOW_CC(__stdcall)
#undef VP_SHADOW_CC

// PORT_FN(0x0043d2e0, "Obstacle::Update", Obstacle_Update, fp_obstacle_update)
//   registers Obstacle_Update as the rewrite of the v1.0 function at 0x43d2e0; fp_obstacle_update has
//   the rewrite's arguments after a Footprint& and says what the function may change.
// PORT_FN_BUILDS(..., prologue, len) also names the first bytes to expect on the race.bin builds.
#define VP_CAT2(a, b) a##b
#define VP_CAT(a, b) VP_CAT2(a, b)
#ifdef VP_FUZZ
// test/fuzz.cpp: the same rewrites, compiled into a test program that runs each pure one against the
// original on random inputs, outside the game (test/fuzz.h)
#include "../test/fuzz.h"
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static FuzzReg VP_CAT(fuzz_, NEW)(V10, NAME, &Fuzz<decltype(&NEW)>::run<&NEW, &FP>);
#else
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    namespace { extern PortFn VP_CAT(port_, NEW); }   /* file-local: rewrites in different files */     \
    namespace { PortFn VP_CAT(port_, NEW)(V10, NAME, (void*)&NEW,                                                \
        (void*)&Shadow<decltype(&NEW)>::call<&VP_CAT(port_, NEW), &NEW, &FP>, PRO, PROLEN); }
#endif
#define PORT_FN(V10, NAME, NEW, FP) PORT_FN_BUILDS(V10, NAME, NEW, FP, 0, 0)

// ---- the framework ----------------------------------------------------------------------------------------
void port_check_stock();                       // before anything is patched: find non-stock functions
void port_note_m1(uint32_t at, uint32_t n);    // M1 patched these bytes (a trampoline may copy them: see port.cpp)
void port_install(const char* ini);            // read [port], hook every registered function
void port_report();                            // the exit log: per function, calls / checks / mismatches
bool port_is_new(const PortFn& f);             // is the rewrite in force (new or shadow)?

// Hook the v1.0 function at v10 to run `to` instead, and return a trampoline that runs the original
// (0 if the build isn't v1.0 or the bytes aren't what prologues.inc expects). For the harness's own
// hooks (the recorder): they always call the original through the trampoline.
void* detour(uint32_t v10, void* to, const char* what);
// The same for a hook in front of a function that may also be rewritten (the recorder, the shadow outputs):
// the jump at v10 goes to `to`, and what comes back is what `to` calls on to -- the rewrite when it's in
// force (in shadow mode, its checking entry), otherwise the original through a trampoline. After port_install.
void* detour_front(uint32_t v10, void* to, const char* what);

// the class a game object's vtable belongs to, and its size (state_layout.inc); 0 if unknown
const char* class_of(const void* obj, uint32_t* size);
// the named field of `cls` (or its bases and embedded parts) that holds byte `off`, e.g. "wheels[2].load"
const char* field_name(const char* cls, uint32_t off, char* buf, size_t n);

// M3 3.1 (replay.cpp): the race recorder, replayer and per-tick state trace
void replay_install(const char* ini);
void replay_report();
