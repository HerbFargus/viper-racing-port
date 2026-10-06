// w32_mem.cpp -- the memory stand-ins (w32_kernel.h): Heap*, Global*, GetSystemInfo, GetVersion.
//
// The heaps are the port's own, the same on Windows and Linux: a heap is a list of blocks from the C library's
// malloc, each with a 16-byte header (its heap, the list links, the size asked for), so a block's address keeps
// malloc's alignment (8 on Windows, 16 on 32-bit glibc; Windows' heap gives 8). What the game sees is Windows':
//   HeapAlloc(0 bytes) is a real block; HEAP_ZERO_MEMORY zeroes (HeapReAlloc: the part that grows); without it the
//   bytes are whatever was there (a reused block keeps old contents, as Windows'); HeapSize is the size asked for;
//   HeapReAlloc keeps the contents, and with HEAP_REALLOC_IN_PLACE_ONLY succeeds only shrinking (Windows can also
//   grow in place into free space after the block -- this heap never does); HeapFree(NULL) succeeds; HeapValidate
//   says no for a pointer that isn't a block's start (the game's MemVerify passes one: krn_core.cpp); HeapDestroy frees
//   every block. A heap handle is the heap's address, as Windows' (not a w32_handle).
// Every heap takes a lock, HEAP_NO_SERIALIZE or not: the game's C runtime is the single-threaded LIBC and both of its
// threads allocate (Windows' heap, not serialised, would race there; this one doesn't).
// Addresses differ from Windows' (and between runs, as they do on Windows): nothing the game computes may depend on a
// block's address -- see the R2b notes in the report (the heap's addresses never reach a recorded session).
//
// GlobalAlloc / GlobalLock / GlobalUnlock are only the clipboard's (krn_util.cpp: the block goes to SetClipboardData,
// which owns it): on Windows they stay Windows' (a real HGLOBAL is what the real clipboard takes); on Linux a block is
// a w32_handle-free allocation the clipboard stand-in reads with w32::global_memory.
// GetSystemInfo / GetVersion describe the machine: Windows' answers on Windows; on Linux what this machine's Windows
// gives a 32-bit program (the game reads dwNumberOfProcessors, and GetVersion's high bit: NT).
#include "w32_kernel.h"
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <atomic>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

const uint32_t HEAP_ZERO = 0x8, HEAP_IN_PLACE = 0x10;
const uint32_t MAGIC = 0x48703332;               // "23pH": a live heap

struct Block {
    struct Heap* heap;
    Block* prev;
    Block* next;
    uint32_t size;                               // the size asked for (HeapSize)
};
static_assert(sizeof(Block) == 16 || sizeof(void*) != 4, "a block's header keeps malloc's alignment");

struct Heap {
    uint32_t magic;
    uint32_t maximum;                            // 0: growable; else the most it holds
    uint32_t total;                              // bytes in live blocks
    std::mutex mu;
    Block head;                                  // the list's sentinel
};

// live heaps: a handle is checked against these before it's touched (MemFree after MemEnd passes 0xffffffff)
const int MAX_HEAPS = 64;
std::atomic<Heap*> g_heaps[MAX_HEAPS];

Heap* heap_of(uint32_t h) {
    if (!h) return 0;
    for (int i = 0; i < MAX_HEAPS; i++)
        if (g_heaps[i].load(std::memory_order_acquire) == (Heap*)(uintptr_t)h) return (Heap*)(uintptr_t)h;
    return 0;
}
Block* block_of(const void* p) { return (Block*)((uint8_t*)p - sizeof(Block)); }
// a block of this heap? (checked through the header, as Windows' heap checks its own; a foreign pointer's header
// isn't this heap's)
bool owns(Heap* hp, const void* p) { return p && ((uintptr_t)p & 7) == 0 && block_of(p)->heap == hp; }

void link(Heap* hp, Block* b) {
    b->heap = hp;
    b->prev = &hp->head;
    b->next = hp->head.next;
    hp->head.next->prev = b;
    hp->head.next = b;
}
void unlink(Block* b) {
    b->prev->next = b->next;
    b->next->prev = b->prev;
    b->heap = 0;
}
// a block's cost in a fixed-size heap: Windows' 8-byte header, 8-byte granules
uint32_t cost(uint32_t n) { return ((n + 7) & ~7u) + 8; }
// Windows' limits: any block under 2 GB; a fixed-size heap's block at most 0x7fff8, and what the heap holds -- its
// maximum less Windows' own overhead (1288 bytes measured for a 64 KB heap; approximate for other sizes: the game's
// heaps are growable)
bool too_big(Heap* hp, uint32_t n, uint32_t freed = 0) {
    if (n >= 0x7fffffffu - sizeof(Block)) return true;
    return hp->maximum && (n > 0x7fff8u || hp->total - freed + cost(n) > hp->maximum - 1288);
}

}  // namespace

uint32_t W32K_CALL w32_HeapCreate(uint32_t options, uint32_t initial_size, uint32_t maximum_size) {
    (void)options;
    Heap* hp = new Heap;
    hp->magic = MAGIC;
    hp->maximum = maximum_size && initial_size > maximum_size ? initial_size : maximum_size;   // (Windows: no error)
    hp->total = 0;
    hp->head.heap = hp;
    hp->head.prev = hp->head.next = &hp->head;
    hp->head.size = 0;
    for (int i = 0; i < MAX_HEAPS; i++) {
        Heap* none = 0;
        if (g_heaps[i].compare_exchange_strong(none, hp)) return (uint32_t)(uintptr_t)hp;
    }
    delete hp;
    w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
    return 0;
}

int32_t W32K_CALL w32_HeapDestroy(uint32_t h) {
    Heap* hp = heap_of(h);
    if (!hp) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    for (int i = 0; i < MAX_HEAPS; i++) {
        Heap* me = hp;
        if (g_heaps[i].compare_exchange_strong(me, (Heap*)0)) break;
    }
    {
        std::lock_guard<std::mutex> lk(hp->mu);
        for (Block* b = hp->head.next; b != &hp->head;) {
            Block* n = b->next;
            b->heap = 0;
            free(b);
            b = n;
        }
        hp->magic = 0;
    }
    delete hp;
    return 1;
}

void* W32K_CALL w32_HeapAlloc(uint32_t h, uint32_t flags, uint32_t bytes) {
    Heap* hp = heap_of(h);
    if (!hp) return 0;                           // (Windows: an access violation or nothing; no SetLastError)
    std::lock_guard<std::mutex> lk(hp->mu);
    Block* b = too_big(hp, bytes) ? 0 : (Block*)malloc(sizeof(Block) + (bytes ? bytes : 1));
    if (!b) {
        w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);   // (documented as not set; Windows 10/11 sets it)
        return 0;
    }
    b->size = bytes;
    link(hp, b);
    hp->total += cost(bytes);
    if (flags & HEAP_ZERO) memset(b + 1, 0, bytes);
    return b + 1;
}

void* W32K_CALL w32_HeapReAlloc(uint32_t h, uint32_t flags, void* mem, uint32_t bytes) {
    Heap* hp = heap_of(h);
    if (!hp) return 0;
    std::lock_guard<std::mutex> lk(hp->mu);
    if (!owns(hp, mem)) {
        w32::set_last_error(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    Block* b = block_of(mem);
    const uint32_t old = b->size;
    if (bytes > old && too_big(hp, bytes, cost(old))) {
        w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (bytes <= old) {                          // shrinking: in place, as Windows (the tail stays allocated)
        b->size = bytes;
        hp->total -= cost(old) - cost(bytes);
        return mem;
    }
    if (flags & HEAP_IN_PLACE) {                 // (this heap never grows a block in place)
        w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    Block* prev = b->prev;
    Block* next = b->next;
    Block* n = (Block*)realloc(b, sizeof(Block) + bytes);
    if (!n) {
        w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    n->prev = prev;                              // (the links point at the old address: relink)
    n->next = next;
    prev->next = n;
    next->prev = n;
    n->size = bytes;
    hp->total += cost(bytes) - cost(old);
    if (flags & HEAP_ZERO) memset((uint8_t*)(n + 1) + old, 0, bytes - old);
    return n + 1;
}

int32_t W32K_CALL w32_HeapFree(uint32_t h, uint32_t flags, void* mem) {
    (void)flags;
    if (!mem) return 1;
    Heap* hp = heap_of(h);
    if (!hp) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    std::lock_guard<std::mutex> lk(hp->mu);
    if (!owns(hp, mem)) {
        w32::set_last_error(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    Block* b = block_of(mem);
    hp->total -= cost(b->size);
    unlink(b);
    free(b);
    return 1;
}

uint32_t W32K_CALL w32_HeapSize(uint32_t h, uint32_t flags, const void* mem) {
    (void)flags;
    Heap* hp = heap_of(h);
    if (!hp) return 0xffffffffu;
    std::lock_guard<std::mutex> lk(hp->mu);
    if (!owns(hp, mem)) {
        w32::set_last_error(w32::ERR_INVALID_PARAMETER);
        return 0xffffffffu;
    }
    return block_of(mem)->size;
}

// a block's start (looked up in the list, so any pointer is safe to ask about), or the whole heap (NULL)
int32_t W32K_CALL w32_HeapValidate(uint32_t h, uint32_t flags, const void* mem) {
    (void)flags;
    Heap* hp = heap_of(h);
    if (!hp) return 0;
    std::lock_guard<std::mutex> lk(hp->mu);
    for (Block* b = hp->head.next; b != &hp->head; b = b->next) {
        if (b->heap != hp || b->next->prev != b) return 0;
        if (mem && (const void*)(b + 1) == mem) return 1;
    }
    return mem ? 0 : 1;
}

// ---- GlobalAlloc: the clipboard's blocks ----------------------------------------------------------------------------
#ifdef _WIN32
uint32_t W32K_CALL w32_GlobalAlloc(uint32_t flags, uint32_t bytes) {
    HGLOBAL g = GlobalAlloc(flags, bytes);
    if (!g) w32::set_last_error(GetLastError());
    return (uint32_t)(uintptr_t)g;
}
void* W32K_CALL w32_GlobalLock(uint32_t mem) {
    void* p = GlobalLock((HGLOBAL)(uintptr_t)mem);
    if (!p) w32::set_last_error(GetLastError());
    return p;
}
int32_t W32K_CALL w32_GlobalUnlock(uint32_t mem) {
    SetLastError(0);
    const BOOL r = GlobalUnlock((HGLOBAL)(uintptr_t)mem);
    w32::set_last_error(GetLastError());         // (0 with NO_ERROR: the block is unlocked now)
    return r;
}
uint32_t W32K_CALL w32_GlobalFree(uint32_t mem) {
    HGLOBAL r = GlobalFree((HGLOBAL)(uintptr_t)mem);
    if (r) w32::set_last_error(GetLastError());
    return (uint32_t)(uintptr_t)r;
}
uint32_t W32K_CALL w32_GlobalSize(uint32_t mem) {
    const SIZE_T n = GlobalSize((HGLOBAL)(uintptr_t)mem);
    if (!n) w32::set_last_error(GetLastError());
    return (uint32_t)n;
}
void* w32::global_memory(uint32_t mem, uint32_t* size) {
    if (size) *size = (uint32_t)GlobalSize((HGLOBAL)(uintptr_t)mem);
    void* p = GlobalLock((HGLOBAL)(uintptr_t)mem);
    if (p) GlobalUnlock((HGLOBAL)(uintptr_t)mem);
    return p;
}
#else
// A block: {magic, size, lock count} then the bytes; the handle is the block's address (GMEM_FIXED's HGLOBAL is its
// pointer on Windows; GMEM_MOVEABLE's isn't, but the game only locks it). GMEM_ZEROINIT zeroes.
namespace {
struct Global { uint32_t magic, size, locks, pad; };
const uint32_t GLOBAL_MAGIC = 0x6c623267;
Global* global_of(uint32_t mem) {
    Global* g = (Global*)(uintptr_t)mem;
    return g && g->magic == GLOBAL_MAGIC ? g : 0;
}
}  // namespace
uint32_t W32K_CALL w32_GlobalAlloc(uint32_t flags, uint32_t bytes) {
    Global* g = (Global*)malloc(sizeof(Global) + (bytes ? bytes : 1));
    if (!g) {
        w32::set_last_error(w32::ERR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    g->magic = GLOBAL_MAGIC;
    g->size = bytes;
    g->locks = 0;
    if (flags & 0x40) memset(g + 1, 0, bytes);  // GMEM_ZEROINIT
    return (uint32_t)(uintptr_t)g;
}
void* W32K_CALL w32_GlobalLock(uint32_t mem) {
    Global* g = global_of(mem);
    if (!g) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    g->locks++;
    return g + 1;
}
int32_t W32K_CALL w32_GlobalUnlock(uint32_t mem) {
    Global* g = global_of(mem);
    if (!g) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    if (!g->locks) {
        w32::set_last_error(158);                // ERROR_NOT_LOCKED
        return 0;
    }
    if (--g->locks) return 1;
    w32::set_last_error(0);
    return 0;
}
uint32_t W32K_CALL w32_GlobalFree(uint32_t mem) {
    Global* g = global_of(mem);
    if (!g) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return mem;
    }
    g->magic = 0;
    free(g);
    return 0;
}
uint32_t W32K_CALL w32_GlobalSize(uint32_t mem) {
    Global* g = global_of(mem);
    if (!g) {
        w32::set_last_error(w32::ERR_INVALID_HANDLE);
        return 0;
    }
    return g->size;
}
void* w32::global_memory(uint32_t mem, uint32_t* size) {
    Global* g = global_of(mem);
    if (size) *size = g ? g->size : 0;
    return g ? g + 1 : 0;
}
#endif

// ---- the machine ------------------------------------------------------------------------------------------------------
void W32K_CALL w32_GetSystemInfo(W32SystemInfo* si) {
#ifdef _WIN32
    static_assert(sizeof(SYSTEM_INFO) == sizeof(W32SystemInfo), "SYSTEM_INFO");
    GetSystemInfo((SYSTEM_INFO*)si);
#else
    // what this machine's Windows gives a 32-bit, large-address-unaware program, with Linux's processor count
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    memset(si, 0, sizeof *si);
    si->processor_architecture = 0;              // PROCESSOR_ARCHITECTURE_INTEL
    si->page_size = 0x1000;
    si->min_app_address = 0x10000;
    si->max_app_address = 0x7ffeffff;
    si->active_processor_mask = n == 32 ? 0xffffffffu : (1u << n) - 1;
    si->number_of_processors = (uint32_t)n;
    si->processor_type = 586;                    // PROCESSOR_INTEL_PENTIUM
    si->allocation_granularity = 0x10000;
    si->processor_level = 6;
    si->processor_revision = 0;
#endif
}

uint32_t W32K_CALL w32_GetVersion() {
#ifdef _WIN32
    typedef DWORD(WINAPI * GetVersion_t)(void);   // (declared deprecated)
    static const GetVersion_t gv = (GetVersion_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetVersion");
    return gv();
#else
    return 0x23f00206u;                          // Windows 10/11 to a program without a compatibility manifest: 6.2.9200
#endif
}
