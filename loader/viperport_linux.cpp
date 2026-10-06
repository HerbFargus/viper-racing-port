// viperport (Linux) -- runs the v1.0 game on the port's code alone, natively (relink stage R2b): the ELF counterpart
// of loader/viperport_main.cpp (viperport.exe), linked with the whole port into one executable (tools/build_linux.sh:
// out-linux/viperport).
//
//   viperport [--race file] [game arguments]      the game: race.exe from the current folder, else beside viperport
//   viperport --check [race.exe]                  map, install, fill and verify; print the report; start nothing
//   viperport --probe [race.exe]                  "will the standalone run this race.exe?" -- exit code 0 = yes, else
//                                                 no; one line ("yes: ..." / "no: ...") on stdout and in
//                                                 viperport-probe.txt beside viperport. No window, no SDL, no fill.
// --race names the v1.0 race.exe to run (e.g. vrmod's stock copy, race.exe.vrmod-original); a relative path is taken
// from the current folder, else from viperport's.
//
// What viperport.exe does on Windows, done here without Windows:
//   * race.exe (v1.0 only: its PE timestamp, image base and size; no TLS; the SHA-256 says whether it is the stock
//     file) is read and mapped at 0x400000 -- each section at its RVA, the headers too, the rest of the image zero,
//     then each section's own protection (MEM_IMAGE in the OS layer's region table: hook/vp_os_linux.h). The range is
//     reserved (MAP_FIXED_NOREPLACE) first thing in main: this executable is a non-PIE ELF linked at 0x08048000, its
//     heap after it and shared objects near the top, so nothing of the process is there.
//   * Its imports are filled from the port's stand-ins (viperport_stand_ins, linked in: hook/w32_table.h) -- all of
//     them before the port installs, the identity three (GetModuleHandleA / GetModuleFileNameA / GetCommandLineA)
//     included: the stand-ins are told who the game is first (w32_set_process_image). DDRAW and DSOUND are the port's
//     own (the renderer's and the audio's install takes their slots); until then, and for any import without a
//     stand-in, a trap that names the import and ends the process. DINPUT's DirectInputCreateA: there is no
//     DirectInput -- E_FAIL (the SDL platform reads the input).
//   * The install folder (race.exe's) is drive C:'s root for the path layer (w32_path.h) and the process's current
//     folder; the game is C:\<race.exe's name> with its arguments after it. The port's own module is "C:\viperport"
//     (vp_linux_set_self): viperport.ini and viperport.log beside race.exe, as with viperport.exe on Windows.
//   * The port installs as the DLL's DllMain does on Windows (DLL_PROCESS_ATTACH, the standalone always: there is no
//     dinput.dll route on Linux); DLL_PROCESS_DETACH at exit (atexit: ExitProcess is exit(), the exit logs).
//   * The main thread: its fs: block (the TIB the game's SEH frames chain through: hook/w32_seh.cpp) and the x87
//     control word Windows starts a process with, 0x27f. Then standalone.cpp's run (viperport_standalone): the fill,
//     the audit, the int3 handler, and race.exe's entry point -- the rewritten _WinMainCRTStartup.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>                                           // hook/linux_inc -> win32_compat.h (the PE structures)
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "../hook/standalone.h"
#include "../hook/vp_os.h"
#include "../hook/vp_os_linux.h"
#include "../hook/w32_kernel.h"
#include "../hook/w32_path.h"
#include "../hook/w32_table.h"

// the port's side (hook/viperport.cpp, hook/standalone.cpp)
void logf(const char* fmt, ...);
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID);
extern "C" int __cdecl viperport_standalone(const VpStandaloneArgs* args);
extern "C" int __cdecl viperport_probe(uint32_t version, char* line, uint32_t n);
// the main thread's fs: block (agent C's, hook/w32_seh.cpp; weak: the loader links before it is written)
#include "../hook/w32_seh.h"
void w32_seh_thread_begin() __attribute__((weak));

namespace {

const uint32_t BASE = 0x400000;
const uint32_t V10_TIMESTAMP = 0x362de68c;
const uint32_t V10_IMAGE = 0x22c000;                   // v1.0's SizeOfImage
// SHA-256 of the stock v1.0 race.exe (2,404,451 bytes; the disc's Data\race.exe)
const char* const V10_SHA256 = "739619fd213c1e8c234b4712b2bae2a8b362d19f94f6be08c477a42e39d6dd81";

bool g_check;
bool g_probe;
std::string g_probe_why;
FILE* g_report;
std::string g_self_dir;                                 // viperport's folder (host path, with a trailing '/')

void __cdecl out(const char* line) {
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
    if (g_report) { fprintf(g_report, "%s\n", line); fflush(g_report); }
}

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    out(line);
}

// the one line a probe, or a launch that refuses, leaves: on stdout and in viperport-probe.txt beside viperport
void probe_line(const std::string& line) {
    fprintf(stdout, "%s\n", line.c_str());
    fflush(stdout);
    if (FILE* f = fopen((g_self_dir + "viperport-probe.txt").c_str(), "w")) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }
}

// a failure before the game starts: printed (--check); kept for the one line (--probe); or, at a launch, the process
// ends at once with exit code 2, the reason on stderr and as a "no: ..." line (stdout, viperport-probe.txt)
int refuse(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int refuse(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    const char* why = !strncmp(line, "viperport: ", 11) ? line + 11 : line;
    if (g_probe) {
        if (g_probe_why.empty()) g_probe_why = why;
        return 2;
    }
    if (g_check) {
        out(line);
        return 2;
    }
    fprintf(stderr, "%s\n", line);
    probe_line(std::string("no: ") + why);
    return 2;
}

std::string folder_of(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? std::string("./") : path.substr(0, s + 1);
}
std::string name_of(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}
bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
std::string absolute(const std::string& p) {
    char buf[4096];
    if (realpath(p.c_str(), buf)) return buf;
    if (!p.empty() && p[0] == '/') return p;
    return getcwd(buf, sizeof buf) ? std::string(buf) + "/" + p : p;
}
// a relative path: from the current folder if the file is there, else from viperport's folder
std::string resolve_path(const std::string& p) {
    if (exists(p) || (!p.empty() && p[0] == '/')) return absolute(p);
    return exists(g_self_dir + p) ? absolute(g_self_dir + p) : absolute(p);
}

// ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------------------------------
std::string sha256_hex(const std::vector<uint8_t>& data) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m(data);
    const uint64_t bits = (uint64_t)data.size() * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (i * 8)));
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 |
                   m[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string hex;
    char two[3];
    for (uint32_t v : h)
        for (int i = 3; i >= 0; i--) { snprintf(two, sizeof two, "%02x", (v >> (i * 8)) & 0xff); hex += two; }
    return hex;
}

// ---- 0x400000 ------------------------------------------------------------------------------------------------------
bool g_reserved;
void reserve_range() {
    void* p = mmap((void*)(uintptr_t)BASE, V10_IMAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p == (void*)(uintptr_t)BASE) g_reserved = true;
    else if (p != MAP_FAILED) munmap(p, V10_IMAGE);             // (a kernel before 4.17 takes the flag as a hint)
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

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + got);
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// Windows' protection for a section's characteristics
DWORD section_prot(uint32_t c) {
    const bool x = c & IMAGE_SCN_MEM_EXECUTE, r = c & IMAGE_SCN_MEM_READ, w = c & IMAGE_SCN_MEM_WRITE;
    if (x) return w ? PAGE_EXECUTE_WRITECOPY : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_WRITECOPY;
    return r ? PAGE_READONLY : PAGE_NOACCESS;
}

// race.exe at its own base, as Windows maps an exe: the headers, each section at its RVA, the rest zero; then each
// section's protection (the headers read-only)
int map_image(Image& im, const std::string& path) {
    if (!g_reserved) {
        reserve_range();
        if (!g_reserved) return refuse("viperport: 0x400000 is taken in this process: race.exe can't be mapped there");
    }
    if (mprotect((void*)(uintptr_t)BASE, V10_IMAGE, PROT_READ | PROT_WRITE) != 0)
        return refuse("viperport: couldn't map race.exe's image at 0x400000 (%s)", strerror(errno));
    im.base = (uint8_t*)(uintptr_t)BASE;
    const uint32_t hdrs = im.nt->OptionalHeader.SizeOfHeaders;
    if (hdrs > im.file.size() || hdrs > V10_IMAGE) return refuse("viperport: %s's headers are damaged", path.c_str());
    memcpy(im.base, im.file.data(), hdrs);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(im.nt);
    const int n = im.nt->FileHeader.NumberOfSections;
    for (int i = 0; i < n; i++) {
        const uint32_t raw = s[i].SizeOfRawData < s[i].Misc.VirtualSize || !s[i].Misc.VirtualSize ? s[i].SizeOfRawData
                                                                                                   : s[i].Misc.VirtualSize;
        if (s[i].PointerToRawData + (uint64_t)raw > im.file.size() || s[i].VirtualAddress + (uint64_t)raw > V10_IMAGE)
            return refuse("viperport: race.exe's section %.8s isn't what the file holds once mapped", (const char*)s[i].Name);
        memcpy(im.base + s[i].VirtualAddress, &im.file[s[i].PointerToRawData], raw);
    }
    // the view holds exactly the file's sections (what the audit compares against)
    for (int i = 0; i < n; i++) {
        const uint32_t raw = s[i].SizeOfRawData < s[i].Misc.VirtualSize || !s[i].Misc.VirtualSize ? s[i].SizeOfRawData
                                                                                                   : s[i].Misc.VirtualSize;
        if (memcmp(im.base + s[i].VirtualAddress, &im.file[s[i].PointerToRawData], raw))
            return refuse("viperport: race.exe's section %.8s isn't what the file holds once mapped", (const char*)s[i].Name);
    }
    vp_linux_note_region(BASE, V10_IMAGE, MEM_IMAGE, PAGE_EXECUTE_WRITECOPY);
    DWORD old;
    vpos_VirtualProtect(im.base, hdrs, PAGE_READONLY, &old);
    for (int i = 0; i < n; i++) {
        const uint32_t size = s[i].Misc.VirtualSize ? s[i].Misc.VirtualSize : s[i].SizeOfRawData;
        if (size && !vpos_VirtualProtect(im.base + s[i].VirtualAddress, size, section_prot(s[i].Characteristics), &old))
            return refuse("viperport: race.exe's section %.8s couldn't be given its protection", (const char*)s[i].Name);
    }
    return 0;
}

// ---- race.exe's import slots ------------------------------------------------------------------------------------------
enum Served { NONE, SHIM, STAND_IN, PORT };             // no stand-in (a trap); the loader's; a stand-in; the port's own
struct Slot {
    uint32_t* at;
    std::string dll, name;                              // name empty: by ordinal
    uint32_t ordinal;
    std::string what;                                   // "KERNEL32.dll!CreateFileA", "WSOCK32.dll #23"
    Served served;
};
std::vector<Slot> g_slots;
Slot* slot_named(const char* what) {
    for (Slot& s : g_slots)
        if (!strcasecmp(s.what.c_str(), what)) return &s;
    return 0;
}

// an import nothing serves: its slot calls a trap that names it and ends the process (on Windows it would be Windows'
// own function; here there is none). Each trap is `push index; call no_stand_in`.
uint8_t* g_traps;
uint32_t g_trap_lo, g_trap_hi;
extern "C" void __cdecl no_stand_in(uint32_t index) {
    const char* what = index < g_slots.size() ? g_slots[index].what.c_str() : "?";
    fprintf(stderr, "viperport: race.exe called %s, which has no stand-in on Linux -- the game ends here\n", what);
    logf("loader: race.exe called %s, which has no stand-in on Linux -- the game ends here", what);
    fflush(stderr);
    _exit(3);
}
uint32_t trap_for(uint32_t index) {
    if (!g_traps) {
        g_traps = (uint8_t*)vpos_VirtualAlloc(0, 512 * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_traps) return 0;
        g_trap_lo = (uint32_t)(uintptr_t)g_traps;
        g_trap_hi = g_trap_lo + 512 * 16;
    }
    if (index >= 512) return 0;
    uint8_t* p = g_traps + 16 * index;
    p[0] = 0x68;                                        // push index
    memcpy(p + 1, &index, 4);
    p[5] = 0xE8;                                        // call no_stand_in
    const int32_t rel = (int32_t)((uintptr_t)&no_stand_in - ((uintptr_t)p + 10));
    memcpy(p + 6, &rel, 4);
    return (uint32_t)(uintptr_t)p;
}
bool is_trap(uint32_t a) { return a >= g_trap_lo && a < g_trap_hi; }

// race.exe's DINPUT import: there is no DirectInput (the SDL platform reads the input)
HRESULT WINAPI no_dinput(HINSTANCE, DWORD, void**, void*) { return (HRESULT)0x80004005L; }   // E_FAIL

const VpStandIn* entry_for(const Slot& s) {
    const VpStandIns& t = viperport_stand_ins;
    for (uint32_t i = 0; i < t.count; i++) {
        const VpStandIn& e = t.entries[i];
        if (!e.dll || !e.fn || strcasecmp(e.dll, s.dll.c_str())) continue;
        if (e.name ? !s.name.empty() && s.name == e.name : s.name.empty() && e.ordinal == s.ordinal) return &e;
    }
    return 0;
}

void set_slot(Slot& s, uint32_t f, Served served) {
    s.served = served;
    DWORD old;
    vpos_VirtualProtect(s.at, 4, PAGE_READWRITE, &old);
    *s.at = f;
    vpos_VirtualProtect(s.at, 4, old, &old);
}

int g_filled;
std::string g_table_unmatched;

// every import into its IAT slot: the stand-in, else (DINPUT) no_dinput, else a trap
int resolve_imports(Image& im) {
    const VpStandIns& t = viperport_stand_ins;
    if (t.size < sizeof(VpStandIns) || t.version != VP_STAND_INS_VERSION)
        return refuse("viperport: the stand-ins' table is from another build (version %u)", (unsigned)t.version);
    const IMAGE_DATA_DIRECTORY d = im.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress) return 0;
    std::vector<bool> used(t.count);
    for (const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(im.base + d.VirtualAddress); imp->Name; imp++) {
        const char* dll = (const char*)(im.base + imp->Name);
        const IMAGE_THUNK_DATA32* names =
            (const IMAGE_THUNK_DATA32*)(im.base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(im.base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            Slot s = {(uint32_t*)&iat->u1.Function, dll, "", 0, "", NONE};
            char what[128];
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                s.ordinal = IMAGE_ORDINAL32(names->u1.Ordinal);
                snprintf(what, sizeof what, "%s #%u", dll, (unsigned)s.ordinal);
            } else {
                s.name = (const char*)((const IMAGE_IMPORT_BY_NAME*)(im.base + names->u1.AddressOfData))->Name;
                snprintf(what, sizeof what, "%s!%s", dll, s.name.c_str());
            }
            s.what = what;
            const uint32_t index = (uint32_t)g_slots.size();
            g_slots.push_back(s);
            Slot& sl = g_slots.back();
            if (!strcasecmp(dll, "DINPUT.dll")) {
                set_slot(sl, (uint32_t)(uintptr_t)&no_dinput, SHIM);
                continue;
            }
            if (const VpStandIn* e = entry_for(sl)) {
                used[e - t.entries] = true;
                set_slot(sl, (uint32_t)(uintptr_t)e->fn, STAND_IN);
                g_filled++;
                continue;
            }
            const uint32_t trap = trap_for(index);
            if (!trap) return refuse("viperport: no room for race.exe's import %s", what);
            set_slot(sl, trap, NONE);
        }
    }
    for (uint32_t i = 0; i < t.count; i++) {
        const VpStandIn& e = t.entries[i];
        if (used[i]) continue;
        char one[96];
        if (e.name) snprintf(one, sizeof one, "%s!%s", e.dll, e.name);
        else snprintf(one, sizeof one, "%s #%u", e.dll, (unsigned)e.ordinal);
        g_table_unmatched += (g_table_unmatched.empty() ? "" : ", ") + std::string(one);
    }
    return 0;
}

// once the port is installed: a slot it took over since (DDRAW and DSOUND: the renderer's and the audio's emulations)
// is the port's; the rest that still trap have no stand-in. The report goes to --check's output, else viperport.log.
int report_stand_ins() {
    std::string none;
    int nn = 0, port = 0;
    for (Slot& s : g_slots) {
        if (s.served == NONE && !is_trap(*s.at)) s.served = PORT;
        if (s.served == PORT) port++;
        if (s.served == NONE) none += (nn++ ? ", " : "") + s.what;
    }
    char line[512];
    snprintf(line, sizeof line, "stand-ins: %d of race.exe's %u imports filled from the port's table of %u (before the "
             "port installed); %d the port's own; %d without one (a trap); DINPUT: no DirectInput (E_FAIL)", g_filled,
             (unsigned)g_slots.size(), (unsigned)viperport_stand_ins.count, port, nn);
    std::vector<std::string> lines;
    lines.push_back(line);
    if (nn) lines.push_back("stand-ins: without a stand-in (the game ends if it calls one): " + none);
    if (!g_table_unmatched.empty()) lines.push_back("stand-ins: table entries race.exe doesn't import: " + g_table_unmatched);
    for (const std::string& l : lines) {
        if (g_check) say("viperport: %s", l.c_str());
        else if (viperport_stand_ins.log) viperport_stand_ins.log(("viperport: " + l).c_str());
    }
    return 0;
}

// ---- the main thread ---------------------------------------------------------------------------------------------------
void start_main_thread() {
    if (w32_seh_thread_begin) w32_seh_thread_begin();           // its TIB behind fs, the fault signals (w32_seh.h)
    else fprintf(stderr, "viperport: no fs: block for the main thread yet (w32_seh_thread_begin isn't linked)\n");
    const uint16_t cw = 0x27f;                                  // Windows' x87 control word at process start
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
}

void at_exit() { DllMain((HINSTANCE)vp_linux_self_module(), DLL_PROCESS_DETACH, 0); }

// ---- --probe -------------------------------------------------------------------------------------------------------
// "Will the standalone run this race.exe?": what a launch would refuse, the cheap way -- race.exe read and its headers
// checked as a launch does, mapped at 0x400000 (nothing filled), and the port's stock check (viperport_probe; the port
// is not installed). Exit codes as viperport.exe's (4-6, about dinput.dll and SDL2.dll, can't happen: one executable):
//   0 yes: ...   2 no: race.exe not found / unreadable   3 no: isn't v1.0 race.exe   7 no: functions patched in a way
//   the port doesn't know   8 no: anything else (race.exe couldn't be mapped)
int probe(const std::string& exe) {
    int code = 0;
    std::string why;
    auto no = [&](int c, const std::string& w) { code = c; why = w; };
    Image im;
    if (!exists(exe)) no(2, "race.exe not found: " + exe);
    else if (!read_file(exe, im.file)) no(2, "race.exe couldn't be read: " + exe);
    if (!code) {
        im.nt = headers_of(im.file);
        if (!im.nt || im.nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || im.nt->FileHeader.TimeDateStamp != V10_TIMESTAMP ||
            im.nt->OptionalHeader.ImageBase != BASE || im.nt->OptionalHeader.SizeOfImage != V10_IMAGE ||
            im.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
            no(3, exe + " isn't v1.0 race.exe (the standalone runs v1.0 only; the race.bin builds keep the dinput.dll route)");
    }
    if (!code && map_image(im, exe)) no(8, g_probe_why.empty() ? "race.exe couldn't be mapped" : g_probe_why);
    if (!code) {
        char line[1024] = "";
        const int r = viperport_probe(VP_STANDALONE_VERSION, line, sizeof line);
        if (r == -2) no(5, std::string("the port and the loader are from different builds (") + line + ")");
        else if (r == -1) no(3, exe + " isn't v1.0 race.exe (the port says so)");
        else if (r > 0) no(7, line);
        else why = line;
    }
    probe_line((code ? "no: " : "yes: ") + why);
    fflush(0);
    _exit(code);                                        // (nothing installed: end without running anything else)
}

// ---- the run ---------------------------------------------------------------------------------------------------------
int run(int argc, char** argv) {
    {
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        g_self_dir = n > 0 ? folder_of(std::string(self, (size_t)n)) : std::string("./");
    }
    std::vector<std::string> args(argv + 1, argv + argc);
    g_check = !args.empty() && args[0] == "--check";
    g_probe = !args.empty() && args[0] == "--probe";
    std::string exe = "race.exe";
    std::vector<std::string> game_args;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& t = args[i];
        if (t == "--race" && i + 1 < args.size()) exe = args[++i];
        else if ((g_check || g_probe) && i == 0) {}
        else if ((g_check || g_probe) && t.size() && t[0] != '-') exe = t;   // --check <race.exe>, --probe <race.exe>
        else game_args.push_back(t);
    }
    exe = resolve_path(exe);
    if (g_probe) return probe(exe);
    if (g_check) g_report = fopen((g_self_dir + "viperport-check.txt").c_str(), "w");

    // race.exe: v1.0 only
    Image im;
    if (!exists(exe)) return refuse("viperport: %s isn't there: run viperport in race.exe's folder, or --race <file>", exe.c_str());
    if (!read_file(exe, im.file)) return refuse("viperport: %s couldn't be read (%s)", exe.c_str(), strerror(errno));
    im.nt = headers_of(im.file);
    if (!im.nt || im.nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
        return refuse("viperport: %s isn't a 32-bit Windows program", exe.c_str());
    if (im.nt->FileHeader.TimeDateStamp != V10_TIMESTAMP || im.nt->OptionalHeader.ImageBase != BASE ||
        im.nt->OptionalHeader.SizeOfImage != V10_IMAGE)
        return refuse("viperport: %s isn't v1.0 race.exe (timestamp %08x): the standalone runs v1.0 only -- the race.bin "
                      "builds keep the dinput.dll route", exe.c_str(), (unsigned)im.nt->FileHeader.TimeDateStamp);
    const std::string hash = sha256_hex(im.file);
    const bool stock = hash == V10_SHA256;
    say("viperport: %s: v1.0 (timestamp %08x), SHA-256 %s%s", exe.c_str(), V10_TIMESTAMP, hash.c_str(),
        stock ? " -- the stock file" : " -- not the stock file (the port checks each function it replaces)");
    if (im.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
        return refuse("viperport: %s has TLS, which v1.0 doesn't", exe.c_str());

    // who the game is: drive C:'s root is race.exe's folder; the game is C:\<its name>, its arguments after it
    const std::string root = folder_of(exe);
    w32::path::set_root(root.c_str());
    if (chdir(root.c_str()) != 0) return refuse("viperport: can't enter %s (%s)", root.c_str(), strerror(errno));
    const std::string race_win = "C:\\" + name_of(exe);
    std::string cmdline = "\"" + race_win + "\"";       // (Windows quotes the program's path when it starts one)
    for (const std::string& a : game_args) cmdline += " " + (a.find_first_of(" \t") != std::string::npos ? "\"" + a + "\"" : a);
    vp_linux_set_self("C:\\viperport");                 // the port's module: viperport.ini / viperport.log beside race.exe
    if (viperport_stand_ins.set_process_image) viperport_stand_ins.set_process_image(BASE, race_win.c_str(), cmdline.c_str());

    // map, import, protect
    if (int r = map_image(im, exe)) return r;
    if (int r = resolve_imports(im)) return r;

    // the port: what DllMain does on Windows (the standalone always), on this thread with Windows' start state
    start_main_thread();
    DllMain((HINSTANCE)vp_linux_self_module(), DLL_PROCESS_ATTACH, 0);
    atexit(at_exit);
    report_stand_ins();

    int loader_fails = 0;
    if (g_check) {
        // what the game will see of its module, through its own import slots (as it calls them)
        char name[MAX_PATH] = "";
        const Slot* gmh = slot_named("KERNEL32.dll!GetModuleHandleA");
        const Slot* gmf = slot_named("KERNEL32.dll!GetModuleFileNameA");
        const Slot* gcl = slot_named("KERNEL32.dll!GetCommandLineA");
        const DWORD nn = gmf ? ((DWORD(WINAPI*)(HMODULE, LPSTR, DWORD))(uintptr_t)*gmf->at)(0, name, MAX_PATH) : 0;
        const HMODULE mh = gmh ? ((HMODULE(WINAPI*)(LPCSTR))(uintptr_t)*gmh->at)(0) : 0;
        const char* cl = gcl ? ((LPSTR(WINAPI*)())(uintptr_t)*gcl->at)() : "(none)";
        say("viperport: the game's GetModuleHandleA(NULL) = %p, GetModuleFileNameA(NULL) = \"%s\"", (void*)mh, nn ? name : "(fails)");
        if (!nn || strcmp(name, race_win.c_str()) != 0) { say("FAIL: the game's module file name isn't race.exe's"); loader_fails++; }
        if (mh != (HMODULE)(uintptr_t)BASE) { say("FAIL: the game's GetModuleHandleA(NULL) isn't race.exe's image"); loader_fails++; }
        say("viperport: the game's command line (its GetCommandLineA) = %s", cl);
        if (strcmp(cl, cmdline.c_str()) != 0) { say("FAIL: the game's command line isn't race.exe's"); loader_fails++; }
        // its resources on its hInstance 0x400000, through its slots (the stand-ins read race.exe's resource section)
        const Slot* li = slot_named("USER32.dll!LoadIconA");
        const Slot* lb = slot_named("USER32.dll!LoadBitmapA");
        const uint32_t icon = li ? ((uint32_t(WINAPI*)(uint32_t, const char*))(uintptr_t)*li->at)(BASE, (const char*)1) : 0;
        const uint32_t bmp = lb ? ((uint32_t(WINAPI*)(uint32_t, const char*))(uintptr_t)*lb->at)(BASE, "SPLASH") : 0;
        say("viperport: on hInstance 0x400000: LoadIcon(1) %s, LoadBitmap(SPLASH) %s", icon ? "ok" : "FAILS", bmp ? "ok" : "FAILS");
        if (!icon || !bmp) { say("FAIL: race.exe's resources on 0x400000"); loader_fails++; }
        // the modem line: TAPI through the game's slot (the stand-in: TAPI with no modem) must start
        const Slot* lis = slot_named("TAPI32.dll!lineInitialize");
        const Slot* lsd = slot_named("TAPI32.dll!lineShutdown");
        if (lis) {
            struct Cb { static void WINAPI f(DWORD, DWORD, DWORD_PTR, DWORD_PTR, DWORD_PTR, DWORD_PTR) {} };
            typedef LONG(WINAPI * LineInitialize_t)(DWORD*, HINSTANCE, void*, LPCSTR, DWORD*);
            DWORD app = 0, devs = 0;
            const LONG via = ((LineInitialize_t)(uintptr_t)*lis->at)(&app, (HINSTANCE)(uintptr_t)BASE, (void*)&Cb::f,
                                                                     "viperport check", &devs);
            if (via == 0 && lsd) ((LONG(WINAPI*)(DWORD))(uintptr_t)*lsd->at)(app);
            say("viperport: TAPI lineInitialize on hInstance 0x400000, through the game's slot: %s (%lu devices)",
                via ? "FAILS" : "ok", (unsigned long)devs);
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
    const int r = viperport_standalone(&a);
    if (g_check) {
        say("viperport --check: %s", r || loader_fails ? "FAILED" : "passed");
        if (g_report) fclose(g_report);
        g_report = 0;
        exit(r || loader_fails ? 1 : 0);                // (the exit logs, as ExitProcess on Windows)
    }
    return refuse("viperport: the game didn't start (%d checks failed): see viperport.log beside race.exe", r);
}

}  // namespace

int main(int argc, char** argv) {
    reserve_range();                                    // race.exe's range first, before anything else allocates
    // The heap low, as a 32-bit Windows process has it: every malloc from the main arena, grown by brk just above this
    // executable (0x08xxxxxx up), never from mmap (blocks of 128 KB and up, other threads' arenas) near the top of the
    // address space. The race recorder's trace leaves pointers out of its hashes by their value -- a 4-aligned word
    // below 0x30000000 (hook/replay.cpp, pointer_like) -- so a heap up at 0xdf000000 / 0xf5000000 put every car's
    // pointers into the hashes and parted every replayed race at tick 0.
    mallopt(M_ARENA_MAX, 1);
    mallopt(M_MMAP_MAX, 0);
    return run(argc, argv);
}
