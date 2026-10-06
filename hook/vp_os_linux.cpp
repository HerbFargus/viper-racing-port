// vp_os_linux.cpp -- the native Linux build's OS layer (relink stage R2b, agent B; vp_os_linux.h says what it is).
#ifndef _WIN32
#include <windows.h>                                           // hook/linux_inc -> win32_compat.h
#include "vp_os.h"
#include "vp_os_linux.h"
#include "w32_kernel.h"
#include "w32_path.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" char __executable_start[];                          // (the linker's: this executable's first byte)
#ifndef ERROR_BAD_LENGTH
#define ERROR_BAD_LENGTH 24L
#endif
#ifndef ERROR_INVALID_ADDRESS
#define ERROR_INVALID_ADDRESS 487L
#endif

namespace {

const uint32_t PAGE = 0x1000, GRANULE = 0x10000;               // Windows' page and allocation granularity

// ---- the region table ----------------------------------------------------------------------------------------------------
struct Region {
    uint32_t base, size;                                       // the allocation (AllocationBase, its size)
    uint32_t type;                                             // MEM_IMAGE / MEM_PRIVATE
    uint32_t alloc_prot;                                       // AllocationProtect
    std::vector<uint32_t> prot;                                // per page: PAGE_*, 0 = reserved (not committed)
};
std::mutex g_mu;
std::map<uint32_t, Region> g_regions;                          // by base

Region* region_of(uint32_t a) {                                // (g_mu held)
    auto it = g_regions.upper_bound(a);
    if (it == g_regions.begin()) return 0;
    --it;
    return a < it->second.base + it->second.size ? &it->second : 0;
}

// a mapping of /proc/self/maps
struct Mapping {
    uint32_t lo, hi;
    char perms[5];
    unsigned long inode;
    std::string path;
};
bool read_maps(std::vector<Mapping>& out) {
    FILE* f = fopen("/proc/self/maps", "r");                   // (a host path: the wrapper passes it through)
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi, off, inode;
        char perms[8] = "", dev[16] = "";
        int name_at = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %15s %lu %n", &lo, &hi, perms, &off, dev, &inode, &name_at) < 6) continue;
        Mapping m;
        m.lo = (uint32_t)lo;
        m.hi = (uint32_t)(hi ? hi : 0);
        if (hi > 0xffffffffUL) m.hi = 0xfffff000u;
        memcpy(m.perms, perms, 4);
        m.perms[4] = 0;
        m.inode = inode;
        if (name_at > 0) {
            m.path = line + name_at;
            while (!m.path.empty() && (m.path.back() == '\n' || m.path.back() == ' ')) m.path.pop_back();
        }
        out.push_back(m);
    }
    fclose(f);
    return true;
}
uint32_t win_prot_of(const char* perms) {
    const bool r = perms[0] == 'r', w = perms[1] == 'w', x = perms[2] == 'x';
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    return r ? PAGE_READONLY : PAGE_NOACCESS;
}
// a file-backed mapping is a module's (MEM_IMAGE); its AllocationBase is the first mapping of the same file
uint32_t module_base(const std::vector<Mapping>& maps, size_t i) {
    if (maps[i].inode == 0) return maps[i].lo;
    size_t j = i;
    while (j > 0 && maps[j - 1].inode == maps[i].inode && maps[j - 1].path == maps[i].path) j--;
    return maps[j].lo;
}

std::string g_self = "C:\\viperport";                          // vp_linux_set_self
std::mutex g_self_mu;

DWORD copy_name(const std::string& s, LPSTR out, DWORD n) {   // GetModuleFileNameA's result, as Windows (XP+) gives it
    if (!n) {
        vpos_SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const DWORD len = (DWORD)s.size();
    if (len >= n) {
        memcpy(out, s.c_str(), n - 1);
        out[n - 1] = 0;
        vpos_SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    memcpy(out, s.c_str(), len + 1);
    vpos_SetLastError(ERROR_SUCCESS);
    return len;
}

}  // namespace

// ---- vp_os_linux.h ---------------------------------------------------------------------------------------------------------
void* vp_linux_self_module() { return (void*)__executable_start; }
void vp_linux_set_self(const char* windows_path) {
    std::lock_guard<std::mutex> lk(g_self_mu);
    g_self = windows_path;
}
void vp_linux_note_region(uint32_t base, uint32_t size, uint32_t type, uint32_t prot) {
    std::lock_guard<std::mutex> lk(g_mu);
    Region r;
    r.base = base & ~(PAGE - 1);
    r.size = (size + (base - r.base) + PAGE - 1) & ~(PAGE - 1);
    r.type = type;
    r.alloc_prot = prot;
    r.prot.assign(r.size / PAGE, prot);
    g_regions[r.base] = r;
}
int vp_linux_unix_prot(uint32_t p) {
    switch (p & 0xff) {                                        // (PAGE_GUARD / PAGE_NOCACHE: not kept)
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return -1;
    }
}

// ---- vp_os.h's O kind ------------------------------------------------------------------------------------------------------
// Windows' tick count: milliseconds since boot, a DWORD that wraps (every 49.7 days)
DWORD vpos_GetTickCount() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

// mprotect on the pages covering [p, p+n); the old protection is the first page's (the table's, else /proc/self/maps')
BOOL vpos_VirtualProtect(LPVOID p, SIZE_T n, DWORD prot, PDWORD old) {
    const int up = vp_linux_unix_prot(prot);
    if (up < 0 || !old) {
        vpos_SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const uint32_t a = (uint32_t)(uintptr_t)p & ~(PAGE - 1);
    const uint32_t end = (uint32_t)(((uint64_t)(uintptr_t)p + (n ? n : 1) + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    std::lock_guard<std::mutex> lk(g_mu);
    Region* r = region_of(a);
    uint32_t was;
    if (r) {
        if (end > r->base + r->size) {                         // (Windows: not across allocations)
            vpos_SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        for (uint32_t x = a; x < end; x += PAGE)
            if (!r->prot[(x - r->base) / PAGE]) {               // reserved, not committed
                vpos_SetLastError(ERROR_INVALID_ADDRESS);
                return FALSE;
            }
        was = r->prot[(a - r->base) / PAGE];
    } else {
        std::vector<Mapping> maps;
        read_maps(maps);
        was = 0;
        for (const Mapping& m : maps)
            if (a >= m.lo && a < m.hi) was = win_prot_of(m.perms);
        if (!was) {
            vpos_SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
    }
    if (mprotect((void*)(uintptr_t)a, end - a, up) != 0) {
        vpos_SetLastError(errno == ENOMEM ? ERROR_INVALID_ADDRESS : ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (r)
        for (uint32_t x = a; x < end; x += PAGE) r->prot[(x - r->base) / PAGE] = prot;
    *old = was;
    return TRUE;
}

SIZE_T vpos_VirtualQuery(LPCVOID p, MEMORY_BASIC_INFORMATION* m, SIZE_T n) {
    if (n < sizeof *m) {
        vpos_SetLastError(ERROR_BAD_LENGTH);
        return 0;
    }
    const uint32_t a = (uint32_t)(uintptr_t)p & ~(PAGE - 1);
    memset(m, 0, sizeof *m);
    m->BaseAddress = (PVOID)(uintptr_t)a;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (Region* r = region_of(a)) {
            const size_t i = (a - r->base) / PAGE;
            size_t j = i;
            while (j < r->prot.size() && r->prot[j] == r->prot[i]) j++;
            m->AllocationBase = (PVOID)(uintptr_t)r->base;
            m->AllocationProtect = r->alloc_prot;
            m->RegionSize = (j - i) * PAGE;
            m->State = r->prot[i] ? MEM_COMMIT : MEM_RESERVE;
            m->Protect = r->prot[i];
            m->Type = r->type;
            return sizeof *m;
        }
    }
    std::vector<Mapping> maps;
    read_maps(maps);
    uint32_t next = 0x7fff0000u;                               // (the end of user space as a 32-bit Windows process sees it)
    for (size_t i = 0; i < maps.size(); i++) {
        const Mapping& mp = maps[i];
        if (a >= mp.lo && a < mp.hi) {
            m->AllocationBase = (PVOID)(uintptr_t)module_base(maps, i);
            m->Protect = m->AllocationProtect = win_prot_of(mp.perms);
            m->RegionSize = mp.hi - a;
            m->State = MEM_COMMIT;
            m->Type = mp.inode ? MEM_IMAGE : MEM_PRIVATE;
            return sizeof *m;
        }
        if (mp.lo > a && mp.lo < next) next = mp.lo;
    }
    m->State = MEM_FREE;                                       // (free: as Windows, no base and NOACCESS)
    m->Protect = PAGE_NOACCESS;
    m->RegionSize = next > a ? next - a : PAGE;
    return sizeof *m;
}

// mmap: a reservation (PROT_NONE) or committed memory, at Windows' 64 KB granularity; MEM_COMMIT inside an earlier
// reservation commits those pages
LPVOID vpos_VirtualAlloc(LPVOID p, SIZE_T n, DWORD type, DWORD prot) {
    const int up = vp_linux_unix_prot(prot);
    if (up < 0 || !n || !(type & (MEM_COMMIT | MEM_RESERVE))) {
        vpos_SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (p && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) {   // commit in a reservation
        const uint32_t a = (uint32_t)(uintptr_t)p & ~(PAGE - 1);
        const uint32_t end = (uint32_t)(((uint64_t)(uintptr_t)p + n + PAGE - 1) & ~(uint64_t)(PAGE - 1));
        std::lock_guard<std::mutex> lk(g_mu);
        Region* r = region_of(a);
        if (!r || end > r->base + r->size) {
            vpos_SetLastError(ERROR_INVALID_ADDRESS);
            return 0;
        }
        if (mprotect((void*)(uintptr_t)a, end - a, up) != 0) {
            vpos_SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        for (uint32_t x = a; x < end; x += PAGE) r->prot[(x - r->base) / PAGE] = prot;
        return (LPVOID)(uintptr_t)a;
    }
    const uint32_t size = (uint32_t)((n + PAGE - 1) & ~(SIZE_T)(PAGE - 1));
    const int mp = (type & MEM_COMMIT) ? up : PROT_NONE;
    void* got;
    if (p) {
        void* at = (void*)((uintptr_t)p & ~(uintptr_t)(GRANULE - 1));
        got = mmap(at, size + ((uintptr_t)p - (uintptr_t)at), mp, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (got == MAP_FAILED || got != at) {
            if (got != MAP_FAILED) munmap(got, size);
            vpos_SetLastError(ERROR_INVALID_ADDRESS);
            return 0;
        }
    } else {                                                   // anywhere, on a 64 KB boundary
        uint8_t* b = (uint8_t*)mmap(0, size + GRANULE, mp, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (b == MAP_FAILED) {
            vpos_SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        uint8_t* a = (uint8_t*)(((uintptr_t)b + GRANULE - 1) & ~(uintptr_t)(GRANULE - 1));
        if (a > b) munmap(b, a - b);
        if (b + size + GRANULE > a + size) munmap(a + size, (b + size + GRANULE) - (a + size));
        got = a;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    Region r;
    r.base = (uint32_t)(uintptr_t)got;
    r.size = size;
    r.type = MEM_PRIVATE;
    r.alloc_prot = prot;
    r.prot.assign(size / PAGE, (type & MEM_COMMIT) ? prot : 0);
    g_regions[r.base] = r;
    return got;
}

// a guarded read: the kernel reads one byte of each page through process_vm_readv, which fails with EFAULT instead of
// faulting (async-signal-safe: the fault logger and the int3 handler call this inside a signal)
BOOL vpos_IsBadReadPtr(const void* p, UINT_PTR n) {
    if (!n) return FALSE;
    if (!p) return TRUE;
    const uintptr_t a = (uintptr_t)p, end = a + n - 1;
    if (end < a) return TRUE;                                  // (wraps)
    static const pid_t self = getpid();
    for (uintptr_t x = a;; x = (x & ~(uintptr_t)(PAGE - 1)) + PAGE) {
        char b;
        struct iovec local = {&b, 1}, remote = {(void*)x, 1};
        if (process_vm_readv(self, &local, 1, &remote, 1, 0) != 1) return TRUE;
        if ((x & ~(uintptr_t)(PAGE - 1)) == (end & ~(uintptr_t)(PAGE - 1))) break;
    }
    return FALSE;
}

// the port asks for race.exe's image only (NULL): 0x400000, where the loader mapped it
HMODULE vpos_GetModuleHandleA(LPCSTR name) {
    if (!name) return (HMODULE)(uintptr_t)0x400000;
    vpos_SetLastError(ERROR_MOD_NOT_FOUND);
    return 0;
}

// 0 or the port's own module: the port's Windows path (vp_linux_set_self); a shared object's handle (GetModuleHandleExA
// by address): its host path
DWORD vpos_GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) {
    if (!m || m == (HMODULE)vp_linux_self_module()) {
        std::string s;
        {
            std::lock_guard<std::mutex> lk(g_self_mu);
            s = g_self;
        }
        return copy_name(s, out, n);
    }
    std::vector<Mapping> maps;
    read_maps(maps);
    for (size_t i = 0; i < maps.size(); i++)
        if (maps[i].lo == (uint32_t)(uintptr_t)m && maps[i].inode && !maps[i].path.empty())
            return copy_name(maps[i].path, out, n);
    vpos_SetLastError(ERROR_MOD_NOT_FOUND);
    return 0;
}

// ---- the C runtime's fopen, for the port's own files (-Wl,--wrap=fopen: vp_os_linux.h) ------------------------------------
extern "C" FILE* __real_fopen(const char* path, const char* mode);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    if (path && (strchr(path, '\\') || (((path[0] | 32) >= 'a' && (path[0] | 32) <= 'z') && path[1] == ':'))) {
        w32::path::Resolved r;
        uint32_t err = 0;
        if (!w32::path::resolve(path, r, &err)) {
            errno = ENOENT;
            return 0;
        }
        return __real_fopen(r.host.c_str(), mode);
    }
    return __real_fopen(path, mode);
}
extern "C" FILE* __real_fopen64(const char* path, const char* mode);
extern "C" FILE* __wrap_fopen64(const char* path, const char* mode) {
    if (path && (strchr(path, '\\') || (((path[0] | 32) >= 'a' && (path[0] | 32) <= 'z') && path[1] == ':'))) {
        w32::path::Resolved r;
        uint32_t err = 0;
        if (!w32::path::resolve(path, r, &err)) {
            errno = ENOENT;
            return 0;
        }
        return __real_fopen64(r.host.c_str(), mode);
    }
    return __real_fopen64(path, mode);
}

// ---- the Win32 names the port calls directly (standalone.cpp, viperport.cpp) ---------------------------------------------
extern "C" {
BOOL WINAPI VirtualProtect(LPVOID p, SIZE_T n, DWORD prot, PDWORD old) { return vpos_VirtualProtect(p, n, prot, old); }
SIZE_T WINAPI VirtualQuery(LPCVOID p, PMEMORY_BASIC_INFORMATION m, SIZE_T n) { return vpos_VirtualQuery(p, m, n); }
BOOL WINAPI FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return TRUE; }   // (x86: coherent)
HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(uintptr_t)0xffffffffu; }
// DEP: the kernel's NX is always on for pages that aren't PROT_EXEC
BOOL WINAPI GetProcessDEPPolicy(HANDLE, LPDWORD flags, PBOOL permanent) {
    if (flags) *flags = PROCESS_DEP_ENABLE;
    if (permanent) *permanent = TRUE;
    return TRUE;
}
BOOL WINAPI IsBadReadPtr(const VOID* p, UINT_PTR n) { return vpos_IsBadReadPtr(p, n); }
HMODULE WINAPI GetModuleHandleA(LPCSTR name) { return vpos_GetModuleHandleA(name); }
// FROM_ADDRESS (standalone.cpp's describe): this executable, else the shared object mapped there
BOOL WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out) {
    if (!out) {
        vpos_SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *out = 0;
    if (!(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) {
        *out = vpos_GetModuleHandleA(name);
        return *out != 0;
    }
    extern char _end[];
    const uintptr_t a = (uintptr_t)name;
    if (a >= (uintptr_t)__executable_start && a < (uintptr_t)_end) {
        *out = (HMODULE)vp_linux_self_module();
        return TRUE;
    }
    std::vector<Mapping> maps;
    read_maps(maps);
    for (size_t i = 0; i < maps.size(); i++)
        if (a >= maps[i].lo && a < maps[i].hi && maps[i].inode) {
            *out = (HMODULE)(uintptr_t)module_base(maps, i);
            return TRUE;
        }
    vpos_SetLastError(ERROR_MOD_NOT_FOUND);
    return FALSE;
}
DWORD WINAPI GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) { return vpos_GetModuleFileNameA(m, out, n); }
DWORD WINAPI GetLastError(void) { return vpos_GetLastError(); }
DWORD WINAPI GetCurrentThreadId(void) { return (DWORD)vpos_GetCurrentThreadId(); }
VOID WINAPI Sleep(DWORD ms) { vpos_Sleep(ms); }
VOID WINAPI ExitProcess(UINT code) { vpos_ExitProcess(code); }
LPSTR WINAPI lstrcatA(LPSTR to, LPCSTR from) { return vpos_lstrcatA(to, from); }
// the host's environment (the variables the port reads: VIPERPORT_PROBE ...), Windows' results
DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n) {
    const char* v = name ? getenv(name) : 0;
    if (!v) {
        vpos_SetLastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    const DWORD len = (DWORD)strlen(v);
    if (!buf || len >= n) return len + 1;                      // (the size needed, terminator included)
    memcpy(buf, v, len + 1);
    return len;
}
BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
    if (!name || !*name || strchr(name, '=')) {
        vpos_SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (value) setenv(name, value, 1);
    else unsetenv(name);
    return TRUE;
}
BOOL WINAPI DisableThreadLibraryCalls(HMODULE) { return TRUE; }   // (one executable: no thread notifications)
}
#endif  // !_WIN32
