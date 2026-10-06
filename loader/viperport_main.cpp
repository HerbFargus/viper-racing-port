// viperport.exe -- runs the v1.0 game on the port's code alone (hook/standalone.h says how the two halves meet).
//
//   viperport.exe [--race file] [game arguments]      beside race.exe and the port's dinput.dll: the game
//   viperport.exe --check [race.exe] [--dll path]     map, install, fill and verify; print the report; start nothing
//   viperport.exe --probe [race.exe] [--dll path]     "will the standalone run this race.exe?" -- exit code 0 = yes,
//                                                     else no; one line ("yes: ..." / "no: ...") on stdout and in
//                                                     viperport-probe.txt. No window, no SDL, no fill (see probe())
// --race names the v1.0 race.exe to run when it isn't race.exe beside viperport.exe (e.g. vrmod's stock copy,
// race.exe.vrmod-original); a relative path is taken from the current folder, else from viperport.exe's.
//
// The game's code and data come from the user's own installed race.exe (v1.0 only: its PE timestamp, 0x362de68c; the
// race.bin builds keep the dinput.dll route). It is mapped at 0x400000 as an image section (SEC_IMAGE), as Windows maps
// an exe -- each section at its RVA with its protection, MEM_IMAGE (so its SEH handlers dispatch with DEP on); v1.0 has
// no relocations and no TLS -- and its imports are resolved here, by name or ordinal, into its own IAT. The game sees
// itself as the process's module through three of those slots (GetModuleHandleA(NULL) -> 0x400000, GetModuleFileNameA
// -> race.exe's path, GetCommandLineA -> race.exe's path and the game's arguments). The port DLL is then loaded by path
// (the same dinput.dll file the dinput.dll route uses) with VIPERPORT_STANDALONE set, and installs from its DllMain on
// top of the resolved IAT, with GetModuleHandle(NULL) briefly race.exe's (set_process_module); the one export does the
// rest.
//
// The stand-ins (relink stage R2a): the DLL's table (hook/w32_table.h -- the port's own implementations of race.exe's
// Windows imports; every one in the GCC build, none in MSVC's) is read as Windows maps the DLL, before its DllMain
// installs the port, and fills race.exe's import slots; what has no stand-in keeps Windows' function and is logged
// (viperport.log, --check's report, and VP_TRACE_IMPORTS' import-trace.txt, where each import says what serves it).
//
// This program is linked at 0x00800000 (fixed), away from race.exe's 0x400000-0x62c000. The process heap can still
// land there before main runs; then the program starts itself again suspended, with the range reserved before the
// child's heap exists, and waits for it.
#define _CRT_SECURE_NO_WARNINGS
#define VP_LOADER
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "../hook/standalone.h"
#include "../hook/w32_table.h"

namespace {

const uint32_t BASE = 0x400000;
const uint32_t V10_TIMESTAMP = 0x362de68c;
const uint32_t V10_IMAGE = 0x22c000;                   // v1.0's SizeOfImage
// SHA-256 of the stock v1.0 race.exe (2,404,451 bytes; the disc's Data\race.exe)
const char* const V10_SHA256 = "739619fd213c1e8c234b4712b2bae2a8b362d19f94f6be08c477a42e39d6dd81";
const char* const CHILD_ENV = "VIPERPORT_LOADER_CHILD";

bool g_check;
bool g_probe;                                           // --probe: a refusal is kept for its one line, not printed
std::string g_probe_why;                                // (the first refusal)
HANDLE g_stdout = INVALID_HANDLE_VALUE;
FILE* g_report;
std::string g_fails;                                    // the run's first FAIL lines, for the message box
std::string g_self_dir;                                 // viperport.exe's folder (viperport-probe.txt goes there)

void __cdecl out(const char* line) {
    if (!strncmp(line, "FAIL: ", 6) && g_fails.size() < 1500) { g_fails += line + 6; g_fails += "\n"; }
    if (g_stdout != INVALID_HANDLE_VALUE && g_stdout) {
        DWORD w;
        WriteFile(g_stdout, line, (DWORD)strlen(line), &w, 0);
        WriteFile(g_stdout, "\r\n", 2, &w, 0);
    }
    if (g_report) { fprintf(g_report, "%s\n", line); fflush(g_report); }
}

void say(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    out(line);
}

// is anyone reading this process's output? (a GUI program started from Explorer or a shortcut has no stdout; a launcher
// that pipes it, or sends it to NUL, does)
bool have_stdout() {
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    return h && h != INVALID_HANDLE_VALUE;
}

// the one line a probe, or a launch that refuses, leaves: on stdout (if there is one) and in viperport-probe.txt
void probe_line(const std::string& line) {
    if (have_stdout()) {
        DWORD w;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.c_str(), (DWORD)line.size(), &w, 0);
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &w, 0);
    }
    if (FILE* f = fopen((g_self_dir + "viperport-probe.txt").c_str(), "w")) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }
}

// a failure before the game starts: printed (--check); kept for the one line (--probe); or, at a launch, the process
// ends at once with exit code 2 -- before any window -- with the reason as a "no: ..." line on stdout and in
// viperport-probe.txt, so a launcher can fall back. A message box says it too, but only when there is no stdout (started
// from Explorer or a shortcut): a launcher that gives viperport.exe a stdout (a pipe, or NUL) is never kept waiting.
int refuse(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    if (g_probe) {
        if (g_probe_why.empty()) g_probe_why = !strncmp(line, "viperport: ", 11) ? line + 11 : line;
        return 2;
    }
    out(line);
    if (!g_check) {
        std::string why = !strncmp(line, "viperport: ", 11) ? line + 11 : line;
        if (!g_fails.empty()) why += " -- " + g_fails.substr(0, g_fails.find('\n'));   // the first FAIL line says why
        probe_line("no: " + why);
        if (!have_stdout()) {
            const std::string box = g_fails.empty() ? std::string(line) : std::string(line) + "\n\n" + g_fails;
            MessageBoxA(0, box.c_str(), "viperport", MB_OK | MB_ICONERROR);
        }
    }
    return 2;
}

std::string folder_of(const std::string& path) {
    const size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? std::string(".\\") : path.substr(0, s + 1);
}

std::string full_path(const std::string& p) {
    char buf[MAX_PATH];
    const DWORD n = GetFullPathNameA(p.c_str(), MAX_PATH, buf, 0);
    return n && n < MAX_PATH ? std::string(buf) : p;
}

// a relative path: from the current folder if the file is there, else from viperport.exe's folder
std::string resolve_path(const std::string& p, const std::string& self_dir) {
    const std::string here = full_path(p);
    if (GetFileAttributesA(here.c_str()) != INVALID_FILE_ATTRIBUTES) return here;
    const bool relative = !(p.size() > 1 && (p[1] == ':' || (p[0] == '\\' && p[1] == '\\')));
    return relative ? full_path(self_dir + p) : here;
}

std::string sha256_hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_HASH_HANDLE h = 0;
    uint8_t digest[32] = {};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0) == 0) {
        if (BCryptCreateHash(alg, &h, 0, 0, 0, 0, 0) == 0) {
            BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0);
            if (BCryptFinishHash(h, digest, sizeof digest, 0) == 0) {
                char two[3];
                for (uint8_t b : digest) { sprintf(two, "%02x", b); hex += two; }
            }
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}

// ---- the command line ---------------------------------------------------------------------------------------------
// the arguments after the program's own name, split the way the C runtime splits them (enough for our flags)
struct Arg { std::string text; std::string raw; };     // unquoted, and as written
std::vector<Arg> args_of(const char* cl) {
    std::vector<Arg> v;
    const char* p = cl;
    bool first = true;
    while (*p) {
        while (*p == ' ' || *p == '	') p++;
        if (!*p) break;
        const char* start = p;
        std::string a;
        bool q = false;
        while (*p && (q || (*p != ' ' && *p != '	'))) {
            if (*p == '"') q = !q;
            else a += *p;
            p++;
        }
        if (!first) v.push_back({a, std::string(start, p)});
        first = false;
    }
    return v;
}

// ---- 0x400000 ------------------------------------------------------------------------------------------------------
// Start this program again, suspended, reserve race.exe's range in it before anything of its own runs, let it go and
// wait: the child finds the range reserved, releases it and maps race.exe there at once.
int relaunch_with_range_reserved() {
    SetEnvironmentVariableA(CHILD_ENV, "1");
    STARTUPINFOA si = {sizeof si};
    GetStartupInfoA(&si);
    PROCESS_INFORMATION pi;
    char self[MAX_PATH];
    GetModuleFileNameA(0, self, MAX_PATH);
    std::vector<char> cl(GetCommandLineA(), GetCommandLineA() + strlen(GetCommandLineA()) + 1);
    if (!CreateProcessA(self, cl.data(), 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi))
        return refuse("viperport: 0x400000 is taken in this process and a fresh one couldn't be started (%lu)", GetLastError());
    // the child (the game) lives only as long as this process: whoever ends viperport.exe -- a launcher, Task Manager --
    // ends the game too, instead of leaving it running unseen. The job's handle closes when this process ends.
    if (HANDLE job = CreateJobObjectA(0, 0)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
            !AssignProcessToJobObject(job, pi.hProcess))
            CloseHandle(job);                           // (best effort: the game still runs)
    }
    if (!VirtualAllocEx(pi.hProcess, (void*)(uintptr_t)BASE, V10_IMAGE, MEM_RESERVE, PAGE_NOACCESS)) {
        TerminateProcess(pi.hProcess, 2);
        return refuse("viperport: couldn't reserve race.exe's range 0x400000 in a fresh process (%lu)", GetLastError());
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

// ---- the image -----------------------------------------------------------------------------------------------------
struct Image {
    std::vector<uint8_t> file;
    const IMAGE_NT_HEADERS32* nt = 0;
    uint8_t* base = 0;
};

const IMAGE_NT_HEADERS32* headers_of(const std::vector<uint8_t>& f) {
    if (f.size() < 0x400 || f[0] != 'M' || f[1] != 'Z') return 0;
    const uint32_t pe = *(const uint32_t*)&f[0x3c];
    if (pe + sizeof(IMAGE_NT_HEADERS32) > f.size()) return 0;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)&f[pe];
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : 0;
}

// race.exe mapped as an image section (SEC_IMAGE) at its own base -- what the Windows loader does for an exe: each
// section at its RVA with its own protection, the rest of .data zero, and the memory an image's (MEM_IMAGE). That last
// part matters: the game's SEH frames name handlers inside race.exe (_WinMainCRTStartup's: _except_handler3), and with
// DEP on Windows won't dispatch to a handler in private memory -- --check raises an exception through such a frame.
// Writes (the IAT, the hooks, the fill) are copy-on-write: the file is never changed.
int map_image(Image& im, const std::string& path) {
    VirtualFree((void*)(uintptr_t)BASE, 0, MEM_RELEASE);    // our reservation (or the parent's, in a child) gives way
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return refuse("viperport: %s doesn't open to map (%lu)", path.c_str(), GetLastError());
    HANDLE sec = CreateFileMappingA(f, 0, PAGE_EXECUTE_READ | SEC_IMAGE, 0, 0, 0);
    CloseHandle(f);
    if (!sec) return refuse("viperport: %s couldn't be mapped as an image (%lu)", path.c_str(), GetLastError());
    void* b = MapViewOfFileEx(sec, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, 0, (void*)(uintptr_t)BASE);
    CloseHandle(sec);                                        // (the view keeps the section)
    if (b != (void*)(uintptr_t)BASE)
        return refuse("viperport: couldn't map race.exe's image at 0x400000 (%lu)", GetLastError());
    im.base = (uint8_t*)b;
    // the view must hold exactly the file's sections (what the audit compares against)
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(im.nt);
    for (int i = 0; i < im.nt->FileHeader.NumberOfSections; i++) {
        const uint32_t raw = s[i].SizeOfRawData < s[i].Misc.VirtualSize || !s[i].Misc.VirtualSize ? s[i].SizeOfRawData
                                                                                                   : s[i].Misc.VirtualSize;
        if (s[i].PointerToRawData + raw > im.file.size() || memcmp(im.base + s[i].VirtualAddress, &im.file[s[i].PointerToRawData], raw))
            return refuse("viperport: race.exe's section %.8s isn't what the file holds once mapped", s[i].Name);
    }
    return 0;
}

// The DPI behaviour Windows would give race.exe. A player who set race.exe's Compatibility -> "Change high DPI settings ->
// Override high DPI scaling: Application" (the registry's AppCompatFlags\Layers value for its path holds HIGHDPIAWARE)
// gets the game at the screen's real size; viperport.exe is another exe, so Windows wouldn't apply that to it and
// would draw the game DPI-virtualised (1536x864 on a 3840x2160 screen at 250%) -- and a session recorded through race.exe
// wouldn't replay here. The same setting is honoured: made system DPI-aware, as the flag does, before any window.
void mirror_dpi_compat(const std::string& exe) {
    static const char* const key = "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY k;
        if (RegOpenKeyExA(root, key, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) continue;
        char data[512] = {};
        DWORD type = 0, n = sizeof data - 1;
        const LONG r = RegQueryValueExA(k, exe.c_str(), 0, &type, (BYTE*)data, &n);
        RegCloseKey(k);
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        if (strstr(data, "HIGHDPIAWARE")) {
            SetProcessDPIAware();
            return;
        }
    }
}

// race.exe's GetModuleFileNameA import: its own module (NULL, or the image's handle) is race.exe, as it would be had
// Windows started it -- the game's paths come from its folder, and its crash report reads the link map appended to the
// exe it names. Any other module: the real thing.
std::string g_race_path;
void** g_gmfn_slot;                                     // its slot in race.exe's IAT
DWORD WINAPI race_GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    if (m && m != (HMODULE)(uintptr_t)BASE) return GetModuleFileNameA(m, buf, n);
    if (!n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    const DWORD len = (DWORD)g_race_path.size();
    if (len >= n) {                                     // truncated, terminated, as Windows does
        memcpy(buf, g_race_path.c_str(), n - 1);
        buf[n - 1] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    memcpy(buf, g_race_path.c_str(), len + 1);
    SetLastError(ERROR_SUCCESS);
    return len;
}

// race.exe's GetModuleHandleA import: NULL is race.exe's image (WinMain's hInstance, the window class, LoadIcon,
// LoadBitmap: all on 0x400000, as when Windows starts race.exe); a named module: the real thing.
HMODULE WINAPI race_GetModuleHandleA(LPCSTR name) {
    if (!name) { SetLastError(ERROR_SUCCESS); return (HMODULE)(uintptr_t)BASE; }
    return GetModuleHandleA(name);
}

// race.exe's TAPI32 lineInitialize import: TAPI refuses the game's hInstance 0x400000 (an image Windows didn't load:
// "Can't initialize TAPI"), so it gets the process's module, as on the dinput.dll route where race.exe is that module.
// The only TAPI call that takes an hInstance. Without it the modem line never starts and the game takes another path.
typedef LONG(WINAPI* LineInitialize_t)(void* line_app, HINSTANCE inst, void* callback, LPCSTR app, DWORD* devices);
LineInitialize_t g_line_initialize;
LONG WINAPI race_lineInitialize(void* line_app, HINSTANCE inst, void* callback, LPCSTR app, DWORD* devices) {
    if (inst == (HINSTANCE)(uintptr_t)BASE) inst = GetModuleHandleA(0);
    return g_line_initialize(line_app, inst, callback, app, devices);
}

// race.exe's GetCommandLineA import: as if Windows had started race.exe -- its own path first, then the game's
// arguments as written (the loader's --race / --dll / --check taken out). The C runtime's argv and WinMain's command
// line come from it.
std::string g_race_cmdline;
LPSTR WINAPI race_GetCommandLineA() { return (LPSTR)g_race_cmdline.c_str(); }

// race.exe's DINPUT import, until the port DLL is loaded and its own export takes the slot
HRESULT WINAPI dinput_not_yet(HINSTANCE, DWORD, void**, void*) { return E_FAIL; }

// ---- race.exe's import slots ------------------------------------------------------------------------------------------
// Every slot resolve_imports filled, with what it imports and how it's served now. A slot's value goes through set_slot,
// which keeps VP_TRACE_IMPORTS' counting stub in front of whatever serves it.
enum Served { WINDOWS, SHIM, STAND_IN, PORT };          // Windows' own; the loader's shim; a stand-in; the DLL's own
struct Slot {
    uint32_t* at;
    std::string dll, name;                              // name empty: by ordinal
    uint32_t ordinal;
    std::string what;                                   // "KERNEL32.dll!CreateFileA", "WSOCK32.dll #23"
    int trace;                                          // its VP_TRACE_IMPORTS entry, or -1
    Served served;
};
std::vector<Slot> g_slots;
Slot* slot_named(const char* what) {
    for (Slot& s : g_slots)
        if (!_stricmp(s.what.c_str(), what)) return &s;
    return 0;
}

// ---- VP_TRACE_IMPORTS=1: which of race.exe's imports the game calls (relink stage R2a.0) -----------------------------------
// Each import slot gets a counting stub -- `lock inc dword [count]; jmp dword [real]`, no register or stack touched (only
// the flags, which no call passes) -- and ExitProcess writes the counts, appended, to import-trace.txt beside viperport.exe,
// each import marked with what serves it ([stand-in], [Windows], [loader] shim or the [port]'s own).
// (The session recorder patches some slots later; its hooks call on through the stub, so those calls still count.)
struct TraceSlot { volatile LONG count; FARPROC real; char what[64]; uint8_t served; };
TraceSlot* g_trace;
int g_trace_n;
typedef VOID(WINAPI* ExitProcess_t)(UINT);
ExitProcess_t g_trace_exit;
void trace_write() {
    static const char* const how[] = {"[Windows]", "[loader]", "[stand-in]", "[port]"};
    if (FILE* f = fopen((g_self_dir + "import-trace.txt").c_str(), "a")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "== %04d-%02d-%02d %02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCommandLineA());
        for (int i = 0; i < g_trace_n; i++)
            fprintf(f, "%10ld  %-40s %s\n", (long)g_trace[i].count, g_trace[i].what, how[g_trace[i].served & 3]);
        fclose(f);
    }
}
VOID WINAPI trace_ExitProcess(UINT code) {
    trace_write();
    g_trace_exit(code);
}
FARPROC trace_stub(const char* what, FARPROC real, int* index) {
    static uint8_t* code;
    static int used;
    *index = -1;
    if (!g_trace) {
        g_trace = (TraceSlot*)VirtualAlloc(0, 512 * sizeof(TraceSlot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        code = (uint8_t*)VirtualAlloc(0, 512 * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_trace || !code) return real;
    }
    if (g_trace_n >= 512) return real;
    *index = g_trace_n;
    TraceSlot* s = &g_trace[g_trace_n++];
    s->real = real;
    lstrcpynA(s->what, what, sizeof s->what);
    if (!_stricmp(what, "KERNEL32.dll!ExitProcess")) {
        g_trace_exit = (ExitProcess_t)real;
        s->real = (FARPROC)&trace_ExitProcess;
    }
    uint8_t* p = code + 16 * used++;
    p[0] = 0xF0; p[1] = 0xFF; p[2] = 0x05;                         // lock inc dword ptr [&s->count]
    *(uint32_t*)(p + 3) = (uint32_t)(uintptr_t)&s->count;
    p[7] = 0xFF; p[8] = 0x25;                                      // jmp dword ptr [&s->real]
    *(uint32_t*)(p + 9) = (uint32_t)(uintptr_t)&s->real;
    return (FARPROC)p;
}

// what serves a slot from now on (behind its counting stub, if it has one)
void set_slot(Slot& s, FARPROC f, Served served) {
    s.served = served;
    if (s.trace >= 0) {
        TraceSlot* t = &g_trace[s.trace];
        t->served = (uint8_t)served;
        if (!_stricmp(s.what.c_str(), "KERNEL32.dll!ExitProcess")) g_trace_exit = (ExitProcess_t)f;   // (after the counts)
        else t->real = f;
        return;
    }
    DWORD old;
    VirtualProtect(s.at, 4, PAGE_READWRITE, &old);
    *s.at = (uint32_t)(uintptr_t)f;
    VirtualProtect(s.at, 4, old, &old);
}
// what a slot calls now (through its counting stub)
uint32_t slot_target(const Slot& s) {
    return s.trace >= 0 ? (uint32_t)(uintptr_t)g_trace[s.trace].real : *s.at;
}

// every import into its IAT slot; DINPUT.dll's -> dinput_not_yet for now (the slots it names, returned)
int resolve_imports(Image& im, std::vector<uint32_t*>& dinput_slots) {
    const bool trace = GetEnvironmentVariableA("VP_TRACE_IMPORTS", 0, 0) != 0;
    const IMAGE_DATA_DIRECTORY d = im.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress) return 0;
    int n = 0;
    for (const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(im.base + d.VirtualAddress); imp->Name; imp++) {
        const char* dll = (const char*)(im.base + imp->Name);
        const bool dinput = _stricmp(dll, "DINPUT.dll") == 0;
        HMODULE m = dinput ? 0 : LoadLibraryA(dll);
        if (!dinput && !m) return refuse("viperport: race.exe imports %s, which didn't load (%lu)", dll, GetLastError());
        const IMAGE_THUNK_DATA32* names = (const IMAGE_THUNK_DATA32*)(im.base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(im.base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++, n++) {
            Slot s = {(uint32_t*)&iat->u1.Function, dll, "", 0, "", -1, WINDOWS};
            char what[128];
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                s.ordinal = IMAGE_ORDINAL32(names->u1.Ordinal);
                _snprintf(what, sizeof what, "%s #%u", dll, (unsigned)s.ordinal);
            } else {
                s.name = (const char*)((const IMAGE_IMPORT_BY_NAME*)(im.base + names->u1.AddressOfData))->Name;
                _snprintf(what, sizeof what, "%s!%s", dll, s.name.c_str());
            }
            what[sizeof what - 1] = 0;
            s.what = what;
            if (dinput) {
                iat->u1.Function = (DWORD)(uintptr_t)&dinput_not_yet;
                dinput_slots.push_back((uint32_t*)&iat->u1.Function);
                s.served = PORT;
                g_slots.push_back(s);
                continue;
            }
            FARPROC f = s.name.empty() ? GetProcAddress(m, (LPCSTR)(uintptr_t)s.ordinal) : GetProcAddress(m, s.name.c_str());
            if (!f) return refuse("viperport: race.exe's import %s wasn't found", what);
            if (!_stricmp(what, "KERNEL32.dll!GetCommandLineA")) f = (FARPROC)&race_GetCommandLineA, s.served = SHIM;
            if (!_stricmp(what, "KERNEL32.dll!GetModuleHandleA")) f = (FARPROC)&race_GetModuleHandleA, s.served = SHIM;
            if (!_stricmp(what, "TAPI32.dll!lineInitialize")) {
                g_line_initialize = (LineInitialize_t)f;
                f = (FARPROC)&race_lineInitialize;
                s.served = SHIM;
            }
            if (!_stricmp(what, "KERNEL32.dll!GetModuleFileNameA")) {
                f = (FARPROC)&race_GetModuleFileNameA;
                g_gmfn_slot = (void**)&iat->u1.Function;
                s.served = SHIM;
            }
            if (trace) {
                f = trace_stub(what, f, &s.trace);
                if (s.trace >= 0) g_trace[s.trace].served = (uint8_t)s.served;
            }
            iat->u1.Function = (DWORD)(uintptr_t)f;
            g_slots.push_back(s);
        }
    }
    return 0;
}

// ---- the stand-ins (relink stage R2a) -------------------------------------------------------------------------------
// The port DLL exports its table of Win32 stand-ins (hook/w32_table.h: every one in the GCC build, none in MSVC's). Its
// DllMain installs the port at once, and the session recorder and net_wsock take what they find in race.exe's slots as
// the real functions -- so the slots must hold the stand-ins before DllMain runs. Windows tells the loader when it has
// mapped a DLL, before the DLL's own initialisation (LdrRegisterDllNotification, documented in ntdll; Wine has it):
// there the table -- constant data, already relocated -- is read and the slots filled. Three slots keep the loader's
// shims until the DLL is up (GetModuleHandleA / GetModuleFileNameA / GetCommandLineA: the stand-ins answer for race.exe
// only once told who it is, set_process_image, which needs the DLL initialised); then they get the stand-ins too.
// lineInitialize's shim goes at once (TAPI is not available: the stand-in never calls TAPI). An import without a
// stand-in keeps Windows' function and is logged.
const VpStandIns* g_table;                              // the DLL's table (null: none, or an older DLL)
bool g_filled_early;                                    // filled from the notification, before the DLL's DllMain
int g_filled;                                           // slots the table filled
std::string g_table_unmatched;                          // table entries no slot imports (names)
const char* const IDENTITY[] = {"KERNEL32.dll!GetModuleHandleA", "KERNEL32.dll!GetModuleFileNameA", "KERNEL32.dll!GetCommandLineA"};

bool is_identity(const Slot& s) {
    for (const char* w : IDENTITY)
        if (!_stricmp(s.what.c_str(), w)) return true;
    return false;
}
const VpStandIn* entry_for(const Slot& s) {
    if (!g_table) return 0;
    for (uint32_t i = 0; i < g_table->count; i++) {
        const VpStandIn& e = g_table->entries[i];
        if (!e.dll || !e.fn || _stricmp(e.dll, s.dll.c_str())) continue;
        if (e.name ? !s.name.empty() && s.name == e.name : s.name.empty() && e.ordinal == s.ordinal) return &e;
    }
    return 0;
}

// an export of a mapped image, found without GetProcAddress (the notification comes under the loader lock)
void* export_of(uint8_t* base, const char* name) {
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress) return 0;
    const IMAGE_EXPORT_DIRECTORY* e = (const IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
    const DWORD* names = (const DWORD*)(base + e->AddressOfNames);
    const WORD* ords = (const WORD*)(base + e->AddressOfNameOrdinals);
    const DWORD* funcs = (const DWORD*)(base + e->AddressOfFunctions);
    for (DWORD i = 0; i < e->NumberOfNames; i++)
        if (!strcmp((const char*)(base + names[i]), name)) return base + funcs[ords[i]];
    return 0;
}

// every slot the table has a stand-in for (the identity three: later, fill_identity)
void fill_from(const VpStandIns* t) {
    if (!t || t->size < sizeof(VpStandIns) || t->version != VP_STAND_INS_VERSION) return;
    g_table = t;
    std::vector<bool> used(t->count);
    for (Slot& s : g_slots) {
        if (s.dll == "DINPUT.dll") continue;
        const VpStandIn* e = entry_for(s);
        if (!e) continue;
        used[e - t->entries] = true;
        if (is_identity(s)) continue;
        set_slot(s, (FARPROC)e->fn, STAND_IN);
        g_filled++;
    }
    for (uint32_t i = 0; i < t->count; i++) {
        const VpStandIn& e = t->entries[i];
        if (!used[i]) {
            char one[96];
            if (e.name) _snprintf(one, sizeof one, "%s!%s", e.dll, e.name);
            else _snprintf(one, sizeof one, "%s #%u", e.dll, e.ordinal);
            one[sizeof one - 1] = 0;
            g_table_unmatched += (g_table_unmatched.empty() ? "" : ", ") + std::string(one);
        }
    }
}

// the identity three, once the stand-ins know race.exe (the DLL is up)
void fill_identity(const std::string& exe, const std::string& cmdline) {
    if (!g_table || !g_table->count || !g_table->set_process_image) return;
    g_table->set_process_image(BASE, exe.c_str(), cmdline.c_str());
    for (Slot& s : g_slots)
        if (is_identity(s))
            if (const VpStandIn* e = entry_for(s)) {
                set_slot(s, (FARPROC)e->fn, STAND_IN);
                g_filled++;
            }
}

// LdrRegisterDllNotification's types (winternl.h has them only in newer SDKs)
struct LdrString { USHORT Length, MaximumLength; PWSTR Buffer; };
struct LdrLoaded { ULONG Flags; const LdrString* FullDllName; const LdrString* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID(CALLBACK* LdrNotify_t)(ULONG reason, const LdrLoaded* data, PVOID context);
typedef LONG(NTAPI* LdrRegister_t)(ULONG flags, LdrNotify_t fn, PVOID context, PVOID* cookie);
typedef LONG(NTAPI* LdrUnregister_t)(PVOID cookie);
void* g_notify_cookie;
VOID CALLBACK on_dll_loaded(ULONG reason, const LdrLoaded* data, PVOID) {
    if (reason != 1 || g_table || !data || !data->DllBase) return;     // LDR_DLL_NOTIFICATION_REASON_LOADED
    if (const void* t = export_of((uint8_t*)data->DllBase, VP_STAND_INS_EXPORT)) {
        fill_from((const VpStandIns*)t);
        g_filled_early = g_table != 0;
    }
}
void watch_dll_loads(bool on) {
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (on) {
        if (LdrRegister_t reg = (LdrRegister_t)GetProcAddress(nt, "LdrRegisterDllNotification"))
            if (reg(0, on_dll_loaded, 0, &g_notify_cookie) != 0) g_notify_cookie = 0;
    } else if (g_notify_cookie) {
        if (LdrUnregister_t un = (LdrUnregister_t)GetProcAddress(nt, "LdrUnregisterDllNotification")) un(g_notify_cookie);
        g_notify_cookie = 0;
    }
}

// what serves each slot once the port is installed: a slot left Windows' that the DLL itself took over since (DDRAW
// and DSOUND: the renderer's and the audio's emulations) is the port's. Returns how many still reach Windows (their
// names in *list).
int still_windows(HMODULE port, std::string* list) {
    uint32_t lo = (uint32_t)(uintptr_t)port, hi = lo;
    if (port) {
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((uint8_t*)port + ((const IMAGE_DOS_HEADER*)port)->e_lfanew);
        hi = lo + nt->OptionalHeader.SizeOfImage;
    }
    int n = 0;
    for (Slot& s : g_slots) {
        if (s.served != WINDOWS) continue;
        const uint32_t to = slot_target(s), now = *s.at;     // (the DLL may have put its own over a counting stub)
        if ((to >= lo && to < hi) || (now >= lo && now < hi)) {
            s.served = PORT;
            if (s.trace >= 0) g_trace[s.trace].served = PORT;
            continue;
        }
        *list += (n++ ? ", " : "") + s.what;
    }
    return n;
}

// the stand-ins' report: --check prints it (a FAIL if the table came too late), a launch writes it to viperport.log
int report_stand_ins(HMODULE port) {
    std::string windows;
    const int nw = still_windows(port, &windows);
    int shims = 0;
    for (const Slot& s : g_slots) shims += s.served == SHIM;
    char line[512];
    if (!g_table || !g_table->count) {
        if (g_check)
            say("viperport.exe: stand-ins: none (this DLL's table is %s): race.exe's %u imports stay Windows' (%d) and the "
                "port's own (%u), %d loader shims", g_table ? "empty -- the MSVC build" : "missing", (unsigned)g_slots.size(),
                nw, (unsigned)g_slots.size() - nw - shims, shims);
        return 0;
    }
    _snprintf(line, sizeof line, "stand-ins: %d of race.exe's %u imports filled from the DLL's table of %u (%s the port "
              "installed); %d still reach Windows; %d loader shims", g_filled, (unsigned)g_slots.size(),
              (unsigned)g_table->count, g_filled_early ? "before" : "AFTER", nw, shims);
    line[sizeof line - 1] = 0;
    std::vector<std::string> lines;
    lines.push_back(line);
    if (nw) lines.push_back("stand-ins: still Windows' (no stand-in yet): " + windows);
    if (!g_table_unmatched.empty()) lines.push_back("stand-ins: table entries race.exe doesn't import: " + g_table_unmatched);
    for (const std::string& l : lines) {
        if (g_check) say("viperport.exe: %s", l.c_str());
        else if (g_table->log) g_table->log(("viperport.exe: " + l).c_str());
    }
    if (g_check && !g_filled_early) {
        say("FAIL: the stand-ins were filled after the port installed (no DLL load notification)");
        return 1;
    }
    return 0;
}

// The PEB's ImageBaseAddress is what GetModuleHandle(NULL) returns to every caller that doesn't go through race.exe's
// own import slots. It points at race.exe's image only while the port DLL loads: its DllMain identifies the build and
// finds race.exe's imports from GetModuleHandle(NULL), exactly as on the dinput.dll route. Then it goes back to
// viperport.exe, a module Windows loaded -- SDL and DirectInput 8 (joysticks, haptics) initialise with it, and
// DirectInput refuses an image Windows never loaded ("Haptic error Initializing DirectInput device"). The game itself
// gets 0x400000 from its GetModuleHandleA slot (race_GetModuleHandleA, below).
void* set_process_module(void* base) {
    uint8_t* peb = (uint8_t*)(uintptr_t)__readfsdword(0x30);
    void* was = *(void**)(peb + 8);
    *(void**)(peb + 8) = base;
    return was;
}

// ---- --probe -------------------------------------------------------------------------------------------------------
// "Will the standalone run this race.exe?", for a launcher (vrmod's Play), in a fraction of a second: what a launch
// would refuse, found the cheap way. race.exe is read and its headers checked as a launch does, then mapped at 0x400000
// (nothing resolved or written); dinput.dll is loaded with VP_PROBE_ENV set, so its DllMain does nothing (no log, no
// install), and its viperport_probe export runs the same stock check the install would (port_check_stock). SDL2.dll is
// loaded the way the DLL will load it, not started. No window, no SDL, no fill. Prints one line and ends the process:
//   0  yes: ...                                    the standalone runs it
//   2  no: race.exe not found ...                  (or unreadable)
//   3  no: ... isn't v1.0 race.exe ...             (a race.bin build, another program)
//   4  no: dinput.dll not found / didn't load ...
//   5  no: dinput.dll isn't the port's ...         (another dinput.dll, an older port build, a different build)
//   6  no: SDL2.dll not found ...
//   7  no: race.exe has N functions patched in a way the port doesn't know: <name> (<address>), ...
//   8  no: ...                                     anything else (race.exe couldn't be mapped)
int probe(const std::string& exe, const std::string& dll, const std::string& self_dir) {
    int code = 0;
    std::string why;
    auto no = [&](int c, const std::string& w) { code = c; why = w; };
    Image im;
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        no(2, "race.exe not found: " + exe);
    } else {
        HANDLE f = CreateFileA(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        DWORD got = 0;
        bool ok = false;
        if (f != INVALID_HANDLE_VALUE) {
            const DWORD n = GetFileSize(f, 0);
            if (n != INVALID_FILE_SIZE) {
                im.file.resize(n);
                ok = ReadFile(f, im.file.data(), n, &got, 0) && got == n;
            }
            CloseHandle(f);
        }
        if (!ok) no(2, "race.exe couldn't be read: " + exe);
    }
    if (!code) {
        im.nt = headers_of(im.file);
        if (!im.nt || im.nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || im.nt->FileHeader.TimeDateStamp != V10_TIMESTAMP ||
            im.nt->OptionalHeader.ImageBase != BASE || im.nt->OptionalHeader.SizeOfImage != V10_IMAGE ||
            im.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
            no(3, exe + " isn't v1.0 race.exe (the standalone runs v1.0 only; the race.bin builds keep the dinput.dll route)");
    }
    if (!code && GetFileAttributesA(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
        no(4, "dinput.dll not found: " + dll + " (the port's dinput.dll goes beside viperport.exe)");
    if (!code) {
        if (_stricmp(folder_of(dll).c_str(), self_dir.c_str()) != 0) SetDllDirectoryA(folder_of(dll).c_str());   // as a launch
        HMODULE sdl = LoadLibraryA("SDL2.dll");          // (only loaded: nothing of SDL starts until SDL_Init)
        if (!sdl || !GetProcAddress(sdl, "SDL_Init"))
            no(6, "SDL2.dll not found beside viperport.exe or dinput.dll (" + folder_of(dll) + ")");
    }
    if (!code && map_image(im, exe)) no(8, g_probe_why.empty() ? "race.exe couldn't be mapped" : g_probe_why);
    if (!code) {
        char v[8];
        sprintf(v, "%d", VP_STANDALONE_VERSION);
        SetEnvironmentVariableA(VP_PROBE_ENV, v);
        HMODULE port = LoadLibraryA(dll.c_str());
        const DWORD err = GetLastError();
        SetEnvironmentVariableA(VP_PROBE_ENV, 0);
        char line[1024] = "";
        if (!port) {
            sprintf(line, "dinput.dll didn't load (error %lu): ", err);
            no(4, line + dll);
        } else if (VpProbe_t pr = (VpProbe_t)GetProcAddress(port, VP_PROBE_EXPORT)) {
            const int r = pr(VP_STANDALONE_VERSION, line, sizeof line);
            if (r == -2) no(5, std::string("dinput.dll isn't the port's build that goes with this viperport.exe (") + line + ")");
            else if (r == -1) no(3, exe + " isn't v1.0 race.exe (the port DLL says so)");
            else if (r > 0) no(7, line);
            else why = line;
        } else if (GetProcAddress(port, VP_STANDALONE_EXPORT)) {
            no(5, "dinput.dll is an older build of the port (it has no --probe entry): " + dll);
        } else {
            no(5, "dinput.dll isn't the port's (another dinput.dll): " + dll);
        }
    }
    probe_line((code ? "no: " : "yes: ") + why);
    // the DLL (if loaded) did nothing in this process; end it without running anything else
    ExitProcess((UINT)code);
    return code;
}

int run(int argc_unused) {
    (void)argc_unused;
    // reserve race.exe's range first, before this program allocates anything more
    void* reserved = VirtualAlloc((void*)(uintptr_t)BASE, V10_IMAGE, MEM_RESERVE, PAGE_NOACCESS);
#ifdef VP_TEST_RELAUNCH                                 // (a test build: take the fresh-process route every time)
    if (reserved && !GetEnvironmentVariableA(CHILD_ENV, 0, 0)) { VirtualFree(reserved, 0, MEM_RELEASE); reserved = 0; }
#endif
    const bool child = GetEnvironmentVariableA(CHILD_ENV, 0, 0) != 0;
    if (child) SetEnvironmentVariableA(CHILD_ENV, 0);

    char selfp[MAX_PATH];
    GetModuleFileNameA(0, selfp, MAX_PATH);
    const std::string self_dir = folder_of(selfp);
    std::vector<Arg> args = args_of(GetCommandLineA());
    std::string exe = self_dir + "race.exe", dll = self_dir + "dinput.dll";
    g_self_dir = self_dir;
    g_check = !args.empty() && args[0].text == "--check";
    g_probe = !args.empty() && args[0].text == "--probe";
    if (!reserved && !child) return relaunch_with_range_reserved();   // (the child reports and runs)
    // the loader's own options come out of the game's command line: --race <file> (any mode), --dll <file>, and
    // --check / --probe [race.exe]; every other argument is the game's, as written
    std::string game_args;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& t = args[i].text;
        if ((t == "--race" || t == "--dll") && i + 1 < args.size()) {
            (t == "--race" ? exe : dll) = args[++i].text;
        } else if ((g_check || g_probe) && i == 0) {
        } else if ((g_check || g_probe) && t.size() && t[0] != '-') {
            exe = t;                                         // --check <race.exe>, --probe <race.exe>
        } else {
            game_args += " " + args[i].raw;
        }
    }
    if (g_probe) return probe(resolve_path(exe, self_dir), resolve_path(dll, self_dir), self_dir);
    if (g_check) {
        g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!g_stdout || g_stdout == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS))
                g_stdout = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        }
        g_report = fopen((self_dir + "viperport-check.txt").c_str(), "w");
    }
    exe = resolve_path(exe, self_dir);
    dll = resolve_path(dll, self_dir);
    g_race_path = exe;
    g_race_cmdline = "\"" + exe + "\"" + game_args;   // what the game's GetCommandLineA returns
    mirror_dpi_compat(exe);                           // before anything makes a window
    // (in a child the parent reserved the range for us: map_image releases it and maps there)

    // race.exe: v1.0 only
    Image im;
    {
        HANDLE f = CreateFileA(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (f == INVALID_HANDLE_VALUE) return refuse("viperport: %s doesn't open (%lu): viperport.exe goes beside race.exe", exe.c_str(), GetLastError());
        const DWORD n = GetFileSize(f, 0);
        im.file.resize(n);
        DWORD got = 0;
        const BOOL ok = ReadFile(f, im.file.data(), n, &got, 0);
        CloseHandle(f);
        if (!ok || got != n) return refuse("viperport: %s couldn't be read", exe.c_str());
    }
    im.nt = headers_of(im.file);
    if (!im.nt || im.nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
        return refuse("viperport: %s isn't a 32-bit Windows program", exe.c_str());
    if (im.nt->FileHeader.TimeDateStamp != V10_TIMESTAMP || im.nt->OptionalHeader.ImageBase != BASE ||
        im.nt->OptionalHeader.SizeOfImage != V10_IMAGE)
        return refuse("viperport: %s isn't v1.0 race.exe (timestamp %08lx): the standalone runs v1.0 only -- the race.bin "
                      "builds keep the dinput.dll route", exe.c_str(), im.nt->FileHeader.TimeDateStamp);
    const std::string hash = sha256_hex(im.file);
    const bool stock = hash == V10_SHA256;
    say("viperport.exe: %s: v1.0 (timestamp %08x), SHA-256 %s%s", exe.c_str(), V10_TIMESTAMP, hash.c_str(),
        stock ? " -- the stock file" : " -- not the stock file (the DLL checks each function it replaces)");
    if (im.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
        return refuse("viperport: %s has TLS, which v1.0 doesn't", exe.c_str());

    // map, import, protect
    if (int r = map_image(im, exe)) return r;
    std::vector<uint32_t*> dinput_slots;
    if (int r = resolve_imports(im, dinput_slots)) return r;


    // the port DLL, by path, told it runs standalone
    char v[8];
    sprintf(v, "%d", VP_STANDALONE_VERSION);
    SetEnvironmentVariableA(VP_STANDALONE_ENV, v);
    if (_stricmp(folder_of(dll).c_str(), self_dir.c_str()) != 0)
        SetDllDirectoryA(folder_of(dll).c_str());       // --dll elsewhere: SDL2.dll beside it (else the app folder has it)
    void* own = set_process_module((void*)(uintptr_t)BASE);   // (see set_process_module)
    if (!g_probe) watch_dll_loads(true);               // (the stand-ins go in as Windows maps the DLL: see fill_from)
    HMODULE port = LoadLibraryA(dll.c_str());
    watch_dll_loads(false);
    set_process_module(own);
    SetEnvironmentVariableA(VP_STANDALONE_ENV, 0);
    if (!port) return refuse("viperport: the port DLL %s didn't load (%lu)", dll.c_str(), GetLastError());
    VpStandalone_t entry = (VpStandalone_t)GetProcAddress(port, VP_STANDALONE_EXPORT);
    FARPROC create = GetProcAddress(port, "DirectInputCreateA");
    if (!entry || !create)
        return refuse("viperport: %s isn't a port DLL with the standalone entry (an older build?)", dll.c_str());
    for (uint32_t* slot : dinput_slots) {
        DWORD old;
        VirtualProtect(slot, 4, PAGE_READWRITE, &old);
        if (*slot == (uint32_t)(uintptr_t)&dinput_not_yet) *slot = (uint32_t)(uintptr_t)create;   // (unless the DLL took it)
        VirtualProtect(slot, 4, old, &old);
    }
    if (!g_table)                                       // (no load notification: filled now, after the install -- a FAIL)
        if (const void* t = (const void*)GetProcAddress(port, VP_STAND_INS_EXPORT)) fill_from((const VpStandIns*)t);
    fill_identity(exe, g_race_cmdline);
    int loader_fails = report_stand_ins(port);

    if (g_check) {
        // what the game will see of its module, through its own import slots (as it calls them)
        char name[MAX_PATH] = "";
        const Slot* gmh = slot_named("KERNEL32.dll!GetModuleHandleA");
        const Slot* gcl = slot_named("KERNEL32.dll!GetCommandLineA");
        const DWORD nn = g_gmfn_slot ? ((DWORD(WINAPI*)(HMODULE, LPSTR, DWORD))*g_gmfn_slot)(0, name, MAX_PATH) : 0;
        const HMODULE mh = gmh ? ((HMODULE(WINAPI*)(LPCSTR))(uintptr_t)*gmh->at)(0) : 0;
        const char* cl = gcl ? ((LPSTR(WINAPI*)())(uintptr_t)*gcl->at)() : "(none)";
        say("viperport.exe: the game's GetModuleHandleA(NULL) = %p, GetModuleFileNameA(NULL) = \"%s\"; the process's "
            "GetModuleHandle(NULL) = %p (viperport.exe)", (void*)mh, nn ? name : "(fails)", (void*)GetModuleHandleA(0));
        if (!nn || _stricmp(name, exe.c_str()) != 0) { say("FAIL: the game's module file name isn't race.exe's"); loader_fails++; }
        if (mh != (HMODULE)(uintptr_t)BASE) { say("FAIL: the game's GetModuleHandleA(NULL) isn't race.exe's image"); loader_fails++; }
        if (GetModuleHandleA(0) == (HMODULE)(uintptr_t)BASE) { say("FAIL: the process's module is still race.exe's image"); loader_fails++; }
        say("viperport.exe: the game's command line (its GetCommandLineA) = %s", cl);
        if (strcmp(cl, g_race_cmdline.c_str()) != 0) { say("FAIL: the game's command line isn't race.exe's"); loader_fails++; }
        // its resources and its window class, on its hInstance 0x400000 (an image Windows didn't load)
        const HINSTANCE inst = (HINSTANCE)(uintptr_t)BASE;
        HRSRC icon = FindResourceA(inst, MAKEINTRESOURCEA(1), (LPCSTR)RT_GROUP_ICON);
        HRSRC splash = FindResourceA(inst, "SPLASH", (LPCSTR)RT_BITMAP);
        HICON li = LoadIconA(inst, MAKEINTRESOURCEA(1));
        HBITMAP lb = LoadBitmapA(inst, "SPLASH");
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = inst;
        wc.lpszClassName = "viperport check window";
        const ATOM cls = RegisterClassA(&wc);
        HWND w = cls ? CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP, 0, 0, 8, 8, 0, 0, inst, 0) : 0;   // never shown
        say("viperport.exe: on hInstance 0x400000: icon group 1 %s, SPLASH %s; LoadIcon %s, LoadBitmap %s, RegisterClass %s, "
            "CreateWindowEx %s", icon ? "found" : "MISSING", splash ? "found" : "MISSING", li ? "ok" : "FAILS",
            lb ? "ok" : "FAILS", cls ? "ok" : "FAILS", w ? "ok" : "FAILS");
        if (!icon || !splash || !li || !lb || !cls || !w) { say("FAIL: race.exe's resources or window class on 0x400000"); loader_fails++; }
        if (w) DestroyWindow(w);
        if (cls) UnregisterClassA(wc.lpszClassName, inst);
        if (lb) DeleteObject(lb);
        // TAPI on 0x400000 straight, and through the game's slot (race_lineInitialize, or the stand-in: TAPI with no
        // modem): the second must start it
        const Slot* li_slot = slot_named("TAPI32.dll!lineInitialize");
        if (g_line_initialize && li_slot) {
            struct Cb { static void CALLBACK f(DWORD, DWORD, DWORD_PTR, DWORD_PTR, DWORD_PTR, DWORD_PTR) {} };
            typedef LONG(WINAPI* LineShutdown_t)(DWORD);
            const LineShutdown_t shut = (LineShutdown_t)GetProcAddress(GetModuleHandleA("TAPI32.dll"), "lineShutdown");
            DWORD app = 0, devs = 0;
            const LONG raw = g_line_initialize(&app, inst, (void*)&Cb::f, "viperport check", &devs);
            if (raw == 0 && shut) shut(app);
            app = 0;
            const LineInitialize_t game = (LineInitialize_t)(uintptr_t)*li_slot->at;
            const LONG via = game(&app, inst, (void*)&Cb::f, "viperport check", &devs);
            typedef LONG(WINAPI* GameShutdown_t)(DWORD);
            const Slot* sd = slot_named("TAPI32.dll!lineShutdown");
            if (via == 0 && sd) ((GameShutdown_t)(uintptr_t)*sd->at)(app);
            say("viperport.exe: TAPI lineInitialize on hInstance 0x400000: %s (%08lx); through the game's slot: %s (%lu "
                "devices)", raw ? "refused" : "ok", (unsigned long)raw, via ? "FAILS" : "ok", (unsigned long)devs);
            if (via) { say("FAIL: TAPI doesn't start for the game (the modem line)"); loader_fails++; }
        }
    }

    VpStandaloneArgs a = {};
    a.size = sizeof a;
    a.version = VP_STANDALONE_VERSION;
    a.mode = g_check ? VP_SA_CHECK : VP_SA_RUN;
    a.exe_path = exe.c_str();
    a.file = im.file.data();
    a.file_size = (uint32_t)im.file.size();
    a.stock = stock;
    a.out = out;
    const int r = entry(&a);
    if (g_check) {
        say("viperport.exe --check: %s", r || loader_fails ? "FAILED" : "passed");
        if (g_report) fclose(g_report);
        // the DLL is installed in this process: end it without its exit logs touching anything else
        ExitProcess(r || loader_fails ? 1 : 0);
    }
    return refuse("viperport: the game didn't start (%d checks failed): see viperport.log beside dinput.dll", r);
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return run(0); }
