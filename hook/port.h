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
// finds every address hooked this way (PORT_FN and detour below) and writes their instructions' length and
// hash from the v1.0 race.exe into prologues.inc; the trampoline copies the live bytes, and is built only if
// they still hash as v1.0's. So the
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

// vrmod (viper-mod-manager) patches some of the functions the port replaces with values the player chooses: the four
// screen modes' sizes (resolution.py), the hornball's mass and radius (hornball.py), the addresses of aspectfix.py's two
// constants (where it found or put them). The stock check accepts those
// functions with any value in those bytes (tools/gen_port_tables.py, VRMOD_MASKS), so a rewrite of one reads each such
// value from the original's own instruction, vrmod_operand(address of the operand): the player's value in a patched
// race.exe, the stock one in a stock race.exe or a harness. The standalone keeps these bytes out of its int3 fill
// (tools/gen_standalone.py finds every vrmod_operand literal). docs/FIXES.md, "vrmod's patches".
static inline uint32_t vrmod_operand(uint32_t at) { return *(const volatile uint32_t*)at; }
// an operand's address kept in a table for vrmod_operand to read later (the tools find it by this name too)
constexpr uint32_t vrmod_operand_at(uint32_t at) { return at; }
// vrmod's opt-in patches that change behaviour rather than a value (headon.py: AICar::headon_panic a bare ret): did the
// stock check (port_check_stock, before anything was hooked) find vrmod's patch in the installed function at v10? A
// rewrite of it then does what the patched function does. The hook's jmp has overwritten the patched bytes by the time
// the rewrite runs, so this is the answer recorded then, not a read of the code. (A harness defines it itself.)
bool port_vrmod_has(uint32_t v10);

// The lit-vertex buffer (mr_model_begin: one model's vertices, lit, 32 bytes each, reused for every model). Stock it
// holds 1,500 vertices, and a model with more writes past it; the rewrite allocates room for 32,768, every vertex an
// int16 face index can name (docs/FIXES.md, "Limits lifted"; vrmod's vertexbuffer.py raises the same push, to at most
// that). vp_g_lit_buf_bytes is what the buffer in use holds: the rewrite's size once it has made it, the original's
// otherwise (footprints read it).
enum : uint32_t { VP_LIT_BUF_STOCK = 0xbb80, VP_LIT_BUF_BYTES = VP_FIX ? 32768u * 32u : VP_LIT_BUF_STOCK };
inline uint32_t vp_g_lit_buf_bytes = VP_LIT_BUF_STOCK;

enum PortMode { PORT_ORIGINAL, PORT_NEW, PORT_SHADOW };

// ---- footprints: the memory a function may change, besides the physics and AI globals ----------------
struct Footprint {
    enum { MAX = 8192 };
    struct Region { void* p; uint32_t n; const char* what; bool stack = false; };
    Region r[MAX];
    int n = 0;
    const char* replay_only = 0;                            // set: no shadow check; the replay checks it
    bool pure = false;                                      // set: touches only its footprint -- no globals
    void add(void* p, uint32_t bytes, const char* what);   // a plain block
    void object(void* obj, const char* what = "this");     // a game object, sized by its class (vtable)
    // a pointer the function may leave aimed at its own stack frame (a local made current and never unset): the
    // two passes' frames differ, so it matches when both leave the same value or both an address on this stack
    void stack_ptr(void* p, const char* what);
};

// ---- a rewritten function ------------------------------------------------------------------------------
struct PortFn {
    uint32_t v10;                     // the original's address in v1.0 race.exe
    const char* name;                 // its name in the map: the ini key
    void* repl;                       // the rewrite
    void* shadow;                     // the shadow wrapper (same signature as the rewrite)
    uint32_t other_hash;              // vp_code_hash of v1.0's first other_len bytes: a PORT_FN_BUILDS, hooked on the
    uint8_t other_len;                // race.bin builds too (only for PORT_NEW there; they check their table's), or 0
    void* orig = 0;                   // trampoline to the original, once installed (v1.0)
    PortMode mode = PORT_NEW;
    volatile long calls = 0, checks = 0, mismatches = 0;
    volatile long heap_skips = 0;     // checks whose original allocated or freed: its result stands, unchecked
    volatile long raced = 0;          // differences while the physics thread dented a car's models: not counted
    int budget_tick = -1, budget_used = 0;         // shadow_per_tick bookkeeping
    bool patched = false;                          // the installed function isn't stock v1.0: left original
    const char* vrmod_patch = 0;                   // it is vrmod's patch the rewrite takes over (port_check_stock)
    bool needs_renderer = false;                   // calls the OpenGL renderer directly (gx_dd.cpp): original without it
    bool needs_audio = false;                      // calls the SDL audio core directly (snd_mix.cpp's wave.obj): likewise
    PortFn(uint32_t v, const char* nm, void* r, void* s, uint32_t oh = 0, uint8_t ol = 0);
};

// shadow machinery, shared by every wrapper (port.cpp). A check runs the ORIGINAL first -- it reads the
// live inputs (random numbers, the controls), and they're logged -- then restores the starting state and
// runs the rewrite, feeding it the logged inputs in order; compares; and puts the original's result back.
bool shadow_begin(PortFn* f);                  // should this call be checked? (not nested, sampled)
Footprint& shadow_footprint();                 // the current check's footprint, to fill
void shadow_snapshot();                        // save the footprint and the globals; the original's pass
// keep its results, restore; the rewrite's pass. false: the original allocated or freed memory, so the rewrite
// isn't run (it would allocate or free again) -- the original's result stands and the check is over
bool shadow_after_original(PortFn* f, const void* ret, size_t n);
void shadow_finish(PortFn* f, const void* ret, size_t n);   // compare, log, restore the original's result
void shadow_abandon(PortFn* f);                // end a check without comparing (replay_only)

// Inputs, for the hooks that deliver them (replay.cpp: Random, DriverGet*). In a check's rewrite pass,
// shadow_feed fills v with what the original's pass read at that point and returns true; otherwise it
// returns false, the hook gets the value as usual and passes it to shadow_saw (which logs it during an
// original's pass, on that thread).
bool shadow_feed(uint8_t kind, void* v, size_t n);
void shadow_saw(uint8_t kind, const void* v, size_t n);
bool shadow_on();                              // is any function in shadow mode? (the input hooks are needed)

// The renderer during a check, on this thread (gl_table.h, gl_core.h): shadow_com_phase() is 0 outside a check's
// passes (or while an output runs), 1 in the original's pass (OpenGL calls are made), 2 in the rewrite's pass
// (they're recorded, not made). shadow_com_effect records a call -- its arguments, and the data they point at
// (hashed past 64 bytes) -- in the pass's output log, which the check compares like its other outputs; the
// original's calls have already been made, so they aren't made again afterwards. shadow_com_check() counts
// checks (the recorder keys what it queues for the rewrite's pass to it).
int shadow_com_phase();
void shadow_com_effect(uint16_t method, const void* self, const void* args, size_t nargs, const void* data = 0,
                       size_t ndata = 0);
unsigned shadow_com_check();
// The renderer's own state (gl_core.cpp) is part of what a check compares: saved as a pass changes it, put back
// between the passes, compared after them, and left as the original's pass left it.
struct ShadowState {
    void (*begin)();                                // a check starts
    void (*after_original)();                       // keep the original's pass's results; put back the start
    bool (*differs)(char* where, size_t n);         // after the rewrite's pass: does it differ? (where: what)
    void (*end)(bool original_only);                // the check is over: the original's results stand
    const char* (*record_name)(uint16_t method);    // an output log's call, by name (0 if not the renderer's)
};
void shadow_set_state(const ShadowState* s);
// this pass did something a check can't do twice (start the renderer): in the original's pass, its result stands
// and the rewrite isn't run (as when it allocates); in the rewrite's pass, it's a mismatch
void shadow_keep_original(const char* what);
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
        template <PortFn* P, auto NEW, void (*FP)(Footprint&, Args...)>                                  \
        static R CC call(Args... a) {                                                                    \
            static_assert(std::is_same_v<decltype(NEW), Fn>, "the rewrite's own type");                  \
            if (!shadow_begin(P)) return ((Fn)P->orig)(a...);                                            \
            FP(shadow_footprint(), a...);                                                                \
            if (shadow_footprint().replay_only) { shadow_abandon(P); return ((Fn)P->orig)(a...); }       \
            shadow_snapshot();                                                                         \
            if constexpr (std::is_void_v<R>) {                                                           \
                ((Fn)P->orig)(a...);                                                                     \
                if (!shadow_after_original(P, 0, 0)) return;                                             \
                NEW(a...);                                                                               \
                shadow_finish(P, 0, 0);                                                                  \
            } else {                                                                                     \
                R ro = ((Fn)P->orig)(a...);                                                              \
                if (!shadow_after_original(P, &ro, sizeof ro)) return ro;                               \
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
// PORT_FN_BUILDS(..., hash, len) also hooks it on the race.bin builds (PORT_NEW only), whose tables say what their
// first bytes should be; hash and len: vp_code_hash (viperport.h) of v1.0's first len bytes.
#define VP_CAT2(a, b) a##b
#define VP_CAT(a, b) VP_CAT2(a, b)

#include "compiler.h"   // VP_GCC, VP_X87_CLOBBERS, VP_ASM_CALLS (MSVC and GCC builds)
#ifdef VP_FUZZ
// test/fuzz.cpp: the same rewrites, compiled into a test program that runs each pure one against the
// original on random inputs, outside the game (test/fuzz.h)
#include "../test/fuzz.h"
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, HASH, LEN)                                                     \
    static FuzzReg VP_CAT(fuzz_, NEW)(V10, NAME, &Fuzz<decltype(&NEW)>::template run<&NEW, &FP>);
#else
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, HASH, LEN)                                                     \
    namespace { extern PortFn VP_CAT(port_, NEW); }   /* file-local: rewrites in different files */     \
    namespace { PortFn VP_CAT(port_, NEW)(V10, NAME, (void*)&NEW,                                                \
        (void*)&Shadow<decltype(&NEW)>::call<&VP_CAT(port_, NEW), &NEW, &FP>, HASH, LEN); }
#endif
#define PORT_FN(V10, NAME, NEW, FP) PORT_FN_BUILDS(V10, NAME, NEW, FP, 0, 0)
// PORT_FN_GL: a rewrite that calls the OpenGL renderer directly (gl_core.h); with the game's own DirectDraw it
// stays original (port_install). A harness defines VP_PORT_NEEDS_RENDERER(NEW) as nothing.
#ifndef VP_PORT_NEEDS_RENDERER
#ifdef VP_FUZZ
#define VP_PORT_NEEDS_RENDERER(NEW)
#else
#define VP_PORT_NEEDS_RENDERER(NEW) namespace { const bool VP_CAT(needs_gl_, NEW) = (VP_CAT(port_, NEW).needs_renderer = true); }
#endif
#endif
#define PORT_FN_GL(V10, NAME, NEW, FP) PORT_FN(V10, NAME, NEW, FP) VP_PORT_NEEDS_RENDERER(NEW)
// PORT_FN_AUDIO: a rewrite that calls the SDL audio core directly (audio_core.h); with the game's own DirectSound it
// stays original (port_install). A harness defines VP_PORT_NEEDS_AUDIO(NEW) as nothing. The fuzzer doesn't register
// them at all: it runs pure rewrites only, and these call the audio core, which isn't linked into it.
#ifndef VP_PORT_NEEDS_AUDIO
#define VP_PORT_NEEDS_AUDIO(NEW) namespace { const bool VP_CAT(needs_audio_, NEW) = (VP_CAT(port_, NEW).needs_audio = true); }
#endif
#ifdef VP_FUZZ
#define PORT_FN_AUDIO(V10, NAME, NEW, FP)
#else
#define PORT_FN_AUDIO(V10, NAME, NEW, FP) PORT_FN(V10, NAME, NEW, FP) VP_PORT_NEEDS_AUDIO(NEW)
#endif

// ---- the framework ----------------------------------------------------------------------------------------
void port_check_stock();                       // before anything is patched: find non-stock functions
int port_unknown_patches(char* names, size_t n, int max);   // after it: those left original, named (viperport.exe --probe)
int port_vrmod_count();                        // after it: how many it accepted as vrmod's patches
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
