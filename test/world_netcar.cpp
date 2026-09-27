// world_netcar.cpp -- the network car's rewrites (hook/phys_netcar.cpp) against the originals, on simulated
// network races, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_netcar.cpp
//        /Fo<outdir>\ /Fe<outdir>\world_netcar.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_netcar.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. Each world
// builds a NetCar with the ORIGINAL constructor (Car::Car stubbed: the Car part is then given random plausible
// state), a remote sender flying a random trajectory (position a parabola that changes, orientation a spin), and
// runs a few hundred physics ticks: the sender's packets arrive through a random network -- latency and jitter
// (so out of order), drops, duplicates, silences long enough to abandon the car, teleport-sized jumps with and
// without the teleport flag, NaN / inf / denormal / huge fields -- each through NewPacket, then NetCar::Update, and
// now and then any other function of the file on the state as it stands (the private ones included; knocks of
// either size through ResolveExternalImpulse). Some worlds are loopbacks (MultiEnabled false: car 0's packets
// through the DelayQueue, with the stack word the original reads as Dequeue's capacity set per call), and
// MultiEnabled / the game state / replay mode change within a world.
//
// Every call is a check: snapshot the arena and all of race.exe's .data/.bss, scribble the stack, run the original,
// keep its result, restore, scribble the stack differently, run the rewrite, and compare the arena, .data/.bss,
// the return value and the stubs' call logs bit for bit; the bytes the original changed must lie inside the
// rewrite's footprint (unless replay_only). The world then goes on from the original's result. Half the worlds
// run CHAINED: during the rewrite's pass every one of the file's 31 original addresses jumps to its rewrite, so the
// rewrites run end to end against the originals end to end. Worlds alternate the x87 between single precision
// (the physics thread) and double.
//
// Stubbed (a jump to a logger; results from a scripted stream replayed identically for both passes, or from the
// world): MemAlloc (a bump arena inside the world), operator delete, Car::Car / ~Car / Update (moves the car,
// sometimes lands it: calls DoneLowering through the vtable) / Teleport / the Set* controls / ApplyExternalForce /
// ResolveExternalImpulse / ActuallyApplyDamage / MakeReplayPacket, PhysicsGetTime (the world's clock), PTimeNow,
// RaceDeity::RegisterCar, AIRegisterNetCar / MultiRegisterNetCar / MultiUnregisterNetCar / AIUnregisterCar,
// GetBall, Ball::Throw, PhysTaskFindCar, the DelayQueue (a FIFO in the world), atexit, loc_is_out_of_bounds.
// The game's own: Slerp, VectorLength, memmove, MakeNetPacket / MakePhysicsPacket (and the time conversions),
// LocalCar::FillNetPacket, MatrixNormalize, MultiEnabled, GetGameState, PhysReplayPlayMode, PhysicsIsDamageOn.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <type_traits>
#include "../hook/port.h"

#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

// ---- what the rewrites link against, standing in for the DLL ------------------------------------------
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
uint32_t A(uint32_t v10) { return v10; }
bool have(uint32_t) { return true; }
bool build_is_v10() { return true; }
static int g_fp_unknown;
static void* g_ball_vt[4];
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    if (vt == 0x004dc2e8) add(obj, 3896, what);            // NetCar (state_layout.inc)
    else if (vt == (uint32_t)(uintptr_t)g_ball_vt) add(obj, 64, what);
    else { printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt); g_fp_unknown++; }
}

#include "../hook/phys_netcar.cpp"

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_state = 0x2545f491u;
static uint32_t rnd() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float special() {
    switch (rnd() % 9) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));               // quiet NaN
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));              // signalling NaN
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));          // inf
    case 4: return fbits(rnd() & 0x807fffffu);                           // denormal
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits((rnd() & 0x80000000u) | 0x7f7fffffu);           // the largest
    case 7: return rf(-1e30f, 1e30f);
    default: return fbits(rnd());
    }
}
static int g_special_pct;
static float sp(float x) { return (int)(rnd() % 1000) < g_special_pct ? special() : x; }

// the scripted stream the stubs draw from: reset to the same seed before the original and the rewrite
static uint32_t g_script = 1;
static uint32_t script() {
    g_script ^= g_script << 13;
    g_script ^= g_script >> 17;
    g_script ^= g_script << 5;
    return g_script;
}

// ---- the original, loaded at 0x400000 (as test/fuzz.cpp) ---------------------------------------------------
static bool load_race_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* file = (uint8_t*)malloc(n);
    fread(file, 1, n, f);
    fclose(f);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(file + ((IMAGE_DOS_HEADER*)file)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x362de68c) { printf("%s isn't the v1.0 race.exe\n", path); return false; }
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, nt->OptionalHeader.SizeOfImage, MEM_COMMIT,
                                           PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) { printf("0x400000 isn't reserved for race.exe in this process\n"); return false; }
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        memcpy(base + sec[i].VirtualAddress, file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData
                                                                                         : sec[i].Misc.VirtualSize);
    free(file);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_WORLD_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) return 2;
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
static void patch_jump(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uint32_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the world ---------------------------------------------------------------------------------------------------
// the arena: everything the world owns (the bump heap too, so both passes allocate the same addresses)
enum : uint32_t {
    ARENA_N = 0x40000,
    A_HDR = 0x0000,                    // u32 bump offset, u32 fifo head, u32 fifo tail
    A_NETCAR = 0x0100,                 // the NetCar under test
    A_SCRATCH_CAR = 0x1100,            // constructor / destructor checks
    A_CAR0 = 0x2100,                   // car 0, a LocalCar (the loopback's sender)
    A_DEITY = 0x3100,                  // a RaceDeity-shaped block (2012)
    A_BALLS = 0x3900,                  // 8 fake balls, 64 bytes
    A_PKTS = 0x3b00,                   // 8 packet buffers, 0x40 apart (NewPacket / munge_packet arguments)
    A_VEC = 0x3d00,                    // P3 / Q4 / Stiffness scratch
    A_FIFO = 0x3e00,                   // the loopback DelayQueue: 32 x {u32 due, 0x3e packet, pad} (0x44)
    FIFO_N = 32, FIFO_STRIDE = 0x44,
    A_REPLAY = 0x4700,                 // replay packet (0x3c)
    A_WINGS = 0x4800,                  // 4 wings, 64 bytes
    A_HEAP = 0x5000,
};
static uint8_t* g_arena;
static uint8_t* const DATA = (uint8_t*)0x004e1000;           // race.exe .data/.bss
enum { DATA_BYTES = 0xf5f2c };
static uint32_t& bump() { return *(uint32_t*)(g_arena + A_HDR); }
static uint32_t& fifo_head() { return *(uint32_t*)(g_arena + A_HDR + 4); }
static uint32_t& fifo_tail() { return *(uint32_t*)(g_arena + A_HDR + 8); }
static NetCar* netcar() { return (NetCar*)(g_arena + A_NETCAR); }
static NetCar* scratch_car() { return (NetCar*)(g_arena + A_SCRATCH_CAR); }
static uint8_t* car0() { return g_arena + A_CAR0; }
static CarPhysicsPacket* pkt_buf(int i) { return (CarPhysicsPacket*)(g_arena + A_PKTS + (i & 7) * 0x40); }
static uint8_t* ball(int i) { return g_arena + A_BALLS + (i & 7) * 64; }

// the world's clock and switches (constant within a check)
static float g_now;
static uint32_t g_ptime_base;
static bool g_out_of_bounds;
static int g_ball_mode;                                // 0: no balls, 1: a ball per car, 2: some

static const char* field_name_net(uint32_t off, char* buf);
static const char* where(const uint8_t* p, char* buf) {
    if (p >= g_arena && p < g_arena + ARENA_N) {
        uint32_t off = (uint32_t)(p - g_arena);
        struct { uint32_t o, n; const char* name; } parts[] = {
            {A_HDR, 0x100, "hdr"}, {A_NETCAR, 0x1000, "netcar"}, {A_SCRATCH_CAR, 0x1000, "scratch car"},
            {A_CAR0, 0x1000, "car0"}, {A_DEITY, 0x800, "deity"}, {A_BALLS, 0x200, "balls"}, {A_PKTS, 0x200, "pkts"},
            {A_VEC, 0x100, "vec"}, {A_FIFO, 0x900, "fifo"}, {A_REPLAY, 0x100, "replay"}, {A_WINGS, 0x100, "wings"},
            {A_HEAP, ARENA_N - A_HEAP, "heap"}};
        for (auto& q : parts)
            if (off >= q.o && off < q.o + q.n) {
                if (q.o == A_NETCAR) {
                    char fb[96];
                    const char* fn = field_name_net(off - q.o, fb);
                    sprintf(buf, "netcar+0x%x%s%s", off - q.o, fn ? " " : "", fn ? fn : "");
                } else {
                    sprintf(buf, "%s+0x%x", q.name, off - q.o);
                }
                return buf;
            }
        sprintf(buf, "arena+0x%x", off);
        return buf;
    }
    sprintf(buf, ".data 0x%08x", (unsigned)(uintptr_t)p);
    return buf;
}

// ---- the stubs' log ------------------------------------------------------------------------------------------------
enum { LOG_MAX = 16384 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void lg(uint32_t w) { if (g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]++] = w; }
static void lg_bytes(const void* p, int n) { lg((uint32_t)n); for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, (const uint8_t*)p + i, n - i < 4 ? n - i : 4); lg(w); } }
static void lg_p3(const void* p) { lg_bytes(p, 12); }

// allocations
static void* __cdecl st_MemAlloc(int n) {
    lg('MALC'); lg((uint32_t)n);
    uint32_t at = (bump() + 15) & ~15u;
    if (at + (uint32_t)n > ARENA_N) { printf("arena full\n"); exit(3); }
    bump() = at + (uint32_t)n;
    uint8_t* p = g_arena + at;
    for (int i = 0; i < n; i++) p[i] = (uint8_t)script();          // not zeroed
    return p;
}
static void __cdecl st_delete(void* p) { lg('DELE'); lg((uint32_t)p); }
// the Car
static void* __fastcall st_Car_Car(void* self, int, void* data, void* p) {
    lg('CCTR'); lg((uint32_t)self); lg((uint32_t)data); lg((uint32_t)p);
    *(uint32_t*)self = 0x004dbfd0;
    return self;
}
static void __fastcall st_Car_dtor(void* self, int) { lg('CDTR'); lg((uint32_t)self); }
static void __fastcall st_Car_Update(NetCar* self, int) {
    lg('CUPD'); lg((uint32_t)self);
    // a stand-in for the physics: gravity and motion, and now and then touch-down after a teleport
    self->velocity.y = self->velocity.y - 0.157f;
    self->pos.x = self->pos.x + self->velocity.x * 0.016f;
    self->pos.y = self->pos.y + self->velocity.y * 0.016f;
    self->pos.z = self->pos.z + self->velocity.z * 0.016f;
    if (self->lowering && script() % 5 == 0) {
        self->lowering = 0;
        VFN(self, 0x50, void)(self, 0);                          // DoneLowering (through the vtable)
    }
}
static void __fastcall st_Car_Teleport(NetCar* self, int, const P3* pos, const float* dir) {
    lg('TELP'); lg((uint32_t)self); lg_p3(pos); lg_bytes(dir, 8);
    memcpy(&self->pos, pos, 12);
    self->lowering = 1;
}
template <uint32_t TAG> static void __fastcall st_set_u(void* self, int, uint32_t v) { lg(TAG); lg((uint32_t)self); lg(v); }
static void __fastcall st_SetHorn(void* self, int, uint32_t v) { lg('HORN'); lg((uint32_t)self); lg(v & 0xff); }
template <uint32_t TAG> static void __fastcall st_force(void* self, int, const P3* a, const P3* b, int32_t s) {
    lg(TAG); lg((uint32_t)self); lg_p3(a); lg_p3(b); lg((uint32_t)s);
}
static void __fastcall st_damage(void* self, int, const P3* a, const P3* b, uint32_t c, uint32_t d) {
    lg('DAMG'); lg((uint32_t)self); lg_p3(a); lg_p3(b); lg(c); lg(d);
}
static void __fastcall st_make_replay(void* self, int, uint8_t* p) {
    lg('MKRP'); lg((uint32_t)self); lg((uint32_t)p);
    for (int i = 0; i < 0x3c; i++) p[i] = (uint8_t)script();
}
// time
static double __cdecl st_PhysicsGetTime() { lg('TIME'); return g_now; }
static int __cdecl st_PTimeNow() { lg('PTIM'); return (int)g_ptime_base + (int)(g_now * 1000.0f); }
// registration
static void __fastcall st_RegisterCar(void* self, int, int32_t i, void* car) { lg('DREG'); lg((uint32_t)self); lg((uint32_t)i); lg((uint32_t)car); }
template <uint32_t TAG> static void __cdecl st_reg(void* c, int32_t i) { lg(TAG); lg((uint32_t)c); lg((uint32_t)i); }
static void __cdecl st_MultiUnregisterNetCar(void* c) { lg('MUNR'); lg((uint32_t)c); }
// balls
static void* __cdecl st_GetBall(int32_t i) {
    lg('GBAL'); lg((uint32_t)i);
    if (g_ball_mode == 0) return 0;
    if (g_ball_mode == 2 && (i & 1)) return 0;
    return ball(i);
}
static void __fastcall st_Ball_Throw(void* self, int, const void* frame, const P3* vel, uint32_t t) {
    lg('THRW'); lg((uint32_t)self); lg((uint32_t)frame); lg((uint32_t)vel); lg(t);
}
static void* __cdecl st_PhysTaskFindCar(int32_t i) { lg('FIND'); lg((uint32_t)i); return car0(); }
static int __cdecl st_atexit(void* fn) { lg('ATEX'); lg((uint32_t)fn); return 0; }
static uint8_t __cdecl st_out_of_bounds(const P3*) { return g_out_of_bounds; }
// the DelayQueue: a FIFO in the arena that behaves as the real one (a packet is due after a scripted delay;
// Dequeue copies it only if the capacity is enough, and drops it either way)
static void* __fastcall st_DQ_ctor(void* self, int, int a, int b) {
    lg('DQCT'); lg((uint32_t)self); lg((uint32_t)a); lg((uint32_t)b);
    *(uint32_t*)self = script() % 8 ? 0x5eed0000u : 0;             // the pool (0: couldn't be made)
    return self;
}
static void __fastcall st_DQ_dtor(void* self, int) { lg('DQDT'); lg((uint32_t)self); }
static unsigned char __fastcall st_DQ_Enqueue(void* self, int, const void* p, int n, uint32_t tag) {
    lg('DQEN'); lg((uint32_t)self); lg_bytes(p, n); lg(tag);
    if (fifo_tail() - fifo_head() >= FIFO_N) return 0;
    uint8_t* e = g_arena + A_FIFO + (fifo_tail() % FIFO_N) * FIFO_STRIDE;
    *(uint32_t*)e = st_PTimeNow() + (int)(script() % 200);
    memcpy(e + 4, p, n < 0x3e ? n : 0x3e);
    fifo_tail()++;
    return 1;
}
static unsigned char __fastcall st_DQ_Dequeue(void* self, int, void* out, int32_t* cap, uint32_t* tag) {
    lg('DQDE'); lg((uint32_t)self); lg((uint32_t)*cap); lg((uint32_t)tag);
    if (fifo_head() == fifo_tail()) return 0;
    uint8_t* e = g_arena + A_FIFO + (fifo_head() % FIFO_N) * FIFO_STRIDE;
    if (*(int32_t*)e > st_PTimeNow()) return 0;
    unsigned char ok = *cap >= 0x3e;
    if (ok) { *cap = 0x3e; memcpy(out, e + 4, 0x3e); }
    fifo_head()++;
    return ok;
}

static void install_stubs() {
    patch_jump(0x004140e0, (void*)st_MemAlloc);
    patch_jump(0x00414390, (void*)st_delete);
    patch_jump(0x004364c0, (void*)st_Car_Car);
    patch_jump(0x00436920, (void*)st_Car_dtor);
    patch_jump(0x00437680, (void*)st_Car_Update);
    patch_jump(0x00439b50, (void*)st_Car_Teleport);
    patch_jump(0x00439620, (void*)st_set_u<'STER'>);
    patch_jump(0x004396d0, (void*)st_set_u<'THRO'>);
    patch_jump(0x004396e0, (void*)st_set_u<'BRAK'>);
    patch_jump(0x004396f0, (void*)st_set_u<'CLUT'>);
    patch_jump(0x00439700, (void*)st_set_u<'EBRK'>);
    patch_jump(0x00439710, (void*)st_set_u<'GEAR'>);
    patch_jump(0x0043afb0, (void*)st_SetHorn);
    patch_jump(0x00436a90, (void*)st_force<'AEXF'>);
    patch_jump(0x00437120, (void*)st_force<'REXI'>);
    patch_jump(0x00436b40, (void*)st_damage);
    patch_jump(0x00438cd0, (void*)st_make_replay);
    patch_jump(0x0042bc80, (void*)st_PhysicsGetTime);
    patch_jump(0x00413b40, (void*)st_PTimeNow);
    patch_jump(0x00443010, (void*)st_RegisterCar);                      // RaceDeity::RegisterCar
    patch_jump(0x0041d2a0, (void*)st_reg<'AIRN'>);
    patch_jump(0x004a27a0, (void*)st_reg<'MURN'>);
    patch_jump(0x0041d2e0, (void*)st_reg<'AIUN'>);
    patch_jump(0x004a2800, (void*)st_MultiUnregisterNetCar);
    patch_jump(0x004410e0, (void*)st_GetBall);
    patch_jump(0x004412a0, (void*)st_Ball_Throw);
    patch_jump(0x00426d40, (void*)st_PhysTaskFindCar);
    patch_jump(0x004ceff0, (void*)st_atexit);
    patch_jump(0x00438660, (void*)st_out_of_bounds);
    patch_jump(0x004a4000, (void*)st_DQ_ctor);
    patch_jump(0x004a4060, (void*)st_DQ_dtor);
    patch_jump(0x004a4090, (void*)st_DQ_Enqueue);
    patch_jump(0x004a4130, (void*)st_DQ_Dequeue);
}

// ---- chaining: every original address of the file jumps to its rewrite (the rewrite's pass only) --------------
struct Hook { uint32_t addr; void* fn; uint8_t saved[5]; };
static Hook g_hooks[] = {
    {addr_interpolate_v_vec, (void*)&interpolate_v_vec}, {addr_interpolate_v_float, (void*)&interpolate_v_float},
    {addr_packet_is_too_old, (void*)&packet_is_too_old}, {addr_munge_packet, (void*)&munge_packet},
    {addr_get_quat_extrapolation, (void*)&get_quat_extrapolation}, {addr_wheels_on_bumpy, (void*)&wheels_on_bumpy},
    {addr_Delta_ctor, (void*)&Delta_ctor}, {addr_CarPhysicsPacket_ctor, (void*)&CarPhysicsPacket_ctor},
    {addr_Stiffness_copy, (void*)&Stiffness_copy}, {addr_NetCar_ctor, (void*)&NetCar_ctor},
    {addr_NetCar_dtor, (void*)&NetCar_dtor}, {addr_NetCar_we_are_abandoned, (void*)&NetCar_we_are_abandoned},
    {addr_NetCar_do_horn, (void*)&NetCar_do_horn}, {addr_NetCar_fade_alpha, (void*)&NetCar_fade_alpha},
    {addr_NetCar_be_abandoned, (void*)&NetCar_be_abandoned}, {addr_NetCar_NewPacket, (void*)&NetCar_NewPacket},
    {addr_NetCar_setup_interpolation, (void*)&NetCar_setup_interpolation},
    {addr_NetCar_GetPerceivedRPM, (void*)&NetCar_GetPerceivedRPM}, {addr_NetCar_teleport_to_p0, (void*)&NetCar_teleport_to_p0},
    {addr_NetCar_update_pos, (void*)&NetCar_update_pos}, {addr_NetCar_update_o, (void*)&NetCar_update_o},
    {addr_NetCar_update_stiffness, (void*)&NetCar_update_stiffness}, {addr_NetCar_go, (void*)&NetCar_go},
    {addr_NetCar_Update, (void*)&NetCar_Update}, {addr_NetCar_ApplyExternalForce, (void*)&NetCar_ApplyExternalForce},
    {addr_NetCar_ResolveExternalImpulse, (void*)&NetCar_ResolveExternalImpulse},
    {addr_NetCar_GetPerceivedThrottle, (void*)&NetCar_GetPerceivedThrottle},
    {addr_NetCar_MakeReplayPacket, (void*)&NetCar_MakeReplayPacket}, {addr_NetCar_ApplyDamage, (void*)&NetCar_ApplyDamage},
    {addr_NetCar_GetReplayPacketSize, (void*)&NetCar_GetReplayPacketSize}, {addr_NetCar_DoneLowering, (void*)&NetCar_DoneLowering},
};
enum { NHOOKS = sizeof g_hooks / sizeof g_hooks[0] };
static_assert(NHOOKS == 31, "the file's 31 rewrites");
static bool g_chain;
static void chain(bool on) {
    for (Hook& h : g_hooks) {
        if (on) { memcpy(h.saved, (void*)h.addr, 5); patch_jump(h.addr, h.fn); }
        else memcpy((void*)h.addr, h.saved, 5);
    }
}

// the NetCar's own fields, for the mismatch report
static const char* field_name_net(uint32_t off, char* buf) {
    struct F { uint32_t o, n; const char* name; };
    static const F fields[] = {
        {0x38, 36, "rot"}, {0x5c, 12, "pos"}, {0x234, 12, "velocity"}, {0x240, 12, "angular_velocity"},
        {0x50c, 4, "opacity"}, {0x538, 28, "aero"}, {0x554, 4 * 0x1a4, "wheels"}, {0xd70, 4, "perceived_rpm"},
        {0xe3d, 1, "lowering"}, {0xec0, 4, "packet_buffer"}, {0xec4, 4, "packets"}, {0xec8, 4, "num_packets"},
        {0xecc, 36, "vel[3]"}, {0xef0, 12, "accel"}, {0xefc, 16, "stiffness"}, {0xf0c, 16, "stiffness_rest"},
        {0xf1c, 4, "hit_time"}, {0xf20, 1, "settling"}, {0xf24, 4, "blend"}, {0xf28, 4, "blend_step"},
        {0xf2c, 4, "settle_time"}, {0xf30, 4, "loop_send_time"}, {0xf34, 1, "abandoned"}};
    for (const F& f : fields)
        if (off >= f.o && off < f.o + f.n) { sprintf(buf, "(%s+%u)", f.name, off - f.o); return buf; }
    return 0;
}

// ---- the sender: a remote car on a random trajectory, and the network between ----------------------------------
struct Pending { float due; CarPhysicsPacket p; };
struct Sender {
    double px, py, pz, vx, vy, vz, ax, ay, az;          // the trajectory now
    double yaw, yaw_rate, pitch, roll;
    float clock_offset;                                  // sender clock = g_now - offset (latency at send)
    float next_send;
    float silent_until;
    float latency, jitter;
    int drop_pct, dup_pct, late_pct, jump_pct, gap_pct, special_pct, same_time_pct;
    float steering, braking, rpm;
    int gear;
    uint8_t horn, wheel[4];
    Pending q[64];
    int nq;
};
static Sender S;

static void quat_from_euler(double yaw, double pitch, double roll, float* q) {
    double cy = cos(yaw * 0.5), sy = sin(yaw * 0.5), cp = cos(pitch * 0.5), spp = sin(pitch * 0.5), cr = cos(roll * 0.5), sr = sin(roll * 0.5);
    q[3] = (float)(cr * cp * cy + sr * spp * sy);
    q[0] = (float)(sr * cp * cy - cr * spp * sy);
    q[1] = (float)(cr * spp * cy + sr * cp * sy);
    q[2] = (float)(cr * cp * sy - sr * spp * cy);
}
static void quat_to_matrix(const float* q, float* m) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = (float)(1 - 2 * (y * y + z * z)); m[1] = (float)(2 * (x * y + w * z)); m[2] = (float)(2 * (x * z - w * y));
    m[3] = (float)(2 * (x * y - w * z)); m[4] = (float)(1 - 2 * (x * x + z * z)); m[5] = (float)(2 * (y * z + w * x));
    m[6] = (float)(2 * (x * z + w * y)); m[7] = (float)(2 * (y * z - w * x)); m[8] = (float)(1 - 2 * (x * x + y * y));
}

static void sender_init() {
    memset(&S, 0, sizeof S);
    S.px = rf(-500, 500); S.py = rf(0, 20); S.pz = rf(-500, 500);
    S.vx = rf(-60, 60); S.vy = rf(-2, 2); S.vz = rf(-60, 60);
    S.yaw = rf(-3.14f, 3.14f); S.yaw_rate = rf(-1, 1); S.pitch = rf(-0.2f, 0.2f); S.roll = rf(-0.2f, 0.2f);
    if (chance(15)) { S.pitch = rf(-3.14f, 3.14f); S.roll = rf(-3.14f, 3.14f); }        // upside down and all
    S.latency = chance(70) ? rf(0.02f, 0.15f) : rf(0.1f, 0.6f);
    S.jitter = chance(50) ? rf(0, 0.02f) : rf(0, 0.15f);
    S.drop_pct = chance(50) ? 0 : ri(1, 25);
    S.dup_pct = chance(50) ? 0 : ri(1, 15);
    S.late_pct = chance(50) ? 0 : ri(1, 15);
    S.jump_pct = chance(50) ? 0 : ri(1, 8);
    S.gap_pct = chance(50) ? 0 : ri(1, 3);
    S.special_pct = chance(60) ? 0 : ri(1, 4);
    S.same_time_pct = chance(70) ? 0 : ri(1, 10);
    S.clock_offset = chance(20) ? rf(-50, 50) : rf(-0.05f, 0.3f);
    S.next_send = g_now;
    S.gear = ri(-1, 6);
    S.rpm = rf(800, 7000);
}
static void sender_tick() {
    const double dt = 0.016;
    if (chance(2)) { S.ax = rf(-9, 9); S.ay = rf(-3, 3); S.az = rf(-9, 9); }
    if (chance(1)) { S.ax = rf(-40, 40); S.az = rf(-40, 40); }                       // a crash: more than 1 g
    S.vx += S.ax * dt; S.vy += S.ay * dt; S.vz += S.az * dt;
    S.px += S.vx * dt; S.py += S.vy * dt; S.pz += S.vz * dt;
    if (chance(3)) S.yaw_rate = rf(-2, 2);
    S.yaw += S.yaw_rate * dt;
    if (chance(4)) S.horn = (uint8_t)(chance(50) ? 0 : ri(1, 255));
    if (chance(1)) S.wheel[rnd() & 3] = (uint8_t)(chance(50) ? 0 : ri(1, 255));
    if (chance(5)) S.gear = ri(-1, 6);
    S.steering = rf(-1, 1); S.braking = chance(70) ? 0 : rf(0, 1); S.rpm = rf(700, 8000);
    if (g_now < S.silent_until) return;
    if (chance(S.gap_pct) && chance(10)) { S.silent_until = g_now + (chance(50) ? rf(0.3f, 1.9f) : rf(2.0f, 5.0f)); return; }
    if (g_now < S.next_send) return;
    S.next_send = g_now + (chance(90) ? 1.0f / 15 : rf(0, 0.2f));
    CarPhysicsPacket p;
    memset(&p, 0, sizeof p);
    p.recv_time = fbits(rnd());                          // the sender leaves it (the NetCar stamps it)
    p.time = g_now - S.clock_offset;
    if (S.same_time_pct && chance(S.same_time_pct) && S.nq) p.time = S.q[S.nq - 1].p.time;
    p.braking = S.braking; p.steering = S.steering; p.throttle = rf(0, 1); p.gear = S.gear; p.rpm = S.rpm;
    p.pos.x = (float)S.px; p.pos.y = (float)S.py; p.pos.z = (float)S.pz;
    quat_from_euler(S.yaw, S.pitch, S.roll, &p.rot.x);
    if (chance(20)) { p.rot.x = -p.rot.x; p.rot.y = -p.rot.y; p.rot.z = -p.rot.z; p.rot.w = -p.rot.w; }  // the same turn
    p.horn = S.horn;
    memcpy(p.wheel_broken, S.wheel, 4);
    if (chance(S.jump_pct)) {                            // a teleport-sized jump, flagged or not
        S.px += rf(-300, 300); S.pz += rf(-300, 300); S.py += rf(0, 30);
        p.pos.x = (float)S.px; p.pos.y = (float)S.py; p.pos.z = (float)S.pz;
        p.teleported = (uint8_t)(chance(50) ? (chance(80) ? 1 : ri(2, 255)) : 0);
    }
    if (chance(S.special_pct)) {                         // a wild field
        float* f[] = {&p.time, &p.braking, &p.steering, &p.rpm, &p.pos.x, &p.pos.y, &p.pos.z, &p.rot.x, &p.rot.y, &p.rot.z, &p.rot.w};
        *f[rnd() % 11] = special();
        if (chance(10)) p.gear = (int32_t)rnd();
    }
    if (chance(S.drop_pct)) return;
    int copies = chance(S.dup_pct) ? 2 : 1;
    for (int c = 0; c < copies && S.nq < 64; c++) {
        float due = g_now + S.latency + rf(0, S.jitter) + (chance(S.late_pct) ? rf(0.05f, 0.5f) : 0.0f) + c * rf(0, 0.1f);
        S.q[S.nq].due = due;
        S.q[S.nq].p = p;
        S.nq++;
    }
}

// ---- running one call both ways ----------------------------------------------------------------------------------
template <typename F, typename... Args> static uint64_t invoke(F f, Args... a) {
    typedef decltype(f(a...)) R;
    if constexpr (std::is_void_v<R>) {
        f(a...);
        return 0;
    } else if constexpr (std::is_floating_point_v<R>) {
        R r = f(a...);
        double d = (double)r;                              // an ST0 return: compare the register's value
        uint64_t u;
        memcpy(&u, &d, 8);
        return u;
    } else {
        R r = f(a...);
        uint64_t u = 0;
        memcpy(&u, &r, sizeof r);
        return u;
    }
}

static __declspec(noinline) void scribble_stack(uint32_t seed) {
    volatile uint32_t buf[4096];
    for (int i = 0; i < 4096; i++) { seed = seed * 1664525u + 1013904223u; buf[i] = seed; }
}

struct Stat {
    const char* name;
    long calls, changed, fails, fp_fails, fp_info, first_fail_world;
    const char* br_name[10];
    long br[10];
};
static Stat g_stats[64];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    Stat& s = g_stats[g_nstats++];
    memset(&s, 0, sizeof s);
    s.name = name;
    s.first_fail_world = -1;
    return s;
}
static void branch(Stat& s, const char* b) {
    for (int i = 0; i < 10; i++) {
        if (!s.br_name[i]) { s.br_name[i] = b; s.br[i] = 1; return; }
        if (!strcmp(s.br_name[i], b)) { s.br[i]++; return; }
    }
}
static uint8_t *g_data_start, *g_data_after, *g_arena_start, *g_arena_after;
static Footprint g_fp;
static int g_world;
static const char* g_running;

static void snapshot(uint8_t* d, uint8_t* a) { memcpy(d, DATA, DATA_BYTES); memcpy(a, g_arena, ARENA_N); }
static void restore(const uint8_t* d, const uint8_t* a) { memcpy(DATA, d, DATA_BYTES); memcpy(g_arena, a, ARENA_N); }

static int crash_filter(EXCEPTION_POINTERS* e, bool orig) {
    printf("CRASH in %s (%s, world %d): exception %08lx at %p\n", g_running, orig ? "original" : "rewrite", g_world,
           e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <typename Run> static uint64_t guarded(Run& run, bool orig) {
    __try {
        return run(orig);
    } __except (crash_filter(GetExceptionInformation(), orig)) {
        if (!orig && g_chain) chain(false);
        return 0xdeaddeaddeaddeadull;
    }
}
static bool log_has(uint32_t tag) {
    for (int i = 0; i < g_nlog[0]; i++) if (g_log[0][i] == tag) return true;
    return false;
}
static NetCar g_pre;                                     // the NetCar before the call (for coverage)
static uint8_t g_pre_teleported;                        // the newest packet's teleport flag before the call
static uint64_t g_ro;                                    // the original's return value (for coverage)
static const NetCar* post() { return (const NetCar*)(g_arena_after + A_NETCAR); }

template <typename Run, typename Fp, typename Cover>
static void check(const char* name, Run run, Fp footprint, Cover cover) {
    Stat& st = stat(name);
    st.calls++;
    memcpy(&g_pre, netcar(), sizeof(NetCar));
    g_pre_teleported = netcar()->packets ? netcar()->packets->teleported : 0;
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    g_pass = 0;
    footprint(g_fp);                                     // (its stub calls are logged and dropped)
    snapshot(g_data_start, g_arena_start);
    uint32_t seed = rnd() | 1;
    uint32_t junk = rnd();
    g_running = name;
    g_script = seed; g_pass = 0; g_nlog[0] = 0;
    scribble_stack(junk);
    uint64_t ro = guarded(run, true);
    snapshot(g_data_after, g_arena_after);
    restore(g_data_start, g_arena_start);
    g_script = seed; g_pass = 1; g_nlog[1] = 0;
    scribble_stack(~junk * 7);
    if (g_chain) chain(true);
    uint64_t rn = guarded(run, false);
    if (g_chain) chain(false);
    bool bad = ro != rn || g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], 4 * g_nlog[0]) != 0;
    bool changed = memcmp(g_data_after, g_data_start, DATA_BYTES) || memcmp(g_arena_after, g_arena_start, ARENA_N);
    int shown = 0;
    char w[128];
    auto compare = [&](const uint8_t* after, const uint8_t* start, const uint8_t* live, uint32_t n) {
        for (uint32_t i = 0; i < n; i += 4) {
            if (!memcmp(after + i, live + i, 4)) continue;
            if (!bad && st.fails < 4) printf("MISMATCH %s (world %d%s)\n", name, g_world, g_chain ? ", chained" : "");
            bad = true;
            if (st.fails < 4 && shown++ < 12) {
                uint32_t sa, aa, ba;
                memcpy(&sa, start + i, 4); memcpy(&aa, after + i, 4); memcpy(&ba, live + i, 4);
                printf("  %s: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", where(live + i, w), sa, aa,
                       fbits(aa), ba, fbits(ba));
            }
        }
    };
    compare(g_arena_after, g_arena_start, g_arena, ARENA_N);
    compare(g_data_after, g_data_start, DATA, DATA_BYTES);
    if (changed) st.changed++;
    if (bad) {
        if (st.fails < 4) {
            if (shown == 0) printf("MISMATCH %s (world %d%s)\n", name, g_world, g_chain ? ", chained" : "");
            if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
            int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
            int diff = 0;
            for (int i = 0; i < nl && diff < 12; i++) {
                bool ha = i < g_nlog[0], hb = i < g_nlog[1];
                if (ha && hb && g_log[0][i] == g_log[1][i]) continue;
                printf("  log[%d]: original %08x, rewrite %08x\n", i, ha ? g_log[0][i] : 0, hb ? g_log[1][i] : 0);
                diff++;
            }
        }
        if (st.first_fail_world < 0) st.first_fail_world = g_world;
        st.fails++;
    }
    // the footprint must cover every byte the original changed (the world's own bookkeeping aside: the bump
    // pointer, the FIFO)
    auto fp_check = [&](const uint8_t* after, const uint8_t* start, uint8_t* live, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) {
            if (after[i] == start[i]) continue;
            uint8_t* p = live + i;
            if (p >= g_arena && p < g_arena + 0x100) continue;
            bool in = false;
            for (int k = 0; k < g_fp.n && !in; k++) in = p >= (uint8_t*)g_fp.r[k].p && p < (uint8_t*)g_fp.r[k].p + g_fp.r[k].n;
            if (in) continue;
            if (g_fp.replay_only) {
                if (st.fp_info++ < 2) printf("footprint (replay_only) %s (world %d): %s changed outside it\n", name, g_world, where(p, w));
            } else if (st.fp_fails++ < 4) {
                printf("FOOTPRINT %s (world %d): %s changed outside it\n", name, g_world, where(p, w));
            }
            return;
        }
    };
    fp_check(g_arena_after, g_arena_start, g_arena, ARENA_N);
    fp_check(g_data_after, g_data_start, DATA, DATA_BYTES);
    g_ro = ro;
    cover(st);
    restore(g_data_after, g_arena_after);                // go on from the original's result
}

#define CHECKC(FN, COVER, ...)                                                                          \
    do {                                                                                                \
        auto args_ = std::make_tuple(__VA_ARGS__);                                                      \
        check(VP_CAT(name_, FN),                                                                        \
              [&](bool orig) {                                                                          \
                  auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN;                    \
                  return std::apply([&](auto... a) { return invoke(f, a...); }, args_);                 \
              },                                                                                        \
              [&](Footprint& fp) { std::apply([&](auto... a) { VP_CAT(fpof_, FN)(fp, a...); }, args_); }, \
              COVER);                                                                                   \
    } while (0)
#define CHECK(FN, ...) CHECKC(FN, [&](Stat&) {}, __VA_ARGS__)

// ---- the world's state ---------------------------------------------------------------------------------------------
static void random_frame_rot(float* m) {
    float q[4];
    quat_from_euler(rf(-3.14f, 3.14f), chance(80) ? rf(-0.3f, 0.3f) : rf(-3.14f, 3.14f), chance(80) ? rf(-0.3f, 0.3f) : rf(-3.14f, 3.14f), q);
    quat_to_matrix(q, m);
    if (chance(10)) for (int i = 0; i < 9; i++) m[i] = m[i] * rf(0.95f, 1.05f);        // not quite orthonormal
    if (chance(3)) m[rnd() % 9] = special();
}
static void random_car_state(NetCar* c) {
    random_frame_rot(c->rot.m);
    c->pos.x = (float)S.px + (chance(70) ? rf(-3, 3) : rf(-50, 50));
    c->pos.y = (float)S.py + rf(-1, 3);
    c->pos.z = (float)S.pz + (chance(70) ? rf(-3, 3) : rf(-50, 50));
    c->velocity.x = rf(-60, 60); c->velocity.y = rf(-5, 5); c->velocity.z = rf(-60, 60);
    c->angular_velocity.x = rf(-3, 3); c->angular_velocity.y = rf(-3, 3); c->angular_velocity.z = rf(-3, 3);
    c->opacity = chance(60) ? 1.0f : chance(50) ? rf(-0.1f, 1.1f) : special();
    c->car_index = ri(0, 7);
    c->perceived_rpm = rf(0, 9000);
    c->body_volume = 0;
    for (int i = 0; i < 5; i++) c->live_models[i] = 0;
    c->damaged = 0;
    c->is_plane = (uint8_t)(chance(10) ? 1 : 0);
    for (int i = 0; i < 4; i++) c->wings[i] = g_arena + A_WINGS + i * 64;
    for (int i = 0; i < 4; i++) c->wheels[i].surface = chance(70) ? 0 : chance(60) ? 10 : ri(0, 16);
    c->lowering = (uint8_t)(chance(30) ? 1 : 0);
}
static void random_car0() {
    uint8_t* c = car0();
    for (int i = 0; i < 0x1000; i++) c[i] = (uint8_t)rnd();
    *(uint32_t*)c = 0x004dc5f0;                                  // LocalCar
    float q[4];
    quat_from_euler(S.yaw, S.pitch, S.roll, q);
    quat_to_matrix(q, (float*)(c + 0x38));
    *(float*)(c + 0x5c) = (float)S.px; *(float*)(c + 0x60) = (float)S.py; *(float*)(c + 0x64) = (float)S.pz;
    *(float*)(c + 0xd70) = rf(700, 8000);
    *(int32_t*)(c + 0xd64) = ri(-1, 6);
    *(float*)(c + 0x500) = rf(-1, 1);
    *(float*)(c + 0xde4) = chance(50) ? 0 : rf(0, 1);
    *(float*)(c + 0xc34) = rf(0, 1);
    *(uint8_t*)(c + 0xe28) = (uint8_t)(chance(80) ? 0 : 1);
    *(uint8_t*)(c + 0xec0) = (uint8_t)(chance(95) ? 0 : 1);
    for (int i = 0; i < 4; i++) *(uint8_t*)(c + 0x574 + i * 0x1a4) = (uint8_t)(chance(95) ? 0 : 1);
}
static void car0_follow() {                                       // car 0 drives the sender's trajectory
    uint8_t* c = car0();
    float q[4];
    quat_from_euler(S.yaw, S.pitch, S.roll, q);
    quat_to_matrix(q, (float*)(c + 0x38));
    *(float*)(c + 0x5c) = (float)S.px; *(float*)(c + 0x60) = (float)S.py; *(float*)(c + 0x64) = (float)S.pz;
    *(float*)(c + 0x500) = S.steering;
    *(float*)(c + 0xde4) = S.braking;
    *(uint8_t*)(c + 0xe28) = S.horn;
    *(uint8_t*)(c + 0xec0) = (uint8_t)(chance(2) ? 1 : 0);
}
static void set_multi(bool on) { *(int32_t*)0x004fb454 = on ? ri(1, 3) : 0; }
static void set_switches() {
    *(int32_t*)0x004e4e3c = chance(70) ? 1 : ri(0, 6);            // the game state (1: racing)
    *(uint8_t*)0x005218e8 = (uint8_t)(chance(90) ? 0 : 1);        // replay play mode
}

// ---- one world -------------------------------------------------------------------------------------------------------
static const char* const k_bumpy_names[5] = {"0 bumpy", "1 bumpy", "2 bumpy", "3 bumpy", "4 bumpy"};
typedef NetCar*(__fastcall* NetCtor_t)(NetCar*, Edx, void*, void*);
static const NetCtor_t orig_netcar_ctor = (NetCtor_t)0x0043f370;

static void run_world() {
    memset(g_arena, 0, ARENA_N);
    bump() = A_HEAP;
    g_now = chance(40) ? rf(0, 1.5f) : chance(80) ? rf(0, 300) : rf(1000, 20000);
    g_ptime_base = rnd();
    g_out_of_bounds = chance(3);
    g_ball_mode = ri(0, 2);
    g_special_pct = chance(50) ? 0 : 20;
    bool loopback = chance(15);
    set_multi(!loopback);
    set_switches();
    *(void**)0x005218ac = g_arena + A_DEITY;
    *(uint32_t*)(g_arena + A_DEITY) = 0x004dc588;                   // a RaceDeity (Car::Update's footprint sizes it)
    *(void**)0x00521f7c = 0;                                        // no splash sound
    *(uint8_t*)0x00521634 = 1;                                      // damage on
    if (chance(50)) *(uint8_t*)0x005220d4 &= ~1;                    // munge_packet's static not yet registered
    for (int i = 0; i < 8; i++) *(void**)ball(i) = g_ball_vt;
    sender_init();
    random_car0();

    // the constructor (checked on a block of random bytes) -- then the NetCar of this world, by the original
    NetCar* sc = scratch_car();
    for (int i = 0; i < 0x1000; i++) ((uint8_t*)sc)[i] = (uint8_t)rnd();
    sc->car_index = ri(0, 7);
    CHECKC(NetCar_ctor, [&](Stat& s) { branch(s, *(int32_t*)0x004fb454 ? "network" : log_has('DQDT') ? "loopback, no pool" : "loopback"); },
           sc, 0, (void*)(g_arena + A_REPLAY), (void*)0x1234);
    NetCar* nc = netcar();
    for (int i = 0; i < 0x1000; i++) ((uint8_t*)nc)[i] = (uint8_t)rnd();
    nc->car_index = ri(0, 7);
    g_pass = 0;
    orig_netcar_ctor(nc, 0, (void*)(g_arena + A_REPLAY), (void*)0x1234);
    random_car_state(nc);
    if (chance(30)) nc->settling = 0;
    nc->settle_time = g_now - (chance(50) ? rf(0, 0.4f) : rf(0.5f, 3));
    if (chance(20)) nc->hit_time = g_now - rf(0, 2);
    if (loopback) set_multi(false);

    Q4* q4 = (Q4*)(g_arena + A_VEC);
    P3* v3 = (P3*)(g_arena + A_VEC + 0x40);
    Stiffness* stf = (Stiffness*)(g_arena + A_VEC + 0x80);
    int ticks = ri(150, 700);
    for (int t = 0; t < ticks; t++) {
        g_now = g_now + (chance(97) ? 0.016f : rf(0, 0.1f));
        if (g_special_pct && chance(1)) g_now = special();           // a wild clock for a tick
        if (chance(1)) set_switches();
        if (chance(1)) set_multi(chance(80) ? !loopback : loopback);  // MultiEnabled flips
        sender_tick();
        car0_follow();
        // deliveries due now, earliest first
        for (;;) {
            int best = -1;
            for (int i = 0; i < S.nq; i++)
                if (S.q[i].due <= g_now && (best < 0 || S.q[i].due < S.q[best].due)) best = i;
            if (best < 0) break;
            CarPhysicsPacket* in = pkt_buf(ri(0, 7));
            *in = S.q[best].p;
            S.q[best] = S.q[--S.nq];
            if (!loopback || chance(10)) {
                CHECKC(NetCar_NewPacket, [&](Stat& s) {
                    branch(s, g_pre.num_packets == 0 ? "first" : !log_has('TIME') ? "dropped" :
                              g_pre.num_packets < 3 ? "filling" : "interpolating");
                    if (log_has('THRW')) branch(s, "horn ball");
                }, nc, 0, (const CarPhysicsPacket*)in);
            }
        }
        // the tick
        CHECKC(NetCar_Update, [&](Stat& s) {
                  if (!*(int32_t*)0x004fb454) branch(s, log_has('DQDE') && log_has('DQEN') ? "loopback send" : "loopback");
                  if (log_has('STER')) branch(s, log_has('TELP') ? "steered, teleport" : log_has('STER') && post()->opacity < g_pre.opacity && bits(post()->opacity) != 0x3f4ccccd ? "steered, stale" : "steered");
                  else if (!g_pre.abandoned && post()->abandoned) branch(s, "abandon: launch");
                  else if (log_has('BRAK')) branch(s, "abandoned, rising");
                  else if (g_pre.opacity != post()->opacity && bits(post()->opacity) != 0x3f4ccccd) branch(s, "abandoned, frozen");
                  else branch(s, "waiting");
                  if (g_pre_teleported && log_has('STER') && !post()->packets->teleported) branch(s, "sender teleported");
              }, nc, 0);
        // now and then, any other function on the state as it is
        if (chance(35)) {
            switch (rnd() % 26) {
            case 0: CHECKC(NetCar_setup_interpolation, [&](Stat& s) {
                        const P3& a = post()->accel;
                        branch(s, fabsf(a.x * a.x + a.y * a.y + a.z * a.z - 96.2361f) < 0.01f ? "capped" : "free");
                    }, nc, 0); break;
            case 1: CHECKC(NetCar_update_pos, [&](Stat& s) { branch(s, log_has('TELP') ? "teleport" : "pulled"); }, nc, 0); break;
            case 2: CHECKC(NetCar_update_o, [&](Stat& s) {
                        const float* m = g_pre.rot.m;
                        branch(s, (double)m[8] + m[4] + m[0] > 0 ? "trace > 0" : m[4] > m[0] ? "i = 1 or 2" : "i = 0 or 2");
                    }, nc, 0); break;
            case 3: CHECKC(NetCar_update_stiffness, [&](Stat& s) {
                        branch(s, !memcmp(&post()->stiffness, &g_pre.stiffness, 16) ? "unchanged" :
                                  !memcmp(&post()->stiffness, &post()->stiffness_rest, 16) ? "rest" : "recovering");
                    }, nc, 0); break;
            case 4: CHECKC(NetCar_go, [&](Stat& s) {
                        branch(s, g_pre.settling ? "settling" : log_has('TELP') ? "teleported" : "steered or waiting");
                        if (g_pre_teleported) branch(s, "teleport flag");
                    }, nc, 0); break;
            case 5: CHECK(NetCar_teleport_to_p0, nc, 0); break;
            case 6: CHECKC(NetCar_we_are_abandoned, [&](Stat& s) {
                        branch(s, (g_ro & 0xff) ? "abandoned" : "not");
                        if (g_pre.num_packets <= 0) branch(s, log_has(0) ? "?" : "no packets yet");
                    }, nc, 0); break;
            case 7: CHECKC(NetCar_be_abandoned, [&](Stat& s) {
                        branch(s, !g_pre.abandoned ? "launch" : log_has('BRAK') ? "rising" : "frozen");
                    }, nc, 0); break;
            case 8: CHECKC(NetCar_do_horn, [&](Stat& s) { branch(s, log_has('THRW') ? "thrown" : "not"); }, nc, 0); break;
            case 9: CHECK(NetCar_fade_alpha, nc, 0); break;
            case 10: CHECKC(NetCar_GetPerceivedRPM, [&](Stat& s) {
                         branch(s, *(uint8_t*)0x005218e8 ? "replay" : g_pre.num_packets < 3 ? "few packets" : "extrapolated");
                     }, nc, 0); break;
            case 11: CHECK(NetCar_GetPerceivedThrottle, nc, 0); break;
            case 12: CHECK(NetCar_GetReplayPacketSize, nc, 0); break;
            case 13: CHECK(NetCar_DoneLowering, nc, 0); break;
            case 14: {
                P3* imp = v3;
                P3* pt = v3 + 1;
                float k = chance(50) ? rf(0, 70) : rf(80, 5000);
                imp->x = sp(rf(-1, 1) * k); imp->y = sp(rf(-1, 1) * k); imp->z = sp(rf(-1, 1) * k);
                pt->x = rf(-2, 2); pt->y = rf(-1, 1); pt->z = rf(-3, 3);
                CHECKC(NetCar_ResolveExternalImpulse, [&](Stat& s) { branch(s, post()->hit_time != g_pre.hit_time ? "knock" : "tap"); },
                       nc, 0, (const P3*)imp, (const P3*)pt, (int32_t)ri(0, 20));
                break;
            }
            case 15: CHECK(NetCar_ApplyExternalForce, nc, 0, (const P3*)v3, (const P3*)(v3 + 1), (int32_t)ri(0, 20)); break;
            case 16: CHECK(NetCar_ApplyDamage, nc, 0, (const P3*)v3, (const P3*)(v3 + 1), rnd()); break;
            case 17: CHECK(NetCar_MakeReplayPacket, nc, 0, g_arena + A_REPLAY); break;
            case 18: CHECKC(packet_is_too_old, [&](Stat& s) { branch(s, (g_ro & 0xff) ? "old" : "fresh"); }, chance(80) ? g_now - rf(0, 1) : special()); break;
            case 19: {
                CarPhysicsPacket* p = pkt_buf(ri(0, 7));
                if (nc->packets && chance(50)) *p = nc->packets[0];
                CHECKC(munge_packet, [&](Stat& s) { branch(s, log_has('ATEX') ? "first (atexit)" : "again"); }, p);
                break;
            }
            case 20: {
                for (int i = 0; i < 9; i++) (&v3->x)[i] = chance(90) ? rf(-100, 100) : special();
                if (chance(50)) for (int i = 6; i < 9; i++) (&v3->x)[i] = chance(90) ? uni() : special();
                P3* out = chance(30) ? v3 : v3 + 3;                      // the output may be an input
                CHECK(interpolate_v_vec, out, (const P3*)v3, (const P3*)(v3 + 1), (const P3*)(v3 + 2));
                break;
            }
            case 21: {
                for (int i = 0; i < 6; i++) (&v3->x)[i] = chance(90) ? rf(-100, 100) : special();
                P3* out = chance(30) ? v3 + 1 : v3 + 3;
                CHECK(interpolate_v_float, out, (const P3*)v3, (const P3*)(v3 + 1), chance(90) ? uni() : special());
                break;
            }
            case 22:
                if (nc->packets) {
                    int a = ri(0, 2), b = ri(0, 2);
                    CHECK(get_quat_extrapolation, q4, &nc->packets[a], &nc->packets[b], chance(90) ? g_now : special());
                }
                break;
            case 23: CHECKC(wheels_on_bumpy, [&](Stat& s) {
                         int n = 0;
                         for (int i = 0; i < 4; i++) n += g_pre.wheels[i].surface == 10;
                         branch(s, k_bumpy_names[n]);
                     }, nc->wheels); break;
            case 24: {
                for (int i = 0; i < 4; i++) stf->k[i] = chance(90) ? uni() : special();
                CHECK(Stiffness_copy, chance(30) ? stf : stf + 1, 0, (const Stiffness*)stf, chance(90) ? uni() : special());
                break;
            }
            default:
                if (chance(50)) CHECK(Delta_ctor, v3 + 4, 0);
                else CHECK(CarPhysicsPacket_ctor, pkt_buf(ri(0, 7)), 0);
                break;
            }
        }
        if (chance(2)) {                                          // the car wanders off, or its wheels change
            nc->pos.x += rf(-20, 20);
            for (int i = 0; i < 4; i++) if (chance(20)) nc->wheels[i].surface = chance(50) ? 10 : 0;
        }
    }
    // the destructor
    CHECKC(NetCar_dtor, [&](Stat& s) { branch(s, *(int32_t*)0x004fb454 ? "network" : "loopback"); }, nc, 0);
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 300;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_netcar.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_arena_start = (uint8_t*)malloc(ARENA_N);
    g_arena_after = (uint8_t*)malloc(ARENA_N);
    g_data_start = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);
    install_stubs();
    for (g_world = 0; g_world < worlds; g_world++) {
        unsigned cw;
        _controlfp_s(&cw, g_world % 4 == 3 ? _PC_53 : _PC_24, _MCW_PC);    // the physics thread's mode, mostly
        g_chain = (g_world & 1) != 0;
        run_world();
    }
    unsigned cw;
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    int failed = 0;
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("  %-34s %7ld calls, %7ld changed: %s%s%s\n", st.name, st.calls, st.changed,
               st.fails ? "DIFFERS" : "identical", st.fp_fails ? "  FOOTPRINT MISSES" : "",
               st.fp_info ? "  (replay_only: writes outside the footprint)" : "");
        if (st.br_name[0]) {
            printf("      ");
            for (int k = 0; k < 10 && st.br_name[k]; k++) printf("%s %ld%s", st.br_name[k], st.br[k], k < 9 && st.br_name[k + 1] ? ", " : "");
            printf("\n");
        }
        failed += st.fails || st.fp_fails;
    }
    printf("%d worlds (half chained); %d of %d functions differ; %d unknown footprint classes\n", worlds, failed, g_nstats, g_fp_unknown);
    return failed || g_fp_unknown ? 1 : 0;
}
