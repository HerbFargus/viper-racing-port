// world_player.cpp -- the player / input rewrites (hook/phys_player.cpp) against the originals, outside the
// game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_player.cpp
//        /Fo%TEMP%\wp\ /Fe%TEMP%\wp\world_player.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_player.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. The world is race.exe's whole .data section
// (the driver's and control mapping's statics, the control name table, the keyboard state, the mouse and
// screen statics, the Xlator cookie, the deity pointer) plus an arena holding a PlayCar-sized car, a message
// buffer, a net packet, string buffers, the telemetry, a deity (RaceDeity's vtable, 2012 bytes; the virtuals the
// rewrites call are patched to stubs), four spotter sounds and 16 other cars.
// Everything is randomised per world (floats from a mix with occasional NaN / inf / denormal values). The
// game functions these rewrites call are patched with logging stubs whose results come from a scripted
// random stream, replayed identically for the original and the rewrite (Car::Set*, Car::Update, the deity's
// virtuals, PhysicsGetTime, OptionsGet/Set, JoyGetPos, Xlator::xlate, SoundDash::Create, ...); the DriverGet*
// readers, MousePeek, ScanDown/ScanHit, KeyConvertScanKey (its MapVirtualKeyA import resolved), strncpy,
// stricmp and MatrixNormalize are the originals. For each call the original runs, the world is restored, the
// rewrite runs, and the whole world, the return value and the stub log are compared bit for bit; the bytes
// the original changed must lie inside the rewrite's footprint. Worlds alternate the x87 between single
// precision (the physics thread) and double (the main thread's usual state).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    if (vt == 0x004dc238) add(obj, 3824, what);            // PlayCar (state_layout.inc)
    else if (vt == 0x004dc5f0) add(obj, 3784, what);       // LocalCar
    else if (vt == 0x004dbfd0) add(obj, 3768, what);       // Car
    else { printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt); g_fp_unknown++; }
}

#include "../hook/phys_player.cpp"

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
static int g_special_pct = 3;
static float special() {
    switch (rnd() % 8) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));
    case 4: return fbits(rnd() & 0x807fffffu);
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits(rnd() & 0x80000000u | 0x7f7fffffu);
    default: return fbits(rnd());
    }
}
static float sp(float x) { return (int)(rnd() % 100) < g_special_pct ? special() : x; }

// the scripted stream the stubs draw from: reset to the same seed before the original and the rewrite
static uint32_t g_script = 1;
static uint32_t script() {
    g_script ^= g_script << 13;
    g_script ^= g_script >> 17;
    g_script ^= g_script << 5;
    return g_script;
}
static float script_float() {                              // (never the world's rnd(): both passes must agree)
    uint32_t r = script();
    float u = (float)(script() & 0xffffff) / 16777216.0f;
    switch (r % 12) {
    case 0: return fbits(script());                       // any bits
    case 1: return u * 200.0f;
    case 2: return -u;
    case 3: return fbits(0x7fc00000u | (script() & 0x3fffff));         // quiet NaN
    case 4: return fbits(0x7f800001u + (script() & 0x3ffffe));         // signalling NaN
    case 5: return fbits(script() & 0x807fffffu);                      // denormal
    default: return u;
    }
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
enum : uint32_t {
    ARENA_N = 0x3000,
    A_CAR = 0x0000, CAR_N = 4096,
    A_MSG = 0x1000, MSG_N = 1024,
    A_PKT = 0x1400, PKT_N = 256,
    A_STATUS = 0x1500, STATUS_N = 64,
    A_STRBUF = 0x1540, STRBUF_N = 64,
    A_CTLS = 0x1580, CTLS_N = 8 * 16,                  // scratch Controls
    A_TELEMETRY = 0x1600, TELEMETRY_N = 64,
    A_DEITY = 0x22c0, DEITY_N = 2012,                  // a RaceDeity (its methods patched: the footprints size it)
    A_SOUNDS = 0x1700, SOUND_N = 128,                  // 4 sounds
    A_CARS = 0x1900, FAKE_CAR_N = 128,                 // 16 other cars: a vtable and a frame
    A_STRINGS = 0x2100, STRING_N = 32,                 // 8 fake translated strings
    A_ENTRIES = 0x2200,                                // 8 scratch ControlEntry rows
    A_END = 0x2aa0,
};
static uint8_t* g_arena;
static uint8_t* g_data;                                // race.exe .data
static uint32_t g_data_n;
static uint8_t* car() { return g_arena + A_CAR; }
static void* fake_car(int i) { return g_arena + A_CARS + i * FAKE_CAR_N; }
static void* sound(int i) { return g_arena + A_SOUNDS + i * SOUND_N; }
static const char* fake_string(int i) { return (const char*)(g_arena + A_STRINGS + (i & 7) * STRING_N); }
static Control* scratch_control(int i) { return (Control*)(g_arena + A_CTLS + (i & 7) * 16); }
static ControlEntry* scratch_entry(int i) { return (ControlEntry*)(g_arena + A_ENTRIES + (i & 7) * 24); }

static const char* where(const void* p, char* buf) {
    uint32_t a = (uint32_t)(uintptr_t)p;
    if (p >= g_arena && p < g_arena + ARENA_N) {
        uint32_t off = (uint32_t)((const uint8_t*)p - g_arena);
        struct { uint32_t o, n; const char* name; } parts[] = {
            {A_CAR, CAR_N, "car"}, {A_MSG, MSG_N, "msg"}, {A_PKT, PKT_N, "packet"}, {A_STATUS, STATUS_N, "status"},
            {A_STRBUF, STRBUF_N, "strbuf"}, {A_CTLS, CTLS_N, "controls"}, {A_TELEMETRY, TELEMETRY_N, "telemetry"},
            {A_DEITY, DEITY_N, "deity"}, {A_SOUNDS, 4 * SOUND_N, "sounds"}, {A_CARS, 16 * FAKE_CAR_N, "other cars"},
            {A_STRINGS, 8 * STRING_N, "strings"}, {A_ENTRIES, 8 * 24, "entries"}};
        for (auto& q : parts)
            if (off >= q.o && off < q.o + q.n) { sprintf(buf, "%s+0x%x", q.name, off - q.o); return buf; }
        sprintf(buf, "arena+0x%x", off);
        return buf;
    }
    const char* what = a >= DRV_BASE && a < DRV_BASE + DRV_SIZE ? "driver static" :
                       a >= CTL_BASE && a < CTL_BASE + CTL_SIZE ? "control static" :
                       a >= TABLE && a < TABLE + TABLE_SIZE ? "control table" :
                       a >= S_KEYBOARD && a < S_KEYBOARD + 256 ? "keyboard" : ".data";
    sprintf(buf, "%s 0x%08x", what, a);
    return buf;
}

// ---- the stubs' log ------------------------------------------------------------------------------------------------
enum { LOG_MAX = 8192 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void lg(uint32_t w) { if (g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]++] = w; }
static void lg_str(const char* s) {
    if (!s) { lg(0xffffffffu); return; }
    lg((uint32_t)strlen(s));
    for (size_t i = 0; s[i]; i += 4) { uint32_t w = 0; memcpy(&w, s + i, strlen(s + i) < 4 ? strlen(s + i) : 4); lg(w); }
}

// __thiscall stubs (received as __fastcall), keyed by the original's address
template <uint32_t ADDR> static void __fastcall st_self(void* self, int) { lg(ADDR); lg((uint32_t)self); }
template <uint32_t ADDR> static void __fastcall st_self_u(void* self, int, uint32_t a) { lg(ADDR); lg((uint32_t)self); lg(a); }
template <uint32_t ADDR> static void __fastcall st_self_b(void* self, int, uint32_t a) { lg(ADDR); lg((uint32_t)self); lg(a & 0xff); }
static void* __fastcall st_Car_Car(void* self, int, void* data, void* p) {
    lg('CCTR'); lg((uint32_t)self); lg((uint32_t)data); lg((uint32_t)p);
    *(uint32_t*)self = 0x004dbfd0;
    return self;
}
static void __fastcall st_Car_Teleport(void* self, int, const void* frame, const void* point) {
    lg('CTLP'); lg((uint32_t)self); lg((uint32_t)frame); lg((uint32_t)point);
}
static void __fastcall st_Car_GetMessage(void* self, int, uint8_t* msg) {
    lg('CMSG'); lg((uint32_t)self); lg((uint32_t)msg);
    uint8_t* mine = msg + *(int32_t*)((uint8_t*)self + 8);
    for (int i = 0; i < 0x182; i++) mine[i] = (uint8_t)script();
}
// __cdecl stubs
template <uint32_t ADDR> static float __cdecl st_float(void) { lg(ADDR); return script_float(); }
static int __cdecl st_PhysicsGetRealism(void) { lg('REAL'); return (int)(script() % 4); }
static uint32_t* __cdecl st_PhysTaskGetTelemetry(void) { lg('TELE'); return (uint32_t*)(g_arena + A_TELEMETRY); }
static void* __cdecl st_PhysTaskFindCar(int i) {
    lg('FIND'); lg((uint32_t)i);
    uint32_t r = script() % 6;
    if (r == 0) return 0;
    if (r == 1) return car();
    return fake_car(i & 15);
}
static unsigned char __cdecl st_CanTeleport(void* c) { lg('CANT'); lg((uint32_t)c); return (unsigned char)(script() & 1); }
template <uint32_t ADDR> static void __cdecl st_reg(void* c, int i) { lg(ADDR); lg((uint32_t)c); lg((uint32_t)i); }
static void __cdecl st_MultiUnregisterLocalCar(void* c) { lg('MUNR'); lg((uint32_t)c); }
static void* __cdecl st_SoundDash_Create(const char* file, int a, int b) {
    lg('SNDC'); lg_str(file); lg((uint32_t)a); lg((uint32_t)b);
    uint32_t r = script() % 5;
    return r == 0 ? 0 : sound(r - 1);
}
static void __cdecl st_OptionsGetI(const char* sec, const char* key, int* v) {
    lg('OPTI'); lg_str(sec); lg_str(key); lg((uint32_t)*v);       // the pointer is a stack local: log the value
    if (script() & 1) *v = (int)(script() % 5) - 1;
}
static void __cdecl st_OptionsGetB(const char* sec, const char* key, unsigned char* v) {
    lg('OPTB'); lg_str(sec); lg_str(key); lg((uint32_t)*v);
    if (script() & 1) *v = (unsigned char)(script() % 3);
}
static void __cdecl st_OptionsGetF(const char* sec, const char* key, float* v) {
    lg('OPTF'); lg_str(sec); lg_str(key); lg(*(uint32_t*)v);
    if (script() & 1) *v = script_float();
}
static const char* pick_name() {                       // a control's ini name, in some case, or something else
    static char buf[40];
    uint32_t r = script();
    if (r % 8 == 0) return "";
    if (r % 8 == 1) return "nothing like it";
    const char* s = (const char*)(uintptr_t)k_init_rows[script() % 83].name;
    size_t n = strlen(s);
    for (size_t i = 0; i <= n; i++) {
        char c = s[i];
        if (r & 0x100) c = (char)toupper((unsigned char)c);
        buf[i] = c;
    }
    return buf;
}
static void __cdecl st_OptionsGetS(const char* sec, const char* key, char* buf, int n) {
    lg('OPTS'); lg_str(sec); lg_str(key); lg_str(buf); lg((uint32_t)n);
    if (script() & 1) {
        strncpy(buf, pick_name(), (size_t)n);
        buf[n - 1] = 0;
    }
}
static void __cdecl st_OptionsSetS(const char* sec, const char* key, const char* v) {
    lg('OSET'); lg_str(sec); lg_str(key); lg_str(v);
}
static void __cdecl st_JoyEnableFF(uint32_t on) { lg('JOFF'); lg(on & 0xff); }
static void __cdecl st_JoySetForce(uint32_t a, uint32_t b, uint32_t c) { lg('JSFO'); lg(a); lg(b); lg(c); }
static void random_joypos(JoyPos* j, uint32_t (*r)()) {
    for (int i = 0; i < 6; i++) j->axis[i] = fbits(r());
    for (int i = 0; i < 6; i++) if (r() % 4) j->axis[i] = (float)(r() & 0xffff) / 32768.0f - 1.0f;
    for (int i = 0; i < 32; i++) j->button[i] = (uint8_t)(r() % 3 == 0 ? r() : 0);
    uint32_t p = r() % 4;
    j->pov = p == 0 ? (int32_t)(r() % 5) : p == 1 ? 0x3f800000 + (int32_t)(r() % 4) * 0x00400000 : p == 2 ? 0 : (int32_t)r();
}
static unsigned char __cdecl st_JoyGetPos(void* p) {
    lg('JPOS'); lg((uint32_t)p);
    if (script() % 8) random_joypos((JoyPos*)p, script);
    return (unsigned char)(script() & 1);
}
static void __fastcall st_Xlator_xlate(void* self, int) {
    lg('XLAT'); lg((uint32_t)self);
    Xlator* x = (Xlator*)self;
    if (script() % 4) {                                  // found: the text and the cookie
        x->text = fake_string((int)(script() & 7));
        x->cookie = *(uint32_t*)S_XLATOR_COOKIE;
    } else {
        x->text = (const char*)0x004eb3fc;               // lookup failed: the "missing" string, cookie stale
    }
}
// the Deity's virtuals (one or two int-sized arguments, as the callers push): RaceDeity's own, patched, so the
// deity has RaceDeity's vtable (PlayCar::Update's footprint accepts only a RaceDeity, as Car::Update's does)
template <int SLOT> static uint32_t __fastcall st_deity1(void* self, int, uint32_t a) { lg('DEI1' + SLOT); lg((uint32_t)self); lg(a); return script(); }
template <int SLOT> static uint32_t __fastcall st_deity2(void* self, int, uint32_t a, uint32_t b) { lg('DEI2' + SLOT); lg((uint32_t)self); lg(a); lg(b); return script(); }
// other cars: IsSolid (vtable +0x30)
static unsigned char __fastcall st_is_solid(void* self, int) { lg('SOLD'); lg((uint32_t)self); return (unsigned char)(script() % 3 != 0); }
static void* g_car_vt[16];
// sounds: the deleting destructor (vtable +0)
static void* __fastcall st_sound_delete(void* self, int, unsigned flags) { lg('SDEL'); lg((uint32_t)self); lg(flags); return self; }
static void* g_sound_vt[4];

static void install_stubs() {
    patch_jump(0x004364c0, (void*)st_Car_Car);
    patch_jump(0x00436920, (void*)st_self<0x436920>);                 // Car::~Car
    patch_jump(0x00439f90, (void*)st_self<0x439f90>);                 // Car::Reset
    patch_jump(0x00439b50, (void*)st_Car_Teleport);
    patch_jump(0x00437680, (void*)st_self<0x437680>);                 // Car::Update
    patch_jump(0x00438ef0, (void*)st_Car_GetMessage);
    patch_jump(0x004395e0, (void*)st_self_u<0x4395e0>);               // SetTractionControl
    patch_jump(0x00439600, (void*)st_self_u<0x439600>);               // SetABSBraking
    patch_jump(0x00439620, (void*)st_self_u<0x439620>);               // SetSteering
    patch_jump(0x004396d0, (void*)st_self_u<0x4396d0>);               // SetThrottle
    patch_jump(0x004396e0, (void*)st_self_u<0x4396e0>);               // SetBraking
    patch_jump(0x004396f0, (void*)st_self_u<0x4396f0>);               // SetClutch
    patch_jump(0x00439700, (void*)st_self_u<0x439700>);               // SetEBrake
    patch_jump(0x00439710, (void*)st_self_u<0x439710>);               // SetGear
    patch_jump(0x00439740, (void*)st_self<0x439740>);                 // SetGearAuto
    patch_jump(0x00439780, (void*)st_self<0x439780>);                 // HelpOut
    patch_jump(0x0043afb0, (void*)st_self_b<0x43afb0>);               // SetHorn (a byte: the rest of the dword is junk)
    patch_jump(0x0043b050, (void*)st_self_u<0x43b050>);               // SetPitch
    patch_jump(0x00446800, (void*)st_self<0x446800>);                 // Engine::Crank
    patch_jump(0x0042bd50, (void*)st_PhysicsGetRealism);
    patch_jump(0x0042bc80, (void*)st_float<0x42bc80>);                // PhysicsGetTime
    patch_jump(0x0040d4e0, (void*)st_float<0x40d4e0>);                // HackThrottleBoost
    patch_jump(0x0040d500, (void*)st_float<0x40d500>);                // HackGripBoost
    patch_jump(0x00428d80, (void*)st_PhysTaskGetTelemetry);
    patch_jump(0x00426d40, (void*)st_PhysTaskFindCar);
    patch_jump(0x00443920, (void*)st_CanTeleport);
    patch_jump(0x0041d260, (void*)st_reg<0x41d260>);                  // AIRegisterPlayCar
    patch_jump(0x0041d2e0, (void*)st_reg<0x41d2e0>);                  // AIUnregisterCar
    patch_jump(0x004a2760, (void*)st_reg<0x4a2760>);                  // MultiRegisterLocalCar
    patch_jump(0x004a27e0, (void*)st_MultiUnregisterLocalCar);
    patch_jump(0x00472510, (void*)st_SoundDash_Create);
    patch_jump(0x004713e0, (void*)st_OptionsGetI);
    patch_jump(0x00471470, (void*)st_OptionsGetB);
    patch_jump(0x00471350, (void*)st_OptionsGetF);
    patch_jump(0x00471500, (void*)st_OptionsGetS);
    patch_jump(0x00471560, (void*)st_OptionsSetS);
    patch_jump(0x00419250, (void*)st_JoyEnableFF);
    patch_jump(0x00419360, (void*)st_JoySetForce);
    patch_jump(0x00418ff0, (void*)st_JoyGetPos);
    patch_jump(0x0041afb0, (void*)st_Xlator_xlate);
    // KeyConvertScanKey calls MapVirtualKeyA through the import table: resolve it
    *(void**)0x005d769c = (void*)GetProcAddress(LoadLibraryA("user32.dll"), "MapVirtualKeyA");
    patch_jump(0x00442fc0, (void*)st_deity2<0x2c>);                   // RaceDeity::RegisterCar(LocalCar)
    patch_jump(0x00443a00, (void*)st_deity2<0x3c>);                   // RaceDeity::TeleportToLine
    patch_jump(0x004441c0, (void*)st_deity1<0x48>);                   // RaceDeity::GetCurrentLap
    patch_jump(0x00444220, (void*)st_deity1<0x50>);                   // RaceDeity::GetDLong
    g_car_vt[0x30 / 4] = (void*)st_is_solid;
    g_sound_vt[0] = (void*)st_sound_delete;
}

// ---- random worlds ------------------------------------------------------------------------------------------------
static void random_bytes(void* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t*)p)[i] = (uint8_t)rnd(); }
static void random_floats(void* p, uint32_t n, float lo, float hi) {
    for (uint32_t i = 0; i + 4 <= n; i += 4) { float f = sp(rf(lo, hi)); memcpy((uint8_t*)p + i, &f, 4); }
}
static void random_control(Control* c) {
    uint32_t r = rnd() % 16;
    c->type = r < 12 ? 1 + (int)(r % 6) : r == 12 ? 0 : r == 13 ? 7 : (int)rnd();
    c->value = 0;
    if (c->type == 1) { c->key = (uint8_t)rnd(); if (rnd() % 4 == 0) c->value |= (int)(rnd() << 8); }
    else c->value = rnd() % 4 == 0 ? (int)rnd() : (int)(rnd() % 18);
}
static void set_bits(uint32_t addr, uint32_t v) { memcpy((void*)addr, &v, 4); }
static void random_mouse(Mouse* m) {
    m->x = sp(rf(-1.2f, 1.2f));
    m->y = sp(rf(-1.2f, 1.2f));
    for (int i = 0; i < 3; i++) m->button[i] = (uint8_t)(rnd() % 3 == 0 ? rnd() : 0);
    m->_0b = (uint8_t)rnd();
}
static const uint32_t k_xlators[44] = {
    0x00522858, 0x00522538, 0x005224a8, 0x00522548, 0x00522560, 0x00522570, 0x00522580, 0x00522590, 0x005226c8,
    0x00522828, 0x00522798, 0x005224d8, 0x00522778, 0x00522838, 0x005227c8, 0x00522628, 0x00522648, 0x00522658,
    0x00522668, 0x00522678, 0x00522688, 0x00522698, 0x005226a8, 0x005226b8, 0x00522610, 0x005227a8, 0x005224b8,
    0x00522720, 0x00522788, 0x005227e8, 0x005224c8, 0x005225a0, 0x00522848, 0x005224f8, 0x005227d8, 0x00522710,
    0x00522600, 0x005224e8, 0x005225f0, 0x00522518, 0x00522528, 0x005226e0, 0x005227b8, 0x00522638};

static void random_table(bool sane) {
    ControlEntry* t = (ControlEntry*)TABLE;
    if (!sane) {
        random_bytes(t, TABLE_SIZE);
        t[0].name = (const char*)0x004eec48;               // statically initialised in the image; control_init never writes it
        return;
    }
    for (int i = 0; i < 83; i++) {
        const InitRow& r = k_init_rows[i];
        t[i].name = (const char*)(uintptr_t)r.name;
        t[i].display = rnd() % 3 == 0 ? 0 : rnd() % 4 == 0 ? fake_string(ri(0, 7)) : (const char*)(uintptr_t)k_init_rows[ri(0, 82)].name;
        t[i].type = rnd() % 8 == 0 ? ri(0, 8) : r.type;
        t[i].value = rnd() % 4 == 0 ? (int)rnd() : r.value;
        for (int k = 0; k < 4; k++) t[i].inline_display[k] = (char)(rnd() % 3 == 0 ? 0 : 'A' + rnd() % 26);
        t[i].inline_display[3] = 0;
        t[i].reserved = (uint8_t)(rnd() % 10 == 0 ? rnd() % 3 : 0);
        for (int k = 0; k < 3; k++) t[i]._15[k] = (uint8_t)rnd();
    }
}

static void build_world() {
    // race.exe's statics: the driver's
    for (uint32_t a = DRV_BASE; a < DRV_BASE + DRV_SIZE; a += 4) set_bits(a, rnd());
    for (uint32_t a : {S_BRAKING, S_STEERING, S_THROTTLE, S_EBRAKE, S_CLUTCH, S_LOOK_SIDE, S_PITCH})
        *(float*)a = sp(rf(-1.5f, 1.5f));
    for (uint32_t a : {S_BRAKE_RANGE, S_BRAKE_SPEED, S_THROTTLE_RANGE, S_THROTTLE_SPEED, S_THROTTLE_SENS, S_BRAKE_SENS,
                       S_STEER_RANGE, S_STEER_SPEED, S_STEER_SENS})
        *(float*)a = sp(rnd() % 3 ? rf(0, 2) : rf(-50, 50));
    for (uint32_t a : {S_FORCE_A, S_FORCE_B, S_FORCE_C}) *(float*)a = sp(rf(-3, 3));
    *(float*)S_FF_TIMER = sp(rf(-0.01f, 0.07f));
    *(int32_t*)S_NUM_GEARS = ri(-1, 7);
    *(int32_t*)S_GEAR = ri(-2, 8);
    for (uint32_t a : {S_SHIFT_LATCH, S_AIRLIFT, S_FORCE_FEEDBACK, S_GAME_PAD, S_HELP, S_REVERSE, S_HORN, S_NONLINEAR, S_LOOK_BACK})
        *(uint8_t*)a = (uint8_t)(rnd() % 4 == 0 ? rnd() : rnd() % 5 == 0);
    for (uint32_t a : {S_CTL_THROTTLE, S_CTL_STEER_LEFT, S_CTL_BRAKE, S_CTL_LOOK_RIGHT, S_CTL_HORN, S_CTL_EBRAKE,
                       S_CTL_STEER_RIGHT, S_CTL_HELP, S_CTL_DOWNSHIFT, S_CTL_LOOK_BACK, S_CTL_AIRLIFT, S_CTL_LOOK_LEFT,
                       S_CTL_REVERSE, S_CTL_UPSHIFT, S_CTL_CLUTCH})
        random_control((Control*)a);
    // the control mapping's
    for (uint32_t a = CTL_BASE; a < CTL_BASE + CTL_SIZE; a += 4) set_bits(a, rnd());
    *(uint32_t*)S_XLATOR_COOKIE = rnd() % 4 ? rnd() : 0;
    for (uint32_t a : k_xlators) {
        Xlator* x = (Xlator*)a;
        x->key = (const char*)(uintptr_t)k_init_rows[ri(0, 82)].name;
        x->text = rnd() % 5 == 0 ? 0 : fake_string(ri(0, 7));
        x->cookie = rnd() % 3 ? *(uint32_t*)S_XLATOR_COOKIE : rnd();
    }
    ((Xlator*)S_XL_UNKNOWN)->text = fake_string(ri(0, 7));
    bool built = rnd() % 3 != 0;
    *(uint8_t*)S_INIT_GUARD = (uint8_t)(built ? (rnd() | 1) : (rnd() & ~1u));
    random_table(built);
    *(ControlEntry**)S_ENTRIES = built ? (ControlEntry*)TABLE : (ControlEntry*)(uintptr_t)rnd();
    *(int32_t*)S_NUM_ENTRIES = built ? (rnd() % 4 ? 83 : ri(0, 83)) : (int)rnd();
    random_joypos((JoyPos*)S_JOY, rnd);
    random_joypos((JoyPos*)S_NEUTRAL_JOY, rnd);
    random_mouse((Mouse*)S_MOUSE);
    random_mouse((Mouse*)S_NEUTRAL_MOUSE);
    // the platform's: keyboard, mouse, screen
    for (int i = 0; i < 256; i++) *(uint8_t*)(S_KEYBOARD + i) = (uint8_t)(rnd() % 3 == 0 ? rnd() : rnd() % 4 == 0 ? 0x80 : 0);
    *(int32_t*)0x00508548 = rnd() % 8 ? ri(0, 1600) : (int)rnd();
    *(int32_t*)0x0050854c = rnd() % 8 ? ri(0, 1200) : (int)rnd();
    *(int32_t*)0x00508650 = rnd() % 2 ? (int)(rnd() % 8) : (int)rnd();
    *gxScreenWid = rnd() % 16 == 0 ? ri(-1, 1) : ri(2, 1600);
    *gxScreenHit = rnd() % 16 == 0 ? ri(-1, 1) : ri(2, 1200);
    *g_deity = g_arena + A_DEITY;
    // the arena
    for (uint32_t i = 0; i < ARENA_N; i += 4) { float x = (rnd() & 1) ? sp(rf(-1, 1)) : sp(rf(-3000, 3000)); memcpy(g_arena + i, &x, 4); }
    uint8_t* c = car();
    *(uint32_t*)c = 0x004dc238;                                         // a PlayCar (the footprints size it)
    *(int32_t*)(c + C_MSG_OFFSET) = ri(0, 64);
    *(uint32_t*)(c + C_STATUS) = rnd() % 8 == 0 ? rnd() : rnd() % 5;
    *(int32_t*)(c + C_CAR_INDEX) = ri(0, 15);
    *(int32_t*)(c + C_RACE_STATE) = ri(0, 3);
    *(int32_t*)(c + C_GEAR) = ri(-1, 6);
    *(int32_t*)(c + C_TRANS_NUM_GEARS) = ri(1, 7);
    for (int i = 0; i < 9; i++) *(float*)(c + C_ROT + 4 * i) = sp(rf(-1, 1));
    for (int i = 0; i < 3; i++) *(float*)(c + C_POS + 4 * i) = sp(rf(-500, 500));
    for (int i = 0; i < 3; i++) *(float*)(c + C_VELOCITY + 4 * i) = sp(rf(-60, 60));
    for (uint32_t off : {C_BRAKING, C_THROTTLE, C_ENGINE_SMOOTHED_THROTTLE, C_CLUTCH_ENGAGEMENT})
        *(float*)(c + off) = sp(rf(-0.2f, 1.2f));
    for (uint32_t off : {C_IS_PLANE, C_ENGINE_STALLED, PC_AUTO_SHIFT, PC_AUTO_CLUTCH, PC_SPOTTER, L_TELEPORTED, C_HORN})
        *(uint8_t*)(c + off) = (uint8_t)(rnd() % 4 == 0 ? rnd() : rnd() % 3 == 0);
    *(float*)(c + PC_AIRLIFT_TIME) = rnd() % 3 == 0 ? 0.0f : rnd() % 2 ? sp(rf(0, 100)) : fbits(rnd() % 2 ? 0x80000000u : 0x4ceb79a3u);
    *(float*)(c + PC_SPOT_TIME) = sp(rf(-1, 100));
    *(uint32_t*)(c + PC_SPOT_FLAGS) = rnd() % 4;
    *(uint32_t*)(c + PC_SPOT_FLAGS_PREV) = rnd() % 4;
    for (uint32_t off : {PC_SND_LEFT, PC_SND_RIGHT, PC_SND_BOTH, PC_SND_CLEAR})
        *(void**)(c + off) = rnd() % 4 == 0 ? 0 : sound(ri(0, 3));
    for (int i = 0; i < 4; i++) *(void**)sound(i) = g_sound_vt;
    *(uint32_t*)(g_arena + A_DEITY) = 0x004dc588;                       // RaceDeity's vtable
    // what PlayCar::Update's footprint follows for Car::Update (stubbed here): no collision volumes, no live models
    *(int32_t*)(c + 0x24) = 0;
    memset(c + 0x4c4, 0, 20);
    for (int i = 0; i < 16; i++) {
        uint8_t* o = (uint8_t*)fake_car(i);
        *(void**)o = g_car_vt;
        for (int k = 0; k < 3; k++) *(float*)(o + C_POS + 4 * k) = sp(*(float*)(c + C_POS + 4 * k) + rf(-12, 12));
    }
    for (int i = 0; i < 8; i++) {
        char* s = (char*)g_arena + A_STRINGS + i * STRING_N;
        int n = ri(0, 30);
        for (int k = 0; k < n; k++) s[k] = (char)('a' + rnd() % 26);
        s[n] = 0;
    }
    for (int i = 0; i < 8; i++) random_control(scratch_control(i));
    for (int i = 0; i < 8; i++) {
        ControlEntry* e = scratch_entry(i);
        e->type = rnd() % 8 == 0 ? (int)rnd() : ri(0, 7);
        e->value = rnd() % 2 ? (int)rnd() : ri(0, 20);
    }
}

// ---- running one call both ways ------------------------------------------------------------------------------------
template <typename F, typename... A> static uint64_t invoke(F f, A... a) {
    typedef decltype(f(a...)) R;
    if constexpr (std::is_void_v<R>) {
        f(a...);
        return 0;
    } else {
        R r = f(a...);
        uint64_t u = 0;
        memcpy(&u, &r, sizeof r);
        return u;
    }
}

struct Stat { const char* name; long calls, changed, fails, fp_fails, fp_info; };
static Stat g_stats[64];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0};
    return g_stats[g_nstats++];
}
static uint8_t *g_start, *g_after;                      // .data then the arena
static uint32_t g_world_n;
static Footprint g_fp;
static int g_world;
static const uint8_t* g_mask;                           // bytes the compare ignores (ControlDetect's junk)
static uint32_t g_mask_n;

static void snapshot(uint8_t* to) { memcpy(to, g_data, g_data_n); memcpy(to + g_data_n, g_arena, ARENA_N); }
static void restore(const uint8_t* from) { memcpy(g_data, from, g_data_n); memcpy(g_arena, from + g_data_n, ARENA_N); }
static uint8_t* live(uint32_t i) { return i < g_data_n ? g_data + i : g_arena + (i - g_data_n); }
static uint32_t index_of(const uint8_t* p) { return p >= g_arena && p < g_arena + ARENA_N ? g_data_n + (uint32_t)(p - g_arena) : (uint32_t)(p - g_data); }

static const char* g_running;
static bool g_running_orig;
static int crash_filter(EXCEPTION_POINTERS* e, bool orig) {
    printf("CRASH in %s (%s, world %d): exception %08lx at %p\n", g_running, orig ? "original" : "rewrite", g_world,
           e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <typename Run> static uint64_t guarded(Run& run, bool orig) {
    __try {
        return run(orig);
    } __except (crash_filter(GetExceptionInformation(), orig)) {
        return 0xdeaddeaddeaddeadull;
    }
}

template <typename Run, typename Fp> static void check(const char* name, Run run, Fp footprint) {
    Stat& st = stat(name);
    st.calls++;
    snapshot(g_start);
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    footprint(g_fp);
    uint32_t seed = rnd() | 1;
    g_script = seed;
    g_pass = 0; g_nlog[0] = 0;
    g_running = name;
    uint64_t ro = guarded(run, true);
    snapshot(g_after);
    restore(g_start);
    g_script = seed;
    g_pass = 1; g_nlog[1] = 0;
    uint64_t rn = guarded(run, false);
    for (uint32_t i = 0; i < g_mask_n; i++) ((uint8_t*)g_mask)[i] = g_after[index_of(g_mask + i)];   // the junk bytes: the original's
    bool changed = false, bad = ro != rn || g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], 4 * g_nlog[0]) != 0;
    int shown = 0;
    for (uint32_t i = 0; i < g_world_n; i += 4) {
        const uint8_t* a = g_after + i;
        const uint8_t* s = g_start + i;
        const uint8_t* b = live(i);
        if (memcmp(a, s, 4)) changed = true;
        if (memcmp(a, b, 4)) {
            if (!bad && st.fails < 4) printf("MISMATCH %s (world %d)\n", name, g_world);
            bad = true;
            if (st.fails < 4 && shown++ < 12) {
                uint32_t sa, aa, ba;
                memcpy(&sa, s, 4); memcpy(&aa, a, 4); memcpy(&ba, b, 4);
                char w[64];
                printf("  %s: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", where(b, w), sa, aa, fbits(aa), ba, fbits(ba));
            }
        }
    }
    if (changed) st.changed++;
    if (bad) {
        if (st.fails < 4) {
            if (shown == 0) printf("MISMATCH %s (world %d)\n", name, g_world);
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
        st.fails++;
    }
    // the footprint must cover every byte the original changed (the telemetry is a physics static the
    // game saves itself; replay_only footprints are reported, not counted)
    for (uint32_t i = 0; i < g_world_n; i++) {
        if (g_after[i] == g_start[i]) continue;
        uint8_t* p = live(i);
        bool in = (p >= g_arena + A_TELEMETRY && p < g_arena + A_TELEMETRY + TELEMETRY_N) ||
                  (p >= g_mask && p < g_mask + g_mask_n);
        for (int k = 0; k < g_fp.n && !in; k++)
            in = p >= (uint8_t*)g_fp.r[k].p && p < (uint8_t*)g_fp.r[k].p + g_fp.r[k].n;
        if (!in) {
            char w[64];
            if (g_fp.replay_only) {
                if (st.fp_info++ < 2) printf("footprint (replay_only) %s (world %d): %s changed outside it\n", name, g_world, where(p, w));
            } else if (st.fp_fails++ < 4) {
                printf("FOOTPRINT %s (world %d): %s changed outside it\n", name, g_world, where(p, w));
            }
            break;
        }
    }
    restore(g_after);                                  // go on from the original's result
    g_mask_n = 0;
}

#define CHECK(FN, ...)                                                                                  \
    do {                                                                                                \
        auto args_ = std::make_tuple(__VA_ARGS__);                                                      \
        check(VP_CAT(name_, FN),                                                                        \
              [&](bool orig) {                                                                          \
                  auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN;                    \
                  return std::apply([&](auto... a) { return invoke(f, a...); }, args_);                 \
              },                                                                                        \
              [&](Footprint& fp) { std::apply([&](auto... a) { VP_CAT(fpof_, FN)(fp, a...); }, args_); });\
    } while (0)

static void run_world() {
    build_world();
    void* self = car();
    Control* ctl = scratch_control(ri(0, 7));
    char* strbuf = (char*)(g_arena + A_STRBUF);
    // the control mapping
    switch (rnd() % 4) {
    case 0: CHECK(control_init); break;
    case 1: CHECK(ControlDetectInit); break;
    default: break;
    }
    for (int s = ri(6, 24); s--;) {
        ctl = scratch_control(ri(0, 7));
        switch (rnd() % 16) {
        case 0: CHECK(ControlUpdate, (unsigned char)(rnd() % 3)); break;
        case 1: CHECK(ControlReadAnalog, (const Control*)ctl); break;
        case 2: CHECK(ControlReadDigital, (const Control*)ctl); break;
        case 3: CHECK(ControlReadTrigger, (const Control*)ctl); break;
        case 4: CHECK(ControlIsDigital, (const Control*)ctl); break;
        case 5: CHECK(get_neutral_value_analog, (const Control*)ctl); break;
        case 6: CHECK(get_neutral_value_digital, (const Control*)ctl); break;
        case 7: CHECK(find_control_entry, (const Control*)ctl); break;
        case 8: CHECK(control_from_entry, ctl, scratch_entry(ri(0, 7))); break;
        case 9: CHECK(ControlFromInternalString, ctl, (const char*)(rnd() % 2 ? pick_name() : fake_string(ri(0, 7)))); break;
        case 10: CHECK(ControlToInternalString, strbuf, ri(1, 32), (const Control*)ctl); break;
        case 11: CHECK(ControlToDisplayString, strbuf, ri(1, 32), (const Control*)ctl); break;
        case 12:
            g_mask = (uint8_t*)ctl + 5; g_mask_n = 3;               // the original copies stack junk there for a key
            CHECK(ControlDetect, ctl);
            break;
        case 13: CHECK(ControlGet, ctl, (const char*)0x004edc20 + 12 * (rnd() % 3)); break;
        case 14: CHECK(ControlSet, (const Control*)ctl, (const char*)0x004edc38); break;
        default: CHECK(ControlUpdate, (unsigned char)1); break;
        }
    }
    // the driver
    switch (rnd() % 8) {
    case 0: CHECK(DriverBegin); break;
    case 1: CHECK(DriverRefresh); break;
    case 2: CHECK(DriverEnd); break;
    default: break;
    }
    for (int s = ri(3, 12); s--;) {
        switch (rnd() % 10) {
        case 0: CHECK(DriverIsSteeringDigital); break;
        case 1: CHECK(DriverIsBrakeDigital); break;
        case 2: CHECK(DriverIsThrottleDigital); break;
        case 3: CHECK(DriverSetForce, rnd(), rnd(), rnd()); break;
        case 4: CHECK(DriverSetNumGears, ri(-1, 8)); break;
        case 5: CHECK(DriverSetGear, ri(-2, 8)); break;
        default: CHECK(DriverUpdate, sp(rnd() % 4 ? rf(0, 0.07f) : rf(-0.1f, 2.0f))); break;
        }
    }
    // the player's car
    for (int s = ri(2, 8); s--;) {
        switch (rnd() % 8) {
        case 0: CHECK(PlayCar_Reset, self, 0); break;
        case 1: CHECK(PlayCar_Teleport, self, 0, (const void*)(g_arena + A_PKT), (const void*)(g_arena + A_PKT + 64)); break;
        case 2: CHECK(LocalCar_Teleport, self, 0, (const void*)(g_arena + A_PKT), (const void*)(g_arena + A_PKT + 64)); break;
        case 3: CHECK(PlayCar_GetMessage, self, 0, g_arena + A_MSG); break;
        case 4: CHECK(PlayCar_MakeStatusString, self, 0, (char*)(g_arena + A_STATUS)); break;
        case 5: CHECK(LocalCar_FillNetPacket, self, 0, g_arena + A_PKT); break;
        default: CHECK(PlayCar_Update, self, 0); break;
        }
        if (rnd() % 3 == 0) {                             // the car moves on
            uint8_t* c = car();
            for (int i = 0; i < 3; i++) *(float*)(c + C_VELOCITY + 4 * i) = sp(rf(-60, 60));
            *(float*)(c + C_BRAKING) = sp(rf(-0.2f, 1.2f));
            *(int32_t*)(c + C_RACE_STATE) = ri(0, 3);
        }
    }
    // the constructors and destructors, which rebuild the car: last
    switch (rnd() % 4) {
    case 0: CHECK(PlayCar_ctor, self, 0, (void*)(g_arena + A_MSG), (void*)(g_arena + A_PKT)); break;
    case 1: CHECK(LocalCar_ctor, self, 0, (void*)(g_arena + A_MSG), (void*)(g_arena + A_PKT)); break;
    case 2: CHECK(PlayCar_dtor, self, 0); break;
    default: CHECK(LocalCar_dtor, self, 0); break;
    }
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 3000;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_player.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(0x400000 + ((IMAGE_DOS_HEADER*)0x400000)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".data", 6)) { g_data = (uint8_t*)0x400000 + sec[i].VirtualAddress; g_data_n = sec[i].Misc.VirtualSize; }
    g_world_n = g_data_n + ARENA_N;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_start = (uint8_t*)malloc(g_world_n);
    g_after = (uint8_t*)malloc(g_world_n);
    install_stubs();
    for (g_world = 0; g_world < worlds; g_world++) {
        unsigned cw;
        _controlfp_s(&cw, g_world & 1 ? _PC_53 : _PC_24, _MCW_PC);    // the main thread's and the physics thread's modes
        g_special_pct = g_world % 4 == 0 ? 0 : g_world % 4 == 1 ? 1 : 5;
        run_world();
    }
    int failed = 0;
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("  %-28s %8ld calls, %8ld changed the world: %s%s%s\n", st.name, st.calls, st.changed,
               st.fails ? "DIFFERS" : "identical", st.fp_fails ? "  FOOTPRINT MISSES" : "",
               st.fp_info ? "  (replay_only: writes outside the footprint)" : "");
        failed += st.fails || st.fp_fails;
    }
    printf("%d worlds; %d of %d functions differ; %d unknown footprint classes\n", worlds, failed, g_nstats, g_fp_unknown);
    return failed || g_fp_unknown ? 1 : 0;
}
