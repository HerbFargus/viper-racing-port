// fuzz.cpp -- run the pure rewrites against the originals, outside the game, on random inputs.
//
//   test\build_fuzz.bat && test\build\fuzz.exe [iterations] [name-filter, or - for all] [seed]
//
// Loads out\race_v10.exe's sections at its own base, 0x400000 (this program is linked elsewhere, so the
// address is free), with no imports resolved: pure functions -- maths -- call nothing outside the image.
// The FPU is put in single precision, as the game's physics thread runs it. Each rewrite registered with
// PORT_FN whose footprint is pure is run against its original (fuzz.h); the first difference is printed
// with the inputs.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hook/port.h"

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
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_state = 0x9e3779b9u;
bool g_fuzz_specials = true;

uint32_t fuzz_rand() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}

static float bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

float fuzz_float() {
    uint32_t r = fuzz_rand(), k = r % 100;
    float u = (float)(fuzz_rand() & 0xffffff) / 16777216.0f;          // [0, 1)
    float s = (r & 0x100) ? -1.0f : 1.0f;
    if (k < 40) return s * u;                                         // unit range: rotations, directions
    if (k < 70) return s * u * 200.0f;                                // world scale: metres, m/s
    if (k < 78) return s * u * 1e6f;                                  // big: forces, masses
    if (k < 84) return s * u * 1e-6f;                                 // small
    if (k < 88) return (r & 0x200) ? 0.0f : -0.0f;
    if (k < 90) return s;                                             // exactly +-1
    if (!g_fuzz_specials) return s * u;
    if (k < 93) return bits(fuzz_rand() & 0x807fffff);                // denormal
    if (k < 95) return bits((r & 0x100 ? 0x80000000u : 0) | 0x7f800000u);   // infinity
    if (k < 97) return bits(0x7fc00000u | (fuzz_rand() & 0x3fffff));  // quiet NaN
    return bits(fuzz_rand());                                         // any bit pattern at all
}

void fuzz_fill(Arena& ar) {
    for (int i = 0; i < ARENA; i += 4) {
        float f = fuzz_float();
        memcpy(ar.a + i, &f, 4);
    }
    memcpy(ar.b, ar.a, ARENA);
}

int fuzz_guarded(void (*fn)(void*), void* arg) {
    __try { fn(arg); } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return 0;
}

static const Arena* g_last_arena;
int fuzz_report(int it, const Arena& ar, const void* ro, const void* rn, size_t rs) {
    g_last_arena = &ar;
    printf("    MISMATCH on iteration %d\n", it);
    if (rs && memcmp(ro, rn, rs)) {
        uint32_t a = 0, b = 0;
        memcpy(&a, ro, rs < 4 ? rs : 4);
        memcpy(&b, rn, rs < 4 ? rs : 4);
        printf("    return value: original %08x (%.9g), rewrite %08x (%.9g)\n", a, bits(a), b, bits(b));
    }
    for (int i = 0; i < ARENA; i += 4)
        if (memcmp(ar.a + i, ar.b + i, 4)) {
            uint32_t a, b;
            memcpy(&a, ar.a + i, 4);
            memcpy(&b, ar.b + i, 4);
            printf("    arena +%-4d original %08x (%.9g), rewrite %08x (%.9g)\n", i, a, bits(a), b, bits(b));
        }
    return it + 1;
}

// ---- the original, loaded at 0x400000 ---------------------------------------------------------------------
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
    // the launcher reserved the range before this process's heap existed (main); commit it
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

// The process heap and the loader's own allocations can land on 0x400000 before main runs. So the program
// starts itself again, suspended, reserves race.exe's range in that child before any of its code runs,
// and lets it go; the child (VP_FUZZ_CHILD set) does the work.
static int relaunch() {
    SetEnvironmentVariableA("VP_FUZZ_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) {
        printf("can't start the test process\n");
        return 2;
    }
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_FUZZ_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);                // a crash mustn't lose what was already printed
    int iterations = argc > 1 ? atoi(argv[1]) : 1000000;
    const char* filter = argc > 2 && strcmp(argv[2], "-") ? argv[2] : 0;   // "-": no filter
    if (argc > 3) g_state = (uint32_t)strtoul(argv[3], 0, 0) | 1;              // a seed
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);                        // ...\test\fuzz.cpp (built /FC) -> ...\out\race_v10.exe
    char* s = strstr(exe, "\\test\\fuzz.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);           // the physics thread's mode (ExceptSinglePrecision)
    int failed = 0, ran = 0;
    for (FuzzReg* r = FuzzReg::head(); r; r = r->next) {
        if (filter && !strstr(r->name, filter)) continue;
        int res = r->run(r->v10, iterations);
        if (res < 0) { printf("  %-28s not pure: needs the game (shadow mode)\n", r->name); continue; }
        ran++;
        if (res == 0) printf("  %-28s %d random calls: identical to the original\n", r->name, iterations);
        else { printf("  %-28s DIFFERS (above)\n", r->name); failed++; }
    }
    printf("%d rewrites run, %d differ\n", ran, failed);
    return failed ? 1 : 0;
}
