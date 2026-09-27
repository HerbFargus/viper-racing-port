// port.cpp -- M3's framework: rewritten functions switched per function, and shadow checks (port.h).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "viperport.h"
#include "port.h"

// ---- the registry (filled by PORT_FN's static constructors, before DllMain) --------------------------
static std::vector<PortFn*>& registry() {
    static std::vector<PortFn*> r;
    return r;
}

PortFn::PortFn(uint32_t v, const char* nm, void* r, void* s, const uint8_t* op, uint8_t ol)
    : v10(v), name(nm), repl(r), shadow(s), other_prologue(op), other_len(ol) {
    registry().push_back(this);
}

bool port_is_new(const PortFn& f) { return f.mode != PORT_ORIGINAL && f.orig; }

// ---- trampolines ---------------------------------------------------------------------------------------
struct Prologue { uint32_t v10; uint8_t len; int8_t rel; uint8_t bytes[16]; };
static const Prologue k_prologues[] = {
#include "prologues.inc"
};

static bool is_v10() { return build_is_v10(); }

static uint8_t* g_tramp_page;
static size_t g_tramp_used;

static void* make_trampoline(const Prologue& p) {
    if (p.rel == -2) return 0;                                    // branches in its first bytes (see below)
    // 32 bytes each, room for every hooked address (prologues.inc) and the harness's own detours; a full page is
    // logged by the caller, never silent
    enum { TRAMP_BYTES = 32 * 8192 };
    if (!g_tramp_page) g_tramp_page = (uint8_t*)VirtualAlloc(0, TRAMP_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp_page || g_tramp_used + 32 > TRAMP_BYTES) return 0;
    uint8_t* t = g_tramp_page + g_tramp_used;
    g_tramp_used += 32;
    memcpy(t, (const void*)p.v10, p.len);                         // the live bytes: M1's operands included
    if (p.rel >= 0) {                                             // a call/jmp rel32 moved: re-aim it
        int32_t d = *(int32_t*)(p.bytes + p.rel);
        uint32_t target = p.v10 + p.rel + 4 + d;
        *(int32_t*)(t + p.rel) = (int32_t)(target - (uint32_t)(t + p.rel + 4));
    }
    t[p.len] = 0xE9;                                              // then on into the rest of the original
    *(int32_t*)(t + p.len + 1) = (int32_t)((p.v10 + p.len) - (uint32_t)(t + p.len + 5));
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    return t;
}

static void write_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((uint8_t*)to - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

// M1's patches (viperport.cpp, patch_fields): an operand it moved can lie in a function's first bytes -- FileVerifyNoOpenFiles'
// `mov esi, <the open-file table>` -- so those bytes differ from prologues.inc's, and a trampoline must carry M1's value
static struct { uint32_t at, n; } g_m1[512];
static int g_nm1;
void port_note_m1(uint32_t at, uint32_t n) {
    if (g_nm1 < (int)(sizeof g_m1 / sizeof g_m1[0])) g_m1[g_nm1++] = {at, n};
    else logf("port: too many M1 patches noted (%08x)", at);
}
static bool m1_patched(uint32_t a) {
    for (int i = 0; i < g_nm1; i++)
        if (a >= g_m1[i].at && a < g_m1[i].at + g_m1[i].n) return true;
    return false;
}

// the prologue record for v10, if the live bytes are still v1.0's (apart from bytes M1 patched)
static const Prologue* live_prologue(uint32_t v10) {
    if (!is_v10()) return 0;
    for (const Prologue& p : k_prologues)
        if (p.v10 == v10) {
            for (int i = 0; i < p.len; i++)
                if (((const uint8_t*)v10)[i] != p.bytes[i] && !m1_patched(v10 + i)) return 0;
            return &p;
        }
    return 0;
}

void* detour(uint32_t v10, void* to, const char* what) {
    const Prologue* p = live_prologue(v10);
    if (!p) {
        logf("NOT hooking %s: not v1.0, or %08x isn't the code prologues.inc expects (rerun tools/gen_port_tables.py)", what, v10);
        return 0;
    }
    void* t = make_trampoline(*p);
    if (!t) { logf("NOT hooking %s: no room for its trampoline", what); return 0; }
    write_jmp(v10, to);
    return t;
}

void* detour_front(uint32_t v10, void* to, const char* what) {
    for (PortFn* f : registry()) {
        if (f->v10 != v10 || f->mode == PORT_ORIGINAL || !f->orig) continue;
        write_jmp(v10, to);                                       // in front of the rewrite port_install hooked
        return f->mode == PORT_SHADOW ? f->shadow : f->repl;
    }
    return detour(v10, to, what);
}

// ---- stock fingerprints (stock.inc) --------------------------------------------------------------------------
// A rewrite replaces only the exact stock v1.0 function it was written from: its code and every read-only
// constant it reads. On a race.exe with vrmod's patches (the hornball, say) a patched function stays
// original, so the patch keeps working. vrmod's two engine fixes (obstacle wake, the AI bead guard) are
// the exception: the rewrites carry those fixes themselves, so a function patched with exactly vrmod's fix
// (`vrmod`, the fingerprint of stock + that patch) is replaced like a stock one. Checked before anything
// is patched.
struct StockConst { uint32_t va, width; };
static const StockConst k_stock_consts[] = {
#define VP_STOCK_CONSTS
#include "stock.inc"
#undef VP_STOCK_CONSTS
};
struct Stock { uint32_t v10, size, hash_code, hash, first, nconst, vrmod; };
static const Stock k_stock[] = {
#define VP_STOCK
#include "stock.inc"
#undef VP_STOCK
};

static uint32_t fnv(const uint8_t* p, uint32_t n, uint32_t h) {
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void port_check_stock() {
    if (!is_v10()) return;
    for (PortFn* f : registry()) {
        for (const Stock& st : k_stock) {
            if (st.v10 != f->v10) continue;
            uint32_t hc = fnv((const uint8_t*)st.v10, st.size, 2166136261u), h = hc;
            for (uint32_t i = st.first; i < st.first + st.nconst; i++)
                h = fnv((const uint8_t*)k_stock_consts[i].va, k_stock_consts[i].width, h);
            if (st.vrmod && h == st.vrmod) {
                logf("port: %s has vrmod's engine fix; the rewrite fixes the same bug and replaces it (docs/FIXES.md)", f->name);
            } else if (h != st.hash) {
                f->patched = true;
                logf("port: %s stays original -- the installed race.exe has %s patched (a vrmod fix?)", f->name,
                     hc != st.hash_code ? "its code" : "a constant it reads");
            }
            break;
        }
    }
}

// ---- classes, sizes and field names (state_layout.inc) --------------------------------------------------
struct ClassInfo { uint32_t vtable, size; const char* name; int first, count; };
struct FieldInfo { uint32_t off, size; const char* name; };
static const ClassInfo k_classes[] = {
#define VP_CLASSES
#include "state_layout.inc"
#undef VP_CLASSES
};
static const FieldInfo k_fields[] = {
#define VP_FIELDS
#include "state_layout.inc"
#undef VP_FIELDS
};

const char* class_of(const void* obj, uint32_t* size) {
    uint32_t vt = 0;
    __try { vt = *(const uint32_t*)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = 0; }
    for (const ClassInfo& c : k_classes)
        if (c.vtable == vt) { if (size) *size = c.size; return c.name; }
    if (size) *size = 0;
    return 0;
}

const char* field_name(const char* cls, uint32_t off, char* buf, size_t n) {
    for (const ClassInfo& c : k_classes) {
        if (!cls || strcmp(c.name, cls)) continue;
        const FieldInfo* best = 0;
        for (int i = c.first; i < c.first + c.count; i++) {
            const FieldInfo& f = k_fields[i];
            if (f.off <= off && off < f.off + (f.size ? f.size : 4) && (!best || f.off >= best->off)) best = &f;
        }
        if (best && best->off == off) _snprintf(buf, n, "%s", best->name);
        else if (best) _snprintf(buf, n, "%s+%u", best->name, off - best->off);
        else _snprintf(buf, n, "+0x%x", off);
        buf[n - 1] = 0;
        return buf;
    }
    _snprintf(buf, n, "+0x%x", off);
    buf[n - 1] = 0;
    return buf;
}

// ---- footprints ----------------------------------------------------------------------------------------
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (!p || !bytes) return;
    if (n == MAX) { logf("shadow: footprint full, %s left out", what); return; }
    r[n++] = {p, bytes, what};
}

void Footprint::stack_ptr(void* p, const char* what) {
    add(p, 4, what);
    if (n && r[n - 1].p == p) r[n - 1].stack = true;
}

static bool on_this_stack(uint32_t v) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    return v >= (uint32_t)(uintptr_t)tib->StackLimit && v < (uint32_t)(uintptr_t)tib->StackBase;
}

void Footprint::object(void* obj, const char* what) {
    uint32_t size = 0;
    class_of(obj, &size);
    if (!size) { logf("shadow: %s at %p has an unknown class; its footprint is left out", what, obj); return; }
    add(obj, size, what);
}

// ---- the physics and AI globals (globals_phys.inc) -------------------------------------------------------
struct Global { uint32_t va, size; const char* name; };
static const Global k_globals[] = {
#include "globals_phys.inc"
};
static uint32_t g_globals_bytes;


// ---- heap blocks the physics owns through a global pointer -------------------------------------------
// State that lives on the heap but belongs to the simulation, reached through a global: saved, restored
// and compared like the globals. Found by shadow mismatches that pointed at a global with no write of its
// own in the function under test.
struct HeapBlock { uint32_t ptr_global, bytes; const char* name; };
static const HeapBlock k_heap_blocks[] = {
    // Collide()'s last 8 crash sounds (0x28 bytes each: time, the pair of volumes, ...); a crash within the
    // window of an entry for the same pair plays nothing, so the table decides whether the index advances
    {0x00521e30, 8 * 0x28, "collision sound table"},
};

// ---- outputs: calls that leave the simulation ---------------------------------------------------------------
// During a shadow check, calls to these are recorded and NOT run in either pass: running them would leave
// their own marks (a sound's position in the sound table, the replay recorder's clock stamp) in state the
// check compares, and play every crash twice. The two records are compared like any other result, then
// the original pass's calls are made for real, in order, once the check is done. That's safe because
// nothing in the simulation reads what they write within the same call: Collide()'s duplicate-crash test
// reads only the fields Collide itself writes before calling Play. Anything an output calls while it runs
// belongs to the output, and isn't recorded again.
enum OutPhase { OUT_OFF, OUT_NEW, OUT_ORIG };
enum { OUT_PLAY_VEL = 1, OUT_PLAY_F, OUT_ADD_EVENT, OUT_COM, OUT_MAX_DATA = 4096 };
static __declspec(thread) int t_out_phase;
static __declspec(thread) int t_in_output;
static __declspec(thread) std::vector<uint8_t>* t_out[3];
static volatile LONG g_outputs_deferred;

// record an output call, as [id][self][bytes: u16][payload]; true = not in a check: run it now
static bool output(uint8_t id, void* self, const void* a, size_t na, const void* b = 0, size_t nb = 0) {
    if (t_out_phase == OUT_OFF || t_in_output) return true;
    std::vector<uint8_t>& log = *t_out[t_out_phase];
    uint16_t len = (uint16_t)(na + nb);
    log.push_back(id);
    log.insert(log.end(), (const uint8_t*)&self, (const uint8_t*)&self + 4);
    log.insert(log.end(), (const uint8_t*)&len, (const uint8_t*)&len + 2);
    log.insert(log.end(), (const uint8_t*)a, (const uint8_t*)a + na);
    if (b && nb) log.insert(log.end(), (const uint8_t*)b, (const uint8_t*)b + nb);
    return false;
}

typedef void(__fastcall* PlayVel_t)(void* self, void* edx, const float* pos, const float* vel);
typedef void(__fastcall* PlayF_t)(void* self, void* edx, const float* pos, float loud);
typedef void(__cdecl* AddEvent_t)(int type, const void* data, int size);
static PlayVel_t o_play_vel;
static PlayF_t o_play_f;
static AddEvent_t o_add_event;

static void __fastcall out_play_vel(void* self, void* edx, const float* pos, const float* vel) {
    float pv[6] = {pos[0], pos[1], pos[2], vel[0], vel[1], vel[2]};
    if (!output(OUT_PLAY_VEL, self, pv, sizeof pv)) return;
    t_in_output++;
    o_play_vel(self, edx, pos, vel);
    t_in_output--;
}

static void __fastcall out_play_f(void* self, void* edx, const float* pos, float loud) {
    float pl[4] = {pos[0], pos[1], pos[2], loud};
    if (!output(OUT_PLAY_F, self, pl, sizeof pl)) return;
    t_in_output++;
    o_play_f(self, edx, pos, loud);
    t_in_output--;
}

static void __cdecl out_add_event(int type, const void* data, int size) {
    int ts[2] = {type, size};
    bool fits = size >= 0 && size <= OUT_MAX_DATA && (data || size == 0);
    if (fits && !output(OUT_ADD_EVENT, 0, ts, sizeof ts, data, size)) return;
    t_in_output++;
    o_add_event(type, data, size);                     // (an event too big to record runs in both passes)
    t_in_output--;
}

// ---- DirectDraw / Direct3D calls (port.h) ----
static __declspec(thread) unsigned t_com_checks;

int shadow_com_phase() {
    if (t_in_output) return 0;
    return t_out_phase == OUT_ORIG ? 1 : t_out_phase == OUT_NEW ? 2 : 0;
}

unsigned shadow_com_check() { return t_com_checks; }

void shadow_com_effect(uint16_t method, const void* self, const void* args, size_t nargs, const void* data,
                       size_t ndata) {
    if (shadow_com_phase() == 0) return;
    uint8_t rec[8 + 64 + 12];                          // method, the arguments, then the data or its hash
    size_t n = 0;
    memcpy(rec, &method, 2); n = 2;
    if (nargs > 64) nargs = 64;
    memcpy(rec + n, args, nargs); n += nargs;
    if (data && ndata <= 64) {
        output(OUT_COM, (void*)self, rec, n, data, ndata);
        return;
    }
    uint64_t h = 1469598103934665603ull;               // FNV-1a over the data
    for (size_t i = 0; data && i < ndata; i++) h = (h ^ ((const uint8_t*)data)[i]) * 1099511628211ull;
    uint32_t nd = (uint32_t)ndata;
    memcpy(rec + n, &nd, 4); n += 4;
    memcpy(rec + n, &h, 8); n += 8;
    output(OUT_COM, (void*)self, rec, n);
}

// make the original pass's recorded output calls, now that the check is over
static void run_outputs(const std::vector<uint8_t>& log) {
    size_t at = 0;
    while (at + 7 <= log.size()) {
        uint8_t id = log[at];
        void* self;
        uint16_t len;
        memcpy(&self, &log[at + 1], 4);
        memcpy(&len, &log[at + 5], 2);
        const uint8_t* p = &log[at + 7];
        at += 7 + len;
        if (at > log.size()) break;
        InterlockedIncrement(&g_outputs_deferred);
        t_in_output++;
        if (id == OUT_COM) { InterlockedDecrement(&g_outputs_deferred); t_in_output--; continue; }   // made live
        if (id == OUT_PLAY_VEL) o_play_vel(self, 0, (const float*)p, (const float*)p + 3);
        else if (id == OUT_PLAY_F) o_play_f(self, 0, (const float*)p, ((const float*)p)[3]);
        else if (id == OUT_ADD_EVENT) {
            int ts[2];
            memcpy(ts, p, 8);
            o_add_event(ts[0], ts[1] ? p + 8 : 0, ts[1]);
        }
        t_in_output--;
    }
}

// The heap, during a check: a function whose original allocates or frees can't be run again from the restored state
// (the rewrite would allocate a second block, or free the same one twice), so it's noted here and the check ends
// after the original (shadow_after_original); a rewrite that allocates where its original didn't is a mismatch.
typedef void*(__cdecl* MemAlloc_t)(int);
typedef void(__cdecl* MemFree_t)(void*);
static MemAlloc_t o_mem_alloc;
static MemFree_t o_mem_free;
static __declspec(thread) uint8_t t_heap;             // 1: the original's pass allocated or freed; 2: the rewrite's
static void note_heap() { t_heap |= t_out_phase == OUT_ORIG ? 1 : t_out_phase == OUT_NEW ? 2 : 0; }
static void* __cdecl heap_alloc(int n) { note_heap(); return o_mem_alloc(n); }
static void __cdecl heap_free(void* p) { note_heap(); o_mem_free(p); }

// A car's dents, during a check on another thread: Car::ActuallyApplyDamage and Car::reset_damage run on the physics
// thread and rewrite the car's live body models, which the main thread's drawing reads. A dent that lands between
// the original's pass and the rewrite's is a different input to the two passes, not a porting difference: such a
// check's difference is counted apart (PortFn::raced). The counter moves at the start and the end of each.
typedef void(__fastcall* Dent_t)(void*, void*, const void*, const void*, uint32_t, uint32_t);
typedef void(__fastcall* Undent_t)(void*, void*);
static Dent_t o_dent;
static Undent_t o_undent;
static volatile LONG g_dents;
static __declspec(thread) LONG t_dents_at;
static void __fastcall dent_front(void* car, void* edx, const void* force, const void* point, uint32_t rev, uint32_t no_dent) {
    InterlockedIncrement(&g_dents);
    o_dent(car, edx, force, point, rev, no_dent);
    InterlockedIncrement(&g_dents);
}
static void __fastcall undent_front(void* car, void* edx) {
    InterlockedIncrement(&g_dents);
    o_undent(car, edx);
    InterlockedIncrement(&g_dents);
}

static void install_outputs() {
    // detour(0x0043c560) detour(0x0043c510) detour(0x0042d8b0) -- listed for gen_port_tables.py
    o_play_vel = (PlayVel_t)detour_front(0x0043c560, (void*)out_play_vel, "CollisionSound::Play (output)");
    o_play_f = (PlayF_t)detour_front(0x0043c510, (void*)out_play_f, "CollisionSound::Play(float) (output)");
    o_add_event = (AddEvent_t)detour_front(0x0042d8b0, (void*)out_add_event, "PhysReplayAddEvent (output)");
    logf("shadow: outputs %s: crash sounds and the game's replay events are compared, then made once",
         o_play_vel && o_play_f && o_add_event ? "hooked" : "NOT all hooked");
    // detour(0x004140e0) detour(0x00414300) -- listed for gen_port_tables.py
    o_mem_alloc = (MemAlloc_t)detour_front(0x004140e0, (void*)heap_alloc, "MemAlloc (checks)");
    o_mem_free = (MemFree_t)detour_front(0x00414300, (void*)heap_free, "MemFree (checks)");
    logf("shadow: MemAlloc and MemFree %s: a check whose original allocates or frees keeps the original's result",
         o_mem_alloc && o_mem_free ? "hooked" : "NOT hooked");
    // detour(0x00436b40) detour(0x00439e50) -- listed for gen_port_tables.py
    o_dent = (Dent_t)detour_front(0x00436b40, (void*)dent_front, "Car::ActuallyApplyDamage (checks)");
    o_undent = (Undent_t)detour_front(0x00439e50, (void*)undent_front, "Car::reset_damage (checks)");
    logf("shadow: car damage %s: a difference while the physics thread dents a car's models is counted apart",
         o_dent && o_undent ? "hooked" : "NOT hooked");
}

// ---- shadow checks -------------------------------------------------------------------------------------
// One check at a time per thread; a rewritten function called inside a check runs its original (the
// rewrite under test is checked alone). Each check lists its spans -- the footprint, the physics and AI
// globals, the heap blocks -- and runs:
//   1. save them (`before`); the ORIGINAL runs, on the live inputs, which are logged
//   2. save its results (`after_orig`); restore `before`; the REWRITE runs, fed the logged inputs
//   3. compare the rewrite's results with the original's; restore the original's; make its outputs
// so the game always continues exactly as if only the original had run.
struct Span { uint8_t* p; uint32_t n; int kind; int index; };   // kind 0 footprint, 1 global, 2 heap block
static int g_shadow_every = 1, g_shadow_per_tick = 4;
static bool g_shadow_on;
static __declspec(thread) int t_depth;
static __declspec(thread) Footprint* t_fp;
static __declspec(thread) std::vector<Span>* t_spans;
static __declspec(thread) std::vector<uint8_t>* t_before;
static __declspec(thread) std::vector<uint8_t>* t_after_orig;
static __declspec(thread) std::vector<uint8_t>* t_inputs;     // [kind][n][bytes], as the original read them
static __declspec(thread) size_t t_input_at;
static __declspec(thread) const char* t_input_problem;
static __declspec(thread) uint8_t t_ret_orig[16];
static __declspec(thread) size_t t_ret_n;

bool shadow_on() { return g_shadow_on; }
volatile unsigned long g_physics_thread_id;

bool shadow_begin(PortFn* f) {
    LONG n = InterlockedIncrement(&f->calls);
    if (t_depth > 0 || (n - 1) % g_shadow_every) return false;
    if (g_shadow_per_tick) {                           // a budget per function per physics tick
        int tick = *(volatile int*)0x0052161c;         // physics_tick
        if (tick != f->budget_tick) { f->budget_tick = tick; f->budget_used = 0; }
        if (f->budget_used >= g_shadow_per_tick) return false;
        f->budget_used++;
    }
    if (!t_fp) {
        t_fp = new Footprint;
        t_spans = new std::vector<Span>;
        t_before = new std::vector<uint8_t>;
        t_after_orig = new std::vector<uint8_t>;
        t_inputs = new std::vector<uint8_t>;
        for (int i = 0; i < 3; i++) t_out[i] = new std::vector<uint8_t>;
    }
    t_depth++;
    t_fp->n = 0;
    t_fp->replay_only = 0;
    t_fp->pure = false;
    return true;
}

Footprint& shadow_footprint() { return *t_fp; }

static void save(std::vector<uint8_t>& to) {
    size_t total = 0;
    for (const Span& s : *t_spans) total += s.n;
    to.resize(total);
    uint8_t* w = to.data();
    for (const Span& s : *t_spans) { memcpy(w, s.p, s.n); w += s.n; }
}

static void restore(const std::vector<uint8_t>& from) {
    const uint8_t* r = from.data();
    for (const Span& s : *t_spans) { memcpy(s.p, r, s.n); r += s.n; }
}

// ---- inputs during a check ----
bool shadow_feed(uint8_t kind, void* v, size_t n) {
    if (t_out_phase != OUT_NEW) return false;
    std::vector<uint8_t>& q = *t_inputs;
    // an input the original didn't read here is a mismatch; the rewrite gets zero, and never the live
    // value (in a replay that would consume the recording)
    if (t_input_at + 2 + n > q.size()) {
        if (!t_input_problem) t_input_problem = "the rewrite read more inputs than the original";
        memset(v, 0, n);
        return true;
    }
    if (q[t_input_at] != kind || q[t_input_at + 1] != n) {
        if (!t_input_problem) t_input_problem = "the rewrite read a different input than the original at the same point";
        memset(v, 0, n);
        return true;
    }
    memcpy(v, &q[t_input_at + 2], n);
    t_input_at += 2 + n;
    return true;
}

void shadow_saw(uint8_t kind, const void* v, size_t n) {
    if (t_out_phase != OUT_ORIG) return;
    std::vector<uint8_t>& q = *t_inputs;
    q.push_back(kind);
    q.push_back((uint8_t)n);
    q.insert(q.end(), (const uint8_t*)v, (const uint8_t*)v + n);
}

void shadow_snapshot() {
    std::vector<Span>& sp = *t_spans;
    sp.clear();
    for (int i = 0; i < t_fp->n; i++) sp.push_back({(uint8_t*)t_fp->r[i].p, t_fp->r[i].n, 0, i});
    // a pure function (maths) changes no globals; a check on another thread mustn't touch the physics'
    if (!t_fp->pure && GetCurrentThreadId() == g_physics_thread_id) {
        for (int i = 0; i < (int)(sizeof k_globals / sizeof k_globals[0]); i++)
            sp.push_back({(uint8_t*)k_globals[i].va, k_globals[i].size, 1, i});
        for (int i = 0; i < (int)(sizeof k_heap_blocks / sizeof k_heap_blocks[0]); i++)
            if (uint8_t* p = *(uint8_t**)k_heap_blocks[i].ptr_global) sp.push_back({p, k_heap_blocks[i].bytes, 2, i});
    }
    save(*t_before);
    t_out[OUT_NEW]->clear();
    t_out[OUT_ORIG]->clear();
    t_inputs->clear();
    t_input_at = 0;
    t_input_problem = 0;
    t_com_checks++;
    t_heap = 0;
    t_dents_at = g_dents;
    t_out_phase = OUT_ORIG;
}

bool shadow_after_original(PortFn* f, const void* ret, size_t n) {
    t_out_phase = OUT_OFF;
    if (t_heap & 1) {
        InterlockedIncrement(&f->heap_skips);
        run_outputs(*t_out[OUT_ORIG]);                 // its sounds and events, made once
        t_depth--;
        return false;
    }
    save(*t_after_orig);
    t_ret_n = n < sizeof t_ret_orig ? n : sizeof t_ret_orig;
    if (ret) memcpy(t_ret_orig, ret, t_ret_n);
    restore(*t_before);
    t_out_phase = OUT_NEW;
    return true;
}

enum { MAX_LOGGED = 20 };

static void describe(const Span& s, uint32_t k, char* where, size_t n) {
    uint32_t w = k & ~3u;
    if (s.kind == 0) {
        uint32_t sz = 0;
        const char* cls = class_of(s.p, &sz);
        char fld[96];
        if (cls) field_name(cls, w, fld, sizeof fld); else _snprintf(fld, sizeof fld, "+0x%x", w);
        fld[sizeof fld - 1] = 0;
        _snprintf(where, n, "%s (%s) %s", t_fp->r[s.index].what, cls ? cls : "?", fld);
    } else if (s.kind == 1) {
        _snprintf(where, n, "global %s+0x%x (%08x)", k_globals[s.index].name, w, (uint32_t)s.p + w);
    } else {
        _snprintf(where, n, "%s +0x%x", k_heap_blocks[s.index].name, w);
    }
    where[n - 1] = 0;
}

void shadow_finish(PortFn* f, const void* ret, size_t n) {
    t_out_phase = OUT_OFF;
    InterlockedIncrement(&f->checks);
    // the live state is now the rewrite's result; after_orig holds the original's
    char where[160] = "";
    uint32_t orig = 0, rewrite = 0;
    bool differs = ret && memcmp(ret, t_ret_orig, t_ret_n) != 0;
    if (differs) {
        _snprintf(where, sizeof where, "the return value");
        memcpy(&orig, t_ret_orig, n < 4 ? n : 4);
        memcpy(&rewrite, ret, n < 4 ? n : 4);
    }
    const uint8_t* saved = t_after_orig->data();
    for (const Span& s : *t_spans) {
        if (differs) break;
        if (memcmp(s.p, saved, s.n) && s.kind == 0 && t_fp->r[s.index].stack) {
            uint32_t a, b;
            memcpy(&a, s.p, 4), memcpy(&b, saved, 4);
            if (on_this_stack(a) && on_this_stack(b)) { saved += s.n; continue; }
        }
        if (memcmp(s.p, saved, s.n)) {
            uint32_t k = 0;
            while (s.p[k] == saved[k]) k++;
            describe(s, k, where, sizeof where);
            memcpy(&orig, saved + (k & ~3u), 4);
            memcpy(&rewrite, s.p + (k & ~3u), 4);
            differs = true;
        }
        saved += s.n;
    }
    if (!differs && (t_input_problem || t_input_at != t_inputs->size())) {
        _snprintf(where, sizeof where, "its inputs: %s", t_input_problem ? t_input_problem : "the rewrite read fewer inputs than the original");
        where[sizeof where - 1] = 0;
        differs = true;
    }
    if (!differs && (t_heap & 2)) {
        _snprintf(where, sizeof where, "the heap: the rewrite allocated or freed memory, the original didn't");
        differs = true;
    }
    if (!differs && *t_out[OUT_NEW] != *t_out[OUT_ORIG]) {
        std::vector<uint8_t>&a = *t_out[OUT_ORIG], &b = *t_out[OUT_NEW];
        size_t k = 0;
        while (k < a.size() && k < b.size() && a[k] == b[k]) k++;
        _snprintf(where, sizeof where, "its outputs (sounds, replay events, DirectX calls): %u bytes of calls vs %u, first differing at byte %u",
                  (unsigned)a.size(), (unsigned)b.size(), (unsigned)k);
        where[sizeof where - 1] = 0;
        differs = true;
    }
    if (differs && GetCurrentThreadId() != g_physics_thread_id && g_dents != t_dents_at) {
        InterlockedIncrement(&f->raced);             // a dent landed during the check: the passes saw different models
        differs = false;
    }
    if (differs) {
        LONG m = InterlockedIncrement(&f->mismatches);
        if (m <= MAX_LOGGED) {
            float fo, fr;
            memcpy(&fo, &orig, 4); memcpy(&fr, &rewrite, 4);
            logf("shadow MISMATCH %s, call %ld: %s -- original %08x (%g), rewrite %08x (%g)%s",
                 f->name, f->calls, where, orig, fo, rewrite, fr, m == MAX_LOGGED ? " (no more logged for it)" : "");
        }
    }
    restore(*t_after_orig);                            // the game continues on the original's result
    run_outputs(*t_out[OUT_ORIG]);                     // and its sounds and events, made once
    t_depth--;
}


void shadow_abandon(PortFn* f) {
    t_out_phase = OUT_OFF;
    static volatile LONG once;
    if (InterlockedIncrement(&once) == 1)
        logf("shadow: %s can't be shadow-checked (%s); the original runs -- check it with a replay", f->name, t_fp->replay_only);
    t_depth--;
}

// ---- install and report ---------------------------------------------------------------------------------
static const char* mode_name(PortMode m) { return m == PORT_NEW ? "new" : m == PORT_SHADOW ? "shadow" : "original"; }

static PortMode parse_mode(const char* s, PortMode dflt) {
    if (!_stricmp(s, "new")) return PORT_NEW;
    if (!_stricmp(s, "original")) return PORT_ORIGINAL;
    if (!_stricmp(s, "shadow")) return PORT_SHADOW;
    return dflt;
}

void port_install(const char* ini) {
    for (const Global& g : k_globals) g_globals_bytes += g.size;
    char buf[64];
    GetPrivateProfileStringA("port", "default", "new", buf, sizeof buf, ini);
    PortMode dflt = parse_mode(buf, PORT_NEW);
    g_shadow_every = GetPrivateProfileIntA("port", "shadow_every", 1, ini);
    if (g_shadow_every < 1) g_shadow_every = 1;
    g_shadow_per_tick = GetPrivateProfileIntA("port", "shadow_per_tick", 4, ini);
    if (g_shadow_per_tick < 0) g_shadow_per_tick = 0;
    int on = 0;
    // M1's texture lift (viperport.cpp) is carried by these rewrites: the originals would write past the stock
    // 120 buckets, so they are always new, whatever the ini says (the rewrites read the bucket array from the
    // operands M1 patches, so without the lift they are the original exactly)
    static const char* const k_always_new[] = {"add_deferred_surf", "end_deferred_surfs", "draw_alpha_deferred_surfs"};
    for (PortFn* f : registry()) {
        GetPrivateProfileStringA("port", f->name, "", buf, sizeof buf, ini);
        f->mode = buf[0] ? parse_mode(buf, dflt) : dflt;
        for (const char* n : k_always_new)
            if (!strcmp(f->name, n) && f->mode != PORT_NEW) {
                logf("port: %s stays new (it carries M1's texture lift)", f->name);
                f->mode = PORT_NEW;
            }
        if (f->mode == PORT_ORIGINAL) { logf("port: %s -- original", f->name); continue; }
        if (f->patched) { f->mode = PORT_ORIGINAL; continue; }             // logged by port_check_stock
        const Prologue* p = live_prologue(f->v10);
        if (p && p->rel == -2) {
            // a short branch in its first 5 bytes: it can be replaced, but there's no trampoline to run the
            // original, so no shadow check -- a replay with it on (new) is its check
            if (f->mode == PORT_SHADOW) {
                logf("port: %s stays original in shadow mode (it branches in its first bytes; check it with a replay)", f->name);
                f->mode = PORT_ORIGINAL;
                continue;
            }
            write_jmp(f->v10, f->repl);
            f->orig = (void*)1;                                     // in force; the original is never called
        } else if (p) {                                             // v1.0: every mode
            f->orig = make_trampoline(*p);
            if (!f->orig) {
                logf("port: %s stays original (no room for its trampoline)", f->name);
                f->mode = PORT_ORIGINAL;
                continue;
            }
            write_jmp(f->v10, f->mode == PORT_SHADOW ? f->shadow : f->repl);
        } else if (!is_v10() && f->other_prologue && f->mode == PORT_NEW) {
            // another build: new only, after the per-build check of its first bytes
            if (!jmp_hook(f->v10, f->other_prologue, f->other_len, f->repl, f->name)) continue;
            f->orig = (void*)1;                                     // marks it in force; never called
        } else {
            logf("port: %s stays original (%s needs v1.0 and its prologue in prologues.inc)", f->name, mode_name(f->mode));
            f->mode = PORT_ORIGINAL;
            continue;
        }
        on++;
        logf("port: %s -- %s%s", f->name, mode_name(f->mode),
             f->mode == PORT_SHADOW ? " (sampled: shadow_every, shadow_per_tick)" : "");
    }
    logf("port: %d of %d rewritten functions in force; shadow checks save %u bytes of physics and AI globals",
         on, (int)registry().size(), g_globals_bytes);
    for (PortFn* f : registry())
        if (f->mode == PORT_SHADOW && f->orig) { g_shadow_on = true; install_outputs(); break; }
}

void port_report() {
    for (PortFn* f : registry())
        if (f->mode == PORT_SHADOW)
            logf("exit: shadow %s: %ld calls, %ld checked, %ld mismatches%s%s%s", f->name, f->calls, f->checks,
                 f->mismatches, f->checks && !f->mismatches ? " -- identical to the original" : "",
                 f->heap_skips ? " (and some calls allocated or freed memory: left to the replay)" : "",
                 f->raced ? " (and some differed while the physics thread dented a car mid-check: not counted)" : "");
    if (g_outputs_deferred)
        logf("exit: shadow: %ld sounds and replay events were compared between the passes, then made once", g_outputs_deferred);
}
