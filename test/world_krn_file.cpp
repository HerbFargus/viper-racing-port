// world_krn_file.cpp -- the kernel's files, log, window shell, messages and shared memory (hook/krn_file.cpp:
// file.obj, log.obj, win32.obj, winerror.obj, msg.obj, shmem.obj) against the originals, outside the game
// (docs/PORTING.md, step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_krn_file.cpp
//        /Fo%TEMP%\k2\ /Fe%TEMP%\k2\world_krn_file.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//        user32.lib
//   run:   world_krn_file.exe [scenarios] [seed] [moved]
//          moved: the open-file table first moved as M1 moves it in the game (hook/res_table_fields.inc: 256 slots in
//          memory of the harness's own, a guard page after them, alloc_file's imm8 capacity 0x7f), then the same
//          scenarios -- the originals run on the moved table, the rewrites must follow it through m1_operand
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS -- the rewrites as the game has them, run on the
//   inputs the fixes are for instead of the scenarios (fix_tests, at the end): the user directory next to a fake
//   race.exe and its one migration (from the VirtualStore's copies, the real folder, vrmod's relative "Config\"; the
//   old files untouched; never twice; the 200-character guard), the log's folder and FileVerifyNoOpenFiles,
//   FileReadLine on CRLF files (against the original), bare-LF and mixed ones (against a model) and exact lengths, and
//   the open-file table as M1 moves it: 300 files through the rewrites (256 open at once, the 257th panics, every
//   byte read back), the originals on the moved table (127 slots, then reuse, then their panic; their footprints
//   checked), the two mixed, nothing written past the table or into the stock one.
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does (a child process with the range reserved before
// its heap exists; 0xb0000 too, the mono monitor's memory). The loader doesn't resolve the game's imports; this
// harness fills its IAT itself: every slot these functions use gets a stub that logs the call and its arguments
// (strings hashed, stack pointers as a marker) and forwards to the real API -- file handles, find handles,
// mappings and mailslots are virtualised (the same small numbers in every pass), views are mapped at fixed
// addresses, paths are confined to a sandbox directory (a drive path outside it goes to <sandbox>\_drv_<X>\..., a
// UNC path other than a local mailslot fails as unreachable; mailslot names get a per-process prefix), GetLastError
// answers what the last stub saw, GetTempFileName(unique 0) is made deterministic. Everything that would touch
// windows, the cursor, processes, semaphores, the registry or the clock is a stub answering from a per-call script
// (never a real window, message box, clip or process). The other KERNEL32 slots (the game CRT's) are the real
// functions; any other import is a trap that ends the run. The game functions outside this group that these call
// are stubs too (Multi*, Sync*, Task*, MemAlloc / MemFree / operator delete, the key and mouse queues,
// create_window / hook_keys / Win32Idle, ExceptInstallExitHandler); the game's CRT (vsprintf, strncpy, strchr,
// stricmp, memmove, isspace, chdir) and abend run from the image -- abend writes to address 0, which is how a
// panic shows here (it ends the call in every pass). The CRT's fatal paths are stubs that end the child quietly.
//
// A scenario is a random script of calls: the file table (create / open / append / read / read lines / write /
// printf / flush / size / find / directories / copy / remove / attributes / memory maps / temp files / the
// 32-slot limit), the log (LogBegin with log.cfg variants -- the mailslot sink, the file sink --, reports and
// errors at the length limit, hooks, the '$' messages, the 1,000,000-byte limit, the mono monitor, every panic
// path), the window shell (the single-instance check, the user directory, the splash window, the window
// procedure's messages, the cursor, hooks, Win32System), messages over a real local mailslot, shared memory
// blocks, and the error names. Each scenario runs three times from the same start -- the sandbox rebuilt to the
// same files, the game's whole .data restored, the same random script: once with the originals, once with the
// rewrites called directly ("isolated": their callees in this group are still the originals) and once with every
// rewrite hooked into the image ("chain"). The passes must agree on every call's result (return, outputs, panic /
// fault), the API log (every stubbed call, in order, with its arguments), the game's .data and the mono memory,
// the arena (every buffer and output), what the log sinks received, and the sandbox's files (names, attributes,
// bytes). In the originals' pass, each call whose footprint isn't replay_only is checked to write only inside it.
// Win32GetErrorString(int) is also checked for every code from -0x10000 to 0x30000 and random ones.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <tuple>
#include <type_traits>
#ifndef FIX_TESTS
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#endif
#include "../hook/port.h"

struct ChainReg {
    uint32_t at; void* fn; const char* name; ChainReg* next;
    static ChainReg*& head() { static ChainReg* h; return h; }
    ChainReg(uint32_t a, void* f, const char* n) : at(a), fn(f), name(n), next(head()) { head() = this; }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;                                                        \
    static ChainReg VP_CAT(chain_, NEW)(V10, (void*)&NEW, NAME);

static std::string g_logf_text;                    // what the rewrites logged (the fix tests read it)
static bool g_logf_quiet;
void logf(const char* fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_logf_text += b;
    g_logf_text += "\n";
    if (!g_logf_quiet) printf("%s\n", b);
}
uint32_t A(uint32_t v10) { return v10; }
bool have(uint32_t) { return true; }
bool build_is_v10() { return true; }
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 0x40, what); }

#include "../hook/krn_file.cpp"

// ---- random values, hashes ----------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
template <typename T> static const T& pick(const std::vector<T>& v) { return v[rnd() % v.size()]; }
static const char* pickc(const char* const* v, int n) { return v[rnd() % (uint32_t)n]; }
static uint32_t hash_bytes(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static uint32_t hs(const char* s) { return s ? hash_bytes(s, strlen(s)) : 0xdead; }

// ---- the original, loaded at 0x400000 ------------------------------------------------------------------------------
enum { DATA_AT = 0x004e1000, DATA_SIZE = 0xf5f2c, MONO_AT = 0x000b0000, MONO_SIZE = 0x2000 };
static std::vector<uint8_t> g_pristine_data;       // .data as loaded (with the stubbed IAT), restored every pass
static IMAGE_NT_HEADERS* g_nt;
static uint8_t* g_file;

static bool load_race_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = (uint8_t*)malloc(n);
    fread(g_file, 1, n, f);
    fclose(f);
    g_nt = (IMAGE_NT_HEADERS*)(g_file + ((IMAGE_DOS_HEADER*)g_file)->e_lfanew);
    if (g_nt->FileHeader.TimeDateStamp != 0x362de68c) { printf("%s isn't the v1.0 race.exe\n", path); return false; }
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, g_nt->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) { printf("0x400000 isn't reserved for race.exe in this process\n"); return false; }
    memcpy(base, g_file, g_nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(g_nt);
    for (int i = 0; i < g_nt->FileHeader.NumberOfSections; i++)
        memcpy(base + sec[i].VirtualAddress, g_file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData
                                                                                         : sec[i].Misc.VirtualSize);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_K2_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) return 2;
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    if (!VirtualAllocEx(pi.hProcess, (void*)MONO_AT, 0x10000, MEM_RESERVE, PAGE_READWRITE)) {
        MEMORY_BASIC_INFORMATION mbi = {};
        VirtualQueryEx(pi.hProcess, (void*)MONO_AT, &mbi, sizeof mbi);
        printf("(0xb0000 was already reserved in the test process: state %lx, base %p, size %lx -- see the child's check)\n",
               mbi.State, mbi.AllocationBase, (unsigned long)mbi.RegionSize);
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// the chain: every rewrite of the group hooked into the image (for the chain pass)
struct ChainSave { uint32_t at; uint8_t b[5]; };
static ChainSave g_chain_save[256];
static int g_nchain;
static void chain_patch() {
    g_nchain = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        ChainSave& s = g_chain_save[g_nchain++];
        s.at = r->at;
        memcpy(s.b, (void*)r->at, 5);
        patch_jmp(r->at, r->fn);
    }
}
static void chain_unpatch() {
    for (int i = g_nchain - 1; i >= 0; i--) memcpy((void*)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_nchain = 0;
}
static void* rewrite_of(uint32_t at) {
    for (ChainReg* r = ChainReg::head(); r; r = r->next)
        if (r->at == at) return r->fn;
    printf("no rewrite registered at %08x\n", at);
    ExitProcess(3);
    return 0;
}

// ---- the arena -------------------------------------------------------------------------------------------------------
enum {
    ARENA_SIZE = 0x100000,
    OFF_STR = 0x00000, STR_SIZE = 0x10000,         // strings (paths, formats, names): a bump allocator per pass
    OFF_BUF = 0x10000, BUF_SIZE = 0x20000,         // read / write / printf buffers
    OFF_INT = 0x30000,                             // int cells: Files, counts, error codes
    OFF_MAP = 0x31000,                             // FileMemoryMaps (16 bytes apart)
    OFF_OUT = 0x32000, OUT_SIZE = 0x4000,          // names out (find, sub-directories, messages)
    OFF_HEAP = 0x80000, HEAP_SIZE = 0x80000,       // MemAlloc
};
static uint8_t* g_arena;
static uint32_t g_str_top, g_heap_top;
static char* astr(const char* s) {                   // a string in the arena (persists for the pass)
    size_t n = strlen(s) + 1;
    if (g_str_top + n > STR_SIZE) g_str_top = 0x8000;   // (wraps into the upper half: the scenario's old strings)
    char* p = (char*)g_arena + OFF_STR + g_str_top;
    memcpy(p, s, n);
    g_str_top += (uint32_t)((n + 3) & ~3u);
    return p;
}
static int* acell(int i) { return (int*)(g_arena + OFF_INT) + i; }
static FileMemoryMap* amap(int i) { return (FileMemoryMap*)(g_arena + OFF_MAP + 16 * i); }
static char* aout(int off) { return (char*)g_arena + OFF_OUT + off; }
static uint8_t* abuf(int off) { return g_arena + OFF_BUF + off; }

// ---- the API log ---------------------------------------------------------------------------------------------------------
#define LOG_KINDS(X)                                                                                                    \
    X(GetVersion) X(CreateFileA) X(GetTempFileNameA) X(SetFilePointer) X(CloseHandle) X(GetFileSize) X(GetLastError)     \
    X(ReadFile) X(WriteFile) X(DuplicateHandle) X(GetCurrentProcess) X(SetFileAttributesA) X(DeleteFileA)               \
    X(FindFirstFileA) X(FindNextFileA) X(FindClose) X(CreateDirectoryA) X(CopyFileA) X(CreateFileMappingA)              \
    X(MapViewOfFile) X(UnmapViewOfFile) X(CreateSemaphoreA) X(CreateProcessA) X(WaitForSingleObject)                    \
    X(GetExitCodeProcess) X(CreateMailslotA) X(SetCurrentDirectoryA) X(MessageBoxA) X(FindWindowA) X(ShowWindow)       \
    X(GetSystemMetrics) X(LoadCursorA) X(RegisterClassA) X(CreateWindowExA) X(UpdateWindow) X(LoadBitmapA)             \
    X(BeginPaint) X(EndPaint) X(DefWindowProcA) X(ShowCursor) X(PostQuitMessage) X(SetCursor) X(ClipCursor)             \
    X(CallNextHookEx) X(DestroyWindow) X(SetForegroundWindow) X(UnhookWindowsHookEx) X(DeleteObject)                   \
    X(CreateCompatibleDC) X(SelectObject) X(BitBlt) X(DeleteDC) X(RegFlushKey) X(timeGetTime)                           \
    X(MultiBegin) X(MultiEnter) X(MultiLeave) X(MultiEnd) X(ExitHandler) X(MemAlloc) X(MemFree) X(OpDelete)            \
    X(TaskGetID) X(TaskGetName) X(SyncMutexAlloc) X(SyncStrobeAlloc) X(SyncMutexLock) X(SyncMutexUnlock)               \
    X(SyncStrobeFlash) X(SyncStrobeWait) X(SyncLameLock) X(SyncLameUnlock) X(SyncMutexFree) X(SyncStrobeFree)          \
    X(KeyDown) X(KeyUp) X(KeyQueueChar) X(KeyQueueMetaChar) X(MouseQueueEvent) X(create_window) X(hook_keys)           \
    X(Win32Idle) X(Sink) X(SinkEnd) X(LogHook) X(MsgHook)
enum LogKind {
    L_NONE,
#define X(n) L_##n,
    LOG_KINDS(X)
#undef X
};
static const char* const g_log_names[] = {
    "?",
#define X(n) #n,
    LOG_KINDS(X)
#undef X
};
struct LogEntry { uint32_t kind, a[8]; };
static std::vector<LogEntry> g_api;
static std::string g_texts;                        // what the sinks, hooks and message boxes received, in order
static uint32_t g_script[64];
static int g_si;
static uint32_t script() { return g_script[g_si++ & 63]; }
static void lg(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0,
               uint32_t a5 = 0, uint32_t a6 = 0, uint32_t a7 = 0) {
    if (g_api.size() < 400000) g_api.push_back({kind, {a0, a1, a2, a3, a4, a5, a6, a7}});
}
// a pointer as the log sees it: the arena and the image as they are; the stack (whose layout differs between the
// original and the rewrite) as a marker
static uint32_t P(const void* p) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    if ((uint8_t*)p >= (uint8_t*)tib->StackLimit && (uint8_t*)p < (uint8_t*)tib->StackBase) return 0x57ac0000u;
    return (uint32_t)(uintptr_t)p;
}

// ---- the sandbox, virtual handles -----------------------------------------------------------------------------------------
static char g_sb[MAX_PATH];                        // the sandbox (no trailing backslash)
static char g_slot_prefix[64];                     // "\\.\mailslot\k2p<pid>\"
static uint32_t g_err;                             // what GetLastError answers: the last stub's error
static bool g_have_mono;

// a path, confined to the sandbox; false: unreachable (a UNC path that isn't a local mailslot)
static bool remap(const char* p, std::string& out) {
    if (!p) { out.clear(); return true; }
    const size_t sbn = strlen(g_sb);
    if (p[0] && p[1] == ':') {
        if (_strnicmp(p, g_sb, sbn) == 0 && (p[sbn] == 0 || p[sbn] == '\\')) { out = p; return true; }
        out = std::string(g_sb) + "\\_drv_" + (char)tolower((uint8_t)p[0]);
        if (p[2] != '\\') out += '\\';
        out += p + 2;
        return true;
    }
    if (p[0] == '\\' && p[1] == '\\') {
        if (_strnicmp(p, "\\\\.\\mailslot\\", 13) == 0) { out = std::string(g_slot_prefix) + (p + 13); return true; }
        return false;
    }
    if (p[0] == '\\' || p[0] == '/') { out = std::string(g_sb) + "\\_root" + p; return true; }
    if (strstr(p, "..")) return false;               // (never generated: nothing may leave the sandbox)
    out = p;
    return true;
}
enum { VK_FILE = 1, VK_FIND, VK_MAP, VK_FAKE };
struct VH { HANDLE real; int kind; };
static std::map<uint32_t, VH> g_vh;
static uint32_t g_vh_next;
static HANDLE vnew(HANDLE real, int kind) {
    const uint32_t v = 0x1000 + 4 * g_vh_next++;
    g_vh[v] = {real, kind};
    return (HANDLE)(uintptr_t)v;
}
static VH* vget(HANDLE v) {
    auto it = g_vh.find((uint32_t)(uintptr_t)v);
    return it == g_vh.end() ? 0 : &it->second;
}
static uint32_t V(const void* h) { return (uint32_t)(uintptr_t)h; }
static std::vector<void*> g_views;
static int g_view_next;
static long g_view_moved;                          // views that couldn't go to their fixed address (nondeterministic)

// ---- KERNEL32 stubs ---------------------------------------------------------------------------------------------------
static DWORD g_version;
static DWORD WINAPI st_GetVersion() { lg(L_GetVersion, g_version); return g_version; }
static DWORD WINAPI st_GetLastError() { lg(L_GetLastError, g_err); return g_err; }
static HANDLE WINAPI st_GetCurrentProcess() { lg(L_GetCurrentProcess); return (HANDLE)-1; }
static HANDLE WINAPI st_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    std::string r;
    HANDLE h = INVALID_HANDLE_VALUE;
    VH* t = vget(tmpl);
    if (!remap(name, r)) g_err = ERROR_BAD_NETPATH;
    else if (tmpl && tmpl != INVALID_HANDLE_VALUE && !t) g_err = ERROR_INVALID_HANDLE;
    else {
        HANDLE rh = CreateFileA(r.c_str(), acc, share, sa, disp, flags, t ? t->real : tmpl);
        g_err = GetLastError();
        if (rh != INVALID_HANDLE_VALUE) h = vnew(rh, VK_FILE);
    }
    lg(L_CreateFileA, hs(name), acc, share, P(sa), disp, flags, V(tmpl), V(h) ^ (g_err << 16));
    return h;
}
static UINT WINAPI st_GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out) {
    std::string d;
    UINT r = 0;
    if (!remap(dir, d)) g_err = ERROR_BAD_NETPATH;
    else if (unique) {
        r = GetTempFileNameA(d.c_str(), prefix, unique, out);
        g_err = GetLastError();
    } else {                                        // Windows starts from the clock; here from 1
        char tmp[MAX_PATH];
        for (UINT u = 1; u < 0x10000; u++) {
            if (!GetTempFileNameA(d.c_str(), prefix, u, tmp)) break;
            HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                strcpy(out, tmp);
                r = u;
                break;
            }
        }
        g_err = 0;
    }
    lg(L_GetTempFileNameA, hs(dir), hs(prefix), unique, P(out), r, r ? hs(out) : 0, g_err);
    return r;
}
static DWORD WINAPI st_SetFilePointer(HANDLE h, LONG dist, PLONG hi, DWORD how) {
    VH* v = vget(h);
    DWORD r = INVALID_SET_FILE_POINTER;
    if (!v) g_err = ERROR_INVALID_HANDLE;
    else { r = SetFilePointer(v->real, dist, hi, how); g_err = GetLastError(); }
    lg(L_SetFilePointer, V(h), (uint32_t)dist, P(hi), how, r, g_err);
    return r;
}
static BOOL WINAPI st_CloseHandle(HANDLE h) {
    VH* v = vget(h);
    BOOL ok = FALSE;
    if (!v) g_err = ERROR_INVALID_HANDLE;
    else {
        ok = v->kind == VK_FAKE ? TRUE : CloseHandle(v->real);
        g_err = ok ? 0 : GetLastError();
        g_vh.erase(V(h));
    }
    lg(L_CloseHandle, V(h), ok, g_err);
    return ok;
}
static DWORD WINAPI st_GetFileSize(HANDLE h, LPDWORD hi) {
    VH* v = vget(h);
    DWORD r = INVALID_FILE_SIZE;
    if (!v) g_err = ERROR_INVALID_HANDLE;
    else { r = GetFileSize(v->real, hi); g_err = GetLastError(); }
    lg(L_GetFileSize, V(h), P(hi), r, g_err);
    return r;
}
static BOOL WINAPI st_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    VH* v = vget(h);
    BOOL ok = FALSE;
    if (got) *got = 0;
    if (!v) g_err = ERROR_INVALID_HANDLE;
    else { ok = ReadFile(v->real, buf, n, got, ov); g_err = ok ? 0 : GetLastError(); }
    lg(L_ReadFile, V(h), P(buf), n, P(got), P(ov), ok, got ? *got : 0, ok && got ? hash_bytes(buf, *got) : g_err);
    return ok;
}
static BOOL WINAPI st_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov) {
    VH* v = vget(h);
    BOOL ok = FALSE;
    const uint32_t hb = hash_bytes(buf, n);
    if (put) *put = 0;
    if (!v) g_err = ERROR_INVALID_HANDLE;
    else { ok = WriteFile(v->real, buf, n, put, ov); g_err = ok ? 0 : GetLastError(); }
    lg(L_WriteFile, V(h), P(buf), n, P(put), P(ov), ok, put ? *put : 0, hb ^ g_err);
    return ok;
}
static BOOL WINAPI st_DuplicateHandle(HANDLE sp, HANDLE h, HANDLE tp, LPHANDLE out, DWORD acc, BOOL inh, DWORD opt) {
    VH* v = vget(h);
    BOOL ok = FALSE;
    uint32_t nv = 0;
    if (!v || sp != (HANDLE)-1 || tp != (HANDLE)-1) g_err = ERROR_INVALID_HANDLE;
    else {
        HANDLE d;
        const int kind = v->kind;
        ok = DuplicateHandle(GetCurrentProcess(), v->real, GetCurrentProcess(), &d, acc, inh, opt);
        g_err = ok ? 0 : GetLastError();
        if (ok) { *out = vnew(d, kind); nv = V(*out); }
    }
    lg(L_DuplicateHandle, V(sp), V(h), V(tp), P(out), acc, inh, opt, ok ? nv : 0);
    return ok;
}
static BOOL WINAPI st_SetFileAttributesA(LPCSTR name, DWORD a) {
    std::string r;
    BOOL ok = FALSE;
    if (!remap(name, r)) g_err = ERROR_BAD_NETPATH;
    else { ok = SetFileAttributesA(r.c_str(), a); g_err = ok ? 0 : GetLastError(); }
    lg(L_SetFileAttributesA, hs(name), a, ok, g_err);
    return ok;
}
static BOOL WINAPI st_DeleteFileA(LPCSTR name) {
    std::string r;
    BOOL ok = FALSE;
    if (!remap(name, r)) g_err = ERROR_BAD_NETPATH;
    else { ok = DeleteFileA(r.c_str()); g_err = ok ? 0 : GetLastError(); }
    lg(L_DeleteFileA, hs(name), ok, g_err);
    return ok;
}
static HANDLE WINAPI st_FindFirstFileA(LPCSTR pat, LPWIN32_FIND_DATAA fd) {
    std::string r;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!remap(pat, r)) g_err = ERROR_BAD_NETPATH;
    else {
        HANDLE rh = FindFirstFileA(r.c_str(), fd);
        g_err = rh == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (rh != INVALID_HANDLE_VALUE) h = vnew(rh, VK_FIND);
    }
    lg(L_FindFirstFileA, hs(pat), P(fd), V(h), h != INVALID_HANDLE_VALUE ? hs(fd->cFileName) : 0, g_err);
    return h;
}
static BOOL WINAPI st_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd) {
    VH* v = vget(h);
    BOOL ok = FALSE;
    if (!v || v->kind != VK_FIND) g_err = ERROR_INVALID_HANDLE;
    else { ok = FindNextFileA(v->real, fd); g_err = ok ? 0 : GetLastError(); }
    lg(L_FindNextFileA, V(h), P(fd), ok, ok ? hs(fd->cFileName) : 0, g_err);
    return ok;
}
static BOOL WINAPI st_FindClose(HANDLE h) {
    VH* v = vget(h);
    BOOL ok = FALSE;
    if (!v || v->kind != VK_FIND) g_err = ERROR_INVALID_HANDLE;
    else { ok = FindClose(v->real); g_err = 0; g_vh.erase(V(h)); }
    lg(L_FindClose, V(h), ok, g_err);
    return ok;
}
static BOOL WINAPI st_CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa) {
    std::string r;
    BOOL ok = FALSE;
    if (!remap(name, r)) g_err = ERROR_BAD_NETPATH;
    else { ok = CreateDirectoryA(r.c_str(), sa); g_err = ok ? 0 : GetLastError(); }
    lg(L_CreateDirectoryA, hs(name), P(sa), ok, g_err);
    return ok;
}
static BOOL WINAPI st_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail) {
    std::string a, b;
    BOOL ok = FALSE;
    if (!remap(from, a) || !remap(to, b)) g_err = ERROR_BAD_NETPATH;
    else { ok = CopyFileA(a.c_str(), b.c_str(), fail); g_err = ok ? 0 : GetLastError(); }
    lg(L_CopyFileA, hs(from), hs(to), fail, ok, g_err);
    return ok;
}
static BOOL WINAPI st_SetCurrentDirectoryA(LPCSTR dir) {
    std::string r;
    BOOL ok = FALSE;
    if (!remap(dir, r)) g_err = ERROR_BAD_NETPATH;
    else { ok = SetCurrentDirectoryA(r.c_str()); g_err = ok ? 0 : GetLastError(); }
    lg(L_SetCurrentDirectoryA, hs(dir), ok, g_err);
    return ok;
}
static HANDLE WINAPI st_CreateFileMappingA(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name) {
    VH* v = vget(h);
    HANDLE m = 0;
    if (!v || v->kind != VK_FILE) g_err = ERROR_INVALID_HANDLE;
    else {
        HANDLE rm = CreateFileMappingA(v->real, sa, prot, hi, lo, name);
        g_err = GetLastError();
        if (rm) m = vnew(rm, VK_MAP);
    }
    lg(L_CreateFileMappingA, V(h), P(sa), prot, hi, lo, hs(name), V(m), g_err);
    return m;
}
static LPVOID WINAPI st_MapViewOfFile(HANDLE m, DWORD acc, DWORD hi, DWORD lo, SIZE_T n) {
    VH* v = vget(m);
    void* p = 0;
    void* base = (void*)(uintptr_t)(0x38000000u + 0x01000000u * (uint32_t)(g_view_next++ & 15));
    if (!v || v->kind != VK_MAP) g_err = ERROR_INVALID_HANDLE;
    else {
        p = MapViewOfFileEx(v->real, acc, hi, lo, n, base);
        if (!p) {
            p = MapViewOfFile(v->real, acc, hi, lo, n);
            if (p) g_view_moved++;
        }
        g_err = p ? 0 : GetLastError();
        if (p) g_views.push_back(p);
    }
    lg(L_MapViewOfFile, V(m), acc, hi, lo, (uint32_t)n, P(p), g_err, p ? hash_bytes(p, 16) : 0);
    return p;
}
static BOOL WINAPI st_UnmapViewOfFile(LPCVOID p) {
    auto it = std::find(g_views.begin(), g_views.end(), (void*)p);
    BOOL ok = FALSE;
    if (it == g_views.end()) g_err = ERROR_INVALID_ADDRESS;
    else { ok = UnmapViewOfFile(p); g_err = 0; g_views.erase(it); }
    lg(L_UnmapViewOfFile, P(p), ok, g_err);
    return ok;
}
static HANDLE WINAPI st_CreateMailslotA(LPCSTR name, DWORD max, DWORD timeout, LPSECURITY_ATTRIBUTES sa) {
    std::string r;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!remap(name, r)) g_err = ERROR_BAD_NETPATH;
    else {
        HANDLE rh = CreateMailslotA(r.c_str(), max, timeout, sa);
        g_err = GetLastError();
        if (rh != INVALID_HANDLE_VALUE) h = vnew(rh, VK_FILE);
    }
    lg(L_CreateMailslotA, hs(name), max, timeout, P(sa), V(h), g_err);
    return h;
}
// scripted: the semaphore, the process
static HANDLE WINAPI st_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCSTR name) {
    const uint32_t s = script();
    HANDLE h = s % 10 == 0 ? 0 : vnew(0, VK_FAKE);
    g_err = (s >> 8) % 3 == 0 ? ERROR_ALREADY_EXISTS : (s % 10 == 0 ? ERROR_ACCESS_DENIED : 0);
    lg(L_CreateSemaphoreA, P(sa), (uint32_t)init, (uint32_t)max, hs(name), V(h), g_err);
    return h;
}
static int g_waits;                                // WaitForSingleObject's timeouts left
static BOOL WINAPI st_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh,
                                     DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi) {
    const uint32_t s = script();
    const BOOL ok = s % 5 != 0;
    const uint32_t hsi = hash_bytes(si, sizeof *si);
    if (ok) {
        pi->hProcess = vnew(0, VK_FAKE);
        pi->hThread = vnew(0, VK_FAKE);
        pi->dwProcessId = 0x100;
        pi->dwThreadId = 0x104;
        g_waits = (int)((s >> 8) % 4);
        g_err = 0;
    } else g_err = ERROR_FILE_NOT_FOUND;
    g_texts += "run: ";
    g_texts += cmd ? cmd : "";
    g_texts += "\n";
    lg(L_CreateProcessA, P(app) ^ hs(cmd), P(pa) ^ P(ta), inh, flags, P(env), P(dir), hsi, ok ? V(pi->hProcess) : 0);
    return ok;
}
static BOOL g_exit_ok;
static DWORD WINAPI st_WaitForSingleObject(HANDLE h, DWORD ms) {
    const DWORD r = g_waits > 0 ? (g_waits--, WAIT_TIMEOUT) : WAIT_OBJECT_0;
    lg(L_WaitForSingleObject, V(h), ms, r);
    return r;
}
static BOOL WINAPI st_GetExitCodeProcess(HANDLE h, LPDWORD code) {
    const uint32_t s = script();
    const BOOL ok = s % 7 != 0;
    if (ok) *code = s % 3 == 0 ? STILL_ACTIVE : (s >> 4) & 0xff;
    g_err = ok ? 0 : ERROR_INVALID_HANDLE;
    lg(L_GetExitCodeProcess, V(h), P(code), ok, ok ? *code : 0);
    return ok;
}

// ---- USER32 / GDI32 / ADVAPI32 / WINMM stubs: log and answer from the script ------------------------------------------
static int WINAPI st_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT type) {
    lg(L_MessageBoxA, V(w), hs(text), hs(cap), type);
    g_texts += "MessageBox[";
    g_texts += cap ? cap : "";
    g_texts += "]: ";
    g_texts += text ? text : "";
    g_texts += "\n";
    return IDOK;
}
static HWND WINAPI st_FindWindowA(LPCSTR cls, LPCSTR title) {
    const uint32_t s = script();
    HWND w = s % 2 ? (HWND)(uintptr_t)(0x00ab0000 + (s & 0xff0)) : 0;
    lg(L_FindWindowA, hs(cls), hs(title), V(w));
    return w;
}
static BOOL WINAPI st_ShowWindow(HWND w, int cmd) { const BOOL r = script() & 1; lg(L_ShowWindow, V(w), cmd, r); return r; }
static int WINAPI st_GetSystemMetrics(int i) { const int r = 640 + (int)(script() % 3300); lg(L_GetSystemMetrics, i, r); return r; }
static HCURSOR WINAPI st_LoadCursorA(HINSTANCE inst, LPCSTR id) { lg(L_LoadCursorA, V(inst), V(id)); return (HCURSOR)(uintptr_t)0xc0c0; }
static ATOM WINAPI st_RegisterClassA(const WNDCLASSA* wc) {
    const ATOM a = script() % 6 == 0 ? 0 : 0xc123;
    lg(L_RegisterClassA, wc->style, (uint32_t)(uintptr_t)wc->lpfnWndProc, (uint32_t)wc->cbClsExtra ^ ((uint32_t)wc->cbWndExtra << 16), V(wc->hInstance),
       V(wc->hIcon) ^ V(wc->hCursor), V(wc->hbrBackground), V(wc->lpszMenuName) ^ hs(wc->lpszClassName), a);
    return a;
}
static HWND WINAPI st_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent,
                                      HMENU menu, HINSTANCE inst, LPVOID param) {
    const uint32_t s = script();
    HWND r = s % 5 == 0 ? 0 : (HWND)(uintptr_t)(0x00cd0000 + (s & 0xff0));
    lg(L_CreateWindowExA, ex ^ hs(cls), hs(title), style, (uint32_t)x, (uint32_t)y, (uint32_t)w ^ ((uint32_t)h << 16),
       V(parent) ^ V(menu) ^ V(param), V(inst) ^ V(r));
    return r;
}
static BOOL WINAPI st_UpdateWindow(HWND w) { lg(L_UpdateWindow, V(w)); return TRUE; }
static HBITMAP WINAPI st_LoadBitmapA(HINSTANCE inst, LPCSTR name) {
    HBITMAP b = script() % 4 == 0 ? 0 : (HBITMAP)(uintptr_t)0xb1b0;
    lg(L_LoadBitmapA, V(inst), hs(name), V(b));
    return b;
}
static HDC WINAPI st_BeginPaint(HWND w, LPPAINTSTRUCT ps) { lg(L_BeginPaint, V(w), P(ps)); memset(ps, 0x5a, sizeof *ps); return (HDC)(uintptr_t)0xdc01; }
static BOOL WINAPI st_EndPaint(HWND w, const PAINTSTRUCT* ps) { lg(L_EndPaint, V(w), P(ps), hash_bytes(ps, sizeof *ps)); return TRUE; }
static LRESULT WINAPI st_DefWindowProcA(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    const LRESULT r = (LRESULT)(script() & 0xffff);
    lg(L_DefWindowProcA, V(w), m, (uint32_t)wp, (uint32_t)lp, (uint32_t)r);
    return r;
}
static int WINAPI st_ShowCursor(BOOL b) { lg(L_ShowCursor, b); return (int)(script() % 5) - 2; }
static void WINAPI st_PostQuitMessage(int c) { lg(L_PostQuitMessage, c); }
static HCURSOR WINAPI st_SetCursor(HCURSOR c) { lg(L_SetCursor, V(c)); return (HCURSOR)(uintptr_t)0xc0c1; }
static BOOL WINAPI st_ClipCursor(const RECT* r) {
    lg(L_ClipCursor, r ? 1 : 0, r ? (uint32_t)r->left : 0, r ? (uint32_t)r->top : 0, r ? (uint32_t)r->right : 0, r ? (uint32_t)r->bottom : 0);
    return TRUE;
}
static LRESULT WINAPI st_CallNextHookEx(HHOOK h, int code, WPARAM wp, LPARAM lp) {
    const LRESULT r = (LRESULT)script();
    lg(L_CallNextHookEx, V(h), (uint32_t)code, (uint32_t)wp, (uint32_t)lp, (uint32_t)r);
    return r;
}
static BOOL WINAPI st_DestroyWindow(HWND w) { lg(L_DestroyWindow, V(w)); return TRUE; }
static BOOL WINAPI st_SetForegroundWindow(HWND w) { lg(L_SetForegroundWindow, V(w)); return TRUE; }
static BOOL WINAPI st_UnhookWindowsHookEx(HHOOK h) { lg(L_UnhookWindowsHookEx, V(h)); return h != 0; }
static BOOL WINAPI st_DeleteObject(HGDIOBJ o) { lg(L_DeleteObject, V(o)); return TRUE; }
static HDC WINAPI st_CreateCompatibleDC(HDC dc) { lg(L_CreateCompatibleDC, V(dc)); return (HDC)(uintptr_t)0xdc02; }
static HGDIOBJ WINAPI st_SelectObject(HDC dc, HGDIOBJ o) { lg(L_SelectObject, V(dc), V(o)); return (HGDIOBJ)(uintptr_t)0x0b0b; }
static BOOL WINAPI st_BitBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop) {
    lg(L_BitBlt, V(d), (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, V(s), (uint32_t)sx ^ ((uint32_t)sy << 16), rop);
    return TRUE;
}
static BOOL WINAPI st_DeleteDC(HDC dc) { lg(L_DeleteDC, V(dc)); return TRUE; }
static LSTATUS WINAPI st_RegFlushKey(HKEY k) { lg(L_RegFlushKey, V(k)); return ERROR_SUCCESS; }
static DWORD WINAPI st_timeGetTime() { const DWORD t = script(); lg(L_timeGetTime, t); return t; }

#ifdef FIX_TESTS
// the fixes find race.exe's folder with GetModuleFileNameA(NULL): a fake exe path in the sandbox ("": it fails)
static std::string g_fake_exe;
static DWORD WINAPI st_GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    if (m || g_fake_exe.empty() || n == 0) return 0;
    const DWORD len = (DWORD)g_fake_exe.size();
    if (len >= n) {                                // cut short, as Windows does: n characters, n - 1 of them the path's
        memcpy(buf, g_fake_exe.c_str(), n - 1);
        buf[n - 1] = 0;
        return n;
    }
    memcpy(buf, g_fake_exe.c_str(), len + 1);
    return len;
}
#endif
// the slots, by the imported name (every one these functions use)
static const struct { const char* name; void* stub; } k_iat_stubs[] = {
#ifdef FIX_TESTS
    {"GetModuleFileNameA", (void*)&st_GetModuleFileNameA},
#endif
    {"GetVersion", (void*)&st_GetVersion}, {"GetLastError", (void*)&st_GetLastError},
    {"GetCurrentProcess", (void*)&st_GetCurrentProcess}, {"CreateFileA", (void*)&st_CreateFileA},
    {"GetTempFileNameA", (void*)&st_GetTempFileNameA}, {"SetFilePointer", (void*)&st_SetFilePointer},
    {"CloseHandle", (void*)&st_CloseHandle}, {"GetFileSize", (void*)&st_GetFileSize}, {"ReadFile", (void*)&st_ReadFile},
    {"WriteFile", (void*)&st_WriteFile}, {"DuplicateHandle", (void*)&st_DuplicateHandle},
    {"SetFileAttributesA", (void*)&st_SetFileAttributesA}, {"DeleteFileA", (void*)&st_DeleteFileA},
    {"FindFirstFileA", (void*)&st_FindFirstFileA}, {"FindNextFileA", (void*)&st_FindNextFileA},
    {"FindClose", (void*)&st_FindClose}, {"CreateDirectoryA", (void*)&st_CreateDirectoryA},
    {"CopyFileA", (void*)&st_CopyFileA}, {"SetCurrentDirectoryA", (void*)&st_SetCurrentDirectoryA},
    {"CreateFileMappingA", (void*)&st_CreateFileMappingA}, {"MapViewOfFile", (void*)&st_MapViewOfFile},
    {"UnmapViewOfFile", (void*)&st_UnmapViewOfFile}, {"CreateMailslotA", (void*)&st_CreateMailslotA},
    {"CreateSemaphoreA", (void*)&st_CreateSemaphoreA}, {"CreateProcessA", (void*)&st_CreateProcessA},
    {"WaitForSingleObject", (void*)&st_WaitForSingleObject}, {"GetExitCodeProcess", (void*)&st_GetExitCodeProcess},
    {"MessageBoxA", (void*)&st_MessageBoxA}, {"FindWindowA", (void*)&st_FindWindowA}, {"ShowWindow", (void*)&st_ShowWindow},
    {"GetSystemMetrics", (void*)&st_GetSystemMetrics}, {"LoadCursorA", (void*)&st_LoadCursorA},
    {"RegisterClassA", (void*)&st_RegisterClassA}, {"CreateWindowExA", (void*)&st_CreateWindowExA},
    {"UpdateWindow", (void*)&st_UpdateWindow}, {"LoadBitmapA", (void*)&st_LoadBitmapA}, {"BeginPaint", (void*)&st_BeginPaint},
    {"EndPaint", (void*)&st_EndPaint}, {"DefWindowProcA", (void*)&st_DefWindowProcA}, {"ShowCursor", (void*)&st_ShowCursor},
    {"PostQuitMessage", (void*)&st_PostQuitMessage}, {"SetCursor", (void*)&st_SetCursor}, {"ClipCursor", (void*)&st_ClipCursor},
    {"CallNextHookEx", (void*)&st_CallNextHookEx}, {"DestroyWindow", (void*)&st_DestroyWindow},
    {"SetForegroundWindow", (void*)&st_SetForegroundWindow}, {"UnhookWindowsHookEx", (void*)&st_UnhookWindowsHookEx},
    {"DeleteObject", (void*)&st_DeleteObject}, {"CreateCompatibleDC", (void*)&st_CreateCompatibleDC},
    {"SelectObject", (void*)&st_SelectObject}, {"BitBlt", (void*)&st_BitBlt}, {"DeleteDC", (void*)&st_DeleteDC},
    {"RegFlushKey", (void*)&st_RegFlushKey}, {"timeGetTime", (void*)&st_timeGetTime},
};
// any other import but KERNEL32's: a trap
static void __cdecl trap_import(uint32_t slot) {
    printf("an unexpected import was called (IAT slot %08x): stopping\n", slot);
    ExitProcess(5);
}
static int g_nstubbed, g_nreal, g_ntrapped;
static void install_iat() {
    uint8_t* base = (uint8_t*)0x400000;
    IMAGE_DATA_DIRECTORY dir = g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    uint8_t* thunks = (uint8_t*)VirtualAlloc(0, 0x10000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    int nt = 0;
    HMODULE k32m = GetModuleHandleA("kernel32.dll");
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.Size && d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        const bool k32 = _stricmp(dll, "KERNEL32.dll") == 0;
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            const char* fn = names->u1.Ordinal & IMAGE_ORDINAL_FLAG32 ? "" : (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            void* to = 0;
            for (auto& s : k_iat_stubs)
                if (!strcmp(s.name, fn)) to = s.stub;
            if (to) g_nstubbed++;
            else if (k32 && *fn && (to = (void*)GetProcAddress(k32m, fn)) != 0) g_nreal++;
            else {                                                  // push slot; mov eax, trap; call eax
                uint8_t* t = thunks + 16 * nt++;
                const uint32_t slot = (uint32_t)(uintptr_t)&iat->u1.Function, fnp = (uint32_t)(uintptr_t)&trap_import;
                t[0] = 0x68; memcpy(t + 1, &slot, 4);
                t[5] = 0xb8; memcpy(t + 6, &fnp, 4);
                t[10] = 0xff; t[11] = 0xd0;
                to = t;
                g_ntrapped++;
            }
            iat->u1.Function = (DWORD)(uintptr_t)to;
        }
    }
}

// ---- the other groups' functions, stubbed -----------------------------------------------------------------------------
static int g_task = 1;
static const char* const k_task_names[4] = {"<task0>", "Main", "Physics", "Sound"};
static bool g_alloc_fail;
static int __cdecl st_MultiBegin(const char* n) { const int h = (int)(script() % 16) + 1; lg(L_MultiBegin, hs(n), h); return h; }
static void __cdecl st_MultiEnter(int h, const char* f, int l) { lg(L_MultiEnter, h, P(f), l); }
static void __cdecl st_MultiLeave(int h, const char* f, int l) { lg(L_MultiLeave, h, P(f), l); }
static void __cdecl st_MultiEnd(int h, const char* f, int l) { lg(L_MultiEnd, h, P(f), l); }
static void __cdecl st_ExitHandler(void* fn) { lg(L_ExitHandler, P(fn)); }
static void* __cdecl st_MemAlloc(int n) {
    void* p = 0;
    const uint32_t sz = (((uint32_t)n + 15) & ~15u) + 16;
    if (!g_alloc_fail && n >= 0 && n < HEAP_SIZE && g_heap_top + sz <= HEAP_SIZE) {
        p = g_arena + OFF_HEAP + g_heap_top;
        g_heap_top += sz;
    }
    g_alloc_fail = false;
    lg(L_MemAlloc, (uint32_t)n, P(p));
    return p;
}
static void __cdecl st_MemFree(void* p) { lg(L_MemFree, P(p)); }
static void __cdecl st_OpDelete(void* p) { lg(L_OpDelete, P(p)); }
static int __cdecl st_TaskGetID() { lg(L_TaskGetID, g_task); return g_task; }
static const char* __cdecl st_TaskGetName(int id) { lg(L_TaskGetName, (uint32_t)id); return k_task_names[id & 3]; }
static void* __cdecl st_SyncMutexAlloc() { const uint32_t s = script(); void* p = s % 25 == 0 ? 0 : (void*)(uintptr_t)(0x00ee0000 + (s & 0xfff0)); lg(L_SyncMutexAlloc, P(p)); return p; }
static void* __cdecl st_SyncStrobeAlloc() { const uint32_t s = script(); void* p = s % 25 == 0 ? 0 : (void*)(uintptr_t)(0x00ef0000 + (s & 0xfff0)); lg(L_SyncStrobeAlloc, P(p)); return p; }
static void __cdecl st_SyncMutexLock(void* p) { lg(L_SyncMutexLock, P(p)); }
static void __cdecl st_SyncMutexUnlock(void* p) { lg(L_SyncMutexUnlock, P(p)); }
static void __cdecl st_SyncStrobeFlash(void* p) { lg(L_SyncStrobeFlash, P(p)); }
static void __cdecl st_SyncStrobeWait(void* p) { lg(L_SyncStrobeWait, P(p)); }
static void __cdecl st_SyncLameLock(void* p) { lg(L_SyncLameLock, P(p)); }
static void __cdecl st_SyncLameUnlock(void* p) { lg(L_SyncLameUnlock, P(p)); }
static void __cdecl st_SyncMutexFree(void** pp) { lg(L_SyncMutexFree, P(pp), P(*pp)); *pp = 0; }
static void __cdecl st_SyncStrobeFree(void** pp) { lg(L_SyncStrobeFree, P(pp), P(*pp)); *pp = 0; }
static void __cdecl st_KeyDown(uint32_t k) { lg(L_KeyDown, k); }
static void __cdecl st_KeyUp(uint32_t k) { lg(L_KeyUp, k); }
static void __cdecl st_KeyQueueChar(uint32_t k, uint32_t c) { lg(L_KeyQueueChar, k, c); }            // (c: the whole pushed dword)
static void __cdecl st_KeyQueueMetaChar(uint32_t k, uint32_t c) { lg(L_KeyQueueMetaChar, k, c); }
static void __cdecl st_MouseQueueEvent(int t, int x, int y, int s) { lg(L_MouseQueueEvent, (uint32_t)t, (uint32_t)x, (uint32_t)y, (uint32_t)s); }
static uint8_t __cdecl st_create_window(void* inst) { const uint8_t r = (uint8_t)(script() % 4 != 0); lg(L_create_window, P(inst), r); return r; }
static void __cdecl st_hook_keys() { lg(L_hook_keys); }
static void __cdecl st_Win32Idle() { lg(L_Win32Idle); }
// the log's sinks and hooks, and the message hooks
static void __cdecl st_sink(const char* t) {
    lg(L_Sink, hs(t), (uint32_t)strlen(t));
    g_texts += "log: ";
    g_texts += t;
    g_texts += "\n";
}
static void __cdecl st_sink_end() { lg(L_SinkEnd); }
template <int K> static void __cdecl st_log_hook(const char* t) { lg(L_LogHook, K, hs(t)); }
static LogFn_t const k_log_hooks[12] = {&st_log_hook<0>, &st_log_hook<1>, &st_log_hook<2>, &st_log_hook<3>, &st_log_hook<4>, &st_log_hook<5>,
                                        &st_log_hook<6>, &st_log_hook<7>, &st_log_hook<8>, &st_log_hook<9>, &st_log_hook<10>, &st_log_hook<11>};
template <int K> static uint8_t __cdecl st_msg_hook(uint32_t m, int wp, int lp) { lg(L_MsgHook, K, m, (uint32_t)wp, (uint32_t)lp); return (uint8_t)script(); }
static MsgHook_t const k_msg_hooks[3] = {&st_msg_hook<0>, &st_msg_hook<1>, &st_msg_hook<2>};

// the game's CRT must never put up a message box: its fatal-error paths end the child quietly
static void __cdecl st_crt_fatal(int code) { printf("the game's CRT hit a fatal error (%d): stopping\n", code); ExitProcess(4); }
static int __cdecl st_crt_msgbox(const char* text, const char*, unsigned) { printf("the game's CRT tried a message box: %s\n", text ? text : "?"); ExitProcess(4); return 0; }

static void install_stubs() {
    struct { uint32_t at; void* to; } p[] = {
        {0x004150a0, (void*)&st_MultiBegin}, {0x00415180, (void*)&st_MultiEnter}, {0x004151d0, (void*)&st_MultiLeave},
        {0x00415220, (void*)&st_MultiEnd}, {0x00415b90, (void*)&st_ExitHandler}, {0x004140e0, (void*)&st_MemAlloc},
        {0x00414300, (void*)&st_MemFree}, {0x00414390, (void*)&st_OpDelete}, {0x00414dd0, (void*)&st_TaskGetID},
        {0x00414ec0, (void*)&st_TaskGetName}, {0x004152a0, (void*)&st_SyncMutexAlloc}, {0x00415370, (void*)&st_SyncStrobeAlloc},
        {0x00415330, (void*)&st_SyncMutexLock}, {0x00415340, (void*)&st_SyncMutexUnlock}, {0x004153b0, (void*)&st_SyncStrobeFlash},
        {0x00415380, (void*)&st_SyncStrobeWait}, {0x004153e0, (void*)&st_SyncLameLock}, {0x00415410, (void*)&st_SyncLameUnlock},
        {0x00415350, (void*)&st_SyncMutexFree}, {0x004153c0, (void*)&st_SyncStrobeFree}, {0x00413e20, (void*)&st_KeyDown},
        {0x00413e70, (void*)&st_KeyUp}, {0x00413dd0, (void*)&st_KeyQueueChar}, {0x00413df0, (void*)&st_KeyQueueMetaChar},
        {0x00414530, (void*)&st_MouseQueueEvent}, {0x00412690, (void*)&st_create_window}, {0x00412ac0, (void*)&st_hook_keys},
        {0x00412bf0, (void*)&st_Win32Idle},
    };
    for (auto& x : p) patch_jmp(x.at, x.to);
    // the CRT's fatal paths (amsg_exit, NMSG_WRITE, FF_MSGBANNER, _fptrap, __crtMessageBoxA)
    patch_jmp(0x004cf730, (void*)&st_crt_fatal);
    patch_jmp(0x004d45b0, (void*)&st_crt_fatal);
    patch_jmp(0x004d4570, (void*)&st_crt_fatal);
    patch_jmp(0x004d5c10, (void*)&st_crt_fatal);
    patch_jmp(0x004d6850, (void*)&st_crt_msgbox);
    // floating point in the game's CRT, as its start-up leaves it (_fpmath -> _cfltcvt_init: vsprintf's %f)
    ((void(__cdecl*)())0x004ce150)();
}

// ---- the sandbox's files -----------------------------------------------------------------------------------------------
struct Fixture { std::string path; std::vector<uint8_t> data; DWORD attrs; };
static std::vector<Fixture> g_fixtures;
static const char* const k_fixture_dirs[] = {"dir1", "dir1\\sub", "Dir2", "My Documents", "Config", "_drv_c", "_drv_c\\Program Files", "_drv_d"};
static std::string g_long_name;                    // a 170-character file name (find's strncpy)
static void add_fixture(const std::string& path, const std::string& text, DWORD attrs = 0) {
    g_fixtures.push_back({path, std::vector<uint8_t>(text.begin(), text.end()), attrs});
}
static void load_fixtures(const char* game_dir) {
    add_fixture("a.txt", "first line\r\n\r\n  indented\tline\r\n" + std::string(200, 'x') + "\r\nlast line without a newline");
    add_fixture("lf.txt", "\nsecond\nthird line\n\nfifth");
    add_fixture("empty.txt", "");
    std::string bin(5000, 0);
    uint32_t s = 12345;
    for (char& c : bin) { s = s * 1103515245u + 12345u; c = (char)(s >> 16); }
    add_fixture("bin.dat", bin);
    add_fixture("ro.txt", "read only\r\n", FILE_ATTRIBUTE_READONLY);
    add_fixture("dir1\\b.txt", "b\r\n");
    add_fixture("dir1\\sub\\c.txt", "c file\r\nline two\r\n");
    add_fixture("My Documents\\notes.txt", "notes\r\n");
    g_long_name = std::string(170, 'L') + ".txt";
    add_fixture(g_long_name, "long\r\n");
    // the game's own files, read-only from the test install
    const char* const game[] = {"options.def", "tune.def", "readme.txt", "english.lng", "hastings.trm", "drivers.res", "Config\\bemidji.sco"};
    for (const char* g : game) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", game_dir, g);
        FILE* f = fopen(p, "rb");
        if (!f) continue;
        std::string d;
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) d.append(b, n);
        fclose(f);
        add_fixture(g, d);
    }
}
static void rm_tree(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p);
            else { SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(p.c_str()); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    SetFileAttributesA(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryA(dir.c_str());
}
static void put_file(const std::string& rel, const void* data, size_t n, DWORD attrs, bool must = true) {
    const std::string p = std::string(g_sb) + "\\" + rel;
    SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE && !must) return;                // (log.cfg, still open from an earlier LogBegin)
    if (h == INVALID_HANDLE_VALUE) { printf("can't make %s (%lu)\n", p.c_str(), GetLastError()); ExitProcess(3); }
    DWORD put;
    if (n) WriteFile(h, data, (DWORD)n, &put, 0);
    CloseHandle(h);
    if (attrs) SetFileAttributesA(p.c_str(), attrs);
}
static void sandbox_build() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    SetCurrentDirectoryA(tmp);
    rm_tree(g_sb);
    if (!CreateDirectoryA(g_sb, 0)) { printf("can't make the sandbox %s (%lu)\n", g_sb, GetLastError()); ExitProcess(3); }
    for (const char* d : k_fixture_dirs) CreateDirectoryA((std::string(g_sb) + "\\" + d).c_str(), 0);
    for (const Fixture& f : g_fixtures) put_file(f.path, f.data.data(), f.data.size(), f.attrs);
    SetCurrentDirectoryA(g_sb);
}
// every file and directory under the sandbox: name, attributes, size, bytes
static void snapshot_tree(const std::string& dir, const std::string& rel, std::vector<std::string>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string p = dir + "\\" + fd.cFileName, r = rel + fd.cFileName;
        char line[512];
        const DWORD a = fd.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                               FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_TEMPORARY);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            snprintf(line, sizeof line, "%s\\ [%lx]", r.c_str(), a);
            out.push_back(line);
            snapshot_tree(p, r + "\\", out);
        } else {
            uint32_t hh = 0;
            HANDLE f = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
            if (f != INVALID_HANDLE_VALUE) {
                std::vector<uint8_t> b(fd.nFileSizeLow);
                DWORD got = 0;
                if (!b.empty()) ReadFile(f, b.data(), (DWORD)b.size(), &got, 0);
                hh = hash_bytes(b.data(), got);
                CloseHandle(f);
            } else hh = 0xbadf11e;
            snprintf(line, sizeof line, "%s [%lx] %lu %08x", r.c_str(), a, fd.nFileSizeLow, hh);
            out.push_back(line);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---- the open-file table, moved as M1 moves it (the "moved" runs and the fix tests) ---------------------------------------
enum { MOVED_BYTES = VP_LIFT_FILES * 0x104, MOVED_SPAN = (MOVED_BYTES + 0xfff) / 0x1000 * 0x1000 + 0x1000 };   // + a guard page
static uint8_t* g_moved;                           // the table's memory (0: never moved)
static bool g_moved_on;                            // the image's operands point at it
static void move_file_table(bool on) {
    struct Ref { uint32_t at; uint8_t off; uint32_t old; int table; };
    static const Ref refs[] = {
#include "../hook/res_table_fields.inc"
    };
    if (!g_moved) g_moved = (uint8_t*)VirtualAlloc(0, MOVED_SPAN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const uint32_t base = (uint32_t)(uintptr_t)g_moved, end = base + MOVED_BYTES;
    for (const Ref& r : refs) {
        if (r.table < 2) continue;                 // (the options and language tables: krn_res's)
        uint32_t* p = (uint32_t*)(uintptr_t)(r.at + r.off);
        const uint32_t neu = r.table == 2 ? base + (r.old - 0x505ea8) : end + (r.old - 0x507f28);
        if (*p != (on ? r.old : neu)) { printf("res_table_fields.inc: %08x+%u isn't what it should be\n", r.at, r.off); ExitProcess(3); }
        *p = on ? neu : r.old;
    }
    *(uint8_t*)0x00411652 = on ? 0x7f : 0x20;      // alloc_file: cmp edx, 0x20 (an imm8)
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
    g_moved_on = on;
}

// ---- the passes ---------------------------------------------------------------------------------------------------------
struct GRange { uint32_t at, n; const char* what; };
static GRange g_granges[] = {
    {0x004e5a1c, 0x1c, "log statics (0x4e5a1c..)"}, {0x004e5c54, 8, "file statics"}, {0x004e5eac, 0x24, "win32 statics"},
    {0x004e6a74, 4, "abend flag"}, {0x00503a74, 4, "shmem count"}, {0x00505e58, 0x44, "log statics (0x505e58..)"},
    {0x00505ea0, 0x2088, "file table"}, {0x00507f30, 4, "splash bitmap"}, {0x00507f48, 0x104, "user directory"},
    {0x00508060, 8, "message hooks"}, {0x00508d38, 0x108, "message and mailslot"}, {0x005d5768, 0x28, "shmem blocks"},
    {0, 0, "the open-file table, moved"},        // (filled in when it is)
};
static uint32_t ghash() {
    uint32_t h = 0;
    for (const GRange& g : g_granges) h = h * 31 + hash_bytes((void*)(uintptr_t)g.at, g.n);
    return h;
}
static void mask_maps() {                          // FileMemoryMap's pad bytes are stack garbage: not compared
    for (int i = 0; i < 16; i++) memset((uint8_t*)amap(i) + 9, 0, 3);
}
static uint32_t ahash() {
    mask_maps();
    return hash_bytes(g_arena + OFF_BUF, OFF_OUT + OUT_SIZE - OFF_BUF);
}

struct Trace { uint32_t op; int result; uint32_t code, data; uint64_t ret; uint32_t nlog, gh, ah; };
struct PassResult {
    std::vector<Trace> trace;
    std::vector<const char*> names;
    std::vector<LogEntry> api;
    std::vector<uint8_t> data, mono, arena;
    std::vector<std::string> files;
    std::string texts;
};
static PassResult g_res[3];
static PassResult* g_cur;
static int g_mode;                                 // 0 the originals, 1 the rewrites called directly, 2 the chain
static HANDLE g_logger_slot = INVALID_HANDLE_VALUE; // the harness's own server for the log's mailslot sink

struct Stat { long calls, panics, faults, fails[3], fp_fails; };
static std::map<std::string, Stat> g_stats;
static long g_scenarios_failed;

static void pass_begin(int mode, uint32_t seed) {
    g_mode = mode;
    g_cur = &g_res[mode];
    *g_cur = PassResult();
    memcpy((void*)DATA_AT, g_pristine_data.data(), DATA_SIZE);
    if (g_moved) memset(g_moved, 0, MOVED_SPAN);
    if (g_have_mono) memset((void*)MONO_AT, 0, MONO_SIZE);
    memset(g_arena, 0, ARENA_SIZE);
    g_str_top = 0;
    g_heap_top = 0;
    g_api.clear();
    g_texts.clear();
    g_vh.clear();
    g_vh_next = 0;
    g_views.clear();
    g_view_next = 0;
    g_err = 0;
    g_task = 1;
    g_alloc_fail = false;
    g_waits = 0;
    g_version = 0x23f00206;                        // NT (Windows 10's GetVersion)
    sandbox_build();
    g_rng = seed;
    if (mode == 2) chain_patch();
}
static void pass_end() {
    if (g_mode == 2) chain_unpatch();
    if (g_logger_slot != INVALID_HANDLE_VALUE) {   // what reached the log's mailslot
        char msg[0x200];
        DWORD got;
        while (ReadFile(g_logger_slot, msg, sizeof msg, &got, 0)) {
            char line[0x140];
            snprintf(line, sizeof line, "slot[%u]: #%d %.*s\n", (unsigned)got, got >= 4 ? *(int*)msg : -1,
                     got > 4 ? (int)strnlen(msg + 4, got - 4) : 0, msg + 4);
            g_texts += line;
        }
        CloseHandle(g_logger_slot);
        g_logger_slot = INVALID_HANDLE_VALUE;
    }
    for (auto& v : g_vh) {
        if (v.second.kind == VK_FIND) FindClose(v.second.real);
        else if (v.second.kind != VK_FAKE) CloseHandle(v.second.real);
    }
    g_vh.clear();
    for (void* p : g_views) UnmapViewOfFile(p);
    g_views.clear();
    SetCurrentDirectoryA(g_sb);
    PassResult& r = *g_cur;
    r.api = g_api;
    r.texts = g_texts;
    r.data.assign((uint8_t*)DATA_AT, (uint8_t*)DATA_AT + DATA_SIZE);
    if (g_moved) r.data.insert(r.data.end(), g_moved, g_moved + MOVED_SPAN);   // (after .data: the moved table, its guard)
    if (g_have_mono) r.mono.assign((uint8_t*)MONO_AT, (uint8_t*)MONO_AT + MONO_SIZE);
    mask_maps();
    r.arena.assign(g_arena, g_arena + ARENA_SIZE);
    snapshot_tree(g_sb, "", r.files);
}

// ---- one call --------------------------------------------------------------------------------------------------------------
static __declspec(noinline) void stack_fill(uint32_t pat) {
    volatile uint32_t buf[0x3000];
    for (int i = 0; i < 0x3000; i++) buf[i] = pat;
}
static uint32_t g_fault_code, g_fault_at, g_fault_data;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    g_fault_data = e->ExceptionRecord->NumberParameters >= 2 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
template <typename Run> static int guarded(Run& run, uint64_t* ret) {
    __try {
        *ret = run();
        return 0;
    } __except (fault_filter(GetExceptionInformation())) {
        return g_fault_code == EXCEPTION_ACCESS_VIOLATION && g_fault_at == 0x00416049 ? 1 : 2;   // abend: a panic
    }
}
template <typename F> static int guarded_fp(F& f) {
    __try {
        f();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
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
template <typename F> static F pickf(uint32_t addr, F rw) { return g_mode == 1 ? rw : (F)(uintptr_t)addr; }

// the footprint check (the originals' pass): every byte the call changed in the game's .data, the mono memory and the
// arena must lie inside its footprint
static Footprint g_fp;
static std::vector<uint8_t> g_before_data, g_before_arena, g_before_mono, g_before_moved;
static bool in_footprint(uint32_t a) {
    for (int k = 0; k < g_fp.n; k++)
        if (a >= (uint32_t)(uintptr_t)g_fp.r[k].p && a < (uint32_t)(uintptr_t)g_fp.r[k].p + g_fp.r[k].n) return true;
    return false;
}
static void fp_check(const char* name) {
    Stat& st = g_stats[name];
    const uint8_t* d = (const uint8_t*)DATA_AT;
    for (uint32_t i = 0; i < DATA_SIZE; i++)
        if (d[i] != g_before_data[i] && !in_footprint(DATA_AT + i)) {
            if (st.fp_fails++ < 3) printf("FOOTPRINT %s: %08x (.data) changed outside it\n", name, DATA_AT + i);
            return;
        }
    if (g_moved)
        for (uint32_t i = 0; i < MOVED_SPAN; i++)
            if (g_moved[i] != g_before_moved[i] && !in_footprint((uint32_t)(uintptr_t)(g_moved + i))) {
                if (st.fp_fails++ < 3) printf("FOOTPRINT %s: the moved open-file table +0x%x changed outside it\n", name, i);
                return;
            }
    for (uint32_t i = 0; i < ARENA_SIZE; i++)
        if (g_arena[i] != g_before_arena[i] && !in_footprint((uint32_t)(uintptr_t)(g_arena + i))) {
            if (st.fp_fails++ < 3) printf("FOOTPRINT %s: arena+0x%x changed outside it\n", name, i);
            return;
        }
    if (g_have_mono)
        for (uint32_t i = 0; i < MONO_SIZE; i++)
            if (((uint8_t*)MONO_AT)[i] != g_before_mono[i] && !in_footprint(MONO_AT + i)) {
                if (st.fp_fails++ < 3) printf("FOOTPRINT %s: mono+0x%x changed outside it\n", name, i);
                return;
            }
}

template <typename Run, typename Fp> static uint64_t run_op(const char* name, Run run, Fp footprint) {
    for (int i = 0; i < 64; i++) g_script[i] = rnd();
    g_si = 0;
    const uint32_t pat = rnd();
    bool check = false;
    if (g_mode == 0) {
        g_fp.n = 0;
        g_fp.replay_only = 0;
        g_fp.pure = false;
        auto f = [&]() { footprint(g_fp); };
        check = !guarded_fp(f) && !g_fp.replay_only;
        if (check) {
            g_before_data.assign((uint8_t*)DATA_AT, (uint8_t*)DATA_AT + DATA_SIZE);
            g_before_arena.assign(g_arena, g_arena + ARENA_SIZE);
            if (g_moved) g_before_moved.assign(g_moved, g_moved + MOVED_SPAN);
            if (g_have_mono) g_before_mono.assign((uint8_t*)MONO_AT, (uint8_t*)MONO_AT + MONO_SIZE);
        }
    }
    stack_fill(pat);
    uint64_t ret = 0;
    const int r = guarded(run, &ret);
    Trace t;
    memset(&t, 0, sizeof t);
    t.op = hs(name);
    t.result = r;
    t.code = r ? g_fault_code : 0;
    t.data = r == 2 ? g_fault_data : 0;
    t.ret = r ? 0 : ret;
    t.nlog = (uint32_t)g_api.size();
    t.gh = ghash();
    t.ah = ahash();
    g_cur->trace.push_back(t);
    g_cur->names.push_back(name);
    if (g_mode == 0) {
        Stat& st = g_stats[name];
        st.calls++;
        if (r == 1) st.panics++;
        if (r == 2) st.faults++;
        if (check && r == 0) fp_check(name);
    }
    return t.ret;
}
// (each argument evaluated once, before the call: they may consume random numbers or allocate strings)
#define CALL(FN, ...) [&]() { auto args_ = std::make_tuple(__VA_ARGS__);                                                  return run_op(VP_CAT(name_, FN),                                                                                                    [&]() { auto f_ = pickf(VP_CAT(addr_, FN), &FN); return std::apply([&](auto... a_) { return invoke(f_, a_...); }, args_); },                   [&](Footprint& fp_) { std::apply([&](auto... a_) { VP_CAT(fpof_, FN)(fp_, a_...); }, args_); }); }()
#define CALL0(FN) run_op(VP_CAT(name_, FN), [&]() { auto f_ = pickf(VP_CAT(addr_, FN), &FN); return invoke(f_); },                          [&](Footprint& fp_) { VP_CAT(fpof_, FN)(fp_); })
// a variadic one, through its variadic type (the rewrite takes the Va window where the original has `...`)
#define CALLV(FN, VT, ...) [&]() { auto args_ = std::make_tuple(__VA_ARGS__);                                             return run_op(VP_CAT(name_, FN),                                                                                                    [&]() { VT f_ = g_mode == 1 ? (VT)(void*)&FN : (VT)(uintptr_t)VP_CAT(addr_, FN);                                              return std::apply([&](auto... a_) { return invoke(f_, a_...); }, args_); },                                    [&](Footprint& fp_) { fp_.replay_only = "variadic"; }); }()
typedef void(__cdecl* VLog_t)(const char*, ...);
typedef void(__cdecl* VAssert_t)(int, const char*, ...);
typedef void(__cdecl* VPrintf_t)(int, char*, const char*, ...);
typedef uint8_t(__cdecl* VSystem_t)(int*, int*, const char*, ...);

// ---- the script -------------------------------------------------------------------------------------------------------------
struct Ctx {
    std::vector<int> files;                        // cells holding open Files
    std::map<int, int> mode;                       // each one's access: 1 read, 2 write, 3 both
    std::vector<HANDLE> finds;
    std::vector<int> maps;                         // amap slots holding a map
    std::vector<uint32_t> shm;                     // live shmem handles
    std::vector<const char*> shm_names;
    int next_cell = 1, next_map = 0;
    int log_file = 0;                              // the log's File: the script never closes it
    HANDLE server = INVALID_HANDLE_VALUE;          // MsgServerBegin's mailslot, while the client's is in the global
    int pending = 0;                               // messages sent to it and not yet received
};
static long g_cov[32];
enum { C_PANIC, C_FAULT, C_FILE_SINK, C_SLOT_SINK, C_FILE_LIMIT, C_LONG_LOG, C_MONO, C_MSGBOX, C_TABLE_FULL, C_FAKE_MAP,
       C_REAL_MAP, C_READLINE, C_MAIL, C_SHM, C_WIN, C_SYSTEM, NCOV };
static const char* const k_cov_names[NCOV] = {"panics", "faults", "file sinks", "mailslot sinks", "log size limits",
    "long log messages", "mono writes", "message boxes", "full file tables", "fake maps", "real maps", "lines read",
    "mailslot messages", "shmem blocks", "window messages", "Win32System runs"};

static const char* rel_names[] = {"a.txt", "lf.txt", "empty.txt", "bin.dat", "ro.txt", "dir1\\b.txt", "dir1\\sub\\c.txt",
                                  "Dir2\\new.txt", "new1.txt", "new2.dat", "missing.txt", "nodir\\x.txt", "options.def",
                                  "tune.def", "readme.txt", "english.lng", "hastings.trm", "drivers.res", "Config\\bemidji.sco",
                                  "My Documents\\notes.txt", "c:\\log.log", "C:\\Program Files\\MGI\\Viper98\\player.cfg",
                                  "d:\\save\\ghost.gst", "\\\\server\\share\\f.txt", "A.TXT", "DIR1\\B.TXT", "", "dir1",
                                  "log.cfg", "new1.txt", "new2.dat"};
static const char* pick_name() {
    const int k = ri(0, 20);
    if (k == 0) return astr(g_long_name.c_str());
    if (k == 1) return astr((std::string(g_sb) + "\\" + pickc(rel_names, 12)).c_str());
    return astr(pickc(rel_names, (int)(sizeof rel_names / sizeof *rel_names)));
}
static const char* pick_dir_path() {
    static const char* const d[] = {"newdir", "newdir\\", "a\\b\\c", "a\\b\\c\\", "dir1\\sub\\deeper\\", "Dir2\\x\\y\\z\\",
                                    "c:\\Program Files\\MGI\\Viper98\\", "C:\\Program Files\\MGI\\Viper98", "d:\\nowhere\\x\\",
                                    "\\\\server\\share\\dir\\", "\\\\server\\", "c:x\\y\\", "\\rooted\\dir\\", "", "\\", "x",
                                    "ro.txt\\sub\\", "a.txt", "My Documents\\Viper\\", "c:\\", "c:"};
    if (chance(10)) return astr((std::string(g_sb) + "\\abs\\one\\").c_str());
    return astr(pickc(d, (int)(sizeof d / sizeof *d)));
}
static std::string rand_text(int n, bool specials) {
    static const char set[] = "abcdefghijklmnopqrstuvwxyz ABCXYZ0123456789.,:-_";
    std::string s;
    for (int i = 0; i < n; i++) {
        if (specials && chance(6)) { const char sp[] = {'\t', '\n', '\r', 7, '%'}; s += sp[rnd() % 5]; if (s.back() == '%') s += '%'; if (s.back() == 7) s += (char)(rnd() & 0x7f ? rnd() & 0x7f : 0x17); }
        else s += set[rnd() % (sizeof set - 1)];
    }
    return s;
}
static int new_cell(Ctx& c) {
    const int k = c.next_cell++;
    if (c.next_cell > 900) c.next_cell = 1;
    return k;
}
static int pick_file(Ctx& c, int want = 3) {       // mostly one it can be used for
    if (c.files.empty()) return 0;
    std::vector<int> ok;
    for (int x : c.files)
        if (c.mode[x] & want) ok.push_back(x);
    if (ok.empty()) return chance(20) ? c.files[rnd() % c.files.size()] : 0;
    return chance(8) ? c.files[rnd() % c.files.size()] : ok[rnd() % ok.size()];
}

// the file table
static void file_op(Ctx& c) {
    const int k = ri(0, 40);
    const int cell = pick_file(c), rcell = pick_file(c, 1), wcell = pick_file(c, 2);
    const int f = cell ? *acell(cell) : 0, fr = rcell ? *acell(rcell) : 0, fw = wcell ? *acell(wcell) : 0;
    switch (k) {
    case 0: case 1: case 2: {                                      // open or create
        const char* n = pick_name();
        const int m = ri(0, 7);
        int r;
        if (m == 0) r = (int)CALL(FileCreate_rw, n);
        else if (m == 1) r = (int)CALL(FileAppend_rw, n);
        else if (m == 2) r = (int)CALL(FileOpenWritable_rw, n);
        else r = (int)CALL(FileOpen_rw, n);
        if (r > 0) { const int nc = new_cell(c); *acell(nc) = r; c.files.push_back(nc); c.mode[nc] = m < 2 ? 2 : m == 2 ? 3 : 1; }
        break;
    }
    case 3: {
        const int r = (int)CALL0(FileCreateTemp_rw);
        if (r > 0) { const int nc = new_cell(c); *acell(nc) = r; c.files.push_back(nc); c.mode[nc] = 3; }
        break;
    }
    case 4: case 5:                                                // close
        if (cell && *acell(cell) != c.log_file) {
            CALL(FileClose_rw, acell(cell));
            c.files.erase(std::find(c.files.begin(), c.files.end(), cell));
        }
        break;
    case 6: if (f) CALL(FileSize_rw, f); break;
    case 7: case 8: if (fr) {                                      // read lines
        const int n = chance(10) ? ri(0, 2) : ri(3, 260);
        char* b = (char*)abuf(0x100);
        memset(b - 0x10, 0x3c, 0x300);
        for (int i = 0; i < ri(1, 6); i++)
            if (CALL(FileReadLine_rw, fr, b, n) == 1) g_cov[C_READLINE]++;
        break;
    }
    case 9: if (fr) {                                              // read
        int* n = acell(0);
        *n = chance(10) ? 0 : ri(1, 0x3000);
        CALL(FileRead_rw, fr, (void*)abuf(0x1000), n);
        break;
    }
    case 10: if (fr) CALL(FileReadExact_rw, fr, (void*)abuf(0x5000), chance(20) ? ri(0, 20) : ri(1, 6000)); break;
    case 11: case 12: if (fw && fw != c.log_file) {                // write
        uint8_t* d = abuf(0x9000);
        const int n = chance(5) ? 0 : ri(1, 3000);
        for (int i = 0; i < n; i++) d[i] = (uint8_t)rnd();
        CALL(FileWrite_rw, fw, (const void*)d, n);
        break;
    }
    case 13: if (fw && fw != c.log_file) {                         // printf
        char* b = (char*)abuf(0xd000);
        const char* s = astr(rand_text(ri(0, 60), false).c_str());
        switch (rnd() % 4) {
        case 0: CALLV(FilePrintf_rw, VPrintf_t, fw, b, astr("%d,%s\r\n"), ri(-100000, 100000), s); break;
        case 1: CALLV(FilePrintf_rw, VPrintf_t, fw, b, astr("%s = %f; %5.2f %e\n"), s, (double)ri(-9999, 9999) / 7.0, 2.5, 1e-7); break;
        case 2: CALLV(FilePrintf_rw, VPrintf_t, fw, b, astr("%x|%c|%08X|%g"), rnd(), 'q', rnd(), (double)rnd()); break;
        default: CALLV(FilePrintf_rw, VPrintf_t, fw, b, astr("")); break;       // nothing: a 0-byte write
        }
        break;
    }
    case 14: if (f) CALL(FileFlush_rw, f); break;
    case 15: CALL(FileSetWritable_rw, pick_name(), (uint8_t)(chance(50) ? 1 : chance(50) ? 0 : ri(2, 255))); break;
    case 16: CALL(FileRemove_rw, pick_name()); break;
    case 17: CALL(FileCopy_rw, pick_name(), pick_name()); break;
    case 18: if (f && chance(30)) { if (chance(50)) CALL(FileSeekAbsolute_rw, f, ri(-5, 5000)); else CALL(FileSeekRelative_rw, f, ri(-5, 5000)); } break;
    case 19: case 20: {                                            // find
        static const char* const pats[] = {"*", "*.txt", "dir1\\*", "nomatch*", "*.def", "Config\\*.sco", "?.txt", "c:\\*",
                                           "\\\\server\\share\\*", "L*", "dir1\\sub\\*.*"};
        const char* p = chance(10) ? astr((std::string(g_sb) + "\\*.*").c_str()) : astr(pickc(pats, 11));
        char* out = aout(0x100);
        memset(out, 0x3c, 0x200);
        const int n = chance(20) ? ri(0, 8) : ri(9, 300);
        HANDLE h = (HANDLE)(uintptr_t)CALL(FileFindFirst_rw, p, out, n);
        if (h != INVALID_HANDLE_VALUE) {
            c.finds.push_back(h);
            for (int i = 0, m = ri(0, 30); i < m; i++)
                if (!CALL(FileFindNext_rw, h, out, chance(10) ? ri(0, 8) : n)) break;
            if (chance(70)) { CALL(FileFindClose_rw, h); c.finds.pop_back(); }
        }
        break;
    }
    case 21: {
        static const char* const d[] = {"dir1", "dir1\\sub", "nodir", "Config", "c:\\", "\\\\server\\share"};
        CALL(FileChangeDir_rw, chance(30) ? astr(g_sb) : astr(pickc(d, 6)));
        break;
    }
    case 22: CALL(FileCreateDirectory_rw, pick_dir_path()); break;
    case 23: case 24: CALL(FileCreateDirectoryRecursively_rw, pick_dir_path()); break;
    case 25: CALL(create_directory_rw, pick_dir_path()); break;
    case 26: case 27: {                                            // get_sub_dir, alone
        char* out = aout(0x400);
        memset(out, 0x3c, 0x200);
        std::string p;
        const int kind = ri(0, 5);
        if (kind == 1) p = "c:";
        else if (kind == 2) p = "\\\\srv";
        else if (kind == 3) p = "c:x";
        for (int i = 0, n = ri(0, 7); i < n; i++) { p += rand_text(ri(0, 9), false); if (chance(80)) p += "\\"; }
        if (chance(3)) p = std::string(ri(1, 250), 'p');
        CALL(get_sub_dir_rw, out, (const char*)astr(p.c_str()), ri(-1, 9));
        break;
    }
    case 28: case 29: if (fr) {                                    // a memory map
        const int m = c.next_map++ & 15;
        FileMemoryMap* mm = amap(m);
        CALL(FileCreateMemoryMap_rw, mm, fr);
        if (mm->mem) { c.maps.push_back(m); g_cov[mm->fake ? C_FAKE_MAP : C_REAL_MAP]++; }
        break;
    }
    case 30: if (!c.maps.empty()) {
        const size_t i = rnd() % c.maps.size();
        FileMemoryMap* mm = amap(c.maps[i]);
        if (mm->fake && chance(30)) CALL(destroy_fake_memory_map_rw, mm);
        else CALL(FileDestroyMemoryMap_rw, mm);
        c.maps.erase(c.maps.begin() + i);
        break;
    }
    case 31: if (f && chance(40)) {                                // the fake map, alone (a slot index, not a File)
        const int m = c.next_map++ & 15;
        CALL(create_fake_memory_map_rw, amap(m), f - 1);
        if (amap(m)->mem) { c.maps.push_back(m); g_cov[C_FAKE_MAP]++; }
        break;
    }
    case 35: {                                                     // an empty file can't be mapped: the fake map
        const int r = (int)CALL(FileOpen_rw, (const char*)astr(chance(80) ? "empty.txt" : "drivers.res"));
        if (r > 0) {
            const int nc = new_cell(c);
            *acell(nc) = r;
            c.files.push_back(nc);
            c.mode[nc] = 1;
            const int m = c.next_map++ & 15;
            CALL(FileCreateMemoryMap_rw, amap(m), r);
            if (amap(m)->mem) { c.maps.push_back(m); g_cov[amap(m)->fake ? C_FAKE_MAP : C_REAL_MAP]++; }
        }
        break;
    }
    case 32: if (chance(15) && (!g_moved_on || *(int32_t*)0x00505ea0 < 90)) {   // the table filled up with fake handles, to its
                                                                   // panic (moved: kept under the original's 127)
        for (int i = 0; i < 34; i++) {
            const int r = (int)CALL(alloc_file_rw, astr(("fake" + std::to_string(i)).c_str()), vnew(0, VK_FAKE));
            if (g_cur->trace.back().result) { g_cov[C_TABLE_FULL]++; break; }
            if (r >= 0 && chance(5)) { const int nc = new_cell(c); *acell(nc) = r + 1; c.files.push_back(nc); c.mode[nc] = 0; }
        }
        break;
    }
    case 33: CALL0(FileVerifyNoOpenFiles_rw); break;
    case 34: if (chance(20) && !c.log_file) { CALL0(FileEnd_rw); CALL0(FileBegin_rw); c.files.clear(); c.maps.clear(); } break;
    default: break;
    }
}

// the log
static void log_setup_panic_path() {                // where a panic goes: the log, the mono monitor or a message box
    const int k = ri(0, 3);
    if (k == 0) { *(int32_t*)0x00505e58 = 0; if (g_have_mono && chance(50)) *(uint8_t*)0x004e5a1c = 0; }
    else if (k == 1) *(int32_t*)0x00505e98 = 0;
    else { *(int32_t*)0x00505e58 = ri(1, 9); *(int32_t*)0x00505e98 = ri(1, 9); }
    if (*(uint8_t*)0x004e5a1c == 0 && *(int32_t*)0x00505e58 == 0) g_cov[C_MONO]++;
    else if (*(int32_t*)0x00505e58 == 0 || *(int32_t*)0x00505e98 == 0) g_cov[C_MSGBOX]++;
}
static std::string len_text(int total) { return rand_text(total < 0 ? 0 : total, false); }
static void log_op(Ctx& c) {
    const int k = ri(0, 30);
    switch (k) {
    case 0: {                                                      // LogBegin, with a log.cfg
        static const char* const cfgs[] = {".", "  .  \r\n", ".\r\n", "machine", "\tfoo bar", "", "logger1", "..........................................",
                                           "\r\n\r\n.", "abcdefghijklmnopqrstuvwxyz0123456789"};
        const int v = ri(-1, 9);
        if (v < 0) DeleteFileA((std::string(g_sb) + "\\log.cfg").c_str());
        else put_file("log.cfg", cfgs[v], strlen(cfgs[v]), 0, false);
        if (g_logger_slot == INVALID_HANDLE_VALUE && chance(70))
            g_logger_slot = CreateMailslotA((std::string(g_slot_prefix) + "mgi\\logger").c_str(), 0, 0, 0);
        g_version = g_have_mono && chance(20) ? 0xc0000a04 : 0x23f00206;
        CALL0(LogBegin_rw);
        const uint32_t sink = *(uint32_t*)0x004e5a20;
        if (sink == 0x00410f10) { c.log_file = *(int32_t*)0x00505e60; g_cov[C_FILE_SINK]++; }
        if (sink == 0x00410ff0) g_cov[C_SLOT_SINK]++;
        break;
    }
    case 1: case 2: case 3: {                                      // LogReport at and around its limit
        const int L = chance(40) ? ri(0xf8, 0x101) : ri(0, 0xf0);
        if (L >= 0xff) g_cov[C_LONG_LOG]++;
        switch (rnd() % 4) {
        case 0: CALLV(LogReport_rw, VLog_t, astr("%s"), astr(len_text(L).c_str())); break;
        case 1: CALLV(LogReport_rw, VLog_t, astr("$%s"), astr(len_text(L > 0 ? L - 1 : 0).c_str())); break;
        case 2: CALLV(LogReport_rw, VLog_t, astr("v %d %x %f %s"), ri(-99999, 99999), rnd(), (double)ri(-9999, 9999) / 3.0, astr(len_text(ri(0, 100)).c_str())); break;
        default: CALLV(LogReport_rw, VLog_t, astr(rand_text(ri(0, 80), true).c_str())); break;
        }
        break;
    }
    case 4: case 5: {
        const int L = chance(40) ? ri(0xf0, 0xf9) : ri(0, 0xe0);
        CALLV(LogError_rw, VLog_t, astr("%s"), astr(len_text(L).c_str()));
        break;
    }
    case 6: CALL(log_rw, (const char*)astr(rand_text(ri(0, 120), true).c_str())); break;
    case 7: if (c.log_file) {                                      // the file sink, at its size limit
        if (chance(40)) { *(int32_t*)0x004e5a28 = ri(999900, 1000100); g_cov[C_FILE_LIMIT]++; }
        if (chance(20)) *(uint8_t*)0x004e5a2c = 0;
        CALL(file_log_rw, (const char*)astr((std::string(chance(20) ? "$" : "") + rand_text(ri(0, 100), false)).c_str()));
        break;
    }
    case 8: CALL(msg_log_rw, (const char*)astr(rand_text(ri(0, 300), false).c_str())); break;
    case 9: case 10: CALL(LogInstallHook_rw, (void*)k_log_hooks[ri(0, 11)]); break;
    case 11: CALL(LogUninstallHook_rw, (void*)k_log_hooks[ri(0, 11)]); break;
    case 12: case 13: {                                            // the mono monitor
        if (g_have_mono) *(uint8_t*)0x004e5a1c = (uint8_t)(chance(60) ? 0 : 1);
        CALL(mputs_rw, ri(-2, 82), ri(-2, 26), (const char*)astr(rand_text(ri(0, 140), true).c_str()));
        break;
    }
    case 14: if (g_have_mono) CALL0(mono_clear_rw); break;
    case 15: if (chance(30)) {                                     // the end: the end sink, whichever it is
        CALL0(LogEnd_rw);
        if (*(uint32_t*)0x004e5a20 != 0x00410f10) c.log_file = 0;
        break;
    }
    case 16: if (c.log_file && chance(30)) { CALL0(file_end_rw); c.log_file = 0; } break;
    case 17: if (chance(20) && *(uint32_t*)0x004e5a20 != 0x00410f10) {   // (never under a live file sink: a failed
        CALL0(log_file_begin_rw);                                  // create would leave it writing to File 0 -- a panic loop)
        if (*(uint32_t*)0x004e5a20 == 0x00410f10) c.log_file = *(int32_t*)0x00505e60;
        break;
    }
    case 18: if (chance(30)) CALL0(msg_end_rw); break;
    case 19: case 20: case 21: {                                   // the panics
        log_setup_panic_path();
        const int L = chance(30) ? ri(0xf0, 0xf8) : ri(0, 0x60);
        const char* s = astr(len_text(L).c_str());
        switch (rnd() % 6) {
        case 0: CALLV(LogPanic_rw, VLog_t, astr("%s"), s); break;
        case 1: CALLV(LogHang_rw, VLog_t, astr("%d %s"), ri(0, 99), s); break;
        case 2: CALLV(LogAssert_rw, VLog_t, astr("%s:%d"), s, ri(0, 999)); break;
        case 3: {
            const int cond = chance(50) ? 0 : ri(1, 5);
            const char* fmt = astr("assert %s");
            run_op(name_LogAssertMsg_rw, [&]() { VAssert_t f_ = g_mode == 1 ? (VAssert_t)(void*)&LogAssertMsg_rw : (VAssert_t)(uintptr_t)addr_LogAssertMsg_rw;
                                                 return invoke(f_, cond, fmt, s); },
                   [&](Footprint& fp_) { if (!cond) fp_.replay_only = "panics"; });
            break;
        }
        case 4: {                                                  // _LogPanic, alone
            const char** pf = (const char**)acell(950);
            *pf = astr("%s|%d|%s");
            int* args = acell(952);
            args[0] = (int)(uintptr_t)s;
            args[1] = ri(-5, 5);
            args[2] = (int)(uintptr_t)astr("tail");
            CALL(LogPanicV_rw, (const char*)astr(chance(50) ? "Custom: " : ""), pf, (char*)args);
            break;
        }
        default: CALLV(LogPanic_rw, VLog_t, astr("%s %f"), s, 1.25); break;
        }
        break;
    }
    default: break;
    }
}

// the window shell
static void win_op(Ctx&) {
    const int k = ri(0, 30);
    switch (k) {
    case 0: CALL0(check_for_canary_launch_rw); break;
    case 1: CALL0(start_unique_instance_rw); break;
    case 2: CALL0(end_unique_instance_rw); break;
    case 3: CALL0(get_user_directory_rw); break;
    case 4: CALL0(make_loading_window_rw); break;
    case 5: case 6: {
        static const UINT msgs[] = {WM_CREATE, WM_DESTROY, WM_PAINT, WM_PAINT, WM_CREATE, WM_MOVE, WM_TIMER, 0};
        CALL(splash_proc_rw, (HWND)(uintptr_t)0x00cd0120, msgs[rnd() % 8], (WPARAM)rnd(), (LPARAM)rnd());
        break;
    }
    case 7: *(uint8_t*)0x004e5ec8 = (uint8_t)(chance(80) ? 0 : 1); CALL0(Win32IsConsoleMode_rw); break;
    case 8: CALL0(Win32Begin_rw); break;
    case 9: case 10: case 11: case 12: {                           // the window procedure
        static const UINT msgs[] = {WM_DESTROY, WM_ACTIVATEAPP, WM_ACTIVATEAPP, WM_SETCURSOR, WM_KEYDOWN, WM_KEYUP, WM_CHAR,
                                    WM_DEADCHAR, WM_SYSKEYDOWN, WM_SYSKEYUP, WM_SYSCOMMAND, WM_SYSCOMMAND, 0x200, 0x201, 0x202,
                                    0x203, 0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, WM_PAINT, WM_SIZE, 0x1b, 0x1d,
                                    0x113, 0x111, 0xff, 0x106};
        const UINT m = msgs[rnd() % (sizeof msgs / sizeof *msgs)];
        WPARAM wp = (WPARAM)rnd();
        if (m == WM_ACTIVATEAPP) { const uint32_t a[] = {0, 1, 2, 3, 0x10000, 0x10001}; wp = a[rnd() % 6]; }
        if (m == WM_SYSCOMMAND && chance(80)) { const uint32_t sc[] = {SC_KEYMENU, SC_TASKLIST, SC_SCREENSAVE, SC_MONITORPOWER, SC_CLOSE, SC_KEYMENU | 3, SC_MINIMIZE, 0xf170 + 1}; wp = sc[rnd() % 8]; }
        if (m >= 0x200 && m <= 0x20a && chance(70)) wp = rnd() & 0x33;
        const LPARAM lp = (LPARAM)(chance(50) ? rnd() : (rnd() & 0x7fff7fff));
        CALL(win32_event_rw, (HWND)(uintptr_t)0x00abc000, m, wp, lp);
        g_cov[C_WIN]++;
        break;
    }
    case 13: CALL(restrict_cursor_rw, (uint8_t)(chance(40) ? 0 : chance(70) ? 1 : ri(2, 255))); break;
    case 14: CALL(key_hook_proc_rw, ri(-1, 3), (WPARAM)rnd(), (LPARAM)rnd()); break;
    case 15: *(uint8_t*)0x004e5ec8 = (uint8_t)(chance(80) ? 0 : 1); CALL0(Win32End_rw); break;
    case 16: CALL0(destroy_window_rw); break;
    case 17: CALL0(unhook_keys_rw); break;
    case 18: CALL0(Win32GetAppInstance_rw); CALL0(Win32GetArgs_rw); CALL0(Win32GetWindow_rw); CALL0(Win32AppName_rw); CALL0(Win32GetUserDirectory_rw); break;
    case 19: CALL0(Win32FlushRegistry_rw); break;
    case 20: case 21: CALL(Win32RegisterMessageHook_rw, (void*)k_msg_hooks[ri(0, 2)]); break;
    case 22: CALL(Win32UnRegisterMessageHook_rw, (void*)k_msg_hooks[ri(0, 2)]); break;
    case 23: CALL0(Win32GetTime_rw); CALL0(Win32RestrictCursor_rw); break;
    case 24: case 25: {
        int* err = chance(70) ? acell(960) : 0;
        int* code = chance(70) ? acell(961) : 0;
        if (err) *err = -7;
        if (code) *code = -8;
        CALLV(Win32System_rw, VSystem_t, err, code, astr("%s %d \"%s\""), astr("tool.exe"), ri(0, 99), astr(rand_text(ri(0, 40), false).c_str()));
        g_cov[C_SYSTEM]++;
        break;
    }
    case 26: g_err = chance(70) ? (uint32_t)ri(0, 1500) : rnd(); CALL0(Win32GetErrorString_rw); break;
    case 27: CALL(Win32GetErrorString_int_rw, chance(50) ? ri(-3, 0x1800) : (int)rnd()); break;
    default: break;
    }
}

// messages over a mailslot
static void msg_op(Ctx& c) {
    HANDLE volatile* slot = (HANDLE volatile*)0x00508e3c;
    const int k = ri(0, 12);
    switch (k) {
    case 0: {
        static const char* const names[] = {"k2box", "logger", "Box2"};
        if (c.server != INVALID_HANDLE_VALUE) break;
        if (CALL(MsgServerBegin_rw, (const char*)astr(pickc(names, 3)))) { c.server = *slot; c.pending = 0; }
        break;
    }
    case 1: {
        static const char* const hosts[] = {".", ".", "nobody", ""};
        static const char* const names[] = {"k2box", "k2box", "nobox"};
        CALL(MsgClientBegin_rw, (const char*)astr(pickc(names, 3)), (const char*)astr(pickc(hosts, 4)));
        break;
    }
    case 2: case 3: case 4:
        if (CALL(MsgClientSend_rw, (const char*)astr(rand_text(chance(20) ? ri(0x100, 0x130) : ri(0, 80), false).c_str()))) {
            c.pending++;
            g_cov[C_MAIL]++;
        }
        break;
    case 5: if (chance(30)) CALL0(MsgClientEnd_rw); break;
    case 6: case 7: if (c.server != INVALID_HANDLE_VALUE && (c.pending > 0 || chance(5))) {
        HANDLE saved = *slot;
        *slot = c.server;
        char* out = aout(0x800);
        memset(out, 0x3c, 0x140);
        if (CALL(MsgServerRecv_rw, acell(970), out, chance(20) ? ri(0, 8) : ri(0x100, 0x120)) && c.pending > 0) c.pending--;
        *slot = saved;
        break;
    }
    case 9: case 10: {                                             // a whole conversation: server, client, messages
        if (c.server == INVALID_HANDLE_VALUE && CALL(MsgServerBegin_rw, (const char*)astr("talk"))) { c.server = *slot; c.pending = 0; }
        if (c.server == INVALID_HANDLE_VALUE) break;
        if (!CALL(MsgClientBegin_rw, (const char*)astr("talk"), (const char*)astr("."))) break;
        for (int i = 0, n = ri(1, 5); i < n; i++)
            if (CALL(MsgClientSend_rw, (const char*)astr(rand_text(chance(20) ? ri(0x100, 0x130) : ri(0, 80), false).c_str()))) { c.pending++; g_cov[C_MAIL]++; }
        if (chance(50)) CALL0(MsgClientEnd_rw);
        HANDLE saved = *slot;
        *slot = c.server;
        for (int i = 0, n = ri(0, 6); i < n && c.pending > 0; i++) {
            char* out = aout(0x800);
            memset(out, 0x3c, 0x140);
            if (CALL(MsgServerRecv_rw, acell(970), out, chance(20) ? ri(0, 8) : ri(0x100, 0x120))) c.pending--;
            else break;
        }
        *slot = saved;
        break;
    }
    case 8: if (c.server != INVALID_HANDLE_VALUE && chance(30)) {
        *slot = c.server;
        CALL0(MsgServerEnd_rw);
        c.server = INVALID_HANDLE_VALUE;
        break;
    }
    default: break;
    }
}

// shared memory
static ShmemBlock* shm_block(uint32_t h) { return ((ShmemBlock**)0x005d5764)[h]; }
static void shm_op(Ctx& c) {
    const int k = ri(0, 16);
    const uint32_t h = c.shm.empty() ? 0 : c.shm[rnd() % c.shm.size()];
    if (chance(20)) g_task = ri(1, 3);
    switch (k) {
    case 0: case 1: {
        static const char* const names[] = {"PhysicsState", "packet", "Ghost", "x"};
        const char* n = astr(pickc(names, 4));
        if (chance(8)) g_alloc_fail = true;
        const uint32_t r = (uint32_t)CALL(ShmemAlloc_rw, n, chance(10) ? 0 : ri(1, 300));
        if (r && g_cur->trace.back().result == 0 && c.shm.size() < 8) { c.shm.push_back(r); c.shm_names.push_back(n); g_cov[C_SHM]++; }
        break;
    }
    case 2: {
        std::string n = c.shm_names.empty() || chance(15) ? "missing" : c.shm_names[rnd() % c.shm_names.size()];
        if (chance(30)) for (char& ch : n) ch = (char)toupper((uint8_t)ch);
        CALL(ShmemGet_rw, (const char*)astr(n.c_str()));
        break;
    }
    case 3: case 4: if (h) {
        void* m = (void*)(uintptr_t)CALL(ShmemGetWritable_rw, h);
        if (m && g_cur->trace.back().result == 0) memset(m, (int)(rnd() & 0xff), 4);
        CALL(ShmemReleaseWritable_rw, h);
        break;
    }
    case 5: if (h) CALL(ShmemPeekReadable_rw, h); break;
    case 6: case 7: if (h) {
        ShmemBlock* b = shm_block(h);
        if (b->reading == 0 || chance(10)) CALL(ShmemGetReadable_rw, h);
        if ((b->reading && chance(50)) || chance(3)) CALL(ShmemReleaseReadable_rw, h);   // (often held on: the writer skips it)
        break;
    }
    case 8: if (h) {                                               // the methods, alone
        ShmemBlock* b = shm_block(h);
        switch (rnd() % 6) {
        case 0: CALL(grab_wr_mem_rw, b, 0); break;
        case 1: CALL(release_wr_mem_rw, b, 0); break;
        case 2: CALL(peek_ro_mem_rw, b, 0); break;
        case 3: if (b->reading == 0) { CALL(grab_ro_mem_rw, b, 0); if (chance(60)) CALL(release_ro_mem_rw, b, 0); } break;
        case 4: if (b->reading) CALL(release_ro_mem_rw, b, 0); break;
        default: break;
        }
        break;
    }
    case 9: if (h && chance(25)) {
        CALL(ShmemFree_rw, h);
        const size_t i = std::find(c.shm.begin(), c.shm.end(), h) - c.shm.begin();
        c.shm.erase(c.shm.begin() + i);
        c.shm_names.erase(c.shm_names.begin() + i);
        break;
    }
    case 10: if (chance(20)) {                                     // a block in the arena, made and destroyed alone
        ShmemBlock* b = (ShmemBlock*)(g_arena + OFF_OUT + 0x2000);
        CALL(shmem_block_ctor_rw, b, 0, (const char*)astr("arena"), ri(0, 64));
        if (g_cur->trace.back().result == 0 && b->mutex && b->strobe) CALL(shmem_block_dtor_rw, b, 0);
        break;
    }
    default: break;
    }
}

static void scenario() {
    Ctx c;
    // the statics as the game leaves them before these run
    *(void**)0x004e5a20 = (void*)&st_sink;                          // the log: a sink that records
    *(void**)0x004e5a24 = (void*)&st_sink_end;
    *(int32_t*)0x00505e58 = ri(1, 9);
    *(int32_t*)0x00505e98 = ri(1, 9);
    *(uint8_t*)0x004e5a1c = 1;
    *(int32_t*)0x00505e5c = ri(0, 24);
    *(void**)0x004e5eb4 = (void*)0x00400000;                        // the instance
    *(char**)0x004e5eb8 = astr("race.exe -nosplash");
    *(uint32_t*)0x004e5eac = 0x00abc000;
    *(uint32_t*)0x004e5eb0 = 0x00abd000;
    *(uint32_t*)0x004e5ebc = chance(50) ? 0x0000beef : 0;
    if (chance(90)) CALL0(FileBegin_rw);
    const int nops = ri(20, 160);
    const int w_file = ri(1, 10), w_log = ri(0, 6), w_win = ri(0, 5), w_msg = ri(0, 4), w_shm = ri(0, 4);
    const int total = w_file + w_log + w_win + w_msg + w_shm;
    for (int i = 0; i < nops; i++) {
        int k = ri(0, total - 1);
        if ((k -= w_file) < 0) file_op(c);
        else if ((k -= w_log) < 0) log_op(c);
        else if ((k -= w_win) < 0) win_op(c);
        else if ((k -= w_msg) < 0) msg_op(c);
        else shm_op(c);
    }
    for (HANDLE h : c.finds) CALL(FileFindClose_rw, h);
    if (chance(50)) CALL0(FileVerifyNoOpenFiles_rw);
    if (chance(30)) CALL0(LogEnd_rw);
    if (chance(50)) CALL0(FileEnd_rw);
}

// ---- comparing the passes ---------------------------------------------------------------------------------------------------
static void print_entry(const char* who, const LogEntry& e) {
    printf("    %s %-20s %08x %08x %08x %08x %08x %08x %08x %08x\n", who, e.kind < sizeof g_log_names / sizeof *g_log_names ? g_log_names[e.kind] : "?",
           e.a[0], e.a[1], e.a[2], e.a[3], e.a[4], e.a[5], e.a[6], e.a[7]);
}
static const char* grange_of(uint32_t a) {
    for (const GRange& g : g_granges)
        if (a >= g.at && a < g.at + g.n) return g.what;
    return "elsewhere in .data";
}
static bool compare(int m, int scen) {
    const PassResult& a = g_res[0];
    const PassResult& b = g_res[m];
    const char* mode = m == 1 ? "isolated" : "chain";
    size_t n = std::min(a.trace.size(), b.trace.size());
    for (size_t i = 0; i < n; i++) {
        const Trace& x = a.trace[i];
        const Trace& y = b.trace[i];
        if (!memcmp(&x, &y, sizeof x)) continue;
        Stat& st = g_stats[a.names[i]];
        if (st.fails[m]++ < 3) {
            printf("MISMATCH %s [%s] (scenario %d, call %d)\n", a.names[i], mode, scen, (int)i);
            printf("    original: result %d code %08x data %08x ret %016llx, %u API calls, globals %08x, arena %08x\n", x.result, x.code, x.data, x.ret, x.nlog, x.gh, x.ah);
            printf("    rewrite:  result %d code %08x data %08x ret %016llx, %u API calls, globals %08x, arena %08x\n", y.result, y.code, y.data, y.ret, y.nlog, y.gh, y.ah);
            const uint32_t from = i ? a.trace[i - 1].nlog : 0;
            for (uint32_t j = from, k = 0; j < std::max(x.nlog, y.nlog) && k < 10; j++) {
                const LogEntry* ea = j < a.api.size() ? &a.api[j] : 0;
                const LogEntry* eb = j < b.api.size() ? &b.api[j] : 0;
                if (ea && eb && !memcmp(ea, eb, sizeof *ea)) continue;
                k++;
                if (ea) print_entry("original", *ea);
                if (eb) print_entry("rewrite ", *eb);
            }
        }
        return false;
    }
    const char* what = 0;
    if (a.trace.size() != b.trace.size()) what = "the number of calls";
    else if (a.api.size() != b.api.size() || memcmp(a.api.data(), b.api.data(), a.api.size() * sizeof(LogEntry))) what = "the API log";
    else if (a.data != b.data) {
        what = ".data";
        for (size_t i = 0; i < a.data.size(); i++)
            if (a.data[i] != b.data[i]) {
                if (i >= DATA_SIZE) printf("    the moved open-file table differs at +0x%x: %02x vs %02x\n", (unsigned)(i - DATA_SIZE), a.data[i], b.data[i]);
                else printf("    .data differs at %08x (%s): %02x vs %02x\n", (unsigned)(DATA_AT + i), grange_of((uint32_t)(DATA_AT + i)), a.data[i], b.data[i]);
                break;
            }
    } else if (a.mono != b.mono) what = "the mono memory";
    else if (a.arena != b.arena) {
        what = "the arena";
        for (size_t i = 0; i < a.arena.size(); i++)
            if (a.arena[i] != b.arena[i]) { printf("    arena differs at +0x%x: %02x vs %02x\n", (unsigned)i, a.arena[i], b.arena[i]); break; }
    } else if (a.texts != b.texts) what = "the log's texts";
    else if (a.files != b.files) {
        what = "the files";
        for (size_t i = 0; i < std::max(a.files.size(), b.files.size()); i++) {
            const std::string x = i < a.files.size() ? a.files[i] : "-", y = i < b.files.size() ? b.files[i] : "-";
            if (x != y) { printf("    original: %s\n    rewrite:  %s\n", x.c_str(), y.c_str()); break; }
        }
    }
    if (!what) return true;
    Stat& st = g_stats["(the whole scenario)"];
    if (st.fails[m]++ < 3) printf("MISMATCH scenario %d [%s]: %s differ\n", scen, mode, what);
    return false;
}

// Win32GetErrorString(int): every code in a range, and random ones
static long check_error_strings() {
    typedef const char*(__cdecl* F)(int);
    const F orig = (F)0x00416080;
    long bad = 0, n = 0;
    auto one = [&](int e) {
        n++;
        if (orig(e) != Win32GetErrorString_int_rw(e) && bad++ < 5)
            printf("MISMATCH Win32GetErrorString(%d): %08x vs %08x\n", e, (unsigned)(uintptr_t)orig(e), (unsigned)(uintptr_t)Win32GetErrorString_int_rw(e));
    };
    for (int e = -0x10000; e < 0x30000; e++) one(e);
    one(INT32_MIN);
    one(INT32_MAX);
    for (int i = 0; i < 2000000; i++) one((int)rnd());
    printf("Win32GetErrorString(int): %ld codes, %ld differ\n", n, bad);
    return bad;
}

#ifdef FIX_TESTS
// ---- the fixes (built with /DFIX_TESTS: the rewrites as the game has them) ---------------------------------------------------
// The user directory (get_user_directory: <race.exe's folder>\Config\, the old one copied in once), the log
// (log_file_begin: <race.exe's folder>\log\log.log; FileVerifyNoOpenFiles skips it) and FileReadLine's bare-LF lines.
// race.exe's folder is a fake one in the sandbox (GetModuleFileNameA's slot), %LOCALAPPDATA% (the VirtualStore) is
// the sandbox's too, and the stubs' own confinement puts "C:\Program Files\..." in the sandbox's _drv_c.
static int g_fix_bad, g_fix_checks;
#define FIX_CHECK(cond, ...)                                                                                     \
    do {                                                                                                         \
        g_fix_checks++;                                                                                          \
        if (!(cond)) {                                                                                           \
            g_fix_bad++;                                                                                         \
            printf("  FAIL (line %d): ", __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                 \
            printf("\n");                                                                                        \
        }                                                                                                        \
    } while (0)

static void mk_dirs(const std::string& path) {                         // every folder of an absolute path
    for (size_t i = 3; i <= path.size(); i++)
        if (i == path.size() || path[i] == '\\') CreateDirectoryA(path.substr(0, i).c_str(), 0);
}
static void put_abs(const std::string& path, const std::string& data, DWORD attrs = 0) {
    mk_dirs(path.substr(0, path.rfind('\\')));
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { printf("can't make %s (%lu)\n", path.c_str(), GetLastError()); ExitProcess(3); }
    DWORD put;
    if (!data.empty()) WriteFile(h, data.data(), (DWORD)data.size(), &put, 0);
    CloseHandle(h);
    if (attrs) SetFileAttributesA(path.c_str(), attrs);
}
static bool is_dir(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool is_file(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
// a tree's folders and files: names, sizes, bytes, the read-only flag; `all`: every attribute and the creation and
// last-write times too (for "untouched")
static void tree_rec(const std::string& dir, const std::string& rel, bool all, std::vector<std::string>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string p = dir + "\\" + fd.cFileName, r = rel + fd.cFileName;
        const bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        char line[600];
        uint32_t hh = 0;
        if (!d) {
            HANDLE f = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            std::vector<uint8_t> b(fd.nFileSizeLow);
            DWORD got = 0;
            if (f != INVALID_HANDLE_VALUE) {
                if (!b.empty()) ReadFile(f, b.data(), (DWORD)b.size(), &got, 0);
                CloseHandle(f);
                hh = hash_bytes(b.data(), got);
            } else hh = 0xbadf11e;
        }
        const DWORD a = all ? fd.dwFileAttributes : fd.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY);
        WIN32_FILE_ATTRIBUTE_DATA fa = {};             // (the entry's own times: a search's copy of a folder's is lazy)
        if (all) GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fa);
        if (all)
            snprintf(line, sizeof line, "%s%s [%lx] %lu %08x %08lx%08lx %08lx%08lx", r.c_str(), d ? "\\" : "", a, fd.nFileSizeLow,
                     hh, fa.ftCreationTime.dwHighDateTime, fa.ftCreationTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime,
                     fa.ftLastWriteTime.dwLowDateTime);
        else
            snprintf(line, sizeof line, "%s%s [%lx] %lu %08x", r.c_str(), d ? "\\" : "", a, fd.nFileSizeLow, hh);
        out.push_back(line);
        if (d) tree_rec(p, r + "\\", all, out);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static std::vector<std::string> tree(const std::string& dir, bool all = false) {
    std::vector<std::string> v;
    tree_rec(dir, "", all, v);
    std::sort(v.begin(), v.end());
    return v;
}
static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += "      " + x + "\n";
    return s;
}

static const char k_stock_dir[] = "C:\\Program Files\\MGI\\Viper98\\";   // the literal at 0x4e5fcc, stock
static std::string g_fx;                               // the fix tests' folder in the sandbox (with a backslash)
static char g_tmp[MAX_PATH];

// a clean start: the image's .data, the sandbox, the file table, the log's statics (a recording sink), the cwd
static void fix_begin() {
    pass_begin(1, 1);
    g_logf_text.clear();
    *(void**)0x004e5a20 = (void*)&st_sink;
    *(void**)0x004e5a24 = (void*)&st_sink_end;
    *(int32_t*)0x00505e58 = 1;
    *(int32_t*)0x00505e98 = 1;
    *(uint8_t*)0x004e5a1c = 1;
    ((void(__cdecl*)())0x00411540)();                  // FileBegin (the original)
    g_fx = std::string(g_sb) + "\\fx\\";
    mk_dirs(g_fx + "game");
    g_fake_exe = g_fx + "game\\race.exe";
    SetEnvironmentVariableA("LOCALAPPDATA", (g_fx + "appdata").c_str());
    SetCurrentDirectoryA(g_sb);
}
static void fix_end() {
    SetCurrentDirectoryA(g_sb);
    pass_end();
}
static std::string ud() { return std::string(g_user_dir); }
// get_user_directory with the image's literal `lit` (a string no longer than the stock one: it is written in place)
static uint8_t user_dir_with(const char* lit) {
    strcpy((char*)0x004e5fcc, lit);
    memset(g_user_dir, 0xcc, 0x104);
    g_logf_text.clear();
    return get_user_directory_rw();
}
static bool logged(const std::string& s) { return g_logf_text.find(s) != std::string::npos; }

// the files an old user directory might hold (a read-only one, a deep tree, a binary)
static void make_old(const std::string& dir, const char* tag) {
    put_abs(dir + "options.cfg", std::string("[video]\r\nresolution 640x480\r\n; ") + tag + "\r\n");
    std::string sco(0x114c8, 0);
    for (size_t i = 0; i < sco.size(); i++) sco[i] = (char)(i * 7 + tag[0]);
    put_abs(dir + "bemidji.sco", sco);
    put_abs(dir + "ghostcar\\bemidjiviper.gst", std::string("ghost ") + tag);
    put_abs(dir + "setups\\bemidji.csu", std::string(0xd4, 'S'));
    put_abs(dir + "paint1.tex", std::string("paint ") + tag, FILE_ATTRIBUTE_READONLY);
    put_abs(dir + "deep\\a\\b\\c.txt", tag);
    mk_dirs(dir + "empty");
}

static void fix_user_directory_tests() {
    // 1. stock literal; the VirtualStore's Program Files copy, its Program Files (x86) copy and the real folder all
    //    there: the first is copied, whole, once; none of them is touched
    {
        fix_begin();
        const std::string game = g_fx + "game\\", cfg = game + "Config\\";
        const std::string vs1 = g_fx + "appdata\\VirtualStore\\Program Files\\MGI\\Viper98\\";
        const std::string vs2 = g_fx + "appdata\\VirtualStore\\Program Files (x86)\\MGI\\Viper98\\";
        const std::string real = std::string(g_sb) + "\\_drv_c\\Program Files\\MGI\\Viper98\\";
        make_old(vs1, "vs1");
        make_old(vs2, "vs2");
        make_old(real, "real");
        const auto before1 = tree(vs1, true), before2 = tree(vs2, true), before3 = tree(real, true);
        FIX_CHECK(user_dir_with(k_stock_dir) == 1, "get_user_directory didn't return 1");
        FIX_CHECK(ud() == cfg, "the user directory is %s, not %s", ud().c_str(), cfg.c_str());
        FIX_CHECK(tree(cfg) == tree(vs1), "the new user directory isn't the VirtualStore's copy:\n    new:\n%s    old:\n%s",
                  join(tree(cfg)).c_str(), join(tree(vs1)).c_str());
        FIX_CHECK(tree(vs1, true) == before1 && tree(vs2, true) == before2 && tree(real, true) == before3,
                  "an old user directory changed");
        FIX_CHECK(logged("fix: user directory " + cfg + " (copied 6 files from " + vs1 + ")"), "logged: %s", g_logf_text.c_str());
        FIX_CHECK(g_texts.find("log: Config Dir: " + cfg) != std::string::npos, "the log didn't get \"Config Dir: %s\": %s",
                  cfg.c_str(), g_texts.c_str());
        // the game writes to its new user directory, and something new appears in the old one: never copied again
        put_abs(cfg + "options.cfg", "[video]\r\nresolution 1920x1080\r\n");
        put_abs(vs1 + "career1.dat", "new in the old folder");
        const auto now = tree(cfg), old_now = tree(vs1, true);
        FIX_CHECK(user_dir_with(k_stock_dir) == 1 && ud() == cfg, "second run: the user directory is %s", ud().c_str());
        FIX_CHECK(tree(cfg) == now, "second run: the new user directory changed");
        FIX_CHECK(tree(vs1, true) == old_now, "second run: the old user directory changed");
        FIX_CHECK(logged("fix: user directory " + cfg + " (already there)"), "second run logged: %s", g_logf_text.c_str());
        fix_end();
    }
    // 2. only the Program Files (x86) VirtualStore copy
    {
        fix_begin();
        const std::string cfg = g_fx + "game\\Config\\";
        const std::string vs2 = g_fx + "appdata\\VirtualStore\\Program Files (x86)\\MGI\\Viper98\\";
        make_old(vs2, "x86");
        const auto before = tree(vs2, true);
        FIX_CHECK(user_dir_with(k_stock_dir) == 1 && ud() == cfg, "(x86): the user directory is %s", ud().c_str());
        FIX_CHECK(tree(cfg) == tree(vs2), "(x86): the new user directory isn't the old one");
        FIX_CHECK(tree(vs2, true) == before, "(x86): the old user directory changed:\n    before:\n%s    after:\n%s",
                  join(before).c_str(), join(tree(vs2, true)).c_str());
        FIX_CHECK(logged("(copied 6 files from " + vs2 + ")"), "(x86) logged: %s", g_logf_text.c_str());
        fix_end();
    }
    // 3. only the real C:\Program Files\MGI\Viper98\ (a game once run elevated, or XP)
    {
        fix_begin();
        const std::string cfg = g_fx + "game\\Config\\";
        const std::string real = std::string(g_sb) + "\\_drv_c\\Program Files\\MGI\\Viper98\\";
        make_old(real, "real");
        const auto before = tree(real, true);
        FIX_CHECK(user_dir_with(k_stock_dir) == 1 && ud() == cfg, "(real): the user directory is %s", ud().c_str());
        FIX_CHECK(tree(cfg) == tree(real), "(real): the new user directory isn't the old one");
        FIX_CHECK(tree(real, true) == before, "(real): the old user directory changed");
        FIX_CHECK(logged(std::string("(copied 6 files from ") + k_stock_dir + ")"), "(real) logged: %s", g_logf_text.c_str());
        fix_end();
    }
    // 4. no old user directory anywhere: a new, empty one
    {
        fix_begin();
        const std::string cfg = g_fx + "game\\Config\\";
        FIX_CHECK(user_dir_with(k_stock_dir) == 1 && ud() == cfg, "(none): the user directory is %s", ud().c_str());
        FIX_CHECK(is_dir(cfg) && tree(cfg).empty(), "(none): the new user directory isn't there and empty");
        FIX_CHECK(logged(std::string("(new; no old user directory at ") + k_stock_dir + ")"), "(none) logged: %s", g_logf_text.c_str());
        fix_end();
    }
    // 5. vrmod's relative "Config\": started from the game's folder it is the new one (nothing to copy; and with its
    //    files already there, nothing happens); started from elsewhere, that folder's Config\ is copied
    {
        fix_begin();
        const std::string game = g_fx + "game\\", cfg = game + "Config\\";
        SetCurrentDirectoryA(game.c_str());
        FIX_CHECK(user_dir_with("Config\\") == 1 && ud() == cfg, "(relative, same): the user directory is %s", ud().c_str());
        FIX_CHECK(is_dir(cfg) && tree(cfg).empty(), "(relative, same): the new user directory isn't there and empty");
        FIX_CHECK(logged("(new; the old user directory " + cfg + " is this one)"), "(relative, same) logged: %s", g_logf_text.c_str());
        fix_end();
        fix_begin();
        make_old(cfg, "vrmod");
        const auto before = tree(cfg, true);
        SetCurrentDirectoryA(game.c_str());
        FIX_CHECK(user_dir_with("Config\\") == 1 && ud() == cfg, "(relative, there): the user directory is %s", ud().c_str());
        FIX_CHECK(tree(cfg, true) == before, "(relative, there): the user directory changed");
        FIX_CHECK(logged("(already there)"), "(relative, there) logged: %s", g_logf_text.c_str());
        fix_end();
        fix_begin();
        const std::string other = g_fx + "elsewhere\\";
        make_old(other + "Config\\", "cwd");
        const auto before2 = tree(other, true);
        SetCurrentDirectoryA(other.c_str());
        FIX_CHECK(user_dir_with("Config\\") == 1 && ud() == cfg, "(relative, elsewhere): the user directory is %s", ud().c_str());
        FIX_CHECK(tree(cfg) == tree(other + "Config\\"), "(relative, elsewhere): the new user directory isn't the old one");
        FIX_CHECK(tree(other, true) == before2, "(relative, elsewhere): the old user directory changed");
        FIX_CHECK(logged("(copied 6 files from " + other + "Config\\)"), "(relative, elsewhere) logged: %s", g_logf_text.c_str());
        fix_end();
    }
    // 6. the guard: <folder>Config\ of exactly 200 characters is taken, 201 keeps the literal (nothing made or
    //    copied); so do a failed or cut-short GetModuleFileNameA
    for (int extra = 0; extra < 2; extra++) {
        fix_begin();
        make_old(std::string(g_sb) + "\\_drv_c\\Program Files\\MGI\\Viper98\\", "real");
        std::string game = g_fx;                       // pad to <game>\ + "Config\" = 200 + extra
        const size_t want = 200 + extra - 7;           // the folder's length with its backslash
        while (game.size() < want) {
            const size_t room = want - game.size() - 1;   // (each component ends with a backslash; none is empty)
            game += std::string(room > 100 ? (room - 100 >= 2 ? 100 : room - 2) : room, 'p') + "\\";
        }
        mk_dirs(game.substr(0, game.size() - 1));
        g_fake_exe = game + "race.exe";
        const std::string cfg = game + "Config\\";
        const uint8_t r = user_dir_with(k_stock_dir);
        if (extra == 0) {
            FIX_CHECK(r == 1 && ud() == cfg && cfg.size() == 200, "(200 characters): the user directory is %s (%u)", ud().c_str(),
                      (unsigned)ud().size());
            FIX_CHECK(tree(cfg) == tree(std::string(g_sb) + "\\_drv_c\\Program Files\\MGI\\Viper98\\"), "(200 characters): not copied");
        } else {
            FIX_CHECK(r == 1 && ud() == k_stock_dir, "(201 characters): the user directory is %s, not the literal", ud().c_str());
            FIX_CHECK(!is_dir(cfg), "(201 characters): %s was made", cfg.c_str());
            FIX_CHECK(logged("would be 201 characters (the game takes 200); keeping " + std::string(k_stock_dir)),
                      "(201 characters) logged: %s", g_logf_text.c_str());
        }
        fix_end();
    }
    {
        fix_begin();
        g_fake_exe = "";
        FIX_CHECK(user_dir_with(k_stock_dir) == 1 && ud() == k_stock_dir, "(no exe path): the user directory is %s", ud().c_str());
        FIX_CHECK(logged("race.exe's folder not found; keeping"), "(no exe path) logged: %s", g_logf_text.c_str());
        g_fake_exe = g_fx + std::string(300, 'q') + "\\race.exe";   // GetModuleFileNameA cuts it short
        FIX_CHECK(user_dir_with(k_stock_dir) == 1 && ud() == k_stock_dir, "(cut short): the user directory is %s", ud().c_str());
        fix_end();
    }
    strcpy((char*)0x004e5fcc, k_stock_dir);
}

static void fix_log_tests() {
    typedef int(__cdecl* Open_t)(const char*);
    const Open_t file_open = (Open_t)0x00411780;
    // the log in <race.exe's folder>\log\, the folder made; FileVerifyNoOpenFiles skips it (and only it)
    for (int again = 0; again < 2; again++) {
        fix_begin();
        const std::string logdir = g_fx + "game\\log", path = logdir + "\\log.log";
        if (again) mk_dirs(logdir);                    // (the folder already there)
        FIX_CHECK(log_file_begin_rw() == 1, "log_file_begin failed");
        const int f = g_log_file;
        FIX_CHECK(f > 0 && is_file(path) && path == file_at(m1_operand(0x00411583), f - 1)->name, "the log isn't %s (file %d: %s)", path.c_str(), f,
                  f > 0 ? file_at(m1_operand(0x00411583), f - 1)->name : "-");
        FIX_CHECK(logged("fix: the game's log is " + path), "logged: %s", g_logf_text.c_str());
        const int other = file_open("a.txt");
        g_texts.clear();
        FileVerifyNoOpenFiles_rw();
        FIX_CHECK(other > 0 && g_texts == "log: Handle to a.txt was never freed.\n", "FileVerifyNoOpenFiles reported: %s", g_texts.c_str());
        fix_end();
    }
    // a folder too long for the file table's names: the literal, as the original (the stubs put it in _drv_c)
    {
        fix_begin();
        g_fake_exe = g_fx + std::string(250 - g_fx.size() - 1, 'r') + "\\race.exe";   // a 250-character folder
        FIX_CHECK(log_file_begin_rw() == 1, "(long) log_file_begin failed");
        const int f = g_log_file;
        FIX_CHECK(f > 0 && !strcmp(file_at(m1_operand(0x00411583), f - 1)->name, "c:\\log.log") && is_file(std::string(g_sb) + "\\_drv_c\\log.log") &&
                      s_log_path[0] == 0, "(long): the log isn't c:\\log.log");
        FIX_CHECK(logged("the game's log stays at c:\\log.log"), "(long) logged: %s", g_logf_text.c_str());
        const int other = file_open("a.txt");
        g_texts.clear();
        FileVerifyNoOpenFiles_rw();
        FIX_CHECK(other > 0 && g_texts == "log: Handle to a.txt was never freed.\n", "(long) FileVerifyNoOpenFiles reported: %s",
                  g_texts.c_str());
        fix_end();
    }
}

// FileReadLine as the fix has it: from `at` in `s`, into buf[0..n): {return, bytes consumed, buffer}. A CRLF line's '\r'
// is cut (the original's behaviour), a bare LF line keeps every character, nothing is written outside buf[0..n).
struct LineModel { uint8_t r; size_t used; std::string buf; };
static LineModel line_model(const std::string& s, size_t at, int n, const std::string& prior) {
    LineModel m{0, 0, prior};
    if (n <= 0) return m;
    int i = 0;
    while (at + m.used < s.size()) {
        const char c = s[at + m.used++];
        if (c == '\n') {
            if (i > 0 && m.buf[i - 1] == '\r') m.buf[i - 1] = 0;
            m.buf[i] = 0;
            m.r = 1;
            return m;
        }
        m.buf[i++] = c;
        if (i >= n) return m;
    }
    return m;
}
static std::string rand_line(bool cr_inside) {
    std::string l;
    const int len = chance(15) ? 0 : chance(10) ? ri(40, 90) : ri(1, 12);
    for (int i = 0; i < len; i++) {
        const int k = ri(0, 60);
        l += cr_inside && k == 0 ? '\r' : k == 1 ? '\t' : (char)ri(0x20, 0x7e);
    }
    return l;
}
static void fix_read_line_tests() {
    typedef uint8_t(__cdecl* ReadLine_t)(int, char*, int);
    typedef int(__cdecl* Open_t)(const char*);
    const ReadLine_t orig = (ReadLine_t)0x004119b0;
    const Open_t file_open = (Open_t)0x00411780;
    typedef void(__cdecl* Close_t)(int*);
    const Close_t file_close = (Close_t)0x00411850;
    enum { GUARD = 16, CAP = 160 };
    long files = 0, calls[2] = {0, 0}, underruns = 0, bad_crlf = 0, bad_model = 0, lf_lines = 0, exact = 0;
    // the explicit cases first: the fixture's bare-LF file, and exact buffer lengths
    {
        fix_begin();
        const int f = file_open("lf.txt");                 // "\nsecond\nthird line\n\nfifth"
        char* b = (char*)abuf(GUARD);
        const char* want[] = {"", "second", "third line", ""};
        for (int k = 0; k < 4; k++) {
            memset(b - GUARD, 0x5a, 64 + 2 * GUARD);
            const uint8_t r = FileReadLine_rw(f, b, 64);
            FIX_CHECK(r == 1 && !strcmp(b, want[k]) && b[-1] == 0x5a, "lf.txt line %d: %d \"%s\" (before the buffer: %02x)", k, r, b,
                      (uint8_t)b[-1]);
        }
        memset(b - GUARD, 0x5a, 64 + 2 * GUARD);
        FIX_CHECK(FileReadLine_rw(f, b, 64) == 0 && !memcmp(b, "fifth", 5) && b[5] == 0x5a, "lf.txt: the last line");
        int ff = f;
        file_close(&ff);
        // n = 8: a line that fills n - 1 bytes (LF: 7 characters; CRLF: 6 and its '\r') is read whole; one that
        // fills all 8 returns 0 unterminated, and its '\n' is then read as an empty line -- which the original ended by
        // writing buf[-1] (for CRLF too: the '\r' was the buffer's last byte)
        for (int crlf = 0; crlf < 2; crlf++) {
            const std::string text = crlf ? "abcdef\r\nabcdefg\r\n" : "abcdefg\nabcdefgh\n";
            const char* const tag = crlf ? "CRLF" : "LF";
            put_file("exact.txt", text.data(), text.size(), 0);
            const int g = file_open("exact.txt");
            const int n = 8;
            memset(b - GUARD, 0x5a, n + 2 * GUARD);
            uint8_t r = FileReadLine_rw(g, b, n);
            FIX_CHECK(r == 1 && !strcmp(b, crlf ? "abcdef" : "abcdefg") && b[-1] == 0x5a, "exact (%s): the n - 1 line: %d \"%s\"", tag, r, b);
            memset(b - GUARD, 0x5a, n + 2 * GUARD);
            r = FileReadLine_rw(g, b, n);
            FIX_CHECK(r == 0 && !memcmp(b, crlf ? "abcdefg\r" : "abcdefgh", 8) && b[-1] == 0x5a && b[8] == 0x5a,
                      "exact (%s): the full line: %d", tag, r);
            memset(b - GUARD, 0x5a, n + 2 * GUARD);
            r = FileReadLine_rw(g, b, n);
            FIX_CHECK(r == 1 && b[0] == 0 && b[-1] == 0x5a && b[1] == 0x5a,
                      "exact (%s): the lone '\\n' after a full buffer: %d, before the buffer %02x", tag, r, (uint8_t)b[-1]);
            memset(b - GUARD, 0x5a, n + 2 * GUARD);
            FIX_CHECK(FileReadLine_rw(g, b, n) == 0 && b[0] == 0x5a, "exact (%s): the end", tag);
            ff = g;
            file_close(&ff);
        }
        fix_end();
    }
    // random files: CRLF-only ones must read exactly as the original (bytes, return, position) -- apart from the
    // original's write before the buffer when a call starts at a '\n' (a CRLF line split after its '\r'), where the
    // fix leaves that byte alone; LF-only and mixed ones must read as the model says
    for (int round = 0; round < 40; round++) {
        fix_begin();
        for (int it = 0; it < 60; it++, files++) {
            const int kind = it % 3;                       // 0 CRLF, 1 bare LF, 2 mixed
            std::string text;
            const int nl = ri(0, 12);
            for (int k = 0; k < nl; k++) {
                text += rand_line(kind == 0 || kind == 2);
                text += kind == 0 ? "\r\n" : kind == 1 ? "\n" : chance(50) ? "\r\n" : "\n";
            }
            if (chance(50)) text += rand_line(kind != 1);
            put_file("rl.txt", text.data(), text.size(), 0);
            const int fa = file_open("rl.txt"), fb = file_open("rl.txt");
            size_t pos = 0;
            for (int call = 0; call < 200; call++) {
                // n: random, or around the next line's length (exact fits and full buffers)
                size_t next = text.find('\n', pos);
                const int line_len = (int)((next == std::string::npos ? text.size() : next) - pos);
                const int pick_n = ri(0, 9);
                int n = pick_n < 3 ? line_len + pick_n : pick_n == 3 ? line_len : pick_n == 4 ? ri(0, 2) : ri(1, 100);
                if (n > CAP) n = CAP;
                if (n == line_len || n == line_len + 1) exact++;
                char* ba = (char*)abuf(GUARD);
                char* bb = (char*)abuf(0x1000 + GUARD);
                for (int k = 0; k < CAP + 2 * GUARD; k++) ba[k - GUARD] = bb[k - GUARD] = (char)(0xa0 + (k & 15));
                const std::string prior(bb, CAP);
                const uint8_t rb = FileReadLine_rw(fb, bb, n);
                calls[1]++;
                const DWORD posb = kSetFilePointer(FH(m1_operand(0x00411555), fb - 1), 0, 0, FILE_CURRENT);
                if (kind == 0) {
                    const uint8_t ra = orig(fa, ba, n);
                    calls[0]++;
                    const DWORD posa = kSetFilePointer(FH(m1_operand(0x00411555), fa - 1), 0, 0, FILE_CURRENT);
                    const bool starts_lf = pos < text.size() && text[pos] == '\n' && n > 0;
                    if (starts_lf && ba[-1] == 0 && bb[-1] == (char)(0xa0 + ((GUARD - 1) & 15))) {
                        underruns++;
                        ba[-1] = bb[-1];                   // (the original's write before the buffer: the fix's one change)
                    }
                    if ((ra != rb || posa != posb || memcmp(ba - GUARD, bb - GUARD, CAP + 2 * GUARD)) && bad_crlf++ < 5)
                        printf("  FAIL CRLF file %ld call %d (n %d, at %u): original %d at %lu, fix %d at %lu, buffers %s\n", files, call, n,
                               (unsigned)pos, ra, posa, rb, posb, memcmp(ba - GUARD, bb - GUARD, CAP + 2 * GUARD) ? "differ" : "same");
                }
                const LineModel m = line_model(text, pos, n, prior);
                const bool guards_ok = [&]() {
                    for (int k = -GUARD; k < 0; k++)
                        if (bb[k] != (char)(0xa0 + ((k + GUARD) & 15))) return false;
                    for (int k = n > 0 ? n : 0; k < CAP + GUARD; k++)
                        if (bb[k] != (char)(0xa0 + ((k + GUARD) & 15))) return false;
                    return true;
                }();
                if (kind != 0 && m.r == 1 && (int)m.used > 0) lf_lines++;
                if ((rb != m.r || posb != pos + m.used || memcmp(bb, m.buf.data(), n > 0 ? n : 0) || !guards_ok) && bad_model++ < 5)
                    printf("  FAIL %s file %ld call %d (n %d, at %u): %d at %lu, the model %d at %u, buffer %s, guards %s\n",
                           kind == 0 ? "CRLF" : kind == 1 ? "LF" : "mixed", files, call, n, (unsigned)pos, rb, posb, m.r,
                           (unsigned)(pos + m.used), memcmp(bb, m.buf.data(), n > 0 ? n : 0) ? "differs" : "same", guards_ok ? "intact" : "WRITTEN");
                pos = posb;
                if (pos >= text.size() && rb == 0) break;
            }
            int x = fa, y = fb;
            file_close(&x);
            file_close(&y);
        }
        fix_end();
    }
    g_fix_checks += 2;
    g_fix_bad += (bad_crlf != 0) + (bad_model != 0);
    printf("  FileReadLine: %ld random files, %ld calls (%ld against the original on CRLF files: %ld differ; %ld where the original "
           "wrote before the buffer), %ld bare-LF lines, %ld exact-length buffers; %ld differ from the model\n",
           files, calls[1], calls[0], bad_crlf, underruns, lf_lines, exact, bad_model);
}

// the open-file table as M1 moves it (the stock one must stay untouched)
static bool zeros(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}
static int slot_of_handle_count() {                    // slots holding a handle
    int k = 0;
    for (int i = 0; i < VP_LIFT_FILES; i++)
        if (*(HANDLE*)(g_moved + i * 0x104 + 0x100) != INVALID_HANDLE_VALUE) k++;
    return k;
}
static void fix_file_table_tests() {
    move_file_table(true);
    g_granges[sizeof g_granges / sizeof *g_granges - 1] = {(uint32_t)(uintptr_t)g_moved, MOVED_SPAN, "the open-file table, moved"};
    const uint8_t* stock = (const uint8_t*)0x00505ea8;
    // 1. the rewrites, chained as in the game: 300 files, 256 open at once
    {
        fix_begin();
        g_mode = 2;
        chain_patch();
        FIX_CHECK(slot_of_handle_count() == 0 && zeros(g_moved + MOVED_BYTES, MOVED_SPAN - MOVED_BYTES),
                  "FileBegin (the original, moved): every slot free");
        std::vector<int> fs;
        int bad_read = 0, panic_at = -1;
        for (int i = 0; i < 300; i++) {
            char nm[32], body[64];
            snprintf(nm, sizeof nm, "tbl%03d.txt", i);
            const int len = snprintf(body, sizeof body, "file %d of the moved table", i);
            put_file(nm, body, (size_t)len, 0);
            const int f = (int)CALL(FileOpen_rw, (const char*)astr(nm));
            if (g_cur->trace.back().result) { panic_at = i; break; }
            fs.push_back(f);
        }
        std::vector<int> sorted = fs;
        std::sort(sorted.begin(), sorted.end());
        bool distinct = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end() && !sorted.empty() && sorted.front() == 1 &&
                        sorted.back() == VP_LIFT_FILES;
        for (size_t i = 0; i < fs.size(); i++) {
            char want[64];
            const int len = snprintf(want, sizeof want, "file %d of the moved table", (int)i);
            uint8_t* b = abuf(0);
            memset(b, 0, 64);
            const int size = (int)CALL(FileSize_rw, fs[i]);
            const uint8_t ok = (uint8_t)CALL(FileReadExact_rw, fs[i], (void*)b, len);
            if (size != len || !ok || memcmp(b, want, len)) bad_read++;
        }
        const int held = slot_of_handle_count();
        FIX_CHECK((int)fs.size() == VP_LIFT_FILES && panic_at == VP_LIFT_FILES && distinct && !bad_read && held == VP_LIFT_FILES,
                  "the rewrites: %d files open (Files 1..%d distinct: %d), the next %s, %d read back wrong, %d slots held",
                  (int)fs.size(), VP_LIFT_FILES, distinct, panic_at == VP_LIFT_FILES ? "panics (MAX_FILES)" : "DOESN'T", bad_read, held);
        *acell(1) = fs[77];
        CALL(FileClose_rw, acell(1));
        const int again = (int)CALL(FileOpen_rw, (const char*)astr("tbl000.txt"));
        FIX_CHECK(again == fs[77], "a slot freed in the full table is reused: File %d (want %d)", again, fs[77]);
        for (size_t i = 0; i < fs.size(); i++) {
            *acell(1) = fs[i];
            CALL(FileClose_rw, acell(1));
        }
        CALL0(FileVerifyNoOpenFiles_rw);
        FIX_CHECK(slot_of_handle_count() == 0 && zeros(g_moved + MOVED_BYTES, MOVED_SPAN - MOVED_BYTES) && zeros(stock, 32 * 0x104),
                  "all closed: %d slots held; nothing past the moved table; the stock table untouched: %d", slot_of_handle_count(),
                  zeros(stock, 32 * 0x104));
        fix_end();
    }
    // 2. the originals on the moved table (their footprints checked: the rewrites' footprints must cover them)
    {
        fix_begin();
        g_mode = 0;
        int n = 0, panic_at = -1;
        for (int i = 0; i < 140; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "orig%03d.txt", i);
            put_file(nm, "x", 1, 0);
            const int f = (int)CALL(FileOpen_rw, (const char*)astr(nm));
            if (g_cur->trace.back().result) { panic_at = i; break; }
            n++;
            *acell(10 + i) = f;
        }
        FIX_CHECK(n == 0x7f && panic_at == 0x7f && *(int32_t*)0x00505ea0 == 0x7f,
                  "the originals: %d files open (the imm8's 127), the next %s", n, panic_at == 0x7f ? "panics" : "DOESN'T");
        CALL(FileClose_rw, acell(10 + 40));
        const int again = (int)CALL(FileOpen_rw, (const char*)astr("orig000.txt"));
        FIX_CHECK(again == 41, "the originals reuse a freed slot: File %d (want 41)", again);
        // 3. mixed: the rewrite (the whole table) after the originals' 127
        g_mode = 1;
        int m = 0;
        for (int i = 0; i < 140; i++) {
            put_file("mixed.txt", "y", 1, 0);
            const int f = (int)CALL(FileOpen_rw, (const char*)astr("mixed.txt"));   // (FileOpen_rw calls the ORIGINAL alloc_file)
            if (g_cur->trace.back().result) break;
            (void)f;
            m++;
        }
        FIX_CHECK(m == 0, "FileOpen's rewrite with the original alloc_file: still the original's limit (%d more)", m);
        int k = 0;
        for (int i = 0; i < 140; i++) {
            const int s = (int)CALL(alloc_file_rw, (const char*)astr("fake"), (HANDLE)(uintptr_t)(0x70000 + i));
            if (g_cur->trace.back().result) break;
            if (s != 0x7f + i) break;
            k++;
        }
        FIX_CHECK(k == VP_LIFT_FILES - 0x7f && *(int32_t*)0x00505ea0 == VP_LIFT_FILES &&
                      zeros(g_moved + MOVED_BYTES, MOVED_SPAN - MOVED_BYTES) && zeros(stock, 32 * 0x104),
                  "alloc_file's rewrite after them: slots 127..%d (%d), then its panic; nothing past the table, the stock one "
                  "untouched", 0x7f + k - 1, k);
        for (int i = 0; i < VP_LIFT_FILES; i++) *(HANDLE*)(g_moved + i * 0x104 + 0x100) = INVALID_HANDLE_VALUE;   // (the fakes)
        fix_end();
    }
    move_file_table(false);
    g_granges[sizeof g_granges / sizeof *g_granges - 1] = {0, 0, "the open-file table, moved"};
}

static int fix_tests() {
    g_logf_quiet = true;
    GetTempPathA(MAX_PATH, g_tmp);
    fix_user_directory_tests();
    printf("fix build: the user directory: %d checks, %d failed\n", g_fix_checks, g_fix_bad);
    const int c0 = g_fix_checks, b0 = g_fix_bad;
    fix_log_tests();
    printf("fix build: the log: %d checks, %d failed\n", g_fix_checks - c0, g_fix_bad - b0);
    const int c1 = g_fix_checks, b1 = g_fix_bad;
    fix_read_line_tests();
    printf("fix build: FileReadLine: %d checks, %d failed\n", g_fix_checks - c1, g_fix_bad - b1);
    const int c2 = g_fix_checks, b2 = g_fix_bad;
    fix_file_table_tests();
    printf("fix build: the open-file table, moved: %d checks, %d failed\n", g_fix_checks - c2, g_fix_bad - b2);
    SetCurrentDirectoryA(g_tmp);
    rm_tree(g_sb);
    printf("fix build: %d checks, %d failed\n", g_fix_checks, g_fix_bad);
    return g_fix_bad ? 1 : 0;
}
#endif

int main(int argc, char** argv) {
    // never a dialog on the desktop: no Windows error boxes, no CRT message boxes (inherited by the child)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (!GetEnvironmentVariableA("VP_K2_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const int scenarios = argc > 1 ? atoi(argv[1]) : 300;
    const uint32_t seed0 = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 1;
    char exe[MAX_PATH], game[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_krn_file.cpp");
    if (!s) { printf("can't find the repository from %s\n", __FILE__); return 2; }
    *s = 0;
    if (getenv("K2_REPO")) snprintf(exe, sizeof exe, "%s", getenv("K2_REPO"));   // (a copy built elsewhere)
    snprintf(game, sizeof game, "%s\\..\\game-files\\installs\\v1.0-RC", exe);
    strcat(exe, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_iat();
    install_stubs();
    {   // the mono memory: 0xb0000 reserved by the parent, or -- where the loader puts the main thread's 1 MB stack there
        // (Windows 11) -- the far, never used end of that stack's reservation (this harness uses well under 200 KB)
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((void*)MONO_AT, &mbi, sizeof mbi);
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        const bool ours = mbi.AllocationBase == (void*)MONO_AT &&
                          (mbi.State == MEM_RESERVE && mbi.RegionSize >= 0x10000) &&
                          ((uintptr_t)tib->StackBase <= MONO_AT || (uintptr_t)tib->StackLimit > MONO_AT + 0x40000);
        g_have_mono = ours && VirtualAlloc((void*)MONO_AT, 0x10000, MEM_COMMIT, PAGE_READWRITE) == (void*)MONO_AT;
        if (!g_have_mono)
            printf("0xb0000 is in use (state %lx, base %p): the mono monitor's writes aren't tested\n", mbi.State, mbi.AllocationBase);
    }
    g_pristine_data.assign((uint8_t*)DATA_AT, (uint8_t*)DATA_AT + DATA_SIZE);
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    snprintf(g_sb, sizeof g_sb, "%sk2sb_%lu", tmp, GetCurrentProcessId());
    snprintf(g_slot_prefix, sizeof g_slot_prefix, "\\\\.\\mailslot\\k2p%lu\\", GetCurrentProcessId());
    load_fixtures(game);
    g_arena = (uint8_t*)VirtualAlloc((void*)0x30000000, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    printf("imports: %d stubbed, %d real (KERNEL32), %d trapped; %d fixtures; mono %s; sandbox %s\n", g_nstubbed, g_nreal,
           g_ntrapped, (int)g_fixtures.size(), g_have_mono ? "yes" : "no", g_sb);
    if (argc > 3 && !strcmp(argv[3], "moved")) {
        move_file_table(true);
        g_granges[sizeof g_granges / sizeof *g_granges - 1] = {(uint32_t)(uintptr_t)g_moved, MOVED_SPAN, "the open-file table, moved"};
        printf("the open-file table moved as M1 moves it: %d slots at %p (alloc_file's imm8 0x7f)\n", VP_LIFT_FILES, g_moved);
    }
#ifdef FIX_TESTS
    (void)scenarios;
    return fix_tests();
#endif
    long bad = check_error_strings();
    long ok_scen = 0;
    for (int i = 0; i < scenarios; i++) {
        const uint32_t seed = (seed0 * 2654435761u + (uint32_t)i * 40503u) | 1;
        for (int m = 0; m < 3; m++) {
            pass_begin(m, seed);
            scenario();
            pass_end();
        }
        if (getenv("K2_DUMP")) printf("---- scenario %d, the originals' texts:\n%s", i, g_res[0].texts.c_str());
        const bool a = compare(1, i), b = compare(2, i);
        if (a && b) ok_scen++;
        else g_scenarios_failed++;
    }
    SetCurrentDirectoryA(tmp);
    rm_tree(g_sb);
    for (auto& st : g_stats) g_cov[C_PANIC] += st.second.panics, g_cov[C_FAULT] += st.second.faults;
    printf("%-36s %8s %7s %7s %9s %8s %8s\n", "function", "calls", "panics", "faults", "isolated", "chain", "fp-miss");
    int failed = 0;
    for (auto& st : g_stats) {
        const Stat& x = st.second;
        printf("%-36s %8ld %7ld %7ld %9ld %8ld %8ld\n", st.first.c_str(), x.calls, x.panics, x.faults, x.fails[1], x.fails[2], x.fp_fails);
        if (x.fails[1] || x.fails[2] || x.fp_fails) failed++;
    }
    int never = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next)
        if (!g_stats.count(r->name) && strcmp(r->name, "Win32GetErrorString(int)")) { printf("never called: %s\n", r->name); never++; }
    printf("%d scenarios (%ld agree in both modes), %d functions differ or escape their footprint, %d never called; views moved %ld\n",
           scenarios, ok_scen, failed, never, g_view_moved);
    printf("covered:");
    for (int i = 0; i < NCOV; i++) printf("%s %s %ld", i ? "," : "", k_cov_names[i], g_cov[i]);
    printf("\n");
    return failed || bad || g_scenarios_failed ? 1 : 0;
}
